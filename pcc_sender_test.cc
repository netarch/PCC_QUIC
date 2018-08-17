#include "third_party/pcc_quic/pcc_sender.h"

#include <algorithm>
#include <memory>

#include "third_party/quic/core/congestion_control/rtt_stats.h"
#include "third_party/quic/platform/api/quic_test.h"
#include "third_party/quic/test_tools/mock_clock.h"
#include "third_party/quic/test_tools/quic_test_utils.h"
#include "third_party/quic/test_tools/quic_connection_peer.h"
#include "third_party/quic/test_tools/quic_sent_packet_manager_peer.h"
#include "third_party/quic/test_tools/simulator/quic_endpoint.h"
#include "third_party/quic/test_tools/simulator/simulator.h"
#include "third_party/quic/test_tools/simulator/switch.h"

namespace quic {
DECLARE_double(max_rtt_fluctuation_tolerance_ratio_in_starting);
DECLARE_double(max_rtt_fluctuation_tolerance_ratio_in_decision_made);
DECLARE_double(rtt_fluctuation_tolerance_gain_in_starting);
DECLARE_double(rtt_fluctuation_tolerance_gain_in_decision_made);
DECLARE_bool(restore_central_rate_upon_app_limited);

namespace test {

namespace {
// Number of bits per Mbit.
const size_t kMegabit = 1024 * 1024;
}

class PccSenderPeer {
 public:
  static PccSender::SenderMode mode(PccSender* sender) { return sender->mode_; }

  static float sending_rate(PccSender* sender) {
    return sender->sending_rate_.ToBitsPerSecond() /
           static_cast<float>(kMegabit);
  }

  static float bandwidth_sample(PccSender* sender) {
    return sender->BandwidthEstimate().ToBitsPerSecond() /
           static_cast<float>(kMegabit);
  }

  static size_t rounds(PccSender* sender) { return sender->rounds_; }

  static PccSender::RateChangeDirection direction(PccSender* sender) {
    return sender->direction_;
  }

  static size_t num_useful(PccSender* sender) {
    return sender->interval_queue_.num_useful_intervals();
  }

  static size_t num_intervals(PccSender* sender) {
    return sender->interval_queue_.size();
  }

  static QuicTime::Delta duration(PccSender* sender) {
    return sender->monitor_duration_;
  }

  static QuicTime::Delta latest_rtt(PccSender* sender) {
    return sender->rtt_stats_->latest_rtt();
  }

  // Set the flag has_seen_valid_rtt_ to be true.
  static void SetSeenRtt(PccSender* sender) {
    sender->has_seen_valid_rtt_ = true;
  }

  // Set the state of sender to facilitate unit test.
  static void SetMode(PccSender* sender,
                      PccSender::SenderMode mode,
                      PccSender::RateChangeDirection direction) {
    sender->mode_ = mode;
    sender->direction_ = direction;
  }
};

namespace {
const size_t kInitialCongestionWindowPackets = 10;
const size_t kNumIntervalGroupsInProbing = 2;

// Test network parameters.  Here, the topology of the network is:
//
//          PCC sender
//               |
//               |  <-- local link (10 Mbps, 2 ms delay)
//               |
//        Network switch
//               *  <-- the bottleneck queue in the direction
//               |          of the receiver
//               |
//               |  <-- test link (5 Mbps, 30 ms delay)
//               |
//               |
//           Receiver
//
const QuicBandwidth kTestLinkBandwidth =
    QuicBandwidth::FromKBitsPerSecond(5000);
const QuicBandwidth kLocalLinkBandwidth =
    QuicBandwidth::FromKBitsPerSecond(10000);
const QuicTime::Delta kTestPropagationDelay =
    QuicTime::Delta::FromMilliseconds(30);
const QuicTime::Delta kLocalPropagationDelay =
    QuicTime::Delta::FromMilliseconds(2);
const QuicTime::Delta kTestTransferTime =
    kTestLinkBandwidth.TransferTime(kMaxPacketSize) +
    kLocalLinkBandwidth.TransferTime(kMaxPacketSize);
const QuicTime::Delta kTestRtt =
    (kTestPropagationDelay + kLocalPropagationDelay + kTestTransferTime) * 2;
const QuicByteCount kTestBdp = kTestRtt * kTestLinkBandwidth;

class PccSenderTest : public QuicTest {
 protected:
  PccSenderTest()
      : sender_(&rtt_stats_,
                &unacked_packets_,
                kInitialCongestionWindowPackets,
                kDefaultMaxCongestionWindowPackets,
                &random_),
        packet_number_(0) {
    uint64_t seed = QuicRandom::GetInstance()->RandUint64();
    random_.set_seed(seed);
  }

