// Non-blocking log sink (STB-1 = perf-supervisor-app W1 = perf-engine PE-01): logging must never block the caller,
// not even when stderr is a pipe nobody reads, which is how the 10-24 / 10-25 outages froze the whole stack.
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include "playout/core/asynclog.h"
#include "playout/core/logsink.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <thread>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

const char* const kEchoVars[] = {"PLAYPLAY_CONSOLE_LOG", "PLAYPLAY_LOG_STDERR", "PLAYPLAY_LOG_DEBUGGER"};

// This process logs through asynclog during the in-process tests, so results go to stdout directly.
void say(const QString& s) { std::fputs(qPrintable(s + "\n"), stdout); std::fflush(stdout); }

QList<QJsonObject> readLines(const QString& path) {
    QList<QJsonObject> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (!line.isEmpty()) out << QJsonDocument::fromJson(line).object();
    }
    return out;
}

QString dayFile(const QString& dir, const QString& base, const QDate& d = QDate::currentDate()) {
    return QDir(dir).filePath(base + "-" + d.toString(Qt::ISODate) + ".log");
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

QProcessEnvironment childEnv(const char* echoVar) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    for (const char* v : kEchoVars) env.remove(QString::fromLatin1(v));
    if (echoVar) env.insert(QString::fromLatin1(echoVar), "1");
    return env;
}

// Counts "flood t=" lines and the drop counts carried by every line of a helper's log.
void countFlood(const QString& dir, int* lines, qint64* dropped, bool* sawEnd) {
    *lines = 0; *dropped = 0; *sawEnd = false;
    for (const QJsonObject& o : readLines(dayFile(dir, "flood"))) {
        const QString m = o.value("msg").toString();
        if (m.startsWith("flood t=")) ++*lines;
        if (m == "flood-end") *sawEnd = true;
        *dropped += o.value("dropped").toInteger();
    }
}

#ifdef Q_OS_WIN
struct ChildResult { bool started = false; bool exited = false; DWORD code = 0; qint64 ms = 0; };

// Runs exe with stdout and stderr on an anonymous pipe that is never read (default ~4 KB buffer, so a child that
// writes to it blocks quickly) and stdin on NUL. Kills the child (ours) if it does not exit within timeoutMs.
ChildResult runWithUnreadPipe(const QString& exe, const QStringList& args, const QProcessEnvironment& env, int timeoutMs) {
    ChildResult r;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    QString cmd = "\"" + QDir::toNativeSeparators(exe) + "\"";
    for (const QString& a : args) cmd += " \"" + a + "\"";
    QString block;
    for (const QString& kv : env.toStringList()) { block += kv; block += QChar(0); }
    block += QChar(0);
    std::wstring wcmd = cmd.toStdWString();
    std::wstring wenv = block.toStdWString();
    PROCESS_INFORMATION pi{};
    QElapsedTimer t;
    t.start();
    const BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   wenv.data(), nullptr, &si, &pi);
    CloseHandle(wr);                                // the child holds the only write end; `rd` stays open, unread
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return r; }
    r.started = true;
    r.exited = WaitForSingleObject(pi.hProcess, DWORD(timeoutMs)) == WAIT_OBJECT_0;
    r.ms = t.elapsed();
    if (!r.exited) { TerminateProcess(pi.hProcess, 99); WaitForSingleObject(pi.hProcess, 5000); }
    GetExitCodeProcess(pi.hProcess, &r.code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return r;
}
#endif

} // namespace

