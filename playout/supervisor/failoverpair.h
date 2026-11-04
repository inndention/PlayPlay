#ifndef FAILOVERPAIR_H
#define FAILOVERPAIR_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QHash>
#include <QTimer>
#include <QNetworkAccessManager>
#include <functional>

namespace sched { class ScheduleStore; class ScheduleExecutor; }
class SessionsManager;
class QNetworkReply;

/**
 * FailoverPair — primary/backup pairing between two Supervisors.
 *
 * Roles (settings key `failover.config`, JSON):
 *   {role: standalone|primary|backup, peerUrl, peerToken, intervalSec: 5, graceSec: 30,
 *    auto: true, replicate: true, replicateIntervalSec: 60, pullMedia: false}
 * Runtime: epoch (unix ms of the last promotion, persisted as `failover.epoch`), last good
 * heartbeat, the peer's last heartbeat body, a 50-event history ring.
 *
 * Heartbeat: every intervalSec GET <peerUrl>/api/v1/failover/heartbeat with the peer's bearer
 * token (4 s timeout). A backup that hears nothing for graceSec promotes itself when `auto`
 * is on (role primary, epoch = now, every channel armed) — otherwise it alarms. Two primaries
 * resolve toward the NEWER epoch: the older one demotes (ties break on the higher nodeId).
 * A 401/403 from the peer counts as alive (it answered) and raises an auth alert instead.
 *
 * Role changes: promote() (and saving role=primary on a backup, which is routed through it) bumps the
 * epoch, arms every channel and alerts. demote() (and saving role=backup) disarms every channel AND emits
 * takeOffAir so the Supervisor stops playout on them (slate): disarming alone only stops the executor pushing.
 *
 * Replication (backup only, `replicate`): every replicateIntervalSec GET
 * <peerUrl>/api/v1/sessions/export-live?groups=…&since=<marks>&secrets=1 and apply the bundle through
 * SessionsManager::applyBundle with keepOutputs + keepArmed + mirror, so the backup's hardware outputs
 * and disarmed state survive and every media record keeps the primary's id. After channels were applied
 * channelsApplied() lets the Supervisor reload the executors and spawn engines for channels new to this node.
 * Missing media files are listed, and with `pullMedia` downloaded
 * from <peerUrl>/api/v1/download/<id> into <dataDir>/media/replica/ (3 in flight) and relinked.
 *
 * fetchJson() is a small shared HTTP/JSON helper the remote-node routes use as well.
 */
class FailoverPair : public QObject {
    Q_OBJECT
public:
    FailoverPair(sched::ScheduleStore* store, sched::ScheduleExecutor* exec, SessionsManager* sessions, QObject* parent = nullptr);

    void setDataDir(const QString& dir) { m_dataDir = dir; }
    /// Starts the heartbeat timer; a node configured as backup is disarmed here so it never drives playout before deciding to.
    void start();

    // --- configuration + state -------------------------------------------------------
    QJsonObject config() const;                       // merged with defaults
    QJsonObject setConfig(const QJsonObject& patch);  // partial update; role changes take effect immediately
    QString role() const;                             // standalone | primary | backup
    bool standby() const { return role() == "backup"; }
    qint64 epoch() const;
    QString nodeId() const;
    QJsonObject status() const;                       // for GET /failover/status and the WS event
    QJsonObject heartbeatBody() const;                // what peers get from GET /failover/heartbeat
    QJsonArray history() const;
    QJsonObject wsPayload() const;                    // compact {role, peer, lastHeartbeat, sessions}

    // --- role changes ----------------------------------------------------------------
    QJsonObject promote(const QString& reason);
    QJsonObject demote(const QString& reason);

    // --- shared HTTP/JSON helper (remote nodes, push/pull) -----------------------------
    using JsonCb = std::function<void(int http, const QJsonObject& json, const QString& error)>;
    /// method GET|POST|PUT|DELETE; `body` ignored for GET/DELETE; `token` sent as a bearer. Calls cb once. http==0 → transport error.
    void fetchJson(const QString& method, const QString& url, const QString& token, const QJsonObject& body, int timeoutMs, JsonCb cb);
    QNetworkAccessManager* nam() { return &m_nam; }

signals:
    void alert(const QString& level, const QString& message);
    void roleChanged(const QString& role);
    void statusChanged();                             // heartbeat state transitions, replication, config
    /// This node is going (back) to standby: the Supervisor should stop playout on every channel so a demoted node
    /// does not keep airing the hour it already pushed next to the new primary. Emitted by demote() and by role=backup.
    void takeOffAir(const QString& reason);
    /// A replication cycle applied the `channels` group (apply report attached: createdChannels, restartChannels).
    /// The Supervisor reloads the executors and spawns / restarts engines (the channel set can change after boot).
    void channelsApplied(const QJsonObject& report);

private:
    sched::ScheduleStore* m_store;
    sched::ScheduleExecutor* m_exec;
    SessionsManager* m_sessions;
    QString m_dataDir;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    bool m_started = false;

    // heartbeat state
    QJsonObject m_peer;                               // last heartbeat body from the peer
    qint64 m_peerLastOkMs = 0;
    qint64 m_lastAttemptMs = 0;
    QString m_peerError;
    int m_missed = 0;
    bool m_inFlight = false;
    bool m_announcedDown = false, m_announcedAuth = false, m_announcedBothBackup = false;

    // replication state
    qint64 m_lastReplTryMs = 0, m_lastReplOkMs = 0;
    QJsonObject m_peerMarks;                          // marks returned by the peer's last export (sent back as `since`)
    QJsonArray m_lastReplGroups;
    QJsonObject m_lastReplReport;
    bool m_replInFlight = false;
    QList<QJsonObject> m_unresolved;                  // media the backup lacks: {id, title, hash, path, size}
    QSet<QString> m_pulling;                          // media ids being downloaded
    QHash<QString, qint64> m_pullFailedMs;            // id -> when it failed (retry after 10 min)
    int m_pulled = 0;

    QList<QJsonObject> m_history;                     // ring of 50

    void restartTimer();
    void cycle();
    void onHeartbeat(int http, const QJsonObject& peer, const QString& error);
    void resolveRoles(const QJsonObject& peer);
    void replicate();
    void pullMissingMedia();
    void pullOne(const QJsonObject& m);
    void setArmedAll(bool armed);
    void note(const QString& event, const QString& detail, const QString& level = "info");
    QString peerUrl() const;
    QString peerToken() const;
    static QString normUrl(const QString& u);
};

#endif // FAILOVERPAIR_H
