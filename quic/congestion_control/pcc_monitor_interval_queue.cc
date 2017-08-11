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
    : first_packet_sent_time(QuicTime::Zero()),
      last_packet_sent_time(QuicTime::Zero()),
      first_packet_number(0),
      last_packet_number(0),
      bytes_total(0),
      bytes_acked(0),
      bytes_lost(0),
      utility(0.0) {}

MonitorInterval::MonitorInterval(float sending_rate_mbps,
                                 bool is_useful,
                                 int64_t rtt_us)
    : sending_rate_mbps(sending_rate_mbps),
      is_useful(is_useful),
      first_packet_sent_time(QuicTime::Zero()),
      last_packet_sent_time(QuicTime::Zero()),
      first_packet_number(0),
      last_packet_number(0),
      bytes_total(0),
      bytes_acked(0),
      bytes_lost(0),
      rtt_on_monitor_start_us(rtt_us),
      rtt_on_monitor_end_us(rtt_us),
      utility(0.0) {}

UtilityInfo::UtilityInfo() : sending_rate_mbps(0.0), utility(0.0) {}

UtilityInfo::UtilityInfo(float rate, float utility)
    : sending_rate_mbps(rate), utility(utility) {}

PccMonitorIntervalQueue::PccMonitorIntervalQueue(
    PccMonitorIntervalQueueDelegateInterface* delegate)
    : num_useful_intervals_(0),
      num_available_intervals_(0),
      delegate_(delegate) {}

void PccMonitorIntervalQueue::EnqueueNewMonitorInterval(float sending_rate_mbps,
                                                        bool is_useful,
                                                        int64_t rtt_us) {
  if (is_useful) {
    ++num_useful_intervals_;
  }

  monitor_intervals_.emplace_back(sending_rate_mbps, is_useful, rtt_us);
}

void PccMonitorIntervalQueue::OnPacketSent(QuicTime sent_time,
                                           QuicPacketNumber packet_number,
                                           QuicByteCount bytes) {
  DCHECK(!monitor_intervals_.empty());

  if (monitor_intervals_.back().bytes_total == 0) {
    // This is the first packet of this interval.
    monitor_intervals_.back().first_packet_sent_time = sent_time;
    monitor_intervals_.back().first_packet_number = packet_number;
  }

  monitor_intervals_.back().last_packet_sent_time = sent_time;
  monitor_intervals_.back().last_packet_number = packet_number;
  monitor_intervals_.back().bytes_total += bytes;
}

void PccMonitorIntervalQueue::OnCongestionEvent(
    const SendAlgorithmInterface::CongestionVector& acked_packets,
    const SendAlgorithmInterface::CongestionVector& lost_packets,
    int64_t rtt_us) {
  if (num_useful_intervals_ == 0) {
    // Skip all the received packets if no intervals are useful.
    return;
  }

  bool has_invalid_utility = false;
  for (MonitorInterval& interval : monitor_intervals_) {
    if (!interval.is_useful || IsUtilityAvailable(interval)) {
      // Skips intervals that are not useful, or have available utilities
      continue;
    }

    for (SendAlgorithmInterface::CongestionVector::const_iterator it =
             lost_packets.cbegin();
         it != lost_packets.cend(); ++it) {
      if (IntervalContainsPacket(interval, it->first)) {
        interval.bytes_lost += it->second;
      }
    }

    for (SendAlgorithmInterface::CongestionVector::const_iterator it =
             acked_packets.cbegin();
         it != acked_packets.cend(); ++it) {
      if (IntervalContainsPacket(interval, it->first)) {
        interval.bytes_acked += it->second;
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
          UtilityInfo(interval.sending_rate_mbps, interval.utility));
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

bool PccMonitorIntervalQueue::IsUtilityAvailable(
    const MonitorInterval& interval) const {
  return (interval.bytes_acked + interval.bytes_lost == interval.bytes_total);
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

  float bytes_acked = static_cast<float>(interval->bytes_acked);
  float bytes_lost = static_cast<float>(interval->bytes_lost);
  float bytes_total = static_cast<float>(interval->bytes_total);

  float current_utility =
      (bytes_acked / static_cast<float>(mi_duration) *
           (1.0 -
            1.0 / (1.0 + exp(kLossCoefficient *
                             (bytes_lost / bytes_total - kLossTolerance)))) *
           (1.0 - 1.0 / (1.0 + exp(kRTTCoefficient * (1.0 - rtt_ratio)))) -
       bytes_lost / static_cast<float>(mi_duration)) *
      1000.0;

  interval->utility = current_utility;
  return true;
}

}  // namespace gfe_quic
