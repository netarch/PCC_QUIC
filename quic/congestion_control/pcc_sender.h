/*
 * pcc_sender.h
 *
 *  Created on: March 28, 2016
 *      Authors:
 *               Xuefeng Zhu (zhuxuefeng1994@126.com)
 *               Mo Dong (mdong@illinois.edu)
 */
#ifndef NET_QUIC_CONGESTION_CONTROL_PCC_SENDER_H_
#define NET_QUIC_CONGESTION_CONTROL_PCC_SENDER_H_

# include <vector>

#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "net/base/net_export.h"
#include "net/quic/congestion_control/send_algorithm_interface.h"
#include "net/quic/quic_bandwidth.h"
#include "net/quic/quic_connection_stats.h"
#include "net/quic/quic_protocol.h"
#include "net/quic/quic_time.h"

namespace net {
typedef int MonitorNumber;

enum MonitorState {
  SENDING,
  WAITING,
  FINISHED
};

enum PacketState {
  UNACK,
  ACK,
  LOST
};

enum UtilityState {
  STARTING,
  GUESSING,
  RECORDING,
  MOVING
};

struct PacketInfo {
  QuicTime sent_time;
  QuicByteCount bytes;
  PacketState state;

  PacketInfo()
  : sent_time(QuicTime::Zero()),
    bytes(0),
    state(UNACK){

    }
};

struct PCCMonitor {
  MonitorState state;
  int64 srtt;

  // time statics
  QuicTime start_time;
  QuicTime end_time;
  QuicTime end_transmission_time;

  // packet statics
  QuicPacketNumber start_seq_num;
  QuicPacketNumber end_seq_num;
  std::vector<PacketInfo> packet_vector;

  PCCMonitor();
  ~PCCMonitor();
};

struct GuessStat {
  MonitorNumber monitor;
  double rate;
  double utility;
};

const int NUM_MONITOR = 100;
const int NUMBER_OF_PROBE = 4;
const int MAX_COUNTINOUS_GUESS = 5;
const double GRANULARITY = 0.05;

class PCCUtility {
 public:
  PCCUtility();
  // Callback function when monitor starts
  void OnMonitorStart(MonitorNumber current_monitor);

  // Callback function when monitor ends
  void OnMonitorEnd(PCCMonitor pcc_monitor,
                    MonitorNumber current_monitor,
                    MonitorNumber end_monitor);

  double GetCurrentRate();

  UtilityState GetCurrentState();

 private:
  UtilityState state_;

  double current_rate_;
  double previous_utility_;
  double previous_rtt_;

  double start_rate_array[NUM_MONITOR];
  MonitorNumber previous_monitor_;

  // variables used for guess phase
  int num_recorded_;
  int guess_time_;
  int continous_guess_count_;
  GuessStat guess_stat_bucket[NUMBER_OF_PROBE];


  // variables used for moving phase
  MonitorNumber tartger_monitor_;

  int change_direction_;
  int change_intense_;

  void GetBytesSum(std::vector<PacketInfo> packet_vector,
                                      double& total,
                                      double& lost);
};

class RttStats;

class NET_EXPORT_PRIVATE PCCSender : public SendAlgorithmInterface {
 public:
  PCCSender(const RttStats* rtt_stats);
  ~PCCSender() override;

  // SendAlgorithmInterface methods.
  void SetFromConfig(const QuicConfig& config,
                     Perspective perspective) override;
  void ResumeConnectionState(
      const CachedNetworkParameters& cached_network_params,
      bool max_bandwidth_resumption) override;
  void SetNumEmulatedConnections(int num_connections) override;
  void SetMaxCongestionWindow(QuicByteCount max_congestion_window) override;
  void OnCongestionEvent(bool rtt_updated,
                         QuicByteCount bytes_in_flight,
                         const CongestionVector& acked_packets,
                         const CongestionVector& lost_packets) override;
  bool OnPacketSent(QuicTime sent_time,
                    QuicByteCount bytes_in_flight,
                    QuicPacketNumber packet_number,
                    QuicByteCount bytes,
                    HasRetransmittableData is_retransmittable) override;
  void OnRetransmissionTimeout(bool packets_retransmitted) override;
  void OnConnectionMigration() override {}
  QuicTime::Delta TimeUntilSend(
      QuicTime now,
      QuicByteCount bytes_in_flight,
      HasRetransmittableData has_retransmittable_data) const override;
  QuicBandwidth PacingRate() const override;
  QuicBandwidth BandwidthEstimate() const override;
  QuicTime::Delta RetransmissionDelay() const override;
  QuicByteCount GetCongestionWindow() const override;
  bool InSlowStart() const override;
  bool InRecovery() const override;
  QuicByteCount GetSlowStartThreshold() const override;
  CongestionControlType GetCongestionControlType() const override;
  // End implementation of SendAlgorithmInterface.

 private:
  const QuicTime::Delta alarm_granularity_ = QuicTime::Delta::FromMicroseconds(1);

  // PCC monitor variable
  MonitorNumber current_monitor_;
  QuicTime current_monitor_end_time_;

  PCCMonitor monitors_[NUM_MONITOR];
  PCCUtility pcc_utility_;
  const RttStats* rtt_stats_;

  QuicTime ideal_next_packet_send_time_;

  // private PCC functions
  // Start a new monitor
  void StartMonitor(QuicTime sent_time);
  // End previous monitor
  void EndMonitor(MonitorNumber monitor_num);
  // Get the monitor corresponding to the sequence number
  MonitorNumber GetMonitor(QuicPacketNumber sequence_number);

  // logging utility
  QuicTime previous_timer_;
  QuicByteCount send_bytes_;
  QuicByteCount ack_bytes_;
  DISALLOW_COPY_AND_ASSIGN(PCCSender);
};

}  // namespace net

#endif
