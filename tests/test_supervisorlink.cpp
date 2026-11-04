#include <QtTest/QtTest>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include "playout/core/enginelock.h"
#include "playout/engine/supervisorlink.h"
#include "playout/ui_ipc/ipctransport.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifndef PP_FAKE_SUPERVISOR
#  define PP_FAKE_SUPERVISOR ""
#endif

// PS-3 (W5): the engine's Supervisor link. In-process cases run a SupervisorLink against an IpcTransport server on a
// private name ("ppsl-<pid>-<n>") or against tests/helpers/fake_supervisor.exe, which also serves as the parent process
// that dies. Process cases run this test binary itself in "--engine-child" mode: the same claimChannel + SupervisorLink
// wiring as engine_main.cpp, without video or outputs (isolated real engines are only allowed --standalone). Channel ids
// are perftest-*; nothing here uses the live IPC name, ports or NDI names.
class TestSupervisorLink : public QObject {
    Q_OBJECT

private slots:
    void protocolHelloAndSupAlive();
    void optionRules_data();
    void optionRules();
    void mutexNameScoped();
    void instanceGuardExclusion();
    void lockFileRoundTrip();
    void helloBeforeReadyOnEveryReconnect();
    void absentSupervisorNeverBlocks();
    void supervisorFatalAndAliveMessages();
    void orphanAdoptedThenOrphanAgain();
    void orphanYieldsToNonAdoptingSupervisor();
    void engineProcessOrphanExit_data();
    void engineProcessOrphanExit();
    void duplicateEngineExitsWithCode4();
};

namespace {

QString uniqueName() {
    static int n = 0;
    return QString("ppsl-%1-%2").arg(QCoreApplication::applicationPid()).arg(++n);
}

// Worst gap between ticks of a 5 ms timer = how long this thread's event loop was blocked.
struct LagProbe {
    QTimer timer; QElapsedTimer clock; qint64 last = 0, worst = 0;
    LagProbe() {
        timer.setTimerType(Qt::PreciseTimer); timer.setInterval(5);
        QObject::connect(&timer, &QTimer::timeout, [this]() { const qint64 now = clock.elapsed(); worst = qMax(worst, now - last); last = now; });
        clock.start(); timer.start();
    }
    qint64 lagMs() const { return qMax<qint64>(0, worst - 5); }
};

// A child process (fake_supervisor or this binary in engine-child mode) whose stdout lines the test waits for.
struct Proc {
    QProcess p;
    QStringList lines;
    QByteArray outBuf, err;
    void start(const QString& program, const QStringList& args) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("QT_FORCE_STDERR_LOGGING", "1");
        p.setProcessEnvironment(env);
        QObject::connect(&p, &QProcess::readyReadStandardOutput, [this]() {
            outBuf += p.readAllStandardOutput();
            int nl;
            while ((nl = outBuf.indexOf('\n')) >= 0) {
                lines << QString::fromUtf8(outBuf.left(nl)).trimmed();
                outBuf.remove(0, nl + 1);
            }
        });
        QObject::connect(&p, &QProcess::readyReadStandardError, [this]() { err += p.readAllStandardError(); });
        p.start(program, args);
    }
    int find(const QString& prefix, int from = 0) const {
        for (int i = from; i < lines.size(); ++i) if (lines[i].startsWith(prefix)) return i;
        return -1;
    }
    // Index of the first line starting with prefix at or after `from`, waiting up to ms; -1 on timeout.
    int waitFor(const QString& prefix, int ms, int from = 0) {
        QElapsedTimer t; t.start();
        for (;;) {
            const int i = find(prefix, from);
            if (i >= 0 || t.elapsed() > ms) return i;
            QTest::qWait(10);
        }
    }
    qint64 pid() const { return p.processId(); }
    ~Proc() {
        if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(3000); }
    }
};

QString fakeSupervisor() { return QStringLiteral(PP_FAKE_SUPERVISOR); }

// fake_supervisor started and listening; its pid (0 on failure).
qint64 startFakeSupervisor(Proc& s, const QStringList& args) {
    s.start(fakeSupervisor(), args);
    const int i = s.waitFor("LISTENING ", 5000);
    return i < 0 ? 0 : s.lines[i].mid(10).toLongLong();
}

// The payload of a "MSG <type> <channel> <pid> <json>" line of fake_supervisor.
QJsonObject msgPayload(const QString& line) {
    const int brace = line.indexOf('{');
    return brace < 0 ? QJsonObject() : QJsonDocument::fromJson(line.mid(brace).toUtf8()).object();
}

QJsonObject facts() {
    return QJsonObject{{"state", "ON_AIR"}, {"itemId", "perftest-item"}, {"frame", 123.0}, {"outputs", QJsonArray{"perftest-ndi", "perftest-udp"}}};
}

SupervisorLink::Options linkOptions(const QString& ipc, const QString& channel, const QString& dataDir) {
    SupervisorLink::Options o;
    o.ipcName = ipc;
    o.channelId = channel;
    o.dataDir = dataDir;
    o.configHash = "perftest-hash";
    o.readyPayload = QJsonObject{{"frameRate", "30000/1001"}, {"width", 1920}, {"height", 1080}};
    return o;
}

} // namespace

