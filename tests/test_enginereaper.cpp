#include <QtTest/QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include "playout/core/enginelock.h"
#include "playout/schedule/schedulemodel.h"
#include "playout/supervisor/channelmanager.h"
#include "playout/supervisor/enginereaper.h"
#include "playout/supervisor/singleinstance.h"
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

using namespace reaper;

// PS-4a (W6): the engine reaper core. decide() / decideAll() are table-tested; the scan runs against a temporary data
// dir with hand-written engine.lock files and against child processes of this binary in "--sleep-child" mode whose
// command lines look like an engine's (--id / --data / --supervisor). The process-list match uses this binary's own exe
// name, so no real PlayPlayChannelEngine is ever looked at. IPC names are "ppreap-<pid>-<n>"; nothing uses the live
// name PlayPlaySupervisor or the live data dir. SingleInstance is checked with a thread and a child process.
class TestEngineReaper : public QObject {
    Q_OBJECT

private slots:
    void decideTable_data();
    void decideTable();
    void decideAllKeepsOnePerChannel();
    void scopeHelpers();
    void splitCommandLineRules_data();
    void splitCommandLineRules();
    void scanLockFiles();
    void scanCommandLinesScoped();
    void singleInstanceRefusesSecond();
    void guardDecideTable();
    void guardFlowHealthyHungReplace();
    void wiringAdoptReapEndToEnd();
    void cleanup();

private:
    QList<QProcess*> m_children;
    QList<qint64> m_enginePids;      // fake engines started by this test or by its --sup-child Supervisors
    QString m_fakeCopy;              // their image (only processes with this image are ever killed)
    std::unique_ptr<QTemporaryDir> m_tmp;   // removed in cleanup(), after the engines running from it are gone
    QProcess* startChild(const QStringList& args);
};

namespace {

QString uniqueIpc() {
    static int n = 0;
    return QString("ppreap-%1-%2").arg(QCoreApplication::applicationPid()).arg(++n);
}

constexpr quint64 kTicksPerMs = 10000ULL;
constexpr quint64 kEpochTicks = 116444736000000000ULL;   // 1970-01-01 in FILETIME ticks
quint64 ticksForMs(qint64 ms) { return kEpochTicks + quint64(ms) * kTicksPerMs; }

sched::ChannelProfile profile(const QString& id, bool enabled = true) {
    sched::ChannelProfile p; p.id = id; p.enabled = enabled; return p;
}

EngineFacts engine(const QString& ch, qint64 pid, const QString& exe, qint64 exeMtime, qint64 startedMs) {
    EngineFacts f;
    f.channelId = ch; f.pid = pid; f.exe = exe; f.exeMtimeMs = exeMtime; f.createTime = ticksForMs(startedMs);
    f.fromLock = exeMtime > 0; f.fromCommandLine = exeMtime == 0;
    return f;
}

} // namespace

QProcess* TestEngineReaper::startChild(const QStringList& args) {
    auto* p = new QProcess;
    p->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    p->start(QCoreApplication::applicationFilePath(), QStringList{"--sleep-child"} + args);
    m_children << p;
    return p->waitForStarted(5000) ? p : nullptr;
}

void TestEngineReaper::cleanup() {
    // Only the processes this test started (kill by our own QProcess handles).
    for (QProcess* p : m_children) {
        if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(3000); }
        delete p;
    }
    m_children.clear();
#ifdef Q_OS_WIN
    // Fake engines spawned by the --sup-child Supervisors (their grandchildren): only pids this test recorded, and only
    // while the pid is still a process running our private copy of fake_engine.exe.
    for (qint64 pid : std::as_const(m_enginePids)) {
        if (m_fakeCopy.isEmpty() || !enginelock::processAlive(pid)) continue;
        if (QDir::cleanPath(QDir::fromNativeSeparators(enginelock::processImagePath(pid))).compare(QDir::cleanPath(m_fakeCopy), Qt::CaseInsensitive) != 0) continue;
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid))) { TerminateProcess(h, 9); WaitForSingleObject(h, 3000); CloseHandle(h); }
    }
#endif
    m_enginePids.clear();
    m_tmp.reset();
}

// --- decide() -------------------------------------------------------------------------------------------------------

void TestEngineReaper::decideTable_data() {
    QTest::addColumn<QString>("channel");        // "" = unknown, "!" prefix = disabled
    QTest::addColumn<QString>("exe");
    QTest::addColumn<qint64>("exeMtime");        // from the lock; 0 = no lock
    QTest::addColumn<qint64>("startedMs");
    QTest::addColumn<bool>("adoptEnabled");
    QTest::addColumn<bool>("duplicate");
    QTest::addColumn<int>("verdict");
    QTest::addColumn<int>("reason");

    const QString exe = "D:\\pp\\build\\PlayPlayChannelEngine.exe";
    const qint64 built = 1'790'000'000'000LL, started = built + 60'000;
    const int A = int(Verdict::Adopt), K = int(Verdict::Kill);
    QTest::newRow("adopt") << "ch2" << exe << built << started << true << false << A << int(Reason::None);
    QTest::newRow("adopt, path spelled differently") << "ch2" << "d:/PP/Build/playplaychannelengine.exe" << built << started << true << false << A << int(Reason::None);
    QTest::newRow("unknown channel") << "" << exe << built << started << true << false << K << int(Reason::UnknownChannel);
    QTest::newRow("disabled channel") << "!ch2" << exe << built << started << true << false << K << int(Reason::Disabled);
    QTest::newRow("build mtime mismatch") << "ch2" << exe << built - 1 << started << true << false << K << int(Reason::BuildMismatch);
    QTest::newRow("build exe mismatch") << "ch2" << "D:\\pp\\build-old\\PlayPlayChannelEngine.exe" << built << started << true << false << K << int(Reason::BuildMismatch);
    QTest::newRow("duplicate") << "ch2" << exe << built << started << true << true << K << int(Reason::Duplicate);
    QTest::newRow("adopt disabled") << "ch2" << exe << built << started << false << false << K << int(Reason::AdoptDisabled);
    QTest::newRow("unknown before build") << "" << "C:\\x.exe" << qint64(1) << started << false << true << K << int(Reason::UnknownChannel);
    QTest::newRow("build before duplicate") << "ch2" << exe << built + 5 << started << true << true << K << int(Reason::BuildMismatch);
    QTest::newRow("lockless, exe older than process") << "ch2" << exe << qint64(0) << started << true << false << A << int(Reason::None);
    QTest::newRow("lockless, exe replaced after start") << "ch2" << exe << qint64(0) << built - 1000 << true << false << K << int(Reason::BuildMismatch);
    QTest::newRow("lockless, start unknown") << "ch2" << exe << qint64(0) << qint64(0) << true << false << K << int(Reason::BuildMismatch);
}

