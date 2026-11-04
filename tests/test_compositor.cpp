#define _USE_MATH_DEFINES
#include <cmath>
#include <QtTest/QtTest>
#include <map>
#include <QTemporaryDir>
#include <QGuiApplication>
#include <QPainter>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <cmath>
#include "playout/core/framerate.h"
#include "playout/engine/compositor/compositor.h"
#include "playout/engine/compositor/basicsources.h"
#include "playout/engine/compositor/gpucompositor.h"
#include "playout/engine/compositor/loudness.h"
#include "playout/engine/compositor/metersnapshot.h"
#include "playout/engine/tickprofiler.h"
#include "playout/engine/mixer.h"

namespace {

/// Busy wait (QThread::msleep has the 15.6 ms Windows timer granularity; these waits are a few ms).
void spinMs(double ms) { QElapsedTimer t; t.start(); while (t.nsecsElapsed() < qint64(ms * 1e6)) QThread::yieldCurrentThread(); }

/// PE-05 probe: an audio layer source whose status() takes `statusMs` and that records what overlapped it.
struct ProbeState {
    std::atomic<int> statusMs{0}, inStatus{0}, statusCalls{0}, updates{0}, updateDuringStatus{0}, destroyed{0}, destroyedDuringStatus{0};
};
class ProbeSource : public LayerSource {
public:
    explicit ProbeSource(std::shared_ptr<ProbeState> st, double toneHz = 300.0) : m_st(std::move(st)), m_hz(toneHz) {}
    ~ProbeSource() override { if (m_st->inStatus.load()) m_st->destroyedDuringStatus++; m_st->destroyed++; }
    QString type() const override { return "probe"; }
    bool open(const OutputFormat& f) override { m_fmt = f; m_open = true; return true; }
    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64, double) override { return nullptr; }
    bool hasAudio() const override { return true; }
    bool audio(const FramePtr& dst, qint64, double) override {
        for (int i = 0; i < dst->nb_samples; ++i) {
            const float v = float(0.05 * std::sin(m_phase)); m_phase += 2.0 * M_PI * m_hz / m_fmt.sampleRate;
            for (int c = 0; c < dst->ch_layout.nb_channels; ++c) reinterpret_cast<float*>(dst->data[c])[i] = v;
        }
        return true;
    }
    void update(const QJsonObject& p) override {
        if (m_st->inStatus.load()) m_st->updateDuringStatus++;
        m_text = p.value("text").toString(m_text);      // the member status() reads: must never overlap it
        m_st->updates++;
    }
    QJsonObject status() const override {
        m_st->inStatus++; m_st->statusCalls++;
        if (const int ms = m_st->statusMs.load()) spinMs(ms);
        QJsonObject o = LayerSource::status(); o["connected"] = true; o["text"] = m_text;
        m_st->inStatus--;
        return o;
    }
private:
    std::shared_ptr<ProbeState> m_st;
    OutputFormat m_fmt;
    bool m_open = false;
    double m_hz, m_phase = 0.0;
    QString m_text = "before";
};

/// Sets PLAYPLAY_COMP_LEGACY_STATUS for the Compositor created in this scope (it is read at construction).
struct LegacyEnv {
    explicit LegacyEnv(bool legacy) { if (legacy) qputenv("PLAYPLAY_COMP_LEGACY_STATUS", "1"); else qunsetenv("PLAYPLAY_COMP_LEGACY_STATUS"); }
    ~LegacyEnv() { qunsetenv("PLAYPLAY_COMP_LEGACY_STATUS"); }
};

QString compactJson(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
QString compactJson(const QJsonArray& a) { return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)); }

/// PE-08: sets PLAYPLAY_PREMUL for the sources created in this scope (a source reads it when it is created); null = unset.
struct PremulEnv {
    explicit PremulEnv(const char* v) { if (v) qputenv("PLAYPLAY_PREMUL", v); else qunsetenv("PLAYPLAY_PREMUL"); }
    ~PremulEnv() { qunsetenv("PLAYPLAY_PREMUL"); }
};

/// PE-08: a layer source handing out one RGBA picture, straight or premultiplied, and saying which.
class RgbaSource : public LayerSource {
public:
    RgbaSource(const QImage& img, bool premul)
        : m_premul(premul), m_frame(premul ? Mixer::imageToFramePremultiplied(img) : Mixer::imageToFrame(img)), m_size(img.size()) {}
    QString type() const override { return "rgba"; }
    bool open(const OutputFormat&) override { return m_frame != nullptr; }
    void close() override {}
    bool isOpen() const override { return m_frame != nullptr; }
    FramePtr video(qint64, double) override { return m_frame; }
    QSize nativeSize() const override { return m_size; }
    bool deliversPremultiplied() const override { return m_premul; }
private:
    bool m_premul;
    FramePtr m_frame;
    QSize m_size;
};

/// Straight-alpha card for the edge cases: alpha = x (0..255) across, one colour per 16-row band (white, black, orange,
/// a mid violet), so every alpha value is blended for a saturated, a dark and an odd colour.
QImage alphaRamp() {
    QImage img(256, 64, QImage::Format_ARGB32);
    const QRgb bands[4] = {qRgb(255, 255, 255), qRgb(0, 0, 0), qRgb(255, 128, 0), qRgb(128, 64, 200)};
    for (int y = 0; y < 64; ++y) {
        QRgb* r = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < 256; ++x) { const QRgb c = bands[y / 16]; r[x] = qRgba(qRed(c), qGreen(c), qBlue(c), x); }
    }
    return img;
}

/// A compositor output copied out of the pool (Format_ARGB32 view of the BGRA bytes).
QImage frameImage(const FramePtr& f) { return f ? QImage(f->data[0], f->width, f->height, f->linesize[0], QImage::Format_ARGB32).copy() : QImage(); }

/// Per-channel difference of two same-size pictures (R, G, B and A), optionally skipping some pixels.
struct ImgDiff {
    int maxDiff = 0; qint64 pixels = 0, differing = 0; QPoint at;
    QString text() const { return QString("max %1 at (%2,%3); %4 of %5 pixels differ").arg(maxDiff).arg(at.x()).arg(at.y()).arg(differing).arg(pixels); }
};
ImgDiff diffImages(const QImage& a, const QImage& b, const std::function<bool(int, int)>& skip = {}) {
    ImgDiff d;
    if (a.size() != b.size()) { d.maxDiff = 9999; return d; }
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* pa = reinterpret_cast<const QRgb*>(a.constScanLine(y));
        const QRgb* pb = reinterpret_cast<const QRgb*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x) {
            if (skip && skip(x, y)) continue;
            ++d.pixels;
            if (pa[x] == pb[x]) continue;
            ++d.differing;
            const int m = qMax(qMax(std::abs(qRed(pa[x]) - qRed(pb[x])), std::abs(qGreen(pa[x]) - qGreen(pb[x]))),
                               qMax(std::abs(qBlue(pa[x]) - qBlue(pb[x])), std::abs(qAlpha(pa[x]) - qAlpha(pb[x]))));
            if (m > d.maxDiff) { d.maxDiff = m; d.at = QPoint(x, y); }
        }
    }
    return d;
}

/// The PE-08 template scene (720p): every layer at an integer 1:1 rect except a scaled box, so CPU, GPU and an
/// independent QPainter reference sample the same pixels. Two 50 %-alpha texts ("#80ffffff" white, "#80000000" black)
/// sit over the program alone. All pictures stay under 256x256 pixels of area (no overlay crop).
struct CgLayer { int id; QJsonObject src; QPoint at; QSize scaleTo; double opacity; bool fadeIn; };
std::vector<CgLayer> cgScene() {
    auto tpl = [](const QJsonObject& kv) { QJsonObject o = kv; o["type"] = "template"; return o; };
    return {
        {20, tpl({{"kind", "bug"}, {"text", "PLAY PLAY"}}), {1040, 30}, {}, 1.0, false},
        {30, tpl({{"kind", "lower_third"}, {"title", "Half and half"}, {"subtitle", "premultiplied alpha"}}), {60, 560}, {}, 1.0, true},
        {40, tpl({{"kind", "ticker"}, {"label", "PE-08"}, {"text", "A crawl of headline text over the program, antialiased glyph edges everywhere"}}), {0, 677}, {}, 1.0, false},
        {45, tpl({{"kind", "countdown"}, {"seconds", 3000.5}, {"label", "ON AIR IN"}}), {40, 30}, {}, 1.0, false},   // "50:00" for the whole test
        {50, tpl({{"kind", "text"}, {"text", "HALF WHITE"}, {"color", "#80ffffff"}, {"fontSize", 60}}), {300, 300}, {}, 1.0, false},
        {51, tpl({{"kind", "text"}, {"text", "HALF BLACK"}, {"color", "#80000000"}, {"fontSize", 60}}), {300, 140}, {}, 1.0, false},
        {52, tpl({{"kind", "box"}, {"color", "#60ff8000"}, {"width", 200}, {"height", 120}}), {1000, 300}, {250, 150}, 1.0, false},
        {53, tpl({{"kind", "text"}, {"text", "Faded"}, {"bgColor", "#c0102030"}, {"color", "#ffffff"}, {"fontSize", 40}}), {900, 560}, {}, 0.55, false},
    };
}
/// The rect a scene layer is drawn at (its natural size unless scaled).
QRect cgRect(const CgLayer& l, const OutputFormat& fmt) {
    if (!l.scaleTo.isEmpty()) return QRect(l.at, l.scaleTo);
    QSize sz; TemplateSource::render(l.src, fmt, 0.0, &sz);
    return QRect(l.at, sz);
}

} // namespace

