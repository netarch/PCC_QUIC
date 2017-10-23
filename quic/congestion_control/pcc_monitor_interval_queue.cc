#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

#include "gfe/quic/core/congestion_control/rtt_stats.h"

namespace gfe_quic {

namespace {
// Number of probing MonitorIntervals necessary for Probing.
const size_t kRoundsPerProbing = 4;
// Tolerance of loss rate by utility function.
const float kLossTolerance = 0.05f;
// Coefficeint of the loss rate term in utility function.
const float kLossCoefficient = -1000.0f;
// Coefficient of RTT term in utility function.
const float kRTTCoefficient = -200.0f;

}  // namespace

MonitorInterval::MonitorInterval()
    : sending_rate(QuicBandwidth::Zero()),
      is_useful(false),
      rtt_fluctuation_tolerance_ratio(0.0),
      first_packet_sent_time(QuicTime::Zero()),
      last_packet_sent_time(QuicTime::Zero()),
      first_packet_number(0),
      last_packet_number(0),
      bytes_sent(0),
      bytes_acked(0),
      bytes_lost(0),
      rtt_on_monitor_start_us(0),
      rtt_on_monitor_end_us(0),
      utility(0.0) {}

MonitorInterval::MonitorInterval(QuicBandwidth sending_rate,
                                 bool is_useful,
                                 float rtt_fluctuation_tolerance_ratio,
                                 int64_t rtt_us)
    : sending_rate(sending_rate),
      is_useful(is_useful),
      rtt_fluctuation_tolerance_ratio(rtt_fluctuation_tolerance_ratio),
      first_packet_sent_time(QuicTime::Zero()),
      last_packet_sent_time(QuicTime::Zero()),
      first_packet_number(0),
      last_packet_number(0),
      bytes_sent(0),
      bytes_acked(0),
      bytes_lost(0),
      rtt_on_monitor_start_us(rtt_us),
      rtt_on_monitor_end_us(rtt_us),
      utility(0.0) {}

UtilityInfo::UtilityInfo()
    : sending_rate(QuicBandwidth::Zero()), utility(0.0) {}

UtilityInfo::UtilityInfo(QuicBandwidth rate, float utility)
    : sending_rate(rate), utility(utility) {}

PccMonitorIntervalQueue::PccMonitorIntervalQueue(
    PccMonitorIntervalQueueDelegateInterface* delegate)
    : num_useful_intervals_(0),
      num_available_intervals_(0),
      delegate_(delegate) {}

void PccMonitorIntervalQueue::EnqueueNewMonitorInterval(
    QuicBandwidth sending_rate,
    bool is_useful,
    float rtt_fluctuation_tolerance_ratio,
    int64_t rtt_us) {
  if (is_useful) {
    ++num_useful_intervals_;
  }

  monitor_intervals_.emplace_back(sending_rate, is_useful,
                                  rtt_fluctuation_tolerance_ratio, rtt_us);
}

void PccMonitorIntervalQueue::OnPacketSent(QuicTime sent_time,
                                           QuicPacketNumber packet_number,
                                           QuicByteCount bytes) {
  if (monitor_intervals_.empty()) {
    QUIC_BUG << "OnPacketSent called with empty queue.";
    return;
  }

  if (monitor_intervals_.back().bytes_sent == 0) {
    // This is the first packet of this interval.
    monitor_intervals_.back().first_packet_sent_time = sent_time;
    monitor_intervals_.back().first_packet_number = packet_number;
  }

  monitor_intervals_.back().last_packet_sent_time = sent_time;
  monitor_intervals_.back().last_packet_number = packet_number;
  monitor_intervals_.back().bytes_sent += bytes;
}

void PccMonitorIntervalQueue::OnCongestionEvent(
    const AckedPacketVector& acked_packets,
    const LostPacketVector& lost_packets,
    int64_t rtt_us) {
  num_available_intervals_ = 0;
  if (num_useful_intervals_ == 0) {
    // Skip all the received packets if no intervals are useful.
    return;
  }

  bool has_invalid_utility = false;
  for (MonitorInterval& interval : monitor_intervals_) {
    if (!interval.is_useful) {
      // Skips useless monitor intervals.
      continue;
    }

    if (IsUtilityAvailable(interval)) {
      // Skip intervals with available utilities.
      ++num_available_intervals_;
      continue;
    }

    for (const LostPacket& lost_packet : lost_packets) {
      if (IntervalContainsPacket(interval, lost_packet.packet_number)) {
        interval.bytes_lost += lost_packet.bytes_lost;
      }
    }

    for (const AckedPacket& acked_packet : acked_packets) {
      if (IntervalContainsPacket(interval, acked_packet.packet_number)) {
        interval.bytes_acked += acked_packet.bytes_acked;
      }
    }

    if (IsUtilityAvailable(interval)) {
      interval.rtt_on_monitor_end_us = rtt_us;
      has_invalid_utility = !CalculateUtility(&interval);
      if (has_invalid_utility) {
        break;
      }
      ++num_available_intervals_;
      QUIC_BUG_IF(num_available_intervals_ > num_useful_intervals_);
    }
  }

  if (num_useful_intervals_ > num_available_intervals_ &&
      !has_invalid_utility) {
    return;
  }

  if (!has_invalid_utility) {
    DCHECK_GT(num_useful_intervals_, 0u);

    std::vector<UtilityInfo> utility_info;
    for (const MonitorInterval& interval : monitor_intervals_) {
      if (!interval.is_useful) {
        continue;
      }
      // All the useful intervals should have available utilities now.
      utility_info.push_back(
          UtilityInfo(interval.sending_rate, interval.utility));
    }
    DCHECK_EQ(num_available_intervals_, utility_info.size());

    delegate_->OnUtilityAvailable(utility_info);
  }

  // Remove MonitorIntervals from the head of the queue,
  // until all useful intervals are removed.
  while (num_useful_intervals_ > 0) {
    if (monitor_intervals_.front().is_useful) {
      --num_useful_intervals_;
    }
    monitor_intervals_.pop_front();
  }
  num_available_intervals_ = 0;
}

const MonitorInterval& PccMonitorIntervalQueue::current() const {
  DCHECK(!monitor_intervals_.empty());
  return monitor_intervals_.back();
}

bool PccMonitorIntervalQueue::empty() const {
  return monitor_intervals_.empty();
}

size_t PccMonitorIntervalQueue::size() const {
  return monitor_intervals_.size();
}

void PccMonitorIntervalQueue::OnRttInflationInStarting() {
  monitor_intervals_.clear();
  num_useful_intervals_ = 0;
  num_available_intervals_ = 0;
}

bool PccMonitorIntervalQueue::IsUtilityAvailable(
    const MonitorInterval& interval) const {
  return (interval.bytes_acked + interval.bytes_lost == interval.bytes_sent);
}

bool PccMonitorIntervalQueue::IntervalContainsPacket(
    const MonitorInterval& interval,
    QuicPacketNumber packet_number) const {
  return (packet_number >= interval.first_packet_number &&
          packet_number <= interval.last_packet_number);
}

bool PccMonitorIntervalQueue::CalculateUtility(MonitorInterval* interval) {
  if (interval->last_packet_sent_time == interval->first_packet_sent_time) {
    // Cannot get valid utility if interval only contains one packet.
    return false;
  }
  const int64_t kMinTransmissionTime = 1l;
  int64_t mi_duration = std::max(
      kMinTransmissionTime,
      (interval->last_packet_sent_time - interval->first_packet_sent_time)
          .ToMicroseconds());

  float rtt_ratio = static_cast<float>(interval->rtt_on_monitor_start_us) /
                    static_cast<float>(interval->rtt_on_monitor_end_us);
  if (rtt_ratio > 1.0 - interval->rtt_fluctuation_tolerance_ratio &&
      rtt_ratio < 1.0 + interval->rtt_fluctuation_tolerance_ratio) {
    rtt_ratio = 1.0;
  }
  float latency_penalty =
      1.0 - 1.0 / (1.0 + exp(kRTTCoefficient * (1.0 - rtt_ratio)));

  float bytes_acked = static_cast<float>(interval->bytes_acked);
  float bytes_lost = static_cast<float>(interval->bytes_lost);
  float bytes_sent = static_cast<float>(interval->bytes_sent);
  float loss_rate = bytes_lost / bytes_sent;
  float loss_penalty =
      1.0 - 1.0 / (1.0 + exp(kLossCoefficient * (loss_rate - kLossTolerance)));

  interval->utility = (bytes_acked / static_cast<float>(mi_duration) *
                           loss_penalty * latency_penalty -
                       bytes_lost / static_cast<float>(mi_duration)) *
                      1000.0;

  return true;
}

}  // namespace gfe_quic
                                                                                