// --- engine-child mode -------------------------------------------------------------------------------------------
// test_supervisorlink --engine-child --ipc <name> --id <channel> --data <dir> [--supervisor-pid P] [--grace S]
//                     [--adopt-wait S] [--takeover] [--claim-wait-ms N] [--quit-file F] [--max-life-ms N]
// The engine_main.cpp lifecycle around the link: claimChannel (exit 4), SupervisorLink (engine.lock, HELLO + READY,
// parent watch, orphan exit 5), heartbeats at 10 Hz, clean exit removes the lock. Events go to stdout, one per line.
static void say(const QString& s) {
    const QByteArray b = s.toUtf8();
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

static int engineChild(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString ipc, id, data, quitFile;
    qint64 supPid = 0;
    int grace = INT_MIN, adoptWait = INT_MIN, claimWaitMs = 5000, maxLifeMs = 30000;
    bool takeover = false;
    const QStringList a = app.arguments();
    for (int i = 1; i < a.size(); ++i) {
        const auto next = [&]() { return i + 1 < a.size() ? a[++i] : QString(); };
        if (a[i] == "--ipc") ipc = next();
        else if (a[i] == "--id") id = next();
        else if (a[i] == "--data") data = next();
        else if (a[i] == "--supervisor-pid") supPid = next().toLongLong();
        else if (a[i] == "--grace") grace = next().toInt();
        else if (a[i] == "--adopt-wait") adoptWait = next().toInt();
        else if (a[i] == "--claim-wait-ms") claimWaitMs = next().toInt();
        else if (a[i] == "--quit-file") quitFile = next();
        else if (a[i] == "--max-life-ms") maxLifeMs = next().toInt();
        else if (a[i] == "--takeover") takeover = true;
    }
    if (ipc.isEmpty() || id.isEmpty() || ipc.compare("PlayPlaySupervisor", Qt::CaseInsensitive) == 0) return 2;

    enginelock::InstanceGuard guard(enginelock::mutexName(ipc, id));
    const int claim = SupervisorLink::claimChannel(guard, id, data, takeover, claimWaitMs);
    if (claim != 0) { say(QString("EXIT %1").arg(claim)); return claim; }
    say(QString("CLAIMED owned=%1 abandoned=%2").arg(int(guard.owned())).arg(int(guard.abandoned())));

    SupervisorLink::Options o = linkOptions(ipc, id, data);
    o.supervisorPid = supPid;
    o.orphanGraceSec = grace != INT_MIN ? grace : SupervisorLink::resolveOrphanGraceSec({}, supPid > 0);
    o.adoptWaitSec = adoptWait != INT_MIN ? adoptWait : SupervisorLink::resolveAdoptWaitSec({});
    SupervisorLink link(o);
    link.setFactsProvider(&facts);
    bool connected = false;
    QObject::connect(&link, &SupervisorLink::connectedChanged, &app, [&](bool up) {
        connected = up;
        say(up ? QString("CONNECTED %1").arg(link.connectedServerPid()) : QStringLiteral("DISCONNECTED"));
    });
    QObject::connect(&link, &SupervisorLink::orphanChanged, &app, [](bool o) { say(QString("ORPHAN %1").arg(int(o))); });
    QObject::connect(&link, &SupervisorLink::adopted, &app, [](qint64 pid) { say(QString("ADOPTED %1").arg(pid)); });
    QObject::connect(&link, &SupervisorLink::exitRequested, &app, [](int code, const QString&) {
        say(QString("EXITREQ %1").arg(code));
        QCoreApplication::exit(code);
    });
    QTimer heartbeat;
    qint64 frame = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&]() { if (connected) link.send(IpcProtocol::makeHeartbeat(id, frame += 3, 4, true)); });
    heartbeat.start(100);
    QTimer poll;   // guard ownership taken in the background (--takeover), and the quit file
    bool ownedSaid = guard.owned();
    QObject::connect(&poll, &QTimer::timeout, &app, [&]() {
        if (!ownedSaid && guard.owned()) { ownedSaid = true; say(QString("OWNED abandoned=%1").arg(int(guard.abandoned()))); }
        if (!quitFile.isEmpty() && QFile::exists(quitFile)) QCoreApplication::exit(0);
    });
    poll.start(50);
    if (maxLifeMs > 0) QTimer::singleShot(maxLifeMs, &app, []() { QCoreApplication::exit(7); });
    link.start();
    const int rc = app.exec();
    say(QString("EXIT %1").arg(rc));
    return rc;   // link (lock removed), then guard (mutex released) go here
}

// --- tests -------------------------------------------------------------------------------------------------------

