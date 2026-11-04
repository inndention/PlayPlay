#ifndef CHANNELMANAGER_H
#define CHANNELMANAGER_H

#include <QObject>
#include <QProcess>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include "../ui_ipc/ipcprotocol.h"
#include "enginereaper.h"

class IpcTransport;
class QWinEventNotifier;
namespace sched { struct ChannelProfile; }

namespace chanmgr {

// Restart policy (perf-supervisor PS-2, C6). Several parts of the Supervisor ask for a restart after one engine death:
// the heartbeat watchdog here, SupervisorCore 1.5 s after channelDied, FailoverController while a channel is not
// running, MSG_ENGINE_FATAL. Each request used to kill and respawn, so the later ones killed the engine the first one
// had just started. Pure and table-tested (test_channelmanager): one death gives one respawn.
enum class RestartVerdict {
    Proceed,       // kill the current engine (if any) and spawn a new one once it has exited
    JoinPending,   // a restart is already waiting for the old engine to exit: this request is the same restart
    Coalesce,      // the current engine was started by a restart moments ago and is alive: a duplicate request
    Refuse         // unknown channel, or an engine this manager did not spawn (no data dir / config to respawn from)
};
struct RestartFacts {
    bool known = false;                 // a record exists
    bool managed = false;               // spawned by this manager (it has the data dir and config to respawn)
    bool pending = false;               // a restart / spawn is waiting for the previous engine to exit
    bool alive = false;                 // the current engine process is starting or running
    qint64 sinceRestartSpawnMs = -1;    // ms since the current engine was spawned by a restart; -1 = not by a restart
    bool internal = false;              // the manager's own death detection (always a new death, never a duplicate)
};
RestartVerdict decideRestart(const RestartFacts& f, int coalesceMs);
const char* verdictName(RestartVerdict v);

} // namespace chanmgr

/**
 * ChannelManager — lives in the Supervisor process.
 *
 * Spawns, monitors, and respawns per-channel Engine processes, and routes
 * messages to them over the Supervisor's IPC server once an engine has
 * identified itself (MSG_ENGINE_READY / heartbeat carry the channelId; the
 * transport tells us which socket it came from).
 *
 * Never blocks the event loop (PS-2, W14): killChannel / restartChannel / removeChannel issue the kill and return. A
 * killed engine is "retiring" until QProcess::finished reports its exit (engineExited); a replacement for the same
 * channel is spawned only after that, so the NDI name, SDI port and UDP output are free first. If the old process has
 * not exited killWaitMs() after the kill, the replacement is spawned anyway (the old code waited 3 + 2 s and then did
 * the same) and the old QProcess stays tracked until it goes; a running QProcess is never deleted (its destructor
 * would wait up to 30 s). restartChannel() requests are coalesced per chanmgr::decideRestart. Only the destructor
 * (Supervisor shutdown, after the event loop) waits, bounded by kShutdownReapMs in total.
 *
 * Orphan engines (PS-4b, design docs/plans/perf-supervisor-app.md §6.2). Every engine is spawned with this pid in
 * PLAYPLAY_SUPERVISOR_PID (the environment, not --supervisor-pid: an engine exe older than PS-3 would reject the option;
 * enginelock.h), so it watches this process and enters orphan mode when it dies. startReaper() runs reaper::scan on a worker thread
 * before the first spawn and every rescanMs after: an engine a previous Supervisor left running in this scope (same IPC
 * name, data dir under ours) is ADOPTED (same build, known enabled channel: a record without a QProcess whose process
 * handle is watched with QWinEventNotifier; it must send MSG_ENGINE_HELLO within helloDeadlineMs, then it gets
 * MSG_SUP_ALIVE for its channel and air continues) or REAPED (TerminateProcess, exit awaited through the handle, never
 * waited on the loop; a spawn of that channel waits for the exit like a restart does, with the takeover flag
 * (PLAYPLAY_ENGINE_TAKEOVER=1) when the kill wait runs out). Binding guard: an engine whose pid is not the channel's engine is refused (MSG_ENGINE_FATAL
 * "duplicate", never bound, alert) instead of rebinding the channel to it.
 */
