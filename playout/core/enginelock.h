#ifndef ENGINELOCK_H
#define ENGINELOCK_H

#include <QJsonObject>
#include <QString>
#include <atomic>
#include <memory>
#include <thread>

/**
 * enginelock — what a channel engine leaves behind for the Supervisor that finds it later, and the one-engine-per-
 * channel guard (perf-supervisor PS-3 / W5, design docs/plans/perf-supervisor-app.md §6.1).
 *
 * engine.lock (<channel data dir>/engine.lock) is written atomically (temp file + rename) when an engine that is linked
 * to a Supervisor starts and again whenever its Supervisor pid changes, and removed on a clean exit (only while it still
 * names this process). It identifies the process exactly (pid + creation time: a reused pid has another creation time),
 * the build (exe path + mtime), the start config (hash) and the Supervisor scope (IPC name, pid, data dir). PS-4a's
 * reaper reads it to adopt or kill the engines a dead Supervisor left running.
 *
 * InstanceGuard is the named mutex Local\PlayPlayEngine-<ipcName>-<channelId>: one engine per channel per Supervisor
 * scope. A second engine for the same channel waits up to 5 s and then exits with kExitDuplicate instead of fighting
 * the first for its NDI name, SDI port and UDP group (10-24: NDIlib_send_create retries, two streams on one group).
 * The OS releases the mutex when its owner dies, however it dies.
 */
namespace enginelock {

constexpr int kExitDuplicate = 4;   // another engine owns this channel (guard held, or the Supervisor refused us)
constexpr int kExitOrphan = 5;      // the Supervisor went away and no Supervisor adopted this engine in time

// PS-4b: a Supervisor hands its pid and the takeover flag to the engines it spawns in the environment, not on the
// command line. An engine exe older than PS-3 rejects unknown options (QCommandLineParser::process exits 1), so a
// Supervisor rebuilt on its own would otherwise fail every spawn; older engines simply ignore these variables. The
// engine's --supervisor-pid / --takeover options still work and win over the environment.
constexpr char kEnvSupervisorPid[] = "PLAYPLAY_SUPERVISOR_PID";
constexpr char kEnvTakeover[] = "PLAYPLAY_ENGINE_TAKEOVER";   // "1" = as --takeover
struct LaunchEnv { qint64 supervisorPid = 0; bool takeover = false; };
// Reads both variables and removes them from this process's environment, so the engine's own children (and anything
// they start) never inherit a Supervisor pid or a takeover meant for this engine.
LaunchEnv takeLaunchEnv();

struct LockInfo {
    QString channelId;
    qint64 pid = 0;
    quint64 createTime = 0;      // process creation time, FILETIME ticks (100 ns since 1601): exact, stored as a string
    QString exe;                 // native path of the executable
    qint64 exeMtimeMs = 0;       // its last-modified time (ms since epoch): the build check
    QString configHash;          // SHA-1 of the start config file (empty without one)
    QString supervisorIpc;
    qint64 supervisorPid = 0;    // 0 = not known yet
    QString dataDir;
    QString version;             // application version

    QJsonObject toJson() const;
    static LockInfo fromJson(const QJsonObject& o, bool* ok = nullptr);   // ok: channelId and pid present
};

QString lockPath(const QString& dataDir);                 // <dataDir>/engine.lock
LockInfo currentProcessInfo();                            // pid, createTime, exe, exeMtimeMs, version of this process
bool writeLock(const QString& path, const LockInfo& info, QString* error = nullptr);   // temp file + rename
bool readLock(const QString& path, LockInfo* info, QString* error = nullptr);
bool removeLockIfOwned(const QString& path, qint64 pid);  // removes it only while it names pid; true if removed

// Process facts (Windows; 0 / empty elsewhere or when the process cannot be opened).
quint64 processCreateTime(qint64 pid);
QString processImagePath(qint64 pid);
bool processAlive(qint64 pid);
qint64 createTimeToMs(quint64 fileTimeTicks);             // FILETIME ticks -> ms since 1970

// "Local\PlayPlayEngine-<ipcName>-<channelId>": a pipe path prefix is stripped and backslashes become '_' (the only
// character a mutex name may not contain after its namespace).
QString mutexName(const QString& ipcName, const QString& channelId);

class InstanceGuard {
public:
    explicit InstanceGuard(const QString& name);
    ~InstanceGuard();   // releases the mutex on the thread that owns it
    InstanceGuard(const InstanceGuard&) = delete;
    InstanceGuard& operator=(const InstanceGuard&) = delete;

    // Blocks the calling thread up to waitMs (engine start, before any event loop). True when this process owns the
    // mutex, also when its previous owner died without releasing it (abandoned()).
    bool acquire(int waitMs);
    // --takeover: the Supervisor killed the previous owner, which may not have exited yet. A thread keeps waiting for
    // the mutex and holds it once it gets it, so a later duplicate is still refused.
    void acquireInBackground();
    bool owned() const { return m_ownedHere || m_ownedByThread.load(); }
    bool abandoned() const { return m_abandoned.load(); }
    QString name() const { return m_name; }
    QString error() const { return m_error; }

private:
    QString m_name;
    QString m_error;
    void* m_handle = nullptr;
    void* m_stopEvent = nullptr;              // ends the takeover thread
    bool m_ownedHere = false;                 // owned by the thread that called acquire()
    std::atomic<bool> m_ownedByThread{false};
    std::atomic<bool> m_abandoned{false};
    std::unique_ptr<std::thread> m_thread;
};

} // namespace enginelock

#endif // ENGINELOCK_H
