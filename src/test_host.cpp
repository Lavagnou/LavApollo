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
#include "logging.h"
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
    if (!line.contains("t")) {
      line["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    }

    std::lock_guard lock {log_mutex};
    static std::filesystem::path ready_dir;  // last directory created successfully
    static std::filesystem::path warned_for;  // last file a failure was reported for

    auto directory = dir();
    auto file = directory / (std::string {stream} + ".jsonl");

    std::error_code ec;
    if (directory != ready_dir) {
      std::filesystem::create_directories(directory, ec);
      if (!ec) {
        ready_dir = directory;
      }
    }

    std::ofstream out;
    if (!ec) {
      out.open(file, std::ios::app);
    }
    if (!out) {
      // These files are the only evidence channel of the mode: say so once per file instead of dropping silently.
      if (warned_for != file) {
        warned_for = file;
        BOOST_LOG(warning) << "Test host: cannot write "sv << file.string() << (ec ? " ("s + ec.message() + ")" : ""s);
      }
      return;
    }
    out << line.dump() << '\n';
  }
}  // namespace test_host
