// tools/anfs/test/sim/sim_main.cpp - anfs_sim.exe (ANFS-F8 skeleton; anfs-cpp.md section 7, layer 2).
//
//   anfs_sim bridge --port 18081 [--park-ms 2000] [--capture DIR] [--token-file FILE]        NodeBridgeSim
//   anfs_sim peer   --bridge http://127.0.0.1:18080 --port 18897 [--relay-only] [--root DIR]
//                   [--interval-ms 5000] [--token-file FILE]                                   NodePeerSim
//   anfs_sim record --target http://127.0.0.1:18180 --out DIR [--path /api/x ...] [--token-file FILE]
//   anfs_sim version
//
// This slice gives the process shape, the CLI, the guards and the minimal wire so the harness can start it:
// - bridge: POST /api/agents/register answers {"ok":true,"id":<md5Hex12(name_ip)>} and records the body;
//   GET /api/agents/poll parks --park-ms then answers 204; POST /api/relay/body and /api/relay/response are
//   captured into --capture DIR; GET /api/health answers {"ok":true,"sim":"bridge"}.
// - peer: binds --port, answers GET /api/health, and registers with the bridge every --interval-ms with the
//   agent.js field names (agentId, name with the "-sim" suffix, ip, port, relayOnly). The mini peer API (browse,
//   drives, download with Range, preview, upload echo), the 3 pollers and the watchdog report are W1 peer-link /
//   srv-relay work (anfs-cpp.md 7, "NodePeerSim").
// - record: raw GETs against the C# reference only; writes <out>\<n>.head and .body with the exact bytes.
// Guards (C7): every port is refused when it is 8080, 8899, 443, 7700 or 7701, every URL when its host is not
// loopback (so 192.168.1.177 and example.invalid can never be reached). The token comes from --token-file (or
// ANFS_COMPAT_TOKEN_FILE) and is never printed. Exit codes: 0 ok, 2 usage/guard, 1 failure.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "core/clock.h"
#include "core/config.h"
#include "core/http_client.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/net.h"
#include "core/version.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace anfs::sim {
// raw_http.cpp
struct RawResult {
    bool ok = false;
    int status = 0;
    std::string head;
    std::string body;
    std::string error;
};
bool refusedPort(int port);
bool refusedHost(std::string_view host);
std::string refuseUrl(std::string_view url);
RawResult rawGet(const std::string& host, int port, const std::string& pathAndQuery,
                 const std::vector<std::pair<std::string, std::string>>& headers, int timeoutMs);
} // namespace anfs::sim

using namespace anfs;

namespace {

struct Args {
    std::string command;
    int port = 0;
    int parkMs = 2000;
    int intervalMs = 5000;
    bool relayOnly = false;
    std::string bridge;
    std::string target;
    std::string root;
    std::string out;
    std::string capture;
    std::string tokenFile;
    std::vector<std::string> paths;
    bool bad = false;
    std::string badText;
};

void usage(FILE* out)
{
    std::fprintf(out,
                 "usage: anfs_sim bridge --port N [--park-ms MS] [--capture DIR] [--token-file FILE]\n"
                 "       anfs_sim peer --bridge URL --port N [--relay-only] [--root DIR] [--interval-ms MS] [--token-file FILE]\n"
                 "       anfs_sim record --target URL --out DIR [--path P ...] [--token-file FILE]\n"
                 "       anfs_sim version\n");
}

Args parse(int argc, char** argv)
{
    Args a;
    if (argc < 2) {
        a.bad = true;
        return a;
    }
    a.command = argv[1];
    auto value = [&](int& i, std::string& dst) {
        if (i + 1 >= argc) {
            a.bad = true;
            a.badText = std::string(argv[i]) + " needs a value";
            return;
        }
        dst = argv[++i];
    };
    auto number = [&](int& i, int& dst) {
        std::string s;
        value(i, s);
        if (!a.bad) dst = std::atoi(s.c_str());
    };
    for (int i = 2; i < argc && !a.bad; ++i) {
        std::string s = argv[i];
        if (s == "--port") number(i, a.port);
        else if (s == "--park-ms") number(i, a.parkMs);
        else if (s == "--interval-ms") number(i, a.intervalMs);
        else if (s == "--relay-only") a.relayOnly = true;
        else if (s == "--bridge") value(i, a.bridge);
        else if (s == "--target") value(i, a.target);
        else if (s == "--root") value(i, a.root);
        else if (s == "--out") value(i, a.out);
        else if (s == "--capture") value(i, a.capture);
        else if (s == "--token-file") value(i, a.tokenFile);
        else if (s == "--path") {
            std::string p;
            value(i, p);
            a.paths.push_back(p);
        } else {
            a.bad = true;
            a.badText = "unknown option " + s;
        }
    }
    return a;
}

// The token: first non-empty line of --token-file (or ANFS_COMPAT_TOKEN_FILE). "" when none. Never printed.
std::string readToken(const Args& a)
{
    std::string file = a.tokenFile;
    if (file.empty()) {
        char buf[1024];
        DWORD n = GetEnvironmentVariableA("ANFS_COMPAT_TOKEN_FILE", buf, sizeof buf);
        if (n > 0 && n < sizeof buf) file.assign(buf, n);
    }
    if (file.empty()) return {};
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos && b < line.size()) return line.substr(b);
    }
    return {};
}

