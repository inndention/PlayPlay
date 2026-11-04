// DEP-05a: the ffmpeg-argument subset parser of the PlayPlayMediaTool graph runner (tools/mediatool/argparse.h).
// Every argument list the Supervisor builds for ffmpeg.exe must parse: suites::presetArgs in the render farm's three forms
// (transcode, split segment local / StarAgent, concat join), SpotGenerator::composeStories with headline::storiesFilter
// for 1 / 3 / 7 scenes with and without music and animated beds, and SpotGenerator::compose with and without music. The
// builders are SpotGenerator members on a Gen, so their argument assembly is mirrored here line for line (spotgenerator.cpp
// composeStories / compose, renderfarm.cpp startTranscode / startSegment / concatSegments) around the real presetArgs,
// sceneTimings and storiesFilter. Anything outside the subset is "unsupported" (exit 64, {unsupported: true}); a command
// line ffmpeg would refuse is an error (exit 2).
// DEP-05b: the runner (graphrunner.h, concat.h). A 3-scene stories spot is rendered from tests/inhouse/data/graph with
// the tool's ffmpeg mode and with the reference ffmpeg.exe: same duration (+-1 frame), same stream layout, moov before
// mdat, PSNR >= 40 dB on sampled frames (byte-identical in practice); -progress pipe:1 parsed by suites::progressSeconds;
// three split segments joined by the concat remux equal the single-pass duration; copy / wav / loudnorm forms match
// ffmpeg.exe byte for byte; a missing input or an existing output with -n fail with 2, never 64.
#include "argparse.h"
#include "graphrunner.h"
#include "concat.h"
#include "headlinenews.h"
#include "ppcli.h"
#include "suitelogic.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#ifndef PP_MEDIATOOL_EXE
#define PP_MEDIATOOL_EXE ""
#endif
#ifndef PP_INHOUSE_DATA_DIR
#define PP_INHOUSE_DATA_DIR ""
#endif
#ifndef PP_FFMPEG_BIN
#define PP_FFMPEG_BIN ""
#endif

using mediatool::GraphCommand;
using mediatool::GraphParseOptions;
using mediatool::parseGraphArgs;

namespace {

const QStringList kPresets = {"h264_1080p30", "h264_720p30", "proxy_540p", "audio_wav", "loudnorm", "copy"};
const QStringList kRenderGlobals = {"-hide_banner", "-nostdin", "-y", "-progress", "pipe:1", "-nostats"};
const QString kIn = "D:\\media\\in\\clip one.mp4";

// renderfarm.cpp startTranscode: globals, -i in, preset, out.<ext>
QStringList transcodeArgs(const QString& preset) {
    QStringList a = kRenderGlobals; a << "-i" << kIn << suites::presetArgs(preset) << "D:\\render\\out\\clip_" + preset + "." + suites::presetExt(preset);
    return a;
}
// renderfarm.cpp startSegment: the local worker (with -progress) and the StarAgent command (without, after "ffmpeg")
QStringList segmentArgs(const QString& preset, bool local) {
    QStringList seg = suites::presetArgs(preset); if (preset == "copy") seg << "-avoid_negative_ts" << "make_zero";
    QStringList a = local ? kRenderGlobals : QStringList{"-hide_banner", "-nostdin", "-y"};
    a << "-ss" << "12.500" << "-t" << "7.250" << "-i" << kIn << seg << (local ? "D:\\render\\work\\seg_003.mp4" : "C:\\agent\\pp_seg_abc_3.mp4");
    return a;
}
// renderfarm.cpp concatSegments
QStringList concatArgs() {
    return {"-hide_banner", "-nostdin", "-y", "-f", "concat", "-safe", "0", "-i", "D:\\render\\work\\concat.txt", "-c", "copy", "-movflags", "+faststart", "D:\\render\\out\\clip_split_abc.mp4"};
}

// spotgenerator.cpp composeStories: scene i's bed (a still, or a looping frame sequence) at 2i, furniture at 2i+1,
// narration WAVs at 2K+i, the music bed at 3K; the filter from storiesFilter; libx264 / aac mp4 cut to the timeline.
struct Stories { QStringList args; headline::SceneTiming t; double D = 0; QVector<bool> animated; };
Stories storiesArgs(int K, bool music, int animatedMode /* 0 none, 1 all, 2 every other */) {
    Stories s;
    QVector<double> wav; QStringList kinds;
    for (int i = 0; i < K; ++i) { wav << 2.0 + 0.37 * i; kinds << (i == 0 ? "intro" : i == K - 1 && K > 1 ? "outro" : "story"); }
    s.t = headline::sceneTimings(wav, kinds, 30.0);
    s.D = double(s.t.totalFrames) / s.t.fps;
    s.args = {"-y", "-nostdin", "-v", "error"};
    for (int i = 0; i < K; ++i) {
        const bool anim = animatedMode == 1 || (animatedMode == 2 && i % 2 == 1); s.animated << anim;
        if (anim) s.args << "-stream_loop" << "-1" << "-framerate" << "15" << "-i" << QString("D:\\spots\\work\\g1\\bed%1\\frame_%04d.png").arg(i);
        else s.args << "-i" << QString("D:\\spots\\work\\g1\\bed%1.jpg").arg(i);
        s.args << "-loop" << "1" << "-i" << QString("D:\\spots\\work\\g1\\furniture%1.png").arg(i);
    }
    for (int i = 0; i < K; ++i) s.args << "-i" << QString("D:\\spots\\work\\g1\\scene%1.wav").arg(i);
    if (music) s.args << "-i" << "D:\\spots\\work\\g1\\music.wav";
    s.args << "-filter_complex" << headline::storiesFilter(s.t, music, s.animated) << "-map" << "[v]" << "-map" << "[a]" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-color_range" << "tv" << "-r" << "30"
           << "-c:a" << "aac" << "-b:a" << "160k" << "-ar" << "48000" << "-ac" << "2" << "-movflags" << "+faststart" << "-t" << QString::number(s.D, 'f', 3) << "D:\\spots\\out\\headlines_20261026_1700_g1.mp4";
    return s;
}

// spotgenerator.cpp compose: one bed, the looped furniture, the narration, optionally the music bed.
QStringList composeArgs(bool music, double* durOut = nullptr) {
    const double D = qMax(10.0, 7.3 + 0.8); const int frames = int(std::ceil(D * 30.0));
    const QString dS = QString::number(D, 'f', 2), foS = QString::number(qMax(0.0, D - 0.6), 'f', 2);
    QString fc = QString("[0:v]scale=2880:1620:force_original_aspect_ratio=increase:out_range=tv,crop=2880:1620,zoompan=z='min(1+0.00035*on,1.22)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=%1:s=1920x1080:fps=30,format=yuv420p[bg];[bg][1:v]overlay=0:0:format=auto,fade=t=in:d=0.4,fade=t=out:st=%2:d=0.6,format=yuv420p[v];").arg(frames).arg(foS);
    QStringList args = {"-y", "-nostdin", "-v", "error", "-i", "D:\\spots\\work\\g2\\photo.jpg", "-loop", "1", "-i", "D:\\spots\\work\\g2\\furniture.png", "-i", "D:\\spots\\work\\g2\\voice.wav"};
    if (music) {
        args << "-i" << "D:\\spots\\work\\g2\\music.wav";
        fc += QString("[2:a]aformat=sample_rates=48000:channel_layouts=stereo,apad,atrim=0:%1,asetpts=PTS-STARTPTS,asplit[n1][n2];[3:a]aformat=sample_rates=48000:channel_layouts=stereo,aloop=loop=-1:size=2000000000,volume=0.16,atrim=0:%1,asetpts=PTS-STARTPTS,afade=t=in:d=0.8,afade=t=out:st=%2:d=1.6[bed];[bed][n1]sidechaincompress=threshold=0.03:ratio=8:attack=40:release=600:level_sc=1[duck];[duck][n2]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[a]").arg(dS, QString::number(qMax(0.0, D - 1.6), 'f', 2));
    } else fc += QString("[2:a]aformat=sample_rates=48000:channel_layouts=stereo,apad,atrim=0:%1,asetpts=PTS-STARTPTS[a]").arg(dS);
    args << "-filter_complex" << fc << "-map" << "[v]" << "-map" << "[a]" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-color_range" << "tv" << "-r" << "30" << "-c:a" << "aac" << "-b:a" << "160k" << "-ar" << "48000" << "-ac" << "2" << "-movflags" << "+faststart" << "-t" << dS << "D:\\spots\\out\\cinema_20261026_1700_g2.mp4";
    if (durOut) *durOut = D;
    return args;
}

struct Run { int code = -1; QByteArray out, err; };
Run run(const QString& exe, const QStringList& args, int timeoutMs = 30000) {
    QProcess p; p.start(exe, args); Run r;
    if (!p.waitForStarted(10000)) return r;
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(2000); return r; }
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError();
    return r;
}

