#include "frameclock.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>

#ifdef Q_OS_WIN
#include <windows.h>
#include <timeapi.h>   // timeBeginPeriod (excluded by WIN32_LEAN_AND_MEAN)
#include <intrin.h>    // __rdtsc
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803+; older SDK headers lack it
#endif
#endif

const qint64 FrameClock::kLateEdgesUs[FrameClock::kLateBuckets - 1] = {50, 100, 200, 500, 1000, 2000, 5000, 16000};

qint64 FrameClock::nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

FrameClock::FrameClock(QObject* parent)
    : QThread(parent)
    , m_rate(FrameRate::fps30())
    , m_currentFrame(0)
    , m_masterOffsetNs(0)
    , m_originNs(0)
    , m_isRunning(false)
    , m_shouldStop(false)
    , m_pendingCorrectionFrame(-1)
    , m_pendingCorrectionAtNs(0)
    , m_hasPendingCorrection(false)
{
}

FrameClock::~FrameClock() {
    m_shouldStop.store(true);
    m_isRunning.store(false);
    wait(1000);
}

void FrameClock::setFrameRate(const FrameRate& rate) {
    if (!rate.isValid()) return;
    QMutexLocker l(&m_rateMutex);
    m_rate = rate;
}

void FrameClock::setMasterOffsetNs(qint64 offsetNs) {
    m_masterOffsetNs.store(offsetNs);
}

void FrameClock::setRunning(bool run) {
    if (run && !m_isRunning.load()) {
        m_isRunning.store(true);
        m_shouldStop.store(false);
        // NB: this class used to shadow QThread::isRunning(); we must test the
        // thread state here or the clock thread is never started.
        if (!QThread::isRunning()) start(QThread::TimeCriticalPriority);
    } else if (!run) {
        m_isRunning.store(false);
        m_shouldStop.store(true);
    }
}

void FrameClock::setRateTrimPpm(double ppm) {
    if (ppm > 500.0) ppm = 500.0; if (ppm < -500.0) ppm = -500.0;
    m_trimPpmX100.store(int(ppm * 100.0));
}

void FrameClock::applyFrameCorrection(qint64 targetFrame, qint64 atTimestampNs) {
    QMutexLocker lock(&m_driftMutex);
    m_pendingCorrectionFrame = targetFrame;
    m_pendingCorrectionAtNs = atTimestampNs;
    m_hasPendingCorrection = true;
}

void FrameClock::sleepUntilSpin(qint64 targetNs) {
    // Pre-PE-14 wait: coarse sleep until ~1.5 ms before target, then spin for precision.
    // Windows timer granularity is ~1 ms even with timeBeginPeriod(1).
    for (;;) {
        const qint64 waitNs = targetNs - nowNs();
        if (waitNs <= 1500000LL) break;
        QThread::usleep(static_cast<unsigned long>((waitNs - 1500000LL) / 1000));
    }
    const qint64 spinFrom = nowNs();
    while (nowNs() < targetNs) {
        QThread::yieldCurrentThread();
    }
    m_spinTotalNs.fetch_add(nowNs() - spinFrom, std::memory_order_relaxed);
}

void FrameClock::sleepUntil(qint64 targetNs) {
#ifdef Q_OS_WIN
    if (m_timer) {
        // PE-14: a high-resolution waitable timer wakes us ~m_spinNs before the target (it is
        // not bound to the 1 ms scheduler tick), so only that margin is spun. Early wakes loop.
        // The timer fires 0.05-0.6 ms after its due time on this box (measured, PE-14 log), so the
        // due time is pulled in by its learned overshoot (an ~80th-percentile tracker: up by 1/8 of
        // the error, down by 1/32, capped at 1 ms); otherwise most ticks would land that much late.
        for (;;) {
            const qint64 now = nowNs();
            const qint64 waitNs = targetNs - now;
            const qint64 leadNs = m_spinNs + m_overshootNs;
            if (waitNs <= leadNs) break;
            LARGE_INTEGER due;
            due.QuadPart = -((waitNs - leadNs) / 100);           // relative, 100 ns units
            if (due.QuadPart >= 0) break;
            const DWORD capMs = DWORD(waitNs / 1000000LL) + 100;  // never wait forever on a lost timer
            if (!SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0)
                || WaitForSingleObject(m_timer, capMs) != WAIT_OBJECT_0) {
                // Timer broke: go back to the old wait for good (logged once, off the hot path).
                CloseHandle(m_timer); m_timer = nullptr;
                m_mode.store(ModeSpin);
                qWarning("[FrameClock] waitable timer failed (%lu); using the spin wait", GetLastError());
                sleepUntilSpin(targetNs);
                return;
            }
            const qint64 over = nowNs() - (targetNs - leadNs);   // how late the timer fired
            const qint64 err = std::clamp<qint64>(over, -1000000, 1000000) - m_overshootNs;
            m_overshootNs += err > 0 ? err / 8 : err / 32;
            m_overshootNs = std::clamp<qint64>(m_overshootNs, 0, 1000000);
            m_overshootStat.store(m_overshootNs, std::memory_order_relaxed);
        }
        const qint64 spinFrom = nowNs();
        qint64 t = spinFrom;
        while (t < targetNs) { SwitchToThread(); t = nowNs(); }
        m_spinTotalNs.fetch_add(t - spinFrom, std::memory_order_relaxed);
        return;
    }
