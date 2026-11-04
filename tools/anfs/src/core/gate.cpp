// core/gate.cpp - ANFS-F6: the WAN password gate (C# Gate.cs, Node wan-gate.js), PBKDF2-SHA256 password records on
// BCrypt (BCryptDeriveKeyPBKDF2, the OS implementation) and the agent-token check.
//
// Decision order (C# Gate.Decide): non-WAN client -> allow; not a gated method+path -> allow; requireTls and plain
// -> 403; agent-exempt route with the token -> allow; a live lockout -> 429 + Retry-After; no password record ->
// 401 (fail closed, the server still starts); no / malformed Basic -> 401; a cached header -> allow; else PBKDF2
// outside the lock (600,000 iterations take a few hundred ms and must not stall other requests): a hit is cached
// 10 min, a miss counts one failure, and the 20th failure within 10 min locks the client out for 10 min.
// The credential cache is keyed by HMAC-SHA256(per-process random key, header + "\n" + client), never by the header,
// so a memory dump of the cache gives nothing usable. Times come from anfs::clock() (ManualClock in tests).
// The middleware answers with the Node wan-gate.js texts and headers (text/plain, Cache-Control: no-store,
// WWW-Authenticate on 401, Retry-After on 429); HEAD is matched as GET so it cannot probe a gated GET route.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include "core/gate.h"

#include "core/clock.h"
#include "core/http_ctx.h"
#include "core/ids.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace anfs {

namespace {

constexpr const char* kText401 = "Authentication required.\n";
constexpr const char* kText403 = "HTTPS is required for remote access.\n";
constexpr const char* kText429 = "Too many failed attempts. Try again later.\n";
constexpr const char* kChallenge = "Basic realm=\"AN-FS\", charset=\"UTF-8\"";
constexpr size_t kPruneAt = 5000;

std::string_view trimView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool startsWithNoCase(std::string_view s, std::string_view prefix)
{
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

std::string clientKey(const net::IpAddr& client)
{
    const net::IpAddr n = client.normalized();
    return n.valid ? n.toString() : std::string("?");
}

// One shared HMAC-SHA256 provider handle for every derivation (algorithm handles are thread-safe and cheap to keep).
BCRYPT_ALG_HANDLE sha256HmacProvider()
{
    static BCRYPT_ALG_HANDLE h = [] {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
            alg = nullptr;
        return alg;
    }();
    return h;
}

} // namespace

// ---- PBKDF2 and records ----------------------------------------------------------------------------------------------

std::vector<uint8_t> pbkdf2Sha256(std::string_view password, const std::vector<uint8_t>& salt, int64_t iterations,
                                  size_t keyBytes)
{
    std::vector<uint8_t> out(keyBytes, 0);
    BCRYPT_ALG_HANDLE alg = sha256HmacProvider();
    if (!alg || iterations < 1 || keyBytes == 0) throw std::runtime_error("pbkdf2: bad arguments or no provider");
    static uint8_t empty = 0;   // BCrypt wants a non-null pointer even for a zero-length password or salt
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(
        alg, password.empty() ? &empty : reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), salt.empty() ? &empty : const_cast<uint8_t*>(salt.data()),
        static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(iterations), out.data(), static_cast<ULONG>(keyBytes), 0);
    if (!NT_SUCCESS(st)) throw std::runtime_error("pbkdf2: BCryptDeriveKeyPBKDF2 failed " + std::to_string(st));
    return out;
}

bool verifyPassword(const PasswordRecord& record, std::string_view password)
{
    if (!record.isSet() || record.iterations > 20000000) return false;
    const bool hexForm = record.hash.size() == 64 && hex::isHex(record.hash) && hex::isHex(record.salt);
    std::vector<uint8_t> salt, want;
    if (hexForm) {
        salt = hex::decode(record.salt);
        want = hex::decode(record.hash);
    } else if (!b64::decode(record.salt, salt) || !b64::decode(record.hash, want)) {
        return false;
    }
    if (salt.empty() || want.empty()) return false;
    std::vector<uint8_t> got;
    try {
        got = pbkdf2Sha256(password, salt, record.iterations, want.size());
    } catch (const std::exception&) {
        return false;
    }
    return constantTimeEquals(std::string_view(reinterpret_cast<const char*>(got.data()), got.size()),
                              std::string_view(reinterpret_cast<const char*>(want.data()), want.size()));
}

