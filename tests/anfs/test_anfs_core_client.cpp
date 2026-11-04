// test_anfs_core_client - ANFS-F5: the WinHTTP client (core/http_client.cpp) against the ANFS-F4 server, a raw
// capture server and a local QSslServer, all on ephemeral loopback ports.
//
// - Raw capture of the exact request bytes WinHTTP sends: header order, no User-Agent / Accept, Host from the URL,
//   caller Host / Content-Length / Transfer-Encoding dropped, the computed Content-Length, "Content-Length: 0" rules,
//   chunked framing written by the client, latin1 header bytes, path escaping, a 5 GiB declared length, CR/LF refused.
// - Response headers lower-cased, trimmed, duplicates in order, latin1 bytes; keep-alive reuse (one TCP connection for
//   sequential requests on one client).
// - Redirects never followed (302, 307 with a body).
// - Streaming GET with Range against the F4 sendFile (206 + Content-Range, full body in odd-sized reads, HEAD, 404,
//   chunked responses).
// - A 2 GiB chunked POST through the F4 server with flat private bytes, every byte verified; a 64 MiB Content-Length
//   POST.
// - Inactivity timeouts with an injected 300 ms (no reply, a stalled body), while a slow trickle keeps going.
// - cancel() from another thread: waiting for the reply, mid-body, during an upload the server does not read, one
//   token for two requests, a token cancelled before the request.
// - https against a QSslServer with a self-signed certificate (made by the conda openssl at run time).
// - fetch(): never throws; truncated bodies, readAll limits, read after close, bad URLs, refused connections.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <winhttp.h>

#include "core/http_client.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/json.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslKey>
#include <QtNetwork/QSslServer>
#include <QtNetwork/QSslSocket>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace anfs;
using namespace anfs::http;

#define SEQ(a, b) QCOMPARE(QString::fromLatin1(std::string(a).c_str()), QString::fromLatin1(std::string(b).c_str()))

