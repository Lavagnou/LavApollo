/**
 * @file src/stream.h
 * @brief Declarations for the streaming protocols.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace rtsp_stream {
  struct launch_session_t;
}  // namespace rtsp_stream

namespace stream {
  constexpr auto VIDEO_STREAM_PORT = 9;
  constexpr auto CONTROL_PORT = 10;
  constexpr auto AUDIO_STREAM_PORT = 11;

  constexpr std::string_view video_format_name(int video_format) {
    switch (video_format) {
      case 0:
        return "H.264";
      case 1:
        return "HEVC";
      case 2:
        return "AV1";
      case 3:
        return "PyroWave";
      default:
        return "Unknown";
    }
  }

  /**
   * @brief One codec spelling for the session history, whatever the caller wrote.
   */
  inline std::string canonical_codec_name(std::string_view codec) {
    std::string lowered {codec};
    for (char &ch : lowered) {
      if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
      }
    }
    if (lowered == "h264" || lowered == "h.264") {
      return "H.264";
    }
    if (lowered == "h265" || lowered == "hevc") {
      return "HEVC";
    }
    if (lowered == "av1") {
      return "AV1";
    }
    if (lowered == "pyrowave") {
      return "PyroWave";
    }
    return std::string {codec};
  }

  struct session_t;

  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int mlFeatureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    uint32_t encryptionFlagsEnabled;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,  ///< The session is stopped
      STOPPING,  ///< The session is stopping
      STARTING,  ///< The session is starting
      RUNNING,  ///< The session is running
    };

    std::shared_ptr<session_t> alloc(config_t &config, rtsp_stream::launch_session_t &launch_session);
    std::string uuid(const session_t& session);
    bool uuid_match(const session_t& session, const std::string_view& uuid);
    bool update_device_info(session_t& session, const std::string& name, const crypto::PERM& newPerm);
    int start(session_t &session, const std::string &addr_string);
    void stop(session_t &session);
    void graceful_stop(session_t& session);
    void join(session_t &session);
    state_e state(session_t &session);
    inline bool send(session_t& session, const std::string_view &payload);
  }  // namespace session

  /**
   * @brief A running session's settings and counters, for the session history and live stats.
   */
  struct session_info_t {
    std::string uuid;  ///< The history id, one per session (not the client's UUID)
    std::string device_name;
    int width;
    int height;
    int fps;
    int encoder_bitrate_kbps;
    int requested_bitrate_kbps;
    int video_format;  ///< 0=H.264, 1=HEVC, 2=AV1, 3=PyroWave
    bool hdr;
    bool yuv444;
    int audio_channels;
    std::string stream_gpu_model;

    std::uint64_t frames_sent;
    std::uint64_t packets_sent;
    std::uint64_t bytes_sent;
    std::uint32_t idr_requests;
    std::uint32_t invalidate_ref_count;
    std::int64_t client_reported_losses;
    double encode_latency_ms;  ///< Latest frame's capture-to-send latency
    std::int64_t last_frame_index;
    double uptime_seconds;
  };

  /**
   * @brief Snapshot of every running video session.
   */
  std::vector<session_info_t> get_all_session_info();
}  // namespace stream
