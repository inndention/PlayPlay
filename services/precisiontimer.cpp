#include "precisiontimer.h"
#include <QDebug>
#include <QDateTime>

PrecisionTimer::PrecisionTimer(const TimerConfig& config, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_isRunning(false)
    , m_startTime(0)
    , m_lastTickTime(0)
    , m_accumulator(0.0)
    , m_watchdogTimer(new QTimer(this))
{
    if (m_config.targetHz <= 0) {
        m_config.targetHz = 20;
    }
    if (m_config.jitterWarningThreshold <= 0) {
        m_config.jitterWarningThreshold = 20;
    }
    if (m_config.maxAccumulatedDrift <= 0) {
        m_config.maxAccumulatedDrift = 100;
    }
    if (m_config.watchdogTimeout <= 0) {
        m_config.watchdogTimeout = 1000;
    }
    
    m_targetInterval = 1000.0 / m_config.targetHz;
    
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(static_cast<int>(m_targetInterval));
    connect(m_timer, &QTimer::timeout, this, &PrecisionTimer::onTick);
    
    m_watchdogTimer->setSingleShot(true);
    m_watchdogTimer->setInterval(m_config.watchdogTimeout);
    connect(m_watchdogTimer, &QTimer::timeout, this, &PrecisionTimer::checkWatchdog);
    
    m_stats = TimerStats();
    m_stats.isHealthy = true;
    m_stats.healthScore = 100;
}

PrecisionTimer::~PrecisionTimer()
{
    stop();
}

void PrecisionTimer::start()
{
    if (m_isRunning) {
        return;
    }
    
    m_elapsedTimer.start();
    m_startTime = QDateTime::currentMSecsSinceEpoch();
    m_lastTickTime = m_startTime;
    m_accumulator = 0.0;
    
    m_stats = TimerStats();
    m_stats.isHealthy = true;
    m_stats.healthScore = 100;
    
    m_timer->start();
    m_watchdogTimer->start();
    m_isRunning = true;
}

void PrecisionTimer::stop()
{
    if (!m_isRunning) {
        return;
    }
    
    m_timer->stop();
    m_watchdogTimer->stop();
    m_isRunning = false;
}

void PrecisionTimer::onTick()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 elapsed = now - m_lastTickTime;
    
    double delta = elapsed / 1000.0; // Convert to seconds
    double jitter = qAbs(elapsed - m_targetInterval);
    
    // Update accumulator
    m_accumulator += delta;
    
    // Check for accumulated drift
    if (m_accumulator > m_config.maxAccumulatedDrift / 1000.0) {
        if (m_config.verbose) {
            qDebug() << "[PrecisionTimer] Resetting accumulator due to drift:" << m_accumulator;
        }
        m_accumulator = 0.0;
    }
    
    // Update stats
    updateStats(jitter);
    
    // Reset watchdog
    m_watchdogTimer->start();
    
    m_lastTickTime = now;
    
    emit tick(delta, now, m_stats);
}

void PrecisionTimer::updateStats(double jitter)
{
    m_stats.totalTicks++;
    m_stats.lastTickTime = m_lastTickTime;
    m_stats.uptime = QDateTime::currentMSecsSinceEpoch() - m_startTime;
    
    // Update jitter stats
    if (m_stats.totalTicks == 1) {
        m_stats.averageJitter = jitter;
        m_stats.maxJitter = jitter;
        m_stats.minJitter = jitter;
    } else {
        m_stats.averageJitter = (m_stats.averageJitter * (m_stats.totalTicks - 1) + jitter) / m_stats.totalTicks;
        if (jitter > m_stats.maxJitter) {
            m_stats.maxJitter = jitter;
        }
        if (jitter < m_stats.minJitter) {
            m_stats.minJitter = jitter;
        }
    }
    
    // Check for jitter warnings
    if (jitter > m_config.jitterWarningThreshold && m_config.verbose) {
        qWarning() << "[PrecisionTimer] High jitter detected:" << jitter << "ms";
    }
    
    // Calculate health score (0-100)
    double jitterRatio = jitter / m_targetInterval;
    m_stats.healthScore = qBound(0, static_cast<int>(100 - (jitterRatio * 50)), 100);
    m_stats.isHealthy = m_stats.healthScore > 50;
}

void PrecisionTimer::checkWatchdog()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 timeSinceLastTick = now - m_lastTickTime;
    
    if (timeSinceLastTick > m_config.watchdogTimeout) {
        qWarning() << "[PrecisionTimer] Watchdog timeout - timer may be stalled";
        m_stats.missedTicks++;
        m_stats.isHealthy = false;
        m_stats.healthScore = qMax(0, m_stats.healthScore - 10);
    }
}

