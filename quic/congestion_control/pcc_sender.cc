/*
 * pcc_sender.cc
 *
 *  Created on: March 28, 2016
 *      Authors:
 *               Xuefeng Zhu (zhuxuefeng1994@126.com)
 *               Mo Dong (modong2@illinois.edu)
 */
#include <stdio.h>

#include "net/quic/congestion_control/pcc_sender.h"
#include "net/quic/congestion_control/rtt_stats.h"
#include "base/time/time.h"

namespace net {

PCCMonitor::PCCMonitor()
  : start_time(QuicTime::Zero()),
    end_time(QuicTime::Zero()),
    end_transmission_time(QuicTime::Zero()),
    start_seq_num(-1),
    end_seq_num(-1){
}

PCCMonitor::~PCCMonitor(){
}

PCCSender::PCCSender(const RttStats* rtt_stats)
  : current_monitor_(-1),
    current_monitor_end_time_(QuicTime::Zero()),
    rtt_stats_(rtt_stats),
    ideal_next_packet_send_time_(QuicTime::Zero()),
    previous_timer_(QuicTime::Zero()),
    send_bytes_(0),
    ack_bytes_(0){
}


PCCSender::~PCCSender() {}

void PCCSender::SetFromConfig(const QuicConfig& config,
                                 Perspective perspective) {
}

void PCCSender::ResumeConnectionState(
    const CachedNetworkParameters& cached_network_params,
    bool max_bandwidth_resumption) {
}

void PCCSender::SetNumEmulatedConnections(int num_connections) {

}

void PCCSender::SetMaxCongestionWindow(
    QuicByteCount max_congestion_window) {
}

bool PCCSender::OnPacketSent(
    QuicTime sent_time,
    QuicByteCount bytes_in_flight,
    QuicPacketNumber packet_number,
    QuicByteCount bytes,
    HasRetransmittableData has_retransmittable_data) {

  if (previous_timer_.Subtract(QuicTime::Zero()).IsZero()) {
    previous_timer_ = sent_time;
  }

  if (sent_time.Subtract(previous_timer_).ToSeconds()) {
    printf("rtt %ldus\n", rtt_stats_->smoothed_rtt().ToMicroseconds());
    printf("sent at rate %f mbps\n", (double)send_bytes_*8 / 1024 / 1024);
    printf("throughput rate %f mbps\n\n", (double)ack_bytes_*8 / 1024 / 1024);

    previous_timer_ = sent_time;
    send_bytes_ = 0;
    ack_bytes_ = 0;
  } else {
    send_bytes_ += bytes;
  }

  // TODO : case for retransmission
  if (current_monitor_end_time_.Subtract(QuicTime::Zero()).IsZero()) {
    StartMonitor(sent_time);
    monitors_[current_monitor_].start_seq_num = packet_number;
    ideal_next_packet_send_time_ = sent_time;
  } else {
    QuicTime::Delta diff = sent_time.Subtract(current_monitor_end_time_);
    if (diff.ToMicroseconds() > 0) {
      monitors_[current_monitor_].state = WAITING;
      monitors_[current_monitor_].end_transmission_time = sent_time;
      monitors_[current_monitor_].end_seq_num = packet_number;
      current_monitor_end_time_ = QuicTime::Zero();
    }
  }
  PacketInfo packet_info;
  packet_info.sent_time = sent_time;
  packet_info.bytes = bytes;
  monitors_[current_monitor_].packet_vector.push_back(packet_info);
  QuicTime::Delta delay = QuicTime::Delta::FromMicroseconds(
    bytes * 8 * base::Time::kMicrosecondsPerSecond / pcc_utility_.GetCurrentRate() / 1024 / 1024);
  ideal_next_packet_send_time_ = ideal_next_packet_send_time_.Add(delay);
  return true;
}

void PCCSender::StartMonitor(QuicTime sent_time){
  current_monitor_ = (current_monitor_ + 1) % NUM_MONITOR;
  pcc_utility_.OnMonitorStart(current_monitor_);

  // calculate monitor interval and monitor end time
  double rand_factor = double(rand() % 3) / 10;
  int64 srtt = rtt_stats_->latest_rtt().ToMicroseconds();
  if (srtt == 0) {
    srtt = rtt_stats_->initial_rtt_us();
  }
  QuicTime::Delta monitor_interval =
      QuicTime::Delta::FromMicroseconds(srtt * (1.5 + rand_factor));
  current_monitor_end_time_ = sent_time.Add(monitor_interval);

  monitors_[current_monitor_].state = SENDING;
  monitors_[current_monitor_].srtt = srtt;
  monitors_[current_monitor_].start_time = sent_time;
  monitors_[current_monitor_].end_time = QuicTime::Zero();
  monitors_[current_monitor_].end_transmission_time = QuicTime::Zero();
  monitors_[current_monitor_].packet_vector.clear();

}

void PCCSender::OnCongestionEvent(
    bool rtt_updated,
    QuicByteCount bytes_in_flight,
    const CongestionVector& acked_packets,
    const CongestionVector& lost_packets) {
  for (CongestionVector::const_iterator it = lost_packets.cbegin();
      it != lost_packets.cend(); ++it) {
    MonitorNumber monitor_num = GetMonitor(it->first);
    if (monitor_num == -1) {
      continue;
    }
    int pos = it->first - monitors_[monitor_num].start_seq_num;
    monitors_[monitor_num].packet_vector[pos].state = LOST;

    if (it->first == monitors_[monitor_num].end_seq_num) {
      EndMonitor(monitor_num);
    }
  }

  for (CongestionVector::const_iterator it = acked_packets.cbegin();
      it != acked_packets.cend(); ++it) {

    MonitorNumber monitor_num = GetMonitor(it->first);
    if (monitor_num == -1) {
      continue;
    }
    int pos = it->first - monitors_[monitor_num].start_seq_num;
    monitors_[monitor_num].packet_vector[pos].state = ACK;
    ack_bytes_ += monitors_[monitor_num].packet_vector[pos].bytes;

    if (it->first == monitors_[monitor_num].end_seq_num) {
      EndMonitor(monitor_num);
    }
  }
}

void PCCSender::EndMonitor(MonitorNumber monitor_num) {
  if (monitors_[monitor_num].state == WAITING){
    monitors_[monitor_num].state = FINISHED;
    pcc_utility_.OnMonitorEnd(monitors_[monitor_num], current_monitor_,
                              monitor_num);
  }
}

MonitorNumber PCCSender::GetMonitor(QuicPacketNumber sequence_number) {
  MonitorNumber result = current_monitor_;

  do {
    int diff = sequence_number - monitors_[result].start_seq_num;
    if (diff >= 0 && diff < (int)monitors_[result].packet_vector.size()) {
      return result;
    }

    result = (result + 99) % NUM_MONITOR;
  } while (result != current_monitor_);

  printf("Monitor is not found\n");
  return -1;
}

void PCCSender::OnRetransmissionTimeout(bool packets_retransmitted) {

}

QuicTime::Delta PCCSender::TimeUntilSend(
      QuicTime now,
      QuicByteCount bytes_in_flight,
      HasRetransmittableData has_retransmittable_data) const {
    // If the next send time is within the alarm granularity, send immediately.
  if (ideal_next_packet_send_time_ > now.Add(alarm_granularity_)) {
    DVLOG(1) << "Delaying packet: "
             << ideal_next_packet_send_time_.Subtract(now).ToMicroseconds();
    return ideal_next_packet_send_time_.Subtract(now);
  }

  DVLOG(1) << "Sending packet now";
  return QuicTime::Delta::Zero();
}

QuicBandwidth PCCSender::PacingRate() const {
  return QuicBandwidth::Zero();
}

QuicBandwidth PCCSender::BandwidthEstimate() const {
  return QuicBandwidth::Zero();
}

QuicTime::Delta PCCSender::RetransmissionDelay() const {
  return QuicTime::Delta::Zero();
}

QuicByteCount PCCSender::GetCongestionWindow() const {
  QuicByteCount pcc_utility_rate = (QuicByteCount)pcc_utility_.GetCurrentRate();
  QuicByteCount pcc_utility_state = (QuicByteCount)pcc_utility_.GetCurrentState();

  return pcc_utility_rate << 32 | pcc_utility_state;
}

bool PCCSender::InSlowStart() const {
  return false;
}

bool PCCSender::InRecovery() const {
  return false;
}

QuicByteCount PCCSender::GetSlowStartThreshold() const {
  return 1000*kMaxPacketSize;
}

CongestionControlType PCCSender::GetCongestionControlType() const {
  return kPcc;
}

PCCUtility::PCCUtility()
  : state_(STARTING),
    current_rate_(10),
    previous_utility_(-10000),
    previous_rtt_(0),
    previous_monitor_(-1),
    num_recorded_(0),
    guess_time_(0),
    continous_guess_count_(0),
    tartger_monitor_(0),
    change_direction_(0),
    change_intense_(1) {
    for (int i = 0; i < NUM_MONITOR; i++) {
      start_rate_array[i] = 0;
    }
  }

void PCCUtility::OnMonitorStart(MonitorNumber current_monitor) {
  UtilityState old_state;
  do {
    old_state = state_;
    switch (state_) {
      case STARTING:
        current_rate_ *= 2;
        start_rate_array[current_monitor] = current_rate_;
        break;
      case GUESSING:
        if (continous_guess_count_ == MAX_COUNTINOUS_GUESS) {
          continous_guess_count_ = 0;
        }

        state_ = RECORDING;
        continous_guess_count_++;

        for (int i = 0; i < NUMBER_OF_PROBE; i += 2) {
          int rand_dir = rand() % 2 * 2 - 1;

          guess_stat_bucket[i].rate = current_rate_
              + rand_dir * continous_guess_count_ * GRANULARITY * current_rate_;
          guess_stat_bucket[i + 1].rate = current_rate_
              - rand_dir * continous_guess_count_ * GRANULARITY * current_rate_;
        }

        for (int i = 0; i < NUMBER_OF_PROBE; i++) {
          guess_stat_bucket[i].monitor = (current_monitor + i) % NUM_MONITOR;
        }
        break;
      case RECORDING:
        current_rate_ = guess_stat_bucket[guess_time_].rate;
        guess_time_++;

        if (guess_time_ == NUMBER_OF_PROBE) {
          guess_time_ = 0;
        }
        break;
      default:
        break;
    }
  } while (old_state != state_);
}

void PCCUtility::OnMonitorEnd(PCCMonitor pcc_monitor,
                              MonitorNumber current_monitor,
                              MonitorNumber end_monitor) {

  double total = 0;
  double loss = 0;
  GetBytesSum(pcc_monitor.packet_vector, total, loss);

  int64 time =
      pcc_monitor.end_transmission_time.Subtract(pcc_monitor.start_time).ToMicroseconds();

  int64 srtt = pcc_monitor.srtt;
  if (previous_rtt_ == 0) previous_rtt_ = srtt;

  double current_utility = ((total - loss) / time *
    (1 - 1 / ( 1 + exp(-1000 * (loss / total - 0.05)))) *
    (1 - 1 / (1 + exp(-10 * (1 - previous_rtt_ / double(srtt))))) - 1 * loss / time) / 1 * 1000;

  previous_rtt_ = srtt;


  switch (state_) {
    case STARTING:
      if (end_monitor == 0) current_utility /= 2;

      if (end_monitor - previous_monitor_ > 1) {
          state_ = GUESSING;
          current_rate_ = previous_monitor_ == -1 ?
            start_rate_array[0] : start_rate_array[previous_monitor_];
        return;
      }

      if (previous_utility_ < current_utility) {
        previous_utility_ = current_utility;
        previous_monitor_ = end_monitor;
      } else {
        state_ = GUESSING;
        current_rate_ = start_rate_array[previous_monitor_];
      }
      return;
    case RECORDING:
      // find corresponding monitor
      for (int i = 0; i < NUMBER_OF_PROBE; i++) {
        if (end_monitor == guess_stat_bucket[i].monitor) {
          num_recorded_++;
          guess_stat_bucket[i].utility = current_utility;
        }
      }

      if (num_recorded_ == NUMBER_OF_PROBE) {
        num_recorded_ = 0;
        int decision = 0;

        for (int i = 0; i < NUMBER_OF_PROBE; i += 2) {
          bool case1 = guess_stat_bucket[i].utility > guess_stat_bucket[i + 1].utility
              && guess_stat_bucket[i].rate > guess_stat_bucket[i + 1].rate;
          bool case2 = guess_stat_bucket[i].utility < guess_stat_bucket[i + 1].utility
              && guess_stat_bucket[i].rate < guess_stat_bucket[i + 1].rate;

          decision += case1 || case2 ? 1 : -1;
        }

        if (decision == 0) {
          state_ = GUESSING;
        } else {
          state_ = MOVING;
          change_direction_ = decision > 0 ? 1 : -1;
          change_intense_ = 1;
          double change_amount = (continous_guess_count_/ 2 + 1)
              * change_intense_ * change_direction_ * GRANULARITY * current_rate_;
          current_rate_ += change_amount;

          previous_utility_ = 0;
          continous_guess_count_ = 0;
          tartger_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        }
      }
      return;
    case MOVING:
      if (end_monitor == tartger_monitor_) {
        // FIX: better name for tmp_rate?
        double tmp_rate = total * 8 / time;
        if (current_rate_ - tmp_rate > 10 && current_rate_ > 200) {
          state_ = GUESSING;
          current_rate_ = tmp_rate;
          return;
        }

        bool continue_moving = current_utility > previous_utility_ || change_intense_ == 1;
        if (continue_moving){
          change_intense_ += 1;
          tartger_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        }

        double change_amount =
            change_intense_ * GRANULARITY * current_rate_ * change_direction_;

        if (continue_moving){
          current_rate_ += change_amount;
        } else {
          state_ = GUESSING;
          current_rate_ -= change_amount;
        }
        previous_utility_ = current_utility;
      }
      return;
    default:
      return;
  }
}

void PCCUtility::GetBytesSum(std::vector<PacketInfo> packet_vector,
                                      double& total,
                                      double& lost) {
  for (std::vector<PacketInfo>::iterator it = packet_vector.begin();
      it != packet_vector.end(); ++it) {
    total += it->bytes;

    if (it->state == LOST){
      lost += it->bytes;
    }
  }
}

double PCCUtility::GetCurrentRate() const {
  return current_rate_;
}

UtilityState PCCUtility::GetCurrentState() const {
  return state_;
}

}  // namespace net
