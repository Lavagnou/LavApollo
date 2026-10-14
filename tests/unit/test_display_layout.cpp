/**
 * @file tests/unit/test_display_layout.cpp
 * @brief Test the `displayLayout=` launch argument parser in src/nvhttp.cpp.
 */
#include "../tests_common.h"

#include <src/nvhttp.h>

namespace {
  // A 3840x1080 canvas made of two side-by-side 1920x1080 displays, the first primary.
  constexpr int CANVAS_W = 3840;
  constexpr int CANVAS_H = 1080;
  const std::string TWO_SIDE_BY_SIDE = "0,0,1920,1080,1;1920,0,1920,1080,0";
}  // namespace

TEST(DisplayLayoutTests, AcceptsValidLayout) {
  auto displays = nvhttp::parse_display_layout(TWO_SIDE_BY_SIDE, CANVAS_W, CANVAS_H);
  ASSERT_EQ(displays.size(), 2u);
  EXPECT_EQ(displays[0].x, 0);
  EXPECT_EQ(displays[0].width, 1920);
  EXPECT_TRUE(displays[0].primary);
  EXPECT_EQ(displays[1].x, 1920);
  EXPECT_FALSE(displays[1].primary);
}

TEST(DisplayLayoutTests, ToleratesTrailingSeparatorAndOddCanvasPixel) {
  EXPECT_EQ(nvhttp::parse_display_layout(TWO_SIDE_BY_SIDE + ";", CANVAS_W + 1, CANVAS_H).size(), 2u);
}

TEST(DisplayLayoutTests, EmptyMeansNoLayout) {
  EXPECT_TRUE(nvhttp::parse_display_layout("", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsHugeValuesThatWouldOverflow) {
  // x + width used to overflow int and slip past the canvas bounds check.
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;2147483000,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,99999999999,1080,0", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,16385,1080,0", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsMalformedFields) {
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920abc,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1; 1920,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;-5,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,,1920,1080,0", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsWrongFieldCount) {
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,1920,1080", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,1920,1080,0,7", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsGeometryErrors) {
  // Overlap
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1900,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
  // Outside the canvas
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,1921,1080,0", CANVAS_W, CANVAS_H).empty());
  // Does not fill the canvas
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,1800,1080,0", CANVAS_W, CANVAS_H).empty());
  // Zero size
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,0,1080,0", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsPrimaryCountOtherThanOne) {
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,1;1920,0,1920,1080,1", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,1920,1080,0;1920,0,1920,1080,0", CANVAS_W, CANVAS_H).empty());
}

TEST(DisplayLayoutTests, RejectsSingleOrTooManyDisplays) {
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,3840,1080,1", CANVAS_W, CANVAS_H).empty());
  EXPECT_TRUE(nvhttp::parse_display_layout("0,0,768,1080,1;768,0,768,1080,0;1536,0,768,1080,0;2304,0,768,1080,0;3072,0,768,1080,0", CANVAS_W, CANVAS_H).empty());
}
