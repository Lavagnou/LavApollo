/**
 * @file tests/unit/test_updater.cpp
 * @brief Test the pure helpers of src/updater.*
 */
#include "../tests_common.h"

#include <src/updater.h>

using namespace updater::detail;

TEST(UpdaterVersionTests, ParsesPlainAndPrefixedVersions) {
  auto v = parse_semver("1.2.3");
  EXPECT_TRUE(v.valid);
  EXPECT_EQ(v.major, 1);
  EXPECT_EQ(v.minor, 2);
  EXPECT_EQ(v.patch, 3);

  auto t = parse_semver("v10.0.7");
  EXPECT_TRUE(t.valid);
  EXPECT_EQ(t.major, 10);
  EXPECT_EQ(t.patch, 7);
}

TEST(UpdaterVersionTests, IgnoresPrereleaseAndBuildMetadata) {
  auto v = parse_semver("1.1.5-ci.42+abc");
  EXPECT_TRUE(v.valid);
  EXPECT_EQ(v.patch, 5);
}

TEST(UpdaterVersionTests, RejectsNonVersions) {
  EXPECT_FALSE(parse_semver("").valid);
  EXPECT_FALSE(parse_semver("v").valid);
  EXPECT_FALSE(parse_semver("latest").valid);
  EXPECT_FALSE(parse_semver("1.2.x").valid);
}

TEST(UpdaterVersionTests, ComparesComponentByComponent) {
  EXPECT_TRUE(newer_than(parse_semver("1.1.6"), parse_semver("1.1.5")));
  EXPECT_TRUE(newer_than(parse_semver("1.2.0"), parse_semver("1.1.9")));
  EXPECT_TRUE(newer_than(parse_semver("2.0.0"), parse_semver("1.99.99")));
  EXPECT_FALSE(newer_than(parse_semver("1.1.5"), parse_semver("1.1.5")));
  EXPECT_FALSE(newer_than(parse_semver("1.1.4"), parse_semver("1.1.5")));
  // Numeric, not lexicographic
  EXPECT_TRUE(newer_than(parse_semver("1.10.0"), parse_semver("1.9.0")));
}

TEST(UpdaterChecksumTests, FindsTheInstallerLine) {
  const std::string hash(64, 'a');
  const std::string other(64, 'b');
  const std::string sums = other + "  LavApollo-1.1.6-Windows-portable.zip\n" +
                           hash + " *LavApollo-1.1.6-Windows-installer.exe\n";
  EXPECT_EQ(extract_expected_sha(sums, "LavApollo-1.1.6-Windows-installer.exe"), hash);
  EXPECT_EQ(extract_expected_sha(sums, "LavApollo-1.1.6-Windows-portable.zip"), other);
}

TEST(UpdaterChecksumTests, RejectsMissingOrMalformedEntries) {
  EXPECT_TRUE(extract_expected_sha("", "x.exe").empty());
  EXPECT_TRUE(extract_expected_sha(std::string(64, 'a') + "  y.exe\n", "x.exe").empty());
  // Wrong length
  EXPECT_TRUE(extract_expected_sha(std::string(63, 'a') + "  x.exe\n", "x.exe").empty());
  // Not hex
  EXPECT_TRUE(extract_expected_sha(std::string(64, 'z') + "  x.exe\n", "x.exe").empty());
}
