#ifndef FAILOVERCONTROLLER_H
#define FAILOVERCONTROLLER_H

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QDateTime>

class ChannelManager;

/**
 * FailoverController — advanced failover and health monitoring.
 *
 * Extends basic heartbeat with:
 *   - Consecutive failure tracking
 *   - Exponential backoff for respawn retries
 *   - Global system health scoring
 *   - Alert escalation
 */
class FailoverController : public QObject {
    Q_OBJECT

public:
    struct HealthScore {
        int score;           // 0-100
        QString status;      // healthy, degraded, critical
        int activeChannels;
        int failedChannels;
        qint64 lastIncidentMs;
        QString lastIncidentReason;
    };

    explicit FailoverController(ChannelManager* channelMgr, QObject* parent = nullptr);
    ~FailoverController();

    void start();
    void stop();

    HealthScore globalHealth() const;

signals:
    void healthChanged(const HealthScore& score);
    void alert(const QString& level, const QString& message);
    void channelMarkedDead(const QString& channelId, const QString& reason);

private slots:
    void onHealthCheck();
    void onChannelDied(const QString& channelId, const QString& reason);
    void onHeartbeatTimeout(const QString& channelId, int missedCount);

private:
    ChannelManager* m_channelMgr;
    QTimer* m_timer;

    struct ChannelStats {
        int consecutiveFailures;
        int respawnAttempts;
        qint64 firstFailureMs;
        qint64 lastRespawnMs;
    };
    QHash<QString, ChannelStats> m_stats;

    int calculateBackoffMs(int respawnAttempts) const;
    void updateGlobalHealth();
};

#endif // FAILOVERCONTROLLER_H
