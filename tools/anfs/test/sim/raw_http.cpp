// tools/anfs/test/sim/raw_http.cpp - anfs_sim (ANFS-F8): a raw Winsock GET for `anfs_sim record`, which captures
// the exact response bytes of the C# reference (status line, header order, chunk framing) into fixture files, and
// the port / host guards the sim shares with the harness (lib-guards.ps1). The functions are declared in
// sim_main.cpp (no header: the sim is two files by design, anfs-cpp.md 2.2).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "core/net.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anfs::sim {

struct RawResult {
    bool ok = false;                              // connected and a head arrived
    int status = 0;
    std::string head;                             // up to and including "\r\n\r\n"
    std::string body;                             // bytes after the head, framing included (chunks as sent)
    std::string error;
};

// The ports the live stack owns; the sim, anfs.exe in test mode and the harness refuse all of them (C7).
bool refusedPort(int port) { return port == 8080 || port == 8899 || port == 443 || port == 7700 || port == 7701; }

// The live Node bridge and its public name; the sim only ever talks to loopback.
bool refusedHost(std::string_view host)
{
    std::string h(host);
    for (char& c : h) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (h == "192.168.1.177" || h == "example.invalid") return true;
    return !net::isLoopbackHost(h);
}

// "" when the URL is a loopback http(s) URL on an allowed port, else the refusal text.
std::string refuseUrl(std::string_view url)
{
    auto u = net::parseUrl(url);
    if (!u) return "not a URL: " + std::string(url);
    if (refusedHost(u->host)) return "refused host " + u->host + " (the sim talks to loopback only)";
    if (refusedPort(u->port)) return "refused port " + std::to_string(u->port) + " (live stack)";
    return {};
}

RawResult rawGet(const std::string& host, int port, const std::string& pathAndQuery,
                 const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs)
{
    RawResult r;
    if (refusedHost(host) || refusedPort(port)) {
        r.error = "refused " + host + ":" + std::to_string(port);
        return r;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        r.error = "socket failed";
        return r;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    if (InetPtonA(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &a.sin_addr) != 1)
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        r.error = "connect failed (" + std::to_string(WSAGetLastError()) + ")";
        closesocket(s);
        return r;
    }
    DWORD t = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    std::string req = "GET " + pathAndQuery + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\n";
    for (const auto& h : headers) req += h.first + ": " + h.second + "\r\n";
    req += "Connection: close\r\n\r\n";
    size_t at = 0;
    while (at < req.size()) {
        int k = ::send(s, req.data() + at, static_cast<int>(req.size() - at), 0);
        if (k <= 0) {
            r.error = "send failed";
            closesocket(s);
            return r;
        }
        at += static_cast<size_t>(k);
    }
    std::string all;
    char buf[65536];
    for (;;) {
        int k = ::recv(s, buf, sizeof buf, 0);
        if (k <= 0) break;
        all.append(buf, static_cast<size_t>(k));
    }
    closesocket(s);
    size_t p = all.find("\r\n\r\n");
    if (p == std::string::npos) {
        r.error = all.empty() ? "no response" : "incomplete head";
        return r;
    }
    r.head = all.substr(0, p + 4);
    r.body = all.substr(p + 4);
    size_t sp = r.head.find(' ');
    if (sp != std::string::npos) r.status = std::atoi(r.head.c_str() + sp + 1);
    r.ok = r.status > 0;
    return r;
}

} // namespace anfs::sim
