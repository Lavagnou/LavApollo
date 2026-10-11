/**
 * @file src/web_auth.cpp
 * @brief Web UI login sessions (see web_auth.h).
 */
// standard includes
#include <algorithm>
#include <fstream>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

// local includes
#include "crypto.h"
#include "logging.h"
#include "utility.h"
#include "web_auth.h"

using namespace std::literals;

namespace web_auth {
  namespace {
    // A login's last_seen is written at most this often, so browsing the UI does not rewrite the file.
    constexpr auto last_seen_resolution = std::chrono::minutes(5);

    constexpr std::size_t token_length = 64;
    constexpr std::size_t id_length = 16;

    std::int64_t to_unix(clock_t::time_point tp) {
      return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();
    }

    clock_t::time_point from_unix(std::int64_t seconds) {
      return clock_t::time_point {std::chrono::seconds(seconds)};
    }

    bool icontains(std::string_view haystack, std::string_view needle) {
      return boost::algorithm::icontains(haystack, needle);
    }

    // The scheme and authority of an Origin or Referer value, lower-cased.
    std::string origin_of(std::string_view url) {
      const auto scheme_end = url.find("://"sv);
      if (scheme_end == std::string_view::npos) {
        return {};
      }
      const auto authority_end = url.find_first_of("/?#"sv, scheme_end + 3);
      std::string origin {url.substr(0, authority_end)};
      std::transform(origin.begin(), origin.end(), origin.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return origin;
    }
  }  // namespace

  dependencies_t default_dependencies() {
    return {
      [] {
        return clock_t::now();
      },
      [](std::size_t length) {
        // Alphanumeric: tokens travel in cookies, ids in URL paths
        return crypto::rand_alphabet(length, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv);
      },
      [](const std::string &value) {
        return util::hex(crypto::hash(value)).to_string();
      },
    };
  }

  session_store_t::session_store_t(std::filesystem::path file, dependencies_t dependencies):
      _file {std::move(file)},
      _deps {std::move(dependencies)} {}

  void session_store_t::load() {
    std::scoped_lock lock {_mutex};
    _entries.clear();

    std::ifstream in {_file};
    if (!in) {
      return;
    }
    try {
      const auto root = nlohmann::json::parse(in);
      for (const auto &item : root.at("sessions")) {
        entry_t entry {
          item.at("id").get<std::string>(),
          item.at("username").get<std::string>(),
          item.at("session_hash").get<std::string>(),
          item.at("refresh_hash").get<std::string>(),
          from_unix(item.at("created_at").get<std::int64_t>()),
          from_unix(item.at("session_expires_at").get<std::int64_t>()),
          from_unix(item.at("refresh_expires_at").get<std::int64_t>()),
          from_unix(item.at("last_seen").get<std::int64_t>()),
          item.value("remember_me", false),
          item.value("user_agent", ""s),
          item.value("address", ""s),
        };
        _entries.push_back(std::move(entry));
      }
    } catch (const std::exception &e) {
      // Losing the logins only means logging in again
      BOOST_LOG(warning) << "Web UI sessions: ignoring unreadable "sv << _file.string() << ": "sv << e.what();
      _entries.clear();
      return;
    }

    const auto before = _entries.size();
    drop_expired_locked(_deps.now());
    if (_entries.size() != before) {
      save_locked();
    }
  }

  tokens_t session_store_t::issue(const std::string &username, bool remember_me, const std::string &user_agent, const std::string &address, std::chrono::seconds session_ttl, std::chrono::seconds refresh_ttl) {
    std::scoped_lock lock {_mutex};
    const auto now = _deps.now();
    drop_expired_locked(now);

    entry_t entry {};
    entry.id = _deps.random(id_length);
    entry.username = username;
    entry.created_at = now;
    entry.refresh_expires_at = now + std::max(refresh_ttl, session_ttl);
    entry.remember_me = remember_me;
    entry.user_agent = user_agent;
    entry.address = address;

    auto tokens = rotate_locked(entry, now, session_ttl);
    _entries.push_back(std::move(entry));
    save_locked();
    return tokens;
  }

