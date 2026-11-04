// Console-less child launch (STB-2 = perf-supervisor-app W2 = perf-engine PE-02): the Supervisor and the engines must
// never share a console or std handles with the process that started them. On 10-24 / 10-25 they shared the app's
// Windows Terminal tab (QProcess::ForwardedChannels), so a tab that stopped draining froze the whole stack, and closing
// it would have ended every process with 0xC000013A.
// STB-3 (W3, W0) adds: PlayPlay.exe is a GUI-subsystem program, the Supervisor and the engine ship with matching PDBs,
// and scripts/start-live.cmd hands the app NUL stdio.
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include "playout/core/processlaunch.h"
#include <cstdio>
#include <string>
#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace {

constexpr qint64 kStatusControlCExit = 0xC000013A;          // what a console process gets on Ctrl+Break / close

QString helper() { return QStringLiteral(PP_CONSOLE_PROBE_HELPER); }
qint64 myPid() { return QCoreApplication::applicationPid(); }

QJsonObject readJson(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

bool hasPid(const QJsonArray& a, qint64 pid) {
    for (const QJsonValue& v : a) if (v.toInteger() == pid) return true;
    return false;
}

QString readText(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();
}

struct Run {
    bool started = false, finished = false;
    int exitCode = -1;
    QProcess::ExitStatus status = QProcess::CrashExit;
    qint64 pid = 0, ms = 0;
    QByteArray out, err;
};

// Runs the helper the way the app starts the Supervisor and the Supervisor starts an engine, and waits (test thread
// only). A helper that is still running at the timeout is ours: it is killed.
Run runHelper(const QStringList& args, const QString& logFile = {}, int timeoutMs = 20000, bool detached = true) {
    Run r;
    QProcess p;
    if (detached) processlaunch::configureDetachedChild(p, logFile);
    else p.setProcessChannelMode(QProcess::ForwardedChannels);    // the pre-STB-2 configuration, as a control
    p.setProgram(helper());
    p.setArguments(args);
    QElapsedTimer t;
    t.start();
    p.start();
    r.started = p.waitForStarted(10000);
    r.pid = p.processId();                                        // ChannelManager reads it right after start()
    r.finished = r.started && p.waitForFinished(timeoutMs);
    r.ms = t.elapsed();
    if (r.started && !r.finished) { p.kill(); p.waitForFinished(5000); }
    r.exitCode = p.exitCode();
    r.status = p.exitStatus();
    // A detached child has no pipe to read (QProcess would only warn "WriteOnly device"): its stdio is NUL or a file.
    if (!detached) { r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError(); }
    return r;
}

void expectNoConsole(const QJsonObject& probe, qint64 notOnConsoleOf, const char* who) {
    QVERIFY2(!probe.isEmpty(), who);
    QVERIFY2(!probe.value("consoleWindow").toBool(), qPrintable(QString("%1 has a console window").arg(who)));
    QVERIFY2(!hasPid(probe.value("consolePids").toArray(), notOnConsoleOf),
             qPrintable(QString("%1 shares the console of pid %2: %3").arg(who).arg(notOnConsoleOf)
                            .arg(QString::fromUtf8(QJsonDocument(probe.value("consolePids").toArray()).toJson(QJsonDocument::Compact)))));
}

// ---- STB-3 ----------------------------------------------------------------------------------------------------------

constexpr int kSubsystemGui = 2, kSubsystemConsole = 3;    // IMAGE_SUBSYSTEM_WINDOWS_GUI / _CUI

// The PE optional header's Subsystem field, read from the file (what "dumpbin /headers" prints); -1 if not a PE file.
int peSubsystem(const QString& exe) {
    QFile f(exe);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray h = f.read(4096);
    if (h.size() < 0x40 || !h.startsWith("MZ")) return -1;
    const qint64 pe = qFromLittleEndian<quint32>(h.constData() + 0x3C);
    const qint64 at = pe + 4 + 20 + 68;                     // "PE\0\0", IMAGE_FILE_HEADER, Subsystem (PE32 and PE32+)
    if (at + 2 > h.size() || h.mid(int(pe), 4) != QByteArray("PE\0\0", 4)) return -1;
    return qFromLittleEndian<quint16>(h.constData() + at);
}

bool samePath(const QString& a, const QString& b) {
    return QDir::cleanPath(QFileInfo(a).absoluteFilePath()).compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()), Qt::CaseInsensitive) == 0;
}

