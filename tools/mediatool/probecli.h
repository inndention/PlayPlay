// PlayPlayMediaTool `probe` (docs/plans/inhouse-deps.md §6.2, DEP-03): the two ffprobe invocations PlayPlay spawns,
// answered byte for byte from PlayPlayAvKit's in-process probe:
//   -v quiet -print_format json -show_format -show_streams <file>       IngestManager::pump (ingestmanager.cpp)
//   -v error -show_entries format=duration -of default=nw=1:nk=1 <file>  RenderFarm::probeDuration (renderfarm.cpp)
// stdout and the exit code are ffprobe's: the JSON document (or "{\r\n\r\n}\r\n" when the input cannot be opened) /
// the duration value ("2.000000" or "N/A"), "\r\n" line ends as ffprobe.exe's text-mode stdout writes them, exit 1 on
// failure with ffprobe's "<file>: <libav error>" line on stderr (under -v error and louder). Also taken, because they print
// the same: -show_format or -show_streams alone, -of for -print_format, "-i <file>", -hide_banner, any -v level, and the
// default writer with or without its wrappers / keys (nw, nk, noprint_wrappers, nokey = 0|1).
// Anything else (another writer, -show_packets, -count_frames, other -show_entries ...) exits 64 with the in-house
// {"ok": false, "error": "unsupported: <arg>", "unsupported": true} line, and the caller runs ffprobe instead (§5.2).
// Inputs are local files only (AvKit's protocol_whitelist=file): a URL / "concat:" / "pipe:" input is unsupported (64)
// rather than silently refused.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace mediatool {

/// libav log level for an ffmpeg / ffprobe "-v" / "-loglevel" value: quiet panic fatal error warning info verbose debug
/// trace, or a number. False for anything else (the "repeat+" / "level+" prefixes are not taken).
bool parseLogLevel(const QString& v, int* level);

/// True for an input that is not a plain local path: "<scheme>:" with a scheme of two or more characters other than
/// "file" (http://, concat:, pipe:, subfile, ...) or "-" (stdin). "C:\x" and "\\server\share\x" are paths.
bool isNonFileInput(const QString& in);

/// Deadline for one-shot work (open + stream info + decode): $PLAYPLAY_MEDIATOOL_TIMEOUT_MS, default 120 s. ffprobe has
/// none; this only stops a wedged read (a dead network share) from holding the caller's queue forever.
int oneShotTimeoutMs();

struct ProbeCommand {
    enum Kind { Json, Duration };
    Kind kind = Json;
    QString input;
    bool showFormat = false, showStreams = false;     // Json
    bool noWrappers = false, noKey = false;           // Duration: the default writer's nw / nk
    int logLevel = 32;                                // AV_LOG_INFO, ffprobe's default
    QString unsupported;                              // an argument outside the supported forms -> exit 64
    QString error;                                    // a usage error (missing value, no input, two inputs) -> exit 2
};
ProbeCommand parseProbeArgs(const QStringList& args);

struct ProbeRun {
    QByteArray out;          // ffprobe's stdout for the command (CRLF line ends on Windows)
    int exitCode = 0;        // 0, or 1 when the input could not be probed (ffprobe's code)
    int averror = 0;
    QString error;           // "<file>: <libav error>" (ffprobe's print_error line) when exitCode != 0
};
ProbeRun runProbe(const ProbeCommand& c, int timeoutMs);

/// The whole `probe` sub-command (args after "probe"): writes stdout, returns the exit code.
int probeCli(const QStringList& args);

} // namespace mediatool
