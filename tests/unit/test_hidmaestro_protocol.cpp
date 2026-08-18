/**
 * @file tests/unit/test_hidmaestro_protocol.cpp
 * @brief Tests for the Sunshine HIDMaestro helper protocol.
 */

// lib includes
#include <gtest/gtest.h>

// standard includes
#include <cstring>

// local includes
#include "src/platform/windows/hidmaestro_protocol.h"

namespace {
  namespace protocol = platf::hidmaestro::protocol;

  TEST(HIDMaestroProtocol, WireStructuresHaveStableSizes) {
    EXPECT_EQ(static_cast<std::uint16_t>(protocol::message_type_e::player_indicator), 14);
    EXPECT_EQ(static_cast<std::uint16_t>(protocol::message_type_e::controller_pcm), 15);
    EXPECT_EQ(sizeof(protocol::message_header_t), 16);
    EXPECT_EQ(sizeof(protocol::status_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::create_controller_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::gamepad_payload_t), 16);
    EXPECT_EQ(sizeof(protocol::touch_payload_t), 20);
    EXPECT_EQ(sizeof(protocol::motion_payload_t), 16);
    EXPECT_EQ(sizeof(protocol::battery_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::rumble_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::rgb_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::player_indicator_payload_t), 4);
    EXPECT_EQ(sizeof(protocol::adaptive_triggers_payload_t), 24);
    EXPECT_EQ(sizeof(protocol::controller_pcm_header_t), 8);
  }

  TEST(HIDMaestroProtocol, MapsMoonlightButtonsToHMButtonBits) {
    constexpr std::uint32_t all_mapped_buttons =
      0x0010 | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x0200 | 0x0400 |
      0x1000 | 0x2000 | 0x4000 | 0x8000 | 0x010000 | 0x020000 |
      0x040000 | 0x080000 | 0x100000 | 0x200000;
    constexpr std::uint32_t all_hm_buttons =
      (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) | (1U << 4) |
      (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8) | (1U << 9) |
      (1U << 10) | (1U << 11) | (1U << 13) | (1U << 14) | (1U << 15) |
      (1U << 16) | (1U << 17);

    EXPECT_EQ(protocol::buttons_from_moonlight(all_mapped_buttons), all_hm_buttons);
    EXPECT_EQ(protocol::buttons_from_moonlight(0), 0U);
    EXPECT_EQ(protocol::buttons_from_moonlight(0x000F), 0U);
  }

  TEST(HIDMaestroProtocol, OptionallyMapsBackToDualSenseTouchpadClick) {
    constexpr std::uint32_t back = 0x0020;
    constexpr std::uint32_t hm_back = 1U << 6;
    constexpr std::uint32_t hm_touchpad = 1U << 11;

    EXPECT_EQ(protocol::buttons_from_moonlight(back, false), hm_back);
    EXPECT_EQ(protocol::buttons_from_moonlight(back, true), hm_back | hm_touchpad);
    EXPECT_EQ(protocol::buttons_from_moonlight(0, true), 0U);
  }

  TEST(HIDMaestroProtocol, ReusesInactiveDualSenseWhenItsProfileSatisfiesTheClient) {
    EXPECT_TRUE(protocol::can_reuse_virtual_dualsense(true, false, true, true));
    EXPECT_TRUE(protocol::can_reuse_virtual_dualsense(true, false, true, false));
    EXPECT_TRUE(protocol::can_reuse_virtual_dualsense(true, false, false, false));
    EXPECT_FALSE(protocol::can_reuse_virtual_dualsense(true, false, false, true));
    EXPECT_FALSE(protocol::can_reuse_virtual_dualsense(true, true, true, true));
    EXPECT_FALSE(protocol::can_reuse_virtual_dualsense(false, false, true, true));
  }

  TEST(HIDMaestroProtocol, DetectsOnlyHapticChannelsInNativePcm) {
    std::vector<std::uint8_t> pcm(16, 0);
    pcm[0] = 0x20;
    EXPECT_FALSE(protocol::contains_pcm_signal(pcm, 4, 2, 8));

    pcm[4] = 0x09;
    EXPECT_TRUE(protocol::contains_pcm_signal(pcm, 4, 2, 8));

    pcm[4] = 0x08;
    pcm[6] = 0xF6;
    pcm[7] = 0xFF;
    EXPECT_TRUE(protocol::contains_pcm_signal(pcm, 4, 2, 8));
    EXPECT_FALSE(protocol::contains_pcm_signal(pcm, 0, 0, 8));
  }

