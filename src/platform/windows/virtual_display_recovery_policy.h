/**
 * @file src/platform/windows/virtual_display_recovery_policy.h
 * @brief When to re-create a session's virtual displays after they disappear.
 *
 * Pure timing logic, kept free of Windows so it can be unit tested. The timings follow
 * Vibepollo's recovery monitor (Nonary, virtual_display_sudovda.cpp).
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace VDISPLAY::recovery {
  using steady_clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  /// How often the monitor checks that every display is still attached.
  constexpr auto check_interval = 150ms;
  /// A display can vanish for a moment during a mode or topology change; wait this long first.
  constexpr auto missing_grace = 500ms;
  /// After a re-creation, let Windows settle before judging the displays again.
  constexpr auto success_cooldown = 2s;
  /// Displays present this long count as recovered, and the attempt budget is refilled.
  constexpr auto stable_requirement = 10s;
  /// Attempts (successful or not) allowed before backing off: a display that keeps vanishing
  /// right after being re-created should not be re-created in a tight loop.
  constexpr int attempts_per_cycle = 3;
  constexpr auto first_backoff = 5s;
  constexpr auto max_backoff = 60s;
  /// Backoff cycles before the monitor stops trying for the rest of the session.
  constexpr int max_backoff_cycles = 5;

  enum class action_e {
    none,  ///< Nothing to do yet
    recover,  ///< Re-create the missing displays now
  };

  class policy_t {
  public:
    /**
     * @brief Feed one observation.
     * @param all_present Whether every display of the session is attached to the desktop.
     */
    action_e observe(bool all_present, steady_clock::time_point now) {
      if (all_present) {
        missing_since.reset();
        if (!present_since) {
          present_since = now;
        } else if (now - *present_since >= stable_requirement) {
          attempts = 0;
          backoff_cycles = 0;
        }
        return action_e::none;
      }

      present_since.reset();
      if (gave_up || now < next_allowed) {
        return action_e::none;
      }
      if (!missing_since) {
        missing_since = now;
        return action_e::none;
      }
      return now - *missing_since >= missing_grace ? action_e::recover : action_e::none;
    }

    /**
     * @brief Report how a recovery attempt went.
     */
    void attempted(bool succeeded, steady_clock::time_point now) {
      missing_since.reset();
      if (++attempts >= attempts_per_cycle) {
        attempts = 0;
        if (++backoff_cycles > max_backoff_cycles) {
          gave_up = true;
          return;
        }
        const auto backoff = std::min<steady_clock::duration>(first_backoff * (1 << (backoff_cycles - 1)), max_backoff);
        next_allowed = now + backoff;
        return;
      }
      next_allowed = succeeded ? now + success_cooldown : now;
    }

    bool has_given_up() const {
      return gave_up;
    }

  private:
    std::optional<steady_clock::time_point> missing_since;
    std::optional<steady_clock::time_point> present_since;
    steady_clock::time_point next_allowed {};
    int attempts = 0;
    int backoff_cycles = 0;
    bool gave_up = false;
  };
}  // namespace VDISPLAY::recovery
