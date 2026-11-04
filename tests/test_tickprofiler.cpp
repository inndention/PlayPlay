#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QThread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include "playout/engine/tickprofiler.h"
#include "playout/engine/engine.h"
#include "playout/engine/compositor/compositor.h"
#include "playout/engine/mediaframe.h"

/**
 * PE-06 (docs/plans/perf-engine.md): the tick phase profiler behind `telemetry.tickPhases`.
 *   accumulatesPhases        per-phase sum/avg/max over a window, tick avg/max, nested phases
 *   p95AndRing               tick p95 is exact up to 256 ticks a window, then over the window's last 256
 *   windowsAndWorst          takeWindow starts an empty window; the tick in progress lands in the next one; the worst
 *                            tick since start carries on with its own phase split and slowest layer
 *   scopedPhaseAndBinding    ScopedPhase times real time, stop() ends it once, nothing is recorded on a thread without a
 *                            bound profiler or outside a tick
 *   slowestLayerTypeCache    the slowest layer-source call wins; type() is asked once per source per window
 *   disabledIsInert          PLAYPLAY_TICK_PROFILE=0: not bound, nothing recorded, the window says disabled
 *   jsonShape                the telemetry JSON keys
 *   overheadPerTick          13 phases + 4 source calls a tick cost a few microseconds at most
 *   compositorNamesSlowLayer a Compositor tick with a slow layer source: layers phase and slowestLayer name it (video
 *                            and audio), and render without a bound profiler records nothing
 *   engineTelemetry          PlayoutEngine's 1 Hz telemetry carries tickPhases (render >= its parts, the CG layer named),
 *                            PLAYPLAY_TICK_PROFILE_LOG=1 logs it, PLAYPLAY_TICK_PROFILE=0 removes it
 */
class TestTickProfiler : public QObject {
    Q_OBJECT
private slots:
    void accumulatesPhases();
    void p95AndRing();
    void windowsAndWorst();
    void scopedPhaseAndBinding();
    void slowestLayerTypeCache();
    void disabledIsInert();
    void jsonShape();
    void overheadPerTick();
    void compositorNamesSlowLayer();
    void engineTelemetry();
};

namespace {

using namespace tickprof;

void spinMs(double ms) { const qint64 end = nowNs() + qint64(ms * 1e6); while (nowNs() < end) {} }

/// A layer source whose video() / audio() take a set time (a stalled receiver), with a counter on type().
class SlowSource : public LayerSource {
public:
    SlowSource(double videoMs, double audioMs, const QString& type, QSize size = QSize(64, 32))
        : m_videoMs(videoMs), m_audioMs(audioMs), m_type(type), m_size(size) {}
    QString type() const override { ++typeCalls; return m_type; }
    bool open(const OutputFormat&) override {
        m_frame = allocVideoFrame(m_size.width(), m_size.height(), kMixerPixelFormat);
        for (int y = 0; y < m_size.height(); ++y) memset(m_frame->data[0] + y * m_frame->linesize[0], 0xff, size_t(m_size.width()) * 4);
        m_open = true; return true;
    }
    void close() override { m_frame.reset(); m_open = false; }
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64, double) override { if (m_videoMs > 0) spinMs(m_videoMs); return m_frame; }      // spin: Sleep() rounds to the timer tick
    bool hasAudio() const override { return m_audioMs >= 0; }
    bool audio(const FramePtr&, qint64, double) override { if (m_audioMs > 0) spinMs(m_audioMs); return false; }
    QSize nativeSize() const override { return m_size; }
    mutable std::atomic<int> typeCalls{0};
private:
    double m_videoMs, m_audioMs;
    QString m_type;
    QSize m_size;
    FramePtr m_frame;
    bool m_open = false;
};

struct FakeSrc { QString t; mutable int calls = 0; QString type() const { ++calls; return t; } };

double phaseMax(const QJsonObject& tp, const char* name) { return tp["phases"].toObject()[name].toObject()["maxMs"].toDouble(); }
double phaseAvg(const QJsonObject& tp, const char* name) { return tp["phases"].toObject()[name].toObject()["avgMs"].toDouble(); }

} // namespace

