#include "restapiserver.h"
#include <QPointer>
#include "channelmanager.h"
#include "../core/loopwatchdog.h"
#include <QJsonDocument>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>
#include <QRegularExpression>
#include <QFile>
#include <QElapsedTimer>
#include <QDeadlineTimer>
#include <memory>

namespace {
const char* CRLF = "\r\n";
qint64 nowMs() { return QDeadlineTimer::current().deadline(); }   // monotonic ms, no QDateTime per byte
}

RestApiServer::RestApiServer(ChannelManager* channelMgr, QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_channelMgr(channelMgr)
{
    connect(m_server, &QTcpServer::newConnection, this, &RestApiServer::onNewConnection);
    m_sweep.setTimerType(Qt::CoarseTimer);
    connect(&m_sweep, &QTimer::timeout, this, &RestApiServer::sweepIdle);
    setIdleTimeoutMs(m_idleMs);
}

RestApiServer::~RestApiServer() {
    stop();
}

bool RestApiServer::start(quint16 port) {
    // The dashboard and the station agents open connections in bursts; with Qt's default backlog (50) the extra SYNs
    // are dropped and those clients wait for a TCP retransmit (seconds) before the server even sees them.
    m_server->setListenBacklogSize(256);
    if (m_server->listen(QHostAddress::Any, port)) {
        qInfo() << "[RestApiServer] listening on port" << m_server->serverPort();
        if (m_idleMs > 0) m_sweep.start();
        return true;
    }
    qWarning() << "[RestApiServer] failed to listen on port" << port << m_server->errorString();
    return false;
}

void RestApiServer::stop() {
    m_sweep.stop();
    m_server->close();
    for (auto* socket : m_conns.keys()) socket->close();
    m_conns.clear();
}

void RestApiServer::setIdleTimeoutMs(int ms) {
    m_idleMs = ms;
    if (ms <= 0) { m_sweep.stop(); return; }
    // One coarse timer for every connection (2,000 idle sockets must not mean 2,000 timers); a connection is closed
    // between idleMs and idleMs + interval after its last activity.
    m_sweep.setInterval(qBound(50, ms / 4, 5000));
    if (m_server->isListening()) m_sweep.start();
}

void RestApiServer::sweepIdle() {
    if (m_idleMs <= 0 || m_conns.isEmpty()) return;
    const qint64 now = nowMs();
    QList<QTcpSocket*> idle;   // collected first: close() may emit disconnected synchronously and change m_conns
    for (auto it = m_conns.cbegin(); it != m_conns.cend(); ++it)
        if (it.value().pending == 0 && now - it.value().lastActiveMs >= m_idleMs) idle.append(it.key());
    for (QTcpSocket* s : idle) {
        ++m_stats.idleClosed;
        if (m_conns.remove(s)) { s->close(); s->deleteLater(); }   // close() alone would keep the QObject under the server
    }
}

void RestApiServer::registerRoute(const QString& method, const QString& path, Handler handler) {
    QMutexLocker lock(&m_routesMutex);
    m_routes[method.toUpper() + " " + path] = handler;
    m_trieDirty = true;
}

QStringList RestApiServer::routeKeys() {
    QMutexLocker lock(&m_routesMutex);
    QStringList k = m_routes.keys(); k.sort(); return k;
}

void RestApiServer::registerUpload(const QString& method, const QString& pathPrefix, UploadHandler handler) {
    QMutexLocker lock(&m_routesMutex);
    m_uploads.append({method.toUpper() + " " + pathPrefix, handler});
}

bool RestApiServer::authorized(const QByteArray& headers, const QHash<QString, QString>& query, bool loopback) const {
    if (m_token.isEmpty() && (m_lanPass.isEmpty() || loopback)) return true;
    static const QRegularExpression auth(R"rx(\r\nAuthorization:\s*Bearer\s+([^\r\n]+))rx", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ppPass(R"rx(\r\nX-PP-Pass:\s*([^\r\n]+))rx", QRegularExpression::CaseInsensitiveOption);
    const QString head = QString::fromUtf8(headers);
    const QRegularExpressionMatch am = auth.match(head), pm = ppPass.match(head);
    const QString presented = am.hasMatch() ? am.captured(1).trimmed() : query.value("token");
    return (!m_token.isEmpty() && presented == m_token) ||
           (!m_lanPass.isEmpty() && (presented == m_lanPass || (pm.hasMatch() && pm.captured(1).trimmed() == m_lanPass)));
}

void RestApiServer::registerRaw(const QString& method, const QString& pathPrefix, RawHandler handler) {
    QMutexLocker lock(&m_routesMutex);
    m_raw.append(qMakePair(method.toUpper() + " " + pathPrefix, handler));
}

void RestApiServer::setupDefaultRoutes() {}

// --- route trie -------------------------------------------------------------------------------------------------------

void RestApiServer::rebuildTrieLocked() {
    m_trie.clear(); m_trieRoots.clear();
    // Sorted keys: when two routes compete for the same node (e.g. "/a/{id}" and "/a/{name}") the first one in sorted
    // order wins every time, where the old per-request scan took whichever QHash order produced.
    QStringList keys = m_routes.keys(); keys.sort();
    for (const QString& key : keys) {
        const int sp = key.indexOf(' ');
        if (sp < 0) continue;
        const QString method = key.left(sp);
        const QStringList segs = key.mid(sp + 1).split('/', Qt::SkipEmptyParts);
        int node;
        auto rit = m_trieRoots.find(method);
        if (rit == m_trieRoots.end()) { node = m_trie.size(); m_trie.append(TrieNode()); m_trieRoots.insert(method, node); }
        else node = rit.value();
        for (const QString& seg : segs) {
            const bool isParam = seg.size() >= 2 && seg.startsWith('{') && seg.endsWith('}');
            if (isParam) {
                if (m_trie[node].param < 0) {
                    const int n = m_trie.size(); m_trie.append(TrieNode());
                    m_trie[node].param = n; m_trie[node].paramName = seg.mid(1, seg.size() - 2);
                }
                node = m_trie[node].param;
            } else {
                auto kit = m_trie[node].kids.find(seg);
                if (kit == m_trie[node].kids.end()) {
                    const int n = m_trie.size(); m_trie.append(TrieNode());
                    kit = m_trie[node].kids.insert(seg, n);
                }
                node = kit.value();
            }
        }
        if (m_trie[node].key.isEmpty()) m_trie[node].key = key;
    }
    m_trieDirty = false;
}

int RestApiServer::matchTrie(int node, const QStringList& segs, int i, QHash<QString, QString>& params) const {
    const TrieNode& n = m_trie[node];
    if (i == segs.size()) return n.key.isEmpty() ? -1 : node;
    auto kit = n.kids.constFind(segs[i]);
    if (kit != n.kids.cend()) { const int r = matchTrie(kit.value(), segs, i + 1, params); if (r >= 0) return r; }
    if (n.param >= 0) {
        const int r = matchTrie(n.param, segs, i + 1, params);
        if (r >= 0) { params.insert(n.paramName, segs[i]); return r; }
    }
    return -1;
}

QString RestApiServer::matchRoute(const QString& method, const QString& path, QHash<QString, QString>* params) {
    QMutexLocker lock(&m_routesMutex);
    const QString exact = method + " " + path;
    if (m_routes.contains(exact)) return exact;
    if (m_trieDirty) rebuildTrieLocked();
    auto rit = m_trieRoots.constFind(method);
    if (rit == m_trieRoots.cend()) return QString();
    QHash<QString, QString> p;
    const int node = matchTrie(rit.value(), path.split('/', Qt::SkipEmptyParts), 0, p);
    if (node < 0) return QString();
    if (params) for (auto it = p.cbegin(); it != p.cend(); ++it) params->insert(it.key(), it.value());
    return m_trie[node].key;
}

// --- responses --------------------------------------------------------------------------------------------------------

void RestApiServer::writeRawHeader(QTcpSocket* socket, int status, const QByteArray& contentType, bool keepAlive) {
    QByteArray h;
    h += "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Error") + CRLF;
    h += "Content-Type: " + contentType + CRLF;
    h += "Access-Control-Allow-Origin: *"; h += CRLF;
    h += "Cache-Control: no-cache, no-store"; h += CRLF;
    h += keepAlive ? "Connection: keep-alive" : "Connection: close"; h += CRLF;
    h += CRLF;
    socket->write(h);
}

bool RestApiServer::serveStatic(QTcpSocket* socket, const QString& path) {
    if (m_staticRoot.isEmpty()) return false;
    QString rel = path;
    if (rel == "/" || rel.isEmpty()) rel = "/index.html";
    if (rel.contains("..")) return false;
    QFile f(m_staticRoot + rel);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        // SPA fallback for client-side routes
        if (rel.contains('.')) return false;
        f.setFileName(m_staticRoot + "/index.html");
        if (!f.open(QIODevice::ReadOnly)) return false;
        rel = "/index.html";
    }
    const QByteArray body = f.readAll();
    QByteArray ct = "application/octet-stream";
    if (rel.endsWith(".html")) ct = "text/html; charset=utf-8";
    else if (rel.endsWith(".js")) ct = "application/javascript";
    else if (rel.endsWith(".css")) ct = "text/css";
    else if (rel.endsWith(".json")) ct = "application/json";
    else if (rel.endsWith(".png")) ct = "image/png";
    else if (rel.endsWith(".jpg") || rel.endsWith(".jpeg")) ct = "image/jpeg";
    else if (rel.endsWith(".svg")) ct = "image/svg+xml";
    else if (rel.endsWith(".ico")) ct = "image/x-icon";
    else if (rel.endsWith(".woff2")) ct = "font/woff2";
    QByteArray h;
    h += "HTTP/1.1 200 OK"; h += CRLF;
    h += "Content-Type: " + ct; h += CRLF;
    h += "Content-Length: " + QByteArray::number(body.size()); h += CRLF;
    h += "Connection: keep-alive"; h += CRLF; h += CRLF;
    socket->write(h + body);
    socket->flush();
    return true;
}

