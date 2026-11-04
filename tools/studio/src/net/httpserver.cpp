#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>                      // shutdown(SD_SEND) for the half-close; before any <windows.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <sys/socket.h>
#endif

#include "net/httpserver.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>

#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"

namespace studio::net {

namespace {

constexpr qint64 kFileChunk = 256 * 1024;  // file bodies are read and queued in pieces of this size ...
constexpr qint64 kWriteHighWater = 1024 * 1024;   // ... while less than this waits in the socket's write buffer
// A pipelined request is read only while less than this waits unsent (and resumes below half of it): a client that
// pipelines and never reads cannot make the server buffer every answer.
constexpr qint64 kPipelineHighWater = 256 * 1024;
constexpr qint64 kLingerDiscardMax = 64LL * 1024 * 1024;   // a lingering client sending more than this is cut off
constexpr int kMaxChunkLine = 1024;        // chunk-size line incl. extensions

bool ieq(QByteArrayView a, QByteArrayView b) { return a.compare(b, Qt::CaseInsensitive) == 0; }

// RFC 9110 tchar.
bool isToken(QByteArrayView s) {
    if (s.isEmpty()) return false;
    for (char c : s) {
        const uchar u = uchar(c);
        if ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) continue;
        if (!std::strchr("!#$%&'*+-.^_`|~", c) || c == 0) return false;
    }
    return true;
}

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Digits only, no sign, no spaces; false on overflow past 2^62.
bool parseDecimal(QByteArrayView s, qint64* out) {
    if (s.isEmpty() || s.size() > 18) return false;
    qint64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

QByteArray jsonBytes(const QJsonValue& v) {
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    return json::compact(v);
}

// Strips CR / LF / NUL from an outgoing header value, so a value built from user data cannot add header lines.
QByteArray safeHeaderValue(const QByteArray& v) {
    if (!v.contains('\r') && !v.contains('\n') && !v.contains('\0')) return v;
    QByteArray out = v;
    out.replace('\r', ' ').replace('\n', ' ').replace('\0', ' ');
    return out;
}

// Weak comparison of entity tags (If-None-Match): W/ ignored.
QByteArrayView opaqueTag(QByteArrayView t) {
    t = t.trimmed();
    if (t.startsWith("W/")) t = t.sliced(2);
    return t;
}
bool etagListMatches(QByteArrayView list, QByteArrayView etag) {
    const QByteArrayView mine = opaqueTag(etag);
    for (const QByteArray& part : list.toByteArray().split(','))
        if (opaqueTag(part) == mine) return true;
    return false;
}

enum class RangeResult { Ignore, Ok, Unsatisfiable };
// One byte range of a representation of `size` bytes (RFC 9110 §14.1.2).
RangeResult parseRange(QByteArrayView value, qint64 size, qint64* first, qint64* last) {
    value = value.trimmed();
    if (value.size() < 6 || !ieq(value.left(6), "bytes=")) return RangeResult::Ignore;
    QByteArrayView spec = value.sliced(6).trimmed();
    if (spec.contains(',')) return RangeResult::Ignore;          // several ranges: the whole representation is fine
    const qsizetype dash = spec.indexOf('-');
    if (dash < 0) return RangeResult::Ignore;
    const QByteArrayView a = spec.left(dash).trimmed(), b = spec.sliced(dash + 1).trimmed();
    if (a.isEmpty()) {                                           // suffix: the last n bytes
        qint64 n = 0;
        if (!parseDecimal(b, &n)) return RangeResult::Ignore;
        if (n == 0 || size == 0) return RangeResult::Unsatisfiable;
        *first = qMax<qint64>(0, size - n);
        *last = size - 1;
        return RangeResult::Ok;
    }
    qint64 f = 0, l = size - 1;
    if (!parseDecimal(a, &f)) return RangeResult::Ignore;
    if (!b.isEmpty() && !parseDecimal(b, &l)) return RangeResult::Ignore;
    if (!b.isEmpty() && l < f) return RangeResult::Ignore;     // invalid spec
    if (f >= size) return RangeResult::Unsatisfiable;
    *first = f;
    *last = qMin(l, size - 1);
    return RangeResult::Ok;
}

// Half-closes the sending side so the peer sees the end of our answer while we keep reading what it still sends.
bool halfClose(QTcpSocket* s) {
    const qintptr fd = s->socketDescriptor();
    if (fd == -1) return false;
#ifdef _WIN32
    return ::shutdown(SOCKET(fd), SD_SEND) == 0;
#else
    return ::shutdown(int(fd), SHUT_WR) == 0;
#endif
}

} // namespace

// ---------------- Headers / parsing helpers ----------------

void Headers::set(const QByteArray& name, const QByteArray& value) {
    remove(name);
    add(name, value);
}

void Headers::remove(QByteArrayView name) {
    m_list.removeIf([&](const QPair<QByteArray, QByteArray>& p) { return ieq(p.first, name); });
}

QByteArray Headers::value(QByteArrayView name, const QByteArray& def) const {
    for (const auto& p : m_list)
        if (ieq(p.first, name)) return p.second;
    return def;
}

QList<QByteArray> Headers::values(QByteArrayView name) const {
    QList<QByteArray> out;
    for (const auto& p : m_list)
        if (ieq(p.first, name)) out << p.second;
    return out;
}

bool Headers::contains(QByteArrayView name) const {
    for (const auto& p : m_list)
        if (ieq(p.first, name)) return true;
    return false;
}

bool Headers::hasToken(QByteArrayView name, QByteArrayView token) const {
    for (const auto& p : m_list) {
        if (!ieq(p.first, name)) continue;
        for (const QByteArray& t : p.second.split(','))
            if (ieq(QByteArrayView(t).trimmed(), token)) return true;
    }
    return false;
}

int parseRequestHead(QByteArrayView head, RequestHead* out, QString* why, int maxHeaders, int maxTargetBytes) {
    auto bad = [why](int code, const char* reason) {
        if (why) *why = QString::fromLatin1(reason);
        return code;
    };
    const qsizetype eol = head.indexOf("\r\n");
    const QByteArrayView line = eol < 0 ? head : head.left(eol);
    // method SP request-target SP HTTP-version, single spaces (RFC 9112 §3).
    const qsizetype sp1 = line.indexOf(' '), sp2 = line.lastIndexOf(' ');
    if (sp1 <= 0 || sp2 <= sp1 + 1) return bad(400, "bad request line");
    const QByteArrayView method = line.left(sp1), target = line.sliced(sp1 + 1, sp2 - sp1 - 1), version = line.sliced(sp2 + 1);
    if (method.size() > 32 || !isToken(method)) return bad(400, "bad method");
    if (target.size() > maxTargetBytes) return bad(414, "request target too long");
    for (char c : target)
        if (uchar(c) <= 0x20 || uchar(c) == 0x7f) return bad(400, "bad character in the request target");
    if (version == "HTTP/1.1") out->minor = 1;
    else if (version == "HTTP/1.0") out->minor = 0;
    else if (version.size() == 8 && version.startsWith("HTTP/") && version[6] == '.' && version[5] >= '0' && version[5] <= '9')
        return bad(505, "HTTP version not supported");
    else return bad(400, "bad HTTP version");
    out->method = method.toByteArray();
    out->target = target.toByteArray();

    int count = 0;
    qsizetype pos = eol < 0 ? head.size() : eol + 2;
    while (pos < head.size()) {
        qsizetype e = head.indexOf("\r\n", pos);
        if (e < 0) e = head.size();
        const QByteArrayView l = head.sliced(pos, e - pos);
        pos = e + 2;
        if (l.isEmpty()) return bad(400, "empty header line");
        if (l[0] == ' ' || l[0] == '\t') return bad(400, "obsolete header line folding");
        const qsizetype colon = l.indexOf(':');
        if (colon <= 0) return bad(400, "header line without a name");
        const QByteArrayView name = l.left(colon);
        if (!isToken(name)) return bad(400, "bad header name");   // also "Name :" (whitespace before the colon)
        const QByteArrayView value = l.sliced(colon + 1).trimmed();
        for (char c : value)
            if ((uchar(c) < 0x20 && c != '\t') || uchar(c) == 0x7f) return bad(400, "bad character in a header value");
        if (++count > maxHeaders) return bad(431, "too many header fields");
        out->headers.add(name.toByteArray().toLower(), value.toByteArray());
    }
    return 0;
}

bool splitTarget(const QByteArray& target, QByteArray* rawPath, QByteArray* rawQuery, QString* authority) {
    QByteArray t = target;
    if (const qsizetype hash = t.indexOf('#'); hash >= 0) t.truncate(hash);
    if (!t.startsWith('/')) {
        // absolute-form (RFC 9112 §3.2.2): http://host[:port]/path?query
        const QByteArray lower = t.left(8).toLower();
        const qsizetype schemeEnd = lower.startsWith("http://") ? 7 : lower.startsWith("https://") ? 8 : -1;
        if (schemeEnd < 0) return false;
        const qsizetype slash = t.indexOf('/', schemeEnd);
        const qsizetype q = t.indexOf('?', schemeEnd);
        const qsizetype end = slash >= 0 ? (q >= 0 ? qMin(slash, q) : slash) : (q >= 0 ? q : t.size());
        const QByteArray auth = t.mid(schemeEnd, end - schemeEnd);
        if (auth.isEmpty() || auth.contains('@')) return false;
        // An authority that does not parse (`evil.example:abc`) is refused, never treated as absent: absent would let
        // the Host header stand in for a target that named another host.
        const QString host = hostName(auth);
        if (host.isEmpty()) return false;
        if (authority) *authority = host;
        t = t.mid(end);
        if (!t.startsWith('/')) t.prepend('/');
    }
    const qsizetype q = t.indexOf('?');
    *rawPath = q < 0 ? t : t.left(q);
    *rawQuery = q < 0 ? QByteArray() : t.mid(q + 1);
    for (qsizetype i = 0; i < rawPath->size(); ++i)
        if (rawPath->at(i) == '%' && (i + 2 >= rawPath->size() || !isHex(rawPath->at(i + 1)) || !isHex(rawPath->at(i + 2)))) return false;
    return true;
}

QString hostName(QByteArrayView h) {
    h = h.trimmed();
    if (h.isEmpty()) return {};
    QByteArrayView host;
    if (h.startsWith('[')) {
        const qsizetype close = h.indexOf(']');
        if (close < 0) return {};
        host = h.sliced(1, close - 1);
        const QByteArrayView rest = h.sliced(close + 1);
        if (!rest.isEmpty() && (!rest.startsWith(':') || !std::all_of(rest.begin() + 1, rest.end(), [](char c) { return c >= '0' && c <= '9'; })))
            return {};
    } else {
        const qsizetype colon = h.indexOf(':');
        if (colon >= 0) {
            if (h.indexOf(':', colon + 1) >= 0) return {};     // a bare IPv6 address must be bracketed
            const QByteArrayView port = h.sliced(colon + 1);
            if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return {};
            host = h.left(colon);
        } else {
            host = h;
        }
    }
    if (host.endsWith('.')) host.chop(1);
    if (host.isEmpty()) return {};
    for (char c : host)
        if (uchar(c) <= 0x20 || c == '/' || c == '\\' || c == '@' || uchar(c) >= 0x7f) return {};
    return QString::fromLatin1(host).toLower();
}

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default: return status >= 500 ? "Server Error" : status >= 400 ? "Client Error" : "Status";
    }
}

