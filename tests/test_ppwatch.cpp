// PlayPlayWatchdog.exe (STB-6 = perf-supervisor-app W19): the external, report-only watchdog. It polls /healthz (STB-4)
// and, when a process hangs, writes an incident bundle (thread wait reasons, netstat, log tails) and a dump of THAT
// process, identified from its configuration; it never acts on a pid outside it, and restarting is opt-in.
// Units drive the probe, the tracker and the system snapshots in process; the end-to-end checks run the real exe against
// tests/helpers/hang_helper (STB-4), which freezes its own main thread on demand, and against a helper this test suspends.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include "playout/core/healthendpoint.h"
#include "playout/core/processlaunch.h"
#include "tools/ppwatch/healthprobe.h"
#include "tools/ppwatch/incident.h"
#include <atomic>
#include <memory>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace ppwatch;

namespace {

QString helperExe() { return QStringLiteral(PP_HANG_HELPER); }
QString watchdogExe() { return QStringLiteral(PP_PPWATCH_EXE); }

std::unique_ptr<QTemporaryDir> tempDir() {
    auto d = std::make_unique<QTemporaryDir>();
    if (qEnvironmentVariableIsSet("PPWATCH_KEEP")) d->setAutoRemove(false);   // look at the bundles afterwards
    return d;
}

struct Helper {
    std::unique_ptr<QProcess> p;
    QString dir;
    quint16 port = 0;
    quint32 pid = 0;
    bool finished(int ms) { return p->state() == QProcess::NotRunning || p->waitForFinished(ms); }
    ~Helper() { if (p && p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(5000); } }   // ours only
};

// hang_helper detached (NUL stdio, no console), its /healthz on a free port that it writes to <dir>/port.txt.
std::unique_ptr<Helper> startHelper(const QString& dir, const QStringList& args) {
    auto h = std::make_unique<Helper>();
    h->dir = dir;
    QDir().mkpath(dir);
    h->p = std::make_unique<QProcess>();
    processlaunch::configureDetachedChild(*h->p);
    h->p->setProgram(helperExe());
    h->p->setArguments(QStringList{"--dir", dir, "--health"} + args);
    h->p->start();
    if (!h->p->waitForStarted(10000)) return h;
    h->pid = quint32(h->p->processId());
    QElapsedTimer t; t.start();
    while (!h->port && t.elapsed() < 10000) {
        QFile f(QDir(dir).filePath("port.txt"));
        if (f.open(QIODevice::ReadOnly)) h->port = quint16(f.readAll().trimmed().toUInt());
        if (!h->port) QThread::msleep(50);
    }
    return h;
}

struct Run { int exitCode = -1; bool finished = false; QByteArray out, err; qint64 ms = 0; };

Run runWatchdog(const QStringList& args, int timeoutMs) {
    Run r;
    QProcess p;
    p.setProgram(watchdogExe());
    p.setArguments(args);
    QElapsedTimer t; t.start();
    p.start();
    if (!p.waitForStarted(10000)) return r;
    r.finished = p.waitForFinished(timeoutMs);
    if (!r.finished) { p.kill(); p.waitForFinished(5000); }
    r.ms = t.elapsed();
    r.exitCode = p.exitCode();
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    return r;
}

QList<QJsonObject> readEvents(const QString& outDir) {
    QList<QJsonObject> l;
    for (const QFileInfo& fi : QDir(outDir).entryInfoList({"ppwatch-*.jsonl"}, QDir::Files, QDir::Name)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray& line : f.readAll().split('\n')) if (!line.trimmed().isEmpty()) l << QJsonDocument::fromJson(line).object();
    }
    return l;
}

QList<QJsonObject> eventsOf(const QList<QJsonObject>& all, const QString& ev, const QString& target = QString()) {
    QList<QJsonObject> l;
    for (const QJsonObject& o : all)
        if (o.value("event").toString() == ev && (target.isEmpty() || o.value("target").toString() == target)) l << o;
    return l;
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

QString readText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// The process id recorded inside a minidump (MiscInfoStream), parsed from the file: proves whose dump it is.
qint64 dumpProcessId(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray d = f.readAll();
    auto u32 = [&d](qint64 off) -> quint32 { if (off < 0 || off + 4 > d.size()) return 0; quint32 v; memcpy(&v, d.constData() + off, 4); return v; };
    if (u32(0) != 0x504d444d) return -1;   // "MDMP"
    const quint32 n = u32(8), dir = u32(12);
    for (quint32 i = 0; i < n; ++i) {
        const qint64 e = qint64(dir) + qint64(i) * 12;
        if (u32(e) == 15 /* MiscInfoStream */) {
            const quint32 rva = u32(e + 8);
            if (u32(rva + 4) & 1 /* MINIDUMP_MISC1_PROCESS_ID */) return u32(rva + 8);
        }
    }
    return -1;
}

QStringList incidentDirs(const QString& root) { return QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name); }

QStringList allDumps(const QString& root) {
    QStringList l;
    QDirIterator it(root, {"*.dmp"}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) l << it.next();
    return l;
}

#ifdef _WIN32
// Freezes a whole process (ours: a helper) the way a frozen machine or a debugger break would: /healthz stops answering
// while the port still accepts connections.
struct ProcessSuspender {
    HANDLE h = nullptr;
    bool suspended = false;
    using Fn = LONG(NTAPI*)(HANDLE);
    explicit ProcessSuspender(quint32 pid) { h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid); }
    bool suspend() { auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess")); suspended = h && f && f(h) >= 0; return suspended; }
    void resume() { auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess")); if (suspended && f) f(h); suspended = false; }
    ~ProcessSuspender() { resume(); if (h) CloseHandle(h); }
};
#endif

} // namespace

