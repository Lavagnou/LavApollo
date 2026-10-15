/**
 * @file src/mic_passthrough.h
 * @brief Client microphone passthrough: a client's microphone, played into a virtual microphone
 *        on the host so voice chat and games can record it.
 *
 * Frames arrive as Opus inside the encrypted control stream (`MICROPHONE_DATA_PTYPE`, see
 * mic_passthrough_policy.h). One session owns the virtual microphone at a time: the first that
 * asks for it. Windows only; elsewhere every call reports the feature as unavailable.
 */
#pragma once

// standard includes
#include <string>
#include <string_view>

// local includes
#include "crypto.h"

namespace mic_passthrough {
  /**
   * @brief What the host can do for a client's microphone, published in `serverinfo`.
   */
  enum class state_e {
    ready,  ///< A client that asks will be heard
    disabled,  ///< `mic_passthrough` is off on the host
    no_permission,  ///< This client lacks the Microphone permission
    no_device,  ///< No virtual microphone (Steam Streaming Microphone or `mic_sink`) is installed
  };

  std::string_view to_string(state_e state);

  /**
   * @brief The state for a client holding `perm`. Never touches the audio stack: the device check
   *        is refreshed every few seconds by `init()`'s thread, and serverinfo runs on the HTTPS
   *        thread, which must never block.
   */
  state_e state(crypto::PERM perm);

  /**
   * @brief Starts playing this session's microphone into the virtual microphone.
   * @param owner The session; identifies it in `submit()` and `release()`.
   * @return `false` when another session already owns it or no device could be opened.
   */
  bool acquire(const void *owner, const std::string &device_name);

  /**
   * @brief Stops playing, if `owner` holds the virtual microphone, and restores the default
   *        recording device it replaced.
   */
  void release(const void *owner);

  /**
   * @brief Plays one microphone message (header + Opus frame). Ignored unless `owner` holds the
   *        virtual microphone. Called from the control thread.
   */
  void submit(const void *owner, std::string_view payload);

  /**
   * @brief At startup, on a background thread: installs Steam Streaming Microphone if it is the
   *        configured device, the Steam audio drivers may be installed, and it is missing; then
   *        keeps checking whether the virtual microphone exists, which is what `state()` reports.
   */
  void init();
}  // namespace mic_passthrough
