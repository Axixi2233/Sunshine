/**
 * @file src/usbip.h
 * @brief LAN-only USB/IP import helpers owned by an authenticated streaming session.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace usbip {
  /**
   * @brief Identify peers eligible for the direct LAN transport, including on-link global IPv6.
   * @param peer Actual authenticated streaming peer.
   * @return Whether USB/IP may use its direct LAN path without the TLS bridge.
   */
  bool is_lan_peer(const std::string &peer);

  /**
   * @brief Inspect the bundled USB/IP driver and the last repair operation without installing anything.
   * @return Driver state and repair progress for the authenticated Web UI.
   */
  nlohmann::json driver_status();

  /**
   * @brief Start an elevated, fixed-purpose repair of an idle host's USB/IP driver.
   * @return Progress or a structured launch error; never accepts a user-supplied device or command.
   */
  nlohmann::json repair_driver();

  /**
   * @brief Discover the Windows source address used for IPv6 USB/IP connections to a local client.
   * @param requested Explicit USB/IP opt-in from the authenticated launch request.
   * @param peer Actual client address.
   * @param encrypted Whether paired TLS transport was negotiated; required for public IPv6.
   * @return Numeric IPv6 source for the paired response, or no advertisement for IPv4/unsupported peers.
   */
  std::optional<std::string> import_source(bool requested, const std::string &peer, bool encrypted = false);

  /** @brief Opaque ownership of one streaming session's Windows importer. */
  class session_t;

  /**
   * @brief Resolve an opted-in stream peer to a local IPv4 or IPv6 import target.
   * @param requested Client USB/IP mode preference.
   * @param peer Actual streaming peer address, including an IPv4-mapped address.
   * @param ipv6_on_link Whether the OS routes this IPv6 peer directly without a gateway.
   * @param encrypted Whether paired TLS transport was negotiated; public IPv4 remains unsupported.
   * @return Canonical local address, or no target for unsupported sessions.
   */
  std::optional<std::string> session_target(bool requested, const std::string &peer, bool ipv6_on_link = false, bool encrypted = false);

  /**
   * @brief Start the bundled importer without blocking ordinary streaming on driver preparation.
   * @param requested Explicit client opt-in.
   * @param peer Actual streaming peer address.
   * @param launch_id Launch-session identity used for graceful cancellation.
   * @param client_cert Exact paired Android certificate, passed through an inherited pipe for TLS pinning.
   * @param token Per-session secret; empty selects legacy LAN-only transport.
   * @return Session-owned helper, or an empty handle when unavailable or not requested.
   */
  std::shared_ptr<session_t> start(bool requested, const std::string &peer, std::uint32_t launch_id, const std::string &client_cert = "", const std::string &token = "");

  /**
   * @brief Signal cancellation without waiting on the control-channel thread.
   * @param session Importer associated with the ending stream.
   */
  void request_stop(const std::shared_ptr<session_t> &session);
}  // namespace usbip
