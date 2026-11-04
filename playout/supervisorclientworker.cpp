#include "supervisorclientworker.h"
#include "ui_ipc/ipctransport.h"
#include <QDebug>
#include <QJsonDocument>
#include <QTimer>

SupervisorClientWorker::SupervisorClientWorker(QObject* parent) : QObject(parent) { m_clock.start(); }

SupervisorClientWorker::~SupervisorClientWorker() { shutdown(); }

void SupervisorClientWorker::connectTo(const QString& ipcName, int timeoutMs) {
    if (!m_ipc) {
        m_ipc = new IpcTransport(IpcTransport::ClientRole, this);
        // PA-2: read msg.binary (liftPreview takes either form), so the Supervisor may skip the base64. A pre-PS-5
        // Supervisor ignores the caps message and keeps sending JSON, which the same code path reads.
        m_ipc->setAcceptBinary(m_acceptBinary);
        connect(m_ipc, &IpcTransport::messageReceived, this, [this](const IpcProtocol::Message& m, const QString&) { onMessage(m); });
        connect(m_ipc, &IpcTransport::connectionStateChanged, this, &SupervisorClientWorker::onTransportState);
        connect(m_ipc, &IpcTransport::connectFailed, this, &SupervisorClientWorker::onConnectFailed);
    }
    if (!m_stallTimer) {
        m_stallTimer = new QTimer(this);
        m_stallTimer->setInterval(200);   // detection lands within 200 ms of the threshold
        connect(m_stallTimer, &QTimer::timeout, this, &SupervisorClientWorker::checkStall);
    }
    if (m_ipc->isConnected() || m_ipc->isConnecting()) return;
    m_ipc->connectToServerAsync(ipcName, timeoutMs);
}

void SupervisorClientWorker::setAcceptBinary(bool on) {
    m_acceptBinary = on;
    if (m_ipc) m_ipc->setAcceptBinary(on);   // re-announced to a connected Supervisor
}

void SupervisorClientWorker::send(const IpcProtocol::Message& msg) {
    if (m_ipc) m_ipc->send(msg);   // the GUI only sends while connected; the transport refuses (and logs caps) otherwise
}

void SupervisorClientWorker::shutdown() {
    if (m_stallTimer) m_stallTimer->stop();
    if (m_ipc) {
        IpcTransport* t = m_ipc; m_ipc = nullptr;
        QObject::disconnect(t, nullptr, this, nullptr);   // nothing it reports while closing reaches us
        t->disconnect();                                  // IpcTransport::disconnect: cancels a pending connect, closes the pipe
        delete t;
    }
    m_up = false; m_busyStall = false;
    m_lastRxMs.store(-1); m_stalled.store(false);
    QMutexLocker lk(&m_mu);
    m_inbox.clear(); m_keySeq.clear(); m_live = 0; m_posted = false;
}

void SupervisorClientWorker::onTransportState(bool up) {
    if (up == m_up) return;   // the transport reports a close from stateChanged and from disconnected
    m_up = up;
    if (up) {
        m_lastRxMs.store(m_clock.elapsed());   // the stall window starts at the connect
        setStalled(false, 0, false);
        if (m_stallTimer) m_stallTimer->start();
    } else {
        if (m_stallTimer) m_stallTimer->stop();
        m_lastRxMs.store(-1);
        setStalled(false, -1, false);   // a closed pipe is "disconnected", not "stalled"
    }
    emit connectionStateChanged(up);
}

void SupervisorClientWorker::onConnectFailed(const QString& reason) {
    // connectToServerAsync says "busy" when the pipe exists but no instance was free for the whole budget: Qt's
    // QLocalServer only makes the next instance from its event loop, so a hung Supervisor looks exactly like this.
    const bool busy = reason.contains(QLatin1String("busy"));
    if (busy) setStalled(true, -1, true);
    else if (m_busyStall) setStalled(false, -1, false);   // gone or another error: absent, no longer "hung"
    emit connectFailed(reason, busy);
}

void SupervisorClientWorker::setStalled(bool on, qint64 silent, bool busy) {
    if (m_stalled.load() == on && (!on || m_busyStall == busy)) return;
    m_stalled.store(on);
    m_busyStall = on && busy;
    emit stalledChanged(on, silent, on && busy);
}

void SupervisorClientWorker::checkStall() {
    const qint64 last = m_lastRxMs.load();
    if (last < 0 || m_stalled.load()) return;
    const qint64 silent = m_clock.elapsed() - last;
    if (silent < m_stallMs.load()) return;
    qWarning().noquote() << QString("[SupervisorClient] no message from the Supervisor for %1 ms with the pipe connected: marking it stalled").arg(silent);
    setStalled(true, silent, false);
}

