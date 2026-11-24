/**
 * @file tests/unit/test_video.cpp
 * @brief Test src/video.*.
 */
#include "../tests_common.h"

#include <src/video.h>

struct EncoderTest: PlatformTestSuite, testing::WithParamInterface<video::encoder_t *> {
  void SetUp() override {
    auto &encoder = *GetParam();
    if (!video::validate_encoder(encoder, false)) {
      // Encoder failed validation,
      // if it's software - fail, otherwise skip
      if (encoder.name == "software") {
        FAIL() << "Software encoder not available";
      } else {
        GTEST_SKIP() << "Encoder not available";
      }
    }
  }
};

INSTANTIATE_TEST_SUITE_P(
  EncoderVariants,
  EncoderTest,
  testing::Values(
#if !defined(__APPLE__)
    &video::nvenc,
#endif
#ifdef _WIN32
    &video::amdvce,
    &video::quicksync,
#endif
#ifdef __linux__
    &video::vaapi,
#endif
#ifdef __APPLE__
    &video::videotoolbox,
#endif
    &video::software
  ),
  [](const auto &info) {
    return std::string(info.param->name);
  }
);

TEST_P(EncoderTest, ValidateEncoder) {
  // todo:: test something besides fixture setup
}

#include <src/benchmark_scene_formula.h>
#include <src/config.h>
#include <src/nvhttp.h>
#include <src/test_host.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

namespace {
  /// A BGRA picture with some row padding, like a real capture's.
  struct bgra_image_t {
    bgra_image_t(int w, int h):
        width {w},
        height {h},
        row_pitch {w * 4 + 64},
        pixels(static_cast<std::size_t>(row_pitch) * h) {
    }

    void render(std::uint64_t frame) {
      benchmark_scene::render_bgra(pixels.data(), width, height, row_pitch, frame);
    }

    std::uint64_t read() const {
      return benchmark_scene::read_frame_number(pixels.data(), width, height, row_pitch);
    }

    int width;
    int height;
    int row_pitch;
    std::vector<std::uint8_t> pixels;
  };

  /// Test host mode and the synthetic capture on for the scope; restores both.
  struct SyntheticScope {
    SyntheticScope():
        saved_test_host {config::sunshine.test_host},
        saved_capture {config::video.capture} {
      config::sunshine.test_host = true;
      config::video.capture = "synthetic";
    }

    ~SyntheticScope() {
      config::sunshine.test_host = saved_test_host;
      config::video.capture = saved_capture;
    }

    bool saved_test_host;
    std::string saved_capture;
  };
}  // namespace

TEST(Video, SyntheticFrameNumberRoundTrips) {
  bgra_image_t image {640, 360};
  for (std::uint64_t frame : {0ull, 1ull, 255ull, 1'000'000ull, 0xFFFF'FFFFull}) {
    image.render(frame);
    EXPECT_EQ(image.read(), frame) << "frame " << frame;
  }

  // Lossy compression stand-in: every byte off by up to +/-60, cell edges included. The reader
  // averages the centre of each cell, so the number still comes back.
  image.render(1'000'000);
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width * 4; ++x) {
      auto &byte = image.pixels[static_cast<std::size_t>(y) * image.row_pitch + x];
      const int noise = (x * 7 + y * 13) % 121 - 60;
      byte = static_cast<std::uint8_t>(std::clamp(byte + noise, 0, 255));
    }
  }
  EXPECT_EQ(image.read(), 1'000'000u);

  // Only the low 32 bits are stamped
  image.render((1ull << 40) + 7);
  EXPECT_EQ(image.read(), 7u);
}

TEST(Video, SyntheticFramesDifferOverTime) {
  bgra_image_t a {320, 240};
  bgra_image_t b {320, 240};
  const int picture_rows = a.height - benchmark_scene::frame_cell_px;  // above the frame number

  for (std::uint64_t frame = 0; frame < 4; ++frame) {
    a.render(frame);
    b.render(frame + 1);

    int changed = 0;
    for (int y = 0; y < picture_rows; ++y) {
      for (int x = 0; x < a.width; ++x) {
        const auto offset = static_cast<std::size_t>(y) * a.row_pitch + x * 4;
        changed += !std::equal(a.pixels.data() + offset, a.pixels.data() + offset + 4, b.pixels.data() + offset);
      }
    }
    // The picture itself moves, not just the number: most of it changes every frame
    EXPECT_GT(changed, a.width * picture_rows / 2) << "frame " << frame;
  }

  // A function of the frame number alone
  a.render(42);
  b.render(42);
  EXPECT_EQ(a.pixels, b.pixels);
}

