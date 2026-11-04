#include <QtTest/QtTest>
#include "playout/core/framerate.h"

// FrameRate / audio cadence / timecode math. These are the invariants that keep
// a 24/7 channel lip-synced and on time, so they get exact tests.
class TestFrameRate : public QObject {
    Q_OBJECT

private slots:
    void frameToNsIsExactAndMonotonic();
    void nsToFrameInvertsFrameToNs();
    void noOverflowAfterDays();
    void audioCadenceSumsExactly();
    void audioCadencePattern();
    void dropFrameTimecodeKnownValues();
    void nonDropTimecode();
    void parsing();
};

void TestFrameRate::frameToNsIsExactAndMonotonic() {
    const FrameRate r = FrameRate::ntsc();
    QCOMPARE(r.frameToNs(0), 0LL);
    QCOMPARE(r.frameToNs(30000), 1001000000000LL);        // 30000 frames = 1001 s exactly
    QCOMPARE(r.frameToNs(1), 33366666LL);                 // floor(1e9*1001/30000)
    qint64 prev = -1;
    for (qint64 f = 0; f < 100000; ++f) {
        const qint64 ns = r.frameToNs(f);
        QVERIFY(ns > prev);
        prev = ns;
    }
}

void TestFrameRate::nsToFrameInvertsFrameToNs() {
    for (const FrameRate& r : { FrameRate::ntsc(), FrameRate::ntsc60(), FrameRate::pal(), FrameRate::film(), FrameRate::fps30() }) {
        for (qint64 f = 0; f < 50000; f += 7) {
            const qint64 ns = r.frameToNs(f);
            QCOMPARE(r.nsToFrame(ns), f);
            QCOMPARE(r.nsToFrame(ns + r.frameDurationNs() - 1), f);
            QCOMPARE(r.nsToFrame(ns + r.frameDurationNs() + 1), f + 1);
        }
    }
}

void TestFrameRate::noOverflowAfterDays() {
    const FrameRate r = FrameRate::ntsc60();
    const qint64 oneYearFrames = 60000LL * 86400LL * 365LL / 1001LL;   // ~1.89e9
    const qint64 ns = r.frameToNs(oneYearFrames);
    QVERIFY(ns > 0);
    // 1 year in ns is 3.1536e16; allow a frame of slack.
    QVERIFY(qAbs(ns - 31536000000000000LL) < 20000000LL);
    QCOMPARE(r.nsToFrame(ns), oneYearFrames);
}

void TestFrameRate::audioCadenceSumsExactly() {
    const FrameRate r = FrameRate::ntsc();
    qint64 sum = 0;
    for (qint64 f = 0; f < 30000; ++f) sum += r.samplesForFrame(f, 48000);
    QCOMPARE(sum, 48000LL * 1001LL);                      // 1001 s of audio in 30000 frames
    for (qint64 f = 0; f < 30000; ++f) {
        const qint64 n = r.samplesForFrame(f, 48000);
        QVERIFY(n == 1601 || n == 1602);
    }
    QCOMPARE(FrameRate::pal().samplesForFrame(123, 48000), 1920LL);
    QCOMPARE(FrameRate::fps30().samplesForFrame(123, 48000), 1600LL);
    QCOMPARE(FrameRate::ntsc60().samplesForFrame(0, 48000), 800LL);
}

void TestFrameRate::audioCadencePattern() {
    // Classic 29.97 cadence: 1602,1601,1602,1601,1602 repeating per 5 frames (8008 samples).
    const FrameRate r = FrameRate::ntsc();
    qint64 five = 0;
    for (int f = 0; f < 5; ++f) five += r.samplesForFrame(f, 48000);
    QCOMPARE(five, 8008LL);
}

void TestFrameRate::dropFrameTimecodeKnownValues() {
    const FrameRate r = FrameRate::ntsc();
    QCOMPARE(Timecode::fromFrames(0, r), QString("00:00:00;00"));
    QCOMPARE(Timecode::fromFrames(29, r), QString("00:00:00;29"));
    QCOMPARE(Timecode::fromFrames(30, r), QString("00:00:01;00"));
    // Minute 0 has all 1800 labels; minute 1 starts by skipping ;00 and ;01.
    QCOMPARE(Timecode::fromFrames(1799, r), QString("00:00:59;29"));
    QCOMPARE(Timecode::fromFrames(1800, r), QString("00:01:00;02"));
    QCOMPARE(Timecode::fromFrames(1800 + 1798, r), QString("00:02:00;02"));
    // 10 minutes = 17982 frames, no drop at the 10-minute mark.
    QCOMPARE(Timecode::fromFrames(17982, r), QString("00:10:00;00"));
    // One hour = 107892 frames.
    QCOMPARE(Timecode::fromFrames(107892, r), QString("01:00:00;00"));
    // 59.94 drops 4 per minute.
    const FrameRate r60 = FrameRate::ntsc60();
    QCOMPARE(Timecode::fromFrames(3600, r60), QString("00:01:00;04"));
    QCOMPARE(Timecode::fromFrames(215784, r60), QString("01:00:00;00"));
}

void TestFrameRate::nonDropTimecode() {
    QCOMPARE(Timecode::fromFrames(25 * 3600 + 25 * 60 + 25 + 1, FrameRate::pal()), QString("01:01:01:01"));
    QCOMPARE(Timecode::fromFrames(30 * 86400 - 1, FrameRate::fps30()), QString("23:59:59:29"));
}

void TestFrameRate::parsing() {
    QCOMPARE(FrameRate::fromString("30000/1001"), FrameRate::ntsc());
    QCOMPARE(FrameRate::fromString("29.97"), FrameRate::ntsc());
    QCOMPARE(FrameRate::fromString("59.94"), FrameRate::ntsc60());
    QCOMPARE(FrameRate::fromString("25"), FrameRate::pal());
    QCOMPARE(FrameRate::fromString("23.976"), FrameRate::film());
    QCOMPARE(FrameRate::ntsc().toString(), QString("30000/1001"));
    QVERIFY(FrameRate::ntsc().isDropFrame());
    QVERIFY(!FrameRate::fps30().isDropFrame());
    QCOMPARE(FrameRate::ntsc().nominalFps(), 30);
    QCOMPARE(FrameRate::ntsc60().nominalFps(), 60);
}

QTEST_GUILESS_MAIN(TestFrameRate)
#include "test_framerate.moc"
