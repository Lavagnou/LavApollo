/**
 * @file src/platform/windows/virtual_display_recovery.cpp
 * @brief Re-creates a session's virtual displays when they disappear mid-stream.
 */
#include "virtual_display_recovery.h"

#include <algorithm>
#include <condition_variable>
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
    std::mutex monitor_mutex;
    std::jthread monitor_thread;

    // Waits for the interval or a stop request, whichever comes first. True when stopping.
    bool wait_or_stop(std::stop_token stop_token, steady_clock::duration interval) {
      std::mutex m;
      std::condition_variable_any cv;
      std::unique_lock lock {m};
      return cv.wait_for(lock, stop_token, interval, [] {
        return false;
      });
    }

    bool recreate(std::vector<display_spec_t> &displays, const hooks_t &hooks, std::stop_token stop_token) {
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
    monitor_thread = {};  // requests stop and joins the previous monitor, if any
    if (displays.empty()) {
      return;
    }
    monitor_thread = std::jthread([displays = std::move(displays), hooks = std::move(hooks)](std::stop_token stop_token) mutable {
      run(std::move(displays), std::move(hooks), stop_token);
    });
  }

  void stop() {
    std::lock_guard lock {monitor_mutex};
    monitor_thread = {};
  }
}  // namespace VDISPLAY::recovery
