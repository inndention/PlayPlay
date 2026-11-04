// core/http_router.cpp - route table and middleware chain, a port of C# HttpHost.Map / Resolve / Run and
// PathPattern.Match (HttpHost.cs:28-76, 453-516, 624-740) without the 405 (Express falls through to the fallback).
//
// Delivered early by ANFS-F4a so the server core links; ANFS-F4b checked it line by line against the C# source and
// covers scoring, the tie rule, fallbacks, flags, middleware and the copy-on-write table in test_anfs_core_http.
// Literal segments compare ASCII-case-insensitively (C# OrdinalIgnoreCase would also fold e.g. U+0131 to 'I'; route
// literals are ASCII, so only that hostile spelling differs, and it now misses, as it does in Express).
#include "core/http_router.h"

#include "core/http_internal.h"
#include "core/json.h"

#include <algorithm>

namespace anfs::http {

struct Router::Table {
    std::vector<std::shared_ptr<const Route>> routes;                 // registration order
    std::vector<std::pair<std::shared_ptr<const Middleware>, std::string>> mw;   // chain order, owner
    std::shared_ptr<const Handler> fallback;
};

std::vector<std::string_view> splitPath(std::string_view path)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        if (j > i) out.push_back(path.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool matchPattern(std::string_view pattern, std::string_view path, Params* params, int* score)
{
    int sc = 0;
    std::vector<std::string_view> ps = splitPath(pattern), xs = splitPath(path);
    Params local;
    auto put = [&](std::string name, std::string value) {
        // C# Dictionary indexer (OrdinalIgnoreCase): a repeated name keeps its first slot and takes the last value.
        for (auto& kv : local)
            if (detail::iequals(kv.first, name)) { kv.second = std::move(value); return; }
        local.emplace_back(std::move(name), std::move(value));
    };
    for (size_t i = 0; i < ps.size(); ++i) {
        std::string_view p = ps[i];
        bool rest = p == "*" || (p.size() >= 3 && p.substr(0, 2) == "{*" && p.back() == '}');
        if (rest) {
            if (i != ps.size() - 1) return false;
            if (p != "*") {
                std::string joined;
                for (size_t k = i; k < xs.size(); ++k) {
                    if (k > i) joined += '/';
                    joined += urlDecode(xs[k], false);
                }
                put(std::string(p.substr(2, p.size() - 3)), std::move(joined));
            }
            sc += 1;
            if (params) *params = std::move(local);
            if (score) *score = sc;
            return true;
        }
        if (i >= xs.size()) return false;
        if (p.size() > 2 && p.front() == '{' && p.back() == '}') {
            put(std::string(p.substr(1, p.size() - 2)), urlDecode(xs[i], false));
            sc += 2;
        } else {
            if (!detail::iequals(p, urlDecode(xs[i], false))) return false;
            sc += 3;
        }
    }
    if (ps.size() != xs.size()) return false;
    if (params) *params = std::move(local);
    if (score) *score = sc;
    return true;
}

Router::Router() : table_(std::make_shared<const Table>()) {}
Router::~Router() = default;

void Router::map(std::string method, std::string pattern, Handler h, uint32_t flags, std::string owner)
{
    for (char& c : method)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    std::lock_guard<std::mutex> lk(mu_);
    auto r = std::make_shared<Route>();
    r->method = std::move(method);
    r->pattern = std::move(pattern);
    r->handler = std::move(h);
    r->flags = flags;
    r->owner = std::move(owner);
    r->seq = ++seq_;
    auto t = std::make_shared<Table>(*table_);
    t->routes.push_back(std::move(r));
    table_ = std::move(t);
}

void Router::get(std::string p, Handler h, uint32_t f, std::string o) { map("GET", std::move(p), std::move(h), f, std::move(o)); }
void Router::post(std::string p, Handler h, uint32_t f, std::string o) { map("POST", std::move(p), std::move(h), f, std::move(o)); }
void Router::put(std::string p, Handler h, uint32_t f, std::string o) { map("PUT", std::move(p), std::move(h), f, std::move(o)); }
void Router::del(std::string p, Handler h, uint32_t f, std::string o) { map("DELETE", std::move(p), std::move(h), f, std::move(o)); }
void Router::all(std::string p, Handler h, uint32_t f, std::string o) { map("*", std::move(p), std::move(h), f, std::move(o)); }

void Router::use(Middleware mw, std::string owner)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto t = std::make_shared<Table>(*table_);
    t->mw.emplace_back(std::make_shared<const Middleware>(std::move(mw)), std::move(owner));
    table_ = std::move(t);
}

void Router::useFirst(Middleware mw, std::string owner)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto t = std::make_shared<Table>(*table_);
    t->mw.insert(t->mw.begin(), {std::make_shared<const Middleware>(std::move(mw)), std::move(owner)});
    table_ = std::move(t);
}

