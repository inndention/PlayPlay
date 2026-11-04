// iface/agent_registry.h - live peer registry of the server (mod-srv-registry.md "Services published"). FROZEN
// (ANFS-F0). Implemented by srv-registry; published with svcSet<IAgentRegistry>.
#pragma once

#include "core/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anfs {

constexpr const char* kLocalNodeId = "__local__";

// One live peer as known from its last heartbeat (server.patched.js agentsRegistry record). A heartbeat replaces the
// record but carries over relay, reachable, probeKey and probeAt. Records are immutable snapshots.
struct Agent {
    std::string id;                               // md5Hex12(hostname + "_" + ip)
    std::string hostname;
    std::string machineName;                      // defaults to hostname
    std::string ip;                               // as reported
    std::optional<std::string> observedIp;        // the request's client IP (trusted-proxy aware)
    int port = 0;                                 // dial port (= reportedPort; no loopback relay endpoints)
    int reportedPort = 0;
    bool relayOnly = false;
    bool relay = false;                           // reached through the in-process relay
    std::optional<bool> reachable;                // last TCP probe result
    std::string probeKey;                         // "host:port" of that probe
    int64_t probeAt = 0;
    JVal drives;                                  // raw JSON as sent
    JVal sharedFolders;                           // raw JSON as sent
    std::optional<std::string> dropboxPath;       // null when missing
    bool fullDrives = false;
    bool elevated = false;
    int64_t lastSeen = 0;                         // epoch ms
};

using AgentPtr = std::shared_ptr<const Agent>;

class IAgentRegistry {
public:
    virtual ~IAgentRegistry() = default;
    virtual AgentPtr get(const std::string& id) const = 0;        // null when unknown or expired
    virtual std::vector<AgentPtr> all() const = 0;
    virtual bool isOnline(const std::string& id) const = 0;
    virtual AgentPtr byName(const std::string& nameOrId) const = 0;   // machineName or hostname (case-insensitive), then id
    // The address to dial: observedIp when it differs from ip, else ip (relayed agents are never dialled).
    virtual std::string agentHost(const Agent& a) const = 0;
    // "Local Bridge" for "" and "__local__", else the live hostname, else the id.
    virtual std::string nodeLabel(const std::string& id) const = 0;
    // Called after each successful register (on the request thread; keep it short). Returns an id for unsubscribe.
    virtual int subscribeRegistered(std::function<void(const AgentPtr&)> cb) = 0;
    virtual void unsubscribeRegistered(int id) = 0;
};

} // namespace anfs
