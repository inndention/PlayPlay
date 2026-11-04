// core/http_server.cpp - ANFS-F4a: the Winsock2 HTTP/1.1 server core (anfs-cpp.md 2.4). ANFS-F4b: sendFile,
// parseRange, weakEtag, parseHttpDate and contentDisposition (the SendFile section at the end).
//
// - One blocking accept thread per listener. Accepted sockets go to an elastic worker pool (threads made on demand
//   with a 256 KB reserved stack, idle ones expire after 30 s). When every one of maxThreads workers holds a
//   connection, the accept thread answers 503 {"error":"Server busy."} itself without blocking (non-blocking socket,
//   the reply fits the empty send buffer) and hands the socket to the closer thread.
// - A worker owns its connection until it closes: keep-alive and pipelining run in the loop below. Inactivity
//   timeouts are SO_RCVTIMEO / SO_SNDTIMEO per phase: keep-alive idle (5 s) until the first byte of the next request,
//   header read (30 s, the first request waits this long for its first byte), body read and write (120 s).
// - Closing is graceful (shutdown(SD_SEND), then the closer thread reads and discards what the client still sends for
//   up to 2 s), so a client that is still uploading sees our reply instead of a reset. Response::abort() is the RST.
// - Protocol errors get Node's bare replies ("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n").
// - TransmitFile only on server editions: client editions of Windows run at most two TransmitFile calls at a time
//   machine-wide and queue the rest (Win32 docs), so one stalled download would hold up others. Elsewhere
//   Response::writeFile copies through 64 KB positioned reads. ANFS_HTTP_TRANSMITFILE=1/0 forces it on/off (tests).
#include "core/http_server.h"

#include "core/clock.h"
#include "core/fsutil.h"
#include "core/http_internal.h"
#include "core/json.h"
#include "core/mime.h"
#include "core/nodepath.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <process.h>
#include <versionhelpers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace anfs::http {

// ---- dates ------------------------------------------------------------------------------------------------------

