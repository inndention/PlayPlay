// Stills (ST-17): port of src/studio/stills.ts (pure part), src/studio/scrub.ts and src/ads/ingest.ts (libav part).
// See stills.h. Where a TS line is ported verbatim the constants keep their TS names in comments.
#include "media/stills.h"

#include "media/frames.h"
#include "media/imageops.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <limits>

namespace studio::media {
namespace {

constexpr double kEdge = 0.05;             // EDGE: stay off the very first / last frame (often black or a fade)
constexpr double kSceneLead = 0.15;        // SCENE_LEAD: a cut is rarely clean on its first frame
constexpr double kMergeEps = 1e-3;         // MERGE_EPS
constexpr double kAutoScenesMaxStep = 2;   // scrub.ts AUTO_SCENES_MAX_STEP: cuts from the scan only while samples are this close
constexpr int kProgressEvery = 10;         // PROGRESS_EVERY
constexpr int kMaxWarnings = 3;            // MAX_WARNINGS
constexpr int kDetectMaxSamples = 4000;    // detectScenes: the step grows past this many samples (a long master)
const double kSharpCeil = std::log1p(255.0 * 255.0);   // SHARP_CEIL
const double kNaN = std::numeric_limits<double>::quiet_NaN();

double jsRound(double x) { return std::floor(x + 0.5); }   // Math.round

// stills.ts num(): non-finite -> default, clamp, optionally rounded
double num(double v, double dflt, double lo, double hi, bool integer = false) {
    if (!std::isfinite(v)) return dflt;
    const double c = std::min(hi, std::max(lo, v));
    return integer ? jsRound(c) : c;
}

// Number(v) of the TS for a JSON member: missing -> NaN (the default), null -> 0, bool -> 0/1, numeric text -> its value.
double jsNumber(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Double: return v.toDouble();
    case QJsonValue::Null: return 0;
    case QJsonValue::Bool: return v.toBool() ? 1 : 0;
    case QJsonValue::String: {
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) return 0;
        bool ok = false;
        const double d = s.toDouble(&ok);
        return ok ? d : kNaN;
    }
    default: return kNaN;
    }
}

double unit(double v) { return std::isfinite(v) ? std::min(1.0, std::max(0.0, v)) : 0.0; }

std::vector<double> sortUnique(std::vector<double> ts) {
    std::sort(ts.begin(), ts.end());
    std::vector<double> out;
    for (double t : ts) if (out.empty() || t - out.back() > kMergeEps) out.push_back(t);
    return out;
}

const uchar* bytes(QByteArrayView v) { return reinterpret_cast<const uchar*>(v.data()); }

std::vector<SceneSpan> spansOf(const std::vector<Scene>& s) {
    std::vector<SceneSpan> out;
    out.reserve(s.size());
    for (const Scene& x : s) out.push_back({x.start, x.end});
    return out;
}

QJsonArray scenesJson(const std::vector<Scene>& scenes) {
    QJsonArray a;
    for (const Scene& s : scenes) a.append(QJsonObject{{QStringLiteral("start"), s.start}, {QStringLiteral("end"), s.end}, {QStringLiteral("midpoint"), s.midpoint}});
    return a;
}

} // namespace

// ---------------------------------------------------------------- options

ScrubOptions clampOptions(const ScrubOptions& x) {
    const ScrubOptions d;
    ScrubOptions o;
    o.fps = num(x.fps, d.fps, 0.25, 8);
    o.maxSamples = int(num(x.maxSamples, d.maxSamples, 20, 4000, true));
    o.keep = int(num(x.keep, d.keep, 1, 120, true));
    o.minGap = num(x.minGap, d.minGap, 0, 60);
    o.minDistance = num(x.minDistance, d.minDistance, 0, 1);
    o.thumbW = int(num(x.thumbW, d.thumbW, 16, 320, true));
    o.thumbH = int(num(x.thumbH, d.thumbH, 9, 320, true));
    o.maxSide = int(num(x.maxSide, d.maxSide, 128, 4096, true));
    o.jpegQuality = num(x.jpegQuality, d.jpegQuality, 0.3, 1);
    return o;
}

