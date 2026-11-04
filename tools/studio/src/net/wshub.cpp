#include "net/wshub.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>

#include "core/clock.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "net/auth.h"
#include "net/httpserver.h"
#include "store/db.h"
#include "store/repos.h"

namespace studio::net {

namespace {

constexpr auto kSlowConsumer = QWebSocketProtocol::CloseCode(4008);   // private range: "reconnect with ?since="

QString keyOf(const QHostAddress& a, quint16 port) { return a.toString() + QLatin1Char('|') + QString::number(port); }

QString compactText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// Liveness is measured on a monotonic clock of its own: an injected test clock (FakeClock) must not freeze it.
qint64 steadyNow() {
    static QElapsedTimer t = [] { QElapsedTimer e; e.start(); return e; }();
    return t.elapsed();
}

} // namespace

WsHub::WsHub(QObject* parent) : QObject(parent), m_epoch(QStringLiteral("mem-") + ids::randomHex(8)) {}

WsHub::~WsHub() { close(); }

qint64 WsHub::nowMs() const { return (m_clock ? *m_clock : Clock::system()).nowMs(); }

QString WsHub::storeEpoch(qint64 seq, qint64 ts, const QString& type, const QString& projectId) {
    const QByteArray key = QByteArray::number(seq) + '|' + QByteArray::number(ts) + '|' + type.toUtf8() + '|' + projectId.toUtf8();
    return QStringLiteral("db-") + QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(16));
}

QString WsHub::encode(qint64 seq, qint64 ts, const QString& type, const QString& projectId, const QJsonObject& data) {
    QJsonObject o;
    o["seq"] = double(seq);
    o["ts"] = double(ts);
    o["type"] = type;
    o["projectId"] = projectId;
    o["data"] = data;
    return compactText(o);
}

void WsHub::attachStore(store::Db* db) {
    m_db = db;
    m_ring.clear();
    m_lastSeq = db ? qMax<qint64>(0, store::events::lastSeq(db->reader())) : 0;
    if (db) {
        // The table's numbering: named after its first row (one small indexed read), unnamed while the table is empty.
        const QList<store::EventRow> first = store::events::since(db->reader(), 0, 1);
        m_epoch = first.isEmpty() ? QString() : storeEpoch(first.first().seq, first.first().tsMs, first.first().type, first.first().projectId);
    } else {
        m_epoch = QStringLiteral("mem-") + ids::randomHex(8);   // memory numbering from 1 again: a new stream
    }
    // Clients connected before hold seqs of the old numbering: make them reconnect and resynchronise.
    const QList<QWebSocket*> old = m_clients.keys();
    m_clients.clear();
    for (QWebSocket* ws : old) {
        ws->disconnect(this);
        ws->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("event store attached; reconnect"));
        ws->deleteLater();
    }
    if (!old.isEmpty()) emit clientCountChanged(0);
}

void WsHub::detachStore() {
    m_db = nullptr;
    if (m_epoch.isEmpty()) m_epoch = QStringLiteral("mem-") + ids::randomHex(8);
}

bool WsHub::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (isListening()) {
        if (error) *error = QStringLiteral("WS hub already listening on port %1").arg(this->port());
        return false;
    }
    if (!m_tcp) {
        m_tcp = new QTcpServer(this);
        connect(m_tcp, &QTcpServer::newConnection, this, &WsHub::onTcpConnection);
    }
    if (!m_wss) {
        m_wss = new QWebSocketServer(QStringLiteral("PlayPlayStudio"), QWebSocketServer::NonSecureMode, this);
        m_wss->setHandshakeTimeout(m_opt.handshakeTimeoutMs);
        m_wss->setMaxPendingConnections(qMax(30, m_opt.maxClients));
        connect(m_wss, &QWebSocketServer::newConnection, this, &WsHub::onUpgraded);
    }
    if (!m_tcp->listen(address, port)) {
        if (error) *error = QStringLiteral("WS listen on %1:%2 failed: %3").arg(address.toString()).arg(port).arg(m_tcp->errorString());
        return false;
    }
    if (!m_pingTimer) {
        m_pingTimer = new QTimer(this);
        connect(m_pingTimer, &QTimer::timeout, this, &WsHub::onPingTimer);
    }
    m_pingTimer->start(qMax(1000, m_opt.pingIntervalMs));
    return true;
}

