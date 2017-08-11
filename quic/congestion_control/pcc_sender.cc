#include "third_party/pcc_quic/pcc_sender.h"

#include <stdio.h>
#include <algorithm>

#include "gfe/quic/core/congestion_control/rtt_stats.h"
#include "gfe/quic/core/quic_time.h"
#include "gfe/quic/platform/api/quic_str_cat.h"

namespace gfe_quic {

namespace {
// Minimum sending rate of the connection.
const float kMinSendingRate = 2.0f;
// Step size for rate change in PROBING mode.
const float kProbingStepSize = 0.05f;
// Base step size for rate change in DECISION_MADE mode.
const float kDecisionMadeStepSize = 0.02f;
// Maximum step size for rate change in DECISION_MADE mode.
const float kMaxDecisionMadeStepSize = 0.10f;
// Groups of useful monitor intervals each time in PROBING mode.
const size_t kNumIntervalGroupsInProbing = 2;
// Number of bits per byte.
const size_t kBitsPerByte = 8;
// Number of bits per Mbit.
const size_t kMegabit = 1024 * 1024;
// Rtt moving average weight.
const float kAverageRttWeight = 0.1;
}  // namespace

PccSender::PccSender(const RttStats* rtt_stats,
                     QuicPacketCount initial_congestion_window,
                     QuicPacketCount max_congestion_window,
                     QuicRandom* random)
    : mode_(STARTING),
      latest_utility_(0.0),
      monitor_duration_(QuicTime::Delta::Zero()),
      direction_(INCREASE),
      rounds_(1),
      interval_queue_(/*delegate=*/this),
      avg_rtt_(QuicTime::Delta::Zero()),
      last_rtt_(QuicTime::Delta::Zero()),
      time_last_rtt_received_(QuicTime::Zero()),
      max_cwnd_bits_(max_congestion_window * kDefaultTCPMSS * kBitsPerByte),
      rtt_stats_(rtt_stats),
      random_(random) {
  sending_rate_mbps_ =
      std::max(static_cast<float>(initial_congestion_window * kDefaultTCPMSS *
                                  kBitsPerByte) /
                   static_cast<float>(rtt_stats_->initial_rtt_us()),
               kMinSendingRate);
}

bool PccSender::OnPacketSent(QuicTime sent_time,
                             QuicByteCount bytes_in_flight,
                             QuicPacketNumber packet_number,
                             QuicByteCount bytes,
                             HasRetransmittableData is_retransmittable) {
  // Start a new monitor interval if (1) there is no useful interval in the
  // queue, or (2) it has been more than monitor_duration since the last
  // interval starts.
  if (interval_queue_.num_useful_intervals() == 0 ||
      sent_time - interval_queue_.current().first_packet_sent_time >
          monitor_duration_) {
    MaybeSetSendingRate();
    // Set the monitor duration to be 1.5 of avg_rtt_.
    monitor_duration_ = 1.5 * avg_rtt_;

    interval_queue_.EnqueueNewMonitorInterval(
        sending_rate_mbps_, CreateUsefulInterval(),
        avg_rtt_.ToMicroseconds());
  }
  interval_queue_.OnPacketSent(sent_time, packet_number, bytes);

  return true;
}

void PccSender::OnCongestionEvent(bool rtt_updated,
                                  QuicByteCount bytes_in_flight,
                                  QuicTime event_time,
                                  const CongestionVector& acked_packets,
                                  const CongestionVector& lost_packets) {
  if (avg_rtt_.IsZero()) {
    avg_rtt_ = rtt_stats_->latest_rtt();
  } else {
    // Ideal packet interval under pacing should be (packet_size/sending_rate).
    // Considering delayed ACK and Nagle's algorithm in practice, each ACK
    // usually acknowledges at least two packets. Thus, we conservatively use
    // that value as the threshold to detect ack aggregation.
    QuicTime::Delta ack_aggregation_threshold =
        QuicTime::Delta::FromMicroseconds(kMaxPacketSize * kBitsPerByte /
                                          sending_rate_mbps_);
    if (event_time - time_last_rtt_received_ > ack_aggregation_threshold) {
      avg_rtt_ = (1 - kAverageRttWeight) * avg_rtt_ +
                    kAverageRttWeight * last_rtt_;
    }
  }
  last_rtt_ = rtt_stats_->latest_rtt();
  time_last_rtt_received_ = event_time;

  interval_queue_.OnCongestionEvent(acked_packets, lost_packets,
                                    avg_rtt_.ToMicroseconds());
}

QuicTime::Delta PccSender::TimeUntilSend(QuicTime now,
                                         QuicByteCount bytes_in_flight) {
  return QuicTime::Delta::Zero();
}

QuicBandwidth PccSender::PacingRate(QuicByteCount bytes_in_flight) const {
  return QuicBandwidth::FromBitsPerSecond(
      static_cast<int64_t>(sending_rate_mbps_ * kMegabit));
}

QuicBandwidth PccSender::BandwidthEstimate() const {
  return QuicBandwidth::Zero();
}

QuicByteCount PccSender::GetCongestionWindow() const {
  // Use avg_rtt_ to calculate expected congestion window except when it
  // equals 0, which happens when the connection just starts.
  int64_t rtt_us = avg_rtt_.IsZero()
                   ? rtt_stats_->initial_rtt_us()
                   : avg_rtt_.ToMicroseconds();
  return static_cast<QuicByteCount>(sending_rate_mbps_ * rtt_us / kBitsPerByte);
}

bool PccSender::InSlowStart() const { return false; }

bool PccSender::InRecovery() const { return false; }

QuicByteCount PccSender::GetSlowStartThreshold() const { return 0; }

CongestionControlType PccSender::GetCongestionControlType() const {
  return kPCC;
}

string PccSender::GetDebugState() const {
  if (interval_queue_.empty()) {
    return "pcc??";
  }

  const MonitorInterval& mi = interval_queue_.current();
  std::string msg = QuicStrCat(
      "[st=", mode_, ",", "r=", sending_rate_mbps_, ",", "pu=", latest_utility_,
      ",", "dir=", direction_, ",", "round=", rounds_, ",",
      "num=", interval_queue_.num_useful_intervals(), ")",
      "[r=", mi.sending_rate_mbps, ",", "use=", mi.is_useful, ",", "(",
      mi.first_packet_sent_time.ToDebuggingValue(), "-", ">",
      mi.last_packet_sent_time.ToDebuggingValue(), ")", "(",
      mi.first_packet_number, "-", ">", mi.last_packet_number, ")", "(",
      mi.bytes_total, "/", mi.bytes_acked, "/", mi.bytes_lost, ")", "(",
      mi.rtt_on_monitor_start_us, "-", ">", mi.rtt_on_monitor_end_us, ")");
  return msg;
}

void PccSender::OnUtilityAvailable(
    const std::vector<UtilityInfo>& utility_info) {
  switch (mode_) {
    case STARTING:
      DCHECK_EQ(1u, utility_info.size());
      if (utility_info[0].utility > latest_utility_) {
        // Stay in STARTING mode. Double the sending rate and update
        // latest_utility.
        sending_rate_mbps_ *= 2;
        latest_utility_ = utility_info[0].utility;
        ++rounds_;
      } else {
        // Enter PROBING mode if utility decreases.
        EnterProbing();
      }
      break;
    case PROBING:
      if (CanMakeDecision(utility_info)) {
        // Enter DECISION_MADE mode if a decision is made.
        direction_ = (utility_info[0].utility > utility_info[1].utility)
                         ? ((utility_info[0].sending_rate_mbps >
                             utility_info[1].sending_rate_mbps)
                                ? INCREASE
                                : DECREASE)
                         : ((utility_info[0].sending_rate_mbps >
                             utility_info[1].sending_rate_mbps)
                                ? DECREASE
                                : INCREASE);
        latest_utility_ =
            std::max(utility_info[2 * kNumIntervalGroupsInProbing - 2].utility,
                     utility_info[2 * kNumIntervalGroupsInProbing - 1].utility);
        EnterDecisionMade();
      } else {
        // Stays in PROBING mode.
        EnterProbing();
      }
      break;
    case DECISION_MADE:
      DCHECK_EQ(1u, utility_info.size());
      if (utility_info[0].utility > latest_utility_) {
        // Remain in DECISION_MADE mode. Keep increasing or decreasing the
        // sending rate.
        ++rounds_;
        if (direction_ == INCREASE) {
          sending_rate_mbps_ *= (1 + std::min(rounds_ * kDecisionMadeStepSize,
                                              kMaxDecisionMadeStepSize));
        } else {
          sending_rate_mbps_ *= (1 - std::min(rounds_ * kDecisionMadeStepSize,
                                              kMaxDecisionMadeStepSize));
        }
        latest_utility_ = utility_info[0].utility;
      } else {
        // Enter PROBING mode if utility decreases.
        EnterProbing();
      }
      break;
  }
}

bool PccSender::CreateUsefulInterval() const {
  if (avg_rtt_.IsZero()) {
    // Create non useful intervals upon starting a connection, until there is
    // valid rtt stats.
    QUIC_BUG_IF(mode_ != STARTING);
    return false;
  }
  // In STARTING and DECISION_MADE mode, there should be at most one useful
  // intervals in the queue; while in PROBING mode, there should be at most
  // 2 * kNumIntervalGroupsInProbing.
  size_t max_num_useful =
      (mode_ == PROBING) ? 2 * kNumIntervalGroupsInProbing : 1;
  return interval_queue_.num_useful_intervals() < max_num_useful;
}

void PccSender::MaybeSetSendingRate() {
  if (mode_ != PROBING || (interval_queue_.num_useful_intervals() ==
                               2 * kNumIntervalGroupsInProbing &&
                           !interval_queue_.current().is_useful)) {
    // Do not change sending rate when (1) current mode is STARTING or
    // DECISION_MADE (since sending rate is already changed in
    // OnUtilityAvailable), or (2) more than 2 * kNumIntervalGroupsInProbing
    // intervals have been created in PROBING mode.
    return;
  }

  if (interval_queue_.num_useful_intervals() != 0) {
    // Restore central sending rate.
    if (direction_ == INCREASE) {
      sending_rate_mbps_ /= (1 + kProbingStepSize);
    } else {
      sending_rate_mbps_ /= (1 - kProbingStepSize);
    }

    if (interval_queue_.num_useful_intervals() ==
        2 * kNumIntervalGroupsInProbing) {
      // This is the first not useful monitor interval, its sending rate is the
      // central rate.
      return;
    }
  }

  // Sender creates several groups of monitor intervals. Each group comprises an
  // interval with increased sending rate and an interval with decreased sending
  // rate. Which interval goes first is randomly decided.
  if (interval_queue_.num_useful_intervals() % 2 == 0) {
    direction_ = (random_->RandUint64() % 2 == 1) ? INCREASE : DECREASE;
  } else {
    direction_ = (direction_ == INCREASE) ? DECREASE : INCREASE;
  }
  if (direction_ == INCREASE) {
    sending_rate_mbps_ *= (1 + kProbingStepSize);
  } else {
    sending_rate_mbps_ *= (1 - kProbingStepSize);
  }
}

bool PccSender::CanMakeDecision(
    const std::vector<UtilityInfo>& utility_info) const {
  // Determine whether increased or decreased probing rate has better utility.
  // Cannot make decision if number of utilities are less than
  // 2 * kNumIntervalGroupsInProbing. This happens when sender does not have
  // enough data to send.
  if (utility_info.size() < 2 * kNumIntervalGroupsInProbing) {
    return false;
  }

  bool increase = false;
  // All the probing groups should have consistent decision. If not, directly
  // return false.
  for (size_t i = 0; i < kNumIntervalGroupsInProbing; ++i) {
    bool increase_i =
        utility_info[2 * i].utility > utility_info[2 * i + 1].utility
            ? utility_info[2 * i].sending_rate_mbps >
                  utility_info[2 * i + 1].sending_rate_mbps
            : utility_info[2 * i].sending_rate_mbps <
                  utility_info[2 * i + 1].sending_rate_mbps;

    if (i == 0) {
      increase = increase_i;
    }
    // Cannot make decision if groups have inconsistent results.
    if (increase_i != increase) {
      return false;
    }
  }

  return true;
}

void PccSender::EnterProbing() {
  switch (mode_) {
    case STARTING:
      // Use half sending_rate_ as central probing rate.
      sending_rate_mbps_ /= 2;
      break;
    case DECISION_MADE:
      // Use sending rate right before utility decreases as central probing
      // rate.
      if (direction_ == INCREASE) {
        sending_rate_mbps_ /= (1 + std::min(rounds_ * kDecisionMadeStepSize,
                                            kMaxDecisionMadeStepSize));
      } else {
        sending_rate_mbps_ /= (1 - std::min(rounds_ * kDecisionMadeStepSize,
                                            kMaxDecisionMadeStepSize));
      }
      break;
    case PROBING:
      // Reset sending rate to central rate when sender does not have enough
      // data to send more than 2 * kNumIntervalGroupsInProbing intervals.
      if (interval_queue_.current().is_useful) {
        if (direction_ == INCREASE) {
          sending_rate_mbps_ /= (1 + kProbingStepSize);
        } else {
          sending_rate_mbps_ /= (1 - kProbingStepSize);
        }
      }
      break;
  }

  if (mode_ == PROBING) {
    ++rounds_;
    return;
  }

  mode_ = PROBING;
  rounds_ = 1;
}

void PccSender::EnterDecisionMade() {
  DCHECK_EQ(PROBING, mode_);

  // Change sending rate from central rate based on the probing rate with higher
  // utility.
  if (direction_ == INCREASE) {
    sending_rate_mbps_ *= (1 + kProbingStepSize) * (1 + kDecisionMadeStepSize);
  } else {
    sending_rate_mbps_ *= (1 - kProbingStepSize) * (1 - kDecisionMadeStepSize);
  }

  mode_ = DECISION_MADE;
  rounds_ = 1;
}

}  // namespace gfe_quic
