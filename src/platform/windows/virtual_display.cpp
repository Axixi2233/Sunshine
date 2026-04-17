/**
 * @file src/platform/windows/virtual_display.cpp
 * @brief Minimal SudoVDA virtual display helpers for Sunshine.
 */
#include "virtual_display.h"

#include <algorithm>
#include <thread>
#include <vector>

#include <dxgi.h>
#include <setupapi.h>
#include <wrl/client.h>

#include <sudovda/sudovda.h>

namespace VDISPLAY {
  using namespace SUDOVDA;

  namespace {
    HANDLE DRIVER_HANDLE {INVALID_HANDLE_VALUE};

    bool findDisplayIds(const wchar_t *display_name, LUID &adapter_id, uint32_t &target_id) {
      UINT32 path_count;
      UINT32 mode_count;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return false;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
      }

      const auto path = std::find_if(paths.begin(), paths.end(), [display_name](const DISPLAYCONFIG_PATH_INFO &path_info) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
        source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source_name.header.size = sizeof(source_name);
        source_name.header.adapterId = path_info.sourceInfo.adapterId;
        source_name.header.id = path_info.sourceInfo.id;

        return DisplayConfigGetDeviceInfo(&source_name.header) == ERROR_SUCCESS &&
               std::wstring_view(display_name) == source_name.viewGdiDeviceName;
      });
      if (path == paths.end()) {
        return false;
      }

      adapter_id = path->sourceInfo.adapterId;
      target_id = path->targetInfo.id;
      return true;
    }
  }  // namespace

  void closeVDisplayDevice() {
    if (DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return;
    }

    CloseHandle(DRIVER_HANDLE);
    DRIVER_HANDLE = INVALID_HANDLE_VALUE;
  }

  DRIVER_STATUS openVDisplayDevice() {
    uint32_t retry_interval = 20;
    while (true) {
      DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
        break;
      }

      if (retry_interval > 320) {
        return DRIVER_STATUS::FAILED;
      }

      retry_interval *= 2;
      Sleep(retry_interval);
    }

    if (!CheckProtocolCompatible(DRIVER_HANDLE)) {
      closeVDisplayDevice();
      return DRIVER_STATUS::VERSION_INCOMPATIBLE;
    }

    return DRIVER_STATUS::OK;
  }

  bool startPingThread(std::function<void()> fail_cb) {
    if (DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdog_out;
    if (!GetWatchdogTimeout(DRIVER_HANDLE, watchdog_out)) {
      return false;
    }

    if (!watchdog_out.Timeout) {
      return true;
    }

    const auto sleep_interval = watchdog_out.Timeout * 1000 / 3;
    std::thread ping_thread([sleep_interval, fail_cb = std::move(fail_cb)]() mutable {
      uint8_t fail_count = 0;
      for (;;) {
        if (!PingDriver(DRIVER_HANDLE)) {
          ++fail_count;
          if (fail_count > 3) {
            fail_cb();
            return;
          }
        } else {
          fail_count = 0;
        }

        Sleep(sleep_interval);
      }
    });

    ping_thread.detach();
    return true;
  }

  bool setRenderAdapterByName(const std::wstring &adapter_name) {
    if (DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    for (UINT index = 0; SUCCEEDED(factory->EnumAdapters(index, &adapter)); ++index) {
      if (FAILED(adapter->GetDesc(&desc))) {
        adapter.Reset();
        continue;
      }

      if (std::wstring_view(desc.Description) == std::wstring_view(adapter_name)) {
        return SetRenderAdapter(DRIVER_HANDLE, desc.AdapterLuid);
      }

      adapter.Reset();
    }

    return false;
  }

  std::wstring createVirtualDisplay(
    const char *serial_number,
    const char *device_name,
    uint32_t width,
    uint32_t height,
    uint32_t refresh_rate,
    const GUID &guid
  ) {
    if (DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return {};
    }

    VIRTUAL_DISPLAY_ADD_OUT output;
    if (!AddVirtualDisplay(DRIVER_HANDLE, width, height, refresh_rate, guid, device_name, serial_number, output)) {
      return {};
    }

    uint32_t retry_interval = 20;
    wchar_t display_name[CCHDEVICENAME] {};
    while (!GetAddedDisplayName(output, display_name)) {
      Sleep(retry_interval);
      if (retry_interval > 320) {
        return {};
      }

      retry_interval *= 2;
    }

    return display_name;
  }

  bool removeVirtualDisplay(const GUID &guid) {
    return DRIVER_HANDLE != INVALID_HANDLE_VALUE && SUDOVDA::RemoveVirtualDisplay(DRIVER_HANDLE, guid);
  }

  LONG changeDisplaySettings(const wchar_t *device_name, int width, int height, int refresh_rate) {
    DEVMODEW device_mode {};
    device_mode.dmSize = sizeof(device_mode);

    if (!EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &device_mode)) {
      return ERROR_INVALID_PARAMETER;
    }

    DWORD target_refresh_rate = refresh_rate / 1000;
    DWORD fallback_refresh_rate = target_refresh_rate;

    if (refresh_rate % 1000) {
      if (refresh_rate % 1000 >= 900) {
        ++target_refresh_rate;
      } else {
        ++fallback_refresh_rate;
      }
    } else if (fallback_refresh_rate > 1) {
      --fallback_refresh_rate;
    }

    device_mode.dmPelsWidth = width;
    device_mode.dmPelsHeight = height;
    device_mode.dmDisplayFrequency = target_refresh_rate;
    device_mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    auto result = ChangeDisplaySettingsExW(device_name, &device_mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
    if (result == ERROR_SUCCESS) {
      return result;
    }

    device_mode.dmDisplayFrequency = fallback_refresh_rate;
    return ChangeDisplaySettingsExW(device_name, &device_mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
  }

  bool setDisplayHDRByName(const wchar_t *display_name, bool enable_advanced_color) {
    LUID adapter_id {};
    uint32_t target_id {};
    if (!findDisplayIds(display_name, adapter_id, target_id)) {
      return false;
    }

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE set_hdr_info {};
    set_hdr_info.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    set_hdr_info.header.size = sizeof(set_hdr_info);
    set_hdr_info.header.adapterId = adapter_id;
    set_hdr_info.header.id = target_id;
    set_hdr_info.enableAdvancedColor = enable_advanced_color;

    return DisplayConfigSetDeviceInfo(&set_hdr_info.header) == ERROR_SUCCESS;
  }
}  // namespace VDISPLAY
