/**
 * @file src/platform/windows/display_synthetic.cpp
 * @brief The test host's capture: a generated picture instead of a real display.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <thread>

// local includes
#include "display_synthetic.h"
#include "src/benchmark_scene_formula.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/test_host.h"

using namespace std::literals;

namespace platf {
  namespace {
    /// A BGRA image in system memory, the format the software encoder converts from (BGR0).
    struct synthetic_img_t: img_t {
      ~synthetic_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    /// Longest single wait: a stop request is noticed at least this often, even at 1 fps.
    constexpr auto max_wait = 100ms;
  }  // namespace

  display_synthetic_t::display_synthetic_t(const ::video::config_t &config):
      rate {std::clamp(config.framerate, 1, 1000)} {
    width = config.width;
    height = config.height;
    env_width = width;
    env_height = height;
  }

  capture_e display_synthetic_t::capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *) {
    using clock = std::chrono::steady_clock;

    auto timer = create_high_precision_timer();
    auto sleep = [&](clock::duration duration) {
      if (timer && *timer) {
        timer->sleep_for(duration);
      } else {
        std::this_thread::sleep_for(duration);
      }
    };

    const auto start = clock::now();
    auto due = [&](std::uint64_t n) {
      return start + std::chrono::nanoseconds(n * 1'000'000'000ull / static_cast<std::uint64_t>(rate));
    };

    sleep_overshoot_logger.reset();

    std::uint64_t frame = 0;
    while (true) {
      auto now = clock::now();
      if (now >= due(frame + 1)) {
        // A whole period late (slow encoder, full image pool): skip to the slot due now rather than
        // send the missed frames in a burst. Their numbers are skipped with them.
        const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
        frame = elapsed * static_cast<std::uint64_t>(rate) / 1'000'000'000ull;
      } else if (const auto target = due(frame); target > now) {
        if (target - now > max_wait) {
          sleep(max_wait);
          // No frame yet: give the capture loop its chance to stop or switch
          if (!push_captured_image_cb(nullptr, false)) {
            return capture_e::ok;
          }
          continue;
        }

        sleep(target - now);
        sleep_overshoot_logger.first_point(target);
        sleep_overshoot_logger.second_point_now_and_log();
      }

      std::shared_ptr<img_t> img;
      if (!pull_free_image_cb(img)) {
        return capture_e::interrupted;
      }
      if (!img || !img->data) {
        BOOST_LOG(error) << "Synthetic capture got an image without pixels"sv;
        return capture_e::error;
      }

      benchmark_scene::render_bgra(img->data, img->width, img->height, img->row_pitch, frame);
      img->frame_timestamp = clock::now();

      if (!push_captured_image_cb(std::move(img), true)) {
        return capture_e::ok;
      }
      ++frame;
    }
  }

  std::shared_ptr<img_t> display_synthetic_t::alloc_img() {
    auto img = std::make_shared<synthetic_img_t>();
    img->width = width;
    img->height = height;
    img->pixel_pitch = 4;
    img->row_pitch = width * 4;
    img->data = new std::uint8_t[static_cast<std::size_t>(img->row_pitch) * height];
    return img;
  }

  int display_synthetic_t::dummy_img(img_t *img) {
    if (!img) {
      return -1;
    }
    if (!img->data) {
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = width * 4;
      img->data = new std::uint8_t[static_cast<std::size_t>(img->row_pitch) * height];
    }

    std::fill_n(img->data, static_cast<std::size_t>(img->row_pitch) * img->height, std::uint8_t {0});
    return 0;
  }

  std::unique_ptr<avcodec_encode_device_t> display_synthetic_t::make_avcodec_encode_device(pix_fmt_e) {
    // No hardware frames: the encoder converts the BGRA image itself (avcodec_software_encode_device_t)
    return std::make_unique<avcodec_encode_device_t>();
  }

  bool display_synthetic_t::is_codec_supported(std::string_view, const ::video::config_t &config) {
    // H.264, 4:2:0, 8-bit: what the test host advertises (nvhttp::server_codec_mode_support()).
    // Refusing the rest here also keeps the encoder probe from validating HEVC, AV1 or 4:4:4.
    return config.videoFormat == 0 && config.chromaSamplingType == 0 && config.dynamicRange == 0;
  }

  bool synthetic_capture_selected() {
    // Test host mode only: outside it `capture = synthetic` is not a value (and test mode forces it anyway)
    return test_host::enabled();
  }

  std::shared_ptr<display_t> synthetic_display(mem_type_e hwdevice_type, const ::video::config_t &config) {
    if (hwdevice_type != mem_type_e::system) {
      // Never a real display instead: the probe moves on to the software encoder
      BOOST_LOG(warning) << "Synthetic capture only feeds the software encoder"sv;
      return nullptr;
    }
    if (config.width <= 0 || config.height <= 0) {
      BOOST_LOG(error) << "Synthetic capture: invalid size "sv << config.width << 'x' << config.height;
      return nullptr;
    }

    BOOST_LOG(info) << "Synthetic capture: "sv << config.width << 'x' << config.height << " at "sv << std::clamp(config.framerate, 1, 1000) << " fps"sv;
    return std::make_shared<display_synthetic_t>(config);
  }
}  // namespace platf
