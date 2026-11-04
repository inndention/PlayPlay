// fake_engine: a stand-in for PlayPlayChannelEngine in ChannelManager / reaper tests (perf-supervisor PS-2, PS-4b).
//
// Takes the engine's command line (--id, --name, --port, --data, --supervisor, --config, --supervisor-pid, --takeover;
// other extras ignored), connects to the --supervisor IPC name like the engine does and sends READY and heartbeats at
// 10 Hz. No video, no NDI, no outputs. Three link behaviours:
//   (default)     PS-2: one blocking connect; exits when the link drops (a Supervisor's death ends the test engine).
//   --fake-link   PS-4b: the real engine's SupervisorLink (playout/engine/supervisorlink.cpp) and duplicate guard, as
//                 engine_main wires them: HELLO + READY on every (re)connect, reconnect every second, engine.lock in
//                 --data, parent watch / orphan mode / adoption by MSG_SUP_ALIVE, exit 4 (duplicate) / 5 (orphan
//                 grace or adopt wait). orphanGraceSec / orphanAdoptWaitSec come from the --config JSON.
//   --fake-stay   PS-4b: an engine from before PS-3 ("legacy"): one connect attempt, READY + heartbeats if it got
//                 through, no HELLO, no lock, never reconnects, keeps running when the link drops, ignores FATAL.
// Test controls arrive as MSG_TRANSPORT_COMMAND actions:
//   fake_hang            stop heartbeating but keep running and connected (a hung engine)
//   fake_resume          heartbeat again
//   fake_exit {code}     exit at once with that code (a crash: no clean shutdown)
//   fake_fatal {reason}  send MSG_ENGINE_FATAL
// Every start appends "<pid> sp=<supervisor pid> to=<takeover 0|1>\n" to <data>/fake_engine_starts.txt (what it got from
// the options or the environment), so a test counts real process launches.
//   --fake-strict  behaves like an engine exe from before PS-3: any option that engine did not know exits 1.
// --supervisor-pid / --takeover may also arrive as PLAYPLAY_SUPERVISOR_PID / PLAYPLAY_ENGINE_TAKEOVER (as the engine).
// Safety: it exits after --fake-max-life-ms (default 120 s), so no copy outlives its test. It never uses a default IPC
// name: without --supervisor it exits with code 2.
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include "playout/core/enginelock.h"
#include "playout/engine/supervisorlink.h"
#include "playout/ui_ipc/ipctransport.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QString id, sup, data, config;
    qint64 supPid = 0;
    int maxLifeMs = 120000;
    bool linkMode = false, stayMode = false, takeover = false;
    const QStringList a = app.arguments();
    if (a.contains("--fake-strict")) {
        // An engine exe from before PS-3: QCommandLineParser::process() exits 1 on an option it does not know.
        static const QStringList withValue{"--id", "--name", "--port", "--data", "--supervisor", "--config", "--fps", "--playlist",
                                           "--run-for", "--log-dir", "--fake-max-life-ms"};
        for (int i = 1; i < a.size(); ++i) {
            if (withValue.contains(a[i])) { ++i; continue; }
            if (a[i].startsWith("--fake-") || a[i] == "--autoplay" || a[i] == "--standalone") continue;
            std::fprintf(stderr, "Unknown option '%s'.\n", qPrintable(a[i]));
            return 1;
        }
    }
    for (int i = 1; i < a.size(); ++i) {
        if (a[i] == "--fake-link") { linkMode = true; continue; }
        if (a[i] == "--fake-stay") { stayMode = true; continue; }
        if (a[i] == "--takeover") { takeover = true; continue; }
        if (i + 1 >= a.size()) break;
        if (a[i] == "--id") id = a[++i];
        else if (a[i] == "--supervisor") sup = a[++i];
        else if (a[i] == "--data") data = a[++i];
        else if (a[i] == "--config") config = a[++i];
        else if (a[i] == "--supervisor-pid") supPid = a[++i].toLongLong();
        else if (a[i] == "--fake-max-life-ms") maxLifeMs = a[++i].toInt();
    }
    // As engine_main: the Supervisor's pid / takeover arrive in the environment (enginelock::takeLaunchEnv); options win.
    {
        const enginelock::LaunchEnv le = enginelock::takeLaunchEnv();
        if (!supPid) supPid = le.supervisorPid;
        takeover = takeover || le.takeover;
    }
    if (sup.isEmpty() || id.isEmpty()) return 2;
    if (maxLifeMs > 0) QTimer::singleShot(maxLifeMs, &app, []() { QCoreApplication::exit(5); });

    qint64 frame = 0;
    QTimer heartbeat;
    std::function<void(const IpcProtocol::Message&)> sendFn;
    auto onCommand = [&](const IpcProtocol::Message& m) {
        if (m.type != IpcProtocol::MSG_TRANSPORT_COMMAND) return;
        const QString action = m.payload.value("action").toString();
        const QJsonObject p = m.payload.value("params").toObject();
        if (action == "fake_hang") heartbeat.stop();
        else if (action == "fake_resume") heartbeat.start(100);
        else if (action == "fake_exit") std::_Exit(p.value("code").toInt(1));
        else if (action == "fake_fatal") {
            IpcProtocol::Message f; f.type = IpcProtocol::MSG_ENGINE_FATAL; f.channelId = id;
            f.payload["reason"] = p.value("reason").toString("fake");
            sendFn(f);
        }
    };
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&]() { sendFn(IpcProtocol::makeHeartbeat(id, frame += 3, 4, true)); });

    if (linkMode) {
        // As engine_main: the guard before anything else, then the link (engine.lock, HELLO, reconnects, orphan mode).
        auto guard = std::make_unique<enginelock::InstanceGuard>(enginelock::mutexName(sup, id));
        const int claim = SupervisorLink::claimChannel(*guard, id, data, takeover);
        if (claim != 0) return claim;
        QJsonObject startConfig;
        QByteArray cfgBytes;
        if (!config.isEmpty()) { QFile f(config); if (f.open(QIODevice::ReadOnly)) cfgBytes = f.readAll(); startConfig = QJsonDocument::fromJson(cfgBytes).object(); }
        if (!data.isEmpty()) {
            QDir().mkpath(data);
            QFile f(QDir(data).filePath("fake_engine_starts.txt"));
            if (f.open(QIODevice::Append))
                f.write(QByteArray::number(QCoreApplication::applicationPid()) + " sp=" + QByteArray::number(supPid) + " to=" + (takeover ? "1" : "0") + "\n");
        }
        SupervisorLink::Options o;
        o.ipcName = sup;
        o.channelId = id;
        o.dataDir = data;
        o.supervisorPid = supPid;
        o.orphanGraceSec = SupervisorLink::resolveOrphanGraceSec(startConfig, supPid > 0);
        o.adoptWaitSec = SupervisorLink::resolveAdoptWaitSec(startConfig);
        o.configHash = cfgBytes.isEmpty() ? QString() : QString::fromLatin1(QCryptographicHash::hash(cfgBytes, QCryptographicHash::Sha1).toHex());
        o.readyPayload = QJsonObject{{"fake", true}};
        auto link = std::make_unique<SupervisorLink>(o);
        link->setFactsProvider([]() { return QJsonObject{{"state", "ON_AIR"}, {"itemId", "fake"}}; });
        sendFn = [&link](const IpcProtocol::Message& m) { if (link) link->send(m); };
        QObject::connect(link.get(), &SupervisorLink::messageReceived, &app, onCommand);
        QObject::connect(link.get(), &SupervisorLink::exitRequested, &app, [](int code, const QString&) { QCoreApplication::exit(code); });
        link->start();
        heartbeat.start(100);
        const int rc = app.exec();
        heartbeat.stop();
        link.reset();     // removes engine.lock (while it names us), as engine_main does after shutdown
        return rc;
    }

    if (!data.isEmpty()) {
        QDir().mkpath(data);
        QFile f(QDir(data).filePath("fake_engine_starts.txt"));
        if (f.open(QIODevice::Append))
            f.write(QByteArray::number(QCoreApplication::applicationPid()) + " sp=" + QByteArray::number(supPid) + " to=" + (takeover ? "1" : "0") + "\n");
    }
    IpcTransport ipc(IpcTransport::ClientRole);
    sendFn = [&ipc](const IpcProtocol::Message& m) { ipc.send(m); };
    const bool up = ipc.connectToServer(sup);
    if (!up && !stayMode) return 3;
    if (!stayMode)
        QObject::connect(&ipc, &IpcTransport::connectionStateChanged, &app, [](bool on) { if (!on) QCoreApplication::exit(0); });
    QObject::connect(&ipc, &IpcTransport::messageReceived, &app, [&](const IpcProtocol::Message& m, const QString&) { onCommand(m); });
    heartbeat.start(100);
    if (up) {
        IpcProtocol::Message ready;
        ready.type = IpcProtocol::MSG_ENGINE_READY;
        ready.channelId = id;
        ready.payload["pid"] = double(QCoreApplication::applicationPid());
        ready.payload["fake"] = true;
        ipc.send(ready);
    }
    return app.exec();
}
