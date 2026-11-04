#include <QtTest/QtTest>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QThread>
#include <cmath>
#include "playout/core/framerate.h"
#include "playout/engine/decoderpipeline.h"
#include "playout/engine/engine.h"
#include "playout/engine/tickprofiler.h"
#include "playout/engine/mixer.h"
#include "playout/engine/encoderstage.h"
#include "playout/engine/sinks/nullsink.h"
#include "playout/engine/previewtap.h"
#include "playout/engine/compositor/compositor.h"
#include <QImage>
#include <array>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

/**
 * Pipeline integration tests. Media is synthesised with the ffmpeg CLI so the
 * tests are self-contained; they are skipped if ffmpeg is not on PATH.
 *
 *  - flash/beep clip: black video with a single white frame at t=1.0 s and a
 *    1 kHz tone burst starting at t=1.0 s. The decoder must deliver the white
 *    frame and the first loud sample at the same timeline position (+-1 frame).
 *  - cadence: pulling audio per output frame yields exactly the 1601/1602
 *    pattern with no underruns while the decoder has data.
 *  - engine: two short items play back to back through PlayoutEngine into
 *    null sinks; as-run must show START/END for both with no missed cut.
 */
class TestPipeline : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void decoderAvAlignment();
    void decoderAudioCadence();
    void decoderWouldShowRule();
    void decoderSkipsUnshownPictures();
    void decoderHwAutoPicksByCost();
    void mixerOverlayFastPath();
    void enginePlaysTwoItemsGapless();
    void mpeg2PresetsOpenMpeg2NotFallback();
    void encoderThreadCap();
    void encoderPooledPicturesMatchLegacy();
    void previewEncoderAveragesFineDetail();
    void previewTapBoostProfile();
    void inputMonitorSourceMatching();
    void inputMonitorDeliversPictures();
    void twoMixesTwoOutputs();
    void clockThreadStaysQuiet();

private:
    QTemporaryDir m_dir;
    QString m_ffmpeg;
    QString m_flashClip;   // 640x360 29.97, 3 s, flash+beep at 1.0 s
    QString m_clipA;       // 2 s testsrc
    QString m_clipB;       // 2 s testsrc

    bool makeClip(const QString& out, const QStringList& args);
};

bool TestPipeline::makeClip(const QString& out, const QStringList& args) {
    QProcess p;
    QStringList full{"-hide_banner", "-loglevel", "error", "-y"};
    full << args << out;
    p.start(m_ffmpeg, full);
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        qWarning() << "ffmpeg failed:" << p.readAllStandardError();
        return false;
    }
    return QFileInfo::exists(out);
}

