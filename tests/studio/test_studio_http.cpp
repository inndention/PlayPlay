// ST-04 test (docs/plans/starmedia-studio.md §8): the Studio's HTTP/1.1 server, router, token auth and WS hub, all on
// ephemeral loopback ports in this process (plus one PlayPlayStudio.exe --print-token run on a temp data dir).
//   - routing (literal > param > end of pattern > tail, so a tail route never shadows the exact route it extends; dot
//     components, also percent-encoded inside one segment, refused);
//   - keep-alive, pipelining (with write back-pressure: a client that does not read is answered one buffer at a time),
//     HTTP/1.0, idle timeout, request limit per connection;
//   - chunked and Content-Length uploads (in memory and spooled to disk, sha256 while receiving, spool file removed
//     after the answer), Expect: 100-continue, body limits and framing errors;
//   - Range downloads (206 / 416 / If-Range / If-None-Match / HEAD) and a large streamed file;
//   - 401 without the token (before any body byte is read), the token file (created owner-only, kept, malformed
//     refused, rotated), the Host check (421) and that the token never reaches the log;
//   - async handlers answer exactly once (second send refused, dropped responder -> 500, deadline -> 503 and a late
//     send refused, an earlier request's responder cannot answer a later one, worker-thread answers, client gone);
//   - protocol errors and limits (400 / 414 / 417 / 431 / 505, header injection, slow loris 408, connection cap 503);
//   - WS: handshake refusals before the upgrade, ?since= replay from the ring and, after a "restart", from the events
//     table, live events without gap or duplicate across a reconnect and a publish burst, resets (epoch of another
//     process or studio.db, ahead judged against the table, gap, too_far), filters, ping, and the slow-consumer close
//     (4008).
// The server runs on this thread, so every wait spins the event loop (qWait / qWaitFor), never a blocking waitFor*().
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>

#include "app/studioservice.h"
#include "core/log.h"
#include "net/auth.h"
#include "net/httpserver.h"
#include "net/router.h"
#include "net/wshub.h"
#include "store/db.h"
#include "store/repos.h"

using namespace studio;
using namespace studio::net;

namespace {

const QByteArray kToken = "tok_test_0123456789abcdefghijklmnopqrstuvwxyz";

struct Resp {
    int status = 0;
    QHash<QByteArray, QByteArray> headers;   // lower-case names
    QByteArray head, body;
    QByteArray h(const char* name) const { return headers.value(name); }
};

// One response from the front of `buf`; returns the bytes it used (0 = not complete yet).
qsizetype parseResponse(const QByteArray& buf, Resp* r, bool headOnly) {
    const qsizetype end = buf.indexOf("\r\n\r\n");
    if (end < 0) return 0;
    Resp out;
    out.head = buf.left(end);
    const QList<QByteArray> lines = out.head.split('\n');
    const QList<QByteArray> sl = lines.value(0).trimmed().split(' ');
    out.status = sl.value(1).toInt();
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const qsizetype c = lines[i].indexOf(':');
        if (c > 0) out.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
    }
    qint64 len = 0;
    if (!headOnly && out.status >= 200 && out.status != 204 && out.status != 304) len = out.headers.value("content-length").toLongLong();
    if (buf.size() < end + 4 + len) return 0;
    out.body = buf.mid(end + 4, len);
    *r = out;
    return end + 4 + len;
}

// A raw client connection. Waits spin the event loop.
class Raw {
public:
    explicit Raw(quint16 port) {
        QObject::connect(&s, &QTcpSocket::readyRead, &s, [this]() { buf += s.readAll(); });
        s.connectToHost(QHostAddress::LocalHost, port);
        (void)QTest::qWaitFor([this]() { return s.state() == QAbstractSocket::ConnectedState; }, 3000);
    }
    bool connected() const { return s.state() == QAbstractSocket::ConnectedState; }
    void send(const QByteArray& b) { s.write(b); }
    bool next(Resp* r, int ms = 5000, bool headOnly = false) {
        qsizetype used = 0;
        const bool ok = QTest::qWaitFor([&]() {
            buf += s.readAll();
            used = parseResponse(buf, r, headOnly);
            return used > 0;
        }, ms);
        if (ok) buf.remove(0, used);
        return ok;
    }
    bool waitClosed(int ms = 5000) {
        return QTest::qWaitFor([this]() { buf += s.readAll(); return s.state() == QAbstractSocket::UnconnectedState; }, ms);
    }
    QTcpSocket s;
    QByteArray buf;
};

Resp exchange(quint16 port, const QByteArray& request, bool headOnly = false) {
    Raw c(port);
    c.send(request);
    Resp r;
    c.next(&r, 5000, headOnly);
    return r;
}

QByteArray get(const QByteArray& path, const QByteArray& extra = {}, bool token = true) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + (token ? "Authorization: Bearer " + kToken + "\r\n" : QByteArray()) + extra + "\r\n";
}

QByteArray randomBytes(qsizetype n) {
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator gen(4242);
    for (qsizetype i = 0; i < n; ++i) b[i] = char(gen.generate() & 0xff);
    return b;
}

QByteArray sha(const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(); }

QJsonObject jsonOf(const QByteArray& b) { return QJsonDocument::fromJson(b).object(); }

struct Got {
    int status = 0;
    QByteArray body;
};
Got nam(QNetworkAccessManager& m, const QByteArray& verb, const QUrl& url, const QByteArray& body = {}, bool token = true) {
    QNetworkRequest rq(url);
    rq.setTransferTimeout(20000);
    if (token) rq.setRawHeader("Authorization", "Bearer " + kToken);
    if (!body.isEmpty()) rq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
    QNetworkReply* r = m.sendCustomRequest(rq, verb, body);
    if (!r->isFinished()) {
        QSignalSpy done(r, &QNetworkReply::finished);
        done.wait(30000);
    }
    Got g{r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), r->readAll()};
    delete r;
    return g;
}

// A WS client collecting the JSON messages it receives.
class WsClient {
public:
    WsClient() {
        QObject::connect(&ws, &QWebSocket::textMessageReceived, &ws, [this](const QString& t) { msgs << QJsonDocument::fromJson(t.toUtf8()).object(); });
    }
    bool open(const QUrl& url, bool header = true) {
        QNetworkRequest rq(url);
        if (header) rq.setRawHeader("Authorization", "Bearer " + kToken);
        ws.open(rq);
        return QTest::qWaitFor([this]() { return ws.state() == QAbstractSocket::ConnectedState; }, 5000);
    }
    bool wait(int n, int ms = 5000) { return QTest::qWaitFor([&]() { return msgs.size() >= n; }, ms); }
    bool waitSeq(qint64 seq, int ms = 5000) { return QTest::qWaitFor([&]() { return !seqs().isEmpty() && seqs().last() >= seq; }, ms); }
    QList<qint64> seqs() const {
        QList<qint64> out;
        for (const QJsonObject& m : msgs)
            if (m.contains("seq")) out << qint64(m.value("seq").toDouble());
        return out;
    }
    QJsonObject hello() const { return msgs.isEmpty() ? QJsonObject() : msgs.first(); }
    void close() {
        ws.close();
        (void)QTest::qWaitFor([this]() { return ws.state() == QAbstractSocket::UnconnectedState; }, 3000);
    }
    QWebSocket ws;
    QList<QJsonObject> msgs;
};

QList<qint64> range(qint64 from, qint64 to) {
    QList<qint64> out;
    for (qint64 i = from; i <= to; ++i) out << i;
    return out;
}

QByteArray wsHandshake(const QByteArray& target, const QByteArray& extra = {}) {
    return "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

} // namespace

