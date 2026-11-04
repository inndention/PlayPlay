// core/config.h - anfs.json (flat schema of C# Config.cs:255-330 plus rev/statusPort from mgmt-integration.md),
// the atomic ConfigStore and the 2 s ConfigWatcher. FROZEN (ANFS-F0); implemented by ANFS-F1b (core/config.cpp).
//
// One data folder is shared with the C# anfs.exe (rollback is a binary swap), so the keys, defaults and the write
// rules match C# exactly: unknown keys survive in `raw` and keep their position; toJval() starts from raw and
// overwrites the typed keys (C# Config.ToDict order rules); exposeFullDrives and allowWrite default to TRUE.
//
//   mode off|peer|server, nodeId, name, port (0 = 8080 server / 8899 peer; test 18080 / 18899), tlsPort (0 = 443,
//   test 18443), statusPort (0 = none; loopback only), bindAll (true), bindAddress ("127.0.0.1"), test,
//   exposeFullDrives (true), allowWrite (true), agentToken (secret), bridgeUrl (string or array) + bridgeFallbacks,
//   corsAllow, shares[{id,name,path}], folders{dropbox,...}, wan{passwordRecord,gatedPaths,trustedProxies,
//   requireTls}, tls{pfx,passwordEnv}, rev (bumped by `anfs config set`, echoed as status appliedRev).
// Module keys live in raw and are read with rawStr/rawBool/rawInt (allowRemoteBridge, denyPaths, webroot, ...).
#pragma once

#include "core/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace anfs {

class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& what) : std::runtime_error(what) {}
};

struct Share {
    std::string id;
    std::string name;
    std::string path;
    bool operator==(const Share& o) const { return id == o.id && name == o.name && path == o.path; }
};

// PBKDF2-SHA256 record (base64 salt and hash; a 64-hex hash with a hex salt is also accepted). Verification and
// creation live in core/gate.h (verifyPassword / makePasswordRecord).
struct PasswordRecord {
    static constexpr const char* kDefaultAlgo = "pbkdf2-sha256";
    std::string algo;
    int64_t iterations = 0;
    std::string salt;
    std::string hash;

    bool isSet() const;                            // iterations > 0, salt and hash present, algo pbkdf2[-sha256]
    JVal toJval() const;                           // {algo, iterations, salt, hash}
    static std::optional<PasswordRecord> fromJval(const JVal& v);   // nullopt unless an object
};

// "METHOD pattern" gate rule; METHOD is * or a comma list; the pattern supports {param}, {*rest} and a trailing *.
struct GateRule {
    std::string method = "*";
    std::string pattern = "/";
    bool matches(std::string_view method, std::string_view path) const;
    std::string toString() const;                 // "METHOD pattern"
    static std::optional<GateRule> parse(const JVal& v);            // string or {method, pattern}
};

struct WanConfig {
    std::optional<PasswordRecord> passwordRecord;
    std::vector<GateRule> gatedPaths;
    std::vector<std::string> trustedProxies{"127.0.0.1", "::1"};
    bool requireTls = false;
};

struct TlsConfig {
    std::string pfx;
    std::string passwordEnv;                      // name of the environment variable, never the password
};

struct Config {
    static constexpr const char* kModeOff = "off";
    static constexpr const char* kModePeer = "peer";
    static constexpr const char* kModeServer = "server";

    std::string mode = kModeOff;
    std::string nodeId;
    std::string name;
    int port = 0;
    int tlsPort = 0;
    int statusPort = 0;
    bool bindAll = true;
    std::string bindAddress = "127.0.0.1";
    bool test = false;
    std::vector<std::string> bridgeUrls;          // bridgeUrl first, then bridgeFallbacks (deduplicated)
    std::vector<Share> shares;
    bool exposeFullDrives = true;
    bool allowWrite = true;
    std::vector<std::pair<std::string, std::string>> folders;   // ordered; keys compared case-insensitively
    WanConfig wan;
    std::string agentToken;                       // secret: never logged, "[set]" in status
    TlsConfig tls;
    std::vector<std::string> corsAllow;
    int64_t rev = 0;
    JVal raw = JVal::object();                    // everything as parsed, unknown keys included

