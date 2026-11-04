// The run queue (ST-10). See runqueue.h for the rules; this file keeps the TS RunQueue structure (fill the pools,
// tell the waiters their positions, then the listeners) with synchronous callbacks in place of promises.
#include "sched/runqueue.h"

#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <exception>
#include <mutex>

#include "core/clock.h"
#include "core/log.h"

namespace studio::sched {

namespace detail {
/// A weak handle on a RunQueue that any thread may hold (release(), an abort): ~RunQueue clears it under the mutex,
/// so a post to a live queue is never a post to a dead one (and Qt drops a post whose receiver dies later).
struct QueueLink {
    std::mutex mutex;
    RunQueue* queue = nullptr;
};
} // namespace detail

namespace {

constexpr QStringView kCat = u"runqueue";
constexpr qint64 kMaxTimerMs = 2000000000LL;         // the TS setTimeout cap; QTimer takes an int of ms

// Runs fn(queue) on the queue's thread: at once when we are on it, else queued. Nothing when the queue is gone.
template <typename F>
void onQueueThread(const std::shared_ptr<detail::QueueLink>& link, F fn) {
    if (!link) return;
    std::unique_lock lock(link->mutex);
    RunQueue* q = link->queue;
    if (!q) return;
    if (q->thread() == QThread::currentThread()) {
        lock.unlock();                                   // on its own thread a queue cannot die under us
        fn(q);
        return;
    }
    QMetaObject::invokeMethod(q, [link, fn]() mutable {
        RunQueue* queue = nullptr;
        { std::lock_guard l(link->mutex); queue = link->queue; }
        if (queue) fn(queue);
    }, Qt::QueuedConnection);
}

/// A reason a caller gave ("the project was closed") is kept; the engines' own generic abort texts (the
/// AbortController default "Cancelled", anything that says "abort") say nothing and become the queue's own words.
QString abortText(const QString& why) {
    const QString msg = why.trimmed();
    if (msg.isEmpty() || msg == u"Cancelled" || msg.contains(u"abort", Qt::CaseInsensitive))
        return QStringLiteral("Cancelled while waiting for a free slot");
    return msg;
}

/// A limit: kUnlimited stays, a number >= 1 is kept, anything else is `d`.
int lim(int n, int d) { return n == kUnlimited || n >= 1 ? n : d; }

qint64 roundSeconds(qint64 ms) { return std::llround(double(std::max<qint64>(0, ms)) / 1000.0); }

} // namespace

// ---------------- names and plain data ----------------

QString slotKindName(SlotKind k) { return k == SlotKind::Heavy ? QStringLiteral("heavy") : QStringLiteral("light"); }

SlotKind slotKindFromName(QStringView s) { return s == u"heavy" ? SlotKind::Heavy : SlotKind::Light; }

QJsonObject SlotMeta::toJson() const {
    QJsonObject o;
    o.insert(QStringLiteral("session"), session);
    o.insert(QStringLiteral("label"), label);
    o.insert(QStringLiteral("priority"), priority);
    if (!phase.isEmpty()) o.insert(QStringLiteral("phase"), phase);
    if (!group.isEmpty()) {
        o.insert(QStringLiteral("group"), group);
        o.insert(QStringLiteral("groupLimit"), groupLimit);
    }
    if (maxHoldMs >= 0) o.insert(QStringLiteral("maxHoldMs"), double(maxHoldMs));
    return o;
}

QJsonObject SlotInfo::toJson() const {
    QJsonObject o = meta.toJson();
    o.insert(QStringLiteral("kind"), slotKindName(kind));
    o.insert(QStringLiteral("since"), double(sinceMs));
    o.insert(QStringLiteral("id"), double(id));
    o.insert(QStringLiteral("heldS"), double(roundSeconds(heldMs)));
    return o;
}

QJsonObject WaitInfo::toJson() const {
    QJsonObject o = meta.toJson();
    o.insert(QStringLiteral("kind"), slotKindName(kind));
    o.insert(QStringLiteral("since"), double(sinceMs));
    o.insert(QStringLiteral("id"), double(id));
    o.insert(QStringLiteral("waitedS"), double(roundSeconds(waitedMs)));
    o.insert(QStringLiteral("ahead"), ahead);
    o.insert(QStringLiteral("why"), why);
    return o;
}

QJsonObject QueueMetrics::toJson() const {
    auto kind = [](const KindMetrics& k) {
        return QJsonObject{{QStringLiteral("acquired"), double(k.acquired)},
                           {QStringLiteral("waitMsTotal"), double(k.waitMsTotal)},
                           {QStringLiteral("waitMsMax"), double(k.waitMsMax)}};
    };
    QJsonObject o;
    o.insert(QStringLiteral("acquired"), double(acquired));
    o.insert(QStringLiteral("released"), double(released));
    o.insert(QStringLiteral("aborted"), double(aborted));
    o.insert(QStringLiteral("cancelled"), double(cancelled));
    o.insert(QStringLiteral("queuedNow"), queuedNow);
    o.insert(QStringLiteral("runningNow"), runningNow);
    o.insert(QStringLiteral("peakRunning"), QJsonObject{{QStringLiteral("heavy"), peakHeavy}, {QStringLiteral("light"), peakLight}});
    o.insert(QStringLiteral("peakQueued"), peakQueued);
    o.insert(QStringLiteral("waitMsTotal"), double(waitMsTotal));
    o.insert(QStringLiteral("waitMsMax"), double(waitMsMax));
    o.insert(QStringLiteral("expired"), double(expired));
    o.insert(QStringLiteral("byKind"), QJsonObject{{QStringLiteral("heavy"), kind(heavy)}, {QStringLiteral("light"), kind(light)}});
    return o;
}

// ---------------- the queue ----------------

RunQueue::RunQueue(Clock& clock, RunQueueOptions o, QObject* parent)
    : QObject(parent), m_clock(clock), m_o(std::move(o)), m_link(std::make_shared<detail::QueueLink>()) {
    m_limits.heavy = lim(m_o.heavy, kBaseQueueHeavy);
    m_limits.light = lim(m_o.light, kBaseQueueLight);
    m_link->queue = this;
}

RunQueue::~RunQueue() {
    { std::lock_guard lock(m_link->mutex); m_link->queue = nullptr; }
    for (Slot& s : m_slots)
        if (s.timer) m_clock.cancel(s.timer);
    m_slots.clear();
    std::vector<Waiter> waiting;
    waiting.swap(m_waiting);
    for (Waiter& w : waiting) {
        w.signal.removeListener(w.listener);
        if (w.refused) {
            try {
                w.refused(QStringLiteral("the run queue was closed"));
            } catch (...) {
            }
        }
    }
}

void RunQueue::setLimits(std::optional<int> heavy, std::optional<int> light) {
    if (heavy) m_limits.heavy = lim(*heavy, m_limits.heavy);
    if (light) m_limits.light = lim(*light, m_limits.light);
    change();
}

quint64 RunQueue::acquire(SlotKind kind, const SlotMeta& meta, Granted granted, Refused refused,
                          const provider::AbortSignal& signal, AcquireOptions o) {
    if (signal.aborted()) {
        ++m_m.aborted;
        if (refused) refused(abortText(signal.reason()));
        return 0;
    }
    Waiter w;
    w.id = ++m_ids;
    w.kind = kind;
    w.meta = meta;
    w.sinceSteady = m_clock.steadyMs();
    w.sinceWall = m_clock.nowMs();
    w.granted = std::move(granted);
    w.refused = std::move(refused);
    w.signal = signal;
    w.onWait = std::move(o.onWait);
    if (signal.valid()) {
        // a Stop while waiting takes the run out of the line at once (from whatever thread pressed it)
        w.listener = signal.onAbort([link = m_link, id = w.id] {
            onQueueThread(link, [id](RunQueue* q) { q->abortWaiter(id); });
        });
        if (!w.listener && signal.aborted()) {           // fired between the check above and the registration
            ++m_m.aborted;
            if (w.refused) w.refused(abortText(signal.reason()));
            return 0;
        }
    }
    const quint64 id = w.id;
    m_waiting.push_back(std::move(w));
    ++m_ver;
    m_m.peakQueued = std::max(m_m.peakQueued, int(m_waiting.size()));
    change();
    return id;
}

void RunQueue::release(quint64 id) {
    onQueueThread(m_link, [id](RunQueue* q) { q->releaseHere(id); });
}

void RunQueue::releaseHere(quint64 id) {
    auto it = std::find_if(m_slots.begin(), m_slots.end(), [&](const Slot& s) { return s.id == id; });
    if (it == m_slots.end()) return;                     // twice, or the watchdog took it: harmless
    if (it->timer) m_clock.cancel(it->timer);
    m_slots.erase(it);
    ++m_m.released;
    change();
}

std::vector<RunQueue::Waiter>::iterator RunQueue::findWaiter(quint64 id) {
    return std::find_if(m_waiting.begin(), m_waiting.end(), [&](const Waiter& w) { return w.id == id; });
}

void RunQueue::refuse(Waiter w, const QString& why, bool aborted) {
    w.signal.removeListener(w.listener);
    if (aborted) ++m_m.aborted;
    else ++m_m.cancelled;
    if (w.refused) w.refused(why);
}

void RunQueue::abortWaiter(quint64 id) {
    auto it = findWaiter(id);
    if (it == m_waiting.end()) return;                   // already running: the holder releases
    Waiter w = std::move(*it);
    m_waiting.erase(it);
    ++m_ver;
    const QString why = abortText(w.signal.reason());
    refuse(std::move(w), why, true);
    change();
}

bool RunQueue::cancel(quint64 id, const QString& why) {
    auto it = findWaiter(id);
    if (it == m_waiting.end()) return false;
    Waiter w = std::move(*it);
    m_waiting.erase(it);
    ++m_ver;
    refuse(std::move(w), abortText(why), false);
    change();
    return true;
}

bool RunQueue::setPriority(quint64 id, int priority) {
    auto it = findWaiter(id);
    if (it == m_waiting.end()) return false;
    it->meta.priority = priority;
    ++m_ver;
    change();
    return true;
}

int RunQueue::topPriority() const {
    int top = 0;
    for (const Waiter& w : m_waiting) top = std::max(top, w.meta.priority);
    return top;
}

int RunQueue::cancelSession(const QString& session) {
    std::vector<Waiter> drop;
    for (auto it = m_waiting.begin(); it != m_waiting.end();) {
        if (it->meta.session == session) {
            drop.push_back(std::move(*it));
            it = m_waiting.erase(it);
        } else {
            ++it;
        }
    }
    if (drop.empty()) return 0;
    ++m_ver;
    for (Waiter& w : drop)
        refuse(std::move(w), QStringLiteral("The project was closed or stopped while waiting for a free slot"), false);
    m_served[0].remove(session);
    m_served[1].remove(session);
    change();
    return int(drop.size());
}

// ---------------- internals ----------------

int RunQueue::groupLimitOf(const SlotMeta& m) { return m.groupLimit < 0 ? 1 : m.groupLimit; }

int RunQueue::groupCount(const QString& group) const {
    int n = 0;
    for (const Slot& s : m_slots) n += s.meta.group == group;
    return n;
}

bool RunQueue::groupOk(const SlotMeta& m) const {
    if (m.group.isEmpty()) return true;
    const int limit = groupLimitOf(m);
    return limit == 0 || groupCount(m.group) < limit;
}

int RunQueue::runningCount(SlotKind k) const {
    int n = 0;
    for (const Slot& s : m_slots) n += s.kind == k;
    return n;
}

int RunQueue::queuedCount(SlotKind k) const {
    int n = 0;
    for (const Waiter& w : m_waiting) n += w.kind == k;
    return n;
}

bool RunQueue::isRunning(quint64 id) const {
    return std::any_of(m_slots.begin(), m_slots.end(), [&](const Slot& s) { return s.id == id; });
}

bool RunQueue::isWaiting(quint64 id) const {
    return std::any_of(m_waiting.begin(), m_waiting.end(), [&](const Waiter& w) { return w.id == id; });
}

/// The waiter that goes next in `kind`: priority, then the session served longest ago, then arrival order.
const RunQueue::Waiter* RunQueue::pick(SlotKind kind) const {
    const Waiter* best = nullptr;
    qint64 bs = 0;
    const QHash<QString, qint64>& served = m_served[size_t(kind)];
    for (const Waiter& w : m_waiting) {
        if (w.kind != kind || !groupOk(w.meta)) continue;
        const qint64 s = served.value(w.meta.session, -1);
        if (!best || w.meta.priority > best->meta.priority ||
            (w.meta.priority == best->meta.priority && (s < bs || (s == bs && w.id < best->id)))) {
            best = &w;
            bs = s;
        }
    }
    return best;
}

/// The order this kind's waiters will be served in, ignoring free capacity and group limits (positions for the
/// board and onWait). Worked out once per change: within one session the next run is always its best (priority,
/// then arrival), and across sessions the heads compete by priority, then served-longest-ago, then arrival.
const std::vector<quint64>& RunQueue::order(SlotKind kind) const {
    OrderCache& cache = m_orders[size_t(kind)];
    if (cache.ver == m_ver) return cache.ids;
    struct Lane {
        QString session;
        std::vector<const Waiter*> l;
        size_t i = 0;
    };
    std::vector<Lane> lanes;
    QHash<QString, size_t> laneOf;
    for (const Waiter& w : m_waiting) {
        if (w.kind != kind) continue;
        auto it = laneOf.find(w.meta.session);
        if (it == laneOf.end()) {
            it = laneOf.insert(w.meta.session, lanes.size());
            lanes.push_back(Lane{w.meta.session, {}, 0});
        }
        lanes[*it].l.push_back(&w);
    }
    for (Lane& x : lanes)
        std::stable_sort(x.l.begin(), x.l.end(), [](const Waiter* a, const Waiter* b) {
            return a->meta.priority != b->meta.priority ? a->meta.priority > b->meta.priority : a->id < b->id;
        });
    QHash<QString, qint64> served = m_served[size_t(kind)];
    qint64 tick = m_tick;
    std::vector<quint64> out;
    for (size_t left = m_waiting.size(); left > 0; --left) {
        Lane* best = nullptr;
        qint64 bs = 0;
        for (Lane& x : lanes) {
            if (x.i >= x.l.size()) continue;
            const Waiter* h = x.l[x.i];
            const qint64 s = served.value(x.session, -1);
            if (!best) {
                best = &x;
                bs = s;
                continue;
            }
            const Waiter* b = best->l[best->i];
            if (h->meta.priority > b->meta.priority ||
                (h->meta.priority == b->meta.priority && (s < bs || (s == bs && h->id < b->id)))) {
                best = &x;
                bs = s;
            }
        }
        if (!best) break;
        out.push_back(best->l[best->i++]->id);
        served.insert(best->session, ++tick);
    }
    cache.ver = m_ver;
    cache.ids = std::move(out);
    return cache.ids;
}

qint64 RunQueue::holdLimit(const Slot& s) const {
    const qint64 v = s.meta.maxHoldMs >= 0 ? s.meta.maxHoldMs : m_o.maxHoldMs;
    return v > 0 ? v : 0;                                // 0 or negative: no watchdog
}

void RunQueue::arm(Slot& s, qint64 ms) {
    if (ms <= 0) return;
    const quint64 id = s.id;
    s.timer = m_clock.callAfter(std::min(ms, kMaxTimerMs), this, [this, id] {
        auto it = std::find_if(m_slots.begin(), m_slots.end(), [&](const Slot& x) { return x.id == id; });
        if (it != m_slots.end()) it->timer = 0;
        sweep();
    });
}

void RunQueue::start(quint64 id) {
    auto it = findWaiter(id);
    if (it == m_waiting.end()) return;
    Waiter w = std::move(*it);
    m_waiting.erase(it);
    ++m_ver;
    w.signal.removeListener(w.listener);
    m_served[size_t(w.kind)].insert(w.meta.session, ++m_tick);
    Slot s;
    s.id = w.id;
    s.kind = w.kind;
    s.meta = w.meta;
    s.sinceSteady = m_clock.steadyMs();
    s.sinceWall = m_clock.nowMs();
    m_slots.push_back(std::move(s));
    arm(m_slots.back(), holdLimit(m_slots.back()));
    const qint64 waited = std::max<qint64>(0, m_slots.back().sinceSteady - w.sinceSteady);
    KindMetrics& mk = w.kind == SlotKind::Heavy ? m_m.heavy : m_m.light;
    ++m_m.acquired;
    ++mk.acquired;
    m_m.waitMsTotal += waited;
    mk.waitMsTotal += waited;
    m_m.waitMsMax = std::max(m_m.waitMsMax, waited);
    mk.waitMsMax = std::max(mk.waitMsMax, waited);
    int& peak = w.kind == SlotKind::Heavy ? m_m.peakHeavy : m_m.peakLight;
    peak = std::max(peak, runningCount(w.kind));
    m_granted.emplace_back(w.id, std::move(w.granted));
}

int RunQueue::sweep() {
    const qint64 t = m_clock.steadyMs();
    std::vector<quint64> ids;
    for (const Slot& s : m_slots) ids.push_back(s.id);
    int n = 0;
    for (quint64 id : ids) {                             // by id: onWarning may call back into the queue
        auto it = std::find_if(m_slots.begin(), m_slots.end(), [&](const Slot& x) { return x.id == id; });
        if (it == m_slots.end()) continue;
        const qint64 limit = holdLimit(*it);
        if (limit <= 0) continue;
        const qint64 held = t - it->sinceSteady;
        if (held < limit) {
            if (!it->timer) arm(*it, limit - held);      // the timer fired early (a moved clock): look again later
            continue;
        }
        if (it->timer) m_clock.cancel(it->timer);
        SlotInfo info{it->id, it->kind, it->meta, it->sinceWall, held};
        m_slots.erase(it);
        ++m_m.expired;
        ++m_m.released;
        ++n;
        const qint64 heldS = roundSeconds(held);
        const QString what = !info.meta.label.isEmpty() ? info.meta.label
                             : !info.meta.phase.isEmpty() ? info.meta.phase : QStringLiteral("a job");
        const QString msg = QStringLiteral("\"%1\" held a %2 slot for %3 min without finishing; the slot was taken "
                                           "back so other work can run")
                                .arg(what, slotKindName(info.kind), QString::number(std::llround(double(heldS) / 60.0)));
        log::warn(kCat, msg);
        if (m_o.onWarning) {
            try {
                m_o.onWarning(msg, info);
            } catch (...) {                              // a broken listener must not break the queue
            }
        }
    }
    if (n) change();
    return n;
}

void RunQueue::fill() {
    for (SlotKind kind : {SlotKind::Heavy, SlotKind::Light}) {
        const int limit = m_limits.of(kind);
        while (limit == kUnlimited || runningCount(kind) < limit) {
            const Waiter* w = pick(kind);
            if (!w) break;
            start(w->id);
        }
    }
}

void RunQueue::notifyWaiters() {
    if (std::none_of(m_waiting.begin(), m_waiting.end(), [](const Waiter& w) { return bool(w.onWait); })) return;
    for (SlotKind kind : {SlotKind::Heavy, SlotKind::Light}) {
        const std::vector<quint64> ids = order(kind);    // a copy: onWait may change the list
        for (size_t i = 0; i < ids.size(); ++i) {
            auto it = findWaiter(ids[i]);
            if (it == m_waiting.end() || !it->onWait || it->lastAhead == int(i)) continue;
            it->lastAhead = int(i);
            try {
                it->onWait(int(i));
            } catch (...) {
            }
        }
    }
}

void RunQueue::change() {
    if (m_pumping) {
        m_again = true;
        return;
    }
    m_pumping = true;
    QPointer<RunQueue> self(this);
    do {
        m_again = false;
        fill();
        notifyWaiters();
        // the granted callbacks (the TS promise resolutions): after this round's bookkeeping, in grant order
        while (!m_granted.empty()) {
            auto [id, fn] = std::move(m_granted.front());
            m_granted.erase(m_granted.begin());
            if (fn) fn(id);
            if (!self) return;
        }
    } while (m_again);
    m_pumping = false;
    if (m_listeners.empty()) return;
    const auto listeners = m_listeners;
    for (const auto& [id, fn] : listeners) {
        try {
            fn();
        } catch (const std::exception& e) {              // a broken listener must not break the queue or the others
            log::warn(kCat, QStringLiteral("change listener threw: %1").arg(QString::fromUtf8(e.what())));
        } catch (...) {
            log::warn(kCat, QStringLiteral("change listener threw"));
        }
        if (!self) return;
    }
}

// ---------------- what is going on ----------------

QList<SlotInfo> RunQueue::running() const {
    const qint64 t = m_clock.steadyMs();
    QList<SlotInfo> out;
    for (const Slot& s : m_slots) out.append(SlotInfo{s.id, s.kind, s.meta, s.sinceWall, std::max<qint64>(0, t - s.sinceSteady)});
    return out;
}

QList<WaitInfo> RunQueue::queued() const {
    const qint64 t = m_clock.steadyMs();
    QList<WaitInfo> out;
    for (SlotKind kind : {SlotKind::Heavy, SlotKind::Light}) {
        const int limit = m_limits.of(kind);
        const bool full = limit != kUnlimited && runningCount(kind) >= limit;
        const std::vector<quint64>& ids = order(kind);
        for (size_t i = 0; i < ids.size(); ++i) {
            auto it = std::find_if(m_waiting.begin(), m_waiting.end(), [&](const Waiter& w) { return w.id == ids[i]; });
            if (it == m_waiting.end()) continue;
            WaitInfo w{it->id, kind, it->meta, it->sinceWall, std::max<qint64>(0, t - it->sinceSteady), int(i), {}};
            w.why = full ? QStringLiteral("pool") : !groupOk(it->meta) ? QStringLiteral("group") : QStringLiteral("next");
            out.append(std::move(w));
        }
    }
    return out;
}

QJsonObject RunQueue::status() const {
    QJsonArray running, queued;
    QJsonObject byThread, groups;
    auto thread = [&](const QString& session) {
        QJsonObject b = byThread.value(session).toObject();
        if (b.isEmpty())
            b = QJsonObject{{QStringLiteral("running"), 0}, {QStringLiteral("queued"), 0}, {QStringLiteral("label"), QString()}};
        return b;
    };
    auto limitJson = [](int limit) { return limit == 0 || limit == kUnlimited ? QJsonValue() : QJsonValue(limit); };
    for (const SlotInfo& s : this->running()) {
        running.append(s.toJson());
        QJsonObject b = thread(s.meta.session);
        b[QStringLiteral("running")] = b.value(QStringLiteral("running")).toInt() + 1;
        b[QStringLiteral("label")] = s.meta.label;
        byThread.insert(s.meta.session, b);
        if (!s.meta.group.isEmpty()) {
            QJsonObject g = groups.value(s.meta.group).toObject();
            g[QStringLiteral("running")] = g.value(QStringLiteral("running")).toInt() + 1;
            g[QStringLiteral("limit")] = limitJson(groupLimitOf(s.meta));
            groups.insert(s.meta.group, g);
        }
    }
    for (const WaitInfo& w : this->queued()) {
        queued.append(w.toJson());
        QJsonObject b = thread(w.meta.session);
        b[QStringLiteral("queued")] = b.value(QStringLiteral("queued")).toInt() + 1;
        if (b.value(QStringLiteral("running")).toInt() == 0) b[QStringLiteral("label")] = w.meta.label;
        byThread.insert(w.meta.session, b);
        if (!w.meta.group.isEmpty() && !groups.contains(w.meta.group))
            groups.insert(w.meta.group, QJsonObject{{QStringLiteral("running"), 0}, {QStringLiteral("limit"), limitJson(groupLimitOf(w.meta))}});
    }
    QJsonObject o;
    o.insert(QStringLiteral("running"), running);
    o.insert(QStringLiteral("queued"), queued);
    o.insert(QStringLiteral("byThread"), byThread);
    o.insert(QStringLiteral("limits"), QJsonObject{{QStringLiteral("heavy"), limitJson(m_limits.heavy)},
                                                   {QStringLiteral("light"), limitJson(m_limits.light)}});
    o.insert(QStringLiteral("groups"), groups);
    return o;
}

QueueMetrics RunQueue::metrics() const {
    QueueMetrics m = m_m;
    m.queuedNow = int(m_waiting.size());
    m.runningNow = int(m_slots.size());
    return m;
}

RunQueue::ListenerId RunQueue::onChange(std::function<void()> fn) {
    if (!fn) return 0;
    const ListenerId id = ++m_nextListener;
    m_listeners.emplace(id, std::move(fn));
    return id;
}

void RunQueue::removeChangeListener(ListenerId id) { m_listeners.erase(id); }

} // namespace studio::sched
