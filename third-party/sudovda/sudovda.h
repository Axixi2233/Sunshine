#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <setupapi.h>
#include <vector>

#include "sudovda-ioctl.h"

#ifdef __cplusplus
namespace SUDOVDA {
#endif

static const HANDLE OpenDevice(const GUID *interface_guid) {
  HDEVINFO device_info_set = SetupDiGetClassDevs(interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (device_info_set == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE handle = INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA device_interface_data;
  ZeroMemory(&device_interface_data, sizeof(device_interface_data));
  device_interface_data.cbSize = sizeof(device_interface_data);

  for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info_set, nullptr, interface_guid, index, &device_interface_data); ++index) {
    DWORD detail_size = 0;
    SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data, nullptr, 0, &detail_size, nullptr);

    auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(calloc(1, detail_size));
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

    if (SetupDiGetDeviceInterfaceDetailA(device_info_set, &device_interface_data, detail, detail_size, &detail_size, nullptr)) {
      handle = CreateFileA(
        detail->DevicePath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH,
        nullptr
      );

      if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        free(detail);
        break;
      }
    }

    free(detail);
  }

  SetupDiDestroyDeviceInfoList(device_info_set);
  return handle;
}

static const bool AddVirtualDisplay(HANDLE device, UINT width, UINT height, UINT refresh_rate, const GUID &monitor_guid, const CHAR *device_name, const CHAR *serial_number, VIRTUAL_DISPLAY_ADD_OUT &output) {
  VIRTUAL_DISPLAY_ADD_PARAMS params {width, height, refresh_rate, monitor_guid, {}, {}};
  strncpy(params.DeviceName, device_name, 13);
  strncpy(params.SerialNumber, serial_number, 13);

  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_ADD_VIRTUAL_DISPLAY, &params, sizeof(params), &output, sizeof(output), &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] AddVirtualDisplay failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool RemoveVirtualDisplay(HANDLE device, const GUID &monitor_guid) {
  VIRTUAL_DISPLAY_REMOVE_PARAMS params {monitor_guid};
  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_REMOVE_VIRTUAL_DISPLAY, &params, sizeof(params), nullptr, 0, &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] RemoveVirtualDisplay failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool SetRenderAdapter(HANDLE device, const LUID &adapter_luid) {
  VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS params {adapter_luid};
  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_SET_RENDER_ADAPTER, &params, sizeof(params), nullptr, 0, &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] SetRenderAdapter failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool GetWatchdogTimeout(HANDLE device, VIRTUAL_DISPLAY_GET_WATCHDOG_OUT &output) {
  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_GET_WATCHDOG, nullptr, 0, &output, sizeof(output), &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] GetWatchdogTimeout failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool GetProtocolVersion(HANDLE device, VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT &output) {
  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_GET_PROTOCOL_VERSION, nullptr, 0, &output, sizeof(output), &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] GetProtocolVersion failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool isProtocolCompatible(const SUVDA_PROTOCAL_VERSION &other_version) {
  if (VDAProtocolVersion.Major != other_version.Major) {
    return false;
  }

  return VDAProtocolVersion.Minor <= other_version.Minor;
}

static const bool CheckProtocolCompatible(HANDLE device) {
  VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT protocol_version;
  return GetProtocolVersion(device, protocol_version) && isProtocolCompatible(protocol_version.Version);
}

static const bool PingDriver(HANDLE device) {
  DWORD bytes_returned;
  BOOL success = DeviceIoControl(device, IOCTL_DRIVER_PING, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
  if (!success) {
    std::cerr << "[SUDOVDA] PingDriver failed: " << GetLastError() << std::endl;
  }

  return success;
}

static const bool GetAddedDisplayName(const VIRTUAL_DISPLAY_ADD_OUT &added_display, wchar_t *device_name) {
  UINT path_count;
  UINT mode_count;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count)) {
    return false;
  }

  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr)) {
    return false;
  }

  auto path = std::find_if(paths.begin(), paths.end(), [&added_display](const auto &current_path) {
    return current_path.targetInfo.id == added_display.TargetId;
  });
  if (path == paths.end()) {
    return false;
  }

  DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
  source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
  source_name.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
  source_name.header.adapterId = added_display.AdapterLuid;
  source_name.header.id = path->sourceInfo.id;
  if (DisplayConfigGetDeviceInfo(reinterpret_cast<DISPLAYCONFIG_DEVICE_INFO_HEADER *>(&source_name))) {
    return false;
  }

  wcscpy_s(device_name, CCHDEVICENAME, source_name.viewGdiDeviceName);
  return true;
}

#ifdef __cplusplus
}  // namespace SUDOVDA
#endif
