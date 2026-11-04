#pragma once
// Stills (ST-17): a small set of good, diverse frames out of a reference video, plus the local scene-cut detector,
// key frames and single-frame grabs. C++ port of StarMedia's browser code:
//   - src/studio/stills.ts (pure arithmetic over tiny RGB thumbnails): sample plan, frame measures (sharpness, exposure,
//     distance, stability), score, scene cuts from the scan's own thumbnails, still selection, contact-sheet geometry,
//     vision batches, fmtT. Same constants and the same rules; deterministic, so test cases port one to one.
//   - src/studio/scrub.ts (the thin layer that feeds it frames): scrubVideo() here, with libav (media/frames.h) in place
//     of WebCodecs + OffscreenCanvas.
//   - src/ads/ingest.ts detectScenes / keyFrames / frameAsPng: detectScenes(), keyFrames(), grabFrame().
// Not ported here: cast grouping, remake spans, still summaries and pruning (stills.ts L350-583). They work on the
// studio's Still records with the vision model's labels and belong with the spot / film models that own those records.
//
// The pure functions are cheap and thread-safe. scrubVideo / detectScenes / keyFrames / grabFrame decode video: run them
// on a worker (the *Async variants), never on the event loop.
#include <QByteArray>
#include <QByteArrayView>
#include <QFuture>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

class QThreadPool;