void TestEngineReaper::decideTable() {
    QFETCH(QString, channel); QFETCH(QString, exe); QFETCH(qint64, exeMtime); QFETCH(qint64, startedMs);
    QFETCH(bool, adoptEnabled); QFETCH(bool, duplicate); QFETCH(int, verdict); QFETCH(int, reason);
    const BuildFacts current{"D:\\pp\\build\\PlayPlayChannelEngine.exe", 1'790'000'000'000LL};
    EngineFacts f = engine(channel.isEmpty() ? "ghost" : channel.mid(channel.startsWith('!') ? 1 : 0), 15704, exe, exeMtime, startedMs);
    if (!startedMs) f.createTime = 0;
    const sched::ChannelProfile p = profile(f.channelId, !channel.startsWith('!'));
    const ReapDecision d = decide(f, channel.isEmpty() ? nullptr : &p, current, adoptEnabled, duplicate);
    QCOMPARE(int(d.verdict), verdict);
    QCOMPARE(int(d.reason), reason);
    QCOMPARE(d.facts.pid, qint64(15704));
    QVERIFY(d.text.contains("pid 15704"));
    if (d.verdict == Verdict::Kill) QVERIFY2(d.text.startsWith("reaped orphan engine") && d.text.contains(reasonName(d.reason)), qPrintable(d.text));
    else QVERIFY2(d.text.startsWith("adopting orphan engine"), qPrintable(d.text));
}

void TestEngineReaper::decideAllKeepsOnePerChannel() {
    const BuildFacts current{"C:\\b\\PlayPlayChannelEngine.exe", 1'790'000'000'000LL};
    const qint64 m = current.exeMtimeMs, t0 = m + 10'000;
    sched::ChannelProfile main = profile("main"), ch2 = profile("ch2"), off = profile("off", false);
    const ProfileLookup lookup = [&](const QString& id) -> const sched::ChannelProfile* {
        if (id == "main") return &main;
        if (id == "ch2") return &ch2;
        if (id == "off") return &off;
        return nullptr;
    };
    auto byPid = [](const QVector<ReapDecision>& ds, qint64 pid) {
        for (const ReapDecision& d : ds) if (d.facts.pid == pid) return d;
        return ReapDecision{};
    };

    // main: three engines. 101 oldest, 102 ON_AIR (younger), 103 build mismatch. ch2: one. off: disabled. ghost: unknown.
    QVector<EngineFacts> es;
    es << engine("main", 101, current.exe, m, t0) << engine("main", 102, current.exe, m, t0 + 5000)
       << engine("main", 103, current.exe, m - 1, t0) << engine("ch2", 201, current.exe, m, t0)
       << engine("off", 301, current.exe, m, t0) << engine("ghost", 401, current.exe, m, t0);
    es[1].state = "ON_AIR";
    QVector<ReapDecision> ds = decideAll(es, lookup, current, true);
    QCOMPARE(ds.size(), es.size());
    QCOMPARE(int(byPid(ds, 102).verdict), int(Verdict::Adopt));            // ON_AIR stays though younger
    QCOMPARE(int(byPid(ds, 101).reason), int(Reason::Duplicate));
    QCOMPARE(int(byPid(ds, 103).reason), int(Reason::BuildMismatch));      // its own reason, not "duplicate"
    QCOMPARE(int(byPid(ds, 201).verdict), int(Verdict::Adopt));
    QCOMPARE(int(byPid(ds, 301).reason), int(Reason::Disabled));
    QCOMPARE(int(byPid(ds, 401).reason), int(Reason::UnknownChannel));
    int adopted = 0;
    for (const ReapDecision& d : ds) adopted += d.verdict == Verdict::Adopt;
    QCOMPARE(adopted, 2);                                                   // never two per channel

    // No ON_AIR: the earliest HELLO stays; without HELLOs, the oldest process. Order of the input does not matter.
    QVector<EngineFacts> two;
    two << engine("main", 501, current.exe, m, t0 + 1) << engine("main", 502, current.exe, m, t0);
    ds = decideAll(two, lookup, current, true);
    QCOMPARE(int(byPid(ds, 502).verdict), int(Verdict::Adopt));
    QCOMPARE(int(byPid(ds, 501).reason), int(Reason::Duplicate));
    two[0].helloAtMs = 1000; two[1].helloAtMs = 2000;
    ds = decideAll(two, lookup, current, true);
    QCOMPARE(int(byPid(ds, 501).verdict), int(Verdict::Adopt));
    QCOMPARE(int(byPid(ds, 502).reason), int(Reason::Duplicate));
    std::reverse(two.begin(), two.end());
    ds = decideAll(two, lookup, current, true);
    QCOMPARE(int(byPid(ds, 501).verdict), int(Verdict::Adopt));

    // Adoption switched off: everything is killed, nothing adopted.
    ds = decideAll(es, lookup, current, false);
    for (const ReapDecision& d : ds) QCOMPARE(int(d.verdict), int(Verdict::Kill));
    QCOMPARE(int(byPid(ds, 102).reason), int(Reason::AdoptDisabled));
    // No lookup = every channel unknown.
    ds = decideAll(es, nullptr, current, true);
    for (const ReapDecision& d : ds) QCOMPARE(int(d.reason), int(Reason::UnknownChannel));
}

// --- scope helpers --------------------------------------------------------------------------------------------------

void TestEngineReaper::scopeHelpers() {
    QCOMPARE(normalizeIpcName("\\\\.\\pipe\\PlayPlayLaneTest"), QString("playplaylanetest"));
    QVERIFY(sameIpc("PlayPlayLaneTest", "\\\\.\\PIPE\\playplaylanetest"));
    QVERIFY(!sameIpc("PlayPlayLaneTest", "PlayPlayLaneTest2"));
    QVERIFY(!sameIpc("", ""));                                               // no name = nothing is in scope

    QVERIFY(pathUnder("C:\\Data\\Sup\\channels\\main", "c:/data/sup"));
    QVERIFY(pathUnder("C:/data/sup/", "C:\\data\\sup"));
    QVERIFY(pathUnder("C:/data/sup/channels/../channels/x", "C:/data/sup"));
    QVERIFY(!pathUnder("C:/data/sup2/channels/main", "C:/data/sup"));       // a sibling with the same prefix
    QVERIFY(!pathUnder("C:/data/sup/../other/channels/x", "C:/data/sup"));
    QVERIFY(!pathUnder("C:/data", "C:/data/sup"));
    QVERIFY(!pathUnder("", "C:/data"));

    EngineArgs a = parseEngineArgs({"PlayPlayChannelEngine.exe", "--id", "ch2", "--name", "Ch 2", "--port", "0",
                                    "--data", "D:\\x\\channels\\ch2", "--config", "c.json"});
    QCOMPARE(a.id, QString("ch2"));
    QCOMPARE(a.data, QString("D:\\x\\channels\\ch2"));
    QCOMPARE(a.supervisor, QString("PlayPlaySupervisor"));                  // engine_main's default
    QVERIFY(!a.supervisorGiven); QVERIFY(!a.standalone);
    a = parseEngineArgs({"e.exe", "--id=perftest-a", "--data=C:/t/channels/a", "--supervisor=ppreap-1", "--standalone"});
    QCOMPARE(a.id, QString("perftest-a"));
    QCOMPARE(a.data, QString("C:/t/channels/a"));
    QCOMPARE(a.supervisor, QString("ppreap-1"));
    QVERIFY(a.supervisorGiven); QVERIFY(a.standalone);
    a = parseEngineArgs({"e.exe", "--supervisor", "", "--id"});              // trailing option without a value
    QVERIFY(a.supervisor.isEmpty()); QVERIFY(a.id.isEmpty());

    QCOMPARE(SingleInstance::mutexName("\\\\.\\pipe\\PlayPlayLaneTest"), QString("Local\\PlayPlaySupervisor-playplaylanetest"));
    QCOMPARE(SingleInstance::mutexName("PlayPlayLaneTest"), SingleInstance::mutexName("playplaylanetest"));
    QVERIFY(SingleInstance::mutexName("a\\b").endsWith("-a_b"));
}

