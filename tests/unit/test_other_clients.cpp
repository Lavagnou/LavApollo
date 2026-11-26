/**
 * @file tests/unit/test_other_clients.cpp
 * @brief Test serverinfo's OtherClientsConnected (src/nvhttp.cpp) and the test host's fake sessions.
 */
#include "../tests_common.h"

#include <src/nvhttp.h>
#include <src/test_host.h>

namespace {
  const std::string ME = "11111111-1111-1111-1111-111111111111";
  const std::string PHONE = "22222222-2222-2222-2222-222222222222";
  const std::string TV = "33333333-3333-3333-3333-333333333333";
}  // namespace

TEST(OtherClients, NoSessionIsNoOtherClient) {
  EXPECT_EQ(nvhttp::count_other_clients({}, ME), 0);
}

TEST(OtherClients, TheAskersOwnSessionDoesNotCount) {
  // A client that polls while it streams must still say « Reprendre »
  EXPECT_EQ(nvhttp::count_other_clients({ME}, ME), 0);
}

TEST(OtherClients, EachOtherDeviceCounts) {
  EXPECT_EQ(nvhttp::count_other_clients({PHONE}, ME), 1);
  EXPECT_EQ(nvhttp::count_other_clients({ME, PHONE, TV}, ME), 2);
}

TEST(OtherClients, ASessionWithoutUuidIsSomeoneElse) {
  EXPECT_EQ(nvhttp::count_other_clients({""}, ME), 1);
  EXPECT_EQ(nvhttp::count_other_clients({""}, ""), 1);
}

TEST(OtherClients, TestHostFakeSessionsAddUp) {
  EXPECT_EQ(nvhttp::count_other_clients({}, ME, 1), 1);
  EXPECT_EQ(nvhttp::count_other_clients({PHONE}, ME, 2), 3);
  EXPECT_EQ(nvhttp::count_other_clients({}, ME, -3), 0);
}

TEST(OtherClients, TestHostSessionStateIsClampedAndReset) {
  test_host::set_session(2, 2520);
  EXPECT_EQ(test_host::other_clients(), 2);
  EXPECT_EQ(test_host::elapsed_offset(), 2520);
  test_host::set_session(-1, -5);
  EXPECT_EQ(test_host::other_clients(), 0);
  EXPECT_EQ(test_host::elapsed_offset(), 0);
}
