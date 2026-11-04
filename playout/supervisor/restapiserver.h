#ifndef RESTAPISERVER_H
#define RESTAPISERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QMutex>
#include <QTimer>
#include <QVector>
#include <functional>

class ChannelManager;

/**
 * RestApiServer — lightweight HTTP/JSON REST server for Supervisor UI control.
 *
 * Endpoints:
 *   GET  /api/v1/channels               → list all channels
 *   POST /api/v1/channels              → create channel
 *   GET  /api/v1/channels/{id}         → channel status
 *   POST /api/v1/channels/{id}/start   → start channel engine
 *   POST /api/v1/channels/{id}/stop    → stop channel engine
 *   POST /api/v1/channels/{id}/kill    → kill channel engine
 *   POST /api/v1/channels/{id}/playlist → push playlist
 *   POST /api/v1/channels/{id}/transport?action=play|pause|stop|skip → transport command
 *   GET  /api/v1/time                  → current master time
 *   GET  /api/v1/status                → supervisor health
 *
 * Hardening (perf-supervisor PS-9 / W15):
 *   - Routes are matched through a per-method segment trie compiled once from the route table (exact key first, then
 *     literal segments before `{param}` ones, with backtracking), not by splitting every registered key per request.
 *   - Keep-alive connections with no request in flight are closed after `idleTimeoutMs` (default 30 s).
 *   - A request whose headers exceed `maxHeaderBytes` (default 64 KB) gets 431 and the connection is closed; a JSON
 *     body over `maxBodyBytes` (default 256 MB) gets 413 (streaming uploads are dispatched on headers alone and are
 *     not limited here).
 *   - Every dispatch runs under the watchdog activity tag "rest:METHOD /path"; handlers that hold the event loop longer
 *     than `slowHandlerMs` (default 50 ms) are logged with their time.
 */
class RestApiServer : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QJsonObject& params, const QHash<QString, QString>& query)>;
    /// Raw handler: owns the socket from here on (streaming responses such as MJPEG).
    using RawHandler = std::function<void(QTcpSocket* socket, const QString& path, const QHash<QString, QString>& query)>;

    explicit RestApiServer(ChannelManager* channelMgr, QObject* parent = nullptr);
    ~RestApiServer();

    bool start(quint16 port = 8080);
    void stop();
    /// The bound port (0 when not listening); start(0) picks a free one.
    quint16 port() const { return m_server->serverPort(); }

    void registerRoute(const QString& method, const QString& path, Handler handler);
    /// Every registered JSON route as "METHOD /path" (sorted): the capability list the station agent publishes.
    QStringList routeKeys();
    /// Asynchronous JSON handler: call `reply(status, body)` once (from any later event-loop turn);
    /// the socket is held until then. status >= 400 sends body["error"] as the error message.
    using Reply = std::function<void(int status, const QJsonObject& body)>;
    using AsyncHandler = std::function<void(const QJsonObject& params, const QHash<QString, QString>& query, Reply reply)>;
    void registerAsync(const QString& method, const QString& path, AsyncHandler handler);
    void registerRaw(const QString& method, const QString& pathPrefix, RawHandler handler);
    /// Streaming upload: called as soon as the headers are in, with whatever body bytes arrived so far;
    /// the handler owns the socket and reads the remaining `contentLength - initial.size()` bytes itself.
    using UploadHandler = std::function<void(QTcpSocket* socket, const QString& path, const QHash<QString, QString>& query, qint64 contentLength, const QByteArray& initial)>;
    void registerUpload(const QString& method, const QString& pathPrefix, UploadHandler handler);
    void setEnableCors(bool on) { m_cors = on; }
    void setStaticRoot(const QString& dir) { m_staticRoot = dir; }   // serves files under / (web dashboard)
    /// When set, /api and /preview require `Authorization: Bearer <token>` or `?token=`; static files stay open.
    void setAuthToken(const QString& token) { m_token = token; }
    /// PlayPlay OBS compatibility: requests to /api/ (not /api/v1/) may authenticate with `X-PP-Pass: <password>`.
    void setStationPassword(const QString& pass) { m_stationPass = pass; }
    void setLanPassword(const QString& pass) { m_lanPass = pass; }
    /// Second listener with the same handlers (the station port).
    void copyRoutes(const RestApiServer& other) { QMutexLocker l(&m_routesMutex); m_routes = other.m_routes; m_trieDirty = true; m_raw = other.m_raw; m_uploads = other.m_uploads; m_staticRoot = other.m_staticRoot; m_token = other.m_token; m_stationPass = other.m_stationPass; m_lanPass = other.m_lanPass; }
    static void writeRawHeader(QTcpSocket* socket, int status, const QByteArray& contentType, bool keepAlive);

    // --- hardening knobs (PS-9); defaults are the production values ---------------------------------------------
    /// Idle keep-alive connections (no request being read, no async reply outstanding) are closed after this; 0 = never.
    void setIdleTimeoutMs(int ms);
    int idleTimeoutMs() const { return m_idleMs; }
    void setMaxHeaderBytes(int bytes) { m_maxHeaderBytes = bytes; }
    void setMaxBodyBytes(qint64 bytes) { m_maxBodyBytes = bytes; }
    /// Handlers (JSON, raw and upload dispatch, static files) that take longer than this are logged; 0 = off.
    void setSlowHandlerMs(int ms) { m_slowMs = ms; }
    /// Open connections this server still tracks (sockets handed to raw / upload handlers are not counted).
    int connectionCount() const { return m_conns.size(); }
    struct Stats { quint64 requests = 0, slow = 0, idleClosed = 0, headerTooLarge = 0, bodyTooLarge = 0; qint64 maxHandlerMs = 0; };
    Stats stats() const { return m_stats; }

    /// Route lookup through the trie (public for tests): the matched key ("METHOD /path/{id}") or "" and the params.
    QString matchRoute(const QString& method, const QString& path, QHash<QString, QString>* params);