size_t Router::removeOwner(std::string_view owner)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto t = std::make_shared<Table>(*table_);
    size_t before = t->routes.size() + t->mw.size();
    t->routes.erase(std::remove_if(t->routes.begin(), t->routes.end(),
                                   [&](const std::shared_ptr<const Route>& r) { return r->owner == owner; }),
                    t->routes.end());
    t->mw.erase(std::remove_if(t->mw.begin(), t->mw.end(), [&](const auto& m) { return m.second == owner; }),
                t->mw.end());
    size_t removed = before - (t->routes.size() + t->mw.size());
    table_ = std::move(t);
    return removed;
}

void Router::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    table_ = std::make_shared<const Table>();
}

void Router::setFallback(Handler h)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto t = std::make_shared<Table>(*table_);
    t->fallback = h ? std::make_shared<const Handler>(std::move(h)) : nullptr;
    table_ = std::move(t);
}

static RouteMatch resolveIn(const std::vector<std::shared_ptr<const Route>>& rs, std::string_view method,
                            std::string_view path)
{
    RouteMatch best, bestFb;
    int bestScore = -1, bestFbScore = -1;
    bool pathMatched = false;
    for (const auto& r : rs) {
        Params p;
        int score = 0;
        if (!matchPattern(r->pattern, path, &p, &score)) continue;
        pathMatched = true;
        bool methodOk = r->method == "*" || r->method == method || (method == "HEAD" && r->method == "GET");
        if (!methodOk) continue;
        if (r->method == method) score += 1;   // an exact method beats HEAD->GET and "*"
        // strictly greater: on a tie the route registered first keeps the win
        if (r->fallback()) {
            if (score > bestFbScore) { bestFb.route = r; bestFb.params = std::move(p); bestFbScore = score; }
        } else if (score > bestScore) {
            best.route = r;
            best.params = std::move(p);
            bestScore = score;
        }
    }
    RouteMatch out = best.route ? std::move(best) : std::move(bestFb);
    out.pathMatched = pathMatched;
    return out;
}

RouteMatch Router::resolve(std::string_view method, std::string_view path) const
{
    std::shared_ptr<const Table> t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = table_;
    }
    return resolveIn(t->routes, method, path);
}

void Router::dispatch(Ctx& ctx) const
{
    std::shared_ptr<const Table> t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = table_;                             // one consistent table for the whole request
    }
    RouteMatch m = resolveIn(t->routes, ctx.req.method, ctx.req.path);
    ctx.route = m.route.get();
    ctx.params = std::move(m.params);
    // The chain runs iteratively deep only as far as middlewares call next(); each step is one std::function frame.
    std::function<void(size_t)> run = [&](size_t i) {
        if (i < t->mw.size()) {
            const Middleware& mw = *t->mw[i].first;
            mw(ctx, [&run, i]() { run(i + 1); });
            return;
        }
        if (m.route) {
            m.route->handler(ctx);
            return;
        }
        if (t->fallback) {
            (*t->fallback)(ctx);
            return;
        }
        ctx.error(404, "Not found");
    };
    run(0);
}

std::vector<std::shared_ptr<const Route>> Router::routes() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return table_->routes;
}

size_t Router::middlewareCount() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return table_->mw.size();
}

} // namespace anfs::http
