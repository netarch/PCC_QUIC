#include "third_party/pcc_quic/pcc_monitor_interval_queue.h"

#include "gfe/quic/core/quic_time.h"
#include "gfe/quic/core/quic_types.h"
#include "gfe/quic/platform/api/quic_test.h"

using testing::StrictMock;
using testing::_;

namespace gfe_quic {
namespace test {
namespace {

class MockDelegate : public PccMonitorIntervalQueueDelegateInterface {
 public:
  MockDelegate() {}
  ~MockDelegate() override {}
  MockDelegate(const MockDelegate&) = delete;
  MockDelegate& operator=(const MockDelegate&) = delete;
  MockDelegate(MockDelegate&&) = delete;
  MockDelegate& operator=(MockDelegate&&) = delete;

  MOCK_METHOD1(OnUtilityAvailable,
               void(const std::vector<UtilityInfo>& utility_info));
};

class PccMonitorIntervalQueueTest : public QuicTest {
 public:
  // Create a monitor interval and send |count_packet| packets.
  void SendMonitorInterval(float sending_rate_mbps,
                           bool is_useful,
                           QuicTime sent_time,
                           QuicPacketNumber first_packet_number,
                           size_t count_packet) {
    float packet_interval_us = kMaxPacketSize * 8 / sending_rate_mbps;
    QuicTime::Delta packet_interval =
        QuicTime::Delta::FromMicroseconds(packet_interval_us);

    sent_time = sent_time + packet_interval;
    queue_.EnqueueNewMonitorInterval(sending_rate_mbps, is_useful, 30000);
    queue_.OnPacketSent(sent_time, first_packet_number, kMaxPacketSize);
    for (size_t i = 1; i < count_packet; ++i) {
      sent_time = sent_time + packet_interval;
      queue_.OnPacketSent(sent_time, first_packet_number + i, kMaxPacketSize);
    }
  }

 protected:
  PccMonitorIntervalQueueTest() : queue_(&delegate_) {}