// ---- DEP-05b helpers: libav probes of the rendered files ------------------------------------------------------------
struct StreamInfo { QString codec; int w = 0, h = 0; QString pixFmt; double fps = 0; int rate = 0, ch = 0; double durationSec = 0; qint64 frames = 0; QString summary() const { return QString("%1 %2x%3 %4 %5 %6/%7 %8s %9").arg(codec).arg(w).arg(h).arg(pixFmt).arg(fps).arg(rate).arg(ch).arg(durationSec).arg(frames); } };
struct FileInfo { double durationSec = 0; QVector<StreamInfo> streams; bool ok = false; QString err; };
FileInfo probeFile(const QString& path) {
    FileInfo fi; AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) { fi.err = "cannot open " + path; return fi; }
    if (avformat_find_stream_info(fc, nullptr) < 0) { avformat_close_input(&fc); fi.err = "no stream info"; return fi; }
    fi.durationSec = fc->duration > 0 ? double(fc->duration) / AV_TIME_BASE : 0;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i]; StreamInfo s;
        s.codec = QString::fromLatin1(avcodec_get_name(st->codecpar->codec_id)); s.w = st->codecpar->width; s.h = st->codecpar->height;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { s.pixFmt = QString::fromLatin1(av_get_pix_fmt_name(AVPixelFormat(st->codecpar->format))); s.fps = av_q2d(st->avg_frame_rate); }
        s.rate = st->codecpar->sample_rate; s.ch = st->codecpar->ch_layout.nb_channels;
        s.durationSec = st->duration > 0 ? st->duration * av_q2d(st->time_base) : 0; s.frames = st->nb_frames;
        fi.streams << s;
    }
    avformat_close_input(&fc); fi.ok = true; return fi;
}
// top-level mp4 atoms in file order ("ftyp moov mdat" with +faststart)
QStringList topAtoms(const QString& path) {
    QStringList out; QFile f(path); if (!f.open(QIODevice::ReadOnly)) return out;
    while (!f.atEnd() && out.size() < 16) {
        const QByteArray hdr = f.read(8); if (hdr.size() < 8) break;
        quint64 size = quint32(quint8(hdr[0])) << 24 | quint32(quint8(hdr[1])) << 16 | quint32(quint8(hdr[2])) << 8 | quint32(quint8(hdr[3]));
        out << QString::fromLatin1(hdr.mid(4, 4));
        if (size == 1) { const QByteArray l = f.read(8); size = 0; for (int i = 0; i < 8; ++i) size = size << 8 | quint8(l[i]); size -= 8; }
        if (size == 0) break;
        if (!f.seek(f.pos() + qint64(size) - 8)) break;
    }
    return out;
}
// every n-th decoded video frame's luma plane
QVector<QByteArray> sampleLuma(const QString& path, int every, int* w, int* h) {
    QVector<QByteArray> out; AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, path.toUtf8().constData(), nullptr, nullptr) < 0) return out;
    avformat_find_stream_info(fc, nullptr);
    const int vs = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fc); return out; }
    const AVCodec* codec = avcodec_find_decoder(fc->streams[vs]->codecpar->codec_id);
    AVCodecContext* dec = avcodec_alloc_context3(codec); avcodec_parameters_to_context(dec, fc->streams[vs]->codecpar); avcodec_open2(dec, codec, nullptr);
    AVPacket* pkt = av_packet_alloc(); AVFrame* fr = av_frame_alloc(); int n = 0;
    auto drain = [&]() { while (avcodec_receive_frame(dec, fr) >= 0) { if (n % every == 0) { QByteArray y; for (int r = 0; r < fr->height; ++r) y.append(reinterpret_cast<const char*>(fr->data[0] + r * fr->linesize[0]), fr->width); out << y; *w = fr->width; *h = fr->height; } ++n; av_frame_unref(fr); } };
    while (av_read_frame(fc, pkt) >= 0) { if (pkt->stream_index == vs && avcodec_send_packet(dec, pkt) >= 0) drain(); av_packet_unref(pkt); }
    avcodec_send_packet(dec, nullptr); drain();
    av_frame_free(&fr); av_packet_free(&pkt); avcodec_free_context(&dec); avformat_close_input(&fc);
    return out;
}
QByteArray fileBytes(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<unreadable>" + path.toUtf8()); }
double psnrDb(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size() || a.isEmpty()) return 0;
    double mse = 0; for (int i = 0; i < a.size(); ++i) { const double d = double(quint8(a[i])) - double(quint8(b[i])); mse += d * d; }
    mse /= a.size();
    return mse <= 0 ? 999.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

// The 3-scene stories spot on the fixtures: bed0.jpg (still), bed1/frame_%04d.png (animated, -stream_loop -1 -framerate
// 15), bed2.jpg (still), the three furniture PNGs looped, scene0-2.wav, music.wav; timings from the real WAV lengths.
struct Fixture { QStringList args; double D = 0; headline::SceneTiming t; };
Fixture fixtureStories(const QString& dir, const QString& out, bool music) {
    Fixture f; QVector<double> wav;
    for (int i = 0; i < 3; ++i) { const FileInfo fi = probeFile(dir + "/scene" + QString::number(i) + ".wav"); wav << fi.durationSec; }
    f.t = headline::sceneTimings(wav, {"intro", "story", "outro"}, 30.0);
    f.D = double(f.t.totalFrames) / f.t.fps;
    const QVector<bool> animated = {false, true, false};
    f.args = {"-y", "-nostdin", "-v", "error"};
    for (int i = 0; i < 3; ++i) {
        if (animated[i]) f.args << "-stream_loop" << "-1" << "-framerate" << "15" << "-i" << dir + "/bed1/frame_%04d.png";
        else f.args << "-i" << dir + "/bed" + QString::number(i) + ".jpg";
        f.args << "-loop" << "1" << "-i" << dir + "/furniture" + QString::number(i) + ".png";
    }
    for (int i = 0; i < 3; ++i) f.args << "-i" << dir + "/scene" + QString::number(i) + ".wav";
    if (music) f.args << "-i" << dir + "/music.wav";
    f.args << "-filter_complex" << headline::storiesFilter(f.t, music, animated) << "-map" << "[v]" << "-map" << "[a]" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-color_range" << "tv" << "-r" << "30"
           << "-c:a" << "aac" << "-b:a" << "160k" << "-ar" << "48000" << "-ac" << "2" << "-movflags" << "+faststart" << "-t" << QString::number(f.D, 'f', 3) << out;
    return f;
}

// The whole spot / render-farm output contract every composed mp4 shares.
void checkSpotOutput(const GraphCommand& c, double D) {
    QCOMPARE(c.outputs.size(), 1);
    const mediatool::GraphOutput& o = c.outputs.first();
    QCOMPARE(o.container, QString("mp4"));
    QCOMPARE(o.maps, QStringList({"v", "a"}));
    QCOMPARE(o.videoEncoder(), QString("libx264")); QCOMPARE(o.audioEncoder(), QString("aac"));
    QCOMPARE(o.preset, QString("veryfast")); QCOMPARE(o.crf, 20); QCOMPARE(o.pixFmt, QString("yuv420p")); QCOMPARE(o.colorRange, QString("tv"));
    QCOMPARE(o.frameRateValue, 30.0); QCOMPARE(o.audioBitrate, qint64(160000)); QCOMPARE(o.sampleRate, 48000); QCOMPARE(o.channels, 2);
    QVERIFY(o.faststart); QVERIFY(o.vf.isEmpty() && o.af.isEmpty());
    QVERIFY2(std::fabs(o.durationSec - D) < 0.002, qPrintable(QString("-t %1 vs %2").arg(o.durationSec).arg(D)));
    QVERIFY(c.overwrite && c.nostdin && !c.progressPipe); QCOMPARE(c.logLevel, 16);   // -v error
}

}   // namespace

