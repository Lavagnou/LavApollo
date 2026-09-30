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
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

// local includes
#include "httpcommon.h"
#include "logging.h"
#include "platform/common.h"
#include "updater.h"
#include "utility.h"

#ifdef _WIN32
  // clang-format off
  #include <windows.h>
  #include <sddl.h>
  #include <shellapi.h>
  #include <shlobj.h>
  #include <softpub.h>
  #include <wincrypt.h>
  #include <wintrust.h>
  // clang-format on
#endif

namespace updater {
  using namespace std::literals;
  using namespace detail;

  namespace {
    constexpr auto LATEST_RELEASE_URL = "https://api.github.com/repos/Lavagnou/LavApollo/releases/latest";
    constexpr auto REPO_HOME = "https://github.com/Lavagnou/LavApollo";
    // Staged installers are named "<prefix><random>.exe" inside staging_directory().
    constexpr auto STAGING_PREFIX = "lavapollo-update-";
    // Where releases before the hardening staged their installer: a fixed name in the service's
    // %TEMP%, which every local user can write to. Removed on startup.
    constexpr auto LEGACY_STAGING_FILENAME = "lavapollo-update.exe";

    /**
     * SHA-1 thumbprint (uppercase hex, no spaces) of the certificate LavApollo installers are
     * Authenticode-signed with. Empty while releases are unsigned: verify_publisher() then accepts
     * any file and the SHA-256 from the release's SHA256SUMS is the only integrity check.
     *
     * Fill it in the day a code-signing certificate exists (and signtool runs in release.yml), and
     * the installer will refuse to launch anything not signed by that exact certificate.
     */
    constexpr std::string_view PUBLISHER_THUMBPRINT = "";
    // How long a successful check_for_update() result is reused without re-hitting GitHub.
    constexpr auto CHECK_CACHE_TTL = std::chrono::minutes(10);

    // Pattern matched against GitHub release asset names to pick the Windows installer.
    constexpr auto INSTALLER_SUFFIX = "-Windows-installer.exe";

  }  // namespace

  namespace detail {
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
      bool any_digit = false;
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
          any_digit = true;
        }
        else {
          return v;  // unexpected character → not a clean numeric version
        }
      }
      flush();
      v.major = parts[0];
      v.minor = parts[1];
      v.patch = parts[2];
      // An empty tag (a release object without tag_name) is not version 0.0.0.
      v.valid = any_digit;
      return v;
    }

    /// @return true if `a` is strictly newer than `b`.
    bool
    newer_than(const semver_t &a, const semver_t &b) {
      if (a.major != b.major) return a.major > b.major;
      if (a.minor != b.minor) return a.minor > b.minor;
      return a.patch > b.patch;
    }
  }  // namespace detail

  namespace {

    /**
     * SHA-256 of everything `read_chunk` yields, as a lowercase hex string, or empty on error.
     * `read_chunk(buf, size)` returns the bytes read, 0 at end of input, or -1 on error.
     */
    template<class F>
    std::string
    sha256_stream(F &&read_chunk) {
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (!ctx) {
        return {};
      }

      EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
      static thread_local unsigned char buf[65536];
      while (true) {
        auto n = read_chunk(buf, sizeof(buf));
        if (n < 0) {
          EVP_MD_CTX_free(ctx);
          return {};
        }
        if (n == 0) {
          break;
        }
        EVP_DigestUpdate(ctx, buf, (size_t) n);
      }

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

    /// SHA-256 of a file as a lowercase hex string, or empty on error.
    std::string
    sha256_file(const std::string &path) {
      FILE *fp = fopen(path.c_str(), "rb");  // NOSONAR
      if (!fp) return {};

      auto hex = sha256_stream([fp](unsigned char *buf, size_t size) -> long long {
        auto n = fread(buf, 1, size, fp);
        if (n == 0 && ferror(fp)) return -1;
        return (long long) n;
      });
      fclose(fp);  // NOSONAR
      return hex;
    }

    bool
    equal_hex(std::string a, std::string b) {
      auto lower = [](std::string &s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
      };
      lower(a);
      lower(b);
      return !a.empty() && a == b;
    }

    /**
     * Directory the installer is downloaded into, created if needed.
     *
     * On Windows this is an `updates` folder next to the executable -- under Program Files for
     * any real install, which ordinary users cannot write to -- created with a protected DACL
     * granting full control to SYSTEM and Administrators only. It used to be the service's
     * %TEMP% (C:\Windows\Temp), where any local user can create files: they could pre-create
     * the staged installer, wait for it to be verified, then swap it for their own program and
     * have the admin launch it elevated.
     *
     * An existing directory is removed and created again, so the DACL in force is always the
     * one set here, never one left behind by whoever made the folder first.
     */
    std::filesystem::path
    staging_directory(std::error_code &ec) {
#ifdef _WIN32
      WCHAR exe_path[MAX_PATH];
      GetModuleFileNameW(nullptr, exe_path, _countof(exe_path));
      auto dir = std::filesystem::path {exe_path}.remove_filename() / L"updates";

      std::filesystem::remove_all(dir, ec);
      if (ec) {
        return {};
      }

      PSECURITY_DESCRIPTOR sd = nullptr;
      // P = protected (no inherited ACEs), OICI = inherited by the files we write into it.
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &sd, nullptr
          )) {
        ec = std::error_code((int) GetLastError(), std::system_category());
        return {};
      }

      SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
      BOOL created = CreateDirectoryW(dir.c_str(), &sa);
      DWORD err = GetLastError();
      LocalFree(sd);

      if (!created) {
        // ERROR_ALREADY_EXISTS here means someone recreated it between the removal and now.
        ec = std::error_code((int) err, std::system_category());
        return {};
      }
      return dir;
#else
      auto dir = platf::appdata() / "updates";
      std::filesystem::create_directories(dir, ec);
      return ec ? std::filesystem::path {} : dir;
#endif
    }

    /// Random, unguessable file name for one staged installer.
    std::string
    random_staging_name() {
      std::random_device rd;
      std::ostringstream name;
      name << STAGING_PREFIX << std::hex << rd() << rd() << ".exe";
      return name.str();
    }

