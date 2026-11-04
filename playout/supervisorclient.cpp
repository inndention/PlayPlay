#include "supervisorclient.h"
#include "core/processlaunch.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>
#include <QDebug>

SupervisorClient::SupervisorClient(QObject* parent) : QObject(parent) {
    m_model = new ChannelListModel(this);
    m_clock.start();
    m_meterTimer.setSingleShot(true);
    m_meterTimer.setTimerType(Qt::PreciseTimer);   // a coarse timer may fire up to 5 % early; the 15 Hz cap is a floor on the gap
    connect(&m_meterTimer, &QTimer::timeout, this, &SupervisorClient::onMeterTimer);
    if (const int sec = qEnvironmentVariableIntValue("PLAYPLAY_UI_STATS_SEC"); sec > 0) {   // diagnostics only; off by default
        m_uiStatsLog.setInterval(sec * 1000);
        connect(&m_uiStatsLog, &QTimer::timeout, this, &SupervisorClient::logUiStats);
        m_uiStatsLog.start();
    }
    connect(&m_reconnect, &QTimer::timeout, this, &SupervisorClient::tryConnect);
    // No 4 s status poll any more (PA-4): this timer only checks that snapshots keep coming (or runs the old poll when
    // statusPollMs asks for it). Interval set in start().
    connect(&m_refresh, &QTimer::timeout, this, &SupervisorClient::onRefreshTimer);
    m_stallTick.setInterval(1000);
    connect(&m_stallTick, &QTimer::timeout, this, [this]() {
        if (++m_stallTicks % 2 == 0) probeHealth();
        updateStallInfo();
    });
}

SupervisorClient::~SupervisorClient() { stop(); }

void SupervisorClient::start(const Options& opt) {
    m_opt = opt;
    if (m_opt.executable.isEmpty()) m_opt.executable = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlaySupervisor.exe");
    if (m_opt.statusPollMs < 0) m_opt.statusPollMs = qMax(0, qEnvironmentVariableIntValue("PLAYPLAY_STATUS_POLL_MS"));
    if (m_opt.binaryPreviews && qEnvironmentVariableIsSet("PLAYPLAY_IPC_BINARY")) {
        const QString v = qEnvironmentVariable("PLAYPLAY_IPC_BINARY").trimmed().toLower();
        if (v == "0" || v == "false" || v == "off" || v == "no") { m_opt.binaryPreviews = false; qInfo() << "[SupervisorClient] PLAYPLAY_IPC_BINARY is off: preview pictures as JSON/base64"; }
    }
    if (m_opt.statusPollMs > 0) qInfo().noquote() << QString("[SupervisorClient] status query every %1 ms (the poll from before PA-4)").arg(m_opt.statusPollMs);
    // The stale check needs no precision: a third of the window, so a missing snapshot is noticed within 1.33x of it.
    m_refresh.setInterval(m_opt.statusPollMs > 0 ? m_opt.statusPollMs : qMax(100, m_opt.snapshotStaleMs / 3));
    startWorker();
    tryConnect();
    m_reconnect.start(2000);
}

void SupervisorClient::stop() {
    m_reconnect.stop();
    m_refresh.stop();
    m_stallTick.stop();
    m_meterTimer.stop(); m_meterDue.clear();
    m_uiStatsLog.stop();
    stopWorker();
    // Quiet reset (no signals): stop() runs at teardown, after QML is gone; a later start() begins from scratch.
    m_connected = false; m_connecting = false; m_stalled = false; m_stallBusy = false; m_stallInfo.clear();
    if (m_nam) {
        const auto replies = m_nam->findChildren<QNetworkReply*>();
        for (QNetworkReply* r : replies) { r->disconnect(this); r->abort(); r->deleteLater(); }
        m_healthInFlight = false;
    }
    if (m_process) {
        // The Supervisor keeps playing out when the UI closes; only stop one we launched if asked.
        m_process->setParent(nullptr);
        m_process->deleteLater();
        m_process = nullptr;
    }
}