ScrubOptions scrubOptions(const QJsonObject& x) {
    const ScrubOptions d;
    auto get = [&](const char* k, double dflt, double lo, double hi, bool integer = false) {
        const QJsonValue v = x.value(QLatin1String(k));
        return v.isUndefined() ? dflt : num(jsNumber(v), dflt, lo, hi, integer);
    };
    ScrubOptions o;
    o.fps = get("fps", d.fps, 0.25, 8);
    o.maxSamples = int(get("maxSamples", d.maxSamples, 20, 4000, true));
    o.keep = int(get("keep", d.keep, 1, 120, true));
    o.minGap = get("minGap", d.minGap, 0, 60);
    o.minDistance = get("minDistance", d.minDistance, 0, 1);
    o.thumbW = int(get("thumbW", d.thumbW, 16, 320, true));
    o.thumbH = int(get("thumbH", d.thumbH, 9, 320, true));
    o.maxSide = int(get("maxSide", d.maxSide, 128, 4096, true));
    o.jpegQuality = get("jpegQuality", d.jpegQuality, 0.3, 1);
    return o;
}

QJsonObject toJson(const ScrubOptions& o) {
    return QJsonObject{
        {QStringLiteral("fps"), o.fps}, {QStringLiteral("maxSamples"), o.maxSamples}, {QStringLiteral("keep"), o.keep},
        {QStringLiteral("minGap"), o.minGap}, {QStringLiteral("minDistance"), o.minDistance},
        {QStringLiteral("thumbW"), o.thumbW}, {QStringLiteral("thumbH"), o.thumbH},
        {QStringLiteral("maxSide"), o.maxSide}, {QStringLiteral("jpegQuality"), o.jpegQuality},
    };
}

// ---------------------------------------------------------------- sample plan

SamplePlan samplePlan(double duration, double fpsIn, int maxSamplesIn, const std::vector<SceneSpan>& scenes) {
    const double dur = duration;
    if (!std::isfinite(dur) || dur <= 0) return {};
    const double fps = num(fpsIn, 2, 0.25, 8);
    const int maxSamples = int(num(maxSamplesIn, 900, 1, 1e6, true));
    auto inside = [&](double t) { return std::isfinite(t) && t >= 0 && t < dur; };
    const double lead = std::min(kEdge, dur / 2);
    // fixed points first: the two ends and the scene anchors; they come out of the uniform budget
    std::vector<double> fixed{lead, dur - lead};
    for (const SceneSpan& s : scenes) {
        if (!std::isfinite(s.start) || !std::isfinite(s.end)) continue;
        for (double t : {s.start + kSceneLead, (s.start + s.end) / 2}) if (inside(t)) fixed.push_back(t);
    }
    std::vector<double> in;
    for (double t : fixed) if (inside(t)) in.push_back(t);
    std::vector<double> anchors = sortUnique(in);
    if (int(anchors.size()) > maxSamples) anchors.resize(size_t(maxSamples));
    const int budget = std::max(0, maxSamples - int(anchors.size()));
    double step = 1 / fps;
    const double span = dur - 2 * lead;
    if (budget > 0 && span > 0) {
        // a uniform grid from lead to dur - lead has floor(span/step) + 1 points; grow the step until that fits
        if (std::floor(span / step) + 1 > budget) step = budget > 1 ? span / (budget - 1) + 1e-9 : span + 1;
    } else {
        step = span > 0 ? span + 1 : 1 / fps;
    }
    std::vector<double> grid;
    if (budget > 0) {
        for (int k = 0;; ++k) {
            const double t = lead + k * step;
            if (t > dur - lead + 1e-9 || !inside(t) || int(grid.size()) >= budget) break;
            grid.push_back(t);
        }
    }
    std::vector<double> all = anchors;
    all.insert(all.end(), grid.begin(), grid.end());
    std::vector<double> times = sortUnique(all);
    // belt and braces: never more than the cap (anchors are kept, the grid is thinned)
    if (int(times.size()) > maxSamples) {
        std::vector<double> extra;
        for (double t : times) if (std::find(anchors.begin(), anchors.end(), t) == anchors.end()) extra.push_back(t);
        const int room = std::max(0, maxSamples - int(anchors.size()));
        std::vector<double> thinned;
        const double n = double(extra.size());
        for (size_t i = 0; i < extra.size() && int(thinned.size()) < room; ++i)
            if (std::floor(double(i) * room / n) != std::floor((double(i) - 1) * room / n)) thinned.push_back(extra[i]);
        std::vector<double> merged = anchors;
        merged.insert(merged.end(), thinned.begin(), thinned.end());
        times = sortUnique(merged);
    }
    return {times, times.size() > 1 ? step : 0};
}