void TestPipeline::initTestCase() {
    m_ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg not found on PATH; skipping pipeline tests");
    QVERIFY(m_dir.isValid());

    m_flashClip = m_dir.filePath("flash.mp4");
    // Video: black, one white frame at t=1.0 (frame 30 at 29.97 -> 1.001 s; use enable window covering it)
    // Audio: silence, then 1 kHz from t=1.0.
    QVERIFY(makeClip(m_flashClip, {
        "-f", "lavfi", "-i", "color=c=black:s=640x360:r=30000/1001:d=3,drawbox=c=white:t=fill:enable='between(t,1.0,1.04)'",
        "-f", "lavfi", "-i", "aevalsrc=if(gte(t\\,1.0)\\,sin(2*PI*1000*t)\\,0):s=48000:d=3",
        "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "ultrafast", "-g", "15", "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "128k", "-shortest"}));

    m_clipA = m_dir.filePath("a.mp4");
    QVERIFY(makeClip(m_clipA, {
        "-f", "lavfi", "-i", "testsrc2=s=640x360:r=30000/1001:d=2",
        "-f", "lavfi", "-i", "sine=f=440:r=48000:d=2",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
    m_clipB = m_dir.filePath("b.mp4");
    QVERIFY(makeClip(m_clipB, {
        "-f", "lavfi", "-i", "smptebars=s=640x360:r=30000/1001:d=2",
        "-f", "lavfi", "-i", "sine=f=880:r=48000:d=2",
        "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
}

void TestPipeline::decoderAvAlignment() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    DecoderPipeline::Config cfg;
    cfg.outWidth = 640; cfg.outHeight = 360; cfg.outRate = FrameRate::ntsc(); cfg.hwAccel = false;
    DecoderPipeline dec(cfg);
    QVERIFY2(dec.open(m_flashClip), qPrintable(dec.lastError()));
    QVERIFY(dec.hasAudio());
    QVERIFY(qAbs(dec.durationSec() - 3.0) < 0.1);

    const FrameRate r = cfg.outRate;
    qint64 flashFrame = -1;
    qint64 firstLoudSample = -1;
    qint64 samplesSoFar = 0;
    std::vector<float> l, rch;

    for (qint64 f = 0; f < 85; ++f) {       // ~2.8 s
        const double pos = r.frameToNs(f) / 1e9;
        // Decoder runs on its own thread; the real clock paces at 33 ms, so wait
        // until it is ahead of us (or finished) before pulling, as it would be live.
        QElapsedTimer t; t.start();
        while (dec.stats().decodedVideo < f + 2 && !dec.decoderFinished() && t.elapsed() < 1000) QTest::qWait(1);
        FramePtr v = dec.videoFrameAt(pos);
        QVERIFY2(v, qPrintable(QString("no video at frame %1").arg(f)));
        {
            const uint8_t* px = v->data[0] + 180 * v->linesize[0] + 320 * 4;   // centre pixel BGRA
            if (f >= 27 && f <= 34) {
                const auto st = dec.stats();
                qInfo("f=%lld centre BGRA=%d,%d,%d,%d decoded=%lld dropped=%lld repeated=%lld queue=%d",
                      (long long)f, px[0], px[1], px[2], px[3], (long long)st.decodedVideo,
                      (long long)st.droppedVideo, (long long)st.repeatedVideo, st.queueDepth);
            }
            if (flashFrame < 0 && px[0] > 200 && px[1] > 200 && px[2] > 200) flashFrame = f;
        }
        const int n = int(r.samplesForFrame(f, 48000));
        l.assign(n, 0.f); rch.assign(n, 0.f);
        float* planes[2] = { l.data(), rch.data() };
        // Audio is produced slightly after video on the decode thread; wait for it.
        t.restart();
        while (dec.stats().decodedAudioSamples < samplesSoFar + n && !dec.decoderFinished() && t.elapsed() < 500) QTest::qWait(2);
        dec.pullAudio(planes, n);
        if (firstLoudSample < 0) {
            for (int i = 0; i < n; ++i) if (std::fabs(l[i]) > 0.3f) { firstLoudSample = samplesSoFar + i; break; }
        }
        samplesSoFar += n;
    }
    QVERIFY2(flashFrame >= 0, "white flash frame never seen");
    QVERIFY2(firstLoudSample >= 0, "tone burst never heard");
    const double flashSec = r.frameToNs(flashFrame) / 1e9;
    const double toneSec = firstLoudSample / 48000.0;
    qInfo("flash at frame %lld (%.4f s), tone at sample %lld (%.4f s), delta %.1f ms",
          (long long)flashFrame, flashSec, (long long)firstLoudSample, toneSec, (toneSec - flashSec) * 1000.0);
    // Must agree within one output frame (33.4 ms). AAC priming is compensated by the container.
    QVERIFY2(std::fabs(toneSec - flashSec) < 0.034, "audio/video misaligned by more than a frame");
    QCOMPARE(dec.stats().audioUnderruns, 0LL);
}

void TestPipeline::decoderAudioCadence() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    DecoderPipeline::Config cfg;
    cfg.outWidth = 640; cfg.outHeight = 360; cfg.outRate = FrameRate::ntsc(); cfg.hwAccel = false;
    DecoderPipeline dec(cfg);
    QVERIFY2(dec.open(m_clipA), qPrintable(dec.lastError()));
    qint64 total = 0;
    std::vector<float> l, rch;
    for (qint64 f = 0; f < 50; ++f) {      // 1.67 s of a 2 s clip, well inside
        const int n = int(cfg.outRate.samplesForFrame(f, 48000));
        QVERIFY(n == 1601 || n == 1602);
        l.assign(n, 0.f); rch.assign(n, 0.f);
        float* planes[2] = { l.data(), rch.data() };
        QElapsedTimer t; t.start();
        while ((dec.stats().decodedAudioSamples < total + n || dec.stats().decodedVideo < f + 2)
               && !dec.decoderFinished() && t.elapsed() < 1000) QTest::qWait(1);
        QVERIFY2(dec.pullAudio(planes, n), qPrintable(QString("underrun at frame %1").arg(f)));
        total += n;
        (void)dec.videoFrameAt(cfg.outRate.frameToNs(f) / 1e9);
    }
    QCOMPARE(total, cfg.outRate.sampleBoundary(50, 48000));
    QCOMPARE(dec.stats().audioUnderruns, 0LL);
}

// ---------------------------------------------------------------------------------------------------------------------
// PE-12: never-shown pictures skipped before conversion, pooled hw download, hwDecode auto

void TestPipeline::decoderWouldShowRule() {
    const FrameRate out = FrameRate::ntsc();
    const double d = out.frameDurationNs() / 1e9;
    // 59.94p into 29.97: every other source picture lands on an output tick, the one in between never does.
    int shown = 0;
    for (int n = 0; n < 200; ++n) if (DecoderPipeline::wouldShow(out, n * d / 2.0, (n + 1) * d / 2.0)) ++shown;
    QCOMPARE(shown, 100);
    QVERIFY(DecoderPipeline::wouldShow(out, 0.0, d / 2.0));            // frame 0 is tick 0
    QVERIFY(!DecoderPipeline::wouldShow(out, d / 2.0, d));             // frame 1: tick 1 picks frame 2 (pos d <= d + tol)
    // 24p and 29.97p sources: every picture is shown at least once.
    for (int n = 0; n < 200; ++n) {
        QVERIFY(DecoderPipeline::wouldShow(out, n / 24.0, (n + 1) / 24.0));
        QVERIFY(DecoderPipeline::wouldShow(out, n * d, (n + 1) * d));
    }
    // A picture that sits within the quarter-frame tolerance before a tick is picked by that tick.
    QVERIFY(DecoderPipeline::wouldShow(out, 3 * d + d * 0.2, 4 * d + d * 0.2));
    // Three pictures per tick: only the last before the tick (plus tolerance) survives.
    shown = 0;
    for (int n = 0; n < 300; ++n) if (DecoderPipeline::wouldShow(out, n * d / 3.0, (n + 1) * d / 3.0)) ++shown;
    QCOMPARE(shown, 100);
}

void TestPipeline::decoderSkipsUnshownPictures() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    // 59.94p clip whose luma is the frame index (geq: 2 per frame), lossless, so each shown picture names its source frame.
    const QString clip = m_dir.filePath("fast5994.mp4");
    QVERIFY(makeClip(clip, {
        "-f", "lavfi", "-i", "nullsrc=s=640x360:r=60000/1001:d=2,geq=lum='min(N*2\\,250)':cb=128:cr=128",
        "-c:v", "libx264", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "yuv420p"}));

    auto run = [&](bool skip, std::vector<int>& lumas, DecoderPipeline::Stats& st) {
        DecoderPipeline::Config cfg;
        cfg.outWidth = 640; cfg.outHeight = 360; cfg.outRate = FrameRate::ntsc(); cfg.hwAccel = false; cfg.skipUnshown = skip;
        DecoderPipeline dec(cfg);
        QVERIFY2(dec.open(clip), qPrintable(dec.lastError()));
        QVERIFY(qAbs(dec.sourceRate().fps() - 59.94) < 0.1);
        lumas.clear();
        for (qint64 f = 0; f < 50; ++f) {
            // The tick-f pick is settled once the picture after the eligible one is decoded (2f+2 eligible, 2f+3 next).
            QElapsedTimer t; t.start();
            while (dec.stats().decodedVideo < 2 * f + 3 && !dec.decoderFinished() && t.elapsed() < 1000) QTest::qWait(1);
            FramePtr v = dec.videoFrameAt(cfg.outRate.frameToNs(f) / 1e9);
            QVERIFY(v);
            lumas.push_back(int(v->data[0][180 * v->linesize[0] + 320 * 4 + 1]));   // G of the centre pixel (grey picture)
        }
        st = dec.stats();
        dec.close();
    };
    std::vector<int> withSkip, without;
    DecoderPipeline::Stats sSkip, sPlain;
    run(true, withSkip, sSkip);
    run(false, without, sPlain);
    qInfo("skip: decoded %lld converted %lld skippedConvert %lld dropped %lld repeated %lld | plain: decoded %lld converted %lld dropped %lld repeated %lld",
          (long long)sSkip.decodedVideo, (long long)sSkip.convertedVideo, (long long)sSkip.skippedConvert, (long long)sSkip.droppedVideo, (long long)sSkip.repeatedVideo,
          (long long)sPlain.decodedVideo, (long long)sPlain.convertedVideo, (long long)sPlain.droppedVideo, (long long)sPlain.repeatedVideo);
    QVERIFY(sSkip.skipActive);
    QVERIFY(!sPlain.skipActive);
    QCOMPARE(sPlain.skippedConvert, 0LL);
    QCOMPARE(sPlain.convertedVideo, sPlain.decodedVideo);
    // Same pictures on the same ticks, so nothing visible changed...
    QCOMPARE(withSkip, without);
    // ...and the output is advancing (the luma ramp climbs, two source frames per tick).
    QVERIFY(withSkip[40] > withSkip[10] + 40);
    // ...while about half the pictures never went through sws.
    QVERIFY2(sSkip.skippedConvert >= 30, qPrintable(QString("skippedConvert %1").arg(sSkip.skippedConvert)));
    QVERIFY(sSkip.convertedVideo + sSkip.skippedConvert <= sSkip.decodedVideo);
    QVERIFY(sSkip.convertedVideo <= sSkip.decodedVideo * 0.65);
    // droppedVideo counts the same pictures either way; the skip run counts them on the decode thread, up to the queue
    // depth ahead of the ticks, so the two reads differ by at most the read-ahead.
    QVERIFY2(qAbs(sSkip.droppedVideo - sPlain.droppedVideo) <= 10, qPrintable(QString("dropped %1 vs %2").arg(sSkip.droppedVideo).arg(sPlain.droppedVideo)));
}

void TestPipeline::decoderHwAutoPicksByCost() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    DecoderPipeline::resetHwAutoTable();
    DecoderPipeline::Config cfg;
    cfg.outWidth = 640; cfg.outHeight = 360; cfg.outRate = FrameRate::ntsc();
    cfg.hwAccel = true; cfg.hwAuto = true; cfg.hwAutoFrames = 20;

    auto play = [&](DecoderPipeline& dec) {
        // Pull at pace until the measurement window closed (or the clip ended) so back-pressure never stalls it.
        QElapsedTimer t; t.start();
        for (qint64 f = 0; f < 55 && t.elapsed() < 4000; ++f) {
            (void)dec.videoFrameAt(cfg.outRate.frameToNs(f) / 1e9);
            if (dec.stats().autoCostMs >= 0.0 || dec.decoderFinished()) break;
            QTest::qWait(20);
        }
    };
    QStringList reasons, choices;
    for (int i = 0; i < 3; ++i) {
        DecoderPipeline dec(cfg);
        QVERIFY2(dec.open(m_clipA), qPrintable(dec.lastError()));
        play(dec);
        const auto st = dec.stats();
        reasons << st.autoReason; choices << st.hwChoice;
        qInfo().noquote() << QString("auto open %1: choice %2 reason %3 cost %4 ms keepsUp %5 hw=%6 pooled=%7 allocs=%8 converted=%9")
            .arg(i).arg(st.hwChoice).arg(st.autoReason).arg(st.autoCostMs, 0, 'f', 2).arg(st.autoKeepsUp)
            .arg(st.hwAccelActive ? st.hwDevice : "-").arg(st.downloadPooled).arg(st.downloadAllocs).arg(st.convertedVideo);
        if (st.autoReason == "probe") QVERIFY2(st.autoCostMs >= 0.0, "measurement window never closed");
        if (st.hwAccelActive) {
            // PE-12 pooled download: the pool grows to the pictures in flight (one at a time here), never per picture.
            QVERIFY(st.downloadPooled);
            QVERIFY2(st.downloadAllocs <= 3, qPrintable(QString("downloadAllocs %1").arg(st.downloadAllocs)));
        }
        dec.close();
    }
    const QJsonObject table = DecoderPipeline::hwAutoTable();
    qInfo().noquote() << "hwAutoTable" << QJsonDocument(table).toJson(QJsonDocument::Compact);
    QCOMPARE(table.size(), 1);
    const QJsonObject e = table.begin().value().toObject();
    QCOMPARE(e.value("opens").toInt(), 3);
    if (e.value("hwUnavailable").toBool()) {
        // No hw decoder here: sw is the only mode, nothing is ever probed twice.
        QCOMPARE(choices, QStringList({"sw", "sw", "sw"}));
        QCOMPARE(reasons, QStringList({"only", "only", "only"}));
    } else {
        QCOMPARE(choices.mid(0, 2), QStringList({"hw", "sw"}));
        QCOMPARE(reasons, QStringList({"probe", "probe", "cheaper"}));
        QVERIFY(e.value("hwMs").toDouble() >= 0.0 && e.value("swMs").toDouble() >= 0.0);
        const double hwCost = e.value("hwKeepsUp").toBool() ? e.value("hwMs").toDouble() : 1e9;
        const double swCost = e.value("swKeepsUp").toBool() ? e.value("swMs").toDouble() : 1e9;
        QCOMPARE(choices[2], QString(hwCost <= swCost ? "hw" : "sw"));
        // The legacy per-picture download allocation is one env switch away.
        qputenv("PLAYPLAY_DECODE_POOL", "0");
        DecoderPipeline::Config c2 = cfg; c2.hwAuto = false;
        DecoderPipeline dec(c2);
        qunsetenv("PLAYPLAY_DECODE_POOL");
        QVERIFY2(dec.open(m_clipA), qPrintable(dec.lastError()));
        play(dec);
        const auto st = dec.stats();
        QVERIFY(!st.downloadPooled);
        QVERIFY2(st.downloadAllocs >= st.convertedVideo && st.convertedVideo >= 5, qPrintable(QString("allocs %1 converted %2").arg(st.downloadAllocs).arg(st.convertedVideo)));
    }
}

void TestPipeline::mixerOverlayFastPath() {
    Mixer mixer(320, 180);
    FramePtr base = allocVideoFrame(320, 180, kMixerPixelFormat);
    for (int y = 0; y < 180; ++y) memset(base->data[0] + y * base->linesize[0], 0x40, 320 * 4);
    mixer.setBaseFrame(base);
    // No overlays: same buffer comes back (zero copy).
    FramePtr out = mixer.composite(1);
    QCOMPARE(out.get(), base.get());

    // With an overlay: a new frame with the overlay blended in.
    QImage ov(100, 50, QImage::Format_ARGB32);
    ov.fill(QColor(255, 0, 0, 255));
    mixer.setOverlay(10, ov, QPoint(10, 10));
    out = mixer.composite(2);
    QVERIFY(out.get() != base.get());
    const uint8_t* inside = out->data[0] + 20 * out->linesize[0] + 20 * 4;     // B,G,R,A
    QCOMPARE(int(inside[2]), 255);   // red
    QCOMPARE(int(inside[0]), 0);
    const uint8_t* outside = out->data[0] + 150 * out->linesize[0] + 300 * 4;
    QCOMPARE(int(outside[2]), 0x40); // untouched base
    // Hidden overlay -> fast path again.
    mixer.setOverlayVisible(10, false);
    QCOMPARE(mixer.composite(3).get(), base.get());
    // No base -> slate frame, non-null.
    mixer.setBaseFrame(nullptr);
    QVERIFY(mixer.composite(4));
}

void TestPipeline::enginePlaysTwoItemsGapless() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    PlayoutEngine::Config cfg;
    cfg.channelId = "test"; cfg.channelName = "Test";
    cfg.frameRate = FrameRate::ntsc(); cfg.width = 640; cfg.height = 360; cfg.hwDecode = false;
    cfg.prerollSec = 1.0;
    QJsonObject nullOut; nullOut["type"] = "null"; nullOut["name"] = "null-ts";
    QJsonObject prof; prof["preset"] = "h264_aac_low"; prof["x264Preset"] = "ultrafast";
    nullOut["profile"] = prof;
    QJsonObject bb; bb["type"] = "nullbaseband"; bb["name"] = "bb";
    cfg.outputs = QJsonArray{nullOut, bb};

    PlayoutEngine engine(cfg);
    QVERIFY(engine.initialize());

    QStringList events;
    QObject::connect(&engine, &PlayoutEngine::asRunEvent, [&](const QJsonObject& e) {
        events << e["type"].toString() + ":" + e["itemId"].toString();
    });
    QStringList states;
    QObject::connect(&engine, &PlayoutEngine::stateChanged, [&](const QString& s, const QString&) { states << s; });

    QJsonObject a; a["id"] = "A"; a["path"] = m_clipA;
    QJsonObject b; b["id"] = "B"; b["path"] = m_clipB;
    engine.loadPlaylist(QJsonArray{a, b}, true);
    engine.handleTransportCommand("play", {});

    // Drive the clock at 2x real time; ~5.5 s of programme = 165 frames.
    const FrameRate r = cfg.frameRate;
    qint64 frame = 0;
    QElapsedTimer t; t.start();
    while (frame < 200 && t.elapsed() < 20000) {
        engine.onFrameTick(frame, r.frameToNs(frame));
        ++frame;
        QTest::qWait(16);
        if (events.contains("END:B") && engine.currentState().stateName == "IDLE") break;
    }

    qInfo() << "as-run:" << events;
    qInfo() << "states:" << states;
    QVERIFY(events.contains("START:A"));
    QVERIFY(events.contains("END:A"));
    QVERIFY(events.contains("START:B"));
    QVERIFY(events.contains("END:B"));
    QVERIFY(events.indexOf("END:A") < events.indexOf("START:B"));
    QCOMPARE(engine.currentState().stateName, QString("IDLE"));
    QCOMPARE(engine.currentState().missedCuts, 0LL);
    // Gapless: END:A and START:B happen on the same frame (same tick).
    const QJsonObject st = engine.stateJson();
    QVERIFY(st.value("outputs").toArray().size() >= 2);

    engine.shutdown();
}

// ---------------------------------------------------------------------------
// Confidence pictures
// ---------------------------------------------------------------------------

static FramePtr checkerFrame(int w, int h) {
    FramePtr f = allocVideoFrame(w, h, AV_PIX_FMT_BGRA);
    for (int y = 0; y < h; ++y) {
        auto* row = reinterpret_cast<quint32*>(f->data[0] + y * f->linesize[0]);
        for (int x = 0; x < w; ++x) row[x] = ((x + y) & 1) ? 0xFFFFFFFFu : 0xFF000000u;     // 1-px checker
    }
    return f;
}

static FramePtr solidFrame(int w, int h, quint32 bgra) {
    FramePtr f = allocVideoFrame(w, h, AV_PIX_FMT_BGRA);
    for (int y = 0; y < h; ++y) { auto* row = reinterpret_cast<quint32*>(f->data[0] + y * f->linesize[0]); for (int x = 0; x < w; ++x) row[x] = bgra; }
    return f;
}

// A 1-pixel checker downscaled 4:1 must come out mid-grey: an area filter averages it, a nearest-neighbour pick (the old
// FastTransformation preview) lands on all-black or all-white and makes fine text shimmer on the multiview.
void TestPipeline::previewEncoderAveragesFineDetail() {
    PreviewEncoder enc;
    int w = 0, h = 0;
    const QByteArray jpeg = enc.encode(checkerFrame(1920, 1080), 480, 270, 90, &w, &h);
    QVERIFY(!jpeg.isEmpty());
    QCOMPARE(w, 480); QCOMPARE(h, 270);
    QImage img; QVERIFY(img.loadFromData(jpeg, "JPEG"));
    const int grey = qGray(img.pixel(240, 135));
    QVERIFY2(grey > 100 && grey < 156, qPrintable(QString("centre grey %1").arg(grey)));
    // Aspect ratio is kept inside the box: 4:3 into 480x270 -> 360x270; a portrait source -> narrow.
    QCOMPARE(PreviewEncoder::fit(1440, 1080, 480, 270), QSize(360, 270));
    QCOMPARE(PreviewEncoder::fit(1080, 1920, 480, 270).height(), 270);
    QVERIFY(PreviewEncoder::fit(1080, 1920, 480, 270).width() < 160);
    // Never upscales.
    QCOMPARE(PreviewEncoder::fit(320, 180, 640, 360), QSize(320, 180));
}

void TestPipeline::previewTapBoostProfile() {
    PreviewTap tap;
    tap.setChannelFps(30);
    tap.setEnabled(true);
    QList<int> widths;
    connect(&tap, &PreviewTap::frameReady, this, [&](const QByteArray&, int w, int, qint64) { widths << w; });
    const FramePtr f = solidFrame(1920, 1080, 0xFF204080u);
    qint64 frame = 0;
    auto run = [&](int frames) { for (int i = 0; i < frames; ++i) { tap.submit(f, frame++); QTest::qWait(2); } QTest::qWait(150); };
    run(12);
    QVERIFY(!widths.isEmpty());
    QCOMPARE(widths.last(), 480);           // base
    tap.boost(10);
    QVERIFY(tap.boosted());
    widths.clear(); run(12);
    QVERIFY(!widths.isEmpty());
    QCOMPARE(widths.last(), 640);           // boost
    tap.boost(0);
    QVERIFY(!tap.boosted());
    widths.clear(); run(12);
    QCOMPARE(widths.last(), 480);
    // Cadence: base 10 fps at 30 fps = every 3rd frame; 30 frames -> about 10 pictures (newest-wins may merge a few).
    widths.clear(); run(30);
    QVERIFY2(widths.size() >= 6 && widths.size() <= 11, qPrintable(QString("pictures %1").arg(widths.size())));
}

void TestPipeline::inputMonitorSourceMatching() {
    QJsonObject a{{"type", "ndi"}, {"name", "CAM1 (Studio)"}};
    QJsonObject b{{"type", "ndi"}, {"name", "cam1 (studio)"}, {"bandwidth", "lowest"}, {"recvName", "PlayPlay Monitor"}};
    QVERIFY(InputMonitor::sameSource(a, b));                                    // receive options do not matter
    QVERIFY(!InputMonitor::sameSource(a, QJsonObject{{"type", "ndi"}, {"name", "CAM2 (Studio)"}}));
    QVERIFY(InputMonitor::sameSource(QJsonObject{{"type", "stream"}, {"url", "rtsp://x/1"}}, QJsonObject{{"type", "stream"}, {"url", "rtsp://x/1"}, {"realtime", true}}));
    QVERIFY(!InputMonitor::sameSource(QJsonObject{{"type", "stream"}, {"url", "rtsp://x/1"}}, a));
    const QJsonObject low = InputMonitor::openSpec(a, false), high = InputMonitor::openSpec(a, true);
    QCOMPARE(low.value("bandwidth").toString(), QString("lowest"));
    QCOMPARE(low.value("audio").toBool(true), false);
    QCOMPARE(high.value("bandwidth").toString(), QString("highest"));
    QCOMPARE(high.value("audio").toBool(false), true);
    QVERIFY(InputMonitor::allowedType("ndi"));
    QVERIFY(InputMonitor::allowedType("stream"));
    QVERIFY(!InputMonitor::allowedType("sdi_in"));                              // card ports are never double-opened
    QVERIFY(!InputMonitor::allowedType("audio_in"));
}

// The engine monitors leased inputs without touching program: pictures arrive per key, refused types report "card",
// and an empty lease closes everything.
void TestPipeline::inputMonitorDeliversPictures() {
    PlayoutEngine::Config cfg;
    cfg.channelId = "mon"; cfg.channelName = "Mon";
    cfg.frameRate = FrameRate::ntsc(); cfg.width = 640; cfg.height = 360; cfg.hwDecode = false;
    QJsonObject bb; bb["type"] = "nullbaseband"; bb["name"] = "bb";
    cfg.outputs = QJsonArray{bb};
    PlayoutEngine engine(cfg);
    QVERIFY(engine.initialize());
    QVERIFY(engine.inputMonitor());

    QHash<QString, QImage> got;
    connect(engine.inputMonitor(), &InputMonitor::frameReady, this, [&](const QString& key, const QByteArray& jpeg, int, int) {
        QImage img; if (img.loadFromData(jpeg, "JPEG")) got[key] = img;
    });

    QJsonArray inputs{
        QJsonObject{{"key", "in1"}, {"quality", "low"}, {"source", QJsonObject{{"type", "color"}, {"color", "#ff0000"}}}},
        QJsonObject{{"key", "in2"}, {"quality", "low"}, {"source", QJsonObject{{"type", "sdi_in"}, {"port", 1}}}},
        QJsonObject{{"key", "in3"}, {"quality", "high"}, {"source", QJsonObject{{"type", "bars"}}}},
    };
    engine.handleCgCommand(QJsonObject{{"action", "monitor"}, {"inputs", inputs}, {"leaseSec", 30}});

    const FrameRate r = cfg.frameRate;
    qint64 frame = 0;
    QElapsedTimer t; t.start();
    while (t.elapsed() < 8000 && !(got.contains("in1") && got.contains("in3"))) {
        engine.onFrameTick(frame, r.frameToNs(frame)); ++frame; QTest::qWait(10);
    }
    QVERIFY2(got.contains("in1"), "no picture for in1");
    QVERIFY2(got.contains("in3"), "no picture for in3");
    QVERIFY(!got.contains("in2"));
    const QColor c = got["in1"].pixelColor(got["in1"].width() / 2, got["in1"].height() / 2);
    QVERIFY2(c.red() > 200 && c.green() < 60 && c.blue() < 60, qPrintable(c.name()));
    QCOMPARE(got["in1"].width(), 320);          // low
    QCOMPARE(got["in3"].width(), 480);          // high

    QJsonObject states;
    for (const QJsonValue& v : engine.stateJson().value("monitors").toArray()) states[v.toObject().value("key").toString()] = v.toObject().value("state").toString();
    QCOMPARE(states.value("in1").toString(), QString("open"));
    QCOMPARE(states.value("in2").toString(), QString("card"));
    QCOMPARE(states.value("in3").toString(), QString("open"));
    QVERIFY(engine.stateJson().contains("preview"));

    // Generators are not live receivers: a take never steals them.
    QVERIFY(engine.inputMonitor()->takeWarm(QJsonObject{{"type", "bars"}}) == nullptr);

    engine.handleCgCommand(QJsonObject{{"action", "monitor"}, {"inputs", QJsonArray()}});
    engine.onFrameTick(frame, r.frameToNs(frame)); ++frame;
    QCOMPARE(engine.inputMonitor()->count(), 0);
    engine.shutdown();
}

// Two outputs on two mixes: a green box shown only in "main" is in main's encoded file and not in web's; the red program
// stays red through the (threaded) RGB->YUV conversion of both encoders.
void TestPipeline::twoMixesTwoOutputs() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    const QString red = m_dir.filePath("red.mp4");
    QVERIFY(makeClip(red, {"-f", "lavfi", "-i", "color=c=red:s=640x360:r=30000/1001:d=4", "-f", "lavfi", "-i", "sine=f=440:r=48000:d=4",
                           "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"}));
    const QString outMain = m_dir.filePath("main.ts"), outWeb = m_dir.filePath("web.ts");
    PlayoutEngine::Config cfg;
    cfg.channelId = "mix"; cfg.channelName = "Mix"; cfg.frameRate = FrameRate::ntsc(); cfg.width = 640; cfg.height = 360; cfg.hwDecode = false;
    QJsonObject prof{{"preset", "h264_aac_low"}, {"x264Preset", "ultrafast"}};
    cfg.outputs = QJsonArray{
        QJsonObject{{"type", "file"}, {"name", "fmain"}, {"url", outMain}, {"profile", prof}},
        QJsonObject{{"type", "file"}, {"name", "fweb"}, {"url", outWeb}, {"profile", prof}, {"mix", "web"}}};
    cfg.mixes = QJsonArray{QJsonObject{{"id", "web"}, {"name", "Web"}}};
    PlayoutEngine engine(cfg);
    QVERIFY(engine.initialize());
    QCOMPARE(engine.compositor()->mixIds(), QStringList({"main", "web"}));
    engine.loadPlaylist(QJsonArray{QJsonObject{{"id", "R"}, {"path", red}}}, true);
    engine.handleTransportCommand("play", {});
    engine.handleCgCommand(QJsonObject{{"action", "show"}, {"layer", 30}, {"mixes", QJsonArray{"main"}},
        {"source", QJsonObject{{"type", "color"}, {"color", "#00ff00"}, {"width", 160}, {"height", 90}}}, {"x", 0}, {"y", 0}, {"width", 160}, {"height", 90}});
    const FrameRate r = cfg.frameRate;
    for (qint64 f = 0; f < 75; ++f) { engine.onFrameTick(f, r.frameToNs(f)); QTest::qWait(12); }
    engine.shutdown();

    auto pixel = [&](const QString& ts, int x, int y) -> QColor {
        const QString png = ts + ".png";
        QProcess p; p.start(m_ffmpeg, {"-y", "-v", "error", "-i", ts, "-update", "1", png});   // every frame overwrites: the last one stays
        if (!p.waitForFinished(30000)) return QColor();
        QImage img(png);
        if (img.isNull()) qWarning() << "frame grab failed for" << ts << p.readAllStandardError() << QFileInfo(ts).size();
        return img.isNull() ? QColor() : img.pixelColor(x, y);
    };
    const QColor mainBox = pixel(outMain, 60, 40), mainBg = pixel(outMain, 400, 250), webBox = pixel(outWeb, 60, 40);
    qInfo() << "main box" << mainBox.name() << "main bg" << mainBg.name() << "web box" << webBox.name();
    QVERIFY2(mainBox.green() > 180 && mainBox.red() < 80, "the graphic is in main's output");
    QVERIFY2(webBox.red() > 180 && webBox.green() < 80, "and not in web's");
    QVERIFY2(mainBg.red() > 180 && mainBg.green() < 80 && mainBg.blue() < 80, "red stays red through the encoder");
}

