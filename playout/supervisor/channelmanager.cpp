#include "channelmanager.h"
#include "../ui_ipc/ipctransport.h"
#include "../core/processlaunch.h"
#include "../core/enginelock.h"
#include "../schedule/schedulemodel.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDebug>
#include <QJsonDocument>
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <memory>
#ifdef Q_OS_WIN
#  include <QWinEventNotifier>
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace chanmgr {

RestartVerdict decideRestart(const RestartFacts& f, int coalesceMs) {
    if (!f.known || !f.managed) return RestartVerdict::Refuse;
    if (f.pending) return RestartVerdict::JoinPending;
    if (f.internal) return RestartVerdict::Proceed;
    // A dead engine is always replaced; only a live one that a restart started moments ago is a duplicate's victim.
    if (f.alive && f.sinceRestartSpawnMs >= 0 && f.sinceRestartSpawnMs < coalesceMs) return RestartVerdict::Coalesce;
    return RestartVerdict::Proceed;
}

const char* verdictName(RestartVerdict v) {
    switch (v) {
    case RestartVerdict::Proceed: return "proceed";
    case RestartVerdict::JoinPending: return "join";
    case RestartVerdict::Coalesce: return "coalesce";
    case RestartVerdict::Refuse: return "refuse";
    }
    return "?";
}

} // namespace chanmgr

ChannelManager::ChannelManager(QObject* parent)
    : QObject(parent)
    , m_nextPort(9001)
    , m_heartbeatTimeoutMs(1500)       // engines heartbeat at 10 Hz; allow jitter
    , m_maxMissedHeartbeats(3)
    , m_healthCheckTimer(new QTimer(this))
    , m_killFn(defaultKiller())
{
    m_clock.start();
    connect(m_healthCheckTimer, &QTimer::timeout, this, &ChannelManager::onHealthCheckTimer);
    m_healthCheckTimer->start(250);
    m_rescanTimer = new QTimer(this);
    connect(m_rescanTimer, &QTimer::timeout, this, [this]() { requestRescan(QStringLiteral("periodic")); });
    m_rescanDebounce = new QTimer(this);
    m_rescanDebounce->setSingleShot(true);
    connect(m_rescanDebounce, &QTimer::timeout, this, [this]() { runScan(false); });
}

ChannelManager::~ChannelManager() {
    m_healthCheckTimer->stop();
    m_rescanTimer->stop();
    m_rescanDebounce->stop();
    m_reaperOn = false;
    // Supervisor shutdown, after the event loop: every engine gets its kill now and nothing restarts. Wait for them
    // together and bounded, so the QProcess destructors below (which wait up to 30 s each for a running process) find
    // them gone; one that still runs after kShutdownReapMs is left to the OS (its TerminateProcess was issued).
    QList<QProcess*> procs = m_retiring.keys();
    for (const ChannelRecord& rec : std::as_const(m_channels)) if (rec.process) procs << rec.process;
    for (QProcess* p : std::as_const(procs)) {
        disconnect(p, nullptr, this, nullptr);
        if (p->state() != QProcess::NotRunning) p->kill();   // a real kill even if a test replaced m_killFn
    }
    QDeadlineTimer deadline(kShutdownReapMs);
#ifdef Q_OS_WIN
    // Adopted engines (still current) and killed ones without a QProcess: the same kill and the same shared budget. The
    // handles close with their notifiers (children of this object).
    QList<HANDLE> osHandles;
    for (ChannelRecord& rec : m_channels) {
        if (!rec.adopted || !rec.osWatch) continue;
        disconnect(rec.osWatch, nullptr, this, nullptr);
        if (rec.osAlive) { TerminateProcess(rec.osWatch->handle(), 1); osHandles << rec.osWatch->handle(); }
    }
    for (RetiringOs& r : m_retiringOs) {
        if (!r.watch) continue;
        disconnect(r.watch, nullptr, this, nullptr);
        osHandles << r.watch->handle();
    }
    for (HANDLE h : std::as_const(osHandles))
        WaitForSingleObject(h, DWORD(qMax<qint64>(0, deadline.remainingTime())));
#endif
    for (QProcess* p : std::as_const(procs)) {
        if (p->state() == QProcess::NotRunning) continue;
        if (!p->waitForFinished(int(qMax<qint64>(0, deadline.remainingTime())))) {
            qWarning() << "[ChannelManager] engine pid" << p->processId() << "still running at shutdown; leaving it to the OS";
            p->setParent(nullptr);   // deliberately leaked: its destructor would wait for it again
        }
    }
}

std::function<void(QProcess*)> ChannelManager::defaultKiller() {
    // terminate() posts WM_CLOSE, which a headless engine ignores: kill outright (TerminateProcess, returns at once).
    return [](QProcess* p) { p->kill(); };
}

int ChannelManager::pickPort() {
    if (m_nextPort > 9999) m_nextPort = 9001;
    return m_nextPort++;
}

QString ChannelManager::channelIdForProcess(QProcess* proc) const {
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        if (it.value().process == proc) return it.key();
    }
    return QString();
}

QString ChannelManager::writeConfig(const QString& dataDir, const QJsonObject& config) const {
    QDir().mkpath(dataDir);
    // Persist the engine config next to the channel data so a respawn gets it too.
    const QString configPath = QDir(dataDir).filePath("channel.json");
    if (!config.isEmpty()) {
        QFile f(configPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QJsonObject out = config;
            if (m_outputsResolver && out.contains("outputs")) out["outputs"] = m_outputsResolver(out.value("outputs").toArray());
            // PS-4b: what an engine does when its Supervisor dies and nobody adopts it (engine default 300 s with
            // --supervisor-pid); the Supervisor's setting wins over the engine default, a profile's own key over both.
            if (m_orphanGraceSec >= -1 && !out.contains("orphanGraceSec")) out["orphanGraceSec"] = m_orphanGraceSec;
            f.write(QJsonDocument(out).toJson(QJsonDocument::Indented));
        }
    }
    return configPath;
}

