/**
 * @file tests/unit/test_audio.cpp
 * @brief Test src/audio.*.
 */
#include "../tests_common.h"

#include <algorithm>
#include <chrono>
#include <vector>

#include <src/audio.h>

using namespace audio;

struct AudioTest: PlatformTestSuite, testing::WithParamInterface<std::tuple<std::basic_string_view<char>, config_t>> {
  void SetUp() override {
    m_config = std::get<1>(GetParam());
    m_mail = std::make_shared<safe::mail_raw_t>();
  }

  config_t m_config;
  safe::mail_t m_mail;
};

constexpr std::bitset<config_t::MAX_FLAGS> config_flags(const int flag = -1) {
  std::bitset<3> result = std::bitset<config_t::MAX_FLAGS>();
  if (flag >= 0) {
    result.set(flag);
  }
  return result;
}

INSTANTIATE_TEST_SUITE_P(
  Configurations,
  AudioTest,
  testing::Values(
    std::make_tuple("HIGH_STEREO", config_t {5, 2, 0x3, {0}, config_flags(config_t::HIGH_QUALITY)}),
    std::make_tuple("SURROUND51", config_t {5, 6, 0x3F, {0}, config_flags()}),
    std::make_tuple("SURROUND71", config_t {5, 8, 0x63F, {0}, config_flags()}),
    std::make_tuple("SURROUND51_CUSTOM", config_t {5, 6, 0x3F, {6, 4, 2, {0, 1, 4, 5, 2, 3}}, config_flags(config_t::CUSTOM_SURROUND_PARAMS)})
  ),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(AudioTest, TestEncode) {
  std::thread timer([&] {
    // Terminate the audio capture after 100 ms
    std::this_thread::sleep_for(100ms);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    const auto audio_packets = m_mail->queue<packet_t>(mail::audio_packets);
    shutdown_event->raise(true);
    audio_packets->stop();
  });
  std::thread capture([&] {
    const auto packets = m_mail->queue<packet_t>(mail::audio_packets);
    const auto shutdown_event = m_mail->event<bool>(mail::shutdown);
    while (const auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }
      if (auto packet_data = packet->second; packet_data.size() == 0) {
        FAIL() << "Empty packet data";
      }
    }
  });
  audio::capture(m_mail, m_config, nullptr);

  timer.join();
  capture.join();
}

#ifdef _WIN32
  #include <cmath>

  #include <src/config.h>
  #include <src/platform/windows/audio_synthetic.h>
  #include <src/test_host.h>

namespace {
  /// Test host mode on for the scope; the real audio factory must never run in these tests.
  struct TestHostScope {
    TestHostScope():
        saved {config::sunshine.test_host} {
      config::sunshine.test_host = true;
    }

    ~TestHostScope() {
      config::sunshine.test_host = saved;
    }

    bool saved;
  };
}  // namespace

TEST(Audio, TestHostToneHasExpectedFrequency) {
  TestHostScope scope;
  auto control = platf::audio_control();  // factory: must be the synthetic one
  ASSERT_TRUE(control);

  auto sink = control->sink_info();
  ASSERT_TRUE(sink);
  EXPECT_FALSE(sink->host.empty());
  EXPECT_TRUE(control->is_sink_available(sink->host));
  EXPECT_EQ(control->set_sink("anything"), 0);

  constexpr int channels = 2;
  constexpr std::uint32_t rate = 48000;
  constexpr std::uint32_t frame = 480;  // 10 ms
  const std::uint8_t mapping[2] = {0, 1};
  auto mic = control->microphone(mapping, channels, rate, frame);
  ASSERT_TRUE(mic);

  // 50 frames: a whole number of 440 Hz periods only at the end, so the phase must carry over frame to frame
  std::vector<float> all;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 50; ++i) {
    std::vector<float> buffer;
    ASSERT_EQ(mic->sample(buffer), platf::capture_e::ok);
    ASSERT_EQ(buffer.size(), frame * channels);
    all.insert(all.end(), buffer.begin(), buffer.end());
  }
  auto elapsed = std::chrono::steady_clock::now() - start;

  // Real-time pacing: 49 frame intervals of 10 ms (the first frame is immediate)
  EXPECT_GE(elapsed, std::chrono::milliseconds(450));
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000));

  // Left channel: peak 0.1, never above; both channels identical
  float peak = 0;
  int crossings = 0;
  float previous = 0;
  for (std::size_t i = 0; i < all.size(); i += channels) {
    EXPECT_EQ(all[i], all[i + 1]);
    peak = std::max(peak, std::abs(all[i]));
    if (i > 0 && previous < 0 && all[i] >= 0) {
      ++crossings;  // upward zero crossings = periods
    }
    previous = all[i];
  }
  EXPECT_NEAR(peak, 0.1f, 0.002f);

  // 0.5 s of audio: 220 periods, a break of phase at a frame boundary would add or remove one
  EXPECT_NEAR(crossings, 220, 1);
}
#endif
