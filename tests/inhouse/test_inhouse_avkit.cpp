// DEP-01: PlayPlayAvKit, the in-process capabilities (avcaps) and ffprobe-compatible probe (avprobe).
// Reference tools: ffmpeg.exe / ffprobe.exe of the same FFmpeg build (next to this test in the build tree, else
// PP_FFMPEG_BIN, else $PLAYPLAY_TEST_FFMPEG_BIN). Never the ffprobe found on PATH (on this box that is a different FFmpeg,
// the very problem AvKit removes). Without them the comparisons skip and the self-checks still run.
//
// Built twice (cmake/lane-inhouse-avkit.cmake):
//   test_inhouse_avkit      the library itself (TestInhouseAvkit, below).
//   test_inhouse_avkit_app  with PP_AVKIT_APP (DEP-02a): the app's MetadataService and bulk importer from the real
//                           PlayPlayCore, which reaches PlayPlayAvKit only through its link interface, run offscreen with
//                           every ffprobe removed from PATH (TestInhouseAvkitApp, at the end).
#ifndef PP_AVKIT_APP
#include "avcaps.h"
#include "avprobe.h"
#else
#include "services/metadataservice.h"
#include "Play_Play_Native/widgets/bulkmediaimporter.h"

#include <QApplication>
#include <QFileDialog>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTimer>

#include <memory>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <atomic>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

#ifndef PP_AVKIT_APP
extern "C" {
#include <libavutil/error.h>
}
#endif

#ifndef PP_FFMPEG_BIN
#define PP_FFMPEG_BIN ""
#endif

namespace {

QString findTool(const QString& exe) {
    QStringList dirs;
    const QString env = qEnvironmentVariable("PLAYPLAY_TEST_FFMPEG_BIN");
    if (!env.isEmpty()) dirs << env;
    dirs << QCoreApplication::applicationDirPath() << QStringLiteral(PP_FFMPEG_BIN);
    for (const QString& d : dirs) {
        if (d.isEmpty()) continue;
        const QString p = QDir(d).filePath(exe);
        if (QFileInfo(p).isFile()) return p;
    }
    return {};
}

struct Run { int code = -1; QByteArray out, err; };
Run run(const QString& exe, const QStringList& args, int timeoutMs = 60000) {
    QProcess p; p.start(exe, args);
    Run r;
    if (!p.waitForStarted(10000)) return r;
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(2000); return r; }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError();
    return r;
}
QByteArray lf(QByteArray b) { return b.replace("\r\n", "\n"); }
QString js(const QJsonValue& v) { return QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)); }

// 16-bit PCM WAV written here, so one media fixture exists even without ffmpeg.exe.
void writeWav(const QString& path, int rate, int channels, double seconds) {
    const int frames = int(seconds * rate);
    QByteArray pcm(frames * channels * 2, '\0');
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c)
            qToLittleEndian<qint16>(qint16(8000 * std::sin(2 * 3.14159265358979 * 440.0 * i / rate)), pcm.data() + (i * channels + c) * 2);
    QByteArray h;
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); h.append(b, 4); };
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); h.append(b, 2); };
    h += "RIFF"; u32(36 + pcm.size()); h += "WAVEfmt "; u32(16); u16(1); u16(quint16(channels)); u32(quint32(rate));
    u32(quint32(rate * channels * 2)); u16(quint16(channels * 2)); u16(16); h += "data"; u32(quint32(pcm.size()));
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(h + pcm);
}

#ifndef PP_AVKIT_APP
// The keys IngestManager::applyRecord (ingestmanager.cpp) and MetadataService::extractMetadataSync read.
const char* const kFormatKeys[] = {"duration", "size", "bit_rate", "format_name", "tags"};
const char* const kStreamKeys[] = {"codec_type", "codec_name", "width", "height", "avg_frame_rate", "r_frame_rate", "channels", "sample_rate"};

// MuxManager::parseFfmpegEncoders, verbatim rules (the spawn path AvKit replaces).
QJsonObject parseEncoders(const QString& text) {
    static const QRegularExpression rx("^ ([VAS])[.A-Z]{5} (\\S+)\\s+(.*)$");
    QJsonArray video, audio; QJsonObject has;
    QStringList lines = text.split('\n'); for (QString& l : lines) l.remove('\r');
    int from = 0; for (int i = 0; i < lines.size(); ++i) if (lines[i].trimmed().startsWith("------")) { from = i + 1; break; }
    for (int i = from; i < lines.size(); ++i) {
        const auto m = rx.match(lines[i]); if (!m.hasMatch() || m.captured(2) == "=") continue;
        QJsonObject e; e["name"] = m.captured(2); e["description"] = m.captured(3).trimmed();
        if (m.captured(1) == "V") video.append(e); else if (m.captured(1) == "A") audio.append(e);
        has[m.captured(2)] = true;
    }
    QJsonObject r; r["video"] = video; r["audio"] = audio; r["has"] = has; return r;
}
#endif

} // namespace