  std::optional<std::string> session_store_t::validate(const std::string &session_token) {
    if (session_token.empty()) {
      return std::nullopt;
    }
    const auto hash = _deps.hash(session_token);

    std::scoped_lock lock {_mutex};
    const auto now = _deps.now();
    const auto it = std::find_if(_entries.begin(), _entries.end(), [&](const entry_t &entry) {
      return entry.session_hash == hash;
    });
    if (it == _entries.end() || now > it->session_expires_at || now > it->refresh_expires_at) {
      return std::nullopt;
    }
    if (now - it->last_seen >= last_seen_resolution) {
      it->last_seen = now;
      save_locked();
    }
    return it->id;
  }

  std::optional<tokens_t> session_store_t::refresh(const std::string &refresh_token, const std::string &user_agent, const std::string &address, std::chrono::seconds session_ttl) {
    if (refresh_token.empty()) {
      return std::nullopt;
    }
    const auto hash = _deps.hash(refresh_token);

    std::scoped_lock lock {_mutex};
    const auto now = _deps.now();
    const auto it = std::find_if(_entries.begin(), _entries.end(), [&](const entry_t &entry) {
      return entry.refresh_hash == hash;
    });
    if (it == _entries.end()) {
      return std::nullopt;
    }
    if (now > it->refresh_expires_at) {
      _entries.erase(it);
      save_locked();
      return std::nullopt;
    }

    it->user_agent = user_agent;
    it->address = address;
    auto tokens = rotate_locked(*it, now, session_ttl);
    save_locked();
    return tokens;
  }

  void session_store_t::revoke_tokens(const std::string &session_token, const std::string &refresh_token) {
    const auto session_hash = session_token.empty() ? std::string {} : _deps.hash(session_token);
    const auto refresh_hash = refresh_token.empty() ? std::string {} : _deps.hash(refresh_token);

    std::scoped_lock lock {_mutex};
    const auto removed = std::erase_if(_entries, [&](const entry_t &entry) {
      return (!session_hash.empty() && entry.session_hash == session_hash) ||
             (!refresh_hash.empty() && entry.refresh_hash == refresh_hash);
    });
    if (removed) {
      save_locked();
    }
  }

  bool session_store_t::revoke(const std::string &id) {
    std::scoped_lock lock {_mutex};
    const auto removed = std::erase_if(_entries, [&](const entry_t &entry) {
      return entry.id == id;
    });
    if (removed) {
      save_locked();
    }
    return removed > 0;
  }

  void session_store_t::revoke_all() {
    std::scoped_lock lock {_mutex};
    _entries.clear();
    save_locked();
  }

  std::vector<session_view_t> session_store_t::list(const std::string &current_id) const {
    std::scoped_lock lock {_mutex};
    const auto now = _deps.now();
    std::vector<session_view_t> result;
    for (const auto &entry : _entries) {
      if (now > entry.refresh_expires_at) {
        continue;
      }
      result.push_back({
        entry.id,
        entry.username,
        device_label(entry.user_agent, entry.address),
        entry.address,
        entry.created_at,
        entry.last_seen,
        entry.refresh_expires_at,
        entry.remember_me,
        !current_id.empty() && entry.id == current_id,
      });
    }
    std::sort(result.begin(), result.end(), [](const session_view_t &a, const session_view_t &b) {
      return a.last_seen > b.last_seen;
    });
    return result;
  }

  tokens_t session_store_t::rotate_locked(entry_t &entry, clock_t::time_point now, std::chrono::seconds session_ttl) {
    tokens_t tokens {
      _deps.random(token_length),
      _deps.random(token_length),
      {},
      {},
      entry.remember_me,
    };
    entry.session_hash = _deps.hash(tokens.session_token);
    entry.refresh_hash = _deps.hash(tokens.refresh_token);
    entry.session_expires_at = std::min(now + session_ttl, entry.refresh_expires_at);
    entry.last_seen = now;

    tokens.session_ttl = std::chrono::duration_cast<std::chrono::seconds>(entry.session_expires_at - now);
    tokens.refresh_ttl = std::chrono::duration_cast<std::chrono::seconds>(entry.refresh_expires_at - now);
    return tokens;
  }

  void session_store_t::drop_expired_locked(clock_t::time_point now) {
    std::erase_if(_entries, [&](const entry_t &entry) {
      return now > entry.refresh_expires_at;
    });
  }