int sceneIndexAt(double t, const std::vector<SceneSpan>& scenes) {
    if (scenes.empty() || !std::isfinite(t)) return -1;
    int best = -1;
    double bestD = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < scenes.size(); ++i) {
        const SceneSpan& s = scenes[i];
        if (!std::isfinite(s.start) || !std::isfinite(s.end)) continue;
        if (t >= s.start && t < s.end) return int(i);
        const double d = t < s.start ? s.start - t : t - s.end;
        if (d < bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// ---------------------------------------------------------------- frame measures

QByteArray greyOf(QByteArrayView rgb, int w, int h) {
    const qsizetype n = std::max<qsizetype>(0, std::min<qsizetype>(qsizetype(std::max(0, w)) * std::max(0, h), rgb.size() / 3));
    QByteArray g(n, Qt::Uninitialized);
    const uchar* p = bytes(rgb);
    for (qsizetype i = 0, j = 0; i < n; ++i, j += 3) g[i] = char((p[j] * 299 + p[j + 1] * 587 + p[j + 2] * 114 + 500) / 1000);
    return g;
}

double sharpness(QByteArrayView grey, int w, int h) {
    if (!(w >= 3) || !(h >= 3) || grey.size() < qsizetype(w) * h) return 0;
    const uchar* g = bytes(grey);
    double sum = 0, sq = 0;
    qint64 n = 0;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const qsizetype i = qsizetype(y) * w + x;
            const int l = 4 * g[i] - g[i - 1] - g[i + 1] - g[i - w] - g[i + w];
            sum += l; sq += double(l) * l; ++n;
        }
    }
    if (!n) return 0;
    const double mean = sum / double(n), v = std::max(0.0, sq / double(n) - mean * mean);
    return std::min(1.0, std::log1p(v) / kSharpCeil);
}

Brightness brightness(QByteArrayView grey) {
    const qsizetype n = grey.size();
    if (!n) return {};
    const uchar* g = bytes(grey);
    double sum = 0;
    qint64 clip = 0;
    for (qsizetype i = 0; i < n; ++i) { const int v = g[i]; sum += v; if (v < 10 || v > 245) ++clip; }
    return {sum / double(n) / 255.0, double(clip) / double(n)};
}

double exposureScore(Brightness b) {
    if (!std::isfinite(b.mean)) return 0;
    // a plateau of 1 around mid-grey (0.45..0.55), gentle just outside it (a 0.35 mean still scores 0.9), steep at the ends
    const double d = std::min(1.0, std::max(0.0, std::abs(b.mean - 0.5) - 0.05) / 0.45);
    const double tone = 1 - std::pow(d, 1.5);
    const double clip = std::isfinite(b.clipped) ? 1 - std::min(1.0, std::max(0.0, b.clipped)) : 1;
    return std::min(1.0, std::max(0.0, tone * clip));
}

double distance(QByteArrayView a, QByteArrayView b) {
    if (a.size() != b.size()) return 1;
    const qsizetype n = a.size();
    if (!n) return 0;
    const uchar* pa = bytes(a);
    const uchar* pb = bytes(b);
    qint64 sum = 0;
    for (qsizetype i = 0; i < n; ++i) sum += std::abs(int(pa[i]) - int(pb[i]));
    return double(sum) / double(n) / 255.0;
}

bool closerThan(QByteArrayView a, QByteArrayView b, double limit) {
    if (a.size() != b.size()) return 1 < limit;
    const qsizetype n = a.size();
    if (!n) return 0 < limit;
    const double cap = limit * double(n) * 255.0;
    const uchar* pa = bytes(a);
    const uchar* pb = bytes(b);
    double sum = 0;
    for (qsizetype i = 0; i < n; ++i) {
        sum += std::abs(int(pa[i]) - int(pb[i]));
        if (sum >= cap) return false;
    }
    return sum / double(n) / 255.0 < limit;
}

