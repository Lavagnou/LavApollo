/**
 * @file src/platform/windows/audio_endpoints.h
 * @brief Audio endpoint lookups shared with client microphone passthrough.
 *
 * Every function here uses COM: the calling thread must have initialized it.
 */
#pragma once

// standard includes
#include <optional>
#include <string>

namespace platf::audio {
  /**
   * @brief The virtual microphone client microphones are played into.
   */
  struct mic_endpoints_t {
    std::wstring render_id;  ///< Playback endpoint the host writes to
    std::wstring capture_id;  ///< Its recording side, what applications use; empty if not found
    std::string name;  ///< Playback endpoint's friendly name, for logs
  };

  /**
   * @brief Finds the virtual microphone.
   * @param mic_sink `config::audio.mic_sink`: empty for Steam Streaming Microphone, otherwise any
   *        name `virtual_sink` accepts. The recording side is the capture endpoint on the same
   *        adapter (true of Steam's microphone and of VB-Cable).
   */
  std::optional<mic_endpoints_t> find_mic_endpoints(const std::string &mic_sink);

  /**
   * @brief The default recording device for `role` (an `ERole`), or an empty string.
   */
  std::wstring default_capture_id(int role);

  /**
   * @brief Makes `id` the default recording device for `role` (an `ERole`).
   */
  bool set_default_capture(const std::wstring &id, int role);

  /**
   * @brief Installs the Steam Streaming Microphone driver when Steam provides it and it is missing.
   *        Needs administrator rights, like the speakers driver.
   */
  void install_steam_microphone_if_missing();
}  // namespace platf::audio