void WsHub::close() {
    if (m_tcp) {
        m_tcp->close();
        for (QTcpSocket* s : m_tcp->findChildren<QTcpSocket*>()) s->abort();   // handshakes still being checked
    }
    if (m_pingTimer) m_pingTimer->stop();
    const QList<QWebSocket*> clients = m_clients.keys();
    m_clients.clear();
    m_pending.clear();
    for (QWebSocket* ws : clients) {
        ws->disconnect(this);
        ws->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("server stopping"));
        ws->deleteLater();
    }
    if (!clients.isEmpty()) emit clientCountChanged(0);
}

bool WsHub::isListening() const { return m_tcp && m_tcp->isListening(); }
quint16 WsHub::port() const { return isListening() ? m_tcp->serverPort() : 0; }

// ---------------- handshake ----------------

void WsHub::onTcpConnection() {
    while (QTcpSocket* s = m_tcp->nextPendingConnection()) {
        auto* t = new QTimer(s);
        t->setSingleShot(true);
        connect(t, &QTimer::timeout, s, &QTcpSocket::abort);
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, this, [this, s]() { inspect(s); });
        t->start(m_opt.handshakeTimeoutMs);
        if (s->bytesAvailable() > 0) inspect(s);
    }
}

void WsHub::refuse(QTcpSocket* s, int status, const QString& why, const QByteArray& extraHeaders) {
    ++m_stats.refusedHandshakes;
    log::debug(u"ws", QStringLiteral("handshake from %1 refused: %2 %3").arg(s->peerAddress().toString()).arg(status).arg(why));
    QJsonObject o;
    o["ok"] = false;
    o["error"] = why;
    const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
    s->disconnect(this);
    s->skip(s->bytesAvailable());          // nothing unread left behind: closing on unread data resets the connection
    s->write("HTTP/1.1 " + QByteArray::number(status) + ' ' + reasonPhrase(status) + "\r\nContent-Type: application/json\r\n"
             "Cache-Control: no-store\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" + extraHeaders +
             "Connection: close\r\n\r\n" + body);
    s->disconnectFromHost();
}

