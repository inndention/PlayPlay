#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "playout/engine/compositor/livesources.h"
#include "playout/engine/compositor/basicsources.h"
#include "playout/engine/mediaframe.h"

/**
 * PE-03 (docs/plans/perf-engine.md): the engine's clock thread calls video() / audio() / status() on live sources every
 * frame (program layers, CG layers, input monitors). Before PE-03 the NDI receive loop slept 200 ms holding the
 * source's mutex while it searched for an absent sender, so those calls stalled the clock (~1.4 late ticks/s, 54 min lost
 * on 2026-10-24). These tests call the three methods from a time-critical "clock" thread every 33 ms and require every
 * call to return in under 2 ms:
 *   ndiMissingSender        NdiSource on a sender that cannot exist (perftest-missing-<pid>): the search loop runs
 *   ndiLegacySwitchStalls   the off switch PLAYPLAY_LIVESRC_LEGACY_LOCK=1 brings the stall back (proves the test sees it)
 *   streamSource            FileSource type "stream" (lavfi test pattern paced in real time, decoder on its own thread)
 *   producerContention      a synthetic LiveSourceBase whose producer publishes pictures + resampled audio while the
 *                           clock polls: no slow call, exact video counters, audio arrives intact, dropBufferedAudio works
 * Only receivers are created (no NDI sender is announced) and every name starts with perftest-, so the live channels'
 * NDI senders (main / ch2), which the finder can see on this machine, never match. PLAYPLAY_TEST_LIVE_SEC (default 5)
 * sets the polling time of each case.
 */
class TestLiveSourceNonBlocking : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void ndiMissingSender();
    void ndiLegacySwitchStalls();
    void streamSource();
    void producerContention();
private:
    double m_seconds = 5.0;
};

