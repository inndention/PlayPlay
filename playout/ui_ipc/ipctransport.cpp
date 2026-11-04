#include "ipctransport.h"
#include <QDebug>
#include <QDeadlineTimer>
#include <QJsonArray>
#include <QMutex>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstring>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// One async connect attempt, shared by the worker that opens the pipe and the transport that adopts it. Whoever sees
// the other side finished closes an unadopted handle, so a cancel (disconnect, a newer connect, the transport's
// destructor) never leaks a pipe instance and the worker never touches the transport.
struct IpcTransport::PendingConnect {
    std::atomic<bool> cancelled{false};
    QMutex mu;
    bool done = false;     // guarded by mu
    quintptr handle = 0;   // guarded by mu: the opened pipe until adopted or closed
    QString error;         // guarded by mu
};

namespace {
#ifdef Q_OS_WIN
void closePipeHandle(quintptr h) { if (h) CloseHandle(reinterpret_cast<HANDLE>(h)); }

// Opens the client end of a named pipe the way QLocalSocket does (overlapped, read/write), but on a worker thread and
// with a bounded wait: while every pipe instance is busy (the server's event loop is not accepting), Qt's own
// connectToServer sits in WaitNamedPipe for 5 s at a time on the caller's thread.
quintptr openPipe(const QString& path, int budgetMs, const std::atomic<bool>& cancelled, QString* err) {
    const std::wstring wpath = path.toStdWString();
    const QDeadlineTimer deadline(qMax(0, budgetMs));
    for (;;) {
        if (cancelled.load()) { *err = QStringLiteral("cancelled"); return 0; }
        HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) return reinterpret_cast<quintptr>(h);
        const DWORD e = GetLastError();
        if (e != ERROR_PIPE_BUSY) {
            *err = (e == ERROR_FILE_NOT_FOUND) ? QStringLiteral("server not found") : QString("cannot open the pipe (Windows error %1)").arg(e);
            return 0;
        }
        const qint64 left = deadline.remainingTime();
        if (left <= 0) {
            *err = QString("timed out after %1 ms: every pipe instance is busy (the server is not accepting connections)").arg(budgetMs);
            return 0;
        }
        // Short slices so a cancel is noticed quickly; returns early when an instance frees up. Never 0 (= server default).
        WaitNamedPipeW(wpath.c_str(), DWORD(qBound(qint64(1), left, qint64(200))));
    }
}
#endif

QString peerLabel(const QString& id) { return id == QLatin1String("server") ? QStringLiteral("server") : QStringLiteral("client ") + id; }
}

IpcTransport::IpcTransport(Role role, QObject* parent)
    : QObject(parent)
    , m_role(role)
    , m_server(nullptr)
    , m_clientSocket(nullptr)
    , m_caps(defaultCaps(role))
    , m_acceptBinary(role == ServerRole)   // the Supervisor relays msg.binary; the app still reads payload["jpegBase64"]
{
    m_clock.start();
}

IpcTransport::Caps IpcTransport::defaultCaps(Role role) {
    Caps c;
    // A client does not cut its own link by default: past the soft cap it only queues small Normal messages (heartbeats,
    // as-run, state changes, operator commands). Engines reconnect since PS-3 and set a hard cap on their link
    // (SupervisorLink), so a frozen Supervisor cannot grow an engine without bound.
    if (role == ClientRole) c.hardBytes = 0;
    return c;
}

void IpcTransport::setReadBufferBytes(qint64 n) {
    m_readBufferBytes = n > 0 ? qMax<qint64>(n, 4096) : kDefaultReadBufferBytes;   // never 0: that is Qt's "unlimited"
    for (QLocalSocket* s : std::as_const(m_clients)) s->setReadBufferSize(m_readBufferBytes);
    if (m_clientSocket) m_clientSocket->setReadBufferSize(m_readBufferBytes);
}

