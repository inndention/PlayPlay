// core/module.cpp - module registry, ModeManager and the single-instance mutex (ANFS-F7). Port of C# ModeManager.cs
// and Program.cs:131-148 onto the frozen core/module.h.
//
// Apply order (one generation per mode|port|bindAll|bindAddress key): the old generation is torn down first (modules
// in reverse start order, then the HTTP server, then the database), the new server is bound BEFORE any module starts
// (a bind failure then costs no module start and no database open, and ctx.port carries the real port for port 0),
// then the modules start in (order, seq). A bind failure rolls back by rebuilding the last good configuration and
// records lastError; tick() retries the wanted configuration every retryMs. Per-module start errors are recorded in
// moduleStatuses() as {"error": text} and never abort the others.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/module.h"

#include "core/clock.h"
#include "core/config.h"
#include "core/cors.h"
#include "core/db.h"
#include "core/fsutil.h"
#include "core/gate.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/log.h"
#include "core/net.h"
#include "core/status.h"
#include "core/version.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace anfs {

// ---- Context --------------------------------------------------------------------------------------------------------

std::shared_ptr<const Config> Context::config() const { return store.current(); }

const char* Context::version() { return kVersion; }

// ---- ModuleRegistry -------------------------------------------------------------------------------------------------

namespace {

std::string lowerTrim(std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string out(s.substr(a, b - a));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// C# ModuleRegistry.Add splits on ',', ' ' and ';'.
std::vector<std::string> splitModes(std::string_view modes)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        const std::string m = lowerTrim(cur);
        if (!m.empty()) out.push_back(m);
        cur.clear();
    };
    for (char c : modes) {
        if (c == ',' || c == ' ' || c == ';') flush();
        else cur.push_back(c);
    }
    flush();
    return out;
}

} // namespace

bool ModuleInfo::runsIn(std::string_view mode) const
{
    const std::string m = lowerTrim(mode);
    if (m != Config::kModePeer && m != Config::kModeServer) return false;   // "off" runs nothing
    for (const std::string& want : splitModes(modes)) {
        if (want == "common" || want == m) return true;
    }
    return false;
}

ModuleRegistry& ModuleRegistry::global()
{
    static ModuleRegistry* r = new ModuleRegistry();   // never destroyed: static registrars run before main
    return *r;
}

int ModuleRegistry::add(std::string_view modes, int order, ModuleFactory factory, const char* className)
{
    for (const std::string& m : splitModes(modes)) {
        if (m != "common" && m != Config::kModePeer && m != Config::kModeServer)
            throw std::invalid_argument("unknown module mode '" + m + "'");
    }
    ModuleInfo info;
    info.className = className ? className : "";
    info.modes = std::string(modes);
    info.order = order;
    const int seq = static_cast<int>(items_.size());
    info.seq = seq;
    info.factory = std::move(factory);
    items_.push_back(std::move(info));
    return seq;
}

std::vector<ModuleInfo> ModuleRegistry::list() const
{
    std::vector<ModuleInfo> out = items_;
    std::stable_sort(out.begin(), out.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        return a.order != b.order ? a.order < b.order : a.seq < b.seq;
    });
    return out;
}

std::vector<std::unique_ptr<Module>> ModuleRegistry::create(std::string_view mode) const
{
    std::vector<std::unique_ptr<Module>> out;
    for (const ModuleInfo& info : list()) {
        if (info.runsIn(mode) && info.factory) out.push_back(info.factory());
    }
    return out;
}

// ---- ModeManager ----------------------------------------------------------------------------------------------------

namespace {

std::string keyFor(const Config& c)
{
    return c.mode + "|" + std::to_string(c.effectivePort()) + "|" + (c.bindAll ? "true" : "false") + "|" + c.bindAddress;
}

constexpr int64_t kStopBudgetMs = 2000;

} // namespace

struct ModeManager::Impl {
    struct Running {
        std::unique_ptr<Module> module;
        std::string name;
        bool started = false;
        std::string error;
    };

    // Everything one apply built. Context holds references, so the members it refers to live in the same object.
    struct Gen {
        http::Router router;
        Gate gate;
        std::unique_ptr<http::Server> server;
        std::shared_ptr<Db> db;
        Context ctx;
        std::vector<Running> running;

        Gen(ConfigStore& store, Log& log, StatusWriter& status)
            : gate([&store] { return store.current(); }, &log), ctx{store, log, router, gate, status}
        {
        }
    };