std::string httpDate(int64_t epochMs)
{
    static const char* kDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    __time64_t secs = static_cast<__time64_t>(epochMs >= 0 ? epochMs / 1000 : (epochMs - 999) / 1000);
    struct tm t {};
    if (_gmtime64_s(&t, &secs) != 0) return {};
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s, %02d %s %04d %02d:%02d:%02d GMT", kDay[t.tm_wday], t.tm_mday, kMon[t.tm_mon],
                  t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

namespace {

using detail::iequals;

// ---- socket helpers ---------------------------------------------------------------------------------------------

struct WsaInit {
    WsaInit()
    {
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WsaInit()
    {
        if (ok) WSACleanup();
    }
    bool ok = false;
};

IoError ioErrorFor(int e, const char* what)
{
    std::string text = std::string(what) + " failed (" + std::to_string(e) + ")";
    if (e == WSAETIMEDOUT) return IoError(std::string(what) + " timed out", false, true);
    return IoError(text, true);                 // reset, aborted, shut down, closed by stop(): the client is gone
}

void setTimeout(SOCKET s, int opt, int ms)
{
    DWORD v = static_cast<DWORD>(std::max(ms, 1));
    setsockopt(s, SOL_SOCKET, opt, reinterpret_cast<const char*>(&v), sizeof v);
}

void setNonBlocking(SOCKET s)
{
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}

std::string addrText(const sockaddr_in& a)
{
    char b[INET_ADDRSTRLEN] = {};
    InetNtopA(AF_INET, &a.sin_addr, b, sizeof b);
    return b;
}

bool useTransmitFile()
{
    if (const char* e = std::getenv("ANFS_HTTP_TRANSMITFILE")) {
        if (std::strcmp(e, "1") == 0) return true;
        if (std::strcmp(e, "0") == 0) return false;
    }
    return IsWindowsServer();
}

class SocketWire final : public detail::Wire {
public:
    SocketWire(SOCKET s, bool transmitFile, int writeTimeoutMs) : s_(s), tf_(transmitFile), writeMs_(writeTimeoutMs) {}

    size_t recv(char* buf, size_t n) override
    {
        if (n == 0) return 0;
        if (ppos_ < pend_.size()) {
            size_t k = std::min(n, pend_.size() - ppos_);
            std::memcpy(buf, pend_.data() + ppos_, k);
            ppos_ += k;
            if (ppos_ == pend_.size()) {
                pend_.clear();
                ppos_ = 0;
            }
            return k;
        }
        if (dead_) throw IoError("connection closed", true);
        int r = ::recv(s_, buf, static_cast<int>(std::min<size_t>(n, 1u << 30)), 0);
        if (r > 0) return static_cast<size_t>(r);
        if (r == 0) return 0;
        throw ioErrorFor(WSAGetLastError(), "recv");
    }

    void unread(const char* p, size_t n) override
    {
        if (n == 0) return;
        std::string rest = pend_.substr(ppos_);
        pend_.assign(p, n);
        pend_ += rest;
        ppos_ = 0;
    }

    void send(const char* p, size_t n) override
    {
        if (dead_) throw IoError("connection closed", true);
        while (n > 0) {
            int k = ::send(s_, p, static_cast<int>(std::min<size_t>(n, 1u << 20)), 0);
            if (k == SOCKET_ERROR) {
                dead_ = true;                   // after a send timeout the socket state is undefined (Winsock docs)
                throw ioErrorFor(WSAGetLastError(), "send");
            }
            p += k;
            n -= static_cast<size_t>(k);
        }
    }

    bool sendFile(void* fileHandle, int64_t offset, int64_t count) override
    {
        if (!tf_) return false;
        if (dead_) throw IoError("connection closed", true);
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return false;
        struct EvGuard { HANDLE h; ~EvGuard() { CloseHandle(h); } } guard{ev};
        while (count > 0) {
            DWORD n = static_cast<DWORD>(std::min<int64_t>(count, int64_t(1) << 30));   // 1 GiB slices
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
            ov.hEvent = ev;
            ResetEvent(ev);
            if (!TransmitFile(s_, static_cast<HANDLE>(fileHandle), n, 0, &ov, nullptr, 0)) {
                int e = WSAGetLastError();
                if (e != WSA_IO_PENDING && e != ERROR_IO_PENDING) {
                    dead_ = true;
                    throw ioErrorFor(e, "TransmitFile");
                }
                // The write timeout is an inactivity limit; a slice may legitimately take long on a slow link, so
                // it gets the write timeout plus the time the slice needs at 32 KB/s.
                DWORD waitMs = static_cast<DWORD>(std::min<int64_t>(
                    int64_t(writeMs_) + int64_t(n) / 32, int64_t(INFINITE) - 1));
                if (WaitForSingleObject(ev, waitMs) == WAIT_TIMEOUT) {
                    CancelIoEx(reinterpret_cast<HANDLE>(s_), &ov);
                    DWORD x = 0, f = 0;
                    WSAGetOverlappedResult(s_, &ov, &x, TRUE, &f);
                    dead_ = true;
                    throw IoError("TransmitFile timed out", false, true);
                }
            }
            DWORD sent = 0, flags = 0;
            if (!WSAGetOverlappedResult(s_, &ov, &sent, TRUE, &flags)) {
                dead_ = true;
                throw ioErrorFor(WSAGetLastError(), "TransmitFile");
            }
            if (sent != n) {
                dead_ = true;
                throw IoError("TransmitFile sent " + std::to_string(sent) + " of " + std::to_string(n) + " bytes");
            }
            offset += n;
            count -= n;
        }
        return true;
    }

    void abort() override
    {
        if (aborted_) return;
        aborted_ = true;
        dead_ = true;
        linger l{1, 0};                         // closesocket() then sends RST
        setsockopt(s_, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        shutdown(s_, SD_BOTH);
    }

    bool aborted() const { return aborted_; }
    bool dead() const { return dead_; }
    bool hasBuffered() const { return ppos_ < pend_.size(); }

private:
    SOCKET s_;
    bool tf_;
    int writeMs_;
    std::string pend_;
    size_t ppos_ = 0;
    bool dead_ = false;
    bool aborted_ = false;
};

struct Conn {
    SOCKET s = INVALID_SOCKET;
    bool proxy = false;
};

// ---- request parsing --------------------------------------------------------------------------------------------

struct ParseFail {
    int status;
};

int64_t parseContentLength(std::string_view v)
{
    v = detail::trimOws(v);
    if (v.empty()) throw ParseFail{400};
    int64_t n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') throw ParseFail{400};
        if (n > (INT64_MAX - (c - '0')) / 10) throw ParseFail{400};
        n = n * 10 + (c - '0');
    }
    return n;
}

// The header block (request line + header lines, without the final empty line) into req. Strict like llhttp:
// CRLF line ends, no obs-fold, no whitespace before ':', upper-case token methods, one Content-Length, no
// Content-Length together with Transfer-Encoding, chunked as the final coding, Host required on HTTP/1.1.
void parseHead(std::string_view block, Request& req)
{
    size_t i = 0;
    auto nextLine = [&](std::string_view& out) -> bool {
        if (i >= block.size()) return false;
        size_t e = block.find("\r\n", i);
        if (e == std::string_view::npos) e = block.size();
        out = block.substr(i, e - i);
        i = e + 2;
        for (char c : out)
            if (c == '\r' || c == '\n') throw ParseFail{400};   // bare CR or LF
        return true;
    };
    std::string_view line;
    if (!nextLine(line)) throw ParseFail{400};
    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp1 == 0 || sp2 == sp1 + 1)
        throw ParseFail{400};
    std::string_view method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1),
                     version = line.substr(sp2 + 1);
    for (char c : method)
        if (!((c >= 'A' && c <= 'Z') || c == '-' || c == '_')) throw ParseFail{400};
    for (char c : target)
        if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7F) throw ParseFail{400};
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || version[6] != '.' || version[5] < '0' ||
        version[5] > '9' || version[7] < '0' || version[7] > '9')
        throw ParseFail{400};
    if (version[5] != '1' || (version[7] != '0' && version[7] != '1')) throw ParseFail{505};
    req.method = std::string(method);
    req.target = std::string(target);
    req.httpMinor = version[7] - '0';

    std::string_view pathPart = target;
    if (pathPart.size() > 7 && (iequals(pathPart.substr(0, 7), "http://") ||
                                (pathPart.size() > 8 && iequals(pathPart.substr(0, 8), "https://")))) {
        size_t s = pathPart.find("//") + 2;   // absolute-form: the path starts after the authority
        size_t slash = pathPart.find_first_of("/?", s);
        pathPart = slash == std::string_view::npos ? std::string_view("/") : pathPart.substr(slash);
        if (!pathPart.empty() && pathPart[0] == '?') req.path = "/";
    }
    size_t q = pathPart.find('?');
    if (req.path.empty()) req.path = std::string(pathPart.substr(0, q));
    req.rawQuery = q == std::string_view::npos ? std::string() : std::string(pathPart.substr(q + 1));
    req.query = Query(req.rawQuery);

    bool haveCl = false, haveTe = false, haveHost = false;
    std::string te;
    while (nextLine(line)) {
        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') throw ParseFail{400};     // obs-fold
        size_t c = line.find(':');
        if (c == std::string_view::npos || c == 0) throw ParseFail{400};
        std::string_view name = line.substr(0, c);
        for (char ch : name)
            if (!detail::isTokenChar(static_cast<unsigned char>(ch))) throw ParseFail{400};
        std::string_view value = detail::trimOws(line.substr(c + 1));
        for (char ch : value)
            if ((static_cast<unsigned char>(ch) < 0x20 && ch != '\t') || ch == 0x7F) throw ParseFail{400};
        if (iequals(name, "Content-Length")) {
            if (haveCl) throw ParseFail{400};   // Node refuses a repeated Content-Length outright
            haveCl = true;
            req.contentLength = parseContentLength(value);
        } else if (iequals(name, "Transfer-Encoding")) {
            if (!te.empty()) te += ", ";
            te += std::string(value);
            haveTe = true;
        } else if (iequals(name, "Host")) {
            haveHost = true;
        }
        req.headers.add(std::string(name), std::string(value));
    }
    if (haveTe) {
        if (haveCl) throw ParseFail{400};
        size_t last = te.rfind(',');
        std::string_view fin = detail::trimOws(std::string_view(te).substr(last == std::string::npos ? 0 : last + 1));
        if (!iequals(fin, "chunked")) throw ParseFail{400};
        req.chunked = true;
        req.contentLength = -1;
    }
    if (req.httpMinor == 1 && !haveHost) throw ParseFail{400};
    if (const std::string* ex = req.headers.find("Expect")) {
        if (!iequals(detail::trimOws(*ex), "100-continue")) throw ParseFail{417};
        req.expectContinue = req.httpMinor >= 1 && (req.chunked || req.contentLength > 0);
    }
}

} // namespace

// ---- Server -----------------------------------------------------------------------------------------------------

struct Server::Impl {
    Impl(Server* self, Router& router, std::function<std::shared_ptr<const Config>()> config, Log* log,
         ServerOptions opt)
        : self(self), router(router), config(std::move(config)), log(log ? log : &nullLog()), opt(std::move(opt))
    {
        tf = useTransmitFile();
    }

    Server* self;
    Router& router;
    std::function<std::shared_ptr<const Config>()> config;
    Log* log;
    ServerOptions opt;
    WsaInit wsa;
    bool tf = false;

    struct Listener {
        SOCKET s = INVALID_SOCKET;
        int port = 0;
        bool proxy = false;
        std::string label;
        std::thread th;
    };

    mutable std::mutex mu;
    std::condition_variable cvWork;             // idle workers wait for queued connections
    std::condition_variable cvDone;             // stop() waits for the pool to empty
    std::vector<std::unique_ptr<Listener>> listeners;
    std::deque<Conn*> queue;
    std::set<Conn*> live;                       // connections a worker is serving (stop() wakes them)
    int threads = 0;
    int idle = 0;
    bool running = false;
    std::atomic<bool> draining{false};
    std::atomic<bool> stopping{false};
    std::atomic<int> inflight{0};
    std::atomic<int64_t> requests{0};
    std::atomic<int64_t> rejected{0};
    int mainPort = 0;