void TestEngineReaper::splitCommandLineRules_data() {
    QTest::addColumn<QString>("line");
    QTest::addColumn<QStringList>("args");
    QTest::newRow("plain") << "e.exe --id ch2  --data\tD:\\x" << QStringList{"e.exe", "--id", "ch2", "--data", "D:\\x"};
    QTest::newRow("quoted exe and spaces") << "\"C:\\Program Files\\PP\\e.exe\" --data \"D:\\My Data\\channels\\a b\""
                                           << QStringList{"C:\\Program Files\\PP\\e.exe", "--data", "D:\\My Data\\channels\\a b"};
    QTest::newRow("trailing backslash in quotes") << "e.exe --data \"D:\\dir with space\\\\\" --id x"
                                                  << QStringList{"e.exe", "--data", "D:\\dir with space\\", "--id", "x"};
    QTest::newRow("escaped quote") << "e.exe a\\\"b \"c\"\"d\"" << QStringList{"e.exe", "a\"b", "c\"d"};
    QTest::newRow("empty argument") << "e.exe --supervisor \"\" --id x" << QStringList{"e.exe", "--supervisor", "", "--id", "x"};
    QTest::newRow("backslashes not before a quote") << "e.exe \\\\server\\share\\x" << QStringList{"e.exe", "\\\\server\\share\\x"};
}

void TestEngineReaper::splitCommandLineRules() {
    QFETCH(QString, line); QFETCH(QStringList, args);
    QCOMPARE(splitCommandLine(line), args);
}

// --- scan -----------------------------------------------------------------------------------------------------------

