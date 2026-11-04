// tests/anfs/anfs_testkit.h - shared Qt Test helpers for the AN-FS module tests (ANFS-F8, anfs-cpp.md section 7,
// layer 1). Linked by every test_anfs_*.exe through the anfs_testkit static library (tools/anfs/CMakeLists.txt).
//
//   WsaInit       WSAStartup/WSACleanup for one scope (socket users count their own references).
//   freePort()    a free loopback port (bound and released); PortHolder keeps one bound so a server bind fails 10048.
//   TempDir       a temp data folder with native separators and sub-folder helpers; removed on destruction.
//   TestServer    Router + http::Server on an ephemeral loopback port with a Config snapshot the test controls:
//                 start(), url(path), port(). No modules: the test maps the routes it wants.
//   ModuleHost    the in-process runtime of test_anfs_core_runtime (temp data dir, Log, ConfigStore + listener,
//                 a private ModuleRegistry, StatusWriter, ModeManager): add the module under test plus its declared
//                 dependencies, write() a config (mode peer/server on an ephemeral port) and the ModeManager binds
//                 the server and starts them in order. fetch() answers over the WinHTTP client.
//   RawClient     a blocking loopback socket client with its own read buffer: byte-exact requests, raw response
//                 bytes (status line, header order, chunk framing), pipelining, half-close and RST.
//   RawResponse   RawClient::readResponse(): status, reason, headers in wire order, body (de-chunked) and the raw
//                 bytes as received.
//   FakePeer      a raw socket server with canned responses per method+path (status, headers, body), per-response
//                 delays and aborts (RST before/after the head), captured requests. Stands in for a Node peer or
//                 bridge where a module talks outbound (relay, register, poll).
//   MemProbe      peak working set / private bytes deltas (GetProcessMemoryInfo) for the "flat memory" assertions.
//
// Nothing here binds a fixed port or contacts anything outside 127.0.0.1. ManualClock + ScopedClock (core/clock.h)
// drive every protocol timer in tests.
#pragma once

#include "core/config.h"
#include "core/http_client.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/json.h"
#include "core/log.h"
#include "core/module.h"
#include "core/status.h"

#include <QtCore/QString>
#include <QtCore/QTemporaryDir>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace anfs::testkit {

// ---- sockets -------------------------------------------------------------------------------------------------------

struct WsaInit {
    WsaInit();
    ~WsaInit();
    WsaInit(const WsaInit&) = delete;
    WsaInit& operator=(const WsaInit&) = delete;
};

// A free loopback port (bound with port 0 and released; the usual small race is acceptable for a test).
int freePort();

// Holds a loopback port bound and listening (SO_EXCLUSIVEADDRUSE) so another bind on it fails with 10048.
class PortHolder {
public:
    explicit PortHolder(int port);
    ~PortHolder();
    PortHolder(const PortHolder&) = delete;
    PortHolder& operator=(const PortHolder&) = delete;
    int port() const { return port_; }            // 0 when the bind failed
    void release();

private:
    uintptr_t s_;
    int port_ = 0;
};

// ---- files ---------------------------------------------------------------------------------------------------------

class TempDir {
public:
    explicit TempDir(const QString& prefix = QStringLiteral("anfs-test"));
    bool valid() const { return dir_.isValid(); }
    const std::string& path() const { return path_; }       // native separators, no trailing backslash
    std::string sub(std::string_view name) const;            // path()\name (not created)
    std::string mkdir(std::string_view name) const;          // created; returns the path
    std::string file(std::string_view name) const { return sub(name); }
    // Writes a file (creating parent folders); returns the path, "" on failure.
    std::string write(std::string_view name, std::string_view data) const;
    std::string read(std::string_view name) const;           // "" when missing

private:
    QTemporaryDir dir_;
    std::string path_;
};

// ---- an in-process server with no modules -------------------------------------------------------------------------

class TestServer {
public:
    explicit TestServer(http::ServerOptions options = {});
    ~TestServer();
    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    void setConfig(Config c);                     // snapshot handed to every later request
    std::shared_ptr<const Config> config() const;
    void start();                                 // ephemeral loopback port; throws http::BindError
    void stop();
    int port() const { return server.port(); }
    std::string url(std::string_view path) const; // "http://127.0.0.1:<port><path>"
    // Convenience over the WinHTTP client (HttpClient::shared()).
    http::HttpClient::Result fetch(std::string_view method, std::string_view path, const http::Headers& headers = {},
                                   std::string_view body = {}, int idleMs = 5000);

    http::Router router;
    http::Server server;

private:
    mutable std::mutex mu_;
    std::shared_ptr<const Config> config_;
};

// ---- the runtime fixture for module tests ------------------------------------------------------------------------

class ModuleHost {
public:
    ModuleHost();
    ~ModuleHost();                                // shutdown() while the status writer lives, then Svc::clear()
    ModuleHost(const ModuleHost&) = delete;
    ModuleHost& operator=(const ModuleHost&) = delete;

