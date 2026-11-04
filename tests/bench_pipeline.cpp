#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <map>
#include <cmath>
#include <algorithm>
#include "playout/engine/decoderpipeline.h"
#include "playout/engine/compositor/compositor.h"
#include "playout/engine/mediaframe.h"
extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}
#ifdef _WIN32
#include <windows.h>
#endif

/**
 * Performance benchmark for the playout pipeline at the live raster (1920x1080 @ 29.97).
 * Not a pass/fail test: it prints numbers (ms per frame, CPU cores) so changes can be compared.
 * Runs only with PLAYPLAY_BENCH=1 (ctest skips it otherwise).
 *
 *   decode     1080p H.264 at real-time pace, software vs hardware (d3d11va): CPU cores used,
 *              download (GPU->CPU) and conversion ms per picture
 *   composite  program (a new picture every frame) + full-raster lower third + bug, CPU vs GPU:
 *              ms per tick on the clock thread (avg / p95 / max)
 *   convert    BGRA -> YUV420P for the encoders, single thread vs threaded swscale
 */
class BenchPipeline : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void decode();
    void composite();
    void composite_data();
    void convert();
private:
    QString m_ffmpeg, m_clip;
    QTemporaryDir m_dir;
};

static double cpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto toS = [](FILETIME f) { ULARGE_INTEGER x; x.LowPart = f.dwLowDateTime; x.HighPart = f.dwHighDateTime; return double(x.QuadPart) / 1e7; };
    return toS(k) + toS(u);
#else
    return 0.0;
#endif
}

struct Dist { double avg = 0, p95 = 0, max = 0; };
static Dist dist(std::vector<double> v) {
    Dist d; if (v.empty()) return d;
    double s = 0; for (double x : v) s += x; d.avg = s / v.size();
    std::sort(v.begin(), v.end()); d.p95 = v[size_t(v.size() * 0.95)]; d.max = v.back();
    return d;
}

void BenchPipeline::initTestCase() {
    if (qEnvironmentVariable("PLAYPLAY_BENCH") != "1") QSKIP("benchmark: set PLAYPLAY_BENCH=1");
    m_ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg not on PATH");
    m_clip = m_dir.filePath("bench1080.mp4");
    QProcess p;
    p.start(m_ffmpeg, {"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30000/1001:d=12",
                       "-f", "lavfi", "-i", "sine=f=440:r=48000:d=12", "-c:v", "libx264", "-preset", "veryfast", "-b:v", "12M",
                       "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", m_clip});
    QVERIFY(p.waitForFinished(120000));
    QVERIFY(QFileInfo::exists(m_clip));
}

void BenchPipeline::decode() {
    for (bool hw : {false, true}) {
        DecoderPipeline::Config dc;
        dc.outWidth = 1920; dc.outHeight = 1080; dc.outRate = FrameRate::ntsc(); dc.hwAccel = hw;
        DecoderPipeline dec(dc);
        QVERIFY(dec.open(m_clip));
        QTest::qWait(300);
        const double c0 = cpuSeconds();
        QElapsedTimer t; t.start();
        const int frames = 240;   // 8 s at real-time pace, like playout
        for (int i = 0; i < frames; ++i) {
            dec.videoFrameAt(i * 1001.0 / 30000.0);
            const qint64 due = qint64((i + 1) * 1001.0 / 30.0);
            while (t.elapsed() < due) QThread::usleep(500);
        }
        const double cores = (cpuSeconds() - c0) / (t.elapsed() / 1000.0);
        const auto st = dec.stats();
        qInfo().noquote() << QString("BENCH decode %1: hw=%2  CPU %3 cores  download %4 ms  convert %5 ms  dropped %6 repeated %7")
            .arg(hw ? "hw " : "sw ").arg(st.hwAccelActive ? st.hwDevice : "-").arg(cores, 0, 'f', 2)
            .arg(st.transferMs, 0, 'f', 2).arg(st.convertMs, 0, 'f', 2).arg(st.droppedVideo).arg(st.repeatedVideo);
        dec.close();
    }
}

