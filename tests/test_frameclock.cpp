#include <QtTest/QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
#include "playout/core/frameclock.h"

// FrameClock timing test.
//
// The clock runs on its own thread and emits frameTick(frameNumber, targetNs).
// We connect with Qt::DirectConnection so the slot runs on the clock thread
// and can measure how late each tick fired relative to its target.
class TestFrameClock : public QObject {
    Q_OBJECT

private slots:
    void ticksAtConfiguredRate();
    void latenessStaysBounded();
    void waitModesAndCpu();         // PE-14: hires timer vs PLAYPLAY_CLOCK_SPIN=1
    void spinMarginEnv();
    void cleanup() { qunsetenv("PLAYPLAY_CLOCK_SPIN"); qunsetenv("PLAYPLAY_CLOCK_SPIN_US"); }
};

static qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TickLog {
    std::mutex m;
    std::vector<qint64> lateNs;
    std::vector<qint64> frames;
};

static QJsonObject runClock(int fps, int ms, TickLog& log, QString* modeOut = nullptr) {
    FrameClock clock;
    clock.setFrameRate(fps);
    QObject::connect(&clock, &FrameClock::frameTick, &clock,
        [&log](qint64 frame, qint64 targetNs) {
            std::lock_guard<std::mutex> g(log.m);
            log.frames.push_back(frame);
            log.lateNs.push_back(nowNs() - targetNs);
        }, Qt::DirectConnection);
    clock.setRunning(true);
    QTest::qWait(ms);
    if (modeOut) *modeOut = clock.waitMode();
    clock.setRunning(false);
    clock.wait(1000);
    return clock.statsJson();       // final CPU sample is taken as the thread exits
}

static qint64 p99Of(std::vector<qint64> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() * 99 / 100];
}

void TestFrameClock::ticksAtConfiguredRate() {
    TickLog log;
    runClock(30, 2000, log);
    // 2 s at 30 fps = 60 ticks; allow start/stop slop.
    QVERIFY2(log.frames.size() >= 55 && log.frames.size() <= 65,
             qPrintable(QString("tick count %1").arg(log.frames.size())));
    // Frame numbers must be strictly consecutive.
    for (size_t i = 1; i < log.frames.size(); ++i) {
        QCOMPARE(log.frames[i], log.frames[i - 1] + 1);
    }
}

void TestFrameClock::latenessStaysBounded() {
    TickLog log;
    runClock(60, 2000, log);
    QVERIFY(log.lateNs.size() > 100);
    std::vector<qint64> sorted = log.lateNs;
    std::sort(sorted.begin(), sorted.end());
    qint64 p50 = sorted[sorted.size() / 2];
    qint64 p99 = sorted[sorted.size() * 99 / 100];
    qint64 maxLate = sorted.back();
    qInfo("fps=60 ticks=%zu p50 late=%lld us p99 late=%lld us max late=%lld us",
          sorted.size(), (long long)(p50 / 1000), (long long)(p99 / 1000), (long long)(maxLate / 1000));
    // Broadcast target is sub-millisecond; on a loaded dev box allow 2 ms at p99.
    QVERIFY2(p99 < 2'000'000LL, qPrintable(QString("p99 lateness %1 us").arg(p99 / 1000)));
    // Never a full frame late (16.6 ms at 60 fps).
    QVERIFY2(maxLate < 16'000'000LL, qPrintable(QString("max lateness %1 us").arg(maxLate / 1000)));
}