/**
 * Compositor tests (no media files needed: generator and template sources).
 *
 *  - program bus: cut is immediate; mix/wipe/dip/push/squeeze reach the target
 *    exactly at the configured duration and are a true blend half way.
 *  - layers: fade-in reaches full opacity, holdSec auto-hides, hide removes.
 *  - audio: a -20 dBFS 1 kHz program tone meters at -20 LUFS (+-1); a ducking
 *    layer pulls program down to the configured depth within attack+2 frames.
 *  - GPU path: the Direct3D 11 renderer agrees with the QPainter path within
 *    a small tolerance on a mixed draw list (skipped if no device).
 *  - PE-05 readers: the meter seqlock never tears; metersJson / status / layersJson from the snapshot equal the old
 *    locked readers; a reader inside a slow source status() neither delays render nor outlives the source, and an
 *    update() never overlaps it; a 100 Hz metersJson reader does not slow 300 renders (PLAYPLAY_COMP_LEGACY_STATUS=1
 *    shows the old lock wait).
 *  - PE-08 premultiplied alpha: PLAYPLAY_PREMUL unset / 0 reproduces an independent reference of the old path bit for
 *    bit; =1 stays within ±2 per channel (template scene at several frames, every alpha 0..255 over white and black,
 *    50 % text), CPU and GPU agree, and a premultiplied program picture is composited onto black, never passed through.
 */
class TestCompositor : public QObject {
    Q_OBJECT

    static QJsonObject color(const QString& hex) { QJsonObject o; o["type"] = "color"; o["color"] = hex; return o; }
    static std::unique_ptr<LayerSource> openSrc(const QJsonObject& spec, const OutputFormat& fmt) {
        QString err; auto s = LayerSource::create(spec, &err);
        if (s && !s->open(fmt)) s.reset();
        return s;
    }
    static QRgb px(const FramePtr& f, int x, int y) { return reinterpret_cast<const QRgb*>(f->data[0] + y * f->linesize[0])[x]; }
    static OutputFormat fmt720() { OutputFormat f; f.width = 1280; f.height = 720; f.rate = FrameRate(30, 1); return f; }

    /// PE-08: renders cgScene() over `program` for frames 0..12 with PLAYPLAY_PREMUL = `env` (null = unset) and returns the
    /// frames in `keep`. The lower third fades in over 0.5 s, the ticker crawls. Empty when `gpu` and no D3D11 device.
    static std::map<int, QImage> renderCgScene(const char* env, const char* program, bool gpu, const QList<int>& keep) {
        const OutputFormat fmt = fmt720();
        std::map<int, QImage> out;
        PremulEnv e(env);                                         // the template sources read it when addLayer creates them
        Compositor c(fmt);
        if (gpu) { c.setGpuEnabled(true); c.setGpuAuto(false); if (!c.gpuActive()) return out; }
        c.programTake(openSrc(color(program), fmt), Compositor::Transition(), 0);
        for (const CgLayer& l : cgScene()) {
            const QRect r = cgRect(l, fmt);
            QJsonObject spec{{"id", l.id}, {"source", l.src}, {"opacity", l.opacity},
                             {"placement", QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}}};
            if (l.fadeIn) spec["in"] = QJsonObject{{"type", "fade"}, {"duration", 0.5}};
            QString err;
            if (!c.addLayer(Compositor::LayerSpec::fromJson(spec), 0, &err)) { qWarning() << "addLayer" << l.id << err; return {}; }
        }
        for (int f = 0; f <= 12; ++f) { const MediaFrame mf = c.render(f, f / 30.0); if (keep.contains(f)) out[f] = frameImage(mf.video); }
        return out;
    }

