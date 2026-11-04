#include "starnodes.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <memory>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QString trimUrl(QString u) { u = u.trimmed(); while (u.endsWith('/')) u.chop(1); if (!u.isEmpty() && !u.contains("://")) u = "http://" + u; return u; }
static QJsonObject errObj(const QString& m) { QJsonObject e; e["error"] = m; return e; }
static QString enc(const QString& v) { return QString::fromLatin1(QUrl::toPercentEncoding(v)); }   // QUrlQuery leaves backslashes to QUrl, which drops them from Windows paths

StarNodes::StarNodes(ScheduleStore* store, QObject* parent) : QObject(parent), m_store(store), m_http(new SuiteHttp(this)) {
    connect(&m_poll, &QTimer::timeout, this, [this]() { probeAll([](const QJsonObject&) {}); });
}

void StarNodes::start() {
    m_poll.start(qBound(15, config().value("pollSec").toInt(60), 3600) * 1000);
    QTimer::singleShot(3000, this, [this]() { probeAll([](const QJsonObject&) {}); });
}

QJsonObject StarNodes::config() const {
    QJsonObject c = m_store->settingJson("starnodes.config");
    if (!c.contains("allowExec")) c["allowExec"] = false;
    if (!c.contains("pollSec")) c["pollSec"] = 60;
    if (!c.contains("stationWrites")) c["stationWrites"] = true;
    return c;
}

QJsonObject StarNodes::setConfig(const QJsonObject& patch) {
    QJsonObject c = m_store->settingJson("starnodes.config");
    for (const char* k : {"allowExec", "pollSec", "stationWrites"}) if (patch.contains(k)) c[k] = patch.value(k);
    c["pollSec"] = qBound(15, c.value("pollSec").toInt(60), 3600);
    m_store->setSettingJson("starnodes.config", c);
    m_poll.start(c.value("pollSec").toInt() * 1000);
    return config();
}

QJsonArray StarNodes::stored() const { return m_store->settingJson("starnodes.nodes").value("nodes").toArray(); }
void StarNodes::storeAll(const QJsonArray& a) { QJsonObject o; o["nodes"] = a; m_store->setSettingJson("starnodes.nodes", o); notify(); }

void StarNodes::notify() { QJsonObject o; o["nodes"] = nodes(); emit event("starnodes", o); }

QJsonObject StarNodes::normalize(const QJsonObject& in, const QJsonObject& prev) const {
    static const QStringList keys = {"id", "name", "host", "execUrl", "token", "stateUrl", "playplayUrl", "playplayPass", "roles", "enabled", "render", "allowExec", "notes", "station"};
    QJsonObject o = prev;
    for (const QString& k : keys) if (in.contains(k)) o[k] = in.value(k);
    if (in.value("token").toString().isEmpty()) o["token"] = prev.value("token");               // blank keeps the stored secret
    if (in.value("playplayPass").toString().isEmpty()) o["playplayPass"] = prev.value("playplayPass");
    QString host = o.value("host").toString().trimmed();
    if (o.value("execUrl").toString().trimmed().isEmpty() && !host.isEmpty()) o["execUrl"] = "http://" + host + ":8898";
    o["execUrl"] = trimUrl(o.value("execUrl").toString()); o["stateUrl"] = trimUrl(o.value("stateUrl").toString()); o["playplayUrl"] = trimUrl(o.value("playplayUrl").toString());
    if (host.isEmpty()) { for (const char* k : {"execUrl", "playplayUrl"}) { const QString h = QUrl(o.value(k).toString()).host(); if (!h.isEmpty()) { host = h; break; } } o["host"] = host; }
    if (o.value("name").toString().trimmed().isEmpty()) o["name"] = host;
    if (!o.contains("enabled")) o["enabled"] = true;
    if (!o.contains("allowExec")) o["allowExec"] = false;
    if (!o.contains("render")) o["render"] = o.value("notes").toString().toLower().remove(' ').contains("render=true");
    QJsonObject st = o.value("station").toObject();
    if (!QStringList{"playplay", "cpp", "masterplay"}.contains(st.value("kind").toString())) st["kind"] = QUrl(o.value("playplayUrl").toString()).port() == 7700 ? "cpp" : "playplay";
    o["station"] = st;
    if (o.value("id").toString().isEmpty()) o["id"] = "sn-" + QUuid::createUuid().toString(QUuid::Id128).left(6);
    return o;
}

