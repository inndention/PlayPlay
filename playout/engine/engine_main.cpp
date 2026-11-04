#include <QCoreApplication>
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include "engine.h"
#include "previewtap.h"
#include "compositor/livesources.h"
#include <thread>
#include <functional>
#include <memory>
#include "supervisorlink.h"
#include "../core/enginelock.h"
#include "../core/frameclock.h"
#include "../core/logsink.h"
#include "../core/loopwatchdog.h"
#include "../timeauthority.h"
#include "../ui_ipc/ipctransport.h"

/**
 * Channel Engine — per-channel playout process.
 *
 * Normally spawned by PlayPlaySupervisor, which pushes config/playlists over IPC.
 * For bench testing it also runs standalone:
 *
 *   PlayPlayChannelEngine --id main --config channel.json --playlist list.json --autoplay
 *
 * channel.json: { "frameRate": "30000/1001", "width": 1920, "height": 1080,
 *                 "outputs": [ { "type": "udp", "url": "udp://239.1.1.1:1234",
 *                                "profile": "h264_aac", "options": {"muxrate": 10000000} },
 *                              { "type": "ndi", "name": "PLAY PLAY Main" } ] }
 * list.json:    [ { "assetId": "a", "path": "C:/media/a.mp4" }, ... ]
 */

static QJsonDocument readJsonFile(const QString& path, bool* ok) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *ok = false; return {}; }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    *ok = (err.error == QJsonParseError::NoError);
    if (!*ok) qCritical() << "[Engine] JSON parse error in" << path << ":" << err.errorString();
    return doc;
}

