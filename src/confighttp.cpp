/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <numeric>
#include <algorithm>

// lib includes
#include <boost/algorithm/string.hpp>
#include <display_device/json.h>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "host_stats.h"
#include "httpcommon.h"
#include "log_export.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "video.h"
#include "session_history.h"
#include "stream.h"
#include "utility.h"
#include "updater.h"
#include "uuid.h"
#include "web_auth.h"

#ifdef _WIN32
  #include "platform/windows/frame_limiter.h"
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  /**
   * @brief Every web UI login, persisted next to the state file (web_auth.h).
   * @details Its own file: `nvhttp::save_state()` rebuilds `sunshine_state.json` from scratch.
   */
  web_auth::session_store_t &web_sessions() {
    static web_auth::session_store_t store {
      std::filesystem::path {config::nvhttp.file_state}.parent_path() / "lavapollo_auth.json",
      web_auth::default_dependencies()
    };
    return store;
  }

  std::string request_header(const req_https_t &request, const char *name) {
    const auto it = request->header.find(name);
    return it == request->header.end() ? std::string {} : it->second;
  }

  std::string request_cookie(const req_https_t &request, std::string_view name) {
    return web_auth::cookie_value(request_header(request, "cookie"), name);
  }

  /**
   * @brief Adds the cookies of a freshly issued token pair.
   * @details Without "remember me" both are browser-session cookies: closing the browser ends the
   *          login. The server still expires them on its own schedule.
   */
  void add_session_cookies(SimpleWeb::CaseInsensitiveMultimap &headers, const web_auth::tokens_t &tokens) {
    std::optional<std::chrono::seconds> session_age;
    std::optional<std::chrono::seconds> refresh_age;
    if (tokens.remember_me) {
      session_age = tokens.session_ttl;
      refresh_age = tokens.refresh_ttl;
    }
    headers.emplace("Set-Cookie", web_auth::set_cookie(web_auth::session_cookie_name, tokens.session_token, session_age));
    headers.emplace("Set-Cookie", web_auth::set_cookie(web_auth::refresh_cookie_name, tokens.refresh_token, refresh_age));
  }

  void add_cleared_cookies(SimpleWeb::CaseInsensitiveMultimap &headers) {
    headers.emplace("Set-Cookie", web_auth::clear_cookie(web_auth::session_cookie_name));
    headers.emplace("Set-Cookie", web_auth::clear_cookie(web_auth::refresh_cookie_name));
  }

  /**
   * @brief Trades the request's refresh cookie for a new token pair, if it holds a live one.
   */
  std::optional<web_auth::tokens_t> refresh_from_cookie(const req_https_t &request) {
    return web_sessions().refresh(
      request_cookie(request, web_auth::refresh_cookie_name),
      request_header(request, "user-agent"),
      net::addr_to_normalized_string(request->remote_endpoint().address()),
      config::sunshine.session_token_ttl
    );
  }

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;
    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }
    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_unauthorized;
    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- redirecting"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Check if the IP origin is allowed.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if allowed, false otherwise.
   */
  bool checkIPOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }

  /**
   * @brief Authenticate the request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param needsRedirect Whether to redirect on failure.
   * @return True if authenticated, false otherwise.
   *
   * Requires a live session cookie (web_auth.h). A page load whose session token has run out but
   * whose refresh cookie is still good is refreshed here and redirected to itself; API calls get a
   * 401 and refresh through `/api/auth/refresh` (the web UI's fetch wrapper does it).
   */
  bool authenticate(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (!checkIPOrigin(response, request))
      return false;
    // If credentials not set, redirect to welcome.
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }
    if (web_sessions().validate(request_cookie(request, web_auth::session_cookie_name))) {
      return true;
    }

    if (!needsRedirect) {
      send_unauthorized(response, request);
      return false;
    }
    if (auto tokens = refresh_from_cookie(request)) {
      SimpleWeb::CaseInsensitiveMultimap headers {
        {"X-Frame-Options", "DENY"},
        {"Content-Security-Policy", "frame-ancestors 'none';"}
      };
      add_session_cookies(headers, *tokens);
      headers.emplace("Location", request->path + (request->query_string.empty() ? "" : "?" + request->query_string));
      response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
      return false;
    }
    std::string redir_path = "/login?redir=.";
    redir_path += request->path;
    send_redirect(response, request, redir_path.c_str());
    return false;
  }

  /**
   * @brief Issued CSRF tokens, by client address (`GET /api/csrf-token`).
   * @details Only needed by pages that are not served from the address they call -- the web UI's own
   *          requests pass on their `Origin` alone.
   */
  struct csrf_tokens_t {
    std::mutex mutex;
    std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>, std::less<>> tokens;
  };

  csrf_tokens_t &csrf_tokens() {
    static csrf_tokens_t instance;
    return instance;
  }

  constexpr auto CSRF_TOKEN_LIFETIME = std::chrono::hours(1);

  /**
   * @brief Refuses a state-changing request sent by a page that is not the web UI (CSRF).
   * @details The session cookies are SameSite=Strict, which already keeps them off cross-site
   *          requests; this also covers same-site pages on another port or name. Allowed: no
   *          `Origin`/`Referer` at all (not a browser page), the request's own `Host` (whatever
   *          address the UI was opened by), `csrf_allowed_origins`, or a valid `X-CSRF-Token`.
   * @return False after answering 403.
   */
  bool check_csrf(resp_https_t response, req_https_t request) {
    auto origin = request_header(request, "origin");
    if (origin.empty()) {
      origin = request_header(request, "referer");
    }
    if (origin.empty() ||
        web_auth::origin_allowed(origin, request_header(request, "host"), config::sunshine.csrf_allowed_origins)) {
      return true;
    }

    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (const auto token = request_header(request, "x-csrf-token"); !token.empty()) {
      auto &csrf = csrf_tokens();
      std::scoped_lock lock {csrf.mutex};
      const auto it = csrf.tokens.find(address);
      if (it != csrf.tokens.end() && it->second.second > std::chrono::steady_clock::now() && it->second.first == token) {
        return true;
      }
    }

    BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- "sv << request->method << ' ' << request->path
                       << " refused: sent from "sv << origin << ", not from the web UI"sv;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(SimpleWeb::StatusCode::client_error_forbidden);
    tree["status"] = false;
    tree["error"] = "Cross-origin request refused";
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::client_error_forbidden, tree.dump(), headers);
    return false;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_not_found;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["error"] = "Not Found";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_bad_request;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }


  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The required content type.
   */
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view& contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }

    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;

    return true;
  }

  /**
   * @brief Get the index page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getIndexPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the PIN page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPinPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "pin.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the apps page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAppsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "apps.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    response->write(content, headers);
  }

  /**
   * @brief Get the clients page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getClientsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "clients.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the configuration page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfigPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "config.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the password page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPasswordPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "password.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the login page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo Combine this function with getWelcomePage if appropriate.
   */
  void getLoginPage(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "login.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the welcome page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getWelcomePage(resp_https_t response, req_https_t request) {
    print_req(request);

    if (!config::sunshine.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "welcome.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the troubleshooting page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getTroubleshootingPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "troubleshooting.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/apollo.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Apollo logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getApolloLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-apollo-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);

    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }

    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      file_tree["current_app"] = proc::proc.get_running_app_uuid();
      file_tree["host_uuid"] = http::unique_id;
      file_tree["host_name"] = config::nvhttp.sunshine_name;

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application the UUID must be empty.
   *        To update an existing application, you must provide the current UUID of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file.",
   *   "uuid": "aaaa-bbbb"
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","uuid": "aaaa-bbbb"}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    BOOST_LOG(info) << config::stream.file_apps;
    try {
      // TODO: Input Validation

      // Read the input JSON from the request body.
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Migrate/merge the new app into the file tree.
      proc::migrate_apps(&fileTree, &inputTree);

      // Write the updated file tree back to disk.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the output response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Reorder applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/reorder| POST| {"order": ["aaaa-bbbb", "cccc-dddd"]}}
   */
  void reorderApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();

      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Get the desired order of UUIDs from the request.
      if (!input_tree.contains("order") || !input_tree["order"].is_array()) {
        throw std::runtime_error("Missing or invalid 'order' array in request body");
      }
      const auto& order_uuids_json = input_tree["order"];

      // Get the original apps array from the fileTree.
      // Default to an empty array if "apps" key is missing or if it's present but not an array (after logging an error).
      nlohmann::json original_apps_list = nlohmann::json::array();
      if (fileTree.contains("apps")) {
        if (fileTree["apps"].is_array()) {
          original_apps_list = fileTree["apps"];
        } else {
          // "apps" key exists but is not an array. This is a malformed state.
          BOOST_LOG(error) << "ReorderApps: 'apps' key in apps configuration file ('" << config::stream.file_apps
                           << "') is present but not an array.";
          throw std::runtime_error("'apps' in file is not an array, cannot reorder.");
        }
      } else {
        // "apps" key is missing. Treat as an empty list. Reordering an empty list is valid.
        BOOST_LOG(debug) << "ReorderApps: 'apps' key missing in apps configuration file ('" << config::stream.file_apps
                         << "'). Treating as an empty list for reordering.";
        // original_apps_list is already an empty array, so no specific action needed here.
      }

      nlohmann::json reordered_apps_list = nlohmann::json::array();
      std::vector<bool> item_moved(original_apps_list.size(), false);

      // Phase 1: Place apps according to the 'order' array from the request.
      // Iterate through the desired order of UUIDs.
      for (const auto& uuid_json_value : order_uuids_json) {
        if (!uuid_json_value.is_string()) {
          BOOST_LOG(warning) << "ReorderApps: Encountered a non-string UUID in the 'order' array. Skipping this entry.";
          continue;
        }
        std::string target_uuid = uuid_json_value.get<std::string>();
        bool found_match_for_ordered_uuid = false;

        // Find the first unmoved app in the original list that matches the current target_uuid.
        for (size_t i = 0; i < original_apps_list.size(); ++i) {
          if (item_moved[i]) {
            continue; // This specific app object has already been placed.
          }

          const auto& app_item = original_apps_list[i];
          // Ensure the app item is an object and has a UUID to match against.
          if (app_item.is_object() && app_item.contains("uuid") && app_item["uuid"].is_string()) {
            if (app_item["uuid"].get<std::string>() == target_uuid) {
              reordered_apps_list.push_back(app_item); // Add the found app object to the new list.
              item_moved[i] = true;                    // Mark this specific object as moved.
              found_match_for_ordered_uuid = true;
              break; // Found an app for this UUID, move to the next UUID in the 'order' array.
            }
          }
        }

        if (!found_match_for_ordered_uuid) {
          // This means a UUID specified in the 'order' array was not found in the original_apps_list
          // among the currently available (unmoved) app objects.
          // Per instruction "If the uuid is missing from the original json file, omit it."
          BOOST_LOG(debug) << "ReorderApps: UUID '" << target_uuid << "' from 'order' array not found in available apps list or its matching app was already processed. Omitting.";
        }
      }

      // Phase 2: Append any remaining apps from the original list that were not explicitly ordered.
      // These are app objects that were not marked 'item_moved' in Phase 1.
      for (size_t i = 0; i < original_apps_list.size(); ++i) {
        if (!item_moved[i]) {
          reordered_apps_list.push_back(original_apps_list[i]);
        }
      }

      // Update the fileTree with the new, reordered list of apps.
      fileTree["apps"] = reordered_apps_list;

      // Write the modified fileTree back to the apps configuration file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));

      // Notify relevant parts of the system that the apps configuration has changed.
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ReorderApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/delete | POST| { uuid: 'aaaa-bbbb' }}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      auto uuid = input_tree["uuid"].get<std::string>();

      // Read the apps file into a nlohmann::json object.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Remove any app with the matching uuid directly from the "apps" array.
      if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
        auto& apps = fileTree["apps"];
        apps.erase(
          std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json& app) {
            return app.value("uuid", "") == uuid;
          }),
          apps.end()
        );
      }

      // Write the updated JSON back to the file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
