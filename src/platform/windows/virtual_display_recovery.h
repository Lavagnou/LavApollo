/**
 * @file src/platform/windows/virtual_display_recovery.h
 * @brief Re-creates a session's virtual displays when they disappear mid-stream.
 *
 * A SudoVDA display can vanish under a running stream: the driver's watchdog fires when
 * our pings stall, the driver restarts after an update, or someone disables the monitor.
 * Capture then falls back to whatever display is left, usually the physical one. The
 * monitor started here notices, re-creates the displays with the same GUIDs and modes,
 * and asks the capture thread to go back to them.
 *
 * Ported in spirit from Vibepollo's recovery monitor (Nonary), reduced to what LavApollo's
 * session model needs: one monitor per stream, covering every display of its layout.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>

namespace VDISPLAY::recovery {
  /**
   * @brief Everything needed to bring one display back exactly as it was created.
   */
  struct display_spec_t {
    GUID guid;
    std::string serial;
    std::string label;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_millihz;
    /// GDI device name; updated in place after every re-creation.
    std::wstring device_name;
  };

  struct hooks_t {
    /// Reopen the driver and restart its watchdog pings if they are not running. Returns readiness.
    std::function<bool()> ensure_driver;
    /// Called once every display is back, with their new device names, before capture is
    /// asked to reopen them. Puts them back in place and publishes the new names.
    std::function<void(const std::vector<display_spec_t> &)> on_recreated;
  };

  /**
   * @brief Start watching these displays. Replaces any monitor already running.
   */
  void start(std::vector<display_spec_t> displays, hooks_t hooks);

  /**
   * @brief Stop watching. Must be called before the displays are removed on purpose,
   *        or the monitor would put them straight back.
   */
  void stop();
}  // namespace VDISPLAY::recovery