int main(int argc, char* argv[]) {
    // Headless GUI application: the mixer uses QPainter/QFont for slates and CG,
    // which need a QGuiApplication, but the engine never opens a window. The
    // native platform plugin is used so system fonts are available; set
    // QT_QPA_PLATFORM=offscreen for environments without a window station.
    QGuiApplication app(argc, argv);
    app.setApplicationName("PlayPlayChannelEngine");
    app.setApplicationVersion("2.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("PlayPlay Per-Channel Playout Engine");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption idOpt("id", "Channel ID (required)", "channelId");
    QCommandLineOption nameOpt("name", "Channel name", "name", "");
    QCommandLineOption portOpt("port", "Watchdog /healthz port on 127.0.0.1 for this engine (0 = off)", "port", "0");
    QCommandLineOption dataOpt("data", "Data directory for this channel", "path", "./data");
    QCommandLineOption supOpt("supervisor", "Supervisor IPC name (empty = standalone)", "name", "PlayPlaySupervisor");
    QCommandLineOption fpsOpt("fps", "Frame rate (30000/1001, 29.97, 25, 50, 59.94)", "rate", "");
    QCommandLineOption configOpt("config", "Channel config JSON file", "file");
    QCommandLineOption playlistOpt("playlist", "Playlist JSON file (array of items)", "file");
    QCommandLineOption autoplayOpt("autoplay", "Start playing the playlist immediately");
    QCommandLineOption runForOpt("run-for", "Exit after N seconds (bench runs)", "seconds", "0");
    QCommandLineOption noSupOpt("standalone", "Do not connect to a supervisor");
    QCommandLineOption logDirOpt("log-dir", "Write JSON-line logs to this directory (daily files)", "dir", "");
    QCommandLineOption supPidOpt("supervisor-pid", "Process id of the Supervisor that started this engine: orphan mode when it exits", "pid", "0");
    QCommandLineOption takeoverOpt("takeover", "Start even while another engine still holds this channel (the Supervisor killed it)");
    parser.addOption(idOpt); parser.addOption(nameOpt); parser.addOption(portOpt); parser.addOption(dataOpt);
    parser.addOption(supOpt); parser.addOption(fpsOpt); parser.addOption(configOpt); parser.addOption(playlistOpt);
    parser.addOption(autoplayOpt); parser.addOption(runForOpt); parser.addOption(noSupOpt); parser.addOption(logDirOpt);
    parser.addOption(supPidOpt); parser.addOption(takeoverOpt);
    parser.process(app);
    if (!parser.value(logDirOpt).isEmpty()) installFileLogging(parser.value(logDirOpt), "engine-" + parser.value("id"));
    // PS-4b: the Supervisor passes its pid / the takeover flag in the environment (enginelock.h); the options win.
    const enginelock::LaunchEnv launchEnv = enginelock::takeLaunchEnv();
    const qint64 supervisorPid = parser.isSet(supPidOpt) ? parser.value(supPidOpt).toLongLong() : launchEnv.supervisorPid;
    const bool takeover = parser.isSet(takeoverOpt) || launchEnv.takeover;

    if (!parser.isSet(idOpt)) { qCritical() << "[Engine] --id is required"; return 1; }
    // PS-3: one engine per channel per Supervisor (named mutex), claimed before the /healthz port, the outputs or
    // engine.lock are touched. A second engine waits up to 5 s for the first to go, then exits with code 4 instead of
    // fighting it for the NDI name / SDI port / UDP group. Standalone bench runs are not guarded (as before).
    const bool standalone = parser.isSet(noSupOpt) || parser.value(supOpt).isEmpty();
    std::unique_ptr<enginelock::InstanceGuard> guard;
    if (!standalone) {
        guard = std::make_unique<enginelock::InstanceGuard>(enginelock::mutexName(parser.value(supOpt), parser.value(idOpt)));
        const int claim = SupervisorLink::claimChannel(*guard, parser.value(idOpt), parser.value(dataOpt), takeover);
        if (claim != 0) return claim;
    }
    // STB-4: event-loop watchdog of the main (IPC) thread, report only; /healthz on the --port ChannelManager reserves.
    LoopWatchdogOptions wdOpt;
    wdOpt.name = "engine-" + parser.value(idOpt);
    wdOpt.logDir = parser.value(logDirOpt);
    wdOpt.healthPort = qBound(0, parser.value(portOpt).toInt(), 65535);
    LoopWatchdog::install(wdOpt);

    // --- Config ---------------------------------------------------------------
    QJsonObject cfgJson;
    QString configHash;   // of the start config file (HELLO, engine.lock)
    if (parser.isSet(configOpt)) {
        bool ok = false;
        cfgJson = readJsonFile(parser.value(configOpt), &ok).object();
        if (!ok) return 2;
        QFile cf(parser.value(configOpt));
        if (cf.open(QIODevice::ReadOnly)) configHash = QString::fromLatin1(QCryptographicHash::hash(cf.readAll(), QCryptographicHash::Sha1).toHex());
    }
    cfgJson["channelId"] = parser.value(idOpt);
    if (parser.isSet(nameOpt)) cfgJson["channelName"] = parser.value(nameOpt);
    if (parser.isSet(fpsOpt)) cfgJson["frameRate"] = parser.value(fpsOpt);
    PlayoutEngine::Config cfg = PlayoutEngine::Config::fromJson(cfgJson);

    const QString dataDir = parser.value(dataOpt);
    QDir().mkpath(dataDir);
    qInfo().noquote() << QString("[Engine] starting %1 (%2) %3x%4 @%5 data=%6")
        .arg(cfg.channelId, cfg.channelName).arg(cfg.width).arg(cfg.height).arg(cfg.frameRate.toString(), dataDir);

    // --- Time authority (slave) + frame clock -------------------------------
    TimeAuthority* timeAuthority = getTimeAuthority();
    timeAuthority->setMode(TimeAuthority::Slave);
    timeAuthority->setFrameRate(cfg.frameRate.nominalFps());

    FrameClock frameClock;
    frameClock.setFrameRate(cfg.frameRate);

    // --- Engine ---------------------------------------------------------------
    PlayoutEngine engine(cfg);
    if (!engine.initialize()) { qCritical() << "[Engine] initialise failed"; return 3; }

    engine.setFrameClock(&frameClock);

    // Frame tick -> engine, on the clock thread (frame-accurate).
    QObject::connect(&frameClock, &FrameClock::frameTick, &engine,
        [&engine](qint64 f, qint64 ns) { engine.onFrameTick(f, ns); }, Qt::DirectConnection);

    // --- Supervisor link (PS-3, supervisorlink.h) -------------------------------
    // Async connect retried every second (never blocks this thread), MSG_ENGINE_HELLO + MSG_ENGINE_READY on every
    // (re)connect, engine.lock, and the parent watch: when the Supervisor exits the engine plays on as an orphan until a
    // Supervisor adopts it, or exits (outputs stopped, code 5) after orphanGraceSec / when a Supervisor that does not
    // adopt it takes the channel. The link's own lines are "[SupervisorLink] ...".
    std::unique_ptr<SupervisorLink> link;
    bool connected = false;   // follows the link's socket
    if (!standalone) {
        SupervisorLink::Options lo;
        lo.ipcName = parser.value(supOpt);
#ifdef PLAYPLAY_DEV_TREE
        if (lo.ipcName == QLatin1String("PlayPlaySupervisor")) { std::fprintf(stderr, "PlayPlayChannelEngine (dev tree): refusing to join the live Supervisor; use --supervisor <test name> or --standalone.\n"); return 3; }
#endif
        lo.channelId = cfg.channelId;
        lo.dataDir = dataDir;
        lo.supervisorPid = supervisorPid;
        lo.orphanGraceSec = SupervisorLink::resolveOrphanGraceSec(cfgJson, lo.supervisorPid > 0);
        lo.adoptWaitSec = SupervisorLink::resolveAdoptWaitSec(cfgJson);
        lo.configHash = configHash;
        lo.readyPayload = QJsonObject{{"frameRate", cfg.frameRate.toString()}, {"width", cfg.width}, {"height", cfg.height}};
        link = std::make_unique<SupervisorLink>(lo);
        QObject::connect(link.get(), &SupervisorLink::connectedChanged, &app, [&connected](bool up) { connected = up; });
        QObject::connect(link.get(), &SupervisorLink::exitRequested, &app, [](int code, const QString&) { QCoreApplication::exit(code); });
        link->setFactsProvider([&engine, &frameClock]() {
            const PlayoutEngine::State s = engine.currentState();
            QJsonArray outs;
            for (const QJsonValue& o : engine.outputsStatus()) outs.append(o.toObject().value("name"));
            return QJsonObject{{"state", s.stateName}, {"itemId", s.currentItemId}, {"assetId", s.currentAssetId},
                               {"frame", double(frameClock.currentFrame())}, {"outputs", outs}};
        });
        link->start();   // HELLO / READY go out once the event loop runs, after the setup below (as READY did)
    }

    std::function<void(const QJsonObject&)> rescanInputs;   // set up with the discovery timer below

    if (link) QObject::connect(link.get(), &SupervisorLink::messageReceived, &engine,
        [&](const IpcProtocol::Message& msg) {
            switch (msg.type) {
            case IpcProtocol::MSG_TIME_SYNC:
                // The engine's frame clock is free-running on the monotonic clock;
                // the master only supplies the NTP offset for wall-clock labelling.
                // (Slewing the frame timeline to house reference arrives with genlock.)
                timeAuthority->setNtpOffset(qint64(msg.payload.value("masterOffsetNs").toDouble()) / 1000000LL);
                break;
            case IpcProtocol::MSG_TRANSPORT_COMMAND:
                if (msg.payload.value("action").toString() == "rescan_inputs") { if (rescanInputs) rescanInputs(msg.payload.value("params").toObject().value("ndi").toObject()); break; }
                engine.handleTransportCommand(msg.payload.value("action").toString(), msg.payload.value("params").toObject());
                break;
            case IpcProtocol::MSG_PLAYLIST_PUSH:
                engine.loadPlaylist(msg.payload.value("playlist").toArray(), msg.payload.value("replaceCurrent").toBool(false));
                break;
            case IpcProtocol::MSG_CONFIG_UPDATE:
                engine.applyConfig(msg.payload);
                if (msg.payload.contains("ndi") && rescanInputs) rescanInputs(msg.payload.value("ndi").toObject());
                break;
            case IpcProtocol::MSG_OUTPUT_CONFIG: {
                QJsonObject cfg; cfg["outputs"] = msg.payload.value("outputs").toArray();
                engine.applyConfig(cfg);
                break;
            }
            case IpcProtocol::MSG_CG_COMMAND:
                engine.handleCgCommand(msg.payload);
                break;
            default: break;
            }
        });

    const QString channelId = cfg.channelId;
    QObject::connect(&engine, &PlayoutEngine::stateChanged, &app, [&](const QString& state, const QString& reason) {
        if (connected) link->send(IpcProtocol::makeStateChange(channelId, state, reason));
    });
    QObject::connect(&engine, &PlayoutEngine::asRunEvent, &app, [&](const QJsonObject& ev) {
        qInfo().noquote() << "[AsRun]" << QJsonDocument(ev).toJson(QJsonDocument::Compact);
        if (connected) {
            IpcProtocol::Message m;
            m.type = IpcProtocol::MSG_ASRUN_LOG;
            m.channelId = channelId;
            m.timestampNs = FrameClock::nowNs();
            m.payload = ev;
            link->send(m);
        }
    });
    QObject::connect(&engine, &PlayoutEngine::telemetry, &app, [&](const QJsonObject& st) {
        if (connected) {
            IpcProtocol::Message m;
            m.type = IpcProtocol::MSG_UI_TELEMETRY;
            m.channelId = channelId;
            m.timestampNs = FrameClock::nowNs();
            m.payload = st;
            link->send(m);
        } else {
            const QJsonObject t = st.value("telemetry").toObject();
            qInfo().noquote() << QString("[Engine] %1 f=%2 tc=%3 pos=%4/%5 tick=%6ms(max %7) late=%8 dq=%9 au=%10 drops=%11 trim=%12ppm%13")
                .arg(st.value("state").toString()).arg(st.value("frame").toDouble(), 0, 'f', 0)
                .arg(st.value("timecode").toString())
                .arg(st.value("position").toDouble(), 0, 'f', 2).arg(st.value("duration").toDouble(), 0, 'f', 2)
                .arg(t.value("tickMsLast").toDouble(), 0, 'f', 2).arg(t.value("tickMsMax").toDouble(), 0, 'f', 2)
                .arg(t.value("lateTicks").toDouble(), 0, 'f', 0).arg(t.value("decoderQueue").toDouble(), 0, 'f', 0)
                .arg(t.value("audioUnderruns").toDouble(), 0, 'f', 0).arg(t.value("outputDrops").toDouble(), 0, 'f', 0)
                .arg(t.value("clockTrimPpm").toDouble(), 0, 'f', 1).arg(t.value("clockLocked").toBool() ? " LOCKED" : "");
            for (const auto& ov : st.value("outputs").toArray()) {
                const QJsonObject o = ov.toObject();
                if (o.contains("underruns") && !o.contains("fifoLoadPct")) {
                    qInfo().noquote() << QString("[Engine]   %1 %2 frames=%3 underruns=%4 overruns=%5 skipped=%6 queued=%7 audioFifo=%8 cbErr=%9 delay=%10f")
                        .arg(o.value("name").toString(), o.value("videoStd").toString())
                        .arg(o.value("framesOut").toDouble(), 0, 'f', 0).arg(o.value("underruns").toDouble(), 0, 'f', 0)
                        .arg(o.value("overruns").toDouble(), 0, 'f', 0).arg(o.value("skippedFrames").toDouble(), 0, 'f', 0)
                        .arg(o.value("queuedFrames").toInt()).arg(o.value("audioFifo").toInt())
                        .arg(o.value("callbackErrors").toDouble(), 0, 'f', 0).arg(o.value("endToEndDelayFrames").toInt());
                }
                if (o.contains("fifoLoadPct")) {
                    qInfo().noquote() << QString("[Engine]   %1 fifo=%2%% (%3 B) tsRate=%4 ufl=%5 ovf=%6 tx=%7 queued=%8 B out=%9 MB")
                        .arg(o.value("name").toString()).arg(o.value("fifoLoadPct").toDouble(), 0, 'f', 1)
                        .arg(o.value("fifoLoadBytes").toDouble(), 0, 'f', 0).arg(o.value("tsRate").toDouble(), 0, 'f', 0)
                        .arg(o.value("underflows").toDouble(), 0, 'f', 0).arg(o.value("overflows").toDouble(), 0, 'f', 0)
                        .arg(o.value("transmitting").toBool()).arg(o.value("queuedBytes").toDouble(), 0, 'f', 0)
                        .arg(o.value("bytesOut").toDouble() / 1048576.0, 0, 'f', 1);
                }
            }
        }
    });
    QObject::connect(&engine, &PlayoutEngine::error, &app, [&](const QString& msg) {
        qWarning().noquote() << "[Engine] ERROR" << msg;
        if (connected) {
            IpcProtocol::Message m;
            m.type = IpcProtocol::MSG_UI_ALERT;
            m.channelId = channelId;
            m.timestampNs = FrameClock::nowNs();
            m.payload["level"] = "error";
            m.payload["message"] = msg;
            link->send(m);
        }
    });

    // Preview snapshots -> Supervisor (UI multiview, web dashboard). The JPEG rides as msg.binary (PS-5): a binary frame
    // to a Supervisor that accepts them, else the transport inlines it as payload["jpegBase64"] exactly as before.
    if (engine.previewTap()) {
        QObject::connect(engine.previewTap(), &PreviewTap::frameReady, &app,
            [&](const QByteArray& jpeg, int w, int h, qint64 frame) {
                if (!connected) return;
                QJsonObject p;
                p["width"] = w; p["height"] = h; p["frame"] = double(frame);
                p["meters"] = engine.meters();     // VU rides along at the preview rate (~5 Hz)
                link->send(IpcProtocol::makePreviewFrame(channelId, jpeg, p));
            }, Qt::QueuedConnection);
    }
    // Extra output mixes -> Supervisor, filed as "<channel>:mix-<id>" like an input monitor.
    QObject::connect(&engine, &PlayoutEngine::mixPreviewReady, &app,
        [&](const QString& mixId, const QByteArray& jpeg, int w, int h) {
            if (!connected) return;
            QJsonObject p;
            p["input"] = "mix-" + mixId;
            p["width"] = w; p["height"] = h;
            link->send(IpcProtocol::makePreviewFrame(channelId, jpeg, p));
        }, Qt::QueuedConnection);
    // Switcher input monitors -> Supervisor, which files them as "<channel>:<key>" (no meters: those belong to program).
    if (engine.inputMonitor()) {
        QObject::connect(engine.inputMonitor(), &InputMonitor::frameReady, &app,
            [&](const QString& key, const QByteArray& jpeg, int w, int h) {
                if (!connected) return;
                QJsonObject p;
                p["input"] = key;
                p["width"] = w; p["height"] = h;
                link->send(IpcProtocol::makePreviewFrame(channelId, jpeg, p));
            }, Qt::QueuedConnection);
    }

    // Live input discovery (NDI finder) every 10 s on a worker thread. The scan honours the
    // channel config's "ndi": {groups, extraIps} (comma-separated), so sources on other subnets
    // or in private NDI groups are found too; the Supervisor can trigger a rescan at any time.
    QTimer discoverTimer;
    std::atomic<bool> discovering{false};
    auto ndiGroups = std::make_shared<QString>(cfgJson.value("ndi").toObject().value("groups").toString());
    auto ndiExtraIps = std::make_shared<QString>(cfgJson.value("ndi").toObject().value("extraIps").toString());
    auto discoverNow = [&, ndiGroups, ndiExtraIps]() {
        if (!NdiSource::isAvailable() || discovering.load()) return;
        discovering = true;
        const QString g = *ndiGroups, e = *ndiExtraIps;
        std::thread([&engine, &discovering, g, e]() {
            const QJsonArray a = NdiSource::discoverFull(1500, g, e);
            QMetaObject::invokeMethod(&engine, "setDiscoveredInputs", Qt::QueuedConnection, Q_ARG(QJsonArray, a));
            discovering = false;
        }).detach();
    };
    QObject::connect(&discoverTimer, &QTimer::timeout, &app, discoverNow);
    discoverTimer.start(10000);
    QTimer::singleShot(1500, &app, discoverNow);
    rescanInputs = [&, ndiGroups, ndiExtraIps](const QJsonObject& ndi) {
        if (ndi.contains("groups")) *ndiGroups = ndi.value("groups").toString();
        if (ndi.contains("extraIps")) *ndiExtraIps = ndi.value("extraIps").toString();
        discoverNow();
    };

    // Heartbeat 10 Hz
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&]() {
        if (!connected) return;
        const auto s = engine.currentState();
        link->send(IpcProtocol::makeHeartbeat(channelId, frameClock.currentFrame(), int(s.decoderQueue), s.isHealthy));
    });
    heartbeat.start(100);

    QObject::connect(&frameClock, &FrameClock::driftDetected, &app, [&](qint64 driftNs, qint64 frame) {
        qWarning() << "[Engine] clock late" << driftNs / 1000 << "us at frame" << frame;
        if (connected && std::llabs(driftNs) > 2000000000LL) {   // > 2 s stall: let the supervisor restart us
            IpcProtocol::Message m;
            m.type = IpcProtocol::MSG_ENGINE_FATAL;
            m.channelId = channelId;
            m.payload["reason"] = QString("drift_%1_ns").arg(driftNs);
            link->send(m);
        }
    });

    // --- Standalone playlist ----------------------------------------------------
    if (parser.isSet(playlistOpt)) {
        bool ok = false;
        const QJsonDocument doc = readJsonFile(parser.value(playlistOpt), &ok);
        if (!ok) return 2;
        const QJsonArray items = doc.isArray() ? doc.array() : doc.object().value("items").toArray();
        engine.loadPlaylist(items, true);
        qInfo() << "[Engine] playlist loaded:" << items.size() << "items";
    }

    frameClock.setRunning(true);
    if (parser.isSet(autoplayOpt)) engine.handleTransportCommand("play", {});
    // MSG_ENGINE_READY (frameRate, width, height, pid) is sent by the link after MSG_ENGINE_HELLO on every connect.

    const int runFor = parser.value(runForOpt).toInt();
    if (runFor > 0) QTimer::singleShot(runFor * 1000, &app, &QCoreApplication::quit);

    // Clean shutdown on Ctrl+C / supervisor kill.
    static QCoreApplication* s_app = &app;
    std::signal(SIGINT, [](int) { if (s_app) QMetaObject::invokeMethod(s_app, &QCoreApplication::quit, Qt::QueuedConnection); });
    std::signal(SIGTERM, [](int) { if (s_app) QMetaObject::invokeMethod(s_app, &QCoreApplication::quit, Qt::QueuedConnection); });

    qInfo() << "[Engine] channel" << channelId << "ready";
    const int rc = app.exec();

    frameClock.setRunning(false);
    frameClock.wait(2000);
    engine.shutdown();
    connected = false;
    link.reset();   // after the outputs stopped: removes engine.lock (if still ours); the guard goes when main returns
    qInfo() << "[Engine] exited" << rc;
    return rc;
}
