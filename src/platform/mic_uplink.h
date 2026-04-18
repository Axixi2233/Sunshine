#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace platf::mic_uplink {
  constexpr std::uint32_t sample_rate = 48000;
  constexpr std::uint32_t channels = 1;
  constexpr std::uint32_t frame_ms = 20;

  class sink_t {
  public:
    virtual bool push_pcm(const std::int16_t *samples, std::size_t frame_count, std::uint32_t channel_count, std::uint32_t input_sample_rate) = 0;
    virtual ~sink_t() = default;
  };

#ifdef _WIN32
  bool available(const std::string &device_name);
  std::unique_ptr<sink_t> create_sink(const std::string &device_name);
#else
  inline bool available(const std::string &) {
    return false;
  }

  inline std::unique_ptr<sink_t> create_sink(const std::string &) {
    return nullptr;
  }
#endif
}  // namespace platf::mic_uplink