double stability(const std::vector<QByteArray>& sigs, int i) {
    if (i < 0 || i >= int(sigs.size()) || sigs[size_t(i)].isNull()) return 0;
    const QByteArray& cur = sigs[size_t(i)];
    double d = 0;
    if (i - 1 >= 0 && !sigs[size_t(i - 1)].isNull()) d = std::max(d, distance(cur, sigs[size_t(i - 1)]));
    if (i + 1 < int(sigs.size()) && !sigs[size_t(i + 1)].isNull()) d = std::max(d, distance(cur, sigs[size_t(i + 1)]));
    return std::min(1.0, std::max(0.0, 1 - d));
}

double scoreSample(double s, double e, double st) { return unit(0.5 * unit(s) + 0.25 * unit(e) + 0.25 * unit(st)); }

Candidate makeCandidate(double t, const QByteArray& rgb, int w, int h, const std::vector<SceneSpan>& scenes,
                        const QByteArray& prev, const QByteArray& next) {
    const QByteArray grey = greyOf(rgb, w, h);
    const double sharp = sharpness(grey, w, h);
    const Brightness b = brightness(grey);
    std::vector<QByteArray> chain;
    if (!prev.isNull()) chain.push_back(prev);
    const int at = int(chain.size());
    chain.push_back(rgb);
    if (!next.isNull()) chain.push_back(next);
    Candidate c;
    c.t = t;
    c.sceneIndex = sceneIndexAt(t, scenes);
    c.score = scoreSample(sharp, exposureScore(b), stability(chain, at));
    c.sharpness = sharp;
    c.brightness = b.mean;
    c.sig = rgb;
    return c;
}

std::vector<Scene> scenesFromSignatures(const std::vector<double>& times, const std::vector<QByteArray>& sigs,
                                        double duration, double minLenIn, int maxScenesIn) {
    const size_t n = std::min(times.size(), sigs.size());
    const double dur = std::isfinite(duration) && duration > 0 ? duration : n ? times[n - 1] : 0;
    if (!n || !(dur > 0)) return {};
    const double minLen = std::isfinite(minLenIn) ? std::max(0.0, minLenIn) : 0;
    const int maxScenes = std::max(1, maxScenesIn > 0 ? maxScenesIn : 8);
    std::vector<double> diffs(n, 0.0);
    for (size_t i = 1; i < n; ++i) diffs[i] = distance(sigs[i], sigs[i - 1]);
    double mean = 0;
    for (double x : diffs) mean += x;
    mean /= double(n);
    double var = 0;
    for (double x : diffs) var += (x - mean) * (x - mean);
    const double sd = std::sqrt(var / double(n));
    const double thresh = std::max(0.09, mean + 3 * sd);
    std::vector<double> cuts{0};
    for (size_t i = 1; i < n; ++i)
        if (diffs[i] > thresh && times[i] - cuts.back() >= minLen && times[i] < dur) cuts.push_back(times[i]);
    std::vector<SceneSpan> scenes;
    for (size_t i = 0; i < cuts.size(); ++i) scenes.push_back({cuts[i], i + 1 < cuts.size() ? cuts[i + 1] : dur});
    while (int(scenes.size()) > maxScenes) {
        size_t k = 0;
        double best = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < scenes.size(); ++i) if (scenes[i].end - scenes[i].start < best) { best = scenes[i].end - scenes[i].start; k = i; }
        const size_t a = std::min(k, k == 0 ? size_t(1) : k - 1);
        scenes[a].end = scenes[a + 1].end;
        scenes.erase(scenes.begin() + qsizetype(a) + 1);
    }
    std::vector<Scene> out;
    for (const SceneSpan& s : scenes) out.push_back({s.start, s.end, (s.start + s.end) / 2});
    return out;
}

// ---------------------------------------------------------------- selection