void TestTickProfiler::accumulatesPhases() {
    TickProfiler p(true);
    // 10 ticks: commands 0.1*i, render 1.0 with program 0.4 inside it, fanout only on even ticks
    for (int i = 1; i <= 10; ++i) {
        p.beginTick(i);
        p.add(Commands, 0.1 * i);
        p.add(Render, 1.0); p.add(Program, 0.4);
        if (i % 2 == 0) p.add(FanOut, 2.0);
        p.add(Composite, 0.25); p.add(Composite, 0.25);          // a phase may run more than once a tick (one per mix)
        p.endTick(2.0 + 0.1 * i);
    }
    const Window w = p.takeWindow();
    QVERIFY(w.enabled);
    QCOMPARE(w.ticks, 10);
    QVERIFY(std::fabs(w.phaseAvgMs(Commands) - 0.55) < 1e-9);
    QVERIFY(std::fabs(w.phases[Commands].maxMs - 1.0) < 1e-9);
    QVERIFY(std::fabs(w.phaseAvgMs(Render) - 1.0) < 1e-9);
    QVERIFY(std::fabs(w.phaseAvgMs(Program) - 0.4) < 1e-9);
    QVERIFY(std::fabs(w.phaseAvgMs(FanOut) - 1.0) < 1e-9);      // averaged over every tick, ran on half of them
    QVERIFY(std::fabs(w.phases[FanOut].maxMs - 2.0) < 1e-9);
    QVERIFY(std::fabs(w.phases[Composite].maxMs - 0.5) < 1e-9);
    QCOMPARE(w.phases[Snapshot].maxMs, 0.0);
    QVERIFY(std::fabs(w.tickAvgMs - 2.55) < 1e-9);
    QVERIFY(std::fabs(w.tickMaxMs - 3.0) < 1e-9);
    QVERIFY(std::fabs(w.slowestTick.ms - 3.0) < 1e-9);
    QCOMPARE(w.slowestTick.frame, qint64(10));
    QVERIFY(std::fabs(w.slowestTick.phases[Commands] - 1.0) < 1e-9);
    QVERIFY(!w.slowestLayer.valid());                           // no layer source was called
    // adds outside a tick are ignored
    p.add(Commands, 50.0);
    p.beginTick(11); p.endTick(1.0);
    QCOMPARE(p.takeWindow().phases[Commands].maxMs, 0.0);
}

void TestTickProfiler::p95AndRing() {
    TickProfiler p(true);
    for (int i = 1; i <= 100; ++i) { p.beginTick(i); p.endTick(double(i)); }
    Window w = p.takeWindow();
    QCOMPARE(w.ticks, 100);
    QCOMPARE(w.tickP95Ms, 95.0);
    QCOMPARE(w.tickMaxMs, 100.0);
    QVERIFY(std::fabs(w.tickAvgMs - 50.5) < 1e-9);
    // one tick: p95 = that tick
    p.beginTick(1); p.endTick(7.0);
    QCOMPARE(p.takeWindow().tickP95Ms, 7.0);
    // 300 ticks (a window far longer than a second): p95 over the last 256 (45..300 -> 95th percentile of those)
    for (int i = 1; i <= 300; ++i) { p.beginTick(i); p.endTick(double(i)); }
    w = p.takeWindow();
    QCOMPARE(w.ticks, 300);
    QCOMPARE(w.tickMaxMs, 300.0);
    QCOMPARE(w.tickP95Ms, 45.0 + std::ceil(0.95 * 256) - 1);
    // empty window
    w = p.takeWindow();
    QCOMPARE(w.ticks, 0); QCOMPARE(w.tickP95Ms, 0.0); QCOMPARE(w.tickAvgMs, 0.0);
}

