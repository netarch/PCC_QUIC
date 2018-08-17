#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

#include "third_party/quic/core/congestion_control/rtt_stats.h"

namespace quic {

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
      rtt_on_monitor_start(QuicTime::Delta::Zero()),
      rtt_on_monitor_end(QuicTime::Delta::Zero()) {}

MonitorInterval::MonitorInterval(QuicBandwidth sending_rate,
                                 bool is_useful,
                                 float rtt_fluctuation_tolerance_ratio,
                                 QuicTime::Delta rtt)
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
      rtt_on_monitor_start(rtt),
      rtt_on_monitor_end(rtt) {}

PccMonitorIntervalQueue::PccMonitorIntervalQueue(
    PccMonitorIntervalQueueDelegateInterface* delegate)
    : num_useful_intervals_(0),
      num_available_intervals_(0),
      delegate_(delegate) {}

void PccMonitorIntervalQueue::EnqueueNewMonitorInterval(
    QuicBandwidth sending_rate,
    bool is_useful,
    float rtt_fluctuation_tolerance_ratio,
    QuicTime::Delta rtt) {
  if (is_useful) {
    ++num_useful_intervals_;
  }

  monitor_intervals_.emplace_back(sending_rate, is_useful,
                                  rtt_fluctuation_tolerance_ratio, rtt);
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
    QuicTime::Delta rtt) {
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
        if (interval.bytes_acked == 0) {
          // This is the RTT before starting sending at interval.sending_rate.
          interval.rtt_on_monitor_start = rtt;
        }
        interval.bytes_acked += acked_packet.bytes_acked;
      }
    }

    if (IsUtilityAvailable(interval)) {
      interval.rtt_on_monitor_end = rtt;
      has_invalid_utility = HasInvalidUtility(&interval);
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

    std::vector<const MonitorInterval *> useful_intervals;
    for (const MonitorInterval& interval : monitor_intervals_) {
      if (!interval.is_useful) {
        continue;
      }
      useful_intervals.push_back(&interval);
    }
    DCHECK_EQ(num_available_intervals_, useful_intervals.size());

    delegate_->OnUtilityAvailable(useful_intervals);
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

const MonitorInterval& PccMonitorIntervalQueue::front() const {
  DCHECK(!monitor_intervals_.empty());
  return monitor_intervals_.front();
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

bool PccMonitorIntervalQueue::HasInvalidUtility(
    const MonitorInterval* interval) const {
  return interval->first_packet_sent_time == interval->last_packet_sent_time;
}

}  // namespace quic
