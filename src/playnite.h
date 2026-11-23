/**
 * @file src/playnite.h
 * @brief Playnite games as LavApollo apps, through the "LavApollo Link" Playnite extension.
 *
 * The extension (tools/playnite/) writes library.json, state.json and PNG covers into
 * %ProgramData%\LavApollo\playnite\; LavApollo writes stop requests into its commands\ folder.
 * Each installed game becomes a virtual app (never written to apps.json), launched through
 * Playnite's playnite://playnite/start/<id> URI. Design:
 * docs/superpowers/specs/2026-11-23-playnite-integration-design.md.
 */
#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace playnite {
  struct game_t {
    std::string id;  ///< Playnite game id (GUID), upper case: it becomes the app's uuid
    std::string name;
    std::filesystem::path cover;  ///< absolute PNG path, empty when the game has none
    std::string last_activity;  ///< Playnite's last play time, ISO 8601 UTC ("o"), empty if never played
  };

  struct running_game_t {
    std::string id;  ///< upper case
    std::string name;
    bool starting = false;  ///< Playnite is still starting it (OnGameStarting)
  };

  struct state_t {
    bool playnite_running = false;
    std::string mode;  ///< "desktop" or "fullscreen"
    std::optional<running_game_t> game;
  };

  /// %ProgramData%\LavApollo\playnite
  std::filesystem::path exchange_dir();

  /// Parses library.json; covers resolve against `covers_dir`. Malformed input gives no games.
  std::vector<game_t> parse_library(const std::string &json, const std::filesystem::path &covers_dir);

  /// Parses state.json. Malformed input gives nothing.
  std::optional<state_t> parse_state(const std::string &json);

  /// Most recently played first, as Playnite's "Recent activity" sort; never played games last, by name.
  void sort_recent_first(std::vector<game_t> &games);

  /// The installed games, from the exchange folder, most recently played first (the clients show
  /// them in this order). Empty when the extension never ran.
  std::vector<game_t> load_library();

  /// Whether library.json changed since the last call (checked at most every 5 s).
  bool library_changed();

  /// The current state.json, read at most once a second. Nothing when absent or unreadable.
  std::optional<state_t> current_state();

  /// Command line that starts a game in Playnite (desktop or fullscreen, starting Playnite if needed).
  std::string launch_command(const std::string &game_id);

  /// Asks the extension to close `game_id`. No-op when the extension's commands folder is missing.
  void request_stop(const std::string &game_id);

  /// Whether an app is the user's Playnite front end (its command starts Playnite).
  bool is_playnite_command(const std::string &cmd);

  /**
   * @brief Follows one Playnite game launched by LavApollo, to end the session with the game.
   *
   * The game is "seen" once state.json reports it (starting or running). After that, a state
   * without it means the game was closed. A game never seen within `launch_grace` failed to start.
   */
  class watch_t {
  public:
    enum class verdict_e {
      running,  ///< keep the session
      ended,  ///< the game was seen, then closed
      never_started,  ///< still not seen after the grace period
    };

    watch_t() = default;
    watch_t(std::string game_id, std::chrono::steady_clock::time_point launched_at, std::chrono::seconds launch_grace = std::chrono::seconds {120});

    verdict_e update(const std::optional<state_t> &state, std::chrono::steady_clock::time_point now);

    bool active() const {
      return !_game_id.empty();
    }

    const std::string &game_id() const {
      return _game_id;
    }

    bool seen() const {
      return _seen;
    }

  private:
    std::string _game_id;
    std::chrono::steady_clock::time_point _launched_at;
    std::chrono::seconds _launch_grace {120};
    bool _seen = false;
  };
}  // namespace playnite