void TestSupervisorLink::protocolHelloAndSupAlive() {
    using namespace IpcProtocol;
    // Appended at the end: the numbers of every older type stay the wire format.
    QCOMPARE(int(MSG_UI_RUNLIST), 21);
    QCOMPARE(int(MSG_ENGINE_HELLO), 22);
    QCOMPARE(int(MSG_SUP_ALIVE), 23);
    const Message h = Message::fromJson(makeEngineHello("perftest-a", QJsonObject{{"pid", 42.0}, {"state", "ON_AIR"}}).toJson());
    QCOMPARE(h.type, MSG_ENGINE_HELLO);
    QCOMPARE(h.channelId, QString("perftest-a"));
    QCOMPARE(h.payload.value("channelId").toString(), QString("perftest-a"));
    QCOMPARE(h.payload.value("pid").toInt(), 42);
    QCOMPARE(h.payload.value("state").toString(), QString("ON_AIR"));
    const Message a = Message::fromJson(makeSupAlive(7, 1.5, 99, "perftest-a").toJson());
    QCOMPARE(a.type, MSG_SUP_ALIVE);
    QCOMPARE(a.channelId, QString("perftest-a"));
    QCOMPARE(a.payload.value("seq").toInt(), 7);
    QCOMPARE(a.payload.value("loopLagMs").toDouble(), 1.5);
    QCOMPARE(a.payload.value("pid").toInt(), 99);
    QVERIFY(Message::fromJson(makeSupAlive(8, 0, 99).toJson()).channelId.isEmpty());   // liveness broadcast
    QCOMPARE(IpcTransport::defaultPriority(MSG_SUP_ALIVE), IpcTransport::Droppable);
    QCOMPARE(IpcTransport::defaultPriority(MSG_ENGINE_HELLO), IpcTransport::Normal);
}

void TestSupervisorLink::optionRules_data() {
    QTest::addColumn<QJsonObject>("cfg");
    QTest::addColumn<bool>("explicitParent");
    QTest::addColumn<int>("grace");
    QTest::addColumn<int>("adoptWait");
    QTest::newRow("explicit parent: design default 300") << QJsonObject{} << true << 300 << 20;
    QTest::newRow("parent from the pipe: play on (today)") << QJsonObject{} << false << -1 << 20;
    QTest::newRow("0 = exit at once") << QJsonObject{{"orphanGraceSec", 0}} << true << 0 << 20;
    QTest::newRow("0 also without explicit parent") << QJsonObject{{"orphanGraceSec", 0}} << false << 0 << 20;
    QTest::newRow("negative = never") << QJsonObject{{"orphanGraceSec", -5}} << true << -1 << 20;
    QTest::newRow("string number") << QJsonObject{{"orphanGraceSec", "45"}} << false << 45 << 20;
    QTest::newRow("garbage = default") << QJsonObject{{"orphanGraceSec", "soon"}} << true << 300 << 20;
    QTest::newRow("adopt wait off") << QJsonObject{{"orphanAdoptWaitSec", 0}} << true << 300 << 0;
    QTest::newRow("adopt wait negative = off") << QJsonObject{{"orphanAdoptWaitSec", -3}} << true << 300 << 0;
    QTest::newRow("adopt wait capped") << QJsonObject{{"orphanAdoptWaitSec", 99999}} << true << 300 << 3600;
}

void TestSupervisorLink::optionRules() {
    QFETCH(QJsonObject, cfg);
    QFETCH(bool, explicitParent);
    QFETCH(int, grace);
    QFETCH(int, adoptWait);
    QCOMPARE(SupervisorLink::resolveOrphanGraceSec(cfg, explicitParent), grace);
    QCOMPARE(SupervisorLink::resolveAdoptWaitSec(cfg), adoptWait);
}

void TestSupervisorLink::mutexNameScoped() {
    QCOMPARE(enginelock::mutexName("PlayPlayLaneTest", "perftest-a"), QString("Local\\PlayPlayEngine-PlayPlayLaneTest-perftest-a"));
    QCOMPARE(enginelock::mutexName("\\\\.\\pipe\\ppsl-x", "perftest-a"), QString("Local\\PlayPlayEngine-ppsl-x-perftest-a"));
    QCOMPARE(enginelock::mutexName("odd\\name", "perftest-a"), QString("Local\\PlayPlayEngine-odd_name-perftest-a"));
    // Scope: another Supervisor (IPC name) or another channel never shares a guard.
    QVERIFY(enginelock::mutexName("ppsl-a", "perftest-a") != enginelock::mutexName("ppsl-b", "perftest-a"));
    QVERIFY(enginelock::mutexName("ppsl-a", "perftest-a") != enginelock::mutexName("ppsl-a", "perftest-b"));
    QVERIFY(enginelock::mutexName(QString(300, 'x'), "perftest-a").size() <= 260);
}

void TestSupervisorLink::instanceGuardExclusion() {
#ifdef Q_OS_WIN
    const QString name = enginelock::mutexName(uniqueName(), "perftest-guard");
    // A Windows mutex is recursive for its owning thread, so the competing owners below are other threads.
    auto tryOnThread = [&](int waitMs) {
        bool got = false;
        std::thread t([&]() { enginelock::InstanceGuard g(name); got = g.acquire(waitMs); });
        t.join();
        return got;
    };
    {
        enginelock::InstanceGuard a(name);
        QVERIFY(a.acquire(0));
        QVERIFY(a.owned());
        QVERIFY(!a.abandoned());
        QElapsedTimer w; w.start();
        QVERIFY(!tryOnThread(300));                          // held: the second waits, then gives up
        QVERIFY(w.elapsed() >= 250);
    }
    QVERIFY(tryOnThread(0));                                 // released by the destructor

    // An owner that dies without releasing (a killed engine): the next one gets it, flagged abandoned. The mutex object
    // must outlive its owner for that (a waiting engine's handle does it in practice; with no handle left the object
    // is gone and the next guard simply creates a new one), so `keep` holds a reference across the owner's death.
    HANDLE keep = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
    QVERIFY(keep);
    std::thread dier([&]() {
        HANDLE h = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);                                      // thread ends still owning it
    });
    dier.join();
    {
        enginelock::InstanceGuard b(name);
        QVERIFY(b.acquire(0));
        QVERIFY(b.abandoned());
    }
    CloseHandle(keep);

    // --takeover: the guard is taken in the background as soon as the old owner lets go, and then refuses others.
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> holding{false};
    std::thread old([&]() {
        HANDLE h = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
        WaitForSingleObject(h, INFINITE);
        holding = true;
        WaitForSingleObject(release, INFINITE);
        ReleaseMutex(h);
        CloseHandle(h);
    });
    QTRY_VERIFY(holding.load());
    {
        enginelock::InstanceGuard c(name);
        QVERIFY(!c.acquire(100));
        c.acquireInBackground();
        QTest::qWait(100);
        QVERIFY(!c.owned());
        SetEvent(release);
        QTRY_VERIFY_WITH_TIMEOUT(c.owned(), 2000);
        QVERIFY(!tryOnThread(100));                          // the background thread holds it
    }
    old.join();
    CloseHandle(release);
    QVERIFY(tryOnThread(0));                                 // and released it with the guard