class TestInhouseGraphrunner : public QObject {
    Q_OBJECT
    QString m_tool;

private slots:
    void initTestCase() {
        m_tool = QStringLiteral(PP_MEDIATOOL_EXE);
        m_data = QStringLiteral(PP_INHOUSE_DATA_DIR) + "/graph";
        const QString ff = QStringLiteral(PP_FFMPEG_BIN) + "/ffmpeg.exe";
        if (QFileInfo(ff).isFile()) m_ffmpeg = ff;
        QVERIFY(m_tmp.isValid());
        qInfo("tool: %s\nfixtures: %s\nreference ffmpeg: %s", qPrintable(m_tool), qPrintable(m_data), qPrintable(m_ffmpeg.isEmpty() ? "(none)" : m_ffmpeg));
    }

    // ---- the render farm: every preset in every form ---------------------------------------------------------------
    void presetsEnumerated() {
        // the preset table of suitelogic.cpp: a new preset must be added to kPresets (and parse) before the farm may use it
        bool ok = false;
        for (const QString& p : kPresets) { suites::presetArgs(p, &ok); QVERIFY2(ok, qPrintable(p)); }
        suites::presetArgs("h265_4k", &ok); QVERIFY(!ok);
    }
    void renderFarmParses_data() {
        QTest::addColumn<QString>("preset"); QTest::addColumn<QString>("form");
        for (const QString& p : kPresets) for (const char* f : {"transcode", "segment-local", "segment-remote"}) QTest::newRow(qPrintable(p + "/" + f)) << p << QString::fromLatin1(f);
        QTest::newRow("concat") << QString() << QString("concat");
    }
    void renderFarmParses() {
        QFETCH(QString, preset); QFETCH(QString, form);
        const QStringList args = form == "transcode" ? transcodeArgs(preset) : form == "segment-local" ? segmentArgs(preset, true) : form == "segment-remote" ? segmentArgs(preset, false) : concatArgs();
        const GraphCommand c = parseGraphArgs(args);
        QVERIFY2(c.ok(), qPrintable(c.unsupported + c.error + "\n" + args.join(' ')));
        QCOMPARE(c.inputs.size(), 1); QCOMPARE(c.outputs.size(), 1);
        const mediatool::GraphInput& in = c.inputs.first(); const mediatool::GraphOutput& o = c.outputs.first();
        QVERIFY(c.overwrite && c.nostdin && c.hideBanner);
        QCOMPARE(c.progressPipe, form == "transcode" || form == "segment-local"); QCOMPARE(c.nostats, c.progressPipe);
        if (form == "concat") {
            QVERIFY(in.concat && in.safe0); QVERIFY(o.copyAll && o.faststart); QCOMPARE(o.videoEncoder(), QString("copy")); QCOMPARE(o.audioEncoder(), QString("copy"));
            return;
        }
        QCOMPARE(in.path, kIn); QVERIFY(!in.concat && !in.loop && !in.streamLoop);
        if (form.startsWith("segment")) { QCOMPARE(in.seekSec, 12.5); QCOMPARE(in.durationSec, 7.25); QCOMPARE(o.makeZeroTs, preset == "copy"); }
        else { QVERIFY(in.seekSec < 0 && in.durationSec < 0); QVERIFY(!o.makeZeroTs); }
        QCOMPARE(o.container, QString(form == "transcode" ? suites::presetExt(preset) : "mp4"));
        if (preset == "h264_1080p30" || preset == "h264_720p30") {
            QCOMPARE(o.videoEncoder(), QString("libx264")); QCOMPARE(o.audioEncoder(), QString("aac"));
            QCOMPARE(mediatool::filterNamesIn(o.vf), QStringList({"scale", "pad", "setsar", "fps"}));
            QCOMPARE(o.preset, QString("veryfast")); QCOMPARE(o.crf, preset == "h264_1080p30" ? 20 : 21); QCOMPARE(o.gop, 60); QCOMPARE(o.audioBitrate, qint64(192000)); QCOMPARE(o.sampleRate, 48000); QCOMPARE(o.channels, 2); QVERIFY(o.faststart);
        } else if (preset == "proxy_540p") {
            QCOMPARE(mediatool::filterNamesIn(o.vf), QStringList({"scale", "fps"})); QCOMPARE(o.preset, QString("ultrafast")); QCOMPARE(o.crf, 28); QCOMPARE(o.audioBitrate, qint64(96000)); QVERIFY(o.faststart);
        } else if (preset == "audio_wav") {
            QVERIFY(o.noVideo); QCOMPARE(o.videoEncoder(), QString()); QCOMPARE(o.audioEncoder(), QString("pcm_s16le")); QCOMPARE(o.sampleRate, 48000); QCOMPARE(o.channels, 2);
        } else if (preset == "loudnorm") {
            QCOMPARE(o.videoEncoder(), QString("copy")); QCOMPARE(o.audioEncoder(), QString("aac")); QCOMPARE(mediatool::filterNamesIn(o.af), QStringList({"loudnorm"})); QCOMPARE(o.audioBitrate, qint64(192000));
        } else if (preset == "copy") {
            QVERIFY(o.copyAll); QCOMPARE(o.videoEncoder(), QString("copy")); QCOMPARE(o.audioEncoder(), QString("copy"));
        }
    }