void TestEngineReaper::scanLockFiles() {
#ifndef Q_OS_WIN
    QSKIP("Windows only");
#endif
    QTemporaryDir tmp, other;
    QVERIFY(tmp.isValid() && other.isValid());
    const QString root = tmp.path(), ipc = uniqueIpc();

    QProcess* live = startChild({"--id", "perftest-live"});                  // no engine-like args: found by lock only
    QVERIFY(live);
    QProcess* gone = startChild({"--exit-now"});
    QVERIFY(gone);
    const qint64 deadPid = gone->processId();                               // 0 once QProcess saw it finish
    QVERIFY(deadPid > 0 && gone->waitForFinished(5000));
    const qint64 pid = live->processId();
    const quint64 ct = enginelock::processCreateTime(pid);
    QVERIFY(ct > 0);
    const QString exe = enginelock::processImagePath(pid);
    QVERIFY(!exe.isEmpty());

    auto lockFor = [&](const QString& ch) {
        enginelock::LockInfo li;
        li.channelId = ch; li.pid = pid; li.createTime = ct; li.exe = exe; li.exeMtimeMs = 1234; li.configHash = "abc";
        li.supervisorIpc = ipc; li.supervisorPid = 999999; li.dataDir = QDir(root).filePath("channels/" + ch);
        return li;
    };
    auto write = [&](const QString& ch, const enginelock::LockInfo& li) {
        QVERIFY(enginelock::writeLock(enginelock::lockPath(QDir(root).filePath("channels/" + ch)), li));
    };
    write("perftest-a", lockFor("perftest-a"));                                        // valid: selected
    { auto li = lockFor("perftest-b"); li.createTime = ct + 1; write("perftest-b", li); }   // pid reused -> stale
    { auto li = lockFor("perftest-c"); li.pid = deadPid; write("perftest-c", li); }    // process gone -> stale
    { auto li = lockFor("perftest-d"); li.supervisorIpc = ipc + "-other"; write("perftest-d", li); }   // other IPC
    { auto li = lockFor("perftest-e"); li.dataDir = QDir(other.path()).filePath("channels/perftest-e"); write("perftest-e", li); }   // other data dir
    { auto li = lockFor("perftest-f"); li.exe = "C:\\nowhere\\PlayPlayChannelEngine.exe"; write("perftest-f", li); }   // other image -> stale
    { auto li = lockFor("perftest-g"); li.supervisorIpc.clear(); write("perftest-g", li); }   // no IPC: never ours
    QDir().mkpath(QDir(root).filePath("channels/perftest-h"));
    { QFile f(enginelock::lockPath(QDir(root).filePath("channels/perftest-h"))); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{not json"); }

    ScanOptions opt;
    opt.dataDir = root; opt.ipcName = ipc; opt.useProcessList = false;
    const ScanResult r = scan(opt);
    QCOMPARE(r.engines.size(), 1);
    const EngineFacts& f = r.engines.first();
    QCOMPARE(f.channelId, QString("perftest-a"));
    QCOMPARE(f.pid, pid);
    QCOMPARE(f.createTime, ct);
    QCOMPARE(f.exeMtimeMs, qint64(1234));
    QCOMPARE(f.configHash, QString("abc"));
    QCOMPARE(f.supervisorPid, qint64(999999));
    QVERIFY(f.fromLock && !f.fromCommandLine);
    QStringList stale;
    for (const QString& s : r.staleLocks) stale << QFileInfo(s).dir().dirName();
    stale.sort();
    QCOMPARE(stale, (QStringList{"perftest-b", "perftest-c", "perftest-f"}));
    const QString notes = r.notes.join('\n');
    QVERIFY2(notes.contains("perftest-d") && notes.contains("perftest-e") && notes.contains("perftest-g") && notes.contains("perftest-h"), qPrintable(notes));

    // The same tree seen by another Supervisor scope (other IPC, or another data dir) selects nothing.
    opt.ipcName = ipc + "-x";
    QVERIFY(scan(opt).engines.isEmpty());
    opt.ipcName = ipc; opt.dataDir = other.path();
    QVERIFY(scan(opt).engines.isEmpty());
    opt.ipcName.clear(); opt.dataDir = root;
    QVERIFY(scan(opt).engines.isEmpty());
}

void TestEngineReaper::scanCommandLinesScoped() {
#ifndef Q_OS_WIN
    QSKIP("Windows only");
#endif
    QTemporaryDir tmp, other;
    QVERIFY(tmp.isValid() && other.isValid());
    const QString root = tmp.path(), ipc = uniqueIpc();
    auto chDir = [&](const QString& base, const QString& ch) { return QDir::toNativeSeparators(QDir(base).filePath("channels/" + ch)); };

    // Engine-like command lines (QProcess quotes them like ChannelManager's spawn does).
    QProcess* m = startChild({"--id", "perftest-m", "--name", "M", "--data", chDir(root, "perftest-m"), "--supervisor", ipc});
    QProcess* q = startChild({"--id=perftest-q", "--data=" + chDir(root, "perftest q spaced"), "--supervisor=\\\\.\\pipe\\" + ipc.toUpper()});
    QProcess* o = startChild({"--id", "perftest-o", "--data", chDir(root, "perftest-o"), "--supervisor", ipc + "-other"});   // other IPC
    QProcess* x = startChild({"--id", "perftest-x", "--data", chDir(other.path(), "perftest-x"), "--supervisor", ipc});      // other data dir
    QProcess* p = startChild({"--id", "perftest-p", "--data", chDir(root + "2", "perftest-p"), "--supervisor", ipc});        // prefix sibling
    QProcess* s = startChild({"--id", "perftest-s", "--data", chDir(root, "perftest-s"), "--supervisor", ipc, "--standalone"});
    QProcess* r = startChild({"--id", "perftest-r", "--data", "channels/perftest-r", "--supervisor", ipc});                  // relative data
    QProcess* d = startChild({"--id", "perftest-d", "--data", chDir(root, "perftest-d")});                                  // default IPC (not ours)
    QProcess* l = startChild({"--id", "perftest-l", "--data", chDir(root, "perftest-l"), "--supervisor", ipc});             // has a lock too
    for (QProcess* c : {m, q, o, x, p, s, r, d, l}) QVERIFY(c);

    enginelock::LockInfo li;
    li.channelId = "perftest-l"; li.pid = l->processId(); li.createTime = enginelock::processCreateTime(li.pid);
    li.exe = enginelock::processImagePath(li.pid); li.exeMtimeMs = QFileInfo(li.exe).lastModified().toMSecsSinceEpoch();
    li.supervisorIpc = ipc; li.dataDir = chDir(root, "perftest-l");
    QVERIFY(enginelock::writeLock(enginelock::lockPath(li.dataDir), li));

    const QString cl = processCommandLine(m->processId());
    QVERIFY2(cl.contains("perftest-m") && cl.contains(ipc), qPrintable(cl));
    QVERIFY(listProcesses(QFileInfo(QCoreApplication::applicationFilePath()).fileName()).size() >= 10);   // 9 children + us

    ScanOptions opt;
    opt.dataDir = root; opt.ipcName = ipc;
    opt.engineExeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    const ScanResult res = scan(opt);
    QHash<qint64, EngineFacts> got;
    for (const EngineFacts& f : res.engines) { QVERIFY2(!got.contains(f.pid), "an engine selected twice"); got.insert(f.pid, f); }
    QCOMPARE(QSet<qint64>(got.keyBegin(), got.keyEnd()), (QSet<qint64>{m->processId(), q->processId(), l->processId()}));
    QVERIFY(!got.contains(QCoreApplication::applicationPid()));
    const EngineFacts fm = got.value(m->processId()), fq = got.value(q->processId()), fl = got.value(l->processId());
    QCOMPARE(fm.channelId, QString("perftest-m"));
    QVERIFY(fm.fromCommandLine && !fm.fromLock);
    QCOMPARE(fm.exeMtimeMs, qint64(0));
    QCOMPARE(fm.createTime, enginelock::processCreateTime(m->processId()));
    QCOMPARE(fq.channelId, QString("perftest-q"));
    QVERIFY(pathUnder(fq.dataDir, root) && fq.dataDir.contains("perftest q spaced"));
    QVERIFY(fl.fromLock && !fl.fromCommandLine);
    QVERIFY2(res.notes.join('\n').contains(QString::number(r->processId())), qPrintable(res.notes.join('\n')));

    // Fed to decideAll with this binary as "the current build": the lockless ones are adoptable (their exe is older
    // than their start), the locked one by its exact mtime; a channel nobody knows is killed.
    sched::ChannelProfile pm = profile("perftest-m"), pl = profile("perftest-l");
    const ProfileLookup lookup = [&](const QString& id) -> const sched::ChannelProfile* {
        return id == "perftest-m" ? &pm : id == "perftest-l" ? &pl : nullptr;
    };
    const QString self = QCoreApplication::applicationFilePath();
    const BuildFacts current{QDir::toNativeSeparators(self), QFileInfo(self).lastModified().toMSecsSinceEpoch()};
    for (const ReapDecision& dd : decideAll(res.engines, lookup, current, true)) {
        if (dd.facts.pid == q->processId()) QCOMPARE(int(dd.reason), int(Reason::UnknownChannel));
        else QVERIFY2(dd.verdict == Verdict::Adopt, qPrintable(dd.text));
    }

    // The mandatory scope rule from the other side: a Supervisor on another IPC name or data dir selects none of them.
    opt.ipcName = ipc + "-nobody";
    QVERIFY(scan(opt).engines.isEmpty());
    opt.ipcName = ipc; opt.dataDir = other.path();
    const ScanResult otherScope = scan(opt);                                  // only the engine that lives there
    QCOMPARE(otherScope.engines.size(), 1);
    QCOMPARE(otherScope.engines.first().pid, x->processId());
}

// --- SingleInstance -------------------------------------------------------------------------------------------------

void TestEngineReaper::singleInstanceRefusesSecond() {
#ifndef Q_OS_WIN
    QSKIP("Windows only");
#endif
    const QString ipc = uniqueIpc();
    {
        SingleInstance first(ipc);
        QVERIFY(first.tryAcquire());
        QVERIFY(first.owned());
        bool second = true, sameNameOtherCase = true, otherName = false;
        // A mutex is recursive for its owning thread, so the "second Supervisor" runs on another thread.
        std::thread t([&]() {
            SingleInstance b(ipc); second = b.tryAcquire(200);
            SingleInstance c("\\\\.\\pipe\\" + ipc.toUpper()); sameNameOtherCase = c.tryAcquire(0);
            SingleInstance d(ipc + "-other"); otherName = d.tryAcquire(0);
        });
        t.join();
        QVERIFY(!second);
        QVERIFY(!sameNameOtherCase);
        QVERIFY(otherName);
    }
    // Released with its owner: the next one gets it at once.
    {
        bool got = false;
        std::thread t([&]() { SingleInstance b(ipc); got = b.tryAcquire(0); });
        t.join();
        QVERIFY(got);
    }
    // Another process holds it: refused; that process is killed (no release): the next one gets it, flagged abandoned.
    QProcess* holder = startChild({"--hold-instance", ipc});
    QVERIFY(holder);
    QByteArray said;
    for (int i = 0; i < 50 && !said.contains("HELD"); ++i) { holder->waitForReadyRead(100); said += holder->readAllStandardOutput(); }
    QVERIFY2(said.contains("HELD"), said.constData());
    SingleInstance mine(ipc);
    QVERIFY(!mine.tryAcquire(300));
    holder->kill();
    QVERIFY(holder->waitForFinished(5000));
    QVERIFY(mine.tryAcquire(2000));
    QVERIFY(mine.abandoned());
}

// --- supervisor_main's guard flow (PS-4b) ---------------------------------------------------------------------------

void TestEngineReaper::guardDecideTable() {
    using supguard::Action;
    supguard::Health none, refused, closed, silent, ok, exiting, hang, stall, odd;
    ok.answered = exiting.answered = hang.answered = stall.answered = odd.answered = true;
    ok.connected = exiting.connected = hang.connected = stall.connected = odd.connected = true;
    ok.state = "ok"; exiting.state = "exiting"; hang.state = "hang"; stall.state = "stall"; odd.state = "";
    closed.connected = true;                         // took the connection, closed it without a /healthz reply
    silent.connected = true; silent.timedOut = true; // took the connection, never answered: its watchdog is stuck
    Q_UNUSED(refused);                               // nothing listens (another port, or /healthz off) == none
    QCOMPARE(supguard::decide(true, hang, false), Action::Run);              // the mutex was free: nothing else matters
    QCOMPARE(supguard::decide(false, ok, true), Action::ExitRunning);        // healthy: never replaced
    QCOMPARE(supguard::decide(false, exiting, true), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, hang, false), Action::ExitRunning);     // hung, but no --replace-hung
    QCOMPARE(supguard::decide(false, hang, true), Action::Replace);
    QCOMPARE(supguard::decide(false, stall, true), Action::Replace);         // a stall that did not clear (after retries)
    QCOMPARE(supguard::decide(false, silent, false), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, silent, true), Action::Replace);
    // Cannot judge (review fix): no port, refused, closed without a reply, an unknown state: never replaced.
    QCOMPARE(supguard::decide(false, none, false), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, none, true), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, closed, true), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, odd, false), Action::ExitRunning);
    QCOMPARE(supguard::decide(false, odd, true), Action::ExitRunning);
    QVERIFY(supguard::looksHung(hang) && supguard::looksHung(silent) && !supguard::looksHung(none) && !supguard::looksHung(odd));
}

