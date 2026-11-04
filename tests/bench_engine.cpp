#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include "playout/engine/decoderpipeline.h"
#include "playout/engine/encoderstage.h"
#include "playout/engine/sinks/nullsink.h"
#include "playout/engine/sinks/ndisink.h"
#include "playout/engine/compositor/basicsources.h"
#include "playout/engine/compositor/livesources.h"
#include "playout/engine/compositor/compositor.h"
#include "playout/engine/mediaframe.h"
#include "playout/engine/mixer.h"
#include "playout/engine/tickprofiler.h"
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
#endif

/**
 * Engine benchmark (perf-engine lane, slice PE-23; docs/plans/perf-engine.md §5.2). Companion of bench_pipeline
 * (decode / composite / convert), which stays untouched so its numbers remain comparable. Every row prints one
 * "BENCH ..." line so before/after runs can be diffed:
 *
 *   encode            EncoderStage + a counting null sink, presets h264_aac and atsc_mpeg2_ac3_hd, 1080p29.97 frames
 *                     pushed at real-time pace: encode ms avg/p95/max, CPU cores, encoder thread count, private bytes,
 *                     first-packet latency (ms and frames), queue drops, output kbps; PE-09 settings (codec thread_count,
 *                     pooled / legacy pictures, rounding) and picture-buffer allocs / copies. PLAYPLAY_BENCH_ENCODE_AB=<n>
 *                     runs n rounds of legacy (pre-PE-09) / pe09 / fastrnd rows per preset
 *   template          TemplateSource::video() per frame for ticker / clock / countdown / a static lower third
 *                     (the clock-thread QPainter cost PE-07 removes), how many new pictures were produced, and the
 *                     PE-07 cache state and paint count; in bench mode each row also runs with PLAYPLAY_TEMPLATE_CACHE=0
 *   composite_cg      one CPU compositor tick with a changing program + lower third + ticker + clock (bench mode: also
 *                     with PLAYPLAY_TEMPLATE_CACHE=0)
 *   livesource_lock   NdiSource on a sender that does not exist (perftest-*): max latency of video()/audio()/status()
 *                     called from the "clock" every 33 ms while the receive loop runs (PE-03 regression row)
 *   meters_contention the composite_cg scene plus 4 audio layers and a second mix, rendered with and without a UI thread
 *                     polling metersJson() at 100 Hz (status() / layersJson() every 10th call), with the PE-05 snapshot
 *                     readers and with PLAYPLAY_COMP_LEGACY_STATUS=1: render ms and render's wait for the compositor lock
 *                     (PLAYPLAY_BENCH_METER_FRAMES, default 300 paced; PLAYPLAY_BENCH_ROUNDS=N interleaves N rounds and
 *                     prints medians)
 *   audio_pool        PE-13 A/B: a tiny raster with a tone program, 6 audio layers and 3 mixes; the compositor's audio phase
 *                     with the pooled buffers / recycled entries (default) and with PLAYPLAY_AUDIO_POOL=0; both must output
 *                     the same samples (interleaved rounds, PLAYPLAY_BENCH_ROUNDS, default 3, medians)
 *   premul_ab         PE-08 A/B: the composite_cg scene (countdown instead of the wall clock) plus an HTML-like full-raster
 *                     page, rendered with PLAYPLAY_PREMUL=0 (straight) and =1 (premultiplied): tick, layers (template
 *                     video()) and composite ms, CPU, the decode-thread premultiply a page picture now costs, and how far
 *                     the two outputs of the same frame are apart (PLAYPLAY_BENCH_PREMUL_FRAMES, default 300 paced;
 *                     PLAYPLAY_BENCH_ROUNDS=N interleaves and prints medians)
 *   ndi_fourcc_option PE-11: options.fourcc / PLAYPLAY_NDI_FOURCC resolution (bgrx default; option beats env)
 *   ndi_uyvy_colours PE-11: NdiUyvyConverter on 100 % / 75 % colour bars: every bar's Y / Cb / Cr is BT.709 limited range
 *                     at 1080 (BT.601 at 480) within 1 code; ring pictures held by NDI are never reused (no network)
 *   ndi_roundtrip    PE-11: NdiSink "perftest-ndi" (uyvy / bgrx / bgra) received back by an NdiSource: bars come back
 *                     at the sent RGB within NDI's codec error, opaque; only with PLAYPLAY_BENCH_NDI=1 (announces a sender)
 *   ndi_send         NdiSink "perftest-bench" CPU per requested FourCC (PLAYPLAY_NDI_FOURCC); only with
 *                     PLAYPLAY_BENCH_NDI=1, because it announces an NDI sender on the network
 *
 * Without PLAYPLAY_BENCH=1 the same code runs as a short smoke test (few frames, no pacing, "SMOKE" lines) that only
 * checks the harness works: packets come out of both encoders, templates render, the live-source calls return.
 * Knobs: PLAYPLAY_BENCH_FRAMES (encode frames, default 600), PLAYPLAY_BENCH_PACE=0 (push as fast as the encoder takes),
 * PLAYPLAY_BENCH_LIVE_SEC (livesource_lock duration, default 5).
 */
class BenchEngine : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void encode_data();
    void encode();
    void templates_data();
    void templates();
    void composite_cg_data();
    void composite_cg();
    void livesource_lock();
    void meters_contention();
    void audio_pool();
    void premul_ab();
    void ndi_fourcc_option();
    void ndi_uyvy_colours_data();
    void ndi_uyvy_colours();
    void ndi_roundtrip_data();
    void ndi_roundtrip();
    void ndi_input_probe();
    void ndi_send();
    void decode_rate();
private:
    bool m_bench = false;
    QTemporaryDir m_dir;
    const char* tag() const { return m_bench ? "BENCH" : "SMOKE"; }
};

namespace {

double cpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto toS = [](FILETIME f) { ULARGE_INTEGER x; x.LowPart = f.dwLowDateTime; x.HighPart = f.dwHighDateTime; return double(x.QuadPart) / 1e7; };
    return toS(k) + toS(u);
#else
    return 0.0;
#endif
}

double privateMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) return double(pmc.PrivateUsage) / 1048576.0;
#endif
    return 0.0;
}

int threadCount() {
#ifdef _WIN32
    const DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int n = 0;
    if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == me) ++n; } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
#else
    return 0;
#endif
}

/// Sets PLAYPLAY_* switches for one row and puts the previous values back (a null value unsets).
class EnvScope {
public:
    explicit EnvScope(const QList<QPair<QByteArray, QByteArray>>& vars) {
        for (const auto& kv : vars) {
            m_saved.append({kv.first, qgetenv(kv.first.constData()), qEnvironmentVariableIsSet(kv.first.constData())});
            if (kv.second.isNull()) qunsetenv(kv.first.constData()); else qputenv(kv.first.constData(), kv.second);
        }
    }
    ~EnvScope() { for (const Saved& s : m_saved) { if (s.wasSet) qputenv(s.name.constData(), s.value); else qunsetenv(s.name.constData()); } }
private:
    struct Saved { QByteArray name, value; bool wasSet; };
    QList<Saved> m_saved;
};

struct Dist { double avg = 0, p95 = 0, max = 0; };
Dist dist(std::vector<double> v) {
    Dist d; if (v.empty()) return d;
    double s = 0; for (double x : v) s += x; d.avg = s / double(v.size());
    std::sort(v.begin(), v.end()); d.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))]; d.max = v.back();
    return d;
}

const double kFramePeriodS = 1001.0 / 30000.0;
const double kTwoPi = 6.283185307179586;

/// Samples of audio in output frame `i` at 29.97 (the 1601/1602 cadence the compositor delivers).
int samplesForFrame(qint64 i, int rate = 48000) {
    auto edge = [rate](qint64 f) { return qint64(std::floor(double(f) * rate * 1001.0 / 30000.0)); };
    return int(edge(i + 1) - edge(i));
}

/// Distinct 1080p BGRA pictures that keep an encoder busy like moving content: a shifting gradient plus a block of
/// deterministic noise that moves across the frame. Built once; frames are handed out by reference (no per-frame copy).
std::vector<FramePtr> makeProgramFrames(int w, int h, int count) {
    std::vector<FramePtr> out;
    quint32 lcg = 12345u;
    for (int k = 0; k < count; ++k) {
        FramePtr f = allocVideoFrame(w, h, AV_PIX_FMT_BGRA);
        const int bx = (k * 97) % qMax(1, w - 256), by = (k * 53) % qMax(1, h - 256);
        for (int y = 0; y < h; ++y) {
            auto* r = reinterpret_cast<quint32*>(f->data[0] + size_t(y) * f->linesize[0]);
            for (int x = 0; x < w; ++x) {
                quint32 px = 0xFF000000u | quint32((x + k * 24) & 255) << 16 | quint32((y + k * 8) & 255) << 8 | quint32(((x ^ y) + k * 15) & 255);
                if (x >= bx && x < bx + 256 && y >= by && y < by + 256) { lcg = lcg * 1664525u + 1013904223u; px = 0xFF000000u | (lcg >> 8); }
                r[x] = px;
            }
        }
        out.push_back(f);
    }
    return out;
}

FramePtr makeAudio(qint64 frame, double& phase) {
    const int n = samplesForFrame(frame);
    FramePtr a = allocAudioFrame(n, 2, 48000);
    for (int c = 0; c < 2; ++c) {
        auto* p = reinterpret_cast<float*>(a->data[c]);
        double ph = phase;
        for (int i = 0; i < n; ++i) { p[i] = float(0.1 * std::sin(ph)); ph += kTwoPi * 440.0 / 48000.0; }
        if (c == 1) phase = std::fmod(ph, kTwoPi);
    }
    return a;
}

