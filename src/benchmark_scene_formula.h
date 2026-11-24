/**
 * @file src/benchmark_scene_formula.h
 * @brief The test host's synthetic picture, drawn on the CPU, and the frame number stamped in it.
 *
 * Two users:
 * - the test host's synthetic capture (`src/platform/windows/display_synthetic.cpp`) draws every
 *   frame with render_bgra();
 * - the Benchmark app's GPU scene (`tools/benchmark_scene.cpp`) stamps its frame number with the
 *   same layout: its shader is compiled with FRAME_CELL_PX / FRAME_BITS defined from the constants
 *   below, so the two cannot drift apart.
 *
 * The CPU picture is much plainer than the GPU scene: it only has to move every frame and carry
 * its number, and it must stay cheap (1080p60 on one thread, well under a frame of CPU time).
 *
 * ## Frame number layout (re-implemented by the testbench's Python reader: keep this exact)
 *
 * - One row of `frame_bits` (32) square cells of `frame_cell_px` (8) x 8 pixels, in the
 *   **bottom-left corner**: pixel rows `height - 8` to `height - 1`, pixel columns `0` to `255`.
 * - Cell `i` (0 to 31) covers columns `8 * i` to `8 * i + 7` and holds **bit `i`** of the frame
 *   number: bit 0 (least significant) is the leftmost cell, bit 31 the rightmost.
 * - A 1 bit is a **white** cell (B = G = R = 255), a 0 bit a **black** cell (B = G = R = 0); every
 *   pixel of a cell has that one colour. Alpha is 255.
 * - The number stamped is the low 32 bits of the frame number.
 * - Reading (read_frame_number()): for each cell, average (R + G + B) / 3 over the **centre 4x4
 *   pixels** of the cell (columns `8 * i + 2` to `8 * i + 5`, rows `height - 6` to `height - 3`);
 *   the bit is 1 when that average is >= 128. Averaging the centre, never a single pixel, is what
 *   lets the number survive H.264 compression: the cells are pure black or white, and the cell
 *   edges, where a codec smears into the moving picture next to it, are not read.
 * - Coordinates are in **stream pixels**: a reader of a scaled picture (a client window that is
 *   not the stream size) must scale them first.
 * - An image narrower than 256 or shorter than 8 pixels gets only the cells that fit completely;
 *   a missing cell reads as 0.
 *
 * ## The picture
 *
 * For pixel (x, y) of frame f:
 * - B = (x + 4f) mod 256, G = (y + 2f) mod 256, R = ((x + y) / 2 + 3f) mod 256: gradients that
 *   scroll every frame, with hard wrap edges that move.
 * - A white 64 x 64 square (smaller on a tiny image) bouncing left and right across the middle,
 *   8 pixels per frame.
 * - Then the frame number cells above, drawn last.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace benchmark_scene {
  /// Side of one frame number cell, in pixels.
  inline constexpr int frame_cell_px = 8;

  /// Number of cells, i.e. of frame number bits.
  inline constexpr int frame_bits = 32;

  /// Side of the bouncing square, in pixels (clipped to the image).
  inline constexpr int box_px = 64;

  /// How far the square moves per frame, in pixels.
  inline constexpr int box_step_px = 8;

  namespace detail {
    inline void put(std::uint8_t *row, int x, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
      row[x * 4 + 0] = b;
      row[x * 4 + 1] = g;
      row[x * 4 + 2] = r;
      row[x * 4 + 3] = 255;
    }

    /// Cells that fit completely in the image.
    inline int cells_that_fit(int width, int height) {
      if (height < frame_cell_px || width < frame_cell_px) {
        return 0;
      }
      return std::min(frame_bits, width / frame_cell_px);
    }
  }  // namespace detail

  /**
   * @brief Draw frame `frame` of the synthetic picture, frame number included.
   * @param dst Top-left pixel of a BGRA (B, G, R, A bytes) image.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param row_pitch Bytes from one row to the next (at least width * 4).
   * @param frame The frame number: the only input, the same number always gives the same image.
   */
  inline void render_bgra(std::uint8_t *dst, int width, int height, int row_pitch, std::uint64_t frame) {
    if (!dst || width <= 0 || height <= 0) {
      return;
    }

    // Moduli applied once, so the per-pixel work stays in small unsigned integers
    const auto f = static_cast<std::uint32_t>(frame & 0xFFFF'FFFF);
    const std::uint32_t shift_b = (4u * f) & 0xFF;
    const std::uint32_t shift_g = (2u * f) & 0xFF;
    const std::uint32_t shift_r = (3u * f) & 0xFF;

    for (int y = 0; y < height; ++y) {
      auto *row = reinterpret_cast<std::uint32_t *>(dst + static_cast<std::ptrdiff_t>(y) * row_pitch);
      const std::uint32_t g = (static_cast<std::uint32_t>(y) + shift_g) & 0xFF;
      for (int x = 0; x < width; ++x) {
        const std::uint32_t b = (static_cast<std::uint32_t>(x) + shift_b) & 0xFF;
        const std::uint32_t r = ((static_cast<std::uint32_t>(x + y) >> 1) + shift_r) & 0xFF;
        // Little endian: bytes B, G, R, A
        row[x] = 0xFF00'0000u | (r << 16) | (g << 8) | b;
      }
    }

    // The bouncing square: position is a triangle wave of the frame number
    const int side = std::min({box_px, width, height});
    const int span = width - side;
    int box_x = 0;
    if (span > 0) {
      const auto period = static_cast<std::uint64_t>(span) * 2;
      const auto phase = static_cast<int>((frame * box_step_px) % period);
      box_x = phase <= span ? phase : static_cast<int>(period) - phase;
    }
    const int box_y = (height - side) / 2;
    for (int y = box_y; y < box_y + side; ++y) {
      auto *row = dst + static_cast<std::ptrdiff_t>(y) * row_pitch;
      for (int x = box_x; x < box_x + side; ++x) {
        detail::put(row, x, 255, 255, 255);
      }
    }

    // Frame number, drawn last so nothing covers it
    const int cells = detail::cells_that_fit(width, height);
    for (int y = height - frame_cell_px; y < height && cells > 0; ++y) {
      auto *row = dst + static_cast<std::ptrdiff_t>(y) * row_pitch;
      for (int i = 0; i < cells; ++i) {
        const std::uint8_t v = ((f >> i) & 1u) ? 255 : 0;
        for (int x = i * frame_cell_px; x < (i + 1) * frame_cell_px; ++x) {
          detail::put(row, x, v, v, v);
        }
      }
    }
  }

  /**
   * @brief Read back the frame number render_bgra() stamped (see the layout at the top of this file).
   * @param bgra Top-left pixel of a BGRA image, possibly after lossy compression.
   * @param width Width in pixels.
   * @param height Height in pixels.
   * @param row_pitch Bytes from one row to the next.
   * @return The low 32 bits of the frame number.
   */
  inline std::uint64_t read_frame_number(const std::uint8_t *bgra, int width, int height, int row_pitch) {
    if (!bgra) {
      return 0;
    }

    const int cells = detail::cells_that_fit(width, height);
    std::uint64_t number = 0;
    for (int i = 0; i < cells; ++i) {
      unsigned sum = 0;
      for (int y = height - frame_cell_px + 2; y <= height - frame_cell_px + 5; ++y) {
        const auto *row = bgra + static_cast<std::ptrdiff_t>(y) * row_pitch;
        for (int x = i * frame_cell_px + 2; x <= i * frame_cell_px + 5; ++x) {
          sum += row[x * 4 + 0] + row[x * 4 + 1] + row[x * 4 + 2];
        }
      }
      // 16 pixels x 3 channels: the mean of (R + G + B) / 3 is sum / 48
      if (sum >= 128u * 48u) {
        number |= std::uint64_t {1} << i;
      }
    }
    return number;
  }
}  // namespace benchmark_scene