namespace {

const double kFramePeriodS = 1001.0 / 30000.0;
const double kSlowMs = 2.0;

struct Dist { double avg = 0, p95 = 0, max = 0; };
Dist dist(std::vector<double> v) {
    Dist d; if (v.empty()) return d;
    double s = 0; for (double x : v) s += x; d.avg = s / double(v.size());
    std::sort(v.begin(), v.end()); d.p95 = v[std::min(v.size() - 1, size_t(double(v.size()) * 0.95))]; d.max = v.back();
    return d;
}

struct Poll {
    std::vector<double> v, a, s;              // ms per call
    int slow = 0, calls = 0, pictures = 0, uniquePictures = 0, audioOk = 0, audioValueOk = 0;
    qint64 lastPts = AV_NOPTS_VALUE;          // pts of the last picture video() returned
    QStringList details;
    QJsonObject lastStatus;
    double maxAll() const { double m = 0; for (const auto* x : {&v, &a, &s}) for (double t : *x) m = std::max(m, t); return m; }
    QString line(const QString& what, double seconds) const {
        const Dist dv = dist(v), da = dist(a), ds = dist(s);
        return QString("LIVESRC %1 %2 s: video max %3 ms p95 %4  audio max %5 p95 %6  status max %7 p95 %8  calls>2ms %9/%10  "
                       "pictures %11 (new %12)  audio blocks %13  detail '%14'")
            .arg(what).arg(seconds, 0, 'f', 1).arg(dv.max, 0, 'f', 3).arg(dv.p95, 0, 'f', 3).arg(da.max, 0, 'f', 3).arg(da.p95, 0, 'f', 3)
            .arg(ds.max, 0, 'f', 3).arg(ds.p95, 0, 'f', 3).arg(slow).arg(calls).arg(pictures).arg(uniquePictures).arg(audioOk)
            .arg(details.join("|"));
    }
};

/// Call video() / audio() / status() every 33 ms for `seconds` from a TimeCritical thread (the engine's clock thread
/// runs at high priority too, and a high priority keeps scheduler preemption out of the per-call timings on a busy box).
/// `audioExpect` > 0: a returned block whose first sample is within 0.02 of it counts as audioValueOk.
Poll pollLikeClock(LayerSource& src, const OutputFormat& fmt, double seconds, float audioExpect = 0.0f) {
    Poll p;
    std::unique_ptr<QThread> t(QThread::create([&]() {
        FramePtr dst = allocAudioFrame(1601, fmt.channels, fmt.sampleRate);
        QElapsedTimer all; all.start();
        for (qint64 i = 0; all.elapsed() < qint64(seconds * 1000.0); ++i) {
            const qint64 dueNs = qint64(double(i) * kFramePeriodS * 1e9);
            while (all.nsecsElapsed() < dueNs) QThread::usleep(500);
            QElapsedTimer c; c.start();
            const FramePtr pic = src.video(i, double(i) * kFramePeriodS);
            const double v = c.nsecsElapsed() / 1e6; c.restart();
            const bool aok = src.audio(dst, i, double(i) * kFramePeriodS);
            const double a = c.nsecsElapsed() / 1e6; c.restart();
            const QJsonObject st = src.status();
            const double s = c.nsecsElapsed() / 1e6;
            p.v.push_back(v); p.a.push_back(a); p.s.push_back(s); p.calls += 3;
            p.slow += (v > kSlowMs) + (a > kSlowMs) + (s > kSlowMs);
            if (pic) { p.pictures++; if (pic->pts != p.lastPts) { p.uniquePictures++; p.lastPts = pic->pts; } }
            if (aok) {
                p.audioOk++;
                if (audioExpect > 0.0f && std::abs(reinterpret_cast<const float*>(dst->data[0])[0] - audioExpect) < 0.02f) p.audioValueOk++;
            }
            const QString d = st.value("detail").toString();
            if (!d.isEmpty() && !p.details.contains(d)) p.details << d;
            p.lastStatus = st;
        }
    }));
    t->start(QThread::TimeCriticalPriority);
    t->wait();
    return p;
}

QString missingSender(const char* suffix) {
    return QString("perftest-missing-%1-%2").arg(QCoreApplication::applicationPid()).arg(suffix);
}

/// A live source with a scripted producer thread: pictures (pts = sequence number) as fast as a 2 ms sleep allows, and
/// interleaved stereo float audio at `rate` Hz in real time (blocks of 10 ms) with every sample = `value`.
class FakeLive : public LiveSourceBase {
public:
    FakeLive() { m_wantAudio = true; }
    ~FakeLive() override { close(); }
    QString type() const override { return "fake_live"; }
    bool open(const OutputFormat& fmt) override { m_fmt = fmt; resetAudio(); m_open = true; return true; }
    void close() override { stopProducer(); clearVideo(); m_open = false; }
    bool isOpen() const override { return m_open; }

    void startProducer(int rate, float value) {
        m_stopProducer = false;
        m_producer.reset(QThread::create([this, rate, value]() {
            FramePool pool(640, 360, kMixerPixelFormat);
            const int block = rate / 100;
            std::vector<float> inter(size_t(block) * 2, value);
            QElapsedTimer t; t.start();
            qint64 pushed = 0;
            while (!m_stopProducer.load()) {
                FramePtr f = pool.get();
                if (f) { f->pts = ++m_published; publishVideo(f); }
                const qint64 due = t.nsecsElapsed() * rate / 1000000000LL;
                while (pushed + block <= due) {
                    const float* planes[1] = { inter.data() };
                    publishAudio(planes, false, 2, rate, block);
                    pushed += block;
                }
                QThread::usleep(2000);
            }
        }));
        m_producer->start(QThread::HighestPriority);
    }
    void stopProducer() {
        m_stopProducer = true;
        if (m_producer) { m_producer->wait(); m_producer.reset(); }
    }
    qint64 published() const { return m_published.load(); }

private:
    bool m_open = false;
    std::atomic<bool> m_stopProducer{false};
    std::atomic<qint64> m_published{0};
    std::unique_ptr<QThread> m_producer;
};

} // namespace

void TestLiveSourceNonBlocking::initTestCase() {
    const int s = qEnvironmentVariableIntValue("PLAYPLAY_TEST_LIVE_SEC");
    if (s > 0) m_seconds = s;
    qunsetenv("PLAYPLAY_LIVESRC_LEGACY_LOCK");     // the default (non-blocking) path unless a case asks for the switch
}

