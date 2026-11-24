/**
 * @file src/test_host.cpp
 * @brief Test host mode: see test_host.h.
 */
// standard includes
#include <chrono>
#include <fstream>
#include <mutex>
#include <set>
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

  bool skip(std::string_view what) {
    if (!enabled()) {
      return false;
    }
    BOOST_LOG(info) << "test host: skipping "sv << what;
    return true;
  }

  static void log_unchecked(std::string_view stream, const nlohmann::json &event) {
    nlohmann::json line = event.is_object() ? event : nlohmann::json {{"value", event}};
    if (!line.contains("t")) {
      line["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    }

    std::lock_guard lock {log_mutex};
    static std::set<std::filesystem::path> reported;  // files a failure was already reported for

    auto directory = dir();
    auto file = directory / (std::string {stream} + ".jsonl");

    // Open first; only create the directory when that fails, so it comes back by itself if it goes away.
    std::error_code ec;
    std::ofstream out {file, std::ios::app};
    if (!out) {
      std::filesystem::create_directories(directory, ec);
      if (!ec) {
        out.clear();
        out.open(file, std::ios::app);
      }
    }
    if (!out) {
      // These files are the only evidence channel of the mode: say so once instead of dropping silently.
      if (reported.insert(file).second) {
        BOOST_LOG(warning) << "Test host: cannot write "sv << file.string() << (ec ? " ("s + ec.message() + ")" : ""s);
      }
      return;
    }
    reported.erase(file);  // a later failure is worth a new warning
    // Client-controlled text can be invalid UTF-8: replace it (the event stays recorded) instead of throwing.
    out << line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
  }

  void log(std::string_view stream, const nlohmann::json &event) {
    try {
      log_unchecked(stream, event);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Test host: dropped a log event: "sv << e.what();
    } catch (...) {
      BOOST_LOG(warning) << "Test host: dropped a log event"sv;
    }
  }
}  // namespace test_host