QByteArray httpDate(qint64 epochMs) {
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC);
    return QLocale::c().toString(t, QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'")).toLatin1();
}

// ---------------- Request / Response ----------------

QString Request::queryValue(const QString& name) const { return query.queryItemValue(name, QUrl::FullyDecoded); }

QByteArray Request::readBody(qint64 max, bool* ok) const {
    if (ok) *ok = true;
    if (bodyFile.isEmpty()) return body;
    QFile f(bodyFile);
    if (f.size() > max || !f.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    return f.readAll();
}

QJsonObject Request::jsonObject(QString* error, bool* ok, qint64 max) const {
    bool readOk = true;
    const QByteArray b = readBody(max, &readOk);
    if (ok) *ok = false;
    if (!readOk) {
        if (error) *error = QStringLiteral("request body too large");
        return {};
    }
    if (b.trimmed().isEmpty()) {
        if (ok) *ok = true;
        return {};
    }
    QString err;
    const QJsonValue v = json::parse(b, &err, max);
    if (!v.isObject()) {
        if (error) *error = err.isEmpty() ? QStringLiteral("the body is not a JSON object") : err;
        return {};
    }
    if (ok) *ok = true;
    return v.toObject();
}

Response Response::json(int status, const QJsonValue& v) {
    Response r;
    r.status = status;
    r.body = jsonBytes(v);
    r.headers.set("Content-Type", "application/json");
    return r;
}

Response Response::error(int status, const QString& message) {
    QJsonObject o;
    o["ok"] = false;
    o["error"] = message;
    return json(status, o);
}

Response Response::text(int status, const QString& text, const QByteArray& type) { return bytes(status, type, text.toUtf8()); }

Response Response::bytes(int status, const QByteArray& type, const QByteArray& body) {
    Response r;
    r.status = status;
    r.body = body;
    r.headers.set("Content-Type", type);
    return r;
}

Response Response::empty(int status) {
    Response r;
    r.status = status;
    return r;
}

Response fileResponse(const Request& rq, const QString& path, const QByteArray& mime, const QByteArray& etagIn) {
    const QFileInfo fi(path);
    if (!fi.isFile()) return Response::error(404, QStringLiteral("not found"));
    const qint64 size = fi.size();
    const qint64 mtime = fi.lastModified(QTimeZone::UTC).toMSecsSinceEpoch();
    QByteArray etag = etagIn;
    if (etag.isEmpty()) etag = '"' + QByteArray::number(size, 16) + '-' + QByteArray::number(mtime, 16) + '"';
    else if (!etag.startsWith('"') && !etag.startsWith("W/")) etag = '"' + etag + '"';

    Response r;
    r.headers.set("Content-Type", mime.isEmpty() ? QByteArray("application/octet-stream") : mime);
    r.headers.set("Accept-Ranges", "bytes");
    r.headers.set("ETag", etag);
    r.headers.set("Last-Modified", httpDate(mtime));
    r.headers.set("Cache-Control", "private, no-cache");      // cache, but revalidate with the ETag
    const QByteArray inm = rq.header("if-none-match");
    if (!inm.isEmpty() && (inm.trimmed() == "*" || etagListMatches(inm, etag))) {
        r.status = 304;
        return r;
    }
    r.status = 200;
    r.filePath = path;
    r.fileOffset = 0;
    r.fileLength = size;
    const QByteArray range = rq.header("range");
    if (range.isEmpty() || (rq.method != "GET" && rq.method != "HEAD")) return r;
    // If-Range: a range only of the representation the client already has (strong comparison; a weak tag never matches).
    const QByteArray ifRange = rq.header("if-range").trimmed();
    if (!ifRange.isEmpty() && (ifRange.startsWith("W/") || ifRange != etag)) return r;
    qint64 first = 0, last = 0;
    switch (parseRange(range, size, &first, &last)) {
    case RangeResult::Ignore: return r;
    case RangeResult::Unsatisfiable: {
        Response e = Response::error(416, QStringLiteral("range not satisfiable"));
        e.headers.set("Content-Range", "bytes */" + QByteArray::number(size));
        e.headers.set("Accept-Ranges", "bytes");
        return e;
    }
    case RangeResult::Ok: break;
    }
    r.status = 206;
    r.fileOffset = first;
    r.fileLength = last - first + 1;
    r.headers.set("Content-Range", "bytes " + QByteArray::number(first) + '-' + QByteArray::number(last) + '/' + QByteArray::number(size));
    return r;
}

// ---------------- Connection ----------------

namespace detail {

// Lets responders on any thread reach their connection. `conn` is cleared on the connection's thread (disconnect,
// destruction) under the mutex, so a responder that finds it set may post to it.
struct ConnLink {
    QMutex mu;
    Connection* conn = nullptr;
};

struct ResponderState {
    std::shared_ptr<ConnLink> link;
    quint64 seq = 0;                       // the request this responder belongs to on its connection
    std::shared_ptr<const Request> request;
    std::atomic<bool> done{false};
    ~ResponderState();
};

bool deliver(const std::shared_ptr<ConnLink>& link, quint64 seq, Response r, bool unanswered = false);

class Connection : public QObject {
public:
    Connection(HttpServer* server, QTcpSocket* s);
    ~Connection() override;
    // Called on the connection's thread with a handler's answer. False when it no longer applies.
    bool finish(quint64 seq, Response r, bool unanswered);
    void abort() { m_sock->abort(); }

private:
    enum class St { Head, Body, Drain, Waiting, Writing, Closing, Linger, Closed };
    enum class After { Keep, Close, Linger };

    void onReadyRead();
    void onBytesWritten();
    void onDisconnected();
    void onTimeout();
    void pump();
    bool parseHead();
    void beginRequest(RequestHead&& h);
    bool readBody();
    bool appendBody(QByteArrayView data);
    void completeBody();
    void dispatch();
    void refuse(int status, const QString& why, After after);
    void writeResponse(Response r, After after);
    void feedFile();
    void responseQueued();
    void startLinger();
    void endRequest();
    void arm(int ms) {
        if (ms > 0) m_timer.start(ms);
        else m_timer.stop();
    }

    HttpServer* m_server;
    QTcpSocket* m_sock;
    const ServerOptions m_opt;
    std::shared_ptr<ConnLink> m_link;
    QTimer m_timer;
    St m_st = St::Head;
    QByteArray m_in;
    bool m_headTimerArmed = false;
    bool m_writePaused = false;            // the last answer waits unsent: no next request until the client reads
    int m_requests = 0;
    quint64 m_seq = 0;

    // the request being read / handled
    std::shared_ptr<Request> m_req;
    Admission m_adm;
    bool m_keepAlive = true;
    bool m_chunked = false;
    enum class Ck { Size, Data, DataEnd, Trailer } m_ck = Ck::Size;
    qint64 m_bodyLeft = 0;                 // Content-Length bytes still to come (also while draining)
    qint64 m_chunkLeft = 0;
    qint64 m_bodyLimit = 0;
    int m_trailerBytes = 0;
    std::unique_ptr<QFile> m_spool;
    std::unique_ptr<QCryptographicHash> m_hash;
    std::optional<Response> m_heldRefusal; // answered once a small refused body is drained
    std::weak_ptr<ResponderState> m_responder;
    QElapsedTimer m_clock;

    // the response being written
    std::unique_ptr<QFile> m_file;
    qint64 m_fileLeft = 0;
    After m_after = After::Keep;
    bool m_lingerPending = false;          // half-close once the write buffer is empty
    int m_status = 0;
    qint64 m_bytesOut = 0;
    qint64 m_lingerDiscarded = 0;
    QElapsedTimer m_lingerClock;
};

ResponderState::~ResponderState() {
    if (link && !done.exchange(true)) deliver(link, seq, Response::error(500, QStringLiteral("the handler did not answer")), true);
}

bool deliver(const std::shared_ptr<ConnLink>& link, quint64 seq, Response r, bool unanswered) {
    QMutexLocker lock(&link->mu);
    Connection* c = link->conn;
    if (!c) return false;
    if (c->thread() == QThread::currentThread()) {
        lock.unlock();                     // the connection dies only on this thread, so not during this call
        return c->finish(seq, std::move(r), unanswered);
    }
    // Posted while the mutex keeps the connection alive; if it is destroyed before the event runs, Qt drops the event.
    QMetaObject::invokeMethod(c, [c, seq, r = std::move(r), unanswered]() mutable { c->finish(seq, std::move(r), unanswered); },
                              Qt::QueuedConnection);
    return true;
}

Connection::Connection(HttpServer* server, QTcpSocket* s)
    : QObject(server), m_server(server), m_sock(s), m_opt(server->options()), m_link(std::make_shared<ConnLink>()) {
    m_link->conn = this;
    m_server->m_conns.insert(this);
    s->setParent(this);
    s->setReadBufferSize(m_opt.socketReadBufferBytes);
    s->setSocketOption(QAbstractSocket::LowDelayOption, 1);     // no Nagle stalls between keep-alive answers
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Connection::onTimeout);
    connect(s, &QTcpSocket::readyRead, this, &Connection::onReadyRead);
    connect(s, &QTcpSocket::bytesWritten, this, &Connection::onBytesWritten);
    connect(s, &QTcpSocket::disconnected, this, &Connection::onDisconnected);
    ++m_server->m_stats.open;
    ++m_server->m_stats.connections;
    arm(m_opt.idleTimeoutMs);
    if (s->bytesAvailable() > 0) QMetaObject::invokeMethod(this, [this]() { onReadyRead(); }, Qt::QueuedConnection);
}

Connection::~Connection() {
    {
        QMutexLocker lock(&m_link->mu);
        m_link->conn = nullptr;
    }
    endRequest();
    --m_server->m_stats.open;
    m_server->m_conns.remove(this);
}

void Connection::onDisconnected() {
    {
        QMutexLocker lock(&m_link->mu);
        m_link->conn = nullptr;            // pending responders now get false at once
    }
    m_st = St::Closed;
    m_timer.stop();
    endRequest();
    deleteLater();
}

void Connection::endRequest() {
    if (m_spool) {
        m_spool->close();
        m_spool.reset();
    }
    if (m_req && !m_req->bodyFile.isEmpty()) QFile::remove(m_req->bodyFile);   // fails quietly when the handler moved it
    m_file.reset();
    m_hash.reset();
    m_heldRefusal.reset();
}

void Connection::onReadyRead() {
    switch (m_st) {
    case St::Head:
    case St::Body:
    case St::Drain: pump(); break;
    case St::Linger: {
        const qint64 n = m_sock->skip(m_sock->bytesAvailable());
        m_server->m_stats.bytesIn += quint64(qMax<qint64>(0, n));
        m_lingerDiscarded += qMax<qint64>(0, n);
        if (m_lingerDiscarded > kLingerDiscardMax || m_lingerClock.elapsed() > 3 * m_opt.lingerMs) m_sock->abort();
        break;
    }
    case St::Closing:
    case St::Closed: m_sock->skip(m_sock->bytesAvailable()); break;
    case St::Waiting:
    case St::Writing: break;               // the next pipelined request waits in the socket buffer (bounded)
    }
}

void Connection::pump() {
    if (m_st != St::Head && m_st != St::Body && m_st != St::Drain) return;
    if (m_writePaused) return;             // what the client sent stays in the (bounded) socket buffer: TCP back-pressure
    if (m_sock->bytesAvailable() > 0) {
        const QByteArray chunk = m_sock->readAll();
        m_server->m_stats.bytesIn += quint64(chunk.size());
        m_in += chunk;
    }
    for (;;) {
        bool more = false;
        if (m_st == St::Head) more = parseHead();
        else if (m_st == St::Body) more = readBody();
        else if (m_st == St::Drain) {
            const qint64 take = qMin<qint64>(m_bodyLeft, m_in.size());
            m_in.remove(0, take);
            m_bodyLeft -= take;
            if (m_bodyLeft > 0) {
                arm(m_opt.bodyTimeoutMs);
                return;
            }
            Response r = std::move(*m_heldRefusal);
            m_heldRefusal.reset();
            writeResponse(std::move(r), After::Keep);
            return;
        }
        if (!more) return;
    }
}

bool Connection::parseHead() {
    while (m_in.startsWith("\r\n")) m_in.remove(0, 2);   // RFC 9112 §2.2: empty lines before a request line are ignored
    if (m_in.isEmpty()) return false;
    const qsizetype end = m_in.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_in.size() > m_opt.maxHeadBytes) {
            refuse(431, QStringLiteral("request head too large"), After::Linger);
            return false;
        }
        if (!m_headTimerArmed) {           // counted from the first byte, never re-armed by a trickle (slow loris)
            m_headTimerArmed = true;
            arm(m_opt.headTimeoutMs);
        }
        return false;
    }
    if (end > m_opt.maxHeadBytes) {
        refuse(431, QStringLiteral("request head too large"), After::Linger);
        return false;
    }
    RequestHead h;
    QString why;
    const int code = parseRequestHead(QByteArrayView(m_in).left(end), &h, &why, m_opt.maxHeaders, m_opt.maxTargetBytes);
    m_in.remove(0, end + 4);
    m_headTimerArmed = false;
    if (code) {
        refuse(code, why, After::Linger);
        return false;
    }
    beginRequest(std::move(h));
    return m_st == St::Body || m_st == St::Drain;
}