/// Counting packet sink that also records when (and after how many pushed frames) the first video packet arrived.
class FirstPacketSink : public PacketSink {
public:
    FirstPacketSink(const std::atomic<qint64>* pushed, const QElapsedTimer* clock) : PacketSink("perftest-bench-null"), m_pushed(pushed), m_clock(clock) {}
    QString type() const override { return "null"; }
    bool start(const AVCodecParameters*, AVRational, const AVCodecParameters*, AVRational) override { m_running = true; return true; }
    void stop() override { m_running = false; }
    bool isRunning() const override { return m_running.load(); }
    void writePacket(const AVPacket* pkt, int streamKind) override {
        if (streamKind == 0) {
            if (firstVideoNs.load() < 0) { firstVideoNs = m_clock->nsecsElapsed(); firstVideoPushed = m_pushed->load(); }
            videoPackets++; videoBytes += pkt->size;
        } else audioPackets++;
    }
    std::atomic<qint64> firstVideoNs{-1}, firstVideoPushed{0}, videoPackets{0}, audioPackets{0}, videoBytes{0};
private:
    const std::atomic<qint64>* m_pushed;
    const QElapsedTimer* m_clock;
    std::atomic<bool> m_running{false};
};

/// Program source handing out a different picture every frame (as a decoder does).
class CyclingSource : public LayerSource {
public:
    explicit CyclingSource(std::vector<FramePtr> frames) : m_frames(std::move(frames)) {}
    QString type() const override { return "bench"; }
    bool open(const OutputFormat&) override { return !m_frames.empty(); }
    void close() override {}
    bool isOpen() const override { return !m_frames.empty(); }
    FramePtr video(qint64 frame, double) override { FramePtr f = makeFramePtr(); av_frame_ref(f.get(), m_frames[size_t(frame) % m_frames.size()].get()); return f; }
private:
    std::vector<FramePtr> m_frames;
};

QJsonObject templateSpec(const QString& kind) {
    if (kind == "ticker") return QJsonObject{{"type", "template"}, {"kind", "ticker"}, {"label", "PERF"},
        {"text", "Isolated engine benchmark ticker: a long crawl of headline text that keeps the painter busy for the whole width of the raster"}};
    if (kind == "clock") return QJsonObject{{"type", "template"}, {"kind", "clock"}, {"format", "hh:mm:ss"}};
    if (kind == "countdown") return QJsonObject{{"type", "template"}, {"kind", "countdown"}, {"seconds", 3600}, {"label", "ON AIR IN"}};
    return QJsonObject{{"type", "template"}, {"kind", "lower_third"}, {"title", "PERF TEST"}, {"subtitle", "bench_engine"}};
}

} // namespace

void BenchEngine::initTestCase() {
    m_bench = qEnvironmentVariable("PLAYPLAY_BENCH") == "1";
#ifdef _WIN32
    timeBeginPeriod(1);   // 1 ms sleeps for the pacing loops (the default 15.6 ms tick would distort real-time pacing)
#endif
    qInfo().noquote() << QString("%1 bench_engine: %2 logical CPUs, mode %3").arg(tag()).arg(QThread::idealThreadCount()).arg(m_bench ? "bench" : "smoke (set PLAYPLAY_BENCH=1 for numbers)");
}

