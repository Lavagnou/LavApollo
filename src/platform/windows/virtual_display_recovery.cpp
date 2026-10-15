/**
 * @file src/platform/windows/virtual_display_recovery.cpp
 * @brief Re-creates a session's virtual displays when they disappear mid-stream.
 */
#include "virtual_display_recovery.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "virtual_display.h"
#include "virtual_display_recovery_policy.h"

using namespace std::literals;

namespace VDISPLAY::recovery {
  namespace {
    /// How long stopping waits for the monitor. It polls EnumDisplayDevicesW, which Windows can
    /// hold in the kernel indefinitely while the display stack is wedged, and stop() is reached
    /// from HTTPS handlers (/cancel, /launch through proc::terminate): joining without a bound
    /// once froze the whole HTTPS server, and every client saw the host as offline.
    constexpr auto stop_timeout = 2s;

    /// Set by the monitor thread as its very last step.
    struct finished_t {
      std::mutex mutex;
      std::condition_variable cv;
      bool done = false;
    };

    std::mutex monitor_mutex;
    std::jthread monitor_thread;
    std::shared_ptr<finished_t> monitor_finished;

    /// Stops the running monitor, waiting at most stop_timeout. A monitor stuck in a display
    /// call is left behind: its stop token is set, so it exits without acting once Windows
    /// lets go of it, and it only owns copies of what it needs.
    void stop_locked() {
      if (!monitor_thread.joinable()) {
        return;
      }

      monitor_thread.request_stop();

      bool finished;
      {
        std::unique_lock lock {monitor_finished->mutex};
        finished = monitor_finished->cv.wait_for(lock, stop_timeout, [&] {
          return monitor_finished->done;
        });
      }

      if (finished) {
        monitor_thread.join();
      } else {
        BOOST_LOG(warning) << "Virtual display recovery: the monitor is stuck in a Windows display call; leaving it behind"sv;
        monitor_thread.detach();
      }

      monitor_thread = {};
      monitor_finished.reset();
    }

    // Waits for the interval or a stop request, whichever comes first. True when stopping.
    bool wait_or_stop(std::stop_token stop_token, steady_clock::duration interval) {
      std::mutex m;
      std::condition_variable_any cv;
      std::unique_lock lock {m};
      cv.wait_for(lock, stop_token, interval, [] {
        return false;
      });
      // Not wait_for()'s result: it returns the predicate, false here even once stop is
      // requested. Returning it made the monitor spin forever after stop(), and stop()'s join
      // (from /cancel) froze the HTTPS server.
      return stop_token.stop_requested();
    }

    bool recreate(std::vector<display_spec_t> &displays, const hooks_t &hooks, std::stop_token stop_token) {
      // A monitor left behind by stop_locked() may get here long after its session ended
      if (stop_token.stop_requested()) {
        return false;
      }
      if (hooks.ensure_driver && !hooks.ensure_driver()) {
        BOOST_LOG(warning) << "Virtual display recovery: the driver is not ready"sv;
        return false;
      }

      for (auto &display : displays) {
        if (stop_token.stop_requested()) {
          return false;
        }
        if (isDisplayAttached(display.device_name)) {
          continue;
        }

        // The driver may still hold a detached display under this GUID; adding over it fails.
        removeVirtualDisplay(display.guid);

        auto device_name = createVirtualDisplay(display.serial.c_str(), display.label.c_str(), display.width, display.height, display.refresh_millihz, display.guid);
        if (device_name.empty()) {
          BOOST_LOG(warning) << "Virtual display recovery: could not re-create \""sv << display.label << '"';
          return false;
        }

        BOOST_LOG(info) << "Virtual display recovery: \""sv << display.label << "\" is back as "sv
                        << platf::to_utf8(device_name) << " (was "sv << platf::to_utf8(display.device_name) << ')';
        display.device_name = std::move(device_name);
        changeDisplaySettings(display.device_name.c_str(), display.width, display.height, display.refresh_millihz);
      }

      if (stop_token.stop_requested()) {
        return false;
      }
      if (hooks.on_recreated) {
        hooks.on_recreated(displays);
      }
      mail::man->event<bool>(mail::recapture_display)->raise(true);
      return true;
    }

    void run(std::vector<display_spec_t> displays, hooks_t hooks, std::stop_token stop_token) {
      policy_t policy;
      while (!wait_or_stop(stop_token, check_interval)) {
        const bool all_present = std::all_of(displays.begin(), displays.end(), [](const auto &display) {
          return isDisplayAttached(display.device_name);
        });

        if (policy.observe(all_present, steady_clock::now()) != action_e::recover) {
          continue;
        }

        BOOST_LOG(warning) << "Virtual display recovery: a virtual display of this session disappeared, re-creating it"sv;
        const bool succeeded = recreate(displays, hooks, stop_token);
        policy.attempted(succeeded, steady_clock::now());
        if (policy.has_given_up()) {
          BOOST_LOG(error) << "Virtual display recovery: giving up for this session after repeated failures"sv;
          return;
        }
      }
    }
  }  // namespace

  void start(std::vector<display_spec_t> displays, hooks_t hooks) {
    std::lock_guard lock {monitor_mutex};
    stop_locked();
    if (displays.empty()) {
      return;
    }
    monitor_finished = std::make_shared<finished_t>();
    monitor_thread = std::jthread([displays = std::move(displays), hooks = std::move(hooks), finished = monitor_finished](std::stop_token stop_token) mutable {
      // run() owns the displays and hooks: both are gone by the time it returns
      run(std::move(displays), std::move(hooks), stop_token);
      {
        std::lock_guard lock {finished->mutex};
        finished->done = true;
      }
      finished->cv.notify_all();
    });
  }

  void stop() {
    std::lock_guard lock {monitor_mutex};
    stop_locked();
  }
}  // namespace VDISPLAY::recovery
