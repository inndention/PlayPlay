// iface/peer_client.h - every server-to-peer call, direct or relayed, behind one API (mod-srv-relay.md "PeerClient";
// C# PeerClient.cs). FROZEN (ANFS-F0). Implemented by srv-relay; published with svcSet<IPeerClient>. Used by
// srv-proxy, srv-xfer, buckets, syncjobs, swarm, knowledge, thumbs, transcripts and Wave 3.
//
// - The path is given WITHOUT agentId; the client always sets header x-agent-auth and query auth=<token> (replacing
//   any existing ones), exactly like Node.
// - Direct: WinHTTP to http://{agentHost}:{port}{path}, inactivity timeout idleMs, no redirects, statuses >= 400 are
//   normal responses.
// - Relay (agent.relay): the wire job {id, method, path (query incl. auth=), headers (filtered), hasBody} goes to the
//   oldest parked poller; 30 s until picked up, 120 s until the first response byte, else a 504 response with
//   {"error":"Relay peer did not pick up the request."} / {"error":"Relay peer did not answer in time."}.
//   Response headers are only what the peer sent in x-relay-headers.
#pragma once

#include "core/http_ctx.h"
#include "core/stream.h"
#include "iface/agent_registry.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace anfs {

// Transport failure or timeout before a response (connection refused, reset, DNS, idle timeout).
class PeerError : public std::runtime_error {
public:
    explicit PeerError(const std::string& what) : std::runtime_error(what) {}
};

struct PeerRequest {
    static constexpr int64_t kUnset = -2;         // derive from body->length(), else chunked
    std::string method = "GET";
    std::string pathAndQuery = "/";
    http::Headers headers;
    Source* body = nullptr;                       // not owned; read on the calling thread
    int64_t bodyLength = kUnset;                  // -1 = chunked
    int idleMs = 30000;

    static PeerRequest get(std::string pathAndQuery)
    {
        PeerRequest r; r.pathAndQuery = std::move(pathAndQuery); return r;
    }
};

// A streaming response. Destroying it releases the connection or the relay job.
class PeerResponse : public Source {
public:
    int status = 0;
    http::Headers headers;                        // lower-case names
    int64_t contentLength = -1;                   // -1 when unknown

    int64_t length() const override { return contentLength; }
    virtual void close() {}
    // Reads at most max bytes of the body as text (error bodies, small JSON).
    std::string readText(size_t max = 1 << 20)
    {
        std::string out;
        char buf[16384];
        while (out.size() < max) {
            size_t k = read(buf, std::min(sizeof(buf), max - out.size()));
            if (k == 0) break;
            out.append(buf, k);
        }
        return out;
    }
};

class IPeerClient {
public:
    virtual ~IPeerClient() = default;
    // Throws PeerError on a transport failure; any HTTP status (including relay 504s) is a response.
    virtual std::unique_ptr<PeerResponse> send(const Agent& agent, PeerRequest& request) = 0;
    // proxyToAgent: copies request headers except host (plus x-agent-auth), drops agentId from the query, streams
    // the body both ways (HEAD: headers only). A transport error before the headers answers
    // 504 {"error":"Connection to <hostname> failed: <message>"}; after the headers it aborts the connection.
    // The caller has already answered 404 {"error":"Selected client agent is offline."} for an offline agent.
    virtual void proxy(http::Ctx& ctx, const Agent& agent, const std::string& peerPath) = 0;
};

} // namespace anfs
