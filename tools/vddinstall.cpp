/**
 * @file tools/vddinstall.cpp
 * @brief Installs or removes the SudoVDA root-enumerated display device.
 */

#include <windows.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <regstr.h>

namespace {
  constexpr GUID DISPLAY_CLASS_GUID {
    0x4D36E968, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}
  };
}

namespace {
  constexpr wchar_t DRIVER_HARDWARE_ID[] = L"Root\\SudoMaker\\SudoVDA";
  constexpr wchar_t ALT_DRIVER_HARDWARE_ID[] = L"SudoVDA";

  class devinfo_handle_t {
  public:
    explicit devinfo_handle_t(HDEVINFO handle):
        handle_(handle) {}

    ~devinfo_handle_t() {
      if (handle_ != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(handle_);
      }
    }

    devinfo_handle_t(const devinfo_handle_t &) = delete;
    devinfo_handle_t &operator=(const devinfo_handle_t &) = delete;

    [[nodiscard]] HDEVINFO get() const {
      return handle_;
    }

    [[nodiscard]] explicit operator bool() const {
      return handle_ != INVALID_HANDLE_VALUE;
    }

  private:
    HDEVINFO handle_ {INVALID_HANDLE_VALUE};
  };

  [[nodiscard]] std::wstring to_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
  }

  [[nodiscard]] bool is_target_hardware_id(const std::wstring &hardware_id) {
    const auto normalized {to_lower(hardware_id)};
    return normalized == to_lower(DRIVER_HARDWARE_ID) || normalized == to_lower(ALT_DRIVER_HARDWARE_ID);
  }

  void print_last_error(const std::wstring &context, DWORD error = GetLastError()) {
    std::wcerr << context << L" failed with error " << error;

    LPWSTR message_buffer = nullptr;
    const auto flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&message_buffer), 0, nullptr) && message_buffer) {
      std::wstring message {message_buffer};
      while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
      }
      std::wcerr << L": " << message;
      LocalFree(message_buffer);
    }

    std::wcerr << std::endl;
  }

  [[nodiscard]] std::vector<std::wstring> get_hardware_ids(HDEVINFO devinfo, SP_DEVINFO_DATA &device_info_data) {
    DWORD required_size = 0;
    SetupDiGetDeviceRegistryPropertyW(devinfo, &device_info_data, SPDRP_HARDWAREID, nullptr, nullptr, 0, &required_size);
    if (required_size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return {};
    }

    std::vector<BYTE> buffer(required_size);
    if (!SetupDiGetDeviceRegistryPropertyW(devinfo, &device_info_data, SPDRP_HARDWAREID, nullptr, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)) {
      return {};
    }

    std::vector<std::wstring> hardware_ids;
    const auto *current = reinterpret_cast<const wchar_t *>(buffer.data());
    while (*current != L'\0') {
      hardware_ids.emplace_back(current);
      current += hardware_ids.back().size() + 1;
    }

    return hardware_ids;
  }

  [[nodiscard]] bool remove_matching_devices() {
    devinfo_handle_t devinfo(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES));
    if (!devinfo) {
      print_last_error(L"SetupDiGetClassDevsW");
      return false;
    }

    bool success = true;
    bool removed_any = false;

    for (DWORD index = 0;; ++index) {
      SP_DEVINFO_DATA device_info_data {};
      device_info_data.cbSize = sizeof(device_info_data);
      if (!SetupDiEnumDeviceInfo(devinfo.get(), index, &device_info_data)) {
        if (GetLastError() == ERROR_NO_MORE_ITEMS) {
          break;
        }

        print_last_error(L"SetupDiEnumDeviceInfo");
        success = false;
        break;
      }

      const auto hardware_ids = get_hardware_ids(devinfo.get(), device_info_data);
      const auto matches = std::any_of(hardware_ids.begin(), hardware_ids.end(), [](const std::wstring &hardware_id) {
        return is_target_hardware_id(hardware_id);
      });
      if (!matches) {
        continue;
      }

      SP_REMOVEDEVICE_PARAMS remove_params {};
      remove_params.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
      remove_params.ClassInstallHeader.InstallFunction = DIF_REMOVE;
      remove_params.Scope = DI_REMOVEDEVICE_GLOBAL;
      remove_params.HwProfile = 0;

      if (!SetupDiSetClassInstallParamsW(devinfo.get(), &device_info_data, &remove_params.ClassInstallHeader, sizeof(remove_params))) {
        print_last_error(L"SetupDiSetClassInstallParamsW");
        success = false;
        continue;
      }

      if (!SetupDiCallClassInstaller(DIF_REMOVE, devinfo.get(), &device_info_data)) {
        print_last_error(L"SetupDiCallClassInstaller(DIF_REMOVE)");
        success = false;
        continue;
      }

      removed_any = true;
    }

    if (removed_any) {
      std::wcout << L"Removed existing SudoVDA device node(s)." << std::endl;
    } else {
      std::wcout << L"No existing SudoVDA device nodes were present." << std::endl;
    }

    return success;
  }

  [[nodiscard]] bool create_root_device() {
    devinfo_handle_t devinfo(SetupDiCreateDeviceInfoList(&DISPLAY_CLASS_GUID, nullptr));
    if (!devinfo) {
      print_last_error(L"SetupDiCreateDeviceInfoList");
      return false;
    }

    SP_DEVINFO_DATA device_info_data {};
    device_info_data.cbSize = sizeof(device_info_data);
    if (!SetupDiCreateDeviceInfoW(devinfo.get(), L"Display", &DISPLAY_CLASS_GUID, nullptr, nullptr, DICD_GENERATE_ID, &device_info_data)) {
      print_last_error(L"SetupDiCreateDeviceInfoW");
      return false;
    }

    const wchar_t hardware_ids[] = L"Root\\SudoMaker\\SudoVDA\0\0";
    if (!SetupDiSetDeviceRegistryPropertyW(devinfo.get(), &device_info_data, SPDRP_HARDWAREID, reinterpret_cast<const BYTE *>(hardware_ids), sizeof(hardware_ids))) {
      print_last_error(L"SetupDiSetDeviceRegistryPropertyW");
      return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devinfo.get(), &device_info_data)) {
      print_last_error(L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE)");
      return false;
    }

    std::wcout << L"Created the SudoVDA root device node." << std::endl;
    return true;
  }

  [[nodiscard]] bool install_driver(const std::filesystem::path &inf_path) {
    const auto absolute_inf_path = std::filesystem::absolute(inf_path);
    if (!std::filesystem::exists(absolute_inf_path)) {
      std::wcerr << L"Driver INF was not found: " << absolute_inf_path.native() << std::endl;
      return false;
    }

    BOOL reboot_required = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, DRIVER_HARDWARE_ID, absolute_inf_path.c_str(), INSTALLFLAG_FORCE, &reboot_required)) {
      print_last_error(L"UpdateDriverForPlugAndPlayDevicesW");
      return false;
    }

    std::wcout << L"Installed the SudoVDA driver from " << absolute_inf_path.native() << L"." << std::endl;
    if (reboot_required) {
      std::wcout << L"A reboot is required to finish applying the driver." << std::endl;
    }
    return true;
  }

  int install_command(const std::filesystem::path &inf_path) {
    if (!remove_matching_devices()) {
      return 1;
    }

    if (!create_root_device()) {
      return 1;
    }

    return install_driver(inf_path) ? 0 : 1;
  }

  int uninstall_command() {
    return remove_matching_devices() ? 0 : 1;
  }
}  // namespace

int wmain(int argc, wchar_t **argv) {
  if (argc < 2) {
    std::wcerr << L"Usage: vddinstall.exe <install <path-to-inf> | uninstall>" << std::endl;
    return 1;
  }

  const std::wstring command {to_lower(argv[1])};
  if (command == L"install") {
    if (argc < 3) {
      std::wcerr << L"Missing INF path for install command." << std::endl;
      return 1;
    }

    return install_command(argv[2]);
  }

  if (command == L"uninstall") {
    return uninstall_command();
  }

  std::wcerr << L"Unsupported command: " << argv[1] << std::endl;
  return 1;
}
