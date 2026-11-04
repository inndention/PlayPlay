// SupervisorCore — overlay routes: /api/v1/overlays/* (docs/overlays.md). The OverlayManager owns the state; this file only
// constructs it, wires engine events in and manager events out, and maps REST onto its methods.
#include "supervisorcore.h"
#include "channelmanager.h"
#include "restapiserver.h"
#include "overlaymanager.h"
#include "pillarpanels.h"
#include "cgdesigner.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QPointer>
#include <QProcessEnvironment>
#include <QDebug>
#include <QUuid>
#include <QDateTime>

using namespace sched;
using Q = QHash<QString, QString>;

// Pillar panels (docs/pillar-panels.md): a child of the SupervisorCore, so the core header stays as it is.
static QPointer<PillarPanels> g_pillar;
// CG Designer (docs/cg-designer.md): AI generator + WYSIWYG editor store; its designs join cgManifest() via CgDesigner::mergeInto.
static QPointer<CgDesigner> g_designer;

void SupervisorCore::registerOverlayRoutes() {
    RestApiServer* r = m_rest.get();
    m_overlays = std::make_unique<OverlayManager>(m_store.get(), m_exec.get(), this);

    OverlayManager::Hooks h;
    h.sendCg = [this](const QString& ch, const QJsonObject& cmd) { return m_channels && m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, ch, cmd)); };
    h.engineConnected = [this](const QString& ch) { return m_channels && m_channels->isChannelConnected(ch); };
    h.manifest = [this]() { return cgManifest(); };
    h.channelIds = [this]() { QJsonArray a; for (const ChannelProfile& p : m_store->channels()) a.append(p.id); return a; };
    h.webRoot = m_opt.webRoot; h.restPort = m_opt.restPort;
    g_pillar = new PillarPanels(m_store.get(), m_exec.get(), this);
    h.pillarFor = [](const QString& ch, const QJsonObject& item) { return g_pillar ? g_pillar->specFor(ch, item) : QJsonObject(); };
    m_overlays->setHooks(h);
    connect(g_pillar.data(), &PillarPanels::changed, this, [this](const QString& ch) {
        if (!m_overlays) return;
        if (!ch.isEmpty()) { m_overlays->refreshPillar(ch); return; }
        for (const ChannelProfile& p : m_store->channels()) m_overlays->refreshPillar(p.id);
    });
    g_pillar->start();
    g_designer = new CgDesigner(m_store.get(), m_opt.webRoot, [this]() { return cgManifest(); }, this);
    g_designer->start();
    g_designer->registerRoutes(r);

    // Engines spawned from now on get PLAYPLAY_BROWSER etc. (settings cg.browserPath / cg.htmlNthFrame / cg.htmlFirstFrameMs).
    auto applyEnv = [this]() {
        QProcessEnvironment env; const QHash<QString, QString> e = m_overlays->engineEnvironment();
        for (auto it = e.begin(); it != e.end(); ++it) env.insert(it.key(), it.value());
        if (m_channels) m_channels->setEngineExtraEnv(env);
    };
    applyEnv();
    connect(m_overlays.get(), &OverlayManager::engineEnvChanged, this, applyEnv);

    connect(m_overlays.get(), &OverlayManager::layersChanged, this, [this](const QString& ch, const QJsonObject& p) { broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, p, "overlays"); });
    connect(m_overlays.get(), &OverlayManager::configChanged, this, [this](const QJsonObject& p) { broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, p.value("channelId").toString(), p, "overlayConfig"); });
    connect(m_overlays.get(), &OverlayManager::alert, this, [this](const QString& ch, const QString& level, const QString& msg) { pushAlert(ch, level, msg); });
    connect(m_overlays.get(), &OverlayManager::cgHtmlState, this, [this](const QString& ch, const QJsonObject& st) {
        if (st.value("cleared").toBool()) m_cgHtml.remove(ch); else m_cgHtml[ch] = st;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, st, "cgHtml");
    });
    // Engine lifecycle and as-run events (START = item clock, CG_SHOW = layer attached).
    connect(m_channels.get(), &ChannelManager::engineConnected, this, [this](const QString& id) { if (m_overlays) m_overlays->onEngineConnected(id); });
    connect(m_channels.get(), &ChannelManager::engineDisconnected, this, [this](const QString& id) { if (m_overlays) m_overlays->onEngineDisconnected(id); });
    connect(m_exec.get(), &ScheduleExecutor::asRunStored, this, [this](const AsRunRow& row) {
        if (m_overlays && (row.eventType == "START" || row.eventType == "CG_SHOW")) m_overlays->onAsRun(row.channelId, row.eventType, row.runlistItemId, row.assetId, row.actualStartMs, row.extra);
    });
    m_overlays->start();

    // --- renderer (exact paths first: they win over the {ch} wildcards) --------------------------------
    r->registerRoute("GET", "/api/v1/overlays/renderer", [this](const QJsonObject&, const Q&) { return m_overlays->renderer(); });
    r->registerRoute("PUT", "/api/v1/overlays/renderer", [this](const QJsonObject& body, const Q&) { return m_overlays->setRenderer(body); });
    r->registerAsync("POST", "/api/v1/overlays/renderer/test", [this](const QJsonObject& body, const Q&, RestApiServer::Reply reply) { m_overlays->testRenderer(body, reply); });

    r->registerRoute("GET", "/api/v1/overlays", [this](const QJsonObject&, const Q&) { return m_overlays->overview(); });
    r->registerRoute("GET", "/api/v1/overlays/presets", [this](const QJsonObject&, const Q&) { return m_overlays->presets(); });
    r->registerRoute("PUT", "/api/v1/overlays/presets", [this](const QJsonObject& body, const Q&) { return m_overlays->setPresets(body); });
    r->registerRoute("GET", "/api/v1/overlays/rules", [this](const QJsonObject&, const Q&) { return m_overlays->rules(); });
    r->registerRoute("PUT", "/api/v1/overlays/rules", [this](const QJsonObject& body, const Q&) { return m_overlays->setRules(body); });
    r->registerRoute("GET", "/api/v1/overlays/rules/test", [this](const QJsonObject&, const Q& q) { return m_overlays->testRules(q.value("channel"), q.value("mediaId"), q); });
    // CG slots CG1, CG2, ... and their switcher-input assignment (docs/overlays.md "CG slots")
    r->registerRoute("GET", "/api/v1/overlays/cgslots", [this](const QJsonObject&, const Q&) { return m_overlays->cgSlots(); });
    r->registerRoute("PUT", "/api/v1/overlays/cgslots", [this](const QJsonObject& body, const Q&) { return m_overlays->setCgSlots(body); });
    r->registerRoute("GET", "/api/v1/shows", [this](const QJsonObject&, const Q&) { return m_store->settingJson("ui.showTemplates"); });
    r->registerRoute("POST", "/api/v1/shows", [this](const QJsonObject& body, const Q&) {
        const QString name = body.value("name").toString().trimmed(); if (name.isEmpty() || name.size() > 100) return err(422, "Show name must be 1–100 characters");
        auto all = m_store->settingJson("ui.showTemplates"); auto list = all.value("items").toArray(); QString id = body.value("id").toString(); int index = -1;
        if (!id.isEmpty()) { for (int i=0; i<list.size(); ++i) if (list[i].toObject().value("id").toString() == id) index = i; if (index < 0) return err(404, "Show template not found"); }
        else { if (list.size() >= 100) return err(422, "At most 100 show templates"); id = QUuid::createUuid().toString(QUuid::Id128); }
        QJsonObject layout = index >= 0 ? list[index].toObject().value("layout").toObject() : QJsonObject();
        const auto suppliedLayout = body.value("layout").toObject();
        for (auto it = suppliedLayout.begin(); it != suppliedLayout.end(); ++it) layout[it.key()] = it.value();
        QJsonObject show{{"id",id}, {"name",name}, {"inputMap",m_store->settingJson("ui.inputMap")}, {"ptzMap",m_store->settingJson("ui.ptzMap")}, {"cgSlots",m_overlays->cgSlots().value("slots").toArray()}, {"cgPresets",m_overlays->presets().value("presets").toArray()}, {"cgRules",m_overlays->rules().value("rules").toArray()}, {"layout",layout}, {"updatedAtMs",double(QDateTime::currentMSecsSinceEpoch())}};
        if (index < 0) list.append(show); else list[index] = show; all["items"] = list;
        if (!m_store->setSettingJson("ui.showTemplates", all)) return err(500, "Could not save show template"); return show;
    });
    r->registerRoute("POST", "/api/v1/shows/{id}/apply", [this](const QJsonObject&, const Q& q) {
        auto all = m_store->settingJson("ui.showTemplates"); QJsonObject show;
        for (const auto& v : all.value("items").toArray()) if (v.toObject().value("id").toString() == q["id"]) { show = v.toObject(); break; }
        if (show.isEmpty()) return err(404, "Show template not found");
        const auto map = show.value("inputMap").toObject(), ptz = show.value("ptzMap").toObject();
        if (map.value("slots").toArray().size() > 8 || ptz.value("slots").toArray().size() > 8 || !show.value("cgSlots").isArray()) return err(422, "Invalid show template");
        if ((show.contains("cgPresets") && !show.value("cgPresets").isArray()) || (show.contains("cgRules") && !show.value("cgRules").isArray())) return err(422, "Invalid show graphics configuration");
        const auto oldMap = m_store->settingJson("ui.inputMap"), oldPtz = m_store->settingJson("ui.ptzMap"); const auto oldCg = m_overlays->cgSlots().value("slots").toArray();
        const auto oldPresets = m_overlays->presets().value("presets").toArray(), oldRules = m_overlays->rules().value("rules").toArray();
        const auto saved = m_overlays->setCgSlots({{"slots",show.value("cgSlots")}}); if (saved.contains("error")) return saved;
        const bool presetsOk = !show.contains("cgPresets") || !m_overlays->setPresets({{"presets",show.value("cgPresets")}}).contains("error");
        const bool rulesOk = presetsOk && (!show.contains("cgRules") || !m_overlays->setRules({{"rules",show.value("cgRules")}}).contains("error"));
        if (!rulesOk || !m_store->setSettingJson("ui.inputMap", map) || !m_store->setSettingJson("ui.ptzMap", ptz)) {
            m_store->setSettingJson("ui.inputMap", oldMap); m_store->setSettingJson("ui.ptzMap", oldPtz); m_overlays->setCgSlots({{"slots",oldCg}}); m_overlays->setPresets({{"presets",oldPresets}}); m_overlays->setRules({{"rules",oldRules}}); return err(500, "Could not apply show template; prior configuration was restored");
        }
        all["active"] = show.value("id"); m_store->setSettingJson("ui.showTemplates", all);
        QJsonObject event{{"id",show.value("id")}, {"name",show.value("name")}, {"layout",show.value("layout")}};
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), event, "showTemplate"); return event;
    });
    r->registerRoute("DELETE", "/api/v1/shows/{id}", [this](const QJsonObject&, const Q& q) {
        auto all = m_store->settingJson("ui.showTemplates"); QJsonArray kept; bool found = false;
        for (const auto& v : all.value("items").toArray()) { if (v.toObject().value("id").toString() == q["id"]) found = true; else kept.append(v); }
        if (!found) return err(404, "Show template not found"); all["items"] = kept; if (all.value("active").toString() == q["id"]) all.remove("active");
        if (!m_store->setSettingJson("ui.showTemplates", all)) return err(500, "Could not delete show template"); return QJsonObject{{"deleted",q["id"]}};
    });
    r->registerRoute("POST", "/api/v1/overlays/cgslots/assign", [this](const QJsonObject& body, const Q&) { return m_overlays->assignCgInput(body); });

    // --- pillar panels (docs/pillar-panels.md) -------------------------------------------------------
    r->registerRoute("GET", "/api/v1/pillar", [](const QJsonObject&, const Q&) { return g_pillar->status(); });
    r->registerRoute("GET", "/api/v1/pillar/config", [](const QJsonObject&, const Q&) { return g_pillar->config(); });
    r->registerRoute("PUT", "/api/v1/pillar/config", [](const QJsonObject& body, const Q&) { return g_pillar->setConfig(body); });
    r->registerRoute("POST", "/api/v1/pillar/refresh", [](const QJsonObject&, const Q&) { return g_pillar->refresh(); });
    r->registerRoute("GET", "/api/v1/pillar/{ch}/data", [this](const QJsonObject&, const Q& q) {
        QJsonObject o; o["channelId"] = q["ch"]; o["data"] = g_pillar->data(q["ch"]); o["item"] = m_overlays->onAirItem(q["ch"]); return o; });
    r->registerRoute("POST", "/api/v1/pillar/{ch}/preview", [](const QJsonObject& body, const Q& q) { return g_pillar->preview(q["ch"], body); });

    // --- per channel -----------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/overlays/{ch}", [this](const QJsonObject&, const Q& q) { return m_overlays->channelState(q["ch"]); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/deploy", [this](const QJsonObject& body, const Q& q) { return m_overlays->deploy(q["ch"], body); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/clear", [this](const QJsonObject& body, const Q& q) { return m_overlays->clear(q["ch"], body); });
    r->registerRoute("GET", "/api/v1/overlays/{ch}/defaults", [this](const QJsonObject&, const Q& q) { return m_overlays->defaults(q["ch"]); });
    r->registerRoute("PUT", "/api/v1/overlays/{ch}/defaults", [this](const QJsonObject& body, const Q& q) { return m_overlays->setDefaults(q["ch"], body); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/defaults/apply", [this](const QJsonObject&, const Q& q) { return m_overlays->applyDefaults(q["ch"]); });
    r->registerRoute("GET", "/api/v1/overlays/{ch}/cues", [this](const QJsonObject&, const Q& q) { return m_overlays->cues(q["ch"]); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/presets/{id}/fire", [this](const QJsonObject& body, const Q& q) { return m_overlays->firePreset(q["ch"], q["id"], body); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/presets/{id}/hide", [this](const QJsonObject&, const Q& q) { return m_overlays->hidePreset(q["ch"], q["id"]); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/cgslots/{id}/fire", [this](const QJsonObject& body, const Q& q) { return m_overlays->fireCgSlot(q["ch"], q["id"], body); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/cgslots/{id}/hide", [this](const QJsonObject&, const Q& q) { return m_overlays->hideCgSlot(q["ch"], q["id"]); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/{layer}/update", [this](const QJsonObject& body, const Q& q) { return m_overlays->update(q["ch"], q["layer"].toInt(), body); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/{layer}/retake", [this](const QJsonObject&, const Q& q) { return m_overlays->retake(q["ch"], q["layer"].toInt()); });
    r->registerRoute("POST", "/api/v1/overlays/{ch}/{layer}/hide", [this](const QJsonObject& body, const Q& q) { return m_overlays->hide(q["ch"], q["layer"].toInt(), body); });
    r->registerRoute("DELETE", "/api/v1/overlays/{ch}/{layer}", [this](const QJsonObject&, const Q& q) { return m_overlays->remove(q["ch"], q["layer"].toInt()); });
}