class ChannelManager : public QObject {
    Q_OBJECT

public:
    struct ChannelRecord {
        QString channelId;
        QString channelName;
        int requestedPort = 0;
        int actualPort = 0;
        QProcess* process = nullptr; // the current engine; null while none runs (stopped, died, or a restart waits)
        QString dataDir;             // empty: an engine this manager did not spawn (bound over IPC only)
        QString configPath;          // JSON written for --config
        QString status;              // starting, running, degraded, restarting, stopped, error
        qint64 lastHeartbeatMs = 0;  // wall clock (epoch ms) of the last heartbeat or bind, for display
        qint64 pid = 0;
        int missedHeartbeats = 0;
        QString clientId;            // IPC client id once the engine connected
        qint64 lastFrame = 0;
        int restarts = 0;
        // PS-2 bookkeeping (monotonic ms of the manager's clock)
        qint64 lastBeatMono = 0;     // heartbeat timeout reference (a wall-clock step must not kill an engine)
        qint64 spawnedAtMono = -1;   // when the current engine was spawned
        bool spawnedByRestart = false;
        bool spawnPending = false;   // a spawn waits for a retiring engine of this channel to exit
        bool pendingIsRestart = false;
        bool refusedLogged = false;  // "not restarting an engine we did not spawn" logged once
        // PS-4b: the current engine was adopted from a previous Supervisor (no QProcess; its handle is watched instead)
        bool adopted = false;
        bool adoptConfirmed = false; // its HELLO arrived (status "adopting" until then)
        bool osAlive = false;        // the adopted process has not exited
        quint64 createTime = 0;      // FILETIME ticks of the adopted process (PID-reuse guard)
        QWinEventNotifier* osWatch = nullptr;   // owns the adopted process handle
    };

    // Orphan scan / adoption (PS-4b). Hooks keep the manager free of the store and the executor.
    struct ReaperOptions {
        bool enabled = true;                         // false: no scan, no adoption (engines.orphanScan)
        QString dataDir;                             // the Supervisor's data dir: the scan's scope with the IPC name
        std::function<bool(const QString& channelId, sched::ChannelProfile* out)> profileOf;   // false = unknown
        std::function<bool()> adoptEnabled;          // engines.adoptOrphans (default true); false = reap instead
        std::function<QJsonObject(const sched::ChannelProfile&)> engineConfig;   // channel.json for an adopted engine
        std::function<void()> startupDone;           // after the first scan's decisions (or its timeout): spawn now
        int rescanMs = 60000;
        int helloDeadlineMs = 15000;                 // an adopted engine must say HELLO within this
        int startupScanTimeoutMs = 10000;            // spawns are not held back longer than this by a slow scan
        int orphanGraceSec = -2;                     // >= -1: start-config default for spawned engines; -2 = leave it
    };
    static constexpr int kRescanDebounceMs = 500;

    static constexpr int kDefaultKillWaitMs = 5000;        // old code: waitForFinished(3000) + waitForFinished(2000)
    // The duplicates come from SupervisorCore 1.5 s after channelDied and from FailoverController's 500 ms tick; twice that
    // leaves slack for a busy loop. A longer window would also swallow a real request about the new engine (its fatal).
    static constexpr int kDefaultRestartCoalesceMs = 3000;
    static constexpr int kShutdownReapMs = 3000;           // destructor only

    explicit ChannelManager(QObject* parent = nullptr);
    ~ChannelManager() override;

    void setIpc(IpcTransport* ipc) { m_ipc = ipc; }
    void setEngineExecutable(const QString& path) { m_engineExe = path; }
    void setEngineExtraArgs(const QStringList& a) { m_extraArgs = a; }
    /// Extra environment variables for engines spawned from now on (PLAYPLAY_BROWSER etc.); layered over the Supervisor's own.
    void setEngineExtraEnv(const QProcessEnvironment& e) { m_extraEnv = e; }
    QString engineExecutable() const { return m_engineExe; }
    void setSupervisorIpcName(const QString& name) { m_ipcName = name; }

    /// Spawn an engine process. `config` is written to <dataDir>/channel.json and
    /// passed with --config so outputs/frame rate are live from the first frame.
    /// If an engine of this channel is still exiting (killed a moment ago), the spawn happens when it has gone and this
    /// returns true; channelSpawned reports the actual start.
    bool spawnChannel(const QString& channelId, const QString& channelName,
                      const QString& dataDir, const QJsonObject& config = {}, int port = 0);
    void killChannel(const QString& channelId);      // async: the exit is reported by engineExited
    void restartChannel(const QString& channelId);   // async and coalesced (chanmgr::decideRestart)
    void killAll();
    void removeChannel(const QString& channelId);

    QList<ChannelRecord> listChannels() const;
    ChannelRecord* channel(const QString& channelId);
    const ChannelRecord* channel(const QString& channelId) const;
    QStringList channelIds() const;
    bool isChannelRunning(const QString& channelId) const;
    bool isChannelConnected(const QString& channelId) const;
    qint64 lastHeartbeatMs(const QString& channelId) const;
    /// Killed engines whose exit has not been reported yet (all channels, or one).
    int retiringCount(const QString& channelId = QString()) const;