void WsHub::inspect(QTcpSocket* s) {
    const qint64 avail = s->bytesAvailable();
    // Peek, never read: QWebSocketServer parses the same bytes after the hand-over.
    const QByteArray head = s->peek(qMin<qint64>(avail, m_opt.maxHeadBytes + 4));
    const qsizetype end = head.indexOf("\r\n\r\n");
    if (end < 0) {
        if (avail > m_opt.maxHeadBytes) refuse(s, 431, QStringLiteral("request head too large"));
        return;
    }
    RequestHead h;
    QString why;
    if (const int code = parseRequestHead(QByteArrayView(head).left(end), &h, &why)) return refuse(s, code, why);
    if (h.method != "GET") return refuse(s, 405, QStringLiteral("method not allowed"), "Allow: GET\r\n");
    if (h.minor != 1) return refuse(s, 400, QStringLiteral("a WebSocket handshake needs HTTP/1.1"));
    QByteArray rawPath, rawQuery;
    QString authority;
    if (!splitTarget(h.target, &rawPath, &rawQuery, &authority)) return refuse(s, 400, QStringLiteral("bad request target"));
    if (rawPath != "/" && rawPath != "/ws") return refuse(s, 404, QStringLiteral("not found (the event stream is /ws)"));
    const QList<QByteArray> hosts = h.headers.values("host");
    if (hosts.size() != 1) return refuse(s, 400, QStringLiteral("exactly one Host header is required"));
    const QString host = hostName(hosts.first());
    if (host.isEmpty()) return refuse(s, 400, QStringLiteral("bad Host header"));
    // An absolute-form target names its host too: both must be ours.
    for (const QString& name : {host, authority})
        if (!name.isEmpty() && !m_allowedHosts.isEmpty() && !m_allowedHosts.contains(name))
            return refuse(s, 421, QStringLiteral("this server does not serve host '%1'").arg(name.left(100)));
    const QByteArray origin = h.headers.value("origin");
    if (!origin.isEmpty()) {
        // A browser page of another site may open a WebSocket to 127.0.0.1 (no CORS for WebSockets): refuse its origin.
        const QUrl u(QString::fromLatin1(origin));
        const bool web = u.isValid() && (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https"));
        if (!web || (!m_allowedHosts.isEmpty() && !m_allowedHosts.contains(u.host().toLower())))
            return refuse(s, 403, QStringLiteral("origin not allowed"));
    }
    if (!h.headers.hasToken("upgrade", "websocket") || !h.headers.hasToken("connection", "upgrade"))
        return refuse(s, 426, QStringLiteral("this port speaks WebSocket only"), "Upgrade: websocket\r\n");
    const QUrlQuery q(QString::fromUtf8(rawQuery));
    if (!m_auth || !m_auth->check(h.headers, &q))
        return refuse(s, 401, QStringLiteral("missing or invalid token"), "WWW-Authenticate: Bearer realm=\"PlayPlayStudio\"\r\n");
    Pending p;
    if (q.hasQueryItem(QStringLiteral("since"))) {
        bool ok = false;
        p.since = q.queryItemValue(QStringLiteral("since")).toLongLong(&ok);
        if (!ok || p.since < 0) return refuse(s, 400, QStringLiteral("since must be a seq (an integer >= 0)"));
        p.hasSince = true;
        p.epoch = q.queryItemValue(QStringLiteral("epoch"), QUrl::FullyDecoded).left(128);
    }
    p.project = q.queryItemValue(QStringLiteral("project"), QUrl::FullyDecoded);
    for (const QString& t : q.queryItemValue(QStringLiteral("types"), QUrl::FullyDecoded).split(QLatin1Char(','), Qt::SkipEmptyParts))
        p.types.insert(t.trimmed());
    if (m_clients.size() + m_pending.size() >= m_opt.maxClients) return refuse(s, 503, QStringLiteral("too many event stream clients"));

    // Hand over: our watchers off, then QWebSocketServer reads the (unconsumed) handshake and upgrades.
    s->disconnect(this);
    QObject::disconnect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
    qDeleteAll(s->findChildren<QTimer*>(QString(), Qt::FindDirectChildrenOnly));
    const qint64 now = steadyNow();
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = now - it->atMs > 2LL * m_opt.handshakeTimeoutMs ? m_pending.erase(it) : std::next(it);
    p.atMs = now;
    m_pending.insert(keyOf(s->peerAddress(), s->peerPort()), p);
    s->setParent(m_wss);
    m_wss->handleConnection(s);
}

void WsHub::onUpgraded() {
    while (m_wss->hasPendingConnections()) {
        QWebSocket* ws = m_wss->nextPendingConnection();
        if (!ws) break;
        const auto it = m_pending.find(keyOf(ws->peerAddress(), ws->peerPort()));
        if (it == m_pending.end()) {       // never passed inspect(): cannot happen, refuse anyway
            ws->close(QWebSocketProtocol::CloseCodePolicyViolated, QStringLiteral("handshake not checked"));
            ws->deleteLater();
            continue;
        }
        const Pending p = it.value();
        m_pending.erase(it);
        ws->setParent(this);
        ws->setMaxAllowedIncomingMessageSize(quint64(m_opt.maxIncomingMessageBytes));
        ws->setMaxAllowedIncomingFrameSize(quint64(m_opt.maxIncomingMessageBytes));
        connect(ws, &QWebSocket::textMessageReceived, this, [this, ws](const QString& t) { onMessage(ws, t); });
        connect(ws, &QWebSocket::binaryMessageReceived, this, [ws](const QByteArray&) {
            ws->close(QWebSocketProtocol::CloseCodeDatatypeNotSupported, QStringLiteral("text messages only"));
        });
        connect(ws, &QWebSocket::pong, this, [this, ws](quint64, const QByteArray&) {
            if (const auto c = m_clients.find(ws); c != m_clients.end()) c->lastSeenMs = steadyNow();
        });
        connect(ws, &QWebSocket::disconnected, this, [this, ws]() { dropClient(ws); });
        Client c;
        c.ws = ws;
        c.project = p.project;
        c.types = p.types;
        c.lastSeenMs = steadyNow();
        ++m_stats.connected;
        Client& ref = m_clients.insert(ws, c).value();
        greet(ref, p);
        emit clientCountChanged(int(m_clients.size()));
    }
}

// ---------------- events ----------------

bool WsHub::wants(const Client& c, const Event& e) const {
    if (!c.project.isEmpty() && !e.projectId.isEmpty() && e.projectId != c.project) return false;
    return c.types.isEmpty() || c.types.contains(e.type);
}

bool WsHub::sendTo(Client& c, const QString& text, bool live) {
    c.ws->sendTextMessage(text);
    return !live || c.ws->bytesToWrite() <= m_opt.maxBufferedBytes;
}

void WsHub::greet(Client& c, const Pending& p) {
    QJsonObject hello;
    // In store mode broadcasts trail the commits (their callbacks may still be queued), so a client can hold seqs past
    // m_lastSeq that it replayed from the table: "ahead" is judged against the table's last seq, not the last broadcast.
    qint64 last = m_lastSeq;
    if (p.hasSince && p.since > last && m_db && m_db->isOpen()) last = qMax(last, store::events::lastSeq(m_db->reader()));
    hello["type"] = QStringLiteral("hello");
    hello["service"] = QStringLiteral("playplay-studio");
    hello["lastSeq"] = double(last);
    hello["epoch"] = m_epoch;
    hello["mode"] = m_db ? QStringLiteral("store") : QStringLiteral("memory");
    QStringList replay;
    QString reset;
    c.hwm = m_lastSeq;                     // without ?since: live events only, from the next seq on
    if (p.hasSince) {
        hello["since"] = double(p.since);
        if (!p.epoch.isEmpty() && p.epoch != m_epoch) {
            reset = QStringLiteral("epoch");   // another process's or another studio.db's numbering: not this stream
        } else if (p.since > last) {
            reset = QStringLiteral("ahead");
        } else if (p.since == last) {
            c.hwm = p.since;               // up to date (past m_lastSeq while callbacks are queued: those are dropped)
        } else {
            qint64 hwm = p.since;
            auto collect = [&](const Event& e) {
                if (e.seq <= hwm) return;
                hwm = e.seq;
                if (wants(c, e)) replay << e.text;
            };
            if (!m_ring.empty() && p.since >= m_ring.front().seq - 1) {
                for (const Event& e : m_ring) collect(e);
            } else if (m_db && m_db->isOpen()) {
                const QList<store::EventRow> rows = store::events::since(m_db->reader(), p.since, m_opt.maxReplay + 1);
                if (rows.size() > m_opt.maxReplay) {
                    reset = QStringLiteral("too_far");
                } else if (p.since > 0 && (rows.isEmpty() || rows.first().seq > p.since + 1)) {
                    reset = QStringLiteral("gap");
                } else {
                    for (const store::EventRow& r : rows) {
                        Event e;
                        e.seq = r.seq;
                        e.type = r.type;
                        e.projectId = r.projectId;
                        if (r.seq > hwm && wants(c, e)) e.text = encode(r.seq, r.tsMs, r.type, r.projectId, r.payload);
                        collect(e);
                    }
                    for (const Event& e : m_ring) collect(e);   // committed and broadcast since the read: no gap
                }
            } else {
                reset = QStringLiteral("gap");
            }
            if (reset.isEmpty()) c.hwm = hwm;
            else replay.clear();
        }
    }
    if (!reset.isEmpty()) {
        hello["reset"] = reset;
        ++m_stats.resets;
    }
    hello["replay"] = int(replay.size());
    sendTo(c, compactText(hello), false);
    for (const QString& t : replay) sendTo(c, t, false);
    m_stats.replayed += quint64(replay.size());
}

bool WsHub::publish(const QString& type, const QString& projectId, const QJsonObject& data) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, type, projectId, data]() { publish(type, projectId, data); }, Qt::QueuedConnection);
        return true;
    }
    if (type.isEmpty()) return false;
    ++m_stats.published;
    const qint64 ts = nowMs();
    if (!m_db) {
        Event e;
        e.seq = m_lastSeq + 1;
        e.ts = ts;
        e.type = type;
        e.projectId = projectId;
        e.text = encode(e.seq, ts, type, projectId, data);
        broadcast(std::move(e));
        return true;
    }
    // Broadcast from the commit callback: an event a client saw is always one replay can give back.
    if (!m_db->isOpen()) {                 // closed without detachStore(): refuse rather than touch a stopped writer
        ++m_stats.persistRefused;
        log::warn(u"ws", QStringLiteral("event '%1' dropped: the event store is closed").arg(type));
        return false;
    }
    const bool queued = m_db->writer().submit(store::events::append(type, projectId, data, ts), this,
                                              [this, type, projectId, data, ts](const store::WriteResult& r) {
        if (!r.ok) {
            ++m_stats.persistFailed;
            log::warn(u"ws", QStringLiteral("event '%1' was not stored, so it is not sent: %2").arg(type, r.error));
            return;
        }
        Event e;
        e.seq = r.value.toLongLong();
        e.ts = ts;
        e.type = type;
        e.projectId = projectId;
        e.text = encode(e.seq, ts, type, projectId, data);
        broadcast(std::move(e));
    });
    if (!queued) {
        ++m_stats.persistRefused;
        log::warn(u"ws", QStringLiteral("event '%1' dropped: the store refused the write").arg(type));
    }
    return queued;
}