PasswordRecord makePasswordRecord(std::string_view password, int64_t iterations)
{
    if (iterations < 1) iterations = 600000;
    std::vector<uint8_t> salt(16);
    ids::randomBytes(salt.data(), salt.size());
    PasswordRecord r;
    r.algo = PasswordRecord::kDefaultAlgo;
    r.iterations = iterations;
    r.salt = b64::encode(salt);
    r.hash = b64::encode(pbkdf2Sha256(password, salt, iterations, 32));
    return r;
}

bool basicPassword(std::string_view authorizationHeader, std::string& password)
{
    std::string_view h = trimView(authorizationHeader);
    if (h.size() < 6 || !startsWithNoCase(h.substr(0, 5), "basic")) return false;
    if (!std::isspace(static_cast<unsigned char>(h[5]))) return false;
    const std::string_view token = trimView(h.substr(6));
    if (token.empty()) return false;
    std::vector<uint8_t> raw;
    if (!b64::decode(token, raw)) return false;
    const std::string decoded(raw.begin(), raw.end());
    const size_t colon = decoded.find(':');
    if (colon == std::string::npos) return false;
    password = decoded.substr(colon + 1);
    return true;
}

// ---- agent token -----------------------------------------------------------------------------------------------------

std::string presentedAgentToken(const http::Ctx& ctx)
{
    const std::string h = ctx.req.headers.get("x-agent-auth");
    if (!h.empty()) return h;
    return ctx.req.query.value("auth");
}

bool agentTokenMatches(std::string_view presented, const Config& config)
{
    if (presented.empty() || config.agentToken.empty()) return false;
    return constantTimeEquals(presented, config.agentToken);
}

// ---- Gate ------------------------------------------------------------------------------------------------------------

struct Gate::Impl {
    std::function<std::shared_ptr<const Config>()> config;
    Log* log = nullptr;
    std::string hmacKey;                                        // 32 random bytes, this process only
    mutable std::mutex mu;
    struct Fail { int count = 0; int64_t firstMs = 0; };
    std::unordered_map<std::string, int64_t> okCache;           // HMAC hex -> expiry (epoch ms)
    std::unordered_map<std::string, Fail> fails;                // client -> failures in the window
    std::unordered_map<std::string, int64_t> lockouts;          // client -> until (epoch ms)

    std::string cacheKey(std::string_view authHeader, const std::string& client) const
    {
        return hex::encode(ids::hmacSha256(hmacKey, std::string(authHeader) + "\n" + client));
    }

    // mu held
    void prune(int64_t now)
    {
        if (okCache.size() > kPruneAt) {
            for (auto it = okCache.begin(); it != okCache.end();) it = it->second <= now ? okCache.erase(it) : std::next(it);
            if (okCache.size() > kPruneAt) okCache.clear();
        }
        if (lockouts.size() > kPruneAt)
            for (auto it = lockouts.begin(); it != lockouts.end();) it = it->second <= now ? lockouts.erase(it) : std::next(it);
        if (fails.size() > kPruneAt) fails.clear();
    }

    GateResult decide(net::NetClass cls, std::string_view method, std::string_view pathIn, const net::IpAddr& client,
                      std::string_view authHeader, std::string_view agentHeader, bool routeAgentExempt, bool secure)
    {
        if (cls != net::NetClass::Wan) return GateResult::allow("local");
        const std::shared_ptr<const Config> c = config ? config() : nullptr;
        if (!c) return GateResult::deny(401, "no configuration", true);   // fail closed
        const size_t q = pathIn.find('?');
        const std::string_view path = q == std::string_view::npos ? pathIn : pathIn.substr(0, q);
        if (!Gate::isGated(*c, method, path)) return GateResult::allow("not gated");
        if (c->wan.requireTls && !secure) return GateResult::deny(403, "TLS required", false);
        if (routeAgentExempt && agentTokenMatches(agentHeader, *c)) {
            GateResult r = GateResult::allow("agent token");
            r.viaAgentToken = true;
            return r;
        }

        const int64_t now = nowMs();
        const std::string ck = clientKey(client);
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = lockouts.find(ck);
            if (it != lockouts.end()) {
                if (it->second > now) {
                    GateResult r = GateResult::deny(429, "too many failed attempts", false);
                    r.retryAfterSec = static_cast<int>((it->second - now + 999) / 1000);
                    return r;
                }
                lockouts.erase(it);
            }
        }

        const std::optional<PasswordRecord>& rec = c->wan.passwordRecord;
        if (!rec || !rec->isSet()) return GateResult::deny(401, "no WAN password configured", true);   // fail closed

