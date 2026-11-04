// Cost estimators (ST-08): starmedia src/pricing.ts. See pricing.h.
#include "catalog/pricing.h"
#include "core/json.h"

#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace studio::catalog {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// JS Math.round: half towards +infinity on the exact double (floor(x + 0.5) is wrong for 0.49999999999999994).
double jsRound(double x) {
    const double f = std::floor(x);
    return x - f >= 0.5 ? f + 1 : f;
}

QString roundedInt(double x) { return groupedInt(qint64(jsRound(x))); }
QString n2s(double x) { return json::jsNumber(x); }

const QStringList& effortOrder() {
    static const QStringList o = {QStringLiteral("none"), QStringLiteral("minimal"), QStringLiteral("low"),   QStringLiteral("medium"),
                                  QStringLiteral("high"), QStringLiteral("xhigh"),   QStringLiteral("max")};
    return o;
}

struct LocalRate { double rate = 0; bool measured = false; };

LocalRate localRate(const ModelInfo& m, std::optional<double> Throughput::*k, double fallback) {
    if (m.local) {
        const std::optional<double>& v = m.local->throughput.*k;
        if (v && *v > 0) return {*v, true};
    }
    return {fallback, false};
}

CostEstimate localEstimate(double seconds, bool measured, const QString& detail) {
    CostEstimate e;
    e.usd = 0;
    e.seconds = std::ceil(seconds);
    e.confidence = Confidence::Approx;
    e.detail = QStringLiteral("%1 · %2 on this machine%3").arg(detail, fmtSeconds(seconds),
                                                              measured ? QString() : QStringLiteral(" (unmeasured: calibrate for a real figure)"));
    return e;
}

CostEstimate estimate(double usd, Confidence c, const QString& detail) {
    CostEstimate e;
    e.usd = usd;
    e.confidence = c;
    e.detail = detail;
    return e;
}

} // namespace

// ---------------- CostEstimate ----------------

QString confidenceName(Confidence c) { return c == Confidence::Exact ? QStringLiteral("exact") : QStringLiteral("approx"); }

bool CostEstimate::finite() const { return std::isfinite(usd); }

std::optional<money::Micro> CostEstimate::micro() const {
    if (!finite()) return std::nullopt;
    bool ok = false;
    const money::Micro m = money::fromUsd(usd, &ok);
    return ok ? std::optional<money::Micro>(m) : std::nullopt;
}

QJsonObject CostEstimate::toJson() const {
    QJsonObject o{{QStringLiteral("usd"), finite() ? QJsonValue(usd) : QJsonValue()},   // JSON.stringify(Infinity) is null
                  {QStringLiteral("confidence"), confidenceName(confidence)},
                  {QStringLiteral("detail"), detail}};
    if (seconds) o.insert(QStringLiteral("seconds"), *seconds);
    return o;
}

CostEstimate CostEstimate::fromJson(const QJsonObject& o) {
    CostEstimate e;
    const QJsonValue u = o.value(QLatin1String("usd"));
    e.usd = u.isDouble() ? u.toDouble() : kInf;
    e.confidence = o.value(QLatin1String("confidence")).toString() == QLatin1String("exact") ? Confidence::Exact : Confidence::Approx;
    e.detail = o.value(QLatin1String("detail")).toString();
    if (o.value(QLatin1String("seconds")).isDouble()) e.seconds = o.value(QLatin1String("seconds")).toDouble();
    return e;
}

CostEstimate zeroEstimate() { return estimate(0, Confidence::Exact, QStringLiteral("local")); }

double sumUsd(const QList<CostEstimate>& xs) {
    double t = 0;
    for (const CostEstimate& c : xs) t += std::isfinite(c.usd) ? c.usd : 0;
    return t;
}

double sumSeconds(const QList<CostEstimate>& xs) {
    double t = 0;
    for (const CostEstimate& c : xs) t += c.seconds && std::isfinite(*c.seconds) ? *c.seconds : 0;
    return t;
}

// ---------------- formatting ----------------