namespace {

struct Wsa {
    Wsa() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~Wsa() { WSACleanup(); }
};

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool waitFor(const std::function<bool()>& cond, int ms)
{
    int64_t until = nowMs() + ms;
    while (nowMs() < until) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

template <class F>
std::optional<ClientError> clientError(F&& f)
{
    try {
        f();
    } catch (const ClientError& e) {
        return e;
    } catch (const std::exception& e) {
        qWarning("unexpected exception: %s", e.what());
        throw;
    }
    return std::nullopt;
}

bool ieq(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

// ---- raw capture server ----------------------------------------------------------------------------------------------

class RawServer;

// One accepted connection; reads poll in 50 ms slices so a stopping server never waits on a blocked recv.
class RawConn {
public:
    RawConn(SOCKET s, const std::atomic<bool>& stop) : s_(s), stop_(stop) {}
    bool stopping() const { return stop_.load(); }
    // false on EOF, error or stop
    bool fill()
    {
        char tmp[65536];
        for (;;) {
            if (stopping()) return false;
            int k = ::recv(s_, tmp, sizeof tmp, 0);
            if (k > 0) {
                buf_.append(tmp, static_cast<size_t>(k));
                return true;
            }
            if (k == 0 || WSAGetLastError() != WSAETIMEDOUT) return false;
        }
    }
    std::string readUntil(const std::string& delim)
    {
        size_t p;
        while ((p = buf_.find(delim)) == std::string::npos)
            if (!fill()) return {};
        std::string r = buf_.substr(0, p + delim.size());
        buf_.erase(0, p + delim.size());
        return r;
    }
    std::string readN(size_t n)
    {
        while (buf_.size() < n)
            if (!fill()) break;
        std::string r = buf_.substr(0, std::min(n, buf_.size()));
        buf_.erase(0, r.size());
        return r;
    }
    // One whole request as raw bytes (head + body as framed on the wire); "" at EOF.
    std::string readRequest()
    {
        std::string head = readUntil("\r\n\r\n");
        if (head.empty()) return {};
        std::string lower = head;
        for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        size_t cl = lower.find("\r\ncontent-length:");
        if (lower.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
            std::string raw;
            for (;;) {
                std::string line = readUntil("\r\n");
                if (line.empty()) return head + raw;
                raw += line;
                size_t n = std::strtoull(line.c_str(), nullptr, 16);
                if (n == 0) {
                    raw += readUntil("\r\n");
                    break;
                }
                raw += readN(n + 2);
            }
            return head + raw;
        }
        if (cl != std::string::npos) {
            int64_t n = std::atoll(head.c_str() + cl + 17);
            if (n > (16 << 20)) return head;                    // declared huge: the caller handles the body
            return head + readN(static_cast<size_t>(n));
        }
        return head;
    }
    void send(const std::string& d)
    {
        size_t at = 0;
        while (at < d.size()) {
            int k = ::send(s_, d.data() + at, static_cast<int>(d.size() - at), 0);
            if (k <= 0) return;
            at += static_cast<size_t>(k);
        }
    }
    // Waits until the client closes or the server stops (never answers).
    void hold()
    {
        while (fill()) buf_.clear();
    }
    void sleepMs(int ms)
    {
        int64_t until = nowMs() + ms;
        while (nowMs() < until && !stopping()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void close() { shutdown(s_, SD_BOTH); }

private:
    SOCKET s_;
    const std::atomic<bool>& stop_;
    std::string buf_;
};

class RawServer {
public:
    using Handler = std::function<void(RawConn&)>;
    explicit RawServer(Handler h, int rcvBuf = 0) : h_(std::move(h))
    {
        ls_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (rcvBuf) setsockopt(ls_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof rcvBuf);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(ls_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        listen(ls_, 64);
        int len = sizeof a;
        getsockname(ls_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] {
            for (;;) {
                SOCKET s = accept(ls_, nullptr, nullptr);
                if (s == INVALID_SOCKET) break;
                DWORD t = 50;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
                std::lock_guard<std::mutex> g(mu_);
                ++accepts_;
                socks_.push_back(s);
                workers_.emplace_back([this, s] {
                    RawConn c(s, stop_);
                    try {
                        h_(c);
                    } catch (...) {
                    }
                });
            }
        });
    }
    ~RawServer() { stop(); }
    int port() const { return port_; }
    int accepts() const { return accepts_.load(); }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    void stop()
    {
        if (stop_.exchange(true)) return;
        closesocket(ls_);
        th_.join();
        std::vector<std::thread> ws;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (SOCKET s : socks_) shutdown(s, SD_BOTH);
            ws.swap(workers_);
        }
        for (auto& w : ws) w.join();
        for (SOCKET s : socks_) closesocket(s);
    }
    // Requests captured by handlers that call record().
    void record(std::string r)
    {
        std::lock_guard<std::mutex> g(mu_);
        captured_.push_back(std::move(r));
    }
    std::vector<std::string> captured()
    {
        std::lock_guard<std::mutex> g(mu_);
        return captured_;
    }

private:
    Handler h_;
    SOCKET ls_ = INVALID_SOCKET;
    int port_ = 0;
    std::thread th_;
    std::mutex mu_;
    std::vector<SOCKET> socks_;
    std::vector<std::thread> workers_;
    std::vector<std::string> captured_;
    std::atomic<int> accepts_{0};
    std::atomic<bool> stop_{false};
};

// ---- F4 server -----------------------------------------------------------------------------------------------------

std::shared_ptr<const Config> cfg0()
{
    static auto c = std::make_shared<const Config>();
    return c;
}

struct TestServer {
    TestServer() : server(router, [] { return cfg0(); }, nullptr, ServerOptions{}) {}
    void start() { server.start(0, false, "127.0.0.1"); }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(server.port()) + path; }
    Router router;
    Server server;
};

// ---- byte patterns (word based, so 2 GiB are generated and checked at memory speed) ------------------------------

inline uint64_t patWord(uint64_t i) { return (i + 1) * 0x9E3779B97F4A7C15ull; }
inline char patByte(uint64_t o) { return static_cast<char>(patWord(o >> 3) >> (8 * (o & 7))); }

void fillPattern(char* p, uint64_t off, size_t n)
{
    while (n && (off & 7)) {
        *p++ = patByte(off++);
        --n;
    }
    while (n >= 8) {
        uint64_t w = patWord(off >> 3);
        std::memcpy(p, &w, 8);
        p += 8;
        off += 8;
        n -= 8;
    }
    while (n) {
        *p++ = patByte(off++);
        --n;
    }
}

size_t patMismatches(const char* p, uint64_t off, size_t n)
{
    size_t bad = 0;
    while (n && (off & 7)) {
        bad += *p++ != patByte(off++);
        --n;
    }
    while (n >= 8) {
        uint64_t w;
        std::memcpy(&w, p, 8);
        bad += w != patWord(off >> 3);
        p += 8;
        off += 8;
        n -= 8;
    }
    while (n) {
        bad += *p++ != patByte(off++);
        --n;
    }
    return bad;
}

class PatternSource final : public Source {
public:
    explicit PatternSource(int64_t total, bool announceLength = false) : total_(total), announce_(announceLength) {}
    size_t read(char* buf, size_t n) override
    {
        size_t k = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(n), total_ - pos_));
        fillPattern(buf, static_cast<uint64_t>(pos_), k);
        pos_ += static_cast<int64_t>(k);
        return k;
    }
    int64_t length() const override { return announce_ ? total_ : -1; }
    int64_t pos() const { return pos_; }

private:
    int64_t total_;
    int64_t pos_ = 0;
    bool announce_;
};

class VerifySink final : public Sink {
public:
    using Sink::write;
    void write(const char* p, size_t n) override
    {
        bad += static_cast<int64_t>(patMismatches(p, static_cast<uint64_t>(off), n));
        off += static_cast<int64_t>(n);
    }
    int64_t off = 0;
    int64_t bad = 0;
};

// Pieces of a body in fixed sizes ("abc", "defgh"), for exact chunk framing.
class PiecesSource final : public Source {
public:
    explicit PiecesSource(std::vector<std::string> p) : p_(std::move(p)) {}
    size_t read(char* buf, size_t n) override
    {
        if (i_ >= p_.size()) return 0;
        size_t k = std::min(n, p_[i_].size());
        std::memcpy(buf, p_[i_].data(), k);
        ++i_;
        return k;
    }

private:
    std::vector<std::string> p_;
    size_t i_ = 0;
};

int64_t privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX m{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof m);
    return static_cast<int64_t>(m.PrivateUsage);
}