    // closer thread: graceful closes and the 503 rejects
    std::mutex cmu;
    std::condition_variable ccv;
    std::vector<std::pair<SOCKET, int64_t>> closing;   // socket, deadline (monotonic ms)
    std::thread closer;
    bool closerStop = false;

    // ---- listeners ----

    int listenOn(const std::string& addr, int port, bool proxy, const std::string& labelSuffix)
    {
        if (!wsa.ok) throw BindError("WSAStartup failed", WSASYSNOTREADY);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) throw BindError("socket() failed", WSAGetLastError());
        BOOL excl = TRUE;                       // nobody else may bind the same port (no port stealing)
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<u_short>(port));
        if (InetPtonA(AF_INET, addr.c_str(), &a.sin_addr) != 1) {
            closesocket(s);
            throw BindError("invalid bind address " + addr, WSAEINVAL);
        }
        if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, SOMAXCONN) != 0) {
            int e = WSAGetLastError();
            closesocket(s);
            throw BindError("cannot listen on " + addr + ":" + std::to_string(port) + " (" + std::to_string(e) + ")", e);
        }
        int len = sizeof a;
        getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        auto l = std::make_unique<Listener>();
        l->s = s;
        l->port = ntohs(a.sin_port);
        l->proxy = proxy;
        l->label = addr + ":" + std::to_string(l->port) + labelSuffix;
        Listener* raw = l.get();
        l->th = std::thread([this, raw] { acceptLoop(*raw); });
        listeners.push_back(std::move(l));
        return raw->port;
    }

    void acceptLoop(Listener& l)
    {
        for (;;) {
            SOCKET c = accept(l.s, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (stopping.load()) return;
                int e = WSAGetLastError();
                if (e == WSAENOTSOCK || e == WSAEINVAL) return;   // listener closed
                std::this_thread::sleep_for(std::chrono::milliseconds(20));   // WSAEMFILE / WSAENOBUFS: back off
                continue;
            }
            if (stopping.load()) {
                closesocket(c);
                continue;
            }
            auto* conn = new Conn{c, l.proxy};
            assign(conn);
        }
    }

    // ---- pool ----

    struct StartArg {
        Impl* d;
        Conn* c;
    };

    static unsigned __stdcall workerMain(void* p)
    {
        StartArg a = *static_cast<StartArg*>(p);
        delete static_cast<StartArg*>(p);
        a.d->workerLoop(a.c);
        return 0;
    }

    void assign(Conn* c)
    {
        std::unique_lock<std::mutex> lk(mu);
        if (draining.load()) {
            lk.unlock();
            reject(c);
            return;
        }
        if (idle > static_cast<int>(queue.size())) {
            queue.push_back(c);
            cvWork.notify_one();
            return;
        }
        if (threads < opt.maxThreads) {
            ++threads;
            auto* arg = new StartArg{this, c};
            uintptr_t h = _beginthreadex(nullptr, static_cast<unsigned>(opt.threadStackBytes), &Impl::workerMain, arg,
                                         STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
            if (h) {
                CloseHandle(reinterpret_cast<HANDLE>(h));
                return;
            }
            delete arg;
            --threads;
            cvDone.notify_all();
        }
        lk.unlock();
        reject(c);
    }

    // 503 {"error":"Server busy."} from the accept thread, never blocking it.
    void reject(Conn* c)
    {
        rejected.fetch_add(1);
        SOCKET s = c->s;
        delete c;
        setNonBlocking(s);
        std::string body = json::errorBody("Server busy.");
        std::string r = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json; charset=utf-8\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\nDate: " + httpDate(anfs::nowMs()) +
                        "\r\nConnection: close\r\n\r\n" + body;
        ::send(s, r.data(), static_cast<int>(r.size()), 0);
        closeGracefully(s);
    }

    void workerLoop(Conn* c)
    {
        for (;;) {
            serve(c);
            std::unique_lock<std::mutex> lk(mu);
            ++idle;
            cvWork.wait_for(lk, std::chrono::milliseconds(std::max(opt.idleThreadExpiryMs, 1)),
                            [&] { return !queue.empty() || stopping.load(); });
            --idle;
            if (!queue.empty() && !stopping.load()) {
                c = queue.front();
                queue.pop_front();
                continue;
            }
            --threads;
            cvDone.notify_all();
            return;
        }
    }

    // ---- closing ----

    void startCloser()
    {
        if (closer.joinable()) return;
        {
            std::lock_guard<std::mutex> lk(cmu);
            closerStop = false;
        }
        closer = std::thread([this] { closerLoop(); });
    }

    // shutdown(SD_SEND) now; the closer thread discards whatever the client still sends (an upload we did not read)
    // until it closes or 2 s pass, so the client reads our reply instead of a reset.
    void closeGracefully(SOCKET s)
    {
        shutdown(s, SD_SEND);
        setNonBlocking(s);
        {
            std::lock_guard<std::mutex> lk(cmu);
            if (closer.joinable() && !closerStop) {
                closing.emplace_back(s, anfs::monoMs() + 2000);
                ccv.notify_one();
                return;
            }
        }
        closesocket(s);
    }

    void closerLoop()
    {
        char scratch[16 * 1024];
        std::unique_lock<std::mutex> lk(cmu);
        for (;;) {
            if (closing.empty()) {
                if (closerStop) return;
                ccv.wait(lk, [&] { return closerStop || !closing.empty(); });
                continue;
            }
            std::vector<std::pair<SOCKET, int64_t>> work;
            work.swap(closing);
            lk.unlock();
            std::vector<std::pair<SOCKET, int64_t>> keep;
            int64_t now = anfs::monoMs();
            for (auto& w : work) {
                bool done = now >= w.second;
                for (int i = 0; i < 64 && !done; ++i) {      // at most 1 MB per socket per pass
                    int r = ::recv(w.first, scratch, sizeof scratch, 0);
                    if (r > 0) continue;
                    if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) break;
                    done = true;                // EOF or error
                }
                if (done) closesocket(w.first);
                else keep.push_back(w);
            }
            lk.lock();
            closing.insert(closing.end(), keep.begin(), keep.end());
            if (!closing.empty() && !closerStop) ccv.wait_for(lk, std::chrono::milliseconds(20));
            if (closerStop) {
                for (auto& w : closing) closesocket(w.first);
                closing.clear();
                return;
            }
        }
    }

    // ---- one connection ----

    void logRequestError(const Request& req, const std::string& what)
    {
        if (!stopping.load()) log->error("request failed: " + req.method + " " + req.path + ": " + what);
    }

    void serve(Conn* c)
    {
        SOCKET s = c->s;
        const bool proxy = c->proxy;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopping.load()) {
                delete c;
                closesocket(s);
                return;
            }
            live.insert(c);
        }
        bool hard = false;
        try {
            hard = serveConnection(s, proxy);
        } catch (...) {
            hard = true;                         // nothing escapes a worker
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            live.erase(c);
        }
        delete c;
        if (hard) closesocket(s);
        else closeGracefully(s);
    }

    // Returns true when the socket must be closed hard (aborted, broken), false for a graceful close.
    bool serveConnection(SOCKET s, bool proxy)
    {
        BOOL nd = opt.tcpNoDelay ? TRUE : FALSE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof nd);
        setTimeout(s, SO_SNDTIMEO, opt.writeTimeoutMs);
        SocketWire wire(s, tf, opt.writeTimeoutMs);

        sockaddr_in peer{}, local{};
        int pl = sizeof peer, ll = sizeof local;
        getpeername(s, reinterpret_cast<sockaddr*>(&peer), &pl);
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &ll);
        std::string peerIp = addrText(peer), localIp = addrText(local);
        int peerPort = ntohs(peer.sin_port), localPort = ntohs(local.sin_port);
        bool secure = false;

        std::string hb;
        if (proxy) {
            // PROXY protocol v1 from the TLS front on loopback only: "PROXY TCP4 <c> <s> <cp> <sp>\r\n" (<= 107 bytes).
            if (peerIp != "127.0.0.1") return true;
            setTimeout(s, SO_RCVTIMEO, opt.headerTimeoutMs);
            char tmp[256];
            size_t eol;
            try {
                while ((eol = hb.find("\r\n")) == std::string::npos) {
                    if (hb.size() > 107) return true;
                    size_t k = wire.recv(tmp, sizeof tmp);
                    if (k == 0) return true;
                    hb.append(tmp, k);
                }
            } catch (const IoError&) {
                return true;
            }
            if (eol > 105) return true;
            std::string line = hb.substr(0, eol);
            wire.unread(hb.data() + eol + 2, hb.size() - eol - 2);
            hb.clear();
            char c4[64] = {}, s4[64] = {};
            unsigned cp = 0, sp = 0;
            if (line.rfind("PROXY TCP4 ", 0) == 0 &&
                std::sscanf(line.c_str() + 11, "%63s %63s %u %u", c4, s4, &cp, &sp) == 4 && cp <= 65535) {
                in_addr chk{};
                if (InetPtonA(AF_INET, c4, &chk) != 1) return true;
                peerIp = c4;
                peerPort = static_cast<int>(cp);
            } else if (line != "PROXY UNKNOWN" && line.rfind("PROXY UNKNOWN ", 0) != 0) {
                return true;
            }
            secure = true;
        }

        bool first = true;
        for (;;) {
            // ---- the header block ----
            hb.clear();
            size_t end = std::string::npos, scanFrom = 0;
            bool started = false;
            setTimeout(s, SO_RCVTIMEO, (first && !wire.hasBuffered()) ? opt.headerTimeoutMs : opt.keepAliveIdleMs);
            char tmp[16 * 1024];
            for (;;) {
                // RFC 9112 2.2: ignore empty lines before the request line (a client's trailing CRLF after a body).
                size_t lead = 0;
                while (lead + 1 < hb.size() && hb[lead] == '\r' && hb[lead + 1] == '\n') lead += 2;
                if (lead) {
                    hb.erase(0, lead);
                    scanFrom = 0;
                }
                end = hb.find("\r\n\r\n", scanFrom > 3 ? scanFrom - 3 : 0);
                if (end != std::string::npos) break;
                scanFrom = hb.size();
                if (hb.size() > opt.maxHeaderBytes) {
                    sendBare(wire, 431);
                    return false;
                }
                size_t k;
                try {
                    k = wire.recv(tmp, sizeof tmp);
                } catch (const IoError&) {
                    return true;                 // idle keep-alive or header timeout, reset, stop()
                }
                if (k == 0) return false;        // the client closed between requests
                hb.append(tmp, k);
                if (!started) {
                    started = true;
                    setTimeout(s, SO_RCVTIMEO, opt.headerTimeoutMs);
                }
            }
            if (end + 4 > opt.maxHeaderBytes) {
                sendBare(wire, 431);
                return false;
            }
            wire.unread(hb.data() + end + 4, hb.size() - end - 4);   // body bytes or the next pipelined request

            Request req;
            try {
                parseHead(std::string_view(hb.data(), end), req);
            } catch (const ParseFail& f) {
                sendBare(wire, f.status);
                return false;
            }
            req.peerIp = peerIp;
            req.peerPort = peerPort;
            req.localIp = localIp;
            req.localPort = localPort;
            req.secure = secure;
            requests.fetch_add(1);

            std::shared_ptr<const Config> cfg = config ? config() : nullptr;
            if (!cfg) cfg = std::make_shared<const Config>();
            req.clientIp = req.peerIp;
            req.clientClass = net::NetClass::Wan;          // fail closed until the runtime wires resolveClient
            if (opt.resolveClient) {
                try {
                    opt.resolveClient(req, *cfg);
                } catch (...) {
                    req.clientIp = req.peerIp;
                    req.clientClass = net::NetClass::Wan;
                }
            }

            detail::RequestScope scope;
            scope.keepAliveSecs = opt.keepAliveIdleMs > 0 ? opt.keepAliveIdleMs / 1000 : -1;   // Node: Math.floor
            scope.closing = &draining;
            detail::ScopeGuard guard(scope);
            BodyReader body(wire, req);
            Response res(wire, req);
            scope.res = &res;
            setTimeout(s, SO_RCVTIMEO, opt.bodyTimeoutMs);

            if (draining.load()) {
                res.setKeepAlive(false);
                try {
                    res.send(503, "application/json; charset=utf-8", json::errorBody("Server busy."));
                } catch (...) {
                }
                return wire.dead();
            }

            inflight.fetch_add(1);
            struct InflightGuard {
                std::atomic<int>& n;
                ~InflightGuard() { n.fetch_sub(1); }
            } ig{inflight};

            Ctx ctx(req, res, body, cfg, self);
            auto answerError = [&](int status, const std::string& text) {
                if (res.aborted()) return;
                if (res.headersSent()) {
                    res.abort();                 // a half-sent reply cannot become an error reply
                    return;
                }
                if (mustCloseAfter(body, scope)) res.setKeepAlive(false);
                detail::resetForError(res);
                try {
                    ctx.error(status, text);
                } catch (...) {
                    res.abort();
                }
            };
            try {
                router.dispatch(ctx);
                if (!res.finished()) {
                    if (res.headersSent()) {
                        res.end();
                    } else {
                        logRequestError(req, "handler returned without a reply");
                        answerError(500, "internal error");
                    }
                }
            } catch (const HttpError& e) {
                answerError(e.status, e.what());
            } catch (const TooLarge&) {
                answerError(413, "request entity too large");
            } catch (const IoError& e) {
                if (!e.peerGone && !e.timeout) logRequestError(req, e.what());
                if (e.peerGone || e.timeout || res.headersSent()) {
                    if (!res.aborted()) res.abort();
                } else {
                    answerError(500, "internal error");
                }
            } catch (const std::exception& e) {
                logRequestError(req, e.what());
                answerError(500, "internal error");
            } catch (...) {
                logRequestError(req, "unknown exception");
                answerError(500, "internal error");
            }

            if (wire.aborted() || wire.dead()) return true;
            bool keep = res.keepAlive() && res.finished() && !draining.load();
            if (keep && !body.finished()) {
                if (scope.bodyBroken) {
                    keep = false;
                } else {
                    try {
                        if (body.drain(opt.keepAliveDrainBytes) < 0) keep = false;
                    } catch (...) {
                        keep = false;
                    }
                }
            }
            if (scope.bodyBroken) keep = false;
            if (!keep) return wire.dead();
            first = false;
        }
    }

    bool mustCloseAfter(const BodyReader& body, const detail::RequestScope& scope) const
    {
        if (body.finished()) return false;
        if (scope.bodyBroken) return true;
        if (body.length() >= 0 && body.length() - body.consumed() > opt.keepAliveDrainBytes) return true;
        return false;
    }

    static void sendBare(SocketWire& w, int status)
    {
        std::string r = detail::bareReply(status);
        try {
            w.send(r.data(), r.size());
        } catch (...) {
        }
    }
};

