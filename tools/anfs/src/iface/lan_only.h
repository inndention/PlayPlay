// iface/lan_only.h - LanOnly, the guard on remote execution and operations routes (mod-srv-wan.md; C#
// SrvWanLanOnly.cs; Node execGateLanOnly). FROZEN (ANFS-F0). Header-only on top of core/net.
//
// Refused when the request carries X-Forwarded-For or x-anfs-loop (it came through a proxy), or when the SOCKET peer
// (never the forwarded client) is not loopback, 10/8, 172.16/12, 192.168/16 or 169.254/16. The tailnet is NOT
// allowed (Node parity).
#pragma once

#include "core/http_ctx.h"
#include "core/net.h"

#include <string>
#include <string_view>

namespace anfs::lanonly {

constexpr const char* kViaProxyText = "Remote execution is not available over the internet.";

// "" when allowed, else the 403 text.
inline std::string refusal(std::string_view forwardedFor, std::string_view loop, const std::string& peerIp)
{
    if (!forwardedFor.empty() || !loop.empty()) return kViaProxyText;
    auto a = net::IpAddr::parse(peerIp);
    if (a && net::isLanSource(*a)) return {};
    return "Remote execution refused for non-LAN source " + (a ? a->normalized().toString() : std::string()) + ".";
}

// Writes 403 {"error": text} and returns false when refused.
inline bool require(http::Ctx& ctx)
{
    std::string why = refusal(ctx.req.headers.get("x-forwarded-for"), ctx.req.headers.get("x-anfs-loop"), ctx.req.peerIp);
    if (why.empty()) return true;
    ctx.error(403, why);
    return false;
}

} // namespace anfs::lanonly
