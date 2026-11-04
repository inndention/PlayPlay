#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
#include "playout/supervisor/channelmanager.h"
#include "playout/supervisor/failovercontroller.h"
#include "playout/ui_ipc/ipctransport.h"
#include "playout/core/processlaunch.h"
#include "playout/core/enginelock.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// PS-2 (C6 + W14 channelmanager part): one restart per engine death, and kill / restart that never wait on the loop.
// Engines are tests/helpers/fake_engine.exe on a private IPC name ("ppcm-<pid>-<n>"), channel ids perftest-*, data in
// a temporary directory. Nothing here uses the live IPC name, ports, NDI names or data dir. The harness wires the
// manager the way SupervisorCore does (supervisorcore.cpp:101-117, :326-335, :403-407), including FailoverController,
// so the duplicate restart requests of the live Supervisor are all present.
class TestChannelManager : public QObject {
    Q_OBJECT

private slots:
    void decideRestartTable_data();
    void decideRestartTable();
    void heartbeatStopGivesExactlyOneRespawn();
    void crashExitGivesExactlyOneRespawn();
    void killAndRestartDoNotBlockTheLoop();
    void killWaitFallbackAndRetiredClient();
    void stopCancelsPendingRestart();
    void removeThenSpawnWaitsForOldExit();
    void unmanagedEngineNeverRespawned();
    void destructorReapsEnginesBounded();
    void olderEngineExeStillSpawns();
};

namespace {

QString uniqueName() {
    static int n = 0;
    return QString("ppcm-%1-%2").arg(QCoreApplication::applicationPid()).arg(++n);
}

// Worst gap between ticks of a 5 ms timer = how long this thread's event loop was blocked.
struct LagProbe {
    QTimer timer; QElapsedTimer clock; qint64 last = 0, worst = 0;
    LagProbe() {
        timer.setTimerType(Qt::PreciseTimer); timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this]() { const qint64 now = clock.elapsed(); worst = qMax(worst, now - last); last = now; });
        reset(); timer.start();
    }
    void reset() { clock.restart(); last = 0; worst = 0; }
    qint64 lagMs() const { return qMax<qint64>(0, worst - 5); }
};

bool processGone(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return true;
    const bool gone = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
#else
    Q_UNUSED(pid); return true;
#endif
}

// The manager plus the Supervisor's wiring around it, on a private IPC server.
struct Harness : QObject {
    QTemporaryDir tmp;
    QString ipcName = uniqueName();
    IpcTransport server{IpcTransport::ServerRole};
    ChannelManager* cm = new ChannelManager();
    std::unique_ptr<FailoverController> failover;
    int coreRestarts = 0;
    QStringList events;   // "spawn:<id>:<pid>", "exit:<id>:<pid>:<expected>", "died:<id>:<reason>", "connected:<id>"