void BenchEngine::cleanupTestCase() {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// encode
// ---------------------------------------------------------------------------------------------------------------------

// PE-09 A/B: PLAYPLAY_BENCH_ENCODE_AB=<rounds> runs every preset as "legacy" (PLAYPLAY_ENC_THREADS=auto +
// PLAYPLAY_ENC_LEGACY_FRAMES=1: the pre-PE-09 encoder), "pe09" (the defaults) and "fastrnd" (+ PLAYPLAY_ENC_FAST_RND=1),
// the order reversed every other round. Without it the rows run with whatever the environment says.
void BenchEngine::encode_data() {
    QTest::addColumn<QString>("preset");
    QTest::addColumn<QString>("mode");
    const QStringList presets{"h264_aac", "atsc_mpeg2_ac3_hd"};
    const int rounds = m_bench ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_ENCODE_AB") : 0;
    if (rounds <= 0) { for (const QString& p : presets) QTest::newRow(qPrintable(p)) << p << QString(); return; }
    for (int r = 0; r < rounds; ++r)
        for (const QString& p : presets) {
            const QStringList modes = (r % 2) ? QStringList{"fastrnd", "pe09", "legacy"} : QStringList{"legacy", "pe09", "fastrnd"};
            for (const QString& m : modes) QTest::newRow(qPrintable(QString("%1 %2 r%3").arg(p, m).arg(r + 1))) << p << m;
        }
}

void BenchEngine::encode() {
    QFETCH(QString, preset);
    QFETCH(QString, mode);
    EnvScope env(mode == "legacy" ? QList<QPair<QByteArray, QByteArray>>{{"PLAYPLAY_ENC_THREADS", "auto"}, {"PLAYPLAY_ENC_LEGACY_FRAMES", "1"}, {"PLAYPLAY_ENC_FAST_RND", QByteArray()}}
               : mode == "pe09" ? QList<QPair<QByteArray, QByteArray>>{{"PLAYPLAY_ENC_THREADS", QByteArray()}, {"PLAYPLAY_ENC_LEGACY_FRAMES", QByteArray()}, {"PLAYPLAY_ENC_FAST_RND", QByteArray()}}
               : mode == "fastrnd" ? QList<QPair<QByteArray, QByteArray>>{{"PLAYPLAY_ENC_THREADS", QByteArray()}, {"PLAYPLAY_ENC_LEGACY_FRAMES", QByteArray()}, {"PLAYPLAY_ENC_FAST_RND", "1"}}
               : QList<QPair<QByteArray, QByteArray>>{});
    const int W = 1920, H = 1080;
    const int frames = m_bench ? qMax(30, qEnvironmentVariableIntValue("PLAYPLAY_BENCH_FRAMES") > 0 ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_FRAMES") : 600) : 45;
    const bool paced = m_bench && qEnvironmentVariable("PLAYPLAY_BENCH_PACE") != "0";
    const std::vector<FramePtr> pics = makeProgramFrames(W, H, 16);

    EncoderProfile profile = EncoderProfile::preset(preset);
    if (preset == "h264_aac") profile.videoBitrateKbps = 6000;     // as the live main UDP output (channel.json: h264_aac @ 6 Mb/s)
    const int threads0 = threadCount();
    const double priv0 = privateMB();
    std::atomic<qint64> pushed{0};
    QElapsedTimer clock;
    auto sink = std::make_unique<FirstPacketSink>(&pushed, &clock);
    EncoderStage stage(profile, W, H, FrameRate::ntsc());
    QVERIFY2(stage.initialize(), qPrintable(stage.lastError()));
    QVERIFY(stage.addSink(sink.get()));
    int threadsMax = threadCount();
    double privMax = privateMB();

    std::vector<double> encMs; encMs.reserve(size_t(frames));
    std::vector<std::pair<qint64, double>> spikes;     // (encoded frame index, ms) of encodes over 50 ms: where the queue drops come from
    qint64 seenEncoded = 0, lumped = 0;
    auto sampleEncoder = [&]() {
        const EncoderStage::Stats st = stage.stats();
        if (st.framesEncoded == seenEncoded + 1) encMs.push_back(st.lastEncodeMs);
        else if (st.framesEncoded > seenEncoded + 1) { encMs.push_back(st.lastEncodeMs); lumped += st.framesEncoded - seenEncoded - 1; }   // polled too late for some
        if (st.framesEncoded > seenEncoded && st.lastEncodeMs > 50.0) spikes.push_back({st.framesEncoded - 1, st.lastEncodeMs});
        seenEncoded = qMax(seenEncoded, st.framesEncoded);
        return st;
    };

    double phase = 0.0;
    const double c0 = cpuSeconds();
    clock.start();
    for (qint64 i = 0; i < frames; ++i) {
        if (paced) {
            const qint64 dueNs = qint64(double(i) * kFramePeriodS * 1e9);
            while (clock.nsecsElapsed() < dueNs) { sampleEncoder(); QThread::usleep(1000); }
        } else {
            // as fast as the encoder takes them, without overflowing its queue (drops would skew the numbers)
            QElapsedTimer guard; guard.start();
            while (sampleEncoder().queueDepth >= 2 && guard.elapsed() < 5000) QThread::usleep(500);
        }
        MediaFrame mf; mf.frameNumber = i; mf.timestampNs = clock.nsecsElapsed();
        mf.video = pics[size_t(i) % pics.size()]; mf.audio = makeAudio(i, phase);
        stage.pushFrame(mf);
        pushed.fetch_add(1);
        if (i % 30 == 0) { threadsMax = qMax(threadsMax, threadCount()); privMax = qMax(privMax, privateMB()); }
    }
    const double pushWallS = clock.nsecsElapsed() / 1e9;
    // drain: wait until every frame the queue kept is encoded
    QElapsedTimer drain; drain.start();
    EncoderStage::Stats st = sampleEncoder();
    while (st.framesEncoded + st.framesDropped < st.framesIn && drain.elapsed() < 15000) { QThread::usleep(1000); st = sampleEncoder(); }
    const double wallS = clock.nsecsElapsed() / 1e9;
    const double cores = (cpuSeconds() - c0) / qMax(0.001, wallS);
    threadsMax = qMax(threadsMax, threadCount()); privMax = qMax(privMax, privateMB());
    const QString vcodec = stage.actualVideoCodec(), acodec = stage.actualAudioCodec();
    const int outW = stage.outputWidth(), outH = stage.outputHeight();
    // PE-09 settings and picture-buffer counters: thread_count given to the codec (0 = its own count), pool or legacy
    // single frame, rounding; allocs / copies / codecHeld from the stage's stats
    const QString pe09 = QString("  [%1threads %2  pictures %3  rnd %4  yuv allocs %5 copies %6 codecHeld %7]")
        .arg(mode.isEmpty() ? QString() : mode + ": ").arg(stage.videoThreads() ? QString::number(stage.videoThreads()) : QString("codec-auto"))
        .arg(QString(stage.pooledPictures() ? "pool" : "legacy")).arg(QString(stage.fastRounding() ? "fast" : "accurate"))
        .arg(st.yuvAllocs).arg(st.yuvCopies).arg(st.yuvCodecHeld);
    QStringList spikeText;
    for (size_t k = 0; k < spikes.size() && k < 6; ++k) spikeText << QString("#%1:%2").arg(spikes[k].first).arg(spikes[k].second, 0, 'f', 0);
    const QString spikeInfo = spikes.empty() ? QString() : QString("  spikes>50ms %1 [%2%3]").arg(qint64(spikes.size())).arg(spikeText.join(' ')).arg(QString(spikes.size() > 6 ? " ..." : ""));
    stage.shutdown();

    const Dist d = dist(encMs);
    const qint64 firstNs = sink->firstVideoNs.load();
    // Rate per encoded frame (what the stage's rate control aims at, comparable with the profile's %7k). Frames the queue
    // dropped leave PTS gaps instead of compressing the timeline, so over the stream timeline (every pushed frame) the rate
    // is lower by the drop share; that figure is appended when frames were dropped.
    const double vbytes = double(sink->videoBytes.load());
    const double kbps = vbytes * 8.0 / 1000.0 / qMax(0.001, double(qMax<qint64>(1, st.framesEncoded)) * kFramePeriodS);
    const QString timelineKbps = st.framesDropped > 0
        ? QString(" (%1 kbps over the timeline)").arg(vbytes * 8.0 / 1000.0 / qMax(0.001, double(frames) * kFramePeriodS), 0, 'f', 0) : QString();
    qInfo().noquote() << QString("%1 encode %2 [%3+%4 %5x%6 %7k %8]: encode avg %9 ms  p95 %10  max %11  CPU %12 cores  threads +%13  private +%14 MB  "
                                 "first packet %15 ms (%16 frames)  dropped %17 repeated %18  video %19 kbps%23  frames %20  push %21 s%22%24")
        .arg(tag(), preset, vcodec, acodec).arg(outW).arg(outH).arg(profile.videoBitrateKbps).arg(paced ? "paced" : "unpaced")
        .arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2).arg(d.max, 0, 'f', 2).arg(cores, 0, 'f', 2)
        .arg(threadsMax - threads0).arg(privMax - priv0, 0, 'f', 0)
        .arg(firstNs >= 0 ? firstNs / 1e6 : -1.0, 0, 'f', 0).arg(sink->firstVideoPushed.load())
        .arg(st.framesDropped).arg(st.videoRepeated).arg(kbps, 0, 'f', 0).arg(st.framesEncoded).arg(pushWallS, 0, 'f', 1)
        .arg(lumped ? QString("  (%1 frames not sampled singly)").arg(lumped) : QString())
        .arg(timelineKbps).arg(pe09 + spikeInfo);

    QVERIFY2(sink->videoPackets.load() > 0, "no video packets came out of the encoder");
    QVERIFY2(sink->audioPackets.load() > 0, "no audio packets came out of the encoder");
    QVERIFY(st.framesEncoded > 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// template (TemplateSource::video on the clock thread)
// ---------------------------------------------------------------------------------------------------------------------

// PE-07: in bench mode every row also runs with PLAYPLAY_TEMPLATE_CACHE=0 (the per-frame render), so before / after come
// from one run; the smoke test runs the default (cached) rows only.
void BenchEngine::templates_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("cacheOff");
    const struct { const char* row; const char* kind; } rows[] = {{"ticker", "ticker"}, {"clock", "clock"}, {"countdown", "countdown"}, {"lower_third (static)", "lower_third"}};
    for (const auto& r : rows) {
        QTest::newRow(r.row) << QString(r.kind) << false;
        if (m_bench) QTest::newRow(qPrintable(QString("%1 cache off").arg(r.row))) << QString(r.kind) << true;
    }
}

void BenchEngine::templates() {
    QFETCH(QString, kind);
    QFETCH(bool, cacheOff);
    EnvScope env({{"PLAYPLAY_TEMPLATE_CACHE", cacheOff ? QByteArray("0") : QByteArray()}});
    OutputFormat fmt;
    TemplateSource src(templateSpec(kind));
    QVERIFY(src.open(fmt));
    for (int i = 0; i < 5; ++i) src.video(i, i * kFramePeriodS);    // warm up fonts / glyph caches
    const int frames = m_bench ? 300 : 30;
    std::vector<double> ms; ms.reserve(size_t(frames));
    int newPictures = 0; const AVFrame* last = nullptr;
    const double c0 = cpuSeconds();
    for (int i = 5; i < 5 + frames; ++i) {
        QElapsedTimer t; t.start();
        FramePtr f = src.video(i, i * kFramePeriodS);
        ms.push_back(t.nsecsElapsed() / 1e6);
        QVERIFY2(f && f->width > 0 && f->height > 0, "template rendered no picture");
        if (f.get() != last) { ++newPictures; last = f.get(); }
    }
    const double cpuMs = (cpuSeconds() - c0) * 1000.0 / frames;
    const QSize sz = src.nativeSize();
    const Dist d = dist(ms);
    const QJsonObject st = src.status();          // PE-07: cache strip | string | off | per-frame: why; paints since open
    const QString cache = st.contains("cache") ? QString("  [cache %1, paints %2]").arg(st.value("cache").toString()).arg(st.value("paints").toDouble()) : QString();
    qInfo().noquote() << QString("%1 template %2 %3x%4: video() avg %5 ms  p95 %6  max %7  CPU %8 ms/frame  new pictures %9/%10%11")
        .arg(tag(), kind).arg(sz.width()).arg(sz.height()).arg(d.avg, 0, 'f', 3).arg(d.p95, 0, 'f', 3).arg(d.max, 0, 'f', 3)
        .arg(cpuMs, 0, 'f', 3).arg(newPictures).arg(frames).arg(cache);
    src.close();
}

// ---------------------------------------------------------------------------------------------------------------------
// composite_cg: whole CPU tick with the usual identity graphics on top of a changing program
// ---------------------------------------------------------------------------------------------------------------------

void BenchEngine::composite_cg_data() {
    QTest::addColumn<bool>("cacheOff");
    QTest::newRow("template cache") << false;
    if (m_bench) QTest::newRow("template cache off") << true;     // PE-07 A/B: the per-frame template render
}

void BenchEngine::composite_cg() {
    QFETCH(bool, cacheOff);
    EnvScope env({{"PLAYPLAY_TEMPLATE_CACHE", cacheOff ? QByteArray("0") : QByteArray()}});
    OutputFormat fmt;
    Compositor comp(fmt);     // CPU path (no setGpuEnabled): what gpuMode auto settles on with this Arc (perf-engine.md §1.3)
    comp.programTake(std::make_unique<CyclingSource>(makeProgramFrames(fmt.width, fmt.height, 8)), Compositor::Transition(), 0);
    const struct { int layer; const char* role; const char* kind; const char* anchor; double mx, my; } cg[] = {
        {30, "lower_third", "lower_third", "bottom_left", 120, 90}, {40, "ticker", "ticker", "bottom", 0, 0}, {45, "legal_id", "clock", "top_left", 60, 60}};
    for (const auto& c : cg) {
        auto src = std::make_unique<TemplateSource>(templateSpec(c.kind)); QVERIFY(src->open(fmt));
        comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"layer", c.layer}, {"role", c.role},
            {"placement", QJsonObject{{"anchor", c.anchor}, {"marginX", c.mx}, {"marginY", c.my}}}}), std::move(src), 0);
    }
    for (int i = 0; i < 30; ++i) comp.render(i, i * kFramePeriodS);    // warm up, layers attach
    const int frames = m_bench ? 300 : 30;
    std::vector<double> ms;
    const double c0 = cpuSeconds();
    for (qint64 f = 30; f < 30 + frames; ++f) {
        QElapsedTimer t; t.start();
        MediaFrame mf = comp.render(f, f * kFramePeriodS);
        ms.push_back(t.nsecsElapsed() / 1e6);
        QVERIFY(mf.video);
    }
    const double cpuMs = (cpuSeconds() - c0) * 1000.0 / frames;
    const Dist d = dist(ms);
    qInfo().noquote() << QString("%1 composite_cg cpu program+L3+ticker+clock%7: tick avg %2 ms  p95 %3  max %4  CPU %5 ms/frame  frames %6")
        .arg(tag()).arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2).arg(d.max, 0, 'f', 2).arg(cpuMs, 0, 'f', 2).arg(frames)
        .arg(cacheOff ? " [template cache off]" : "");
}

// ---------------------------------------------------------------------------------------------------------------------
// livesource_lock: clock-thread calls into a live source whose sender is absent
// ---------------------------------------------------------------------------------------------------------------------

