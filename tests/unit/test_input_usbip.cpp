/**
 * @file tests/unit/test_input_usbip.cpp
 * @brief Validate explicit USB/IP controller retirement markers.
 */

#include "../tests_common.h"
#include "src/input.h"

TEST(UsbIpGamepadRetirement, RequiresExactInactiveMarker) {
  EXPECT_TRUE(input::is_usbip_gamepad_retirement(false, 0, 0x4000));
  EXPECT_FALSE(input::is_usbip_gamepad_retirement(true, 0, 0x4000));
  EXPECT_FALSE(input::is_usbip_gamepad_retirement(false, 1, 0x4000));
  EXPECT_FALSE(input::is_usbip_gamepad_retirement(false, 0, 0x4001));
  EXPECT_FALSE(input::is_usbip_gamepad_retirement(false, 0, 0));
}
