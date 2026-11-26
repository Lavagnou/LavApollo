/**
 * @file tests/unit/test_playnite.cpp
 * @brief Test src/playnite.*
 */
#include "../tests_common.h"

#include <cstdlib>
#include <fstream>

#include <src/config.h>
#include <src/playnite.h>

using namespace std::chrono_literals;
using playnite::watch_t;

namespace {
  const char *kLibrary = R"({
    "version": 1,
    "games": [
      {"id": "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28", "name": "Cyberpunk 2077", "cover": "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28.png"},
      {"id": "2f03ecc8-d3a6-4453-815a-ff75f8f7fedb", "name": "No cover", "cover": ""},
      {"id": "not-a-guid", "name": "Rejected"},
      {"id": "11111111-2222-3333-4444-555555555555", "name": ""},
      {"id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee", "name": "Escaping cover", "cover": "..\\..\\evil.png"}
    ]
  })";

  playnite::state_t running(const std::string &id, bool starting = false) {
    playnite::state_t state;
    state.playnite_running = true;
    state.game = playnite::running_game_t {id, "Game", starting};
    return state;
  }

  playnite::state_t idle() {
    playnite::state_t state;
    state.playnite_running = true;
    return state;
  }
}  // namespace

TEST(Playnite, parseLibraryKeepsValidGames) {
  auto games = playnite::parse_library(kLibrary, "C:\\covers");
  ASSERT_EQ(games.size(), 3u);
  EXPECT_EQ(games[0].id, "FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28");
  EXPECT_EQ(games[0].name, "Cyberpunk 2077");
  EXPECT_EQ(games[0].cover, std::filesystem::path("C:\\covers") / "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28.png");
  EXPECT_TRUE(games[1].cover.empty());
  // A cover that is not a bare file name inside covers\ is dropped, not followed.
  EXPECT_EQ(games[2].name, "Escaping cover");
  EXPECT_TRUE(games[2].cover.empty());
}

TEST(Playnite, sortRecentFirst) {
  auto games = playnite::parse_library(R"({"games": [
    {"id": "11111111-0000-0000-0000-000000000001", "name": "never played b", "lastActivity": ""},
    {"id": "11111111-0000-0000-0000-000000000002", "name": "Old", "lastActivity": "2024-11-28T14:26:17.0380000Z"},
    {"id": "11111111-0000-0000-0000-000000000003", "name": "Never played A"},
    {"id": "11111111-0000-0000-0000-000000000004", "name": "Recent", "lastActivity": "2026-09-23T07:30:06.0000000Z"}
  ]})", "C:\covers");
  ASSERT_EQ(games.size(), 4u);
  EXPECT_EQ(games[1].last_activity, "2024-11-28T14:26:17.0380000Z");
  playnite::sort_recent_first(games);
  EXPECT_EQ(games[0].name, "Recent");
  EXPECT_EQ(games[1].name, "Old");
  EXPECT_EQ(games[2].name, "Never played A");
  EXPECT_EQ(games[3].name, "never played b");
}

TEST(Playnite, sortRecentFirstComparesDatesNotText) {
  // The testbench and older exports write whole seconds; Playnite's "o" format has 7 decimals.
  auto games = playnite::parse_library(R"({"games": [
    {"id": "11111111-0000-0000-0000-000000000001", "name": "A", "lastActivity": "2026-11-20T21:00:00.5000000Z"},
    {"id": "11111111-0000-0000-0000-000000000002", "name": "B", "lastActivity": "2026-11-20T21:00:01Z"},
    {"id": "11111111-0000-0000-0000-000000000003", "name": "C", "lastActivity": "2026-11-20T22:00:00+02:00"}
  ]})", "C:\\covers");
  playnite::sort_recent_first(games);
  ASSERT_EQ(games.size(), 3u);
  EXPECT_EQ(games[0].name, "B");
  EXPECT_EQ(games[1].name, "A");
  EXPECT_EQ(games[2].name, "C");  // 20:00 UTC
}

TEST(Playnite, parseIsoUtc) {
  EXPECT_EQ(playnite::parse_iso_utc("1970-01-02T00:00:00Z"), 0);  // placeholder-old: "never"
  EXPECT_EQ(playnite::parse_iso_utc("2026-11-20T21:00:00Z"), 1795208400);
  EXPECT_EQ(playnite::parse_iso_utc("2026-11-20T21:00:00.0000000Z"), 1795208400);
  EXPECT_EQ(playnite::parse_iso_utc("2026-11-20T22:30:00+01:30"), 1795208400);
  EXPECT_EQ(playnite::parse_iso_utc("2026-11-20T21:00:00"), 1795208400);
  EXPECT_EQ(playnite::parse_iso_utc("0001-01-01T00:00:00.0000000"), 0);  // DateTime.MinValue
  EXPECT_EQ(playnite::parse_iso_utc(""), 0);
  EXPECT_EQ(playnite::parse_iso_utc("yesterday"), 0);
  EXPECT_EQ(playnite::parse_iso_utc("2026-13-20T21:00:00Z"), 0);
  EXPECT_EQ(playnite::parse_iso_utc("2026-11-20T21:00:00Zjunk"), 0);
}