    ConfigStore& store;
    Log& log;
    ModuleRegistry& registry;
    std::string dataDir;
    StatusWriter& status;

    mutable std::mutex mu;
    std::unique_ptr<Gen> gen;
    std::shared_ptr<const Config> lastGood;       // the last configuration that applied (or mode off)
    std::string currentKey;
    std::string mode = Config::kModeOff;
    int port = 0;
    int generation = 0;
    int64_t appliedRev = 0;
    bool failed = false;
    std::string lastError;
    int64_t lastErrorAt = 0;
    int64_t lastAttemptMono = 0;
    int retryMs = 30000;
    bool refused = false;                         // lastError came from testModeRefusal(); the running generation stayed
    bool shutDown = false;                        // shutdown() ran: the destructor must not touch the StatusWriter again

    Impl(ConfigStore& store, Log& log, ModuleRegistry& registry, std::string dataDir, StatusWriter& status)
        : store(store), log(log), registry(registry), dataDir(std::move(dataDir)), status(status)
    {
    }

    void setError(const std::string& text)
    {
        lastError = text;
        lastErrorAt = anfs::nowMs();
        log.error(text);
    }

    // Builds and binds a generation for c. Returns false (nothing left running) when the bind fails.
    bool build(const Config& c)
    {
        auto g = std::make_unique<Gen>(store, log, status);
        Context& x = g->ctx;
        x.mode = c.mode;
        x.dataDir = dataDir;
        x.logsDir = dataDir + "\\logs";
        x.tempDir = dataDir + (c.mode == Config::kModeServer ? "\\temp_uploads" : "\\temp_uploads_agent");
        x.cacheDir = dataDir + "\\cache";
        x.generation = generation + 1;

        http::ServerOptions opt;
        // The trusted-proxy client rule (F6): X-Forwarded-For counts only from a trusted peer, last hop only.
        opt.resolveClient = [](http::Request& req, const Config& cfg) {
            std::optional<net::IpAddr> peer = net::IpAddr::parse(req.peerIp);
            const net::IpAddr client =
                net::realClient(peer ? *peer : net::IpAddr::none(), req.headers.joined("x-forwarded-for"), cfg.wan.trustedProxies);
            req.clientIp = client.toString();
            req.clientClass = net::classify(client);
        };
        g->server = std::make_unique<http::Server>(g->router, [this] { return store.current(); }, &log, opt);
        x.server = g->server.get();
        try {
            g->server->start(c.effectivePort(), c.bindAll, c.bindAddress);
        } catch (const std::exception& e) {
            setError(std::string("http start failed: ") + e.what());
            return false;
        }
        x.port = g->server->port();

        // Server mode: CORS (allow-list) first, then the WAN gate; srv-wan seeds the lists and owns no middleware.
        // Peer mode: pfs-setup installs the peer CORS and the agent-token check itself.
        if (c.mode == Config::kModeServer) {
            g->router.useFirst(g->gate.middleware(), "runtime");
            g->router.useFirst(cors::serverMiddleware(), "runtime");
        }
        std::string err;
        if (!fs::makeDirs(x.tempDir, &err)) log.warn("cannot create " + x.tempDir + ": " + err);
        if (!fs::makeDirs(x.cacheDir, &err)) log.warn("cannot create " + x.cacheDir + ": " + err);
        try {
            g->db = Db::open(dataDir + "\\anfs.db");
            x.db = g->db;
        } catch (const std::exception& e) {
            setError(std::string("database open failed: ") + e.what());   // modules run without a database
        }

        for (std::unique_ptr<Module>& m : registry.create(c.mode)) {
            Running r;
            r.name = m->name() ? m->name() : "";
            r.module = std::move(m);
            try {
                r.module->start(x);
                r.started = true;
            } catch (const std::exception& e) {
                r.error = e.what();
                setError("module " + r.name + " failed to start: " + r.error);
                try { r.module->stop(); } catch (...) {}
            } catch (...) {
                r.error = "unknown error";
                setError("module " + r.name + " failed to start");
                try { r.module->stop(); } catch (...) {}
            }
            g->running.push_back(std::move(r));
        }
        ++generation;
        gen = std::move(g);
        log.state("mode " + c.mode + " applied: port " + std::to_string(x.port) + ", " +
                  std::to_string(gen->running.size()) + " module(s), generation " + std::to_string(generation));
        return true;
    }

