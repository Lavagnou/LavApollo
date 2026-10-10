/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Convert a UTF-8 string into a UTF-16 wide string.
   * @param string The UTF-8 string.
   * @return The converted UTF-16 wide string.
   */
  std::wstring from_utf8(const std::string_view &string);

  /**
   * @brief Convert a UTF-16 wide string into a UTF-8 string.
   * @param string The UTF-16 wide string.
   * @return The converted UTF-8 string.
   */
  std::string to_utf8(const std::wstring_view &string);

  bool is_running_as_system();

  /**
   * @brief Read a file's VS_FIXEDFILEINFO version as "major.minor.build.revision".
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);

  /**
   * @brief Whether any DXGI adapter is an NVIDIA GPU.
   */
  bool has_nvidia_gpu();

  /**
   * @brief Whether any DXGI adapter is an AMD GPU.
   */
  bool has_amd_gpu();

  STARTUPINFOEXW create_startup_info(FILE *file, HANDLE *job, std::error_code &ec);
  void free_proc_thread_attr_list(LPPROC_THREAD_ATTRIBUTE_LIST list);

  /**
   * @brief Launch a process in the interactive user's session.
   *
   * When Sunshine runs as SYSTEM (the service), the process gets the logged-on user's
   * token and environment; otherwise it simply inherits ours. Unlike run_command(), the
   * caller keeps the raw PROCESS_INFORMATION, for processes it has to watch or stop.
   */
  bool launch_process_with_impersonation(bool elevated, const std::string &cmd, const std::wstring &start_dir, DWORD creation_flags, STARTUPINFOEXW &startup_info, PROCESS_INFORMATION &process_info, std::error_code &ec);
}  // namespace platf
