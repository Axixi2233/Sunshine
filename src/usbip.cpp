/**
 * @file src/usbip.cpp
 * @brief Validate local USB/IP peers and manage the bundled Windows importer process.
 */
#include "usbip.h"

#include "logging.h"

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
  #include <iphlpapi.h>
  #include <shellapi.h>
  #include <windows.h>
#endif

namespace usbip {
  namespace {
    std::mutex driver_mutex;  ///< Serialize maintenance and importer startup checks.
#ifdef _WIN32
    HANDLE repair_process = nullptr;  ///< Elevated helper retained until its exit status is consumed.
#endif
    std::string repair_result;  ///< Last repair result, retained across Web UI refreshes.

    /** @brief Resolve the helper beside the actual running executable.
     * @return Fixed bundled helper path, or an empty path if unavailable.
     */
    std::filesystem::path driver_helper() {
#ifdef _WIN32
      std::array<wchar_t, 32768> executable {};
      const auto size = GetModuleFileNameW(nullptr, executable.data(), executable.size());
      if (size && size < executable.size()) {
        return std::filesystem::path(executable.data()).parent_path() / "tools" / "usbip-reconnect.exe";
      }
#endif
      return {};
    }

    /** @brief Consume a completed repair process under driver_mutex.
     * @return Whether an elevated repair is still running.
     */
    bool repair_busy() {
#ifdef _WIN32
      if (repair_process) {
        if (WaitForSingleObject(repair_process, 0) == WAIT_TIMEOUT) {
          return true;
        }
        DWORD code = 1;
        GetExitCodeProcess(repair_process, &code);
        CloseHandle(repair_process);
        repair_process = nullptr;
        repair_result = code == 0 ? "success" : code == 3010 ? "reboot_required" :
                                              code == 5      ? "admin_required" :
                                                               "failed";
      }
#endif
      return false;
    }

    /** @brief Run the read-only status helper with bounded time and output.
     * @return Driver classification, including unknown when inspection fails.
     */
    nlohmann::json inspect_driver() {
#ifdef _WIN32
      const auto helper = driver_helper();
      if (!std::filesystem::exists(helper)) {
        return {{"state", "helper_missing"}};
      }
      SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
      HANDLE read = nullptr, write = nullptr;
      if (!CreatePipe(&read, &write, &security, 4096)) {
        return {{"state", "unknown"}};
      }
      SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdOutput = write;
      startup.hStdError = write;
      PROCESS_INFORMATION process {};
      auto command = L"\"" + helper.wstring() + L"\" --driver-status";
      const bool launched = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &process);
      CloseHandle(write);
      std::string output;
      if (launched) {
        if (WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0) {
          std::array<char, 4096> buffer {};
          DWORD count = 0;
          if (ReadFile(read, buffer.data(), buffer.size(), &count, nullptr)) {
            output.assign(buffer.data(), count);
          }
        } else {
          TerminateProcess(process.hProcess, 1);  // Read-only inspection has no driver operations to interrupt.
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
      }
      CloseHandle(read);
      auto result = nlohmann::json::parse(output, nullptr, false);
      if (result.is_object() && result.contains("state")) {
        return result;
      }
#endif
      return {{"state", "unknown"}};
    }
  }  // namespace

  nlohmann::json driver_status() {
    std::lock_guard lock(driver_mutex);
    const bool busy = repair_busy();
    auto result = busy ? nlohmann::json {{"state", "checking"}} : inspect_driver();
    result["repairing"] = busy;
    result["repair_result"] = repair_result;
    return result;
  }

