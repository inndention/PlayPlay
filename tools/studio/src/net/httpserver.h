#pragma once
// HTTP/1.1 server on the Qt event loop (ST-04, docs/plans/starmedia-studio.md §4.1 / §4.9). Hand-written because this Qt
// has no QtHttpServer. One QTcpServer, one Connection object per socket, everything non-blocking on the thread that owns
// the server (normally the main loop):
//   - keep-alive (HTTP/1.1 default, HTTP/1.0 with "Connection: keep-alive"), pipelined requests answered in order;
//     the next pipelined request is read only once most of the previous answer has left (write back-pressure), so a
//     client that pipelines and never reads cannot make the server buffer its answers;
//   - request bodies with Content-Length or "Transfer-Encoding: chunked" (uploads), "Expect: 100-continue", bodies over
//     a threshold spooled to a temp file and optionally hashed (sha256) while they arrive;
//   - responses from memory or streamed from a file (fileResponse(): Range / If-Range / If-None-Match, 206 / 416 / 304);
//   - async handlers: a handler gets a Responder it may keep and answer later from any thread. A request is answered
//     exactly once: a second send() is refused, a Responder dropped unanswered answers 500, a handler that does not
//     answer in time gets 503 (a late send() is then refused), and a Responder of an earlier request on the same
//     keep-alive connection can never answer a later one.
// Strict where request smuggling and DNS rebinding live: one request line format, no obs-fold, no whitespace before a
// header colon, Transfer-Encoding + Content-Length together refused, conflicting Content-Lengths refused, a Host
// header required on HTTP/1.1 and checked against ServerOptions::allowedHosts (421 for any other name, so a web page
// that rebinds its own host name to 127.0.0.1 cannot reach the API). Limits answer 408 / 413 / 414 / 431 / 503.
// Refusing a request whose body was not read answers and then closes with a half-close + linger, so the client still
// reads the answer instead of losing it to a TCP reset.
//
// Routing and token checks live in Router (router.h), which the server calls through an Admit function with the head of
// each request before any body byte is read: an unauthenticated client cannot make the server read or store a body.
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrlQuery>
#include <functional>
#include <memory>
#include <optional>

class QTcpServer;

namespace studio::net {

/// Header fields in arrival order. Lookups are case-insensitive; request header names are stored lower-case.
class Headers {
public:
    void add(const QByteArray& name, const QByteArray& value) { m_list.append({name, value}); }
    void set(const QByteArray& name, const QByteArray& value);       // replaces every field of that name
    void remove(QByteArrayView name);
    QByteArray value(QByteArrayView name, const QByteArray& def = {}) const;   // the first one
    QList<QByteArray> values(QByteArrayView name) const;
    bool contains(QByteArrayView name) const;
    /// True when a comma-separated field (Connection, Upgrade ...) lists `token`, case-insensitive.
    bool hasToken(QByteArrayView name, QByteArrayView token) const;
    const QList<QPair<QByteArray, QByteArray>>& list() const { return m_list; }
    qsizetype size() const { return m_list.size(); }

private:
    QList<QPair<QByteArray, QByteArray>> m_list;
};

struct RequestHead {
    QByteArray method, target;             // as sent
    int minor = 1;                         // HTTP/1.<minor>
    Headers headers;                       // names lower-case
};

/// Parses a request head (request line and header lines, without the blank line that ends it). 0 on success, else the
/// status to refuse it with (400, 414, 431, 505) and why. Shared with the WS hub's handshake check.
int parseRequestHead(QByteArrayView head, RequestHead* out, QString* why, int maxHeaders = 100, int maxTargetBytes = 8192);
/// Splits an origin-form or absolute-form target into its raw path and raw query (fragment dropped). False for any
/// other form, an absolute-form authority that does not parse (userinfo, a bad port, an empty host) or a malformed
/// percent escape in the path; `authority` gets the host of an absolute-form target (empty for origin-form).
bool splitTarget(const QByteArray& target, QByteArray* rawPath, QByteArray* rawQuery, QString* authority = nullptr);
/// The host name of a Host header value ("127.0.0.1:7760" -> "127.0.0.1", "[::1]:80" -> "::1", "LocalHost." ->
/// "localhost"); empty when malformed.
QString hostName(QByteArrayView hostHeader);
QByteArray reasonPhrase(int status);
/// RFC 9110 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT").
QByteArray httpDate(qint64 epochMs);

struct Request {
    quint64 id = 0;                        // per server, for logs
    QByteArray method;
    QByteArray target;                     // as sent
    QByteArray rawPath;                    // percent-encoded path of the target
    QString path;                          // decoded (logs, simple handlers); the router decodes segment by segment
    QUrlQuery query;
    int minor = 1;
    Headers headers;
    QHostAddress peer;
    quint16 peerPort = 0;
    QHash<QString, QString> params;        // router captures ({id} -> value), decoded
    bool authenticated = false;            // a valid token was presented (set by the router)