// --- connections ------------------------------------------------------------------------------------------------------

void RestApiServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        Conn c; c.lastActiveMs = nowMs();
        m_conns.insert(socket, c);
        connect(socket, &QTcpSocket::readyRead, this, &RestApiServer::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &RestApiServer::onSocketDisconnected);
    }
}

void RestApiServer::detach(QTcpSocket* socket) {
    m_conns.remove(socket);
    disconnect(socket, &QTcpSocket::readyRead, this, &RestApiServer::onSocketReadyRead);
    disconnect(socket, &QTcpSocket::disconnected, this, &RestApiServer::onSocketDisconnected);
}

void RestApiServer::onSocketReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    auto cit = m_conns.find(socket);
    if (cit == m_conns.end()) return;

    cit.value().lastActiveMs = nowMs();
    cit.value().buf.append(socket->readAll());
    QByteArray& buf = cit.value().buf;

    for (;;) {
        const int headerEnd = buf.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            // Header cap: a client that never ends its headers (or sends a huge one) gets 431 and is dropped, instead
            // of growing this buffer until memory runs out.
            if (buf.size() > m_maxHeaderBytes) {
                ++m_stats.headerTooLarge;
                qWarning() << "[RestApiServer] request header over" << m_maxHeaderBytes << "bytes from" << socket->peerAddress().toString() << "- 431";
                buf.clear();
                sendError(socket, 431, "Request header fields too large", true);
            }
            return;
        }
        if (headerEnd > m_maxHeaderBytes) {
            ++m_stats.headerTooLarge;
            buf.clear();
            sendError(socket, 431, "Request header fields too large", true);
            return;
        }
        // Streaming uploads are dispatched on headers alone so a multi-GB body never sits in memory.
        {
            const QString firstLine = QString::fromUtf8(buf.left(buf.indexOf("\r\n")));
            const QStringList parts = firstLine.split(' ');
            if (parts.size() >= 2) {
                const QString method = parts[0].toUpper(); const QUrl url(parts[1]);
                QMutexLocker lock(&m_routesMutex);
                for (const auto& u : m_uploads) {
                    const QString key = method + " ";
                    if (!u.first.startsWith(key) || !url.path().startsWith(u.first.mid(key.size()))) continue;
                    UploadHandler h = u.second; lock.unlock();
                    QHash<QString, QString> query; for (const auto& item : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) query[item.first] = item.second;
                    if (!authorized(buf.left(headerEnd), query, socket->peerAddress().isLoopback())) { sendError(socket, 401, "Unauthorized: password required"); buf.clear(); return; }
                    static const QRegularExpression cl(R"rx(Content-Length:\s*(\d+))rx", QRegularExpression::CaseInsensitiveOption);
                    const QRegularExpressionMatch cm = cl.match(QString::fromUtf8(buf.left(headerEnd)));
                    const qint64 len = cm.hasMatch() ? cm.captured(1).toLongLong() : -1;
                    const QByteArray initial = buf.mid(headerEnd + 4);
                    detach(socket);
                    ++m_stats.requests;
                    QElapsedTimer t; t.start();
                    { WatchdogScope tag(QByteArray("rest:") + method.toUtf8() + ' ' + url.path().toUtf8()); h(socket, url.path(), query, len, initial); }
                    const qint64 ms = t.elapsed(); m_stats.maxHandlerMs = qMax(m_stats.maxHandlerMs, ms);
                    if (m_slowMs > 0 && ms > m_slowMs) { ++m_stats.slow; qWarning() << "[RestApiServer] slow upload handler" << (method + " " + url.path()) << ms << "ms on the event loop"; }
                    return;
                }
            }
        }
        qint64 contentLength = 0;
        static const QRegularExpression re("Content-Length:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = re.match(QString::fromUtf8(buf.left(headerEnd)));
        if (match.hasMatch()) contentLength = match.captured(1).toLongLong();
        if (contentLength < 0 || contentLength > m_maxBodyBytes) {
            ++m_stats.bodyTooLarge;
            qWarning() << "[RestApiServer] request body of" << contentLength << "bytes from" << socket->peerAddress().toString() << "- 413";
            buf.clear();
            sendError(socket, 413, "Request body too large", true);
            return;
        }
        const qint64 totalLen = headerEnd + 4 + contentLength;
        if (buf.size() < totalLen) return;
        const QByteArray request = buf.left(int(totalLen));
        buf.remove(0, int(totalLen));
        parseAndRoute(socket, request);
        cit = m_conns.find(socket);
        if (cit == m_conns.end()) return;   // taken over by a raw handler, or closed
        if (cit.value().buf.isEmpty()) return;
    }
}

