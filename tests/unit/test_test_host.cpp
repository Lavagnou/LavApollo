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
