// Spawned by test_processlaunch (STB-2): reports how it was launched. Its stdio may be NUL, so every answer goes to
// the JSON file named first; stdout/stderr are only written when asked (--write / --marker).
//
//   console_probe_helper <result.json> [--write <bytes>] [--marker <text>] [--hold <ms>] [--exit <code>]
//                        [--spawn <child.json>]
//                        [--ctrl-break <dir> [--wait-exit <ms>] [--ctrl-break-exe <exe> <args...>]]
//
//   result: {pid, consoleWindow, consolePids[], stdin/stdout/stderr: "nul"|"console"|"pipe"|"disk"|"none"|"char:<name>",
//            stdinEof, wrote, writeMs, spawn{...}, ctrl{...}}
//   --spawn       launches a copy of itself through processlaunch::configureDetachedChild (app -> Supervisor -> engine).
//   --ctrl-break  stands in for "the operator closed the Windows Terminal tab": starts one child that shares this
//                 console (creation flags 0, what ForwardedChannels did from a console app) and one through
//                 configureDetachedChild (or the given exe), then sends CTRL_BREAK to every process on this console.
//                 Run it only inside a console of its own (the test launches it detached), never in a user's terminal.
#include "playout/core/processlaunch.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <cstdio>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

void writeJson(const QString& path, const QJsonObject& o) {
    QSaveFile f(path);                                     // atomic: a polling test never reads half a file
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson()); f.commit(); }
}

QJsonObject readJson(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject();
}

#ifdef Q_OS_WIN
// Kernel object name of a handle ("\Device\Null" for NUL). Only asked for FILE_TYPE_CHAR handles: NtQueryObject can
// hang on a synchronous pipe with a pending read, and those never get here.
QString objectName(HANDLE h) {
    struct UStr { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
    using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto fn = reinterpret_cast<NtQueryObjectFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
    if (!fn) return {};
    alignas(8) unsigned char buf[2048] = {};
    ULONG len = 0;
    if (fn(h, 1 /* ObjectNameInformation */, buf, sizeof(buf), &len) < 0) return {};
    const UStr* s = reinterpret_cast<const UStr*>(buf);
    return s->Buffer ? QString::fromWCharArray(s->Buffer, s->Length / 2) : QString();
}

QString handleKind(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return "none";
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) return "console";
    switch (GetFileType(h)) {
    case FILE_TYPE_DISK: return "disk";
    case FILE_TYPE_PIPE: return "pipe";
    case FILE_TYPE_CHAR: {
        const QString n = objectName(h);
        return n.compare("\\Device\\Null", Qt::CaseInsensitive) == 0 ? QStringLiteral("nul") : "char:" + n;
    }
    default: return "unknown";
    }
}

QJsonArray consolePids() {
    DWORD pids[64] = {};
    const DWORD n = GetConsoleProcessList(pids, 64);
    QJsonArray a;
    for (DWORD i = 0; i < n && i < 64; ++i) a.append(qint64(pids[i]));
    return a;
}
#endif

QJsonObject probe() {
    QJsonObject r{{"pid", qint64(QCoreApplication::applicationPid())}};
#ifdef Q_OS_WIN
    r["consoleWindow"] = GetConsoleWindow() != nullptr;
    r["consolePids"] = consolePids();
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    r["stdin"] = handleKind(in);
    r["stdout"] = handleKind(GetStdHandle(STD_OUTPUT_HANDLE));
    r["stderr"] = handleKind(GetStdHandle(STD_ERROR_HANDLE));
    if (r["stdin"] == "nul") { char c; DWORD n = 0; r["stdinEof"] = ReadFile(in, &c, 1, &n, nullptr) && n == 0; }
#endif
    return r;
}

void writeBoth(const QByteArray& out, const QByteArray& err) {
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fflush(stdout);
    std::fwrite(err.constData(), 1, size_t(err.size()), stderr);
    std::fflush(stderr);
}

// Waits for a child's result file (it writes it right after start) so the child is fully up before we signal it.
bool waitForFile(const QString& path, int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) { if (!readJson(path).isEmpty()) return true; QThread::msleep(20); }
    return false;
}

#ifdef Q_OS_WIN
BOOL WINAPI survive(DWORD) { return TRUE; }               // this process must outlive its own CTRL_BREAK