    // Stops modules in reverse start order (2 s budget each, logged when exceeded), then the server, then the db.
    void teardown()
    {
        std::unique_ptr<Gen> g = std::move(gen);
        if (!g) return;
        for (size_t i = g->running.size(); i-- > 0;) {
            Running& r = g->running[i];
            if (!r.started) continue;
            const int64_t t0 = anfs::monoMs();
            try {
                r.module->stop();
            } catch (const std::exception& e) {
                log.error("module " + r.name + " failed to stop: " + e.what());
            } catch (...) {
                log.error("module " + r.name + " failed to stop");
            }
            const int64_t took = anfs::monoMs() - t0;
            if (took > kStopBudgetMs)
                log.warn("module " + r.name + " took " + std::to_string(took) + " ms to stop (budget " +
                         std::to_string(kStopBudgetMs) + " ms)");
            r.started = false;
        }
        if (g->server) {
            try { g->server->stop(); } catch (const std::exception& e) { log.error(std::string("http stop failed: ") + e.what()); }
        }
        g->running.clear();                       // modules die before the context they were given
        g->ctx.db.reset();
        g->db.reset();
        g->server.reset();
    }

    // The locked part of apply(); returns true when status should be rewritten (always, kept for clarity).
    void applyLocked(const std::shared_ptr<const Config>& c)
    {
        lastAttemptMono = anfs::monoMs();
        const std::string key = keyFor(*c);
        // The guard runs before the same-key shortcut: modules read store.current() live, so a refused bridge URL
        // on the running port must not be published as applied either.
        const std::string refusal = ModeManager::testModeRefusal(*c);
        if (!refusal.empty()) {
            setError("config refused: " + refusal);   // the running generation stays as it is
            refused = true;
            return;
        }
        if (key == currentKey && !failed) {
            // Same listeners: modules read store.current() per use, so a plain edit is live at once.
            appliedRev = c->rev;
            lastGood = c;
            if (refused) {                        // a same-port correction of a refused config clears its error
                lastError.clear();
                lastErrorAt = 0;
                refused = false;
            }
            return;
        }
        if (!currentKey.empty())
            log.state("mode change: " + mode + " -> " + c->mode + " (port " + std::to_string(c->effectivePort()) + ")");
        teardown();
        mode = c->mode;
        port = c->effectivePort();
        lastError.clear();
        lastErrorAt = 0;
        failed = false;
        refused = false;
        currentKey = key;
        if (c->mode == Config::kModeOff || build(*c)) {
            appliedRev = c->rev;
            lastGood = c;
            return;
        }
        // Bind failed: roll back to the last good configuration (it owned its port until a moment ago).
        failed = true;
        const std::string err = lastError;
        const int64_t errAt = lastErrorAt;
        if (lastGood && keyFor(*lastGood) != key) {
            log.state("rolling back to mode " + lastGood->mode + " (port " + std::to_string(lastGood->effectivePort()) + ")");
            if (lastGood->mode == Config::kModeOff || build(*lastGood)) {
                mode = lastGood->mode;
                port = lastGood->effectivePort();
                currentKey = keyFor(*lastGood);
            } else {
                currentKey.clear();               // nothing runs; the next apply rebuilds from scratch
            }
            lastError = err;                      // the wanted configuration's failure stays visible
            lastErrorAt = errAt;
        }
    }
};

ModeManager::ModeManager(ConfigStore& store, Log& log, ModuleRegistry& registry, std::string dataDir, StatusWriter& status)
    : d_(std::make_unique<Impl>(store, log, registry, std::move(dataDir), status))
{
}

// The destructor only tears the generation down: the StatusWriter handed to the constructor may already be gone by
// now (it is usually declared after the manager so the status builder can capture it), so no status write here.
ModeManager::~ModeManager()
{
    std::lock_guard<std::mutex> lk(d_->mu);
    if (d_->shutDown) return;
    d_->teardown();
    d_->shutDown = true;
}

void ModeManager::apply(std::shared_ptr<const Config> config)
{
    if (!config) return;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->applyLocked(config);
    }
    // Outside the lock: the status builder reads this manager.
    try { d_->status.writeNow(); } catch (...) {}
}

void ModeManager::tick()
{
    bool retry = false;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        retry = d_->failed && anfs::monoMs() - d_->lastAttemptMono >= d_->retryMs;
    }
    if (retry) apply(d_->store.current());
}

void ModeManager::shutdown()
{
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->shutDown) return;                 // idempotent: a second call never touches the writer again
        d_->teardown();
        d_->currentKey.clear();
        d_->mode = Config::kModeOff;
        d_->port = 0;
        d_->shutDown = true;
    }
    try { d_->status.writeNow(); } catch (...) {}
}