  // Update RttStats so that smoothed RTT is increased by a given ratio.
  void IncreaseSmoothedRttByRatio(float inflation_ratio) {
    rtt_stats_.UpdateRtt((1 + 8 * inflation_ratio) * rtt_stats_.smoothed_rtt(),
                         QuicTime::Delta::Zero(), QuicTime::Zero());
    // Allow a small margin for float/int64_t cast.
    EXPECT_LT(((1 + inflation_ratio) * rtt_stats_.previous_srtt())
                  .ToMicroseconds() -
                  rtt_stats_.smoothed_rtt().ToMicroseconds(),
              2);
  }

  // Create a monitor interval and send maximum packets allowed in it based on
  // sending rate and interval duration. Note that this function is only called
  // after sender has the valid rtt.
  void SendPacketsInOneInterval() {
    DCHECK(!PccSenderPeer::latest_rtt(&sender_).IsZero())
        << "SendPacketsInOneInterval can only be called when latest RTT is "
           "available";

    QuicTime interval_start_time = clock_.Now();
    do {
      sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                           HAS_RETRANSMITTABLE_DATA);
      clock_.AdvanceTime(QuicTime::Delta::FromMicroseconds(
          kMaxPacketSize * 8 / PccSenderPeer::sending_rate(&sender_)));
    } while (clock_.Now() - interval_start_time <
             PccSenderPeer::duration(&sender_));
  }

  // Mark packets within range [start_packet_number, end_packet_number) as
  // acked.
  void AckPackets(QuicPacketNumber start_packet_number,
                  QuicPacketNumber end_packet_number) {
    packets_acked_.clear();
    packets_lost_.clear();
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number; ++i) {
      packets_acked_.push_back(AckedPacket(i, kMaxPacketSize,
                                           QuicTime::Zero()));
    }
    sender_.OnCongestionEvent(true, 0, clock_.Now(), packets_acked_,
                              packets_lost_);
  }

  // Mark packets within range [start_packet_number, end_packet_number) as lost.
  void LosePackets(QuicPacketNumber start_packet_number,
                   QuicPacketNumber end_packet_number) {
    packets_acked_.clear();
    packets_lost_.clear();
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number; ++i) {
      packets_lost_.push_back(LostPacket(i, kMaxPacketSize));
    }
    sender_.OnCongestionEvent(true, 0, clock_.Now(), packets_acked_,
                              packets_lost_);
  }

  RttStats rtt_stats_;
  QuicUnackedPacketMap unacked_packets_;
  SimpleRandom random_;
  PccSender sender_;

  // List of acked/lost packets, used when calling OnCongestionEvent on sender_.
  AckedPacketVector packets_acked_;
  LostPacketVector packets_lost_;

  QuicPacketNumber packet_number_;

  MockClock clock_;
};

class PccSenderSimulatorTest : public QuicTest {
 protected :
  PccSenderSimulatorTest() : simulator_(),
                             pcc_sender1_(&simulator_,
                                          "PCC sender 1",
                                          "Receiver 1",
                                          Perspective::IS_CLIENT,
                                          /*connection_id=*/42),
                             pcc_sender2_(&simulator_,
                                          "PCC sender 2",
                                          "Receiver 2",
                                          Perspective::IS_CLIENT,
                                          /*connection_id=*/43),
                             receiver1_(&simulator_,
                                        "Receiver 1",
                                        "PCC sender 1",
                                        Perspective::IS_SERVER,
                                        /*connection_id=*/42),
                             receiver2_(&simulator_,
                                        "Receiver 2",
                                        "PCC sender 2",
                                        Perspective::IS_SERVER,
                                        /*connection_id=*/43),
                             receiver_multiplexer_("Receiver multiplexer",
                                                   {&receiver1_, &receiver2_}) {
    rtt_stats_ = pcc_sender1_.connection()->sent_packet_manager().GetRttStats();
    sender_ = SetupPccSender(&pcc_sender1_);

    clock_ = simulator_.GetClock();
    simulator_.set_random_generator(&random_);

    uint64_t seed = QuicRandom::GetInstance()->RandUint64();
    random_.set_seed(seed);
    QUIC_LOG(INFO) << "PccSenderTest simulator set up with seed: " << seed;
  }