class TestAsyncLog : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        for (const char* v : kEchoVars) qunsetenv(v);
        QVERIFY2(QFileInfo::exists(PP_LOGFLOOD_HELPER), PP_LOGFLOOD_HELPER);
    }

    void cleanupTestCase() {
        asynclog::testing::setWriterPaused(false);
        asynclog::testing::setDateOverride(QDate());
        asynclog::shutdown(true);
    }

    // A fresh folder per test; the sink is stopped (file closed, QTest's handler back) before the folder goes.
    void init() {
        m_dir.reset(new QTemporaryDir);
        QVERIFY(m_dir->isValid());
    }
    void cleanup() {
        asynclog::testing::setWriterPaused(false);
        asynclog::testing::setDateOverride(QDate());
        asynclog::shutdown(true);
        m_dir.reset();
    }

    // (a) The exact failure class: 20 000 lines from 4 threads with stdout/stderr on a pipe nobody reads. The old sink
    // blocked forever after ~4 KB; this one never touches stderr and must be done (process exited) in < 2 s.
    void floodWithUnreadStderr() {
#ifndef Q_OS_WIN
        QSKIP("Windows pipe test");
#else
        QTemporaryDir& dir = *m_dir;
        const ChildResult r = runWithUnreadPipe(PP_LOGFLOOD_HELPER, {dir.path(), "20000", "4"}, childEnv(nullptr), 15000);
        QVERIFY(r.started);
        QVERIFY2(r.exited, "helper hung with an unread stderr pipe");
        QCOMPARE(int(r.code), 0);
        const QJsonObject res = readJson(dir.filePath("result.json"));
        say(QString("flood file-only: process %1 ms, log loop %2 ms, written %3, dropped %4")
                .arg(r.ms).arg(res.value("logMs").toInteger()).arg(res.value("written").toInteger()).arg(res.value("dropped").toInteger()));
        QVERIFY2(r.ms < 2000, qPrintable(QString("helper took %1 ms").arg(r.ms)));
        QCOMPARE(res.value("consoleEcho").toBool(), false);
        int lines = 0; qint64 dropped = 0; bool sawEnd = false;
        countFlood(dir.path(), &lines, &dropped, &sawEnd);
        QVERIFY(sawEnd);
        QCOMPARE(dropped, res.value("dropped").toInteger());        // every drop is reported on a later line
        QCOMPARE(qint64(lines) + dropped, qint64(20000));             // nothing lost silently
#endif
    }

    // Echo on (PLAYPLAY_CONSOLE_LOG=1) with the same unread pipe: only the echo thread blocks, its bounded ring drops
    // the rest, the file still gets every line, and exit does not wait for the blocked console write.
    void floodWithEchoOnUnreadStderr() {
#ifndef Q_OS_WIN
        QSKIP("Windows pipe test");
#else
        QTemporaryDir& dir = *m_dir;
        const ChildResult r = runWithUnreadPipe(PP_LOGFLOOD_HELPER, {dir.path(), "20000", "4"}, childEnv("PLAYPLAY_CONSOLE_LOG"), 15000);
        QVERIFY(r.started);
        QVERIFY2(r.exited, "helper hung on exit with a blocked echo thread");
        QCOMPARE(int(r.code), 0);
        const QJsonObject res = readJson(dir.filePath("result.json"));
        say(QString("flood with echo: process %1 ms, log loop %2 ms, console written %3, console dropped %4, file dropped %5")
                .arg(r.ms).arg(res.value("logMs").toInteger()).arg(res.value("consoleWritten").toInteger())
                .arg(res.value("consoleDropped").toInteger()).arg(res.value("dropped").toInteger()));
        QVERIFY2(r.ms < 2000, qPrintable(QString("helper took %1 ms").arg(r.ms)));
        QCOMPARE(res.value("consoleEcho").toBool(), true);
        QVERIFY(res.value("consoleDropped").toInteger() > 0);         // the console blocked; the echo ring dropped
        int lines = 0; qint64 dropped = 0; bool sawEnd = false;
        countFlood(dir.path(), &lines, &dropped, &sawEnd);
        QVERIFY(sawEnd);
        QCOMPARE(qint64(lines) + dropped, qint64(20000));
#endif
    }

    // Both variables turn the echo on; without them nothing reaches stderr at all (QProcess drains the pipes here).
    void echoEnvironmentFlags_data() {
        QTest::addColumn<QString>("var");
        QTest::addColumn<bool>("echo");
        QTest::newRow("none") << QString() << false;
        QTest::newRow("PLAYPLAY_CONSOLE_LOG") << QString("PLAYPLAY_CONSOLE_LOG") << true;
        QTest::newRow("PLAYPLAY_LOG_STDERR") << QString("PLAYPLAY_LOG_STDERR") << true;
    }
    void echoEnvironmentFlags() {
        QFETCH(QString, var);
        QFETCH(bool, echo);
        QTemporaryDir& dir = *m_dir;
        const QByteArray v = var.toLatin1();
        QProcess p;
        p.setProcessEnvironment(childEnv(var.isEmpty() ? nullptr : v.constData()));
        p.start(PP_LOGFLOOD_HELPER, {dir.path(), "200", "1"});
        QVERIFY(p.waitForFinished(15000));
        QCOMPARE(p.exitCode(), 0);
        const QByteArray err = p.readAllStandardError(), out = p.readAllStandardOutput();
        if (echo) {
            QCOMPARE(int(err.count("flood t=")), 200);
            QVERIFY(err.contains("flood-end"));
        } else {
            QVERIFY2(!err.contains("flood"), err.left(200).constData());
            QVERIFY(!out.contains("flood"));
        }
        QCOMPARE(readJson(dir.filePath("result.json")).value("consoleEcho").toBool(), echo);
        int lines = 0; qint64 dropped = 0; bool sawEnd = false;
        countFlood(dir.path(), &lines, &dropped, &sawEnd);
        QCOMPARE(lines, 200);
    }

    // p99 of one qInfo() call < 50 us from 4 threads at once (through installFileLogging, the production entry point);
    // per-thread order is kept in the file and every line is either written or counted as dropped.
    void latencyAndOrder() {
        QTemporaryDir& dir = *m_dir;
        installFileLogging(dir.path(), "lat");
        constexpr int kThreads = 4, kPer = 5000;
        std::vector<std::vector<qint64>> ns(kThreads);
        std::atomic<bool> go{false};
        std::vector<std::thread> pool;
        for (int t = 0; t < kThreads; ++t) {
            pool.emplace_back([&, t] {
                ns[t].reserve(kPer);
                while (!go.load()) std::this_thread::yield();
                for (int i = 0; i < kPer; ++i) {
                    const auto a = std::chrono::steady_clock::now();
                    qInfo("lat t=%d i=%d", t, i);
                    ns[t].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - a).count());
                }
            });
        }
        go.store(true);
        for (std::thread& th : pool) th.join();
        std::vector<qint64> all;
        for (const auto& v : ns) all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        const qint64 p50 = all[all.size() / 2], p99 = all[all.size() * 99 / 100], mx = all.back();
        QVERIFY(asynclog::flush(5000));
        qInfo("lat-end");
        QVERIFY(asynclog::flush(5000));
        const asynclog::Stats st = asynclog::stats();
        say(QString("latency over %1 calls: p50 %2 us, p99 %3 us, max %4 us; written %5, dropped %6")
                .arg(all.size()).arg(p50 / 1000.0, 0, 'f', 2).arg(p99 / 1000.0, 0, 'f', 2).arg(mx / 1000.0, 0, 'f', 1)
                .arg(st.written).arg(st.dropped));
        QVERIFY2(p99 < 50000, qPrintable(QString("p99 %1 ns").arg(p99)));

        const QList<QJsonObject> lines = readLines(dayFile(dir.path(), "lat"));
        QVERIFY(!lines.isEmpty());
        QVERIFY(lines.first().value("msg").toString().startsWith("[Log] writing"));
        QVERIFY(lines.first().value("msg").toString().contains("file only"));
        const QRegularExpression ts("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d\\.\\d{3}Z$");
        QVERIFY2(ts.match(lines.first().value("ts").toString()).hasMatch(), qPrintable(lines.first().value("ts").toString()));
        QCOMPARE(lines.first().value("level").toString(), QString("info"));
        const QRegularExpression re("^lat t=(\\d+) i=(\\d+)$");
        int last[kThreads] = {-1, -1, -1, -1};
        int count = 0;
        qint64 dropped = 0;
        for (const QJsonObject& o : lines) {
            dropped += o.value("dropped").toInteger();
            const auto m = re.match(o.value("msg").toString());
            if (!m.hasMatch()) continue;
            const int t = m.captured(1).toInt(), i = m.captured(2).toInt();
            QVERIFY2(i > last[t], qPrintable(QString("thread %1: %2 after %3").arg(t).arg(i).arg(last[t])));
            last[t] = i;
            ++count;
        }
        QCOMPARE(dropped, qint64(st.dropped));
        QCOMPARE(qint64(count) + dropped, qint64(kThreads * kPer));
    }

    // Daily files: a date change rotates to <base>-<new date>.log; files of this base older than keepDays are pruned,
    // anything else in the folder is left alone.
    void dailyRotationAndPrune() {
        QTemporaryDir& dir = *m_dir;
        const QDate d1(2026, 3, 10), d2 = d1.addDays(1);
        auto touch = [&](const QString& name) { QFile f(dir.filePath(name)); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{}\n"); };
        touch("rot-2026-02-01.log");        // older than 14 days: pruned
        touch("rot-2026-03-05.log");        // within 14 days: kept
        touch("rot-notes.log");             // no date: kept
        touch("other-2026-01-01.log");      // another base name: kept
        asynclog::testing::setDateOverride(d1);
        asynclog::Options o;
        o.dir = dir.path(); o.baseName = "rot"; o.keepDays = 14;
        asynclog::install(o);
        qInfo("day one");
        QVERIFY(asynclog::flush(3000));
        asynclog::testing::setDateOverride(d2);
        qWarning("day two");
        QVERIFY(asynclog::flush(3000));
        asynclog::testing::setDateOverride(QDate());

        const QList<QJsonObject> one = readLines(dayFile(dir.path(), "rot", d1)), two = readLines(dayFile(dir.path(), "rot", d2));
        QCOMPARE(one.size(), 1);
        QCOMPARE(one.first().value("msg").toString(), QString("day one"));
        QCOMPARE(two.size(), 1);
        QCOMPARE(two.first().value("msg").toString(), QString("day two"));
        QCOMPARE(two.first().value("level").toString(), QString("warning"));
        QVERIFY(asynclog::currentPath().endsWith("rot-2026-03-11.log"));
        QVERIFY(!QFile::exists(dir.filePath("rot-2026-02-01.log")));
        QVERIFY(QFile::exists(dir.filePath("rot-2026-03-05.log")));
        QVERIFY(QFile::exists(dir.filePath("rot-notes.log")));
        QVERIFY(QFile::exists(dir.filePath("other-2026-01-01.log")));
    }

    // Ring overflow: with the writer held, a 64-cell ring takes 64 lines and drops 100; the next line that gets in
    // carries "dropped": 100. A full ring never blocks the caller.
    void droppedCounter() {
        QTemporaryDir& dir = *m_dir;
        asynclog::Options o;
        o.dir = dir.path(); o.baseName = "drop"; o.ringSize = 64;
        asynclog::install(o);
        asynclog::testing::setWriterPaused(true);
        QTest::qWait(30);                   // let a writer pass that started before the pause finish
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 164; ++i) qInfo("fill %d", i);
        const qint64 fillMs = t.elapsed();
        QCOMPARE(asynclog::stats().dropped, quint64(100));
        QVERIFY(!asynclog::flush(50));      // writer held: nothing can be flushed
        asynclog::testing::setWriterPaused(false);
        QVERIFY(asynclog::flush(3000));
        qInfo("after");
        QVERIFY(asynclog::flush(3000));
        QVERIFY2(fillMs < 100, qPrintable(QString("filling a full ring took %1 ms").arg(fillMs)));

        const QList<QJsonObject> lines = readLines(dayFile(dir.path(), "drop"));
        QCOMPARE(lines.size(), 65);
        for (int i = 0; i < 64; ++i) {
            QCOMPARE(lines.at(i).value("msg").toString(), QString("fill %1").arg(i));
            QVERIFY(!lines.at(i).contains("dropped"));
        }
        QCOMPARE(lines.at(64).value("msg").toString(), QString("after"));
        QCOMPARE(lines.at(64).value("dropped").toInteger(), qint64(100));
    }

    // qFatal: the handler drains the ring and writes the fatal line on the calling thread, flushed before it returns
    // (Qt aborts right after). Called directly here so the test process survives; the writer is held to prove the
    // lines did not come from it.
    void fatalWritesSynchronously() {
        QTemporaryDir& dir = *m_dir;
        asynclog::Options o;
        o.dir = dir.path(); o.baseName = "fatal";
        asynclog::install(o);
        asynclog::testing::setWriterPaused(true);
        QTest::qWait(30);
        for (int i = 0; i < 10; ++i) qInfo("before %d", i);
        QVERIFY(readLines(dayFile(dir.path(), "fatal")).isEmpty());
        const QMessageLogContext ctx;
        asynclog::testing::handler()(QtFatalMsg, ctx, QStringLiteral("fatal boom"));
        const QList<QJsonObject> lines = readLines(dayFile(dir.path(), "fatal"));
        asynclog::testing::setWriterPaused(false);
        QCOMPARE(lines.size(), 11);
        for (int i = 0; i < 10; ++i) QCOMPARE(lines.at(i).value("msg").toString(), QString("before %1").arg(i));
        QCOMPARE(lines.at(10).value("msg").toString(), QString("fatal boom"));
        QCOMPARE(lines.at(10).value("level").toString(), QString("fatal"));
    }

    // After shutdown (what atexit runs) messages are still written, synchronously.
    void writesAfterShutdown() {
        QTemporaryDir& dir = *m_dir;
        asynclog::Options o;
        o.dir = dir.path(); o.baseName = "late";
        asynclog::install(o);
        qInfo("queued");
        asynclog::shutdown(false);
        qWarning("late line");
        const QList<QJsonObject> lines = readLines(dayFile(dir.path(), "late"));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0).value("msg").toString(), QString("queued"));
        QCOMPARE(lines.at(1).value("msg").toString(), QString("late line"));
    }

private:
    std::unique_ptr<QTemporaryDir> m_dir;
};

QTEST_GUILESS_MAIN(TestAsyncLog)
#include "test_asynclog.moc"