void WsHub::broadcast(Event e) {
    // The first event of an empty table names its numbering (callbacks arrive in commit order: this is its first row).
    if (m_epoch.isEmpty()) m_epoch = m_db ? storeEpoch(e.seq, e.ts, e.type, e.projectId) : QStringLiteral("mem-") + ids::randomHex(8);
    m_lastSeq = qMax(m_lastSeq, e.seq);
    ++m_stats.broadcast;
    QList<QWebSocket*> slow;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        Client& c = it.value();
        if (e.seq <= c.hwm) continue;      // already replayed to this client
        c.hwm = e.seq;
        if (wants(c, e) && !sendTo(c, e.text, true)) slow << c.ws;
    }
    for (QWebSocket* ws : slow) {
        ++m_stats.slowDropped;
        log::warn(u"ws", QStringLiteral("event stream client %1 is too far behind; closing it (it can reconnect with ?since=)")
                             .arg(ws->peerAddress().toString()));
        m_clients.remove(ws);
        ws->close(kSlowConsumer, QStringLiteral("too slow: reconnect with ?since=<last seq>"));
        QTimer::singleShot(5000, ws, [ws]() { ws->abort(); });
    }
    if (!slow.isEmpty()) emit clientCountChanged(int(m_clients.size()));
    const qint64 seq = e.seq;
    if (m_opt.ringSize > 0) {
        m_ring.push_back(std::move(e));
        while (m_ring.size() > size_t(m_opt.ringSize)) m_ring.pop_front();
    }
    emit broadcasted(seq);
}

