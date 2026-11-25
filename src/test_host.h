/**
 * @file src/test_host.h
 * @brief Test host mode: LavApollo run as a sandboxed fake PC for automated client tests.
 *
 * Nothing here is active unless the config key `test_host = enabled` is set.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace test_host {
  /**
   * @brief Whether test host mode is on (config key `test_host`).
   */
  bool enabled();

  /**
   * @brief Directory the mode writes its logs to: `<appdata>/testhost`.
   */
  std::filesystem::path dir();

  /**
   * @brief Append one JSON line to `dir()/<stream>.jsonl` ("input", "commands" or "events").
   *
   * The event gets a `"t"` field (milliseconds since the process started) unless it already has one.
   * Thread safe. A write failure (unwritable directory, ...) never throws: one warning is
   * logged per file and failure (a later success re-arms it). A directory that disappears is recreated.
   */
  void log(std::string_view stream, const nlohmann::json &event);

  /**
   * @brief Guard for a code path that touches the real system (display driver, GPU profiles, Windows tweaks).
   *
   * In test mode: logs one `test host: skipping <what>` line and returns true, the caller takes its safe
   * early-out. Otherwise returns false and does nothing.
   */
  bool skip(std::string_view what);

  // Runtime state of the fake PC, driven by the testbench through the unauthenticated, 127.0.0.1-only
  // `/api/test/*` routes of the web server (confighttp.cpp), which exist in test host mode only.

  /**
   * @brief The display mode `serverinfo` announces from now on (`WIDTHxHEIGHTxREFRESH`), in place of
   * `test_host_display`: a mid-session mode change (`POST /api/test/display`). Empty = back to the config value.
   */
  void set_display(const std::string &mode);

  /**
   * @brief The mode set by set_display(), if any.
   */
  std::optional<std::string> display();

  /**
   * @brief Make the next `what` request fail (`POST /api/test/fail`): "launch" and "resume" answer 503,
   * "cancel" answers success but leaves the app running (what a host does when another device started it).
   */
  void fail_next(const std::string &what);

  /**
   * @brief Whether a failure of `what` was asked for; consumes it.
   */
  bool take_failure(const std::string &what);

  /**
   * @brief The fake PC's clipboard (the real one is never touched in test mode).
   */
  std::string clipboard();
  void set_clipboard(const std::string &text);
}  // namespace test_host
