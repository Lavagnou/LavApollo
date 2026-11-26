/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"

#include <iostream>

#ifdef _WIN32
  #include <windows.h>
#endif

namespace {
  // test_scratch next to the test binary (test_utils::scratch_dir)
  std::filesystem::path scratch_root(const char *argv0) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, sizeof(exe) / sizeof(exe[0]));
    if (n > 0 && n < sizeof(exe) / sizeof(exe[0])) {
      return std::filesystem::path(exe).parent_path() / "test_scratch";
    }
#endif
    return std::filesystem::absolute(argv0).parent_path() / "test_scratch";
  }
}  // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);

  test_utils::scratch_dir() = scratch_root(argv[0]);
  std::error_code ec;
  std::filesystem::create_directories(test_utils::scratch_dir(), ec);
  if (ec) {
    std::cerr << "Cannot create the tests' scratch folder " << test_utils::scratch_dir().string() << ": " << ec.message() << std::endl;
    return 1;
  }
  testing::AddGlobalTestEnvironment(new SunshineEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new SunshineEventListener);
  return RUN_ALL_TESTS();
}
