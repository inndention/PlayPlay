// test_anfs_core_http - ANFS-F4a: the Winsock HTTP/1.1 server core (core/http_server.cpp, core/http_ctx.cpp) driven
// by a raw socket client on ephemeral loopback ports.
//
// - Exact reply bytes against what Node 22.13.1's http module writes (captured once with node; the Date value masked):
//   res.json-style replies, 204, an unknown status ("unknown"), HTTP/1.0 close-delimited bodies, Connection: close,
//   Node's bare protocol-error replies.
// - Keep-alive and pipelining (five requests in one send(), bodies and a chunked request in between), HTTP/1.0.
// - Chunked request bodies with extensions and trailers, fed whole and one byte per send(); framing errors -> 400 and
//   close; chunked responses (and close-delimited for HTTP/1.0).
// - Expect: 100-continue sent on the first body read only; never when the handler answers without reading; 417.
// - 431 past 64 KB of headers; 413 from readAll / readJson / copyTo (declared and chunked); the connection kept when
//   the rest of the body fits the 1 MB drain, closed otherwise; readJson's Express rules.
// - int64 Content-Length (5 GB declared), overflow -> 400; the strict parser cases (lower-case method, missing Host,
//   repeated Content-Length, CL + TE, TE not ending in chunked, obs-fold, space before ':', bare LF, HTTP/2.0 -> 505).
// - HEAD: GET handlers run, the GET Content-Length goes out, no body, the connection stays usable.
// - writeFile at an offset through the 64 KB copy path and through TransmitFile (forced on), plain and chunked.
// - A client abort mid-body: the handler sees IoError{peerGone}, the worker is released (maxThreads 1 serves the next
//   connection); a body read timeout ends the request.
// - 512 connections hold all 512 workers; the 513th gets 503 {"error":"Server busy."}; a freed worker is reused.
// - Header / keep-alive idle timeouts; handler errors (HttpError, exceptions, no reply, an error after the headers
//   went out = reset, CRLF in a header refused); stop() drains (503 for new requests, in-flight ones finish) and wakes
//   idle connections at once; restart; the PROXY v1 listener.
// - Headers / Query (qs rules) / urlDecode / encodeURIComponent / Query::build / reasonPhrase (live against node when
//   it is installed).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>

#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/json.h"
#include "core/nodepath.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace anfs;
using namespace anfs::http;

#define SEQ(a, b) QCOMPARE(QString::fromStdString(std::string(a)), QString::fromStdString(std::string(b)))

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

struct Resp {
    bool ok = false;
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string raw;                             // status line + headers (Date masked) + body as received
    std::string header(const std::string& n) const
    {
        for (const auto& h : headers)
            if (_stricmp(h.first.c_str(), n.c_str()) == 0) return h.second;
        return {};
    }
    bool has(const std::string& n) const
    {
        for (const auto& h : headers)
            if (_stricmp(h.first.c_str(), n.c_str()) == 0) return true;
        return false;
    }
};

// A blocking raw client with its own read buffer (pipelined replies arrive together).
class Raw {
public:
    Raw() = default;
    explicit Raw(int port, int timeoutMs = 5000) { connectTo(port, timeoutMs); }
    ~Raw() { close(); }
    Raw(const Raw&) = delete;
    Raw& operator=(const Raw&) = delete;

    bool connectTo(int port, int timeoutMs = 5000)
    {
        s_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
        if (::connect(s_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        setTimeout(timeoutMs);
        BOOL nd = TRUE;
        setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
        return true;
    }
    void setTimeout(int ms)
    {
        DWORD v = static_cast<DWORD>(ms);
        setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
    }
    bool send(const std::string& d)
    {
        size_t at = 0;
        while (at < d.size()) {
            int k = ::send(s_, d.data() + at, static_cast<int>(std::min<size_t>(d.size() - at, 1 << 20)), 0);
            if (k <= 0) return false;
            at += static_cast<size_t>(k);
        }
        return true;
    }
    void close()
    {
        if (s_ != INVALID_SOCKET) closesocket(s_);
        s_ = INVALID_SOCKET;
    }
    void reset()
    {
        linger l{1, 0};
        setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        close();
    }
    void shutdownSend() { shutdown(s_, SD_SEND); }

    // One more piece from the socket; false on EOF, error or timeout (eof_ tells which).
    bool fill()
    {
        char tmp[65536];
        int k = ::recv(s_, tmp, sizeof tmp, 0);
        if (k > 0) {
            buf_.append(tmp, static_cast<size_t>(k));
            return true;
        }
        eof_ = k == 0 || WSAGetLastError() != WSAETIMEDOUT;
        return false;
    }
    bool need(size_t n)
    {
        while (buf_.size() < n)
            if (!fill()) return false;
        return true;
    }
    std::string take(size_t n)
    {
        std::string r = buf_.substr(0, n);
        buf_.erase(0, n);
        return r;
    }
    // Exactly n bytes, or what arrived before EOF/timeout.
    std::string readN(size_t n)
    {
        need(n);
        return take(std::min(n, buf_.size()));
    }
    std::string readUntil(const std::string& delim)
    {
        size_t p;
        while ((p = buf_.find(delim)) == std::string::npos)
            if (!fill()) return take(buf_.size());
        return take(p + delim.size());
    }
    std::string readToClose()
    {
        while (fill()) {
        }
        return take(buf_.size());
    }
    // True when the server closed the connection (EOF or reset) within ms and sent nothing more.
    bool closedWithin(int ms)
    {
        setTimeout(ms);
        bool r = !fill() && eof_;
        setTimeout(5000);
        return r && buf_.empty();
    }
    std::string& buffer() { return buf_; }

    Resp read(bool headReq = false)
    {
        Resp r;
        std::string head = readUntil("\r\n\r\n");
        if (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) return r;
        std::string masked;
        size_t i = 0;
        bool firstLine = true;
        while (i + 2 <= head.size()) {
            size_t e = head.find("\r\n", i);
            std::string line = head.substr(i, e - i);
            i = e + 2;
            if (line.empty()) break;
            if (firstLine) {
                firstLine = false;
                r.status = std::atoi(line.substr(9, 3).c_str());
                r.reason = line.size() > 13 ? line.substr(13) : std::string();
                masked += line + "\r\n";
                continue;
            }
            size_t c = line.find(": ");
            std::string n = line.substr(0, c), v = line.substr(c + 2);
            r.headers.emplace_back(n, v);
            masked += (n == "Date" ? std::string("Date: D") : line) + "\r\n";
        }
        masked += "\r\n";
        if (headReq || r.status == 204 || r.status == 304 || r.status < 200) {
            // no body
        } else if (r.has("Content-Length")) {
            r.body = readN(static_cast<size_t>(std::atoll(r.header("Content-Length").c_str())));
        } else if (_stricmp(r.header("Transfer-Encoding").c_str(), "chunked") == 0) {
            for (;;) {
                std::string sz = readUntil("\r\n");
                size_t n = std::strtoull(sz.c_str(), nullptr, 16);
                if (n == 0) {
                    readUntil("\r\n");
                    break;
                }
                r.body += readN(n);
                readN(2);
            }
        } else {
            r.body = readToClose();
        }
        r.raw = masked + r.body;
        r.ok = true;
        return r;
    }

    SOCKET s_ = INVALID_SOCKET;

private:
    std::string buf_;
    bool eof_ = false;
};

std::shared_ptr<const Config> cfg0()
{
    static auto c = std::make_shared<const Config>();
    return c;
}

struct TestServer {
    explicit TestServer(ServerOptions o = {}) : server(router, [] { return cfg0(); }, nullptr, std::move(o)) {}
    void start() { server.start(0, false, "127.0.0.1"); }
    int port() const { return server.port(); }
    Router router;
    Server server;
};

std::string get(const std::string& path, const std::string& extra = {})
{
    return "GET " + path + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n";
}

std::string post(const std::string& path, const std::string& body, const std::string& extra = {})
{
    return "POST " + path + " HTTP/1.1\r\nHost: t\r\n" + extra + "Content-Length: " + std::to_string(body.size()) +
           "\r\n\r\n" + body;
}

// Routes most slots share.
void addCommon(Router& r)
{
    r.get("/j", [](Ctx& c) { c.json(200, JVal::object({{"ok", true}, {"n", 1}})); });
    r.post("/echo", [](Ctx& c) {
        std::string b = c.body.readAll(1 << 24);
        JVal t = JVal::object();
        for (const auto& f : c.body.trailers().fields()) t.set(f.first, f.second);
        c.json(200, JVal::object({{"body", b}, {"len", static_cast<int64_t>(b.size())}, {"trailers", t},
                                  {"chunked", c.req.chunked}}));
    });
    r.post("/json", [](Ctx& c) { c.json(200, c.readJson(100)); });
    r.get("/q", [](Ctx& c) {
        c.json(200, JVal::object({{"path", c.req.path}, {"x", c.queryOr("x")}, {"target", c.req.target}}));
    });
    r.get("/chunked", [](Ctx& c) {
        c.res.setHeader("X-A", "1");
        c.res.begin(200, -1);
        c.res.write("ab");
        c.res.write(std::string(20000, 'z'));
        c.res.write("cd");
        c.res.end();
    });
    r.get("/nc", [](Ctx& c) { c.res.sendEmpty(204); });
    r.get("/599", [](Ctx& c) { c.res.sendEmpty(599); });
}

const char* kJ = "{\"ok\":true,\"n\":1}";

std::string nodeExe()
{
    const char* p = "C:\\Program Files\\nodejs\\node.exe";
    return QFile::exists(QString::fromUtf8(p)) ? p : std::string();
}

// ---- ANFS-F4b helpers ----------------------------------------------------------------------------------------

// The Express packages the Node peer ships (express 4.21 / send 0.19 / range-parser 1.2.1 / fresh 0.5.2 /
// content-disposition 0.5.4 / finalhandler 1.3), a local copy on this workstation (nothing is downloaded).
QString nodeModules()
{
    const QString p = QStringLiteral("C:/libs/services/broadcast-playout/node_modules");
    return QFile::exists(p + "/express/package.json") && QFile::exists(p + "/content-disposition/package.json")
               ? p
               : QString();
}

// Runs `node script args...`; the script's stdout, or "" with a skip reason when node or the packages are missing.
bool runNode(const QString& script, const QStringList& args, QByteArray& out, QString& why)
{
    if (nodeExe().empty() || nodeModules().isEmpty()) {
        why = "node or the Express packages are not installed here";
        return false;
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QString::fromStdString(nodeExe()), QStringList{script} + args);
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        why = "node failed: " + QString::fromUtf8(p.readAllStandardError());
        return false;
    }
    out = p.readAllStandardOutput();
    return true;
}

std::string toHex(const std::string& s)
{
    static const char* k = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) {
        o += k[c >> 4];
        o += k[c & 15];
    }
    return o;
}

// Printable form for failure messages (header values carry latin1 bytes); long bodies are cut.
std::string esc(const std::string& s)
{
    std::string o;
    for (unsigned char c : s.substr(0, 1500)) {
        if (c == '\r') o += "\\r";
        else if (c == '\n') o += "\\n\n";
        else if (c < 0x20 || c >= 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o;
}

// A response as one comparable text: status line, headers without Date / X-Powered-By, body.
std::string normalized(const Resp& r)
{
    std::string s = std::to_string(r.status) + " " + r.reason + "\n";
    for (const auto& h : r.headers)
        if (_stricmp(h.first.c_str(), "Date") != 0 && _stricmp(h.first.c_str(), "X-Powered-By") != 0)
            s += h.first + ": " + h.second + "\n";
    return s + "\n" + r.body;
}

void writeBytes(const QString& path, const std::string& data)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(data.data(), static_cast<qint64>(data.size()));
}

// Sets the last-write time to an exact 100 ns tick count since 1970 (Node rounds the sub-millisecond part).
bool setMtimeTicks(const QString& path, int64_t ticksSince1970)
{
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const uint64_t t = static_cast<uint64_t>(ticksSince1970 + 116444736000000000LL);
    FILETIME ft{static_cast<DWORD>(t & 0xFFFFFFFFu), static_cast<DWORD>(t >> 32)};
    BOOL ok = SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    return ok != 0;
}

int64_t privateBytes()
{
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    return static_cast<int64_t>(pm.PrivateUsage);
}

} // namespace

