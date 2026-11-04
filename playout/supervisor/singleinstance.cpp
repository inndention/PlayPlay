#include "singleinstance.h"
#include "enginereaper.h"
#include "../core/enginelock.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QThread>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <dbghelp.h>
#endif

// The mutex mechanics (create unowned, wait, abandoned = ours, release on the owning thread) are enginelock's
// InstanceGuard, the same guard the engines use per channel (PS-3); only the name differs.

SingleInstance::SingleInstance(const QString& ipcName)
    : m_guard(std::make_unique<enginelock::InstanceGuard>(mutexName(ipcName))) {}

SingleInstance::~SingleInstance() = default;

bool SingleInstance::tryAcquire(int waitMs) { return m_guard->acquire(waitMs); }
bool SingleInstance::owned() const { return m_guard->owned(); }
bool SingleInstance::abandoned() const { return m_guard->abandoned(); }
QString SingleInstance::name() const { return m_guard->name(); }
QString SingleInstance::error() const { return m_guard->error(); }

QString SingleInstance::mutexName(const QString& ipcName) {
    QString n = QStringLiteral("PlayPlaySupervisor-") + reaper::normalizeIpcName(ipcName);
    n.replace(QLatin1Char('\\'), QLatin1Char('_'));   // the only character a name may not contain after "Local\"
    return QStringLiteral("Local\\") + n.left(240);
}

// --- supguard (PS-4b) ------------------------------------------------------------------------------------------------

namespace supguard {

Health probeHealth(quint16 port, int timeoutMs) {
    Health h;
    if (!port) return h;
    QElapsedTimer t;
    t.start();
    QTcpSocket s;
    s.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    if (!s.waitForConnected(timeoutMs)) return h;
    h.connected = true;
    s.write("GET /healthz HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    QByteArray buf;
    auto tryParse = [&]() {
        const int at = buf.indexOf("\r\n\r\n");
        if (at < 0) return false;
        const QJsonDocument d = QJsonDocument::fromJson(buf.mid(at + 4));
        if (!d.isObject()) return false;
        const QJsonObject o = d.object();
        h.answered = true;
        h.state = o.value("state").toString();
        h.pid = qint64(o.value("pid").toDouble());
        h.raw = QString::fromUtf8(buf.mid(at + 4)).trimmed();
        return true;
    };
    while (t.elapsed() < timeoutMs) {
        const bool more = s.bytesAvailable() > 0 || s.waitForReadyRead(int(qMax<qint64>(1, timeoutMs - t.elapsed())));
        buf += s.readAll();
        if (tryParse() || !more) break;
    }
    // Still open with no reply: the endpoint is stuck. Closed (or a reply that is not ours): no evidence either way.
    if (!h.answered && s.state() == QAbstractSocket::ConnectedState) h.timedOut = true;
    return h;
}

bool looksHung(const Health& h) {
    if (h.answered) return h.state == QLatin1String("hang") || h.state == QLatin1String("stall");
    return h.connected && h.timedOut;
}

Action decide(bool acquired, const Health& h, bool replaceHung) {
    if (acquired) return Action::Run;
    if (h.answered && (h.state == QLatin1String("ok") || h.state == QLatin1String("exiting"))) return Action::ExitRunning;
    // Replace only on positive evidence of a hang (the watchdog says hang, a stall that did not clear, or its port took
    // the connection and never answered). Refused / no port / an unknown state: cannot judge, never replaced.
    return (replaceHung && looksHung(h)) ? Action::Replace : Action::ExitRunning;
}

const char* actionName(Action a) {
    switch (a) {
    case Action::Run: return "run";
    case Action::ExitRunning: return "exit";
    case Action::Replace: return "replace";
    }
    return "?";
}

QList<qint64> findSupervisorPids(const QString& ipcName, const QString& exeName) {
    QList<qint64> out;
    const qint64 self = QCoreApplication::applicationPid();
    for (const reaper::ProcessEntry& e : reaper::listProcesses(exeName)) {
        if (e.pid == self) continue;
        const QStringList argv = reaper::splitCommandLine(reaper::processCommandLine(e.pid));
        if (argv.isEmpty()) continue;            // unreadable: never guessed
        QString ipc = QStringLiteral("PlayPlaySupervisor");
        for (int i = 1; i < argv.size(); ++i) {
            const QString& a = argv[i];
            if ((a == "--ipc-name" || a == "-i") && i + 1 < argv.size()) ipc = argv[++i];
            else if (a.startsWith("--ipc-name=")) ipc = a.mid(11);
            else if (a.startsWith("-i=")) ipc = a.mid(3);
        }
        if (reaper::sameIpc(ipc, ipcName)) out << e.pid;
    }
    return out;
}

QString dumpProcess(qint64 pid, const QString& dir, const QString& tag, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return QString(); };
#ifdef Q_OS_WIN
    using Fn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                              PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
    const Fn write = dbg ? reinterpret_cast<Fn>(GetProcAddress(dbg, "MiniDumpWriteDump")) : nullptr;
    if (!write) return fail(QStringLiteral("dbghelp.dll / MiniDumpWriteDump not available"));
    HANDLE p = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(pid));
    if (!p) return fail(QString("cannot open pid %1 for a dump (Windows error %2)").arg(pid).arg(GetLastError()));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QString("supervisor-%1-%2-%3.dmp").arg(pid)
                                                .arg(QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmssZ"), tag));
    HANDLE f = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { CloseHandle(p); return fail(QString("cannot create %1 (Windows error %2)").arg(path).arg(GetLastError())); }
    const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    const BOOL ok = write(p, DWORD(pid), f, type, nullptr, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(f);
    CloseHandle(p);
    if (!ok) { QFile::remove(path); return fail(QString("MiniDumpWriteDump failed (error %1)").arg(err)); }
    return path;
#else
    Q_UNUSED(pid); Q_UNUSED(dir); Q_UNUSED(tag);
    return fail(QStringLiteral("not supported on this platform"));
#endif
}

bool terminateAndWait(qint64 pid, int waitMs, QString* error) {
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) {
        const DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) return true;   // already gone
        if (error) *error = QString("cannot open pid %1 (Windows error %2)").arg(pid).arg(e);
        return false;
    }
    if (!TerminateProcess(h, 1) && WaitForSingleObject(h, 0) != WAIT_OBJECT_0) {
        if (error) *error = QString("TerminateProcess(pid %1) failed (Windows error %2)").arg(pid).arg(GetLastError());
        CloseHandle(h);
        return false;
    }
    const bool gone = WaitForSingleObject(h, DWORD(qMax(0, waitMs))) == WAIT_OBJECT_0;
    CloseHandle(h);
    if (!gone && error) *error = QString("pid %1 has not exited %2 ms after TerminateProcess").arg(pid).arg(waitMs);
    return gone;
#else
    Q_UNUSED(pid); Q_UNUSED(waitMs);
    if (error) *error = QStringLiteral("not supported on this platform");
    return false;
#endif
}

