#include "supervisorlink.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include "../core/frameclock.h"
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

namespace {
int intKey(const QJsonObject& o, const char* key, bool* found) {
    const QJsonValue v = o.value(QLatin1String(key));
    bool ok = false;
    int n = 0;
    if (v.isDouble()) { n = v.toInt(); ok = true; }
    else if (v.isString()) n = v.toString().trimmed().toInt(&ok);
    *found = ok;
    return n;
}
} // namespace

int SupervisorLink::resolveOrphanGraceSec(const QJsonObject& startConfig, bool explicitParent) {
    bool found = false;
    const int n = intKey(startConfig, "orphanGraceSec", &found);
    if (found) return n < 0 ? -1 : qMin(n, 7 * 24 * 3600);
    // Explicit parent (--supervisor-pid, a Supervisor that adopts): the design's 300 s. Parent learned from the pipe (a
    // Supervisor without PS-4b started us): play on as before when no Supervisor comes back.
    return explicitParent ? 300 : -1;
}

int SupervisorLink::resolveAdoptWaitSec(const QJsonObject& startConfig) {
    bool found = false;
    const int n = intKey(startConfig, "orphanAdoptWaitSec", &found);
    if (!found) return 20;
    return qBound(0, n, 3600);
}

int SupervisorLink::claimChannel(enginelock::InstanceGuard& guard, const QString& channelId, const QString& dataDir,
                                 bool takeover, int waitMs) {
    if (!guard.error().isEmpty() && !guard.owned()) {
        // Fail open: a guard that cannot be created must not keep the channel off air (today's behaviour).
        qWarning().noquote() << "[Engine] duplicate guard" << guard.name() << "unavailable:" << guard.error() << "- starting without it";
        return 0;
    }
    if (guard.acquire(0)) {
        if (guard.abandoned()) qInfo().noquote() << "[Engine] the previous engine of channel" << channelId << "ended without releasing its guard";
        return 0;
    }
    qInfo().noquote() << QString("[Engine] another engine holds channel %1; waiting up to %2 ms").arg(channelId).arg(waitMs);
    if (guard.acquire(waitMs)) return 0;
    if (!guard.error().isEmpty()) {
        // The wait itself failed (WAIT_FAILED), which says nothing about another engine: fail open as above.
        qWarning().noquote() << "[Engine] duplicate guard" << guard.name() << "failed:" << guard.error() << "- starting without it";
        return 0;
    }
    enginelock::LockInfo owner;
    const bool known = enginelock::readLock(enginelock::lockPath(dataDir), &owner) && owner.channelId == channelId;
    const QString who = known ? QString("pid %1").arg(owner.pid) : QStringLiteral("pid unknown");
    if (takeover) {
        qWarning().noquote() << QString("[Engine] --takeover: channel %1 is still held by %2 after %3 ms; starting anyway and taking the guard when it exits")
                                    .arg(channelId, who).arg(waitMs);
        guard.acquireInBackground();
        return 0;
    }
    qCritical().noquote() << QString("[Engine] another engine owns channel %1 (%2); exiting with code %3")
                                 .arg(channelId, who).arg(enginelock::kExitDuplicate);
    return enginelock::kExitDuplicate;
}

SupervisorLink::SupervisorLink(const Options& opt, QObject* parent) : QObject(parent), m_opt(opt) {
    if (!m_opt.dataDir.isEmpty()) m_lockPath = enginelock::lockPath(m_opt.dataDir);
    m_ipc.setCaps(IpcTransport::Caps{});   // 4 MB droppable / 64 MB disconnect: this link reconnects
    connect(&m_ipc, &IpcTransport::connectionStateChanged, this, &SupervisorLink::onStateChanged);
    connect(&m_ipc, &IpcTransport::connectFailed, this, &SupervisorLink::onConnectFailed);
    connect(&m_ipc, &IpcTransport::messageReceived, this, [this](const IpcProtocol::Message& m, const QString&) { onMessage(m); });
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &SupervisorLink::attempt);
    m_grace.setSingleShot(true);
    connect(&m_grace, &QTimer::timeout, this, [this]() {
        requestExit(enginelock::kExitOrphan, QString("no Supervisor adopted this engine within orphanGraceSec %1 s").arg(m_opt.orphanGraceSec));
    });
    m_adoptWait.setSingleShot(true);
    connect(&m_adoptWait, &QTimer::timeout, this, [this]() {
        requestExit(enginelock::kExitOrphan,
                    QString("connected to Supervisor pid %1 for %2 s without being adopted (it predates engine adoption or chose "
                            "not to adopt); leaving channel %3 to the engine it starts").arg(m_serverPid).arg(m_opt.adoptWaitSec).arg(m_opt.channelId));
    });
}

