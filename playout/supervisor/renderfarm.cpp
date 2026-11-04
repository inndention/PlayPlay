#include "renderfarm.h"
#include "starclient.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "../schedule/schedulestore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <memory>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QString newId(int n = 10) { return QUuid::createUuid().toString(QUuid::Id128).left(n); }
static QString normPath(const QString& p) { return QDir::cleanPath(QDir::fromNativeSeparators(p)).toLower(); }
static bool finished(const QString& s) { return s == "done" || s == "failed" || s == "cancelled"; }
static QJsonObject withSegs(const QJsonObject& o, const QJsonArray& segs) { if (segs.isEmpty()) return o; QJsonObject x = o; x["segments"] = segs; return x; }

RenderFarm::RenderFarm(ScheduleStore* store, StarClient* star, const QString& dataDir, const QString& outDir,
                       const QString& ffmpeg, const QString& ffprobe, QObject* parent)
    : QObject(parent), m_store(store), m_star(star), m_http(new SuiteHttp(this)), m_dataDir(dataDir), m_outDir(outDir), m_ffmpeg(ffmpeg), m_ffprobe(ffprobe) {
    QDir().mkpath(m_outDir);
    m_persist.setSingleShot(true); m_notify.setSingleShot(true);
    connect(&m_persist, &QTimer::timeout, this, &RenderFarm::persist);
    connect(&m_notify, &QTimer::timeout, this, [this]() {
        QJsonObject o; QJsonArray active; for (Job* j : m_jobs) if (!finished(j->o.value("state").toString()) || nowMs() - qint64(j->o.value("finishedMs").toDouble()) < 60000) { QJsonArray sg; for (const Seg& s : j->segs) sg.append(s.o); active.append(withSegs(j->o, sg)); }
        o["jobs"] = active; o["counts"] = counts(); emit event("render", o);
    });
    connect(&m_tick, &QTimer::timeout, this, &RenderFarm::pump);
    connect(&m_healthTimer, &QTimer::timeout, this, [this]() { probeAll([](const QJsonObject&) {}); });
    if (m_star) connect(m_star, &StarClient::fetched, this, [this](const QString& localPath, const QString&, const QString& node, const QString& remotePath) {
        for (Job* j : m_jobs) for (int i = 0; i < j->segs.size(); ++i) {
            Seg& s = j->segs[i];
            if (s.o.value("state").toString() == "fetching" && s.o.value("worker").toString() == node && s.o.value("remoteOutput").toString() == remotePath) {
                s.o["state"] = "done"; s.o["output"] = localPath; s.o["pct"] = 100; logLine(j, QString("segment %1 fetched from %2").arg(i).arg(node)); runSegments(j); return;
            }
        }
    });
}

RenderFarm::~RenderFarm() {
    persist();
    for (Job* j : m_jobs) { if (j->proc) j->proc->kill(); for (Seg& s : j->segs) if (s.proc) s.proc->kill(); delete j; }
}

void RenderFarm::start() {
    load();
    m_tick.start(1000);
    m_healthTimer.start(60000);
    QTimer::singleShot(1500, this, [this]() { probeAll([](const QJsonObject&) {}); });
}

// =====================================================================================================
// configuration + nodes
// =====================================================================================================
QJsonObject RenderFarm::config() const {
    QJsonObject c = m_store->settingJson("render.config");
    auto def = [&c](const char* k, const QJsonValue& v) { if (!c.contains(k)) c[k] = v; };
    def("studioMode", "prefer"); def("renderMode", "offline"); def("waitSec", 1200); def("maxLocal", 2); def("meshDispatch", "auto");
    def("allowCustomArgs", false); def("discoverLocal", true); def("place", "Martinsville, VA");
    return c;
}

QJsonObject RenderFarm::setConfig(const QJsonObject& patch) {
    QJsonObject c = m_store->settingJson("render.config");
    static const QStringList keys = {"studioMode", "renderMode", "waitSec", "maxLocal", "meshDispatch", "allowCustomArgs", "discoverLocal", "place"};
    for (auto it = patch.begin(); it != patch.end(); ++it) if (keys.contains(it.key())) c[it.key()] = it.value();
    if (!QStringList{"off", "prefer", "only"}.contains(c.value("studioMode").toString("prefer"))) c["studioMode"] = "prefer";
    if (!QStringList{"offline", "hybrid"}.contains(c.value("renderMode").toString("offline"))) c["renderMode"] = "offline";
    if (!QStringList{"auto", "local", "off"}.contains(c.value("meshDispatch").toString("auto"))) c["meshDispatch"] = "auto";
    c["maxLocal"] = qBound(1, c.value("maxLocal").toInt(2), 8); c["waitSec"] = qBound(60, c.value("waitSec").toInt(1200), 7200);
    m_store->setSettingJson("render.config", c);
    return config();
}

QJsonArray RenderFarm::configuredNodes() const { return m_store->settingJson("render.nodes").value("nodes").toArray(); }

QJsonObject RenderFarm::discoveredSpotNode() const {
    if (!config().value("discoverLocal").toBool(true)) return {};
    const QString home = m_star && !m_star->starHome().isEmpty() ? m_star->starHome() : QString("D:/StarMedia");
    if (!QFileInfo::exists(QDir(home).filePath("mesh/token"))) return {};
    QJsonObject n; n["id"] = "spotnode-local"; n["name"] = "spot-node (this PC)"; n["kind"] = "spotnode"; n["url"] = "http://127.0.0.1:8775";
    n["enabled"] = true; n["render"] = true; n["discovered"] = true; n["tokenFile"] = QDir(home).filePath("mesh/token"); n["maxBusy"] = 20;
    return n;
}

QString RenderFarm::tokenFor(const QJsonObject& n) const {
    if (!n.value("token").toString().isEmpty()) return n.value("token").toString();
    const QString tf = n.value("tokenFile").toString();
    if (!tf.isEmpty()) { QFile f(tf); if (f.open(QIODevice::ReadOnly)) return QString::fromUtf8(f.readAll()).trimmed(); }
    return {};
}

QList<QPair<QByteArray, QByteArray>> RenderFarm::authHeaders(const QJsonObject& n) const {
    const QByteArray t = tokenFor(n).toUtf8(); QList<QPair<QByteArray, QByteArray>> h;
    if (t.isEmpty()) return h;
    h.append({"Authorization", "Bearer " + t});
    if (n.value("kind").toString() == "playplay") { h.append({"X-PP-Key", t}); h.append({"X-PP-Pass", t}); }
    return h;
}

QJsonArray RenderFarm::nodes(bool withSecrets) const {
    QJsonArray out; QSet<QString> urls;
    QJsonObject local; local["id"] = "local"; local["name"] = "This Supervisor"; local["kind"] = "local"; local["enabled"] = true; local["render"] = true; local["builtin"] = true;
    QJsonObject lh; lh["ok"] = QFileInfo::exists(m_ffmpeg) || m_ffmpeg == "ffmpeg"; lh["ready"] = true; QJsonObject q; q["running"] = localRunning(); int queued = 0;
    for (Job* j : m_jobs) if (j->o.value("state").toString() == "queued" && j->o.value("kind").toString() != "spot") ++queued;
    q["queued"] = queued; q["max"] = config().value("maxLocal").toInt(2); lh["queue"] = q; lh["ffmpeg"] = m_ffmpeg; local["health"] = lh; local["maxBusy"] = 1 << 20;
    out.append(local);
    for (const QJsonValue& v : configuredNodes()) {
        QJsonObject n = v.toObject(); urls.insert(n.value("url").toString().toLower());
        n["hasToken"] = !n.value("token").toString().isEmpty(); if (!withSecrets) n.remove("token");
        n["health"] = m_health.value(n.value("id").toString()); out.append(n);
    }
    QJsonObject d = discoveredSpotNode();
    if (!d.isEmpty() && !urls.contains(d.value("url").toString().toLower())) { d["hasToken"] = true; d["health"] = m_health.value(d.value("id").toString()); out.append(d); }
    return out;
}