    // ---- composeStories + storiesFilter: 1 / 3 / 7 scenes x music x animated beds ---------------------------------
    void storiesParse_data() {
        QTest::addColumn<int>("scenes"); QTest::addColumn<bool>("music"); QTest::addColumn<int>("animated");
        for (int K : {1, 3, 7}) for (int m = 0; m < 2; ++m) for (int a = 0; a < 3; ++a)
            QTest::newRow(qPrintable(QString("K=%1 %2 %3").arg(K).arg(m ? "music" : "dry", a == 0 ? "stills" : a == 1 ? "animated" : "mixed"))) << K << bool(m) << a;
    }
    void storiesParse() {
        QFETCH(int, scenes); QFETCH(bool, music); QFETCH(int, animated);
        const Stories s = storiesArgs(scenes, music, animated);
        const GraphCommand c = parseGraphArgs(s.args);
        QVERIFY2(c.ok(), qPrintable(c.unsupported + c.error));
        QCOMPARE(c.inputs.size(), 3 * scenes + (music ? 1 : 0));
        for (int i = 0; i < scenes; ++i) {
            const mediatool::GraphInput& bed = c.inputs[2 * i], &furn = c.inputs[2 * i + 1];
            QCOMPARE(bed.streamLoop, s.animated[i]); QCOMPARE(bed.framerate, s.animated[i] ? 15.0 : 0.0); QVERIFY(!bed.loop);
            QVERIFY(furn.loop && !furn.streamLoop); QVERIFY(furn.path.endsWith(".png"));
            QVERIFY(c.inputs[2 * scenes + i].path.endsWith(".wav"));
        }
        // the graph reads every input exactly where the builder put it
        QStringList expect; for (int i = 0; i < 2 * scenes; ++i) expect << QString::number(i) + ":v";
        for (int i = 0; i < scenes; ++i) expect << QString::number(2 * scenes + i) + ":a";
        if (music) expect << QString::number(3 * scenes) + ":a";
        QStringList got = c.graphInputs; got.sort(); expect.sort(); QCOMPARE(got, expect);
        checkSpotOutput(c, s.D);
        const QStringList names = mediatool::filterNamesIn(c.filterComplex);
        QVERIFY(names.contains("overlay") && names.contains("concat") && names.contains("fade"));
        QCOMPARE(names.contains("xfade"), scenes > 1); QCOMPARE(names.contains("sidechaincompress"), music); QCOMPARE(names.contains("amix"), music);
        QCOMPARE(names.count("zoompan"), scenes - int(std::count(s.animated.begin(), s.animated.end(), true)));
    }

    // ---- compose: the single-card spot -------------------------------------------------------------------------------
    void composeParse_data() { QTest::addColumn<bool>("music"); QTest::newRow("dry") << false; QTest::newRow("music") << true; }
    void composeParse() {
        QFETCH(bool, music);
        double D = 0; const QStringList args = composeArgs(music, &D);
        const GraphCommand c = parseGraphArgs(args);
        QVERIFY2(c.ok(), qPrintable(c.unsupported + c.error));
        QCOMPARE(c.inputs.size(), music ? 4 : 3);
        QVERIFY(!c.inputs[0].loop && c.inputs[1].loop && !c.inputs[2].loop);
        QStringList expect = {"0:v", "1:v", "2:a"}; if (music) expect << "3:a";
        QCOMPARE(c.graphInputs, expect);
        checkSpotOutput(c, D);
        const QStringList names = mediatool::filterNamesIn(c.filterComplex);
        QCOMPARE(names.count("zoompan"), 1); QCOMPARE(names.contains("asplit"), music); QCOMPARE(names.contains("aloop"), music);
    }

    // ---- every filter the builders use is in this libavfilter (inhouse-deps.md §6.2 marked this unverified) ------------
    void filtersAvailable() {
        const QStringList design = {"zoompan", "overlay", "xfade", "fade", "trim", "setpts", "scale", "crop", "format", "pad", "setsar", "fps", "aformat", "apad", "atrim", "asetpts", "asplit", "aloop", "volume", "afade", "sidechaincompress", "amix", "loudnorm", "adelay", "concat", "anull"};
        for (const QString& f : design) QVERIFY2(mediatool::filterExists(f), qPrintable(f));
        QVERIFY(!mediatool::filterExists("no_such_filter_pp"));
        // the union of what the builders actually emit
        QSet<QString> used;
        auto graphOf = [](const QStringList& a) { return a.at(a.indexOf("-filter_complex") + 1); };
        for (int K : {1, 3, 7}) for (int m = 0; m < 2; ++m) for (int a = 0; a < 3; ++a) for (const QString& n : mediatool::filterNamesIn(graphOf(storiesArgs(K, m, a).args))) used << n;
        for (int m = 0; m < 2; ++m) for (const QString& n : mediatool::filterNamesIn(graphOf(composeArgs(m)))) used << n;
        for (const QString& p : kPresets) { const QStringList a = suites::presetArgs(p); for (const char* k : {"-vf", "-af"}) { const int i = a.indexOf(k); if (i >= 0) for (const QString& n : mediatool::filterNamesIn(a.at(i + 1))) used << n; } }
        for (const QString& n : used) QVERIFY2(design.contains(n), qPrintable("builder uses a filter the design does not list: " + n));
    }

