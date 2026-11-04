#include "websocketbroker.h"
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QThread>

namespace wsbroker {
Send decide(qint64 queuedBytes, bool droppable, qint64 softCapBytes, qint64 hardCapBytes) {
    if (hardCapBytes > 0 && queuedBytes >= hardCapBytes) return Send::Drop;
    if (droppable && softCapBytes > 0 && queuedBytes >= softCapBytes) return Send::Skip;
    return Send::Now;
}
}

namespace {
const QChar kKeySep(0x1f);    // (type, channel) slot key separator: never part of a type or channel id
}

WebSocketBroker::WebSocketBroker(QObject* parent)
    : QObject(parent)
    , m_server(new QWebSocketServer("PlayPlaySupervisorWS", QWebSocketServer::NonSecureMode, this))
    , m_nextClientId(1)
{
    connect(m_server, &QWebSocketServer::newConnection, this, &WebSocketBroker::onNewConnection);
    // heartbeat keeps today's 1 Hz. telemetry sits just under the engines' 1 Hz, so the steady stream passes untouched.
    // snapshot: the 2 s tick passes untouched and a connect storm (each connect broadcasts a snapshot to everyone) folds
    // into one per second; 1 s rather than closer to 2 s so a folded one never pushes the next tick back.
    m_lowInterval.insert("heartbeat", 1000);
    m_lowInterval.insert("telemetry", 900);
    m_lowInterval.insert("snapshot", 1000);
    m_flushTimer.setSingleShot(true);
    connect(&m_flushTimer, &QTimer::timeout, this, &WebSocketBroker::flushLow);
    m_clock.start();
}

WebSocketBroker::~WebSocketBroker() {
    stop();
}

bool WebSocketBroker::start(quint16 port, const QHostAddress& address) {
    if (m_server->listen(address, port)) {
        qDebug() << "[WebSocketBroker] Listening on port" << m_server->serverPort();
        return true;
    }
    qWarning() << "[WebSocketBroker] Failed to listen on port" << port << m_server->errorString();
    return false;
}

quint16 WebSocketBroker::port() const { return m_server->serverPort(); }

void WebSocketBroker::stop() {
    m_server->close();
    m_flushTimer.stop();
    QList<Client*> all;
    {   // close outside the lock: a socket may report disconnected synchronously, and that slot takes the lock too
        QMutexLocker lock(&m_clientsMutex);
        all.swap(m_clients);
    }
    for (Client* c : all) { c->socket->close(); delete c; }
}

int WebSocketBroker::clientCount() const {
    QMutexLocker lock(&m_clientsMutex);
    return m_clients.size();
}

void WebSocketBroker::setPendingCaps(qint64 softCapBytes, qint64 hardCapBytes) {
    m_softCap = softCapBytes; m_hardCap = hardCapBytes;
}

void WebSocketBroker::setLowRateInterval(const QString& type, int ms) {
    if (ms > 0) m_lowInterval.insert(type, ms); else m_lowInterval.remove(type);
}

bool WebSocketBroker::isDroppable(const QString& type) {
    // State a later message replaces; everything else (alerts, as-run, runlists, events) is never skipped.
    return type == QLatin1String("telemetry") || type == QLatin1String("snapshot") || type == QLatin1String("heartbeat");
}

WebSocketBroker::Client* WebSocketBroker::findClient(QWebSocket* s) const {
    for (Client* c : m_clients) if (c->socket == s) return c;
    return nullptr;
}

bool WebSocketBroker::removeClient(QWebSocket* s) {
    QMutexLocker lock(&m_clientsMutex);
    for (int i = 0; i < m_clients.size(); ++i)
        if (m_clients[i]->socket == s) { delete m_clients.takeAt(i); return true; }
    return false;
}

bool WebSocketBroker::deliver(Client* c, const QString& text, bool droppable, bool snapshot) {
    // bytesToWrite is the socket's own queue (Qt buffers every write until the OS takes it), so it measures how far
    // behind this one client is without touching the others.
    const qint64 queued = c->socket->bytesToWrite();
    switch (wsbroker::decide(queued, droppable, m_softCap, m_hardCap)) {
    case wsbroker::Send::Drop:
        c->dropping = true;
        return false;
    case wsbroker::Send::Skip:
        ++c->skipped;
        if (!c->skipping) { c->skipping = true; qDebug() << "[WebSocketBroker] Client" << c->id << "is behind (" << queued << "bytes queued): skipping state updates"; }
        return true;
    case wsbroker::Send::Now:
        break;
    }
    if (queued < m_softCap) c->skipping = false;
    c->socket->sendTextMessage(text);
    ++c->sent;
    if (snapshot) c->gotSnapshot = true;
    return true;
}