bool ChannelManager::spawnChannel(const QString& channelId, const QString& channelName,
                                  const QString& dataDir, const QJsonObject& config, int port) {
    const auto existing = m_channels.constFind(channelId);
    const bool wasPending = existing != m_channels.constEnd() && existing->spawnPending;
    if (existing != m_channels.constEnd() && engineAlive(existing.value())) {
        qDebug() << "[ChannelManager] Channel already running:" << channelId;
        return false;
    }

    if (m_engineExe.isEmpty()) {
        m_engineExe = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayChannelEngine.exe");
    }
    const QString configPath = writeConfig(dataDir, config);

    const int usePort = (port > 0) ? port : (wasPending && existing->requestedPort > 0 ? existing->requestedPort : pickPort());

    ChannelRecord& rec = m_channels[channelId];
    rec.channelId = channelId;
    rec.channelName = channelName;
    rec.requestedPort = usePort;
    rec.actualPort = usePort;
    rec.dataDir = dataDir;
    rec.configPath = configPath;
    rec.missedHeartbeats = 0;
    rec.clientId.clear();
    rec.refusedLogged = false;
    rec.adopted = false;          // an adopted engine of this channel is gone by now (engineAlive above)
    rec.adoptConfirmed = false;
    if (!wasPending) rec.pendingIsRestart = false;

    // An engine of this channel killed a moment ago (config change: removeChannel + spawnChannel; stop + start) is still
    // exiting: start the new one when it has gone, not next to it (it still owns the NDI name / ports).
    if (blockedByRetiring(channelId)) {
        rec.status = rec.pendingIsRestart ? "restarting" : "starting";
        rec.spawnPending = true;
        qInfo() << "[ChannelManager] spawn of" << channelId << "waits for its previous engine to exit";
        return true;
    }
    return launch(channelId, wasPending && rec.pendingIsRestart);
}

bool ChannelManager::launch(const QString& channelId, bool byRestart) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return false;
    if (m_engineExe.isEmpty()) m_engineExe = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayChannelEngine.exe");
    ChannelRecord& rec = it.value();
    rec.spawnPending = false;
    rec.pendingIsRestart = false;
    if (rec.process) { rec.process->deleteLater(); rec.process = nullptr; }   // only a dead one (failed to start) gets here

    QProcess* proc = new QProcess(this);
    processlaunch::configureDetachedChild(proc);   // own hidden console, NUL stdio (the engine logs to --log-dir)
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ChannelManager::onProcessFinished);
    connect(proc, &QProcess::errorOccurred, this, &ChannelManager::onProcessError);

    QStringList args;
    args << "--id" << channelId
         << "--name" << rec.channelName
         << "--port" << QString::number(rec.requestedPort)
         << "--data" << rec.dataDir
         << "--supervisor" << m_ipcName;
    if (QFile::exists(rec.configPath)) args << "--config" << rec.configPath;
    args << m_extraArgs;

    // PS-4b: our pid (the engine watches this process and enters orphan mode when it dies; a successor adopts it) and,
    // while an engine of this channel that we killed has not exited yet (only after killWaitMs), the takeover flag (the
    // new one starts anyway and takes the one-engine-per-channel guard when the old one goes, instead of exiting 4).
    // Both go in the environment, not on the command line: an engine exe older than PS-3 rejects unknown options and
    // exits 1, so a Supervisor rebuilt on its own would put every channel into a spawn-fail loop (enginelock.h).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_extraEnv.isEmpty()) env.insert(m_extraEnv);
    env.insert(QString::fromLatin1(enginelock::kEnvSupervisorPid), QString::number(QCoreApplication::applicationPid()));
    if (retiringCount(channelId) > 0) env.insert(QString::fromLatin1(enginelock::kEnvTakeover), QStringLiteral("1"));
    else env.remove(QString::fromLatin1(enginelock::kEnvTakeover));
    proc->setProcessEnvironment(env);
    proc->setProgram(m_engineExe);
    proc->setArguments(args);

    rec.process = proc;
    rec.pid = 0;
    rec.status = "starting";
    rec.lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    rec.lastBeatMono = m_clock.elapsed();
    rec.missedHeartbeats = 0;
    rec.clientId.clear();
    rec.spawnedAtMono = m_clock.elapsed();
    rec.spawnedByRestart = byRestart;
    const int port = rec.requestedPort;

    proc->start();
    // start() reports FailedToStart synchronously (onProcessError, channelDied): look the record up again.
    const auto again = m_channels.find(channelId);
    const bool mine = again != m_channels.end() && again->process == proc;
    if (proc->state() == QProcess::Running || proc->state() == QProcess::Starting) {
        if (mine) again->pid = proc->processId();
        qInfo() << "[ChannelManager] spawned" << channelId << "pid" << proc->processId() << (byRestart ? "(restart)" : "");
        emit channelSpawned(channelId, port);
        if (byRestart) emit channelRespawned(channelId);
        return true;
    }
    if (mine) again->status = "error";
    qWarning() << "[ChannelManager] failed to spawn" << channelId << proc->errorString();
    return false;
}

// Takes the current engine off the channel: its IPC link stops counting for the channel, and a running process gets
// its kill and is tracked as retiring until QProcess::finished reports the exit. Never waits.
void ChannelManager::retireCurrent(const QString& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    QProcess* proc = it->process;
    it->process = nullptr;
    const QString clientId = it->clientId;
    if (it->adopted) {
        // An adopted engine has no QProcess: kill it through its handle and track it until the handle reports the exit.
        QWinEventNotifier* w = it->osWatch;
        const bool alive = it->osAlive;
        it->adopted = false; it->adoptConfirmed = false; it->osAlive = false; it->osWatch = nullptr;
        if (w && alive) {
            RetiringOs r;
            r.channelId = channelId; r.pid = it->pid; r.killedAtMono = m_clock.elapsed(); r.why = QStringLiteral("adopted engine retired"); r.watch = w;
            m_retiringOs.insert(r.pid, r);
            terminateOs(r.pid);
        } else if (w) {
            w->deleteLater();
        }
    }
    if (!clientId.isEmpty()) {
        m_channelByClient.remove(clientId);
        // Still connected (onClientDisconnected clears clientId): whatever it sends until its link closes must not bind
        // the channel again. Client ids are socket addresses, so only ids of live sockets go in (the disconnect removes them).
        if (!m_ipc || m_ipc->clientIds().contains(clientId)) m_retiredClients.insert(clientId);
        it->clientId.clear();
    }
    if (proc) {
        if (proc->state() != QProcess::NotRunning) {
            Retiring r;
            r.channelId = channelId;
            r.pid = proc->processId();
            r.killedAtMono = m_clock.elapsed();
            m_retiring.insert(proc, r);
            qInfo() << "[ChannelManager] killing engine" << channelId << "pid" << r.pid;
            m_killFn(proc);
            if (m_retiring.contains(proc))
                QTimer::singleShot(m_killWaitMs, this, [this, p = QPointer<QProcess>(proc)]() { if (p) onKillWaitExpired(p.data()); });
        } else {
            proc->deleteLater();
        }
    }
    if (!clientId.isEmpty()) emit engineDisconnected(channelId);
}

bool ChannelManager::blockedByRetiring(const QString& channelId) const {
    for (auto it = m_retiring.constBegin(); it != m_retiring.constEnd(); ++it)
        if (it->channelId == channelId && !it->overdue) return true;
    for (auto it = m_retiringOs.constBegin(); it != m_retiringOs.constEnd(); ++it)
        if (it->channelId == channelId && !it->overdue) return true;
    return false;
}