  void session_store_t::save_locked() const {
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto &entry : _entries) {
      sessions.push_back({
        {"id", entry.id},
        {"username", entry.username},
        {"session_hash", entry.session_hash},
        {"refresh_hash", entry.refresh_hash},
        {"created_at", to_unix(entry.created_at)},
        {"session_expires_at", to_unix(entry.session_expires_at)},
        {"refresh_expires_at", to_unix(entry.refresh_expires_at)},
        {"last_seen", to_unix(entry.last_seen)},
        {"remember_me", entry.remember_me},
        {"user_agent", entry.user_agent},
        {"address", entry.address},
      });
    }
    const nlohmann::json root {{"version", 1}, {"sessions", std::move(sessions)}};

    // Write a sibling file then swap it in: a crash mid-write must not leave a truncated file.
    auto temp = _file;
    temp += ".tmp";
    {
      std::ofstream out {temp, std::ios::trunc};
      if (!out || !(out << root.dump(2))) {
        BOOST_LOG(warning) << "Web UI sessions: could not write "sv << temp.string();
        return;
      }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), _file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      BOOST_LOG(warning) << "Web UI sessions: could not replace "sv << _file.string() << " ("sv << GetLastError() << ')';
    }
#else
    std::error_code ec;
    std::filesystem::rename(temp, _file, ec);
    if (ec) {
      BOOST_LOG(warning) << "Web UI sessions: could not replace "sv << _file.string() << ": "sv << ec.message();
    }
#endif
  }

  std::string cookie_value(std::string_view cookie_header, std::string_view name) {
    while (!cookie_header.empty()) {
      const auto end = cookie_header.find(';');
      auto pair = cookie_header.substr(0, end);
      cookie_header = end == std::string_view::npos ? std::string_view {} : cookie_header.substr(end + 1);

      while (!pair.empty() && pair.front() == ' ') {
        pair.remove_prefix(1);
      }
      const auto eq = pair.find('=');
      if (eq != std::string_view::npos && pair.substr(0, eq) == name) {
        return std::string {pair.substr(eq + 1)};
      }
    }
    return {};
  }

  std::string set_cookie(std::string_view name, std::string_view value, std::optional<std::chrono::seconds> max_age) {
    // __Host- cookies must be Secure, Path=/ and carry no Domain. HttpOnly: no script reads them.
    std::string cookie;
    cookie.append(name).append("=").append(value).append("; Path=/; Secure; HttpOnly; SameSite=Strict");
    if (max_age) {
      cookie.append("; Max-Age=").append(std::to_string(max_age->count()));
    }
    return cookie;
  }

  std::string clear_cookie(std::string_view name) {
    return set_cookie(name, "", std::chrono::seconds::zero());
  }

  bool origin_allowed(std::string_view origin, std::string_view host, const std::vector<std::string> &extra_origins) {
    const auto actual = origin_of(origin);
    if (actual.empty()) {
      return false;
    }
    if (!host.empty() && boost::algorithm::iequals(actual, "https://"s.append(host))) {
      return true;
    }
    // An allowed origin without a port matches it on any port: the web UI's port is configurable.
    return std::any_of(extra_origins.begin(), extra_origins.end(), [&](const std::string &allowed) {
      const auto expected = origin_of(allowed);
      return !expected.empty() &&
             (actual == expected || (actual.starts_with(expected) && actual[expected.size()] == ':'));
    });
  }

  std::string device_label(std::string_view user_agent, std::string_view address) {
    std::string browser;
    if (icontains(user_agent, "edg/")) {
      browser = "Edge";
    } else if (icontains(user_agent, "opr/")) {
      browser = "Opera";
    } else if (icontains(user_agent, "firefox/")) {
      browser = "Firefox";
    } else if (icontains(user_agent, "chrome/")) {
      browser = "Chrome";
    } else if (icontains(user_agent, "safari/")) {
      browser = "Safari";
    }

    std::string os;
    if (icontains(user_agent, "windows")) {
      os = "Windows";
    } else if (icontains(user_agent, "iphone") || icontains(user_agent, "ipad")) {
      os = "iOS";
    } else if (icontains(user_agent, "android")) {
      os = "Android";
    } else if (icontains(user_agent, "mac os x")) {
      os = "macOS";
    } else if (icontains(user_agent, "linux")) {
      os = "Linux";
    }

    if (!browser.empty() && !os.empty()) {
      return browser + " on " + os;
    }
    if (!browser.empty() || !os.empty()) {
      return browser + os;
    }
    return address.empty() ? "Unknown device"s : std::string {address};
  }
}  // namespace web_auth
