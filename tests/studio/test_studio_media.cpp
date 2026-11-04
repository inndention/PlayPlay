// ST-17 media utilities (docs/plans/starmedia-studio.md §8): libav probe, frame access, scene cuts, still scoring and
// selection (port of StarMedia src/studio/stills.ts + scrub.ts + src/ads/ingest.ts), 16 kHz mono WAV for transcription
// split into <= 5 min pieces (src/payload.ts splitWav / src/limits.ts sttFor), and QImage operations (payload.ts
// shrinkImage, scrub.ts contactSheet) with the libav fallback for JPEG.
//
// The pure parts are table tests ported from starmedia test/stills.ts and test/payload.ts. The libav parts run on
// synthetic media made in a temp dir with the staged ffmpeg.exe of the same FFmpeg build (build-seg/ffmpeg.exe):
// clean cuts, a blurred / dark / faded half per scene for still scoring, a 650 s AAC file for the audio split, a
// transport stream whose sound starts 0.5 s late, and playlists that must be refused. Hermetic: one QTemporaryDir,
// no network, no live process; the only child process is our own ffmpeg.exe.
#include <QtTest>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QThreadPool>

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <set>

#include "media/audio.h"
#include "media/avopen.h"
#include "media/frames.h"
#include "media/imageops.h"
#include "media/probe.h"
#include "media/stills.h"

extern "C" {
#include <libavutil/log.h>
}

using namespace studio::media;

namespace {

// ---------------------------------------------------------------- synthetic thumbnails (test/stills.ts helpers)

constexpr int W = 16, H = 9;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

uchar u8(int v) { return uchar(unsigned(v) & 0xFFu); }   // JS Uint8Array assignment (ToUint8: modulo 256)

QByteArray thumbRgb(const std::function<std::array<int, 3>(int, int)>& f, int w = W, int h = H) {
    QByteArray out(qsizetype(w) * h * 3, '\0');
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::array<int, 3> v = f(x, y);
            const qsizetype i = (qsizetype(y) * w + x) * 3;
            out[i] = char(u8(v[0])); out[i + 1] = char(u8(v[1])); out[i + 2] = char(u8(v[2]));
        }
    return out;
}
QByteArray thumbGrey(const std::function<int(int, int)>& f, int w = W, int h = H) {
    return thumbRgb([&](int x, int y) { const int v = f(x, y); return std::array<int, 3>{v, v, v}; }, w, h);
}
QByteArray flat(int v) { return thumbGrey([v](int, int) { return v; }); }
const QByteArray& checker() { static const QByteArray c = thumbGrey([](int x, int y) { return (x + y) & 1 ? 255 : 0; }); return c; }
constexpr double kPi = 3.14159265358979323846;
int sineAt(int x, int y) { return int(std::floor(128 + 100 * std::sin(double(x) / W * kPi) * std::cos(double(y) / H * kPi) + 0.5)); }
const QByteArray& sine() { static const QByteArray s = thumbGrey(sineAt); return s; }
QByteArray blur(const QByteArray& rgb, int w = W, int h = H) {
    const uchar* p = reinterpret_cast<const uchar*>(rgb.constData());
    return thumbRgb([&](int x, int y) {
        int s[3] = {0, 0, 0}, n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x + dx, yy = y + dy;
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                const int i = (yy * w + xx) * 3;
                s[0] += p[i]; s[1] += p[i + 1]; s[2] += p[i + 2]; ++n;
            }
        return std::array<int, 3>{int(std::floor(double(s[0]) / n + 0.5)), int(std::floor(double(s[1]) / n + 0.5)), int(std::floor(double(s[2]) / n + 0.5))};
    }, w, h);
}
std::mt19937& rng() { static std::mt19937 g(7); return g; }
double rnd() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng()); }
QByteArray noise(int amount) { return thumbGrey([amount](int, int) { return int(std::floor(128 + (rnd() - 0.5) * 2 * amount + 0.5)); }); }

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }
bool sortedStrict(const std::vector<double>& xs) { for (size_t i = 1; i < xs.size(); ++i) if (!(xs[i] > xs[i - 1])) return false; return true; }

// ---------------------------------------------------------------- WAV helpers (test/payload.ts)

/// 16 kHz mono 16-bit WAV of `seconds`, samples = index % 32000 (so pieces can be checked for continuity).
QByteArray wav16(double seconds) {
    const qint64 n = std::llround(seconds * 16000);
    QByteArray b = wavHeader(n * 2, 16000, 1, 16);
    b.resize(44 + n * 2);
    for (qint64 i = 0; i < n; ++i) {
        const qint16 v = qint16(i % 32000);
        b[44 + i * 2] = char(v & 0xFF);
        b[44 + i * 2 + 1] = char((v >> 8) & 0xFF);
    }
    return b;
}
qint16 sampleAt(const QByteArray& wav, qint64 index) {
    const uchar* p = reinterpret_cast<const uchar*>(wav.constData()) + 44 + index * 2;
    return qint16(p[0] | (p[1] << 8));
}

// ---------------------------------------------------------------- image helpers

QColor meanColor(const QImage& img, const QRect& r) {
    const QImage c = img.convertToFormat(QImage::Format_RGB32);
    qint64 s[3] = {0, 0, 0}, n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QRgb p = c.pixel(x, y);
            s[0] += qRed(p); s[1] += qGreen(p); s[2] += qBlue(p); ++n;
        }
    return n ? QColor(int(s[0] / n), int(s[1] / n), int(s[2] / n)) : QColor();
}
bool closeColor(const QColor& a, const QColor& b, int tol) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol;
}
std::array<double, 3> meanRgb(const QByteArray& rgb) {
    std::array<double, 3> m{0, 0, 0};
    const uchar* p = reinterpret_cast<const uchar*>(rgb.constData());
    const qsizetype n = rgb.size() / 3;
    for (qsizetype i = 0; i < n; ++i) for (int c = 0; c < 3; ++c) m[size_t(c)] += p[i * 3 + c];
    for (double& v : m) v /= double(std::max<qsizetype>(1, n));
    return m;
}
/// The JPEG with an APP1 Exif segment right after SOI: little-endian TIFF header, IFD0 with the one entry
/// Orientation (0x0112, SHORT, 1 value), as a phone camera writes it.
QByteArray withExifOrientation(const QByteArray& jpeg, int orientation) {
    QByteArray tiff("II*\0\x08\0\0\0", 8);
    auto le16 = [&](int v) { tiff.append(char(v & 0xFF)); tiff.append(char((v >> 8) & 0xFF)); };
    auto le32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) tiff.append(char((v >> (8 * i)) & 0xFF)); };
    le16(1);                                                      // one entry
    le16(0x0112); le16(3); le32(1); le16(orientation); le16(0);   // tag, SHORT, count 1, value (padded to 4 bytes)
    le32(0);                                                      // no next IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    const int len = int(payload.size()) + 2;
    QByteArray seg("\xFF\xE1", 2);
    seg.append(char((len >> 8) & 0xFF));
    seg.append(char(len & 0xFF));
    return jpeg.left(2) + seg + payload + jpeg.mid(2);
}

QImage noiseImage(int w, int h, unsigned seed) {
    QImage img(w, h, QImage::Format_RGB32);
    std::mt19937 g(seed);
    for (int y = 0; y < h; ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) line[x] = qRgb(int(g() & 255), int(g() & 255), int(g() & 255));
    }
    return img;
}

} // namespace

class TestStudioMedia : public QObject {
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_ffmpeg;
    bool m_media = false;
    QString m_mediaError;

    QString path(const QString& name) const { return m_tmp.filePath(name); }