int ChannelManager::retiringCount(const QString& channelId) const {
    if (channelId.isEmpty()) return int(m_retiring.size() + m_retiringOs.size());
    int n = 0;
    for (auto it = m_retiring.constBegin(); it != m_retiring.constEnd(); ++it) if (it->channelId == channelId) ++n;
    for (auto it = m_retiringOs.constBegin(); it != m_retiringOs.constEnd(); ++it) if (it->channelId == channelId) ++n;
    return n;
}

void ChannelManager::releasePendingSpawn(const QString& channelId) {
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd() || !it->spawnPending || blockedByRetiring(channelId)) return;
    launch(channelId, it->pendingIsRestart);
}

void ChannelManager::onKillWaitExpired(QProcess* proc) {
    auto it = m_retiring.find(proc);
    if (it == m_retiring.end() || it->overdue) return;
    it->overdue = true;
    const QString id = it->channelId;
    const auto rec = m_channels.constFind(id);
    const bool replacement = rec != m_channels.constEnd() && rec->spawnPending;
    qWarning() << "[ChannelManager] engine" << id << "pid" << it->pid << "has not exited" << m_killWaitMs << "ms after its kill;"
               << (replacement ? "starting its replacement anyway" : "still waiting for it") << "(the old process stays tracked)";
    releasePendingSpawn(id);
}

void ChannelManager::killChannel(const QString& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    it->status = "stopped";
    if (it->spawnPending) qInfo() << "[ChannelManager] pending spawn of" << channelId << "cancelled";
    it->spawnPending = false;
    it->pendingIsRestart = false;
    retireCurrent(channelId);
}

void ChannelManager::restartChannel(const QString& channelId) {
    requestRestart(channelId, false, "request");
}

// The one restart path (C6). Requests for the same death join or coalesce (chanmgr::decideRestart); a restart kills the
// current engine and spawns the new one when the old has exited (or after killWaitMs).
void ChannelManager::requestRestart(const QString& channelId, bool internal, const QString& why) {
    auto it = m_channels.find(channelId);
    chanmgr::RestartFacts f;
    f.known = it != m_channels.end();
    f.internal = internal;
    if (f.known) {
        const ChannelRecord& rec = it.value();
        f.managed = !rec.dataDir.isEmpty();
        f.pending = rec.spawnPending;
        f.alive = engineAlive(rec);
        f.sinceRestartSpawnMs = (rec.spawnedByRestart && rec.spawnedAtMono >= 0) ? m_clock.elapsed() - rec.spawnedAtMono : -1;
    }
    switch (chanmgr::decideRestart(f, m_restartCoalesceMs)) {
    case chanmgr::RestartVerdict::Refuse:
        if (f.known) {
            if (internal) it->status = "error";   // keeps the heartbeat watchdog from declaring it dead again every window
            if (!it->refusedLogged) {
                it->refusedLogged = true;
                qWarning().noquote() << "[ChannelManager] not restarting" << channelId << "(" + why + "): its engine was not spawned by this Supervisor";
            }
        }
        return;
    case chanmgr::RestartVerdict::JoinPending:
        qInfo().noquote() << "[ChannelManager] restart of" << channelId << "(" + why + ") joins the restart waiting for the old engine to exit";
        return;
    case chanmgr::RestartVerdict::Coalesce:
        qInfo().noquote() << "[ChannelManager] restart of" << channelId << "(" + why + ") ignored: engine pid" << it->pid
                          << "was started by a restart" << f.sinceRestartSpawnMs << "ms ago";
        return;
    case chanmgr::RestartVerdict::Proceed:
        break;
    }
    it->restarts++;
    it->status = "restarting";
    it->spawnPending = true;       // set before retiring: a request from an engineDisconnected listener joins this one
    it->pendingIsRestart = true;
    qInfo().noquote() << "[ChannelManager] restarting" << channelId << "(" + why + ", restart #" + QString::number(it->restarts) + ")";
    retireCurrent(channelId);
    releasePendingSpawn(channelId);   // now, unless an engine of this channel is still exiting
}

void ChannelManager::killAll() {
    for (const QString& id : m_channels.keys()) killChannel(id);
}

void ChannelManager::removeChannel(const QString& channelId) {
    killChannel(channelId);   // the process stays tracked as retiring until it has exited
    m_channels.remove(channelId);
}

QList<ChannelManager::ChannelRecord> ChannelManager::listChannels() const { return m_channels.values(); }
ChannelManager::ChannelRecord* ChannelManager::channel(const QString& channelId) {
    auto it = m_channels.find(channelId);
    return it == m_channels.end() ? nullptr : &it.value();
}
const ChannelManager::ChannelRecord* ChannelManager::channel(const QString& channelId) const {
    auto it = m_channels.find(channelId);
    return it == m_channels.end() ? nullptr : &it.value();
}
QStringList ChannelManager::channelIds() const { return m_channels.keys(); }

bool ChannelManager::isChannelRunning(const QString& channelId) const {
    const ChannelRecord* r = channel(channelId);
    return r && ((r->process && r->process->state() == QProcess::Running) || (r->adopted && r->osAlive));
}

bool ChannelManager::engineAlive(const ChannelRecord& r) const {
    return (r.process && r.process->state() != QProcess::NotRunning) || (r.adopted && r.osAlive);
}

bool ChannelManager::isChannelConnected(const QString& channelId) const {
    const ChannelRecord* r = channel(channelId);
    return r && !r->clientId.isEmpty();
}

qint64 ChannelManager::lastHeartbeatMs(const QString& channelId) const {
    const ChannelRecord* r = channel(channelId);
    return r ? r->lastHeartbeatMs : 0;
}

bool ChannelManager::startChannel(const QString& channelId) {
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd()) return false;
    if (engineAlive(it.value())) return true;
    if (it->spawnPending) return true;
    const ChannelRecord rec = it.value();
    return spawnChannel(rec.channelId, rec.channelName, rec.dataDir, {}, rec.requestedPort);
}

bool ChannelManager::stopChannel(const QString& channelId) {
    killChannel(channelId);
    return true;
}

bool ChannelManager::sendToChannel(const QString& channelId, const IpcProtocol::Message& msg) {
    const ChannelRecord* r = channel(channelId);
    if (!r || r->clientId.isEmpty() || !m_ipc) return false;
    IpcProtocol::Message m = msg;
    if (m.channelId.isEmpty()) m.channelId = channelId;
    if (m_outputsResolver && m.payload.contains("outputs")) m.payload["outputs"] = m_outputsResolver(m.payload.value("outputs").toArray());
    m_ipc->sendTo(r->clientId, m);
    return true;
}