#else
    QSKIP("Windows named mutex");
#endif
}

void TestSupervisorLink::lockFileRoundTrip() {
    QTemporaryDir tmp;
    const qint64 pid = QCoreApplication::applicationPid();
    enginelock::LockInfo me = enginelock::currentProcessInfo();
    QCOMPARE(me.pid, pid);
    QVERIFY(me.createTime != 0);
    QCOMPARE(me.createTime, enginelock::processCreateTime(pid));   // the PID-reuse guard PS-4a compares
    QCOMPARE(me.exe.toLower(), enginelock::processImagePath(pid).toLower());
    QVERIFY(me.exeMtimeMs > 0);
    const qint64 startedMs = enginelock::createTimeToMs(me.createTime);
    QVERIFY(startedMs <= QDateTime::currentMSecsSinceEpoch() + 1000);
    QVERIFY(QDateTime::currentMSecsSinceEpoch() - startedMs < 3600 * 1000);

    me.channelId = "perftest-lock";
    me.configHash = "abc";
    me.supervisorIpc = "ppsl-lock";
    me.supervisorPid = 4242;
    me.dataDir = tmp.path();
    const QString path = enginelock::lockPath(tmp.path() + "/channels/perftest-lock");   // the directory is created
    QString err;
    QVERIFY2(enginelock::writeLock(path, me, &err), qPrintable(err));
    enginelock::LockInfo back;
    QVERIFY(enginelock::readLock(path, &back, &err));
    QCOMPARE(back.createTime, me.createTime);                // exact, not through a double
    QCOMPARE(back.toJson(), me.toJson());
    QVERIFY(!QFile::exists(path + ".tmp"));
    QVERIFY(!enginelock::removeLockIfOwned(path, pid + 1));  // a successor's lock is left alone
    QVERIFY(QFile::exists(path));
    QVERIFY(enginelock::removeLockIfOwned(path, pid));
    QVERIFY(!QFile::exists(path));
    QVERIFY(enginelock::processAlive(pid));
    QVERIFY(!enginelock::processAlive(0));
}

