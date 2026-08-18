/**
 * @file tests/unit/platform/windows/test_capture_diagnostics.cpp
 * @brief Tests for Windows display capture diagnostic helpers.
 */
#include "../../../tests_common.h"

#include "src/platform/windows/capture_diagnostics.h"

using namespace std::chrono_literals;

/**
 * @brief Test fixture for Windows capture stall tracking.
 */
class CaptureDiagnosticsTest: public BaseTest {
protected:
  using tracker_t = platf::dxgi::capture_stall_tracker_t;  ///< Tracker type under test.
  using time_point_t = tracker_t::time_point_t;  ///< Deterministic monotonic timestamp type.
};

TEST_F(CaptureDiagnosticsTest, ReportsOngoingStallAtConfiguredIntervals) {
  tracker_t tracker {1s, 5s};
  const time_point_t start {};

  EXPECT_FALSE(tracker.on_frame(start));
  EXPECT_FALSE(tracker.on_timeout(start + 999ms));
  EXPECT_EQ(tracker.on_timeout(start + 1s), 1000ms);
  EXPECT_FALSE(tracker.on_timeout(start + 5999ms));
  EXPECT_EQ(tracker.on_timeout(start + 6s), 6000ms);
  EXPECT_EQ(tracker.active_duration(start + 6s), 6000ms);
}

TEST_F(CaptureDiagnosticsTest, ReportsLongStallWhenCaptureResumes) {
  tracker_t tracker {1s, 5s};
  const time_point_t start {};

  EXPECT_FALSE(tracker.on_frame(start));
  EXPECT_EQ(tracker.on_timeout(start + 1s), 1000ms);
  EXPECT_EQ(tracker.on_frame(start + 6100ms), 6100ms);
  EXPECT_FALSE(tracker.active_duration(start + 6100ms));
}

TEST_F(CaptureDiagnosticsTest, IgnoresShortNoFrameIntervals) {
  tracker_t tracker {1s, 5s};
  const time_point_t start {};

  EXPECT_FALSE(tracker.on_frame(start));
  EXPECT_FALSE(tracker.on_timeout(start + 100ms));
  EXPECT_FALSE(tracker.on_frame(start + 500ms));
}

TEST_F(CaptureDiagnosticsTest, TracksStallBeforeFirstCapturedFrame) {
  tracker_t tracker {1s, 5s};
  const time_point_t start {};

  EXPECT_FALSE(tracker.on_timeout(start));
  EXPECT_EQ(tracker.on_timeout(start + 1s), 1000ms);
}