#ifdef _WIN32
    output_tree["platform"] = "windows";
#endif
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Update client information.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "name": "<Friendly Name>",
   *   "display_mode": "1920x1080x59.94",
   *   "do": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "undo": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "perm": <uint32_t>
   * }
   * @endcode
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      std::string name = input_tree.value("name", "");
      std::string display_mode = input_tree.value("display_mode", "");
      bool enable_legacy_ordering = input_tree.value("enable_legacy_ordering", true);
      bool allow_client_commands = input_tree.value("allow_client_commands", true);
      bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      auto do_cmds = nvhttp::extract_command_entries(input_tree, "do");
      auto undo_cmds = nvhttp::extract_command_entries(input_tree, "undo");
      auto perm = static_cast<crypto::PERM>(input_tree.value("perm", static_cast<uint32_t>(crypto::PERM::_no)) & static_cast<uint32_t>(crypto::PERM::_all));
      output_tree["status"] = nvhttp::update_device_info(
        uuid,
        name,
        display_mode,
        do_cmds,
        undo_cmds,
        perm,
        enable_legacy_ordering,
        allow_client_commands,
        always_use_virtual_display
      );
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/clients/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::unpair_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
#ifdef _WIN32
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#endif
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output_tree[name] = value;
    }
    send_response(response, output_tree);
  }

#ifdef _WIN32
  /**
   * @brief Report what the frame limiter would use and what it is using right now.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/frame-limiter/status| GET| null}
   */
  void getFrameLimiterStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const auto status = platf::frame_limiter_get_status();
    nlohmann::json output_tree;
    output_tree["enabled"] = status.enabled;
    output_tree["configured_provider"] = platf::frame_limiter_provider_to_string(status.configured_provider);
    output_tree["active_provider"] = platf::frame_limiter_provider_to_string(status.active_provider);
    output_tree["nvidia_available"] = status.nvidia_available;
    output_tree["nvcp_ready"] = status.nvcp_ready;
    output_tree["rtss_available"] = status.rtss_available;
    output_tree["disable_vsync"] = status.disable_vsync;
    output_tree["nv_overrides_supported"] = status.nv_overrides_supported;
    output_tree["rtss"] = {
      {"configured_path", status.rtss.configured_path},
      {"resolved_path", status.rtss.resolved_path},
      {"path_exists", status.rtss.path_exists},
      {"hooks_found", status.rtss.hooks_found},
      {"profile_found", status.rtss.profile_found},
      {"can_bootstrap_profile", status.rtss.can_bootstrap_profile},
      {"process_running", status.rtss.process_running},
    };
    send_response(response, output_tree);
  }