// The fake QLocalServer drops and re-listens: the link reconnects within its 1 s cadence and starts the new connection
// with HELLO then READY again. The loop never stalls.
void TestSupervisorLink::helloBeforeReadyOnEveryReconnect() {
    QTemporaryDir tmp;
    const QString name = uniqueName();
    const qint64 myPid = QCoreApplication::applicationPid();
    auto server = std::make_unique<IpcTransport>(IpcTransport::ServerRole);
    QVERIFY(server->startServer(name));
    QList<IpcProtocol::Message> got;
    auto wire = [&](IpcTransport* s) {
        connect(s, &IpcTransport::messageReceived, this, [&](const IpcProtocol::Message& m, const QString&) { got << m; });
    };
    wire(server.get());

    LagProbe lag;
    SupervisorLink link(linkOptions(name, "perftest-link", tmp.path()));
    link.setFactsProvider(&facts);
    QSignalSpy conn(&link, &SupervisorLink::connectedChanged);
    QSignalSpy exits(&link, &SupervisorLink::exitRequested);
    QElapsedTimer call; call.start();
    link.start();
    QVERIFY2(call.elapsed() < 50, "start() returns at once");
    QTRY_VERIFY_WITH_TIMEOUT(got.size() >= 2, 3000);
    QCOMPARE(got[0].type, IpcProtocol::MSG_ENGINE_HELLO);
    QCOMPARE(got[1].type, IpcProtocol::MSG_ENGINE_READY);
    const QJsonObject h = got[0].payload;
    QCOMPARE(got[0].channelId, QString("perftest-link"));
    QCOMPARE(h.value("channelId").toString(), QString("perftest-link"));
    QCOMPARE(qint64(h.value("pid").toDouble()), myPid);
    QCOMPARE(h.value("createTime").toString(), QString::number(enginelock::processCreateTime(myPid)));
    QCOMPARE(h.value("exe").toString().toLower(), enginelock::processImagePath(myPid).toLower());
    QVERIFY(h.value("exeMtime").toDouble() > 0);
    QCOMPARE(h.value("configHash").toString(), QString("perftest-hash"));
    QCOMPARE(h.value("supervisorIpc").toString(), name);
    QCOMPARE(qint64(h.value("supervisorPid").toDouble()), myPid);   // learned from the pipe: the server is this process
    QCOMPARE(h.value("orphan").toBool(true), false);
    QCOMPARE(h.value("state").toString(), QString("ON_AIR"));
    QCOMPARE(h.value("itemId").toString(), QString("perftest-item"));
    QCOMPARE(h.value("outputs").toArray().size(), 2);
    QCOMPARE(h.value("connects").toInt(), 1);
    QCOMPARE(got[1].payload.value("frameRate").toString(), QString("30000/1001"));
    QCOMPARE(qint64(got[1].payload.value("pid").toDouble()), myPid);
    QVERIFY(link.isConnected());
    QCOMPARE(conn.count(), 1);
    QCOMPARE(conn.last().at(0).toBool(), true);
    QCOMPARE(link.supervisorPid(), myPid);
    enginelock::LockInfo lock;
    QVERIFY(enginelock::readLock(enginelock::lockPath(tmp.path()), &lock));
    QCOMPARE(lock.pid, myPid);
    QCOMPARE(lock.supervisorPid, myPid);                     // rewritten once the parent was learned
    QCOMPARE(lock.supervisorIpc, name);
    QVERIFY(link.send(IpcProtocol::makeHeartbeat("perftest-link", 1, 1, true)));
    QTRY_COMPARE(got.last().type, IpcProtocol::MSG_HEARTBEAT);

    // Drop: the server goes away with its client.
    const quint64 attempts0 = quint64(link.stats().value("attempts").toDouble());
    server.reset();
    QTRY_VERIFY_WITH_TIMEOUT(!link.isConnected(), 2000);
    QCOMPARE(conn.last().at(0).toBool(), false);
    QTest::qWait(2600);                                      // nobody listening: attempts fail ~once a second
    const quint64 tries = quint64(link.stats().value("attempts").toDouble()) - attempts0;
    qInfo().noquote() << QString("attempts while the server was away for 2.6 s: %1").arg(tries);
    QVERIFY2(tries >= 2 && tries <= 4, qPrintable(QString("attempts %1").arg(tries)));
    QVERIFY(!link.isConnected());

    // Re-listen: reconnect within the 1 s cadence (+ slack), HELLO first again.
    got.clear();
    server = std::make_unique<IpcTransport>(IpcTransport::ServerRole);
    wire(server.get());
    QVERIFY(server->startServer(name));
    QElapsedTimer back; back.start();
    QTRY_VERIFY_WITH_TIMEOUT(link.isConnected(), 3000);
    const qint64 reconnectMs = back.elapsed();
    QTRY_VERIFY_WITH_TIMEOUT(got.size() >= 2, 2000);
    qInfo().noquote() << QString("reconnected %1 ms after the server listened again").arg(reconnectMs);
    QVERIFY2(reconnectMs <= 1500, qPrintable(QString("reconnect took %1 ms").arg(reconnectMs)));
    QCOMPARE(got[0].type, IpcProtocol::MSG_ENGINE_HELLO);
    QCOMPARE(got[1].type, IpcProtocol::MSG_ENGINE_READY);
    QCOMPARE(got[0].payload.value("connects").toInt(), 2);
    QCOMPARE(int(link.stats().value("hellos").toDouble()), 2);
    QCOMPARE(exits.count(), 0);
    qInfo().noquote() << QString("worst loop lag through connect / drop / retries / reconnect: %1 ms").arg(lag.lagMs());
    QVERIFY2(lag.lagMs() < 50, qPrintable(QString("loop lag %1 ms").arg(lag.lagMs())));
}

void TestSupervisorLink::absentSupervisorNeverBlocks() {
    QTemporaryDir tmp;
    LagProbe lag;
    SupervisorLink link(linkOptions(uniqueName(), "perftest-absent", tmp.path()));
    QSignalSpy exits(&link, &SupervisorLink::exitRequested);
    QElapsedTimer call; call.start();
    link.start();
    QVERIFY(call.elapsed() < 50);
    QTest::qWait(3200);
    const int attempts = link.stats().value("attempts").toInt();
    qInfo().noquote() << QString("absent Supervisor: %1 attempts in 3.2 s, worst loop lag %2 ms").arg(attempts).arg(lag.lagMs());
    QVERIFY2(attempts >= 3 && attempts <= 5, qPrintable(QString("attempts %1").arg(attempts)));
    QVERIFY(lag.lagMs() < 50);
    QVERIFY(!link.isConnected());
    QCOMPARE(exits.count(), 0);                              // no parent known: never an orphan
    QVERIFY(!link.isOrphan());
    enginelock::LockInfo lock;
    QVERIFY(enginelock::readLock(enginelock::lockPath(tmp.path()), &lock));
    QCOMPARE(lock.supervisorPid, qint64(0));
}

