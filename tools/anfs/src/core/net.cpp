// core/net.cpp - ANFS-F6: IP addresses, client classes (C# Net.Classify / wan-gate.js kindOf), the trusted-proxy
// client rule (C# Net.RealClient), the LanOnly source rule, URL origins and the local-address helpers.
//
// Rules pinned by test_anfs_core_gate:
// - Classes: loopback 127/8 and ::1; LAN 10/8, 172.16/12, 192.168/16, fc00::/7, fe80::/10; tailnet 100.64/10; else
//   WAN. IPv4-mapped IPv6 is normalised first. Anything unparsable is WAN (fail closed).
// - realClient: X-Forwarded-For is honoured only when the TCP peer is a trusted proxy, and then only the LAST hop
//   (our own proxy appends it). An unparsable last hop is IpAddr::none() (WAN). An absent / blank header is the peer
//   itself (C# rule; Node fails closed on a present-but-empty header, which the F4 server never passes on).
// - isLanSource (LanOnly): loopback, 10/8, 172.16/12, 192.168/16, 169.254/16, ::1. The tailnet is NOT allowed (Node).
// Winsock is initialised per call (WSAStartup is reference-counted), so this file does not depend on the server.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>

#include "core/net.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace anfs::net {

namespace {

struct Wsa {
    Wsa() { WSADATA d; ok = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
    ~Wsa() { if (ok) WSACleanup(); }
    bool ok = false;
};

std::string_view trimView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::string lowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool isV4Mapped(const IpAddr& a)
{
    if (!a.v6) return false;
    for (int i = 0; i < 10; ++i)
        if (a.bytes[i] != 0) return false;
    return a.bytes[10] == 0xFF && a.bytes[11] == 0xFF;
}

bool isV6Loopback(const IpAddr& a)
{
    for (int i = 0; i < 15; ++i)
        if (a.bytes[i] != 0) return false;
    return a.bytes[15] == 1;
}

// inet_pton on the bare address text (no port, no zone, no brackets).
std::optional<IpAddr> parseBare(std::string_view s)
{
    if (s.empty() || s.size() > 64) return std::nullopt;
    std::string z(s);
    IpAddr a;
    in_addr v4{};
    if (inet_pton(AF_INET, z.c_str(), &v4) == 1) {
        a.valid = true;
        std::memcpy(a.bytes.data(), &v4, 4);
        return a;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, z.c_str(), &v6) == 1) {
        a.valid = true;
        a.v6 = true;
        std::memcpy(a.bytes.data(), &v6, 16);
        return a;
    }
    return std::nullopt;
}

// Formats a socket address (IPv4 or IPv6) as text; "" on failure.
std::string addrText(const sockaddr* sa)
{
    char buf[INET6_ADDRSTRLEN + 1] = {};
    if (sa->sa_family == AF_INET) {
        const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(sa);
        if (!inet_ntop(AF_INET, const_cast<in_addr*>(&in->sin_addr), buf, sizeof buf)) return {};
    } else if (sa->sa_family == AF_INET6) {
        const sockaddr_in6* in = reinterpret_cast<const sockaddr_in6*>(sa);
        if (!inet_ntop(AF_INET6, const_cast<in6_addr*>(&in->sin6_addr), buf, sizeof buf)) return {};
    } else {
        return {};
    }
    return buf;
}

} // namespace

const char* className(NetClass c)
{
    switch (c) {
    case NetClass::Loopback: return "Loopback";
    case NetClass::Lan: return "Lan";
    case NetClass::Tailnet: return "Tailnet";
    default: return "Wan";
    }
}

// ---- IpAddr ----------------------------------------------------------------------------------------------------------

std::optional<IpAddr> IpAddr::parse(std::string_view text)
{
    std::string_view s = trimView(text);
    // C# ParseAddress also strips a surrounding pair of quotes (values copied from JSON by hand).
    while (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = trimView(s.substr(1, s.size() - 2));
    if (s.empty()) return std::nullopt;
    if (s.front() == '[') {
        const size_t e = s.find(']');
        if (e == std::string_view::npos) return std::nullopt;
        s = s.substr(1, e - 1);
    } else {
        const size_t first = s.find(':');
        if (first != std::string_view::npos && first == s.rfind(':')) s = s.substr(0, first);   // "1.2.3.4:80"
    }
    const size_t pct = s.find('%');
    if (pct != std::string_view::npos) s = s.substr(0, pct);                                   // "fe80::1%3"
    return parseBare(s);
}

IpAddr IpAddr::v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    IpAddr r;
    r.valid = true;
    r.bytes[0] = a; r.bytes[1] = b; r.bytes[2] = c; r.bytes[3] = d;
    return r;
}

IpAddr IpAddr::none() { return IpAddr{}; }