void SupervisorClient::startWorker() {
    if (m_worker) return;
    m_thread = new QThread;   // no parent: if it ever failed to stop it is left running, never destroyed while running
    m_thread->setObjectName(QStringLiteral("supervisor-ipc"));
    m_worker = new SupervisorClientWorker;
    m_worker->setStallMs(m_opt.stallMs);
    m_worker->setAcceptBinary(m_opt.binaryPreviews);   // before the thread starts; the transport is created on connect
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &SupervisorClientWorker::inboxReady, this, &SupervisorClient::drainInbox, Qt::QueuedConnection);
    connect(m_worker, &SupervisorClientWorker::connectionStateChanged, this, &SupervisorClient::onConnectionState, Qt::QueuedConnection);
    connect(m_worker, &SupervisorClientWorker::connectFailed, this, &SupervisorClient::onConnectFailed, Qt::QueuedConnection);
    connect(m_worker, &SupervisorClientWorker::stalledChanged, this, &SupervisorClient::onStalledChanged, Qt::QueuedConnection);
    m_thread->start();
}

void SupervisorClient::stopWorker() {
    if (!m_worker) return;
    SupervisorClientWorker* w = m_worker;
    QThread* t = m_thread;
    m_worker = nullptr; m_thread = nullptr;   // queued reports still in flight see no worker and return
    QObject::disconnect(w, nullptr, this, nullptr);
    // The pipe is closed on its own thread, then the thread ends and the worker deletes itself. Pipe I/O is overlapped,
    // so nothing in there waits for the Supervisor: this returns in milliseconds even when the Supervisor is hung.
    QMetaObject::invokeMethod(w, [w]() { w->shutdown(); QThread::currentThread()->quit(); }, Qt::QueuedConnection);
    if (t->wait(3000)) delete t;
    else qWarning() << "[SupervisorClient] the IPC thread did not stop within 3 s; leaving it behind";
}

void SupervisorClient::sendToWorker(const IpcProtocol::Message& msg) {
    if (!m_worker) return;
    SupervisorClientWorker* w = m_worker;   // the queued call is dropped with the worker if it goes first
    QMetaObject::invokeMethod(w, [w, msg]() { w->send(msg); }, Qt::QueuedConnection);
}

void SupervisorClient::tryConnect() {
    if (m_connected || m_connecting || !m_worker) return;
    // Async (perf-app PA-1): the blocking connectToServer sat in WaitNamedPipe for 5 s on this thread whenever the
    // Supervisor's loop was hung. The answer comes back as onConnectionState / onConnectFailed.
    m_connecting = true;
    SupervisorClientWorker* w = m_worker;
    const QString name = m_opt.ipcName;
    const int budget = m_opt.connectTimeoutMs;
    QMetaObject::invokeMethod(w, [w, name, budget]() { w->connectTo(name, budget); }, Qt::QueuedConnection);
}

void SupervisorClient::onConnectFailed(const QString& reason, bool busy) {
    if (!m_worker) return;
    m_connecting = false;
    if (reason != m_lastConnectError) { m_lastConnectError = reason; qInfo().noquote() << "[SupervisorClient] cannot connect to" << m_opt.ipcName + ":" << reason; }
    // Launch our own only when there is no Supervisor at all: a busy pipe means one exists but is hung, and a second
    // one would only fight it for the ports.
    if (!busy && m_opt.launchIfMissing && !m_triedLaunch) launchSupervisor();
}