void TestLiveSourceNonBlocking::ndiMissingSender() {
    if (!NdiSource::isAvailable()) QSKIP("NDI not compiled in");
    const QString missing = missingSender("a");
    QVERIFY(missing.startsWith("perftest-"));
    OutputFormat fmt;
    NdiSource src(QJsonObject{{"type", "ndi"}, {"name", missing}, {"recvName", "perftest-test-rx"}, {"audio", true}});
    QVERIFY(!src.legacyLocking());
    if (!src.open(fmt)) QSKIP(qPrintable("NDI runtime not usable: " + src.lastError()));
    const Poll p = pollLikeClock(src, fmt, m_seconds);
    qInfo().noquote() << p.line("ndi(missing sender)", m_seconds);

    // The case under test really ran: the receive loop was searching (and never found the perftest- name).
    QVERIFY2(p.details.contains("waiting for source"), qPrintable("search loop not observed: " + p.details.join("|")));
    QCOMPARE(p.pictures, 0);
    QVERIFY(!p.lastStatus.value("connected").toBool(true));
    QVERIFY(!p.lastStatus.contains("legacyLock"));
    QVERIFY2(p.slow == 0, qPrintable(QString("%1 of %2 calls took > 2 ms (max %3 ms)").arg(p.slow).arg(p.calls).arg(p.maxAll(), 0, 'f', 2)));

    // Switching the wanted name while the loop searches: status() shows it at once, no lock wait.
    const QString other = missingSender("b");
    src.update(QJsonObject{{"name", other}});
    QElapsedTimer t; t.start();
    const QJsonObject st = src.status();
    const double statusMs = t.nsecsElapsed() / 1e6;
    QCOMPARE(st.value("source").toString(), other);
    QVERIFY2(statusMs < 50.0, qPrintable(QString("status() after update took %1 ms").arg(statusMs)));

    t.restart();
    src.close();
    qInfo().noquote() << QString("LIVESRC ndi close while searching: %1 ms").arg(t.nsecsElapsed() / 1e6, 0, 'f', 1);
    QVERIFY(!src.hasPicture());
}

void TestLiveSourceNonBlocking::ndiLegacySwitchStalls() {
    if (!NdiSource::isAvailable()) QSKIP("NDI not compiled in");
    qputenv("PLAYPLAY_LIVESRC_LEGACY_LOCK", "1");
    OutputFormat fmt;
    NdiSource src(QJsonObject{{"type", "ndi"}, {"name", missingSender("legacy")}, {"recvName", "perftest-test-rx"}, {"audio", true}});
    qunsetenv("PLAYPLAY_LIVESRC_LEGACY_LOCK");     // read once, when the source is created
    QVERIFY(src.legacyLocking());
    if (!src.open(fmt)) QSKIP(qPrintable("NDI runtime not usable: " + src.lastError()));
    const double seconds = std::min(m_seconds, 2.5);
    const Poll p = pollLikeClock(src, fmt, seconds);
    qInfo().noquote() << p.line("ndi(missing sender, PLAYPLAY_LIVESRC_LEGACY_LOCK=1)", seconds);
    src.close();
    QVERIFY(p.lastStatus.value("legacyLock").toBool());
    // The search loop holds the lock through a 200 ms sleep at least every ~0.7 s, and the clock polls every 33 ms, so a
    // call lands in a locked window: if this ever stops failing slow, the test can no longer see a PE-03 regression.
    QVERIFY2(p.maxAll() > 50.0, qPrintable(QString("legacy switch did not reproduce the stall (max %1 ms)").arg(p.maxAll(), 0, 'f', 2)));
}

