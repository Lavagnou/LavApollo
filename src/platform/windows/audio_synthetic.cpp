/**
 * @file src/platform/windows/audio_synthetic.cpp
 * @brief Test host mode audio: see audio_synthetic.h.
 */
// standard includes
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>

// local includes
#include "audio_synthetic.h"

using namespace std::literals;

namespace platf::audio {
  namespace {
    constexpr double TONE_HZ = 440.0;
    constexpr float TONE_AMPLITUDE = 0.1f;
    const std::string SINK_NAME = "test-host-sink";

    /**
     * @brief Delivers a sine one frame at a time, paced like a capture device.
     */
    class tone_mic_t: public mic_t {
    public:
      tone_mic_t(int channels, std::uint32_t sample_rate, std::uint32_t frame_size):
          _channels {channels},
          _rate {sample_rate},
          _frame_size {frame_size} {
      }

      capture_e sample(std::vector<float> &frame_buffer) override {
        // A capture hands out a frame when its time has come, not before
        auto frame_duration = std::chrono::nanoseconds {std::chrono::seconds {1}} * _frame_size / _rate;
        auto now = std::chrono::steady_clock::now();
        if (!_started) {
          _started = true;
          _next = now;
        }
        if (_next > now) {
          std::this_thread::sleep_until(_next);
        } else if (now - _next > 10 * frame_duration) {
          _next = now;  // far behind (the thread was stalled): resume instead of bursting
        }
        _next += frame_duration;

        frame_buffer.resize(static_cast<std::size_t>(_frame_size) * _channels);
        for (std::uint32_t i = 0; i < _frame_size; ++i) {
          // The index wraps once a second, a whole number of periods: phase stays continuous, and exact
          auto phase = 2.0 * std::numbers::pi * TONE_HZ * static_cast<double>(_index % _rate) / _rate;
          auto value = TONE_AMPLITUDE * static_cast<float>(std::sin(phase));
          for (int c = 0; c < _channels; ++c) {
            frame_buffer[static_cast<std::size_t>(i) * _channels + c] = value;
          }
          ++_index;
        }
        return capture_e::ok;
      }

    private:
      int _channels;
      std::uint32_t _rate;
      std::uint32_t _frame_size;
      std::uint64_t _index = 0;
      bool _started = false;
      std::chrono::steady_clock::time_point _next;
    };

    class synthetic_control_t: public ::platf::audio_control_t {
    public:
      int set_sink(const std::string &) override {
        return 0;
      }

      std::unique_ptr<mic_t> microphone(const std::uint8_t *, int channels, std::uint32_t sample_rate, std::uint32_t frame_size) override {
        return std::make_unique<tone_mic_t>(channels, sample_rate, frame_size);
      }

      bool is_sink_available(const std::string &) override {
        return true;
      }

      std::optional<sink_t> sink_info() override {
        sink_t sink;
        sink.host = SINK_NAME;
        return sink;
      }
    };
  }  // namespace

  std::unique_ptr<::platf::audio_control_t> synthetic_audio_control() {
    return std::make_unique<synthetic_control_t>();
  }
}  // namespace platf::audio
