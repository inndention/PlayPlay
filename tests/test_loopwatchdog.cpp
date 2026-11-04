// Event-loop watchdog, /healthz, self minidumps and the crash filter (STB-4 = perf-supervisor-app W4, P1 rank 20).
// On 10-24 / 10-25 the Supervisor's loop froze for hours and nothing said so; on 10-23 the app died in Qt6Core with no
// dump. Every process now reports its own stalls: a JSON line per stall/hang/recovery, /healthz from a thread of its
// own while the main thread is frozen, a rate-limited dump on a long hang, a dump on a crash.
// Most checks run tests/helpers/hang_helper.cpp, which freezes its own main thread on demand.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include "playout/core/healthendpoint.h"
#include "playout/core/loopwatchdog.h"
#include "playout/core/minidump.h"
#include "playout/core/processlaunch.h"
#include <atomic>
#include <thread>

namespace {

QString helper() { return QStringLiteral(PP_HANG_HELPER); }

struct Http { int status = 0; QByteArray body; QJsonObject json; bool ok = false; qint64 ms = 0; };

Http httpGet(quint16 port, const QByteArray& path, const QByteArray& method = "GET", int timeoutMs = 3000) {
    Http r;
    QElapsedTimer t; t.start();
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    if (!s.waitForConnected(timeoutMs)) return r;
    s.write(method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    QByteArray all;
    while (s.state() == QAbstractSocket::ConnectedState && t.elapsed() < timeoutMs) {
        if (s.waitForReadyRead(200)) all += s.readAll();
    }
    all += s.readAll();
    r.ms = t.elapsed();
    const int sp = all.indexOf(' '), hdrEnd = all.indexOf("\r\n\r\n");
    if (sp < 0 || hdrEnd < 0) return r;
    r.status = all.mid(sp + 1, 3).toInt();
    r.body = all.mid(hdrEnd + 4);
    r.json = QJsonDocument::fromJson(r.body).object();
    r.ok = true;
    return r;
}

QList<QJsonObject> readJsonl(const QString& dir, const QString& name) {
    QList<QJsonObject> out;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"watchdog-" + name + "-*.jsonl"}, QDir::Files, QDir::Name)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray& line : f.readAll().split('\n'))
            if (!line.trimmed().isEmpty()) out << QJsonDocument::fromJson(line).object();
    }
    return out;
}

QList<QJsonObject> events(const QList<QJsonObject>& all, const QString& ev) {
    QList<QJsonObject> out;
    for (const QJsonObject& o : all) if (o.value("event").toString() == ev) out << o;
    return out;
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

bool isMinidump(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) && f.read(4) == "MDMP" && f.size() > 4096;
}

struct Sample { qint64 t; int status; QString state, activity; };

struct HelperRun {
    bool started = false, finished = false;
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    qint64 pid = 0, ms = 0;
    quint16 port = 0;
    QList<Sample> samples;          // /healthz polled while it ran
};

// Starts the helper detached (NUL stdio, no console, as the stack starts its processes), polls /healthz every 120 ms
// when it has one, and waits for it to exit. A helper still running at the timeout is ours: it is killed.
HelperRun runHelper(const QString& dir, const QStringList& args, int timeoutMs) {
    HelperRun r;
    QProcess p;
    processlaunch::configureDetachedChild(p);
    p.setProgram(helper());
    p.setArguments(QStringList{"--dir", dir} + args);
    QElapsedTimer t; t.start();
    p.start();
    r.started = p.waitForStarted(10000);
    r.pid = p.processId();
    const bool health = args.contains("--health");
    while (r.started && t.elapsed() < timeoutMs) {
        if (p.waitForFinished(health ? 120 : 250)) { r.finished = true; break; }
        if (!health) continue;
        if (!r.port) {
            QFile pf(QDir(dir).filePath("port.txt"));
            if (pf.open(QIODevice::ReadOnly)) r.port = quint16(pf.readAll().trimmed().toUInt());
            if (!r.port) continue;
        }
        const qint64 at = t.elapsed();
        const Http h = httpGet(r.port, "/healthz", "GET", 1500);
        if (h.ok) r.samples << Sample{at, h.status, h.json.value("state").toString(), h.json.value("activity").toString()};
    }
    if (r.started && !r.finished) { p.kill(); p.waitForFinished(5000); }
    r.ms = t.elapsed();
    r.exitCode = p.exitCode();
    r.status = p.exitStatus();
    return r;
}