Selection selectStills(const std::vector<Candidate>& cands, int keepIn, double minGapIn, double minDistanceIn, double minScoreIn) {
    Selection sel;
    std::vector<Candidate> all;
    for (const Candidate& c : cands) {
        if (!std::isfinite(c.t) || c.sig.isNull()) continue;
        all.push_back(c);
        if (!std::isfinite(all.back().score)) all.back().score = 0;
    }
    std::stable_sort(all.begin(), all.end(), [](const Candidate& a, const Candidate& b) {
        return a.score != b.score ? a.score > b.score : a.t < b.t;   // byScore
    });
    const int keep = std::max(1, keepIn > 0 ? keepIn : 1);
    const double minGap = std::isfinite(minGapIn) ? std::max(0.0, minGapIn) : 0;
    const double minDistance = std::isfinite(minDistanceIn) ? std::max(0.0, minDistanceIn) : 0;
    const double minScore = std::isfinite(minScoreIn) ? std::max(0.0, minScoreIn) : 0.12;
    if (all.empty()) return sel;
    std::vector<size_t> kept;
    std::vector<char> isKept(all.size(), 0);
    enum Clash { None, NearDuplicate, TooClose };
    auto clash = [&](const Candidate& c) {
        bool near = false, close = false;
        for (size_t k : kept) {
            if (minDistance > 0 && closerThan(c.sig, all[k].sig, minDistance)) { near = true; break; }
            if (minGap > 0 && std::abs(c.t - all[k].t) < minGap) close = true;
        }
        return near ? NearDuplicate : close ? TooClose : None;
    };
    auto add = [&](size_t i) { kept.push_back(i); isKept[i] = 1; };

    // 1) one per scene, strongest scene first (groups appear in the order of their best frame, as in the TS Map)
    std::vector<int> order;
    std::vector<std::vector<size_t>> groups;
    for (size_t i = 0; i < all.size(); ++i) {
        const auto it = std::find(order.begin(), order.end(), all[i].sceneIndex);
        if (it == order.end()) { order.push_back(all[i].sceneIndex); groups.push_back({i}); }
        else groups[size_t(it - order.begin())].push_back(i);
    }
    for (const std::vector<size_t>& g : groups) {
        if (int(kept.size()) >= keep) break;
        size_t pick = g.front();
        for (size_t i : g) if (all[i].score >= minScore && clash(all[i]) == None) { pick = i; break; }
        add(pick);
    }
    // 2) fill by score; past the cap the rest is still classified so the counts say what the scrub threw away
    for (size_t i = 0; i < all.size(); ++i) {
        if (isKept[i]) continue;
        if (all[i].score < minScore) { ++sel.dropped.lowQuality; continue; }
        const Clash why = clash(all[i]);
        if (why == NearDuplicate) ++sel.dropped.nearDuplicate;
        else if (why == TooClose) ++sel.dropped.tooClose;
        else if (int(kept.size()) < keep) add(i);
    }
    for (size_t i : kept) sel.kept.push_back(all[i]);
    std::stable_sort(sel.kept.begin(), sel.kept.end(), [](const Candidate& a, const Candidate& b) { return a.t < b.t; });
    return sel;
}

// ---------------------------------------------------------------- vision pass helpers

SheetLayout contactSheetLayout(int n, int cellW, int cellH, int gap, int colsIn) {
    SheetLayout L;
    n = std::max(0, n);
    cellW = std::max(1, cellW);
    cellH = std::max(1, cellH);
    gap = std::max(0, gap);
    if (!n) return L;
    const int want = colsIn >= 1 ? colsIn : std::min(4, int(std::ceil(std::sqrt(double(n)))));
    L.cols = std::max(1, std::min(want, n));
    L.rows = (n + L.cols - 1) / L.cols;
    for (int i = 0; i < n; ++i)
        L.cells.push_back({i, gap + (i % L.cols) * (cellW + gap), gap + (i / L.cols) * (cellH + gap), cellW, cellH});
    L.width = L.cols * cellW + (L.cols + 1) * gap;
    L.height = L.rows * cellH + (L.rows + 1) * gap;
    return L;
}

QString fmtT(double t) {
    // round to tenths first so 59.99 reads "1:00.0", not "0:60.0"
    const qint64 tenths = std::isfinite(t) && t > 0 ? qint64(jsRound(t * 10)) : 0;
    const qint64 h = tenths / 36000, m = (tenths % 36000) / 600, s = tenths % 600;
    const QString sec = QStringLiteral("%1.%2").arg(s / 10, 2, 10, QLatin1Char('0')).arg(s % 10);
    return h ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(sec) : QStringLiteral("%1:%2").arg(m).arg(sec);
}

