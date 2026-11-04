#pragma once
// Request gates (ST-06): how many provider requests of each kind are on the wire at once. A port of the Gate of
// starmedia src/openrouter.ts (L72-204) with the server-side additions of design §4.3:
//
//   videos / images / audio / chat   the four TS gates: POSTs that start model work (gateFor), each with its AIMD
//                                    controller (AdaptiveLimit): a rate answer halves the gate once per episode and
//                                    pauses it for Retry-After, successes and quiet time give slots back.
//   poll                             new: GETs (video status polls, content downloads, catalog reads) that TS leaves
//                                    ungated. Light and wide, with its own AIMD, so a 429 on polling slows polling
//                                    without touching the generation gates.
//   heavy                            the shared cap across videos + images + audio (TS heavyPool; 0 = none). No AIMD:
//                                    its size is the server's, not ours to adapt.
//   per-provider sub-gates           new, optional (GateLimits::perProvider, 0 = off, the TS behaviour): one gate per
//                                    upstream provider key (the model prefix, providerKeyOf) in front of the kind gate,
//                                    each with its own AIMD, so one provider's 429 throttles that provider only. A
//                                    request waiting on a throttled provider holds no slot of the kind gate.
//   QuotaBreaker                     "this model's quota is used up": calls fail at once until it lapses.
//
// The rules the TS states and this port keeps:
//   - a request holds its slot only while it is on the wire (the request layer releases it when the answer has been
//     read, and never holds it while it sleeps between retries, so a retry storm cannot deadlock the line);
//   - FIFO: nobody overtakes a waiter (a new request goes out at once only when the line is empty), so the line is fair;
//   - an AbortSignal takes a waiting request out of the line at once, and it never reaches the provider;
//   - while paused nothing new goes out; a timer wakes the line when the pause ends or when the slow-down gives a slot
//     back by time alone (even if nothing finishes);
//   - configure(): only a real change of a width starts that gate over; re-applying the same limits keeps a slow-down.
//
// Threads: a Gate / Gates lives on one thread (the Studio's main loop) and is used from it. Slot::release() and an
// AbortController::abort() may come from any thread: they hop to the gate's thread. Gate callbacks run on the gate's
// thread; Gates::acquire() delivers to the caller's context object (queued, like a resolved promise).
#include "provider/backpressure.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

namespace studio {
class Clock;
}

namespace studio::provider {

class ModelHealth;

enum class GateKind { Videos, Images, Audio, Chat, Poll };
constexpr int kGateKindCount = 5;
QString gateName(GateKind k);                            // "videos" | "images" | "audio" | "chat" | "poll"
std::optional<GateKind> gateKindFromName(QStringView s);
/// The gate a request goes through. POST /videos, /images, /audio/*, /chat/completions → their gate (TS gateFor; the
/// path may be a full URL, with or without /api/v1, and a query); GET / HEAD of anything → Poll; any other request (an
/// embeddings or rerank POST, as in TS) → none.
std::optional<GateKind> gateFor(QStringView method, QStringView pathOrUrl);
/// True for the kinds that share the heavy cap (videos, images, audio).
bool isHeavy(GateKind k);
/// The sub-gate key of a model id: its prefix before '/', lower case ("google/veo-3.1" → "google"); empty when the id
/// has no prefix or the prefix is not a plain name ([a-z0-9._-], at most 64 chars).
QString providerKeyOf(QStringView model);

// ---------------------------------------------------------------------------------------------------------------

namespace detail {
struct AbortState;
struct GateLink;
} // namespace detail

/// A Stop button for one or more requests (AbortController / AbortSignal). A default-constructed signal never fires.
class AbortSignal {
public:
    using ListenerId = quint64;
    AbortSignal() = default;
    bool valid() const { return bool(m_state); }
    bool aborted() const;
    QString reason() const;
    /// Registers `fn` to run once when abort() is called (on the thread that calls it). Returns 0 and does not
    /// register when the signal is invalid or already aborted (check aborted() first).
    ListenerId onAbort(std::function<void()> fn) const;
    void removeListener(ListenerId id) const;

private:
    friend class AbortController;
    std::shared_ptr<detail::AbortState> m_state;
};

class AbortController {
public:
    AbortController();
    AbortSignal signal() const;
    /// Fires the signal once (later calls do nothing). Thread-safe; listeners run on the calling thread.
    void abort(const QString& reason = QStringLiteral("Cancelled"));
    bool aborted() const { return signal().aborted(); }

private:
    AbortSignal m_signal;
};

// ---------------------------------------------------------------------------------------------------------------

/// Where a limit answer is booked (Slot::onLimit).
enum class LimitScope {
    Auto,       // the provider sub-gate when the request has one and the signal names an upstream provider, else the kind gate
    Provider,   // the provider sub-gate (the kind gate when the request has none)
    Gate,       // the kind gate (OpenRouter itself limited the key: every provider is affected)
};

class Gate;

/// One request's place on the wire: the slots it holds (sub-gate, kind gate, heavy). Move-only; destroying it or
/// release() gives every slot back exactly once (from any thread). onLimit / onSuccess feed the AIMD controllers of
/// the request's gates; call them on the gates' thread before release().
class Slot {
public:
    Slot() = default;
    Slot(Slot&& o) noexcept;
    Slot& operator=(Slot&& o) noexcept;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot();

