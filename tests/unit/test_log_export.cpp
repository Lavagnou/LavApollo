/**
 * @file tests/unit/test_log_export.cpp
 * @brief Tests for the support-log sanitizer and ZIP writer.
 */
#include "../tests_common.h"

#include "src/log_export.h"

#include <cstring>

namespace {
  uint32_t read_le32(const std::string &s, std::size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(s[at])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24);
  }

  uint16_t read_le16(const std::string &s, std::size_t at) {
    return static_cast<uint16_t>(static_cast<unsigned char>(s[at]) | (static_cast<unsigned char>(s[at + 1]) << 8));
  }
}  // namespace

TEST(LogExportSanitizer, MasksPersonalIdentifiers) {
  log_export::sanitizer_t sanitizer;
  const std::string out = sanitizer.sanitize(
    "Client 192.168.1.42 connected from C:\\Users\\Alice\\AppData\n"
    "mac aa:bb:cc:dd:ee:ff mail alice@example.com v6 fe80::1:2:3:4\n"
  );
  EXPECT_EQ(out.find("192.168.1.42"), std::string::npos);
  EXPECT_EQ(out.find("Alice"), std::string::npos);
  EXPECT_EQ(out.find("aa:bb:cc:dd:ee:ff"), std::string::npos);
  EXPECT_EQ(out.find("alice@example.com"), std::string::npos);
  EXPECT_EQ(out.find("fe80::1:2:3:4"), std::string::npos);
  EXPECT_NE(out.find("<IP_"), std::string::npos);
  EXPECT_NE(out.find("<USER_"), std::string::npos);
  // The rest of the line survives
  EXPECT_NE(out.find("connected from C:\\Users\\"), std::string::npos);
}

TEST(LogExportSanitizer, SameValueSamePlaceholder) {
  log_export::sanitizer_t sanitizer;
  const std::string a = sanitizer.sanitize("from 10.0.0.5");
  const std::string b = sanitizer.sanitize("again 10.0.0.5, then 10.0.0.6");
  EXPECT_EQ(a, "from <IP_1>");
  EXPECT_EQ(b, "again <IP_1>, then <IP_2>");
}

TEST(LogExportSanitizer, KeepsTimestampsAndServiceAccounts) {
  log_export::sanitizer_t sanitizer;
  const std::string line = "[2026:10:11:12:30:01.123]: Info: user=SYSTEM version 1.2.3.4";
  const std::string out = sanitizer.sanitize(line);
  EXPECT_NE(out.find("[2026:10:11:12:30:01.123]"), std::string::npos);
  EXPECT_NE(out.find("user=SYSTEM"), std::string::npos);
}

TEST(LogExportZip, WritesReadableArchive) {
  const std::string text(4096, 'a');  // compressible: stored deflated
  const std::vector<log_export::zip_entry_t> entries {
    {"sunshine.log", text, std::nullopt},
    {"x", "y", std::nullopt},  // incompressible: stored as-is
  };
  const std::string zip = log_export::build_zip(entries);

  ASSERT_GE(zip.size(), 22u);
  const std::size_t eocd = zip.size() - 22;
  EXPECT_EQ(read_le32(zip, eocd), 0x06054b50u);
  EXPECT_EQ(read_le16(zip, eocd + 10), 2);  // entries
  const uint32_t cd_size = read_le32(zip, eocd + 12);
  const uint32_t cd_start = read_le32(zip, eocd + 16);
  EXPECT_EQ(cd_start + cd_size, eocd);

  // First local header: deflated, with the original size and name
  EXPECT_EQ(read_le32(zip, 0), 0x04034b50u);
  EXPECT_EQ(read_le16(zip, 8), 8);
  EXPECT_EQ(read_le32(zip, 22), text.size());
  EXPECT_EQ(zip.substr(30, 12), "sunshine.log");

  // Second entry, located through the central directory, is stored
  const std::size_t second_cd = cd_start + 46 + 12;
  EXPECT_EQ(read_le32(zip, second_cd), 0x02014b50u);
  const uint32_t second_local = read_le32(zip, second_cd + 42);
  EXPECT_EQ(read_le16(zip, second_local + 8), 0);
  EXPECT_EQ(zip.substr(second_local + 30, 2), "xy");
}