void BenchEngine::livesource_lock() {
    if (!NdiSource::isAvailable()) QSKIP("NDI not compiled in");
    // A sender that cannot exist (perftest- prefix + pid): the receive loop keeps searching, exactly the 2026-10-24 case.
    const QString missing = QString("perftest-missing-%1").arg(QCoreApplication::applicationPid());
    QVERIFY(missing.startsWith("perftest-"));
    OutputFormat fmt;
    NdiSource src(QJsonObject{{"type", "ndi"}, {"name", missing}, {"recvName", "perftest-bench-rx"}, {"audio", true}});
    if (!src.open(fmt)) QSKIP(qPrintable("NDI runtime not usable: " + src.lastError()));
    const double seconds = m_bench ? (qEnvironmentVariableIntValue("PLAYPLAY_BENCH_LIVE_SEC") > 0 ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_LIVE_SEC") : 5.0) : 1.5;
    FramePtr dst = allocAudioFrame(1601, fmt.channels, fmt.sampleRate);
    std::vector<double> vMs, aMs, sMs; int over2 = 0, calls = 0;
    QString detail;
    QElapsedTimer all; all.start();
    for (qint64 i = 0; all.elapsed() < qint64(seconds * 1000.0); ++i) {
        const qint64 dueNs = qint64(double(i) * kFramePeriodS * 1e9);
        while (all.nsecsElapsed() < dueNs) QThread::usleep(1000);
        QElapsedTimer t; t.start();
        src.video(i, i * kFramePeriodS); const double v = t.nsecsElapsed() / 1e6; t.restart();
        src.audio(dst, i, i * kFramePeriodS); const double a = t.nsecsElapsed() / 1e6; t.restart();
        const QJsonObject st = src.status(); const double s = t.nsecsElapsed() / 1e6;
        vMs.push_back(v); aMs.push_back(a); sMs.push_back(s); calls += 3;
        over2 += (v > 2.0) + (a > 2.0) + (s > 2.0);
        detail = st.value("detail").toString();
    }
    src.close();
    const Dist v = dist(vMs), a = dist(aMs), s = dist(sMs);
    qInfo().noquote() << QString("%1 livesource_lock ndi(missing sender) %2 s: video max %3 ms p95 %4 avg %5  audio max %6 p95 %7  status max %8 p95 %9  calls>2ms %10/%11  (%12)")
        .arg(tag()).arg(seconds, 0, 'f', 1).arg(v.max, 0, 'f', 2).arg(v.p95, 0, 'f', 2).arg(v.avg, 0, 'f', 3)
        .arg(a.max, 0, 'f', 2).arg(a.p95, 0, 'f', 2).arg(s.max, 0, 'f', 2).arg(s.p95, 0, 'f', 2).arg(over2).arg(calls).arg(detail);
    QVERIFY(!vMs.empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// meters_contention: render while a UI reader polls the meters (PE-05 A/B)
// ---------------------------------------------------------------------------------------------------------------------

void BenchEngine::meters_contention() {
    const int frames = m_bench ? (qEnvironmentVariableIntValue("PLAYPLAY_BENCH_METER_FRAMES") > 0 ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_METER_FRAMES") : 300) : 30;
    const bool pace = m_bench && qEnvironmentVariable("PLAYPLAY_BENCH_PACE") != "0";
    // PLAYPLAY_BENCH_ROUNDS > 1 interleaves the four configurations (alternating which switch goes first) and prints
    // medians, so machine-load drift between runs does not read as a difference between the two code paths.
    const int rounds = m_bench ? qMax(1, qEnvironmentVariableIntValue("PLAYPLAY_BENCH_ROUNDS")) : 1;
    const std::vector<FramePtr> pics = makeProgramFrames(1920, 1080, 8);
    struct Cfg { bool legacy, withReader; std::vector<double> renderAvg, compositeAvg, audioAvg, lockMax; };
    std::vector<Cfg> cfgs = {{true, false, {}, {}, {}, {}}, {false, false, {}, {}, {}, {}}, {true, true, {}, {}, {}, {}}, {false, true, {}, {}, {}, {}}};
    for (int round = 0; round < rounds; ++round) for (int k = 0; k < 4; ++k) {
        Cfg& cfg = cfgs[size_t(round % 2 ? (k ^ 1) : k)];      // odd rounds: PE-05 before legacy
        const bool legacy = cfg.legacy, withReader = cfg.withReader;
        if (legacy) qputenv("PLAYPLAY_COMP_LEGACY_STATUS", "1"); else qunsetenv("PLAYPLAY_COMP_LEGACY_STATUS");
        OutputFormat fmt;
        Compositor comp(fmt);                     // the switch is read here
        qunsetenv("PLAYPLAY_COMP_LEGACY_STATUS");
        QCOMPARE(comp.legacyStatus(), legacy);
        comp.setMix("web", QJsonObject{{"name", "Web"}});
        comp.programTake(std::make_unique<CyclingSource>(pics), Compositor::Transition(), 0);
        const struct { int layer; const char* role; const char* kind; const char* anchor; double mx, my; bool mainOnly; } cg[] = {
            {30, "lower_third", "lower_third", "bottom_left", 120, 90, false}, {40, "ticker", "ticker", "bottom", 0, 0, true}, {45, "legal_id", "clock", "top_left", 60, 60, false}};
        for (const auto& c : cg) {
            auto src = std::make_unique<TemplateSource>(templateSpec(c.kind)); QVERIFY(src->open(fmt));
            QJsonObject spec{{"layer", c.layer}, {"role", c.role}, {"placement", QJsonObject{{"anchor", c.anchor}, {"marginX", c.mx}, {"marginY", c.my}}}};
            if (c.mainOnly) spec["mixes"] = QJsonArray{"main"};
            comp.addLayer(Compositor::LayerSpec::fromJson(spec), std::move(src), 0);
        }
        for (int i = 0; i < 4; ++i)
            QVERIFY(comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 60 + i}, {"role", "audio"},
                {"source", QJsonObject{{"type", "tone"}, {"toneHz", 300 + 100 * i}, {"toneDbfs", -24.0}, {"label", QString("Line %1").arg(i + 1)}, {"width", 2}, {"height", 2}}}}), 0));
        for (int i = 0; i < 30; ++i) { std::map<QString, MediaFrame> mx; comp.render(i, i * kFramePeriodS, &mx); }   // warm up
        std::atomic<bool> stop{false};
        std::atomic<int> reads{0};
        std::vector<double> readMs;             // reader thread only
        std::thread reader;
        if (withReader) reader = std::thread([&] {
            QElapsedTimer clock; clock.start();
            for (int i = 0; !stop.load(); ++i) {
                while (!stop.load() && clock.nsecsElapsed() < qint64(i) * 10000000LL) QThread::usleep(500);   // 100 Hz
                if (stop.load()) break;
                QElapsedTimer t; t.start();
                (void)comp.metersJson();
                if (i % 10 == 5) { (void)comp.status(); (void)comp.layersJson(); }
                readMs.push_back(t.nsecsElapsed() / 1e6);
                reads++;
            }
        });
        tickprof::TickProfiler prof(true);
        std::vector<double> ms;
        {
            tickprof::TickProfiler::Bind bind(prof);
            QElapsedTimer clock; clock.start();
            for (qint64 f = 30; f < 30 + frames; ++f) {
                if (pace) { const qint64 dueNs = qint64(double(f - 30) * kFramePeriodS * 1e9); while (clock.nsecsElapsed() < dueNs) QThread::usleep(1000); }
                prof.beginTick(f);
                QElapsedTimer t; t.start();
                std::map<QString, MediaFrame> mx;
                comp.render(f, f * kFramePeriodS, &mx);
                ms.push_back(t.nsecsElapsed() / 1e6);
                prof.endTick();
            }
        }
        stop = true;
        if (reader.joinable()) reader.join();
        const tickprof::Window w = prof.takeWindow();
        const Dist d = dist(ms), rd = dist(readMs);
        qInfo().noquote() << QString("%1 meters_contention %2 reader=%3: render avg %4 ms p95 %5 max %6 (composite %7, audio %8) | lock wait avg %9 max %10 ms | reads %11 avg %12 ms max %13 | frames %14%15")
            .arg(QString(tag()), QString(legacy ? "legacy" : "pe05  "), QString(withReader ? "100Hz" : "off  ")).arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2).arg(d.max, 0, 'f', 2)
            .arg(w.phaseAvgMs(tickprof::Composite), 0, 'f', 2).arg(w.phaseAvgMs(tickprof::Audio), 0, 'f', 3)
            .arg(w.phaseAvgMs(tickprof::Lock), 0, 'f', 4).arg(w.phases[tickprof::Lock].maxMs, 0, 'f', 3)
            .arg(reads.load()).arg(rd.avg, 0, 'f', 3).arg(rd.max, 0, 'f', 3).arg(frames).arg(pace ? "" : " (unpaced)") + (rounds > 1 ? QString(" round %1").arg(round + 1) : QString());
        if (withReader) QVERIFY(reads.load() > 0);
        cfg.renderAvg.push_back(d.avg); cfg.compositeAvg.push_back(w.phaseAvgMs(tickprof::Composite));
        cfg.audioAvg.push_back(w.phaseAvgMs(tickprof::Audio)); cfg.lockMax.push_back(w.phases[tickprof::Lock].maxMs);
    }
    if (rounds < 2) return;
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
    for (const Cfg& c : cfgs)
        qInfo().noquote() << QString("BENCH meters_contention median of %1 rounds %2 reader=%3: render avg %4 ms (composite %5, audio %6) | lock wait max %7 ms")
            .arg(rounds).arg(QString(c.legacy ? "legacy" : "pe05  "), QString(c.withReader ? "100Hz" : "off  ")).arg(median(c.renderAvg), 0, 'f', 2)
            .arg(median(c.compositeAvg), 0, 'f', 2).arg(median(c.audioAvg), 0, 'f', 3).arg(median(c.lockMax), 0, 'f', 3);
}