std::atomic<bool> g_stop{false};
std::mutex g_stopMu;
std::condition_variable g_stopCv;

BOOL WINAPI onConsole(DWORD)
{
    g_stop.store(true);
    g_stopCv.notify_all();
    return TRUE;
}

void waitForStop()
{
    std::unique_lock<std::mutex> lk(g_stopMu);
    g_stopCv.wait(lk, [] { return g_stop.load(); });
}

bool writeFile(const std::string& path, std::string_view data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::string portGuard(int port)
{
    if (port <= 0 || port > 65535) return "a port between 1 and 65535 is required";
    if (sim::refusedPort(port)) return "refused port " + std::to_string(port) + " (live stack: 8080/8899/443/7700/7701)";
    return {};
}

std::shared_ptr<const Config> simConfig()
{
    static std::shared_ptr<const Config> c = [] {
        auto cfg = std::make_shared<Config>();
        cfg->test = true;
        cfg->bindAll = false;
        return std::shared_ptr<const Config>(cfg);
    }();
    return c;
}

// ---- bridge ---------------------------------------------------------------------------------------------------------

int runBridge(const Args& a)
{
    if (std::string e = portGuard(a.port); !e.empty()) {
        std::fprintf(stderr, "anfs_sim bridge: %s\n", e.c_str());
        return 2;
    }
    std::string token = readToken(a);
    std::string capture = a.capture;
    if (!capture.empty()) CreateDirectoryA(capture.c_str(), nullptr);
    std::atomic<int> captured{0};
    int parkMs = a.parkMs;

    http::Router router;
    router.get("/api/health", [](http::Ctx& c) {
        c.json(200, JVal::object({{"ok", true}, {"sim", "bridge"}, {"version", kVersion}}));
    });
    router.post("/api/agents/register", [&](http::Ctx& c) {
        JVal body = c.readJson();
        if (!token.empty() && c.header("x-agent-auth") != token && c.queryOr("auth") != token) {
            c.error(401, "Unauthorized");
            return;
        }
        std::string name = body.get("name").asString();
        std::string ip = body.get("ip").asString();
        std::string id = body.get("agentId").asString();
        if (id.empty()) id = ids::md5Hex12(name + "_" + ip);
        if (!capture.empty()) writeFile(capture + "\\register-" + std::to_string(++captured) + ".json", json::stringify(body));
        c.json(200, JVal::object({{"ok", true}, {"id", id}}));
    });
    router.get("/api/agents/poll", [&](http::Ctx& c) {
        // Parks like the Node bridge's long poll, then 204 (no job). Injected jobs: W1 peer-link.
        std::unique_lock<std::mutex> lk(g_stopMu);
        anfs::clock().waitFor(g_stopCv, lk, parkMs, [] { return g_stop.load(); });
        c.res.sendEmpty(204);
    });
    auto captureBody = [&](const char* kind) {
        return [&, kind](http::Ctx& c) {
            std::string body = c.body.readAll(64LL << 20);
            if (!capture.empty())
                writeFile(capture + "\\" + kind + "-" + std::to_string(++captured) + ".bin", body);
            c.json(200, JVal::object({{"ok", true}, {"bytes", static_cast<long long>(body.size())}}));
        };
    };
    router.post("/api/relay/body", captureBody("relay-body"));
    router.post("/api/relay/response", captureBody("relay-response"));
    router.post("/api/watchdog/report", captureBody("watchdog"));

    http::Server server(router, simConfig, nullptr, http::ServerOptions{});
    try {
        server.start(a.port, false, "127.0.0.1");
    } catch (const http::BindError& e) {
        std::fprintf(stderr, "anfs_sim bridge: bind 127.0.0.1:%d failed: %s (wsa %d)\n", a.port, e.what(), e.wsaError);
        return 1;
    }
    std::printf("anfs_sim bridge listening on 127.0.0.1:%d park=%dms capture=%s pid=%lu\n", server.port(), parkMs,
                capture.empty() ? "(none)" : capture.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));
    std::fflush(stdout);
    waitForStop();
    server.stop();
    return 0;
}

// ---- peer -----------------------------------------------------------------------------------------------------------