signals:
    void requestReceived(const QString& method, const QString& path, const QJsonObject& body);

private slots:
    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void sweepIdle();

private:
    struct Conn {
        QByteArray buf;
        qint64 lastActiveMs = 0;   // monotonic; a request byte, a dispatch or an async reply
        int pending = 0;           // async replies outstanding: the connection is not idle while > 0
    };
    // Route trie: one root per method; a node holds literal children, at most one {param} child and the route key of
    // the handler that ends there (looked up in m_routes, so registration order and copyRoutes stay as they were).
    struct TrieNode {
        QHash<QString, int> kids;
        int param = -1;
        QString paramName;
        QString key;               // "METHOD /path" when a route ends here
    };
    QTcpServer* m_server;
    ChannelManager* m_channelMgr;   // may be null when routes are supplied externally
    QHash<QTcpSocket*, Conn> m_conns;
    QHash<QString, Handler> m_routes; // "METHOD /path" -> handler
    QHash<QString, int> m_trieRoots;  // method -> node
    QVector<TrieNode> m_trie;
    bool m_trieDirty = true;
    QTcpSocket* m_currentSocket = nullptr;   // socket of the request being dispatched (for async wrappers)
    QList<QPair<QString, RawHandler>> m_raw;   // "METHOD /prefix"
    QList<QPair<QString, UploadHandler>> m_uploads;
    bool authorized(const QByteArray& headers, const QHash<QString, QString>& query, bool loopback) const;
    QMutex m_routesMutex;
    bool m_cors = true;
    QString m_staticRoot;
    QString m_token;
    QString m_stationPass;
    QString m_lanPass;
    QTimer m_sweep;
    int m_idleMs = 30000;
    int m_maxHeaderBytes = 64 * 1024;
    qint64 m_maxBodyBytes = 256LL * 1024 * 1024;
    int m_slowMs = 50;
    Stats m_stats;
    bool serveStatic(QTcpSocket* socket, const QString& path);

    void setupDefaultRoutes();
    void rebuildTrieLocked();                       // m_routesMutex held
    int matchTrie(int node, const QStringList& segs, int i, QHash<QString, QString>& params) const;
    void detach(QTcpSocket* socket);                // a raw / upload handler owns the socket from here on
    void sendResponse(QTcpSocket* socket, int statusCode, const QJsonObject& body);
    void sendResponse(QTcpSocket* socket, int statusCode, const QJsonArray& body);
    void sendError(QTcpSocket* socket, int statusCode, const QString& message, bool closeAfter = false);
    void parseAndRoute(QTcpSocket* socket, const QByteArray& request);
};

#endif // RESTAPISERVER_H