QJsonArray StarNodes::nodes(bool withSecrets) const {
    QJsonArray out;
    for (const QJsonValue& v : stored()) {
        QJsonObject n = v.toObject();
        n["hasToken"] = !n.value("token").toString().isEmpty(); n["hasPlayplayPass"] = !n.value("playplayPass").toString().isEmpty();
        if (!withSecrets) { n.remove("token"); n.remove("playplayPass"); }
        n["health"] = m_health.value(n.value("id").toString());
        out.append(n);
    }
    return out;
}

QJsonObject StarNodes::node(const QString& id, bool* found, bool withSecrets) const {
    for (const QJsonValue& v : nodes(withSecrets)) if (v.toObject().value("id").toString() == id) { if (found) *found = true; return v.toObject(); }
    if (found) *found = false; return {};
}

QList<QPair<QByteArray, QByteArray>> StarNodes::execHeaders(const QJsonObject& n) const {
    QList<QPair<QByteArray, QByteArray>> h; if (!n.value("token").toString().isEmpty()) h.append({"x-agent-auth", n.value("token").toString().toUtf8()}); return h;
}
QList<QPair<QByteArray, QByteArray>> StarNodes::stationHeaders(const QJsonObject& n) const {
    QList<QPair<QByteArray, QByteArray>> h; const QByteArray p = n.value("playplayPass").toString().toUtf8();
    if (!p.isEmpty()) { h.append({"X-PP-Pass", p}); h.append({"X-PP-Key", p}); h.append({"Authorization", "Bearer " + p}); }
    return h;
}

void StarNodes::execCall(const QJsonObject& n, const QString& method, const QString& path, const QJsonObject& body, int timeoutMs, Reply reply) {
    const QString base = n.value("execUrl").toString();
    if (base.isEmpty()) { reply(409, errObj("this node has no star-node exec API (execUrl)")); return; }
    m_http->json(method, base + path, execHeaders(n), body, timeoutMs, [reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        if (http == 0) { reply(502, errObj("node unreachable: " + e)); return; }
        QJsonObject out = j; if (j.isEmpty() && !raw.isEmpty()) out["text"] = QString::fromUtf8(raw.left(65536));
        if (http >= 400 && !out.contains("error")) out["error"] = SuiteHttp::errorText(http, j, raw, e);
        reply(http, out);
    });
}

// =====================================================================================================
// enrol / update / probe
// =====================================================================================================
void StarNodes::enrol(const QJsonObject& body, Reply reply) {
    QJsonObject prev;
    for (const QJsonValue& v : stored()) if (!body.value("id").toString().isEmpty() && v.toObject().value("id").toString() == body.value("id").toString()) prev = v.toObject();
    const QJsonObject n = normalize(body, prev);
    if (n.value("execUrl").toString().isEmpty() && n.value("playplayUrl").toString().isEmpty()) { reply(400, errObj("give execUrl (or host) of the star-node, or the node's playplayUrl")); return; }
    const bool force = body.value("force").toBool();
    auto store = [this, n, reply](bool online, const QString& note) {
        QJsonArray list = stored(); bool replaced = false;
        for (int i = 0; i < list.size(); ++i) if (list[i].toObject().value("id").toString() == n.value("id").toString()) { list[i] = n; replaced = true; }
        if (!replaced) list.append(n);
        storeAll(list); logAudit(n.value("id").toString(), "enrol", QJsonObject{{"online", online}}, 200, note);
        probe(n.value("id").toString(), [reply, note](const QJsonObject& node) { QJsonObject o; o["node"] = node; if (!note.isEmpty()) o["note"] = note; reply(200, o); });
    };
    auto verifyStation = [this, n, force, store, reply]() {
        const QString pp = n.value("playplayUrl").toString();
        m_http->json("GET", pp + (n.value("station").toObject().value("kind").toString() == "cpp" ? "/api/v1/status" : "/api/status"), stationHeaders(n), {}, 8000,
                     [force, store, reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            if (http == 200) { store(true, QString()); return; }
            if (http == 401 || http == 403) { reply(403, errObj("the station refused the password: " + SuiteHttp::errorText(http, j, raw, e))); return; }
            if (force) { store(false, "stored offline: " + SuiteHttp::errorText(http, j, raw, e)); return; }
            reply(502, errObj("could not verify the station (" + SuiteHttp::errorText(http, j, raw, e) + "); send force:true to store it anyway"));
        });
    };
    if (n.value("execUrl").toString().isEmpty()) { verifyStation(); return; }
    if (n.value("token").toString().isEmpty()) { reply(400, errObj("token required (the star-node authToken from star-node-config.json)")); return; }
    m_http->json("GET", n.value("execUrl").toString() + "/api/fleet/status", execHeaders(n), {}, 8000,
                 [n, force, store, reply, verifyStation](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        if (http == 200) { store(true, QString()); return; }
        if (http == 401) { reply(403, errObj("the node refused the token (x-agent-auth)")); return; }
        if (http > 0 && http != 401) { store(true, "exec API answered " + SuiteHttp::errorText(http, j, raw, e)); return; }   // reachable and authenticated far enough
        if (!n.value("playplayUrl").toString().isEmpty()) { verifyStation(); return; }
        if (force) { store(false, "stored offline: " + SuiteHttp::errorText(http, j, raw, e)); return; }
        reply(502, errObj("could not reach the node (" + SuiteHttp::errorText(http, j, raw, e) + "); send force:true to store it anyway"));
    });
}