void SupervisorClient::launchSupervisor() {
    m_triedLaunch = true;
    if (!QFileInfo::exists(m_opt.executable)) {
        qWarning() << "[SupervisorClient] supervisor executable not found:" << m_opt.executable;
        return;
    }
    m_process = new QProcess(this);
    processlaunch::configureDetachedChild(m_process);   // own hidden console, NUL stdio: never blocks on / dies with ours
    QStringList args = m_opt.arguments;
    args << "--ipc-name" << m_opt.ipcName << "--rest-port" << QString::number(m_opt.restPort);
    if (!m_opt.dataDir.isEmpty()) args << "--data-dir" << m_opt.dataDir;
    const QString engine = QDir(QFileInfo(m_opt.executable).absolutePath()).filePath("PlayPlayChannelEngine.exe");
    if (QFileInfo::exists(engine)) args << "--engine-path" << engine;
    m_process->setProgram(m_opt.executable);
    m_process->setArguments(args);
    m_process->setWorkingDirectory(QFileInfo(m_opt.executable).absolutePath());
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        qWarning() << "[SupervisorClient] supervisor exited with code" << code;
        m_process->deleteLater(); m_process = nullptr; m_triedLaunch = false;   // allow relaunch
        emit connectedChanged();
    });
    m_process->start();
    qInfo() << "[SupervisorClient] launched" << m_opt.executable << args;
    emit connectedChanged();
}

void SupervisorClient::onConnectionState(bool up) {
    if (!m_worker) return;
    m_connecting = false;
    if (up == m_connected) return;
    if (!up) {
        // Everything the old connection delivered goes in first, unsliced, so none of it lands after "disconnected".
        SupervisorClientWorker::Delivery d;
        while (m_worker && m_worker->takeNext(&d)) apply(d);
    }
    m_connected = up;
    if (up) {
        m_lastConnectError.clear();
        qInfo() << "[SupervisorClient] connected to" << m_opt.ipcName;
        query(QueryConnect);   // registers us; a new connection is a new client, so the reply is the full state
        if (m_opt.statusPollMs > 0 || m_opt.snapshotStaleMs > 0) m_refresh.start();
    } else {
        m_refresh.stop();
        qWarning() << "[SupervisorClient] disconnected";
        // Said out loud (PA-3): delegates now outlive snapshots, so a card or tile that took its telemetry through a
        // Connections handler would keep the old connection's ON AIR until that engine reports again (or for good).
        const QStringList gone = m_telemetry.keys();
        m_telemetry.clear();
        for (const QString& id : gone) emit telemetryChanged(id);
        if (!gone.isEmpty()) { ++m_ui.activeState; emit activeStateChanged(); }
    }
    emit connectedChanged();
}

void SupervisorClient::onStalledChanged(bool stalled, qint64 silentMs, bool busy) {
    if (!m_worker) return;
    const bool wasBusy = m_stallBusy;
    m_stallBusy = stalled && busy;
    if (stalled == m_stalled) { if (wasBusy != m_stallBusy) updateStallInfo(); return; }
    m_stalled = stalled;
    if (stalled) {
        m_stallSinceMs = QDateTime::currentMSecsSinceEpoch() - qMax<qint64>(0, busy ? m_opt.connectTimeoutMs : silentMs);
        m_stallTicks = 0; m_health.clear(); m_healthError.clear();
        m_stallTick.start();
        probeHealth();
    } else {
        m_stallTick.stop();
        // One query after the silence: same connection, so a Supervisor with PS-6 answers with the snapshot and only
        // the runlists that changed since it last sent us one.
        if (m_connected) query(QueryRecover);
    }
    updateStallInfo();
    emit supervisorStalledChanged();
}

int SupervisorClient::healthPort() const {
    if (m_opt.healthPort >= 0) return m_opt.healthPort;
    return m_opt.restPort == 7700 ? 7709 : m_opt.restPort + 2;
}

void SupervisorClient::updateStallInfo() {
    QVariantMap info;
    if (m_stalled) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        info["reason"] = m_stallBusy ? QStringLiteral("busy") : QStringLiteral("silent");
        info["sinceMs"] = m_stallSinceMs;
        const qint64 silent = (!m_stallBusy && m_worker) ? m_worker->silentMs() : -1;
        info["silentMs"] = silent >= 0 ? silent : now - m_stallSinceMs;
        info["healthPort"] = healthPort();
        if (!m_health.isEmpty()) info["health"] = m_health;
        if (!m_healthError.isEmpty()) info["healthError"] = m_healthError;
    }
    if (info == m_stallInfo) return;
    m_stallInfo = info;
    emit stallInfoChanged();
}

