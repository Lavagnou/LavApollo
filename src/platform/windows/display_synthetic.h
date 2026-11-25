/**
 * @file src/platform/windows/display_synthetic.h
 * @brief The test host's capture: a generated picture instead of a real display.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>

// local includes
#include "src/platform/common.h"
#include "src/video.h"

namespace platf {
  /**
   * @brief A display that never looks at the screen: every frame is drawn by
   * benchmark_scene::render_bgra() (src/benchmark_scene_formula.h), numbered from 0.
   *
   * System memory only (BGRA images the software encoder converts itself), SDR, at the size and
   * rate of the stream config. It offers H.264 4:2:0 8-bit only: the test host advertises nothing
   * else.
   *
   * Pacing: frame n is due at capture start + n / framerate. A frame that misses its slot by a
   * whole period is skipped, never sent late in a burst; its number is skipped too, so a frame
   * lost on the host shows as a gap in the numbers like one lost on the network.
   */
  class display_synthetic_t: public display_t {
  public:
    explicit display_synthetic_t(const ::video::config_t &config);

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override;

    std::shared_ptr<img_t> alloc_img() override;

    int dummy_img(img_t *img) override;

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override;

    bool is_codec_supported(std::string_view name, const ::video::config_t &config) override;

    /// Frames per second the capture is paced at.
    int frame_rate() const {
      return rate;
    }

  private:
    int rate;
  };

  /**
   * @brief The display platf::display() returns in test host mode.
   * @return A synthetic display for system memory, nullptr for any other memory type (a hardware
   * encoder has no synthetic source yet; the caller must not fall back to a real display).
   */
  std::shared_ptr<display_t> synthetic_display(mem_type_e hwdevice_type, const ::video::config_t &config);

  /// Whether platf::display() and display_names() hand out the synthetic display: in test host mode only.
  bool synthetic_capture_selected();
}  // namespace platf
