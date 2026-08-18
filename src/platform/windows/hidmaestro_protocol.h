/**
 * @file src/platform/windows/hidmaestro_protocol.h
 * @brief Binary protocol shared with the Sunshine HIDMaestro helper.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <utility>
#include <vector>

namespace platf::hidmaestro::protocol {
  constexpr std::uint32_t magic = 0x314D4853;  ///< Little-endian wire value for "SHM1".
  constexpr std::uint16_t version = 1;  ///< Current protocol version.
  constexpr std::uint32_t max_payload_size = 64 * 1024;  ///< Largest accepted payload.
  constexpr std::size_t max_pcm_bytes = 3920;  ///< Largest PCM window transported in one helper event.

  /**
   * @brief Commands and events carried by the HIDMaestro pipe.
   */
  enum class message_type_e : std::uint16_t {
    ready = 1,  ///< Helper initialization result.
    create_controller = 2,  ///< Create a virtual DualSense.
    controller_created = 3,  ///< Result of a create request.
    destroy_controller = 4,  ///< Destroy a virtual DualSense.
    gamepad_state = 5,  ///< Buttons, sticks, and triggers.
    touch = 6,  ///< Touchpad contact update.
    motion = 7,  ///< Accelerometer or gyroscope update.
    battery = 8,  ///< Battery update.
    rumble = 9,  ///< Dual-motor rumble output.
    rgb = 10,  ///< Lightbar output.
    adaptive_triggers = 11,  ///< DualSense adaptive-trigger output.
    log = 12,  ///< UTF-8 helper log record.
    shutdown = 13,  ///< Gracefully stop the helper.
    player_indicator = 14,  ///< DualSense player-indicator output.
    controller_pcm = 15,  ///< Native DualSense four-channel audio/haptics PCM.
  };

  /**
   * @brief One complete outbound helper message waiting to be written.
   */
  struct outbound_message_t {
    message_type_e type;  ///< Message type.
    std::uint32_t controller_id;  ///< Sunshine global controller index.
    std::vector<std::uint8_t> payload;  ///< Owned payload bytes.
  };

  /**
   * @brief Bounded helper-message queue with state update coalescing.
   *
   * @details This class is not internally synchronized. The caller must hold
   * the transport queue mutex while accessing it.
   */
  class outbound_queue_t {
  public:
    static constexpr std::size_t max_messages = 256;  ///< Maximum queued messages before low-priority input is dropped.

    /**
     * @brief Queue a message without allowing unbounded input growth.
     *
     * @param type Message type.
     * @param controller_id Sunshine global controller index.
     * @param payload Payload bytes, or null for an empty payload.
     * @param payload_size Payload byte count.
     * @return True when the message was queued or coalesced.
     */
    bool push(message_type_e type, std::uint32_t controller_id, const void *payload, std::uint32_t payload_size) {
      if (payload_size != 0 && payload == nullptr) {
        return false;
      }
      const auto *payload_bytes = static_cast<const std::uint8_t *>(payload);
      if (can_coalesce(type)) {
        for (auto message = messages_.rbegin(); message != messages_.rend(); ++message) {
          if (message->controller_id == controller_id && is_controller_barrier(message->type)) {
            break;
          }
          if (same_coalescing_key(*message, type, controller_id, payload_bytes, payload_size)) {
            message->payload.assign(payload_bytes, payload_bytes + payload_size);
            return true;
          }
          if (type == message_type_e::gamepad_state && message->type == type && message->controller_id == controller_id) {
            break;
          }
        }
      }

      if (messages_.size() >= max_messages) {
        if (!is_controller_barrier(type)) {
          return false;
        }
        const auto droppable = std::find_if(messages_.begin(), messages_.end(), [](const outbound_message_t &message) {
          return !is_controller_barrier(message.type);
        });
        if (droppable == messages_.end()) {
          return false;
        }
        messages_.erase(droppable);
      }

      outbound_message_t message {type, controller_id, {}};
      if (payload_size != 0) {
        message.payload.assign(payload_bytes, payload_bytes + payload_size);
      }
      messages_.push_back(std::move(message));
      return true;
    }

    /**
     * @brief Remove and return the oldest message.
     * @return The oldest queued message.
     */
    outbound_message_t pop() {
      auto message = std::move(messages_.front());
      messages_.pop_front();
      return message;
    }

    /**
     * @brief Check whether the queue has no messages.
     * @return True when empty.
     */
    [[nodiscard]] bool empty() const {
      return messages_.empty();
    }

    /**
     * @brief Return the number of queued messages.
     * @return Queued message count.
     */
    [[nodiscard]] std::size_t size() const {
      return messages_.size();
    }

    /**
     * @brief Drop all queued messages.
     */
    void clear() {
      messages_.clear();
    }

  private:
    /**
     * @brief Check whether later state of this type supersedes earlier state.
     * @param type Message type.
     * @return True when messages of this type may be coalesced.
     */
    static constexpr bool can_coalesce(message_type_e type) {
      return type == message_type_e::gamepad_state || type == message_type_e::motion || type == message_type_e::battery;
    }

    /**
     * @brief Check whether a message changes controller lifetime ordering.
     * @param type Message type.
     * @return True for controller create, destroy, and transport shutdown.
     */
    static constexpr bool is_controller_barrier(message_type_e type) {
      return type == message_type_e::create_controller || type == message_type_e::destroy_controller || type == message_type_e::shutdown;
    }

    /**
     * @brief Compare the coalescing identity of a queued and incoming message.
     *
     * @param queued Existing queued message.
     * @param type Incoming message type.
     * @param controller_id Incoming controller index.
     * @param payload Incoming payload bytes.
     * @param payload_size Incoming payload byte count.
     * @return True when the incoming state may replace the queued state.
     */
    static bool same_coalescing_key(const outbound_message_t &queued, message_type_e type, std::uint32_t controller_id, const std::uint8_t *payload, std::uint32_t payload_size) {
      if (queued.type != type || queued.controller_id != controller_id) {
        return false;
      }
      if (type == message_type_e::gamepad_state) {
        constexpr std::size_t button_bytes = sizeof(std::uint32_t);
        constexpr std::size_t hat_offset = 14;
        return queued.payload.size() > hat_offset && payload_size > hat_offset &&
               std::equal(queued.payload.begin(), queued.payload.begin() + button_bytes, payload) &&
               queued.payload[hat_offset] == payload[hat_offset];
      }
      if (type != message_type_e::motion) {
        return true;
      }
      constexpr std::size_t motion_type_offset = 12;
      return queued.payload.size() > motion_type_offset && payload_size > motion_type_offset && queued.payload[motion_type_offset] == payload[motion_type_offset];
    }

    std::deque<outbound_message_t> messages_;  ///< FIFO of messages awaiting the writer thread.
  };

