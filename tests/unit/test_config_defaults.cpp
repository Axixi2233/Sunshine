/**
 * @file tests/unit/test_config_defaults.cpp
 * @brief Regression tests for Sunshine's aggregate configuration defaults.
 */

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/config.h"

namespace {
  TEST(ConfigDefaults, InputFeaturesMatchDocumentedDefaults) {
    EXPECT_TRUE(config::input.keyboard);
    EXPECT_FALSE(config::input.key_rightalt_to_key_win);
    EXPECT_TRUE(config::input.mouse);
    EXPECT_TRUE(config::input.controller);
    EXPECT_TRUE(config::input.always_send_scancodes);
    EXPECT_TRUE(config::input.high_resolution_scrolling);
    EXPECT_TRUE(config::input.native_pen_touch);
    EXPECT_FALSE(config::input.ds5_back_as_touchpad_click);
  }
}  // namespace
