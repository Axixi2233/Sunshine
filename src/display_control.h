/**
 * @file src/display_control.h
 * @brief Paired-client display inventory and targeted Windows display controls.
 */
#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace display_control {
  /**
   * @brief Record the display actually opened by the streaming capture loop.
   * @param name Platform display name.
   * @param available Capture backend's ordered available display names.
   */
  void report_capture(const std::string &name, const std::vector<std::string> &available);
  /**
   * @brief Resolve a pending stable-device selection against the latest capture list.
   * @param fallback Index requested by a legacy hotkey.
   * @param available Fresh capture backend display names.
   * @param current Current capture index.
   * @return Resolved capture index.
   */
  int resolve_switch(int fallback, const std::vector<std::string> &available, int current);
  /**
   * @brief Resolve a named display after hotplug has reordered or removed outputs.
   * @param name Requested platform name.
   * @param available Fresh capture output names.
   * @param current Current index to retain when the target disappeared.
   * @return Selected index, or -1 for an empty list.
   */
  int select_capture_index(const std::string &name, const std::vector<std::string> &available, int current);
  /**
   * @brief Build the current inventory with supported modes and per-monitor controls.
   * @return JSON response including status, devices and actual capture identity.
   */
  nlohmann::json inventory();
  /**
   * @brief Apply one validated operation to one active display.
   * @param request Device ID, action and action-specific parameters.
   * @return Operation result or an error message.
   */
  nlohmann::json apply(const nlohmann::json &request);
}  // namespace display_control
