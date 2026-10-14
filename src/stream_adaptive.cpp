/**
 * @file src/stream_adaptive.cpp
 * @brief Adaptive FEC and bitrate controller driven by the client's per-frame FEC reports.
 */
// standard includes
#include <algorithm>

// local includes
#include "stream_adaptive.h"

namespace stream {
  using namespace std::literals;

  namespace {
    // A window is "in trouble" when it lost a frame, or when at least this share of its frames
    // were near misses (1 in 50, i.e. about one per second at 60 fps).
    constexpr std::uint32_t NEAR_MISS_RATIO_DENOMINATOR = 50;

    constexpr int FEC_STEP_UP = 10;
    constexpr int FEC_STEP_DOWN = 5;
    constexpr auto FEC_UP_INTERVAL = 1s;
    constexpr auto FEC_DOWN_QUIET = 10s;

    constexpr int BITRATE_CUT_PERCENT = 80;  // keep 80%, i.e. cut 20%
    constexpr int BITRATE_RESTORE_PERCENT_OF_NEGOTIATED = 10;
    constexpr auto BITRATE_CUT_INTERVAL = 2s;
    constexpr auto BITRATE_RESTORE_QUIET = 15s;
    constexpr int BITRATE_ABSOLUTE_FLOOR_KBPS = 500;

    std::uint16_t read_be16(const unsigned char *p) {
      return (std::uint16_t)((p[0] << 8) | p[1]);
    }

    std::uint32_t read_be32(const unsigned char *p) {
      return ((std::uint32_t) p[0] << 24) | ((std::uint32_t) p[1] << 16) | ((std::uint32_t) p[2] << 8) | p[3];
    }
  }  // namespace

  std::optional<frame_fec_status_t> parse_frame_fec_status(std::string_view payload) {
    if (payload.size() < FRAME_FEC_STATUS_SIZE) {
      return std::nullopt;
    }

    auto p = (const unsigned char *) payload.data();

    frame_fec_status_t status;
    status.frame_index = read_be32(p);
    status.highest_received_sequence_number = read_be16(p + 4);
    status.next_contiguous_sequence_number = read_be16(p + 6);
    status.missing_packets_before_highest_received = read_be16(p + 8);
    status.total_data_packets = read_be16(p + 10);
    status.total_parity_packets = read_be16(p + 12);
    status.received_data_packets = read_be16(p + 14);
    status.received_parity_packets = read_be16(p + 16);
    status.fec_percentage = p[18];
    status.multi_fec_block_index = p[19];
    status.multi_fec_block_count = p[20];
    return status;
  }

  adaptive_controller_t::adaptive_controller_t(const config_t &config, clock::time_point now):
      config_ {config},
      fec_percentage_ {std::clamp(config.base_fec_percentage, 1, 255)},
      fec_floor_ {std::clamp(std::min(config.base_fec_percentage, 10), 1, 255)},
      fec_ceiling_ {std::clamp(std::max(config.base_fec_percentage, 55), 1, 255)},
      bitrate_kbps_ {config.negotiated_bitrate_kbps},
      bitrate_floor_kbps_ {std::min(std::max(config.negotiated_bitrate_kbps / 4, BITRATE_ABSOLUTE_FLOOR_KBPS), config.negotiated_bitrate_kbps)},
      bitrate_active_ {config.adaptive_bitrate && config.negotiated_bitrate_kbps > 0 && config.encoder_supports_bitrate_change},
      window_start_ {now},
      last_trouble_ {now},
      last_unrecovered_ {now},
      last_fec_change_ {now},
      last_bitrate_change_ {now} {
  }

  void adaptive_controller_t::on_frame_sent() {
    ++frames_sent_;
  }