    // The body. Small bodies are in `body`; a route that allows spooling gets bodies over the threshold in `bodyFile`
    // (a temp file removed after the response or when the client goes away: rename it inside the handler, before
    // answering, to keep it).
    QByteArray body;
    QString bodyFile;
    qint64 bodySize = 0;
    QByteArray bodySha256;                 // hex, when the route asked for it (RouteOptions::hashBody)
    bool chunked = false;

    QByteArray header(QByteArrayView name) const { return headers.value(name); }
    QString param(const QString& name) const { return params.value(name); }
    QString queryValue(const QString& name) const;
    bool hasQuery(const QString& name) const { return query.hasQueryItem(name); }
    /// The body (in memory or spooled, at most `max` bytes) as a JSON object; an empty body is {}. ok=false otherwise.
    QJsonObject jsonObject(QString* error = nullptr, bool* ok = nullptr, qint64 max = 64LL * 1024 * 1024) const;
    /// The body whether in memory or spooled; ok=false when a spooled body is larger than `max` or unreadable.
    QByteArray readBody(qint64 max = 64LL * 1024 * 1024, bool* ok = nullptr) const;
};

struct Response {
    int status = 200;
    Headers headers;
    QByteArray body;
    QString filePath;                      // set: the body is bytes [fileOffset, fileOffset + fileLength) of this file
    qint64 fileOffset = 0, fileLength = 0;
    bool close = false;                    // close the connection after this response

