#include "third_party/pcc_quic/pcc_sender.h"

#include <algorithm>
#include <memory>

#include "gfe/quic/core/congestion_control/rtt_stats.h"
#include "gfe/quic/platform/api/quic_test.h"
#include "gfe/quic/test_tools/mock_clock.h"
#include "gfe/quic/test_tools/quic_test_utils.h"

namespace gfe_quic {
namespace test {

class PccSenderPeer {
 public:
  static PccSender::SenderMode mode(PccSender* sender) { return sender->mode_; }

  static float sending_rate(PccSender* sender) {
    return sender->sending_rate_mbps_;
  }

  static size_t rounds(PccSender* sender) { return sender->rounds_; }

  static PccSender::RateChangeDirection direction(PccSender* sender) {
    return sender->direction_;
  }

  static QuicTime interval_start_time(PccSender* sender) {
    return sender->interval_queue_.current().first_packet_sent_time;
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

class PccSenderTest : public QuicTest {
 protected:
  PccSenderTest()
      : sender_(&rtt_stats_,
                kInitialCongestionWindowPackets,
                kDefaultMaxCongestionWindowPackets,
                &random_),
        packet_number_(0) {
    uint64_t seed = QuicRandom::GetInstance()->RandUint64();
    random_.set_seed(seed);
  }

  // Create a monitor interval and send maximum packets allowed in it based on
  // sending rate and interval duration. Returns the number of sent packets.
  void SendPacketsInOneInterval() {
    do {
      sender_.OnPacketSent(clock_.Now(), 0, ++packet_number_, kMaxPacketSize,
                           HAS_RETRANSMITTABLE_DATA);
      clock_.AdvanceTime(QuicTime::Delta::FromMicroseconds(
          kMaxPacketSize * 8 / PccSenderPeer::sending_rate(&sender_)));
    } while (clock_.Now() - PccSenderPeer::interval_start_time(&sender_) <
             PccSenderPeer::duration(&sender_));
  }

  // Mark packets within range [start_packet_number, end_packet_number) as
  // acked.
  void AckPackets(QuicPacketNumber start_packet_number,
                  QuicPacketNumber end_packet_number) {
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number;
         ++i) {
      packets_acked_.push_back(std::make_pair(i, kMaxPacketSize));
    }
  }

  // Mark packets within range [start_packet_number, end_packet_number) as lost.
  void LosePackets(QuicPacketNumber start_packet_number,
                   QuicPacketNumber end_packet_number) {
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number;
         ++i) {
      packets_lost_.push_back(std::make_pair(i, kMaxPacketSize));
    }
  }

  RttStats rtt_stats_;
  SimpleRandom random_;
  PccSender sender_;

  // List of acked/lost packets, used when calling OnCongestionEvent on sender_.
  SendAlgorithmInterface::CongestionVector packets_acked_;
  SendAlgorithmInterface::CongestionVector packets_lost_;

  QuicPacketNumber packet_number_;

