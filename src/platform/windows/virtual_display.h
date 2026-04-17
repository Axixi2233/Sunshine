/**
 * @file src/platform/windows/virtual_display.h
 * @brief Minimal SudoVDA virtual display helpers for Sunshine.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <windows.h>

namespace VDISPLAY {
  enum class DRIVER_STATUS {
    UNKNOWN = 1,
    OK = 0,
    FAILED = -1,
    VERSION_INCOMPATIBLE = -2,
    WATCHDOG_FAILED = -3,
  };

  void closeVDisplayDevice();
  DRIVER_STATUS openVDisplayDevice();
  bool startPingThread(std::function<void()> fail_cb);
  bool setRenderAdapterByName(const std::wstring &adapter_name);
  std::wstring createVirtualDisplay(
    const char *serial_number,
    const char *device_name,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_rate,
    const GUID &guid
  );
  bool removeVirtualDisplay(const GUID &guid);
  LONG changeDisplaySettings(const wchar_t *device_name, int width, int height, int refresh_rate);
  bool setDisplayHDRByName(const wchar_t *display_name, bool enable_advanced_color);
}  // namespace VDISPLAY
