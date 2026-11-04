#pragma once
// Cost estimators (ST-08): a port of starmedia src/pricing.ts. Everything works from the *published* per-unit prices
// in the catalog; where a unit conversion needs a guess (tokens per image, tokens per video-second) the estimate is
// tagged Approx. A local/* model costs $0 and is estimated in wall-clock seconds instead (conservative defaults until a
// calibration measured it).
//
// Parity: the same doubles, the same operation order and the same detail texts as the TS (test_studio_catalog checks
// every estimator against a golden the TS code produced for the same models). Text with JS number formatting goes
// through json::jsNumber; toFixed() is written out (toFixedJs) because the C runtime rounds exact ties to even where
// JS rounds them up; toLocaleString() of a whole number is en-US grouping ("12,345"), the TS test machine's locale.
#include "catalog/catalog.h"
#include "core/money.h"

#include <QString>
#include <QStringView>
#include <optional>

namespace studio::catalog {

enum class Confidence { Exact, Approx };
QString confidenceName(Confidence c);                   // "exact" | "approx"

struct CostEstimate {
    double usd = 0;                                     // may be +inf ("no video pricing")
    Confidence confidence = Confidence::Exact;
    QString detail;
    std::optional<double> seconds;                      // wall-clock seconds; set for local models

    bool finite() const;
    /// The amount in micro-USD (money::fromUsd); nullopt when not finite (never book an unknown price).
    std::optional<money::Micro> micro() const;
    QJsonObject toJson() const;
    static CostEstimate fromJson(const QJsonObject& o);
};

/// TS ZERO: {usd 0, exact, "local"}.
CostEstimate zeroEstimate();
/// Sum of the finite usd values / of the seconds that are set (TS sumUsd / sumSeconds).
double sumUsd(const QList<CostEstimate>& xs);
double sumSeconds(const QList<CostEstimate>& xs);

// ---------------- formatting (TS usd / fmtUsd / fmtSeconds) ----------------

/// JS Number.prototype.toFixed(d) for finite values (exact decimal expansion, ties rounded up).
QString toFixedJs(double v, int decimals);
/// JS toLocaleString() of a whole number in en-US: "1,234,567".
QString groupedInt(qint64 n);
/// "$12.34" from $1, "$0.123" from one cent, else "$0.0012" (pricing.ts usd / fmtUsd).
QString fmtUsd(double usd);
/// "?" when not finite, "~12s" under 90 s (at least 1), else "~10 min".
QString fmtSeconds(double s);
/// ceil(chars / 3.6)
qint64 approxTokens(qint64 chars);

// ---------------- local models: time instead of dollars ----------------

struct LocalDefaults {
    double tokensPerSec = 12;
    double secondsPerImage = 45;
    double secondsPerVideoSecond = 120;
    double secondsPerSpokenSecond = 0.6;
    double secondsPerAudioMinute = 15;
};
const LocalDefaults& localDefaultThroughput();
constexpr double kPrefillFactor = 8;                    // prefill runs this much faster than decoding
constexpr double kLocalOverheadS = 3;                   // model load / first-token latency per call

struct LocalTextTime { double seconds = 0; bool measured = false; double rate = 0; };
/// Seconds a local text model needs for a prompt + completion (hidden reasoning tokens included in outTok).
LocalTextTime localTextSeconds(const ModelInfo& m, double inTok, double outTok);

// ---------------- text ----------------

/// Hidden output tokens (relative to the visible output) per reasoning effort; nullopt for an unknown effort (the TS
/// `?? 1` then counts one hidden token per visible one).
std::optional<double> reasoningFactor(QStringView effort);
/// The desired effort snapped to one the model accepts; empty when the model has no reasoning control. "none" only when
/// the model lists it (or lists nothing and is not mandatory). Ties go to the cheaper effort.
QString chooseEffort(const ModelInfo& m, QStringView desired);

/// USD for ONE web search request by `m` returning up to `results` results: the model's listed price, else a native
/// provider's unlisted rate ($0.035), else Exa ($0.007 + $0.001 per result over 10).
double searchRequestUsd(const ModelInfo& m, int results = 5);

struct TextOptions {
    QString effort;                                     // empty = the model's default (medium when reasoning is on)
    int searches = 0;                                   // web search requests the call may make
    std::optional<int> searchResults;                   // results per search (default 5)
    bool reasoningIncluded = false;                     // outTok is the whole max_tokens, reasoning included
};
CostEstimate textCost(const ModelInfo& m, qint64 inTok, qint64 outTok, const TextOptions& opts = {});

// ---------------- image ----------------

constexpr double kUnknownImageUsd = 0.1;                // assumed per image when a paid model publishes no readable price
/// true when the catalog read an image price for this model (a published $0 counts).
bool imagePriceKnown(const ModelInfo& m);
struct ImageOptions { int n = 1; QString quality = QStringLiteral("auto"); };
CostEstimate imageCost(const ModelInfo& m, const ImageOptions& o);

// ---------------- video ----------------

struct VideoRate {
    double perSec = 0;
    bool approx = false;
    std::optional<double> minTotal;
    QString key;                                        // the sku the rate came from
};
/// The per-second rate of `m` at `res` (with / without audio) from its pricing_skus; nullopt when none can be read.
std::optional<VideoRate> videoRate(const ModelInfo& m, QStringView res, bool audio);
struct VideoOptions { double seconds = 0; QString resolution; bool audio = false; };
CostEstimate videoCost(const ModelInfo& m, const VideoOptions& o);

// ---------------- speech / transcription / embeddings / rerank ----------------

constexpr double kCharsPerSpokenSecond = 15;
CostEstimate ttsCost(const ModelInfo& m, qint64 chars);
CostEstimate sttCost(const ModelInfo& m, double seconds);
CostEstimate embedCost(const ModelInfo& m, qint64 tokens);
CostEstimate rerankCost(const ModelInfo& m);

} // namespace studio::catalog