#ifndef PP_AVKIT_APP
class TestInhouseAvkit : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_ffmpeg, m_ffprobe;
    QStringList m_media;          // files compared with ffprobe (temp clips + read-only build clips)
    QString m_mp4;                // an h264/aac mp4 for the failure cases

    QString tmp(const QString& name) const { return QDir(m_tmp.path()).filePath(name); }
    bool gen(const QString& name, const QStringList& args) {
        if (m_ffmpeg.isEmpty()) return false;
        const Run r = run(m_ffmpeg, QStringList{"-v", "error", "-y"} + args + QStringList{tmp(name)});
        if (r.code != 0 || !QFileInfo(tmp(name)).isFile()) { qWarning("fixture %s not generated: %s", qPrintable(name), r.err.constData()); return false; }
        m_media << tmp(name);
        return true;
    }
    void patchBytes(const QString& path, const QByteArray& from, const QByteArray& to) {   // same length, in place
        QFile f(path); QVERIFY(f.open(QIODevice::ReadWrite));
        QByteArray all = f.readAll(); const int at = all.indexOf(from); QVERIFY2(at >= 0, "marker not found");
        all.replace(at, from.size(), to); f.seek(0); f.write(all);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_ffmpeg = findTool("ffmpeg.exe"); m_ffprobe = findTool("ffprobe.exe");
        qInfo("reference ffmpeg: %s, ffprobe: %s", qPrintable(m_ffmpeg.isEmpty() ? "(none)" : m_ffmpeg), qPrintable(m_ffprobe.isEmpty() ? "(none)" : m_ffprobe));
        writeWav(tmp("tone.wav"), 48000, 2, 1.25); m_media << tmp("tone.wav");
        const QStringList v320 = {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30000/1001"};
        const QStringList sine48 = {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"};
        if (gen("h264_aac.mp4", v320 + sine48 + QStringList{"-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2",
                                                            "-metadata", "title=Test Clip ü", "-metadata", "comment=quote \" and \\ back"}))
            m_mp4 = tmp("h264_aac.mp4");
        gen("mpeg2_ac3_480i.ts", QStringList{"-f", "lavfi", "-i", "testsrc2=size=720x480:rate=30000/1001"} + sine48 +
                                 QStringList{"-t", "2", "-vf", "interlace=scan=tff", "-c:v", "mpeg2video", "-flags", "+ilme+ildct", "-b:v", "4M", "-c:a", "ac3", "-ac", "2"});
        gen("still.png", {"-f", "lavfi", "-i", "testsrc2=size=64x48", "-frames:v", "1"});
        gen("still.jpg", {"-f", "lavfi", "-i", "testsrc2=size=64x48", "-frames:v", "1"});
        gen("still.webp", {"-f", "lavfi", "-i", "testsrc2=size=64x48", "-frames:v", "1"});
        if (gen("mono.mp3", {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "2", "-c:a", "libmp3lame", "-b:a", "128k", "-metadata", "artist=Me"}) &&
            QFileInfo(tmp("still.jpg")).isFile())
            gen("cover.mp3", {"-i", tmp("mono.mp3"), "-i", tmp("still.jpg"), "-map", "0", "-map", "1", "-c", "copy", "-disposition:v", "attached_pic"});
        {
            QFile srt(tmp("subs.srt"));
            if (srt.open(QIODevice::WriteOnly)) srt.write("1\n00:00:00,000 --> 00:00:01,000\nHello\n\n2\n00:00:01,000 --> 00:00:02,000\nWorld\n");
        }
        gen("h264_opus_sub.mkv", QStringList{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25"} + sine48 + QStringList{"-i", tmp("subs.srt"), "-t", "2",
             "-map", "0", "-map", "1", "-map", "2", "-c:v", "libx264", "-c:a", "libopus", "-ac", "2", "-c:s", "srt", "-metadata:s:a:0", "language=eng"});
        if (!m_mp4.isEmpty()) gen("rotated.mp4", {"-display_rotation", "90", "-i", m_mp4, "-c", "copy"});
        gen("timecode.mov", v320 + QStringList{"-t", "1", "-c:v", "mpeg4", "-timecode", "01:00:00:00"});
        gen("pcm_51.wav", {"-f", "lavfi", "-i", "sine=sample_rate=48000", "-t", "1", "-af", "pan=5.1|c0=c0|c1=c0|c2=c0|c3=c0|c4=c0|c5=c0", "-c:a", "pcm_s24le"});
        // a non-ASCII file name (UTF-8 path open and "filename" printing) and tags with invalid UTF-8 (print_str_validate)
        if (!m_mp4.isEmpty() && QFile::copy(m_mp4, tmp(QString::fromUtf8("clip \xc3\xbc \xe2\x82\xac.mp4")))) m_media << tmp(QString::fromUtf8("clip \xc3\xbc \xe2\x82\xac.mp4"));
        if (gen("badutf8.mp4", v320 + QStringList{"-t", "0.5", "-c:v", "libx264", "-metadata", "title=AB__CD", "-metadata", "comment=EF___GH", "-metadata", "artist=XYZ_W"})) {
            patchBytes(tmp("badutf8.mp4"), "AB__CD", QByteArray("AB\xff\xfe" "CD"));         // two invalid lead bytes
            patchBytes(tmp("badutf8.mp4"), "EF___GH", QByteArray("EF\xed\xa0\x80" "GH"));   // an encoded surrogate
            patchBytes(tmp("badutf8.mp4"), "XYZ_W", QByteArray("XY\xc3(W"));                // a lead byte without continuation
        }
        gen("faststart.mp4", v320 + sine48 + QStringList{"-t", "2", "-c:v", "libx264", "-c:a", "aac", "-movflags", "+faststart"});
        // the live tree's sample clips, read-only
        for (const char* c : {"build/clipC_720p25.mp4", "build/clipB_1080p.mp4"}) {
            const QString p = QDir(QStringLiteral(PP_SOURCE_DIR)).filePath(QString::fromLatin1(c));
            if (QFileInfo(p).isFile()) m_media << p;
        }
        qInfo("%d media fixtures", int(m_media.size()));
    }

    // ---- capabilities ------------------------------------------------------------------------------------------------
    void encodersListBroadcastCodecs() {
        for (const char* n : {"libx264", "mpeg2video", "ac3", "aac", "eac3", "mp2"}) QVERIFY2(avkit::hasEncoder(n), n);
        QVERIFY(!avkit::hasEncoder("no_such_encoder"));
        bool sawMpeg2 = false;
        for (const avkit::Encoder& e : avkit::encoders()) {
            QCOMPARE(e.flags.size(), 6); QCOMPARE(e.flags.at(0), e.type);
            if (e.name == "mpeg2video") { sawMpeg2 = true; QCOMPARE(e.type, QChar('V')); QCOMPARE(e.flags.at(2), QChar('S')); QVERIFY(!e.hardware); }
            if (e.name == "h264_qsv") QVERIFY(e.hardware);
            if (e.name == "libx264") { QCOMPARE(e.codec, QString("h264")); QVERIFY(e.description().endsWith("(codec h264)")); }
        }
        QVERIFY(sawMpeg2);
    }

    void configurationAndVersions() {
        const QString conf = avkit::configuration();
        QVERIFY2(conf.contains("--enable-libsrt"), qPrintable(conf.left(200)));
        QVERIFY(conf.contains("--enable-libx264"));
        const QJsonObject v = avkit::versions();
        QVERIFY(!v.value("ffmpeg").toString().isEmpty());
        for (const char* k : {"libavutil", "libavcodec", "libavformat", "libswscale", "libswresample", "libavfilter", "libavdevice"})
            QVERIFY2(QRegularExpression("^\\d+\\.\\d+\\.\\d+$").match(v.value(k).toString()).hasMatch(), k);
        QVERIFY(v.value("license").toString().contains("GPL"));
        QVERIFY(avkit::versionLine().startsWith("ffmpeg version " + avkit::versionInfo()));
    }

    void capabilitiesShape() {
        QElapsedTimer t; t.start();
        const QJsonObject ff = avkit::ffmpegCapabilities();
        qInfo("ffmpegCapabilities: %lld ms", t.elapsed());
        QStringList keys = ff.keys(); keys.sort();
        QCOMPARE(keys, QStringList({"audio", "build", "found", "has", "path", "version", "video"}));
        QCOMPARE(ff.value("path").toString(), QString(avkit::kInProcessPath));
        QVERIFY(ff.value("found").toBool());
        const QJsonObject has = ff.value("has").toObject();
        for (const QString& n : avkit::watchedEncoders()) QVERIFY2(has.contains(n), qPrintable(n));
        for (const char* n : {"libx264", "mpeg2video", "ac3", "aac"}) QVERIFY2(has.value(n).toBool(), n);
        const QJsonObject b = ff.value("build").toObject();
        QStringList bk = b.keys(); bk.sort();
        QCOMPARE(bk, QStringList({"ndi", "nvenc", "qsv", "rist", "srt", "x264", "x265"}));
        QVERIFY(b.value("srt").toBool()); QVERIFY(b.value("x264").toBool()); QVERIFY(b.value("qsv").toBool());
        for (const QJsonValue& e : ff.value("video").toArray()) { QVERIFY(e.toObject().contains("name")); QVERIFY(e.toObject().contains("description")); }
        QVERIFY(ff.value("video").toArray().size() > 20); QVERIFY(ff.value("audio").toArray().size() > 10);
    }

    // `ffmpeg -hide_banner -encoders` / `-version` of the same build: text identical, so MuxManager's parse of either is equal.
    void capabilitiesMatchFfmpegExe() {
        if (m_ffmpeg.isEmpty()) QSKIP("no reference ffmpeg.exe");
        const Run enc = run(m_ffmpeg, {"-hide_banner", "-encoders"});
        QCOMPARE(enc.code, 0);
        const QByteArray mine = avkit::encodersText();
        if (lf(enc.out) != mine) {
            const QList<QByteArray> a = lf(enc.out).split('\n'), b = mine.split('\n');
            for (int i = 0; i < qMax(a.size(), b.size()); ++i)
                if (a.value(i) != b.value(i)) { qWarning("line %d\n ffmpeg: %s\n avkit:  %s", i, a.value(i).constData(), b.value(i).constData()); break; }
        }
        QCOMPARE(mine, lf(enc.out));
        const QJsonObject parsed = parseEncoders(QString::fromUtf8(enc.out)), ff = avkit::ffmpegCapabilities();
        QCOMPARE(ff.value("video").toArray(), parsed.value("video").toArray());
        QCOMPARE(ff.value("audio").toArray(), parsed.value("audio").toArray());
        QJsonObject has = parsed.value("has").toObject();
        for (const QString& n : avkit::watchedEncoders()) if (!has.contains(n)) has[n] = false;
        QCOMPARE(ff.value("has").toObject(), has);
        const Run ver = run(m_ffmpeg, {"-hide_banner", "-version"});
        QCOMPARE(ver.code, 0);
        const QString vt = QString::fromUtf8(lf(ver.out));
        QVERIFY(vt.section('\n', 0, 0).startsWith("ffmpeg version " + avkit::versionInfo() + " "));
        const QString conf = vt.section("configuration:", 1).section('\n', 0, 0).trimmed();
        QCOMPARE(avkit::configuration().trimmed(), conf);
        QCOMPARE(ff.value("build").toObject(), avkit::buildFlags(vt.section("configuration:", 1).section('\n', 0, 0), parsed.value("has").toObject()));
    }

    // ---- probe vs ffprobe.exe ----------------------------------------------------------------------------------------
    void probeMatchesFfprobe_data() {
        QTest::addColumn<QString>("path");
        for (const QString& p : std::as_const(m_media)) QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }
    void probeMatchesFfprobe() {
        QFETCH(QString, path);
        QElapsedTimer t; t.start();
        const avkit::ProbeResult r = avkit::probe(path);
        const qint64 ms = t.elapsed();
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.json.value("streams").toArray().size() >= 1);
        QCOMPARE(QJsonDocument::fromJson(r.text).object(), r.json);    // the text and the object say the same
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        const Run fp = run(m_ffprobe, {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", path});
        QCOMPARE(fp.code, 0);
        const QJsonObject ref = QJsonDocument::fromJson(fp.out).object();
        QVERIFY(!ref.isEmpty());
        // every key the ingest and the app read, value for value
        const QJsonObject rf = ref.value("format").toObject(), mf = r.json.value("format").toObject();
        for (const char* k : kFormatKeys) QVERIFY2(rf.value(k) == mf.value(k), qPrintable(QString("format.%1: ffprobe %2, avkit %3").arg(QString::fromLatin1(k), js(rf.value(k)), js(mf.value(k)))));
        const QJsonArray rs = ref.value("streams").toArray(), ms2 = r.json.value("streams").toArray();
        QCOMPARE(ms2.size(), rs.size());
        for (int i = 0; i < rs.size(); ++i) {
            const QJsonObject a = rs[i].toObject(), b = ms2[i].toObject();
            for (const char* k : kStreamKeys) QVERIFY2(a.value(k) == b.value(k), qPrintable(QString("stream %1 %2 differs").arg(i).arg(k)));
            QCOMPARE(b.value("disposition").toObject().value("attached_pic"), a.value("disposition").toObject().value("attached_pic"));
        }
        // and the whole document: same keys, values, order and layout (ffprobe writes CRLF on Windows)
        if (lf(fp.out) != r.text) {
            const QList<QByteArray> a = lf(fp.out).split('\n'), b = r.text.split('\n');
            for (int i = 0; i < qMax(a.size(), b.size()); ++i)
                if (a.value(i) != b.value(i)) { qWarning("first difference, line %d\n ffprobe: %s\n avkit:   %s", i + 1, a.value(i).constData(), b.value(i).constData()); break; }
        }
        QCOMPARE(r.json, ref);
        QCOMPARE(r.text, lf(fp.out));
        qInfo("%s: in-process %lld ms", qPrintable(QFileInfo(path).fileName()), ms);
    }

    void durationFormMatchesFfprobe() {
        for (const QString& p : std::as_const(m_media)) {
            const avkit::ProbeResult r = avkit::probe(p);
            QVERIFY(r.ok);
            const QByteArray mine = avkit::formatDurationValue(r);
            if (m_ffprobe.isEmpty()) { QVERIFY(mine == "N/A" || mine.toDouble() > 0); continue; }
            const Run fp = run(m_ffprobe, {"-v", "quiet", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1", p});
            QCOMPARE(fp.code, 0);
            QCOMPARE(mine, fp.out.trimmed());
        }
    }

    // durationOnly (header first, stream info only when needed) agrees with the full probe's format.duration.
    void durationOnlyAgrees() {
        for (const QString& p : std::as_const(m_media)) {
            const avkit::ProbeResult r = avkit::probe(p);
            const double full = r.json.value("format").toObject().value("duration").toString().toDouble();
            QString err;
            const double quick = avkit::durationOnly(p, 10000, &err);
            if (full <= 0) { QCOMPARE(quick, 0.0); QVERIFY(!err.isEmpty()); continue; }
            QVERIFY2(std::abs(quick - full) < 0.0015, qPrintable(QString("%1: durationOnly %2 vs probe %3").arg(QFileInfo(p).fileName()).arg(quick).arg(full)));
            QVERIFY(err.isEmpty());
        }
        QCOMPARE(avkit::durationOnly(tmp("does-not-exist.mp4")), 0.0);
    }

    // ---- failures never take the caller down -------------------------------------------------------------------------
    void failuresAreReported() {
        avkit::ProbeResult r = avkit::probe(tmp("does-not-exist.mp4"));
        QVERIFY(!r.ok); QCOMPARE(r.averror, AVERROR(ENOENT)); QVERIFY(r.error.contains("does-not-exist.mp4")); QCOMPARE(r.text, QByteArray("{\n\n}\n"));
        QVERIFY(r.json.isEmpty());
        QVERIFY(!avkit::probe(QString()).ok);
        {   // a text file renamed to .mp4
            QFile f(tmp("notes.mp4")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QByteArray("just some text, not a movie\n").repeated(40)); f.close();
            r = avkit::probe(tmp("notes.mp4"));
            QVERIFY2(!r.ok, r.text.constData()); QVERIFY(!r.error.isEmpty());
            QString err; QVERIFY(avkit::probeJson(tmp("notes.mp4"), {}, &err).isEmpty()); QVERIFY(!err.isEmpty());
            if (!m_ffprobe.isEmpty()) { const Run fp = run(m_ffprobe, {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", tmp("notes.mp4")}); QCOMPARE(lf(fp.out), r.text); QVERIFY(fp.code != 0); }
        }
        // truncated MP4s (moov last: unreadable; moov first: readable header, missing media): refused or partial, never a
        // crash, and the same answer as ffprobe
        for (const QString& name : {QString("h264_aac.mp4"), QString("faststart.mp4")}) {
            QFile src(tmp(name)); if (!src.open(QIODevice::ReadOnly)) continue;
            const QByteArray all = src.readAll();
            for (int cut : {64, 4096, int(all.size() / 2)}) {
                QFile f(tmp(QString("cut%1_%2").arg(cut).arg(name))); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(all.left(cut)); f.close();
                r = avkit::probe(f.fileName());
                qInfo("%s truncated at %d: ok=%d %s", qPrintable(name), cut, int(r.ok), qPrintable(r.error));
                if (r.ok) QCOMPARE(QJsonDocument::fromJson(r.text).object(), r.json);
                if (!m_ffprobe.isEmpty()) {
                    const Run fp = run(m_ffprobe, {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", f.fileName()});
                    QCOMPARE(r.text, lf(fp.out)); QCOMPARE(r.ok, fp.code == 0);
                }
            }
        }
        // local files only: a URL is refused before any connection is made (port 9 on loopback would refuse anyway)
        QElapsedTimer t; t.start();
        r = avkit::probe("http://127.0.0.1:9/x.mp4");
        QVERIFY(!r.ok); QVERIFY(t.elapsed() < 2000);
        QCOMPARE(avkit::durationOnly("http://127.0.0.1:9/x.mp4"), 0.0);
    }

    void cancelFlagAborts() {
        if (m_mp4.isEmpty()) QSKIP("needs the generated mp4");
        std::atomic<bool> cancel{true};
        avkit::ProbeOptions o; o.cancel = &cancel;
        const avkit::ProbeResult r = avkit::probe(m_mp4, o);
        QVERIFY(!r.ok); QVERIFY(r.cancelled); QVERIFY(r.error.startsWith("cancelled"));
        cancel = false;
        QVERIFY(avkit::probe(m_mp4, o).ok);
    }

    void optionsSubsets() {
        if (m_mp4.isEmpty()) QSKIP("needs the generated mp4");
        avkit::ProbeOptions o; o.showStreams = false;
        avkit::ProbeResult r = avkit::probe(m_mp4, o);
        QVERIFY(r.ok); QCOMPARE(r.json.keys(), QStringList{"format"});
        o = {}; o.showFormat = false;
        r = avkit::probe(m_mp4, o);
        QVERIFY(r.ok); QCOMPARE(r.json.keys(), QStringList{"streams"});
        o = {}; o.printName = "shown.mp4";
        QCOMPARE(avkit::probe(m_mp4, o).json.value("format").toObject().value("filename").toString(), QString("shown.mp4"));
        o = {}; o.openDecoders = false;   // the keys the ingest reads do not depend on the decoders
        r = avkit::probe(m_mp4, o);
        const QJsonObject full = avkit::probeJson(m_mp4);
        QVERIFY(r.ok);
        for (const char* k : kFormatKeys) QCOMPARE(r.json.value("format").toObject().value(k), full.value("format").toObject().value(k));
        const QJsonObject v0 = r.json.value("streams").toArray().at(0).toObject(), f0 = full.value("streams").toArray().at(0).toObject();
        for (const char* k : kStreamKeys) QCOMPARE(v0.value(k), f0.value(k));
        QVERIFY(!v0.contains("is_avc")); QVERIFY(f0.contains("is_avc"));
    }

    // Many threads probing at once (the app's importer uses QtConcurrent): identical results, no crash.
    void concurrentProbes() {
        QList<QByteArray> expected;
        for (const QString& p : std::as_const(m_media)) expected << avkit::probe(p).text;
        std::atomic<int> mismatches{0}, done{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < 6; ++t)
            pool.emplace_back([&, t] {
                for (int round = 0; round < 3; ++round)
                    for (int i = 0; i < m_media.size(); ++i) {
                        const int k = (i + t) % m_media.size();
                        if (avkit::probe(m_media[k]).text != expected[k]) ++mismatches;
                        if (round == 0 && t == 0) (void)avkit::ffmpegCapabilities();
                        ++done;
                    }
            });
        for (std::thread& th : pool) th.join();
        QCOMPARE(mismatches.load(), 0);
        QCOMPARE(done.load(), 6 * 3 * int(m_media.size()));
    }
};

QTEST_GUILESS_MAIN(TestInhouseAvkit)

#else  // PP_AVKIT_APP --------------------------------------------------------------------------------------------------

// DEP-02a: services/metadataservice.cpp probes in-process through PlayPlayAvKit. Every ffprobe directory is taken off PATH
// once Qt and libav are loaded (on this box chocolatey, C:\ffmpeg\bin, conda's Library\bin and the build folder all hold
// one), which is the app on a station without ffprobe installed. The reference values come from the FFmpeg build's own
// ffprobe.exe, run by absolute path.
class TestInhouseAvkitApp : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_ffmpeg, m_ffprobe;     // reference tools (absolute paths), never looked up on PATH
    QString m_import;                // the folder the bulk importer is pointed at
    QStringList m_clips;             // its media files
    QByteArray m_path;               // PATH before stripping, restored at the end

    bool gen(const QString& name, const QStringList& args) {
        if (m_ffmpeg.isEmpty()) return false;
        const QString out = QDir(m_import).filePath(name);
        const Run r = run(m_ffmpeg, QStringList{"-v", "error", "-y"} + args + QStringList{out});
        if (r.code != 0 || !QFileInfo(out).isFile()) { qWarning("fixture %s not generated: %s", qPrintable(name), r.err.constData()); return false; }
        m_clips << out;
        return true;
    }
    QJsonObject ffprobeJson(const QString& path) const {
        const Run r = run(m_ffprobe, {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", path});
        return r.code == 0 ? QJsonDocument::fromJson(r.out).object() : QJsonObject();
    }
    // the format.duration the app should show for a clip: ffprobe's, or (without the reference tool) whatever it read
    double expectedDuration(const QString& path) const {
        return ffprobeJson(path).value("format").toObject().value("duration").toString().toDouble();
    }
    static bool isStill(const QString& path) { return path.endsWith(".png", Qt::CaseInsensitive); }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_ffmpeg = findTool("ffmpeg.exe"); m_ffprobe = findTool("ffprobe.exe");
        qInfo("reference ffmpeg: %s, ffprobe: %s", qPrintable(m_ffmpeg.isEmpty() ? "(none)" : m_ffmpeg), qPrintable(m_ffprobe.isEmpty() ? "(none)" : m_ffprobe));
        m_import = QDir(m_tmp.path()).filePath("import");
        QVERIFY(QDir().mkpath(m_import));
        writeWav(QDir(m_import).filePath("tone.wav"), 48000, 2, 1.25); m_clips << QDir(m_import).filePath("tone.wav");
        const QStringList sine48 = {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"};
        gen("news_clip.mp4", QStringList{"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30000/1001"} + sine48 +
                             QStringList{"-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", "-metadata", "title=News"});
        gen("promo_teaser.mkv", QStringList{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25"} + sine48 +
                                QStringList{"-t", "1.5", "-c:v", "libx264", "-c:a", "libopus", "-ac", "2"});
        gen("program_mpeg2.mov", QStringList{"-f", "lavfi", "-i", "testsrc2=size=720x480:rate=30000/1001"} + sine48 +
                                 QStringList{"-t", "1", "-c:v", "mpeg2video", "-b:v", "4M", "-c:a", "pcm_s16le", "-ac", "2"});
        gen("bed.mp3", {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "3", "-c:a", "libmp3lame", "-b:a", "128k"});
        gen("still.png", {"-f", "lavfi", "-i", "testsrc2=size=64x48", "-frames:v", "1"});
        qInfo("%d clips in %s", int(m_clips.size()), qPrintable(m_import));

        // Take every directory holding an ffprobe off PATH (the DLLs are loaded by now). The old spawn of "ffprobe" looks
        // there first; after an empty lookup Windows still tries the exe's own folder (see legacySwitchSpawnsFfprobe),
        // so the proof that the default path spawns nothing is the concat: case there, not this.
        m_path = qgetenv("PATH");
        QList<QByteArray> kept; QStringList removed;
        for (const QByteArray& d : m_path.split(';')) {
            if (d.trimmed().isEmpty()) continue;
            const QDir dir(QString::fromLocal8Bit(d));
            bool has = false;
            for (const char* e : {"ffprobe", "ffprobe.exe", "ffprobe.com", "ffprobe.bat", "ffprobe.cmd"}) has = has || QFileInfo(dir.filePath(e)).exists();
            if (has) removed << dir.path(); else kept << d;
        }
        qputenv("PATH", kept.join(';'));
        qunsetenv("PLAYPLAY_PROBE_ENGINE");
        qInfo("took %d ffprobe directories off PATH: %s", int(removed.size()), qPrintable(removed.join(", ")));
        QVERIFY2(QStandardPaths::findExecutable("ffprobe").isEmpty(), qPrintable(QStandardPaths::findExecutable("ffprobe")));
    }
    void cleanupTestCase() { qputenv("PATH", m_path); qunsetenv("PLAYPLAY_PROBE_ENGINE"); }

    // Every value MetadataService::extractMetadataSync fills equals what it reads from ffprobe.exe's JSON for the clip.
    void metadataWithoutFfprobe_data() {
        QTest::addColumn<QString>("path");
        for (const QString& p : std::as_const(m_clips)) QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }
    void metadataWithoutFfprobe() {
        QFETCH(QString, path);
        MetadataService svc;
        QSignalSpy errors(&svc, &MetadataService::metadataError);
        QElapsedTimer t; t.start();
        std::unique_ptr<MediaMetadata> m(svc.extractMetadata(path).result());
        const qint64 ms = t.elapsed();
        QVERIFY2(m, "no metadata without ffprobe on PATH");
        QCOMPARE(errors.count(), 0);
        QCOMPARE(m->fileSize, QFileInfo(path).size());
        if (!isStill(path)) QVERIFY(m->duration > 0);
        qInfo("%s: %lld ms, duration %.6f s, size %lld, %s %dx%d %s %d ch %d Hz", qPrintable(QFileInfo(path).fileName()), ms, m->duration,
              m->fileSize, qPrintable(m->videoCodec), m->width, m->height, qPrintable(m->audioCodec), m->audioChannels, m->audioSampleRate);
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        const QJsonObject ref = ffprobeJson(path), f = ref.value("format").toObject();
        QVERIFY(!ref.isEmpty());
        QCOMPARE(m->duration, f.value("duration").toString().toDouble());
        QCOMPARE(m->fileSize, f.value("size").toString().toLongLong());
        QCOMPARE(m->format, f.value("format_name").toString());
        QCOMPARE(m->bitrate, f.value("bit_rate").toString().toInt());
        QCOMPARE(m->tags, f.value("tags").toObject());
        QString vcodec, acodec; int w = 0, h = 0, ch = 0, rate = 0;
        for (const QJsonValue& v : ref.value("streams").toArray()) {      // the service keeps the last stream of each kind
            const QJsonObject s = v.toObject();
            if (s.value("codec_type") == "video") { vcodec = s.value("codec_name").toString(); w = s.value("width").toInt(); h = s.value("height").toInt(); }
            if (s.value("codec_type") == "audio") { acodec = s.value("codec_name").toString(); ch = s.value("channels").toInt(); rate = s.value("sample_rate").toString().toInt(); }
        }
        QCOMPARE(m->videoCodec, vcodec); QCOMPARE(m->width, w); QCOMPARE(m->height, h);
        QCOMPARE(m->audioCodec, acodec); QCOMPARE(m->audioChannels, ch); QCOMPARE(m->audioSampleRate, rate);
    }

    // enrichMediaItem (the other public entry): duration and fileSize filled; a missing file leaves the item as it was.
    void enrichFillsDurationAndSize() {
        MetadataService svc;
        for (const QString& p : std::as_const(m_clips)) {
            const QJsonObject e = svc.enrichMediaItem(QJsonObject{{"filePath", p}, {"title", "x"}}).result();
            QCOMPARE(e.value("fileSize").toVariant().toLongLong(), QFileInfo(p).size());
            QVERIFY2(e.contains("duration"), qPrintable(p));
            if (!isStill(p)) QVERIFY(e.value("duration").toDouble() > 0);
            if (!m_ffprobe.isEmpty()) QCOMPARE(e.value("duration").toDouble(), expectedDuration(p));
        }
        QSignalSpy errors(&svc, &MetadataService::metadataError);
        const QJsonObject missing{{"filePath", QDir(m_tmp.path()).filePath("gone.mp4")}, {"title", "gone"}};
        QCOMPARE(svc.enrichMediaItem(missing).result(), missing);
        QCOMPARE(errors.count(), 1);
    }

    // The importer as the operator drives it: Select Folder... (answered with the clip folder), Import. Every item carries
    // the clip's duration.
    void bulkImporterFillsDurations() {
        BulkMediaImporter w;
        w.show();
        QPushButton *selectFolder = nullptr, *import = nullptr;
        for (QPushButton* b : w.findChildren<QPushButton*>()) {
            if (b->text() == "Select Folder...") selectFolder = b;
            if (b->text() == "Import") import = b;
        }
        QLabel* status = nullptr;
        for (QLabel* l : w.findChildren<QLabel*>()) if (l->text() == "Ready") status = l;
        QVERIFY(selectFolder && import && status);

        bool answered = false;                 // the folder picker is a widget QFileDialog (AA_DontUseNativeDialogs)
        QTimer poll; poll.setInterval(20);
        connect(&poll, &QTimer::timeout, this, [&] {
            for (QWidget* tl : QApplication::topLevelWidgets())
                if (auto* d = qobject_cast<QFileDialog*>(tl); d && d->isVisible()) {
                    d->setDirectory(m_import);
                    static_cast<QDialog*>(d)->accept();     // public in QDialog, dispatches to QFileDialog::accept (protected)
                    answered = true; poll.stop(); return;
                }
        });
        poll.start();
        QTimer::singleShot(10000, &poll, [&] { poll.stop(); for (QWidget* tl : QApplication::topLevelWidgets()) if (auto* d = qobject_cast<QFileDialog*>(tl)) d->reject(); });
        selectFolder->click();
        poll.stop();
        QVERIFY2(answered, "the folder dialog never showed");
        QCOMPARE(status->text(), QString("%1 file(s) found").arg(m_clips.size()));

        QSignalSpy done(&w, &BulkMediaImporter::importComplete);
        QElapsedTimer t; t.start();
        import->click();
        const qint64 ms = t.elapsed();
        QCOMPARE(done.count(), 1);
        const QJsonArray items = done.at(0).at(0).value<QJsonArray>();
        QCOMPARE(items.size(), m_clips.size());
        QStringList clips;                     // compared canonically: the dialog's model may spell the folder differently
        for (const QString& c : std::as_const(m_clips)) clips << QFileInfo(c).canonicalFilePath();
        for (const QJsonValue& v : items) {
            const QJsonObject it = v.toObject();
            const QString p = it.value("filePath").toString();
            QVERIFY2(clips.contains(QFileInfo(p).canonicalFilePath()), qPrintable(p));
            if (!isStill(p)) QVERIFY2(it.value("duration").toDouble() > 0, qPrintable(p));
            if (!m_ffprobe.isEmpty()) QCOMPARE(it.value("duration").toDouble(), expectedDuration(p));
        }
        QCOMPARE(status->text(), QString("Imported %1 item(s)").arg(m_clips.size()));
        qInfo("imported %d clips in %lld ms without ffprobe", int(items.size()), ms);
    }

    // PLAYPLAY_PROBE_ENGINE=ffprobe still routes to the old spawn. A "concat:" URL tells the two apart: ffprobe opens it,
    // the in-process probe (local files only) refuses it.
    void legacySwitchSpawnsFfprobe() {
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        const QString wav = QDir(m_import).filePath("tone.wav");
        const QString viaConcat = "concat:" + QDir::toNativeSeparators(wav);
        const QByteArray stripped = qgetenv("PATH");
        auto restore = qScopeGuard([&] { qputenv("PATH", stripped); qunsetenv("PLAYPLAY_PROBE_ENGINE"); });
        MetadataService svc;
        {
            QSignalSpy errors(&svc, &MetadataService::metadataError);
            std::unique_ptr<MediaMetadata> m(svc.extractMetadata(viaConcat).result());
            QVERIFY2(!m, "the in-process probe opened a non-file URL");
            QCOMPARE(errors.count(), 1);
        }
        // the old engine with PATH still stripped: report what it does (measured: it still answers, Windows finding the
        // ffprobe.exe the build stages next to this exe; the app had no such guarantee where ffprobe was not staged)
        qputenv("PLAYPLAY_PROBE_ENGINE", "ffprobe");
        {
            QElapsedTimer t; t.start();
            std::unique_ptr<MediaMetadata> m(svc.extractMetadata(wav).result());
            qInfo("legacy engine without ffprobe on PATH: %s (%lld ms)", m ? "answered" : "no metadata, as the app before AvKit", t.elapsed());
        }
        // the old engine with the build's ffprobe back on PATH: answers the concat URL, so the switch really spawns it
        qputenv("PATH", QFile::encodeName(QDir::toNativeSeparators(QFileInfo(m_ffprobe).absolutePath())) + ';' + stripped);
        QVERIFY(!QStandardPaths::findExecutable("ffprobe").isEmpty());
        QElapsedTimer t; t.start();
        std::unique_ptr<MediaMetadata> legacy(svc.extractMetadata(viaConcat).result());
        QVERIFY2(legacy, "PLAYPLAY_PROBE_ENGINE=ffprobe did not spawn ffprobe");
        QVERIFY2(std::abs(legacy->duration - 1.25) < 0.01, qPrintable(QString::number(legacy->duration)));
        std::unique_ptr<MediaMetadata> viaSpawn(svc.extractMetadata(wav).result());
        const qint64 spawnMs = t.elapsed();
        qunsetenv("PLAYPLAY_PROBE_ENGINE");
        t.restart();
        std::unique_ptr<MediaMetadata> inProcess(svc.extractMetadata(wav).result());
        qInfo("two ffprobe spawns %lld ms; in-process %lld ms", spawnMs, t.elapsed());
        QVERIFY(viaSpawn && inProcess);
        QCOMPARE(inProcess->toJson(), viaSpawn->toJson());   // both engines fill the same values
    }
};

int main(int argc, char** argv)
{
    // offscreen: no window on the station desktop; widget dialogs, so the test can answer the importer's folder picker
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    TestInhouseAvkitApp tc;
    return QTest::qExec(&tc, argc, argv);
}
#endif // PP_AVKIT_APP

#include "test_inhouse_avkit.moc"