void ChannelManager::bindClient(const QString& channelId, const QString& clientId) {
    if (channelId.isEmpty() || clientId.isEmpty()) return;
    // A killed or dead engine whose link has not closed yet (heartbeats or READY still in flight) never takes the
    // channel back from its replacement, nor marks a stopped channel running.
    if (m_retiredClients.contains(clientId) || m_refusedClients.contains(clientId)) return;
    // Startup scan running: an engine for a channel nobody owns yet may be one the scan adopts or reaps. Hold the bind
    // until its decisions are applied (finishStartup binds what is left, as before).
    if (m_holding && !m_channels.contains(channelId)) { m_held.insert(clientId, channelId); return; }
    const qint64 peer = m_ipc ? m_ipc->peerProcessId(clientId) : 0;
    if (const auto ex = m_channels.constFind(channelId); ex != m_channels.constEnd() && ex->clientId != clientId) {
        // Binding guard (PS-4b): the channel's engine is the process we spawned or adopted (pid), else the client bound
        // to it; another process is a duplicate (two NDI senders of one name, two streams on one group). Refused, never
        // a flip-flop. Unknown pids (0) fail open, as before.
        // An adopted engine is bound only through its HELLO (confirmAdoption). A client whose pid is unknown (0) is
        // not bound fail-open here either: bound unconfirmed, the HELLO deadline would reap an engine that works.
        if (ex->adopted && !ex->adoptConfirmed && (peer <= 0 || peer == ex->pid)) {
            if (m_hellos.value(clientId).pid == ex->pid) confirmAdoption(channelId, clientId);
            return;   // READY before its HELLO cannot happen on one connection: wait for the HELLO
        }
        qint64 owner = (ex->pid > 0 && engineAlive(ex.value())) ? ex->pid : 0;
        if (!owner && !ex->clientId.isEmpty() && m_ipc && m_ipc->clientIds().contains(ex->clientId)) owner = m_ipc->peerProcessId(ex->clientId);
        if (peer > 0 && owner > 0 && peer != owner) { refuseDuplicate(channelId, clientId, peer, owner); return; }
    }
    if (!m_channels.contains(channelId)) {
        // An engine we did not spawn (standalone bench run attached to us): adopt it.
        ChannelRecord rec;
        rec.channelId = channelId;
        rec.channelName = channelId;
        rec.status = "running";
        m_channels[channelId] = rec;
    }
    ChannelRecord& rec = m_channels[channelId];
    const bool fresh = (rec.clientId != clientId);
    if (fresh && !rec.clientId.isEmpty()) m_channelByClient.remove(rec.clientId);
    rec.clientId = clientId;
    rec.lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    rec.lastBeatMono = m_clock.elapsed();
    rec.missedHeartbeats = 0;
    if (rec.status == "starting" || rec.status == "stopped") rec.status = "running";
    m_channelByClient[clientId] = channelId;
    if (fresh) {
        // Engine links skip time sync while behind (soft cap) but are never cut off for it (no hard cap): a frozen
        // engine stops heartbeating and onHealthCheckTimer restarts it, which also frees its queue. A cap-disconnect
        // would clear clientId and take the channel out of that watchdog until engines reconnect (PS-3).
        if (m_ipc) { IpcTransport::Caps c = m_ipc->caps(); c.hardBytes = 0; m_ipc->setPeerCaps(clientId, c); }
        qInfo() << "[ChannelManager] engine connected:" << channelId << "client" << clientId;
        emit engineConnected(channelId);
    }
}

void ChannelManager::onClientDisconnected(const QString& clientId) {
    m_retiredClients.remove(clientId);
    m_refusedClients.remove(clientId);
    m_held.remove(clientId);
    m_hellos.remove(clientId);
    const QString channelId = m_channelByClient.take(clientId);
    if (channelId.isEmpty()) return;
    if (m_channels.contains(channelId) && m_channels[channelId].clientId == clientId) {
        m_channels[channelId].clientId.clear();
        qWarning() << "[ChannelManager] engine disconnected:" << channelId;
        emit engineDisconnected(channelId);
    }
}

QString ChannelManager::channelForClient(const QString& clientId) const {
    return m_channelByClient.value(clientId);
}

void ChannelManager::onHeartbeat(const QString& channelId, qint64 frameNumber, bool healthy) {
    const auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    ChannelRecord& rec = it.value();
    // No engine bound: the heartbeat comes from a killed or dead engine (bindClient refused it), not from this channel's.
    if (rec.clientId.isEmpty()) return;
    rec.lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    rec.lastBeatMono = m_clock.elapsed();
    rec.missedHeartbeats = 0;
    rec.lastFrame = frameNumber;
    rec.status = healthy ? "running" : "degraded";
    emit heartbeatReceived(channelId, frameNumber, healthy);
}

void ChannelManager::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    QProcess* proc = qobject_cast<QProcess*>(sender());
    if (!proc) return;
    const bool crashed = status == QProcess::CrashExit;

    // An engine we killed: report it, then start the replacement that waited for it.
    const auto rit = m_retiring.find(proc);
    if (rit != m_retiring.end()) {
        const Retiring r = rit.value();
        m_retiring.erase(rit);
        proc->deleteLater();
        qInfo() << "[ChannelManager] engine" << r.channelId << "pid" << r.pid << "exited" << (m_clock.elapsed() - r.killedAtMono)
                << "ms after its kill" << (r.overdue ? "(late)" : "");
        emit engineExited(r.channelId, r.pid, exitCode, crashed, true);
        releasePendingSpawn(r.channelId);
        return;
    }

    const QString id = channelIdForProcess(proc);
    if (id.isEmpty()) { proc->deleteLater(); return; }
    ChannelRecord& rec = m_channels[id];
    const bool expected = (rec.status == "stopped");
    const qint64 pid = rec.pid;
    const QString clientId = rec.clientId;
    rec.process = nullptr;
    if (!clientId.isEmpty()) {
        m_channelByClient.remove(clientId);
        if (!m_ipc || m_ipc->clientIds().contains(clientId)) m_retiredClients.insert(clientId);   // its link closes next
        rec.clientId.clear();
    }
    if (!expected) rec.status = "error";
    proc->deleteLater();
    if (!clientId.isEmpty()) emit engineDisconnected(id);
    emit engineExited(id, pid, exitCode, crashed, expected);
    if (!expected) {
        const QString reason = QString("exit_%1_%2").arg(crashed ? "crash" : "code").arg(exitCode);
        qCritical() << "[ChannelManager] engine" << id << "ended unexpectedly:" << reason;
        emit channelDied(id, reason);
    }
}