void WebSocketBroker::dropStalled(const QList<QWebSocket*>& victims) {
    for (QWebSocket* s : victims) {
        QString id;
        {
            QMutexLocker lock(&m_clientsMutex);
            if (!findClient(s)) continue;          // already gone
            id = s->property("clientId").toString();
            ++m_droppedSlow;
        }
        qWarning().noquote() << QString("[WebSocketBroker] Client %1 (%2) stopped reading: %3 bytes queued (cap %4); disconnecting it")
                                    .arg(id, s->peerAddress().toString()).arg(s->bytesToWrite()).arg(m_hardCap);
        s->abort();                                // discards its queue; it reconnects and gets a fresh snapshot
        // abort() normally reports disconnected right away (onSocketDisconnected cleans up); if it did not, do it here.
        if (removeClient(s)) {
            s->disconnect(this);
            s->deleteLater();
            emit clientDisconnected(id);
        }
    }
}

void WebSocketBroker::broadcast(const QJsonObject& message) {
    if (QThread::currentThread() != thread()) {    // sockets and the flush timer belong to the broker's thread
        QMetaObject::invokeMethod(this, [this, message]() { broadcast(message); }, Qt::QueuedConnection);
        return;
    }
    QMutexLocker lock(&m_clientsMutex);
    if (m_clients.isEmpty()) return;

    const QString msgType = message.value("type").toString();
    QString channelId = message.value("channelId").toString();
    if (channelId.isEmpty()) channelId = message.value("_channelId").toString();
    // Serialized and converted once: the QString is implicitly shared by every per-client send below.
    const QString text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    const bool droppable = isDroppable(msgType);
    const bool snapshot = msgType == QLatin1String("snapshot");

    // Low-rate clients: at most one message per interval per (type, channel); a newer one replaces the held one and the
    // flush timer sends it when the interval ends, so the latest state always arrives and bursts never do.
    const int interval = m_lowInterval.value(msgType, 0);
    bool lowNow = true;
    if (interval > 0) {
        const qint64 now = m_clock.elapsed();
        LowSlot& s = m_low[msgType + kKeySep + channelId];
        s.intervalMs = interval;
        if (s.lastSentMs < 0 || now - s.lastSentMs >= interval) {
            s.lastSentMs = now; s.hasPending = false; s.pending.clear();
        } else {
            lowNow = false;
            if (s.hasPending) ++m_coalesced;
            s.pending = text; s.droppable = droppable; s.snapshot = snapshot; s.hasPending = true;
            scheduleFlush(now);
        }
    }

    QList<QWebSocket*> victims;
    for (Client* c : m_clients) {
        if (c->dropping || c->socket->state() != QAbstractSocket::ConnectedState) continue;
        const bool now = interval == 0 || c->high || lowNow || (snapshot && !c->gotSnapshot);
        if (now && !deliver(c, text, droppable, snapshot)) victims << c->socket;
    }
    lock.unlock();
    dropStalled(victims);
}

bool WebSocketBroker::sendTo(const QString& clientId, const QJsonObject& message) {
    if (QThread::currentThread() != thread()) {    // sockets belong to the broker's thread
        QMetaObject::invokeMethod(this, [this, clientId, message]() { sendTo(clientId, message); }, Qt::QueuedConnection);
        return true;
    }
    QMutexLocker lock(&m_clientsMutex);
    Client* target = nullptr;
    for (Client* c : m_clients) if (c->id == clientId) { target = c; break; }
    if (!target || target->dropping || target->socket->state() != QAbstractSocket::ConnectedState) return false;
    const QString msgType = message.value("type").toString();
    const QString text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    QList<QWebSocket*> victims;
    const bool ok = deliver(target, text, isDroppable(msgType), msgType == QLatin1String("snapshot"));
    if (!ok) victims << target->socket;
    lock.unlock();
    dropStalled(victims);
    return ok;
}

void WebSocketBroker::scheduleFlush(qint64 nowMs) {
    qint64 next = -1;
    for (auto it = m_low.cbegin(); it != m_low.cend(); ++it)
        if (it->hasPending) { const qint64 due = it->lastSentMs + it->intervalMs; if (next < 0 || due < next) next = due; }
    if (next < 0) { m_flushTimer.stop(); return; }
    const int wait = int(qBound(qint64(0), next - nowMs, qint64(60000)));
    if (!m_flushTimer.isActive() || m_flushTimer.remainingTime() > wait) m_flushTimer.start(wait);
}

