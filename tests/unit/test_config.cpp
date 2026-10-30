/**
 * @file tests/unit/test_config.cpp
 * @brief Regression coverage for configuration boolean conversion.
 */
#include <gtest/gtest.h>
#include <string>

namespace config {
  /**
   * @brief Parse a boolean configuration value.
   * @param value Value normalized in place.
   * @return Parsed boolean value.
   */
  bool to_bool(std::string &value);
}

TEST(ConfigParsing, CaseInsensitiveBooleans) {
  for (const auto *text : {"Enabled", "TRUE", "On", "Yes", "1"}) {
    std::string value(text);
    EXPECT_TRUE(config::to_bool(value)) << text;
  }
  for (const auto *text : {"Disabled", "False", "OFF", "0", "unknown"}) {
    std::string value(text);
    EXPECT_FALSE(config::to_bool(value)) << text;
  }
}
