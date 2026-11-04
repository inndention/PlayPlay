#ifndef PLAYPLAY_RUNLISTBUILDER_H
#define PLAYPLAY_RUNLISTBUILDER_H

#include <QJsonArray>
#include <QJsonObject>
#include <functional>
#include <QHash>
#include <QSet>
#include "schedulemodel.h"
#include "fillpolicy.h"
#include "queryblocks.h"

namespace sched {

/**
 * RunlistItem — one playable entry with absolute planned times.
 *
 * RunlistBuilder lays an HourSlot onto the wall clock:
 *   soft_start   follows the previous item (or the hour start).
 *   hard_start   anchored at hourStart + fixedOffsetSec (or fixedUtcMs); whatever
 *                precedes it is truncated to make room; a gap before it is filled.
 *   fill_until   stretches (loops the media) to the next anchor or the hour end.
 * Items that overrun the end of the hour are truncated unless the following hour is a
 * continuation of a merged multi-hour event. Smart slots are resolved through the
 * MediaLookup callbacks. Gaps at the end of the hour are filled by FillPolicy.
 */
struct RunlistItem {
    QString runlistId;          // unique per push: "<slotId>#<itemId>#<n>"
    SlotItem item;              // resolved (path, title, duration filled in)
    qint64 plannedStartMs = 0;
    qint64 plannedEndMs = 0;
    double inSec = 0.0;         // effective in point
    double outSec = 0.0;        // effective out point (0 = media end)
    double durationSec = 0.0;   // planned on-air duration
    bool hard = false;          // engine hardStartUtcMs set
    bool truncated = false;
    bool loop = false;
    QString origin = "schedule";  // schedule|fill|emergency|smart|continuation
    QString slotId;
    QString note;

    QJsonObject toEngineJson() const;   // what PlayoutEngine::Item::fromJson expects
    QJsonObject toJson() const;         // UI-facing (superset)
};

struct Runlist {
    QString channelId;
    QString slotId;
    qint64 hourStartMs = 0;
    qint64 hourEndMs = 0;
    QList<RunlistItem> items;
    QList<RunlistItem> dropped; // events that did not fit and were left out (origin "dropped", note = why, durationSec = their full length):
                                // they are overrun the flight plan counts, even though nothing airs
    QStringList notes;          // human-readable decisions (truncations, fills, failures)
    double scheduledSec = 0.0;  // playable seconds from the slot itself
    double filledSec = 0.0;     // seconds added by fill
    double gapSec = 0.0;        // seconds left unfilled

    QJsonArray toEngineJson() const;
    QJsonObject toJson() const;
};

struct MediaLookup {
    std::function<MediaRecord(const QString& id, bool* found)> byId;
    std::function<QList<MediaRecord>(const QString& series)> bySeries;     // newest first
    std::function<QList<MediaRecord>(const QStringList& types)> byTypes;   // fill pool
    std::function<QList<MediaRecord>()> all;                               // query/folder block pool (optional: byTypes is used when unset)
};

struct BuildOptions {
    FillSettings fill;
    bool fillGaps = true;
    bool continuesIntoNextHour = false;   // next hour slot is a continuation: do not truncate at hour end
    bool continuesFromPreviousHour = false; // this slot is a continuation: no hard start at hour start
    double startOffsetSec = 0.0;          // continuation: the previous hour's programme runs this far into this hour,
                                          // so this hour's own items are laid out after it instead of on top of it
    double minItemSec = 0.5;
    qint64 nowMs = 0;                     // for availability checks and fill weighting
    quint32 fillSeed = 0;
    // Late-bound blocks: picks are cached per hour+item so an hour keeps its picks while it airs
    // (key "<hourStartMs>#<itemId>" -> media ids, plus "<key>@spec" -> a hash of the block spec: an edited block, a pick
    // that stopped matching or one another item now holds re-resolves the block instead of reusing stale picks);
    // `excludeIds` are what the neighbouring hour already took; `contractBonus` is the ad-pacing score
    // (FormatClock::contractPacing).
    QHash<QString, QStringList>* pickCache = nullptr;
    QSet<QString> excludeIds;
    std::function<double(const QString& mediaId)> contractBonus;
    // Re-roll: `seedSalt` (the channel's re-roll epoch) changes every block's seed, and `avoidPicks`
    // ("<hourStartMs>#<itemId>" -> the picks being replaced) ranks those media last when the block resolves again.
    // An entry is consumed (removed) when its block re-resolves.
    quint32 seedSalt = 0;
    QHash<QString, QStringList>* avoidPicks = nullptr;
};

class RunlistBuilder {
public:
    static Runlist build(const HourSlot& slot, qint64 hourStartMs, const MediaLookup& media, const BuildOptions& opt);

    /// Resolve a smart slot or bare mediaId into a concrete SlotItem (path/title/duration filled).
    /// Query/folder blocks return true untouched: they expand in build() (see expandQuery).
    static bool resolveItem(SlotItem& item, const MediaLookup& media, qint64 nowMs, QString* why = nullptr);

    /// Expand a query/folder block item into concrete items (one per pick). Returns an empty list with `why` set when nothing matches.
    /// A first pick longer than the block's target (nothing fitted) is cut to the target (outPoint); cached picks (BuildOptions::pickCache)
    /// are reused only while the spec is unchanged and each pick still matches, is playable and is not in `exclude`.
    static QList<SlotItem> expandQuery(const SlotItem& item, const MediaLookup& media, const BuildOptions& opt, const QString& channelId,
                                       qint64 hourStartMs, QSet<QString>& exclude, QString* why = nullptr);

    /// Soft anchors (locked items with lockAtSec): move each to the item boundary nearest its offset (nothing is cut).
    static void seatAnchors(QList<SlotItem>& items);

    /// Hour start in UTC ms for a local date/hour in the given time zone.
    static qint64 hourStartUtcMs(const QDate& localDate, int hour, const QTimeZone& tz);
    static qint64 hourStartUtcMsForTime(qint64 utcMs, const QTimeZone& tz, QDate* localDate = nullptr, int* hour = nullptr);
};

} // namespace sched

#endif // PLAYPLAY_RUNLISTBUILDER_H