bool IpcTransport::setPeerCaps(const QString& clientId, const Caps& caps) {
    QLocalSocket* s = m_clients.value(clientId, nullptr);
    auto pit = s ? m_peers.find(s) : m_peers.end();
    if (pit == m_peers.end()) return false;
    pit->hasCaps = true;
    pit->caps = caps;
    return true;
}

IpcTransport::~IpcTransport() {
    disconnect();
}

IpcTransport::Priority IpcTransport::defaultPriority(IpcProtocol::MessageType type) {
    switch (type) {
    case IpcProtocol::MSG_PREVIEW_FRAME:   // the next picture replaces it
    case IpcProtocol::MSG_UI_TELEMETRY:    // engine stateJson: the next one (1 Hz) carries the same fields
    case IpcProtocol::MSG_TIME_SYNC:       // engines only take the NTP offset from it
    case IpcProtocol::MSG_SUP_ALIVE:       // 1 Hz liveness / adoption confirmation: the next one says the same (PS-3)
        return Droppable;
    default:
        return Normal;
    }
}

IpcTransport::Verdict IpcTransport::decide(qint64 queuedBytes, Priority p, const Caps& caps) {
    if (caps.hardBytes > 0 && queuedBytes >= caps.hardBytes) return Verdict::Disconnect;
    if (p == Droppable && caps.softBytes > 0 && queuedBytes >= caps.softBytes) return Verdict::Drop;
    return Verdict::Send;
}

bool IpcTransport::startServer(const QString& serverName) {
    if (m_role != ServerRole) return false;
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, &IpcTransport::onNewConnection);

    QLocalServer::removeServer(serverName);
    if (!m_server->listen(serverName)) {
        qWarning() << "[IpcTransport] Failed to start server:" << m_server->errorString();
        return false;
    }
    qDebug() << "[IpcTransport] Server listening on" << serverName;
    return true;
}

void IpcTransport::wireClientSocket(QLocalSocket* s) {
    Peer peer; peer.id = QStringLiteral("server");
    m_peers.insert(s, peer);
    connect(s, &QLocalSocket::readyRead, this, &IpcTransport::onSocketReadyRead);
    connect(s, &QLocalSocket::stateChanged, this, &IpcTransport::onSocketStateChanged);
    connect(s, &QLocalSocket::disconnected, this, [this, s]() {
        const QPointer<IpcTransport> self(this);
        processBuffer(s, true);   // what the server sent before it left (normally handed out already)
        if (self) emit connectionStateChanged(false);
    });
}

void IpcTransport::dropClientSocket() {
    if (!m_clientSocket) return;
    QLocalSocket* s = m_clientSocket;
    m_clientSocket = nullptr;
    m_peers.remove(s);
    QObject::disconnect(s, nullptr, this, nullptr);   // a replaced socket reports nothing any more
    s->abort();
    s->deleteLater();
}

bool IpcTransport::connectToServer(const QString& serverName) {
    if (m_role != ClientRole) return false;
    cancelPendingConnect();
    dropClientSocket();
    m_clientSocket = new QLocalSocket(this);
    m_clientSocket->setReadBufferSize(m_readBufferBytes);   // before the pipe opens: Qt starts reading at once
    wireClientSocket(m_clientSocket);

    m_clientSocket->connectToServer(serverName);
    return m_clientSocket->waitForConnected(5000);
}

