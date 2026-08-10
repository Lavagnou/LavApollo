/**
 * @file src/updater.h
 * @brief In-app updater: check, download, verify, and install LavApollo releases from GitHub.
 *
 * Windows provides the full implementation (the release workflow only ships Windows installers, and the
 * install path relies on the Apollo service + NSIS installer). On other platforms the functions compile as
 * stubs that report "not supported".
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>

namespace updater {
  // State machine for the download/install pipeline.
  enum class state_e {
    idle,         ///< Nothing in progress.
    checking,     ///< Querying GitHub for the latest release.
    downloading,  ///< Fetching the installer.
    verifying,    ///< Verifying the installer checksum.
    ready,        ///< Installer staged + verified, waiting for the user to apply it.
    installing,   ///< Installer has been launched.
    error         ///< The last operation failed; see status_t::message.
  };

  struct asset_t {
    std::string name;
    std::string url;
    uint64_t size = 0;  ///< Bytes, from the GitHub release metadata (0 if unknown).
  };

  struct version_info_t {
    std::string current;        ///< Running version (PROJECT_VERSION).
    std::string latest;         ///< Latest released version (e.g. "1.0.4").
    std::string latest_tag;     ///< Latest release tag (e.g. "v1.0.4").
    std::string release_notes_url;  ///< GitHub HTML URL of the release.
    bool update_available = false;  ///< latest is newer than current.
    bool service_managed = false;   ///< Running under the Apollo service (Windows) — install supported.
    asset_t asset;              ///< The Windows installer asset (empty if none found).
    std::string error;          ///< Non-empty if the check itself failed.
  };

  // Snapshot of the pipeline progress, safe to read from the HTTP thread.
  struct status_t {
    state_e state = state_e::idle;
    uint64_t downloaded_bytes = 0;
    uint64_t total_bytes = 0;
    std::string message;
    std::string target_version;
  };

  /**
   * @brief Query GitHub for the latest release (cached for ~10 minutes) and compare against the running
   *        version. Never throws — network/parse failures are reported via version_info_t::error.
   */
  version_info_t check_for_update();

  /**
   * @brief Start downloading + verifying the installer to a staging path on a detached thread.
   * @return false if a download/verify is already in progress or there is nothing to download.
   */
  bool start_download(const version_info_t &info);

  /**
   * @brief Launch the staged installer silently and elevated (Windows service-managed installs only).
   * @return false if not ready, not on Windows, or not service-managed.
   *
   * On success the installer drives the rest: its uninstall step stops the service (which stops this
   * process), swaps files, and restarts the service into the new binary. This function therefore does NOT
   * self-exit — that would race the service's auto-respawn.
   */
  bool stage_installer();

  /// Snapshot of the current pipeline state for the status endpoint.
  status_t get_status();

  /// Whether this process is managed by the Apollo service (Windows). Always false on other platforms.
  bool is_service_managed();

  /// Reset the pipeline back to idle so a failed operation can be retried.
  void reset();
}  // namespace updater
