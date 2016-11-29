/*
 * pcc_sender.h
 *
 *  Created on: March 28, 2016
 *      Authors:
 *               Xuefeng Zhu (zhuxuefeng1994@126.com)
 *               Mo Dong (modong2@illinois.edu)
 *               Tong Meng (tongm2@illinois.edu)
 */
#ifndef NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_
#define NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_

# include <vector>

//#include "base/basictypes.h"
#include "base/compiler_specific.h"
#include "net/base/net_export.h"
#include "net/quic/core/congestion_control/send_algorithm_interface.h"
#include "net/quic/core/quic_bandwidth.h"
#include "net/quic/core/quic_connection_stats.h"
#include "net/quic/core/quic_protocol.h"
#include "net/quic/core/quic_time.h"

//#define DEBUG_
//#define TRACE_

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
  STARTING = 0,
  UMOVING,
  DMOVING
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
  int64_t srtt;
  int64_t ertt;

  // time statics
  QuicTime start_time;
  //QuicTime end_time;
  QuicTime end_transmission_time;

  // packet statics
  QuicPacketNumber start_seq_num;
  QuicPacketNumber end_seq_num;
  std::vector<PacketInfo> packet_vector;

  PCCMonitor();
  ~PCCMonitor();
};

const int NUM_MONITOR = 100;
const double GRANULARITY = 0.05;
const double MIN_RATE = 4;

class PCCUtility {
 public:
  PCCUtility();
  // Callback function when monitor starts
  void OnMonitorStart(MonitorNumber current_monitor);

  // Callback function when monitor ends
  void OnMonitorEnd(PCCMonitor pcc_monitor,
                    MonitorNumber current_monitor,
                    MonitorNumber end_monitor);

  double GetCurrentRate() const;

  UtilityState GetCurrentState() const;
  bool GetEarlyEndFlag() const;
  void SetEarlyEndFlag(bool newFlag);

 private:
  UtilityState state_;

  double current_rate_;
  double previous_utility_;
  
  bool current_monitor_early_end_;

  MonitorNumber target_monitor_;
  double waiting_rate_;
  double probing_rate_;
  
  int decreasing_intense_;

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
                     Perspective perspective) override {};
  void ResumeConnectionState(
      const CachedNetworkParameters& cached_network_params,
      bool max_bandwidth_resumption) override {};
  void SetNumEmulatedConnections(int num_connections) override {};
  void OnCongestionEvent(bool rtt_updated,
                         QuicByteCount bytes_in_flight,
                         const CongestionVector& acked_packets,
                         const CongestionVector& lost_packets) override;
  bool OnPacketSent(QuicTime sent_time,
                    QuicByteCount bytes_in_flight,
                    QuicPacketNumber packet_number,
                    QuicByteCount bytes,
                    HasRetransmittableData is_retransmittable) override;
  void OnRetransmissionTimeout(bool packets_retransmitted) override {};
  void OnConnectionMigration() override {}
  QuicTime::Delta TimeUntilSend(
      QuicTime now,
      QuicByteCount bytes_in_flight) const override;
  QuicBandwidth PacingRate(QuicByteCount bytes_in_flight) const override;
  QuicBandwidth BandwidthEstimate() const override;
  QuicTime::Delta RetransmissionDelay() const override;
  QuicByteCount GetCongestionWindow() const override;
  bool InSlowStart() const override;
  bool InRecovery() const override;
  QuicByteCount GetSlowStartThreshold() const override;
  CongestionControlType GetCongestionControlType() const override;
  
  std::string GetDebugState() const override;
  
  void OnApplicationLimited(QuicByteCount bytes_in_flight) override {};
  
  // End implementation of SendAlgorithmInterface.

 private:
  const QuicTime::Delta alarm_granularity_ =
      QuicTime::Delta::FromMicroseconds(1);

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
  
#ifdef TRACE_
  // logging utility
  QuicTime previous_timer_;
  QuicByteCount send_bytes_;
  QuicByteCount ack_bytes_;
#endif
  
  DISALLOW_COPY_AND_ASSIGN(PCCSender);
};

}  // namespace net

#endif
