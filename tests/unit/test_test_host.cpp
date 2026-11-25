/**
 * @file tests/unit/test_test_host.cpp
 * @brief Test src/test_host.* and the test_host config keys.
 */
#include "../tests_common.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

#include <boost/process/v1.hpp>

#include <src/config.h>
#include <src/current_display.h>
#include <src/platform/common.h>
#include <src/test_host.h>

namespace fs = std::filesystem;

namespace {
  class TestHost: public ::testing::Test {
  protected:
    void SetUp() override {
      const char *old = std::getenv("LAVAPOLLO_APPDATA");
      had_env = old != nullptr;
      if (old) {
        old_env = old;
      }
      tmp = fs::temp_directory_path() / ("lav_test_host_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
      fs::remove_all(tmp);
      fs::create_directories(tmp);
      _putenv_s("LAVAPOLLO_APPDATA", tmp.string().c_str());

      // config::parse writes into the globals: put them back afterwards
      saved_sunshine = config::sunshine;
      saved_audio = config::audio;
      saved_video = config::video;
      saved_limiter = config::frame_limiter;
      saved_file_apps = config::stream.file_apps;
      // apply_config checks this path before it reads the config file: keep it out of the real appdata
      config::stream.file_apps = (tmp / "apps.json").string();
      std::ofstream {tmp / "apps.json"} << "{\"apps\":[]}";  // so apply_config has nothing to copy from the assets
    }

    void TearDown() override {
      config::sunshine = saved_sunshine;
      config::audio = saved_audio;
      config::video = saved_video;
      config::frame_limiter = saved_limiter;
      config::stream.file_apps = saved_file_apps;
      _putenv_s("LAVAPOLLO_APPDATA", had_env ? old_env.c_str() : "");
      std::error_code ec;
      fs::remove_all(tmp, ec);
    }

    // Runs config::parse on a config file holding `body`.
    void parse(const std::string &body) {
      auto conf = tmp / "sunshine.conf";
      {
        std::ofstream out {conf};
        out << "file_apps = " << (tmp / "apps.json").generic_string() << "\n" << body;
      }
      auto conf_str = conf.string();
      std::string arg0 = "sunshine";
      char *argv[] = {arg0.data(), conf_str.data()};
      ASSERT_EQ(config::parse(2, argv), 0);
    }

    fs::path tmp;
    bool had_env = false;
    std::string old_env;
    config::sunshine_t saved_sunshine;
    config::audio_t saved_audio;
    config::video_t saved_video;
    config::frame_limiter_t saved_limiter;
    std::string saved_file_apps;
  };

  std::vector<nlohmann::json> read_lines(const fs::path &path) {
    std::vector<nlohmann::json> lines;
    std::ifstream in {path};
    for (std::string line; std::getline(in, line);) {
      lines.push_back(nlohmann::json::parse(line));
    }
    return lines;
  }
}  // namespace

TEST_F(TestHost, DisabledByDefault) {
  EXPECT_FALSE(test_host::enabled());
  EXPECT_TRUE(config::sunshine.test_host_mac.empty());
  EXPECT_TRUE(config::sunshine.test_host_appversion.empty());
  EXPECT_FALSE(config::sunshine.test_host_this_pc);
  EXPECT_TRUE(config::sunshine.test_host_display.empty());
}

TEST_F(TestHost, AppdataFollowsEnvironment) {
  EXPECT_EQ(platf::appdata(), tmp);
  EXPECT_EQ(test_host::dir(), tmp / "testhost");
}

TEST_F(TestHost, LogsOneJsonLinePerEvent) {
  test_host::log("input", {{"type", "key"}, {"code", 65}});
  test_host::log("input", {{"type", "mouse"}, {"dx", -3}});

  auto lines = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]["type"], "key");
  EXPECT_EQ(lines[0]["code"], 65);
  EXPECT_EQ(lines[1]["dx"], -3);
  EXPECT_TRUE(lines[0]["t"].is_number());
  EXPECT_LE(lines[0]["t"].get<long long>(), lines[1]["t"].get<long long>());
}

TEST_F(TestHost, KeepsCallerTimestamp) {
  test_host::log("events", {{"type", "x"}, {"t", 12345}});
  auto lines = read_lines(test_host::dir() / "events.jsonl");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0]["t"], 12345);
}