  // Enables PCC on |endpoint| and returns the associated PCCSender
  PccSender* SetupPccSender(simulator::QuicEndpoint* endpoint) {
    const RttStats* rtt_stats =
        endpoint->connection()->sent_packet_manager().GetRttStats();
    // Ownership of the sender will be overtaken by the endpoint.
    PccSender* sender = new PccSender(
        rtt_stats,
        QuicSentPacketManagerPeer::GetUnackedPacketMap(
            QuicConnectionPeer::GetSentPacketManager(endpoint->connection())),
        kInitialCongestionWindowPackets, kDefaultMaxCongestionWindowPackets,
        &random_);
    QuicConnectionPeer::SetSendAlgorithm(endpoint->connection(), sender);
    // Enable output traces via Sponge
    endpoint->RecordTrace();
    return sender;
  }

  // Creates a network setup with a bottleneck between the receiver and the
  // switch. The switch has the buffers that is double bottleneck BDP, which
  // should guarantee zero loss for PCC.
  void CreateDefaultSetup() {
    switch_ = QuicMakeUnique<simulator::Switch>(&simulator_, "Switch", 8,
                                                2 * kTestBdp);
    pcc_sender1_link_ = QuicMakeUnique<simulator::SymmetricLink>(
        &pcc_sender1_, switch_->port(1), kLocalLinkBandwidth,
        kLocalPropagationDelay);
    receiver_link_ = QuicMakeUnique<simulator::SymmetricLink>(
        &receiver1_, switch_->port(2), kTestLinkBandwidth,
        kTestPropagationDelay);
  }

  // Creates a network with the same bottleneck as the default setup, but with
  // two competing senders.
  void CreateCompetitionSetup() {
    switch_ = QuicMakeUnique<simulator::Switch>(&simulator_, "Switch", 8,
                                                2 * kTestBdp);

    // Add a small offset to the competing link in order to avoid
    // synchronization effects.
    const QuicTime::Delta small_offset = QuicTime::Delta::FromMicroseconds(3);

    pcc_sender1_link_ = QuicMakeUnique<simulator::SymmetricLink>(
        &pcc_sender1_, switch_->port(1), kLocalLinkBandwidth,
        kLocalPropagationDelay);
    pcc_sender2_link_ = QuicMakeUnique<simulator::SymmetricLink>(
        &pcc_sender2_, switch_->port(3), kLocalLinkBandwidth,
        kLocalPropagationDelay + small_offset);
    receiver_link_ = QuicMakeUnique<simulator::SymmetricLink>(
        &receiver_multiplexer_, switch_->port(2), kTestLinkBandwidth,
        kTestPropagationDelay);
  }

  // Creates a PCC vs PCC competition setup.
  void CreatePccVsPccSetup() {
    SetupPccSender(&pcc_sender2_);
    CreateCompetitionSetup();
  }

  void DoSimpleTransfer(QuicByteCount transfer_size, QuicTime::Delta deadline) {
    pcc_sender1_.AddBytesToTransfer(transfer_size);
    bool simulator_result = simulator_.RunUntilOrTimeout(
        [this]() { return pcc_sender1_.bytes_to_transfer() == 0; }, deadline);
    EXPECT_TRUE(simulator_result)
        << "PCC simple transfer failed.  Bytes remaining: "
        << pcc_sender1_.bytes_to_transfer();
  }