void TestTickProfiler::windowsAndWorst() {
    TickProfiler p(true);
    FakeSrc ndi{"ndi"};
    p.beginTick(5); p.add(Layers, 190.0); p.noteSource(20, &ndi, "video", 189.0, [&] { return ndi.type(); }); p.endTick(200.0);
    p.beginTick(6); p.add(Layers, 0.1); p.endTick(1.0);
    Window w1 = p.takeWindow();
    QCOMPARE(w1.worst.frame, qint64(5));
    QCOMPARE(w1.worst.ms, 200.0);
    QVERIFY(w1.worst.utcMs > 0);
    // the tick in progress at takeWindow lands in the next window
    p.beginTick(7); p.add(Commands, 0.5);
    Window w2 = p.takeWindow();
    QCOMPARE(w2.ticks, 0);
    QVERIFY(!w2.slowestTick.valid());
    QVERIFY(!w2.slowestLayer.valid());
    QCOMPARE(w2.phases[Commands].maxMs, 0.0);
    QCOMPARE(w2.worst.frame, qint64(5));                        // carries on
    QCOMPARE(w2.worst.phases[Layers], 190.0);
    QVERIFY(w2.worst.slowest.valid());
    QCOMPARE(w2.worst.slowest.layer, 20);
    QCOMPARE(w2.worst.slowest.type, QString("ndi"));
    p.endTick(2.0);
    Window w3 = p.takeWindow();
    QCOMPARE(w3.ticks, 1);
    QCOMPARE(w3.phases[Commands].maxMs, 0.5);
    QCOMPARE(w3.slowestTick.frame, qint64(7));
    QCOMPARE(w3.worst.frame, qint64(5));
    // a slower tick replaces the worst
    p.beginTick(8); p.endTick(250.0);
    QCOMPARE(p.takeWindow().worst.frame, qint64(8));
}

void TestTickProfiler::scopedPhaseAndBinding() {
    TickProfiler p(true);
    QVERIFY(TickProfiler::current() == nullptr);
    {
        TickProfiler::Bind b(p);
        QVERIFY(TickProfiler::current() == &p);
        p.beginTick(1);
        {
            ScopedPhase render(Render);                 // bound profiler
            { ScopedPhase prog(p, Program); spinMs(2.0); }
            ScopedPhase lk(Lock); spinMs(1.0); lk.stop(); lk.stop();      // stop() once, the destructor adds nothing more
            spinMs(20.0);                                                  // counted again, lock would read >= 21 ms
        }
        // another thread has no profiler bound: its phases go nowhere
        std::thread([&] { QVERIFY(TickProfiler::current() == nullptr); ScopedPhase other(Monitor); spinMs(1.0); }).join();
        p.endTick();
    }
    QVERIFY(TickProfiler::current() == nullptr);                // Bind restored the previous (none)
    const Window w = p.takeWindow();
    QCOMPARE(w.ticks, 1);
    const double prog = w.phases[Program].maxMs, lock = w.phases[Lock].maxMs, render = w.phases[Render].maxMs;
    QVERIFY2(prog >= 1.99 && prog < 50.0, qPrintable(QString::number(prog)));
    QVERIFY2(lock >= 0.99 && lock < 15.0, qPrintable(QString::number(lock)));       // generous: a busy box may preempt the spin
    QVERIFY2(render >= prog + lock + 19.9, qPrintable(QString("%1 vs %2 + %3").arg(render).arg(prog).arg(lock)));
    QCOMPARE(w.phases[Monitor].maxMs, 0.0);
    QVERIFY(w.tickMaxMs >= render);                             // measured from beginTick
    // a ScopedPhase on a bound profiler outside a tick records nothing
    {
        TickProfiler::Bind b(p);
        ScopedPhase s(Commands); spinMs(0.2);
    }
    QCOMPARE(p.takeWindow().phases[Commands].maxMs, 0.0);
    // nested Bind restores the outer profiler
    TickProfiler q(true);
    {
        TickProfiler::Bind b1(p);
        { TickProfiler::Bind b2(q); QVERIFY(TickProfiler::current() == &q); }
        QVERIFY(TickProfiler::current() == &p);
    }
}