    // Registers a module class (default-constructible) before init(): modes "common", "peer", "server" or a list.
    template <class M> void add(std::string_view modes, int order = 100)
    {
        registry.add(modes, order, [] { return std::unique_ptr<Module>(new M()); }, typeid(M).name());
    }
    void add(std::string_view modes, int order, ModuleFactory factory, const char* className = "TestModule");
    // Creates the ModeManager and subscribes it to the store. Call once after add().
    void init();
    // Rewrites anfs.json like an external writer and reloads the store (the ConfigWatcher's job in `anfs run`):
    // the listener applies it. Returns false when the write or load failed.
    bool write(const Config& c);
    // Applies the config directly (no file). Needs init().
    void apply(const Config& c);
    // A test-mode loopback config. port 0 picks a free loopback port at call time (a config port of 0 would mean
    // the mode default 18899/18080, which a parallel test run may hold); mode off keeps 0.
    static Config cfg(const char* mode, int port = 0, int64_t rev = 1);
    int port() const;                             // bound port of the running generation, 0 in mode off
    std::string url(std::string_view path) const;
    http::HttpClient::Result fetch(std::string_view path, const http::Headers& headers = {},
                                   std::string_view method = "GET", std::string_view body = {}, int idleMs = 5000);
    JVal statusDoc() const;                       // buildStatus(...) as the writer would write it

    TempDir tmp;
    std::string dataDir;
    Log log;
    ConfigStore store;
    ModuleRegistry registry;
    std::unique_ptr<ModeManager> mm;
    StatusWriter status;
    int64_t startedAt = 0;
    int subscription = -1;
    http::HttpClient client;
};

// ---- raw socket client ---------------------------------------------------------------------------------------------

struct RawResponse {
    int status = 0;
    std::string reason;
    std::string version;                          // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;   // wire order, names as sent
    std::string body;                             // de-chunked
    std::string raw;                              // every byte consumed for this response, framing included
    std::vector<std::string> headerNames() const;
    std::string header(std::string_view name, std::string_view def = {}) const;   // first, case-insensitive
    bool has(std::string_view name) const;
    bool chunked() const;
    bool complete = false;                        // false when EOF/timeout cut the response short
};

class RawClient {
public:
    RawClient() = default;
    explicit RawClient(int port, int timeoutMs = 5000) { connectTo(port, timeoutMs); }
    ~RawClient();
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;

    bool connectTo(int port, int timeoutMs = 5000);
    bool connected() const;
    void setTimeout(int ms);                      // SO_RCVTIMEO
    bool send(std::string_view bytes);
    // "METHOD path HTTP/1.1\r\nHost: 127.0.0.1\r\n<headers>\r\n<body>"; Content-Length added when body is given
    // and no header names it. Returns the bytes sent.
    std::string request(std::string_view method, std::string_view path,
                        const std::vector<std::pair<std::string, std::string>>& headers = {},
                        std::string_view body = {}, std::string_view version = "HTTP/1.1");
    void close();
    void reset();                                 // RST (SO_LINGER 0)
    void shutdownSend();                          // half-close

    bool fill();                                  // one more recv; false on EOF, error or timeout
    bool eof() const { return eof_; }
    bool need(size_t n);
    std::string take(size_t n);
    std::string readN(size_t n);                  // exactly n bytes, or what arrived before EOF/timeout
    std::string readUntil(std::string_view delim);
    std::string readToClose();
    std::string readHead();                       // up to and including "\r\n\r\n"
    // A whole response: head, then Content-Length / chunked body (or to close when neither and the server
    // closes). A HEAD request's response has no body: pass headOnly.
    RawResponse readResponse(bool headOnly = false);
    const std::string& buffered() const { return buf_; }

private:
    uintptr_t s_ = ~uintptr_t(0);
    std::string buf_;
    bool eof_ = false;
};

// Parses a head block ("HTTP/1.1 200 OK\r\nA: b\r\n\r\n") into status/reason/version/headers. False when malformed.
bool parseHead(std::string_view head, RawResponse& out);

// ---- fake peer / bridge --------------------------------------------------------------------------------------------

struct FakeRequest {
    std::string method;
    std::string target;                           // "/api/browse?path=..."
    std::string path;                             // target without the query
    std::string version;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;                             // de-chunked
    std::string raw;                              // as framed on the wire
    std::string header(std::string_view name, std::string_view def = {}) const;
};