QJsonObject StarNodes::update(const QString& id, const QJsonObject& patch) {
    QJsonArray list = stored();
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].toObject().value("id").toString() != id) continue;
        QJsonObject p = patch; p["id"] = id; list[i] = normalize(p, list[i].toObject()); storeAll(list);
        logAudit(id, "update", QJsonObject(), 200, QString());
        bool f = false; return node(id, &f);
    }
    QJsonObject e = errObj("no such node"); e["status"] = 404; return e;
}

bool StarNodes::remove(const QString& id) {
    QJsonArray list = stored(), out; bool hit = false;
    for (const QJsonValue& v : list) { if (v.toObject().value("id").toString() == id) { hit = true; continue; } out.append(v); }
    if (hit) { storeAll(out); m_health.remove(id); logAudit(id, "remove", QJsonObject(), 200, QString()); }
    return hit;
}

void StarNodes::probe(const QString& id, JsonCb cb) {
    bool f = false; const QJsonObject n = node(id, &f, true);
    if (!f) { cb(errObj("no such node")); return; }
    struct Acc { QJsonObject h; int left = 0; QElapsedTimer t; };
    auto acc = std::make_shared<Acc>(); acc->t.start();
    auto done = [this, id, cb, acc]() {
        if (--acc->left > 0) return;
        const bool ok = acc->h.value("exec").toObject().value("ok").toBool() || acc->h.value("playplay").toObject().value("ok").toBool();
        acc->h["ok"] = ok; acc->h["checkedMs"] = double(nowMs()); acc->h["latencyMs"] = double(acc->t.elapsed());
        const bool was = m_health.value(id).value("ok").toBool(true);
        m_health[id] = acc->h; notify();
        if (was && !ok && m_health.size() > 0) emit alert("warn", "Star node " + id + " is not answering");
        bool f2 = false; cb(node(id, &f2));
    };
    const bool hasExec = !n.value("execUrl").toString().isEmpty(), hasState = !n.value("stateUrl").toString().isEmpty(), hasPp = !n.value("playplayUrl").toString().isEmpty();
    acc->left = int(hasExec) + int(hasState) + int(hasPp);
    if (acc->left == 0) { acc->left = 1; done(); return; }
    if (hasExec) m_http->json("GET", n.value("execUrl").toString() + "/api/fleet/status", execHeaders(n), {}, 8000, [acc, done](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        QJsonObject x; x["ok"] = http == 200; x["http"] = http; if (http != 200) x["reason"] = SuiteHttp::errorText(http, j, raw, e);
        else { const QJsonArray st = j.value("stations").toArray(); x["stations"] = st.size(); }
        acc->h["exec"] = x; done();
    });
    if (hasState) m_http->json("GET", n.value("stateUrl").toString() + "/api/state", {}, {}, 6000, [acc, done](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        QJsonObject x; x["ok"] = http == 200; if (http != 200) x["reason"] = SuiteHttp::errorText(http, j, raw, e);
        else { QJsonArray svc; for (const QJsonValue& s : j.value("state").toObject().value("services").toArray()) { const QJsonObject so = s.toObject(); QJsonObject r; r["id"] = so.value("id"); r["label"] = so.value("label"); r["up"] = so.value("up").toBool(so.value("running").toBool()); svc.append(r); } x["services"] = svc; }
        acc->h["watchdog"] = x; done();
    });
    if (hasPp) {
        const bool cpp = n.value("station").toObject().value("kind").toString() == "cpp";
        m_http->json("GET", n.value("playplayUrl").toString() + (cpp ? "/api/v1/status" : "/api/status"), stationHeaders(n), {}, 6000, [acc, done, cpp](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            QJsonObject x; x["ok"] = http == 200; x["flavor"] = cpp ? "cpp" : "playplay-obs"; if (http != 200) x["reason"] = SuiteHttp::errorText(http, j, raw, e);
            else { x["channels"] = j.value("channels").isArray() ? j.value("channels").toArray().size() : j.value("obs_connections").toObject().value("per_channel").toObject().size(); x["version"] = j.value("version"); }
            acc->h["playplay"] = x; done();
        });
    }
}

