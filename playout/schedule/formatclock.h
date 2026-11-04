#ifndef PLAYPLAY_FORMATCLOCK_H
#define PLAYPLAY_FORMATCLOCK_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QStringList>
#include <functional>
#include "schedulemodel.h"
#include "queryblocks.h"

namespace sched {

/**
 * FormatClock — how an hour gets its shape without an operator dragging files.
 *
 * Three PlayPlay OBS mechanisms, ported:
 *
 *  HOUR RECIPE   a repeating pattern of TIME, not of files: "10 min programs, 5 min ads,
 *                3 min autogen, 1 min IDs, go round again". Each part becomes a query
 *                block, laid until the hour is full; locked items of the existing hour
 *                survive (leads first, anchors at their offsets). Nothing is bound to a
 *                file: the blocks resolve at air through RunlistBuilder/QueryBlocks.
 *
 *  WEIGHTED FILL the "format clock" proper: program-mix buckets with airtime shares
 *                (Programs 60 %, Ads 30 %, IDs 10 %). A rotation queue per bucket
 *                (least played → freshness → seeded jitter), deficit round robin by
 *                SECONDS so a 4-minute music video and a 30-second spot are weighed
 *                honestly, anchors reserved, one program per hour with long programs
 *                segmented and carried into the next hour. Produces concrete media.
 *
 *  AUTOFILL      per-channel policy for hours that have nothing: lay the default recipe
 *                (late bound) or a weighted fill (seeded by the hour) when the executor
 *                builds the hour, and the typed order + no-repeat window + ad-contract
 *                pacing used when picking interstitials at air.
 */
struct RecipePart {
    QString label;
    QStringList types;
    double minutes = 0.0;
    QString prefer = "least_played";
    QStringList categories, subcategories, series, tags;
    bool anchorTop = false;        // place this part first in every cycle at the top of the hour only (IDs)
    QString folder;                // fill from a folder (path prefix) or a category / subcategory / tag / series name
    Conditions cond;               // age limits ("newer than 14 days") and stacked rules, carried into the laid blocks

    static RecipePart fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    QueryBlock block() const;
};

struct HourRecipe {
    QString id, name, description;
    QList<RecipePart> parts;
    bool idAtTop = false;          // a station ID query block at :00 before the cycle
    QString idTypes = "station_id";

    static HourRecipe fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    double cycleSec() const;
    bool isValid() const { return !parts.isEmpty(); }
};

struct WeightBucket {
    QString label;
    QStringList types, categories, subcategories, tags;
    QString series;
    double pct = 0.0;              // airtime share of the hour
    int maxPerHour = 0;            // 0 = no cap

    static WeightBucket fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    bool matches(const MediaRecord& m) const;
};

struct AutofillPolicy {
    bool enabled = false;          // synthesize empty hours when the executor builds them
    QString mode = "recipe";       // recipe | weighted
    QString recipeId;              // "" = the default recipe
    QStringList order = {"ad", "promo", "bumper", "station_id", "filler"};   // interstitial pick order at air
    double noRepeatMin = 30.0;
    bool adPacing = true;          // ad-contract pacing bonus

    static AutofillPolicy fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
};

struct AdContract {
    QString id, client, title;
    QStringList mediaIds;
    int spotsTotal = 0;            // 0 = unlimited (pacing by spotsPerDay)
    int spotsPerDay = 0;
    QDate startDate, endDate;
    QStringList channels;          // empty = all
    QJsonArray dayparts;           // [{from:"06:00", to:"10:00"}]
    QString status = "active";     // active | paused | done
    int priority = 0;

    static AdContract fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    bool activeOn(const QDate& day, const QString& channelId) const;
};

/// Result of laying one hour.
struct HourLayout {
    QList<SlotItem> items;
    QStringList notes;
    double placedSec = 0.0;
    QJsonObject carry;             // long program continuation for the next hour (weighted fill)
    QJsonObject toJson() const;
};

class FormatClock {
public:
    // --- recipes ------------------------------------------------------------------
    static HourRecipe defaultRecipe();
    /// Ready-made hour shapes (ids "builtin-..."): breaks every N minutes, half-hour and long-form shows, fresh-only and
    /// classics, news at the top, a news/weather wheel, ad-heavy, programmes only, overnight filler. Read-only; copy to edit.
    static QList<HourRecipe> builtinRecipes();
    /// A recipe by id or name among the built-ins and `saved` ("default" = defaultRecipe). False when unknown.
    static bool findRecipe(const QString& idOrName, const QList<HourRecipe>& saved, HourRecipe* out);
    static QList<HourRecipe> recipesFromSettings(const QJsonObject& settings);      // settings "programming.recipes"
    static QJsonObject recipesToSettings(const QList<HourRecipe>& recipes);
    /// "Programs with ad breaks every N minutes": programMin of programs, adMin of ads (+ optional autogen and ID minutes).
    static HourRecipe adBreakRecipe(const QJsonObject& spec);
    /// Lay a recipe into one hour as query blocks, keeping the locked items of `existing`. A non-empty `idPrefix` makes the
    /// block ids deterministic ("<prefix>-q<n>", used when the executor synthesizes an hour on every rebuild); empty = random ids.
    /// The last gap of the hour is closed with the recipe's longest interstitial part that fits, never a cut program.
    static HourLayout layRecipe(const HourRecipe& r, const HourSlot& existing, bool keepLocks, int cycleLimit = 200, const QString& idPrefix = QString(),
                                double startSec = 0.0);   // startSec > 0: lay only the rest of the hour after that much carried-in content