// ---------------------------------------------------------------------------------------------------------------------
// audio_pool: the compositor's audio phase with and without PE-13's pools (PLAYPLAY_AUDIO_POOL=0)
// ---------------------------------------------------------------------------------------------------------------------

/// A tiny raster (the picture costs next to nothing) with a tone program, 6 audio layers and 3 mixes at 29.97, pipelined,
/// with the last 8 ticks' frames held like an encoder queue holds them. Audio phase avg / p95 / max (PE-06 profiler) per
/// configuration, interleaved over PLAYPLAY_BENCH_ROUNDS (default 3) rounds; outputs of both configurations must match.
void BenchEngine::audio_pool() {
    const int frames = m_bench ? 3000 : 60;
    const int rounds = m_bench ? qMax(3, qEnvironmentVariableIntValue("PLAYPLAY_BENCH_ROUNDS")) : 1;
    struct Cfg { bool pool; std::vector<double> avg, p95, max; double checksum = 0; };
    std::vector<Cfg> cfgs = {{false, {}, {}, {}}, {true, {}, {}, {}}};
    for (int round = 0; round < rounds; ++round) for (int k = 0; k < 2; ++k) {
        Cfg& cfg = cfgs[size_t(round % 2 ? (k ^ 1) : k)];
        OutputFormat fmt; fmt.width = 64; fmt.height = 36;
        EnvScope env({{"PLAYPLAY_AUDIO_POOL", cfg.pool ? QByteArray() : QByteArray("0")}});
        Compositor comp(fmt);
        QCOMPARE(comp.audioPooled(), cfg.pool);
        comp.setPipelineDepth(2);
        comp.setMix("web", QJsonObject{{"name", "Web"}});
        comp.setMix("radio", QJsonObject{{"name", "Radio"}, {"strips", QJsonObject{{"L61", QJsonObject{{"mute", true}}}}}});
        QVERIFY(comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 10}, {"source", QJsonObject{{"type", "tone"}, {"toneHz", 1000}, {"toneDbfs", -20.0}, {"width", 2}, {"height", 2}}}}), 0));
        for (int i = 0; i < 6; ++i)
            QVERIFY(comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 60 + i}, {"role", "audio"}, {"duck", i == 0},
                {"source", QJsonObject{{"type", "tone"}, {"toneHz", 300 + 100 * i}, {"toneDbfs", -30.0}, {"width", 2}, {"height", 2}}}}), 0));
        std::deque<std::map<QString, MediaFrame>> queue;      // what an encoder queue keeps referenced
        tickprof::TickProfiler prof(true);
        std::vector<double> audioMs;
        double checksum = 0;
        {
            tickprof::TickProfiler::Bind bind(prof);
            for (qint64 f = 0; f < frames; ++f) {
                prof.beginTick(f);
                std::map<QString, MediaFrame> mx;
                MediaFrame m = comp.render(f, f * kFramePeriodS, &mx);
                prof.endTick();
                if (f % 30 == 29) { const tickprof::Window w = prof.takeWindow(); if (f >= (m_bench ? 60 : 0)) audioMs.push_back(w.phaseAvgMs(tickprof::Audio)); }
                mx["main"] = m;
                if (m.audio) checksum += double(reinterpret_cast<const float*>(m.audio->data[1])[m.audio->nb_samples / 2]);
                queue.push_back(std::move(mx)); if (queue.size() > 8) queue.pop_front();
            }
        }
        const Dist d = dist(audioMs);
        if (cfg.avg.empty()) cfg.checksum = checksum;
        QCOMPARE(checksum, cfg.checksum);
        qInfo().noquote() << QString("%1 audio_pool %2: audio phase avg %3 ms (1 s windows: p95 %4 max %5) | frames %6 checksum %7")
            .arg(QString(tag()), QString(cfg.pool ? "pool  " : "nopool")).arg(d.avg, 0, 'f', 4).arg(d.p95, 0, 'f', 4).arg(d.max, 0, 'f', 4)
            .arg(frames).arg(checksum, 0, 'g', 10) + (rounds > 1 ? QString(" round %1").arg(round + 1) : QString());
        cfg.avg.push_back(d.avg); cfg.p95.push_back(d.p95); cfg.max.push_back(d.max);
    }
    QCOMPARE(cfgs[0].checksum, cfgs[1].checksum);
    if (rounds < 2) return;
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
    for (const Cfg& c : cfgs)
        qInfo().noquote() << QString("BENCH audio_pool median of %1 rounds %2: audio phase avg %3 ms p95 %4 max %5")
            .arg(rounds).arg(QString(c.pool ? "pool  " : "nopool")).arg(median(c.avg), 0, 'f', 4).arg(median(c.p95), 0, 'f', 4).arg(median(c.max), 0, 'f', 4);
}

// ---------------------------------------------------------------------------------------------------------------------
// premul_ab: templates and an HTML-like page, straight vs premultiplied (PE-08 A/B)
// ---------------------------------------------------------------------------------------------------------------------

namespace {
/// An HTML layer as HtmlPage hands it out: full-raster pictures (a translucent panel with antialiased text), two of them
/// alternating every other frame like a 15 fps animation, decoded straight or premultiplied beforehand.
class PageLikeSource : public LayerSource {
public:
    PageLikeSource(std::vector<FramePtr> pics, bool premul) : m_pics(std::move(pics)), m_premul(premul) {}
    QString type() const override { return "html"; }
    bool open(const OutputFormat&) override { return !m_pics.empty(); }
    void close() override {}
    bool isOpen() const override { return !m_pics.empty(); }
    FramePtr video(qint64 frame, double) override { return m_pics[size_t(frame / 2) % m_pics.size()]; }
    bool deliversPremultiplied() const override { return m_premul; }
private:
    std::vector<FramePtr> m_pics;
    bool m_premul;
};

/// Picture `k` of the page, straight ARGB32 (what a PNG with alpha decodes to).
QImage pagePicture(int w, int h, int k) {
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
    QPainter p(&img); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::TextAntialiasing);
    const QRectF panel(w * 0.06 + k * 6, h * 0.62, w * 0.55, h * 0.16);
    QPainterPath path; path.addRoundedRect(panel, 18, 18); p.fillPath(path, QColor(10, 20, 60, 200));
    p.fillRect(QRectF(panel.left(), panel.top(), 12, panel.height()), QColor(255, 180, 0, 230));
    QFont f("Segoe UI", -1, QFont::Bold); f.setPixelSize(int(h * 0.06)); p.setFont(f); p.setPen(QColor(255, 255, 255, 235));
    p.drawText(panel.adjusted(40, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, QString("HTML lower third %1").arg(k));
    p.end();
    return img.convertToFormat(QImage::Format_ARGB32);
}
} // namespace

