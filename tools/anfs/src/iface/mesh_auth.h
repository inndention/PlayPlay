// iface/mesh_auth.h - MeshAuth, the check on mutating routes Node left open (mod-srv-ui.md; C# SrvUiMeshAuth.cs;
// deviations D7, D8, C6). FROZEN (ANFS-F0).
//
// ok() is true for loopback, LAN and tailnet clients (Request::clientClass already honours trusted proxies, so a
// proxied WAN client is WAN), for the agent token (x-agent-auth or ?auth=), for "Authorization: Bearer <agent token>"
// (C6: what PlayPlay MeshClient sends), for a valid portal session cookie and for HTTP Basic whose password verifies
// against the WAN record. The last two come from srv-ui through IMeshAuthProvider; without srv-ui running, WAN
// clients without a token fail (closed).
#pragma once

#include "core/gate.h"
#include "core/http_ctx.h"
#include "core/svc.h"

#include <cctype>
#include <string>
#include <string_view>

namespace anfs {

// Published by srv-ui (svcSet<IMeshAuthProvider>).
class IMeshAuthProvider {
public:
    virtual ~IMeshAuthProvider() = default;
    virtual bool sessionOk(const http::Ctx& ctx) const = 0;      // anfs_session=<64 hex> cookie, unexpired
    virtual bool basicOk(const http::Ctx& ctx) const = 0;        // Basic password verifies against wan.passwordRecord
};

namespace meshauth {

constexpr const char* kRequiredText = "Mesh authentication required.";
constexpr const char* kLoginPath = "/portal-login";

// "Authorization: Bearer <agent token>" (case-insensitive scheme).
inline bool bearerOk(const http::Ctx& ctx)
{
    std::string h = ctx.req.headers.get("authorization");
    if (h.size() < 8 || !ctx.config) return false;
    std::string scheme = h.substr(0, 7);
    for (auto& ch : scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (scheme != "bearer ") return false;
    std::string_view tok(h);
    tok.remove_prefix(7);
    while (!tok.empty() && tok.front() == ' ') tok.remove_prefix(1);
    return agentTokenMatches(tok, *ctx.config);
}

inline bool ok(const http::Ctx& ctx)
{
    if (!ctx.isWan()) return true;
    if (agentTokenOk(ctx) || bearerOk(ctx)) return true;
    auto p = svcGet<IMeshAuthProvider>();
    return p && (p->sessionOk(ctx) || p->basicOk(ctx));
}

// Writes 401 {"error":"Mesh authentication required.","login":"/portal-login"} and returns false when !ok(). The
// "login" field is the UI's cue to open the login page; no other 401 carries it.
inline bool require(http::Ctx& ctx)
{
    if (ok(ctx)) return true;
    ctx.json(401, JVal::object({{"error", JVal(kRequiredText)}, {"login", JVal(kLoginPath)}}));
    return false;
}

} // namespace meshauth
} // namespace anfs
