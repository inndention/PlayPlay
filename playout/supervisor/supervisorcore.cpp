#include "supervisorcore.h"
#include "livestream.h"
#include "laneext.h"
#ifdef PLAYPLAY_HAVE_REPORTERFEED
#include "reporterfeed.h"
#endif
#include "channelmanager.h"
#include "restapiserver.h"
#include "websocketbroker.h"
#include "failovercontroller.h"
#include "onvifmanager.h"
#include "meshclient.h"
#include "ingestmanager.h"
#include "starclient.h"
#include "muxmanager.h"
#include "scte35.h"
#include "transcriptionmanager.h"
#include "spotgenerator.h"
#include "reports.h"
#include "sessionsmanager.h"
#include "failoverpair.h"
#include "overlaymanager.h"
#include "renderfarm.h"
#include "starnodes.h"
#include "publisher.h"
#include "spotstudio.h"
#include "agentsuite.h"
#include "aimodels.h"
#include "systemstats.h"
#include "statepublisher.h"
#include "../timeauthority.h"
#include "../core/ntpclient.h"
#include "../core/framerate.h"
#include "../core/loopwatchdog.h"
#include "../schedule/runlistbuilder.h"
#include "../schedule/schedulevalidator.h"
#include "../schedule/storereader.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <algorithm>
#include <QJsonDocument>
#include <QDateTime>
#include <QTcpSocket>
#include <QStandardPaths>
#include <QStorageInfo>
#include <memory>
#include <QDebug>
#include <QLoggingCategory>
#include <QUuid>
#include <QSet>

using namespace sched;

SupervisorCore::SupervisorCore(QObject* parent) : QObject(parent) {}