void ChannelManager::onProcessError(QProcess::ProcessError error) {
    QProcess* proc = qobject_cast<QProcess*>(sender());
    const QString id = channelIdForProcess(proc);
    if (id.isEmpty()) return;
    if (error == QProcess::FailedToStart) {
        m_channels[id].status = "error";
        qCritical() << "[ChannelManager] engine failed to start for" << id << ":" << proc->errorString();
        emit channelDied(id, "failed_to_start");
    }
}

void ChannelManager::onHealthCheckTimer() {
    const qint64 now = m_clock.elapsed();   // monotonic: a wall-clock step (NTP) must not look like missed heartbeats
    QList<QPair<QString, int>> misses;
    QStringList dead;
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        ChannelRecord& rec = it.value();
        if (rec.status == "stopped" || rec.status == "error" || rec.status == "starting" || rec.status == "restarting" || rec.status == "adopting") continue;
        if (rec.clientId.isEmpty()) continue;    // not connected yet: nothing to time out
        if (now - rec.lastBeatMono > m_heartbeatTimeoutMs) {
            rec.missedHeartbeats++;
            rec.lastBeatMono = now;          // one miss per timeout window
            misses.append({it.key(), rec.missedHeartbeats});
            if (rec.missedHeartbeats >= m_maxMissedHeartbeats) dead << it.key();
        }
    }
    // Signals after the walk: a listener may add or remove channels.
    for (const auto& m : std::as_const(misses)) {
        emit heartbeatTimeout(m.first, m.second);
        qWarning() << "[ChannelManager]" << m.first << "missed heartbeat #" << m.second;
    }
    for (const QString& id : std::as_const(dead)) {
        qCritical() << "[ChannelManager]" << id << "declared dead";
        emit channelDied(id, "heartbeat_timeout");
        // Our own restart; SupervisorCore's (1.5 s after channelDied) and FailoverController's requests for this death
        // join or coalesce with it instead of killing the engine it starts (C6).
        requestRestart(id, true, "heartbeat_timeout");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// PS-4b: orphan engines (adopt / reap), the binding guard, processes without a QProcess
// ---------------------------------------------------------------------------------------------------------------------

bool ChannelManager::trackedPid(qint64 pid) const {
    if (pid <= 0) return false;
    if (m_retiringOs.contains(pid)) return true;
    for (const Retiring& r : m_retiring) if (r.pid == pid) return true;
    for (const ChannelRecord& rec : m_channels) if (rec.pid == pid && engineAlive(rec)) return true;
    return false;
}

reaper::BuildFacts ChannelManager::currentBuild() {
    if (m_engineExe.isEmpty()) m_engineExe = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayChannelEngine.exe");
    const QFileInfo fi(m_engineExe);
    reaper::BuildFacts b;
    b.exe = QDir::toNativeSeparators(fi.absoluteFilePath());
    b.exeMtimeMs = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;   // what enginelock records at engine start
    return b;
}

// Opens pid for waiting and killing, and proves it is the process described (its creation time: a pid can be reused).
// The notifier owns the handle; its activation (the process exited) goes to onOsProcessExited.
QWinEventNotifier* ChannelManager::watchProcess(qint64 pid, quint64 createTime, QString* error) {
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) { *error = QString("cannot open pid %1 (Windows error %2)").arg(pid).arg(GetLastError()); return nullptr; }
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { CloseHandle(h); *error = QString("pid %1 has exited").arg(pid); return nullptr; }
    if (createTime) {
        FILETIME c{}, e{}, k{}, u{};
        const quint64 ct = GetProcessTimes(h, &c, &e, &k, &u) ? ((quint64(c.dwHighDateTime) << 32) | c.dwLowDateTime) : 0;
        if (ct != createTime) { CloseHandle(h); *error = QString("pid %1 is another process now (creation time differs)").arg(pid); return nullptr; }
    }
    auto* n = new QWinEventNotifier(h, this);
    QObject::connect(n, &QObject::destroyed, [h]() { CloseHandle(h); });
    connect(n, &QWinEventNotifier::activated, this, [this, pid]() { onOsProcessExited(pid); });
    n->setEnabled(true);
    return n;
#else
    Q_UNUSED(pid); Q_UNUSED(createTime);
    *error = QStringLiteral("not supported on this platform");
    return nullptr;
#endif
}

void ChannelManager::terminateOs(qint64 pid) {
    auto it = m_retiringOs.find(pid);
    if (it == m_retiringOs.end() || !it->watch) return;
    qInfo().noquote() << QString("[ChannelManager] killing engine %1 pid %2 (%3)").arg(it->channelId).arg(pid).arg(it->why);
#ifdef Q_OS_WIN
    HANDLE h = it->watch->handle();
    if (!TerminateProcess(h, 1) && WaitForSingleObject(h, 0) != WAIT_OBJECT_0)
        qWarning().noquote() << QString("[ChannelManager] TerminateProcess(pid %1) failed (Windows error %2)").arg(pid).arg(GetLastError());
#endif
    const qint64 killedAt = it->killedAtMono;
    QTimer::singleShot(m_killWaitMs, this, [this, pid, killedAt]() {
        const auto r = m_retiringOs.constFind(pid);
        if (r != m_retiringOs.constEnd() && r->killedAtMono == killedAt) onOsKillWaitExpired(pid);
    });
}

void ChannelManager::onOsKillWaitExpired(qint64 pid) {
    auto it = m_retiringOs.find(pid);
    if (it == m_retiringOs.end() || it->overdue) return;
    it->overdue = true;
    const QString id = it->channelId;
    qWarning().noquote() << QString("[ChannelManager] engine %1 pid %2 has not exited %3 ms after its kill; a waiting spawn starts anyway (takeover)")
                                .arg(id).arg(pid).arg(m_killWaitMs);
    releasePendingSpawn(id);
}

