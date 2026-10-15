/**
 * @file tests/unit/platform/test_virtual_display_recovery_stop.cpp
 * @brief Stopping the virtual-display recovery monitor never hangs its caller.
 */
#include "../../tests_common.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

#include "src/platform/windows/virtual_display_recovery.h"

using namespace std::literals;

// The monitor polls EnumDisplayDevicesW, which Windows can hold in the kernel indefinitely.
// proc::terminate() stops it from the /cancel HTTPS handler, so an unbounded join there froze
// the whole HTTPS server. A hook that blocks stands in for the stuck display call.
TEST(VirtualDisplayRecoveryStop, ReturnsWhenTheMonitorIsStuck) {
  auto entered = std::make_shared<std::promise<void>>();
  auto entered_future = entered->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto release_future = release->get_future().share();
  auto finished = std::make_shared<std::atomic_bool>(false);

  VDISPLAY::recovery::display_spec_t display {};
  display.label = "test";
  display.device_name = L"\\\\.\\DISPLAY_LAVAPOLLO_TEST_MISSING";

  VDISPLAY::recovery::hooks_t hooks;
  hooks.ensure_driver = [entered, release_future, finished]() {
    entered->set_value();
    release_future.wait();
    finished->store(true);
    // Not ready: nothing gets created even if this ever got past the stop check
    return false;
  };

  // A display that is never attached: after the grace period, the monitor tries to recover it
  VDISPLAY::recovery::start({display}, hooks);
  ASSERT_EQ(entered_future.wait_for(10s), std::future_status::ready);

  auto start = std::chrono::steady_clock::now();
  VDISPLAY::recovery::stop();
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(elapsed, 3s);

  // Let the abandoned monitor go: it must wind down on its own
  release->set_value();
  for (int i = 0; i < 100 && !finished->load(); ++i) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_TRUE(finished->load());
  std::this_thread::sleep_for(300ms);
}

TEST(VirtualDisplayRecoveryStop, StopsAnIdleMonitorPromptly) {
  VDISPLAY::recovery::display_spec_t display {};
  display.label = "test";
  display.device_name = L"\\\\.\\DISPLAY_LAVAPOLLO_TEST_MISSING";

  VDISPLAY::recovery::start({display}, {});

  auto start = std::chrono::steady_clock::now();
  VDISPLAY::recovery::stop();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);

  // Stopping again is harmless
  VDISPLAY::recovery::stop();
}