    explicit Harness(bool liveWiring) {
        if (!server.startServer(ipcName)) qFatal("IPC server failed on %s", qPrintable(ipcName));
        cm->setIpc(&server);
        cm->setSupervisorIpcName(ipcName);                       // never the live default
        cm->setEngineExecutable(QStringLiteral(PP_FAKE_ENGINE)); // never PlayPlayChannelEngine.exe next to the test
        // SupervisorCore::onIpcMessage, the engine part.
        connect(&server, &IpcTransport::messageReceived, this, [this, liveWiring](const IpcProtocol::Message& msg, const QString& clientId) {
            using namespace IpcProtocol;
            if (!cm) return;
            if (msg.type == MSG_ENGINE_READY) cm->bindClient(msg.channelId, clientId);
            else if (msg.type == MSG_HEARTBEAT) {
                if (cm->channelForClient(clientId).isEmpty()) cm->bindClient(msg.channelId, clientId);
                cm->onHeartbeat(msg.channelId, qint64(msg.payload.value("frameNumber").toDouble()), msg.payload.value("isHealthy").toBool(true));
            } else if (msg.type == MSG_ENGINE_FATAL && liveWiring) {
                const QString id = msg.channelId;
                QTimer::singleShot(0, this, [this, id]() { if (cm) cm->restartChannel(id); });
            }
        });
        connect(&server, &IpcTransport::clientDisconnected, this, [this](const QString& c) { if (cm) cm->onClientDisconnected(c); });
        connect(cm, &ChannelManager::channelSpawned, this, [this](const QString& id, int) { events << QString("spawn:%1:%2").arg(id).arg(cm->channel(id) ? cm->channel(id)->pid : 0); });
        connect(cm, &ChannelManager::engineExited, this, [this](const QString& id, qint64 pid, int, bool, bool expected) { events << QString("exit:%1:%2:%3").arg(id).arg(pid).arg(expected ? 1 : 0); });
        connect(cm, &ChannelManager::channelDied, this, [this](const QString& id, const QString& why) { events << QString("died:%1:%2").arg(id, why); });
        connect(cm, &ChannelManager::engineConnected, this, [this](const QString& id) { events << QString("connected:%1").arg(id); });
        if (liveWiring) {
            // supervisorcore.cpp:115-116: a second restart request 1.5 s after every channelDied.
            connect(cm, &ChannelManager::channelDied, this, [this](const QString& id, const QString&) {
                QTimer::singleShot(1500, this, [this, id]() { ++coreRestarts; if (cm) cm->restartChannel(id); });
            });
            // FailoverController: restarts any channel that is not running, on a 500 ms tick.
            failover = std::make_unique<FailoverController>(cm);
            failover->start();
        }
    }
    ~Harness() override { destroyManager(); server.disconnect(); }
    void destroyManager() { failover.reset(); delete cm; cm = nullptr; }

    QString dataDir(const QString& id) const { return tmp.filePath("channels/" + id); }
    bool spawn(const QString& id) {
        QJsonObject cfg; cfg["channelId"] = id; cfg["outputs"] = QJsonArray();
        return cm->spawnChannel(id, id, dataDir(id), cfg);
    }
    int starts(const QString& id) const {
        QFile f(QDir(dataDir(id)).filePath("fake_engine_starts.txt"));
        return f.open(QIODevice::ReadOnly) ? int(f.readAll().count('\n')) : 0;
    }
    int count(const QString& prefix) const { int n = 0; for (const QString& e : events) if (e.startsWith(prefix)) ++n; return n; }
    int indexOf(const QString& prefix, int from = 0) const {
        for (int i = from; i < events.size(); ++i) if (events[i].startsWith(prefix)) return i;
        return -1;
    }
    void send(const QString& id, const QString& action, const QJsonObject& params = {}) {
        cm->sendToChannel(id, IpcProtocol::makeTransportCommand(id, action, params));
    }
    qint64 pid(const QString& id) const { return cm->channel(id) ? cm->channel(id)->pid : 0; }
};

} // namespace

void TestChannelManager::decideRestartTable_data() {
    QTest::addColumn<bool>("known");
    QTest::addColumn<bool>("managed");
    QTest::addColumn<bool>("pending");
    QTest::addColumn<bool>("alive");
    QTest::addColumn<qint64>("sinceRestartSpawnMs");
    QTest::addColumn<bool>("internal");
    QTest::addColumn<int>("coalesceMs");
    QTest::addColumn<int>("verdict");
    using V = chanmgr::RestartVerdict;
    QTest::newRow("unknown channel") << false << false << false << false << qint64(-1) << false << 3000 << int(V::Refuse);
    QTest::newRow("engine we did not spawn") << true << false << false << false << qint64(-1) << false << 3000 << int(V::Refuse);
    QTest::newRow("not spawned by us, heartbeat death") << true << false << false << true << qint64(-1) << true << 3000 << int(V::Refuse);
    QTest::newRow("restart waiting for the old exit") << true << true << true << false << qint64(-1) << false << 3000 << int(V::JoinPending);
    QTest::newRow("pending, heartbeat death") << true << true << true << false << qint64(-1) << true << 3000 << int(V::JoinPending);
    QTest::newRow("C6: SupervisorCore 1.5 s after our respawn") << true << true << false << true << qint64(1500) << false << 3000 << int(V::Coalesce);
    QTest::newRow("Failover just after a respawn") << true << true << false << true << qint64(10) << false << 3000 << int(V::Coalesce);
    QTest::newRow("respawned engine is dead again") << true << true << false << false << qint64(1500) << false << 3000 << int(V::Proceed);
    QTest::newRow("respawn older than the window") << true << true << false << true << qint64(3000) << false << 3000 << int(V::Proceed);
    QTest::newRow("operator restart after a first spawn") << true << true << false << true << qint64(-1) << false << 3000 << int(V::Proceed);
    QTest::newRow("our own heartbeat death of a fresh respawn") << true << true << false << true << qint64(200) << true << 3000 << int(V::Proceed);
    QTest::newRow("coalescing off") << true << true << false << true << qint64(0) << false << 0 << int(V::Proceed);
}

