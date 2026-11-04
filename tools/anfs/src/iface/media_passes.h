// iface/media_passes.h - background passes run by media-lib's loop after its own index/FTS/prune passes
// (mod-media-lib.md; thumbs registers the thumbnail pass, transcripts the transcript pass). FROZEN (ANFS-F0).
// Header-only list, like BrowseHooks. A pass does a bounded amount of work per call (thumbs 20, transcripts 1), keeps
// each database transaction short, and returns promptly when stop becomes true.
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace anfs {

class IMediaPass {
public:
    virtual ~IMediaPass() = default;
    virtual std::string name() const = 0;
    virtual void run(const std::atomic<bool>& stop) = 0;
};

class MediaPasses {
public:
    static void add(std::shared_ptr<IMediaPass> p)
    {
        if (!p) return;
        std::lock_guard<std::mutex> lk(mu());
        list().push_back(std::move(p));
    }
    static void remove(const std::shared_ptr<IMediaPass>& p)
    {
        std::lock_guard<std::mutex> lk(mu());
        auto& v = list();
        v.erase(std::remove(v.begin(), v.end(), p), v.end());
    }
    static std::vector<std::shared_ptr<IMediaPass>> all()
    {
        std::lock_guard<std::mutex> lk(mu());
        return list();
    }

private:
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::vector<std::shared_ptr<IMediaPass>>& list() { static std::vector<std::shared_ptr<IMediaPass>> v; return v; }
};

} // namespace anfs