class TestAnfsCoreHttp : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void headersQueryAndCodes();
    void exactRepliesKeepAlive();
    void pipelining();
    void chunkedRequestsWithTrailers();
    void expectContinue();
    void limits431And413();
    void int64ContentLengthAndStrictParser();
    void headSuppression();
    void writeFileCopyAndTransmitFile();
    void clientAbortReleasesThread();
    void busyCap503();
    void timeouts();
    void handlerErrors();
    void stopDrainsAndRestarts();
    void proxyListener();
    // ANFS-F4b
    void routerScoringAndTies();
    void routerDispatchMiddlewareAndFlags();
    void routerCopyOnWrite();
    void rangeParserMatrix();
    void httpDatesAndEtag();
    void contentDispositionRule();
    void sendFileMatrix();
    void sendFileAgainstExpress();
    void sendFileThreeGiBFlatMemory();

private:
    Wsa wsa_;
};

void TestAnfsCoreHttp::initTestCase()
{
    _putenv_s("ANFS_HTTP_TRANSMITFILE", "");
}

void TestAnfsCoreHttp::headersQueryAndCodes()
{
    Headers h;
    h.add("Content-Type", "a");
    h.add("X-Dup", "1");
    h.add("x-dup", "2");
    h.add("Accept", "b");
    SEQ(h.get("content-type"), "a");
    SEQ(h.joined("X-DUP"), "1, 2");
    QCOMPARE(h.getAll("x-dup").size(), size_t(2));
    h.set("X-DUP", "3");                         // replaces both, keeps the first position, takes the new spelling
    QCOMPARE(h.size(), size_t(3));
    SEQ(h.fields()[1].first, "X-DUP");
    SEQ(h.fields()[1].second, "3");
    QVERIFY(h.remove("accept"));
    QVERIFY(!h.has("Accept"));
    SEQ(h.get("missing", "def"), "def");

    // qs rules: '+' is a space; a bad escape or non-UTF-8 keeps the whole (plus-replaced) text; first wins for get();
    // empty parts and empty keys dropped; a part without '=' has value "".
    Query q("a=1&b=x+y&a=2&&c&=z&d=%41%42&e=%zz%41&f=%FF&g=%E2%82%AC&h=1=2&i[]=3");
    SEQ(*q.get("a"), "1");
    QCOMPARE(q.all("a").size(), size_t(2));
    SEQ(*q.get("b"), "x y");
    SEQ(*q.get("c"), "");
    QVERIFY(!q.get(""));
    SEQ(*q.get("d"), "AB");
    SEQ(*q.get("e"), "%zz%41");
    SEQ(*q.get("f"), "%FF");
    SEQ(*q.get("g"), "\xE2\x82\xAC");
    SEQ(*q.get("h"), "1=2");
    SEQ(*q.get("i[]"), "3");
    QVERIFY(!q.has("A"));                         // keys are case-sensitive
    std::string many;
    for (int i = 0; i < 1100; ++i) many += "k" + std::to_string(i) + "=1&";
    Query qm(many);
    QCOMPARE(qm.items().size(), size_t(1000));   // qs parameterLimit

    SEQ(urlDecode("a%20b+c%zz%4", false), "a b+c%zz%4");
    SEQ(urlDecode("a+b", true), "a b");
    SEQ(encodeURIComponent("a b/c?d=é!'()*~_.-"), "a%20b%2Fc%3Fd%3D%C3%A9!'()*~_.-");
    SEQ(Query::build({{"a b", "c&d"}, {"e", "f*~"}}), "a+b=c%26d&e=f*%7E");

    SEQ(reasonPhrase(200), "OK");
    SEQ(reasonPhrase(418), "I'm a Teapot");
    SEQ(reasonPhrase(599), "unknown");
    const std::string node = nodeExe();
    if (node.empty()) {
        qInfo("node not installed: STATUS_CODES not compared live");
        return;
    }
    QProcess p;
    p.start(QString::fromStdString(node), {"-e", "const c=require('http').STATUS_CODES;for(const k in c)"
                                                 "process.stdout.write(k+'\\t'+c[k]+'\\n')"});
    QVERIFY(p.waitForFinished(20000));
    const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    QVERIFY(lines.size() > 60);
    int n = 0;
    for (int code = 100; code < 600; ++code) {
        QString want = "unknown";
        for (const QString& l : lines)
            if (l.section('\t', 0, 0).toInt() == code) want = l.section('\t', 1);
        QCOMPARE(QString::fromUtf8(reasonPhrase(code)), want);
        ++n;
    }
    QCOMPARE(n, 500);
}

void TestAnfsCoreHttp::exactRepliesKeepAlive()
{
    TestServer ts;
    addCommon(ts.router);
    ts.start();
    Raw c(ts.port());
    // The bytes Node 22 writes for the same replies (Express res.json order: Content-Type, Content-Length).
    c.send(get("/j"));
    Resp r = c.read();
    SEQ(r.raw, std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: 17\r\n"
                           "Date: D\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n\r\n") + kJ);
    QVERIFY(QRegularExpression("^[A-Z][a-z]{2}, \\d\\d [A-Z][a-z]{2} \\d{4} \\d\\d:\\d\\d:\\d\\d GMT$")
                .match(QString::fromStdString(r.header("Date"))).hasMatch());
    c.send(get("/nc"));
    SEQ(c.read().raw, "HTTP/1.1 204 No Content\r\nDate: D\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n\r\n");
    c.send(get("/599"));
    SEQ(c.read().raw, "HTTP/1.1 599 unknown\r\nContent-Length: 0\r\nDate: D\r\nConnection: keep-alive\r\n"
                      "Keep-Alive: timeout=5\r\n\r\n");
    c.send(get("/chunked"));
    r = c.read();
    SEQ(r.header("Transfer-Encoding"), "chunked");
    QCOMPARE(r.body, "ab" + std::string(20000, 'z') + "cd");
    QVERIFY(r.raw.rfind("HTTP/1.1 200 OK\r\nX-A: 1\r\nDate: D\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n", 0) == 0);
    // Connection: close from the client
    c.send(get("/j", "Connection: close\r\n"));
    SEQ(c.read().raw, std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
                                  "Content-Length: 17\r\nDate: D\r\nConnection: close\r\n\r\n") + kJ);
    QVERIFY(c.closedWithin(2000));

    // HTTP/1.0: close by default, a chunked-style handler becomes close-delimited (Node: no Transfer-Encoding)
    Raw c10(ts.port());
    c10.send("GET /chunked HTTP/1.0\r\n\r\n");
    std::string all = c10.readToClose();
    QVERIFY(all.rfind("HTTP/1.1 200 OK\r\nX-A: 1\r\nDate: ", 0) == 0);
    QVERIFY(all.find("\r\nConnection: close\r\n\r\nab") != std::string::npos);
    QVERIFY(all.find("Transfer-Encoding") == std::string::npos);
    QVERIFY(all.size() > 20004 && all.compare(all.size() - 4, 4, "zzcd") == 0);
    // HTTP/1.0 keep-alive on request
    Raw k10(ts.port());
    k10.send("GET /j HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
    r = k10.read();
    SEQ(r.header("Connection"), "keep-alive");
    k10.send("GET /j HTTP/1.0\r\n\r\n");
    r = k10.read();
    SEQ(r.header("Connection"), "close");
    QVERIFY(k10.closedWithin(2000));
    // fallback
    Raw f(ts.port());
    f.send(get("/nothing/here"));
    r = f.read();
    QCOMPARE(r.status, 404);
    SEQ(r.body, "{\"error\":\"Not found\"}");
    QCOMPARE(ts.server.requests(), int64_t(9));
}

void TestAnfsCoreHttp::pipelining()
{
    TestServer ts;
    addCommon(ts.router);
    ts.start();
    Raw c(ts.port());
    std::string chunked = "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    c.send(get("/q?x=1") + post("/echo", "hello") + get("/q?x=2") + chunked + "\r\n" + get("/q?x=3"));
    Resp a = c.read(), b = c.read(), d = c.read(), e = c.read(), g = c.read();
    SEQ(a.body, "{\"path\":\"/q\",\"x\":\"1\",\"target\":\"/q?x=1\"}");
    SEQ(b.body, "{\"body\":\"hello\",\"len\":5,\"trailers\":{},\"chunked\":false}");
    SEQ(d.body, "{\"path\":\"/q\",\"x\":\"2\",\"target\":\"/q?x=2\"}");
    SEQ(e.body, "{\"body\":\"abc\",\"len\":3,\"trailers\":{},\"chunked\":true}");
    SEQ(g.body, "{\"path\":\"/q\",\"x\":\"3\",\"target\":\"/q?x=3\"}");   // the extra CRLF before it is skipped
    QCOMPARE(ts.server.requests(), int64_t(5));
    QCOMPARE(ts.server.threads(), 1);
    // A handler that leaves a small body unread: drained, the next pipelined request still parses.
    ts.router.post("/ignore", [](Ctx& x) { x.json(200, JVal::object({{"ignored", true}})); });
    c.send(post("/ignore", std::string(5000, 'q')) + get("/q?x=4"));
    SEQ(c.read().body, "{\"ignored\":true}");
    SEQ(c.read().body, "{\"path\":\"/q\",\"x\":\"4\",\"target\":\"/q?x=4\"}");
    // absolute-form target
    c.send("GET http://t:80/q?x=5 HTTP/1.1\r\nHost: t\r\n\r\n");
    SEQ(c.read().body, "{\"path\":\"/q\",\"x\":\"5\",\"target\":\"http://t:80/q?x=5\"}");
}

