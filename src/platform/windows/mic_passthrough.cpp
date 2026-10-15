/**
 * @file src/platform/windows/mic_passthrough.cpp
 * @brief Client microphone passthrough on Windows: Opus decoding and WASAPI playback into a
 *        virtual microphone.
 */
// standard includes
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

// platform includes
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

// lib includes
#include <opus/opus.h>

// local includes
#include "audio_endpoints.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/mic_passthrough.h"
#include "src/mic_passthrough_policy.h"
#include "src/utility.h"

using namespace std::literals;

namespace mic_passthrough {
  namespace {
    /// Opus' longest frame, 120 ms.
    constexpr int MAX_FRAME_SAMPLES = SAMPLE_RATE * 120 / 1000;

    /// Played once this much is buffered: absorbs network jitter.
    constexpr std::size_t TARGET_SAMPLES = SAMPLE_RATE * 40 / 1000;

    /// Past this, the oldest audio is dropped back to the target: bounds the latency that the
    /// client's capture clock running faster than ours would otherwise pile up.
    constexpr std::size_t MAX_SAMPLES = SAMPLE_RATE * 150 / 1000;

    /// WASAPI buffer, in 100 ns units.
    constexpr REFERENCE_TIME DEVICE_BUFFER = 20 * 10000;

    /// Initializes COM for the current thread, and undoes it only if that worked.
    class com_t {
    public:
      com_t():
          _status {CoInitializeEx(nullptr, COINIT_MULTITHREADED)} {
      }

      ~com_t() {
        if (SUCCEEDED(_status)) {
          CoUninitialize();
        }
      }

    private:
      HRESULT _status;
    };

    template<class T>
    struct com_release {
      void operator()(T *p) const {
        p->Release();
      }
    };

    template<class T>
    using com_ptr = std::unique_ptr<T, com_release<T>>;

    struct opus_decoder_free {
      void operator()(OpusDecoder *decoder) const {
        opus_decoder_destroy(decoder);
      }
    };

    /**
     * @brief Plays buffered samples into one endpoint from its own thread.
     */
    class renderer_t {
    public:
      explicit renderer_t(std::wstring device_id):
          _device_id {std::move(device_id)},
          _buffer {TARGET_SAMPLES, MAX_SAMPLES} {
      }

      ~renderer_t() {
        _stop = true;
        if (_thread.joinable()) {
          _thread.join();
        }
      }

      /**
       * @brief Opens the device and starts playing.
       * @return Whether the device opened.
       */
      bool start() {
        std::promise<bool> opened;
        auto result = opened.get_future();
        _thread = std::thread {&renderer_t::run, this, std::move(opened)};
        return result.get();
      }

      /// @return How many samples were dropped to keep latency bounded.
      std::size_t push(const float *samples, std::size_t count) {
        std::lock_guard lg {_mutex};
        return _buffer.push(samples, count);
      }