SupervisorCore::~SupervisorCore() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool SupervisorCore::start(const Options& opt) {
    m_opt = opt;
    m_uptime.start();
    QDir().mkpath(m_opt.dataDir);

    // 1. Time
    m_time = getTimeAuthority();
    m_time->setMode(TimeAuthority::Master);
    m_time->setFrameRate(30);
    m_ntp = std::make_unique<NtpClient>();
    m_ntp->setServers(QStringList() << m_opt.ntpServer);
    connect(m_ntp.get(), &NtpClient::syncResult, this, [this](qint64 offsetMs, qint64 rtt, bool ok) {
        if (ok) { m_time->setNtpOffset(offsetMs); qInfo() << "[Supervisor] NTP offset" << offsetMs << "ms rtt" << rtt << "ms"; }
    });
    connect(m_ntp.get(), &NtpClient::syncError, this, [](const QString& r) { qWarning() << "[Supervisor] NTP:" << r; });
    m_ntp->query();
    connect(&m_ntpTimer, &QTimer::timeout, m_ntp.get(), &NtpClient::query);
    m_ntpTimer.start(m_opt.ntpIntervalSec * 1000);

    // 2. Store
    m_store = std::make_unique<ScheduleStore>();
    if (!m_store->open(QDir(m_opt.dataDir).filePath("playplay.db"))) return false;
    if (m_store->channels().isEmpty()) {
        ChannelProfile p; p.id = "main"; p.name = "Main"; p.callLetters = "PLAY PLAY";
        QJsonObject ndi; ndi["type"] = "ndi"; ndi["name"] = "PLAY PLAY Main";
        p.outputs.append(ndi);
        m_store->saveChannel(p);
        qInfo() << "[Supervisor] created default channel 'main' (NDI output)";
    }

    // 3. IPC hub
    m_ipc = std::make_unique<IpcTransport>(IpcTransport::ServerRole);
    if (!m_ipc->startServer(m_opt.ipcName)) { qCritical() << "[Supervisor] IPC server failed"; return false; }
    connect(m_ipc.get(), &IpcTransport::messageReceived, this, &SupervisorCore::onIpcMessage);
    connect(m_ipc.get(), &IpcTransport::clientDisconnected, this, &SupervisorCore::onIpcClientDisconnected);

    // 4. Channels
    m_channels = std::make_unique<ChannelManager>();
    m_channels->setIpc(m_ipc.get());
    m_channels->setSupervisorIpcName(m_opt.ipcName);
    m_channels->setOutputsResolver([this](const QJsonArray& outs) { return livestream::hydrate(outs, [this](const QString& ref) { return m_store->settingJson(ref); }); });
    if (!m_opt.enginePath.isEmpty()) m_channels->setEngineExecutable(m_opt.enginePath);

    // 5. Executor
    m_exec = std::make_unique<ScheduleExecutor>(m_store.get(), this);
    connect(m_channels.get(), &ChannelManager::engineConnected, this, [this](const QString& id) {
        m_exec->onEngineConnected(id);
        pushAlert(id, "info", "engine connected");
        broadcastUi(IpcProtocol::MSG_UI_STATUS, id, channelSnapshot(id), "channel");
    });
    connect(m_channels.get(), &ChannelManager::engineDisconnected, this, [this](const QString& id) {
        m_webMeters.remove(id);
        m_exec->onEngineDisconnected(id);
        m_engineState[id] = "OFFLINE";
        pushAlert(id, "warning", "engine disconnected");
        broadcastUi(IpcProtocol::MSG_UI_STATUS, id, channelSnapshot(id), "channel");
    });
    connect(m_channels.get(), &ChannelManager::channelDied, this, [this](const QString& id, const QString& reason) {
        pushAlert(id, "critical", "engine died: " + reason + " (respawning)");
    });
    connect(m_channels.get(), &ChannelManager::channelDied, this, [this](const QString& id, const QString&) {
        QTimer::singleShot(1500, this, [this, id]() { if (m_channels) m_channels->restartChannel(id); });
    });
    connect(m_exec.get(), &ScheduleExecutor::alert, this, &SupervisorCore::pushAlert);
    connect(m_exec.get(), &ScheduleExecutor::runlistChanged, this, [this](const QString& id, const QJsonObject& rl) {
        broadcastUi(IpcProtocol::MSG_UI_RUNLIST, id, rl, "runlist");
    });
    connect(m_exec.get(), &ScheduleExecutor::pushed, this, [this](const QString& id, int n, bool replace, const QString& why) {
        QJsonObject o; o["items"] = n; o["replaceCurrent"] = replace; o["reason"] = why;
        broadcastUi(IpcProtocol::MSG_UI_RUNLIST, id, m_exec->runlistJson(id), "runlist");
        Q_UNUSED(o);
    });
    connect(m_exec.get(), &ScheduleExecutor::asRunStored, this, [this](const AsRunRow& r) {
#ifdef PLAYPLAY_HAVE_REPORTERFEED
        if (r.eventType == "FAILED") reporterfeed::noteAsRunFailure(r.channelId, r.assetId, r.reason);
#endif
        broadcastUi(IpcProtocol::MSG_UI_ASRUN, r.channelId, r.toJson(), "asrun");
    });
    connect(m_store.get(), &ScheduleStore::slotChanged, this, [this](const QString& ch, const QString& slot) {
        QJsonObject o; o["channelId"] = ch; o["slotId"] = slot;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, o, "schedule");
    });

    // 6. Failover scoring
    m_failover = std::make_unique<FailoverController>(m_channels.get());
    m_failover->start();
    connect(m_failover.get(), &FailoverController::alert, this, [this](const QString& level, const QString& msg) { pushAlert("", level, msg); });

    // 7. Time sync broadcast 1 Hz (was 50 Hz to every client, PS-1): engines only take the NTP offset from it, which moves
    //    at NTP polls; UI clients ignore it and keep the 1 Hz copy until perf-app PA-1 uses MSG_SUP_ALIVE. Droppable: a
    //    client that is behind skips it.
    connect(&m_timeSyncTimer, &QTimer::timeout, this, [this]() {
        const qint64 nowNs = QDateTime::currentMSecsSinceEpoch() * 1000000LL;
        m_ipc->broadcast(IpcProtocol::makeTimeSync(m_time->currentFrame(), nowNs, m_time->ntpOffset() * 1000000LL), IpcTransport::Droppable);
    });
    m_timeSyncTimer.start(1000);

    // 8. REST + WS
    m_rest = std::make_unique<RestApiServer>(nullptr);
    registerRoutes();
    if (!m_opt.webRoot.isEmpty()) m_rest->setStaticRoot(m_opt.webRoot);
    if (!m_opt.logDir.isEmpty()) m_channels->setEngineExtraArgs({"--log-dir", m_opt.logDir});
    if (!m_rest->start(quint16(m_opt.restPort))) qWarning() << "[Supervisor] REST failed on port" << m_opt.restPort;
    m_ws = std::make_unique<WebSocketBroker>();
    if (!m_ws->start(quint16(m_opt.wsPort))) qWarning() << "[Supervisor] WebSocket failed on port" << m_opt.wsPort;
    const QString token = m_store->setting("rest.token");
    if (!token.isEmpty()) { m_rest->setAuthToken(token); m_ws->setAuthToken(token); qInfo() << "[Supervisor] REST/WS protected by bearer token (settings rest.token)"; }
    connect(m_ws.get(), &WebSocketBroker::messageReceived, this, [this](const QString& cid, const QJsonObject& o) {
        if (o.value("type").toString() == "command") onUiCommand(o.value("channelId").toString(), o.value("action").toString(), o.value("params").toObject(), "ws:" + cid);
    });

    // 9. Integrations: ONVIF cameras, AN-FS mesh, scheduled device commands
    m_onvif = std::make_unique<OnvifManager>(m_store.get());
    connect(m_onvif.get(), &OnvifManager::devicesChanged, this, [this]() { QJsonObject o; o["cameras"] = m_onvif->devicesJson(); broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "cameras"); });
    m_mesh = std::make_unique<MeshClient>(m_store.get(), QDir(uploadDir()).filePath("mesh"));
    connect(m_mesh.get(), &MeshClient::jobsChanged, this, [this]() { QJsonObject o; o["jobs"] = m_mesh->jobs(); broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "meshJobs"); });
    connect(m_mesh.get(), &MeshClient::fetched, this, [this](const QString& local, const QString& type, const QString& node, const QString& remote) {
        const QString native = QDir::toNativeSeparators(local);
        bool found = false; MediaRecord m = m_store->mediaByPath(native, &found);
        QJsonObject meta; meta["meshNode"] = node; meta["meshPath"] = remote;
        if (found) { m.extra["meshNode"] = node; m.extra["meshPath"] = remote; m_store->saveMedia(m); }
        else m_ingest->enqueue(native, type, "mesh", meta);
        pushAlert("", "info", QString("mesh: fetched %1 from %2").arg(QFileInfo(local).fileName(), node.isEmpty() ? "server" : node));
    });
    m_ingest = std::make_unique<IngestManager>(m_store.get(), m_opt.dataDir, uploadDir());
    connect(m_ingest.get(), &IngestManager::jobsChanged, this, [this]() { QJsonObject o; o["jobs"] = m_ingest->jobsJson(60); o["pending"] = m_ingest->pending(); broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "ingest"); });
    connect(m_ingest.get(), &IngestManager::scanDone, this, [this]() { QJsonObject o; o["done"] = true; broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "mediaScanDone"); });
    connect(m_ingest.get(), &IngestManager::mediaIngested, this, [this](const QString& id, const QString& origin) { QJsonObject o; o["mediaId"] = id; o["origin"] = origin; broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "mediaIngested"); });
    m_star = std::make_unique<StarClient>(m_store.get(), QDir(uploadDir()).filePath("star"));
    connect(m_star.get(), &StarClient::jobsChanged, this, [this]() { QJsonObject o; QJsonArray all = m_mesh->jobs(); for (const QJsonValue& v : m_star->jobs()) all.append(v); o["jobs"] = all; broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "meshJobs"); });
    connect(m_star.get(), &StarClient::fetched, this, [this](const QString& local, const QString& type, const QString& node, const QString& remote) {
        QJsonObject meta; meta["starNode"] = node; meta["starPath"] = remote;
        m_ingest->enqueue(QDir::toNativeSeparators(local), type, "star", meta);
        pushAlert("", "info", QString("star: fetched %1 from %2").arg(QFileInfo(local).fileName(), node));
    });
    {
        QString scripts = QDir(QCoreApplication::applicationDirPath()).filePath("scripts");
        if (!QFileInfo(QDir(scripts).filePath("transcribe.py")).exists()) { const QString src = QDir(QCoreApplication::applicationDirPath()).filePath("../scripts"); if (QFileInfo(QDir(src).filePath("transcribe.py")).exists()) scripts = QDir(src).absolutePath(); }
        m_transcribe = std::make_unique<TranscriptionManager>(m_store.get(), m_opt.dataDir, scripts);
        connect(m_transcribe.get(), &TranscriptionManager::jobsChanged, this, [this]() { QJsonObject o; o["jobs"] = m_transcribe->jobsJson(); broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "transcribe"); });
        connect(m_transcribe.get(), &TranscriptionManager::transcriptReady, this, [this](const QString& id) { QJsonObject o; o["mediaId"] = id; broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "transcriptReady"); });
        connect(m_ingest.get(), &IngestManager::mediaIngested, this, [this](const QString& id, const QString& origin) {
            if (origin == "rethumb" || origin == "reprobe") return;
            const QJsonObject tc = m_transcribe->config();
            if (tc.value("autoTranscribe").toBool(false)) { bool f = false; const MediaRecord m = m_store->media(id, &f); if (f && m.type != "image" && m.type != "live") m_transcribe->enqueue(id, tc.value("autoLevel").toString("fast")); }
        });
    }
    m_mux = std::make_unique<MuxManager>(m_store.get(), m_opt.dataDir);
    m_mux->setTitleResolver([this](const QString& channelId, const QDateTime& hourLocal, QString* desc) -> QString {
        const HourSlot slot = m_store->effectiveSlot(channelId, hourLocal.date(), hourLocal.time().hour());
        const QString show = slot.showBlock.value("title").toString();
        QString first; for (const SlotItem& it : slot.items) { if (it.type == "program" || it.type == "live" || it.type == "video") { first = it.title; break; } }
        if (desc) { QStringList parts; for (const SlotItem& it : slot.items) if (it.type == "program" && !it.title.isEmpty()) parts << it.title; *desc = parts.mid(0, 6).join(" / "); }
        return !show.isEmpty() ? show : first;
    });
    connect(m_mux.get(), &MuxManager::alert, this, [this](const QString& level, const QString& msg) { pushAlert("mux", level, msg); });
    connect(m_mux.get(), &MuxManager::servicesChanged, this, &SupervisorCore::applyMuxOutputs);
    m_muxNotify.setSingleShot(true);
    connect(&m_muxNotify, &QTimer::timeout, this, [this]() { QJsonObject o = m_mux->status(); broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "mux"); });
    connect(m_mux.get(), &MuxManager::statusChanged, this, [this]() { if (!m_muxNotify.isActive()) m_muxNotify.start(800); });
    // 10. Programming (query blocks / recipes / weighted fill), generated spots, reports, redundancy.
    //     Registered after every manager exists and before the station listener copies the route table.
    registerProgrammingRoutes();
    registerSpotRoutes();
    registerReportRoutes();
    registerRedundancyRoutes();
    registerChannelAutomationRoutes();
    registerOverlayRoutes();
    registerFlightPlanRoutes();
    registerSuiteRoutes();
    SystemStats::install(m_rest.get(), this, [this]() {       // GET /api/v1/system/stats: CPU / memory / network / per-channel fps + bitrate
        QList<SystemStats::ProcRef> l; for (const auto& r : m_channels->listChannels()) l.append({r.channelId, r.pid}); return l;
    });
    {   // lane extensions (playout/supervisor/laneext.h): self-contained modules add their routes here
        laneext::Context lx; lx.parent = this; lx.rest = m_rest.get(); lx.store = m_store.get(); lx.ingest = m_ingest.get(); lx.dataDir = m_opt.dataDir; lx.restPort = m_opt.restPort;
        lx.restToken = [this]() { return m_store->setting("rest.token"); };
        lx.setLanPassword = [this](const QString& password) { m_rest->setLanPassword(password); if (m_stationRest) m_stationRest->setLanPassword(password); if (m_ws) m_ws->setLanPassword(password); };
        lx.alert = [this](const QString& ch, const QString& level, const QString& msg) { pushAlert(ch, level, msg); };
        lx.broadcast = [this](const QString& type, const QJsonObject& payload) { QJsonObject o = payload; o["type"] = type; m_ws->broadcast(o); };
        lx.reloadChannel = [this](const QString& ch) { m_exec->reloadChannel(ch); };
        lx.runlist = [this](const QString& ch) { return m_exec->runlistJson(ch); };
        lx.engineState = [this](const QString& ch) { return m_telemetry.value(ch); };
        laneext::installAll(lx);
    }
    {   // station API listener (PlayPlay OBS compatibility, port 7860 by default when enabled)
        const int sp = m_store->setting("station.port", "0").toInt();
        m_rest->setStationPassword(m_store->setting("station.password"));
        if (sp > 0) { m_stationRest = std::make_unique<RestApiServer>(nullptr); m_stationRest->copyRoutes(*m_rest); if (!m_stationRest->start(quint16(sp))) qWarning() << "[Supervisor] station API failed on port" << sp; else qInfo() << "[Supervisor] station API (PlayPlay OBS compatible) on port" << sp; }
    }
    connect(&m_deviceTimer, &QTimer::timeout, this, &SupervisorCore::deviceCommandTick);
    m_deviceTimer.start(1000);
    QTimer::singleShot(2500, this, [this]() { if (m_onvif) m_onvif->discover(3000); });
    // UI state publisher (PS-6 / W12): the periodic snapshot is built once per tick and sent only when it changed (its fast
    // gauges aside), plus a keepalive: settings ui.snapshotKeepaliveMs, default 10 s; 0 = every tick, as before.
    m_state = std::make_unique<StatePublisher>([this]() { return fullSnapshot(); },
        [this](const QJsonObject& snap) { broadcastUi(IpcProtocol::MSG_UI_STATUS, QString(), snap, "snapshot"); });
    m_state->setListenerCheck([this]() { return !m_uiClients.isEmpty() || (m_ws && m_ws->clientCount() > 0); });
    m_state->setKeepaliveMs(m_store->setting("ui.snapshotKeepaliveMs", QString::number(StatePublisher::kDefaultKeepaliveMs)).toInt());
    // A dashboard that connects gets the current snapshot itself (every connect used to broadcast one to every client).
    connect(m_ws.get(), &WebSocketBroker::clientConnected, this, [this](const QString& cid) {
        QJsonObject snap = m_state->replySnapshot(true); snap["type"] = "snapshot"; m_ws->sendTo(cid, snap);
    });
    m_rest->registerRoute("GET", "/api/v1/system/publisher", [this](const QJsonObject&, const QHash<QString, QString>&) {
        QJsonObject o = m_state->stats(); o["ipc"] = m_ipc->stats(); o["ws"] = m_ws->stats(); return o;
    });

    // 9. Engines. PS-4b: engines a previous Supervisor left running in this scope (IPC name + data dir) are adopted (same
    //    build, known enabled channel: air continues) or reaped before the first spawn; the scan runs on a worker and
    //    again every 60 s. Settings: engines.orphanScan (false = no scan, as before), engines.adoptOrphans (false = reap
    //    instead), engines.orphanGraceSec (start-config default for spawned engines; -1 = an orphan plays on, as before).
    {
        auto on = [this](const char* key) { const QString v = m_store->setting(key, "true").trimmed().toLower(); return v != "false" && v != "0" && v != "off"; };
        ChannelManager::ReaperOptions ro;
        ro.enabled = on("engines.orphanScan");
        ro.dataDir = m_opt.dataDir;
        ro.adoptEnabled = [on]() { return on("engines.adoptOrphans"); };
        ro.orphanGraceSec = qBound(-1, m_store->setting("engines.orphanGraceSec", "-1").toInt(), 7 * 24 * 3600);
        ro.profileOf = [this](const QString& id, ChannelProfile* out) { bool f = false; *out = m_store->channel(id, &f); return f; };
        ro.engineConfig = [this](const ChannelProfile& p) { return engineConfigFor(p); };
        ro.startupDone = [this]() { if (m_opt.autoStartChannels) ensureChannelsFromStore(); };
        connect(m_channels.get(), &ChannelManager::reaperAlert, this, &SupervisorCore::pushAlert);
        m_channels->startReaper(ro);
    }
    m_exec->start(1000);

    // 10. Periodic UI snapshot: built once every 2 s, sent on change or keepalive (StatePublisher)
    connect(&m_uiSnapshotTimer, &QTimer::timeout, this, [this]() { m_state->tick(); });
    m_uiSnapshotTimer.start(m_state->tickMs());

    qInfo().noquote() << QString("[Supervisor] ready: IPC '%1', REST :%2, WS :%3, data %4, channels %5")
        .arg(m_opt.ipcName).arg(m_opt.restPort).arg(m_opt.wsPort).arg(QDir(m_opt.dataDir).absolutePath()).arg(m_store->channels().size());
    return true;
}

