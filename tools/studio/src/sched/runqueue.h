#pragma once
// The run queue (ST-10, docs/plans/starmedia-studio.md §4.3 layer 2): a port of starmedia src/studio/runqueue.ts.
// Several projects and agents work at the same time, but a few heavy runs (video generation, renders, many images)
// must not starve the rest, nor the small runs the screens make. The queue limits heavy work across all projects,
// keeps capacity free for light work, and reports what each project is doing.
//
//   - two pools, heavy and light, independent: a light run never waits behind a heavy one;
//   - higher priority first; at equal priority the sessions (projects) take turns, the one served longest ago first,
//     and within one session first come first served, so a project that queues twenty runs cannot starve the others;
//   - group + groupLimit cap a kind of run across both pools (renders: one at a time) without blocking the runs queued
//     behind it;
//   - an AbortSignal takes a waiting run out of the line at once (refused with the signal's reason, or "Cancelled
//     while waiting for a free slot" when it gave none); once the slot is granted the holder releases it, an abort
//     later does not take it away (the work has to wind down first); release() twice is harmless;
//   - watchdog: a slot held longer than maxHoldMs (default 45 min; SlotMeta::maxHoldMs per run; 0 = never) is taken
//     back by sweep() and onWarning is told. A run whose provider call never answers must not keep one of the few heavy
//     slots for ever; the holder's own release() afterwards is a harmless no-op. The queue's Clock arms one timer per
//     slot (a FakeClock in tests), and sweep() can be called by hand;
//   - limits: a number >= 1; kUnlimited = no limit; anything else (0, negative) keeps the default (heavy 3, light 8)
//     or, in setLimits(), the limit in force. groupLimit 0 = unlimited, >= 1 = that many, negative = 1.
//
// Deterministic: no globals, injectable Clock. Timing differs from the TS in one way: a promise there resolves as a
// microtask, so here `granted` runs synchronously — from inside acquire() when a slot is free, else later from the
// release(), cancel(), sweep() or setLimits() that made room — after the queue's own bookkeeping of that round, in
// the order the slots were granted. It may call back into the queue (acquire, release). `refused` runs synchronously
// from the abort / cancel that caused it.
//
// Threads: a RunQueue lives on one thread (the Studio's main loop) and is used from it, like Gates. release() and an
// abort may come from any thread: they hop to the queue's thread. The change listeners run on the queue's thread.
#include "provider/gates.h"   // AbortSignal

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QVariant>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace studio {
class Clock;
}