void SupervisorClient::probeHealth() {
    // The Supervisor's watchdog answers /healthz from its own thread, so it tells what the hung loop is doing
    // (activity, lag) while IPC and REST are silent. Async, at most one request in flight, only while stalled.
    const int port = healthPort();
    if (!m_stalled || port <= 0 || m_healthInFlight) return;
    if (!m_nam) { m_nam = new QNetworkAccessManager(this); m_nam->setProxy(QNetworkProxy::NoProxy); }
    QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/healthz").arg(port)));
    req.setTransferTimeout(1500);
    m_healthInFlight = true;
    QNetworkReply* r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        m_healthInFlight = false;
        r->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        if (r->error() == QNetworkReply::NoError && o.contains("state")) { m_health = o.toVariantMap(); m_healthError.clear(); }
        else { m_health.clear(); m_healthError = r->error() == QNetworkReply::NoError ? QStringLiteral("no health report") : r->errorString(); }
        updateStallInfo();
    });
}

QVariantMap SupervisorClient::snapshot() const {
    ++m_reads.snapshot;
    if (!m_snapshotMapValid) { m_snapshotMap = m_snapshot.toVariantMap(); m_snapshotMapValid = true; }
    return m_snapshotMap;
}

void SupervisorClient::refresh() {
    // Skipped while stalled: each request would become one more full snapshot in a hung Supervisor's backlog.
    if (m_connected && m_worker && !m_stalled) query(QueryOperator);
}

void SupervisorClient::query(QueryReason why) {
    ++m_queries[why];
    m_lastSnapshotMs = m_clock.elapsed();   // the answer is a snapshot: the stale window starts again
    sendToWorker(IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, QString(), {}));
}

void SupervisorClient::onRefreshTimer() {
    if (!m_connected || !m_worker || m_stalled) return;   // after a stall, onStalledChanged asks once
    if (m_opt.statusPollMs > 0) { query(QueryPoll); return; }
    if (m_opt.snapshotStaleMs <= 0) return;
    const qint64 quiet = m_clock.elapsed() - m_lastSnapshotMs;
    if (quiet < m_opt.snapshotStaleMs) return;
    // A pushing Supervisor never gets here (one at least every 10 s): a dropped push and a missed keepalive, or a
    // Supervisor that does not push. One query per window, never a stream.
    qInfo().noquote() << QString("[SupervisorClient] no snapshot for %1 ms: asking for one").arg(quiet);
    query(QueryStale);
}

void SupervisorClient::setActiveChannelId(const QString& id) {
    if (id == m_active) return;
    m_active = id;
    emit activeChannelChanged();
    emit telemetryChanged(id);   // kept: Connections handlers and the engine adapter followed activeState through these
    emit runlistChanged(id);
    ++m_ui.activeState; emit activeStateChanged();
    ++m_ui.activeRunlist; emit activeRunlistChanged();
    ++m_ui.activeChannelData; emit activeChannelDataChanged();
    emit activeProfileChanged();
    emit activeExecutorChanged();
    ++m_ui.activeMeters; emit activeMetersChanged();
    updateActiveMixes();
}

void SupervisorClient::applyChannelChanges(const QHash<QString, int>& changes) {
    if (changes.isEmpty()) return;
    ++m_ui.channels; emit channelsChanged();
    const int bits = changes.value(m_active, 0);
    if (!bits) return;
    ++m_ui.activeChannelData; emit activeChannelDataChanged();
    const int whole = ChannelListModel::Inserted | ChannelListModel::Removed;
    if (bits & (whole | ChannelListModel::Profile)) emit activeProfileChanged();
    if (bits & (whole | ChannelListModel::Executor)) emit activeExecutorChanged();
}

