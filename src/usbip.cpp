/**
 * @file src/usbip.cpp
 * @brief Validate local USB/IP peers and manage the bundled Windows importer process.
 */
#include "usbip.h"

#include "logging.h"

#include <array>
#include <boost/asio/ip/address.hpp>
#include <filesystem>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace usbip {
  std::optional<std::string> session_target(bool requested, const std::string &peer) {
    if (!requested) {
      return std::nullopt;
    }
    boost::system::error_code error;
    auto address = boost::asio::ip::make_address(peer, error);
    if (error) {
      return std::nullopt;
    }
    if (address.is_v6()) {
      const auto v6 = address.to_v6();
      if (!v6.is_v4_mapped()) {
        return std::nullopt;
      }
      const auto bytes = v6.to_bytes();
      address = boost::asio::ip::address_v4({bytes[12], bytes[13], bytes[14], bytes[15]});
    }
    const auto bytes = address.to_v4().to_bytes();
    if (bytes[0] == 10 || (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) || (bytes[0] == 192 && bytes[1] == 168)) {
      return address.to_string();
    }
    return std::nullopt;
  }

  /**
   * @brief Own the importer process and the cancellation event for exactly one stream.
   */
  class session_t {
  public:
#ifdef _WIN32
    /**
     * @brief Adopt the successfully launched helper and its private stop event.
     * @param process Importer process handle.
     * @param stop Session cancellation event.
     */
    session_t(HANDLE process, HANDLE stop):
        process_(process),
        stop_(stop) {}

    /**
     * @brief Release imported devices before closing the process handles.
     */
    ~session_t() {
      request_stop();
      if (WaitForSingleObject(process_, 5000) == WAIT_TIMEOUT) {
        BOOST_LOG(warning) << "USB/IP helper is finishing an SDK operation; its session cancellation remains signaled";
      }
      CloseHandle(process_);
      CloseHandle(stop_);
    }

    /**
     * @brief Notify the helper that its stream ended.
     */
    void request_stop() {
      SetEvent(stop_);
    }

  private:
    HANDLE process_;  ///< Running helper process.
    HANDLE stop_;  ///< Session-owned manual-reset event.
#else
    /** @brief Non-Windows builds have no importer process. */
    void request_stop() {}
#endif
  };

  std::shared_ptr<session_t> start(bool requested, const std::string &peer, std::uint32_t launch_id) {
    const auto target = session_target(requested, peer);
    if (!target) {
      if (requested) {
        BOOST_LOG(warning) << "USB/IP requires a private IPv4 streaming peer";
      }
      return {};
    }
#ifdef _WIN32
    std::array<wchar_t, 32768> executable {};
    const auto size = GetModuleFileNameW(nullptr, executable.data(), executable.size());
    if (size == 0 || size >= executable.size()) {
      return {};
    }
    const auto helper = std::filesystem::path(executable.data()).parent_path() / "tools" / "usbip-reconnect.exe";
    std::error_code file_error;
    if (!std::filesystem::exists(helper, file_error)) {
      BOOST_LOG(error) << "Bundled USB/IP helper is missing: " << helper.string();
      return {};
    }
    const auto identity = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(launch_id);
    const auto event_name = L"Local\\StarCoreUsbIpStop-" + identity;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, event_name.c_str());
    if (!stop) {
      return {};
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(stop);
      BOOST_LOG(warning) << "USB/IP importer already exists for launch session " << launch_id;
      return {};
    }
    auto command = L"\"" + helper.wstring() + L"\" --watch " + std::wstring(target->begin(), target->end()) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(launch_id);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process {};
    if (!CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &process)) {
      BOOST_LOG(error) << "Failed to start USB/IP importer: " << GetLastError();
      CloseHandle(stop);
      return {};
    }
    CloseHandle(process.hThread);
    BOOST_LOG(info) << "USB/IP importer started for " << *target << ":3240/1-1, session " << launch_id
                    << ", PID " << process.dwProcessId;
    return std::make_shared<session_t>(process.hProcess, stop);
#else
    return {};
#endif
  }

  void request_stop(const std::shared_ptr<session_t> &session) {
    if (session) {
      session->request_stop();
    }
  }
}  // namespace usbip
