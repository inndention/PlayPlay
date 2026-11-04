// core/status.h - status.json (every 2 s, atomic) and the loopback status endpoint. FROZEN (ANFS-F0); implemented by
// ANFS-F7 (core/status.cpp).
//
// status.json is read by StarRoot/starcmd (mgmt-integration.md) and by `anfs status`. Fields (C# Status.Build plus
// the mgmt-integration additions), in this order:
//   mode, version, ver, pid, nodeId, name, port, statusPort, listening (array or null), loopbackOnly, startedAt,
//   updatedAt (ISO-ms), uptimeSec, uptimeS, rev, appliedRev, bridge ("connected"|"connecting"|"down"|null),
//   modules {name: status}, lastError, lastErrorTs, configError, generation, gate, db {path, mode, sizeBytes},
//   peers {count, relayed, nodes[{id, live, relay}]} (server with srv-registry only), config (toSafeJval()).
// Secrets never appear: agentToken and passwordRecord are "[set]".
#pragma once

#include "core/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace anfs {

class ConfigStore;
class Log;
class ModeManager;

class StatusWriter {
public:
    StatusWriter(std::string path, std::function<JVal()> build, Log* log = nullptr);
    ~StatusWriter();                              // stop()
    StatusWriter(const StatusWriter&) = delete;
    StatusWriter& operator=(const StatusWriter&) = delete;

    const std::string& path() const;
    void setBuilder(std::function<JVal()> build);
    void set(const std::string& key, JVal value); // extra top-level keys merged into every write (e.g. "bridge")
    void remove(const std::string& key);
    void start(int intervalMs = 2000);            // own thread, anfs::clock() time
    void stop();
    void writeNow();                              // a failed write is logged once until the next success
    JVal snapshot() const;                        // what the next write would contain

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// The standard status document for `anfs run`.
JVal buildStatus(const ModeManager& mm, const ConfigStore& store, const Log& log, int64_t startedAtMs);

// Reads <dataDir>\status.json for `anfs status`; adds running (process alive) and ageSec. null when missing.
JVal readStatusFile(const std::string& dataDir);

} // namespace anfs
