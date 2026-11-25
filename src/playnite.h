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
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playnite {
  struct game_t {
    std::string id;  ///< Playnite game id (GUID), upper case: it becomes the app's uuid
    std::string name;
    std::filesystem::path cover;  ///< absolute PNG path, empty when the game has none
    std::string last_activity;  ///< Playnite's last play time, ISO 8601 UTC ("o"), empty if never played
    std::int64_t last_played = 0;  ///< last_activity as Unix seconds, 0 if never played (or unreadable)
    bool installed = true;  ///< absent from library.json (extension 1.0.x exported installed games only): true
    std::uint64_t playtime = 0;  ///< Playnite's Playtime, seconds
    std::filesystem::path background;  ///< absolute background image path (JPEG), empty when none
  };

  struct running_game_t {
    std::string id;  ///< upper case
    std::string name;
    bool starting = false;  ///< Playnite is still starting it (OnGameStarting)
    std::int64_t started_at = 0;  ///< Unix seconds the extension saw it start, 0 if unknown
  };

  struct state_t {
    bool playnite_running = false;
    std::string mode;  ///< "desktop" or "fullscreen"
    std::optional<running_game_t> game;
  };

  /// How long the session of a game Playnite is installing waits for the game (watch_t launch grace):
  /// the install, then the user starting it, take as long as they take.
  constexpr std::chrono::hours install_grace {24};

  /// %ProgramData%\LavApollo\playnite
  std::filesystem::path exchange_dir();

  /// Parses library.json; covers resolve against `covers_dir`, backgrounds against `backgrounds_dir`
  /// (default: a `backgrounds` folder next to `covers_dir`). Malformed input gives no games.
  std::vector<game_t> parse_library(const std::string &json, const std::filesystem::path &covers_dir, const std::filesystem::path &backgrounds_dir = {});

  /// An ISO 8601 date and time ("2026-11-20T21:00:00Z", "...:00.1234567Z", "+01:00" offsets; no zone = UTC)
  /// as Unix seconds. 0 when unreadable or before 1971.
  std::int64_t parse_iso_utc(const std::string &text);

  /// Parses state.json. Malformed input gives nothing.
  std::optional<state_t> parse_state(const std::string &json);

  /// Most recently played first, as Playnite's "Recent activity" sort; never played games last, by name.
  void sort_recent_first(std::vector<game_t> &games);

  /// The exported games (installed or not, never the hidden ones), from the exchange folder, most
  /// recently played first (the clients show them in this order). Empty when the extension never ran.
  std::vector<game_t> load_library();

  /**
   * @brief The library as library.json says right now (re-read when the file changes, checked at
   * most every 2 s), most recently played first.
   *
   * Unlike the app list, which is rebuilt only when nothing runs (proc::refresh() replaces the running
   * app), this follows every export of the extension: /applist orders the game apps and gives their
   * installed state, play time and last activity from it, also while a session runs. Never null.
   */
  std::shared_ptr<const std::vector<game_t>> library_snapshot();

  /**
   * @brief The order to list these game ids in: `library`'s (most recently played first), then the
   * ids absent from it, in their own order. Indices into `ids`.
   */
  std::vector<std::size_t> library_order(const std::vector<std::string> &ids, const std::vector<game_t> &library);

  /// The game of this id (upper case) in `library`, null when absent.
  const game_t *find_game(const std::vector<game_t> &library, const std::string &id);

  /// Whether library.json changed since the last call (checked at most every 5 s).
  bool library_changed();

  /// The current state.json, read at most once a second. Nothing when absent or unreadable.
  std::optional<state_t> current_state();

  /// Command line that starts a game in Playnite (desktop or fullscreen, starting Playnite if needed).
  std::string launch_command(const std::string &game_id);

  /// Asks the extension to close `game_id`. No-op when the extension's commands folder is missing.
  void request_stop(const std::string &game_id);

  /// Test host mode: when `cmd` is a Playnite launch command (launch_command()), logs
  /// `{"playnite_launch": id}` to the events stream and writes the fake library's state.json as
  /// "that game is running". Returns whether `cmd` was one. Does nothing outside test mode.
  bool test_host_launch(const std::string &cmd);

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