    // --- programmes that run past the end of the hour ---------------------------------
    /// What to do with an item that ends after the top of the hour.
    ///   continue  plays in full; the next hour becomes a continuation that starts where it ends
    ///   split     breaks it at its segments (or every breakEverySec) with ad breaks; the hour ends exactly at :00 and the
    ///             remaining parts open the next hour
    ///   cut       ends it at the top of the hour (out point)
    /// `next` decides the rest of the next hour: keep (its own schedule follows), ads (ads and promos), recipe (laid from `recipe`).
    struct OverrunPlan {
        QString mode = "continue";
        QString next = "keep";
        HourRecipe recipe;
        QStringList adTypes = {"ad", "promo", "psa"};
        double breakSec = 120.0;
        double breakEverySec = 900.0;
        QJsonArray segments;          // split: the item's media segments [{startTime, endTime, label}]
    };
    struct OverrunResult {
        bool ok = false;
        QString error;
        HourSlot cur, next;           // both hours after the plan (the caller saves them)
        double overrunSec = 0.0;      // how far the item ran past the hour
        double carrySec = 0.0;        // continue: how far this hour's content runs into the next
        QStringList notes;
    };
    static OverrunResult planOverrun(const HourSlot& cur, const QString& itemId, const HourSlot& next, const OverrunPlan& plan);
    /// Seconds an item takes in the hour as laid in the editor (blocks: their target, media: in to out).
    static double slotItemSec(const SlotItem& it);

    // --- weighted fill --------------------------------------------------------------
    static QList<WeightBucket> defaultBuckets();
    static QList<WeightBucket> bucketsFromJson(const QJsonValue& v);
    static QJsonArray bucketsToJson(const QList<WeightBucket>& b);

    struct WeightedState {                        // rotation persists across the hours of one fill run
        QList<QList<MediaRecord>> queues;         // per bucket, front = next
        QSet<QString> usedRun;                    // ids placed anywhere in this run (soft no-repeat across hours)
        QJsonObject carry;
        int hoursDone = 0;
    };
    static WeightedState beginWeighted(const QList<WeightBucket>& buckets, const QList<MediaRecord>& pool,
                                       const QString& channelId, qint64 nowMs, quint32 seed);
    /// Fill one hour from the rotation; `existing` locks are kept (leads first, anchors at offsets). `idPrefix`: see layRecipe
    /// ("<prefix>-w<n>"). A program longer than `programCapSec` airs its leading segment; the rest is left in `st.carry` (and
    /// HourLayout::carry) and leads the next hour that is filled with the same state or with `st.carry` set from it.
    static HourLayout fillWeighted(const QList<WeightBucket>& buckets, WeightedState& st, const HourSlot& existing,
                                   bool keepLocks, bool oneProgramPerHour, double programCapSec = 2400.0, const QString& idPrefix = QString());

    // --- autofill / contracts -----------------------------------------------------------
    static AutofillPolicy policyFor(const ChannelProfile& p);
    static QList<AdContract> contractsFromSettings(const QJsonObject& settings);   // settings "traffic.contracts"
    static QJsonObject contractsToSettings(const QList<AdContract>& c);
    /// Pacing bonus per media id for `day`: positive when a campaign is behind its linear target, negative when fulfilled.
    /// The totals (spotsTotal / spotsPerDay) are the CONTRACT's: airings of all its creatives are summed and every creative
    /// gets the contract's bonus. `airedCount(mediaId, fromMs, toMs)` comes from the as-run table.
    static QHash<QString, double> contractPacing(const QList<AdContract>& contracts, const QString& channelId, const QDate& day,
                                                 const QTimeZone& tz, const std::function<int(const QString&, qint64, qint64)>& airedCount);
    static QJsonObject contractReport(const AdContract& c, const QDate& today, const std::function<int(const QString&, qint64, qint64)>& airedCount, const QTimeZone& tz);

    // --- locks ------------------------------------------------------------------------------
    static void splitLocks(const QList<SlotItem>& items, QList<SlotItem>* leads, QList<SlotItem>* anchors, QList<SlotItem>* free);
    /// Re-seat the locked items of `src` into a freshly built `dest` list (nearest-boundary anchors).
    static QList<SlotItem> carryLocks(QList<SlotItem> dest, const QList<SlotItem>& src);
    static int indexAtOffset(const QList<SlotItem>& items, double offsetSec);
    /// Where a locked item sits in the hour (seconds): its soft anchor, else the offset of its hard start, else -1.
    /// splitLocks, carryLocks and fillWeighted all order and place anchors by this one number.
    static double lockOffset(const SlotItem& it);
    static double itemSec(const SlotItem& it) { return it.isQuery() && it.durationSec > 0 ? it.durationSec : it.effectiveDurationSec(); }
    static QString newItemId(const QString& prefix = "q");
    /// Shuffle the unlocked interstitials of an hour (programs keep their order and positions).
    static QList<SlotItem> shuffleAds(const QList<SlotItem>& items, quint32 seed);
};

} // namespace sched

#endif // PLAYPLAY_FORMATCLOCK_H