IpAddr IpAddr::normalized() const
{
    if (!valid || !isV4Mapped(*this)) return *this;
    return v4(bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool IpAddr::isLoopback() const
{
    const IpAddr a = normalized();
    if (!a.valid) return false;
    return a.v6 ? isV6Loopback(a) : a.bytes[0] == 127;
}

std::string IpAddr::toString() const
{
    if (!valid) return {};
    char buf[INET6_ADDRSTRLEN + 1] = {};
    if (v6) {
        in6_addr v{};
        std::memcpy(&v, bytes.data(), 16);
        if (!inet_ntop(AF_INET6, &v, buf, sizeof buf)) return {};
    } else {
        in_addr v{};
        std::memcpy(&v, bytes.data(), 4);
        if (!inet_ntop(AF_INET, &v, buf, sizeof buf)) return {};
    }
    return buf;
}

bool IpAddr::operator==(const IpAddr& o) const
{
    if (valid != o.valid) return false;
    if (!valid) return true;
    if (v6 != o.v6) return false;
    return std::memcmp(bytes.data(), o.bytes.data(), v6 ? 16 : 4) == 0;
}

// ---- classes ---------------------------------------------------------------------------------------------------------

NetClass classify(const IpAddr& addr)
{
    const IpAddr a = addr.normalized();
    if (!a.valid) return NetClass::Wan;
    const auto& b = a.bytes;
    if (!a.v6) {
        if (b[0] == 127) return NetClass::Loopback;
        if (b[0] == 100 && (b[1] & 0xC0) == 64) return NetClass::Tailnet;
        if (b[0] == 10) return NetClass::Lan;
        if (b[0] == 172 && (b[1] & 0xF0) == 16) return NetClass::Lan;
        if (b[0] == 192 && b[1] == 168) return NetClass::Lan;
        return NetClass::Wan;
    }
    if (isV6Loopback(a)) return NetClass::Loopback;
    if ((b[0] & 0xFE) == 0xFC) return NetClass::Lan;                 // fc00::/7 unique local
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return NetClass::Lan;  // fe80::/10 link local
    return NetClass::Wan;
}

NetClass classify(std::string_view addressText)
{
    const std::optional<IpAddr> a = IpAddr::parse(addressText);
    return a ? classify(*a) : NetClass::Wan;
}

bool matches(const IpAddr& addr, std::string_view entryText)
{
    const IpAddr a = addr.normalized();
    const std::string_view entry = trimView(entryText);
    if (!a.valid || entry.empty()) return false;
    const size_t slash = entry.find('/');
    if (slash == std::string_view::npos) {
        const std::optional<IpAddr> e = IpAddr::parse(entry);
        return e && e->normalized() == a;
    }
    const std::optional<IpAddr> net = IpAddr::parse(entry.substr(0, slash));
    const std::string_view bitsText = entry.substr(slash + 1);
    if (!net || bitsText.empty() || bitsText.size() > 3) return false;
    int bits = 0;
    for (char c : bitsText) {
        if (c < '0' || c > '9') return false;
        bits = bits * 10 + (c - '0');
    }
    const IpAddr n = net->normalized();
    if (n.v6 != a.v6) return false;
    const int len = a.v6 ? 16 : 4;
    if (bits < 0 || bits > len * 8) return false;
    for (int i = 0; i < len && bits > 0; ++i, bits -= 8) {
        const int mask = bits >= 8 ? 0xFF : (0xFF << (8 - bits)) & 0xFF;
        if ((a.bytes[i] & mask) != (n.bytes[i] & mask)) return false;
    }
    return true;
}

bool isTrustedProxy(const IpAddr& peer, const std::vector<std::string>& trusted)
{
    if (!peer.valid) return false;
    for (const std::string& t : trusted)
        if (matches(peer, t)) return true;
    return false;
}

IpAddr realClient(const IpAddr& tcpPeer, std::string_view forwardedFor, const std::vector<std::string>& trusted)
{
    const IpAddr peer = tcpPeer.normalized();
    if (!peer.valid) return IpAddr::none();
    if (trimView(forwardedFor).empty()) return peer;
    if (!isTrustedProxy(peer, trusted)) return peer;
    const size_t comma = forwardedFor.rfind(',');
    const std::string_view last = comma == std::string_view::npos ? forwardedFor : forwardedFor.substr(comma + 1);
    const std::optional<IpAddr> r = IpAddr::parse(last);
    // an unparsable entry from a trusted proxy fails closed: treated as a WAN address
    return r ? r->normalized() : IpAddr::none();
}

bool isLanSource(const IpAddr& addr)
{
    const IpAddr a = addr.normalized();
    if (!a.valid) return false;
    const auto& b = a.bytes;
    if (a.v6) return isV6Loopback(a);
    if (b[0] == 127 || b[0] == 10) return true;
    if (b[0] == 172 && (b[1] & 0xF0) == 16) return true;
    if (b[0] == 192 && b[1] == 168) return true;
    if (b[0] == 169 && b[1] == 254) return true;
    return false;
}

// ---- URLs ------------------------------------------------------------------------------------------------------------

std::string Url::origin() const
{
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    const int def = scheme == "https" ? 443 : 80;
    std::string out = scheme + "://" + h;
    if (port != def) out += ":" + std::to_string(port);
    return out;
}

std::optional<Url> parseUrl(std::string_view text)
{
    std::string_view s = trimView(text);
    const size_t p = s.find("://");
    if (p == std::string_view::npos || p == 0) return std::nullopt;
    Url u;
    u.scheme = lowerAscii(s.substr(0, p));
    if (u.scheme != "http" && u.scheme != "https") return std::nullopt;
    s.remove_prefix(p + 3);
    const size_t frag = s.find('#');
    if (frag != std::string_view::npos) s = s.substr(0, frag);                 // fragments never go on the wire
    const size_t end = s.find_first_of("/?");
    std::string_view authority = end == std::string_view::npos ? s : s.substr(0, end);
    std::string_view rest = end == std::string_view::npos ? std::string_view{} : s.substr(end);
    if (authority.empty() || authority.find('@') != std::string_view::npos) return std::nullopt;
    std::string_view portText;
    if (authority.front() == '[') {
        const size_t e = authority.find(']');
        if (e == std::string_view::npos) return std::nullopt;
        u.host = lowerAscii(authority.substr(1, e - 1));
        std::string_view after = authority.substr(e + 1);
        if (!after.empty()) {
            if (after.front() != ':') return std::nullopt;
            portText = after.substr(1);
        }
        if (!parseBare(u.host) || !parseBare(u.host)->v6) return std::nullopt;
    } else {
        const size_t colon = authority.find(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':', colon + 1) != std::string_view::npos) return std::nullopt;   // bare IPv6 needs []
            portText = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
        }
        u.host = lowerAscii(authority);
        if (u.host.empty()) return std::nullopt;
        for (unsigned char c : u.host)
            if (c <= ' ' || c == '/' || c == '\\' || c == '?' || c == '#' || c == '[' || c == ']') return std::nullopt;
    }
    if (portText.empty()) {
        u.port = u.scheme == "https" ? 443 : 80;
    } else {
        if (portText.size() > 5) return std::nullopt;
        int port = 0;
        for (char c : portText) {
            if (c < '0' || c > '9') return std::nullopt;
            port = port * 10 + (c - '0');
        }
        if (port < 1 || port > 65535) return std::nullopt;
        u.port = port;
    }
    if (rest.empty()) u.pathAndQuery = "/";
    else if (rest.front() == '?') u.pathAndQuery = "/" + std::string(rest);
    else u.pathAndQuery = std::string(rest);
    return u;
}

