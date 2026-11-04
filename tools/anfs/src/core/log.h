// core/log.h - process log. FROZEN (ANFS-F0); implemented by ANFS-F1b (core/log.cpp).
//
// - File: <logsDir>\anfs-YYYYMMDD.log (local date), rotated at midnight, the newest 14 files kept.
// - Never blocks the caller on disk or console I/O: lines go to a bounded in-memory queue drained by one writer thread
//   (the PlayPlay 10-24/10-25 outage was processes blocking on a shared console). When the queue is full the oldest
//   Debug/Info lines are dropped and a "dropped N lines" marker is written.
// - stdout gets only state changes (state()) and Warn/Error, rate-limited to 20 lines per 10 s, because the StarRoot
//   per-component log has no rotation (mgmt-integration.md section 0).
// - Modules log state changes and errors only, never one line per request (CONVENTIONS.md section 3).
// - Secrets never reach the log: every line passes redact(), and registerSecret() literals (agent token, portal
//   password, peer keys) are masked as "[secret]" wherever they appear.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace anfs {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Log {
public:
    // logsDir "" = no file (tests, `anfs status`). The directory is created on first write.
    explicit Log(std::string logsDir = {}, int keepDays = 14);
    ~Log();                                       // flushes the queue (bounded wait, at most 2 s)
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void write(LogLevel level, std::string_view msg);
    void debug(std::string_view msg) { write(LogLevel::Debug, msg); }
    void info(std::string_view msg) { write(LogLevel::Info, msg); }
    void warn(std::string_view msg) { write(LogLevel::Warn, msg); }
    void error(std::string_view msg) { write(LogLevel::Error, msg); }
    // A state change (mode applied, listening, module failed/recovered): Info in the file and also on stdout.
    void state(std::string_view msg);

    void setMinLevel(LogLevel level);
    void setConsole(bool on);                     // stdout echo of state()/Warn/Error (default on for `anfs run`)
    void registerSecret(std::string literal);     // ignored when shorter than 6 characters
    void flush();                                 // waits until queued lines are written (tests)

    std::string lastError() const;                // last Error line (redacted), "" if none
    int64_t lastErrorAtMs() const;                // epoch ms of that line, 0 if none
    int errorCount() const;
    std::string currentFile() const;              // today's file path, "" without a directory
    std::string dir() const;

    // Masks credential-looking values: x-agent-auth / authorization header values, auth= / token= / password= /
    // key= query or JSON values, and Basic/Bearer credentials. Used on every line.
    static std::string redact(std::string_view s);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A Log that discards everything (default for components constructed without one).
Log& nullLog();

} // namespace anfs