class TestPpwatch : public QObject {
    Q_OBJECT
private slots:
    // --- units ----------------------------------------------------------------------------------------------------------
    void urlsAreLoopbackOnly();
    void trackerEpisodes();
    void trackerRestartIsOptIn();
    void probeAnswersTimeoutsAndRefusals();
    void snapshotsThreadsSocketsCommandLines();
    void textHelpers();
    void incidentFoldersRateLimitAndPrune();
    void writeIncidentForAHelper();
    // --- the exe ----------------------------------------------------------------------------------------------------
    void onceAndPrintConfig();
    void hangAndUnresponsiveWriteBundlesOnlyForConfiguredPids();
    void restartIsOptInAndScoped();
};

void TestPpwatch::urlsAreLoopbackOnly() {
    Url u;
    QString err;
    QVERIFY(parseUrl("http://127.0.0.1:7799/healthz", &u, &err));
    QCOMPARE(u.port, quint16(7799));
    QCOMPARE(u.path, QByteArray("/healthz"));
    QVERIFY(parseUrl("localhost:9001", &u, &err));
    QCOMPARE(u.host, QString("127.0.0.1"));
    QCOMPARE(u.path, QByteArray("/healthz"));
    QVERIFY(parseUrl("http://[::1]:7799/healthz?x=1", &u, &err));
    QVERIFY(u.v6);
    QCOMPARE(u.text(), QString("http://[::1]:7799/healthz?x=1"));
    // Never another machine (the live AN-FS host, the LAN), never https, never without a port.
    for (const char* bad : {"http://192.168.1.177:7709/healthz", "http://example.invalid:8080/", "https://127.0.0.1:7799/healthz",
                            "http://127.0.0.1/healthz", "ftp://127.0.0.1:21/", "http://0.0.0.0:7799/healthz", "http://127.0.0.1:99999/"}) {
        QVERIFY2(!parseUrl(QString::fromLatin1(bad), &u, &err), bad);
        QVERIFY(!err.isEmpty());
    }
}

void TestPpwatch::trackerEpisodes() {
    Tracker t;   // defaults: 2 missed polls -> unresponsive, restart off
    auto ans = [](qint64 ms, const char* st, qint64 pid = 100) { Observation o; o.tMs = ms; o.reach = Observation::Answered; o.state = st; o.pid = pid; return o; };
    auto miss = [](qint64 ms, qint64 pid = 100) { Observation o; o.tMs = ms; o.reach = Observation::NoAnswer; o.pid = pid; return o; };
    Decision d = t.feed(ans(0, "ok"));
    QVERIFY(d.changed); QCOMPARE(d.from, QString("unknown")); QCOMPARE(d.to, QString("ok")); QVERIFY(!d.incident);
    QVERIFY(!t.feed(ans(2000, "ok")).changed);
    d = t.feed(ans(4000, "stall"));
    QVERIFY(d.changed && !d.incident);
    d = t.feed(ans(6000, "hang"));
    QVERIFY(d.incident); QCOMPARE(d.episodeMs, 0);
    d = t.feed(ans(8000, "hang"));
    QVERIFY(!d.incident && !d.changed); QCOMPARE(d.episodeMs, 2000);
    // hang -> unresponsive (its health thread stops too) is the same episode: no second incident.
    QVERIFY(!t.feed(miss(10000)).changed);           // one miss is noise
    d = t.feed(miss(12000));
    QVERIFY(d.changed); QCOMPARE(d.to, QString("unresponsive")); QVERIFY(!d.incident); QCOMPARE(d.episodeMs, 6000);
    d = t.feed(ans(14000, "ok"));
    QVERIFY(d.changed && !t.inEpisode());
    // A process that accepts and never answers: unresponsive after the second miss, one incident.
    QVERIFY(!t.feed(miss(16000)).incident);
    d = t.feed(miss(18000));
    QVERIFY(d.incident); QCOMPARE(d.to, QString("unresponsive"));
    // Someone restarted it (new pid behind the port, still hung): a new episode, a new incident.
    d = t.feed(ans(20000, "hang", 200));
    QVERIFY(d.incident); QCOMPARE(t.episodePid(), qint64(200));
    // Nothing listening / pid gone end the episode and are reported as such.
    Observation down; down.tMs = 22000; down.reach = Observation::NoListener;
    d = t.feed(down);
    QCOMPARE(d.to, QString("down")); QVERIFY(!t.inEpisode());
    Observation gone; gone.tMs = 24000; gone.reach = Observation::PidGone;
    QCOMPARE(t.feed(gone).to, QString("gone"));
    // A missing "state" field (an older or generic endpoint) answering 200 counts as ok.
    QCOMPARE(t.feed(ans(26000, "")).to, QString("ok"));
}

void TestPpwatch::trackerRestartIsOptIn() {
    auto hang = [](qint64 ms) { Observation o; o.tMs = ms; o.reach = Observation::Answered; o.state = "hang"; o.pid = 7; return o; };
    // Default: never, however long the hang.
    Tracker off(TrackerOptions{2, 0, true});
    for (qint64 t = 0; t <= 600000; t += 2000) QVERIFY(!off.feed(hang(t)).restart);
    // Opt-in but not the supervisor role: never.
    Tracker engine(TrackerOptions{2, 10000, false});
    for (qint64 t = 0; t <= 60000; t += 2000) QVERIFY(!engine.feed(hang(t)).restart);
    // Opt-in supervisor: once, when the episode has lasted restartAfterMs; again only in a new episode.
    Tracker sup(TrackerOptions{2, 10000, true});
    int restarts = 0; qint64 at = -1;
    for (qint64 t = 0; t <= 60000; t += 2000) { const Decision d = sup.feed(hang(t)); if (d.restart) { ++restarts; at = t; } }
    QCOMPARE(restarts, 1);
    QCOMPARE(at, qint64(10000));
    Observation ok; ok.tMs = 62000; ok.reach = Observation::Answered; ok.state = "ok"; ok.pid = 7;
    sup.feed(ok);
    restarts = 0;
    for (qint64 t = 64000; t <= 90000; t += 2000) if (sup.feed(hang(t)).restart) ++restarts;
    QCOMPARE(restarts, 1);
}

