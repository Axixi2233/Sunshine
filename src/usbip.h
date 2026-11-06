/**
 * @file src/usbip.h
 * @brief LAN-only USB/IP import helpers owned by an authenticated streaming session.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace usbip {
  /** @brief Opaque ownership of one streaming session's Windows importer. */
  class session_t;

  /**
   * @brief Resolve an explicitly opted-in stream peer to a private IPv4 import target.
   * @param requested Client USB/IP mode preference.
   * @param peer Actual streaming peer address, including an IPv4-mapped address.
   * @return Canonical RFC1918 IPv4 address, or no target for unsupported sessions.
   */
  std::optional<std::string> session_target(bool requested, const std::string &peer);

  /**
   * @brief Start the bundled importer without blocking ordinary streaming on driver preparation.
   * @param requested Explicit client opt-in.
   * @param peer Actual streaming peer address.
   * @param launch_id Launch-session identity used for graceful cancellation.
   * @return Session-owned helper, or an empty handle when unavailable or not requested.
   */
  std::shared_ptr<session_t> start(bool requested, const std::string &peer, std::uint32_t launch_id);

  /**
   * @brief Signal cancellation without waiting on the control-channel thread.
   * @param session Importer associated with the ending stream.
   */
  void request_stop(const std::shared_ptr<session_t> &session);
}  // namespace usbip
