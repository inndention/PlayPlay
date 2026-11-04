// PlayPlayMediaTool graph runner, part 2 (docs/plans/inhouse-deps.md §6.2, DEP-05b): runs a GraphCommand the subset
// parser (argparse.h, DEP-05a) accepted, in-process on libavformat / libavcodec / libavfilter, the way ffmpeg.exe runs
// the same command line:
//  - every -i is opened with libavformat (the image2 demuxer's `loop` / `framerate` for -loop 1 stills and frame
//    sequences; -stream_loop -1 = re-seek to the start on EOF with ffmpeg's timestamp offset; -ss = seek to the keyframe
//    before t then drop / cut what is before t, like ffmpeg's accurate seek; -t = stop the input at t);
//  - only the streams the graph reads are decoded (two decoder threads each); each decoded stream feeds the
//    buffer / abuffer sources that stand for its [k:v] / [k:a] labels, with ffmpeg's autorotate (a display matrix becomes
//    transpose / hflip / vflip) before the user's filters;
//  - the -filter_complex string goes unchanged into avfilter_graph_parse_ptr (so headline::storiesFilter and the compose
//    graphs of spotgenerator.cpp are reused verbatim); -vf / -af become the simple graph of the automatically mapped
//    stream; behind every mapped output ffmpeg's own filters are added: format (the -pix_fmt, else what the encoder
//    takes), aformat (-ar, -ac, the encoder's sample formats) and trim / atrim for the output -t;
//  - the graph is driven as fftools did: avfilter_graph_request_oldest, then the input with the most failed buffersrc
//    requests gets one more packet; frames from the buffersinks go to libx264 / aac / pcm_s16le (or mjpeg) with -preset
//    -crf -g -r -b:a, through ffmpeg's cfr video sync (-r: duplicate / drop to the output rate) into the mp4 / mov / wav
//    muxer with -movflags +faststart and -avoid_negative_ts make_zero;
//  - -c copy on a stream next to an encoded one (the loudnorm preset) is a packet remux of that stream; a command with
//    nothing to encode (the concat join, a copy segment) is the remux of concat.h.
// -progress pipe:1 prints ffmpeg's key=value blocks (frame, fps, bitrate, total_size, out_time_us, out_time_ms,
// out_time, dup_frames, drop_frames, speed, progress=continue|end) on stdout every 500 ms, which suites::progressSeconds
// reads; the ppcli result line still comes last. Everything else (libav messages at the -v level) goes to stderr.
// Defensive defaults: local files only, an optional deadline and a cancel hook through the AVIOInterruptCB and the
// feed loop; the output is written in place (ffmpeg does the same) so a failed run leaves a partial file, which the
// caller deletes like it does after an ffmpeg.exe failure.
#pragma once

#include "argparse.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace mediatool {

struct RunOptions {
    int decoderThreads = 2;            // per decoder (frame + slice)
    int timeoutMs = 0;                 // the whole job; <= 0: none (ffmpeg has none; a transcode may take minutes)
    bool localOnly = true;             // protocol_whitelist=file (the concat demuxer opens its entries through it too)
    std::function<bool()> cancelled;   // polled by the interrupt callback and between packets
    /// Receives each -progress block (ffmpeg's key=value lines, "progress=continue" / "progress=end" last); null = none.
    std::function<void(const QByteArray&)> progress;
    int progressIntervalMs = 500;      // ffmpeg's -stats_period default
};

struct RunResult {
    bool ok = false;
    QString error;
    int averror = 0;
    bool timedOut = false, cancelled = false;
    QString out;                       // the output path as given
    double durationSec = -1;           // the longest stream written (last packet pts + duration)
    qint64 videoFrames = 0;            // encoded or copied video packets
    qint64 audioSamples = 0;           // encoded audio samples (0 for a copied stream)
    qint64 bytes = 0;                  // the output file
    int dupFrames = 0, dropFrames = 0; // the cfr video sync
    QStringList streams;               // "video h264 1920x1080 yuv420p 30/1" / "audio aac 48000 stereo" / "... (copy)"
    double elapsedSec = 0;
};

/// Runs a command the parser accepted (c.ok()); a command with unsupported / error set fails without touching the output.
RunResult runGraph(const GraphCommand& c, const RunOptions& o);
/// The result line: {"ok": true, "out", "duration", "frames", "samples", "bytes", "dup", "drop", "streams", "elapsed"} or
/// {"ok": false, "error", "averror", "timeout"?, "cancelled"?}.
QJsonObject runJson(const RunResult& r);

/// The ffmpeg-style default mode of PlayPlayMediaTool: parse, then run. Exit 0 / 2; 64 (with {unsupported: true}) when
/// the command line is outside the subset so the caller runs ffmpeg.exe.
int graphRunCli(const QStringList& args);

/// ffmpeg's -progress block for a point in the run; `outTimeUs` < 0 prints "N/A" for the out_time keys as ffmpeg does.
QByteArray progressBlock(qint64 frames, double fps, double bitrateKbps, qint64 totalSize, qint64 outTimeUs, int dup, int drop, double speed, bool end);

} // namespace mediatool
