#ifndef PLAYPLAY_SCHEDULEEXECUTOR_H
#define PLAYPLAY_SCHEDULEEXECUTOR_H

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include "schedulemodel.h"
#include "schedulestore.h"
#include "runlistbuilder.h"

namespace sched {

/**
 * EngineLink — how the executor talks to a channel engine. The Supervisor
 * implements it over IPC; tests implement it in memory.
 */
class EngineLink {
public:
    virtual ~EngineLink() = default;
    virtual bool pushPlaylist(const QString& channelId, const QJsonArray& items, bool replaceCurrent) = 0;
    virtual bool transport(const QString& channelId, const QString& action, const QJsonObject& params) = 0;
    virtual bool sendConfig(const QString& channelId, const QJsonObject& config) = 0;
};

/**
 * ScheduleExecutor — engine-owned timing, Supervisor-owned decisions.
 *
 * Once per second, for every armed channel with a connected engine:
 *   1. Work out the local hour (channel time zone), build the runlist for this
 *      hour and the next from the store (dated override > template).
 *   2. If the hour changed, the schedule changed, the engine just connected, or
 *      the engine is idle while something is due, push the runlist:
 *        - items already finished are dropped,
 *        - the item that should be on air now gets its in-point advanced
 *          (join in progress) unless the engine is already playing it,
 *        - the first item of the next hour carries a hard start at :00 so the
 *          engine cuts exactly on time, except for merged-event continuations,
 *        - an empty schedule becomes the emergency loop.
 *   3. Map engine as-run events back to planned times and store START/END rows
 *      with variance.
 *
 * All timing accuracy lives in the engine (frame clock + hard starts); the
 * executor never needs to be on time itself.
 */
class ScheduleExecutor : public QObject {
    Q_OBJECT
public:
    struct ChannelStatus {
        QString channelId;
        bool armed = false;
        bool connected = false;
        bool emergency = false;
        QString currentSlotId;
        qint64 hourStartMs = 0;
        qint64 lastPushMs = 0;
        QString lastPushReason;
        int pushedItems = 0;
        QString engineState;
        QString onAirRunlistId;
        QString onAirTitle;
        double driftSec = 0.0;
        int rebuilds = 0;                 // two-hour runlist builds since the channel was added (PS-7: diagnostics)
        QJsonObject toJson() const;
    };

    ScheduleExecutor(ScheduleStore* store, EngineLink* link, QObject* parent = nullptr);

    void start(int intervalMs = 1000);
    void stop();

    void addChannel(const QString& channelId);
    void removeChannel(const QString& channelId);
    QStringList channelIds() const { return m_channels.keys(); }
    bool hasChannel(const QString& id) const { return m_channels.contains(id); }

    /// Engine lifecycle (from the Supervisor).
    void onEngineConnected(const QString& channelId);
    void onEngineDisconnected(const QString& channelId);
    void onTelemetry(const QString& channelId, const QJsonObject& state);
    void onEngineAsRun(const QString& channelId, const QJsonObject& event);

    /// Operator actions.
    void setArmed(const QString& channelId, bool armed);
    void loadHourNow(const QString& channelId);              // force a fresh push with join-in-progress
    void returnToSchedule(const QString& channelId);         // leaving an operator take: queue the schedule from now behind it
    void snapToSchedule(const QString& channelId);           // seek the engine to the wall-clock position
    void reloadChannel(const QString& channelId);            // re-read profile (outputs/timezone) and resend config
    /// Operator "re-roll": blocks that have not started airing get different picks (the previous ones rank last, the seeds
    /// move on) and the next hour's synthesized layout is dropped; a block that is on air or already aired keeps its picks.
    /// `fresh = false` only re-resolves them (same seeds, nothing avoided): for when the inputs changed, e.g. ad contracts.
    void rerollBlocks(const QString& channelId, bool fresh = true);
    /// Build the runlist for any slot without side effects (previews of recipes / blocks): dated or template slot id.
    /// `sharedPicks`: one cache across several previewed hours, so rotation ("even", least played) spreads across them
    /// the way it will on air. nullptr = a scratch cache for this hour only.
    Runlist previewSlot(const QString& channelId, const HourSlot& slot, qint64 hourStartMs, QHash<QString, QStringList>* sharedPicks = nullptr) const;

    /// The hours the executor has built for the channel (current, next), for the flight-plan views; empty before the first build.
    QList<Runlist> builtRunlists(const QString& channelId) const;
    /// A copy of the channel's query-block pick cache: previews that pass it resolve every block exactly as it is airing.
    QHash<QString, QStringList> pickCacheCopy(const QString& channelId) const;
    qint64 clockNowMs() const { return nowMs(); }

    ChannelStatus status(const QString& channelId) const;
    QJsonObject runlistJson(const QString& channelId) const; // current + next hour as pushed
    const RunlistItem* lookupRunlistItem(const QString& channelId, const QString& runlistId) const;