    bool startChannel(const QString& channelId);
    bool stopChannel(const QString& channelId);

    /// Route a message to the engine for channelId. Returns false if the engine
    /// has not connected yet (the caller may queue and retry on engineConnected).
    bool sendToChannel(const QString& channelId, const IpcProtocol::Message& msg);

    /// Applied to every `outputs` array on its way to an engine (spawn config and IPC messages): puts stream keys back into
    /// outputs stored with a `keyRef` (see livestream.h), so the keys never sit in channel profiles or REST replies.
    void setOutputsResolver(std::function<QJsonArray(const QJsonArray&)> fn) { m_outputsResolver = std::move(fn); }

    /// IPC bookkeeping (called by the Supervisor core from transport signals).
    void bindClient(const QString& channelId, const QString& clientId);
    void onClientDisconnected(const QString& clientId);
    QString channelForClient(const QString& clientId) const;

    void setHeartbeatTimeoutMs(int ms) { m_heartbeatTimeoutMs = ms; }
    int heartbeatTimeoutMs() const { return m_heartbeatTimeoutMs; }
    void setMaxMissedHeartbeats(int n) { m_maxMissedHeartbeats = n; }
    void setKillWaitMs(int ms) { m_killWaitMs = qMax(0, ms); }
    int killWaitMs() const { return m_killWaitMs; }
    void setRestartCoalesceMs(int ms) { m_restartCoalesceMs = qMax(0, ms); }
    int restartCoalesceMs() const { return m_restartCoalesceMs; }
    /// How a retiring engine is killed (default QProcess::kill, i.e. TerminateProcess). Tests use it to model an engine
    /// that does not exit at once; production never sets it.
    void setProcessKiller(std::function<void(QProcess*)> fn) { m_killFn = fn ? std::move(fn) : defaultKiller(); }

    // --- PS-4b: orphan engines -----------------------------------------------------------------------------------
    /// Starts the first scan (worker thread) and holds IPC binds of channels nobody owns yet until its decisions are
    /// applied; then calls startupDone and re-scans every rescanMs. Call once, before the first spawn.
    void startReaper(const ReaperOptions& o);
    void requestRescan(const QString& why);           // debounced; also used by an untracked engine's HELLO
    /// Takes over a running engine as this channel's engine (no QProcess). False when the channel already has a live
    /// engine of its own, a spawn waits, or the process cannot be opened / is not the one described (creation time).
    bool adoptChannel(const QString& channelId, const QString& channelName, const QString& dataDir,
                      const QJsonObject& config, const reaper::EngineFacts& f);
    /// Kills an engine this manager does not run (TerminateProcess; the exit is awaited through its handle). A spawn
    /// of f.channelId waits for that exit. False when the process is gone or is not the one described.
    bool reapEngine(const reaper::EngineFacts& f, const QString& why);
    /// MSG_ENGINE_HELLO from an IPC client: confirms an adoption, re-confirms our own engine after an orphan spell,
    /// refuses a duplicate, or asks for a re-scan for an engine nobody tracks.
    void onEngineHello(const QString& clientId, const QString& channelId, const QJsonObject& hello);
    bool isHolding() const { return m_holding; }
    /// A refused duplicate or a killed (retiring) engine whose link is still open: its state, telemetry and as-run are
    /// not the channel's and are dropped by SupervisorCore.
    bool isRejectedClient(const QString& clientId) const { return m_refusedClients.contains(clientId) || m_retiredClients.contains(clientId); }
    bool isRefusedClient(const QString& clientId) const { return m_refusedClients.contains(clientId); }
    QJsonObject reaperStats() const;

signals:
    void channelSpawned(const QString& channelId, int port);
    void channelDied(const QString& channelId, const QString& reason);
    void channelRespawned(const QString& channelId);
    void engineConnected(const QString& channelId);      // IPC bound: safe to push config/playlist
    void engineDisconnected(const QString& channelId);
    void heartbeatReceived(const QString& channelId, qint64 frameNumber, bool healthy);
    void heartbeatTimeout(const QString& channelId, int missedCount);
    /// An engine process ended (from QProcess::finished, never from a wait). expected = it was killed by us.
    void engineExited(const QString& channelId, qint64 pid, int exitCode, bool crashed, bool expected);
    /// Reaper / binding-guard events for the operator (adopted, reaped, duplicate refused). Same shape as pushAlert.
    void reaperAlert(const QString& channelId, const QString& level, const QString& message);
    void engineAdopted(const QString& channelId, qint64 pid);   // its HELLO confirmed the adoption
    void scanApplied(int found, int adopted, int reaped);       // after each scan's decisions (tests, logs)

public slots:
    void onHeartbeat(const QString& channelId, qint64 frameNumber, bool healthy);

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void onHealthCheckTimer();

private:
    struct Retiring {
        QString channelId;
        qint64 pid = 0;
        qint64 killedAtMono = 0;
        bool overdue = false;        // killWaitMs passed: no longer holds back the replacement
    };

