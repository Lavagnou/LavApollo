/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <cerrno>
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("100.64.0.0/10"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  std::string_view af_to_any_address_string(af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    auto any_addr = net::af_to_any_address_string(af);
    enet_address_set_host(&addr, any_addr.data());
    enet_address_set_port(&addr, port);

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  namespace {
    // Canonical streaming port layout, as offsets from the base port. Kept as raw
    // ints here so network.cpp does not need to include the streaming headers
    // (nvhttp.h, confighttp.h, rtsp.h, stream.h) and stay self-contained. Source
    // of truth for the values: those headers' PORT_* / RTSP_SETUP_PORT constants.
    //   base-5  TCP  HTTPS streaming     (nvhttp::PORT_HTTPS)
    //   base+0  TCP  HTTP streaming      (nvhttp::PORT_HTTP)
    //   base+1  TCP  Web UI              (confighttp::PORT_HTTPS)
    //   base+21 TCP  RTSP setup          (rtsp_stream::RTSP_SETUP_PORT)
    //   base+9  UDP  video               (stream::VIDEO_STREAM_PORT)
    //   base+10 UDP  control (+ mic)     (stream::CONTROL_PORT)
    //   base+11 UDP  audio               (stream::AUDIO_STREAM_PORT)
    struct port_spec_t {
      int delta;
      const char *proto;
      const char *role;
    };
    constexpr port_spec_t kStreamingPorts[] = {
      {-5, "TCP", "HTTPS streaming (nvhttp)"},
      {0, "TCP", "HTTP streaming (nvhttp)"},
      {1, "TCP", "Web UI (confighttp)"},
      {21, "TCP", "RTSP setup"},
      {9, "UDP", "video stream"},
      {10, "UDP", "control"},
      {11, "UDP", "audio stream"},
    };
  }  // namespace

  port_validation_t validate_stream_ports() {
    boost::asio::io_context ioc;
    const auto af = af_from_enum_string(config::sunshine.address_family);
    const bool is_v4 = (af == IPV4);

    port_validation_t result;
#ifdef _WIN32
    // Windows reserves parts of the ephemeral range (>= 49152) at every boot for
    // Hyper-V / WSL2 / Docker / WinNAT, so a base port there can fail non-deterministically.
    result.ephemeral_range = config::sunshine.port >= 49152;
#endif

    for (const auto &spec : kStreamingPorts) {
      const auto port = map_port(spec.delta);
      boost::system::error_code ec;

      if (spec.proto == "TCP"sv) {
        ip::tcp::acceptor acc(ioc);
        acc.open(is_v4 ? ip::tcp::v4() : ip::tcp::v6(), ec);
        if (!ec) {
          acc.set_option(boost::asio::socket_base::reuse_address {true}, ec);
          if (!ec) acc.bind(ip::tcp::endpoint(is_v4 ? ip::tcp::v4() : ip::tcp::v6(), port), ec);
          boost::system::error_code ignored;
          acc.close(ignored);
        }
      } else {
        ip::udp::socket sock(ioc);
        sock.open(is_v4 ? ip::udp::v4() : ip::udp::v6(), ec);
        if (!ec) {
          sock.set_option(boost::asio::socket_base::reuse_address {true}, ec);
          if (!ec) sock.bind(ip::udp::endpoint(is_v4 ? ip::udp::v4() : ip::udp::v6(), port), ec);
          boost::system::error_code ignored;
          sock.close(ignored);
        }
      }

      if (ec) {
        result.failures.push_back({spec.delta, port, spec.proto, spec.role, ec.value(), ec.message()});
        if (spec.delta == 1) result.ui_port_blocked = true;
      }
    }

    return result;
  }

  std::string format_port_failure(const port_failure_t &f) {
    // Reconstruct an error_code solely to drive bind_error_explanation(), which only
    // inspects the integer value. The original message is preserved in f.message.
    boost::system::error_code ec(
      f.error_value,
#ifdef _WIN32
      boost::system::system_category()
#else
      boost::system::generic_category()
#endif
    );

    std::ostringstream ss;
    ss << "["sv << f.proto << "] "sv << f.role << " on port "sv << f.port
       << ": ["sv << f.error_value << "] "sv << f.message
       << bind_error_explanation(ec);
    return ss.str();
  }

  std::string bind_error_explanation(const boost::system::error_code &ec) {
    if (!ec) {
      return {};
    }

#ifdef _WIN32
    constexpr int kAccessDenied = 10013;  // WSAEACCES
    constexpr int kAddrInUse = 10048;  // WSAEADDRINUSE
#else
    constexpr int kAccessDenied = EACCES;
    constexpr int kAddrInUse = EADDRINUSE;
#endif

    std::ostringstream ss;
    ss << "\n        Hint: "sv;
    if (ec.value() == kAccessDenied) {
#ifdef _WIN32
      ss << "permission denied. The port is likely inside a reserved/excluded Windows port range "
            "(Hyper-V, WSL2, Docker, WinNAT). Inspect with:\n"
            "          netsh int ipv4 show excludedportrange protocol=tcp\n"
            "        Choose a base port below 49152: Windows may reserve any port >= 49152 at each boot."sv;
#else
      ss << "permission denied. Ports below 1024 require elevated privileges. Run LavApollo as root, "
            "grant 'cap_net_bind_service' with setcap, or pick a base port whose derived ports are all >= 1024."sv;
#endif
    } else if (ec.value() == kAddrInUse) {
      ss << "the port is already in use by another process."sv;
    } else {
      ss << "the port may be in use or blocked by a firewall/antivirus."sv;
    }
    return ss.str();
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "LavApollo";
  }
}  // namespace net
