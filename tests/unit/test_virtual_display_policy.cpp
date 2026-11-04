/**
 * @file tests/unit/test_virtual_display_policy.cpp
 * @brief Verify virtual display configuration precedence.
 */
#include "src/virtual_display_policy.h"

#include <gtest/gtest.h>

TEST(VirtualDisplayPolicy, ClientAlwaysWins) {
  for (int client = 0; client <= 2; ++client) {
    for (bool enabled : {false, true}) {
      for (bool app : {false, true}) {
        for (int mode : {1, 2}) {
          EXPECT_EQ(virtual_display_policy::resolve(client, enabled, mode, app), client);
        }
      }
    }
  }
}

TEST(VirtualDisplayPolicy, GlobalAndLegacyDefaults) {
  EXPECT_EQ(virtual_display_policy::resolve(std::nullopt, false, 1, false), 0);
  EXPECT_EQ(virtual_display_policy::resolve(std::nullopt, false, 2, true), 1);
  EXPECT_EQ(virtual_display_policy::resolve(std::nullopt, true, 1, true), 1);
  EXPECT_EQ(virtual_display_policy::resolve(std::nullopt, true, 2, false), 2);
  EXPECT_EQ(virtual_display_policy::resolve(std::nullopt, true, 99, false), 1);
}
