#ifndef PLAYPLAY_METERSNAPSHOT_H
#define PLAYPLAY_METERSNAPSHOT_H

#include <QtGlobal>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <type_traits>

/**
 * Compositor meter snapshot (perf-engine PE-05, docs/plans/perf-engine.md §7).
 *
 * Compositor::render() fills a Snapshot with the tick's measured sound (every source's pre-fader peak / RMS, every
 * mix's bus peaks, loudness, ducker, clip count and per-strip post-fader peaks) and publishes it through a SeqLock at
 * the end of the tick. metersJson() / status() copy it without taking the compositor mutex, so a UI reader can never
 * hold the clock thread up while it builds JSON. Plain numbers only: names, fader settings and layer specs are copied
 * by the readers under a short lock (implicitly shared values, no JSON), and source status() calls happen outside it.
 *
 * SeqLock: one writer (the clock thread, under the compositor mutex), any number of readers that never block it. The
 * writer makes the sequence odd, stores the payload, makes it even again; a reader copies the payload between two reads
 * of the sequence and retries when they differ or are odd (a store was in progress). The payload lives in relaxed
 * 64-bit atomics (plain moves on x86-64), so no access is a data race in the C++ memory model; the fences are Boehm's
 * seqlock recipe ("Can seqlocks get along with programming language memory models?", MSPC 2012).
 */
namespace metersnap {

constexpr int kMaxChannels = 16;    // audio channels metered per strip (a format with more reports the first 16)
constexpr int kMaxMixes = 8;        // Compositor::setMix's own limit
constexpr int kMaxSources = 32;     // program + audio-carrying layers (line-ins, PiP sources) pulled in one tick

/// One source's sound this tick before any fader (the per-source VU); the same for every mix.
struct alignas(8) Source {
    qint32 layer = 0;               // 0 = program, else the layer id (strip "L<id>")
    quint8 audible = 0, duck = 0;
    quint8 pad[2] = {};
    float prePeak[kMaxChannels] = {}, preRms[kMaxChannels] = {};
};

/// One source in one mix: the fader it was mixed with (before solo, item mute and ducking) and its post-fader peak.
struct alignas(8) MixStrip {
    double gain = 1.0;              // linear
    quint8 mute = 0, explicitSet = 0, inMix = 0;
    quint8 pad[5] = {};
    float peak[kMaxChannels] = {};
};

struct alignas(8) Mix {
    quint32 serial = 0;             // Compositor MixState serial: readers match mixes by it (an id can come back after removeMix)
    qint32 pad0 = 0;
    double momentaryLufs = -70.0, shortTermLufs = -70.0, peakDbfs = -100.0;   // LoudnessMeter's initial values
    double duckGain = 1.0, clips = 0.0;
    float chPeak[kMaxChannels] = {};    // bus peak per channel after master gain and clip (linear)
    MixStrip strips[kMaxSources];       // [i] belongs to Snapshot::sources[i]
};

struct alignas(8) Snapshot {
    qint64 frame = -1;              // the tick that published it; -1 = nothing rendered yet
    qint32 channels = 0;            // entries used in every per-channel array
    qint32 nSources = 0, nMixes = 0;
    qint32 pad0 = 0;
    qint64 integratedFrame = -1;    // the tick integratedLufs was computed at
    qint64 integratedBlocks = -1;   // LoudnessMeter::blocks() then
    double integratedLufs = -70.0;  // main mix, gated (BS.1770); refreshed about once a second: it scans up to an hour of blocks
    Source sources[kMaxSources];
    Mix mixes[kMaxMixes];           // last member: a store copies only the mixes in use (bytesFor)

    /// Bytes of the struct that hold data when `nMix` mixes are in use (the header, every source slot, those mixes).
    static constexpr size_t bytesFor(int nMix) {
        return offsetof(Snapshot, mixes) + size_t(nMix < 0 ? 0 : (nMix > kMaxMixes ? kMaxMixes : nMix)) * sizeof(Mix);
    }
    int findSource(int layer) const { for (int i = 0; i < nSources && i < kMaxSources; ++i) if (sources[i].layer == layer) return i; return -1; }
    const Mix* findMix(quint32 serial) const { for (int i = 0; i < nMixes && i < kMaxMixes; ++i) if (mixes[i].serial == serial) return &mixes[i]; return nullptr; }
};

/// Single-writer, multi-reader sequence lock over a trivially copyable T (see the file comment).
template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable<T>::value, "SeqLock holds plain data only");
    static_assert(sizeof(T) % 8 == 0, "SeqLock payload must be a whole number of 64-bit words (alignas(8))");
    static_assert(std::atomic<quint64>::is_always_lock_free, "64-bit atomics must be lock-free");
public:
    static constexpr size_t kWords = sizeof(T) / 8;

    SeqLock() { const T init{}; store(init); }
    SeqLock(const SeqLock&) = delete;
    SeqLock& operator=(const SeqLock&) = delete;

    /// Publish `v` (writer only; one writer at a time). `bytes` < sizeof(T) stores only that prefix: the rest keeps what
    /// an earlier store put there (readers must not use it; Snapshot's counts say what is valid).
    void store(const T& v, size_t bytes = sizeof(T)) {
        const size_t words = (qMin(bytes, sizeof(T)) + 7) / 8;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
        const quint32 s = m_seq.load(std::memory_order_relaxed);
        m_seq.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < words; ++i) { quint64 w; std::memcpy(&w, p + i * 8, 8); m_words[i].store(w, std::memory_order_relaxed); }
        m_seq.store(s + 2, std::memory_order_release);
    }
    /// One attempt: false when a store was in progress or happened meanwhile (`out` is then garbage).
    bool tryLoad(T& out) const {
        const quint32 s0 = m_seq.load(std::memory_order_acquire);
        if (s0 & 1u) return false;
        unsigned char* p = reinterpret_cast<unsigned char*>(&out);
        for (size_t i = 0; i < kWords; ++i) { const quint64 w = m_words[i].load(std::memory_order_relaxed); std::memcpy(p + i * 8, &w, 8); }
        std::atomic_thread_fence(std::memory_order_acquire);
        return m_seq.load(std::memory_order_relaxed) == s0;
    }
    /// A consistent copy. Never blocks the writer; retries while a store is in flight (a store takes about a microsecond).
    /// Returns the number of retries.
    int load(T& out) const {
        for (int tries = 0;; ++tries) {
            if (tryLoad(out)) return tries;
            if (tries >= 8) std::this_thread::yield();
        }
    }
    /// Stores so far (diagnostics and tests).
    quint32 stores() const { return m_seq.load(std::memory_order_acquire) / 2; }

private:
    std::atomic<quint32> m_seq{0};
    std::atomic<quint64> m_words[kWords];
};

} // namespace metersnap

#endif // PLAYPLAY_METERSNAPSHOT_H