private slots:
    void initTestCase() {}

    void programCutIsImmediate() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
        MediaFrame mf = c.render(0, 0.0);
        QVERIFY(mf.video);
        QCOMPARE(qRed(px(mf.video, 100, 100)), 255);
        QCOMPARE(qBlue(px(mf.video, 100, 100)), 0);
        // Audio is produced every frame, cadence-correct.
        QVERIFY(mf.audio);
        QCOMPARE(mf.audio->nb_samples, 1600);
    }

    void programMixBlendsAndCompletes() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
        c.render(0, 0.0);
        Compositor::Transition t; t.type = "mix"; t.durationSec = 1.0;    // 30 frames
        c.programTake(openSrc(color("#0000ff"), fmt), t, 1);
        QVERIFY(c.programTransitioning());
        MediaFrame mid = c.render(16, 0.5);     // ease(0.5) = 0.5
        const QRgb m = px(mid.video, 10, 10);
        QVERIFY2(std::abs(qRed(m) - 128) <= 3 && std::abs(qBlue(m) - 128) <= 3, qPrintable(QString("mid=%1,%2,%3").arg(qRed(m)).arg(qGreen(m)).arg(qBlue(m))));
        MediaFrame end = c.render(31, 1.0);
        QCOMPARE(qBlue(px(end.video, 10, 10)), 255);
        QCOMPARE(qRed(px(end.video, 10, 10)), 0);
        QVERIFY(!c.programTransitioning());
        QCOMPARE(c.status()["transitioning"].toBool(), false);
    }

    void programWipeHasHardEdge() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
        c.render(0, 0.0);
        Compositor::Transition t; t.type = "wipe_left"; t.durationSec = 1.0;
        c.programTake(openSrc(color("#0000ff"), fmt), t, 1);
        MediaFrame mid = c.render(16, 0.5);    // boundary at x = 640
        QCOMPARE(qBlue(px(mid.video, 600, 300)), 255);
        QCOMPARE(qRed(px(mid.video, 680, 300)), 255);
    }

    void programDipPushSqueeze() {
        const OutputFormat fmt = fmt720();
        for (const char* type : {"dip", "push_left", "squeeze", "wipe_down", "push_up"}) {
            Compositor c(fmt);
            c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
            c.render(0, 0.0);
            Compositor::Transition t; t.type = type; t.durationSec = 0.5; t.dipColor = Qt::black;
            c.programTake(openSrc(color("#00ff00"), fmt), t, 1);
            MediaFrame mid = c.render(8, 0.0);
            QVERIFY(mid.video);
            MediaFrame end = c.render(16, 0.5);
            QVERIFY2(qGreen(px(end.video, 640, 360)) == 255 && qRed(px(end.video, 640, 360)) == 0, type);
            QVERIFY2(!c.programTransitioning(), type);
        }
    }

    void programClearGoesToSlateThenBlack() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        MediaFrame none = c.render(0, 0.0);         // nothing on the bus, no slate: black
        QCOMPARE(px(none.video, 5, 5), qRgb(0, 0, 0));
        QJsonObject bars; bars["type"] = "bars";
        c.setSlate(openSrc(bars, fmt));
        MediaFrame slate = c.render(1, 0.0);
        QVERIFY(px(slate.video, 50, 50) != qRgb(0, 0, 0));
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 2);
        QCOMPARE(qRed(px(c.render(2, 0.0).video, 50, 50)), 255);
        Compositor::Transition t; t.type = "mix"; t.durationSec = 0.5;
        c.programClear(t, 3);
        c.render(10, 0.0);
        MediaFrame after = c.render(20, 0.0);
        QVERIFY(!c.programSource());
        QCOMPARE(px(after.video, 50, 50), px(slate.video, 50, 50));
    }

    void layerFadeHoldAndHide() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.programTake(openSrc(color("#000000"), fmt), Compositor::Transition(), 0);
        QJsonObject spec;
        spec["id"] = 30; spec["role"] = "lower_third";
        QJsonObject tpl; tpl["type"] = "template"; tpl["kind"] = "box"; tpl["color"] = "#ffffff"; tpl["bgColor"] = "#ffffff"; tpl["width"] = 400; tpl["height"] = 100;
        spec["source"] = tpl;
        QJsonObject pl; pl["x"] = 100; pl["y"] = 500; pl["width"] = 400; pl["height"] = 100; spec["placement"] = pl;
        QJsonObject in; in["type"] = "fade"; in["durationSec"] = 0.5; spec["in"] = in;
        spec["holdSec"] = 2.0;
        QString err;
        QVERIFY2(c.addLayer(Compositor::LayerSpec::fromJson(spec), 0, &err), qPrintable(err));
        QCOMPARE(c.layersJson().size(), 1);
        const int lum0 = qRed(px(c.render(0, 0.0).video, 300, 550));
        const int lum8 = qRed(px(c.render(8, 0.0).video, 300, 550));
        const int lum30 = qRed(px(c.render(30, 0.0).video, 300, 550));
        QVERIFY2(lum0 < lum8 && lum8 < lum30, qPrintable(QString("%1 %2 %3").arg(lum0).arg(lum8).arg(lum30)));
        QVERIFY(lum30 > 240);
        QCOMPARE(qRed(px(c.render(31, 0.0).video, 50, 50)), 0);        // outside the box stays program
        // holdSec = 2 s -> hidden after frame 60 (+ cut out)
        c.render(61, 0.0);
        QCOMPARE(c.layersJson()[0].toObject()["state"].toString(), QString("hidden"));
        QCOMPARE(qRed(px(c.render(62, 0.0).video, 300, 550)), 0);
        // show again, then hide with a cut, then remove
        QVERIFY(c.showLayer(30, Compositor::Transition(), 63));
        QVERIFY(qRed(px(c.render(63, 0.0).video, 300, 550)) > 240);
        QVERIFY(c.hideLayer(30, Compositor::Transition(), 64));
        QCOMPARE(qRed(px(c.render(64, 0.0).video, 300, 550)), 0);
        QVERIFY(c.removeLayer(30));
        QCOMPARE(c.layersJson().size(), 0);
    }

    void layerSlideAndAnchorPlacement() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.programTake(openSrc(color("#000000"), fmt), Compositor::Transition(), 0);
        QJsonObject spec; spec["id"] = 20; spec["role"] = "bug";
        QJsonObject tpl; tpl["type"] = "color"; tpl["color"] = "#ffffff"; tpl["width"] = 200; tpl["height"] = 100; spec["source"] = tpl;
        QJsonObject pl; pl["anchor"] = "top_right"; pl["margin"] = 40; spec["placement"] = pl;
        QJsonObject in; in["type"] = "slide_right"; in["durationSec"] = 1.0; spec["in"] = in;
        QVERIFY(c.addLayer(Compositor::LayerSpec::fromJson(spec), 0));
        // final rect: x = 1280-200-40 = 1040..1240, y = 40..140
        MediaFrame early = c.render(1, 0.0);
        QCOMPARE(qRed(px(early.video, 1100, 90)), 0);          // still off to the right
        MediaFrame done = c.render(40, 0.0);
        QCOMPARE(qRed(px(done.video, 1100, 90)), 255);
        QCOMPARE(qRed(px(done.video, 1000, 90)), 0);
        QCOMPARE(qRed(px(done.video, 1100, 160)), 0);
    }

    void audioMeterAndDucking() {
        OutputFormat fmt = fmt720(); fmt.channels = 2;
        Compositor c(fmt);
        QJsonObject tone; tone["type"] = "tone"; tone["toneHz"] = 1000; tone["toneDbfs"] = -20.0;
        c.programTake(openSrc(tone, fmt), Compositor::Transition(), 0);
        for (qint64 f = 0; f < 120; ++f) c.render(f, f / 30.0);      // 4 s
        const double lufs = c.meter().momentaryLufs();
        QVERIFY2(std::fabs(lufs - (-20.0)) < 1.0, qPrintable(QString("momentary %1 LUFS").arg(lufs)));
        QVERIFY(c.meter().peakDbfs() < -19.5 && c.meter().peakDbfs() > -20.5);
        QCOMPARE(c.status()["audio"].toObject()["clips"].toDouble(), 0.0);

        // Ducking layer: -12 dB depth, 50 ms attack.
        Compositor::DuckSettings d; d.depthDb = -12.0; d.attackSec = 0.05; d.releaseSec = 0.3;
        c.setDucking(d);
        QJsonObject spec; spec["id"] = 50; spec["duck"] = true;
        QJsonObject vo; vo["type"] = "tone"; vo["toneHz"] = 400; vo["toneDbfs"] = -20.0; vo["width"] = 2; vo["height"] = 2; spec["source"] = vo;
        QVERIFY(c.addLayer(Compositor::LayerSpec::fromJson(spec), 120));
        for (qint64 f = 120; f < 135; ++f) c.render(f, f / 30.0);   // 0.5 s >> attack
        const double duck = c.status()["audio"].toObject()["duckGainDb"].toDouble();
        QVERIFY2(std::fabs(duck - (-12.0)) < 0.5, qPrintable(QString("duck gain %1 dB").arg(duck)));
        QVERIFY(c.layersJson()[0].toObject()["audible"].toBool());
        // Release after the layer is removed.
        c.removeLayer(50);
        for (qint64 f = 135; f < 195; ++f) c.render(f, f / 30.0);   // 2 s
        QVERIFY(c.status()["audio"].toObject()["duckGainDb"].toDouble() > -0.5);
    }

    void audioCrossfadesOnMix() {
        OutputFormat fmt = fmt720();
        Compositor c(fmt);
        QJsonObject a; a["type"] = "tone"; a["toneHz"] = 1000; a["toneDbfs"] = -20.0;
        QJsonObject silent = color("#000000");
        c.programTake(openSrc(a, fmt), Compositor::Transition(), 0);
        c.render(0, 0.0);
        Compositor::Transition t; t.type = "mix"; t.durationSec = 1.0;
        c.programTake(openSrc(silent, fmt), t, 1);
        auto rms = [](const FramePtr& au) { double e = 0; const float* s = reinterpret_cast<const float*>(au->data[0]); for (int i = 0; i < au->nb_samples; ++i) e += double(s[i]) * s[i]; return std::sqrt(e / au->nb_samples); };
        const double r0 = rms(c.render(1, 0.0).audio);
        const double rMid = rms(c.render(16, 0.5).audio);
        const double rEnd = rms(c.render(40, 1.3).audio);
        QVERIFY2(r0 > rMid && rMid > rEnd, qPrintable(QString("%1 %2 %3").arg(r0).arg(rMid).arg(rEnd)));
        QVERIFY(rEnd < 1e-6);
    }

    // PE-13: pooled audio buffers, recycled source entries and cached strip keys change nothing audible or metered.
    // Every combination of PLAYPLAY_AUDIO_POOL and the PE-05 legacy readers produces the same samples (29.97 cadence,
    // two mixes, a ducking layer only in web, pipelined), the same meters JSON, and frames still held downstream keep
    // their samples while the pool recycles the ones that were released.
    void audioPoolMatchesUnpooled() {
        OutputFormat fmt; fmt.width = 320; fmt.height = 180; fmt.rate = FrameRate::ntsc(); fmt.channels = 2;
        struct Run { std::vector<std::vector<float>> mainS, webS; std::vector<int> n; std::vector<qint64> pts; QString meters; bool heldOk = true; };
        auto run = [&](bool pool, bool legacy) {
            LegacyEnv le(legacy);
            if (pool) qunsetenv("PLAYPLAY_AUDIO_POOL"); else qputenv("PLAYPLAY_AUDIO_POOL", "0");
            Run r;
            {
                Compositor c(fmt);
                qunsetenv("PLAYPLAY_AUDIO_POOL");
                if (c.audioPooled() != pool) { r.heldOk = false; return r; }
                c.setPipelineDepth(2);
                c.setMix("web", QJsonObject{{"name", "Web"}, {"strips", QJsonObject{{"L61", QJsonObject{{"gainDb", -6.0}}}}}});
                QJsonObject tone; tone["type"] = "tone"; tone["toneHz"] = 1000; tone["toneDbfs"] = -20.0;
                c.programTake(openSrc(tone, fmt), Compositor::Transition(), 0);
                auto probe = [&](double hz) { auto p = std::make_unique<ProbeSource>(std::make_shared<ProbeState>(), hz); p->open(fmt); return p; };
                Compositor::LayerSpec ls; ls.id = 61; ls.role = "audio"; ls.duck = true; ls.mixes = QStringList{"web"};
                c.addLayer(ls, probe(440.0), 0);
                Compositor::LayerSpec ls2; ls2.id = 70; ls2.role = "audio"; ls2.audioGain = 0.5;
                auto copy = [&](const FramePtr& a) {
                    std::vector<float> v;
                    for (int ch = 0; ch < fmt.channels; ++ch) { const float* x = reinterpret_cast<const float*>(a->data[ch]); v.insert(v.end(), x, x + a->nb_samples); }
                    return v;
                };
                std::vector<std::pair<FramePtr, std::vector<float>>> held;   // every 7th main frame stays referenced
                for (qint64 f = 0; f < 60; ++f) {
                    std::map<QString, MediaFrame> mixes;
                    MediaFrame m = c.render(f, f / 29.97, &mixes);
                    if (!m.audio || !mixes.count("web") || !mixes["web"].audio) { r.heldOk = false; return r; }
                    const FramePtr& a = m.audio;
                    const qint64 gap = qAbs(qint64(reinterpret_cast<quintptr>(a->data[1])) - qint64(reinterpret_cast<quintptr>(a->data[0])));
                    if (a->linesize[0] < a->nb_samples * int(sizeof(float)) || gap < qint64(a->nb_samples) * qint64(sizeof(float))) r.heldOk = false;
                    r.n.push_back(a->nb_samples); r.pts.push_back(a->pts);
                    r.mainS.push_back(copy(a)); r.webS.push_back(copy(mixes["web"].audio));
                    if (f % 7 == 0) held.emplace_back(a, r.mainS.back());
                    if (f == 10) c.addLayer(ls2, probe(250.0), f);      // a source joins mid-run ...
                    if (f == 30) c.removeLayer(70);                     // ... and leaves: its entry is recycled
                }
                for (const auto& h : held) if (copy(h.first) != h.second) r.heldOk = false;
                // A clone made writable must copy, not scribble on the pooled buffer (encoders do this).
                FramePtr cl = makeFramePtr(av_frame_clone(held.back().first.get()));
                if (av_frame_make_writable(cl.get()) < 0 || cl->data[0] == held.back().first->data[0]) r.heldOk = false;
                QJsonObject mj = c.metersJson(); mj.remove("integratedLufs");
                r.meters = compactJson(mj);
            }
            return r;
        };
        const Run base = run(false, false);
        QVERIFY(base.heldOk);
        QVERIFY(std::find(base.n.begin(), base.n.end(), 1601) != base.n.end() && std::find(base.n.begin(), base.n.end(), 1602) != base.n.end());
        QVERIFY2(base.meters.contains("\"L61\"") && base.meters.contains("\"program\""), qPrintable(base.meters.left(400)));
        const Run legacyBase = run(false, true);
        for (auto [pool, legacy] : {std::pair<bool, bool>{true, false}, {true, true}}) {
            const Run r = run(pool, legacy);
            const QString tag = QString("pool=%1 legacy=%2").arg(pool).arg(legacy);
            QVERIFY2(r.heldOk, qPrintable(tag));
            QVERIFY2(r.n == base.n && r.pts == base.pts, qPrintable(tag));
            QVERIFY2(r.mainS == base.mainS, qPrintable(tag + " main samples differ"));
            QVERIFY2(r.webS == base.webS, qPrintable(tag + " web samples differ"));
            QCOMPARE(r.meters, legacy ? legacyBase.meters : base.meters);
        }
        QVERIFY(legacyBase.mainS == base.mainS);
    }

    // Two output mixes: the same program, a graphic only in main, a sound only in web; each mix has its own faders.
    void mixesDifferInGraphicsAndAudio() {
        OutputFormat fmt = fmt720(); fmt.channels = 2;
        Compositor c(fmt);
        c.setMix("web", QJsonObject{{"name", "Web"}});
        QCOMPARE(c.mixIds(), QStringList({"main", "web"}));
        QJsonObject tone; tone["type"] = "tone"; tone["toneHz"] = 1000; tone["toneDbfs"] = -20.0;
        c.programTake(openSrc(tone, fmt), Compositor::Transition(), 0);
        QJsonObject g{{"id", 30}, {"source", color("#00ff00")}, {"x", 0}, {"y", 0}, {"width", 200}, {"height", 100}, {"mixes", QJsonArray{"main"}}};
        QVERIFY(c.addLayer(Compositor::LayerSpec::fromJson(g), 0));
        QJsonObject beep{{"type", "tone"}, {"toneHz", 400}, {"toneDbfs", -20.0}, {"width", 2}, {"height", 2}};
        QVERIFY(c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 61}, {"role", "audio"}, {"source", beep}, {"mixes", QJsonArray{"web"}}}), 0));
        auto rms = [](const FramePtr& au) { double e = 0; const float* x = reinterpret_cast<const float*>(au->data[0]); for (int i = 0; i < au->nb_samples; ++i) e += double(x[i]) * x[i]; return std::sqrt(e / qMax(1, au->nb_samples)); };
        std::map<QString, MediaFrame> mixes;
        MediaFrame mainF;
        for (qint64 f = 0; f < 5; ++f) { mixes.clear(); mainF = c.render(f, f / 30.0, &mixes); }
        QVERIFY(mixes.count("web"));
        const MediaFrame& web = mixes["web"];
        QCOMPARE(qGreen(px(mainF.video, 50, 50)), 255);            // the graphic is in main
        QVERIFY(qGreen(px(web.video, 50, 50)) < 40);               // and not in web
        QVERIFY(mainF.video.get() != web.video.get());
        const double mainRms = rms(mainF.audio), webRms = rms(web.audio);
        QVERIFY2(webRms > mainRms * 1.2, qPrintable(QString("main %1 web %2").arg(mainRms).arg(webRms)));   // web also carries the 400 Hz layer
        // Independent faders: mute the program in web only.
        c.setMix("web", QJsonObject{{"strips", QJsonObject{{"program", QJsonObject{{"mute", true}}}}}});
        for (qint64 f = 5; f < 8; ++f) { mixes.clear(); mainF = c.render(f, f / 30.0, &mixes); }
        const double mainAfter = rms(mainF.audio), webAfter = rms(mixes["web"].audio);
        QVERIFY2(std::fabs(mainAfter - mainRms) < mainRms * 0.05, "main must not change");
        QVERIFY2(webAfter < webRms * 0.9 && webAfter > 0.01, qPrintable(QString("web after mute %1").arg(webAfter)));
        // Per-source (pre-fader) meters and per-mix strips are reported.
        const QJsonObject m = c.metersJson();
        QVERIFY(m["sources"].toObject().contains("program"));
        QVERIFY(m["sources"].toObject().contains("L61"));
        const double prePk = m["sources"].toObject()["program"].toObject()["peaks"].toArray()[0].toDouble();
        QVERIFY2(std::fabs(prePk - (-20.0)) < 1.0, qPrintable(QString("program pre-fader peak %1").arg(prePk)));
        QCOMPARE(m["mixes"].toArray().size(), 2);
        bool sawMute = false;
        for (const QJsonValue& st : m["mixes"].toArray()[1].toObject()["strips"].toArray())
            if (st.toObject()["id"].toString() == "program") sawMute = st.toObject()["mute"].toBool();
        QVERIFY(sawMute);
        // A graphic with no mixes list is in every mix; removing the mix drops its output.
        c.removeMix("web");
        mixes.clear(); c.render(9, 0.3, &mixes);
        QVERIFY(mixes.empty());
    }

    // Pipelined output: picture and sound come out `depth` frames late, together.
    void pipelinedOutputIsDelayedTogether() {
        OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.setPipelineDepth(1);
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
        MediaFrame f0 = c.render(0, 0.0);
        QCOMPARE(qRed(px(f0.video, 10, 10)), 0);                    // start-up frame: black
        c.programTake(openSrc(color("#00ff00"), fmt), Compositor::Transition(), 1);
        MediaFrame f1 = c.render(1, 0.033);
        QCOMPARE(qRed(px(f1.video, 10, 10)), 255);                  // frame 0's picture
        MediaFrame f2 = c.render(2, 0.066);
        QCOMPARE(qGreen(px(f2.video, 10, 10)), 255);                // frame 1's picture
        QVERIFY(f1.audio && f2.audio);
        QCOMPARE(c.pipelineDepth(), 1);
    }

    // Drawing only the visible box of a mostly transparent full-raster overlay gives exactly the same picture.
    void overlayCropMatchesFullDraw() {
        const OutputFormat fmt = fmt720();
        QTemporaryDir dir;
        QImage ov(fmt.width, fmt.height, QImage::Format_ARGB32); ov.fill(Qt::transparent);
        { QPainter p(&ov); p.fillRect(QRect(100, 520, 900, 90), QColor(20, 60, 200, 200)); p.fillRect(QRect(110, 530, 300, 20), QColor(255, 255, 255, 255)); }
        const QString path = dir.filePath("ov.png"); QVERIFY(ov.save(path));
        auto renderWith = [&](bool crop) -> QImage {
            if (crop) qunsetenv("PLAYPLAY_NO_OVERLAY_CROP"); else qputenv("PLAYPLAY_NO_OVERLAY_CROP", "1");
            Compositor c(fmt);
            c.programTake(openSrc(color("#804020"), fmt), Compositor::Transition(), 0);
            if (!c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 30}, {"source", QJsonObject{{"type", "image"}, {"path", path}}}}), 0)) return QImage();
            MediaFrame mf;
            for (int f = 0; f < 3; ++f) mf = c.render(f, f / 30.0);
            QImage out(mf.video->data[0], fmt.width, fmt.height, mf.video->linesize[0], QImage::Format_ARGB32);
            return out.copy();
        };
        const QImage a = renderWith(true), b = renderWith(false);
        QVERIFY(!a.isNull() && !b.isNull());
        qunsetenv("PLAYPLAY_NO_OVERLAY_CROP");
        int diff = 0;
        for (int y = 0; y < fmt.height; y += 3) for (int x = 0; x < fmt.width; x += 3) if (a.pixel(x, y) != b.pixel(x, y)) ++diff;
        QCOMPARE(diff, 0);
        QCOMPARE(qBlue(a.pixel(500, 560)) > 150, true);             // the bar is there
        QCOMPARE(a.pixel(500, 100), b.pixel(500, 100));
    }

    void gpuMatchesCpu() {
        const OutputFormat fmt = fmt720();
        QString err;
        auto gpu = GpuCompositor::create(fmt.width, fmt.height, &err);
        if (!gpu) QSKIP(qPrintable("no D3D11 device: " + err));
        qInfo() << "GPU adapter:" << gpu->adapterName();

        // Build a representative draw list: full program, a semi-transparent
        // scaled overlay, a clipped (wipe) layer, and a translated push.
        FramePool pool(fmt.width, fmt.height, kMixerPixelFormat);
        auto fill = [&](int w, int h, QColor col) {
            QImage img(w, h, QImage::Format_ARGB32); img.fill(col);
            return Mixer::imageToFrame(img);
        };
        FramePtr prog = fill(1280, 720, QColor(40, 90, 200));
        FramePtr over = fill(300, 200, QColor(255, 200, 0, 160));
        FramePtr wipe = fill(1280, 720, QColor(0, 180, 60));
        std::vector<GpuCompositor::DrawOp> ops;
        ops.push_back({prog, QRectF(0, 0, 1280, 720), QRectF(), 1.0, QRectF(), false});
        ops.push_back({wipe, QRectF(0, 0, 1280, 720), QRectF(), 1.0, QRectF(0, 0, 500, 720), false});
        ops.push_back({over, QRectF(700, 100, 450, 300), QRectF(0, 0, 300, 200), 0.7, QRectF(), false});
        ops.push_back({prog, QRectF(-400, 0, 1280, 720), QRectF(), 0.5, QRectF(), false});
        FramePtr gpuOut = pool.get();
        QVERIFY(gpu->render(ops, gpuOut));
        const double firstMs = gpu->lastRenderMs();
        QVERIFY(gpu->render(ops, gpuOut));
        QVERIFY(gpu->render(ops, gpuOut));
        qInfo() << "GPU render ms: first" << firstMs << "steady" << gpu->lastRenderMs();

        // CPU reference with the same semantics as Compositor::renderCpu.
        FramePtr cpuOut = pool.get();
        QImage canvas(cpuOut->data[0], fmt.width, fmt.height, cpuOut->linesize[0], QImage::Format_ARGB32_Premultiplied);
        canvas.fill(QColor(0, 0, 0, 255));
        QPainter p(&canvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (const auto& op : ops) {
            const QImage src(op.frame->data[0], op.frame->width, op.frame->height, op.frame->linesize[0], QImage::Format_ARGB32);
            p.save(); p.setOpacity(op.opacity); if (!op.clip.isNull()) p.setClipRect(op.clip);
            p.drawImage(op.dst, src, op.src.isNull() ? QRectF(0, 0, op.frame->width, op.frame->height) : op.src);
            p.restore();
        }
        p.end();

        // Compare away from edges (filtering differs by half a pixel at boundaries).
        int maxDiff = 0; qint64 sum = 0; int n = 0;
        for (int y = 4; y < fmt.height - 4; y += 3) for (int x = 4; x < fmt.width - 4; x += 3) {
            const QRgb a = px(gpuOut, x, y), b = px(cpuOut, x, y);
            const int d = qMax(qMax(std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b))), std::abs(qBlue(a) - qBlue(b)));
            if (std::abs(x - 500) <= 2 || (std::abs(x - 700) <= 2) || std::abs(x - 1150) <= 2 || std::abs(y - 100) <= 2 || std::abs(y - 400) <= 2 || std::abs(x - 880) <= 2) continue;
            maxDiff = qMax(maxDiff, d); sum += d; ++n;
        }
        const double mean = double(sum) / qMax(1, n);
        qInfo() << "GPU vs CPU: max diff" << maxDiff << "mean" << mean << "render ms" << gpu->lastRenderMs();
        QVERIFY2(maxDiff <= 6, qPrintable(QString("max diff %1").arg(maxDiff)));
        QVERIFY(mean < 1.0);
    }

    void compositorGpuPathRenders() {
        const OutputFormat fmt = fmt720();
        Compositor c(fmt);
        c.setGpuEnabled(true);
        if (!c.gpuActive()) QSKIP("no GPU");
        c.programTake(openSrc(color("#ff0000"), fmt), Compositor::Transition(), 0);
        QJsonObject spec; spec["id"] = 20;
        QJsonObject s; s["type"] = "color"; s["color"] = "#00ff00"; s["width"] = 100; s["height"] = 100; spec["source"] = s;
        QJsonObject pl; pl["x"] = 10; pl["y"] = 10; pl["width"] = 100; pl["height"] = 100; spec["placement"] = pl;
        QVERIFY(c.addLayer(Compositor::LayerSpec::fromJson(spec), 0));
        MediaFrame mf = c.render(0, 0.0);
        QCOMPARE(qGreen(px(mf.video, 50, 50)), 255);
        QCOMPARE(qRed(px(mf.video, 500, 500)), 255);
        QVERIFY(c.status()["gpu"].toBool());
    }

    void sourceFactoryErrors() {
        QString err;
        QVERIFY(!LayerSource::create(QJsonObject{{"type", "bogus"}}, &err));
        QVERIFY(err.contains("unknown"));
        err.clear();
        auto html = LayerSource::create(QJsonObject{{"type", "html"}, {"url", "http://x"}}, &err);
        QVERIFY(html);                         // html is always built now (headless Chromium over DevTools); opening is what can fail
        QCOMPARE(html->type(), QString("html"));
        // Spec round trip and role defaults
        QJsonObject spec; spec["role"] = "legal_id"; spec["source"] = QJsonObject{{"type", "bars"}};
        const auto ls = Compositor::LayerSpec::fromJson(spec);
        QCOMPARE(ls.id, 80);
        QCOMPARE(Compositor::LayerSpec::fromJson(ls.toJson()).id, 80);
    }

    void loudnessMeterSine() {
        LoudnessMeter m(48000, 2);
        std::vector<float> l(4800), r(4800);
        for (int blk = 0; blk < 40; ++blk) {      // 4 s
            for (int i = 0; i < 4800; ++i) { const double ph = 2 * M_PI * 1000.0 * (blk * 4800 + i) / 48000.0; l[i] = r[i] = float(std::sin(ph) * std::pow(10.0, -23.0 / 20.0)); }
            const float* planes[2] = { l.data(), r.data() };
            m.process(planes, 4800);
        }
        // BS.1770 calibration point: a 1 kHz sine at -23 dBFS peak in both channels
        // reads -23.0 LKFS (per channel -26.0 dB mean square, +0.69 dB pre-filter gain
        // at 1 kHz, +3.0 dB for two channels, -0.691 dB offset).
        QVERIFY2(std::fabs(m.momentaryLufs() - (-23.0)) < 0.3, qPrintable(QString("M=%1").arg(m.momentaryLufs())));
        QVERIFY2(std::fabs(m.integratedLufs() - m.shortTermLufs()) < 0.5, qPrintable(QString("I=%1 S=%2").arg(m.integratedLufs()).arg(m.shortTermLufs())));
        QVERIFY(std::fabs(m.peakDbfs() - (-23.0)) < 0.1);
    }

    // ---- PE-05: meters / status / layers readers --------------------------------------------------------------------

    // The seqlock hands out whole stores only: a writer storing a 4 KB payload (every word = the store number) while
    // three readers copy it never yields a mixed copy. A prefix store leaves the rest as the previous store put it.
    void seqlockNeverTears() {
        struct alignas(8) Payload { quint64 w[512]; };
        auto lock = std::make_unique<metersnap::SeqLock<Payload>>();
        std::atomic<bool> stop{false};
        std::atomic<qint64> torn{0}, loads{0}, retries{0};
        std::thread writer([&] {
            auto p = std::make_unique<Payload>();
            for (quint64 v = 1; !stop.load(); ++v) {
                for (quint64& x : p->w) x = v;
                lock->store(*p);
                QElapsedTimer t; t.start(); while (t.nsecsElapsed() < 5000) {}      // ~5 us between stores (render: 33 ms)
            }
        });
        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) readers.emplace_back([&] {
            auto q = std::make_unique<Payload>();
            while (!stop.load()) {
                retries += lock->load(*q); loads++;
                for (quint64 x : q->w) if (x != q->w[0]) { torn++; break; }
            }
        });
        QThread::msleep(400);
        stop = true;
        writer.join();
        for (std::thread& t : readers) t.join();
        qInfo().noquote() << QString("seqlock: %1 stores, %2 loads, %3 retries, %4 torn").arg(lock->stores()).arg(loads.load()).arg(retries.load()).arg(torn.load());
        QCOMPARE(torn.load(), qint64(0));
        QVERIFY(loads.load() > 1000);
        QVERIFY(lock->stores() > 1000);

        auto a = std::make_unique<Payload>(), b = std::make_unique<Payload>(), c = std::make_unique<Payload>();
        for (quint64& x : a->w) x = 7;
        for (quint64& x : b->w) x = 9;
        lock->store(*a);
        lock->store(*b, 10 * 8);
        lock->load(*c);
        QCOMPARE(c->w[0], quint64(9)); QCOMPARE(c->w[9], quint64(9)); QCOMPARE(c->w[10], quint64(7)); QCOMPARE(c->w[511], quint64(7));
        QCOMPARE(metersnap::Snapshot::bytesFor(metersnap::kMaxMixes), sizeof(metersnap::Snapshot));
        QVERIFY(metersnap::Snapshot::bytesFor(1) < sizeof(metersnap::Snapshot) / 2);      // one mix: ~7 KB per tick
    }

    // The snapshot readers report exactly what the old locked readers do, through renders, fader / solo / mix changes and
    // layer removal (integrated loudness is refreshed about once a second, so it is compared within 0.1 LU).
    void snapshotJsonMatchesLegacy() {
        OutputFormat fmt = fmt720(); fmt.channels = 2;
        auto build = [&](bool legacy) {
            LegacyEnv env(legacy);
            auto c = std::make_unique<Compositor>(fmt);
            c->setMix("web", QJsonObject{{"name", "Web"}, {"strips", QJsonObject{{"program", QJsonObject{{"gainDb", -6.0}}}}}});
            c->setMix("aux", QJsonObject{{"name", "Aux"}, {"follow", false}, {"master", QJsonObject{{"gainDb", -10.0}}}});   // a second extra mix: own meters
            c->programTake(openSrc(QJsonObject{{"type", "tone"}, {"toneHz", 1000}, {"toneDbfs", -20.0}}, fmt), Compositor::Transition(), 0);
            const QJsonObject box{{"type", "template"}, {"kind", "box"}, {"color", "#ffffff"}, {"bgColor", "#2040a0"}, {"width", 400}, {"height", 100}};
            c->addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 30}, {"role", "lower_third"}, {"source", box}, {"x", 100}, {"y", 500}, {"width", 400}, {"height", 100}}), 0);
            c->addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 61}, {"role", "audio"}, {"mixes", QJsonArray{"web"}},
                {"source", QJsonObject{{"type", "tone"}, {"toneHz", 400}, {"toneDbfs", -18.0}, {"label", "Line 1"}, {"width", 2}, {"height", 2}}}}), 0);
            c->addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 62}, {"duck", true},
                {"source", QJsonObject{{"type", "tone"}, {"toneHz", 600}, {"toneDbfs", -24.0}, {"width", 2}, {"height", 2}}}}), 0);
            return c;
        };
        std::unique_ptr<Compositor> old = build(true), snap = build(false);
        QVERIFY(old->legacyStatus());
        QVERIFY(!snap->legacyStatus());
        auto run = [&](qint64 from, qint64 to) {
            for (qint64 f = from; f < to; ++f) for (Compositor* c : {old.get(), snap.get()}) { std::map<QString, MediaFrame> mx; c->render(f, f / 30.0, &mx); }
        };
        auto splitIntegrated = [](QJsonObject st, double* integrated) {
            QJsonObject a = st["audio"].toObject(); *integrated = a["integratedLufs"].toDouble(); a.remove("integratedLufs"); st["audio"] = a; return st;
        };
        // Integrated loudness: the snapshot recomputes it on the first tick a second after the last (frames 0, 30, 60 ...
        // here), so after a loudness change it can trail by up to a second of history; at a refresh tick it is exact.
        auto same = [&](const QString& when, bool meters, double integratedTol = 0.5) {
            if (meters) {
                const QJsonObject a = old->metersJson(), b = snap->metersJson();
                QVERIFY2(a == b, qPrintable(QString("%1: metersJson differs\n legacy   %2\n snapshot %3").arg(when, compactJson(a), compactJson(b))));
            }
            double ia = 0.0, ib = 0.0;
            const QJsonObject sa = splitIntegrated(old->status(), &ia), sb = splitIntegrated(snap->status(), &ib);
            QVERIFY2(sa == sb, qPrintable(QString("%1: status differs\n legacy   %2\n snapshot %3").arg(when, compactJson(sa), compactJson(sb))));
            QVERIFY2(std::fabs(ia - ib) <= integratedTol, qPrintable(QString("%1: integrated %2 vs %3 LUFS (tolerance %4)").arg(when).arg(ia).arg(ib).arg(integratedTol)));
            const QJsonArray la = old->layersJson(), lb = snap->layersJson();
            QVERIFY2(la == lb, qPrintable(QString("%1: layersJson differs\n legacy   %2\n snapshot %3").arg(when, compactJson(la), compactJson(lb))));
        };
        // Before the first tick metersJson differs on purpose: the snapshot lists an audio layer from its first tick on.
        same("before any render", false);
        if (QTest::currentTestFailed()) return;
        run(0, 90);
        same("after 3 s", true);
        if (QTest::currentTestFailed()) return;
        QVERIFY(snap->metersJson()["sources"].toObject().contains("L62"));
        {   // the two extra mixes really meter differently (aux: no follow, -10 dB master), so a mix-up would show
            const QJsonArray mx = snap->metersJson()["mixes"].toArray();
            QCOMPARE(mx.size(), 3);
            QVERIFY2(std::fabs(mx[1].toObject()["momentaryLufs"].toDouble() - mx[2].toObject()["momentaryLufs"].toDouble()) > 3.0, qPrintable(compactJson(mx)));
        }
        for (Compositor* c : {old.get(), snap.get()}) {
            c->setMix("web", QJsonObject{{"strips", QJsonObject{{"L61", QJsonObject{{"gainDb", -3.0}}}, {"program", QJsonObject{{"mute", true}}}}}});
            c->setSolo("L61");
            c->updateLayer(61, QJsonObject{{"audioGainDb", -2.0}});
            c->hideLayer(62, Compositor::Transition(), 90);
        }
        run(90, 96);
        same("after solo, mute, gain and hide", true);
        if (QTest::currentTestFailed()) return;
        for (Compositor* c : {old.get(), snap.get()}) { c->setSolo(QString()); c->removeLayer(61); c->removeMix("web"); c->setMix("web2", QJsonObject{{"name", "Web 2"}}); }
        run(96, 100);
        same("after removing a layer and a mix", true);
        if (QTest::currentTestFailed()) return;
        run(100, 121);                                                  // frame 120 is a refresh tick (90 + 30)
        same("at an integrated-loudness refresh tick", true, 0.0);
    }

    // A reader inside a slow source status() (80 ms) neither delays render nor outlives the source: a layer removed
    // meanwhile is parked until the reader is out, and an update() is applied only after it (never overlapping status()).
    // With PLAYPLAY_COMP_LEGACY_STATUS=1 the same render waits for the reader.
    void slowSourceStatusDoesNotBlockRender() {
        const OutputFormat fmt = fmt720();
        for (bool legacy : {false, true}) {
            LegacyEnv env(legacy);
            Compositor c(fmt);
            int retired = 0;
            c.setRetireHandler([&](std::unique_ptr<LayerSource> s) { ++retired; s.reset(); });
            c.programTake(openSrc(color("#202020"), fmt), Compositor::Transition(), 0);
            auto slow = std::make_shared<ProbeState>(), fast = std::make_shared<ProbeState>();
            slow->statusMs = 80;
            for (const auto& p : {std::make_pair(70, slow), std::make_pair(71, fast)}) {
                auto s = std::make_unique<ProbeSource>(p.second); s->open(fmt);
                c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", p.first}, {"role", "audio"}, {"source", QJsonObject{{"type", "probe"}}}}), std::move(s), 0);
            }
            for (qint64 f = 0; f < 3; ++f) c.render(f, f / 30.0);
            std::thread reader([&] { (void)c.layersJson(); });         // asks layer 70 (80 ms), then 71
            struct Joiner { std::thread& t; ~Joiner() { if (t.joinable()) t.join(); } } joiner{reader};
            QElapsedTimer wait; wait.start();
            while (slow->inStatus.load() == 0 && wait.elapsed() < 3000) QThread::yieldCurrentThread();
            QCOMPARE(slow->inStatus.load(), 1);
            QElapsedTimer t; t.start();
            c.render(3, 0.1);
            const double renderMs = t.nsecsElapsed() / 1e6;
            qInfo().noquote() << QString("%1: render while a reader is inside an 80 ms status(): %2 ms").arg(legacy ? "legacy" : "PE-05").arg(renderMs, 0, 'f', 2);
            if (legacy) { QVERIFY2(renderMs > 40.0, qPrintable(QString("legacy render %1 ms").arg(renderMs))); continue; }
            QVERIFY2(renderMs < 30.0, qPrintable(QString("render waited %1 ms for a reader").arg(renderMs)));
            t.restart();
            QVERIFY(c.updateLayer(71, QJsonObject{{"text", "after"}}));
            const double updateMs = t.nsecsElapsed() / 1e6;
            QVERIFY2(updateMs < 30.0, qPrintable(QString("updateLayer waited %1 ms").arg(updateMs)));
            QCOMPARE(fast->updates.load(), 0);                         // deferred: the reader may still ask layer 71
            QVERIFY(c.removeLayer(70));
            c.render(4, 0.13);
            const bool stillInside = slow->inStatus.load() == 1;
            if (stillInside) { QCOMPARE(slow->destroyed.load(), 0); QCOMPARE(retired, 0); }   // parked, not handed on
            reader.join();
            c.render(5, 0.16);                                         // the next tick hands it on and applies the update
            QCOMPARE(retired, 1);
            QCOMPARE(slow->destroyed.load(), 1);
            QCOMPARE(slow->destroyedDuringStatus.load(), 0);
            QCOMPARE(fast->updates.load(), 1);
            QCOMPARE(fast->updateDuringStatus.load(), 0);
            const QJsonArray layers = c.layersJson();
            QCOMPARE(layers.size(), 1);
            QCOMPARE(layers[0].toObject()["sourceStatus"].toObject()["text"].toString(), QString("after"));
        }
    }

    // Acceptance (lane plan PE-05): a thread calls metersJson() at 100 Hz (status() and layersJson() every 10th call)
    // during 300 renders; render max must stay at the no-reader baseline and render must not wait for the lock. The
    // scene has 6 audio strips, 2 mixes and one source whose status() takes 3 ms. With PLAYPLAY_COMP_LEGACY_STATUS=1
    // the lock wait shows up (the test can see the regression).
    void metersReaderDoesNotSlowRender() {
        struct Run { double renderAvg = 0, renderP99 = 0, renderMax = 0, lockAvg = 0, lockMax = 0, readAvg = 0, readMax = 0; int reads = 0, badReads = 0; };
        OutputFormat fmt = fmt720(); fmt.channels = 2;
        auto measure = [&](bool legacy, bool withReader) {
            LegacyEnv env(legacy);
            Compositor c(fmt);
            c.setMix("web", QJsonObject{{"name", "Web"}});
            c.programTake(openSrc(QJsonObject{{"type", "bars"}}, fmt), Compositor::Transition(), 0);
            const QJsonObject box{{"type", "template"}, {"kind", "box"}, {"color", "#ffffff"}, {"bgColor", "#2040a0"}, {"width", 600}, {"height", 110}};
            c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 30}, {"role", "lower_third"}, {"source", box}, {"x", 100}, {"y", 560}, {"width", 600}, {"height", 110}}), 0);
            c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 45}, {"role", "legal_id"}, {"source", QJsonObject{{"type", "template"}, {"kind", "clock"}, {"format", "hh:mm:ss"}}},
                {"placement", QJsonObject{{"anchor", "top_left"}, {"margin", 40}}}}), 0);
            for (int i = 0; i < 4; ++i)
                c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 60 + i}, {"role", "audio"}, {"mixes", QJsonArray{i % 2 ? "web" : "main"}},
                    {"source", QJsonObject{{"type", "tone"}, {"toneHz", 300 + 100 * i}, {"toneDbfs", -24.0}, {"width", 2}, {"height", 2}}}}), 0);
            auto probe = std::make_shared<ProbeState>(); probe->statusMs = 3;
            { auto s = std::make_unique<ProbeSource>(probe); s->open(fmt); c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 70}, {"role", "audio"}, {"source", QJsonObject{{"type", "probe"}}}}), std::move(s), 0); }
            for (qint64 f = 0; f < 30; ++f) { std::map<QString, MediaFrame> mx; c.render(f, f / 30.0, &mx); }   // warm up
            std::atomic<bool> stop{false};
            std::atomic<int> reads{0}, bad{0};
            std::atomic<qint64> readNsSum{0}, readNsMax{0};
            std::thread reader;
            if (withReader) reader = std::thread([&] {
                QElapsedTimer clock; clock.start();
                for (int i = 0; !stop.load(); ++i) {
                    while (!stop.load() && clock.nsecsElapsed() < qint64(i) * 10000000LL) QThread::yieldCurrentThread();   // 100 Hz
                    if (stop.load()) break;
                    QElapsedTimer t; t.start();
                    const QJsonObject m = c.metersJson();
                    if (i % 10 == 5) { (void)c.status(); (void)c.layersJson(); }
                    const qint64 ns = t.nsecsElapsed();
                    readNsSum += ns; if (ns > readNsMax.load()) readNsMax = ns;
                    if (m["strips"].toArray().size() != 6 || m["sources"].toObject().size() != 6 || m["mixes"].toArray().size() != 2) bad++;
                    reads++;
                }
            });
            tickprof::TickProfiler prof(true);
            std::vector<double> ms;
            {
                tickprof::TickProfiler::Bind bind(prof);                // render's own "lock" phase = its wait for the mutex
                QElapsedTimer pace; pace.start();
                for (qint64 f = 30; f < 330; ++f) {
                    while (pace.nsecsElapsed() < (f - 30) * 4000000LL) QThread::yieldCurrentThread();     // one render every 4 ms
                    prof.beginTick(f);
                    QElapsedTimer t; t.start();
                    std::map<QString, MediaFrame> mx;
                    c.render(f, f / 30.0, &mx);
                    ms.push_back(t.nsecsElapsed() / 1e6);
                    prof.endTick();
                }
            }
            stop = true;
            if (reader.joinable()) reader.join();
            const tickprof::Window w = prof.takeWindow();
            Run r;
            double sum = 0; for (double x : ms) sum += x;
            std::sort(ms.begin(), ms.end());
            r.renderAvg = sum / double(ms.size()); r.renderP99 = ms[size_t(double(ms.size()) * 0.99)]; r.renderMax = ms.back();
            r.lockAvg = w.phaseAvgMs(tickprof::Lock); r.lockMax = w.phases[tickprof::Lock].maxMs;
            r.reads = reads.load(); r.badReads = bad.load();
            r.readAvg = r.reads ? readNsSum.load() / 1e6 / r.reads : 0.0; r.readMax = readNsMax.load() / 1e6;
            return r;
        };
        auto line = [](const QString& name, const Run& r) {
            return QString("%1: render avg %2 ms p99 %3 max %4 | lock wait avg %5 max %6 ms | reads %7 (avg %8 ms, max %9) bad %10")
                .arg(name).arg(r.renderAvg, 0, 'f', 3).arg(r.renderP99, 0, 'f', 3).arg(r.renderMax, 0, 'f', 3).arg(r.lockAvg, 0, 'f', 4).arg(r.lockMax, 0, 'f', 3)
                .arg(r.reads).arg(r.readAvg, 0, 'f', 3).arg(r.readMax, 0, 'f', 3).arg(r.badReads);
        };
        const Run legacyBase = measure(true, false), legacyReader = measure(true, true);
        qInfo().noquote() << line("legacy, no reader      ", legacyBase);
        qInfo().noquote() << line("legacy, 100 Hz reader  ", legacyReader);
        QVERIFY(legacyReader.reads >= 60);
        QVERIFY2(legacyReader.lockMax > 1.5, qPrintable("the old reader holds the render mutex through a 3 ms status(): " + line("legacy", legacyReader)));
        bool ok = false;
        for (int attempt = 1; attempt <= 3 && !ok; ++attempt) {
            const Run base = measure(false, false), withReader = measure(false, true);
            qInfo().noquote() << line(QString("PE-05, no reader (#%1)  ").arg(attempt), base);
            qInfo().noquote() << line(QString("PE-05, 100 Hz reader(#%1)").arg(attempt), withReader);
            QVERIFY(withReader.reads >= 60);
            QCOMPARE(withReader.badReads, 0);
            // Render max is a single sample: allow 25 % + 1 ms of scheduler noise; the lock wait must be gone outright.
            ok = withReader.lockMax < 1.0 && withReader.renderMax <= base.renderMax * 1.25 + 1.0;
            if (!ok) qWarning().noquote() << QString("attempt %1 outside the bound (lock max %2 ms, render max %3 vs %4 ms); measuring again")
                                                 .arg(attempt).arg(withReader.lockMax, 0, 'f', 3).arg(withReader.renderMax, 0, 'f', 3).arg(base.renderMax, 0, 'f', 3);
        }
        QVERIFY2(ok, "with a 100 Hz metersJson reader, render waited for the lock or its max exceeded the no-reader baseline (3 attempts)");
    }

    // ---- PE-08: premultiplied alpha end to end ----------------------------------------------------------------------

    // PLAYPLAY_PREMUL decides, per source and when it is created, whether a template hands out the painter's premultiplied
    // pixels. Unset and "0" give exactly the straight frame the pre-PE-08 code made (Mixer::imageToFrame of the render).
    void premulSwitchAndFrames() {
        const OutputFormat fmt = fmt720();
        const QJsonObject spec{{"type", "template"}, {"kind", "lower_third"}, {"title", "Half and half"}, {"subtitle", "edge alpha"}};
        const QImage painted = TemplateSource::render(spec, fmt, 0.0);
        QCOMPARE(painted.format(), QImage::Format_ARGB32_Premultiplied);
        const QImage straightBytes = frameImage(Mixer::imageToFrame(painted));             // the old frame
        const QImage premulBytes = frameImage(Mixer::imageToFramePremultiplied(painted));  // the painter's own bytes
        QVERIFY(premulBytes == QImage(painted.constBits(), painted.width(), painted.height(), painted.bytesPerLine(), QImage::Format_ARGB32));
        QVERIFY(premulBytes != straightBytes);                                              // translucent enough to tell apart
        struct Seen { bool premul = false; QJsonObject status; QImage frame; };
        auto make = [&](const char* env) {
            Seen s; PremulEnv e(env);
            TemplateSource src(spec);
            if (!src.open(fmt)) return s;
            s.premul = src.deliversPremultiplied(); s.status = src.status(); s.frame = frameImage(src.video(0, 0.0));
            return s;
        };
        const Seen unset = make(nullptr), off = make("0"), on = make("1"), onWord = make("on");
        QVERIFY(!unset.frame.isNull() && !off.frame.isNull() && !on.frame.isNull());
        QVERIFY(!unset.premul && !off.premul && on.premul && onWord.premul);
        QVERIFY(unset.frame == straightBytes);
        QVERIFY(off.frame == straightBytes);
        QVERIFY(on.frame == premulBytes);
        QVERIFY(!unset.status.contains("premultiplied") && !off.status.contains("premultiplied"));
        QVERIFY(on.status.value("premultiplied").toBool());
        // Read once: a source made with the switch on stays premultiplied after the variable is gone.
        std::unique_ptr<TemplateSource> kept;
        { PremulEnv e("1"); kept = std::make_unique<TemplateSource>(spec); }
        QVERIFY(kept->open(fmt));
        QVERIFY(kept->deliversPremultiplied());
        QVERIFY(frameImage(kept->video(0, 0.0)) == premulBytes);
        // Opaque RGB32 is valid premultiplied already and is copied unchanged.
        QImage opaque(64, 32, QImage::Format_RGB32); opaque.fill(QColor(10, 200, 30));
        QVERIFY(frameImage(Mixer::imageToFramePremultiplied(opaque)) == QImage(opaque.constBits(), 64, 32, opaque.bytesPerLine(), QImage::Format_ARGB32));
        // Sources that do not opt in stay straight whatever the switch says.
        PremulEnv e("1");
        QVERIFY(!openSrc(color("#123456"), fmt)->deliversPremultiplied());
    }

    // Edge cases: every alpha 0..255 of white, black, orange and violet, drawn 1:1, scaled 2x and at 50 % layer opacity
    // over white and over black, on the CPU and the GPU. The premultiplied picture gives the straight one's output within
    // ±2 per channel, and both match the analytic blend (a path that premultiplied twice would put half-alpha white over
    // black near 64 instead of 128). GPU and CPU agree within ±2 at 1:1 and ±6 inside the scaled rect.
    void premulEdgeAlpha() {
        const OutputFormat fmt = fmt720();
        const QImage ramp = alphaRamp();
        const QRect r11(100, 100, 256, 64), r2x(420, 100, 512, 128), rHalf(100, 300, 256, 64);
        auto render = [&](bool premul, const char* program, bool gpu) -> QImage {
            Compositor c(fmt);
            if (gpu) { c.setGpuEnabled(true); c.setGpuAuto(false); if (!c.gpuActive()) return QImage(); }
            c.programTake(openSrc(color(program), fmt), Compositor::Transition(), 0);
            const struct { int id; QRect r; double opacity; } ls[] = {{30, r11, 1.0}, {31, r2x, 1.0}, {32, rHalf, 0.5}};
            for (const auto& l : ls)
                c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", l.id}, {"opacity", l.opacity},
                               {"placement", QJsonObject{{"x", l.r.x()}, {"y", l.r.y()}, {"width", l.r.width()}, {"height", l.r.height()}}}}),
                           std::make_unique<RgbaSource>(ramp, premul), 0);
            return frameImage(c.render(0, 0.0).video);
        };
        auto scaledEdge = [&](int x, int y) { return r2x.adjusted(-2, -2, 2, 2).contains(x, y) && !r2x.adjusted(2, 2, -2, -2).contains(x, y); };
        auto expectBlend = [&](const QImage& out, const QRect& r, double opacity, QRgb bg, int tol, const QString& what) {
            int worst = 0; QString at;
            for (int y = 0; y < 64; ++y) for (int x = 0; x < 256; ++x) {
                const QRgb c = ramp.pixel(x, y), o = out.pixel(r.x() + x, r.y() + y);
                const double a = qAlpha(c) / 255.0 * opacity;
                auto ch = [&](int s, int b) { return qRound(s * a + b * (1.0 - a)); };
                const int d = qMax(qMax(std::abs(qRed(o) - ch(qRed(c), qRed(bg))), std::abs(qGreen(o) - ch(qGreen(c), qGreen(bg)))),
                                   std::abs(qBlue(o) - ch(qBlue(c), qBlue(bg))));
                if (d > worst) { worst = d; at = QString("alpha %1, band %2").arg(x).arg(y / 16); }
            }
            QVERIFY2(worst <= tol, qPrintable(QString("%1: %2 off the analytic blend (%3)").arg(what).arg(worst).arg(at)));
        };
        bool haveGpu = true;
        for (const char* program : {"#ffffff", "#000000"}) {
            const QRgb bg = QColor(program).rgb();
            const QImage cs = render(false, program, false), cp = render(true, program, false);
            QVERIFY(!cs.isNull() && !cp.isNull());
            const ImgDiff d = diffImages(cs, cp);
            qInfo().noquote() << "PE-08 edge alpha over" << program << "CPU straight vs premultiplied:" << d.text();
            QVERIFY2(d.maxDiff <= 2, qPrintable(d.text()));
            expectBlend(cs, r11, 1.0, bg, 2, "cpu straight 1:1");      expectBlend(cp, r11, 1.0, bg, 2, "cpu premultiplied 1:1");
            expectBlend(cs, rHalf, 0.5, bg, 3, "cpu straight 50 %");   expectBlend(cp, rHalf, 0.5, bg, 3, "cpu premultiplied 50 %");
            const QImage gs = render(false, program, true), gp = render(true, program, true);
            if (gs.isNull() || gp.isNull()) { haveGpu = false; continue; }
            const ImgDiff g = diffImages(gs, gp, scaledEdge);
            qInfo().noquote() << "PE-08 edge alpha over" << program << "GPU straight vs premultiplied:" << g.text();
            QVERIFY2(g.maxDiff <= 2, qPrintable(g.text()));
            expectBlend(gs, r11, 1.0, bg, 2, "gpu straight 1:1");      expectBlend(gp, r11, 1.0, bg, 2, "gpu premultiplied 1:1");
            expectBlend(gs, rHalf, 0.5, bg, 3, "gpu straight 50 %");   expectBlend(gp, rHalf, 0.5, bg, 3, "gpu premultiplied 50 %");
            const ImgDiff x11 = diffImages(gp, cp, [&](int x, int y) { return r2x.adjusted(-2, -2, 2, 2).contains(x, y); });
            const ImgDiff x2 = diffImages(gp, cp, [&](int x, int y) { return !r2x.adjusted(2, 2, -2, -2).contains(x, y); });
            qInfo().noquote() << "PE-08 edge alpha over" << program << "GPU vs CPU premultiplied: 1:1" << x11.text() << "| scaled" << x2.text();
            QVERIFY2(x11.maxDiff <= 2, qPrintable(x11.text()));
            QVERIFY2(x2.maxDiff <= 6, qPrintable(x2.text()));
        }
        if (!haveGpu) qInfo() << "PE-08 edge alpha: no D3D11 device, GPU half skipped";
    }

    // The template scene (bug, fading lower third, ticker, countdown, 50 % white and black text, a scaled translucent box,
    // a faded text) over black, white and a mid colour. PLAYPLAY_PREMUL unset and "0" draw, bit for bit, what an
    // independent QPainter reference of the pre-PE-08 path draws (straight frames painted onto the premultiplied canvas);
    // "1" stays within ±2 per channel of "0" at frames 0, 6 and 12, and the 50 % texts land on the analytic value at full
    // glyph coverage (white over black 128, black over white 127, ...) in both modes.
    void premulTemplatesMatchStraight() {
        const OutputFormat fmt = fmt720();
        const std::vector<CgLayer> scene = cgScene();
        std::map<int, QRect> rects; for (const CgLayer& l : scene) rects[l.id] = cgRect(l, fmt);
        const QList<int> keep = {0, 6, 12};
        auto reference = [&](const char* program) {      // the old renderCpu by hand, frame 0 (the lower third is still at opacity 0)
            QImage canvas(fmt.width, fmt.height, QImage::Format_ARGB32_Premultiplied);
            canvas.fill(QColor(program));
            QPainter p(&canvas);
            p.setRenderHint(QPainter::SmoothPixmapTransform, true);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
            for (const CgLayer& l : scene) {
                if (l.fadeIn) continue;
                const QImage straight = TemplateSource::render(l.src, fmt, 0.0).convertToFormat(QImage::Format_ARGB32);
                p.save(); p.setOpacity(l.opacity);
                p.drawImage(QRectF(rects[l.id]), straight, QRectF(0, 0, straight.width(), straight.height()));
                p.restore();
            }
            p.end();
            return QImage(canvas.constBits(), fmt.width, fmt.height, canvas.bytesPerLine(), QImage::Format_ARGB32).copy();
        };
        std::map<QString, std::map<int, QImage>> runs;           // "<program>|<env>"
        for (const char* program : {"#000000", "#ffffff", "#3070a0"}) {
            for (const char* env : {(const char*)nullptr, "0", "1"}) {
                runs[QString("%1|%2").arg(program, env ? env : "unset")] = renderCgScene(env, program, false, keep);
                QCOMPARE(int(runs[QString("%1|%2").arg(program, env ? env : "unset")].size()), 3);
            }
            const auto& unset = runs[QString("%1|unset").arg(program)];
            const auto& off = runs[QString("%1|0").arg(program)];
            const auto& on = runs[QString("%1|1").arg(program)];
            const QImage ref = reference(program);
            const ImgDiff ru = diffImages(unset.at(0), ref), r0 = diffImages(off.at(0), ref), r1 = diffImages(on.at(0), ref);
            qInfo().noquote() << QString("PE-08 template scene over %1, frame 0 vs the pre-PE-08 reference: unset %2 | 0 %3 | 1 %4").arg(program, ru.text(), r0.text(), r1.text());
            QVERIFY2(ru.differing == 0, qPrintable("PLAYPLAY_PREMUL unset: " + ru.text()));
            QVERIFY2(r0.differing == 0, qPrintable("PLAYPLAY_PREMUL=0: " + r0.text()));
            QVERIFY2(r1.maxDiff <= 2, qPrintable("PLAYPLAY_PREMUL=1: " + r1.text()));
            for (int f : keep) {
                QVERIFY2(diffImages(unset.at(f), off.at(f)).differing == 0, "unset and 0 are the same path");
                const ImgDiff d = diffImages(off.at(f), on.at(f));
                qInfo().noquote() << QString("PE-08 template scene over %1, frame %2: straight vs premultiplied %3").arg(program).arg(f).arg(d.text());
                QVERIFY2(d.maxDiff <= 2, qPrintable(d.text()));
            }
        }
        // 50 % texts at full glyph coverage (template alpha exactly 128), over the bare program.
        for (const int id : {50, 51}) {
            const CgLayer& l = *std::find_if(scene.begin(), scene.end(), [&](const CgLayer& x) { return x.id == id; });
            const QImage glyphs = TemplateSource::render(l.src, fmt, 0.0);
            std::vector<QPoint> solid;                  // over the bare program: not under any other layer's rect
            auto covered = [&](const QPoint& pt) { for (const auto& kv : rects) if (kv.first != id && kv.second.contains(pt)) return true; return false; };
            for (int y = 0; y < glyphs.height(); ++y) for (int x = 0; x < glyphs.width(); ++x) {
                const QPoint pt = rects[id].topLeft() + QPoint(x, y);
                if (qAlpha(glyphs.pixel(x, y)) == 128 && !covered(pt)) solid.push_back(pt);
            }
            QVERIFY2(solid.size() > 200, qPrintable(QString("only %1 fully covered glyph pixels in layer %2").arg(solid.size()).arg(id)));
            for (const char* program : {"#000000", "#ffffff"}) {
                const int text = id == 50 ? 255 : 0, bg = QColor(program).red();
                const int want = qRound(text * 128 / 255.0 + bg * 127 / 255.0);
                for (const char* env : {"0", "1"}) {
                    const QImage& img = runs[QString("%1|%2").arg(program, env)].at(0);
                    int worst = 0;
                    for (const QPoint& pt : solid) { const QRgb o = img.pixel(pt); worst = qMax(worst, qMax(qMax(std::abs(qRed(o) - want), std::abs(qGreen(o) - want)), std::abs(qBlue(o) - want))); }
                    qInfo().noquote() << QString("PE-08 50 % text layer %1 over %2, PLAYPLAY_PREMUL=%3: %4 full-coverage pixels, want %5, worst %6 off")
                                             .arg(id).arg(program, env).arg(solid.size()).arg(want).arg(worst);
                    QVERIFY2(worst <= 2, qPrintable(QString("layer %1 over %2 with PLAYPLAY_PREMUL=%3: %4 off %5 at full coverage").arg(id).arg(program, env).arg(worst).arg(want)));
                }
            }
        }
    }

    // The same scene composited on the GPU: premultiplied and straight agree within ±2 per channel, and the premultiplied
    // GPU picture matches the CPU one within ±2 on the 1:1 layers (±6 inside the scaled box, whose filtering differs).
    void premulTemplatesGpuMatchesCpu() {
        const QList<int> keep = {6};
        const auto gs = renderCgScene("0", "#3070a0", true, keep), gp = renderCgScene("1", "#3070a0", true, keep);
        if (gs.empty() || gp.empty()) QSKIP("no D3D11 device");
        const auto cp = renderCgScene("1", "#3070a0", false, keep);
        QCOMPARE(int(cp.size()), 1);
        const QRect box = cgRect(cgScene()[6], fmt720());
        QCOMPARE(cgScene()[6].id, 52);
        const ImgDiff s = diffImages(gs.at(6), gp.at(6), [&](int x, int y) { return box.adjusted(-2, -2, 2, 2).contains(x, y) && !box.adjusted(2, 2, -2, -2).contains(x, y); });
        const ImgDiff x11 = diffImages(gp.at(6), cp.at(6), [&](int x, int y) { return box.adjusted(-2, -2, 2, 2).contains(x, y); });
        const ImgDiff x2 = diffImages(gp.at(6), cp.at(6), [&](int x, int y) { return !box.adjusted(2, 2, -2, -2).contains(x, y); });
        qInfo().noquote() << "PE-08 template scene GPU straight vs premultiplied:" << s.text();
        qInfo().noquote() << "PE-08 template scene GPU vs CPU premultiplied: 1:1" << x11.text() << "| scaled box" << x2.text();
        QVERIFY2(s.maxDiff <= 2, qPrintable(s.text()));
        QVERIFY2(x11.maxDiff <= 2, qPrintable(x11.text()));
        QVERIFY2(x2.maxDiff <= 6, qPrintable(x2.text()));
    }

    // A premultiplied program picture is never passed through, so outputs stay straight BGRA: a 50 % red template as the
    // program comes out opaque dark red (red over black), alone or under a layer, CPU and GPU. With the switch off the
    // template is passed through untouched (straight 255,0,0 at alpha 128), exactly as before.
    void premulProgramIsCompositedOntoBlack() {
        const OutputFormat fmt = fmt720();
        const QJsonObject spec{{"type", "template"}, {"kind", "box"}, {"color", "#80ff0000"}, {"width", fmt.width}, {"height", fmt.height}, {"radius", 0}};
        auto centre = [&](const char* env, bool gpu, bool withLayer, bool* ran) -> QRgb {
            PremulEnv e(env);
            Compositor c(fmt);
            *ran = true;
            if (gpu) { c.setGpuEnabled(true); c.setGpuAuto(false); if (!c.gpuActive()) { *ran = false; return 0; } }
            c.programTake(openSrc(spec, fmt), Compositor::Transition(), 0);
            if (withLayer)
                c.addLayer(Compositor::LayerSpec::fromJson(QJsonObject{{"id", 20}, {"source", QJsonObject{{"type", "color"}, {"color", "#00ff00"}, {"width", 50}, {"height", 50}}},
                                                                       {"placement", QJsonObject{{"x", 10}, {"y", 10}, {"width", 50}, {"height", 50}}}}), 0);
            const MediaFrame mf = c.render(0, 0.0);
            if (withLayer && qGreen(px(mf.video, 30, 30)) != 255) qWarning() << "layer missing";
            return px(mf.video, fmt.width / 2, fmt.height / 2);
        };
        bool ran = false;
        const QRgb straight = centre("0", false, false, &ran);
        QCOMPARE(qRed(straight), 255); QCOMPARE(qAlpha(straight), 128);         // zero copy, as before
        for (bool layer : {false, true}) {
            const QRgb p = centre("1", false, layer, &ran);
            QVERIFY2(qAlpha(p) == 255 && std::abs(qRed(p) - 128) <= 1 && qGreen(p) == 0 && qBlue(p) == 0,
                     qPrintable(QString("cpu%1: %2,%3,%4,%5").arg(layer ? " + layer" : "").arg(qRed(p)).arg(qGreen(p)).arg(qBlue(p)).arg(qAlpha(p))));
        }
        const QRgb g = centre("1", true, false, &ran);
        if (!ran) QSKIP("no D3D11 device for the GPU half");
        QVERIFY2(qAlpha(g) == 255 && std::abs(qRed(g) - 128) <= 1 && qGreen(g) == 0,
                 qPrintable(QString("gpu: %1,%2,%3,%4").arg(qRed(g)).arg(qGreen(g)).arg(qBlue(g)).arg(qAlpha(g))));
    }
};

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestCompositor tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_compositor.moc"