TEST_F(TestHost, UnwritableLocationDoesNotThrowNorWrite) {
  // A regular file where the appdata directory should be: neither testhost/ nor any log can be created.
  auto blocker = tmp / "not_a_dir";
  { std::ofstream {blocker} << "x"; }
  _putenv_s("LAVAPOLLO_APPDATA", blocker.string().c_str());

  EXPECT_NO_THROW(test_host::log("commands", {{"cmd", "a"}}));
  EXPECT_NO_THROW(test_host::log("commands", {{"cmd", "b"}}));
  EXPECT_FALSE(fs::exists(test_host::dir() / "commands.jsonl"));

  // And logging works again once the location is fixed.
  _putenv_s("LAVAPOLLO_APPDATA", tmp.string().c_str());
  test_host::log("commands", {{"cmd", "c"}});
  EXPECT_EQ(read_lines(test_host::dir() / "commands.jsonl").size(), 1u);
}

TEST_F(TestHost, RecoversWhenTheDirectoryIsRemoved) {
  test_host::log("events", {{"n", 1}});
  ASSERT_EQ(read_lines(test_host::dir() / "events.jsonl").size(), 1u);

  fs::remove_all(test_host::dir());
  test_host::log("events", {{"n", 2}});
  test_host::log("input", {{"n", 3}});

  auto events = read_lines(test_host::dir() / "events.jsonl");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0]["n"], 2);
  EXPECT_EQ(read_lines(test_host::dir() / "input.jsonl").size(), 1u);
}

TEST_F(TestHost, ConfigWithoutTestHostKeepsDefaults) {
  auto sunshine_before = config::sunshine;
  auto limiter_before = config::frame_limiter.enable;
  auto mic_before = config::audio.mic_set_default;
  auto encoder_before = config::video.encoder;
  auto capture_before = config::video.capture;

  parse("");

  EXPECT_FALSE(test_host::enabled());
  EXPECT_EQ(config::sunshine.enable_discovery, sunshine_before.enable_discovery);
  EXPECT_EQ(config::sunshine.system_tray, sunshine_before.system_tray);
  EXPECT_EQ(config::sunshine.update_auto_check, sunshine_before.update_auto_check);
  EXPECT_EQ(config::audio.mic_set_default, mic_before);
  EXPECT_EQ(config::frame_limiter.enable, limiter_before);
  EXPECT_EQ(config::video.encoder, encoder_before);
  EXPECT_EQ(config::video.capture, capture_before);
  EXPECT_NE(config::video.capture, "synthetic");
}

TEST_F(TestHost, ForcedConfigValues) {
  // Ask for everything the mode must override.
  parse(
    "test_host = enabled\n"
    "test_host_mac = 02:00:00:00:00:01\n"
    "test_host_appversion = 7.1.431.0\n"
    "test_host_this_pc = enabled\n"
    "enable_discovery = enabled\n"
    "system_tray = enabled\n"
    "upnp = enabled\n"
    "update_auto_check = enabled\n"
    "address_family = both\n"
    "mic_set_default = enabled\n"
    "frame_limiter_enable = enabled\n"
    "encoder = nvenc\n"
    "capture = wgc\n"
  );

  EXPECT_TRUE(test_host::enabled());
  EXPECT_EQ(config::sunshine.test_host_mac, "02:00:00:00:00:01");
  EXPECT_EQ(config::sunshine.test_host_appversion, "7.1.431.0");
  EXPECT_TRUE(config::sunshine.test_host_this_pc);

  EXPECT_FALSE(config::sunshine.enable_discovery);
  EXPECT_FALSE(config::sunshine.system_tray);
  EXPECT_FALSE(config::sunshine.flags[config::flag::UPNP]);
  EXPECT_FALSE(config::sunshine.update_auto_check);
  EXPECT_EQ(config::sunshine.address_family, "ipv4");
  EXPECT_FALSE(config::audio.mic_set_default);
  EXPECT_FALSE(config::frame_limiter.enable);
  EXPECT_EQ(config::video.encoder, "software");
  EXPECT_EQ(config::video.capture, "synthetic");
}

TEST_F(TestHost, SkipIsTrueOnlyInTestMode) {
  // Outside test mode: false, and the caller goes on to do the real thing.
  EXPECT_FALSE(test_host::enabled());
  EXPECT_FALSE(test_host::skip("virtual display driver"));

  parse("test_host = enabled\n");
  EXPECT_TRUE(test_host::enabled());
  EXPECT_TRUE(test_host::skip("virtual display driver"));
  EXPECT_TRUE(test_host::skip("virtual display driver"));  // every call, not only the first
}

