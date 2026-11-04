// MediaPreview: exact-frame JPEGs, storyboard sprites, SMPTE timecode. The clip is made with the staged ffmpeg.exe:
// 10 s at 25 fps, H.264 with B-frames and a 2 s GOP, every frame a flat grey whose level encodes its frame number,
// so an off-by-one frame (or a keyframe returned instead of the frame) shows up as the wrong grey.
#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonArray>
#include "playout/supervisor/mediapreview.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

class TestMediaPreview : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void infoReportsRateAndFrames();
    void framesAreExactAcrossGopsAndBackwardSteps();
    void timeSnapsToTheFrameOnScreen();
    void sameFrameGivesSameBytes();
    void storyboardSprite();
    void dropFrameTimecode();
private:
    QTemporaryDir m_dir;
    QString m_clip;
    QScopedPointer<MediaPreview> m_mp;
    static int expectedFull(qint64 n) { return int(std::lround((n * 7 % 200) * 255.0 / 219.0)); }   // studio grey -> JPEG full range
    static int meanLuma(const QByteArray& jpeg, int* w = nullptr, int* h = nullptr);
};

int TestMediaPreview::meanLuma(const QByteArray& jpeg, int* w, int* h) {
    const AVCodec* c = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    AVCodecContext* cc = avcodec_alloc_context3(c);
    if (avcodec_open2(cc, c, nullptr) < 0) { avcodec_free_context(&cc); return -1; }
    AVPacket* p = av_packet_alloc(); AVFrame* f = av_frame_alloc();
    p->data = reinterpret_cast<uint8_t*>(const_cast<char*>(jpeg.constData())); p->size = int(jpeg.size());
    int mean = -1;
    if (avcodec_send_packet(cc, p) >= 0 && avcodec_receive_frame(cc, f) >= 0) {
        qint64 sum = 0; const int x0 = f->width / 4, y0 = f->height / 4, x1 = 3 * f->width / 4, y1 = 3 * f->height / 4;
        for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) sum += f->data[0][y * f->linesize[0] + x];
        mean = int(sum / qMax(1, (x1 - x0) * (y1 - y0)));
        if (w) *w = f->width; if (h) *h = f->height;
    }
    p->data = nullptr; p->size = 0;
    av_packet_free(&p); av_frame_free(&f); avcodec_free_context(&cc);
    return mean;
}

void TestMediaPreview::initTestCase() {
    QVERIFY(m_dir.isValid());
    QString ff = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (!QFileInfo::exists(ff)) ff = QStandardPaths::findExecutable("ffmpeg");
    if (ff.isEmpty()) QSKIP("ffmpeg not found");
    m_clip = m_dir.filePath("grey.mp4");
    QProcess pr;
    pr.start(ff, {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=320x240:r=25:d=10,format=yuv420p,geq=lum='16+mod(N*7\\,200)':cb=128:cr=128",
                  "-c:v", "libx264", "-preset", "veryfast", "-qp", "4", "-g", "50", "-bf", "3", "-pix_fmt", "yuv420p", m_clip});
    QVERIFY(pr.waitForFinished(60000));
    QVERIFY2(pr.exitCode() == 0 && QFileInfo(m_clip).size() > 1000, pr.readAllStandardError().constData());
    m_mp.reset(new MediaPreview(m_dir.filePath("sb")));
}

void TestMediaPreview::infoReportsRateAndFrames() {
    const MediaPreview::Info in = m_mp->info(m_clip);
    QVERIFY2(in.ok, qPrintable(in.error));
    QCOMPARE(in.fpsNum, 25); QCOMPARE(in.fpsDen, 1);
    QCOMPARE(in.frames, qint64(250));
    QVERIFY(qAbs(in.durationSec - 10.0) < 0.05);
    QCOMPARE(in.width, 320); QCOMPARE(in.height, 240);
    QVERIFY(!in.dropFrame); QCOMPARE(in.tcBase, 25);
}