void IpcTransport::connectToServerAsync(const QString& serverName, int timeoutMs) {
    if (m_role != ClientRole) { emit connectFailed(QStringLiteral("not a client transport")); return; }
    cancelPendingConnect();
    dropClientSocket();
    auto st = std::make_shared<PendingConnect>();
    m_pending = st;
#ifdef Q_OS_WIN
    const QString pipePrefix = QStringLiteral("\\\\.\\pipe\\");
    const QString path = serverName.startsWith(pipePrefix) ? serverName : pipePrefix + serverName;   // QLocalServer's naming
    QThread* t = QThread::create([st, path, timeoutMs]() {
        QString err;
        const quintptr h = openPipe(path, timeoutMs, st->cancelled, &err);
        QMutexLocker lk(&st->mu);
        st->done = true;
        if (st->cancelled.load()) { closePipeHandle(h); return; }
        st->handle = h;
        st->error = err;
    });
    t->setObjectName(QStringLiteral("ipc-connect"));
    connect(t, &QThread::finished, this, [this, st]() { finishAsyncConnect(st); });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
#else
    // Unix domain sockets connect without blocking; bound the attempt with a timer.
    auto* s = new QLocalSocket(this);
    s->setReadBufferSize(m_readBufferBytes);
    m_clientSocket = s;
    wireClientSocket(s);
    connect(s, &QLocalSocket::connected, this, [this, st]() { if (m_pending == st) m_pending.reset(); });
    connect(s, &QLocalSocket::errorOccurred, this, [this, s, st](QLocalSocket::LocalSocketError) {
        if (m_pending != st) return;
        m_pending.reset();
        emit connectFailed(s->errorString());
    });
    QTimer::singleShot(qMax(0, timeoutMs), s, [this, s, st]() {
        if (m_pending != st) return;
        m_pending.reset();
        s->abort();
        emit connectFailed(QStringLiteral("timed out"));
    });
    s->connectToServer(serverName);
#endif
}

void IpcTransport::cancelPendingConnect() {
    if (!m_pending) return;
    std::shared_ptr<PendingConnect> st = std::move(m_pending);
    m_pending.reset();
    st->cancelled.store(true);
    QMutexLocker lk(&st->mu);
#ifdef Q_OS_WIN
    if (st->done && st->handle) { closePipeHandle(st->handle); st->handle = 0; }
#endif
}

void IpcTransport::finishAsyncConnect(const std::shared_ptr<PendingConnect>& st) {
#ifdef Q_OS_WIN
    if (st != m_pending) return;   // cancelled or superseded: the cancel closed (or the worker will close) the handle
    m_pending.reset();
    quintptr h = 0; QString err;
    { QMutexLocker lk(&st->mu); h = st->handle; st->handle = 0; err = st->error; }
    if (!h) { emit connectFailed(err.isEmpty() ? QStringLiteral("connect failed") : err); return; }
    auto* s = new QLocalSocket(this);
    s->setReadBufferSize(m_readBufferBytes);   // before adoption, which starts Qt's background reads
    if (!s->setSocketDescriptor(qintptr(h), QLocalSocket::ConnectedState, QIODevice::ReadWrite)) {
        // (Qt's Windows implementation always adopts; if it ever refuses, the socket may still own the handle, so it
        // is left to the socket rather than risk closing it twice.)
        delete s;
        emit connectFailed(QStringLiteral("cannot adopt the pipe handle"));
        return;
    }
    m_clientSocket = s;
    wireClientSocket(s);   // after adoption: its own stateChanged(Connected) is not reported twice
    sendCaps(s);           // first on the connection, before the owner hears about it (PS-5)
    emit connectionStateChanged(true);
#else
    Q_UNUSED(st);
#endif
}

void IpcTransport::disconnect() {
    cancelPendingConnect();
    if (m_clientSocket) {
        QLocalSocket* s = m_clientSocket;
        m_clientSocket = nullptr;
        m_peers.remove(s);
        s->disconnectFromServer();
        s->deleteLater();
    }
    if (m_server) {
        m_server->close();
        // Iterate a copy: disconnectFromServer can report the disconnect synchronously, and that slot edits m_clients.
        // Peers go first, so such a report hands out nothing more while we tear down.
        const QMap<QString, QLocalSocket*> clients = m_clients;
        m_peers.clear();
        for (auto* s : clients) {
            s->disconnectFromServer();
            s->deleteLater();
        }
        m_clients.clear();
        m_peers.clear();
        m_server->deleteLater();
        m_server = nullptr;
    }
}

bool IpcTransport::isConnected() const {
    if (m_role == ClientRole && m_clientSocket) {
        if (m_peers.value(m_clientSocket).doomed) return false;
        return m_clientSocket->state() == QLocalSocket::ConnectedState;
    }
    return false;
}

