#include "enginelock.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <string>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace enginelock {

namespace {
#ifdef Q_OS_WIN
quint64 ticksOf(const FILETIME& ft) { return (quint64(ft.dwHighDateTime) << 32) | quint64(ft.dwLowDateTime); }

quint64 createTimeOf(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(h, &c, &e, &k, &u) ? ticksOf(c) : 0;
}
#endif
} // namespace

LaunchEnv takeLaunchEnv() {
    LaunchEnv e;
    e.supervisorPid = qMax<qint64>(0, qgetenv(kEnvSupervisorPid).trimmed().toLongLong());
    e.takeover = qgetenv(kEnvTakeover).trimmed() == "1";
    qunsetenv(kEnvSupervisorPid);
    qunsetenv(kEnvTakeover);
    return e;
}

QJsonObject LockInfo::toJson() const {
    QJsonObject o;
    o["channelId"] = channelId;
    o["pid"] = double(pid);
    o["createTime"] = QString::number(createTime);     // 1.3e17 does not fit a JSON double exactly
    o["createTimeMs"] = double(createTimeToMs(createTime));
    o["exe"] = exe;
    o["exeMtime"] = double(exeMtimeMs);
    o["configHash"] = configHash;
    o["supervisorIpc"] = supervisorIpc;
    o["supervisorPid"] = double(supervisorPid);
    o["dataDir"] = dataDir;
    o["version"] = version;
    return o;
}

LockInfo LockInfo::fromJson(const QJsonObject& o, bool* ok) {
    LockInfo i;
    i.channelId = o.value("channelId").toString();
    i.pid = qint64(o.value("pid").toDouble());
    i.createTime = o.value("createTime").toString().toULongLong();
    i.exe = o.value("exe").toString();
    i.exeMtimeMs = qint64(o.value("exeMtime").toDouble());
    i.configHash = o.value("configHash").toString();
    i.supervisorIpc = o.value("supervisorIpc").toString();
    i.supervisorPid = qint64(o.value("supervisorPid").toDouble());
    i.dataDir = o.value("dataDir").toString();
    i.version = o.value("version").toString();
    if (ok) *ok = !i.channelId.isEmpty() && i.pid > 0;
    return i;
}

QString lockPath(const QString& dataDir) { return QDir(dataDir).filePath(QStringLiteral("engine.lock")); }

LockInfo currentProcessInfo() {
    LockInfo i;
    i.pid = QCoreApplication::applicationPid();
#ifdef Q_OS_WIN
    i.createTime = createTimeOf(GetCurrentProcess());
#endif
    const QString exe = QCoreApplication::applicationFilePath();
    i.exe = QDir::toNativeSeparators(exe);
    i.exeMtimeMs = QFileInfo(exe).lastModified().toMSecsSinceEpoch();
    i.version = QCoreApplication::applicationVersion();
    return i;
}

bool writeLock(const QString& path, const LockInfo& info, QString* error) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile writes a temporary file next to the target and renames it over the old one on commit (MoveFileEx with
    // REPLACE_EXISTING), so a reader sees the old lock or the new one, never half a file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = f.errorString(); return false; }
    f.write(QJsonDocument(info.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    return true;
}

bool readLock(const QString& path, LockInfo* info, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return false; }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.read(64 * 1024), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) { if (error) *error = pe.errorString(); return false; }
    bool ok = false;
    const LockInfo i = LockInfo::fromJson(doc.object(), &ok);
    if (!ok) { if (error) *error = QStringLiteral("no channelId / pid"); return false; }
    if (info) *info = i;
    return true;
}

bool removeLockIfOwned(const QString& path, qint64 pid) {
    LockInfo i;
    if (!readLock(path, &i) || i.pid != pid) return false;   // a successor's lock (it took over) stays
    return QFile::remove(path);
}

quint64 processCreateTime(qint64 pid) {
#ifdef Q_OS_WIN
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return 0;
    const quint64 t = createTimeOf(h);
    CloseHandle(h);
    return t;
#else
    Q_UNUSED(pid); return 0;
#endif
}

