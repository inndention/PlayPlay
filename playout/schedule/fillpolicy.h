#ifndef PLAYPLAY_FILLPOLICY_H
#define PLAYPLAY_FILLPOLICY_H

#include <QList>
#include <QStringList>
#include <QRandomGenerator>
#include "schedulemodel.h"

namespace sched {

/**
 * FillPolicy — picks interstitial content for gaps and the emergency loop.
 *
 * Weighting merges the two reference apps:
 *   PlayPlayClean rankMediaForSmartFill: freshness, priority x4, series boost,
 *   -playCount x8; trafficEngine recency multipliers (<1 h x0.7, <4 h x0.9).
 *   PlayPlayPy: typed emergency order station_id, bumper, filler, promo.
 *
 * Selection is weighted random without repeats until the gap is filled, with
 * a station ID forced first when `idFirst` is set (legal ID at top of hour).
 */
struct FillSettings {
    bool fillWithIds = true;
    bool fillWithBumpers = true;
    bool fillWithFiller = true;
    bool fillWithPromos = true;
    bool fillWithPsa = true;
    double maxFillSec = 3600.0;      // cap per gap
    double minGapSec = 2.0;          // ignore gaps shorter than this
    bool idFirst = true;             // put a station ID first when filling a top-of-hour gap
    QStringList emergencyTypes = {"station_id", "bumper", "filler", "promo", "psa"};
    QStringList emergencyPlaylist;   // explicit media ids; overrides the typed pool when set
    QStringList fillExtraTypes;      // more media types gap fill may use inside an hour (e.g. "news", "weather": generated spots); none by default

    static FillSettings fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
};

struct FillContext {
    qint64 nowMs = 0;
    QString channelId;
    QString currentSeries;           // series of the surrounding programme, for bumper matching
    QStringList recentlyPlayedIds;   // avoid immediate repeats
    quint32 seed = 0;                // 0 = random
};

class FillPolicy {
public:
    /// Score a candidate (higher = more likely). Returns <= 0 for ineligible.
    static double weight(const MediaRecord& m, const FillContext& ctx, const FillSettings& s);

    /// Choose items from `pool` to cover `gapSec` as closely as possible without
    /// exceeding it (last pick may be shorter). Returns chosen records in order.
    static QList<MediaRecord> fillGap(double gapSec, const QList<MediaRecord>& pool,
                                      const FillContext& ctx, const FillSettings& s, bool atTopOfHour);

    /// Build a loop covering `totalSec` from the emergency pool (typed order, repeating).
    static QList<MediaRecord> emergencyLoop(double totalSec, const QList<MediaRecord>& pool,
                                            const FillSettings& s, int maxItems = 500);

    static QStringList fillTypes(const FillSettings& s);
};

} // namespace sched

#endif // PLAYPLAY_FILLPOLICY_H