  simulator::Simulator simulator_;
  simulator::QuicEndpoint pcc_sender1_;
  simulator::QuicEndpoint pcc_sender2_;
  simulator::QuicEndpoint receiver1_;
  simulator::QuicEndpoint receiver2_;
  simulator::QuicEndpointMultiplexer receiver_multiplexer_;
  std::unique_ptr<simulator::Switch> switch_;
  std::unique_ptr<simulator::SymmetricLink> pcc_sender1_link_;
  std::unique_ptr<simulator::SymmetricLink> pcc_sender2_link_;
  std::unique_ptr<simulator::SymmetricLink> receiver_link_;

  SimpleRandom random_;
  const QuicClock* clock_;
  const RttStats* rtt_stats_;

  PccSender* sender_;
};

TEST_F(PccSenderTest, AlwaysCreatNonUsefulIntervalUntilRttStatValid) {
  sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                       HAS_RETRANSMITTABLE_DATA);
  // Advance the clock by 1 sec.
  clock_.AdvanceTime(QuicTime::Delta::FromSeconds(1));
  // Sent another packet.
  sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                       HAS_RETRANSMITTABLE_DATA);
  // The two packets should be in one non-useful monitor interval.
  EXPECT_EQ(1u, PccSenderPeer::num_intervals(&sender_));
  EXPECT_EQ(0u, PccSenderPeer::num_useful(&sender_));

  // Set the smoothed rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Send packets for another interval, it will be useful.
  SendPacketsInOneInterval();
  EXPECT_EQ(2u, PccSenderPeer::num_intervals(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::num_useful(&sender_));

  // Advance the clock by 1 sec, so next sent packet will be in a new interval.
  clock_.AdvanceTime(QuicTime::Delta::FromSeconds(1));
  // Send a new packet.
  sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                       HAS_RETRANSMITTABLE_DATA);
  // A new non-useful monitor interval is created.
  EXPECT_EQ(3u, PccSenderPeer::num_intervals(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::num_useful(&sender_));
}

TEST_F(PccSenderTest, StayInStarting) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  // First initialize the smoothed rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Sender should start in STARTING mode.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  SendPacketsInOneInterval();
  AckPackets(1, packet_number_ + 1);

  ExpectApproxEq(2.0f * initial_rate_mbps,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
}

TEST_F(PccSenderTest, StartingToProbing) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Losing all packets causes utility to decrease.
  LosePackets(1, packet_number_ + 1);

  ExpectApproxEq(0.5f * initial_rate_mbps,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
}

