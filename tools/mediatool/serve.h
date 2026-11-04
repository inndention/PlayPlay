// PlayPlayMediaTool `--serve` (docs/plans/inhouse-deps.md §5.4, §6.2, DEP-03): one long-lived process for the many small
// probes and thumbnails of an ingest scan, instead of one ffprobe + one ffmpeg spawn per file. JSON-lines on stdin /
// stdout through ppcli::ServeLoop (tools/ppcommon): N worker threads (default 3), replies in completion order matched by
// id, per-request timeout (the reply {"ok": false, "timeout": true} at the deadline, the job cancelled), ping / quit,
// exit 2 when a request hangs past its grace so the client restarts the tool and marks only the in-flight files failed.
//
// Requests ({"id": .., "op": .., "timeoutMs"?: ..}) and replies (each also carries "id", "ms" and "worker"):
//   {"op": "probe", "path", "format"?: true, "streams"?: true}
//       -> {"ok": true, "probe": <the object ffprobe -print_format json -show_format -show_streams prints>}
//   {"op": "duration", "path"}
//       -> {"ok": true, "duration": <seconds, 0 when unknown>, "value": "2.000000" | "N/A"}   (the -show_entries form)
//   {"op": "thumb", "path", "out", "at"?: <s>, "width"?: 320, "height"?: -2, "q"?: 4, "overwrite"?: true}
//       -> {"ok": true, "out", "width", "height", "srcWidth", "srcHeight", "at", "frameTime", "bytes", "pixFmt", "autorotate"?}
//   {"op": "version"} -> {"ok": true, "tool", "ops", "ffmpeg"}
//   failures: {"ok": false, "error", "averror"?}; a non-file path, an unknown op or a thumbnail whose display rotation
//   the tool does not reproduce (thumbnailer.h): {"ok": false, "unsupported": true}, so the client runs ffmpeg instead.
// A malformed file costs one {"ok": false} reply, never the process (libav errors are returned, not thrown); a crash or
// a wedged decoder is the client's restart case. libav logging is off unless -v asks for it (stderr is diagnostics only).
#pragma once

#include <QJsonObject>
#include <QStringList>

#include <atomic>

namespace mediatool {

struct ServeConfig {
    int workers = 3;
    int timeoutMs = 60000;          // per request unless the request says "timeoutMs"
    int idleExitMs = 0;             // > 0: exit 0 after this long without work
    int maxPending = 256;           // queued + running; more -> {"ok": false, "busy": true}
    int hangGraceMs = 10000;        // a request still running this long after its timeout -> exit 2
    int decoderThreads = 2;         // per thumbnail
    int logLevel = -8;              // AV_LOG_QUIET
};

/// One request, in process (the serve handler; also what tests call). `timeoutMs` is the request's effective timeout,
/// `cancel` the serve loop's cancel flag for it (ServeContext::cancelFlag; null = never): set on a timeout reply or a
/// closed stdout, it stops the probe (AvKit's interrupt callback) and the thumbnailer (callback and decode loop) early.
QJsonObject handleRequest(const QJsonObject& req, int timeoutMs, const std::atomic<bool>* cancel, const ServeConfig& cfg);

/// The `--serve` / `serve` sub-command: --workers N --timeout-ms N --idle-exit-ms N --max-pending N --hang-grace-ms N
/// --threads N (decoder threads per thumbnail) -v <level>. Returns the process exit code (0 EOF / quit / idle, 2 hang).
int serveCli(const QStringList& args);

} // namespace mediatool