Server::Server(Router& router, std::function<std::shared_ptr<const Config>()> config, Log* log, ServerOptions options)
    : d_(std::make_unique<Impl>(this, router, std::move(config), log, std::move(options)))
{
}

Server::~Server() { stop(); }

void Server::start(int port, bool bindAll, const std::string& bindAddress)
{
    Impl& d = *d_;
    std::lock_guard<std::mutex> lk(d.mu);
    if (d.running) throw std::logic_error("http server already running");
    d.draining = false;
    d.stopping = false;
    std::string addr = bindAll ? "0.0.0.0" : (bindAddress.empty() ? "127.0.0.1" : bindAddress);
    d.mainPort = d.listenOn(addr, port, false, "");   // throws BindError before anything else starts
    d.startCloser();
    d.running = true;
    d.log->state("http listening on " + addr + ":" + std::to_string(d.mainPort));
}

int Server::startProxyListener(int port)
{
    Impl& d = *d_;
    std::lock_guard<std::mutex> lk(d.mu);
    int p = d.listenOn("127.0.0.1", port, true, " (proxy)");
    d.startCloser();
    d.running = true;
    return p;
}

void Server::stop()
{
    Impl& d = *d_;
    std::vector<std::unique_ptr<Impl::Listener>> ls;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        if (!d.running && d.listeners.empty() && !d.closer.joinable()) return;
        d.draining = true;
    }
    // New requests get 503 while in-flight ones finish.
    int64_t until = anfs::monoMs() + std::max(d.opt.drainOnStopMs, 0);
    while (d.inflight.load() > 0 && anfs::monoMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.stopping = true;
        ls.swap(d.listeners);
        for (auto& l : ls) closesocket(l->s);   // accept() returns at once
        // Wake workers blocked in recv/send: cancel their socket I/O and shut the socket down. The worker itself
        // closes the socket (never here, so a handle value is never closed twice).
        for (Conn* c : d.live) {
            CancelIoEx(reinterpret_cast<HANDLE>(c->s), nullptr);
            shutdown(c->s, SD_BOTH);
        }
        for (Conn* c : d.queue) {
            closesocket(c->s);
            delete c;
        }
        d.queue.clear();
        d.cvWork.notify_all();
    }
    for (auto& l : ls)
        if (l->th.joinable()) l->th.join();
    {
        std::unique_lock<std::mutex> lk(d.mu);
        if (!d.cvDone.wait_for(lk, std::chrono::seconds(15), [&] { return d.threads == 0; }))
            d.log->error("http stop: " + std::to_string(d.threads) + " worker(s) still busy after 15 s");
    }
    {
        std::lock_guard<std::mutex> lk(d.cmu);
        d.closerStop = true;
        d.ccv.notify_all();
    }
    if (d.closer.joinable()) d.closer.join();
    {
        std::lock_guard<std::mutex> lk(d.mu);
        bool was = d.running;
        d.running = false;
        d.draining = false;
        d.stopping = false;
        if (was) d.log->state("http stopped (port " + std::to_string(d.mainPort) + ")");
    }
}

