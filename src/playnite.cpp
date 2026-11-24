/**
 * @file src/playnite.cpp
 * @brief Playnite games as LavApollo apps (see playnite.h).
 */
#include "playnite.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include <nlohmann/json.hpp>

#include "logging.h"
#include "test_host.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace playnite {
  namespace {
    std::string upper(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::toupper(c);
      });
      return s;
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    std::optional<std::string> read_text(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      return ss.str();
    }

    std::string string_field(const nlohmann::json &node, const char *key) {
      auto it = node.find(key);
      if (it == node.end() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    // A GUID, the only thing a game id may be: it ends up in a command line.
    bool is_guid(const std::string &s) {
      if (s.size() != 36) {
        return false;
      }
      for (size_t i = 0; i < s.size(); i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !std::isxdigit((unsigned char) s[i])) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  fs::path exchange_dir() {
    const char *program_data = std::getenv("ProgramData");
    fs::path root = program_data && *program_data ? fs::path(program_data) : fs::path("C:\\ProgramData");
    return root / "LavApollo" / "playnite";
  }

  std::vector<game_t> parse_library(const std::string &json, const fs::path &covers_dir) {
    std::vector<game_t> games;
    try {
      auto tree = nlohmann::json::parse(json);
      auto list = tree.find("games");
      if (list == tree.end() || !list->is_array()) {
        return games;
      }
      for (auto &node : *list) {
        if (!node.is_object()) {
          continue;
        }
        game_t game;
        game.id = upper(string_field(node, "id"));
        game.name = string_field(node, "name");
        if (!is_guid(game.id) || game.name.empty()) {
          continue;
        }
        game.last_activity = string_field(node, "lastActivity");
        auto cover = string_field(node, "cover");
        // A bare file name inside covers\: never let the file point elsewhere.
        if (!cover.empty() && fs::path(cover).filename().string() == cover) {
          game.cover = covers_dir / cover;
        }
        games.emplace_back(std::move(game));
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "Playnite: unreadable library.json: "sv << e.what();
      games.clear();
    }
    return games;
  }

  std::optional<state_t> parse_state(const std::string &json) {
    try {
      auto tree = nlohmann::json::parse(json);
      if (!tree.is_object()) {
        return std::nullopt;
      }
      state_t state;
      state.playnite_running = tree.value("playniteRunning", false);
      state.mode = string_field(tree, "mode");
      auto game = tree.find("game");
      if (game != tree.end() && game->is_object()) {
        running_game_t running;
        running.id = upper(string_field(*game, "id"));
        running.name = string_field(*game, "name");
        running.starting = game->value("starting", false);
        if (is_guid(running.id)) {
          state.game = std::move(running);
        }
      }
      // A closed Playnite runs no game, whatever an interrupted write left behind.
      if (!state.playnite_running) {
        state.game.reset();
      }
      return state;
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  std::vector<game_t> load_library() {
    auto dir = exchange_dir();
    auto text = read_text(dir / "library.json");
    if (!text) {
      return {};
    }
    auto games = parse_library(*text, dir / "covers");
    sort_recent_first(games);
    return games;
  }

  void sort_recent_first(std::vector<game_t> &games) {
    // The extension writes every date with DateTime.ToString("o") in UTC: same length, same
    // zone, so the strings sort as the dates do.
    std::sort(games.begin(), games.end(), [](const game_t &a, const game_t &b) {
      if (a.last_activity != b.last_activity) {
        if (a.last_activity.empty() || b.last_activity.empty()) {
          return b.last_activity.empty();
        }
        return a.last_activity > b.last_activity;
      }
      return lower(a.name) < lower(b.name);
    });
  }

  bool library_changed() {
    static std::mutex lock;
    static std::chrono::steady_clock::time_point last_check;
    static fs::file_time_type last_write {};
    static bool initialized = false;

    std::lock_guard lg(lock);
    auto now = std::chrono::steady_clock::now();
    if (initialized && now - last_check < 5s) {
      return false;
    }
    last_check = now;

    std::error_code ec;
    auto write = fs::last_write_time(exchange_dir() / "library.json", ec);
    if (ec) {
      write = {};
    }
    if (!initialized) {
      // The app list was just built from the current file.
      initialized = true;
      last_write = write;
      return false;
    }
    if (write != last_write) {
      last_write = write;
      return true;
    }
    return false;
  }

  std::optional<state_t> current_state() {
    static std::mutex lock;
    static std::chrono::steady_clock::time_point last_read;
    static std::optional<state_t> cached;
    static bool has_read = false;

    std::lock_guard lg(lock);
    auto now = std::chrono::steady_clock::now();
    if (has_read && now - last_read < 1s) {
      return cached;
    }
    has_read = true;
    last_read = now;

    auto text = read_text(exchange_dir() / "state.json");
    cached = text ? parse_state(*text) : std::nullopt;
    return cached;
  }

  std::string launch_command(const std::string &game_id) {
    // Playnite registers playnite:// for its desktop app, which forwards the request to the
    // running instance, fullscreen included, or starts Playnite first.
    return "cmd /c start \"\" \"playnite://playnite/start/" + lower(game_id) + "\"";
  }

  namespace {
    // Test host mode: the fake PC's own Playnite state (the exchange folder is redirected with %ProgramData%).
    void write_test_state(const nlohmann::json &game) {
      nlohmann::json state;
      state["playniteRunning"] = true;
      state["mode"] = "fullscreen";
      if (!game.is_null()) {
        state["game"] = game;
      }
      auto dir = exchange_dir();
      std::error_code ec;
      fs::create_directories(dir, ec);
      auto tmp = dir / "state.json.tmp";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << state.dump();
      }
      fs::rename(tmp, dir / "state.json", ec);
      if (ec) {
        BOOST_LOG(warning) << "Playnite: test host cannot write state.json: "sv << ec.message();
      }
    }
  }  // namespace

  bool test_host_launch(const std::string &cmd) {
    static const std::string marker = "playnite://playnite/start/";
    if (!test_host::enabled()) {
      return false;
    }
    auto at = lower(cmd).find(marker);
    if (at == std::string::npos) {
      return false;
    }
    auto begin = at + marker.size();
    auto end = cmd.find_first_of("\" ", begin);
    auto id = upper(cmd.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (!is_guid(id)) {
      return false;
    }

    std::string name;
    for (auto &game : load_library()) {
      if (game.id == id) {
        name = game.name;
        break;
      }
    }
    test_host::log("events", {{"playnite_launch", lower(id)}});
    write_test_state({{"id", lower(id)}, {"name", name}, {"starting", false}});
    return true;
  }

  void request_stop(const std::string &game_id) {
    static std::atomic<unsigned> counter {0};

    if (test_host::enabled()) {
      test_host::log("events", {{"playnite_stop", lower(game_id)}});
      write_test_state(nullptr);
      return;
    }

    // The extension creates this folder, and owns it: that is what lets it delete our files.
    auto dir = exchange_dir() / "commands";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      BOOST_LOG(info) << "Playnite: no extension commands folder, cannot close the game"sv;
      return;
    }

    auto now = std::chrono::system_clock::now();
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    nlohmann::json command;
    command["action"] = "stop-game";
    command["gameId"] = lower(game_id);
    {
      auto t = std::chrono::system_clock::to_time_t(now);
      std::tm tm {};
#ifdef _WIN32
      gmtime_s(&tm, &t);
#else
      gmtime_r(&t, &tm);
#endif
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
      command["issuedAt"] = buf;
    }

    auto name = "stop-" + std::to_string(stamp) + "-" + std::to_string(counter++) + ".json";
    auto tmp = dir / (name + ".tmp");
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << command.dump();
    }
    fs::rename(tmp, dir / name, ec);
    if (ec) {
      BOOST_LOG(warning) << "Playnite: cannot write stop request: "sv << ec.message();
      fs::remove(tmp, ec);
      return;
    }
    BOOST_LOG(info) << "Playnite: asked the extension to close "sv << game_id;
  }

  bool is_playnite_command(const std::string &cmd) {
    auto l = lower(cmd);
    return l.find("playnite.fullscreenapp.exe") != std::string::npos || l.find("playnite.desktopapp.exe") != std::string::npos;
  }

  watch_t::watch_t(std::string game_id, std::chrono::steady_clock::time_point launched_at, std::chrono::seconds launch_grace):
      _game_id {upper(std::move(game_id))},
      _launched_at {launched_at},
      _launch_grace {launch_grace} {
  }

  watch_t::verdict_e watch_t::update(const std::optional<state_t> &state, std::chrono::steady_clock::time_point now) {
    if (_game_id.empty()) {
      return verdict_e::running;
    }

    bool present = state && state->game && state->game->id == _game_id;
    if (present) {
      _seen = true;
      return verdict_e::running;
    }

    if (_seen) {
      // An unreadable state.json (half-written, locked) is not proof the game closed.
      return state ? verdict_e::ended : verdict_e::running;
    }

    return now - _launched_at >= _launch_grace ? verdict_e::never_started : verdict_e::running;
  }
}  // namespace playnite
