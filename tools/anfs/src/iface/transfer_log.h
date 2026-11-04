// iface/transfer_log.h - the transfer log (table transfer_log, owner srv-xfer; mod-srv-xfer.md; C# SrvXferLog.cs).
// FROZEN (ANFS-F0). Published with svcSet<ITransferLog>; peers' modules and wave 2 (thumbs, transcripts, wd-feed)
// use it. Entries are JSON objects in Node's key order:
//   {id, ts, timestamp, action, file, path, size, srcNode, dstNode, initiatedBy, userAgent, thumbnail, transcript}
#pragma once

#include "core/json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace anfs {

class ITransferLog {
public:
    virtual ~ITransferLog() = default;
    // Returns the new log id (ids::logId form), or "" when the entry could not be written. Never throws.
    virtual std::string log(const std::string& action, const std::string& file, const std::string& path, int64_t size,
                            const std::string& srcNode, const std::string& dstNode, const std::string& initiatedBy,
                            const std::string& userAgent) = 0;
    // (logId, path), raised after the entry is committed, on the logging thread: subscribers queue work, never block.
    virtual int subscribeLogged(std::function<void(const std::string& logId, const std::string& path)> cb) = 0;
    virtual void unsubscribeLogged(int id) = 0;
    // nullopt leaves that field unchanged.
    virtual void setEnrichment(const std::string& logId, const std::optional<JVal>& thumbnail,
                               const std::optional<JVal>& transcript) = 0;
    virtual JVal entry(const std::string& id) const = 0;           // null when unknown
    virtual JVal all() const = 0;                                  // array, newest first
};

} // namespace anfs
