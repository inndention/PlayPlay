// SupervisorCore — the PlayPlay OBS / Star suites: render nodes, Star nodes, spot studio, publisher and agents.
// Constructs m_render, m_starNodes, m_publisher, m_studio, m_agents and exposes them over REST (/api/v1/...) plus the
// PlayPlay OBS station-compatible routes the Star tooling calls (/api/newsroom/*, /api/stations*, /api/nodes*, /api/fleet,
// /api/social/*, /api/publish/*, /api/op/*, /api/ai/*). Every state change is broadcast on the WS with its own type.
#include "supervisorcore.h"
#include "restapiserver.h"
#include "channelmanager.h"
#include "ingestmanager.h"
#include "spotgenerator.h"
#include "starclient.h"
#include "renderfarm.h"
#include "starnodes.h"
#include "publisher.h"
#include "spotstudio.h"
#include "agentsuite.h"
#include "aimodels.h"
#include "suitelogic.h"
#include "failoverpair.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDir>
#include <QJsonArray>
#include <QMap>
#include <QUrl>
#include <QJsonDocument>
#include <QStorageInfo>
#include <memory>

using namespace sched;
using Q = QHash<QString, QString>;
using Reply = RestApiServer::Reply;

static QJsonObject fail(int status, const QString& m) { QJsonObject e; e["error"] = m; e["status"] = status; return e; }
static int statusOf(const QJsonObject& o, int def = 200) { return o.contains("error") ? o.value("status").toInt(400) : def; }