#ifdef _WIN32
    /**
     * Check that `path` carries a valid Authenticode signature from PUBLISHER_THUMBPRINT.
     * Accepts everything while no thumbprint is configured (unsigned releases).
     */
    bool
    verify_publisher(const std::filesystem::path &path, std::string &why) {
      if (PUBLISHER_THUMBPRINT.empty()) {
        return true;
      }

      WINTRUST_FILE_INFO file_info {};
      file_info.cbStruct = sizeof(file_info);
      file_info.pcwszFilePath = path.c_str();

      GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
      WINTRUST_DATA trust {};
      trust.cbStruct = sizeof(trust);
      trust.dwUIChoice = WTD_UI_NONE;
      trust.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
      trust.dwUnionChoice = WTD_CHOICE_FILE;
      trust.pFile = &file_info;
      trust.dwStateAction = WTD_STATEACTION_VERIFY;

      LONG status = WinVerifyTrust(nullptr, &action, &trust);
      bool ok = false;

      if (status != ERROR_SUCCESS) {
        why = "The installer is not validly signed (WinVerifyTrust error " + std::to_string(status) + ").";
      } else if (auto *prov = WTHelperProvDataFromStateData(trust.hWVTStateData)) {
        auto *signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0);
        auto *cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;

        BYTE hash[20];
        DWORD hash_size = sizeof(hash);
        if (cert && CertGetCertificateContextProperty(cert->pCert, CERT_SHA1_HASH_PROP_ID, hash, &hash_size)) {
          static const char *digits = "0123456789ABCDEF";
          std::string thumbprint;
          for (DWORD i = 0; i < hash_size; ++i) {
            thumbprint += digits[hash[i] >> 4];
            thumbprint += digits[hash[i] & 0x0f];
          }
          ok = thumbprint == PUBLISHER_THUMBPRINT;
          if (!ok) {
            why = "The installer is signed by an unexpected publisher (" + thumbprint + ").";
          }
        } else {
          why = "Could not read the installer's signing certificate.";
        }
      } else {
        why = "Could not read the installer's signature.";
      }

      trust.dwStateAction = WTD_STATEACTION_CLOSE;
      WinVerifyTrust(nullptr, &action, &trust);
      return ok;
    }
