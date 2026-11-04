// PlayPlayMediaTool graph runner, part 1 (docs/plans/inhouse-deps.md §6.2, DEP-05a): the ffmpeg-argument subset parser.
// The Supervisor hands the tool ffmpeg's own command line (no sub-command) for spot composition and render-farm jobs.
// This parser takes exactly the argument lists those call sites build, enumerated from
//   suites::presetArgs (suitelogic.cpp)            render-farm transcode / split segments / concat (renderfarm.cpp),
//   SpotGenerator::composeStories + headline::storiesFilter (headlinenews.cpp), SpotGenerator::compose (spotgenerator.cpp),
// and nothing else: an argument, value or position outside the subset is "unsupported" (exit 64), so the caller runs
// ffmpeg.exe and never gets a silently different render. A command line ffmpeg itself would refuse (a missing value,
// -y with -n, a -map label no graph produces, a file index past the inputs) is a usage error (exit 2).
//
// The subset:
//   global      -y -n -v|-loglevel <lvl> -hide_banner -nostdin -nostats -progress pipe:1 -filter_complex <graph> (once)
//   per input   -ss <t> -t <t> -loop 0|1 -stream_loop -1 -framerate <rate> -f concat -safe 0, then -i <local file>
//   per output  -map [label] -vf|-filter:v <chain> -af|-filter:a <chain> -vn -an -c copy -c:v libx264|copy|mjpeg
//               -c:a aac|pcm_s16le|copy -preset <x264 preset> -crf <0..51> -g <n> -pix_fmt <fmt> -color_range <tv|pc|...>
//               -r <rate> -b:a <bits> -ar <hz> -ac <n> -movflags +faststart -avoid_negative_ts make_zero
//               -frames:v|-vframes <n> -q:v|-qscale:v <1..31> -t <t>, then one output file (.mp4 .m4v .mov .wav .jpg .jpeg)
// Positions follow ffmpeg: a per-input option applies to the next -i, a per-output option to the output file after the
// last -i. An output option before an -i (ffmpeg would apply it to the input, or refuse), an input option after the last
// -i, anything per-file after the output (ffmpeg ignores trailing options), a second output or a second -filter_complex
// are unsupported rather than reinterpreted. Global options go anywhere.
// Filtergraphs are tokenised like libavfilter's graphparser (quotes, backslashes, [labels]) to check that every filter
// exists in this libavfilter (else unsupported), that every [k:v]/[k:a] reads an input that exists, that every labelled
// output is mapped exactly once and that no output is left for ffmpeg to map on its own. The strings themselves are kept
// verbatim for avfilter_graph_parse_ptr (DEP-05b).
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace mediatool {

struct GraphInput {
    QString path;                 // a local file, an image2 pattern (frame_%04d.png) or a concat list
    double seekSec = -1;          // -ss (input seek); < 0 none
    double durationSec = -1;      // -t (input limit); < 0 none
    bool loop = false;            // -loop 1: the image2 demuxer repeats the still forever
    bool streamLoop = false;      // -stream_loop -1: the input is re-read forever
    double framerate = 0;         // -framerate (image2 sequence rate); 0 = the demuxer's default
    bool concat = false;          // -f concat: `path` is a concat list
    bool safe0 = false;           // -safe 0 (concat: any path in the list)
};

struct GraphOutput {
    QString path;
    QString container;            // from the extension: "mp4" (also .m4v), "mov", "wav", "jpg"
    QStringList maps;             // -map [label], in order; empty = ffmpeg's automatic mapping (-vf/-af forms)
    QString vf, af;               // -vf / -af chains, verbatim
    bool noVideo = false, noAudio = false;   // -vn / -an
    bool copyAll = false;         // -c copy
    QString videoCodec, audioCodec;          // -c:v / -c:a as given; empty = the container's default (see encoder())
    QString preset;               // -preset (libx264)
    int crf = -1, gop = -1;       // -crf, -g; -1 = not given
    QString pixFmt, colorRange;   // -pix_fmt, -color_range
    QString frameRate;            // -r as given ("30"); empty = none
    double frameRateValue = 0;
    qint64 audioBitrate = 0;      // -b:a in bit/s (192k = 192000)
    int sampleRate = 0, channels = 0;        // -ar, -ac
    bool faststart = false;       // -movflags +faststart
    bool makeZeroTs = false;      // -avoid_negative_ts make_zero
    int frames = -1;              // -frames:v
    double qscale = -1;           // -q:v
    double durationSec = -1;      // -t (output); < 0 none

    /// The encoder ffmpeg would pick: "copy", the explicit codec, or the container's default (mp4/mov: libx264 + aac,
    /// wav: pcm_s16le, jpg: mjpeg); empty = no stream of that kind (-vn / -an, a wav's video, a jpg's audio).
    QString videoEncoder() const;
    QString audioEncoder() const;
};

struct GraphCommand {
    bool overwrite = false;       // -y
    bool noOverwrite = false;     // -n (neither: ffmpeg asks; with -nostdin it fails on an existing output)
    bool nostdin = false, hideBanner = false, nostats = false;
    bool progressPipe = false;    // -progress pipe:1: out_time_us= lines on stdout (suites::progressSeconds)
    int logLevel = 32;            // -v; AV_LOG_INFO, ffmpeg's default
    QString filterComplex;        // -filter_complex, verbatim; empty = none
    QStringList graphInputs;      // the [k:v] / [k:a] labels the complex graph reads, in order of first use
    QVector<GraphInput> inputs;
    QVector<GraphOutput> outputs; // exactly one when ok()
    QString unsupported;          // non-empty: outside the subset -> exit 64, the caller runs ffmpeg.exe
    QString error;                // non-empty: ffmpeg would refuse it too -> exit 2
    bool ok() const { return unsupported.isEmpty() && error.isEmpty(); }
    QJsonObject toJson() const;
};

struct GraphParseOptions {
    bool checkFilters = true;     // avfilter_get_by_name for every filter in every graph (unsupported when missing)
};
GraphCommand parseGraphArgs(const QStringList& args, const GraphParseOptions& o = {});

/// One filter of a tokenised filtergraph.
struct FilterNode {
    QStringList inputs, outputs;  // [labels] as written, without the brackets
    QString name;                 // without an @instance suffix
    QString args;                 // the raw text after '=', quotes and escapes kept
};
/// libavfilter graphparser's tokenisation: chains split on ';', filters on ',', quotes ('...') and backslashes honoured,
/// labels in brackets. False with `err` on a syntax error (an empty name, an unterminated label).
bool tokenizeGraph(const QString& graph, QVector<QVector<FilterNode>>* chains, QString* err = nullptr);
/// The filter names in a graph, in order, duplicates kept; empty on a syntax error.
QStringList filterNamesIn(const QString& graph);
/// Whether this libavfilter has the filter (avfilter_get_by_name).
bool filterExists(const QString& name);

/// The `parse` sub-command: prints {"ok": true, "command": {...}} for a command line in the subset, else the
/// unsupported (64) or error (2) line. A diagnostic for operators and the tests; the runner itself is DEP-05b.
int graphParseCli(const QStringList& args);

} // namespace mediatool
