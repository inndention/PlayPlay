#ifndef PLAYPLAY_QUERYBLOCKS_H
#define PLAYPLAY_QUERYBLOCKS_H

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QHash>
#include <QJsonArray>
#include <QStringList>
#include <functional>
#include "schedulemodel.h"

namespace sched {

/**
 * Query blocks and folder blocks — late binding of schedule items to media.
 *
 * A QUERY block is a playlist item that says WHAT should play ("10 minutes of
 * programs, least played", "the newest weather spot from the autogen rotation")
 * instead of WHICH file. It is resolved when the hour is built, so the same
 * hour shape keeps producing fresh air week after week (PlayPlay OBS
 * queryblocks.py). A FOLDER block is the "drag a folder into the playlist"
 * behaviour: the pool is a folder (or a category/subcategory/tag acting as
 * one) and the pick is random-but-fair, or ordered (name/date/size round robin).
 *
 * Resolution rules (ported):
 *   - channel scope (allowedChannels), availability window, archived, path set;
 *   - type / category / subcategory / series filters (empty = any);
 *   - `prefer` orders candidates; recently aired (< noRepeatMin) fall behind
 *     fresh picks but stay eligible — a tiny pool must still resolve;
 *   - ids already placed this hour (`exclude`) are skipped;
 *   - a SERIES block selects positionally (nthLatest) in episode order and
 *     ignores exclusions: "latest" and "last week's" must coexist in one hour;
 *   - a block with targetSec > 0 takes items until the target is covered
 *     (last pick may be shorter); when nothing fits, the best candidate is
 *     taken anyway and flagged `overrun`; RunlistBuilder::expandQuery then cuts
 *     it to the block target (outPoint) so the hour keeps its shape;
 *   - `avoid` (a re-roll) ranks the previous picks behind every other candidate,
 *     so a deterministic ordering moves on to the next best one;
 *   - ad-contract pacing (`contractBonus`) floats behind-schedule campaigns
 *     to the top of their tier.
 */
/**
 * Stackable conditions shared by query and folder blocks.
 *   maxAgeDays / minAgeDays   newer than / older than N days, measured on `ageField`
 *                             ("added" = entered the library, "episode" = episode date)
 *   rules                     [{field, op, value}] combined by `match` ("all" | "any"); a rule may itself be a
 *                             group {match, rules:[...]} and any rule or group may carry "not": true, so conditions
 *                             stack and nest: (news OR weather) AND newer than 14 days AND NOT tagged "expired".
 * Fields: type category subcategory series client title filename path tag codec | duration (s) durationMin
 *         ageDays episodeAgeDays lastPlayedDays (never aired = infinite) playCount height width priority
 * Ops:    is is_not contains not_contains starts_with ends_with in not_in | lt lte gt gte between | exists empty
 */
struct Conditions {
    double maxAgeDays = 0.0, minAgeDays = 0.0;   // 0 = no limit
    QString ageField = "added";
    QJsonArray rules;
    QString match = "all";

    bool isEmpty() const { return maxAgeDays <= 0.0 && minAgeDays <= 0.0 && rules.isEmpty(); }
    bool matches(const MediaRecord& m, qint64 nowMs) const;
    static bool ruleMatches(const MediaRecord& m, const QJsonObject& rule, qint64 nowMs);
    static Conditions fromJson(const QJsonObject& o);
    void writeJson(QJsonObject& o) const;
    QString label() const;           // "<=14d, 2 rules"
    QStringList errors() const;      // unknown fields / ops, for the validator and the editors
};

struct QueryBlock {
    QStringList types;             // media types (program|ad|promo|news|weather|station_id|bumper|filler|psa|...)
    QStringList categories;        // MediaRecord.extra.category (case-insensitive)
    QStringList subcategories;     // MediaRecord.extra.subcategory
    QStringList series;            // MediaRecord.series (any of)
    QStringList tags;              // MediaRecord.extra.tags (any of)
    QStringList clients;           // MediaRecord.client (any of)
    double targetSec = 0.0;        // 0 = one item, whatever its length
    QString prefer = "least_played";   // even|newest|oldest|most_played|least_played|random|longest|shortest
                                       // even = strict rotation: longest since aired (or planned) first, so every
                                       // matching file plays once before any file plays twice
    int nthLatest = 0;             // series blocks: 0 latest, 1 previous ...
    double noRepeatMin = 30.0;     // recently aired within this window rank last
    double minSec = 0.0, maxSec = 0.0;   // optional per-item length bounds
    bool fitToTarget = true;       // prefer items that fit the remaining target
    Conditions cond;               // age limits + stacked rules (all must hold, on top of the lists above)

