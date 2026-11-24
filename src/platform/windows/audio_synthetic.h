/**
 * @file src/platform/windows/audio_synthetic.h
 * @brief Test host mode audio: a 440 Hz tone instead of a capture, and no change to the Windows audio setup.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

namespace platf::audio {
  /**
   * @brief Audio control for test host mode. Never opens, changes or installs anything on the system.
   *
   * `set_sink` does nothing, the sink is a made-up one, `microphone()` returns a real-time 440 Hz tone.
   */
  std::unique_ptr<::platf::audio_control_t> synthetic_audio_control();
}  // namespace platf::audio
