# PCC_QUIC

## How to import PCC to QUIC:
1. Place [pcc_sender.cc](https://github.com/netarch/PCC_QUIC/blob/master/quic/congestion_control/pcc_sender.cc) and [pcc_sender.h](https://github.com/netarch/PCC_QUIC/blob/master/quic/congestion_control/pcc_sender.h) in `net/quic/congestion_control` directory
2. Add `quic/congestion_control/pcc_sender.cc` and `quic/congestion_control/pcc_sender.h` to `net.gypi`
3. Add PCC control type to [send_algorithm_interface.cc](https://github.com/google/proto-quic/blob/master/src/net/quic/congestion_control/send_algorithm_interface.cc#L47)
![Step 3](https://github.com/netarch/PCC_QUIC/blob/master/instructions/3.png)
4. Add PCC QuicTag `const QuicTag kPCC = TAG('k', 'P', 'C', 'C');` to [crypto_protocol.h](https://github.com/google/proto-quic/blob/master/src/net/quic/crypto/crypto_protocol.h#L33)
5. Add `kPcc` to `enum CongestionControlType` in [quic_protocol.h](https://github.com/google/proto-quic/blob/master/src/net/quic/quic_protocol.h#L1004)
6. Add PCC config option to [quic_sent_packet_manager.cc](https://github.com/google/proto-quic/blob/master/src/net/quic/quic_sent_packet_manager.cc#L150)
![Step 6](https://github.com/netarch/PCC_QUIC/blob/master/instructions/6.png)
7. Config [quic_simple_server.cc](https://github.com/google/proto-quic/blob/master/src/net/tools/quic/quic_simple_server.cc#L91) to use PCC as control algorithms. We open up the flow control window because PCC is rate based. Please also note that during the demo comparing QUIC+CUBIC and QUIC+PCC, we use the same large value for QUIC+CUBIC to achieve an apple-to-apple comparison. ![Step 7](https://github.com/netarch/PCC_QUIC/blob/master/instructions/7.png)
8. Config [quic_simple_client.cc](https://github.com/google/proto-quic/blob/master/src/net/tools/quic/quic_simple_client.cc#L81) to use PCC as control algorithm ![Step 8](https://github.com/netarch/PCC_QUIC/blob/master/instructions/8.png)

