// PlayPlayMediaTool `thumb` (docs/plans/inhouse-deps.md §6.2, DEP-03): the ingest's thumbnail spawn
//   ffmpeg -y -v quiet [-ss <t>] -i <in> -frames:v 1 -vf scale=320:-2 -q:v 4 <out>.jpg     (IngestManager::applyRecord)
// in-process with libavformat / libavcodec / libswscale, following what ffmpeg does for those arguments:
//  - the video stream ffmpeg maps by default (largest picture, +default disposition; attached pictures last);
//  - input -ss: avformat_seek_file to the keyframe before t (3/23 s earlier for streams with B-frame delay, as
//    ffmpeg_demux.c does), then decode and drop frames before t: the JPEG is the first frame at or after t;
//  - autorotate (on by default in ffmpeg): the frame's display matrix (a phone video's rotation, a JPEG's EXIF
//    orientation; else the stream's) becomes transpose / hflip / vflip before the scale, by ffmpeg_filter.c's rule. Done
//    losslessly plane by plane for the formats those filters take as they are; a 90/270 turn of a format with unequal
//    chroma subsampling (4:2:2 ...), a palette format, or an angle that is not a multiple of 90 (ffmpeg's rotate filter)
//    is "unsupported" (exit 64) after decoding, so the caller runs ffmpeg.exe rather than get a different picture;
//  - scale=W:H with the scale filter's size rule (-1 keep aspect, -n keep aspect and round to a multiple of n) and SAR,
//    bicubic, into the MJPEG encoder's best pixel format for the source at full (JPEG) range;
//  - mjpeg at qscale (-q:v): global_quality = FF_QP2LAMBDA * q, the frame's quality set to it, like ffmpeg_enc.c.
// With the FFmpeg 9 build PlayPlay links, the JPEG is byte-identical to ffmpeg.exe's for the same arguments on every
// fixture of test_inhouse_mediatool (H.264 4:2:0 / 4:2:2 10-bit, MPEG-2 480i TS, MPEG-4, anamorphic, full range, stills,
// rotated MP4s, EXIF-oriented JPEGs). The JPEG is written atomically (QSaveFile), so a reader never sees half a file
// under the expected name, and a job cancelled or timed out before the commit leaves none.
// Decoding is copied from MediaPreview's approach (mediapreview.cpp), not linked, so this tool does not depend on a
// file other sessions edit. Defensive defaults: local files only, a deadline and a cancel hook through the
// AVIOInterruptCB and the decode loop, two decoder threads.
//
// The CLI (`thumb` and the ffmpeg-style default mode of PlayPlayMediaTool) takes exactly that argument subset:
//   global -y -n -v/-loglevel <lvl> -hide_banner -nostdin -nostats; -ss <t> before or after -i; one -i <in>;
//   -frames:v 1 | -vframes 1 | -frames 1; -vf | -filter:v scale=<W>:<H> (also w=..:h=..); -q:v | -qscale:v | -q <q>;
//   -an -sn -dn -update 1 -f image2|mjpeg -c:v | -vcodec mjpeg; the output <file>.jpg|.jpeg last.
// The per-file options sit where ffmpeg takes them (-ss once, before or after -i; the output options after -i; nothing
// per-file after the output, which ffmpeg would ignore); the global ones (-y -n -v ...) anywhere.
// Anything else exits 64 ("unsupported: <arg>") so the caller runs ffmpeg.exe instead. Result: the one-line JSON
// {"ok": true, "out", "width", "height", "srcWidth", "srcHeight", "at", "frameTime", "bytes", "pixFmt", "autorotate"?}
// / {"ok": false, "error"} on stdout, exit 0 / 2 (64 for a rotation not reproduced, found after decoding).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace mediatool {

struct ThumbOptions {
    double seekSec = -1;            // -ss (input seek, then the first frame at or after it); < 0: none
    int width = 320, height = -2;   // scale=W:H
    double qscale = 4;              // -q:v
    bool overwrite = true;          // -y; false = ffmpeg -n: fail when the output exists
    int threads = 2;                // decoder threads (frame + slice)
    int timeoutMs = 120000;         // whole job (open, seek, decode, encode); <= 0: none
    bool localOnly = true;          // protocol_whitelist=file, also for nested opens
    std::function<bool()> cancelled;   // polled by the interrupt callback and the decode loop
};

struct ThumbResult {
    bool ok = false;
    QString error;
    int averror = 0;
    bool timedOut = false, cancelled = false;
    bool unsupported = false;           // ffmpeg would do something this tool does not reproduce: run ffmpeg (exit 64)
    int width = 0, height = 0;          // the JPEG
    int srcWidth = 0, srcHeight = 0;    // the decoded frame (before autorotate)
    QString autorotate;                 // what the display matrix asked for, as ffmpeg's filters: "transpose=clock",
                                        // "hflip", "vflip", "hflip,vflip"; empty = none
    int stream = -1;                    // the input stream used
    double frameSec = -1;               // the chosen frame's time from the file start (pts - start_time)
    qint64 bytes = 0;
    QString pixFmt;                     // what the encoder got, e.g. "yuv420p" (full range)
};

ThumbResult makeThumbnail(const QString& in, const QString& out, const ThumbOptions& o);
/// The result line: {"ok": true, "out" (native separators), "width", "height", "srcWidth", "srcHeight", "at",
/// "frameTime", "bytes", "pixFmt", "autorotate"? (only when one was applied)} or {"ok": false, "error", "averror",
/// "timeout"?, "cancelled"?}, or ppcli's {"ok": false, "error": "unsupported: ...", "unsupported": true}.
QJsonObject thumbJson(const ThumbResult& r, const QString& out, const ThumbOptions& o);

/// The scale filter's size rule (libavfilter scale_eval.c ff_scale_eval_dimensions): W/H -1 keep the aspect ratio,
/// -n keep it and round to a multiple of n (av_rescale, round to nearest); both negative = the input size.
bool scaleSize(int inW, int inH, int reqW, int reqH, int* outW, int* outH);

struct ThumbCommand {
    QString in, out;
    ThumbOptions opt;
    int logLevel = 32;              // AV_LOG_INFO, ffmpeg's default
    QString unsupported;            // -> exit 64
    QString error;                  // usage error -> exit 2
};
ThumbCommand parseThumbArgs(const QStringList& args);

/// The `thumb` sub-command and the ffmpeg-style default mode: prints the result line, returns 0 / 2 / 64.
int thumbCli(const QStringList& args);

} // namespace mediatool