class TestStudioHttp : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    Auth m_auth;
    Router m_router;
    HttpServer m_http;
    QString m_file;
    QByteArray m_fileData;

    // what handlers saw
    int m_uploadCalls = 0;
    QString m_lastSpool;
    bool m_lastSpoolExisted = false;
    std::atomic<int> m_secondSend{-1};
    std::atomic<int> m_threadSend{-1};
    std::atomic<int> m_slowSend{-1};
    Responder m_held;

    quint16 port() const { return m_http.port(); }
    QUrl url(const QString& p) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(p)); }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        log::Options lo;
        lo.minLevel = log::Level::Debug;
        QVERIFY(log::start(lo));                        // no file: recent() only, to check what reaches the log
        m_auth.setToken(QString::fromLatin1(kToken));
        m_router.setAuth(&m_auth);
        ServerOptions so;
        so.allowedHosts = allowedHostsFor(QHostAddress(QHostAddress::LocalHost));
        so.spoolDir = m_tmp.filePath(QStringLiteral("spool"));
        so.spoolThresholdBytes = 256 * 1024;
        m_http.setOptions(so);
        m_http.setAdmit([this](Request& rq) { return m_router.admit(rq); });

        const RouteOptions open{Access::Public};
        QVERIFY(m_router.add("GET", QStringLiteral("/pub"), [](const Request&) { return Response::text(200, QStringLiteral("pub")); }, open));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/echo/{id}"), [](const Request& rq) {
            QJsonObject o;
            o["id"] = rq.param(QStringLiteral("id"));
            o["x"] = rq.queryValue(QStringLiteral("x"));
            o["method"] = QString::fromLatin1(rq.method);
            o["auth"] = rq.authenticated;
            o["path"] = rq.path;
            return Response::json(o);
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/tail/{*rest}"), [](const Request& rq) {
            return Response::json(QJsonObject{{"rest", rq.param(QStringLiteral("rest"))}});
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/items/{id}"), [](const Request& rq) {
            return Response::json(QJsonObject{{"which", "param"}, {"id", rq.param(QStringLiteral("id"))}});
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/items/active"), [](const Request&) { return Response::json(QJsonObject{{"which", "literal"}}); }));
        // An exact route and a public tail route that extends it, the tail one added first: /api/spec is the exact
        // route's (and needs the token), never the tail's with an empty tail.
        QVERIFY(m_router.add("GET", QStringLiteral("/api/spec/{*rest}"), [](const Request& rq) {
            return Response::json(QJsonObject{{"which", "tail"}, {"rest", rq.param(QStringLiteral("rest"))}});
        }, RouteOptions{Access::Public}));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/spec"), [](const Request&) { return Response::json(QJsonObject{{"which", "exact"}}); }));
        // 1 MiB answers from memory, numbered, for the write back-pressure check.
        QVERIFY(m_router.add("GET", QStringLiteral("/api/big/{n}"), [](const Request& rq) {
            return Response::bytes(200, "application/octet-stream", rq.param(QStringLiteral("n")).toLatin1() + ':' + QByteArray(1024 * 1024, 'b'));
        }));
        RouteOptions up;
        up.spool = true;
        up.hashBody = true;
        up.maxBodyBytes = 64LL * 1024 * 1024;
        QVERIFY(m_router.add("POST", QStringLiteral("/api/upload"), [this](const Request& rq) {
            ++m_uploadCalls;
            m_lastSpool = rq.bodyFile;
            m_lastSpoolExisted = !rq.bodyFile.isEmpty() && QFileInfo::exists(rq.bodyFile);
            bool ok = false;
            const QByteArray all = rq.readBody(128LL * 1024 * 1024, &ok);
            QJsonObject o;
            o["size"] = double(rq.bodySize);
            o["sha"] = QString::fromLatin1(rq.bodySha256);
            o["readSha"] = QString::fromLatin1(sha(all));
            o["spooled"] = !rq.bodyFile.isEmpty();
            o["chunked"] = rq.chunked;
            o["readOk"] = ok;
            return Response::json(o);
        }, up));
        RouteOptions small;
        small.maxBodyBytes = 1024;
        QVERIFY(m_router.add("POST", QStringLiteral("/api/small"), [](const Request& rq) { return Response::json(QJsonObject{{"size", double(rq.bodySize)}}); }, small));
        QVERIFY(m_router.add("POST", QStringLiteral("/api/json"), [](const Request& rq) {
            QString err;
            bool ok = false;
            const QJsonObject o = rq.jsonObject(&err, &ok);
            return ok ? Response::json(QJsonObject{{"got", o}}) : Response::error(400, err);
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/file"), [this](const Request& rq) { return fileResponse(rq, m_file, "application/octet-stream"); }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/file-etag"), [this](const Request& rq) { return fileResponse(rq, m_file, "image/png", "abc123"); }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/missing-file"), [this](const Request& rq) { return fileResponse(rq, m_tmp.filePath("nope.bin"), {}); }));
        QVERIFY(m_router.addAsync("GET", QStringLiteral("/api/async"), [this](const Request&, Responder res) {
            QTimer::singleShot(50, this, [this, res]() {
                res.send(Response::text(200, QStringLiteral("first")));
                m_secondSend = res.send(Response::text(200, QStringLiteral("second"))) ? 1 : 0;
            });
        }));
        QVERIFY(m_router.addAsync("GET", QStringLiteral("/api/thread"), [this](const Request&, Responder res) {
            std::thread([this, res]() {
                QThread::msleep(30);
                m_threadSend = res.send(Response::json(QJsonObject{{"from", "worker"}})) ? 1 : 0;
            }).detach();
        }));
        QVERIFY(m_router.addAsync("GET", QStringLiteral("/api/drop"), [](const Request&, Responder) {}));
        RouteOptions quick;
        quick.timeoutMs = 300;
        QVERIFY(m_router.addAsync("GET", QStringLiteral("/api/hold"), [this](const Request&, Responder res) { m_held = res; }, quick));
        QVERIFY(m_router.addAsync("GET", QStringLiteral("/api/slow/{ms}"), [this](const Request& rq, Responder res) {
            const int ms = rq.param(QStringLiteral("ms")).toInt();
            QTimer::singleShot(ms, this, [this, res, ms]() { m_slowSend = res.send(Response::text(200, QStringLiteral("slow %1").arg(ms))) ? 1 : 0; });
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/throw"), [](const Request&) -> Response { throw std::runtime_error("boom"); }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/close"), [](const Request&) {
            Response r = Response::text(200, QStringLiteral("bye"));
            r.close = true;
            return r;
        }));
        QVERIFY(m_router.add("GET", QStringLiteral("/api/inject"), [](const Request&) {
            return Response::text(200, QStringLiteral("x")).header("X-Test", "a\r\nX-Evil: 1");
        }));
        QString err;
        QVERIFY2(m_http.listen(QHostAddress(QHostAddress::LocalHost), 0, &err), qPrintable(err));
        QVERIFY(port() > 0);

        m_fileData = randomBytes(1024 * 1024 + 123);
        m_file = m_tmp.filePath(QStringLiteral("blob.bin"));
        QFile f(m_file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(m_fileData), qint64(m_fileData.size()));
    }

    void routing() {
        // Router rules on their own.
        QVERIFY(Router::validPattern(QStringLiteral("/api/v1/projects/{id}/runs")));
        QVERIFY(Router::validPattern(QStringLiteral("/static/{*path}")));
        QVERIFY(Router::validPattern(QStringLiteral("/")));
        for (const char* bad : {"", "api", "/a/{", "/a/{1x}", "/a/{*t}/b", "/a/{x}/{x}", "/a//b", "/a/{x}y", "/a/.."})
            QVERIFY2(!Router::validPattern(QString::fromLatin1(bad)), bad);
        QVERIFY(!m_router.add("GET", QStringLiteral("/pub"), [](const Request&) { return Response::empty(); }));   // duplicate
        QVERIFY(m_router.add("PUT", QStringLiteral("/api/tmp"), [](const Request&) { return Response::empty(); }));
        QVERIFY(m_router.remove("put", QStringLiteral("/api/tmp")));
        QVERIFY(!m_router.remove("PUT", QStringLiteral("/api/tmp")));

        Resp r = exchange(port(), get("/pub", {}, false));
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("pub"));
        QCOMPARE(r.h("cache-control"), QByteArray("no-store"));
        QCOMPARE(r.h("x-content-type-options"), QByteArray("nosniff"));
        QCOMPARE(exchange(port(), get("/pub/", {}, false)).status, 200);   // trailing slash

        r = exchange(port(), get("/api/echo/a%2Fb%20c?x=1%202&y"));
        QCOMPARE(r.status, 200);
        QJsonObject o = jsonOf(r.body);
        QCOMPARE(o.value("id").toString(), QStringLiteral("a/b c"));        // %2F stays inside the segment
        QCOMPARE(o.value("x").toString(), QStringLiteral("1 2"));
        QCOMPARE(o.value("auth").toBool(), true);
        QCOMPARE(o.value("path").toString(), QStringLiteral("/api/echo/a/b c"));

        QCOMPARE(jsonOf(exchange(port(), get("/api/items/active")).body).value("which").toString(), QStringLiteral("literal"));
        o = jsonOf(exchange(port(), get("/api/items/42")).body);
        QCOMPARE(o.value("which").toString(), QStringLiteral("param"));
        QCOMPARE(o.value("id").toString(), QStringLiteral("42"));
        QCOMPARE(jsonOf(exchange(port(), get("/api/tail/a/b/c")).body).value("rest").toString(), QStringLiteral("a/b/c"));
        QCOMPARE(jsonOf(exchange(port(), get("/api/tail")).body).value("rest").toString(), QString());

        // A tail route that extends an exact one (its tail would be empty) never shadows it, whichever was added first,
        // and the exact route's access level applies; longer paths still go to the tail route.
        QCOMPARE(jsonOf(exchange(port(), get("/api/spec")).body).value("which").toString(), QStringLiteral("exact"));
        QCOMPARE(exchange(port(), get("/api/spec", {}, false)).status, 401);
        o = jsonOf(exchange(port(), get("/api/spec/a/b", {}, false)).body);
        QCOMPARE(o.value("which").toString(), QStringLiteral("tail"));
        QCOMPARE(o.value("rest").toString(), QStringLiteral("a/b"));
        {
            // The same on Routers of their own (match() gives the route index), in both insertion orders.
            const auto noop = [](const Request&) { return Response::empty(); };
            for (const bool reversed : {false, true}) {
                QStringList patterns{"/", "/{*path}", "/files", "/files/{*p}", "/a/{id}", "/a/{id}/{*rest}", "/a/{id}/x", "/a/{id}/{x}"};
                if (reversed) std::reverse(patterns.begin(), patterns.end());
                Router rt;
                for (const QString& pat : patterns) QVERIFY(rt.add("GET", pat, noop));
                auto picked = [&](const QByteArray& path) {
                    const Router::Match m = rt.match("GET", path);
                    return m.status == 0 ? patterns.value(m.route) : QString::number(m.status);
                };
                QCOMPARE(picked("/"), QStringLiteral("/"));
                QCOMPARE(picked("/other/thing"), QStringLiteral("/{*path}"));
                QCOMPARE(picked("/files"), QStringLiteral("/files"));
                QCOMPARE(picked("/files/"), QStringLiteral("/files"));
                QCOMPARE(picked("/files/x/y"), QStringLiteral("/files/{*p}"));
                QCOMPARE(picked("/a/5"), QStringLiteral("/a/{id}"));
                QCOMPARE(picked("/a/5/x"), QStringLiteral("/a/{id}/x"));
                QCOMPARE(picked("/a/5/y"), QStringLiteral("/a/{id}/{x}"));
                QCOMPARE(picked("/a/5/y/z"), QStringLiteral("/a/{id}/{*rest}"));
                QCOMPARE(picked("/a"), QStringLiteral("/{*path}"));
            }
        }

        // Dot components that a percent-encoded '/' or '\' would carry inside one segment (into a {param} value or a
        // joined tail), and names Windows would trim to "." / "..", are refused like plain dot segments.
        for (const char* bad : {"/api/echo/..%2F..%2Fsecret", "/api/echo/..%5C..%5Cwin.ini", "/api/echo/a%2F.%2Fb", "/api/echo/...",
                                "/api/echo/..%20", "/api/tail/a/..%2Fb", "/api/tail/x/%2E%2E%5Cy", "/api/echo/a%2F..", "/api/echo/%5C.."})
            QVERIFY2(exchange(port(), get(bad)).status == 400, bad);
        QCOMPARE(jsonOf(exchange(port(), get("/api/echo/..a")).body).value("id").toString(), QStringLiteral("..a"));   // a name
        QCOMPARE(jsonOf(exchange(port(), get("/api/echo/v1.2%2Fx.y")).body).value("id").toString(), QStringLiteral("v1.2/x.y"));

        QCOMPARE(exchange(port(), get("/nope")).status, 404);
        QCOMPARE(exchange(port(), get("/api/echo/")).status, 404);          // an empty segment is never a parameter
        r = exchange(port(), "POST /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
        QCOMPARE(r.status, 405);
        QCOMPARE(r.h("allow"), QByteArray("GET, HEAD, OPTIONS"));
        r = exchange(port(), "OPTIONS /api/echo/x HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        QCOMPARE(r.status, 204);
        QCOMPARE(r.h("allow"), QByteArray("GET, HEAD, OPTIONS"));
        QVERIFY(!r.headers.contains("access-control-allow-origin"));       // no CORS: other origins stay out
        QCOMPARE(exchange(port(), get("/api/../pub")).status, 400);
        QCOMPARE(exchange(port(), get("/api/echo/%2e%2e")).status, 400);
        QCOMPARE(exchange(port(), get("/api/echo/a%00b")).status, 400);
        QCOMPARE(exchange(port(), get("/api/echo/%zz")).status, 400);

        // A public route reads at most kPublicMaxBody unless it says otherwise (anonymous clients).
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 100000\r\n\r\n").status, 413);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n\r\nhello").status, 200);

        r = exchange(port(), "HEAD /pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", true);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.h("content-length"), QByteArray("3"));
        QVERIFY(r.body.isEmpty());
    }

    void auth() {
        const int calls = m_uploadCalls;
        Resp r = exchange(port(), get("/api/echo/1", {}, false));
        QCOMPARE(r.status, 401);
        QVERIFY(r.h("www-authenticate").startsWith("Bearer"));
        QCOMPARE(jsonOf(r.body).value("ok").toBool(true), false);
        QCOMPARE(exchange(port(), get("/api/echo/1", "Authorization: Bearer wrong-token-0123456789abcdef\r\n", false)).status, 401);
        QCOMPARE(exchange(port(), get("/api/echo/1", "Authorization: Basic " + kToken + "\r\n", false)).status, 401);
        QCOMPARE(exchange(port(), get("/api/echo/1", "authorization: bearer " + kToken + "\r\n", false)).status, 200);
        QCOMPARE(exchange(port(), get("/api/echo/1", "X-Studio-Token: " + kToken + "\r\n", false)).status, 200);
        QCOMPARE(exchange(port(), get("/api/echo/1?token=" + kToken, {}, false)).status, 401);   // ?token= is for the WS handshake only
        QCOMPARE(jsonOf(exchange(port(), get("/api/echo/1")).body).value("auth").toBool(), true);

        // Host: other names are refused even on public routes (DNS rebinding), HTTP/1.1 needs one, HTTP/1.0 may omit it.
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: evil.example\r\n\r\n").status, 421);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: evil.example:80\r\n\r\n").status, 421);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: localhost:1234\r\n\r\n").status, 200);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: [::1]\r\n\r\n").status, 200);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.0\r\n\r\n").status, 200);

        // Refused before the body: a 10 MB upload without the token gets 401 at once, nothing is read or stored, and the
        // connection closes after the answer (with Expect: 100-continue no "100 Continue" is sent).
        {
            Raw c(port());
            c.send("POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 10000000\r\n\r\n" + QByteArray(1000, 'x'));
            Resp a;
            QVERIFY(c.next(&a, 3000));
            QCOMPARE(a.status, 401);
            QCOMPARE(a.h("connection"), QByteArray("close"));
            QVERIFY(c.waitClosed(5000));
        }
        {
            Raw c(port());
            c.send("POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nExpect: 100-continue\r\nContent-Length: 5000000\r\n\r\n");
            Resp a;
            QVERIFY(c.next(&a, 3000));
            QCOMPARE(a.status, 401);
            QVERIFY(!a.head.contains("100 Continue"));
            QVERIFY(c.waitClosed(5000));
        }
        QCOMPARE(m_uploadCalls, calls);
        QVERIFY(!QDir(m_tmp.filePath(QStringLiteral("spool"))).exists() || QDir(m_tmp.filePath(QStringLiteral("spool"))).isEmpty());

        // A Router without an Auth fails closed.
        Router bare;
        QVERIFY(bare.add("GET", QStringLiteral("/x"), [](const Request&) { return Response::empty(); }));
        Request rq;
        rq.method = "GET";
        rq.rawPath = "/x";
        rq.headers.add("authorization", "Bearer " + kToken);
        const Admission adm = bare.admit(rq);
        QVERIFY(adm.refuse);
        QCOMPARE(adm.refuse->status, 401);

        // The token never reaches the log, though 401s and requests were logged.
        QVERIFY(log::flush());
        const QString lines = log::recent(500).join(QLatin1Char('\n'));
        QVERIFY(lines.contains(QStringLiteral("missing or invalid token")));
        QVERIFY(lines.contains(QStringLiteral("/api/echo/1")));
        QVERIFY(!lines.contains(QString::fromLatin1(kToken)));
        QVERIFY(!lines.contains(QStringLiteral("wrong-token")));
    }

    void tokenFile() {
        const QString dir = m_tmp.filePath(QStringLiteral("tok"));
        QVERIFY(QDir().mkpath(dir));
        Auth a;
        QString err;
        QVERIFY2(a.load(dir, &err), qPrintable(err));
        QVERIFY(a.hasToken());
        const QString path = dir + QStringLiteral("/studio-token");
        QCOMPARE(QDir::cleanPath(a.tokenPath()), QDir::cleanPath(path));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray raw = f.readAll();
        f.close();
        QVERIFY(raw.endsWith('\n'));
        const QByteArray tok = raw.trimmed();
        QCOMPARE(tok.size(), 43);                       // 32 bytes, base64url, no padding
        QVERIFY(QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{43}$")).match(QString::fromLatin1(tok)).hasMatch());
        QVERIFY(a.matches(tok));
        QVERIFY(!a.matches(tok.left(42)));
        QVERIFY(!a.matches(""));
#ifdef Q_OS_WIN
        bool prot = false;
        QStringList sids = aclSids(path, &prot, &err);
        QVERIFY2(!sids.isEmpty(), qPrintable(err));
        QVERIFY(prot);                                  // nothing inherited from the folder
        const QString me = currentUserSid();
        QVERIFY(!me.isEmpty());
        for (const QString& s : sids) QVERIFY2(s == me || s == QLatin1String("S-1-5-18"), qPrintable(s));
        QVERIFY(sids.contains(me));
#endif
        // Kept across loads.
        Auth b;
        QVERIFY(b.load(dir, &err));
        QVERIFY(b.matches(tok));
        QCOMPARE(Auth::readTokenFile(path), QString::fromLatin1(tok));
        // Rotation: new token, old one refused, still owner-only.
        QVERIFY2(b.rotate(&err), qPrintable(err));
        const QString fresh = Auth::readTokenFile(path, &err);
        QVERIFY(!fresh.isEmpty());
        QVERIFY(fresh.toLatin1() != tok);
        QVERIFY(b.matches(fresh.toLatin1()));
        QVERIFY(!b.matches(tok));
#ifdef Q_OS_WIN
        sids = aclSids(path, &prot, &err);
        QVERIFY(prot);
        for (const QString& s : sids) QVERIFY2(s == currentUserSid() || s == QLatin1String("S-1-5-18"), qPrintable(s));
#endif
        // A pasted token with a BOM and CRLF is accepted; malformed files refuse start-up and are left alone.
        auto writeRaw = [&](const QByteArray& content) {
            QFile w(path);
            QVERIFY(w.open(QIODevice::WriteOnly | QIODevice::Truncate));
            w.write(content);
        };
        writeRaw("\xEF\xBB\xBF" "pasted-mesh-token-0123456789abcdef\r\n");
        Auth c;
        QVERIFY2(c.load(dir, &err), qPrintable(err));
        QVERIFY(c.matches("pasted-mesh-token-0123456789abcdef"));
        for (const QByteArray& bad : {QByteArray(), QByteArray("short"), QByteArray("has a space in it 0123456789abcdef"), QByteArray(5000, 'a')}) {
            writeRaw(bad);
            Auth d;
            QVERIFY(!d.load(dir, &err));
            QVERIFY(!d.hasToken());
            QVERIFY(err.contains(QStringLiteral("studio-token")));
            QFile again(path);
            QVERIFY(again.open(QIODevice::ReadOnly));
            QCOMPARE(again.readAll(), bad);                // never replaced behind the operator's back
        }
        // A private file is not overwritten without `replace`.
        bool existed = false;
        QVERIFY(!writePrivateFile(path, "x", false, &err, &existed));
        QVERIFY(existed);
        QVERIFY(QDir(dir).entryList(QDir::Files).filter(QStringLiteral(".tmp-")).isEmpty());   // no temp file left
    }

    void keepAlive() {
        const HttpServer::Stats before = m_http.stats();
        {
            Raw c(port());
            for (int i = 0; i < 3; ++i) {
                c.send(get("/api/echo/" + QByteArray::number(i)));
                Resp r;
                QVERIFY(c.next(&r));
                QCOMPARE(r.status, 200);
                QCOMPARE(r.h("connection"), QByteArray("keep-alive"));
                QCOMPARE(jsonOf(r.body).value("id").toString(), QString::number(i));
            }
            QVERIFY(c.connected());
            // Pipelining: three requests in one write, a slow async one first; answers come back in order.
            c.send(get("/api/slow/150") + get("/pub", {}, false) + get("/api/echo/p"));
            Resp a, b, d;
            QVERIFY(c.next(&a));
            QVERIFY(c.next(&b));
            QVERIFY(c.next(&d));
            QCOMPARE(a.body, QByteArray("slow 150"));
            QCOMPARE(b.body, QByteArray("pub"));
            QCOMPARE(jsonOf(d.body).value("id").toString(), QStringLiteral("p"));
            // "Connection: close" ends it after the answer.
            c.send(get("/pub", "Connection: close\r\n", false));
            Resp e;
            QVERIFY(c.next(&e));
            QCOMPARE(e.h("connection"), QByteArray("close"));
            QVERIFY(c.waitClosed());
        }
        QCOMPARE(m_http.stats().connections - before.connections, quint64(1));
        QVERIFY(m_http.stats().reused - before.reused >= 6);

        // HTTP/1.0: close unless keep-alive is asked for.
        {
            Raw c(port());
            c.send("GET /pub HTTP/1.0\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.h("connection"), QByteArray("close"));
            QVERIFY(c.waitClosed());
        }
        {
            Raw c(port());
            c.send("GET /pub HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.h("connection"), QByteArray("keep-alive"));
            c.send("GET /pub HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
        }
        // A handler can end the connection.
        {
            Raw c(port());
            c.send(get("/api/close"));
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.body, QByteArray("bye"));
            QCOMPARE(r.h("connection"), QByteArray("close"));
            QVERIFY(c.waitClosed());
        }
        // QNetworkAccessManager reuses one connection for sequential requests.
        const quint64 conns = m_http.stats().connections;
        QNetworkAccessManager m;
        for (int i = 0; i < 5; ++i) QCOMPARE(nam(m, "GET", url(QStringLiteral("/api/echo/%1").arg(i))).status, 200);
        QCOMPARE(m_http.stats().connections - conns, quint64(1));

        // Idle timeout and the per-connection request limit, on a server of their own.
        HttpServer s2;
        ServerOptions o;
        o.idleTimeoutMs = 300;
        o.maxRequestsPerConnection = 3;
        s2.setOptions(o);
        s2.setAdmit([this](Request& rq) { return m_router.admit(rq); });
        QVERIFY(s2.listen(QHostAddress(QHostAddress::LocalHost), 0));
        {
            Raw c(s2.port());
            c.send(get("/pub", {}, false));
            Resp r;
            QVERIFY(c.next(&r));
            QElapsedTimer t;
            t.start();
            QVERIFY(c.waitClosed(3000));
            QVERIFY2(t.elapsed() >= 200, qPrintable(QString::number(t.elapsed())));
        }
        {
            Raw c(s2.port());
            Resp r;
            for (int i = 1; i <= 3; ++i) {
                c.send(get("/pub", {}, false));
                QVERIFY(c.next(&r));
                QCOMPARE(r.h("connection"), i < 3 ? QByteArray("keep-alive") : QByteArray("close"));
            }
            QVERIFY(c.waitClosed());
        }
    }

    void pipelineBackPressure() {
        // A client that pipelines requests and does not read its answers: the server reads the next request only once
        // the previous answer has mostly left, so it never holds every answer in memory; once the client reads, the
        // rest follow, whole and in order, on the same connection.
        const quint64 connsBefore = m_http.stats().connections;
        QTcpSocket s;
        s.setReadBufferSize(64 * 1024);                 // full: Qt stops taking bytes from the OS (the client "does not read")
        s.connectToHost(QHostAddress::LocalHost, port());
        QVERIFY(QTest::qWaitFor([&]() { return s.state() == QAbstractSocket::ConnectedState; }, 3000));
        constexpr int kCount = 64;                      // 64 answers of 1 MiB
        const quint64 before = m_http.stats().responses;
        QByteArray all;
        for (int i = 0; i < kCount; ++i) all += get("/api/big/" + QByteArray::number(i));
        s.write(all);
        QTest::qWait(1000);
        const quint64 early = m_http.stats().responses - before;
        qInfo("answers queued while the client did not read: %llu of %d", static_cast<unsigned long long>(early), kCount);
        QVERIFY2(early >= 1 && early < quint64(kCount) / 2, qPrintable(QString::number(early)));   // OS buffers hold a few
        QVERIFY(s.state() == QAbstractSocket::ConnectedState);

        s.setReadBufferSize(0);
        QByteArray buf;
        int got = 0;
        QString bad;
        const bool done = QTest::qWaitFor([&]() {
            buf += s.readAll();
            Resp r;
            while (const qsizetype used = parseResponse(buf, &r, false)) {
                buf.remove(0, used);
                const QByteArray prefix = QByteArray::number(got) + ':';
                if (bad.isEmpty() && (r.status != 200 || r.body.size() != prefix.size() + 1024 * 1024 || !r.body.startsWith(prefix)))
                    bad = QStringLiteral("answer %1: status %2, %3 bytes").arg(got).arg(r.status).arg(r.body.size());
                ++got;
            }
            return got >= kCount;
        }, 30000);
        QVERIFY2(done, qPrintable(QString::number(got)));
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
        QCOMPARE(m_http.stats().responses - before, quint64(kCount));
        QCOMPARE(m_http.stats().connections - connsBefore, quint64(1));   // all on the one connection
    }

    void chunkedUpload() {
        // Small chunked body: split writes, chunk extensions, a trailer field.
        {
            Raw c(port());
            const QByteArray auth = "Authorization: Bearer " + kToken + "\r\n";
            c.send("POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\n" + auth + "Transfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhel");
            QTest::qWait(60);
            c.send("lo\r\n6\r\n world\r\n");
            QTest::qWait(60);
            c.send("0\r\nX-Trailer: v\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
            const QJsonObject o = jsonOf(r.body);
            QCOMPARE(o.value("size").toInt(), 11);
            QCOMPARE(o.value("sha").toString().toLatin1(), sha("hello world"));
            QCOMPARE(o.value("chunked").toBool(), true);
            QCOMPARE(o.value("spooled").toBool(), false);
            // The connection stays usable after a chunked body.
            c.send(get("/pub", {}, false));
            QVERIFY(c.next(&r));
            QCOMPARE(r.body, QByteArray("pub"));
        }
        // A large chunked upload goes to a spool file, hashed while it arrives; the file is gone after the answer.
        {
            const QByteArray data = randomBytes(6 * 1024 * 1024 + 7);
            Raw c(port());
            c.send("POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + kToken + "\r\nTransfer-Encoding: chunked\r\n\r\n");
            for (qsizetype off = 0; off < data.size(); off += 65536) {
                const QByteArray piece = data.mid(off, 65536);
                c.send(QByteArray::number(piece.size(), 16) + "\r\n" + piece + "\r\n");
                if ((off / 65536) % 16 == 0) QTest::qWait(1);
            }
            c.send("0\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r, 20000));
            QCOMPARE(r.status, 200);
            const QJsonObject o = jsonOf(r.body);
            QCOMPARE(qint64(o.value("size").toDouble()), qint64(data.size()));
            QCOMPARE(o.value("sha").toString().toLatin1(), sha(data));
            QCOMPARE(o.value("readSha").toString().toLatin1(), sha(data));
            QCOMPARE(o.value("spooled").toBool(), true);
            QVERIFY(m_lastSpoolExisted);
            QVERIFY(m_lastSpool.startsWith(m_tmp.filePath(QStringLiteral("spool"))));
            QVERIFY(QTest::qWaitFor([&]() { return !QFileInfo::exists(m_lastSpool); }, 2000));
        }
        // Content-Length upload through QNetworkAccessManager, spooled too.
        {
            const QByteArray data = randomBytes(3 * 1024 * 1024);
            QNetworkAccessManager m;
            const Got g = nam(m, "POST", url(QStringLiteral("/api/upload")), data);
            QCOMPARE(g.status, 200);
            const QJsonObject o = jsonOf(g.body);
            QCOMPARE(o.value("sha").toString().toLatin1(), sha(data));
            QCOMPARE(o.value("spooled").toBool(), true);
            QCOMPARE(o.value("chunked").toBool(), false);
        }
        // Expect: 100-continue gets the interim answer before the body is sent.
        {
            Raw c(port());
            c.send("POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + kToken + "\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 100);
            c.send("abcd");
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
            QCOMPARE(jsonOf(r.body).value("sha").toString().toLatin1(), sha("abcd"));
        }
        // JSON bodies.
        {
            QNetworkAccessManager m;
            Got g = nam(m, "POST", url(QStringLiteral("/api/json")), R"({"a":1})");
            QCOMPARE(g.status, 200);
            QCOMPARE(jsonOf(g.body).value("got").toObject().value("a").toInt(), 1);
            QCOMPARE(nam(m, "POST", url(QStringLiteral("/api/json")), "[1,2]").status, 400);
            QCOMPARE(nam(m, "POST", url(QStringLiteral("/api/json")), "{bad").status, 400);
        }
        // Framing errors and limits.
        const QByteArray head = "POST /api/small HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + kToken + "\r\n";
        {
            Raw c(port());
            c.send(head + "Transfer-Encoding: chunked\r\n\r\nzz\r\nab\r\n0\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 400);
            QVERIFY(c.waitClosed());
        }
        {
            Raw c(port());
            c.send(head + "Transfer-Encoding: chunked\r\n\r\n800\r\n" + QByteArray(0x800, 'a') + "\r\n0\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 413);                        // 2 KiB chunked into a 1 KiB route
            QVERIFY(c.waitClosed());
        }
        QCOMPARE(exchange(port(), head + "Transfer-Encoding: gzip, chunked\r\n\r\n").status, 501);
        QCOMPARE(exchange(port(), head + "Transfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\nabc").status, 400);
        QCOMPARE(exchange(port(), head + "Content-Length: -1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), head + "Content-Length: 3, 4\r\n\r\nabc").status, 400);
        QCOMPARE(jsonOf(exchange(port(), head + "Content-Length: 3, 3\r\n\r\nabc").body).value("size").toInt(), 3);
        {
            // A small over-limit body is drained, so the connection stays open.
            Raw c(port());
            c.send(head + "Content-Length: 5000\r\n\r\n" + QByteArray(5000, 'b'));
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 413);
            QCOMPARE(r.h("connection"), QByteArray("keep-alive"));
            c.send(get("/pub", {}, false));
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
        }
        {
            // A large one is refused at once and the connection closed (the body is never read).
            Raw c(port());
            c.send(head + "Content-Length: 10000000\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r, 2000));
            QCOMPARE(r.status, 413);
            QVERIFY(c.waitClosed());
        }
    }

    void rangeDownload() {
        const qint64 size = m_fileData.size();
        Raw c(port());
        Resp r;
        c.send(get("/api/file"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 200);
        QCOMPARE(r.h("accept-ranges"), QByteArray("bytes"));
        QCOMPARE(r.h("content-type"), QByteArray("application/octet-stream"));
        QCOMPARE(r.body.size(), size);
        QVERIFY(r.body == m_fileData);
        const QByteArray etag = r.h("etag");
        QVERIFY(etag.startsWith('"') && etag.endsWith('"'));
        QVERIFY(!r.h("last-modified").isEmpty());

        auto ranged = [&](const QByteArray& range, const QByteArray& extra = {}) {
            c.send(get("/api/file", "Range: " + range + "\r\n" + extra));
            Resp x;
            if (!c.next(&x)) return Resp{};
            return x;
        };
        r = ranged("bytes=0-99");
        QCOMPARE(r.status, 206);
        QCOMPARE(r.h("content-range"), "bytes 0-99/" + QByteArray::number(size));
        QVERIFY(r.body == m_fileData.left(100));
        r = ranged("bytes=-100");
        QCOMPARE(r.status, 206);
        QVERIFY(r.body == m_fileData.right(100));
        r = ranged("bytes=1000-");
        QCOMPARE(r.status, 206);
        QCOMPARE(r.body.size(), size - 1000);
        QVERIFY(r.body == m_fileData.mid(1000));
        r = ranged("bytes=5-" + QByteArray::number(size + 1000));   // past the end: clipped
        QCOMPARE(r.status, 206);
        QCOMPARE(r.body.size(), size - 5);
        r = ranged("bytes=" + QByteArray::number(size) + "-");
        QCOMPARE(r.status, 416);
        QCOMPARE(r.h("content-range"), "bytes */" + QByteArray::number(size));
        r = ranged("bytes=-0");
        QCOMPARE(r.status, 416);
        r = ranged("bytes=0-1,5-6");                        // several ranges: the whole file
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body.size(), size);
        r = ranged("bytes=abc");
        QCOMPARE(r.status, 200);
        r = ranged("items=0-5");
        QCOMPARE(r.status, 200);
        r = ranged("bytes=10-19", "If-Range: " + etag + "\r\n");
        QCOMPARE(r.status, 206);
        QVERIFY(r.body == m_fileData.mid(10, 10));
        r = ranged("bytes=10-19", "If-Range: \"stale\"\r\n");
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body.size(), size);
        r = ranged("bytes=10-19", "If-Range: W/" + etag + "\r\n");   // a weak tag never matches If-Range
        QCOMPARE(r.status, 200);

        c.send(get("/api/file", "If-None-Match: \"x\", " + etag + "\r\n"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 304);
        QVERIFY(r.body.isEmpty());
        QVERIFY(!r.headers.contains("content-length"));
        c.send("HEAD /api/file HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + kToken + "\r\nRange: bytes=0-99\r\n\r\n");
        QVERIFY(c.next(&r, 5000, true));
        QCOMPARE(r.status, 206);
        QCOMPARE(r.h("content-length"), QByteArray("100"));
        c.send(get("/api/file-etag"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.h("etag"), QByteArray("\"abc123\""));
        QCOMPARE(r.h("content-type"), QByteArray("image/png"));
        c.send(get("/api/missing-file"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 404);
        // Still the same connection after all of that.
        c.send(get("/pub", {}, false));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 200);
        QVERIFY(c.connected());

        // A large file is streamed (never read whole) and arrives intact.
        const QByteArray big = randomBytes(24 * 1024 * 1024 + 11);
        const QString bigPath = m_tmp.filePath(QStringLiteral("big.bin"));
        {
            QFile f(bigPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(big), qint64(big.size()));
        }
        QVERIFY(m_router.add("GET", QStringLiteral("/api/big"), [bigPath](const Request& rq) { return fileResponse(rq, bigPath, {}); }));
        QNetworkAccessManager m;
        const Got g = nam(m, "GET", url(QStringLiteral("/api/big")));
        QCOMPARE(g.status, 200);
        QCOMPARE(g.body.size(), big.size());
        QCOMPARE(sha(g.body), sha(big));
    }

    void asyncAnswers() {
        Raw c(port());
        Resp r;
        // Answered once: the handler's second send() is refused and nothing else arrives on the connection.
        m_secondSend = -1;
        c.send(get("/api/async"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.body, QByteArray("first"));
        QVERIFY(QTest::qWaitFor([this]() { return m_secondSend.load() != -1; }, 2000));
        QCOMPARE(m_secondSend.load(), 0);
        QTest::qWait(100);
        QVERIFY2(c.buf.isEmpty(), c.buf.constData());
        // From a worker thread.
        m_threadSend = -1;
        c.send(get("/api/thread"));
        QVERIFY(c.next(&r));
        QCOMPARE(jsonOf(r.body).value("from").toString(), QStringLiteral("worker"));
        QVERIFY(QTest::qWaitFor([this]() { return m_threadSend.load() != -1; }, 2000));
        QCOMPARE(m_threadSend.load(), 1);
        // A responder dropped unanswered answers 500.
        const quint64 unanswered = m_http.stats().unanswered;
        c.send(get("/api/drop"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 500);
        QVERIFY(jsonOf(r.body).value("error").toString().contains(QStringLiteral("did not answer")));
        QCOMPARE(m_http.stats().unanswered, unanswered + 1);
        // A handler that throws gets 500.
        c.send(get("/api/throw"));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 500);
        // Past the deadline: 503; then the next request on the same connection gets its own answer, and the held
        // responder of the earlier request can no longer answer anything.
        const quint64 timeouts = m_http.stats().handlerTimeouts;
        QElapsedTimer t;
        t.start();
        c.send(get("/api/hold"));
        QVERIFY(c.next(&r, 3000));
        QCOMPARE(r.status, 503);
        QVERIFY(t.elapsed() >= 250);
        QCOMPARE(m_http.stats().handlerTimeouts, timeouts + 1);
        QVERIFY(m_held.answered());
        QVERIFY(!m_held.alive());
        c.send(get("/api/slow/200"));
        QTest::qWait(50);
        QVERIFY(!m_held.send(Response::text(200, QStringLiteral("late"))));
        QVERIFY(c.next(&r));
        QCOMPARE(r.body, QByteArray("slow 200"));
        QTest::qWait(100);
        QVERIFY2(c.buf.isEmpty(), c.buf.constData());
        QCOMPARE(m_held.request().path, QStringLiteral("/api/hold"));
        m_held = Responder();

        // The client leaves before the answer: send() returns false, nothing breaks.
        m_slowSend = -1;
        {
            Raw gone(port());
            gone.send(get("/api/slow/300"));
            QTest::qWait(50);
            gone.s.abort();
        }
        QVERIFY(QTest::qWaitFor([this]() { return m_slowSend.load() != -1; }, 2000));
        QCOMPARE(m_slowSend.load(), 0);
        c.send(get("/pub", {}, false));
        QVERIFY(c.next(&r));
        QCOMPARE(r.status, 200);
    }

    void protocolErrors() {
        QCOMPARE(exchange(port(), "GARBAGE\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/2.0\r\nHost: 127.0.0.1\r\n\r\n").status, 505);
        QCOMPARE(exchange(port(), "GET /pub http/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET  /pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nNoColon\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nX-A : b\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nX-A: b\r\n c\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nX-A: b\nc\r\n\r\n").status, 400);
        QByteArray many = "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        for (int i = 0; i < 101; ++i) many += "X-H" + QByteArray::number(i) + ": v\r\n";
        QCOMPARE(exchange(port(), many + "\r\n").status, 431);
        QCOMPARE(exchange(port(), "GET /" + QByteArray(9000, 'a') + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 414);
        QCOMPARE(exchange(port(), "OPTIONS * HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET http://127.0.0.1/pub HTTP/1.1\r\n\r\n").status, 200);   // absolute-form
        QCOMPARE(exchange(port(), "GET http://evil.example/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 421);
        // An authority that does not parse is refused, never replaced by the Host header.
        QCOMPARE(exchange(port(), "GET http://evil.example:abc/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET http://127.0.0.1:abc/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET http://:80/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET http://[::1/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 400);
        QCOMPARE(exchange(port(), "GET http://127.0.0.1:7760/pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 200);
        QCOMPARE(exchange(port(), "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nExpect: something\r\n\r\n").status, 417);
        QCOMPARE(exchange(port(), "\r\n\r\nGET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 200);   // leading empty lines
        // A handler cannot inject header lines.
        const Resp inj = exchange(port(), get("/api/inject"));
        QCOMPARE(inj.status, 200);
        QVERIFY(!inj.headers.contains("x-evil"));
        QVERIFY(inj.h("x-test").startsWith("a"));
        // An error closes the connection; the stack does not reset it before the client read the answer.
        {
            Raw c(port());
            c.send("GET /pub HTTP/9.9\r\nHost: 127.0.0.1\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 505);
            QCOMPARE(r.h("connection"), QByteArray("close"));
            QVERIFY(c.waitClosed());
        }

        // Slow loris and the connection cap, on a server of their own.
        HttpServer s2;
        ServerOptions o;
        o.headTimeoutMs = 300;
        o.maxConnections = 2;
        s2.setOptions(o);
        s2.setAdmit([this](Request& rq) { return m_router.admit(rq); });
        QVERIFY(s2.listen(QHostAddress(QHostAddress::LocalHost), 0));
        {
            Raw c(s2.port());
            const QByteArray partial = "GET /pub HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Slow: ";
            QElapsedTimer t;
            t.start();
            Resp r;
            bool got = false;
            for (int i = 0; i < 20 && !got; ++i) {   // one byte every 100 ms never re-arms the head timeout
                c.send(i == 0 ? partial : QByteArray("a"));
                got = c.next(&r, 100);
            }
            QVERIFY(got);
            QCOMPARE(r.status, 408);
            QVERIFY2(t.elapsed() < 1500, qPrintable(QString::number(t.elapsed())));
            QVERIFY(c.waitClosed());
        }
        QVERIFY(QTest::qWaitFor([&]() { return s2.stats().open == 0; }, 3000));
        {
            Raw a(s2.port()), b(s2.port());
            QVERIFY(QTest::qWaitFor([&]() { return s2.stats().open == 2; }, 3000));
            Raw third(s2.port());
            Resp r;
            QVERIFY(third.next(&r));
            QCOMPARE(r.status, 503);
            QVERIFY(third.waitClosed());
            QCOMPARE(s2.stats().refusedConnections, quint64(1));
            a.send(get("/pub", {}, false));
            QVERIFY(a.next(&r));
            QCOMPARE(r.status, 200);
        }
    }

    void wsHandshakeChecks() {
        WsHub hub;
        hub.setAuth(&m_auth);
        hub.setAllowedHosts(allowedHostsFor(QHostAddress(QHostAddress::LocalHost)));
        QVERIFY(hub.listen(QHostAddress(QHostAddress::LocalHost), 0));
        const quint16 p = hub.port();
        const QByteArray auth = "Authorization: Bearer " + kToken + "\r\n";
        auto status = [&](const QByteArray& req) {
            Raw c(p);
            c.send(req);
            Resp r;
            c.next(&r, 3000);
            return r.status;
        };
        QCOMPARE(status(wsHandshake("/ws")), 401);
        QCOMPARE(status(wsHandshake("/ws", "Authorization: Bearer nope-nope-nope-nope-nope-nope\r\n")), 401);
        QCOMPARE(status(wsHandshake("/other", auth)), 404);
        QCOMPARE(status(wsHandshake("/ws?since=-1", auth)), 400);
        QCOMPARE(status(wsHandshake("/ws?since=abc", auth)), 400);
        QCOMPARE(status(wsHandshake("/ws", auth + "Origin: http://evil.example\r\n")), 403);
        QCOMPARE(status(wsHandshake("/ws", auth + "Origin: null\r\n")), 403);
        QCOMPARE(status("GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + auth + "\r\n"), 426);
        QCOMPARE(status("GET /ws HTTP/1.1\r\nHost: evil.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" + auth + "\r\n"), 421);
        QCOMPARE(status("POST /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"), 405);
        QCOMPARE(status(wsHandshake("http://evil.example/ws", auth)), 421);    // an absolute-form target's host is checked too
        QCOMPARE(status(wsHandshake("http://evil.example:abc/ws", auth)), 400);
        // Accepted handshakes are upgraded by QWebSocketServer from the bytes the check only peeked at.
        QCOMPARE(status(wsHandshake("/ws", auth)), 101);
        QCOMPARE(status(wsHandshake("/ws?token=" + kToken)), 101);
        QCOMPARE(status(wsHandshake("/", auth + "Origin: http://127.0.0.1:7760\r\n")), 101);
        QVERIFY(hub.stats().value("refusedHandshakes").toInt() >= 10);
        hub.close();
    }

    void wsReplayMemory() {
        WsHub hub;
        HubOptions ho;
        ho.ringSize = 64;
        hub.setOptions(ho);
        hub.setAuth(&m_auth);
        hub.setAllowedHosts(allowedHostsFor(QHostAddress(QHostAddress::LocalHost)));
        QVERIFY(hub.listen(QHostAddress(QHostAddress::LocalHost), 0));
        const QString base = QStringLiteral("ws://127.0.0.1:%1/ws").arg(hub.port());
        for (int i = 1; i <= 5; ++i) QVERIFY(hub.publish(QStringLiteral("job"), QStringLiteral("p1"), QJsonObject{{"n", i}}));
        QCOMPARE(hub.lastSeq(), qint64(5));

        auto a = std::make_unique<WsClient>();
        QVERIFY(a->open(QUrl(base + QStringLiteral("?since=2"))));
        QVERIFY(a->wait(4));
        QJsonObject hello = a->hello();
        QCOMPARE(hello.value("type").toString(), QStringLiteral("hello"));
        QCOMPARE(hello.value("lastSeq").toInt(), 5);
        QCOMPARE(hello.value("since").toInt(), 2);
        QCOMPARE(hello.value("replay").toInt(), 3);
        QCOMPARE(hello.value("mode").toString(), QStringLiteral("memory"));
        const QString epoch = hello.value("epoch").toString();
        QVERIFY(epoch.startsWith(QStringLiteral("mem-")));
        QCOMPARE(epoch, hub.epoch());
        QVERIFY(!hello.contains("reset"));
        QCOMPARE(a->seqs(), range(3, 5));
        QCOMPARE(a->msgs.last().value("data").toObject().value("n").toInt(), 5);
        QCOMPARE(a->msgs.last().value("projectId").toString(), QStringLiteral("p1"));
        hub.publish(QStringLiteral("run"), {}, QJsonObject{{"n", 6}});
        QVERIFY(a->waitSeq(6));
        QCOMPARE(a->seqs(), range(3, 6));
        a->close();
        QVERIFY(QTest::qWaitFor([&]() { return hub.clientCount() == 0; }, 3000));

        // Offline while 7 and 8 happen; reconnecting with since=6 and the hello's epoch (token in the query, as a browser
        // does) gets exactly them.
        hub.publish(QStringLiteral("job"), QStringLiteral("p1"), QJsonObject{{"n", 7}});
        hub.publish(QStringLiteral("job"), QStringLiteral("p2"), QJsonObject{{"n", 8}});
        WsClient b;
        QVERIFY(b.open(QUrl(base + QStringLiteral("?since=6&epoch=%1&token=").arg(epoch) + QString::fromLatin1(kToken)), false));
        QVERIFY(b.wait(3));
        QCOMPARE(b.hello().value("replay").toInt(), 2);
        QCOMPARE(b.seqs(), range(7, 8));
        hub.publish(QStringLiteral("job"), QStringLiteral("p1"), QJsonObject{{"n", 9}});
        QVERIFY(b.waitSeq(9));
        QCOMPARE(b.seqs(), range(7, 9));

        // Without ?since: live only. With a filter: that project's events plus project-less ones, of the listed types.
        WsClient live, filtered;
        QVERIFY(live.open(QUrl(base)));
        QVERIFY(filtered.open(QUrl(base + QStringLiteral("?since=0&project=p1&types=job,budget"))));
        QVERIFY(live.wait(1));
        QVERIFY(filtered.wait(8));                       // hello + 7 replayed
        QCOMPARE(live.hello().value("replay").toInt(), 0);
        QCOMPARE(filtered.hello().value("replay").toInt(), 7);
        QCOMPARE(filtered.seqs(), QList<qint64>({1, 2, 3, 4, 5, 7, 9}));   // 6 is a run, 8 is p2
        hub.publish(QStringLiteral("budget"), {}, QJsonObject{});            // 10: no project
        hub.publish(QStringLiteral("job"), QStringLiteral("p2"), QJsonObject{});   // 11
        hub.publish(QStringLiteral("job"), QStringLiteral("p1"), QJsonObject{});   // 12
        QVERIFY(live.waitSeq(12));
        QVERIFY(filtered.waitSeq(12));
        QCOMPARE(live.seqs(), range(10, 12));
        QCOMPARE(filtered.seqs(), QList<qint64>({1, 2, 3, 4, 5, 7, 9, 10, 12}));

        // Resets: a seq from the future (a new database), and history the ring no longer has.
        WsClient ahead;
        QVERIFY(ahead.open(QUrl(base + QStringLiteral("?since=500"))));
        QVERIFY(ahead.wait(1));
        QCOMPARE(ahead.hello().value("reset").toString(), QStringLiteral("ahead"));
        QCOMPARE(ahead.hello().value("replay").toInt(), 0);
        hub.publish(QStringLiteral("job"), {}, QJsonObject{});              // 13 still reaches it live
        QVERIFY(ahead.waitSeq(13));
        QCOMPARE(ahead.seqs(), QList<qint64>({13}));
        // A seq of another numbering (another process): a reset, not unrelated events replayed as if they continued.
        WsClient stranger;
        QVERIFY(stranger.open(QUrl(base + QStringLiteral("?since=2&epoch=mem-0123456789abcdef"))));
        QVERIFY(stranger.wait(1));
        QCOMPARE(stranger.hello().value("reset").toString(), QStringLiteral("epoch"));
        QCOMPARE(stranger.hello().value("replay").toInt(), 0);
        hub.publish(QStringLiteral("job"), {}, QJsonObject{});              // 14: live from now on
        QVERIFY(stranger.waitSeq(14));
        QCOMPARE(stranger.seqs(), QList<qint64>({14}));

        // ping
        live.ws.sendTextMessage(QStringLiteral(R"({"op":"ping"})"));
        QVERIFY(QTest::qWaitFor([&]() { return live.msgs.last().value("type").toString() == QLatin1String("pong"); }, 3000));
        QCOMPARE(live.msgs.last().value("lastSeq").toInt(), 14);
        QCOMPARE(live.msgs.last().value("epoch").toString(), epoch);

        WsHub small;
        HubOptions so;
        so.ringSize = 3;
        small.setOptions(so);
        small.setAuth(&m_auth);
        QVERIFY(small.listen(QHostAddress(QHostAddress::LocalHost), 0));
        for (int i = 0; i < 10; ++i) small.publish(QStringLiteral("job"), {}, QJsonObject{});
        WsClient gap, recent;
        QVERIFY(gap.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=2").arg(small.port()))));
        QVERIFY(recent.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=7").arg(small.port()))));
        QVERIFY(gap.wait(1));
        QVERIFY(recent.wait(4));
        QCOMPARE(gap.hello().value("reset").toString(), QStringLiteral("gap"));
        QVERIFY(gap.seqs().isEmpty());
        QCOMPARE(recent.seqs(), range(8, 10));                    // no epoch sent: replayed as before
        QVERIFY(small.stats().value("resets").toInt() >= 1);
        // `small` stands for the first hub's process restarted in memory mode: it numbers from 1 again, so a client of
        // the old one (since=8 and the old epoch) gets a reset instead of 9..10 of the new numbering.
        QVERIFY(small.epoch().startsWith(QStringLiteral("mem-")) && small.epoch() != epoch);
        WsClient restarted;
        QVERIFY(restarted.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=8&epoch=%2").arg(small.port()).arg(epoch))));
        QVERIFY(restarted.wait(1));
        QCOMPARE(restarted.hello().value("reset").toString(), QStringLiteral("epoch"));
        QVERIFY(restarted.seqs().isEmpty());
    }

    void wsSlowConsumer() {
        WsHub hub;
        HubOptions ho;
        ho.maxBufferedBytes = 1024;
        hub.setOptions(ho);
        hub.setAuth(&m_auth);
        QVERIFY(hub.listen(QHostAddress(QHostAddress::LocalHost), 0));
        WsClient c;
        QVERIFY(c.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(hub.port()))));
        QVERIFY(c.wait(1));
        hub.publish(QStringLiteral("job"), {}, QJsonObject{{"pad", QString(256 * 1024, QLatin1Char('x'))}});
        QVERIFY(QTest::qWaitFor([&]() { return c.ws.state() == QAbstractSocket::UnconnectedState; }, 5000));
        QCOMPARE(int(c.ws.closeCode()), 4008);
        QCOMPARE(hub.clientCount(), 0);
        QCOMPARE(hub.stats().value("slowDropped").toInt(), 1);
    }

    void wsReplayStore() {
        const QString path = m_tmp.filePath(QStringLiteral("db/studio.db"));
        store::DbOptions dopt;
        dopt.path = path;
        dopt.batchWindowMs = 20;
        store::Db db(dopt);
        QString err;
        QVERIFY2(db.open(&err), qPrintable(err));
        QString epoch1;                                     // the numbering of this studio.db
        {
            WsHub hub;
            hub.setAuth(&m_auth);
            hub.attachStore(&db);
            QVERIFY(hub.hasStore());
            QCOMPARE(hub.lastSeq(), qint64(0));
            QVERIFY(hub.epoch().isEmpty());                 // an empty table has no numbering yet
            QVERIFY(hub.listen(QHostAddress(QHostAddress::LocalHost), 0));
            WsClient c;
            QVERIFY(c.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=0").arg(hub.port()))));
            QVERIFY(c.wait(1));
            QCOMPARE(c.hello().value("mode").toString(), QStringLiteral("store"));
            QCOMPARE(c.hello().value("epoch").toString(), QString());
            for (int i = 1; i <= 10; ++i) QVERIFY(hub.publish(QStringLiteral("job"), QStringLiteral("p%1").arg(i % 2), QJsonObject{{"n", i}}));
            QCOMPARE(hub.lastSeq(), qint64(0));            // broadcast only after the commit
            QVERIFY(c.waitSeq(10));
            QCOMPARE(c.seqs(), range(1, 10));
            QCOMPARE(hub.lastSeq(), qint64(10));
            epoch1 = hub.epoch();                           // named by its first event
            QVERIFY(epoch1.startsWith(QStringLiteral("db-")));
            c.ws.sendTextMessage(QStringLiteral(R"({"op":"ping"})"));   // a client that connected before learns it
            QVERIFY(QTest::qWaitFor([&]() { return c.msgs.last().value("type").toString() == QLatin1String("pong"); }, 3000));
            QCOMPARE(c.msgs.last().value("epoch").toString(), epoch1);
            // Every broadcast event is in the table.
            QCOMPARE(store::events::lastSeq(db.reader()), qint64(10));
            hub.close();
        }
        {
            // A "restart": a new hub over the same table. The ring is empty, so replay reads the table.
            WsHub hub;
            hub.setAuth(&m_auth);
            hub.attachStore(&db);
            QCOMPARE(hub.lastSeq(), qint64(10));
            QCOMPARE(hub.epoch(), epoch1);                  // the same table after a restart: the same numbering
            QVERIFY(hub.listen(QHostAddress(QHostAddress::LocalHost), 0));
            const QString base = QStringLiteral("ws://127.0.0.1:%1/ws").arg(hub.port());
            WsClient c;
            QVERIFY(c.open(QUrl(base + QStringLiteral("?since=4&epoch=") + epoch1)));
            QVERIFY(c.wait(7));
            QCOMPARE(c.hello().value("replay").toInt(), 6);
            QCOMPARE(c.seqs(), range(5, 10));
            QCOMPARE(c.msgs.last().value("data").toObject().value("n").toInt(), 10);
            QCOMPARE(c.msgs.last().value("projectId").toString(), QStringLiteral("p0"));
            WsClient p1;
            QVERIFY(p1.open(QUrl(base + QStringLiteral("?since=4&project=p1"))));
            QVERIFY(p1.wait(4));
            QCOMPARE(p1.seqs(), QList<qint64>({5, 7, 9}));

            // A client too far behind is told to reload instead of getting a huge replay.
            WsHub tiny;
            HubOptions ho;
            ho.maxReplay = 8;
            tiny.setOptions(ho);
            tiny.setAuth(&m_auth);
            tiny.attachStore(&db);
            QVERIFY(tiny.listen(QHostAddress(QHostAddress::LocalHost), 0));
            WsClient far, near;
            QVERIFY(far.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=0").arg(tiny.port()))));
            QVERIFY(near.open(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=3").arg(tiny.port()))));
            QVERIFY(far.wait(1));
            QVERIFY(near.wait(8));
            QCOMPARE(far.hello().value("reset").toString(), QStringLiteral("too_far"));   // 10 > maxReplay 8
            QVERIFY(far.seqs().isEmpty());
            QCOMPARE(near.seqs(), range(4, 10));
            tiny.close();

            // A burst while clients connect: whatever is committed, read from the table, in the ring or still in
            // flight, each client sees every seq exactly once, in order (since=4 replays from the table and the ring,
            // since=<lastSeq> from the ring or live).
            for (int i = 0; i < 100; ++i) QVERIFY(hub.publish(QStringLiteral("job"), {}, QJsonObject{{"burst", i}}));
            const qint64 at = hub.lastSeq();
            WsClient mid, early;
            QVERIFY(mid.open(QUrl(base + QStringLiteral("?since=%1").arg(at))));
            QVERIFY(early.open(QUrl(base + QStringLiteral("?since=4"))));
            QVERIFY(mid.waitSeq(110, 10000));
            QVERIFY(early.waitSeq(110, 10000));
            QCOMPARE(mid.seqs(), range(at + 1, 110));
            QCOMPARE(early.seqs(), range(5, 110));
            QVERIFY(c.waitSeq(110, 10000));
            QCOMPARE(c.seqs(), range(5, 110));
            QCOMPARE(store::events::lastSeq(db.reader()), qint64(110));
            QCOMPARE(hub.stats().value("persistFailed").toInt(), 0);

            // "ahead" is judged against the table, not the last broadcast: a hub whose broadcasts trail the commits
            // (here one attached at 110 while `hub` commits and broadcasts 111..113) replays what is committed instead
            // of a spurious reset, and only a seq past the table is "ahead".
            WsHub trailing;
            trailing.setAuth(&m_auth);
            trailing.attachStore(&db);
            QCOMPARE(trailing.epoch(), epoch1);
            QVERIFY(trailing.listen(QHostAddress(QHostAddress::LocalHost), 0));
            for (int i = 0; i < 3; ++i) QVERIFY(hub.publish(QStringLiteral("job"), {}, QJsonObject{{"late", i}}));
            QVERIFY(QTest::qWaitFor([&]() { return hub.lastSeq() == 113; }, 5000));
            QCOMPARE(trailing.lastSeq(), qint64(110));
            const QString tbase = QStringLiteral("ws://127.0.0.1:%1/ws?epoch=%2&since=").arg(trailing.port()).arg(epoch1);
            WsClient behind, current, future;
            QVERIFY(behind.open(QUrl(tbase + QStringLiteral("111"))));
            QVERIFY(current.open(QUrl(tbase + QStringLiteral("113"))));
            QVERIFY(future.open(QUrl(tbase + QStringLiteral("114"))));
            QVERIFY(behind.wait(3));
            QVERIFY(current.wait(1));
            QVERIFY(future.wait(1));
            QVERIFY(!behind.hello().contains("reset"));
            QCOMPARE(behind.seqs(), range(112, 113));
            QVERIFY(!current.hello().contains("reset"));
            QCOMPARE(current.hello().value("lastSeq").toInt(), 113);
            QCOMPARE(current.hello().value("replay").toInt(), 0);
            QCOMPARE(future.hello().value("reset").toString(), QStringLiteral("ahead"));
            trailing.close();

            QVERIFY(!hub.publish(QString(), {}, QJsonObject{}));   // an event needs a type
            hub.close();
        }
        {
            // A recreated studio.db numbers from 1 again: its epoch differs, so a client of the old database gets a reset
            // instead of the new database's events as if they continued its stream.
            store::DbOptions o2 = dopt;
            o2.path = m_tmp.filePath(QStringLiteral("db2/studio.db"));
            store::Db db2(o2);
            QVERIFY2(db2.open(&err), qPrintable(err));
            WsHub fresh;
            fresh.setAuth(&m_auth);
            fresh.attachStore(&db2);
            QVERIFY(fresh.epoch().isEmpty());
            QVERIFY(fresh.listen(QHostAddress(QHostAddress::LocalHost), 0));
            for (int i = 1; i <= 5; ++i) QVERIFY(fresh.publish(QStringLiteral("job"), {}, QJsonObject{{"n", i}}));
            QVERIFY(QTest::qWaitFor([&]() { return fresh.lastSeq() == 5; }, 5000));
            QVERIFY(fresh.epoch().startsWith(QStringLiteral("db-")));
            QVERIFY(fresh.epoch() != epoch1);
            const QString base2 = QStringLiteral("ws://127.0.0.1:%1/ws?since=2&epoch=").arg(fresh.port());
            WsClient old, mine;
            QVERIFY(old.open(QUrl(base2 + epoch1)));
            QVERIFY(mine.open(QUrl(base2 + fresh.epoch())));
            QVERIFY(old.wait(1));
            QVERIFY(mine.wait(4));
            QCOMPARE(old.hello().value("reset").toString(), QStringLiteral("epoch"));
            QVERIFY(old.seqs().isEmpty());
            QCOMPARE(mine.seqs(), range(3, 5));
            fresh.close();
            fresh.detachStore();
            db2.close();
        }
        // The store closed under a hub (ST-14 detaches first; this must still not touch the stopped writer).
        WsHub late;
        late.setAuth(&m_auth);
        late.attachStore(&db);
        QCOMPARE(late.lastSeq(), qint64(113));
        db.close();
        QVERIFY(!late.publish(QStringLiteral("job"), {}, QJsonObject{}));
        QCOMPARE(late.stats().value("persistRefused").toInt(), 1);
        late.detachStore();
    }

    void serviceWiring() {
        const QString dir = m_tmp.filePath(QStringLiteral("svc"));
        StudioService svc;
        ServiceOptions opt;
        opt.port = 0;
        opt.wsPort = 0;
        opt.dataDir = dir;
        QString err;
        QVERIFY2(svc.start(opt, &err), qPrintable(err));
        QVERIFY(svc.wsPort() > 0 && svc.wsPort() != svc.port());
        const QString token = Auth::readTokenFile(svc.tokenPath(), &err);
        QVERIFY2(!token.isEmpty(), qPrintable(err));
        QCOMPARE(exchange(svc.port(), "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 401);
        Resp r = exchange(svc.port(), "GET /api/v1/status HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token.toLatin1() + "\r\n\r\n");
        QCOMPARE(r.status, 200);
        QCOMPARE(jsonOf(r.body).value("service").toString(), QStringLiteral("playplay-studio"));
        QCOMPARE(exchange(svc.port(), "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n").status, 200);
        QCOMPARE(exchange(svc.port(), "GET /health HTTP/1.1\r\nHost: rebound.example\r\n\r\n").status, 421);
        {
            Raw c(svc.wsPort());
            c.send(wsHandshake("/ws"));
            Resp h;
            QVERIFY(c.next(&h, 3000));
            QCOMPARE(h.status, 401);
        }
        QWebSocket ws;
        QStringList got;
        QObject::connect(&ws, &QWebSocket::textMessageReceived, &ws, [&](const QString& t) { got << t; });
        QNetworkRequest rq(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws?since=0").arg(svc.wsPort())));
        rq.setRawHeader("Authorization", "Bearer " + token.toLatin1());
        ws.open(rq);
        QVERIFY(QTest::qWaitFor([&]() { return !got.isEmpty(); }, 5000));
        QCOMPARE(jsonOf(got.first().toUtf8()).value("type").toString(), QStringLiteral("hello"));
        svc.hub().publish(QStringLiteral("project"), QStringLiteral("prj_1"), QJsonObject{{"title", "x"}});
        QVERIFY(QTest::qWaitFor([&]() { return got.size() >= 2; }, 5000));
        QCOMPARE(jsonOf(got.last().toUtf8()).value("seq").toInt(), 1);
        ws.close();
        svc.stop();
        QVERIFY(!svc.isListening());

        // Option guards for the WS port.
        ServiceOptions bad = opt;
        bad.wsPort = 7701;
        QVERIFY(checkOptions(bad).contains(QStringLiteral("7701")));
        bad.wsPort = -2;
        QVERIFY(!checkOptions(bad).isEmpty());
        bad.port = 7770;
        bad.wsPort = 7770;
        QVERIFY(checkOptions(bad).contains(QStringLiteral("differ")));
        bad.wsPort = -1;
        QVERIFY(checkOptions(bad).isEmpty());

        // A malformed token file refuses start-up; nothing listens.
        const QString broken = m_tmp.filePath(QStringLiteral("broken"));
        QVERIFY(QDir().mkpath(broken));
        {
            QFile f(broken + QStringLiteral("/studio-token"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        StudioService other;
        ServiceOptions o2 = opt;
        o2.dataDir = broken;
        QVERIFY(!other.start(o2, &err));
        QVERIFY(err.contains(QStringLiteral("studio-token")));
        QVERIFY(!other.isListening());
        QCOMPARE(other.wsPort(), quint16(0));

        // PlayPlayStudio.exe --print-token prints the token of that data dir (creating it when missing).
        QProcess p;
        p.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--print-token"), QStringLiteral("--data"), dir});
        QVERIFY2(p.waitForFinished(15000), qPrintable(p.errorString()));
        QCOMPARE(p.exitCode(), 0);
        QCOMPARE(QString::fromLatin1(p.readAllStandardOutput()).trimmed(), token);
        const QString fresh = m_tmp.filePath(QStringLiteral("fresh"));
        QProcess q;
        q.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--print-token"), QStringLiteral("--data"), fresh});
        QVERIFY(q.waitForFinished(15000));
        QCOMPARE(q.exitCode(), 0);
        QCOMPARE(QString::fromLatin1(q.readAllStandardOutput()).trimmed(), Auth::readTokenFile(fresh + QStringLiteral("/studio-token")));
        QProcess r2;
        r2.start(QStringLiteral(STUDIO_EXE_PATH), {QStringLiteral("--print-token"), QStringLiteral("--data"), broken});
        QVERIFY(r2.waitForFinished(15000));
        QCOMPARE(r2.exitCode(), 1);
        QVERIFY(r2.readAllStandardOutput().isEmpty());
    }

    void cleanupTestCase() {
        m_http.close();
        QVERIFY(!m_http.isListening());
        QVERIFY(QTest::qWaitFor([this]() { return m_http.stats().open == 0; }, 3000));
        log::stop();
    }
};

QTEST_GUILESS_MAIN(TestStudioHttp)
#include "test_studio_http.moc"