    /// Build what would be pushed right now (no side effects): for previews and tests.
    QJsonArray plannedPlaylist(const QString& channelId, qint64 nowMs, bool* replaceCurrent = nullptr) const;

    void setNowProvider(std::function<qint64()> f) { m_now = std::move(f); }
    void tick();

signals:
    void runlistChanged(const QString& channelId, const QJsonObject& runlist);
    void asRunStored(const sched::AsRunRow& row);
    void alert(const QString& channelId, const QString& level, const QString& message);
    void pushed(const QString& channelId, int items, bool replaceCurrent, const QString& reason);

private:
    /// A weighted-autofill hour once synthesized: concrete media, so it is frozen per hour (the rotation sorts on play counts,
    /// which move as items air). `sig` = policy + buckets + incoming carry; a mismatch re-synthesizes.
    struct SynthHour {
        QString sig;
        QList<SlotItem> items;
        QStringList notes;
        QJsonObject carry;                           // continuation of a long program for the following hour
    };
    struct ChannelRun {
        ChannelProfile profile;
        bool connected = false;
        bool emergency = false;
        qint64 hourStartMs = 0;
        Runlist current, next;
        QString pushedHash;
        // The planned-list hash, computed once per rebuild (PS-7). evaluate() compares it with pushedHash every second; it
        // used to serialize and SHA-1 both hours per channel per tick, and push()'s emergency "visible" list, which replaces
        // ch.current, changed it (one extra push per rebuild while the loop started: the startup emergency/join flap).
        QString plannedHash;
        quint64 storeRevision = 0;                   // ScheduleStore::scheduleRevision() at the last rebuild (0 forces one)
        quint64 settingsRevision = 0;                // settingsRevision(kExecutorSettingKeys) at the last rebuild
        int rebuilds = 0;
        QHash<QString, RunlistItem> byRunlistId;     // everything pushed recently (for as-run)
        QJsonObject state;                           // last telemetry
        qint64 stateMs = 0;
        qint64 lastPushMs = 0;
        QString lastPushReason;
        int pushedItems = 0;
        bool forcePush = false;
        bool configSent = false;
        double driftSec = 0.0;
        QHash<QString, QStringList> picks;           // query-block pick cache: "<hourStartMs>#<itemId>" -> media ids
        QHash<QString, double> pacing;               // ad-contract pacing bonus per media id (refreshed per rebuild)
        QHash<QString, QStringList> avoid;           // re-roll: previous picks per block key, ranked last when the block re-resolves
        quint32 rerollEpoch = 0;                     // bumped by every re-roll; folded into the block seeds
        QHash<qint64, SynthHour> synth;              // weighted autofill layouts per hourStartMs
        QHash<qint64, QSet<QString>> synthAvoid;     // re-roll: media of the dropped layout per hourStartMs, queued last when the hour is laid again
        QHash<qint64, double> carryByHour;           // continuation hours: how far the previous hour's content runs into them (s)
        QString lastTakeId;                          // operator take on air ("take-..."), seen in telemetry
        qint64 takeEndsAtMs = 0;                     // when it ends (0 = open-ended, e.g. a live source)
    };

    ScheduleStore* m_store;
    EngineLink* m_link;
    QTimer m_timer;
    QHash<QString, ChannelRun> m_channels;
    std::function<qint64()> m_now;

    qint64 nowMs() const;
    MediaLookup mediaLookup() const;
    void rebuild(ChannelRun& ch, qint64 now, qint64 hourStart);
    HourSlot effectiveOrSynthesized(ChannelRun& ch, const QDate& d, int hour, qint64 hourStart, QStringList* notes, const MediaLookup& media) const;
    void refreshPacing(ChannelRun& ch, qint64 now) const;
    QJsonArray composePlaylist(const ChannelRun& ch, qint64 now, bool* replaceCurrent, QString* reason) const;
    void push(ChannelRun& ch, qint64 now, const QString& reason);
    void evaluate(ChannelRun& ch, qint64 now);
    static QString hashOf(const QJsonArray& a);
    /// The hash of the schedule the two built hours plan (the emergency loop's visible items are not schedule); a
    /// fixed-source channel hashes its source spec instead, since its one endless item is the same every hour.
    static QString plannedHashOf(const ChannelRun& ch);
    /// Rebuild when the schedule (channel + slot writes) or one of the settings the build reads changed; setSetting of
    /// anything else (every module that persists state) no longer rebuilds all channels (PS-7).
    bool storeChangedSince(const ChannelRun& ch) const;
    AsRunRow rowFromEvent(const ChannelRun& ch, const QJsonObject& ev) const;
    QJsonObject engineConfigFor(const ChannelProfile& p) const;
};

} // namespace sched

Q_DECLARE_METATYPE(sched::AsRunRow)

#endif // PLAYPLAY_SCHEDULEEXECUTOR_H