  nlohmann::json repair_driver() {
    std::lock_guard lock(driver_mutex);
    if (repair_busy()) {
      return {{"status", false}, {"error", "busy"}};
    }
#ifdef _WIN32
    if (inspect_driver().value("state", "unknown") == "healthy") {
      repair_result = "success";
      return {{"status", true}};
    }
    const auto helper = driver_helper();
    if (!std::filesystem::exists(helper)) {
      return {{"status", false}, {"error", "helper_missing"}};
    }
    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"runas";
    execute.lpFile = helper.c_str();
    execute.lpParameters = L"--repair-driver";
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
      repair_result = GetLastError() == ERROR_CANCELLED ? "admin_cancelled" : "failed";
      return {{"status", false}, {"error", repair_result}};
    }
    repair_process = execute.hProcess;
    repair_result.clear();
    return {{"status", true}};
#else
    return {{"status", false}, {"error", "unsupported"}};
#endif
  }

  std::optional<std::string> session_target(bool requested, const std::string &peer, bool ipv6_on_link, bool encrypted) {
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
        const auto bytes = v6.to_bytes();
        if ((bytes[0] & 0xfe) == 0xfc || (v6.is_link_local() && v6.scope_id() != 0) || ((bytes[0] & 0xe0) == 0x20 && (ipv6_on_link || encrypted))) {
          return address.to_string();
        }
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
   * @brief Check the OS route so global IPv6 addresses remain restricted to the local link.
   * @param peer Actual streaming peer, with a local scope ID for link-local addresses.
   * @return Whether Windows has a direct, non-default IPv6 route to the peer.
   */
  static bool ipv6_on_link(const std::string &peer) {
#ifdef _WIN32
    boost::system::error_code error;
    const auto address = boost::asio::ip::make_address(peer, error);
    if (!error && address.is_v6()) {
      const auto v6 = address.to_v6();
      const auto bytes = v6.to_bytes();
      SOCKADDR_INET destination {};
      destination.Ipv6.sin6_family = AF_INET6;
      destination.Ipv6.sin6_scope_id = v6.scope_id();
      std::copy(bytes.begin(), bytes.end(), destination.Ipv6.sin6_addr.u.Byte);
      MIB_IPFORWARD_ROW2 route {};
      SOCKADDR_INET source {};
      if (GetBestRoute2(nullptr, 0, nullptr, &destination, 0, &route, &source) == NO_ERROR) {
        const auto &next = route.NextHop.Ipv6.sin6_addr;
        return route.DestinationPrefix.PrefixLength > 0 && IN6_IS_ADDR_UNSPECIFIED(&next);
      }
    }
#endif
    return false;
  }

  bool is_lan_peer(const std::string &peer) {
    return session_target(true, peer, ipv6_on_link(peer)).has_value();
  }

  std::optional<std::string> import_source(bool requested, const std::string &peer, bool encrypted) {
    const auto target = session_target(requested, peer, requested && ipv6_on_link(peer), encrypted);
    if (!target || target->find(':') == std::string::npos) {
      return std::nullopt;
    }
    boost::asio::io_context context;
    boost::system::error_code error;
    boost::asio::ip::udp::socket socket(context);
    socket.open(boost::asio::ip::udp::v6(), error);
    if (error) {
      return std::nullopt;
    }
    // UDP connect queries source selection without transmitting a packet.
    socket.connect({boost::asio::ip::make_address(*target), 3240}, error);
    if (error) {
      return std::nullopt;
    }
    const auto local = socket.local_endpoint(error).address();
    if (error || !local.is_v6() || local.is_unspecified() || local.is_loopback()) {
      return std::nullopt;
    }
    return local.to_string();
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

  std::shared_ptr<session_t> start(bool requested, const std::string &peer, std::uint32_t launch_id, const std::string &client_cert, const std::string &token) {
    std::unique_lock maintenance(driver_mutex, std::try_to_lock);
    if (requested && (!maintenance.owns_lock() || repair_busy())) {
      BOOST_LOG(warning) << "USB/IP driver maintenance is in progress; ordinary streaming remains available";
      return {};
    }
    if (maintenance.owns_lock()) {
      maintenance.unlock();
    }
    const bool encrypted = token.size() == 64 && !client_cert.empty();
    const auto target = session_target(requested, peer, requested && ipv6_on_link(peer), encrypted);
    if (!target) {
      if (requested) {
        BOOST_LOG(warning) << "USB/IP peer " << peer << " requires LAN connectivity or paired TLS for public IPv6";
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
    auto command = L"\"" + helper.wstring() + (encrypted ? L"\" --watch-tls " : L"\" --watch ") + std::wstring(target->begin(), target->end()) + L" " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(launch_id);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    HANDLE bootstrap_read = nullptr, bootstrap_write = nullptr, null_output = INVALID_HANDLE_VALUE;
    if (encrypted) {
      SECURITY_ATTRIBUTES security {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
      if (!CreatePipe(&bootstrap_read, &bootstrap_write, &security, 8192)) {
        CloseHandle(stop);
        return {};
      }
      SetHandleInformation(bootstrap_write, HANDLE_FLAG_INHERIT, 0);
      null_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
      if (null_output == INVALID_HANDLE_VALUE) {
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(stop);
        return {};
      }
      startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = bootstrap_read;
      startup.hStdOutput = null_output;
      startup.hStdError = null_output;
    }
    PROCESS_INFORMATION process {};
    const bool created = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, encrypted, CREATE_NO_WINDOW, nullptr, helper.parent_path().c_str(), &startup, &process);
    if (encrypted) {
      CloseHandle(bootstrap_read);
      CloseHandle(null_output);
      if (created) {
        const auto bootstrap = nlohmann::json {{"token", token}, {"certificate", client_cert}}.dump();
        DWORD written = 0;
        if (bootstrap.size() > 4096 || !WriteFile(bootstrap_write, bootstrap.data(), bootstrap.size(), &written, nullptr) || written != bootstrap.size()) {
          SetEvent(stop);
          BOOST_LOG(error) << "Could not pass private USB/IP TLS bootstrap to helper";
        }
      }
      CloseHandle(bootstrap_write);
    }
    if (!created) {
      BOOST_LOG(error) << "Failed to start USB/IP importer: " << GetLastError();
      CloseHandle(stop);
      return {};
    }
    CloseHandle(process.hThread);
    const auto endpoint = target->find(':') == std::string::npos ? *target : "[" + *target + "]";
    BOOST_LOG(info) << "USB/IP importer started for " << endpoint << ":3240/1-1, session " << launch_id
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