void WebSocketBroker::flushLow() {
    QMutexLocker lock(&m_clientsMutex);
    const qint64 now = m_clock.elapsed();
    QList<QWebSocket*> victims;
    for (auto it = m_low.begin(); it != m_low.end(); ++it) {
        LowSlot& s = it.value();
        if (!s.hasPending || now - s.lastSentMs < s.intervalMs) continue;
        const QString text = s.pending;
        s.pending.clear(); s.hasPending = false; s.lastSentMs = now;
        for (Client* c : m_clients) {                 // high clients had it when it was broadcast
            if (c->high || c->dropping || c->socket->state() != QAbstractSocket::ConnectedState) continue;
            if (!deliver(c, text, s.droppable, s.snapshot)) victims << c->socket;
        }
    }
    scheduleFlush(now);
    lock.unlock();
    dropStalled(victims);
}

void WebSocketBroker::broadcastToChannel(const QString& channelId, const QJsonObject& message) {
    QJsonObject wrapped = message;
    wrapped["_channelId"] = channelId;
    broadcast(wrapped);
}

QJsonObject WebSocketBroker::stats() const {
    QMutexLocker lock(&m_clientsMutex);
    QJsonArray clients;
    for (const Client* c : m_clients) {
        QJsonObject o;
        o["clientId"] = c->id; o["rate"] = c->high ? "high" : "low";
        o["queuedBytes"] = double(c->socket->bytesToWrite());
        o["sent"] = double(c->sent); o["skipped"] = double(c->skipped);
        clients.append(o);
    }
    QJsonObject o;
    o["softCapBytes"] = double(m_softCap); o["hardCapBytes"] = double(m_hardCap);
    o["droppedSlowClients"] = double(m_droppedSlow); o["coalesced"] = double(m_coalesced);
    o["clients"] = clients;
    return o;
}

QString WebSocketBroker::generateClientId() {
    return QString("client_%1").arg(m_nextClientId++);
}

void WebSocketBroker::setLanPassword(const QString& password) {
    if (m_lanPassword == password) return;
    m_lanPassword = password;
    if (password.isEmpty()) return;
    QList<QWebSocket*> remote;
    { QMutexLocker lock(&m_clientsMutex); for (Client* c : m_clients) if (c->socket && !c->socket->peerAddress().isLoopback()) remote.append(c->socket); }
    for (QWebSocket* socket : remote) socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "password required");
}

void WebSocketBroker::onNewConnection() {
    QWebSocket* socket = m_server->nextPendingConnection();
    if (!socket) return;
    if (!m_token.isEmpty() || (!m_lanPassword.isEmpty() && !socket->peerAddress().isLoopback())) {
        const QString presented = QUrlQuery(socket->requestUrl()).queryItemValue("token");
        if (!((!m_token.isEmpty() && presented == m_token) || (!m_lanPassword.isEmpty() && presented == m_lanPassword))) { socket->close(QWebSocketProtocol::CloseCodePolicyViolated, "password required"); socket->deleteLater(); return; }
    }

    QString clientId = generateClientId();
    socket->setParent(this);              // freed with the broker if it never reports disconnected
    socket->setProperty("clientId", clientId);
    socket->setProperty("rate", "low"); // Default to low rate telemetry to prevent flooding UI thread

    auto* c = new Client;
    c->socket = socket; c->id = clientId;
    {
        QMutexLocker lock(&m_clientsMutex);
        m_clients.append(c);
    }

    connect(socket, &QWebSocket::textMessageReceived, this, &WebSocketBroker::onTextMessage);
    connect(socket, &QWebSocket::disconnected, this, &WebSocketBroker::onSocketDisconnected);

    qDebug() << "[WebSocketBroker] Client connected:" << clientId;
    emit clientConnected(clientId);

    // Send welcome with current state
    QJsonObject welcome;
    welcome["type"] = "connected";
    welcome["clientId"] = clientId;
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(welcome).toJson(QJsonDocument::Compact)));
}

void WebSocketBroker::onSocketDisconnected() {
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    QString clientId = socket->property("clientId").toString();
    removeClient(socket);

    qDebug() << "[WebSocketBroker] Client disconnected:" << clientId;
    emit clientDisconnected(clientId);
    socket->deleteLater();
}

void WebSocketBroker::onTextMessage(const QString& message) {
    QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
    if (!socket) return;

    QString clientId = socket->property("clientId").toString();
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isObject()) {
        QJsonObject obj = doc.object();
        if (obj.value("type").toString() == "subscribe") {
            QString rate = obj.value("rate").toString();
            socket->setProperty("rate", rate);
            {
                QMutexLocker lock(&m_clientsMutex);
                if (Client* c = findClient(socket)) c->high = rate == QLatin1String("high");
            }
            qDebug() << "[WebSocketBroker] Client" << clientId << "subscribed to" << rate << "rate telemetry";
            return; // Intercepted: do not emit messageReceived
        }
        emit messageReceived(clientId, obj);
    }
}
