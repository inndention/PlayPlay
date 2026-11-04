// core/status.cpp - status.json writer and the standard status document (ANFS-F7). Port of C# Status.cs onto the
// frozen core/status.h: a 2 s timer on anfs::clock(), atomic writes, extras merged into every write, the first failed
// write logged until the next success. buildStatus() follows the field order of status.h (C# Status.Build plus the
// mgmt-integration.md keys: ver, statusPort, uptimeS, rev, appliedRev, bridge, lastErrorTs, config).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/status.h"

#include "core/clock.h"
#include "core/config.h"
#include "core/db.h"
#include "core/fsutil.h"
#include "core/http_server.h"
#include "core/log.h"
#include "core/module.h"
#include "core/svc.h"
#include "core/version.h"
#include "iface/agent_registry.h"
#include "iface/bridge_client.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace anfs {

struct StatusWriter::Impl {
    std::string path;
    std::function<JVal()> build;
    Log* log;
    std::mutex mu;                                // builder, extras
    JObject extras;
    std::mutex writeMu;                           // one write at a time (timer vs writeNow callers)
    int failed = 0;

    std::mutex thMu;
    std::condition_variable cv;
    std::thread th;
    bool stop = false;
    int intervalMs = 2000;

    Impl(std::string p, std::function<JVal()> b, Log* l) : path(std::move(p)), build(std::move(b)), log(l ? l : &nullLog()) {}
};

StatusWriter::StatusWriter(std::string path, std::function<JVal()> build, Log* log)
    : d_(std::make_unique<Impl>(std::move(path), std::move(build), log))
{
}

StatusWriter::~StatusWriter() { stop(); }

const std::string& StatusWriter::path() const { return d_->path; }

void StatusWriter::setBuilder(std::function<JVal()> build)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->build = std::move(build);
}

void StatusWriter::set(const std::string& key, JVal value)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    if (value.isNull()) d_->extras.erase(key);    // C# Set(key, null) removes
    else d_->extras.set(key, std::move(value));
}

void StatusWriter::remove(const std::string& key)
{
    std::lock_guard<std::mutex> lk(d_->mu);
    d_->extras.erase(key);
}

JVal StatusWriter::snapshot() const
{
    std::function<JVal()> build;
    JObject extras;
    {
        std::lock_guard<std::mutex> lk(d_->mu);
        build = d_->build;
        extras = d_->extras;
    }
    JVal doc;
    try {
        doc = build ? build() : JVal::object();
    } catch (const std::exception& e) {
        doc = JVal::object({{"error", std::string("status build failed: ") + e.what()}});
    }
    if (!doc.isObject()) doc = JVal::object();
    for (const auto& kv : extras) doc.set(kv.first, kv.second);
    return doc;
}

void StatusWriter::writeNow()
{
    Impl& d = *d_;
    std::lock_guard<std::mutex> lk(d.writeMu);
    const JVal doc = snapshot();
    std::string err;
    if (writeFileAtomic(d.path, json::stringifyPretty(doc, 2), &err)) {
        d.failed = 0;
        return;
    }
    if (d.failed++ == 0) d.log->warn("status write failed: " + err);
}

void StatusWriter::start(int intervalMs)
{
    Impl& d = *d_;
    stop();
    {
        std::lock_guard<std::mutex> lk(d.thMu);
        d.stop = false;
        d.intervalMs = intervalMs > 0 ? intervalMs : 2000;
    }
    writeNow();
    d.th = std::thread([this] {
        Impl& d = *d_;
        std::unique_lock<std::mutex> lk(d.thMu);
        for (;;) {
            if (anfs::clock().waitFor(d.cv, lk, d.intervalMs, [&] { return d.stop; })) return;
            lk.unlock();
            writeNow();
            lk.lock();
        }
    });
}

void StatusWriter::stop()
{
    Impl& d = *d_;
    {
        std::lock_guard<std::mutex> lk(d.thMu);
        d.stop = true;
    }
    d.cv.notify_all();
    if (d.th.joinable()) {
        if (d.th.get_id() == std::this_thread::get_id()) d.th.detach();   // stop() from the timer thread itself
        else d.th.join();
    }
}

// ---- the standard document ---------------------------------------------------------------------------------------

namespace {

JVal isoOrNull(int64_t ms) { return ms > 0 ? JVal(json::isoMs(ms)) : JVal(); }

} // namespace