void SupervisorClient::onMeters(const QString& channelId, const QJsonObject& meters) {
    ++m_ui.metersIn;
    CachedJson& m = m_meters[channelId];
    // Silence (or a frozen source) repeats the same levels: nothing to redraw.
    if (m.json == meters && (m_meterLastMs.contains(channelId) || meters.isEmpty())) { ++m_ui.metersSame; return; }
    m.set(meters);
    if (m_meterDue.contains(channelId)) return;   // its slot is booked; the newest value goes out then
    const qint64 now = m_clock.elapsed();
    const auto last = m_meterLastMs.constFind(channelId);
    if (last == m_meterLastMs.cend() || now - *last >= kMeterIntervalMs) { emitMeters(channelId, now); return; }
    m_meterDue.insert(channelId);
    const int wait = int(qMax<qint64>(1, *last + kMeterIntervalMs - now));
    if (!m_meterTimer.isActive() || m_meterTimer.remainingTime() > wait) m_meterTimer.start(wait);
}

void SupervisorClient::onMeterTimer() {
    const qint64 now = m_clock.elapsed();
    QStringList due;
    qint64 next = -1;
    for (const QString& ch : std::as_const(m_meterDue)) {
        const qint64 at = m_meterLastMs.value(ch) + kMeterIntervalMs;
        if (at <= now) due << ch;
        else next = next < 0 ? at - now : qMin(next, at - now);
    }
    for (const QString& ch : std::as_const(due)) m_meterDue.remove(ch);
    if (next >= 0) m_meterTimer.start(int(qMax<qint64>(1, next)));
    for (const QString& ch : std::as_const(due)) emitMeters(ch, now);
}

void SupervisorClient::emitMeters(const QString& channelId, qint64 nowMs) {
    m_meterLastMs[channelId] = nowMs;
    ++m_ui.metersEmitted;
    emit metersChanged(channelId);
    if (channelId != m_active) return;
    updateActiveMixes();
    ++m_ui.activeMeters; emit activeMetersChanged();
}

void SupervisorClient::updateActiveMixes() {
    QJsonArray mixes;
    for (const QJsonValue& v : jsonOf(m_meters, m_active).value(QLatin1String("mixes")).toArray()) {
        const QJsonObject o = v.toObject();
        QJsonObject e; e["id"] = o.value("id"); e["name"] = o.value("name");
        if (o.contains("follow")) e["follow"] = o.value("follow");
        mixes.append(e);
    }
    if (mixes == m_activeMixesJson) return;
    m_activeMixesJson = mixes;
    m_activeMixes = mixes.toVariantList();
    ++m_ui.activeMixes; emit activeMixesChanged();
}

QVariantMap SupervisorClient::uiCounters() const {
    const ChannelListModel::Stats& ms = m_model->stats();
    return QVariantMap{
        {"previews", m_ui.previews}, {"metersIn", m_ui.metersIn}, {"metersSame", m_ui.metersSame}, {"metersEmitted", m_ui.metersEmitted},
        {"activeMetersNotify", m_ui.activeMeters}, {"activeMixesNotify", m_ui.activeMixes}, {"activeStateNotify", m_ui.activeState},
        {"activeRunlistNotify", m_ui.activeRunlist}, {"channelsNotify", m_ui.channels}, {"activeChannelDataNotify", m_ui.activeChannelData},
        {"snapshots", m_ui.snapshots}, {"snapshotsSame", m_ui.snapshotsSame},
        {"readsActiveMeters", m_reads.activeMeters}, {"readsActiveState", m_reads.activeState}, {"readsActiveRunlist", m_reads.activeRunlist},
        {"readsChannels", m_reads.channels}, {"readsActiveChannelData", m_reads.activeChannelData}, {"readsSnapshot", m_reads.snapshot},
        {"modelSyncs", ms.syncs}, {"modelUnchanged", ms.unchanged}, {"modelResets", ms.resets}, {"modelInserted", ms.inserted},
        {"modelRemoved", ms.removed}, {"modelMoved", ms.moved}, {"modelRowUpdates", ms.updated}};
}

