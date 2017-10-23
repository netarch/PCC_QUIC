#include "third_party/pcc_quic/pcc_sender.h"

#include <algorithm>
#include <memory>

#include "gfe/quic/core/congestion_control/rtt_stats.h"
#include "gfe/quic/platform/api/quic_test.h"
#include "gfe/quic/test_tools/mock_clock.h"
#include "gfe/quic/test_tools/quic_test_utils.h"

namespace gfe_quic {
DECLARE_double(max_rtt_fluctuation_tolerance_ratio_in_starting);
DECLARE_double(max_rtt_fluctuation_tolerance_ratio_in_decision_made);

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
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number; ++i) {
      packets_acked_.push_back(std::make_pair(i, kMaxPacketSize));
    }
  }

  // Mark packets within range [start_packet_number, end_packet_number) as lost.
  void LosePackets(QuicPacketNumber start_packet_number,
                   QuicPacketNumber end_packet_number) {
    for (QuicPacketNumber i = start_packet_number; i < end_packet_number; ++i) {
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
  // First initialize the smoothed rtt to be 30ms.
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  // Sender should start in STARTING mode.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

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
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

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
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender stays in DECISION_MADE mode, and further increases sending rate to
  // initial_rate_mbps * (1 + rounds_ * 0.02), where rounds_ equals 2.
  EXPECT_EQ(PccSender::DECISION_MADE, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  EXPECT_EQ(PccSender::INCREASE, PccSenderPeer::direction(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, EarlyTerminationInStarting) {
  QuicTime::Delta rtt = QuicTime::Delta::FromMicroseconds(30000);
  rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);

  // Send all the packets for a useful monitor interval.
  SendPacketsInOneInterval();
  // Update the RTT so that the smoothed rtt is increased by
  // 1.1 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting.
  rtt_stats_.UpdateRtt(
      (1 + 8 * 1.1 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
          rtt_stats_.smoothed_rtt(),
      QuicTime::Delta::Zero(), QuicTime::Zero());
  // Allow a small margin for float/int64_t cast.
  EXPECT_LT(
      ((1 + 1.1 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) * rtt)
              .ToMicroseconds() -
          rtt_stats_.smoothed_rtt().ToMicroseconds(),
      2);
  // Acknowledge all the packets for the interval.
  packets_acked_.clear();
  AckPackets(1, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender exits STARTING mode, and halves the sending rate to
  // initial_rate_mbps / 2, with rounds_ reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps / 2, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, LatencyInflationToleranceInStarting) {
  QuicTime::Delta rtt = QuicTime::Delta::FromMicroseconds(30000);
  rtt_stats_.UpdateRtt(rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 2.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Update the RTT so that the smoothed rtt is increased by
  // 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting.
  rtt_stats_.UpdateRtt(
      (1 + 8 * 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
          rtt_stats_.smoothed_rtt(),
      QuicTime::Delta::Zero(), QuicTime::Zero());
  // Allow a small margin for float/int64_t cast.
  EXPECT_LT(
      ((1 + 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
          rtt).ToMicroseconds() -
      rtt_stats_.smoothed_rtt().ToMicroseconds(), 2);
  rtt = rtt_stats_.smoothed_rtt();
  // Acknowledge all the packets for the second useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender stays in STARTING mode, and increases sending rate to
  // initial_rate_mbps * 4, where rounds_ equals 3.
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(3u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 4.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the third useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Update the RTT so that the smoothed rtt is further increased by
  // 1.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting.
  rtt_stats_.UpdateRtt(
      (1 + 8 * 1.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
          rtt_stats_.smoothed_rtt(),
      QuicTime::Delta::Zero(), QuicTime::Zero());
  // Allow a small margin for float/int64_t cast.
  EXPECT_LT(
      ((1 + 1.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
          rtt).ToMicroseconds() -
      rtt_stats_.smoothed_rtt().ToMicroseconds(), 2);
  // Acknowledge all the packets for the third useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender exits STARTING mode, and halves the sending rate to
  // initial_rate_mbps * 2, with rounds_ reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 2.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, NoLatencyInflationToleranceInStarting) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMicroseconds(30000),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Disable latency inflation tolerance in STARTING.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting, 0.0);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
  EXPECT_EQ(PccSender::STARTING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(2u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 2.0f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);

  // Send all the packets for the second useful monitor interval.
  start_packet_number = packet_number_ + 1;
  SendPacketsInOneInterval();
  // Update the RTT so that the smoothed rtt is increased by 2 percent.
  QuicTime::Delta new_rtt = (1 + 8 * 0.02) * rtt_stats_.smoothed_rtt();
  rtt_stats_.UpdateRtt(new_rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  EXPECT_EQ(static_cast<int64_t>(30000.0 * 1.02),
            rtt_stats_.smoothed_rtt().ToMicroseconds());
  // Acknowledge all the packets for the second useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender exits STARTING mode, and halves the sending rate to
  // initial_rate_mbps, with rounds_ reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, LatencyInflationToleranceInDecisionMade) {
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
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
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
  // Update the RTT so that the smoothed rtt is increased by
  // 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made.
  rtt_stats_.UpdateRtt(
      (1 + 4 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made) *
          rtt_stats_.smoothed_rtt(),
      QuicTime::Delta::Zero(), QuicTime::Zero());
  // Allow a small margin for float/int64_t cast.
  EXPECT_LT(
      ((1 + 0.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made) *
          rtt).ToMicroseconds() -
      rtt_stats_.smoothed_rtt().ToMicroseconds(), 2);
  rtt = rtt_stats_.smoothed_rtt();
  // Acknowledge all the packets for the second useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

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
  // Update the RTT so that the smoothed rtt is further increased by
  // 1.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made.
  rtt_stats_.UpdateRtt(
      (1 + 12 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made) *
          rtt_stats_.smoothed_rtt(),
      QuicTime::Delta::Zero(), QuicTime::Zero());
  // Allow a small margin for float/int64_t cast.
  EXPECT_LT(
      ((1 + 1.5 * FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made) *
          rtt).ToMicroseconds() -
      rtt_stats_.smoothed_rtt().ToMicroseconds(), 2);
  // Acknowledge all the packets for the third useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender will enter PROBING mode, and change sending rate to central rate
  // initial_rate_mbps * 1.04. Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps * 1.04f,
                 PccSenderPeer::sending_rate(&sender_), 0.001f);
}

TEST_F(PccSenderTest, NoLatencyInflationToleranceInDecisionMade) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMicroseconds(30000),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
  PccSenderPeer::SetMode(&sender_, PccSender::DECISION_MADE,
                         PccSender::INCREASE);
  float initial_rate_mbps = PccSenderPeer::sending_rate(&sender_);
  QuicPacketNumber start_packet_number = 1;

  // Disable latency inflation tolerance in DECISION_MADE.
  base::SetFlag(&FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made,
                0.0);

  // Send all the packets for the first useful monitor interval.
  SendPacketsInOneInterval();
  // Acknowledge all the packets for the first useful interval.
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);
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
  // Update the RTT so that the smoothed rtt is increased by 2 percent.
  QuicTime::Delta new_rtt = (1 + 8 * 0.02) * rtt_stats_.smoothed_rtt();
  rtt_stats_.UpdateRtt(new_rtt, QuicTime::Delta::Zero(), QuicTime::Zero());
  EXPECT_EQ(static_cast<int64_t>(30000.0 * 1.02),
            rtt_stats_.smoothed_rtt().ToMicroseconds());
  // Acknowledge all the packets for the second useful interval.
  packets_acked_.clear();
  AckPackets(start_packet_number, packet_number_ + 1);
  sender_.OnCongestionEvent(true, 0, QuicTime::Zero(), packets_acked_,
                            packets_lost_);

  // Sender will enter PROBING mode, and change sending rate to
  // initial_rate_mbps. Also, rounds_ is reset to 1.
  EXPECT_EQ(PccSender::PROBING, PccSenderPeer::mode(&sender_));
  EXPECT_EQ(1u, PccSenderPeer::rounds(&sender_));
  ExpectApproxEq(initial_rate_mbps, PccSenderPeer::sending_rate(&sender_),
                 0.001f);
}

TEST_F(PccSenderTest, DecisionMadeToProbing) {
  rtt_stats_.UpdateRtt(QuicTime::Delta::FromMilliseconds(30),
                       QuicTime::Delta::Zero(), QuicTime::Zero());
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

}  // namespace
}  // namespace test
}  // namespace gfe_quic