    // ---- the tokeniser: libavfilter's quoting, chains, labels -------------------------------------------------------
    void tokenizer() {
        QVector<QVector<mediatool::FilterNode>> chains; QString err;
        QVERIFY(mediatool::tokenizeGraph("[0:v]scale=2880:1620,zoompan=z='min(1+0.001*on,1.16)':x='iw/2-(iw/zoom/2)':d=90,format=yuv420p[bg];[bg][1:v]overlay=0:0:format=auto[v]", &chains, &err));
        QCOMPARE(chains.size(), 2); QCOMPARE(chains[0].size(), 3); QCOMPARE(chains[1].size(), 1);
        QCOMPARE(chains[0][0].inputs, QStringList({"0:v"})); QCOMPARE(chains[0][0].name, QString("scale")); QCOMPARE(chains[0][0].args, QString("2880:1620"));
        QCOMPARE(chains[0][1].args, QString("z='min(1+0.001*on,1.16)':x='iw/2-(iw/zoom/2)':d=90"));   // the quoted commas do not split the filter
        QCOMPARE(chains[0][2].outputs, QStringList({"bg"})); QCOMPARE(chains[1][0].inputs, QStringList({"bg", "1:v"})); QCOMPARE(chains[1][0].outputs, QStringList({"v"}));
        QVERIFY(mediatool::tokenizeGraph("drawtext=text='a\\,b':x=1, format@x=yuv420p ;", &chains, &err));   // escape, instance suffix, spaces, trailing ';'
        QCOMPARE(chains.size(), 1); QCOMPARE(chains[0].size(), 2); QCOMPARE(chains[0][1].name, QString("format")); QCOMPARE(chains[0][1].args, QString("yuv420p"));
        QVERIFY(!mediatool::tokenizeGraph("[0:v", &chains, &err)); QVERIFY(err.contains("unterminated"));
        QVERIFY(!mediatool::tokenizeGraph("[]scale", &chains, &err)); QVERIFY(err.contains("empty"));
        QVERIFY(!mediatool::tokenizeGraph("[0:v],scale", &chains, &err)); QVERIFY(err.contains("no filter name"));
        QCOMPARE(mediatool::filterNamesIn("[0:v"), QStringList());
    }

    // ---- outside the subset -> unsupported; what ffmpeg refuses -> error ------------------------------------------
    void unsupportedAndErrors_data() {
        QTest::addColumn<QStringList>("args"); QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("mention");
        const QStringList base = transcodeArgs("h264_720p30");
        auto with = [&](std::function<void(QStringList&)> f) { QStringList a = base; f(a); return a; };
        QTest::newRow("unknown option") << with([](QStringList& a) { a.insert(a.size() - 1, "-tune"); a.insert(a.size() - 1, "film"); }) << "unsupported" << "-tune";
        QTest::newRow("other encoder") << with([](QStringList& a) { a[a.indexOf("libx264")] = "libvpx-vp9"; }) << "unsupported" << "libvpx";
        QTest::newRow("other audio encoder") << with([](QStringList& a) { a[a.indexOf("aac")] = "libmp3lame"; }) << "unsupported" << "libmp3lame";
        QTest::newRow("x264 preset") << with([](QStringList& a) { a[a.indexOf("veryfast")] = "turbo"; }) << "unsupported" << "turbo";
        QTest::newRow("trailing option") << with([](QStringList& a) { a << "-shortest"; }) << "unsupported" << "-shortest";
        QTest::newRow("second output") << with([](QStringList& a) { a << "D:\\x\\second.mp4"; }) << "unsupported" << "second";
        QTest::newRow("output extension") << with([](QStringList& a) { a.last() = "D:\\x\\out.mkv"; }) << "unsupported" << "out.mkv";
        QTest::newRow("input format") << with([](QStringList& a) { const int i = a.indexOf("-i"); a.insert(i, "-f"); a.insert(i + 1, "rawvideo"); }) << "unsupported" << "-f rawvideo";
        QTest::newRow("non-file input") << with([](QStringList& a) { a[a.indexOf("-i") + 1] = "http://example.test/clip.mp4"; }) << "unsupported" << "http://";
        QTest::newRow("output option before -i") << with([](QStringList& a) { const int i = a.indexOf("-i"); a.insert(i, "-c:v"); a.insert(i + 1, "libx264"); }) << "unsupported" << "before -i";
        QTest::newRow("progress elsewhere") << with([](QStringList& a) { a[a.indexOf("pipe:1")] = "pipe:2"; }) << "unsupported" << "pipe:2";
        QTest::newRow("unknown filter") << with([](QStringList& a) { a[a.indexOf("-vf") + 1] = "scale=1280:720,ppnofilter"; }) << "unsupported" << "ppnofilter";
        QTest::newRow("copy with encoder options") << with([](QStringList& a) { a[a.indexOf("libx264")] = "copy"; }) << "unsupported" << "-c:v copy";
        QTest::newRow("stream_loop 3") << with([](QStringList& a) { const int i = a.indexOf("-i"); a.insert(i, "-stream_loop"); a.insert(i + 1, "3"); }) << "unsupported" << "-stream_loop 3";
        QTest::newRow("loop on a video") << with([](QStringList& a) { const int i = a.indexOf("-i"); a.insert(i, "-loop"); a.insert(i + 1, "1"); }) << "unsupported" << "-loop 1";
        QTest::newRow("missing value") << with([](QStringList& a) { a.removeLast(); a << "-crf"; }) << "error" << "Missing argument";
        QTest::newRow("-y with -n") << with([](QStringList& a) { a.insert(1, "-n"); }) << "error" << "-y and -n";
        QTest::newRow("bad crf") << with([](QStringList& a) { a[a.indexOf("-crf") + 1] = "99"; }) << "error" << "-crf 99";
        QTest::newRow("bad pix_fmt") << with([](QStringList& a) { a[a.indexOf("-pix_fmt") + 1] = "yuv420z"; }) << "error" << "yuv420z";
        QTest::newRow("bad loglevel") << with([](QStringList& a) { a.insert(0, "-v"); a.insert(1, "chatty"); }) << "error" << "loglevel";
        QTest::newRow("no input") << QStringList{"-y", "D:\\x\\o.mp4"} << "error" << "before any input";
        QTest::newRow("no output") << QStringList{"-y", "-i", kIn, "-c", "copy"} << "error" << "no output file";
        QTest::newRow("map without graph") << with([](QStringList& a) { a.insert(a.size() - 1, "-map"); a.insert(a.size() - 1, "[v]"); }) << "error" << "does not exist";
        // the complex graphs
        const Stories st = storiesArgs(3, true, 0);
        auto withS = [&](std::function<void(QStringList&)> f) { QStringList a = st.args; f(a); return a; };
        QTest::newRow("graph: filter_complex twice") << withS([](QStringList& a) { a.insert(a.indexOf("-map"), "-filter_complex"); a.insert(a.indexOf("-map"), "anull"); }) << "unsupported" << "twice";
        QTest::newRow("graph: unmapped output") << withS([](QStringList& a) { const int i = a.indexOf("[a]"); a.removeAt(i); a.removeAt(i - 1); }) << "error" << "unconnected";
        QTest::newRow("graph: no -map") << withS([](QStringList& a) { for (int k = 0; k < 2; ++k) { const int i = a.indexOf("-map"); a.removeAt(i); a.removeAt(i); } }) << "error" << "unconnected";
        QTest::newRow("graph: unknown map label") << withS([](QStringList& a) { a[a.indexOf("[a]")] = "[audio]"; }) << "error" << "audio";
        QTest::newRow("graph: input past the inputs") << withS([](QStringList& a) { const int i = a.indexOf("-i"); a.removeAt(i); a.removeAt(i); }) << "error" << "Invalid file index";
        QTest::newRow("graph: unknown filter") << withS([](QStringList& a) { a[a.indexOf("-filter_complex") + 1].replace("amix=", "ppnofilter="); }) << "unsupported" << "ppnofilter";
        QTest::newRow("graph: chain without label") << withS([](QStringList& a) { a[a.indexOf("-filter_complex") + 1] += ";[v]format=yuv420p"; }) << "unsupported" << "without an output label";
        QTest::newRow("graph: -vf beside it") << withS([](QStringList& a) { a.insert(a.indexOf("-map"), "-vf"); a.insert(a.indexOf("-map"), "hflip"); }) << "unsupported" << "-vf";
        QTest::newRow("graph: map a file stream") << withS([](QStringList& a) { a[a.indexOf("[a]")] = "0:a"; }) << "unsupported" << "-map 0:a";
    }
    void unsupportedAndErrors() {
        QFETCH(QStringList, args); QFETCH(QString, kind); QFETCH(QString, mention);
        const GraphCommand c = parseGraphArgs(args);
        QVERIFY2(!c.ok(), "parsed although it should not");
        const QString& msg = kind == "unsupported" ? c.unsupported : c.error;
        QVERIFY2(msg.contains(mention, Qt::CaseInsensitive), qPrintable(QString("%1 should mention '%2': unsupported='%3' error='%4'").arg(kind, mention, c.unsupported, c.error)));
        QVERIFY2((kind == "unsupported" ? c.error : c.unsupported).isEmpty(), "only one of unsupported / error is set");
        // a parse without the libavfilter check keeps everything but the filter-name verdicts
        GraphParseOptions o; o.checkFilters = false;
        const GraphCommand d = parseGraphArgs(args, o);
        if (mention != "ppnofilter") QCOMPARE(d.ok(), false); else QVERIFY2(d.ok(), qPrintable(d.unsupported + d.error));
    }

