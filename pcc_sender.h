// PCC (Performance Oriented Congestion Control) algorithm

#ifndef NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_
#define NET_QUIC_CORE_CONGESTION_CONTROL_PCC_SENDER_H_

#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

#include <vector>

#include "base/macros.h"
#include "third_party/quic/core/congestion_control/bandwidth_sampler.h"
#include "third_party/quic/core/congestion_control/send_algorithm_interface.h"
#include "third_party/quic/core/congestion_control/windowed_filter.h"
#include "third_party/quic/core/quic_bandwidth.h"
#include "third_party/quic/core/quic_connection_stats.h"
#include "third_party/quic/core/quic_time.h"
#include "third_party/quic/core/quic_types.h"
#include "third_party/quic/core/quic_unacked_packet_map.h"

namespace quic {

namespace test {
class PccSenderPeer;
}  // namespace test

class RttStats;

// UtilityInfo is used to store <sending_rate, utility> pairs
struct UtilityInfo {
  UtilityInfo();
  UtilityInfo(QuicBandwidth rate, float utility);
  ~UtilityInfo() {}

  QuicBandwidth sending_rate;
  float utility;
};

typedef uint64_t QuicRoundTripCount;

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

  // Debug state to be exported for purpose of troubleshoot.
  struct DebugState {
    explicit DebugState(const PccSender& sender);
    DebugState(const DebugState& state) = default;

    SenderMode mode;
    QuicBandwidth sending_rate;
    QuicTime::Delta latest_rtt;
    QuicTime::Delta smoothed_rtt;
    QuicTime::Delta rtt_dev;
    bool is_useful;
    QuicTime first_packet_sent_time;
    QuicTime last_packet_sent_time;
    QuicPacketNumber first_packet_number;
    QuicPacketNumber last_packet_number;
    QuicByteCount bytes_sent;
    QuicByteCount bytes_acked;
    QuicByteCount bytes_lost;
    QuicTime::Delta rtt_on_monitor_start;
    QuicTime::Delta rtt_on_monitor_end;
    float latest_utility;
    QuicBandwidth bandwidth;
  };

  PccSender(const RttStats* rtt_stats,
            const QuicUnackedPacketMap* unacked_packets,
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
  bool ShouldSendProbingPacket() const override;

  void SetFromConfig(const QuicConfig& config,
                     Perspective perspective) override {}

  void SetInitialCongestionWindowInPackets(QuicPacketCount packets) override {}

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
  QuicString GetDebugState() const override;
  void OnApplicationLimited(QuicByteCount bytes_in_flight) override;
  // End implementation of SendAlgorithmInterface.

  // Implementation of PccMonitorIntervalQueueDelegate.
  // Called when all useful intervals' utilities are available,
  // so the sender can make a decision.
  void OnUtilityAvailable(
      const std::vector<const MonitorInterval *>& useful_intervals) override;

  // Generate PCC DebugState.
  DebugState ExportDebugState() const;

 private:
  friend class test::PccSenderPeer;
  typedef WindowedFilter<QuicBandwidth,
                         MaxFilter<QuicBandwidth>,
                         QuicRoundTripCount,
                         QuicRoundTripCount>
      MaxBandwidthFilter;

  // Returns true if next created monitor interval is useful,
  // i.e., its utility will be used when a decision can be made.
  bool CreateUsefulInterval() const;
  // Returns the sending rate for non-useful monitor interval.
  QuicBandwidth GetSendingRateForNonUsefulInterval() const;
  // Maybe set sending_rate_ for next created monitor interval.
  void MaybeSetSendingRate();
  // Returns the max RTT fluctuation tolerance according to sender mode.
  float GetMaxRttFluctuationTolerance() const;

  // Set sending rate to central probing rate for the coming round of PROBING.
  void RestoreCentralSendingRate();
  // Returns true if the sender can enter DECISION_MADE from PROBING mode.
  bool CanMakeDecision(const std::vector<UtilityInfo>& utility_info) const;
  // Set the sending rate to the central rate used in PROBING mode.
  void EnterProbing();
  // Set the sending rate when entering DECISION_MADE from PROBING mode.
  void EnterDecisionMade();

  // Returns true if the RTT inflation is larger than the tolerance.
  bool CheckForRttInflation();

  // Update the bandwidth sampler when OnCongestionEvent is called.
  void UpdateBandwidthSampler(QuicTime event_time,
                              const AckedPacketVector& acked_packets,
                              const LostPacketVector& lost_packets);

  // Current mode of PccSender.
  SenderMode mode_;
  // Sending rate for the next monitor intervals.
  QuicBandwidth sending_rate_;
  // Initialized to be false, and set to true after receiving the first ACK.
  bool has_seen_valid_rtt_;
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

  // Smoothed RTT before consecutive inflated RTTs happen.
  QuicTime::Delta rtt_on_inflation_start_;

  // Maximum congestion window in bytes, used to cap sending rate.
  QuicByteCount max_cwnd_bytes_;

  const RttStats* rtt_stats_;
  const QuicUnackedPacketMap* unacked_packets_;
  QuicRandom* random_;

  // Bandwidth sample provides the bandwidth measurement that is used when
  // exiting STARTING phase upon early termination.
  BandwidthSampler sampler_;
  // Filter that tracks maximum bandwidth over multiple recent round trips.
  MaxBandwidthFilter max_bandwidth_;
  // Packet number for the most recently sent packet.
  QuicPacketNumber last_sent_packet_;
  // Largest packet number that is sent in current round trips.
  QuicPacketNumber current_round_trip_end_;
  // Number of round trips since connection start.
  QuicRoundTripCount round_trip_count_;
  // Latched value of FLAGS_exit_starting_based_on_sampled_bandwidth.
  const bool exit_starting_based_on_sampled_bandwidth_;
};

// Overload operator for purpose of PCC DebugState printing
QUIC_EXPORT_PRIVATE std::ostream& operator<<(
    std::ostream& os,
    const PccSender::DebugState& state);

}  // namespace quic

#endif
