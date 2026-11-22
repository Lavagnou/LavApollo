/**
 * @file src/current_display.cpp
 * @brief The mode of the display the stream captures, for serverinfo.
 */
#include "current_display.h"

#include <algorithm>
#include <climits>

#ifdef _WIN32
  #include "config.h"
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
    return std::nullopt;
  }
#endif
}  // namespace current_display