TEST(Video, TestHostAdvertisesH264Only) {
  const auto saved_test_host = config::sunshine.test_host;
  const auto saved_hevc = video::active_hevc_mode;
  const auto saved_av1 = video::active_av1_mode;
  const auto saved_pyrowave = video::active_pyrowave_mode;
  const auto saved_yuv444 = video::last_encoder_probe_supported_yuv444_for_codec;

  // As if the probe had found everything
  video::active_hevc_mode = 3;
  video::active_av1_mode = 3;
  video::active_pyrowave_mode = 2;
  video::last_encoder_probe_supported_yuv444_for_codec = {true, true, true};

  config::sunshine.test_host = false;
  const auto normal = nvhttp::server_codec_mode_support();
  config::sunshine.test_host = true;
  const auto test_mode = nvhttp::server_codec_mode_support();

  config::sunshine.test_host = saved_test_host;
  video::active_hevc_mode = saved_hevc;
  video::active_av1_mode = saved_av1;
  video::active_pyrowave_mode = saved_pyrowave;
  video::last_encoder_probe_supported_yuv444_for_codec = saved_yuv444;

  EXPECT_NE(normal & SCM_HEVC, 0u);
  EXPECT_NE(normal & SCM_AV1_MAIN8, 0u);
  EXPECT_EQ(test_mode, static_cast<std::uint32_t>(SCM_H264));
}

#ifdef _WIN32
TEST(Video, SyntheticCaptureIsPacedAndNumbered) {
  SyntheticScope scope;

  video::config_t config {};
  config.width = 320;
  config.height = 240;
  config.framerate = 60;

  // The factories hand out the synthetic display, and never a real one
  EXPECT_EQ(platf::display_names(platf::mem_type_e::system), std::vector<std::string> {"synthetic"});
  EXPECT_FALSE(platf::display(platf::mem_type_e::dxgi, "synthetic", config));
  auto disp = platf::display(platf::mem_type_e::system, "DISPLAY1", config);
  ASSERT_TRUE(disp);
  EXPECT_EQ(disp->width, 320);
  EXPECT_EQ(disp->height, 240);
  EXPECT_FALSE(disp->is_hdr());
  EXPECT_TRUE(disp->is_codec_supported("libx264", config));
  auto hevc = config;
  hevc.videoFormat = 1;
  EXPECT_FALSE(disp->is_codec_supported("libx265", hevc));
  auto yuv444 = config;
  yuv444.chromaSamplingType = 1;
  EXPECT_FALSE(disp->is_codec_supported("libx264", yuv444));

  // dummy_img() is black
  auto dummy = disp->alloc_img();
  ASSERT_TRUE(dummy);
  ASSERT_EQ(disp->dummy_img(dummy.get()), 0);
  EXPECT_EQ(dummy->row_pitch, 320 * 4);
  EXPECT_TRUE(std::all_of(dummy->data, dummy->data + dummy->row_pitch * dummy->height, [](auto byte) {
    return byte == 0;
  }));

  // Capture through the same kind of callbacks as video.cpp: a small pool, images in use are skipped
  std::vector<std::shared_ptr<platf::img_t>> pool {disp->alloc_img(), disp->alloc_img(), disp->alloc_img()};
  auto pull = [&](std::shared_ptr<platf::img_t> &img_out) {
    for (auto &img : pool) {
      if (img.use_count() == 1) {
        img_out = img;
        img_out->frame_timestamp.reset();
        return true;
      }
    }
    return false;
  };

  constexpr std::size_t wanted = 6;
  std::vector<std::uint64_t> numbers;
  std::vector<std::chrono::steady_clock::time_point> timestamps;
  auto push = [&](std::shared_ptr<platf::img_t> &&img, bool frame_captured) {
    if (frame_captured) {
      EXPECT_TRUE(img->frame_timestamp);
      if (img->frame_timestamp) {
        timestamps.push_back(*img->frame_timestamp);
      }
      numbers.push_back(benchmark_scene::read_frame_number(img->data, img->width, img->height, img->row_pitch));
    }
    return numbers.size() < wanted;
  };

  bool cursor = false;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(disp->capture(push, pull, &cursor), platf::capture_e::ok);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_EQ(numbers.size(), wanted);
  EXPECT_EQ(numbers.front(), 0u);
  for (std::size_t i = 1; i < numbers.size(); ++i) {
    EXPECT_GT(numbers[i], numbers[i - 1]);
  }
  ASSERT_EQ(timestamps.size(), wanted);
  for (std::size_t i = 1; i < timestamps.size(); ++i) {
    EXPECT_GE(timestamps[i], timestamps[i - 1]);
  }
  // Frames 0 to 5 at 60 fps: frame 5 is due 83 ms after the first one
  EXPECT_GE(elapsed, std::chrono::milliseconds(75));
  EXPECT_LT(elapsed, std::chrono::seconds(2));
}

TEST(Video, TestHostProbesSoftwareH264Only) {
  SyntheticScope scope;
  const auto saved_hevc = video::active_hevc_mode;
  const auto saved_av1 = video::active_av1_mode;
  // Asked for explicitly (in auto mode the software encoder is H264_ONLY and would not even try):
  // the probe tries HEVC and AV1, and the synthetic display must turn them down
  video::active_hevc_mode = 2;
  video::active_av1_mode = 2;

  // Encodes frames from the synthetic display with libx264 (validate_config())
  const bool valid = video::validate_encoder(video::software, false);

  video::active_hevc_mode = saved_hevc;
  video::active_av1_mode = saved_av1;

  ASSERT_TRUE(valid);
  EXPECT_TRUE(video::software.h264[video::encoder_t::PASSED]);
  EXPECT_FALSE(video::software.h264[video::encoder_t::YUV444]);
  EXPECT_FALSE(video::software.hevc[video::encoder_t::PASSED]);
  EXPECT_FALSE(video::software.av1[video::encoder_t::PASSED]);
}
#endif