QVariantMap SupervisorClient::ipcCounters() const {
    const SupervisorClientWorker::Stats s = ipcStats();
    const quint64 typed = s.snapshotBytes + s.runlistBytes + s.telemetryBytes + s.previewBytes + s.timeSyncBytes;
    return QVariantMap{
        {"received", s.received}, {"bytes", s.bytes}, {"snapshotBytes", s.snapshotBytes}, {"runlistBytes", s.runlistBytes},
        {"telemetryBytes", s.telemetryBytes}, {"previewBytes", s.previewBytes}, {"timeSyncBytes", s.timeSyncBytes},
        {"otherBytes", s.bytes - qMin(s.bytes, typed)}, {"snapshots", s.snapshots}, {"runlists", s.runlists}, {"events", s.events},
        {"timeSyncs", s.timeSyncs}, {"alive", s.alive}, {"coalesced", s.coalesced},
        {"queries", queriesSent()}, {"queriesConnect", m_queries[QueryConnect]}, {"queriesRecover", m_queries[QueryRecover]},
        {"queriesStale", m_queries[QueryStale]}, {"queriesPoll", m_queries[QueryPoll]}, {"queriesOperator", m_queries[QueryOperator]},
        {"statusPollMs", m_opt.statusPollMs}, {"snapshotStaleMs", m_opt.snapshotStaleMs}};
}

void SupervisorClient::logUiStats() {
    // One line per period: notifications and getter reads per second (a read is one binding evaluation or JS call).
    const double s = m_uiStatsLog.interval() / 1000.0;
    const UiStats& a = m_ui; const UiStats& b = m_uiLogged;
    const UiReads& r = m_reads; const UiReads& q = m_readsLogged;
    auto rate = [s](quint64 x, quint64 y) { return QString::number((x - y) / s, 'f', 1); };
    const ChannelListModel::Stats& ms = m_model->stats();
    qInfo().noquote() << QString("[SupervisorClient] ui/s: previews %1, meters in %2 -> activeMeters %3, activeState %4, activeRunlist %5, channels %6, "
                                 "snapshots %7 (same %8); reads/s: activeMeters %9, activeState %10, activeRunlist %11, channels %12, activeChannelData %13; "
                                 "model rows %14, updated in place %15, inserted %16, removed %17, moved %18, resets %19")
                             .arg(rate(a.previews, b.previews), rate(a.metersIn, b.metersIn), rate(a.activeMeters, b.activeMeters), rate(a.activeState, b.activeState),
                                  rate(a.activeRunlist, b.activeRunlist), rate(a.channels, b.channels), rate(a.snapshots, b.snapshots), rate(a.snapshotsSame, b.snapshotsSame),
                                  rate(r.activeMeters, q.activeMeters))
                             .arg(rate(r.activeState, q.activeState), rate(r.activeRunlist, q.activeRunlist), rate(r.channels, q.channels), rate(r.activeChannelData, q.activeChannelData))
                             .arg(m_model->count()).arg(ms.updated).arg(ms.inserted).arg(ms.removed).arg(ms.moved).arg(ms.resets);
    // The link (PA-4): what the Supervisor sent in this period, per minute, by kind; totals since start for long runs.
    const SupervisorClientWorker::Stats n = ipcStats(); const SupervisorClientWorker::Stats& o = m_ipcLogged;
    auto kbMin = [s](quint64 x, quint64 y) { return QString::number((x - y) / 1024.0 * 60.0 / s, 'f', 1); };
    const quint64 typedN = n.snapshotBytes + n.runlistBytes + n.telemetryBytes + n.previewBytes + n.timeSyncBytes;
    const quint64 typedO = o.snapshotBytes + o.runlistBytes + o.telemetryBytes + o.previewBytes + o.timeSyncBytes;
    qInfo().noquote() << QString("[SupervisorClient] ipc in %1 KB/min (snapshots %2, runlists %3, telemetry %4, previews %5, time sync %6, other %7); "
                                 "msgs %8, snapshots %9, runlists %10, events %11; queries %12 in the period; totals: %13 bytes, %14 msgs, queries %15 "
                                 "(connect %16, recover %17, stale %18, poll %19, operator %20)")
                             .arg(kbMin(n.bytes, o.bytes), kbMin(n.snapshotBytes, o.snapshotBytes), kbMin(n.runlistBytes, o.runlistBytes),
                                  kbMin(n.telemetryBytes, o.telemetryBytes), kbMin(n.previewBytes, o.previewBytes), kbMin(n.timeSyncBytes, o.timeSyncBytes),
                                  kbMin(n.bytes - qMin(n.bytes, typedN), o.bytes - qMin(o.bytes, typedO)))
                             .arg(n.received - o.received).arg(n.snapshots - o.snapshots).arg(n.runlists - o.runlists).arg(n.events - o.events).arg(queriesSent() - m_queriesLogged)
                             .arg(n.bytes).arg(n.received).arg(queriesSent())
                             .arg(m_queries[QueryConnect]).arg(m_queries[QueryRecover]).arg(m_queries[QueryStale]).arg(m_queries[QueryPoll]).arg(m_queries[QueryOperator]);
    m_uiLogged = m_ui; m_readsLogged = m_reads; m_ipcLogged = n; m_queriesLogged = queriesSent();
}