bool sawState(const QList<Sample>& s, const QString& state, const QString& activity = QString(), int status = 0) {
    for (const Sample& x : s)
        if (x.state == state && (activity.isEmpty() || x.activity == activity) && (!status || x.status == status)) return true;
    return false;
}

QString describe(const QList<Sample>& s) {
    QStringList l;
    for (const Sample& x : s) l << QString("%1:%2/%3").arg(x.t).arg(x.state).arg(x.status);
    return l.join(' ');
}

} // namespace

class TestLoopWatchdog : public QObject {
    Q_OBJECT
private slots:
    // --- in-process units -------------------------------------------------------------------------------------------
    void healthEndpointServesAndRefuses();
    void activityTagsNestAndCopy();
    void inProcessRecoverySignalOnce();
    void dumpNamesAreScopedToOneName();
    // --- hang_helper: the slice's acceptance checks -----------------------------------------------------------------
    void stallLineAndHealthzDuringBlock();
    void busyLoopIsNotBlocked();
    void hangWritesStackAndOneDump();
    void dumpsAreRateLimitedAcrossRestarts();
    void accessViolationWritesCrashDump();
    void abortPolicyDumpsThenTerminates();
};

void TestLoopWatchdog::healthEndpointServesAndRefuses() {
    HealthEndpoint ep;
    std::atomic<int> calls{0};
    QString err;
    QVERIFY2(ep.start(0, [&calls](const std::string&, const std::string& path) {
        ++calls;
        HealthEndpoint::Reply r;
        if (path == "/healthz") r.body = "{\"state\":\"ok\"}";
        else { r.status = 404; r.body = "nope"; }
        return r;
    }, &err), qPrintable(err));
    const quint16 port = ep.port();
    QVERIFY(port != 0);

    Http h = httpGet(port, "/healthz?x=1");
    QVERIFY(h.ok);
    QCOMPARE(h.status, 200);
    QCOMPARE(h.json.value("state").toString(), QString("ok"));
    QCOMPARE(httpGet(port, "/other").status, 404);
    QCOMPARE(httpGet(port, "/healthz", "POST").status, 405);
    const Http head = httpGet(port, "/healthz", "HEAD");
    QCOMPARE(head.status, 200);
    QVERIFY(head.body.isEmpty());

    // The port is ours alone: a second listener on it is refused (SO_EXCLUSIVEADDRUSE), with a reason.
    HealthEndpoint second;
    QString err2;
    QVERIFY(!second.start(port, [](const std::string&, const std::string&) { return HealthEndpoint::Reply(); }, &err2));
    QVERIFY(err2.contains(QString::number(port)));

    // A client that connects and says nothing costs at most about a second; the next one is still served.
    QTcpSocket idle;
    idle.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(idle.waitForConnected(2000));
    QElapsedTimer t; t.start();
    h = httpGet(port, "/healthz", "GET", 4000);
    QVERIFY(h.ok);
    QVERIFY2(t.elapsed() < 2500, qPrintable(QString("served after %1 ms").arg(t.elapsed())));

    t.restart();
    ep.stop();
    QVERIFY2(t.elapsed() < 1500, qPrintable(QString("stop took %1 ms").arg(t.elapsed())));
    QVERIFY(!ep.isRunning());
    QTcpSocket gone;
    gone.connectToHost(QHostAddress::LocalHost, port);
    QVERIFY(!gone.waitForConnected(500) || !gone.waitForReadyRead(500));
}