// ---------------------------------------------------------------- results as JSON

QJsonObject StillsScan::toJson() const {
    return QJsonObject{
        {QStringLiteral("at"), at}, {QStringLiteral("sampled"), sampled}, {QStringLiteral("kept"), kept},
        {QStringLiteral("step"), step}, {QStringLiteral("ms"), double(ms)},
        {QStringLiteral("dropped"), QJsonObject{{QStringLiteral("nearDuplicate"), dropped.nearDuplicate},
                                                {QStringLiteral("lowQuality"), dropped.lowQuality},
                                                {QStringLiteral("tooClose"), dropped.tooClose}}},
        {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)},
    };
}

QJsonObject ScrubResult::toJson() const {
    if (!ok) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    QJsonArray a;
    for (const ScrubbedStill& s : stills) {
        a.append(QJsonObject{
            {QStringLiteral("t"), s.t}, {QStringLiteral("sceneIndex"), s.sceneIndex},
            {QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height},
            {QStringLiteral("score"), s.score}, {QStringLiteral("sharpness"), s.sharpness},
            {QStringLiteral("brightness"), s.brightness}, {QStringLiteral("jpegBytes"), double(s.jpeg.size())}});
    }
    return QJsonObject{
        {QStringLiteral("ok"), true}, {QStringLiteral("stills"), a}, {QStringLiteral("scan"), scan.toJson()},
        {QStringLiteral("width"), width}, {QStringLiteral("height"), height}, {QStringLiteral("duration"), duration},
        {QStringLiteral("scenes"), scenesJson(scenes)}, {QStringLiteral("scenesDerived"), scenesDerived},
    };
}

QJsonObject DetectResult::toJson() const {
    if (!ok) return QJsonObject{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("scenes"), scenesJson(scenes)},
                       {QStringLiteral("duration"), duration}, {QStringLiteral("sampled"), sampled}};
}

// ---------------------------------------------------------------- libav: scrub