std::string ModeManager::mode() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->mode; }
int ModeManager::port() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->port; }
int ModeManager::generation() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->generation; }
int64_t ModeManager::appliedRev() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->appliedRev; }
bool ModeManager::failed() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->failed; }
std::string ModeManager::lastError() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->lastError; }
int64_t ModeManager::lastErrorAtMs() const { std::lock_guard<std::mutex> lk(d_->mu); return d_->lastErrorAt; }
void ModeManager::setRetryMs(int ms) { std::lock_guard<std::mutex> lk(d_->mu); d_->retryMs = std::max(ms, 0); }

std::vector<std::string> ModeManager::moduleNames() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    std::vector<std::string> out;
    if (d_->gen)
        for (const Impl::Running& r : d_->gen->running)
            if (r.started) out.push_back(r.name);
    return out;
}

JVal ModeManager::moduleStatuses() const
{
    // Held while status() runs: a module's status is a few counters, and the generation must not die under it.
    std::lock_guard<std::mutex> lk(d_->mu);
    JVal out = JVal::object();
    if (!d_->gen) return out;
    for (const Impl::Running& r : d_->gen->running) {
        if (!r.started) {
            out.set(r.name, JVal::object({{"error", r.error}}));
            continue;
        }
        try {
            out.set(r.name, r.module->status());
        } catch (const std::exception& e) {
            out.set(r.name, JVal::object({{"error", std::string("status: ") + e.what()}}));
        } catch (...) {
            out.set(r.name, JVal::object({{"error", "status failed"}}));
        }
    }
    return out;
}

http::Server* ModeManager::server() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->gen ? d_->gen->server.get() : nullptr;
}

std::string ModeManager::testModeRefusal(const Config& c)
{
    if (!c.test || c.mode == Config::kModeOff) return "";
    auto livePort = [](int p) { return p == 8080 || p == 8899 || p == 443; };
    const int p = c.effectivePort();
    if (livePort(p)) return "test mode refuses port " + std::to_string(p) + " (a live AN-FS port)";
    if (c.tlsPort > 0 && livePort(c.tlsPort)) return "test mode refuses TLS port " + std::to_string(c.tlsPort) + " (a live AN-FS port)";
    if (c.mode == Config::kModePeer && !c.rawBool("allowRemoteBridge", false)) {
        for (const std::string& u : c.bridgeUrls) {
            std::optional<net::Url> url = net::parseUrl(u);
            if (!url) return "test mode refuses the bridge URL '" + u + "' (not a valid http(s) URL)";
            if (!net::isLoopbackHost(url->host))
                return "test mode refuses the non-loopback bridge " + url->origin() + " (set allowRemoteBridge:true to allow it)";
        }
    }
    return "";
}

// ---- single instance ------------------------------------------------------------------------------------------------

namespace {

HANDLE g_singleInstance = nullptr;

// .NET ToLowerInvariant: LCMapStringEx with the invariant locale on the UTF-16 form.
std::string invariantLower(const std::string& utf8)
{
    std::wstring w = fs::widen(utf8);
    if (w.empty()) return {};
    std::wstring out(w.size() * 2 + 8, L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.c_str(), static_cast<int>(w.size()),
                                out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n <= 0) {
        for (wchar_t& c : w) c = static_cast<wchar_t>(towlower(c));
        return fs::narrow(w);
    }
    out.resize(static_cast<size_t>(n));
    return fs::narrow(out);
}

} // namespace

bool acquireSingleInstance(const std::string& dataDir, std::string* mutexName)
{
    std::string id = ids::sha1Hex(invariantLower(dataDir)).substr(0, 16);
    for (char& c : id) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));   // BitConverter.ToString form
    const std::string name = "Global\\anfs-" + id;
    if (mutexName) *mutexName = name;
    if (g_singleInstance) return true;            // already ours
    HANDLE h = CreateMutexW(nullptr, TRUE, fs::widen(name).c_str());
    const DWORD err = GetLastError();
    if (!h) return false;                         // ERROR_ACCESS_DENIED: held by another account (C# counts it as held)
    if (err == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return false;
    }
    g_singleInstance = h;
    return true;
}

void releaseSingleInstance()
{
    if (!g_singleInstance) return;
    ReleaseMutex(g_singleInstance);
    CloseHandle(g_singleInstance);
    g_singleInstance = nullptr;
}

} // namespace anfs
