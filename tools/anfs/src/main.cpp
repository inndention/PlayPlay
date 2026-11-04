// anfs.exe entry point: the CLI (ANFS-F7). Same commands, options and exit codes as the C# anfs.exe (Program.cs),
// plus the commands anfs-cpp.md 2.1 adds (config, import, install, uninstall, peer-legacy, shell); the ones whose
// modules land in later waves answer "not implemented in this build" with exit 2.
//
//   anfs run [--data DIR | --config FILE] [--verbose]      service loop; stays alive and idle in mode off
//   anfs status [--data DIR]                               prints status.json + running/ageSec; exit 1 when not running
//   anfs version [--verbose]
//   anfs hash-password [--iterations N]                    reads ANFS_PASSWORD (>= 8 chars), prints the PBKDF2 record
//   anfs config get | set k=v ... | set-wan-record <b64>   the single writer of anfs.json (atomic, rev++)
//
// Exit codes: 0 ok, 1 not running / failure, 2 usage or validation, 3 another instance owns the data folder.
//
// `run` wiring: the single-instance mutex, Log(<data>\logs) with console echo, the default anfs.json (mode off,
// fresh nodeId, machine name) when missing, ConfigWatcher -> ConfigStore::subscribe -> ModeManager::apply, the
// StatusWriter every 2 s, the loopback status endpoint on statusPort (GET /status), Ctrl+C / console close -> clean
// shutdown (StarRoot kills hard anyway, so nothing here relies on it).
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
#include "core/fsutil.h"
#include "core/gate.h"
#include "core/http_ctx.h"
#include "core/http_router.h"
#include "core/http_server.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "core/module.h"
#include "core/net.h"
#include "core/status.h"
#include "core/version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace anfs;

namespace {

struct Args {
    std::string command;
    std::vector<std::string> positional;          // after the command: config's verb and k=v pairs, shell's words
    std::string dataDir;                          // --data
    std::string configFile;                      // --config
    std::string tmpDir;                           // --tmp (selftest)
    std::string group;                            // --group (selftest)
    std::string from;                             // --from (import)
    std::string dir;                              // --dir (peer-legacy)
    int64_t iterations = 600000;                  // --iterations (hash-password)
    bool verbose = false;
    bool purge = false;
};

void usage(std::FILE* out)
{
    std::fprintf(out, "%s\n", versionLine());
    std::fprintf(out, "usage: anfs <run|status|version|hash-password|config|import|install|uninstall|selftest|peer-legacy|shell>"
                      " [--data DIR] [--config FILE] [--iterations N] [--tmp DIR] [--group TEXT] [--verbose]\n");
    std::fprintf(out, "  run            service loop (reads anfs.json from the data folder; idle in mode off)\n");
    std::fprintf(out, "  status         prints status.json (+ running, ageSec); exit 1 when not running\n");
    std::fprintf(out, "  version        prints the version (--verbose adds the build)\n");
    std::fprintf(out, "  hash-password  reads ANFS_PASSWORD (min 8 chars), prints a PBKDF2-SHA256 record as JSON\n");
    std::fprintf(out, "  config get | set k=v ... | set-wan-record <base64 json>   the only writer of anfs.json (rev++)\n");
    std::fprintf(out, "  import | install | uninstall | selftest | peer-legacy | shell   (later slices)\n");
}

// C# Program.Parse plus the C++ options. Throws std::invalid_argument on a bad line (exit 2).
Args parseArgs(int argc, char** argv)
{
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto value = [&](std::string& into) {
            if (i + 1 >= argc) throw std::invalid_argument(s + " needs a value");
            into = argv[++i];
        };
        if (s == "--data") value(a.dataDir);
        else if (s == "--config") value(a.configFile);
        else if (s == "--tmp") value(a.tmpDir);
        else if (s == "--group") value(a.group);
        else if (s == "--from") value(a.from);
        else if (s == "--dir") value(a.dir);
        else if (s == "--iterations") {
            std::string v;
            value(v);
            char* end = nullptr;
            const long long n = std::strtoll(v.c_str(), &end, 10);
            if (v.empty() || *end != '\0' || n < 1 || n > 20000000) throw std::invalid_argument("bad --iterations");
            a.iterations = n;
        } else if (s == "--verbose") a.verbose = true;
        else if (s == "--purge") a.purge = true;
        else if (a.command.empty() && !s.empty() && s[0] != '-') {
            a.command = s;
            for (char& c : a.command) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!a.command.empty() && (a.command == "config" || a.command == "shell" || a.command == "selftest") &&
                   (s.empty() || s[0] != '-'))
            a.positional.push_back(s);
        else throw std::invalid_argument("unknown argument: " + s);
    }
    return a;
}

