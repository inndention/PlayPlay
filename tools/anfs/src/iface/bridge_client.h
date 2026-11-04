// iface/bridge_client.h - the peer's channel to its bridge (mod-peer-link.md; C# PeerLinkModule.cs IBridgeClient).
// FROZEN (ANFS-F0). Implemented by peer-link; published with svcSet<IBridgeClient>; wd-scan posts reports through it
// and w3-peer-shell uses it for the picker calls.
//
// Every call carries x-agent-auth; bridge failover (bridgeUrl, bridgeFallbacks) is peer-link's business. Calls never
// throw: a transport failure is status 0 with the reason in text.
#pragma once

#include "core/http_ctx.h"
#include "core/stream.h"

#include <cstdint>
#include <string>

namespace anfs {

struct BridgeResult {
    int status = 0;                               // 0 = transport failure
    std::string text;                             // response body, or the failure reason
    http::Headers headers;                        // lower-case names
};

class IBridgeClient {
public:
    virtual ~IBridgeClient() = default;
    virtual std::string agentId() const = 0;      // "" until a register succeeded
    virtual bool connected() const = 0;           // a register returned 200 within the last 15 s
    virtual std::string bridgeUrl() const = 0;    // the bridge currently in use ("" when none configured)
    virtual BridgeResult send(const std::string& method, const std::string& pathAndQuery,
                              const std::string& contentType, const std::string& body, int timeoutMs) = 0;
    // Streaming body (the multi-MB watchdog report): length -1 = chunked.
    virtual BridgeResult sendStream(const std::string& method, const std::string& pathAndQuery,
                                    const std::string& contentType, Source& body, int64_t length, int timeoutMs) = 0;
};

} // namespace anfs