void RestApiServer::onSocketDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    if (m_conns.remove(socket)) socket->deleteLater();
}

void RestApiServer::registerAsync(const QString& method, const QString& path, AsyncHandler handler) {
    registerRoute(method, path, [this, handler](const QJsonObject& params, const QHash<QString, QString>& query) {
        QPointer<QTcpSocket> sp(m_currentSocket);
        // state: 0 = handler still running, 1 = reply outstanding (pending counted), 2 = replied. A handler that
        // replies synchronously never counts as pending, so the idle sweep sees the connection correctly.
        auto state = std::make_shared<int>(0);
        handler(params, query, [this, sp, state](int status, const QJsonObject& body) {
            if (*state == 2) return;
            const bool counted = *state == 1;
            *state = 2;
            if (!sp) return;
            auto cit = m_conns.find(sp);
            if (cit != m_conns.end()) { cit.value().lastActiveMs = nowMs(); if (counted && cit.value().pending > 0) --cit.value().pending; }
            if (status >= 400) sendError(sp, status, body.value("error").toString(body.value("message").toString("error")));
            else sendResponse(sp, status, body);
        });
        if (*state == 0) {
            *state = 1;
            if (sp) { auto cit = m_conns.find(sp); if (cit != m_conns.end()) ++cit.value().pending; }
        }
        QJsonObject marker; marker["_async"] = true; return marker;
    });
}