QJsonObject RenderFarm::node(const QString& id, bool* found, bool withSecrets) const {
    for (const QJsonValue& v : nodes(withSecrets)) if (v.toObject().value("id").toString() == id) { if (found) *found = true; return v.toObject(); }
    if (found) *found = false; return {};
}

QJsonArray RenderFarm::nodesForChoice() const { return nodes(true); }

static QJsonObject cleanNode(QJsonObject n, const QJsonObject& prev) {
    static const QStringList keys = {"id", "name", "kind", "url", "token", "enabled", "render", "primary", "maxBusy", "notes", "starNode"};
    QJsonObject o; for (const QString& k : keys) if (n.contains(k)) o[k] = n.value(k);
    for (auto it = prev.begin(); it != prev.end(); ++it) if (!o.contains(it.key()) && keys.contains(it.key())) o[it.key()] = it.value();
    if (o.value("token").toString().isEmpty() && !prev.value("token").toString().isEmpty()) o["token"] = prev.value("token");   // blank keeps the stored token
    if (!QStringList{"spotnode", "playplay", "staragent"}.contains(o.value("kind").toString())) o["kind"] = "spotnode";
    QString url = o.value("url").toString().trimmed(); while (url.endsWith('/')) url.chop(1);
    if (!url.isEmpty() && !url.contains("://")) url = "http://" + url;
    o["url"] = url;
    if (!o.contains("enabled")) o["enabled"] = true;
    if (!o.contains("render")) o["render"] = o.value("kind").toString() != "playplay" || o.value("notes").toString().toLower().remove(' ').contains("render=true");
    if (o.value("name").toString().isEmpty()) o["name"] = QUrl(url).host().isEmpty() ? o.value("starNode").toString() : QUrl(url).host();
    return o;
}

QJsonObject RenderFarm::saveNode(const QJsonObject& body, const QString& id) {
    QJsonArray list = configuredNodes(); QJsonObject prev; int at = -1;
    const QString key = id.isEmpty() ? body.value("id").toString() : id;
    for (int i = 0; i < list.size(); ++i) if (!key.isEmpty() && list[i].toObject().value("id").toString() == key) { prev = list[i].toObject(); at = i; }
    if (!id.isEmpty() && at < 0) { QJsonObject e; e["error"] = "no such node"; e["status"] = 404; return e; }
    QJsonObject n = cleanNode(body, prev);
    if (n.value("url").toString().isEmpty() && n.value("kind").toString() != "staragent") { QJsonObject e; e["error"] = "url required"; e["status"] = 400; return e; }
    if (n.value("kind").toString() == "staragent" && n.value("starNode").toString().isEmpty()) { QJsonObject e; e["error"] = "starNode (the StarAgent node id) required"; e["status"] = 400; return e; }
    if (n.value("id").toString().isEmpty() || n.value("id").toString() == "local") n["id"] = "rn-" + newId(6);
    if (at >= 0) list[at] = n; else list.append(n);
    QJsonObject cfg; cfg["nodes"] = list; m_store->setSettingJson("render.nodes", cfg);
    changed();
    QJsonObject out = n; out["hasToken"] = !out.value("token").toString().isEmpty(); out.remove("token"); return out;
}

QJsonArray RenderFarm::setNodes(const QJsonArray& in) {
    const QJsonArray prevList = configuredNodes(); QJsonArray out;
    for (const QJsonValue& v : in) {
        const QJsonObject n = v.toObject(); if (n.value("discovered").toBool() || n.value("builtin").toBool()) continue;
        QJsonObject prev; for (const QJsonValue& p : prevList) if (p.toObject().value("id").toString() == n.value("id").toString()) prev = p.toObject();
        QJsonObject c = cleanNode(n, prev); if (c.value("id").toString().isEmpty()) c["id"] = "rn-" + newId(6); out.append(c);
    }
    QJsonObject cfg; cfg["nodes"] = out; m_store->setSettingJson("render.nodes", cfg); changed();
    return nodes();
}

bool RenderFarm::deleteNode(const QString& id) {
    QJsonArray list = configuredNodes(), out; bool hit = false;
    for (const QJsonValue& v : list) { if (v.toObject().value("id").toString() == id) { hit = true; continue; } out.append(v); }
    if (!hit) return false;
    QJsonObject cfg; cfg["nodes"] = out; m_store->setSettingJson("render.nodes", cfg); m_health.remove(id); changed();
    return true;
}

void RenderFarm::probe(const QString& id, JsonCb cb) {
    bool f = false; const QJsonObject n = node(id, &f, true);
    if (!f) { QJsonObject e; e["error"] = "no such node"; e["status"] = 404; cb(e); return; }
    const QString kind = n.value("kind").toString(), url = n.value("url").toString();
    auto t0 = std::make_shared<qint64>(nowMs());
    auto store = [this, id, cb, t0](QJsonObject h) {
        h["checkedMs"] = double(nowMs()); h["latencyMs"] = double(nowMs() - *t0); m_health[id] = h; changed();
        bool f2 = false; cb(node(id, &f2));
    };
    if (kind == "local") { store(n.value("health").toObject()); return; }
    if (kind == "spotnode") {
        m_http->json("GET", url + "/health", authHeaders(n), {}, 8000, [store](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            QJsonObject h;
            if (http != 200) { h["ok"] = false; h["ready"] = false; h["reason"] = SuiteHttp::errorText(http, j, raw, e); store(h); return; }
            const QJsonObject q = j.value("queue").toObject();
            h["ok"] = true; h["capable"] = j.value("capable").toBool(); h["queue"] = q; h["version"] = j.value("version"); h["service"] = j.value("service");
            h["ready"] = j.value("capable").toBool() && q.value("queued").toInt() < q.value("max").toInt(20);
            QStringList reasons; for (const QJsonValue& r : j.value("reasons").toArray()) reasons << r.toString(); h["reason"] = reasons.join("; ");
            QJsonObject eng; const QJsonObject e2 = j.value("engines").toObject(); for (auto it = e2.begin(); it != e2.end(); ++it) eng[it.key()] = it.value().toObject().value("ok").toBool();
            h["engines"] = eng; h["nodeName"] = j.value("node").toObject().value("name"); h["cores"] = j.value("cores"); h["scratchFree"] = j.value("scratch").toObject().value("freeBytes");
            store(h);
        });
        return;
    }
    if (kind == "playplay") {
        // C++ PlayPlay peers answer /api/v1/render/status; PlayPlay OBS peers answer /api/status (capabilities.newsroom_mesh_v2)
        m_http->json("GET", url + "/api/v1/render/status", authHeaders(n), {}, 6000, [this, n, url, store](int http, const QJsonObject& j, const QByteArray&, const QString&) {
            if (http == 200) {
                QJsonObject h; h["ok"] = true; h["meshRender"] = true; h["flavor"] = "cpp"; const QJsonObject c = j.value("counts").toObject();
                QJsonObject q; q["running"] = c.value("running").toInt(); q["queued"] = c.value("queued").toInt(); h["queue"] = q; store(h); return;
            }
            m_http->json("GET", url + "/api/status", authHeaders(n), {}, 6000, [store](int http2, const QJsonObject& s, const QByteArray& raw, const QString& e) {
                QJsonObject h;
                if (http2 != 200) { h["ok"] = false; h["reason"] = SuiteHttp::errorText(http2, s, raw, e); store(h); return; }
                h["ok"] = true; h["flavor"] = "playplay-obs"; h["meshRender"] = s.value("capabilities").toObject().value("newsroom_mesh_v2").toBool();
                h["paused"] = s.value("background_paused").toBool(); h["failoverRole"] = s.value("failover_role").toString();
                const QJsonObject mq = s.value("media_queue").toObject().value("queue").toObject();
                QJsonObject q; q["running"] = mq.value("running").toInt(); q["queued"] = mq.value("queued").toInt(); h["queue"] = q;
                if (!h.value("meshRender").toBool()) h["reason"] = "peer does not advertise newsroom_mesh_v2";
                store(h);
            });
        });
        return;
    }
    if (kind == "staragent" && m_star) {
        m_star->health(n.value("starNode").toString(), [store](int status, const QJsonObject& body) {
            QJsonObject h; h["ok"] = status == 200; if (status != 200) h["reason"] = body.value("error").toString(QString("HTTP %1").arg(status));
            else { h["version"] = body.value("data").toObject().value("version"); h["hostname"] = body.value("data").toObject().value("hostname"); }
            QJsonObject q; q["running"] = 0; q["queued"] = 0; h["queue"] = q; h["ready"] = status == 200; store(h);
        });
        return;
    }
    QJsonObject h; h["ok"] = false; h["reason"] = "unknown node kind"; store(h);
}

