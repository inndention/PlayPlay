// iface/watchdog_status.h - the peer scanner's state for /peer/api/watchdog-status (mod-wd-scan.md). FROZEN
// (ANFS-F0). Implemented by wd-scan (peer half); published with svcSet<IWatchdogStatus>; read by pfs-setup.
#pragma once

#include "core/json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace anfs {

struct WatchdogStatus {
    int64_t lastReportTime = 0;                   // epoch ms of the last successful report, 0 = never
    int64_t entries = 0;                          // entries in that report
    int intervalSec = 60;
    std::vector<std::string> roots;

    // {lastReportTime (number or null), entries, intervalSec, roots} in agent.js order.
    JVal toJson() const
    {
        JVal roots = JVal::array();
        for (const auto& r : this->roots) roots.push(JVal(r));
        return JVal::object({{"lastReportTime", lastReportTime ? JVal(lastReportTime) : JVal()},
                             {"entries", JVal(entries)},
                             {"intervalSec", JVal(intervalSec)},
                             {"roots", roots}});
    }
};

class IWatchdogStatus {
public:
    virtual ~IWatchdogStatus() = default;
    virtual WatchdogStatus status() const = 0;
};

} // namespace anfs
