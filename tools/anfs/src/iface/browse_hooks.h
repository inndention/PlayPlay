// iface/browse_hooks.h - listing flavours, the FsResult value and the browse enrichment hooks (mod-pfs-browse.md;
// C# PeerBrowseOps.cs). FROZEN (ANFS-F0). Header-only: the hook list needs no owning module.
//
// Hooks run only in the Bridge flavour (the server's own listings, srv-local) and from srv-proxy on peer listings:
// first Hide() per entry, then Enrich() once per directory. Orders: archive 10, links 20, wd-feed 30 (the exemptions
// hide lives in wd-feed). Enrich should run one grouped query per directory, never one per item.
#pragma once

#include "core/json.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anfs {

// Peer = agent.js texts ("Outside shared paths.", "Not found."); Bridge = server.patched.js texts
// ("File not found.", "Not a directory.", "Cannot access directory.").
enum class Flavor { Peer, Bridge };

struct FsResult {
    int status = 200;
    JVal body;                                    // the JSON body to send (Node key order)

    static FsResult ok(JVal body) { FsResult r; r.body = std::move(body); return r; }
    static FsResult err(int status, const std::string& text)
    {
        FsResult r; r.status = status; r.body = JVal::object({{"error", JVal(text)}}); return r;
    }
    bool isOk() const { return status == 200; }
};

class IBrowseHook {
public:
    virtual ~IBrowseHook() = default;
    virtual int order() const = 0;
    virtual bool hide(const std::string& nodeId, const std::string& fullPathLower) = 0;
    // items: the listing's entry objects for dir, in listing order; add keys in place.
    virtual void enrich(const std::string& nodeId, const std::string& dir, std::vector<JVal>& items) = 0;
};

// Process-wide hook list, sorted by order (stable for equal orders), thread-safe.
class BrowseHooks {
public:
    static void add(std::shared_ptr<IBrowseHook> h)
    {
        if (!h) return;
        std::lock_guard<std::mutex> lk(mu());
        auto& v = list();
        v.push_back(std::move(h));
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a->order() < b->order(); });
    }
    static void remove(const std::shared_ptr<IBrowseHook>& h)
    {
        std::lock_guard<std::mutex> lk(mu());
        auto& v = list();
        v.erase(std::remove(v.begin(), v.end(), h), v.end());
    }
    static std::vector<std::shared_ptr<IBrowseHook>> all()
    {
        std::lock_guard<std::mutex> lk(mu());
        return list();
    }

private:
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::vector<std::shared_ptr<IBrowseHook>>& list() { static std::vector<std::shared_ptr<IBrowseHook>> v; return v; }
};

} // namespace anfs