JVal buildStatus(const ModeManager& mm, const ConfigStore& store, const Log& log, int64_t startedAtMs)
{
    const std::shared_ptr<const Config> c = store.current();
    const int64_t now = anfs::nowMs();
    const int64_t uptime = startedAtMs > 0 ? std::max<int64_t>(0, (now - startedAtMs) / 1000) : 0;
    JVal d = JVal::object();
    d.set("mode", mm.mode());
    d.set("version", kVersion);
    d.set("ver", kVersion);
    d.set("pid", static_cast<int64_t>(GetCurrentProcessId()));
    d.set("nodeId", c->nodeId);
    d.set("name", c->name);
    d.set("port", mm.port());
    d.set("statusPort", c->statusPort);
    http::Server* srv = mm.server();
    if (srv && srv->running()) {
        JVal l = JVal::array();
        for (const std::string& s : srv->listening()) l.push(s);
        d.set("listening", l);
    } else {
        d.set("listening", JVal());
    }
    d.set("loopbackOnly", false);                 // the C++ server never falls back to loopback (ModeManager decides)
    d.set("startedAt", isoOrNull(startedAtMs));
    d.set("updatedAt", json::isoMs(now));
    d.set("uptimeSec", uptime);
    d.set("uptimeS", uptime);
    d.set("rev", c->rev);
    d.set("appliedRev", mm.appliedRev());
    // bridge: only a peer with a running peer-link publishes IBridgeClient.
    if (auto bridge = svcGet<IBridgeClient>()) {
        if (bridge->connected()) d.set("bridge", "connected");
        else if (bridge->agentId().empty()) d.set("bridge", "connecting");
        else d.set("bridge", "down");
    } else {
        d.set("bridge", JVal());
    }
    d.set("modules", mm.moduleStatuses());
    std::string err = mm.lastError();
    int64_t errAt = mm.lastErrorAtMs();
    if (err.empty()) {
        err = log.lastError();
        errAt = log.lastErrorAtMs();
    }
    d.set("lastError", err.empty() ? JVal() : JVal(err));
    d.set("lastErrorTs", err.empty() ? JVal() : isoOrNull(errAt));
    const std::string cfgErr = store.lastError();
    d.set("configError", cfgErr.empty() ? JVal() : JVal(cfgErr));
    d.set("generation", mm.generation());
    if (srv) d.set("gate", "foundation");
    // db (while a generation runs and the file exists) and peers (srv-registry's service) as C# AddDb / AddPeers.
    {
        const std::string dataDir = [&] {
            const std::string p = store.path();
            const size_t cut = p.find_last_of("\\/");
            return cut == std::string::npos ? std::string(".") : p.substr(0, cut);
        }();
        const std::string dbPath = dataDir + "\\anfs.db";
        if (srv && fs::exists(dbPath)) {
            const fs::FileStat main = fs::stat(dbPath);
            const fs::FileStat wal = fs::stat(dbPath + "-wal");
            d.set("db", JVal::object({{"path", dbPath}, {"mode", mm.mode()}, {"sizeBytes", main.size + (wal.exists ? wal.size : 0)}}));
        }
    }
    if (auto reg = svcGet<IAgentRegistry>()) {
        JVal nodes = JVal::array();
        int64_t relayed = 0;
        for (const AgentPtr& a : reg->all()) {
            if (!a) continue;
            nodes.push(JVal::object({{"id", a->id}, {"live", reg->isOnline(a->id)}, {"relay", a->relay}}));
            if (a->relay) ++relayed;
        }
        const int64_t count = static_cast<int64_t>(nodes.size());
        d.set("peers", JVal::object({{"count", count}, {"relayed", relayed}, {"nodes", nodes}}));
    }
    d.set("config", c->toSafeJval());
    return d;
}

JVal readStatusFile(const std::string& dataDir)
{
    const std::string path = dataDir + "\\status.json";
    const fs::FileStat st = fs::stat(path);
    if (!st.exists || st.isDir) return JVal();
    std::string text, err;
    if (!fs::readFile(path, text, 16 << 20, &err))
        return JVal::object({{"running", false}, {"error", "unreadable status: " + err}});
    JVal d;
    std::string perr;
    if (!json::tryParse(text, d, &perr) || !d.isObject())
        return JVal::object({{"running", false}, {"error", "unreadable status: " + (perr.empty() ? std::string("not a JSON object") : perr)}});
    const int64_t age = std::max<int64_t>(0, (anfs::nowMs() - st.mtimeMs) / 1000);
    const int64_t pid = d.getInt("pid", 0);
    bool alive = false;
    if (pid > 0 && pid < 0x7fffffff) {
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid))) {
            DWORD code = 0;
            alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
            CloseHandle(h);
        }
    }
    d.set("ageSec", age);
    d.set("running", alive && age < 20);
    return d;
}

} // namespace anfs