void ChannelManager::onOsProcessExited(qint64 pid) {
    int code = 0;
    auto exitCodeOf = [&code](QWinEventNotifier* w) {
        if (!w) return;
        w->setEnabled(false);   // a process handle stays signalled: report it once
#ifdef Q_OS_WIN
        DWORD c = 0;
        if (GetExitCodeProcess(w->handle(), &c)) code = int(c);
#endif
        w->deleteLater();
    };
    const auto rit = m_retiringOs.find(pid);
    if (rit != m_retiringOs.end()) {
        const RetiringOs r = rit.value();
        m_retiringOs.erase(rit);
        exitCodeOf(r.watch);
        qInfo().noquote() << QString("[ChannelManager] engine %1 pid %2 exited %3 ms after its kill%4")
                                 .arg(r.channelId).arg(pid).arg(m_clock.elapsed() - r.killedAtMono).arg(r.overdue ? " (late)" : "");
        emit engineExited(r.channelId, pid, code, false, true);
        releasePendingSpawn(r.channelId);
        return;
    }
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) {
        if (!it->adopted || it->pid != pid || !it->osWatch) continue;
        // The adopted engine ended on its own: what onProcessFinished does for a spawned one.
        const QString id = it.key();
        ChannelRecord& rec = it.value();
        exitCodeOf(rec.osWatch);
        rec.osWatch = nullptr; rec.osAlive = false; rec.adopted = false; rec.adoptConfirmed = false;
        const QString clientId = rec.clientId;
        if (!clientId.isEmpty()) {
            m_channelByClient.remove(clientId);
            if (!m_ipc || m_ipc->clientIds().contains(clientId)) m_retiredClients.insert(clientId);
            rec.clientId.clear();
        }
        const bool expected = rec.status == "stopped";
        if (!expected) rec.status = "error";
        const bool crashed = quint32(code) >= 0xC0000000u;   // NTSTATUS error codes: an access violation, an abort ...
        if (!clientId.isEmpty()) emit engineDisconnected(id);
        emit engineExited(id, pid, code, crashed, expected);
        if (!expected) {
            const QString reason = QString("exit_%1_%2").arg(crashed ? "crash" : "code").arg(code);
            qCritical().noquote() << "[ChannelManager] adopted engine" << id << "pid" << pid << "ended unexpectedly:" << reason;
            emit channelDied(id, reason);
        }
        return;
    }
}

void ChannelManager::sendSupAlive(const QString& clientId, const QString& channelId) {
    // Addressed to the channel: the engine leaves orphan mode and watches this process from now on (SupervisorLink).
    // Normal priority: it must not be skipped like the 1 Hz liveness copy.
    if (m_ipc) m_ipc->sendTo(clientId, IpcProtocol::makeSupAlive(qint64(++m_supAliveSeq), 0.0, QCoreApplication::applicationPid(), channelId), IpcTransport::Normal);
}

void ChannelManager::refuseDuplicate(const QString& channelId, const QString& clientId, qint64 pid, qint64 ownerPid) {
    if (m_refusedClients.contains(clientId)) return;
    m_refusedClients.insert(clientId);
    m_held.remove(clientId);
    ++m_refusedTotal;
    const QString text = QString("refused duplicate engine pid %1 for channel %2 (its engine is pid %3)").arg(pid).arg(channelId).arg(ownerPid);
    qWarning().noquote() << "[ChannelManager]" << text;
    if (m_ipc) {
        IpcProtocol::Message f = IpcProtocol::make(IpcProtocol::MSG_ENGINE_FATAL, channelId, QJsonObject{
            {"reason", "duplicate"}, {"pid", double(pid)}, {"ownerPid", double(ownerPid)},
            {"message", QString("channel %1 is owned by engine pid %2").arg(channelId).arg(ownerPid)}});
        m_ipc->sendTo(clientId, f, IpcTransport::Normal);
    }
    emit reaperAlert(channelId, "warning", text);
}

// Every IPC client of process pid stops counting for any channel (it is being killed).
void ChannelManager::detachClientsOf(qint64 pid) {
    if (!m_ipc || pid <= 0) return;
    const QStringList clients = m_ipc->clientIds();
    for (const QString& cid : clients) {
        if (m_ipc->peerProcessId(cid) != pid) continue;
        m_held.remove(cid);
        m_retiredClients.insert(cid);
        const QString ch = m_channelByClient.take(cid);
        auto it = m_channels.find(ch);
        if (ch.isEmpty() || it == m_channels.end() || it->clientId != cid) continue;
        it->clientId.clear();
        emit engineDisconnected(ch);
        it = m_channels.find(ch);   // a listener may have changed the map
        // A record bindClient made for this engine (no data dir, nothing of ours runs): it goes with the engine.
        if (it != m_channels.end() && it->dataDir.isEmpty() && !engineAlive(it.value()) && it->clientId.isEmpty()) m_channels.erase(it);
    }
}

bool ChannelManager::reapEngine(const reaper::EngineFacts& f, const QString& why) {
    if (f.pid <= 0 || f.pid == QCoreApplication::applicationPid()) return false;
    if (m_retiringOs.contains(f.pid)) return true;
    for (const Retiring& r : m_retiring) if (r.pid == f.pid) return true;
    for (const ChannelRecord& rec : m_channels)
        if (rec.pid == f.pid && engineAlive(rec)) { qWarning() << "[ChannelManager] not reaping pid" << f.pid << ": it is channel" << rec.channelId << "'s engine"; return false; }
    QString err;
    QWinEventNotifier* w = watchProcess(f.pid, f.createTime, &err);
    if (!w) { qWarning().noquote() << QString("[ChannelManager] not reaping engine %1 pid %2: %3").arg(f.channelId).arg(f.pid).arg(err); return false; }
    detachClientsOf(f.pid);
    RetiringOs r;
    r.channelId = f.channelId; r.pid = f.pid; r.killedAtMono = m_clock.elapsed(); r.why = why; r.watch = w;
    m_retiringOs.insert(f.pid, r);
    ++m_reapedTotal;
    terminateOs(f.pid);
    return true;
}

bool ChannelManager::adoptChannel(const QString& channelId, const QString& channelName, const QString& dataDir,
                                  const QJsonObject& config, const reaper::EngineFacts& f) {
    if (channelId.isEmpty() || f.pid <= 0) return false;
    auto ex = m_channels.find(channelId);
    if (ex != m_channels.end()) {
        if (ex->adopted && ex->pid == f.pid && ex->osAlive) return true;
        if (engineAlive(ex.value()) || ex->spawnPending) return false;   // ours runs or is about to
        if (!ex->clientId.isEmpty() && m_ipc && m_ipc->clientIds().contains(ex->clientId)) {
            const qint64 bound = m_ipc->peerProcessId(ex->clientId);
            if (bound > 0 && bound != f.pid) return false;               // another attached process holds the channel
        }
    }
    QString err;
    QWinEventNotifier* w = watchProcess(f.pid, f.createTime, &err);
    if (!w) { qWarning().noquote() << QString("[ChannelManager] cannot adopt engine %1 pid %2: %3").arg(channelId).arg(f.pid).arg(err); return false; }

    const QString configPath = writeConfig(dataDir, config);   // a later restart of this channel spawns from it
    ChannelRecord& rec = m_channels[channelId];
    if (rec.process) { rec.process->deleteLater(); rec.process = nullptr; }   // a dead one only (engineAlive above)
    rec.channelId = channelId;
    rec.channelName = channelName.isEmpty() ? channelId : channelName;
    if (rec.requestedPort <= 0) rec.requestedPort = pickPort();
    rec.actualPort = 0;   // the port the orphan was started with is not known
    rec.dataDir = dataDir;
    rec.configPath = configPath;
    rec.pid = f.pid;
    rec.createTime = f.createTime;
    rec.adopted = true;
    rec.adoptConfirmed = false;
    rec.osAlive = true;
    rec.osWatch = w;
    rec.status = "adopting";
    rec.missedHeartbeats = 0;
    rec.refusedLogged = false;
    rec.spawnPending = false;
    rec.pendingIsRestart = false;
    rec.spawnedAtMono = m_clock.elapsed();
    rec.spawnedByRestart = false;
    rec.lastBeatMono = m_clock.elapsed();
    rec.lastHeartbeatMs = QDateTime::currentMSecsSinceEpoch();
    ++m_adoptedTotal;
    const int deadline = m_reaper.helloDeadlineMs > 0 ? m_reaper.helloDeadlineMs : 15000;
    QTimer::singleShot(deadline, this, [this, channelId, pid = f.pid]() { onAdoptDeadline(channelId, pid); });
    qInfo().noquote() << QString("[ChannelManager] adopting engine %1 pid %2; waiting up to %3 ms for its HELLO").arg(channelId).arg(f.pid).arg(deadline);
    // It may have said HELLO already (it connected while the scan ran, or before it).
    for (auto h = m_hellos.constBegin(); h != m_hellos.constEnd(); ++h) {
        if (h->pid != f.pid || h->channelId != channelId) continue;
        if (m_ipc && !m_ipc->clientIds().contains(h.key())) continue;
        confirmAdoption(channelId, h.key());
        break;
    }
    return true;
}