void TestEngineReaper::guardFlowHealthyHungReplace() {
#ifndef Q_OS_WIN
    QSKIP("Windows only");
#endif
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString ipc = uniqueIpc();
    // A /healthz that says ok, on an ephemeral port.
    QProcess* health = startChild({"--health-child", "ok"});
    QVERIFY(health);
    QByteArray said;
    for (int i = 0; i < 50 && !said.contains("PORT "); ++i) { health->waitForReadyRead(100); said += health->readAllStandardOutput(); }
    const quint16 port = quint16(said.mid(said.indexOf("PORT ") + 5).trimmed().toUInt());
    QVERIFY2(port > 0, said.constData());
    const supguard::Health h = supguard::probeHealth(port, 2000);
    QVERIFY(h.answered);
    QCOMPARE(h.state, QString("ok"));
    QCOMPARE(h.pid, health->processId());
    const supguard::Health nobody = supguard::probeHealth(1, 300);            // nothing listens on port 1
    QVERIFY(!nobody.answered && !nobody.connected && !nobody.timedOut);
    // A /healthz that says hang, and one that takes the connection and never answers.
    auto portOf = [](QProcess* p) {
        QByteArray s;
        for (int i = 0; i < 50 && !s.contains("PORT "); ++i) { p->waitForReadyRead(100); s += p->readAllStandardOutput(); }
        return quint16(s.mid(s.indexOf("PORT ") + 5).trimmed().toUInt());
    };
    QProcess* hangChild = startChild({"--health-child", "hang"});
    QProcess* silentChild = startChild({"--health-child", "silent"});
    QVERIFY(hangChild && silentChild);
    const quint16 hangPort = portOf(hangChild), silentPort = portOf(silentChild);
    QVERIFY(hangPort > 0 && silentPort > 0);
    const supguard::Health sh = supguard::probeHealth(silentPort, 500);
    QVERIFY(!sh.answered && sh.connected && sh.timedOut);

    // The "old Supervisor": a child holding the guard, with --ipc-name on its command line, never answering /healthz.
    QProcess* holder = startChild({"--hold-instance", ipc, "--ipc-name", ipc});
    QVERIFY(holder);
    said.clear();
    for (int i = 0; i < 50 && !said.contains("HELD"); ++i) { holder->waitForReadyRead(100); said += holder->readAllStandardOutput(); }
    QVERIFY2(said.contains("HELD"), said.constData());
    const QString exe = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    QCOMPARE(supguard::findSupervisorPids(ipc, exe), (QList<qint64>{holder->processId()}));
    QVERIFY(supguard::findSupervisorPids(ipc + "-x", exe).isEmpty());

    SingleInstance mine(ipc);
    supguard::Options o;
    o.ipcName = ipc; o.exeName = exe; o.dumpDir = tmp.path(); o.predecessorWaitMs = 200; o.healthTimeoutMs = 1000;
    // 1. it answers healthy (the ok endpoint stands in for the holder's): exit "already running", nothing touched
    o.healthPort = port;
    supguard::Result r = supguard::acquireOrReplace(mine, o);
    QVERIFY2(!r.run && r.exitCode == supguard::kExitAlreadyRunning && r.message.contains("already running"), qPrintable(r.message));
    // 2. cannot judge, even with --replace-hung (review fix: its /healthz may be on another port, or off): no port to
    //    ask, and a port nothing listens on. Exit, the holder untouched.
    o.replaceHung = true;
    for (const quint16 p : {quint16(0), quint16(1)}) {
        o.healthPort = p;
        r = supguard::acquireOrReplace(mine, o);
        QVERIFY2(!r.run && r.exitCode == supguard::kExitAlreadyRunning && r.message.contains("cannot be checked") && r.oldPid == 0,
                 qPrintable(r.message));
        QVERIFY(holder->state() == QProcess::Running);
        QVERIFY(!mine.owned());
    }
    // 3. it says hang, without --replace-hung: exit, the holder untouched
    o.replaceHung = false;
    o.healthPort = hangPort;
    o.stallRetries = 0;
    QElapsedTimer t; t.start();
    r = supguard::acquireOrReplace(mine, o);
    QVERIFY2(!r.run && r.exitCode == supguard::kExitAlreadyRunning && r.message.contains("--replace-hung"), qPrintable(r.message));
    QVERIFY(holder->state() == QProcess::Running);
    QVERIFY(!mine.owned());
    // 4. it says hang, with --replace-hung: dumped, terminated, the guard is ours (abandoned by the old owner). The pid
    //    /healthz names (the stand-in endpoint) is not a candidate, so the one candidate on this IPC name is the target.
    o.replaceHung = true;
    t.restart();
    r = supguard::acquireOrReplace(mine, o);
    QVERIFY2(r.run, qPrintable(r.message));
    QCOMPARE(r.oldPid, holder->processId());
    QVERIFY2(!r.dumpPath.isEmpty() && QFileInfo(r.dumpPath).size() > 0, qPrintable(r.message));
    QVERIFY(holder->waitForFinished(3000));
    QVERIFY(mine.owned());
    QVERIFY(mine.abandoned());
    qInfo().noquote() << "replace-hung:" << r.message << "in" << t.elapsed() << "ms";
}