void TestSupervisorLink::supervisorFatalAndAliveMessages() {
    QTemporaryDir tmp;
    const QString name = uniqueName();
    IpcTransport server(IpcTransport::ServerRole);
    QVERIFY(server.startServer(name));
    QString client;
    connect(&server, &IpcTransport::clientConnected, this, [&](const QString& id) { client = id; });
    SupervisorLink link(linkOptions(name, "perftest-fatal", tmp.path()));
    QSignalSpy exits(&link, &SupervisorLink::exitRequested), adopted(&link, &SupervisorLink::adopted), msgs(&link, &SupervisorLink::messageReceived);
    link.start();
    QTRY_VERIFY(link.isConnected() && !client.isEmpty());
    const qint64 myPid = QCoreApplication::applicationPid();
    auto fatal = [&](const QString& ch, const QString& reason, qint64 pid) {
        IpcProtocol::Message f = IpcProtocol::make(IpcProtocol::MSG_ENGINE_FATAL, ch, QJsonObject{{"reason", reason}});
        if (pid) f.payload["pid"] = double(pid);
        server.sendTo(client, f);
    };
    fatal("perftest-other", "duplicate", 0);                 // another channel
    fatal("perftest-fatal", "duplicate", myPid + 1);         // another engine of this channel
    fatal("perftest-fatal", "drift", 0);                     // not the binding guard
    server.sendTo(client, IpcProtocol::makeSupAlive(1, 0, myPid));                    // liveness broadcast
    server.sendTo(client, IpcProtocol::makeSupAlive(2, 0, myPid, "perftest-fatal"));  // the parent we already follow
    server.sendTo(client, IpcProtocol::makeTransportCommand("perftest-fatal", "play", {}));
    QTRY_COMPARE(msgs.count(), 1);                            // only the command reaches the engine
    QCOMPARE(exits.count(), 0);
    QCOMPARE(adopted.count(), 0);
    fatal("perftest-fatal", "duplicate", myPid);
    QTRY_COMPARE(exits.count(), 1);
    QCOMPARE(exits.at(0).at(0).toInt(), enginelock::kExitDuplicate);
}

// The parent (a fake_supervisor process) dies: orphan mode. A new Supervisor that adopts (MSG_SUP_ALIVE for the channel)
// ends it and becomes the watched parent; when that one dies too, the grace runs out and the engine asks to exit.
void TestSupervisorLink::orphanAdoptedThenOrphanAgain() {
    if (fakeSupervisor().isEmpty()) QSKIP("fake_supervisor not built");
    QTemporaryDir tmp;
    const QString name = uniqueName();
    Proc a;
    const qint64 pidA = startFakeSupervisor(a, {"--name", name, "--exit-after-ms", "2500"});
    QVERIFY(pidA > 0);
    SupervisorLink::Options o = linkOptions(name, "perftest-adopt", tmp.path());
    o.supervisorPid = pidA;
    o.orphanGraceSec = 4;
    o.adoptWaitSec = 10;
    SupervisorLink link(o);
    QSignalSpy orphan(&link, &SupervisorLink::orphanChanged), adopted(&link, &SupervisorLink::adopted), exits(&link, &SupervisorLink::exitRequested);
    link.start();
    QTRY_VERIFY_WITH_TIMEOUT(link.isConnected(), 3000);
    const int helloA = a.waitFor("MSG 22 perftest-adopt", 3000);
    QVERIFY(helloA >= 0);
    QCOMPARE(qint64(msgPayload(a.lines[helloA]).value("supervisorPid").toDouble()), pidA);
    QVERIFY(a.waitFor("MSG 8 perftest-adopt", 2000, helloA + 1) > helloA);     // READY after HELLO

    QTRY_VERIFY_WITH_TIMEOUT(orphan.count() == 1, 5000);   // A dies at 2.5 s
    QElapsedTimer sinceOrphan; sinceOrphan.start();
    QCOMPARE(orphan.at(0).at(0).toBool(), true);
    QVERIFY(link.isOrphan());
    QTRY_VERIFY_WITH_TIMEOUT(!link.isConnected(), 2000);

    Proc b;
    const qint64 pidB = startFakeSupervisor(b, {"--name", name, "--adopt", "--exit-after-ms", "5500"});
    QVERIFY(pidB > 0);
    QTRY_VERIFY_WITH_TIMEOUT(adopted.count() == 1, 3000);
    qInfo().noquote() << QString("adopted by the new Supervisor %1 ms after orphan mode began").arg(sinceOrphan.elapsed());
    QCOMPARE(adopted.at(0).at(0).toLongLong(), pidB);
    QCOMPARE(orphan.count(), 2);
    QCOMPARE(orphan.at(1).at(0).toBool(), false);
    QVERIFY(!link.isOrphan());
    QCOMPARE(link.supervisorPid(), pidB);
    const int helloB = b.find("MSG 22 perftest-adopt");
    QVERIFY(helloB >= 0);
    QCOMPARE(msgPayload(b.lines[helloB]).value("orphan").toBool(), true);       // the new Supervisor learns the history
    QCOMPARE(qint64(msgPayload(b.lines[helloB]).value("supervisorPid").toDouble()), pidA);
    enginelock::LockInfo lock;
    QVERIFY(enginelock::readLock(enginelock::lockPath(tmp.path()), &lock));
    QCOMPARE(lock.supervisorPid, pidB);
    // The first grace (4 s from orphan mode) passes without an exit: adoption cancelled it.
    while (sinceOrphan.elapsed() < 4600) QTest::qWait(50);
    QCOMPARE(exits.count(), 0);

    // B dies too: orphan again, and this time nobody adopts.
    QTRY_VERIFY_WITH_TIMEOUT(orphan.count() == 3, 6000);
    QElapsedTimer second; second.start();
    QTRY_VERIFY_WITH_TIMEOUT(exits.count() == 1, 7000);
    const qint64 graceMs = second.elapsed();
    qInfo().noquote() << QString("exit requested %1 ms after the second orphan mode began (grace 4 s)").arg(graceMs);
    QCOMPARE(exits.at(0).at(0).toInt(), enginelock::kExitOrphan);
    QVERIFY2(graceMs >= 3500 && graceMs <= 5500, qPrintable(QString("grace %1 ms").arg(graceMs)));
}

