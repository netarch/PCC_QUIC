#include "third_party/pcc_quic/pcc_sender.h"

#include <algorithm>

#include "base/commandlineflags.h"
#include "gfe/quic/core/congestion_control/rtt_stats.h"
#include "gfe/quic/core/quic_time.h"
#include "gfe/quic/platform/api/quic_str_cat.h"

namespace gfe_quic {

DEFINE_double(max_rtt_fluctuation_tolerance_ratio_in_starting, 0.3,
              "Ignore RTT fluctuation within 30 percent in STARTING mode");
DEFINE_double(max_rtt_fluctuation_tolerance_ratio_in_decision_made, 0.05,
              "Ignore RTT fluctuation within 5 percent in DECISION_MADE mode");

namespace {
// Step size for rate change in PROBING mode.
const float kProbingStepSize = 0.05f;
// Base percentile step size for rate change in DECISION_MADE mode.
const float kDecisionMadeStepSize = 0.02f;
// Maximum percentile step size for rate change in DECISION_MADE mode.
const float kMaxDecisionMadeStepSize = 0.10f;
// Groups of useful monitor intervals each time in PROBING mode.
const size_t kNumIntervalGroupsInProbing = 2;
// Number of bits per byte.
const size_t kBitsPerByte = 8;
}  // namespace

PccSender::PccSender(const RttStats* rtt_stats,
                     QuicPacketCount initial_congestion_window,
                     QuicPacketCount max_congestion_window,
                     QuicRandom* random)
    : mode_(STARTING),
      sending_rate_(QuicBandwidth::FromBitsPerSecond(
          initial_congestion_window * kDefaultTCPMSS * kBitsPerByte *
          kNumMicrosPerSecond / rtt_stats->initial_rtt_us())),
      latest_utility_(0.0),
      monitor_duration_(QuicTime::Delta::Zero()),
      direction_(INCREASE),
      rounds_(1),
      interval_queue_(/*delegate=*/this),
      max_cwnd_bytes_(max_congestion_window * kDefaultTCPMSS),
      rtt_stats_(rtt_stats),
      random_(random) {
}

void PccSender::OnPacketSent(QuicTime sent_time,
                             QuicByteCount bytes_in_flight,
                             QuicPacketNumber packet_number,
                             QuicByteCount bytes,
                             HasRetransmittableData is_retransmittable) {
  // Start a new monitor interval if the interval queue is empty. If latest RTT
  // is available, start a new monitor interval if (1) there is no useful
  // interval or (2) it has been more than monitor_duration since the last
  // interval starts.
  if (interval_queue_.empty() ||
      (!rtt_stats_->latest_rtt().IsZero() &&
       (interval_queue_.num_useful_intervals() == 0 ||
        sent_time - interval_queue_.current().first_packet_sent_time >
            monitor_duration_))) {
    MaybeSetSendingRate();
    // Set the monitor duration to 1.5 of smoothed rtt.
    monitor_duration_ = QuicTime::Delta::FromMicroseconds(
        rtt_stats_->smoothed_rtt().ToMicroseconds() * 1.5);

    float rtt_fluctuation_tolerance_ratio = 0.0;
    // No rtt fluctuation tolerance no during PROBING.
    if (mode_ == STARTING) {
      // Use a larger tolerance at START to boost sending rate.
      rtt_fluctuation_tolerance_ratio =
          FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting;
    } else if (mode_ == DECISION_MADE) {
      rtt_fluctuation_tolerance_ratio =
          FLAGS_max_rtt_fluctuation_tolerance_ratio_in_decision_made;
    }

    bool is_useful = CreateUsefulInterval();
    // Use halved sending rate for non-useful intervals.
    interval_queue_.EnqueueNewMonitorInterval(
        is_useful ? sending_rate_ : 0.5 * sending_rate_, is_useful,
        rtt_fluctuation_tolerance_ratio,
        rtt_stats_->smoothed_rtt().ToMicroseconds());
  }
  interval_queue_.OnPacketSent(sent_time, packet_number, bytes);
}

void PccSender::OnCongestionEvent(bool rtt_updated,
                                  QuicByteCount bytes_in_flight,
                                  QuicTime event_time,
                                  const AckedPacketVector& acked_packets,
                                  const LostPacketVector& lost_packets) {
  int64_t avg_rtt_us = rtt_stats_->smoothed_rtt().ToMicroseconds();
  if (avg_rtt_us == 0) {
    QUIC_BUG_IF(mode_ != STARTING);
    avg_rtt_us = rtt_stats_->initial_rtt_us();
  } else {
    if (mode_ == STARTING && !interval_queue_.empty() &&
        avg_rtt_us >
            static_cast<int64_t>(
                (1 + FLAGS_max_rtt_fluctuation_tolerance_ratio_in_starting) *
                static_cast<double>(
                    interval_queue_.current().rtt_on_monitor_start_us))) {
      // Directly enter PROBING when rtt inflation already exceeds the tolerance
      // ratio, so as to reduce packet losses and mitigate rtt inflation.
      interval_queue_.OnRttInflationInStarting();
      EnterProbing();
      return;
    }
  }

  interval_queue_.OnCongestionEvent(acked_packets, lost_packets, avg_rtt_us);
}

bool PccSender::CanSend(QuicByteCount bytes_in_flight) {
  return true;
}

QuicBandwidth PccSender::PacingRate(QuicByteCount bytes_in_flight) const {
  return interval_queue_.empty() ? sending_rate_
                                 : interval_queue_.current().sending_rate;
}

QuicBandwidth PccSender::BandwidthEstimate() const {
  return QuicBandwidth::Zero();
}

QuicByteCount PccSender::GetCongestionWindow() const {
  // Use smoothed_rtt to calculate expected congestion window except when it
  // equals 0, which happens when the connection just starts.
  int64_t rtt_us = rtt_stats_->smoothed_rtt().ToMicroseconds() == 0
                       ? rtt_stats_->initial_rtt_us()
                       : rtt_stats_->smoothed_rtt().ToMicroseconds();
  return static_cast<QuicByteCount>(sending_rate_.ToBytesPerSecond() * rtt_us /
                                    kNumMicrosPerSecond);
}

bool PccSender::InSlowStart() const { return false; }

bool PccSender::InRecovery() const { return false; }

bool PccSender::IsProbingForMoreBandwidth() const { return false; }

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
      "[st=", mode_, ",", "r=", sending_rate_.ToKBitsPerSecond(), ",",
      "pu=", QuicStringPrintf("%.15g", latest_utility_), ",",
      "dir=", direction_, ",", "round=", rounds_, ",",
      "num=", interval_queue_.num_useful_intervals(), "]",
      "[r=", mi.sending_rate.ToKBitsPerSecond(), ",", "use=", mi.is_useful, ",",
      "(", mi.first_packet_sent_time.ToDebuggingValue(), "-", ">",
      mi.last_packet_sent_time.ToDebuggingValue(), ")", "(",
      mi.first_packet_number, "-", ">", mi.last_packet_number, ")", "(",
      mi.bytes_sent, "/", mi.bytes_acked, "/", mi.bytes_lost, ")", "(",
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
        sending_rate_ = sending_rate_ * 2;
        latest_utility_ = utility_info[0].utility;
        ++rounds_;
      } else {
        // Enter PROBING mode if utility decreases.
        EnterProbing();
      }
      break;
    case PROBING:
      if (CanMakeDecision(utility_info)) {
        DCHECK_EQ(2 * kNumIntervalGroupsInProbing, utility_info.size());
        // Enter DECISION_MADE mode if a decision is made.
        direction_ = (utility_info[0].utility > utility_info[1].utility)
                         ? ((utility_info[0].sending_rate >
                             utility_info[1].sending_rate)
                                ? INCREASE
                                : DECREASE)
                         : ((utility_info[0].sending_rate >
                             utility_info[1].sending_rate)
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
          sending_rate_ = sending_rate_ *
                          (1 + std::min(rounds_ * kDecisionMadeStepSize,
                                        kMaxDecisionMadeStepSize));
        } else {
          sending_rate_ = sending_rate_ *
                          (1 - std::min(rounds_ * kDecisionMadeStepSize,
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
  if (rtt_stats_->smoothed_rtt().ToMicroseconds() == 0) {
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
      sending_rate_ = sending_rate_ * (1.0 / (1 + kProbingStepSize));
    } else {
      sending_rate_ = sending_rate_ * (1.0 / (1 - kProbingStepSize));
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
    sending_rate_ = sending_rate_ * (1 + kProbingStepSize);
  } else {
    sending_rate_ = sending_rate_ * (1 - kProbingStepSize);
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
            ? utility_info[2 * i].sending_rate >
                  utility_info[2 * i + 1].sending_rate
            : utility_info[2 * i].sending_rate <
                  utility_info[2 * i + 1].sending_rate;

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
      sending_rate_ = sending_rate_ * 0.5;
      break;
    case DECISION_MADE:
      // Use sending rate right before utility decreases as central probing
      // rate.
      if (direction_ == INCREASE) {
        sending_rate_ = sending_rate_ *
                        (1.0 / (1 + std::min(rounds_ * kDecisionMadeStepSize,
                                             kMaxDecisionMadeStepSize)));
      } else {
        sending_rate_ = sending_rate_ *
                        (1.0 / (1 - std::min(rounds_ * kDecisionMadeStepSize,
                                             kMaxDecisionMadeStepSize)));
      }
      break;
    case PROBING:
      // Reset sending rate to central rate when sender does not have enough
      // data to send more than 2 * kNumIntervalGroupsInProbing intervals.
      if (interval_queue_.current().is_useful) {
        if (direction_ == INCREASE) {
          sending_rate_ = sending_rate_ * (1.0 / (1 + kProbingStepSize));
        } else {
          sending_rate_ = sending_rate_ * (1.0 / (1 - kProbingStepSize));
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
    sending_rate_ = sending_rate_ * (1 + kProbingStepSize) *
                    (1 + kDecisionMadeStepSize);
  } else {
    sending_rate_ = sending_rate_ * (1 - kProbingStepSize) *
                    (1 - kDecisionMadeStepSize);
  }

  mode_ = DECISION_MADE;
  rounds_ = 1;
}

}  // namespace gfe_quic