TEST_F(PccSenderTest, ProbingToDecisionMadeIncrease) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  // Sent a packet to create a non-useful interval in the queue.
  sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                       HAS_RETRANSMITTABLE_DATA);
  // Advance the clock by 100 us, so the next packet is sent at different time.
  clock_.AdvanceTime(QuicTime::Delta::FromMicroseconds(100));
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Set the mode of sender to PROBING for convenience.
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets in 6 intervals (4 useful and 2 non useful).
  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing + 1; ++i) {
    SendPacketsInOneInterval();
  }
  // Verify sending_rate restores to central rate.
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
  SendPacketsInOneInterval();
  // Verify sending_rate remains at central rate.
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);

  AckPackets(1, packet_number_ + 1);
  // The intervals with larger sending rate should have higher utility, because
  // there's no packet loss. In that case, the sender will enter DECISION_MADE
  // and increase sending rate. Specifically, the sending rate will be changed
  // to initial_rate_mbps*(1+0.05)*(1+0.02).
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.05f * 1.02f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send and acknowledge packets in a interval with the increased sending rate.
  QuicPacketNumber start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  AckPackets(start_packet_number, packet_number_ + 1);
  // Sender should further increase sending rate in DECISION_MADE mode, with a
  // larger step size.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.05f * 1.02f * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, ProbingToDecisionMadeDecrease) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  // Initialize RTT. So the first interval would be useful.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing + 1; ++i) {
    SendPacketsInOneInterval();
  }
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);

  // Losing all the packets, so that larger sending rate has smaller utility.
  LosePackets(1, packet_number_ + 1);
  // The sender will enter DECISION_MADE and decrease sending rate.
  // The sending rate will be changed to initial_rate_mbps*(1-0.05)*(1-0.02).
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::DECREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 0.95f * 0.98f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, ProbingWithoutNonUsefulInterval) {
  base::SetFlag(&FLAGS_restore_central_rate_upon_app_limited, true);
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets in 4 intervals, without non-useful interval in the end.
  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing; ++i) {
    SendPacketsInOneInterval();
  }
  // Sending rate is not central rate because no non-useful interval is started.
  if (PccSenderPeer::direction(&sender_) == PccSender::INCREASE) {
    ExpectApproxEq(initial_rate_mbps * 1.05f,
                   PccSenderPeer::sending_rate(&sender_), 0.001f);
  } else {
    ExpectApproxEq(initial_rate_mbps * 0.95f,
                   PccSenderPeer::sending_rate(&sender_), 0.001f);
  }
  // Acknowledge all the sent packets.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, clock_.Now(), packets_acked_,
                            packets_lost_);
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  // Sending rate is increased by the correct amount.
  ExpectApproxEq(initial_rate_mbps * 1.05f * 1.02f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, CannotMakeDecisionBecauseInconsistentResults) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Set the mode of sender to PROBING.
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets for 4 probing intervals, and losing all the packets for
  // intervals using different sending rates in the two interval groups, so that
  // the sender cannot make a decision.
  std::vector<QuicPacketNumber> end_packet_number;
  for (size_t i = 0; i < kNumIntervalGroupsInProbing * 2; ++i) {
    SendPacketsInOneInterval();
    end_packet_number.push_back(packet_number_);
  }
  QuicPacketNumber start_packet_number = 1;
  PccSender::RateChangeDirection loss_packet_direction = PccSender::INCREASE;
  for (size_t i = 0; i < kNumIntervalGroupsInProbing; ++i) {
    for (size_t j = 0; j < 2; ++j) {
      if (PccSenderPeer::direction(&sender_) == loss_packet_direction) {
        AckPackets(start_packet_number, end_packet_number[2 * i + j] + 1);
      } else {
        LosePackets(start_packet_number, end_packet_number[2 * i + j] + 1);
      }
      start_packet_number = end_packet_number[2 * i + j] + 1;
    }

    // Toggle the sending rate with packet losses in the next interval group.
    loss_packet_direction = loss_packet_direction == PccSender::INCREASE
                                ? PccSender::DECREASE
                                : PccSender::INCREASE;
  }

  // The sender will stay in PROBING mode, because the two groups of monitor
  // intervals cannot lead to consistent decision. Thus, rounds_ is increased.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, StayInDecisionMade) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(1, packet_number_ + 1);

  // Sender stays in DECISION_MADE mode, and further increases sending rate to
  // initial_rate_mbps * (1 + rounds_ * 0.02), where rounds_ equals 2.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, EarlyTerminationInStarting) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  QuicTime::Delta rtt = QuicTime::Delta::FromMicroseconds(30000);
  rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send a burst of packets in a useful monitor interval, which should be
  // enough to trigger early termination.
  for (size_t i = 1; i < 100; ++i) {
    sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                         HAS_RETRANSMITTABLE_DATA);
  }
  // Try to acknowledge all but the last packet in the interval, so that its
  // utility will not be available.
  for (size_t i = 1; i < packet_number_; ++i) {
    if (PccSenderPeer::num_intervals(&sender_) == 0) {
      // Early termination is triggered, do not ack the remaining packets.
      break;
    }
    // Inflate RTT by 10 persent each time, and ackowledge the sent packets one
    // at a time.
    rtt = 1.1 * rtt;
    rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
    AckPackets(i, i + 1);
  }

  // Store the bandwidth sample.
  float end_sending_rate = sender_.BandwidthEstimate().IsZero()
      ? initial_rate_mbps / 2.0f
      : std::min(initial_rate_mbps / 2.0f,
                 PccSenderPeer::bandwidth_sample(&sender_) * 0.95f);
  // Sender already exits STARTING mode, and reduces the sending rate to
  // initial_rate_mbps / 2, with rounds_ reset to 1. Because not all packets are
  // acknowledged, the reason for sender mode change must be early termination.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(end_sending_rate, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, LatencyInflationToleranceInStarting) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  QuicTime::Delta rtt = QuicTime::Delta::FromMicroseconds(30000);
  rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Set latency inflation tolerance in STARTING to 30 percent.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting, 0.30);
  base::SetFlag(&FLAGS_rtt_fluctuation_tolerance_gain_in_starting, 2.5);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 2.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Acknowledge the first packet in this interval, to update the value of
  // rtt_on_monitor_start member variable.
  AckPackets(start_packet_number, start_packet_number + 1);
  start_packet_number ++;
  // Inflate the smoothed RTT by
  // 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting.
  IncreaseSmoothedRttByRatio(0.5 *
      FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting);
  // Acknowledge all the packets for the second useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);

  // Sender stays in STARTING mode, and increases sending rate to
  // initial_rate_mbps * 4, where rounds_ equals 3.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(3u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 4.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the third useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Acknowledge all the packets in the interval one by one, and inflated the
  // latest RTT by as large as 50 percent each time.
  for (size_t i = start_packet_number; i <= packet_number_; ++i) {
    if (PccSenderPeer::num_intervals(&sender_) == 0) {
      break;
    }
    rtt = 1.5 * rtt;
    rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
    packets_acked_.clear();
    AckPackets(i, i + 1);
  }

  float end_sending_rate = sender_.BandwidthEstimate().IsZero()
      ? initial_rate_mbps * 2.0f
      : std::min(initial_rate_mbps * 2.0f,
                 PccSenderPeer::bandwidth_sample(&sender_) * 0.95f);
  // Sender exits STARTING mode, and halves the sending rate to
  // initial_rate_mbps * 2, with rounds_ reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(end_sending_rate, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, NoLatencyInflationToleranceInStarting) {
  // Disable latency inflation tolerance in STARTING.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting, 0.0);
  base::SetFlag(&FLAGS_rtt_fluctuation_tolerance_gain_in_starting, 0.0);
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMicroseconds(30000),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 2.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Acknowledge the first packet in this interval, to update the value of
  // rtt_on_monitor_start member variable.
  AckPackets(start_packet_number, start_packet_number + 1);
  start_packet_number ++;
  // Inflate the smoothed rtt slightly by 2 percent.
  IncreaseSmoothedRttByRatio(0.02);
  // Acknowledge all the packets for the second useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);

  // Sender exits STARTING mode, and halves the sending rate to
  // initial_rate_mbps, with rounds_ reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(
      sender_.BandwidthEstimate().IsZero()
          ? initial_rate_mbps
          : std::min(initial_rate_mbps,
                     PccSenderPeer::bandwidth_sample(&sender_) * 0.95f),
      PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, LatencyInflationToleranceInDecisionMade) {
  // Set latency inflation gain and tolerance in DECISION_MADE to default.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made,
                0.05);
  base::SetFlag(&FLAGS_rtt_fluctuation_tolerance_gain_in_decision_made, 1.5);
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  QuicTime::Delta rtt = QuicTime::Delta::FromMicroseconds(30000);
  rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  // Sender stays in DECISION_MADE mode, and further increases sending rate to
  // initial_rate_mbps * (1 + rounds_ * 0.02), where rounds_ equals 2.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  AckPackets(start_packet_number, start_packet_number + 1);
  start_packet_number ++;
  // Inflate the smoothed rtt by
  // 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made.
  IncreaseSmoothedRttByRatio(0.5 *
      FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made);
  // Acknowledge all the packets for the second useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);

  // Sender stays in DECISION_MADE mode, and further increases sending rate by
  // rounds_ * 0.02, where rounds_ equals 3.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(3u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f * 1.06f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the third useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Acknowledge all the packets in the interval one by one, and inflated the
  // latest RTT by 20 percent each time.
  for (size_t i = start_packet_number; i <= packet_number_; ++i) {
    rtt = 1.2 * rtt;
    rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
    packets_acked_.clear();
    AckPackets(i, i + 1);
  }

  // Sender will enter PROBING mode, and change sending rate to central rate
  // initial_rate_mbps * 1.04. Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, NoLatencyInflationToleranceInDecisionMade) {
  // Disable latency inflation tolerance in STARTING.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made,
                0.0);
  base::SetFlag(&FLAGS_rtt_fluctuation_tolerance_gain_in_decision_made, 0.0);
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMicroseconds(30000),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  // Sender stays in DECISION_MADE mode, and further increases sending rate to
  // initial_rate_mbps * (1 + rounds_ * 0.02), where rounds_ equals 2.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  AckPackets(start_packet_number, start_packet_number + 1);
  start_packet_number ++;
  // Inflate the smoothed rtt slightly by 2 percent.
  IncreaseSmoothedRttByRatio(0.02);
  // Acknowledge all the packets for the second useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);

  // Sender will enter PROBING mode, and change sending rate to
  // initial_rate_mbps. Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, DecisionMadeToProbing) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Mark all the packets as lost, which will lead to a negative utility.
  LosePackets(1, packet_number_ + 1);

  // Sender will enter PROBING mode, and change sending rate to central rate
  // initial_rate_mbps / (1 + 0.02). Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps / 1.02f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, StayInProbingBecauseInsufficientResults) {
  // Set the sender to have seen RTT, so as to avoid changing sending rate when
  // manually update RTT.
  PccSenderPeer::SetSeenRtt(&sender_);
  // Sent a packet to create a non-useful interval in the queue.
  sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                       HAS_RETRANSMITTABLE_DATA);
  // Advance the clock by 100 us, so the next packet is sent at different time.
  clock_.AdvanceTime(QuicTime::Delta::FromMicroseconds(100));
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets for only 3 probing intervals.
  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing - 1; ++i) {
    SendPacketsInOneInterval();
  }

  AckPackets(1, packet_number_ + 1);
  // The sender cannot make a decision because there are less than
  // kNumIntervalGroupsInProbing utilities. It will stay in PROBING. The sending
  // rate will be reset to central rate, and rounds_ will be increased.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