bool Server::running() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->running;
}

int Server::port() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->mainPort;
}

std::vector<std::string> Server::listening() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    std::vector<std::string> out;
    for (const auto& l : d_->listeners) out.push_back(l->label);
    return out;
}

int Server::inFlight() const { return d_->inflight.load(); }

int Server::threads() const
{
    std::lock_guard<std::mutex> lk(d_->mu);
    return d_->threads;
}

int64_t Server::requests() const { return d_->requests.load(); }
int64_t Server::rejectedBusy() const { return d_->rejected.load(); }
const ServerOptions& Server::options() const { return d_->opt; }

// ---- SendFile (ANFS-F4b) ----------------------------------------------------------------------------------------
//
// A port of what Express 4.21 res.sendFile / res.download do through send 0.19, range-parser 1.2.1 (combine: true),
// fresh 0.5.2, etag 1.8.1, content-disposition 0.5.4 and finalhandler 1.3 (the versions in node_modules next to the
// bridge sources). Checked live against those packages in test_anfs_core_http. Points where Node differs from what
// a reader might expect:
// - "bytes=5-1" is not ignored: range-parser drops the bad part and, with nothing left, says unsatisfiable (416).
//   Overlapping or adjacent parts are combined first, so "bytes=0-4,5-9" is one range (206).
// - If-None-Match / If-Modified-Since are checked before Range: a fresh conditional request gets 304 even with Range.
//   A stale If-Range turns the request into a full 200, also when the range itself was unsatisfiable.
// - 412 (If-Match / If-Unmodified-Since) and 416 go through Express's finalhandler: an HTML page with CSP / nosniff
//   headers, the file headers already set (ETag, Last-Modified ...) kept. We write the production page; Node in its
//   default "development" env would put the error's stack trace there (never relied on, and leaks paths).
// - The weak ETag and Last-Modified use Node's stat mtime: Math.round of the 100 ns file time in ms, not floor.

namespace {

// Node's fs.Stats mtime: libuv splits the FILETIME into sec / nsec (C division), Node computes
// sec * 1000 + nsec / 1e6 as a double and Date takes Math.round of it.
int64_t nodeMtimeMs(const FILETIME& ft)
{
    const int64_t t = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    const int64_t ticks = t - 116444736000000000LL;
    const int64_t sec = ticks / 10000000;
    const int64_t nsec = (ticks - sec * 10000000) * 100;
    const double ms = static_cast<double>(sec) * 1000.0 + static_cast<double>(nsec) / 1e6;
    return static_cast<int64_t>(std::floor(ms + 0.5));
}

std::string hexSigned(int64_t v)
{
    char b[24];
    std::snprintf(b, sizeof b, "%s%llx", v < 0 ? "-" : "", static_cast<unsigned long long>(v < 0 ? -v : v));
    return b;
}

// JS parseInt(s, 10) for range-parser: leading white space, an optional sign, decimal digits; nullopt = NaN.
std::optional<double> jsParseInt(std::string_view s)
{
    size_t i = 0;
    auto ws = [](unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D) || c == 0xA0; };   // header is latin1
    while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    size_t d = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
    if (i == d) return std::nullopt;
    double v = std::strtod(std::string(s.substr(d, i - d)).c_str(), nullptr);
    return neg ? -v : v;
}