qint64 IpcTransport::queuedBytes() const {
    return m_clientSocket ? m_clientSocket->bytesToWrite() : 0;
}

qint64 IpcTransport::queuedBytes(const QString& clientId) const {
    QLocalSocket* s = m_clients.value(clientId, nullptr);
    return s ? s->bytesToWrite() : -1;
}

qint64 IpcTransport::peerProcessId(const QString& clientId) const {
#ifdef Q_OS_WIN
    const QLocalSocket* s = m_role == ClientRole ? m_clientSocket : m_clients.value(clientId, nullptr);
    if (!s || s->state() != QLocalSocket::ConnectedState) return 0;
    const HANDLE h = reinterpret_cast<HANDLE>(s->socketDescriptor());   // the pipe handle on Windows
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    ULONG pid = 0;
    const BOOL ok = m_role == ClientRole ? GetNamedPipeServerProcessId(h, &pid) : GetNamedPipeClientProcessId(h, &pid);
    return ok ? qint64(pid) : 0;
#else
    Q_UNUSED(clientId);
    return 0;
#endif
}

const QByteArray& IpcTransport::Framer::get(bool binary) {
    QByteArray& slot = binary ? bin : json;
    if (slot.isEmpty()) slot = IpcProtocol::encodeFrame(msg, binary);
    return slot;
}

bool IpcTransport::previewPass(const PreviewSub& sub, const QString& key, qint64 nowMs, qint64 lastMs) {
    if (!sub.all) {
        bool hit = false;
        for (const QString& k : sub.keys) {
            if (k.endsWith(QLatin1Char('*')) ? key.startsWith(QStringView(k).chopped(1)) : key == k) { hit = true; break; }
        }
        if (!hit) return false;
    }
    if (sub.maxFps > 0 && lastMs >= 0 && double(nowMs - lastMs) < 1000.0 / sub.maxFps * 0.85) return false;
    return true;
}

void IpcTransport::setAcceptBinary(bool on) {
    if (m_acceptBinary == on) return;
    m_acceptBinary = on;
    if (m_role == ClientRole) { if (m_clientSocket && isConnected()) sendCaps(m_clientSocket); return; }
    for (QLocalSocket* s : std::as_const(m_clients)) if (m_peers.value(s).capsSeen) sendCaps(s);
}

bool IpcTransport::peerAcceptsBinary(const QString& clientId) const {
    const QLocalSocket* s = m_role == ClientRole ? m_clientSocket : m_clients.value(clientId, nullptr);
    return s && m_peers.value(s).peerBinary >= IpcProtocol::kBinaryFrameVersion;
}

void IpcTransport::sendCaps(QLocalSocket* socket) {
    const IpcProtocol::Message m = IpcProtocol::makeIpcCaps(m_acceptBinary ? IpcProtocol::kBinaryFrameVersion : 0);
    Framer f{m};
    writeFramed(socket, f.get(false), Normal);
}

bool IpcTransport::subscribePreviews(bool all, const QStringList& keys, double maxFps) {
    if (m_role != ClientRole || !m_clientSocket || !isConnected()) return false;
    const IpcProtocol::Message m = IpcProtocol::makePreviewSubscribe(all, keys, maxFps);
    Framer f{m};
    return writeFramed(m_clientSocket, f.get(false), Normal);
}

