#ifndef PLAYPLAY_SCHEDULEMODEL_H
#define PLAYPLAY_SCHEDULEMODEL_H

#include <QString>
#include <QStringList>
#include <QDate>
#include <QDateTime>
#include <QTimeZone>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

/**
 * Schedule domain model (Supervisor side).
 *
 * This is the union of the two reference apps' schedule shapes, with every
 * field persisted (PlayPlayClean silently dropped most of them). JSON field
 * names follow the PlayPlayClean TypeScript types where they exist so weekly
 * schedules exported from the Electron app import directly.
 *
 * Slot ids:  "D-H"           weekly template (D = 0 Sunday .. 6 Saturday)
 *            "YYYY-MM-DD-H"  dated override (wins over the template)
 */
namespace sched {

struct SlotItem {
    QString id;                 // unique within the slot
    QString mediaId;            // asset id in the media table ("" for live/cg/gap)
    QString title;
    QString path;               // resolved file path or URL (may be filled from media)
    QString type;               // program|ad|promo|station_id|bumper|filler|psa|live|cg|gap|image|video
    double durationSec = 0.0;   // full media duration (informational; runlist uses in/out)
    double inPointSec = 0.0;    // timeIn
    double outPointSec = 0.0;   // timeOut (0 = media end)
    QString timingMode = "soft_start";   // soft_start | hard_start | fill_until
    double fixedOffsetSec = -1.0;        // hard_start at hour + offset (seconds into the hour)
    qint64 fixedUtcMs = 0;               // or an absolute wall-clock hard start
    bool loop = false;
    bool locked = false;
    int priority = 0;           // -10..10
    QString transition = "cut"; // cut|fade|mix|wipe
    double transitionSec = 0.0;
    QJsonObject smartConfig;    // {type: latest|previous|specific_date|random, seriesName, targetDate, clientId}
    QStringList cgOnStart;
    QStringList cgOnEnd;
    QJsonArray cgOverlays;      // [{id, templateId|filePath, startOffset, duration, layer}]
    QString liveSourceId;
    QJsonObject liveSource;
    QString segmentId, segmentLabel, parentMediaId;
    QString note;
    QString status;             // scheduled|playing|completed|skipped|failed (runtime, informational)
    // Late-bound blocks (PlayPlay OBS): resolved to concrete media when the hour is built, not when it is saved.
    QJsonObject queryBlock;     // {types[], targetSec, prefer newest|oldest|most_played|least_played|random, categories[], subcategories[], series[], nthLatest, noRepeatMin}
    QJsonObject folderBlock;    // {folder (path or category/subcategory/tag), targetSec, weight freshness|uniform, order random|name|date|size}
    double lockAtSec = -1.0;    // locked anchor: air at this offset into the hour (soft anchor: nearest item boundary, nothing is cut)
    QString lockLabel;          // operator label for the lock ("GEN 12 Weather")
    QJsonObject extra;          // passthrough for fields we do not model

    /// The block spec, whether it was set on the field or left in `extra` by a producer that predates the field.
    QJsonObject query() const { return !queryBlock.isEmpty() ? queryBlock : extra.value("queryBlock").toObject(extra.value("query_block").toObject()); }
    QJsonObject folder() const { return !folderBlock.isEmpty() ? folderBlock : extra.value("folderBlock").toObject(extra.value("folder_block").toObject()); }
    bool isQuery() const { return !query().isEmpty() || !folder().isEmpty(); }
    /// Soft anchor offset (seconds into the hour) or -1. Locked items with lockAtSec; also honours extra.lockAtSec.
    double anchorSec() const {
        if (!locked) return -1.0;
        if (lockAtSec >= 0.0) return lockAtSec;
        const QJsonValue v = extra.contains("lockAtSec") ? extra.value("lockAtSec") : extra.value("lock_at_sec");
        return v.isDouble() && v.toDouble() >= 0.0 ? qMin(v.toDouble(), 3600.0) : -1.0;
    }
    bool isHardStart() const { return timingMode == "hard_start" && (fixedOffsetSec >= 0.0 || fixedUtcMs > 0); }
    bool isFillUntil() const { return timingMode == "fill_until"; }
    bool isPlayable() const { return type != "cg" && type != "gap"; }
    double effectiveDurationSec() const {
        if (outPointSec > inPointSec) return outPointSec - inPointSec;
        if (durationSec > inPointSec) return durationSec - inPointSec;
        return 0.0;
    }

    QJsonObject toJson() const;
    static SlotItem fromJson(const QJsonObject& o);
};

struct HourSlot {
    QString id;                 // "D-H" or "YYYY-MM-DD-H"
    QString channelId;
    int dayOfWeek = -1;         // 0..6 for template slots, -1 for dated
    QDate date;                 // valid for dated slots
    int hour = 0;
    QList<SlotItem> items;
    bool isLooping = false;
    bool randomizeAds = true;
    bool keepProgramOrder = true;
    QString blockType = "general";   // general|show|news|sports|paid_program|maintenance
    QJsonObject showBlock;           // {id, title, seriesName, locked, autofillTemplateId, notes}
    QJsonObject showHour;            // {enabled, watchFolderPath}
    QJsonObject mergedEvent;         // {id, rootSlotId, slotIds[], title, durationHours, isContinuation}
    QJsonObject loopOverride;        // {enabled, loopUntilHour, startedAtHour, createdAt}
    QJsonArray deviceCommands;       // [{id, commandId, offsetSec, label}]
    qint64 updatedAtMs = 0;

