// tests/anfs/anfs_testkit.cpp - ANFS-F8: the shared test helpers declared in anfs_testkit.h.
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

#include "anfs_testkit.h"

#include "core/clock.h"
#include "core/svc.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace anfs::testkit {

namespace {

constexpr uintptr_t kInvalid = ~uintptr_t(0);
inline SOCKET sock(uintptr_t s) { return static_cast<SOCKET>(s); }

bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string lowerCopy(std::string_view s)
{
    std::string r(s);
    for (char& c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

bool sendAll(SOCKET s, std::string_view d)
{
    size_t at = 0;
    while (at < d.size()) {
        int k = ::send(s, d.data() + at, static_cast<int>(std::min<size_t>(d.size() - at, 1 << 20)), 0);
        if (k <= 0) return false;
        at += static_cast<size_t>(k);
    }
    return true;
}

void rstClose(SOCKET s)
{
    linger l{1, 0};
    setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
    closesocket(s);
}

// Reads a message body after a parsed head from a buffered reader: Content-Length, chunked (framing kept in raw),
// else nothing (or to close when toClose). fill/take operate on the caller's buffer.
template <class Fill>
bool readBody(std::string& buf, Fill fill, const std::vector<std::pair<std::string, std::string>>& headers,
              std::string& body, std::string& raw, bool toClose)
{
    auto header = [&](std::string_view n) -> const std::string* {
        for (const auto& h : headers)
            if (ieq(h.first, n)) return &h.second;
        return nullptr;
    };
    auto need = [&](size_t n) {
        while (buf.size() < n)
            if (!fill()) return false;
        return true;
    };
    auto take = [&](size_t n) {
        std::string r = buf.substr(0, n);
        buf.erase(0, n);
        raw += r;
        return r;
    };
    auto until = [&](std::string_view delim, std::string& out) {
        size_t p;
        while ((p = buf.find(delim)) == std::string::npos)
            if (!fill()) return false;
        out = take(p + delim.size());
        return true;
    };
    if (const std::string* te = header("Transfer-Encoding"); te && lowerCopy(*te).find("chunked") != std::string::npos) {
        for (;;) {
            std::string line;
            if (!until("\r\n", line)) return false;
            size_t n = std::strtoull(line.c_str(), nullptr, 16);
            if (n == 0) {
                // trailers until the empty line
                for (;;) {
                    std::string t;
                    if (!until("\r\n", t)) return false;
                    if (t == "\r\n") return true;
                }
            }
            if (!need(n + 2)) {
                body += take(std::min(n, buf.size()));
                return false;
            }
            body += take(n).substr(0, n);
            take(2);
        }
    }
    if (const std::string* cl = header("Content-Length")) {
        size_t n = static_cast<size_t>(std::strtoull(cl->c_str(), nullptr, 10));
        bool ok = need(n);
        body = take(std::min(n, buf.size()));
        return ok;
    }
    if (toClose) {
        while (fill()) {
        }
        body = take(buf.size());
        return true;
    }
    return true;
}

} // namespace

// ---- sockets -------------------------------------------------------------------------------------------------------

WsaInit::WsaInit()
{
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
}
WsaInit::~WsaInit() { WSACleanup(); }

int freePort()
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        int len = sizeof a;
        if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
    }
    closesocket(s);
    return port;
}

PortHolder::PortHolder(int p) : s_(kInvalid)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(p));
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 && listen(s, 1) == 0) port_ = p;
    s_ = static_cast<uintptr_t>(s);
}
PortHolder::~PortHolder() { release(); }
void PortHolder::release()
{
    if (s_ != kInvalid) closesocket(sock(s_));
    s_ = kInvalid;
    port_ = 0;
}

// ---- files ---------------------------------------------------------------------------------------------------------