    void toJsonRoundTrip() {
        const Stories s = storiesArgs(3, true, 2);
        const QJsonObject j = parseGraphArgs(s.args).toJson();
        QCOMPARE(j.value("inputs").toArray().size(), 10); QCOMPARE(j.value("graphInputs").toArray().size(), 10);
        QVERIFY(j.value("filters").toArray().contains("xfade")); QCOMPARE(j.value("logLevel").toInt(), 16);
        const QJsonObject in1 = j.value("inputs").toArray().at(2).toObject();   // scene 1: animated
        QVERIFY(in1.value("streamLoop").toBool()); QCOMPARE(in1.value("framerate").toDouble(), 15.0);
        const QJsonObject o = j.value("outputs").toArray().at(0).toObject();
        QCOMPARE(o.value("videoEncoder").toString(), QString("libx264")); QCOMPARE(o.value("maps").toArray(), QJsonArray({"v", "a"})); QVERIFY(o.value("faststart").toBool());
        QCOMPARE(o.value("audioBitrate").toDouble(), 160000.0); QCOMPARE(o.value("t").toDouble(), s.D);
        QCOMPARE(QJsonDocument::fromJson(ppcli::resultLine(j)).object(), j);
    }

    // ---- the exe: parse sub-command, ffmpeg mode, exit codes ------------------------------------------------------
    void exeParse() {
        if (!QFileInfo(m_tool).isFile()) QSKIP("PlayPlayMediaTool not built");
        const Run v = run(m_tool, {"version"}); QCOMPARE(v.code, 0);
        QVERIFY(QJsonDocument::fromJson(v.out.trimmed()).object().value("commands").toArray().contains("parse"));

        const Stories s = storiesArgs(3, true, 2);
        Run r = run(m_tool, QStringList{"parse"} + s.args); QCOMPARE(r.code, 0);
        QJsonObject j = ppcli::parseResult(r.out); QVERIFY(j.value("ok").toBool());
        QCOMPARE(j.value("command").toObject().value("inputs").toArray().size(), 10);
        QCOMPARE(r.out.trimmed().split('\n').size(), 1);

        // ffmpeg mode: in the subset -> the runner, which fails with 2 on the missing inputs (never 64: ffmpeg.exe would
        // fail on them as well, there is nothing to fall back to)
        r = run(m_tool, s.args); QCOMPARE(r.code, ppcli::ExitFailed);
        j = ppcli::parseResult(r.out); QVERIFY(!j.value("ok").toBool() && !j.value("unsupported").toBool()); QVERIFY2(j.value("error").toString().contains("bed0.jpg"), qPrintable(j.value("error").toString()));
        QVERIFY(!ppcli::isUnsupported(r.code, j));
        // an unknown argument -> 64 and {unsupported: true}, both ways
        QStringList bad = transcodeArgs("copy"); bad.insert(bad.size() - 1, "-tune"); bad.insert(bad.size() - 1, "film");
        for (const QStringList& a : {bad, QStringList{"parse"} + bad}) {
            r = run(m_tool, a); QCOMPARE(r.code, ppcli::ExitUnsupported);
            j = ppcli::parseResult(r.out); QVERIFY(j.value("unsupported").toBool()); QVERIFY(j.value("error").toString().contains("-tune")); QVERIFY(!j.contains("parsed"));
        }
        // what ffmpeg refuses -> 2, both ways
        QStringList err = transcodeArgs("copy"); err.insert(1, "-n");
        for (const QStringList& a : {err, QStringList{"parse"} + err}) {
            r = run(m_tool, a); QCOMPARE(r.code, ppcli::ExitFailed);
            j = ppcli::parseResult(r.out); QVERIFY(!j.value("ok").toBool() && !j.value("unsupported").toBool()); QVERIFY(j.value("error").toString().contains("-y and -n"));
        }
        // the thumbnail form still goes to thumb (a missing input fails with 2, not with 64)
        r = run(m_tool, {"-y", "-v", "quiet", "-i", "D:\\nope\\missing.mp4", "-frames:v", "1", "-vf", "scale=320:-2", "-q:v", "4", "D:\\nope\\t.jpg"});
        QCOMPARE(r.code, ppcli::ExitFailed); QVERIFY(!ppcli::parseResult(r.out).value("unsupported").toBool());
    }

