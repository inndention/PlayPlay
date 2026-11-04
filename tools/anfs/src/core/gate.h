// core/gate.h - WAN password gate, PBKDF2 password records and the agent-token check. FROZEN (ANFS-F0); implemented
// by ANFS-F6 (core/gate.cpp). The srv-wan module wires the middleware and owns the route lists.
//
// Gate (C# Gate.cs, wan-gate.js):
// - Only WAN clients on wan.gatedPaths are gated; loopback, LAN and tailnet pass.
// - wan.requireTls and not secure: 403.
// - An agentExempt route passes with the agent token in x-agent-auth.
// - 20 failures per client IP within 10 min lock that IP out for 10 min: 429 + Retry-After (seconds).
// - Credentials: HTTP Basic, password checked against the PBKDF2-SHA256 record; a verified Authorization header is
//   cached 10 min per client (keyed by HMAC-SHA256 of the header with a per-process random key, never the header).
// - No record configured: fail closed (401 with a challenge); the server still starts (OQ7).
// - Times come from anfs::clock() (tests use a ManualClock).
#pragma once

#include "core/config.h"
#include "core/http_router.h"
#include "core/log.h"
#include "core/net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anfs {

struct GateResult {
    bool allowed = false;
    int status = 0;                               // 0 when allowed
    std::string reason;
    bool challenge = false;                       // send WWW-Authenticate: Basic realm="AN-FS"
    int retryAfterSec = 0;                        // 429
    bool viaAgentToken = false;
    bool viaCache = false;

    static GateResult allow(std::string why) { GateResult r; r.allowed = true; r.reason = std::move(why); return r; }
    static GateResult deny(int status, std::string why, bool challenge)
    {
        GateResult r; r.status = status; r.reason = std::move(why); r.challenge = challenge; return r;
    }
};

class Gate {
public:
    static constexpr int kMaxFailures = 20;
    static constexpr int64_t kLockoutMs = 10 * 60 * 1000;
    static constexpr int64_t kCacheMs = 10 * 60 * 1000;
    static constexpr const char* kRealm = "AN-FS";

    explicit Gate(std::function<std::shared_ptr<const Config>()> config, Log* log = nullptr);
    ~Gate();
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    GateResult decide(std::string_view method, std::string_view path, const net::IpAddr& client,
                      std::string_view authorizationHeader, std::string_view agentHeader, bool routeAgentExempt,
                      bool secure);
    static bool isGated(const Config& c, std::string_view method, std::string_view path);
    bool isLockedOut(const net::IpAddr& client) const;
    void reset();                                 // forget failures, lockouts and the credential cache

    // The middleware srv-wan installs: decide() for the request, else the deny answer (Node wan-gate.js texts,
    // WWW-Authenticate on a challenge, Retry-After on 429). Open-flagged routes are never gated.
    http::Middleware middleware();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// PBKDF2-HMAC-SHA256 (BCryptDeriveKeyPBKDF2).
std::vector<uint8_t> pbkdf2Sha256(std::string_view password, const std::vector<uint8_t>& salt, int64_t iterations,
                                  size_t keyBytes);
// C# PasswordRecord.Verify: base64 salt/hash, or a 64-hex hash with a hex salt; iterations above 20,000,000 refused.
bool verifyPassword(const PasswordRecord& record, std::string_view password);
// `anfs hash-password`: 16 random salt bytes, 32-byte key, base64, algo "pbkdf2-sha256".
PasswordRecord makePasswordRecord(std::string_view password, int64_t iterations = 600000);
// The password of an "Authorization: Basic <b64(user:pass)>" header; false when absent or malformed.
bool basicPassword(std::string_view authorizationHeader, std::string& password);

// Agent token (CONVENTIONS.md section 2): the x-agent-auth header when non-empty, else the auth query value.
std::string presentedAgentToken(const http::Ctx& ctx);
// Constant-time compare against config.agentToken; false when either side is empty. Never logs either value.
bool agentTokenMatches(std::string_view presented, const Config& config);
inline bool agentTokenOk(const http::Ctx& ctx) { return ctx.config && agentTokenMatches(presentedAgentToken(ctx), *ctx.config); }

} // namespace anfs