void TestMediaPreview::framesAreExactAcrossGopsAndBackwardSteps() {
    // forward within a GOP, across GOP boundaries (49/50/51), a jump, then steps backwards (keyframe seeks)
    const qint64 order[] = {0, 1, 2, 49, 50, 51, 123, 124, 249, 200, 199, 150, 149, 100, 3};
    for (qint64 n : order) {
        const MediaPreview::Frame f = m_mp->frameJpeg(m_clip, 0, n, 160);
        QVERIFY2(!f.jpeg.isEmpty(), qPrintable(f.error));
        QCOMPARE(f.frame, n);
        const int got = meanLuma(f.jpeg), want = expectedFull(n);
        QVERIFY2(qAbs(got - want) <= 3, qPrintable(QString("frame %1: luma %2, expected %3").arg(n).arg(got).arg(want)));
    }
    // past the end clamps to the last frame
    const MediaPreview::Frame e = m_mp->frameJpeg(m_clip, 0, 100000, 160);
    QCOMPARE(e.frame, qint64(249));
}

void TestMediaPreview::timeSnapsToTheFrameOnScreen() {
    QCOMPARE(m_mp->frameJpeg(m_clip, 4.000, -1, 160).frame, qint64(100));
    QCOMPARE(m_mp->frameJpeg(m_clip, 4.039, -1, 160).frame, qint64(100));   // still frame 100 on screen
    QCOMPARE(m_mp->frameJpeg(m_clip, 4.041, -1, 160).frame, qint64(101));
    const MediaPreview::Frame f = m_mp->frameJpeg(m_clip, 7.5, -1, 160);
    QCOMPARE(f.frame, qint64(187)); QVERIFY(qAbs(f.timeSec - 7.48) < 1e-6);
}

void TestMediaPreview::sameFrameGivesSameBytes() {
    const QByteArray a = m_mp->frameJpeg(m_clip, 0, 77, 200).jpeg;
    m_mp->frameJpeg(m_clip, 0, 5, 200);                                     // move the decoder elsewhere
    const QByteArray b = m_mp->frameJpeg(m_clip, 0, 77, 200).jpeg;
    QVERIFY(!a.isEmpty()); QCOMPARE(a, b);
    int w = 0, h = 0; meanLuma(a, &w, &h);
    QCOMPARE(w, 200); QCOMPARE(h, 150);                                     // 4:3 kept, even size
}

void TestMediaPreview::storyboardSprite() {
    QString err;
    const QJsonObject o = m_mp->storyboard(m_clip, "grey_20_96", 20, 96, &err);
    QVERIFY2(!o.isEmpty(), qPrintable(err));
    QCOMPARE(o.value("n").toInt(), 20); QCOMPARE(o.value("cols").toInt(), 10); QCOMPARE(o.value("rows").toInt(), 2);
    QCOMPARE(o.value("tileW").toInt(), 96); QCOMPARE(o.value("tileH").toInt(), 72);
    QCOMPARE(o.value("times").toArray().size(), 20);
    QCOMPARE(o.value("placed").toInt(), 20);
    QFile f(QDir(m_mp->cacheDir()).filePath("grey_20_96.jpg"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    int w = 0, h = 0; QVERIFY(meanLuma(f.readAll(), &w, &h) >= 0);
    QCOMPARE(w, 960); QCOMPARE(h, 144);
    // second call comes from the cache (same object back, no regeneration)
    const QJsonObject o2 = m_mp->storyboard(m_clip, "grey_20_96", 20, 96, &err);
    QCOMPARE(o2.value("bytes"), o.value("bytes"));
}

void TestMediaPreview::dropFrameTimecode() {
    QCOMPARE(MediaPreview::timecode(0, 30000, 1001), QString("00:00:00;00"));
    QCOMPARE(MediaPreview::timecode(1799, 30000, 1001), QString("00:00:59;29"));
    QCOMPARE(MediaPreview::timecode(1800, 30000, 1001), QString("00:01:00;02"));   // ;00 and ;01 are skipped
    QCOMPARE(MediaPreview::timecode(17982, 30000, 1001), QString("00:10:00;00"));  // every tenth minute keeps them
    QCOMPARE(MediaPreview::timecode(107892, 30000, 1001), QString("01:00:00;00"));
    QCOMPARE(MediaPreview::timecode(3600, 60000, 1001), QString("00:01:00;04"));
    QCOMPARE(MediaPreview::timecode(90000, 25, 1), QString("01:00:00:00"));
    QCOMPARE(MediaPreview::timecode(24 * 61 + 5, 24000, 1001), QString("00:01:01:05"));   // 23.976 is not drop-frame
}

QTEST_GUILESS_MAIN(TestMediaPreview)
#include "test_mediapreview.moc"
