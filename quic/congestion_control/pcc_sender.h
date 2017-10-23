// PCC (Performance Oriented Congestion Control) algorithm

#ifndef NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_
#define NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_

#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

#include <vector>

#include "base/macros.h"
#include "gfe/quic/core/congestion_control/send_algorithm_interface.h"
#include "gfe/quic/core/quic_bandwidth.h"
#include "gfe/quic/core/quic_connection_stats.h"
#include "gfe/quic/core/quic_time.h"
#include "gfe/quic/core/quic_types.h"

namespace gfe_quic {

namespace test {
class PccSenderPeer;
}  // namespace test

class RttStats;

// PccSender implements the PCC congestion control algorithm. PccSender
// evaluates the benefits of different sending rates by comparing their
// utilities, and adjusts the sending rate towards the direction of
// higher utility.
class QUIC_EXPORT_PRIVATE PccSender
    : public SendAlgorithmInterface,
      public PccMonitorIntervalQueueDelegateInterface {
 public:
  // Sender's mode during a connection.
  enum SenderMode {
    // Initial phase of the connection. Sending rate gets doubled as
    // long as utility keeps increasing, and the sender enters
    // PROBING mode when utility decreases.
    STARTING,
    // Sender tries different sending rates to decide whether higher
    // or lower sending rate has greater utility. Sender enters
    // DECISION_MADE mode once a decision is made.
    PROBING,
    // Sender keeps increasing or decreasing sending rate until
    // utility decreases, then sender returns to PROBING mode.
    // TODO(tongmeng): a better name?
    DECISION_MADE
  };

  // Indicates whether sender should increase or decrease sending rate.
  enum RateChangeDirection { INCREASE, DECREASE };

  PccSender(const RttStats* rtt_stats,
            QuicPacketCount initial_congestion_window,
            QuicPacketCount max_congestion_window, QuicRandom* random);
  PccSender(const PccSender&) = delete;
  PccSender& operator=(const PccSender&) = delete;
  PccSender(PccSender&&) = delete;
  PccSender& operator=(PccSender&&) = delete;
  ~PccSender() override {}

  // Start implementation of SendAlgorithmInterface.
  bool InSlowStart() const override;
  bool InRecovery() const override;
  bool IsProbingForMoreBandwidth() const override;

  void SetFromConfig(const QuicConfig& config,
                     Perspective perspective) override {}

  void AdjustNetworkParameters(QuicBandwidth bandwidth,
                               QuicTime::Delta rtt) override {}
  void SetNumEmulatedConnections(int num_connections) override {}
  void OnCongestionEvent(bool rtt_updated,
                         QuicByteCount bytes_in_flight,
                         QuicTime event_time,
                         const AckedPacketVector& acked_packets,
                         const LostPacketVector& lost_packets) override;
  void OnPacketSent(QuicTime sent_time,
                    QuicByteCount bytes_in_flight,
                    QuicPacketNumber packet_number,
                    QuicByteCount bytes,
                    HasRetransmittableData is_retransmittable) override;
  void OnRetransmissionTimeout(bool packets_retransmitted) override {}
  void OnConnectionMigration() override {}
  bool CanSend(QuicByteCount bytes_in_flight) override;
  QuicBandwidth PacingRate(QuicByteCount bytes_in_flight) const override;
  QuicBandwidth BandwidthEstimate() const override;
  QuicByteCount GetCongestionWindow() const override;
  QuicByteCount GetSlowStartThreshold() const override;
  CongestionControlType GetCongestionControlType() const override;
  string GetDebugState() const override;
  void OnApplicationLimited(QuicByteCount bytes_in_flight) override {}
  // End implementation of SendAlgorithmInterface.

  // Implementation of PccMonitorIntervalQueueDelegate.
  // Called when all useful intervals' utilities are available,
  // so the sender can make a decision.
  void OnUtilityAvailable(
      const std::vector<UtilityInfo>& utility_info) override;

 private:
  friend class test::PccSenderPeer;
  // Returns true if next created monitor interval is useful,
  // i.e., its utility will be used when a decision can be made.
  bool CreateUsefulInterval() const;
  // Maybe set sending_rate_ for next created monitor interval.
  void MaybeSetSendingRate();

  // Returns true if the sender can enter DECISION_MADE from PROBING mode.
  bool CanMakeDecision(const std::vector<UtilityInfo>& utility_info) const;
  // Set the sending rate to the central rate used in PROBING mode.
  void EnterProbing();
  // Set the sending rate when entering DECISION_MADE from PROBING mode.
  void EnterDecisionMade();

  // Current mode of PccSender.
  SenderMode mode_;
  // Sending rate for the next monitor intervals.
  QuicBandwidth sending_rate_;
  // Most recent utility used when making the last rate change decision.
  float latest_utility_;
  // Duration of the current monitor interval.
  QuicTime::Delta monitor_duration_;
  // Current direction of rate changes.
  RateChangeDirection direction_;
  // Number of rounds sender remains in current mode.
  size_t rounds_;
  // Queue of monitor intervals with pending utilities.
  PccMonitorIntervalQueue interval_queue_;

  // Maximum congestion window in bytes, used to cap sending rate.
  QuicByteCount max_cwnd_bytes_;

  const RttStats* rtt_stats_;
  QuicRandom* random_;
};

}  // namespace gfe_quic

#endif