// An orphan that reaches a Supervisor which never adopts it (a Supervisor without PS-4b, which starts its own engine
// for the channel) leaves after adoptWaitSec instead of keeping that engine waiting on the guard for the whole grace.
void TestSupervisorLink::orphanYieldsToNonAdoptingSupervisor() {
    if (fakeSupervisor().isEmpty()) QSKIP("fake_supervisor not built");
    QTemporaryDir tmp;
    const QString name = uniqueName();
    Proc a;
    const qint64 pidA = startFakeSupervisor(a, {"--name", name, "--exit-after-ms", "1500"});
    QVERIFY(pidA > 0);
    SupervisorLink::Options o = linkOptions(name, "perftest-yield", tmp.path());
    o.supervisorPid = pidA;
    o.orphanGraceSec = 30;
    o.adoptWaitSec = 2;
    SupervisorLink link(o);
    QSignalSpy orphan(&link, &SupervisorLink::orphanChanged), exits(&link, &SupervisorLink::exitRequested);
    link.start();
    QTRY_VERIFY_WITH_TIMEOUT(orphan.count() == 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!link.isConnected(), 2000);
    Proc b;
    QVERIFY(startFakeSupervisor(b, {"--name", name}) > 0);   // listens, never adopts
    QTRY_VERIFY_WITH_TIMEOUT(link.isConnected(), 3000);
    QElapsedTimer connected; connected.start();
    const int hello = b.waitFor("MSG 22 perftest-yield", 2000);
    QVERIFY(hello >= 0);
    QVERIFY(b.waitFor("MSG 8 perftest-yield", 2000, hello + 1) > hello);       // READY still goes out (older Supervisors)
    QTRY_VERIFY_WITH_TIMEOUT(exits.count() == 1, 4000);
    qInfo().noquote() << QString("left the channel %1 ms after reaching a non-adopting Supervisor (adopt wait 2 s)").arg(connected.elapsed());
    QCOMPARE(exits.at(0).at(0).toInt(), enginelock::kExitOrphan);
    QVERIFY(exits.at(0).at(1).toString().contains("without being adopted"));
    QVERIFY(connected.elapsed() >= 1700 && connected.elapsed() <= 3500);
}

void TestSupervisorLink::engineProcessOrphanExit_data() {
    QTest::addColumn<bool>("explicitParent");
    QTest::newRow("--supervisor-pid") << true;
    QTest::newRow("parent learned from the pipe") << false;
}