void TestTickProfiler::slowestLayerTypeCache() {
    TickProfiler p(true);
    TickProfiler::Bind b(p);
    FakeSrc color{"color"}, ndi{"ndi"}, tpl{"template"};
    auto note = [&](int layer, FakeSrc& s, const char* call, double ms) { p.noteSource(layer, &s, call, ms, [&] { return s.type(); }); };
    for (int t = 0; t < 30; ++t) {                    // 30 ticks, ndi is the slowest every tick but tick 12
        p.beginTick(t);
        note(10, color, "video", 0.2);
        note(20, ndi, "video", t == 12 ? 0.1 : 3.0 + t * 0.01);
        note(40, tpl, "video", t == 12 ? 9.0 : 1.0);
        note(20, ndi, "audio", 0.05);
        p.endTick(10.0);
    }
    Window w = p.takeWindow();
    QVERIFY(w.slowestLayer.valid());
    QCOMPARE(w.slowestLayer.layer, 40);               // 9 ms in tick 12 beats ndi's 3.29
    QCOMPARE(w.slowestLayer.type, QString("template"));
    QCOMPARE(QString(w.slowestLayer.call), QString("video"));
    QCOMPARE(w.slowestLayer.ms, 9.0);
    QCOMPARE(w.slowestTick.slowest.layer, 20);        // every tick took 10 ms: the first (tick 0) is the window's slowest
    // type() was asked at most once per source in the window (cache), never per tick
    QVERIFY2(color.calls <= 1 && ndi.calls == 1 && tpl.calls == 1, qPrintable(QString("%1 %2 %3").arg(color.calls).arg(ndi.calls).arg(tpl.calls)));
    // the cache is per window: a new window asks again once
    p.beginTick(31); note(20, ndi, "audio", 4.0); p.endTick(5.0);
    w = p.takeWindow();
    QCOMPARE(ndi.calls, 2);
    QCOMPARE(w.slowestLayer.layer, 20);
    QCOMPARE(QString(w.slowestLayer.call), QString("audio"));
    // SourceCall on a real LayerSource
    SlowSource slow(3.0, -1, "slowtest");
    p.beginTick(40);
    { SourceCall sc(77, static_cast<LayerSource*>(&slow), "video"); slow.video(40, 0.0); }
    { LayerSource* none = nullptr; SourceCall sc(78, none, "video"); }      // no source: nothing
    p.endTick();
    w = p.takeWindow();
    QCOMPARE(w.slowestLayer.layer, 77);
    QCOMPARE(w.slowestLayer.type, QString("slowtest"));
    QVERIFY(w.slowestLayer.ms >= 2.9);
}

void TestTickProfiler::disabledIsInert() {
    qputenv("PLAYPLAY_TICK_PROFILE", "0");
    QVERIFY(!TickProfiler::enabledByEnvironment());
    TickProfiler p;                                     // from the environment
    qunsetenv("PLAYPLAY_TICK_PROFILE");
    QVERIFY(TickProfiler::enabledByEnvironment());
    QVERIFY(!p.enabled());
    {
        TickProfiler::Bind b(p);
        QVERIFY(TickProfiler::current() == nullptr);    // a disabled profiler is never bound
        p.beginTick(1);
        { ScopedPhase s(p, Render); spinMs(0.2); }
        p.add(Commands, 5.0);
        p.endTick(3.0);
    }
    const Window w = p.takeWindow();
    QVERIFY(!w.enabled);
    QCOMPARE(w.ticks, 0);
    QCOMPARE(w.phases[Render].maxMs, 0.0);
    for (const char* off : {"off", "false", "FALSE"}) { qputenv("PLAYPLAY_TICK_PROFILE", off); QVERIFY(!TickProfiler::enabledByEnvironment()); }
    qputenv("PLAYPLAY_TICK_PROFILE", "1"); QVERIFY(TickProfiler::enabledByEnvironment());
    qunsetenv("PLAYPLAY_TICK_PROFILE");
}