bool IpcTransport::writeMessage(QLocalSocket* socket, Framer& f, Priority p) {
    auto pit = m_peers.find(socket);
    if (pit == m_peers.end() || pit->doomed) return false;
    const IpcProtocol::Message& msg = f.msg;
    qint64 now = -1;
    if (m_role == ServerRole && pit->hasSub && msg.type == IpcProtocol::MSG_PREVIEW_FRAME) {
        now = m_clock.elapsed();
        if (!previewPass(pit->sub, msg.channelId, now, pit->subLastMs.value(msg.channelId, -1))) { ++m_subscriptionSkips; return false; }
    }
    const bool bin = !msg.binary.isEmpty() && pit->peerBinary >= IpcProtocol::kBinaryFrameVersion;
    const QByteArray& framed = f.get(bin);
    if (!fitsFrame(framed, msg.type)) return false;
    if (!writeFramed(socket, framed, p)) return false;
    pit = m_peers.find(socket);   // writeFramed never removes a peer, but look again rather than trust the iterator
    if (pit != m_peers.end() && now >= 0) pit->subLastMs.insert(msg.channelId, now);
    if (!msg.binary.isEmpty()) { if (bin) ++m_binarySent; else ++m_binaryFallbacks; }
    return true;
}

// Transport control messages (PS-5) are handled here and never reach the owner.
bool IpcTransport::handleControl(QLocalSocket* socket, const IpcProtocol::Message& msg) {
    using namespace IpcProtocol;
    if (msg.type != MSG_IPC_CAPS && !(m_role == ServerRole && msg.type == MSG_UI_PREVIEW_SUBSCRIBE)) return false;
    auto pit = m_peers.find(socket);
    if (pit == m_peers.end()) return true;
    if (msg.type == MSG_IPC_CAPS) {
        const bool first = !pit->capsSeen;
        pit->capsSeen = true;
        pit->peerBinary = qMax(0, msg.payload.value(QLatin1String("binaryFrames")).toInt(0));
        if (m_role == ServerRole && first) sendCaps(socket);   // answer once: only peers that speak caps hear ours
        return true;
    }
    PreviewSub sub;
    sub.all = msg.payload.value(QLatin1String("all")).toBool(false);
    const QJsonArray keys = msg.payload.value(QLatin1String("keys")).toArray();
    for (const QJsonValue& v : keys) if (v.isString()) sub.keys << v.toString();
    sub.maxFps = qMax(0.0, msg.payload.value(QLatin1String("maxFps")).toDouble(0));
    pit->hasSub = true;
    pit->sub = sub;
    pit->subLastMs.clear();
    return true;
}

// A frame the receiver would reject (it closes the connection on it) is not sent at all.
bool IpcTransport::fitsFrame(const QByteArray& framed, IpcProtocol::MessageType type) const {
    if (framed.size() - 4 <= m_maxMessageBytes) return true;
    qWarning().noquote() << QString("[IpcTransport] not sending message type %1: %2 bytes is over the %3-byte frame limit")
                                .arg(int(type)).arg(framed.size() - 4).arg(m_maxMessageBytes);
    return false;
}

bool IpcTransport::writeFramed(QLocalSocket* socket, const QByteArray& framed, Priority p) {
    if (!socket) return false;
    auto pit = m_peers.find(socket);
    if (pit == m_peers.end()) return false;   // being torn down (disconnect()): never track a socket about to be deleted
    Peer& peer = pit.value();
    if (peer.doomed || socket->state() != QLocalSocket::ConnectedState) return false;
    const qint64 queued = socket->bytesToWrite();
    const Caps& caps = peer.hasCaps ? peer.caps : m_caps;
    switch (decide(queued, p, caps)) {
    case Verdict::Disconnect:
        ++m_slowDisconnects;
        dropConnection(socket, QString("stopped reading: %1 bytes queued (cap %2)").arg(queued).arg(caps.hardBytes));
        return false;
    case Verdict::Drop:
        ++peer.dropped; ++peer.droppedEpisode; ++m_droppedTotal;
        if (!peer.dropping) {   // one line per episode (and at most every 30 s), not one per skipped picture
            peer.dropping = true;
            if (peer.lastWarnMs < 0 || m_clock.elapsed() - peer.lastWarnMs >= 30000) {
                peer.lastWarnMs = m_clock.elapsed();
                peer.warned = true;
                qWarning().noquote() << QString("[IpcTransport] %1 is behind: %2 bytes queued (soft cap %3); skipping previews/telemetry/time sync until it catches up")
                                            .arg(peerLabel(peer.id)).arg(queued).arg(caps.softBytes);
            }
        }
        return false;
    case Verdict::Send:
        break;
    }
    if (peer.dropping && queued < caps.softBytes / 2) {
        if (peer.warned) qInfo().noquote() << QString("[IpcTransport] %1 caught up (%2 droppable messages skipped)").arg(peerLabel(peer.id)).arg(peer.droppedEpisode);
        peer.dropping = false; peer.warned = false; peer.droppedEpisode = 0;
    }
    socket->write(framed);   // buffered by Qt; the overlapped pipe write never blocks this thread
    socket->flush();
    peer.peakQueued = qMax(peer.peakQueued, queued + framed.size());
    return true;
}

