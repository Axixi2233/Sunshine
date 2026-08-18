/**
 * @file tests/unit/platform/test_common.cpp
 * @brief Test src/platform/common.*.
 */
#include "../../tests_common.h"

#include <boost/asio/ip/host_name.hpp>
#include <src/platform/common.h>

TEST(HostnameTests, TestAsioEquality) {
  // These should be equivalent on all platforms for ASCII hostnames
  ASSERT_EQ(platf::get_host_name(), boost::asio::ip::host_name());
}

TEST(GamepadFeedbackTests, CreatesDualSensePlayerIndicatorMessage) {
  const auto message = platf::gamepad_feedback_msg_t::make_player_indicator(3, 0x15);

  EXPECT_EQ(message.type, platf::gamepad_feedback_e::set_player_indicator);
  EXPECT_EQ(message.id, 3);
  EXPECT_EQ(message.data.player_indicator.value, 0x15);
}

TEST(GamepadFeedbackTests, CreatesBoundedControllerHapticPcmMessage) {
  std::array<std::uint8_t, platf::MAX_GAMEPAD_HAPTIC_PCM_BYTES + 8> pcm {};
  pcm.front() = 0x12;
  pcm[platf::MAX_GAMEPAD_HAPTIC_PCM_BYTES - 1] = 0x34;

  const auto message = platf::gamepad_feedback_msg_t::make_controller_haptic_pcm(2, 7, 48000, 4, 16, pcm);

  EXPECT_EQ(message.type, platf::gamepad_feedback_e::controller_haptic_pcm);
  EXPECT_EQ(message.id, 2);
  EXPECT_EQ(message.data.controller_haptic_pcm.sequence, 7);
  EXPECT_EQ(message.data.controller_haptic_pcm.sample_rate, 48000U);
  EXPECT_EQ(message.data.controller_haptic_pcm.channels, 4);
  EXPECT_EQ(message.data.controller_haptic_pcm.bits_per_sample, 16);
  EXPECT_EQ(message.data.controller_haptic_pcm.size, platf::MAX_GAMEPAD_HAPTIC_PCM_BYTES);
  EXPECT_EQ(message.data.controller_haptic_pcm.pcm.front(), 0x12);
  EXPECT_EQ(message.data.controller_haptic_pcm.pcm.back(), 0x34);
}

TEST(GamepadArrivalTests, DecodesClientGamepadEmulationPreference) {
  EXPECT_EQ(platf::gamepad_emulation_from_capabilities(LI_CCAP_EMULATION_AUTO), platf::gamepad_emulation_e::automatic);
  EXPECT_EQ(platf::gamepad_emulation_from_capabilities(LI_CCAP_EMULATION_X360), platf::gamepad_emulation_e::x360);
  EXPECT_EQ(platf::gamepad_emulation_from_capabilities(LI_CCAP_EMULATION_DS4), platf::gamepad_emulation_e::ds4);
  EXPECT_EQ(platf::gamepad_emulation_from_capabilities(LI_CCAP_EMULATION_DS5), platf::gamepad_emulation_e::ds5);
  EXPECT_EQ(
    platf::gamepad_emulation_from_capabilities(LI_CCAP_EMULATION_DS5 | LI_CCAP_ACCEL | LI_CCAP_HAPTIC_PCM),
    platf::gamepad_emulation_e::ds5
  );
}

TEST(GamepadArrivalTests, ClientGamepadEmulationPreferenceOverridesHost) {
  EXPECT_EQ(
    platf::resolve_gamepad_emulation(platf::gamepad_emulation_e::ds5, "x360"),
    platf::gamepad_emulation_e::ds5
  );
  EXPECT_EQ(
    platf::resolve_gamepad_emulation(platf::gamepad_emulation_e::x360, "ds5"),
    platf::gamepad_emulation_e::x360
  );
}

TEST(GamepadArrivalTests, AutomaticClientGamepadEmulationPreferenceUsesHost) {
  EXPECT_EQ(
    platf::resolve_gamepad_emulation(platf::gamepad_emulation_e::automatic, "ds4"),
    platf::gamepad_emulation_e::ds4
  );
  EXPECT_EQ(
    platf::resolve_gamepad_emulation(platf::gamepad_emulation_e::automatic, "auto"),
    platf::gamepad_emulation_e::automatic
  );
}