    bool runFfmpeg(const QStringList& args, QString* err) {
        QProcess p;
        p.setProgram(m_ffmpeg);
        p.setArguments(QStringList{QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-nostdin"), QStringLiteral("-y")} + args);
        p.setWorkingDirectory(m_tmp.path());
        p.setProcessChannelMode(QProcess::MergedChannels);
        p.start();
        if (!p.waitForStarted(15000)) { *err = QStringLiteral("ffmpeg did not start"); return false; }
        if (!p.waitForFinished(120000)) { p.kill(); p.waitForFinished(5000); *err = QStringLiteral("ffmpeg timed out"); return false; }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            *err = QStringLiteral("ffmpeg exit %1: %2").arg(p.exitCode()).arg(QString::fromLocal8Bit(p.readAll()).left(600));
            return false;
        }
        return true;
    }

    bool makeMedia(QString* err) {
        const QString sz = QStringLiteral("s=320x180:r=25");
        // cuts.mp4: three static scenes (4 s light grid, 5 s dark grid, 3 s yellow grid): hard cuts at 4.0 and 9.0
        if (!runFfmpeg({"-f", "lavfi", "-i", "color=c=0xE0E0E0:" + sz + ":d=4,drawgrid=w=32:h=32:t=2:c=black",
                        "-f", "lavfi", "-i", "color=c=0x101010:" + sz + ":d=5,drawgrid=w=32:h=32:t=2:c=white",
                        "-f", "lavfi", "-i", "color=c=0xF0C020:" + sz + ":d=3,drawgrid=w=32:h=32:t=2:c=0x303030",
                        "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1:a=0,format=yuv420p[v]",
                        "-map", "[v]", "-c:v", "libx264", "-preset", "veryfast", "-g", "25", "cuts.mp4"}, err)) return false;
        // stills.mp4: 3 s scenes, each with a bad first half: blurred testsrc2, crushed-dark bars, testsrc fading in
        if (!runFfmpeg({"-f", "lavfi", "-i", "testsrc2=" + sz + ":d=3", "-f", "lavfi", "-i", "smptebars=" + sz + ":d=3",
                        "-f", "lavfi", "-i", "testsrc=" + sz + ":d=3", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=9",
                        "-filter_complex",
                        "[0:v]format=yuv420p,boxblur=8:enable='lt(t,1.5)',setsar=1[a];"
                        "[1:v]format=yuv420p,lutyuv=y=val*0.15:enable='lt(t,1.5)',setsar=1[b];"
                        "[2:v]format=yuv420p,fade=t=in:st=0:d=1.5,setsar=1[c];"
                        "[a][b][c]concat=n=3:v=1:a=0[v]",
                        "-map", "[v]", "-map", "3:a", "-ac", "2", "-c:v", "libx264", "-preset", "veryfast", "-g", "25",
                        "-c:a", "aac", "-b:a", "96k", "stills.mp4"}, err)) return false;
        if (!runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=44100:duration=7", "-ac", "2", "-c:a", "pcm_s16le", "tone.wav"}, err)) return false;
        // 10 min 50 s: three pieces of 300 / 300 / 50 s
        if (!runFfmpeg({"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=650", "-ac", "2", "-c:a", "aac", "-b:a", "32k", "long.m4a"}, err)) return false;
        // sound starts 0.5 s after the picture
        if (!runFfmpeg({"-f", "lavfi", "-i", "color=c=gray:s=160x90:r=25:d=3", "-itsoffset", "0.5", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2",
                        "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "veryfast", "-bf", "0", "-c:a", "mp2", "-f", "mpegts", "late.ts"}, err)) return false;
        // rot.mp4: a 320x180 red picture with a blue band on its left, flagged (display matrix, stream copy) to be shown
        // turned 90 degrees counter-clockwise, as a phone stores a portrait clip; rot1.png is ffmpeg's own autorotated
        // frame at 1 s, the oracle for which way it turns
        if (!runFfmpeg({"-f", "lavfi", "-i", "color=c=red:" + sz + ":d=2,drawbox=x=0:y=0:w=80:h=180:c=blue:t=fill",
                        "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-g", "25", "upright.mp4"}, err)) return false;
        if (!runFfmpeg({"-display_rotation:v:0", "90", "-i", "upright.mp4", "-c", "copy", "rot.mp4"}, err)) return false;
        if (!runFfmpeg({"-ss", "1", "-i", "rot.mp4", "-frames:v", "1", "rot1.png"}, err)) return false;
        auto write = [&](const QString& name, const QByteArray& data) {
            QFile f(path(name));
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        return write(QStringLiteral("list.ffconcat"), "ffconcat version 1.0\nfile 'stills.mp4'\n")
            && write(QStringLiteral("list.m3u8"), "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXTINF:9.0,\nstills.mp4\n#EXT-X-ENDLIST\n")
            && write(QStringLiteral("notes.txt"), "this is not a media file, just some words that go on for a while.\n");
    }

#define NEED_MEDIA() do { if (!m_media) QSKIP(qPrintable(QStringLiteral("no synthetic media: ") + m_mediaError)); } while (0)

private slots:
    void initTestCase() {
        av_log_set_level(AV_LOG_ERROR);
        QVERIFY(m_tmp.isValid());
        const QStringList candidates{
            QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe"),
            qEnvironmentVariable("PLAYPLAY_FFMPEG"),
            QStringLiteral("C:/SDKs/ffmpeg-9.0-win64-gpl-shared/bin/ffmpeg.exe"),
        };
        for (const QString& c : candidates) if (!c.isEmpty() && QFile::exists(c)) { m_ffmpeg = c; break; }
        if (m_ffmpeg.isEmpty()) { m_mediaError = QStringLiteral("ffmpeg.exe not found"); return; }
        QElapsedTimer t;
        t.start();
        m_media = makeMedia(&m_mediaError);
        qInfo("synthetic media %s in %lld ms (%s)", m_media ? "made" : "FAILED", qint64(t.elapsed()), qPrintable(m_ffmpeg));
        QVERIFY2(m_media, qPrintable(m_mediaError));
    }

    // ================================================================ stills.ts (pure)

    void options() {
        const ScrubOptions d = scrubOptions();
        QCOMPARE(d.fps, 2.0); QCOMPARE(d.keep, 24); QCOMPARE(d.maxSamples, 900); QCOMPARE(d.minGap, 0.75);
        QCOMPARE(d.minDistance, 0.06); QCOMPARE(d.thumbW, 80); QCOMPARE(d.thumbH, 45); QCOMPARE(d.maxSide, 1280); QCOMPARE(d.jpegQuality, 0.85);
        QCOMPARE(toJson(d), toJson(ScrubOptions()));
        const ScrubOptions o = scrubOptions(QJsonObject{{"fps", 100}, {"keep", 0}, {"maxSamples", 5}, {"minDistance", 3}, {"jpegQuality", "x"}, {"thumbW", -4}});
        QCOMPARE(o.fps, 8.0); QCOMPARE(o.keep, 1); QCOMPARE(o.maxSamples, 20); QCOMPARE(o.minDistance, 1.0); QCOMPARE(o.jpegQuality, 0.85); QCOMPARE(o.thumbW, 16);
        QCOMPARE(scrubOptions(QJsonObject{{"keep", "7.6"}, {"fps", QJsonValue::Null}}).keep, 8);    // numeric text, rounded
        QCOMPARE(scrubOptions(QJsonObject{{"fps", QJsonValue::Null}}).fps, 0.25);                // Number(null) = 0, clamped
        ScrubOptions bad; bad.jpegQuality = kNaN; bad.minGap = -3; bad.maxSide = 99999;
        const ScrubOptions c = clampOptions(bad);
        QCOMPARE(c.jpegQuality, 0.85); QCOMPARE(c.minGap, 0.0); QCOMPARE(c.maxSide, 4096);
    }

    void samplePlanRules() {
        {
            const SamplePlan p = samplePlan(10, 2, 900);
            QVERIFY(p.times.size() >= 20 && p.times.size() <= 22);
            QVERIFY(near(p.step, 0.5, 1e-6));
            QVERIFY(near(p.times.front(), 0.05) && near(p.times.back(), 9.95));
            QVERIFY(sortedStrict(p.times));
            for (double t : p.times) QVERIFY(t >= 0 && t < 10);
        }
        {
            const SamplePlan p = samplePlan(3600, 2, 900);
            QVERIFY(p.times.size() <= 900 && p.times.size() >= 880);
            QVERIFY(p.step > 3.9);
            QVERIFY(sortedStrict(p.times));
        }
        {
            const SamplePlan p = samplePlan(10, 1, 900, {{0, 4}, {4, 10}});
            auto has = [&](double t) { return std::any_of(p.times.begin(), p.times.end(), [&](double x) { return near(x, t, 1e-3); }); };
            QVERIFY(has(0.15) && has(4.15) && has(2) && has(7));
            std::set<qint64> ms;
            for (double t : p.times) ms.insert(std::llround(t * 1000));
            QCOMPARE(ms.size(), p.times.size());
        }
        {
            const SamplePlan p = samplePlan(10, 2, 30, {{-5, 2}, {8, 40}, {kNaN, 3}});
            QVERIFY(p.times.size() <= 30);
            for (double t : p.times) QVERIFY(t >= 0 && t < 10);
        }
        for (double d : {0.0, -3.0, kNaN, std::numeric_limits<double>::infinity()}) QVERIFY(samplePlan(d, 2, 900).times.empty());
        {
            const SamplePlan p = samplePlan(0.033, 2, 900);
            QCOMPARE(p.times.size(), size_t(1));
            QVERIFY(p.times[0] >= 0 && p.times[0] < 0.033);
            QCOMPARE(p.step, 0.0);
        }
        {
            const SamplePlan p = samplePlan(100, 8, 20);
            QVERIFY(p.times.size() <= 20 && p.times.size() >= 18);
            QVERIFY(near(p.times.front(), 0.05) && near(p.times.back(), 99.95));
        }
        const std::vector<SceneSpan> two{{0, 4}, {4, 10}};
        QCOMPARE(sceneIndexAt(5, two), 1);
        QCOMPARE(sceneIndexAt(3.99, two), 0);
        QCOMPARE(sceneIndexAt(12, two), 1);
        QCOMPARE(sceneIndexAt(-1, {{0, 4}}), 0);
        QCOMPARE(sceneIndexAt(1, {}), -1);
        QCOMPARE(sceneIndexAt(kNaN, {{0, 4}}), -1);
    }

    void frameMeasures() {
        const QByteArray four("\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\xC8\xC8\xC8", 12);
        const QByteArray g4 = greyOf(four, 4, 1);
        QCOMPARE(g4.size(), qsizetype(4));
        QCOMPARE(int(uchar(g4[0])), 76); QCOMPARE(int(uchar(g4[1])), 150); QCOMPARE(int(uchar(g4[2])), 29); QCOMPARE(int(uchar(g4[3])), 200);
        auto g = [](const QByteArray& rgb) { return greyOf(rgb, W, H); };
        const QByteArray nz = noise(60);
        const double sCheck = sharpness(g(checker()), W, H), sSine = sharpness(g(sine()), W, H), sFlat = sharpness(g(flat(128)), W, H);
        const double sBlur = sharpness(g(blur(checker())), W, H), sNoise = sharpness(g(nz), W, H), sNoiseBlur = sharpness(g(blur(nz)), W, H);
        QVERIFY2(sCheck > sSine && sSine > sFlat && sFlat == 0, qPrintable(QStringLiteral("%1 %2 %3").arg(sCheck).arg(sSine).arg(sFlat)));
        QVERIFY(sBlur < sCheck && sNoiseBlur < sNoise);
        QVERIFY(sCheck >= 0.99 && sCheck <= 1 && sSine > 0 && sSine < 1);
        QCOMPARE(sharpness(QByteArray(4, '\0'), 2, 2), 0.0);
        QCOMPARE(sharpness(QByteArray(), 0, 0), 0.0);
        QCOMPARE(sharpness(g(checker()), 100, 100), 0.0);

        const Brightness black = brightness(g(flat(0))), white = brightness(g(flat(255))), mid = brightness(g(flat(128)));
        const Brightness half = brightness(g(thumbGrey([](int x, int) { return x < W / 2 ? 0 : 128; })));
        QVERIFY(black.mean == 0 && black.clipped == 1 && white.mean == 1 && white.clipped == 1);
        QVERIFY(near(mid.mean, 128.0 / 255) && mid.clipped == 0 && near(half.clipped, 0.5) && near(half.mean, 64.0 / 255));
        QCOMPARE(exposureScore(mid), 1.0);
        QCOMPARE(exposureScore(black), 0.0);
        QCOMPARE(exposureScore(white), 0.0);
        QVERIFY(exposureScore(half) < exposureScore(mid) && exposureScore(half) > 0);
        QVERIFY(exposureScore({0.35, 0}) > 0.8 && exposureScore({0.08, 0}) < 0.3 && exposureScore({0.5, 0.9}) < 0.15);
        QVERIFY(!std::isnan(exposureScore(brightness(QByteArray()))));
        QCOMPARE(exposureScore({kNaN, 0}), 0.0);
    }

    void distanceAndStability() {
        QCOMPARE(distance(sine(), sine()), 0.0);
        QVERIFY(near(distance(sine(), checker()), distance(checker(), sine())));
        QCOMPARE(distance(flat(0), flat(255)), 1.0);
        QVERIFY(distance(flat(0), flat(128)) > 0.5 && distance(flat(0), flat(128)) < 0.51);
        QCOMPARE(distance(sine(), QByteArray(9, '\0')), 1.0);
        QCOMPARE(distance(QByteArray(), QByteArray()), 0.0);
        {
            const QByteArray A = flat(40), B = flat(220), A2 = flat(44);
            const std::vector<QByteArray> sigs{A, A2, A, B, B};
            QVERIFY(stability(sigs, 2) < 0.4 && stability(sigs, 3) < 0.4 && stability(sigs, 1) > 0.97 && stability(sigs, 4) == 1);
            QVERIFY(stability(sigs, 0) > 0.97);
            QCOMPARE(stability({A}, 0), 1.0);
            QCOMPARE(stability({}, 0), 0.0);
        }
        {
            const QByteArray n30a = noise(30), n30b = noise(30);
            const std::vector<std::pair<QByteArray, QByteArray>> pairs{
                {sine(), sine()}, {sine(), checker()}, {flat(0), flat(255)}, {flat(100), flat(104)}, {n30a, n30b},
                {sine(), QByteArray(9, '\0')}, {QByteArray(), QByteArray()}};
            for (const auto& [a, b] : pairs)
                for (double lim : {0.0, 0.01, 0.0157, 0.06, 0.5, 1.0, 1.01}) QCOMPARE(closerThan(a, b, lim), distance(a, b) < lim);
        }
        QVERIFY(near(scoreSample(1, 0, 0), 0.5));
        QVERIFY(near(scoreSample(0, 1, 1), 0.5));
        QCOMPARE(scoreSample(9, kNaN, -1), 0.5);
        {
            const Candidate c = makeCandidate(4.5, sine(), W, H, {{0, 4}, {4, 10}}, sine(), sine());
            const Candidate onCut = makeCandidate(4.5, sine(), W, H, {}, flat(0), sine());
            QCOMPARE(c.sceneIndex, 1);
            QVERIFY(c.score > 0 && c.score <= 1 && c.sig == sine() && c.sharpness > 0);
            QVERIFY(near(c.brightness, brightness(greyOf(sine(), W, H)).mean));
            QCOMPARE(onCut.sceneIndex, -1);
            QVERIFY(onCut.score < c.score);
        }
    }

    void selection() {
        // three scenes of 10 samples at 0.5 s: own base look each, a little motion, one blurry (low-score) frame and one
        // exact repeat of a neighbour (near-duplicate) per scene
        std::vector<Candidate> cands;
        for (int s = 0; s < 3; ++s) {
            const int base = std::array<int, 3>{30, 128, 220}[size_t(s)];
            for (int k = 0; k < 10; ++k) {
                Candidate c;
                c.t = s * 5 + k * 0.5 + 0.05;
                c.sceneIndex = s;
                c.sig = thumbGrey([&](int x, int y) { return (((x + k) & 3) == 0 ? base + 60 : base - 20) + ((y & 1) ? 10 : 0); });
                c.score = 0.4 + 0.02 * k + s * 0.05;
                if (k == 4) { c.sig = flat(base); c.score = 0.05; }
                if (k == 7) c.sig = cands.back().sig;
                c.sharpness = c.score; c.brightness = 0.5;
                cands.push_back(c);
            }
        }
        const Selection sel = selectStills(cands, 8, 0.75, 0.06);
        std::vector<double> ts;
        for (const Candidate& c : sel.kept) ts.push_back(c.t);
        QCOMPARE(sel.kept.size(), size_t(8));
        QVERIFY(sortedStrict(ts));
        for (int s = 0; s < 3; ++s) QVERIFY(std::any_of(sel.kept.begin(), sel.kept.end(), [s](const Candidate& c) { return c.sceneIndex == s; }));
        for (size_t i = 0; i < sel.kept.size(); ++i)
            for (size_t j = 0; j < sel.kept.size(); ++j)
                if (i != j) { QVERIFY(std::abs(sel.kept[i].t - sel.kept[j].t) >= 0.75); QVERIFY(distance(sel.kept[i].sig, sel.kept[j].sig) >= 0.06); }
        QVERIFY(std::none_of(sel.kept.begin(), sel.kept.end(), [](const Candidate& c) { return c.score < 0.12; }));
        QCOMPARE(sel.dropped.lowQuality, 3);
        QVERIFY(sel.dropped.nearDuplicate >= 3);
        QVERIFY(std::count_if(sel.kept.begin(), sel.kept.end(), [](const Candidate& c) { return c.sceneIndex == 2; }) >= 3);
        QVERIFY(std::any_of(sel.kept.begin(), sel.kept.end(), [](const Candidate& c) { return near(c.score, 0.4 + 0.18 + 0.1); }));
        {
            auto key = [](const Selection& s) {
                QString k;
                for (const Candidate& c : s.kept) k += QString::number(c.t, 'f', 3) + QLatin1Char(',');
                return k + QStringLiteral("|%1/%2/%3").arg(s.dropped.nearDuplicate).arg(s.dropped.lowQuality).arg(s.dropped.tooClose);
            };
            std::vector<Candidate> shuffled = cands, reversed(cands.rbegin(), cands.rend());
            std::shuffle(shuffled.begin(), shuffled.end(), rng());
            QCOMPARE(key(selectStills(shuffled, 8, 0.75, 0.06)), key(sel));
            QCOMPARE(key(selectStills(reversed, 8, 0.75, 0.06)), key(sel));
            QCOMPARE(cands[0].t, 0.05);
            QCOMPARE(cands.size(), size_t(30));
        }
        {
            const Selection s = selectStills(cands, 24, 0.75, 0.06);
            QVERIFY(s.kept.size() > 8 && s.kept.size() <= 24);
            for (size_t i = 0; i < s.kept.size(); ++i)
                for (size_t j = 0; j < s.kept.size(); ++j) if (i != j) QVERIFY(std::abs(s.kept[i].t - s.kept[j].t) >= 0.75);
        }
        {
            std::vector<Candidate> dim = cands;
            for (Candidate& c : dim) if (c.sceneIndex == 1) c.score = 0.03;
            const Selection s = selectStills(dim, 8, 0.75, 0.06);
            QCOMPARE(std::count_if(s.kept.begin(), s.kept.end(), [](const Candidate& c) { return c.sceneIndex == 1; }), 1);
        }
        {
            const Selection s = selectStills({}, 8, 0.75, 0.06);
            QVERIFY(s.kept.empty() && s.dropped.lowQuality == 0);
        }
        {
            const Selection s = selectStills(cands, 1, 0, 0);
            double best = 0;
            for (const Candidate& c : cands) best = std::max(best, c.score);
            QCOMPARE(s.kept.size(), size_t(1));
            QCOMPARE(s.kept[0].score, best);
        }
        {
            std::vector<Candidate> same;
            for (int i = 0; i < 12; ++i) { Candidate c; c.t = i; c.score = 0.5; c.sig = sine(); same.push_back(c); }
            const Selection s = selectStills(same, 6, 0.5, 0.06);
            QCOMPARE(s.kept.size(), size_t(1));
            QCOMPARE(s.dropped.nearDuplicate, 11);
        }
        {
            // a false cut: scene 1's sharp frame repeats scene 0's; its only other sample is a black fade
            const QByteArray A2 = thumbGrey([](int x, int y) { return std::min(255, sineAt(x, y) + 1); });
            Candidate a; a.t = 1; a.sceneIndex = 0; a.score = 0.9; a.sig = sine();
            Candidate b; b.t = 4.2; b.sceneIndex = 1; b.score = 0.85; b.sig = A2;
            Candidate c; c.t = 5.9; c.sceneIndex = 1; c.score = 0.03; c.sig = flat(0);
            const Selection s = selectStills({a, b, c}, 24, 0.75, 0.06);
            QCOMPARE(s.kept.size(), size_t(2));
            QCOMPARE(s.kept[0].t, 1.0);
            QCOMPARE(s.kept[1].t, 4.2);
            QCOMPARE(s.dropped.lowQuality, 1);
        }
        {
            Candidate nan = cands[0]; nan.score = kNaN; nan.t = 100;
            const Selection s = selectStills({nan, cands[1]}, 1, 0, 0);
            QCOMPARE(s.kept.size(), size_t(1));
            QCOMPARE(s.kept[0].t, cands[1].t);
        }
    }

    void scenesFromThumbs() {
        // three shots of 4 s sampled every 0.5 s, a little motion inside each, hard cuts at 4 and 8
        std::vector<double> times;
        std::vector<QByteArray> sigs;
        for (double t = 0.05; t < 12; t += 0.5) {
            const int s = t < 4 ? 0 : t < 8 ? 1 : 2, k = int(std::floor(t * 2 + 0.5));
            const int base = std::array<int, 3>{40, 128, 220}[size_t(s)];
            times.push_back(t);
            sigs.push_back(thumbGrey([&](int x, int y) { return (((x + k) & 3) == 0 ? base + 20 : base) + ((y & 1) ? 4 : 0); }));
        }
        const std::vector<Scene> sc = scenesFromSignatures(times, sigs, 12);
        QCOMPARE(sc.size(), size_t(3));
        QCOMPARE(sc[0].start, 0.0);
        QCOMPARE(sc[2].end, 12.0);
        QVERIFY(near(sc[1].start, 4.05) && near(sc[2].start, 8.05));
        for (const Scene& s : sc) QVERIFY(near(s.midpoint, (s.start + s.end) / 2));
        QCOMPARE(scenesFromSignatures(times, std::vector<QByteArray>(times.size(), sine()), 12).size(), size_t(1));
        QCOMPARE(scenesFromSignatures({}, {}, 12).size(), size_t(0));
        QCOMPARE(scenesFromSignatures(times, sigs, kNaN).size(), size_t(3));
        // 200 samples over 60 s with a hard cut every 20 samples: ten shots, more than the cap of eight
        std::vector<double> many;
        std::vector<QByteArray> manySigs;
        for (int k = 0; k < 200; ++k) {
            const int shot = k / 20;
            many.push_back(k * 0.3);
            manySigs.push_back(thumbGrey([&](int x, int) { return (shot % 2 ? 20 : 230) + ((x + k) & 1); }));
        }
        const std::vector<Scene> capped = scenesFromSignatures(many, manySigs, 60, 0.9, 8);
        QCOMPARE(capped.size(), size_t(8));
        QCOMPARE(capped.front().start, 0.0);
        QCOMPARE(capped.back().end, 60.0);
        for (size_t i = 1; i < capped.size(); ++i) QVERIFY(near(capped[i].start, capped[i - 1].end));
        QCOMPARE(scenesFromSignatures({0, 0.2, 0.4}, {flat(0), flat(255), flat(0)}, 1).size(), size_t(1));
    }

    void layoutBatchesAndTimes() {
        const SheetLayout L1 = contactSheetLayout(1, 100, 50), L5 = contactSheetLayout(5, 100, 50), L12 = contactSheetLayout(12, 100, 50), L13 = contactSheetLayout(13, 100, 50, 4);
        QVERIFY(L1.cols == 1 && L1.rows == 1 && L1.width == 100 && L1.height == 50 && L1.cells.size() == 1 && L1.cells[0].x == 0);
        QVERIFY(L5.cols == 3 && L5.rows == 2 && L5.width == 300 && L5.height == 100 && L5.cells[4].x == 100 && L5.cells[4].y == 50);
        QVERIFY(L12.cols == 4 && L12.rows == 3 && L12.cells.size() == 12 && L12.cells[11].x == 300 && L12.cells[11].y == 100);
        QVERIFY(L13.cols == 4 && L13.rows == 4 && L13.width == 4 * 100 + 5 * 4 && L13.height == 4 * 50 + 5 * 4 && L13.cells[0].x == 4 && L13.cells[5].x == 108 && L13.cells[5].y == 58);
        QCOMPARE(contactSheetLayout(30, 10, 10).cols, 4);
        QCOMPARE(contactSheetLayout(30, 10, 10, 0, 6).cols, 6);
        QVERIFY(contactSheetLayout(0, 10, 10).cells.empty());

        struct T { double t; };
        std::vector<T> ss;
        for (int i = 0; i < 13; ++i) ss.push_back({double(13 - i)});
        const std::function<double(const T&)> tOf = [](const T& x) { return x.t; };
        const auto b = mineBatches(ss, 12, tOf);
        QVERIFY(b.size() == 2 && b[0].size() == 7 && b[1].size() == 6 && b[0][0].t == 1 && b[1][5].t == 13);
        const auto b12 = mineBatches(std::vector<T>(ss.begin(), ss.begin() + 12), 12, tOf);
        QVERIFY(b12.size() == 1 && b12[0].size() == 12);
        const auto b3 = mineBatches(ss, 5, tOf);
        size_t total = 0;
        for (const auto& x : b3) { QVERIFY(x.size() <= 5); total += x.size(); }
        QVERIFY(b3.size() == 3 && total == 13);
        QVERIFY(mineBatches(std::vector<T>{}, 12, tOf).empty());
        QString order;
        for (const auto& batch : mineBatches(std::vector<QString>{"b", "a", "c"}, 2)) for (const QString& s : batch) order += s;
        QCOMPARE(order, QStringLiteral("bac"));

        QCOMPARE(fmtT(65.2), QStringLiteral("1:05.2"));
        QCOMPARE(fmtT(0), QStringLiteral("0:00.0"));
        QCOMPARE(fmtT(5.25), QStringLiteral("0:05.3"));
        QCOMPARE(fmtT(3723.4), QStringLiteral("1:02:03.4"));
        QCOMPARE(fmtT(kNaN), QStringLiteral("0:00.0"));
        QCOMPARE(fmtT(-4), QStringLiteral("0:00.0"));
        QCOMPARE(fmtT(59.99), QStringLiteral("1:00.0"));
    }

    // ================================================================ payload.ts / limits.ts (pure)

    void wavPieces() {
        const QByteArray w = wav16(11 * 60 + 7);
        const std::optional<WavInfo> info = wavInfo(w);
        QVERIFY(info);
        QVERIFY(info->sampleRate == 16000 && info->channels == 1 && info->bitsPerSample == 16 && info->dataOffset == 44);
        const std::vector<WavPiece> parts = splitWav(w, kSttPieceSeconds);
        QCOMPARE(parts.size(), size_t(3));
        QCOMPARE(parts[0].offset, 0.0); QCOMPARE(parts[1].offset, 300.0); QCOMPARE(parts[2].offset, 600.0);
        qint64 total = 0;
        for (const WavPiece& p : parts) {
            const std::optional<WavInfo> pi = wavInfo(p.bytes);
            QVERIFY(pi && pi->dataBytes == p.bytes.size() - 44);
            QVERIFY(pi->dataBytes <= qint64(kSttPieceSeconds * kSttBytesPerSecond));   // no piece over 5 minutes
            total += p.bytes.size() - 44;
        }
        QCOMPARE(wavInfo(parts[2].bytes)->dataBytes, qint64(67 * 16000) * 2);
        QCOMPARE(total, info->dataBytes);                                              // nothing lost or duplicated
        QCOMPARE(int(sampleAt(parts[0].bytes, 300 * 16000 - 1)), (300 * 16000 - 1) % 32000);
        QCOMPARE(int(sampleAt(parts[1].bytes, 0)), (300 * 16000) % 32000);            // the second starts where the first ended
        QCOMPARE(int(sampleAt(parts[2].bytes, 1)), (600 * 16000 + 1) % 32000);
        QVERIFY(parts[0].bytes.size() < 10 * 1024 * 1024);
        QCOMPARE(splitWav(QByteArray("\x01\x02\x03", 3), 10).size(), size_t(1));
        QCOMPARE(splitWav(wav16(4), 300).size(), size_t(1));                           // already fits

        // stereo 44.1 kHz: pieces on whole sample frames, offsets from bytes
        QByteArray st = wavHeader(176400, 44100, 2, 16);
        st.append(QByteArray(176400, '\x11'));
        const std::vector<WavPiece> sp = splitWav(st, 0.3);
        QCOMPARE(sp.size(), size_t(4));
        for (size_t i = 0; i < sp.size(); ++i) {
            QCOMPARE((sp[i].bytes.size() - 44) % 4, qsizetype(0));
            QVERIFY(near(sp[i].offset, 0.3 * double(i), 1e-9));
        }
        QCOMPARE(sp[3].bytes.size() - 44, qsizetype(176400 - 3 * 52920));

        // a LIST chunk with an odd length (pad byte) before the data
        QByteArray chunked = wavHeader(0, 16000, 1, 16).left(36);
        chunked += QByteArray("LIST\x03\x00\x00\x00" "abc\x00", 12);
        chunked += QByteArray("data\x08\x00\x00\x00", 8) + QByteArray(8, '\x05');
        const std::optional<WavInfo> ci = wavInfo(chunked);
        QVERIFY(ci && ci->dataOffset == 36 + 12 + 8 && ci->dataBytes == 8);
        QVERIFY(!wavInfo(QByteArray(60, 'x')));

        const SttLimits none = sttFor();
        QCOMPARE(none.pieceSeconds, 300.0); QCOMPARE(none.maxSingleBytes, kSttMaxSingle);
        QCOMPARE(sttFor(2).pieceSeconds, 114.0);
        QCOMPARE(sttFor(20).pieceSeconds, 300.0);
        const SttLimits oneMb = sttFor(0, 1);
        QCOMPARE(oneMb.pieceSeconds, 30.0);          // floor(29.49) under the 30 s floor
        QCOMPARE(oneMb.maxSingleBytes, qint64(996147));
    }

    // ================================================================ imageops

    void imageOps() {
        QCOMPARE(sniffMime(QByteArray("\x89PNG\r\n\x1a\n....", 12)), QStringLiteral("image/png"));
        QCOMPARE(sniffMime(QByteArray("\xFF\xD8\xFF\xE0", 4)), QStringLiteral("image/jpeg"));
        QCOMPARE(sniffMime(QByteArray("RIFF\x00\x00\x00\x00WEBPVP8 ", 16)), QStringLiteral("image/webp"));
        QCOMPARE(sniffMime(QByteArray("GIF89a")), QStringLiteral("image/gif"));
        QCOMPARE(sniffMime(QByteArray("hello world")), QString());

        // PNG round trip, Qt and libav decoders agree pixel for pixel
        QImage src(64, 48, QImage::Format_RGB32);
        for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) src.setPixel(x, y, qRgb(x * 4, y * 5, (x + y) * 2));
        const QByteArray png = encodePng(src);
        QVERIFY(!png.isEmpty());
        QCOMPARE(sniffMime(png), QStringLiteral("image/png"));
        QString err;
        const QImage viaQt = decodeImage(png, &err, DecodeLimits(), Codec::Qt);
        const QImage viaAv = decodeImage(png, &err, DecodeLimits(), Codec::Libav);
        QVERIFY2(!viaQt.isNull() && !viaAv.isNull(), qPrintable(err));
        QCOMPARE(viaAv.size(), src.size());
        bool same = true;
        for (int y = 0; y < 48 && same; ++y) for (int x = 0; x < 64 && same; ++x) same = (viaAv.pixel(x, y) & 0xFFFFFF) == (src.pixel(x, y) & 0xFFFFFF);
        QVERIFY2(same, "libav PNG decode differs from the source");

        // JPEG through Qt's plugin and through libav's MJPEG encoder; each decodes with the other path
        const QImage solid = [] { QImage i(200, 120, QImage::Format_RGB32); i.fill(qRgb(200, 60, 30)); return i; }();
        const QByteArray jq = encodeJpeg(solid, 0.85, &err, Codec::Qt);
        const QByteArray ja = encodeJpeg(solid, 0.85, &err, Codec::Libav);
        QVERIFY2(!jq.isEmpty() && !ja.isEmpty(), qPrintable(err));
        QCOMPARE(sniffMime(ja), QStringLiteral("image/jpeg"));
        const QImage da = decodeImage(ja, &err, DecodeLimits(), Codec::Qt), dq = decodeImage(jq, &err, DecodeLimits(), Codec::Libav);
        QVERIFY2(!da.isNull() && !dq.isNull(), qPrintable(err));
        QCOMPARE(da.size(), solid.size());
        QCOMPARE(dq.size(), solid.size());
        QVERIFY(closeColor(meanColor(da, da.rect()), QColor(200, 60, 30), 8));
        QVERIFY(closeColor(meanColor(dq, dq.rect()), QColor(200, 60, 30), 8));
        QVERIFY(encodeJpeg(QImage(), 0.8, &err).isEmpty());

        // limits and garbage
        QVERIFY(decodeImage(png, &err, DecodeLimits{qint64(1) << 30, 100}).isNull());
        QVERIFY2(err.contains(QLatin1String("pixels")), qPrintable(err));
        QVERIFY(decodeImage(png, &err, DecodeLimits{10, 50'000'000}).isNull());
        QVERIFY(decodeImage(QByteArray("definitely not an image at all"), &err).isNull());
        QVERIFY(!err.isEmpty());
        QVERIFY(decodeImage(QByteArray(), &err).isNull());
        // Qt's allocation limit is one static for the whole process: decoding never changes it, whatever the limits
        const int qtLimit = QImageReader::allocationLimit();
        QVERIFY(!decodeImage(png, &err, DecodeLimits{qint64(1) << 30, 1'000'000}, Codec::Qt).isNull());
        QVERIFY(decodeImage(png, &err, DecodeLimits{qint64(1) << 30, 100}, Codec::Qt).isNull());
        QCOMPARE(QImageReader::allocationLimit(), qtLimit);
        // the libav fallback decodes still images only: SVG (librsvg would render it) is refused, not decoded
        // (it starts with "<svg" so libav's probe does pick svg_pipe)
        const QByteArray svg("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\">"
                             "<rect width=\"64\" height=\"64\" fill=\"red\"/></svg>");
        QVERIFY(decodeImage(svg, &err).isNull());
        QVERIFY2(err.contains(QLatin1String("not an image (svg_pipe)")), qPrintable(err));
        // a header claiming 10000 x 10000 (100 MP > the 50 MP default) is refused by both paths before decoding
        QByteArray huge = png;
        const char dims[8] = {0, 0, 0x27, 0x10, 0, 0, 0x27, 0x10};   // IHDR width / height, big-endian 10000
        huge.replace(16, 8, QByteArray(dims, 8));
        quint32 crc = 0xFFFFFFFFu;                                   // the chunk CRC over "IHDR" + its 13 bytes
        for (int i = 12; i < 29; ++i) {
            crc ^= uchar(huge[i]);
            for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        crc ^= 0xFFFFFFFFu;
        for (int i = 0; i < 4; ++i) huge[29 + i] = char((crc >> (24 - 8 * i)) & 0xFF);
        QVERIFY(decodeImage(huge, &err, DecodeLimits(), Codec::Qt).isNull());
        QVERIFY2(err.contains(QLatin1String("pixels")), qPrintable(err));
        QVERIFY(decodeImage(huge, &err, DecodeLimits(), Codec::Libav).isNull());

        // fit / cover / flatten / thumbs / data URLs
        QImage wide(400, 200, QImage::Format_RGB32);
        wide.fill(Qt::red);
        { QPainter p(&wide); p.fillRect(0, 0, 100, 200, Qt::green); p.fillRect(300, 0, 100, 200, Qt::blue); }
        QCOMPARE(fitInside(wide, 100, 100).size(), QSize(100, 50));
        QCOMPARE(fitInside(wide, 0, 50).size(), QSize(100, 50));
        QCOMPARE(fitInside(QImage(50, 20, QImage::Format_RGB32), 100, 100).size(), QSize(50, 20));   // never upscaled
        const QImage cov = coverFit(wide, 100, 100);
        QCOMPARE(cov.size(), QSize(100, 100));
        QVERIFY(closeColor(QColor(cov.pixel(50, 50)), QColor(Qt::red), 10));
        QImage clear(10, 10, QImage::Format_ARGB32);
        clear.fill(Qt::transparent);
        QCOMPARE(QColor(flatten(clear).pixel(5, 5)), QColor(Qt::white));
        QVERIFY(!flatten(clear).hasAlphaChannel());
        const QByteArray th = rgbThumb(solid, 80, 45);
        QCOMPARE(th.size(), qsizetype(80 * 45 * 3));
        const auto m = meanRgb(th);
        QVERIFY(std::abs(m[0] - 200) < 2 && std::abs(m[1] - 60) < 2 && std::abs(m[2] - 30) < 2);
        QVERIFY(dataUrl(png, QStringLiteral("image/png")).startsWith("data:image/png;base64,iVBORw0KGgo"));
    }

    void shrink() {
        QString err;
        // fits already: the very same bytes back
        QImage small(100, 80, QImage::Format_RGB32);
        small.fill(qRgb(10, 120, 200));
        const QByteArray smallPng = encodePng(small);
        Shrunk s = shrinkImage(smallPng);
        QVERIFY2(s.ok, qPrintable(s.error));
        QVERIFY(!s.changed && s.bytes == smallPng && s.mime == QLatin1String("image/png") && s.width == 100);
        // too large a side: JPEG at <= 1536
        QImage big(2400, 1200, QImage::Format_RGB32);
        { QPainter p(&big); QLinearGradient g(0, 0, 2400, 1200); g.setColorAt(0, Qt::darkBlue); g.setColorAt(1, Qt::yellow); p.fillRect(big.rect(), g); }
        s = shrinkImage(encodePng(big), kReferenceImage);
        QVERIFY2(s.ok, qPrintable(s.error));
        QVERIFY(s.changed && s.mime == QLatin1String("image/jpeg") && std::max(s.width, s.height) <= 1536 && s.bytes.size() <= kReferenceImage.maxBytes);
        QCOMPARE(s.width, 1536);
        QCOMPARE(s.quality, 0.9);
        // too many bytes: lower quality, then smaller, until it fits
        const QByteArray noisy = encodePng(noiseImage(600, 400, 3));
        s = shrinkImage(noisy, ImageLimit{1536, 60 * 1024});
        QVERIFY2(s.ok, qPrintable(s.error));
        QVERIFY(s.changed && s.bytes.size() <= 60 * 1024 && s.width < 600);
        QCOMPARE(decodeImage(s.bytes, &err).width(), s.width);
        // impossible
        s = shrinkImage(noisy, ImageLimit{1536, 200});
        QVERIFY(!s.ok && s.error.contains(QLatin1String("smaller")));
        // transparency is flattened onto white, not black
        QImage alpha(2000, 100, QImage::Format_ARGB32);
        alpha.fill(Qt::transparent);
        { QPainter p(&alpha); p.fillRect(1000, 0, 1000, 100, Qt::red); }
        s = shrinkImage(encodePng(alpha), kReferenceImage);
        QVERIFY2(s.ok && s.changed, qPrintable(s.error));
        const QImage back = decodeImage(s.bytes, &err);
        QVERIFY(closeColor(meanColor(back, QRect(10, 10, 40, 40)), QColor(Qt::white), 6));
        QVERIFY(closeColor(meanColor(back, QRect(back.width() - 60, 10, 40, 40)), QColor(Qt::red), 12));
        // a format models do not take (BMP) is re-encoded even when it is small
        QByteArray bmp;
        { QBuffer b(&bmp); b.open(QIODevice::WriteOnly); QVERIFY(small.save(&b, "BMP")); }
        s = shrinkImage(bmp);
        QVERIFY(s.ok && s.changed && s.mime == QLatin1String("image/jpeg"));
    }

    void orientation() {
        // orientImage moves pixels exactly: a 3 x 2 picture whose pixels all differ
        QImage p(3, 2, QImage::Format_RGB32);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) p.setPixel(x, y, qRgb(x * 80, y * 200, 7));
        auto px = [](const QImage& i, int x, int y) { return i.pixel(x, y) & 0xFFFFFFu; };
        auto src = [&](int x, int y) { return px(p, x, y); };
        QVERIFY(orientImage(p, 1) == p && orientImage(p, 0) == p && orientImage(p, 9) == p);
        const QImage r6 = orientImage(p, 6);   // 90 clockwise: the bottom-left pixel ends top-left
        QCOMPARE(r6.size(), QSize(2, 3));
        QCOMPARE(px(r6, 0, 0), src(0, 1));
        QCOMPARE(px(r6, 1, 0), src(0, 0));
        QCOMPARE(px(r6, 0, 2), src(2, 1));
        const QImage r8 = orientImage(p, 8);   // 90 counter-clockwise: the top-right pixel ends top-left
        QCOMPARE(r8.size(), QSize(2, 3));
        QCOMPARE(px(r8, 0, 0), src(2, 0));
        QCOMPARE(px(r8, 1, 2), src(0, 1));
        QCOMPARE(px(orientImage(p, 3), 0, 0), src(2, 1));   // 180
        QCOMPARE(px(orientImage(p, 2), 0, 0), src(2, 0));   // mirror left-right
        QCOMPARE(px(orientImage(p, 4), 0, 0), src(0, 1));   // mirror top-bottom
        QCOMPARE(px(orientImage(p, 5), 1, 0), src(0, 1));   // transpose
        QCOMPARE(px(orientImage(p, 7), 0, 0), src(2, 1));   // transverse
        QVERIFY(orientImage(r6, 8) == p);
        QVERIFY(orientImage(orientImage(p, 5), 5) == p);
        // the frame format (RGB888) and an indexed picture move the same way
        QVERIFY(orientImage(p.convertToFormat(QImage::Format_RGB888), 6).convertToFormat(QImage::Format_RGB32) == r6);
        const QImage indexed = p.convertToFormat(QImage::Format_Indexed8);
        QVERIFY(orientImage(indexed, 7).convertToFormat(QImage::Format_RGB32) == orientImage(indexed.convertToFormat(QImage::Format_RGB32), 7));

        // a phone photo: a JPEG stored 200 x 100 (red left half, blue right half) tagged EXIF orientation 6
        // (show it turned 90 degrees clockwise). Both decode paths give it upright: 100 x 200, red on top.
        QImage wide(200, 100, QImage::Format_RGB32);
        wide.fill(Qt::red);
        { QPainter pp(&wide); pp.fillRect(100, 0, 100, 100, Qt::blue); }
        QString err;
        const QByteArray plain = encodeJpeg(wide, 0.9, &err);
        QVERIFY2(!plain.isEmpty(), qPrintable(err));
        const QByteArray exif6 = withExifOrientation(plain, 6);
        for (const Codec c : {Codec::Auto, Codec::Qt, Codec::Libav}) {
            const QImage img = decodeImage(exif6, &err, DecodeLimits(), c);
            QVERIFY2(!img.isNull(), qPrintable(err));
            QCOMPARE(img.size(), QSize(100, 200));
            QVERIFY(closeColor(meanColor(img, QRect(30, 30, 40, 40)), QColor(Qt::red), 40));
            QVERIFY(closeColor(meanColor(img, QRect(30, 130, 40, 40)), QColor(Qt::blue), 40));
        }
        const QImage left = decodeImage(withExifOrientation(plain, 8), &err, DecodeLimits(), Codec::Libav);   // 90 ccw
        QVERIFY(left.size() == QSize(100, 200) && closeColor(meanColor(left, QRect(30, 130, 40, 40)), QColor(Qt::red), 40));
        QCOMPARE(decodeImage(plain, &err, DecodeLimits(), Codec::Libav).size(), QSize(200, 100));
        // shrinkImage re-encodes it upright: the EXIF tag does not survive a re-encode, so the pixels must be turned
        const Shrunk s = shrinkImage(exif6, ImageLimit{150, 3 * kMiB});
        QVERIFY2(s.ok && s.changed, qPrintable(s.error));
        QCOMPARE(QSize(s.width, s.height), QSize(75, 150));
        const QImage back = decodeImage(s.bytes, &err);
        QCOMPARE(back.size(), QSize(75, 150));
        QVERIFY(closeColor(meanColor(back, QRect(20, 20, 30, 30)), QColor(Qt::red), 40));
    }

    void contactSheetAndPixelFont() {
        std::vector<SheetInput> cells;
        const QColor cols[] = {QColor(220, 30, 30), QColor(30, 200, 40), QColor(40, 60, 220), QColor(230, 220, 40)};
        for (int i = 0; i < 4; ++i) {
            QImage img(640, 360, QImage::Format_RGB32);
            img.fill(cols[i]);
            cells.push_back({img, QString::number(i + 1)});
        }
        cells.push_back({QImage(), QStringLiteral("5")});
        const ContactSheet cs = contactSheet(cells, 384, 0.9);
        QVERIFY2(cs.ok, qPrintable(cs.error));
        QCOMPARE(cs.failed, QStringList{QStringLiteral("5")});
        QVERIFY(cs.layout.cols == 3 && cs.layout.rows == 2 && cs.layout.width == 3 * 384 + 4 * 6 && cs.layout.height == 2 * 216 + 3 * 6);
        QString err;
        const QImage sheet = decodeImage(cs.jpeg, &err);
        QCOMPARE(sheet.size(), QSize(cs.layout.width, cs.layout.height));
        // background: the empty sixth slot of the 3 x 2 grid (a 6 px gutter shares JPEG blocks with the cells next to it)
        QVERIFY(closeColor(meanColor(sheet, QRect(6 + 2 * 390 + 150, 6 + 222 + 80, 60, 40)), QColor(0x18, 0x18, 0x1b), 6));
        for (int i = 0; i < 4; ++i) {
            const SheetCell& c = cs.layout.cells[size_t(i)];
            QVERIFY(closeColor(meanColor(sheet, QRect(c.x + c.w / 2 - 10, c.y + 20, 20, 20)), cols[i], 12));
        }
        const SheetCell& failed = cs.layout.cells[4];
        QVERIFY(closeColor(meanColor(sheet, QRect(failed.x + failed.w / 2 - 10, failed.y + 20, 20, 20)), QColor(0x3f, 0x3f, 0x46), 10));
        // the label pill bottom-left of cell 1: a dark pill with white glyph pixels in it
        const SheetCell& c0 = cs.layout.cells[0];
        const int fontPx = 24, pillH = 38, px = c0.x + 12, py = c0.y + c0.h - pillH - 12;
        int white = 0;
        for (int y = py; y < py + pillH; ++y)
            for (int x = px; x < px + 40; ++x) if (qGray(sheet.pixel(x, y)) > 200) ++white;
        QVERIFY2(white > 10, qPrintable(QStringLiteral("white label pixels: %1").arg(white)));
        QVERIFY(qGray(sheet.pixel(px + 4, py + pillH / 2)) < 110);   // pill background over red, darkened
        Q_UNUSED(fontPx);
        QVERIFY(!contactSheet({}).ok);

        QCOMPARE(pixelTextWidth(QStringLiteral("12"), 2), 22);
        QCOMPARE(pixelTextWidth(QString(), 3), 0);
        QImage canvas(40, 12, QImage::Format_RGB32);
        canvas.fill(Qt::black);
        drawPixelText(canvas, 1, 1, QStringLiteral("8"), 1, qRgb(255, 255, 255));
        int lit = 0;
        for (int y = 0; y < 12; ++y) for (int x = 0; x < 40; ++x) if (canvas.pixel(x, y) == qRgb(255, 255, 255)) ++lit;
        QCOMPARE(lit, 3 + 2 + 2 + 3 + 2 + 2 + 3);   // the 5x7 '8'
        QImage q1(12, 9, QImage::Format_RGB32), q2(12, 9, QImage::Format_RGB32);
        q1.fill(Qt::black); q2.fill(Qt::black);
        drawPixelText(q1, 1, 1, QStringLiteral("\u00e9"), 1, qRgb(255, 255, 255));   // not in the font: '?'
        drawPixelText(q2, 1, 1, QStringLiteral("?"), 1, qRgb(255, 255, 255));
        QVERIFY(q1 == q2);
    }

    // ================================================================ libav: opening rules and probe

    void openRules() {
        QVERIFY(av::refusedFormat("concat"));
        QVERIFY(av::refusedFormat("hls"));
        QVERIFY(av::refusedFormat("avisynth"));
        QVERIFY(av::refusedFormat("image2"));
        QVERIFY(av::refusedFormat(nullptr));
        QVERIFY(av::refusedFormat("mov,mp4,m4a", true));   // AVFMT_NOFILE
        QVERIFY(!av::refusedFormat("mov,mp4,m4a,3gp,3g2,mj2"));
        QVERIFY(!av::refusedFormat("png_pipe"));
        QVERIFY(!av::refusedFormat("wav"));
        QString err;
        QVERIFY(!av::openFile(QStringLiteral("http://127.0.0.1:9/x.mp4"), nullptr, &err));
        QVERIFY(err.contains(QLatin1String("local files")));
        NEED_MEDIA();
        const MediaInfo concat = probe(path("list.ffconcat"));
        QVERIFY(!concat.ok);
        QVERIFY2(concat.error.contains(QLatin1String("refused format concat")), qPrintable(concat.error));
        const MediaInfo hls = probe(path("list.m3u8"));
        QVERIFY(!hls.ok);
        QVERIFY2(hls.error.contains(QLatin1String("refused format hls")), qPrintable(hls.error));
        QVERIFY(!FrameSampler(path("list.ffconcat")).ok());
        QVERIFY(!toWav16k(path("list.m3u8")).ok);
        const MediaInfo txt = probe(path("notes.txt"));
        QVERIFY(!txt.ok);
        QCOMPARE(txt.toJson().value("ok").toBool(true), false);
        const MediaInfo missing = probe(path("nope.mp4"));
        QVERIFY(!missing.ok && missing.error.contains(QLatin1String("cannot open")));
        std::atomic<bool> stop{true};
        ProbeOptions po;
        po.cancel = &stop;
        const MediaInfo cancelled = probe(path("stills.mp4"), po);
        QVERIFY2(!cancelled.ok && cancelled.error.contains(QLatin1String("cancelled")), qPrintable(cancelled.error));
        // memory input follows the same rules
        QFile f(path("list.ffconcat"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(!av::openMemory(f.readAll(), nullptr, &err));
    }

    void probeFiles() {
        NEED_MEDIA();
        const MediaInfo mp4 = probe(path("stills.mp4"));
        QVERIFY2(mp4.ok, qPrintable(mp4.error));
        QVERIFY(mp4.format.contains(QLatin1String("mp4")));
        QVERIFY2(near(mp4.durationSec, 9.0, 0.06), qPrintable(QString::number(mp4.durationSec)));
        QVERIFY(mp4.hasVideo() && mp4.hasAudio() && !mp4.isImage);
        QCOMPARE(mp4.video.codec, QStringLiteral("h264"));
        QVERIFY(mp4.video.width == 320 && mp4.video.height == 180 && mp4.video.displayWidth == 320);
        QVERIFY(near(mp4.video.fps, 25.0, 0.01));
        QCOMPARE(mp4.video.rotation, 0);
        QCOMPARE(mp4.audio.codec, QStringLiteral("aac"));
        QVERIFY(mp4.audio.sampleRate == 48000 && mp4.audio.channels == 2 && mp4.audio.layout == QLatin1String("stereo"));
        QVERIFY(mp4.sizeBytes > 1000);
        const QJsonObject j = mp4.toJson();
        QVERIFY(j.value("ok").toBool() && j.value("video").toObject().value("width").toInt() == 320 && j.value("audio").toObject().value("channels").toInt() == 2);

        ProbeOptions exact;
        exact.exactDuration = true;
        const MediaInfo mx = probe(path("stills.mp4"), exact);
        QVERIFY(mx.ok && mx.durationExact);
        QVERIFY2(near(mx.durationSec, 9.0, 0.08), qPrintable(QString::number(mx.durationSec)));

        const MediaInfo wav = probe(path("tone.wav"));
        QVERIFY2(wav.ok, qPrintable(wav.error));
        QCOMPARE(wav.format, QStringLiteral("wav"));
        QVERIFY(near(wav.durationSec, 7.0, 0.01));
        QVERIFY(!wav.hasVideo() && wav.hasAudio());
        QVERIFY(wav.audio.codec == QLatin1String("pcm_s16le") && wav.audio.sampleRate == 44100 && wav.audio.channels == 2);
        QVERIFY(wav.toJson().value("video").isNull());

        const MediaInfo cuts = probe(path("cuts.mp4"));
        QVERIFY(cuts.ok && cuts.hasVideo() && !cuts.hasAudio());
        QVERIFY(near(cuts.durationSec, 12.0, 0.06));

        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::cyan);
        QFile pf(path("pic.png"));
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.write(encodePng(img));
        pf.close();
        const MediaInfo pic = probe(path("pic.png"));
        QVERIFY2(pic.ok, qPrintable(pic.error));
        QVERIFY(pic.isImage && pic.video.codec == QLatin1String("png") && pic.video.width == 64 && pic.video.height == 48);
        QCOMPARE(pic.durationSec, 0.0);
        QCOMPARE(pic.video.fps, 0.0);
        // a "%d" in the name is not a sequence pattern here (image2 would claim it and be refused)
        QVERIFY(QFile::copy(path("pic.png"), path("pic%d.png")));
        const MediaInfo pct = probe(path("pic%d.png"));
        QVERIFY2(pct.ok && pct.isImage && pct.format == pic.format, qPrintable(pct.error + u' ' + pct.format));
        // JPEGs over 2 KB: image2 claims any .jpg / .jpeg name with 50 (52 with '{'), above jpeg_pipe's 26 before the
        // end marker is in the probe buffer, and image2 is refused, so every ordinary JPEG failed. The bytes decide.
        const QByteArray jpg = encodeJpeg(noiseImage(160, 120, 7), 0.9);
        QVERIFY(jpg.size() > 8 * 1024);
        // a camera-style JPEG with a 60 KB segment before the picture: the second probe has to read past the first one's
        QByteArray com("\xFF\xFE", 2);
        com.append(char(0xEA)).append(char(0x62)).append(QByteArray(0xEA62 - 2, '\0'));   // COM, length 60002
        const QByteArray camera = jpg.left(2) + com + jpg.mid(2);
        const auto writeFile = [&](const char* name, const QByteArray& bytes) {
            QFile f(path(name));
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        for (const char* name : {"photo.jpg", "photo.JPEG", "photo{1}.jpg", "pic%d.jpg", "camera.jpg", "noext"}) {
            QVERIFY(writeFile(name, QLatin1String(name) == QLatin1String("camera.jpg") ? camera : jpg));
            const MediaInfo jm = probe(path(name));
            QVERIFY2(jm.ok && jm.isImage && jm.format == QLatin1String("jpeg_pipe") && jm.video.codec == QLatin1String("mjpeg")
                         && jm.video.width == 160 && jm.video.height == 120,
                     qPrintable(QLatin1String(name) + u' ' + jm.error + u' ' + jm.format));
        }
        // a PNG named .jpg is still a PNG (the bytes beat the name), and a playlist the bytes alone do not match keeps
        // the refusal the name gave it (hls wants the .m3u8 name), checked in openRules
        QVERIFY(QFile::copy(path("pic.png"), path("png.jpg")));
        const MediaInfo pj = probe(path("png.jpg"));
        QVERIFY2(pj.ok && pj.format == pic.format, qPrintable(pj.error + u' ' + pj.format));

        // the async form runs on a pool and gives the same answer; the test loop stays free while it runs
        QThreadPool pool;
        pool.setMaxThreadCount(2);
        QFuture<MediaInfo> fut = probeAsync(path("stills.mp4"), ProbeOptions(), &pool);
        QTRY_VERIFY_WITH_TIMEOUT(fut.isFinished(), 15000);
        QVERIFY(fut.result().ok && fut.result().durationSec == mp4.durationSec);
    }

    // ================================================================ libav: frames, scenes, stills

    void frameSampler() {
        NEED_MEDIA();
        FrameSampler fs(path("cuts.mp4"));
        QVERIFY2(fs.ok(), qPrintable(fs.error()));
        QVERIFY(near(fs.duration(), 12.0, 0.06));
        QVERIFY(fs.width() == 320 && fs.height() == 180 && fs.displayWidth() == 320 && fs.displayHeight() == 180 && near(fs.fps(), 25.0, 0.01));
        QCOMPARE(fs.orientation(), 1);
        // an ascending scan decodes each frame at most once (no seeks at 0.25 s steps)
        std::vector<QByteArray> scan;
        for (int k = 0; k < 48; ++k) {
            QByteArray t = fs.thumbAt(k * 0.25, 48, 27);
            QVERIFY2(t.size() == 48 * 27 * 3, qPrintable(fs.error()));
            scan.push_back(t);
        }
        QVERIFY2(fs.framesDecoded() <= 300 + 2, qPrintable(QString::number(fs.framesDecoded())));
        const qint64 linear = fs.framesDecoded();
        auto rgbAt = [&](double t) { return meanRgb(fs.thumbAt(t, 48, 27)); };
        const auto light = rgbAt(3.8), dark = rgbAt(4.2), yellow = rgbAt(10.0);
        QVERIFY(light[0] > 150 && light[1] > 150 && light[2] > 150);
        QVERIFY(dark[0] < 80 && dark[1] < 80 && dark[2] < 80);
        QVERIFY(yellow[0] > 180 && yellow[1] > 140 && yellow[2] < 110);
        // the frame on screen: an exact frame time, and just after it
        QVERIFY(!fs.thumbAt(2.0, 16, 9).isEmpty());
        QVERIFY(near(fs.lastFrameTime(), 2.0, 1e-3));
        QVERIFY(!fs.thumbAt(2.013, 16, 9).isEmpty());
        QVERIFY(near(fs.lastFrameTime(), 2.0, 1e-3));
        // past the end: the last frame
        QVERIFY(!fs.thumbAt(11.99, 16, 9).isEmpty());
        QVERIFY(fs.lastFrameTime() > 11.9);
        // a seek back gives exactly the frame the linear scan gave
        QCOMPARE(fs.thumbAt(1.0, 48, 27), scan[4]);
        QCOMPARE(fs.thumbAt(7.0, 48, 27), scan[28]);
        // display-size images, never upscaled
        QCOMPARE(fs.imageAt(5.0, 160, 0).size(), QSize(160, 90));
        QCOMPARE(fs.imageAt(5.0, 0, 0).size(), QSize(320, 180));
        QCOMPARE(fs.imageAt(5.0, 1000, 1000).size(), QSize(320, 180));
        QVERIFY(fs.thumbAt(1.0, 0, 9).isEmpty());

        // steps longer than seekAheadSec (0.1 s here, 1 s GOPs): a seek happens only when the keyframe before the
        // target lies past the decoded frame, so the scan still decodes each frame at most once, with the same bytes
        FrameSampler::Options eager;
        eager.seekAheadSec = 0.1;
        FrameSampler fe(path("cuts.mp4"), eager);
        for (int k = 0; k < 48; ++k) QCOMPARE(fe.thumbAt(k * 0.25, 48, 27), scan[size_t(k)]);
        QVERIFY2(fe.framesDecoded() <= 300 + 2, qPrintable(QString::number(fe.framesDecoded())));
        qInfo("ascending scan, 0.25 s steps: %lld frames decoded (seekAheadSec 4), %lld (seekAheadSec 0.1)",
              linear, fe.framesDecoded());

        // a video handed to decodeImage is not an image (its first frame is not decoded)
        QFile vf(path("cuts.mp4"));
        QVERIFY(vf.open(QIODevice::ReadOnly));
        QString err;
        QVERIFY(decodeImage(vf.readAll(), &err).isNull());
        QVERIFY2(err.contains(QLatin1String("not an image")), qPrintable(err));

        FrameSampler noVideo(path("tone.wav"));
        QVERIFY(!noVideo.ok() && noVideo.error().contains(QLatin1String("no video")));
        std::atomic<bool> stop{true};
        FrameSampler::Options o;
        o.cancel = &stop;
        FrameSampler cancelled(path("cuts.mp4"), o);
        QVERIFY(!cancelled.ok() && cancelled.error().contains(QLatin1String("cancelled")));
        const ThumbScan ts = sampleThumbs(path("cuts.mp4"), {0.5, 4.5, 9.5}, 8, 8);
        QVERIFY(ts.ok && ts.rgb.size() == 3 && ts.failed == 0 && ts.width == 320);
    }

    void rotatedVideo() {
        NEED_MEDIA();
        // a portrait phone clip: stored 320 x 180, display matrix says turn it; every frame comes out as ffmpeg shows it
        const MediaInfo mi = probe(path("rot.mp4"));
        QVERIFY2(mi.ok, qPrintable(mi.error));
        QCOMPARE(mi.video.rotation, 270);   // 90 counter-clockwise = 270 clockwise
        QString err;
        QFile of(path("rot1.png"));
        QVERIFY(of.open(QIODevice::ReadOnly));
        const QImage oracle = decodeImage(of.readAll(), &err);
        QVERIFY2(oracle.size() == QSize(180, 320), qPrintable(err));
        const QRect top(40, 20, 100, 40), bottom(40, 260, 100, 40);
        QVERIFY(!closeColor(meanColor(oracle, top), meanColor(oracle, bottom), 60));   // the band is at one end only

        FrameSampler fs(path("rot.mp4"));
        QVERIFY2(fs.ok(), qPrintable(fs.error()));
        QCOMPARE(fs.orientation(), 8);
        QVERIFY(fs.width() == 320 && fs.height() == 180 && fs.displayWidth() == 180 && fs.displayHeight() == 320);
        const QImage img = fs.imageAt(1.0, 0, 0);
        QCOMPARE(img.size(), QSize(180, 320));
        QVERIFY(closeColor(meanColor(img, top), meanColor(oracle, top), 24));
        QVERIFY(closeColor(meanColor(img, bottom), meanColor(oracle, bottom), 24));
        QVERIFY(closeColor(meanColor(img, bottom), QColor(Qt::blue), 40));   // turned counter-clockwise: left edge at the bottom
        QCOMPARE(fs.imageAt(1.0, 90, 0).size(), QSize(90, 160));
        QCOMPARE(fs.imageAt(1.0, 0, 160).size(), QSize(90, 160));
        // thumbnails are turned the same way, then stretched
        const QByteArray th = fs.thumbAt(1.0, 18, 32);
        QCOMPARE(th.size(), qsizetype(18 * 32 * 3));
        const QImage thumb = QImage(reinterpret_cast<const uchar*>(th.constData()), 18, 32, 18 * 3, QImage::Format_RGB888).copy();
        QVERIFY(closeColor(meanColor(thumb, QRect(4, 2, 10, 4)), meanColor(oracle, top), 40));
        QVERIFY(closeColor(meanColor(thumb, QRect(4, 26, 10, 4)), meanColor(oracle, bottom), 40));
        // autoRotate off: the coded picture
        FrameSampler::Options raw;
        raw.autoRotate = false;
        FrameSampler fr(path("rot.mp4"), raw);
        QVERIFY(fr.ok() && fr.orientation() == 1 && fr.displayWidth() == 320 && fr.displayHeight() == 180);
        QCOMPARE(fr.imageAt(1.0, 0, 0).size(), QSize(320, 180));
        // grabs, key frames and the scrub report the display size
        const GrabbedFrame g = grabFrame(path("rot.mp4"), 1.0, 160);
        QVERIFY2(g.ok && g.width == 90 && g.height == 160, qPrintable(g.error));
        QCOMPARE(decodeImage(g.jpeg, &err).size(), QSize(90, 160));
        const std::vector<GrabbedFrame> kf = keyFrames(path("rot.mp4"), {Scene{0, 2, 1}}, 90);
        QVERIFY(kf.size() == 1 && kf[0].width == 90 && kf[0].height == 160);
        ScrubInput in;
        in.scenes = std::vector<SceneSpan>{{0, 2}};
        in.options.keep = 1;
        in.grab = false;
        const ScrubResult sr = scrubVideo(path("rot.mp4"), in);
        QVERIFY2(sr.ok, qPrintable(sr.error));
        QVERIFY(sr.width == 180 && sr.height == 320 && sr.stills.size() == 1 && sr.stills[0].width == 180 && sr.stills[0].height == 320);
    }

    void sceneCuts() {
        NEED_MEDIA();
        const DetectResult d = detectScenes(path("cuts.mp4"));
        QVERIFY2(d.ok, qPrintable(d.error));
        QCOMPARE(d.sampled, 48);
        QCOMPARE(d.scenes.size(), size_t(3));
        QCOMPARE(d.scenes[0].start, 0.0);
        QVERIFY2(near(d.scenes[1].start, 4.0, 0.26) && near(d.scenes[2].start, 9.0, 0.26),
                 qPrintable(QStringLiteral("%1 %2").arg(d.scenes[1].start).arg(d.scenes[2].start)));
        QVERIFY(near(d.scenes[2].end, 12.0, 0.06));
        QVERIFY(d.toJson().value("scenes").toArray().size() == 3);
        // key frames: one per scene at its midpoint, width-capped
        const std::vector<GrabbedFrame> kf = keyFrames(path("cuts.mp4"), d.scenes, 160);
        QCOMPARE(kf.size(), size_t(3));
        for (size_t i = 0; i < kf.size(); ++i) {
            QVERIFY(kf[i].ok && kf[i].width == 160 && kf[i].height == 90);
            QVERIFY(near(kf[i].t, d.scenes[i].midpoint, 0.006));
        }
        QString err;
        QVERIFY(closeColor(meanColor(decodeImage(kf[2].jpeg, &err), QRect(70, 40, 20, 10)), QColor(0xF0, 0xC0, 0x20), 40));
        const GrabbedFrame g = grabFrame(path("stills.mp4"), 100, 1280);
        QVERIFY2(g.ok && near(g.t, 8.95, 0.06) && g.width == 320 && g.height == 180, qPrintable(g.error));
        // async on a pool
        QThreadPool pool;
        QFuture<DetectResult> fut = detectScenesAsync(path("cuts.mp4"), DetectOptions(), &pool);
        QTRY_VERIFY_WITH_TIMEOUT(fut.isFinished(), 30000);
        QCOMPARE(fut.result().scenes.size(), size_t(3));
    }

    void stillScoring() {
        NEED_MEDIA();
        // the measures on real decoded thumbnails: blur, crushed blacks and a fade score lower than the clean halves
        const ThumbScan t = sampleThumbs(path("stills.mp4"), {0.5, 2.5, 3.5, 5.5, 6.3, 8.0}, 80, 45);
        QVERIFY2(t.ok && t.rgb.size() == 6, qPrintable(t.error));
        auto grey = [&](int i) { return greyOf(t.rgb[size_t(i)], 80, 45); };
        const double blurred = sharpness(grey(0), 80, 45), sharp = sharpness(grey(1), 80, 45);
        QVERIFY2(sharp > blurred + 0.03, qPrintable(QStringLiteral("sharp %1 blurred %2").arg(sharp).arg(blurred)));
        const double darkExp = exposureScore(brightness(grey(2))), brightExp = exposureScore(brightness(grey(3)));
        QVERIFY2(brightExp > darkExp + 0.3, qPrintable(QStringLiteral("bright %1 dark %2").arg(brightExp).arg(darkExp)));
        QVERIFY(brightness(grey(4)).mean < brightness(grey(5)).mean);

        // one still per given scene, each from its clean half
        ScrubInput in;
        in.scenes = std::vector<SceneSpan>{{0, 3}, {3, 6}, {6, 9}};
        in.options.keep = 3;
        QStringList phases;
        in.onProgress = [&](const ScrubProgress& p) { if (!phases.contains(p.phase)) phases << p.phase; };
        const ScrubResult r = scrubVideo(path("stills.mp4"), in);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.stills.size(), size_t(3));
        QString picked;
        for (const ScrubbedStill& s : r.stills) picked += QStringLiteral("%1@%2(%3) ").arg(s.sceneIndex).arg(s.t, 0, 'f', 2).arg(s.score, 0, 'f', 3);
        QVERIFY2(r.stills[0].sceneIndex == 0 && r.stills[0].t >= 1.5 && r.stills[0].t < 3, qPrintable(picked));
        QVERIFY2(r.stills[1].sceneIndex == 1 && r.stills[1].t >= 4.5 && r.stills[1].t < 6, qPrintable(picked));
        QVERIFY2(r.stills[2].sceneIndex == 2 && r.stills[2].t >= 7.5 - 1e-6 && r.stills[2].t < 9, qPrintable(picked));
        QString err;
        for (const ScrubbedStill& s : r.stills) {
            QVERIFY(s.width == 320 && s.height == 180);
            const QImage img = decodeImage(s.jpeg, &err);
            QCOMPARE(img.size(), QSize(320, 180));
        }
        QVERIFY(!r.scenesDerived && r.scenes.size() == 3);
        QCOMPARE(phases, (QStringList{"open", "scan", "select", "grab", "done"}));
        QVERIFY(r.scan.sampled > 15 && r.scan.kept == 3 && r.scan.step > 0);
        QVERIFY(r.toJson().value("stills").toArray().size() == 3);
        qInfo("picked: %s", qPrintable(picked));
    }

    void scrubDerivedScenes() {
        NEED_MEDIA();
        ScrubInput in;
        in.options.fps = 4;   // 0.25 s samples: enough of them for the cut rule to see the two cuts
        const ScrubResult r = scrubVideo(path("cuts.mp4"), in);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.scenesDerived);
        QVERIFY2(r.scenes.size() == 3, qPrintable(QString::number(r.scenes.size())));
        QCOMPARE(r.scenes.front().start, 0.0);
        QVERIFY(near(r.scenes.back().end, 12.0, 0.06));
        for (size_t i = 1; i < r.scenes.size(); ++i) QVERIFY(near(r.scenes[i].start, r.scenes[i - 1].end));
        QVERIFY(!r.stills.empty());
        for (const ScrubbedStill& s : r.stills) QVERIFY(s.sceneIndex >= 0 && s.sceneIndex < 3 && !s.jpeg.isEmpty());
        // static scenes: one still each, the rest are near-duplicates
        QCOMPARE(r.stills.size(), size_t(3));
        QVERIFY(r.scan.dropped.nearDuplicate > 30);
        // times only, no JPEG work
        in.grab = false;
        const ScrubResult noGrab = scrubVideo(path("cuts.mp4"), in);
        QVERIFY(noGrab.ok && noGrab.stills.size() == 3 && noGrab.stills[0].jpeg.isEmpty() && noGrab.stills[0].width == 320);
        // cancel and async
        std::atomic<bool> stop{true};
        in.cancel = &stop;
        QVERIFY(scrubVideo(path("cuts.mp4"), in).error.contains(QLatin1String("cancelled")));
        in.cancel = nullptr;
        QThreadPool pool;
        QFuture<ScrubResult> fut = scrubVideoAsync(path("cuts.mp4"), in, &pool);
        QTRY_VERIFY_WITH_TIMEOUT(fut.isFinished(), 30000);
        QVERIFY(fut.result().ok);
        QVERIFY(!scrubVideo(path("tone.wav")).ok);
    }

    // ================================================================ libav: 16 kHz audio

    void wav16kAndPieces() {
        NEED_MEDIA();
        const Wav16k tone = toWav16k(path("tone.wav"));
        QVERIFY2(tone.ok, qPrintable(tone.error));
        const std::optional<WavInfo> ti = wavInfo(tone.wav);
        QVERIFY(ti && ti->sampleRate == 16000 && ti->channels == 1 && ti->bitsPerSample == 16);
        QVERIFY2(near(tone.seconds, 7.0, 0.01), qPrintable(QString::number(tone.seconds)));
        QCOMPARE(tone.leadSilence, 0.0);
        QVERIFY(toWav16k(path("cuts.mp4")).error.contains(QLatin1String("no audio")));

        // 650 s of AAC -> 300 + 300 + 50 s pieces
        const QString dir = path("pieces");
        QElapsedTimer t;
        t.start();
        std::vector<double> seen;
        SplitOptions so;
        so.onPiece = [&](int, double offset, double) { seen.push_back(offset); };
        const SplitResult sp = splitAudio16k(path("long.m4a"), dir, so);
        QVERIFY2(sp.ok, qPrintable(sp.error));
        qInfo("650 s split in %lld ms", qint64(t.elapsed()));
        QCOMPARE(sp.pieceSeconds, 300.0);
        QCOMPARE(sp.pieces.size(), size_t(3));
        QCOMPARE(seen, (std::vector<double>{0, 300, 600}));
        QVERIFY(sp.sourceCodec == QLatin1String("aac") && sp.sourceRate == 48000 && sp.sourceChannels == 2);
        QVERIFY2(near(sp.seconds, 650.0, 0.05), qPrintable(QString::number(sp.seconds, 'f', 4)));
        QByteArray joined;
        for (size_t i = 0; i < sp.pieces.size(); ++i) {
            const AudioPiece& p = sp.pieces[i];
            QCOMPARE(p.index, int(i));
            QCOMPARE(QFileInfo(p.path).fileName(), QStringLiteral("audio-%1.wav").arg(i, 3, 10, QLatin1Char('0')));
            QCOMPARE(p.offset, 300.0 * double(i));
            QVERIFY(p.seconds <= 300.0);
            QFile f(p.path);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QByteArray bytes = f.readAll();
            QCOMPARE(qint64(bytes.size()), p.bytes);
            const std::optional<WavInfo> pi = wavInfo(bytes);
            QVERIFY(pi && pi->sampleRate == 16000 && pi->channels == 1 && pi->bitsPerSample == 16);
            QCOMPARE(pi->dataBytes, qint64(std::llround(p.seconds * 16000)) * 2);
            joined += bytes.mid(44);
        }
        QCOMPARE(sp.pieces[0].seconds, 300.0);
        QCOMPARE(sp.pieces[1].seconds, 300.0);
        QCOMPARE(QFileInfo(sp.pieces[0].path).size(), qint64(44 + 9'600'000));   // 300 s x 32,000 bytes
        QVERIFY(near(sp.pieces[2].seconds, 50.0, 0.05));
        // the pieces are exactly the one-piece conversion cut at the boundaries: nothing lost or doubled
        const Wav16k whole = toWav16k(path("long.m4a"));
        QVERIFY2(whole.ok, qPrintable(whole.error));
        QVERIFY2(whole.wav.mid(44) == joined, "pieces differ from the one-piece conversion");
        QVERIFY(sp.toJson().value("pieces").toArray().size() == 3);

        // a longer piece length asked for is clamped to 5 minutes; maxSeconds stops early; short pieces
        SplitOptions longer;
        longer.pieceSeconds = 1000;
        longer.baseName = QStringLiteral("big ../name");
        const SplitResult cl = splitAudio16k(path("long.m4a"), path("clamped"), longer);
        QVERIFY(cl.ok && cl.pieceSeconds == 300.0 && cl.pieces.size() == 3);
        QVERIFY(QFileInfo(cl.pieces[0].path).fileName().startsWith(QLatin1String("big_.._name-000")));
        QCOMPARE(QFileInfo(cl.pieces[0].path).absolutePath(), QFileInfo(path("clamped")).absoluteFilePath());
        SplitOptions capped;
        capped.maxSeconds = 100;
        const SplitResult cp = splitAudio16k(path("long.m4a"), path("capped"), capped);
        QVERIFY(cp.ok && cp.pieces.size() == 1 && cp.pieces[0].seconds == 100.0 && cp.seconds == 100.0);
        SplitOptions two;
        two.pieceSeconds = 2;
        const SplitResult tw = splitAudio16k(path("tone.wav"), path("two"), two);
        QVERIFY2(tw.ok && tw.pieces.size() == 4, qPrintable(tw.error));
        QCOMPARE(tw.pieces[3].offset, 6.0);
        QVERIFY(near(tw.pieces[3].seconds, 1.0, 0.01));

        // late sound is padded so transcript times match the picture
        const Wav16k late = toWav16k(path("late.ts"));
        QVERIFY2(late.ok, qPrintable(late.error));
        QVERIFY2(late.leadSilence > 0.45 && late.leadSilence < 0.55, qPrintable(QString::number(late.leadSilence)));
        QVERIFY2(near(late.seconds, 2.5, 0.06), qPrintable(QString::number(late.seconds)));
        Wav16kOptions noAlign;
        noAlign.alignToStart = false;
        QVERIFY(near(toWav16k(path("late.ts"), noAlign).seconds, 2.0, 0.06));

        // failures leave nothing behind
        std::atomic<bool> stop{true};
        SplitOptions cancelled;
        cancelled.cancel = &stop;
        const SplitResult cx = splitAudio16k(path("long.m4a"), path("cancelled"), cancelled);
        QVERIFY(!cx.ok && cx.error.contains(QLatin1String("cancelled")) && cx.pieces.empty());
        QVERIFY(QDir(path("cancelled")).entryList(QDir::Files).isEmpty());
        const SplitResult na = splitAudio16k(path("cuts.mp4"), path("noaudio"));
        QVERIFY(!na.ok && QDir(path("noaudio")).entryList(QDir::Files).isEmpty());
        QVERIFY(!splitAudio16k(path("tone.wav"), path("notes.txt")).ok);   // the out dir is a file
        Wav16kOptions tiny;
        tiny.maxBytes = 1000;
        QVERIFY(toWav16k(path("tone.wav"), tiny).error.contains(QLatin1String("pieces")));
        QThreadPool pool;
        QFuture<SplitResult> fut = splitAudio16kAsync(path("tone.wav"), path("async"), SplitOptions(), &pool);
        QTRY_VERIFY_WITH_TIMEOUT(fut.isFinished(), 30000);
        QVERIFY(fut.result().ok && fut.result().pieces.size() == 1);
    }
};

QTEST_GUILESS_MAIN(TestStudioMedia)
#include "test_studio_media.moc"