void Connection::beginRequest(RequestHead&& h) {
    m_clock.start();
    ++m_requests;
    ++m_server->m_stats.requests;
    if (m_requests > 1) ++m_server->m_stats.reused;
    auto rq = std::make_shared<Request>();
    rq->id = ++m_server->m_nextRequestId;
    rq->method = std::move(h.method);
    rq->target = std::move(h.target);
    rq->minor = h.minor;
    rq->headers = std::move(h.headers);
    rq->peer = m_sock->peerAddress();
    rq->peerPort = m_sock->peerPort();
    m_req = rq;
    m_chunked = false;
    m_bodyLeft = 0;
    m_keepAlive = rq->minor == 1 ? !rq->headers.hasToken("connection", "close") : rq->headers.hasToken("connection", "keep-alive");
    if (m_requests >= m_opt.maxRequestsPerConnection) m_keepAlive = false;

    QByteArray rawQuery;
    QString authority;
    if (!splitTarget(rq->target, &rq->rawPath, &rawQuery, &authority))
        return refuse(400, QStringLiteral("bad request target"), After::Linger);
    rq->path = QUrl::fromPercentEncoding(rq->rawPath);
    if (rq->path.contains(QChar(0))) return refuse(400, QStringLiteral("bad request target"), After::Linger);
    rq->query.setQuery(QString::fromUtf8(rawQuery));

    // Host: required on HTTP/1.1, one only, and a name this server serves (DNS-rebinding guard).
    const QList<QByteArray> hosts = rq->headers.values("host");
    if (hosts.size() > 1) return refuse(400, QStringLiteral("more than one Host header"), After::Linger);
    if (hosts.isEmpty() && authority.isEmpty() && rq->minor == 1) return refuse(400, QStringLiteral("missing Host header"), After::Linger);
    QString host = authority;
    if (host.isEmpty() && !hosts.isEmpty()) {
        host = hostName(hosts.first());
        if (host.isEmpty()) return refuse(400, QStringLiteral("bad Host header"), After::Linger);
    }
    if (!m_opt.allowedHosts.isEmpty() && !host.isEmpty() && !m_opt.allowedHosts.contains(host))
        return refuse(421, QStringLiteral("this server does not serve host '%1'").arg(host.left(100)), After::Linger);

    // Framing (RFC 9112 §6): chunked or Content-Length, never both, never conflicting lengths.
    const QList<QByteArray> te = rq->headers.values("transfer-encoding");
    const QList<QByteArray> cl = rq->headers.values("content-length");
    qint64 length = 0;
    if (!te.isEmpty()) {
        if (!cl.isEmpty()) return refuse(400, QStringLiteral("both Transfer-Encoding and Content-Length"), After::Linger);
        if (rq->minor == 0) return refuse(400, QStringLiteral("Transfer-Encoding in an HTTP/1.0 request"), After::Linger);
        QList<QByteArray> codings;
        for (const QByteArray& v : te)
            for (const QByteArray& c : v.split(',')) codings << c.trimmed().toLower();
        if (codings != QList<QByteArray>{"chunked"}) return refuse(501, QStringLiteral("unsupported transfer coding"), After::Linger);
        m_chunked = true;
    } else if (!cl.isEmpty()) {
        bool seen = false;
        for (const QByteArray& v : cl)
            for (const QByteArray& part : v.split(',')) {
                qint64 n = 0;
                if (!parseDecimal(QByteArrayView(part).trimmed(), &n) || (seen && n != length))
                    return refuse(400, QStringLiteral("bad Content-Length"), After::Linger);
                seen = true;
                length = n;
            }
    }
    const bool hasBody = m_chunked || length > 0;
    const QByteArray expect = rq->headers.value("expect").trimmed();
    if (!expect.isEmpty() && !ieq(expect, "100-continue"))
        return refuse(417, QStringLiteral("unsupported expectation"), hasBody ? After::Linger : After::Keep);
    const bool wantsContinue = !expect.isEmpty() && rq->minor == 1 && hasBody;

    // Routing and auth from the head only: nothing of a refused body is stored.
    Admission adm;
    if (m_server->m_admit) {
        try {
            adm = m_server->m_admit(*rq);
        } catch (...) {
            adm.refuse = Response::error(500, QStringLiteral("internal error"));
        }
    } else {
        adm.refuse = Response::error(404, QStringLiteral("not found"));
    }
    if (!adm.refuse && !adm.handler) adm.refuse = Response::error(500, QStringLiteral("no handler"));
    m_bodyLimit = adm.maxBodyBytes >= 0 ? adm.maxBodyBytes : m_opt.maxBodyBytes;
    if (!adm.refuse && !m_chunked && length > m_bodyLimit) adm.refuse = Response::error(413, QStringLiteral("request body too large"));
    if (adm.refuse) {
        Response r = std::move(*adm.refuse);
        if (!hasBody) return writeResponse(std::move(r), After::Keep);
        if (!m_chunked && !wantsContinue && length <= m_opt.drainBytes) {
            m_heldRefusal = std::move(r);  // read the small body first, so the connection can stay open
            m_bodyLeft = length;
            m_st = St::Drain;
            arm(m_opt.bodyTimeoutMs);
            return;
        }
        return writeResponse(std::move(r), After::Linger);
    }
    m_adm = std::move(adm);
    rq->chunked = m_chunked;
    if (!hasBody) return dispatch();
    m_bodyLeft = length;
    m_ck = Ck::Size;
    m_chunkLeft = 0;
    m_trailerBytes = 0;
    if (m_adm.hashBody) m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    if (!m_chunked) rq->body.reserve(qsizetype(m_adm.spool ? qMin(length, m_opt.spoolThresholdBytes) : length));
    if (wantsContinue) m_sock->write("HTTP/1.1 100 Continue\r\n\r\n");
    m_st = St::Body;
    arm(m_opt.bodyTimeoutMs);
}