void TestLiveSourceNonBlocking::streamSource() {
    // A network pull is a FileSource ("stream", realtime): the decoder runs on its own thread and the clock takes the
    // newest picture. lavfi's realtime filters pace the test pattern like a live feed, with no file and no network.
    OutputFormat fmt;
    FileSource src(QJsonObject{
        {"type", "stream"},
        {"url", "testsrc2=size=320x180:rate=30000/1001,realtime[out0];sine=frequency=440:sample_rate=48000,arealtime[out1]"},
        {"inputFormat", "lavfi"}, {"hwDecode", false}});
    QElapsedTimer o; o.start();
    if (!src.open(fmt)) {
        if (src.lastError().contains("input format not available")) QSKIP("lavfi not in this FFmpeg build");
        QFAIL(qPrintable("stream open failed: " + src.lastError()));
    }
    qInfo().noquote() << QString("LIVESRC stream open %1 ms").arg(o.elapsed());
    const Poll p = pollLikeClock(src, fmt, m_seconds);
    qInfo().noquote() << p.line("stream(lavfi realtime)", m_seconds);
    src.close();
    QVERIFY2(p.uniquePictures > 10, qPrintable(QString("only %1 new pictures from the stream").arg(p.uniquePictures)));
    QVERIFY2(p.audioOk > 0, "no audio from the stream");
    QVERIFY2(p.slow == 0, qPrintable(QString("%1 of %2 calls took > 2 ms (max %3 ms)").arg(p.slow).arg(p.calls).arg(p.maxAll(), 0, 'f', 2)));
}

void TestLiveSourceNonBlocking::producerContention() {
    // 44.1 kHz input: every audio block goes through the resampler on the producer side while the clock reads the FIFO.
    OutputFormat fmt;
    FakeLive src;
    QVERIFY(src.open(fmt));
    src.startProducer(44100, 0.25f);
    QThread::msleep(300);                          // fill the ¼ s FIFO before the clock starts pulling
    const Poll p = pollLikeClock(src, fmt, m_seconds, 0.25f);
    src.stopProducer();
    qInfo().noquote() << p.line("synthetic producer (pictures every ~2 ms, 44.1k audio)", m_seconds);

    QVERIFY2(p.slow == 0, qPrintable(QString("%1 of %2 calls took > 2 ms (max %3 ms)").arg(p.slow).arg(p.calls).arg(p.maxAll(), 0, 'f', 2)));
    QVERIFY(p.uniquePictures > 10);

    // Video counters stay exact with the split locks: every published picture was either shown or replaced unseen
    // (videoDropped), and every poll that got an already-shown picture counted one repeat.
    const FramePtr last = src.video(0, 0.0);       // serve the one the producer left, if the clock had not seen it yet
    QVERIFY(last);
    const bool lastWasNew = last->pts != p.lastPts;
    const QJsonObject st = src.status();
    const qint64 in = qint64(st.value("videoIn").toDouble()), dropped = qint64(st.value("videoDropped").toDouble());
    const qint64 repeats = qint64(st.value("videoRepeats").toDouble());
    qInfo().noquote() << QString("LIVESRC synthetic counters: published %1 videoIn %2 shown %3 dropped %4 repeats %5")
        .arg(src.published()).arg(in).arg(p.uniquePictures + (lastWasNew ? 1 : 0)).arg(dropped).arg(repeats);
    QCOMPARE(in, src.published());
    QCOMPARE(last->pts, src.published());
    QCOMPARE(in, qint64(p.uniquePictures) + (lastWasNew ? 1 : 0) + dropped);
    QCOMPARE(repeats, qint64(p.pictures - p.uniquePictures) + (lastWasNew ? 0 : 1));

    // Audio arrived through the resampler, intact, on nearly every frame.
    QVERIFY2(p.audioOk >= p.calls / 3 * 9 / 10, qPrintable(QString("audio on %1 of %2 frames").arg(p.audioOk).arg(p.calls / 3)));
    QVERIFY2(p.audioValueOk >= p.audioOk - 1, qPrintable(QString("%1 of %2 audio blocks had the wrong value").arg(p.audioOk - p.audioValueOk).arg(p.audioOk)));

    // A warm take drops the buffered sound: the next pull is silence.
    FramePtr dst = allocAudioFrame(1601, fmt.channels, fmt.sampleRate);
    src.dropBufferedAudio();
    QVERIFY(!src.audio(dst, 0, 0.0));
    src.close();
    QVERIFY(!src.hasPicture());
}

QTEST_GUILESS_MAIN(TestLiveSourceNonBlocking)
#include "test_livesource_nonblocking.moc"