#ifdef Q_OS_WIN
// DbgHelp on one image, the lookup the watchdog's stack samples (STB-4) will make. Never a debugger: the process handle
// is just a unique value. Search path = the exe's folder only, so _NT_SYMBOL_PATH (symbol servers) is never consulted.
class SymSession {
public:
    explicit SymSession(const QString& exe) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_EXACT_SYMBOLS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        const std::wstring dir = QDir::toNativeSeparators(QFileInfo(exe).absolutePath()).toStdWString();
        m_init = SymInitializeW(m_proc, dir.c_str(), FALSE);
        if (!m_init) return;
        const std::wstring path = QDir::toNativeSeparators(QFileInfo(exe).absoluteFilePath()).toStdWString();
        m_base = SymLoadModuleExW(m_proc, nullptr, path.c_str(), nullptr, 0, 0, nullptr, 0);
        m_info.SizeOfStruct = sizeof(m_info);
        if (m_base && !SymGetModuleInfoW64(m_proc, m_base, &m_info)) m_base = 0;
    }
    ~SymSession() { if (m_base) SymUnloadModule64(m_proc, m_base); if (m_init) SymCleanup(m_proc); }
    bool loaded() const { return m_base != 0; }
    const IMAGEHLP_MODULEW64& info() const { return m_info; }
    // First symbol matching the mask that has a source line ("name @ file:line"), or empty. Public names alone (code
    // compiled without /Zi) match the mask but have no line.
    QString withLine(const QString& mask, int* matches) const {
        struct Ctx { HANDLE proc; int n = 0; QString hit; } c{m_proc};
        const std::wstring m = mask.toStdWString();
        SymEnumSymbolsW(m_proc, m_base, m.c_str(), [](PSYMBOL_INFOW s, ULONG, PVOID p) -> BOOL {
            auto* c = static_cast<Ctx*>(p);
            ++c->n;
            IMAGEHLP_LINEW64 l{};
            l.SizeOfStruct = sizeof(l);
            DWORD disp = 0;
            if (!SymGetLineFromAddrW64(c->proc, s->Address, &disp, &l)) return TRUE;   // keep looking
            c->hit = QString("%1 @ %2:%3").arg(QString::fromWCharArray(s->Name), QString::fromWCharArray(l.FileName))
                         .arg(l.LineNumber);                // Name is NUL-terminated (NameLen counted the NUL here)
            return FALSE;
        }, &c);
        if (matches) *matches = c.n;
        return c.hit;
    }
private:
    HANDLE m_proc = reinterpret_cast<HANDLE>(quintptr(0x5740B3));
    BOOL m_init = FALSE;
    DWORD64 m_base = 0;
    IMAGEHLP_MODULEW64 m_info{};
};

// Runs a .cmd through cmd.exe with pipes on cmd's own stdin/stdout/stderr (SeparateChannels) and returns the probe's
// report from <json>. The probe is started by the script with "start", so cmd returns at once; wait for the file.
QJsonObject runScript(const QString& script, const QString& json) {
    QProcess cmd;
    cmd.setProgram(QStringLiteral("cmd.exe"));
    cmd.setNativeArguments(QString("/d /s /c \"\"%1\" \"%2\"\"").arg(QDir::toNativeSeparators(script), QDir::toNativeSeparators(json)));
    cmd.start();
    if (!cmd.waitForFinished(15000)) { cmd.kill(); cmd.waitForFinished(5000); }
    QElapsedTimer t;
    t.start();
    QJsonObject o;
    while (t.elapsed() < 15000 && (o = readJson(json)).isEmpty()) QTest::qWait(50);
    if (const qint64 pid = o.value("pid").toInteger()) {        // let the probe exit before its folder is removed
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) { WaitForSingleObject(h, 5000); CloseHandle(h); }
    }
    return o;
}
#endif

} // namespace