SupervisorLink::~SupervisorLink() {
    m_exiting = true;
    m_retry.stop(); m_grace.stop(); m_adoptWait.stop();
    unwatchParent();
    if (m_lockWritten) enginelock::removeLockIfOwned(m_lockPath, m_self.pid);
    // The transport is a member destroyed after this body: nothing it reports on the way down may reach us.
    QObject::disconnect(&m_ipc, nullptr, this, nullptr);
    m_ipc.disconnect();
}

void SupervisorLink::start() {
    if (m_started) return;
    m_started = true;
    m_clock.start();
    m_self = enginelock::currentProcessInfo();
    m_self.channelId = m_opt.channelId;
    m_self.configHash = m_opt.configHash;
    m_self.supervisorIpc = m_opt.ipcName;
    if (!m_opt.dataDir.isEmpty()) m_self.dataDir = QDir::toNativeSeparators(QDir(m_opt.dataDir).absolutePath());
    m_explicitParent = m_opt.supervisorPid > 0;
    if (m_explicitParent) watchParent(m_opt.supervisorPid);
    writeLock();
    qInfo().noquote() << QString("[SupervisorLink] channel %1: connecting to supervisor %2 (parent %3; orphan grace %4; adopt wait %5)")
                             .arg(m_opt.channelId, m_opt.ipcName,
                                  m_explicitParent ? QString("pid %1").arg(m_opt.supervisorPid) : QStringLiteral("learned at connect"),
                                  m_opt.orphanGraceSec < 0 ? QStringLiteral("never") : QString("%1 s").arg(m_opt.orphanGraceSec),
                                  m_opt.adoptWaitSec <= 0 ? QStringLiteral("never") : QString("%1 s").arg(m_opt.adoptWaitSec));
    attempt();
}

bool SupervisorLink::isConnected() const { return m_ipc.isConnected(); }

bool SupervisorLink::send(const IpcProtocol::Message& msg, IpcTransport::Priority p) { return m_ipc.send(msg, p); }

void SupervisorLink::attempt() {
    if (m_exiting || !m_started || m_ipc.isConnected() || m_ipc.isConnecting()) return;
    ++m_attempts;
    m_ipc.connectToServerAsync(m_opt.ipcName, m_opt.connectTimeoutMs);   // ends in onStateChanged(true) or onConnectFailed
}

void SupervisorLink::scheduleRetry() {
    if (m_exiting || !m_started || m_retry.isActive() || m_ipc.isConnecting() || m_ipc.isConnected()) return;
    m_retry.start(qMax(50, m_opt.reconnectMs));
}

// The transport reports a close more than once (stateChanged and disconnected): only real transitions count.
void SupervisorLink::onStateChanged(bool up) {
    if (up == m_up) { if (!up) scheduleRetry(); return; }
    m_up = up;
    if (up) {
        ++m_connects;
        m_retry.stop();
        m_serverPid = m_ipc.peerProcessId();
        if (m_downSinceMs < 0 && m_connects == 1)
            qInfo().noquote() << QString("[SupervisorLink] connected to supervisor %1 (pid %2)").arg(m_opt.ipcName).arg(m_serverPid);
        else
            qInfo().noquote() << QString("[SupervisorLink] reconnected to supervisor %1 (pid %2) after %3 ms, %4 attempt(s)")
                                     .arg(m_opt.ipcName).arg(m_serverPid).arg(m_downSinceMs >= 0 ? nowMs() - m_downSinceMs : 0).arg(m_outageFails + 1);
        m_downSinceMs = -1; m_outageFails = 0; m_lastFailLogMs = -1;
        if (m_parentPid == 0 && m_serverPid > 0 && !m_orphan) {
            watchParent(m_serverPid);   // no --supervisor-pid: the Supervisor at the other end is our parent
            writeLock();
        } else if (!m_orphan && m_serverPid > 0 && m_serverPid != m_parentPid && !m_foreignWarned) {
            m_foreignWarned = true;
            qWarning().noquote() << QString("[SupervisorLink] connected to a Supervisor with pid %1, not this engine's parent pid %2 (two Supervisors on one IPC name?)")
                                        .arg(m_serverPid).arg(m_parentPid);
        }
        sendHelloAndReady();   // first on the connection, before anyone hears connectedChanged
        if (m_orphan && m_opt.adoptWaitSec > 0) m_adoptWait.start(m_opt.adoptWaitSec * 1000);
        emit connectedChanged(true);
    } else {
        m_serverPid = 0;
        m_adoptWait.stop();
        m_downSinceMs = nowMs();
        if (!m_exiting) qWarning().noquote() << QString("[SupervisorLink] supervisor link lost; reconnecting every %1 ms").arg(m_opt.reconnectMs);
        emit connectedChanged(false);
        scheduleRetry();
    }
}