qint64 SupervisorClientWorker::wireBytes(const IpcProtocol::Message& msg, qsizetype extraBase64) {
    qint64 n = 4 + msg.toJson().size();                     // the length header + what the Supervisor serialized
    if (extraBase64 >= 0) n += qint64(extraBase64) + 16;    // ,"jpegBase64":"<base64>" (base64 needs no escaping)
    return n;
}

qint64 SupervisorClientWorker::binaryWireBytes(const IpcProtocol::Message& msg, const QString& binaryKey, qsizetype jpegLen) {
    // IpcProtocol::encodeFrame(msg, true): u32 header | u8 version | u32 headLen | head | bytes, where the head is the
    // compact JSON of {type, channelId, timestampNs, payload (without the key), binaryKey} in QJsonObject's sorted order:
    // exactly toJson() of the picture-less message plus the "binaryKey" member (and its comma) before "channelId".
    const QByteArray keyLit = QJsonDocument(QJsonArray{binaryKey}).toJson(QJsonDocument::Compact);   // ["..."] escaped as JSON does
    return 4 + 5 + msg.toJson().size() + (keyLit.size() - 2) + 13 + qint64(jpegLen);
}

void SupervisorClientWorker::onMessage(const IpcProtocol::Message& msg) {
    using namespace IpcProtocol;
    const qint64 now = m_clock.elapsed();
    const qint64 last = m_lastRxMs.exchange(now);
    if (m_stalled.load() && !m_busyStall) {
        const qint64 silent = last >= 0 ? now - last : -1;
        qInfo().noquote() << QString("[SupervisorClient] the Supervisor answers again after %1 ms of silence").arg(silent);
        setStalled(false, silent, false);
    }
    // Time sync and the liveness broadcast only say "alive" to the app (engines take the NTP offset from time sync):
    // they never reach the GUI thread (50 wake-ups a second against a Supervisor from before PS-1).
    const bool aliveOnly = msg.type == MSG_TIME_SYNC || (msg.type == MSG_SUP_ALIVE && msg.channelId.isEmpty());
    if (aliveOnly) {
        const qint64 n = wireBytes(msg);
        QMutexLocker lk(&m_mu);
        ++m_stats.received; m_stats.bytes += quint64(n);
        if (msg.type == MSG_TIME_SYNC) { ++m_stats.timeSyncs; m_stats.timeSyncBytes += quint64(n); } else ++m_stats.alive;
        return;
    }

    Delivery d;
    d.msg = msg;
    QString key;
    qint64 bytes = 0;
    if (msg.type == MSG_PREVIEW_FRAME) {
        // The JPEG stays bytes: the image provider decodes it at tile size on its loader thread (the decode used to run
        // here for every frame of every source, and before PA-1 on the GUI thread). Either wire form lands in d.jpeg.
        const bool wasBinary = !msg.binary.isEmpty();
        const bool had = wasBinary || d.msg.payload.contains(QLatin1String("jpegBase64"));
        d.jpeg = liftPreview(d.msg);   // JSON form: base64 decoded once, key removed; binary form: the bytes as they came
        const QString binaryKey = d.msg.binaryKey.isEmpty() ? QStringLiteral("jpegBase64") : d.msg.binaryKey;
        d.msg.binary.clear(); d.msg.binaryKey.clear();   // the GUI's copy of the message carries no picture
        // The picture by its length (no second pass over ~100 KB): as base64 in the JSON form, raw in the binary one.
        bytes = wasBinary ? binaryWireBytes(d.msg, binaryKey, d.jpeg.size()) : wireBytes(d.msg, had ? base64Length(d.jpeg.size()) : -1);
        if (!looksLikeJpeg(d.jpeg)) {
            d.jpeg.clear();   // a meters-only frame carries no picture (not counted); one that carried something else is bad
            if (had) { QMutexLocker lk(&m_mu); ++m_stats.badPreviews; }
        } else {
            key = coalesceKey(msg);   // only a picture replaces a picture; a frame without one keeps its meters in order
            if (wasBinary) { QMutexLocker lk(&m_mu); ++m_stats.binaryPreviews; }
        }
    } else {
        bytes = wireBytes(msg);
        key = coalesceKey(msg);
    }
    {
        QMutexLocker lk(&m_mu);
        ++m_stats.received; m_stats.bytes += quint64(bytes);
        switch (msg.type) {
        case MSG_PREVIEW_FRAME: m_stats.previewBytes += quint64(bytes); break;
        case MSG_UI_TELEMETRY: m_stats.telemetryBytes += quint64(bytes); break;
        case MSG_UI_RUNLIST: m_stats.runlistBytes += quint64(bytes); ++m_stats.runlists; break;
        case MSG_UI_STATUS: m_stats.snapshotBytes += quint64(bytes); if (msg.channelId.isEmpty()) ++m_stats.snapshots; break;
        case MSG_UI_SCHEDULE: ++m_stats.events; break;
        default: break;
        }
    }
    push(std::move(d), key);
}