QJsonObject ctrlBreak(const QString& dir, const QString& self, const QStringList& exeArgs, int waitExitMs) {
    QJsonObject r;
    QDir().mkpath(dir);
    const qint64 me = QCoreApplication::applicationPid();
    if (consolePids() != QJsonArray{me}) { r["error"] = "this console is not ours alone; not sending CTRL_BREAK"; return r; }
    SetConsoleCtrlHandler(survive, TRUE);

    // Old behaviour: creation flags 0 from a process with a console, so the child attaches to this console.
    const QString sharedJson = QDir(dir).filePath("shared.json");
    std::wstring cmd = QString("\"%1\" \"%2\" --hold 20000").arg(QDir::toNativeSeparators(self), sharedJson).toStdWString();
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    const bool sharedStarted = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    r["sharedStarted"] = sharedStarted;
    r["sharedPid"] = sharedStarted ? qint64(pi.dwProcessId) : 0;

    // New behaviour: the helper itself, or the real program under test (e.g. an engine --standalone).
    const QString detachedJson = QDir(dir).filePath("detached.json");
    QProcess detached;
    processlaunch::configureDetachedChild(detached);
    if (exeArgs.isEmpty()) { detached.setProgram(self); detached.setArguments({detachedJson, "--hold", "20000"}); }
    else { detached.setProgram(exeArgs.first()); detached.setArguments(exeArgs.mid(1)); }
    detached.start();
    r["detachedStarted"] = detached.waitForStarted(10000);
    r["detachedPid"] = qint64(detached.processId());

    r["sharedReady"] = sharedStarted && waitForFile(sharedJson, 10000);
    if (exeArgs.isEmpty()) r["detachedReady"] = waitForFile(detachedJson, 10000);
    else QThread::msleep(1500);                            // a foreign program writes no result file: let it start up
    const QJsonArray before = consolePids();
    r["consolePidsBefore"] = before;

    // Safety: only this process and the shared child may be on this console, or the break would hit someone else.
    for (const QJsonValue& v : before) {
        if (v.toInteger() != me && v.toInteger() != r["sharedPid"].toInteger()) {
            r["error"] = "unexpected process on this console; not sending CTRL_BREAK";
            if (sharedStarted) { TerminateProcess(pi.hProcess, 99); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            detached.kill();
            detached.waitForFinished(5000);
            return r;
        }
    }
    r["breakSent"] = bool(GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0));   // 0 = every process on this console
    if (sharedStarted) {
        r["sharedExited"] = WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0;
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        r["sharedExitCode"] = qint64(code);
        if (code == STILL_ACTIVE) { TerminateProcess(pi.hProcess, 99); WaitForSingleObject(pi.hProcess, 3000); }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    // The detached child must still be running well after the shared one died.
    r["detachedSurvived"] = !detached.waitForFinished(1000) && detached.state() == QProcess::Running;
    if (!exeArgs.isEmpty() && waitExitMs > 0 && detached.state() == QProcess::Running) {
        r["detachedExitedOnItsOwn"] = detached.waitForFinished(waitExitMs);
        if (detached.state() == QProcess::NotRunning) r["detachedExitCode"] = detached.exitCode();
    }
    if (detached.state() != QProcess::NotRunning) { detached.kill(); detached.waitForFinished(5000); }
    return r;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 2) return 2;
    const QString resultPath = a.at(1);
    qint64 writeBytes = 0;
    int holdMs = 0, exitCode = 0, waitExitMs = 0;
    QString marker, spawnJson, ctrlDir;
    QStringList ctrlExe;
    for (int i = 2; i < a.size(); ++i) {
        const QString k = a.at(i);
        const QString v = i + 1 < a.size() ? a.at(i + 1) : QString();
        if (k == "--write") { writeBytes = v.toLongLong(); ++i; }
        else if (k == "--marker") { marker = v; ++i; }
        else if (k == "--hold") { holdMs = v.toInt(); ++i; }
        else if (k == "--exit") { exitCode = v.toInt(); ++i; }
        else if (k == "--spawn") { spawnJson = v; ++i; }
        else if (k == "--ctrl-break") { ctrlDir = v; ++i; }
        else if (k == "--wait-exit") { waitExitMs = v.toInt(); ++i; }
        else if (k == "--ctrl-break-exe") { ctrlExe = a.mid(i + 1); break; }
    }

    QJsonObject r = probe();
    writeJson(resultPath, r);                               // early copy: a test may kill a --hold helper

    if (!marker.isEmpty()) writeBoth(marker.toUtf8() + "-out\n", marker.toUtf8() + "-err\n");
    if (writeBytes > 0) {
        QElapsedTimer t;
        t.start();
        const QByteArray chunk(64 * 1024, 'x');
        qint64 left = writeBytes;
        while (left > 0) {
            const qint64 n = qMin<qint64>(left, chunk.size());
            writeBoth(chunk.left(int(n)), chunk.left(int(n)));
            left -= n;
        }
        r["wrote"] = writeBytes;
        r["writeMs"] = t.elapsed();
    }
    if (!spawnJson.isEmpty()) {
        QProcess child;
        processlaunch::configureDetachedChild(child);
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({spawnJson});
        child.start();
        const bool started = child.waitForStarted(10000);
        const bool finished = started && child.waitForFinished(15000);
        r["spawn"] = QJsonObject{{"started", started}, {"finished", finished}, {"pid", qint64(child.processId())},
                                 {"exitCode", finished ? child.exitCode() : -1},
                                 {"channelMode", int(child.processChannelMode())}};
    }
#ifdef Q_OS_WIN
    if (!ctrlDir.isEmpty()) r["ctrl"] = ctrlBreak(ctrlDir, QCoreApplication::applicationFilePath(), ctrlExe, waitExitMs);
#endif
    if (holdMs > 0) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < holdMs) QThread::msleep(50);
    }
    writeJson(resultPath, r);
    return exitCode;
}
