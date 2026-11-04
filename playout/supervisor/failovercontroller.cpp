#include "failovercontroller.h"
#include "channelmanager.h"
#include <QDebug>

FailoverController::FailoverController(ChannelManager* channelMgr, QObject* parent)
    : QObject(parent)
    , m_channelMgr(channelMgr)
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &FailoverController::onHealthCheck);

    if (m_channelMgr) {
        connect(m_channelMgr, &ChannelManager::channelDied,
                this, &FailoverController::onChannelDied);
        connect(m_channelMgr, &ChannelManager::heartbeatTimeout,
                this, &FailoverController::onHeartbeatTimeout);
    }
}

FailoverController::~FailoverController() {
    stop();
}

void FailoverController::start() {
    m_timer->start(500); // 500ms global health check
    qDebug() << "[FailoverController] Started";
}

void FailoverController::stop() {
    m_timer->stop();
}

FailoverController::HealthScore FailoverController::globalHealth() const {
    HealthScore score;
    score.activeChannels = 0;
    score.failedChannels = 0;
    score.score = 100;
    score.status = "healthy";
    score.lastIncidentMs = 0;

    if (!m_channelMgr) return score;

    for (const auto& id : m_channelMgr->channelIds()) {
        if (m_channelMgr->isChannelRunning(id)) {
            score.activeChannels++;
        } else {
            score.failedChannels++;
        }
    }

    int total = score.activeChannels + score.failedChannels;
    if (total > 0) {
        score.score = (score.activeChannels * 100) / total;
    }

    if (score.score >= 90) score.status = "healthy";
    else if (score.score >= 50) score.status = "degraded";
    else score.status = "critical";

    return score;
}

void FailoverController::onHealthCheck() {
    if (!m_channelMgr) return;

    qint64 now = QDateTime::currentDateTime().toMSecsSinceEpoch();

    for (const auto& id : m_channelMgr->channelIds()) {
        if (!m_channelMgr->isChannelRunning(id)) {
            ChannelStats& stats = m_stats[id];
            stats.consecutiveFailures++;

            if (stats.firstFailureMs == 0) {
                stats.firstFailureMs = now;
            }

            // Attempt respawn with backoff
            int backoff = calculateBackoffMs(stats.respawnAttempts);
            qint64 elapsedSinceRespawn = now - stats.lastRespawnMs;

            if (elapsedSinceRespawn > backoff) {
                stats.respawnAttempts++;
                stats.lastRespawnMs = now;
                qWarning() << "[FailoverController] Respawning" << id
                           << "(attempt" << stats.respawnAttempts << ", backoff" << backoff << "ms)";
                m_channelMgr->restartChannel(id);
                emit alert("warning", QString("Respawning channel %1 (attempt %2)")
                    .arg(id).arg(stats.respawnAttempts));
            }

            // Escalation: too many consecutive failures
            if (stats.consecutiveFailures >= 10) {
                emit alert("critical", QString("Channel %1 has failed %2 consecutive checks")
                    .arg(id).arg(stats.consecutiveFailures));
                emit channelMarkedDead(id, "consecutive_failures");
            }
        } else {
            // Channel is healthy — reset stats
            if (m_stats.contains(id)) {
                m_stats[id].consecutiveFailures = 0;
                m_stats[id].respawnAttempts = 0;
            }
        }
    }

    updateGlobalHealth();
}

void FailoverController::onChannelDied(const QString& channelId, const QString& reason) {
    ChannelStats& stats = m_stats[channelId];
    stats.consecutiveFailures++;
    stats.firstFailureMs = QDateTime::currentDateTime().toMSecsSinceEpoch();

    emit alert("error", QString("Channel %1 died: %2").arg(channelId, reason));
    updateGlobalHealth();
}

void FailoverController::onHeartbeatTimeout(const QString& channelId, int missedCount) {
    if (missedCount >= 3) {
        emit alert("critical", QString("Channel %1 heartbeat timeout (%2 missed)").arg(channelId).arg(missedCount));
    }
    (void)channelId;
}

int FailoverController::calculateBackoffMs(int respawnAttempts) const {
    // Exponential backoff: 2s, 4s, 8s, 16s, 32s, then cap at 60s
    int base = 2000;
    int max = 60000;
    int backoff = base * (1 << qMin(respawnAttempts, 5));
    return qMin(backoff, max);
}

void FailoverController::updateGlobalHealth() {
    HealthScore score = globalHealth();
    emit healthChanged(score);

    if (score.status == "critical") {
        emit alert("critical", QString("System health CRITICAL: %1/%2 channels active")
            .arg(score.activeChannels).arg(score.activeChannels + score.failedChannels));
    } else if (score.status == "degraded") {
        emit alert("warning", QString("System health DEGRADED: %1/%2 channels active")
            .arg(score.activeChannels).arg(score.activeChannels + score.failedChannels));
    }
}
