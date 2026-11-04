#pragma once
// Deterministic filtergraph builder (ST-18a): a Timeline (compose/timeline.h) → the ffmpeg steps that render it.
// Port of StarMedia mcp/spot/compose.ts renderComposition with the I/O taken out:
//   1. one SEGMENT per slot (each clip, then the end card), normalised to the same size, fps and pixel format (in
//      limited range unless PlanOptions::limitedRange is off: compose.ts lets a JPEG make the spot full range), of
//      length slot + the overlap of the transition into the next slot: stills through zoompan (Ken Burns presets or a
//      keyframed move, on a 1.5x / 2x upscaled frame so the move steps below one output pixel), clips looped and
//      fps-converted, colour cards from the color source; headlines drawn with drawtext from text files;
//   2. one JOIN pass: segments re-stamped to one time base, joined with xfade (fade, dissolve, fadeblack, slideleft,
//      wipeleft, zoomin) or concat for cuts, captions drawn on the joined timeline inside the safe area, cloned / trimmed
//      to the exact length; voice parts placed with adelay and mixed, the music bed looped, faded and ducked with
//      sidechaincompress, loudness normalised (-16 / -24 LUFS), padded and trimmed to the same exact length;
//   3. a POSTER frame from the finished file.
// Every number is formatted as JavaScript prints it and computed in the same order, so for a ComposeSpec the argument
// lists, graph text and drawtext files are byte-identical to compose.ts (test_studio_compose compares them with
// golden/compose-golden.json, recorded from the real TS). The ads/types.ts extras (srcStart / speed / focus / move /
// clip sound / audio "from" / master fades / per-clip transition length) only add text when they are used.
//
// Pure apart from PlanOptions::probe: no files are written and nothing is run. The renderer (ST-18b) stages the font
// and the stills (Step::staged), writes Step::texts and the join graph (Step::graphFile) into the work dir, runs each
// step's args with the common prefix (-hide_banner -nostdin -y -loglevel error, plus -progress pipe:1 -nostats when
// Step::progress) and cwd = the work dir; segments are independent of each other (run them in parallel), the join
// needs all of them, the poster needs the final file. PlanOptions::probe answers "can this file be read" (media::probe
// in the renderer; each path is probed at most once per plan); without it every file counts as readable. With a probe
// that reads files buildPlan BLOCKS: call it off any event-loop thread.
//
// Inputs: validate the Timeline first (local absolute paths only). With PlanOptions::fileOnly (the default) every -i
// also gets -protocol_whitelist file, so even a local playlist (.m3u8, .ffconcat) cannot pull a URL. One plan has one
// join that opens every segment and sound at once; above kMaxJoinInputs inputs or kMaxJoinCommandChars characters of
// arguments buildPlan refuses (Plan::error) and the caller splits the timeline into parts.
//
// Two shapes beyond compose.ts (ST-18b; the defaults keep the compose.ts plan byte for byte):
//   - TWO LOUDNESS PASSES (PlanOptions::loudnessPasses = 2, when there is sound and loudness is on): a MIX step renders
//     the programme's sound alone to mix.wav (no loudnorm; the master fades are in it), a MEASURE step runs loudnorm's
//     analysis on it (the JSON lands in the log at -loglevel info), and the consumer of mix.wav carries kLoudnormToken
//     where the renderer puts loudnormSecondPass(measure): a linear gain to the exact target when the true peak and
//     the range allow it (loudnorm falls back to its dynamic mode otherwise, now with the measured start values).
//   - SEGMENT-PARALLEL JOIN (PlanOptions::partSeconds > 0, film scale): the output frames are cut into windows of about
//     partSeconds at slot boundaries (the frame where the transition into a slot is over, so no transition is split);
//     each PART joins only its run of segments exactly as the single join does (same xfade / concat chain, offsets
//     relative to its first slot), clone-pads, keeps its window by frame index, draws the captions of its window on
//     absolute times and encodes the picture alone with the join's encoder settings. The MIX renders the sound for
//     the whole programme (so loudness is measured over all of it, as R128 asks), and a CONCAT step joins the parts
//     with the concat demuxer (-c:v copy) and encodes the sound: the frame count is the single join's exactly. Parts
//     are independent of each other and need only their segments, so they run while later segments still render.
#include "compose/timeline.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace studio::compose {