void TestTickProfiler::jsonShape() {
    TickProfiler p(true);
    FakeSrc ndi{"ndi"};
    p.beginTick(100); p.add(Render, 0.5); p.add(Layers, 0.4); p.noteSource(15, &ndi, "video", 0.3456, [&] { return ndi.type(); }); p.endTick(0.75);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject o = p.takeWindow().toJson(now + 1500);
    QCOMPARE(o["ticks"].toInt(), 1);
    QVERIFY(o.contains("windowSec"));
    const QJsonObject tick = o["tick"].toObject();
    QCOMPARE(tick["avgMs"].toDouble(), 0.75); QCOMPARE(tick["p95Ms"].toDouble(), 0.75); QCOMPARE(tick["maxMs"].toDouble(), 0.75);
    const QJsonObject ph = o["phases"].toObject();
    QCOMPARE(ph.size(), int(PhaseCount));
    for (const char* n : {"commands", "advance", "render", "lock", "program", "layers", "composite", "fetch", "audio", "fanout", "preview", "monitor", "snapshot"}) {
        QVERIFY2(ph.contains(n), n);
        QVERIFY(ph[n].toObject().contains("avgMs") && ph[n].toObject().contains("maxMs"));
    }
    for (const char* n : {"lock", "program", "layers", "composite", "fetch", "audio"}) QCOMPARE(ph[n].toObject()["in"].toString(), QString("render"));
    for (const char* n : {"commands", "render", "fanout", "snapshot"}) QVERIFY(!ph[n].toObject().contains("in"));
    QCOMPARE(ph["layers"].toObject()["maxMs"].toDouble(), 0.4);
    const QJsonObject sl = o["slowestLayer"].toObject();
    QCOMPARE(sl["layer"].toInt(), 15); QCOMPARE(sl["type"].toString(), QString("ndi")); QCOMPARE(sl["call"].toString(), QString("video"));
    QCOMPARE(sl["ms"].toDouble(), 0.346);                       // rounded to the microsecond
    const QJsonObject st = o["slowestTick"].toObject();
    QCOMPARE(st["frame"].toDouble(), 100.0); QCOMPARE(st["ms"].toDouble(), 0.75);
    QCOMPARE(st["phases"].toObject().size(), int(PhaseCount));
    QCOMPARE(st["slowestLayer"].toObject()["layer"].toInt(), 15);
    const QJsonObject wo = o["worst"].toObject();
    QCOMPARE(wo["frame"].toDouble(), 100.0);
    QVERIFY2(wo["ageSec"].toDouble() >= 1.4 && wo["ageSec"].toDouble() <= 1.7, qPrintable(QString::number(wo["ageSec"].toDouble())));
    // an empty window has no slowestLayer / slowestTick, and keeps the worst
    const QJsonObject e = p.takeWindow().toJson();
    QVERIFY(!e.contains("slowestLayer") && !e.contains("slowestTick") && e.contains("worst"));
    qInfo().noquote() << "tickPhases sample:" << QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void TestTickProfiler::overheadPerTick() {
    TickProfiler p(true);
    FakeSrc srcs[4] = {{"color"}, {"template"}, {"ndi"}, {"html"}};
    const int ticks = 20000;
    TickProfiler::Bind b(p);
    QElapsedTimer t; t.start();
    for (int i = 0; i < ticks; ++i) {
        p.beginTick(i);
        for (int ph = 0; ph < PhaseCount; ++ph) { ScopedPhase s{static_cast<Phase>(ph)}; }
        for (int l = 0; l < 4; ++l) { const FakeSrc* s = &srcs[l]; SourceCall<FakeSrc> sc(10 * (l + 1), s, "video"); }
        p.endTick();
        if (i % 30 == 29) p.takeWindow();
    }
    const double usPerTick = t.nsecsElapsed() / 1e3 / ticks;
    qInfo().noquote() << QString("TICKPROF overhead: %1 us per tick (13 phases + 4 source calls + endTick, a window every 30 ticks)").arg(usPerTick, 0, 'f', 3);
    QVERIFY2(usPerTick < 10.0, qPrintable(QString::number(usPerTick)));
}

void TestTickProfiler::compositorNamesSlowLayer() {
    OutputFormat fmt; fmt.width = 320; fmt.height = 180;
    Compositor c(fmt);
    auto colorSrc = [&](const char* col) { auto s = LayerSource::create(QJsonObject{{"type", "color"}, {"color", col}}); s->open(fmt); return s; };
    c.programTake(colorSrc("#000000"), Compositor::Transition(), 0);
    auto add = [&](int id, std::unique_ptr<LayerSource> s) {
        s->open(fmt);
        Compositor::LayerSpec spec = Compositor::LayerSpec::fromJson(QJsonObject{{"id", id}, {"placement", QJsonObject{{"x", id}, {"y", 10}, {"width", 64}, {"height", 32}}}});
        c.addLayer(spec, std::move(s), 0);
    };
    auto* slowRaw = new SlowSource(12.0, -1, "slowtest");
    add(30, std::make_unique<SlowSource>(0.0, -1, "fasttest"));
    add(40, std::unique_ptr<LayerSource>(slowRaw));
    add(50, std::make_unique<SlowSource>(0.0, 6.0, "slowaudio"));       // audio() is the slow call

    // without a bound profiler render works and nothing is timed
    TickProfiler idle(true);
    QVERIFY(c.render(0, 0.0).video);
    QCOMPARE(idle.takeWindow().ticks, 0);

    TickProfiler p(true);
    for (qint64 f = 1; f <= 5; ++f) {
        TickProfiler::Bind b(p);
        p.beginTick(f);
        MediaFrame mf;
        { ScopedPhase r(p, Render); mf = c.render(f, f / 30.0); }
        QVERIFY(mf.video && mf.audio);
        p.endTick();
    }
    Window w = p.takeWindow();
    QCOMPARE(w.ticks, 5);
    QVERIFY(w.slowestLayer.valid());
    QCOMPARE(w.slowestLayer.layer, 40);
    QCOMPARE(w.slowestLayer.type, QString("slowtest"));
    QCOMPARE(QString(w.slowestLayer.call), QString("video"));
    QVERIFY2(w.slowestLayer.ms >= 11.5, qPrintable(QString::number(w.slowestLayer.ms)));
    QVERIFY2(slowRaw->typeCalls.load() <= 1, qPrintable(QString::number(slowRaw->typeCalls.load())));   // cached per window
    QVERIFY2(w.phases[Layers].maxMs >= 11.5, qPrintable(QString::number(w.phases[Layers].maxMs)));
    QVERIFY2(w.phases[Audio].maxMs >= 5.5, qPrintable(QString::number(w.phases[Audio].maxMs)));
    QVERIFY(w.phases[Composite].maxMs > 0.0);                   // three layers over the program: a CPU paint per tick
    QVERIFY(w.phases[Program].maxMs > 0.0);
    QCOMPARE(w.phases[Fetch].maxMs, 0.0);                       // CPU path: no GPU readback
    for (int ph = Lock; ph <= Audio; ++ph) QVERIFY2(w.phases[size_t(ph)].sumMs <= w.phases[Render].sumMs + 1e-6, phaseName(ph));
    QVERIFY(w.phases[Render].sumMs >= w.phases[Layers].sumMs + w.phases[Audio].sumMs);
    QVERIFY(w.slowestTick.slowest.valid() && w.slowestTick.slowest.layer == 40);

    // the audio call is named when it is the slowest: make the video stall go away
    c.removeLayer(40);
    for (qint64 f = 6; f <= 8; ++f) { TickProfiler::Bind b(p); p.beginTick(f); c.render(f, f / 30.0); p.endTick(); }
    w = p.takeWindow();
    QCOMPARE(w.slowestLayer.layer, 50);
    QCOMPARE(w.slowestLayer.type, QString("slowaudio"));
    QCOMPARE(QString(w.slowestLayer.call), QString("audio"));
    QVERIFY(w.slowestLayer.ms >= 5.5);
    QVERIFY(w.phases[Layers].maxMs < 5.0);
}

void TestTickProfiler::engineTelemetry() {
    auto runEngine = [](double seconds, QList<QJsonObject>* telemetry) {
        PlayoutEngine::Config cfg;
        cfg.channelId = "perftest-tickprof"; cfg.channelName = "perftest";
        cfg.frameRate = FrameRate::ntsc(); cfg.width = 320; cfg.height = 180; cfg.hwDecode = false;
        auto engine = std::make_unique<PlayoutEngine>(cfg);
        if (!engine->initialize()) return engine;
        QObject::connect(engine.get(), &PlayoutEngine::telemetry, [telemetry](const QJsonObject& st) { telemetry->append(st); });
        // a CG layer (opened on a loader thread, attached a few ticks later) so the layer stack has a source to name
        engine->handleCgCommand(QJsonObject{{"action", "show"}, {"layer", 30},
                                            {"source", QJsonObject{{"type", "color"}, {"color", "#ffffff"}, {"width", 64}, {"height", 32}}},
                                            {"placement", QJsonObject{{"x", 10}, {"y", 10}, {"width", 64}, {"height", 32}}}});
        const FrameRate r = cfg.frameRate;
        QElapsedTimer t; t.start();
        for (qint64 f = 0; t.elapsed() < qint64(seconds * 1000); ++f) { engine->onFrameTick(f, r.frameToNs(f)); QTest::qWait(30); }
        QTest::qWait(50);                                       // queued log lines
        engine->shutdown();
        return engine;
    };

    // capture "[Engine] tickPhases" log lines (PLAYPLAY_TICK_PROFILE_LOG=1)
    static QMutex logMutex; static QStringList logLines; static QtMessageHandler prev = nullptr;
    prev = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        if (msg.startsWith("[Engine] tickPhases")) { QMutexLocker l(&logMutex); logLines << msg; }
        else if (prev) prev(type, ctx, msg);
    });
    qputenv("PLAYPLAY_TICK_PROFILE_LOG", "1");
    QList<QJsonObject> tel;
    runEngine(2.6, &tel);
    qunsetenv("PLAYPLAY_TICK_PROFILE_LOG");
    qInstallMessageHandler(prev);

    QVERIFY2(tel.size() >= 3, qPrintable(QString::number(tel.size())));
    const QJsonObject tp = tel.last()["telemetry"].toObject()["tickPhases"].toObject();
    qInfo().noquote() << "engine tickPhases:" << QJsonDocument(tp).toJson(QJsonDocument::Compact);
    QVERIFY(!tp.isEmpty());
    QVERIFY2(tp["ticks"].toInt() >= 10 && tp["ticks"].toInt() <= 60, qPrintable(QString::number(tp["ticks"].toInt())));   // ~1 s of ~30-45 ms ticks
    QVERIFY(tp["windowSec"].toDouble() > 0.8 && tp["windowSec"].toDouble() < 1.6);
    QCOMPARE(tp["phases"].toObject().size(), int(PhaseCount));
    const double tickAvg = tp["tick"].toObject()["avgMs"].toDouble(), tickMax = tp["tick"].toObject()["maxMs"].toDouble();
    QVERIFY(tickAvg > 0.0 && tickMax >= tp["tick"].toObject()["p95Ms"].toDouble() && tp["tick"].toObject()["p95Ms"].toDouble() >= 0.0);
    QVERIFY(phaseAvg(tp, "render") > 0.0 && phaseAvg(tp, "snapshot") > 0.0);     // slate + layer: a CPU paint every tick
    QVERIFY(phaseAvg(tp, "render") + 1e-3 >= phaseAvg(tp, "program") + phaseAvg(tp, "layers") + phaseAvg(tp, "audio") - 3e-3);   // rounding
    QVERIFY(tickAvg + 1e-3 >= phaseAvg(tp, "render"));
    QVERIFY(phaseMax(tp, "render") <= tickMax + 1e-3);
    QVERIFY2(tp.contains("slowestLayer"), "the CG layer was never timed");
    QCOMPARE(tp["slowestLayer"].toObject()["layer"].toInt(), 30);
    QCOMPARE(tp["slowestLayer"].toObject()["type"].toString(), QString("color"));
    QVERIFY(tp.contains("slowestTick") && tp.contains("worst"));
    QVERIFY(tp["worst"].toObject()["ms"].toDouble() >= tickMax - 1e-3);
    // the other telemetry keys are unchanged
    const QJsonObject t = tel.last()["telemetry"].toObject();
    for (const char* k : {"tickMsLast", "tickMsMax", "lateTicks", "decoderQueue", "audioUnderruns", "outputDrops", "missedCuts"}) QVERIFY2(t.contains(k), k);
    // the opt-in log line carries the same JSON
    { QMutexLocker l(&logMutex);
      QVERIFY2(logLines.size() >= 2, qPrintable(QString::number(logLines.size())));
      const QJsonObject logged = QJsonDocument::fromJson(logLines.last().mid(QString("[Engine] tickPhases ").size()).toUtf8()).object();
      QVERIFY(logged.contains("phases") && logged.contains("tick")); }

    // off switch: no key
    qputenv("PLAYPLAY_TICK_PROFILE", "0");
    QList<QJsonObject> off;
    runEngine(1.2, &off);
    qunsetenv("PLAYPLAY_TICK_PROFILE");
    QVERIFY(!off.isEmpty());
    for (const QJsonObject& st : off) QVERIFY(!st["telemetry"].toObject().contains("tickPhases"));
    QVERIFY(off.last()["telemetry"].toObject().contains("tickMsLast"));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    TestTickProfiler tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_tickprofiler.moc"