struct FakeResponse {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::vector<std::pair<std::string, std::string>> headers;   // extra, after Content-Type
    std::string body;
    bool chunked = false;                         // chunked framing of body (one chunk per chunkSize bytes)
    size_t chunkSize = 0;                         // 0 = one chunk
    int delayMs = 0;                              // before the head
    int bodyDelayMs = 0;                          // between the head and the body
    bool abortBeforeHead = false;                 // RST without an answer
    bool abortAfterHead = false;                  // head sent, then RST (body never arrives)
    bool close = false;                           // "Connection: close" and close after the body
    bool hold = false;                            // never answers; the connection stays open until the client leaves
    static FakeResponse json(int status, std::string_view body);
    static FakeResponse text(int status, std::string_view body);
};

class FakePeer {
public:
    using Handler = std::function<FakeResponse(const FakeRequest&)>;
    FakePeer();                                   // listens on an ephemeral loopback port at once
    ~FakePeer();
    FakePeer(const FakePeer&) = delete;
    FakePeer& operator=(const FakePeer&) = delete;

    int port() const { return port_; }
    std::string url(std::string_view path) const;
    std::string origin() const;                   // "http://127.0.0.1:<port>"

    // Canned answers: method "*" or exact; path compared without the query (exact), or a prefix when it ends
    // with '*'. First registered wins. A request nothing matches gets 404 {"error":"Not found"}.
    void on(std::string method, std::string path, FakeResponse r);
    void onGet(std::string path, FakeResponse r) { on("GET", std::move(path), std::move(r)); }
    void onPost(std::string path, FakeResponse r) { on("POST", std::move(path), std::move(r)); }
    // A dynamic handler consulted before the canned table (return a FakeResponse with status 0 to fall through).
    void setHandler(Handler h);
    void clearRoutes();
    void setDefaultDelayMs(int ms);               // applied to every answer without its own delay

    std::vector<FakeRequest> requests() const;    // in arrival order
    size_t requestCount() const;
    int accepts() const { return accepts_.load(); }
    void clearRequests();
    // Waits (real time) until n requests arrived; false on timeout.
    bool waitForRequests(size_t n, int timeoutMs = 5000) const;
    void stop();                                  // idempotent; closes every connection

private:
    struct Canned {
        std::string method;
        std::string path;
        FakeResponse r;
    };
    struct Conn;
    void serve(uintptr_t s);
    FakeResponse answer(const FakeRequest& q) const;

    uintptr_t ls_ = ~uintptr_t(0);
    int port_ = 0;
    std::thread acceptThread_;
    mutable std::mutex mu_;
    std::vector<Canned> canned_;
    Handler handler_;
    int defaultDelayMs_ = 0;
    std::vector<FakeRequest> requests_;
    std::vector<uintptr_t> socks_;
    std::vector<std::thread> workers_;
    std::atomic<int> accepts_{0};
    std::atomic<bool> stop_{false};
};

// ---- memory probe --------------------------------------------------------------------------------------------------

struct MemSample {
    int64_t workingSet = 0;                       // bytes
    int64_t peakWorkingSet = 0;
    int64_t privateBytes = 0;                     // PagefileUsage
    int64_t peakPrivateBytes = 0;
};
MemSample memSample();                            // GetProcessMemoryInfo for this process

class MemProbe {
public:
    MemProbe() : start_(memSample()) {}
    void restart() { start_ = memSample(); }
    const MemSample& start() const { return start_; }
    MemSample now() const { return memSample(); }
    int64_t workingSetDelta() const { return now().workingSet - start_.workingSet; }
    int64_t peakWorkingSetDelta() const { return now().peakWorkingSet - start_.peakWorkingSet; }
    int64_t privateDelta() const { return now().privateBytes - start_.privateBytes; }
    // The "flat memory" assertion: peak private bytes grew by at most limit since the probe started.
    bool flat(int64_t limitBytes) const { return now().peakPrivateBytes - start_.peakPrivateBytes <= limitBytes; }
    std::string describe() const;                 // "ws +1.2 MB, peak ws +3.4 MB, private +0.5 MB"

private:
    MemSample start_;
};

// ---- misc ---------------------------------------------------------------------------------------------------------

// Waits in real time (5 ms slices) until pred() is true; false on timeout.
bool waitUntil(const std::function<bool()>& pred, int timeoutMs = 5000);
// Keys of an ordered object, in order.
std::vector<std::string> keysOf(const JVal& o);
// Path of the anfs.exe this test tree built: ANFS_EXE_PATH is defined per test executable (not for the testkit
// library), so this is inline and evaluated in the test's own translation unit; "" when the define is absent.
inline std::string anfsExePath()
{
#ifdef ANFS_EXE_PATH
    return ANFS_EXE_PATH;
#else
    return {};
#endif
}
// pwsh.exe when installed ("C:\Program Files\PowerShell\7\pwsh.exe" or on PATH), "" otherwise.
std::string pwshPath();

} // namespace anfs::testkit

// QCOMPARE on std::string / string_view / const char* with a readable diff.
#define ANFS_SEQ(a, b) QCOMPARE(QString::fromUtf8(std::string(a).c_str()), QString::fromUtf8(std::string(b).c_str()))