void TestAnfsCoreHttp::chunkedRequestsWithTrailers()
{
    TestServer ts;
    addCommon(ts.router);
    ts.start();
    const std::string req = "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, Chunked\r\n\r\n"
                            "4;name=val\r\nWiki\r\n5 ; x\r\npedia\r\nE\r\n in\r\n\r\nchunks.\r\n0\r\n"
                            "X-Trail: yes\r\nX-Sum:  12 \r\n\r\n";
    const std::string want = "{\"body\":\"Wikipedia in\\r\\n\\r\\nchunks.\",\"len\":23,"
                             "\"trailers\":{\"X-Trail\":\"yes\",\"X-Sum\":\"12\"},\"chunked\":true}";
    {
        Raw c(ts.port());
        c.send(req + get("/q?x=after"));
        SEQ(c.read().body, want);
        SEQ(c.read().body, "{\"path\":\"/q\",\"x\":\"after\",\"target\":\"/q?x=after\"}");
    }
    {
        Raw c(ts.port());                        // one byte per send: every framing state split everywhere
        std::string all = req + get("/q?x=b");
        for (char ch : all) {
            c.send(std::string(1, ch));
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        SEQ(c.read().body, want);
        SEQ(c.read().body, "{\"path\":\"/q\",\"x\":\"b\",\"target\":\"/q?x=b\"}");
    }
    // Large chunks straight from the socket.
    {
        Raw c(ts.port());
        std::string big(300000, 'x');
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
        std::string body = "493E0\r\n" + big + "\r\n0\r\n\r\n";
        c.send("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n" + body);
        Resp r = c.read();
        QCOMPARE(r.status, 200);
        QVERIFY(r.body.find("\"len\":300000") != std::string::npos);
    }
    // Framing errors: 400 {"error":"invalid chunked body"} and the connection closes.
    for (const char* bad : {"zz\r\nab\r\n0\r\n\r\n", "3\r\nabcX\r\n0\r\n\r\n", "3\nabc\r\n0\r\n\r\n",
                            "3\r\nabc\r\n0\r\nbad trailer\r\n\r\n", "FFFFFFFFFFFFFFFFF\r\n"}) {
        Raw c(ts.port());
        c.send(std::string("POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n") + bad);
        Resp r = c.read();
        QCOMPARE(r.status, 400);
        SEQ(r.body, "{\"error\":\"invalid chunked body\"}");
        SEQ(r.header("Connection"), "close");
        QVERIFY(c.closedWithin(3000));
    }
}

void TestAnfsCoreHttp::expectContinue()
{
    TestServer ts;
    addCommon(ts.router);
    ts.start();
    {
        Raw c(ts.port());
        c.send("POST /echo HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
        SEQ(c.readN(25), "HTTP/1.1 100 Continue\r\n\r\n");
        c.send("hello");
        Resp r = c.read();
        SEQ(r.body, "{\"body\":\"hello\",\"len\":5,\"trailers\":{},\"chunked\":false}");
        SEQ(r.header("Connection"), "keep-alive");
        c.send(get("/j"));                       // still usable
        SEQ(c.read().body, kJ);
    }
    {
        // A handler that answers without reading: no 100, the reply, then the connection closes (the client may
        // still be waiting to send its body).
        Raw c(ts.port());
        ts.router.post("/j2", [](Ctx& x) { x.json(401, JVal::object({{"error", "no"}})); });
        c.send("POST /j2 HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n");
        Resp r = c.read();
        QCOMPARE(r.status, 401);
        QVERIFY(c.closedWithin(3000));
        QVERIFY(c.buffer().find("100 Continue") == std::string::npos);
    }
    {
        // A handler that sends a reply and then reads: no 100 (the reply already started).
        ts.router.post("/late", [](Ctx& x) {
            x.res.begin(200, -1);
            x.res.write("x");
            x.res.end();
            (void)x.body.finished();
        });
        Raw c(ts.port());
        c.send("POST /late HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n");
        Resp r = c.read();
        QCOMPARE(r.status, 200);
        QVERIFY(r.raw.find("100 Continue") == std::string::npos);
    }
    {
        Raw c(ts.port());
        c.send(get("/j", "Expect: something\r\n"));
        SEQ(c.readToClose(), "HTTP/1.1 417 Expectation Failed\r\nConnection: close\r\n\r\n");
    }
}

void TestAnfsCoreHttp::limits431And413()
{
    TestServer ts;
    addCommon(ts.router);
    ts.router.post("/copy", [](Ctx& c) {
        NullSink n;
        int64_t got = c.body.copyTo(n, 1000);
        c.json(200, JVal::object({{"got", got}}));
    });
    ts.start();
    {
        Raw c(ts.port());
        c.send(get("/j", "X-Big: " + std::string(70 * 1024, 'a') + "\r\n"));
        SEQ(c.readToClose(), "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n");
    }
    {
        Raw c(ts.port());                        // 60 KB of headers are fine
        c.send(get("/j", "X-Big: " + std::string(60 * 1024, 'a') + "\r\n"));
        SEQ(c.read().body, kJ);
    }
    {
        Raw c(ts.port());                        // never ends its header block: 431 once past 64 KB
        c.send("GET /j HTTP/1.1\r\nHost: t\r\n");
        for (int i = 0; i < 70; ++i) c.send("X-" + std::to_string(i) + ": " + std::string(1000, 'b') + "\r\n");
        SEQ(c.readToClose(), "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n");
    }
    const std::string tooLarge = "{\"error\":\"request entity too large\"}";
    {
        Raw c(ts.port());                        // declared past the limit: 413 before reading; the 200 bytes drain
        c.send(post("/json", std::string(200, ' '), "Content-Type: application/json\r\n"));
        Resp r = c.read();
        QCOMPARE(r.status, 413);
        SEQ(r.body, tooLarge);
        SEQ(r.header("Connection"), "keep-alive");
        c.send(get("/j"));
        SEQ(c.read().body, kJ);
    }
    {
        Raw c(ts.port());                        // 5 GB declared: 413 with Connection: close, never drained
        c.send("POST /json HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: 5000000000\r\n\r\n");
        c.send(std::string(100000, ' '));
        Resp r = c.read();
        QCOMPARE(r.status, 413);
        SEQ(r.header("Connection"), "close");
        QVERIFY(c.closedWithin(4000));
    }
    {
        Raw c(ts.port());                        // chunked past the limit
        c.send("POST /copy HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n3E8\r\n" + std::string(1000, 'a') +
               "\r\n1\r\nb\r\n0\r\n\r\n" + get("/j"));
        Resp r = c.read();
        QCOMPARE(r.status, 413);
        SEQ(r.body, tooLarge);
        SEQ(c.read().body, kJ);                 // the rest drained, keep-alive kept
    }
    {
        Raw c(ts.port());
        c.send(post("/copy", std::string(999, 'a')));
        SEQ(c.read().body, "{\"got\":999}");
    }
    // readJson: Express json() rules
    Raw c(ts.port());
    c.send(post("/json", "{\"a\":1,\"b\":[2]}", "Content-Type: Application/JSON; charset=utf-8\r\n"));
    SEQ(c.read().body, "{\"a\":1,\"b\":[2]}");
    c.send(post("/json", "{\"a\":1}", "Content-Type: text/plain\r\n"));
    SEQ(c.read().body, "{}");
    c.send(post("/json", "", "Content-Type: application/json\r\n"));
    SEQ(c.read().body, "{}");
    c.send(post("/json", "{bad", "Content-Type: application/json\r\n"));
    Resp r = c.read();
    QCOMPARE(r.status, 400);
    SEQ(r.body, "{\"error\":\"Invalid JSON body.\"}");
    c.send(post("/json", "[1,2]", "Content-Type: application/json\r\n"));
    SEQ(c.read().body, "[1,2]");
}

void TestAnfsCoreHttp::int64ContentLengthAndStrictParser()
{
    TestServer ts;
    addCommon(ts.router);
    ts.router.post("/len", [](Ctx& c) {
        c.json(200, JVal::object({{"cl", c.req.contentLength}, {"length", c.body.length()}}));
    });
    ts.start();
    {
        Raw c(ts.port());
        c.send("POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: 5000000000\r\n\r\nabc");
        Resp r = c.read();
        SEQ(r.body, "{\"cl\":5000000000,\"length\":5000000000}");
        QVERIFY(c.closedWithin(4000));          // 5 GB unread: never drained
    }
    {
        Raw c(ts.port());
        c.send("POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: 9223372036854775807\r\n\r\n");
        SEQ(c.read().body, "{\"cl\":9223372036854775807,\"length\":9223372036854775807}");
    }
    const std::string bad400 = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
    const std::vector<std::string> bad = {
        "POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: 99999999999999999999\r\n\r\n",
        "POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\na",
        "POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: -1\r\n\r\n",
        "POST /len HTTP/1.1\r\nHost: t\r\nContent-Length: 1, 1\r\n\r\na",
        "POST /len HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n0\r\n\r\n",
        "POST /len HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n",
        "get /j HTTP/1.1\r\nHost: t\r\n\r\n",
        "GET /j HTTP/1.1\r\n\r\n",
        "GET /j HTTP/1.1\r\nHost: t\r\nX-A: 1\r\n folded\r\n\r\n",
        "GET /j HTTP/1.1\r\nHost : t\r\n\r\n",
        "GET /j HTTP/1.1\r\nHost: t\nX: 1\r\n\r\n",
        "GET /j  HTTP/1.1\r\nHost: t\r\n\r\n",
        "GET /j HTTP/1.1 \r\nHost: t\r\n\r\n",
        "GET /j HTP/1.1\r\nHost: t\r\n\r\n",
        "GET /j HTTP/1.1\r\nHost: t\r\nX-Ctl: a\x01" "b\r\n\r\n",
    };
    for (const std::string& b : bad) {
        Raw c(ts.port());
        c.send(b);
        QString msg = QString::fromStdString(b.substr(0, b.find("\r\n\r\n")));
        QVERIFY2(c.readToClose() == bad400, qPrintable(msg));
    }
    Raw v(ts.port());
    v.send("GET /j HTTP/2.0\r\nHost: t\r\n\r\n");
    SEQ(v.readToClose(), "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n");
}

void TestAnfsCoreHttp::headSuppression()
{
    TestServer ts;
    addCommon(ts.router);
    ts.start();
    Raw c(ts.port());
    c.send("HEAD /j HTTP/1.1\r\nHost: t\r\n\r\n");
    Resp r = c.read(true);
    SEQ(r.raw, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: 17\r\n"
               "Date: D\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n\r\n");
    // Node: a HEAD reply of a handler without a length has neither Content-Length nor Transfer-Encoding.
    c.send("HEAD /chunked HTTP/1.1\r\nHost: t\r\n\r\n");
    r = c.read(true);
    SEQ(r.raw, "HTTP/1.1 200 OK\r\nX-A: 1\r\nDate: D\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5\r\n\r\n");
    c.send(get("/j"));                           // no stray body bytes came before this reply
    r = c.read();
    QCOMPARE(r.status, 200);
    SEQ(r.body, kJ);
    QVERIFY(c.buffer().empty());
    c.send("HEAD /nowhere HTTP/1.1\r\nHost: t\r\n\r\n");
    r = c.read(true);
    QCOMPARE(r.status, 404);
    SEQ(r.header("Content-Length"), "21");
    c.send(get("/j"));
    SEQ(c.read().body, kJ);
}

void TestAnfsCoreHttp::writeFileCopyAndTransmitFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("f.bin");
    std::string data(3 * 1024 * 1024 + 77, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 131 + i / 7) & 0xFF);
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data.data(), static_cast<qint64>(data.size()));
    }
    for (const char* mode : {"0", "1"}) {
        _putenv_s("ANFS_HTTP_TRANSMITFILE", mode);
        TestServer ts;
        ts.router.get("/file", [&](Ctx& c) {
            HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) throw HttpError(500, "open");
            int64_t off = std::atoll(c.queryOr("off", "0").c_str());
            int64_t n = static_cast<int64_t>(data.size()) - off;
            c.res.setHeader("Content-Type", "application/octet-stream");
            try {
                c.res.begin(200, c.queryOr("chunked") == "1" ? -1 : n);
                c.res.writeFile(h, off, n);
                c.res.end();
            } catch (...) {
                CloseHandle(h);
                throw;
            }
            CloseHandle(h);
        });
        ts.start();
        Raw c(ts.port());
        for (const char* q : {"/file?off=5", "/file?off=0&chunked=1", "/file?off=1048577"}) {
            c.send(get(q));
            Resp r = c.read();
            QCOMPARE(r.status, 200);
            int64_t off = std::atoll(QString::fromUtf8(q).section("off=", 1).section('&', 0, 0).toUtf8().constData());
            QVERIFY2(r.body == data.substr(static_cast<size_t>(off)), q);
        }
        c.send("HEAD /file?off=5 HTTP/1.1\r\nHost: t\r\n\r\n");
        Resp h = c.read(true);
        SEQ(h.header("Content-Length"), std::to_string(data.size() - 5));
        c.send(get("/file?off=7"));
        QCOMPARE(c.read().body.size(), data.size() - 7);
        qInfo("writeFile mode %s ok", mode);
    }
    _putenv_s("ANFS_HTTP_TRANSMITFILE", "");
}