void TestLoopWatchdog::activityTagsNestAndCopy() {
    LoopWatchdog::setActivity("outer");
    QCOMPARE(LoopWatchdog::activity(), QByteArray("outer"));
    {
        WatchdogScope a("static-inner");
        QCOMPARE(LoopWatchdog::activity(), QByteArray("static-inner"));
        {
            WatchdogScope b(QByteArray("REST GET /api/v1/library"));
            QCOMPARE(LoopWatchdog::activity(), QByteArray("REST GET /api/v1/library"));
            {
                WatchdogScope c("deepest");
                QCOMPARE(LoopWatchdog::activity(), QByteArray("deepest"));
            }
            QCOMPARE(LoopWatchdog::activity(), QByteArray("REST GET /api/v1/library"));
        }
        QCOMPARE(LoopWatchdog::activity(), QByteArray("static-inner"));
    }
    QCOMPARE(LoopWatchdog::activity(), QByteArray("outer"));
    // Copied tags are cut to 95 bytes, never inside a UTF-8 sequence.
    LoopWatchdog::setActivityText(QByteArray(200, 'x'));
    QCOMPARE(LoopWatchdog::activity().size(), 95);
    LoopWatchdog::setActivityText(QByteArray(94, 'y') + QString::fromUtf8("\xc3\xa9\xc3\xa9").toUtf8());   // 94 + 2x2 bytes
    QCOMPARE(LoopWatchdog::activity(), QByteArray(94, 'y'));
    LoopWatchdog::setActivity(nullptr);
    QVERIFY(LoopWatchdog::activity().isEmpty());
}

void TestLoopWatchdog::inProcessRecoverySignalOnce() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Old JSON lines of this name are pruned at install; another name's file that only shares the prefix is not.
    const QString old = dir.filePath("watchdog-inproc-2020-01-01.jsonl"), other = dir.filePath("watchdog-inproc-x-2020-01-01.jsonl");
    for (const QString& f : {old, other}) { QFile x(f); QVERIFY(x.open(QIODevice::WriteOnly)); x.write("{}\n"); }

    LoopWatchdogOptions o;
    o.name = "inproc"; o.logDir = dir.path(); o.beatMs = 50; o.stallMs = 300; o.hangMs = 5000; o.dumpAfterMs = 0;
    o.healthPort = -1; o.crashDumps = false;
    LoopWatchdog* wd = LoopWatchdog::install(o);
    QVERIFY(wd);
    QCOMPARE(LoopWatchdog::instance(), wd);
    QSignalSpy spy(wd, &LoopWatchdog::recovered);
    QTest::qWait(400);                                           // a few beats: state ok
    QCOMPARE(httpGet(wd->healthPort(), "/healthz").json.value("state").toString(), QString("ok"));

    // Freeze this (main) thread for 1.2 s while another thread asks /healthz half-way through.
    Http during;
    std::thread probe([&during, port = wd->healthPort()]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        during = httpGet(port, "/healthz", "GET", 2000);
    });
    {
        WatchdogScope scope("inproc-freeze");
        QThread::msleep(1200);
    }
    probe.join();
    QVERIFY(during.ok);
    QCOMPARE(during.json.value("state").toString(), QString("stall"));
    QCOMPARE(during.json.value("activity").toString(), QString("inproc-freeze"));
    QCOMPARE(during.json.value("name").toString(), QString("inproc"));
    QCOMPARE(during.json.value("pid").toInteger(), QCoreApplication::applicationPid());
    QVERIFY(during.json.value("lastBeatAgeMs").toInteger() >= 600);

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 3000);
    QTest::qWait(500);                                           // and it stays at one
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QString("inproc"));
    QVERIFY2(args.at(1).toLongLong() >= 1100 && args.at(1).toLongLong() < 2500, qPrintable(QString::number(args.at(1).toLongLong())));
    QCOMPARE(args.at(2).toString(), QString("inproc-freeze"));
    QCOMPARE(args.at(3).toString(), QString("stall"));

    const LoopWatchdog::Stats st = wd->stats();
    QCOMPARE(st.stalls, quint64(1));
    QCOMPARE(st.hangs, quint64(0));
    QVERIFY(st.maxLagMs >= 1000);
    QVERIFY(st.recentMaxLagMs >= 1000);                          // what /metrics exports right after the stall
    QCOMPARE(st.state, int(LoopWatchdog::Ok));

    QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(old), 2000);
    QVERIFY(QFile::exists(other));
    const QList<QJsonObject> lines = readJsonl(dir.path(), "inproc");
    QCOMPARE(events(lines, "start").size(), 1);
    QCOMPARE(events(lines, "stall").size(), 1);
    QCOMPARE(events(lines, "recovered").size(), 1);
    QCOMPARE(events(lines, "stall").first().value("activity").toString(), QString("inproc-freeze"));

    // Calls from another thread never change the main thread's tag.
    LoopWatchdog::setActivity("main-tag");
    std::thread other_([]() { LoopWatchdog::setActivity("from-worker"); LoopWatchdog::setActivityText("worker-text"); WatchdogScope s("worker-scope"); });
    other_.join();
    QCOMPARE(LoopWatchdog::activity(), QByteArray("main-tag"));
    LoopWatchdog::setActivity(nullptr);

    delete wd;
    QVERIFY(!LoopWatchdog::instance());
}

