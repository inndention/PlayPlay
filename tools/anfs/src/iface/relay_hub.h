// iface/relay_hub.h - what srv-registry asks the relay about (mod-srv-relay.md, C# SrvRelayHub.cs IRelayHub). FROZEN
// (ANFS-F0). Implemented by srv-relay; published with svcSet<IRelayHub>.
#pragma once

#include "core/json.h"

#include <string>

namespace anfs {

class IRelayHub {
public:
    virtual ~IRelayHub() = default;
    // A peer is relay-capable when it polled within the last 45 s (anfs::clock() time).
    virtual bool relayActive(const std::string& agentId) const = 0;
    // The peer expired: drop its queued jobs and answer its parked polls 204.
    virtual void destroy(const std::string& agentId) = 0;
    // {relayed, queuedJobs, parkedPolls, pendingJobs} (the module status).
    virtual JVal stats() const = 0;
};

} // namespace anfs