void TestAnfsCoreHttp::clientAbortReleasesThread()
{
    ServerOptions o;
    o.maxThreads = 1;
    TestServer ts(o);
    addCommon(ts.router);
    std::atomic<int> outcome{0};                 // 1 = peerGone, 2 = timeout, 3 = other
    std::atomic<int64_t> got{0};
    ts.router.post("/upload", [&](Ctx& c) {
        NullSink n;
        try {
            c.body.copyTo(n, -1, [&](int64_t t) { got = t; });
        } catch (const IoError& e) {
            outcome = e.peerGone ? 1 : e.timeout ? 2 : 3;
            throw;
        }
        c.json(200, JVal::object({{"done", true}}));
    });
    ts.start();
    for (int round = 0; round < 2; ++round) {
        outcome = 0;
        got = 0;
        {
            Raw c(ts.port());
            c.send("POST /upload HTTP/1.1\r\nHost: t\r\nContent-Length: 10000000\r\n\r\n" + std::string(100000, 'u'));
            QVERIFY(waitFor([&] { return got.load() == 100000; }, 5000));
            if (round == 0) c.close();          // FIN
            else c.reset();                     // RST
        }
        QVERIFY(waitFor([&] { return outcome.load() != 0; }, 5000));
        QCOMPARE(outcome.load(), 1);
        QVERIFY(waitFor([&] { return ts.server.inFlight() == 0; }, 3000));
        // The single worker is free again: the next connection is served, not refused.
        Raw n(ts.port());
        n.send(get("/j"));
        Resp r = n.read();
        QCOMPARE(r.status, 200);
        QCOMPARE(ts.server.rejectedBusy(), int64_t(0));
        QCOMPARE(ts.server.threads(), 1);
    }
}

void TestAnfsCoreHttp::busyCap503()
{
    TestServer ts;                               // default options: 512 workers
    addCommon(ts.router);
    ts.start();
    QCOMPARE(ts.server.options().maxThreads, 512);
    std::vector<std::unique_ptr<Raw>> held;
    for (int i = 0; i < 512; ++i) {
        held.push_back(std::make_unique<Raw>());
        QVERIFY(held.back()->connectTo(ts.port()));
    }
    QVERIFY2(waitFor([&] { return ts.server.threads() == 512; }, 20000),
             qPrintable(QString::number(ts.server.threads())));
    {
        Raw extra(ts.port());
        extra.send(get("/j"));
        Resp r = extra.read();
        QCOMPARE(r.status, 503);
        SEQ(r.body, "{\"error\":\"Server busy.\"}");
        SEQ(r.header("Connection"), "close");
        SEQ(r.header("Content-Type"), "application/json; charset=utf-8");
        QVERIFY(extra.closedWithin(3000));
    }
    QCOMPARE(ts.server.rejectedBusy(), int64_t(1));
    // Every held connection is served by its own worker.
    held[0]->send(get("/j"));
    SEQ(held[0]->read().body, kJ);
    held[511]->send(get("/j"));
    SEQ(held[511]->read().body, kJ);
    // One connection leaves; its worker goes idle and takes the next connection.
    held[100]->close();
    QVERIFY(waitFor([&] {
        Raw n(ts.port());
        n.send(get("/j"));
        return n.read().status == 200;
    }, 5000));
    QCOMPARE(ts.server.threads(), 512);
    held.clear();
    QVERIFY(waitFor([&] { return ts.server.inFlight() == 0; }, 3000));
    Raw n(ts.port());
    n.send(get("/j"));
    QCOMPARE(n.read().status, 200);
}

void TestAnfsCoreHttp::timeouts()
{
    ServerOptions o;
    o.headerTimeoutMs = 300;
    o.keepAliveIdleMs = 300;
    o.bodyTimeoutMs = 400;
    TestServer ts(o);
    addCommon(ts.router);
    std::atomic<int> outcome{0};
    ts.router.post("/slow", [&](Ctx& c) {
        try {
            c.body.readAll(1 << 20);
        } catch (const IoError& e) {
            outcome = e.timeout ? 2 : e.peerGone ? 1 : 3;
            throw;
        }
        c.json(200, JVal::object());
    });
    ts.start();
    {
        Raw c(ts.port());                        // says nothing: closed after the header timeout
        QElapsedTimer t;
        t.start();
        QVERIFY(c.closedWithin(3000));
        QVERIFY(t.elapsed() >= 250);
    }
    {
        Raw c(ts.port());                        // half a request line, then silence
        c.send("GET /j HT");
        QVERIFY(c.closedWithin(3000));
    }
    {
        Raw c(ts.port());
        c.send(get("/j"));
        SEQ(c.read().header("Keep-Alive"), "timeout=0");   // 300 ms rounds down like Node's Math.floor
        QElapsedTimer t;
        t.start();
        QVERIFY(c.closedWithin(3000));          // idle keep-alive
        QVERIFY(t.elapsed() < 2000);
    }
    {
        Raw c(ts.port());
        c.send("POST /slow HTTP/1.1\r\nHost: t\r\nContent-Length: 100\r\n\r\nabc");
        QVERIFY(waitFor([&] { return outcome.load() != 0; }, 5000));
        QCOMPARE(outcome.load(), 2);
        QVERIFY(waitFor([&] { return ts.server.inFlight() == 0; }, 3000));
    }
}

void TestAnfsCoreHttp::handlerErrors()
{
    TestServer ts;
    addCommon(ts.router);
    ts.router.get("/403", [](Ctx&) { throw HttpError(403, "nope"); });
    ts.router.get("/boom", [](Ctx& c) {
        c.res.setHeader("Content-Disposition", "attachment");   // must not leak into the error reply
        throw std::runtime_error("secret detail");
    });
    ts.router.get("/silent", [](Ctx&) {});
    ts.router.get("/half", [](Ctx& c) {
        c.res.begin(200, 100);
        c.res.write("partial");
        throw std::runtime_error("after headers");
    });
    ts.router.get("/crlf", [](Ctx& c) {
        c.res.setHeader("X-Evil", "a\r\nSet-Cookie: x=1");
        c.json(200, JVal::object());
    });
    ts.router.get("/short", [](Ctx& c) {
        c.res.begin(200, 10);
        c.res.write("12345");
        c.res.end();
    });
    ts.router.get("/redir", [](Ctx& c) { c.res.redirect("/j"); });
    ts.router.get("/p/{id}/{*rest}", [](Ctx& c) {
        c.json(200, JVal::object({{"id", c.param("ID")}, {"rest", c.param("rest")}}));
    });
    ts.start();
    Raw c(ts.port());
    c.send(get("/403"));
    Resp r = c.read();
    QCOMPARE(r.status, 403);
    SEQ(r.body, "{\"error\":\"nope\"}");
    c.send(get("/boom"));
    r = c.read();
    QCOMPARE(r.status, 500);
    SEQ(r.body, "{\"error\":\"internal error\"}");
    QVERIFY(!r.has("Content-Disposition"));
    c.send(get("/silent"));
    r = c.read();
    QCOMPARE(r.status, 500);
    c.send(get("/crlf"));
    r = c.read();
    QCOMPARE(r.status, 500);
    QVERIFY(!r.has("Set-Cookie"));
    c.send(get("/redir"));
    r = c.read();
    QCOMPARE(r.status, 302);
    SEQ(r.header("Location"), "/j");
    SEQ(r.body, "Found. Redirecting to /j");
    c.send(get("/p/a%20b/x/y%2Fz"));
    SEQ(c.read().body, "{\"id\":\"a b\",\"rest\":\"x/y/z\"}");
    c.send(get("/half"));                         // reset after the headers went out
    std::string got = c.readToClose();         // "partial" may be lost to the reset; no error reply follows
    QVERIFY(got.find("internal error") == std::string::npos);
    Raw s(ts.port());
    s.send(get("/short"));                       // shorter than its Content-Length: the connection closes
    std::string sh = s.readToClose();
    QVERIFY(sh.size() >= 5 && sh.compare(sh.size() - 5, 5, "12345") == 0);
}

void TestAnfsCoreHttp::stopDrainsAndRestarts()
{
    TestServer ts;
    addCommon(ts.router);
    std::atomic<bool> slowStarted{false};
    ts.router.get("/slow", [&](Ctx& c) {
        slowStarted = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        c.json(200, JVal::object({{"slow", true}}));
    });
    ts.start();
    const int port = ts.port();
    QCOMPARE(ts.server.listening().size(), size_t(1));
    SEQ(ts.server.listening()[0], "127.0.0.1:" + std::to_string(port));
    Raw idle(port);                              // an idle keep-alive connection
    idle.send(get("/j"));
    SEQ(idle.read().body, kJ);
    Raw slow(port);
    slow.send(get("/slow"));
    QVERIFY(waitFor([&] { return slowStarted.load(); }, 3000));
    Raw ka(port);                                // a kept-alive connection that asks again during the drain
    ka.send(get("/j"));
    SEQ(ka.read().body, kJ);
    QElapsedTimer t;
    t.start();
    std::thread stopper([&] { ts.server.stop(); });
    struct Joiner {                              // a failed QCOMPARE returns early: never leave the thread joinable
        std::thread& t;
        ~Joiner() { if (t.joinable()) t.join(); }
    } joiner{stopper};
    QVERIFY(waitFor([&] { return ts.server.inFlight() == 1; }, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ka.send(get("/j"));
    Resp busy = ka.read();
    QCOMPARE(busy.status, 503);
    SEQ(busy.body, "{\"error\":\"Server busy.\"}");
    Raw late(port);
    late.send(get("/j"));
    QCOMPARE(late.read().status, 503);
    Resp sr = slow.read();
    QCOMPARE(sr.status, 200);                    // the in-flight request finished
    SEQ(sr.header("Connection"), "close");
    stopper.join();
    QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));   // the idle socket did not hold it up
    QVERIFY(!ts.server.running());
    QCOMPARE(ts.server.threads(), 0);
    QVERIFY(idle.closedWithin(2000));
    // Restartable; a taken port gives BindError 10048.
    ts.start();
    Raw again(ts.port());
    again.send(get("/j"));
    SEQ(again.read().body, kJ);
    TestServer other;
    bool threw = false;
    try {
        other.server.start(ts.port(), false, "127.0.0.1");
    } catch (const BindError& e) {
        threw = true;
        QCOMPARE(e.wsaError, 10048);
    }
    QVERIFY(threw);
    ts.server.stop();
}

void TestAnfsCoreHttp::proxyListener()
{
    TestServer ts;
    addCommon(ts.router);
    ts.router.get("/who", [](Ctx& c) {
        c.json(200, JVal::object({{"peer", c.req.peerIp}, {"port", c.req.peerPort}, {"secure", c.req.secure},
                                  {"client", c.req.clientIp}}));
    });
    ts.start();
    int pp = ts.server.startProxyListener(0);
    QCOMPARE(ts.server.listening().size(), size_t(2));
    SEQ(ts.server.listening()[1], "127.0.0.1:" + std::to_string(pp) + " (proxy)");
    {
        Raw c(pp);
        c.send("PROXY TCP4 203.0.113.7 127.0.0.1 5555 443\r\n" + get("/who") + get("/who"));
        SEQ(c.read().body, "{\"peer\":\"203.0.113.7\",\"port\":5555,\"secure\":true,\"client\":\"203.0.113.7\"}");
        SEQ(c.read().body, "{\"peer\":\"203.0.113.7\",\"port\":5555,\"secure\":true,\"client\":\"203.0.113.7\"}");
    }
    for (const char* bad : {"GET /who HTTP/1.1\r\n", "PROXY TCP4 nonsense\r\n", "PROXY TCP6 ::1 ::1 1 2\r\n"}) {
        Raw c(pp);
        c.send(std::string(bad) + "Host: t\r\n\r\n");
        QVERIFY2(c.readToClose().empty(), bad);
    }
    Raw plain(ts.port());                        // the main listener knows nothing of PROXY lines
    plain.send(get("/who"));
    Resp r = plain.read();
    QVERIFY(r.body.find("\"secure\":false") != std::string::npos);
    QVERIFY(r.body.find("\"peer\":\"127.0.0.1\"") != std::string::npos);
}