TEST(Playnite, parseLibraryExtraFields) {
  auto games = playnite::parse_library(R"({"games": [
    {"id": "11111111-0000-0000-0000-000000000001", "name": "Installed", "installed": true, "playtime": 7200,
     "background": "11111111-0000-0000-0000-000000000001.jpg", "lastActivity": "2026-11-20T21:00:00Z"},
    {"id": "11111111-0000-0000-0000-000000000002", "name": "Not installed", "installed": false, "playtime": 0,
     "background": "..\\escape.jpg"},
    {"id": "11111111-0000-0000-0000-000000000003", "name": "Old export"}
  ]})", "C:\\pn\\covers");
  ASSERT_EQ(games.size(), 3u);
  EXPECT_TRUE(games[0].installed);
  EXPECT_EQ(games[0].playtime, 7200u);
  EXPECT_EQ(games[0].last_played, 1795208400);
  EXPECT_EQ(games[0].background, std::filesystem::path("C:\\pn") / "backgrounds" / "11111111-0000-0000-0000-000000000001.jpg");
  EXPECT_FALSE(games[1].installed);
  EXPECT_TRUE(games[1].background.empty());  // not a bare file name: dropped
  EXPECT_EQ(games[1].last_played, 0);
  // Extension 1.0.x: installed games only, no installed field
  EXPECT_TRUE(games[2].installed);
  EXPECT_EQ(games[2].playtime, 0u);
}

TEST(Playnite, libraryOrderFollowsTheLibrary) {
  auto library = playnite::parse_library(R"({"games": [
    {"id": "11111111-0000-0000-0000-000000000001", "name": "Old", "lastActivity": "2026-01-01T00:00:00Z"},
    {"id": "11111111-0000-0000-0000-000000000002", "name": "Just played", "lastActivity": "2026-11-25T20:00:00Z"}
  ]})", "C:\\covers");
  playnite::sort_recent_first(library);
  // The app list was built before "Just played" was played; a third game left the library since
  std::vector<std::string> ids {"11111111-0000-0000-0000-000000000001", "11111111-0000-0000-0000-0000000000AA", "11111111-0000-0000-0000-000000000002"};
  auto order = playnite::library_order(ids, library);
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 2u);
  EXPECT_EQ(order[1], 0u);
  EXPECT_EQ(order[2], 1u);
  EXPECT_EQ(playnite::find_game(library, "11111111-0000-0000-0000-000000000002")->name, "Just played");
  EXPECT_EQ(playnite::find_game(library, "11111111-0000-0000-0000-0000000000AA"), nullptr);
}

TEST(Playnite, parseLibraryMalformed) {
  EXPECT_TRUE(playnite::parse_library("{not json", "C:\\covers").empty());
  EXPECT_TRUE(playnite::parse_library("{\"games\": 3}", "C:\\covers").empty());
}

TEST(Playnite, parseState) {
  auto state = playnite::parse_state(R"({"playniteRunning": true, "mode": "fullscreen",
    "game": {"id": "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28", "name": "Cyberpunk 2077", "starting": true}})");
  ASSERT_TRUE(state.has_value());
  EXPECT_TRUE(state->playnite_running);
  EXPECT_EQ(state->mode, "fullscreen");
  ASSERT_TRUE(state->game.has_value());
  EXPECT_EQ(state->game->id, "FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28");
  EXPECT_TRUE(state->game->starting);
  EXPECT_EQ(state->game->started_at, 0);

  auto started = playnite::parse_state(R"({"playniteRunning": true, "mode": "fullscreen",
    "game": {"id": "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28", "name": "Cyberpunk 2077", "startedAt": "2026-11-20T21:00:00.0000000Z"}})");
  ASSERT_TRUE(started && started->game);
  EXPECT_EQ(started->game->started_at, 1795208400);
}

TEST(Playnite, closedPlayniteRunsNoGame) {
  auto state = playnite::parse_state(R"({"playniteRunning": false,
    "game": {"id": "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28", "name": "Left behind"}})");
  ASSERT_TRUE(state.has_value());
  EXPECT_FALSE(state->game.has_value());
  EXPECT_FALSE(playnite::parse_state("garbage").has_value());
}

TEST(Playnite, launchCommandUsesThePlayniteUri) {
  EXPECT_EQ(playnite::launch_command("FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28"),
            "cmd /c start \"\" \"playnite://playnite/start/fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28\"");
}

TEST(Playnite, recognisesThePlayniteFrontEnd) {
  EXPECT_TRUE(playnite::is_playnite_command("cmd /c \"D:\\Games\\Playnite\\Playnite.FullscreenApp.exe\""));
  EXPECT_TRUE(playnite::is_playnite_command("D:\\Playnite\\playnite.desktopapp.exe --hidesplashscreen"));
  EXPECT_FALSE(playnite::is_playnite_command("steam://rungameid/570"));
  EXPECT_FALSE(playnite::is_playnite_command(""));
}

