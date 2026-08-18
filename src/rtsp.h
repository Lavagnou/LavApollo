/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <memory>
#include <list>
#include <string>
#include <vector>

// local includes
#include "crypto.h"
#include "thread_safe.h"

#ifdef _WIN32
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

#ifdef _WIN32
  /**
   * @brief Upper bound on the displays a client may ask us to emulate.
   *
   * The driver itself has been measured to hold at least five monitors at once, so this
   * is a sanity limit on untrusted input rather than a hardware one.
   */
  constexpr std::size_t MAX_VIRTUAL_DISPLAYS = 4;

  /**
   * @brief One emulated display of a client's monitor layout.
   *
   * Rectangles live in "canvas" space: the client normalises its physical monitor layout
   * so the bounding box starts at (0,0), and sends that bounding box as `mode=`. Keeping
   * the canvas geometrically identical to the host desktop is deliberate -- it is what
   * lets absolute mouse, touch and pen coordinates map straight through with no protocol
   * change at all, because make_port() in video.cpp derives the client plane from the
   * captured display's own offset and size.
   */
  struct virtual_display_t {
    GUID guid {};
    int x {};
    int y {};
    int width {};
    int height {};
    bool primary {};

    /// `\\.\DISPLAYn`, filled in by proc_t::execute() once the display actually exists.
    std::wstring device_name;
  };
#endif

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    crypto::PERM perm;

    bool input_only;
    bool host_audio;
    int width;
    int height;
    int fps;
    int gcmap;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool virtual_display;
    uint32_t scale_factor;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

  #ifdef _WIN32
    /**
     * The displays to emulate for this session, in canvas space.
     *
     * Populated from `displayLayout=` when the client asked for a specific monitor
     * layout. A client that did not ask still ends up with exactly one entry here, added
     * by proc_t::execute(), so creation, layout and teardown all have a single code path
     * and the single-display case cannot drift away from the multi-display one.
     */
    std::vector<virtual_display_t> virtual_displays;
  #endif
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  std::shared_ptr<stream::session_t>
  find_session(const std::string_view& uuid);

  std::list<std::string>
  get_all_session_uuids();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