// ---- ANFS-F4b: router ---------------------------------------------------------------------------------------------

void TestAnfsCoreHttp::routerScoringAndTies()
{
    QCOMPARE(splitPath("//a///b/").size(), size_t(2));
    QCOMPARE(splitPath("/").size(), size_t(0));
    Params p;
    int sc = -1;
    QVERIFY(matchPattern("/a/{b}/*", "/a/x/y/z", &p, &sc));
    QCOMPARE(sc, 6);                             // literal 3 + param 2 + rest 1
    QCOMPARE(p.size(), size_t(1));
    SEQ(p[0].first, "b");
    SEQ(p[0].second, "x");
    QVERIFY(!matchPattern("/a/*/c", "/a/b/c", nullptr, nullptr));   // a rest segment must be last
    QVERIFY(!matchPattern("/a", "/a/b", nullptr, nullptr));
    QVERIFY(!matchPattern("/a/{b}", "/a", nullptr, nullptr));
    QVERIFY(matchPattern("/files/{*rest}", "/files", &p, &sc));     // zero rest segments
    QCOMPARE(sc, 4);
    SEQ(p[0].second, "");
    QVERIFY(matchPattern("/files/{*rest}", "/files/a/b%2Fc/%C3%A9", &p, &sc));
    SEQ(p[0].second, "a/b/c/\xC3\xA9");
    QVERIFY(matchPattern("/{x}/{X}", "/1/2", &p, &sc));            // C# dictionary: first slot, last value
    QCOMPARE(p.size(), size_t(1));
    SEQ(p[0].first, "x");
    SEQ(p[0].second, "2");
    QVERIFY(matchPattern("/API/List", "/api/%4cist", nullptr, &sc)); // percent-decoded, case-insensitive
    QCOMPARE(sc, 6);
    QVERIFY(matchPattern("/", "/", nullptr, &sc));
    QCOMPARE(sc, 0);
    QVERIFY(matchPattern("/{p}", "/a%20b+c%zz", &p, nullptr));        // '+' stays, a bad escape stays literal
    SEQ(p[0].second, "a b+c%zz");
    QVERIFY(!matchPattern("/api", "/ap%C4%B1", nullptr, nullptr));     // U+0131 does not fold to 'i' (ASCII only)
    QVERIFY(matchPattern("/a/{b}", "//a//x//", nullptr, nullptr));   // empty segments dropped

    Router r;
    Handler nop = [](Ctx&) {};
    r.get("/api/{id}", nop, kRouteNone, "p1");
    r.get("/api/{name}", nop, kRouteNone, "p2");      // same score as p1: the first registered wins
    r.get("/api/list", nop, kRouteNone, "lit");
    r.all("/api/list", nop, kRouteNone, "all");
    r.get("/api/*", nop, kRouteNone, "star");
    r.get("/{*rest}", nop, kRouteNone, "rest");
    r.map("head", "/api/{id}", nop, kRouteNone, "head");   // method upper-cased
    r.all("/x/{a}", nop, kRouteNone, "x-all");
    r.get("/x/{b}", nop, kRouteNone, "x-get");
    auto who = [&](std::string_view m, std::string_view path) {
        RouteMatch x = r.resolve(m, path);
        return x.route ? x.route->owner : std::string("-");
    };
    SEQ(who("GET", "/api/list"), "lit");          // 7 vs all 6, p1 6 (GET +1), star 5, rest 2
    SEQ(who("POST", "/api/list"), "all");
    SEQ(who("HEAD", "/api/list"), "lit");         // lit and all both 6 for HEAD: lit was registered first
    SEQ(who("GET", "/api/7"), "p1");
    QCOMPARE(r.resolve("GET", "/api/7").params.size(), size_t(1));
    SEQ(r.resolve("GET", "/api/7").params[0].first, "id");
    SEQ(r.resolve("GET", "/api/a%20b").params[0].second, "a b");
    SEQ(who("HEAD", "/api/7"), "head");           // an exact HEAD route (+1) beats GET's HEAD fallback
    SEQ(who("GET", "/api/a/b"), "star");
    SEQ(who("GET", "/zzz/y"), "rest");
    SEQ(r.resolve("GET", "/zzz/y").params[0].second, "zzz/y");
    SEQ(who("GET", "/"), "rest");
    SEQ(who("GET", "/API//List"), "lit");
    SEQ(who("HEAD", "/x/1"), "x-all");            // "*" and GET-for-HEAD tie at 5: first registered
    SEQ(who("GET", "/x/1"), "x-get");             // exact method +1
    SEQ(who("PUT", "/x/1"), "x-all");
    RouteMatch none = r.resolve("POST", "/api/7");
    QVERIFY(!none.route);
    QVERIFY(none.pathMatched);
    QCOMPARE(r.routes().size(), size_t(9));
    QCOMPARE(r.routes()[0]->seq < r.routes()[1]->seq, true);
    SEQ(r.routes()[6]->method, "HEAD");

    // Fallback-flagged routes only when no regular route matches; among themselves the usual rules.
    Router f;
    f.get("/f/{x}", nop, kRouteFallback, "fb1");
    f.get("/f/one", nop, kRouteFallback, "fb2");
    auto fwho = [&](std::string_view m, std::string_view path) {
        RouteMatch x = f.resolve(m, path);
        return x.route ? x.route->owner : std::string("-");
    };
    SEQ(fwho("GET", "/f/1"), "fb1");
    SEQ(fwho("GET", "/f/one"), "fb2");
    f.get("/{*all}", nop, kRouteNone, "regular");
    SEQ(fwho("GET", "/f/one"), "regular");        // a regular match wins whatever its score
    SEQ(fwho("POST", "/f/one"), "-");
    QVERIFY(f.resolve("POST", "/f/one").pathMatched);
    QVERIFY(!Router().resolve("GET", "/x").pathMatched);
}

void TestAnfsCoreHttp::routerDispatchMiddlewareAndFlags()
{
    TestServer ts;
    Router& r = ts.router;
    auto mark = [](const char* t) {
        return [t](Ctx& c, const Next& next) {
            c.items.set("o", c.items.getStr("o") + t);
            next();
        };
    };
    r.use(mark("A"), "m1");
    r.use(mark("B"), "m2");
    r.useFirst(mark("F"), "m1");
    // a gate-like middleware sees the route and its flags before the handler, and may end the request
    r.use([](Ctx& c, const Next& next) {
        if (c.header("X-Block") == "1" && !(c.route && c.route->open())) {
            c.error(403, "blocked");
            return;
        }
        if (c.route) c.res.setHeader("X-Flags", std::to_string(c.route->flags));
        next();
    }, "gate");
    r.get("/h/{id}", [](Ctx& c) {
        c.json(200, JVal::object({{"o", c.items.getStr("o")}, {"id", c.param("ID")}, {"route", c.route->pattern}}));
    }, kRouteAgentExempt, "mod");
    r.get("/pub/{*rest}", [](Ctx& c) { c.json(200, JVal::object({{"rest", c.param("rest")}})); }, kRouteOpen, "mod");
    ts.start();
    Raw c(ts.port());
    c.send(get("/h/42"));
    Resp a = c.read();
    QCOMPARE(a.status, 200);
    SEQ(a.body, "{\"o\":\"FAB\",\"id\":\"42\",\"route\":\"/h/{id}\"}");
    SEQ(a.header("X-Flags"), "1");
    c.send("HEAD /h/42 HTTP/1.1\r\nHost: t\r\n\r\n");                  // HEAD runs the GET handler
    Resp hd = c.read(true);
    QCOMPARE(hd.status, 200);
    SEQ(hd.header("Content-Length"), std::to_string(a.body.size()));
    c.send(get("/h/42", "X-Block: 1\r\n"));
    Resp b = c.read();
    QCOMPARE(b.status, 403);
    SEQ(b.body, "{\"error\":\"blocked\"}");
    c.send(get("/pub/a/b%20c", "X-Block: 1\r\n"));
    Resp o = c.read();
    QCOMPARE(o.status, 200);
    SEQ(o.body, "{\"rest\":\"a/b c\"}");
    SEQ(o.header("X-Flags"), "2");
    c.send(get("/nothing"));                     // no route: middleware still runs, then the default 404
    Resp n = c.read();
    QCOMPARE(n.status, 404);
    SEQ(n.body, "{\"error\":\"Not found\"}");
    QVERIFY(!n.has("X-Flags"));
    c.send(post("/h/42", "x"));                  // no 405: the method falls through to the fallback
    QCOMPARE(c.read().status, 404);
    r.setFallback([](Ctx& c2) { c2.json(404, JVal::object({{"fallback", c2.req.method}})); });
    c.send(post("/h/42", "x"));
    SEQ(c.read().body, "{\"fallback\":\"POST\"}");
    r.setFallback(nullptr);
    c.send(get("/nothing"));
    SEQ(c.read().body, "{\"error\":\"Not found\"}");

    QCOMPARE(r.middlewareCount(), size_t(4));
    QCOMPARE(r.removeOwner("m1"), size_t(2));   // F and A
    QCOMPARE(r.removeOwner("mod"), size_t(2));  // both routes
    QCOMPARE(r.removeOwner("nobody"), size_t(0));
    QCOMPARE(r.middlewareCount(), size_t(2));
    r.get("/h/{id}", [](Ctx& c2) { c2.json(200, JVal(c2.items.getStr("o"))); });
    c.send(get("/h/1"));
    SEQ(c.read().body, "\"B\"");
    r.clear();
    QCOMPARE(r.middlewareCount(), size_t(0));
    QCOMPARE(r.routes().size(), size_t(0));
    c.send(get("/h/1"));
    QCOMPARE(c.read().status, 404);
}

void TestAnfsCoreHttp::routerCopyOnWrite()
{
    TestServer ts;
    Router& r = ts.router;
    // A handler that changes the table: its own request keeps the table it started with.
    r.get("/grow", [&r](Ctx& c) {
        r.use([](Ctx& c2, const Next& next) {
            c2.res.setHeader("X-Late", "1");
            next();
        }, "late");
        r.get("/new", [](Ctx& c2) { c2.json(200, JVal("new")); }, kRouteNone, "late");
        c.json(200, JVal(static_cast<int64_t>(r.routes().size())));
    });
    ts.start();
    Raw c(ts.port());
    c.send(get("/grow"));
    Resp g = c.read();
    SEQ(g.body, "2");
    QVERIFY(!g.has("X-Late"));
    c.send(get("/new"));
    Resp n = c.read();
    SEQ(n.body, "\"new\"");
    SEQ(n.header("X-Late"), "1");
    QCOMPARE(r.removeOwner("late"), size_t(2));

    // Readers never see a torn table while one thread maps and removes.
    Router big;
    Handler nop = [](Ctx&) {};
    big.get("/stable/{x}", nop, kRouteNone, "stable");
    std::atomic<bool> stop{false};
    std::atomic<int64_t> resolved{0}, wrong{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&] {
            while (!stop.load()) {
                RouteMatch m = big.resolve("GET", "/stable/1");
                if (!m.route || m.route->owner != "stable" || m.params.size() != 1) ++wrong;
                RouteMatch v = big.resolve("GET", "/v/1");
                if (v.route && v.route->owner != "v") ++wrong;
                ++resolved;
            }
        });
    for (int i = 0; i < 2000; ++i) {
        big.get("/v/{n}", nop, kRouteNone, "v");
        big.use([](Ctx&, const Next& next) { next(); }, "v");
        if (i % 3 == 0) big.removeOwner("v");
    }
    stop = true;
    for (auto& t : readers) t.join();
    QCOMPARE(wrong.load(), int64_t(0));
    QVERIFY(resolved.load() > 0);
    QVERIFY(big.removeOwner("v") > 0);
    QCOMPARE(big.routes().size(), size_t(1));
}

// ---- ANFS-F4b: SendFile pieces ------------------------------------------------------------------------------------