void TestLoopWatchdog::dumpNamesAreScopedToOneName() {
    QCOMPARE(minidump::sanitizedName("engine-main"), QString("engine-main"));
    QCOMPARE(minidump::sanitizedName("engine-a b/c"), QString("engine-a_b_c"));
    QTemporaryDir dir;
    for (const char* n : {"engine-main-100-20261026T120000Z-hang.dmp", "engine-main-2-200-20261026T120000Z-hang.dmp",
                          "engine-main-x.dmp", "engine-main-300-20261026T130000Z-crash.dmp"}) {
        QFile f(dir.filePath(n)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x");
    }
    const QStringList mine = minidump::existingDumps(dir.path(), "engine-main");
    QCOMPARE(mine.size(), 2);
    for (const QString& p : mine) QVERIFY2(!p.contains("engine-main-2-") && !p.contains("engine-main-x"), qPrintable(p));
    QCOMPARE(minidump::existingDumps(dir.path(), "engine-main-2").size(), 1);
}

void TestLoopWatchdog::stallLineAndHealthzDuringBlock() {
    // The slice's first check, with the real Supervisor thresholds: main thread blocked 3 s.
    QTemporaryDir dir;
    const HelperRun r = runHelper(dir.path(), {"--name", "blk", "--health", "--block-at", "1000", "--block", "3000",
                                               "--activity", "test-block", "--exit-after", "5500"}, 30000);
    QVERIFY(r.started && r.finished);
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.port != 0);
    QVERIFY2(sawState(r.samples, "ok"), qPrintable(describe(r.samples)));
    QVERIFY2(sawState(r.samples, "stall", "test-block", 200), qPrintable(describe(r.samples)));
    QVERIFY2(!sawState(r.samples, "hang"), qPrintable(describe(r.samples)));
    int lastStall = -1, okAfter = 0;                              // and back to ok afterwards
    for (int i = 0; i < r.samples.size(); ++i) {
        if (r.samples[i].state == "stall") lastStall = i;
        else if (lastStall >= 0 && r.samples[i].state == "ok") ++okAfter;
    }
    QVERIFY2(okAfter >= 2, qPrintable(describe(r.samples)));

    const QList<QJsonObject> lines = readJsonl(dir.path(), "blk");
    const QList<QJsonObject> stalls = events(lines, "stall"), rec = events(lines, "recovered");
    QCOMPARE(events(lines, "start").size(), 1);
    QCOMPARE(stalls.size(), 1);
    QCOMPARE(stalls.first().value("activity").toString(), QString("test-block"));
    QVERIFY(stalls.first().value("lagMs").toInteger() >= 1000);
    QCOMPARE(stalls.first().value("blocked").toBool(), true);    // Sleep: waiting, not computing
    QVERIFY(stalls.first().value("pid").toInteger() == r.pid);
    QVERIFY(stalls.first().contains("privateMB") && stalls.first().contains("handles") && stalls.first().value("threads").toInteger() > 1);
    QCOMPARE(rec.size(), 1);
    QCOMPARE(rec.first().value("worst").toString(), QString("stall"));
    const qint64 stalled = rec.first().value("stalledMs").toInteger();
    QVERIFY2(stalled >= 2900 && stalled < 3800, qPrintable(QString::number(stalled)));
    QVERIFY(events(lines, "hang").isEmpty());
    QVERIFY(events(lines, "dump").isEmpty());
    QVERIFY(minidump::existingDumps(dir.filePath("dumps"), "blk").isEmpty());

    const QJsonObject res = readJson(dir.filePath("result.json"));
    QCOMPARE(res.value("recovered").toArray().size(), 1);         // the recovery signal fired exactly once
    QCOMPARE(res.value("recovered").toArray().first().toObject().value("activity").toString(), QString("test-block"));
    QCOMPARE(res.value("stalls").toInt(), 1);
    QCOMPARE(res.value("hangs").toInt(), 0);
}