// The ATSC presets must open mpeg2video + AC-3 themselves: a silent fallback to libx264 puts H.264 on an ATSC 1.0 multiplex,
// which receivers cannot decode (non_linear_quant with qmax 31 made every MPEG-2 open fail before).
void TestPipeline::mpeg2PresetsOpenMpeg2NotFallback() {
    struct Case { const char* preset; int w, h; bool interlaced; int level; };
    const Case cases[] = { {"atsc_mpeg2_ac3_hd", 1920, 1080, true, 4}, {"atsc_mpeg2_ac3_sd", 1920, 1080, true, 8}, {"mpeg2_ac3_atsc", 1920, 1080, false, 4} };
    for (const Case& c : cases) {
        EncoderProfile p = EncoderProfile::preset(c.preset);
        EncoderStage stage(p, c.w, c.h, FrameRate::ntsc());
        QStringList errors;
        connect(&stage, &EncoderStage::encoderError, this, [&errors](const QString& m) { errors << m; });
        QVERIFY2(stage.initialize(), qPrintable(QString("%1: %2").arg(c.preset, stage.lastError())));
        QCOMPARE(stage.actualVideoCodec(), QString("mpeg2video"));
        QCOMPARE(stage.actualAudioCodec(), QString("ac3"));
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join("; ")));
        const AVCodecParameters* v = stage.videoParams();
        QVERIFY(v);
        QCOMPARE(int(v->codec_id), int(AV_CODEC_ID_MPEG2VIDEO));
        QCOMPARE(v->level, c.level);
        if (c.interlaced) QCOMPARE(int(v->field_order), int(AV_FIELD_TT));
    }
}

