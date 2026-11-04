#pragma once
// Role-based model selection under a budget (ST-08): a port of starmedia src/selector.ts and the Role / Medium /
// RunOptions pieces of src/types.ts. A stage names a role (writer, judge, image, tts ...); eligible() filters the
// catalog by the role's hard requirements (kind, context, tools, reference images ...), the run's options (free-only,
// data collection, exclusions, local policy) and the per-role price-rank cut; pickModel() takes the highest-ranked
// model whose estimate fits the allowance, with up to two fallbacks from other providers; cheapestModel() is the
// planner's last resort; pinnedPick() / pinIneligible() handle a model somebody chose by hand.
//
// The rank key: quality (+ a price-rank lean from the cost slider), or with per-medium tiers quality × weight + price
// rank × (tier - 0.5) × 0.8, plus the bounded Fast ↔ Detailed term (speedBias) and +10 for local models under policy
// "prefer". Ties keep catalog order (a stable sort, as V8's), then the cheaper, then the quicker estimate. Every
// formula is the TS one, so the picks equal the TS goldens.
#include "catalog/catalog.h"
#include "catalog/pricing.h"

#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <functional>
#include <optional>

namespace studio::catalog {

enum class Role { Writer, Editor, Judge, Reasoner, Research, Fast, Vision, Image, ImageEdit, Video, VideoI2V, VideoEdit, Tts, TtsClone, Stt, Embed, Rerank };
QString roleName(Role r);                               // "writer" ... "imageEdit", "videoI2V", "ttsClone"
std::optional<Role> roleFromName(QStringView name);
const QList<Role>& allRoles();
Kind roleKind(Role r);

enum class Medium { Text, Image, Video, Audio };
QString mediumName(Medium m);
std::optional<Medium> mediumFromName(QStringView name);
/// image → image, video → video, speech / stt → audio, else text (TS mediumOf).
Medium mediumOf(Kind k);

/// Policy for models served by this machine (local/*): off | auto (default) | prefer | only.
enum class LocalMode { Off, Auto, Prefer, Only };
QString localModeName(LocalMode m);
std::optional<LocalMode> localModeFromName(QStringView name);

/// The selection-relevant part of the TS RunOptions (sliders, pins, exclusions, policies, tiers, speed).
struct RunOptions {
    double cost = 50;                                   // sliders.cost 0..100 (0 = free-tier only)
    double complexity = 50;                             // sliders.complexity
    bool allowFree = false;                             // :free models when cost > 0
    bool denyDataCollection = false;                    // free endpoints typically retain prompts
    QMap<Role, QString> pins;                           // role → model id
    QStringList exclude;                                // model-id substrings
    LocalMode local = LocalMode::Auto;
    /// Cost ↔ Quality per medium 0..100; set (even empty) = the dial replaces the cost slider in ranking
    std::optional<QMap<Medium, double>> tiers;
    /// Fast ↔ Detailed per medium 0..100; set (even empty) = the speed term is on (50 or a missing medium = no lean)
    std::optional<QMap<Medium, double>> speed;

    /// The TS object shape: {sliders:{cost,complexity}, allowFree?, denyDataCollection?, pins?, exclude?, local?,
    /// tiers?, speed?}; unknown roles / media / modes in the input are dropped.
    QJsonObject toJson() const;
    static RunOptions fromJson(const QJsonObject& o);
};

struct PickCtx {
    const Catalog* catalog = nullptr;
    RunOptions opts;
    bool freeOnly = false;                              // the cost slider is at 0
    double nowSec = 0;                                  // for expiry checks (TS Date.now() / 1000); 0 = the system clock
};

using Estimator = std::function<CostEstimate(const ModelInfo&)>;

struct Pick {
    ModelInfo model;
    CostEstimate estimate;
    QStringList fallbacks;
    bool pinned = false;
};

// ---------------- dials ----------------

LocalMode localModeOf(const RunOptions& opts);
constexpr double kPreferLocalBoost = 10;
/// token-priced speech models with no voice catalog are audio generators (music / SFX), never narration
bool isNarrationTts(const ModelInfo& m);
/// Cost ↔ Quality for a kind: 0 = cheap end, 1 = flagship (the medium's tier, else the cost slider), / 100.
double tierFor(const RunOptions& opts, Kind kind);
/// Weight of the quality score in the rank key: 0.25 + 0.75 × tier with tiers, else 1.
double qualityWeight(const RunOptions& opts, Kind kind);
/// -1 = a speed-oriented variant (turbo, flash, mini ...), 1 = a quality-oriented one (pro, hd, ultra ...), 0 =
/// neither or both; whole words of the id without its provider, else of the display name without "Provider: ".
int variantLean(QStringView id, QStringView name = {});
/// Fast ↔ Detailed for a kind 0..1; nullopt when not set for its medium.
std::optional<double> speedFor(const RunOptions& opts, Kind kind);
constexpr double kSpeedWeight = 0.12;
/// The bounded Fast ↔ Detailed ranking term (kSpeedWeight × qualityWeight × (2s - 1) × lean), 0 when off.
double speedBias(const RunOptions& opts, const ModelInfo& m);

// ---------------- selection ----------------

/// Whether a model meets the role's hard requirement (ROLES[role].need).
bool roleNeeds(Role role, const ModelInfo& m);
/// The role's share of the price range (ROLES[role].maxPriceRank).
double roleMaxPriceRank(Role role);

struct EligibleOptions { bool json = false; };
/// The catalog's models that can do `role` under the run's options (TS eligible), in catalog order.
QList<const ModelInfo*> eligible(Role role, const PickCtx& ctx, const EligibleOptions& o = {});

/// A local pick fits when its estimated wall time is within what is left of the time budget (hosted picks always fit).
bool fitsTime(const ModelInfo& m, const CostEstimate& est, std::optional<double> secondsLeft);

/// Why a model picked by hand cannot do `role` right now; empty when it can (TS pinIneligible). `m` null = not in the
/// catalog (`id` names it in the reason).
QString pinIneligible(Role role, const PickCtx& ctx, const ModelInfo* m, QStringView id = {});

struct PinnedResult { std::optional<Pick> pick; QString reason; };
/// Exactly the pinned model (never a substitute), its estimate and the listed fallbacks that pass the same checks;
/// `reason` says why there is no pick.
PinnedResult pinnedPick(Role role, const PickCtx& ctx, QStringView id, const Estimator& estimate, const QStringList& fallbackIds = {});

struct PickOptions {
    bool json = false;
    QSet<QString> exclude;                              // ids to pass over this time
    std::optional<QSet<QString>> distinctProviderFrom;  // prefer a model from another provider than these
    std::optional<double> secondsLeft;                  // time budget left for local picks
};
/// The highest-ranked eligible model whose estimate fits `allowance` USD (TS pickModel); nullopt when nothing fits.
std::optional<Pick> pickModel(Role role, const PickCtx& ctx, double allowance, const Estimator& estimate, const PickOptions& o = {});

struct CheapestOptions { bool json = false; std::optional<double> secondsLeft; };
/// The cheapest eligible model (TS cheapestModel): local ones over the time budget are passed over while a hosted
/// option exists.
std::optional<Pick> cheapestModel(Role role, const PickCtx& ctx, const Estimator& estimate, const CheapestOptions& o = {});

} // namespace studio::catalog