void SupervisorCore::stop() {
    m_uiSnapshotTimer.stop(); m_timeSyncTimer.stop(); m_ntpTimer.stop(); m_deviceTimer.stop();
    if (m_mux) m_mux->stop();
    if (m_exec) m_exec->stop();
    if (m_probe) { m_probe->kill(); m_probe->deleteLater(); m_probe = nullptr; }
    if (m_channels) m_channels->killAll();
    if (m_ws) m_ws->stop();
    if (m_rest) m_rest->stop();
    if (m_ipc) m_ipc->disconnect();
    if (m_store) m_store->close();
}

// ---------------------------------------------------------------------------
// Engines
// ---------------------------------------------------------------------------

QJsonObject SupervisorCore::engineConfigFor(const ChannelProfile& p) const {
    QJsonObject c;
    c["channelId"] = p.id; c["channelName"] = p.name; c["frameRate"] = p.frameRate;
    c["width"] = p.width; c["height"] = p.height; c["audioChannels"] = p.audioChannels;
    c["prerollSec"] = p.prerollSec; c["outputs"] = p.outputs;
    QJsonArray caps = p.identity.value("slateCaptions").toArray();
    if (caps.isEmpty()) caps << (p.callLetters.isEmpty() ? p.name.toUpper() : p.callLetters) << "STAND BY";
    c["slateCaptions"] = caps;
    c["ndi"] = m_store->settingJson("ndi.discovery");     // {groups, extraIps} for the engine's NDI finder
    // Output mixes [{id, name, strips, master, solo, follow}] and the compositing mode, kept in the profile's extra keys. The GPU
    // flag belongs in the start config too (as ScheduleExecutor::engineConfigFor sends it): enabled later by a config update the
    // engine composites synchronously instead of with its readback pipeline.
    c["gpu"] = p.extra.value("gpu").toBool(false);
    c["hwDecode"] = p.extra.value("hwDecode").toBool(true);
    if (p.extra.value("mixes").isArray()) c["mixes"] = p.extra.value("mixes").toArray();
    if (p.extra.value("gpuMode").isString()) c["gpuMode"] = p.extra.value("gpuMode").toString();
    return c;
}

bool SupervisorCore::spawnChannel(const ChannelProfile& p) {
    m_exec->addChannel(p.id);
    const QString dir = QDir(m_opt.dataDir).filePath("channels/" + p.id);
    return m_channels->spawnChannel(p.id, p.name, dir, engineConfigFor(p));
}

void SupervisorCore::ensureChannelsFromStore() {
    for (const ChannelProfile& p : m_store->channels()) {
        if (!p.enabled) continue;
        if (m_channels->isChannelRunning(p.id)) continue;
        spawnChannel(p);
    }
}

bool SupervisorCore::pushPlaylist(const QString& channelId, const QJsonArray& items, bool replaceCurrent) {
    return m_channels->sendToChannel(channelId, IpcProtocol::makePlaylistPush(channelId, items, replaceCurrent));
}

bool SupervisorCore::transport(const QString& channelId, const QString& action, const QJsonObject& params) {
    return m_channels->sendToChannel(channelId, IpcProtocol::makeTransportCommand(channelId, action, params));
}