// Test a simple long data transfer in the default setup.
TEST_F(PccSenderSimulatorTest, SimpleTransfer) {
  // Disable Ack Decimation on the receiver, because it can increase srtt.
  QuicConnectionPeer::SetAckMode(receiver1_.connection(),
                                 QuicConnection::AckMode::TCP_ACKING);
  CreateDefaultSetup();

  // Verify that Sender is in slow start.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(sender_));

  DoSimpleTransfer(12 * 1024 * 1024, QuicTime::Delta::FromSeconds(30));
  // TODO(tongmeng): add zero packet loss check after PCC changes to rtt
  // deviation based early termination instead of rtt ratio based.
  // EXPECT_EQ(0u, pcc_sender1_.connection()->GetStats().packets_lost);
  // There should be low RTT inflation.
  ExpectApproxEq(kTestRtt, rtt_stats_->smoothed_rtt(), 0.2f);
}

// Test that two PCC flows started slightly apart from each other terminate.
TEST_F(PccSenderSimulatorTest, SimpleCompetition) {
  const QuicByteCount transfer_size = 10 * 1024 * 1024;
  const QuicTime::Delta transfer_time =
      kTestLinkBandwidth.TransferTime(transfer_size);
  CreatePccVsPccSetup();

  // Transfer 10% of data in first transfer.
  pcc_sender1_.AddBytesToTransfer(transfer_size);
  bool simulator_result = simulator_.RunUntilOrTimeout(
      [this, transfer_size]() {
        return receiver1_.bytes_received() >= 0.1 * transfer_size;
      },
      0.2 * transfer_time);
  EXPECT_TRUE(simulator_result);

  // Start the second transfer and wait until both finish.
  pcc_sender2_.AddBytesToTransfer(transfer_size);
  simulator_result = simulator_.RunUntilOrTimeout(
      [this, transfer_size]() {
        return receiver1_.bytes_received() == transfer_size &&
               receiver2_.bytes_received() == transfer_size;
      },
      3 * transfer_time);
  ASSERT_TRUE(simulator_result);
}

}  // namespace
}  // namespace test
}  // namespace quic
