/**
 * @file src/platform/windows/mic_uplink.cpp
 * @brief Windows microphone uplink playback for VB-CABLE style virtual microphones.
 */

// standard includes
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

// platform includes
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

// local includes
#include "src/logging.h"
#include "src/platform/mic_uplink.h"
#include "src/utility.h"

using Microsoft::WRL::ComPtr;
using namespace std::literals;

namespace platf::mic_uplink {
  namespace {
    std::string lowercase_copy(const std::string &value) {
      std::string result = value;
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return result;
    }

    std::string wide_to_utf8(const std::wstring &value) {
      if (value.empty()) {
        return {};
      }

      auto size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
      if (size <= 0) {
        return {};
      }

      std::string result(size, '\0');
      WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
      return result;
    }

    std::string propvariant_to_utf8(const PROPVARIANT &value) {
      if (value.vt != VT_LPWSTR || value.pwszVal == nullptr) {
        return {};
      }

      return wide_to_utf8(value.pwszVal);
    }

    bool matches_device_name(const std::string &candidate, const std::string &needle) {
      if (candidate.empty() || needle.empty()) {
        return false;
      }

      return lowercase_copy(candidate).find(lowercase_copy(needle)) != std::string::npos;
    }

    ComPtr<IMMDevice> find_render_device(IMMDeviceEnumerator *device_enum, const std::string &device_name) {
      if (device_enum == nullptr) {
        return nullptr;
      }

      ComPtr<IMMDeviceCollection> collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate active render endpoints for mic uplink [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      UINT count = 0;
      status = collection->GetCount(&count);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't query render endpoint count for mic uplink [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) {
          continue;
        }

        ComPtr<IPropertyStore> prop_store;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &prop_store)) || !prop_store) {
          continue;
        }

        PROPVARIANT friendly_name {};
        PropVariantInit(&friendly_name);
        const auto clear_friendly_name = [&]() {
          PropVariantClear(&friendly_name);
        };

        if (FAILED(prop_store->GetValue(PKEY_Device_FriendlyName, &friendly_name))) {
          clear_friendly_name();
          continue;
        }

        auto friendly_name_utf8 = propvariant_to_utf8(friendly_name);
        clear_friendly_name();

        if (!matches_device_name(friendly_name_utf8, device_name)) {
          continue;
        }

        BOOST_LOG(info) << "Matched microphone uplink playback device ["sv << friendly_name_utf8 << ']';
        return device;
      }

      return nullptr;
    }

    class renderer_t final: public sink_t {
    public:
      explicit renderer_t(std::string device_name):
          _device_name(std::move(device_name)),
          _stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
      }

      renderer_t(const renderer_t &) = delete;
      renderer_t &operator=(const renderer_t &) = delete;

      bool start() {
        std::promise<bool> ready_promise;
        auto ready_future = ready_promise.get_future();

        if (_thread.joinable()) {
          _thread.join();
        }

        _thread = std::thread([this](std::promise<bool> promise) {
          worker(std::move(promise));
        }, std::move(ready_promise));

        _ready = ready_future.get();
        return _ready;
      }

      bool push_pcm(const std::int16_t *samples, std::size_t frame_count, std::uint32_t channel_count, std::uint32_t input_sample_rate) override {
        if (!_ready || samples == nullptr || frame_count == 0) {
          return false;
        }

        if (channel_count != channels || input_sample_rate != sample_rate) {
          BOOST_LOG(warning) << "Ignoring client microphone frame with unexpected format ["sv << channel_count << "ch @"sv << input_sample_rate << "Hz]"sv;
          return false;
        }

        constexpr std::size_t max_queued_samples = sample_rate / 2;

        {
          std::scoped_lock lock {_queue_mutex};
          if (_queued_samples.size() > max_queued_samples) {
            BOOST_LOG(warning) << "Dropping queued microphone uplink audio to keep latency bounded"sv;
            _queued_samples.clear();
          }

          _queued_samples.insert(_queued_samples.end(), samples, samples + frame_count * channel_count);
        }

        return true;
      }

      ~renderer_t() override {
        if (_stop_event) {
          SetEvent(_stop_event);
        }

        if (_thread.joinable()) {
          _thread.join();
        }

        if (_stop_event) {
          CloseHandle(_stop_event);
        }
      }

    private:
      void worker(std::promise<bool> ready_promise) {
        auto coinit_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
        const bool should_uninitialize = SUCCEEDED(coinit_status);

        ComPtr<IMMDeviceEnumerator> device_enum;
        auto status = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, reinterpret_cast<void **>(device_enum.GetAddressOf()));
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't create render device enumerator for mic uplink [0x"sv << util::hex(status).to_string_view() << ']';
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        auto device = find_render_device(device_enum.Get(), _device_name);
        if (!device) {
          BOOST_LOG(warning) << "Couldn't find microphone uplink playback device ["sv << _device_name << "]"sv;
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        ComPtr<IAudioClient> audio_client;
        status = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(audio_client.GetAddressOf()));
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't activate mic uplink audio client [0x"sv << util::hex(status).to_string_view() << ']';
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        WAVEFORMATEX desired_format {};
        desired_format.wFormatTag = WAVE_FORMAT_PCM;
        desired_format.nChannels = static_cast<WORD>(channels);
        desired_format.nSamplesPerSec = sample_rate;
        desired_format.wBitsPerSample = 16;
        desired_format.nBlockAlign = desired_format.nChannels * desired_format.wBitsPerSample / 8;
        desired_format.nAvgBytesPerSec = desired_format.nSamplesPerSec * desired_format.nBlockAlign;
        desired_format.cbSize = 0;

        const DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        status = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &desired_format, nullptr);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't initialize mic uplink playback client [0x"sv << util::hex(status).to_string_view() << ']';
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        ComPtr<IAudioRenderClient> render_client;
        status = audio_client->GetService(IID_IAudioRenderClient, reinterpret_cast<void **>(render_client.GetAddressOf()));
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't acquire mic uplink render client [0x"sv << util::hex(status).to_string_view() << ']';
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        HANDLE render_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!render_event) {
          BOOST_LOG(error) << "Couldn't create mic uplink render event"sv;
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        status = audio_client->SetEventHandle(render_event);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't set mic uplink render event [0x"sv << util::hex(status).to_string_view() << ']';
          CloseHandle(render_event);
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        UINT32 buffer_frame_count = 0;
        status = audio_client->GetBufferSize(&buffer_frame_count);
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't query mic uplink buffer size [0x"sv << util::hex(status).to_string_view() << ']';
          CloseHandle(render_event);
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        BYTE *initial_buffer = nullptr;
        status = render_client->GetBuffer(buffer_frame_count, &initial_buffer);
        if (SUCCEEDED(status)) {
          std::memset(initial_buffer, 0, buffer_frame_count * desired_format.nBlockAlign);
          render_client->ReleaseBuffer(buffer_frame_count, 0);
        }

        status = audio_client->Start();
        if (FAILED(status)) {
          BOOST_LOG(error) << "Couldn't start mic uplink playback client [0x"sv << util::hex(status).to_string_view() << ']';
          CloseHandle(render_event);
          ready_promise.set_value(false);
          if (should_uninitialize) {
            CoUninitialize();
          }
          return;
        }

        BOOST_LOG(info) << "Client microphone uplink playback initialized on ["sv << _device_name << "]"sv;
        ready_promise.set_value(true);

        HANDLE wait_handles[2] = {_stop_event, render_event};
        bool running = true;
        while (running) {
          auto wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
          switch (wait_result) {
            case WAIT_OBJECT_0:
              running = false;
              break;
            case WAIT_OBJECT_0 + 1:
              pump_audio(audio_client.Get(), render_client.Get(), buffer_frame_count, desired_format.nBlockAlign);
              break;
            default:
              running = false;
              break;
          }
        }

        audio_client->Stop();
        CloseHandle(render_event);

        if (should_uninitialize) {
          CoUninitialize();
        }
      }

      void pump_audio(IAudioClient *audio_client, IAudioRenderClient *render_client, UINT32 buffer_frame_count, WORD block_align) {
        if (audio_client == nullptr || render_client == nullptr) {
          return;
        }

        UINT32 padding = 0;
        auto status = audio_client->GetCurrentPadding(&padding);
        if (FAILED(status)) {
          return;
        }

        auto frames_available = buffer_frame_count - padding;
        if (frames_available == 0) {
          return;
        }

        BYTE *render_buffer = nullptr;
        status = render_client->GetBuffer(frames_available, &render_buffer);
        if (FAILED(status) || render_buffer == nullptr) {
          return;
        }

        const auto samples_available = static_cast<std::size_t>(frames_available) * channels;
        std::size_t samples_to_copy = 0;

        {
          std::scoped_lock lock {_queue_mutex};
          samples_to_copy = std::min(samples_available, _queued_samples.size());
          if (samples_to_copy != 0) {
            std::copy_n(_queued_samples.begin(), static_cast<std::ptrdiff_t>(samples_to_copy), reinterpret_cast<std::int16_t *>(render_buffer));
            _queued_samples.erase(_queued_samples.begin(), _queued_samples.begin() + static_cast<std::ptrdiff_t>(samples_to_copy));
          }
        }

        if (samples_to_copy < samples_available) {
          std::memset(render_buffer + samples_to_copy * sizeof(std::int16_t), 0, (samples_available - samples_to_copy) * sizeof(std::int16_t));
        }

        render_client->ReleaseBuffer(frames_available, 0);
      }

      std::string _device_name;
      HANDLE _stop_event {};
      std::thread _thread;
      std::mutex _queue_mutex;
      std::deque<std::int16_t> _queued_samples;
      bool _ready {false};
    };
  }  // namespace

  bool available(const std::string &device_name) {
    auto coinit_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    const bool should_uninitialize = SUCCEEDED(coinit_status);

    ComPtr<IMMDeviceEnumerator> device_enum;
    auto status = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, reinterpret_cast<void **>(device_enum.GetAddressOf()));
    if (FAILED(status)) {
      if (should_uninitialize) {
        CoUninitialize();
      }
      return false;
    }

    const auto found = static_cast<bool>(find_render_device(device_enum.Get(), device_name));
    if (should_uninitialize) {
      CoUninitialize();
    }
    return found;
  }

  std::unique_ptr<sink_t> create_sink(const std::string &device_name) {
    auto sink = std::make_unique<renderer_t>(device_name);
    if (!sink->start()) {
      return nullptr;
    }
    return sink;
  }
}  // namespace platf::mic_uplink