bool SupervisorCore::sendConfig(const QString& channelId, const QJsonObject& config) {
    return m_channels->sendToChannel(channelId, IpcProtocol::make(IpcProtocol::MSG_CONFIG_UPDATE, channelId, config));
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------

void SupervisorCore::onIpcMessage(const IpcProtocol::Message& msg, const QString& clientId) {
    using namespace IpcProtocol;
    switch (msg.type) {
    case MSG_ENGINE_HELLO:   // PS-4b: adoption confirmation / binding guard / untracked engine -> re-scan
        m_channels->onEngineHello(clientId, msg.channelId, msg.payload);
        break;
    case MSG_ENGINE_READY:
        m_channels->bindClient(msg.channelId, clientId);
        break;
    case MSG_HEARTBEAT:
        if (m_channels->channelForClient(clientId).isEmpty()) m_channels->bindClient(msg.channelId, clientId);
        // Binding guard (PS-4b): only the bound engine's heartbeats count (a refused duplicate's must not hide a hang).
        if (m_channels->channelForClient(clientId) == msg.channelId)
            m_channels->onHeartbeat(msg.channelId, qint64(msg.payload.value("frameNumber").toDouble()), msg.payload.value("isHealthy").toBool(true));
        break;
    case MSG_STATE_CHANGE: {
        if (m_channels->isRejectedClient(clientId)) break;   // PS-4b: a refused duplicate / killed engine is not the channel
        m_engineState[msg.channelId] = msg.payload.value("state").toString();
        QJsonObject o = msg.payload; o["channelId"] = msg.channelId;
        broadcastUi(MSG_UI_TELEMETRY, msg.channelId, o, "stateChange");
        break;
    }
    case MSG_UI_TELEMETRY: {   // engine telemetry (stateJson)
        if (m_channels->isRejectedClient(clientId)) break;   // PS-4b: as MSG_STATE_CHANGE
        m_telemetry[msg.channelId] = msg.payload;
        SystemStats::noteTelemetry(msg.channelId, msg.payload);
        m_engineState[msg.channelId] = msg.payload.value("state").toString();
        m_exec->onTelemetry(msg.channelId, msg.payload);
        if (m_overlays) m_overlays->onTelemetry(msg.channelId, msg.payload);
        if (m_mux) {                       // SCTE-35: a new on-air item may start or end a break on the services this channel feeds
            static QHash<QString, QString> lastItem;
            const QString itemId = msg.payload.value("itemId").toString();
            if (!itemId.isEmpty() && msg.payload.value("state").toString() == "ON_AIR" && lastItem.value(msg.channelId) != itemId) {
                lastItem[msg.channelId] = itemId;
                QString type;
                if (const RunlistItem* ri = m_exec->lookupRunlistItem(msg.channelId, itemId)) type = ri->item.type;
                else if (itemId.startsWith("take-")) {      // an operator take: the library type of what was taken (an ad played by hand is a break too)
                    bool f = false; MediaRecord m = m_store->media(msg.payload.value("assetId").toString(), &f);
                    if (!f && !msg.payload.value("path").toString().isEmpty() && !msg.payload.value("path").toString().contains("://")) m =m_store->mediaByPath(msg.payload.value("path").toString(), &f);
                    type = f ? m.type : QString("live");
                }
                const QJsonObject sc = m_mux->scte35Config();
                double breakSec = 0.0;
                if (sc.value("enabled").toBool(false) && scte35::isBreakType(sc, type)) {
                    QStringList bt; for (const QJsonValue& v : sc.value("breakTypes").toArray()) bt << v.toString();
                    breakSec = scte35::breakSecondsFrom(m_exec->runlistJson(msg.channelId), itemId, bt);
                }
                m_mux->onChannelItem(msg.channelId, itemId, type, breakSec);
            }
        }
        broadcastUi(MSG_UI_TELEMETRY, msg.channelId, msg.payload, "telemetry");
        break;
    }
    case MSG_ASRUN_LOG:   // a killed engine's last as-run lines are real history; a refused duplicate's are not (PS-4b)
        if (m_channels->isRefusedClient(clientId)) break;
        m_exec->onEngineAsRun(msg.channelId, msg.payload);
        break;
    case MSG_PREVIEW_FRAME: {
        // PS-5: a binary frame brings the JPEG as msg.binary (no base64, no big JSON string); an engine from before PS-5
        // still sends payload.jpegBase64, which liftPreview decodes once into rel.binary.
        IpcProtocol::Message rel = msg;
        const QByteArray jpeg = IpcProtocol::liftPreview(rel);
        // Program pictures are filed under the channel id; switcher input monitors under "<channel>:<key>" (e.g. main:in3),
        // so /preview/main:in3.mjpeg and image://preview/main:in3 work like a channel's.
        const QString input = rel.payload.value("input").toString();
        if (input.isEmpty() && rel.payload.value("meters").isObject()) m_webMeters[rel.channelId] = rel.payload.value("meters").toObject();
        const QString key = input.isEmpty() ? rel.channelId : rel.channelId + ":" + input;
        m_preview[key] = jpeg;
        // UI clients over IPC get the frame under that key; MJPEG streams get a part. Droppable (PS-1): a UI that is 4 MB
        // behind skips pictures instead of queueing them. Each wire form is built once for all UIs (PS-5): a binary frame
        // for UIs that accept them, the old JSON/base64 frame for the rest; a UI's preview subscription filters here too.
        rel.channelId = key;
        m_ipc->sendTo(m_uiClients, rel, IpcTransport::Droppable);
        auto it = m_mjpeg.find(key);
        if (it != m_mjpeg.end()) {
            QByteArray part = "--playplayframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(jpeg.size()) + "\r\n\r\n";
            part += jpeg; part += "\r\n";
            for (auto s = it->begin(); s != it->end();) {
                if ((*s)->state() != QAbstractSocket::ConnectedState) { (*s)->deleteLater(); s = it->erase(s); continue; }
                // A viewer on a slow link skips pictures instead of queueing seconds of them (the wall stays live, not late).
                if ((*s)->bytesToWrite() < 4 * jpeg.size() + 65536) (*s)->write(part);
                ++s;
            }
        }
        break;
    }
    case MSG_UI_ALERT:
        if (m_overlays) m_overlays->onEngineAlert(msg.channelId, msg.payload.value("level").toString("warning"), msg.payload.value("message").toString());
        pushAlert(msg.channelId, msg.payload.value("level").toString("warning"), msg.payload.value("message").toString());
        break;
    case MSG_ENGINE_FATAL: {
        pushAlert(msg.channelId, "critical", "engine fatal: " + msg.payload.value("reason").toString());
        const QString id = msg.channelId;
        // Binding guard (PS-4b): a refused or retired engine's fatal does not restart the channel's real engine.
        const QString bound = m_channels->channelForClient(clientId);
        if (bound.isEmpty() ? !m_channels->isChannelConnected(id) : bound == id)
            QTimer::singleShot(0, this, [this, id]() { if (m_channels) m_channels->restartChannel(id); });
        break;
    }
    case MSG_UI_QUERY_STATUS:
        // The app asks again every few seconds: the registration is logged once, not on every poll (PS-6).
        if (!m_uiClients.contains(clientId)) { m_uiClients.insert(clientId); qInfo() << "[Supervisor] UI client registered:" << clientId; }
        sendUiSnapshot(clientId);
        break;
    case MSG_UI_COMMAND:
        onUiCommand(msg.channelId.isEmpty() ? msg.payload.value("channelId").toString() : msg.channelId,
                    msg.payload.value("action").toString(), msg.payload.value("params").toObject(), clientId);
        break;
    default:
        break;
    }
}

void SupervisorCore::onIpcClientDisconnected(const QString& clientId) {
    m_uiClients.remove(clientId);
    if (m_state) m_state->forgetClient(clientId);
    m_channels->onClientDisconnected(clientId);
}

QJsonObject SupervisorCore::resolveTake(const QJsonObject& in, QString* why) const {
    QJsonObject p = in;
    // Every take carries a "take-" id, whichever UI sent it (the web dashboard sent none): that is how the executor
    // recognises a take on air and queues the schedule behind it instead of cutting it.
    if (!p.value("id").toString().startsWith("take-")) p["id"] = "take-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString path = p.value("path").toString(p.value("assetPath").toString());
    MediaRecord m; bool found = false;
    const QString mid = p.value("mediaId").toString(p.value("assetId").toString());
    if (!mid.isEmpty()) m = m_store->media(mid, &found);
    if (!found && !path.isEmpty()) m = m_store->mediaByPath(path, &found);
    const bool live = path.startsWith("live://") || (found && m.type == "live");
    if (!live) return p;
    // The source spec the library keeps for the entry, else what the live:// address itself says.
    QJsonObject src = found ? m.extra.value("liveSource").toObject() : QJsonObject();
    if (src.isEmpty() && path.startsWith("live://")) {
        const QString rest = path.mid(7); const int slash = rest.indexOf('/');
        const QString kind = (slash > 0 ? rest.left(slash) : rest).toLower(), ref = slash > 0 ? rest.mid(slash + 1) : QString();
        if (kind == "ndi") { src["type"] = "ndi"; src["name"] = ref; }
        else if (kind == "sdi" || kind == "sdi_in") { src["type"] = "sdi_in"; src["port"] = ref.toInt(); }
        else if (kind == "stream" || kind == "url") { src["type"] = "stream"; src["url"] = ref; src["realtime"] = true; }
        else if (kind == "camera" && m_onvif) {      // a camera of the cameras list (live://camera/<id>), also when its library entry is gone: its RTSP stream
            bool known = false; const QString uri = m_onvif->deviceJson(ref, &known).value("streamUri").toString();
            if (known && !uri.isEmpty()) { src["type"] = "stream"; src["url"] = uri; src["realtime"] = true; src["lowLatency"] = true; }
        }
        else if (kind == "rtsp" || kind == "srt" || kind == "udp" || kind == "rtmp" || kind == "http" || kind == "https") { src["type"] = "stream"; src["url"] = kind + "://" + ref; src["realtime"] = true; }
    }
    // Same order as RunlistBuilder::resolveItem for scheduled live items.
    QString engPath = src.value("url").toString();
    if (engPath.isEmpty() && !src.value("ndiName").toString().isEmpty()) engPath = "ndi://" + src.value("ndiName").toString();
    if (engPath.isEmpty() && !src.value("name").toString().isEmpty()) engPath = "ndi://" + src.value("name").toString();
    if (engPath.isEmpty() && src.value("port").toInt() > 0) engPath = "sdi://" + QString::number(src.value("port").toInt());
    if (engPath.isEmpty()) {
        if (why) *why = QString("'%1' has no NDI name, SDI port or stream URL; add it to the library again from Inputs").arg(p.value("title").toString(path));
        return QJsonObject();
    }
    p["path"] = engPath; p["source"] = src; p["type"] = "live";
    if (found) { p["mediaId"] = m.id; p["assetId"] = m.id; if (p.value("title").toString().isEmpty()) p["title"] = m.title; }
    return p;
}

void SupervisorCore::onUiCommand(const QString& channelId, const QString& action, const QJsonObject& params, const QString& from) {
    // W17 (STB-5): the input map renews its preview-boost lease every 15 s (cg "preview" with only boostSec, InputMap.qml),
    // 16 % of all Supervisor lines on 10-25. It goes to debug in its own category, off unless
    // QT_LOGGING_RULES="playplay.supervisor.lease.debug=true"; every other command (preview profile changes too) stays info.
    static const QLoggingCategory lcLease("playplay.supervisor.lease", QtInfoMsg);
    if (action.compare("cg", Qt::CaseInsensitive) == 0 && params.value("action").toString() == "preview" && !params.contains("base") && !params.contains("boost"))
        qCDebug(lcLease).noquote() << QString("[Supervisor] command cg preview lease on %1 from %2").arg(channelId, from);
    else
        qInfo().noquote() << QString("[Supervisor] command %1 on %2 from %3").arg(action, channelId, from);
    const QString a = action.toLower();
    if (a == "arm") m_exec->setArmed(channelId, params.value("armed").toBool(true));
    else if (a == "disarm") m_exec->setArmed(channelId, false);
    else if (a == "loadhour" || a == "load_hour") m_exec->loadHourNow(channelId);
    else if (a == "snap") m_exec->snapToSchedule(channelId);
    else if (a == "reload") m_exec->reloadChannel(channelId);
    else if (a == "restart") m_channels->restartChannel(channelId);
    else if (a == "playlist") pushPlaylist(channelId, params.value("playlist").toArray(), params.value("replaceCurrent").toBool(true));
    else if (a == "outputs") {
        ChannelProfile p = m_store->channel(channelId); p.outputs = params.value("outputs").toArray(); m_store->saveChannel(p);
        QJsonObject o; o["outputs"] = p.outputs;
        m_channels->sendToChannel(channelId, IpcProtocol::make(IpcProtocol::MSG_OUTPUT_CONFIG, channelId, o));
    }
    else if (a == "cg") m_channels->sendToChannel(channelId, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, channelId, params));
    else if (a == "take" || a == "playitem") {
        QString why;
        const QJsonObject p = resolveTake(params, &why);
        if (p.isEmpty()) { pushAlert(channelId, "error", "Take failed: " + why); return; }
        transport(channelId, a, p);
    }
    else if (a == "skip" && m_telemetry.value(channelId).value("itemId").toString().startsWith("take-")) {
        // Leaving an operator take: queue the schedule from the current time first, so the skip lands where the hour is now
        // (not where it was when the take started).
        m_exec->returnToSchedule(channelId);
        transport(channelId, a, params);
    }
    else transport(channelId, a, params);   // play pause stop skip goto seek hold cue
}

void SupervisorCore::broadcastUi(IpcProtocol::MessageType type, const QString& channelId, const QJsonObject& payload, const QString& wsType) {
    if (!m_uiClients.isEmpty()) {
        QJsonObject p = payload; if (!wsType.isEmpty()) p["_type"] = wsType;
        const IpcProtocol::Message m = IpcProtocol::make(type, channelId, p);
        // Telemetry (and state changes, sent as telemetry) and the periodic snapshot are replaced by the next one:
        // Droppable for a UI that is behind (PS-1). Alerts, runlists, as-run and schedule changes always go out.
        const bool droppable = type == IpcProtocol::MSG_UI_TELEMETRY || wsType == QLatin1String("snapshot");
        m_ipc->sendTo(m_uiClients, m, droppable ? IpcTransport::Droppable : IpcTransport::Normal);
        // PS-6: a UI that got this runlist does not get it again with its next status query.
        if (type == IpcProtocol::MSG_UI_RUNLIST && m_state) m_state->noteRunlistBroadcast(channelId, m_uiClients);
    }
    if (m_ws && m_ws->clientCount() > 0) {
        QJsonObject o = payload; o["type"] = wsType; if (!channelId.isEmpty()) o["channelId"] = channelId;
        m_ws->broadcast(o);
    }
}

void SupervisorCore::sendUiSnapshot(const QString& clientId) {
    // PS-6: a client's first query gets the snapshot, every channel's last telemetry and every runlist (as before); a repeated
    // query (the app polls every 4 s until perf-app PA-4) gets the snapshot (the published one when nothing meaningful changed)
    // and only the runlists that changed since this client last got one. Telemetry keeps coming at 1 Hz from the engines.
    if (!m_state) return;
    const StatePublisher::Reply r = m_state->queryReply(clientId, m_exec->channelIds());
    m_ipc->sendTo(clientId, IpcProtocol::make(IpcProtocol::MSG_UI_STATUS, QString(), r.snapshot));
    if (r.telemetry)
        for (auto it = m_telemetry.begin(); it != m_telemetry.end(); ++it)
            m_ipc->sendTo(clientId, IpcProtocol::make(IpcProtocol::MSG_UI_TELEMETRY, it.key(), it.value()));
    for (const QString& ch : r.runlists)
        m_ipc->sendTo(clientId, IpcProtocol::make(IpcProtocol::MSG_UI_RUNLIST, ch, m_exec->runlistJson(ch)));
}

void SupervisorCore::pushAlert(const QString& channelId, const QString& level, const QString& message) {
    QJsonObject a; a["channelId"] = channelId; a["level"] = level; a["message"] = message;
    a["utcMs"] = double(QDateTime::currentMSecsSinceEpoch());
    m_recentAlerts.append(a);
    while (m_recentAlerts.size() > 200) m_recentAlerts.removeFirst();
    if (level == "critical" || level == "error") qCritical().noquote() << "[Alert]" << channelId << message;
    else qInfo().noquote() << "[Alert]" << channelId << level << message;
    broadcastUi(IpcProtocol::MSG_UI_ALERT, channelId, a, "alert");
    emit alert(channelId, level, message);
}

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