void SupervisorLink::onConnectFailed(const QString& reason) {
    if (m_downSinceMs < 0) m_downSinceMs = nowMs();
    ++m_outageFails;
    const qint64 t = nowMs();
    // One line when an outage starts, then one a minute: an orphan retries every second for minutes.
    if (m_outageFails == 1 || m_lastFailLogMs < 0 || t - m_lastFailLogMs >= 60000) {
        m_lastFailLogMs = t;
        if (m_outageFails == 1)
            qWarning().noquote() << QString("[SupervisorLink] supervisor %1 not reachable (%2); retrying every %3 ms").arg(m_opt.ipcName, reason).arg(m_opt.reconnectMs);
        else
            qWarning().noquote() << QString("[SupervisorLink] supervisor %1 still not reachable after %2 s (%3 attempts): %4")
                                        .arg(m_opt.ipcName).arg((t - m_downSinceMs) / 1000).arg(m_outageFails).arg(reason);
    }
    scheduleRetry();
}

void SupervisorLink::onMessage(const IpcProtocol::Message& msg) {
    using namespace IpcProtocol;
    if (msg.type == MSG_SUP_ALIVE) {
        // Addressed to our channel = the Supervisor bound this engine (adoption); a broadcast is liveness for UIs.
        if (!msg.channelId.isEmpty() && msg.channelId == m_opt.channelId) {
            const qint64 pid = qint64(msg.payload.value("pid").toDouble());
            adoptBy(pid > 0 ? pid : m_serverPid);
        }
        return;
    }
    if (msg.type == MSG_ENGINE_FATAL) {
        // Supervisor -> engine only for the PS-4b binding guard: another engine is bound to this channel.
        const qint64 pid = qint64(msg.payload.value("pid").toDouble());
        if (msg.channelId == m_opt.channelId && msg.payload.value("reason").toString() == QLatin1String("duplicate") && (pid <= 0 || pid == m_self.pid))
            requestExit(enginelock::kExitDuplicate, QString("the Supervisor refused this engine as a duplicate for channel %1%2").arg(m_opt.channelId,
                        msg.payload.value("message").toString().isEmpty() ? QString() : ": " + msg.payload.value("message").toString()));
        return;   // the engine never acted on a fatal from the Supervisor before
    }
    emit messageReceived(msg);
}

void SupervisorLink::sendHelloAndReady() {
    ++m_hellos;
    m_ipc.send(IpcProtocol::makeEngineHello(m_opt.channelId, helloPayload()));
    IpcProtocol::Message ready;
    ready.type = IpcProtocol::MSG_ENGINE_READY;
    ready.channelId = m_opt.channelId;
    ready.timestampNs = FrameClock::nowNs();
    ready.payload = m_opt.readyPayload;
    ready.payload["pid"] = double(QCoreApplication::applicationPid());
    m_ipc.send(ready);
}

QJsonObject SupervisorLink::helloPayload() const {
    QJsonObject h = m_facts ? m_facts() : QJsonObject();
    if (!h.contains("state")) h["state"] = QString();
    if (!h.contains("itemId")) h["itemId"] = QString();
    if (!h.contains("frame")) h["frame"] = 0.0;
    if (!h.contains("outputs")) h["outputs"] = QJsonArray();
    h["channelId"] = m_opt.channelId;
    h["pid"] = double(m_self.pid ? m_self.pid : QCoreApplication::applicationPid());
    h["createTime"] = QString::number(m_self.createTime);
    h["createTimeMs"] = double(enginelock::createTimeToMs(m_self.createTime));
    h["exe"] = m_self.exe;
    h["exeMtime"] = double(m_self.exeMtimeMs);
    h["configHash"] = m_opt.configHash;
    h["supervisorIpc"] = m_opt.ipcName;
    h["supervisorPid"] = double(m_parentPid);
    h["orphan"] = m_orphan;
    h["orphanForMs"] = double(m_orphan && m_orphanSinceMs >= 0 ? nowMs() - m_orphanSinceMs : 0);
    h["connects"] = double(m_connects);
    h["version"] = m_self.version;
    h["helloVersion"] = 1;
    return h;
}

QJsonObject SupervisorLink::stats() const {
    QJsonObject o;
    o["connected"] = isConnected();
    o["attempts"] = double(m_attempts);
    o["connects"] = double(m_connects);
    o["hellos"] = double(m_hellos);
    o["adoptions"] = double(m_adoptions);
    o["orphan"] = m_orphan;
    o["parentPid"] = double(m_parentPid);
    o["explicitParent"] = m_explicitParent;
    o["serverPid"] = double(m_serverPid);
    return o;
}