void StarNodes::probeAll(JsonCb cb) {
    const QJsonArray all = nodes();
    auto left = std::make_shared<int>(0);
    for (const QJsonValue& v : all) if (v.toObject().value("enabled").toBool(true)) ++*left;
    if (*left == 0) { QJsonObject o; o["nodes"] = nodes(); cb(o); return; }
    for (const QJsonValue& v : all) {
        if (!v.toObject().value("enabled").toBool(true)) continue;
        probe(v.toObject().value("id").toString(), [this, left, cb](const QJsonObject&) { if (--*left == 0) { QJsonObject o; o["nodes"] = nodes(); cb(o); } });
    }
}

// =====================================================================================================
// allowlisted actions
// =====================================================================================================
void StarNodes::logAudit(const QString& nodeId, const QString& action, const QJsonObject& args, int status, const QString& error) {
    QJsonObject all = m_store->settingJson("starnodes.audit"); QJsonArray a = all.value("entries").toArray();
    QJsonObject e; e["ms"] = double(nowMs()); e["node"] = nodeId; e["action"] = action; QJsonObject safe = args; safe.remove("token"); safe.remove("password");
    if (safe.contains("body")) safe["body"] = QString::fromUtf8(QJsonDocument(safe.value("body").toObject()).toJson(QJsonDocument::Compact)).left(400);
    e["args"] = safe; e["status"] = status; e["ok"] = status > 0 && status < 400; if (!error.isEmpty()) e["error"] = error.left(300);
    a.append(e); while (a.size() > 300) a.removeFirst(); all["entries"] = a; m_store->setSettingJson("starnodes.audit", all);
}

QJsonArray StarNodes::audit(int limit) const {
    const QJsonArray a = m_store->settingJson("starnodes.audit").value("entries").toArray(); QJsonArray out;
    for (int i = a.size() - 1; i >= 0 && out.size() < limit; --i) out.append(a[i]);
    return out;
}