ScrubResult scrubVideo(const QString& path, const ScrubInput& inp) {
    QElapsedTimer clock;
    clock.start();
    ScrubResult res;
    const ScrubOptions o = clampOptions(inp.options);
    auto progress = [&](const char* phase, int done, int total, const QString& message) {
        if (inp.onProgress) inp.onProgress(ScrubProgress{QString::fromLatin1(phase), done, total, message});
    };
    auto warn = [&](const QString& line) { if (res.scan.warnings.size() < kMaxWarnings) res.scan.warnings << line; };
    auto fail = [&](const QString& m) { res.ok = false; res.error = m; res.stills.clear(); return res; };

    // ---- open ----
    progress("open", 0, 1, QStringLiteral("opening video"));
    FrameSampler::Options so;
    so.timeoutMs = inp.timeoutMs;
    so.cancel = inp.cancel;
    FrameSampler fs(path, so);
    if (!fs.ok()) return fail(fs.error());
    res.width = fs.displayWidth();   // turned upright, like the stills
    res.height = fs.displayHeight();
    res.duration = fs.duration();
    if (!(res.duration > 0)) return fail(QStringLiteral("this video reports no duration; try re-exporting it as H.264 MP4"));

    std::optional<std::vector<SceneSpan>> scenes;
    if (inp.scenes) {
        std::vector<SceneSpan> given;
        for (const SceneSpan& s : *inp.scenes) if (std::isfinite(s.start) && std::isfinite(s.end) && s.end > s.start) given.push_back(s);
        scenes = given;
        for (const SceneSpan& s : given) res.scenes.push_back({s.start, s.end, (s.start + s.end) / 2});
    }

    // ---- scan ----
    const SamplePlan plan = samplePlan(res.duration, o.fps, o.maxSamples, scenes ? *scenes : std::vector<SceneSpan>());
    const int n = int(plan.times.size());
    std::vector<double> times;
    std::vector<QByteArray> sigs;
    int failed = 0;
    QString firstError;
    for (int i = 0; i < n; ++i) {
        if (i % kProgressEvery == 0) progress("scan", i, n, QStringLiteral("scanning frame %1 of %2").arg(i + 1).arg(n));
        QByteArray rgb = fs.thumbAt(plan.times[size_t(i)], o.thumbW, o.thumbH);
        if (rgb.isEmpty()) {
            if (fs.aborted()) return fail(fs.error());
            ++failed;
            if (firstError.isEmpty()) firstError = fs.error();
            continue;
        }
        times.push_back(plan.times[size_t(i)]);
        sigs.push_back(std::move(rgb));
    }
    if (failed) warn(QStringLiteral("%1 of %2 frames could not be decoded%3").arg(failed).arg(n).arg(firstError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(firstError)));
    // the decoder's own words go into the error too: a codec problem and a corrupt file need different fixes
    if (sigs.size() < 2)
        return fail(QStringLiteral("could not read frames from this video%1; try re-exporting it as H.264 MP4").arg(firstError.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(firstError)));

    // no cuts given: take them from the thumbnails just read (free), instead of a second pass over the video
    if (!scenes && plan.step <= kAutoScenesMaxStep) {
        res.scenes = scenesFromSignatures(times, sigs, res.duration);
        res.scenesDerived = true;
        scenes = spansOf(res.scenes);
    }
    const std::vector<SceneSpan> spans = scenes ? *scenes : std::vector<SceneSpan>();

    // ---- select ----
    progress("select", 0, 1, QStringLiteral("scoring %1 frames").arg(sigs.size()));
    std::vector<Candidate> cands;
    cands.reserve(sigs.size());
    for (size_t k = 0; k < sigs.size(); ++k)
        cands.push_back(makeCandidate(times[k], sigs[k], o.thumbW, o.thumbH, spans,
                                      k ? sigs[k - 1] : QByteArray(), k + 1 < sigs.size() ? sigs[k + 1] : QByteArray()));
    const Selection sel = selectStills(cands, o.keep, o.minGap, o.minDistance);
    progress("select", 1, 1, QStringLiteral("%1 stills chosen").arg(sel.kept.size()));

    // ---- grab ----
    // kept is sorted by t; each grab is a forward step or a seek from the nearest keyframe (a couple of dozen at most)
    int grabFailed = 0;
    const double kDisp = std::min(1.0, double(o.maxSide) / std::max(res.width, res.height));
    for (size_t i = 0; i < sel.kept.size(); ++i) {
        const Candidate& k = sel.kept[i];
        progress("grab", int(i), int(sel.kept.size()), QStringLiteral("saving still %1 of %2").arg(i + 1).arg(sel.kept.size()));
        ScrubbedStill st;
        st.t = k.t; st.sceneIndex = k.sceneIndex; st.score = k.score; st.sharpness = k.sharpness; st.brightness = k.brightness;
        if (!inp.grab) {
            st.width = std::max(1, int(std::lround(res.width * kDisp)));
            st.height = std::max(1, int(std::lround(res.height * kDisp)));
            res.stills.push_back(st);
            continue;
        }
        const QImage img = fs.imageAt(std::clamp(k.t, 0.0, std::max(0.0, res.duration - 0.05)), o.maxSide, o.maxSide);
        QString err = fs.error();
        if (!img.isNull()) st.jpeg = encodeJpeg(img, o.jpegQuality, &err);
        if (fs.aborted()) return fail(fs.error());
        if (img.isNull() || st.jpeg.isEmpty()) {
            if (++grabFailed == 1) warn(QStringLiteral("could not save the still at %1 s (%2)").arg(k.t, 0, 'f', 2).arg(err));
            continue;
        }
        st.width = img.width();
        st.height = img.height();
        res.stills.push_back(std::move(st));
    }
    if (grabFailed > 1) warn(QStringLiteral("%1 of %2 chosen stills could not be saved").arg(grabFailed).arg(sel.kept.size()));
    if (res.stills.empty()) return fail(QStringLiteral("could not read frames from this video; try re-exporting it as H.264 MP4"));

    res.scan.at = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    res.scan.sampled = int(sigs.size());
    res.scan.kept = int(res.stills.size());
    res.scan.step = plan.step;
    res.scan.dropped = sel.dropped;
    res.scan.ms = clock.elapsed();
    res.ok = true;
    progress("done", res.scan.kept, res.scan.kept, QStringLiteral("%1 stills from %2 frames in %3 s").arg(res.scan.kept).arg(res.scan.sampled).arg(res.scan.ms / 1000.0, 0, 'f', 1));
    return res;
}