  MockClock clock_;
};

TEST_F(PccSenderTest, AlwaysCreatNonUsefulIntervalUntilRttStatValid) {
  // Send Packets in 10 monitor intervals without initializing RTT stats. All
  // 10 intervals are non useful.
  for (size_t i = 0; i < 10; ++i) {
    SendPacketsInOneInterval();
    // Each non-useful interval contains one pacekt.
    EXPECT_EQ(i + 1, packet_number_);
  }
  // Verify that there are 10 intervals, but none of them are useful.
  EXPECT_EQ(10u, PccSenderPeer::num_intervals(&sender_));
  EXPECT_EQ(0u, PccSenderPeer::num_useful(&sender_));

  // Set the latest rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Acknowledge all sent packets and avg_rtt_ is updated.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  // Sent packets for another interval, it will be useful.
  SendPacketsInOneInterval();
  EXPECT_EQ(11u, PccSenderPeer::num_intervals(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::num_useful(&sender_));
}

TEST_F(PccSenderTest, StayInStarting) {
  // Sender should start in STARTING mode.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // First initialize the latest rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  SendPacketsInOneInterval();
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  ExpectApproxEq(2.0f * initial_rate_mbps,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
}

TEST_F(PccSenderTest, StartingToProbing) {
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Losing all packets causes utility to decrease.
  LosePackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  ExpectApproxEq(0.5f * initial_rate_mbps,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
}

TEST_F(PccSenderTest, ProbingToDecisionMadeIncrease) {
  // Send Packets in a monitor interval without initializing RTT stats. This
  // interval is non-useful.
  SendPacketsInOneInterval();
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // The non-useful interval should contain only a single packet.
  EXPECT_EQ(1u, packet_number_);
  // Acknowledge the sent packet, so that avg_rtt_ can be updated.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
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

  AckPackets(2, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
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
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  // Sender should further increase sending rate in DECISION_MADE mode, with a
  // larger step size.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.05f * 1.02f * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, ProbingToDecisionMadeDecrease) {
  // Initialize RTT. So the first interval would be useful.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing + 1; ++i) {
    SendPacketsInOneInterval();
  }
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);

  // Losing all the packets, so that larger sending rate has smaller utility.
  LosePackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  // The sender will enter DECISION_MADE and decrease sending rate.
  // The sending rate will be changed to initial_rate_mbps*(1-0.05)*(1-0.02).
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(PccSender::DECREASE, PccSenderPeer::direction(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 0.95f * 0.98f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, CannotMakeDecisionBecauseInconsistentResults) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  // Set the mode of sender to PROBING.
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets for 4 probing intervals, and losing all the packets for
  // intervals using different sending rates in the two interval groups, so that
  // the sender cannot make a decision.
  QuicPacketNumber start_packet_number = 1;
  PccSender::RateChangeDirection loss_packet_direction = PccSender::INCREASE;
  for (size_t i = 0; i < kNumIntervalGroupsInProbing; ++i) {
    for (size_t i = 0; i < 2; ++i) {
      SendPacketsInOneInterval();
      if (PccSenderPeer::direction(&sender_) == loss_packet_direction) {
        AckPackets(start_packet_number, packet_number_ + 1);
      } else {
        LosePackets(start_packet_number, packet_number_ + 1);
      }
      start_packet_number = packet_number_ + 1;
    }

    // Toggle the sending rate with packet losses in the next interval group.
    loss_packet_direction = loss_packet_direction == PccSender::INCREASE
                                ? PccSender::DECREASE
                                : PccSender::INCREASE;
  }
  // Send packets for another non-useful interval.
  SendPacketsInOneInterval();
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // The sender will stay in PROBING mode, because the two groups of monitor
  // intervals cannot lead to consistent decision. Thus, rounds_ is increased.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, StayInDecisionMade) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender stays in DecisionMade mode, and further increases sending rate to
  // initial_rate_mbps * (1 + rounds_ * 0.02), where rounds_ equals 2.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, DecisionMadeToProbing) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Mark all the packets as lost, which will lead to a negative utility.
  LosePackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender will enter PROBING mode, and change sending rate to central rate
  // initial_rate_mbps / (1 + 0.02). Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps / 1.02f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, StayInProbingBecauseInsufficientResults) {
  // Send Packets in 10 monitor intervals without initializing RTT stats. All
  // 10 intervals are non useful.
  for (size_t i = 0; i < 10; ++i) {
    SendPacketsInOneInterval();
  }
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Each non-useful interval should contain only a single packet.
  EXPECT_EQ(10u, packet_number_);
  // Acknowledge the sent packet, so that avg_rtt_ can be updated.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  PccSenderPeer::SetMode(&sender_, PccSender::PROBING, PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send packets for only 3 probing intervals.
  for (size_t i = 0; i < 2 * kNumIntervalGroupsInProbing - 1; ++i) {
    SendPacketsInOneInterval();
  }

  AckPackets(10, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  // The sender cannot make a decision because there are less than
  // kNumIntervalGroupsInProbing utilities. It will stay in PROBING. The sending
  // rate will be reset to central rate, and rounds_ will be increased.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, FilteringAggregatedAck) {
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicTime event_time = QuicTime::Zero();

  // First initialize the latest rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Update avg_rtt_.
  sender_.OnCongestionEvent(true, 0, event_time, packets_acked_, packets_lost_);
  // Update the latest rtt sample to be 300ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(300),
                       QuicTime::Delta::Zero(), QuicTime::Zero());

  SendPacketsInOneInterval();
  // Acknowledge packets in the interval, leaving the last packet un-acked.
  // After that, the sender will store 300ms in last_rtt_, and avg_rtt_ remains
  // to be 30ms.
  AckPackets(1, packet_number_);
  sender_.OnCongestionEvent(true, 0, event_time, packets_acked_, packets_lost_);
  packets_acked_.clear();
  // Acknowledge the last packet after only 1us from the last ack. The sender
  // will not update avg_rtt_.
  event_time = event_time + QuicTime::Delta::FromMicroseconds(1);
  AckPackets(packet_number_, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, event_time, packets_acked_, packets_lost_);
  packets_acked_.clear();

  // The 300ms rtt sample will be filtered out due to aggregation. Sender stays
  // in STARING, and sending rate is doubled.
  ExpectApproxEq(2.0f * initial_rate_mbps,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));

  // Send packets for another interval, and acknowledge the packets after a long
  // interval of 100 ms from the last ack. That triggers the update of avg_rtt_.
  QuicPacketNumber start_packet_number = packet_number_;
  SendPacketsInOneInterval();
  event_time = event_time + QuicTime::Delta::FromMilliseconds(100);
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, event_time, packets_acked_, packets_lost_);
  // The interval between two consecutive acks are large enough. The 300ms
  // last_rtt_ leads to an increasing avg_rtt_ in PccSender, causing the sender
  // to exit STARTING and enter PROBING mode.
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
}

}  // namespace
}  // namespace test
}  // namespace gfe_quic
