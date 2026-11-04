// iface/index_store.h - the file index (tables watchdog_snapshots, change_events, index_exemptions; owner wd-ingest;
// mod-wd-ingest.md; C# WdIngestStore.cs). FROZEN (ANFS-F0). Published with svcSet<IIndexStore>; consumers (wd-scan,
// wd-feed, media-lib, swarm, syncjobs, transcripts) tolerate svcGet returning null.
//
// Deviation C3: ingest diffs in memory and writes deltas in 2,000-row transactions (unchanged rows touched by one
// UPDATE); change_events keep the newest 100,000 rows and 90 days. Deviation C1: the first report of a node is a
// baseline (no change events) unless raw.watchdogBaseline is false.
#pragma once

#include "core/json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace anfs {

// One file or directory as a peer reports it (watchdog report entry) or as the local scan produces it.
struct IndexEntry {
    std::string path;
    bool isDirectory = false;
    int64_t size = 0;
    int64_t mtime = 0;                            // epoch ms (integer, as the wire sends it)
    std::string hash;                             // quickFileHash, "" when absent
};

struct IngestResult {
    int changes = 0;                              // change events inserted
    int scanned = 0;                              // entries kept after filtering
    int filtered = 0;                             // dropped (bad path, denied, exempted, icon noise)
    bool baseline = false;                        // first snapshot of the node: no change events
};

// A watchdog_snapshots row. nodeId is set by bySize(); hash is "" when never fingerprinted.
struct SnapEntry {
    std::string nodeId;
    std::string path;
    bool isDirectory = false;
    int64_t size = 0;
    int64_t mtime = 0;
    std::string hash;
};

struct HashSource {
    std::string nodeId;
    std::string filePath;
    int64_t size = 0;
    int64_t mtime = 0;
    std::string machineName;
    std::string hostname;
    bool online = false;                          // nodes.online (false when the node row is unknown)
};

struct ChangeRow {
    int64_t id = 0;
    std::string nodeId;
    std::string changeType;                       // "added" | "modified" | "deleted"
    std::string path;
    int64_t size = 0;
    int64_t mtime = 0;
    int64_t detectedAt = 0;
    std::string hostname;
    std::string machineName;
};

// getDirSwarmInfo: byPath = lower-cased path -> hash for the direct file children; presence = hash -> node ids.
struct DirSwarmInfo {
    std::map<std::string, std::string> byPath;
    std::map<std::string, std::vector<std::string>> presence;
};

class IIndexStore {
public:
    virtual ~IIndexStore() = default;
    virtual IngestResult ingest(const std::string& nodeId, const std::vector<IndexEntry>& entries) = 0;
    virtual std::vector<std::string> exemptedPaths(const std::string& nodeId) const = 0;
    virtual int64_t addExemption(const std::string& nodeId, const std::string& path, const std::string& label) = 0;
    virtual JVal exemptions(const std::string& nodeId) const = 0;  // rows (key order of the wd-feed route)
    virtual void removeExemption(int64_t id) = 0;
    virtual int purge(const std::string& nodeId, const std::string& path) = 0;   // /api/index/forget
    virtual DirSwarmInfo dirSwarm(const std::string& nodeId, const std::string& dir) const = 0;
    virtual std::vector<HashSource> hashSources(const std::string& hash) const = 0;
    virtual std::vector<SnapEntry> loadSnapshot(const std::string& nodeId) const = 0;
    virtual std::vector<SnapEntry> bySize(int64_t size) const = 0;
    virtual std::string hashOf(const std::string& nodeId, const std::string& path) const = 0;   // "" when unknown
    virtual std::vector<ChangeRow> changes(int limit, const std::string& nodeId) const = 0;     // nodeId "" = all
    virtual std::vector<ChangeRow> addedSince(int64_t sinceId, int limit) const = 0;
};

} // namespace anfs
