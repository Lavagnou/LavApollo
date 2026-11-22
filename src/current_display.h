/**
 * @file src/current_display.h
 * @brief The mode of the display the stream captures, for serverinfo.
 *
 * A LavArtemis client that joins a running session, or launches without a virtual display,
 * streams at this display's aspect and refresh instead of its own (HostDisplayFit on the clients).
 */
#pragma once

#include <optional>
#include <vector>

namespace current_display {
  struct display_mode_t {
    int x;
    int y;
    int width;
    int height;
    int refresh;
  };

  /**
   * @brief One mode for several displays: their bounding box (the emulated multi-display canvas)
   * at the slowest refresh. Empty input gives nothing.
   */
  std::optional<display_mode_t> combine(const std::vector<display_mode_t> &modes);

  /**
   * @brief The current mode of what a stream captures: the running session's display(s), else the
   * configured output, else the primary display. Nothing when it cannot be read (non-Windows).
   * Called from serverinfo on the HTTPS thread: only quick reads, no waiting on display calls.
   */
  std::optional<display_mode_t> captured();
}  // namespace current_display
