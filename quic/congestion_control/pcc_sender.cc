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
    end_time(QuicTime::Zero()),
    end_transmission_time(QuicTime::Zero()),
    start_seq_num(-1),
    end_seq_num(-1){
}

PCCMonitor::~PCCMonitor(){
}

#ifdef DEBUG_
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
  
#ifdef DEBUG_
  if (!previous_timer_.IsInitialized()) {
    previous_timer_ = sent_time;
  }

  if ((sent_time - previous_timer_).ToSeconds()) {
    printf("|_ rtt %6ldus, ", rtt_stats_->smoothed_rtt().ToMicroseconds());
    printf("sent at rate %6.3f mbps, ", (double)send_bytes_*8 / 1024 / 1024);
    printf("throughput rate %6.3f mbps\n", (double)ack_bytes_*8 / 1024 / 1024);

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
    //if (diff.ToMicroseconds() > 0) {
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

void PCCSender::StartMonitor(QuicTime sent_time){
  current_monitor_ = (current_monitor_ + 1) % NUM_MONITOR;
  pcc_utility_.OnMonitorStart(current_monitor_);

  // calculate monitor interval and monitor end time
  double rand_factor = 0.1 + (double(rand() % 3) / 10);
  int64_t srtt = rtt_stats_->latest_rtt().ToMicroseconds();
  //int64_t srtt = rtt_stats_->smoothed_rtt().ToMicroseconds();
  if (srtt == 0) {srtt = rtt_stats_->initial_rtt_us();}
  int64_t rtt1000 = int64_t(12000000.0 / pcc_utility_.GetCurrentRate());
  if (rtt1000 < srtt) {srtt = rtt1000;}
  
  QuicTime::Delta monitor_interval =
      QuicTime::Delta::FromMicroseconds(srtt * (1.0 + rand_factor));
  // TODO: restrict maximum MI duration
  current_monitor_end_time_ = sent_time + monitor_interval;

  monitors_[current_monitor_].state = SENDING;
  monitors_[current_monitor_].tx_rate = pcc_utility_.GetCurrentRate();
  monitors_[current_monitor_].srtt = srtt;
  monitors_[current_monitor_].ertt = 0;
  monitors_[current_monitor_].start_time = sent_time;
  monitors_[current_monitor_].end_time = QuicTime::Zero();
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
#ifdef DEBUG_
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
    monitors_[monitor_num].ertt = rtt_stats_->latest_rtt().ToMicroseconds();
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
#ifndef DEBUG_
  // Maybe have PccUtility dump state.
  const PCCUtility &u = pcc_utility_;
  StrAppend(&msg, "[st=", u.state_, ",");
  StrAppend(&msg, "r=", u.current_rate_, ",");
  StrAppend(&msg, "pu=", u.previous_utility_, ",");
  StrAppend(&msg, "(gt=", u.guess_time_, ",");
  StrAppend(&msg, "nr=", u.num_recorded_, ",");
  StrAppend(&msg, "cg=", u.continuous_guess_count_, ")");
  StrAppend(&msg, "(dir=", u.change_direction_, ",");
  StrAppend(&msg, "ci=", u.change_intense_, ",");
  StrAppend(&msg, "cm=", (int)current_monitor_, ",");
  StrAppend(&msg, "tm=", (int)u.target_monitor_, ")] ");

  const PCCMonitor &m = monitors_[current_monitor_];
  StrAppend(&msg, "[ms=", m.state, ",");
  StrAppend(&msg, "tx(", m.start_time.ToDebuggingValue(), "-");
  StrAppend(&msg, ">", m.end_transmission_time.ToDebuggingValue(), "),");
  StrAppend(&msg, "sn(", m.start_seq_num, "-");
  StrAppend(&msg, ">", m.end_seq_num, ")]");
#endif
  return msg;
}

PCCUtility::PCCUtility()
  : state_(STARTING),
    current_rate_(2),
    waiting_rate_(2),
    probing_rate_(2),
    target_monitor_(1),
    current_monitor_early_end_(false),
    previous_utility_(0),
    loss_ignorance_count_(70000),
    guess_time_(0),
    num_recorded_(0),
    continuous_guess_count_(0),
    change_direction_(0),
    change_intense_(1) {
  }

void PCCUtility::OnMonitorStart(MonitorNumber current_monitor) {
  UtilityState old_state;
  do {
    old_state = state_;
    switch (state_) {
      case STARTING:
        if (current_monitor != target_monitor_) {
          current_rate_ = waiting_rate_;
        }
        break;
      case GUESSING:
        if (continuous_guess_count_ == MAX_COUNTINOUS_GUESS) {
          // current_rate_ *= (1 - GRANULARITY);
          continuous_guess_count_ = 0;
        }

        state_ = RECORDING;
        continuous_guess_count_++;

        for (int i = 0; i < NUMBER_OF_PROBE; i += 2) {
          int rand_dir = rand() % 2 * 2 - 1;

          guess_stat_bucket[i].rate = current_rate_
              + rand_dir * GRANULARITY * current_rate_;
          guess_stat_bucket[i + 1].rate = current_rate_
              - rand_dir * GRANULARITY * current_rate_;
        }

        for (int i = 0; i < NUMBER_OF_PROBE; i++) {
          guess_stat_bucket[i].monitor = (current_monitor + i) % NUM_MONITOR;
        }
        guess_time_ = 0;
        break;
      case RECORDING:
        if (guess_time_ != -1) {
          current_rate_ = guess_stat_bucket[guess_time_].rate;
          guess_time_++;

          if (guess_time_ == NUMBER_OF_PROBE) {
            guess_time_ = -1;
          }
        } else {
          current_rate_ = (guess_stat_bucket[0].rate < guess_stat_bucket[1].rate) ? 
                        guess_stat_bucket[0].rate : guess_stat_bucket[1].rate;
        }
        break;
      case MOVING:
        if(current_monitor != target_monitor_) {
          current_rate_ = waiting_rate_;
        }
        break;
     default:
        LOG(FATAL) << "unhandled switch.  old_state = "
                   << old_state << " and state = " << state_;
        break;
    }
  } while (old_state != state_);

#ifdef DEBUG_
  printf("S %2d | st=%d r=%6.3lf gt=%d/nr=%d/cg=%d ",
      current_monitor, state_, current_rate_,
      guess_time_, num_recorded_, continuous_guess_count_);
  printf("dir=%d/ci=%d/tm=%2d\n",
      change_direction_, change_intense_, (int)target_monitor_);
#endif
}

void PCCUtility::OnMonitorEnd(PCCMonitor pcc_monitor,
                              MonitorNumber current_monitor,
                              MonitorNumber end_monitor) {

  double total = 0;
  double loss = 0;
  GetBytesSum(pcc_monitor.packet_vector, total, loss);

  int64_t time = 
      (pcc_monitor.end_transmission_time - pcc_monitor.start_time).ToMicroseconds();

  double rtt_ratio = 1.0 * pcc_monitor.srtt / pcc_monitor.ertt;

  double current_utility = ((total - loss) / time *
    (1 - 1 / (1 + exp(-1000 * (loss / total - 0.05)))) *
    (1 - 1 / (1 + exp(-10 * (1 - rtt_ratio)))) - loss / time) * 1000;
#ifdef DEBUG_
  double current_utility_nonlatency = ((total - loss) / time *
    (1 - 1 / ( 1 + exp(-1000 * (loss / total - 0.05)))) * 0.5
    - loss / time) * 1000;
  double actual_tx_rate = total * 8 / time;
#endif

  double actual_bw = pcc_monitor.tx_rate * time * 1.0 /
                  (time + pcc_monitor.ertt - pcc_monitor.srtt);
  
  switch (state_) {
    case STARTING:
      {
      if (loss_ignorance_count_ > 0) {
        if (loss_ignorance_count_ >= loss) {
          loss_ignorance_count_ -= loss;
          loss = 0;
        } else {
          loss -= loss_ignorance_count_;
          loss_ignorance_count_ = 0;
        }
        
        if (rtt_ratio > 0.7) {
          current_utility = ((total - loss) / time *
                          (1 - 1 / (1 + exp(-1000 * (loss / total - 0.05)))) * 0.5
                          - loss / time) * 1000;
        }
      }
      
      if (end_monitor == target_monitor_) {
        if (current_utility > previous_utility_) {
          waiting_rate_ = probing_rate_;
          current_rate_ = probing_rate_ * 2;
          probing_rate_ = current_rate_;
          
          target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        } else {
          state_ = GUESSING;
          current_rate_ = actual_bw * (1 - GRANULARITY);
          probing_rate_ = current_rate_;
          if (waiting_rate_ > current_rate_) {
            waiting_rate_ = current_rate_;
          }
        }
        
        previous_utility_ = current_utility;
        current_monitor_early_end_ = true;
      }
      break;
      }
    case RECORDING:
      {
      // find corresponding monitor
      for (int i = 0; i < NUMBER_OF_PROBE; i++) {
        if (end_monitor == guess_stat_bucket[i].monitor) {
          num_recorded_++;
          guess_stat_bucket[i].utility = current_utility;
          guess_stat_bucket[i].total = total;
          guess_stat_bucket[i].loss = loss;
          guess_stat_bucket[i].srtt = pcc_monitor.srtt;
          guess_stat_bucket[i].ertt = pcc_monitor.ertt;
          guess_stat_bucket[i].bw = actual_bw;
          break;
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
        
        double sum_total = 0, sum_loss = 0, avg_loss_rate;
        double avg_ratio = 0;
        double sample_ratio = guess_stat_bucket[0].srtt /
                            guess_stat_bucket[NUMBER_OF_PROBE-1].ertt;
        double avg_bw = 0;
        for (int i = 0; i < NUMBER_OF_PROBE; i ++) {
          sum_total += guess_stat_bucket[i].total;
          sum_loss += guess_stat_bucket[i].loss;
          
          avg_ratio += (1.0 * guess_stat_bucket[i].srtt / guess_stat_bucket[i].ertt);
          avg_bw += guess_stat_bucket[i].bw;
        }
        avg_loss_rate = sum_loss / sum_total;
        avg_ratio /= NUMBER_OF_PROBE;
        avg_bw /= NUMBER_OF_PROBE;
        if (avg_ratio < 0.8 || sample_ratio < 0.66) {
          decision = -6;
        } else if (avg_loss_rate > 0.05) {
          decision = -2;
        }

        if (decision == -6) {
          current_rate_ = avg_bw * (1 - 1.5 * GRANULARITY);
          state_ = GUESSING;
        } else if (decision == 0) {
          current_rate_ = (guess_stat_bucket[0].rate + guess_stat_bucket[1].rate) / 2;
          state_ = GUESSING;
        } else {
          state_ = MOVING;
          change_direction_ = decision > 0 ? 1 : -1;
          change_intense_ = 1;
          
          double rate1 = guess_stat_bucket[2].rate;
          double rate2 = guess_stat_bucket[3].rate;
          double tmp_rate = (change_direction_*(rate1 - rate2) > 0) ? rate1 : rate2;
          
          double utility1 = guess_stat_bucket[2].utility;
          double utility2 = guess_stat_bucket[3].utility;
          previous_utility_ = (utility1 > utility2) ? utility1 : utility2;
          
          double change_amount = change_direction_* GRANULARITY * tmp_rate;
          if(change_amount > 0 && previous_utility_ < 0) {
            change_amount = 0.0;
          }
          current_rate_ = tmp_rate + change_amount;
          probing_rate_ = current_rate_;
          if(change_direction_ > 0) {
            waiting_rate_ = tmp_rate;
          } else {
            waiting_rate_ = current_rate_;
          }
          
          continuous_guess_count_ = 0;
          target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        }
        current_monitor_early_end_ = true;
      }
      break;
      }
    case MOVING:
      {
      if (end_monitor == target_monitor_) {
        if (rtt_ratio > 0.95 && rtt_ratio < 1.05) {
          current_utility = ((total - loss) / time *
              (1 - 1 / (1 + exp(-1000*(loss/total - 0.05)))) * 0.5 - loss/time) * 1000;
        }
      
        bool continue_moving = current_utility > previous_utility_;
        if (continue_moving) {
          if (change_direction_ < 0 || current_utility > 0) {
            change_intense_ += 1;
          }
          target_monitor_ = (current_monitor + 1) % NUM_MONITOR;
        }

        double change_amount =
            change_intense_ * GRANULARITY * probing_rate_ * change_direction_;
        if (current_utility < 0 && change_direction_ > 0 && continue_moving) {
          change_amount = 0.0;
        } else if (change_direction_ < 0 && loss == 0 && continue_moving) {
          change_amount = 0.0;
          continue_moving = false;
        } else if (change_amount > 0.1 * probing_rate_) {
          change_amount = 0.1 * probing_rate_;
        } else if (change_amount < -0.15 * probing_rate_) {
          change_amount = -0.15 * probing_rate_;
        }

        if (rtt_ratio < 0.7) {
          if (change_amount > 0) {
            current_rate_ = probing_rate_ - change_amount * 2;
          } else {
            current_rate_ = probing_rate_ + change_amount * 2;
          }
          if (current_rate_ < 0) {current_rate_ = 2.0;}
          if (actual_bw < current_rate_) {current_rate_ = actual_bw;}
          
          if (continue_moving && change_direction_ < 0) {
            probing_rate_ = waiting_rate_ = current_rate_;
          } else {
            state_ = GUESSING;
          }
        } else if (continue_moving) {
          current_rate_ = probing_rate_ + change_amount;
          if(change_direction_ > 0) {
            waiting_rate_ = probing_rate_;
          } else {
            waiting_rate_ = current_rate_;
          }
          probing_rate_ = current_rate_;
        } else {
          // FIXME: use which rate when entering guessing?
          current_rate_ = probing_rate_ - change_amount;
          state_ = GUESSING;
        }
        previous_utility_ = current_utility;
        current_monitor_early_end_ = true;
      }
      break;
      }
    case GUESSING:
      break;
    default:
      break;
  }

#ifdef DEBUG_  
  printf("E %2d | st=%d r=%6.3lf pr=%8.2lf gt=%d/nr=%d/cg=%d ",
      end_monitor, state_, current_rate_, previous_utility_,
      guess_time_, num_recorded_, continuous_guess_count_);
  printf("dir=%d/ci=%d/tm=%2d | %8.2lf/%9.2lf %.0lf/%.0lf %6ld/%6ld/%6ld ",
      change_direction_, change_intense_, (int)target_monitor_,
      current_utility, current_utility_nonlatency, total, loss,
      time, pcc_monitor.srtt, pcc_monitor.ertt);
  printf("%6.2lf/%6.2lf \n", actual_tx_rate, actual_bw);
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