    struct RetiringOs {              // a killed engine without a QProcess (adopted, or reaped as an orphan)
        QString channelId;
        qint64 pid = 0;
        qint64 killedAtMono = 0;
        bool overdue = false;
        QString why;
        QWinEventNotifier* watch = nullptr;   // owns the process handle
    };
    struct Hello { QString channelId; qint64 pid = 0; QString state; qint64 atMs = 0; bool orphan = false; };

    std::function<QJsonArray(const QJsonArray&)> m_outputsResolver;
    QMap<QString, ChannelRecord> m_channels;
    QHash<QString, QString> m_channelByClient;
    QHash<QProcess*, Retiring> m_retiring;
    QHash<qint64, RetiringOs> m_retiringOs;
    QSet<QString> m_retiredClients;  // IPC clients of killed engines, until their socket goes: never re-bound
    QSet<QString> m_refusedClients;  // duplicates refused by the binding guard, until their socket goes
    QHash<QString, QString> m_held;  // client -> channel: binds held while the startup scan runs
    QHash<QString, Hello> m_hellos; // client -> its last HELLO
    ReaperOptions m_reaper;
    bool m_reaperOn = false, m_holding = false, m_startupPending = false, m_scanRunning = false, m_rescanAgain = false;
    QTimer* m_rescanTimer = nullptr;
    QTimer* m_rescanDebounce = nullptr;
    QString m_rescanWhy;
    QSet<QString> m_notesSeen;
    quint64 m_supAliveSeq = 0;
    quint64 m_scans = 0, m_adoptedTotal = 0, m_reapedTotal = 0, m_refusedTotal = 0, m_confirmedTotal = 0;
    qint64 m_lastScanMs = -1;
    int m_orphanGraceSec = -2;
    IpcTransport* m_ipc = nullptr;
    QString m_engineExe;
    QStringList m_extraArgs;
    QProcessEnvironment m_extraEnv;
    QString m_ipcName = "PlayPlaySupervisor";
    int m_nextPort;
    int m_heartbeatTimeoutMs;
    int m_maxMissedHeartbeats;
    int m_killWaitMs = kDefaultKillWaitMs;
    int m_restartCoalesceMs = kDefaultRestartCoalesceMs;
    QTimer* m_healthCheckTimer;
    QElapsedTimer m_clock;
    std::function<void(QProcess*)> m_killFn;

    static std::function<void(QProcess*)> defaultKiller();
    int pickPort();
    QString channelIdForProcess(QProcess* proc) const;
    bool launch(const QString& channelId, bool byRestart);
    void retireCurrent(const QString& channelId);
    bool blockedByRetiring(const QString& channelId) const;
    void releasePendingSpawn(const QString& channelId);
    void onKillWaitExpired(QProcess* proc);
    void requestRestart(const QString& channelId, bool internal, const QString& why);
    // PS-4b helpers
    bool engineAlive(const ChannelRecord& r) const;
    bool trackedPid(qint64 pid) const;
    QString writeConfig(const QString& dataDir, const QJsonObject& config) const;
    QWinEventNotifier* watchProcess(qint64 pid, quint64 createTime, QString* error);
    void onOsProcessExited(qint64 pid);
    void onOsKillWaitExpired(qint64 pid);
    void terminateOs(qint64 pid);
    void runScan(bool startup);
    void applyScan(const reaper::ScanResult& r, bool startup);
    void finishStartup(const QString& why);
    void confirmAdoption(const QString& channelId, const QString& clientId);
    void onAdoptDeadline(const QString& channelId, qint64 pid);
    void refuseDuplicate(const QString& channelId, const QString& clientId, qint64 pid, qint64 ownerPid);
    void detachClientsOf(qint64 pid);
    void sendSupAlive(const QString& clientId, const QString& channelId);
    reaper::BuildFacts currentBuild();
};

#endif // CHANNELMANAGER_H
