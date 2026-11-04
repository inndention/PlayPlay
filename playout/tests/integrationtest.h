#ifndef INTEGRATIONTEST_H
#define INTEGRATIONTEST_H

#include <QObject>
#include <QTimer>
#include <QJsonObject>

/**
 * IntegrationTest — automated validation suite for playout engine.
 *
 * Tests:
 *   1. Supervisor spawns channel engine successfully
 *   2. Engine connects and receives time sync within 1s
 *   3. Frame-accurate cue execution (±1 frame tolerance)
 *   4. Engine crash → Supervisor respawn within 2s
 *   5. Supervisor crash → engines run independently
 *   6. 24-hour soak test with frame-drop telemetry
 */
class IntegrationTest : public QObject {
    Q_OBJECT

public:
    explicit IntegrationTest(QObject* parent = nullptr);

    void runAll();

    // Individual tests
    void testChannelSpawn();
    void testTimeSync();
    void testFrameAccurateCue();
    void testCrashRespawn();
    void testEngineIndependence();

    // Long-running soak test
    void startSoakTest(int durationHours = 24);
    void stopSoakTest();

signals:
    void testPassed(const QString& testName);
    void testFailed(const QString& testName, const QString& reason);
    void soakTestFrameStats(qint64 totalFrames, int droppedFrames, double avgLatencyMs);
    void allTestsComplete(int passed, int failed);

private slots:
    void onSoakTick();

private:
    QTimer* m_soakTimer;
    qint64 m_soakStartMs;
    int m_soakDurationHours;
    qint64 m_totalFrames;
    int m_droppedFrames;
    double m_avgLatencyMs;
    bool m_soakRunning;

    void logResult(const QString& testName, bool passed, const QString& reason = QString());
};

#endif // INTEGRATIONTEST_H