// Samples private bytes every 10 ms until stopped; peak() is the highest sample.
class MemPeak {
public:
    MemPeak() : base_(privateBytes()), peak_(base_)
    {
        th_ = std::thread([this] {
            while (!done_) {
                int64_t v = privateBytes();
                if (v > peak_) peak_ = v;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~MemPeak() { stop(); }
    void stop()
    {
        if (!done_.exchange(true)) th_.join();
    }
    int64_t grew() const { return peak_.load() - base_; }

private:
    int64_t base_;
    std::atomic<int64_t> peak_;
    std::atomic<bool> done_{false};
    std::thread th_;
};

// ---- TLS server on its own Qt thread -------------------------------------------------------------------------------

class TlsServer {
public:
    ~TlsServer() { stop(); }
    bool start(const QByteArray& certPem, const QByteArray& keyPem, QString* why)
    {
        ctx_ = new QObject;
        ctx_->moveToThread(&th_);
        th_.start();
        bool ok = false;
        QMetaObject::invokeMethod(ctx_, [&] {
            QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
            QSslCertificate cert(certPem, QSsl::Pem);
            QSslKey key(keyPem, QSsl::Rsa, QSsl::Pem);
            if (cert.isNull() || key.isNull()) {
                *why = "cannot load the generated certificate or key";
                return;
            }
            cfg.setLocalCertificate(cert);
            cfg.setPrivateKey(key);
            cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
            srv_ = new QSslServer(ctx_);
            srv_->setSslConfiguration(cfg);
            QObject::connect(srv_, &QSslServer::pendingConnectionAvailable, srv_, [this] {
                while (QTcpSocket* t = srv_->nextPendingConnection()) {
                    auto buf = std::make_shared<QByteArray>();
                    auto onData = [this, t, buf] {
                        buf->append(t->readAll());
                        int e = buf->indexOf("\r\n\r\n");
                        if (e < 0) return;
                        {
                            std::lock_guard<std::mutex> g(mu_);
                            heads_.push_back(buf->left(e + 4).toStdString());
                        }
                        buf->remove(0, e + 4);
                        t->write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 6\r\n\r\ntls ok");
                    };
                    QObject::connect(t, &QTcpSocket::readyRead, t, onData);
                    QObject::connect(t, &QTcpSocket::disconnected, t, &QObject::deleteLater);
                    onData();
                }
            });
            ok = srv_->listen(QHostAddress::LocalHost, 0);
            if (ok) port_ = srv_->serverPort();
            else *why = srv_->errorString();
        }, Qt::BlockingQueuedConnection);
        return ok;
    }
    void stop()
    {
        if (!ctx_) return;
        QMetaObject::invokeMethod(ctx_, [this] {
            delete srv_;
            srv_ = nullptr;
        }, Qt::BlockingQueuedConnection);
        th_.quit();
        th_.wait();
        delete ctx_;
        ctx_ = nullptr;
    }
    int port() const { return port_; }
    std::vector<std::string> heads()
    {
        std::lock_guard<std::mutex> g(mu_);
        return heads_;
    }

private:
    QThread th_;
    QObject* ctx_ = nullptr;
    QSslServer* srv_ = nullptr;
    int port_ = 0;
    std::mutex mu_;
    std::vector<std::string> heads_;
};

const char* kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

} // namespace

class TestAnfsCoreClient : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { qInfo("WinHTTP client tests on ephemeral loopback ports"); }

    // ---- exact request bytes ---------------------------------------------------------------------------------------
    void requestBytesExact()
    {
        Wsa wsa;
        RawServer* self = nullptr;                    // set before the first connection
        RawServer srv([&](RawConn& c) {
            for (;;) {
                std::string r = c.readRequest();
                if (r.empty()) return;
                self->record(r);
                c.send(kOk);
            }
        });
        self = &srv;
        const std::string host = "127.0.0.1:" + std::to_string(srv.port());
        HttpClient client;

        {   // GET: caller order kept, caller Host / Content-Length / Transfer-Encoding dropped, no UA, no Accept added
            ClientRequest q;
            q.url = srv.url("/a/b?x=1&auth=zz#frag");
            q.headers.add("X-Zeta", "1");
            q.headers.add("accept", "*/*");
            q.headers.add("Host", "evil.example");
            q.headers.add("X-Alpha", "two  words");
            q.headers.add("Content-Length", "99");
            q.headers.add("Transfer-Encoding", "gzip");
            q.headers.add("x-agent-auth", "tok");
            q.headers.add("range", "bytes=0-");
            q.headers.add("authorization", "Basic eDp5");
            q.headers.add("user-agent", "anfs-test");
            q.headers.add("If-None-Match", "W/\"1-2\"");
            q.headers.add("cookie", "c=1");
            q.headers.add("referer", "http://r/");
            q.headers.add("accept-encoding", "identity");
            q.headers.add("Connection", "keep-alive");
            auto r = client.send(q);
            QCOMPARE(r->status(), 200);
            SEQ(r->readAll(), "ok");
        }
        {   // POST with a known length
            ClientRequest q;
            q.method = "POST";
            q.url = srv.url("/p");
            q.headers.add("Content-Type", "application/json");
            StringSource body("{\"a\":1}");
            q.body = &body;
            q.bodyLength = 7;
            SEQ(client.send(q)->readAll(), "ok");
        }
        SEQ(client.fetch("POST", srv.url("/empty")).body, "ok");        // no body: Content-Length: 0
        SEQ(client.fetch("DELETE", srv.url("/d")).body, "ok");          // no body, no length
        {   // chunked framing written by the client
            ClientRequest q;
            q.method = "PUT";
            q.url = srv.url("/c");
            PiecesSource body({"abc", "defgh", std::string(20, 'z')});
            q.body = &body;
            q.bodyLength = -1;
            SEQ(client.send(q)->readAll(), "ok");
        }
        {   // latin1 header bytes go out as they are; the path is percent-encoded outside 0x21..0x7E
            Headers h;
            h.add("X-Latin", "caf\xE9 \x80");
            auto f = client.fetch("GET", srv.url("/a b/\xC3\xA9?q=a b"), h);
            if (!f.error.empty()) qWarning("%s", f.error.c_str());
            SEQ(f.body, "ok");
        }
        auto cap = srv.captured();
        QCOMPARE(int(cap.size()), 6);
        for (size_t i = 0; i < cap.size(); ++i) qInfo("request %d: %s", int(i), QByteArray::fromStdString(cap[i]).toPercentEncoding(" :/?=&*,;\"{}").constData());
        // WinHTTP's header munging (the plan's named risk), pinned here: headers it knows (Accept, Accept-Encoding,
        // Authorization, Cookie, If-None-Match, Referer, Range, User-Agent) leave the caller's order and go first in
        // its canonical case, alphabetically; a caller "Connection: keep-alive" merges into its own
        // "Connection: Keep-Alive"; everything else keeps order and spelling, Content-Type included. Receivers
        // (Express, the C# port) treat names case-insensitively and do not depend on order.
        SEQ(cap[0], "GET /a/b?x=1&auth=zz HTTP/1.1\r\nConnection: Keep-Alive\r\nAccept: */*\r\n"
                    "Accept-Encoding: identity\r\nAuthorization: Basic eDp5\r\nCookie: c=1\r\nIf-None-Match: W/\"1-2\"\r\n"
                    "Referer: http://r/\r\nRange: bytes=0-\r\nUser-Agent: anfs-test\r\nX-Zeta: 1\r\nX-Alpha: two  words\r\n"
                    "x-agent-auth: tok\r\nHost: " + host + "\r\n\r\n");
        SEQ(cap[1], "POST /p HTTP/1.1\r\nConnection: Keep-Alive\r\nContent-Type: application/json\r\nContent-Length: 7\r\nHost: " + host +
                        "\r\n\r\n{\"a\":1}");
        SEQ(cap[2], "POST /empty HTTP/1.1\r\nConnection: Keep-Alive\r\nContent-Length: 0\r\nHost: " + host + "\r\n\r\n");
        SEQ(cap[3], "DELETE /d HTTP/1.1\r\nConnection: Keep-Alive\r\nHost: " + host + "\r\n\r\n");
        SEQ(cap[4], "PUT /c HTTP/1.1\r\nConnection: Keep-Alive\r\nTransfer-Encoding: chunked\r\nHost: " + host +
                        "\r\n\r\n3\r\nabc\r\n5\r\ndefgh\r\n14\r\nzzzzzzzzzzzzzzzzzzzz\r\n0\r\n\r\n");
        SEQ(cap[5], "GET /a%20b/%C3%A9?q=a%20b HTTP/1.1\r\nConnection: Keep-Alive\r\nX-Latin: caf\xE9 \x80\r\nHost: " + host + "\r\n\r\n");
        QCOMPARE(srv.accepts(), 1);                                      // all six on one kept-alive connection

        // CR / LF / NUL in a header: refused before anything is sent (the value is not echoed)
        for (const char* bad : {"a\r\nX-Injected: 1", "a\nb", "a\rb"}) {
            ClientRequest q;
            q.url = srv.url("/x");
            q.headers.add("X-Bad", bad);
            auto e = clientError([&] { client.send(q); });
            QVERIFY(e.has_value());
            QCOMPARE(e->winError, uint32_t(ERROR_INVALID_PARAMETER));
            QVERIFY(std::string(e->what()).find("Injected") == std::string::npos);
        }
        {
            ClientRequest q;
            q.url = srv.url("/x");
            q.headers.add("Bad Name", "1");
            QVERIFY(clientError([&] { client.send(q); }).has_value());
        }
        QCOMPARE(int(srv.captured().size()), 6);
    }

    // A declared length over 4 GiB goes out as written; a body source that throws aborts the request and its
    // exception reaches the caller unchanged.
    void hugeDeclaredLengthAndThrowingSource()
    {
        Wsa wsa;
        RawServer* self = nullptr;
        RawServer srv([&](RawConn& c) {
            self->record(c.readRequest());
            c.hold();
        });
        self = &srv;
        class Throwing final : public Source {
        public:
            size_t read(char* b, size_t n) override
            {
                if (calls++ == 3) throw IoError("client went away", true);
                std::memset(b, 'x', n);
                return n;
            }
            int calls = 0;
        } src;
        HttpClient client;
        ClientRequest q;
        q.method = "POST";
        q.url = srv.url("/up");
        q.body = &src;
        q.bodyLength = int64_t(5) << 30;
        bool peerGone = false;
        try {
            client.send(q);
        } catch (const ClientError& e) {
            QFAIL(e.what());
        } catch (const IoError& e) {
            peerGone = e.peerGone && std::string(e.what()) == "client went away";
        }
        QVERIFY(peerGone);
        QVERIFY(waitFor([&] { return !srv.captured().empty(); }, 3000));
        SEQ(srv.captured()[0], "POST /up HTTP/1.1\r\nConnection: Keep-Alive\r\nContent-Length: 5368709120\r\nHost: 127.0.0.1:" +
                                   std::to_string(srv.port()) + "\r\n\r\n");
        // A source that ends before its declared length
        StringSource shortBody("abc");
        q.body = &shortBody;
        q.bodyLength = 10;
        auto e = clientError([&] { client.send(q); });
        QVERIFY(e.has_value());
        QVERIFY(std::string(e->what()).find("7 bytes before its length") != std::string::npos);
    }

    // ---- response side ---------------------------------------------------------------------------------------------
    void responseHeadersAndKeepAlive()
    {
        Wsa wsa;
        RawServer srv([&](RawConn& c) {
            while (!c.readRequest().empty())
                c.send("HTTP/1.1 201 Created\r\nX-Mixed-Case: A\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
                       "X-Latin: caf\xE9\r\nX-Space:   padded  \r\nContent-Length: 3\r\n\r\nabc");
        });
        HttpClient client;
        for (int i = 0; i < 3; ++i) {
            ClientRequest q;
            q.url = srv.url("/h");
            auto r = client.send(q);
            QCOMPARE(r->status(), 201);
            QCOMPARE(r->contentLength(), int64_t(3));
            QCOMPARE(r->length(), int64_t(3));
            std::vector<std::string> names;
            for (const auto& f : r->headers().fields()) names.push_back(f.first);
            QVERIFY(std::find(names.begin(), names.end(), "x-mixed-case") != names.end());
            for (const auto& n : names)
                for (char c : n) QVERIFY(!(c >= 'A' && c <= 'Z'));
            SEQ(r->header("X-MIXED-CASE"), "A");
            QCOMPARE(int(r->headers().getAll("set-cookie").size()), 2);
            SEQ(r->headers().joined("set-cookie"), "a=1, b=2");
            SEQ(r->header("x-latin"), "caf\xE9");
            SEQ(r->header("x-space"), "padded");
            SEQ(r->readAll(), "abc");
            QCOMPARE(r->read(nullptr, 0), size_t(0));
        }
        QCOMPARE(srv.accepts(), 1);                                      // keep-alive: one connection for three
        HttpClient other;                                                // WinHTTP pools per process, not per session
        SEQ(other.fetch("GET", srv.url("/h")).body, "abc");
        SEQ(HttpClient::shared().fetch("GET", srv.url("/h")).body, "abc");
        QVERIFY(srv.accepts() <= 3);
    }

    void noRedirects()
    {
        TestServer ts;
        std::atomic<int> hits{0};
        ts.router.get("/r", [](Ctx& c) { c.res.redirect("/target", 302); });
        ts.router.post("/r307", [](Ctx& c) { c.res.redirect("/target", 307); });
        ts.router.all("/target", [&](Ctx& c) {
            ++hits;
            c.json(200, JVal::object({{"t", true}}));
        });
        ts.start();
        HttpClient client;
        auto a = client.fetch("GET", ts.url("/r"));
        QCOMPARE(a.status, 302);
        SEQ(a.headers.get("location"), "/target");
        auto b = client.fetch("POST", ts.url("/r307"), {}, "{\"x\":1}");
        QCOMPARE(b.status, 307);
        SEQ(b.headers.get("location"), "/target");
        auto u = client.fetch("GET", ts.url("/nothing"));
        QCOMPARE(u.status, 404);
        SEQ(u.body, "{\"error\":\"Not found\"}");
        QCOMPARE(hits.load(), 0);
    }

    // ---- streaming GET with Range against the F4 server --------------------------------------------------------------
    void streamingGetWithRange()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const std::string file = QDir::toNativeSeparators(dir.filePath("media.bin")).toStdString();
        const int64_t size = (int64_t(8) << 20) + 7;
        {
            std::string data(static_cast<size_t>(size), '\0');
            fillPattern(&data[0], 0, data.size());
            QFile f(QString::fromStdString(file));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QCOMPARE(f.write(data.data(), static_cast<qint64>(data.size())), size);
        }
        TestServer ts;
        ts.router.get("/f", [&](Ctx& c) {
            if (!sendFile(c, file)) c.error(500, "sendFile refused the test file");
        });
        ts.router.get("/chunked", [](Ctx& c) {
            c.res.begin(200, -1);
            for (int i = 0; i < 50; ++i) c.res.write(std::string(10000, static_cast<char>('a' + i % 26)));
            c.res.end();
        });
        ts.start();
        HttpClient client;
        {   // whole file in odd-sized reads
            ClientRequest q;
            q.url = ts.url("/f");
            auto r = client.send(q);
            QCOMPARE(r->status(), 200);
            QCOMPARE(r->contentLength(), size);
            SEQ(r->header("accept-ranges"), "bytes");
            QVERIFY(r->header("etag").rfind("W/\"", 0) == 0);
            std::vector<char> buf(12345);
            int64_t got = 0, bad = 0;
            for (;;) {
                size_t k = r->read(buf.data(), buf.size());
                if (!k) break;
                bad += static_cast<int64_t>(patMismatches(buf.data(), static_cast<uint64_t>(got), k));
                got += static_cast<int64_t>(k);
            }
            QCOMPARE(got, size);
            QCOMPARE(bad, int64_t(0));
        }
        {   // a single range
            ClientRequest q;
            q.url = ts.url("/f");
            q.headers.add("Range", "bytes=1000001-2000000");
            auto r = client.send(q);
            QCOMPARE(r->status(), 206);
            SEQ(r->header("content-range"), "bytes 1000001-2000000/" + std::to_string(size));
            QCOMPARE(r->contentLength(), int64_t(1000000));
            std::string body = r->readAll();
            QCOMPARE(int64_t(body.size()), int64_t(1000000));
            QCOMPARE(patMismatches(body.data(), 1000001, body.size()), size_t(0));
        }
        {   // suffix range, If-None-Match 304, HEAD
            Headers h;
            h.add("Range", "bytes=-10");
            auto a = client.fetch("GET", ts.url("/f"), h);
            QCOMPARE(a.status, 206);
            QCOMPARE(patMismatches(a.body.data(), static_cast<uint64_t>(size - 10), a.body.size()), size_t(0));
            Headers n;
            n.add("If-None-Match", a.headers.get("etag"));
            auto nm = client.fetch("GET", ts.url("/f"), n);
            QCOMPARE(nm.status, 304);
            QVERIFY(nm.body.empty());
            ClientRequest q;
            q.method = "HEAD";
            q.url = ts.url("/f");
            auto r = client.send(q);
            QCOMPARE(r->status(), 200);
            QCOMPARE(r->contentLength(), size);
            char c;
            QCOMPARE(r->read(&c, 1), size_t(0));
        }
        {   // chunked response: unknown length, decoded body
            ClientRequest q;
            q.url = ts.url("/chunked");
            auto r = client.send(q);
            QCOMPARE(r->contentLength(), int64_t(-1));
            std::string body = r->readAll();
            QCOMPARE(int(body.size()), 500000);
            QCOMPARE(body[0], 'a');
            QCOMPARE(body[499999], static_cast<char>('a' + 49 % 26));
        }
        QVERIFY(ts.server.requests() >= 6);
    }

    // ---- uploads ---------------------------------------------------------------------------------------------------
    void chunkedPost2GiBFlatMemory()
    {
        TestServer ts;
        struct Seen {
            std::atomic<int64_t> bytes{-1}, bad{-1};
            std::atomic<bool> chunked{false};
            std::atomic<int64_t> declared{-2};
        } seen;
        ts.router.post("/sink", [&](Ctx& c) {
            VerifySink sink;
            int64_t n = c.body.copyTo(sink);
            seen.bytes = n;
            seen.bad = sink.bad;
            seen.chunked = c.req.chunked;
            seen.declared = c.req.contentLength;
            c.json(200, JVal::object({{"n", n}}));
        });
        ts.start();
        HttpClient client;
        {   // 64 MiB with a Content-Length
            PatternSource src(int64_t(64) << 20, true);
            ClientRequest q;
            q.method = "POST";
            q.url = ts.url("/sink");
            q.body = &src;
            q.bodyLength = src.length();
            auto r = client.send(q);
            QCOMPARE(r->status(), 200);
            SEQ(r->readAll(), "{\"n\":67108864}");
            QCOMPARE(seen.bytes.load(), int64_t(64) << 20);
            QCOMPARE(seen.bad.load(), int64_t(0));
            QVERIFY(!seen.chunked.load());
            QCOMPARE(seen.declared.load(), int64_t(64) << 20);
        }
        const int64_t total = int64_t(2) << 30;
        PatternSource src(total);
        ClientRequest q;
        q.method = "POST";
        q.url = ts.url("/sink");
        q.body = &src;
        q.bodyLength = -1;
        QElapsedTimer tm;
        tm.start();
        MemPeak mem;
        auto r = client.send(q);
        std::string body = r->readAll();
        mem.stop();
        const int64_t ms = tm.elapsed();
        QCOMPARE(r->status(), 200);
        SEQ(body, "{\"n\":" + std::to_string(total) + "}");
        QCOMPARE(seen.bytes.load(), total);
        QCOMPARE(seen.bad.load(), int64_t(0));
        QVERIFY(seen.chunked.load());
        QCOMPARE(src.pos(), total);
        qInfo("2 GiB chunked POST in %lld ms (%.0f MB/s), private bytes peak +%lld KB", static_cast<long long>(ms),
              ms ? total / 1048.576 / ms : 0.0, static_cast<long long>(mem.grew() / 1024));
        QVERIFY2(mem.grew() < (int64_t(32) << 20), "memory grew with the upload");
    }

    // ---- inactivity timeouts ---------------------------------------------------------------------------------------
    void idleTimeouts()
    {
        Wsa wsa;
        RawServer srv([&](RawConn& c) {
            std::string r = c.readRequest();
            if (r.find("GET /silent ") == 0) {
                c.hold();
            } else if (r.find("GET /stall ") == 0) {
                c.send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789");
                c.hold();
            } else if (r.find("GET /trickle ") == 0) {
                c.send("HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\n");
                for (int i = 0; i < 15 && !c.stopping(); ++i) {
                    c.sleepMs(100);
                    c.send(std::string(1, static_cast<char>('a' + i)));
                }
                c.hold();
            }
        });
        HttpClient client;
        {
            ClientRequest q;
            q.url = srv.url("/silent");
            q.idleMs = 300;
            int64_t t0 = nowMs();
            auto e = clientError([&] { client.send(q); });
            int64_t ms = nowMs() - t0;
            QVERIFY(e.has_value());
            qInfo("no reply: %s after %lld ms", e->what(), static_cast<long long>(ms));
            QVERIFY(e->timeout);
            QVERIFY(!e->cancelled);
            QCOMPARE(e->winError, uint32_t(ERROR_WINHTTP_TIMEOUT));
            QVERIFY2(ms >= 250 && ms < 2600, "the 300 ms idle timeout did not fire in time");
            QCOMPARE(srv.accepts(), 1);                                  // no WinHTTP retries on fresh connections
        }
        {
            ClientRequest q;
            q.url = srv.url("/stall");
            q.idleMs = 300;
            auto r = client.send(q);
            char buf[64];
            size_t got = 0;
            int64_t t0 = 0;
            auto e = clientError([&] {
                for (;;) {
                    t0 = nowMs();
                    size_t k = r->read(buf, sizeof buf);
                    if (!k) break;
                    got += k;
                }
            });
            int64_t ms = nowMs() - t0;
            QVERIFY(e.has_value());
            qInfo("stalled body: %s after %lld ms", e->what(), static_cast<long long>(ms));
            QCOMPARE(got, size_t(10));
            QVERIFY(e->timeout);
            QVERIFY(ms >= 250 && ms < 2600);
            QVERIFY(clientError([&] { r->read(buf, sizeof buf); }).has_value());   // the request is gone
        }
        {   // 1.5 s in total, never 300 ms without a byte: inactivity, not a total timeout
            ClientRequest q;
            q.url = srv.url("/trickle");
            q.idleMs = 300;
            int64_t t0 = nowMs();
            auto r = client.send(q);
            SEQ(r->readAll(), "abcdefghijklmno");
            QVERIFY(nowMs() - t0 >= 1400);
        }
    }

    // ---- cancel from another thread ----------------------------------------------------------------------------------
    void cancelFromAnotherThread()
    {
        Wsa wsa;
        RawServer srv([&](RawConn& c) {
            std::string r = c.readRequest();
            if (r.find("GET /stall ") == 0) c.send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789");
            c.hold();
        });
        HttpClient client;
        auto cancelAfter = [](CancelToken& t, int ms) {
            return std::thread([&t, ms] {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                t.cancel();
            });
        };
        {   // waiting for the reply (idle timeout far away)
            CancelToken tok;
            ClientRequest q;
            q.url = srv.url("/silent");
            q.cancel = &tok;
            int64_t t0 = nowMs();
            std::thread th = cancelAfter(tok, 200);
            auto e = clientError([&] { client.send(q); });
            th.join();
            int64_t ms = nowMs() - t0;
            QVERIFY(e.has_value());
            QVERIFY(e->cancelled);
            QVERIFY(!e->timeout);
            QVERIFY2(ms >= 150 && ms < 2000, "cancel did not wake the request");
        }
        {   // mid-body
            CancelToken tok;
            ClientRequest q;
            q.url = srv.url("/stall");
            q.cancel = &tok;
            auto r = client.send(q);
            char buf[64];
            QCOMPARE(r->read(buf, sizeof buf), size_t(10));
            int64_t t0 = nowMs();
            std::thread th = cancelAfter(tok, 200);
            auto e = clientError([&] { r->read(buf, sizeof buf); });
            th.join();
            QVERIFY(e.has_value() && e->cancelled);
            QVERIFY(nowMs() - t0 < 2000);
            auto again = clientError([&] { r->read(buf, sizeof buf); });
            QVERIFY(again.has_value() && again->cancelled);
        }
        {   // a cancelled token stops a new request before it connects
            CancelToken tok;
            tok.cancel();
            QVERIFY(tok.cancelled());
            const int before = srv.accepts();
            ClientRequest q;
            q.url = srv.url("/silent");
            q.cancel = &tok;
            auto e = clientError([&] { client.send(q); });
            QVERIFY(e.has_value() && e->cancelled);
            QCOMPARE(srv.accepts(), before);
        }
        {   // one token, two requests on two threads
            CancelToken tok;
            std::atomic<int> cancelled{0};
            auto run = [&] {
                ClientRequest q;
                q.url = srv.url("/silent");
                q.cancel = &tok;
                auto e = clientError([&] { client.send(q); });
                if (e && e->cancelled) ++cancelled;
            };
            std::thread a(run), b(run);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            tok.cancel();
            a.join();
            b.join();
            QCOMPARE(cancelled.load(), 2);
        }
        // During an upload the server never reads: the write blocks on full socket buffers until the cancel.
        RawServer sink([&](RawConn& c) { c.hold(); }, 4096);
        {
            CancelToken tok;
            PatternSource src(int64_t(1) << 30);
            ClientRequest q;
            q.method = "PUT";
            q.url = sink.url("/up");
            q.body = &src;
            q.bodyLength = int64_t(1) << 30;
            q.cancel = &tok;
            int64_t t0 = nowMs();
            std::thread th = cancelAfter(tok, 400);
            auto e = clientError([&] { client.send(q); });
            th.join();
            QVERIFY(e.has_value() && e->cancelled);
            qInfo("upload cancelled after %lld bytes", static_cast<long long>(src.pos()));
            QVERIFY(src.pos() < (int64_t(1) << 30));
            QVERIFY(nowMs() - t0 < 3000);
        }
        // The client still works after all that (fetch reports the stalled body as an error, it does not throw).
        auto f = client.fetch("GET", srv.url("/stall"), {}, {}, 300);
        QCOMPARE(f.status, 0);
        QVERIFY(f.error.find("timed out") != std::string::npos);
    }

    // ---- https with a self-signed certificate --------------------------------------------------------------------------
    void httpsSelfSigned()
    {
        if (!QSslSocket::supportsSsl()) QSKIP("no Qt TLS backend");
        // openssl.exe from PATH (a conda/Qt "Libraryin" folder is on it when the tests run), else <PLAYPLAY_QT_ROOT>/bin.
        QString openssl = QStandardPaths::findExecutable("openssl");
        const QString qtRoot = qEnvironmentVariable("PLAYPLAY_QT_ROOT");
        if (openssl.isEmpty() && !qtRoot.isEmpty()) openssl = qtRoot + "/bin/openssl.exe";
        if (openssl.isEmpty() || !QFile::exists(openssl)) QSKIP("no openssl.exe to make a test certificate");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        const QString cnf = QFileInfo(openssl).absoluteDir().absoluteFilePath("../ssl/openssl.cnf");
        if (QFile::exists(cnf)) env.insert("OPENSSL_CONF", cnf);
        p.setProcessEnvironment(env);
        p.start(openssl, {"req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", dir.filePath("k.pem"), "-out",
                          dir.filePath("c.pem"), "-days", "2", "-subj", "/CN=anfs-selfsigned-test"});
        QVERIFY(p.waitForFinished(60000));
        if (p.exitCode() != 0) QSKIP(qPrintable("openssl failed: " + QString::fromLocal8Bit(p.readAllStandardError())));
        QFile cf(dir.filePath("c.pem")), kf(dir.filePath("k.pem"));
        QVERIFY(cf.open(QIODevice::ReadOnly) && kf.open(QIODevice::ReadOnly));
        TlsServer tls;
        QString why;
        if (!tls.start(cf.readAll(), kf.readAll(), &why)) QSKIP(qPrintable("TLS server: " + why));
        qInfo("QSslServer backend %s on port %d", qPrintable(QSslSocket::activeBackend()), tls.port());
        HttpClient client;
        for (const char* host : {"127.0.0.1", "localhost"}) {   // unknown CA and a CN that matches neither name
            const std::string url = std::string("https://") + host + ":" + std::to_string(tls.port()) + "/hello";
            auto r = client.fetch("GET", url);
            if (r.status == 0) qWarning("%s", r.error.c_str());
            QCOMPARE(r.status, 200);
            SEQ(r.body, "tls ok");
        }
        QVERIFY(waitFor([&] { return tls.heads().size() >= 2; }, 3000));
        SEQ(tls.heads()[0], "GET /hello HTTP/1.1\r\nConnection: Keep-Alive\r\nHost: 127.0.0.1:" + std::to_string(tls.port()) + "\r\n\r\n");
        tls.stop();
    }

    // ---- fetch and errors ------------------------------------------------------------------------------------------
    void fetchAndErrors()
    {
        Wsa wsa;
        TestServer ts;
        ts.router.post("/json", [](Ctx& c) { c.json(200, JVal::object({{"got", c.readJson()}})); });
        ts.router.get("/big", [](Ctx& c) { c.res.sendText(200, std::string(5000, 'b')); });
        ts.start();
        HttpClient client;
        Headers h;
        h.add("Content-Type", "application/json");
        auto j = client.fetch("POST", ts.url("/json"), h, "{\"a\":[1,2]}");
        QCOMPARE(j.status, 200);
        SEQ(j.body, "{\"got\":{\"a\":[1,2]}}");
        SEQ(j.headers.get("content-type"), "application/json; charset=utf-8");
        QVERIFY(j.error.empty());

        auto big = client.fetch("GET", ts.url("/big"), {}, {}, 30000, 1000);   // past maxBody
        QCOMPARE(big.status, 0);
        QVERIFY(big.body.empty());
        QVERIFY(big.error.find("larger than 1000") != std::string::npos);
        {
            ClientRequest q;
            q.url = ts.url("/big");
            auto r = client.send(q);
            bool tooLarge = false;
            try {
                r->readAll(100);
            } catch (const TooLarge&) {
                tooLarge = true;
            }
            QVERIFY(tooLarge);
            auto e = clientError([&] { r->read(nullptr, 1); });
            QVERIFY(e.has_value());
            QVERIFY(e->cancelled);                                          // closed by the caller
        }
        {   // close() mid-body, then read
            ClientRequest q;
            q.url = ts.url("/big");
            auto r = client.send(q);
            r->close();
            char c;
            QVERIFY(clientError([&] { r->read(&c, 1); }).has_value());
        }

        // A refused connection, as fetch and as send.
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
        bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
        int len = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        const int dead = ntohs(a.sin_port);
        closesocket(s);
        const std::string deadUrl = "http://127.0.0.1:" + std::to_string(dead) + "/x?auth=secret-value";
        auto refused = client.fetch("GET", deadUrl);
        QCOMPARE(refused.status, 0);
        QVERIFY(!refused.error.empty());
        QVERIFY2(refused.error.find("secret-value") == std::string::npos, "the query reached an error text");
        qInfo("refused: %s", refused.error.c_str());
        {
            ClientRequest q;
            q.url = deadUrl;
            auto e = clientError([&] { client.send(q); });
            QVERIFY(e.has_value());
            QCOMPARE(e->winError, uint32_t(ERROR_WINHTTP_CANNOT_CONNECT));
            QVERIFY(!e->timeout && !e->cancelled);
        }

        // A body shorter than its Content-Length is an error, never a silent EOF.
        RawServer cut([&](RawConn& c) {
            c.readRequest();
            c.send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789");
            c.close();
        });
        {
            ClientRequest q;
            q.url = cut.url("/c");
            auto r = client.send(q);
            QCOMPARE(r->contentLength(), int64_t(100));
            auto e = clientError([&] { r->readAll(); });
            QVERIFY(e.has_value());
            qInfo("truncated: %s", e->what());
        }
        // Close-delimited body (no length, no chunking) reads to EOF.
        RawServer closeDelim([&](RawConn& c) {
            c.readRequest();
            c.send("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nuntil-close");
            c.close();
        });
        {
            ClientRequest q;
            q.url = closeDelim.url("/c");
            auto r = client.send(q);
            QCOMPARE(r->contentLength(), int64_t(-1));
            SEQ(r->readAll(), "until-close");
        }

        // Bad URLs and requests: refused before any network activity.
        for (const char* u : {"ftp://127.0.0.1/x", "127.0.0.1/x", "http:///x", "http://h:0/", "http://h:65536/",
                              "http://h:12a/", "http://user:pw@h/", "http://[::1/"}) {
            ClientRequest q;
            q.url = u;
            auto e = clientError([&] { client.send(q); });
            QVERIFY2(e.has_value(), u);
            QVERIFY(e->winError == ERROR_WINHTTP_INVALID_URL || e->winError == ERROR_WINHTTP_UNRECOGNIZED_SCHEME);
        }
        auto badFetch = client.fetch("GET", "nope");
        QCOMPARE(badFetch.status, 0);
        QVERIFY(!badFetch.error.empty());
        {
            ClientRequest q;
            q.url = ts.url("/x");
            q.method = "GE T";
            QVERIFY(clientError([&] { client.send(q); }).has_value());
        }
    }
};

QTEST_GUILESS_MAIN(TestAnfsCoreClient)
#include "test_anfs_core_client.moc"
