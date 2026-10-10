/**
 * @file tests/unit/platform/test_virtual_display_recovery_policy.cpp
 * @brief When the virtual-display recovery monitor re-creates a lost display.
 */
#include "../../tests_common.h"

#include "src/platform/windows/virtual_display_recovery_policy.h"

namespace {
  using namespace VDISPLAY::recovery;

  // Feeds the same observation every check_interval until the policy acts or `limit` passes.
  std::optional<steady_clock::time_point> run_until_recover(policy_t &policy, bool present, steady_clock::time_point &now, steady_clock::duration limit) {
    const auto end = now + limit;
    for (; now <= end; now += check_interval) {
      if (policy.observe(present, now) == action_e::recover) {
        return now;
      }
    }
    return std::nullopt;
  }
}  // namespace

TEST(VirtualDisplayRecoveryPolicy, PresentDisplaysNeedNothing) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  EXPECT_FALSE(run_until_recover(policy, true, now, 1min));
}

TEST(VirtualDisplayRecoveryPolicy, WaitsOutTheGraceBeforeRecovering) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  const auto lost_at = now;
  const auto recover_at = run_until_recover(policy, false, now, 5s);
  ASSERT_TRUE(recover_at);
  EXPECT_GE(*recover_at - lost_at, missing_grace);
  EXPECT_LT(*recover_at - lost_at, missing_grace + 2 * check_interval);
}

TEST(VirtualDisplayRecoveryPolicy, BlipShorterThanGraceIsIgnored) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  EXPECT_EQ(policy.observe(false, now), action_e::none);
  now += missing_grace / 2;
  EXPECT_EQ(policy.observe(true, now), action_e::none);
  now += missing_grace;
  // Missing again: the grace starts over instead of acting at once
  EXPECT_EQ(policy.observe(false, now), action_e::none);
}

TEST(VirtualDisplayRecoveryPolicy, FlappingDisplayBacksOffThenGivesUp) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  int recoveries = 0;
  // A display that vanishes again right after every successful re-creation
  for (int i = 0; i < 1000 && !policy.has_given_up(); i++) {
    if (const auto at = run_until_recover(policy, false, now, 2min)) {
      recoveries++;
      policy.attempted(true, *at);
    } else {
      break;
    }
  }
  EXPECT_TRUE(policy.has_given_up());
  EXPECT_EQ(recoveries, attempts_per_cycle * (max_backoff_cycles + 1));
}

TEST(VirtualDisplayRecoveryPolicy, StableDisplayRefillsTheBudget) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  for (int i = 0; i < attempts_per_cycle - 1; i++) {
    const auto at = run_until_recover(policy, false, now, 1min);
    ASSERT_TRUE(at);
    policy.attempted(true, *at);
  }
  // Back for longer than the stable requirement
  run_until_recover(policy, true, now, stable_requirement + 1s);

  // The next loss is handled within the grace, not after a backoff
  const auto lost_at = now;
  const auto at = run_until_recover(policy, false, now, 10s);
  ASSERT_TRUE(at);
  EXPECT_LT(*at - lost_at, missing_grace + 2 * check_interval);
}

TEST(VirtualDisplayRecoveryPolicy, FailedAttemptRetriesWithoutCooldown) {
  policy_t policy;
  auto now = steady_clock::time_point {} + 1h;
  const auto first = run_until_recover(policy, false, now, 5s);
  ASSERT_TRUE(first);
  policy.attempted(false, *first);
  const auto second = run_until_recover(policy, false, now, 5s);
  ASSERT_TRUE(second);
  EXPECT_LT(*second - *first, success_cooldown);
}