QJsonObject SupervisorCore::channelSnapshot(const QString& channelId) const {
    QJsonObject o;
    bool found = false;
    const ChannelProfile p = m_store->channel(channelId, &found);
    o["profile"] = p.toJson();
    o["exists"] = found;
    const auto* rec = m_channels->channel(channelId);
    QJsonObject proc;
    proc["running"] = m_channels->isChannelRunning(channelId);
    proc["connected"] = m_channels->isChannelConnected(channelId);
    proc["status"] = rec ? rec->status : "none";
    proc["pid"] = rec ? double(rec->pid) : 0.0;
    proc["restarts"] = rec ? rec->restarts : 0;
    proc["lastHeartbeatMs"] = rec ? double(rec->lastHeartbeatMs) : 0.0;
    o["process"] = proc;
    o["executor"] = m_exec->status(channelId).toJson();
    o["engine"] = m_telemetry.value(channelId);
    o["engineState"] = m_engineState.value(channelId, "OFFLINE");
    o["hasPreview"] = m_preview.contains(channelId);
    o["slave"] = m_slaveStatus.value(channelId);
    return o;
}

QJsonObject SupervisorCore::fullSnapshot() const {
    QJsonObject o;
    o["supervisor"] = true;
    o["uptimeMs"] = double(m_uptime.elapsed());
    o["utcMs"] = double(QDateTime::currentMSecsSinceEpoch());
    o["ntpOffsetMs"] = double(m_time ? m_time->ntpOffset() : 0);
    o["restPort"] = m_opt.restPort; o["wsPort"] = m_opt.wsPort; o["ipcName"] = m_opt.ipcName;
    o["dataDir"] = QDir(m_opt.dataDir).absolutePath();
    QJsonArray chans;
    auto profiles = m_store->channels();
    std::stable_sort(profiles.begin(), profiles.end(), [](const ChannelProfile& a, const ChannelProfile& b) { return a.extra.value("automationSlot").toInt(999) < b.extra.value("automationSlot").toInt(999); });
    for (const ChannelProfile& p : profiles) {
        if (p.extra.contains("automationSlot") && !p.enabled) continue;
        chans.append(channelSnapshot(p.id));
    }
    o["channels"] = chans;
    QJsonArray alerts; for (int i = qMax(0, m_recentAlerts.size() - 20); i < m_recentAlerts.size(); ++i) alerts.append(m_recentAlerts[i]);
    o["alerts"] = alerts;
    if (m_mesh) o["mesh"] = m_mesh->status();
    if (m_onvif) o["cameras"] = m_onvif->devicesJson().size();
    if (m_mux) { QJsonObject mx; const QJsonObject st = m_mux->status(); mx["state"] = st.value("state"); mx["services"] = st.value("services"); mx["muxBitrate"] = st.value("metrics").toObject().value("muxBitrate"); o["mux"] = mx; }
    if (m_ingest) o["ingestPending"] = m_ingest->pending();
    QJsonObject cgh; for (auto it = m_cgHtml.begin(); it != m_cgHtml.end(); ++it) cgh[it.key()] = it.value(); o["cgHtml"] = cgh;
    o["htmlCg"] = QFileInfo(QDir(m_opt.webRoot).filePath("cg/templates.json")).exists();
    o["health"] = m_failover ? QJsonObject{{"score", m_failover->globalHealth().score}, {"status", m_failover->globalHealth().status}} : QJsonObject();
    return o;
}

// ---------------------------------------------------------------------------
// Media scan
// ---------------------------------------------------------------------------

QString SupervisorCore::uploadDir() const {
    const QString base = m_opt.mediaRoot.isEmpty() ? QDir(m_opt.dataDir).filePath("media") : m_opt.mediaRoot;
    return QDir(base).filePath("uploads");
}

QString SupervisorCore::ffprobePath() const {
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffprobe.exe");
    if (QFileInfo::exists(local)) return local;
    const QString onPath = QStandardPaths::findExecutable("ffprobe");
    return onPath.isEmpty() ? "ffprobe" : onPath;
}

int SupervisorCore::scanFolder(const QString& folder, bool recursive, const QString& defaultType) {
    return m_ingest ? m_ingest->scanFolder(folder, recursive, defaultType, "scan") : 0;
}

void SupervisorCore::pumpScan() { /* the IngestManager owns the probe queue now */ }

// ---------------------------------------------------------------------------
// REST
// ---------------------------------------------------------------------------

qint64 SupervisorCore::parseTimeMs(const QString& s, qint64 def) {
    if (s.isEmpty()) return def;
    bool ok = false; const qint64 n = s.toLongLong(&ok);
    if (ok) return n;
    const QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (dt.isValid()) return dt.toMSecsSinceEpoch();
    const QDate d = QDate::fromString(s, Qt::ISODate);
    if (d.isValid()) return QDateTime(d, QTime(0, 0)).toMSecsSinceEpoch();
    return def;
}