QFuture<ScrubResult> scrubVideoAsync(const QString& path, const ScrubInput& in, QThreadPool* pool) {
    return QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(), [path, in] { return scrubVideo(path, in); });
}

// ---------------------------------------------------------------- libav: scenes, key frames, grabs

DetectResult detectScenes(const QString& path, const DetectOptions& o) {
    DetectResult r;
    FrameSampler::Options so;
    so.timeoutMs = o.timeoutMs;
    so.cancel = o.cancel;
    FrameSampler fs(path, so);
    if (!fs.ok()) { r.error = fs.error(); return r; }
    r.duration = fs.duration();
    if (!(r.duration > 0)) { r.error = QStringLiteral("this video reports no duration"); return r; }
    // RGB, not luma: two scenes can differ completely in colour yet share a brightness (ingest.ts)
    constexpr int W = 48, H = 27;
    const double step = std::max(std::isfinite(o.step) ? std::clamp(o.step, 0.04, 10.0) : 0.25, r.duration / kDetectMaxSamples);
    std::vector<double> times;
    std::vector<QByteArray> sigs;
    QString firstError;
    for (qint64 k = 0;; ++k) {
        const double t = double(k) * step;
        if (t >= r.duration) break;
        QByteArray rgb = fs.thumbAt(t, W, H);
        if (rgb.isEmpty()) {
            if (fs.aborted()) { r.error = fs.error(); return r; }
            if (firstError.isEmpty()) firstError = fs.error();
            continue;
        }
        times.push_back(t);
        sigs.push_back(std::move(rgb));
    }
    r.sampled = int(sigs.size());
    if (sigs.empty()) { r.error = QStringLiteral("could not read frames from this video (%1)").arg(firstError); return r; }
    r.scenes = scenesFromSignatures(times, sigs, r.duration, o.minLen, o.maxScenes);
    r.ok = true;
    return r;
}

QFuture<DetectResult> detectScenesAsync(const QString& path, const DetectOptions& o, QThreadPool* pool) {
    return QtConcurrent::run(pool ? pool : QThreadPool::globalInstance(), [path, o] { return detectScenes(path, o); });
}

GrabbedFrame grabFrame(const QString& path, double t, int maxSide, double quality, int timeoutMs, const std::atomic<bool>* cancel) {
    GrabbedFrame g;
    FrameSampler::Options so;
    so.timeoutMs = timeoutMs;
    so.cancel = cancel;
    FrameSampler fs(path, so);
    if (!fs.ok()) { g.error = fs.error(); return g; }
    g.t = std::clamp(std::isfinite(t) ? t : 0.0, 0.0, std::max(0.0, fs.duration() - 0.05));
    const int side = std::max(16, maxSide);
    const QImage img = fs.imageAt(g.t, side, side);
    if (img.isNull()) { g.error = fs.error(); return g; }
    g.jpeg = encodeJpeg(img, quality, &g.error);
    if (g.jpeg.isEmpty()) return g;
    g.width = img.width();
    g.height = img.height();
    g.ok = true;
    return g;
}

std::vector<GrabbedFrame> keyFrames(const QString& path, const std::vector<Scene>& scenes, int width, int timeoutMs,
                                    const std::atomic<bool>* cancel) {
    std::vector<GrabbedFrame> out;
    FrameSampler::Options so;
    so.timeoutMs = timeoutMs;
    so.cancel = cancel;
    FrameSampler fs(path, so);
    if (!fs.ok()) return out;
    for (const Scene& s : scenes) {
        GrabbedFrame g;
        g.t = std::round(std::clamp(s.midpoint, 0.0, std::max(0.0, fs.duration() - 0.05)) * 100) / 100;
        const QImage img = fs.imageAt(g.t, std::max(16, width), 0);
        if (fs.aborted()) break;
        if (img.isNull()) continue;
        g.jpeg = encodeJpeg(img, 0.72, &g.error);
        if (g.jpeg.isEmpty()) continue;
        g.width = img.width();
        g.height = img.height();
        g.ok = true;
        out.push_back(std::move(g));
    }
    return out;
}

} // namespace studio::media