QString toFixedJs(double v, int decimals) {
    if (!std::isfinite(v)) return json::jsNumber(v);
    decimals = std::clamp(decimals, 0, 100);
    const bool neg = v < 0;
    if (neg) v = -v;
    // The exact decimal expansion (the C runtime prints every digit asked for), then half-up at `decimals`: the
    // first dropped digit decides, so an exact tie rounds up as toFixed does and the C runtime's half-to-even never
    // shows (the 40 guard digits make a carry into the decided digit impossible for any value this is used on).
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", decimals + 40, v);
    QByteArray s(buf);
    const int dot = int(s.indexOf('.'));
    QByteArray kept = s.left(dot + 1 + decimals);
    const char next = s.at(dot + 1 + decimals);
    if (next >= '5') {
        // add one unit in the last place, carrying through the digits (and over the point)
        int i = int(kept.size()) - 1;
        while (i >= 0) {
            if (kept[i] == '.') { --i; continue; }
            if (kept[i] == '9') { kept[i] = '0'; --i; continue; }
            kept[i] = char(kept[i] + 1);
            break;
        }
        if (i < 0) kept.prepend('1');
    }
    if (decimals == 0) kept.chop(1);                    // the point itself
    QString out = QString::fromLatin1(kept);
    if (neg && out.count(u'0') + out.count(u'.') != out.size()) out.prepend(u'-');   // JS: (-0).toFixed(2) is "0.00"
    return out;
}

QString groupedInt(qint64 n) {
    static const QLocale us(QLocale::English, QLocale::UnitedStates);
    return us.toString(n);
}

QString fmtUsd(double n) {
    return n >= 1 ? u'$' + toFixedJs(n, 2) : n >= 0.01 ? u'$' + toFixedJs(n, 3) : u'$' + toFixedJs(n, 4);
}

QString fmtSeconds(double s) {
    if (!std::isfinite(s)) return QStringLiteral("?");
    if (s < 90) return QStringLiteral("~%1s").arg(qint64(std::max(1.0, std::ceil(s))));
    return QStringLiteral("~%1 min").arg(qint64(jsRound(s / 60)));
}

qint64 approxTokens(qint64 chars) { return qint64(std::ceil(double(chars) / 3.6)); }

// ---------------- local ----------------

const LocalDefaults& localDefaultThroughput() {
    static const LocalDefaults d;
    return d;
}

LocalTextTime localTextSeconds(const ModelInfo& m, double inTok, double outTok) {
    const auto r = localRate(m, &Throughput::tokensPerSec, localDefaultThroughput().tokensPerSec);
    return {kLocalOverheadS + inTok / (r.rate * kPrefillFactor) + outTok / r.rate, r.measured, r.rate};
}

// ---------------- text ----------------

std::optional<double> reasoningFactor(QStringView effort) {
    static const QHash<QString, double> f = {{QStringLiteral("none"), 0},     {QStringLiteral("minimal"), 0.3}, {QStringLiteral("low"), 0.7},
                                             {QStringLiteral("medium"), 1.6}, {QStringLiteral("high"), 3},      {QStringLiteral("xhigh"), 4.5},
                                             {QStringLiteral("max"), 5.5}};
    const auto it = f.constFind(effort.toString());
    return it == f.cend() ? std::nullopt : std::optional<double>(it.value());
}