// PE-14: the default wait is the high-resolution waitable timer; PLAYPLAY_CLOCK_SPIN=1 restores the
// old usleep + 1.5 ms spin. Both keep lateness bounded; the timer mode burns less clock-thread CPU.
void TestFrameClock::waitModesAndCpu() {
    struct Run { QString mode; QJsonObject st; qint64 p99 = 0; size_t ticks = 0; };
    auto one = [](bool spin) {
        if (spin) qputenv("PLAYPLAY_CLOCK_SPIN", "1"); else qunsetenv("PLAYPLAY_CLOCK_SPIN");
        TickLog log; Run r;
        r.st = runClock(60, 3000, log, &r.mode);
        r.p99 = p99Of(log.lateNs); r.ticks = log.lateNs.size();
        qInfo("mode=%s ticks=%zu p99 late=%lld us stats=%s", qPrintable(r.mode), r.ticks, (long long)(r.p99 / 1000),
              QJsonDocument(r.st).toJson(QJsonDocument::Compact).constData());
        return r;
    };
    const Run hires = one(false), spin = one(true);
    QCOMPARE(spin.mode, QString("spin"));
    QCOMPARE(spin.st.value("mode").toString(), QString("spin"));
#ifdef Q_OS_WIN
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION exists since Windows 10 1803; every target here has it.
    QCOMPARE(hires.mode, QString("hires"));
    QCOMPARE(hires.st.value("spinUs").toDouble(), 200.0);
#endif
    for (const Run* r : {&hires, &spin}) {
        QVERIFY2(r->ticks >= 170 && r->ticks <= 190, qPrintable(QString("%1 ticks %2").arg(r->mode).arg(r->ticks)));
        QVERIFY2(r->p99 < 2'000'000LL, qPrintable(QString("%1 p99 lateness %2 us").arg(r->mode).arg(r->p99 / 1000)));
        // The histogram counts every emitted tick, and its p99 agrees with the test's own measurement.
        const QJsonObject h = r->st.value("lateHistUs").toObject();
        double sum = 0; for (auto it = h.begin(); it != h.end(); ++it) sum += it.value().toDouble();
        QCOMPARE(qint64(sum), qint64(r->st.value("ticks").toDouble()));
        QVERIFY(qAbs(qint64(sum) - qint64(r->ticks)) <= 2);
        QVERIFY2(r->st.value("lateP99Us").toDouble() <= 2000.0, qPrintable(QString("%1 stats p99").arg(r->mode)));
    }
#ifdef Q_OS_WIN
    const double cpuHires = hires.st.value("cpuMs").toDouble(), cpuSpin = spin.st.value("cpuMs").toDouble();
    qInfo("clock-thread CPU over 3 s at 60 fps: hires %.0f ms (%.1f %%), spin %.0f ms (%.1f %%)",
          cpuHires, hires.st.value("cpuPct").toDouble(), cpuSpin, spin.st.value("cpuPct").toDouble());
    QVERIFY2(cpuSpin > 0 && cpuHires > 0, "the cycle-exact CPU sample must see the clock thread");
    // Measured here: hires 1.6-1.7 %, spin 1.9-2.1 % of a core at 60 fps (PE-14 log). The gap is small on a
    // busy box, so the gate is "no worse" with slack for a 3 s sample, not "less".
    QVERIFY2(cpuHires <= cpuSpin * 1.3 + 10.0, qPrintable(QString("hires %1 ms vs spin %2 ms").arg(cpuHires).arg(cpuSpin)));
    // The timer mode's final spin is the learned timer lead + 200 us, never the old 1.5 ms window.
    QVERIFY2(hires.st.value("spinAvgUs").toDouble() < 1200.0, "hires spin per tick");
    QVERIFY(hires.st.value("timerLeadUs").toDouble() <= 1000.0);
#endif
}

void TestFrameClock::spinMarginEnv() {
    qputenv("PLAYPLAY_CLOCK_SPIN_US", "500");
    TickLog log; QString mode;
    const QJsonObject st = runClock(30, 600, log, &mode);
    if (mode == "hires") QCOMPARE(st.value("spinUs").toDouble(), 500.0);
    qputenv("PLAYPLAY_CLOCK_SPIN_US", "99999");        // clamped to 2000
    const QJsonObject st2 = runClock(30, 300, log, &mode);
    if (mode == "hires") QCOMPARE(st2.value("spinUs").toDouble(), 2000.0);
    QVERIFY(log.frames.size() > 5);
}

QTEST_GUILESS_MAIN(TestFrameClock)
#include "test_frameclock.moc"