    std::string bridgeUrl() const;                // first bridge URL or ""
    std::string folder(std::string_view key) const;
    std::string dropbox() const { return folder("dropbox"); }
    void setFolder(std::string_view key, std::string value);
    int effectivePort() const;                    // port, else 8080/18080 (server) or 8899/18899 (peer, off)
    int effectiveTlsPort() const;                 // tlsPort, else 443/18443

    std::string rawStr(std::string_view key, std::string_view def = {}) const;
    bool rawBool(std::string_view key, bool def) const;
    int64_t rawInt(std::string_view key, int64_t def) const;
    const JVal& rawGet(std::string_view key) const;   // null when absent

    void validate();                              // trims/lower-cases mode ("" -> off); throws ConfigError on bad
                                                  // mode or out-of-range ports
    static void validateMode(const std::string& mode);

    static Config fromJson(std::string_view text);   // throws ConfigError ("invalid JSON: ...", validation texts)
    static Config fromJval(const JVal& obj);
    JVal toJval() const;                          // raw + typed keys (C# ToDict): absent-when-default keys removed
    std::string toJsonText() const;               // json::stringifyPretty(toJval()) + "\r\n"
    JVal toSafeJval() const;                      // agentToken and wan.passwordRecord -> "[set]", tls.passwordEnv dropped
};

// %ProgramData%\StarRoot\anfs (C# Config.DefaultDataDir).
std::string defaultDataDir();

// Writes data to path atomically: a temp file in the same folder, FlushFileBuffers, then
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), retried briefly on sharing violations.
// Used for anfs.json, status.json and peer-config.json. Returns false and fills err on failure.
bool writeFileAtomic(const std::string& path, std::string_view data, std::string* err = nullptr);

// Owns anfs.json: load, atomic save, conditional read-modify-write. Snapshots are immutable shared_ptrs, so readers
// never lock against a writer. Subscribers run after the store's locks are released (they may call update()).
class ConfigStore {
public:
    using Listener = std::function<void(std::shared_ptr<const Config>)>;

    explicit ConfigStore(std::string path);
    ~ConfigStore();
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    const std::string& path() const;
    std::shared_ptr<const Config> current() const;   // never null (defaults before the first load)
    std::string lastError() const;                   // "" when the last load/save succeeded
    int64_t version() const;                         // bumps on every successful load or save

    // True when the file's size, mtime or SHA-256 differs from what was last loaded or saved.
    bool hasChangedOnDisk() const;
    // Reads the file. Invalid content or a vanished file keeps the previous config and records the error.
    bool load();
    // Writes c (validated, toJsonText) atomically and swaps it in. Throws ConfigError on invalid c or a write failure.
    void save(const Config& c);
    // Atomic read-modify-write: mutate runs on a copy of the newest config and may run more than once (a concurrent
    // save/load between read and write re-runs it), so it must depend only on its argument. bumpRev increments rev.
    std::shared_ptr<const Config> update(const std::function<void(Config&)>& mutate, bool bumpRev = false);

    int subscribe(Listener l);
    void unsubscribe(int id);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Polls the store every intervalMs (default 2000) on its own thread (anfs::clock() time) and calls load() when the
// file changed on disk. Load errors are reported once per distinct text through onError.
class ConfigWatcher {
public:
    explicit ConfigWatcher(ConfigStore& store, int intervalMs = 2000);
    ~ConfigWatcher();                             // stops the thread (within one interval, at most 2 s)
    ConfigWatcher(const ConfigWatcher&) = delete;
    ConfigWatcher& operator=(const ConfigWatcher&) = delete;

    void start();
    void stop();
    void poll();                                  // one synchronous check (tests)
    void onError(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace anfs
