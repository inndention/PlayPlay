// SupervisorCore: redundancy routes — sessions (station state bundles), failover pairing, remote nodes.
// Constructs m_sessions + m_redundancy (see registerRoutes order in supervisorcore.cpp).
#include "supervisorcore.h"
#include "restapiserver.h"
#include "sessionsmanager.h"
#include "failoverpair.h"
#include "channelmanager.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>
#include <QSet>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <memory>

using namespace sched;
using Q = QHash<QString, QString>;

namespace {
QStringList groupsOf(const QJsonObject& body, const Q& q) {
    QStringList g;
    for (const QJsonValue& v : body.value("groups").toArray()) g << v.toString();
    if (g.isEmpty() && body.value("groups").isString()) g = body.value("groups").toString().split(',', Qt::SkipEmptyParts);
    if (g.isEmpty() && q.contains("groups")) g = q.value("groups").split(',', Qt::SkipEmptyParts);
    for (QString& s : g) s = s.trimmed();
    g.removeAll(QString());
    return g;
}
QString slug(const QString& s) {
    QString o; for (const QChar& c : s) o += (c.isLetterOrNumber() ? c.toLower() : QChar('-'));
    while (o.contains("--")) o.replace("--", "-");
    o = o.trimmed(); if (o.startsWith('-')) o.remove(0, 1); if (o.endsWith('-')) o.chop(1);
    return o.isEmpty() ? "session" : o.left(48);
}
// Paths a remote node may be driven through the proxy (everything else is refused).
const QStringList kProxyPrefixes = {"/channels", "/schedule", "/status", "/media", "/sessions", "/failover", "/asrun", "/reports", "/spots", "/mux", "/library"};

// A 401/403 from a node means THAT node rejected the token we hold for it. Forwarded verbatim, the browser sees a 401 from this
// Supervisor and the dashboard (api() treats every 401 as "ask for the local token") opens the local login dialog, which cannot fix it.
// Report it as a gateway failure (502) that names the cause.
bool upstreamRejectedToken(int http) { return http == 401 || http == 403; }
int upstreamStatus(int http) { return upstreamRejectedToken(http) ? 502 : http; }
QJsonObject upstreamBody(int http, QJsonObject o, const QString& what) {
    if (!upstreamRejectedToken(http)) return o;
    // registerAsync forwards only `error` for a status >= 400, so the cause has to be in the message itself.
    o["error"] = QString("%1: the node rejected our token (HTTP %2: %3) — correct its token under Remote > Nodes").arg(what).arg(http).arg(o.value("error").toString().left(120));
    return o;
}
}