void RenderFarm::probeAll(JsonCb cb) {
    const QJsonArray all = nodes();
    auto left = std::make_shared<int>(all.size());
    auto doneAll = [this, cb]() { QJsonObject o; o["nodes"] = nodes(); cb(o); };
    if (*left == 0) { doneAll(); return; }
    for (const QJsonValue& v : all) probe(v.toObject().value("id").toString(), [left, doneAll](const QJsonObject&) { if (--*left == 0) doneAll(); });
}

void RenderFarm::templates(JsonCb cb) {
    if (!m_templates.isEmpty() && nowMs() - m_templatesMs < 600000) { QJsonObject o; o["templates"] = m_templates; o["node"] = m_templatesNode; o["cached"] = true; cb(o); return; }
    QJsonObject sn; for (const QJsonValue& v : nodes(true)) { const QJsonObject n = v.toObject(); if (n.value("kind").toString() == "spotnode" && n.value("enabled").toBool(true)) { sn = n; if (n.value("health").toObject().value("ok").toBool()) break; } }
    if (sn.isEmpty()) { QJsonObject o; o["templates"] = QJsonArray(); o["reason"] = "no spot-node configured"; cb(o); return; }
    m_http->json("GET", sn.value("url").toString() + "/v1/templates", authHeaders(sn), {}, 10000, [this, sn, cb](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        QJsonObject o;
        if (http != 200) { o["templates"] = m_templates; o["reason"] = SuiteHttp::errorText(http, j, raw, e); cb(o); return; }
        QJsonArray t; for (const QJsonValue& v : j.value("data").toArray()) { const QJsonObject x = v.toObject(); QJsonObject s;
            for (const char* k : {"id", "title", "category", "tags", "aspects", "lengths", "description", "videos"}) if (x.contains(k)) s[k] = x.value(k); t.append(s); }
        m_templates = t; m_templatesMs = nowMs(); m_templatesNode = sn.value("id").toString();
        o["templates"] = t; o["node"] = m_templatesNode; cb(o);
    });
}

QJsonObject RenderFarm::counts() const {
    QJsonObject c; int q = 0, r = 0, d = 0, f = 0, x = 0;
    for (Job* j : m_jobs) { const QString s = j->o.value("state").toString(); if (s == "queued") ++q; else if (s == "running") ++r; else if (s == "done") ++d; else if (s == "failed") ++f; else ++x; }
    c["queued"] = q; c["running"] = r; c["done"] = d; c["failed"] = f; c["cancelled"] = x; return c;
}

QJsonObject RenderFarm::status() const {
    QJsonObject o; o["config"] = config(); o["nodes"] = nodes(); o["counts"] = counts();
    QJsonObject l; l["ffmpeg"] = m_ffmpeg; l["ffprobe"] = m_ffprobe; l["running"] = localRunning(); l["max"] = config().value("maxLocal").toInt(2); o["local"] = l;
    o["outDir"] = m_outDir; return o;
}

// =====================================================================================================
// jobs: bookkeeping
// =====================================================================================================
RenderFarm::Job* RenderFarm::find(const QString& id) const { for (Job* j : m_jobs) if (j->o.value("id").toString() == id) return j; return nullptr; }

QJsonArray RenderFarm::jobs(const QString& state, int limit) const {
    QJsonArray a;
    for (int i = m_jobs.size() - 1; i >= 0 && a.size() < limit; --i) { const QJsonObject& o = m_jobs[i]->o; if (state.isEmpty() || o.value("state").toString() == state || (state == "active" && !finished(o.value("state").toString()))) { QJsonArray sg; for (const Seg& s : m_jobs[i]->segs) sg.append(s.o); a.append(withSegs(o, sg)); } }
    return a;
}

QJsonObject RenderFarm::job(const QString& id, bool* found) const { Job* j = find(id); if (found) *found = j != nullptr; if (!j) return {}; QJsonArray sg; for (const Seg& s : j->segs) sg.append(s.o); return withSegs(j->o, sg); }

void RenderFarm::persist() {
    QJsonArray a; const int from = qMax(0, int(m_jobs.size()) - 300);
    for (int i = from; i < m_jobs.size(); ++i) { QJsonObject o = m_jobs[i]->o; QJsonArray segs; for (const Seg& s : m_jobs[i]->segs) segs.append(s.o); if (!segs.isEmpty()) o["segments"] = segs; a.append(o); }
    QJsonObject o; o["jobs"] = a; m_store->setSettingJson("render.jobs", o);
}

void RenderFarm::load() {
    for (const QJsonValue& v : m_store->settingJson("render.jobs").value("jobs").toArray()) {
        Job* j = new Job; j->o = v.toObject();
        for (const QJsonValue& s : j->o.value("segments").toArray()) { Seg sg; sg.o = s.toObject(); j->segs.append(sg); }
        const QString st = j->o.value("state").toString();
        if (st == "running") {
            const bool remoteSpot = j->o.value("kind").toString() == "spot" && j->o.value("nodeKind").toString() == "spotnode" && !j->o.value("remoteId").toString().isEmpty();
            if (remoteSpot) { j->deadlineMs = nowMs() + qint64(config().value("waitSec").toInt(1200)) * 1000; QTimer::singleShot(2000, this, [this, j]() { logLine(j, "resumed after a Supervisor restart"); pollSpotNode(j); }); }
            else { j->o["state"] = "failed"; j->o["error"] = "interrupted by a Supervisor restart"; j->o["finishedMs"] = double(nowMs()); }
        }
        m_jobs.append(j);
    }
    while (m_jobs.size() > 300) delete m_jobs.takeFirst();
}

int RenderFarm::localRunning() const {
    int n = 0;   // a running transcode holds its slot from the probe on; split jobs hold one per running local segment (+ the join)
    for (Job* j : m_jobs) {
        const bool running = j->o.value("state").toString() == "running";
        if (running && j->o.value("kind").toString() == "transcode") ++n; else if (j->proc) ++n;
        for (const Seg& s : j->segs) if (s.proc) ++n;
    }
    return n;
}

void RenderFarm::setStep(Job* j, const QString& step, double pct) { j->o["step"] = step; if (pct >= 0) j->o["pct"] = qBound(0.0, pct, 100.0); changed(); }

void RenderFarm::logLine(Job* j, const QString& line) {
    QJsonArray log = j->o.value("log").toArray(); log.append(QDateTime::currentDateTime().toString("HH:mm:ss ") + line); while (log.size() > 40) log.removeFirst(); j->o["log"] = log; changed();
}

void RenderFarm::finish(Job* j, const QString& state, const QString& error, const QJsonObject& extra) {
    if (finished(j->o.value("state").toString())) return;
    for (auto it = extra.begin(); it != extra.end(); ++it) j->o[it.key()] = it.value();
    j->o["state"] = state; j->o["finishedMs"] = double(nowMs());
    if (state == "done") { j->o["pct"] = 100; j->o["step"] = "done"; }
    if (!error.isEmpty()) { j->o["error"] = error; logLine(j, error); }
    if (state == "failed") emit alert("warn", QString("render job %1 (%2) failed: %3").arg(j->o.value("id").toString(), j->o.value("kind").toString(), error));
    if (j->done) { Done d = j->done; j->done = Done(); d(state == "done", j->o); }
    changed(); persist();
}

