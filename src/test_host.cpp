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

    std::mutex state_mutex;
    std::optional<std::string> display_override;
    std::set<std::string> failures;
    std::string clipboard_text;
    int fake_other_clients = 0;
    long long fake_elapsed_offset = 0;
  }  // namespace

  void set_session(int other_clients, long long elapsed_offset_s) {
    std::lock_guard lock {state_mutex};
    fake_other_clients = other_clients < 0 ? 0 : other_clients;
    fake_elapsed_offset = elapsed_offset_s < 0 ? 0 : elapsed_offset_s;
  }

  int other_clients() {
    std::lock_guard lock {state_mutex};
    return fake_other_clients;
  }

  long long elapsed_offset() {
    std::lock_guard lock {state_mutex};
    return fake_elapsed_offset;
  }

  void set_display(const std::string &mode) {
    std::lock_guard lock {state_mutex};
    if (mode.empty()) {
      display_override.reset();
    } else {
      display_override = mode;
    }
  }

  std::optional<std::string> display() {
    std::lock_guard lock {state_mutex};
    return display_override;
  }

  void fail_next(const std::string &what) {
    std::lock_guard lock {state_mutex};
    failures.insert(what);
  }

  bool take_failure(const std::string &what) {
    std::lock_guard lock {state_mutex};
    return failures.erase(what) > 0;
  }

  std::string clipboard() {
    std::lock_guard lock {state_mutex};
    return clipboard_text;
  }

  void set_clipboard(const std::string &text) {
    std::lock_guard lock {state_mutex};
    clipboard_text = text;
  }

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
