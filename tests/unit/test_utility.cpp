/**
 * @file tests/unit/test_utility.cpp
 * @brief Bounds regression tests for hexadecimal conversion.
 */
#include <gtest/gtest.h>
#include "src/utility.h"

TEST(UtilityFromHexTests, LongInputKeepsTheLowOrderBytes) {
  EXPECT_EQ(util::from_hex<std::uint32_t>("123456789A"), util::endian::little(std::uint32_t {0x3456789A}));
  EXPECT_EQ(util::from_hex<std::uint32_t>("123456789A", true), util::endian::big(std::uint32_t {0x3456789A}));
}
