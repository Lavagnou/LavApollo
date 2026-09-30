/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include "../tests_common.h"

#include <src/network.h>

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Sunshine"),
    std::make_tuple("", "Sunshine"),
    std::make_tuple("😁", "Sunshine"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

TEST(StreamingBindTests, SucceedsOnceEveryServerBound) {
  net::expect_streaming_binds(2);
  net::report_streaming_bind(true);
  net::report_streaming_bind(true);
  EXPECT_TRUE(net::wait_for_streaming_binds(std::chrono::milliseconds(0), nullptr));
}

TEST(StreamingBindTests, FailsAsSoonAsOneServerFails) {
  net::expect_streaming_binds(3);
  net::report_streaming_bind(true);
  net::report_streaming_bind(false);
  EXPECT_FALSE(net::wait_for_streaming_binds(std::chrono::seconds(5), nullptr));
}

TEST(StreamingBindTests, TimesOutWhenAServerNeverReports) {
  net::expect_streaming_binds(2);
  net::report_streaming_bind(true);
  EXPECT_FALSE(net::wait_for_streaming_binds(std::chrono::milliseconds(50), nullptr));
}

TEST(StreamingBindTests, AbortStopsTheWait) {
  net::expect_streaming_binds(1);
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(net::wait_for_streaming_binds(std::chrono::seconds(30), [] {
    return true;
  }));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(1));
}
