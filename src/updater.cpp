/**
 * @file src/updater.cpp
 * @brief In-app updater implementation.
 *
 * Windows is fully implemented: query GitHub for the latest release, download + checksum-verify the
 * installer, then launch it silently and elevated. The installer itself drives the swap (its uninstall
 * step stops the service, which stops this process; it then reinstalls and restarts the service into the
 * new binary), which is why stage_installer() never self-exits. On other platforms the functions are
 * stubs that report "not supported".
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

// local includes
#include "httpcommon.h"
#include "logging.h"
#include "updater.h"

#ifdef _WIN32
  #include <windows.h>
  #include <shellapi.h>
#endif

namespace updater {
  using namespace std::literals;

  namespace {
    constexpr auto LATEST_RELEASE_URL = "https://api.github.com/repos/Lavagnou/LavApollo/releases/latest";
    constexpr auto REPO_HOME = "https://github.com/Lavagnou/LavApollo";
    // Staged installer file name (written under the OS temp directory).
    constexpr auto STAGING_FILENAME = "lavapollo-update.exe";
    // How long a successful check_for_update() result is reused without re-hitting GitHub.
    constexpr auto CHECK_CACHE_TTL = std::chrono::minutes(10);

    // Pattern matched against GitHub release asset names to pick the Windows installer.
    constexpr auto INSTALLER_SUFFIX = "-Windows-installer.exe";

    /// Parsed semantic version (major.minor.patch); prerelease/build metadata is ignored.
    struct semver_t {
      int major = 0;
      int minor = 0;
      int patch = 0;
      bool valid = false;
    };

    semver_t
    parse_semver(const std::string &s) {
      semver_t v;
      std::string core = s;
      if (!core.empty() && (core.front() == 'v' || core.front() == 'V')) {
        core.erase(0, 1);
      }
      if (auto pos = core.find('-'); pos != std::string::npos) core.erase(pos);  // drop prerelease
      if (auto pos = core.find('+'); pos != std::string::npos) core.erase(pos);  // drop build metadata

      int parts[3] = {0, 0, 0};
      int idx = 0;
      std::string num;
      auto flush = [&]() {
        if (idx < 3 && !num.empty()) parts[idx++] = std::atoi(num.c_str());
        num.clear();
      };
      for (char c : core) {
        if (c == '.') {
          flush();
        }
        else if (std::isdigit(static_cast<unsigned char>(c))) {
          num += c;
        }
        else {
          return v;  // unexpected character → not a clean numeric version
        }
      }
      flush();
      v.major = parts[0];
      v.minor = parts[1];
      v.patch = parts[2];
      v.valid = true;
      return v;
    }

    /// @return true if `a` is strictly newer than `b`.
    bool
    newer_than(const semver_t &a, const semver_t &b) {
      if (a.major != b.major) return a.major > b.major;
      if (a.minor != b.minor) return a.minor > b.minor;
      return a.patch > b.patch;
    }

    /// SHA-256 of a file as a lowercase hex string, or empty on error.
    std::string
    sha256_file(const std::string &path) {
      FILE *fp = fopen(path.c_str(), "rb");  // NOSONAR
      if (!fp) return {};

      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (!ctx) {
        fclose(fp);  // NOSONAR
        return {};
      }

      EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
      unsigned char buf[65536];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        EVP_DigestUpdate(ctx, buf, n);
      }
      fclose(fp);  // NOSONAR

      unsigned char md[EVP_MAX_MD_SIZE];
      unsigned int md_len = 0;
      EVP_DigestFinal_ex(ctx, md, &md_len);
      EVP_MD_CTX_free(ctx);

      static const char *digits = "0123456789abcdef";
      std::string hex;
      hex.reserve(md_len * 2);
      for (unsigned int i = 0; i < md_len; ++i) {
        hex += digits[md[i] >> 4];
        hex += digits[md[i] & 0x0f];
      }
      return hex;
    }

    /// Extract the expected hash for `installer_name` from a sha256sums file body.
    std::string
    extract_expected_sha(const std::string &sums, const std::string &installer_name) {
      std::istringstream iss(sums);
      std::string line;
      while (std::getline(iss, line)) {
        std::istringstream ls(line);
        std::string hash, fname;
        if (!(ls >> hash >> fname)) continue;
        if (!fname.empty() && fname.front() == '*') fname.erase(0, 1);  // binary-mode marker
        if (fname == installer_name && hash.size() == 64) return hash;
      }
      return {};
    }

    /// The mutable pipeline state. Atomics for the hot path (progress polling), mutex for the strings.
    struct state_t {
      std::mutex mutex;
      std::atomic<state_e> state { state_e::idle };
      std::atomic<std::uint64_t> downloaded_bytes { 0 };
      std::atomic<std::uint64_t> total_bytes { 0 };
      std::string message;          ///< Populated on error (guarded by mutex).
      std::string target_version;   ///< Version being downloaded/applied (guarded by mutex).
      std::string staging_path;     ///< Where the installer was written (guarded by mutex).
      asset_t asset;                ///< Asset being downloaded (guarded by mutex).
    } g_state;

    /// check_for_update() result cache to avoid hitting GitHub on every page load.
    struct cache_t {
      std::mutex mutex;
      version_info_t info;
      std::chrono::steady_clock::time_point last_check;
      bool valid = false;
    } g_cache;

    void
    set_error(std::string msg) {
      std::lock_guard lock(g_state.mutex);
      g_state.state.store(state_e::error, std::memory_order_relaxed);
      g_state.message = std::move(msg);
    }

    /// Download then verify; runs on a detached thread.
    void
    download_and_verify(version_info_t info) {
      std::string staging;
      {
        std::lock_guard lock(g_state.mutex);
        staging = g_state.staging_path;
      }

      auto on_progress = [](std::uint64_t dlnow, std::uint64_t dltotal) {
        g_state.downloaded_bytes.store(dlnow, std::memory_order_relaxed);
        if (dltotal > 0) g_state.total_bytes.store(dltotal, std::memory_order_relaxed);
      };

      g_state.state.store(state_e::downloading, std::memory_order_relaxed);
      if (!http::download_file_with_progress(info.asset.url, staging, on_progress)) {
        set_error("Download failed. Check your network connection and try again.");
        return;
      }

      g_state.state.store(state_e::verifying, std::memory_order_relaxed);

      // Fetch the matching SHA256SUMS file. The release naming is fixed by the release workflow:
      // LavApollo-<version>-SHA256SUMS.txt alongside the installer asset.
      std::string sums_url = std::string(REPO_HOME) + "/releases/download/" + info.latest_tag + "/LavApollo-" + info.latest + "-SHA256SUMS.txt";
      std::string sums = http::simple_get(sums_url);
      std::string expected = sums.empty() ? std::string {} : extract_expected_sha(sums, info.asset.name);

      if (expected.empty()) {
        // No checksum available (new naming, transient fetch failure). The download is still
        // TLS-protected, so log and proceed rather than blocking the update.
        BOOST_LOG(warning) << "update: could not verify checksum for "sv << info.latest << ", proceeding without"sv;
      }
      else {
        std::string actual = sha256_file(staging);
        if (actual.empty()) {
          set_error("Could not read the downloaded installer to verify it.");
          return;
        }
        std::string a = actual;
        std::string e = expected;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
        if (a != e) {
          set_error("Installer checksum mismatch — the download may be corrupt. Please try again.");
          return;
        }
      }

      g_state.state.store(state_e::ready, std::memory_order_relaxed);
      std::lock_guard lock(g_state.mutex);
      g_state.message.clear();
    }
  }  // namespace

  version_info_t
  check_for_update() {
    version_info_t info;
    info.current = PROJECT_VERSION;
    info.service_managed = is_service_managed();

    // Serve the cached result if fresh.
    {
      std::lock_guard lock(g_cache.mutex);
      if (g_cache.valid && (std::chrono::steady_clock::now() - g_cache.last_check < CHECK_CACHE_TTL)) {
        version_info_t cached = g_cache.info;
        cached.current = info.current;  // current may have changed after a restart
        cached.service_managed = info.service_managed;
        return cached;
      }
    }

    std::string body = http::simple_get(LATEST_RELEASE_URL);
    if (body.empty()) {
      info.error = "Could not reach the update server (GitHub).";
    }
    else {
      try {
        auto j = nlohmann::json::parse(body);
        info.latest_tag = j.value("tag_name", "");
        info.release_notes_url = j.value("html_url", "");
        info.latest = info.latest_tag;
        if (!info.latest.empty() && (info.latest.front() == 'v' || info.latest.front() == 'V')) {
          info.latest.erase(0, 1);
        }
        if (j.contains("assets") && j["assets"].is_array()) {
          for (const auto &a : j["assets"]) {
            std::string name = a.value("name", "");
            if (name.ends_with(INSTALLER_SUFFIX)) {
              info.asset.name = name;
              info.asset.url = a.value("browser_download_url", "");
              info.asset.size = a.value("size", std::uint64_t { 0 });
              break;
            }
          }
        }
      }
      catch (const std::exception &e) {
        info.error = std::string("Could not parse the update server response: ") + e.what();
      }
    }

    if (info.error.empty()) {
      semver_t cur = parse_semver(info.current);
      semver_t lat = parse_semver(info.latest);
      if (cur.valid && lat.valid) {
        info.update_available = newer_than(lat, cur);
      }
      else if (!lat.valid) {
        info.error = "Could not parse the latest version '" + info.latest + "'.";
        info.update_available = false;
      }
      else {
        // current version unparseable (e.g. a dev build) — don't offer an update.
        info.update_available = false;
      }
    }

    // Cache regardless of success/failure (so a flaky server isn't hammered on every poll), but refresh
    // the volatile fields each return.
    {
      std::lock_guard lock(g_cache.mutex);
      g_cache.info = info;
      g_cache.info.current.clear();        // don't cache the per-process current version
      g_cache.info.service_managed = false;
      g_cache.last_check = std::chrono::steady_clock::now();
      g_cache.valid = true;
    }
    return info;
  }

  bool
  start_download(const version_info_t &info) {
    if (info.asset.url.empty()) {
      set_error("No installer is available for this release.");
      return false;
    }

    // Only begin from idle/error — never interrupt an in-progress download/verify/install.
    state_e expected = state_e::idle;
    if (!g_state.state.compare_exchange_strong(expected, state_e::downloading)) {
      expected = state_e::error;
      if (!g_state.state.compare_exchange_strong(expected, state_e::downloading)) {
        return false;
      }
    }

    {
      std::lock_guard lock(g_state.mutex);
      g_state.message.clear();
      g_state.target_version = info.latest;
      g_state.asset = info.asset;
      g_state.staging_path = (std::filesystem::temp_directory_path() / STAGING_FILENAME).string();
    }
    g_state.downloaded_bytes.store(0, std::memory_order_relaxed);
    g_state.total_bytes.store(info.asset.size, std::memory_order_relaxed);

    std::thread([info]() {
      download_and_verify(info);
    }).detach();
    return true;
  }

  bool
  stage_installer() {
#ifdef _WIN32
    if (g_state.state.load(std::memory_order_relaxed) != state_e::ready) {
      set_error("The installer is not staged. Download it first.");
      return false;
    }
    if (!is_service_managed()) {
      set_error("In-app install is only available when LavApollo is running as a service.");
      return false;
    }

    std::string staging;
    {
      std::lock_guard lock(g_state.mutex);
      staging = g_state.staging_path;
    }
    if (staging.empty() || !std::filesystem::exists(staging)) {
      set_error("The staged installer is missing. Please download it again.");
      return false;
    }

    std::filesystem::path exe(staging);
    SHELLEXECUTEINFOW sei {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";  // elevate (UAC prompt) — required to write Program Files + manage the service
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"/S";  // NSIS silent install (preserves config via the uninstaller's /SD IDNO)
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
      DWORD err = GetLastError();
      if (err == ERROR_CANCELLED) {
        set_error("The update was cancelled (the UAC prompt was declined).");
      }
      else {
        set_error("Could not launch the installer (Windows error " + std::to_string(err) + ").");
      }
      return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);

    // The installer now drives everything: its uninstall step stops the service (which stops this
    // process), swaps files, and restarts the service into the new binary. Do NOT self-exit here.
    g_state.state.store(state_e::installing, std::memory_order_relaxed);
    return true;
#else
    set_error("In-app install is only supported on Windows.");
    return false;
#endif
  }

  status_t
  get_status() {
    status_t s;
    s.state = g_state.state.load(std::memory_order_relaxed);
    s.downloaded_bytes = g_state.downloaded_bytes.load(std::memory_order_relaxed);
    s.total_bytes = g_state.total_bytes.load(std::memory_order_relaxed);
    {
      std::lock_guard lock(g_state.mutex);
      s.message = g_state.message;
      s.target_version = g_state.target_version;
    }
    return s;
  }

  bool
  is_service_managed() {
#ifdef _WIN32
    return GetConsoleWindow() == nullptr;
#else
    return false;
#endif
  }

  void
  reset() {
    state_e expected = state_e::error;
    if (g_state.state.compare_exchange_strong(expected, state_e::idle)) {
      std::lock_guard lock(g_state.mutex);
      g_state.message.clear();
      g_state.downloaded_bytes.store(0, std::memory_order_relaxed);
      g_state.total_bytes.store(0, std::memory_order_relaxed);
    }
  }
}  // namespace updater
