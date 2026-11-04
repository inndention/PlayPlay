#ifndef PLAYPLAY_TICKPROFILER_H
#define PLAYPLAY_TICKPROFILER_H

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

/**
 * Tick phase profiler (perf-engine PE-06, docs/plans/perf-engine.md): where the clock thread's time goes, phase by
 * phase, published once a second as `telemetry.tickPhases`. It exists to name the next stall in one look: the window's
 * per-phase avg/max, the slowest single layer-source call (layer id + source type), and the slowest tick of the window
 * and since start, each split by phase.
 *
 * Threading: everything runs on the clock thread. The engine owns one TickProfiler and binds it to the clock thread for
 * one tick (Bind); code called from the tick (the compositor, later the sources) times itself with ScopedPhase /
 * SourceCall, which find the bound profiler through a thread-local pointer and do nothing when none is bound (a test
 * rendering a Compositor directly, any other thread). No locks, no allocation per tick: two steady-clock reads per
 * phase, and a layer source's type() string is asked for only when that source becomes a tick's slowest and is not in
 * the small per-window cache. ~1 us per tick.
 *
 * Windows: takeWindow() closes the window (complete ticks only; a tick in progress lands in the next one) and returns a
 * plain value the engine keeps for stateJson(); toJson() can run on any thread. Phases may nest (render contains lock ..
 * audio), so phase sums can exceed the tick. Off switch: PLAYPLAY_TICK_PROFILE=0 (no timing, no telemetry key).
 */
namespace tickprof {

enum Phase : int {
    Commands,   // queued transport / CG commands applied
    Advance,    // item end / cut / next-item housekeeping
    Render,     // Compositor::render, all of it (the next six phases are inside it)
    Lock,       //   waiting for the compositor mutex (a reader such as metersJson / status holding it)
    Program,    //   program bus: A/B source pictures, slate, transition draw list
    Layers,     //   layer stack: every shown layer's video()
    Composite,  //   per-mix CPU paint or GPU submit
    Fetch,      //   GPU readback
    Audio,      //   every source's audio pulled once, then each output mix summed
    FanOut,     // frames handed to encoder stages and baseband sinks
    Preview,    // confidence-picture taps (program + extra mixes)
    Monitor,    // input monitor receivers
    Snapshot,   // state snapshot; once a second also output stats, hardening and the telemetry JSON
    PhaseCount
};

inline const char* phaseName(int p) {
    static const char* const names[PhaseCount] = {"commands", "advance", "render", "lock", "program", "layers",
                                                  "composite", "fetch", "audio", "fanout", "preview", "monitor", "snapshot"};
    return p >= 0 && p < PhaseCount ? names[p] : "?";
}
inline bool insideRender(int p) { return p >= Lock && p <= Audio; }

inline qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

/// One call into a layer source (video() or audio()).
struct SlowCall {
    int layer = 0;
    double ms = 0.0;
    const char* call = nullptr;     // "video" | "audio" (static strings)
    QString type;                   // LayerSource::type()
    bool valid() const { return call != nullptr; }
    QJsonObject toJson() const { return {{"layer", layer}, {"type", type}, {"call", QString::fromLatin1(call ? call : "")}, {"ms", round3(ms)}}; }
};

/// One tick, split by phase.
struct TickRecord {
    double ms = 0.0;
    qint64 frame = -1;
    qint64 utcMs = 0;
    std::array<double, PhaseCount> phases{};
    SlowCall slowest;               // the slowest layer-source call of that tick
    bool valid() const { return frame >= 0; }
    QJsonObject toJson(qint64 nowUtcMs) const {
        QJsonObject ph; for (int p = 0; p < PhaseCount; ++p) ph[phaseName(p)] = round3(phases[size_t(p)]);
        QJsonObject o{{"ms", round3(ms)}, {"frame", double(frame)}, {"utcMs", double(utcMs)},
                      {"ageSec", std::round(double(nowUtcMs - utcMs) / 100.0) / 10.0}, {"phases", ph}};
        if (slowest.valid()) o["slowestLayer"] = slowest.toJson();
        return o;
    }
};

struct PhaseStat { double sumMs = 0.0, maxMs = 0.0; };

/// One closed window (about one telemetry second).
struct Window {
    bool enabled = false;
    int ticks = 0;                  // complete ticks in the window
    double seconds = 0.0;           // wall time the window covered
    double tickAvgMs = 0.0, tickP95Ms = 0.0, tickMaxMs = 0.0;     // whole onFrameTick, snapshot included
    std::array<PhaseStat, PhaseCount> phases{};
    SlowCall slowestLayer;          // the slowest layer-source call in the window
    TickRecord slowestTick;         // the window's slowest tick
    TickRecord worst;               // the slowest tick since the profiler started (start-up stalls included)

