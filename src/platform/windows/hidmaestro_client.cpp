/**
 * @file src/platform/windows/hidmaestro_client.cpp
 * @brief Sunshine client for the out-of-process HIDMaestro DualSense host.
 */

// platform includes
#include <Windows.h>

// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/filesystem.hpp>
#include <boost/process/v1/child.hpp>

// local includes
#include "hidmaestro_client.h"
#include "hidmaestro_protocol.h"
#include "src/logging.h"

namespace platf::hidmaestro {
  namespace bp = boost::process::v1;
  using namespace std::chrono_literals;
  using namespace std::literals;
  using protocol::message_type_e;

  namespace {
    constexpr std::size_t native_pcm_channels = 4;  ///< DualSense composite USB audio channel count.
    constexpr std::size_t native_pcm_signal_first_channel = 0;  ///< First speaker or voice-coil channel that keeps forwarding active.
    constexpr std::uint32_t native_pcm_silent_frame_limit = 48000 / 4;  ///< 250 ms of 48 kHz PCM.
    constexpr std::int16_t native_pcm_silence_threshold = 8;  ///< Ignore converter noise below this absolute amplitude.

  }  // namespace

  static_assert(DPAD_UP == 0x0001);
  static_assert(DPAD_DOWN == 0x0002);
  static_assert(DPAD_LEFT == 0x0004);
  static_assert(DPAD_RIGHT == 0x0008);
  static_assert(START == 0x0010);
  static_assert(BACK == 0x0020);
  static_assert(LEFT_STICK == 0x0040);
  static_assert(RIGHT_STICK == 0x0080);
  static_assert(LEFT_BUTTON == 0x0100);
  static_assert(RIGHT_BUTTON == 0x0200);
  static_assert(HOME == 0x0400);
  static_assert(A == 0x1000);
  static_assert(B == 0x2000);
  static_assert(X == 0x4000);
  static_assert(Y == 0x8000);
  static_assert(PADDLE1 == 0x010000);
  static_assert(PADDLE2 == 0x020000);
  static_assert(PADDLE3 == 0x040000);
  static_assert(PADDLE4 == 0x080000);
  static_assert(TOUCHPAD_BUTTON == 0x100000);
  static_assert(MISC_BUTTON == 0x200000);

  namespace {
    constexpr std::int32_t pending_status = std::numeric_limits<std::int32_t>::min();  ///< Status value used while awaiting a reply.

    /**
     * @brief Write an exact number of bytes to a Windows pipe.
     * @param pipe Pipe handle.
     * @param event Manual-reset event dedicated to serialized pipe writes.
     * @param data Bytes to write.
     * @param size Byte count.
     * @return True when all bytes were written.
     */
    bool write_exact(HANDLE pipe, HANDLE event, const void *data, std::size_t size) {
      const auto *cursor = static_cast<const std::uint8_t *>(data);
      while (size != 0) {
        ResetEvent(event);
        OVERLAPPED overlapped {};
        overlapped.hEvent = event;
        DWORD written = 0;
        if (!WriteFile(pipe, cursor, static_cast<DWORD>(size), &written, &overlapped)) {
          const auto error = GetLastError();
          if (error != ERROR_IO_PENDING || !GetOverlappedResult(pipe, &overlapped, &written, TRUE)) {
            return false;
          }
        }
        if (written == 0) {
          SetLastError(ERROR_BROKEN_PIPE);
          return false;
        }
        cursor += written;
        size -= written;
      }
      return true;
    }

    /**
     * @brief Read an exact number of bytes from a Windows pipe.
     * @param pipe Pipe handle.
     * @param event Manual-reset event dedicated to the pipe reader.
     * @param data Destination buffer.
     * @param size Byte count.
     * @return True when all bytes were read.
     */
    bool read_exact(HANDLE pipe, HANDLE event, void *data, std::size_t size) {
      auto *cursor = static_cast<std::uint8_t *>(data);
      while (size != 0) {
        ResetEvent(event);
        OVERLAPPED overlapped {};
        overlapped.hEvent = event;
        DWORD read = 0;
        if (!ReadFile(pipe, cursor, static_cast<DWORD>(size), &read, &overlapped)) {
          const auto error = GetLastError();
          if (error != ERROR_IO_PENDING || !GetOverlappedResult(pipe, &overlapped, &read, TRUE)) {
            return false;
          }
        }
        if (read == 0) {
          SetLastError(ERROR_BROKEN_PIPE);
          return false;
        }
        cursor += read;
        size -= read;
      }
      return true;
    }
  }  // namespace