void TestPpwatch::probeAnswersTimeoutsAndRefusals() {
    HealthEndpoint ep;
    QString err;
    QVERIFY2(ep.start(0, [](const std::string&, const std::string& path) {
        HealthEndpoint::Reply r;
        if (path == "/healthz") r.body = "{\"name\":\"unit\",\"pid\":4242,\"state\":\"stall\",\"lagMs\":1234}";
        else { r.status = 404; r.body = "{}"; }
        return r;
    }, &err), qPrintable(err));
    // Accepts (the kernel completes the handshake) and never answers: the 10-25 Supervisor's REST port.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost, 0));
    QTcpServer closedSrv;
    QVERIFY(closedSrv.listen(QHostAddress::LocalHost, 0));
    const quint16 closedPort = closedSrv.serverPort();
    closedSrv.close();

    Url a, b, c, d;
    QVERIFY(parseUrl(QString("http://127.0.0.1:%1/healthz").arg(ep.port()), &a));
    QVERIFY(parseUrl(QString("http://127.0.0.1:%1/healthz").arg(silent.serverPort()), &b));
    QVERIFY(parseUrl(QString("http://127.0.0.1:%1/healthz").arg(closedPort), &c));
    QVERIFY(parseUrl(QString("http://127.0.0.1:%1/other").arg(ep.port()), &d));
    QElapsedTimer t; t.start();
    const std::vector<ProbeResult> r = probeAll({a, b, c, d}, 1200);
    const qint64 ms = t.elapsed();
    QCOMPARE(int(r.size()), 4);
    QCOMPARE(int(r[0].kind), int(ProbeResult::Answered));
    QCOMPARE(r[0].status, 200);
    QCOMPARE(r[0].state(), QString("stall"));
    QCOMPARE(r[0].pid(), qint64(4242));
    QCOMPARE(r[0].json.value("lagMs").toInt(), 1234);
    QCOMPARE(int(r[1].kind), int(ProbeResult::ReplyTimeout));
    // A closed loopback port: refused, or (Windows retries the SYN for about 2 s) no connection within the timeout.
    QVERIFY2(r[2].kind == ProbeResult::Refused || r[2].kind == ProbeResult::ConnectTimeout, ProbeResult::kindName(r[2].kind));
    QCOMPARE(int(r[3].kind), int(ProbeResult::Answered));
    QCOMPARE(r[3].status, 404);
    QCOMPARE(r[3].state(), QString("error"));
    // All at once: one timeout in all, not one per target.
    QVERIFY2(ms < 1200 + 600, qPrintable(QString("probeAll took %1 ms").arg(ms)));
    QVERIFY2(r[0].ms < 1000, qPrintable(QString("answer took %1 ms").arg(r[0].ms)));
}