void SupervisorClient::drainInbox() {
    // Short slices: after a busy spell the worker may hold many events; the GUI keeps painting in between.
    if (!m_worker) return;
    QElapsedTimer t; t.start();
    SupervisorClientWorker::Delivery d;
    while (m_worker && m_worker->takeNext(&d)) {
        apply(d);
        if (t.elapsed() >= kGuiSliceMs) { QMetaObject::invokeMethod(this, &SupervisorClient::drainInbox, Qt::QueuedConnection); return; }
    }
}

void SupervisorClient::apply(const SupervisorClientWorker::Delivery& d) {
    using namespace IpcProtocol;
    const Message& msg = d.msg;
    ++m_applied;
    switch (msg.type) {
    case MSG_UI_STATUS:
        if (msg.channelId.isEmpty()) {
            m_lastSnapshotMs = m_clock.elapsed();
            if (msg.payload == m_snapshot) ++m_ui.snapshotsSame;   // a keepalive or a query's answer repeats it: nothing to tell QML
            else {
                m_snapshot = msg.payload; m_snapshotMapValid = false; m_snapshotMap.clear();
                ++m_ui.snapshots;
                applyChannelChanges(m_model->sync(msg.payload.value("channels").toArray()));   // rows updated in place
                emit snapshotChanged();
            }
            if (m_alerts.isEmpty()) { const QJsonArray a = msg.payload.value("alerts").toArray(); for (int i = a.size() - 1; i >= 0; --i) m_alerts.prepend(a[i].toObject().toVariantMap()); if (!a.isEmpty()) emit alertsChanged(); }
        }
        else {
            // Single-channel update: merge into the snapshot's channel list (and the model's row).
            const int bits = m_model->upsert(msg.payload);
            if (!bits) break;   // the same entry again
            QJsonArray chans = m_snapshot.value("channels").toArray();
            bool replaced = false;
            for (int i = 0; i < chans.size(); ++i) {
                if (chans[i].toObject().value("profile").toObject().value("id").toString() == msg.channelId) { chans[i] = msg.payload; replaced = true; }
            }
            if (!replaced) chans.append(msg.payload);
            m_snapshot["channels"] = chans; m_snapshotMapValid = false; m_snapshotMap.clear();
            applyChannelChanges({{ChannelListModel::idOf(msg.payload), bits}});
            emit snapshotChanged();
        }
        break;
    case MSG_UI_TELEMETRY:
        if (msg.payload.contains("state") && msg.payload.contains("reason") && !msg.payload.contains("frame")) {
            emit stateChanged(msg.channelId, msg.payload.value("state").toString(), msg.payload.value("reason").toString());
        } else {
            m_telemetry[msg.channelId].set(msg.payload);
            emit telemetryChanged(msg.channelId);
            if (msg.channelId == m_active) { ++m_ui.activeState; emit activeStateChanged(); }
        }
        break;
    case MSG_UI_RUNLIST:
        m_runlists[msg.channelId].set(msg.payload);
        emit runlistChanged(msg.channelId);
        if (msg.channelId == m_active) { ++m_ui.activeRunlist; emit activeRunlistChanged(); }
        break;
    case MSG_UI_ASRUN:
        m_asRun.prepend(msg.payload.toVariantMap()); while (m_asRun.size() > 300) m_asRun.removeLast();
        emit asRun(msg.channelId, msg.payload); emit asRunChanged();
        break;
    case MSG_UI_ALERT:
        m_alerts.prepend(msg.payload.toVariantMap()); while (m_alerts.size() > 300) m_alerts.removeLast();
        emit alert(msg.channelId, msg.payload.value("level").toString(), msg.payload.value("message").toString()); emit alertsChanged();
        break;
    case MSG_UI_SCHEDULE: {
        const QString t = msg.payload.value("_type").toString();
        if (t == "mediaScanDone" || msg.payload.value("done").toBool()) emit mediaScanDone();
        else if (t == "transcriptReady") emit transcriptReady(msg.payload.value("mediaId").toString());
        else if (t.isEmpty() || t == "schedule") emit scheduleChanged(msg.channelId, msg.payload.value("slotId").toString());
        if (!t.isEmpty()) emit uiEvent(t, msg.payload);
        break;
    }
    case MSG_PREVIEW_FRAME:
        // The JPEG bytes (d.jpeg, either wire form) are kept as they are: the image provider decodes them at tile size
        // when a tile asks (PA-2). The payload no longer carries the picture. Meters ride along at the preview rate;
        // they reach QML through metersChanged / activeMetersChanged (<= 15 Hz), not previewChanged.
        if (msg.payload.contains("meters")) onMeters(msg.channelId, msg.payload.value("meters").toObject());
        if (!d.jpeg.isEmpty()) {
            { QMutexLocker lk(&m_previewMu); m_previewJpegs[msg.channelId] = d.jpeg; }
            ++m_previewSerial; ++m_ui.previews; emit previewChanged(msg.channelId);
        }
        break;
    default: break;
    }
}