  /**
   * @brief Private implementation of the HIDMaestro helper client.
   */
  class client_t::impl_t {
  public:
    /**
     * @brief Per-controller routing and duplicate-suppression state.
     */
    struct controller_t {
      feedback_queue_t feedback_queue;  ///< Queue returning output to Moonlight.
      std::uint8_t client_relative_index {};  ///< Controller index understood by Moonlight.
      bool allocated {};  ///< Whether this index contains a virtual DualSense.
      bool active {};  ///< Whether a Moonlight controller is currently bound to the virtual device.
      bool create_pending {};  ///< Whether the helper still owes a create reply.
      bool uses_composite {};  ///< Whether the virtual device exposes the native PCM audio profile.
      bool accepts_native_pcm {};  ///< Whether Moonlight can consume native controller PCM.
      bool logged_native_pcm {};  ///< Whether receipt of native PCM has been logged.
      bool native_pcm_paused {};  ///< Whether sustained silence currently suppresses PCM forwarding.
      std::uint32_t native_pcm_silent_frames {};  ///< Consecutive silent native PCM sample frames.
      std::int32_t create_status {pending_status};  ///< Latest create reply status.
      bool have_rumble {};  ///< Whether a rumble event has already been forwarded.
      protocol::rumble_payload_t last_rumble {};  ///< Most recently forwarded rumble.
      bool have_rgb {};  ///< Whether an RGB event has already been forwarded.
      protocol::rgb_payload_t last_rgb {};  ///< Most recently forwarded lightbar color.
      bool have_player_indicator {};  ///< Whether a player-indicator event has already been forwarded.
      protocol::player_indicator_payload_t last_player_indicator {};  ///< Most recently forwarded player-indicator mask.
      bool have_adaptive_triggers {};  ///< Whether an adaptive-trigger event has already been forwarded.
      protocol::adaptive_triggers_payload_t last_adaptive_triggers {};  ///< Most recently forwarded adaptive-trigger effect.
    };

    ~impl_t() {
      stop();
    }

    /**
     * @brief Launch and connect to the helper.
     * @return Zero on success.
     */
    int init() {
      const auto helper_path = boost::filesystem::current_path() / "tools" / "sunshine-hidmaestro-host.exe";
      if (!boost::filesystem::exists(helper_path)) {
        BOOST_LOG(error) << "HIDMaestro helper not found: "sv << helper_path.string();
        return -1;
      }

      pipe_name_ = "sunshine-hidmaestro-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
      const auto helper_path_wide = helper_path.wstring();
      const auto working_dir_wide = helper_path.parent_path().wstring();
      auto command_line = L'"' + helper_path_wide + L"\" --pipe \"" + std::wstring(pipe_name_.begin(), pipe_name_.end()) + L'"';
      STARTUPINFOW startup_info {};
      startup_info.cb = sizeof(startup_info);
      PROCESS_INFORMATION process_info {};
      if (!CreateProcessW(
            helper_path_wide.c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            working_dir_wide.c_str(),
            &startup_info,
            &process_info
          )) {
        BOOST_LOG(error) << "Failed to launch HIDMaestro helper: "sv << GetLastError();
        return -1;
      }
      try {
        child_ = std::make_unique<bp::child>(static_cast<bp::pid_t>(process_info.dwProcessId));
      } catch (...) {
        TerminateProcess(process_info.hProcess, 1);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        BOOST_LOG(error) << "Failed to attach to the HIDMaestro helper process"sv;
        return -1;
      }
      CloseHandle(process_info.hThread);
      CloseHandle(process_info.hProcess);
      BOOST_LOG(info) << "HIDMaestro helper running with PID "sv << child_->id();

      const auto pipe_path = L"\\\\.\\pipe\\" + std::wstring(pipe_name_.begin(), pipe_name_.end());
      const auto deadline = std::chrono::steady_clock::now() + 30s;
      while (std::chrono::steady_clock::now() < deadline) {
        pipe_ = CreateFileW(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
          break;
        }
        const auto pipe_error = GetLastError();
        if (pipe_error != ERROR_PIPE_BUSY && pipe_error != ERROR_FILE_NOT_FOUND) {
          break;
        }
        if (pipe_error == ERROR_PIPE_BUSY) {
          WaitNamedPipeW(pipe_path.c_str(), 250);
        } else {
          std::this_thread::sleep_for(25ms);
        }
      }
      if (pipe_ == INVALID_HANDLE_VALUE) {
        BOOST_LOG(error) << "Failed to connect to HIDMaestro helper pipe: "sv << GetLastError();
        stop();
        return -1;
      }

      read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (read_event_ == nullptr || write_event_ == nullptr) {
        BOOST_LOG(error) << "Failed to create HIDMaestro overlapped I/O events: "sv << GetLastError();
        stop();
        return -1;
      }

      running_ = true;
      reader_ = std::thread(&impl_t::reader_loop, this);

      std::unique_lock lock(state_mutex_);
      if (!state_changed_.wait_for(lock, 90s, [this]() {
            return ready_status_ != pending_status || !running_;
          })) {
        BOOST_LOG(error) << "Timed out while HIDMaestro initialized its driver"sv;
        lock.unlock();
        stop();
        return -1;
      }
      if (ready_status_ != 0) {
        BOOST_LOG(error) << "HIDMaestro helper initialization failed with status "sv << ready_status_;
        lock.unlock();
        stop();
        return -1;
      }
      lock.unlock();

      {
        std::lock_guard queue_lock(outbound_mutex_);
        accepting_writes_ = true;
        writer_stopping_ = false;
      }
      writer_ = std::thread(&impl_t::writer_loop, this);

      BOOST_LOG(info) << "HIDMaestro DualSense backend is ready"sv;
      return 0;
    }

