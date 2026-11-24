/**
 * @file src/test_host.h
 * @brief Test host mode: LavApollo run as a sandboxed fake PC for automated client tests.
 *
 * Nothing here is active unless the config key `test_host = enabled` is set.
 */
#pragma once

// standard includes
#include <filesystem>
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
}  // namespace test_host