TEST_F(TestHost, RunCommandIsLoggedNotRun) {
  parse("test_host = enabled\n");
  auto marker = tmp / "ran.txt";
  auto cmd = "cmd /c echo x > \"" + marker.string() + "\"";
  boost::filesystem::path working_dir = tmp.string();
  std::error_code ec;
  auto env = boost::this_process::environment();

  auto child = platf::run_command(true, true, cmd, working_dir, env, nullptr, ec, nullptr);
  EXPECT_FALSE(ec);
  child.wait();  // the stand-in exits at once; give a real run every chance to finish too
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_FALSE(fs::exists(marker));
  auto lines = read_lines(test_host::dir() / "commands.jsonl");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0]["cmd"], cmd);
  EXPECT_EQ(lines[0]["elevated"], true);
  EXPECT_EQ(lines[0]["interactive"], true);
  EXPECT_EQ(lines[0]["working_dir"], tmp.string());
}

TEST_F(TestHost, AppCommandWithAGroupStaysUpUntilTheGroupIsTerminated) {
  parse("test_host = enabled\n");
  boost::filesystem::path working_dir = tmp.string();
  std::error_code ec;
  auto env = boost::this_process::environment();
  boost::process::v1::group group;

  auto child = platf::run_command(false, true, "cmd /c exit 3", working_dir, env, nullptr, ec, &group);
  ASSERT_FALSE(ec);
  std::this_thread::sleep_for(std::chrono::seconds(1));
  EXPECT_TRUE(child.running());

  group.terminate(ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_FALSE(child.running());
}

TEST_F(TestHost, ClipboardIsKeptInMemoryAndLogged) {
  parse("test_host = enabled\n");
  EXPECT_TRUE(platf::set_clipboard("hello"));
  EXPECT_EQ(platf::get_clipboard(), "hello");
  auto lines = read_lines(test_host::dir() / "events.jsonl");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0]["clipboard_set"], "hello");
}

// Input tests: test mode is switched on before any platf input call, so nothing below
// can reach Windows (SendInput, ViGEm, VHF, synthetic pointer devices).
TEST_F(TestHost, KeyboardIsLoggedNotInjected) {
  parse("test_host = enabled\n");
  auto input = platf::input();
  platf::keyboard_update(input, 0xC0, false, 0);  // VK_OEM_3, the voice push-to-talk key
  platf::keyboard_update(input, 0xC0, true, 0);

  auto lines = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]["type"], "key");
  EXPECT_EQ(lines[0]["vk"], 192);
  EXPECT_EQ(lines[0]["down"], true);
  EXPECT_TRUE(lines[0].contains("t"));
  EXPECT_EQ(lines[1]["down"], false);
}

TEST_F(TestHost, MouseScrollAndTextAreLogged) {
  parse("test_host = enabled\n");
  auto input = platf::input();
  platf::touch_port_t port {};
  port.width = 1920;
  port.height = 1080;
  platf::move_mouse(input, 5, -7);
  platf::abs_mouse(input, port, 10.5f, 20.5f);
  platf::button_mouse(input, 3, false);
  platf::button_mouse(input, 3, true);
  platf::scroll(input, 120);
  platf::hscroll(input, -120);
  char text[] = "h\xC3\xA9";
  platf::unicode(input, text, 3);

  auto lines = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(lines.size(), 7u);
  EXPECT_EQ(lines[0], (nlohmann::json {{"type", "mouse_move"}, {"dx", 5}, {"dy", -7}, {"t", lines[0]["t"]}}));
  EXPECT_EQ(lines[1]["type"], "mouse_abs");
  EXPECT_FLOAT_EQ(lines[1]["x"].get<float>(), 10.5f);
  EXPECT_EQ(lines[1]["width"], 1920);
  EXPECT_EQ(lines[2]["type"], "mouse_button");
  EXPECT_EQ(lines[2]["button"], 3);
  EXPECT_EQ(lines[2]["down"], true);
  EXPECT_EQ(lines[3]["down"], false);
  EXPECT_EQ(lines[4]["type"], "scroll");
  EXPECT_EQ(lines[4]["distance"], 120);
  EXPECT_EQ(lines[5]["type"], "hscroll");
  EXPECT_EQ(lines[5]["distance"], -120);
  EXPECT_EQ(lines[6]["type"], "unicode");
  EXPECT_EQ(lines[6]["text"], "h\xC3\xA9");
}

TEST_F(TestHost, TouchAndPenAreLoggedWithoutAPointerDevice) {
  parse("test_host = enabled\n");
  auto input = platf::input();
  auto client = platf::allocate_client_input_context(input);
  platf::touch_port_t port {};
  port.width = 1920;
  port.height = 1080;
  platf::touch_input_t touch {};
  touch.eventType = LI_TOUCH_EVENT_DOWN;
  touch.pointerId = 4;
  touch.x = 0.25f;
  touch.y = 0.5f;
  platf::touch_update(client.get(), port, touch);
  platf::pen_input_t pen {};
  pen.eventType = LI_TOUCH_EVENT_MOVE;
  pen.penButtons = 1;
  pen.x = 0.75f;
  platf::pen_update(client.get(), port, pen);

  auto lines = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0]["type"], "touch");
  EXPECT_EQ(lines[0]["event"], LI_TOUCH_EVENT_DOWN);
  EXPECT_EQ(lines[0]["pointer_id"], 4);
  EXPECT_FLOAT_EQ(lines[0]["x"].get<float>(), 0.25f);
  EXPECT_EQ(lines[1]["type"], "pen");
  EXPECT_EQ(lines[1]["buttons"], 1);
}