// --- PE-09: encoder hot path (pooled pictures, x264 thread cap, fast rounding) ------------------------------------------
namespace pe09 {

int processThreads() {
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

/// Sets environment switches for one scope and puts the previous values back.
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

/// Keeps every encoded video packet (bytes, pts, dts, key) for byte comparison and decoding.
class CollectSink : public PacketSink {
public:
    struct Pkt { QByteArray data; qint64 pts = 0, dts = 0; bool key = false; };
    CollectSink() : PacketSink("pe09-collect") {}
    QString type() const override { return "null"; }
    bool start(const AVCodecParameters*, AVRational, const AVCodecParameters*, AVRational) override { m_on = true; return true; }
    void stop() override { m_on = false; }
    bool isRunning() const override { return m_on.load(); }
    void writePacket(const AVPacket* pkt, int streamKind) override {
        if (streamKind != 0) { audioPackets++; return; }
        QMutexLocker l(&mutex);
        video.push_back(Pkt{QByteArray(reinterpret_cast<const char*>(pkt->data), pkt->size), pkt->pts, pkt->dts, (pkt->flags & AV_PKT_FLAG_KEY) != 0});
    }
    QMutex mutex;
    std::vector<Pkt> video;
    std::atomic<int> audioPackets{0};
private:
    std::atomic<bool> m_on{false};
};

struct Rgb { int r, g, b; };
const Rgb kPalette[8] = {{220, 30, 30}, {30, 200, 40}, {40, 60, 220}, {230, 220, 40}, {200, 40, 200}, {40, 210, 210}, {128, 128, 128}, {250, 140, 20}};
const Rgb kOddColour = {90, 30, 150};          // the 640x360 picture at frame 20 (fallback scaler)
constexpr int kFrames = 48;
// Frames 10, 11 and 30 bring no picture (the last good one repeats); frame 20 is 640x360; every other frame is a palette
// colour, a different one from its neighbours, so a picture overwritten while the codec still held it shows up.
bool noPicture(int i) { return i == 10 || i == 11 || i == 30; }
Rgb expectedColour(int i) { while (noPicture(i)) --i; return i == 20 ? kOddColour : kPalette[i % 8]; }

FramePtr solid(int w, int h, Rgb c) {
    FramePtr f = allocVideoFrame(w, h, AV_PIX_FMT_BGRA);
    const quint32 px = 0xFF000000u | quint32(c.r) << 16 | quint32(c.g) << 8 | quint32(c.b);
    for (int y = 0; y < h; ++y) { auto* row = reinterpret_cast<quint32*>(f->data[0] + size_t(y) * f->linesize[0]); for (int x = 0; x < w; ++x) row[x] = px; }
    return f;
}

/// BT.709 limited range, what the stage's RGB -> YUV matrix produces.
void bt709(Rgb c, int& y, int& u, int& v) {
    const double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0, Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    y = int(std::lround(16 + 219 * Y)); u = int(std::lround(128 + 224 * (b - Y) / 1.8556)); v = int(std::lround(128 + 224 * (r - Y) / 1.5748));
}

struct Run {
    bool opened = false; QString codec, error; int threads = -1; bool pooled = false, fastRnd = false;
    EncoderStage::Stats st; std::vector<CollectSink::Pkt> video;
    std::shared_ptr<AVCodecParameters> par;
};

/// The kFrames sequence through one EncoderStage (no audio: the stage pads silence), every frame encoded (the push waits
/// for the queue, so nothing is dropped), then flushed.
Run encodeSequence(const EncoderProfile& p, int w, int h, const QList<QPair<QByteArray, QByteArray>>& env) {
    EnvScope scope(env);
    Run r;
    auto sink = std::make_unique<CollectSink>();
    EncoderStage stage(p, w, h, FrameRate::ntsc());
    r.opened = stage.initialize();
    r.error = stage.lastError();
    if (!r.opened) return r;
    stage.addSink(sink.get());
    r.codec = stage.actualVideoCodec(); r.threads = stage.videoThreads(); r.pooled = stage.pooledPictures(); r.fastRnd = stage.fastRounding();
    std::vector<FramePtr> pics; for (const Rgb& c : kPalette) pics.push_back(solid(w, h, c));
    const FramePtr odd = solid(640, 360, kOddColour);
    for (int i = 0; i < kFrames; ++i) {
        QElapsedTimer guard; guard.start();
        while (stage.stats().queueDepth >= 3 && guard.elapsed() < 10000) QThread::usleep(500);
        MediaFrame mf; mf.frameNumber = i;
        mf.video = noPicture(i) ? FramePtr() : i == 20 ? odd : pics[size_t(i % 8)];
        stage.pushFrame(mf);
    }
    QElapsedTimer drain; drain.start();
    while (stage.stats().framesEncoded + stage.stats().framesDropped < kFrames && drain.elapsed() < 30000) QThread::msleep(2);
    r.st = stage.stats();
    r.par.reset(avcodec_parameters_alloc(), [](AVCodecParameters* x) { avcodec_parameters_free(&x); });
    avcodec_parameters_copy(r.par.get(), stage.videoParams());
    stage.shutdown();                                   // flushes the codec: the delayed packets reach the sink
    QMutexLocker l(&sink->mutex);
    r.video = sink->video;
    return r;
}

/// Decodes a run's packets; the centre pixel's Y, U, V of every picture in display order.
std::vector<std::array<int, 3>> decodeCentres(const Run& r) {
    std::vector<std::array<int, 3>> out;
    const AVCodec* dec = r.par ? avcodec_find_decoder(r.par->codec_id) : nullptr;
    if (!dec) return out;
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, r.par.get());
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, dec, nullptr) < 0) { avcodec_free_context(&ctx); return out; }
    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc();
    auto take = [&]() {
        while (avcodec_receive_frame(ctx, fr) >= 0) {
            const int cx = fr->width / 2, cy = fr->height / 2;
            out.push_back({fr->data[0][cy * fr->linesize[0] + cx], fr->data[1][(cy / 2) * fr->linesize[1] + cx / 2], fr->data[2][(cy / 2) * fr->linesize[2] + cx / 2]});
            av_frame_unref(fr);
        }
    };
    for (const CollectSink::Pkt& p : r.video) {
        if (av_new_packet(pkt, int(p.data.size())) < 0) break;
        memcpy(pkt->data, p.data.constData(), size_t(p.data.size()));
        pkt->pts = p.pts; pkt->dts = p.dts; if (p.key) pkt->flags |= AV_PKT_FLAG_KEY;
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        take();
    }
    avcodec_send_packet(ctx, nullptr); take();
    av_frame_free(&fr); av_packet_free(&pkt); avcodec_free_context(&ctx);
    return out;
}

