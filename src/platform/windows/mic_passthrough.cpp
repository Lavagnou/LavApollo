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
#include <vector>

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

      // Endpoints that were disabled in the Sound settings and that we enabled for the session
      std::vector<std::wstring> enabled_endpoints;

      // An endpoint whose format we changed for the session, and the format to put back
      std::wstring reformatted_endpoint;
      std::vector<std::uint8_t> original_format;

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

    /**
     * @brief Enables whichever side of the virtual microphone is disabled, noting it in
     *        `enabled`, and waits for Windows to bring it up.
     * @return The endpoints as they are now, or nothing if they never became usable.
     */
    std::optional<platf::audio::mic_endpoints_t> enable_endpoints(platf::audio::mic_endpoints_t endpoints, std::vector<std::wstring> &enabled) {
      if (endpoints.render_disabled && platf::audio::set_endpoint_enabled(endpoints.render_id, true)) {
        enabled.push_back(endpoints.render_id);
      }
      if (endpoints.capture_disabled && platf::audio::set_endpoint_enabled(endpoints.capture_id, true)) {
        enabled.push_back(endpoints.capture_id);
      }
      if (enabled.empty()) {
        return endpoints.render_disabled ? std::nullopt : std::optional {endpoints};
      }

      BOOST_LOG(info) << "Microphone passthrough: enabled ["sv << endpoints.name << "] for the session"sv;
      for (int i = 0; i < 30; ++i) {
        auto now = platf::audio::find_mic_endpoints(config::audio.mic_sink);
        if (now && !now->render_disabled) {
          return now;
        }
        std::this_thread::sleep_for(100ms);
      }
      return std::nullopt;
    }

    /**
     * @brief Gives both sides of the virtual microphone the same format.
     *
     * Steam Streaming Microphone is a loopback that copies the playback side's samples to the
     * recording side as raw bytes, with no conversion. Out of the box its playback side is stereo
     * 48 kHz and its recording side mono 44.1 kHz, so applications read stereo as mono at the
     * wrong rate: garbled, slowed-down, unintelligible voice. The playback side is ours (nobody
     * else plays into a virtual microphone), so it takes the recording side's format; only if
     * the driver refuses is the recording side changed instead. Windows converts our stream to
     * whichever format results.
     */
    void match_formats(const platf::audio::mic_endpoints_t &endpoints, std::wstring &reformatted, std::vector<std::uint8_t> &original) {
      if (endpoints.capture_id.empty()) {
        return;
      }

      auto render_format = platf::audio::device_format(endpoints.render_id);
      auto capture_format = platf::audio::device_format(endpoints.capture_id);
      if (render_format.empty() || capture_format.empty() || platf::audio::same_device_format(render_format, capture_format)) {
        return;
      }

      if (platf::audio::set_device_format(endpoints.render_id, capture_format)) {
        reformatted = endpoints.render_id;
        original = std::move(render_format);
        BOOST_LOG(info) << "Microphone passthrough: playing at the recording side's format ("sv
                        << platf::audio::device_format_to_string(capture_format) << ") for the session"sv;
        return;
      }

      if (platf::audio::set_device_format(endpoints.capture_id, render_format)) {
        reformatted = endpoints.capture_id;
        original = std::move(capture_format);
        BOOST_LOG(info) << "Microphone passthrough: recording at the playback side's format ("sv
                        << platf::audio::device_format_to_string(render_format) << ") for the session"sv;
        return;
      }

      BOOST_LOG(warning) << "Microphone passthrough: the two sides of ["sv << endpoints.name << "] use different formats ("sv
                         << platf::audio::device_format_to_string(render_format) << " vs "sv
                         << platf::audio::device_format_to_string(capture_format)
                         << ") and neither could be changed; set both to the same Default Format in the Sound settings"sv;
    }

    void restore_format(std::wstring &reformatted, std::vector<std::uint8_t> &original) {
      if (!reformatted.empty()) {
        platf::audio::set_device_format(reformatted, original);
      }
      reformatted.clear();
      original.clear();
    }

    void disable_endpoints(std::vector<std::wstring> &enabled) {
      for (const auto &id : enabled) {
        platf::audio::set_endpoint_enabled(id, false);
      }
      enabled.clear();
    }

    /// Whether the virtual microphone exists (enabled or not): -1 until first checked.
    /// Refreshed by a background thread so that serverinfo never touches the audio stack.
    std::atomic_int device_present {-1};

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

    // Disabled in the Sound settings still counts: acquire() enables it for the session
    return device_present.load() == 1 ? state_e::ready : state_e::no_device;
  }

  bool acquire(const void *session, const std::string &device_name) {
    std::lock_guard lg {owner_mutex};

    if (owner.session) {
      BOOST_LOG(warning) << "Microphone passthrough: ["sv << device_name << "] asked for the microphone, but ["sv
                         << owner.device_name << "] is already using it"sv;
      return false;
    }

    com_t com;
    auto found = platf::audio::find_mic_endpoints(config::audio.mic_sink);
    if (!found) {
      BOOST_LOG(warning) << "Microphone passthrough: no virtual microphone found ("sv
                         << (config::audio.mic_sink.empty() ? "Steam Streaming Microphone"s : config::audio.mic_sink) << ')';
      return false;
    }

    std::vector<std::wstring> enabled;
    auto endpoints = enable_endpoints(*found, enabled);
    // Puts back whatever we enabled if the session doesn't get the microphone after all
    auto undo_enable = util::fail_guard([&enabled]() {
      disable_endpoints(enabled);
    });
    if (!endpoints) {
      BOOST_LOG(warning) << "Microphone passthrough: ["sv << found->name << "] is disabled and couldn't be enabled"sv;
      return false;
    }

    // Before the playback client exists: it is initialized against the device format
    std::wstring reformatted;
    std::vector<std::uint8_t> original_format;
    match_formats(*endpoints, reformatted, original_format);
    auto undo_format = util::fail_guard([&reformatted, &original_format]() {
      restore_format(reformatted, original_format);
    });

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
    owner.enabled_endpoints = std::move(enabled);
    undo_enable.disable();
    owner.reformatted_endpoint = std::move(reformatted);
    owner.original_format = std::move(original_format);
    undo_format.disable();

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
    // Back to how the user left their Sound settings
    restore_format(owner.reformatted_endpoint, owner.original_format);
    disable_endpoints(owner.enabled_endpoints);
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

  void init() {
    if (!config::audio.mic_passthrough) {
      return;
    }

    // Lives as long as the process. The driver install waits seconds for Windows to settle, and
    // the device check must stay off HTTPS request threads: both happen here.
    std::thread {[]() {
      com_t com;
      if (config::audio.mic_sink.empty() && config::audio.install_steam_drivers) {
        platf::audio::install_steam_microphone_if_missing();
      }

      while (true) {
        device_present = platf::audio::find_mic_endpoints(config::audio.mic_sink).has_value() ? 1 : 0;
        std::this_thread::sleep_for(3s);
      }
    }}.detach();
  }
}  // namespace mic_passthrough