void SupervisorLink::watchParent(qint64 pid) {
    unwatchParent();
    m_parentPid = pid;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!h) {
        const DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) {   // no such process: it is already gone
            QTimer::singleShot(0, this, [this, pid]() { onParentExited(pid); });
        } else {
            qWarning().noquote() << QString("[SupervisorLink] cannot watch Supervisor pid %1 (Windows error %2): orphan mode will not start on its own")
                                        .arg(pid).arg(e);
        }
        return;
    }
    auto* n = new QWinEventNotifier(h, this);
    // The handle lives exactly as long as the notifier waiting on it (unwatchParent may run inside its own signal).
    QObject::connect(n, &QObject::destroyed, [h]() { CloseHandle(h); });
    connect(n, &QWinEventNotifier::activated, this, [this, pid]() { onParentExited(pid); });
    n->setEnabled(true);
    m_parentNotifier = n;
#endif
}

void SupervisorLink::unwatchParent() {
#ifdef Q_OS_WIN
    if (!m_parentNotifier) return;
    m_parentNotifier->setEnabled(false);   // a process handle stays signalled: never report it twice
    m_parentNotifier->deleteLater();
    m_parentNotifier = nullptr;
#endif
}

void SupervisorLink::onParentExited(qint64 pid) {
    if (m_exiting || pid != m_parentPid || m_orphan) return;
    unwatchParent();
    enterOrphan(QString("Supervisor pid %1 exited").arg(pid));
}

void SupervisorLink::enterOrphan(const QString& why) {
    if (m_orphan || m_exiting) return;
    m_orphan = true;
    m_orphanSinceMs = nowMs();
    const int g = m_opt.orphanGraceSec;
    qWarning().noquote() << QString("[SupervisorLink] channel %1: %2: orphan mode. Outputs keep playing, reconnecting every %3 ms; %4")
                                .arg(m_opt.channelId, why).arg(m_opt.reconnectMs)
                                .arg(g < 0 ? QStringLiteral("no exit deadline (orphanGraceSec -1)")
                                     : g == 0 ? QStringLiteral("exiting now (orphanGraceSec 0)")
                                     : QString("exiting in %1 s unless a Supervisor adopts this engine").arg(g));
    emit orphanChanged(true);
    if (g == 0) { requestExit(enginelock::kExitOrphan, QStringLiteral("the Supervisor exited and orphanGraceSec is 0")); return; }
    if (g > 0) m_grace.start(g * 1000);
    // Already connected to another Supervisor (its connect beat the parent's exit notification): the same deadline.
    if (m_up && m_opt.adoptWaitSec > 0 && m_serverPid > 0 && m_serverPid != m_parentPid) m_adoptWait.start(m_opt.adoptWaitSec * 1000);
}

void SupervisorLink::adoptBy(qint64 pid) {
    if (pid <= 0 || m_exiting) return;
    const bool wasOrphan = m_orphan;
    if (!wasOrphan && pid == m_parentPid) return;   // the 1 Hz confirmation from the Supervisor we already follow
    m_grace.stop();
    m_adoptWait.stop();
    const qint64 orphanFor = wasOrphan && m_orphanSinceMs >= 0 ? nowMs() - m_orphanSinceMs : -1;
    const qint64 old = m_parentPid;
    m_orphan = false;
    m_orphanSinceMs = -1;
    ++m_adoptions;
    watchParent(pid);   // the adopting Supervisor is the parent from now on
    writeLock();
    if (wasOrphan)
        qInfo().noquote() << QString("[SupervisorLink] channel %1 adopted by Supervisor pid %2 after %3 ms as an orphan").arg(m_opt.channelId).arg(pid).arg(orphanFor);
    else
        qInfo().noquote() << QString("[SupervisorLink] channel %1: Supervisor pid %2 bound this engine (parent was pid %3)").arg(m_opt.channelId).arg(pid).arg(old);
    if (wasOrphan) emit orphanChanged(false);
    emit adopted(pid);
}

void SupervisorLink::requestExit(int code, const QString& reason) {
    if (m_exiting) return;
    m_exiting = true;
    m_retry.stop(); m_grace.stop(); m_adoptWait.stop();
    qCritical().noquote() << QString("[SupervisorLink] channel %1: %2; exiting with code %3").arg(m_opt.channelId, reason).arg(code);
    emit exitRequested(code, reason);
}

void SupervisorLink::writeLock() {
    if (m_lockPath.isEmpty() || !m_started) return;
    enginelock::LockInfo l = m_self;
    l.supervisorPid = m_parentPid;
    QString err;
    if (enginelock::writeLock(m_lockPath, l, &err)) m_lockWritten = true;
    else qWarning().noquote() << "[SupervisorLink] cannot write" << m_lockPath << ":" << err;
}