namespace studio::media {

// ---------------------------------------------------------------- options (stills.ts SCRUB_DEFAULTS / scrubOptions)

struct ScrubOptions {
    double fps = 2;                // samples per second of video
    int maxSamples = 900;          // hard cap on frames looked at (the step grows for long videos)
    int keep = 24;                 // stills to keep
    double minGap = 0.75;          // seconds between kept stills
    double minDistance = 0.06;     // 0..1 thumbnail distance below which two frames are near-duplicates
    int thumbW = 80, thumbH = 45;  // signature thumbnail
    int maxSide = 1280;            // longest side of a kept still JPEG
    double jpegQuality = 0.85;
};
/// Defaults filled in and nonsense clamped exactly like scrubOptions(): fps 0.25..8, maxSamples 20..4000, keep
/// 1..120, minGap 0..60, minDistance 0..1, thumbW 16..320, thumbH 9..320, maxSide 128..4096, jpegQuality 0.3..1.
/// Missing members and non-numbers (NaN, "x") take the default; integers are rounded.
ScrubOptions scrubOptions(const QJsonObject& partial = {});
/// The same clamping for options built in C++ (NaN members take the default).
ScrubOptions clampOptions(const ScrubOptions& o);
QJsonObject toJson(const ScrubOptions& o);

// ---------------------------------------------------------------- sample plan

struct SceneSpan { double start = 0, end = 0; };
struct Scene { double start = 0, end = 0, midpoint = 0; };

struct SamplePlan { std::vector<double> times; double step = 0; };
/// Uniform at 1/fps (the step grown so the count stays <= maxSamples, scene points included in the budget), plus every
/// scene's start + 0.15 s and midpoint; always ~0.05 s and duration - 0.05 s; sorted, unique (merged within 1e-3 s),
/// all within [0, duration). A zero / NaN / infinite duration yields no times; a tiny one a single time.
SamplePlan samplePlan(double duration, double fps, int maxSamples, const std::vector<SceneSpan>& scenes = {});
/// Index of the scene containing t (start <= t < end); the nearest scene when t is outside all; -1 with no scenes.
int sceneIndexAt(double t, const std::vector<SceneSpan>& scenes);

// ---------------------------------------------------------------- frame measures

/// RGB thumbnail (w*h*3 bytes) -> grey (w*h), Rec. 601 weights, rounded as the TS does.
QByteArray greyOf(QByteArrayView rgb, int w, int h);
/// Variance of a 3x3 Laplacian over the grey thumb, log-compressed to 0..1 (0 = flat / blurred, ~1 = very sharp).
double sharpness(QByteArrayView grey, int w, int h);
struct Brightness { double mean = 0, clipped = 0; };
/// mean 0..1; clipped = fraction of pixels < 10 or > 245.
Brightness brightness(QByteArrayView grey);
/// 1 around mid-grey (plateau 0.45..0.55), gentle just outside, steep at the ends, times (1 - clipped).
double exposureScore(Brightness b);
/// Mean absolute difference of two equal-length thumbs, 0..1 (different sizes: 1; both empty: 0).
double distance(QByteArrayView a, QByteArrayView b);
/// distance(a, b) < limit with an early exit (same answer).
bool closerThan(QByteArrayView a, QByteArrayView b, double limit);
/// 1 - max(distance to the previous, distance to the next); ends use their one neighbour; a null entry is missing.
double stability(const std::vector<QByteArray>& sigs, int i);
/// 0.5 sharpness + 0.25 exposure + 0.25 stability, each clamped to 0..1 (NaN = 0).
double scoreSample(double sharpness, double exposure, double stability);

struct Candidate {
    double t = 0;
    int sceneIndex = -1;
    double score = 0, sharpness = 0, brightness = 0;
    QByteArray sig;                            // the RGB thumbnail
};
/// All measures of one sampled frame; stability from the neighbours given (null = none).
Candidate makeCandidate(double t, const QByteArray& rgb, int w, int h, const std::vector<SceneSpan>& scenes,
                        const QByteArray& prev = QByteArray(), const QByteArray& next = QByteArray());

/// Scene cuts from the scan's own thumbnails: a jump in the distance between neighbouring samples above
/// max(0.09, mean + 3 sd) is a cut; cuts closer than minLen to the previous one are ignored; the shortest scenes are
/// merged into a neighbour until at most maxScenes remain. One scene spanning the video when nothing stands out;
/// empty when there is nothing to measure. A non-finite duration falls back to the last time.
std::vector<Scene> scenesFromSignatures(const std::vector<double>& times, const std::vector<QByteArray>& sigs,
                                        double duration, double minLen = 0.9, int maxScenes = 8);

// ---------------------------------------------------------------- selection

struct Dropped { int nearDuplicate = 0, lowQuality = 0, tooClose = 0; };
struct Selection { std::vector<Candidate> kept; Dropped dropped; };
/// 1) the best-scoring frame of every scene (strongest scene first; the best that clears minScore and does not clash
/// with one already kept, else the best outright: a scene is never left empty); 2) best score first, any frame at
/// least minGap seconds AND minDistance away from everything kept, until `keep`. Frames under minScore count as
/// lowQuality; clashes as nearDuplicate / tooClose; frames past the cap are not counted. Input order does not matter;
/// `kept` comes back sorted by t.
Selection selectStills(const std::vector<Candidate>& cands, int keep, double minGap, double minDistance, double minScore = 0.12);

// ---------------------------------------------------------------- vision pass helpers

struct SheetCell { int i = 0, x = 0, y = 0, w = 0, h = 0; };
struct SheetLayout { int cols = 0, rows = 0, width = 0, height = 0; std::vector<SheetCell> cells; };
/// n cells of cellW x cellH with `gap` px around and between them; cols = ceil(sqrt(n)) capped at 4 unless given (>= 1).
SheetLayout contactSheetLayout(int n, int cellW, int cellH, int gap = 0, int cols = 0);

/// Split items into requests of at most perRequest (default 12), balanced (13 -> 7 + 6). With `tOf`, items are put in
/// t order first (stills.ts sorts when every item carries a t); without it they keep their order.
template <class T>
std::vector<std::vector<T>> mineBatches(std::vector<T> items, int perRequest = 12, std::function<double(const T&)> tOf = {}) {
    const int per = perRequest >= 1 ? perRequest : 12;
    if (tOf) std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return tOf(a) < tOf(b); });
    const int n = int(items.size());
    std::vector<std::vector<T>> out;
    if (!n) return out;
    const int batches = (n + per - 1) / per, size = (n + batches - 1) / batches;
    for (int i = 0; i < n; i += size) out.emplace_back(items.begin() + i, items.begin() + std::min(n, i + size));
    return out;
}

