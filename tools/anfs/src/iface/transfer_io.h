// iface/transfer_io.h - open a transfer source and write a destination on any node (local or peer, direct or
// relayed). mod-srv-xfer.md "TransferIo"; C# SrvXferIo.cs. FROZEN (ANFS-F0). Implemented by srv-xfer; published with
// svcSet<ITransferIo>; used by buckets, syncjobs and zip-send. Streams are never buffered whole.
#pragma once

#include "core/stream.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace anfs {

// A failure with the HTTP status the route should answer (403 "Outside shared paths.", 404 "Source file not found.",
// 404 "Source agent offline.", ...).
class XferError : public std::runtime_error {
public:
    XferError(int status, const std::string& what) : std::runtime_error(what), status(status) {}
    int status = 500;
};

struct XferSource {
    std::unique_ptr<Source> stream;               // file or peer response body
    int64_t size = 0;                             // 0 when the peer sent no length
};

struct XferWriteResult {
    std::string path;                             // final local path, or "<dstHostname>:<dstDir>\<name>" for a peer
    bool skipped = false;                         // conflict "skip" and the file existed
};

class ITransferIo {
public:
    virtual ~ITransferIo() = default;
    // agentId "" or "__local__" = this machine. batchStyle selects the folder/bucket flavour texts
    // ("source returned N" vs "Source returned N").
    virtual XferSource openSource(const std::string& agentId, const std::string& absPath, bool batchStyle,
                                  const std::function<bool()>& cancelled) = 0;
    // Writes src to dstDir\relDir\fileName on the destination: local ConflictName/overwrite/skip, or
    // POST /api/transfer-receive?dstDir&fileName[&relPath][&conflict] (octet-stream, chunked when the size is unknown).
    // conflict: "" (rename), "overwrite" or "skip". progress(bytesSoFar).
    virtual XferWriteResult writeDest(const std::string& dstAgentId, const std::string& dstDir,
                                      const std::string& relDir, const std::string& fileName, XferSource& src,
                                      const std::function<void(int64_t)>& progress, const std::string& conflict,
                                      bool batchStyle, const std::function<bool()>& cancelled) = 0;
};

} // namespace anfs