    bool isDated() const { return date.isValid(); }
    bool isContinuation() const { return mergedEvent.value("isContinuation").toBool(false); }
    bool isEmpty() const { return items.isEmpty(); }
    double totalSeconds() const;

    static QString templateId(int dayOfWeek, int hour) { return QString("%1-%2").arg(dayOfWeek).arg(hour); }
    static QString dateId(const QDate& d, int hour) { return QString("%1-%2").arg(d.toString(Qt::ISODate)).arg(hour); }
    static bool parseId(const QString& id, int* dayOfWeek, QDate* date, int* hour);

    QJsonObject toJson() const;
    static HourSlot fromJson(const QJsonObject& o);
};

struct ChannelProfile {
    QString id = "main";
    QString name = "Main";
    QString callLetters;
    QString brandName;
    bool enabled = true;
    QString frameRate = "30000/1001";
    int width = 1920;
    int height = 1080;
    int audioChannels = 2;
    QString timezone = "America/New_York";   // hour boundaries are computed here
    double prerollSec = 3.0;
    QJsonArray outputs;                      // engine output config (see PlayoutEngine::Config)
    QJsonObject identity;                    // {bugAsset, legalIdAsset, emergencySlate, slateCaptions[]}
    QJsonObject fill;                        // {fillGapsWithIds, fillGapsWithBumpers, fillWithFiller, maxFillSec,
                                             //  emergencyPlaylist[], emergencyTypes[], shuffleAdsInHour}
    bool armed = true;                       // schedule execution enabled
    bool strict = false;                     // always snap to wall clock at item end
    QJsonObject extra;

    QTimeZone tz() const { QTimeZone z(timezone.toUtf8()); return z.isValid() ? z : QTimeZone::systemTimeZone(); }
    QJsonObject toJson() const;
    static ChannelProfile fromJson(const QJsonObject& o);
};

struct MediaRecord {
    QString id;
    QString path;
    QString title;
    QString type = "video";     // same vocabulary as SlotItem::type
    double durationSec = 0.0;
    QString series;             // programSeries
    QDate episodeDate;
    QString client;
    int priority = 0;
    int playCount = 0;
    qint64 lastPlayedMs = 0;
    bool archived = false;
    qint64 availableStartMs = 0;
    qint64 availableEndMs = 0;
    QStringList allowedChannels;   // empty = all
    int width = 0, height = 0;
    QString frameRate;
    QString videoCodec, audioCodec;
    qint64 fileSizeBytes = 0;
    QString thumbnailPath;
    QJsonArray segments;           // [{id,label,startTime,endTime}]
    qint64 addedAtMs = 0;
    QJsonObject extra;

    bool isAvailableAt(qint64 utcMs) const {
        if (archived) return false;
        if (availableStartMs > 0 && utcMs < availableStartMs) return false;
        if (availableEndMs > 0 && utcMs > availableEndMs) return false;
        return true;
    }
    bool operator==(const MediaRecord& o) const { return id == o.id; }
    QJsonObject toJson() const;
    static MediaRecord fromJson(const QJsonObject& o);
};

struct AsRunRow {
    qint64 rowId = 0;
    QString channelId;
    QString runlistItemId;      // engine item id (slot:item:index)
    QString itemId;             // slot item id
    QString assetId;
    QString title;
    QString type;
    QString eventType;          // START|END|SKIP|FAILED|TRUNCATED|HOLD|STOP|PAUSE|RESUME|SEEK|REPLACED|FILL
    QString origin;             // schedule|fill|emergency|manual|smart
    qint64 plannedStartMs = 0;
    qint64 actualStartMs = 0;
    qint64 plannedEndMs = 0;
    qint64 actualEndMs = 0;
    qint64 plannedDurationMs = 0;
    qint64 actualDurationMs = 0;
    qint64 varianceMs = 0;      // actual - planned start
    qint64 frame = 0;
    QString timecode;
    QString status;             // ON_TIME|LATE_START|EARLY_START|FAILED|SKIPPED|TRUNCATED|FILL|MANUAL
    QString reason;
    QString slotId;
    QJsonObject extra;

    QJsonObject toJson() const;
};

// Day-of-week helper: Qt uses 1 = Monday .. 7 = Sunday; schedule uses 0 = Sunday.
inline int scheduleDow(const QDate& d) { return d.dayOfWeek() % 7; }

} // namespace sched

#endif // PLAYPLAY_SCHEDULEMODEL_H
