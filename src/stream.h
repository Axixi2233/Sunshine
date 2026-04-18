/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>
#include <utility>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;
  constexpr auto MIC_UPLINK_PORT = 12;

  struct session_t;

  struct mic_uplink_info_t {
    std::uint32_t session_id {};
    std::array<std::uint8_t, 16> token {};
  };

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void join(session_t &session);
    state_e state(session_t &session);
  }  // namespace session

  std::optional<mic_uplink_info_t> request_mic_uplink(std::string_view unique_id);
}  // namespace stream
