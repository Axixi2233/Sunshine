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

TEST(UsbIpSession, AdvertisesImportSourceOnlyForOptedInLocalIpv6Clients) {
  EXPECT_FALSE(usbip::import_source(false, "fd00::2"));
  for (const auto *peer : {"192.168.1.2", "::ffff:192.168.1.2", "::1", "::", "ff02::1", "host.invalid"}) {
    EXPECT_FALSE(usbip::import_source(true, peer)) << peer;
  }
}

TEST(UsbIpSession, AcceptsOnlyPrivateIpv4Peers) {
  for (const auto *address : {"10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.1.1", "::ffff:192.168.1.1"}) {
    EXPECT_TRUE(usbip::session_target(true, address)) << address;
  }
  for (const auto *address : {"", "host.local", "127.0.0.1", "169.254.1.1", "172.15.1.1", "172.32.1.1", "8.8.8.8", "224.0.0.1", "::1", "192.168.1.999"}) {
    EXPECT_FALSE(usbip::session_target(true, address)) << address;
  }
}

TEST(UsbIpSession, FollowsLocalIpv6StreamingPeers) {
  EXPECT_EQ(usbip::session_target(true, "fd12:3456::2"), "fd12:3456::2");
  EXPECT_EQ(usbip::session_target(true, "fe80::1234%7"), "fe80::1234%7");
  EXPECT_EQ(usbip::session_target(true, "2001:db8:1234::2", true), "2001:db8:1234::2");
  EXPECT_FALSE(usbip::session_target(false, "fd00::2", true));
  for (const auto *address : {"::", "::1", "ff02::1", "fe80::1", "fec0::1", "::192.168.1.1", "2001:db8::2"}) {
    EXPECT_FALSE(usbip::session_target(true, address)) << address;
  }
  for (const auto *address : {"::", "::1", "ff02::1", "fec0::1", "8.8.8.8"}) {
    EXPECT_FALSE(usbip::session_target(true, address, true)) << address;
  }
}

TEST(UsbIpSession, PublicIpv6RequiresNegotiatedTls) {
  EXPECT_FALSE(usbip::session_target(true, "2001:db8:abcd::2", false, false));
  EXPECT_EQ(usbip::session_target(true, "2001:db8:abcd::2", false, true), "2001:db8:abcd::2");
  EXPECT_FALSE(usbip::session_target(false, "2001:db8:abcd::2", false, true));
  for (const auto *peer : {"8.8.8.8", "::ffff:8.8.8.8", "::", "::1", "ff02::1", "fe80::2"}) {
    EXPECT_FALSE(usbip::session_target(true, peer, false, true)) << peer;
  }
}

TEST(UsbIpSession, KeepsLanPeersOnDirectTransport) {
  for (const auto *peer : {"192.168.1.2", "10.0.0.2", "::ffff:192.168.1.2", "fd00::2", "fe80::2%7"}) {
    EXPECT_TRUE(usbip::is_lan_peer(peer)) << peer;
  }
  for (const auto *peer : {"8.8.8.8", "::1", "ff02::1", "", "host.invalid"}) {
    EXPECT_FALSE(usbip::is_lan_peer(peer)) << peer;
  }
}