QString SupervisorClientWorker::coalesceKey(const IpcProtocol::Message& m) {
    using namespace IpcProtocol;
    switch (m.type) {
    case MSG_PREVIEW_FRAME: return QLatin1String("pv:") + m.channelId;
    case MSG_UI_TELEMETRY:
        // {state, reason} without a frame is a state-change event (SupervisorClient::stateChanged): every one is kept.
        if (m.payload.contains(QLatin1String("state")) && m.payload.contains(QLatin1String("reason")) && !m.payload.contains(QLatin1String("frame"))) return {};
        return QLatin1String("tel:") + m.channelId;
    case MSG_UI_STATUS: return m.channelId.isEmpty() ? QStringLiteral("snap") : QLatin1String("st:") + m.channelId;
    case MSG_UI_RUNLIST: return QLatin1String("rl:") + m.channelId;
    default: return {};   // alerts, as-run, schedule events, unknown types
    }
}

void SupervisorClientWorker::push(Delivery&& d, const QString& key) {
    bool post = false;
    quint64 overflowNow = 0;
    {
        QMutexLocker lk(&m_mu);
        // A newer state message retires the queued one and goes to the back: it is applied after everything that
        // arrived before it, as it would have been without coalescing.
        if (!key.isEmpty() && !m_inbox.empty()) {
            const auto it = m_keySeq.constFind(key);
            const quint64 head = m_inbox.front().seq;
            if (it != m_keySeq.constEnd() && *it >= head) {
                Entry& old = m_inbox[size_t(*it - head)];
                if (!old.dead) { old.dead = true; old.d = Delivery(); --m_live; ++m_stats.coalesced; }
            }
        }
        Entry e;
        e.d = std::move(d);
        e.key = key;
        e.seq = m_nextSeq++;
        if (!key.isEmpty()) m_keySeq.insert(key, e.seq);
        m_inbox.push_back(std::move(e));
        ++m_live; ++m_stats.queued;
        // Bound: only events pile up (state is coalesced), so this takes a GUI thread stuck for many minutes.
        while (m_live > kMaxInbox && !m_inbox.empty()) {
            Entry& f = m_inbox.front();
            if (!f.dead) { --m_live; ++m_stats.overflowDropped; ++overflowNow; }
            if (!f.key.isEmpty()) { const auto kit = m_keySeq.find(f.key); if (kit != m_keySeq.end() && *kit == f.seq) m_keySeq.erase(kit); }
            m_inbox.pop_front();
        }
        if (m_inbox.size() > size_t(2 * m_live + 1024)) compactLocked();   // retired entries are tiny; still, keep it bounded
        m_stats.inboxPeak = qMax(m_stats.inboxPeak, m_live);
        if (!m_posted) { m_posted = true; post = true; }
    }
    if (overflowNow && (m_stats.overflowDropped == overflowNow || m_stats.overflowDropped % 1000 == 0))
        qWarning().noquote() << QString("[SupervisorClient] GUI thread is not taking IPC messages: dropped the oldest (%1 so far)").arg(m_stats.overflowDropped);
    if (post) emit inboxReady();
}

void SupervisorClientWorker::compactLocked() {
    std::deque<Entry> live;
    m_keySeq.clear();
    quint64 seq = m_inbox.empty() ? m_nextSeq : m_inbox.front().seq;
    for (Entry& e : m_inbox) {
        if (e.dead) continue;
        e.seq = seq++;
        if (!e.key.isEmpty()) m_keySeq.insert(e.key, e.seq);
        live.push_back(std::move(e));
    }
    m_inbox.swap(live);
    m_nextSeq = seq;
}

bool SupervisorClientWorker::takeNext(Delivery* out) {
    QMutexLocker lk(&m_mu);
    while (!m_inbox.empty()) {
        Entry e = std::move(m_inbox.front());
        m_inbox.pop_front();
        if (!e.key.isEmpty()) { const auto it = m_keySeq.find(e.key); if (it != m_keySeq.end() && *it == e.seq) m_keySeq.erase(it); }
        if (e.dead) continue;
        --m_live;
        *out = std::move(e.d);
        return true;
    }
    m_posted = false;   // seen empty: the next arrival posts inboxReady again
    return false;
}

int SupervisorClientWorker::inboxSize() const { QMutexLocker lk(&m_mu); return m_live; }

qint64 SupervisorClientWorker::silentMs() const {
    const qint64 last = m_lastRxMs.load();
    return last < 0 ? -1 : m_clock.elapsed() - last;
}

SupervisorClientWorker::Stats SupervisorClientWorker::stats() const { QMutexLocker lk(&m_mu); return m_stats; }