QJsonObject RenderFarm::submit(const QJsonObject& body, Done done) {
    const QString kind = body.value("kind").toString("spot");
    QJsonObject e; e["status"] = 400;
    if (!QStringList{"spot", "transcode", "split"}.contains(kind)) { e["error"] = "kind must be spot, transcode or split"; return e; }
    if (kind == "spot" && body.value("brief").toString().trimmed().isEmpty() && body.value("recipe").toString().isEmpty() && !body.contains("spec") && body.value("template").toString().isEmpty()) { e["error"] = "a spot job needs a brief, a template (with scenes) or a recipe"; return e; }
    if (kind != "spot") {
        const QString in = resolveInput(body.value("input").toString());
        if (in.isEmpty()) { e["error"] = "input not found (a file path on this Supervisor or a media id)"; return e; }
        const QString preset = body.value("preset").toString(kind == "split" ? "h264_1080p30" : "h264_1080p30");
        bool ok = false; suites::presetArgs(body.value("copy").toBool() ? "copy" : preset, &ok);
        if (!ok && !(preset == "custom" && config().value("allowCustomArgs").toBool())) { e["error"] = "unknown preset (h264_1080p30, h264_720p30, proxy_540p, audio_wav, loudnorm, copy)"; return e; }
    }
    Job* j = new Job; j->done = std::move(done);
    QJsonObject o; o["id"] = "rj-" + newId(8); o["kind"] = kind; o["state"] = "queued"; o["step"] = "queued"; o["pct"] = 0; o["createdMs"] = double(nowMs());
    QJsonObject req = body; req.remove("token"); o["request"] = req;
    o["requestId"] = body.value("requestId").toString(newId(32)); o["title"] = body.value("title").toString(body.value("ingest").toObject().value("title").toString());
    if (kind != "spot") o["input"] = resolveInput(body.value("input").toString());
    j->o = o; m_jobs.append(j);
    while (m_jobs.size() > 300 && finished(m_jobs.first()->o.value("state").toString())) delete m_jobs.takeFirst();
    changed(); QTimer::singleShot(0, this, &RenderFarm::pump);
    return o;
}

bool RenderFarm::cancel(const QString& id) {
    Job* j = find(id); if (!j || finished(j->o.value("state").toString())) return false;
    if (j->proc) j->proc->kill();
    for (Seg& s : j->segs) if (s.proc) s.proc->kill();
    if (j->o.value("nodeKind").toString() == "spotnode" && !j->o.value("remoteId").toString().isEmpty()) {
        bool f = false; const QJsonObject n = node(j->o.value("node").toString(), &f, true);
        if (f) m_http->json("POST", n.value("url").toString() + "/v1/jobs/" + j->o.value("remoteId").toString() + "/cancel", authHeaders(n), QJsonObject(), 8000, [](int, const QJsonObject&, const QByteArray&, const QString&) {});
    }
    finish(j, "cancelled", "cancelled by the operator");
    return true;
}

bool RenderFarm::remove(const QString& id) {
    Job* j = find(id); if (!j) return false;
    if (!finished(j->o.value("state").toString())) cancel(id);
    m_jobs.removeOne(j); delete j; changed(); persist(); return true;
}

int RenderFarm::clearFinished() {
    int n = 0;
    for (int i = m_jobs.size() - 1; i >= 0; --i) if (finished(m_jobs[i]->o.value("state").toString())) { delete m_jobs.takeAt(i); ++n; }
    changed(); persist(); return n;
}

void RenderFarm::pump() {
    const int maxLocal = config().value("maxLocal").toInt(2);
    for (Job* j : m_jobs) {
        const QString st = j->o.value("state").toString(), kind = j->o.value("kind").toString();
        if (st == "queued") {
            if (kind == "spot" || localRunning() < maxLocal) startJob(j);
        } else if (st == "running") {
            if (j->o.value("step").toString() == "ingest" && nowMs() - qint64(j->o.value("ingestMs").toDouble()) > 180000) {
                m_pendingIngest.remove(normPath(j->o.value("output").toString()));
                finish(j, "done", QString(), QJsonObject{{"note", "rendered; the library record did not appear within 3 min (already in the library?)"}});
            }
            if (kind == "split") runSegments(j);
        }
    }
}

void RenderFarm::startJob(Job* j) {
    j->o["state"] = "running"; j->o["startedMs"] = double(nowMs()); changed();
    const QString kind = j->o.value("kind").toString();
    j->workDir = QDir(m_dataDir).filePath("render/work/" + j->o.value("id").toString()); QDir().mkpath(j->workDir);
    if (kind == "spot") startSpot(j); else if (kind == "transcode") startTranscode(j); else startSplit(j);
}

QString RenderFarm::resolveInput(const QString& input) const {
    if (input.isEmpty()) return {};
    if (QFileInfo(input).isFile()) return QFileInfo(input).absoluteFilePath();
    bool f = false; const MediaRecord m = m_store->media(input, &f);
    if (f && QFileInfo(m.path).isFile()) return m.path;
    return {};
}