int runPeer(const Args& a)
{
    if (std::string e = portGuard(a.port); !e.empty()) {
        std::fprintf(stderr, "anfs_sim peer: %s\n", e.c_str());
        return 2;
    }
    if (a.bridge.empty()) {
        std::fprintf(stderr, "anfs_sim peer: --bridge URL is required\n");
        return 2;
    }
    if (std::string e = sim::refuseUrl(a.bridge); !e.empty()) {
        std::fprintf(stderr, "anfs_sim peer: %s\n", e.c_str());
        return 2;
    }
    std::string token = readToken(a);
    std::string name = net::hostName() + "-sim";
    std::string ip = "127.0.0.1";
    std::string agentId = ids::md5Hex12(name + "_" + ip);

    http::Router router;
    router.get("/api/health", [&](http::Ctx& c) {
        c.json(200, JVal::object({{"ok", true}, {"sim", "peer"}, {"agentId", agentId}, {"relayOnly", a.relayOnly}}));
    });
    http::Server server(router, simConfig, nullptr, http::ServerOptions{});
    try {
        server.start(a.port, false, "127.0.0.1");
    } catch (const http::BindError& e) {
        std::fprintf(stderr, "anfs_sim peer: bind 127.0.0.1:%d failed: %s (wsa %d)\n", a.port, e.what(), e.wsaError);
        return 1;
    }
    std::printf("anfs_sim peer listening on 127.0.0.1:%d bridge=%s agentId=%s pid=%lu\n", server.port(),
                a.bridge.c_str(), agentId.c_str(), static_cast<unsigned long>(GetCurrentProcessId()));
    std::fflush(stdout);

    // Register loop (agent.js field names); the pollers and the relay job executor come with W1 peer-link.
    std::thread reg([&] {
        http::HttpClient client;
        std::string url = a.bridge;
        while (!url.empty() && url.back() == '/') url.pop_back();
        url += "/api/agents/register";
        for (int n = 0; !g_stop.load(); ++n) {
            JVal body = JVal::object({{"agentId", agentId}, {"name", name}, {"ip", ip}, {"port", server.port()},
                                      {"relayOnly", a.relayOnly}, {"version", kVersion}});
            http::Headers h;
            h.add("Content-Type", "application/json");
            if (!token.empty()) h.add("x-agent-auth", token);
            auto r = client.fetch("POST", url, h, json::stringify(body), 5000);
            if (n == 0 || r.status == 0) {
                std::printf("register -> %d%s\n", r.status, r.error.empty() ? "" : (" " + r.error).c_str());
                std::fflush(stdout);
            }
            std::unique_lock<std::mutex> lk(g_stopMu);
            g_stopCv.wait_for(lk, std::chrono::milliseconds(a.intervalMs), [] { return g_stop.load(); });
        }
    });
    waitForStop();
    reg.join();
    server.stop();
    return 0;
}

// ---- record ---------------------------------------------------------------------------------------------------------

int runRecord(const Args& a)
{
    if (a.target.empty() || a.out.empty()) {
        std::fprintf(stderr, "anfs_sim record: --target URL and --out DIR are required\n");
        return 2;
    }
    if (std::string e = sim::refuseUrl(a.target); !e.empty()) {
        std::fprintf(stderr, "anfs_sim record: %s\n", e.c_str());
        return 2;
    }
    auto u = net::parseUrl(a.target);
    std::string token = readToken(a);
    CreateDirectoryA(a.out.c_str(), nullptr);
    std::vector<std::string> paths = a.paths;
    if (paths.empty()) paths = {"/api/health", "/api/nodes"};
    int failures = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!token.empty()) headers.emplace_back("x-agent-auth", token);
        sim::RawResult r = sim::rawGet(u->host, u->port, paths[i], headers, 10000);
        std::string base = a.out + "\\" + std::to_string(i + 1);
        if (!r.ok) {
            std::printf("%s -> %s\n", paths[i].c_str(), r.error.c_str());
            ++failures;
            continue;
        }
        writeFile(base + ".head", r.head);
        writeFile(base + ".body", r.body);
        writeFile(base + ".path", paths[i]);
        std::printf("%s -> %d (%zu body bytes)\n", paths[i].c_str(), r.status, r.body.size());
    }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    Args a = parse(argc, argv);
    if (a.bad) {
        if (!a.badText.empty()) std::fprintf(stderr, "anfs_sim: %s\n", a.badText.c_str());
        usage(stderr);
        return 2;
    }
    if (a.command == "version") {
        std::printf("anfs_sim %s\n", kVersion);
        return 0;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleCtrlHandler(onConsole, TRUE);
    int rc;
    if (a.command == "bridge") rc = runBridge(a);
    else if (a.command == "peer") rc = runPeer(a);
    else if (a.command == "record") rc = runRecord(a);
    else {
        std::fprintf(stderr, "anfs_sim: unknown command %s\n", a.command.c_str());
        usage(stderr);
        rc = 2;
    }
    WSACleanup();
    return rc;
}
