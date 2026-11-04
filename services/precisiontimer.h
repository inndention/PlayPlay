#ifndef PRECISIONTIMER_H
#define PRECISIONTIMER_H

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>

struct TimerConfig {
    int targetHz;
    int jitterWarningThreshold;
    int maxAccumulatedDrift;
    bool frameAccurate;
    int watchdogTimeout;
    bool verbose;
};

struct TimerStats {
    qint64 totalTicks;
    qint64 missedTicks;
    double averageJitter;
    double maxJitter;
    double minJitter;
    qint64 lastTickTime;
    qint64 uptime;
    bool isHealthy;
    int healthScore;
};

class PrecisionTimer : public QObject
{
    Q_OBJECT
    
public:
    explicit PrecisionTimer(const TimerConfig& config = TimerConfig(), QObject* parent = nullptr);
    ~PrecisionTimer();
    
    void start();
    void stop();
    bool isRunning() const { return m_isRunning; }
    
    TimerStats getStats() const { return m_stats; }
    TimerConfig getConfig() const { return m_config; }
    
signals:
    void tick(double delta, qint64 now, const TimerStats& stats);
    
private slots:
    void onTick();
    
private:
    void updateStats(double jitter);
    void checkWatchdog();
    
    QTimer* m_timer;
    QElapsedTimer m_elapsedTimer;
    TimerConfig m_config;
    TimerStats m_stats;
    bool m_isRunning;
    qint64 m_startTime;
    qint64 m_lastTickTime;
    double m_accumulator;
    double m_targetInterval;
    QTimer* m_watchdogTimer;
};

#endif // PRECISIONTIMER_H