  PccMonitorIntervalQueue queue_;
  StrictMock<MockDelegate> delegate_;
};

TEST_F(PccMonitorIntervalQueueTest, CreateNewMonitors) {
  EXPECT_TRUE(queue_.empty());

  // Create a new monitor interval, and the queue size should be 1.
  SendMonitorInterval(2.0, true, QuicTime::Zero(), 0, 1);
  EXPECT_EQ(1u, queue_.size());

  // Create another new monitor interval, and the queue size should increase.
  SendMonitorInterval(2.0, false, QuicTime::Zero(), 1, 1);
  EXPECT_EQ(2u, queue_.size());
}

TEST_F(PccMonitorIntervalQueueTest, OnPacketSent) {
  EXPECT_TRUE(queue_.empty());

  float sending_rate_mbps = 2.0;
  QuicTime sent_time = QuicTime::Zero();
  float packet_interval_us = kMaxPacketSize * 8 / sending_rate_mbps;
  QuicTime::Delta packet_interval =
      QuicTime::Delta::FromMicroseconds(packet_interval_us);

  // Create a new monitor interval.
  SendMonitorInterval(2.0, true, sent_time, 0, 1);
  // Check the current last_packet_sent_time and bytes_total.
  MonitorInterval interval = queue_.current();
  EXPECT_EQ(kMaxPacketSize, interval.bytes_total);
  EXPECT_EQ(sent_time + packet_interval, interval.last_packet_sent_time);

  // Sent another packet in this MonitorInterval.
  sent_time = sent_time + 2 * packet_interval;
  queue_.OnPacketSent(sent_time, 1, kMaxPacketSize);
  interval = queue_.current();
  // Check the last_packet_sent and bytes_total are updated.
  EXPECT_EQ(2 * kMaxPacketSize, interval.bytes_total);
  EXPECT_EQ(sent_time, interval.last_packet_sent_time);
}

TEST_F(PccMonitorIntervalQueueTest, OnCongestionEventUtilityNotAvailable) {
  float sending_rate_mbps = 2.0;
  QuicTime sent_time = QuicTime::Zero();

  size_t duration_us = 50000;
  float packet_interval_us = kMaxPacketSize * 8 / sending_rate_mbps;
  float count_packet = duration_us / packet_interval_us;

  // Create three useful MonitorIntervals.
  for (size_t i = 0; i < 3; ++i) {
    SendMonitorInterval(2.0, true, sent_time, i * count_packet, count_packet);
  }
  // There should be 3 MonitorIntervals in the queue now.
  EXPECT_EQ(3u, queue_.size());

  // Give an empty list of acked packets.
  SendAlgorithmInterface::CongestionVector packets_acked, packets_lost;
  EXPECT_CALL(delegate_, OnUtilityAvailable(_)).Times(0);
  // The queue size should not change, and OnUtilityAvailable is not called.
  queue_.OnCongestionEvent(packets_acked, packets_lost, 30000);
  EXPECT_EQ(3u, queue_.size());
}

TEST_F(PccMonitorIntervalQueueTest, OnCongestionEvent) {
  EXPECT_TRUE(queue_.empty());

  float sending_rate_mbps = 2.0;
  QuicTime sent_time = QuicTime::Zero();
  QuicPacketNumber packet_number = 0;
  size_t rtt_us = 30000;

  size_t duration_us = 50000;
  float packet_interval_us = kMaxPacketSize * 8 / sending_rate_mbps;
  float count_packet = duration_us / packet_interval_us;

  // Create six MonitorIntervals, with the last one being not 'useful'.
  // Create four useful MonitorIntervals
  for (size_t i = 0; i < 4; ++i) {
    SendMonitorInterval(2.0, true, sent_time, i * count_packet, count_packet);
  }
  // Then create two non-useful MonitorIntervals
  for (size_t i = 4; i < 6; ++i) {
    SendMonitorInterval(2.0, false, sent_time, i * count_packet, count_packet);
  }
  // Queue size is six after the creation of two MonitorIntervals.
  EXPECT_EQ(6u, queue_.size());

  SendAlgorithmInterface::CongestionVector packets_acked, packets_lost;
  packet_number = 0;
  // Mark all the packets of the first three MonitorIntervals as acked.
  for (size_t i = 0; i < 3 * count_packet; ++i) {
    packets_acked.push_back(std::make_pair(packet_number, kMaxPacketSize));
    ++packet_number;
  }
  // Mark all the packets of the fourth MonitorInterval as lost.
  for (size_t i = 3 * count_packet; i < 4 * count_packet; ++i) {
    packets_lost.push_back(std::make_pair(packet_number, kMaxPacketSize));
    ++packet_number;
  }
  EXPECT_CALL(delegate_, OnUtilityAvailable(_));
  // OnUtilityAvailable is called, removing the first four MonitorIntervals.
  queue_.OnCongestionEvent(packets_acked, packets_lost, rtt_us);
  EXPECT_EQ(2u, queue_.size());
}

TEST_F(PccMonitorIntervalQueueTest, NumUsefulIntervals) {
  EXPECT_EQ(0u, queue_.num_useful_intervals());

  QuicTime sent_time = QuicTime::Zero();

  // Create a useful monitor interval
  SendMonitorInterval(2.0, true, sent_time, 0, 2);
  // There should be one useful intervals now
  EXPECT_EQ(1u, queue_.num_useful_intervals());

  // Create a non-useful monitor interval
  sent_time = sent_time + QuicTime::Delta::FromMicroseconds(100000);
  SendMonitorInterval(2.0, false, sent_time, 2, 2);
  // The number of useful intervals should stay the same
  EXPECT_EQ(1u, queue_.num_useful_intervals());

  // Create another useful monitor interval
  sent_time = sent_time + QuicTime::Delta::FromMicroseconds(100000);
  SendMonitorInterval(2.0, true, sent_time, 4, 2);
  // The number of useful intervals should increase by 1
  EXPECT_EQ(2u, queue_.num_useful_intervals());

  // Mark all the packets as acked.
  SendAlgorithmInterface::CongestionVector packets_acked, packets_lost;
  QuicPacketNumber packet_number = 0;
  for (size_t i = 0; i < 6; ++i) {
    packets_acked.push_back(std::make_pair(packet_number, kMaxPacketSize));
    ++packet_number;
  }
  EXPECT_CALL(delegate_, OnUtilityAvailable(_));
  queue_.OnCongestionEvent(packets_acked, packets_lost, 30000);
  // There should be no useful intervals at last
  EXPECT_EQ(0u, queue_.num_useful_intervals());
}

TEST_F(PccMonitorIntervalQueueTest, InvalidUtility) {
  EXPECT_EQ(0u, queue_.num_useful_intervals());

  // Create a useful monitor interval with only one packet.
  SendMonitorInterval(2.0, true, QuicTime::Zero(), 0, 1);
  // Create a non-useful monitor interval.
  SendMonitorInterval(2.0, false, QuicTime::Zero(), 1, 2);
  // There should be one useful interval, and overall two intervals.
  EXPECT_EQ(1u, queue_.num_useful_intervals());
  EXPECT_EQ(2u, queue_.size());

  // Acknowledge the fist packet. OnUtilityAvailable is not called because the
  // useful interval has invalid utility.
  SendAlgorithmInterface::CongestionVector packets_acked, packets_lost;
  packets_acked.push_back(std::make_pair(0, kMaxPacketSize));
  EXPECT_CALL(delegate_, OnUtilityAvailable(_)).Times(0);
  queue_.OnCongestionEvent(packets_acked, packets_lost, 30000);
  // There should be no useful interval, and only a non-useful interval left.
  EXPECT_EQ(0u, queue_.num_useful_intervals());
  EXPECT_EQ(1u, queue_.size());
}

}  // namespace
}  // namespace test
}  // namespace gfe_quic
