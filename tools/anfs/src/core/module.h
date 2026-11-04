// core/module.h - the module contract, static registration and the ModeManager. FROZEN (ANFS-F0); implemented by
// ANFS-F7 (core/module.cpp).
//
// A module is a folder tools/anfs/src/mod/<id>/ whose *_module.cpp ends with one registration line:
//
//   ANFS_MODULE(SrvRegistryModule, "server", 20);    // modes: comma list of "common", "peer", "server"
//
// All modules compile into the OBJECT library anfs_obj, so every static registrar runs (a static library would let
// the linker drop them). Lower order starts first; the router's first-registered tie rule depends on it
// (anfs-cpp.md 2.10 start-order table). Equal orders start in registration (link) order.
//
// Lifecycle (CONVENTIONS.md section 3): start() and stop() are idempotent and restartable in-process (a mode switch
// never exits the process). start() registers routes (owner = name()), publishes services (svcSet) and starts threads;
// stop() stops threads within 2 s, closes handles and removes its services (svcRemoveIf). status() is a flat object
// without secrets; it appears in status.json under modules.<name>. Log state changes and errors only.
#pragma once

#include "core/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace anfs {

class ConfigStore;
class Db;
class Gate;
class Log;
class StatusWriter;
struct Config;
namespace http {
class Router;
class Server;
} // namespace http

struct Context {
    ConfigStore& store;
    Log& log;
    http::Router& http;
    Gate& gate;
    StatusWriter& status;                         // from a module's own threads use set()/remove() only: writeNow()
                                                  // runs the status builder, which locks the ModeManager that may be
                                                  // joining that thread in stop() -> deadlock
    std::shared_ptr<Db> db;                       // the data folder's anfs.db; null when it could not be opened
    http::Server* server = nullptr;               // the bound server of this generation
    std::string mode;                             // "peer" or "server"
    std::string dataDir;
    std::string logsDir;                          // <data>\logs
    std::string tempDir;                          // <data>\temp_uploads (server) or <data>\temp_uploads_agent (peer)
    std::string cacheDir;                         // <data>\cache
    int port = 0;                                 // bound HTTP port
    int generation = 0;                           // bumps on every apply

    std::shared_ptr<const Config> config() const; // store.current()
    static const char* version();                 // core/version.h kVersion
};

class Module {
public:
    virtual ~Module() = default;
    virtual const char* name() const = 0;         // the module id ("srv-registry", "pfs-browse", ...)
    virtual void start(Context& ctx) = 0;         // throw to report a start failure (recorded, others continue)
    virtual void stop() = 0;
    virtual JVal status() const = 0;
};

using ModuleFactory = std::function<std::unique_ptr<Module>()>;

struct ModuleInfo {
    std::string className;
    std::string modes;                            // "common", "peer", "server" or a comma list
    int order = 100;
    int seq = 0;
    ModuleFactory factory;
    bool runsIn(std::string_view mode) const;     // "common" runs in peer and server; never in off
};

class ModuleRegistry {
public:
    static ModuleRegistry& global();              // filled by ANFS_MODULE static registrars
    ModuleRegistry() = default;                   // tests build their own with only the modules under test

    int add(std::string_view modes, int order, ModuleFactory factory, const char* className);
    std::vector<ModuleInfo> list() const;         // sorted by (order, seq)
    std::vector<std::unique_ptr<Module>> create(std::string_view mode) const;

private:
    std::vector<ModuleInfo> items_;
};

// Applies a config: creates the modules for the mode (common + mode), binds the server, starts them in order;
// stops the old ones in reverse (2 s each) first. A port/bind change rebinds; a bind failure rolls back to the
// previous working state and records lastError. Per-module start errors are recorded without aborting the others.
// Test-mode guards (deviation C7) refuse ports 8080, 8899 and 443 and a non-loopback bridge URL unless
// raw.allowRemoteBridge is true. Mode "off" runs no modules and binds nothing (only the status loop).
// ANFS-F7 notes: the server is bound before any module starts (ctx.port is the bound port, also for port 0); in
// server mode the runtime installs cors::serverMiddleware() then Gate::middleware() with useFirst (owner "runtime"),
// so srv-wan only seeds the lists and must not install a second gate; peer mode gets no runtime middleware
// (pfs-setup installs its own). A refused test-mode config leaves the running generation alone (lastError only).
class ModeManager {
public:
    ModeManager(ConfigStore& store, Log& log, ModuleRegistry& registry, std::string dataDir, StatusWriter& status);
    ~ModeManager();                               // tears down without a status write (the writer may be gone)
    ModeManager(const ModeManager&) = delete;
    ModeManager& operator=(const ModeManager&) = delete;

    void apply(std::shared_ptr<const Config> config);
    void tick();                                  // retries a failed apply every retryMs (anfs::clock() time)
    void shutdown();                              // idempotent; writes status once, so call it while the writer lives

    std::string mode() const;
    int port() const;
    int generation() const;
    int64_t appliedRev() const;                   // rev of the last successfully applied config
    bool failed() const;
    std::string lastError() const;
    int64_t lastErrorAtMs() const;
    std::vector<std::string> moduleNames() const;
    JVal moduleStatuses() const;                  // {name: status | {"error": text}}
    http::Server* server() const;
    void setRetryMs(int ms);

    // "" when allowed, else the refusal text (used by apply() and by `anfs config set` validation).
    static std::string testModeRefusal(const Config& c);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Single instance per data folder, shared with the C# binary (Program.cs:131-148): named mutex
// Global\anfs-<first 16 UPPER-case hex digits of SHA-1(UTF-8(ToLowerInvariant(dataDir)))>, dataDir being the full
// path (GetFullPath of --data, else the default). C# formats with BitConverter.ToString (upper case) and Windows object
// names are case-sensitive, so ids::sha1Hex (lower case) must be upper-cased, or both binaries could run on one data
// folder during a rollback swap. The loser exits 3; access denied (held by another account) also counts as held.
// Returns false when another process holds it; the handle lives until releaseSingleInstance().
bool acquireSingleInstance(const std::string& dataDir, std::string* mutexName = nullptr);
void releaseSingleInstance();

} // namespace anfs

#define ANFS_MODULE_CAT2(a, b) a##b
#define ANFS_MODULE_CAT(a, b) ANFS_MODULE_CAT2(a, b)
// ANFS_MODULE(Class, "modes", order): registers Class (default-constructible, derived from anfs::Module).
#define ANFS_MODULE(Class, modes, order)                                                                   \
    static const int ANFS_MODULE_CAT(anfs_module_registrar_, __LINE__) = ::anfs::ModuleRegistry::global().add( \
        (modes), (order), [] { return std::unique_ptr<::anfs::Module>(new Class()); }, #Class)