// fresh / send parseTokenList: split on ',', spaces around tokens dropped.
std::vector<std::string> tokenList(std::string_view s)
{
    std::vector<std::string> out;
    size_t start = 0, end = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ') {
            if (start == end) start = end = i + 1;
        } else if (s[i] == ',') {
            out.emplace_back(s.substr(start, end - start));
            start = end = i + 1;
        } else {
            end = i + 1;
        }
    }
    out.emplace_back(s.substr(start, end - start));
    return out;
}

bool etagListMatches(std::string_view list, const std::string& etag)
{
    for (const std::string& m : tokenList(list))
        if (m == etag || m == "W/" + etag || "W/" + m == etag) return true;
    return false;
}

// ---- Date.parse subset ------------------------------------------------------------------------------------------

int64_t daysFromCivil(int64_t y, int m, int d)   // H. Hinnant; m 1..12, d may run past the month (V8 MakeDay)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + (d - 1);
}

// A wall-clock time read as local time (V8: no zone in the legacy format = local).
std::optional<int64_t> localToUtcMs(int64_t wallMs)
{
    const int64_t ms = ((wallMs % 1000) + 1000) % 1000;
    __time64_t secs = static_cast<__time64_t>((wallMs - ms) / 1000);
    struct tm t {};
    if (_gmtime64_s(&t, &secs) != 0) return std::nullopt;
    t.tm_isdst = -1;
    const __time64_t utc = _mktime64(&t);
    if (utc == -1) return std::nullopt;
    return static_cast<int64_t>(utc) * 1000 + ms;
}

// V8's legacy Date.parse for the forms HTTP clients send: IMF-fixdate, RFC 850 ("Sunday, 06-Nov-94 ..."), asctime,
// with GMT/UTC/UT/Z, US zone names or +hhmm offsets, parenthesised comments, words before the first number ignored.
// ES5 ISO strings go through json::parseIsoMs first, as V8 tries that grammar first. Numeric-only legacy dates
// ("10/14/2026") are not supported (nullopt), nor is a missing year (V8 would guess 2001).
std::optional<int64_t> jsDateParse(std::string_view s)
{
    if (!s.empty() && ((s[0] >= '0' && s[0] <= '9') || s[0] == '+' || s[0] == '-')) {
        int64_t v = json::parseIsoMs(s);
        if (v != -1) return v;
    }
    static const char* kMon[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    static const struct { const char* name; int minutes; } kZones[] = {
        {"ut", 0}, {"utc", 0}, {"gmt", 0}, {"z", 0}, {"est", -300}, {"edt", -240}, {"cst", -360}, {"cdt", -300},
        {"mst", -420}, {"mdt", -360}, {"pst", -480}, {"pdt", -420}};
    int mon = -1, hh = -1, mi = 0, ss = 0, msPart = 0, ampm = -1;
    bool tz = false, sawNumber = false, sawTime = false;
    int tzMin = 0;
    std::vector<int64_t> nums;
    size_t i = 0;
    auto digitsAt = [&](size_t& k, int64_t& v) {
        size_t b = k;
        v = 0;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9' && k - b < 12) v = v * 10 + (s[k++] - '0');
        return k - b;
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == ' ' || c == ',' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (c == '(') {
            int depth = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '(') ++depth;
                else if (s[i] == ')' && --depth == 0) { ++i; break; }
            }
            continue;
        }
        if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') {
            size_t b = i;
            while (i < s.size() && (s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z') ++i;
            std::string w(s.substr(b, i - b));
            for (char& ch : w) ch = static_cast<char>(ch | 0x20);
            bool known = false;
            if (w.size() >= 3)
                for (int m = 0; m < 12; ++m)
                    if (w.compare(0, 3, kMon[m]) == 0) {
                        if (mon >= 0) return std::nullopt;
                        mon = m + 1;
                        known = true;
                    }
            if (!known && (w == "am" || w == "pm")) { ampm = w == "pm"; known = true; }
            if (!known && w == "t") known = true;
            for (const auto& z : kZones)
                if (!known && w == z.name) { tz = true; tzMin = z.minutes; known = true; }
            if (!known && sawNumber) return std::nullopt;   // V8: unknown words count only before the first number
            continue;
        }
        if ((c == '+' || c == '-') && (sawTime || tz) && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            size_t k = i + 1;
            int64_t v = 0;
            size_t n = digitsAt(k, v);
            int off;
            if (k < s.size() && s[k] == ':' && n <= 2) {
                size_t k2 = k + 1;
                int64_t m2 = 0;
                if (digitsAt(k2, m2) != 2) return std::nullopt;
                off = static_cast<int>(v * 60 + m2);
                k = k2;
            } else if (n <= 2) {
                off = static_cast<int>(v * 60);
            } else if (n == 4) {
                off = static_cast<int>((v / 100) * 60 + v % 100);
            } else {
                return std::nullopt;
            }
            if (!tz) tzMin = 0;
            tz = true;
            tzMin += c == '-' ? -off : off;
            i = k;
            continue;
        }
        if (c >= '0' && c <= '9') {
            int64_t v = 0;
            size_t n = digitsAt(i, v);
            if (i < s.size() && s[i] >= '0' && s[i] <= '9') return std::nullopt;   // absurdly long number
            sawNumber = true;
            if (i < s.size() && s[i] == ':') {
                if (sawTime) return std::nullopt;
                sawTime = true;
                hh = static_cast<int>(v);
                ++i;
                int64_t m2 = 0, s2 = 0;
                if (digitsAt(i, m2) == 0) return std::nullopt;
                mi = static_cast<int>(m2);
                if (i < s.size() && s[i] == ':') {
                    ++i;
                    if (digitsAt(i, s2) == 0) return std::nullopt;
                    ss = static_cast<int>(s2);
                    if (i < s.size() && s[i] == '.') {
                        ++i;
                        size_t b = i;
                        int64_t f = 0;
                        size_t fn = digitsAt(i, f);
                        if (fn == 0) return std::nullopt;
                        std::string fs(s.substr(b, std::min<size_t>(fn, 3)));
                        while (fs.size() < 3) fs += '0';
                        msPart = std::atoi(fs.c_str());
                    }
                }
                continue;
            }
            (void)n;
            if (nums.size() >= 3) return std::nullopt;
            nums.push_back(v);
            continue;
        }
        if (c == '-' || c == '/' || c == '.') { ++i; continue; }
        return std::nullopt;
    }
    if (mon < 0 || nums.size() != 2) return std::nullopt;
    int64_t day, year;
    if (nums[0] >= 1 && nums[0] <= 31) { day = nums[0]; year = nums[1]; }
    else { year = nums[0]; day = nums[1]; }
    if (day < 1 || day > 31) return std::nullopt;
    if (year >= 0 && year <= 49) year += 2000;
    else if (year >= 50 && year <= 99) year += 1900;
    if (hh < 0) hh = 0;
    if (ampm >= 0) {
        if (hh < 1 || hh > 12) return std::nullopt;
        hh = hh % 12 + (ampm ? 12 : 0);
    }
    if (hh > 24 || mi > 59 || ss > 59 || (hh == 24 && (mi || ss || msPart))) return std::nullopt;
    const int64_t wall = ((daysFromCivil(year, mon, static_cast<int>(day)) * 24 + hh) * 60 + mi) * 60000 +
                         int64_t(ss) * 1000 + msPart;
    if (tz) return wall - int64_t(tzMin) * 60000;
    return localToUtcMs(wall);
}

