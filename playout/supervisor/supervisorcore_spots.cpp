// SupervisorCore — auto-generated spots: constructs m_spots (SpotGenerator) and exposes recipes,
// configuration, recurring jobs, one-off generation, GEN markers and the news scatter over REST.
// Generation is asynchronous (one render at a time); every state change is broadcast as WS "spots".
#include "supervisorcore.h"
#include "restapiserver.h"
#include "ingestmanager.h"
#include "spotgenerator.h"
#include "aimodels.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <QDebug>
#include <memory>

using namespace sched;
using Q = QHash<QString, QString>;

/// Finished / pending script previews for the polling form of preview-script (newest 24 kept).
struct PreviewBook {
    QHash<QString, QJsonObject> byId; QStringList order;
    void put(const QString& id, const QJsonObject& o) { if (!byId.contains(id)) order.append(id); byId[id] = o; while (order.size() > 24) byId.remove(order.takeFirst()); }
};

void SupervisorCore::registerSpotRoutes() {
    RestApiServer* r = m_rest.get();

    // --- construction ------------------------------------------------------------------------------
    QString scripts = QDir(QCoreApplication::applicationDirPath()).filePath("scripts");
    if (!QFileInfo(QDir(scripts).filePath("spot_tts.py")).exists()) { const QString src = QDir(QCoreApplication::applicationDirPath()).filePath("../scripts"); if (QFileInfo(QDir(src).filePath("spot_tts.py")).exists()) scripts = QDir(src).absolutePath(); }
    m_spots = std::make_unique<SpotGenerator>(m_store.get(), m_opt.dataDir, scripts);
    m_spots->setOutputDir(QDir(uploadDir()).filePath("spots"));
    m_spots->setIngest([this](const QString& path, const QString& type, const QJsonObject& meta) { m_ingest->enqueue(path, type, "autogen", meta); });
    // "schedule" events (bulk marker writes) name their channel so the operator app's weekly grid, which filters on it, refreshes; "spots" status has none
    connect(m_spots.get(), &SpotGenerator::event, this, [this](const QString& type, const QJsonObject& p) { broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, type == "schedule" ? p.value("channelId").toString() : QString(), p, type); });
    connect(m_spots.get(), &SpotGenerator::alert, this, [this](const QString& level, const QString& msg) { pushAlert("spots", level, msg); });
    connect(m_ingest.get(), &IngestManager::mediaIngested, this, [this](const QString& id, const QString& origin) { if (m_spots) m_spots->onMediaIngested(id, origin); });
    m_spots->start();

    // async reply adapter: SpotGenerator::Done -> RestApiServer::Reply (called exactly once)
    auto replyWith = [](RestApiServer::Reply reply) { return [reply](bool ok, const QJsonObject& res) { reply(ok ? 200 : res.value("status").toInt(500), res); }; };

    // --- recipes + config ----------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/spots/recipes", [this](const QJsonObject&, const Q&) { QJsonObject o; o["recipes"] = m_spots->recipes(); return o; });
    r->registerRoute("GET", "/api/v1/spots/config", [this](const QJsonObject&, const Q&) { return m_spots->config(); });
    // Area markets (Martinsville / Reidsville / Danville + operator-defined ones in settings spots.markets): the picker's list, one market in full, and the
    // recipes as that market would localize them. The station default is PUT /spots/config {market: {default: "danville"}}.
    r->registerRoute("GET", "/api/v1/spots/markets", [this](const QJsonObject&, const Q&) { return m_spots->marketInfo(); });
    r->registerRoute("GET", "/api/v1/spots/markets/{key}", [this](const QJsonObject&, const Q& q) {
        const QJsonObject m = m_spots->marketData(q["key"]); if (m.isEmpty()) return err(404, "no such market");
        QJsonObject o = m; o["localizedRecipes"] = m_spots->recipesIn(m.value("key").toString()); return o; });
    r->registerRoute("PUT", "/api/v1/spots/config", [this](const QJsonObject& body, const Q&) {
        QJsonObject b = body; QJsonObject llm = b.value("llm").toObject();
        if (!llm.value("apiKey").toString().trimmed().isEmpty()) {   // one shared OpenRouter key (aimodels): a key typed here becomes the ai.config override
            QJsonObject ai = m_store->settingJson("ai.config"); ai["apiKey"] = llm.value("apiKey").toString().trimmed(); m_store->setSettingJson("ai.config", ai);
        }
        if (b.contains("llm")) { llm.remove("apiKey"); if (llm.contains("model")) llm["model"] = AiModels::model(m_store.get(), llm.value("model").toString()); b["llm"] = llm; }
        return m_spots->setConfig(b); });
    // The LLM key is write-only. The generic GET /settings/{key} would hand the raw row back, so this exact route (matched before the {key}
    // wildcard) answers for spots.llm with the key stripped.
    r->registerRoute("GET", "/api/v1/settings/spots.llm", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["key"] = "spots.llm"; const QString raw = m_store->setting("spots.llm");
        if (raw.isEmpty()) { o["value"] = raw; return o; }
        QJsonObject v = m_store->settingJson("spots.llm"); v.remove("apiKey"); const QJsonObject k = AiModels::keyStatus(m_store.get()); v["hasKey"] = k.value("hasKey"); v["keySource"] = k.value("keySource");
        o["value"] = QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact)); return o;
    });

    // --- jobs -----------------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/spots/jobs", [this](const QJsonObject&, const Q&) { QJsonObject o; o["jobs"] = m_spots->jobs(); o["markers"] = m_spots->markerStates(); return o; });
    r->registerRoute("POST", "/api/v1/spots/jobs", [this](const QJsonObject& body, const Q&) { if (body.value("recipe").toString().isEmpty()) return err(400, "recipe required"); return m_spots->saveJob(body); });
    r->registerRoute("PUT", "/api/v1/spots/jobs/{id}", [this](const QJsonObject& body, const Q& q) { return m_spots->saveJob(body, q["id"]); });
    r->registerRoute("DELETE", "/api/v1/spots/jobs/{id}", [this](const QJsonObject&, const Q& q) { if (!m_spots->deleteJob(q["id"])) return err(404, "no such job"); QJsonObject o; o["deleted"] = q["id"]; return o; });
    r->registerAsync("POST", "/api/v1/spots/jobs/{id}/run", [this, replyWith](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        if (body.value("wait").toBool(true)) { const QJsonObject o = m_spots->runJob(q["id"], replyWith(reply)); if (o.contains("error")) reply(o.value("status").toInt(400), o); }
        else { const QJsonObject o = m_spots->runJob(q["id"], SpotGenerator::Done()); reply(o.contains("error") ? o.value("status").toInt(400) : 202, o); }
    });
    r->registerRoute("POST", "/api/v1/spots/jobs/{id}/placements", [this](const QJsonObject&, const Q& q) { return m_spots->applyPlacements(q["id"]); });

    // --- one-off generation + script preview ------------------------------------------------------------
    r->registerAsync("POST", "/api/v1/spots/generate", [this, replyWith](const QJsonObject& body, const Q&, RestApiServer::Reply reply) {
        if (body.value("wait").toBool(true)) { const QJsonObject o = m_spots->generate(body, replyWith(reply)); if (o.contains("error")) reply(o.value("status").toInt(400), o); }
        else { const QJsonObject o = m_spots->generate(body, SpotGenerator::Done()); reply(o.contains("error") ? o.value("status").toInt(400) : 202, o); }
    });
    // A preview queues behind the render in progress (minutes) and then behind slow feeds / the LLM, and a client with a transfer timeout (the operator
    // app aborts after 8 s of silence) cannot hold the request open: {wait: false} answers 202 {previewId} at once and GET preview-script/{id} polls.
    auto previews = std::make_shared<PreviewBook>();
    r->registerAsync("POST", "/api/v1/spots/preview-script", [this, previews](const QJsonObject& body, const Q&, RestApiServer::Reply reply) {
        if (body.value("wait").toBool(true)) { m_spots->previewScript(body, [reply](const QJsonObject& o) { reply(o.contains("error") ? o.value("status").toInt(500) : 200, o); }); return; }
        const QString pid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        QJsonObject pending; pending["pending"] = true; previews->put(pid, pending);
        m_spots->previewScript(body, [previews, pid](const QJsonObject& o) { QJsonObject res = o; res.remove("status"); res["pending"] = false; previews->put(pid, res); });
        QJsonObject a; a["queued"] = true; a["previewId"] = pid; reply(202, a);
    });
    r->registerRoute("GET", "/api/v1/spots/preview-script/{id}", [this, previews](const QJsonObject&, const Q& q) { if (!previews->byId.contains(q["id"])) return err(404, "no such preview"); return previews->byId.value(q["id"]); });
    r->registerRoute("DELETE", "/api/v1/spots/generation/{id}", [this](const QJsonObject&, const Q& q) { if (!m_spots->cancel(q["id"])) return err(404, "no such generation"); QJsonObject o; o["cancelled"] = q["id"]; return o; });

    // --- status / markers / spot metadata ---------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/spots/status", [this](const QJsonObject&, const Q&) { return m_spots->status(); });
    r->registerRoute("GET", "/api/v1/spots/markers", [this](const QJsonObject&, const Q&) { QJsonObject o; o["markers"] = m_spots->markers(); o["states"] = m_spots->markerStates(); return o; });
    r->registerRoute("POST", "/api/v1/spots/markers/enforce", [this](const QJsonObject&, const Q&) { return m_spots->enforceMarkersNow(); });
    r->registerRoute("GET", "/api/v1/spots/spot/{mediaId}", [this](const QJsonObject&, const Q& q) { return m_spots->spotInfo(q["mediaId"]); });
    r->registerRoute("POST", "/api/v1/spots/sweep", [this](const QJsonObject&, const Q&) { QJsonObject o; o["changed"] = m_spots->expireSweep(); return o; });

    // --- news scatter -----------------------------------------------------------------------------------
    r->registerAsync("POST", "/api/v1/spots/scatter/generate", [this, replyWith](const QJsonObject&, const Q&, RestApiServer::Reply reply) {
        const QJsonObject o = m_spots->scatterGenerate(replyWith(reply)); if (o.contains("error")) reply(o.value("status").toInt(400), o);
    });
    r->registerRoute("POST", "/api/v1/spots/scatter/apply", [this](const QJsonObject& body, const Q&) { return m_spots->scatterApply(body.value("channel").toString(), body.value("perHour").toInt(0)); });
    r->registerRoute("GET", "/api/v1/spots/scatter", [this](const QJsonObject&, const Q&) { return m_spots->scatterStatus(); });
}