void IpcTransport::dropConnection(QLocalSocket* socket, const QString& why) {
    auto pit = m_peers.find(socket);
    if (pit == m_peers.end() || pit->doomed) return;
    pit->doomed = true;
    qWarning().noquote() << QString("[IpcTransport] %1 %2; disconnecting it").arg(peerLabel(pit->id), why);
    // Deferred: the caller may be iterating over clients (broadcast, the Supervisor's UI loops), and abort() reports
    // the disconnect synchronously. Until then the peer is skipped for reads and writes.
    QTimer::singleShot(0, socket, [socket]() { socket->abort(); });
}

bool IpcTransport::send(const IpcProtocol::Message& msg, Priority p) {
    if (m_role != ClientRole || !m_clientSocket || !isConnected()) return false;
    Framer f{msg};
    return writeMessage(m_clientSocket, f, p == Auto ? defaultPriority(msg.type) : p);
}

void IpcTransport::broadcast(const IpcProtocol::Message& msg, Priority p) {
    if (m_role != ServerRole || m_clients.isEmpty()) return;
    Framer f{msg};   // each form once for every client
    const Priority pr = p == Auto ? defaultPriority(msg.type) : p;
    const QMap<QString, QLocalSocket*> clients = m_clients;   // a write never edits m_clients today; stay safe if Qt reports synchronously
    for (auto* s : clients) writeMessage(s, f, pr);
}

bool IpcTransport::sendTo(const QString& clientId, const IpcProtocol::Message& msg, Priority p) {
    if (m_role != ServerRole) return false;
    QLocalSocket* s = m_clients.value(clientId, nullptr);
    if (!s) return false;
    Framer f{msg};
    return writeMessage(s, f, p == Auto ? defaultPriority(msg.type) : p);
}

int IpcTransport::sendTo(const QSet<QString>& clientIds, const IpcProtocol::Message& msg, Priority p) {
    if (m_role != ServerRole || clientIds.isEmpty() || m_clients.isEmpty()) return 0;
    Framer f{msg};   // each form (binary / JSON) once, not once per client
    const Priority pr = p == Auto ? defaultPriority(msg.type) : p;
    int n = 0;
    const QMap<QString, QLocalSocket*> clients = m_clients;
    for (const QString& id : clientIds) {
        QLocalSocket* s = clients.value(id, nullptr);
        if (s && writeMessage(s, f, pr)) ++n;
    }
    return n;
}

QJsonObject IpcTransport::stats() const {
    QJsonObject o;
    o["softCapBytes"] = double(m_caps.softBytes);
    o["hardCapBytes"] = double(m_caps.hardBytes);
    o["maxMessageBytes"] = double(m_maxMessageBytes);
    o["dropped"] = double(m_droppedTotal);
    o["slowDisconnects"] = double(m_slowDisconnects);
    o["oversizeCloses"] = double(m_oversizeCloses);
    o["readBufferBytes"] = double(m_readBufferBytes);
    o["readYields"] = double(m_readYields);
    o["acceptBinary"] = m_acceptBinary;
    o["binarySent"] = double(m_binarySent);
    o["binaryFallbacks"] = double(m_binaryFallbacks);   // messages with bytes sent as JSON/base64 to a peer without binary
    o["binaryReceived"] = double(m_binaryReceived);
    o["unknownFrames"] = double(m_unknownFrames);
    o["subscriptionSkips"] = double(m_subscriptionSkips);
    QJsonArray peers;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        QJsonObject p;
        p["id"] = it->id;
        p["queuedBytes"] = double(it.key()->bytesToWrite());
        p["peakQueuedBytes"] = double(it->peakQueued);
        p["unreadBytes"] = double(it.key()->bytesAvailable() + it->rx.size());
        p["dropped"] = double(it->dropped);
        p["behind"] = it->dropping;
        p["disconnecting"] = it->doomed;
        p["binary"] = it->peerBinary;
        if (it->hasSub) p["previewSub"] = it->sub.all ? QJsonValue(QStringLiteral("all")) : QJsonValue(QJsonArray::fromStringList(it->sub.keys));
        peers.append(p);
    }
    o["peers"] = peers;
    return o;
}