// Express's error path for send's 412 / 416: finalhandler's page (production form), its headers on top of the ones
// the file headers already set.
void finalhandlerError(Ctx& ctx, int status, const std::string& contentRange = {})
{
    Headers& h = ctx.res.headers();
    h.remove("Content-Encoding");
    h.remove("Content-Language");
    h.remove("Content-Range");
    if (!contentRange.empty()) h.set("Content-Range", contentRange);
    h.set("Content-Security-Policy", "default-src 'none'");
    h.set("X-Content-Type-Options", "nosniff");
    // Node quirk, seen on the wire: this reply goes out through res.end(string) and its header block comes out as
    // the latin1 header bytes read as UTF-8 (a "filename=\"<U+00DC>...\"" byte DC becomes EF BF BD). Copied so the
    // bytes stay identical; only header values with bytes >= 0x80 (download names) are touched. HEAD ends without
    // the string, so its headers stay latin1.
    std::vector<std::pair<std::string, std::string>> recoded;
    for (const auto& f : h.fields())
        if (!ctx.res.isHead())
        for (unsigned char ch : f.second)
            if (ch >= 0x80) {
                recoded.emplace_back(f.first, fs::narrow(fs::widen(f.second)));
                break;
            }
    for (auto& r : recoded) h.set(r.first, std::move(r.second));
    const std::string doc = std::string("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                                        "<title>Error</title>\n</head>\n<body>\n<pre>") +
                            reasonPhrase(status) + "</pre>\n</body>\n</html>\n";
    ctx.res.send(status, "text/html; charset=utf-8", doc);
}

} // namespace

std::string weakEtag(int64_t size, int64_t mtimeMs) { return "W/\"" + hexSigned(size) + "-" + hexSigned(mtimeMs) + "\""; }

int64_t parseHttpDate(std::string_view s)
{
    std::optional<int64_t> v = jsDateParse(s);
    return v ? *v : -1;
}

RangeResult parseRange(std::string_view header, int64_t length, int64_t& start, int64_t& end)
{
    start = 0;
    end = length - 1;
    size_t i = 0;
    while (i < header.size() && header[i] == ' ') ++i;   // send: /^ *bytes=/
    if (header.substr(i, 6) != "bytes=") return RangeResult::Full;
    const std::string_view spec = header.substr(header.find('=') + 1);
    const double size = static_cast<double>(length);
    struct R { double s, e; };
    std::vector<R> rs;
    size_t p = 0;
    for (;;) {
        size_t q = spec.find(',', p);
        std::string_view part = spec.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p);
        // range-parser: arr[i].split('-'); [0] start, [1] end (undefined -> NaN)
        size_t dash = part.find('-');
        std::optional<double> s = jsParseInt(part.substr(0, dash));
        std::optional<double> e;
        if (dash != std::string_view::npos) {
            std::string_view tail = part.substr(dash + 1);
            e = jsParseInt(tail.substr(0, tail.find('-')));
        }
        if (!s) {                                // -nnn
            if (e) s = size - *e;
            e = size - 1;
        } else if (!e) {                         // nnn-
            e = size - 1;
        }
        if (*e > size - 1) e = size - 1;
        if (s && !(*s > *e) && !(*s < 0)) rs.push_back({*s, *e});
        if (q == std::string_view::npos) break;
        p = q + 1;
    }
    if (rs.empty()) return RangeResult::Unsatisfiable;
    // combineRanges: sorted by start (stable), overlapping or adjacent parts merged.
    std::stable_sort(rs.begin(), rs.end(), [](const R& a, const R& b) { return a.s < b.s; });
    size_t j = 0;
    for (size_t k = 1; k < rs.size(); ++k) {
        if (rs[k].s > rs[j].e + 1) rs[++j] = rs[k];
        else if (rs[k].e > rs[j].e) rs[j].e = rs[k].e;
    }
    if (j != 0) return RangeResult::Full;        // several ranges: Express sends the whole file
    start = static_cast<int64_t>(rs[0].s);
    end = static_cast<int64_t>(rs[0].e);
    return RangeResult::Partial;
}

std::string contentDisposition(std::string_view filename, std::string_view type)
{
    // format(): the type must be a token and is written lower-case.
    std::string out;
    if (type.empty()) throw std::invalid_argument("invalid type");
    for (char c : type) {
        if (!detail::isTokenChar(static_cast<unsigned char>(c))) throw std::invalid_argument("invalid type");
        out += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    // JS strings are UTF-16: every test below is per code unit (a pair outside the BMP gives two '?').
    const std::wstring name = fs::widen(nodepath::basename(filename));
    bool quoted = !name.empty(), hexEscape = false;
    std::wstring fallback = name;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t u = name[i];
        if (!((u >= 0x20 && u <= 0x7E) || (u >= 0x80 && u <= 0xFF))) quoted = false;   // TEXT_REGEXP
        if (!((u >= 0x20 && u <= 0x7E) || (u >= 0xA0 && u <= 0xFF))) fallback[i] = L'?';   // getlatin1
        auto hex = [](wchar_t x) { return (x >= '0' && x <= '9') || ((x | 0x20) >= 'a' && (x | 0x20) <= 'f'); };
        if (u == L'%' && i + 2 < name.size() && hex(name[i + 1]) && hex(name[i + 2])) hexEscape = true;
    }
    const bool hasFallback = fallback != name;
    if (quoted || hasFallback) {
        // qstring; Node writes header text as latin1, so each code unit (all <= 0xFF here) is one byte.
        out += "; filename=\"";
        for (wchar_t u : hasFallback ? fallback : name) {
            if (u == L'\\' || u == L'"') out += '\\';
            out += static_cast<char>(static_cast<unsigned char>(u));
        }
        out += '"';
    }
    if (hasFallback || !quoted || hexEscape) {
        // ustring: encodeURIComponent (a lone surrogate throws URIError there: Express answers 500), then
        // ' ( ) * also escaped.
        static const char* kHex = "0123456789ABCDEF";
        out += "; filename*=UTF-8''";
        for (size_t i = 0; i < name.size(); ++i) {
            uint32_t cp = name[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF)
                cp = 0x10000 + ((cp - 0xD800) << 10) + (name[++i] - 0xDC00);
            else if (cp >= 0xD800 && cp <= 0xDFFF)
                throw std::invalid_argument("URI malformed");
            if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '-' ||
                cp == '_' || cp == '.' || cp == '!' || cp == '~') {
                out += static_cast<char>(cp);
                continue;
            }
            unsigned char b[4];
            int n = 0;
            if (cp < 0x80) b[n++] = static_cast<unsigned char>(cp);
            else if (cp < 0x800) { b[n++] = 0xC0 | (cp >> 6); b[n++] = 0x80 | (cp & 0x3F); }
            else if (cp < 0x10000) { b[n++] = 0xE0 | (cp >> 12); b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F); }
            else {
                b[n++] = 0xF0 | (cp >> 18); b[n++] = 0x80 | ((cp >> 12) & 0x3F);
                b[n++] = 0x80 | ((cp >> 6) & 0x3F); b[n++] = 0x80 | (cp & 0x3F);
            }
            for (int k = 0; k < n; ++k) {
                out += '%';
                out += kHex[b[k] >> 4];
                out += kHex[b[k] & 15];
            }
        }
    }
    return out;
}

