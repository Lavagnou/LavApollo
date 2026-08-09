/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
#include <enet/enet.h>

// local includes
#include "utility.h"

namespace net {
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  /**
   * @brief A streaming socket that could not be bound during pre-flight validation.
   */
  struct port_failure_t {
    int delta;  ///< offset from the base port (e.g. -5, 0, 1, 9, 10, 11, 21)
    std::uint16_t port;  ///< the derived port number (map_port(delta))
    const char *proto;  ///< "TCP" or "UDP"
    const char *role;  ///< human-readable label, e.g. "HTTPS streaming (nvhttp)"
    int error_value;  ///< native error code (WSAEACCES on Windows, errno on POSIX)
    std::string message;  ///< boost error message (ec.message())
  };

  /**
   * @brief Result of pre-flight validation of all streaming ports.
   */
  struct port_validation_t {
    std::vector<port_failure_t> failures;
    bool ui_port_blocked = false;  ///< true if the Web UI port (base+1) is among the failures
    bool ephemeral_range = false;  ///< true if the base port lies in the OS ephemeral range (Windows)
  };

  /**
   * @brief Test-bind every streaming socket and report the ones that cannot be bound.
   * @details Opens a short-lived TCP/UDP socket matching each derived port, sets
   *          reuse_address, binds, then closes. Captures the error_code for each
   *          failure. Does not itself log; the caller formats the result. Resolves
   *          the @todo on map_port.
   * @return Aggregated failures plus ephemeral-range/UI-port flags.
   */
  port_validation_t validate_stream_ports();

  /**
   * @brief Format a single port_failure_t as a one-line message including the hint.
   */
  std::string format_port_failure(const port_failure_t &failure);

  /**
   * @brief Return a platform-specific, actionable explanation for a bind error_code.
   * @details On Windows, WSAEACCES (10013) yields the reserved/excluded-port-range
   *          advice (Hyper-V/WSL/Docker/WinNAT) with the netsh command; on POSIX,
   *          EACCES yields the privileged-port advice. Otherwise returns a generic
   *          "in use" hint. Returns an empty string for a non-error code. The text
   *          includes a leading separator so it can be appended after err.what().
   */
  std::string bind_error_explanation(const boost::system::error_code &ec);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  net_e from_enum_string(const std::string_view &view);
  std::string_view to_enum_string(net_e net);

  net_e from_address(const std::string_view &view);

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Get the address family enum value from a string.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);
}  // namespace net
