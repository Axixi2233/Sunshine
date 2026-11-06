/**
 * @file src/display_control.cpp
 * @brief Display inventory and single-display controls used by paired streaming clients.
 */
#include "display_control.h"

#include "display_device.h"
#include "globals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>
#ifdef _WIN32
  #include <display_device/windows/win_api_layer.h>
  #include <display_device/windows/win_display_device.h>
  #include <windows.h>
#endif

namespace display_control {
  namespace {
    std::mutex state_mutex;  ///< Protects capture observations and pending selection.
    std::mutex operation_mutex;  ///< Serializes inventory queries and user display changes.
    std::string captured_name;  ///< Last successfully opened streaming display.
    std::vector<std::string> capture_names;  ///< Capture backend's available displays.
    std::optional<std::string> pending_name;  ///< Stable selection awaiting capture reinitialization.

    /**
     * @brief Convert display-device floating-point representations to a JSON number.
     * @param value Display-library floating-point value.
     * @return Numeric value or zero for an invalid denominator.
     */
    double number(const display_device::FloatingPoint &value) {
      return std::visit([](const auto &v) -> double {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, double>) {
          return v;
        } else {
          return v.m_denominator ? static_cast<double>(v.m_numerator) / v.m_denominator : 0.;
        }
      },
                        value);
    }

#ifdef _WIN32
    constexpr std::array scales {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};  ///< Windows per-monitor scale steps.

    /**
     * @brief Windows source DPI query, supported only when the OS accepts this device-info request.
     */
    struct scale_query_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {};  ///< Adapter and source identity.
      int minimum {};  ///< Minimum relative scale step.
      int current {};  ///< Current relative scale step.
      int maximum {};  ///< Maximum relative scale step.
    };

    /**
     * @brief Windows source DPI update.
     */
    struct scale_set_t {
      DISPLAYCONFIG_DEVICE_INFO_HEADER header {};  ///< Adapter and source identity.
      int relative {};  ///< Scale step relative to the display recommendation.
    };

    /**
     * @brief Resolve a GDI display name to its active Windows source and query scale support.
     * @param name GDI display name.
     * @return Validated relative scale range, or no value when unsupported.
     */
    std::optional<scale_query_t> query_scale(const std::string &name) {
      display_device::WinApiLayer api;
      const auto config = api.queryDisplayConfig(display_device::QueryType::Active);
      if (!config) {
        return std::nullopt;
      }
      for (const auto &path : config->m_paths) {
        if (api.getDisplayName(path) != name) {
          continue;
        }
        scale_query_t query;
        query.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3), sizeof(query), path.sourceInfo.adapterId, path.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&query.header) == ERROR_SUCCESS && query.minimum <= 0 && query.maximum >= 0 && query.current >= query.minimum && query.current <= query.maximum && query.maximum - static_cast<long long>(query.minimum) < static_cast<long long>(scales.size())) {
          return query;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Enumerate distinct driver-advertised resolution and refresh-rate combinations.
     * @param name GDI display name.
     * @return Array of progressive display modes.
     */
    nlohmann::json modes(const std::string &name) {
      nlohmann::json result = nlohmann::json::array();
      std::set<std::array<DWORD, 3>> seen;
      const std::wstring wide(name.begin(), name.end());
      DEVMODEW mode {};
      mode.dmSize = sizeof(mode);
      for (DWORD i = 0; EnumDisplaySettingsExW(wide.c_str(), i, &mode, 0); ++i) {
        if (mode.dmBitsPerPel < 24 || mode.dmDisplayFrequency < 2 || (mode.dmDisplayFlags & DM_INTERLACED) != 0) {
          continue;
        }
        if (seen.insert({mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency}).second) {
          result.push_back({{"width", mode.dmPelsWidth}, {"height", mode.dmPelsHeight}, {"refresh_hz", mode.dmDisplayFrequency}});
        }
      }
      return result;
    }
#endif
  }  // namespace

  void report_capture(const std::string &name, const std::vector<std::string> &available) {
    std::lock_guard lock(state_mutex);
    captured_name = name;
    capture_names = available;
  }

  int select_capture_index(const std::string &name, const std::vector<std::string> &available, int current) {
    if (available.empty()) {
      return -1;
    }
    const auto found = std::find(available.begin(), available.end(), name);
    return found == available.end() ? std::clamp(current, 0, static_cast<int>(available.size()) - 1) : static_cast<int>(found - available.begin());
  }

  int resolve_switch(int fallback, const std::vector<std::string> &available, int current) {
    std::lock_guard lock(state_mutex);
    if (pending_name) {
      const auto selected = select_capture_index(*pending_name, available, current);
      pending_name.reset();
      return selected;
    }
    if (available.empty()) {
      return -1;
    }
    return std::clamp(fallback, 0, static_cast<int>(available.size()) - 1);
  }

  nlohmann::json inventory() {
    std::lock_guard operation(operation_mutex);
    nlohmann::json result = {{"status", true}, {"version", 1}, {"devices", nlohmann::json::array()}};
    std::string captured;
    {
      std::lock_guard lock(state_mutex);
      captured = captured_name;
    }
    for (const auto &device : display_device::enumerate_devices()) {
      nlohmann::json row = {
        {"device_id", device.m_device_id},
        {"name", device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name},
        {"display_name", device.m_display_name},
        {"active", device.m_info.has_value()},
        {"capturing", !captured.empty() && captured == device.m_display_name},
        {"primary", device.m_info && device.m_info->m_primary},
        {"modes", nlohmann::json::array()},
        {"scale_options", nlohmann::json::array()},
        {"scale_percent", nullptr},
        {"hdr_supported", false}
      };
      if (device.m_info) {
        const auto &info = *device.m_info;
        row["width"] = info.m_resolution.m_width;
        row["height"] = info.m_resolution.m_height;
        row["refresh_hz"] = number(info.m_refresh_rate);
        row["hdr_supported"] = info.m_hdr_state.has_value();
        row["hdr_enabled"] = info.m_hdr_state == display_device::HdrState::Enabled;
#ifdef _WIN32
        row["modes"] = modes(device.m_display_name);
        DEVMODEW current_mode {};
        current_mode.dmSize = sizeof(current_mode);
        const std::wstring wide_name(device.m_display_name.begin(), device.m_display_name.end());
        if (EnumDisplaySettingsExW(wide_name.c_str(), ENUM_CURRENT_SETTINGS, &current_mode, 0)) {
          row["mode_refresh_hz"] = current_mode.dmDisplayFrequency;
        }
        if (const auto scale = query_scale(device.m_display_name)) {
          row["scale_percent"] = scales.at(scale->current - scale->minimum);
          for (int i = 0; i <= scale->maximum - scale->minimum; ++i) {
            row["scale_options"].push_back(scales.at(i));
          }
        }
#endif
      }
      result["devices"].push_back(std::move(row));
    }
    return result;
  }

  nlohmann::json apply(const nlohmann::json &request) {
    std::lock_guard operation(operation_mutex);
    const auto error = [](const std::string &message) {
      return nlohmann::json {{"status", false}, {"message", message}};
    };
    try {
      const auto id = request.at("device_id").get<std::string>();
      const auto action = request.at("action").get<std::string>();
      const auto devices = display_device::enumerate_devices();
      const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto &d) {
        return d.m_device_id == id && d.m_info.has_value();
      });
      if (device == devices.end()) {
        return error("Display is no longer active. Refresh the display list.");
      }
      if (action == "capture") {
        std::lock_guard lock(state_mutex);
        if (std::find(capture_names.begin(), capture_names.end(), device->m_display_name) == capture_names.end()) {
          return error("This display is unavailable to the current capture backend.");
        }
        pending_name = device->m_display_name;
        mail::man->event<int>(mail::switch_display)->raise(0);
        return {{"status", true}, {"pending", true}};
      }