  TEST(HIDMaestroProtocol, DetectsSpeakerChannelsForControllerPcmForwarding) {
    std::vector<std::uint8_t> pcm(16, 0);
    pcm[0] = 0x20;

    EXPECT_TRUE(protocol::contains_pcm_signal(pcm, 4, 0, 8));
    EXPECT_FALSE(protocol::contains_pcm_signal(pcm, 4, 2, 8));
  }

  TEST(HIDMaestroProtocol, PausesOnSustainedPcmSilenceAndResumesOnSignal) {
    std::uint32_t silent_frames = 0;
    bool paused = false;

    EXPECT_EQ(protocol::update_pcm_silence_gate(false, 12, 4, silent_frames, paused),
              protocol::pcm_silence_event_e::forward);
    EXPECT_EQ(protocol::update_pcm_silence_gate(false, 12, 4, silent_frames, paused),
              protocol::pcm_silence_event_e::forward);
    EXPECT_EQ(protocol::update_pcm_silence_gate(false, 12, 4, silent_frames, paused),
              protocol::pcm_silence_event_e::pause);
    EXPECT_TRUE(paused);
    EXPECT_EQ(protocol::update_pcm_silence_gate(false, 12, 4, silent_frames, paused),
              protocol::pcm_silence_event_e::suppressed);
    EXPECT_EQ(protocol::update_pcm_silence_gate(true, 12, 4, silent_frames, paused),
              protocol::pcm_silence_event_e::resume);
    EXPECT_FALSE(paused);
    EXPECT_EQ(silent_frames, 0);
  }

  TEST(HIDMaestroProtocol, MapsAllDpadDirectionsToHMHatValues) {
    EXPECT_EQ(protocol::hat_from_moonlight(0), 0);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0001), 1);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0001 | 0x0008), 2);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0008), 3);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0002 | 0x0008), 4);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0002), 5);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0002 | 0x0004), 6);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0004), 7);
    EXPECT_EQ(protocol::hat_from_moonlight(0x0001 | 0x0004), 8);
  }

  TEST(HIDMaestroProtocol, CoalescesHighFrequencyStateWithoutCrossingLifetimeBarriers) {
    protocol::outbound_queue_t queue;
    protocol::gamepad_payload_t first {};
    first.left_stick_x = 100;
    protocol::gamepad_payload_t second {};
    second.left_stick_x = 200;

    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &first, sizeof(first)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &second, sizeof(second)));
    EXPECT_EQ(queue.size(), 1U);
    auto coalesced = queue.pop();
    protocol::gamepad_payload_t coalesced_payload {};
    std::memcpy(&coalesced_payload, coalesced.payload.data(), sizeof(coalesced_payload));
    EXPECT_EQ(coalesced_payload.left_stick_x, 200);

    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &first, sizeof(first)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::destroy_controller, 0, nullptr, 0));
    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &second, sizeof(second)));
    EXPECT_EQ(queue.size(), 3U);
  }

  TEST(HIDMaestroProtocol, PreservesGamepadButtonTransitions) {
    protocol::outbound_queue_t queue;
    protocol::gamepad_payload_t released {};
    protocol::gamepad_payload_t pressed {};
    pressed.buttons = 1;

    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &released, sizeof(released)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &pressed, sizeof(pressed)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::gamepad_state, 0, &released, sizeof(released)));
    EXPECT_EQ(queue.size(), 3U);
  }

  TEST(HIDMaestroProtocol, CoalescesAccelerometerAndGyroscopeIndependently) {
    protocol::outbound_queue_t queue;
    protocol::motion_payload_t accelerometer {.x = 1.0f, .motion_type = 1};
    protocol::motion_payload_t gyroscope {.x = 2.0f, .motion_type = 2};
    protocol::motion_payload_t newer_accelerometer {.x = 3.0f, .motion_type = 1};

    ASSERT_TRUE(queue.push(protocol::message_type_e::motion, 0, &accelerometer, sizeof(accelerometer)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::motion, 0, &gyroscope, sizeof(gyroscope)));
    ASSERT_TRUE(queue.push(protocol::message_type_e::motion, 0, &newer_accelerometer, sizeof(newer_accelerometer)));
    EXPECT_EQ(queue.size(), 2U);

    auto first = queue.pop();
    protocol::motion_payload_t first_payload {};
    std::memcpy(&first_payload, first.payload.data(), sizeof(first_payload));
    EXPECT_EQ(first_payload.motion_type, 1);
    EXPECT_FLOAT_EQ(first_payload.x, 3.0f);

    auto second = queue.pop();
    protocol::motion_payload_t second_payload {};
    std::memcpy(&second_payload, second.payload.data(), sizeof(second_payload));
    EXPECT_EQ(second_payload.motion_type, 2);
    EXPECT_FLOAT_EQ(second_payload.x, 2.0f);
  }
}  // namespace
