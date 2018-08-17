#ifndef THIRD_PARTY_PCC_QUIC_PCC_UTILITY_MANAGER_H_
#define THIRD_PARTY_PCC_QUIC_PCC_UTILITY_MANAGER_H_

#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

namespace quic {

// Calculates utility for |interval|.
float CalculateUtility(const MonitorInterval* interval);

}  // namespace quic

#endif