void ChannelManager::confirmAdoption(const QString& channelId, const QString& clientId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end() || !it->adopted || it->adoptConfirmed) return;
    m_held.remove(clientId);
    it->adoptConfirmed = true;
    if (it->status == "adopting") it->status = "starting";   // bindClient turns it to running
    const qint64 pid = it->pid, waited = m_clock.elapsed() - it->spawnedAtMono;
    ++m_confirmedTotal;
    // Before engineConnected listeners push config and playlists: the engine leaves orphan mode first.
    sendSupAlive(clientId, channelId);
    if (it->clientId != clientId) bindClient(channelId, clientId);
    else { it->status = "running"; }
    qInfo().noquote() << QString("[ChannelManager] adopted engine %1 pid %2 confirmed by its HELLO (%3 ms after the adoption)").arg(channelId).arg(pid).arg(waited);
    emit engineAdopted(channelId, pid);
}

void ChannelManager::onAdoptDeadline(const QString& channelId, qint64 pid) {
    const auto it = m_channels.constFind(channelId);
    if (it == m_channels.constEnd() || !it->adopted || it->pid != pid || it->adoptConfirmed || !it->osAlive) return;
    const QString text = QString("reaped adopted engine pid %1 (%2): no HELLO within %3 ms").arg(pid).arg(channelId).arg(m_reaper.helloDeadlineMs);
    qWarning().noquote() << "[ChannelManager]" << text;
    ++m_reapedTotal;
    emit reaperAlert(channelId, "warning", text);
    requestRestart(channelId, true, QStringLiteral("adopted engine sent no HELLO"));
}

void ChannelManager::onEngineHello(const QString& clientId, const QString& channelId, const QJsonObject& hello) {
    if (channelId.isEmpty() || clientId.isEmpty()) return;
    const qint64 peer = m_ipc ? m_ipc->peerProcessId(clientId) : 0;
    Hello h;
    h.channelId = channelId;
    h.pid = peer > 0 ? peer : qint64(hello.value("pid").toDouble());
    h.state = hello.value("state").toString();
    h.atMs = QDateTime::currentMSecsSinceEpoch();
    h.orphan = hello.value("orphan").toBool();
    m_hellos.insert(clientId, h);
    if (m_retiredClients.contains(clientId) || m_refusedClients.contains(clientId)) return;
    const auto it = m_channels.constFind(channelId);
    if (it != m_channels.constEnd()) {
        if (it->adopted && !it->adoptConfirmed && h.pid > 0 && h.pid == it->pid) { confirmAdoption(channelId, clientId); return; }
        if (h.pid > 0 && it->pid == h.pid && engineAlive(it.value())) {
            // Our engine reconnecting. After an orphan spell (or with a parent other than us) it waits for SUP_ALIVE.
            const qint64 parent = qint64(hello.value("supervisorPid").toDouble());
            if (h.orphan || (parent > 0 && parent != QCoreApplication::applicationPid())) sendSupAlive(clientId, channelId);
            return;
        }
        qint64 owner = (it->pid > 0 && engineAlive(it.value())) ? it->pid : 0;
        if (!owner && !it->clientId.isEmpty() && it->clientId != clientId && m_ipc && m_ipc->clientIds().contains(it->clientId))
            owner = m_ipc->peerProcessId(it->clientId);
        if (h.pid > 0 && owner > 0 && h.pid != owner) { refuseDuplicate(channelId, clientId, h.pid, owner); return; }
    }
    // An engine nobody here runs (a previous Supervisor's, reconnecting after the scan): the scan decides, soon.
    if (m_reaperOn && h.pid > 0 && !trackedPid(h.pid)) {
        if (m_scanRunning || m_holding) m_rescanAgain = true;
        else requestRescan(QString("HELLO from untracked engine pid %1 (%2)").arg(h.pid).arg(channelId));
    }
}

void ChannelManager::startReaper(const ReaperOptions& o) {
    m_reaper = o;
    m_orphanGraceSec = o.orphanGraceSec;
    if (!o.enabled) {
        qInfo() << "[ChannelManager] orphan engine scan off (setting engines.orphanScan)";
        if (o.startupDone) o.startupDone();
        return;
    }
    m_reaperOn = true;
    m_holding = true;
    m_startupPending = true;
    const int limit = qMax(0, o.startupScanTimeoutMs);
    QTimer::singleShot(limit, this, [this, limit]() {
        if (m_startupPending) finishStartup(QString("the orphan scan has not finished after %1 ms; starting channels without it").arg(limit));
    });
    runScan(true);
}

void ChannelManager::requestRescan(const QString& why) {
    if (!m_reaperOn) return;
    m_rescanWhy = why;
    if (!m_rescanDebounce->isActive()) m_rescanDebounce->start(kRescanDebounceMs);
}

