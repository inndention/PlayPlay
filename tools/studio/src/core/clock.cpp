#include "core/clock.h"

#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <vector>

namespace studio {

QString Clock::isoUtc(qint64 epochMs) {
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzzZ"));
}

QString Clock::localDateOf(qint64 epochMs) {
    return QDateTime::fromMSecsSinceEpoch(epochMs).toLocalTime().date().toString(QStringLiteral("yyyy-MM-dd"));
}

Clock& Clock::system() {
    // Leaked on purpose: timers and log threads may still ask for the time while static destructors run.
    static SystemClock* const s = new SystemClock;
    return *s;
}

// ---------------- SystemClock ----------------

struct SystemClock::State {
    std::mutex mutex;
    TimerId next = 0;
    std::map<TimerId, QPointer<QTimer>> timers;
    QElapsedTimer steady;
};

SystemClock::SystemClock() : m_state(std::make_shared<State>()) { m_state->steady.start(); }

SystemClock::~SystemClock() {
    // Pending timers are children of their contexts; they keep a weak reference to the state and simply stop mattering.
    std::lock_guard<std::mutex> g(m_state->mutex);
    m_state->timers.clear();
}

qint64 SystemClock::nowMs() const { return QDateTime::currentMSecsSinceEpoch(); }

qint64 SystemClock::steadyMs() const { return m_state->steady.elapsed(); }

Clock::TimerId SystemClock::callAfter(qint64 ms, QObject* context, std::function<void()> fn) {
    Q_ASSERT(context);
    Q_ASSERT_X(context->thread() == QThread::currentThread(), "SystemClock::callAfter", "call from the context's thread");
    auto* t = new QTimer(context);         // dies with the context: a timer of a destroyed owner never fires
    t->setSingleShot(true);
    t->setTimerType(Qt::PreciseTimer);
    TimerId id;
    {
        std::lock_guard<std::mutex> g(m_state->mutex);
        id = ++m_state->next;
        m_state->timers[id] = t;
    }
    const std::weak_ptr<State> weak = m_state;
    QObject::connect(t, &QTimer::timeout, t, [weak, id, t, fn = std::move(fn)]() {
        if (auto st = weak.lock()) {
            std::lock_guard<std::mutex> g(st->mutex);
            st->timers.erase(id);
        }
        t->deleteLater();
        fn();
    });
    QObject::connect(t, &QObject::destroyed, [weak, id]() {   // context destroyed first: forget the entry
        if (auto st = weak.lock()) {
            std::lock_guard<std::mutex> g(st->mutex);
            st->timers.erase(id);
        }
    });
    t->start(int(std::clamp<qint64>(ms, 0, INT_MAX)));       // > 24.8 days is clamped (QTimer takes int ms)
    return id;
}

bool SystemClock::cancel(TimerId id) {
    QPointer<QTimer> t;
    {
        std::lock_guard<std::mutex> g(m_state->mutex);
        auto it = m_state->timers.find(id);
        if (it == m_state->timers.end()) return false;
        t = it->second;
        m_state->timers.erase(it);
    }
    if (!t) return false;
    t->stop();
    t->deleteLater();
    return true;
}

// ---------------- FakeClock ----------------

FakeClock::FakeClock(qint64 startMs) : m_wallOffset(startMs) {}

qint64 FakeClock::nowMs() const { std::lock_guard<std::mutex> g(m_mutex); return m_steady + m_wallOffset; }

qint64 FakeClock::steadyMs() const { std::lock_guard<std::mutex> g(m_mutex); return m_steady; }

Clock::TimerId FakeClock::callAfter(qint64 ms, QObject* context, std::function<void()> fn) {
    std::lock_guard<std::mutex> g(m_mutex);
    const TimerId id = ++m_nextId;
    const qint64 due = m_steady + std::max<qint64>(0, ms);
    m_queue.emplace(Key{due, id}, Timer{context, context != nullptr, std::move(fn)});
    m_due.emplace(id, due);
    return id;
}

bool FakeClock::cancel(TimerId id) {
    std::lock_guard<std::mutex> g(m_mutex);
    auto it = m_due.find(id);
    if (it == m_due.end()) return false;
    m_queue.erase(Key{it->second, id});
    m_due.erase(it);
    return true;
}

int FakeClock::advance(qint64 ms) {
    qint64 target;
    {
        std::lock_guard<std::mutex> g(m_mutex);
        target = m_steady + std::max<qint64>(0, ms);
    }
    int ran = 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard<std::mutex> g(m_mutex);
            auto first = m_queue.begin();
            if (first == m_queue.end() || first->first.first > target) { m_steady = target; break; }
            m_steady = std::max(m_steady, first->first.first);
            const bool dead = first->second.hadContext && first->second.context.isNull();
            if (!dead) fn = std::move(first->second.fn);
            m_due.erase(first->first.second);
            m_queue.erase(first);
        }
        if (fn) { fn(); ++ran; }          // outside the lock: a callback may set or cancel timers
    }
    return ran;
}

void FakeClock::setNowMs(qint64 ms) {
    std::lock_guard<std::mutex> g(m_mutex);
    m_wallOffset = ms - m_steady;
}

int FakeClock::pendingTimers() const {
    std::lock_guard<std::mutex> g(m_mutex);
    return int(m_queue.size());
}

qint64 FakeClock::nextDueSteadyMs() const {
    std::lock_guard<std::mutex> g(m_mutex);
    return m_queue.empty() ? -1 : m_queue.begin()->first.first;
}

} // namespace studio