#pragma pack(push, 1)

  /**
   * @brief Header preceding every binary protocol payload.
   */
  struct message_header_t {
    std::uint32_t magic;  ///< Protocol magic.
    std::uint16_t version;  ///< Protocol version.
    message_type_e type;  ///< Message type.
    std::uint32_t payload_size;  ///< Number of payload bytes following this header.
    std::uint32_t controller_id;  ///< Sunshine global controller index.
  };

  /**
   * @brief Generic signed status payload.
   */
  struct status_payload_t {
    std::int32_t status;  ///< Zero on success, otherwise a helper error code.
  };

  /**
   * @brief Select the virtual DualSense profile used for a controller.
   */
  struct create_controller_payload_t {
    std::uint8_t use_composite;  ///< Nonzero to request the USB composite profile with native PCM.
    std::array<std::uint8_t, 3> reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief Canonical gamepad state translated for HIDMaestro.
   */
  struct gamepad_payload_t {
    std::uint32_t buttons;  ///< HIDMaestro HMButton-compatible bit mask.
    std::int16_t left_stick_x;  ///< Left stick X in Sunshine signed range.
    std::int16_t left_stick_y;  ///< Left stick Y in Sunshine signed range.
    std::int16_t right_stick_x;  ///< Right stick X in Sunshine signed range.
    std::int16_t right_stick_y;  ///< Right stick Y in Sunshine signed range.
    std::uint8_t left_trigger;  ///< Left trigger in the range 0..255.
    std::uint8_t right_trigger;  ///< Right trigger in the range 0..255.
    std::uint8_t hat;  ///< HIDMaestro HMHat-compatible value.
    std::uint8_t reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief Controller touchpad update.
   */
  struct touch_payload_t {
    std::uint32_t pointer_id;  ///< Client-provided contact identifier.
    float x;  ///< Normalized horizontal coordinate.
    float y;  ///< Normalized vertical coordinate.
    float pressure;  ///< Normalized contact pressure.
    std::uint8_t event_type;  ///< Moonlight touch event type.
    std::array<std::uint8_t, 3> reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief Controller motion sample.
   */
  struct motion_payload_t {
    float x;  ///< X component in Moonlight physical units.
    float y;  ///< Y component in Moonlight physical units.
    float z;  ///< Z component in Moonlight physical units.
    std::uint8_t motion_type;  ///< Moonlight motion type.
    std::array<std::uint8_t, 3> reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief Controller battery state.
   */
  struct battery_payload_t {
    std::uint8_t state;  ///< Moonlight battery state.
    std::uint8_t percentage;  ///< Charge percentage.
    std::array<std::uint8_t, 2> reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief Standard DualSense motor output.
   */
  struct rumble_payload_t {
    std::uint16_t low_frequency;  ///< Low-frequency motor strength.
    std::uint16_t high_frequency;  ///< High-frequency motor strength.
  };

  /**
   * @brief DualSense lightbar output.
   */
  struct rgb_payload_t {
    std::uint8_t red;  ///< Red channel.
    std::uint8_t green;  ///< Green channel.
    std::uint8_t blue;  ///< Blue channel.
    std::uint8_t reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief DualSense player-indicator output.
   */
  struct player_indicator_payload_t {
    std::uint8_t value;  ///< Raw five-bit DualSense player-indicator mask.
    std::array<std::uint8_t, 3> reserved;  ///< Reserved and set to zero.
  };

  /**
   * @brief DualSense adaptive-trigger output.
   */
  struct adaptive_triggers_payload_t {
    std::uint8_t event_flags;  ///< 0x04 for right and 0x08 for left.
    std::uint8_t type_left;  ///< Left effect type.
    std::uint8_t type_right;  ///< Right effect type.
    std::uint8_t reserved;  ///< Reserved and set to zero.
    std::array<std::uint8_t, 10> left;  ///< Left effect payload excluding type.
    std::array<std::uint8_t, 10> right;  ///< Right effect payload excluding type.
  };

  /**
   * @brief Metadata preceding native controller PCM bytes.
   */
  struct controller_pcm_header_t {
    std::uint16_t sequence;  ///< Rolling PCM window sequence number.
    std::uint8_t channels;  ///< Interleaved channel count.
    std::uint8_t bits_per_sample;  ///< Bits stored for each sample.
    std::uint32_t sample_rate;  ///< Sample rate in Hz.
  };

#pragma pack(pop)

  static_assert(sizeof(message_header_t) == 16);
  static_assert(sizeof(status_payload_t) == 4);
  static_assert(sizeof(create_controller_payload_t) == 4);
  static_assert(sizeof(gamepad_payload_t) == 16);
  static_assert(sizeof(touch_payload_t) == 20);
  static_assert(sizeof(motion_payload_t) == 16);
  static_assert(sizeof(battery_payload_t) == 4);
  static_assert(sizeof(rumble_payload_t) == 4);
  static_assert(sizeof(rgb_payload_t) == 4);
  static_assert(sizeof(player_indicator_payload_t) == 4);
  static_assert(sizeof(adaptive_triggers_payload_t) == 24);
  static_assert(sizeof(controller_pcm_header_t) == 8);

  /**
   * @brief Convert Moonlight button flags to HIDMaestro HMButton bits.
   * @param flags Moonlight button flags.
   * @return HIDMaestro-compatible button mask.
   */
  constexpr std::uint32_t buttons_from_moonlight(std::uint32_t flags) {
    std::uint32_t buttons = 0;
    buttons |= (flags & 0x1000) ? 1U << 0 : 0;  // A / Cross
    buttons |= (flags & 0x2000) ? 1U << 1 : 0;  // B / Circle
    buttons |= (flags & 0x4000) ? 1U << 2 : 0;  // X / Square
    buttons |= (flags & 0x8000) ? 1U << 3 : 0;  // Y / Triangle
    buttons |= (flags & 0x0100) ? 1U << 4 : 0;  // Left bumper
    buttons |= (flags & 0x0200) ? 1U << 5 : 0;  // Right bumper
    buttons |= (flags & 0x0020) ? 1U << 6 : 0;  // Back / Share
    buttons |= (flags & 0x0010) ? 1U << 7 : 0;  // Start / Options
    buttons |= (flags & 0x0040) ? 1U << 8 : 0;  // Left stick
    buttons |= (flags & 0x0080) ? 1U << 9 : 0;  // Right stick
    buttons |= (flags & 0x0400) ? 1U << 10 : 0;  // Guide / PS
    buttons |= (flags & 0x100000) ? 1U << 11 : 0;  // Touchpad click
    buttons |= (flags & 0x010000) ? 1U << 13 : 0;  // Right paddle
    buttons |= (flags & 0x020000) ? 1U << 14 : 0;  // Left paddle
    buttons |= (flags & 0x200000) ? 1U << 15 : 0;  // Misc / mute
    buttons |= (flags & 0x040000) ? 1U << 16 : 0;  // Right paddle 2
    buttons |= (flags & 0x080000) ? 1U << 17 : 0;  // Left paddle 2
    return buttons;
  }

  /**
   * @brief Convert Moonlight button flags to HIDMaestro bits with optional Back-to-touchpad mapping.
   *
   * @param flags Moonlight button flags.
   * @param back_as_touchpad_click Whether Back/Select should also press the DualSense touchpad.
   * @return HIDMaestro-compatible button mask.
   */
  constexpr std::uint32_t buttons_from_moonlight(std::uint32_t flags, bool back_as_touchpad_click) {
    constexpr std::uint32_t back = 0x0020;
    constexpr std::uint32_t touchpad = 1U << 11;
    const auto buttons = buttons_from_moonlight(flags);
    return back_as_touchpad_click && (flags & back) ? buttons | touchpad : buttons;
  }

  /**
   * @brief Determine whether an inactive virtual DualSense can be rebound without PnP recreation.
   *
   * @param allocated Whether the virtual controller still exists.
   * @param active Whether a Moonlight controller is currently bound to it.
   * @param uses_composite Whether the existing controller exposes the composite PCM profile.
   * @param requests_native_pcm Whether the new Moonlight controller can consume native PCM.
   * @return True when the existing virtual controller satisfies the new binding.
   */
  constexpr bool can_reuse_virtual_dualsense(bool allocated, bool active, bool uses_composite, bool requests_native_pcm) {
    return allocated && !active && (uses_composite || !requests_native_pcm);
  }

  /**
   * @brief Determine whether native DualSense PCM contains a voice-coil haptic signal.
   *
   * @param pcm Interleaved signed 16-bit little-endian PCM bytes.
   * @param channels Interleaved channel count.
   * @param first_haptic_channel Zero-based index of the first haptic channel.
   * @param silence_threshold Absolute sample values at or below this level are silence.
   * @return True when a haptic-channel sample exceeds the threshold.
   */
  inline bool contains_pcm_signal(std::span<const std::uint8_t> pcm, std::size_t channels, std::size_t first_haptic_channel, std::int16_t silence_threshold) {
    constexpr std::size_t sample_bytes = sizeof(std::int16_t);
    const std::size_t frame_bytes = channels * sample_bytes;
    if (channels == 0 || first_haptic_channel >= channels || frame_bytes == 0) {
      return false;
    }
    for (std::size_t offset = 0; offset + frame_bytes <= pcm.size(); offset += frame_bytes) {
      for (std::size_t channel = first_haptic_channel; channel < channels; ++channel) {
        const std::size_t sample_offset = offset + channel * sample_bytes;
        const auto sample_bits = static_cast<std::uint16_t>(pcm[sample_offset]) |
                                 (static_cast<std::uint16_t>(pcm[sample_offset + 1]) << 8);
        const auto sample = static_cast<std::int16_t>(sample_bits);
        if (sample < -silence_threshold || sample > silence_threshold) {
          return true;
        }
      }
    }
    return false;
  }

  /**
   * @brief State transition produced by the native PCM silence gate.
   */
  enum class pcm_silence_event_e {
    forward,  ///< Forward the current window without changing pause state.
    pause,  ///< The silence limit was reached; suppress this and later silent windows.
    suppressed,  ///< The gate was already paused and remains silent.
    resume,  ///< A useful signal resumed; forward the current window.
  };

  /**
   * @brief Advance the native PCM silence gate by one captured window.
   *
   * @param has_signal Whether the current window contains useful haptic samples.
   * @param silent_frame_limit Consecutive silent PCM frames required to pause forwarding.
   * @param silent_frames_this_window PCM frames in the current captured window.
   * @param silent_frames Mutable consecutive-silence counter.
   * @param paused Mutable gate state.
   * @return The resulting forwarding or transition event.
   */
  constexpr pcm_silence_event_e update_pcm_silence_gate(bool has_signal, std::uint32_t silent_frame_limit, std::uint32_t silent_frames_this_window, std::uint32_t &silent_frames, bool &paused) {
    if (has_signal) {
      silent_frames = 0;
      if (paused) {
        paused = false;
        return pcm_silence_event_e::resume;
      }
      return pcm_silence_event_e::forward;
    }

    if (silent_frames < silent_frame_limit) {
      silent_frames += std::min(silent_frames_this_window, silent_frame_limit - silent_frames);
    }
    if (silent_frames < silent_frame_limit) {
      return pcm_silence_event_e::forward;
    }
    if (!paused) {
      paused = true;
      return pcm_silence_event_e::pause;
    }
    return pcm_silence_event_e::suppressed;
  }

  /**
   * @brief Convert Moonlight D-pad flags to an HIDMaestro HMHat value.
   * @param flags Moonlight button flags.
   * @return Hat value from zero (neutral) through eight (north-west).
   */
  constexpr std::uint8_t hat_from_moonlight(std::uint32_t flags) {
    constexpr std::uint32_t up = 0x0001;
    constexpr std::uint32_t down = 0x0002;
    constexpr std::uint32_t left = 0x0004;
    constexpr std::uint32_t right = 0x0008;

    if (flags & up) {
      if (flags & right) {
        return 2;
      }
      if (flags & left) {
        return 8;
      }
      return 1;
    }
    if (flags & down) {
      if (flags & right) {
        return 4;
      }
      if (flags & left) {
        return 6;
      }
      return 5;
    }
    if (flags & right) {
      return 3;
    }
    if (flags & left) {
      return 7;
    }
    return 0;
  }
}  // namespace platf::hidmaestro::protocol