void TestAnfsCoreHttp::rangeParserMatrix()
{
    struct V { int64_t len; const char* h; RangeResult r; int64_t s, e; };
    const V vs[] = {
        {10000, "bytes=0-0", RangeResult::Partial, 0, 0},
        {10000, "bytes=0-", RangeResult::Partial, 0, 9999},
        {10000, "bytes=-500", RangeResult::Partial, 9500, 9999},
        {10000, "bytes=500-", RangeResult::Partial, 500, 9999},
        {10000, "bytes=9000-20000", RangeResult::Partial, 9000, 9999},   // end clamped
        {10000, "bytes=-20000", RangeResult::Unsatisfiable, 0, 0},       // start < 0
        {10000, "bytes=10000-", RangeResult::Unsatisfiable, 0, 0},       // past the end
        {10000, "bytes=5-1", RangeResult::Unsatisfiable, 0, 0},          // range-parser: dropped, none left
        {10000, "bytes=5-1,7-8", RangeResult::Partial, 7, 8},
        {10000, "bytes=0-1,3-4", RangeResult::Full, 0, 0},               // two ranges: whole file
        {10000, "bytes=0-4,5-9", RangeResult::Partial, 0, 9},            // adjacent: combined
        {10000, "bytes=5-20,0-9", RangeResult::Partial, 0, 20},          // overlapping: combined
        {10000, "bytes=-0", RangeResult::Unsatisfiable, 0, 0},
        {10000, "bytes=5", RangeResult::Partial, 5, 9999},               // no '-': end is NaN = to the end
        {10000, "bytes=-", RangeResult::Unsatisfiable, 0, 0},
        {10000, "bytes=abc", RangeResult::Unsatisfiable, 0, 0},
        {10000, "  bytes= 7 - 9", RangeResult::Partial, 7, 9},           // parseInt skips white space
        {10000, "bytes=+3-4", RangeResult::Partial, 3, 4},
        {10000, "bytes=0x10-20", RangeResult::Partial, 0, 20},           // parseInt stops at 'x'
        {10000, "items=0-5", RangeResult::Full, 0, 0},
        {10000, "Bytes=0-5", RangeResult::Full, 0, 0},
        {10000, "", RangeResult::Full, 0, 0},
        {0, "bytes=0-", RangeResult::Unsatisfiable, 0, 0},
        {0, "bytes=-5", RangeResult::Unsatisfiable, 0, 0},
        {int64_t(5) << 30, "bytes=4294967296-", RangeResult::Partial, int64_t(1) << 32, (int64_t(5) << 30) - 1},
        {10000, "bytes=0-99999999999999999999999", RangeResult::Partial, 0, 9999},
    };
    for (const V& v : vs) {
        int64_t s = -7, e = -7;
        RangeResult r = parseRange(v.h, v.len, s, e);
        QVERIFY2(r == v.r, v.h);
        if (r == RangeResult::Partial) {
            QVERIFY2(s == v.s && e == v.e, v.h);
        }
    }

    // Live: the same rule as send's use of range-parser, over the vectors above and 4,000 random headers.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    JVal cases = JVal::array();
    for (const V& v : vs) cases.push(JVal::array({JVal(v.len), JVal(v.h)}));
    uint32_t seed = 12345;
    auto rnd = [&seed](uint32_t n) {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 8) % n;
    };
    const char* pre[] = {"bytes=", " bytes=", "bytes=", "bytes=", "items=", "bytes ="};
    const char* atoms[] = {"0", "1", "5", "9", "10", "99", "100", "999", "1000", "1001", "-", "-", "-", ",", ",",
                           " ", "+", "x", "", "18446744073709551616"};
    const int64_t lens[] = {0, 1, 10, 100, 1000};
    for (int i = 0; i < 4000; ++i) {
        std::string h = pre[rnd(6)];
        int n = 1 + static_cast<int>(rnd(8));
        for (int k = 0; k < n; ++k) h += atoms[rnd(20)];
        cases.push(JVal::array({JVal(lens[rnd(5)]), JVal(h)}));
    }
    writeBytes(dir.filePath("cases.json"), json::stringify(cases));
    writeBytes(dir.filePath("rp.js"),
               "const NM = process.argv[2];\n"
               "const rp = require(NM + '/range-parser');\n"
               "const cases = JSON.parse(require('fs').readFileSync(process.argv[3], 'utf8'));\n"
               "const out = cases.map(([size, h]) => {\n"
               "  if (!/^ *bytes=/.test(h)) return 'F';\n"
               "  const r = rp(size, h, { combine: true });\n"
               "  if (r === -1) return 'U';\n"
               "  if (r === -2 || r.length !== 1) return 'F';\n"
               "  return 'P ' + r[0].start + ' ' + r[0].end;\n"
               "});\n"
               "process.stdout.write(JSON.stringify(out));\n");
    QByteArray out;
    QString why;
    if (!runNode(dir.filePath("rp.js"), {nodeModules(), dir.filePath("cases.json")}, out, why)) {
        qInfo("range-parser differential skipped: %s", qPrintable(why));
        return;
    }
    JVal node = json::parse(out.toStdString());
    QCOMPARE(node.arr().size(), cases.arr().size());
    int partial = 0, unsat = 0, full = 0;
    for (size_t i = 0; i < cases.arr().size(); ++i) {
        const std::string h = cases.arr()[i].arr()[1].asString();
        int64_t s = 0, e = 0;
        RangeResult r = parseRange(h, cases.arr()[i].arr()[0].asInt(), s, e);
        std::string mine = r == RangeResult::Full ? "F"
                           : r == RangeResult::Unsatisfiable
                               ? "U"
                               : "P " + std::to_string(s) + " " + std::to_string(e);
        (r == RangeResult::Full ? full : r == RangeResult::Partial ? partial : unsat)++;
        QVERIFY2(mine == node.arr()[i].asString(),
                 qPrintable(QString::fromStdString(esc(h) + " len " + std::to_string(cases.arr()[i].arr()[0].asInt()) +
                                                   ": mine " + mine + ", node " + node.arr()[i].asString())));
    }
    qInfo("range-parser: %d cases identical to node (%d partial, %d unsatisfiable, %d full)",
          static_cast<int>(cases.arr().size()), partial, unsat, full);
}

void TestAnfsCoreHttp::httpDatesAndEtag()
{
    SEQ(weakEtag(10000, 1793000000001), "W/\"2710-1a17720ca01\"");
    SEQ(weakEtag(0, 0), "W/\"0-0\"");
    SEQ(weakEtag(5, -1000), "W/\"5--3e8\"");     // JS toString(16) of a negative number
    SEQ(httpDate(1793000000001), "Mon, 26 Oct 2026 07:33:20 GMT");
    QCOMPARE(parseHttpDate("Mon, 26 Oct 2026 07:33:20 GMT"), int64_t(1793000000000));
    QCOMPARE(parseHttpDate("Monday, 26-Oct-26 07:33:20 GMT"), int64_t(1793000000000));
    QCOMPARE(parseHttpDate("Mon, 26 Oct 2026 09:33:20 +0200"), int64_t(1793000000000));
    QCOMPARE(parseHttpDate("garbage"), int64_t(-1));
    QCOMPARE(parseHttpDate(""), int64_t(-1));

    // Live: Date.parse on the forms clients send back (local-time forms use this machine's zone in both).
    const char* vs[] = {
        "Wed, 14 Oct 2026 13:17:58 GMT", "Wednesday, 14-Oct-26 13:17:58 GMT", "Sunday, 06-Nov-94 08:49:37 GMT",
        "Sun Nov  6 08:49:37 1994", "Wed Oct 14 13:17:58 2026", "wed, 14 oct 2026 13:17:58 gmt",
        "Wed, 14 Oct 2026 13:17:58 +0200", "Wed, 14 Oct 2026 13:17:58 GMT+0130", "Wed, 14 Oct 2026 13:17:58 -0500",
        "Wed, 14 Oct 2026 13:17:58 EST", "Wed, 14 Oct 2026 13:17:58 PDT", "Wed, 14 Oct 2026 13:17:58 UTC",
        "Wed, 14 Oct 2026 13:17:58 UT", "Wed, 14 Oct 2026 13:17:58 Z", "14 Oct 2026 13:17:58 GMT",
        "Oct 14 2026 13:17:58 GMT", "Wed, 14 Oct 2026 13:17:58 GMT (Coordinated)", "Wed, 14 Oct 2026 1:02 PM GMT",
        "Wed, 14 Oct 2026 12:00 AM GMT", "Wed, 14 Oct 2026 12:30 PM GMT", "Wed, 14 Oct 2026 GMT",
        "Wed, 31 Feb 2026 10:00:00 GMT", "Wed, 14 Oct 2026 13:17:58.123 GMT", "Thu, 01 Jan 1970 00:00:00 GMT",
        "Fri, 31 Dec 1999 23:59:59 GMT", "Wed, 14 Oct 26 13:17:58 GMT", "Wed, 14 Oct 2026 13:17:58",
        "Wed, 14 October 2026 13:17:58 GMT", "2026-10-14T13:17:58.770Z", "2026-10-14T13:17:58Z",
        "2026-10-14T13:17:58+02:00", "Wed, 14 Oct 2026 24:00:00 GMT", "", "nonsense",
        "Wed, 14 Foo 2026 13:17:58 GMT", "Wed, 14 Oct 2026 25:00:00 GMT", "Wed, 14 Oct 2026 13:60:00 GMT",
        "Wed, 14 Oct 2026 13:17:58 GMT extra", "Wed, 14 Oct 2026 13 PM GMT x", "Wed, 14 Oct 2026 13:17:58 PM GMT",
    };
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    JVal list = JVal::array();
    for (const char* v : vs) list.push(JVal(v));
    writeBytes(dir.filePath("dates.json"), json::stringify(list));
    writeBytes(dir.filePath("d.js"),
               "const v = JSON.parse(require('fs').readFileSync(process.argv[3], 'utf8'));\n"
               "process.stdout.write(JSON.stringify(v.map(s => { const t = Date.parse(s); "
               "return isNaN(t) ? null : t; })));\n");
    QByteArray out;
    QString why;
    if (!runNode(dir.filePath("d.js"), {nodeModules(), dir.filePath("dates.json")}, out, why)) {
        qInfo("Date.parse differential skipped: %s", qPrintable(why));
        return;
    }
    JVal node = json::parse(out.toStdString());
    QCOMPARE(node.arr().size(), list.arr().size());
    for (size_t i = 0; i < list.arr().size(); ++i) {
        const int64_t mine = parseHttpDate(vs[i]);
        const JVal& n = node.arr()[i];
        const std::string a = mine == -1 ? "NaN" : std::to_string(mine);
        const std::string b = n.isNull() ? "NaN" : std::to_string(static_cast<int64_t>(n.asDouble()));
        QVERIFY2(a == b, qPrintable(QString("%1: mine %2, node %3").arg(vs[i], a.c_str(), b.c_str())));
    }
    qInfo("Date.parse: %d vectors identical to node", static_cast<int>(list.arr().size()));
}