// A whole engine-like process: its parent (fake_supervisor) is killed, it enters orphan mode, keeps retrying, and exits
// with code 5 after its 3 s test grace, removing engine.lock on the way out.
void TestSupervisorLink::engineProcessOrphanExit() {
    QFETCH(bool, explicitParent);
    if (fakeSupervisor().isEmpty()) QSKIP("fake_supervisor not built");
    QTemporaryDir tmp;
    const QString name = uniqueName();
    const QString data = tmp.path() + "/channels/perftest-orphan";
    Proc a;
    const qint64 pidA = startFakeSupervisor(a, {"--name", name});
    QVERIFY(pidA > 0);
    Proc e;
    QStringList args{"--engine-child", "--ipc", name, "--id", "perftest-orphan", "--data", data, "--grace", "3"};
    if (explicitParent) args << "--supervisor-pid" << QString::number(pidA);
    e.start(QCoreApplication::applicationFilePath(), args);
    QVERIFY(e.waitFor("CLAIMED", 5000) >= 0);
    const int c = e.waitFor("CONNECTED ", 5000);
    QVERIFY(c >= 0);
    QCOMPARE(e.lines[c].mid(10).toLongLong(), pidA);
    const int hello = a.waitFor(QString("MSG 22 perftest-orphan %1").arg(e.pid()), 3000);
    QVERIFY(hello >= 0);
    QVERIFY(a.waitFor(QString("MSG 8 perftest-orphan %1").arg(e.pid()), 2000, hello + 1) > hello);
    QVERIFY(a.waitFor(QString("MSG 4 perftest-orphan %1").arg(e.pid()), 2000) >= 0);   // heartbeats
    enginelock::LockInfo lock;
    QTRY_VERIFY(enginelock::readLock(enginelock::lockPath(data), &lock) && lock.supervisorPid == pidA);
    QCOMPARE(lock.pid, e.pid());
    QCOMPARE(lock.channelId, QString("perftest-orphan"));
    QCOMPARE(lock.createTime, enginelock::processCreateTime(e.pid()));
    QCOMPARE(lock.supervisorIpc, name);

    QElapsedTimer sinceKill; sinceKill.start();
    a.p.kill();                                               // TerminateProcess: the Supervisor crashed
    QVERIFY(a.p.waitForFinished(3000));
    QVERIFY(e.waitFor("ORPHAN 1", 3000) >= 0);
    QVERIFY(e.p.waitForFinished(8000));
    const qint64 exitMs = sinceKill.elapsed();
    qInfo().noquote() << QString("engine child exited %1 ms after its parent was killed (grace 3 s), code %2").arg(exitMs).arg(e.p.exitCode());
    QCOMPARE(e.p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(e.p.exitCode(), enginelock::kExitOrphan);
    QVERIFY(e.find("EXITREQ 5") >= 0);
    QVERIFY2(exitMs >= 2800 && exitMs <= 5500, qPrintable(QString("exit after %1 ms").arg(exitMs)));
    QVERIFY(e.err.contains("not reachable"));                 // it kept trying to reconnect meanwhile
    QVERIFY(!QFile::exists(enginelock::lockPath(data)));      // clean exit removes the lock
}

// Two engines for one channel of one Supervisor: the second waits 5 s and exits with code 4 naming the owner's pid;
// --takeover starts anyway and takes the guard when the old engine dies; a clean exit releases it at once.
void TestSupervisorLink::duplicateEngineExitsWithCode4() {
    QTemporaryDir tmp;
    const QString name = uniqueName();   // nobody listens: the guard does not need a Supervisor
    const QString data = tmp.path() + "/channels/perftest-dup";
    const QString self = QCoreApplication::applicationFilePath();
    const QStringList base{"--engine-child", "--ipc", name, "--id", "perftest-dup", "--data", data, "--grace", "-1"};

    Proc e1;
    e1.start(self, base);
    QVERIFY(e1.waitFor("CLAIMED owned=1", 5000) >= 0);
    QTRY_VERIFY(QFile::exists(enginelock::lockPath(data)));

    Proc e2;
    QElapsedTimer t2; t2.start();
    e2.start(self, base);
    QVERIFY(e2.p.waitForFinished(9000));
    const qint64 waited = t2.elapsed();
    qInfo().noquote() << QString("second engine exited after %1 ms with code %2").arg(waited).arg(e2.p.exitCode());
    QCOMPARE(e2.p.exitCode(), enginelock::kExitDuplicate);
    QVERIFY2(waited >= 4800 && waited <= 8000, qPrintable(QString("waited %1 ms").arg(waited)));
    QVERIFY(e2.find("CLAIMED") < 0);
    QVERIFY2(e2.err.contains(QString("another engine owns channel perftest-dup (pid %1)").arg(e1.pid()).toUtf8()), e2.err.constData());
    enginelock::LockInfo lock;
    QVERIFY(enginelock::readLock(enginelock::lockPath(data), &lock));
    QCOMPARE(lock.pid, e1.pid());                             // the refused engine wrote nothing
    QVERIFY(e1.p.state() == QProcess::Running);

    // --takeover (the Supervisor killed the old engine, which has not exited yet): starts, owns the guard once e1 dies.
    Proc e3;
    const QString quit3 = tmp.path() + "/quit-e3";
    e3.start(self, QStringList(base) << "--takeover" << "--claim-wait-ms" << "1000" << "--quit-file" << quit3);
    QVERIFY(e3.waitFor("CLAIMED owned=0", 5000) >= 0);
    QTRY_VERIFY(enginelock::readLock(enginelock::lockPath(data), &lock) && lock.pid == e3.pid());
    e1.p.kill();
    QVERIFY(e1.p.waitForFinished(3000));
    QVERIFY(e3.waitFor("OWNED abandoned=1", 3000) >= 0);     // e1 died holding it
    QVERIFY(e3.err.contains("--takeover"));

    Proc e4;
    e4.start(self, QStringList(base) << "--claim-wait-ms" << "1000");
    QVERIFY(e4.p.waitForFinished(5000));
    QCOMPARE(e4.p.exitCode(), enginelock::kExitDuplicate);    // the takeover engine holds the guard now
    QVERIFY(e4.err.contains(QString("(pid %1)").arg(e3.pid()).toUtf8()));

    // A clean exit removes the lock and releases the guard: the next engine claims it without waiting.
    QFile(quit3).open(QIODevice::WriteOnly);
    QVERIFY(e3.p.waitForFinished(5000));
    QCOMPARE(e3.p.exitCode(), 0);
    QVERIFY(!QFile::exists(enginelock::lockPath(data)));
    Proc e5;
    const QString quit5 = tmp.path() + "/quit-e5";
    QElapsedTimer t5; t5.start();
    e5.start(self, QStringList(base) << "--quit-file" << quit5);
    QVERIFY(e5.waitFor("CLAIMED owned=1 abandoned=0", 5000) >= 0);
    QVERIFY2(t5.elapsed() < 3000, qPrintable(QString("claim took %1 ms").arg(t5.elapsed())));
    QVERIFY(!e5.err.contains("waiting up to"));
    QFile(quit5).open(QIODevice::WriteOnly);
    QVERIFY(e5.p.waitForFinished(5000));
    QCOMPARE(e5.p.exitCode(), 0);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--engine-child") == 0) return engineChild(argc, argv);
    QCoreApplication app(argc, argv);
    TestSupervisorLink t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_supervisorlink.moc"