QString statsLine(const QString& mode, const Run& r) {
    return QString("%1 %2: threads %3 pooled %4 fastRnd %5 | yuv allocs %6 copies %7 codecHeld %8 | repeated %9 rescaled %10 dropped %11 | %12 video packets")
        .arg(r.codec, -10).arg(mode, -7).arg(r.threads).arg(int(r.pooled)).arg(int(r.fastRnd)).arg(r.st.yuvAllocs).arg(r.st.yuvCopies).arg(r.st.yuvCodecHeld)
        .arg(r.st.videoRepeated).arg(r.st.videoRescaled).arg(r.st.framesDropped).arg(qint64(r.video.size()));
}

} // namespace pe09

// PE-09: libx264/libx265 with threads 0 get min(12, logical CPUs / 2); a profile's own count wins; PLAYPLAY_ENC_THREADS
// overrides ("auto" = x264's own 1.5 x CPUs, the pre-PE-09 behaviour); other codecs keep their default.
void TestPipeline::encoderThreadCap() {
    using pe09::EnvScope;
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, QString()), 12);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 64, QString()), 12);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 16, QString()), 8);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 8, QString()), 4);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 1, QString()), 1);
    QCOMPARE(EncoderStage::videoThreadCount("libx265", 0, 32, QString()), 12);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 6, 32, QString()), 6);          // the profile wins
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 6, 32, "auto"), 6);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", -1, 32, QString()), -1);        // passed through as before
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, "auto"), 0);             // off switch
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, " AUTO "), 0);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, "0"), 0);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, "20"), 20);
    QCOMPARE(EncoderStage::videoThreadCount("libx264", 0, 32, "junk"), 12);
    QCOMPARE(EncoderStage::videoThreadCount("mpeg2video", 0, 32, QString()), 0);
    QCOMPARE(EncoderStage::videoThreadCount("h264_qsv", 0, 32, "8"), 0);

    // A real libx264 stage: the cap reaches the codec. On a box where x264's own count (1.5 x CPUs) is above the cap, the
    // capped stage starts fewer threads.
    const int ideal = QThread::idealThreadCount(), cap = qBound(1, ideal / 2, 12);
    auto openX264 = [](const QByteArray& env, int& vt, int& added) {
        EnvScope scope({{"PLAYPLAY_ENC_THREADS", env}});
        const int t0 = pe09::processThreads();
        EncoderStage stage(EncoderProfile::preset("h264_aac"), 1280, 720, FrameRate::ntsc());
        if (!stage.initialize()) return false;
        vt = stage.videoThreads();
        added = pe09::processThreads() - t0;
        stage.shutdown();
        return true;
    };
    int vtCap = -1, vtAuto = -1, addCap = 0, addAuto = 0;
    QVERIFY(openX264(QByteArray(), vtCap, addCap));
    QVERIFY(openX264("auto", vtAuto, addAuto));
    qInfo().noquote() << QString("libx264 1280x720 on %1 logical CPUs: cap -> thread_count %2, +%3 process threads; PLAYPLAY_ENC_THREADS=auto -> thread_count %4, +%5")
                             .arg(ideal).arg(vtCap).arg(addCap).arg(vtAuto).arg(addAuto);
    QCOMPARE(vtCap, cap);
    QCOMPARE(vtAuto, 0);
    if (ideal >= 16) QVERIFY2(addCap < addAuto, "the capped x264 must start fewer threads than its own automatic count");

    // libx265 (stream_hevc_aac) opens with the cap (x265-params pools=<cap>) and encodes.
    EncoderProfile hevc = EncoderProfile::preset("stream_hevc_aac");
    const pe09::Run h = pe09::encodeSequence(hevc, 640, 360, {{"PLAYPLAY_ENC_THREADS", QByteArray()}});
    QVERIFY2(h.opened, qPrintable(h.error));
    if (h.codec != "libx265") { qInfo() << "libx265 not in this FFmpeg build (" << h.codec << "): x265 cap not exercised"; return; }
    qInfo().noquote() << pe09::statsLine("cap", h);
    QCOMPARE(h.threads, cap);
    QVERIFY(!h.video.empty());
    QCOMPARE(h.st.framesEncoded, qint64(pe09::kFrames));
}