void StarNodes::action(const QString& id, const QString& action, const QJsonObject& args, Reply reply) {
    bool f = false; const QJsonObject n = node(id, &f, true);
    if (!f) { reply(404, errObj("no such node")); return; }
    if (!suites::starNodeActionAllowed(action)) { logAudit(id, action, args, 403, "not allowlisted"); reply(403, errObj("action not allowed: " + action)); return; }
    if (!n.value("enabled").toBool(true)) { reply(409, errObj("node is disabled")); return; }
    auto done = [this, id, action, args, reply](int status, const QJsonObject& body) {
        logAudit(id, action, args, status, body.value("error").toString()); QJsonObject o; o["status"] = status; o["action"] = action; o["result"] = body; reply(status >= 400 ? status : 200, status >= 400 ? body : o);
    };
    if (action == "fleet.status") { execCall(n, "GET", "/api/fleet/status", {}, 20000, done); return; }
    if (action == "fleet.analytics") { execCall(n, "GET", "/api/fleet/analytics", {}, 30000, done); return; }
    if (action == "fleet.asPlayed") { const QString d = args.value("date").toString(); execCall(n, "GET", "/api/fleet/as-played" + (d.isEmpty() ? QString() : "?date=" + enc(d)), {}, 30000, done); return; }
    if (action == "fleet.query") {
        const QString cmd = args.value("command").toString();
        if (!suites::fleetCommandAllowed(cmd)) { done(403, errObj("fleet command not allowed: " + cmd)); return; }
        if (args.value("stationId").toString().isEmpty()) { done(400, errObj("stationId required")); return; }
        QJsonObject b; b["stationId"] = args.value("stationId"); b["command"] = cmd; b["args"] = args.value("args").toObject();
        execCall(n, "POST", "/api/fleet/query", b, 30000, done); return;
    }
    if (action == "sync.jobs") { execCall(n, "GET", "/api/sync/jobs", {}, 15000, done); return; }
    if (action == "sync.run") { const QString jid = args.value("id").toString(); if (!QRegularExpression("^[A-Za-z0-9_-]{1,80}$").match(jid).hasMatch()) { done(400, errObj("sync job id required")); return; } execCall(n, "POST", "/api/sync/jobs/" + jid + "/run", QJsonObject(), 120000, done); return; }
    if (action == "swarm.index") { execCall(n, "GET", "/api/swarm/index", {}, 15000, done); return; }
    if (action == "browse") { execCall(n, "GET", "/api/browse?path=" + enc(args.value("path").toString()), {}, 15000, done); return; }
    if (action == "state" || action == "service.restart") {
        const QString su = n.value("stateUrl").toString();
        if (su.isEmpty()) { done(409, errObj("the watchdog API is loopback-only; set stateUrl (http://127.0.0.1:8125) on the node that runs this Supervisor")); return; }
        if (action == "state") { m_http->json("GET", su + "/api/state", {}, {}, 10000, [done](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) { QJsonObject o = j; if (http != 200) o["error"] = SuiteHttp::errorText(http, j, raw, e); done(http ? http : 502, o); }); return; }
        const QString svc = args.value("service").toString();
        if (!suites::restartableServices().contains(svc)) { done(403, errObj("service not restartable: " + svc)); return; }
        m_http->json("POST", su + "/api/service/" + svc + "/restart", {}, QJsonObject(), 60000, [done](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) { QJsonObject o = j; if (http != 200) o["error"] = SuiteHttp::errorText(http, j, raw, e); done(http ? http : 502, o); });
        return;
    }
    if (action == "station.status") {
        const QString pp = n.value("playplayUrl").toString(); if (pp.isEmpty()) { done(409, errObj("node has no playplayUrl")); return; }
        const bool cpp = n.value("station").toObject().value("kind").toString() == "cpp";
        m_http->json("GET", pp + (cpp ? "/api/v1/status" : "/api/status"), stationHeaders(n), {}, 10000, [done](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) { QJsonObject o = j; if (http != 200) o["error"] = SuiteHttp::errorText(http, j, raw, e); done(http ? http : 502, o); });
        return;
    }
    if (action == "station.capabilities") { stationCapabilities(id, args.value("force").toBool(), [done](int s, const QJsonObject& b) { done(s, b); }); return; }
    if (action == "station.call") { stationCall(id, args.value("method").toString("GET"), args.value("path").toString(), args.value("body"), [done](int s, const QJsonObject& b) { done(s, b); }); return; }
    if (action == "exec") {
        if (!config().value("allowExec").toBool() || !n.value("allowExec").toBool()) { done(403, errObj("exec is off: it needs allowExec on the node and in starnodes.config")); return; }
        const QString cmd = args.value("command").toString().trimmed(); if (cmd.isEmpty()) { done(400, errObj("command required")); return; }
        QJsonObject b; b["command"] = cmd; b["shell"] = args.value("shell").toString("powershell") == "cmd" ? "cmd" : "powershell"; b["timeout"] = qBound(1000, args.value("timeoutMs").toInt(30000), 120000);
        execCall(n, "POST", "/api/exec", b, 130000, done); return;
    }
    done(400, errObj("unhandled action " + action));
}

// =====================================================================================================
// stations
// =====================================================================================================
QJsonArray StarNodes::stations() const {
    QJsonArray out;
    QJsonObject local; local["id"] = "local"; local["label"] = "This station"; local["kind"] = "cpp"; local["online"] = true; local["node_id"] = ""; local["machine"] = ""; out.append(local);
    for (const QJsonValue& v : nodes()) {
        const QJsonObject n = v.toObject(); if (n.value("playplayUrl").toString().isEmpty()) continue;
        QJsonObject s; s["id"] = n.value("id"); s["label"] = n.value("name"); s["kind"] = n.value("station").toObject().value("kind").toString("playplay");
        s["machine"] = n.value("host"); s["note"] = n.value("notes"); s["channel"] = n.value("station").toObject().value("channel");
        const QJsonObject pp = n.value("health").toObject().value("playplay").toObject();
        s["online"] = pp.value("ok").toBool(); s["node_id"] = n.value("id"); s["ip"] = n.value("host");
        if (!pp.value("ok").toBool()) s["why"] = pp.value("reason").toString("not checked yet");
        if (s.value("kind").toString() == "masterplay") s["why"] = "MasterPlay has no scheduling API: files can be delivered, nothing can be scheduled";
        out.append(s);
    }
    return out;
}

