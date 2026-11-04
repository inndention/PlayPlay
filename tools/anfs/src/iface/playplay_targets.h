// iface/playplay_targets.h - PlayPlay stations the bridge can send media to (w3-playplay; Node playplay_targets table,
// db.js:205, playplayFetch server.patched.js:2210). FROZEN (ANFS-F0). Implemented by w3-playplay; published with
// svcSet<IPlayplayTargets>; used by w3-ingest, w3-ingests, w3-dash and w3-switchboard.
//
// peerKey (X-PP-Key) is a secret: stored, sent to the station, never returned by any route or logged.
#pragma once

#include "core/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anfs {

struct PlayplayTarget {
    std::string id;
    std::string label;
    std::string nodeId;                           // AN-FS node hosting it ("" = not paired)
    std::string baseUrl;                          // direct http://host:7860; "" = reach through the node's agent
    std::string peerKey;                          // secret
    std::string landingDir;                       // absolute folder ON THAT MACHINE for dropped files
    std::string channelId = "CH1";
    bool enabled = true;
    int64_t sort = 0;
    int64_t createdAt = 0;

    // The table row without peer_key, in column order (GET /api/playplay/targets then appends nodeOnline, reachable,
    // detail, channels: server.patched.js:2300-2321).
    JVal toSafeRow() const
    {
        return JVal::object({{"id", JVal(id)},
                             {"label", JVal(label)},
                             {"node_id", JVal(nodeId)},
                             {"base_url", JVal(baseUrl)},
                             {"landing_dir", JVal(landingDir)},
                             {"channel_id", JVal(channelId)},
                             {"enabled", JVal(enabled ? 1 : 0)},
                             {"sort", JVal(sort)},
                             {"created_at", JVal(createdAt)}});
    }
};

class IPlayplayTargets {
public:
    virtual ~IPlayplayTargets() = default;
    virtual std::vector<PlayplayTarget> all() const = 0;           // sorted by sort, then label
    virtual std::optional<PlayplayTarget> get(const std::string& id) const = 0;
    // playplayFetch: the station API, direct (baseUrl, X-PP-Key) or through the node's agent
    // (/api/playplay-proxy?path&method&auth). Returns the parsed JSON body (or {"raw": text}); throws
    // std::runtime_error with Node's texts ("<label> has neither an address nor a paired AN-FS node.",
    // "<label>: node is offline, so its PlayPlay cannot be reached.", HTTP errors).
    virtual JVal fetch(const PlayplayTarget& target, const std::string& pathname, const std::string& method = "GET",
                       const JVal* body = nullptr, int timeoutMs = 15000) = 0;
};

} // namespace anfs