void TestLoopWatchdog::busyLoopIsNotBlocked() {
    QTemporaryDir dir;
    const HelperRun r = runHelper(dir.path(), {"--name", "busy", "--stall", "500", "--block-at", "500", "--block", "2500",
                                               "--busy", "--exit-after", "3800"}, 30000);
    QVERIFY(r.started && r.finished);
    const QList<QJsonObject> lines = readJsonl(dir.path(), "busy");
    const QList<QJsonObject> rec = events(lines, "recovered");
    QCOMPARE(rec.size(), 1);
    QCOMPARE(rec.first().value("blocked").toBool(), false);       // spinning: the main thread's CPU time rose
    QVERIFY2(rec.first().value("mainCpuDeltaMs").toInteger() > 1000, qPrintable(QJsonDocument(rec.first()).toJson()));
}

void TestLoopWatchdog::hangWritesStackAndOneDump() {
    // Blocked 12 s with the Supervisor defaults (stall 1 s, hang 5 s, dump 10 s, abort never).
    QTemporaryDir dir;
    const HelperRun r = runHelper(dir.path(), {"--name", "hang12", "--health", "--block-at", "800", "--block", "12000",
                                               "--exit-after", "14000"}, 40000);
    QVERIFY(r.started && r.finished);
    QCOMPARE(r.exitCode, 0);                                      // report only: it was not killed
    QVERIFY2(sawState(r.samples, "stall", "test-block", 200), qPrintable(describe(r.samples)));
    // 200 even in a hang: the app's stall banner (SupervisorClient::probeHealth) drops the body of an error reply.
    QVERIFY2(sawState(r.samples, "hang", "test-block", 200), qPrintable(describe(r.samples)));

    const QList<QJsonObject> lines = readJsonl(dir.path(), "hang12");
    QCOMPARE(events(lines, "stall").size(), 1);
    const QList<QJsonObject> hang = events(lines, "hang");
    QCOMPARE(hang.size(), 1);
    QVERIFY(hang.first().value("lagMs").toInteger() >= 5000);
    const QJsonArray stack = hang.first().value("stack").toArray();
    QVERIFY2(stack.size() >= 3, qPrintable(QJsonDocument(hang.first()).toJson()));
    QString frames;
    for (const QJsonValue& v : stack) frames += v.toString() + "\n";
    // The helper has a PDB (lane fragment): the frame names the function that blocks, with its source line.
    QVERIFY2(frames.contains("hang_helper!") && frames.contains("blockMainThread") && frames.contains("hang_helper.cpp:"), qPrintable(frames));
    QVERIFY2(frames.contains("Sleep", Qt::CaseInsensitive), qPrintable(frames));

    const QList<QJsonObject> dump = events(lines, "dump");
    QCOMPARE(dump.size(), 1);
    const QString path = dump.first().value("dump").toString();
    QVERIFY2(!path.isEmpty(), qPrintable(QJsonDocument(dump.first()).toJson()));
    QVERIFY(dump.first().value("lagMs").toInteger() >= 10000);
    QVERIFY2(isMinidump(path), qPrintable(path));
    QVERIFY(path.endsWith("-hang.dmp"));
    QVERIFY(QFileInfo(path).fileName().startsWith(QString("hang12-%1-").arg(r.pid)));
    QCOMPARE(minidump::existingDumps(dir.filePath("dumps"), "hang12").size(), 1);

    const QList<QJsonObject> rec = events(lines, "recovered");
    QCOMPARE(rec.size(), 1);
    QCOMPARE(rec.first().value("worst").toString(), QString("hang"));
    QVERIFY2(rec.first().value("stalledMs").toInteger() >= 11500, qPrintable(QJsonDocument(rec.first()).toJson()));
    const QJsonObject res = readJson(dir.filePath("result.json"));
    QCOMPARE(res.value("recovered").toArray().size(), 1);
    QCOMPARE(res.value("dumps").toInt(), 1);
    QCOMPARE(res.value("hangs").toInt(), 1);
}

