// iface/node_store.h - persistent node and share rows (tables nodes, node_shares; owner srv-registry; C#
// SrvRegistryStore.cs). FROZEN (ANFS-F0). Published with svcSet<INodeStore>.
#pragma once

#include "core/config.h"
#include "core/json.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace anfs {

struct NodeRow {
    std::string id;
    std::string hostname;
    std::string machineName;
    std::string ip;
    int port = 0;
    std::string dropboxPath;                      // "" stored as NULL
    bool isLocal = false;
    int64_t lastSeen = 0;
};

class INodeStore {
public:
    virtual ~INodeStore() = default;
    // Upsert with online = 1 (one tx together with syncNodeShares on register; failures are logged, never thrown
    // into the register route).
    virtual void upsertNode(const NodeRow& row) = 0;
    // Deletes the node's share rows, then inserts {share id, name, path} (entries without id/name/path skipped).
    virtual void syncNodeShares(const std::string& nodeId, const std::vector<Share>& shares) = 0;
    // online = 0 for every node not in onlineIds ("__local__" included by the caller).
    virtual void markOffline(const std::set<std::string>& onlineIds) = 0;
    // Rows {nodeId, machineName, hostname, online} (w3-dash, srv-shares).
    virtual JVal knownNodes() const = 0;
    // Rows {nodeId, machineName, hostname, shareId, shareName, path, online}.
    virtual JVal allShares() const = 0;
    virtual std::string nameOf(const std::string& nodeId) const = 0;   // machine name or hostname, else ""
    // POST /api/nodes/prune (db.js pruneSessions): {removed, before, after}.
    virtual JVal prune() = 0;
};

} // namespace anfs
