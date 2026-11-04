#ifndef SUPERVISORLINK_H
#define SUPERVISORLINK_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>
#include "../core/enginelock.h"
#include "../ui_ipc/ipctransport.h"

class QWinEventNotifier;

/**
 * SupervisorLink — a channel engine's connection to its Supervisor (perf-supervisor PS-3 / W5, design
 * docs/plans/perf-supervisor-app.md §6.1). Replaces the one-shot blocking connect of engine_main.cpp, which never
 * reconnected (C1: when a Supervisor died its engines played on as orphans holding NDI names and UDP groups).
 *
 * Connection. Async (IpcTransport::connectToServerAsync: the pipe is opened on a worker, so a hung or absent Supervisor
 * never blocks the engine's main thread), re-tried every reconnectMs (1 s) after a failed attempt or a lost link, for
 * as long as the engine runs. isConnected() follows the socket. On every (re)connect the link sends MSG_ENGINE_HELLO
 * and then MSG_ENGINE_READY (unchanged payload: frameRate, width, height, pid), before connectedChanged(true), so HELLO
 * is always the first message on a connection. Supervisors that predate HELLO ignore it and (re)bind the engine on
 * READY / heartbeats as before. The link's transport has both caps (4 MB droppable, 64 MB disconnect): an engine that
 * reconnects can cut a link the Supervisor stopped reading instead of growing without bound.
 *
 * HELLO facts: {channelId, pid, createTime (FILETIME ticks, decimal string), createTimeMs, exe, exeMtime (ms),
 * configHash, supervisorIpc, supervisorPid, orphan, orphanForMs, connects, version, helloVersion: 1} plus what the
 * facts provider adds (engine_main: state, itemId, assetId, frame, outputs[names]).
 *
 * Parent watch. The Supervisor pid is --supervisor-pid when given (explicit parent; the PS-4b ChannelManager passes
 * it), else the pid at the other end of the pipe at the first connect (GetNamedPipeServerProcessId). Its process
 * handle is watched (QWinEventNotifier); when it exits the engine enters ORPHAN mode:
 *   - it keeps playing and keeps reconnecting every second; HELLO says orphan: true;
 *   - a Supervisor adopts it by sending MSG_SUP_ALIVE addressed to its channel (channelId set) on the link: orphan
 *     mode ends and that Supervisor's pid (payload "pid", else the pipe's) becomes the watched parent;
 *   - after orphanGraceSec without adoption it asks to exit (exitRequested(kExitOrphan)): engine_main stops its outputs
 *     and exits, which frees the NDI name, SDI port and UDP group. 0 = at once, -1 = never;
 *   - connected to a Supervisor that has not adopted it after adoptWaitSec, it also exits: that Supervisor predates
 *     adoption (or decided against it) and starts its own engine for the channel, which the duplicate guard keeps
 *     waiting while this one runs. 0 = never.
 * orphanGraceSec comes from the start config key "orphanGraceSec" (resolveOrphanGraceSec): default 300 with an
 * explicit parent, -1 when the parent was only learned from the pipe (an engine started by a Supervisor without PS-4b
 * keeps today's behaviour when no Supervisor comes back: it plays on). adoptWaitSec: key "orphanAdoptWaitSec",
 * default 20 (PS-4b's Supervisor waits 15 s for an adopted engine's HELLO).
 *
 * MSG_ENGINE_FATAL {reason: "duplicate"} for this channel from the Supervisor (PS-4b binding guard) asks to exit with
 * kExitDuplicate. engine.lock (enginelock.h) is written by start() and whenever the parent pid changes, and removed by
 * the destructor (only while it still names this process).
 *
 * Main-thread object; nothing here blocks (a tiny lock-file write at start / on a parent change).
 */