    static Response json(int status, const QJsonValue& v);
    static Response json(const QJsonObject& o) { return json(200, o); }
    /// {"ok": false, "error": message} (the shape every Studio route answers errors with).
    static Response error(int status, const QString& message);
    static Response text(int status, const QString& text, const QByteArray& type = "text/plain; charset=utf-8");
    static Response bytes(int status, const QByteArray& type, const QByteArray& body);
    static Response empty(int status = 204);
    Response& header(const QByteArray& name, const QByteArray& value) {
        headers.set(name, value);
        return *this;
    }
};

/// A file response honouring Range (one range: "bytes=a-b", "a-", "-n"; several ranges or bad syntax are ignored and
/// the whole file is served, as RFC 9110 allows), If-Range (strong ETag only) and If-None-Match (304). The ETag is
/// `etag` when given (an artifact's sha256), else "<size>-<mtime>". 404 when `path` is not a file, 416 with
/// "Content-Range: bytes */<size>" for a range past the end. The file is streamed by the connection, never read whole.
Response fileResponse(const Request& rq, const QString& path, const QByteArray& mime, const QByteArray& etag = {});

namespace detail {
struct ResponderState;
class Connection;
}

/// The one way a handler answers. Copyable; every copy answers the same request. Thread-safe: send() from a worker
/// thread is delivered on the connection's thread.
class Responder {
public:
    Responder() = default;
    /// True when this was the answer (the first one, while the request still waited and its connection was open);
    /// false for a second answer, after the handler timeout, or when the client went away.
    bool send(Response r) const;
    bool answered() const;
    /// Not answered yet and the connection is still open (a hint for long work: the client may have left).
    bool alive() const;
    const Request& request() const;
    std::shared_ptr<const Request> requestPtr() const;

private:
    friend class detail::Connection;
    explicit Responder(std::shared_ptr<detail::ResponderState> d) : d(std::move(d)) {}
    std::shared_ptr<detail::ResponderState> d;
};

using Handler = std::function<void(const Request&, Responder)>;

/// What the server does with a request, decided from its head alone (Router::admit).
struct Admission {
    std::optional<Response> refuse;        // answer this right away (401, 404, 405 ...); the body is never stored
    Handler handler;                       // else: read the body, then call this
    qint64 maxBodyBytes = -1;              // -1: ServerOptions::maxBodyBytes
    bool spool = false;                    // bodies over spoolThresholdBytes go to a temp file
    bool hashBody = false;                 // Request::bodySha256
    int timeoutMs = -1;                    // handler deadline; -1: ServerOptions::handlerTimeoutMs, 0: none
};
using Admit = std::function<Admission(Request&)>;

struct ServerOptions {
    int maxHeadBytes = 16 * 1024;          // request line + headers (431)
    int maxHeaders = 100;                  // header fields (431)
    int maxTargetBytes = 8 * 1024;         // request target (414)
    qint64 maxBodyBytes = 8LL * 1024 * 1024;       // default body limit (413); routes may raise it
    qint64 spoolThresholdBytes = 1024 * 1024;      // bodies over this go to a temp file (routes that allow it)
    QString spoolDir;                      // empty: <temp>/PlayPlayStudio-spool
    qint64 drainBytes = 64 * 1024;         // a refused request's body up to this size is read so the connection stays
    int headTimeoutMs = 10000;             // from the first byte of a head to its end (slow-loris guard; 408)
    int idleTimeoutMs = 30000;             // keep-alive: no new request by then closes quietly
    int bodyTimeoutMs = 30000;             // no body bytes for this long (408)
    int handlerTimeoutMs = 60000;          // default handler deadline (503)
    int writeTimeoutMs = 60000;            // no write progress for this long aborts
    int lingerMs = 2000;                   // after a refusal with an unread body: read and discard this long, then close
    int maxConnections = 256;              // more get a canned 503 and are closed
    int maxRequestsPerConnection = 1000;
    qint64 socketReadBufferBytes = 1024 * 1024;    // per socket: bounds memory, gives TCP back-pressure on uploads
    QStringList allowedHosts;              // Host names served (lower-case); empty = any. Others get 421.
};

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(QObject* parent = nullptr);
    ~HttpServer() override;

    void setOptions(const ServerOptions& o) { m_opt = o; }   // applies to connections accepted afterwards
    const ServerOptions& options() const { return m_opt; }
    void setAdmit(Admit a) { m_admit = std::move(a); }

    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    /// Stops listening and aborts every connection (pending handlers' answers are then refused).
    void close();
    bool isListening() const;
    quint16 port() const;

    struct Stats {
        quint64 connections = 0;           // accepted
        quint64 refusedConnections = 0;    // over maxConnections
        quint64 requests = 0;
        quint64 reused = 0;                // requests on a connection that already served one (keep-alive)
        quint64 responses = 0;
        quint64 protocolErrors = 0;        // 400 / 408 / 413 / 414 / 431 / 505 from the parser and limits
        quint64 handlerTimeouts = 0;
        quint64 unanswered = 0;            // responders dropped without an answer (500)
        quint64 bytesIn = 0, bytesOut = 0;
        int open = 0;
    };
    const Stats& stats() const { return m_stats; }
    QJsonObject statsJson() const;

private:
    friend class detail::Connection;
    void onNewConnection();

    QTcpServer* m_tcp = nullptr;
    ServerOptions m_opt;
    Admit m_admit;
    Stats m_stats;
    quint64 m_nextRequestId = 0;
    QSet<detail::Connection*> m_conns;     // open connections (children; each removes itself when destroyed)
};

} // namespace studio::net
