/**
 * @file tests/unit/test_benchmark_app.cpp
 * @brief Test how /applist treats the hidden Benchmark app (src/nvhttp.cpp).
 */
#include "../tests_common.h"

#include <src/nvhttp.h>

namespace {
  constexpr int BENCHMARK = 4242;
  constexpr int GAME = 1111;
}  // namespace

TEST(BenchmarkAppTests, HiddenWhileNotRunning) {
  EXPECT_FALSE(nvhttp::benchmark_app_listed(BENCHMARK, 0, BENCHMARK));
  EXPECT_FALSE(nvhttp::benchmark_app_listed(BENCHMARK, GAME, BENCHMARK));
}

TEST(BenchmarkAppTests, ListedWhileRunningSoItCanBeQuit) {
  EXPECT_TRUE(nvhttp::benchmark_app_listed(BENCHMARK, BENCHMARK, BENCHMARK));
}

TEST(BenchmarkAppTests, OtherAppsUnaffected) {
  EXPECT_TRUE(nvhttp::benchmark_app_listed(GAME, 0, BENCHMARK));
  EXPECT_TRUE(nvhttp::benchmark_app_listed(GAME, BENCHMARK, BENCHMARK));
}

TEST(BenchmarkAppTests, NothingHiddenWhenNotOffered) {
  EXPECT_TRUE(nvhttp::benchmark_app_listed(GAME, 0, -1));
  EXPECT_TRUE(nvhttp::benchmark_app_listed(-1, 0, -1));
}
