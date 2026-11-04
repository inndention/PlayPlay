// iface/tls_status.h - the TLS front's state (mod-srv-wan.md; C# SrvWanLanOnly.cs ITlsStatus). FROZEN (ANFS-F0).
// Published by w3-tls; svcGet<ITlsStatus>() is null until then (srv-shares reports no public URL).
#pragma once

#include <string>

namespace anfs {

class ITlsStatus {
public:
    virtual ~ITlsStatus() = default;
    virtual bool active() const = 0;              // the HTTPS listener is up
    virtual std::string publicUrl() const = 0;    // e.g. https://example.invalid, "" when unknown
};

} // namespace anfs