bool Connection::readBody() {
    if (!m_chunked) {
        const qint64 take = qMin<qint64>(m_bodyLeft, m_in.size());
        if (take > 0) {
            const bool ok = appendBody(QByteArrayView(m_in).left(take));
            m_in.remove(0, take);
            m_bodyLeft -= take;
            if (!ok) return false;
        }
        if (m_bodyLeft == 0) {
            completeBody();
            return false;
        }
        arm(m_opt.bodyTimeoutMs);          // progress (or still waiting): the body timeout counts from the last byte
        return false;
    }
    for (;;) {
        switch (m_ck) {
        case Ck::Size: {
            const qsizetype eol = m_in.indexOf("\r\n");
            if (eol < 0) {
                if (m_in.size() > kMaxChunkLine) {
                    refuse(400, QStringLiteral("chunk size line too long"), After::Linger);
                    return false;
                }
                arm(m_opt.bodyTimeoutMs);
                return false;
            }
            if (eol > kMaxChunkLine) {
                refuse(400, QStringLiteral("chunk size line too long"), After::Linger);
                return false;
            }
            QByteArrayView line = QByteArrayView(m_in).left(eol);
            if (const qsizetype semi = line.indexOf(';'); semi >= 0) line = line.left(semi);   // chunk extensions ignored
            line = line.trimmed();
            qint64 size = 0;
            bool ok = !line.isEmpty() && line.size() <= 15;
            for (char c : line) {
                if (!isHex(c)) { ok = false; break; }
                size = size * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            m_in.remove(0, eol + 2);
            if (!ok) {
                refuse(400, QStringLiteral("bad chunk size"), After::Linger);
                return false;
            }
            if (size == 0) {
                m_ck = Ck::Trailer;
                break;
            }
            if (m_req->bodySize + size > m_bodyLimit) {
                refuse(413, QStringLiteral("request body too large"), After::Linger);
                return false;
            }
            m_chunkLeft = size;
            m_ck = Ck::Data;
            break;
        }
        case Ck::Data: {
            const qint64 take = qMin<qint64>(m_chunkLeft, m_in.size());
            if (take == 0) {
                arm(m_opt.bodyTimeoutMs);
                return false;
            }
            const bool ok = appendBody(QByteArrayView(m_in).left(take));
            m_in.remove(0, take);
            if (!ok) return false;
            m_chunkLeft -= take;
            if (m_chunkLeft == 0) m_ck = Ck::DataEnd;
            break;
        }
        case Ck::DataEnd:
            if (m_in.size() < 2) {
                arm(m_opt.bodyTimeoutMs);
                return false;
            }
            if (!m_in.startsWith("\r\n")) {
                refuse(400, QStringLiteral("chunk data not followed by CRLF"), After::Linger);
                return false;
            }
            m_in.remove(0, 2);
            m_ck = Ck::Size;
            break;
        case Ck::Trailer: {
            const qsizetype eol = m_in.indexOf("\r\n");
            if (eol < 0) {
                if (m_trailerBytes + m_in.size() > m_opt.maxHeadBytes) {
                    refuse(431, QStringLiteral("trailer section too large"), After::Linger);
                    return false;
                }
                arm(m_opt.bodyTimeoutMs);
                return false;
            }
            m_in.remove(0, eol + 2);
            if (eol == 0) {                // the blank line after the (discarded) trailer fields
                completeBody();
                return false;
            }
            m_trailerBytes += int(eol + 2);
            if (m_trailerBytes > m_opt.maxHeadBytes) {
                refuse(431, QStringLiteral("trailer section too large"), After::Linger);
                return false;
            }
            break;
        }
        }
    }
}

bool Connection::appendBody(QByteArrayView data) {
    if (data.isEmpty()) return true;
    if (m_req->bodySize + data.size() > m_bodyLimit) {
        refuse(413, QStringLiteral("request body too large"), After::Linger);
        return false;
    }
    if (m_hash) m_hash->addData(data);
    m_req->bodySize += data.size();
    if (!m_spool && m_adm.spool && m_req->body.size() + data.size() > m_opt.spoolThresholdBytes) {
        const QString dir = m_opt.spoolDir.isEmpty() ? QDir::tempPath() + QStringLiteral("/PlayPlayStudio-spool") : m_opt.spoolDir;
        QDir().mkpath(dir);
        const QString path = dir + QStringLiteral("/upload-") + ids::randomHex(8) + QStringLiteral(".part");
        auto f = std::make_unique<QFile>(path);
        if (!f->open(QIODevice::WriteOnly | QIODevice::NewOnly) || f->write(m_req->body) != m_req->body.size()) {
            log::warn(u"http", QStringLiteral("cannot spool an upload to %1: %2").arg(QDir::toNativeSeparators(path), f->errorString()));
            f->close();
            QFile::remove(path);
            refuse(507, QStringLiteral("cannot store the request body"), After::Linger);
            return false;
        }
        m_req->bodyFile = path;            // removed by endRequest() unless the handler moved it
        m_req->body = QByteArray();
        m_spool = std::move(f);
    }
    if (m_spool) {
        if (m_spool->write(data.data(), data.size()) != data.size()) {
            log::warn(u"http", QStringLiteral("upload spool write failed: %1").arg(m_spool->errorString()));
            refuse(507, QStringLiteral("cannot store the request body"), After::Linger);
            return false;
        }
    } else {
        m_req->body.append(data);
    }
    return true;
}

void Connection::completeBody() {
    if (m_spool) {
        const bool ok = m_spool->flush();
        m_spool->close();
        m_spool.reset();
        if (!ok) return refuse(507, QStringLiteral("cannot store the request body"), After::Linger);
    }
    if (m_hash) {
        m_req->bodySha256 = m_hash->result().toHex();
        m_hash.reset();
    }
    dispatch();
}

void Connection::dispatch() {
    m_st = St::Waiting;
    ++m_seq;
    auto state = std::make_shared<ResponderState>();
    state->link = m_link;
    state->seq = m_seq;
    state->request = m_req;
    m_responder = state;
    arm(m_adm.timeoutMs >= 0 ? m_adm.timeoutMs : m_opt.handlerTimeoutMs);
    const Handler handler = std::move(m_adm.handler);
    m_adm.handler = nullptr;
    // A local reference: a synchronous answer finishes the request (m_req is reset) while the handler still runs.
    const std::shared_ptr<Request> req = m_req;
    Responder res(std::move(state));
    try {
        handler(*req, res);
    } catch (const std::exception& e) {
        log::error(u"http", QStringLiteral("handler for %1 %2 threw: %3").arg(QString::fromLatin1(req->method), req->path, QString::fromLocal8Bit(e.what())));
        res.send(Response::error(500, QStringLiteral("internal error")));
    } catch (...) {
        log::error(u"http", QStringLiteral("handler for %1 %2 threw").arg(QString::fromLatin1(req->method), req->path));
        res.send(Response::error(500, QStringLiteral("internal error")));
    }
    // `res` ends here: a handler that kept no copy and did not answer gets 500 from ~ResponderState.
}

bool Connection::finish(quint64 seq, Response r, bool unanswered) {
    if (seq != m_seq || m_st != St::Waiting) return false;   // stale (timed out, closed, or an earlier request)
    if (unanswered) {
        ++m_server->m_stats.unanswered;
        log::warn(u"http", QStringLiteral("%1 %2: the handler did not answer").arg(QString::fromLatin1(m_req->method), m_req->path));
    }
    writeResponse(std::move(r), After::Keep);
    return true;
}

void Connection::refuse(int status, const QString& why, After after) {
    ++m_server->m_stats.protocolErrors;
    if (!m_req) {                          // a head that never became a request: give the log line something
        m_clock.start();
        ++m_server->m_stats.requests;
    }
    writeResponse(Response::error(status, why), after);
}

void Connection::onTimeout() {
    switch (m_st) {
    case St::Head:
        if (m_writePaused) {               // answers waiting and no write progress for writeTimeoutMs: the client is gone
            m_sock->abort();
            return;
        }
        if (m_in.isEmpty()) {              // idle keep-alive connection: close quietly
            m_st = St::Closing;
            m_sock->disconnectFromHost();
            arm(m_opt.writeTimeoutMs);
        } else {
            refuse(408, QStringLiteral("request head timeout"), After::Linger);
        }
        return;
    case St::Body:
    case St::Drain: refuse(408, QStringLiteral("request body timeout"), After::Linger); return;
    case St::Waiting: {
        const std::shared_ptr<ResponderState> s = m_responder.lock();
        if (!s || s->done.exchange(true)) return;   // an answer is already on its way to finish()
        ++m_server->m_stats.handlerTimeouts;
        log::warn(u"http", QStringLiteral("%1 %2: no answer within the handler deadline").arg(QString::fromLatin1(m_req->method), m_req->path));
        writeResponse(Response::error(503, QStringLiteral("the request was not answered in time")), After::Keep);
        return;
    }
    case St::Writing:
    case St::Closing:
    case St::Linger:
    case St::Closed: m_sock->abort(); return;
    }
}

void Connection::writeResponse(Response r, After after) {
    m_timer.stop();
    const bool headOnly = m_req && m_req->method == "HEAD";
    const bool noBody = r.status < 200 || r.status == 204 || r.status == 304;
    qint64 length = r.filePath.isEmpty() ? r.body.size() : r.fileLength;
    m_file.reset();
    m_fileLeft = 0;
    if (!r.filePath.isEmpty() && !noBody && !headOnly && r.fileLength > 0) {
        auto f = std::make_unique<QFile>(r.filePath);
        if (!f->open(QIODevice::ReadOnly) || (r.fileOffset > 0 && !f->seek(r.fileOffset)) || f->size() < r.fileOffset + r.fileLength) {
            log::warn(u"http", QStringLiteral("cannot read %1: %2").arg(QDir::toNativeSeparators(r.filePath), f->errorString()));
            r = Response::error(500, QStringLiteral("cannot read the file"));
            length = r.body.size();
        } else {
            m_file = std::move(f);
            m_fileLeft = r.fileLength;
        }
    }
    if (after == After::Keep && (!m_keepAlive || r.close)) after = After::Close;
    if (!m_server->isListening() && after == After::Keep) after = After::Close;

    QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + reasonPhrase(r.status) + "\r\n";
    for (const auto& h : r.headers.list()) {
        if (ieq(h.first, "content-length") || ieq(h.first, "connection") || ieq(h.first, "transfer-encoding") || !isToken(h.first)) continue;
        head += h.first + ": " + safeHeaderValue(h.second) + "\r\n";
    }
    if (!r.headers.contains("cache-control")) head += "Cache-Control: no-store\r\n";
    if (!r.headers.contains("x-content-type-options")) head += "X-Content-Type-Options: nosniff\r\n";
    if (!noBody && length > 0 && !r.headers.contains("content-type")) head += "Content-Type: application/octet-stream\r\n";
    if (!noBody) head += "Content-Length: " + QByteArray::number(length) + "\r\n";
    head += after == After::Keep ? QByteArrayLiteral("Connection: keep-alive\r\n\r\n") : QByteArrayLiteral("Connection: close\r\n\r\n");
    m_sock->write(head);
    m_bytesOut = head.size();
    if (!headOnly && !noBody && r.filePath.isEmpty() && !r.body.isEmpty()) {
        m_sock->write(r.body);
        m_bytesOut += r.body.size();
    }
    m_status = r.status;
    m_after = after;
    m_st = St::Writing;
    if (m_file) feedFile();
    else responseQueued();
}

void Connection::feedFile() {
    while (m_fileLeft > 0 && m_sock->bytesToWrite() < kWriteHighWater) {
        const QByteArray chunk = m_file->read(qMin(kFileChunk, m_fileLeft));
        if (chunk.isEmpty()) {             // shrank or failed mid-way: the status is sent, so all we can do is cut it short
            log::warn(u"http", QStringLiteral("file body ended early: %1").arg(m_file->errorString()));
            m_file.reset();
            m_sock->abort();
            return;
        }
        m_sock->write(chunk);
        m_bytesOut += chunk.size();
        m_fileLeft -= chunk.size();
    }
    if (m_fileLeft > 0) {
        arm(m_opt.writeTimeoutMs);         // re-armed by every bytesWritten
        return;
    }
    m_file.reset();
    responseQueued();
}

void Connection::onBytesWritten() {
    if (m_st == St::Writing && m_file) {
        arm(m_opt.writeTimeoutMs);
        feedFile();
    } else if (m_st == St::Head && m_writePaused) {
        if (m_sock->bytesToWrite() > kPipelineHighWater / 2) {
            arm(m_opt.writeTimeoutMs);     // progress: keep waiting
            return;
        }
        m_writePaused = false;             // the client reads again: on to its next request
        arm(m_opt.idleTimeoutMs);
        pump();
    } else if (m_st == St::Linger && m_lingerPending && m_sock->bytesToWrite() == 0) {
        startLinger();
    } else if (m_st == St::Closing || m_st == St::Linger) {
        arm(m_opt.writeTimeoutMs);
    }
}

void Connection::responseQueued() {
    ++m_server->m_stats.responses;
    m_server->m_stats.bytesOut += quint64(m_bytesOut);
    if (m_req) {
        const QString line = QStringLiteral("%1 %2 %3 %4B %5ms").arg(QString::fromLatin1(m_req->method.left(16)), m_req->path.left(300))
                                 .arg(m_status).arg(m_bytesOut).arg(m_clock.isValid() ? m_clock.elapsed() : 0);
        if (m_status >= 500) log::warn(u"http", line);
        else log::debug(u"http", line);
    }
    endRequest();
    m_req.reset();
    m_responder.reset();
    switch (m_after) {
    case After::Keep:
        m_st = St::Head;
        m_headTimerArmed = false;
        // Write back-pressure (only file bodies had it): while this answer mostly waits unsent, the next pipelined
        // request is not read, so a client that pipelines and never reads holds at most about one answer here.
        // onBytesWritten resumes; no progress for writeTimeoutMs aborts.
        if (m_sock->bytesToWrite() > kPipelineHighWater) {
            m_writePaused = true;
            arm(m_opt.writeTimeoutMs);
            return;
        }
        arm(m_opt.idleTimeoutMs);
        // Later, not now: this may run inside a handler's send(), and the next pipelined request must not be dispatched
        // from inside the previous handler.
        QMetaObject::invokeMethod(this, [this]() { pump(); }, Qt::QueuedConnection);
        return;
    case After::Close:
        m_st = St::Closing;
        m_sock->disconnectFromHost();      // flushes the answer first
        arm(m_opt.writeTimeoutMs);
        return;
    case After::Linger:
        m_st = St::Linger;
        m_in.clear();
        m_lingerClock.start();
        if (m_sock->bytesToWrite() == 0) startLinger();
        else {
            m_lingerPending = true;
            arm(m_opt.writeTimeoutMs);
        }
        return;
    }
}

void Connection::startLinger() {
    m_lingerPending = false;
    // The answer is out: half-close so the client sees its end, then read and drop what it still sends for a while.
    // Closing with unread data would make the stack send a reset, which can destroy the answer before the client reads it.
    if (!halfClose(m_sock)) {
        m_st = St::Closing;
        m_sock->disconnectFromHost();
        arm(m_opt.writeTimeoutMs);
        return;
    }
    m_sock->skip(m_sock->bytesAvailable());
    arm(m_opt.lingerMs);
}

} // namespace detail