void TestLoopWatchdog::dumpsAreRateLimitedAcrossRestarts() {
    QTemporaryDir dir;
    const QStringList fast{"--name", "rate", "--stall", "300", "--hang", "800", "--dump", "1500", "--max-dumps", "1"};
    // Two hangs in one process: the first writes a dump, the second is over the limit (1 per hour).
    HelperRun r = runHelper(dir.path(), fast + QStringList{"--block-at", "500", "--block", "2500", "--blocks", "2", "--gap", "700",
                                                          "--exit-after", "7000"}, 30000);
    QVERIFY(r.started && r.finished);
    QList<QJsonObject> dumps = events(readJsonl(dir.path(), "rate"), "dump");
    QCOMPARE(dumps.size(), 2);
    QVERIFY(isMinidump(dumps.at(0).value("dump").toString()));
    QVERIFY2(dumps.at(1).value("dumpError").toString().startsWith("rate limit"), qPrintable(QJsonDocument(dumps.at(1)).toJson()));
    QCOMPARE(readJson(dir.filePath("result.json")).value("recovered").toArray().size(), 2);   // once per episode
    // A restarted process counts the files already on disk.
    r = runHelper(dir.path(), fast + QStringList{"--block-at", "500", "--block", "2200", "--exit-after", "3200"}, 30000);
    QVERIFY(r.started && r.finished);
    dumps = events(readJsonl(dir.path(), "rate"), "dump");
    QCOMPARE(dumps.size(), 3);
    QVERIFY(dumps.at(2).value("dumpError").toString().startsWith("rate limit"));
    QCOMPARE(minidump::existingDumps(dir.filePath("dumps"), "rate").size(), 1);
}

void TestLoopWatchdog::accessViolationWritesCrashDump() {
    QTemporaryDir dir;
    const HelperRun r = runHelper(dir.path(), {"--name", "crash", "--crash", "av", "--crash-at", "600", "--prev-filter",
                                               "--exit-after", "8000"}, 30000);
    QVERIFY(r.started && r.finished);
    QCOMPARE(quint32(r.exitCode), quint32(0xC0000005));           // it still dies the way it did before
    QVERIFY2(r.ms < 7000, qPrintable(QString::number(r.ms)));
    const QStringList d = minidump::existingDumps(dir.filePath("dumps"), "crash");
    QCOMPARE(d.size(), 1);
    QVERIFY(d.first().endsWith("-crash.dmp"));
    QVERIFY(isMinidump(d.first()));
    QVERIFY2(QFile::exists(dir.filePath("prevfilter.txt")), "the filter installed before the watchdog's was not called");
}

void TestLoopWatchdog::abortPolicyDumpsThenTerminates() {
    QTemporaryDir dir;
    const HelperRun r = runHelper(dir.path(), {"--name", "abort", "--stall", "300", "--hang", "600", "--dump", "1000",
                                               "--abort", "2000", "--block-at", "500", "--block", "10000",
                                               "--exit-after", "12000"}, 30000);
    QVERIFY(r.started && r.finished);
    QCOMPARE(quint32(r.exitCode), quint32(0xDEAD));
    QVERIFY2(r.ms < 8000, qPrintable(QString::number(r.ms)));
    const QList<QJsonObject> lines = readJsonl(dir.path(), "abort");
    QCOMPARE(events(lines, "dump").size(), 1);
    QCOMPARE(events(lines, "abort").size(), 1);
    QCOMPARE(minidump::existingDumps(dir.filePath("dumps"), "abort").size(), 1);
    QVERIFY(!QFile::exists(dir.filePath("result.json")));
}

QTEST_GUILESS_MAIN(TestLoopWatchdog)
#include "test_loopwatchdog.moc"