void SupervisorCore::registerRoutes() {
    using Q = QHash<QString, QString>;
    RestApiServer* r = m_rest.get();

    r->registerRoute("GET", "/api/v1/status", [this](const QJsonObject&, const Q&) { return fullSnapshot(); });
    r->registerRoute("GET", "/api/v1/time", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["utcMs"] = double(QDateTime::currentMSecsSinceEpoch()); o["ntpOffsetMs"] = double(m_time->ntpOffset());
        o["iso"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); return o;
    });
    r->registerRoute("GET", "/api/v1/alerts", [this](const QJsonObject&, const Q&) {
        QJsonObject o; QJsonArray a; for (const auto& x : m_recentAlerts) a.append(x); o["alerts"] = a; return o;
    });

    // --- channels ---------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/channels", [this](const QJsonObject&, const Q&) {
        QJsonObject o; QJsonArray a; for (const auto& p : m_store->channels()) a.append(channelSnapshot(p.id)); o["channels"] = a; return o;
    });
    r->registerRoute("POST", "/api/v1/channels", [this](const QJsonObject& body, const Q&) {
        ChannelProfile p = ChannelProfile::fromJson(body);
        if (p.id.isEmpty()) return err(400, "id required");
        m_store->saveChannel(p);
        if (p.enabled) spawnChannel(p);
        return channelSnapshot(p.id);
    });
    r->registerRoute("GET", "/api/v1/channels/{id}", [this](const QJsonObject&, const Q& q) {
        bool f = false; m_store->channel(q["id"], &f);
        return f ? channelSnapshot(q["id"]) : err(404, "no such channel");
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}", [this](const QJsonObject& body, const Q& q) {
        bool f = false; const ChannelProfile old = m_store->channel(q["id"], &f);
        QJsonObject merged = old.toJson();
        for (auto it = body.begin(); it != body.end(); ++it) merged[it.key()] = it.value();
        ChannelProfile p = ChannelProfile::fromJson(merged); p.id = q["id"];
        m_store->saveChannel(p);
        const bool restart = f && (p.frameRate != old.frameRate || p.width != old.width || p.height != old.height || p.audioChannels != old.audioChannels);
        if (restart || !m_channels->isChannelRunning(p.id)) { if (m_channels->channel(p.id)) m_channels->removeChannel(p.id); if (p.enabled) { spawnChannel(p); m_exec->reloadChannel(p.id); } }
        else {
            m_exec->reloadChannel(p.id);
            if (p.outputs != old.outputs) { QJsonObject o; o["outputs"] = p.outputs; m_channels->sendToChannel(p.id, IpcProtocol::make(IpcProtocol::MSG_OUTPUT_CONFIG, p.id, o)); }
        }
        return channelSnapshot(p.id);
    });
    r->registerRoute("DELETE", "/api/v1/channels/{id}", [this](const QJsonObject&, const Q& q) {
        m_exec->removeChannel(q["id"]); m_channels->removeChannel(q["id"]); m_store->deleteChannel(q["id"]);
        QJsonObject o; o["deleted"] = q["id"]; return o;
    });
    for (const QString& act : {"start", "stop", "restart", "arm", "disarm", "load-hour", "snap", "reload"}) {
        r->registerRoute("POST", "/api/v1/channels/{id}/" + act, [this, act](const QJsonObject& body, const Q& q) {
            const QString id = q["id"];
            if (act == "start") { bool f = false; const ChannelProfile p = m_store->channel(id, &f); if (!f) return err(404, "no such channel"); spawnChannel(p); }
            else if (act == "stop") { m_exec->setArmed(id, false); m_channels->stopChannel(id); }
            else onUiCommand(id, act == "load-hour" ? "loadhour" : act, body, "rest");
            return channelSnapshot(id);
        });
    }
    r->registerRoute("POST", "/api/v1/channels/{id}/transport", [this](const QJsonObject& body, const Q& q) {
        const QString action = q.value("action", body.value("action").toString());
        if (action.isEmpty()) return err(400, "action required");
        QJsonObject params = body.value("params").toObject();
        if (params.isEmpty()) { params = body; params.remove("action"); }
        // Accepted is not on air: a command for the engine is refused (503) when the engine is not connected, a take that cannot be
        // resolved to a source is refused (422), and a delivered command reports confirmed:false. The confirmation is the engine's
        // own state change / as-run row (WS "stateChange", "asrun"), which carries the same channel.
        const QString act = action.toLower();
        static const QSet<QString> supervisorSide = {"arm", "disarm", "loadhour", "load_hour", "snap", "reload", "restart", "outputs"};
        if (!supervisorSide.contains(act) && !m_channels->isChannelConnected(q["id"])) return err(503, QString("engine for channel %1 is not connected: the command was not delivered").arg(q["id"]));
        if (act == "take" || act == "playitem") { QString why; if (resolveTake(params, &why).isEmpty()) return err(422, "take refused: " + why); }
        onUiCommand(q["id"], action, params, "rest");
        QJsonObject o; o["ok"] = true; o["accepted"] = true; o["confirmed"] = false; o["action"] = action;
        o["commandId"] = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); o["channelId"] = q["id"]; o["acceptedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        return o;
    });
    r->registerRoute("POST", "/api/v1/channels/{id}/playlist", [this](const QJsonObject& body, const Q& q) {
        const QJsonArray pl = body.value("playlist").toArray();
        if (body.value("disarm").toBool(false)) m_exec->setArmed(q["id"], false);
        const bool ok = pushPlaylist(q["id"], pl, body.value("replaceCurrent").toBool(true));
        if (ok && body.value("play").toBool(true)) transport(q["id"], "play", {});
        QJsonObject o; o["ok"] = ok; o["items"] = pl.size(); return o;
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}/outputs", [this](const QJsonObject& body, const Q& q) {
        QJsonObject p; p["outputs"] = body.value("outputs").toArray();
        onUiCommand(q["id"], "outputs", p, "rest");
        return channelSnapshot(q["id"]);
    });
    r->registerRoute("GET", "/api/v1/channels/{id}/state", [this](const QJsonObject&, const Q& q) {
        QJsonObject o = m_telemetry.value(q["id"]); o["executor"] = m_exec->status(q["id"]).toJson(); o["engineState"] = m_engineState.value(q["id"], "OFFLINE"); return o;
    });
    r->registerRoute("GET", "/api/v1/channels/{id}/meters", [this](const QJsonObject&, const Q& q) {
        if (!m_webMeters.contains(q["id"])) return QJsonObject{{"available", false}};
        return m_webMeters.value(q["id"]);
    });
    r->registerRoute("GET", "/api/v1/channels/{id}/runlist", [this](const QJsonObject&, const Q& q) { return m_exec->runlistJson(q["id"]); });
    r->registerRoute("GET", "/api/v1/channels/{id}/planned", [this](const QJsonObject&, const Q& q) {
        bool rep = false; QJsonObject o; o["playlist"] = m_exec->plannedPlaylist(q["id"], QDateTime::currentMSecsSinceEpoch(), &rep); o["replaceCurrent"] = rep; return o;
    });
    r->registerRoute("POST", "/api/v1/channels/{id}/cg", [this](const QJsonObject& body, const Q& q) {
        onUiCommand(q["id"], "cg", body, "rest"); QJsonObject o; o["ok"] = true; return o;
    });
    // Output mixes: GET lists the channel's mixes, PUT {mixes:[{id, name, strips, master, solo, follow}]} saves them in the channel
    // profile and applies them to the running engine at once (the list is authoritative: a mix left out is removed; its outputs
    // fall back to main). Ids: letters, digits, '-' and '_'; "main" is implicit.
    r->registerRoute("GET", "/api/v1/channels/{id}/mixes", [this](const QJsonObject&, const Q& q) {
        bool found = false; const ChannelProfile p = m_store->channel(q["id"], &found);
        if (!found) return err(404, "no such channel");
        QJsonObject o; o["channelId"] = p.id; o["mixes"] = p.extra.value("mixes").toArray(); return o;
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}/mixes", [this](const QJsonObject& body, const Q& q) {
        bool found = false; ChannelProfile p = m_store->channel(q["id"], &found);
        if (!found) return err(404, "no such channel");
        static const QRegularExpression idRe("^[A-Za-z0-9_-]{1,32}$");
        QJsonArray clean; QSet<QString> seen;
        for (const QJsonValue& v : body.value("mixes").toArray()) {
            QJsonObject m = v.toObject();
            const QString id = m.value("id").toString().trimmed();
            if (!idRe.match(id).hasMatch() || id == "main" || seen.contains(id)) return err(422, QString("bad mix id '%1'").arg(id));
            seen.insert(id);
            if (m.value("name").toString().trimmed().isEmpty()) m["name"] = id;
            clean.append(m);
        }
        if (clean.size() > 7) return err(422, "at most 7 extra mixes");
        p.extra["mixes"] = clean;
        m_store->saveChannel(p);
        QJsonObject cfg; cfg["mixes"] = clean;
        const bool live = sendConfig(p.id, cfg);
        QJsonObject o; o["ok"] = true; o["mixes"] = clean; o["applied"] = live; return o;
    });
    // Switcher input monitors: live pictures of the input bus without putting anything on air (engine InputMonitor).
    // body {inputs:[{key:"in1", path:"live://ndi/..", mediaId, title, quality:"low"|"high"}], leaseSec}. The UI renews the
    // lease while the On Air page is open; the engine drops every receiver when it runs out. An empty list closes them now.
    r->registerRoute("POST", "/api/v1/channels/{id}/monitor", [this](const QJsonObject& body, const Q& q) {
        if (!m_channels->isChannelConnected(q["id"])) return err(503, QString("engine for channel %1 is not connected").arg(q["id"]));
        QJsonArray inputs, skipped, keys;
        for (const QJsonValue& v : body.value("inputs").toArray()) {
            const QJsonObject in = v.toObject();
            const QString key = in.value("key").toString();
            if (key.isEmpty() || key.contains(':') || key.size() > 32) continue;
            QString why;
            const QJsonObject res = resolveTake(in, &why);
            QJsonObject src = res.value("source").toObject();
            if (src.isEmpty()) { QJsonObject s; s["key"] = key; s["why"] = why.isEmpty() ? QString("not a live input") : why; skipped.append(s); continue; }
            // A feed with a second copy for monitors ("monitorUrl": a UDP port takes one reader, the take owns "url") is watched from that copy.
            if (src.contains("monitorUrl")) { const QString mu = src.value("monitorUrl").toString(); if (!mu.isEmpty()) src["url"] = mu; src.remove("monitorUrl"); }
            QString type = src.value("type").toString().toLower();
            if (type.isEmpty()) type = src.contains("url") ? "stream" : src.contains("port") ? "sdi_in" : "ndi";
            if (type == "sdi_in" || type == "sdi" || type == "dektec_sdi_in") {
                QJsonObject s; s["key"] = key; s["why"] = "SDI inputs show as a card: a second receiver on a card port would fight the take"; skipped.append(s); continue;
            }
            QJsonObject o; o["key"] = key; o["source"] = src; o["quality"] = in.value("quality").toString() == "high" ? "high" : "low";
            o["title"] = res.value("title").toString();
            inputs.append(o); keys.append(key);
        }
        QJsonObject cmd; cmd["action"] = "monitor"; cmd["inputs"] = inputs; cmd["leaseSec"] = qBound(5, body.value("leaseSec").toInt(45), 600);
        m_channels->sendToChannel(q["id"], IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, q["id"], cmd));
        // Forget the pictures of inputs that are no longer leased, so a stale frame never shows on a remapped key.
        const QString prefix = q["id"] + ":";
        for (auto it = m_preview.begin(); it != m_preview.end();) {
            if (it.key().startsWith(prefix) && !keys.contains(it.key().mid(prefix.size()))) it = m_preview.erase(it); else ++it;
        }
        QJsonObject o; o["ok"] = true; o["monitored"] = keys; o["skipped"] = skipped; o["leaseSec"] = cmd["leaseSec"]; return o;
    });

    // --- schedule --------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/schedule/{ch}/week", [this](const QJsonObject&, const Q& q) {
        QDate start = QDate::fromString(q.value("start"), Qt::ISODate);
        const QTimeZone tz = m_store->channel(q["ch"]).tz();
        if (!start.isValid()) start = QDateTime::currentDateTime().toTimeZone(tz).date();
        start = start.addDays(-scheduleDow(start));     // back to Sunday
        QJsonObject o; o["channelId"] = q["ch"]; o["weekStart"] = start.toString(Qt::ISODate);
        QJsonObject slotMap; for (const auto& s : m_store->week(q["ch"], start)) slotMap[s.isDated() && s.showBlock.contains("_override") ? s.id : HourSlot::templateId(scheduleDow(s.date), s.hour)] = s.toJson();
        o["slots"] = slotMap; return o;
    });
    r->registerRoute("GET", "/api/v1/schedule/{ch}/template", [this](const QJsonObject&, const Q& q) {
        QJsonObject o, slotMap; for (const auto& s : m_store->templateSlots(q["ch"])) slotMap[s.id] = s.toJson(); o["slots"] = slotMap; return o;
    });
    r->registerRoute("GET", "/api/v1/schedule/{ch}/slot/{slot}", [this](const QJsonObject&, const Q& q) {
        bool f = false; HourSlot s = m_store->slot(q["ch"], q["slot"], &f); QJsonObject o = s.toJson(); o["exists"] = f; return o;
    });
    r->registerRoute("PUT", "/api/v1/schedule/{ch}/slot/{slot}", [this](const QJsonObject& body, const Q& q) {
        HourSlot s = HourSlot::fromJson(body); s.channelId = q["ch"]; s.id = q["slot"];
        if (!HourSlot::parseId(s.id, &s.dayOfWeek, &s.date, &s.hour)) return err(400, "bad slot id");
        if (!m_store->saveSlot(s)) return err(500, "save failed");
        // PlayPlay OBS enforce_markers: generated-spot placements (GEN markers) survive any save path.
        if (m_spots) QTimer::singleShot(0, this, [this]() { if (m_spots) m_spots->enforceMarkers(); });
        return s.toJson();
    });
    r->registerRoute("DELETE", "/api/v1/schedule/{ch}/slot/{slot}", [this](const QJsonObject&, const Q& q) {
        m_store->deleteSlot(q["ch"], q["slot"]); QJsonObject o; o["deleted"] = q["slot"]; return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/slot/{slot}/validate", [this](const QJsonObject& body, const Q& q) {
        HourSlot s = body.isEmpty() ? m_store->slot(q["ch"], q["slot"]) : HourSlot::fromJson(body);
        s.channelId = q["ch"]; s.id = q["slot"]; HourSlot::parseId(s.id, &s.dayOfWeek, &s.date, &s.hour);
        const ChannelProfile p = m_store->channel(q["ch"]);
        const QDate d = s.isDated() ? s.date : QDateTime::currentDateTime().toTimeZone(p.tz()).date();
        ValidatorOptions vo; vo.requireLegalId = p.fill.value("requireLegalId").toBool(false);
        ScheduleStore* st = m_store.get();
        vo.media.byId = [st](const QString& id, bool* f) { return st->media(id, f); };
        vo.media.bySeries = [st](const QString& s2) { return st->mediaBySeries(s2); };
        vo.media.byTypes = [st](const QStringList& t) { return st->mediaByType(t); };
        return ScheduleValidator::validate(s, RunlistBuilder::hourStartUtcMs(d, s.hour, p.tz()), vo).toJson();
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/import", [this](const QJsonObject& body, const Q& q) {
        // PlayPlayClean WeeklySchedule JSON: { slots: { "D-H": HourSlot, "YYYY-MM-DD-H": HourSlot } }
        const QJsonObject slotMap = body.value("slots").toObject();
        int n = 0;
        for (auto it = slotMap.begin(); it != slotMap.end(); ++it) {
            HourSlot s = HourSlot::fromJson(it.value().toObject()); s.channelId = q["ch"]; s.id = it.key();
            if (!HourSlot::parseId(s.id, &s.dayOfWeek, &s.date, &s.hour)) continue;
            if (m_store->saveSlot(s)) ++n;
        }
        QJsonObject o; o["imported"] = n; return o;
    });

    // --- media -----------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/media", [this](const QJsonObject&, const Q& q) {
        QList<MediaRecord> list;
        if (!q.value("q").isEmpty()) list = m_store->searchMedia(q["q"], q.value("limit", "200").toInt());
        else if (!q.value("type").isEmpty()) list = m_store->mediaByType(q["type"].split(','));
        else list = m_store->allMedia(q.value("archived") == "1");
        QJsonObject o; QJsonArray a; for (const auto& m : list) a.append(m.toJson()); o["media"] = a; o["count"] = a.size(); return o;
    });
    r->registerRoute("POST", "/api/v1/media", [this](const QJsonObject& body, const Q&) {
        MediaRecord m = MediaRecord::fromJson(body);
        if (m.path.isEmpty()) return err(400, "path required");
        if (!m_store->saveMedia(m)) return err(500, "save failed");
        return m_store->mediaByPath(m.path).toJson();
    });
    r->registerRoute("GET", "/api/v1/media/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const MediaRecord m = m_store->media(q["id"], &f); return f ? m.toJson() : err(404, "no such media"); });
    r->registerRoute("PUT", "/api/v1/media/{id}", [this](const QJsonObject& body, const Q& q) {
        bool f = false; MediaRecord old = m_store->media(q["id"], &f); if (!f) return err(404, "no such media");
        QJsonObject merged = old.toJson(); for (auto it = body.begin(); it != body.end(); ++it) merged[it.key()] = it.value();
        MediaRecord m = MediaRecord::fromJson(merged); m.id = q["id"]; m.playCount = old.playCount; m.lastPlayedMs = old.lastPlayedMs;
        m_store->saveMedia(m); return m_store->media(q["id"]).toJson();
    });
    r->registerRoute("DELETE", "/api/v1/media/{id}", [this](const QJsonObject&, const Q& q) { m_store->deleteMedia(q["id"]); QJsonObject o; o["deleted"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/media/scan", [this](const QJsonObject& body, const Q&) {
        const QString type = body.value("type").toString("program");
        if (body.contains("files")) {
            // Explicit files (drag-and-drop from Explorer): queue each one for probing.
            int queued = 0;
            for (const auto& v : body.value("files").toArray()) {
                QString path = v.toString(); if (path.startsWith("file:///")) path = QUrl(path).toLocalFile();
                if (!QFileInfo::exists(path)) continue;
                if (QFileInfo(path).isDir()) { queued += m_ingest->scanFolder(path, true, type, "drop"); continue; }
                const QJsonObject j = m_ingest->enqueue(path, IngestManager::typeFromPath(path, type), "drop");
                if (j.value("state").toString() != "skipped") ++queued;
            }
            QJsonObject o; o["queued"] = queued; o["pending"] = m_ingest->pending(); return o;
        }
        QString folder = body.value("folder").toString(m_opt.mediaRoot).trimmed();
        if (folder.startsWith('"') && folder.endsWith('"') && folder.size() > 1) folder = folder.mid(1, folder.size() - 2);
        folder = QDir::fromNativeSeparators(folder);
        while (folder.size() > 3 && folder.endsWith('/')) folder.chop(1);
        if (folder.isEmpty()) return err(400, "folder required (a path on the Supervisor's machine)");
        if (!QFileInfo(folder).isDir()) return err(400, "folder not found on the Supervisor's machine: " + QDir::toNativeSeparators(folder));
        QJsonObject o; o["queued"] = scanFolder(folder, body.value("recursive").toBool(true), type); o["pending"] = m_ingest->pending(); return o;
    });

    // --- as-run ---------------------------------------------------------------------
    // PS-9 (W15): /asrun (up to 5,000 rows plus a summary over 100,000) runs on a read-only reader connection off the
    // event loop; when the reader could not open the database the route answers from the store as before.
    auto* asrunReader = new sched::StoreReader(m_store->path(), 2, this);
    r->registerAsync("GET", "/api/v1/asrun", [this, asrunReader](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 from = parseTimeMs(q.value("from"), now - 86400000LL), to = parseTimeMs(q.value("to"), now + 3600000LL);
        const QString channel = q.value("channel"); const int limit = q.value("limit", "5000").toInt();
        const bool queued = asrunReader->run(
            [channel, from, to, limit](QSqlDatabase& db) { return sched::StoreReader::asRunReport(db, channel, from, to, limit); },
            [reply](const QJsonObject& o) { reply(o.contains("error") ? o.value("status").toInt(503) : 200, o); });
        if (queued) return;
        QJsonObject o; QJsonArray a; for (const auto& r2 : m_store->asRun(channel, from, to, limit)) a.append(r2.toJson());
        o["rows"] = a; o["count"] = a.size(); o["fromMs"] = double(from); o["toMs"] = double(to); o["summary"] = m_store->asRunSummary(channel, from, to); reply(200, o);
    });
    r->registerAsync("GET", "/api/v1/asrun/summary", [this, asrunReader](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 from = parseTimeMs(q.value("from"), now - 86400000LL), to = parseTimeMs(q.value("to"), now); const QString channel = q.value("channel");
        const bool queued = asrunReader->run(
            [channel, from, to](QSqlDatabase& db) { return sched::StoreReader::asRunSummary(db, channel, from, to); },
            [reply](const QJsonObject& o) { reply(o.contains("error") ? o.value("status").toInt(503) : 200, o); });
        if (!queued) reply(200, m_store->asRunSummary(channel, from, to));
    });

    // --- settings --------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/settings/{key}", [this](const QJsonObject&, const Q& q) {
        const QString key = q["key"];
        if (key == "remote.nodes" || key == "remote.lan.discovery" || key == "station.password" || key == "rest.token")
            return err(403, "This setting contains credentials; use its dedicated management route");
        QJsonObject o; o["key"] = key; o["value"] = m_store->setting(key); return o;
    });
    r->registerRoute("PUT", "/api/v1/settings/{key}", [this](const QJsonObject& body, const Q& q) {
        const QJsonValue v = body.value("value");
        m_store->setSetting(q["key"], v.isString() ? v.toString() : QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)));
        QJsonObject o; o["key"] = q["key"]; o["ok"] = true; return o;
    });

    // --- previews (raw streaming) -------------------------------------------------
    // --- ingest: streaming upload + download ------------------------------------------------
    // PUT /api/v1/media/upload?name=<file>&type=<program|ad|...>  (body = file bytes, Content-Length required)
    r->registerUpload("PUT", "/api/v1/media/upload", [this](QTcpSocket* sock, const QString&, const Q& q, qint64 len, const QByteArray& initial) {
        const QString name = QFileInfo(q.value("name", "upload.bin")).fileName();
        const QString type = q.value("type", "program");
        if (name.isEmpty() || name.contains("..") || len < 0) { RestApiServer::writeRawHeader(sock, 400, "application/json", false); sock->write("{\"error\":\"name and Content-Length required\"}"); sock->disconnectFromHost(); return; }
        const QString dir = uploadDir();
        QDir().mkpath(dir);
        QString target = QDir(dir).filePath(name);
        if (QFileInfo::exists(target)) { const QFileInfo fi(target); target = QDir(dir).filePath(fi.completeBaseName() + "_" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + (fi.suffix().isEmpty() ? "" : "." + fi.suffix())); }
        // Pre-chosen specifics: ?meta=<URL-encoded JSON> carries what the operator chose before uploading (only these keys are taken).
        QJsonObject meta;
        {
            const QJsonObject in = QJsonDocument::fromJson(q.value("meta").toUtf8()).object();
            auto str = [&](const char* k) { const QString v = in.value(k).toString().trimmed(); if (!v.isEmpty()) meta[k] = v.left(300); };
            for (const char* k : {"title", "client", "series", "category", "subcategory", "notes", "episodeDate"}) str(k);
            if (in.value("priority").isDouble()) meta["priority"] = qBound(-10, in.value("priority").toInt(), 10);
            const QString st = in.value("status").toString(); if (st == "approved" || st == "review") meta["status"] = st;
            auto list = [&](const char* k, bool lower) { QJsonArray out; for (const QJsonValue& v : in.value(k).toArray()) { QString s = v.toString().trimmed().left(60); if (lower) s = s.toLower(); if (!s.isEmpty() && !out.contains(s)) out.append(s); } if (!out.isEmpty()) meta[k] = out; };
            list("tags", true); list("allowedChannels", false);
            for (const char* k : {"availableStartMs", "availableEndMs"}) if (in.value(k).toDouble() > 0) meta[k] = in.value(k).toDouble();
        }
        auto* file = new QFile(target + ".part", sock);
        if (!file->open(QIODevice::WriteOnly)) { RestApiServer::writeRawHeader(sock, 500, "application/json", false); sock->write("{\"error\":\"cannot write upload\"}"); sock->disconnectFromHost(); return; }
        auto remaining = std::make_shared<qint64>(len);
        auto finish = [this, sock, file, target, type, remaining, meta]() {
            file->close();
            QFile::remove(target); QFile::rename(target + ".part", target);
            const QString native = QDir::toNativeSeparators(target);
            bool found = false; m_store->mediaByPath(native, &found);
            if (!found) m_ingest->enqueue(native, IngestManager::typeFromPath(native, type), "upload", meta);
            QJsonObject o; o["ok"] = true; o["path"] = native; o["bytes"] = double(QFileInfo(target).size()); o["queued"] = !found; o["meta"] = meta;
            const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
            RestApiServer::writeRawHeader(sock, 200, "application/json", false);
            sock->write(body); sock->flush(); sock->disconnectFromHost();
            qInfo().noquote() << "[Ingest] uploaded" << native << QFileInfo(target).size() << "bytes";
        };
        if (!initial.isEmpty()) { file->write(initial); *remaining -= initial.size(); }
        if (*remaining <= 0) { finish(); return; }
        connect(sock, &QTcpSocket::readyRead, sock, [sock, file, remaining, finish]() {
            const QByteArray chunk = sock->read(*remaining);
            file->write(chunk); *remaining -= chunk.size();
            if (*remaining <= 0) finish();
        });
        connect(sock, &QTcpSocket::disconnected, sock, [sock, file, target, remaining]() {
            if (*remaining > 0) { file->close(); QFile::remove(target + ".part"); qWarning() << "[Ingest] upload aborted" << target; }
            sock->deleteLater();
        });
    });
    // GET /api/v1/download/{mediaId}  (streams the file; Content-Disposition attachment)
    r->registerRaw("GET", "/api/v1/download/", [this](QTcpSocket* sock, const QString& path, const Q&) {
        const QString id = path.mid(QString("/api/v1/download/").size());
        bool found = false; const MediaRecord m = m_store->media(id, &found);
        if (!found || !QFileInfo(m.path).isFile()) { RestApiServer::writeRawHeader(sock, 404, "application/json", false); sock->write("{\"error\":\"no such media\"}"); sock->disconnectFromHost(); return; }
        auto* file = new QFile(m.path, sock);
        if (!file->open(QIODevice::ReadOnly)) { RestApiServer::writeRawHeader(sock, 500, "application/json", false); sock->write("{\"error\":\"cannot read file\"}"); sock->disconnectFromHost(); return; }
        const QFileInfo fi(m.path);
        QByteArray h;
        h += "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
        h += "Content-Length: " + QByteArray::number(fi.size()) + "\r\n";
        h += "Content-Disposition: attachment; filename=\"" + fi.fileName().toUtf8() + "\"\r\n";
        h += "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
        sock->write(h);
        auto pump = [sock, file]() {
            while (!file->atEnd() && sock->bytesToWrite() < 4 * 1024 * 1024) sock->write(file->read(1024 * 1024));
            if (file->atEnd() && sock->bytesToWrite() == 0) sock->disconnectFromHost();
        };
        connect(sock, &QTcpSocket::bytesWritten, sock, [pump](qint64) { pump(); });
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
        pump();
    });
    r->registerRoute("GET", "/api/v1/media/upload-info", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["uploadDir"] = QDir::toNativeSeparators(uploadDir()); o["mediaRoot"] = QDir::toNativeSeparators(m_opt.mediaRoot);
        const QStorageInfo si(uploadDir()); o["freeBytes"] = double(si.bytesAvailable()); o["pendingProbes"] = m_ingest->pending(); return o;
    });

    registerIntegrationRoutes();
    registerLibraryRoutes();

    // STB-4: the event-loop watchdog (supervisor_main.cpp) reports each stall once the loop runs again.
    if (LoopWatchdog* wd = LoopWatchdog::instance())
        connect(wd, &LoopWatchdog::recovered, this, [this](const QString&, qint64 ms, const QString& activity, const QString& worst) {
            pushAlert("", "warning", QString("event loop %1 %2 s%3").arg(worst == "hang" ? "hung" : "stalled")
                      .arg(ms / 1000.0, 0, 'f', 1).arg(activity.isEmpty() ? QString() : " in " + activity));
        });

    // Prometheus text exposition: scrape http://host:7700/metrics
    r->registerRaw("GET", "/metrics", [this](QTcpSocket* sock, const QString&, const Q&) {
        QString m;
        auto g = [&](const QString& name, const QString& labels, double v, const QString& help = QString()) {
            if (!help.isEmpty()) m += "# HELP " + name + " " + help + "\n# TYPE " + name + " gauge\n";
            m += name + (labels.isEmpty() ? "" : "{" + labels + "}") + " " + QString::number(v, 'g', 12) + "\n";
        };
        g("playplay_supervisor_uptime_seconds", "", m_uptime.elapsed() / 1000.0, "Supervisor uptime");
        g("playplay_supervisor_channels", "", m_store->channels().size(), "Configured channels");
        g("playplay_supervisor_ui_clients", "", m_uiClients.size());
        g("playplay_supervisor_ws_clients", "", m_ws ? m_ws->clientCount() : 0);
        g("playplay_supervisor_ntp_offset_ms", "", m_time ? double(m_time->ntpOffset()) : 0.0);
        if (const LoopWatchdog* wd = LoopWatchdog::instance()) {   // STB-4 (a scrape runs on the loop: lag over the last 5-10 s)
            const LoopWatchdog::Stats ws = wd->stats();
            g("playplay_supervisor_loop_lag_ms", "", double(ws.recentMaxLagMs), "Supervisor event-loop lag, largest in the last 5-10 s");
            g("playplay_supervisor_loop_stalls_total", "", double(ws.stalls), "Supervisor event-loop stalls (watchdog)");
        }
        for (const ChannelProfile& p : m_store->channels()) {
            const QString l = "channel=\"" + p.id + "\"";
            const QJsonObject t = m_telemetry.value(p.id); const QJsonObject tt = t.value("telemetry").toObject();
            const auto st = m_exec->status(p.id);
            g("playplay_engine_connected", l, m_channels->isChannelConnected(p.id) ? 1 : 0, "Engine process connected");
            g("playplay_engine_on_air", l, t.value("state").toString() == "ON_AIR" ? 1 : 0, "Engine state is ON_AIR");
            g("playplay_channel_armed", l, st.armed ? 1 : 0);
            g("playplay_channel_emergency", l, st.emergency ? 1 : 0);
            g("playplay_engine_tick_ms", l, tt.value("tickMsLast").toDouble(), "Last frame tick cost");
            g("playplay_engine_tick_max_ms", l, tt.value("tickMsMax").toDouble());
            g("playplay_engine_late_ticks_total", l, tt.value("lateTicks").toDouble(), "Frame ticks delivered late");
            g("playplay_engine_missed_cuts_total", l, tt.value("missedCuts").toDouble(), "Items not ready at their cut");
            g("playplay_engine_decoder_queue", l, tt.value("decoderQueue").toDouble());
            g("playplay_engine_audio_underruns_total", l, tt.value("audioUnderruns").toDouble());
            g("playplay_engine_clock_trim_ppm", l, tt.value("clockTrimPpm").toDouble(), "Frame clock rate trim toward the SDI output");
            g("playplay_engine_clock_locked", l, tt.value("clockLocked").toBool() ? 1 : 0);
            g("playplay_engine_stall_seconds", l, tt.value("stallSeconds").toDouble());
            const QJsonObject au = t.value("compositor").toObject().value("audio").toObject();
            g("playplay_audio_momentary_lufs", l, au.value("momentaryLufs").toDouble(-70), "Program loudness, momentary");
            g("playplay_audio_integrated_lufs", l, au.value("integratedLufs").toDouble(-70));
            g("playplay_audio_peak_dbfs", l, au.value("peakDbfs").toDouble(-100));
            g("playplay_audio_clips_total", l, au.value("clips").toDouble());
            g("playplay_schedule_drift_seconds", l, st.driftSec, "Planned vs actual start of the on-air item");
            for (const auto& ov : t.value("outputs").toArray()) {
                const QJsonObject o = ov.toObject(); const QString n = o.value("name").toString(o.value("profile").toString());
                const QString ol = l + ",output=\"" + n + "\",kind=\"" + o.value("kind").toString() + "\"";
                if (o.value("kind").toString() == "encoder") { g("playplay_encoder_frames_total", ol, o.value("encoded").toDouble()); g("playplay_encoder_dropped_total", ol, o.value("dropped").toDouble()); g("playplay_encoder_avg_ms", ol, o.value("avgEncodeMs").toDouble()); continue; }
                g("playplay_output_connected", ol, o.value("connected").toBool() ? 1 : 0);
                g("playplay_output_dropped_total", ol, o.value("dropped").toDouble());
                g("playplay_output_bytes_total", ol, o.value("bytesOut").toDouble());
                if (o.contains("underruns")) g("playplay_output_underruns_total", ol, o.value("underruns").toDouble());
                if (o.contains("fifoLoadPct")) g("playplay_output_fifo_load_pct", ol, o.value("fifoLoadPct").toDouble());
            }
        }
        const QByteArray body = m.toUtf8();
        RestApiServer::writeRawHeader(sock, 200, "text/plain; version=0.0.4; charset=utf-8", false);
        sock->write(body); sock->flush(); sock->disconnectFromHost();
        connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
    });

    r->registerRaw("GET", "/preview/", [this](QTcpSocket* sock, const QString& path, const Q&) {
        QString id = path.mid(QString("/preview/").size());
        const bool mjpeg = id.endsWith(".mjpeg");
        id.remove(QRegularExpression("\\.(mjpeg|jpg|jpeg)$"));
        if (mjpeg) {
            RestApiServer::writeRawHeader(sock, 200, "multipart/x-mixed-replace; boundary=playplayframe", true);
            m_mjpeg[id].append(sock);
            connect(sock, &QTcpSocket::disconnected, this, [this, id, sock]() { m_mjpeg[id].removeAll(sock); sock->deleteLater(); });
            if (m_preview.contains(id)) {
                const QByteArray& jpeg = m_preview[id];
                sock->write("--playplayframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + QByteArray::number(jpeg.size()) + "\r\n\r\n" + jpeg + "\r\n");
            }
        } else {
            // PS-9: close() alone left the socket object under the QTcpServer (one per preview poll); it goes once
            // the bytes have left.
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            if (!m_preview.contains(id)) { RestApiServer::writeRawHeader(sock, 404, "text/plain", false); sock->write("no preview"); sock->disconnectFromHost(); if (sock->state() == QAbstractSocket::UnconnectedState) sock->deleteLater(); return; }
            const QByteArray& jpeg = m_preview[id];
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\nContent-Length: " + QByteArray::number(jpeg.size()) + "\r\nConnection: close\r\n\r\n" + jpeg);
            sock->flush(); sock->disconnectFromHost(); if (sock->state() == QAbstractSocket::UnconnectedState) sock->deleteLater();
        }
    });
}