// ---------------- Responder ----------------

bool Responder::send(Response r) const {
    if (!d || d->done.exchange(true)) return false;
    return detail::deliver(d->link, d->seq, std::move(r));
}

bool Responder::answered() const { return !d || d->done.load(); }

bool Responder::alive() const {
    if (!d || d->done.load()) return false;
    QMutexLocker lock(&d->link->mu);
    return d->link->conn != nullptr;
}

const Request& Responder::request() const {
    static const Request empty;
    return d && d->request ? *d->request : empty;
}

std::shared_ptr<const Request> Responder::requestPtr() const { return d ? d->request : nullptr; }

// ---------------- HttpServer ----------------

HttpServer::HttpServer(QObject* parent) : QObject(parent) {}

HttpServer::~HttpServer() {
    close();
    // Now, while this object is whole: ~Connection updates the stats, and QObject's own cleanup would run too late.
    const QSet<detail::Connection*> conns = m_conns;
    qDeleteAll(conns);
}

bool HttpServer::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (isListening()) {
        if (error) *error = QStringLiteral("already listening on port %1").arg(this->port());
        return false;
    }
    if (!m_tcp) {
        m_tcp = new QTcpServer(this);
        m_tcp->setMaxPendingConnections(qMax(30, m_opt.maxConnections));
        connect(m_tcp, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);
    }
    if (!m_tcp->listen(address, port)) {
        if (error) *error = QStringLiteral("listen on %1:%2 failed: %3").arg(address.toString()).arg(port).arg(m_tcp->errorString());
        return false;
    }
    return true;
}