    // ---- DEP-05b: the runner against ffmpeg.exe -----------------------------------------------------------------------
    // (a) the 3-scene stories spot: same duration (+-1 frame), stream layout, moov before mdat, PSNR >= 40 dB
    void renderStories() {
        if (!QFileInfo(m_tool).isFile()) QSKIP("PlayPlayMediaTool not built");
        if (!QFileInfo(m_data + "/bed0.jpg").isFile()) QSKIP("fixtures missing (tests/inhouse/data/graph)");
        m_spot = m_tmp.filePath("stories_tool.mp4");
        const Fixture f = fixtureStories(m_data, m_spot, true);
        QVERIFY2(parseGraphArgs(f.args).ok(), "the fixture command line is in the subset");
        Run r = run(m_tool, f.args, 120000);
        QJsonObject j = ppcli::parseResult(r.out);
        QVERIFY2(r.code == 0 && j.value("ok").toBool(), qPrintable(QString::fromUtf8(r.out) + "\n" + QString::fromUtf8(r.err)));
        qInfo("runner: %.2f s for %.3f s of spot (%lld frames, %lld bytes)", j.value("elapsed").toDouble(), j.value("duration").toDouble(), (long long)j.value("frames").toDouble(), (long long)j.value("bytes").toDouble());
        QCOMPARE(qint64(j.value("frames").toDouble()), qint64(f.t.totalFrames));
        QVERIFY(std::fabs(j.value("duration").toDouble() - f.D) <= 1.0 / 30 + 1e-6);
        QCOMPARE(r.out.trimmed().split('\n').size(), 1);                             // -v error, no -progress: the result line only
        const FileInfo ti = probeFile(m_spot); QVERIFY2(ti.ok, qPrintable(ti.err));
        QCOMPARE(ti.streams.size(), 2);
        QCOMPARE(ti.streams[0].codec, QString("h264")); QCOMPARE(ti.streams[0].w, 1920); QCOMPARE(ti.streams[0].h, 1080); QCOMPARE(ti.streams[0].pixFmt, QString("yuv420p"));
        QCOMPARE(ti.streams[0].fps, 30.0); QCOMPARE(ti.streams[0].frames, qint64(f.t.totalFrames)); QVERIFY(std::fabs(ti.streams[0].durationSec - f.D) <= 1.0 / 30 + 1e-6);
        QCOMPARE(ti.streams[1].codec, QString("aac")); QCOMPARE(ti.streams[1].rate, 48000); QCOMPARE(ti.streams[1].ch, 2);
        QVERIFY(std::fabs(ti.streams[1].durationSec - f.D) < 0.03);
        const QStringList atoms = topAtoms(m_spot);
        QVERIFY2(atoms.indexOf("moov") >= 0 && atoms.indexOf("moov") < atoms.indexOf("mdat"), qPrintable(atoms.join(' ')));

        if (m_ffmpeg.isEmpty()) QSKIP("no reference ffmpeg.exe");
        const QString ref = m_tmp.filePath("stories_ffmpeg.mp4");
        QStringList refArgs = f.args; refArgs.last() = ref;
        r = run(m_ffmpeg, refArgs, 120000);
        QVERIFY2(r.code == 0, qPrintable(QString::fromUtf8(r.err)));
        const FileInfo ri = probeFile(ref); QVERIFY(ri.ok);
        QCOMPARE(ri.streams.size(), 2);
        QCOMPARE(ti.streams[0].summary(), ri.streams[0].summary());
        QCOMPARE(ti.streams[1].summary(), ri.streams[1].summary());
        QVERIFY2(std::fabs(ti.durationSec - ri.durationSec) <= 1.0 / 30 + 1e-6, qPrintable(QString("%1 vs %2").arg(ti.durationSec).arg(ri.durationSec)));
        QCOMPARE(topAtoms(ref), atoms);
        int w = 0, h = 0, w2 = 0, h2 = 0;
        const QVector<QByteArray> a = sampleLuma(m_spot, 15, &w, &h), b = sampleLuma(ref, 15, &w2, &h2);
        QCOMPARE(a.size(), b.size()); QVERIFY(a.size() >= 10); QCOMPARE(w, w2); QCOMPARE(h, h2);
        double worst = 999;
        for (int i = 0; i < a.size(); ++i) worst = std::min(worst, psnrDb(a[i], b[i]));
        qInfo("PSNR on %d sampled frames: worst %.1f dB; sizes %lld / %lld bytes%s", a.size(), worst, (long long)QFileInfo(m_spot).size(), (long long)QFileInfo(ref).size(),
              fileBytes(m_spot) == fileBytes(ref) ? " (byte-identical)" : "");
        QVERIFY2(worst >= 40.0, qPrintable(QString("PSNR %1 dB").arg(worst)));
    }

    // (c) -progress pipe:1 on a render-farm transcode, read by suites::progressSeconds as the farm reads it
    void progressParsed() {
        if (m_spot.isEmpty() || !QFileInfo(m_spot).isFile()) QSKIP("no spot rendered (renderStories)");
        const QString out = m_tmp.filePath("t720.mp4");
        QStringList a = kRenderGlobals; a << "-i" << m_spot << suites::presetArgs("h264_720p30") << out;
        const Run r = run(m_tool, a, 120000);
        QVERIFY2(r.code == 0, qPrintable(QString::fromUtf8(r.out) + QString::fromUtf8(r.err)));
        const FileInfo spot = probeFile(m_spot);
        const double last = suites::progressSeconds(r.out);
        QVERIFY2(std::fabs(last - spot.durationSec) < 0.05, qPrintable(QString("progress %1 vs %2").arg(last).arg(spot.durationSec)));
        QVERIFY(r.out.contains("progress=continue\n") || spot.durationSec < 1);
        QVERIFY(r.out.contains("progress=end\n"));
        QVERIFY(r.out.contains("out_time_us=") && r.out.contains("frame=") && r.out.contains("speed="));
        const QJsonObject j = ppcli::parseResult(r.out);                           // the result is still the last line
        QVERIFY(j.value("ok").toBool()); QCOMPARE(QDir::fromNativeSeparators(j.value("out").toString()), out);
        const FileInfo ti = probeFile(out); QCOMPARE(ti.streams[0].w, 1280); QCOMPARE(ti.streams[0].h, 720);
        if (!m_ffmpeg.isEmpty()) {
            QStringList b = a; b.last() = m_tmp.filePath("t720_ffmpeg.mp4");
            const Run rr = run(m_ffmpeg, b, 120000); QCOMPARE(rr.code, 0);
            QVERIFY2(fileBytes(out) == fileBytes(b.last()), "the 720p transcode matches ffmpeg.exe byte for byte");
        }
    }