#ifdef _WIN32
      display_device::WinDisplayDevice api(std::make_shared<display_device::WinApiLayer>());
      if (action == "mode") {
        const auto width = request.at("width").get<unsigned int>();
        const auto height = request.at("height").get<unsigned int>();
        const auto refresh = request.at("refresh_hz").get<unsigned int>();
        const auto supported = modes(device->m_display_name);
        const bool valid = std::any_of(supported.begin(), supported.end(), [&](const auto &mode) {
          return mode.at("width") == width && mode.at("height") == height && mode.at("refresh_hz") == refresh;
        });
        if (!valid) {
          return error("The requested resolution and refresh rate are unavailable.");
        }
        if (!api.setDisplayModes({{id, {{width, height}, {refresh, 1}}}})) {
          return error("Windows rejected the display mode.");
        }
      } else if (action == "hdr") {
        if (!device->m_info->m_hdr_state) {
          return error("HDR is unavailable on this display.");
        }
        const auto state = request.at("enabled").get<bool>() ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
        if (!api.setHdrStates({{id, state}})) {
          return error("Windows rejected the HDR change.");
        }
      } else if (action == "scale") {
        const auto scale = query_scale(device->m_display_name);
        const int percent = request.at("percent").get<int>();
        if (!scale) {
          return error("Per-monitor scaling is unavailable.");
        }
        const auto found = std::find(scales.begin(), scales.end(), percent);
        const int index = static_cast<int>(found - scales.begin());
        if (found == scales.end() || index > scale->maximum - scale->minimum) {
          return error("The requested scale is unavailable.");
        }
        scale_set_t update;
        update.header = scale->header;
        update.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);
        update.header.size = sizeof(update);
        update.relative = index + scale->minimum;
        if (DisplayConfigSetDeviceInfo(&update.header) != ERROR_SUCCESS) {
          return error("Windows rejected the scale change.");
        }
      } else {
        return error("Unknown display action.");
      }
      return {{"status", true}};
#else
      return error("Display changes are unavailable on this platform.");
#endif
    } catch (const nlohmann::json::exception &) {
      return error("Invalid display request.");
    }
  }
}  // namespace display_control