    bool valid() const { return !m_holds.empty(); }
    void release();
    /// The request came back fine (the AIMD windows count it).
    void onSuccess();
    /// The request got a limit answer: Rate cuts / pauses the gate chosen by `scope`; Quota and Cap only record it.
    AdaptiveLimit::LimitResult onLimit(const LimitSignal& sig, LimitScope scope = LimitScope::Auto);
    /// The gate whose AIMD the last onLimit() would use with `scope` (diagnostics, tests); empty when none.
    QString feedbackGate(LimitScope scope, const LimitSignal& sig) const;

private:
    friend class Gate;
    friend class Gates;
    struct Hold { std::shared_ptr<detail::GateLink> link; quint64 id = 0; };
    void merge(Slot&& other);
    std::vector<Hold> m_holds;
    std::shared_ptr<detail::GateLink> m_kind;            // the kind gate (AIMD)
    std::shared_ptr<detail::GateLink> m_sub;             // the provider sub-gate, when there is one
};

class Gate : public QObject {
    Q_OBJECT
public:
    /// Runs on the gate's thread, synchronously from acquire() when a slot is free, else from a later drain(). It must
    /// not block; it may call into the gate. Dropping the Slot releases it.
    using Granted = std::function<void(Slot)>;
    /// The wait ended without a slot (the signal fired, cancel(), or the gate went away). Same thread rules.
    using Refused = std::function<void(const QString& why)>;

    /// `configured` is the width (read on every use). `adaptive` gives the gate an AdaptiveLimit over it.
    Gate(QString name, std::function<int()> configured, Clock& clock, bool adaptive, QObject* parent = nullptr);
    ~Gate() override;

    const QString& name() const { return m_name; }
    AdaptiveLimit* adapt() { return m_adapt.get(); }
    /// the limit right now: the configured one, or lower while the provider is limiting us
    int limit();
    int configured() const;
    bool paused() const { return m_adapt && m_adapt->paused(); }
    int running() const { return m_running; }
    int queued() const { return int(m_line.size()); }
    int peak() const { return m_peak; }
    quint64 served() const { return m_served; }

    /// Waits in line for a slot. Returns a ticket for cancel() (0 when answered at once).
    quint64 acquire(Granted granted, Refused refused = {}, const AbortSignal& signal = {});
    /// A slot now, only when one is free and nobody waits (never overtakes the line).
    std::optional<Slot> tryAcquire();
    /// Takes a waiting request out of the line; its Refused runs with `why`. False when the ticket is not waiting.
    bool cancel(quint64 ticket, const QString& why = QStringLiteral("Cancelled"));
    /// Lets waiters out while slots are free and the gate is not paused; arms the wake-up timer otherwise.
    void drain();
    /// peak = running, served = 0 (TS resetGateStats).
    void resetStats();
    /// TS GateStatus: {limit, running, queued, peak, served, configured, limited, paused, pausedForMs, cuts,
    /// rateAnswers, lastSignal?}.
    QJsonObject statusJson();

private:
    friend class Slot;
    friend class Gates;
    struct Waiter { quint64 ticket; Granted granted; Refused refused; AbortSignal signal; AbortSignal::ListenerId listener; };
    void grant(Waiter& w);
    Slot makeSlot();
    void releaseHold(quint64 id);
    void armTimer(qint64 ms);