    double phaseAvgMs(int p) const { return ticks > 0 ? phases[size_t(p)].sumMs / ticks : 0.0; }
    QJsonObject toJson(qint64 nowUtcMs = QDateTime::currentMSecsSinceEpoch()) const {
        QJsonObject o;
        o["ticks"] = ticks; o["windowSec"] = round3(seconds);
        o["tick"] = QJsonObject{{"avgMs", round3(tickAvgMs)}, {"p95Ms", round3(tickP95Ms)}, {"maxMs", round3(tickMaxMs)}};
        QJsonObject ph;
        for (int p = 0; p < PhaseCount; ++p) {
            QJsonObject e{{"avgMs", round3(phaseAvgMs(p))}, {"maxMs", round3(phases[size_t(p)].maxMs)}};
            if (insideRender(p)) e["in"] = QStringLiteral("render");
            ph[phaseName(p)] = e;
        }
        o["phases"] = ph;
        if (slowestLayer.valid()) o["slowestLayer"] = slowestLayer.toJson();
        if (slowestTick.valid()) o["slowestTick"] = slowestTick.toJson(nowUtcMs);
        if (worst.valid()) o["worst"] = worst.toJson(nowUtcMs);
        return o;
    }
};

class TickProfiler {
public:
    /// On unless PLAYPLAY_TICK_PROFILE is 0 / off / false.
    static bool enabledByEnvironment() {
        const QByteArray v = qgetenv("PLAYPLAY_TICK_PROFILE").trimmed().toLower();
        return !(v == "0" || v == "off" || v == "false" || v == "no");
    }
    explicit TickProfiler(bool enabled = enabledByEnvironment()) : m_on(enabled), m_windowStartNs(nowNs()) {}
    TickProfiler(const TickProfiler&) = delete;
    TickProfiler& operator=(const TickProfiler&) = delete;
    bool enabled() const { return m_on; }

    /// The profiler bound to the calling thread (nullptr when none).
    static TickProfiler* current() { return boundSlot(); }
    /// Binds a profiler to this thread for one scope (a tick); a disabled profiler is not bound.
    class Bind {
    public:
        explicit Bind(TickProfiler& p) : m_prev(boundSlot()) { if (p.enabled()) boundSlot() = &p; }
        ~Bind() { boundSlot() = m_prev; }
        Bind(const Bind&) = delete;
        Bind& operator=(const Bind&) = delete;
    private:
        TickProfiler* m_prev;
    };

    void beginTick(qint64 frame) {
        if (!m_on) return;
        m_inTick = true; m_curFrame = frame; m_curStartNs = nowNs();
        m_cur.fill(0.0); m_curSlow = SlowCall();
    }
    void add(Phase p, double ms) { if (m_on && m_inTick && p >= 0 && p < PhaseCount) m_cur[size_t(p)] += ms; }
    /// A layer source call took `ms`; `key` identifies the source (its address) for the type cache, `typeOf()` returns
    /// its type string and is called only when the call is the tick's slowest so far and the source is not cached.
    template <class TypeFn>
    void noteSource(int layer, const void* key, const char* call, double ms, TypeFn&& typeOf) {
        if (!m_on || !m_inTick || (m_curSlow.valid() && ms <= m_curSlow.ms)) return;
        m_curSlow.layer = layer; m_curSlow.ms = ms; m_curSlow.call = call;
        m_curSlow.type = typeFor(key, std::forward<TypeFn>(typeOf));
    }
    /// Close the tick. `totalMs` < 0 measures from beginTick (the engine); tests pass a value.
    void endTick(double totalMs = -1.0) {
        if (!m_on || !m_inTick) return;
        m_inTick = false;
        const double ms = totalMs >= 0.0 ? totalMs : double(nowNs() - m_curStartNs) / 1e6;
        ++m_ticks; m_tickSum += ms; m_tickMax = std::max(m_tickMax, ms);
        m_ring[size_t(m_ringPos++ % kRing)] = ms;
        for (size_t p = 0; p < size_t(PhaseCount); ++p) { m_phases[p].sumMs += m_cur[p]; m_phases[p].maxMs = std::max(m_phases[p].maxMs, m_cur[p]); }
        if (m_curSlow.valid() && (!m_slowLayer.valid() || m_curSlow.ms > m_slowLayer.ms)) m_slowLayer = m_curSlow;
        if (!m_slowTick.valid() || ms > m_slowTick.ms) record(m_slowTick, ms);
        if (!m_worst.valid() || ms > m_worst.ms) record(m_worst, ms);
    }
    /// Close the window: the ticks completed since the last call. Starts the next window (the worst tick carries on).
    Window takeWindow() {
        Window w; w.enabled = m_on;
        if (!m_on) return w;
        const qint64 now = nowNs();
        w.seconds = double(now - m_windowStartNs) / 1e9; m_windowStartNs = now;
        w.ticks = m_ticks; w.tickAvgMs = m_ticks > 0 ? m_tickSum / m_ticks : 0.0; w.tickMaxMs = m_tickMax; w.tickP95Ms = p95();
        w.phases = m_phases; w.slowestLayer = m_slowLayer; w.slowestTick = m_slowTick; w.worst = m_worst;
        m_ticks = 0; m_ringPos = 0; m_tickSum = 0.0; m_tickMax = 0.0;
        m_phases.fill(PhaseStat()); m_slowLayer = SlowCall(); m_slowTick = TickRecord();
        for (TypeEntry& e : m_types) e.key = nullptr;      // a freed source's address may be reused: forget types each window
        return w;
    }

private:
    static constexpr int kRing = 256;           // p95 uses the window's last 256 ticks (a 1 s window at 60 fps has 60)
    struct TypeEntry { const void* key = nullptr; QString type; };

