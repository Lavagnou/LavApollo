/**
 * @file src/test_host.cpp
 * @brief Test host mode: see test_host.h.
 */
// standard includes
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

// local includes
#include "config.h"
#include "platform/common.h"
#include "test_host.h"

namespace test_host {
  namespace {
    const auto started = std::chrono::steady_clock::now();
    std::mutex log_mutex;
  }  // namespace

  bool enabled() {
    return config::sunshine.test_host;
  }

  std::filesystem::path dir() {
    return platf::appdata() / "testhost";
  }

  void log(std::string_view stream, const nlohmann::json &event) {
    nlohmann::json line = event.is_object() ? event : nlohmann::json {{"value", event}};
    line["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();

    std::lock_guard lock {log_mutex};
    std::error_code ec;
    std::filesystem::create_directories(dir(), ec);
    std::ofstream out {dir() / (std::string {stream} + ".jsonl"), std::ios::app};
    if (out) {
      out << line.dump() << '\n';
    }
  }
}  // namespace test_host
