// iface/bucket_store.h - bucket targets for /api/transfer-batch with bucketId (mod-srv-xfer.md; C# SrvXferBatch.cs).
// FROZEN (ANFS-F0). Defined for srv-xfer, implemented by buckets (svcGet<IBucketStore>() is null until then: 400
// "No targets configured for this bucket.").
#pragma once

#include <string>
#include <vector>

namespace anfs {

struct BucketTarget {
    std::string nodeId;
    std::string folderPath;
};

class IBucketStore {
public:
    virtual ~IBucketStore() = default;
    virtual std::vector<BucketTarget> targets(const std::string& bucketId) const = 0;
};

} // namespace anfs