void BenchEngine::premul_ab() {
    const int frames = m_bench ? (qEnvironmentVariableIntValue("PLAYPLAY_BENCH_PREMUL_FRAMES") > 0 ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_PREMUL_FRAMES") : 300) : 20;
    const bool pace = m_bench && qEnvironmentVariable("PLAYPLAY_BENCH_PACE") != "0";
    const int rounds = m_bench ? qMax(1, qEnvironmentVariableIntValue("PLAYPLAY_BENCH_ROUNDS")) : 1;
    OutputFormat fmt;
    const std::vector<FramePtr> pics = makeProgramFrames(fmt.width, fmt.height, 8);
    // The page in both conventions. The premultiply (in place, as HtmlPage's decode thread now does it once per new
    // picture, off the clock thread) is timed on its own.
    std::vector<FramePtr> straightPages, premulPages;
    std::vector<double> convMs;
    for (int k = 0; k < 2; ++k) {
        const QImage page = pagePicture(fmt.width, fmt.height, k);
        straightPages.push_back(Mixer::imageToFrame(page));
        QImage pm;
        for (int rep = 0; rep < 5; ++rep) {
            pm = page.copy();
            QElapsedTimer t; t.start();
            pm.convertTo(QImage::Format_ARGB32_Premultiplied);
            convMs.push_back(t.nsecsElapsed() / 1e6);
        }
        premulPages.push_back(Mixer::imageToFramePremultiplied(pm));
    }
    struct Cfg { const char* name; const char* env; std::vector<double> tick, layers, composite, cpu; QImage last; };
    std::vector<Cfg> cfgs = {{"straight", "0", {}, {}, {}, {}, {}}, {"premul  ", "1", {}, {}, {}, {}, {}}};
    for (int round = 0; round < rounds; ++round) for (int k = 0; k < 2; ++k) {
        Cfg& cfg = cfgs[size_t(round % 2 ? 1 - k : k)];         // odd rounds: premultiplied first
        const bool premul = cfg.env[0] == '1';
        EnvScope env({{"PLAYPLAY_PREMUL", QByteArray(cfg.env)}});
        Compositor comp(fmt);                                   // CPU path, as composite_cg
        comp.programTake(std::make_unique<CyclingSource>(pics), Compositor::Transition(), 0);
        const struct { int layer; const char* role; const char* kind; const char* anchor; double mx, my; } cg[] = {
            {30, "lower_third", "lower_third", "bottom_left", 120, 90}, {40, "ticker", "ticker", "bottom", 0, 0}, {45, "legal_id", "countdown", "top_left", 60, 60}};
        for (const auto& c : cg) {
            auto src = std::make_unique<TemplateSource>(templateSpec(c.kind)); QVERIFY(src->open(fmt));
            QCOMPARE(src->deliversPremultiplied(), premul);
            comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"layer", c.layer}, {"role", c.role},
                {"placement", QJsonObject{{"anchor", c.anchor}, {"marginX", c.mx}, {"marginY", c.my}}}}), std::move(src), 0);
        }
        comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"layer", 50}, {"role", "html"}}),
                      std::make_unique<PageLikeSource>(premul ? premulPages : straightPages, premul), 0);
        for (int i = 0; i < 30; ++i) comp.render(i, i * kFramePeriodS);    // warm up
        tickprof::TickProfiler prof(true);
        std::vector<double> ms;
        MediaFrame last;
        const double c0 = cpuSeconds();
        {
            tickprof::TickProfiler::Bind bind(prof);
            QElapsedTimer clock; clock.start();
            for (qint64 f = 30; f < 30 + frames; ++f) {
                if (pace) { const qint64 dueNs = qint64(double(f - 30) * kFramePeriodS * 1e9); while (clock.nsecsElapsed() < dueNs) QThread::usleep(1000); }
                prof.beginTick(f);
                QElapsedTimer t; t.start();
                last = comp.render(f, f * kFramePeriodS);
                ms.push_back(t.nsecsElapsed() / 1e6);
                prof.endTick();
            }
        }
        const double cpuMs = (cpuSeconds() - c0) * 1000.0 / frames;
        QVERIFY(last.video);
        const tickprof::Window w = prof.takeWindow();
        const Dist d = dist(ms);
        qInfo().noquote() << QString("%1 premul_ab %2 program+L3+ticker+countdown+html page: tick avg %3 ms p95 %4 max %5 | layers %6 | composite %7 | CPU %8 ms/frame | frames %9%10")
            .arg(QString(tag()), QString(cfg.name)).arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2).arg(d.max, 0, 'f', 2)
            .arg(w.phaseAvgMs(tickprof::Layers), 0, 'f', 3).arg(w.phaseAvgMs(tickprof::Composite), 0, 'f', 2).arg(cpuMs, 0, 'f', 2)
            .arg(frames).arg(pace ? "" : " (unpaced)") + (rounds > 1 ? QString(" round %1").arg(round + 1) : QString());
        cfg.tick.push_back(d.avg); cfg.layers.push_back(w.phaseAvgMs(tickprof::Layers)); cfg.composite.push_back(w.phaseAvgMs(tickprof::Composite)); cfg.cpu.push_back(cpuMs);
        cfg.last = QImage(last.video->data[0], last.video->width, last.video->height, last.video->linesize[0], QImage::Format_ARGB32).copy();
    }
    // The same frame number in both modes (every source is a function of the frame): how far apart the pictures are.
    int maxDiff = 0; qint64 differing = 0;
    const QImage& a = cfgs[0].last; const QImage& b = cfgs[1].last;
    QCOMPARE(a.size(), b.size());
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y)); const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) if (pa[x] != pb[x]) {
            ++differing;
            maxDiff = qMax(maxDiff, qMax(qMax(std::abs(qRed(pa[x]) - qRed(pb[x])), std::abs(qGreen(pa[x]) - qGreen(pb[x]))), std::abs(qBlue(pa[x]) - qBlue(pb[x]))));
        }
    }
    const Dist cd = dist(convMs);
    qInfo().noquote() << QString("%1 premul_ab outputs of frame %2: %3 pixels differ, max %4 per channel | decode-thread premultiply of a 1080p page picture: avg %5 ms max %6")
        .arg(tag()).arg(29 + frames).arg(differing).arg(maxDiff).arg(cd.avg, 0, 'f', 2).arg(cd.max, 0, 'f', 2);
    // Draw cost alone, the way renderCpu draws an overlay (premultiplied canvas, SmoothPixmapTransform, source-over):
    // the page's visible box and a ticker-sized strip, straight vs premultiplied source, interleaved.
    {
        FramePtr canvasFrame = allocVideoFrame(fmt.width, fmt.height, kMixerPixelFormat);
        QImage canvas(canvasFrame->data[0], fmt.width, fmt.height, canvasFrame->linesize[0], QImage::Format_ARGB32_Premultiplied);
        const QRect box(int(fmt.width * 0.06), int(fmt.height * 0.62), int(fmt.width * 0.55) + 8, int(fmt.height * 0.16) + 2);
        const int reps = m_bench ? 400 : 20;
        for (const char* what : {"page box", "full page"}) {
            const QRect sr = QString(what) == "full page" ? QRect(0, 0, fmt.width, fmt.height) : box;
            std::vector<double> sMs, pMs;
            for (int i = 0; i < reps; ++i) for (int mode = 0; mode < 2; ++mode) {
                const bool pm = (mode ^ (i & 1)) != 0;
                const FramePtr& f = pm ? premulPages[0] : straightPages[0];
                const QImage src(f->data[0], f->width, f->height, f->linesize[0], pm ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32);
                canvas.fill(QColor(40, 90, 200));
                QElapsedTimer t; t.start();
                QPainter p(&canvas);
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.setCompositionMode(QPainter::CompositionMode_SourceOver);
                p.drawImage(QRectF(sr), src, QRectF(sr));
                p.end();
                (pm ? pMs : sMs).push_back(t.nsecsElapsed() / 1e6);
            }
            const Dist ds = dist(sMs), dp = dist(pMs);
            qInfo().noquote() << QString("%1 premul_ab draw %2 %3x%4: straight avg %5 ms p95 %6 | premultiplied avg %7 ms p95 %8 (%9 draws each)")
                .arg(tag(), what).arg(sr.width()).arg(sr.height()).arg(ds.avg, 0, 'f', 3).arg(ds.p95, 0, 'f', 3).arg(dp.avg, 0, 'f', 3).arg(dp.p95, 0, 'f', 3).arg(reps);
        }
    }
    QVERIFY2(maxDiff <= 2, "premultiplied output more than 2 off the straight one");
    if (rounds < 2) return;
    auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0 : v[v.size() / 2]; };
    for (const Cfg& c : cfgs)
        qInfo().noquote() << QString("BENCH premul_ab median of %1 rounds %2: tick avg %3 ms | layers %4 | composite %5 | CPU %6 ms/frame")
            .arg(rounds).arg(QString(c.name)).arg(median(c.tick), 0, 'f', 2).arg(median(c.layers), 0, 'f', 3).arg(median(c.composite), 0, 'f', 2).arg(median(c.cpu), 0, 'f', 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// PE-11: NDI send FourCC (option / env), UYVY conversion colours, and a send -> receive round trip
// ---------------------------------------------------------------------------------------------------------------------

namespace {

/// 16 opaque BGRA bars: 100 % then 75 % white, yellow, cyan, green, magenta, red, blue, black.
struct Bar { int r, g, b; };
std::vector<Bar> barColours() {
    std::vector<Bar> v;
    for (int level : {255, 191}) {
        const int L = level;
        for (const Bar b : {Bar{L, L, L}, Bar{L, L, 0}, Bar{0, L, L}, Bar{0, L, 0}, Bar{L, 0, L}, Bar{L, 0, 0}, Bar{0, 0, L}, Bar{0, 0, 0}}) v.push_back(b);
    }
    return v;
}

FramePtr makeBars(int w, int h) {
    const std::vector<Bar> bars = barColours();
    FramePtr f = allocVideoFrame(w, h, AV_PIX_FMT_BGRA);
    if (!f) return f;
    const int n = int(bars.size());
    for (int y = 0; y < h; ++y) {
        auto* row = f->data[0] + size_t(y) * f->linesize[0];
        for (int x = 0; x < w; ++x) {
            const Bar& b = bars[size_t(qMin(n - 1, x * n / w))];
            row[x * 4 + 0] = uint8_t(b.b); row[x * 4 + 1] = uint8_t(b.g); row[x * 4 + 2] = uint8_t(b.r); row[x * 4 + 3] = 0xFF;
        }
    }
    return f;
}

/// Even x in the middle of bar `i` (chroma there is the bar's own, whatever the horizontal filter).
int barCentreX(int i, int w, int n) { return ((2 * i + 1) * w / (2 * n)) & ~1; }

} // namespace

void BenchEngine::ndi_fourcc_option() {
    EnvScope env({{"PLAYPLAY_NDI_FOURCC", QByteArray()}});
    bool ok = false;
    QCOMPARE(NdiSink::parseFourcc(" UYVY ", &ok), NdiSink::VideoFourcc::Uyvy); QVERIFY(ok);
    QCOMPARE(NdiSink::parseFourcc("bgra", &ok), NdiSink::VideoFourcc::Bgra); QVERIFY(ok);
    QCOMPARE(NdiSink::parseFourcc("nv12", &ok), NdiSink::VideoFourcc::Bgrx); QVERIFY(!ok);
    NdiSink a("perftest-opt");
    QCOMPARE(a.fourcc(), NdiSink::VideoFourcc::Bgrx);                 // the PE-11 default
    a.setFourcc("uyvy");  QCOMPARE(a.fourcc(), NdiSink::VideoFourcc::Uyvy);
    a.setFourcc("");      QCOMPARE(a.fourcc(), NdiSink::VideoFourcc::Bgrx);
    a.setFourcc("bogus"); QCOMPARE(a.fourcc(), NdiSink::VideoFourcc::Bgrx);
    {
        EnvScope e2({{"PLAYPLAY_NDI_FOURCC", "bgra"}});                // the station-wide off switch
        NdiSink b("perftest-opt2");
        QCOMPARE(b.fourcc(), NdiSink::VideoFourcc::Bgra);
        b.setFourcc("");      QCOMPARE(b.fourcc(), NdiSink::VideoFourcc::Bgra);
        b.setFourcc("uyvy");  QCOMPARE(b.fourcc(), NdiSink::VideoFourcc::Uyvy);   // the output's option wins
    }
}

void BenchEngine::ndi_uyvy_colours_data() {
    QTest::addColumn<int>("w");
    QTest::addColumn<int>("h");
    QTest::newRow("1080 bt709") << 1920 << 1080;
    QTest::newRow("720 bt709") << 1280 << 720;
    QTest::newRow("480 bt601") << 720 << 480;
}

void BenchEngine::ndi_uyvy_colours() {
    QFETCH(int, w); QFETCH(int, h);
    const FramePtr bars = makeBars(w, h);
    QVERIFY(bars);
    NdiUyvyConverter conv;
    FramePtr u = conv.convert(bars.get());
    QVERIFY(u);
    QCOMPARE(u->format, int(AV_PIX_FMT_UYVY422));
    QCOMPARE(u->width, w); QCOMPARE(u->height, h);
    const bool bt709 = h >= 720;
    const double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1.0 - kr - kb;
    const std::vector<Bar> cols = barColours();
    const int n = int(cols.size());
    int worst = 0;
    for (int row : {0, h / 2, h - 1}) {
        const uint8_t* line = u->data[0] + size_t(row) * u->linesize[0];
        for (int i = 0; i < n; ++i) {
            const Bar c = cols[size_t(i)];
            const double R = c.r / 255.0, G = c.g / 255.0, B = c.b / 255.0, Y = kr * R + kg * G + kb * B;
            const int ey = int(std::lround(16.0 + 219.0 * Y));
            const int eu = int(std::lround(128.0 + 224.0 * (B - Y) / (2.0 * (1.0 - kb))));
            const int ev = int(std::lround(128.0 + 224.0 * (R - Y) / (2.0 * (1.0 - kr))));
            const uint8_t* px = line + size_t(barCentreX(i, w, n)) * 2;    // U Y0 V Y1
            const int du = std::abs(px[0] - eu), dy0 = std::abs(px[1] - ey), dv = std::abs(px[2] - ev), dy1 = std::abs(px[3] - ey);
            worst = std::max({worst, du, dy0, dv, dy1});
            if (std::max({du, dy0, dv, dy1}) > 1)
                QFAIL(qPrintable(QString("%1x%2 row %3 bar %4 (%5,%6,%7): got Y %8/%9 Cb %10 Cr %11, want Y %12 Cb %13 Cr %14")
                    .arg(w).arg(h).arg(row).arg(i).arg(c.r).arg(c.g).arg(c.b).arg(px[1]).arg(px[3]).arg(px[0]).arg(px[2]).arg(ey).arg(eu).arg(ev)));
        }
    }
    // Limited range: nothing below 16 / above 235 (luma) or 240 (chroma) anywhere in the picture.
    int minY = 255, maxY = 0, minC = 255, maxC = 0;
    for (int y = 0; y < h; y += 7) {
        const uint8_t* line = u->data[0] + size_t(y) * u->linesize[0];
        for (int x = 0; x < w * 2; x += 2) { minC = qMin(minC, int(line[x])); maxC = qMax(maxC, int(line[x])); minY = qMin(minY, int(line[x + 1])); maxY = qMax(maxY, int(line[x + 1])); }
    }
    QVERIFY2(minY >= 16 && maxY <= 235 && minC >= 16 && maxC <= 240, qPrintable(QString("range Y %1..%2 C %3..%4").arg(minY).arg(maxY).arg(minC).arg(maxC)));

    // Ring: a picture NDI may still read (held, like the sink's m_inFlight) is never handed out again.
    FramePtr u2 = conv.convert(bars.get());
    QVERIFY(u2 && u2.get() != u.get());
    FramePtr u3 = conv.convert(bars.get());
    QVERIFY(u3 && u3.get() != u.get() && u3.get() != u2.get());
    QVERIFY(!conv.convert(bars.get()));                 // all three held: refuse rather than overwrite
    u.reset();
    FramePtr u4 = conv.convert(bars.get());
    QVERIFY(u4 && u4.get() != u2.get() && u4.get() != u3.get());
    // Odd widths / other formats are refused (the sink then sends BGRX).
    const FramePtr odd = allocVideoFrame(w - 1, h, AV_PIX_FMT_BGRA);
    QVERIFY(!conv.convert(odd.get()));
    u2.reset(); u3.reset(); u4.reset();
    // Timing on a 1080 raster (smoke numbers; the CPU A/B is ndi_send).
    double ms = 0; const int reps = m_bench ? 100 : 10;
    for (int i = 0; i < reps; ++i) { FramePtr t = conv.convert(bars.get()); QVERIFY(t); ms += conv.lastMs(); }
    qInfo().noquote() << QString("%1 ndi_uyvy_colours %2x%3 %4: worst bar error %5 code(s), Y %6..%7 C %8..%9, convert avg %10 ms (%11 sws threads)")
        .arg(tag()).arg(w).arg(h).arg(bt709 ? "bt709" : "bt601").arg(worst).arg(minY).arg(maxY).arg(minC).arg(maxC)
        .arg(ms / reps, 0, 'f', 2).arg(conv.threads());
}

void BenchEngine::ndi_roundtrip_data() {
    QTest::addColumn<QString>("fourcc");
    for (const char* f : {"uyvy", "bgrx", "bgra"}) QTest::newRow(f) << QString(f);
}

void BenchEngine::ndi_roundtrip() {
    QFETCH(QString, fourcc);
    if (qEnvironmentVariable("PLAYPLAY_BENCH_NDI") != "1") QSKIP("ndi_roundtrip: set PLAYPLAY_BENCH_NDI=1 (announces NDI sender perftest-ndi)");
    if (!NdiSink::isAvailable() || !NdiSource::isAvailable()) QSKIP("NDI not compiled in");
    const QString sender = "perftest-ndi";
    QVERIFY(sender.startsWith("perftest-"));    // never a live name (main / ch2)
    EnvScope env({{"PLAYPLAY_NDI_FOURCC", QByteArray()}});
    const int w = 1920, h = 1080;
    const FramePtr bars = makeBars(w, h);
    NdiSink sink(sender, sender);
    sink.setFourcc(fourcc);
    BasebandSink::Format f;
    QVERIFY2(sink.start(f), qPrintable(sink.stats().lastError));
    OutputFormat ofmt;
    // NDI names are "HOST (perftest-ndi)": the brackets keep "perftest-ndi-x" senders from matching.
    NdiSource src(QJsonObject{{"type", "ndi"}, {"name", "(" + sender + ")"}, {"recvName", "perftest-ndi-rx"}});
    if (!src.open(ofmt)) { sink.stop(); QSKIP(qPrintable("NDI runtime not usable: " + src.lastError())); }
    FramePtr got;
    QElapsedTimer all; all.start();
    double phase = 0;
    int received = 0, audibleBlocks = 0;
    for (qint64 i = 0; all.elapsed() < 15000; ++i) {
        const qint64 dueNs = qint64(double(i) * kFramePeriodS * 1e9);
        while (all.nsecsElapsed() < dueNs) QThread::usleep(1000);
        MediaFrame mf; mf.frameNumber = i; mf.video = bars; mf.audio = makeAudio(i, phase);
        sink.pushFrame(mf);
        FramePtr sound = allocAudioFrame(samplesForFrame(i), 2, 48000);
        silenceAudioFrame(sound);
        if (src.audio(sound, i, i * kFramePeriodS)) {
            const float* samples = reinterpret_cast<const float*>(sound->data[0]);
            for (int j = 0; j < sound->nb_samples; ++j) if (std::abs(samples[j]) > 0.001f) { ++audibleBlocks; break; }
        }
        if (FramePtr v = src.video(i, i * kFramePeriodS)) { got = v; ++received; }
        if (received >= 20 && audibleBlocks >= 10) break;
    }
    const SinkStats ss = sink.stats();
    src.close();
    sink.stop();
    QVERIFY2(got, "no picture came back from perftest-ndi within 15 s");
    QVERIFY2(audibleBlocks >= 10, qPrintable(QString("NDI audio did not return reliably: %1 audible blocks").arg(audibleBlocks)));
    QCOMPARE(got->width, w); QCOMPARE(got->height, h);
    QCOMPARE(ss.extra.value("fourcc").toString(), fourcc);
    const std::vector<Bar> cols = barColours();
    const int n = int(cols.size());
    int worst = 0, alphaMin = 255;
    QString detail;
    for (int i = 0; i < n; ++i) {
        const Bar c = cols[size_t(i)];
        const int x = barCentreX(i, w, n);
        const uint8_t* px = got->data[0] + size_t(h / 2) * got->linesize[0] + size_t(x) * 4;
        const int e = std::max({std::abs(px[2] - c.r), std::abs(px[1] - c.g), std::abs(px[0] - c.b)});
        worst = qMax(worst, e); alphaMin = qMin(alphaMin, int(px[3]));
        if (e > 6) detail += QString(" bar%1 sent(%2,%3,%4) got(%5,%6,%7)").arg(i).arg(c.r).arg(c.g).arg(c.b).arg(px[2]).arg(px[1]).arg(px[0]);
    }
    qInfo().noquote() << QString("%1 ndi_roundtrip %2: sent as %3, %4 pictures back, worst bar error %5 (RGB codes), alpha min %6%7")
        .arg(tag(), fourcc, ss.extra.value("fourcc").toString()).arg(received).arg(worst).arg(alphaMin).arg(detail);
    // A wrong matrix (601 vs 709) or range (full vs limited) moves saturated bars by 20+ codes; NDI's codec costs a few.
    QVERIFY2(worst <= 6, qPrintable("bars came back off:" + detail));
    QCOMPARE(alphaMin, 255);
}

// Read-only opt-in inspection of one named input; never sends or changes a channel.
void BenchEngine::ndi_input_probe() {
    const QString name = qEnvironmentVariable("PLAYPLAY_TEST_NDI_SOURCE");
    if (name.isEmpty()) QSKIP("Set PLAYPLAY_TEST_NDI_SOURCE to inspect an input");
    OutputFormat fmt;
    NdiSource source(QJsonObject{{"type", "ndi"}, {"name", name}, {"recvName", "PlayPlay diagnostic"}});
    QVERIFY2(source.open(fmt), qPrintable(source.lastError()));
    QElapsedTimer timer; timer.start();
    double peak = 0; int blocks = 0; qint64 frame = 0;
    while (timer.elapsed() < 6000) {
        FramePtr sound = allocAudioFrame(samplesForFrame(frame), 2, 48000);
        silenceAudioFrame(sound);
        if (source.audio(sound, frame, frame * kFramePeriodS)) {
            ++blocks;
            for (int c = 0; c < 2; ++c) { const float* p = reinterpret_cast<const float*>(sound->data[c]); for (int i = 0; i < sound->nb_samples; ++i) peak = qMax(peak, double(std::abs(p[i]))); }
        }
        ++frame;
        QThread::msleep(33);
    }
    qInfo().noquote() << "NDI input diagnostic" << name << "blocks" << blocks << "output peak" << peak << "status" << QJsonDocument(source.status()).toJson(QJsonDocument::Compact);
    source.close();
}

// ---------------------------------------------------------------------------------------------------------------------
// ndi_send: NDI program sender cost per FourCC (opt-in: it announces "perftest-bench" on the network)
// ---------------------------------------------------------------------------------------------------------------------

void BenchEngine::ndi_send() {
    if (!m_bench || qEnvironmentVariable("PLAYPLAY_BENCH_NDI") != "1") QSKIP("ndi_send: set PLAYPLAY_BENCH=1 and PLAYPLAY_BENCH_NDI=1 (announces NDI sender perftest-bench)");
    if (!NdiSink::isAvailable()) QSKIP("NDI not compiled in");
    const QString sender = "perftest-bench";
    QVERIFY(sender.startsWith("perftest-"));    // never a live name (main / ch2)
    const std::vector<FramePtr> pics = makeProgramFrames(1920, 1080, 8);
    const QByteArray savedFourcc = qgetenv("PLAYPLAY_NDI_FOURCC");
    for (const char* fourcc : {"bgra", "bgrx", "uyvy"}) {
        qputenv("PLAYPLAY_NDI_FOURCC", fourcc);     // honoured by NdiSink from PE-11 on; before that every row sends BGRA
        NdiSink sink(sender, sender);
        BasebandSink::Format f;
        QVERIFY2(sink.start(f), qPrintable(sink.stats().lastError));
        double phase = 0.0;
        const int frames = 150;
        const double c0 = cpuSeconds();
        QElapsedTimer clock; clock.start();
        for (qint64 i = 0; i < frames; ++i) {
            const qint64 dueNs = qint64(double(i) * kFramePeriodS * 1e9);
            while (clock.nsecsElapsed() < dueNs) QThread::usleep(1000);
            MediaFrame mf; mf.frameNumber = i; mf.video = pics[size_t(i) % pics.size()]; mf.audio = makeAudio(i, phase);
            sink.pushFrame(mf);
        }
        QThread::msleep(100);
        const double cores = (cpuSeconds() - c0) / qMax(0.001, clock.nsecsElapsed() / 1e9);
        const SinkStats ss = sink.stats();
        sink.stop();
        const QString sent = ss.extra.value("fourcc").toString();
        qInfo().noquote() << QString("BENCH ndi_send %1 requested=%2 sent=%3: CPU %4 cores  out %5/%6  dropped %7")
            .arg(sender, fourcc, sent.isEmpty() ? QString("bgra (sink reports no fourcc)") : sent).arg(cores, 0, 'f', 2)
            .arg(ss.framesOut).arg(frames).arg(ss.dropped);
    }
    if (savedFourcc.isNull()) qunsetenv("PLAYPLAY_NDI_FOURCC"); else qputenv("PLAYPLAY_NDI_FOURCC", savedFourcc);
}

QTEST_MAIN(BenchEngine)
// ---------------------------------------------------------------------------------------------------------------------
// decode_rate (PE-12)

void BenchEngine::decode_rate() {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) QSKIP("ffmpeg not on PATH");
    QVERIFY(m_dir.isValid());
    const int w = m_bench ? 1920 : 640, h = m_bench ? 1080 : 360;
    const int frames = m_bench ? qMax(60, qEnvironmentVariableIntValue("PLAYPLAY_BENCH_DECODE_FRAMES") > 0 ? qEnvironmentVariableIntValue("PLAYPLAY_BENCH_DECODE_FRAMES") : 150) : 20;
    const double clipSec = frames / 29.97 + 1.5;
    const QString clip = m_dir.filePath("decode5994.mp4");
    {
        QProcess p;
        p.start(ffmpeg, {"-y", "-v", "error", "-f", "lavfi", "-i", QString("testsrc2=s=%1x%2:r=60000/1001:d=%3").arg(w).arg(h).arg(clipSec, 0, 'f', 1),
                         "-c:v", "libx264", "-preset", "veryfast", "-b:v", m_bench ? "16M" : "2M", "-pix_fmt", "yuv420p", clip});
        QVERIFY(p.waitForFinished(120000));
        QVERIFY2(QFileInfo::exists(clip), qPrintable(p.readAllStandardError()));
    }
    struct Row { const char* name; bool hw; const char* skip; const char* pool; };
    std::vector<Row> rows = { {"sw skip", false, "1", "1"}, {"sw noskip", false, "0", "1"},
                              {"hw skip pool", true, "1", "1"}, {"hw noskip pool", true, "0", "1"}, {"hw skip nopool", true, "1", "0"} };
    if (!m_bench) rows.resize(2);
    for (const Row& r : rows) {
        qputenv("PLAYPLAY_DECODE_SKIP", r.skip);
        qputenv("PLAYPLAY_DECODE_POOL", r.pool);
        DecoderPipeline::Config dc;
        dc.outWidth = w; dc.outHeight = h; dc.outRate = FrameRate::ntsc(); dc.hwAccel = r.hw;
        DecoderPipeline dec(dc);
        qunsetenv("PLAYPLAY_DECODE_SKIP"); qunsetenv("PLAYPLAY_DECODE_POOL");
        if (!dec.open(clip)) { qWarning() << "decode_rate: open failed" << dec.lastError(); continue; }
        if (r.hw && !dec.stats().hwAccelActive) { qInfo().noquote() << QString("%1 decode_rate %2: no hw decoder, row skipped").arg(tag()).arg(r.name); dec.close(); continue; }
        QTest::qWait(m_bench ? 300 : 50);
        const double c0 = cpuSeconds();
        QElapsedTimer t; t.start();
        for (int i = 0; i < frames; ++i) {
            QVERIFY(dec.videoFrameAt(dc.outRate.frameToNs(i) / 1e9));
            if (m_bench) { const qint64 due = qint64((i + 1) * 1001.0 / 30.0); while (t.elapsed() < due) QThread::usleep(500); }
            else { QElapsedTimer wt; wt.start(); while (dec.stats().decodedVideo < 2 * i + 5 && !dec.decoderFinished() && wt.elapsed() < 500) QThread::msleep(1); }
        }
        const double cores = (cpuSeconds() - c0) / qMax(0.001, t.elapsed() / 1000.0);
        const auto st = dec.stats();
        qInfo().noquote() << QString("%1 decode_rate %2: hw=%3 CPU %4 cores | decoded %5 converted %6 skippedConvert %7 dropped %8 repeated %9 | download %10 ms convert %11 ms | dlAllocs %12 pooled %13 skipActive %14")
            .arg(tag()).arg(r.name).arg(st.hwAccelActive ? st.hwDevice : "-").arg(cores, 0, 'f', 2)
            .arg(st.decodedVideo).arg(st.convertedVideo).arg(st.skippedConvert).arg(st.droppedVideo).arg(st.repeatedVideo)
            .arg(st.transferMs, 0, 'f', 2).arg(st.convertMs, 0, 'f', 2).arg(st.downloadAllocs).arg(st.downloadPooled).arg(st.skipActive);
        QVERIFY(st.convertedVideo > 0);
        if (QString(r.skip) == "1") { QVERIFY(st.skipActive); QVERIFY(st.convertedVideo <= st.decodedVideo * 0.7); }
        else { QVERIFY(!st.skipActive); QCOMPARE(st.skippedConvert, 0LL); }
        dec.close();
    }
}

#include "bench_engine.moc"
