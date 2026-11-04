#pragma once
// Injectable time for the Studio engine (ST-02). The TS modules this engine ports take their clock as a parameter
// (backpressure.ts `now`, runqueue.ts `o.now`, the pub lane's setClockForTests) so their tests never sleep; the C++
// ports take a `Clock&` the same way. Production code passes Clock::system(); tests pass a FakeClock and move time by
// hand, timers included (AIMD recovery windows, quota TTLs, run-slot watchdogs, video poll intervals).
//
// Two time bases, as in any server that runs for hours:
//   nowMs()     wall clock, ms since the Unix epoch (UTC): stamps, expiries persisted to the DB, local calendar day.
//   steadyMs()  monotonic ms from an arbitrary origin: timeouts and durations, immune to NTP / DST / manual changes.
#include <QDateTime>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimeZone>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace studio {

class Clock {
public:
    using TimerId = quint64;               // never 0
    virtual ~Clock() = default;

    virtual qint64 nowMs() const = 0;
    virtual qint64 steadyMs() const = 0;

    /// Runs `fn` once, `ms` (clamped to >= 0) from now, on `context`'s thread, unless cancel() or the destruction of
    /// `context` comes first. Call it from `context`'s thread. Returns the id for cancel().
    virtual TimerId callAfter(qint64 ms, QObject* context, std::function<void()> fn) = 0;
    /// True when the timer was still pending (it will not run). Call it from the timer's context thread.
    virtual bool cancel(TimerId id) = 0;

    QDateTime now() const { return QDateTime::fromMSecsSinceEpoch(nowMs(), QTimeZone::UTC); }
    /// "2026-10-26T14:03:12.345Z" (JS Date.toISOString).
    QString isoNow() const { return isoUtc(nowMs()); }
    /// Local calendar date "yyyy-MM-dd" of now: daily caps follow the user's day, not UTC (mcp/core/util.ts localDate).
    QString localDate() const { return localDateOf(nowMs()); }

    static QString isoUtc(qint64 epochMs);
    static QString localDateOf(qint64 epochMs);
    /// The process-wide real clock (thread-safe; lives until exit).
    static Clock& system();
};

/// Real time: QDateTime for the wall clock, QElapsedTimer for steady time, a single-shot QTimer per callAfter().
class SystemClock final : public Clock {
public:
    SystemClock();
    ~SystemClock() override;
    qint64 nowMs() const override;
    qint64 steadyMs() const override;
    TimerId callAfter(qint64 ms, QObject* context, std::function<void()> fn) override;
    bool cancel(TimerId id) override;

private:
    struct State;
    std::shared_ptr<State> m_state;        // shared with the timers, so a late timeout never touches a dead clock
};

/// Manual time for tests. Nothing moves until advance() / setNowMs(). Not meant to be shared across threads beyond
/// reading the time (nowMs/steadyMs are thread-safe; timers run on the thread that calls advance()).
class FakeClock final : public Clock {
public:
    /// Starts at `startMs` wall time (default 2026-10-26T12:00:00Z) and steady time 0.
    explicit FakeClock(qint64 startMs = 1793016000000LL);
    qint64 nowMs() const override;
    qint64 steadyMs() const override;
    TimerId callAfter(qint64 ms, QObject* context, std::function<void()> fn) override;
    bool cancel(TimerId id) override;

    /// Moves wall and steady time forward by `ms`, running every timer that falls due on the way, in due order (ties in
    /// the order they were set), each with the clock showing its due time. Timers set by a callback run in the same
    /// advance when they fall due before its end. Returns how many timers ran.
    int advance(qint64 ms);
    /// Runs the timers already due (callAfter(0, ...) without moving time).
    int runDue() { return advance(0); }
    /// Jumps the wall clock only (an NTP step, a day roll-over, a manual change): steady time and timers stay put.
    void setNowMs(qint64 ms);
    int pendingTimers() const;
    /// Steady time of the next pending timer, or -1.
    qint64 nextDueSteadyMs() const;

private:
    struct Timer { QPointer<QObject> context; bool hadContext; std::function<void()> fn; };
    using Key = std::pair<qint64, TimerId>;  // (due, id): ids grow, so equal due times keep the order they were set
    mutable std::mutex m_mutex;
    qint64 m_wallOffset;                   // wall = steady + offset
    qint64 m_steady = 0;
    TimerId m_nextId = 0;
    std::map<Key, Timer> m_queue;
    std::map<TimerId, qint64> m_due;       // id -> due, for cancel()
};

} // namespace studio