class SupervisorLink : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString ipcName;                // the Supervisor's IPC name (required)
        QString channelId;              // required
        QString dataDir;                // engine.lock goes here; empty = no lock file
        qint64 supervisorPid = 0;       // --supervisor-pid; 0 = learn it from the pipe at the first connect
        int orphanGraceSec = 300;       // see resolveOrphanGraceSec(); -1 = never, 0 = at once
        int adoptWaitSec = 20;          // see above; 0 = never
        int reconnectMs = 1000;
        int connectTimeoutMs = 2000;    // one attempt while every pipe instance is busy (the Supervisor is not accepting)
        QString configHash;             // of the start config (HELLO, engine.lock)
        QJsonObject readyPayload;       // MSG_ENGINE_READY payload (frameRate, width, height); "pid" is added
    };

    // Pure option rules (table-tested).
    static int resolveOrphanGraceSec(const QJsonObject& startConfig, bool explicitParent);
    static int resolveAdoptWaitSec(const QJsonObject& startConfig);
    // The duplicate guard at engine start (blocks up to waitMs; call before the event loop and before any output
    // opens). 0 = this process owns the channel; kExitDuplicate = another engine does (logged with its pid from
    // engine.lock). takeover (--takeover, passed by a Supervisor that killed the old engine): never refuses; if the old
    // engine has not exited within waitMs, the mutex is taken in the background when it does.
    static int claimChannel(enginelock::InstanceGuard& guard, const QString& channelId, const QString& dataDir,
                            bool takeover, int waitMs = 5000);

    explicit SupervisorLink(const Options& opt, QObject* parent = nullptr);
    ~SupervisorLink() override;    // removes engine.lock (if still ours) and closes the link

    // HELLO facts that change while the engine runs (state, itemId, frame, outputs); called on each (re)connect.
    void setFactsProvider(std::function<QJsonObject()> f) { m_facts = std::move(f); }
    void start();                  // writes engine.lock, arms the parent watch, starts connecting (returns at once)
    bool send(const IpcProtocol::Message& msg, IpcTransport::Priority p = IpcTransport::Auto);
    bool isConnected() const;
    bool isOrphan() const { return m_orphan; }
    qint64 supervisorPid() const { return m_parentPid; }     // the watched parent (0 = not known yet)
    qint64 connectedServerPid() const { return m_serverPid; }
    QJsonObject helloPayload() const;
    QJsonObject stats() const;     // attempts, connects, hellos, orphan, parent pid
    IpcTransport& transport() { return m_ipc; }
    const Options& options() const { return m_opt; }

signals:
    void connectedChanged(bool connected);
    void messageReceived(const IpcProtocol::Message& msg);
    void orphanChanged(bool orphan);
    void adopted(qint64 supervisorPid);
    void exitRequested(int code, const QString& reason);   // engine_main: QCoreApplication::exit(code)

private:
    void attempt();
    void scheduleRetry();
    void onStateChanged(bool up);
    void onConnectFailed(const QString& reason);
    void onMessage(const IpcProtocol::Message& msg);
    void sendHelloAndReady();
    void watchParent(qint64 pid);
    void unwatchParent();
    void onParentExited(qint64 pid);
    void enterOrphan(const QString& why);
    void adoptBy(qint64 pid);
    void requestExit(int code, const QString& reason);
    void writeLock();
    qint64 nowMs() const { return m_clock.isValid() ? m_clock.elapsed() : 0; }

    Options m_opt;
    IpcTransport m_ipc{IpcTransport::ClientRole};
    std::function<QJsonObject()> m_facts;
    enginelock::LockInfo m_self;
    QString m_lockPath;
    QTimer m_retry, m_grace, m_adoptWait;
    QElapsedTimer m_clock;              // monotonic, since start()
    bool m_started = false, m_up = false, m_orphan = false, m_exiting = false;
    bool m_explicitParent = false, m_lockWritten = false, m_foreignWarned = false;
    qint64 m_parentPid = 0;             // watched Supervisor pid (0 = not known)
    qint64 m_serverPid = 0;             // pid at the other end of the current connection
    QWinEventNotifier* m_parentNotifier = nullptr;   // owns (closes) the parent's process handle
    qint64 m_orphanSinceMs = -1, m_downSinceMs = -1, m_lastFailLogMs = -1;
    quint64 m_attempts = 0, m_connects = 0, m_hellos = 0, m_outageFails = 0, m_adoptions = 0;
};

#endif // SUPERVISORLINK_H
