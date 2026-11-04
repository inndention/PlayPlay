#ifndef PLAYPLAY_SCHEDULESTORE_H
#define PLAYPLAY_SCHEDULESTORE_H

#include <QObject>
#include <QSqlDatabase>
#include <QMutex>
#include <QHash>
#include "schedulemodel.h"

namespace sched {

/**
 * ScheduleStore — the single SQLite database behind the Supervisor.
 *
 * Tables: channels, hour_slots, media, as_run, settings. Every HourSlot field
 * is stored as JSON in one column (plus indexed key columns), so adding fields
 * never needs a migration and nothing is silently dropped.
 *
 * Thread-safety: all calls take an internal mutex; one connection per store.
 *
 * Channel cache (perf-supervisor PS-6 / W12): channels() and channel() answer from an in-memory copy of the channels
 * table instead of one SQLite query per call (every UI snapshot, /status and /metrics walk every channel). The copy is
 * loaded on first use and dropped by every channel write; every write to that table goes through saveChannel /
 * deleteChannel (nothing else in the tree writes it, and the store owns its connection).
 */
class ScheduleStore : public QObject {
    Q_OBJECT
public:
    explicit ScheduleStore(QObject* parent = nullptr);
    ~ScheduleStore() override;

    bool open(const QString& dbPath);
    void close();
    bool isOpen() const { return m_open; }
    QString path() const { return m_path; }
    /// Monotonic counter bumped on every schedule/channel write and on every setSetting (unchanged meaning: the executor
    /// rebuilds on it). The split counters below tell the two apart (PS-6; the executor moves to them in PS-7).
    quint64 revision() const { return m_revision; }
    /// Bumped by channel and hour-slot writes only (saveChannel, deleteChannel, saveSlot, deleteSlot), never by setSetting.
    quint64 scheduleRevision() const;
    /// Bumped by every successful setSetting / setSettingJson.
    quint64 settingsRevision() const;
    /// The settingsRevision() at which a key starting with one of `keyPrefixes` was last written since open (0: none
    /// was), so a consumer can follow only the settings it reads (e.g. "programming.recipes", "traffic.").
    quint64 settingsRevision(const QStringList& keyPrefixes) const;

    // --- channels ---------------------------------------------------------------
    QList<ChannelProfile> channels() const;                                         // cached, ordered by id
    ChannelProfile channel(const QString& id, bool* found = nullptr) const;         // cached
    bool saveChannel(const ChannelProfile& c);
    bool deleteChannel(const QString& id);

    // --- hour slots -------------------------------------------------------------
    HourSlot slot(const QString& channelId, const QString& slotId, bool* found = nullptr) const;
    bool saveSlot(const HourSlot& s);
    bool deleteSlot(const QString& channelId, const QString& slotId);
    /// The slot in force for a local date/hour: dated override first, else template.
    HourSlot effectiveSlot(const QString& channelId, const QDate& localDate, int hour, bool* isOverride = nullptr) const;
    /// 7x24 week view starting at weekStart (a Sunday), overrides applied; returns 168 slots.
    QList<HourSlot> week(const QString& channelId, const QDate& weekStart) const;
    QList<HourSlot> templateSlots(const QString& channelId) const;
    int clearHour(const QString& channelId, const QString& slotId) { return deleteSlot(channelId, slotId) ? 1 : 0; }

    // --- media ------------------------------------------------------------------
    bool saveMedia(const MediaRecord& m);
    bool deleteMedia(const QString& id);
    MediaRecord media(const QString& id, bool* found = nullptr) const;
    MediaRecord mediaByPath(const QString& path, bool* found = nullptr) const;
    QList<MediaRecord> allMedia(bool includeArchived = false) const;
    QList<MediaRecord> mediaByType(const QStringList& types, bool includeArchived = false) const;
    QList<MediaRecord> mediaBySeries(const QString& series) const;   // newest episode first
    QList<MediaRecord> searchMedia(const QString& text, int limit = 200) const;
    bool recordPlay(const QString& mediaId, qint64 atMs);

    // --- as-run -----------------------------------------------------------------
    qint64 insertAsRun(const AsRunRow& r);
    /// Update the END fields of the START row for runlistItemId (returns true if found).
    bool completeAsRun(const QString& channelId, const QString& runlistItemId, qint64 actualEndMs, const QString& eventType);
    QList<AsRunRow> asRun(const QString& channelId, qint64 fromMs, qint64 toMs, int limit = 5000) const;
    QJsonObject asRunSummary(const QString& channelId, qint64 fromMs, qint64 toMs) const;
    /// The /asrun summary of a row set (what asRunSummary computes); also used by StoreReader off the main thread.
    static QJsonObject summarizeAsRun(const QList<AsRunRow>& rows);
    /// Row from a `SELECT * FROM as_run` cursor (public so a read-only reader connection can share it).
    static AsRunRow asRunFromQuery(const class QSqlQuery& q);
    /// START rows for a media id in [fromMs, toMs] (all channels when channelId is empty): ad-contract pacing, reports.
    int airedCount(const QString& mediaId, qint64 fromMs, qint64 toMs, const QString& channelId = QString()) const;

    // --- settings ---------------------------------------------------------------
    QString setting(const QString& key, const QString& def = QString()) const;
    bool setSetting(const QString& key, const QString& value);
    QJsonObject settingJson(const QString& key) const;
    bool setSettingJson(const QString& key, const QJsonObject& v);

signals:
    void channelChanged(const QString& channelId);
    void slotChanged(const QString& channelId, const QString& slotId);
    void mediaChanged(const QString& mediaId);
    void asRunAdded(const AsRunRow& row);

private:
    mutable QMutex m_mutex;
    QSqlDatabase m_db;
    QString m_connName;
    QString m_path;
    bool m_open = false;
    quint64 m_revision = 1;
    quint64 m_scheduleRevision = 1;
    quint64 m_settingsRevision = 1;
    QHash<QString, quint64> m_settingKeyRevision;          // key -> m_settingsRevision when it was last written

    // Channel cache (guarded by m_mutex): rows as channels() returns them, plus row id -> index for channel().
    mutable QList<ChannelProfile> m_chanCache;
    mutable QHash<QString, int> m_chanIndex;
    mutable bool m_chanCacheValid = false;
    bool loadChannelsLocked() const;                        // false: store closed or the query failed (nothing cached)
    void dropChannelCacheLocked() { m_chanCacheValid = false; m_chanCache.clear(); m_chanIndex.clear(); }

    bool ensureTables();
    static MediaRecord mediaFromQuery(const class QSqlQuery& q);
};

} // namespace sched

#endif // PLAYPLAY_SCHEDULESTORE_H