void ChannelManager::runScan(bool startup) {
    if (!m_reaperOn) return;
    if (m_scanRunning) { m_rescanAgain = true; return; }
    m_scanRunning = true;
    if (m_engineExe.isEmpty()) m_engineExe = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayChannelEngine.exe");
    reaper::ScanOptions so;
    so.dataDir = m_reaper.dataDir;
    so.ipcName = m_ipcName;
    so.engineExeName = QFileInfo(m_engineExe).fileName();
    if (!startup && m_rescanWhy != QLatin1String("periodic")) qInfo().noquote() << "[ChannelManager] orphan re-scan:" << m_rescanWhy;
    // The scan opens processes and reads files: a worker thread, never this loop. The result comes back through the
    // thread's finished signal (dropped with this object if it is gone by then).
    auto res = std::make_shared<reaper::ScanResult>();
    QThread* th = QThread::create([so, res]() { *res = reaper::scan(so); });
    th->setObjectName(QStringLiteral("orphan-scan"));
    const qint64 t0 = m_clock.elapsed();
    connect(th, &QThread::finished, this, [this, res, startup, t0]() { m_lastScanMs = m_clock.elapsed() - t0; applyScan(*res, startup); });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start(QThread::LowPriority);
}

void ChannelManager::applyScan(const reaper::ScanResult& r, bool startup) {
    m_scanRunning = false;
    ++m_scans;
    const bool first = startup && m_startupPending;
    const qint64 self = QCoreApplication::applicationPid();
    QVector<reaper::EngineFacts> untracked;
    for (reaper::EngineFacts f : r.engines) {
        if (f.pid == self || trackedPid(f.pid)) continue;
        for (const Hello& h : std::as_const(m_hellos)) if (h.pid == f.pid) { f.state = h.state; f.helloAtMs = h.atMs; }
        untracked << f;
    }
    for (const QString& n : r.notes)   // what the scan left alone and why: once per note, not every minute
        if (!m_notesSeen.contains(n) && m_notesSeen.size() < 1000) { m_notesSeen.insert(n); qInfo().noquote() << "[ChannelManager] orphan scan:" << n; }

    QHash<QString, sched::ChannelProfile> profiles;
    for (const reaper::EngineFacts& f : std::as_const(untracked)) {
        if (profiles.contains(f.channelId)) continue;
        sched::ChannelProfile p;
        if (m_reaper.profileOf && m_reaper.profileOf(f.channelId, &p)) profiles.insert(f.channelId, p);
    }
    const reaper::ProfileLookup lookup = [&profiles](const QString& id) -> const sched::ChannelProfile* {
        const auto it = profiles.constFind(id);
        return it == profiles.constEnd() ? nullptr : &it.value();
    };
    const reaper::BuildFacts build = currentBuild();
    const bool adoptOn = !m_reaper.adoptEnabled || m_reaper.adoptEnabled();
    int adopted = 0, reaped = 0;
    for (reaper::ReapDecision d : reaper::decideAll(untracked, lookup, build, adoptOn)) {
        const reaper::EngineFacts f = d.facts;
        const auto rec = m_channels.constFind(f.channelId);
        if (d.verdict == reaper::Verdict::Adopt && rec != m_channels.constEnd() && (engineAlive(rec.value()) || rec->spawnPending)) {
            d = reaper::decide(f, lookup(f.channelId), build, adoptOn, true);   // this Supervisor runs the channel already
        } else if (d.verdict == reaper::Verdict::Adopt && rec != m_channels.constEnd() && rec->status == "stopped") {
            d.verdict = reaper::Verdict::Kill;                                  // the operator stopped the channel
            d.reason = reaper::Reason::Disabled;
            d.text = QString("reaped orphan engine pid %1 (%2, channel stopped)").arg(f.pid).arg(f.channelId);
        }
        if (d.verdict == reaper::Verdict::Adopt) {
            const sched::ChannelProfile* p = lookup(f.channelId);
            const QString dir = QDir(m_reaper.dataDir).filePath("channels/" + f.channelId);   // where a respawn goes
            const QJsonObject cfg = (p && m_reaper.engineConfig) ? m_reaper.engineConfig(*p) : QJsonObject();
            if (p && adoptChannel(f.channelId, p->name, dir, cfg, f)) {
                ++adopted;
                qInfo().noquote() << "[ChannelManager]" << d.text;
                emit reaperAlert(f.channelId, "info", d.text);
                continue;
            }
            d.verdict = reaper::Verdict::Kill;
            d.text = QString("reaped orphan engine pid %1 (%2): it could not be adopted").arg(f.pid).arg(f.channelId);
        }
        if (reapEngine(f, d.text)) {
            ++reaped;
            qWarning().noquote() << "[ChannelManager]" << d.text;
            emit reaperAlert(f.channelId, "warning", d.text);
        }
    }
    if (!untracked.isEmpty() || first)
        qInfo().noquote() << QString("[ChannelManager] orphan scan%1: %2 untracked engine(s) in scope, %3 adopted, %4 reaped, %5 stale lock(s), %6 ms")
                                 .arg(first ? " (startup)" : "").arg(untracked.size()).arg(adopted).arg(reaped).arg(r.staleLocks.size()).arg(m_lastScanMs);
    emit scanApplied(int(untracked.size()), adopted, reaped);
    if (first) finishStartup(QStringLiteral("orphan scan done"));
    if (m_rescanAgain && !m_holding) { m_rescanAgain = false; requestRescan(QStringLiteral("an engine said HELLO during the scan")); }
}

void ChannelManager::finishStartup(const QString& why) {
    if (!m_startupPending) return;
    m_startupPending = false;
    m_holding = false;
    // Binds held during the scan that no adoption took: what bindClient did before (an attached engine gets a record).
    const QHash<QString, QString> held = m_held;
    m_held.clear();
    for (auto it = held.constBegin(); it != held.constEnd(); ++it)
        if (m_ipc && m_ipc->clientIds().contains(it.key())) bindClient(it.value(), it.key());
    if (m_reaper.rescanMs > 0) m_rescanTimer->start(m_reaper.rescanMs);
    qInfo().noquote() << "[ChannelManager]" << why << "- starting channels";
    if (m_reaper.startupDone) m_reaper.startupDone();
    if (m_rescanAgain && !m_scanRunning) { m_rescanAgain = false; requestRescan(QStringLiteral("an engine said HELLO during the startup scan")); }
}

QJsonObject ChannelManager::reaperStats() const {
    QJsonObject o;
    o["enabled"] = m_reaperOn;
    o["holding"] = m_holding;
    o["scans"] = double(m_scans);
    o["lastScanMs"] = double(m_lastScanMs);
    o["adopted"] = double(m_adoptedTotal);
    o["adoptionsConfirmed"] = double(m_confirmedTotal);
    o["reaped"] = double(m_reapedTotal);
    o["duplicatesRefused"] = double(m_refusedTotal);
    o["retiring"] = retiringCount();
    o["rescanMs"] = m_reaper.rescanMs;
    o["helloDeadlineMs"] = m_reaper.helloDeadlineMs;
    return o;
}