void RenderFarm::probeDuration(const QString& path, std::function<void(double)> cb) {
    bool f = false; const MediaRecord m = m_store->mediaByPath(path, &f);
    if (f && m.durationSec > 0) { cb(m.durationSec); return; }
    auto* p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, cb](int, QProcess::ExitStatus) { const double d = QString::fromUtf8(p->readAllStandardOutput()).trimmed().toDouble(); p->deleteLater(); cb(d); });
    connect(p, &QProcess::errorOccurred, this, [p, cb](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { p->deleteLater(); cb(0); } });
    p->start(m_ffprobe, {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", path});
}

void RenderFarm::deliver(Job* j, const QString& path) {
    j->o["output"] = QDir::toNativeSeparators(path);
    const QJsonValue ingestV = j->o.value("request").toObject().value("ingest");
    if ((ingestV.isBool() && !ingestV.toBool()) || !m_ingest) { finish(j, "done"); return; }
    QJsonObject meta = ingestV.toObject();
    const QString type = meta.value("type").toString(j->o.value("kind").toString() == "spot" ? "promo" : "program");
    meta.remove("type"); meta.remove("archived");
    if (meta.value("title").toString().isEmpty()) meta["title"] = j->o.value("title").toString(QFileInfo(path).completeBaseName());
    meta["renderJob"] = j->o.value("id").toString();
    if (!j->o.value("node").toString().isEmpty()) meta["renderNode"] = j->o.value("nodeName").toString(j->o.value("node").toString());
    m_pendingIngest[normPath(path)] = j->o.value("id").toString();
    j->o["ingestMs"] = double(nowMs()); setStep(j, "ingest", 98);
    m_ingest(path, type, meta);
}

void RenderFarm::onMediaIngested(const QString& mediaId, const QString&) {
    if (m_pendingIngest.isEmpty()) return;
    bool f = false; MediaRecord m = m_store->media(mediaId, &f); if (!f) return;
    const QString key = normPath(m.path); if (!m_pendingIngest.contains(key)) return;
    Job* j = find(m_pendingIngest.take(key)); if (!j) return;
    if (j->o.value("request").toObject().value("ingest").toObject().value("archived").toBool()) { m.archived = true; m_store->saveMedia(m); }
    finish(j, "done", QString(), QJsonObject{{"mediaId", mediaId}});
}

// =====================================================================================================
// spot jobs
// =====================================================================================================
QJsonObject RenderFarm::spotPayload(const QJsonObject& b, const QString& requestId) const {
    const QJsonObject cfg = config();
    QJsonObject p;
    const bool tpl = b.value("type").toString() == "template_render" || (b.value("type").toString().isEmpty() && (b.contains("scenes") || b.contains("variables")) && !b.value("template").toString().isEmpty());
    p["type"] = tpl ? "template_render" : "motion_spot";
    p["length_s"] = qBound(3, b.value("lengthSec").toInt(b.value("length_s").toInt(15)), 120);
    p["aspect"] = b.value("aspect").toString("16:9");
    p["mode"] = b.value("mode").toString(cfg.value("renderMode").toString("offline"));
    if (b.contains("brand")) p["brand"] = b.value("brand");
    if (b.value("assets").isArray() && !b.value("assets").toArray().isEmpty()) p["assets"] = b.value("assets");
    if (!b.value("template").toString().isEmpty()) p["template"] = b.value("template");
    if (tpl) {
        QJsonObject vars = b.value("variables").toObject(); if (!vars.contains("scenes")) vars["scenes"] = b.value("scenes").toArray(); p["variables"] = vars;
        if (!b.value("voice").toString().isEmpty() && p.value("mode").toString() == "offline") { const QString v = b.value("voice").toString(); p["voice"] = v.contains(':') ? v : "kokoro:" + v; }
    } else {
        if (b.contains("spec")) p["spec"] = b.value("spec");
        else {
            QString brief = b.value("brief").toString().trimmed();
            if (brief.isEmpty() && !b.value("recipe").toString().isEmpty())
                brief = QString("%1 for the station. Use ONLY these facts, with their exact names and numbers; do not invent anything:\n%2").arg(b.value("recipeLabel").toString(b.value("recipe").toString()), b.value("data").toString().left(3000));
            p["brief"] = brief.left(4000);
        }
        for (const char* k : {"quality", "music", "captions", "seed", "language"}) if (b.contains(k)) p[k] = b.value(k);
        if (!b.value("voice").toString().isEmpty()) { const QString v = b.value("voice").toString(); p["voice"] = v.contains(':') || v.startsWith("Microsoft") ? v : "kokoro:" + v; }
    }
    if (!b.value("renderOn").toString().isEmpty()) p["render_on"] = b.value("renderOn");
    QJsonObject meta; meta["source"] = "playplay"; meta["requestId"] = requestId; if (!b.value("recipe").toString().isEmpty()) meta["recipe"] = b.value("recipe"); if (!b.value("project").toString().isEmpty()) meta["project"] = b.value("project");
    p["metadata"] = meta;
    return p;
}

void RenderFarm::startSpot(Job* j) {
    const QJsonObject b = j->o.value("request").toObject();
    const QString target = b.value("target").toString("auto"), recipe = b.value("recipe").toString();
    const QJsonObject cfg = config();
    if (target == "local") { startLocalSpot(j); return; }
    setStep(j, "choosing a node", 2);
    probeAll([this, j, b, target, recipe, cfg](const QJsonObject&) {
        if (finished(j->o.value("state").toString())) return;
        const QJsonArray all = nodesForChoice();
        if (target != "auto") {
            for (const QJsonValue& v : all) {
                const QJsonObject n = v.toObject(); if (n.value("id").toString() != target) continue;
                const QString k = n.value("kind").toString();
                if (k == "spotnode") { startSpotNode(j, n); return; }
                if (k == "playplay") { startPeerSpot(j, n); return; }
                if (k == "local") { startLocalSpot(j); return; }
                finish(j, "failed", "a " + k + " node cannot render spots"); return;
            }
            finish(j, "failed", "unknown target node " + target); return;
        }
        const QString studio = cfg.value("studioMode").toString("prefer");
        const QJsonObject sn = suites::chooseRenderNode(all, "spotnode");
        if (recipe.isEmpty() || studio != "off") {
            if (!sn.isEmpty()) { startSpotNode(j, sn); return; }
            if (recipe.isEmpty() || studio == "only") {
                QString why = "no spot-node configured";
                for (const QJsonValue& v : all) if (v.toObject().value("kind").toString() == "spotnode") { suites::nodeEligible(v.toObject(), &why); break; }
                finish(j, "failed", "no spot-node ready: " + why); return;
            }
        }
        if (cfg.value("meshDispatch").toString("auto") == "auto") { const QJsonObject peer = suites::chooseRenderNode(all, "playplay"); if (!peer.isEmpty()) { startPeerSpot(j, peer); return; } }
        startLocalSpot(j);
    });
}

void RenderFarm::startSpotNode(Job* j, const QJsonObject& n) {
    j->o["node"] = n.value("id"); j->o["nodeName"] = n.value("name"); j->o["nodeKind"] = "spotnode";
    j->deadlineMs = nowMs() + qint64(config().value("waitSec").toInt(1200)) * 1000;
    const QJsonObject payload = spotPayload(j->o.value("request").toObject(), j->o.value("requestId").toString());
    j->o["payload"] = payload;
    auto h = authHeaders(n); h.append({"Idempotency-Key", j->o.value("requestId").toString().toUtf8()});
    setStep(j, "submitting to " + n.value("name").toString(), 4);
    m_http->json("POST", n.value("url").toString() + "/v1/jobs", h, payload, 30000, [this, j](int http, const QJsonObject& r, const QByteArray& raw, const QString& e) {
        if (finished(j->o.value("state").toString())) return;
        QString rid = r.value("id").toString();
        if (http == 409) rid = r.value("error").toObject().value("job_id").toString();
        if ((http != 200 && http != 201 && http != 409) || rid.isEmpty()) { finish(j, "failed", "spot-node refused the job: " + SuiteHttp::errorText(http, r, raw, e)); return; }
        j->o["remoteId"] = rid; j->o["costEstimateUsd"] = r.value("cost").toObject().value("estimate_usd");
        logLine(j, "accepted as " + rid); setStep(j, "queued on " + j->o.value("nodeName").toString(), 5); persist();
        QTimer::singleShot(3000, this, [this, j]() { pollSpotNode(j); });
    });
}

void RenderFarm::pollSpotNode(Job* j) {
    if (finished(j->o.value("state").toString())) return;
    bool f = false; const QJsonObject n = node(j->o.value("node").toString(), &f, true);
    if (!f) { finish(j, "failed", "render node removed while rendering"); return; }
    if (j->deadlineMs && nowMs() > j->deadlineMs) {
        m_http->json("POST", n.value("url").toString() + "/v1/jobs/" + j->o.value("remoteId").toString() + "/cancel", authHeaders(n), QJsonObject(), 8000, [](int, const QJsonObject&, const QByteArray&, const QString&) {});
        finish(j, "failed", QString("render timed out after %1 s (remote job cancelled)").arg(config().value("waitSec").toInt(1200))); return;
    }
    m_http->json("GET", n.value("url").toString() + "/v1/jobs/" + j->o.value("remoteId").toString(), authHeaders(n), {}, 15000, [this, j](int http, const QJsonObject& r, const QByteArray& raw, const QString& e) {
        if (finished(j->o.value("state").toString())) return;
        if (http != 200) {
            if (++j->pollFails > 40) { finish(j, "failed", "lost the render node: " + SuiteHttp::errorText(http, r, raw, e)); return; }
            QTimer::singleShot(5000, this, [this, j]() { pollSpotNode(j); }); return;
        }
        j->pollFails = 0;
        const QString st = r.value("status").toString(); const QJsonObject pr = r.value("progress").toObject();
        const QString stage = pr.value("stage").toString(st);
        if (stage != j->o.value("remoteStage").toString()) { j->o["remoteStage"] = stage; logLine(j, "remote: " + stage); }
        setStep(j, stage, 5 + pr.value("pct").toDouble() * 0.9);
        if (st == "succeeded") { fetchSpotNodeResult(j, r); return; }
        if (st == "failed" || st == "canceled") { const QJsonObject er = r.value("error").toObject(); finish(j, "failed", "remote render " + st + ": " + er.value("message").toString(er.value("code").toString("unknown error"))); return; }
        QTimer::singleShot(3000, this, [this, j]() { pollSpotNode(j); });
    });
}

void RenderFarm::fetchSpotNodeResult(Job* j, const QJsonObject& remote) {
    bool f = false; const QJsonObject n = node(j->o.value("node").toString(), &f, true);
    if (!f) { finish(j, "failed", "render node removed"); return; }
    const QJsonObject video = remote.value("result").toObject().value("video").toObject();
    j->o["remoteResult"] = remote.value("result"); j->o["costUsd"] = remote.value("cost").toObject().value("actual_usd").toDouble();
    if (video.value("duration_s").toDouble() > 0) j->o["durationSec"] = video.value("duration_s");
    QString base = j->o.value("title").toString().simplified(); base.replace(QRegularExpression("[^A-Za-z0-9_-]+"), "_"); base = base.left(40);
    const QString out = QDir(m_outDir).filePath(QString("%1%2%3.mp4").arg(base, base.isEmpty() ? "" : "_", j->o.value("id").toString()));
    setStep(j, "downloading", 96);
    const QString url = n.value("url").toString() + "/v1/jobs/" + j->o.value("remoteId").toString() + "/result";
    m_http->download(url, authHeaders(n), out, 300000, SuiteHttp::Progress(), [this, j, out, n](int http, const QJsonObject& r, const QByteArray& raw, const QString& e) {
        if (finished(j->o.value("state").toString())) return;
        if (http >= 400 || http == 0) { finish(j, "failed", "could not download the render: " + SuiteHttp::errorText(http, r, raw, e)); return; }
        if (QFileInfo(out).size() < 20000) { QFile::remove(out); finish(j, "failed", "the render node returned a tiny file; refusing to ingest it"); return; }
        // poster (best effort) for the library thumbnail of the review page
        const QString poster = out.left(out.size() - 4) + "_poster.png";
        m_http->download(n.value("url").toString() + "/v1/jobs/" + j->o.value("remoteId").toString() + "/result?file=poster.png", authHeaders(n), poster, 30000, SuiteHttp::Progress(),
                         [this, j, poster](int h2, const QJsonObject&, const QByteArray&, const QString&) { if (h2 == 200) { j->o["poster"] = QDir::toNativeSeparators(poster); changed(); } });
        deliver(j, out);
    });
}

void RenderFarm::startPeerSpot(Job* j, const QJsonObject& n) {
    j->o["node"] = n.value("id"); j->o["nodeName"] = n.value("name"); j->o["nodeKind"] = "playplay";
    const QJsonObject b = j->o.value("request").toObject();
    QJsonObject p; p["recipe"] = b.value("recipe"); p["data"] = b.value("data").toString(); p["channel"] = b.value("channel").toString(); p["voice"] = b.value("voice").toString();
    p["notes"] = b.value("notes").toString(); p["background"] = false; p["mesh"] = "local"; p["request_id"] = j->o.value("requestId");
    setStep(j, "rendering on " + n.value("name").toString(), 10);
    m_http->json("POST", n.value("url").toString() + "/api/newsroom/spot", authHeaders(n), p, 900000, [this, j, n](int http, const QJsonObject& r, const QByteArray& raw, const QString& e) {
        if (finished(j->o.value("state").toString())) return;
        if (http == 200 && r.value("status").toString() != "running") { finish(j, "done", QString(), QJsonObject{{"remote", r}, {"note", "rendered on " + n.value("name").toString() + "; the asset is in that station's library"}}); return; }
        if (http == 200 || (http == 0 && !e.contains("refused", Qt::CaseInsensitive))) { logLine(j, "ambiguous answer, reconciling by request id"); reconcilePeer(j, n, 0); return; }
        if (http == 0) { logLine(j, "peer unreachable, rendering locally: " + e); j->o.remove("node"); j->o.remove("nodeName"); j->o.remove("nodeKind"); startLocalSpot(j); return; }
        finish(j, "failed", "peer render failed: " + SuiteHttp::errorText(http, r, raw, e));
    });
}

void RenderFarm::reconcilePeer(Job* j, const QJsonObject& n, int attempt) {
    if (finished(j->o.value("state").toString())) return;
    if (attempt >= 60) { finish(j, "failed", "the peer render is still unresolved; no local duplicate was made"); return; }
    m_http->json("GET", n.value("url").toString() + "/api/newsroom/render-request/" + j->o.value("requestId").toString(), authHeaders(n), {}, 10000,
                 [this, j, n, attempt](int http, const QJsonObject& r, const QByteArray&, const QString&) {
        if (finished(j->o.value("state").toString())) return;
        if (http == 200 && r.value("status").toString() == "done") { finish(j, "done", QString(), QJsonObject{{"remote", r.value("result")}, {"note", "rendered on " + n.value("name").toString()}}); return; }
        if (http == 200 && r.value("status").toString() == "error") { finish(j, "failed", "peer render failed: " + r.value("error").toString()); return; }
        if (http == 404 && attempt >= 2) { logLine(j, "the peer never recorded the request; rendering locally"); startLocalSpot(j); return; }
        QTimer::singleShot(2000, this, [this, j, n, attempt]() { reconcilePeer(j, n, attempt + 1); });
    });
}

void RenderFarm::startLocalSpot(Job* j) {
    const QJsonObject b = j->o.value("request").toObject();
    if (b.value("recipe").toString().isEmpty() || !m_localSpot) { finish(j, "failed", "no render node is ready and this job has no recipe for the local spot generator"); return; }
    j->o["node"] = "local"; j->o["nodeName"] = "This Supervisor"; j->o["nodeKind"] = "local";
    QJsonObject req; req["recipe"] = b.value("recipe"); req["data"] = b.value("data").toString(b.value("brief").toString()); req["channel"] = b.value("channel"); req["voice"] = b.value("voice");
    req["notes"] = b.value("notes"); if (b.contains("lengthSec")) req["targetSec"] = b.value("lengthSec");
    setStep(j, "local spot generator", 10);
    const QJsonObject r = m_localSpot(req, [this, j](bool ok, const QJsonObject& res) {
        if (finished(j->o.value("state").toString())) return;
        if (!ok) { finish(j, res.value("skipped").toBool() ? "done" : "failed", res.value("error").toString(res.value("reason").toString()), QJsonObject{{"result", res}}); return; }
        const QString mid = res.value("mediaId").toString();
        if (!mid.isEmpty() && j->o.value("request").toObject().value("ingest").toObject().value("archived").toBool()) { bool f = false; MediaRecord m = m_store->media(mid, &f); if (f) { m.archived = true; m_store->saveMedia(m); } }
        finish(j, "done", QString(), QJsonObject{{"mediaId", mid}, {"output", res.value("path")}, {"durationSec", res.value("durationSec")}, {"result", res}});
    });
    if (r.contains("error")) { finish(j, "failed", r.value("error").toString()); return; }
    j->o["genId"] = r.value("genId");
}

// =====================================================================================================
// transcode + split (local ffmpeg, StarAgent segments)
// =====================================================================================================
static QStringList jobArgs(const QJsonObject& req, const QJsonObject& cfg) {
    if (req.value("copy").toBool()) return suites::presetArgs("copy");
    const QString preset = req.value("preset").toString("h264_1080p30");
    if (preset == "custom" && cfg.value("allowCustomArgs").toBool()) { QStringList a; for (const QJsonValue& v : req.value("args").toArray()) a << v.toString(); return a; }
    return suites::presetArgs(preset);
}

void RenderFarm::startTranscode(Job* j) {
    const QJsonObject req = j->o.value("request").toObject();
    const QString in = j->o.value("input").toString();
    if (!QFileInfo(in).isFile()) { finish(j, "failed", "input not found: " + in); return; }
    const QString preset = req.value("copy").toBool() ? "copy" : req.value("preset").toString("h264_1080p30");
    const QString out = QDir(m_outDir).filePath(QString("%1_%2_%3.%4").arg(QFileInfo(in).completeBaseName().left(40), preset, j->o.value("id").toString(), suites::presetExt(preset)));
    setStep(j, "probing", 1);
    probeDuration(in, [this, j, in, out, req](double dur) {
        if (finished(j->o.value("state").toString())) return;
        j->durationSec = dur;
        QStringList args = {"-hide_banner", "-nostdin", "-y", "-progress", "pipe:1", "-nostats", "-i", in};
        args << jobArgs(req, config()) << out;
        auto* p = new QProcess(this); j->proc = p; setStep(j, "transcoding", 2);
        auto errTail = std::make_shared<QByteArray>();
        connect(p, &QProcess::readyReadStandardOutput, this, [this, j, p]() { const double s = suites::progressSeconds(p->readAllStandardOutput()); if (s >= 0 && j->durationSec > 0) setStep(j, "transcoding", 2 + 95 * qMin(1.0, s / j->durationSec)); });
        connect(p, &QProcess::readyReadStandardError, this, [p, errTail]() { errTail->append(p->readAllStandardError()); if (errTail->size() > 4000) *errTail = errTail->right(4000); });
        connect(p, &QProcess::finished, this, [this, j, p, out, errTail](int code, QProcess::ExitStatus st) {
            j->proc = nullptr; p->deleteLater();
            if (finished(j->o.value("state").toString())) return;
            if (st != QProcess::NormalExit || code != 0) { finish(j, "failed", "ffmpeg failed: " + QString::fromUtf8(errTail->right(400)).simplified()); return; }
            deliver(j, out);
        });
        connect(p, &QProcess::errorOccurred, this, [this, j, p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { j->proc = nullptr; p->deleteLater(); finish(j, "failed", "ffmpeg did not start: " + m_ffmpeg); } });
        p->start(m_ffmpeg, args);
    });
}

void RenderFarm::startSplit(Job* j) {
    const QJsonObject req = j->o.value("request").toObject();
    const QString in = j->o.value("input").toString();
    if (!QFileInfo(in).isFile()) { finish(j, "failed", "input not found: " + in); return; }
    setStep(j, "probing", 1);
    auto plan = [this, j, req](double dur) {
        if (finished(j->o.value("state").toString())) return;
        if (dur <= 0) { finish(j, "failed", "could not read the input's duration (give durationSec)"); return; }
        j->durationSec = dur;
        QStringList workers; for (const QJsonValue& v : req.value("workers").toArray()) if (!v.toString().isEmpty()) workers << v.toString();
        if (workers.isEmpty()) workers << "local";
        const QJsonObject remoteIn = req.value("remoteInput").toObject();
        for (const QString& w : workers) {
            if (w == "local") continue;
            bool f = false; const QJsonObject n = node(w, &f);
            if (!f || n.value("kind").toString() != "staragent") { finish(j, "failed", "split workers must be \"local\" or staragent nodes: " + w); return; }
            if (remoteIn.value(w).toString().isEmpty()) { finish(j, "failed", "worker " + w + " needs remoteInput." + w + " (the input's path on that node)"); return; }
        }
        const QJsonArray segs = suites::planSegments(dur, qBound(1, req.value("segments").toInt(4), 50), workers);
        for (const QJsonValue& v : segs) { Seg s; s.o = v.toObject(); s.o["state"] = "queued"; s.o["pct"] = 0; j->segs.append(s); }
        logLine(j, QString("%1 segments over %2").arg(segs.size()).arg(workers.join(", ")));
        setStep(j, "rendering segments", 3); runSegments(j);
    };
    const double given = req.value("durationSec").toDouble();
    if (given > 0) plan(given); else probeDuration(in, plan);
}

void RenderFarm::runSegments(Job* j) {
    if (finished(j->o.value("state").toString()) || j->segs.isEmpty()) return;
    int done = 0; double pct = 0; const int maxLocal = config().value("maxLocal").toInt(2);
    for (int i = 0; i < j->segs.size(); ++i) {
        const QString st = j->segs[i].o.value("state").toString();
        if (st == "failed") { for (Seg& s : j->segs) if (s.proc) s.proc->kill(); finish(j, "failed", QString("segment %1 failed: %2").arg(i).arg(j->segs[i].o.value("error").toString())); return; }
        if (st == "done") ++done;
        pct += j->segs[i].o.value("pct").toDouble();
    }
    setStep(j, done == j->segs.size() ? "joining" : QString("segments %1/%2").arg(done).arg(j->segs.size()), 3 + 0.9 * pct / j->segs.size());
    if (done == j->segs.size()) { if (j->o.value("step").toString() == "joining" && !j->proc && !j->o.value("joinStarted").toBool()) { j->o["joinStarted"] = true; concatSegments(j); } return; }
    for (int i = 0; i < j->segs.size(); ++i) {
        if (j->segs[i].o.value("state").toString() != "queued") continue;
        if (j->segs[i].o.value("worker").toString() == "local" && localRunning() >= maxLocal) continue;
        startSegment(j, i);
    }
}

static QString cmdQuote(const QString& a) { return a.contains(QRegularExpression("[\\s,()&|<>^\"]")) ? "\"" + QString(a).replace("\"", "\\\"") + "\"" : a; }

void RenderFarm::startSegment(Job* j, int i) {
    Seg& s = j->segs[i];
    const QJsonObject req = j->o.value("request").toObject();
    const QString worker = s.o.value("worker").toString();
    const QString start = QString::number(s.o.value("startSec").toDouble(), 'f', 3), dur = QString::number(s.o.value("durSec").toDouble(), 'f', 3);
    QStringList segArgs = jobArgs(req, config()); if (req.value("copy").toBool()) segArgs << "-avoid_negative_ts" << "make_zero";
    if (worker == "local") {
        const QString out = QDir(j->workDir).filePath(QString("seg_%1.mp4").arg(i, 3, 10, QChar('0')));
        QStringList args = {"-hide_banner", "-nostdin", "-y", "-progress", "pipe:1", "-nostats", "-ss", start, "-t", dur, "-i", j->o.value("input").toString()};
        args << segArgs << out;
        auto* p = new QProcess(this); s.proc = p; s.o["state"] = "running"; s.o["output"] = out;
        const double segDur = s.o.value("durSec").toDouble();
        auto errTail = std::make_shared<QByteArray>();
        connect(p, &QProcess::readyReadStandardOutput, this, [this, j, i, p, segDur]() { const double t = suites::progressSeconds(p->readAllStandardOutput()); if (t >= 0 && segDur > 0 && i < j->segs.size()) { j->segs[i].o["pct"] = qMin(100.0, 100 * t / segDur); changed(); } });
        connect(p, &QProcess::readyReadStandardError, this, [p, errTail]() { errTail->append(p->readAllStandardError()); if (errTail->size() > 4000) *errTail = errTail->right(4000); });
        connect(p, &QProcess::finished, this, [this, j, i, p, errTail](int code, QProcess::ExitStatus st) {
            p->deleteLater(); if (i >= j->segs.size()) return; Seg& sg = j->segs[i]; sg.proc = nullptr;
            if (finished(j->o.value("state").toString())) return;
            if (st == QProcess::NormalExit && code == 0) { sg.o["state"] = "done"; sg.o["pct"] = 100; } else { sg.o["state"] = "failed"; sg.o["error"] = QString::fromUtf8(errTail->right(300)).simplified(); }
            runSegments(j);
        });
        connect(p, &QProcess::errorOccurred, this, [this, j, i, p](QProcess::ProcessError e) { if (e == QProcess::FailedToStart && i < j->segs.size()) { j->segs[i].proc = nullptr; j->segs[i].o["state"] = "failed"; j->segs[i].o["error"] = "ffmpeg did not start"; p->deleteLater(); runSegments(j); } });
        p->start(m_ffmpeg, args);
        changed(); return;
    }
    // StarAgent worker: run ffmpeg on the node against its own copy of the input, then fetch the segment back
    bool f = false; const QJsonObject n = node(worker, &f);
    const QString starNode = n.value("starNode").toString(worker);
    const QString rin = req.value("remoteInput").toObject().value(worker).toString();
    QString rdir = rin; rdir = rdir.left(qMax(rdir.lastIndexOf('\\'), rdir.lastIndexOf('/')));
    const QString rout = rdir + "\\pp_seg_" + j->o.value("id").toString() + "_" + QString::number(i) + ".mp4";
    QStringList cmd = {"ffmpeg", "-hide_banner", "-nostdin", "-y", "-ss", start, "-t", dur, "-i", rin}; cmd << segArgs << rout;
    QStringList quoted; for (const QString& a : cmd) quoted << cmdQuote(a);
    s.o["state"] = "running"; s.o["remoteOutput"] = rout; s.o["pct"] = 10; changed();
    QJsonObject body; body["command"] = quoted.join(' '); body["shell"] = "cmd"; body["timeoutMs"] = 3600000;
    m_star->call(starNode, "POST", "/exec", body, [this, j, i, starNode, rout](int status, const QJsonObject& r) {
        if (i >= j->segs.size() || finished(j->o.value("state").toString())) return;
        Seg& sg = j->segs[i];
        const QJsonObject d = r.value("data").toObject();
        const int exitCode = d.contains("exitCode") ? d.value("exitCode").toInt() : d.value("code").toInt(status == 200 ? 0 : 1);
        if (status != 200 || exitCode != 0) { sg.o["state"] = "failed"; sg.o["error"] = r.value("error").toObject().value("message").toString(d.value("stderr").toString().right(300)); runSegments(j); return; }
        sg.o["state"] = "fetching"; sg.o["pct"] = 70; changed();
        const QJsonObject fj = m_star->startFetch(starNode, rout, "render");
        if (fj.contains("error")) { sg.o["state"] = "failed"; sg.o["error"] = fj.value("error").toString(); runSegments(j); }
    }, 3600000);
}

void RenderFarm::concatSegments(Job* j) {
    QString list; for (const Seg& s : j->segs) list += "file '" + QDir::fromNativeSeparators(s.o.value("output").toString()).replace("'", "'\\''") + "'\n";
    const QString listPath = QDir(j->workDir).filePath("concat.txt");
    QFile lf(listPath); if (!lf.open(QIODevice::WriteOnly)) { finish(j, "failed", "cannot write the concat list"); return; }
    lf.write(list.toUtf8()); lf.close();
    const QString in = j->o.value("input").toString();
    const QString out = QDir(m_outDir).filePath(QString("%1_split_%2.mp4").arg(QFileInfo(in).completeBaseName().left(40), j->o.value("id").toString()));
    auto* p = new QProcess(this); j->proc = p;
    auto errTail = std::make_shared<QByteArray>();
    connect(p, &QProcess::readyReadStandardError, this, [p, errTail]() { errTail->append(p->readAllStandardError()); if (errTail->size() > 4000) *errTail = errTail->right(4000); });
    connect(p, &QProcess::finished, this, [this, j, p, out, errTail](int code, QProcess::ExitStatus st) {
        j->proc = nullptr; p->deleteLater();
        if (finished(j->o.value("state").toString())) return;
        if (st != QProcess::NormalExit || code != 0) { finish(j, "failed", "join failed: " + QString::fromUtf8(errTail->right(400)).simplified()); return; }
        if (!j->o.value("request").toObject().value("keepSegments").toBool()) for (const Seg& s : j->segs) QFile::remove(s.o.value("output").toString());
        deliver(j, out);
    });
    p->start(m_ffmpeg, {"-hide_banner", "-nostdin", "-y", "-f", "concat", "-safe", "0", "-i", listPath, "-c", "copy", "-movflags", "+faststart", out});
}

// =====================================================================================================
// PlayPlay OBS station-compatible newsroom routes
// =====================================================================================================
void RenderFarm::studioHealth(JsonCb cb) {
    QJsonObject sn; for (const QJsonValue& v : nodes()) if (v.toObject().value("kind").toString() == "spotnode") { sn = v.toObject(); break; }
    const QString mode = config().value("studioMode").toString();
    if (sn.isEmpty()) { QJsonObject o; o["ok"] = false; o["ready"] = false; o["reason"] = "no spot-node configured"; o["mode"] = mode; cb(o); return; }
    probe(sn.value("id").toString(), [cb, mode](const QJsonObject& n) {
        QJsonObject o = n.value("health").toObject(); o["url"] = n.value("url"); o["mode"] = mode; o["node"] = n.value("id"); cb(o);
    });
}

QJsonObject RenderFarm::renderRequest(const QString& requestId) const {
    const QJsonObject rq = m_store->settingJson("render.requests").value("requests").toObject().value(requestId).toObject();
    if (rq.isEmpty()) { QJsonObject e; e["error"] = "unknown render request"; e["status"] = 404; return e; }
    QJsonObject o; o["request_id"] = requestId; o["status"] = rq.value("status");
    if (rq.value("status").toString() == "done") o["result"] = rq.value("result");
    else if (rq.value("status").toString() == "error") o["error"] = rq.value("error");
    return o;
}

static QJsonObject stationResult(const QJsonObject& job) {
    QJsonObject o; o["media_id"] = job.value("mediaId"); o["mediaId"] = job.value("mediaId"); o["recipe"] = job.value("request").toObject().value("recipe");
    o["duration_sec"] = job.value("durationSec"); o["path"] = job.value("output"); o["render_job"] = job.value("id"); o["node"] = job.value("nodeName");
    const QJsonObject res = job.value("result").toObject(); if (res.contains("headline")) o["title"] = res.value("headline");
    if (job.contains("remote")) o["remote"] = job.value("remote");
    return o;
}

void RenderFarm::newsroomSpot(const QJsonObject& body, std::function<void(int, const QJsonObject&)> reply) {
    auto fail = [reply](int s, const QString& m) { QJsonObject e; e["error"] = m; reply(s, e); };
    const QString recipe = body.value("recipe").toString();
    if (recipe.isEmpty()) { fail(400, "recipe required"); return; }
    const QString requestId = body.value("request_id").toString().trimmed();
    if (!requestId.isEmpty()) {
        if (body.value("background").toBool()) { fail(400, "request_id requires background=false"); return; }
        if (!QRegularExpression("^[A-Za-z0-9_-]{8,100}$").match(requestId).hasMatch()) { fail(400, "invalid request_id"); return; }
        QJsonObject canonical = body; canonical.remove("request_id");
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(canonical).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
        QJsonObject all = m_store->settingJson("render.requests"); QJsonObject reqs = all.value("requests").toObject();
        const QJsonObject ex = reqs.value(requestId).toObject();
        if (!ex.isEmpty()) {
            if (ex.value("hash").toString() != hash) { fail(409, "request_id belongs to a different render"); return; }
            if (ex.value("status").toString() == "done") { reply(200, ex.value("result").toObject()); return; }
            if (ex.value("status").toString() == "error") { fail(502, ex.value("error").toString("peer render failed")); return; }
            QJsonObject o; o["request_id"] = requestId; o["status"] = "running"; reply(200, o); return;
        }
        const qint64 cutoff = nowMs() - 14LL * 86400000;
        for (auto it = reqs.begin(); it != reqs.end();) { if (qint64(it.value().toObject().value("ms").toDouble()) < cutoff) it = reqs.erase(it); else ++it; }
        QJsonObject rec; rec["hash"] = hash; rec["status"] = "running"; rec["ms"] = double(nowMs()); reqs[requestId] = rec; all["requests"] = reqs; m_store->setSettingJson("render.requests", all);
        QJsonObject jb; jb["kind"] = "spot"; jb["recipe"] = recipe; jb["data"] = body.value("data"); jb["channel"] = body.value("channel"); jb["voice"] = body.value("voice"); jb["notes"] = body.value("notes"); jb["target"] = "local"; jb["requestId"] = requestId;
        submit(jb, [this, requestId, reply](bool ok, const QJsonObject& job) {
            QJsonObject all2 = m_store->settingJson("render.requests"); QJsonObject reqs2 = all2.value("requests").toObject(); QJsonObject r = reqs2.value(requestId).toObject();
            const QJsonObject res = stationResult(job);
            if (ok) { r["status"] = "done"; r["result"] = res; } else { r["status"] = "error"; r["error"] = job.value("error").toString("render failed"); }
            reqs2[requestId] = r; all2["requests"] = reqs2; m_store->setSettingJson("render.requests", all2);
            if (ok) reply(200, res); else { QJsonObject e; e["error"] = "spot generation failed: " + job.value("error").toString(); reply(502, e); }
        });
        return;
    }
    const QString mesh = body.value("mesh").toString("auto").toLower();
    QJsonObject jb; jb["kind"] = "spot"; jb["recipe"] = recipe; jb["data"] = body.value("data"); jb["channel"] = body.value("channel"); jb["voice"] = body.value("voice"); jb["notes"] = body.value("notes");
    jb["target"] = mesh == "auto" ? "auto" : "local";
    if (body.value("background").toBool()) { const QJsonObject o = submit(jb); QJsonObject r; r["started"] = true; r["recipe"] = recipe; r["render_job"] = o.value("id"); reply(200, r); return; }
    submit(jb, [reply](bool ok, const QJsonObject& job) {
        if (ok) { reply(200, stationResult(job)); return; }
        QJsonObject e; e["error"] = "spot generation failed: " + job.value("error").toString(); reply(502, e);
    });
}
