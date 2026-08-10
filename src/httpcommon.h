/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <string>

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"
#include "uuid.h"

namespace http {

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  int reload_user_creds(const std::string &file);
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);

  // Download `url` to `file`, invoking `progress_cb(downloaded, total)` as bytes arrive.
  // Follows redirects (GitHub release assets redirect to a CDN). Blocks until complete.
  bool download_file_with_progress(
    const std::string &url,
    const std::string &file,
    std::function<void(uint64_t, uint64_t)> progress_cb,
    long ssl_version = CURL_SSLVERSION_TLSv1_2
  );

  // GET `url` and return the response body as a string (sets a User-Agent; follows redirects).
  std::string simple_get(const std::string &url, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