    QString m_name;
    std::function<int()> m_configured;
    Clock& m_clock;
    std::unique_ptr<AdaptiveLimit> m_adapt;
    std::shared_ptr<detail::GateLink> m_link;
    std::deque<Waiter> m_line;
    std::unordered_set<quint64> m_live;                  // holds not yet released
    quint64 m_nextId = 0;
    int m_running = 0;
    int m_peak = 0;
    quint64 m_served = 0;
    bool m_draining = false;
    bool m_drainAgain = false;
    quint64 m_timer = 0;                                 // Clock timer id, 0 = none
    qint64 m_timerAt = 0;                                // steady ms
};

// ---------------------------------------------------------------------------------------------------------------

/// The widths. Defaults are the TS BASE numbers (local/concurrency.mjs: gates 3/4/4/8) plus poll 8; the server ranges
/// (design §4.3) are wider than the browser's: videos / images / audio / poll 1-64, chat 1-256.
struct GateLimits {
    std::array<int, kGateKindCount> width{3, 4, 4, 8, 8};
    /// per-provider sub-gate width per kind; 0 = no sub-gates for that kind (the TS behaviour, the default)
    std::array<int, kGateKindCount> perProvider{0, 0, 0, 0, 0};
    /// shared cap over videos + images + audio; 0 = none (TS heavyCap)
    int heavyCap = 0;

    int& operator[](GateKind k) { return width[size_t(k)]; }
    int operator[](GateKind k) const { return width[size_t(k)]; }
    static int minWidth(GateKind) { return 1; }
    static int maxWidth(GateKind k) { return k == GateKind::Chat ? 256 : 64; }
    /// Clamps every number into its range; one warning per value that was changed.
    GateLimits clamped(QStringList* warnings = nullptr) const;
    QJsonObject toJson() const;
};

class Gates : public QObject {
    Q_OBJECT
public:
    explicit Gates(Clock& clock, QObject* parent = nullptr);
    ~Gates() override;

    /// Applies new widths (clamped). Only a real change of a width resets that gate's slow-down (TS configureOpenRouter);
    /// every gate is drained. Returns the clamp warnings.
    QStringList configure(const GateLimits& limits);
    const GateLimits& limits() const { return m_limits; }
    /// The AIMD timing of every gate, sub-gates included (TS configureBackpressure).
    void tune(const AdaptiveTuning& t);

    Gate& gate(GateKind k) { return *m_gates[size_t(k)]; }
    Gate& heavy() { return *m_heavy; }
    /// The provider sub-gate of `key` for `kind`, nullptr when there is none (yet).
    Gate* subGate(GateKind kind, const QString& key) const;
    QuotaBreaker& breaker() { return m_breaker; }
    /// The model-health memory reset() and clearQuota() clear together with the breaker (TS getHealth()); may be null.
    void setHealth(ModelHealth* h) { m_health = h; }

    struct Request {
        GateKind kind = GateKind::Chat;
        /// the upstream provider key (providerKeyOf(model)); used only when perProvider[kind] > 0
        QString providerKey;
        /// a call that does not go through the proxy (a local model service): skips the shared heavy cap (TS ownOnly)
        bool ownOnly = false;
        AbortSignal signal;
    };
    /// Waits for the request's slots, in this order: provider sub-gate, kind gate, heavy cap. `granted` / `refused`
    /// run exactly once, queued on `context`'s thread (never inside this call); when `context` is destroyed first, the
    /// slots are released and nothing runs. A signal that fired before delivery refuses (the slots go back).
    void acquire(const Request& rq, QObject* context, Gate::Granted granted, Gate::Refused refused = {});

    /// Forgets every slow-down and every remembered quota (TS resetBackpressure; the board's "try again now").
    void reset();
    /// Forgets one model's quota (or all): the breaker and the model-health rest go together (TS clearQuota).
    void clearQuota(const QString& model = {});
    void resetStats();

    /// {videos:{...,providers:{key:{...}}}, images, audio, chat, poll}
    QJsonObject gateStatus();
    /// {limited, gates, heavy:{cap, running, queued, peak}, quota:[...]} (TS backpressureStatus + heavyPoolStatus)
    QJsonObject backpressureStatus();

    /// at most this many provider sub-gates per kind; idle ones are dropped first, and past it a request uses the
    /// kind gate alone
    static constexpr int kMaxSubGates = 64;

private:
    struct Pending;
    Gate* subFor(GateKind kind, const QString& key);
    void step(const std::shared_ptr<Pending>& p);
    void finish(const std::shared_ptr<Pending>& p, bool ok, const QString& why);

    Clock& m_clock;
    GateLimits m_limits;
    AdaptiveTuning m_tuning;
    std::array<Gate*, kGateKindCount> m_gates{};
    Gate* m_heavy = nullptr;
    std::array<QHash<QString, Gate*>, kGateKindCount> m_subs;
    QuotaBreaker m_breaker;
    ModelHealth* m_health = nullptr;
};

} // namespace studio::provider
