/**
 * @file tests/unit/test_display_control.cpp
 * @brief Validate stable capture selection and display request rejection.
 */
#include "../tests_common.h"
#include "src/display_control.h"

TEST(DisplayControl, ResolvesNamesAfterDisplayReordering) {
  EXPECT_EQ(display_control::select_capture_index("second", {"second", "first"}, 1), 0);
  EXPECT_EQ(display_control::select_capture_index("second", {"first", "second", "third"}, 0), 1);
}

TEST(DisplayControl, KeepsTheCurrentDisplayWhenTheRequestedDisplayDisappears) {
  EXPECT_EQ(display_control::select_capture_index("removed", {"first", "second"}, 1), 1);
  EXPECT_EQ(display_control::select_capture_index("removed", {"first"}, 5), 0);
  EXPECT_EQ(display_control::select_capture_index("removed", {}, 0), -1);
}

TEST(DisplayControl, PreservesLegacyHotkeyClamping) {
  EXPECT_EQ(display_control::resolve_switch(99, {"first", "second"}, 0), 1);
  EXPECT_EQ(display_control::resolve_switch(-1, {"first", "second"}, 1), 0);
  EXPECT_EQ(display_control::resolve_switch(0, {}, 0), -1);
}

TEST(DisplayControl, RejectsMalformedOperationsBeforeChangingWindowsSettings) {
  EXPECT_FALSE(display_control::apply(nlohmann::json::object()).at("status").get<bool>());
  EXPECT_FALSE(display_control::apply({{"device_id", 1}, {"action", "mode"}}).at("status").get<bool>());
}