TEST_F(TestHost, GamepadIsLoggedWithoutAVirtualController) {
  parse("test_host = enabled\n");
  auto input = platf::input();
  platf::gamepad_id_t id {2, 1};
  platf::gamepad_arrival_t arrival {};
  arrival.type = LI_CTYPE_XBOX;
  arrival.capabilities = LI_CCAP_RUMBLE;
  EXPECT_EQ(platf::alloc_gamepad(input, id, arrival, {}), 0);

  platf::gamepad_state_t state {};
  state.buttonFlags = 0x1000;
  state.lt = 200;
  state.lsX = -1234;
  platf::gamepad_update(input, 2, state);
  platf::gamepad_touch_t touch {id, LI_TOUCH_EVENT_DOWN, 3, 0.5f, 0.5f, 1.0f};
  platf::gamepad_touch(input, touch);
  platf::gamepad_motion_t motion {id, LI_MOTION_TYPE_GYRO, 1.0f, 2.0f, 3.0f};
  platf::gamepad_motion(input, motion);
  platf::gamepad_battery_t battery {id, LI_BATTERY_STATE_CHARGING, 80};
  platf::gamepad_battery(input, battery);
  platf::free_gamepad(input, 2);

  auto lines = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(lines.size(), 6u);
  EXPECT_EQ(lines[0]["type"], "gamepad_alloc");
  EXPECT_EQ(lines[0]["index"], 2);
  EXPECT_EQ(lines[0]["client_index"], 1);
  EXPECT_EQ(lines[1]["type"], "gamepad");
  EXPECT_EQ(lines[1]["buttons"], 0x1000);
  EXPECT_EQ(lines[1]["lt"], 200);
  EXPECT_EQ(lines[1]["ls_x"], -1234);
  EXPECT_EQ(lines[2]["type"], "gamepad_touch");
  EXPECT_EQ(lines[3]["type"], "gamepad_motion");
  EXPECT_FLOAT_EQ(lines[3]["z"].get<float>(), 3.0f);
  EXPECT_EQ(lines[4]["type"], "gamepad_battery");
  EXPECT_EQ(lines[4]["percentage"], 80);
  EXPECT_EQ(lines[5]["type"], "gamepad_free");
}

TEST_F(TestHost, InvalidUtf8InTheLogDoesNotThrowAndIsStillRecorded) {
  parse("test_host = enabled\n");
  auto input = platf::input();
  char bad[] = "a\xFF" "b";
  EXPECT_NO_THROW(platf::unicode(input, bad, 3));
  EXPECT_NO_THROW(test_host::log("events", {{"raw", std::string("x\xC3")}}));

  auto in = read_lines(test_host::dir() / "input.jsonl");
  ASSERT_EQ(in.size(), 1u);
  EXPECT_EQ(in[0]["type"], "unicode");
  EXPECT_NE(in[0]["text"].get<std::string>().find('a'), std::string::npos);
  EXPECT_EQ(read_lines(test_host::dir() / "events.jsonl").size(), 1u);
}

TEST_F(TestHost, AnnouncesAFixedDisplayModeNeverTheRealMonitor) {
  // Default: 1920x1080@60, whatever the monitor of this PC
  parse("test_host = enabled\n");
  auto m = current_display::captured();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->width, 1920);
  EXPECT_EQ(m->height, 1080);
  EXPECT_EQ(m->refresh, 60);

  // The fixture's mode
  parse("test_host = enabled\ntest_host_display = 2560x1600x120\n");
  EXPECT_EQ(config::sunshine.test_host_display, "2560x1600x120");
  m = current_display::captured();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->width, 2560);
  EXPECT_EQ(m->height, 1600);
  EXPECT_EQ(m->refresh, 120);

  // A malformed value is dropped at parse time (warning), the default is announced
  parse("test_host = enabled\ntest_host_display = 2560x1600\n");
  EXPECT_TRUE(config::sunshine.test_host_display.empty());
  m = current_display::captured();
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->width, 1920);
  EXPECT_EQ(m->height, 1080);
  EXPECT_EQ(m->refresh, 60);
}
