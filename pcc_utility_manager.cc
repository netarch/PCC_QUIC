#include "third_party/pcc_quic/pcc_utility_manager.h"

#include "third_party/quic/core/congestion_control/rtt_stats.h"

namespace quic {

namespace {
// Tolerance of loss rate by utility function.
const float kLossTolerance = 0.05f;
// Coefficeint of the loss rate term in utility function.
const float kLossCoefficient = -1000.0f;
// Coefficient of RTT term in utility function.
const float kRTTCoefficient = -200.0f;

}  // namespace

float CalculateUtility(const MonitorInterval* interval) {
  // The caller should guarantee utility of this interval is available.
  QUIC_BUG_IF(interval->first_packet_sent_time ==
              interval->last_packet_sent_time);

  // Add the transfer time of the last packet in the monitor interval when
  // calculating monitor interval duration.
  float interval_duration =
      static_cast<float>((interval->last_packet_sent_time -
                              interval->first_packet_sent_time +
                              interval->sending_rate
                                  .TransferTime(kMaxPacketSize))
                             .ToMicroseconds());

  float rtt_ratio =
      static_cast<float>(interval->rtt_on_monitor_start.ToMicroseconds()) /
      static_cast<float>(interval->rtt_on_monitor_end.ToMicroseconds());
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

  return (bytes_acked / interval_duration * loss_penalty * latency_penalty -
      bytes_lost / interval_duration) * 1000.0;
}

}  // namespace quic