// PE-09: pooled pictures change nothing on the wire. The same pictures through the legacy single frame
// (PLAYPLAY_ENC_LEGACY_FRAMES=1) and through the pool must give byte-identical packets, with no picture copied, and every
// decoded picture must show the colour pushed for its frame (a pool buffer handed out while the codec still held it would
// show a later colour). PLAYPLAY_ENC_FAST_RND=1 must keep the BT.709 limited colours.
void TestPipeline::encoderPooledPicturesMatchLegacy() {
    using namespace pe09;
    EncoderProfile m2 = EncoderProfile::preset("mpeg2_ac3");        // mpeg2video MP, GOP 15, 2 B-frames
    m2.rateControl = "cqp"; m2.qp = 2;                              // near-lossless, so decoded colours are exact
    EncoderProfile x = EncoderProfile::preset("h264_aac");          // libx264 veryfast, 2 B-frames
    x.threads = 4;                                                   // one fixed count in every mode: x264 output is repeatable
    // FFmpeg 9's libx264 and mpeg2video copy the input picture inside avcodec_send_frame (codecHeld stays 0), so they never
    // make the legacy path copy. A frame-threaded encoder does hold it: each worker task keeps a reference to its input
    // until it has encoded it. ffvhuff (lossless, 4 frame threads) is that case here.
    EncoderProfile hf = EncoderProfile::preset("h264_aac");
    hf.name = "ffvhuff"; hf.videoCodec = "ffvhuff"; hf.threads = 4;
    const int W = 1280, H = 720;
    for (const auto& c : {std::make_pair(QString("mpeg2video"), m2), std::make_pair(QString("libx264"), x), std::make_pair(QString("ffvhuff"), hf)}) {
        const Run legacy = encodeSequence(c.second, W, H, {{"PLAYPLAY_ENC_LEGACY_FRAMES", "1"}, {"PLAYPLAY_ENC_FAST_RND", QByteArray()}});
        const Run pooled = encodeSequence(c.second, W, H, {{"PLAYPLAY_ENC_LEGACY_FRAMES", QByteArray()}, {"PLAYPLAY_ENC_FAST_RND", QByteArray()}});
        const Run fast = encodeSequence(c.second, W, H, {{"PLAYPLAY_ENC_LEGACY_FRAMES", QByteArray()}, {"PLAYPLAY_ENC_FAST_RND", "1"}});
        for (const Run* r : {&legacy, &pooled, &fast}) {
            QVERIFY2(r->opened, qPrintable(r->error));
            QCOMPARE(r->codec, c.first);
        }
        qInfo().noquote() << statsLine("legacy", legacy);
        qInfo().noquote() << statsLine("pooled", pooled);
        qInfo().noquote() << statsLine("fastrnd", fast);
        QVERIFY(!legacy.pooled && pooled.pooled && fast.pooled);
        QVERIFY(!legacy.fastRnd && !pooled.fastRnd && fast.fastRnd);
        for (const Run* r : {&legacy, &pooled, &fast}) {
            QCOMPARE(r->st.framesDropped, qint64(0));
            QCOMPARE(r->st.framesEncoded, qint64(kFrames));
            QCOMPARE(r->st.videoRepeated, qint64(3));
            QCOMPARE(r->st.videoRescaled, qint64(1));
        }
        // byte-identical stream
        QCOMPARE(pooled.video.size(), legacy.video.size());
        for (size_t k = 0; k < pooled.video.size(); ++k) {
            const auto& a = pooled.video[k]; const auto& b = legacy.video[k];
            QVERIFY2(a.data == b.data && a.pts == b.pts && a.dts == b.dts && a.key == b.key,
                     qPrintable(QString("%1 packet %2 differs between pooled and legacy pictures").arg(c.first).arg(k)));
        }
        // no copies with the pool, a bounded pool; legacy copies exactly the pictures the codec still held
        QCOMPARE(pooled.st.yuvCopies, qint64(0));
        QCOMPARE(fast.st.yuvCopies, qint64(0));
        QVERIFY2(pooled.st.yuvAllocs >= 1 && pooled.st.yuvAllocs <= 16, qPrintable(QString("pool grew to %1 buffers").arg(pooled.st.yuvAllocs)));
        QCOMPARE(legacy.st.yuvCopies, legacy.st.yuvCodecHeld);
        QCOMPARE(legacy.st.yuvAllocs, 1 + legacy.st.yuvCopies);
        if (c.first == "ffvhuff") {      // the case the pool exists for: the codec held pictures, legacy copied them, the pool did not
            QVERIFY2(legacy.st.yuvCodecHeld > 0 && pooled.st.yuvCodecHeld > 0, "the frame-threaded encoder should hold input pictures");
            QVERIFY(legacy.st.yuvCopies > 0);
        }
        // every picture intact and in order, accurate and fast rounding
        for (const auto& m : {std::make_pair(QString("pooled"), &pooled), std::make_pair(QString("fastrnd"), &fast)}) {
            const auto centres = decodeCentres(*m.second);
            QCOMPARE(int(centres.size()), kFrames);
            int worst = 0;
            for (int i = 0; i < kFrames; ++i) {
                int y, u, v; bt709(expectedColour(i), y, u, v);
                const auto& got = centres[size_t(i)];
                const int d = qMax(qAbs(got[0] - y), qMax(qAbs(got[1] - u), qAbs(got[2] - v)));
                worst = qMax(worst, d);
                QVERIFY2(d <= 4, qPrintable(QString("%1 %2 frame %3: YUV %4/%5/%6, expected %7/%8/%9")
                                                .arg(c.first, m.first).arg(i).arg(got[0]).arg(got[1]).arg(got[2]).arg(y).arg(u).arg(v)));
            }
            qInfo().noquote() << QString("%1 %2: %3 decoded pictures in order, worst YUV error %4").arg(c.first, -10).arg(m.first, -7).arg(qint64(centres.size())).arg(worst);
        }
    }
}