namespace {
/// Program source that hands out a different picture every frame (as a decoder does), so nothing can be cached.
class CyclingSource : public LayerSource {
public:
    QString type() const override { return "bench"; }
    bool open(const OutputFormat& fmt) override {
        for (int k = 0; k < 16; ++k) {   // more distinct pictures than the GPU upload cache holds: every frame is uploaded
            FramePtr f = allocVideoFrame(fmt.width, fmt.height, AV_PIX_FMT_BGRA);
            for (int y = 0; y < fmt.height; ++y) { auto* r = reinterpret_cast<quint32*>(f->data[0] + y * f->linesize[0]); for (int x = 0; x < fmt.width; ++x) r[x] = 0xFF000000u | quint32((x + k * 40) & 255) << 16 | quint32(y & 255) << 8 | quint32((k * 15) & 255); }
            m_frames.push_back(f);
        }
        return true;
    }
    void close() override { m_frames.clear(); }
    bool isOpen() const override { return !m_frames.empty(); }
    FramePtr video(qint64 frame, double) override {
        // a fresh AVFrame each call, sharing the buffer (like a decoder pool handing out a new picture)
        FramePtr f = makeFramePtr(); av_frame_ref(f.get(), m_frames[size_t(frame % 16)].get()); return f;
    }
private:
    std::vector<FramePtr> m_frames;
};

/// Full-raster overlay with straight alpha (an HTML template page): a bar across the lower third, transparent elsewhere.
/// animated = a new picture every frame (a ticker / animating page), else the same picture (a lower third at rest).
class OverlaySource : public LayerSource {
public:
    explicit OverlaySource(bool animated) : m_animated(animated) {}
    QString type() const override { return "bench_overlay"; }
    bool open(const OutputFormat& fmt) override {
        for (int k = 0; k < 2; ++k) {
            FramePtr f = allocVideoFrame(fmt.width, fmt.height, AV_PIX_FMT_BGRA);
            for (int y = 0; y < fmt.height; ++y) {
                auto* r = reinterpret_cast<quint32*>(f->data[0] + y * f->linesize[0]);
                const bool bar = y > fmt.height * 0.72 && y < fmt.height * 0.86;
                for (int x = 0; x < fmt.width; ++x) r[x] = bar ? (0xD0000000u | quint32(30 + k * 20) << 16 | 0x4080u) : 0u;
            }
            m_frames.push_back(f);
        }
        return true;
    }
    void close() override { m_frames.clear(); }
    bool isOpen() const override { return !m_frames.empty(); }
    FramePtr video(qint64 frame, double) override {
        if (!m_animated) return m_frames[0];
        FramePtr f = makeFramePtr(); av_frame_ref(f.get(), m_frames[size_t(frame % 2)].get()); return f;
    }
private:
    bool m_animated;
    std::vector<FramePtr> m_frames;
};
}

void BenchPipeline::composite_data() {
    QTest::addColumn<bool>("gpu");
    QTest::addColumn<int>("layers");
    QTest::addColumn<bool>("pipelined");
    QTest::addColumn<int>("mixes");
    QTest::newRow("cpu passthrough") << false << 0 << false << 1;
    QTest::newRow("cpu +static L3 +animated page") << false << 2 << false << 1;
    QTest::newRow("OLD gpu (sync, no upload cache) +static L3 +animated page") << true << 2 << false << -1;
    QTest::newRow("gpu sync +static L3 +animated page") << true << 2 << false << 1;
    QTest::newRow("gpu pipelined +static L3 +animated page") << true << 2 << true << 1;
    QTest::newRow("gpu depth1 +static L3 +animated page") << true << 2 << true << 11;
    QTest::newRow("gpu depth3 +static L3 +animated page") << true << 2 << true << 13;
    QTest::newRow("gpu pipelined +static L3") << true << 1 << true << 1;
    QTest::newRow("gpu pipelined 2 mixes, different CG") << true << 2 << true << 2;
    QTest::newRow("cpu 2 mixes, different CG") << false << 2 << false << 2;
    QTest::newRow("cpu no-crop +static L3 +animated page") << false << 2 << false << 21;
}