// --- the wiring end to end (PS-4b) ----------------------------------------------------------------------------------

namespace {

// One --sup-child Supervisor: its "EV ..." stdout lines.
struct SupChild {
    QProcess* p = nullptr;
    QStringList lines;
    QByteArray partial;
    void pull() {
        partial += p->readAllStandardOutput();
        for (int nl; (nl = partial.indexOf('\n')) >= 0; partial.remove(0, nl + 1)) lines << QString::fromUtf8(partial.left(nl)).trimmed();
    }
    int find(const QString& prefix, int from = 0) const {
        for (int i = from; i < lines.size(); ++i) if (lines[i].startsWith(prefix)) return i;
        return -1;
    }
    int waitFor(const QString& prefix, int ms, int from = 0) {
        QElapsedTimer t; t.start();
        for (;;) {
            pull();
            const int i = find(prefix, from);
            if (i >= 0) return i;
            if (t.elapsed() > ms || p->state() == QProcess::NotRunning) return -1;
            p->waitForReadyRead(50);
        }
    }
    qint64 field(int line, int n) const { return line < 0 ? 0 : lines[line].split(' ').value(n).toLongLong(); }
    QString dump() const { return lines.join('\n'); }
};

bool setMtime(const QString& path, qint64 ms) {
#ifdef Q_OS_WIN
    // FILE_WRITE_ATTRIBUTES only: allowed while the exe runs (its image section blocks writing data, not attributes).
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const quint64 t = ticksForMs(ms);
    FILETIME ft; ft.dwLowDateTime = DWORD(t & 0xFFFFFFFFu); ft.dwHighDateTime = DWORD(t >> 32);
    const bool ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok;
#else
    Q_UNUSED(path); Q_UNUSED(ms); return false;
#endif
}

int lineCount(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    const QList<QByteArray> ls = f.readAll().split('\n');
    return int(std::count_if(ls.begin(), ls.end(), [](const QByteArray& l) { return !l.trimmed().isEmpty(); }));
}

} // namespace