struct SourceInfo {
    bool video = true;               // a picture can be decoded (compose.ts probe(...).hasVideo)
    bool audio = true;               // it has a sound stream (needed for Clip::audioGain)
};
using ProbeFn = std::function<SourceInfo(const QString& path)>;

struct PlanOptions {
    /// Prefix for the outputs compose.ts names by absolute path (segments, out.mp4); "" = bare names (the step runs
    /// with cwd = the work dir either way).
    QString workDir;
    QString outFile;                 // the final file the poster is taken from ("" = the join output)
    QString posterFile;              // the poster PNG ("" = <workDir>/poster.png)
    /// drawtext font option: "fontfile=font.ttf" once the renderer staged a font in the work dir, else fontconfig's
    /// default (compose.ts stageFont fallback).
    QString font = QStringLiteral("font=Sans");
    ProbeFn probe;
    /// "-protocol_whitelist file" before every -i (nested opens inherit it). Off only to compare with compose.ts, which
    /// has no such option (the golden test).
    bool fileOnly = true;
    /// Every segment leaves its graph in limited (TV) range: "scale=out_range=tv,format=yuv420p" where compose.ts has
    /// "format=yuv420p". A JPEG still or logo (or a full-range clip) decodes as full range, format=yuv420p keeps the
    /// range, and through the join the whole spot came out full range (yuvj420p, pc), which playout and broadcast
    /// chains read as limited (crushed blacks, clipped whites). In the graph, not as an encoder option, so it does not
    /// depend on the ffmpeg CLI's range negotiation. Off only to compare with compose.ts (the golden test).
    bool limitedRange = true;
    /// > 0: segment-parallel join in windows of about this many seconds (Plan::parts), when the timeline has a slot
    /// boundary to cut at; 0 = one join (compose.ts).
    double partSeconds = 0;
    /// 2 = mix, measure, then a linear second loudnorm pass (Plan::mix / measure); 1 = compose.ts's single pass.
    int loudnessPasses = 1;
};

/// One join opens every input at once: at most this many files (segments + sound) ...
constexpr int kMaxJoinInputs = 256;
/// ... and this many characters of arguments (Windows' CreateProcess takes 32,767 for the whole command line).
constexpr qsizetype kMaxJoinCommandChars = 30000;

struct TextFile { QString name; QString text; };     // drawtext textfile= (work-dir relative, UTF-8)
struct StagedFile { QString from; QString name; };   // copy `from` into the work dir as `name` before the step

struct Step {
    /// Join = the single join (compose.ts); Concat = the parts' join (plan.join in parts mode); Mix / Measure = the
    /// programme's sound and its loudness analysis; Part = one window of the segment-parallel join.
    enum class Kind { Segment, EndCard, Join, Poster, Mix, Measure, Part, Concat };
    Kind kind = Kind::Segment;
    int slot = -1;                   // slot index (Segment, EndCard); part index (Part)
    QStringList args;                // ffmpeg arguments after the common prefix
    QString graph;                   // the filtergraph (inline in args for segments; graph.txt for the join)
    QString graphFile;               // "graph.txt" for the join: write `graph` there (passed with -/filter_complex)
    QString output;                  // the file the step writes (its last argument; "" for Measure, which writes nothing)
    double seconds = 0;              // expected length of the output (progress)
    bool progress = false;           // compose.ts asks for -progress on the join only
    QVector<TextFile> texts;
    QVector<StagedFile> staged;
    // ST-18b
    int first = -1, last = -1;       // Part: the segments it joins (slot indices, inclusive)
    qint64 frameStart = 0, frames = 0;   // Part: its window of output frames
    QString logLevel;                // "" = error (compose.ts); "info" for Measure (loudnorm prints its JSON at info)
    bool loudnessToken = false;      // `graph` holds kLoudnormToken: replace it before writing graphFile
};

/// One slot of the timeline (a clip or the end card).
struct Slot {
    QString kind;                    // "image" | "video" | "color" (as asked, before any fallback) | "endcard"
    double start = 0, duration = 0;  // slot on the output timeline
    double overlap = 0;              // overlap of the transition into the next slot (0 for cuts and the last slot)
    Transition transition = Transition::Cut;
    QString label;                   // clip text or end-card brand / cta (raw)
};

