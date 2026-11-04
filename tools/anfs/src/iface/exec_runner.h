// iface/exec_runner.h - remote command execution (w3-exec: peer POST /api/exec, server POST /api/nodes/{id}/exec for
// "__local__"; agent.js:945-1000, server.patched.js:1061-1092). FROZEN (ANFS-F0). Implemented by w3-exec; published
// with svcSet<IExecRunner>. Routes using it are LanOnly; the peer refuses with 423 unless elevated and appends every
// run to exec-audit.log (never the output).
#pragma once

#include "core/json.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace anfs {

struct ExecRequest {
    std::string command;
    std::string shell = "cmd";                    // "cmd" (cmd /d /s /c) or "powershell" (-NoProfile -NonInteractive -Command)
    std::string cwd;                              // used only when it exists
    int64_t timeoutMs = 120000;

    // Node: min(max(parseInt(timeoutMs, 10) || 120000, 1000), 3600000) (agent.js:970). The caller passes 0 where
    // parseInt gives NaN (absent, non-numeric); only 0 falls back to 120000: a negative value is truthy in JS and
    // clamps to 1000.
    static constexpr int64_t clampTimeout(int64_t requested)
    {
        if (requested == 0) requested = 120000;
        return std::min<int64_t>(std::max<int64_t>(requested, 1000), 3600000);
    }
};

struct ExecResult {
    std::string host;
    std::string shell;
    int64_t exitCode = 0;
    bool timedOut = false;
    int64_t durationMs = 0;
    std::string stdoutText;                       // at most 20 MB (Node maxBuffer)
    std::string stderrText;

    // {host, shell, exitCode, timedOut, durationMs, stdout, stderr} in Node's order.
    JVal toJson() const
    {
        return JVal::object({{"host", JVal(host)},
                             {"shell", JVal(shell)},
                             {"exitCode", JVal(exitCode)},
                             {"timedOut", JVal(timedOut)},
                             {"durationMs", JVal(durationMs)},
                             {"stdout", JVal(stdoutText)},
                             {"stderr", JVal(stderrText)}});
    }
};

class IExecRunner {
public:
    virtual ~IExecRunner() = default;
    virtual bool elevated() const = 0;
    // Runs hidden (no console window), captures output, kills the process tree on timeout.
    virtual ExecResult run(const ExecRequest& request, const std::string& requestedBy) = 0;
};

} // namespace anfs