QString chooseEffort(const ModelInfo& m, QStringView desired) {
    if (!m.reasoning) return {};
    const QStringList& listed = m.reasoning->efforts;
    const bool canDisable = !listed.isEmpty() ? listed.contains(QLatin1String("none")) : !m.reasoning->mandatory;
    if (desired == u"none" && canDisable) return QStringLiteral("none");
    const QStringList supported = !listed.isEmpty() ? listed : QStringList{QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high")};
    QStringList opts;
    for (const QString& e : supported)
        if (e != QLatin1String("none")) opts << e;
    const auto idx = [](const QString& e) { return effortOrder().indexOf(e); };
    std::stable_sort(opts.begin(), opts.end(), [&idx](const QString& a, const QString& b) { return idx(a) < idx(b); });
    if (opts.isEmpty()) return {};
    const qsizetype want = effortOrder().indexOf(desired.toString());
    QString best = opts[0];
    double bestD = kInf;
    for (const QString& o : opts) {
        const double d = std::abs(double(idx(o) - want));
        if (d < bestD) { best = o; bestD = d; }
    }
    return best;
}

double searchRequestUsd(const ModelInfo& m, int results) {
    constexpr double kExaRequest = 0.007, kExaExtraResult = 0.001, kNativeUnlisted = 0.035;
    static const QRegularExpression native(QStringLiteral("^(openai|anthropic|google|x-ai)/"));
    if (m.price.webSearch.value_or(0) > 0) return *m.price.webSearch;
    if (native.match(m.id).hasMatch()) return kNativeUnlisted;
    return kExaRequest + std::max(0, results - 10) * kExaExtraResult;
}

CostEstimate textCost(const ModelInfo& m, qint64 inTok, qint64 outTok, const TextOptions& opts) {
    QString effort;
    if (!opts.reasoningIncluded) {
        if (!opts.effort.isEmpty()) effort = chooseEffort(m, opts.effort);
        else if (m.reasoning && m.reasoning->defaultEnabled) effort = QStringLiteral("medium");
    }
    const double hidden = !effort.isEmpty() ? double(outTok) * reasoningFactor(effort).value_or(1) : 0;
    const QString reasoningText = !effort.isEmpty() ? QStringLiteral(" (reasoning:%1)").arg(effort) : QString();
    if (m.isLocal()) {
        const LocalTextTime t = localTextSeconds(m, double(inTok), double(outTok) + hidden);
        return localEstimate(t.seconds, t.measured,
                             QStringLiteral("%1 in + %2 out%3 at %4 tok/s").arg(groupedInt(inTok), roundedInt(double(outTok) + hidden), reasoningText,
                                                                               QString::number(qint64(jsRound(t.rate)))));
    }
    const double search = opts.searches ? opts.searches * searchRequestUsd(m, opts.searchResults.value_or(5)) : 0;
    const double total = double(inTok) * m.price.in + (double(outTok) + hidden) * m.price.out + search;
    return estimate(total, hidden > 0 || search > 0 ? Confidence::Approx : Confidence::Exact,
                    QStringLiteral("%1 in + %2 out%3%4").arg(groupedInt(inTok), roundedInt(double(outTok) + hidden), reasoningText,
                                                         opts.searches ? QStringLiteral(" + %1 searches").arg(opts.searches) : QString()));
}

// ---------------- image ----------------

bool imagePriceKnown(const ModelInfo& m) {
    const auto& s = m.image;
    return s && (s->perImage || s->perMegapixel || s->perOutputToken.value_or(0) != 0 || s->perInputToken.value_or(0) != 0);
}

CostEstimate imageCost(const ModelInfo& m, const ImageOptions& o) {
    if (m.isLocal()) {
        const auto r = localRate(m, &Throughput::secondsPerImage, localDefaultThroughput().secondsPerImage);
        const double q = o.quality == QLatin1String("low") ? 0.6 : o.quality == QLatin1String("high") ? 1.4 : 1;   // smaller / larger renders
        return localEstimate(kLocalOverheadS + o.n * r.rate * q, r.measured, QStringLiteral("%1 × %2/image (%3)").arg(o.n).arg(fmtSeconds(r.rate * q), o.quality));
    }
    // pricing in a unit the catalog does not parse is UNKNOWN, not free: assume a typical price so caps and approval
    // apply (an explicit ":free" model id stays free)
    if (!imagePriceKnown(m) && !m.id.endsWith(QLatin1String(":free")))
        return estimate(kUnknownImageUsd * o.n, Confidence::Approx, QStringLiteral("%1 × %2/image (price not published: assumed)").arg(o.n).arg(fmtUsd(kUnknownImageUsd)));
    if (!m.image) return estimate(0, Confidence::Approx, QStringLiteral("no image pricing"));
    const ImageSpec& s = *m.image;
    if (s.perImage) return estimate(*s.perImage * o.n, Confidence::Exact, QStringLiteral("%1 × %2/image").arg(o.n).arg(fmtUsd(*s.perImage)));
    if (s.perMegapixel)
        return estimate(*s.perMegapixel * 1.5 * o.n, Confidence::Approx, QStringLiteral("%1 × ~1.5MP @ %2/MP").arg(o.n).arg(fmtUsd(*s.perMegapixel)));
    static const QHash<QString, int> tokensPerImage = {{QStringLiteral("low"), 300}, {QStringLiteral("auto"), 1200}, {QStringLiteral("medium"), 1200}, {QStringLiteral("high"), 4300}};
    const int tok = tokensPerImage.value(o.quality, 1200);
    const double c = (s.perOutputToken.value_or(0) * tok + s.perInputToken.value_or(0) * 200) * o.n;
    return estimate(c, Confidence::Approx, QStringLiteral("%1 × ~%2 output tokens (%3)").arg(o.n).arg(tok).arg(o.quality));
}

// ---------------- video ----------------

std::optional<VideoRate> videoRate(const ModelInfo& m, QStringView res, bool audio) {
    if (!m.video) return std::nullopt;
    const QJsonObject& sk = m.video->skus;
    const QString r = res.toString().toLower();
    const QString a = audio ? QStringLiteral("with_audio") : QStringLiteral("without_audio");
    const auto cents = [](const QString& k) { return k.startsWith(QLatin1String("cents_")) || k.contains(QLatin1String("cents_per")) ? 0.01 : 1.0; };
    // `sk.minimum_cents_per_generation ? ... : undefined`: a JS-truthy value (a non-empty string, a non-zero number)
    std::optional<double> minTotal;
    const QJsonValue minV = sk.value(QLatin1String("minimum_cents_per_generation"));
    if ((minV.isString() && !minV.toString().isEmpty()) || (minV.isDouble() && minV.toDouble() != 0) || (minV.isBool() && minV.toBool()))
        minTotal = num(minV) / 100;
    const QStringList direct = {QStringLiteral("duration_seconds_%1_%2").arg(a, r),   QStringLiteral("duration_seconds_%1").arg(a),
                                QStringLiteral("duration_seconds_%1").arg(r),         QStringLiteral("text_to_video_duration_seconds_%1").arg(r),
                                QStringLiteral("duration_seconds"),                   QStringLiteral("cents_per_second_output_%1").arg(r),
                                QStringLiteral("cents_per_second_output"),            QStringLiteral("cents_per_video_output_second_%1").arg(r)};
    for (const QString& k : direct)
        if (sk.contains(k)) return VideoRate{num(sk.value(k)) * cents(k), false, minTotal, k};
    static const QHash<QString, QPair<int, int>> resPixels = {{QStringLiteral("480p"), {854, 480}},   {QStringLiteral("720p"), {1280, 720}},
                                                              {QStringLiteral("768p"), {1366, 768}},  {QStringLiteral("1080p"), {1920, 1080}},
                                                              {QStringLiteral("2k"), {2048, 1152}},   {QStringLiteral("4k"), {3840, 2160}}};
    const QStringList tokenKeys = {QStringLiteral("video_tokens_%1").arg(r), audio ? QStringLiteral("video_tokens") : QStringLiteral("video_tokens_without_audio"),
                                   QStringLiteral("video_tokens")};
    for (const QString& k : tokenKeys) {
        if (sk.contains(k)) {
            const auto [w, h] = resPixels.value(r, {1280, 720});
            return VideoRate{num(sk.value(k)) * ((double(w) * h * 24) / 1024), true, minTotal, k};
        }
    }
    for (auto it = sk.constBegin(); it != sk.constEnd(); ++it)   // sorted keys (see catalog.h); the TS takes the first listed
        if (it.key().contains(QLatin1String("duration_seconds")) || it.key().contains(QLatin1String("second")))
            return VideoRate{num(it.value()) * cents(it.key()), true, minTotal, it.key()};
    return std::nullopt;
}

CostEstimate videoCost(const ModelInfo& m, const VideoOptions& o) {
    if (m.isLocal()) {
        const auto r = localRate(m, &Throughput::secondsPerVideoSecond, localDefaultThroughput().secondsPerVideoSecond);
        const QString res = o.resolution.toLower();
        const double f = res == QLatin1String("1080p") ? 2.5 : res == QLatin1String("720p") ? 1.6 : 1;   // calibrated at 480p
        return localEstimate(kLocalOverheadS + o.seconds * r.rate * f, r.measured,
                             QStringLiteral("%1s @ %2 × %3 per video-second").arg(n2s(o.seconds), o.resolution, fmtSeconds(r.rate * f)));
    }
    const auto r = videoRate(m, o.resolution, o.audio);
    if (!r) return estimate(kInf, Confidence::Approx, QStringLiteral("no video pricing"));
    const double total = std::max(r->perSec * o.seconds, r->minTotal.value_or(0));
    return estimate(total, r->approx ? Confidence::Approx : Confidence::Exact,
                    QStringLiteral("%1s @ %2%3 × %4/s").arg(n2s(o.seconds), o.resolution, o.audio ? QStringLiteral("+audio") : QString(), fmtUsd(r->perSec)));
}

// ---------------- speech / transcription / embeddings / rerank ----------------

CostEstimate ttsCost(const ModelInfo& m, qint64 chars) {
    if (m.isLocal()) {
        const auto r = localRate(m, &Throughput::secondsPerSpokenSecond, localDefaultThroughput().secondsPerSpokenSecond);
        const double spoken = double(chars) / kCharsPerSpokenSecond;
        return localEstimate(kLocalOverheadS + spoken * r.rate, r.measured,
                             QStringLiteral("%1 chars → ~%2s audio at %3× real time").arg(groupedInt(chars), QString::number(qint64(jsRound(spoken))), toFixedJs(r.rate, 2)));
    }
    if (m.price.out > 0) {
        // token-priced TTS (Gemini-style): text tokens in, ~32 audio tokens per second out
        const double secs = double(chars) / kCharsPerSpokenSecond;
        return estimate(double(approxTokens(chars)) * m.price.in + secs * 32 * m.price.out, Confidence::Approx,
                        QStringLiteral("%1 chars → ~%2s audio (token-priced)").arg(chars).arg(qint64(jsRound(secs))));
    }
    return estimate(double(chars) * m.price.in, Confidence::Exact, QStringLiteral("%1 chars × %2/char").arg(groupedInt(chars), n2s(m.price.in)));
}

CostEstimate sttCost(const ModelInfo& m, double seconds) {
    if (m.isLocal()) {
        const auto r = localRate(m, &Throughput::secondsPerAudioMinute, localDefaultThroughput().secondsPerAudioMinute);
        return localEstimate(kLocalOverheadS + (seconds / 60) * r.rate, r.measured,
                             QStringLiteral("%1s of audio at %2 per minute").arg(qint64(jsRound(seconds))).arg(fmtSeconds(r.rate)));
    }
    // Most STT is priced per second; a few outliers list per-hour figures (> $0.01/s is not plausible per second).
    const double perSec = m.price.in > 0.01 ? m.price.in / 3600 : m.price.in;
    const bool tokenPriced = m.price.out > 0;
    const double c = tokenPriced ? seconds * 10 * m.price.in + (seconds * 3) * m.price.out : seconds * perSec;
    return estimate(c, Confidence::Approx, QStringLiteral("%1s of audio").arg(qint64(jsRound(seconds))));
}

CostEstimate embedCost(const ModelInfo& m, qint64 tokens) {
    if (m.isLocal()) {
        // embedding models read at prefill speed; measured as tokensPerSec of the embedding call itself
        const auto r = localRate(m, &Throughput::tokensPerSec, localDefaultThroughput().tokensPerSec);
        return localEstimate(1 + double(tokens) / (r.rate * (r.measured ? 1 : kPrefillFactor)), r.measured, QStringLiteral("%1 tokens").arg(groupedInt(tokens)));
    }
    return estimate(double(tokens) * m.price.in, Confidence::Exact, QStringLiteral("%1 tokens").arg(groupedInt(tokens)));
}

CostEstimate rerankCost(const ModelInfo& m) {
    if (m.isLocal()) {
        const auto r = localRate(m, &Throughput::tokensPerSec, localDefaultThroughput().tokensPerSec);
        return localEstimate(1 + 2500 / (r.rate * (r.measured ? 1 : kPrefillFactor)), r.measured, QStringLiteral("1 rerank request (embeddings + cosine)"));
    }
    // Catalog lists 0 for most rerankers (billed per search); assume a typical ~$2 / 1k searches.
    return estimate(m.price.in > 0 ? m.price.in * 2000 : 0.002, Confidence::Approx, QStringLiteral("1 rerank request"));
}

} // namespace studio::catalog