void SupervisorCore::registerRedundancyRoutes() {
    RestApiServer* r = m_rest.get();
    m_sessions = std::make_unique<SessionsManager>(m_store.get(), m_opt.dataDir);
    m_redundancy = std::make_unique<FailoverPair>(m_store.get(), m_exec.get(), m_sessions.get());
    m_redundancy->setDataDir(m_opt.dataDir);

    auto pushRedundancy = [this]() { broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), m_redundancy->wsPayload(), "redundancy"); };
    connect(m_redundancy.get(), &FailoverPair::alert, this, [this](const QString& level, const QString& msg) { pushAlert("failover", level, msg); });
    connect(m_redundancy.get(), &FailoverPair::roleChanged, this, [pushRedundancy](const QString&) { pushRedundancy(); });
    connect(m_redundancy.get(), &FailoverPair::statusChanged, this, pushRedundancy);
    connect(m_sessions.get(), &SessionsManager::changed, this, pushRedundancy);

    // After a REAL applyBundle (stored session, posted bundle or a replication cycle): the executors cache each channel's profile, and the
    // channel set / frame format can have changed. Reload every executor, spawn an engine for each channel the apply created, restart the
    // engine of a running channel whose frame format changed (same rule as PUT /channels/{id}). A dry run or an error report (nothing
    // written: no `applied`) touches nothing.
    auto afterApply = [this](const QJsonObject& rep) {
        if (rep.value("dryRun").toBool(true) || !rep.contains("applied")) return;
        QSet<QString> restart;
        for (const QJsonValue& v : rep.value("restartChannels").toArray()) restart.insert(v.toString());
        if (m_opt.autoStartChannels) {
            for (const QJsonValue& v : rep.value("createdChannels").toArray()) {
                bool f = false; const ChannelProfile p = m_store->channel(v.toString(), &f);
                if (f && p.enabled && !m_exec->hasChannel(p.id) && !m_channels->isChannelRunning(p.id)) { spawnChannel(p); qInfo().noquote() << "[Redundancy] engine spawned for channel" << p.id << "created by an applied bundle"; }
            }
        }
        for (const ChannelProfile& p : m_store->channels()) {
            if (!m_exec->hasChannel(p.id)) continue;
            if (restart.contains(p.id) && m_channels->isChannelRunning(p.id)) {
                if (m_channels->channel(p.id)) m_channels->removeChannel(p.id);
                if (p.enabled) spawnChannel(p);
                qInfo().noquote() << "[Redundancy] engine restarted for channel" << p.id << "(frame format changed)";
            }
            m_exec->reloadChannel(p.id);
        }
    };
    connect(m_redundancy.get(), &FailoverPair::channelsApplied, this, afterApply);
    // Demotion / role=backup: disarming only stops the executor pushing, so also stop playout (engine slate) on every channel.
    connect(m_redundancy.get(), &FailoverPair::takeOffAir, this, [this](const QString& reason) {
        for (const ChannelProfile& p : m_store->channels()) if (m_exec->hasChannel(p.id)) transport(p.id, "stop", QJsonObject());
        qInfo().noquote() << "[Redundancy] playout stopped on every channel (" + reason + ")";
    });

    // ------------------------------------------------------------------ sessions
    r->registerRoute("GET", "/api/v1/sessions", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["sessions"] = m_sessions->list(); o["node"] = m_sessions->nodeInfo(); o["groups"] = QJsonArray::fromStringList(SessionsManager::allGroups());
        o["dir"] = QDir::toNativeSeparators(m_sessions->sessionsDir()); return o;
    });
    r->registerRoute("POST", "/api/v1/sessions/capture", [this](const QJsonObject& body, const Q&) {
        const QJsonObject s = m_sessions->capture(body.value("name").toString("Session"), body.value("notes").toString());
        if (s.isEmpty()) return err(500, "capture failed (sessions directory not writable?)");
        return s;
    });
    r->registerRoute("POST", "/api/v1/sessions/import", [this](const QJsonObject& body, const Q&) {
        QJsonObject wire = body.value("bundle").isObject() ? body.value("bundle").toObject() : body.value("data").isObject() ? body.value("data").toObject() : body;
        const QJsonObject s = m_sessions->importBundle(wire, body.value("name").toString());
        if (s.contains("error")) return err(400, s.value("error").toString());
        return s;
    });
    r->registerRoute("POST", "/api/v1/sessions/receive", [this](const QJsonObject& body, const Q&) {
        const QJsonObject res = m_sessions->receiveBundle(body);
        if (res.contains("error")) return err(400, res.value("error").toString());
        return res;
    });
    // Replication export: the current station state for the requested groups whose marks differ from `since`.
    r->registerRoute("GET", "/api/v1/sessions/export-live", [this](const QJsonObject& body, const Q& q) {
        QJsonObject since = QJsonDocument::fromJson(q.value("since").toUtf8()).object();
        if (since.isEmpty() && body.value("since").isObject()) since = body.value("since").toObject();
        // Credential settings (spots LLM key, Star tokens, camera passwords) only go to a caller that authenticated: with an empty
        // rest.token this endpoint is open to the whole LAN, so `secrets=1` is then ignored (response says `secrets:false`).
        const bool wantSecrets = q.value("secrets") == "1" || q.value("secrets") == "true" || body.value("secrets").toBool(false);
        return m_sessions->exportLive(groupsOf(body, q), since, wantSecrets && !m_store->setting("rest.token").isEmpty());
    });
    r->registerRoute("GET", "/api/v1/sessions/marks", [this](const QJsonObject&, const Q&) { return m_sessions->marks(); });
    r->registerRoute("GET", "/api/v1/sessions/{id}", [this](const QJsonObject&, const Q& q) {
        bool f = false; const QJsonObject s = m_sessions->get(q["id"], q.value("data") == "1" || q.value("data") == "true", &f);
        if (!f) return err(404, "unknown session");
        return s;
    });
    r->registerRoute("PUT", "/api/v1/sessions/{id}", [this](const QJsonObject& body, const Q& q) {
        if (!m_sessions->rename(q["id"], body.value("name").toString(), body.value("notes").toString(), body.contains("name"), body.contains("notes"))) return err(404, "unknown session");
        return m_sessions->get(q["id"], false);
    });
    r->registerRoute("DELETE", "/api/v1/sessions/{id}", [this](const QJsonObject&, const Q& q) {
        if (!m_sessions->remove(q["id"])) return err(404, "unknown session");
        QJsonObject o; o["deleted"] = q["id"]; return o;
    });
    r->registerRoute("POST", "/api/v1/sessions/{id}/recapture", [this](const QJsonObject&, const Q& q) {
        bool f = false; const QJsonObject s = m_sessions->recapture(q["id"], &f);
        if (!f) return err(404, "unknown session");
        return s;
    });
    r->registerRoute("POST", "/api/v1/sessions/{id}/clone", [this](const QJsonObject& body, const Q& q) {
        bool f = false; const QJsonObject s = m_sessions->clone(q["id"], body.value("name").toString(), &f);
        if (!f) return err(404, "unknown session");
        return s;
    });
    r->registerRoute("GET", "/api/v1/sessions/{id}/export", [this](const QJsonObject&, const Q& q) {
        bool f = false; const QJsonObject w = m_sessions->exportBundle(q["id"], &f);
        if (!f) return err(404, "unknown session");
        return w;
    });
    // Browser download of the same bundle (raw: Content-Disposition). Prefix chosen so it cannot shadow /sessions/{id}.
    r->registerRaw("GET", "/api/v1/sessions/export/", [this](QTcpSocket* sock, const QString& path, const Q&) {
        const QString id = path.mid(QString("/api/v1/sessions/export/").size()).section('.', 0, 0);
        bool f = false; const QJsonObject w = m_sessions->exportBundle(id, &f);
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        if (!f) { RestApiServer::writeRawHeader(sock, 404, "application/json", false); sock->write("{\"error\":\"unknown session\"}"); sock->flush(); sock->disconnectFromHost(); return; }
        const QByteArray json = QJsonDocument(w).toJson(QJsonDocument::Indented);
        QByteArray h = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\n";
        h += "Content-Disposition: attachment; filename=\"session-" + slug(w.value("name").toString()).toUtf8() + ".ppsession.json\"\r\n";
        h += "Content-Length: " + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n";
        sock->write(h + json); sock->flush(); sock->disconnectFromHost();
    });
    r->registerRoute("POST", "/api/v1/sessions/{id}/apply", [this, afterApply](const QJsonObject& body, const Q& q) {
        SessionsManager::ApplyOptions opt;
        opt.replace = body.value("replace").toBool(false);
        opt.keepArmed = !body.value("armFromBundle").toBool(false);
        opt.keepOutputs = true;   // "outputs" in groups overrides inside applyBundle
        bool f = false;
        const QJsonObject rep = m_sessions->applySession(q["id"], groupsOf(body, q), body.value("dryRun").toBool(false) || body.value("dry_run").toBool(false), opt, &f);
        if (!f) return err(404, "unknown session");
        afterApply(rep);
        return rep;
    });
    // Apply a bundle posted directly (no stored session): {bundle, groups, dryRun, replace}.
    r->registerRoute("POST", "/api/v1/sessions/apply-bundle", [this, afterApply](const QJsonObject& body, const Q& q) {
        if (!body.value("bundle").isObject()) return err(400, "bundle required");
        SessionsManager::ApplyOptions opt; opt.replace = body.value("replace").toBool(false); opt.keepArmed = !body.value("armFromBundle").toBool(false);
        const QJsonObject rep = m_sessions->applyBundle(body.value("bundle").toObject(), groupsOf(body, q), body.value("dryRun").toBool(false), opt);
        afterApply(rep);
        return rep;
    });

    // ------------------------------------------------------------------ failover
    r->registerRoute("GET", "/api/v1/failover/status", [this](const QJsonObject&, const Q&) { return m_redundancy->status(); });
    // Partial update. A client that changes `role` should also send `expectedRole` (the role it last saw): if the node has taken a
    // different role since (auto-promote, a peer's newer epoch), the change is refused with 409 instead of silently reverting it.
    auto putConfig = [this](const QJsonObject& body, const Q&) {
        if (body.contains("role")) { const QString role = body.value("role").toString().toLower(); if (role != "standalone" && role != "primary" && role != "backup" && !role.isEmpty()) return err(400, "role must be standalone, primary or backup"); }
        if (body.contains("expectedRole") && body.value("expectedRole").toString() != m_redundancy->role())
            return err(409, QString("this node's role is now %1 (not %2) — reload before changing the role").arg(m_redundancy->role(), body.value("expectedRole").toString()));
        return m_redundancy->setConfig(body);
    };
    r->registerRoute("PUT", "/api/v1/failover/config", putConfig);
    r->registerRoute("POST", "/api/v1/failover/config", putConfig);
    r->registerRoute("GET", "/api/v1/failover/heartbeat", [this](const QJsonObject&, const Q&) { return m_redundancy->heartbeatBody(); });
    r->registerRoute("POST", "/api/v1/failover/promote", [this](const QJsonObject& body, const Q&) { return m_redundancy->promote(body.value("reason").toString("operator")); });
    r->registerRoute("POST", "/api/v1/failover/demote", [this](const QJsonObject& body, const Q&) { return m_redundancy->demote(body.value("reason").toString("operator")); });
    r->registerRoute("GET", "/api/v1/failover/history", [this](const QJsonObject&, const Q&) { QJsonObject o; o["history"] = m_redundancy->history(); return o; });

    // ------------------------------------------------------------------ remote nodes
    auto nodes = [this]() { return m_store->settingJson("remote.nodes").value("nodes").toArray(); };
    auto findNode = [nodes](const QString& id) -> QJsonObject {
        for (const QJsonValue& v : nodes()) if (v.toObject().value("id").toString() == id) return v.toObject();
        return QJsonObject();
    };
    auto nodeUrl = [](const QJsonObject& n) {
        QString u = n.value("url").toString().trimmed();
        const bool bareAddress = !u.contains("://");
        if (bareAddress) u = "http://" + u;
        QUrl parsed(u);
        if (bareAddress && parsed.port() < 0 && !parsed.host().isEmpty()) parsed.setPort(7700);
        u = parsed.toString(); while (u.endsWith('/')) u.chop(1); return u;
    };
    auto publicNodes = [](const QJsonArray& list) {
        QJsonArray out;
        for (const QJsonValue& value : list) {
            QJsonObject node = value.toObject();
            node["hasPassword"] = !node.value("token").toString().isEmpty();
            node.remove("token"); node.remove("password"); out.append(node);
        }
        return out;
    };
    // GET <node>/api/v1/status + /api/v1/failover/status → one compact row.
    auto nodeStatus = [this, nodeUrl](const QJsonObject& n, std::function<void(const QJsonObject&)> done) {
        struct Acc { QJsonObject status, fo; int pending = 2; bool ok = false; QString error; QElapsedTimer t; qint64 latency = -1; };
        auto acc = std::make_shared<Acc>(); acc->t.start();
        const QString url = nodeUrl(n), token = n.value("token").toString();
        auto finish = [n, url, acc, done]() {
            if (--acc->pending > 0) return;
            QJsonObject row; row["id"] = n.value("id"); row["name"] = n.value("name"); row["url"] = url;
            row["reachable"] = acc->ok; row["latencyMs"] = double(acc->latency); row["error"] = acc->error;
            row["role"] = acc->fo.value("role").toString(acc->ok ? "standalone" : ""); row["epoch"] = acc->fo.value("epoch");
            row["peer"] = acc->fo.value("peer").toObject();
            row["uptimeMs"] = acc->status.value("uptimeMs"); row["health"] = acc->status.value("health"); row["nodeName"] = acc->fo.value("nodeName");
            QJsonArray chans;
            for (const QJsonValue& v : acc->status.value("channels").toArray()) {
                const QJsonObject c = v.toObject(), p = c.value("profile").toObject(), ex = c.value("executor").toObject(), eng = c.value("engine").toObject();
                QJsonObject o; o["id"] = p.value("id"); o["name"] = p.value("name"); o["armed"] = ex.value("armed").toBool(p.value("armed").toBool());
                o["state"] = eng.value("state").toString(c.value("engineState").toString()); o["title"] = eng.value("title").toString(ex.value("onAirTitle").toString());
                o["connected"] = c.value("process").toObject().value("connected"); chans.append(o);
            }
            row["channels"] = chans;
            done(row);
        };
        m_redundancy->fetchJson("GET", url + "/api/v1/status", token, QJsonObject(), 4000, [acc, finish](int http, const QJsonObject& j, const QString& e) {
            acc->latency = acc->t.elapsed(); if (http == 200) { acc->ok = true; acc->status = j; } else acc->error = e.isEmpty() ? QString("HTTP %1").arg(http) : e; finish(); });
        m_redundancy->fetchJson("GET", url + "/api/v1/failover/status", token, QJsonObject(), 4000, [acc, finish](int http, const QJsonObject& j, const QString&) {
            if (http == 200) acc->fo = j; finish(); });
    };

    r->registerRoute("GET", "/api/v1/remote/nodes", [this, nodes, publicNodes](const QJsonObject&, const Q&) { QJsonObject o; o["nodes"] = publicNodes(nodes()); o["self"] = m_sessions->nodeInfo(); return o; });
    r->registerRoute("PUT", "/api/v1/remote/nodes", [this, nodeUrl, nodes, publicNodes](const QJsonObject& body, const Q&) {
        QHash<QString, QString> existingPasswords;
        for (const QJsonValue& v : nodes()) { const QJsonObject n = v.toObject(); existingPasswords.insert(n.value("id").toString(), n.value("token").toString()); }
        QJsonArray out;
        for (const QJsonValue& v : body.value("nodes").toArray()) {
            QJsonObject n = v.toObject();
            if (n.value("url").toString().trimmed().isEmpty()) continue;
            if (n.value("id").toString().isEmpty()) n["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
            const QString supplied = n.value("password").toString(n.value("token").toString());
            if (supplied.contains('\r') || supplied.contains('\n') || supplied.size() > 1024) return err(400, "Invalid node password");
            if (!supplied.isEmpty()) n["token"] = supplied;
            else if (existingPasswords.contains(n.value("id").toString())) n["token"] = existingPasswords.value(n.value("id").toString());
            n.remove("password"); n.remove("hasPassword");
            n["url"] = nodeUrl(n);
            const QUrl target(n.value("url").toString());
            if (!target.isValid() || target.host().isEmpty() || (target.scheme() != "http" && target.scheme() != "https") || !target.userInfo().isEmpty()) return err(400, "Enter a valid IP address or HTTP(S) URL");
            if (n.value("name").toString().isEmpty()) n["name"] = QUrl(n.value("url").toString()).host();
            out.append(n);
        }
        QJsonObject cfg; cfg["nodes"] = out; m_store->setSettingJson("remote.nodes", cfg);
        QJsonObject o; o["nodes"] = publicNodes(out); return o;
    });
    r->registerAsync("GET", "/api/v1/remote/nodes/{id}/status", [findNode, nodeStatus](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const QJsonObject n = findNode(q["id"]);
        if (n.isEmpty()) { reply(404, QJsonObject{{"error", "unknown node"}}); return; }
        nodeStatus(n, [reply](const QJsonObject& row) { reply(200, row); });
    });
    r->registerAsync("GET", "/api/v1/remote/overview", [this, nodes, nodeStatus](const QJsonObject&, const Q&, RestApiServer::Reply reply) {
        const QJsonArray list = nodes();
        struct Acc { QHash<QString, QJsonObject> rows; int pending; bool replied = false; };
        auto acc = std::make_shared<Acc>(); acc->pending = list.size();
        auto send = [this, acc, list, reply]() {
            if (acc->replied) return; acc->replied = true;
            QJsonArray rows;
            for (const QJsonValue& v : list) { const QJsonObject n = v.toObject(); const QString id = n.value("id").toString();
                if (acc->rows.contains(id)) rows.append(acc->rows[id]); else { QJsonObject r2 = n; r2["reachable"] = false; r2["error"] = "timeout"; rows.append(r2); } }
            QJsonObject o; o["nodes"] = rows; o["self"] = m_sessions->nodeInfo(); o["failover"] = m_redundancy->wsPayload(); o["timeMs"] = double(QDateTime::currentMSecsSinceEpoch());
            reply(200, o);
        };
        if (list.isEmpty()) { send(); return; }
        QTimer::singleShot(5000, this, send);
        for (const QJsonValue& v : list) nodeStatus(v.toObject(), [acc, send](const QJsonObject& row) { acc->rows[row.value("id").toString()] = row; if (--acc->pending <= 0) send(); });
    });
    // Forward a call to a node: {method, path, body} → upstream status + JSON. Path must start with a whitelisted prefix.
    r->registerAsync("POST", "/api/v1/remote/nodes/{id}/proxy", [this, findNode, nodeUrl](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const QJsonObject n = findNode(q["id"]);
        if (n.isEmpty()) { reply(404, QJsonObject{{"error", "unknown node"}}); return; }
        QString path = body.value("path").toString().trimmed();
        if (path.startsWith("/api/v1")) path = path.mid(7);
        if (!path.startsWith('/')) path = "/" + path;
        bool allowed = false;
        for (const QString& p : kProxyPrefixes) if (path == p || path.startsWith(p + "/") || path.startsWith(p + "?")) { allowed = true; break; }
        if (!allowed || path.contains("..")) { reply(403, QJsonObject{{"error", "path not allowed through the proxy: " + path}}); return; }
        const QString method = body.value("method").toString("GET").toUpper();
        if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") { reply(400, QJsonObject{{"error", "method must be GET, POST, PUT or DELETE"}}); return; }
        m_redundancy->fetchJson(method, nodeUrl(n) + "/api/v1" + path, n.value("token").toString(), body.value("body").toObject(), 15000,
                                [reply, path](int http, const QJsonObject& j, const QString& e) {
            if (http == 0) { reply(502, QJsonObject{{"error", "node unreachable: " + e}}); return; }
            QJsonObject o = j; if (http >= 400 && !o.contains("error")) o["error"] = e.isEmpty() ? QString("HTTP %1 from node").arg(http) : e;
            reply(upstreamStatus(http), upstreamBody(http, o, "proxy"));
        });
    });
    // ------------------------------------------------------------------ remote pad: mappable, themed buttons that drive this
    // station ("local") or any remote node. A button is {id, label, node, channel?, method, path, body, role, confirm, group, hotkey}; pressing it
    // makes that REST call through the same HTTP helper as the proxy (path limited to operator prefixes, `{channel}` filled from the button).
    static const QStringList kPadPrefixes = {"/channels", "/schedule", "/status", "/spots", "/live", "/outputs", "/overlays", "/mux", "/library", "/inputs", "/flight"};
    static const QStringList kPadRoles = {"neutral", "go", "stop", "warn", "info", "accent"};
    auto padThemes = []() {
        auto th = [](const char* id, const char* name, const char* bg, const char* panel, const char* border, const char* text, const char* muted, const char* neutral, const char* go, const char* stop, const char* warn, const char* info, const char* accent) {
            QJsonObject o; o["id"] = id; o["name"] = name; o["bg"] = bg; o["panel"] = panel; o["border"] = border; o["text"] = text; o["muted"] = muted;
            o["neutral"] = neutral; o["go"] = go; o["stop"] = stop; o["warn"] = warn; o["info"] = info; o["accent"] = accent; return o;
        };
        QJsonArray a;
        a.append(th("studio", "Studio", "#0f1621", "#18222f", "#2a3a4e", "#e6edf5", "#8a9bb0", "#33445a", "#1f9d55", "#d63b3b", "#d9922b", "#2f7fd1", "#4aa3ff"));
        a.append(th("broadcast", "Broadcast", "#0b0b0d", "#17171b", "#34343c", "#f2f2f2", "#9a9aa5", "#3a3a44", "#18a058", "#e0241f", "#f0a500", "#3a86d8", "#ff4d3d"));
        a.append(th("night", "Night", "#06090d", "#0d131a", "#1b2733", "#b9c6d3", "#5f7183", "#1d2a38", "#16724a", "#9c2f2f", "#99681d", "#235c96", "#3b82c4"));
        a.append(th("daylight", "Daylight", "#eef1f5", "#ffffff", "#c9d1db", "#1b2430", "#5c6b7c", "#d5dce6", "#2e9e5b", "#d64545", "#e0a030", "#3b84d6", "#2563eb"));
        a.append(th("neon", "Neon", "#0a0714", "#150f26", "#33265a", "#f3ecff", "#9a8ab8", "#2a2050", "#12c98a", "#ff3d7f", "#ffb020", "#3d9bff", "#b86bff"));
        return a;
    };
    auto padCatalog = []() {
        auto it = [](const char* label, const char* group, const char* method, const char* path, QJsonObject body, const char* role, bool confirm, bool needChannel) {
            QJsonObject o; o["label"] = label; o["group"] = group; o["method"] = method; o["path"] = path; o["body"] = body; o["role"] = role; o["confirm"] = confirm; o["needsChannel"] = needChannel; return o;
        };
        auto act = [](const char* a) { QJsonObject b; b["action"] = a; return b; };
        QJsonArray a;
        a.append(it("Skip", "Transport", "POST", "/channels/{channel}/transport", act("skip"), "warn", true, true));
        a.append(it("Play", "Transport", "POST", "/channels/{channel}/transport", act("play"), "go", false, true));
        a.append(it("Pause", "Transport", "POST", "/channels/{channel}/transport", act("pause"), "neutral", false, true));
        a.append(it("Stop", "Transport", "POST", "/channels/{channel}/transport", act("stop"), "stop", true, true));
        a.append(it("Arm", "Automation", "POST", "/channels/{channel}/transport", act("arm"), "go", true, true));
        a.append(it("Disarm", "Automation", "POST", "/channels/{channel}/transport", act("disarm"), "stop", true, true));
        a.append(it("Load hour", "Automation", "POST", "/channels/{channel}/transport", act("loadhour"), "info", false, true));
        a.append(it("Snap to schedule", "Automation", "POST", "/channels/{channel}/transport", act("snap"), "info", true, true));
        a.append(it("Restart engine", "Automation", "POST", "/channels/{channel}/transport", act("restart"), "stop", true, true));
        a.append(it("Go live: Facebook", "Streaming", "POST", "/live/facebook/start", QJsonObject(), "go", true, false));
        a.append(it("End live: Facebook", "Streaming", "POST", "/live/facebook/stop", QJsonObject(), "stop", true, false));
        a.append(it("Status", "Info", "GET", "/status", QJsonObject(), "neutral", false, false));
        return a;
    };
    auto cleanPad = [](const QJsonObject& in, const QJsonArray& themes) {
        QJsonObject out; QString theme = in.value("theme").toString("studio"); bool ok = false; for (const QJsonValue& t : themes) if (t.toObject().value("id").toString() == theme) ok = true; out["theme"] = ok ? theme : "studio";
        QJsonArray btns; QSet<QString> ids; static const QRegularExpression idRe("^[A-Za-z0-9_-]{1,40}$"), chRe("^[A-Za-z0-9_.-]{0,60}$");
        for (const QJsonValue& v : in.value("buttons").toArray()) {
            if (btns.size() >= 96) break;
            const QJsonObject b = v.toObject(); QJsonObject o;
            QString id = b.value("id").toString(); if (!idRe.match(id).hasMatch() || ids.contains(id)) id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); ids.insert(id);
            QString path = b.value("path").toString().trimmed(); if (path.startsWith("/api/v1")) path = path.mid(7); if (!path.startsWith('/')) path = "/" + path;
            bool allowed = false; for (const QString& pre : kPadPrefixes) if (path == pre || path.startsWith(pre + "/") || path.startsWith(pre + "?")) allowed = true;
            if (!allowed || path.contains("..") || path.contains("//")) continue;
            QString method = b.value("method").toString("POST").toUpper(); if (method != "GET" && method != "POST" && method != "PUT" && method != "DELETE") method = "POST";
            const QString ch = b.value("channel").toString().trimmed(); if (!chRe.match(ch).hasMatch()) continue;
            QString role = b.value("role").toString("neutral"); if (!kPadRoles.contains(role)) role = "neutral";
            o["id"] = id; o["label"] = b.value("label").toString().trimmed().left(28); if (o.value("label").toString().isEmpty()) o["label"] = path;
            o["node"] = b.value("node").toString("local"); o["channel"] = ch; o["method"] = method; o["path"] = path; o["body"] = b.value("body").toObject();
            o["role"] = role; o["confirm"] = b.value("confirm").toBool(false); o["group"] = b.value("group").toString("Main").trimmed().left(24); o["hotkey"] = b.value("hotkey").toString().trimmed().left(12);
            btns.append(o);
        }
        out["buttons"] = btns; return out;
    };
    auto padLog = std::make_shared<QJsonArray>();
    r->registerRoute("GET", "/api/v1/remote/pad", [this, nodes, padThemes, padCatalog](const QJsonObject&, const Q&) {
        QJsonObject cur = m_store->settingJson("remote.pad"); if (!cur.contains("buttons")) cur["buttons"] = QJsonArray();
        if (cur.value("theme").toString().isEmpty()) cur["theme"] = "studio";
        QJsonArray ns; { QJsonObject l; l["id"] = "local"; l["name"] = "This station"; ns.append(l); }
        for (const QJsonValue& v : nodes()) { QJsonObject n; n["id"] = v.toObject().value("id"); n["name"] = v.toObject().value("name"); ns.append(n); }
        QJsonArray chans; for (const ChannelProfile& p : m_store->channels()) { if (p.extra.value("hidden").toBool(false)) continue; QJsonObject c; c["id"] = p.id; c["name"] = p.name; chans.append(c); }
        cur["themes"] = padThemes(); cur["catalog"] = padCatalog(); cur["nodes"] = ns; cur["channels"] = chans; return cur;
    });
    r->registerRoute("PUT", "/api/v1/remote/pad", [this, padThemes, cleanPad](const QJsonObject& body, const Q&) {
        const QJsonObject clean = cleanPad(body, padThemes()); m_store->setSettingJson("remote.pad", clean);
        QJsonObject o = clean; o["saved"] = true; return o;
    });
    r->registerRoute("GET", "/api/v1/remote/pad/log", [padLog](const QJsonObject&, const Q&) { QJsonObject o; o["log"] = *padLog; return o; });
    r->registerAsync("POST", "/api/v1/remote/pad/{id}/press", [this, findNode, nodeUrl, padLog](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        QJsonObject btn; for (const QJsonValue& v : m_store->settingJson("remote.pad").value("buttons").toArray()) if (v.toObject().value("id").toString() == q["id"]) btn = v.toObject();
        if (btn.isEmpty()) { reply(404, QJsonObject{{"error", "no such pad button"}}); return; }
        QString path = btn.value("path").toString(); const QString ch = btn.value("channel").toString();
        if (path.contains("{channel}")) { if (ch.isEmpty()) { reply(409, QJsonObject{{"error", "this button needs a channel"}}); return; } path.replace("{channel}", QString::fromLatin1(QUrl::toPercentEncoding(ch))); }
        const QString nodeId = btn.value("node").toString("local"); QString url, token;
        if (nodeId == "local" || nodeId.isEmpty()) { url = QString("http://127.0.0.1:%1").arg(m_opt.restPort); token = m_store->setting("rest.token"); }
        else { const QJsonObject n = findNode(nodeId); if (n.isEmpty()) { reply(404, QJsonObject{{"error", "unknown node for this button"}}); return; } url = nodeUrl(n); token = n.value("token").toString(); }
        const QString method = btn.value("method").toString("POST"); const QString label = btn.value("label").toString(); auto t = std::make_shared<QElapsedTimer>(); t->start();
        qInfo().noquote() << "[Pad]" << label << "->" << nodeId << method << path;
        m_redundancy->fetchJson(method, url + "/api/v1" + path, token, btn.value("body").toObject(), 15000, [reply, padLog, t, btn, nodeId, method, path, label](int http, const QJsonObject& j, const QString& e) {
            QJsonObject row; row["tMs"] = double(QDateTime::currentMSecsSinceEpoch()); row["button"] = btn.value("id"); row["label"] = label; row["node"] = nodeId; row["method"] = method; row["path"] = path; row["http"] = http; row["ms"] = double(t->elapsed());
            const bool ok = http >= 200 && http < 300; row["ok"] = ok; if (!ok) row["error"] = j.value("error").toString(e.isEmpty() ? QString("HTTP %1").arg(http) : e);
            padLog->append(row); while (padLog->size() > 60) padLog->removeFirst();
            if (http == 0) { reply(502, QJsonObject{{"error", "target unreachable: " + e}, {"ok", false}}); return; }
            QJsonObject o; o["ok"] = ok; o["http"] = http; o["ms"] = row.value("ms"); o["response"] = j; if (!ok) o["error"] = row.value("error");
            reply(ok ? 200 : (upstreamRejectedToken(http) ? 502 : http), o);
        });
    });
    // Push one of our sessions to a node (it upserts by id), or pull one of the node's sessions into our registry.
    r->registerAsync("POST", "/api/v1/remote/nodes/{id}/push-session/{sid}", [this, findNode, nodeUrl](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const QJsonObject n = findNode(q["id"]);
        if (n.isEmpty()) { reply(404, QJsonObject{{"error", "unknown node"}}); return; }
        bool f = false; const QJsonObject wire = m_sessions->exportBundle(q["sid"], &f);
        if (!f) { reply(404, QJsonObject{{"error", "unknown session"}}); return; }
        m_redundancy->fetchJson("POST", nodeUrl(n) + "/api/v1/sessions/receive", n.value("token").toString(), wire, 60000, [reply](int http, const QJsonObject& j, const QString& e) {
            if (http == 0) { reply(502, QJsonObject{{"error", "push failed: " + e}}); return; }
            QJsonObject o = j; if (http >= 400 && !o.contains("error")) o["error"] = e; reply(upstreamStatus(http), upstreamBody(http, o, "push"));
        });
    });
    r->registerAsync("POST", "/api/v1/remote/nodes/{id}/pull-session/{sid}", [this, findNode, nodeUrl](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const QJsonObject n = findNode(q["id"]);
        if (n.isEmpty()) { reply(404, QJsonObject{{"error", "unknown node"}}); return; }
        if (!SessionsManager::isValidSessionId(q["sid"])) { reply(400, QJsonObject{{"error", "invalid session id"}}); return; }   // goes into the node's URL: keep it a plain id
        m_redundancy->fetchJson("GET", nodeUrl(n) + "/api/v1/sessions/" + q["sid"] + "?data=1", n.value("token").toString(), QJsonObject(), 60000, [this, reply](int http, const QJsonObject& j, const QString& e) {
            if (http == 0) { reply(502, QJsonObject{{"error", "pull failed: " + e}}); return; }
            if (http >= 400) { QJsonObject o = j; if (!o.contains("error")) o["error"] = e; reply(upstreamStatus(http), upstreamBody(http, o, "pull")); return; }
            const QJsonObject res = m_sessions->receiveBundle(j);
            if (res.contains("error")) { reply(400, res); return; }
            reply(200, res);
        });
    });

    m_redundancy->start();
    qInfo() << "[Supervisor] redundancy routes registered: sessions, failover (" << m_redundancy->role() << "), remote nodes";
}