#endif
    sleepUntilSpin(targetNs);
}

void FrameClock::recordTick(qint64 lateNs) {
    if (lateNs < 0) lateNs = 0;
    const qint64 us = lateNs / 1000;
    int b = 0;
    while (b < kLateBuckets - 1 && us > kLateEdgesUs[b]) ++b;
    m_lateHist[b].fetch_add(1, std::memory_order_relaxed);
    m_statTicks.fetch_add(1, std::memory_order_relaxed);
    if (lateNs > m_lateMaxNs.load(std::memory_order_relaxed)) m_lateMaxNs.store(lateNs, std::memory_order_relaxed);
}

void FrameClock::sampleThreadCpu(qint64 now) {
#ifdef Q_OS_WIN
    // QueryThreadCycleTime counts every cycle (GetThreadTimes is tick-sampled and misses the short
    // yield/spin bursts this thread is made of). Cycles are TSC ticks: converted with the TSC rate
    // measured against the steady clock over the same interval.
    ULONG64 cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) return;
    const qint64 tsc = qint64(__rdtsc());
    if (m_cpuAtNs.load() == 0) { m_cpuBaseCycles = qint64(cycles); m_tscBase = tsc; m_cpuNs.store(0); m_cpuAtNs.store(now); return; }
    const qint64 dTsc = tsc - m_tscBase, dNs = now - m_statStartNs.load();
    if (dTsc <= 0 || dNs <= 0) return;
    m_cpuNs.store(qint64(double(qint64(cycles) - m_cpuBaseCycles) * double(dNs) / double(dTsc)));
    m_cpuAtNs.store(now);
#else
    Q_UNUSED(now);
#endif
}

QString FrameClock::waitMode() const {
    switch (m_mode.load()) {
    case ModeHires: return QStringLiteral("hires");
    case ModeSpin:  return QStringLiteral("spin");
    default:        return QString();
    }
}

QJsonObject FrameClock::statsJson() const {
    QJsonObject o;
    o["mode"] = waitMode();
    o["spinUs"] = m_mode.load() == ModeHires ? double(m_spinNs / 1000) : 1500.0;
    const qint64 ticks = m_statTicks.load(std::memory_order_relaxed);
    o["ticks"] = double(ticks);
    QJsonObject h;
    qint64 cum = 0, p99 = -1;
    for (int b = 0; b < kLateBuckets; ++b) {
        const qint64 n = m_lateHist[b].load(std::memory_order_relaxed);
        const QString key = b < kLateBuckets - 1 ? QString::number(kLateEdgesUs[b]) : QStringLiteral("more");
        h[key] = double(n);
        cum += n;
        if (p99 < 0 && ticks > 0 && cum * 100 >= ticks * 99)
            p99 = b < kLateBuckets - 1 ? kLateEdgesUs[b] : m_lateMaxNs.load() / 1000;
    }
    o["lateHistUs"] = h;
    o["lateP99Us"] = double(p99 < 0 ? 0 : p99);
    o["lateMaxUs"] = double(m_lateMaxNs.load(std::memory_order_relaxed) / 1000);
    const qint64 cpuNs = m_cpuNs.load(), at = m_cpuAtNs.load(), start = m_statStartNs.load();
    o["spinAvgUs"] = ticks > 0 ? double(m_spinTotalNs.load(std::memory_order_relaxed) / ticks / 1000) : 0.0;
    o["timerLeadUs"] = double(m_overshootStat.load(std::memory_order_relaxed) / 1000);  // learned timer overshoot
    o["cpuMs"] = double(cpuNs / 1000000);
    o["cpuPct"] = (at > start && start > 0) ? qRound(double(cpuNs) / double(at - start) * 1000.0) / 10.0 : 0.0;
    return o;
}

