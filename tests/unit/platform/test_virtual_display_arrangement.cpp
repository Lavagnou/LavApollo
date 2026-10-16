/**
 * @file tests/unit/platform/test_virtual_display_arrangement.cpp
 * @brief Test how an emulated display layout is judged against where Windows put the displays.
 */
#include "../../tests_common.h"

#ifdef _WIN32
  // virtual_display.h relies on the Windows types being declared first
  #include <windows.h>
  #include <src/platform/windows/virtual_display.h>

namespace {
  // The layout that regressed: a 1080p monitor above a 1920x515 bar display
  const std::vector<VDISPLAY::LayoutEntry> STACKED = {
    {L"\\\\.\\DISPLAY12", 0, 0, 1920, 1080, true},
    {L"\\\\.\\DISPLAY13", 0, 1080, 1920, 515, false},
  };
}  // namespace

TEST(VirtualDisplayArrangementTests, SameArrangementAnywhereOnTheDesktop) {
  EXPECT_TRUE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}, {0, 1080, 1920, 515}}));
  // Restored by Windows past the user's own monitors: the block moved, not its shape
  EXPECT_TRUE(VDISPLAY::arrangementMatches(STACKED, {{2560, -200, 1920, 1080}, {2560, 880, 1920, 515}}));
}

TEST(VirtualDisplayArrangementTests, ToleratesAModeRoundedByAPixel) {
  EXPECT_TRUE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}, {0, 1080, 1920, 516}}));
  EXPECT_FALSE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}, {0, 1080, 1920, 520}}));
}

TEST(VirtualDisplayArrangementTests, RejectsDisplaysWhereWindowsDroppedThem) {
  // Side by side instead of stacked: the picture would no longer match the client's canvas
  EXPECT_FALSE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}, {1920, 0, 1920, 515}}));
  EXPECT_FALSE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}, {0, 1081, 1920, 515}}));
}

TEST(VirtualDisplayArrangementTests, RejectsAMissingDisplay) {
  EXPECT_FALSE(VDISPLAY::arrangementMatches(STACKED, {{0, 0, 1920, 1080}}));
  EXPECT_FALSE(VDISPLAY::arrangementMatches({}, {}));
}
#endif