void TestAnfsCoreHttp::contentDispositionRule()
{
    SEQ(contentDisposition("a.txt"), "attachment; filename=\"a.txt\"");
    SEQ(contentDisposition("C:\\dir\\sub\\Report 1.pdf", "INLINE"), "inline; filename=\"Report 1.pdf\"");
    // printable ISO-8859-1 stays in filename= as latin1 bytes (Node writes header strings as latin1)
    SEQ(contentDisposition("caf\xC3\xA9.txt"), "attachment; filename=\"caf\xE9.txt\"");
    SEQ(contentDisposition("\xE2\x82\xAC" "uro.txt"), "attachment; filename=\"?uro.txt\"; filename*=UTF-8''%E2%82%ACuro.txt");
    SEQ(contentDisposition("100%41.txt"), "attachment; filename=\"100%41.txt\"; filename*=UTF-8''100%2541.txt");
    SEQ(contentDisposition("a*b(c)'d!~.txt"),
        "attachment; filename=\"a*b(c)'d!~.txt\"");
    SEQ(contentDisposition("q\"b.txt"), "attachment; filename=\"q\\\"b.txt\"");
    SEQ(contentDisposition("dir\\q\\b.txt"), "attachment; filename=\"b.txt\"");   // path.win32.basename
    SEQ(contentDisposition(""), "attachment; filename*=UTF-8''");
    SEQ(contentDisposition("\xF0\x9F\x98\x80.png"), "attachment; filename=\"??.png\"; filename*=UTF-8''%F0%9F%98%80.png");
    bool threw = false;
    try {
        contentDisposition("lone \xED\xA0\x80.txt");   // a lone surrogate: encodeURIComponent throws URIError
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    QVERIFY(threw);

    // Live against content-disposition 0.5.4 (latin1 bytes of the header string).
    const char* names[] = {"a.txt", "", "\xC3\x9Cn\xC3\xAF" "code.mp4", "\xE5\x90\x8D\xE5\x89\x8D file.bin",
                           "a%20b's (1).json", "quote\"back\\slash.txt", "C:\\dir\\sub\\file.txt", "dir/file.txt",
                           "\xE2\x82\xAC" "uro.txt", "\xC2\x80" "ctl", "emoji \xF0\x9F\x98\x80.png", "tab\there",
                           "lone \xED\xA0\x80 half", "a*b(c)'d!~.txt", "100%", "%41", "%4G", "trailing.", "x:stream",
                           "\xC2\xA0nbsp\xC3\xBF", "semi;colon=eq,comma.txt", "\x7F" "del", "C:\\", "dir\\"};
    const char* types[] = {"attachment", "inline", "ATTACHMENT", "bad type"};
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    JVal list = JVal::array();
    for (const char* n : names) list.push(JVal(n));
    writeBytes(dir.filePath("names.json"), json::stringify(list));
    writeBytes(dir.filePath("cd.js"),
               "const cd = require(process.argv[2] + '/content-disposition');\n"
               "const names = JSON.parse(require('fs').readFileSync(process.argv[3], 'utf8'));\n"
               "const out = [];\n"
               "for (const n of names) for (const t of ['attachment', 'inline', 'ATTACHMENT', 'bad type']) {\n"
               "  try { out.push(Buffer.from(cd(n, { type: t }), 'latin1').toString('hex')); }\n"
               "  catch (e) { out.push('THROW'); }\n"
               "}\n"
               "process.stdout.write(JSON.stringify(out));\n");
    QByteArray out;
    QString why;
    if (!runNode(dir.filePath("cd.js"), {nodeModules(), dir.filePath("names.json")}, out, why)) {
        qInfo("content-disposition differential skipped: %s", qPrintable(why));
        return;
    }
    JVal node = json::parse(out.toStdString());
    size_t k = 0;
    for (const char* n : names)
        for (const char* t : types) {
            std::string mine;
            try {
                mine = toHex(contentDisposition(n, t));
            } catch (const std::invalid_argument&) {
                mine = "THROW";
            }
            QVERIFY2(k < node.arr().size() && mine == node.arr()[k].asString(),
                     qPrintable(QString::fromStdString(esc(n) + " / " + t + ": mine " + mine + ", node " +
                                                       (k < node.arr().size() ? node.arr()[k].asString() : ""))));
            ++k;
        }
    qInfo("content-disposition: %d cases identical to node", static_cast<int>(k));
}

namespace {

void addSendFileRoutes(Router& r)
{
    r.get("/f", [](Ctx& c) {
        if (!sendFile(c, c.queryOr("p"))) c.error(404, "Not found");
    });
    r.get("/d", [](Ctx& c) {                     // res.download(p, path.basename(p))
        SendFileOptions o;
        o.downloadName = nodepath::basename(c.queryOr("p"));
        if (!sendFile(c, c.queryOr("p"), o)) c.error(404, "Not found");
    });
    r.get("/t", [](Ctx& c) {                     // preview-media: the handler sets the type first
        c.res.setHeader("Content-Type", "video/mp4");
        if (!sendFile(c, c.queryOr("p"))) c.error(404, "Not found");
    });
}

std::string fileQuery(const char* route, const QString& path)
{
    return std::string(route) + "?p=" + encodeURIComponent(QDir::toNativeSeparators(path).toStdString());
}

Resp ask(int port, const std::string& target, const std::string& extra, bool head = false)
{
    Raw c(port);
    c.send(std::string(head ? "HEAD " : "GET ") + target + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n");
    return c.read(head);
}

} // namespace

void TestAnfsCoreHttp::sendFileMatrix()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    std::string data(10000, '\0');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>('a' + (i * 7 + i / 13) % 26);
    const QString path = dir.filePath("m.txt");
    writeBytes(path, data);
    QVERIFY(setMtimeTicks(path, 1793000000000LL * 10000 + 5000));   // .5 ms: Node rounds up
    writeBytes(dir.filePath("empty.dat"), std::string());
    TestServer ts;
    addSendFileRoutes(ts.router);
    ts.start();
    const int port = ts.port();
    const std::string f = fileQuery("/f", path);
    const std::string etag = "W/\"2710-1a17720ca01\"", lm = "Mon, 26 Oct 2026 07:33:20 GMT";
    const std::string fileHeaders = "Accept-Ranges: bytes\nCache-Control: public, max-age=0\nLast-Modified: " + lm +
                                    "\nETag: " + etag + "\n";
    const std::string tail = "Connection: keep-alive\nKeep-Alive: timeout=5\n\n";

    Resp r = ask(port, f, "");
    SEQ(normalized(r), "200 OK\n" + fileHeaders + "Content-Type: text/plain; charset=UTF-8\nContent-Length: 10000\n" +
                           tail + data);
    auto partial = [&](const std::string& range, int64_t s, int64_t e) {
        Resp p = ask(port, f, "Range: " + range + "\r\n");
        SEQ(normalized(p), "206 Partial Content\n" + fileHeaders + "Content-Type: text/plain; charset=UTF-8\n" +
                               "Content-Range: bytes " + std::to_string(s) + "-" + std::to_string(e) +
                               "/10000\nContent-Length: " + std::to_string(e - s + 1) + "\n" + tail +
                               data.substr(static_cast<size_t>(s), static_cast<size_t>(e - s + 1)));
    };
    partial("bytes=0-0", 0, 0);
    partial("bytes=0-", 0, 9999);
    partial("bytes=-500", 9500, 9999);
    partial("bytes=500-", 500, 9999);
    partial("bytes=0-4,5-9", 0, 9);
    const std::string page416 = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                                "<title>Error</title>\n</head>\n<body>\n<pre>Range Not Satisfiable</pre>\n"
                                "</body>\n</html>\n";
    for (const char* bad : {"bytes=10000-", "bytes=5-1", "bytes=-0"}) {
        Resp u = ask(port, f, std::string("Range: ") + bad + "\r\n");
        SEQ(normalized(u), "416 Range Not Satisfiable\n" + fileHeaders + "Content-Type: text/html; charset=utf-8\n" +
                               "Content-Range: bytes */10000\nContent-Security-Policy: default-src 'none'\n"
                               "X-Content-Type-Options: nosniff\nContent-Length: " +
                               std::to_string(page416.size()) + "\n" + tail + page416);
    }
    Resp multi = ask(port, f, "Range: bytes=0-1,3-4\r\n");
    QCOMPARE(multi.status, 200);
    QVERIFY(multi.body == data);
    QVERIFY(!multi.has("Content-Range"));

    // Conditional requests (fresh before Range; If-Range only steers the Range).
    const std::string notModified = "304 Not Modified\n" + fileHeaders + tail;
    SEQ(normalized(ask(port, f, "If-None-Match: " + etag + "\r\n")), notModified);
    SEQ(normalized(ask(port, f, "If-None-Match: \"x\", " + etag + "\r\n")), notModified);
    SEQ(normalized(ask(port, f, "If-None-Match: " + etag.substr(2) + "\r\n")), notModified);   // W/ either side
    SEQ(normalized(ask(port, f, "If-None-Match: *\r\n")), notModified);
    SEQ(normalized(ask(port, f, "If-None-Match: " + etag + "\r\nRange: bytes=0-0\r\n")), notModified);
    SEQ(normalized(ask(port, f, "If-Modified-Since: " + lm + "\r\n")), notModified);
    QCOMPARE(ask(port, f, "If-None-Match: W/\"other\"\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-None-Match: " + etag + "\r\nCache-Control: max-age=0, no-cache\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Modified-Since: Mon, 26 Oct 2026 07:33:19 GMT\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Modified-Since: garbage\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Range: " + etag + "\r\nRange: bytes=0-0\r\n").status, 206);
    QCOMPARE(ask(port, f, "If-Range: W/\"other\"\r\nRange: bytes=0-0\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Range: " + lm + "\r\nRange: bytes=0-0\r\n").status, 206);
    QCOMPARE(ask(port, f, "If-Range: Mon, 26 Oct 2026 07:33:19 GMT\r\nRange: bytes=0-0\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Range: W/\"other\"\r\nRange: bytes=99999-\r\n").status, 200);   // stale beats 416
    QCOMPARE(ask(port, f, "If-Match: " + etag + "\r\n").status, 200);
    QCOMPARE(ask(port, f, "If-Match: *\r\n").status, 200);
    Resp pre = ask(port, f, "If-Match: \"other\"\r\n");
    QCOMPARE(pre.status, 412);
    QVERIFY(pre.body.find("<pre>Precondition Failed</pre>") != std::string::npos);
    QCOMPARE(ask(port, f, "If-Unmodified-Since: Mon, 26 Oct 2026 07:33:19 GMT\r\n").status, 412);
    QCOMPARE(ask(port, f, "If-Unmodified-Since: " + lm + "\r\n").status, 200);

    // HEAD, download name, a preset type, an empty file, missing paths.
    Resp h = ask(port, f, "Range: bytes=10-19\r\n", true);
    QCOMPARE(h.status, 206);
    SEQ(h.header("Content-Length"), "10");
    QVERIFY(h.body.empty());
    Resp d = ask(port, fileQuery("/d", path), "");
    QCOMPARE(d.status, 200);
    SEQ(d.headers[0].first, "Content-Disposition");
    SEQ(d.headers[0].second, "attachment; filename=\"m.txt\"");
    Resp t = ask(port, fileQuery("/t", path), "Range: bytes=1-2\r\n");
    QCOMPARE(t.status, 206);
    SEQ(t.headers[0].second, "video/mp4");
    const std::string e = fileQuery("/f", dir.filePath("empty.dat"));
    Resp e0 = ask(port, e, "");
    QCOMPARE(e0.status, 200);
    SEQ(e0.header("Content-Length"), "0");
    SEQ(e0.header("Content-Type"), "application/octet-stream");
    Resp e1 = ask(port, e, "Range: bytes=0-\r\n");
    QCOMPARE(e1.status, 416);
    SEQ(e1.header("Content-Range"), "bytes */0");
    for (const QString& p : {dir.filePath("missing.txt"), dir.path()}) {
        Resp m = ask(port, fileQuery("/f", p), "");
        QCOMPARE(m.status, 404);
        SEQ(m.body, "{\"error\":\"Not found\"}");
        QVERIFY(!m.has("ETag"));
    }
    // Keep-alive survives a 304 and a 416 on one connection.
    Raw c(port);
    c.send(get(f, "If-None-Match: " + etag + "\r\n") + get(f, "Range: bytes=10000-\r\n") + get(f, "Range: bytes=1-1\r\n"));
    QCOMPARE(c.read().status, 304);
    QCOMPARE(c.read().status, 416);
    Resp last = c.read();
    QCOMPARE(last.status, 206);
    SEQ(last.body, data.substr(1, 1));
}

void TestAnfsCoreHttp::sendFileAgainstExpress()
{
    if (nodeExe().empty() || nodeModules().isEmpty()) QSKIP("node or the Express packages are not installed here");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    struct F { QString name; size_t size; int64_t ticks; };
    const int64_t base = 1793000000000LL * 10000;
    const F files[] = {
        {"plain.txt", 10000, base + 5000},                                        // x.5 ms: round up
        {QString::fromUtf8("\xC3\x9Cn\xC3\xAF \xE5\x90\x8D\xE5\x89\x8D (1).bin"), 3000, base + 4999},
        {"a%20b's.json", 77, base + 1},
        {"empty.dat", 0, base + 7777},
        {"clip.mpg", 4096, base - 123456789},
    };
    for (const F& fl : files) {
        std::string data(fl.size, '\0');
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>('0' + (i * 11 + i / 5) % 64);
        writeBytes(dir.filePath(fl.name), data);
        QVERIFY(setMtimeTicks(dir.filePath(fl.name), fl.ticks));
    }
    writeBytes(dir.filePath("srv.js"),
               "const NM = process.argv[2];\n"
               "const express = require(NM + '/express');\n"
               "const path = require('path');\n"
               "const app = express();\n"
               "app.get('/d', (req, res) => res.download(req.query.p, path.basename(req.query.p)));\n"
               "app.get('/f', (req, res) => res.sendFile(req.query.p));\n"
               "app.get('/t', (req, res) => { res.setHeader('Content-Type', 'video/mp4'); res.sendFile(req.query.p); });\n"
               "const srv = app.listen(0, '127.0.0.1', () => console.log('PORT ' + srv.address().port));\n"
               "process.stdin.on('end', () => process.exit(0));\n"
               "process.stdin.resume();\n");
    QProcess node;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("NODE_ENV", "production");        // finalhandler's page without the stack trace
    node.setProcessEnvironment(env);
    node.start(QString::fromStdString(nodeExe()), {dir.filePath("srv.js"), nodeModules()});
    QVERIFY(node.waitForStarted(10000));
    struct Stop {
        QProcess& p;
        ~Stop()
        {
            p.closeWriteChannel();
            if (!p.waitForFinished(3000)) p.kill();
            p.waitForFinished(3000);
        }
    } stopNode{node};
    int nodePort = 0;
    QElapsedTimer tm;
    tm.start();
    while (nodePort == 0 && tm.elapsed() < 15000) {
        node.waitForReadyRead(200);
        const QByteArray line = node.readLine().trimmed();
        if (line.startsWith("PORT ")) nodePort = line.mid(5).toInt();
    }
    QVERIFY2(nodePort > 0, node.readAllStandardError().constData());

    TestServer ts;
    addSendFileRoutes(ts.router);
    ts.start();
    int compared = 0;
    for (const F& fl : files) {
        const QString p = dir.filePath(fl.name);
        // The file's own validators, taken from our first reply (they are compared with Node's too).
        const Resp first = ask(ts.port(), fileQuery("/f", p), "");
        const std::string etag = first.header("ETag"), lm = first.header("Last-Modified");
        QVERIFY(!etag.empty());
        const std::string older = httpDate(parseHttpDate(lm) - 1000);
        const std::vector<std::string> extras = {
            "", "Range: bytes=0-0\r\n", "Range: bytes=0-\r\n", "Range: bytes=-500\r\n", "Range: bytes=500-\r\n",
            "Range: bytes=99999-\r\n", "Range: bytes=5-1\r\n", "Range: bytes=0-1,3-4\r\n", "Range: bytes=0-4,5-9\r\n",
            "Range: bytes=-0\r\n", "Range: items=0-5\r\n", "Range:   bytes=1-2\r\n", "Range: bytes=5\r\n",
            "Range: bytes=0-1\r\nRange: bytes=3-4\r\n",
            "If-None-Match: " + etag + "\r\n", "If-None-Match: \"zz\", " + etag + "\r\n",
            "If-None-Match: W/\"zz\"\r\n", "If-None-Match: *\r\n",
            "If-None-Match: " + etag + "\r\nRange: bytes=0-0\r\n",
            "If-None-Match: " + etag + "\r\nCache-Control: no-cache\r\n",
            "If-Modified-Since: " + lm + "\r\n", "If-Modified-Since: " + older + "\r\n",
            "If-Modified-Since: garbage\r\n", "If-Modified-Since: " + lm + "\r\nIf-None-Match: W/\"zz\"\r\n",
            "If-Range: " + etag + "\r\nRange: bytes=0-0\r\n", "If-Range: W/\"zz\"\r\nRange: bytes=0-0\r\n",
            "If-Range: " + lm + "\r\nRange: bytes=1-1\r\n", "If-Range: " + older + "\r\nRange: bytes=1-1\r\n",
            "If-Range: W/\"zz\"\r\nRange: bytes=99999-\r\n", "If-Match: " + etag + "\r\n", "If-Match: \"zz\"\r\n",
            "If-Match: *\r\n", "If-Unmodified-Since: " + older + "\r\n", "If-Unmodified-Since: " + lm + "\r\n",
            "Connection: close\r\nRange: bytes=2-3\r\n",
        };
        for (const char* route : {"/f", "/d", "/t"}) {
            const std::string target = fileQuery(route, p);
            for (const std::string& x : extras)
                for (bool head : {false, true}) {
                    if (head && x.find("Range") == std::string::npos && !x.empty()) continue;
                    const Resp mine = ask(ts.port(), target, x, head);
                    const Resp theirs = ask(nodePort, target, x, head);
                    QVERIFY2(theirs.ok && mine.ok, qPrintable(QString::fromStdString(esc(x))));
                    const std::string a = normalized(mine), b = normalized(theirs);
                    QVERIFY2(a == b, qPrintable(QString::fromStdString(std::string(route) + " " + esc(x) + "\n--- mine\n" +
                                                                       esc(a) + "\n--- express\n" + esc(b))));
                    ++compared;
                }
        }
    }
    qInfo("sendFile: %d replies byte-identical to Express (status, reason, headers in order except Date and "
          "X-Powered-By, body)", compared);
}

void TestAnfsCoreHttp::sendFileThreeGiBFlatMemory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("sparse.bin");
    const int64_t size = (int64_t(5) << 30) + 123, from = int64_t(2) << 30, total = size - from;
    struct Mark { int64_t at; std::string bytes; };
    const std::vector<Mark> marks = {{from, "START"},
                                     {(int64_t(3) << 30) - 3, "S1|S2"},         // across the first 1 GiB slice
                                     {(int64_t(4) << 30) - 2, "4GiB"},          // across the 32-bit offset
                                     {(int64_t(4) << 30) + 7, "past4"},
                                     {size - 5, "END!!"}};
    {
        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, 0, nullptr);
        QVERIFY(h != INVALID_HANDLE_VALUE);
        DWORD br = 0;
        const bool sparse = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr) != 0;
        LARGE_INTEGER li;
        li.QuadPart = size;
        bool ok = sparse && SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && SetEndOfFile(h);
        for (const Mark& m : marks) {
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(m.at) & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(m.at) >> 32);
            DWORD w = 0;
            ok = ok && WriteFile(h, m.bytes.data(), static_cast<DWORD>(m.bytes.size()), &w, &ov) && w == m.bytes.size();
        }
        CloseHandle(h);
        if (!sparse) QSKIP("the temp volume has no sparse files");
        QVERIFY(ok);
    }
    int64_t markBytes = 0;
    for (const Mark& m : marks) markBytes += static_cast<int64_t>(m.bytes.size());
    auto expected = [&](int64_t pos) -> char {
        for (const Mark& m : marks)
            if (pos >= m.at && pos < m.at + static_cast<int64_t>(m.bytes.size()))
                return m.bytes[static_cast<size_t>(pos - m.at)];
        return 0;
    };
    for (const char* mode : {"1", "0"}) {                // TransmitFile forced on, then the 64 KB copy path
        _putenv_s("ANFS_HTTP_TRANSMITFILE", mode);
        TestServer ts;
        addSendFileRoutes(ts.router);
        ts.start();
        Raw c(ts.port(), 30000);
        const std::string f = fileQuery("/f", path);
        c.send(get(f, "Range: bytes=" + std::to_string(from) + "-\r\n"));
        const std::string head = c.readUntil("\r\n\r\n");
        QVERIFY2(head.rfind("HTTP/1.1 206 Partial Content\r\n", 0) == 0, head.c_str());
        QVERIFY(head.find("Content-Range: bytes 2147483648-5368709242/5368709243\r\n") != std::string::npos);
        QVERIFY(head.find("Content-Length: " + std::to_string(total) + "\r\n") != std::string::npos);
        QVERIFY(head.find("Content-Type: application/octet-stream\r\n") != std::string::npos);

        const int64_t base = privateBytes();
        std::atomic<bool> done{false};
        std::atomic<int64_t> peak{base};
        std::thread mon([&] {
            while (!done.load()) {
                int64_t p = privateBytes();
                if (p > peak.load()) peak = p;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
        int64_t got = 0, wrong = 0, marked = 0;
        auto check = [&](const char* p, size_t n) {
            size_t i = 0;
            for (; i + 8 <= n; i += 8) {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                if (w == 0) {
                    // a mark could still be expected here (only zeros arrived): checked by the count below
                    continue;
                }
                for (size_t k = i; k < i + 8; ++k) {
                    if (!p[k]) continue;
                    if (p[k] == expected(from + got + static_cast<int64_t>(k))) ++marked;
                    else ++wrong;
                }
            }
            for (; i < n; ++i)
                if (p[i]) (p[i] == expected(from + got + static_cast<int64_t>(i)) ? marked : wrong)++;
            got += static_cast<int64_t>(n);
        };
        QElapsedTimer tm;
        tm.start();
        std::string& pre = c.buffer();
        const size_t preN = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(pre.size()), total));
        check(pre.data(), preN);
        pre.erase(0, preN);
        std::vector<char> buf(1 << 20);
        while (got < total) {
            int k = ::recv(c.s_, buf.data(), static_cast<int>(std::min<int64_t>(total - got, int64_t(buf.size()))), 0);
            if (k <= 0) break;
            check(buf.data(), static_cast<size_t>(k));
        }
        done = true;
        mon.join();
        const int64_t ms = tm.elapsed();
        QCOMPARE(got, total);
        QCOMPARE(wrong, int64_t(0));
        QCOMPARE(marked, markBytes);
        const int64_t grew = peak.load() - base;
        qInfo("TransmitFile=%s: %lld bytes in %lld ms (%.0f MB/s), private bytes peak +%lld KB", mode,
              static_cast<long long>(got), static_cast<long long>(ms), ms ? got / 1048.576 / ms : 0.0,
              static_cast<long long>(grew / 1024));
        QVERIFY2(grew < (int64_t(64) << 20), "memory grew with the file");
        // The connection is still in step: a short range past 4 GiB, then HEAD of the whole file.
        c.send(get(f, "Range: bytes=" + std::to_string((int64_t(4) << 30) - 2) + "-" +
                          std::to_string((int64_t(4) << 30) + 11) + "\r\n"));
        Resp r = c.read();
        QCOMPARE(r.status, 206);
        SEQ(r.body, std::string("4GiB") + std::string(5, '\0') + "past4");
        c.send("HEAD " + f + " HTTP/1.1\r\nHost: t\r\n\r\n");
        Resp hd = c.read(true);
        QCOMPARE(hd.status, 200);
        SEQ(hd.header("Content-Length"), std::to_string(size));
    }
    _putenv_s("ANFS_HTTP_TRANSMITFILE", "");
}

QTEST_GUILESS_MAIN(TestAnfsCoreHttp)
#include "test_anfs_core_http.moc"