namespace studio::sched {

namespace detail {
struct QueueLink;
}

enum class SlotKind { Heavy, Light };
QString slotKindName(SlotKind k);          // "heavy" | "light"
/// "heavy" → Heavy; anything else → Light (the TS treats an unknown kind as light).
SlotKind slotKindFromName(QStringView s);

constexpr int kBaseQueueHeavy = 3;         // concurrency.mjs BASE queue.heavy
constexpr int kBaseQueueLight = 8;         // BASE queue.light
constexpr qint64 kDefaultMaxHoldMs = 45LL * 60 * 1000;   // TS DEFAULT_MAX_HOLD_MS
constexpr int kUnlimited = std::numeric_limits<int>::max();

struct SlotMeta {
    QString session;                       // the project (round robin is per session)
    QString label;                         // for the board: "Coffee: generating shots"
    int priority = 0;                      // higher first
    QString phase;                         // which run phase ("assets", "render" ...)
    QString group;                         // runs with the same group share groupLimit slots across both pools
    int groupLimit = 1;                    // 0 = unlimited, negative = 1
    qint64 maxHoldMs = -1;                 // -1 = the queue's maxHoldMs; 0 = never; else ms
    QVariant user;                         // the caller's payload (a run id); never read here
    /// {session, label, priority[, phase][, group, groupLimit][, maxHoldMs]} (the TS spreads the meta).
    QJsonObject toJson() const;
};

struct QueueLimits {
    int heavy = kBaseQueueHeavy;
    int light = kBaseQueueLight;
    int of(SlotKind k) const { return k == SlotKind::Heavy ? heavy : light; }
};

/// A run holding a slot.
struct SlotInfo {
    quint64 id = 0;
    SlotKind kind = SlotKind::Light;
    SlotMeta meta;
    qint64 sinceMs = 0;                    // wall clock, ms since the epoch
    qint64 heldMs = 0;
    QJsonObject toJson() const;            // meta + {kind, since, id, heldS}
};
/// A run waiting for a slot.
struct WaitInfo {
    quint64 id = 0;
    SlotKind kind = SlotKind::Light;
    SlotMeta meta;
    qint64 sinceMs = 0;
    qint64 waitedMs = 0;
    int ahead = 0;                         // runs of its kind the queue serves before it (0 = next)
    QString why;                           // "pool" (its pool is full) | "group" (no room in its group) | "next"
    QJsonObject toJson() const;            // meta + {kind, since, id, waitedS, ahead, why}
};

struct KindMetrics {
    qint64 acquired = 0, waitMsTotal = 0, waitMsMax = 0;
};
struct QueueMetrics {
    qint64 acquired = 0, released = 0, aborted = 0, cancelled = 0;
    int queuedNow = 0, runningNow = 0;
    int peakHeavy = 0, peakLight = 0, peakQueued = 0;
    qint64 waitMsTotal = 0, waitMsMax = 0;
    qint64 expired = 0;                    // slots the watchdog took back
    KindMetrics heavy, light;
    const KindMetrics& of(SlotKind k) const { return k == SlotKind::Heavy ? heavy : light; }
    QJsonObject toJson() const;            // the TS QueueMetrics shape
};

struct RunQueueOptions {
    int heavy = kBaseQueueHeavy;
    int light = kBaseQueueLight;
    qint64 maxHoldMs = kDefaultMaxHoldMs;  // 0 (or negative) = no watchdog
    /// Told when the watchdog takes a slot back (the session shows it in the log).
    std::function<void(const QString& message, const SlotInfo& slot)> onWarning;
};

class RunQueue : public QObject {
    Q_OBJECT
public:
    /// The slot is yours: `id` is the one acquire() returned; call release(id) when done (always, also on errors).
    using Granted = std::function<void(quint64 id)>;
    /// No slot: the signal fired, cancel() / cancelSession() dropped the run, or the queue went away.
    using Refused = std::function<void(const QString& why)>;
    struct AcquireOptions {
        /// Called when the run is queued and again whenever the number of runs ahead of it changes (0 = next in line).
        std::function<void(int ahead)> onWait;
    };
    using ListenerId = quint64;

    explicit RunQueue(Clock& clock, RunQueueOptions o = {}, QObject* parent = nullptr);
    ~RunQueue() override;

    QueueLimits limits() const { return m_limits; }
    /// Changes the limits at run time (nullopt keeps one): runs already going keep their slot, waiting ones start
    /// when there is room. Invalid numbers keep the limit in force.
    void setLimits(std::optional<int> heavy, std::optional<int> light);
    void setLimits(const QueueLimits& l) { setLimits(l.heavy, l.light); }
    qint64 maxHoldMs() const { return m_o.maxHoldMs; }
    void setMaxHoldMs(qint64 ms) { m_o.maxHoldMs = ms; }

    /// Asks for a slot. Returns the run's id (for cancel() / setPriority() / release(); 0 when refused at once).
    /// `granted` or `refused` runs exactly once (see the thread and timing notes above).
    quint64 acquire(SlotKind kind, const SlotMeta& meta, Granted granted, Refused refused = {},
                    const provider::AbortSignal& signal = {}, AcquireOptions o = {});
    /// Gives a slot back (any thread). Twice, or after the watchdog took it, is harmless.
    void release(quint64 id);