bool sendFile(Ctx& ctx, const std::string& path, const SendFileOptions& o)
{
    Response& res = ctx.res;
    if (res.headersSent()) throw std::logic_error("Can't set headers after they are sent.");
    // One handle for the metadata and the body, so size, ETag and bytes agree even while the file changes.
    const fs::FileStat st = fs::stat(path);
    if (!st.exists || st.isDir) return false;
    fs::FileReader file;
    if (!file.open(path)) {
        const fs::FileStat again = fs::stat(path);
        if (!again.exists || again.isDir) return false;
        finalhandlerError(ctx, 500);           // send's onStatError for EBUSY / EPERM: Express's 500 page
        return true;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(static_cast<HANDLE>(file.nativeHandle()), &info)) {
        finalhandlerError(ctx, 500);
        return true;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    const int64_t size = static_cast<int64_t>((static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    const int64_t mtimeMs = nodeMtimeMs(info.ftLastWriteTime);

    // send.setHeader (after Express's 'headers' event, which res.download uses for Content-Disposition).
    Headers& h = res.headers();
    if (!o.downloadName.empty()) h.set("Content-Disposition", contentDisposition(o.downloadName, o.dispositionType));
    if (!h.has("Accept-Ranges")) h.set("Accept-Ranges", "bytes");
    if (!o.cacheControl.empty() && !h.has("Cache-Control")) h.set("Cache-Control", o.cacheControl);
    if (o.lastModified && !h.has("Last-Modified")) h.set("Last-Modified", httpDate(mtimeMs));
    if (o.etag && !h.has("ETag")) h.set("ETag", weakEtag(size, mtimeMs));
    if (!h.has("Content-Type"))
        h.set("Content-Type", o.contentType.empty() ? mime::contentTypeForFile(path) : o.contentType);

    // Node joins repeated list headers with ", " and keeps the first of a repeated date header.
    const Headers& rq = ctx.req.headers;
    const std::string ifMatch = rq.joined("If-Match"), ifNoneMatch = rq.joined("If-None-Match");
    const std::string ifUnmod = rq.get("If-Unmodified-Since"), ifMod = rq.get("If-Modified-Since");
    const std::string etag = h.get("ETag"), lastMod = h.get("Last-Modified");
    if (!ifMatch.empty() || !ifUnmod.empty() || !ifNoneMatch.empty() || !ifMod.empty()) {
        bool failed = false;                   // isPreconditionFailure
        if (!ifMatch.empty()) {
            failed = etag.empty() || (ifMatch != "*" && !etagListMatches(ifMatch, etag));
        } else if (std::optional<int64_t> since = jsDateParse(ifUnmod); since && !ifUnmod.empty()) {
            std::optional<int64_t> lm = jsDateParse(lastMod);
            failed = !lm || *lm > *since;
        }
        if (failed) {
            finalhandlerError(ctx, 412);
            return true;
        }
        // fresh(): the response status is still 200 here, so it is always cachable. Only If-None-Match and
        // If-Modified-Since make a request conditional for it (If-Match alone never gives 304).
        bool fresh = !ifNoneMatch.empty() || !ifMod.empty();
        const std::string cc = rq.joined("Cache-Control");
        for (size_t at = cc.find("no-cache"); fresh && at != std::string::npos; at = cc.find("no-cache", at + 1)) {
            // /(?:^|,)\s*?no-cache\s*?(?:,|$)/
            size_t b = at, e = at + 8;
            while (b > 0 && (cc[b - 1] == ' ' || cc[b - 1] == '\t')) --b;
            while (e < cc.size() && (cc[e] == ' ' || cc[e] == '\t')) ++e;
            if ((b == 0 || cc[b - 1] == ',') && (e == cc.size() || cc[e] == ',')) fresh = false;
        }
        if (fresh && !ifNoneMatch.empty() && ifNoneMatch != "*")
            fresh = !etag.empty() && etagListMatches(ifNoneMatch, etag);
        if (fresh && !ifMod.empty()) {
            std::optional<int64_t> lm = jsDateParse(lastMod), since = jsDateParse(ifMod);
            fresh = !lastMod.empty() && lm && since && *lm <= *since;
        }
        if (fresh) {
            for (const char* n : {"Content-Encoding", "Content-Language", "Content-Length", "Content-Range",
                                  "Content-Type"})
                h.remove(n);
            res.sendEmpty(304);
            return true;
        }
    }

    int status = 200;
    int64_t offset = 0, len = size;
    const std::string range = rq.joined("Range");
    size_t sp = 0;
    while (sp < range.size() && range[sp] == ' ') ++sp;
    if (range.compare(sp, 6, "bytes=") == 0) {
        int64_t a = 0, b = 0;
        RangeResult rr = parseRange(range, size, a, b);
        // isRangeFresh
        const std::string ifRange = rq.joined("If-Range");
        bool rangeFresh = true;
        if (!ifRange.empty()) {
            if (ifRange.find('"') != std::string::npos) {
                rangeFresh = !etag.empty() && ifRange.find(etag) != std::string::npos;
            } else {
                std::optional<int64_t> lm = jsDateParse(lastMod), ir = jsDateParse(ifRange);
                rangeFresh = lm && ir && *lm <= *ir;
            }
        }
        if (!rangeFresh) rr = RangeResult::Full;
        if (rr == RangeResult::Unsatisfiable) {
            finalhandlerError(ctx, 416, "bytes */" + std::to_string(size));
            return true;
        }
        if (rr == RangeResult::Partial) {
            status = 206;
            h.set("Content-Range", "bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string(size));
            offset = a;
            len = b - a + 1;
        }
    }
    res.begin(status, len);
    if (!res.isHead() && len > 0) res.writeFile(file.nativeHandle(), offset, len);
    res.end();
    return true;
}

} // namespace anfs::http