#endif

  }  // namespace

  namespace detail {
    std::string
    extract_expected_sha(const std::string &sums, const std::string &installer_name) {
      std::istringstream iss(sums);
      std::string line;
      while (std::getline(iss, line)) {
        std::istringstream ls(line);
        std::string hash, fname;
        if (!(ls >> hash >> fname)) continue;
        if (!fname.empty() && fname.front() == '*') fname.erase(0, 1);  // binary-mode marker
        if (fname == installer_name && hash.size() == 64 &&
            std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
              return std::isxdigit(c);
            })) {
          return hash;
        }
      }
      return {};
    }
  }  // namespace detail

  namespace {

    /// The mutable pipeline state. Atomics for the hot path (progress polling), mutex for the strings.
    struct state_t {
      std::mutex mutex;
      std::atomic<state_e> state { state_e::idle };
      std::atomic<std::uint64_t> downloaded_bytes { 0 };
      std::atomic<std::uint64_t> total_bytes { 0 };
      std::string message;          ///< Populated on error (guarded by mutex).
      std::string target_version;   ///< Version being downloaded/applied (guarded by mutex).
      std::string staging_path;     ///< Where the installer was written (guarded by mutex).
      std::string expected_sha256;  ///< Verified hash, re-checked right before launch (guarded by mutex).
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
      std::error_code ec;
      auto dir = staging_directory(ec);
      if (dir.empty()) {
        BOOST_LOG(error) << "update: could not prepare the staging directory: "sv << ec.message();
        set_error("Could not prepare a safe folder for the installer: " + ec.message());
        return;
      }

      std::string staging = (dir / random_staging_name()).string();
      {
        std::lock_guard lock(g_state.mutex);
        g_state.staging_path = staging;
        g_state.expected_sha256.clear();
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
        // Nothing to verify against means nothing to vouch for what we are about to run elevated.
        // Every release since the updater exists publishes the checksum file, so this is either a
        // transient fetch failure (retry) or a release that should be installed by hand.
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        BOOST_LOG(error) << "update: no checksum published for "sv << info.asset.name << ", refusing to stage it"sv;
        set_error("Could not fetch the installer checksum, so it cannot be verified. Try again, or install the release manually.");
        return;
      }

      std::string actual = sha256_file(staging);
      if (actual.empty()) {
        set_error("Could not read the downloaded installer to verify it.");
        return;
      }
      if (!equal_hex(actual, expected)) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        set_error("Installer checksum mismatch — the download may be corrupt. Please try again.");
        return;
      }

      std::lock_guard lock(g_state.mutex);
      g_state.expected_sha256 = expected;
      g_state.message.clear();
      g_state.state.store(state_e::ready, std::memory_order_relaxed);
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
      g_state.staging_path.clear();
      g_state.expected_sha256.clear();
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
    std::string expected;
    {
      std::lock_guard lock(g_state.mutex);
      staging = g_state.staging_path;
      expected = g_state.expected_sha256;
    }
    if (staging.empty() || expected.empty() || !std::filesystem::exists(staging)) {
      set_error("The staged installer is missing. Please download it again.");
      return false;
    }

    std::filesystem::path exe(staging);

    // The checksum was verified when the download finished, which can be hours before this
    // click. Hold the file open without write or delete sharing from here until the process has
    // been created, and hash it again through that handle: nothing can change or swap it between
    // the check and the launch.
    HANDLE lock_handle = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock_handle == INVALID_HANDLE_VALUE) {
      set_error("Could not open the staged installer (Windows error " + std::to_string(GetLastError()) + "). Please download it again.");
      return false;
    }
    auto close_lock = util::fail_guard([&]() {
      CloseHandle(lock_handle);
    });

    auto actual = sha256_stream([lock_handle](unsigned char *buf, size_t size) -> long long {
      DWORD read = 0;
      if (!ReadFile(lock_handle, buf, (DWORD) size, &read, nullptr)) return -1;
      return (long long) read;
    });
    if (!equal_hex(actual, expected)) {
      BOOST_LOG(error) << "update: the staged installer changed after it was verified; refusing to run it"sv;
      set_error("The staged installer changed after it was verified and was not run. Please download it again.");
      return false;
    }

    if (std::string why; !verify_publisher(exe, why)) {
      BOOST_LOG(error) << "update: "sv << why;
      set_error(why);
      return false;
    }
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
  cleanup_staged_installers() {
    std::error_code ec;

    // Releases before the staging hardening left a fixed-name installer in %TEMP%.
    if (auto tmp = std::filesystem::temp_directory_path(ec); !ec && !tmp.empty()) {
      std::filesystem::remove(tmp / LEGACY_STAGING_FILENAME, ec);
    }

#ifdef _WIN32
    WCHAR exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, _countof(exe_path));
    auto dir = std::filesystem::path {exe_path}.remove_filename() / L"updates";
#else
    auto dir = platf::appdata() / "updates";
#endif
    // Anything here was either installed already (we are the new version) or abandoned. The
    // process that launched the installer is the one being replaced, so it cannot clean up after
    // itself; its successor does it here.
    auto removed = std::filesystem::remove_all(dir, ec);
    if (ec) {
      // Expected right after an in-app update: the installer that started us may still be
      // finishing up, and Windows will not delete a running executable. The next start gets it.
      BOOST_LOG(info) << "update: old staged installers not removed yet: "sv << ec.message();
    } else if (removed > 1) {
      BOOST_LOG(info) << "update: removed "sv << (removed - 1) << " old staged installer file(s)"sv;
    }
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