Result acquireOrReplace(SingleInstance& guard, const Options& o) {
    Result r;
    if (guard.tryAcquire(0)) {
        r.run = true;
        if (guard.abandoned()) r.message = QStringLiteral("the previous Supervisor on this IPC name ended without releasing its guard");
        return r;
    }
    if (!guard.error().isEmpty() && !guard.owned()) {
        // Fail open: a guard that cannot be created must not keep the station's Supervisor from starting.
        r.run = true;
        r.message = QString("single-instance guard %1 unavailable (%2); starting without it").arg(guard.name(), guard.error());
        return r;
    }
    // A predecessor that is just shutting down (a restart) releases it in a moment.
    if (o.predecessorWaitMs > 0 && guard.tryAcquire(o.predecessorWaitMs)) { r.run = true; return r; }

    Health h = probeHealth(o.healthPort, o.healthTimeoutMs);
    for (int i = 0; i < o.stallRetries && h.answered && h.state == QLatin1String("stall"); ++i) {
        QThread::msleep(1000);
        h = probeHealth(o.healthPort, o.healthTimeoutMs);
    }
    const Action a = decide(false, h, o.replaceHung);
    const QString health = h.answered ? QString("/healthz on port %1 says %2 (pid %3)").arg(o.healthPort).arg(h.state.isEmpty() ? "?" : h.state).arg(h.pid)
                         : !o.healthPort ? QStringLiteral("no /healthz port to ask")
                         : h.timedOut ? QString("/healthz on port %1 took the connection and did not answer within %2 ms").arg(o.healthPort).arg(o.healthTimeoutMs)
                         : h.connected ? QString("port %1 answered something that is not a /healthz reply").arg(o.healthPort)
                                       : QString("nothing answers /healthz on port %1").arg(o.healthPort);
    if (a == Action::ExitRunning) {
        r.exitCode = kExitAlreadyRunning;
        if (h.answered && (h.state == "ok" || h.state == "exiting"))
            r.message = QString("another Supervisor is already running on IPC %1 (%2); exiting").arg(o.ipcName, health);
        else if (looksHung(h))
            r.message = QString("another Supervisor holds IPC %1 and looks hung (%2); exiting (start with --replace-hung to replace it)").arg(o.ipcName, health);
        else
            r.message = QString("another Supervisor holds IPC %1 and its health cannot be checked (%2); exiting%3")
                            .arg(o.ipcName, health, o.replaceHung ? QStringLiteral(" (--replace-hung replaces only a holder whose /healthz reports a hang)") : QString());
        return r;
    }
    // Replace: only a process we can name for certain.
    const QString exe = o.exeName.isEmpty() ? QFileInfo(QCoreApplication::applicationFilePath()).fileName() : o.exeName;
    const QList<qint64> cands = findSupervisorPids(o.ipcName, exe);
    const qint64 target = (h.pid > 0 && cands.contains(h.pid)) ? h.pid : cands.size() == 1 ? cands.first() : 0;
    if (!target) {
        r.exitCode = kExitAlreadyRunning;
        r.message = QString("another Supervisor holds IPC %1 and looks hung (%2), but %3 candidate process(es) named %4 were found; not replacing a process that cannot be named for certain")
                        .arg(o.ipcName, health).arg(cands.size()).arg(exe);
        return r;
    }
    r.oldPid = target;
    QString err;
    r.dumpPath = dumpProcess(target, o.dumpDir, QStringLiteral("replace-hung"), &err);
    const QString dumpNote = r.dumpPath.isEmpty() ? QString("no dump: %1").arg(err) : QString("dump %1").arg(QDir::toNativeSeparators(r.dumpPath));
    if (!terminateAndWait(target, o.terminateWaitMs, &err)) {
        r.exitCode = kExitAlreadyRunning;
        r.message = QString("--replace-hung: could not end the hung Supervisor pid %1 (%2; %3); exiting").arg(target).arg(err, dumpNote);
        return r;
    }
    if (!guard.tryAcquire(2000)) {
        r.exitCode = kExitAlreadyRunning;
        r.message = QString("--replace-hung: Supervisor pid %1 ended but the guard is still held (another instance?); exiting").arg(target);
        return r;
    }
    r.run = true;
    r.message = QString("--replace-hung: replaced the hung Supervisor pid %1 on IPC %2 (%3; %4); its engines are adopted by the orphan scan")
                    .arg(target).arg(o.ipcName, health, dumpNote);
    return r;
}

} // namespace supguard