    /**
     * @brief Stop the helper and release transport resources.
     */
    void stop() {
      {
        std::lock_guard lock(outbound_mutex_);
        accepting_writes_ = false;
        writer_stopping_ = true;
        outbound_queue_.clear();
      }
      outbound_changed_.notify_all();

      running_ = false;
      state_changed_.notify_all();
      if (pipe_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipe_, nullptr);
      }
      if (writer_.joinable()) {
        writer_.join();
      }
      if (reader_.joinable()) {
        reader_.join();
      }

      if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
      }
      if (read_event_ != nullptr) {
        CloseHandle(read_event_);
        read_event_ = nullptr;
      }
      if (write_event_ != nullptr) {
        CloseHandle(write_event_);
        write_event_ = nullptr;
      }

      if (child_) {
        std::error_code process_error;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (child_->running(process_error) && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(50ms);
        }
        if (child_->running(process_error)) {
          child_->terminate(process_error);
        }
        child_.reset();
      }
    }

    /**
     * @brief Return whether the helper completed initialization.
     * @return True when ready.
     */
    [[nodiscard]] bool available() const {
      std::lock_guard lock(state_mutex_);
      return running_ && ready_status_ == 0;
    }

    /**
     * @brief Send creation of a virtual controller without waiting for its result.
     *
     * @details Controller creation is a lifetime barrier and is written directly
     * to the pipe before ordinary input can be queued for the new controller.
     * HIDMaestro performs the slower PnP work in the helper process, while the
     * status reply remains asynchronous.
     *
     * @param id Sunshine controller identifiers.
     * @param feedback_queue Queue returning output to Moonlight.
     * @param enable_native_pcm Whether the client can consume native controller PCM.
     * @return Zero on success.
     */
    int alloc_gamepad(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool enable_native_pcm) {
      if (id.globalIndex < 0 || id.globalIndex >= static_cast<int>(controllers_.size())) {
        return -1;
      }
      if (!available()) {
        return -1;
      }

      feedback_queue_t rebound_feedback_queue;
      bool reused = false;
      bool waiting_for_create = false;
      bool recreate = false;
      {
        std::lock_guard lock(state_mutex_);
        auto &controller = controllers_[id.globalIndex];
        if (controller.active) {
          return -1;
        }
        if (protocol::can_reuse_virtual_dualsense(controller.allocated, controller.active, controller.uses_composite, enable_native_pcm)) {
          controller.feedback_queue = std::move(feedback_queue);
          controller.client_relative_index = id.clientRelativeIndex;
          controller.active = true;
          controller.accepts_native_pcm = enable_native_pcm;
          controller.logged_native_pcm = false;
          controller.native_pcm_paused = false;
          controller.native_pcm_silent_frames = 0;
          controller.have_rumble = false;
          controller.have_rgb = false;
          controller.have_player_indicator = false;
          controller.have_adaptive_triggers = false;
          rebound_feedback_queue = controller.feedback_queue;
          waiting_for_create = controller.create_pending;
          reused = true;
        } else if (controller.create_pending) {
          BOOST_LOG(warning) << "Cannot upgrade pending HIDMaestro controller "sv << id.globalIndex << " to the composite PCM profile"sv;
          return -1;
        } else {
          recreate = controller.allocated;
          controller.feedback_queue = std::move(feedback_queue);
          controller.client_relative_index = id.clientRelativeIndex;
          controller.allocated = true;
          controller.active = true;
          controller.create_pending = true;
          controller.uses_composite = enable_native_pcm;
          controller.accepts_native_pcm = enable_native_pcm;
          controller.logged_native_pcm = false;
          controller.native_pcm_paused = false;
          controller.native_pcm_silent_frames = 0;
          controller.create_status = pending_status;
          controller.have_rumble = false;
          controller.have_rgb = false;
          controller.have_player_indicator = false;
          controller.have_adaptive_triggers = false;
        }
      }

      if (reused) {
        BOOST_LOG(info) << "Reused HIDMaestro virtual DualSense controller "sv << id.globalIndex << " without changing its Windows device identity"sv;
        if (!waiting_for_create) {
          rebound_feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
          rebound_feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
        }
        return 0;
      }

      BOOST_LOG(info) << "Sending HIDMaestro virtual DualSense controller request "sv << id.globalIndex;
      protocol::create_controller_payload_t create_payload {};
      create_payload.use_composite = enable_native_pcm ? 1 : 0;
      const protocol::outbound_message_t request {
        message_type_e::create_controller,
        static_cast<std::uint32_t>(id.globalIndex),
        std::vector<std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(&create_payload),
          reinterpret_cast<const std::uint8_t *>(&create_payload) + sizeof(create_payload)
        ),
      };
      const bool request_sent = recreate ?
                                  send(message_type_e::destroy_controller, id.globalIndex, nullptr, 0) &&
                                    send(message_type_e::create_controller, id.globalIndex, &create_payload, sizeof(create_payload)) :
                                  write_message(request);
      if (!request_sent) {
        std::lock_guard lock(state_mutex_);
        auto &controller = controllers_[id.globalIndex];
        controller.allocated = false;
        controller.active = false;
        controller.create_pending = false;
        controller.feedback_queue.reset();
        return -1;
      }
      return 0;
    }

    /**
     * @brief Detach feedback routing without destroying the Windows virtual device.
     * @param nr Sunshine global controller index.
     */
    void detach_gamepad(int nr) {
      if (nr < 0 || nr >= static_cast<int>(controllers_.size())) {
        return;
      }

      feedback_queue_t feedback_queue;
      std::uint8_t client_relative_index = 0;
      {
        std::lock_guard lock(state_mutex_);
        auto &controller = controllers_[nr];
        if (!controller.allocated || !controller.active) {
          return;
        }
        feedback_queue = controller.feedback_queue;
        client_relative_index = controller.client_relative_index;
        controller.active = false;
        controller.accepts_native_pcm = false;
        controller.feedback_queue.reset();
      }

      if (feedback_queue) {
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_relative_index, LI_MOTION_TYPE_ACCEL, 0));
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_relative_index, LI_MOTION_TYPE_GYRO, 0));
      }
      BOOST_LOG(info) << "Detached Moonlight controller from HIDMaestro virtual DualSense "sv << nr << " while preserving the Windows device"sv;
    }

    /**
     * @brief Destroy a virtual controller.
     * @param nr Sunshine global controller index.
     */
    void free_gamepad(int nr) {
      if (nr < 0 || nr >= static_cast<int>(controllers_.size())) {
        return;
      }
      {
        std::lock_guard lock(state_mutex_);
        if (!controllers_[nr].allocated && !controllers_[nr].create_pending) {
          return;
        }
        controllers_[nr].allocated = false;
        controllers_[nr].active = false;
        controllers_[nr].accepts_native_pcm = false;
        controllers_[nr].feedback_queue.reset();
      }
      send(message_type_e::destroy_controller, nr, nullptr, 0);
    }

    /**
     * @brief Queue a typed payload for the helper writer thread.
     * @param type Protocol message type.
     * @param controller_id Sunshine global controller index.
     * @param payload Payload bytes, or null for an empty payload.
     * @param payload_size Payload byte count.
     * @return True on success.
     */
    bool send(message_type_e type, std::uint32_t controller_id, const void *payload, std::uint32_t payload_size) {
      std::lock_guard lock(outbound_mutex_);
      if (!accepting_writes_ || !running_) {
        return false;
      }
      const auto queued = outbound_queue_.push(type, controller_id, payload, payload_size);
      if (queued) {
        outbound_changed_.notify_one();
      }
      return queued;
    }

    /**
     * @brief Write one already-owned message to the helper pipe.
     * @param message Complete outbound message.
     * @return True when the header and payload were written.
     */
    bool write_message(const protocol::outbound_message_t &message) {
      std::lock_guard write_lock(pipe_write_mutex_);
      const protocol::message_header_t header {
        protocol::magic,
        protocol::version,
        message.type,
        static_cast<std::uint32_t>(message.payload.size()),
        message.controller_id,
      };
      if (!write_exact(pipe_, write_event_, &header, sizeof(header)) || (!message.payload.empty() && !write_exact(pipe_, write_event_, message.payload.data(), message.payload.size()))) {
        BOOST_LOG(error) << "Failed writing to HIDMaestro helper pipe: "sv << GetLastError();
        return false;
      }
      if (message.type == message_type_e::create_controller) {
        BOOST_LOG(info) << "Requested HIDMaestro virtual DualSense controller "sv << message.controller_id;
      } else if (message.type == message_type_e::destroy_controller) {
        BOOST_LOG(info) << "Requested removal of HIDMaestro virtual DualSense controller "sv << message.controller_id;
      }
      return true;
    }

    /**
     * @brief Drain outbound messages on a thread independent of Sunshine input.
     */
    void writer_loop() {
      while (true) {
        protocol::outbound_message_t message {};
        {
          std::unique_lock lock(outbound_mutex_);
          outbound_changed_.wait(lock, [this]() {
            return writer_stopping_ || !outbound_queue_.empty();
          });
          if (writer_stopping_) {
            return;
          }
          message = outbound_queue_.pop();
        }

        if (!write_message(message)) {
          {
            std::lock_guard lock(outbound_mutex_);
            accepting_writes_ = false;
            writer_stopping_ = true;
            outbound_queue_.clear();
          }
          running_ = false;
          state_changed_.notify_all();
          CancelIoEx(pipe_, nullptr);
          return;
        }
      }
    }

    /**
     * @brief Receive helper messages until the pipe closes.
     */
    void reader_loop() {
      while (running_) {
        protocol::message_header_t header {};
        if (!read_exact(pipe_, read_event_, &header, sizeof(header))) {
          break;
        }
        if (header.magic != protocol::magic || header.version != protocol::version || header.payload_size > protocol::max_payload_size) {
          BOOST_LOG(error) << "Rejected invalid HIDMaestro protocol header"sv;
          break;
        }

        std::vector<std::uint8_t> payload(header.payload_size);
        if (!payload.empty() && !read_exact(pipe_, read_event_, payload.data(), payload.size())) {
          break;
        }
        handle_message(header, payload);
      }

      running_ = false;
      state_changed_.notify_all();
    }

    /**
     * @brief Dispatch one helper message.
     * @param header Validated message header.
     * @param payload Message payload.
     */
    void handle_message(const protocol::message_header_t &header, const std::vector<std::uint8_t> &payload) {
      switch (header.type) {
        case message_type_e::ready:
        case message_type_e::controller_created:
          handle_status(header, payload);
          break;
        case message_type_e::rumble:
          handle_rumble(header.controller_id, payload);
          break;
        case message_type_e::rgb:
          handle_rgb(header.controller_id, payload);
          break;
        case message_type_e::adaptive_triggers:
          handle_adaptive_triggers(header.controller_id, payload);
          break;
        case message_type_e::player_indicator:
          handle_player_indicator(header.controller_id, payload);
          break;
        case message_type_e::controller_pcm:
          handle_controller_pcm(header.controller_id, payload);
          break;
        case message_type_e::log:
          BOOST_LOG(info) << "HIDMaestro: "sv << std::string(payload.begin(), payload.end());
          break;
        default:
          BOOST_LOG(warning) << "Ignored unexpected HIDMaestro message type "sv << static_cast<std::uint16_t>(header.type);
          break;
      }
    }

    /**
     * @brief Handle helper status replies.
     * @param header Message header.
     * @param payload Status payload bytes.
     */
    void handle_status(const protocol::message_header_t &header, const std::vector<std::uint8_t> &payload) {
      if (payload.size() != sizeof(protocol::status_payload_t)) {
        return;
      }
      protocol::status_payload_t status {};
      std::memcpy(&status, payload.data(), sizeof(status));
      if (header.type == message_type_e::ready) {
        std::lock_guard lock(state_mutex_);
        ready_status_ = status.status;
        state_changed_.notify_all();
        return;
      }
      if (header.controller_id >= controllers_.size()) {
        return;
      }

      feedback_queue_t feedback_queue;
      std::uint8_t client_relative_index = 0;
      bool controller_is_active = false;
      {
        std::lock_guard lock(state_mutex_);
        auto &controller = controllers_[header.controller_id];
        controller.create_status = status.status;
        controller.create_pending = false;
        if (status.status != 0) {
          controller.allocated = false;
          controller.active = false;
          controller.feedback_queue.reset();
        } else if (controller.allocated && controller.active && controller.feedback_queue) {
          feedback_queue = controller.feedback_queue;
          client_relative_index = controller.client_relative_index;
          controller_is_active = true;
        }
      }

      if (status.status != 0) {
        BOOST_LOG(error) << "HIDMaestro failed to create controller "sv << header.controller_id << " with status "sv << status.status;
      } else if (controller_is_active) {
        BOOST_LOG(info) << "HIDMaestro created virtual DualSense controller "sv << header.controller_id;
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_relative_index, LI_MOTION_TYPE_ACCEL, 100));
        feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(client_relative_index, LI_MOTION_TYPE_GYRO, 100));
      }
    }

    /**
     * @brief Handle standard rumble feedback.
     * @param controller_id Sunshine global controller index.
     * @param payload Rumble payload bytes.
     */
    void handle_rumble(std::uint32_t controller_id, const std::vector<std::uint8_t> &payload) {
      if (controller_id >= controllers_.size() || payload.size() != sizeof(protocol::rumble_payload_t)) {
        return;
      }
      protocol::rumble_payload_t rumble {};
      std::memcpy(&rumble, payload.data(), sizeof(rumble));
      std::lock_guard lock(state_mutex_);
      auto &controller = controllers_[controller_id];
      if (!controller.allocated || !controller.active || !controller.feedback_queue) {
        return;
      }
      if (controller.have_rumble && std::memcmp(&controller.last_rumble, &rumble, sizeof(rumble)) == 0) {
        return;
      }
      controller.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(controller.client_relative_index, rumble.low_frequency, rumble.high_frequency));
      controller.last_rumble = rumble;
      controller.have_rumble = true;
    }

    /**
     * @brief Handle lightbar feedback.
     * @param controller_id Sunshine global controller index.
     * @param payload RGB payload bytes.
     */
    void handle_rgb(std::uint32_t controller_id, const std::vector<std::uint8_t> &payload) {
      if (controller_id >= controllers_.size() || payload.size() != sizeof(protocol::rgb_payload_t)) {
        return;
      }
      protocol::rgb_payload_t rgb {};
      std::memcpy(&rgb, payload.data(), sizeof(rgb));
      std::lock_guard lock(state_mutex_);
      auto &controller = controllers_[controller_id];
      if (!controller.allocated || !controller.active || !controller.feedback_queue) {
        return;
      }
      if (controller.have_rgb && std::memcmp(&controller.last_rgb, &rgb, sizeof(rgb)) == 0) {
        return;
      }
      controller.feedback_queue->raise(gamepad_feedback_msg_t::make_rgb_led(controller.client_relative_index, rgb.red, rgb.green, rgb.blue));
      controller.last_rgb = rgb;
      controller.have_rgb = true;
    }

    /**
     * @brief Handle DualSense player-indicator feedback.
     * @param controller_id Sunshine global controller index.
     * @param payload Player-indicator payload bytes.
     */
    void handle_player_indicator(std::uint32_t controller_id, const std::vector<std::uint8_t> &payload) {
      if (controller_id >= controllers_.size() || payload.size() != sizeof(protocol::player_indicator_payload_t)) {
        return;
      }
      protocol::player_indicator_payload_t indicator {};
      std::memcpy(&indicator, payload.data(), sizeof(indicator));
      std::lock_guard lock(state_mutex_);
      auto &controller = controllers_[controller_id];
      if (!controller.allocated || !controller.active || !controller.feedback_queue) {
        return;
      }
      if (controller.have_player_indicator && std::memcmp(&controller.last_player_indicator, &indicator, sizeof(indicator)) == 0) {
        return;
      }
      controller.feedback_queue->raise(gamepad_feedback_msg_t::make_player_indicator(controller.client_relative_index, indicator.value));
      controller.last_player_indicator = indicator;
      controller.have_player_indicator = true;
    }

    /**
     * @brief Handle adaptive-trigger feedback.
     * @param controller_id Sunshine global controller index.
     * @param payload Adaptive-trigger payload bytes.
     */
    void handle_adaptive_triggers(std::uint32_t controller_id, const std::vector<std::uint8_t> &payload) {
      if (controller_id >= controllers_.size() || payload.size() != sizeof(protocol::adaptive_triggers_payload_t)) {
        return;
      }
      protocol::adaptive_triggers_payload_t triggers {};
      std::memcpy(&triggers, payload.data(), sizeof(triggers));
      std::lock_guard lock(state_mutex_);
      auto &controller = controllers_[controller_id];
      if (!controller.allocated || !controller.active || !controller.feedback_queue) {
        return;
      }
      if (controller.have_adaptive_triggers && std::memcmp(&controller.last_adaptive_triggers, &triggers, sizeof(triggers)) == 0) {
        return;
      }
      controller.feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(controller.client_relative_index, triggers.event_flags, triggers.type_left, triggers.type_right, triggers.left, triggers.right));
      controller.last_adaptive_triggers = triggers;
      controller.have_adaptive_triggers = true;
    }

    /**
     * @brief Handle native DualSense controller audio/haptics PCM.
     * @param controller_id Sunshine global controller index.
     * @param payload PCM metadata followed by interleaved samples.
     */
    void handle_controller_pcm(std::uint32_t controller_id, const std::vector<std::uint8_t> &payload) {
      if (controller_id >= controllers_.size() || payload.size() < sizeof(protocol::controller_pcm_header_t)) {
        return;
      }

      protocol::controller_pcm_header_t pcm_header {};
      std::memcpy(&pcm_header, payload.data(), sizeof(pcm_header));
      const std::size_t pcm_size = payload.size() - sizeof(pcm_header);
      const std::size_t sample_frame_size = static_cast<std::size_t>(pcm_header.channels) * (pcm_header.bits_per_sample / 8);
      if (pcm_header.sample_rate != 48000 || pcm_header.channels != 4 || pcm_header.bits_per_sample != 16 ||
          pcm_size == 0 || pcm_size > protocol::max_pcm_bytes || sample_frame_size == 0 || pcm_size % sample_frame_size != 0) {
        BOOST_LOG(warning) << "Ignored invalid HIDMaestro native PCM payload for controller "sv << controller_id;
        return;
      }

      std::lock_guard lock(state_mutex_);
      auto &controller = controllers_[controller_id];
      if (!controller.allocated || !controller.active || !controller.accepts_native_pcm || !controller.feedback_queue) {
        return;
      }

      const auto pcm = std::span<const std::uint8_t>(payload).subspan(sizeof(pcm_header));
      const auto silence_event = protocol::update_pcm_silence_gate(
        protocol::contains_pcm_signal(pcm, native_pcm_channels, native_pcm_signal_first_channel, native_pcm_silence_threshold),
        native_pcm_silent_frame_limit,
        static_cast<std::uint32_t>(pcm_size / sample_frame_size),
        controller.native_pcm_silent_frames,
        controller.native_pcm_paused
      );
      if (silence_event == protocol::pcm_silence_event_e::resume) {
        BOOST_LOG(info) << "HIDMaestro native DualSense PCM resumed after controller audio/haptic signal for controller "sv << controller_id;
      } else if (silence_event == protocol::pcm_silence_event_e::pause) {
        BOOST_LOG(info) << "HIDMaestro native DualSense PCM paused after sustained controller audio/haptic silence for controller "sv << controller_id;
        return;
      } else if (silence_event == protocol::pcm_silence_event_e::suppressed) {
        return;
      }
      controller.feedback_queue->raise(gamepad_feedback_msg_t::make_controller_haptic_pcm(
        controller.client_relative_index,
        pcm_header.sequence,
        pcm_header.sample_rate,
        pcm_header.channels,
        pcm_header.bits_per_sample,
        pcm
      ));
      if (!controller.logged_native_pcm) {
        BOOST_LOG(info) << "HIDMaestro native DualSense PCM started for controller "sv << controller_id
                        << ": "sv << static_cast<unsigned>(pcm_header.channels) << " channels, "sv
                        << pcm_header.sample_rate << " Hz, "sv << static_cast<unsigned>(pcm_header.bits_per_sample) << "-bit"sv;
        controller.logged_native_pcm = true;
      }
    }

    std::array<controller_t, MAX_GAMEPADS> controllers_;  ///< Controller routing table.
    mutable std::mutex state_mutex_;  ///< Protects readiness and controller state.
    std::condition_variable state_changed_;  ///< Signals helper status replies.
    std::mutex outbound_mutex_;  ///< Protects the bounded helper-message queue.
    std::condition_variable outbound_changed_;  ///< Wakes the helper writer thread.
    protocol::outbound_queue_t outbound_queue_;  ///< Messages awaiting named-pipe transmission.
    std::mutex pipe_write_mutex_;  ///< Prevents lifecycle and queued messages from interleaving on the pipe.
    HANDLE pipe_ {INVALID_HANDLE_VALUE};  ///< Connected named pipe.
    HANDLE read_event_ {};  ///< Manual-reset event for overlapped pipe reads.
    HANDLE write_event_ {};  ///< Manual-reset event for serialized overlapped pipe writes.
    std::unique_ptr<bp::child> child_;  ///< Helper process.
    std::thread reader_;  ///< Feedback reader thread.
    std::thread writer_;  ///< Named-pipe writer isolated from Sunshine input processing.
    std::atomic_bool running_ {};  ///< Whether the feedback loop should run.
    bool accepting_writes_ {};  ///< Whether callers may enqueue helper messages.
    bool writer_stopping_ {};  ///< Whether the writer should exit without draining queued input.
    std::int32_t ready_status_ {pending_status};  ///< Helper initialization result.
    std::string pipe_name_;  ///< Pipe name passed to the helper.
  };

  client_t::client_t():
      impl_(std::make_unique<impl_t>()) {
  }

  client_t::~client_t() = default;

  int client_t::init() {
    return impl_->init();
  }

  bool client_t::available() const {
    return impl_->available();
  }

  int client_t::alloc_gamepad(const gamepad_id_t &id, feedback_queue_t feedback_queue, bool enable_native_pcm) {
    return impl_->alloc_gamepad(id, std::move(feedback_queue), enable_native_pcm);
  }

  void client_t::detach_gamepad(int nr) {
    impl_->detach_gamepad(nr);
  }

  void client_t::free_gamepad(int nr) {
    impl_->free_gamepad(nr);
  }

  void client_t::update(int nr, const gamepad_state_t &state, bool back_as_touchpad_click) {
    const protocol::gamepad_payload_t payload {
      protocol::buttons_from_moonlight(state.buttonFlags, back_as_touchpad_click),
      state.lsX,
      state.lsY,
      state.rsX,
      state.rsY,
      state.lt,
      state.rt,
      protocol::hat_from_moonlight(state.buttonFlags),
      0,
    };
    impl_->send(message_type_e::gamepad_state, nr, &payload, sizeof(payload));
  }

  void client_t::touch(const gamepad_touch_t &touch) {
    const protocol::touch_payload_t payload {
      touch.pointerId,
      touch.x,
      touch.y,
      touch.pressure,
      touch.eventType,
      {},
    };
    impl_->send(message_type_e::touch, touch.id.globalIndex, &payload, sizeof(payload));
  }

  void client_t::motion(const gamepad_motion_t &motion) {
    const protocol::motion_payload_t payload {
      motion.x,
      motion.y,
      motion.z,
      motion.motionType,
      {},
    };
    impl_->send(message_type_e::motion, motion.id.globalIndex, &payload, sizeof(payload));
  }

  void client_t::battery(const gamepad_battery_t &battery) {
    const protocol::battery_payload_t payload {
      battery.state,
      battery.percentage,
      {},
    };
    impl_->send(message_type_e::battery, battery.id.globalIndex, &payload, sizeof(payload));
  }

  bool client_t::owns_gamepad(int nr) const {
    if (nr < 0 || nr >= static_cast<int>(impl_->controllers_.size())) {
      return false;
    }
    std::lock_guard lock(impl_->state_mutex_);
    return impl_->controllers_[nr].allocated && impl_->controllers_[nr].active;
  }

  bool client_t::has_gamepad(int nr) const {
    if (nr < 0 || nr >= static_cast<int>(impl_->controllers_.size())) {
      return false;
    }
    std::lock_guard lock(impl_->state_mutex_);
    return impl_->controllers_[nr].allocated;
  }
}  // namespace platf::hidmaestro
