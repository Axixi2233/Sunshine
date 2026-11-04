/**
 * @file tests/unit/test_web_sessions.cpp
 * @brief Test browser session expiry, revocation, and cookie parsing.
 */
#include "src/web_sessions.h"

#include <gtest/gtest.h>

TEST(WebSessions, Lifecycle) {
  web_sessions::store sessions;
  const auto now = std::chrono::steady_clock::now();
  sessions.add("token", "credentials", now);
  EXPECT_TRUE(sessions.valid("token", "credentials", now));
  EXPECT_FALSE(sessions.valid("unknown", "credentials", now));
  EXPECT_FALSE(sessions.valid("token", "changed", now));
  sessions.add("token", "credentials", now);
  EXPECT_FALSE(sessions.valid("token", "credentials", now + std::chrono::hours(12)));
  sessions.add("token", "credentials", now);
  sessions.remove("token");
  EXPECT_FALSE(sessions.valid("token", "credentials", now));
  sessions.add("token", "credentials", now);
  sessions.clear();
  EXPECT_FALSE(sessions.valid("token", "credentials", now));
}

TEST(WebSessions, CookieParsing) {
  EXPECT_EQ(web_sessions::cookie_token("other=x; sunshine_session=abc; third=y"), "abc");
  EXPECT_EQ(web_sessions::cookie_token(" sunshine_session=abc "), "abc");
  EXPECT_TRUE(web_sessions::cookie_token("not_sunshine_session=abc").empty());
  EXPECT_TRUE(web_sessions::cookie_token("sunshine_session=").empty());
  EXPECT_TRUE(web_sessions::cookie_token("sunshine_session=a; sunshine_session=b").empty());
  EXPECT_TRUE(web_sessions::cookie_token("").empty());
  EXPECT_TRUE(web_sessions::cookie_token(" ; \t ; other=x").empty());
}

TEST(WebSessions, BoundedStorageAndExpiredEntryCleanup) {
  web_sessions::store sessions;
  const auto now = std::chrono::steady_clock::now();
  sessions.add("old", "credentials", now - std::chrono::hours(12));
  sessions.add("fresh", "credentials", now);
  EXPECT_FALSE(sessions.valid("old", "credentials", now));
  EXPECT_TRUE(sessions.valid("fresh", "credentials", now));
  for (int i = 0; i < 300; ++i) {
    sessions.add(std::to_string(i), "credentials", now);
  }
  int active = sessions.valid("fresh", "credentials", now) ? 1 : 0;
  for (int i = 0; i < 300; ++i) {
    active += sessions.valid(std::to_string(i), "credentials", now) ? 1 : 0;
  }
  EXPECT_EQ(active, 256);
}