void StarNodes::stationCapabilities(const QString& sid, bool force, Reply reply) {
    static const QList<QPair<QString, QString>> probes = {{"hour_recipes", "/api/hour-recipes"}, {"recipe_week_plan", "/api/hour-recipes/week-plan"}, {"day_shuffle", "/api/schedule/CH1/0/shuffle"},
        {"folder_blocks", "/api/containers/resolve"}, {"folder_jobs", "/api/ingest/folder/probe"}, {"mesh_fetch", "/api/mesh/fetch"}, {"newsroom", "/api/newsroom/overview"}, {"inputs", "/api/inputs"}};
    if (sid == "local") { QJsonObject o; for (const auto& p : probes) o[p.first] = true; reply(200, o); return; }
    bool f = false; const QJsonObject n = node(sid, &f, true);
    if (!f) { reply(404, errObj("unknown station")); return; }
    if (n.value("station").toObject().value("kind").toString() == "masterplay") { QJsonObject o; for (const auto& p : probes) o[p.first] = false; reply(200, o); return; }
    const auto hit = m_caps.value(sid);
    if (!force && hit.first && nowMs() - hit.first < 600000) { reply(200, hit.second); return; }
    const QString pp = n.value("playplayUrl").toString(); if (pp.isEmpty()) { reply(409, errObj("node has no playplayUrl")); return; }
    auto acc = std::make_shared<QJsonObject>(); auto left = std::make_shared<int>(probes.size());
    for (const auto& p : probes) {
        const QString key = p.first;
        m_http->raw("OPTIONS", pp + p.second, stationHeaders(n), QByteArray(), QByteArray(), 15000, [this, sid, key, acc, left, reply](int http, const QJsonObject&, const QByteArray&, const QString&) {
            (*acc)[key] = http == 405 || http == 200 || http == 204 ? QJsonValue(true) : http == 404 ? QJsonValue(false) : QJsonValue(QJsonValue::Null);
            if (--*left == 0) { m_caps[sid] = {nowMs(), *acc}; reply(200, *acc); }
        });
    }
}

void StarNodes::stationCall(const QString& sid, const QString& method, const QString& path, const QJsonValue& body, Reply reply) {
    const QString m = method.toUpper();
    if (!path.startsWith("/api/")) { reply(400, errObj("path must start with /api/")); return; }
    if (sid == "local") { reply(404, errObj("call this station's API directly")); return; }
    bool f = false; const QJsonObject n = node(sid, &f, true);
    if (!f) { reply(404, errObj("unknown station")); return; }
    if (n.value("station").toObject().value("kind").toString() == "masterplay") { reply(409, errObj(n.value("name").toString() + " runs MasterPlay: it has no PlayPlay API to call")); return; }
    if (!suites::stationPathAllowed(m, path)) { logAudit(sid, "station.call", QJsonObject{{"method", m}, {"path", path}}, 403, "not allowlisted"); reply(403, errObj(m + " " + path.section('?', 0, 0) + " is not something the HUB may do on another station")); return; }
    if (m != "GET" && !config().value("stationWrites").toBool(true)) { reply(403, errObj("station writes are switched off (starnodes.config.stationWrites)")); return; }
    const QString pp = n.value("playplayUrl").toString(); if (pp.isEmpty()) { reply(409, errObj("node has no playplayUrl")); return; }
    QJsonObject b = body.isObject() ? body.toObject() : QJsonObject(); if (body.isArray()) b["_array"] = body.toArray();
    auto t = std::make_shared<QElapsedTimer>(); t->start();
    m_http->json(m, pp + path, stationHeaders(n), b, 25000, [this, sid, m, path, t, reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        QJsonObject o; o["status"] = http; o["ms"] = double(t->elapsed());
        if (http == 0) o["error"] = "the station's PlayPlay didn't answer: " + e;
        else if (j.contains("_array")) o["body"] = j.value("_array"); else if (!j.isEmpty()) o["body"] = j; else o["body"] = QString::fromUtf8(raw.left(65536));
        if (m != "GET") { logAudit(sid, "station.call", QJsonObject{{"method", m}, {"path", path}}, http, o.value("error").toString()); emit event("starnodes", QJsonObject{{"stationWrite", true}, {"station", sid}, {"path", path}, {"status", http}}); }
        reply(200, o);
    });
}

