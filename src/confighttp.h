/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <string>

// local includes
#include "thread_safe.h"

#define WEB_DIR SUNSHINE_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  void start();

  /**
   * @brief Whether the unauthenticated `POST /api/test/pin` route exists: test host mode only.
   */
  bool test_pin_route_enabled();

  /**
   * @brief Whether a request from this (normalized) address may use `POST /api/test/pin`: 127.0.0.1 only.
   */
  bool test_pin_address_allowed(const std::string &address);
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"map", "application/json"},
  {"mjs", "application/javascript"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"webmanifest", "application/manifest+json"},
  {"webp", "image/webp"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
