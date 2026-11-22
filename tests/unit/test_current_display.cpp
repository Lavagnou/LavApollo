/**
 * @file tests/unit/test_current_display.cpp
 * @brief Test src/current_display.*
 */
#include "../tests_common.h"

#include <src/current_display.h>

using current_display::display_mode_t;

TEST(CurrentDisplay, combineSingle) {
  auto m = current_display::combine({display_mode_t {0, 0, 1920, 1080, 60}});
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->x, 0);
  EXPECT_EQ(m->y, 0);
  EXPECT_EQ(m->width, 1920);
  EXPECT_EQ(m->height, 1080);
  EXPECT_EQ(m->refresh, 60);
}

TEST(CurrentDisplay, combineUsesBoundingBox) {
  // An emulated multi-display canvas: the stream carries the bounding box, at the slowest refresh
  auto m = current_display::combine({
    display_mode_t {0, 0, 1920, 1080, 60},
    display_mode_t {1920, -200, 1080, 1920, 144},
  });
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->x, 0);
  EXPECT_EQ(m->y, -200);
  EXPECT_EQ(m->width, 3000);
  EXPECT_EQ(m->height, 1920);
  EXPECT_EQ(m->refresh, 60);
}

TEST(CurrentDisplay, combineEmpty) {
  EXPECT_FALSE(current_display::combine({}).has_value());
}