void SupervisorCore::registerSuiteRoutes() {
    RestApiServer* r = m_rest.get();

    // --- construction ------------------------------------------------------------------------------------
    AiModels::migrate(m_store.get());                   // legacy model names (gpt-4o, claude-3, ...) -> default; per-feature keys -> ai.config
    m_ai = std::make_unique<AiModels>(m_store.get());
    m_render = std::make_unique<RenderFarm>(m_store.get(), m_star.get(), m_opt.dataDir, QDir(uploadDir()).filePath("render"), m_ingest->ffmpegPath(), m_ingest->ffprobePath());
    m_render->setIngest([this](const QString& path, const QString& type, const QJsonObject& meta) { m_ingest->enqueue(path, type, "render", meta, true); });
    m_render->setLocalSpot([this](const QJsonObject& req, RenderFarm::Done done) { return m_spots->generate(req, done); });
    m_starNodes = std::make_unique<StarNodes>(m_store.get());
    m_publisher = std::make_unique<Publisher>(m_store.get());
    m_studio = std::make_unique<SpotStudio>(m_store.get(), m_spots.get(), m_render.get(), m_opt.dataDir, QDir(uploadDir()).filePath("studio"));
    m_studio->setPublisher(m_publisher.get());
    m_agents = std::make_unique<AgentSuite>(m_store.get(), m_star.get(), m_starNodes.get());

    auto ws = [this](const QString& type, const QJsonObject& p) { broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), p, type); };
    connect(m_render.get(), &RenderFarm::event, this, ws);
    connect(m_starNodes.get(), &StarNodes::event, this, ws);
    connect(m_publisher.get(), &Publisher::event, this, ws);
    connect(m_studio.get(), &SpotStudio::event, this, ws);
    connect(m_agents.get(), &AgentSuite::event, this, ws);
    connect(m_render.get(), &RenderFarm::alert, this, [this](const QString& l, const QString& m) { pushAlert("render", l, m); });
    connect(m_starNodes.get(), &StarNodes::alert, this, [this](const QString& l, const QString& m) { pushAlert("star", l, m); });
    connect(m_publisher.get(), &Publisher::alert, this, [this](const QString& l, const QString& m) { pushAlert("publish", l, m); });
    connect(m_ingest.get(), &IngestManager::mediaIngested, this, [this](const QString& id, const QString& origin) {
        if (m_render) m_render->onMediaIngested(id, origin);
        if (origin == "autogen" && m_publisher) QTimer::singleShot(1500, this, [this, id]() { if (m_publisher) m_publisher->considerSpot(id); });   // after SpotGenerator has adopted it
    });

    // --- operator commands (the station agent; /api/op/exec; the LLM assistant's tools) ------------------------
    m_agents->setOperator([this](const QString& cmd, const QJsonObject& a, AgentSuite::Reply reply) {
        const QString ch = a.value("channel").toString();
        auto needChannel = [this, &ch, &reply]() { bool f = false; m_store->channel(ch, &f); if (!f) { reply(400, fail(400, ch.isEmpty() ? "channel required" : "no such channel " + ch)); return false; } return true; };
        auto chanRow = [this](const ChannelProfile& p) {
            const QJsonObject ex = m_exec->status(p.id).toJson(), eng = m_telemetry.value(p.id);
            QJsonObject o; o["id"] = p.id; o["name"] = p.name; o["armed"] = ex.value("armed"); o["engineState"] = m_engineState.value(p.id, "OFFLINE");
            o["onAir"] = eng.value("title").toString(ex.value("onAirTitle").toString()); o["positionSec"] = eng.value("positionSec"); o["durationSec"] = eng.value("durationSec");
            o["running"] = m_channels->isChannelRunning(p.id); o["driftSec"] = ex.value("driftSec"); return o;
        };
        if (cmd == "channel_list") { QJsonArray a2; for (const ChannelProfile& p : m_store->channels()) a2.append(chanRow(p)); reply(200, QJsonObject{{"channels", a2}}); return; }
        if (cmd == "on_air") {
            QJsonArray a2;
            for (const ChannelProfile& p : m_store->channels()) {
                if (!ch.isEmpty() && p.id != ch) continue;
                QJsonObject o = chanRow(p); const QJsonObject rl = m_exec->runlistJson(p.id); const qint64 now = QDateTime::currentMSecsSinceEpoch();
                for (const QJsonValue& v : rl.value("current").toObject().value("items").toArray() + rl.value("next").toObject().value("items").toArray()) {
                    const QJsonObject it = v.toObject(); if (qint64(it.value("plannedStartMs").toDouble()) > now) { o["next"] = it.value("title"); o["nextAt"] = it.value("plannedStart"); break; }
                }
                a2.append(o);
            }
            reply(200, QJsonObject{{"channels", a2}}); return;
        }
        if (cmd == "system_health") {
            QJsonObject o; o["uptimeSec"] = double(m_uptime.elapsed() / 1000); int running = 0, connected = 0, n = 0;
            for (const ChannelProfile& p : m_store->channels()) { ++n; if (m_channels->isChannelRunning(p.id)) ++running; if (m_channels->isChannelConnected(p.id)) ++connected; }
            o["channels"] = n; o["enginesRunning"] = running; o["enginesConnected"] = connected;
            const QStorageInfo si(m_opt.dataDir); o["diskFreeGb"] = double(si.bytesAvailable()) / 1e9;
            QJsonArray al; for (int i = qMax(0, int(m_recentAlerts.size()) - 10); i < m_recentAlerts.size(); ++i) al.append(m_recentAlerts[i]); o["recentAlerts"] = al;
            o["render"] = m_render->status().value("counts"); o["publish"] = m_publisher->status().value("counts");
            reply(200, o); return;
        }
        if (cmd == "schedule_hour") {
            if (!needChannel()) return;
            const QDate d = a.contains("date") ? QDate::fromString(a.value("date").toString(), Qt::ISODate) : QDate::currentDate();
            const int h = a.contains("hour") ? a.value("hour").toVariant().toInt() : QTime::currentTime().hour();
            if (!d.isValid() || h < 0 || h > 23) { reply(400, fail(400, "bad date or hour")); return; }
            bool ov = false; const HourSlot s = m_store->effectiveSlot(ch, d, h, &ov);
            QJsonObject o = s.toJson(); o["dateOverride"] = ov; reply(200, o); return;
        }
        if (cmd == "upcoming") {
            if (!needChannel()) return;
            const int count = qBound(1, a.value("count").toVariant().toInt() > 0 ? a.value("count").toVariant().toInt() : 10, 50);
            const QJsonObject rl = m_exec->runlistJson(ch); const qint64 now = QDateTime::currentMSecsSinceEpoch(); QJsonArray out;
            for (const QJsonValue& v : rl.value("current").toObject().value("items").toArray() + rl.value("next").toObject().value("items").toArray()) {
                const QJsonObject it = v.toObject(); if (qint64(it.value("plannedEndMs").toDouble()) < now) continue;
                out.append(QJsonObject{{"title", it.value("title")}, {"plannedStart", it.value("plannedStart")}, {"type", it.value("type")}, {"durationSec", it.value("durationSec")}, {"mediaId", it.value("mediaId")}});
                if (out.size() >= count) break;
            }
            reply(200, QJsonObject{{"channel", ch}, {"upcoming", out}}); return;
        }
        if (cmd == "library_stats") {
            QJsonObject byType; int total = 0, archived = 0; for (const MediaRecord& m : m_store->allMedia(true)) { ++total; if (m.archived) ++archived; else byType[m.type] = byType.value(m.type).toInt() + 1; }
            reply(200, QJsonObject{{"total", total}, {"archived", archived}, {"byType", byType}}); return;
        }
        if (cmd == "find_media") {
            const QString q = a.value("q").toString(); if (q.trimmed().isEmpty()) { reply(400, fail(400, "q required")); return; }
            QJsonArray out; for (const MediaRecord& m : m_store->searchMedia(q, qBound(1, a.value("limit").toVariant().toInt() > 0 ? a.value("limit").toVariant().toInt() : 20, 100)))
                out.append(QJsonObject{{"id", m.id}, {"title", m.title}, {"type", m.type}, {"durationSec", m.durationSec}, {"archived", m.archived}});
            reply(200, QJsonObject{{"results", out}}); return;
        }
        if (cmd == "as_played_today") {
            if (!needChannel()) return;
            const qint64 from = QDateTime(QDate::currentDate(), QTime(0, 0)).toMSecsSinceEpoch(), to = QDateTime::currentMSecsSinceEpoch();
            QJsonArray out; for (const AsRunRow& row : m_store->asRun(ch, from, to, 500)) { if (row.eventType != "START") continue; out.append(QJsonObject{{"title", row.title}, {"type", row.type}, {"startMs", double(row.actualStartMs)}, {"varianceMs", double(row.varianceMs)}, {"status", row.status}}); }
            reply(200, QJsonObject{{"channel", ch}, {"starts", out.size()}, {"rows", out}}); return;
        }
        if (cmd == "expiring_soon") {
            const int days = qBound(1, a.value("days").toVariant().toInt() > 0 ? a.value("days").toVariant().toInt() : 14, 365);
            const qint64 now = QDateTime::currentMSecsSinceEpoch(), lim = now + qint64(days) * 86400000; QJsonArray out;
            for (const MediaRecord& m : m_store->allMedia(false)) if (m.availableEndMs > now && m.availableEndMs < lim) out.append(QJsonObject{{"id", m.id}, {"title", m.title}, {"type", m.type}, {"endsMs", double(m.availableEndMs)}});
            reply(200, QJsonObject{{"days", days}, {"media", out}}); return;
        }
        if (cmd == "spots_status") { QJsonObject s = m_spots->status(); s.remove("recent"); reply(200, s); return; }
        if (cmd == "render_status") { QJsonObject s = m_render->status(); s["jobs"] = m_render->jobs("active", 20); reply(200, s); return; }
        if (cmd == "publish_queue") { reply(200, QJsonObject{{"status", m_publisher->status()}, {"waiting", m_publisher->queue("needs_attention")}, {"pending", m_publisher->queue("waiting")}}); return; }
        if (cmd == "star_nodes") { reply(200, QJsonObject{{"nodes", m_starNodes->nodes()}}); return; }
        // --- actions -----------------------------------------------------------------------------------
        if (cmd == "skip_current") { if (!needChannel()) return; const bool ok = transport(ch, "skip", {}); reply(ok ? 200 : 409, ok ? QJsonObject{{"ok", true}, {"channel", ch}} : fail(409, "engine not connected")); return; }
        if (cmd == "arm" || cmd == "disarm" || cmd == "snap") { if (!needChannel()) return; onUiCommand(ch, cmd, QJsonObject(), "agent"); reply(200, QJsonObject{{"ok", true}, {"channel", ch}, {"action", cmd}}); return; }
        if (cmd == "run_spot_job") { const QJsonObject o = m_spots->runJob(a.value("id").toString(), SpotGenerator::Done()); reply(statusOf(o), o); return; }
        if (cmd == "enforce_markers") { reply(200, m_spots->enforceMarkersNow()); return; }
        if (cmd == "publish_approve") { const QJsonObject o = m_publisher->act(a.value("id").toString(), "approve", QJsonObject(), "agent"); reply(statusOf(o), o); return; }
        // --- the whole system through this station's own REST surface (credentials hidden, operator areas only) ------------
        auto self = [this, reply](const QString& method, const QString& path, const QJsonObject& body) {
            QString why; if (!suites::agentApiAllowed(method, path, &why)) { reply(403, fail(403, "not allowed for an agent: " + why)); return; }
            QString p = path.trimmed(); if (p.startsWith("/api/v1")) p = p.mid(7); if (!p.startsWith('/')) p = "/" + p;
            m_redundancy->fetchJson(method, QString("http://127.0.0.1:%1/api/v1").arg(m_opt.restPort) + p, m_store->setting("rest.token"), body, 20000, [reply](int http, const QJsonObject& j, const QString& e) {
                if (http == 0) { reply(502, fail(502, "station REST unreachable: " + e)); return; }
                QJsonObject o = suites::redactSecrets(j).toObject();
                const QByteArray raw = QJsonDocument(o).toJson(QJsonDocument::Compact);
                if (raw.size() > 120000) { QJsonObject t; t["truncated"] = true; t["bytes"] = raw.size(); t["note"] = "reply too large: ask for a narrower route (channel, id, limit)"; t["preview"] = QString::fromUtf8(raw.left(4000)); reply(http >= 200 && http < 300 ? 200 : http, t); return; }
                if (http >= 400 && !o.contains("error")) o["error"] = e.isEmpty() ? QString("HTTP %1").arg(http) : e;
                reply(http >= 200 && http < 300 ? 200 : http, o);
            });
        };
        if (cmd == "capabilities") {
            const QString grp = a.value("group").toString().trimmed().toLower(); QMap<QString, QJsonArray> groups;
            for (const QString& k : m_rest->routeKeys()) {
                const int sp = k.indexOf(' '); const QString m = k.left(sp), path = k.mid(sp + 1); const QString g = suites::routeGroup(path); QString why;
                const bool isGet = m == "GET"; const bool ok = suites::agentApiAllowed(m, path, &why);
                QJsonObject r; r["method"] = m; r["path"] = path; r["agent"] = !ok ? "blocked" : isGet ? "api_get" : "api_call";
                if (g == "settings" && !isGet) continue;
                groups[g].append(r);
            }
            QJsonObject o;
            if (grp.isEmpty()) { QJsonObject c; for (auto it = groups.begin(); it != groups.end(); ++it) c[it.key()] = it.value().size(); o["groups"] = c; o["note"] = "ask again with {group} for the routes; api_get reads, api_call writes operator areas"; }
            else { if (!groups.contains(grp)) { reply(404, fail(404, "no such group: " + grp)); return; } o["group"] = grp; o["routes"] = groups.value(grp); }
            o["commands"] = suites::operatorCommands(); reply(200, o); return;
        }
        if (cmd == "api_get") { if (a.value("path").toString().isEmpty()) { reply(400, fail(400, "path required")); return; } self("GET", a.value("path").toString(), QJsonObject()); return; }
        if (cmd == "api_call") {
            const QString m = a.value("method").toString("POST").toUpper(); if (a.value("path").toString().isEmpty()) { reply(400, fail(400, "path required")); return; }
            if (m != "POST" && m != "PUT") { reply(400, fail(400, "method must be POST or PUT")); return; }
            self(m, a.value("path").toString(), a.value("body").toObject()); return;
        }
        if (cmd == "outputs") { self("GET", ch.isEmpty() ? "/outputs" : "/channels/" + QString::fromLatin1(QUrl::toPercentEncoding(ch)) + "/outputs", QJsonObject()); return; }
        if (cmd == "output_control") {
            if (!needChannel()) return; const QString name = a.value("name").toString(), op = a.value("op").toString();
            if (name.isEmpty() || (op != "start" && op != "stop" && op != "restart")) { reply(400, fail(400, "name and op (start | stop | restart) required")); return; }
            self("POST", "/channels/" + QString::fromLatin1(QUrl::toPercentEncoding(ch)) + "/outputs/" + QString::fromLatin1(QUrl::toPercentEncoding(name)) + "/" + op, QJsonObject()); return;
        }
        if (cmd == "facebook_status") { self("GET", "/live/facebook", QJsonObject()); return; }
        if (cmd == "facebook_go_live") { self("POST", "/live/facebook/start", QJsonObject()); return; }
        if (cmd == "facebook_end") { self("POST", "/live/facebook/stop", QJsonObject()); return; }
        if (cmd == "pad_list") { self("GET", "/remote/pad", QJsonObject()); return; }
        if (cmd == "pad_press") { const QString id = a.value("id").toString(); if (id.isEmpty()) { reply(400, fail(400, "id required")); return; } self("POST", "/remote/pad/" + QString::fromLatin1(QUrl::toPercentEncoding(id)) + "/press", QJsonObject()); return; }
        if (cmd == "spot_generate") { const QString rk = a.value("recipe").toString(); if (rk.isEmpty()) { reply(400, fail(400, "recipe required")); return; } QJsonObject b; b["recipe"] = rk; b["wait"] = false; self("POST", "/spots/generate", b); return; }
        if (cmd == "system_stats") { self("GET", "/system/stats", QJsonObject()); return; }
        if (cmd == "readiness") { self("GET", "/readiness", QJsonObject()); return; }
        if (cmd == "flight_plan") { if (!needChannel()) return; self("GET", "/flightplan/" + QString::fromLatin1(QUrl::toPercentEncoding(ch)), QJsonObject()); return; }
        reply(400, fail(400, "unknown command " + cmd));
    });

    m_render->start(); m_starNodes->start(); m_publisher->start(); m_agents->start();

    auto async = [](Reply reply) { return [reply](const QJsonObject& o) { reply(statusOf(o), o); }; };

    // =====================================================================================================
    // render
    // =====================================================================================================
    r->registerRoute("GET", "/api/v1/render/status", [this](const QJsonObject&, const Q&) { return m_render->status(); });
    r->registerRoute("GET", "/api/v1/render/config", [this](const QJsonObject&, const Q&) { return m_render->config(); });
    r->registerRoute("PUT", "/api/v1/render/config", [this](const QJsonObject& b, const Q&) { return m_render->setConfig(b); });
    r->registerRoute("GET", "/api/v1/render/nodes", [this](const QJsonObject&, const Q&) { return QJsonObject{{"nodes", m_render->nodes()}}; });
    r->registerRoute("PUT", "/api/v1/render/nodes", [this](const QJsonObject& b, const Q&) { return QJsonObject{{"nodes", m_render->setNodes(b.value("nodes").toArray())}}; });
    r->registerRoute("POST", "/api/v1/render/nodes", [this](const QJsonObject& b, const Q&) { return m_render->saveNode(b.value("node").isObject() ? b.value("node").toObject() : b); });
    r->registerRoute("PUT", "/api/v1/render/nodes/{id}", [this](const QJsonObject& b, const Q& q) { return m_render->saveNode(b, q["id"]); });
    r->registerRoute("DELETE", "/api/v1/render/nodes/{id}", [this](const QJsonObject&, const Q& q) { if (!m_render->deleteNode(q["id"])) return fail(404, "no such node (discovered and built-in nodes cannot be removed)"); return QJsonObject{{"deleted", q["id"]}}; });
    r->registerAsync("POST", "/api/v1/render/probe", [this, async](const QJsonObject&, const Q&, Reply reply) { m_render->probeAll(async(reply)); });
    r->registerAsync("POST", "/api/v1/render/nodes/{id}/probe", [this, async](const QJsonObject&, const Q& q, Reply reply) { m_render->probe(q["id"], async(reply)); });
    r->registerAsync("GET", "/api/v1/render/templates", [this, async](const QJsonObject&, const Q&, Reply reply) { m_render->templates(async(reply)); });
    r->registerRoute("GET", "/api/v1/render/jobs", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"jobs", m_render->jobs(q.value("state"), q.value("limit", "200").toInt())}}; });
    r->registerRoute("GET", "/api/v1/render/jobs/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject j = m_render->job(q["id"], &f); return f ? j : fail(404, "no such job"); });
    r->registerRoute("POST", "/api/v1/render/jobs", [this](const QJsonObject& b, const Q&) { return m_render->submit(b); });
    r->registerRoute("POST", "/api/v1/render/jobs/clear", [this](const QJsonObject&, const Q&) { return QJsonObject{{"cleared", m_render->clearFinished()}}; });
    r->registerRoute("POST", "/api/v1/render/jobs/{id}/cancel", [this](const QJsonObject&, const Q& q) { if (!m_render->cancel(q["id"])) return fail(409, "no running job with that id"); bool f; return m_render->job(q["id"], &f); });
    r->registerRoute("DELETE", "/api/v1/render/jobs/{id}", [this](const QJsonObject&, const Q& q) { if (!m_render->remove(q["id"])) return fail(404, "no such job"); return QJsonObject{{"deleted", q["id"]}}; });
    // station-compatible newsroom routes
    r->registerAsync("GET", "/api/newsroom/studio", [this](const QJsonObject&, const Q&, Reply reply) { m_render->studioHealth([reply](const QJsonObject& o) { reply(200, o); }); });
    r->registerRoute("GET", "/api/newsroom/render-request/{id}", [this](const QJsonObject&, const Q& q) { return m_render->renderRequest(q["id"]); });
    r->registerAsync("POST", "/api/newsroom/spot", [this](const QJsonObject& b, const Q&, Reply reply) { m_render->newsroomSpot(b, reply); });
    r->registerRoute("GET", "/api/newsroom/status", [this](const QJsonObject&, const Q&) {
        const QJsonObject c = m_render->config(); QJsonObject o; o["available"] = true; o["recipes"] = m_spots->recipes(); o["story_engine_review_gate"] = true;
        o["studio"] = QJsonObject{{"mode", c.value("studioMode")}, {"enabled", c.value("studioMode").toString() != "off"}}; return o; });

    // =====================================================================================================
    // Star nodes
    // =====================================================================================================
    r->registerRoute("GET", "/api/v1/starnodes", [this](const QJsonObject&, const Q&) {
        QJsonObject allow; QJsonArray a, f, s; for (const QString& x : suites::starNodeActions()) a.append(x); for (const QString& x : suites::fleetReadCommands()) f.append(x); for (const QString& x : suites::stationAllowPatterns()) s.append(x);
        QJsonArray sv; for (const QString& x : suites::restartableServices()) sv.append(x);
        allow["actions"] = a; allow["fleetCommands"] = f; allow["stationPaths"] = s; allow["services"] = sv;
        return QJsonObject{{"nodes", m_starNodes->nodes()}, {"config", m_starNodes->config()}, {"allow", allow}, {"stations", m_starNodes->stations()}};
    });
    r->registerRoute("GET", "/api/v1/starnodes/config", [this](const QJsonObject&, const Q&) { return m_starNodes->config(); });
    r->registerRoute("PUT", "/api/v1/starnodes/config", [this](const QJsonObject& b, const Q&) { return m_starNodes->setConfig(b); });
    r->registerRoute("GET", "/api/v1/starnodes/audit", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"audit", m_starNodes->audit(q.value("limit", "200").toInt())}}; });
    r->registerAsync("POST", "/api/v1/starnodes/enrol", [this](const QJsonObject& b, const Q&, Reply reply) { m_starNodes->enrol(b, reply); });
    r->registerAsync("POST", "/api/v1/starnodes/probe", [this, async](const QJsonObject&, const Q&, Reply reply) { m_starNodes->probeAll(async(reply)); });
    r->registerAsync("POST", "/api/v1/starnodes/{id}/probe", [this, async](const QJsonObject&, const Q& q, Reply reply) { m_starNodes->probe(q["id"], async(reply)); });
    r->registerAsync("POST", "/api/v1/starnodes/{id}/action", [this](const QJsonObject& b, const Q& q, Reply reply) { m_starNodes->action(q["id"], b.value("action").toString(), b.value("args").toObject(), reply); });
    r->registerRoute("PUT", "/api/v1/starnodes/{id}", [this](const QJsonObject& b, const Q& q) { return m_starNodes->update(q["id"], b); });
    r->registerRoute("DELETE", "/api/v1/starnodes/{id}", [this](const QJsonObject&, const Q& q) { if (!m_starNodes->remove(q["id"])) return fail(404, "no such node"); return QJsonObject{{"deleted", q["id"]}}; });
    // station-compatible: stations, /api/nodes, /api/fleet
    r->registerRoute("GET", "/api/stations", [this](const QJsonObject&, const Q&) { return QJsonObject{{"stations", m_starNodes->stations()}}; });
    r->registerAsync("GET", "/api/stations/{sid}/capabilities", [this](const QJsonObject&, const Q& q, Reply reply) { m_starNodes->stationCapabilities(q["sid"], q.value("force") == "true" || q.value("force") == "1", reply); });
    r->registerAsync("POST", "/api/stations/{sid}/call", [this](const QJsonObject& b, const Q& q, Reply reply) { m_starNodes->stationCall(q["sid"], b.value("method").toString("GET"), b.value("path").toString(), b.value("body"), reply); });
    r->registerRoute("GET", "/api/nodes", [this](const QJsonObject&, const Q&) { return QJsonObject{{"nodes", m_starNodes->stationNodeRows()}}; });
    r->registerAsync("POST", "/api/nodes", [this](const QJsonObject& b, const Q&, Reply reply) { m_starNodes->createStationNode(b, reply); });
    auto patchNode = [this](const QJsonObject& b, const Q& q) { QJsonObject p; for (const char* k : {"name", "notes", "enabled"}) if (b.contains(k)) p[k] = k == QString("enabled") ? QJsonValue(b.value(k).toVariant().toBool()) : b.value(k); return m_starNodes->update(q["nid"], p); };
    r->registerRoute("PATCH", "/api/nodes/{nid}", patchNode);
    r->registerRoute("PUT", "/api/nodes/{nid}", patchNode);
    r->registerRoute("DELETE", "/api/nodes/{nid}", [this](const QJsonObject&, const Q& q) { m_starNodes->remove(q["nid"]); return QJsonObject{{"ok", true}}; });
    r->registerAsync("POST", "/api/nodes/{nid}/refresh", [this](const QJsonObject&, const Q& q, Reply reply) { m_starNodes->refreshInventory(q["nid"], reply); });
    r->registerRoute("GET", "/api/nodes/{nid}/inventory", [this](const QJsonObject&, const Q& q) { return m_starNodes->inventory(q["nid"]); });
    r->registerRoute("GET", "/api/fleet", [this](const QJsonObject&, const Q&) {
        QJsonArray chans;
        for (const ChannelProfile& p : m_store->channels()) {
            const QJsonObject ex = m_exec->status(p.id).toJson();
            chans.append(QJsonObject{{"id", p.id}, {"name", p.name}, {"state", QJsonObject{{"title", m_telemetry.value(p.id).value("title").toString(ex.value("onAirTitle").toString())}, {"armed", ex.value("armed")}, {"engine", m_engineState.value(p.id, "OFFLINE")}}},
                                     {"obs_connected", m_channels->isChannelConnected(p.id)}, {"preview_wanted", m_preview.contains(p.id)}, {"workspace", QJsonValue()}, {"watchdog", QJsonValue()}});
        }
        return QJsonObject{{"channels", chans}, {"obs", QJsonObject{{"note", "no OBS: engines are native"}}}, {"workspace", QJsonObject()}, {"watchdog", QJsonObject{{"reachable", false}, {"services", QJsonArray()}}}, {"nodes", m_starNodes->nodes()}};
    });

    // =====================================================================================================
    // publisher
    // =====================================================================================================
    r->registerRoute("GET", "/api/v1/publish/status", [this](const QJsonObject&, const Q&) { return m_publisher->status(); });
    r->registerRoute("GET", "/api/v1/publish/config", [this](const QJsonObject&, const Q&) { return m_publisher->config(); });
    r->registerRoute("PUT", "/api/v1/publish/config", [this](const QJsonObject& b, const Q&) { return m_publisher->setConfig(b); });
    r->registerAsync("POST", "/api/v1/publish/verify", [this](const QJsonObject&, const Q&, Reply reply) { m_publisher->verify(reply); });
    r->registerRoute("GET", "/api/v1/publish/queue", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"items", m_publisher->queue(q.value("status"), q.value("kind"), q.value("source"), q.value("channel"), q.value("limit", "300").toInt())}}; });
    r->registerRoute("GET", "/api/v1/publish/audit", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"audit", m_publisher->audit(q.value("limit", "200").toInt())}}; });
    r->registerRoute("GET", "/api/v1/publish/items/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject it = m_publisher->item(q["id"], &f); return f ? it : fail(404, "unknown item"); });
    r->registerRoute("POST", "/api/v1/publish/draft", [this](const QJsonObject& b, const Q&) { return m_publisher->draft(b, b.value("actor").toString("operator")); });
    for (const QString& act : {"approve", "hold", "skip", "schedule", "retry", "correct", "retract"})
        r->registerRoute("POST", "/api/v1/publish/items/{id}/" + act, [this, act](const QJsonObject& b, const Q& q) { return m_publisher->act(q["id"], act, b, b.value("actor").toString("operator")); });
    r->registerAsync("POST", "/api/v1/publish/items/{id}/send", [this](const QJsonObject& b, const Q& q, Reply reply) { m_publisher->send(q["id"], b.value("actor").toString("operator"), reply); });
    r->registerAsync("POST", "/api/v1/publish/items/{id}/metrics", [this](const QJsonObject&, const Q& q, Reply reply) { m_publisher->metrics(q["id"], reply); });
    r->registerRoute("DELETE", "/api/v1/publish/items/{id}", [this](const QJsonObject&, const Q& q) { return m_publisher->act(q["id"], "delete", QJsonObject(), "operator"); });
    r->registerRoute("POST", "/api/v1/publish/stop", [this](const QJsonObject& b, const Q&) { if (b.value("reason").toString().trimmed().size() < 3) return fail(400, "give a reason"); return m_publisher->stop(b.value("reason").toString(), b.value("scope").toString("all"), b.value("actor").toString("operator")); });
    r->registerRoute("POST", "/api/v1/publish/release", [this](const QJsonObject& b, const Q&) { return m_publisher->release(b.value("scope").toString("all"), b.value("actor").toString("operator")); });
    // station-compatible: social + publish
    r->registerRoute("GET", "/api/social/config", [this](const QJsonObject&, const Q&) { return m_publisher->socialConfig(); });
    r->registerRoute("POST", "/api/social/config", [this](const QJsonObject& b, const Q&) { return m_publisher->saveSocialConfig(b); });
    r->registerAsync("POST", "/api/social/verify", [this](const QJsonObject&, const Q&, Reply reply) { m_publisher->verify(reply); });
    r->registerRoute("GET", "/api/social/queue", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"posts", m_publisher->socialQueue(q.value("all", "1") != "0")}}; });
    r->registerRoute("POST", "/api/social/queue", [this](const QJsonObject& b, const Q&) { return m_publisher->socialEnqueue(b); });
    r->registerRoute("POST", "/api/social/queue/{pid}/approve", [this](const QJsonObject&, const Q& q) { const QJsonObject o = m_publisher->act(q["pid"], "approve", QJsonObject(), "station-api"); return o.contains("error") ? o : Publisher::socialRow(o); });
    r->registerRoute("POST", "/api/social/queue/{pid}/skip", [this](const QJsonObject&, const Q& q) { const QJsonObject o = m_publisher->act(q["pid"], "skip", QJsonObject(), "station-api"); return o.contains("error") ? o : Publisher::socialRow(o); });
    r->registerRoute("POST", "/api/social/queue/{pid}/retry", [this](const QJsonObject&, const Q& q) { const QJsonObject o = m_publisher->act(q["pid"], "retry", QJsonObject(), "station-api"); return o.contains("error") ? o : Publisher::socialRow(o); });
    r->registerRoute("GET", "/api/social/pulse", [this](const QJsonObject&, const Q&) { return m_publisher->pulse(); });
    r->registerRoute("GET", "/api/publish/config", [this](const QJsonObject&, const Q&) { return m_publisher->publishConfig(); });
    r->registerRoute("GET", "/api/publish/publications", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"publications", m_publisher->publications(q.value("limit", "200").toInt())}}; });
    r->registerRoute("POST", "/api/publish/schedule", [this](const QJsonObject& b, const Q&) {
        QString id = b.value("item_id").toString(b.value("story_id").toString());
        if (id.isEmpty()) { QJsonObject d; d["title"] = b.value("headline"); d["body"] = b.value("body"); d["channels"] = QJsonArray{b.value("platform").toString("facebook") == "website" ? "website" : "facebook"}; const QJsonObject it = m_publisher->draft(d, "station-api"); if (it.contains("error")) return it; id = it.value("id").toString(); }
        if (b.value("publish_at").toString().isEmpty()) { bool f = false; return m_publisher->item(id, &f); }
        QJsonObject s; s["at"] = b.value("publish_at"); return m_publisher->act(id, "schedule", s, "station-api");
    });
    r->registerAsync("POST", "/api/publish/{pid}/send", [this](const QJsonObject&, const Q& q, Reply reply) { m_publisher->send(q["pid"].section(':', 0, 0), "station-api", reply); });
    r->registerAsync("POST", "/api/publish/{pid}/metrics", [this](const QJsonObject&, const Q& q, Reply reply) { m_publisher->metrics(q["pid"].section(':', 0, 0), reply); });
    r->registerRoute("POST", "/api/publish/{pid}/correct", [this](const QJsonObject& b, const Q& q) { return m_publisher->act(q["pid"].section(':', 0, 0), "correct", b, "station-api"); });
    r->registerRoute("POST", "/api/publish/{pid}/retract", [this](const QJsonObject& b, const Q& q) { return m_publisher->act(q["pid"].section(':', 0, 0), "retract", b, "station-api"); });

    // =====================================================================================================
    // spot studio
    // =====================================================================================================
    r->registerAsync("GET", "/api/v1/spotstudio/options", [this](const QJsonObject&, const Q&, Reply reply) { m_studio->options([reply](const QJsonObject& o) { reply(200, o); }); });
    r->registerRoute("GET", "/api/v1/spotstudio/projects", [this](const QJsonObject&, const Q&) { return QJsonObject{{"projects", m_studio->projects()}}; });
    r->registerRoute("POST", "/api/v1/spotstudio/projects", [this](const QJsonObject& b, const Q&) { return m_studio->create(b); });
    r->registerRoute("GET", "/api/v1/spotstudio/projects/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject p = m_studio->project(q["id"], &f); return f ? p : fail(404, "no such project"); });
    r->registerRoute("PUT", "/api/v1/spotstudio/projects/{id}", [this](const QJsonObject& b, const Q& q) { return m_studio->update(q["id"], b); });
    r->registerRoute("DELETE", "/api/v1/spotstudio/projects/{id}", [this](const QJsonObject&, const Q& q) { if (!m_studio->remove(q["id"])) return fail(404, "no such project"); return QJsonObject{{"deleted", q["id"]}}; });
    r->registerRoute("POST", "/api/v1/spotstudio/projects/{id}/generate", [this](const QJsonObject& b, const Q& q) { return m_studio->generate(q["id"], b); });
    r->registerRoute("POST", "/api/v1/spotstudio/projects/{id}/review", [this](const QJsonObject& b, const Q& q) { return m_studio->review(q["id"], b.value("action").toString(), b.value("notes").toString(), b.value("actor").toString("operator")); });
    r->registerRoute("POST", "/api/v1/spotstudio/projects/{id}/publish", [this](const QJsonObject& b, const Q& q) { return m_studio->publish(q["id"], b, b.value("actor").toString("operator")); });
    r->registerRoute("POST", "/api/v1/spots/spot/{mediaId}/revise", [this](const QJsonObject& b, const Q& q) { return m_studio->reviseAutoSpot(q["mediaId"], b.value("notes").toString(), b.value("voice").toString()); });
    r->registerRoute("POST", "/api/newsroom/spot/{mediaId}/revise", [this](const QJsonObject& b, const Q& q) { QJsonObject o = m_studio->reviseAutoSpot(q["mediaId"], b.value("notes").toString(), b.value("voice").toString()); if (!o.contains("error")) { o["started"] = true; o["media_id"] = q["mediaId"]; } return o; });

    // =====================================================================================================
    // agents
    // =====================================================================================================
    r->registerRoute("GET", "/api/v1/agents", [this](const QJsonObject&, const Q&) { return QJsonObject{{"agents", m_agents->agents()}, {"config", m_agents->config()}}; });
    r->registerRoute("GET", "/api/v1/agents/commands", [](const QJsonObject&, const Q&) { return QJsonObject{{"commands", suites::operatorCommands()}}; });
    r->registerRoute("GET", "/api/v1/agents/config", [this](const QJsonObject&, const Q&) { return m_agents->config(); });
    r->registerRoute("PUT", "/api/v1/agents/config", [this](const QJsonObject& b, const Q&) { return m_agents->setConfig(b); });
    r->registerRoute("GET", "/api/v1/agents/tasks", [this](const QJsonObject&, const Q& q) { return QJsonObject{{"tasks", m_agents->tasks(q.value("agent"), q.value("state"), q.value("limit", "100").toInt())}}; });
    r->registerRoute("POST", "/api/v1/agents/tasks", [this](const QJsonObject& b, const Q&) { return m_agents->submit(b); });
    r->registerRoute("POST", "/api/v1/agents/tasks/clear", [this](const QJsonObject&, const Q&) { return QJsonObject{{"cleared", m_agents->clearFinished()}}; });
    r->registerRoute("GET", "/api/v1/agents/tasks/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject t = m_agents->task(q["id"], &f); return f ? t : fail(404, "no such task"); });
    r->registerRoute("DELETE", "/api/v1/agents/tasks/{id}", [this](const QJsonObject&, const Q& q) { if (!m_agents->removeTask(q["id"])) return fail(404, "no such task"); return QJsonObject{{"removed", q["id"]}}; });
    // --- AI models + key (shared by the assistant, spot studio and the spot generator writer) ---------------
    r->registerAsync("GET", "/api/v1/ai/models", [this](const QJsonObject&, const Q& q, Reply reply) {
        const bool all = q.value("all") == "1" || q.value("all") == "true", refresh = q.value("refresh") == "1" || q.value("refresh") == "true";
        m_ai->models(all, refresh, [reply](const QJsonObject& o) { reply(200, o); });
    });
    r->registerRoute("GET", "/api/v1/ai/config", [this](const QJsonObject&, const Q&) { return m_ai->config(); });
    r->registerRoute("PUT", "/api/v1/ai/config", [this](const QJsonObject& b, const Q&) { return m_ai->setConfig(b); });
    // the generic GET /settings/{key} returns raw rows: this exact route answers for ai.config without the key
    r->registerRoute("GET", "/api/v1/settings/ai.config", [this](const QJsonObject&, const Q&) {
        QJsonObject v = m_store->settingJson("ai.config"); v.remove("apiKey"); const QJsonObject k = AiModels::keyStatus(m_store.get()); v["hasKey"] = k.value("hasKey"); v["keySource"] = k.value("keySource");
        QJsonObject o; o["key"] = "ai.config"; o["value"] = QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact)); return o;
    });
    // station-compatible: /api/op/*, /api/ai/*
    r->registerAsync("GET", "/api/op/status", [this](const QJsonObject&, const Q&, Reply reply) {
        auto acc = std::make_shared<QJsonObject>(); auto left = std::make_shared<int>(3);
        for (const QString& c : {QString("on_air"), QString("channel_list"), QString("system_health")}) {
            const QString key = c == "channel_list" ? "channels" : c == "system_health" ? "health" : "on_air";
            m_agents->runOperator(c, QJsonObject(), [acc, left, key, reply](int, const QJsonObject& o) { (*acc)[key] = o; if (--*left == 0) reply(200, *acc); });
        }
    });
    r->registerRoute("GET", "/api/op/commands", [](const QJsonObject&, const Q&) { return QJsonObject{{"commands", suites::operatorCommands()}}; });
    r->registerAsync("POST", "/api/op/exec", [this](const QJsonObject& b, const Q&, Reply reply) { m_agents->runOperator(b.value("command").toString(), b.value("args").toObject(), reply); });
    r->registerRoute("GET", "/api/ai/status", [this](const QJsonObject&, const Q&) { return m_agents->aiStatus(); });
    r->registerAsync("POST", "/api/ai/test", [this](const QJsonObject&, const Q&, Reply reply) { m_agents->llmTest(reply); });
}
