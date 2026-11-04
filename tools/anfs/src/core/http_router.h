// core/http_router.h - routes and the middleware chain (port of C# HttpHost.Map / PathPattern.Match). FROZEN
// (ANFS-F0); implemented by ANFS-F4b (core/http_router.cpp).
//
// Matching (HttpHost.cs:43-76, 624-650):
// - Pattern segments: literal (score 3, case-insensitive, compared with the percent-decoded path segment),
//   {name} (score 2), {*name} or a trailing * (score 1, the rest of the path joined with '/').
// - An exact method match adds 1 (beats "*" and HEAD-runs-GET). The best score wins; on a tie the FIRST registered
//   route wins, which is why modules register in their ANFS_MODULE order.
// - Fallback-flagged routes match only when no regular route does.
// - Unlike C#, there is no 405: Express falls through, so an unmatched method reaches the fallback handler and then
//   the default 404 {"error":"Not found"}.
// The route table is copy-on-write: map()/removeOwner() while requests run are safe, and a request sees one
// consistent table.
#pragma once

#include "core/http_ctx.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anfs::http {

using Handler = std::function<void(Ctx&)>;
using Next = std::function<void()>;
using Middleware = std::function<void(Ctx&, const Next&)>;

enum RouteFlag : uint32_t {
    kRouteNone = 0,
    kRouteAgentExempt = 1u << 0,   // the x-agent-auth token may bypass the WAN gate here (AGENT_EXEMPT_ROUTES)
    kRouteOpen = 1u << 1,          // never gated (OPEN_ROUTES: /api/pub/*, /files/{token})
    kRouteFallback = 1u << 2,      // matches only when no regular route matches
};

struct Route {
    std::string method;            // upper-case, or "*" for any
    std::string pattern;
    Handler handler;
    uint32_t flags = kRouteNone;
    std::string owner;             // module name, for removeOwner()
    int seq = 0;                   // registration order
    bool agentExempt() const { return (flags & kRouteAgentExempt) != 0; }
    bool open() const { return (flags & kRouteOpen) != 0; }
    bool fallback() const { return (flags & kRouteFallback) != 0; }
};

using Params = std::vector<std::pair<std::string, std::string>>;

// Splits on '/', dropping empty segments.
std::vector<std::string_view> splitPath(std::string_view path);
// True when pattern matches path; fills params (decoded) and score when given.
bool matchPattern(std::string_view pattern, std::string_view path, Params* params, int* score);

struct RouteMatch {
    std::shared_ptr<const Route> route;           // null when nothing matched the method and path
    Params params;
    bool pathMatched = false;                     // some route matched the path with another method
};

class Router {
public:
    Router();
    ~Router();
    Router(const Router&) = delete;
    Router& operator=(const Router&) = delete;

    void map(std::string method, std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});
    void get(std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});
    void post(std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});
    void put(std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});
    void del(std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});
    void all(std::string pattern, Handler h, uint32_t flags = kRouteNone, std::string owner = {});   // method "*"

    // Appends to the chain (runs in registration order, before the route handler). A middleware that does not call
    // next() ends the request (it must have answered).
    void use(Middleware mw, std::string owner = {});
    // Puts mw first (the CORS / gate middleware installed by pfs-setup or srv-wan).
    void useFirst(Middleware mw, std::string owner = {});
    // Drops every route and middleware registered with this owner (a stopping module); returns how many.
    size_t removeOwner(std::string_view owner);
    void clear();

    // GET/HEAD (and any method) that no route handles. Default: 404 {"error":"Not found"}.
    void setFallback(Handler h);

    RouteMatch resolve(std::string_view method, std::string_view path) const;
    // Resolves, sets ctx.route/params, runs the middleware chain, then the handler or the fallback.
    void dispatch(Ctx& ctx) const;

    std::vector<std::shared_ptr<const Route>> routes() const;
    size_t middlewareCount() const;

private:
    struct Table;
    mutable std::mutex mu_;
    std::shared_ptr<const Table> table_;
    int seq_ = 0;
};

} // namespace anfs::http
