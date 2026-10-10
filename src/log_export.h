/**
 * @file src/log_export.h
 * @brief Support-log sanitization and ZIP encoding for the log export endpoint.
 *
 * Ported from Vibepollo (Nonary, src/log_export.h). The ZIP is written by hand
 * (local headers + central directory, raw deflate via zlib) so no archive
 * library is needed.
 */
#pragma once

#include <algorithm>
#include <array>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/crc.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <zlib.h>

namespace log_export {
  inline void write_le16(std::string &out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
  }

  inline void write_le32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
  }

  inline void to_dos_datetime(std::time_t tt, uint16_t &dos_time, uint16_t &dos_date) {
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    dos_time = static_cast<uint16_t>(((tm.tm_hour & 0x1F) << 11) | ((tm.tm_min & 0x3F) << 5) | ((tm.tm_sec / 2) & 0x1F));
    const int year = std::clamp(tm.tm_year + 1900, 1980, 2107);
    dos_date = static_cast<uint16_t>(((year - 1980) << 9) | (((tm.tm_mon + 1) & 0x0F) << 5) | (tm.tm_mday & 0x1F));
  }

  inline std::time_t file_time_to_time_t(std::filesystem::file_time_type ft) {
    const auto tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    return std::chrono::system_clock::to_time_t(tp);
  }

  /**
   * @brief Raw-deflate a buffer (no zlib header), as ZIP method 8 expects.
   */
  inline bool deflate_buffer(const char *data, std::size_t size, std::string &out) {
    z_stream zs {};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      return false;
    }
    std::array<unsigned char, 16384> buf {};
    out.clear();
    std::size_t offset = 0;
    int ret = Z_OK;
    do {
      if (zs.avail_in == 0 && offset < size) {
        const std::size_t chunk = std::min<std::size_t>(size - offset, std::numeric_limits<uInt>::max());
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data + offset));
        zs.avail_in = static_cast<uInt>(chunk);
        offset += chunk;
      }
      zs.next_out = buf.data();
      zs.avail_out = static_cast<uInt>(buf.size());
      const int flush = (offset >= size && zs.avail_in == 0) ? Z_FINISH : Z_NO_FLUSH;
      ret = deflate(&zs, flush);
      if (ret == Z_STREAM_ERROR) {
        deflateEnd(&zs);
        return false;
      }
      out.append(reinterpret_cast<char *>(buf.data()), buf.size() - zs.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
  }

  struct zip_entry_t {
    std::string name;
    std::string data;
    std::optional<std::filesystem::file_time_type> write_time;
  };

  /**
   * @brief Replaces personal identifiers with stable placeholders.
   *
   * Masks user names (home-directory paths and explicit user fields), IPv4/IPv6
   * addresses, MAC addresses and e-mail addresses. The same value always maps to
   * the same placeholder within one sanitizer, so an exported bundle still shows
   * which lines refer to the same client.
   */
  class sanitizer_t {
  public:
    std::string sanitize(const std::string &input) {
      struct replacement_t {
        std::size_t begin;
        std::size_t end;
        int priority;
        std::string value;
      };

      std::vector<replacement_t> replacements;
      const auto add_matches = [&](const std::regex &pattern, std::size_t capture, int priority, const std::string &kind, bool validate_ipv6 = false, bool preserve_service_identity = false) {
        std::match_results<std::string::const_iterator> match;
        auto search_start = input.cbegin();
        while (std::regex_search(search_start, input.cend(), match, pattern)) {
          if (capture >= match.size() || !match[capture].matched) {
            break;
          }
          const std::string value = match.str(capture);
          const bool skip = (validate_ipv6 && !is_ipv6(value)) ||
                            (preserve_service_identity && is_service_identity(value));
          if (!skip) {
            const auto offset = static_cast<std::size_t>(std::distance(input.cbegin(), search_start));
            replacements.push_back(replacement_t {
              offset + static_cast<std::size_t>(match.position(capture)),
              offset + static_cast<std::size_t>(match.position(capture) + match.length(capture)),
              priority,
              placeholder(kind, value),
            });
          }
          // Resume after the captured value, not the whole match: boundary-based
          // patterns consume punctuation that the next address in a comma- or
          // space-separated list still needs.
          search_start += match.position(capture) + std::max<std::ptrdiff_t>(match.length(capture), 1);
        }
      };

      // Compiled once per process: construction dominates on small files.
      static const std::regex user_home_windows {R"((?:[A-Za-z]:[\\/]+Users[\\/]+)([^\\/,:;"'<>()[\]{}|]+))", std::regex_constants::icase};
      static const std::regex user_home_posix {R"((?:^|[^A-Za-z0-9_])/(?:home|Users)/([^/\s:,;"'<>()[\]{}|]+))", std::regex_constants::icase};
      // Explicit fields are safer to redact than arbitrary words that happen to look like names.
      static const std::regex user_field_quoted {R"(\b(?:username|user_name|user)\b\s*[:=]\s*["']([^"']+)["'])", std::regex_constants::icase};
      static const std::regex user_field_bare {R"(\b(?:username|user_name|user)\b\s*[:=]\s*([A-Za-z0-9._-]+))", std::regex_constants::icase};
      static const std::regex ipv4_pattern {R"((?:^|[^0-9A-Za-z])((?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])(?:\.(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])){3})(?:[^0-9A-Za-z]|$))"};
      static const std::regex ipv6_candidate {R"((?:^|[^0-9A-Za-z:.])([0-9A-Fa-f:.]{2,})(?:[^0-9A-Za-z:.]|$))"};
      static const std::regex mac_pattern {R"(\b([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}|[0-9A-Fa-f]{2}(?:-[0-9A-Fa-f]{2}){5})\b)"};
      static const std::regex email_pattern {R"(\b[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+\b)"};

      add_matches(user_home_windows, 1, 10, "USER");
      add_matches(user_home_posix, 1, 10, "USER");
      add_matches(user_field_quoted, 1, 0, "USER", false, true);
      add_matches(user_field_bare, 1, 0, "USER", false, true);
      add_matches(ipv4_pattern, 1, 20, "IP");
      add_matches(ipv6_candidate, 1, 20, "IP", true);
      add_matches(mac_pattern, 1, 30, "MAC");
      add_matches(email_pattern, 0, 40, "EMAIL");

      // Overlapping matches: the earliest, then the longest, then the lowest priority wins.
      std::sort(replacements.begin(), replacements.end(), [](const auto &a, const auto &b) {
        if (a.begin != b.begin) {
          return a.begin < b.begin;
        }
        if (a.end != b.end) {
          return a.end > b.end;
        }
        return a.priority < b.priority;
      });

      std::string output;
      output.reserve(input.size());
      std::size_t cursor = 0;
      for (const auto &replacement : replacements) {
        if (replacement.begin < cursor || replacement.end > input.size()) {
          continue;
        }
        output.append(input, cursor, replacement.begin - cursor);
        output.append(replacement.value);
        cursor = replacement.end;
      }
      output.append(input, cursor, std::string::npos);
      return output;
    }

  private:
    static bool is_service_identity(const std::string &value) {
      std::string normalized;
      for (const unsigned char ch : value) {
        if (!std::isspace(ch)) {
          normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
      }
      return normalized == "system" || normalized == "localsystem" || normalized == "root" || normalized == "daemon" ||
             normalized == "unknown" || normalized == "nobody" || normalized == "networkservice" || normalized == "localservice" ||
             normalized == "www-data";
    }

    static bool is_ipv6(const std::string &value) {
      // The candidate pattern matches every timestamp and hex token in a log;
      // reject what cannot be IPv6 text (needs "::" or the 6-7 colons of an
      // uncompressed address) before paying for a real parse.
      const auto colons = std::count(value.begin(), value.end(), ':');
      if (colons < 6 && value.find("::") == std::string::npos) {
        return false;
      }
      boost::system::error_code ec;
      boost::asio::ip::make_address_v6(value, ec);
      return !ec;
    }

    std::string placeholder(const std::string &kind, const std::string &value) {
      std::string key = kind + ":" + value;
      if (kind != "USER") {
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
          return static_cast<char>(std::tolower(ch));
        });
      }
      const auto it = placeholders.find(key);
      if (it != placeholders.end()) {
        return it->second;
      }
      std::string result = "<" + kind + "_" + std::to_string(next_id++) + ">";
      placeholders.emplace(std::move(key), result);
      return result;
    }

    std::unordered_map<std::string, std::string> placeholders;
    std::size_t next_id = 1;
  };

  /**
   * @brief Builds a ZIP archive in memory. Each entry is deflated when that makes it smaller.
   */
  inline std::string build_zip(const std::vector<zip_entry_t> &entries) {
    struct central_entry_t {
      std::string name;
      uint32_t crc;
      uint32_t comp_size;
      uint32_t uncomp_size;
      uint16_t method;
      uint32_t offset;
      uint16_t dos_time;
      uint16_t dos_date;
    };

    std::string out;
    std::vector<central_entry_t> central;
    for (const auto &e : entries) {
      boost::crc_32_type crc;
      crc.process_bytes(e.data.data(), e.data.size());
      std::string compressed;
      const bool use_deflate = deflate_buffer(e.data.data(), e.data.size(), compressed) && compressed.size() < e.data.size();
      const std::string &payload = use_deflate ? compressed : e.data;

      central_entry_t c {e.name, crc.checksum(), static_cast<uint32_t>(payload.size()), static_cast<uint32_t>(e.data.size()), static_cast<uint16_t>(use_deflate ? 8 : 0), static_cast<uint32_t>(out.size()), 0, 0};
      to_dos_datetime(e.write_time ? file_time_to_time_t(*e.write_time) : std::time(nullptr), c.dos_time, c.dos_date);

      write_le32(out, 0x04034b50u);  // local file header
      write_le16(out, 20);  // version needed
      write_le16(out, 0);  // flags
      write_le16(out, c.method);
      write_le16(out, c.dos_time);
      write_le16(out, c.dos_date);
      write_le32(out, c.crc);
      write_le32(out, c.comp_size);
      write_le32(out, c.uncomp_size);
      write_le16(out, static_cast<uint16_t>(c.name.size()));
      write_le16(out, 0);  // extra length
      out.append(c.name);
      out.append(payload);
      central.push_back(std::move(c));
    }

    const auto cd_start = static_cast<uint32_t>(out.size());
    for (const auto &c : central) {
      write_le32(out, 0x02014b50u);  // central directory header
      write_le16(out, 20);  // version made by
      write_le16(out, 20);  // version needed
      write_le16(out, 0);  // flags
      write_le16(out, c.method);
      write_le16(out, c.dos_time);
      write_le16(out, c.dos_date);
      write_le32(out, c.crc);
      write_le32(out, c.comp_size);
      write_le32(out, c.uncomp_size);
      write_le16(out, static_cast<uint16_t>(c.name.size()));
      write_le16(out, 0);  // extra length
      write_le16(out, 0);  // comment length
      write_le16(out, 0);  // disk number
      write_le16(out, 0);  // internal attributes
      write_le32(out, 0);  // external attributes
      write_le32(out, c.offset);
      out.append(c.name);
    }
    const auto cd_size = static_cast<uint32_t>(out.size()) - cd_start;

    write_le32(out, 0x06054b50u);  // end of central directory
    write_le16(out, 0);
    write_le16(out, 0);
    write_le16(out, static_cast<uint16_t>(central.size()));
    write_le16(out, static_cast<uint16_t>(central.size()));
    write_le32(out, cd_size);
    write_le32(out, cd_start);
    write_le16(out, 0);
    return out;
  }
}  // namespace log_export
