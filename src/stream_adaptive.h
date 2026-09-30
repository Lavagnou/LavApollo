/**
 * @file src/stream_adaptive.h
 * @brief Adaptive FEC and bitrate controller driven by the client's per-frame FEC reports.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace stream {
  /**
   * @brief Moonlight's `SS_FRAME_FEC_STATUS` (control packet type 0x5502), decoded.
   *
   * The client sends one for every frame that needed FEC to be recovered or could not be
   * recovered at all (`RtpVideoQueue.c`, `reportFinalFrameFecStatus()`), and nothing for a frame
   * that arrived intact. On the wire the fields are packed and big-endian.
   */
  struct frame_fec_status_t {
    std::uint32_t frame_index;
    std::uint16_t highest_received_sequence_number;
    std::uint16_t next_contiguous_sequence_number;
    std::uint16_t missing_packets_before_highest_received;
    std::uint16_t total_data_packets;
    std::uint16_t total_parity_packets;
    std::uint16_t received_data_packets;
    std::uint16_t received_parity_packets;
    std::uint8_t fec_percentage;
    std::uint8_t multi_fec_block_index;
    std::uint8_t multi_fec_block_count;
  };

  /// Size of `SS_FRAME_FEC_STATUS` on the wire.
  constexpr std::size_t FRAME_FEC_STATUS_SIZE = 21;

  /// Control packet type of `SS_FRAME_FEC_STATUS` (client to host).
  constexpr std::uint16_t FRAME_FEC_STATUS_PTYPE = 0x5502;

  /**
   * @brief Decode a `SS_FRAME_FEC_STATUS` payload.
   * @return The status, or nothing if the payload is too short to hold one.
   */
  std::optional<frame_fec_status_t> parse_frame_fec_status(std::string_view payload);

  /**
   * @brief Decides the FEC percentage and encoder bitrate of one session from what the client
   *        reports about the frames it receives.
   *
   * Pure logic: no session, no clock, no threads. The caller feeds it events and asks for a
   * decision with the current time, which is what makes it testable. It is not thread-safe; the
   * session guards it with a mutex.
   *
   * The signal is the client's per-frame FEC status, not a raw packet loss rate. A frame whose
   * losses the FEC repaired costs nothing visible, so it does not justify more FEC by itself --
   * steering on raw loss used to push FEC to its ceiling on links where every loss was already
   * absorbed. What does justify it:
   *  - an **unrecovered** frame (not enough packets arrived to rebuild it), which costs an IDR or
   *    a reference frame invalidation on the client, or an explicit recovery request from it;
   *  - frequent **near misses**, frames repaired using more than half of their parity, which
   *    predict unrecovered ones.
   *
   * FEC is added on top of the video bitrate (Sunshine's behaviour), so raising it is the first
   * resort. Only when frames are still lost with FEC at its ceiling is the encoder bitrate cut,
   * and only on encoders that can change it mid-stream.
   */
  class adaptive_controller_t {
  public:
    using clock = std::chrono::steady_clock;

    struct config_t {
      bool adaptive_fec = true;
      bool adaptive_bitrate = true;

      /// The configured `fec_percentage`: starting point, and bound for the floor and ceiling.
      int base_fec_percentage = 20;

      /// Bitrate the client asked for. Never exceeded.
      int negotiated_bitrate_kbps = 0;

      /// Whether the active encoder can change its bitrate without restarting.
      bool encoder_supports_bitrate_change = false;
    };

    struct decision_t {
      int fec_percentage;
      int bitrate_kbps;
      bool fec_changed = false;
      bool bitrate_changed = false;
    };

    /// Length of one observation window.
    static constexpr auto WINDOW = std::chrono::seconds(1);

    adaptive_controller_t(const config_t &config, clock::time_point now);

    /// A video frame left the host.
    void on_frame_sent();

    /// The client reported a frame that needed FEC or could not be recovered.
    void on_fec_status(const frame_fec_status_t &status);

    /// The client asked for an IDR or a reference frame invalidation: a frame was lost.
    void on_recovery_request();

    /**
     * @brief Close the observation window if it has elapsed, and adjust.
     *
     * Cheap when the window is still open, so it can be called for every frame.
     */
    decision_t evaluate(clock::time_point now);

    int fec_percentage() const {
      return fec_percentage_;
    }

    int bitrate_kbps() const {
      return bitrate_kbps_;
    }

    /// Adaptive bitrate is enabled, negotiated, and supported by the encoder.
    bool bitrate_active() const {
      return bitrate_active_;
    }

    int fec_floor() const {
      return fec_floor_;
    }

    int fec_ceiling() const {
      return fec_ceiling_;
    }

    int bitrate_floor_kbps() const {
      return bitrate_floor_kbps_;
    }

  private:
    config_t config_;

    int fec_percentage_;
    int fec_floor_;
    int fec_ceiling_;

    int bitrate_kbps_;
    int bitrate_floor_kbps_;
    bool bitrate_active_;

    // Counters of the window in progress
    clock::time_point window_start_;
    std::uint32_t frames_sent_ = 0;
    std::uint32_t unrecovered_frames_ = 0;
    std::uint32_t near_miss_frames_ = 0;

    // Consecutive windows that were in trouble / that lost frames
    int troubled_streak_ = 0;
    int unrecovered_streak_ = 0;

    clock::time_point last_trouble_;
    clock::time_point last_unrecovered_;
    clock::time_point last_fec_change_;
    clock::time_point last_bitrate_change_;
  };
}  // namespace stream
