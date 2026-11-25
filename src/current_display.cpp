/**
 * @file src/current_display.cpp
 * @brief The mode of the display the stream captures, for serverinfo.
 */
#include "current_display.h"

#include <algorithm>
#include <charconv>
#include <climits>

#include "config.h"
#include "test_host.h"

#ifdef _WIN32
  #include "platform/common.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/virtual_display.h"
  #include "process.h"
#endif

namespace current_display {
  std::optional<display_mode_t> combine(const std::vector<display_mode_t> &modes) {
    if (modes.empty()) {
      return std::nullopt;
    }

    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    int refresh = INT_MAX;
    for (const auto &m : modes) {
      left = std::min(left, m.x);
      top = std::min(top, m.y);
      right = std::max(right, m.x + m.width);
      bottom = std::max(bottom, m.y + m.height);
      refresh = std::min(refresh, m.refresh);
    }
    return display_mode_t {left, top, right - left, bottom - top, refresh};
  }

  std::optional<display_mode_t> parse_mode(std::string_view text) {
    int values[3] {};
    const int limits[3] {16384, 16384, 1000};
    for (int i = 0; i < 3; ++i) {
      const auto end = i < 2 ? text.find('x') : text.size();
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      const auto part = text.substr(0, end);
      const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), values[i]);
      if (ec != std::errc {} || ptr != part.data() + part.size() || values[i] < 1 || values[i] > limits[i]) {
        return std::nullopt;
      }
      text.remove_prefix(i < 2 ? end + 1 : end);
    }
    return display_mode_t {0, 0, values[0], values[1], values[2]};
  }

  display_mode_t test_host_mode() {
    return parse_mode(config::sunshine.test_host_display).value_or(test_host_default_mode);
  }

#ifdef _WIN32
  namespace {
    std::optional<display_mode_t> query(const std::wstring &device_name) {
      if (device_name.empty()) {
        return std::nullopt;
      }
      DEVMODEW dev_mode {};
      dev_mode.dmSize = sizeof(dev_mode);
      if (!VDISPLAY::getDeviceSettings(device_name.c_str(), dev_mode)) {
        return std::nullopt;
      }
      return display_mode_t {
        static_cast<int>(dev_mode.dmPosition.x),
        static_cast<int>(dev_mode.dmPosition.y),
        static_cast<int>(dev_mode.dmPelsWidth),
        static_cast<int>(dev_mode.dmPelsHeight),
        static_cast<int>(dev_mode.dmDisplayFrequency),
      };
    }

    std::optional<display_mode_t> query_all(const std::vector<std::string> &names) {
      std::vector<display_mode_t> modes;
      for (const auto &name : names) {
        if (auto m = query(platf::from_utf8(name))) {
          modes.push_back(*m);
        }
      }
      // Every display must answer, or the canvas would be measured wrong
      if (modes.size() != names.size()) {
        return std::nullopt;
      }
      return combine(modes);
    }
  }  // namespace

  std::optional<display_mode_t> captured() {
    // Test host mode: a fixed mode, never the user's real monitor (its synthetic capture has no display)
    if (test_host::enabled()) {
      return test_host_mode();
    }

    // A running session captures its own display(s): a virtual display, or a '+'-joined layout
    if (proc::proc.running()) {
      auto display_name = proc::proc.get_display_name();
      if (!display_name.empty()) {
        if (auto m = query_all(platf::split_display_names(display_name))) {
          return m;
        }
      }
    }

    // Otherwise what a session without a virtual display would capture
    if (!config::video.output_name.empty()) {
      if (auto m = query(platf::from_utf8(config::video.output_name))) {
        return m;
      }
    }
    return query(VDISPLAY::getPrimaryDisplay());
  }
#else
  std::optional<display_mode_t> captured() {
    if (test_host::enabled()) {
      return test_host_mode();
    }
    return std::nullopt;
  }
#endif
}  // namespace current_display
