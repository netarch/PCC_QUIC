/*
 * pcc_sender.cc
 *
 *  Created on: March 28, 2016
 *      Authors:
 *               Xuefeng Zhu (zhuxuefeng1994@126.com)
 *               Mo Dong (modong2@illinois.edu)
 *               Tong Meng (tongm2@illinois.edu)
 */
#include <stdio.h>

#include "net/quic/core/congestion_control/pcc_sender.h"
#include "net/quic/core/congestion_control/rtt_stats.h"
#include "net/quic/core/quic_time.h"
#include "base/time/time.h"

namespace net {

PCCMonitor::PCCMonitor()
  : start_time(QuicTime::Zero()),
    end_transmission_time(QuicTime::Zero()),
    start_seq_num(-1),
    end_seq_num(-1) {
}

PCCMonitor::~PCCMonitor() {
}

#ifdef TRACE_
PCCSender::PCCSender(const RttStats* rtt_stats)
  : current_monitor_(-1),
    current_monitor_end_time_(QuicTime::Zero()),
    rtt_stats_(rtt_stats),
    ideal_next_packet_send_time_(QuicTime::Zero()),
    previous_timer_(QuicTime::Zero()),
    send_bytes_(0),
    ack_bytes_(0) {
}
#else
PCCSender::PCCSender(const RttStats* rtt_stats)
  : current_monitor_(-1),
    current_monitor_end_time_(QuicTime::Zero()),
    rtt_stats_(rtt_stats),
    ideal_next_packet_send_time_(QuicTime::Zero()) {
}
#endif


PCCSender::~PCCSender() {}

bool PCCSender::OnPacketSent(
    QuicTime sent_time,
    QuicByteCount bytes_in_flight,
    QuicPacketNumber packet_number,
    QuicByteCount bytes,
    HasRetransmittableData has_retransmittable_data) {
  
#ifdef TRACE_
  if (!previous_timer_.IsInitialized()) {
    previous_timer_ = sent_time;
  }

  if ((sent_time - previous_timer_).ToSeconds()) {
    printf("|_ rtt %6ld/%6ld, %6.3f/%6.3f mbps\n",
        rtt_stats_->latest_rtt().ToMicroseconds(),
        rtt_stats_->smoothed_rtt().ToMicroseconds(),
        (double)send_bytes_*8 / 1024 / 1024,
        (double)ack_bytes_*8 / 1024 / 1024);

    previous_timer_ = sent_time;
    send_bytes_ = 0;
    ack_bytes_ = 0;
  } else {
    send_bytes_ += bytes;
  }
#endif

  // TODO : case for retransmission
  if (!current_monitor_end_time_.IsInitialized()) {
    StartMonitor(sent_time);
    monitors_[current_monitor_].start_seq_num = packet_number;
    ideal_next_packet_send_time_ = sent_time;
  } else {
    QuicTime::Delta diff = sent_time - current_monitor_end_time_;
    if (diff.ToMicroseconds() > 0 || pcc_utility_.GetEarlyEndFlag()) {
      monitors_[current_monitor_].state = WAITING;
      monitors_[current_monitor_].end_transmission_time = sent_time;
      monitors_[current_monitor_].end_seq_num = packet_number;
      current_monitor_end_time_ = QuicTime::Zero();
      pcc_utility_.SetEarlyEndFlag(false);
    }
  }
  PacketInfo packet_info;
  packet_info.sent_time = sent_time;
  packet_info.bytes = bytes;
  monitors_[current_monitor_].packet_vector.push_back(packet_info);
  QuicTime::Delta delay = QuicTime::Delta::FromMicroseconds(
      bytes * 8 * base::Time::kMicrosecondsPerSecond /
    pcc_utility_.GetCurrentRate() / 1024 / 1024);
  ideal_next_packet_send_time_ = ideal_next_packet_send_time_ + delay;
  return true;
}

void PCCSender::StartMonitor(QuicTime sent_time) {
  current_monitor_ = (current_monitor_ + 1) % NUM_MONITOR;
  pcc_utility_.OnMonitorStart(current_monitor_);

  // calculate monitor interval and monitor end time
  double rand_factor = 0.1 + (double(rand() % 3) / 10);
  int64_t srtt = rtt_stats_->smoothed_rtt().ToMicroseconds();
  if (srtt == 0) {
    srtt = rtt_stats_->initial_rtt_us();
  }
  QuicTime::Delta monitor_interval =
      QuicTime::Delta::FromMicroseconds(srtt * (1.0 + rand_factor));
  current_monitor_end_time_ = sent_time + monitor_interval;

  monitors_[current_monitor_].state = SENDING;
  monitors_[current_monitor_].srtt = srtt;
  monitors_[current_monitor_].ertt = 0;
  monitors_[current_monitor_].start_time = sent_time;
  monitors_[current_monitor_].end_transmission_time = QuicTime::Zero();
  monitors_[current_monitor_].end_seq_num = -1;
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
#ifdef TRACE_
    ack_bytes_ += monitors_[monitor_num].packet_vector[pos].bytes;
#endif

    if (it->first == monitors_[monitor_num].end_seq_num) {
      EndMonitor(monitor_num);
    }
  }
}

