// core/cors.cpp - ANFS-F6: the two CORS flavours.
//
// Peer (agent.js:431-443): reflect the Origin with Access-Control-Allow-Credentials: true, else "*"; OPTIONS is
// Express sendStatus(200) ("OK", text/plain).
// Server (wan-gate.js corsMiddleware + deviation D1): the Origin is reflected with credentials only when allowed by
// corsOriginAllowed (wyat.getstarnews.com and subdomains, localhost and *.localhost, hosts that are non-WAN IPs) or
// by a config.corsAllow entry (C# Cors.EntryMatches forms: "https://host[:port]", "host", "*.suffix", "host:port");
// otherwise "*" without credentials. Vary: Origin whenever an Origin was sent. OPTIONS is 204 with no body (D1).
#include "core/cors.h"

#include "core/net.h"

#include <cctype>

namespace anfs::cors {

namespace {

constexpr const char* kMethods = "GET, POST, PUT, DELETE, OPTIONS";
constexpr const char* kHeaders = "Content-Type, Authorization, x-agent-auth, x-upload-id, x-chunk-index";
constexpr const char* kSiteHost = "wyat.getstarnews.com";

std::string lowerAscii(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trimView(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// wan-gate.js corsOriginAllowed.
bool siteOriginAllowed(const net::Url& u)
{
    std::string host = u.host;                       // parseUrl lower-cases it and strips the brackets
    if (!host.empty() && host.back() == '.') host.pop_back();
    if (host == kSiteHost || endsWith(host, std::string(".") + kSiteHost)) return true;
    if (host == "localhost" || endsWith(host, ".localhost")) return true;
    const std::optional<net::IpAddr> ip = net::IpAddr::parse(host);
    return ip && net::classify(*ip) != net::NetClass::Wan;
}

// C# Cors.EntryMatches: one config.corsAllow entry against a parsed origin.
bool entryMatches(std::string_view entryText, const net::Url& u)
{
    std::string entry = lowerAscii(trimView(entryText));
    while (!entry.empty() && entry.back() == '/') entry.pop_back();
    if (entry.empty() || entry == "*") return false;
    if (entry.find("://") != std::string::npos) return entry == u.origin();
    const bool bracketed = u.host.find(':') != std::string::npos;
    const std::string hostPart = bracketed ? "[" + u.host + "]" : u.host;
    if (entry.size() > 2 && entry[0] == '*' && entry[1] == '.') return endsWith(u.host, entry.substr(1));
    const size_t colon = entry.find(':');
    if (colon != std::string::npos && colon > 0 && colon == entry.rfind(':')) {
        const int def = u.scheme == "https" ? 443 : 80;
        const std::string authority = u.port == def ? hostPart : hostPart + ":" + std::to_string(u.port);
        return entry == authority;
    }
    return entry == u.host;
}

void addCommon(HeaderList& h)
{
    h.emplace_back("Access-Control-Allow-Methods", kMethods);
    h.emplace_back("Access-Control-Allow-Headers", kHeaders);
}

} // namespace

HeaderList peerHeaders(std::string_view origin)
{
    HeaderList h;
    if (!origin.empty()) {
        h.emplace_back("Access-Control-Allow-Origin", std::string(origin));
        h.emplace_back("Access-Control-Allow-Credentials", "true");
    } else {
        h.emplace_back("Access-Control-Allow-Origin", "*");
    }
    addCommon(h);
    return h;
}

bool serverOriginAllowed(std::string_view origin, const std::vector<std::string>& extraAllow)
{
    const std::optional<net::Url> u = net::parseUrl(origin);
    if (!u) return false;
    if (siteOriginAllowed(*u)) return true;
    for (const std::string& e : extraAllow)
        if (entryMatches(e, *u)) return true;
    return false;
}

HeaderList serverHeaders(std::string_view origin, const std::vector<std::string>& extraAllow)
{
    HeaderList h;
    if (!origin.empty() && serverOriginAllowed(origin, extraAllow)) {
        h.emplace_back("Access-Control-Allow-Origin", std::string(origin));
        h.emplace_back("Access-Control-Allow-Credentials", "true");
        h.emplace_back("Vary", "Origin");
    } else {
        h.emplace_back("Access-Control-Allow-Origin", "*");
        if (!origin.empty()) h.emplace_back("Vary", "Origin");
    }
    addCommon(h);
    return h;
}

http::Middleware peerMiddleware()
{
    return [](http::Ctx& ctx, const http::Next& next) {
        for (auto& [name, value] : peerHeaders(ctx.req.headers.get("origin"))) ctx.res.setHeader(name, value);
        if (ctx.req.method == "OPTIONS") {
            ctx.res.send(200, "text/plain; charset=utf-8", "OK");
            return;
        }
        next();
    };
}

http::Middleware serverMiddleware()
{
    return [](http::Ctx& ctx, const http::Next& next) {
        static const std::vector<std::string> none;
        const std::vector<std::string>& extra = ctx.config ? ctx.config->corsAllow : none;
        for (auto& [name, value] : serverHeaders(ctx.req.headers.get("origin"), extra)) ctx.res.setHeader(name, value);
        if (ctx.req.method == "OPTIONS") {
            ctx.res.sendEmpty(204);
            return;
        }
        next();
    };
}

} // namespace anfs::cors