  void adaptive_controller_t::on_fec_status(const frame_fec_status_t &status) {
    const auto received = (std::uint32_t) status.received_data_packets + status.received_parity_packets;

    if (received < status.total_data_packets) {
      // Not enough shards arrived to rebuild the data: the frame is gone.
      ++unrecovered_frames_;
      return;
    }

    // Recovered. Each missing data shard was rebuilt from one parity shard, so this is how much
    // of the parity margin the frame used up.
    const auto missing_data = status.total_data_packets > status.received_data_packets ?
                                status.total_data_packets - status.received_data_packets :
                                0;
    if (status.total_parity_packets > 0 && missing_data * 2 > status.total_parity_packets) {
      ++near_miss_frames_;
    }
  }

  void adaptive_controller_t::on_recovery_request() {
    ++unrecovered_frames_;
  }

  adaptive_controller_t::decision_t adaptive_controller_t::evaluate(clock::time_point now) {
    decision_t decision {fec_percentage_, bitrate_kbps_};

    if (now - window_start_ < WINDOW) {
      return decision;
    }

    const bool lost_frames = unrecovered_frames_ > 0;
    const bool near_misses = frames_sent_ > 0 && near_miss_frames_ * NEAR_MISS_RATIO_DENOMINATOR >= frames_sent_;
    const bool troubled = lost_frames || near_misses;

    troubled_streak_ = troubled ? troubled_streak_ + 1 : 0;
    unrecovered_streak_ = lost_frames ? unrecovered_streak_ + 1 : 0;
    if (troubled) {
      last_trouble_ = now;
    }
    if (lost_frames) {
      last_unrecovered_ = now;
    }

    window_start_ = now;
    frames_sent_ = 0;
    unrecovered_frames_ = 0;
    near_miss_frames_ = 0;

    if (config_.adaptive_fec) {
      // Two troubled windows in a row, so a single burst does not move anything.
      if (troubled_streak_ >= 2 && fec_percentage_ < fec_ceiling_ && now - last_fec_change_ >= FEC_UP_INTERVAL) {
        fec_percentage_ = std::min(fec_percentage_ + FEC_STEP_UP, fec_ceiling_);
        last_fec_change_ = now;
        decision.fec_changed = true;
      } else if (!troubled && fec_percentage_ > fec_floor_ && now - last_trouble_ >= FEC_DOWN_QUIET && now - last_fec_change_ >= FEC_DOWN_QUIET) {
        fec_percentage_ = std::max(fec_percentage_ - FEC_STEP_DOWN, fec_floor_);
        last_fec_change_ = now;
        decision.fec_changed = true;
      }
    }

    if (bitrate_active_) {
      // No amount of parity outruns a saturated link. Once FEC has nothing left to give and
      // frames are still being lost, the channel is narrower than the stream.
      const bool fec_exhausted = !config_.adaptive_fec || fec_percentage_ >= fec_ceiling_;

      if (unrecovered_streak_ >= 2 && fec_exhausted && bitrate_kbps_ > bitrate_floor_kbps_ && now - last_bitrate_change_ >= BITRATE_CUT_INTERVAL) {
        bitrate_kbps_ = std::max(bitrate_kbps_ * BITRATE_CUT_PERCENT / 100, bitrate_floor_kbps_);
        last_bitrate_change_ = now;
        decision.bitrate_changed = true;
      } else if (!lost_frames && bitrate_kbps_ < config_.negotiated_bitrate_kbps && now - last_unrecovered_ >= BITRATE_RESTORE_QUIET && now - last_bitrate_change_ >= BITRATE_RESTORE_QUIET) {
        const auto step = std::max(config_.negotiated_bitrate_kbps * BITRATE_RESTORE_PERCENT_OF_NEGOTIATED / 100, 1);
        bitrate_kbps_ = std::min(bitrate_kbps_ + step, config_.negotiated_bitrate_kbps);
        last_bitrate_change_ = now;
        decision.bitrate_changed = true;
      }
    }

    decision.fec_percentage = fec_percentage_;
    decision.bitrate_kbps = bitrate_kbps_;
    return decision;
  }
}  // namespace stream