void TestEngineReaper::wiringAdoptReapEndToEnd() {
#ifndef Q_OS_WIN
    QSKIP("Windows only");
#endif
    m_tmp = std::make_unique<QTemporaryDir>();
    QTemporaryDir outside;
    QVERIFY(m_tmp->isValid() && outside.isValid());
    const QString root = QDir(m_tmp->path()).absolutePath(), ipc = uniqueIpc(), data = root + "/data";
    // A private copy of fake_engine: its mtime is "the build" the Supervisors compare against, and touching it must not
    // touch the build tree.
    QDir().mkpath(root + "/bin");
    m_fakeCopy = root + "/bin/fake_engine.exe";
    QVERIFY(QFile::copy(QStringLiteral(PP_FAKE_ENGINE), m_fakeCopy));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();   // the fake's DLLs live next to the original
    env.insert("PATH", QDir::toNativeSeparators(QFileInfo(QStringLiteral(PP_FAKE_ENGINE)).absolutePath()) + ";" + env.value("PATH"));
    const QString self = QCoreApplication::applicationFilePath();
    auto chDir = [&](const QString& ch) { return QDir::toNativeSeparators(data + "/channels/" + ch); };
    auto starts = [&](const QString& ch) { return lineCount(chDir(ch) + "/fake_engine_starts.txt"); };
    int supN = 0;
    auto startSup = [&](const QStringList& channels, int helloMs, int rescanMs) {
        SupChild s;
        s.p = new QProcess;
        m_children << s.p;
        s.p->setProcessEnvironment(env);
        s.p->setStandardErrorFile(QDir(root).filePath(QString("sup-%1.log").arg(++supN)));
        s.p->start(self, {"--sup-child", "--ipc", ipc, "--data", data, "--engine", m_fakeCopy, "--channels", channels.join(','),
                          "--hello-ms", QString::number(helloMs), "--rescan-ms", QString::number(rescanMs)});
        if (!s.p->waitForStarted(5000) || s.waitFor("EV ready", 10000) < 0) s.lines << "FAILED TO START";
        return s;
    };
    auto startFake = [&](const QStringList& args) {
        auto* p = new QProcess;
        m_children << p;
        p->setProcessEnvironment(env);
        p->start(m_fakeCopy, args);
        if (!p->waitForStarted(5000)) return qint64(0);
        m_enginePids << p->processId();
        return qint64(p->processId());
    };
    auto killSup = [](SupChild& s) { s.p->kill(); s.p->waitForFinished(5000); };   // a crash: its engines stay

    // (1) Supervisor A spawns perftest-a; A dies; the engine plays on; Supervisor B adopts the same pid.
    SupChild a = startSup({"perftest-a"}, 15000, 60000);
    int i = a.waitFor("EV connected perftest-a", 15000);
    QVERIFY2(i >= 0, qPrintable(a.dump()));
    const qint64 P = a.field(a.find("EV spawned perftest-a"), 3);
    QVERIFY(P > 0);
    m_enginePids << P;
    // The Supervisor pid is passed at spawn in the environment (an engine exe older than PS-3 would reject the option).
    const QString cl = processCommandLine(P);
    QVERIFY2(!cl.contains("--supervisor-pid") && !cl.contains("--takeover"), qPrintable(cl));
    {
        QFile sf(chDir("perftest-a") + "/fake_engine_starts.txt");
        QVERIFY(sf.open(QIODevice::ReadOnly));
        const QByteArray line = sf.readAll().trimmed();
        QVERIFY2(line == QByteArray::number(P) + " sp=" + QByteArray::number(a.p->processId()) + " to=0", line.constData());
    }
    killSup(a);
    QTest::qWait(1500);
    QVERIFY2(enginelock::processAlive(P), "the engine must survive its Supervisor");
    SupChild b = startSup({"perftest-a"}, 15000, 60000);
    i = b.waitFor(QString("EV confirmed perftest-a %1").arg(P), 15000);
    QVERIFY2(i >= 0, qPrintable(b.dump()));
    QVERIFY2(b.find("EV alert perftest-a info adopting orphan engine pid " + QString::number(P)) >= 0, qPrintable(b.dump()));
    QVERIFY2(b.find("EV spawned") < 0, qPrintable(b.dump()));               // no second engine, no new NDI sender
    QCOMPARE(starts("perftest-a"), 1);
    // The engine left orphan mode and follows B (SupervisorLink rewrites engine.lock on adoption).
    enginelock::LockInfo li;
    for (int k = 0; k < 40 && !(enginelock::readLock(enginelock::lockPath(chDir("perftest-a")), &li) && li.supervisorPid == b.p->processId()); ++k) QTest::qWait(50);
    QCOMPARE(li.supervisorPid, qint64(b.p->processId()));
    QCOMPARE(li.pid, P);

    // (2) A new build (the engine exe's mtime changes) while B is gone: C reaps the old engine and spawns after its exit.
    killSup(b);
    const qint64 oldMtime = QFileInfo(m_fakeCopy).lastModified().toMSecsSinceEpoch();
    QVERIFY(setMtime(m_fakeCopy, oldMtime - 86'400'000LL));   // earlier, so the lockless engine below counts as this build
    QCOMPARE(QFileInfo(m_fakeCopy).lastModified().toMSecsSinceEpoch(), oldMtime - 86'400'000LL);
    SupChild c = startSup({"perftest-a"}, 15000, 60000);
    const int reapedAt = c.waitFor(QString("EV alert perftest-a warning reaped orphan engine pid %1").arg(P), 15000);
    QVERIFY2(reapedAt >= 0 && c.lines[reapedAt].contains("build mismatch"), qPrintable(c.dump()));
    const int exitedAt = c.waitFor(QString("EV exited perftest-a %1").arg(P), 10000);
    const int spawnedAt = c.waitFor("EV spawned perftest-a", 10000);
    QVERIFY2(exitedAt >= 0 && spawnedAt > exitedAt, qPrintable(c.dump()));  // the replacement starts after the old exit
    const qint64 Q = c.field(spawnedAt, 3);
    QVERIFY(Q > 0 && Q != P);
    m_enginePids << Q;
    QVERIFY(!enginelock::processAlive(P));
    QVERIFY2(c.waitFor("EV connected perftest-a", 10000) >= 0, qPrintable(c.dump()));
    QCOMPARE(starts("perftest-a"), 2);

    // (3) + (4): C dies (Q is orphaned). A lockless legacy engine of perftest-b (no Supervisor up: never connected), and
    // two engines of another IPC name in our data dir (one with a lock, one legacy). D (channels a, b; 1.5 s HELLO
    // deadline, 1.5 s re-scan) adopts Q, matches the legacy engine by its command line, adopts it, reaps it when no HELLO
    // comes and spawns perftest-b; it never touches the other IPC name's engines.
    killSup(c);
    const qint64 L = startFake({"--fake-stay", "--id", "perftest-b", "--data", chDir("perftest-b"), "--supervisor", ipc, "--fake-max-life-ms", "60000"});
    const qint64 O1 = startFake({"--fake-link", "--id", "perftest-a", "--data", chDir("perftest-z"), "--supervisor", ipc + "-other", "--fake-max-life-ms", "60000"});
    const qint64 O2 = startFake({"--fake-stay", "--id", "perftest-a", "--data", chDir("perftest-y"), "--supervisor", ipc + "-other", "--fake-max-life-ms", "60000"});
    QVERIFY(L > 0 && O1 > 0 && O2 > 0);
    for (int k = 0; k < 40 && !QFile::exists(enginelock::lockPath(chDir("perftest-z"))); ++k) QTest::qWait(50);
    QVERIFY(QFile::exists(enginelock::lockPath(chDir("perftest-z"))));        // in our data dir, but not our IPC name
    SupChild d = startSup({"perftest-a", "perftest-b"}, 1500, 1500);
    QVERIFY2(d.waitFor(QString("EV confirmed perftest-a %1").arg(Q), 15000) >= 0, qPrintable(d.dump()));
    QVERIFY2(d.waitFor(QString("EV alert perftest-b info adopting orphan engine pid %1").arg(L), 5000) >= 0, qPrintable(d.dump()));
    const int noHello = d.waitFor(QString("EV alert perftest-b warning reaped adopted engine pid %1").arg(L), 8000);
    QVERIFY2(noHello >= 0 && d.lines[noHello].contains("no HELLO"), qPrintable(d.dump()));
    const int lExit = d.waitFor(QString("EV exited perftest-b %1").arg(L), 8000);
    const int bSpawn = d.waitFor("EV spawned perftest-b", 8000);
    QVERIFY2(lExit >= 0 && bSpawn > lExit, qPrintable(d.dump()));
    m_enginePids << d.field(bSpawn, 3);
    QVERIFY2(d.waitFor("EV connected perftest-b", 10000) >= 0, qPrintable(d.dump()));
    QVERIFY2(d.find("EV spawned perftest-a") < 0, qPrintable(d.dump()));    // Q stays: adopted, not replaced
    QCOMPARE(starts("perftest-a"), 2);

    // (binding guard) Another engine for perftest-a on our IPC name, its data dir outside our scope (the scan cannot
    // see it): it connects and says HELLO; D refuses it (MSG_ENGINE_FATAL "duplicate") and it exits with code 4.
    QProcess* dup = new QProcess;
    m_children << dup;
    dup->setProcessEnvironment(env);
    dup->start(m_fakeCopy, {"--fake-link", "--takeover", "--id", "perftest-a", "--data", QDir::toNativeSeparators(outside.path() + "/channels/perftest-a"),
                            "--supervisor", ipc, "--fake-max-life-ms", "60000"});
    QVERIFY(dup->waitForStarted(5000));
    const qint64 X = dup->processId();
    m_enginePids << X;
    QVERIFY2(dup->waitForFinished(15000), "the duplicate must exit");
    QCOMPARE(dup->exitCode(), int(enginelock::kExitDuplicate));
    QVERIFY2(d.waitFor(QString("EV alert perftest-a warning refused duplicate engine pid %1").arg(X), 3000) >= 0, qPrintable(d.dump()));
    QVERIFY2(d.find(QString("EV connected perftest-a %1").arg(X)) < 0, qPrintable(d.dump()));
    QVERIFY(enginelock::processAlive(Q));

    // (re-scan) A legacy engine of a channel D does not know appears in scope; it connects (bound as an attached engine,
    // as before) and the next re-scan reaps it.
    const qint64 U = startFake({"--fake-stay", "--id", "perftest-c", "--data", chDir("perftest-c"), "--supervisor", ipc, "--fake-max-life-ms", "60000"});
    QVERIFY(U > 0);
    const int uReaped = d.waitFor(QString("EV alert perftest-c warning reaped orphan engine pid %1").arg(U), 8000);
    QVERIFY2(uReaped >= 0 && d.lines[uReaped].contains("unknown channel"), qPrintable(d.dump()));
    QVERIFY2(d.waitFor(QString("EV exited perftest-c %1").arg(U), 8000) >= 0, qPrintable(d.dump()));

    // (4) the other IPC name's engines: alive, and never named by D.
    QVERIFY(enginelock::processAlive(O1));
    QVERIFY(enginelock::processAlive(O2));
    d.pull();
    for (const QString& line : std::as_const(d.lines))
        QVERIFY2(!line.contains(QString::number(O1)) && !line.contains(QString::number(O2)), qPrintable(line));
    QVERIFY2(d.lines.filter("EV scan").size() >= 3, qPrintable(d.dump()));   // startup + re-scans ran past them
    killSup(d);
}