#endif

  /**
   * @brief Get the locale setting.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    std::stringstream ss;

    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");
      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));
        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    std::string content = file_handler::read_file(config::sunshine.log_file.c_str());

    // tail=N keeps the last N lines (the v2 log viewer); without it, the whole file
    const auto query = request->parse_query_string();
    if (const auto tail = query.find("tail"); tail != query.end()) {
      std::size_t lines = 0;
      try {
        lines = std::stoul(tail->second);
      } catch (...) {}
      if (lines > 0) {
        std::size_t pos = content.size();
        if (pos > 0 && content[pos - 1] == '\n') {
          --pos;
        }
        for (std::size_t found = 0; pos > 0 && found < lines; --pos) {
          if (content[pos - 1] == '\n' && ++found == lines) {
            break;
          }
        }
        content.erase(0, pos);
      }
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Download the current and previous logs plus the config file as a ZIP,
   *        with user names, IP/MAC addresses and e-mail addresses masked.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs/export| GET| null}
   */
  void exportLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    try {
      logging::log_flush();

      // The previous run's log is what matters after a crash; logging::init() rotates it to ".backup".
      const std::vector<std::filesystem::path> sources {
        config::sunshine.log_file,
        config::sunshine.log_file + ".backup",
        config::sunshine.config_file,
      };
      constexpr std::uintmax_t max_file_bytes = 64 * 1024 * 1024;

      log_export::sanitizer_t sanitizer;
      std::vector<log_export::zip_entry_t> entries;
      for (const auto &path : sources) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
          continue;
        }
        const auto size = std::filesystem::file_size(path, ec);
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec || size > max_file_bytes) {
          continue;
        }
        std::ifstream file(path, std::ios::binary);
        // Bounded read: the active log keeps growing while we collect it.
        std::string data(static_cast<std::size_t>(size), '\0');
        file.read(data.data(), static_cast<std::streamsize>(data.size()));
        data.resize(static_cast<std::size_t>(file.gcount()));
        entries.push_back({path.filename().string(), sanitizer.sanitize(data), mtime});
      }
      if (entries.empty()) {
        bad_request(response, request, "No log files are available");
        return;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/zip");
      headers.emplace("Content-Disposition", "attachment; filename=\"lavapollo_logs.zip\"");
      headers.emplace("Cache-Control", "no-store");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(SimpleWeb::StatusCode::success_ok, log_export::build_zip(entries), headers);
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    if ((!config::sunshine.username.empty() && !authenticate(response, request)) || !validateContentType(response, request, "application/json"))
      return;
    print_req(request);
    std::vector<std::string> errors;
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty())
        newUsername = username;
      if (newUsername.empty()) {
        errors.push_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() ||
            (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            // New credentials end every login, including this one
            web_sessions().revoke_all();
            output_tree["status"] = true;
          }
        } else {
          errors.push_back("Invalid Current Credentials");
        }
      }
      if (!errors.empty()) {
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(),
                                              [](const std::string &a, const std::string &b) {
                                                return a.empty() ? b : a + ", " + b;
                                              });
        bad_request(response, request, error);
        return;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get a one-time password (OTP).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/otp| GET| null}
   */
  void getOTP(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      std::string passphrase = input_tree.value("passphrase", "");
      if (passphrase.empty())
        throw std::runtime_error("Passphrase not provided!");
      if (passphrase.size() < 4)
        throw std::runtime_error("Passphrase too short!");

      std::string deviceName = input_tree.value("deviceName", "");
      output_tree["otp"] = nvhttp::request_otp(passphrase, deviceName);
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.sunshine_name;
      output_tree["status"] = true;
      output_tree["message"] = "OTP created, effective within 3 minutes.";
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "OTP creation failed: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a PIN code to the host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string pin = input_tree.value("pin", "");
      std::string name = input_tree.value("name", "");
      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Quit Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * On Windows, if running in a service, a special shutdown code is returned.
   */
  void quit(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    BOOST_LOG(warning) << "Requested quit from config page!"sv;

    proc::proc.terminate();

#ifdef _WIN32
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
    } else
#endif
    {
      lifetime::exit_sunshine(0, true);
    }
    // If exit fails, write a response after 5 seconds.
    std::thread write_resp([response]{
      std::this_thread::sleep_for(5s);
      response->write();
    });
    write_resp.detach();
  }

  /**
   * @brief Check GitHub for the latest LavApollo release.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/update/check| GET| null}
   */
  void updateCheckGet(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto info = updater::check_for_update();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["current"] = info.current;
    output_tree["latest"] = info.latest;
    output_tree["latest_tag"] = info.latest_tag;
    output_tree["update_available"] = info.update_available;
    output_tree["service_managed"] = info.service_managed;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["release_notes_url"] = info.release_notes_url;
    nlohmann::json asset;
    asset["name"] = info.asset.name;
    asset["url"] = info.asset.url;
    asset["size"] = info.asset.size;
    output_tree["asset"] = asset;
    if (!info.error.empty()) output_tree["error"] = info.error;
    send_response(response, output_tree);
  }

  /**
   * @brief Start downloading (and verifying) the latest installer.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/update/download| POST| null}
   */
  void updateDownloadPost(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto info = updater::check_for_update();
    bool started = updater::start_download(info);
    auto status = updater::get_status();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["started"] = started;
    output_tree["state"] = static_cast<int>(status.state);
    if (!status.message.empty()) output_tree["message"] = status.message;
    send_response(response, output_tree);
  }

  /**
   * @brief Report the current download/install pipeline progress (polled by the UI).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/update/status| GET| null}
   */
  void updateStatusGet(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto status = updater::get_status();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["state"] = static_cast<int>(status.state);
    output_tree["downloaded_bytes"] = status.downloaded_bytes;
    output_tree["total_bytes"] = status.total_bytes;
    output_tree["target_version"] = status.target_version;
    if (!status.message.empty()) output_tree["message"] = status.message;
    send_response(response, output_tree);
  }

  /**
   * @brief Apply the staged update: launch the installer silently and elevated.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Refused (409) while a streaming session is in progress. Does not self-exit — the installer drives the
   * teardown (its uninstall step stops the service, which stops this process).
   *
   * @api_examples{/api/update/install| POST| null}
   */
  void updateInstallPost(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto status = updater::get_status();
    if (status.state != updater::state_e::ready) {
      bad_request(response, request, "The installer is not staged or verified. Download it first.");
      return;
    }

    // Refuse while a session/app is running — the update would kill it mid-stream.
    if (proc::proc.running()) {
      constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_conflict;
      nlohmann::json output_tree;
      output_tree["status_code"] = static_cast<int>(code);
      output_tree["status"] = false;
      output_tree["error"] = "A session is currently in progress. Please close it before updating.";
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      response->write(code, output_tree.dump(), headers);
      return;
    }

    bool launched = updater::stage_installer();
    nlohmann::json output_tree;
    output_tree["status"] = launched;
    output_tree["state"] = static_cast<int>(updater::get_status().state);
    if (!launched) output_tree["error"] = updater::get_status().message;
    send_response(response, output_tree);
  }

  /**
   * @brief Launch an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void launchApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      std::string uuid = input_tree["uuid"].get<std::string>();

      nlohmann::json output_tree;
      const auto &apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          crypto::named_cert_t named_cert {
            .name = "",
            .uuid = http::unique_id,
            .perm = crypto::PERM::_all,
          };
          BOOST_LOG(info) << "Launching app ["sv << app.name << "] from web UI"sv;
          auto launch_session = nvhttp::make_launch_session(true, false, request->parse_query_string(), &named_cert);
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            bad_request(response, request, err == 503 ?
                        "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
                        "Failed to start the specified application");
          } else {
            output_tree["status"] = true;
            send_response(response, output_tree);
          }
          return;
        }
      }
      BOOST_LOG(error) << "Couldn't find app with uuid ["sv << uuid << ']';
      bad_request(response, request, "Cannot find requested application");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "LaunchApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void disconnect(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::find_and_stop_session(uuid, true);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Disconnect: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Login the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "username": "<username>",
   *   "password": "<password>"
   * }
   * @endcode
   */
  namespace {
    /**
     * Failed Web UI logins, per client address.
     *
     * The login endpoint answers as fast as it can hash, so without this a machine on the LAN
     * could try passwords as fast as the network allows. After a few misses an address is
     * locked out for an exponentially growing time instead. Locking out, rather than sleeping
     * in the handler, matters: the server runs its handlers on one thread, so a sleep would
     * stall the Web UI for everyone, including the legitimate user.
     */
    struct login_failures_t {
      int count = 0;
      std::chrono::steady_clock::time_point locked_until {};
      std::chrono::steady_clock::time_point last_failure {};
    };

    constexpr int LOGIN_FREE_ATTEMPTS = 3;
    constexpr auto LOGIN_MAX_LOCKOUT = 30s;
    constexpr std::size_t LOGIN_TRACKED_ADDRESSES = 256;

    std::mutex login_failures_mutex;
    std::unordered_map<std::string, login_failures_t> login_failures;

    /// Seconds the address must still wait before trying again, or 0.
    long long login_lockout_remaining(const std::string &address) {
      std::lock_guard lg {login_failures_mutex};
      auto it = login_failures.find(address);
      if (it == login_failures.end()) {
        return 0;
      }
      auto remaining = std::chrono::ceil<std::chrono::seconds>(it->second.locked_until - std::chrono::steady_clock::now());
      return std::max<long long>(remaining.count(), 0);
    }

    void record_login_failure(const std::string &address) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard lg {login_failures_mutex};

      if (!login_failures.contains(address) && login_failures.size() >= LOGIN_TRACKED_ADDRESSES) {
        // Bounded: forget the address that has been quiet the longest.
        auto oldest = std::min_element(login_failures.begin(), login_failures.end(), [](const auto &a, const auto &b) {
          return a.second.last_failure < b.second.last_failure;
        });
        login_failures.erase(oldest);
      }

      auto &entry = login_failures[address];
      entry.count++;
      entry.last_failure = now;
      if (entry.count >= LOGIN_FREE_ATTEMPTS) {
        // 1 s, 2 s, 4 s... capped.
        auto shift = std::min(entry.count - LOGIN_FREE_ATTEMPTS, 5);
        entry.locked_until = now + std::min<std::chrono::steady_clock::duration>(std::chrono::seconds(1LL << shift), LOGIN_MAX_LOCKOUT);
      }
    }

    void clear_login_failures(const std::string &address) {
      std::lock_guard lg {login_failures_mutex};
      login_failures.erase(address);
    }
  }  // namespace

  void login(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (auto wait = login_lockout_remaining(address); wait > 0) {
      BOOST_LOG(warning) << "Web UI Login from ["sv << address << "] refused: too many failed attempts, "sv << wait << "s left"sv;
      const SimpleWeb::CaseInsensitiveMultimap headers {
        {"Retry-After", std::to_string(wait)}
      };
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests, headers);
      return;
    }

    auto fg = util::fail_guard([&] {
      record_login_failure(address);
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      const bool remember_me = input_tree.value("remember_me", false);
      std::string hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
      if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password) {
        BOOST_LOG(info) << "Web UI: ["sv << address << "] -- login failed"sv;
        return;
      }
      const auto tokens = web_sessions().issue(
        config::sunshine.username,
        remember_me,
        request_header(request, "user-agent"),
        address,
        config::sunshine.session_token_ttl,
        remember_me ? config::sunshine.remember_me_refresh_token_ttl : web_auth::default_refresh_ttl
      );
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- logged in"sv << (remember_me ? " (remembered)"sv : ""sv);

      SimpleWeb::CaseInsensitiveMultimap headers;
      add_session_cookies(headers, tokens);
      // Retire the cookie of the single-session login this replaced
      headers.emplace("Set-Cookie", "auth=; Path=/; Secure; SameSite=Strict; Max-Age=0");
      clear_login_failures(address);
      response->write(headers);
      fg.disable();
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << address << "]: "sv << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      fg.disable();
      return;
    }
  }

  double round_to(double value, double factor) {
    return std::round(value * factor) / factor;
  }

  /**
   * @brief Send a JSON error with the given status.
   */
  void send_error(resp_https_t response, SimpleWeb::StatusCode code, const std::string &message) {
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = message;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(code, tree.dump(), headers);
  }

  nlohmann::json rtsp_session_to_json(const stream::session_info_t &info) {
    nlohmann::json output;
    output["uuid"] = info.uuid;
    output["device_name"] = info.device_name;
    output["width"] = info.width;
    output["height"] = info.height;
    output["fps"] = info.fps;
    output["encoder_bitrate_kbps"] = info.encoder_bitrate_kbps;
    output["requested_bitrate_kbps"] = info.requested_bitrate_kbps;
    output["video_format"] = info.video_format;
    output["codec"] = std::string {stream::video_format_name(info.video_format)};
    output["hdr"] = info.hdr;
    output["yuv444"] = info.yuv444;
    output["audio_channels"] = info.audio_channels;
    output["stream_gpu_model"] = info.stream_gpu_model;
    output["frames_sent"] = info.frames_sent;
    output["packets_sent"] = info.packets_sent;
    output["bytes_sent"] = info.bytes_sent;
    output["idr_requests"] = info.idr_requests;
    output["invalidate_ref_count"] = info.invalidate_ref_count;
    output["client_reported_losses"] = info.client_reported_losses;
    output["encode_latency_ms"] = round_to(info.encode_latency_ms, 10.0);
    output["last_frame_index"] = info.last_frame_index;
    output["uptime_seconds"] = round_to(info.uptime_seconds, 10.0);
    return output;
  }

  nlohmann::json host_stats_to_json(const platf::host_stats_t &stats) {
    nlohmann::json output;
    output["cpu_percent"] = stats.cpu_percent;
    output["cpu_temp_c"] = stats.cpu_temp_c;
    output["ram_used_bytes"] = stats.ram_used_bytes;
    output["ram_total_bytes"] = stats.ram_total_bytes;
    output["ram_percent"] = stats.ram_total_bytes > 0 ?
                              static_cast<double>(stats.ram_used_bytes) * 100.0 / static_cast<double>(stats.ram_total_bytes) :
                              0.0;
    output["gpu_percent"] = stats.gpu_percent;
    output["gpu_encoder_percent"] = stats.gpu_encoder_percent;
    output["gpu_temp_c"] = stats.gpu_temp_c;
    const auto vram_used_bytes = stats.vram_total_bytes > 0 && stats.vram_used_bytes > stats.vram_total_bytes ?
                                   stats.vram_total_bytes :
                                   stats.vram_used_bytes;
    output["vram_used_bytes"] = vram_used_bytes;
    output["vram_total_bytes"] = stats.vram_total_bytes;
    output["vram_percent"] = stats.vram_total_bytes > 0 ?
                               static_cast<double>(vram_used_bytes) * 100.0 / static_cast<double>(stats.vram_total_bytes) :
                               0.0;
    output["net_rx_bps"] = stats.net_rx_bps;
    output["net_tx_bps"] = stats.net_tx_bps;
    return output;
  }

  nlohmann::json host_info_to_json(const platf::host_info_t &info) {
    nlohmann::json output;
    output["cpu_model"] = info.cpu_model;
    output["gpu_model"] = info.gpu_model;
    output["cpu_logical_cores"] = info.cpu_logical_cores;
    output["ram_total_bytes"] = info.ram_total_bytes;
    output["vram_total_bytes"] = info.vram_total_bytes;
    output["net_interface"] = info.net_interface;
    output["net_link_speed_mbps"] = info.net_link_speed_mbps;
    return output;
  }

  nlohmann::json session_summary_to_json(const session_history::session_summary_t &summary) {
    nlohmann::json output;
    output["uuid"] = summary.uuid;
    output["protocol"] = summary.protocol;
    output["client_name"] = summary.client_name;
    output["device_name"] = summary.device_name;
    output["app_name"] = summary.app_name;
    output["width"] = summary.width;
    output["height"] = summary.height;
    output["target_fps"] = summary.target_fps;
    output["encoder_bitrate_kbps"] = summary.encoder_bitrate_kbps;
    output["requested_bitrate_kbps"] = summary.requested_bitrate_kbps;
    output["codec"] = summary.codec;
    output["hdr"] = summary.hdr;
    output["yuv444"] = summary.yuv444;
    output["audio_channels"] = summary.audio_channels;
    output["start_time_unix"] = summary.start_time_unix;
    output["end_time_unix"] = summary.end_time_unix;
    output["duration_seconds"] = round_to(summary.duration_seconds, 10.0);
    output["verdict"] = summary.verdict;
    output["server_version"] = summary.server_version;
    output["host_cpu_model"] = summary.host_cpu_model;
    output["host_gpu_model"] = summary.host_gpu_model;
    output["stream_gpu_model"] = summary.stream_gpu_model;
    return output;
  }

  nlohmann::json session_sample_to_json(const session_history::session_sample_t &sample) {
    // -1 means "not available on this host", and stays -1 instead of being rounded
    const auto metric = [](double value) {
      return value < 0 ? -1.0 : round_to(value, 10.0);
    };
    nlohmann::json output;
    output["session_uuid"] = sample.session_uuid;
    output["timestamp_unix"] = sample.timestamp_unix;
    output["bytes_sent_total"] = sample.bytes_sent_total;
    output["packets_sent_video"] = sample.packets_sent_video;
    output["frames_sent"] = sample.frames_sent;
    output["last_frame_index"] = sample.last_frame_index;
    output["video_dropped"] = sample.video_dropped;
    output["audio_dropped"] = sample.audio_dropped;
    output["client_reported_losses"] = sample.client_reported_losses;
    output["idr_requests"] = sample.idr_requests;
    output["ref_invalidations"] = sample.ref_invalidations;
    output["encode_latency_ms"] = round_to(sample.encode_latency_ms, 10.0);
    output["actual_fps"] = round_to(sample.actual_fps, 10.0);
    output["actual_bitrate_kbps"] = round_to(sample.actual_bitrate_kbps, 10.0);
    output["frame_interval_jitter_ms"] = round_to(sample.frame_interval_jitter_ms, 100.0);
    output["host_cpu_percent"] = metric(sample.host_cpu_percent);
    output["host_gpu_percent"] = metric(sample.host_gpu_percent);
    output["host_gpu_encoder_percent"] = metric(sample.host_gpu_encoder_percent);
    output["host_ram_percent"] = metric(sample.host_ram_percent);
    output["host_vram_percent"] = metric(sample.host_vram_percent);
    output["host_cpu_temp_c"] = metric(sample.host_cpu_temp_c);
    output["host_gpu_temp_c"] = metric(sample.host_gpu_temp_c);
    output["host_net_rx_bps"] = sample.host_net_rx_bps < 0 ? -1.0 : sample.host_net_rx_bps;
    output["host_net_tx_bps"] = sample.host_net_tx_bps < 0 ? -1.0 : sample.host_net_tx_bps;
    return output;
  }

  nlohmann::json session_event_to_json(const session_history::session_event_t &event) {
    nlohmann::json output;
    output["session_uuid"] = event.session_uuid;
    output["timestamp_unix"] = event.timestamp_unix;
    output["event_type"] = event.event_type;
    output["payload"] = event.payload;
    return output;
  }

  nlohmann::json active_session_to_json(const session_history::active_session_t &session) {
    nlohmann::json output;
    output["uuid"] = session.uuid;
    output["protocol"] = session.protocol;
    output["client_name"] = session.client_name;
    output["device_name"] = session.device_name;
    output["app_name"] = session.app_name;
    output["width"] = session.width;
    output["height"] = session.height;
    output["target_fps"] = session.target_fps;
    output["encoder_bitrate_kbps"] = session.encoder_bitrate_kbps;
    output["requested_bitrate_kbps"] = session.requested_bitrate_kbps;
    output["codec"] = session.codec;
    output["hdr"] = session.hdr;
    output["yuv444"] = session.yuv444;
    output["stream_gpu_model"] = session.stream_gpu_model;
    output["uptime_seconds"] = round_to(session.uptime_seconds, 10.0);
    output["actual_fps"] = round_to(session.actual_fps, 10.0);
    output["actual_bitrate_kbps"] = round_to(session.actual_bitrate_kbps, 10.0);
    output["encode_latency_ms"] = round_to(session.encode_latency_ms, 10.0);
    output["frame_interval_jitter_ms"] = round_to(session.frame_interval_jitter_ms, 100.0);
    output["frames_sent"] = session.frames_sent;
    output["bytes_sent"] = session.bytes_sent;
    output["client_reported_losses"] = session.client_reported_losses;
    output["idr_requests"] = session.idr_requests;
    return output;
  }

  nlohmann::json history_status_to_json(const session_history::history_status_t &status) {
    nlohmann::json output;
    output["available"] = status.available;
    output["degraded"] = status.degraded;
    output["dropped_samples"] = status.dropped_samples;
    output["failed_writes"] = status.failed_writes;
    output["pending_control_commands"] = status.pending_control_commands;
    output["pending_priority_commands"] = status.pending_priority_commands;
    output["pending_regular_commands"] = status.pending_regular_commands;
    output["pending_samples"] = status.pending_samples;
    return output;
  }

  /**
   * @brief Whether an app runs and how many clients stream it, for UI messaging.
   *
   * @api_examples{/api/session/status| GET| null}
   */
  void getSessionStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json output_tree;
    const int active = rtsp_stream::session_count();
    const bool app_running = proc::proc.running() > 0;
    output_tree["activeSessions"] = active;
    output_tree["appRunning"] = app_running;
    output_tree["appName"] = app_running ? proc::proc.get_last_run_app_name() : "";
    output_tree["paused"] = app_running && active == 0;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Live host load: CPU, GPU, encoder, memory, temperatures, network (-1: not available).
   *
   * @api_examples{/api/host/stats| GET| null}
   */
  void getHostStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    send_response(response, host_stats_to_json(host_stats::latest_for_consumer()));
  }

  /**
   * @brief The host's CPU and GPU models and memory sizes, sampled once.
   *
   * @api_examples{/api/host/info| GET| null}
   */
  void getHostInfo(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    send_response(response, host_info_to_json(host_stats::info()));
  }

  /**
   * @brief Running video sessions with their live counters.
   *
   * @api_examples{/api/rtsp/sessions| GET| null}
   */
  void listRTSPSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json output;
    output["sessions"] = nlohmann::json::array();
    for (const auto &info : stream::get_all_session_info()) {
      output["sessions"].push_back(rtsp_session_to_json(info));
    }
    send_response(response, output);
  }

  /**
   * @brief Recorded sessions, newest first. Query: `limit` (1-100, default 25), `offset`.
   *
   * @api_examples{/api/history/sessions| GET| null}
   */
  void listSessionHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    int limit = 25;
    int offset = 0;
    const auto query = request->parse_query_string();
    if (const auto it = query.find("limit"); it != query.end()) {
      try {
        limit = std::stoi(it->second);
      } catch (...) {}
    }
    if (const auto it = query.find("offset"); it != query.end()) {
      try {
        offset = std::stoi(it->second);
      } catch (...) {}
    }
    limit = std::clamp(limit, 1, 100);
    offset = std::max(offset, 0);

    nlohmann::json output;
    output["sessions"] = nlohmann::json::array();
    for (const auto &summary : session_history::list_sessions(limit, offset)) {
      output["sessions"].push_back(session_summary_to_json(summary));
    }
    output["history_status"] = history_status_to_json(session_history::get_history_status());
    send_response(response, output);
  }

  /**
   * @brief One recorded session with its samples and events. Query: `full=1` for every sample.
   *
   * @api_examples{/api/history/sessions/{uuid}| GET| null}
   */
  void getSessionHistoryDetail(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    const auto uuid = request->path_match[1].str();
    const auto query = request->parse_query_string();
    const auto full = query.find("full");
    const bool include_all = full != query.end() && (full->second == "1" || full->second == "true");

    const auto detail = session_history::get_session_detail(uuid, include_all);
    if (!detail) {
      not_found(response, request);
      return;
    }

    nlohmann::json output = session_summary_to_json(detail->summary);
    output["total_samples"] = detail->total_samples;
    output["total_events"] = detail->total_events;
    output["samples_truncated"] = detail->samples_truncated;
    output["events_truncated"] = detail->events_truncated;
    output["samples"] = nlohmann::json::array();
    for (const auto &sample : detail->samples) {
      output["samples"].push_back(session_sample_to_json(sample));
    }
    output["events"] = nlohmann::json::array();
    for (const auto &event : detail->events) {
      output["events"].push_back(session_event_to_json(event));
    }
    output["history_status"] = history_status_to_json(session_history::get_history_status());
    send_response(response, output);
  }

  /**
   * @brief Delete one ended session from the history.
   *
   * @api_examples{/api/history/sessions/{uuid}| DELETE| null}
   */
  void deleteSessionHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    const auto uuid = request->path_match[1].str();
    switch (session_history::delete_session(uuid)) {
      case session_history::delete_result_e::deleted:
        break;
      case session_history::delete_result_e::not_found:
        not_found(response, request);
        return;
      case session_history::delete_result_e::active_session:
        send_error(response, SimpleWeb::StatusCode::client_error_conflict, "Cannot delete a session that is still streaming");
        return;
      case session_history::delete_result_e::timeout:
        send_error(response, SimpleWeb::StatusCode::server_error_gateway_timeout, "Timed out waiting for the session history");
        return;
      case session_history::delete_result_e::unavailable:
      case session_history::delete_result_e::failed:
        send_error(response, SimpleWeb::StatusCode::server_error_service_unavailable, "Session history unavailable");
        return;
    }

    nlohmann::json output;
    output["status"] = true;
    output["uuid"] = uuid;
    send_response(response, output);
  }

  /**
   * @brief Sessions being recorded right now, with their latest computed metrics.
   *
   * @api_examples{/api/history/sessions/active| GET| null}
   */
  void getActiveSessionHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json output;
    output["sessions"] = nlohmann::json::array();
    for (const auto &session : session_history::get_active_sessions()) {
      output["sessions"].push_back(active_session_to_json(session));
    }
    send_response(response, output);
  }

  /**
   * @brief Serve the v2 web UI (`/v2/...`), a single-page app built into `WEB_DIR/v2`.
   * @details Its files are public, like the current UI's assets: every API call it makes still
   *          authenticates. A path that is not a file is a route of the app, answered with its
   *          `index.html`. Hashed bundles are cached for a year, `index.html` never.
   */
  void getWebV2(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    const fs::path root = fs::weakly_canonical(fs::path {WEB_DIR} / "v2");
    std::string relative = request->path.size() > 3 ? request->path.substr(3) : std::string {};  // drop "/v2"
    while (!relative.empty() && relative.front() == '/') {
      relative.erase(relative.begin());
    }

    auto file = fs::weakly_canonical(root / fs::path {relative});
    const bool inside = file == root || isChildPath(file, root);
    if (!inside) {
      BOOST_LOG(warning) << "Web UI v2: refusing "sv << request->path << ", outside the v2 folder"sv;
      bad_request(response, request);
      return;
    }

    const bool is_file = !relative.empty() && fs::is_regular_file(file);
    if (!is_file) {
      // An asset that is missing is a 404; anything else is a route of the app
      if (relative.starts_with("assets/") || relative.starts_with("images/")) {
        not_found(response, request);
        return;
      }
      file = root / "index.html";
      if (!fs::exists(file)) {
        not_found(response, request);
        return;
      }
    }

    const auto extension = file.extension().string();
    const auto mime = mime_types.find(extension.empty() ? std::string {} : extension.substr(1));
    if (mime == mime_types.end()) {
      bad_request(response, request);
      return;
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mime->second == "text/html" ? "text/html; charset=utf-8" : mime->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    headers.emplace("Cache-Control", relative.starts_with("assets/") && extension != ".json" ? "public, max-age=31536000, immutable" : "no-cache");
    std::ifstream in(file.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Whether credentials exist and whether this browser is signed in.
   * @details Refreshes the login here when only the session token has run out, so that loading
   *          the v2 UI does not ask for the password every two hours.
   *
   * @api_examples{/api/auth/status| GET| null}
   */
  void getAuthStatus(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    const bool configured = !config::sunshine.username.empty();
    bool authenticated = configured && web_sessions().validate(request_cookie(request, web_auth::session_cookie_name)).has_value();

    SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    if (configured && !authenticated) {
      if (auto tokens = refresh_from_cookie(request)) {
        add_session_cookies(headers, *tokens);
        authenticated = true;
      }
    }

    nlohmann::json output_tree;
    output_tree["authenticated"] = authenticated;
    output_tree["credentials_configured"] = configured;
    output_tree["login_required"] = configured && !authenticated;
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief What the host is and what it can do, for the v2 UI. Never probes anything.
   *
   * @api_examples{/api/metadata| GET| null}
   */
  void getMetadata(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = SUNSHINE_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
    output_tree["commit"] = PROJECT_VERSION_COMMIT;
    output_tree["prerelease"] = "";
    output_tree["branch"] = "unknown";
    output_tree["release_date"] = "";
    output_tree["providers"] = nlohmann::json::object();

    const bool probed = video::encoder_probe_succeeded();
    output_tree["encoder_status"] = {
      {"state", probed ? "ready" : "unknown"},
      {"h264", probed},
      {"hevc", probed && video::active_hevc_mode >= 2},
      {"av1", probed && video::active_av1_mode >= 2},
    };

#ifdef _WIN32
    const auto driver_status = proc::vDisplayDriverStatus;
    const auto status_name = [&]() -> const char * {
      switch (driver_status) {
        case VDISPLAY::DRIVER_STATUS::OK:
          return "ready";
        case VDISPLAY::DRIVER_STATUS::FAILED:
          return "failed";
        case VDISPLAY::DRIVER_STATUS::VERSION_INCOMPATIBLE:
          return "version_incompatible";
        case VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED:
          return "watchdog_failed";
        default:
          return "unknown";
      }
    }();
    output_tree["virtual_display_driver"] = {
      {"configured", "sudovda"},
      {"active", driver_status == VDISPLAY::DRIVER_STATUS::OK ? nlohmann::json("sudovda") : nlohmann::json(nullptr)},
      {"status", status_name},
      {"status_code", static_cast<int>(driver_status)},
    };

    output_tree["has_nvidia_gpu"] = platf::has_nvidia_gpu();
    output_tree["has_amd_gpu"] = platf::has_amd_gpu();
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Change some configuration keys, keeping the others. `null` or `""` removes a key.
   * @details LavApollo reads its configuration at startup, so a change applies after a restart.
   *
   * @api_examples{/api/config| PATCH| {"key":"value"}}
   */
  void patchConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      const nlohmann::json patch_tree = nlohmann::json::parse(ss);
      if (!patch_tree.is_object()) {
        bad_request(response, request, "PATCH body must be a JSON object");
        return;
      }

      auto current = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (const auto &[key, value] : patch_tree.items()) {
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) {
          current.erase(key);
        } else {
          // Strings are written raw, anything else as JSON, like saveConfig()
          current[key] = value.is_string() ? value.get<std::string>() : value.dump();
        }
      }

      // Sorted, so that saving one key does not reshuffle the file
      std::map<std::string, std::string> ordered {current.begin(), current.end()};
      std::stringstream config_stream;
      for (const auto &[key, value] : ordered) {
        config_stream << key << " = " << value << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());

      nlohmann::json output_tree;
      output_tree["status"] = true;
      output_tree["appliedNow"] = false;
      output_tree["deferred"] = false;
      output_tree["restartRequired"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "PatchConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief The display devices Windows knows, active or not.
   *
   * @api_examples{/api/display-devices| GET| null}
   */
  void getDisplayDevices(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    try {
      const auto devices = display_device::enumerate_devices();
      SimpleWeb::CaseInsensitiveMultimap headers {
        {"Content-Type", "application/json"},
        {"X-Frame-Options", "DENY"},
        {"Content-Security-Policy", "frame-ancestors 'none';"}
      };
      response->write(display_device::toJson(devices), headers);
    } catch (const std::exception &e) {
      bad_request(response, request, std::string {"Failed to enumerate display devices: "} + e.what());
    }
  }

  /**
   * @brief Whether the ViGEmBus gamepad driver is installed.
   *
   * @api_examples{/api/health/vigem| GET| {"installed":true}}
   */
  void getVigemHealth(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json output_tree;
#ifdef _WIN32
    bool installed = false;
    if (auto scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
      if (auto service = OpenServiceW(scm, L"ViGEmBus", SERVICE_QUERY_STATUS)) {
        installed = true;
        CloseServiceHandle(service);
      }
      CloseServiceHandle(scm);
    }
    output_tree["installed"] = installed;
    // Not needed when the user picked the VHF gamepad driver (docs/vhf-gamepad.md)
    output_tree["required"] = !config::input.gamepad.starts_with("vhf");
#else
    output_tree["installed"] = false;
    output_tree["required"] = false;
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Delete an application by UUID (the v2 UI's form of `POST /api/apps/delete`).
   *
   * @api_examples{/api/apps/{uuid}| DELETE| null}
   */
  void deleteAppByUuid(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      const auto uuid = request->path_match[1].str();
      nlohmann::json file_tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      bool removed = false;
      if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
        auto &apps = file_tree["apps"];
        const auto end = std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json &app) {
          return app.value("uuid", "") == uuid;
        });
        removed = end != apps.end();
        apps.erase(end, apps.end());
      }
      if (!removed) {
        not_found(response, request);
        return;
      }

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      proc::refresh(config::stream.file_apps);

      nlohmann::json output_tree;
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief An application's cover image, by UUID; the default box art when it has none.
   *
   * @api_examples{/api/apps/{uuid}/cover| GET| null}
   */
  void getAppCover(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    try {
      const auto uuid = request->path_match[1].str();
      std::string image_path;
      const nlohmann::json file_tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      for (const auto &app : file_tree.value("apps", nlohmann::json::array())) {
        if (app.value("uuid", "") == uuid) {
          image_path = app.value("image-path", "");
          break;
        }
      }

      const auto file = proc::validate_app_image_path(image_path);
      const auto extension = fs::path {file}.extension().string();
      const auto mime = mime_types.find(extension.empty() ? std::string {} : extension.substr(1));

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", mime != mime_types.end() ? mime->second : "image/png");
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
      headers.emplace("Cache-Control", "no-cache");
      std::ifstream in(file, std::ios::binary);
      response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Trade the refresh cookie for a new pair of login cookies.
   * @details What the web UI calls when an API request returns 401, before retrying it once.
   *
   * @api_examples{/api/auth/refresh| POST| null}
   */
  void refreshSession(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers {
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    auto tokens = refresh_from_cookie(request);
    if (!tokens) {
      add_cleared_cookies(headers);
      response->write(SimpleWeb::StatusCode::client_error_unauthorized, headers);
      return;
    }
    add_session_cookies(headers, *tokens);
    response->write(headers);
  }

  /**
   * @brief End the requesting login and clear its cookies. Succeeds when there is nothing to end.
   *
   * @api_examples{/api/auth/logout| POST| null}
   */
  void logout(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }
    web_sessions().revoke_tokens(
      request_cookie(request, web_auth::session_cookie_name),
      request_cookie(request, web_auth::refresh_cookie_name)
    );
    SimpleWeb::CaseInsensitiveMultimap headers {
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    add_cleared_cookies(headers);
    response->write(headers);
  }

  /**
   * @brief List the web UI logins that are still valid.
   *
   * @api_examples{/api/auth/sessions| GET| null}
   */
  void getSessions(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    const auto current = web_sessions().validate(request_cookie(request, web_auth::session_cookie_name));
    const auto to_unix = [](web_auth::clock_t::time_point tp) {
      return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    };

    nlohmann::json sessions = nlohmann::json::array();
    for (const auto &session : web_sessions().list(current.value_or(""))) {
      sessions.push_back({
        {"id", session.id},
        {"username", session.username},
        {"device", session.device},
        {"address", session.address},
        {"created_at", to_unix(session.created_at)},
        {"last_seen", to_unix(session.last_seen)},
        {"expires_at", to_unix(session.expires_at)},
        {"remember_me", session.remember_me},
        {"current", session.current},
        // The names the v2 UI reads
        {"device_label", session.device},
        {"remote_address", session.address},
        {"refresh_expires_at", to_unix(session.expires_at)},
      });
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["sessions"] = std::move(sessions);
    send_response(response, output_tree);
  }

  /**
   * @brief End one web UI login by id. Ending the requesting one also clears its cookies.
   *
   * @api_examples{/api/auth/sessions/{id}| DELETE| null}
   */
  void revokeSession(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    const std::string id = request->path_match[1];
    const auto current = web_sessions().validate(request_cookie(request, web_auth::session_cookie_name));
    if (!web_sessions().revoke(id)) {
      not_found(response, request);
      return;
    }
    BOOST_LOG(info) << "Web UI: ["sv << net::addr_to_normalized_string(request->remote_endpoint().address())
                    << "] -- ended a web UI login"sv;

    SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    if (current == id) {
      add_cleared_cookies(headers);
    }
    response->write(nlohmann::json {{"status", true}}.dump(), headers);
  }

  /**
   * @brief Issue a CSRF token for this client, for pages not served from the address they call.
   * @details Readable only by same-origin pages (no CORS headers), and bound to the client's address.
   *
   * @api_examples{/api/csrf-token| GET| null}
   */
  void getCSRFToken(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    const auto now = std::chrono::steady_clock::now();

    auto &csrf = csrf_tokens();
    std::string token;
    {
      std::scoped_lock lock {csrf.mutex};
      std::erase_if(csrf.tokens, [&](const auto &entry) {
        return entry.second.second <= now;
      });
      auto &slot = csrf.tokens[address];
      if (slot.first.empty()) {
        slot = {crypto::rand_alphabet(32, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv), now + CSRF_TOKEN_LIFETIME};
      }
      token = slot.first;
    }
    nlohmann::json output_tree;
    output_tree["csrf_token"] = token;
    send_response(response, output_tree);
  }

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["GET"] = not_found;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/login/?$"]["GET"] = getLoginPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/auth/refresh$"]["POST"] = refreshSession;
    server.resource["^/api/auth/logout$"]["POST"] = logout;
    server.resource["^/api/auth/sessions$"]["GET"] = getSessions;
    server.resource["^/api/auth/sessions/([A-Za-z0-9]+)$"]["DELETE"] = revokeSession;
    server.resource["^/api/csrf-token$"]["GET"] = getCSRFToken;
    server.resource["^/v2(/.*)?$"]["GET"] = getWebV2;
    server.resource["^/api/auth/status$"]["GET"] = getAuthStatus;
    server.resource["^/api/auth/login$"]["POST"] = login;
    server.resource["^/api/metadata$"]["GET"] = getMetadata;
    server.resource["^/api/config$"]["PATCH"] = patchConfig;
    server.resource["^/api/display-devices$"]["GET"] = getDisplayDevices;
    server.resource["^/api/health/vigem$"]["GET"] = getVigemHealth;
    server.resource["^/api/apps/([A-Fa-f0-9-]+)$"]["DELETE"] = deleteAppByUuid;
    server.resource["^/api/apps/([A-Fa-f0-9-]+)/cover$"]["GET"] = getAppCover;
    server.resource["^/api/session/status$"]["GET"] = getSessionStatus;
    server.resource["^/api/host/stats$"]["GET"] = getHostStats;
    server.resource["^/api/host/info$"]["GET"] = getHostInfo;
    server.resource["^/api/rtsp/sessions$"]["GET"] = listRTSPSessions;
    server.resource["^/api/history/sessions$"]["GET"] = listSessionHistory;
    server.resource["^/api/history/sessions/active$"]["GET"] = getActiveSessionHistory;
    server.resource["^/api/history/sessions/([A-Fa-f0-9-]+)$"]["GET"] = getSessionHistoryDetail;
    server.resource["^/api/history/sessions/([A-Fa-f0-9-]+)$"]["DELETE"] = deleteSessionHistory;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/otp$"]["POST"] = getOTP;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/reorder$"]["POST"] = reorderApps;
    server.resource["^/api/apps/delete$"]["POST"] = deleteApp;
    server.resource["^/api/apps/launch$"]["POST"] = launchApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/logs/export$"]["GET"] = exportLogs;
#ifdef _WIN32
    server.resource["^/api/frame-limiter/status$"]["GET"] = getFrameLimiterStatus;
#endif
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/quit$"]["POST"] = quit;
    server.resource["^/api/update/check$"]["GET"] = updateCheckGet;
    server.resource["^/api/update/download$"]["POST"] = updateDownloadPost;
    server.resource["^/api/update/status$"]["GET"] = updateStatusGet;
    server.resource["^/api/update/install$"]["POST"] = updateInstallPost;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/disconnect$"]["POST"] = disconnect;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/images/apollo.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-apollo-45.png$"]["GET"] = getApolloLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;

    // Every state-changing route checks where the request comes from first, whatever its own
    // authentication: a route added later is covered without thinking about it.
    for (auto &[pattern, methods] : server.resource) {
      for (auto &[method, handler] : methods) {
        if (method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE") {
          handler = [inner = handler](resp_https_t response, req_https_t request) {
            if (check_csrf(response, request)) {
              inner(std::move(response), std::move(request));
            }
          };
        }
      }
    }

    web_sessions().load();

    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([port_https](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what() << net::bind_error_explanation(err.code());
        // This IS the Web UI: if it cannot bind there is no banner channel left, so shut down cleanly.
        shutdown_event->raise(true);
        return;
      } catch (std::exception &err) {
        // Safety net so no exception can escape a std::thread and trigger std::terminate/abort.
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