void BenchPipeline::composite() {
    QFETCH(bool, gpu);
    QFETCH(int, layers);
    QFETCH(bool, pipelined);
    QFETCH(int, mixes);
    const bool oldPath = mixes < 0;
    int depth = pipelined ? 2 : 0;
    if (mixes > 20 || oldPath) { if (mixes > 20) mixes = 1; qputenv("PLAYPLAY_NO_OVERLAY_CROP", "1"); } else qunsetenv("PLAYPLAY_NO_OVERLAY_CROP");
    if (mixes > 10) { depth = mixes - 10; mixes = 1; }
    if (oldPath) { mixes = 1; qputenv("PLAYPLAY_GPU_NOCACHE", "1"); } else qunsetenv("PLAYPLAY_GPU_NOCACHE");
    OutputFormat fmt; fmt.width = 1920; fmt.height = 1080; fmt.rate = FrameRate::ntsc();
    Compositor comp(fmt);
    if (gpu) { comp.setGpuAuto(false); comp.setGpuEnabled(true); if (!comp.gpuActive()) QSKIP("no GPU"); }   // forced GPU: measure that path
    comp.setPipelineDepth(depth);
    if (mixes > 1) comp.setMix("web", QJsonObject{{"name", "Web"}});
    auto prog = std::make_unique<CyclingSource>(); prog->open(fmt);
    comp.programTake(std::move(prog), Compositor::Transition(), 0);
    if (layers >= 1) {
        auto ov = std::make_unique<OverlaySource>(false); ov->open(fmt);
        comp.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"layer", 30}, {"role", "lower_third"}}), std::move(ov), 0);
    }
    if (layers >= 2) {
        // the animated page goes to main only when there are two mixes: the mixes then differ and both are rendered
        QJsonObject spec{{"layer", 40}, {"role", "ticker"}};
        if (mixes > 1) spec["mixes"] = QJsonArray{"main"};
        auto ov = std::make_unique<OverlaySource>(true); ov->open(fmt);
        comp.addLayer(Compositor::LayerSpec::fromJson(spec), std::move(ov), 0);
    }
    for (int i = 0; i < 30; ++i) comp.render(i, i / 29.97);     // warm up
    std::vector<double> ms;
    const double c0 = cpuSeconds();
    QElapsedTimer all; all.start();
    QElapsedTimer pace; pace.start();
    for (qint64 f = 30; f < 330; ++f) {
        // paced like the engine's frame clock: the GPU gets the rest of each 33.37 ms period
        const qint64 due = qint64((f - 30) * 1001000000.0 / 30000.0);
        while (pace.nsecsElapsed() < due) QThread::usleep(200);
        QElapsedTimer t; t.start();
        std::map<QString, MediaFrame> others;
        MediaFrame mf = comp.render(f, f / 29.97, &others);
        if (mixes > 1) QVERIFY(others.count("web") && others["web"].video);
        ms.push_back(t.nsecsElapsed() / 1e6);
        QVERIFY(mf.video);
    }
    const double cpuMsPerFrame = (cpuSeconds() - c0) * 1000.0 / 300.0;   // whole process (incl. the pacing loop's wakeups)
    const Dist d = dist(ms);
    qInfo().noquote() << QString("BENCH composite %1%7 layers=%2 mixes=%8: tick avg %3 ms  p95 %4  max %5   CPU %6 ms/frame   %9")
        .arg(gpu ? QString("gpu d%1").arg(depth) : QString("cpu")).arg(layers).arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2).arg(d.max, 0, 'f', 2).arg(cpuMsPerFrame, 0, 'f', 2).arg(pipelined ? " pipelined" : "").arg(mixes)
        .arg(gpu ? QString::fromUtf8(QJsonDocument(comp.status().value("gpuStats").toObject()).toJson(QJsonDocument::Compact)) : QString());
}

void BenchPipeline::convert() {
    FramePtr src = allocVideoFrame(1920, 1080, AV_PIX_FMT_BGRA);
    for (int y = 0; y < 1080; ++y) memset(src->data[0] + y * src->linesize[0], y & 255, 1920 * 4);
    FramePtr dst = allocVideoFrame(1920, 1080, AV_PIX_FMT_YUV420P);
    for (int threads : {1, 0}) {
        SwsContext* sws = sws_alloc_context();
        av_opt_set_int(sws, "srcw", 1920, 0); av_opt_set_int(sws, "srch", 1080, 0); av_opt_set_int(sws, "src_format", AV_PIX_FMT_BGRA, 0);
        av_opt_set_int(sws, "dstw", 1920, 0); av_opt_set_int(sws, "dsth", 1080, 0); av_opt_set_int(sws, "dst_format", AV_PIX_FMT_YUV420P, 0);
        av_opt_set_int(sws, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND, 0);
        av_opt_set_int(sws, "threads", threads, 0);
        QVERIFY(sws_init_context(sws, nullptr, nullptr) >= 0);
        std::vector<double> ms;
        for (int i = 0; i < 60; ++i) {
            QElapsedTimer t; t.start();
            av_frame_unref(dst.get()); dst->format = AV_PIX_FMT_YUV420P; dst->width = 1920; dst->height = 1080; av_frame_get_buffer(dst.get(), 32);
            sws_scale_frame(sws, dst.get(), src.get());
            ms.push_back(t.nsecsElapsed() / 1e6);
        }
        const Dist d = dist(ms);
        qInfo().noquote() << QString("BENCH convert BGRA->YUV420P 1080p threads=%1: avg %2 ms  p95 %3").arg(threads == 0 ? QString("auto") : QString::number(threads)).arg(d.avg, 0, 'f', 2).arg(d.p95, 0, 'f', 2);
        sws_freeContext(sws);
    }
    // the cost of one plain 1080p BGRA copy, for scale
    FramePtr cp = allocVideoFrame(1920, 1080, AV_PIX_FMT_BGRA);
    QElapsedTimer t; t.start();
    for (int i = 0; i < 60; ++i) for (int y = 0; y < 1080; ++y) memcpy(cp->data[0] + y * cp->linesize[0], src->data[0] + y * src->linesize[0], 1920 * 4);
    qInfo().noquote() << QString("BENCH memcpy 1080p BGRA: %1 ms").arg(t.nsecsElapsed() / 1e6 / 60.0, 0, 'f', 2);
}

QTEST_MAIN(BenchPipeline)
#include "bench_pipeline.moc"