        std::string pw;
        if (!basicPassword(authHeader, pw)) return GateResult::deny(401, "authentication required", true);

        const std::string key = cacheKey(authHeader, ck);
        {
            std::lock_guard<std::mutex> lk(mu);
            auto it = okCache.find(key);
            if (it != okCache.end()) {
                if (it->second > now) {
                    GateResult r = GateResult::allow("cached");
                    r.viaCache = true;
                    return r;
                }
                okCache.erase(it);
            }
        }

        const bool ok = verifyPassword(*rec, pw);   // the slow part, outside the lock
        std::lock_guard<std::mutex> lk(mu);
        if (ok) {
            prune(now);
            okCache[key] = now + kCacheMs;
            fails.erase(ck);
            return GateResult::allow("password");
        }
        Fail& f = fails[ck];
        if (f.count == 0 || now - f.firstMs > kLockoutMs) f = Fail{0, now};
        ++f.count;
        if (f.count >= kMaxFailures) {
            prune(now);
            lockouts[ck] = now + kLockoutMs;
            fails.erase(ck);
            if (log) log->warn("gate: locked out " + ck + " for 10 minutes after " + std::to_string(kMaxFailures) +
                               " failed attempts");
        }
        return GateResult::deny(401, "invalid credentials", true);
    }
};

Gate::Gate(std::function<std::shared_ptr<const Config>()> config, Log* log) : d_(std::make_unique<Impl>())
{
    d_->config = std::move(config);
    d_->log = log;
    d_->hmacKey.assign(32, '\0');
    ids::randomBytes(&d_->hmacKey[0], d_->hmacKey.size());
}

Gate::~Gate() = default;

GateResult Gate::decide(std::string_view method, std::string_view path, const net::IpAddr& client,
                        std::string_view authorizationHeader, std::string_view agentHeader, bool routeAgentExempt,
                        bool secure)
{
    return d_->decide(net::classify(client), method, path, client, authorizationHeader, agentHeader, routeAgentExempt,
                      secure);
}

bool Gate::isGated(const Config& c, std::string_view method, std::string_view path)
{
    for (const GateRule& r : c.wan.gatedPaths) {
        if (r.matches(method, path)) return true;
        if (method == "HEAD" && r.matches("GET", path)) return true;
    }
    return false;
}

bool Gate::isLockedOut(const net::IpAddr& client) const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    auto it = d_->lockouts.find(clientKey(client));
    return it != d_->lockouts.end() && it->second > nowMs();
}

void Gate::reset()
{
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->okCache.clear();
    d_->fails.clear();
    d_->lockouts.clear();
}

http::Middleware Gate::middleware()
{
    Impl* d = d_.get();
    return [d](http::Ctx& ctx, const http::Next& next) {
        if (ctx.route && ctx.route->open()) {
            next();
            return;
        }
        // The request's class comes from the server (resolveClient: trusted proxies + classify); without it the
        // server marks every client WAN, so an unwired runtime gates everything rather than nothing.
        const net::IpAddr client = net::IpAddr::parse(ctx.req.clientIp).value_or(net::IpAddr::none());
        const GateResult r = d->decide(ctx.req.clientClass, ctx.req.method, ctx.req.path, client,
                                       ctx.req.headers.get("authorization"), ctx.req.headers.get("x-agent-auth"),
                                       ctx.route && ctx.route->agentExempt(), ctx.req.secure);
        if (r.allowed) {
            next();
            return;
        }
        if (d->log && (r.status != 401 || r.reason == "invalid credentials"))
            d->log->warn("gate: denied " + std::string(net::className(ctx.req.clientClass)) + " " + ctx.req.clientIp +
                         " " + ctx.req.method + " " + ctx.req.path + ": " + r.reason);
        const char* body = r.status == 401 ? kText401 : r.status == 403 ? kText403 : kText429;
        ctx.res.setHeader("Cache-Control", "no-store");
        if (r.challenge) ctx.res.setHeader("WWW-Authenticate", kChallenge);
        if (r.status == 429) ctx.res.setHeader("Retry-After", std::to_string(std::max(1, r.retryAfterSec)));
        // A refused upload is not read: close instead of draining a body that may be large.
        if (ctx.body.hasBody()) ctx.res.setKeepAlive(false);
        ctx.res.send(r.status, "text/plain; charset=utf-8", body);
    };
}

} // namespace anfs
