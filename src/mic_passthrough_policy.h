/**
 * @file src/mic_passthrough_policy.h
 * @brief Platform-independent parts of client microphone passthrough: message parsing, loss
 *        detection and the jitter buffer.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>

namespace mic_passthrough {
  /// Control packet type of a microphone frame (client to host, LavArtemis extension).
  constexpr std::uint16_t MICROPHONE_DATA_PTYPE = 0x3080;

  /// Sample rate of the client's Opus stream, and of everything after the decoder.
  constexpr int SAMPLE_RATE = 48000;

  /// Bytes before the Opus frame: little-endian u16 sequence number, u32 timestamp (48 kHz samples).
  constexpr std::size_t HEADER_SIZE = 6;

  /// Largest Opus frame a client may send (`LI_MAX_MICROPHONE_FRAME_LENGTH` in moonlight-common-c).
  constexpr std::size_t MAX_FRAME_BYTES = 400;

  /**
   * @brief One microphone message, decoded. `opus` points into the message.
   */
  struct frame_t {
    std::uint16_t sequence;
    std::uint32_t timestamp;
    std::string_view opus;
  };

  /**
   * @brief Splits a microphone message into its header and Opus frame.
   * @return Nothing for a runt, an empty frame or an oversized one.
   */
  inline std::optional<frame_t> parse_frame(std::string_view payload) {
    if (payload.size() <= HEADER_SIZE || payload.size() - HEADER_SIZE > MAX_FRAME_BYTES) {
      return std::nullopt;
    }

    auto byte = [&payload](std::size_t i) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload[i]));
    };

    frame_t frame;
    frame.sequence = static_cast<std::uint16_t>(byte(0) | byte(1) << 8);
    frame.timestamp = byte(2) | byte(3) << 8 | byte(4) << 16 | byte(5) << 24;
    frame.opus = payload.substr(HEADER_SIZE);
    return frame;
  }

  /**
   * @brief Tells, from sequence numbers, how many frames were lost before each one.
   *
   * Frames travel unsequenced, so they can arrive late or twice. A late frame is dropped: its
   * slot has already been filled by concealment, and playing it now would only add delay.
   */
  class sequence_tracker_t {
  public:
    /// A gap this large is a restart of the client's counter, not a loss worth concealing.
    static constexpr int MAX_CONCEALED_FRAMES = 5;

    /**
     * @return The number of frames lost just before this one (0 when none), or nothing when
     *         the frame is a duplicate or arrived after a newer one.
     */
    std::optional<int> accept(std::uint16_t sequence) {
      if (!_last) {
        _last = sequence;
        return 0;
      }

      auto delta = static_cast<std::int16_t>(static_cast<std::uint16_t>(sequence - *_last));
      if (delta <= 0) {
        return std::nullopt;
      }

      _last = sequence;
      int lost = delta - 1;
      return lost > MAX_CONCEALED_FRAMES ? 0 : lost;
    }

    void reset() {
      _last.reset();
    }

  private:
    std::optional<std::uint16_t> _last;
  };

  /**
   * @brief Decoded mono samples between the network and the audio device.
   *
   * Packets arrive in bursts while the device consumes at a steady rate. The buffer only starts
   * playing once it holds `target` samples, and whenever it runs dry it waits for `target` again,
   * so a gap costs one short silence instead of a stutter on every packet.
   *
   * The client's capture clock and the host's playback clock never match exactly. When the client
   * runs faster, samples pile up: past `max`, the oldest are dropped back down to `target`, trading
   * one skip for bounded latency.
   */
  class jitter_buffer_t {
  public:
    jitter_buffer_t(std::size_t target_samples, std::size_t max_samples):
        _target {target_samples},
        _max {std::max(max_samples, target_samples)} {
    }

    /**
     * @brief Appends decoded samples.
     * @return How many samples were dropped to stay under the maximum.
     */
    std::size_t push(const float *samples, std::size_t count) {
      _samples.insert(_samples.end(), samples, samples + count);

      if (_samples.size() <= _max) {
        return 0;
      }

      auto dropped = _samples.size() - _target;
      _samples.erase(_samples.begin(), _samples.begin() + static_cast<std::ptrdiff_t>(dropped));
      return dropped;
    }

    /**
     * @brief Fills `count` samples for the device, with silence where there is no audio.
     * @return How many real samples were written.
     */
    std::size_t pull(float *out, std::size_t count) {
      if (!_playing) {
        if (_samples.size() < _target) {
          std::fill_n(out, count, 0.0f);
          return 0;
        }
        _playing = true;
      }

      auto available = std::min(count, _samples.size());
      std::copy_n(_samples.begin(), available, out);
      _samples.erase(_samples.begin(), _samples.begin() + static_cast<std::ptrdiff_t>(available));
      std::fill_n(out + available, count - available, 0.0f);

      if (_samples.empty()) {
        _playing = false;
      }

      return available;
    }

    std::size_t level() const {
      return _samples.size();
    }

    bool playing() const {
      return _playing;
    }

    void clear() {
      _samples.clear();
      _playing = false;
    }

  private:
    std::size_t _target;
    std::size_t _max;
    std::deque<float> _samples;
    bool _playing = false;
  };
}  // namespace mic_passthrough