class TestProcessLaunch : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY2(QFileInfo::exists(helper()), qPrintable(helper()));
        std::fprintf(stdout, "test process pid %lld, console window: %s\n", myPid(),
#ifdef Q_OS_WIN
                     GetConsoleWindow() ? "yes" : "no");
#else
                     "n/a");
#endif
    }

    // The configuration itself: channel modes and the creation flags the modifier applies on top of Qt's own.
    void configuration() {
        QProcess p;
        processlaunch::configureDetachedChild(p);
        QCOMPARE(p.processChannelMode(), QProcess::SeparateChannels);
        QCOMPARE(p.inputChannelMode(), QProcess::ManagedInputChannel);
        QProcess m;
        QTemporaryDir tmp;
        processlaunch::configureDetachedChild(&m, tmp.filePath("crumbs.log"));
        QCOMPARE(m.processChannelMode(), QProcess::MergedChannels);
        processlaunch::configureDetachedChild(static_cast<QProcess*>(nullptr));   // no-op, no crash
#ifdef Q_OS_WIN
        QCOMPARE(processlaunch::detachedCreationFlags(), quint32(CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP));
        const auto mod = p.createProcessArgumentsModifier();
        QVERIFY(bool(mod));
        QProcess::CreateProcessArguments a{};
        a.flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE | DETACHED_PROCESS;
        mod(&a);
        QCOMPARE(a.flags, (unsigned long)(CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP));
#else
        QCOMPARE(processlaunch::detachedCreationFlags(), quint32(0));
#endif
    }

    // The plan's unit check: GetConsoleWindow()==NULL in the child, its stdout/stderr (and stdin) are NUL, and it is
    // not attached to our console. finished() and the exit code still work without pipes.
    void childHasNoConsoleAndNulStdio() {
        QTemporaryDir tmp;
        const QString json = tmp.filePath("probe.json");
        const Run r = runHelper({json, "--exit", "7"});
        QVERIFY(r.started);
        QVERIFY(r.pid > 0);
        QVERIFY(r.finished);
        QCOMPARE(r.status, QProcess::NormalExit);
        QCOMPARE(r.exitCode, 7);
        const QJsonObject o = readJson(json);
        expectNoConsole(o, myPid(), "child");
#ifdef Q_OS_WIN
        QCOMPARE(o.value("stdout").toString(), QString("nul"));
        QCOMPARE(o.value("stderr").toString(), QString("nul"));
        QCOMPARE(o.value("stdin").toString(), QString("nul"));
        QVERIFY(o.value("stdinEof").toBool());
        // CREATE_NO_WINDOW: a hidden console of its own, with nobody else on it.
        QCOMPARE(o.value("consolePids").toArray(), QJsonArray{o.value("pid").toInteger()});
#endif
    }

    // Control for the probe: the old configuration from a process with a visible console does share that console.
    // QProcess itself passes CREATE_NO_WINDOW when the parent has no console window, so this only runs from a terminal.
    void controlForwardedChannelsSharesOurConsole() {
#ifdef Q_OS_WIN
        if (!GetConsoleWindow()) QSKIP("this test process has no console window, so there is nothing to share");
        QTemporaryDir tmp;
        const QString json = tmp.filePath("probe.json");
        const Run r = runHelper({json}, {}, 20000, false);
        QVERIFY(r.finished);
        const QJsonObject o = readJson(json);
        QVERIFY2(hasPid(o.value("consolePids").toArray(), myPid()), "the probe did not detect a shared console");
        QVERIFY(o.value("consoleWindow").toBool());
#else
        QSKIP("Windows only");
#endif
    }

    // A child that floods stdout and stderr must exit promptly: nothing reads them, and nothing has to.
    void floodToStdioExitsPromptly_data() {
        QTest::addColumn<qint64>("bytes");
        QTest::newRow("1 MB") << qint64(1) * 1024 * 1024;
        QTest::newRow("32 MB") << qint64(32) * 1024 * 1024;
    }
    void floodToStdioExitsPromptly() {
        QFETCH(qint64, bytes);
        QTemporaryDir tmp;
        const QString json = tmp.filePath("probe.json");
        const Run r = runHelper({json, "--write", QString::number(bytes)}, {}, 10000);
        QVERIFY2(r.finished, "the child blocked writing to stdout/stderr");
        QCOMPARE(r.exitCode, 0);
        const QJsonObject o = readJson(json);
        QCOMPARE(o.value("wrote").toInteger(), bytes);
        std::fprintf(stdout, "  %lld bytes to stdout and to stderr: written in %lld ms, process done in %lld ms\n",
                     bytes, o.value("writeMs").toInteger(), r.ms);
        QVERIFY2(o.value("writeMs").toInteger() < 2000, "writes to NUL took too long");
        QVERIFY2(r.ms < 5000, "the child did not exit promptly");
    }

    // Optional breadcrumb file: stdout and stderr merged and appended (two launches), still no console.
    void logFileGetsMergedAppendedOutput() {
        QTemporaryDir tmp;
        const QString log = tmp.filePath("sub/dir/child-stdio.log");     // the folder does not exist yet
        const QString json = tmp.filePath("probe.json");
        QVERIFY(runHelper({json, "--marker", "first"}, log).finished);
        const QJsonObject o = readJson(json);
        expectNoConsole(o, myPid(), "child with a log file");
#ifdef Q_OS_WIN
        QCOMPARE(o.value("stdout").toString(), QString("disk"));
        QCOMPARE(o.value("stderr").toString(), QString("disk"));
        QCOMPARE(o.value("stdin").toString(), QString("nul"));
#endif
        QVERIFY(runHelper({json, "--marker", "second"}, log).finished);
        const QString text = readText(log);
        for (const char* m : {"first-out", "first-err", "second-out", "second-err"})
            QVERIFY2(text.contains(QLatin1String(m)), qPrintable(QString("%1 missing from: %2").arg(m, text)));
        QVERIFY(text.indexOf("first-out") < text.indexOf("second-out"));
    }

    // A breadcrumb path that cannot be opened must not stop a launch (it would keep the Supervisor or an engine off
    // air): it falls back to NUL.
    void unusableLogFileFallsBackToNul() {
        QTemporaryDir tmp;
        QFile blocker(tmp.filePath("afile"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        const QString bad = tmp.filePath("afile/child.log");            // its "folder" is a file
        QProcess p;
        processlaunch::configureDetachedChild(p, bad);
        QCOMPARE(p.processChannelMode(), QProcess::SeparateChannels);
        const QString json = tmp.filePath("probe.json");
        const Run r = runHelper({json, "--marker", "x"}, bad);
        QVERIFY(r.started && r.finished);
        QCOMPARE(r.exitCode, 0);
#ifdef Q_OS_WIN
        QCOMPARE(readJson(json).value("stdout").toString(), QString("nul"));
#endif
    }

    // App -> Supervisor -> engine: a detached child that launches its own detached child; neither shares a console.
    void nestedLaunchHasNoConsole() {
        QTemporaryDir tmp;
        const QString json = tmp.filePath("child.json"), grand = tmp.filePath("grandchild.json");
        const Run r = runHelper({json, "--spawn", grand}, {}, 30000);
        QVERIFY(r.finished);
        const QJsonObject c = readJson(json), g = readJson(grand);
        expectNoConsole(c, myPid(), "child");
        const QJsonObject s = c.value("spawn").toObject();
        QVERIFY(s.value("started").toBool() && s.value("finished").toBool());
        QCOMPARE(s.value("exitCode").toInt(), 0);
        QCOMPARE(s.value("channelMode").toInt(), int(QProcess::SeparateChannels));
        expectNoConsole(g, myPid(), "grandchild");
        expectNoConsole(g, c.value("pid").toInteger(), "grandchild");
#ifdef Q_OS_WIN
        QCOMPARE(g.value("stdout").toString(), QString("nul"));
#endif
    }

    // ChannelManager::killChannel uses kill(): it still ends a detached child at once.
    void killStillWorks() {
        QTemporaryDir tmp;
        const QString json = tmp.filePath("probe.json");
        QProcess p;
        processlaunch::configureDetachedChild(p);
        p.setProgram(helper());
        p.setArguments({json, "--hold", "30000"});
        p.start();
        QVERIFY(p.waitForStarted(10000));
        QTRY_VERIFY_WITH_TIMEOUT(!readJson(json).isEmpty(), 10000);
        QElapsedTimer t;
        t.start();
        p.kill();
        QVERIFY(p.waitForFinished(5000));
        QVERIFY(t.elapsed() < 3000);
        QCOMPARE(p.exitStatus(), QProcess::CrashExit);
    }

    // Stand-in for "the operator closed (or Ctrl+C'd) the Windows Terminal tab": inside a console of its own, the
    // helper starts one child the old way (shares the console) and one through configureDetachedChild, then sends
    // CTRL_BREAK to every process on that console. The shared child dies with 0xC000013A, as the stack did on
    // 10-23 07:26; the detached child keeps running.
    void consoleCtrlEventDoesNotReachDetachedChild() {
#ifdef Q_OS_WIN
        QTemporaryDir tmp;
        const QString json = tmp.filePath("h1.json");
        const Run r = runHelper({json, "--ctrl-break", tmp.filePath("ctrl")}, {}, 40000);
        QVERIFY(r.finished);
        const QJsonObject h = readJson(json);
        expectNoConsole(h, myPid(), "console owner");
        const QJsonObject c = h.value("ctrl").toObject();
        QVERIFY2(!c.contains("error"), qPrintable(c.value("error").toString()));
        QVERIFY(c.value("sharedStarted").toBool() && c.value("sharedReady").toBool());
        QVERIFY(c.value("detachedStarted").toBool() && c.value("detachedReady").toBool());
        QVERIFY(c.value("breakSent").toBool());

        const QJsonObject shared = readJson(tmp.filePath("ctrl/shared.json"));
        const QJsonObject detached = readJson(tmp.filePath("ctrl/detached.json"));
        QVERIFY2(hasPid(shared.value("consolePids").toArray(), h.value("pid").toInteger()), "control child did not share the console");
        expectNoConsole(detached, h.value("pid").toInteger(), "detached child");

        QVERIFY2(c.value("sharedExited").toBool(), "the control child survived CTRL_BREAK; the stand-in proves nothing");
        QCOMPARE(c.value("sharedExitCode").toInteger(), kStatusControlCExit);
        QVERIFY2(c.value("detachedSurvived").toBool(), "the detached child was ended by its parent's console event");
#else
        QSKIP("Windows only");
#endif
    }

    // STB-3 (W3): PlayPlay.exe is a GUI-subsystem program, so no start of it (Explorer, start-live.cmd, a terminal)
    // gives it a console: no Windows Terminal tab that can stop draining or be closed under it. The Supervisor and the
    // engine stay console programs on hidden consoles of their own (STB-2). Built in this tree or skipped; a stale exe
    // fails (rebuild PlayPlay).
    void programSubsystems_data() {
        QTest::addColumn<QString>("exe");
        QTest::addColumn<int>("subsystem");
        QTest::newRow("PlayPlay.exe GUI") << QStringLiteral(PP_APP_EXE) << kSubsystemGui;
        QTest::newRow("PlayPlaySupervisor.exe console") << QStringLiteral(PP_SUPERVISOR_EXE) << kSubsystemConsole;
        QTest::newRow("PlayPlayChannelEngine.exe console") << QStringLiteral(PP_ENGINE_EXE) << kSubsystemConsole;
    }
    void programSubsystems() {
        QFETCH(QString, exe);
        QFETCH(int, subsystem);
        if (!QFileInfo::exists(exe)) QSKIP(qPrintable(exe + " is not built in this tree"));
        const int got = peSubsystem(exe);
        QVERIFY2(got == subsystem, qPrintable(QString("%1: subsystem %2, expected %3 (2 = GUI, 3 = console)").arg(exe).arg(got).arg(subsystem)));
    }

    // STB-3 (W3): a PDB next to each program that matches it (DbgHelp exact load) and has source lines for the
    // program's own code and for the static libraries it is made of. Without /Zi on a library its functions still match
    // by public name but have no line, so each mask needs one hit with a line.
    void programsHaveMatchingPdbs_data() {
        QTest::addColumn<QString>("exe");
        QTest::addColumn<QStringList>("masks");
        QTest::newRow("PlayPlay.exe") << QStringLiteral(PP_APP_EXE)
                                      << QStringList{"SupervisorClient::*", "Logger::*"};               // app, PlayPlayCore
        QTest::newRow("PlayPlaySupervisor.exe") << QStringLiteral(PP_SUPERVISOR_EXE)
            << QStringList{"SupervisorCore::*", "sched::ScheduleStore::*", "installFileLogging"};    // own, Schedule, Core
        QTest::newRow("PlayPlayChannelEngine.exe") << QStringLiteral(PP_ENGINE_EXE)
            << QStringList{"main", "PlayoutEngine::*", "installFileLogging"};                        // own, Engine, Core
    }
    void programsHaveMatchingPdbs() {
#ifdef Q_OS_WIN
        QFETCH(QString, exe);
        QFETCH(QStringList, masks);
        if (!QFileInfo::exists(exe)) QSKIP(qPrintable(exe + " is not built in this tree"));
        const QFileInfo fi(exe);
        const QString pdb = fi.absolutePath() + '/' + fi.completeBaseName() + ".pdb";
        QVERIFY2(QFileInfo::exists(pdb), qPrintable("no PDB next to " + exe));
        const SymSession s(exe);
        QVERIFY2(s.loaded(), qPrintable(QString("DbgHelp could not load %1 (error %2)").arg(exe).arg(GetLastError())));
        const IMAGEHLP_MODULEW64& m = s.info();
        const QString loaded = QString::fromWCharArray(m.LoadedPdbName);
        QVERIFY2(m.SymType == SymPdb && !m.PdbUnmatched,
                 qPrintable(QString("%1: symbols type %2, pdb unmatched %3 (%4)").arg(exe).arg(int(m.SymType)).arg(int(m.PdbUnmatched)).arg(loaded)));
        QVERIFY2(samePath(loaded, pdb), qPrintable(QString("loaded %1, expected %2").arg(loaded, pdb)));
        QVERIFY2(m.LineNumbers, "the PDB has no line numbers");
        for (const QString& mask : masks) {
            int n = 0;
            const QString hit = s.withLine(mask, &n);
            QVERIFY2(!hit.isEmpty(), qPrintable(QString("%1: %2 symbols match %3, none with a source line (compiled without /Zi?)").arg(fi.fileName()).arg(n).arg(mask)));
            std::fprintf(stdout, "  %s: %s\n", qPrintable(fi.fileName()), qPrintable(hit));
        }
#else
        QSKIP("Windows only");
#endif
    }

    // W0: start-live.cmd starts the app with NUL stdin/stdout/stderr, so nothing it (or an older console build and its
    // children) writes can reach a terminal tab. A copy of the real script, with BUILD pointed at a scratch folder whose
    // "PlayPlay.exe" is the probe, runs under a cmd whose own stdio are pipes. Control: the same copy without the
    // redirects hands the probe those pipes, so the probe does see the difference.
    void startLiveGivesTheAppNulStdio() {
#ifdef Q_OS_WIN
        const QString src = readText(QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("scripts/start-live.cmd"));
        if (src.isEmpty()) QSKIP("scripts/start-live.cmd is the original station's launcher and is not part of the public source tree");
        const auto ml = QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption;
        // Not a raw string: moc's tokenizer miscounts the odd number of quotes in one and then finds no Q_OBJECT class.
        const QRegularExpression buildRe(QStringLiteral("^set \"BUILD=[^\"\\r\\n]*\"[ \\t]*$"), ml);
        const QRegularExpression launchRe(R"(^start\b[^\r\n]*"%BUILD%\\PlayPlay\.exe"[^\r\n]*$)", ml);
        QCOMPARE(int(src.count(buildRe)), 1);
        const QRegularExpressionMatch b = buildRe.match(src), l = launchRe.match(src);
        QVERIFY2(l.hasMatch(), "start-live.cmd no longer starts \"%BUILD%\\PlayPlay.exe\" with start; update this test");
        const QString launch = l.captured(0);
        for (const char* re : {R"(<\s*NUL\b)", R"(>\s*NUL\b)", R"(2>&1)"})
            QVERIFY2(launch.contains(QRegularExpression(QLatin1String(re), QRegularExpression::CaseInsensitiveOption)),
                     qPrintable(QString("the launch line lost %1: %2").arg(QLatin1String(re), launch)));

        QTemporaryDir tmp;
        const QString build = QDir::toNativeSeparators(tmp.filePath("build"));
        QVERIFY(QDir().mkpath(build));
        QVERIFY(QFile::copy(helper(), build + "\\PlayPlay.exe"));
        // Splice, not QString::replace(regex): a path segment like "\4d80..." would be read as a back-reference.
        QString copy = src;
        copy.replace(b.capturedStart(), b.capturedLength(), QString("set \"BUILD=%1\"").arg(build));
        QVERIFY2(!copy.contains(QLatin1String("%~dp0..\\build"), Qt::CaseInsensitive), "the copy still points at the live build folder");
        QCOMPARE(int(copy.count(buildRe)), 1);
        QString control = copy;
        QString bare = launch;
        bare.remove(QRegularExpression(R"(\s*(<\s*NUL|>\s*NUL|2>&1))", QRegularExpression::CaseInsensitiveOption));
        control.replace(launch, bare);
        QVERIFY(control != copy);
        auto write = [&](const QString& name, QString text) {
            text.replace("\r\n", "\n").replace("\n", "\r\n");
            QFile f(tmp.filePath(name));
            return f.open(QIODevice::WriteOnly) && f.write(text.toLocal8Bit()) > 0;
        };
        QVERIFY(write("start-live.cmd", copy));
        QVERIFY(write("control.cmd", control));

        const QJsonObject o = runScript(tmp.filePath("start-live.cmd"), tmp.filePath("nul.json"));
        QVERIFY2(!o.isEmpty(), "the copy of start-live.cmd did not start the probe");
        for (const char* h : {"stdin", "stdout", "stderr"}) {
            const QString kind = o.value(QLatin1String(h)).toString();
            QVERIFY2(kind == "nul", qPrintable(QString("%1 of the started app is %2, not NUL").arg(QLatin1String(h), kind)));
        }
        const QJsonObject c = runScript(tmp.filePath("control.cmd"), tmp.filePath("control.json"));
        QVERIFY2(!c.isEmpty(), "the control script did not start the probe");
        QCOMPARE(c.value("stdout").toString(), QString("pipe"));
        std::fprintf(stdout, "  start-live copy: stdin/stdout/stderr %s/%s/%s; control without redirects: %s/%s/%s\n",
                     qPrintable(o.value("stdin").toString()), qPrintable(o.value("stdout").toString()), qPrintable(o.value("stderr").toString()),
                     qPrintable(c.value("stdin").toString()), qPrintable(c.value("stdout").toString()), qPrintable(c.value("stderr").toString()));
#else
        QSKIP("Windows only");
#endif
    }

    // Guard against the outage class coming back: the two spawn sites the plan names must launch detached.
    void spawnSitesLaunchDetached_data() {
        QTest::addColumn<QString>("file");
        QTest::newRow("app -> Supervisor") << QStringLiteral("playout/supervisorclient.cpp");
        QTest::newRow("Supervisor -> engine") << QStringLiteral("playout/supervisor/channelmanager.cpp");
    }
    void spawnSitesLaunchDetached() {
        QFETCH(QString, file);
        const QString src = readText(QDir(QStringLiteral(PP_SOURCE_DIR)).filePath(file));
        QVERIFY2(!src.isEmpty(), qPrintable(file));
        QVERIFY2(!src.contains(QRegularExpression(R"(set(Process|Input)ChannelMode\s*\(\s*QProcess::Forwarded)")),
                 qPrintable(file + " forwards its console to a child again (use processlaunch::configureDetachedChild)"));
        const int spawns = int(src.count(QRegularExpression(R"(new\s+QProcess\b)")));
        const int detachedCalls = int(src.count(QRegularExpression(R"(processlaunch::configureDetachedChild\s*\()")));
        QVERIFY2(spawns > 0, qPrintable(file + " no longer creates a QProcess; move this guard to the new spawn site"));
        QVERIFY2(detachedCalls >= spawns, qPrintable(QString("%1: %2 QProcess, %3 configureDetachedChild").arg(file).arg(spawns).arg(detachedCalls)));
    }
};

QTEST_GUILESS_MAIN(TestProcessLaunch)
#include "test_processlaunch.moc"