/// "1:05.2" (hours only when needed: "1:02:03.4"); nonsense -> "0:00.0".
QString fmtT(double t);

// ---------------------------------------------------------------- libav: scrub, scenes, key frames, grabs

struct ScrubProgress { QString phase; int done = 0, total = 0; QString message; };   // phase open|scan|select|grab|done

struct ScrubInput {
    std::optional<std::vector<SceneSpan>> scenes;   // known cuts (e.g. detectScenes); derived from the scan when missing
    ScrubOptions options;
    bool grab = true;                               // encode the kept stills as JPEG (false: times and scores only)
    int timeoutMs = 10 * 60 * 1000;
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const ScrubProgress&)> onProgress;   // called on the worker thread; must not block
};

struct ScrubbedStill {
    double t = 0;
    int sceneIndex = -1;
    int width = 0, height = 0;
    double score = 0, sharpness = 0, brightness = 0;
    QByteArray jpeg;                                // empty with grab = false
};

struct StillsScan {
    QString at;                                     // ISO time of the scan
    int sampled = 0, kept = 0;
    double step = 0;
    qint64 ms = 0;
    Dropped dropped;
    QStringList warnings;                           // at most 3 (decode failures, stills that could not be saved)
    QJsonObject toJson() const;
};

struct ScrubResult {
    bool ok = false;
    QString error;
    std::vector<ScrubbedStill> stills;
    StillsScan scan;
    int width = 0, height = 0;                      // display size: sample aspect applied, turned upright (frames.h)
    double duration = 0;
    std::vector<Scene> scenes;                      // the scenes used (given, derived, or none)
    bool scenesDerived = false;
    /// Everything but the JPEG bytes (stills carry jpegBytes instead).
    QJsonObject toJson() const;
};
/// Scan, score, select and grab (scrub.ts scrubVideo). Fails when fewer than 2 frames decode or no still can be saved.
ScrubResult scrubVideo(const QString& path, const ScrubInput& in = ScrubInput());
QFuture<ScrubResult> scrubVideoAsync(const QString& path, const ScrubInput& in = ScrubInput(), QThreadPool* pool = nullptr);

struct DetectOptions {
    double step = 0.25;                             // seconds between samples (ingest.ts: ~4 per second)
    double minLen = 0.9;
    int maxScenes = 8;
    int timeoutMs = 10 * 60 * 1000;
    const std::atomic<bool>* cancel = nullptr;
};
struct DetectResult {
    bool ok = false;
    QString error;
    std::vector<Scene> scenes;
    double duration = 0;
    int sampled = 0;
    QJsonObject toJson() const;
};
/// Hard cuts from 48x27 RGB thumbnails sampled every `step` seconds (ingest.ts detectScenes). Two deliberate changes
/// from the TS: sample k is at k x step (no accumulated float drift), and the step grows so a long master is sampled
/// at most 4000 times.
DetectResult detectScenes(const QString& path, const DetectOptions& o = DetectOptions());
QFuture<DetectResult> detectScenesAsync(const QString& path, const DetectOptions& o = DetectOptions(), QThreadPool* pool = nullptr);

struct GrabbedFrame {
    bool ok = false;
    QString error;
    double t = 0;                                   // the time asked for (clamped into the video)
    QByteArray jpeg;
    int width = 0, height = 0;
};
/// One JPEG of the frame at t, longest side <= maxSide (scrub.ts grabFrame; t clamped to [0, duration - 0.05]).
GrabbedFrame grabFrame(const QString& path, double t, int maxSide = 1280, double quality = 0.85,
                       int timeoutMs = 120000, const std::atomic<bool>* cancel = nullptr);
/// One JPEG per scene at its midpoint, width <= `width`, quality 0.72 (ingest.ts keyFrames); scenes that fail are left out.
std::vector<GrabbedFrame> keyFrames(const QString& path, const std::vector<Scene>& scenes, int width = 640,
                                    int timeoutMs = 10 * 60 * 1000, const std::atomic<bool>* cancel = nullptr);

} // namespace studio::media
