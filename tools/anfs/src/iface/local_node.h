// iface/local_node.h - the server's own machine as node "__local__" (mod-srv-local.md; C# SrvLocalNode.cs).
// FROZEN (ANFS-F0). Implemented by srv-local; published with svcSet<ILocalNode>; used by srv-proxy, srv-shares,
// srv-xfer, buckets, syncjobs, knowledge and wd-scan. Registers no routes itself.
//
// Every method uses Bridge-flavour semantics (no share sandbox; texts "Directory not found.", "File not found.",
// "Not a directory.", "Cannot access directory."). Download* log "DOWNLOAD (OUT)" / "DOWNLOAD DIR (OUT)" and
// uploadComplete logs "UPLOAD (IN)" through ITransferLog. browse() runs the BrowseHooks with nodeId "__local__" and
// adds birthtime.
#pragma once

#include "core/config.h"
#include "core/http_ctx.h"
#include "core/stream.h"
#include "iface/browse_hooks.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anfs {

class ILocalNode {
public:
    virtual ~ILocalNode() = default;

    virtual std::string webroot() const = 0;      // raw.webroot, default C:\webroot
    virtual std::string dropbox() const = 0;
    virtual std::string tempDir() const = 0;      // <data>\temp_uploads

    // Shares with id, name and an existing path (stale ones hidden, not deleted).
    virtual std::vector<Share> shares() const = 0;
    // ConfigStore::update(shares = s), then INodeStore::syncNodeShares("__local__", s) when it runs.
    virtual void saveShares(const std::vector<Share>& s) = 0;
    // Creates the folder and stores it as folders.dropbox.
    virtual void setDropbox(const std::string& path) = 0;
    // webroot, desktop, downloads, dropbox ({id, name, path}); bookmarks filters them to existing paths.
    virtual std::vector<Share> defaultShortcuts() const = 0;

    virtual FsResult drives() = 0;
    virtual FsResult browse(const std::string& rawPath) = 0;
    virtual FsResult browsePick(const std::string& rawPath) = 0;
    virtual FsResult previewText(const std::string& rawPath) = 0;
    virtual FsResult createFolder(const std::string& parent, const std::string& name) = 0;
    virtual FsResult dropboxList() = 0;
    virtual FsResult walk(const std::string& rawPath, int maxEntries) = 0;   // 403 for a hard-denied path

    // Streaming routes answer on the context themselves.
    virtual void previewMedia(http::Ctx& ctx, const std::string& rawPath) = 0;
    virtual void download(http::Ctx& ctx, const std::string& rawPath) = 0;
    virtual void downloadDir(http::Ctx& ctx, const std::string& rawPath) = 0;
    virtual void uploadInit(http::Ctx& ctx) = 0;
    virtual void uploadChunk(http::Ctx& ctx) = 0;
    virtual void uploadComplete(http::Ctx& ctx) = 0;

    // Raw file access for transfers (null + err when missing or denied).
    virtual std::unique_ptr<Source> openRead(const std::string& path, std::string* err = nullptr) = 0;
    virtual int64_t size(const std::string& path) = 0;   // -1 when missing
};

} // namespace anfs