namespace {
// Moves what the frame still needs (up to `want` bytes in rx) out of the socket. True once rx holds `want` bytes.
// Capacity grows geometrically but never past the frame, and only as data arrives: a 64 MB header followed by
// nothing costs nothing.
bool fillTo(QLocalSocket* s, QByteArray& rx, qint64 want) {
    const qint64 have = rx.size();
    if (have >= want) return true;
    const qint64 n = qMin(want - have, s->bytesAvailable());
    if (n <= 0) return false;
    if (rx.capacity() < have + n) rx.reserve(qsizetype(qMin(want, qMax(have + n, qint64(rx.capacity()) * 2))));
    rx.resize(qsizetype(have + n));
    const qint64 got = s->read(rx.data() + have, n);
    rx.resize(qsizetype(have + qMax<qint64>(0, got)));
    return rx.size() >= want;
}
}

// Hands out complete frames. Reads only what the current frame needs, so whatever this thread has not taken yet stays
// in the socket's bounded read buffer (and, past that, in the sender's queue); a time slice per call keeps catching
// up after a stall to short loop turns.
void IpcTransport::processBuffer(QLocalSocket* socket, bool drainAll) {
    const QPointer<IpcTransport> self(this);
    const QPointer<QLocalSocket> guard(socket);
    QElapsedTimer slice; slice.start();
    int handed = 0;
    for (;;) {
        auto pit = m_peers.find(socket);   // looked up again each round: a handler may have changed m_peers
        if (pit == m_peers.end() || pit->doomed) return;   // gone, or closing: ignore what is left
        if (!drainAll && handed > 0 && slice.elapsed() >= m_readSliceMs) {
            if (socket->bytesAvailable() > 0) postDrain(socket);   // else the next readyRead continues
            return;
        }
        QByteArray& rx = pit->rx;
        if (!fillTo(socket, rx, 4)) return;   // the rest arrives with a later readyRead
        quint32 hdr = 0;
        memcpy(&hdr, rx.constData(), 4);
        const bool binFrame = (hdr & IpcProtocol::kBinaryFrameFlag) != 0;   // PS-5: only from a peer we told we accept it
        const quint32 len = hdr & IpcProtocol::kFrameLengthMask;
        if (qint64(len) > m_maxMessageBytes) {
            // A corrupt or hostile header (up to 4 GB): never wait for, or allocate, that much. 64-bit compare, so
            // 4 + len cannot wrap the way the old 32-bit check did.
            ++m_oversizeCloses;
            rx = QByteArray();
            dropConnection(socket, QString("sent a %1-byte frame header (limit %2)").arg(len).arg(m_maxMessageBytes));
            return;
        }
        if (!fillTo(socket, rx, 4 + qint64(len))) return;
        IpcProtocol::Message msg;
        bool deliver = true;
        if (!binFrame) {
            msg = IpcProtocol::Message::fromJson(QByteArray::fromRawData(rx.constData() + 4, qsizetype(len)));
        } else {
            const IpcProtocol::BinaryDecode r = IpcProtocol::decodeBinaryBody(rx.constData() + 4, qint64(len), &msg);
            if (r == IpcProtocol::BinaryDecode::Ok) {
                ++m_binaryReceived;
                if (!m_acceptBinary) IpcProtocol::inlineBinary(msg);   // an owner that only knows the JSON shape
            } else {
                // A newer binary version (or a broken body): its length is valid, so skip it and stay connected.
                deliver = false;
                if (m_unknownFrames++ % 1000 == 0)
                    qWarning().noquote() << QString("[IpcTransport] %1 sent a %2 binary frame (version %3, %4 bytes); skipped")
                                                .arg(peerLabel(pit->id), r == IpcProtocol::BinaryDecode::UnknownVersion ? QStringLiteral("newer") : QStringLiteral("malformed"))
                                                .arg(len > 0 ? int(static_cast<unsigned char>(rx.at(4))) : -1).arg(len);
            }
        }
        if (rx.capacity() > (qint64(256) << 10)) rx = QByteArray(); else rx.resize(0);   // keep small buffers for the next frame
        const QString id = pit->id;
        ++handed;
        if (!deliver || handleControl(socket, msg)) continue;
        emit messageReceived(msg, id);
        if (!self || !guard) return;   // a handler deleted the transport or the socket
    }
}