    bool isValid() const { return !types.isEmpty() || !categories.isEmpty() || !subcategories.isEmpty() || !series.isEmpty() || !tags.isEmpty() || !clients.isEmpty() || !cond.isEmpty(); }
    static QueryBlock fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    QString label() const;         // "Programs ~10m (least played)"
};

struct FolderBlock {
    QString folder;                // a path prefix, or a category / subcategory / tag / series name
    double targetSec = 0.0;
    QString weight = "freshness";  // freshness|uniform (random modes)
    QString order = "random";      // random|even|name|date|size|added   (ordered modes play round robin; even = longest since aired)
    bool recursive = true;
    Conditions cond;               // age limits + stacked rules on top of the folder match

    bool isValid() const { return !folder.trimmed().isEmpty(); }
    static FolderBlock fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
    QString label() const;
};

struct QueryContext {
    QString channelId;
    qint64 nowMs = 0;
    QSet<QString> exclude;                             // media ids already placed in this hour (and the neighbours)
    QSet<QString> avoid;                               // soft exclusion: previous picks of a re-rolled block (ranked last, still eligible)
    quint32 seed = 0;                                  // deterministic randomness per hour/item
    std::function<double(const QString& mediaId)> contractBonus;   // ad pacing (0 when unset)
    QHash<QString, int> planned;                       // picks already made for other hours that have not aired yet (rotation)
};

struct QueryPick {
    MediaRecord media;
    bool overrun = false;          // longer than the room the block had left
    bool repeat = false;           // aired inside the no-repeat window
};

class QueryBlocks {
public:
    /// Playable right now on this channel (path, duration, availability window, channel scope, not missing/blocked).
    static bool eligible(const MediaRecord& m, const QueryContext& ctx);
    /// Slack a block allows around its target (10 %, at least 15 s); a first pick longer than target + tolerance is an overrun.
    static double tolerance(double targetSec) { return qMax(15.0, targetSec * 0.10); }
    static bool isOverrun(double durationSec, double targetSec) { return targetSec > 0.0 && durationSec > targetSec + tolerance(targetSec); }

    /// Eligibility + filters (no ordering).
    static bool matches(const MediaRecord& m, const QueryBlock& b, const QueryContext& ctx);
    static bool matchesFolder(const MediaRecord& m, const FolderBlock& f);

    /// Ranked candidates (best first), at most `n`.
    static QList<QueryPick> candidates(const QList<MediaRecord>& pool, const QueryBlock& b, const QueryContext& ctx, int n = 50);

    /// The block's picks covering targetSec (one item when targetSec == 0).
    static QList<QueryPick> resolve(const QList<MediaRecord>& pool, const QueryBlock& b, const QueryContext& ctx, QString* why = nullptr);
    static QList<QueryPick> resolveFolder(const QList<MediaRecord>& pool, const FolderBlock& f, const QueryContext& ctx, QString* why = nullptr);
    /// One pass of resolveFolder (no repeat fallback).
    static QList<QueryPick> resolveFolderOnce(const QList<MediaRecord>& pool, const FolderBlock& f, const QueryContext& ctx, QString* why = nullptr);

    /// Normalisation helpers shared with the format clock.
    static QString norm(const QString& s) { return s.trimmed().toLower(); }
    static bool inList(const QString& v, const QStringList& l);
    static QString category(const MediaRecord& m) { return m.extra.value("category").toString(); }
    static QString subcategory(const MediaRecord& m) { return m.extra.value("subcategory").toString(); }
    static QStringList tags(const MediaRecord& m);
    static QStringList listOf(const QJsonObject& o, const char* a, const char* b = nullptr);
};

} // namespace sched

#endif // PLAYPLAY_QUERYBLOCKS_H
