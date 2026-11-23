/**
 * @file tests/unit/test_playnite.cpp
 * @brief Test src/playnite.*
 */
#include "../tests_common.h"

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