// =====================================================================================================
// PlayPlay OBS /api/nodes registry
// =====================================================================================================
QJsonArray StarNodes::stationNodeRows() const {
    QJsonArray out; const QJsonObject inv = m_store->settingJson("starnodes.inventory");
    for (const QJsonValue& v : nodes()) {
        const QJsonObject n = v.toObject(); const QJsonObject h = n.value("health").toObject();
        QJsonObject r; r["id"] = n.value("id"); r["name"] = n.value("name"); r["url"] = n.value("playplayUrl").toString(n.value("execUrl").toString()); r["enabled"] = n.value("enabled").toBool(true) ? 1 : 0;
        r["notes"] = n.value("notes"); r["status"] = h.value("ok").toBool() ? "online" : (h.isEmpty() ? "unknown" : "offline");
        r["last_seen"] = h.value("ok").toBool() ? QDateTime::fromMSecsSinceEpoch(qint64(h.value("checkedMs").toDouble())).toUTC().toString(Qt::ISODate) : QString();
        r["inventory_count"] = inv.value(n.value("id").toString()).toObject().value("items").toArray().size(); r["render"] = n.value("render");
        out.append(r);
    }
    return out;
}

void StarNodes::createStationNode(const QJsonObject& body, Reply reply) {
    QJsonObject b; b["name"] = body.value("name").toString("Node"); b["playplayUrl"] = body.value("url"); b["playplayPass"] = body.value("auth_key"); b["notes"] = body.value("notes"); b["force"] = true;
    if (!body.value("execUrl").toString().isEmpty()) { b["execUrl"] = body.value("execUrl"); b["token"] = body.value("token"); }
    enrol(b, [this, reply](int status, const QJsonObject& o) {
        if (status != 200) { reply(status, o); return; }
        const QString id = o.value("node").toObject().value("id").toString();
        refreshInventory(id, [this, id, reply](int, const QJsonObject&) { for (const QJsonValue& v : stationNodeRows()) if (v.toObject().value("id").toString() == id) { reply(200, v.toObject()); return; } reply(200, QJsonObject{{"id", id}}); });
    });
}

void StarNodes::refreshInventory(const QString& id, Reply reply) {
    bool f = false; const QJsonObject n = node(id, &f, true);
    if (!f) { reply(404, errObj("no such node")); return; }
    const QString pp = n.value("playplayUrl").toString(); if (pp.isEmpty()) { reply(409, errObj("node has no playplayUrl")); return; }
    m_http->json("GET", pp + "/api/media?limit=100000", stationHeaders(n), {}, 30000, [this, id, reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        if (http != 200) { reply(502, errObj("node unreachable: " + SuiteHttp::errorText(http, j, raw, e))); return; }
        const QJsonArray rows = j.contains("_array") ? j.value("_array").toArray() : j.value("media").toArray(j.value("items").toArray());
        QJsonArray items;
        for (const QJsonValue& v : rows) { const QJsonObject m = v.toObject(); QJsonObject r; r["id"] = m.value("id"); r["title"] = m.value("display_title").toString(m.value("title").toString());
            r["fingerprint"] = m.value("fingerprint").toString(m.value("hash").toString()); r["duration_sec"] = m.value("duration_sec").toDouble(m.value("durationSec").toDouble()); items.append(r); }
        QJsonObject all = m_store->settingJson("starnodes.inventory"); QJsonObject one; one["items"] = items; one["ms"] = double(nowMs()); all[id] = one; m_store->setSettingJson("starnodes.inventory", all);
        QJsonObject o; o["ok"] = true; o["assets"] = items.size(); reply(200, o);
    });
}

QJsonObject StarNodes::inventory(const QString& id) const {
    const QJsonObject one = m_store->settingJson("starnodes.inventory").value(id).toObject();
    QJsonObject o; for (const QJsonValue& v : one.value("items").toArray()) { const QJsonObject r = v.toObject(); o[r.value("id").toString()] = r; }
    return o;
}