void PCCSender::EndMonitor(MonitorNumber monitor_num) {
  if (monitors_[monitor_num].state == WAITING){
    monitors_[monitor_num].state = FINISHED;
    monitors_[monitor_num].ertt = rtt_stats_->smoothed_rtt().ToMicroseconds();
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

#ifdef DEBIG_
  printf("Monitor is not found\n");
#endif
  return -1;
}

QuicTime::Delta PCCSender::TimeUntilSend(
      QuicTime now,
      QuicByteCount bytes_in_flight) const {
    // If the next send time is within the alarm granularity, send immediately.
  if (ideal_next_packet_send_time_ > now + alarm_granularity_) {
    QuicTime::Delta packet_delay = ideal_next_packet_send_time_ - now;
    DVLOG(1) << "Delaying packet: "
             << packet_delay.ToMicroseconds();
    return packet_delay;
  }

  DVLOG(1) << "Sending packet now";
  return QuicTime::Delta::Zero();
}

QuicBandwidth PCCSender::PacingRate(QuicByteCount bytes_in_flight) const {
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

std::string PCCSender::GetDebugState() const {
  std::string msg;
  
  const PCCUtility &u = pcc_utility_;
  StrAppend(&msg, "st=", u.state_, ",");
  StrAppend(&msg, "r=", u.current_rate_, ",");
  StrAppend(&msg, "pu=", u.previous_utility_, ",");
  StrAppend(&msg, "cm=", (int)current_monitor_, ",");
  StrAppend(&msg, "tm=", (int)u.target_monitor_, ", ");

  const PCCMonitor &m = monitors_[current_monitor_];
  StrAppend(&msg, "[ms=", m.state, ",");
  StrAppend(&msg, "tx(", m.start_time.ToDebuggingValue(), "-");
  StrAppend(&msg, ">", m.end_transmission_time.ToDebuggingValue(), "),");
  StrAppend(&msg, "rtt(", m.srtt, "-");
  StrAppend(&msg, ">", m.ertt, "),");
  StrAppend(&msg, "sn(", m.start_seq_num, "-");
  StrAppend(&msg, ">", m.end_seq_num, ")]");
  
  return msg;
}

PCCUtility::PCCUtility()
  : state_(STARTING),
    current_rate_(MIN_RATE),
    previous_utility_(0),
    current_monitor_early_end_(false),
    target_monitor_(-1),
    waiting_rate_(MIN_RATE),
    probing_rate_(MIN_RATE),
    decreasing_intense_(1) {
  }

void PCCUtility::OnMonitorStart(MonitorNumber current_monitor) {
  if (current_monitor != target_monitor_) {
    current_rate_ = waiting_rate_;
  }
#ifdef DEBUG_
  printf("S %2d | st=%d r=%6.3lf ", current_monitor, state_, current_rate_);
  printf("tm=%2d di=%2d\n", (int)target_monitor_, decreasing_intense_);
#endif
}

void PCCUtility::OnMonitorEnd(PCCMonitor pcc_monitor,
                              MonitorNumber current_monitor,
                              MonitorNumber end_monitor) {

  double total = 0;
  double lossAll = 0;
  GetBytesSum(pcc_monitor.packet_vector, total, lossAll);
  
  int64_t time = 
      (pcc_monitor.end_transmission_time - pcc_monitor.start_time).ToMicroseconds();
  double actual_tx_rate = total * 8 / time;

  double rtt_ratio = 1.0 * pcc_monitor.srtt / pcc_monitor.ertt;
  if (rtt_ratio > 0.9 && rtt_ratio < 1.1) {rtt_ratio = 1;}

#ifdef DEBUG_
  int64_t previous_rtt = pcc_monitor.srtt;
  int64_t current_rtt = pcc_monitor.ertt;
  double old_utility = previous_utility_;
#endif

  total -= 1400;
  double loss = lossAll - 1400;
  if(loss < 0) {loss += 1400;}
  double loss_ratio = loss / total;
  
  double current_utility, target_utility;
  if (state_ == STARTING) {
    current_utility = ( (total - loss) / time *
      (1 - 1 / (1 + exp(-1000 * (loss_ratio - 0.05)))) * 0.5
      - 1 * loss / time) * 1000;
  } else {
    current_utility = ( (total - loss) / time *
      (1 - 1 / (1 + exp(-1000 * (loss_ratio - 0.05)))) *
      (1 - 1 / (1 + exp(-10 * (1 - rtt_ratio)))) - 1 * loss / time) * 1000;
  }
  target_utility = (total / time * (1 - 1 / (1 + exp(50))) * 0.5) * 1000;

  if (loss_ratio > 0.2) {
    state_ = DMOVING;
    current_rate_ /= 2.0;
    if (current_rate_ < MIN_RATE) {current_rate_ = MIN_RATE;}
    waiting_rate_ = probing_rate_ = current_rate_;
    
    target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
    current_monitor_early_end_ = true;
  } else if(current_rate_ > MIN_RATE + 0.5 && end_monitor == target_monitor_ &&
      actual_tx_rate < probing_rate_ * 0.8) {
    state_ = DMOVING;
    current_rate_ = actual_tx_rate;
    if (current_rate_ < MIN_RATE) {current_rate_ = MIN_RATE;}
    waiting_rate_ = probing_rate_ = current_rate_;
    
    previous_utility_ = current_utility;
    target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
    current_monitor_early_end_ = true;
  } else {
    switch (state_) {
      case STARTING:
        {
        if (current_rate_ - MIN_RATE < 0.001) {
          if (actual_tx_rate > current_rate_ * 0.9) {
            waiting_rate_ = current_rate_ * 1.1;
            current_rate_ = current_rate_ * 2;
            probing_rate_ = current_rate_;
            
            target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
            previous_utility_ = current_utility;
            current_monitor_early_end_ = true;
          }
        } else {
          if (end_monitor != target_monitor_) {break;}
          
          if (current_utility > previous_utility_) {
            waiting_rate_ = probing_rate_;
            current_rate_ = probing_rate_ * 2;
            probing_rate_ = current_rate_;
            
            previous_utility_ = current_utility;
          } else {
            state_ = DMOVING;
            current_rate_ = waiting_rate_ * (1 + GRANULARITY);
            probing_rate_ = current_rate_;
          }
          
          target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
          current_monitor_early_end_ = true;
        }
        break;
        }
      case UMOVING:
        if (end_monitor != target_monitor_) {break;}

        if (current_utility > target_utility * (1-GRANULARITY-0.01)) {
          waiting_rate_ = probing_rate_;
          current_rate_ = probing_rate_ * (1 + GRANULARITY);
          probing_rate_ = current_rate_;
        } else if (current_utility > target_utility * 0.5) {
          if (current_utility > previous_utility_) {
            waiting_rate_ = probing_rate_;
            current_rate_ = probing_rate_ * (1 + GRANULARITY);
            probing_rate_ = current_rate_;
          } else {
            state_ = DMOVING;
            current_rate_ = probing_rate_ * (1 - 3 * GRANULARITY);
            waiting_rate_ = probing_rate_ = current_rate_;
            decreasing_intense_ = 1;
          }
        } else {
          state_ = DMOVING;
          if (current_utility > target_utility * 0.2) {
            current_rate_ = probing_rate_ * (1 - 3 * GRANULARITY);
          } else {
            current_rate_ = probing_rate_ * (1 - 4 * GRANULARITY);
            if (current_rate_ < MIN_RATE) {current_rate_ = MIN_RATE;}
          }
          waiting_rate_ = probing_rate_ = current_rate_;
          decreasing_intense_ = 1;
        }
        
        target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        current_monitor_early_end_ = true;
        if (current_utility > target_utility) {
          previous_utility_ = target_utility;
        } else {
          previous_utility_ = current_utility;
        }
        break;
      case DMOVING:
        if (end_monitor != target_monitor_) {break;}
        
        if (current_utility > target_utility * 1.4) {
          // seems rtt decreases, stay on the same rate and see what happens
          current_rate_ = probing_rate_;
        } /*else if (current_utility > target_utility * (1-GRANULARITY-0.01)) {
          state_ = UMOVING;
          waiting_rate_ = probing_rate_;
          current_rate_ = probing_rate_ * (1 + GRANULARITY);
          probing_rate_ = current_rate_;
        }*/ else if (current_utility > target_utility * 0.5) {
          if (current_utility > previous_utility_ || previous_utility_ < 0) {
            current_rate_ = probing_rate_ * (1 - GRANULARITY);
            waiting_rate_ = probing_rate_ = current_rate_;
          } else {
            state_ = UMOVING;
            waiting_rate_ = probing_rate_;
            current_rate_ = probing_rate_ * (1 + (decreasing_intense_*1.0 / 2.0) * GRANULARITY);
            probing_rate_ = current_rate_;
          }
        } else {
          if (current_utility > target_utility * 0.2) {
            current_rate_ = probing_rate_ * (1 - decreasing_intense_ * GRANULARITY);
          } else {
            current_rate_ = probing_rate_ * (1 - 2.5 * decreasing_intense_ * GRANULARITY);
          }
          if (current_rate_ < MIN_RATE) {current_rate_ = MIN_RATE;}
          waiting_rate_ = probing_rate_ = current_rate_;
          decreasing_intense_ += 1;
        }
        
        target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        current_monitor_early_end_ = true;
        if (current_utility > target_utility) {
          previous_utility_ = target_utility;
        } else {
          previous_utility_ = current_utility;
        }
        break;
      default:
        break;
    }
  }

#ifdef DEBUG_
  printf("E %2d | st=%d r=%6.3lf ", end_monitor, state_, current_rate_);
  printf("tm=%2d di=%2d | ", (int)target_monitor_, decreasing_intense_);
  printf("%6ld->%6ld(%4.2lf) %6ld %8.2lf/%8.2lf(%4.2lf) %8.2f ",
      previous_rtt, current_rtt, rtt_ratio, time,
      current_utility, target_utility,
      (current_utility / target_utility), old_utility);
  printf("%7.0lf(%5.2lf) (L)%7.0lf(%6.3lf)\n",
      (total+1400), actual_tx_rate, lossAll, loss_ratio);
#endif
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

bool PCCUtility::GetEarlyEndFlag() const {
  return current_monitor_early_end_;
}

void PCCUtility::SetEarlyEndFlag(bool newFlag) {
  current_monitor_early_end_ = newFlag;
}

UtilityState PCCUtility::GetCurrentState() const {
  return state_;
}

}  // namespace net