std::string trimSlash(std::string p)
{
    while (p.size() > 3 && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

// The data folder: --data, else the folder of --config, else %ProgramData%\StarRoot\anfs (C# Config.DefaultDataDir).
std::string dataDirOf(const Args& a)
{
    if (!a.dataDir.empty()) return trimSlash(fs::fullPath(a.dataDir));
    if (!a.configFile.empty()) {
        const std::string full = fs::fullPath(a.configFile);
        const size_t cut = full.find_last_of("\\/");
        return cut == std::string::npos ? fs::cwd() : trimSlash(full.substr(0, cut));
    }
    return defaultDataDir();
}

std::string configPathOf(const Args& a)
{
    return a.configFile.empty() ? dataDirOf(a) + "\\anfs.json" : fs::fullPath(a.configFile);
}

std::string machineName()
{
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(buf, &n)) return fs::narrow(std::wstring(buf, n));
    return net::hostName();
}

std::string envUtf8(const wchar_t* name)
{
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring w(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, w.data(), n);
    w.resize(got);
    return fs::narrow(w);
}

std::string guidN() { std::string u = ids::uuid(); u.erase(std::remove(u.begin(), u.end(), '-'), u.end()); return u; }

// Cross-process writer lock for one anfs.json: ConfigStore::update is conditional only inside a process, so two
// `anfs config set` invocations (or one racing `run`'s nodeId fill) would each read rev N and both write N+1. The
// writer holds Global\anfs-cfg-<16 hex of SHA-1(lower(path))> around load + update; readers never take it.
class ConfigLock {
public:
    explicit ConfigLock(const std::string& configPath)
    {
        std::string low = configPath;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const std::string name = "Global\\anfs-cfg-" + ids::sha1Hex(low).substr(0, 16);
        h_ = CreateMutexW(nullptr, FALSE, fs::widen(name).c_str());
        if (!h_) h_ = CreateMutexW(nullptr, FALSE, fs::widen("Local\\anfs-cfg-" + ids::sha1Hex(low).substr(0, 16)).c_str());
        if (!h_) return;
        const DWORD w = WaitForSingleObject(h_, 30000);
        held_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;   // an abandoned one (a killed writer) is ours now
        if (!held_) {
            CloseHandle(h_);
            h_ = nullptr;
        }
    }
    ~ConfigLock()
    {
        if (held_) ReleaseMutex(h_);
        if (h_) CloseHandle(h_);
    }
    bool held() const { return held_; }

private:
    HANDLE h_ = nullptr;
    bool held_ = false;
};

// C# Program.Run: a missing anfs.json gets defaults (mode off, fresh nodeId, machine name); an invalid one keeps the
// defaults and reports; an empty nodeId is filled in. Callers hold the ConfigLock.
void ensureConfig(ConfigStore& store, Log& log)
{
    if (!fs::exists(store.path())) {
        Config def;
        def.nodeId = guidN();
        def.name = machineName();
        store.save(def);
        log.info("created default config " + store.path() + " (mode off)");
    } else if (!store.load()) {
        log.error("config invalid, starting idle: " + store.lastError());
    }
    if (store.current()->nodeId.empty()) store.update([](Config& c) { c.nodeId = guidN(); });
}

// ---- status endpoint (127.0.0.1:<statusPort>/status, mgmt-integration.md 1.1): read-only, no secrets -----------------

class StatusEndpoint {
public:
    StatusEndpoint(StatusWriter& status, ConfigStore& store, Log& log) : status_(status), store_(store), log_(log)
    {
        auto handler = [this](http::Ctx& c) { c.json(200, status_.snapshot()); };
        router_.get("/status", handler);
        router_.get("/", handler);
    }
    ~StatusEndpoint() { stop(); }

    void apply(int wantPort)
    {
        if (wantPort == port_ && (wantPort <= 0 || (server_ && server_->running()))) return;
        stop();
        if (wantPort <= 0) return;
        http::ServerOptions opt;
        opt.maxThreads = 8;
        opt.resolveClient = [](http::Request& req, const Config&) {
            req.clientIp = req.peerIp;
            req.clientClass = net::classify(req.peerIp);
        };
        server_ = std::make_unique<http::Server>(router_, [this] { return store_.current(); }, &log_, opt);
        try {
            server_->start(wantPort, false, "127.0.0.1");
            port_ = wantPort;
            status_.remove("statusPortError");
        } catch (const std::exception& e) {
            log_.error(std::string("status endpoint: ") + e.what());
            status_.set("statusPortError", e.what());
            server_.reset();
            port_ = 0;
        }
    }

    void stop()
    {
        if (server_) server_->stop();
        server_.reset();
        port_ = 0;
    }

private:
    StatusWriter& status_;
    ConfigStore& store_;
    Log& log_;
    http::Router router_;
    std::unique_ptr<http::Server> server_;
    int port_ = 0;
};

// ---- commands -------------------------------------------------------------------------------------------------------

int cmdVersion(const Args& a)
{
    if (a.verbose) std::printf("%s (%s)\n", versionLine(), buildInfo());
    else std::printf("%s\n", versionLine());
    return 0;
}

// Reads the password from ANFS_PASSWORD only; the password is never printed.
int cmdHashPassword(const Args& a)
{
    const std::string pw = envUtf8(L"ANFS_PASSWORD");
    if (pw.empty()) {
        std::fprintf(stderr, "set the ANFS_PASSWORD environment variable\n");
        return 2;
    }
    if (pw.size() < 8) {
        std::fprintf(stderr, "ANFS_PASSWORD is too short: use at least 8 characters\n");
        return 2;
    }
    const PasswordRecord r = makePasswordRecord(pw, a.iterations);
    std::printf("%s\n", json::stringify(r.toJval()).c_str());
    return 0;
}

int cmdStatus(const Args& a)
{
    const JVal d = readStatusFile(dataDirOf(a));
    if (d.isNull()) {
        std::printf("{\"running\":false,\"mode\":\"unknown\",\"error\":\"no status file\"}\n");
        return 1;
    }
    std::printf("%s\n", json::stringifyPretty(d, 2).c_str());
    return d.getBool("running", false) ? 0 : 1;
}

HANDLE g_stopEvent = nullptr;

BOOL WINAPI onConsoleCtrl(DWORD)
{
    if (g_stopEvent) SetEvent(g_stopEvent);
    return TRUE;                                  // handled: the loop ends and main returns
}

int cmdRun(const Args& a)
{
    const std::string dataDir = dataDirOf(a);
    std::string err;
    if (!fs::makeDirs(dataDir, &err)) {
        std::fprintf(stderr, "cannot create the data folder %s: %s\n", dataDir.c_str(), err.c_str());
        return 1;
    }
    if (!acquireSingleInstance(dataDir)) {
        std::fprintf(stderr, "anfs is already running for %s\n", dataDir.c_str());
        return 3;
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    int rc = 0;
    {
        Log log(dataDir + "\\logs");
        log.setConsole(true);
        if (a.verbose) log.setMinLevel(LogLevel::Debug);
        const int64_t started = anfs::nowMs();
        log.state(std::string(versionLine()) + " starting, data " + dataDir);

        ConfigStore store(configPathOf(a));
        {
            ConfigLock lock(store.path());
            if (!lock.held()) log.warn("config writer lock not available; continuing");
            ensureConfig(store, log);
        }
        auto noteSecrets = [&log](const std::shared_ptr<const Config>& c) {
            if (!c->agentToken.empty()) log.registerSecret(c->agentToken);
        };
        noteSecrets(store.current());

        ModuleRegistry& registry = ModuleRegistry::global();
        std::unique_ptr<ModeManager> mm;
        StatusWriter status(dataDir + "\\status.json",
                            [&] { return mm ? buildStatus(*mm, store, log, started) : JVal::object(); }, &log);
        mm = std::make_unique<ModeManager>(store, log, registry, dataDir, status);
        StatusEndpoint endpoint(status, store, log);

        ConfigWatcher watcher(store);
        watcher.onError([&](const std::string& e) {
            log.error("config ignored, keeping previous settings: " + e);
            status.writeNow();
        });
        const int sub = store.subscribe([&](std::shared_ptr<const Config> c) {
            noteSecrets(c);
            try { mm->apply(c); } catch (const std::exception& e) { log.error(std::string("apply config failed: ") + e.what()); }
            endpoint.apply(c->statusPort);
        });

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

        try { mm->apply(store.current()); } catch (const std::exception& e) { log.error(std::string("startup apply failed: ") + e.what()); }
        endpoint.apply(store.current()->statusPort);
        status.start(2000);
        watcher.start();

        while (WaitForSingleObject(g_stopEvent, 1000) == WAIT_TIMEOUT) {
            try { mm->tick(); } catch (const std::exception& e) { log.error(std::string("tick failed: ") + e.what()); }
        }

        log.state("stopping");
        watcher.stop();
        store.unsubscribe(sub);
        endpoint.stop();
        mm->shutdown();
        status.stop();
        status.writeNow();
        mm.reset();                               // before `status` (declared after it) goes out of scope
        log.state("stopped");
        SetConsoleCtrlHandler(onConsoleCtrl, FALSE);
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    releaseSingleInstance();
    WSACleanup();
    return rc;
}

// -- config: the single writer of anfs.json --------------------------------------------------------------------------

// k=v values by key: typed keys keep their type, list keys split on commas, anything else is JSON when it parses
// (true, 7, ["a"]) and a string otherwise, so module keys in raw work without quoting.
JVal valueFor(const std::string& key, const std::string& text)
{
    static const char* const kStrings[] = {"mode", "nodeId", "name", "bindAddress", "agentToken", "bridgeUrl",
                                           "tls.pfx", "tls.passwordEnv"};
    static const char* const kInts[] = {"port", "tlsPort", "statusPort", "rev"};
    static const char* const kBools[] = {"bindAll", "test", "exposeFullDrives", "allowWrite", "wan.requireTls"};
    static const char* const kLists[] = {"bridgeFallbacks", "corsAllow", "wan.trustedProxies", "wan.gatedPaths"};
    auto in = [&](const char* const* list, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (key == list[i]) return true;
        return false;
    };
    if (key.rfind("folders.", 0) == 0 || in(kStrings, std::size(kStrings))) return JVal(text);
    if (in(kInts, std::size(kInts))) {
        char* end = nullptr;
        const long long n = std::strtoll(text.c_str(), &end, 10);
        if (text.empty() || *end != '\0') throw std::invalid_argument(key + " must be an integer");
        return JVal(static_cast<int64_t>(n));
    }
    if (in(kBools, std::size(kBools))) {
        std::string t = text;
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (t == "true" || t == "1" || t == "yes" || t == "on") return JVal(true);
        if (t == "false" || t == "0" || t == "no" || t == "off" || t.empty()) return JVal(false);
        throw std::invalid_argument(key + " must be true or false");
    }
    if (in(kLists, std::size(kLists))) {
        JVal arr = JVal::array();
        size_t i = 0;
        while (i <= text.size()) {
            size_t j = text.find(',', i);
            if (j == std::string::npos) j = text.size();
            std::string item = text.substr(i, j - i);
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
            while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
            if (!item.empty()) arr.push(item);
            i = j + 1;
        }
        return arr;
    }
    JVal parsed;
    if (!text.empty() && (text[0] == '{' || text[0] == '[' || text[0] == '"' || text[0] == '-' || text == "true" ||
                          text == "false" || text == "null" || std::isdigit(static_cast<unsigned char>(text[0]))) &&
        json::tryParse(text, parsed))
        return parsed;
    return JVal(text);
}

// a.b.c = v on an ordered object tree (intermediate non-objects are replaced).
void setPath(JVal& root, const std::string& key, JVal v)
{
    JVal* cur = &root;
    size_t i = 0;
    for (;;) {
        const size_t dot = key.find('.', i);
        const std::string part = key.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty()) throw std::invalid_argument("bad key '" + key + "'");
        if (dot == std::string::npos) {
            cur->set(part, std::move(v));
            return;
        }
        JVal* next = cur->find(part);
        if (!next || !next->isObject()) next = &cur->set(part, JVal::object());
        cur = next;
        i = dot + 1;
    }
}

void checkPortRange(const char* name, int p)
{
    if (p != 0 && (p < 1024 || p > 65535)) throw ConfigError(std::string(name) + " must be 0 or 1024-65535");
}

int cmdConfig(const Args& a)
{
    if (a.positional.empty()) {
        std::fprintf(stderr, "usage: anfs config get | set k=v ... | set-wan-record <base64 json> [--data DIR | --config FILE]\n");
        return 2;
    }
    const std::string verb = a.positional[0];
    const std::string dataDir = dataDirOf(a);
    std::string err;
    if (verb != "get" && !fs::makeDirs(dataDir, &err)) {
        std::fprintf(stderr, "cannot create the data folder %s: %s\n", dataDir.c_str(), err.c_str());
        return 1;
    }
    Log& log = nullLog();
    ConfigStore store(configPathOf(a));
    if (verb == "get") {
        if (!fs::exists(store.path())) {
            std::fprintf(stderr, "no config at %s\n", store.path().c_str());
            return 1;
        }
        if (!store.load()) {
            std::fprintf(stderr, "config invalid: %s\n", store.lastError().c_str());
            return 1;
        }
        std::printf("%s\n", json::stringifyPretty(store.current()->toSafeJval(), 2).c_str());
        return 0;
    }
    try {
        ConfigLock lock(store.path());
        if (!lock.held()) throw std::runtime_error("another config writer holds " + store.path() + " (30 s)");
        if (verb == "set") {
            if (a.positional.size() < 2) throw std::invalid_argument("config set needs at least one k=v");
            std::vector<std::pair<std::string, JVal>> edits;
            for (size_t i = 1; i < a.positional.size(); ++i) {
                const std::string& kv = a.positional[i];
                const size_t eq = kv.find('=');
                if (eq == std::string::npos || eq == 0) throw std::invalid_argument("expected k=v, got '" + kv + "'");
                edits.emplace_back(kv.substr(0, eq), valueFor(kv.substr(0, eq), kv.substr(eq + 1)));
            }
            ensureConfig(store, log);
            if (!store.lastError().empty()) throw ConfigError("config invalid, fix it by hand: " + store.lastError());
            const std::shared_ptr<const Config> saved = store.update([&](Config& c) {
                JVal j = c.toJval();
                for (const auto& e : edits) setPath(j, e.first, e.second);
                Config next = Config::fromJval(j);
                next.validate();
                checkPortRange("port", next.port);
                checkPortRange("tlsPort", next.tlsPort);
                checkPortRange("statusPort", next.statusPort);
                const std::string refusal = ModeManager::testModeRefusal(next);
                if (!refusal.empty()) throw ConfigError(refusal);
                next.rev = c.rev;
                c = std::move(next);
            }, /*bumpRev*/ true);
            std::printf("{\"rev\":%lld,\"mode\":%s}\n", static_cast<long long>(saved->rev), json::quote(saved->mode).c_str());
            return 0;
        }
        if (verb == "set-wan-record") {
            if (a.positional.size() != 2) throw std::invalid_argument("config set-wan-record needs one base64 argument");
            std::vector<uint8_t> bytes;
            if (!b64::decode(a.positional[1], bytes)) throw std::invalid_argument("set-wan-record: not base64");
            JVal j;
            std::string perr;
            if (!json::tryParse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), j, &perr))
                throw std::invalid_argument("set-wan-record: not JSON: " + perr);
            std::optional<PasswordRecord> rec = PasswordRecord::fromJval(j);
            if (!rec || !rec->isSet()) throw std::invalid_argument("set-wan-record: not a pbkdf2-sha256 record");
            ensureConfig(store, log);
            if (!store.lastError().empty()) throw ConfigError("config invalid, fix it by hand: " + store.lastError());
            const std::shared_ptr<const Config> saved = store.update([&](Config& c) { c.wan.passwordRecord = rec; }, true);
            std::printf("{\"rev\":%lld,\"passwordRecord\":\"[set]\"}\n", static_cast<long long>(saved->rev));
            return 0;
        }
        std::fprintf(stderr, "anfs config: unknown verb '%s'\n", verb.c_str());
        return 2;
    } catch (const ConfigError& e) {
        std::fprintf(stderr, "anfs config: %s\n", e.what());
        return 2;
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "anfs config: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anfs config: %s\n", e.what());
        return 1;
    }
}

} // namespace

int main(int argc, char** argv)
{
    Args a;
    try {
        a = parseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    if (a.command.empty()) {
        usage(stderr);
        return 0;
    }
    if (a.command == "version") return cmdVersion(a);
    if (a.command == "hash-password") return cmdHashPassword(a);
    if (a.command == "status") return cmdStatus(a);
    if (a.command == "run") return cmdRun(a);
    if (a.command == "config") return cmdConfig(a);
    static const char* const later[] = {"import", "install", "uninstall", "selftest", "peer-legacy", "shell"};
    for (const char* k : later) {
        if (a.command == k) {
            std::fprintf(stderr, "anfs %s: not implemented in this build (a later AN-FS slice adds it)\n", k);
            return 2;
        }
    }
    usage(stderr);
    return 2;
}
