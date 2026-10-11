/**
 * @file src/web_auth.h
 * @brief Web UI login sessions: persistent, revocable, with rotating refresh tokens (after Vibepollo).
 *
 * A login issues two random tokens, each sent as an HttpOnly `__Host-` cookie:
 * - a short **session token** (default 2 h) that every request presents;
 * - a **refresh token** (24 h, or 7 days with "remember me") that buys a new pair once the session
 *   token has expired. Each refresh replaces both tokens; the refresh token's absolute lifetime does
 *   not slide, so a remembered login still ends after its refresh lifetime.
 *
 * Only SHA-256 hashes of the tokens are kept, in their own file (`lavapollo_auth.json` next to the
 * state file), never in `sunshine_state.json`: `nvhttp::save_state()` rebuilds that one from scratch.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace web_auth {
  using clock_t = std::chrono::system_clock;

  inline constexpr std::string_view session_cookie_name {"__Host-apollo_session"};
  inline constexpr std::string_view refresh_cookie_name {"__Host-apollo_refresh"};

  /// Refresh lifetime of a login without "remember me".
  inline constexpr std::chrono::seconds default_refresh_ttl {std::chrono::hours(24)};

  /**
   * @brief A freshly issued token pair, to be sent as cookies.
   */
  struct tokens_t {
    std::string session_token;
    std::string refresh_token;
    std::chrono::seconds session_ttl;
    std::chrono::seconds refresh_ttl;
    bool remember_me;
  };

  /**
   * @brief A login as shown to the user. Never carries a token or a token hash.
   */
  struct session_view_t {
    std::string id;  ///< Stable across refreshes; what revocation takes
    std::string username;
    std::string device;  ///< "Firefox on Windows", derived from the user agent
    std::string address;
    clock_t::time_point created_at;
    clock_t::time_point last_seen;
    clock_t::time_point expires_at;  ///< When the refresh token runs out
    bool remember_me;
    bool current;  ///< The session making the request
  };

  /**
   * @brief What the store needs from the outside world, injectable for tests.
   */
  struct dependencies_t {
    std::function<clock_t::time_point()> now;
    std::function<std::string(std::size_t)> random;
    std::function<std::string(const std::string &)> hash;
  };

  /**
   * @brief The production clock, `crypto::rand_alphabet` and SHA-256.
   */
  dependencies_t default_dependencies();

  /**
   * @brief Every web UI login, persisted to one file. Thread-safe.
   */
  class session_store_t {
  public:
    session_store_t(std::filesystem::path file, dependencies_t dependencies);

    /**
     * @brief Reads the file, dropping logins that have expired. A missing or unreadable file means no logins.
     */
    void load();

    /**
     * @brief Records a new login.
     * @param refresh_ttl Lifetime of the refresh token; never shorter than `session_ttl`.
     */
    tokens_t issue(const std::string &username, bool remember_me, const std::string &user_agent, const std::string &address, std::chrono::seconds session_ttl, std::chrono::seconds refresh_ttl);

    /**
     * @brief Checks a session token.
     * @return The login's id, or nothing when the token is unknown or its session part has expired.
     */
    std::optional<std::string> validate(const std::string &session_token);

    /**
     * @brief Trades a refresh token for a new pair. The presented refresh token stops working.
     * @return Nothing when the refresh token is unknown, already used, or expired.
     */
    std::optional<tokens_t> refresh(const std::string &refresh_token, const std::string &user_agent, const std::string &address, std::chrono::seconds session_ttl);

    /**
     * @brief Ends the login holding either token. Unknown tokens are ignored.
     */
    void revoke_tokens(const std::string &session_token, const std::string &refresh_token);

    /**
     * @brief Ends one login by id.
     * @return Whether it existed.
     */
    bool revoke(const std::string &id);

    /**
     * @brief Ends every login, as a credentials change must.
     */
    void revoke_all();

    /**
     * @brief Lists the logins that have not expired, most recently seen first.
     * @param current_id The requesting session's id, flagged in the result.
     */
    std::vector<session_view_t> list(const std::string &current_id = {}) const;

  private:
    struct entry_t {
      std::string id;
      std::string username;
      std::string session_hash;
      std::string refresh_hash;
      clock_t::time_point created_at;
      clock_t::time_point session_expires_at;
      clock_t::time_point refresh_expires_at;
      clock_t::time_point last_seen;
      bool remember_me;
      std::string user_agent;
      std::string address;
    };

    tokens_t rotate_locked(entry_t &entry, clock_t::time_point now, std::chrono::seconds session_ttl);
    void drop_expired_locked(clock_t::time_point now);
    void save_locked() const;

    std::filesystem::path _file;
    dependencies_t _deps;
    mutable std::mutex _mutex;
    std::vector<entry_t> _entries;
  };

  /**
   * @brief Reads one cookie from a `Cookie` header.
   * @return Its value, or an empty string.
   */
  std::string cookie_value(std::string_view cookie_header, std::string_view name);

  /**
   * @brief A `Set-Cookie` value for one of the auth cookies.
   * @param max_age Lifetime, or nothing for a browser-session cookie.
   */
  std::string set_cookie(std::string_view name, std::string_view value, std::optional<std::chrono::seconds> max_age);

  /**
   * @brief A `Set-Cookie` value that deletes a cookie.
   */
  std::string clear_cookie(std::string_view name);

  /**
   * @brief Whether a state-changing request's `Origin` (or `Referer`) is one the web UI is served from.
   * @details The request's own `Host` is always allowed, so the UI keeps working whatever address it
   *          was opened by (IP on the LAN, host name, localhost). `extra_origins` are `https://` origins
   *          from `csrf_allowed_origins`.
   * @param origin The `Origin` header, or the `Referer` header when there is no `Origin`.
   * @param host The request's `Host` header.
   */
  bool origin_allowed(std::string_view origin, std::string_view host, const std::vector<std::string> &extra_origins);

  /**
   * @brief A short label for a login's device, from its user agent.
   */
  std::string device_label(std::string_view user_agent, std::string_view address);
}  // namespace web_auth
