// LibObsSource against a real headless libobs (docs/libobs-spike.md). Built only with PLAYPLAY_WITH_LIBOBS=ON;
// skipped (QSKIP) when the libobs install or the staged obs.dll is missing.
//
// Measures, at 1080p29.97 with colour scenes:
//   (a) frame age at pull time (libobs frame timestamp vs the pull) p50 / p99 / max over 120 pulled frames
//   (b) cut latency: ticks from a "switch" command to the first pulled frame that shows the new scene
//   (c) process CPU and working-set delta: before libobs starts / libobs source open / source closed
// Numbers are printed as "[measure] key=value" lines so they can be lifted into the doc verbatim.
#include <QtTest>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#include <timeapi.h>      // timeBeginPeriod (WIN32_LEAN_AND_MEAN leaves mmsystem out)
#endif
#include "compositor/libobssource.h"
#include "compositor/basicsources.h"

namespace {

OutputFormat fmt1080() { OutputFormat f; f.width = 1920; f.height = 1080; f.rate = FrameRate(30000, 1001); return f; }
QRgb px(const FramePtr& f, int x, int y) { return reinterpret_cast<const QRgb*>(f->data[0] + y * f->linesize[0])[x]; }
bool isRed(QRgb c) { return qRed(c) > 220 && qGreen(c) < 40 && qBlue(c) < 40; }
bool isBlue(QRgb c) { return qBlue(c) > 220 && qGreen(c) < 40 && qRed(c) < 40; }

QJsonObject colorItem(const QString& name, const QString& hex) {
    QJsonObject it; it["id"] = "color_source_v3"; it["name"] = name; QJsonObject s; s["color"] = hex; it["settings"] = s; return it;
}
QJsonObject scene(const QString& name, const QJsonArray& items) { QJsonObject s; s["name"] = name; s["items"] = items; return s; }
QJsonObject spec(const QJsonArray& scenes, const QString& program, const QJsonObject& transition = QJsonObject()) {
    QJsonObject o; o["type"] = "libobs"; o["scenes"] = scenes; o["program"] = program;
    if (!transition.isEmpty()) o["transition"] = transition;
    return o;
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return -1;
    const size_t k = size_t(p * double(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + qint64(k), v.end());
    return v[k];
}

struct ProcStats {
    double cpuSec = 0, wsMb = 0, privMb = 0, wall = 0;
    static ProcStats now() {
        ProcStats s;
#ifdef Q_OS_WIN
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        auto ft = [](const FILETIME& f) { return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        s.cpuSec = ft(k) + ft(u);
        PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            s.wsMb = double(pmc.WorkingSetSize) / 1048576.0; s.privMb = double(pmc.PrivateUsage) / 1048576.0;
        }
#endif
        s.wall = double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1e6;
        return s;
    }
};

/// Sleeps to successive 29.97 ticks (steady clock), like the engine's FrameClock would.
struct Ticker {
    std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now();
    std::chrono::nanoseconds period{33366667};
    void wait() { next += period; std::this_thread::sleep_until(next); }
};

struct Phase { double cpuPct = 0, wsMb = 0, privMb = 0; int nonNull = 0, distinct = 0; };
Phase pullFor(LayerSource* src, int frames) {
    Ticker tk; Phase p;
    const ProcStats a = ProcStats::now();
    const void* last = nullptr;
    for (int i = 0; i < frames; ++i) {
        tk.wait();
        FramePtr f = src ? src->video(i, i / 29.97) : FramePtr();
        if (f) { p.nonNull++; if (f.get() != last) p.distinct++; last = f.get(); }
    }
    const ProcStats b = ProcStats::now();
    p.cpuPct = 100.0 * (b.cpuSec - a.cpuSec) / (b.wall - a.wall);
    p.wsMb = b.wsMb; p.privMb = b.privMb;
    return p;
}

} // namespace

class TestLibObsSource : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        timeBeginPeriod(1);
#endif
        const QString root = LibObsSource::defaultRoot();
        if (!QFileInfo::exists(root + "/bin/64bit/obs.dll")) QSKIP(qPrintable("no libobs install at " + root));
        if (!QFileInfo::exists(QCoreApplication::applicationDirPath() + "/obs.dll")) QSKIP("obs.dll not staged next to the test (build target stage_libobs)");
        qInfo() << "[measure] libobs_root=" << root;
    }
    void cleanupTestCase() { LibObsSource::shutdownRuntime(); }

    // (c) CPU / working set: baseline (libobs never started) -> source open -> source closed (runtime idle)
    void cpuAndMemory() {
        const OutputFormat fmt = fmt1080();
        QJsonObject bars; bars["type"] = "bars";
        std::unique_ptr<LayerSource> base = LayerSource::create(bars);
        QVERIFY(base && base->open(fmt));
        const Phase off = pullFor(base.get(), 120);           // 4 s at 29.97, before libobs exists in the process
        QVERIFY(!LibObsSource::runtimeStarted());

        // random: test-input's noise source (every frame differs); colour behind it fills the raster.
        QJsonObject noise; noise["id"] = "random"; noise["name"] = "noise"; noise["fit"] = "inner";
        QJsonObject sz; sz["width"] = 960; sz["height"] = 540; noise["size"] = sz;
        QJsonObject pos; pos["x"] = 480; pos["y"] = 270; noise["pos"] = pos;
        QJsonArray items; items << colorItem("bg", "#2040c0") << noise;
        std::unique_ptr<LayerSource> obs = LayerSource::create(spec({scene("A", items)}, "A"));
        QVERIFY(obs);
        const ProcStats beforeOpen = ProcStats::now();
        QVERIFY2(obs->open(fmt), qPrintable(obs->lastError()));
        QVERIFY(LibObsSource::runtimeStarted());
        const QJsonObject st0 = obs->status().value("obs").toObject();
        qInfo().noquote() << "[measure] libobs_version=" << st0.value("version").toString() << "modules=" << QJsonDocument(st0.value("modules").toArray()).toJson(QJsonDocument::Compact);
        if (!st0.value("modules").toArray().contains("test-input")) qWarning() << "test-input module not loaded; noise item will be missing";
        for (int i = 0; i < 90 && !static_cast<LiveSourceBase*>(obs.get())->hasPicture(); ++i) QTest::qWait(33);
        QVERIFY2(static_cast<LiveSourceBase*>(obs.get())->hasPicture(), "no frame from libobs within 3 s");
        const double openSec = ProcStats::now().wall - beforeOpen.wall;
        const Phase on = pullFor(obs.get(), 120);
        const QJsonObject st = obs->status().value("obs").toObject();
        obs->close();
        const Phase after = pullFor(base.get(), 120);
        base->close();
        qInfo() << QString("[measure] open_to_first_frame_s=%1").arg(openSec, 0, 'f', 2);
        qInfo() << QString("[measure] cpu_pct off=%1 on=%2 after_close=%3 (one core = 100)").arg(off.cpuPct, 0, 'f', 1).arg(on.cpuPct, 0, 'f', 1).arg(after.cpuPct, 0, 'f', 1);
        qInfo() << QString("[measure] working_set_mb off=%1 on=%2 after_close=%3").arg(off.wsMb, 0, 'f', 1).arg(on.wsMb, 0, 'f', 1).arg(after.wsMb, 0, 'f', 1);
        qInfo() << QString("[measure] private_mb off=%1 on=%2 after_close=%3").arg(off.privMb, 0, 'f', 1).arg(on.privMb, 0, 'f', 1).arg(after.privMb, 0, 'f', 1);
        qInfo() << QString("[measure] on_frames non_null=%1 distinct=%2 callbacks=%3 obs_fps=%4 obs_frame_ms=%5 lagged=%6")
                       .arg(on.nonNull).arg(on.distinct).arg(st.value("callbacks").toDouble()).arg(st.value("obsFps").toDouble(), 0, 'f', 2)
                       .arg(st.value("obsFrameTimeMs").toDouble(), 0, 'f', 2).arg(st.value("obsLaggedFrames").toDouble());
        QVERIFY(on.nonNull >= 110);
        QVERIFY(on.wsMb - off.wsMb < 1500.0);
    }

    // (a) 120 frames at 29.97: frames arrive, picture is the scene's colour, age statistics
    void framesAndAge() {
        const OutputFormat fmt = fmt1080();
        std::unique_ptr<LayerSource> src = LayerSource::create(spec({scene("A", {colorItem("red", "#ff0000")})}, "A"));
        QVERIFY(src);
        QVERIFY2(src->open(fmt), qPrintable(src->lastError()));
        auto* obs = dynamic_cast<LibObsSource*>(src.get());
        QVERIFY(obs);
        for (int i = 0; i < 90 && !obs->hasPicture(); ++i) QTest::qWait(33);
        QVERIFY(obs->hasPicture());
        Ticker tk;
        std::vector<double> ageMs, copyMs;
        int nonNull = 0, distinct = 0, repeats = 0; const void* last = nullptr; FramePtr sample;
        for (int i = 0; i < 120; ++i) {
            tk.wait();
            FramePtr f = src->video(i, i / 29.97);
            if (!f) continue;
            nonNull++;
            if (f.get() != last) distinct++; else repeats++;
            last = f.get(); sample = f;
            const LibObsSource::PullTiming t = obs->lastPull();
            if (t.ageNs >= 0) { ageMs.push_back(double(t.ageNs) / 1e6); copyMs.push_back(double(t.sinceCopyNs) / 1e6); }
        }
        QVERIFY(sample);
        QCOMPARE(sample->width, 1920); QCOMPARE(sample->height, 1080);
        QVERIFY2(isRed(px(sample, 960, 540)), qPrintable(QString("centre pixel %1").arg(px(sample, 960, 540), 8, 16)));
        QVERIFY2(isRed(px(sample, 5, 5)) && isRed(px(sample, 1914, 1074)), "corners not red: colour source does not fill the raster");
        const QJsonObject st = src->status().value("obs").toObject();
        qInfo() << QString("[measure] pulled=%1 non_null=%2 distinct=%3 repeats=%4 dropped=%5")
                       .arg(120).arg(nonNull).arg(distinct).arg(repeats).arg(src->status().value("videoDropped").toDouble());
        qInfo() << QString("[measure] frame_age_ms p50=%1 p99=%2 max=%3 min=%4 (frame = 33.37 ms)")
                       .arg(pct(ageMs, 0.5), 0, 'f', 2).arg(pct(ageMs, 0.99), 0, 'f', 2).arg(*std::max_element(ageMs.begin(), ageMs.end()), 0, 'f', 2).arg(*std::min_element(ageMs.begin(), ageMs.end()), 0, 'f', 2);
        qInfo() << QString("[measure] since_copy_ms p50=%1 p99=%2").arg(pct(copyMs, 0.5), 0, 'f', 2).arg(pct(copyMs, 0.99), 0, 'f', 2);
        qInfo() << QString("[measure] status_age_ms p50=%1 p99=%2 max=%3 samples=%4").arg(st.value("ageP50Ms").toDouble(), 0, 'f', 2).arg(st.value("ageP99Ms").toDouble(), 0, 'f', 2).arg(st.value("ageMaxMs").toDouble(), 0, 'f', 2).arg(st.value("ageSamples").toDouble());
        QVERIFY(nonNull >= 115);
        QVERIFY2(distinct >= 100, qPrintable(QString("only %1 distinct frames of %2").arg(distinct).arg(nonNull)));
        QVERIFY(pct(ageMs, 0.99) < 200.0);
        src->close();
    }

    // (b) switch -> first pulled frame showing the new scene (cut, then a 300 ms fade)
    void cutLatency() {
        const OutputFormat fmt = fmt1080();
        QJsonObject cut; cut["id"] = "cut_transition";
        std::unique_ptr<LayerSource> src = LayerSource::create(spec({scene("A", {colorItem("red", "#ff0000")}), scene("B", {colorItem("blue", "#0000ff")})}, "A", cut));
        QVERIFY(src);
        QVERIFY2(src->open(fmt), qPrintable(src->lastError()));
        Ticker tk;
        auto pullUntil = [&](bool (*pred)(QRgb), int maxTicks, int* ticks) -> bool {
            for (int i = 1; i <= maxTicks; ++i) { tk.wait(); FramePtr f = src->video(i, 0); if (f && pred(px(f, 960, 540))) { *ticks = i; return true; } }
            return false;
        };
        int t = 0;
        QVERIFY2(pullUntil(isRed, 120, &t), "scene A never showed");
        std::vector<int> cutTicks; std::vector<double> cutMs;
        for (int trial = 0; trial < 6; ++trial) {
            const bool toBlue = (trial % 2) == 0;
            tk.wait(); src->video(0, 0);                          // one settled tick, then switch right after a pull
            const qint64 t0 = LibObsSource::obsNowNs();
            QJsonObject sw; sw["switch"] = toBlue ? "B" : "A";
            src->update(sw);
            int ticks = 0;
            QVERIFY2(pullUntil(toBlue ? isBlue : isRed, 60, &ticks), "switch never reached the output");
            const LibObsSource::PullTiming pt = dynamic_cast<LibObsSource*>(src.get())->lastPull();
            cutTicks.push_back(ticks); cutMs.push_back(double(pt.pullNs - t0) / 1e6);
            for (int i = 0; i < 15; ++i) { tk.wait(); src->video(0, 0); }   // settle
        }
        QString rows; for (size_t i = 0; i < cutTicks.size(); ++i) rows += QString("%1(%2ms) ").arg(cutTicks[i]).arg(cutMs[i], 0, 'f', 1);
        std::vector<int> sorted = cutTicks; std::sort(sorted.begin(), sorted.end());
        qInfo() << QString("[measure] cut_latency_frames trials=%1 median=%2 min=%3 max=%4").arg(rows.trimmed()).arg(sorted[sorted.size() / 2]).arg(sorted.front()).arg(sorted.back());
        const QJsonObject st = src->status().value("obs").toObject();
        qInfo() << QString("[measure] switch_dispatch_ms=%1 (update() to obs_transition_start)").arg(st.value("switchDispatchMs").toDouble(), 0, 'f', 3);
        QVERIFY2(sorted.back() <= 8, "a cut took more than 8 frames to reach the pulled picture");

        // fade: first changed frame and first fully-blue frame
        tk.wait(); src->video(0, 0);
        QRgb before = 0; { FramePtr f = src->video(0, 0); QVERIFY(f); before = px(f, 960, 540); QVERIFY(isRed(before)); }
        QJsonObject sw; sw["switch"] = "B"; QJsonObject tr; tr["id"] = "fade_transition"; tr["durationMs"] = 300; sw["transition"] = tr;
        src->update(sw);
        int firstChange = 0, fullBlue = 0;
        for (int i = 1; i <= 90; ++i) {
            tk.wait(); FramePtr f = src->video(i, 0); if (!f) continue;
            const QRgb c = px(f, 960, 540);
            if (!firstChange && c != before) firstChange = i;
            if (isBlue(c)) { fullBlue = i; break; }
        }
        qInfo() << QString("[measure] fade_300ms first_change_frame=%1 full_frame=%2 (expected ~9 frames of fade)").arg(firstChange).arg(fullBlue);
        QVERIFY(firstChange > 0 && fullBlue > firstChange);
        src->close();
    }

    // audio from a libobs scene (test-input sine) reaches audio()
    void audioArrives() {
        const OutputFormat fmt = fmt1080();
        QJsonObject sine; sine["id"] = "test_sinewave"; sine["name"] = "tone";
        std::unique_ptr<LayerSource> src = LayerSource::create(spec({scene("A", {colorItem("bg", "#202020"), sine})}, "A"));
        QVERIFY(src);
        if (!src->open(fmt)) QSKIP(qPrintable("open failed: " + src->lastError() + " (test-input module missing?)"));
        Ticker tk;
        bool heard = false; double peak = 0;
        for (int i = 0; i < 90 && !heard; ++i) {
            tk.wait(); src->video(i, 0);
            FramePtr a = allocAudioFrame(1601, fmt.channels, fmt.sampleRate);
            if (src->audio(a, i, 0)) {
                const float* L = reinterpret_cast<const float*>(a->data[0]);
                for (int n = 0; n < a->nb_samples; ++n) peak = qMax(peak, double(qAbs(L[n])));
                if (peak > 0.01) heard = true;
            }
        }
        const QJsonObject st = src->status();
        qInfo() << QString("[measure] audio heard=%1 peak=%2 audioCallbacks=%3 underrun=%4 overflow=%5").arg(heard).arg(peak, 0, 'f', 3)
                       .arg(st.value("obs").toObject().value("audioCallbacks").toDouble()).arg(st.value("audioUnderrun").toDouble()).arg(st.value("audioOverflow").toDouble());
        QVERIFY(heard);
        src->close();
    }

    void secondSourceRefused() {
        const OutputFormat fmt = fmt1080();
        std::unique_ptr<LayerSource> a = LayerSource::create(spec({scene("A", {colorItem("red", "#ff0000")})}, "A"));
        std::unique_ptr<LayerSource> b = LayerSource::create(spec({scene("A", {colorItem("red", "#ff0000")})}, "A"));
        QVERIFY(a->open(fmt));
        QVERIFY(!b->open(fmt));
        QVERIFY(b->lastError().contains("one source per process"));
        a->close();
        QVERIFY(b->open(fmt));
        b->close();
    }
    void liveChromaKey() {
        OutputFormat f; f.width = 320; f.height = 180;
        auto source = LayerSource::create(spec({scene("A", {colorItem("background", "#ff0000"), colorItem("subject", "#00ff00")}), scene("B", {colorItem("subject", "#00ff00")})}, "A"));
        QVERIFY(source); QVERIFY2(source->open(f), qPrintable(source->lastError()));
        qint64 frame = 0;
        auto waitColor = [&](bool red) { for (int i = 0; i < 90; ++i) { QTest::qWait(34); auto v = source->video(frame++, 0); if (v) { const auto c = px(v, 160, 90); if (red ? isRed(c) : qGreen(c) > 220 && qRed(c) < 40 && qBlue(c) < 40) return true; } } return false; };
        QVERIFY(waitColor(false));
        auto key = [&](const QString& color) { source->update({{"item", "subject"}, {"scene", "A"}, {"filter", QJsonObject{{"id", "chroma_key_filter_v2"}, {"settings", QJsonObject{{"key_color_type", "custom"}, {"key_color", color}, {"similarity", 400}, {"smoothness", 80}, {"spill", 100}, {"opacity", 1.0}}}}}}); };
        key("#00ff00"); QVERIFY(waitColor(true));
        source->update({{"switch", "B"}}); QVERIFY(waitColor(false)); // same item name in another scene is unaffected
        source->update({{"switch", "A"}}); QVERIFY(waitColor(true));
        key("#0000ff"); QVERIFY(waitColor(false));
        QCOMPARE(source->status().value("obs").toObject().value("filterUpdates").toInt(), 2);
        source->close();
    }
};

QTEST_GUILESS_MAIN(TestLibObsSource)
#include "test_libobssource.moc"