void IpcTransport::postDrain(QLocalSocket* socket) {
    auto pit = m_peers.find(socket);
    if (pit == m_peers.end() || pit->drainPosted) return;
    pit->drainPosted = true;
    ++m_readYields;
    QMetaObject::invokeMethod(this, [this, s = QPointer<QLocalSocket>(socket)]() {
        if (!s) return;
        auto it = m_peers.find(s.data());
        if (it == m_peers.end()) return;
        it->drainPosted = false;
        processBuffer(s.data());
    }, Qt::QueuedConnection);
}

void IpcTransport::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        socket->setReadBufferSize(m_readBufferBytes);
        QString clientId = QString::number(reinterpret_cast<quintptr>(socket));
        m_clients[clientId] = socket;
        Peer peer; peer.id = clientId;
        m_peers.insert(socket, peer);
        connect(socket, &QLocalSocket::readyRead, this, &IpcTransport::onClientReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &IpcTransport::onClientDisconnected);
        emit clientConnected(clientId);
        qDebug() << "[IpcTransport] Client connected:" << clientId;
        if (socket->bytesAvailable() > 0) postDrain(socket);   // in case it arrived before readyRead was connected
    }
}

// While a continuation is queued it does the reading: one time slice per socket per loop round, not two.
void IpcTransport::onClientReadyRead() {
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) return;
    const auto pit = m_peers.constFind(socket);
    if (pit != m_peers.constEnd() && pit->drainPosted) return;
    processBuffer(socket);
}

void IpcTransport::onClientDisconnected() {
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) return;
    QString clientId = QString::number(reinterpret_cast<quintptr>(socket));
    const QPointer<IpcTransport> self(this);
    processBuffer(socket, true);   // everything it sent before leaving (an engine's last as-run / fatal) comes first
    if (!self) return;
    m_clients.remove(clientId);
    m_peers.remove(socket);
    socket->deleteLater();
    emit clientDisconnected(clientId);
}

void IpcTransport::onSocketReadyRead() {
    if (!m_clientSocket) return;
    const auto pit = m_peers.constFind(m_clientSocket);
    if (pit != m_peers.constEnd() && pit->drainPosted) return;
    processBuffer(m_clientSocket);
}

void IpcTransport::onSocketStateChanged(QLocalSocket::LocalSocketState state) {
    auto* s = qobject_cast<QLocalSocket*>(sender());
    const QPointer<IpcTransport> self(this);
    if (s && state != QLocalSocket::ConnectedState) processBuffer(s, true);   // hand out what arrived before the close
    if (s && state == QLocalSocket::ConnectedState && s == m_clientSocket) sendCaps(s);   // PS-5: caps go first
    if (self) emit connectionStateChanged(state == QLocalSocket::ConnectedState);
}