bool isLoopbackHost(std::string_view hostText)
{
    std::string_view host = trimView(hostText);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (ieq(host, "localhost")) return true;
    const std::optional<IpAddr> a = parseBare(host);
    return a && a->isLoopback();
}

// ---- local machine ---------------------------------------------------------------------------------------------------

std::string hostName()
{
    Wsa wsa;
    char buf[256] = {};
    if (gethostname(buf, sizeof buf - 1) != 0) return {};
    return buf;
}

std::optional<std::string> localAddressFor(std::string_view hostText, int port)
{
    Wsa wsa;
    if (!wsa.ok) return std::nullopt;
    std::string host(trimView(hostText));
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (host.empty()) return std::nullopt;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* res = nullptr;
    const std::string portText = std::to_string(port > 0 && port <= 65535 ? port : 9);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &res) != 0 || !res) return std::nullopt;
    std::optional<std::string> out;
    for (addrinfo* ai = res; ai && !out; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6) continue;
        SOCKET s = socket(ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
        // A UDP connect sends nothing: it only picks the route and binds the local address.
        if (connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            sockaddr_storage local{};
            int len = sizeof local;
            if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                std::string t = addrText(reinterpret_cast<sockaddr*>(&local));
                if (!t.empty()) out = t;
            }
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return out;
}

std::vector<std::string> localIPv4Addresses()
{
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    for (int attempt = 0; attempt < 4; ++attempt) {
        buf.resize(size);
        const ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                                           GAA_FLAG_SKIP_DNS_SERVER,
                                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
        if (rc == ERROR_BUFFER_OVERFLOW) continue;
        if (rc != NO_ERROR) return out;
        for (const IP_ADAPTER_ADDRESSES* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (const IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next) {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const std::string t = addrText(u->Address.lpSockaddr);
                if (t.empty() || t.rfind("127.", 0) == 0) continue;
                if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
            }
        }
        return out;
    }
    return out;
}

} // namespace anfs::net
