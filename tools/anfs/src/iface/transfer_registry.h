// iface/transfer_registry.h - live transfer jobs (/api/transfers; mod-srv-xfer.md; C# SrvXferRegistry.cs). FROZEN
// (ANFS-F0). Implemented by srv-xfer; published with svcSet<ITransferRegistry>; used by buckets, syncjobs and
// zip-send.
//
// Job JSON (key order): id, file, srcNode, dstNode, size, bytesSent, speed, status ("active"|"done"|"error"|
// "partial"), error, [batch: fileCount, filesDone, filesFailed, currentFile, truncated (array or null)], startedAt,
// endedAt (null while active). srcNode/dstNode are labels (IAgentRegistry::nodeLabel). speed =
// round(bytesSent / elapsed seconds). Prune on each new job: non-active jobs 5 min after endedAt; cap 200 (oldest
// startedAt first). Times from anfs::clock().
#pragma once

#include "core/json.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anfs {

class ITransferJob {
public:
    virtual ~ITransferJob() = default;
    virtual std::string id() const = 0;
    virtual std::string status() const = 0;
    virtual void addBytes(int64_t n) = 0;         // updates bytesSent and speed
    virtual void setSize(int64_t size) = 0;
    virtual void setCurrentFile(const std::string& name) = 0;
    virtual void setFileCount(int64_t n) = 0;
    virtual void setTruncated(const std::vector<std::string>& names) = 0;
    virtual void fileDone() = 0;
    virtual void fileFailed() = 0;
    virtual void done() = 0;                      // status "done", endedAt
    virtual void fail(const std::string& message) = 0;   // status "error"
    // Any final status ("done", "error", "partial"); fullBytes sets bytesSent = size.
    virtual void finish(const std::string& status, const std::string& error, bool fullBytes) = 0;
    virtual JVal toJson() const = 0;
};

using TransferJobPtr = std::shared_ptr<ITransferJob>;

class ITransferRegistry {
public:
    virtual ~ITransferRegistry() = default;
    // Creates and registers an active job (prunes first). batch adds the batch keys to its JSON.
    virtual TransferJobPtr start(const std::string& id, const std::string& file, const std::string& srcNode,
                                 const std::string& dstNode, bool batch = false) = 0;
    virtual void prune() = 0;
    virtual TransferJobPtr get(const std::string& id) const = 0;
    virtual std::vector<TransferJobPtr> all() const = 0;           // newest first
};

} // namespace anfs
