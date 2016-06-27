#include "net/quic/congestion_control/pcc_sender.h"


#include "base/logging.h"
#include "net/quic/test_tools/mock_clock.h"
#include "net/quic/test_tools/quic_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::Return;
using testing::StrictMock;
using testing::_;

namespace net {
namespace test {


class PCCSenderTest : public ::testing::Test {
 protected:
  PCCSenderTest() {}

  ~PCCSenderTest() override {}

  PCCMonitor createPCCMonitor(int ack, int loss, int rtt) {
    PCCMonitor pcc_monitor;
    pcc_monitor.srtt = rtt;
    pcc_monitor.start_time = QuicTime::Zero();
    pcc_monitor.end_transmission_time = pcc_monitor.start_time.Add(
        QuicTime::Delta::FromMicroseconds(3 * rtt));

    for (int i = 0; i < ack; i++) {
      PacketInfo packet_info;
      packet_info.bytes = 1;
      packet_info.state = ACK;

      pcc_monitor.packet_vector.push_back(packet_info);
    }

    for (int i = 0; i < loss; i++) {
      PacketInfo packet_info;
      packet_info.bytes = 1;
      packet_info.state = LOST;

      pcc_monitor.packet_vector.push_back(packet_info);
    }

    return pcc_monitor;
  }

  void moveToGuessing() {
    PCCMonitor pcc_monitor0 = createPCCMonitor(1, 1, 10);
    PCCMonitor pcc_monitor1 = createPCCMonitor(1, 5, 10);

    pcc_utility_.OnMonitorEnd(pcc_monitor0, 0, 0);
    EXPECT_EQ(pcc_utility_.GetCurrentState(), STARTING);

    pcc_utility_.OnMonitorEnd(pcc_monitor1, 1, 1);
    EXPECT_EQ(pcc_utility_.GetCurrentState(), GUESSING);
  }

  void moveToRecording() {
    moveToGuessing();
    pcc_utility_.OnMonitorStart(2);
    EXPECT_EQ(pcc_utility_.GetCurrentState(), RECORDING);
  }

  void moveToMoving() {
    moveToRecording();
    PCCMonitor pcc_monitor = createPCCMonitor(1, 1, 10);
    pcc_utility_.OnMonitorEnd(pcc_monitor, 2, 2);
    pcc_utility_.OnMonitorEnd(pcc_monitor, 3, 3);
    pcc_utility_.OnMonitorEnd(pcc_monitor, 4, 4);
    pcc_utility_.OnMonitorEnd(pcc_monitor, 5, 5);
    EXPECT_EQ(pcc_utility_.GetCurrentState(), MOVING);
  }

  void firstMove() {
    moveToMoving();
    PCCMonitor pcc_monitor = createPCCMonitor(1, 2, 10);
    pcc_utility_.OnMonitorEnd(pcc_monitor, 6, 6);
    EXPECT_EQ(pcc_utility_.GetCurrentState(), MOVING);
  }

  PCCUtility pcc_utility_;
};

// Rate should double on each monitor start at starting phase
TEST_F(PCCSenderTest, StartingPhase) {
  double current_rate = pcc_utility_.GetCurrentRate();
  EXPECT_EQ(pcc_utility_.GetCurrentState(), STARTING);

  pcc_utility_.OnMonitorStart(0);
  EXPECT_EQ(pcc_utility_.GetCurrentRate(), 2 * current_rate);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), STARTING);
}

// Exit starting phase if lost all previous monitors
TEST_F(PCCSenderTest, ExitStartingPhase0) {
  PCCMonitor pcc_monitor = createPCCMonitor(1, 1, 10);

  pcc_utility_.OnMonitorEnd(pcc_monitor, 2, 2);
  EXPECT_EQ(pcc_utility_.GetCurrentRate(), 0);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), GUESSING);
}

// Exist starting phase if more than one monitor lost
TEST_F(PCCSenderTest, ExitStartingPhase1) {
  PCCMonitor pcc_monitor = createPCCMonitor(1, 1, 10);

  pcc_utility_.OnMonitorStart(0);
  pcc_utility_.OnMonitorEnd(pcc_monitor, 0, 0);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), STARTING);

  pcc_utility_.OnMonitorEnd(pcc_monitor, 2, 2);
  EXPECT_EQ(pcc_utility_.GetCurrentRate(), 20);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), GUESSING);
}

// Exit starting phase, if current utility is lower than previous one
TEST_F(PCCSenderTest, ExitStartingPhase2) {
  moveToGuessing();
}

// make guesses and move to recording state
TEST_F(PCCSenderTest, GuessingToRecording) {
  moveToRecording();
}

// collect all guesses, make decision, and decide to move downward
TEST_F(PCCSenderTest, RecordingToMoving) {
  moveToMoving();
}

// Regardless current utility, PCC makes one move based on dicision
TEST_F(PCCSenderTest, FirstMove) {
  firstMove();
}

// Continue moving if current utility is larger than previous one
TEST_F(PCCSenderTest, ContinueMoving) {
  firstMove();

  PCCMonitor pcc_monitor = createPCCMonitor(1, 1, 10);
  pcc_utility_.OnMonitorEnd(pcc_monitor, 7, 7);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), MOVING);
}

// Exit moving if current utility is smaller than previous one
TEST_F(PCCSenderTest, ExitMoving) {
  firstMove();

  PCCMonitor pcc_monitor = createPCCMonitor(1, 3, 10);
  pcc_utility_.OnMonitorEnd(pcc_monitor, 7, 7);
  EXPECT_EQ(pcc_utility_.GetCurrentState(), GUESSING);
}

}  // namespace test
}  // namespace net