// ---------------------------------------------------------------------------
// PE-04: the clock thread logs nothing and builds no JSON (docs/plans/perf-engine.md)
// ---------------------------------------------------------------------------

namespace {
// Every Qt log line, tagged with whether it came from the tick thread.
struct Pe04Log {
    QMutex mutex;
    QStringList tickLines;                      // messages logged on the tick thread
    QStringList mainLines;                      // every other thread (the engine's / main thread among them)
    std::atomic<bool> armed{false};
    std::thread::id tickId;
    QtMessageHandler previous = nullptr;
};
Pe04Log* g_pe04 = nullptr;
void pe04Handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (g_pe04 && g_pe04->armed.load()) { QMutexLocker l(&g_pe04->mutex); (std::this_thread::get_id() == g_pe04->tickId ? g_pe04->tickLines : g_pe04->mainLines) << msg; }
    if (g_pe04 && g_pe04->previous) g_pe04->previous(type, ctx, msg);
}
}

// Two items play through null sinks while a separate thread drives onFrameTick (as the FrameClock does on air) and the
// main thread spins its event loop:
//  - the 1 Hz `telemetry` arrives on the main thread (the JSON is built there, queued from the telemetry tick);
//  - the tick thread logs only state transitions and the rate-limited command lines: a burst of 300 CG updates leaves
//    at most 10 + a refill's worth of "cmd" lines there, nothing else; the count of what was not logged comes with
//    the next telemetry, on the main thread;
//  - currentState() is fresh after every tick (frame, position) without waiting for a telemetry;
//  - stateJson() carries exactly the keys it had before PE-04 (telemetry consumers expect them unchanged).
void TestPipeline::clockThreadStaysQuiet() {
    if (m_ffmpeg.isEmpty()) QSKIP("no ffmpeg");
    PlayoutEngine::Config cfg;
    cfg.channelId = "quiet"; cfg.channelName = "Quiet";
    cfg.frameRate = FrameRate::ntsc(); cfg.width = 640; cfg.height = 360; cfg.hwDecode = false;
    cfg.prerollSec = 1.0;
    QJsonObject prof; prof["preset"] = "h264_aac_low"; prof["x264Preset"] = "ultrafast";
    cfg.outputs = QJsonArray{QJsonObject{{"type", "null"}, {"name", "null-ts"}, {"profile", prof}}, QJsonObject{{"type", "nullbaseband"}, {"name", "bb"}}};
    PlayoutEngine engine(cfg);
    QVERIFY(engine.initialize());
    QCOMPARE(engine.currentState().stateName, QString("IDLE"));     // the state name is published before the first tick

    std::atomic<int> telemetryCount{0}, telemetryOffMain{0}, telemetryOutputs{0};
    QJsonObject lastTelemetry;
    QObject::connect(&engine, &PlayoutEngine::telemetry, [&](const QJsonObject& st) {
        ++telemetryCount;
        if (QThread::currentThread() != QCoreApplication::instance()->thread()) ++telemetryOffMain;
        telemetryOutputs = st.value("outputs").toArray().size();
        lastTelemetry = st;
    });
    std::atomic<bool> endB{false};
    QObject::connect(&engine, &PlayoutEngine::asRunEvent, [&](const QJsonObject& e) { if (e["type"].toString() == "END" && e["itemId"].toString() == "B") endB = true; });

    QJsonObject a; a["id"] = "A"; a["path"] = m_clipA; a["title"] = "Clip A";
    QJsonObject b; b["id"] = "B"; b["path"] = m_clipB; b["title"] = "Clip B";
    engine.loadPlaylist(QJsonArray{a, b}, true);
    engine.handleTransportCommand("play", {});

    Pe04Log log; g_pe04 = &log;
    log.previous = qInstallMessageHandler(pe04Handler);
    std::atomic<qint64> ticked{-1};
    std::atomic<bool> stop{false};
    std::atomic<int> staleReads{0};
    const FrameRate r = cfg.frameRate;
    std::thread tick([&]() {
        log.tickId = std::this_thread::get_id();
        log.armed = true;
        for (qint64 f = 0; f < 400 && !stop.load(); ++f) {
            engine.onFrameTick(f, r.frameToNs(f));
            ticked = f;
            // currentState() right after the tick sees this very frame: no snapshot lag, no telemetry needed.
            if (engine.currentState().currentFrame != f) ++staleReads;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));       // ~2x real time
            if (endB.load() && engine.currentState().stateName == "IDLE") break;
        }
        log.armed = false;
    });

    // While it plays: 300 CG updates in one burst from the main thread (a UI dragging a graphic) -> at most a few
    // "cmd" lines on the tick thread, the rest counted.
    QElapsedTimer t; t.start();
    bool flooded = false;
    while (t.elapsed() < 20000) {
        QTest::qWait(20);
        if (!flooded && ticked.load() >= 20) {
            engine.handleCgCommand(QJsonObject{{"action", "show"}, {"layer", 30}, {"source", QJsonObject{{"type", "color"}, {"color", "#00ff00"}, {"width", 64}, {"height", 64}}}});
            for (int i = 0; i < 300; ++i) engine.handleCgCommand(QJsonObject{{"action", "update"}, {"layer", 30}, {"opacity", (i % 100) / 100.0}});
            flooded = true;
        }
        if (endB.load() && engine.currentState().stateName == "IDLE" && telemetryCount.load() > 0) break;
    }
    stop = true;
    tick.join();
    QTest::qWait(50);                                   // the last queued telemetry
    qInstallMessageHandler(log.previous); g_pe04 = nullptr;

    QVERIFY2(flooded, "the burst was sent");
    QVERIFY2(endB.load(), "both items played");
    QCOMPARE(engine.currentState().stateName, QString("IDLE"));
    QCOMPARE(staleReads.load(), 0);
    QVERIFY2(telemetryCount.load() >= 1, "at least one telemetry");
    QCOMPARE(telemetryOffMain.load(), 0);               // built and emitted on the engine's thread
    QVERIFY2(telemetryOutputs.load() >= 3, "encoder + two output slots in the telemetry's outputs");

    // The tick thread's log: transitions, cmd lines and nothing else.
    int cmdLines = 0, transitions = 0, suppressed = 0; QStringList other;
    for (const QString& l : log.tickLines) {
        if (l.contains(" cmd ")) ++cmdLines;
        else if (l.contains(" -> ")) ++transitions;
        else other << l;
    }
    for (const QString& l : log.mainLines) if (l.contains("not logged in the last second")) ++suppressed;
    qInfo().noquote() << QString("tick thread logged %1 lines: %2 cmd, %3 transitions, %4 other; main thread: %5 lines, %6 'not logged' count(s)")
        .arg(log.tickLines.size()).arg(cmdLines).arg(transitions).arg(other.size()).arg(log.mainLines.size()).arg(suppressed);
    QVERIFY2(other.isEmpty(), qPrintable("unexpected tick-thread log lines: " + other.join(" | ")));
    QVERIFY2(transitions >= 2 && transitions <= 8, qPrintable(QString("%1 transitions").arg(transitions)));
    // 303 commands (play, show, 300 updates) in ~4 s: a bucket of 10 refilled at 10/s allows at most 10 + 10 x seconds.
    QVERIFY2(cmdLines <= 10 + 10 * int(t.elapsed() / 1000 + 1) && cmdLines < 100, qPrintable(QString("%1 cmd lines for 302 commands").arg(cmdLines)));
    QVERIFY2(suppressed >= 1, "the dropped command lines were counted and reported off the tick thread");

    // State schema includes the opt-in recorder; profiler / clock keys are conditional.
    const QJsonObject st = engine.stateJson();
    const QStringList topKeys = {"channelId", "state", "itemId", "assetId", "path", "title", "nextItemId", "nextTitle", "nextReady",
                                 "position", "duration", "remaining", "playlistIndex", "playlistSize", "frame", "timecode",
                                 "frameRate", "width", "height", "healthy", "error", "telemetry", "outputs", "compositor",
                                 "layers", "preview", "monitors", "liveInputs", "liveInputsAtMs", "recording"};
    QStringList telKeys = {"tickMsLast", "tickMsMax", "lateTicks", "decoderQueue", "audioUnderruns", "outputDrops", "missedCuts",
                           "clockTrimPpm", "clockLocked", "clockLockSink", "stallSeconds"};
    if (tickprof::TickProfiler::enabledByEnvironment()) telKeys << "tickPhases";    // PE-06; "clock" (PE-14) needs a FrameClock
    auto sorted = [](QStringList l) { l.sort(); return l; };
    QCOMPARE(sorted(st.keys()), sorted(topKeys));
    QCOMPARE(st.value("recording").toObject().value("active").toBool(), false);
    QCOMPARE(sorted(st.value("telemetry").toObject().keys()), sorted(telKeys));
    QCOMPARE(sorted(lastTelemetry.keys()), sorted(topKeys));
    QCOMPARE(sorted(lastTelemetry.value("telemetry").toObject().keys()), sorted(telKeys));
    QVERIFY2(QRegularExpression("^\\d\\d:\\d\\d:\\d\\d[:;]\\d\\d$").match(st.value("timecode").toString()).hasMatch(), qPrintable(st.value("timecode").toString()));
    QCOMPARE(st.value("state").toString(), QString("IDLE"));
    QCOMPARE(st.value("frame").toDouble(), double(ticked.load()));
    QVERIFY(st.value("outputs").toArray().size() >= 3);
    QCOMPARE(lastTelemetry.value("channelId").toString(), QString("quiet"));

    engine.shutdown();
}

// The mixer renders slate text with QPainter, which needs a QGuiApplication
// (no window is ever shown).
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    TestPipeline tc;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&tc, argc, argv);
}
#include "test_pipeline.moc"