    private:
      void run(std::promise<bool> opened) {
        com_t com;
        bool reported = false;
        auto report = [&](bool ok) {
          if (!reported) {
            reported = true;
            opened.set_value(ok);
          }
        };
        auto fg = util::fail_guard([&]() {
          report(false);
        });

        IMMDeviceEnumerator *enumerator_raw = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **) &enumerator_raw))) {
          BOOST_LOG(error) << "Microphone passthrough: couldn't create the device enumerator"sv;
          return;
        }
        com_ptr<IMMDeviceEnumerator> enumerator {enumerator_raw};

        IMMDevice *device_raw = nullptr;
        if (FAILED(enumerator->GetDevice(_device_id.c_str(), &device_raw))) {
          BOOST_LOG(error) << "Microphone passthrough: the virtual microphone disappeared"sv;
          return;
        }
        com_ptr<IMMDevice> device {device_raw};

        IAudioClient *client_raw = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &client_raw))) {
          BOOST_LOG(error) << "Microphone passthrough: couldn't activate the virtual microphone"sv;
          return;
        }
        com_ptr<IAudioClient> client {client_raw};

        // Mono float at 48 kHz, whatever the device runs at: AUTOCONVERTPCM has Windows convert
        // the rate and channel count. The device's own format is never changed.
        WAVEFORMATEX format {};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = SAMPLE_RATE;
        format.wBitsPerSample = 32;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

        auto status = client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          DEVICE_BUFFER,
          0,
          &format,
          nullptr
        );
        if (FAILED(status)) {
          BOOST_LOG(error) << "Microphone passthrough: couldn't initialize the virtual microphone: 0x"sv << util::hex(status).to_string_view();
          return;
        }

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
          return;
        }
        auto close_event = util::fail_guard([event]() {
          CloseHandle(event);
        });
        client->SetEventHandle(event);

        UINT32 buffer_frames = 0;
        client->GetBufferSize(&buffer_frames);

        IAudioRenderClient *render_raw = nullptr;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void **) &render_raw))) {
          BOOST_LOG(error) << "Microphone passthrough: couldn't get the render client"sv;
          return;
        }
        com_ptr<IAudioRenderClient> render {render_raw};

        if (FAILED(client->Start())) {
          BOOST_LOG(error) << "Microphone passthrough: couldn't start playback"sv;
          return;
        }
        auto stop_client = util::fail_guard([&client]() {
          client->Stop();
        });

        DWORD task_index = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        auto revert_mmcss = util::fail_guard([mmcss]() {
          if (mmcss) {
            AvRevertMmThreadCharacteristics(mmcss);
          }
        });

        report(true);

        while (!_stop) {
          WaitForSingleObject(event, 100);

          UINT32 padding = 0;
          status = client->GetCurrentPadding(&padding);
          if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
            BOOST_LOG(warning) << "Microphone passthrough: the virtual microphone was removed"sv;
            return;
          }
          if (FAILED(status) || padding >= buffer_frames) {
            continue;
          }

          auto frames = buffer_frames - padding;
          BYTE *data = nullptr;
          if (FAILED(render->GetBuffer(frames, &data))) {
            continue;
          }

          {
            std::lock_guard lg {_mutex};
            _buffer.pull(reinterpret_cast<float *>(data), frames);
          }

          render->ReleaseBuffer(frames, 0);
        }
      }

      std::wstring _device_id;
      std::thread _thread;
      std::atomic_bool _stop {false};

      std::mutex _mutex;
      jitter_buffer_t _buffer;
    };

    /**
     * @brief The session holding the virtual microphone, and everything it uses.
     */
    struct owner_t {
      const void *session = nullptr;
      std::string device_name;
      std::unique_ptr<renderer_t> renderer;
      std::unique_ptr<OpusDecoder, opus_decoder_free> decoder;
      sequence_tracker_t tracker;

      // Default recording devices replaced while streaming, by ERole; empty when untouched.
      std::wstring capture_id;
      std::array<std::wstring, ERole_enum_count> previous_defaults;

      std::uint64_t frames = 0;
      std::uint64_t concealed = 0;
      std::uint64_t late = 0;
      std::uint64_t dropped_samples = 0;
    };

    std::mutex owner_mutex;
    owner_t owner;

    void push(const float *samples, int count) {
      if (count > 0) {
        owner.dropped_samples += owner.renderer->push(samples, static_cast<std::size_t>(count));
      }
    }

    void make_default(const std::wstring &capture_id) {
      owner.capture_id = capture_id;
      for (int role = 0; role < ERole_enum_count; ++role) {
        auto previous = platf::audio::default_capture_id(role);
        if (previous == capture_id || !platf::audio::set_default_capture(capture_id, role)) {
          continue;
        }
        owner.previous_defaults[role] = std::move(previous);
      }
    }

    void restore_defaults() {
      for (int role = 0; role < ERole_enum_count; ++role) {
        auto &previous = owner.previous_defaults[role];
        // Leave it alone if the user picked another default meanwhile
        if (!previous.empty() && platf::audio::default_capture_id(role) == owner.capture_id) {
          platf::audio::set_default_capture(previous, role);
        }
        previous.clear();
      }
      owner.capture_id.clear();
    }
  }  // namespace

  std::string_view to_string(state_e state) {
    switch (state) {
      case state_e::ready:
        return "ready"sv;
      case state_e::disabled:
        return "disabled"sv;
      case state_e::no_permission:
        return "no_permission"sv;
      case state_e::no_device:
        return "no_device"sv;
    }
    return "disabled"sv;
  }

  state_e state(crypto::PERM perm) {
    if (!config::audio.mic_passthrough) {
      return state_e::disabled;
    }
    if (!(perm & crypto::PERM::microphone)) {
      return state_e::no_permission;
    }

    static std::mutex cache_mutex;
    static std::chrono::steady_clock::time_point checked_at;
    static bool device_found = false;

    std::lock_guard lg {cache_mutex};
    auto now = std::chrono::steady_clock::now();
    if (checked_at == std::chrono::steady_clock::time_point {} || now - checked_at > 5s) {
      com_t com;
      device_found = platf::audio::find_mic_endpoints(config::audio.mic_sink).has_value();
      checked_at = now;
    }

    return device_found ? state_e::ready : state_e::no_device;
  }

  bool acquire(const void *session, const std::string &device_name) {
    std::lock_guard lg {owner_mutex};

    if (owner.session) {
      BOOST_LOG(warning) << "Microphone passthrough: ["sv << device_name << "] asked for the microphone, but ["sv
                         << owner.device_name << "] is already using it"sv;
      return false;
    }

    com_t com;
    auto endpoints = platf::audio::find_mic_endpoints(config::audio.mic_sink);
    if (!endpoints) {
      BOOST_LOG(warning) << "Microphone passthrough: no virtual microphone found ("sv
                         << (config::audio.mic_sink.empty() ? "Steam Streaming Microphone"s : config::audio.mic_sink) << ')';
      return false;
    }

    int opus_error = OPUS_OK;
    std::unique_ptr<OpusDecoder, opus_decoder_free> decoder {opus_decoder_create(SAMPLE_RATE, 1, &opus_error)};
    if (opus_error != OPUS_OK || !decoder) {
      BOOST_LOG(error) << "Microphone passthrough: couldn't create the Opus decoder: "sv << opus_strerror(opus_error);
      return false;
    }

    auto renderer = std::make_unique<renderer_t>(endpoints->render_id);
    if (!renderer->start()) {
      return false;
    }

    owner.session = session;
    owner.device_name = device_name;
    owner.decoder = std::move(decoder);
    owner.renderer = std::move(renderer);
    owner.tracker.reset();
    owner.frames = owner.concealed = owner.late = owner.dropped_samples = 0;

    if (config::audio.mic_set_default) {
      if (endpoints->capture_id.empty()) {
        BOOST_LOG(warning) << "Microphone passthrough: couldn't find the recording side of ["sv << endpoints->name
                           << "], so it can't become the default microphone"sv;
      } else {
        make_default(endpoints->capture_id);
      }
    }

    BOOST_LOG(info) << "Microphone passthrough: playing ["sv << device_name << "]'s microphone into ["sv << endpoints->name << ']';
    return true;
  }

  void release(const void *session) {
    std::lock_guard lg {owner_mutex};

    if (!session || owner.session != session) {
      return;
    }

    com_t com;
    restore_defaults();

    owner.renderer.reset();
    owner.decoder.reset();
    owner.session = nullptr;

    BOOST_LOG(info) << "Microphone passthrough: ["sv << owner.device_name << "] stopped after "sv << owner.frames << " frames ("sv
                    << owner.concealed << " concealed, "sv << owner.late << " late, "sv << owner.dropped_samples
                    << " samples dropped for latency)"sv;
  }

  void submit(const void *session, std::string_view payload) {
    std::lock_guard lg {owner_mutex};

    if (!session || owner.session != session) {
      return;
    }

    auto frame = parse_frame(payload);
    if (!frame) {
      return;
    }

    auto lost = owner.tracker.accept(frame->sequence);
    if (!lost) {
      ++owner.late;
      return;
    }

    auto data = reinterpret_cast<const unsigned char *>(frame->opus.data());
    auto size = static_cast<opus_int32>(frame->opus.size());
    auto frame_samples = opus_packet_get_nb_samples(data, size, SAMPLE_RATE);
    if (frame_samples <= 0 || frame_samples > MAX_FRAME_SAMPLES) {
      return;
    }

    std::array<float, MAX_FRAME_SAMPLES> pcm;
    auto decoder = owner.decoder.get();

    // Rebuild what went missing: the frame right before this one from the FEC this frame
    // carries, any earlier ones by packet loss concealment.
    if (*lost > 0) {
      for (int i = 0; i < *lost - 1; ++i) {
        push(pcm.data(), opus_decode_float(decoder, nullptr, 0, pcm.data(), frame_samples, 0));
      }
      push(pcm.data(), opus_decode_float(decoder, data, size, pcm.data(), frame_samples, 1));
      owner.concealed += *lost;
    }

    push(pcm.data(), opus_decode_float(decoder, data, size, pcm.data(), MAX_FRAME_SAMPLES, 0));
    ++owner.frames;
  }

  void install_driver_if_needed() {
    if (!config::audio.mic_passthrough || !config::audio.mic_sink.empty() || !config::audio.install_steam_drivers) {
      return;
    }

    // The driver install waits seconds for Windows to settle: keep it off the startup path.
    std::thread {[]() {
      com_t com;
      platf::audio::install_steam_microphone_if_missing();
    }}.detach();
  }
}  // namespace mic_passthrough