QImage SupervisorClient::preview(const QString& channelId) const {
    const QByteArray jpeg = previewJpeg(channelId);
    QImage img;
    if (!jpeg.isEmpty()) img.loadFromData(jpeg, "JPEG");
    return img;
}

void SupervisorClient::command(const QString& action, const QVariantMap& params, const QString& channelId) {
    if (!m_connected || !m_worker) { qWarning() << "[SupervisorClient] not connected; dropped" << action; return; }
    QJsonObject p; p["action"] = action; p["params"] = QJsonObject::fromVariantMap(params);
    const QString ch = channelId.isEmpty() ? m_active : channelId;
    p["channelId"] = ch;
    // Operator commands still go out while the Supervisor is stalled: they wait in the pipe and run when it recovers.
    sendToWorker(IpcProtocol::make(IpcProtocol::MSG_UI_COMMAND, ch, p));
}

void SupervisorClient::take(const QString& path, const QString& title, double duration, const QString& channelId) {
    QVariantMap p; p["path"] = path; p["title"] = title.isEmpty() ? QFileInfo(path).completeBaseName() : title;
    p["id"] = "take-" + QString::number(QDateTime::currentMSecsSinceEpoch());
    if (duration > 0) p["duration"] = duration;
    command("take", p, channelId);
}

void SupervisorClient::arm(bool armed, const QString& channelId) {
    QVariantMap p; p["armed"] = armed;
    command("arm", p, channelId);
}

void SupervisorClient::seek(double seconds, const QString& channelId) {
    QVariantMap p; p["seconds"] = seconds;
    command("seek", p, channelId);
}