TempDir::TempDir(const QString& prefix)
    : dir_(QDir::tempPath() + QLatin1Char('/') + prefix + QStringLiteral("-XXXXXX"))
{
    path_ = QDir::toNativeSeparators(dir_.path()).toStdString();
}
std::string TempDir::sub(std::string_view name) const { return path_ + "\\" + std::string(name); }
std::string TempDir::mkdir(std::string_view name) const
{
    QDir(QString::fromStdString(path_)).mkpath(QString::fromUtf8(name.data(), static_cast<int>(name.size())));
    return sub(name);
}
std::string TempDir::write(std::string_view name, std::string_view data) const
{
    std::string p = sub(name);
    QFileInfo fi(QString::fromStdString(p));
    QDir().mkpath(fi.absolutePath());
    QFile f(fi.absoluteFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    if (f.write(data.data(), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) return {};
    return p;
}
std::string TempDir::read(std::string_view name) const
{
    QFile f(QString::fromStdString(sub(name)));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray b = f.readAll();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

// ---- TestServer ----------------------------------------------------------------------------------------------------

TestServer::TestServer(http::ServerOptions options)
    : server(router, [this] { return config(); }, nullptr, std::move(options)),
      config_(std::make_shared<const Config>())
{
}
TestServer::~TestServer() { stop(); }
void TestServer::setConfig(Config c)
{
    std::lock_guard<std::mutex> g(mu_);
    config_ = std::make_shared<const Config>(std::move(c));
}
std::shared_ptr<const Config> TestServer::config() const
{
    std::lock_guard<std::mutex> g(mu_);
    return config_;
}
void TestServer::start() { server.start(0, false, "127.0.0.1"); }
void TestServer::stop()
{
    if (server.running()) server.stop();
}
std::string TestServer::url(std::string_view path) const
{
    return "http://127.0.0.1:" + std::to_string(port()) + std::string(path);
}
http::HttpClient::Result TestServer::fetch(std::string_view method, std::string_view path, const http::Headers& headers,
                                           std::string_view body, int idleMs)
{
    return http::HttpClient::shared().fetch(method, url(path), headers, body, idleMs);
}

// ---- ModuleHost ----------------------------------------------------------------------------------------------------

ModuleHost::ModuleHost()
    : tmp(QStringLiteral("anfs-host")), dataDir(tmp.path()), log(), store(dataDir + "\\anfs.json"),
      status(dataDir + "\\status.json", [this] { return statusDoc(); }, &log)
{
    log.setConsole(false);
    startedAt = anfs::nowMs();
}
ModuleHost::~ModuleHost()
{
    if (subscription >= 0) store.unsubscribe(subscription);
    if (mm) {
        mm->shutdown();                           // while `status` (destroyed first otherwise) is alive
        mm.reset();
    }
    Svc::clear();
}
void ModuleHost::add(std::string_view modes, int order, ModuleFactory factory, const char* className)
{
    registry.add(modes, order, std::move(factory), className);
}
void ModuleHost::init()
{
    mm = std::make_unique<ModeManager>(store, log, registry, dataDir, status);
    subscription = store.subscribe([this](std::shared_ptr<const Config> c) { mm->apply(c); });
}
bool ModuleHost::write(const Config& c)
{
    std::string err;
    if (!writeFileAtomic(store.path(), c.toJsonText(), &err)) return false;
    return store.load();
}
void ModuleHost::apply(const Config& c)
{
    if (mm) mm->apply(std::make_shared<const Config>(c));
}
Config ModuleHost::cfg(const char* mode, int port, int64_t rev)
{
    Config c;
    c.mode = mode;
    // Config port 0 means the mode default (18899 / 18080 in test mode), which another test run may hold: a module
    // test asks for an ephemeral port instead. Mode off binds nothing, so it keeps 0.
    if (port == 0 && std::string_view(mode) != Config::kModeOff) port = freePort();
    c.port = port;
    c.bindAll = false;                            // loopback only: no firewall prompt on this box
    c.test = true;
    c.rev = rev;
    c.nodeId = "node-testkit";
    c.name = "testkit";
    return c;
}
// The bound port of the running generation. ModeManager::port() reports the effective config port even in mode
// off (18899 by default), so the server itself is asked.
int ModuleHost::port() const
{
    if (!mm) return 0;
    http::Server* s = mm->server();
    return s && s->running() ? s->port() : 0;
}
std::string ModuleHost::url(std::string_view path) const
{
    return "http://127.0.0.1:" + std::to_string(port()) + std::string(path);
}
http::HttpClient::Result ModuleHost::fetch(std::string_view path, const http::Headers& headers, std::string_view method,
                                           std::string_view body, int idleMs)
{
    return client.fetch(method, url(path), headers, body, idleMs);
}
JVal ModuleHost::statusDoc() const
{
    return mm ? buildStatus(*mm, store, log, startedAt) : JVal::object();
}

// ---- RawResponse / parseHead ------------------------------------------------------------------------------------------

std::vector<std::string> RawResponse::headerNames() const
{
    std::vector<std::string> out;
    for (const auto& h : headers) out.push_back(h.first);
    return out;
}
std::string RawResponse::header(std::string_view name, std::string_view def) const
{
    for (const auto& h : headers)
        if (ieq(h.first, name)) return h.second;
    return std::string(def);
}
bool RawResponse::has(std::string_view name) const
{
    for (const auto& h : headers)
        if (ieq(h.first, name)) return true;
    return false;
}
bool RawResponse::chunked() const { return lowerCopy(header("Transfer-Encoding")).find("chunked") != std::string::npos; }

bool parseHead(std::string_view head, RawResponse& out)
{
    size_t eol = head.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string_view line = head.substr(0, eol);
    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    out.version = std::string(line.substr(0, sp1));
    size_t sp2 = line.find(' ', sp1 + 1);
    std::string_view code = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    out.status = std::atoi(std::string(code).c_str());
    out.reason = sp2 == std::string::npos ? std::string() : std::string(line.substr(sp2 + 1));
    out.headers.clear();
    size_t at = eol + 2;
    while (at < head.size()) {
        size_t e = head.find("\r\n", at);
        if (e == std::string::npos) e = head.size();
        std::string_view l = head.substr(at, e - at);
        at = e + 2;
        if (l.empty()) break;
        size_t c = l.find(':');
        if (c == std::string::npos) return false;
        std::string_view v = l.substr(c + 1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
        out.headers.emplace_back(std::string(l.substr(0, c)), std::string(v));
    }
    return out.status > 0;
}

// ---- RawClient -------------------------------------------------------------------------------------------------------

RawClient::~RawClient() { close(); }
bool RawClient::connectTo(int port, int timeoutMs)
{
    close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return false;
    }
    s_ = static_cast<uintptr_t>(s);
    buf_.clear();
    eof_ = false;
    setTimeout(timeoutMs);
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
    return true;
}
bool RawClient::connected() const { return s_ != kInvalid; }
void RawClient::setTimeout(int ms)
{
    if (s_ == kInvalid) return;
    DWORD v = static_cast<DWORD>(ms);
    setsockopt(sock(s_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v);
}
bool RawClient::send(std::string_view bytes) { return s_ != kInvalid && sendAll(sock(s_), bytes); }
std::string RawClient::request(std::string_view method, std::string_view path,
                               const std::vector<std::pair<std::string, std::string>>& headers, std::string_view body,
                               std::string_view version)
{
    std::string r;
    r.append(method).append(" ").append(path).append(" ").append(version).append("\r\n");
    bool host = false, cl = false;
    for (const auto& h : headers) {
        if (ieq(h.first, "Host")) host = true;
        if (ieq(h.first, "Content-Length") || ieq(h.first, "Transfer-Encoding")) cl = true;
    }
    if (!host) r += "Host: 127.0.0.1\r\n";
    for (const auto& h : headers) r.append(h.first).append(": ").append(h.second).append("\r\n");
    if (!body.empty() && !cl) r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "\r\n";
    r.append(body);
    send(r);
    return r;
}
void RawClient::close()
{
    if (s_ != kInvalid) closesocket(sock(s_));
    s_ = kInvalid;
}
void RawClient::reset()
{
    if (s_ != kInvalid) rstClose(sock(s_));
    s_ = kInvalid;
}
void RawClient::shutdownSend()
{
    if (s_ != kInvalid) shutdown(sock(s_), SD_SEND);
}
bool RawClient::fill()
{
    if (s_ == kInvalid) {
        eof_ = true;
        return false;
    }
    char tmp[65536];
    int k = ::recv(sock(s_), tmp, sizeof tmp, 0);
    if (k > 0) {
        buf_.append(tmp, static_cast<size_t>(k));
        return true;
    }
    eof_ = k == 0 || WSAGetLastError() != WSAETIMEDOUT;
    return false;
}
bool RawClient::need(size_t n)
{
    while (buf_.size() < n)
        if (!fill()) return false;
    return true;
}
std::string RawClient::take(size_t n)
{
    std::string r = buf_.substr(0, n);
    buf_.erase(0, n);
    return r;
}
std::string RawClient::readN(size_t n)
{
    need(n);
    return take(std::min(n, buf_.size()));
}
std::string RawClient::readUntil(std::string_view delim)
{
    size_t p;
    while ((p = buf_.find(delim)) == std::string::npos)
        if (!fill()) return take(buf_.size());
    return take(p + delim.size());
}
std::string RawClient::readToClose()
{
    while (fill()) {
    }
    return take(buf_.size());
}
std::string RawClient::readHead() { return readUntil("\r\n\r\n"); }
RawResponse RawClient::readResponse(bool headOnly)
{
    RawResponse r;
    std::string head = readHead();
    r.raw = head;
    if (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0 || !parseHead(head, r)) return r;
    if (headOnly || r.status == 204 || r.status == 304 || r.status / 100 == 1) {
        r.complete = true;
        return r;
    }
    bool toClose = ieq(r.header("Connection"), "close") || r.version == "HTTP/1.0";
    r.complete = readBody(buf_, [this] { return fill(); }, r.headers, r.body, r.raw, toClose);
    return r;
}

// ---- FakePeer -------------------------------------------------------------------------------------------------------

std::string FakeRequest::header(std::string_view name, std::string_view def) const
{
    for (const auto& h : headers)
        if (ieq(h.first, name)) return h.second;
    return std::string(def);
}
FakeResponse FakeResponse::json(int status, std::string_view body)
{
    FakeResponse r;
    r.status = status;
    r.body = std::string(body);
    return r;
}
FakeResponse FakeResponse::text(int status, std::string_view body)
{
    FakeResponse r;
    r.status = status;
    r.contentType = "text/plain; charset=utf-8";
    r.body = std::string(body);
    return r;
}

FakePeer::FakePeer()
{
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof a);
    listen(ls, 64);
    int len = sizeof a;
    getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    ls_ = static_cast<uintptr_t>(ls);
    acceptThread_ = std::thread([this] {
        for (;;) {
            SOCKET s = accept(sock(ls_), nullptr, nullptr);
            if (s == INVALID_SOCKET) break;
            DWORD t = 50;                         // reads poll so a stopping server never waits on a blocked recv
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
            BOOL nd = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
            std::lock_guard<std::mutex> g(mu_);
            if (stop_.load()) {
                closesocket(s);
                break;
            }
            ++accepts_;
            socks_.push_back(static_cast<uintptr_t>(s));
            workers_.emplace_back([this, s] {
                try {
                    serve(static_cast<uintptr_t>(s));
                } catch (...) {
                }
            });
        }
    });
}
FakePeer::~FakePeer() { stop(); }
std::string FakePeer::url(std::string_view path) const { return origin() + std::string(path); }
std::string FakePeer::origin() const { return "http://127.0.0.1:" + std::to_string(port_); }
void FakePeer::on(std::string method, std::string path, FakeResponse r)
{
    std::lock_guard<std::mutex> g(mu_);
    canned_.push_back({std::move(method), std::move(path), std::move(r)});
}
void FakePeer::setHandler(Handler h)
{
    std::lock_guard<std::mutex> g(mu_);
    handler_ = std::move(h);
}
void FakePeer::clearRoutes()
{
    std::lock_guard<std::mutex> g(mu_);
    canned_.clear();
    handler_ = nullptr;
}
void FakePeer::setDefaultDelayMs(int ms)
{
    std::lock_guard<std::mutex> g(mu_);
    defaultDelayMs_ = ms;
}
std::vector<FakeRequest> FakePeer::requests() const
{
    std::lock_guard<std::mutex> g(mu_);
    return requests_;
}
size_t FakePeer::requestCount() const
{
    std::lock_guard<std::mutex> g(mu_);
    return requests_.size();
}
void FakePeer::clearRequests()
{
    std::lock_guard<std::mutex> g(mu_);
    requests_.clear();
}
bool FakePeer::waitForRequests(size_t n, int timeoutMs) const
{
    return waitUntil([&] { return requestCount() >= n; }, timeoutMs);
}
void FakePeer::stop()
{
    if (stop_.exchange(true)) return;
    closesocket(sock(ls_));
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::thread> ws;
    std::vector<uintptr_t> socks;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (uintptr_t s : socks_) shutdown(sock(s), SD_BOTH);
        ws.swap(workers_);
        socks.swap(socks_);
    }
    for (auto& w : ws) w.join();
    for (uintptr_t s : socks) closesocket(sock(s));
}
FakeResponse FakePeer::answer(const FakeRequest& q) const
{
    Handler h;
    std::vector<Canned> canned;
    int delay = 0;
    {
        std::lock_guard<std::mutex> g(mu_);
        h = handler_;
        canned = canned_;
        delay = defaultDelayMs_;
    }
    FakeResponse out;
    bool found = false;
    if (h) {
        out = h(q);
        found = out.status != 0;
    }
    if (!found) {
        for (const auto& c : canned) {
            if (c.method != "*" && !ieq(c.method, q.method)) continue;
            bool match = !c.path.empty() && c.path.back() == '*'
                             ? q.path.compare(0, c.path.size() - 1, c.path, 0, c.path.size() - 1) == 0
                             : q.path == c.path;
            if (!match) continue;
            out = c.r;
            found = true;
            break;
        }
    }
    if (!found) out = FakeResponse::json(404, "{\"error\":\"Not found\"}");
    if (out.delayMs == 0) out.delayMs = delay;
    return out;
}

// One connection: requests in sequence (keep-alive) until the client leaves or stop().
void FakePeer::serve(uintptr_t su)
{
    SOCKET s = sock(su);
    std::string buf;
    bool eof = false;
    auto fill = [&] {
        char tmp[65536];
        for (;;) {
            if (stop_.load()) return false;
            int k = ::recv(s, tmp, sizeof tmp, 0);
            if (k > 0) {
                buf.append(tmp, static_cast<size_t>(k));
                return true;
            }
            if (k == 0 || WSAGetLastError() != WSAETIMEDOUT) {
                eof = true;
                return false;
            }
        }
    };
    auto sleepSlices = [&](int ms) {
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until && !stop_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    for (;;) {
        size_t p;
        while ((p = buf.find("\r\n\r\n")) == std::string::npos)
            if (!fill()) return;
        FakeRequest q;
        std::string head = buf.substr(0, p + 4);
        buf.erase(0, p + 4);
        q.raw = head;
        {
            size_t eol = head.find("\r\n");
            std::string line = head.substr(0, eol);
            size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
            if (sp1 == std::string::npos || sp2 == sp1) return;
            q.method = line.substr(0, sp1);
            q.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
            q.version = line.substr(sp2 + 1);
            q.path = q.target.substr(0, q.target.find('?'));
            size_t at = eol + 2;
            while (at < head.size()) {
                size_t e = head.find("\r\n", at);
                std::string l = head.substr(at, e - at);
                at = e + 2;
                if (l.empty()) break;
                size_t c = l.find(':');
                if (c == std::string::npos) continue;
                std::string v = l.substr(c + 1);
                while (!v.empty() && v.front() == ' ') v.erase(0, 1);
                q.headers.emplace_back(l.substr(0, c), v);
            }
        }
        readBody(buf, fill, q.headers, q.body, q.raw, false);
        {
            std::lock_guard<std::mutex> g(mu_);
            requests_.push_back(q);
        }
        FakeResponse r = answer(q);
        if (r.hold) {
            while (fill()) buf.clear();
            return;
        }
        if (r.delayMs > 0) sleepSlices(r.delayMs);
        if (stop_.load()) return;
        if (r.abortBeforeHead) {
            rstClose(s);
            std::lock_guard<std::mutex> g(mu_);
            socks_.erase(std::remove(socks_.begin(), socks_.end(), su), socks_.end());
            return;
        }
        bool headOnly = q.method == "HEAD";
        std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + http::reasonPhrase(r.status) + "\r\n";
        if (!r.contentType.empty()) out += "Content-Type: " + r.contentType + "\r\n";
        for (const auto& h : r.headers) out += h.first + ": " + h.second + "\r\n";
        if (r.chunked) out += "Transfer-Encoding: chunked\r\n";
        else out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
        out += r.close ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
        out += "\r\n";
        if (!sendAll(s, out)) return;
        if (r.abortAfterHead) {
            rstClose(s);
            std::lock_guard<std::mutex> g(mu_);
            socks_.erase(std::remove(socks_.begin(), socks_.end(), su), socks_.end());
            return;
        }
        if (r.bodyDelayMs > 0) sleepSlices(r.bodyDelayMs);
        if (!headOnly) {
            if (r.chunked) {
                size_t step = r.chunkSize ? r.chunkSize : std::max<size_t>(r.body.size(), 1);
                for (size_t at = 0; at < r.body.size(); at += step) {
                    size_t n = std::min(step, r.body.size() - at);
                    char hex[32];
                    std::snprintf(hex, sizeof hex, "%zx\r\n", n);
                    if (!sendAll(s, hex) || !sendAll(s, std::string_view(r.body).substr(at, n)) || !sendAll(s, "\r\n")) return;
                }
                if (!sendAll(s, "0\r\n\r\n")) return;
            } else if (!sendAll(s, r.body)) {
                return;
            }
        }
        if (r.close) {
            shutdown(s, SD_SEND);
            return;
        }
        if (eof) return;
    }
}

// ---- MemProbe --------------------------------------------------------------------------------------------------------

MemSample memSample()
{
    MemSample m;
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c)) {
        m.workingSet = static_cast<int64_t>(c.WorkingSetSize);
        m.peakWorkingSet = static_cast<int64_t>(c.PeakWorkingSetSize);
        m.privateBytes = static_cast<int64_t>(c.PrivateUsage);
        m.peakPrivateBytes = static_cast<int64_t>(c.PeakPagefileUsage);
    }
    return m;
}
std::string MemProbe::describe() const
{
    auto mb = [](int64_t b) {
        char t[32];
        std::snprintf(t, sizeof t, "%+.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
        return std::string(t);
    };
    MemSample n = now();
    return "ws " + mb(n.workingSet - start_.workingSet) + ", peak ws " + mb(n.peakWorkingSet - start_.peakWorkingSet) +
           ", private " + mb(n.privateBytes - start_.privateBytes) + ", peak private " +
           mb(n.peakPrivateBytes - start_.peakPrivateBytes);
}

// ---- misc ---------------------------------------------------------------------------------------------------------

bool waitUntil(const std::function<bool()>& pred, int timeoutMs)
{
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= until) return pred();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
std::vector<std::string> keysOf(const JVal& o)
{
    std::vector<std::string> out;
    for (const auto& kv : o.obj()) out.push_back(kv.first);
    return out;
}
std::string pwshPath()
{
    QString p = QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe");
    if (QFileInfo::exists(p)) return QDir::toNativeSeparators(p).toStdString();
    p = QStandardPaths::findExecutable(QStringLiteral("pwsh"));
    return p.isEmpty() ? std::string() : QDir::toNativeSeparators(p).toStdString();
}

} // namespace anfs::testkit
