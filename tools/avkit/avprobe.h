// PlayPlayAvKit probe (docs/plans/inhouse-deps.md §6.1, DEP-01): ffprobe's
//   -v quiet -print_format json -show_format -show_streams <file>
// in-process, plus a header-only duration probe.
//
// Output contract: the JSON ffprobe 9 prints for the same file, the same keys in the same order, numbers and strings as
// ffprobe types them ("duration", "size", "bit_rate", "sample_rate", "nb_frames" are strings; index/width/height/channels
// are numbers; absent values are left out as ffprobe's JSON writer does). Every key IngestManager::applyRecord and the
// app's MetadataService read is covered and compared with ffprobe.exe by test_inhouse_avkit. Not reproduced (yet):
// the -show_data_hash / -count_frames / -analyze_frames extras, and the side-data detail of types other than display
// matrix, CPB properties, mastering display, content light level and audio service type (those print only
// "side_data_type"). The MediaTool (DEP-03) prints `text` as its stdout.
//
// Trust: in-process probing is for trusted, cheap work (capabilities, the app's importer, files the operator picked).
// Bulk probing of untrusted files belongs in the PlayPlayMediaTool process, so a malformed file can never take down the
// on-air Supervisor (§8 "Ingest crash isolation"). Defaults are defensive: local files only (also for nested opens, so a
// playlist on disk cannot make this process fetch URLs), a deadline, a cancel flag, no frame threads.
//
// Thread-safe: every call owns its contexts. libav's own warnings still go to av_log (the host's log level applies).
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <atomic>

namespace avkit {

struct ProbeOptions {
    bool showFormat = true;         // -show_format
    bool showStreams = true;        // -show_streams
    // ffprobe binds a decoder to every stream (no frame is decoded): coded_width/height, bits_per_raw_sample,
    // max_bit_rate and the decoder's exported fields (is_avc, nal_length_size, AC-3 downmix levels) come from it.
    // Without it those values come from the stream parameters or are left out. One thread per decoder either way.
    bool openDecoders = true;
    bool localOnly = true;          // protocol_whitelist=file (also for nested opens: HLS/concat lists, image sequences)
    int timeoutMs = 20000;          // deadline for open + stream info + decoder binding (AVIOInterruptCB); <= 0: none
    const std::atomic<bool>* cancel = nullptr;   // set from another thread to abort (ProbeResult::cancelled)
    QString printName;              // format.filename; empty = the path exactly as given (ffprobe prints its argument)
};

struct ProbeResult {
    bool ok = false;
    int averror = 0;                // the libav error when !ok (AVERROR(ENOENT), AVERROR_INVALIDDATA, AVERROR_EXIT ...)
    bool timedOut = false, cancelled = false;
    QString error;                  // "cannot open <path>: No such file or directory", "timeout after 20000 ms" ...
    QJsonObject json;               // {"streams": [...], "format": {...}}: what QJsonDocument::fromJson(ffprobe stdout) gives
    QByteArray text;                // the same in ffprobe's layout: key order, 4-space indent, its string escaping, '\n'
                                    // line ends (ffprobe.exe writes "\r\n" on Windows; JSON strings never hold a raw '\n').
                                    // On failure ffprobe's "{\n\n}\n".
};

/// The full probe. Never throws; on failure ok=false with error/averror set and text = ffprobe's empty object.
ProbeResult probe(const QString& path, const ProbeOptions& opts = {});
/// probe(path, opts).json, or {} on failure (error set when given).
QJsonObject probeJson(const QString& path, const ProbeOptions& opts = {}, QString* error = nullptr);

/// Seconds, from the container header; stream info is read only when the header has no duration (the drag-and-drop
/// quickDurationSec rule in supervisorcore_library.cpp). 0 when unknown or unreadable. Local files only, with a deadline.
double durationOnly(const QString& path, int timeoutMs = 10000, QString* error = nullptr);

/// The value `ffprobe -v quiet -show_entries format=duration -of default=nw=1:nk=1 <file>` prints for a probe result:
/// "10.000000", or "N/A" when the container has no duration (or the probe failed).
QByteArray formatDurationValue(const ProbeResult& r);

} // namespace avkit
