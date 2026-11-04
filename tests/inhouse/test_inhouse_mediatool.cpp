// DEP-03: PlayPlayMediaTool.exe - `probe` (ffprobe's two forms PlayPlay spawns, byte for byte), `thumb` (the ingest's
// ffmpeg thumbnail arguments, autorotate included) and `--serve` (JSON-lines, 3 workers).
// Reference tools: ffmpeg.exe / ffprobe.exe of the same FFmpeg build (next to this test in the build tree, else
// PP_FFMPEG_BIN, else $PLAYPLAY_TEST_FFMPEG_BIN), never the ones on PATH (another FFmpeg on this box). Without them the
// fixtures shrink to a WAV written here, the comparisons skip and the contract checks still run.
#include "avprobe.h"
#include "probecli.h"
#include "serve.h"
#include "thumbnailer.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <atomic>
#include <cmath>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#ifndef PP_FFMPEG_BIN
#define PP_FFMPEG_BIN ""
#endif
#ifndef PP_MEDIATOOL_EXE
#define PP_MEDIATOOL_EXE ""
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

struct Run { int code = -1; QByteArray out, err; qint64 ms = 0; };
Run run(const QString& exe, const QStringList& args, int timeoutMs = 60000) {
    QProcess p;
    QElapsedTimer t; t.start();
    p.start(exe, args);
    Run r;
    if (!p.waitForStarted(10000)) return r;
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(2000); return r; }
    r.ms = t.elapsed();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError();
    return r;
}
QJsonObject lastJson(const QByteArray& out) {
    const QList<QByteArray> lines = out.trimmed().split('\n');
    return QJsonDocument::fromJson(lines.isEmpty() ? QByteArray() : lines.last().trimmed()).object();
}
int lineCount(const QByteArray& out) { return int(out.trimmed().split('\n').size()); }

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
    QFile f(path); if (f.open(QIODevice::WriteOnly)) f.write(h + pcm);
}
QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
bool writeBytes(const QString& path, const QByteArray& b) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(b) == b.size(); }

// The JPEG with an EXIF APP1 segment after SOI that holds only IFD0 Orientation (0x0112, SHORT): what a phone writes for
// a portrait photo (6) or one taken upside down (3). Empty when `jpeg` is not a JPEG.
QByteArray withExifOrientation(const QByteArray& jpeg, quint16 orientation) {
    if (!jpeg.startsWith("\xff\xd8")) return {};
    QByteArray tiff("II*\0", 4);
    auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); tiff.append(b, 2); };
    auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); tiff.append(b, 4); };
    u32(8); u16(1); u16(0x0112); u16(3); u32(1); u16(orientation); u16(0); u32(0);   // IFD0: 1 entry, no next IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    const int len = int(payload.size()) + 2;
    return jpeg.left(2) + QByteArray("\xff\xe1", 2) + char(len >> 8) + char(len & 0xff) + payload + jpeg.mid(2);
}

// The first picture of an image / video file as 8-bit luma, decoded with libav (no Qt image plugin needed).
struct Gray { int w = 0, h = 0; QByteArray y; QString pixFmt; };
Gray decodeGray(const QString& path) {
    Gray g;
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) return g;
    auto closeFc = qScopeGuard([&] { avformat_close_input(&fc); });
    if (avformat_find_stream_info(fc, nullptr) < 0) return g;
    const AVCodec* dec = nullptr;
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (vs < 0 || !dec) return g;
    AVCodecContext* cc = avcodec_alloc_context3(dec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    auto freeAll = qScopeGuard([&] { avcodec_free_context(&cc); av_packet_free(&pkt); av_frame_free(&fr); });
    if (avcodec_parameters_to_context(cc, fc->streams[vs]->codecpar) < 0 || avcodec_open2(cc, dec, nullptr) < 0) return g;
    bool got = false;
    while (!got && av_read_frame(fc, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, fr) >= 0) got = true;
        av_packet_unref(pkt);
    }
    if (!got && avcodec_send_packet(cc, nullptr) >= 0) got = avcodec_receive_frame(cc, fr) >= 0;
    if (!got) return g;
    SwsContext* sws = sws_getContext(fr->width, fr->height, AVPixelFormat(fr->format), fr->width, fr->height, AV_PIX_FMT_GRAY8, SWS_POINT, nullptr, nullptr, nullptr);
    if (!sws) return g;
    g.y.resize(fr->width * fr->height);
    uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(g.y.data()), nullptr, nullptr, nullptr};
    int ls[4] = {fr->width, 0, 0, 0};
    sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, ls);
    sws_freeContext(sws);
    g.w = fr->width; g.h = fr->height;
    g.pixFmt = QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(fr->format)));
    return g;
}
double psnr(const Gray& a, const Gray& b) {
    if (a.w != b.w || a.h != b.h || a.y.isEmpty()) return 0.0;
    double se = 0;
    for (int i = 0; i < a.y.size(); ++i) { const double d = double(uchar(a.y[i])) - double(uchar(b.y[i])); se += d * d; }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / a.y.size()));
}