void TestChannelManager::decideRestartTable() {
    QFETCH(bool, known); QFETCH(bool, managed); QFETCH(bool, pending); QFETCH(bool, alive);
    QFETCH(qint64, sinceRestartSpawnMs); QFETCH(bool, internal); QFETCH(int, coalesceMs); QFETCH(int, verdict);
    chanmgr::RestartFacts f;
    f.known = known; f.managed = managed; f.pending = pending; f.alive = alive;
    f.sinceRestartSpawnMs = sinceRestartSpawnMs; f.internal = internal;
    const auto v = chanmgr::decideRestart(f, coalesceMs);
    QVERIFY2(int(v) == verdict, chanmgr::verdictName(v));
}

void TestChannelManager::heartbeatStopGivesExactlyOneRespawn() {
    Harness h(true);
    h.cm->setHeartbeatTimeoutMs(300);
    h.cm->setMaxMissedHeartbeats(3);
    const QString id = "perftest-hb";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 pid1 = h.pid(id);
    QVERIFY(pid1 > 0);

    h.send(id, "fake_hang");   // alive and connected, no heartbeats: a hung engine
    QTRY_COMPARE_WITH_TIMEOUT(h.count("died:"), 1, 5000);
    QCOMPARE(h.events.at(h.indexOf("died:")), QString("died:%1:heartbeat_timeout").arg(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    // SupervisorCore's request (1.5 s after channelDied) and FailoverController's ticks have had their turn.
    QTRY_COMPARE_WITH_TIMEOUT(h.coreRestarts, 1, 5000);
    QTest::qWait(1200);

    qInfo().noquote() << "events:" << h.events.join(" | ");
    QCOMPARE(h.count("spawn:"), 2);          // the first engine and exactly one respawn (C6: was 3)
    QCOMPARE(h.starts(id), 2);               // real process launches, counted by the fake itself
    QCOMPARE(h.count("died:"), 1);
    QCOMPARE(h.cm->channel(id)->restarts, 1);
    const qint64 pid2 = h.pid(id);
    QVERIFY(pid2 > 0 && pid2 != pid1);
    QVERIFY(h.cm->isChannelRunning(id));
    QVERIFY(h.cm->isChannelConnected(id));
    QCOMPARE(h.count(QString("exit:%1:%2:1").arg(id).arg(pid1)), 1);   // the hung engine was killed (expected exit)
    QVERIFY(h.indexOf(QString("exit:%1:%2").arg(id).arg(pid1)) < h.indexOf(QString("spawn:%1:%2").arg(id).arg(pid2)));   // gone before its replacement
    QVERIFY(processGone(pid1));
}

void TestChannelManager::crashExitGivesExactlyOneRespawn() {
    // A crash is restarted by FailoverController (its next 500 ms tick) and again by SupervisorCore 1.5 s after
    // channelDied: the second used to kill the engine the first had started.
    Harness h(true);
    const QString id = "perftest-crash";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 pid1 = h.pid(id);

    QJsonObject p; p["code"] = 3;
    h.send(id, "fake_exit", p);
    QTRY_COMPARE_WITH_TIMEOUT(h.count("died:"), 1, 5000);
    QCOMPARE(h.events.at(h.indexOf("died:")), QString("died:%1:exit_code_3").arg(id));
    QCOMPARE(h.count(QString("exit:%1:%2:0").arg(id).arg(pid1)), 1);   // unexpected exit, from QProcess::finished
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(h.coreRestarts, 1, 5000);
    QTest::qWait(1200);

    qInfo().noquote() << "events:" << h.events.join(" | ");
    QCOMPARE(h.count("spawn:"), 2);
    QCOMPARE(h.starts(id), 2);
    QCOMPARE(h.cm->channel(id)->restarts, 1);
    QVERIFY(h.cm->isChannelConnected(id));
}

void TestChannelManager::killAndRestartDoNotBlockTheLoop() {
    Harness h(false);
    h.cm->setRestartCoalesceMs(0);   // back-to-back operator restarts, each one real
    const QString id = "perftest-lag";
    QElapsedTimer st; st.start();
    QVERIFY(h.spawn(id));
    const qint64 spawnMs = st.elapsed();   // a spawn runs CreateProcess on the loop; the restarts below include one each
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);

    LagProbe probe;
    qint64 worstCall = 0;
    QList<qint64> lags;
    for (int i = 0; i < 5; ++i) {
        probe.reset();
        const qint64 oldPid = h.pid(id);
        const int exitsBefore = h.count("exit:");
        QElapsedTimer t; t.start();
        h.cm->restartChannel(id);
        worstCall = qMax(worstCall, t.elapsed());
        // No wait inside: the old engine's exit can only be reported later, by QProcess::finished on this loop.
        QCOMPARE(h.count("exit:"), exitsBefore);
        QCOMPARE(h.cm->channel(id)->status, QString("restarting"));
        QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2 + i, 10000);
        const int ex = h.indexOf(QString("exit:%1:%2:1").arg(id).arg(oldPid));
        QVERIFY2(ex >= 0, "the killed engine's exit was reported");
        QVERIFY(ex < h.indexOf(QString("spawn:%1:%2").arg(id).arg(h.pid(id))));   // the replacement started after the exit
        QVERIFY(processGone(oldPid));
        lags << probe.lagMs();
    }
    qint64 worstLag = 0;
    for (qint64 l : lags) worstLag = qMax(worstLag, l);
    probe.reset();
    QCOMPARE(h.cm->channel(id)->restarts, 5);
    QCOMPARE(h.starts(id), 6);

    const qint64 pid = h.pid(id);
    const int exitsBefore = h.count("exit:");
    QElapsedTimer t; t.start();
    h.cm->killChannel(id);
    worstCall = qMax(worstCall, t.elapsed());
    QCOMPARE(h.count("exit:"), exitsBefore);
    QVERIFY(!h.cm->isChannelConnected(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count(QString("exit:%1:%2:1").arg(id).arg(pid)), 1, 5000);
    QVERIFY(processGone(pid));
    QTest::qWait(300);
    QCOMPARE(h.count("spawn:"), 6);   // a stopped channel stays stopped here (no Failover in this harness)
    QCOMPARE(h.count("died:"), 0);

    worstLag = qMax(worstLag, probe.lagMs());
    QStringList ls; for (qint64 l : lags) ls << QString::number(l);
    qInfo("first spawn call %lld ms; 5 restarts + 1 kill: worst call %lld ms, loop lag per restart [%s] ms, worst %lld ms",
          spawnMs, worstCall, qPrintable(ls.join(", ")), worstLag);
    QVERIFY2(worstCall < 50, qPrintable(QString("a call took %1 ms").arg(worstCall)));
    QVERIFY2(worstLag < 50, qPrintable(QString("loop lag %1 ms").arg(worstLag)));
}

void TestChannelManager::killWaitFallbackAndRetiredClient() {
    // An engine that does not exit after its kill: the replacement starts after killWaitMs anyway (the old code did the
    // same after 3 + 2 s of blocking), and the old engine, still connected and heartbeating, never gets the channel back.
    Harness h(false);
    QList<QPointer<QProcess>> stuck;
    h.cm->setProcessKiller([&](QProcess* p) { stuck << p; });
    h.cm->setKillWaitMs(400);
    const QString id = "perftest-stuck";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 oldPid = h.pid(id);
    const QString oldClient = h.cm->channel(id)->clientId;

    h.cm->restartChannel(id);
    QCOMPARE(h.cm->channel(id)->status, QString("restarting"));
    QCOMPARE(h.cm->retiringCount(id), 1);
    QCOMPARE(stuck.size(), 1);
    h.cm->restartChannel(id);            // a second request joins the waiting restart
    QCOMPARE(stuck.size(), 1);
    QTest::qWait(150);
    QCOMPARE(h.count("spawn:"), 1);      // still waiting for the old engine
    QTRY_COMPARE_WITH_TIMEOUT(h.count("spawn:"), 2, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    const QString newClient = h.cm->channel(id)->clientId;
    QVERIFY(!newClient.isEmpty() && newClient != oldClient);

    QTest::qWait(800);                   // the old engine keeps sending heartbeats all along
    QCOMPARE(h.cm->channel(id)->clientId, newClient);
    QVERIFY(h.cm->channelForClient(oldClient).isEmpty());
    QCOMPARE(h.count("connected:"), 2);
    QCOMPARE(h.count("died:"), 0);
    QCOMPARE(h.cm->retiringCount(id), 1);

    for (const auto& p : stuck) if (p) p->kill();   // it finally goes
    QTRY_COMPARE_WITH_TIMEOUT(h.count(QString("exit:%1:%2:1").arg(id).arg(oldPid)), 1, 5000);
    QTest::qWait(300);
    QCOMPARE(h.cm->retiringCount(id), 0);
    QCOMPARE(h.count("spawn:"), 2);
    QCOMPARE(h.count("died:"), 0);
    QVERIFY(h.cm->isChannelConnected(id));
    QCOMPARE(h.cm->channel(id)->clientId, newClient);
}

void TestChannelManager::olderEngineExeStillSpawns() {
    // PS-4b review: a Supervisor rebuilt alone next to an engine exe from before PS-3 (which exits 1 on any option it
    // does not know) must still start engines, also the takeover spawn after an unanswered kill. The Supervisor pid and
    // the takeover flag travel in the environment; the fake (--fake-strict) records what it received.
    Harness h(false);
    h.cm->setEngineExtraArgs({"--fake-strict"});
    QList<QPointer<QProcess>> stuck;
    h.cm->setProcessKiller([&](QProcess* p) { stuck << p; });
    h.cm->setKillWaitMs(300);
    const QString id = "perftest-strict";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 oldPid = h.pid(id);
    h.cm->restartChannel(id);            // the old engine ignores its kill: the replacement starts with the takeover flag
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    QCOMPARE(h.count("died:"), 0);
    QFile f(QDir(h.dataDir(id)).filePath("fake_engine_starts.txt"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QList<QByteArray> lines = f.readAll().trimmed().split('\n');
    QCOMPARE(lines.size(), 2);
    const QByteArray sp = "sp=" + QByteArray::number(QCoreApplication::applicationPid());
    QVERIFY2(lines[0].contains(sp) && lines[0].endsWith("to=0"), lines[0].constData());
    QVERIFY2(lines[1].contains(sp) && lines[1].endsWith("to=1"), lines[1].constData());
    QVERIFY(qgetenv(enginelock::kEnvTakeover).isEmpty());   // never leaks into the Supervisor's own environment
    for (const auto& p : stuck) if (p) p->kill();
    QTRY_COMPARE_WITH_TIMEOUT(h.count(QString("exit:%1:%2:1").arg(id).arg(oldPid)), 1, 5000);
}

void TestChannelManager::stopCancelsPendingRestart() {
    Harness h(false);
    QList<QPointer<QProcess>> stuck;
    h.cm->setProcessKiller([&](QProcess* p) { stuck << p; });
    h.cm->setKillWaitMs(400);
    const QString id = "perftest-stop";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 oldPid = h.pid(id);

    h.cm->restartChannel(id);
    QVERIFY(h.cm->channel(id)->spawnPending);
    QVERIFY(h.cm->stopChannel(id));
    QVERIFY(!h.cm->channel(id)->spawnPending);
    QTest::qWait(900);                   // past the kill wait
    QCOMPARE(h.count("spawn:"), 1);
    QCOMPARE(h.cm->channel(id)->status, QString("stopped"));

    for (const auto& p : stuck) if (p) p->kill();
    QTRY_COMPARE_WITH_TIMEOUT(h.count(QString("exit:%1:%2:1").arg(id).arg(oldPid)), 1, 5000);
    QTest::qWait(300);
    QCOMPARE(h.count("spawn:"), 1);
    QCOMPARE(h.count("died:"), 0);
    QCOMPARE(h.starts(id), 1);
}

void TestChannelManager::removeThenSpawnWaitsForOldExit() {
    // The config-change path (supervisorcore.cpp:651: removeChannel + spawnChannel): the new engine starts when the old
    // one has exited, not next to it, and not after the whole kill wait.
    Harness h(false);
    QList<QPointer<QProcess>> stuck;
    h.cm->setProcessKiller([&](QProcess* p) { stuck << p; });
    h.cm->setKillWaitMs(5000);
    const QString id = "perftest-cfg";
    QVERIFY(h.spawn(id));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    const qint64 oldPid = h.pid(id);

    QElapsedTimer t; t.start();
    h.cm->removeChannel(id);
    QVERIFY(!h.cm->channel(id));
    QVERIFY(h.spawn(id));                // accepted, deferred
    QCOMPARE(h.count("spawn:"), 1);
    QVERIFY(h.cm->channel(id)->spawnPending);
    QVERIFY(h.cm->startChannel(id));     // joins, no second engine
    QTest::qWait(300);
    QCOMPARE(h.count("spawn:"), 1);
    for (const auto& p : stuck) if (p) p->kill();
    QTRY_COMPARE_WITH_TIMEOUT(h.count("spawn:"), 2, 3000);
    QVERIFY2(t.elapsed() < 4000, "released by the exit, not by the 5 s kill wait");
    QVERIFY(h.indexOf(QString("exit:%1:%2:1").arg(id).arg(oldPid)) < h.indexOf(QString("spawn:%1:%2").arg(id).arg(h.pid(id))));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    QCOMPARE(h.starts(id), 2);
    QCOMPARE(h.cm->channel(id)->restarts, 0);
}

void TestChannelManager::unmanagedEngineNeverRespawned() {
    // An engine this manager did not spawn (a standalone run attached over IPC) has no data dir or config to respawn
    // from. FailoverController asks for a restart at once (it is "not running" for us) and the heartbeat watchdog
    // declares it dead when it hangs: neither may start a second engine for that channel.
    Harness h(true);
    h.cm->setHeartbeatTimeoutMs(300);
    const QString id = "perftest-um";
    QProcess own;
    processlaunch::configureDetachedChild(own);
    own.start(QStringLiteral(PP_FAKE_ENGINE), {"--id", id, "--supervisor", h.ipcName, "--data", h.dataDir(id)});
    QVERIFY(own.waitForStarted(5000));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 1, 10000);
    QVERIFY(h.cm->channel(id) && h.cm->channel(id)->dataDir.isEmpty());
    QTest::qWait(1200);                  // two Failover ticks
    QCOMPARE(h.count("spawn:"), 0);
    QVERIFY(h.cm->isChannelConnected(id));

    h.send(id, "fake_hang");
    QTRY_COMPARE_WITH_TIMEOUT(h.count("died:"), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(h.coreRestarts, 1, 5000);
    QTest::qWait(600);
    QCOMPARE(h.count("died:"), 1);       // not declared dead again every window
    QCOMPARE(h.count("spawn:"), 0);
    QCOMPARE(h.starts(id), 1);
    QCOMPARE(h.cm->channel(id)->status, QString("error"));
    own.kill();
    own.waitForFinished(3000);           // the test's own child, not the manager's
}

void TestChannelManager::destructorReapsEnginesBounded() {
    Harness h(false);
    QVERIFY(h.spawn("perftest-d1"));
    QVERIFY(h.spawn("perftest-d2"));
    QTRY_COMPARE_WITH_TIMEOUT(h.count("connected:"), 2, 10000);
    const qint64 p1 = h.pid("perftest-d1"), p2 = h.pid("perftest-d2");
    QElapsedTimer t; t.start();
    h.destroyManager();                  // Supervisor shutdown
    const qint64 ms = t.elapsed();
    qInfo("destructor with 2 engines: %lld ms", ms);
    QVERIFY(ms < ChannelManager::kShutdownReapMs + 500);
    QVERIFY(processGone(p1));
    QVERIFY(processGone(p2));
}

QTEST_GUILESS_MAIN(TestChannelManager)
#include "test_channelmanager.moc"