void HttpServer::close() {
    if (m_tcp) m_tcp->close();
    const QSet<detail::Connection*> conns = m_conns;   // abort() may lead to deleteLater, never to an immediate delete
    for (detail::Connection* c : conns) c->abort();
}

bool HttpServer::isListening() const { return m_tcp && m_tcp->isListening(); }
quint16 HttpServer::port() const { return isListening() ? m_tcp->serverPort() : 0; }

void HttpServer::onNewConnection() {
    while (QTcpSocket* s = m_tcp->nextPendingConnection()) {
        if (m_stats.open >= m_opt.maxConnections) {
            ++m_stats.refusedConnections;
            const QByteArray body = R"({"ok":false,"error":"too many connections"})";
            s->write("HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nRetry-After: 1\r\nContent-Length: " +
                     QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            s->disconnectFromHost();
            QTimer::singleShot(5000, s, [s]() { s->abort(); });
            continue;
        }
        new detail::Connection(this, s);
    }
}

QJsonObject HttpServer::statsJson() const {
    QJsonObject o;
    o["listening"] = isListening();
    o["port"] = int(port());
    o["open"] = m_stats.open;
    o["connections"] = double(m_stats.connections);
    o["refusedConnections"] = double(m_stats.refusedConnections);
    o["requests"] = double(m_stats.requests);
    o["reused"] = double(m_stats.reused);
    o["responses"] = double(m_stats.responses);
    o["protocolErrors"] = double(m_stats.protocolErrors);
    o["handlerTimeouts"] = double(m_stats.handlerTimeouts);
    o["unanswered"] = double(m_stats.unanswered);
    o["bytesIn"] = double(m_stats.bytesIn);
    o["bytesOut"] = double(m_stats.bytesOut);
    return o;
}

} // namespace studio::net