    /// Drops one waiting run (the board's Cancel): its refused() runs with `why`. A run that already holds a slot is
    /// not touched (false). Counts as cancelled.
    bool cancel(quint64 id, const QString& why = QStringLiteral("Cancelled while waiting for a free slot"));
    /// Changes the priority of one waiting run (the board's "Run next"). False when it is not waiting.
    bool setPriority(quint64 id, int priority);
    /// The highest priority anything waiting has right now (0 when nothing waits), so a caller can go in front.
    int topPriority() const;
    /// Drops everything a session has waiting (it was closed); its running runs are not touched. How many dropped.
    int cancelSession(const QString& session);

    /// Watchdog: takes back every slot held longer than its limit, with onWarning for each. The per-slot timers call
    /// it; tests call it after moving a FakeClock. Returns how many it took back.
    int sweep();

    // ---------------- what is going on ----------------
    int runningCount(SlotKind k) const;
    int queuedCount(SlotKind k) const;
    int runningTotal() const { return int(m_slots.size()); }
    int queuedTotal() const { return int(m_waiting.size()); }
    bool isRunning(quint64 id) const;
    bool isWaiting(quint64 id) const;
    /// Runs holding a slot, in the order they started.
    QList<SlotInfo> running() const;
    /// Runs waiting, per kind in the order the queue will serve them (heavy first, then light).
    QList<WaitInfo> queued() const;
    /// TS QueueStatus: {running: [...], queued: [...], byThread: {session: {running, queued, label}}, limits:
    /// {heavy, light} (null = unlimited), groups: {name: {running, limit}}}. A snapshot.
    QJsonObject status() const;
    QueueMetrics metrics() const;

    /// Told after every change (a start, a queue, a release, a cancel, new limits), on the queue's thread; a listener
    /// that throws does not break the queue or the others. A listener may call back into the queue.
    ListenerId onChange(std::function<void()> fn);
    void removeChangeListener(ListenerId id);

private:
    struct Waiter {
        quint64 id = 0;                    // also the arrival order
        SlotKind kind = SlotKind::Light;
        SlotMeta meta;
        qint64 sinceSteady = 0, sinceWall = 0;
        Granted granted;
        Refused refused;
        provider::AbortSignal signal;
        provider::AbortSignal::ListenerId listener = 0;
        std::function<void(int)> onWait;
        int lastAhead = -1;
    };
    struct Slot {
        quint64 id = 0;
        SlotKind kind = SlotKind::Light;
        SlotMeta meta;
        qint64 sinceSteady = 0, sinceWall = 0;
        quint64 timer = 0;                 // Clock timer id, 0 = none
    };

    static int groupLimitOf(const SlotMeta& m);     // 0 = unlimited
    int groupCount(const QString& group) const;
    bool groupOk(const SlotMeta& m) const;
    qint64 holdLimit(const Slot& s) const;          // 0 = no watchdog
    void arm(Slot& s, qint64 ms);
    const Waiter* pick(SlotKind kind) const;
    const std::vector<quint64>& order(SlotKind kind) const;
    void start(quint64 id);
    void fill();
    void notifyWaiters();
    void change();
    void refuse(Waiter w, const QString& why, bool aborted);
    void abortWaiter(quint64 id);
    void releaseHere(quint64 id);
    std::vector<Waiter>::iterator findWaiter(quint64 id);

    Clock& m_clock;
    RunQueueOptions m_o;
    QueueLimits m_limits;
    std::shared_ptr<detail::QueueLink> m_link;
    std::vector<Waiter> m_waiting;         // arrival order
    std::vector<Slot> m_slots;             // start order
    std::array<QHash<QString, qint64>, 2> m_served;   // per kind: session → the tick it was last served at
    qint64 m_tick = 0;
    quint64 m_ids = 0;
    std::map<ListenerId, std::function<void()>> m_listeners;
    ListenerId m_nextListener = 0;
    bool m_pumping = false;
    bool m_again = false;
    quint64 m_ver = 0;                     // bumped whenever the waiting list or the served order changes
    struct OrderCache { quint64 ver = ~0ULL; std::vector<quint64> ids; };
    mutable std::array<OrderCache, 2> m_orders;
    std::vector<std::pair<quint64, Granted>> m_granted;   // callbacks due after this round's bookkeeping
    QueueMetrics m_m;
};

} // namespace studio::sched