    static TickProfiler*& boundSlot() { static thread_local TickProfiler* p = nullptr; return p; }
    template <class TypeFn>
    QString typeFor(const void* key, TypeFn&& typeOf) {
        for (const TypeEntry& e : m_types) if (e.key == key && key) return e.type;
        TypeEntry& e = m_types[size_t(m_typeNext++ % int(m_types.size()))];
        e.key = key; e.type = typeOf();
        return e.type;
    }
    void record(TickRecord& r, double ms) {
        r.ms = ms; r.frame = m_curFrame; r.utcMs = QDateTime::currentMSecsSinceEpoch(); r.phases = m_cur; r.slowest = m_curSlow;
    }
    double p95() const {
        const int n = std::min(m_ticks, kRing);
        if (n <= 0) return 0.0;
        std::array<double, kRing> v;
        std::copy(m_ring.begin(), m_ring.begin() + n, v.begin());
        const int k = std::max(0, int(std::ceil(0.95 * n)) - 1);
        std::nth_element(v.begin(), v.begin() + k, v.begin() + n);
        return v[size_t(k)];
    }

    const bool m_on;
    // the tick in progress
    bool m_inTick = false;
    qint64 m_curFrame = -1, m_curStartNs = 0;
    std::array<double, PhaseCount> m_cur{};
    SlowCall m_curSlow;
    // the open window
    qint64 m_windowStartNs = 0;
    int m_ticks = 0, m_ringPos = 0;
    double m_tickSum = 0.0, m_tickMax = 0.0;
    std::array<double, kRing> m_ring{};
    std::array<PhaseStat, PhaseCount> m_phases{};
    SlowCall m_slowLayer;
    TickRecord m_slowTick, m_worst;
    std::array<TypeEntry, 8> m_types;
    int m_typeNext = 0;
};

/// Adds the scope's duration to `phase` of the bound (or given) profiler; stop() ends it early. Nothing when unbound.
class ScopedPhase {
public:
    explicit ScopedPhase(Phase phase) : ScopedPhase(TickProfiler::current(), phase) {}
    ScopedPhase(TickProfiler& prof, Phase phase) : ScopedPhase(prof.enabled() ? &prof : nullptr, phase) {}
    ~ScopedPhase() { stop(); }
    void stop() { if (m_p) { m_p->add(m_phase, double(nowNs() - m_t0) / 1e6); m_p = nullptr; } }
    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
private:
    ScopedPhase(TickProfiler* p, Phase phase) : m_p(p), m_phase(phase), m_t0(p ? nowNs() : 0) {}
    TickProfiler* m_p;
    Phase m_phase;
    qint64 m_t0;
};

/// Times one call into a layer source for the "slowest layer" record: `SourceCall sc(layerId, src, "video");`
template <class Src>
class SourceCall {
public:
    SourceCall(int layer, const Src* src, const char* call)
        : m_p(src ? TickProfiler::current() : nullptr), m_src(src), m_layer(layer), m_call(call), m_t0(m_p ? nowNs() : 0) {}
    ~SourceCall() {
        if (!m_p) return;
        const Src* s = m_src;
        m_p->noteSource(m_layer, s, m_call, double(nowNs() - m_t0) / 1e6, [s]() { return s->type(); });
    }
    SourceCall(const SourceCall&) = delete;
    SourceCall& operator=(const SourceCall&) = delete;
private:
    TickProfiler* m_p;
    const Src* m_src;
    int m_layer;
    const char* m_call;
    qint64 m_t0;
};

} // namespace tickprof

#endif // PLAYPLAY_TICKPROFILER_H
