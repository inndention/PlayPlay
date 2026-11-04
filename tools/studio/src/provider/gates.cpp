#include "provider/gates.h"

#include "core/clock.h"
#include "provider/modelhealth.h"

#include <QJsonArray>
#include <QMetaObject>
#include <QRegularExpression>
#include <QThread>
#include <algorithm>
#include <climits>
#include <map>
#include <mutex>

namespace studio::provider {

namespace {
constexpr GateKind kAllKinds[] = {GateKind::Videos, GateKind::Images, GateKind::Audio, GateKind::Chat, GateKind::Poll};
}

QString gateName(GateKind k) {
    switch (k) {
    case GateKind::Videos: return QStringLiteral("videos");
    case GateKind::Images: return QStringLiteral("images");
    case GateKind::Audio: return QStringLiteral("audio");
    case GateKind::Chat: return QStringLiteral("chat");
    case GateKind::Poll: return QStringLiteral("poll");
    }
    return QStringLiteral("chat");
}

std::optional<GateKind> gateKindFromName(QStringView s) {
    for (GateKind k : kAllKinds)
        if (s == gateName(k)) return k;
    return std::nullopt;
}

bool isHeavy(GateKind k) { return k == GateKind::Videos || k == GateKind::Images || k == GateKind::Audio; }

std::optional<GateKind> gateFor(QStringView method, QStringView pathOrUrl) {
    const QString m = method.isEmpty() ? QStringLiteral("GET") : method.toString().toUpper();
    if (m == QLatin1String("GET") || m == QLatin1String("HEAD")) return GateKind::Poll;
    if (m != QLatin1String("POST")) return std::nullopt;
    // p = path.split('?')[0].replace(/^https?:\/\/[^/]+(?:\/api\/v1)?/, '')
    static const QRegularExpression origin(QStringLiteral("^https?://[^/]+(?:/api/v1)?"));
    QString p = pathOrUrl.toString().section(QLatin1Char('?'), 0, 0);
    p.remove(origin);
    if (p == QLatin1String("/videos")) return GateKind::Videos;
    if (p == QLatin1String("/images")) return GateKind::Images;
    if (p.startsWith(QLatin1String("/audio/"))) return GateKind::Audio;
    if (p == QLatin1String("/chat/completions")) return GateKind::Chat;
    return std::nullopt;
}

QString providerKeyOf(QStringView model) {
    const qsizetype slash = model.indexOf(QLatin1Char('/'));
    if (slash <= 0 || slash > 64) return {};
    const QString key = model.left(slash).toString().trimmed().toLower();
    for (QChar c : key) {
        const char16_t u = c.unicode();
        if (!((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-')) return {};
    }
    return key;
}

// ---------------------------------------------------------------------------------------------------------------

namespace detail {

struct AbortState {
    std::mutex mutex;
    bool aborted = false;
    QString reason;
    quint64 nextId = 0;
    std::map<quint64, std::function<void()>> listeners;
};

/// A weak handle on a Gate that any thread may hold: ~Gate clears it under the mutex, so a post to a live gate is
/// never a post to a dead one (and Qt drops a post whose receiver dies later).
struct GateLink {
    std::mutex mutex;
    Gate* gate = nullptr;
};

} // namespace detail

namespace {

// Runs fn(gate) on the gate's thread: at once when we are on it, else queued. Nothing when the gate is gone.
template <typename F>
void onGateThread(const std::shared_ptr<detail::GateLink>& link, F fn) {
    if (!link) return;
    std::unique_lock lock(link->mutex);
    Gate* g = link->gate;
    if (!g) return;
    if (g->thread() == QThread::currentThread()) {
        lock.unlock();                                   // on its own thread a gate cannot die under us
        fn(g);
        return;
    }
    QMetaObject::invokeMethod(g, [link, fn]() mutable {
        Gate* gate = nullptr;
        { std::lock_guard l(link->mutex); gate = link->gate; }
        if (gate) fn(gate);
    }, Qt::QueuedConnection);
}

Gate* gateHere(const std::shared_ptr<detail::GateLink>& link) {
    if (!link) return nullptr;
    std::lock_guard lock(link->mutex);
    return link->gate && link->gate->thread() == QThread::currentThread() ? link->gate : nullptr;
}

} // namespace

bool AbortSignal::aborted() const {
    if (!m_state) return false;
    std::lock_guard lock(m_state->mutex);
    return m_state->aborted;
}

QString AbortSignal::reason() const {
    if (!m_state) return {};
    std::lock_guard lock(m_state->mutex);
    return m_state->reason;
}

AbortSignal::ListenerId AbortSignal::onAbort(std::function<void()> fn) const {
    if (!m_state || !fn) return 0;
    std::lock_guard lock(m_state->mutex);
    if (m_state->aborted) return 0;
    const ListenerId id = ++m_state->nextId;
    m_state->listeners.emplace(id, std::move(fn));
    return id;
}

void AbortSignal::removeListener(ListenerId id) const {
    if (!m_state || !id) return;
    std::lock_guard lock(m_state->mutex);
    m_state->listeners.erase(id);
}

AbortController::AbortController() { m_signal.m_state = std::make_shared<detail::AbortState>(); }

AbortSignal AbortController::signal() const { return m_signal; }

void AbortController::abort(const QString& reason) {
    std::map<quint64, std::function<void()>> fns;
    {
        std::lock_guard lock(m_signal.m_state->mutex);
        if (m_signal.m_state->aborted) return;
        m_signal.m_state->aborted = true;
        m_signal.m_state->reason = reason.isEmpty() ? QStringLiteral("Cancelled") : reason;
        fns.swap(m_signal.m_state->listeners);
    }
    for (auto& [id, fn] : fns) fn();
}

// ---------------------------------------------------------------------------------------------------------------

Slot::Slot(Slot&& o) noexcept
    : m_holds(std::move(o.m_holds)), m_kind(std::move(o.m_kind)), m_sub(std::move(o.m_sub)) {
    o.m_holds.clear();
}

Slot& Slot::operator=(Slot&& o) noexcept {
    if (this != &o) {
        release();
        m_holds = std::move(o.m_holds);
        m_kind = std::move(o.m_kind);
        m_sub = std::move(o.m_sub);
        o.m_holds.clear();
    }
    return *this;
}

Slot::~Slot() { release(); }

void Slot::release() {
    std::vector<Hold> holds;
    holds.swap(m_holds);
    // the heavy cap first, the provider sub-gate last: the reverse of the order they were taken in
    for (auto it = holds.rbegin(); it != holds.rend(); ++it) {
        const quint64 id = it->id;
        onGateThread(it->link, [id](Gate* g) { g->releaseHold(id); });
    }
    m_kind.reset();
    m_sub.reset();
}

void Slot::merge(Slot&& other) {
    for (Hold& h : other.m_holds) m_holds.push_back(std::move(h));
    other.m_holds.clear();
    if (other.m_kind) m_kind = std::move(other.m_kind);
    if (other.m_sub) m_sub = std::move(other.m_sub);
}

void Slot::onSuccess() {
    for (const auto& link : {m_sub, m_kind}) {
        onGateThread(link, [](Gate* g) {
            if (g->adapt()) g->adapt()->onSuccess();
            g->drain();
        });
    }
}

QString Slot::feedbackGate(LimitScope scope, const LimitSignal& sig) const {
    const std::shared_ptr<detail::GateLink>& target =
        scope == LimitScope::Gate ? m_kind
        : scope == LimitScope::Provider ? (m_sub ? m_sub : m_kind)
        : (m_sub && !sig.provider.isEmpty() ? m_sub : m_kind);
    if (!target) return {};
    std::lock_guard lock(target->mutex);
    return target->gate ? target->gate->name() : QString();
}

AdaptiveLimit::LimitResult Slot::onLimit(const LimitSignal& sig, LimitScope scope) {
    const std::shared_ptr<detail::GateLink> target =
        scope == LimitScope::Gate ? m_kind
        : scope == LimitScope::Provider ? (m_sub ? m_sub : m_kind)
        : (m_sub && !sig.provider.isEmpty() ? m_sub : m_kind);
    if (!target) return {};
    if (Gate* g = gateHere(target)) {
        AdaptiveLimit::LimitResult r;
        if (g->adapt()) r = g->adapt()->onLimit(sig);
        g->drain();                                      // arms the wake-up for anyone already waiting
        return r;
    }
    onGateThread(target, [sig](Gate* g) {
        if (g->adapt()) g->adapt()->onLimit(sig);
        g->drain();
    });
    return {};
}

// ---------------------------------------------------------------------------------------------------------------

Gate::Gate(QString name, std::function<int()> configured, Clock& clock, bool adaptive, QObject* parent)
    : QObject(parent), m_name(std::move(name)), m_configured(std::move(configured)), m_clock(clock),
      m_link(std::make_shared<detail::GateLink>()) {
    m_link->gate = this;
    if (adaptive) m_adapt = std::make_unique<AdaptiveLimit>(m_name, [this] { return this->configured(); }, m_clock);
}

Gate::~Gate() {
    { std::lock_guard lock(m_link->mutex); m_link->gate = nullptr; }
    if (m_timer) m_clock.cancel(m_timer);
    std::deque<Waiter> line;
    line.swap(m_line);
    for (Waiter& w : line) {
        w.signal.removeListener(w.listener);
        if (w.refused) w.refused(QStringLiteral("the %1 gate was closed").arg(m_name));
    }
}

int Gate::configured() const { return m_configured ? std::max(1, m_configured()) : 1; }

int Gate::limit() { return m_adapt ? m_adapt->limit() : configured(); }

quint64 Gate::acquire(Granted granted, Refused refused, const AbortSignal& signal) {
    if (signal.aborted()) {
        if (refused) refused(signal.reason());
        return 0;
    }
    if (m_running < limit() && m_line.empty() && !paused()) {
        Waiter w{0, std::move(granted), std::move(refused), signal, 0};
        grant(w);
        return 0;
    }
    const quint64 ticket = ++m_nextId;
    AbortSignal::ListenerId listener = 0;
    if (signal.valid()) {
        // a Stop while waiting takes the request out of the line at once (from whatever thread pressed it)
        listener = signal.onAbort([link = m_link, ticket] {
            onGateThread(link, [ticket](Gate* g) {
                auto it = std::find_if(g->m_line.begin(), g->m_line.end(), [&](const Waiter& w) { return w.ticket == ticket; });
                if (it != g->m_line.end()) g->cancel(ticket, it->signal.reason());
            });
        });
    }
    m_line.push_back(Waiter{ticket, std::move(granted), std::move(refused), signal, listener});
    drain();                                             // nothing can start now; this arms the wake-up
    return ticket;
}

std::optional<Slot> Gate::tryAcquire() {
    if (m_running < limit() && m_line.empty() && !paused()) return makeSlot();
    return std::nullopt;
}

bool Gate::cancel(quint64 ticket, const QString& why) {
    auto it = std::find_if(m_line.begin(), m_line.end(), [&](const Waiter& w) { return w.ticket == ticket; });
    if (it == m_line.end()) return false;
    Waiter w = std::move(*it);
    m_line.erase(it);
    w.signal.removeListener(w.listener);
    if (w.refused) w.refused(why.isEmpty() ? QStringLiteral("Cancelled") : why);
    drain();
    return true;
}

Slot Gate::makeSlot() {
    const quint64 id = ++m_nextId;
    m_live.insert(id);
    ++m_running;
    ++m_served;
    m_peak = std::max(m_peak, m_running);
    Slot s;
    s.m_holds.push_back(Slot::Hold{m_link, id});
    if (m_adapt) s.m_kind = m_link;
    return s;
}

void Gate::grant(Waiter& w) {
    w.signal.removeListener(w.listener);
    Slot s = makeSlot();
    if (w.granted) w.granted(std::move(s));             // a caller without a callback just drops (releases) it
}

void Gate::releaseHold(quint64 id) {
    if (m_live.erase(id) == 0) return;                   // released once only
    --m_running;
    drain();
}

void Gate::drain() {
    if (m_draining) { m_drainAgain = true; return; }     // a grant callback released or acquired: loop again
    m_draining = true;
    do {
        m_drainAgain = false;
        while (!m_line.empty() && m_running < limit() && !paused()) {
            Waiter w = std::move(m_line.front());
            m_line.pop_front();
            grant(w);
        }
    } while (m_drainAgain);
    m_draining = false;
    // somebody is still waiting: wake the line when the pause ends, or when the slow-down gives a slot back by time
    if (!m_line.empty() && m_adapt) {
        const std::optional<qint64> wait = m_adapt->paused() ? std::optional<qint64>(m_adapt->pausedForMs())
                                                             : m_adapt->nextStepMs();
        if (wait) armTimer(*wait);
    }
}

// wake the line in `ms`; an earlier wake-up replaces a later one (a cleared pause must not leave a long timer behind)
void Gate::armTimer(qint64 ms) {
    const qint64 at = m_clock.steadyMs() + std::max<qint64>(1, ms) + 5;
    if (m_timer && m_timerAt <= at) return;
    if (m_timer) m_clock.cancel(m_timer);
    m_timerAt = at;
    m_timer = m_clock.callAfter(at - m_clock.steadyMs(), this, [this] {
        m_timer = 0;
        drain();
    });
}

void Gate::resetStats() {
    m_peak = m_running;
    m_served = 0;
}

QJsonObject Gate::statusJson() {
    QJsonObject o{{QStringLiteral("running"), m_running},
                  {QStringLiteral("queued"), queued()},
                  {QStringLiteral("peak"), m_peak},
                  {QStringLiteral("served"), double(m_served)}};
    if (m_adapt) {
        const AdaptiveStatus a = m_adapt->status();
        const QJsonObject s = a.toJson();
        for (auto it = s.begin(); it != s.end(); ++it)
            if (it.key() != QLatin1String("name") && it.key() != QLatin1String("effective")) o.insert(it.key(), it.value());
        o.insert(QStringLiteral("limit"), a.effective);
    } else {
        const int c = configured();
        o.insert(QStringLiteral("limit"), c);
        o.insert(QStringLiteral("configured"), c);
        o.insert(QStringLiteral("limited"), false);
        o.insert(QStringLiteral("paused"), false);
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------------------

GateLimits GateLimits::clamped(QStringList* warnings) const {
    GateLimits c = *this;
    auto clamp = [&](int& v, int lo, int hi, const QString& key) {
        const int n = std::clamp(v, lo, hi);
        if (n != v && warnings) *warnings << QStringLiteral("%1: %2 is outside %3..%4, using %5").arg(key).arg(v).arg(lo).arg(hi).arg(n);
        v = n;
    };
    for (GateKind k : kAllKinds) {
        clamp(c.width[size_t(k)], minWidth(k), maxWidth(k), QStringLiteral("gates.%1").arg(gateName(k)));
        clamp(c.perProvider[size_t(k)], 0, maxWidth(k), QStringLiteral("gates.perProvider.%1").arg(gateName(k)));
    }
    clamp(c.heavyCap, 0, 256, QStringLiteral("gates.heavyCap"));
    return c;
}

QJsonObject GateLimits::toJson() const {
    QJsonObject w, p;
    for (GateKind k : kAllKinds) {
        w.insert(gateName(k), width[size_t(k)]);
        p.insert(gateName(k), perProvider[size_t(k)]);
    }
    return {{QStringLiteral("gates"), w}, {QStringLiteral("perProvider"), p}, {QStringLiteral("heavyCap"), heavyCap}};
}

struct Gates::Pending {
    Slot slot;
    std::vector<Gate*> stages;
    size_t next = 0;
    std::shared_ptr<detail::GateLink> kindLink, subLink;
    QPointer<QObject> context;
    bool hadContext = false;
    Gate::Granted granted;
    Gate::Refused refused;
    AbortSignal signal;
};

Gates::Gates(Clock& clock, QObject* parent) : QObject(parent), m_clock(clock), m_breaker(clock) {
    for (GateKind k : kAllKinds)
        m_gates[size_t(k)] = new Gate(gateName(k), [this, k] { return m_limits.width[size_t(k)]; }, m_clock, true, this);
    m_heavy = new Gate(QStringLiteral("heavy"), [this] { return m_limits.heavyCap > 0 ? m_limits.heavyCap : INT_MAX; },
                       m_clock, false, this);
}

Gates::~Gates() {
    // delete the gates while the limits they read are still alive (QObject would do it after this body)
    for (auto& subs : m_subs) {
        for (Gate* g : subs) delete g;
        subs.clear();
    }
    for (Gate*& g : m_gates) { delete g; g = nullptr; }
    delete m_heavy;
    m_heavy = nullptr;
}

QStringList Gates::configure(const GateLimits& limits) {
    QStringList warnings;
    const GateLimits c = limits.clamped(&warnings);
    for (GateKind k : kAllKinds) {
        const size_t i = size_t(k);
        // only a real change starts the gate over: re-applying the same limits must not wipe a slow-down in progress
        if (c.width[i] != m_limits.width[i]) {
            m_limits.width[i] = c.width[i];
            m_gates[i]->adapt()->reset();
        }
        if (c.perProvider[i] != m_limits.perProvider[i]) {
            m_limits.perProvider[i] = c.perProvider[i];
            for (Gate* g : m_subs[i]) g->adapt()->reset();
        }
    }
    m_limits.heavyCap = c.heavyCap;
    for (Gate* g : m_gates) g->drain();
    for (auto& subs : m_subs)
        for (Gate* g : subs) g->drain();
    m_heavy->drain();
    return warnings;
}

void Gates::tune(const AdaptiveTuning& t) {
    m_tuning = t;
    for (Gate* g : m_gates) g->adapt()->tune(t);
    for (auto& subs : m_subs)
        for (Gate* g : subs) g->adapt()->tune(t);
}

Gate* Gates::subGate(GateKind kind, const QString& key) const { return m_subs[size_t(kind)].value(key, nullptr); }

Gate* Gates::subFor(GateKind kind, const QString& key) {
    const size_t i = size_t(kind);
    if (key.isEmpty() || m_limits.perProvider[i] <= 0) return nullptr;
    if (Gate* g = m_subs[i].value(key, nullptr)) return g;
    if (m_subs[i].size() >= kMaxSubGates) {
        for (auto it = m_subs[i].begin(); it != m_subs[i].end();) {
            Gate* g = it.value();
            if (g->running() == 0 && g->queued() == 0 && !g->adapt()->status().limited) {
                it = m_subs[i].erase(it);
                delete g;
            } else {
                ++it;
            }
        }
        if (m_subs[i].size() >= kMaxSubGates) return nullptr;
    }
    auto* g = new Gate(gateName(kind) + QLatin1Char(':') + key, [this, i] { return m_limits.perProvider[i]; },
                       m_clock, true, this);
    g->adapt()->tune(m_tuning);
    m_subs[i].insert(key, g);
    return g;
}

void Gates::acquire(const Request& rq, QObject* context, Gate::Granted granted, Gate::Refused refused) {
    auto p = std::make_shared<Pending>();
    p->context = context;
    p->hadContext = context != nullptr;
    p->granted = std::move(granted);
    p->refused = std::move(refused);
    p->signal = rq.signal;
    Gate* sub = subFor(rq.kind, rq.providerKey);
    Gate* kind = m_gates[size_t(rq.kind)];
    if (sub) p->stages.push_back(sub);
    p->stages.push_back(kind);
    // a slot in the request's own gate and, for generation calls through the proxy (not ownOnly), in the heavy pool
    if (isHeavy(rq.kind) && m_limits.heavyCap > 0 && !rq.ownOnly) p->stages.push_back(m_heavy);
    p->kindLink = kind->m_link;
    p->subLink = sub ? sub->m_link : nullptr;
    step(p);
}

void Gates::step(const std::shared_ptr<Pending>& p) {
    if (p->next == p->stages.size()) { finish(p, true, {}); return; }
    Gate* g = p->stages[p->next++];
    g->acquire([this, p](Slot s) {
                   p->slot.merge(std::move(s));
                   step(p);
               },
               [this, p](const QString& why) {
                   p->slot.release();                    // what was already held goes back
                   finish(p, false, why);
               },
               p->signal);
}

void Gates::finish(const std::shared_ptr<Pending>& p, bool ok, const QString& why) {
    if (ok) {
        p->slot.m_kind = p->kindLink;
        p->slot.m_sub = p->subLink;
    }
    QObject* target = p->hadContext ? p->context.data() : this;
    if (!target) { p->slot.release(); return; }        // the caller is gone: nothing to tell, nothing kept
    // delivered like a resolved promise: never inside acquire(), on the caller's thread. If the target dies first, Qt
    // drops this call and with it the last reference to p, whose Slot then goes back.
    QMetaObject::invokeMethod(target, [p, ok, why] {
        if (ok && p->signal.aborted()) {
            p->slot.release();
            if (p->refused) p->refused(p->signal.reason());
            return;
        }
        if (ok) {
            if (p->granted) p->granted(std::move(p->slot));
            else p->slot.release();
        } else if (p->refused) {
            p->refused(why);
        }
    }, Qt::QueuedConnection);
}

void Gates::reset() {
    for (Gate* g : m_gates) { g->adapt()->reset(); g->adapt()->resetStats(); g->drain(); }
    for (auto& subs : m_subs)
        for (Gate* g : subs) { g->adapt()->reset(); g->adapt()->resetStats(); g->drain(); }
    m_breaker.clear();
    if (m_health) m_health->clear();
}

void Gates::clearQuota(const QString& model) {
    m_breaker.clear(model);
    if (m_health) m_health->clear(model);
}

void Gates::resetStats() {
    for (Gate* g : m_gates) g->resetStats();
    for (auto& subs : m_subs)
        for (Gate* g : subs) g->resetStats();
    m_heavy->resetStats();
}

QJsonObject Gates::gateStatus() {
    QJsonObject out;
    for (GateKind k : kAllKinds) {
        QJsonObject o = gate(k).statusJson();
        const auto& subs = m_subs[size_t(k)];
        if (!subs.isEmpty()) {
            QJsonObject providers;
            for (auto it = subs.begin(); it != subs.end(); ++it) providers.insert(it.key(), it.value()->statusJson());
            o.insert(QStringLiteral("providers"), providers);
        }
        out.insert(gateName(k), o);
    }
    return out;
}

QJsonObject Gates::backpressureStatus() {
    const QJsonObject g = gateStatus();
    QJsonArray quota;
    for (const auto& e : m_breaker.list()) quota.append(e.toJson());
    bool limited = !quota.isEmpty();
    for (auto it = g.begin(); it != g.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        if (o.value(QLatin1String("limited")).toBool()) limited = true;
        const QJsonObject providers = o.value(QLatin1String("providers")).toObject();
        for (auto p = providers.begin(); p != providers.end(); ++p)
            if (p.value().toObject().value(QLatin1String("limited")).toBool()) limited = true;
    }
    const QJsonObject heavy{{QStringLiteral("cap"), m_limits.heavyCap},
                            {QStringLiteral("running"), m_heavy->running()},
                            {QStringLiteral("queued"), m_heavy->queued()},
                            {QStringLiteral("peak"), m_heavy->peak()}};
    return {{QStringLiteral("limited"), limited},
            {QStringLiteral("gates"), g},
            {QStringLiteral("heavy"), heavy},
            {QStringLiteral("quota"), quota}};
}

} // namespace studio::provider