void TestPpwatch::snapshotsThreadsSocketsCommandLines() {
#ifndef _WIN32
    QSKIP("Windows only");
#else
    const quint32 self = GetCurrentProcessId();
    std::atomic<quint32> sleeperTid{0};
    std::thread sleeper([&sleeperTid]() {
        using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        if (auto f = reinterpret_cast<SetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))
            f(GetCurrentThread(), L"ppwatch-test-sleeper");
        sleeperTid = GetCurrentThreadId();
        Sleep(2500);
    });
    while (!sleeperTid) QThread::msleep(5);
    QThread::msleep(200);   // inside Sleep by now

    QString err;
    const std::vector<ProcInfo> procs = processSnapshot(true, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(procs.size() > 20);
    const ProcInfo* me = findProcess(procs, self);
    QVERIFY(me);
    QCOMPARE(me->image.toLower(), QString("test_ppwatch.exe"));
    QVERIFY(me->ppid != 0);
    QVERIFY(me->threads.size() >= 2);
    QVERIFY(me->handles > 0 && me->privateBytes > 0 && me->createTime > 0);
    const ThreadInfo* st = nullptr;
    for (const ThreadInfo& t : me->threads) if (t.tid == sleeperTid) st = &t;
    QVERIFY(st);
    QCOMPARE(threadStateName(st->state), QString("Waiting"));
    QVERIFY2(waitReasonName(st->waitReason).contains("DelayExecution"), qPrintable(waitReasonName(st->waitReason)));

    QString text;
    const QJsonArray threads = describeThreads(*me, &text);
    QCOMPARE(threads.size(), int(me->threads.size()));
    QVERIFY(threads.first().toObject().value("first").toBool());    // the main thread leads
    bool named = false;
    for (const QJsonValue& v : threads) {
        const QJsonObject o = v.toObject();
        if (quint32(o.value("tid").toDouble()) == sleeperTid) {
            named = o.value("name").toString() == "ppwatch-test-sleeper";
            QVERIFY(!o.value("start").toString().isEmpty());
        }
    }
    QVERIFY(named);
    QVERIFY(text.contains("ppwatch-test-sleeper"));
    QVERIFY(text.contains("DelayExecution"));
    QVERIFY(text.contains("(first thread)"));
    sleeper.join();

    // Who owns a listening port.
    HealthEndpoint ep;
    QVERIFY(ep.start(0, [](const std::string&, const std::string&) { return HealthEndpoint::Reply(); }));
    const std::vector<TcpRow> tcp = tcpTable(&err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(listenerPid(tcp, ep.port()), self);
    QCOMPARE(listenerPid(tcp, 1), quint32(0));
    const QString ns = netstatText(tcp, {ep.port()}, 0, &procs);
    QVERIFY(ns.contains(QString("127.0.0.1:%1").arg(ep.port())));
    QVERIFY(ns.contains("LISTEN"));
    QVERIFY(ns.contains(QString("%1: LISTEN 1").arg(ep.port())));

    // This process, read through a handle.
    const ProcessId q = queryProcess(self);
    QVERIFY(q.alive);
    QCOMPARE(q.image.toLower(), QString("test_ppwatch.exe"));
    QCOMPARE(q.createTime, me->createTime);
    QVERIFY(!queryProcess(0x7FFFFFF0).alive);
    QVERIFY(isProtectedProcess(self, q.image));                     // never itself
    QVERIFY(isProtectedProcess(4, "System"));
    QVERIFY(isProtectedProcess(1234, "svchost.exe"));
    QVERIFY(isProtectedProcess(1234, ""));                          // unknown image: nothing to verify
    QVERIFY(!isProtectedProcess(1234, "PlayPlaySupervisor.exe"));
    QVERIFY(commandLine(self).contains("test_ppwatch", Qt::CaseInsensitive));
#endif
}

void TestPpwatch::textHelpers() {
    // The engine command line as ChannelManager builds it (channelmanager.cpp: --id --name --port --data --supervisor
    // --config, + --log-dir from SupervisorCore).
    const QString cmd = R"("D:\PlayPlay\PlayPlayChannelEngine.exe" --id main --name "Main Channel" --port 9001 --data "C:\data dir\main" )"
                        R"(--supervisor PlayPlaySupervisor --log-dir "C:\Users\x\AppData\Roaming\PlayPlay\PLAY PLAY\supervisor\logs")";
    EngineArgs e = parseEngineArgs(cmd);
    QCOMPARE(e.id, QString("main"));
    QCOMPARE(e.port, 9001);
    QCOMPARE(e.logDir, QString(R"(C:\Users\x\AppData\Roaming\PlayPlay\PLAY PLAY\supervisor\logs)"));
    QVERIFY(!e.standalone);
    e = parseEngineArgs("engine.exe --standalone --id=perftest-1 --port=50050");
    QVERIFY(e.standalone);
    QCOMPARE(e.id, QString("perftest-1"));
    QCOMPARE(e.port, 50050);
    QCOMPARE(splitCommandLine(cmd).size(), 13);

    // Credentials never reach a bundle.
    const QString red = redactCommandLine(R"(tool.exe --api-key sk-or-123 --token=abc --name x --Password "p w" /auth:z --port 1)");
    QVERIFY2(!red.contains("sk-or-123") && !red.contains("abc") && !red.contains("p w"), qPrintable(red));
    QVERIFY(red.contains("--api-key (hidden)"));
    QVERIFY(red.contains("--token=(hidden)"));
    QVERIFY(red.contains("--name x"));
    QVERIFY(red.contains("--port 1"));
    QVERIFY(red.contains("/auth:(hidden)"));

    // Orphans: an engine whose parent is gone, whose parent pid was reused, or whose parent is not the Supervisor.
    auto proc = [](quint32 pid, quint32 ppid, const char* image, qint64 created) { ProcInfo p; p.pid = pid; p.ppid = ppid; p.image = image; p.createTime = created; return p; };
    const std::vector<ProcInfo> ps = {
        proc(10, 1, "PlayPlaySupervisor.exe", 100),
        proc(11, 10, "PlayPlayChannelEngine.exe", 200),    // fine
        proc(12, 99, "PlayPlayChannelEngine.exe", 200),    // parent gone
        proc(20, 1, "PlayPlaySupervisor.exe", 500),         // started after 13: pid 20 was reused
        proc(13, 20, "PlayPlayChannelEngine.exe", 300),
        proc(30, 1, "pwsh.exe", 100),
        proc(14, 30, "PlayPlayChannelEngine.exe", 300),    // started by hand
        proc(15, 10, "hang_helper.exe", 300)};
    const std::vector<Orphan> o = findOrphans(ps, "PlayPlayChannelEngine.exe", "PlayPlaySupervisor.exe");
    QCOMPARE(int(o.size()), 3);
    QCOMPARE(o[0].pid, quint32(12)); QVERIFY(o[0].reason.contains("gone"));
    QCOMPARE(o[1].pid, quint32(13)); QVERIFY(o[1].reason.contains("newer"));
    QCOMPARE(o[2].pid, quint32(14)); QVERIFY(o[2].reason.contains("pwsh.exe"));

    // Log tails: last N lines, CRLF or LF, with or without a final newline, from the newest matching file.
    auto tmp = tempDir();
    const QString a = QDir(tmp->path()).filePath("supervisor-2026-10-25.log"), b = QDir(tmp->path()).filePath("supervisor-2026-10-26.log");
    { QFile f(a); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("old\n"); }
    QThread::msleep(30);
    {
        QFile f(b); QVERIFY(f.open(QIODevice::WriteOnly));
        for (int i = 0; i < 1000; ++i) f.write(QString("line %1 \xc3\xa9\r\n").arg(i).toUtf8());
        f.write("last line without newline");
    }
    QCOMPARE(newestMatch(QDir(tmp->path()).filePath("supervisor-*.log")), b);
    QCOMPARE(newestMatch(QDir(tmp->path()).filePath("nothing-*.log")), QString());
    QCOMPARE(newestMatch(a), a);
    QString err;
    QStringList t = tailLines(b, 500, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(t.size(), 500);
    QCOMPARE(t.last(), QString("last line without newline"));
    QCOMPARE(t.first(), QString::fromUtf8("line 501 \xc3\xa9"));
    t = tailLines(b, 5000, &err);
    QCOMPARE(t.size(), 1001);                                   // whole file
    QCOMPARE(t.first(), QString::fromUtf8("line 0 \xc3\xa9"));
    t = tailLines(b, 3, &err, 64);                             // byte cap: never a torn first line
    QVERIFY(t.size() <= 3);
    for (const QString& l : t) QVERIFY(l.startsWith("line") || l.startsWith("last"));
    QVERIFY(tailLines(QDir(tmp->path()).filePath("missing.log"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
#ifdef _WIN32
    // Read while another handle keeps the file open for writing (the logger does).
    QFile writer(b);
    QVERIFY(writer.open(QIODevice::Append));
    writer.write("\r\nappended\r\n");
    writer.flush();
    QCOMPARE(tailLines(b, 1, &err).value(0), QString("appended"));
#endif
}

void TestPpwatch::incidentFoldersRateLimitAndPrune() {
    auto tmp = tempDir();
    const QString root = tmp->path();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto mk = [&root](qint64 ms, const QString& name, int pid, bool dump) {
        const QString d = QDir(root).filePath(QString("%1-%2-%3").arg(utcStamp(ms), name).arg(pid));
        QDir().mkpath(d);
        if (dump) { QFile f(d + "/x.dmp"); f.open(QIODevice::WriteOnly); f.write("MDMP"); }
    };
    mk(now - 7200 * 1000, "supervisor", 1, true);   // two hours ago
    mk(now - 600 * 1000, "supervisor", 1, true);
    mk(now - 300 * 1000, "supervisor", 2, false);
    mk(now - 100 * 1000, "engine-main", 3, true);
    mk(now - 50 * 1000, "engine-main-2", 4, true);  // a name extending another is not counted for it
    QDir().mkpath(QDir(root).filePath("not-an-incident"));
    QCOMPARE(recentIncidents(root, "supervisor", 3600, false, now), 2);
    QCOMPARE(recentIncidents(root, "supervisor", 3600, true, now), 1);
    QCOMPARE(recentIncidents(root, "engine-main", 3600, true, now), 1);
    QCOMPARE(recentIncidents(root, "engine-main-2", 3600, false, now), 1);
    QCOMPARE(pruneIncidents(root, 3), 2);
    const QStringList left = incidentDirs(root);
    QCOMPARE(left.size(), 4);                         // 3 incidents + the foreign folder, which is never touched
    QVERIFY(left.contains("not-an-incident"));
    QCOMPARE(recentIncidents(root, "supervisor", 99999, false, now), 1);   // the oldest two went
    QCOMPARE(sanitizedName("engine main/1"), QString("engine_main_1"));
    QCOMPARE(utcStamp(0), QString("19700101T000000Z"));
}

void TestPpwatch::writeIncidentForAHelper() {
    auto tmp = tempDir();
    auto h = startHelper(QDir(tmp->path()).filePath("h"), {"--name", "direct", "--exit-after", "20000"});
    QVERIFY(h->pid && h->port);
    const QString logFile = QDir(tmp->path()).filePath("direct-2026-10-26.log");
    { QFile f(logFile); QVERIFY(f.open(QIODevice::WriteOnly)); for (int i = 0; i < 200; ++i) f.write(QByteArray("entry ") + QByteArray::number(i) + '\n'); }
    const ProcessId q = queryProcess(h->pid);
    QVERIFY(q.alive);
    QCOMPARE(q.image.toLower(), QString("hang_helper.exe"));

    IncidentRequest r;
    r.root = QDir(tmp->path()).filePath("incidents");
    r.target = "direct"; r.url = QString("http://127.0.0.1:%1/healthz").arg(h->port); r.role = "other";
    r.pid = h->pid; r.createTime = q.createTime; r.image = q.image; r.trigger = "hang"; r.episodeMs = 4200;
    r.lastHealth = QJsonObject{{"state", "hang"}, {"lagMs", 6100}, {"activity", "unit-test"}};
    r.logGlobs = {QDir(tmp->path()).filePath("direct-*.log"), QDir(tmp->path()).filePath("absent-*.log")};
    r.ports = {h->port};
    r.tailLines = 50;
    r.images = {"hang_helper.exe"};
    const IncidentResult res = writeIncident(r);
    QVERIFY2(res.ok, qPrintable(res.error));
    QVERIFY2(res.dumpError.isEmpty(), qPrintable(res.dumpError));
    QVERIFY(QFileInfo(res.dir).fileName().endsWith(QString("-direct-%1").arg(h->pid)));
    QCOMPARE(dumpProcessId(res.dumpPath), qint64(h->pid));
    const QJsonObject m = readJson(res.dir + "/incident.json");
    QCOMPARE(m.value("pid").toInt(), int(h->pid));
    QCOMPARE(m.value("trigger").toString(), QString("hang"));
    QVERIFY(m.value("summary").toString().contains("lag 6100 ms in 'unit-test'"));
    QVERIFY(m.value("process").toObject().value("threads").toInt() >= 3);
    QCOMPARE(m.value("process").toObject().value("parentImage").toString().toLower(), QString("test_ppwatch.exe"));
    QVERIFY(m.value("firstThread").toObject().value("first").toBool());
    QCOMPARE(m.value("logs").toArray().size(), 2);
    QCOMPARE(m.value("logs").toArray()[1].toObject().value("error").toString(), QString("no file matches"));
    const QStringList tail = readText(res.dir + "/" + m.value("logs").toArray()[0].toObject().value("file").toString()).split('\n', Qt::SkipEmptyParts);
    QCOMPARE(tail.size(), 50);
    QCOMPARE(tail.last(), QString("entry 199"));
    QVERIFY(readText(res.dir + "/threads.txt").contains("(first thread)"));
    const QString ns = readText(res.dir + "/netstat.txt");
    QVERIFY(ns.contains(QString("127.0.0.1:%1").arg(h->port)) && ns.contains("LISTEN") && ns.contains(QString::number(h->pid)));
    const QString ps = readText(res.dir + "/processes.txt");
    QVERIFY(ps.contains("hang_helper.exe") && ps.contains("test_ppwatch.exe"));

    // Pid reuse and wrong images are refused through the handle; the helper is untouched.
    QString err;
    QVERIFY(!dumpProcess(h->pid, q.createTime + 1, QDir(tmp->path()).filePath("x.dmp"), &err));
    QVERIFY2(err.contains("another process"), qPrintable(err));
    QVERIFY(!terminateProcess(h->pid, q.createTime, "PlayPlaySupervisor.exe", 0xDEAD, 1000, &err));
    QVERIFY2(err.contains("not PlayPlaySupervisor.exe"), qPrintable(err));
    QVERIFY(!terminateProcess(h->pid, q.createTime + 1, "hang_helper.exe", 0xDEAD, 1000, &err));
    QVERIFY(!terminateProcess(GetCurrentProcessId(), 0, QString(), 0xDEAD, 1000, &err));   // never itself
    QVERIFY(queryProcess(h->pid).alive);
    QCOMPARE(recentIncidents(r.root, "direct", 3600, true), 1);
    h.reset();   // ours: killed by the guard
}

void TestPpwatch::onceAndPrintConfig() {
    auto tmp = tempDir();
    auto h = startHelper(QDir(tmp->path()).filePath("h"), {"--name", "oncehelper", "--exit-after", "15000"});
    QVERIFY(h->pid && h->port);
    Run r = runWatchdog({"--once", "--target", QString("name=h,url=http://127.0.0.1:%1/healthz").arg(h->port)}, 15000);
    QVERIFY(r.finished);
    QCOMPARE(r.exitCode, 0);
    const QJsonObject s = QJsonDocument::fromJson(r.out).object().value("targets").toArray().first().toObject();
    QCOMPARE(s.value("state").toString(), QString("ok"));
    QCOMPARE(s.value("pid").toInt(), int(h->pid));
    QCOMPARE(s.value("health").toObject().value("name").toString(), QString("oncehelper"));
    const QJsonObject id = s.value("identity").toObject();
    QVERIFY(id.value("verified").toBool());
    QVERIFY(id.value("by").toArray().contains(QJsonValue("healthz-pid")));
    // Pinned to the wrong pid: not verified, and said why.
    r = runWatchdog({"--once", "--target", QString("name=h,url=http://127.0.0.1:%1/healthz,pid=%2").arg(h->port).arg(GetCurrentProcessId())}, 15000);
    const QJsonObject s2 = QJsonDocument::fromJson(r.out).object().value("targets").toArray().first().toObject();
    QVERIFY(!s2.value("identity").toObject().value("verified").toBool());
    QVERIFY(s2.value("identity").toObject().value("reason").toString().contains("not the configured pid"));
    h.reset();

    // The station preset (printed only: nothing is polled, nothing is written).
    r = runWatchdog({"--print-config", "--station", "--data-dir", QDir(tmp->path()).filePath("data"), "--app-log-dir", QDir(tmp->path()).filePath("app")}, 15000);
    QCOMPARE(r.exitCode, 0);
    const QJsonObject c = QJsonDocument::fromJson(r.out).object();
    const QJsonObject sup = c.value("targets").toArray().first().toObject();
    QCOMPARE(sup.value("url").toString(), QString("http://127.0.0.1:7709/healthz"));
    QCOMPARE(sup.value("exe").toString(), QString("PlayPlaySupervisor.exe"));
    QCOMPARE(sup.value("role").toString(), QString("supervisor"));
    QCOMPARE(c.value("restartHungSupervisorAfterSec").toInt(), 0);     // restart off by default
    QVERIFY(c.value("discoverEngines").toBool());
    QVERIFY(!c.value("dumpEngines").toBool());
    QVERIFY(c.value("out").toString().endsWith(QDir::toNativeSeparators("data/logs")));
    QVERIFY(!QFileInfo::exists(QDir(tmp->path()).filePath("data")));
    // Refusals: a remote host, a restart command without the opt-in, nothing to watch.
    QCOMPARE(runWatchdog({"--once", "--target", "url=http://192.168.1.177:7709/healthz"}, 15000).exitCode, 2);
    QCOMPARE(runWatchdog({"--target", "url=http://127.0.0.1:50099/healthz", "--restart-command", "x.exe"}, 15000).exitCode, 2);
    QCOMPARE(runWatchdog({"--print-config"}, 15000).exitCode, 2);
}

// The acceptance run: A blocks its main thread (its /healthz says hang), C is frozen whole (no answer at all), B blocks
// too but is reachable only through a target pinned to A's pid. Expect one bundle + dump for A and one for C, nothing for
// B, nobody killed, nothing on the console.
void TestPpwatch::hangAndUnresponsiveWriteBundlesOnlyForConfiguredPids() {
#ifndef _WIN32
    QSKIP("Windows only");
#else
    auto tmp = tempDir();
    const QString out = QDir(tmp->path()).filePath("out");
    const QString global = QDir(tmp->path()).filePath("global-2026-10-26.log");
    { QFile f(global); QVERIFY(f.open(QIODevice::WriteOnly)); for (int i = 0; i < 1000; ++i) f.write(QByteArray("global ") + QByteArray::number(i) + '\n'); }
    const QStringList hangArgs = {"--hang", "2500", "--dump", "0", "--block-at", "2500", "--block", "9000", "--exit-after", "15000"};
    auto a = startHelper(QDir(tmp->path()).filePath("a"), QStringList{"--name", "a"} + hangArgs);
    auto b = startHelper(QDir(tmp->path()).filePath("b"), QStringList{"--name", "b"} + hangArgs);
    auto c = startHelper(QDir(tmp->path()).filePath("c"), {"--name", "c", "--dump", "0", "--exit-after", "15000"});
    QVERIFY(a->port && b->port && c->port);

    QProcess w;
    w.setProgram(watchdogExe());
    w.setArguments({"--out", out, "--interval-ms", "600", "--timeout-ms", "450", "--run-for", "15", "--log", global,
                    "--target", QString("name=a,url=http://127.0.0.1:%1/healthz,exe=hang_helper.exe,logs=%2/watchdog-a-*.jsonl").arg(a->port).arg(a->dir),
                    "--target", QString("name=c,url=http://127.0.0.1:%1/healthz,pid=%2,logs=%3/watchdog-c-*.jsonl").arg(c->port).arg(c->pid).arg(c->dir),
                    "--target", QString("name=mismatch,url=http://127.0.0.1:%1/healthz,pid=%2").arg(b->port).arg(a->pid)});
    w.start();
    QVERIFY(w.waitForStarted(10000));
    {
        // C: frozen from ~2.5 s to ~10 s after the helpers started.
        QThread::msleep(1500);
        ProcessSuspender sus(c->pid);
        QVERIFY(sus.suspend());
        QThread::msleep(7500);
        sus.resume();
    }
    QVERIFY(w.waitForFinished(30000));
    QCOMPARE(w.exitCode(), 0);
    QVERIFY2(w.readAllStandardOutput().isEmpty(), "the watchdog wrote to stdout while watching");
    QVERIFY2(w.readAllStandardError().isEmpty(), "the watchdog wrote to stderr while watching");
    // Report only: every helper ran to its own end (exit code 0, result written).
    for (Helper* h : {a.get(), b.get(), c.get()}) {
        QVERIFY(h->finished(15000));
        QCOMPARE(h->p->exitCode(), 0);
        QVERIFY(QFileInfo::exists(h->dir + "/result.json"));
    }

    const QList<QJsonObject> ev = readEvents(out);
    QVERIFY(!eventsOf(ev, "start").isEmpty());
    QCOMPARE(eventsOf(ev, "stop").value(0).value("reason").toString(), QString("run-for"));
    QVERIFY(eventsOf(ev, "restart").isEmpty() && eventsOf(ev, "restart-due").isEmpty());
    auto states = [&ev](const QString& t) { QStringList l; for (const QJsonObject& o : eventsOf(ev, "state", t)) l << o.value("to").toString(); return l; };
    // Hung, then seen to recover (a helper may also have exited, "down", before the watchdog's run ended).
    const QStringList sa = states("a"), sc = states("c");
    QVERIFY2(sa.indexOf("hang") > 0 && sa.lastIndexOf("ok") > sa.indexOf("hang"), qPrintable(sa.join(',')));
    QVERIFY2(sc.indexOf("unresponsive") > 0 && sc.lastIndexOf("ok") > sc.indexOf("unresponsive"), qPrintable(sc.join(',')));
    // B hung behind a target pinned to A's pid: refused, with the reason.
    const QList<QJsonObject> refused = eventsOf(ev, "refused", "mismatch");
    QVERIFY2(!refused.isEmpty(), "no refusal for the mismatched target");
    QVERIFY2(refused.first().value("reason").toString().contains("not the configured pid"), qPrintable(refused.first().value("reason").toString()));
    QCOMPARE(refused.first().value("identity").toObject().value("lastVerifiedPid").toInt(), 0);   // B was never taken for the target
    QCOMPARE(eventsOf(ev, "incident").size(), 2);

    const QString root = out + "/incidents";
    const QStringList dirs = incidentDirs(root);
    QCOMPARE(dirs.size(), 2);
    QString aDir, cDir;
    for (const QString& d : dirs) {
        if (d.endsWith(QString("-a-%1").arg(a->pid))) aDir = root + "/" + d;
        if (d.endsWith(QString("-c-%1").arg(c->pid))) cDir = root + "/" + d;
    }
    QVERIFY2(!aDir.isEmpty() && !cDir.isEmpty(), qPrintable(dirs.join(' ')));
    // Only the configured pids were dumped: by file name and by the pid recorded inside each dump.
    for (const QString& d : allDumps(out)) {
        const qint64 p = dumpProcessId(d);
        QVERIFY2(p == a->pid || p == c->pid, qPrintable(QString("%1 holds pid %2").arg(d).arg(p)));
        QVERIFY(!d.contains(QString("-%1-").arg(b->pid)));
    }

    const QJsonObject am = readJson(aDir + "/incident.json");
    QCOMPARE(am.value("trigger").toString(), QString("hang"));
    QCOMPARE(am.value("pid").toInt(), int(a->pid));
    QCOMPARE(am.value("health").toObject().value("state").toString(), QString("hang"));
    QCOMPARE(am.value("health").toObject().value("activity").toString(), QString("test-block"));
    QVERIFY(am.value("identity").toObject().value("verified").toBool());
    const QJsonArray by = am.value("identity").toObject().value("by").toArray();
    QVERIFY(by.contains(QJsonValue("exe")) && by.contains(QJsonValue("healthz-pid")));
    QVERIFY(!am.value("dump").toString().isEmpty());
    QCOMPARE(dumpProcessId(aDir + "/" + am.value("dump").toString()), qint64(a->pid));
    QVERIFY(am.value("polls").toArray().size() >= 3);
    // The blocked main thread (Sleep in hang_helper) shows its wait reason.
    QCOMPARE(am.value("firstThread").toObject().value("waitReason").toString(), QString("DelayExecution"));
    QVERIFY(readText(aDir + "/netstat.txt").contains(QString("127.0.0.1:%1").arg(a->port)));
    bool sawStall = false, sawGlobal = false;
    for (const QJsonValue& v : am.value("logs").toArray()) {
        const QString text = readText(aDir + "/" + v.toObject().value("file").toString());
        sawStall = sawStall || text.contains("\"event\":\"stall\"");
        if (v.toObject().value("source").toString().endsWith("global-2026-10-26.log")) {
            const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
            sawGlobal = lines.size() == 500 && lines.last() == "global 999";
        }
    }
    QVERIFY(sawStall);
    QVERIFY(sawGlobal);

    const QJsonObject cm = readJson(cDir + "/incident.json");
    QCOMPARE(cm.value("trigger").toString(), QString("unresponsive"));
    QVERIFY(cm.value("identity").toObject().value("by").toArray().contains(QJsonValue("pid")));
    QCOMPARE(dumpProcessId(cDir + "/" + cm.value("dump").toString()), qint64(c->pid));
    QVERIFY2(readText(cDir + "/threads.txt").contains("Suspended"), "the frozen process's threads should read Suspended");
    // Frozen whole: the port still accepts (the kernel completes the handshake), nothing answers.
    bool replyTimeout = false;
    for (const QJsonValue& v : cm.value("polls").toArray())
        if (v.toObject().value("reach").toString() == "no-answer") replyTimeout = replyTimeout || v.toObject().value("kind").toString() == "reply-timeout";
    QVERIFY(replyTimeout);
#endif
}

void TestPpwatch::restartIsOptInAndScoped() {
#ifndef _WIN32
    QSKIP("Windows only");
#else
    auto tmp = tempDir();
    const QString out = QDir(tmp->path()).filePath("out");
    const QString relaunchDir = QDir(tmp->path()).filePath("relaunched");
    auto s = startHelper(QDir(tmp->path()).filePath("s"), {"--name", "sup", "--hang", "2000", "--dump", "0", "--block-at", "1500", "--block", "30000", "--exit-after", "40000"});
    // A bystander that hangs at the same time and is not configured.
    auto x = startHelper(QDir(tmp->path()).filePath("x"), {"--name", "bystander", "--hang", "2000", "--dump", "0", "--block-at", "1500", "--block", "7000", "--exit-after", "10000"});
    QVERIFY(s->port && x->port);
    const QString cmd = QString("\"%1\" --dir \"%2\" --name relaunched --exit-after 1500").arg(QDir::toNativeSeparators(helperExe()), QDir::toNativeSeparators(relaunchDir));
    QElapsedTimer t; t.start();
    const Run r = runWatchdog({"--out", out, "--interval-ms", "500", "--timeout-ms", "400", "--run-for", "11",
                               "--target", QString("name=sup,url=http://127.0.0.1:%1/healthz,role=supervisor,exe=hang_helper.exe").arg(s->port),
                               "--restart-hung-supervisor-after", "2", "--restart-command", cmd}, 30000);
    QVERIFY(r.finished);
    QCOMPARE(r.exitCode, 0);
    QVERIFY(r.out.isEmpty() && r.err.isEmpty());
    // The hung "supervisor" was terminated with the watchdog's code, after its incident; the bystander was not touched.
    QVERIFY(s->finished(5000));
    QCOMPARE(quint32(s->p->exitCode()), 0xDEADu);
    QVERIFY(!QFileInfo::exists(s->dir + "/result.json"));
    QVERIFY(x->finished(15000));
    QCOMPARE(x->p->exitCode(), 0);
    QVERIFY(QFileInfo::exists(x->dir + "/result.json"));
    const QList<QJsonObject> ev = readEvents(out);
    const QList<QJsonObject> restart = eventsOf(ev, "restart", "sup");
    QCOMPARE(restart.size(), 1);
    QVERIFY2(restart.first().value("ok").toBool(), qPrintable(restart.first().value("error").toString()));
    QCOMPARE(restart.first().value("pid").toInt(), int(s->pid));
    QVERIFY(restart.first().value("newPid").toInt() > 0);
    QCOMPARE(eventsOf(ev, "incident", "sup").size(), 1);
    // Order: the incident (with its dump) is on disk before the kill.
    int iIncident = -1, iRestart = -1;
    for (int i = 0; i < ev.size(); ++i) {
        if (ev[i].value("event").toString() == "incident") iIncident = i;
        if (ev[i].value("event").toString() == "restart") iRestart = i;
    }
    QVERIFY(iIncident >= 0 && iIncident < iRestart);
    const QJsonObject inc = eventsOf(ev, "incident", "sup").first();
    QVERIFY(!inc.value("dump").toString().isEmpty());
    QCOMPARE(dumpProcessId(inc.value("dump").toString()), qint64(s->pid));
    // The restart command ran detached and did its job.
    QElapsedTimer wait; wait.start();
    while (!QFileInfo::exists(relaunchDir + "/result.json") && wait.elapsed() < 10000) QThread::msleep(100);
    QVERIFY2(QFileInfo::exists(relaunchDir + "/result.json"), "the restart command did not run");
    // The relaunched helper ended by itself (--exit-after 1500); make sure (it is ours).
    const quint32 np = quint32(restart.first().value("newPid").toInt());
    if (queryProcess(np).alive && sameImage(queryProcess(np).image, "hang_helper.exe")) {
        if (HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, np)) { TerminateProcess(hp, 1); CloseHandle(hp); }
    }
#endif
}

QTEST_GUILESS_MAIN(TestPpwatch)
#include "test_ppwatch.moc"