void WsHub::dropClient(QWebSocket* ws) {
    if (m_clients.remove(ws)) emit clientCountChanged(int(m_clients.size()));
    ws->deleteLater();
}

void WsHub::onMessage(QWebSocket* ws, const QString& text) {
    const auto it = m_clients.find(ws);
    if (it == m_clients.end()) return;
    it->lastSeenMs = steadyNow();
    const QJsonObject msg = json::parseObject(text.toUtf8(), nullptr, m_opt.maxIncomingMessageBytes);
    QJsonObject out;
    if (msg.value(QStringLiteral("op")).toString() == QLatin1String("ping")) {
        out["type"] = QStringLiteral("pong");
        out["ts"] = double(nowMs());
        out["lastSeq"] = double(m_lastSeq);
        out["epoch"] = m_epoch;            // a client that connected while a store's table was empty learns it here
    } else {
        out["type"] = QStringLiteral("error");
        out["error"] = QStringLiteral("unknown op (supported: ping)");
    }
    sendTo(*it, compactText(out), false);
}

void WsHub::onPingTimer() {
    const qint64 now = steadyNow();
    QList<QWebSocket*> dead;
    for (auto it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (now - it->lastSeenMs > m_opt.pongTimeoutMs) dead << it->ws;
        else it->ws->ping();
    }
    for (QWebSocket* ws : dead) {
        m_clients.remove(ws);
        ws->abort();
        ws->deleteLater();
    }
    if (!dead.isEmpty()) emit clientCountChanged(int(m_clients.size()));
    for (auto it = m_pending.begin(); it != m_pending.end();)
        it = now - it->atMs > 2LL * m_opt.handshakeTimeoutMs ? m_pending.erase(it) : std::next(it);
}

QJsonObject WsHub::stats() const {
    QJsonObject o;
    o["listening"] = isListening();
    o["port"] = int(port());
    o["mode"] = m_db ? QStringLiteral("store") : QStringLiteral("memory");
    o["clients"] = int(m_clients.size());
    o["lastSeq"] = double(m_lastSeq);
    o["epoch"] = m_epoch;
    o["ring"] = int(m_ring.size());
    o["published"] = double(m_stats.published);
    o["broadcast"] = double(m_stats.broadcast);
    o["replayed"] = double(m_stats.replayed);
    o["resets"] = double(m_stats.resets);
    o["connected"] = double(m_stats.connected);
    o["refusedHandshakes"] = double(m_stats.refusedHandshakes);
    o["slowDropped"] = double(m_stats.slowDropped);
    o["persistFailed"] = double(m_stats.persistFailed);
    o["persistRefused"] = double(m_stats.persistRefused);
    return o;
}

} // namespace studio::net
