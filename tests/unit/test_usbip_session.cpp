/**
 * @file tests/unit/test_usbip_session.cpp
 * @brief Validate explicit LAN-only USB/IP importer selection.
 */
#include "../tests_common.h"
#include "src/usbip.h"

TEST(UsbIpSession, RequiresClientOptIn) {
  EXPECT_FALSE(usbip::session_target(false, "192.168.31.201"));
  EXPECT_EQ(usbip::session_target(true, "192.168.31.201"), "192.168.31.201");
}

TEST(UsbIpSession, AcceptsOnlyPrivateIpv4Peers) {
  for (const auto *address : {"10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.1.1", "::ffff:192.168.1.1"}) {
    EXPECT_TRUE(usbip::session_target(true, address)) << address;
  }
  for (const auto *address : {"", "host.local", "127.0.0.1", "169.254.1.1", "172.15.1.1", "172.32.1.1", "8.8.8.8", "224.0.0.1", "::1", "fd00::1", "192.168.1.999"}) {
    EXPECT_FALSE(usbip::session_target(true, address)) << address;
  }
}
