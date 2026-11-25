/**
 * @file src/current_display.h
 * @brief The mode of the display the stream captures, for serverinfo.
 *
 * A LavArtemis client that joins a running session, or launches without a virtual display,
 * streams at this display's aspect and refresh instead of its own (HostDisplayFit on the clients).
 */
#pragma once

#include <optional>
#include <string_view>
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

  /// The mode a test host announces when its fixture sets no `test_host_display`.
  inline constexpr display_mode_t test_host_default_mode {0, 0, 1920, 1080, 60};

  /**
   * @brief Parse a `WIDTHxHEIGHTxREFRESH` mode such as `1920x1080x60` (config key `test_host_display`).
   * Nothing when malformed, or when a value is not in 1..16384 (refresh 1..1000).
   */
  std::optional<display_mode_t> parse_mode(std::string_view text);

  /**
   * @brief The fixed mode a test host announces: `test_host_display`, else test_host_default_mode.
   * Never a real display: the stream size a test client negotiates must not depend on the monitor of
   * the PC the tests run on, nor change when that monitor changes mode.
   */
  display_mode_t test_host_mode();

  /**
   * @brief The current mode of what a stream captures: in test host mode the fixed test_host_mode();
   * otherwise the running session's display(s), else the configured output, else the primary display.
   * Nothing when it cannot be read (non-Windows).
   * Called from serverinfo on the HTTPS thread: only quick reads, no waiting on display calls.
   */
  std::optional<display_mode_t> captured();
}  // namespace current_display
