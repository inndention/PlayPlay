#ifndef FRAMECLOCK_H
#define FRAMECLOCK_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QJsonObject>
#include <atomic>
#include "framerate.h"

/**
 * FrameClock — high-resolution frame-accurate timing for broadcast playout.
 *
 * Runs on its own thread and emits frameTick() at an exact rational frame
 * rate (29.97 = 30000/1001). Frame N fires at origin + rate.frameToNs(N), so
 * the timeline never drifts from accumulated rounding. Supports correction
 * against an external master clock (TimeAuthority via the Supervisor).
 *
 * frameTick is emitted from the clock thread. Connect with
 * Qt::DirectConnection for frame-accurate work; keep the slot short.
 *
 * Waiting (PE-14): on Windows the coarse wait is a high-resolution waitable timer
 * (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) that wakes ~200 us before the target, then
 * the thread spins (yielding) for the rest. The old wait — usleep to 1.5 ms before the
 * target, then a 1.5 ms spin, ~9 % of a core at 60 fps — is used when that timer is not
 * available or PLAYPLAY_CLOCK_SPIN=1. PLAYPLAY_CLOCK_SPIN_US=n sets the spin margin of
 * the timer mode (default 200, 0..2000). Both are read when the clock thread starts.
 */
class FrameClock : public QThread {
    Q_OBJECT

public:
    explicit FrameClock(QObject* parent = nullptr);
    ~FrameClock() override;

    void setFrameRate(const FrameRate& rate);
    void setFrameRate(int fps) { setFrameRate(FrameRate(fps, 1)); }
    FrameRate frameRate() const { QMutexLocker l(&m_rateMutex); return m_rate; }

    void setMasterOffsetNs(qint64 offsetNs);
    qint64 masterOffsetNs() const { return m_masterOffsetNs.load(); }

    void setRunning(bool run);
    bool isTicking() const { return m_isRunning.load(); }

    qint64 currentFrame() const { return m_currentFrame.load(); }
    qint64 frameDurationNs() const { return frameRate().frameDurationNs(); }

    /// Steady-clock origin (ns) of frame 0. Valid once running.
    qint64 originNs() const { return m_originNs.load(); }

    /// Thread-safe: master says "frame N happened at steady time T".
    void applyFrameCorrection(qint64 targetFrame, qint64 atTimestampNs);
    /// Slow rate trim in parts per million (+ = run faster). Used to lock the clock to an
    /// output device's frame cadence (SDI card) instead of free-running. Clamped to +-500.
    void setRateTrimPpm(double ppm);
    double rateTrimPpm() const { return m_trimPpmX100.load() / 100.0; }

    /// Steady clock now, in ns. Same clock as frameTick timestamps.
    static qint64 nowNs();

    /// Wait method of the running clock thread: "hires" (waitable timer + short spin),
    /// "spin" (old usleep + 1.5 ms spin) or "" before the thread has started.
    QString waitMode() const;
    /// Lock-free clock statistics since the thread started (any thread may call it):
    /// mode, spinUs, ticks, lateness at emit (histogram "lateHistUs" with upper bucket
    /// edges in us, lateMaxUs, lateP99Us = upper edge of the p99 bucket), spinAvgUs (final
    /// spin per tick), timerLeadUs (learned timer overshoot, hires), cpuMs / cpuPct of the
    /// clock thread (cycle-exact, sampled about once a second, Windows only).
    QJsonObject statsJson() const;

signals:
    void frameTick(qint64 frameNumber, qint64 timestampNs);
    void driftDetected(qint64 driftNs, qint64 frameNumber);

protected:
    void run() override;

private:
    mutable QMutex m_rateMutex;
    FrameRate m_rate;
    std::atomic<qint64> m_currentFrame;
    std::atomic<qint64> m_masterOffsetNs;
    std::atomic<qint64> m_originNs;
    std::atomic<bool> m_isRunning;
    std::atomic<bool> m_shouldStop;

    QMutex m_driftMutex;
    std::atomic<int> m_trimPpmX100{0};
    qint64 m_pendingCorrectionFrame;
    qint64 m_pendingCorrectionAtNs;
    bool m_hasPendingCorrection;

    void sleepUntil(qint64 targetNs);
    void sleepUntilSpin(qint64 targetNs);       // pre-PE-14 wait
    void recordTick(qint64 lateNs);
    void sampleThreadCpu(qint64 nowNs);

    // PE-14 wait state; touched only by the clock thread except the atomics.
    void* m_timer = nullptr;                    // HANDLE of the high-resolution waitable timer
    qint64 m_spinNs = 200000;
    qint64 m_overshootNs = 0;                   // learned lateness of the timer's wake (0..1 ms)
    std::atomic<qint64> m_overshootStat{0};
    std::atomic<qint64> m_spinTotalNs{0};       // time spent in the final spin, all ticks
    enum : int { ModeNone = 0, ModeHires = 1, ModeSpin = 2 };
    std::atomic<int> m_mode{ModeNone};

    // Statistics (written by the clock thread, read lock-free by statsJson).
    static constexpr int kLateBuckets = 9;      // <=50,100,200,500,1000,2000,5000,16000 us, more
    static const qint64 kLateEdgesUs[kLateBuckets - 1];
    std::atomic<qint64> m_lateHist[kLateBuckets] = {};
    std::atomic<qint64> m_statTicks{0};
    std::atomic<qint64> m_lateMaxNs{0};
    std::atomic<qint64> m_statStartNs{0};
    std::atomic<qint64> m_cpuNs{0};             // clock thread CPU time since start at the last sample
    std::atomic<qint64> m_cpuAtNs{0};           // steady time of that sample
    qint64 m_cpuBaseCycles = 0;                 // clock-thread cycles / TSC at the first sample
    qint64 m_tscBase = 0;
};

#endif // FRAMECLOCK_H