QString processImagePath(qint64 pid) {
#ifdef Q_OS_WIN
    if (pid <= 0) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return {};
    std::wstring buf(32768, L'\0');
    DWORD n = DWORD(buf.size());
    const bool ok = QueryFullProcessImageNameW(h, 0, buf.data(), &n) != 0;
    CloseHandle(h);
    return ok ? QString::fromWCharArray(buf.data(), int(n)) : QString();
#else
    Q_UNUSED(pid); return {};
#endif
}

bool processAlive(qint64 pid) {
#ifdef Q_OS_WIN
    if (pid <= 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   // exists, but not ours to open
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    Q_UNUSED(pid); return false;
#endif
}

qint64 createTimeToMs(quint64 t) {
    if (!t) return 0;
    return qint64(t / 10000ULL) - 11644473600000LL;   // 1601 -> 1970
}

QString mutexName(const QString& ipcName, const QString& channelId) {
    QString ipc = ipcName;
    const QString pipePrefix = QStringLiteral("\\\\.\\pipe\\");
    if (ipc.startsWith(pipePrefix, Qt::CaseInsensitive)) ipc = ipc.mid(pipePrefix.size());
    QString n = QStringLiteral("PlayPlayEngine-") + ipc + QLatin1Char('-') + channelId;
    n.replace(QLatin1Char('\\'), QLatin1Char('_'));
    return QStringLiteral("Local\\") + n.left(240);   // MAX_PATH bound
}

// --- InstanceGuard -------------------------------------------------------------------------------------------------

InstanceGuard::InstanceGuard(const QString& name) : m_name(name) {
#ifdef Q_OS_WIN
    // Not initially owned: ownership is taken by acquire() (or the takeover thread) with a wait, which also reports an
    // abandoned mutex (owner died) as ours.
    m_handle = CreateMutexW(nullptr, FALSE, reinterpret_cast<const wchar_t*>(name.utf16()));
    if (!m_handle) m_error = QString("CreateMutex failed (Windows error %1)").arg(GetLastError());
#endif
}

InstanceGuard::~InstanceGuard() {
#ifdef Q_OS_WIN
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_thread && m_thread->joinable()) m_thread->join();   // the thread releases what it owns before it ends
    if (m_stopEvent) CloseHandle(m_stopEvent);
    if (m_handle) {
        if (m_ownedHere) ReleaseMutex(m_handle);
        CloseHandle(m_handle);
    }
#endif
}

bool InstanceGuard::acquire(int waitMs) {
#ifdef Q_OS_WIN
    if (!m_handle) return false;
    if (owned()) return true;
    const DWORD r = WaitForSingleObject(m_handle, DWORD(qMax(0, waitMs)));
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
        m_ownedHere = true;
        if (r == WAIT_ABANDONED) m_abandoned = true;
        return true;
    }
    if (r == WAIT_FAILED) m_error = QString("waiting for the mutex failed (Windows error %1)").arg(GetLastError());
    return false;
#else
    Q_UNUSED(waitMs); return true;   // no guard off Windows
#endif
}

void InstanceGuard::acquireInBackground() {
#ifdef Q_OS_WIN
    if (!m_handle || owned() || m_thread) return;
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) { m_error = QString("CreateEvent failed (Windows error %1)").arg(GetLastError()); return; }
    HANDLE mutex = m_handle, stop = m_stopEvent;
    m_thread = std::make_unique<std::thread>([this, mutex, stop]() {
        HANDLE both[2] = {stop, mutex};
        const DWORD r = WaitForMultipleObjects(2, both, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 + 1 && r != WAIT_ABANDONED_0 + 1) return;   // stopped (or failed) before getting it
        if (r == WAIT_ABANDONED_0 + 1) m_abandoned = true;
        m_ownedByThread = true;
        WaitForSingleObject(stop, INFINITE);   // hold it: a mutex belongs to the thread that took it
        ReleaseMutex(mutex);
        m_ownedByThread = false;
    });
#endif
}

} // namespace enginelock