void FrameClock::run() {
#ifdef Q_OS_WIN
    timeBeginPeriod(1); // 1 ms scheduler granularity for this process
    // PE-14: high-resolution waitable timer unless PLAYPLAY_CLOCK_SPIN=1 (old wait) or unavailable.
    {
        bool ok = false;
        const int spinUs = qEnvironmentVariableIntValue("PLAYPLAY_CLOCK_SPIN_US", &ok);
        m_spinNs = ok ? qint64(qBound(0, spinUs, 2000)) * 1000 : 200000;
        if (qEnvironmentVariableIntValue("PLAYPLAY_CLOCK_SPIN") != 1)
            m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        m_mode.store(m_timer ? ModeHires : ModeSpin);
    }
#else
    m_mode.store(ModeSpin);
#endif
    for (auto& b : m_lateHist) b.store(0);
    m_statTicks.store(0); m_lateMaxNs.store(0); m_cpuNs.store(0); m_cpuAtNs.store(0);
    m_overshootNs = 0; m_overshootStat.store(0); m_spinTotalNs.store(0);
    m_statStartNs.store(nowNs());
    sampleThreadCpu(m_statStartNs.load());
    qint64 nextCpuSampleNs = m_statStartNs.load() + 1000000000LL;
    FrameRate rate = frameRate();
    qint64 originNs = nowNs();
    m_originNs.store(originNs);
    qint64 frameNum = 0;
    double trimAccumNs = 0.0;       // accumulated rate trim, applied to every target

    while (!m_shouldStop.load()) {
        if (!m_isRunning.load()) {
            QThread::msleep(10);
            continue;
        }

        // Pick up a rate change (rare; restarts the timeline at current frame).
        {
            FrameRate r = frameRate();
            if (r != rate) {
                originNs = nowNs() - r.frameToNs(frameNum);
                m_originNs.store(originNs);
                rate = r;
            }
        }

        const qint64 masterOffset = m_masterOffsetNs.load();
        const qint64 frameNs = rate.frameDurationNs();
        // Rate trim: +ppm shortens every frame period by that fraction (the clock runs faster).
        trimAccumNs -= double(frameNs) * (m_trimPpmX100.load() / 100.0) * 1e-6;
        qint64 targetNs = originNs + rate.frameToNs(frameNum) + masterOffset + qint64(trimAccumNs);

        // Pending correction from the master clock.
        {
            QMutexLocker lock(&m_driftMutex);
            if (m_hasPendingCorrection) {
                const qint64 expectedAtNs = originNs + rate.frameToNs(m_pendingCorrectionFrame);
                const qint64 drift = m_pendingCorrectionAtNs - expectedAtNs;
                if (std::llabs(drift) > frameNs / 2) {
                    emit driftDetected(drift, m_pendingCorrectionFrame);
                    // Gradual: shift origin by 10% of drift per correction.
                    originNs += drift / 10;
                    m_originNs.store(originNs);
                    targetNs = originNs + rate.frameToNs(frameNum) + masterOffset + qint64(trimAccumNs);
                }
                m_hasPendingCorrection = false;
            }
        }

        const qint64 now = nowNs();
        if (now < targetNs) {
            sleepUntil(targetNs);
        } else if (now - targetNs > frameNs) {
            // Late by more than a frame: report, and skip ahead so we do not
            // burst-fire a backlog of ticks (which would starve outputs).
            emit driftDetected(now - targetNs, frameNum);
            const qint64 behind = (now - targetNs) / frameNs;
            frameNum += behind;
            targetNs = originNs + rate.frameToNs(frameNum) + masterOffset + qint64(trimAccumNs);
        }

        const qint64 emitNs = nowNs();
        recordTick(emitNs - targetNs);
        if (emitNs >= nextCpuSampleNs) { sampleThreadCpu(emitNs); nextCpuSampleNs = emitNs + 1000000000LL; }
        m_currentFrame.store(frameNum);
        emit frameTick(frameNum, targetNs);
        ++frameNum;
    }
    sampleThreadCpu(nowNs());
#ifdef Q_OS_WIN
    if (m_timer) { CloseHandle(m_timer); m_timer = nullptr; }
    timeEndPeriod(1);
#endif
}