struct Plan {
    bool ok = false;
    QString error;                   // "nothing to render: no scenes" | "too many inputs for one join ..." (no steps then)
    int width = 0, height = 0;
    double fps = 30;
    double natural = 0;              // sum of the slots
    double total = 0;                // exact output length
    QVector<Slot> spans;                // one per slot ("slots" is a Qt keyword)
    QVector<Step> segments;          // one per slot, independent
    Step join;                       // the single join, or the parts' Concat
    Step poster;
    QStringList warnings;            // de-duplicated, in order
    // ST-18b
    qint64 frames = 0;               // output frames: ceil(total * fps) (what trim=duration keeps); the parts add up to it
    QVector<Step> parts;             // segment-parallel join windows, in order (empty = one join)
    Step mix;                        // Kind::Mix when the sound is rendered alone (parts, or two loudness passes)
    Step measure;                    // Kind::Measure with two loudness passes
    LoudnessTarget loudness;         // the target of the loudnorm pass(es)
    bool loudnessOn = false;         // there is sound and loudness is not off
    int loudnessPasses = 1;          // 2 only when it applies (loudnessOn)

    bool split() const { return !parts.isEmpty(); }
    bool hasMix() const { return mix.kind == Step::Kind::Mix; }
    bool twoPass() const { return measure.kind == Step::Kind::Measure; }
    /// compose.ts ComposeResult.timeline: [{start, end, kind, label?}] (r2 times, label cleaned, <= 60 chars).
    QJsonArray timelineJson() const;
    /// Every step in run order: mix, measure, segments, parts, join, poster (those that exist). Dependencies: measure
    /// after mix; part k after segments first..last; the join after every segment (single) or every part (concat) and
    /// after the mix / measure it reads; the poster after the join (and the copy of its output to outFile).
    QVector<const Step*> steps() const;
};

Plan buildPlan(const Timeline& t, const PlanOptions& o = {});

// ---------------------------------------------------------------- loudness (ST-18b)

/// Where a two-pass consumer's graph takes the second loudnorm pass (see Step::loudnessToken).
inline constexpr char16_t kLoudnormToken[] = u"{LOUDNORM}";
/// One pass: "loudnorm=I=-16:TP=-1.5:LRA=11" (the compose.ts strings for social / broadcast).
QString loudnormFilter(const LoudnessTarget& t);
/// The analysis pass: loudnormFilter + ":print_format=json".
QString loudnormMeasureFilter(const LoudnessTarget& t);

/// What loudnorm's analysis printed (input_i / input_tp / input_lra / input_thresh / target_offset).
struct LoudnessMeasure {
    bool ok = false;
    QString error;
    double i = 0, tp = 0, lra = 0, thresh = 0, offset = 0;   // -inf for silence
    QJsonObject toJson() const;                              // {i, tp, lra, thresh, offset} (null for -inf)
};
/// The last JSON block loudnorm printed into an ffmpeg log (print_format=json, -loglevel info).
LoudnessMeasure parseLoudnormLog(QStringView log);
/// The second pass for a measured mix: linear=true with the measured values (clamped to loudnorm's option ranges).
/// "anull" when there was nothing to measure (silence: input_i is -inf), so silence stays silence.
QString loudnormSecondPass(const LoudnessTarget& t, const LoudnessMeasure& m);

// ---------------------------------------------------------------- pieces (exported for tests and ST-18b)

/// xfade transition name ("" for cut): fade, dissolve, fadeblack (dip), slideleft (slide), wipeleft (wipe), zoomin (zoom).
QString xfadeName(Transition t);
struct ZoomExpr { QString z, x, y; };
/// zoompan expressions of a Ken Burns preset over `frames` frames (Auto is resolved by the caller).
ZoomExpr kenburnsExpr(KenBurns kb, int frames);
/// zoompan expressions of a keyframed move over `frames` frames.
ZoomExpr moveExpr(const ClipMove& m, int frames);
/// Normalise `inLabel` to w x h: cover = scale up + crop (centred on the focus point), contain = the whole picture on a
/// blurred cover copy of itself.
QString fitChain(const QString& inLabel, int w, int h, Fit fit, const QString& outLabel, double focusX = 0.5, double focusY = 0.5);
/// One voice part: resampled to 48 kHz stereo float, optional seek / tempo / cut with fade / volume, delayed to absStart.
QString audioChain(const QString& in, const AudioPlacement& a, double absStart, const QString& label);

} // namespace studio::compose