    // (d) three split segments (the farm's local worker form) joined by the concat remux = the single-pass duration
    void concatSegments() {
        if (m_spot.isEmpty() || !QFileInfo(m_spot).isFile()) QSKIP("no spot rendered (renderStories)");
        const FileInfo spot = probeFile(m_spot);
        const double seg = spot.durationSec / 3;
        QStringList list;
        for (int i = 0; i < 3; ++i) {
            const QString out = m_tmp.filePath(QString("seg_%1.mp4").arg(i));
            QStringList a = kRenderGlobals; a << "-ss" << QString::number(i * seg, 'f', 3) << "-t" << QString::number(seg, 'f', 3) << "-i" << m_spot << suites::presetArgs("proxy_540p") << out;
            const Run r = run(m_tool, a, 120000);
            QVERIFY2(r.code == 0, qPrintable(QString::fromUtf8(r.out) + QString::fromUtf8(r.err)));
            const FileInfo si = probeFile(out); QVERIFY(si.ok);
            QVERIFY2(std::fabs(si.streams[0].durationSec - seg) <= 1.0 / 30 + 1e-6, qPrintable(QString("segment %1: %2 s").arg(i).arg(si.streams[0].durationSec)));
            list << "file '" + QDir::toNativeSeparators(out).replace('\\', "\\\\") + "'";
        }
        const QString txt = m_tmp.filePath("concat.txt");
        { QFile f(txt); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(list.join('\n').toUtf8() + '\n'); }
        const QString joined = m_tmp.filePath("joined.mp4");
        const Run r = run(m_tool, {"-hide_banner", "-nostdin", "-y", "-f", "concat", "-safe", "0", "-i", txt, "-c", "copy", "-movflags", "+faststart", joined}, 120000);
        const QJsonObject j = ppcli::parseResult(r.out);
        QVERIFY2(r.code == 0 && j.value("ok").toBool(), qPrintable(QString::fromUtf8(r.out) + QString::fromUtf8(r.err)));
        QVERIFY(j.value("streams").toArray().at(0).toString().contains("(copy)"));
        const FileInfo ji = probeFile(joined); QVERIFY(ji.ok);
        QVERIFY2(std::fabs(ji.streams[0].durationSec - spot.durationSec) <= 1.0 / 30 + 1e-6, qPrintable(QString("joined %1 vs %2").arg(ji.streams[0].durationSec).arg(spot.durationSec)));
        // +-1 frame: a 3-decimal -t (the farm's) can end a segment one frame before its share (11.2 s / 3 = 3.733 s < 112/30)
        QVERIFY2(std::llabs(ji.streams[0].frames - spot.streams[0].frames) <= 1, qPrintable(QString("%1 vs %2 frames").arg(ji.streams[0].frames).arg(spot.streams[0].frames)));
        QVERIFY(topAtoms(joined).indexOf("moov") < topAtoms(joined).indexOf("mdat"));
        if (!m_ffmpeg.isEmpty()) {
            const QString ref = m_tmp.filePath("joined_ffmpeg.mp4");
            const Run rr = run(m_ffmpeg, {"-hide_banner", "-nostdin", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", txt, "-c", "copy", "-movflags", "+faststart", ref}, 120000);
            QCOMPARE(rr.code, 0);
            QVERIFY2(fileBytes(joined) == fileBytes(ref), "the concat join matches ffmpeg.exe byte for byte");
        }
    }

    // the other render-farm forms: copy segment, audio_wav, loudnorm (video copy + audio graph), copy
    void copyForms_data() {
        QTest::addColumn<QString>("preset"); QTest::addColumn<bool>("segment");
        QTest::newRow("copy segment") << "copy" << true; QTest::newRow("copy") << "copy" << false;
        QTest::newRow("audio_wav") << "audio_wav" << false; QTest::newRow("loudnorm") << "loudnorm" << false; QTest::newRow("h264_1080p30 segment") << "h264_1080p30" << true;
    }
    void copyForms() {
        QFETCH(QString, preset); QFETCH(bool, segment);
        if (m_spot.isEmpty() || !QFileInfo(m_spot).isFile()) QSKIP("no spot rendered (renderStories)");
        const QString out = m_tmp.filePath(QString("%1_%2.%3").arg(preset, segment ? "seg" : "full", suites::presetExt(preset)));
        QStringList a = segment ? QStringList{"-hide_banner", "-nostdin", "-y", "-v", "error"} : kRenderGlobals;
        if (segment) a << "-ss" << "2.500" << "-t" << "3.000";
        a << "-i" << m_spot << suites::presetArgs(preset);
        if (segment && preset == "copy") a << "-avoid_negative_ts" << "make_zero";
        a << out;
        const Run r = run(m_tool, a, 120000);
        const QJsonObject j = ppcli::parseResult(r.out);
        QVERIFY2(r.code == 0 && j.value("ok").toBool(), qPrintable(a.join(' ') + "\n" + QString::fromUtf8(r.out) + QString::fromUtf8(r.err)));
        const FileInfo fi = probeFile(out); QVERIFY(fi.ok);
        if (preset == "audio_wav") { QCOMPARE(fi.streams.size(), 1); QCOMPARE(fi.streams[0].codec, QString("pcm_s16le")); QCOMPARE(fi.streams[0].rate, 48000); }
        else { QCOMPARE(fi.streams.size(), 2); QCOMPARE(fi.streams[0].codec, QString("h264")); QCOMPARE(fi.streams[1].codec, QString("aac")); }
        if (segment && preset != "copy") QVERIFY(std::fabs(fi.streams[0].durationSec - 3.0) <= 1.0 / 30 + 1e-6);
        if (m_ffmpeg.isEmpty()) return;
        QStringList b = a; b.last() = m_tmp.filePath(QString("%1_%2_ffmpeg.%3").arg(preset, segment ? "seg" : "full", suites::presetExt(preset)));
        const Run rr = run(m_ffmpeg, b, 120000); QCOMPARE(rr.code, 0);
        QVERIFY2(fileBytes(out) == fileBytes(b.last()), qPrintable(preset + ": the output matches ffmpeg.exe byte for byte"));
    }

    // failures are 2 (ffmpeg.exe would fail too), never 64; nothing is left behind by -n
    void runnerFailures() {
        if (!QFileInfo(m_tool).isFile()) QSKIP("PlayPlayMediaTool not built");
        if (!QFileInfo(m_data + "/scene0.wav").isFile()) QSKIP("fixtures missing (tests/inhouse/data/graph)");
        const QString out = m_tmp.filePath("exists.mp4");
        { QFile f(out); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("x"); }
        Run r = run(m_tool, {"-n", "-v", "error", "-i", m_data + "/scene0.wav", "-c:a", "aac", out});
        QCOMPARE(r.code, ppcli::ExitFailed); QVERIFY(ppcli::parseResult(r.out).value("error").toString().contains("already exists")); QCOMPARE(QFileInfo(out).size(), qint64(1));
        r = run(m_tool, {"-y", "-v", "error", "-i", m_data + "/nope.wav", "-c:a", "aac", m_tmp.filePath("nope.mp4")});
        QCOMPARE(r.code, ppcli::ExitFailed); QVERIFY(!ppcli::parseResult(r.out).value("unsupported").toBool());
        // in-process: the same verdicts without the exe
        mediatool::RunOptions o; QStringList a = {"-y", "-i", m_data + "/nope.wav", "-c:a", "aac", m_tmp.filePath("nope2.mp4")};
        const mediatool::RunResult rr = mediatool::runGraph(parseGraphArgs(a), o);
        QVERIFY(!rr.ok && rr.averror != 0 && rr.error.contains("nope.wav"));
        QVERIFY(mediatool::isCopyOnly(parseGraphArgs({"-y", "-i", "a.mp4", "-c", "copy", "b.mp4"})));
        QVERIFY(!mediatool::isCopyOnly(parseGraphArgs({"-y", "-i", "a.mp4", "-c:v", "copy", "-c:a", "aac", "b.mp4"})));
        const QByteArray block = mediatool::progressBlock(300, 60.5, 1234.5, 999, 10000000, 1, 2, 2.5, true);
        QCOMPARE(suites::progressSeconds(block), 10.0); QVERIFY(block.endsWith("progress=end\n")); QVERIFY(block.contains("out_time=00:00:10.000000\n"));
    }

private:
    QString m_data, m_ffmpeg, m_spot;
    QTemporaryDir m_tmp;
};

QTEST_GUILESS_MAIN(TestInhouseGraphrunner)
#include "test_inhouse_graphrunner.moc"