void RestApiServer::parseAndRoute(QTcpSocket* socket, const QByteArray& request) {
    const int firstLineEnd = request.indexOf("\r\n");
    if (firstLineEnd < 0) { sendError(socket, 400, "Bad request"); return; }
    const QString firstLine = QString::fromUtf8(request.left(firstLineEnd));
    const QStringList parts = firstLine.split(' ');
    if (parts.size() < 2) { sendError(socket, 400, "Bad request"); return; }

    const QString method = parts[0].toUpper();
    const QUrl url(parts[1]);
    const QString path = url.path();
    QHash<QString, QString> query;
    for (const auto& item : QUrlQuery(url).queryItems(QUrl::FullyDecoded)) query[item.first] = item.second;

    ++m_stats.requests;
    // Watchdog activity tag for the whole dispatch (static files, raw and JSON handlers): a stall report names the
    // route that held the loop. Per-route timing feeds the slow-handler log below.
    WatchdogScope tag(QByteArray("rest:") + method.toUtf8() + ' ' + path.toUtf8());
    QElapsedTimer timer; timer.start();
    struct SlowLog {
        RestApiServer* s; const QString& method; const QString& path; QElapsedTimer& t;
        ~SlowLog() {
            const qint64 ms = t.elapsed(); s->m_stats.maxHandlerMs = qMax(s->m_stats.maxHandlerMs, ms);
            if (s->m_slowMs > 0 && ms > s->m_slowMs) { ++s->m_stats.slow; qWarning() << "[RestApiServer] slow handler" << (method + " " + path) << ms << "ms on the event loop"; }
        }
    } slowLog{this, method, path, timer};

    QJsonObject body;
    const int bodyStart = request.indexOf("\r\n\r\n");
    if (bodyStart >= 0) {
        const QByteArray bodyData = request.mid(bodyStart + 4);
        if (!bodyData.isEmpty()) {
            const QJsonDocument doc = QJsonDocument::fromJson(bodyData);
            if (doc.isObject()) body = doc.object();
            else if (doc.isArray()) body["_array"] = doc.array();
        }
    }

    if (method == "OPTIONS") {
        QByteArray h;
        h += "HTTP/1.1 204 No Content"; h += CRLF;
        h += "Access-Control-Allow-Origin: *"; h += CRLF;
        h += "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS"; h += CRLF;
        h += "Access-Control-Allow-Headers: Content-Type, Authorization"; h += CRLF;
        h += "Content-Length: 0"; h += CRLF;
        h += "Connection: keep-alive"; h += CRLF; h += CRLF;
        socket->write(h);
        socket->flush();
        return;
    }

    // Bearer-token gate for the API and preview streams (static dashboard files stay open so the
    // login prompt can load; the dashboard then sends the token with every call).
    const bool stationPath = path.startsWith("/api/") && !path.startsWith("/api/v1/");
    const bool loopback = socket->peerAddress().isLoopback();
    if ((!m_token.isEmpty() && (path.startsWith("/api/") || path.startsWith("/preview/"))) || (stationPath && !m_stationPass.isEmpty()) ||
        (!m_lanPass.isEmpty() && !loopback && (path.startsWith("/api/") || path.startsWith("/preview/")))) {
        static const QRegularExpression auth(R"rx(\r\nAuthorization:\s*Bearer\s+([^\r\n]+))rx", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression ppPass(R"rx(\r\nX-PP-Pass:\s*([^\r\n]+))rx", QRegularExpression::CaseInsensitiveOption);
        const QString head = QString::fromUtf8(request.left(bodyStart < 0 ? request.size() : bodyStart));
        const QRegularExpressionMatch am = auth.match(head);
        const QString presented = am.hasMatch() ? am.captured(1).trimmed() : query.value("token");
        const QRegularExpressionMatch pm = ppPass.match(head);
        const bool passOk = !m_stationPass.isEmpty() && pm.hasMatch() && pm.captured(1).trimmed() == m_stationPass;
        const bool tokenOk = !m_token.isEmpty() && presented == m_token;
        const bool lanOk = !m_lanPass.isEmpty() && (presented == m_lanPass || (pm.hasMatch() && pm.captured(1).trimmed() == m_lanPass));
        if (!(tokenOk || passOk || lanOk || (m_token.isEmpty() && loopback))) { sendError(socket, 401, "Unauthorized: password required"); return; }
    }

    // Raw (streaming) handlers take the socket over.
    {
        QMutexLocker lock(&m_routesMutex);
        for (const auto& r : m_raw) {
            const QString key = method + " ";
            if (r.first.startsWith(key) && path.startsWith(r.first.mid(key.size()))) {
                RawHandler h = r.second;
                lock.unlock();
                detach(socket);
                h(socket, path, query);
                return;
            }
        }
    }
    if (method == "GET" && !path.startsWith("/api/") && serveStatic(socket, path)) return;

    // Route matching: exact key first, then the segment trie ({param} segments bind into the query).
    Handler handler;
    {
        const QString key = matchRoute(method, path, &query);
        if (!key.isEmpty()) { QMutexLocker lock(&m_routesMutex); handler = m_routes.value(key); }
    }

    if (!handler) { sendError(socket, 404, "Not found: " + method + " " + path); return; }

    m_currentSocket = socket;
    const QJsonObject response = handler(body, query);
    m_currentSocket = nullptr;
    if (response.size() == 1 && response.value("_async").toBool()) return;   // reply arrives later
    if (response.contains("error") && response.contains("status")) {
        sendError(socket, response["status"].toInt(400), response["error"].toString());
    } else {
        sendResponse(socket, 200, response);
    }
}

void RestApiServer::sendResponse(QTcpSocket* socket, int statusCode, const QJsonObject& body) {
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray h;
    h += "HTTP/1.1 " + QByteArray::number(statusCode) + " OK"; h += CRLF;
    h += "Content-Type: application/json"; h += CRLF;
    h += "Access-Control-Allow-Origin: *"; h += CRLF;
    h += "Content-Length: " + QByteArray::number(json.size()); h += CRLF;
    h += "Connection: keep-alive"; h += CRLF; h += CRLF;
    socket->write(h + json);
    socket->flush();
}

void RestApiServer::sendResponse(QTcpSocket* socket, int statusCode, const QJsonArray& body) {
    QJsonObject o; o["items"] = body;
    sendResponse(socket, statusCode, o);
}

void RestApiServer::sendError(QTcpSocket* socket, int statusCode, const QString& message, bool closeAfter) {
    QJsonObject body; body["error"] = message; body["status"] = statusCode;
    const QByteArray json = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray h;
    h += "HTTP/1.1 " + QByteArray::number(statusCode) + " Error"; h += CRLF;
    h += "Content-Type: application/json"; h += CRLF;
    h += "Access-Control-Allow-Origin: *"; h += CRLF;
    h += "Content-Length: " + QByteArray::number(json.size()); h += CRLF;
    h += closeAfter ? "Connection: close" : "Connection: keep-alive"; h += CRLF; h += CRLF;
    socket->write(h + json);
    socket->flush();
    if (closeAfter) {
        // Drop the connection once the error has left: disconnectFromHost drains the write buffer first, and the
        // disconnected slot removes and deletes the socket (a synchronous disconnect does the same right here).
        socket->disconnectFromHost();
        if (socket->state() == QAbstractSocket::UnconnectedState && m_conns.remove(socket)) socket->deleteLater();
    }
}