// --- child modes ----------------------------------------------------------------------------------------------------
// --sleep-child [engine-like args]  : sleeps up to 60 s (the test kills it sooner); "--exit-now" exits at once.
// --hold-instance <ipc>             : takes SingleInstance(ipc), prints HELD, sleeps up to 60 s.
// --health-child <state>            : serves /healthz {"pid", "state"} on an ephemeral port, prints "PORT n", 60 s
//                                     ("silent": accepts connections and never answers).
// --sup-child --ipc --data --engine --channels a,b --hello-ms --rescan-ms : a Supervisor's engine side (IpcTransport +
//     ChannelManager + reaper, wired as supervisorcore.cpp does) that spawns the --engine (fake_engine --fake-link);
//     prints "EV ..." lines. Gives up after 90 s.
static int childMain(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--exit-now") == 0) return 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--hold-instance") == 0) {
            SingleInstance s(QString::fromLocal8Bit(argv[i + 1]));
            if (!s.tryAcquire(0)) { std::printf("REFUSED\n"); std::fflush(stdout); return 3; }
            std::printf("HELD\n"); std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
}

static int healthChildMain(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString state = "ok";
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], "--health-child") == 0) state = QString::fromLocal8Bit(argv[i + 1]);
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) return 3;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&]() {
        while (QTcpSocket* s = server.nextPendingConnection()) {
            QObject::connect(s, &QTcpSocket::readyRead, s, [s, state]() {
                if (!s->readAll().contains("\r\n\r\n") || state == "silent") return;   // silent: a stuck endpoint
                const QByteArray body = QJsonDocument(QJsonObject{{"name", "supervisor"}, {"pid", double(QCoreApplication::applicationPid())},
                                                                  {"state", state}}).toJson(QJsonDocument::Compact);
                s->write("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                s->disconnectFromHost();
            });
            QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        }
    });
    std::printf("PORT %d\n", int(server.serverPort()));
    std::fflush(stdout);
    QTimer::singleShot(60000, &app, &QCoreApplication::quit);
    return app.exec();
}

static int supChildMain(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString ipc, data, engine;
    QStringList channels;
    int helloMs = 15000, rescanMs = 60000;
    const QStringList a = app.arguments();
    for (int i = 1; i + 1 < a.size(); ++i) {
        if (a[i] == "--ipc") ipc = a[++i];
        else if (a[i] == "--data") data = a[++i];
        else if (a[i] == "--engine") engine = a[++i];
        else if (a[i] == "--channels") channels = a[++i].split(',', Qt::SkipEmptyParts);
        else if (a[i] == "--hello-ms") helloMs = a[++i].toInt();
        else if (a[i] == "--rescan-ms") rescanMs = a[++i].toInt();
    }
    if (!ipc.startsWith("ppreap-")) return 2;   // never the live name
    auto out = [](const QString& line) { std::printf("%s\n", line.toUtf8().constData()); std::fflush(stdout); };
    IpcTransport server(IpcTransport::ServerRole);
    if (!server.startServer(ipc)) { out("EV failed ipc"); return 3; }
    ChannelManager cm;
    cm.setIpc(&server);
    cm.setSupervisorIpcName(ipc);
    cm.setEngineExecutable(engine);
    cm.setEngineExtraArgs({"--fake-link", "--fake-max-life-ms", "90000"});
    auto pidOf = [&cm](const QString& id) { const ChannelManager::ChannelRecord* r = cm.channel(id); return r ? r->pid : 0; };
    // As supervisorcore.cpp onIpcMessage / onIpcClientDisconnected (PS-4b).
    QObject::connect(&server, &IpcTransport::messageReceived, &app, [&cm](const IpcProtocol::Message& msg, const QString& cid) {
        using namespace IpcProtocol;
        if (msg.type == MSG_ENGINE_HELLO) cm.onEngineHello(cid, msg.channelId, msg.payload);
        else if (msg.type == MSG_ENGINE_READY) cm.bindClient(msg.channelId, cid);
        else if (msg.type == MSG_HEARTBEAT) {
            if (cm.channelForClient(cid).isEmpty()) cm.bindClient(msg.channelId, cid);
            if (cm.channelForClient(cid) == msg.channelId) cm.onHeartbeat(msg.channelId, qint64(msg.payload.value("frameNumber").toDouble()), true);
        }
    });
    QObject::connect(&server, &IpcTransport::clientDisconnected, &app, [&cm](const QString& cid) { cm.onClientDisconnected(cid); });
    QObject::connect(&cm, &ChannelManager::channelSpawned, &app, [&](const QString& id, int) { out(QString("EV spawned %1 %2").arg(id).arg(pidOf(id))); });
    QObject::connect(&cm, &ChannelManager::engineConnected, &app, [&](const QString& id) { out(QString("EV connected %1 %2").arg(id).arg(pidOf(id))); });
    QObject::connect(&cm, &ChannelManager::engineAdopted, &app, [&](const QString& id, qint64 pid) { out(QString("EV confirmed %1 %2").arg(id).arg(pid)); });
    QObject::connect(&cm, &ChannelManager::engineExited, &app, [&](const QString& id, qint64 pid, int code, bool, bool expected) {
        out(QString("EV exited %1 %2 %3 %4").arg(id).arg(pid).arg(code).arg(expected ? "expected" : "unexpected"));
    });
    QObject::connect(&cm, &ChannelManager::reaperAlert, &app, [&](const QString& ch, const QString& level, const QString& msg) { out(QString("EV alert %1 %2 %3").arg(ch, level, msg)); });
    QObject::connect(&cm, &ChannelManager::scanApplied, &app, [&](int found, int adopted, int reaped) { out(QString("EV scan %1 %2 %3").arg(found).arg(adopted).arg(reaped)); });
    QObject::connect(&cm, &ChannelManager::channelDied, &app, [&](const QString& id, const QString& why) { out(QString("EV died %1 %2").arg(id, why)); });
    auto config = [](const QString& id) { return QJsonObject{{"channelId", id}, {"orphanAdoptWaitSec", 20}}; };
    ChannelManager::ReaperOptions ro;
    ro.dataDir = data;
    ro.helloDeadlineMs = helloMs;
    ro.rescanMs = rescanMs;
    ro.orphanGraceSec = 60;   // an orphan of a killed test Supervisor leaves on its own within a minute
    ro.profileOf = [channels](const QString& id, sched::ChannelProfile* p) {
        if (!channels.contains(id)) return false;
        p->id = id; p->name = id; p->enabled = true;
        return true;
    };
    ro.engineConfig = [config](const sched::ChannelProfile& p) { return config(p.id); };
    ro.startupDone = [&]() {
        out("EV startup");
        for (const QString& id : channels)
            if (!cm.isChannelRunning(id)) cm.spawnChannel(id, id, QDir(data).filePath("channels/" + id), config(id));
    };
    cm.startReaper(ro);
    out(QString("EV ready %1").arg(QCoreApplication::applicationPid()));
    QTimer::singleShot(90000, &app, &QCoreApplication::quit);
    return app.exec();
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--sleep-child") == 0) {
            for (int j = 1; j < argc; ++j) if (std::strcmp(argv[j], "--health-child") == 0) return healthChildMain(argc, argv);
            return childMain(argc, argv);
        }
        if (std::strcmp(argv[i], "--sup-child") == 0) return supChildMain(argc, argv);
    }
    QCoreApplication app(argc, argv);
    TestEngineReaper t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_enginereaper.moc"
