// core/net.h - IP addresses, client classes, trusted-proxy client address, URLs and local-address helpers.
// FROZEN (ANFS-F0); implemented by ANFS-F6 (core/net.cpp).
//
// Classes (C# Net.Classify, wan-gate.js kindOf): loopback (127/8, ::1), LAN (10/8, 172.16/12, 192.168/16, fc00::/7,
// fe80::/10), tailnet (100.64/10), else WAN. IPv4-mapped IPv6 is normalised to IPv4 first. An unparsable address is
// WAN (fail closed).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anfs::net {

enum class NetClass { Loopback, Lan, Tailnet, Wan };
const char* className(NetClass c);                // "Loopback", "Lan", "Tailnet", "Wan" (C# enum names, for logs)

struct IpAddr {
    bool valid = false;
    bool v6 = false;
    std::array<uint8_t, 16> bytes{};              // IPv4 uses bytes[0..3]

    static std::optional<IpAddr> parse(std::string_view s);   // "1.2.3.4", "1.2.3.4:80", "[::1]:80", "fe80::1%3"
    static IpAddr v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
    static IpAddr none();                         // invalid address (classified WAN)
    IpAddr normalized() const;                    // ::ffff:a.b.c.d -> a.b.c.d
    bool isLoopback() const;
    std::string toString() const;                 // canonical text ("10.0.0.5", "::1")
    bool operator==(const IpAddr& o) const;
    bool operator!=(const IpAddr& o) const { return !(*this == o); }
};

NetClass classify(const IpAddr& a);
NetClass classify(std::string_view addressText);
inline bool isLocalOrPrivate(NetClass c) { return c != NetClass::Wan; }

// entry is an address or a CIDR ("10.0.0.0/8", "fd00::/8").
bool matches(const IpAddr& a, std::string_view entry);
bool isTrustedProxy(const IpAddr& peer, const std::vector<std::string>& trusted);
// The real client: X-Forwarded-For is honoured only when the TCP peer is a trusted proxy, and then only its LAST hop
// (the one our own proxy appended). An unparsable last hop from a trusted proxy is IpAddr::none() (WAN).
IpAddr realClient(const IpAddr& tcpPeer, std::string_view forwardedFor, const std::vector<std::string>& trusted);
// LanOnly source rule: loopback, 127/8, 10/8, 172.16/12, 192.168/16, 169.254/16, ::1. Tailnet is NOT allowed (Node).
bool isLanSource(const IpAddr& a);

struct Url {
    std::string scheme;                           // "http" or "https" (lower-case)
    std::string host;                             // without brackets
    int port = 0;                                 // explicit or the scheme default
    std::string pathAndQuery = "/";
    std::string origin() const;                   // scheme://host[:port] (port omitted when default)
};
std::optional<Url> parseUrl(std::string_view text);
// Test-mode bridge guard: true for "localhost", 127.0.0.0/8 and ::1.
bool isLoopbackHost(std::string_view host);

// The OS DNS host name (gethostname; Node os.hostname()), never the NetBIOS machine name.
std::string hostName();
// The local address a UDP socket connected to host:port would use (peer register "ip"). nullopt when no route.
std::optional<std::string> localAddressFor(std::string_view host, int port);
// Non-loopback IPv4 addresses of up interfaces, in adapter order (HostId.LocalIp fallbacks).
std::vector<std::string> localIPv4Addresses();

// Note: socket users call WSAStartup themselves (it is reference-counted), so http_server.cpp (F4) does not depend
// on net.cpp (F6). The server fills Request::clientIp/clientClass through ServerOptions::resolveClient, which the
// runtime (F7) wires to realClient() + classify().

} // namespace anfs::net