// IngestManager::applyRecord's thumbnail step, verbatim: stills at 0, video at 10 % (at most 0.5 s before the end).
const QStringList kImageExts = {"png", "jpg", "jpeg", "bmp", "gif", "webp"};
double ingestAt(const QString& path, double durationSec) {
    return kImageExts.contains(QFileInfo(path).suffix().toLower()) ? 0.0 : qMax(0.0, qMin(durationSec * 0.1, durationSec - 0.5));
}
double asPassed(double at) { return QString::number(at, 'f', 2).toDouble(); }   // the -ss value the ingest really passes
QStringList ingestThumbArgs(const QString& in, const QString& out, double at) {
    QStringList args = {"-y", "-v", "quiet"};
    if (at > 0) args << "-ss" << QString::number(at, 'f', 2);
    args << "-i" << in << "-frames:v" << "1" << "-vf" << "scale=320:-2" << "-q:v" << "4" << out;
    return args;
}
const QStringList kJsonForm = {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams"};          // ingestmanager.cpp
const QStringList kDurationForm = {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1:nk=1"};  // renderfarm.cpp

// A --serve child: JSON lines in, replies collected by id.
struct ServeChild {
    QProcess p;
    QByteArray buf;
    QHash<QString, QJsonObject> replies;     // by compact id
    QList<QJsonObject> unmatched;            // replies with a null id (bad request lines)
    bool start(const QStringList& args = {}) {
        p.setProgram(QStringLiteral(PP_MEDIATOOL_EXE));
        p.setArguments(QStringList{"--serve"} + args);
        p.start();
        return p.waitForStarted(10000);
    }
    void sendRaw(const QByteArray& line) { p.write(line + '\n'); }
    void send(const QJsonObject& o) { sendRaw(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
    static QString key(const QJsonValue& id) { return QString::fromUtf8(QJsonDocument(QJsonArray{id}).toJson(QJsonDocument::Compact)); }
    void pump(int waitMs) {
        if (p.bytesToWrite() > 0) p.waitForBytesWritten(waitMs); else p.waitForReadyRead(waitMs);
        buf += p.readAllStandardOutput();
        int nl;
        while ((nl = buf.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buf.left(nl)).object();
            buf.remove(0, nl + 1);
            if (!o.contains("ok")) continue;                    // progress events (none for these ops)
            if (o.value("id").isNull()) unmatched << o; else replies.insert(key(o.value("id")), o);
        }
    }
    bool waitFor(int count, int ms) {
        QElapsedTimer t; t.start();
        while (replies.size() < count && t.elapsed() < ms) {
            pump(50);
            if (p.state() == QProcess::NotRunning) { pump(0); break; }
        }
        return replies.size() >= count;
    }
    QJsonObject call(const QJsonObject& req, int ms = 30000) {
        const QString k = key(req.value("id"));
        send(req);
        QElapsedTimer t; t.start();
        while (!replies.contains(k) && t.elapsed() < ms && p.state() != QProcess::NotRunning) pump(50);
        pump(0);
        return replies.take(k);
    }
    ~ServeChild() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } }   // only the child this test started
};

} // namespace

class TestInhouseMediatool : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_tool, m_ffmpeg, m_ffprobe;
    QStringList m_media;                         // probe-able media (temp clips + read-only build clips)
    QStringList m_video;                         // files the ingest would thumbnail (a non-attached video stream or a still)
    QStringList m_bad;                           // inputs that cannot be probed
    QString m_mp4, m_trunc, m_text, m_wav, m_missing;
    QHash<QString, QByteArray> m_refJson;        // path -> ffprobe (else tool) stdout of the JSON form
    QHash<QString, QByteArray> m_refDuration;    // path -> stdout of the duration form
    QHash<QString, QByteArray> m_thumbs;         // path -> the one-shot tool JPEG (ingest form)
    QHash<QString, QString> m_autorotate;        // path -> the filters ffmpeg's autorotate rule picks for it
    QSet<QString> m_portrait;                    // thumbnail inputs that come out portrait (turned 90 / 270 degrees)
    QStringList m_rotFallback;                   // rotations the tool does not reproduce: exit 64
    qint64 m_ffprobeMs = 0, m_toolMs = 0; int m_spawns = 0;

    QString tmp(const QString& name) const { return QDir(m_tmp.path()).filePath(name); }
    bool gen(const QString& name, const QStringList& args, bool video = true) {
        if (m_ffmpeg.isEmpty()) return false;
        const Run r = run(m_ffmpeg, QStringList{"-v", "error", "-y"} + args + QStringList{tmp(name)});
        if (r.code != 0 || !QFileInfo(tmp(name)).isFile()) { qWarning("fixture %s not generated: %s", qPrintable(name), r.err.constData()); return false; }
        m_media << tmp(name);
        if (video) m_video << tmp(name);
        return true;
    }
    Run tool(const QStringList& args, int timeoutMs = 60000) { return run(m_tool, args, timeoutMs); }
    double durationOf(const QString& path) { return avkit::probeJson(path).value("format").toObject().value("duration").toString().toDouble(); }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        m_tool = QStringLiteral(PP_MEDIATOOL_EXE);
        QVERIFY2(QFileInfo(m_tool).isFile(), qPrintable("PlayPlayMediaTool not built: " + m_tool));
        m_ffmpeg = findTool("ffmpeg.exe"); m_ffprobe = findTool("ffprobe.exe");
        qInfo("tool: %s\nreference ffmpeg: %s, ffprobe: %s", qPrintable(m_tool), qPrintable(m_ffmpeg.isEmpty() ? "(none)" : m_ffmpeg), qPrintable(m_ffprobe.isEmpty() ? "(none)" : m_ffprobe));

        m_wav = tmp("tone.wav"); writeWav(m_wav, 48000, 2, 1.25); m_media << m_wav;
        const QStringList v320 = {"-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30000/1001"};
        const QStringList sine48 = {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"};
        if (gen("h264_aac.mp4", v320 + sine48 + QStringList{"-t", "3", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ac", "2", "-metadata", "title=Test Clip \xc3\xbc"}))
            m_mp4 = tmp("h264_aac.mp4");
        gen("hd720.mp4", QStringList{"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=25"} + sine48 + QStringList{"-t", "4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac"});
        gen("mpeg2_ac3_480i.ts", QStringList{"-f", "lavfi", "-i", "testsrc2=size=720x480:rate=30000/1001"} + sine48 +
                                 QStringList{"-t", "3", "-vf", "interlace=scan=tff", "-c:v", "mpeg2video", "-flags", "+ilme+ildct", "-b:v", "4M", "-c:a", "ac3", "-ac", "2"});
        gen("h264_opus.mkv", QStringList{"-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25"} + sine48 + QStringList{"-t", "2", "-c:v", "libx264", "-c:a", "libopus", "-ac", "2"});
        gen("yuv422p10.mkv", {"-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=25", "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv422p10le"});
        gen("anamorphic.mp4", {"-f", "lavfi", "-i", "testsrc2=size=1440x1080:rate=25", "-t", "2", "-vf", "setsar=4/3", "-c:v", "libx264"});
        gen("mpeg4.avi", v320 + QStringList{"-t", "2", "-c:v", "mpeg4"});
        for (const char* s : {"still.png", "still.jpg", "still.webp", "still.bmp", "still.gif"})
            gen(QString::fromLatin1(s), {"-f", "lavfi", "-i", "testsrc2=size=64x48", "-frames:v", "1"});
        if (gen("mono.mp3", {"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-t", "2", "-c:a", "libmp3lame", "-b:a", "128k"}, false) && QFileInfo(tmp("still.jpg")).isFile())
            gen("cover.mp3", {"-i", tmp("mono.mp3"), "-i", tmp("still.jpg"), "-map", "0", "-map", "1", "-c", "copy", "-disposition:v", "attached_pic"}, false);
        if (!m_mp4.isEmpty()) {
            const QString u = tmp(QString::fromUtf8("clip \xc3\xbc \xe2\x82\xac.mp4"));                 // non-ASCII path
            if (QFile::copy(m_mp4, u)) { m_media << u; m_video << u; }
            gen("faststart.mp4", v320 + sine48 + QStringList{"-t", "3", "-c:v", "libx264", "-c:a", "aac", "-movflags", "+faststart"});
        }
        // autorotate: phone-video display matrices (remuxed with ffmpeg's -display_* input options) and JPEG EXIF
        // orientations, each with the filters ffmpeg_filter.c's rule inserts for it
        if (QFileInfo(tmp("hd720.mp4")).isFile()) {
            const struct { const char* name; QStringList opts; const char* filters; bool portrait; } rot[] = {
                {"rot90.mp4", {"-display_rotation:v:0", "90"}, "transpose=cclock", true},
                {"rot270.mp4", {"-display_rotation:v:0", "270"}, "transpose=clock", true},
                {"rot270_hflip.mp4", {"-display_rotation:v:0", "270", "-display_hflip:v:0"}, "transpose=cclock_flip", true},
                {"rot90_hflip.mp4", {"-display_rotation:v:0", "90", "-display_hflip:v:0"}, "transpose=clock_flip", true},
                {"rot180.mp4", {"-display_rotation:v:0", "180"}, "hflip,vflip", false},
                {"vflip.mp4", {"-display_vflip:v:0"}, "vflip", false}};
            for (const auto& x : rot)
                if (gen(QString::fromLatin1(x.name), x.opts + QStringList{"-i", tmp("hd720.mp4"), "-c", "copy"})) {
                    m_autorotate.insert(tmp(x.name), QString::fromLatin1(x.filters));
                    if (x.portrait) m_portrait << tmp(x.name);
                }
            // not reproduced (exit 64): an angle ffmpeg turns with its rotate filter; a 4:2:2 picture turned 90 degrees,
            // which ffmpeg converts to 4:4:4 before the transpose
            if (gen("rot45.mp4", {"-display_rotation:v:0", "45", "-i", tmp("hd720.mp4"), "-c", "copy"}, false)) m_rotFallback << tmp("rot45.mp4");
            if (QFileInfo(tmp("yuv422p10.mkv")).isFile() && gen("rot90_422p10.mp4", {"-display_rotation:v:0", "90", "-i", tmp("yuv422p10.mkv"), "-c", "copy"}, false))
                m_rotFallback << tmp("rot90_422p10.mp4");
        }
        if (gen("photo.jpg", {"-f", "lavfi", "-i", "testsrc2=size=640x360", "-frames:v", "1"})) {
            const QByteArray photo = readAll(tmp("photo.jpg"));
            const struct { const char* name; quint16 orientation; const char* filters; bool portrait; } exif[] = {
                {"exif6_portrait.jpg", 6, "transpose=clock", true}, {"exif3_upside_down.jpg", 3, "hflip,vflip", false}};
            for (const auto& x : exif)
                if (writeBytes(tmp(x.name), withExifOrientation(photo, x.orientation))) {
                    m_media << tmp(x.name); m_video << tmp(x.name);
                    m_autorotate.insert(tmp(x.name), QString::fromLatin1(x.filters));
                    if (x.portrait) m_portrait << tmp(x.name);
                }
        }
        for (const char* c : {"build/clipC_720p25.mp4", "build/clipB_1080p.mp4"}) {                    // the live tree's samples, read-only
            const QString p = QDir(QStringLiteral(PP_SOURCE_DIR)).filePath(QString::fromLatin1(c));
            if (QFileInfo(p).isFile()) { m_media << p; m_video << p; }
        }
        // inputs that cannot be probed: a truncated moov-last MP4, a text file renamed .mp4, an empty file, a missing one
        if (!m_mp4.isEmpty()) {
            const QByteArray all = readAll(m_mp4);
            m_trunc = tmp("truncated_half.mp4"); writeBytes(m_trunc, all.left(all.size() / 2)); m_bad << m_trunc;
            writeBytes(tmp("truncated_64.mp4"), all.left(64)); m_bad << tmp("truncated_64.mp4");
        }
        if (QFileInfo(tmp("faststart.mp4")).isFile()) {   // moov first: the probe may succeed on what is left (ffprobe decides)
            const QByteArray all = readAll(tmp("faststart.mp4"));
            writeBytes(tmp("truncated_faststart.mp4"), all.left(all.size() / 2)); m_bad << tmp("truncated_faststart.mp4");
        }
        m_text = tmp("renamed_text.mp4"); writeBytes(m_text, "This is a text file that was renamed to .mp4\r\nline 2\r\n"); m_bad << m_text;
        writeBytes(tmp("empty.mp4"), {}); m_bad << tmp("empty.mp4");
        m_missing = tmp("missing.mp4"); m_bad << m_missing;
        qInfo("%d media fixtures, %d thumbnail inputs, %d bad inputs", int(m_media.size()), int(m_video.size()), int(m_bad.size()));
    }

    // ---- in-process: parsers and the size rule ------------------------------------------------------------------------
    void parsersAndScaleRule() {
        int w = 0, h = 0;
        struct { int iw, ih, rw, rh, ew, eh; } cases[] = {
            {1920, 1080, 320, -2, 320, 180}, {1280, 720, 320, -2, 320, 180}, {720, 480, 320, -2, 320, 214},   // 106.67 -> 107 * 2
            {320, 240, 320, -2, 320, 240}, {64, 48, 320, -2, 320, 240}, {1440, 1080, 320, -2, 320, 240},
            {333, 187, 320, -2, 320, 180}, {333, 187, 320, -1, 320, 180}, {1920, 1080, -2, 180, 320, 180},
            {1920, 1080, -1, -1, 1920, 1080}, {1920, 1080, -2, -2, 1920, 1080}, {720, 576, 0, 288, 720, 288}, {1920, 1080, 321, -4, 321, 180}};
        for (const auto& c : cases) {
            QVERIFY2(mediatool::scaleSize(c.iw, c.ih, c.rw, c.rh, &w, &h), qPrintable(QString("%1x%2 %3:%4").arg(c.iw).arg(c.ih).arg(c.rw).arg(c.rh)));
            QCOMPARE(w, c.ew); QCOMPARE(h, c.eh);
        }
        QVERIFY(!mediatool::scaleSize(0, 1080, 320, -2, &w, &h));

        for (const char* p : {"C:\\media\\a.mp4", "\\\\server\\share\\a.mp4", "file:C:/a.mp4", "rel/a.mp4", "clip,v2.mp4", "a:b.mp4"})
            QVERIFY2(!mediatool::isNonFileInput(QString::fromLatin1(p)), p);
        for (const char* p : {"http://127.0.0.1/a.mp4", "concat:a.mp4|b.mp4", "pipe:0", "-", "subfile,,start,0,end,9,,:a.mp4", "srt://x:1"})
            QVERIFY2(mediatool::isNonFileInput(QString::fromLatin1(p)), p);
        int lvl = 0;
        QVERIFY(mediatool::parseLogLevel("quiet", &lvl) && lvl == AV_LOG_QUIET);
        QVERIFY(mediatool::parseLogLevel("error", &lvl) && lvl == AV_LOG_ERROR);
        QVERIFY(mediatool::parseLogLevel("24", &lvl) && lvl == 24);
        QVERIFY(!mediatool::parseLogLevel("loud", &lvl));

        mediatool::ProbeCommand pc = mediatool::parseProbeArgs(kJsonForm + QStringList{"x.mp4"});
        QVERIFY(pc.unsupported.isEmpty() && pc.error.isEmpty());
        QCOMPARE(int(pc.kind), int(mediatool::ProbeCommand::Json)); QVERIFY(pc.showFormat && pc.showStreams); QCOMPARE(pc.logLevel, int(AV_LOG_QUIET));
        pc = mediatool::parseProbeArgs(kDurationForm + QStringList{"x.mp4"});
        QCOMPARE(int(pc.kind), int(mediatool::ProbeCommand::Duration)); QVERIFY(pc.noWrappers && pc.noKey); QCOMPARE(pc.input, QString("x.mp4"));
        QVERIFY(!mediatool::parseProbeArgs({"-show_packets", "x.mp4"}).unsupported.isEmpty());
        QVERIFY(!mediatool::parseProbeArgs({"-print_format", "json", "-show_format", "a", "b"}).error.isEmpty());
        QVERIFY(!mediatool::parseProbeArgs({"-print_format", "json", "-show_format"}).error.isEmpty());

        mediatool::ThumbCommand tc = mediatool::parseThumbArgs(ingestThumbArgs("in.mp4", "out.jpg", 1.234));
        QVERIFY2(tc.unsupported.isEmpty() && tc.error.isEmpty(), qPrintable(tc.unsupported + tc.error));
        QCOMPARE(tc.in, QString("in.mp4")); QCOMPARE(tc.out, QString("out.jpg"));
        QCOMPARE(tc.opt.seekSec, 1.23); QCOMPARE(tc.opt.width, 320); QCOMPARE(tc.opt.height, -2); QCOMPARE(tc.opt.qscale, 4.0);
        QVERIFY(tc.opt.overwrite); QCOMPARE(tc.logLevel, int(AV_LOG_QUIET));
        tc = mediatool::parseThumbArgs({"-i", "in.mp4", "-ss", "00:01:02.5", "-frames:v", "1", "-q:v", "2", "o.jpeg"});
        QVERIFY(tc.unsupported.isEmpty() && tc.error.isEmpty()); QCOMPARE(tc.opt.seekSec, 62.5); QVERIFY(!tc.opt.overwrite);
        QCOMPARE(tc.opt.width, -1); QCOMPARE(tc.opt.height, -1);                              // no -vf: the input size
        for (const QStringList& bad : QList<QStringList>{
                 {"-y", "-i", "in.mp4", "-frames:v", "1", "-vf", "crop=10:10", "-q:v", "4", "o.jpg"},
                 {"-y", "-i", "in.mp4", "-frames:v", "2", "-q:v", "4", "o.jpg"},
                 {"-y", "-i", "in.mp4", "-frames:v", "1", "-q:v", "4", "o.png"},
                 {"-y", "-i", "in.mp4", "-frames:v", "1", "o.jpg"},
                 {"-y", "-i", "a.mp4", "-i", "b.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 {"-y", "-i", "http://x/a.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 {"-y", "-i", "in.mp4", "-c:v", "libx264", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 // option positions ffmpeg reads differently: two seeks, output options before -i (ffmpeg refuses
                 // them), per-file options after the output (ffmpeg ignores them)
                 {"-y", "-ss", "1", "-i", "in.mp4", "-ss", "2", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 {"-y", "-frames:v", "1", "-i", "in.mp4", "-q:v", "4", "o.jpg"},
                 {"-y", "-vf", "scale=320:-2", "-i", "in.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 {"-y", "-q:v", "4", "-i", "in.mp4", "-frames:v", "1", "o.jpg"},
                 {"-y", "-c:v", "mjpeg", "-i", "in.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg"},
                 {"-y", "-i", "in.mp4", "-frames:v", "1", "o.jpg", "-q:v", "4"},
                 {"-y", "-i", "in.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg", "-ss", "1"}})
            QVERIFY2(!mediatool::parseThumbArgs(bad).unsupported.isEmpty(), qPrintable(bad.join(' ')));
        // global options are taken anywhere, also after the output
        tc = mediatool::parseThumbArgs({"-i", "in.mp4", "-frames:v", "1", "-q:v", "4", "o.jpg", "-y", "-v", "error"});
        QVERIFY2(tc.unsupported.isEmpty() && tc.error.isEmpty(), qPrintable(tc.unsupported + tc.error));
        QVERIFY(tc.opt.overwrite); QCOMPARE(tc.logLevel, int(AV_LOG_ERROR));
    }

    // ---- probe: the ingest's JSON form, byte for byte against ffprobe.exe -----------------------------------------------
    void probeJsonMatchesFfprobe_data() {
        QTest::addColumn<QString>("path");
        for (const QString& p : m_media + m_bad) QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }
    void probeJsonMatchesFfprobe() {
        QFETCH(QString, path);
        const Run mine = tool(QStringList{"probe"} + kJsonForm + QStringList{path});
        QVERIFY2(mine.code == 0 || mine.code == 1, mine.err.constData());
        QVERIFY2(mine.err.isEmpty(), mine.err.constData());                                  // -v quiet
        m_refJson.insert(path, mine.out);
        if (mine.code != 0) QCOMPARE(mine.out, QByteArray("{\r\n\r\n}\r\n"));
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        const Run ref = run(m_ffprobe, kJsonForm + QStringList{path});
        m_ffprobeMs += ref.ms; m_toolMs += mine.ms; ++m_spawns;
        QCOMPARE(mine.code, ref.code);
        if (mine.out != ref.out) {
            const QList<QByteArray> a = ref.out.split('\n'), b = mine.out.split('\n');
            for (int i = 0; i < qMax(a.size(), b.size()); ++i)
                if (a.value(i) != b.value(i)) { qWarning("line %d\n ffprobe: %s\n tool:    %s", i, a.value(i).constData(), b.value(i).constData()); break; }
        }
        QCOMPARE(mine.out, ref.out);                                                           // byte for byte, CRLF included
        m_refJson.insert(path, ref.out);
        // and key by key what IngestManager::applyRecord reads (the whole object is equal already; this names the keys)
        const QJsonObject a = QJsonDocument::fromJson(mine.out).object(), b = QJsonDocument::fromJson(ref.out).object();
        QCOMPARE(a, b);
        for (const char* k : {"filename", "format_name", "duration", "size", "bit_rate", "tags"})
            QCOMPARE(a.value("format").toObject().value(k), b.value("format").toObject().value(k));
        const QJsonArray sa = a.value("streams").toArray(), sb = b.value("streams").toArray();
        QCOMPARE(sa.size(), sb.size());
        for (int i = 0; i < sa.size(); ++i)
            for (const char* k : {"index", "codec_type", "codec_name", "width", "height", "avg_frame_rate", "r_frame_rate", "channels", "sample_rate", "disposition"})
                QCOMPARE(sa[i].toObject().value(k), sb[i].toObject().value(k));
    }

    // ---- probe: the render farm's duration form ---------------------------------------------------------------------
    void durationFormMatchesFfprobe_data() { probeJsonMatchesFfprobe_data(); }
    void durationFormMatchesFfprobe() {
        QFETCH(QString, path);
        const Run mine = tool(QStringList{"probe"} + kDurationForm + QStringList{path});
        QVERIFY(mine.code == 0 || mine.code == 1);
        m_refDuration.insert(path, mine.out);
        if (mine.code == 0) {
            QVERIFY2(mine.out.endsWith("\r\n") && lineCount(mine.out) == 1, mine.out.constData());
            QVERIFY(mine.out.trimmed() == "N/A" || mine.out.trimmed().toDouble() > 0);
        } else {
            QVERIFY(mine.out.isEmpty());
            QVERIFY2(mine.err.contains(QFileInfo(path).fileName().toUtf8() + ": ") || mine.err.contains(path.toUtf8() + ": "), mine.err.constData());   // ffprobe's print_error line
        }
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        const Run ref = run(m_ffprobe, kDurationForm + QStringList{path});
        QCOMPARE(mine.code, ref.code);
        QCOMPARE(mine.out, ref.out);
        // RenderFarm::probeDuration's parse of either
        QCOMPARE(QString::fromUtf8(mine.out).trimmed().toDouble(), QString::fromUtf8(ref.out).trimmed().toDouble());
    }

    // The other spellings both forms accept print the same bytes as ffprobe.
    void probeVariantsMatchFfprobe() {
        if (m_ffprobe.isEmpty()) QSKIP("no reference ffprobe.exe");
        QStringList files;
        for (const QString& f : {m_mp4, tmp("mpeg2_ac3_480i.ts"), tmp("still.png"), m_wav, m_missing}) if (!f.isEmpty() && (QFileInfo(f).isFile() || f == m_missing)) files << f;
        const QList<QStringList> variants = {
            {"-v", "quiet", "-print_format", "json", "-show_format"},
            {"-v", "quiet", "-print_format", "json", "-show_streams"},
            {"-v", "quiet", "-of", "json", "-show_streams", "-show_format"},
            {"-hide_banner", "-loglevel", "quiet", "-print_format", "json", "-show_format", "-show_streams", "-i"},
            {"-v", "error", "-show_entries", "format=duration", "-of", "default=nk=1:nw=1"},
            {"-v", "error", "-show_entries", "format=duration", "-of", "default=noprint_wrappers=1:nokey=1"},
            {"-v", "error", "-show_entries", "format=duration", "-of", "default=nw=1"},
            {"-v", "error", "-show_entries", "format=duration", "-of", "default=nk=1"},
            {"-v", "error", "-show_entries", "format=duration", "-of", "default"},
            {"-v", "quiet", "-show_entries", "format=duration"},
        };
        int compared = 0;
        for (const QString& f : files)
            for (const QStringList& v : variants) {
                const Run mine = tool(QStringList{"probe"} + v + QStringList{f}), ref = run(m_ffprobe, v + QStringList{f});
                const QString what = v.join(' ') + " " + QFileInfo(f).fileName();
                QVERIFY2(mine.code == ref.code, qPrintable(what));
                QVERIFY2(mine.out == ref.out, qPrintable(what + "\n tool: " + QString::fromUtf8(mine.out.left(300)) + "\n ffprobe: " + QString::fromUtf8(ref.out.left(300))));
                ++compared;
            }
        qInfo("%d variant runs byte-identical to ffprobe.exe", compared);
    }

    void probeFailuresKeepTheContract() {
        for (const QString& f : {m_missing, m_text, m_trunc}) {
            if (f.isEmpty()) continue;
            Run r = tool(QStringList{"probe"} + kJsonForm + QStringList{f});
            QCOMPARE(r.code, 1); QCOMPARE(r.out, QByteArray("{\r\n\r\n}\r\n")); QVERIFY(r.err.isEmpty());
            r = tool(QStringList{"probe"} + kDurationForm + QStringList{f});
            QCOMPARE(r.code, 1); QVERIFY(r.out.isEmpty()); QVERIFY2(r.err.contains(f.toUtf8() + ": "), r.err.constData());
        }
        // what IngestManager::onProbeFinished makes of it: exit 1 with an empty object -> "ffprobe failed", no record
        const Run r = tool(QStringList{"probe"} + kJsonForm + QStringList{m_text});
        QVERIFY(r.code != 0 && QJsonDocument::fromJson(r.out).object().isEmpty());
    }

    // ---- the in-house contract for what the tool does not take --------------------------------------------------------
    void unsupportedArgumentsExit64() {
        const QString f = m_mp4.isEmpty() ? m_wav : m_mp4;
        const QList<QStringList> unsupported = {
            {"probe", "-v", "quiet", "-print_format", "json", "-show_packets", f},
            {"probe", "-count_frames", "-print_format", "json", "-show_streams", f},
            {"probe", "-print_format", "xml", "-show_format", f},
            {"probe", "-show_format", f},                                                        // the default writer for full sections
            {"probe", "-v", "error", "-show_entries", "stream=codec_name", "-of", "default=nw=1:nk=1", f},
            {"probe", "-print_format", "json", "-show_entries", "format=duration", f},
            {"probe", "-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", "http://127.0.0.1:9/a.mp4"},
            {"probe", "-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", "concat:" + f + "|" + f},
            {"thumb", "-y", "-i", f, "-frames:v", "1", "-vf", "crop=10:10", "-q:v", "4", tmp("u1.jpg")},
            {"thumb", "-y", "-i", f, "-frames:v", "1", "-vf", "scale=320:-2", "-q:v", "4", tmp("u2.png")},
            {"thumb", "-y", "-i", f, "-frames:v", "1", "-vf", "scale=320:-2", tmp("u3.jpg")},
            {"-y", "-i", f, "-filter_complex", "[0:v]null[v]", "-map", "[v]", "-tune", "film", tmp("u4.mp4")},   // ffmpeg mode: outside the runner's subset (DEP-05)
            {"-y", "-i", f, "-c", "copy", "-f", "matroska", tmp("u5.mkv")},
            {"bogus-command"},
            {"version", "--extra"},
            {"--serve", "--bogus"},
        };
        for (const QStringList& a : unsupported) {
            const Run r = tool(a);
            const QJsonObject j = lastJson(r.out);
            QVERIFY2(r.code == 64, qPrintable(a.join(' ') + " -> exit " + QString::number(r.code)));
            QVERIFY2(j.value("unsupported").toBool() && !j.value("ok").toBool(true), qPrintable(a.join(' ') + " -> " + QString::fromUtf8(r.out)));
            QVERIFY(j.value("error").toString().startsWith("unsupported: "));
            QCOMPARE(lineCount(r.out), 1);
        }
        for (const char* u : {"u1.jpg", "u2.png", "u3.jpg", "u4.mp4", "u5.mp4"}) QVERIFY(!QFileInfo::exists(tmp(QString::fromLatin1(u))));
        const QList<QStringList> usage = {
            {"probe", "-v", "quiet", "-print_format", "json", "-show_format"},                     // no input
            {"probe", "-print_format", "json", "-show_format", f, f},                             // two inputs
            {"probe", "-v", "loud", "-print_format", "json", "-show_format", f},
            {"probe", "-print_format"},
            {"thumb", "-y", "-ss", "abc", "-i", f, "-frames:v", "1", "-q:v", "4", tmp("u6.jpg")},
            {"thumb", "-y", "-n", "-i", f, "-frames:v", "1", "-q:v", "4", tmp("u6.jpg")},
            {"--serve", "--workers", "0"},
            {},
        };
        for (const QStringList& a : usage) {
            const Run r = tool(a);
            QVERIFY2(r.code == 2, qPrintable(a.join(' ') + " -> exit " + QString::number(r.code)));
            const QJsonObject j = lastJson(r.out);
            QVERIFY2(j.contains("ok") && !j.value("ok").toBool() && !j.value("unsupported").toBool(), qPrintable(a.join(' ') + " -> " + QString::fromUtf8(r.out)));
        }
        const Run v = tool({"version"});
        QCOMPARE(v.code, 0);
        const QJsonObject vj = lastJson(v.out);
        QVERIFY(vj.value("ok").toBool()); QCOMPARE(vj.value("tool").toString(), QString("PlayPlayMediaTool"));
        QCOMPARE(vj.value("commands").toArray(), QJsonArray({"probe", "thumb", "serve", "version", "parse"}));   // parse: DEP-05a
        QVERIFY(!vj.value("ffmpeg").toString().isEmpty());
        const Run h = tool({"--help"});
        QCOMPARE(h.code, 0); QVERIFY(h.out.startsWith("usage: PlayPlayMediaTool"));
    }

    // ---- thumb: the ingest's arguments ------------------------------------------------------------------------------
    void thumbIngestForm_data() {
        QTest::addColumn<QString>("path");
        for (const QString& p : m_video) QTest::newRow(qPrintable(QFileInfo(p).fileName())) << p;
    }
    void thumbIngestForm() {
        QFETCH(QString, path);
        const QJsonObject probe = avkit::probeJson(path);
        int sw = 0, sh = 0;
        for (const QJsonValue& s : probe.value("streams").toArray()) {
            const QJsonObject o = s.toObject();
            if (o.value("codec_type").toString() == "video" && o.value("disposition").toObject().value("attached_pic").toInt() == 0) { sw = o.value("width").toInt(); sh = o.value("height").toInt(); break; }
        }
        QVERIFY(sw > 0 && sh > 0);
        const bool portrait = m_portrait.contains(path);                                       // autorotate turns it first
        int ew = 0, eh = 0; QVERIFY(mediatool::scaleSize(portrait ? sh : sw, portrait ? sw : sh, 320, -2, &ew, &eh));
        const double at = ingestAt(path, durationOf(path));
        const QString out = tmp("thumb_" + QFileInfo(path).completeBaseName() + ".jpg");
        const Run r = tool(QStringList{"thumb"} + ingestThumbArgs(path, out, at));
        QVERIFY2(r.code == 0, r.out.constData());
        QVERIFY2(r.err.isEmpty(), r.err.constData());                                          // -v quiet
        QCOMPARE(lineCount(r.out), 1);
        const QJsonObject j = lastJson(r.out);
        QVERIFY(j.value("ok").toBool());
        QCOMPARE(j.value("width").toInt(), 320); QCOMPARE(j.value("height").toInt(), eh);
        QCOMPARE(j.value("srcWidth").toInt(), sw); QCOMPARE(j.value("srcHeight").toInt(), sh);
        QCOMPARE(j.value("autorotate").toString(), m_autorotate.value(path));                  // absent when none applies
        QCOMPARE(QDir::fromNativeSeparators(j.value("out").toString()), QFileInfo(out).absoluteFilePath());
        QVERIFY(j.value("frameTime").toDouble() >= asPassed(at) - 1e-6);                       // the first frame at or after -ss
        const Gray g = decodeGray(out);                                                        // it decodes, and at 320 wide
        QCOMPARE(g.w, 320); QCOMPARE(g.h, eh);
        QVERIFY(g.pixFmt.startsWith("yuvj") || g.pixFmt.startsWith("yuv"));
        m_thumbs.insert(path, readAll(out));
        QCOMPARE(qint64(m_thumbs.value(path).size()), qint64(j.value("bytes").toDouble()));
        if (m_ffmpeg.isEmpty()) QSKIP("no reference ffmpeg.exe");
        const QString ref = tmp("ref_" + QFileInfo(path).completeBaseName() + ".jpg");
        const Run fr = run(m_ffmpeg, ingestThumbArgs(path, ref, at));
        QCOMPARE(fr.code, 0);
        const Gray rg = decodeGray(ref);
        QCOMPARE(rg.w, g.w); QCOMPARE(rg.h, g.h);
        const bool same = readAll(ref) == m_thumbs.value(path);
        const double db = psnr(g, rg);
        qInfo("%s: at %.2f, %dx%d, %s, luma PSNR %.1f dB vs ffmpeg.exe", qPrintable(QFileInfo(path).fileName()), at, g.w, g.h,
              same ? "byte-identical" : "different bytes", db);
        QVERIFY2(same || db >= 40.0, qPrintable(QString("PSNR %1 dB").arg(db)));
    }

    // The ffmpeg-style default mode (no sub-command word) takes the same arguments and writes the same JPEG.
    void ffmpegModeThumbnail() {
        if (m_mp4.isEmpty() || !m_thumbs.contains(m_mp4)) QSKIP("needs the h264 fixture");
        const QString out = tmp("ffmpeg_mode.jpg");
        const Run r = tool(ingestThumbArgs(m_mp4, out, ingestAt(m_mp4, durationOf(m_mp4))));
        QCOMPARE(r.code, 0);
        QVERIFY(lastJson(r.out).value("ok").toBool());
        QCOMPARE(readAll(out), m_thumbs.value(m_mp4));
        // into a non-ASCII folder
        QVERIFY(QDir(m_tmp.path()).mkpath(QString::fromUtf8("th\xc3\xbcmb \xe5\xa3\xb0")));
        const QString uout = tmp(QString::fromUtf8("th\xc3\xbcmb \xe5\xa3\xb0/t.jpg"));
        const Run u = tool(QStringList{"thumb"} + ingestThumbArgs(m_mp4, uout, 0.5));
        QCOMPARE(u.code, 0);
        QCOMPARE(decodeGray(uout).w, 320);
    }

    void thumbFailures() {
        auto expectFail = [&](const QStringList& args, const QString& outPath, const char* errPart) {
            const Run r = tool(QStringList{"thumb"} + args);
            const QJsonObject j = lastJson(r.out);
            QVERIFY2(r.code == 2, qPrintable(args.join(' ') + " -> exit " + QString::number(r.code) + " " + QString::fromUtf8(r.out)));
            QVERIFY(!j.value("ok").toBool(true)); QVERIFY(!j.value("unsupported").toBool());
            QVERIFY2(j.value("error").toString().contains(QLatin1String(errPart)), qPrintable(j.value("error").toString()));
            QCOMPARE(lineCount(r.out), 1);
            if (!outPath.isEmpty()) QVERIFY2(!QFileInfo::exists(outPath), qPrintable(outPath));
        };
        expectFail(ingestThumbArgs(m_missing, tmp("f1.jpg"), 0), tmp("f1.jpg"), "No such file");
        expectFail(ingestThumbArgs(m_wav, tmp("f2.jpg"), 0), tmp("f2.jpg"), "no video stream");
        expectFail(ingestThumbArgs(m_text, tmp("f3.jpg"), 0), tmp("f3.jpg"), "cannot open");
        if (!m_trunc.isEmpty()) expectFail(ingestThumbArgs(m_trunc, tmp("f4.jpg"), 1.0), tmp("f4.jpg"), "cannot open");
        if (!m_mp4.isEmpty()) {
            expectFail(ingestThumbArgs(m_mp4, tmp("f5.jpg"), 60.0), tmp("f5.jpg"), "no video frame at or after");   // past the end
            // an existing output without -y is left alone (ffmpeg -n)
            QVERIFY(writeBytes(tmp("keep.jpg"), "original"));
            QStringList noY = ingestThumbArgs(m_mp4, tmp("keep.jpg"), 0.5); noY.removeFirst();
            expectFail(noY, QString(), "already exists");
            QCOMPARE(readAll(tmp("keep.jpg")), QByteArray("original"));
            QStringList withN = noY; withN.prepend("-n");
            expectFail(withN, QString(), "already exists");
        }
        // a truncated moov-first file: whatever it decodes, the tool answers (one line, 0 or 2) and leaves no partial file
        if (QFileInfo(tmp("truncated_faststart.mp4")).isFile()) {
            const Run r = tool(QStringList{"thumb"} + ingestThumbArgs(tmp("truncated_faststart.mp4"), tmp("f6.jpg"), 0.5));
            QVERIFY(r.code == 0 || r.code == 2);
            QCOMPARE(lineCount(r.out), 1);
            if (r.code == 0) QCOMPARE(decodeGray(tmp("f6.jpg")).w, 320); else QVERIFY(!QFileInfo::exists(tmp("f6.jpg")));
        }
        for (const QFileInfo& fi : QDir(m_tmp.path()).entryInfoList({"*.jpg.*"}, QDir::Files)) QFAIL(qPrintable("temporary file left: " + fi.fileName()));
    }

    // A rotation ffmpeg needs more than a lossless remap for (its rotate filter, a format conversion before the
    // transpose): exit 64 found after decoding, one unsupported line, no file; the serve op answers unsupported too. The
    // caller then runs ffmpeg.exe, which makes the picture: never a sideways or differently converted thumbnail.
    void thumbRotationNotReproducedFallsBack() {
        if (m_rotFallback.isEmpty()) QSKIP("no reference ffmpeg.exe to make the rotated fixtures");
        const mediatool::ServeConfig cfg;
        for (const QString& f : m_rotFallback) {
            const QString out = tmp("fallback_" + QFileInfo(f).completeBaseName() + ".jpg");
            const Run r = tool(QStringList{"thumb"} + ingestThumbArgs(f, out, 0.5));
            const QJsonObject j = lastJson(r.out);
            QVERIFY2(r.code == 64, qPrintable(QFileInfo(f).fileName() + " -> exit " + QString::number(r.code) + " " + QString::fromUtf8(r.out)));
            QVERIFY(j.value("unsupported").toBool()); QVERIFY(j.value("error").toString().startsWith("unsupported: "));
            QCOMPARE(lineCount(r.out), 1);
            QVERIFY(!QFileInfo::exists(out));
            const QJsonObject s = mediatool::handleRequest({{"op", "thumb"}, {"path", f}, {"out", out}, {"at", 0.5}}, 30000, nullptr, cfg);
            QVERIFY2(s.value("unsupported").toBool() && !s.value("ok").toBool(true), QJsonDocument(s).toJson(QJsonDocument::Compact).constData());
            QVERIFY(!QFileInfo::exists(out));
            QCOMPARE(run(m_ffmpeg, ingestThumbArgs(f, out, 0.5)).code, 0);                    // the fallback itself works
            QVERIFY(QFileInfo(out).size() > 0);
            qInfo("%s: %s", qPrintable(QFileInfo(f).fileName()), qPrintable(j.value("error").toString()));
        }
    }

    // ---- --serve ------------------------------------------------------------------------------------------------------
    void serveInProcessHandler() {
        if (m_mp4.isEmpty()) QSKIP("needs the h264 fixture");
        const mediatool::ServeConfig cfg;
        const std::atomic<bool>* never = nullptr;
        QJsonObject r = mediatool::handleRequest({{"op", "probe"}, {"path", m_mp4}}, 30000, never, cfg);
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("probe").toObject(), QJsonDocument::fromJson(m_refJson.value(m_mp4)).object());
        r = mediatool::handleRequest({{"op", "duration"}, {"path", m_mp4}}, 30000, never, cfg);
        QVERIFY(r.value("ok").toBool()); QCOMPARE(r.value("value").toString().toUtf8() + "\r\n", m_refDuration.value(m_mp4));
        r = mediatool::handleRequest({{"op", "duration"}, {"path", tmp("still.png")}}, 30000, never, cfg);
        if (QFileInfo(tmp("still.png")).isFile()) { QVERIFY(r.value("ok").toBool()); QCOMPARE(r.value("value").toString(), QString("N/A")); QCOMPARE(r.value("duration").toDouble(), 0.0); }
        r = mediatool::handleRequest({{"op", "version"}}, 30000, never, cfg);
        QVERIFY(r.value("ok").toBool()); QCOMPARE(r.value("tool").toString(), QString("PlayPlayMediaTool"));
        // the serve loop's cancel flag (a timed-out request, a closed stdout) stops a thumbnail between packets and a
        // probe / duration in AvKit's interrupt callback
        std::atomic<bool> stop{true};
        r = mediatool::handleRequest({{"op", "thumb"}, {"path", m_mp4}, {"out", tmp("cancelled.jpg")}, {"at", 1.0}}, 30000, &stop, cfg);
        QVERIFY(!r.value("ok").toBool(true)); QVERIFY(r.value("cancelled").toBool());
        QVERIFY(!QFileInfo::exists(tmp("cancelled.jpg")));
        for (const char* op : {"probe", "duration"}) {
            r = mediatool::handleRequest({{"op", op}, {"path", m_mp4}}, 30000, &stop, cfg);
            QVERIFY2(!r.value("ok").toBool(true) && r.value("cancelled").toBool(), QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
        }
        // a rotated input through the serve op: the same portrait JPEG as the one-shot CLI
        if (m_thumbs.contains(tmp("rot90.mp4"))) {
            const QString in = tmp("rot90.mp4");
            QJsonObject req{{"op", "thumb"}, {"path", in}, {"out", tmp("serve_rot90.jpg")}};
            const double at = asPassed(ingestAt(in, durationOf(in)));
            if (at > 0) req["at"] = at;
            r = mediatool::handleRequest(req, 30000, never, cfg);
            QVERIFY(r.value("ok").toBool());
            QCOMPARE(r.value("autorotate").toString(), QString("transpose=cclock"));
            QCOMPARE(r.value("width").toInt(), 320); QCOMPARE(r.value("height").toInt(), 568);
            QCOMPARE(readAll(tmp("serve_rot90.jpg")), m_thumbs.value(in));
        }
        r = mediatool::handleRequest({{"op", "probe"}, {"path", "http://127.0.0.1:9/x.mp4"}}, 30000, never, cfg);
        QVERIFY(r.value("unsupported").toBool());
        r = mediatool::handleRequest({{"op", "thumb"}, {"path", m_mp4}}, 30000, never, cfg);
        QVERIFY(!r.value("ok").toBool(true)); QVERIFY(r.value("error").toString().contains("out"));
        r = mediatool::handleRequest({{"op", "transcode"}, {"path", m_mp4}}, 30000, never, cfg);
        QVERIFY(r.value("unsupported").toBool());
        QVERIFY(r.contains("ms")); QVERIFY(r.contains("worker"));
    }

    void serveHandles200Requests() {
        QStringList good;
        for (const QString& p : m_media) if (m_refJson.contains(p) && !m_refJson.value(p).startsWith("{\r\n\r\n}")) good << p;
        QVERIFY(!good.isEmpty());
        QStringList thumbIn;
        for (const QString& p : m_video) if (m_thumbs.contains(p)) thumbIn << p;
        QStringList bad = {m_text, m_missing};
        if (!m_trunc.isEmpty()) bad << m_trunc;

        ServeChild c;
        QVERIFY(c.start());                                                                    // default: 3 workers
        const QJsonObject pong = c.call({{"id", "p1"}, {"op", "ping"}});
        QVERIFY(pong.value("pong").toBool());
        const qint64 pid = qint64(pong.value("pid").toDouble());
        QVERIFY(pid > 0);

        struct Expect { QString op, path, out; bool ok; };
        QHash<QString, Expect> expect;
        QElapsedTimer t; t.start();
        for (int i = 0; i < 200; ++i) {
            QJsonObject req{{"id", i}};
            Expect e;
            if (i % 25 == 3 && !thumbIn.isEmpty()) {                                          // 8 thumbnails
                e.op = "thumb"; e.path = thumbIn.at((i / 25) % thumbIn.size()); e.ok = true;
                e.out = tmp(QString("serve_thumb_%1.jpg").arg(i));
                const double at = asPassed(ingestAt(e.path, durationOf(e.path)));             // what a client would send: the CLI's -ss value
                if (at > 0) req["at"] = at;
                req["out"] = e.out;
            } else if (i % 25 == 7) { e.op = "thumb"; e.path = bad.at((i / 25) % bad.size()); e.ok = false; e.out = tmp(QString("serve_bad_%1.jpg").arg(i)); req["out"] = e.out; }
            else if (i % 10 == 9) { e.op = "probe"; e.path = bad.at(i % bad.size()); e.ok = false; }
            else if (i % 10 == 5) { e.op = "duration"; e.path = good.at(i % good.size()); e.ok = true; }
            else { e.op = "probe"; e.path = good.at(i % good.size()); e.ok = true; }
            req["op"] = e.op; req["path"] = e.path;
            expect.insert(ServeChild::key(i), e);
            c.send(req);                                                                       // all 200 queued at once
        }
        QVERIFY2(c.waitFor(200, 90000), qPrintable(QString("%1 of 200 replies").arg(c.replies.size())));
        const qint64 wall = t.elapsed();
        QSet<int> workers;
        int okCount = 0, failCount = 0, thumbs = 0, identicalThumbs = 0;
        for (auto it = expect.cbegin(); it != expect.cend(); ++it) {
            const QJsonObject r = c.replies.value(it.key());
            const Expect& e = it.value();
            const QString what = e.op + " " + QFileInfo(e.path).fileName() + " -> " + QString::fromUtf8(QJsonDocument(r).toJson(QJsonDocument::Compact).left(300));
            QVERIFY2(!r.isEmpty(), qPrintable("no reply for " + it.key()));
            QVERIFY2(r.value("ok").toBool() == e.ok, qPrintable(what));
            workers.insert(r.value("worker").toInt());
            if (!e.ok) { ++failCount; QVERIFY(!r.value("error").toString().isEmpty()); if (!e.out.isEmpty()) QVERIFY(!QFileInfo::exists(e.out)); continue; }
            ++okCount;
            if (e.op == "probe") QVERIFY2(r.value("probe").toObject() == QJsonDocument::fromJson(m_refJson.value(e.path)).object(), qPrintable(what));
            else if (e.op == "duration") QVERIFY2(r.value("value").toString().toUtf8() + "\r\n" == m_refDuration.value(e.path), qPrintable(what));
            else {
                ++thumbs;
                QCOMPARE(decodeGray(e.out).w, 320);
                if (readAll(e.out) == m_thumbs.value(e.path)) ++identicalThumbs;                // serve and one-shot write the same JPEG
            }
        }
        QCOMPARE(identicalThumbs, thumbs);
        QCOMPARE(workers.size(), 3);                                                           // the work spread over the 3 workers
        qInfo("--serve: 200 requests (%d ok, %d failed as expected, %d thumbnails) in %lld ms over workers %d",
              okCount, failCount, thumbs, wall, int(workers.size()));
        if (m_spawns > 0)
            qInfo("one-shot spawns of the JSON form: ffprobe.exe %.1f ms, PlayPlayMediaTool %.1f ms per file (avg over %d)",
                  double(m_ffprobeMs) / m_spawns, double(m_toolMs) / m_spawns, m_spawns);
        // the bad files did not take the server down: same process, still answering
        const QJsonObject pong2 = c.call({{"id", "p2"}, {"op", "ping"}});
        QVERIFY(pong2.value("pong").toBool());
        QCOMPARE(qint64(pong2.value("pid").toDouble()), pid);
        QCOMPARE(c.call({{"id", "q"}, {"op", "quit"}}).value("bye").toBool(), true);
        QVERIFY(c.p.waitForFinished(10000));
        QCOMPARE(c.p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(c.p.exitCode(), 0);
    }

    void serveSurvivesBadInput() {
        ServeChild c;
        QVERIFY(c.start({"--workers", "1", "--timeout-ms", "20000"}));
        const qint64 pid = qint64(c.call({{"id", 1}, {"op", "ping"}}).value("pid").toDouble());
        QVERIFY(pid > 0);
        QJsonObject r;
        if (!m_trunc.isEmpty()) {
            r = c.call({{"id", 2}, {"op", "probe"}, {"path", m_trunc}});
            QVERIFY(!r.value("ok").toBool(true)); QVERIFY(r.value("error").toString().contains("Invalid data"));
            r = c.call({{"id", 3}, {"op", "thumb"}, {"path", m_trunc}, {"out", tmp("sb1.jpg")}});
            QVERIFY(!r.value("ok").toBool(true));
        }
        r = c.call({{"id", 4}, {"op", "probe"}, {"path", m_text}});
        QVERIFY(!r.value("ok").toBool(true));
        r = c.call({{"id", 5}, {"op", "thumb"}, {"path", m_text}, {"out", tmp("sb2.jpg")}});
        QVERIFY(!r.value("ok").toBool(true)); QVERIFY(!QFileInfo::exists(tmp("sb2.jpg")));
        r = c.call({{"id", 6}, {"op", "probe"}, {"path", "http://127.0.0.1:9/a.mp4"}});
        QVERIFY(r.value("unsupported").toBool());
        r = c.call({{"id", 7}, {"op", "explode"}});
        QVERIFY(r.value("unsupported").toBool());
        r = c.call({{"id", 8}, {"op", "probe"}});
        QVERIFY(!r.value("ok").toBool(true)); QVERIFY(r.value("error").toString().contains("path"));
        c.sendRaw("{this is not json");
        for (int i = 0; i < 40 && c.unmatched.isEmpty(); ++i) c.pump(50);
        QCOMPARE(c.unmatched.size(), 1); QVERIFY(c.unmatched.first().value("error").toString().startsWith("bad request"));
        r = c.call({{"id", 9}, {"op", "duration"}, {"path", m_wav}});
        QVERIFY(r.value("ok").toBool()); QCOMPARE(r.value("duration").toDouble(), 1.25);
        // a request past its timeout: {"ok": false, "timeout": true} (from the loop or from the thumbnailer's own deadline,
        // whichever fires first), no JPEG left behind, and the server goes on
        const QString big = QFileInfo(tmp("hd720.mp4")).isFile() ? tmp("hd720.mp4") : m_mp4;
        if (!big.isEmpty()) {
            r = c.call({{"id", 11}, {"op", "thumb"}, {"path", big}, {"out", tmp("timeout.jpg")}, {"at", 2.0}, {"timeoutMs", 1}});
            QVERIFY2(!r.value("ok").toBool(true) && r.value("timeout").toBool(), QJsonDocument(r).toJson(QJsonDocument::Compact).constData());
            r = c.call({{"id", 12}, {"op", "thumb"}, {"path", big}, {"out", tmp("after_timeout.jpg")}, {"at", 1.0}});
            QVERIFY(r.value("ok").toBool());
            QVERIFY(!QFileInfo::exists(tmp("timeout.jpg")));
        }
        QCOMPARE(qint64(c.call({{"id", 10}, {"op", "ping"}}).value("pid").toDouble()), pid);
        c.p.closeWriteChannel();                                                               // EOF: in-flight work drains, exit 0
        QVERIFY(c.p.waitForFinished(10000));
        QCOMPARE(c.p.exitCode(), 0);
    }
};

QTEST_GUILESS_MAIN(TestInhouseMediatool)
#include "test_inhouse_mediatool.moc"