TEST(Playnite, watchEndsWhenTheSeenGameCloses) {
  auto t0 = std::chrono::steady_clock::now();
  watch_t watch {"fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28", t0};
  EXPECT_EQ(watch.update(idle(), t0 + 1s), watch_t::verdict_e::running);
  EXPECT_EQ(watch.update(running("FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28", true), t0 + 2s), watch_t::verdict_e::running);
  EXPECT_EQ(watch.update(running("FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28"), t0 + 30s), watch_t::verdict_e::running);
  // An unreadable state.json is not a closed game.
  EXPECT_EQ(watch.update(std::nullopt, t0 + 31s), watch_t::verdict_e::running);
  EXPECT_EQ(watch.update(idle(), t0 + 32s), watch_t::verdict_e::ended);
}

TEST(Playnite, watchEndsWhenAnotherGameReplacesIt) {
  auto t0 = std::chrono::steady_clock::now();
  watch_t watch {"FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28", t0};
  watch.update(running("FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28"), t0 + 5s);
  EXPECT_EQ(watch.update(running("2F03ECC8-D3A6-4453-815A-FF75F8F7FEDB"), t0 + 6s), watch_t::verdict_e::ended);
}

TEST(Playnite, watchGivesUpOnAGameThatNeverStarts) {
  auto t0 = std::chrono::steady_clock::now();
  watch_t watch {"FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28", t0, 120s};
  EXPECT_EQ(watch.update(idle(), t0 + 119s), watch_t::verdict_e::running);
  EXPECT_EQ(watch.update(std::nullopt, t0 + 120s), watch_t::verdict_e::never_started);
}

TEST(Playnite, inactiveWatchKeepsRunning) {
  watch_t watch;
  EXPECT_FALSE(watch.active());
  EXPECT_EQ(watch.update(idle(), std::chrono::steady_clock::now()), watch_t::verdict_e::running);
}

namespace {
  class PlayniteTestHost: public ::testing::Test {
  protected:
    void SetUp() override {
      const char *old = std::getenv("ProgramData");
      had_env = old != nullptr;
      if (old) {
        old_env = old;
      }
      tmp = test_utils::scratch_dir() / ("lav_playnite_th_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
      std::filesystem::remove_all(tmp);
      _putenv_s("ProgramData", (tmp / "appdata" / "pd").string().c_str());
      std::filesystem::create_directories(playnite::exchange_dir() / "commands");
      old_appdata = std::getenv("LAVAPOLLO_APPDATA") ? std::getenv("LAVAPOLLO_APPDATA") : "";
      _putenv_s("LAVAPOLLO_APPDATA", (tmp / "appdata").string().c_str());
      saved = config::sunshine.test_host;
      config::sunshine.test_host = true;
      std::ofstream {playnite::exchange_dir() / "library.json"} << kLibrary;
    }

    void TearDown() override {
      config::sunshine.test_host = saved;
      _putenv_s("LAVAPOLLO_APPDATA", old_appdata.c_str());
      _putenv_s("ProgramData", had_env ? old_env.c_str() : "");
      std::error_code ec;
      std::filesystem::remove_all(tmp, ec);
    }

    std::string state_text() {
      std::ifstream in {playnite::exchange_dir() / "state.json"};
      return {std::istreambuf_iterator<char>(in), {}};
    }

    std::filesystem::path tmp;
    bool had_env = false;
    bool saved = false;
    std::string old_env;
    std::string old_appdata;
  };
}  // namespace

TEST_F(PlayniteTestHost, TestHostLaunchWritesRunningState) {
  auto id = "fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28";
  EXPECT_TRUE(playnite::test_host_launch(playnite::launch_command(id)));
  EXPECT_FALSE(playnite::test_host_launch("cmd /c echo hello"));  // not a Playnite launch

  auto state = playnite::parse_state(state_text());
  ASSERT_TRUE(state);
  EXPECT_TRUE(state->playnite_running);
  ASSERT_TRUE(state->game);
  EXPECT_EQ(state->game->id, "FB5D018A-058E-4FFE-AE4B-3FD3D48A5B28");
  EXPECT_EQ(state->game->name, "Cyberpunk 2077");
  EXPECT_FALSE(state->game->starting);

  // A stop request empties the game again
  playnite::request_stop(id);
  state = playnite::parse_state(state_text());
  ASSERT_TRUE(state);
  EXPECT_TRUE(state->playnite_running);
  EXPECT_FALSE(state->game);
}

TEST_F(PlayniteTestHost, RefusesToWriteOutsideTheTestAppdata) {
  _putenv_s("ProgramData", (tmp / "elsewhere").string().c_str());
  std::filesystem::create_directories(playnite::exchange_dir());
  std::ofstream {playnite::exchange_dir() / "library.json"} << kLibrary;

  playnite::test_host_launch(playnite::launch_command("fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28"));
  playnite::request_stop("fb5d018a-058e-4ffe-ae4b-3fd3d48a5b28");
  EXPECT_FALSE(std::filesystem::exists(playnite::exchange_dir() / "state.json"));
}
