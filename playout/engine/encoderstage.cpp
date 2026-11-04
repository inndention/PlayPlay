#include "encoderstage.h"
#include <QThread>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonValue>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/buffer.h>
#include <libswresample/swresample.h>
}

namespace {
bool envOn(const char* name) {
    const QString v = qEnvironmentVariable(name).trimmed().toLower();
    return v == "1" || v == "on" || v == "true" || v == "yes";
}
size_t alignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
QString avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}
// x264 speed preset -> NVENC p1..p7
QString nvencPreset(const QString& x) {
    if (x == "ultrafast" || x == "superfast" || x.startsWith('p')) return x.startsWith('p') ? x : "p1";
    if (x == "veryfast") return "p2"; if (x == "faster") return "p3"; if (x == "fast" || x == "medium") return "p4";
    if (x == "slow") return "p5"; if (x == "slower") return "p6"; if (x == "veryslow" || x == "placebo") return "p7";
    return "p4";
}
// "4.1" -> 41 (x265 level-idc / nvenc level names accept "4.1")
int levelIdc(const QString& level) { bool ok = false; const double d = level.toDouble(&ok); return ok ? int(d * 10 + 0.5) : 0; }
} // namespace

// ---------------------------------------------------------------------------
// EncoderProfile
// ---------------------------------------------------------------------------

EncoderProfile EncoderProfile::preset(const QString& name0) {
    EncoderProfile p;
    const QString name = name0.isEmpty() ? "h264_aac" : name0;
    p.name = name;
    if (name == "mpeg2_ac3" || name == "mpeg2_ac3_atsc") {
        p.videoCodec = "mpeg2video"; p.videoBitrateKbps = 12000; p.maxrateKbps = 12000; p.bufsizeKbps = 1835;   // MP@HL VBV (1,835,008 bits)
        p.gopFrames = 15; p.bFrames = 2; p.profile = "main"; p.audioCodec = "ac3"; p.audioBitrateKbps = 384;
    } else if (name == "atsc_mpeg2_ac3_hd") {
        p.videoCodec = "mpeg2video"; p.videoBitrateKbps = 12000; p.maxrateKbps = 12000; p.bufsizeKbps = 1835; p.rateControl = "cbr";
        p.gopFrames = 15; p.bFrames = 2; p.profile = "main"; p.interlaced = true; p.audioCodec = "ac3"; p.audioBitrateKbps = 384; p.dialnorm = -27;
    } else if (name == "atsc_mpeg2_ac3_sd") {
        p.videoCodec = "mpeg2video"; p.width = 704; p.height = 480; p.videoBitrateKbps = 4000; p.maxrateKbps = 4000; p.bufsizeKbps = 1835; p.rateControl = "cbr";
        p.gopFrames = 15; p.bFrames = 2; p.profile = "main"; p.interlaced = true; p.audioCodec = "ac3"; p.audioBitrateKbps = 192; p.dialnorm = -27;
    } else if (name == "atsc_h264_ac3") {
        p.videoCodec = "libx264"; p.videoBitrateKbps = 6000; p.maxrateKbps = 6000; p.bufsizeKbps = 6000; p.rateControl = "cbr";
        p.gopFrames = 30; p.bFrames = 2; p.x264Preset = "veryfast"; p.profile = "high"; p.level = "4.1"; p.audioCodec = "ac3"; p.audioBitrateKbps = 384; p.dialnorm = -27;
    } else if (name == "stream_h264_aac_1080p") {
        p.videoCodec = "libx264"; p.videoBitrateKbps = 6000; p.maxrateKbps = 7000; p.bufsizeKbps = 12000; p.rateControl = "vbr";
        p.gopFrames = 60; p.bFrames = 2; p.x264Preset = "veryfast"; p.profile = "high"; p.level = "4.1"; p.audioCodec = "aac"; p.audioBitrateKbps = 160;
    } else if (name == "stream_h264_aac_720p") {
        p.videoCodec = "libx264"; p.width = 1280; p.height = 720; p.videoBitrateKbps = 3500; p.maxrateKbps = 4000; p.bufsizeKbps = 7000; p.rateControl = "vbr";
        p.gopFrames = 60; p.bFrames = 2; p.x264Preset = "veryfast"; p.profile = "main"; p.level = "3.1"; p.audioCodec = "aac"; p.audioBitrateKbps = 128;
    } else if (name == "stream_hevc_aac") {
        p.videoCodec = "libx265"; p.videoBitrateKbps = 4000; p.maxrateKbps = 5000; p.bufsizeKbps = 8000; p.rateControl = "vbr";
        p.gopFrames = 60; p.bFrames = 2; p.x264Preset = "veryfast"; p.profile = "main"; p.level = "4.1"; p.audioCodec = "aac"; p.audioBitrateKbps = 128;
    } else if (name == "qsv_h264_aac") {
        p.videoCodec = "h264_qsv"; p.videoBitrateKbps = 6000; p.maxrateKbps = 6000; p.bufsizeKbps = 12000; p.rateControl = "cbr";
        p.gopFrames = 60; p.bFrames = 2; p.x264Preset = "veryfast"; p.profile = "high"; p.level = "4.1"; p.audioCodec = "aac"; p.audioBitrateKbps = 160;
    } else if (name == "h264_aac_low") {
        p.videoCodec = "libx264"; p.videoBitrateKbps = 2500; p.x264Preset = "veryfast"; p.profile = "main"; p.audioCodec = "aac"; p.audioBitrateKbps = 128;
    } else { // h264_aac default
        p.name = "h264_aac";
    }
    return p;
}

QStringList EncoderProfile::presetNames() {
    return {"h264_aac", "h264_aac_low", "mpeg2_ac3", "atsc_mpeg2_ac3_hd", "atsc_mpeg2_ac3_sd", "atsc_h264_ac3",
            "stream_h264_aac_1080p", "stream_h264_aac_720p", "stream_hevc_aac", "qsv_h264_aac"};
}

QJsonArray EncoderProfile::presetCatalogue() {
    static const QHash<QString, QString> labels = {
        {"h264_aac", "H.264 + AAC (stream, 8 Mb/s)"}, {"h264_aac_low", "H.264 main + AAC 2.5 Mb/s (low)"}, {"mpeg2_ac3", "MPEG-2 + AC-3 (ATSC, legacy name)"},
        {"atsc_mpeg2_ac3_hd", "ATSC MPEG-2 1080i MP@HL 12 Mb/s + AC-3 384k"}, {"atsc_mpeg2_ac3_sd", "ATSC MPEG-2 480i MP@ML 4 Mb/s + AC-3 192k"},
        {"atsc_h264_ac3", "ATSC H.264 high@4.1 6 Mb/s CBR + AC-3 384k"}, {"stream_h264_aac_1080p", "Stream H.264 1080p 6 Mb/s VBR + AAC 160k"},
        {"stream_h264_aac_720p", "Stream H.264 720p 3.5 Mb/s VBR + AAC 128k"}, {"stream_hevc_aac", "Stream HEVC 4 Mb/s VBR + AAC 128k"}, {"qsv_h264_aac", "Intel QSV H.264 6 Mb/s CBR + AAC (needs QSV FFmpeg)"}};
    QJsonArray a;
    for (const QString& n : presetNames()) { QJsonObject o = preset(n).toJson(); o["id"] = n; o["label"] = labels.value(n, n); a.append(o); }
    return a;
}

EncoderProfile EncoderProfile::fromJson(const QJsonObject& o) {
    EncoderProfile p = preset(o.value("preset").toString(o.value("name").toString("h264_aac")));
    if (o.contains("name")) p.name = o["name"].toString();
    // Canonical names, then the aliases used by the mux services / PlayPlayClean output forms.
    if (o.contains("videoCodec")) p.videoCodec = o["videoCodec"].toString(); else if (o.contains("codec")) p.videoCodec = o["codec"].toString();
    if (o.contains("width")) p.width = o["width"].toInt(); if (o.contains("height")) p.height = o["height"].toInt();
    if (o.contains("videoBitrateKbps")) p.videoBitrateKbps = o["videoBitrateKbps"].toInt(); else if (o.contains("vidBitrateKbps")) p.videoBitrateKbps = o["vidBitrateKbps"].toInt();
    if (o.contains("maxrateKbps")) p.maxrateKbps = o["maxrateKbps"].toInt();
    if (o.contains("bufsizeKbps")) p.bufsizeKbps = o["bufsizeKbps"].toInt();
    if (o.contains("rateControl")) p.rateControl = o["rateControl"].toString().toLower();
    if (o.contains("qp")) p.qp = o["qp"].toInt();
    if (o.contains("gopFrames")) p.gopFrames = o["gopFrames"].toInt(); else if (o.contains("gopSize")) p.gopFrames = o["gopSize"].toInt(); else if (o.contains("gop")) p.gopFrames = o["gop"].toInt();
    if (o.contains("keyintMin")) p.keyintMin = o["keyintMin"].toInt();
    if (o.contains("scenecut")) p.scenecut = o["scenecut"].toBool();
    if (o.contains("closedGop")) p.closedGop = o["closedGop"].toBool();
    if (o.contains("bFrames")) p.bFrames = o["bFrames"].toInt(); else if (o.contains("bframes")) p.bFrames = o["bframes"].toInt();
    if (o.contains("x264Preset")) p.x264Preset = o["x264Preset"].toString(); else if (o.contains("speedPreset")) p.x264Preset = o["speedPreset"].toString();
    if (o.contains("profile") && o["profile"].isString()) p.profile = o["profile"].toString();
    if (o.contains("level")) p.level = o["level"].isString() ? o["level"].toString() : QString::number(o["level"].toDouble());
    if (o.contains("interlaced")) p.interlaced = o["interlaced"].toBool();
    if (o.contains("ccPassthrough")) p.ccPassthrough = o["ccPassthrough"].toBool();
    if (o.contains("globalHeader")) p.globalHeader = o["globalHeader"].toBool();
    if (o.contains("afd")) p.afd = o["afd"].toInt(-1);
    if (o.contains("pixelFormat")) p.pixelFormat = o["pixelFormat"].toString();
    if (o.contains("threads")) p.threads = o["threads"].toInt();
    if (o.contains("audioCodec")) p.audioCodec = o["audioCodec"].toString(); else if (o.contains("audCodec")) p.audioCodec = o["audCodec"].toString();
    if (o.contains("audioBitrateKbps")) p.audioBitrateKbps = o["audioBitrateKbps"].toInt(); else if (o.contains("audBitrateKbps")) p.audioBitrateKbps = o["audBitrateKbps"].toInt();
    if (o.contains("audioChannels")) p.audioChannels = o["audioChannels"].toInt();
    if (o.contains("audioSampleRate")) p.audioSampleRate = o["audioSampleRate"].toInt();
    if (o.contains("dialnorm")) p.dialnorm = o["dialnorm"].toInt();
    if (o.contains("mixingLevel")) p.mixingLevel = o["mixingLevel"].toInt();
    if (o.contains("aacProfile")) p.aacProfile = o["aacProfile"].toString();
    if (o.contains("loudnessTargetLkfs")) p.loudnessTargetLkfs = o["loudnessTargetLkfs"].toDouble();
    // nested audio object (mux service shape)
    const QJsonObject au = o.value("audio").toObject();
    if (!au.isEmpty()) {
        if (au.contains("codec")) p.audioCodec = au["codec"].toString(); if (au.contains("bitrateKbps")) p.audioBitrateKbps = au["bitrateKbps"].toInt();
        if (au.contains("channels")) p.audioChannels = au["channels"].toInt(); if (au.contains("sampleRate")) p.audioSampleRate = au["sampleRate"].toInt();
        if (au.contains("dialnorm")) p.dialnorm = au["dialnorm"].toInt(); if (au.contains("mixingLevel")) p.mixingLevel = au["mixingLevel"].toInt();
        if (au.contains("aacProfile")) p.aacProfile = au["aacProfile"].toString();
    }
    if (o.contains("extra")) { const QJsonValue x = o["extra"]; if (x.isObject()) p.extra = x.toObject(); else if (x.isString() && !x.toString().isEmpty()) p.extra["x264-params"] = x.toString(); }
    if (p.rateControl != "cbr" && p.rateControl != "vbr" && p.rateControl != "cqp") p.rateControl = (p.maxrateKbps == 0 || p.maxrateKbps == p.videoBitrateKbps) ? "cbr" : "vbr";
    if (p.rateControl == "cbr") p.maxrateKbps = p.videoBitrateKbps;
    return p;
}

QJsonObject EncoderProfile::toJson() const {
    QJsonObject o;
    o["name"] = name; o["videoCodec"] = videoCodec; o["videoBitrateKbps"] = videoBitrateKbps;
    if (width > 0 && height > 0) { o["width"] = width; o["height"] = height; }
    o["maxrateKbps"] = maxrateKbps; o["bufsizeKbps"] = bufsizeKbps; o["rateControl"] = rateControl; if (rateControl == "cqp") o["qp"] = qp;
    o["gopFrames"] = gopFrames; o["keyintMin"] = keyintMin; o["scenecut"] = scenecut; o["closedGop"] = closedGop;
    o["bFrames"] = bFrames; o["x264Preset"] = x264Preset; o["profile"] = profile; if (!level.isEmpty()) o["level"] = level; o["interlaced"] = interlaced;
    o["ccPassthrough"] = ccPassthrough; if (afd >= 0) o["afd"] = afd;
    o["pixelFormat"] = pixelFormat; if (threads) o["threads"] = threads;
    if (globalHeader) o["globalHeader"] = true;
    o["audioCodec"] = audioCodec; o["audioBitrateKbps"] = audioBitrateKbps;
    o["audioChannels"] = audioChannels; o["audioSampleRate"] = audioSampleRate;
    o["dialnorm"] = dialnorm; if (mixingLevel >= 0) o["mixingLevel"] = mixingLevel; o["aacProfile"] = aacProfile; o["loudnessTargetLkfs"] = loudnessTargetLkfs;
    if (!extra.isEmpty()) o["extra"] = extra;
    return o;
}

// ---------------------------------------------------------------------------
// EncoderStage
// ---------------------------------------------------------------------------

EncoderStage::EncoderStage(const EncoderProfile& profile, int width, int height, const FrameRate& rate, QObject* parent)
    : QThread(parent), m_profile(profile), m_width(width), m_height(height), m_rate(rate)
{
    // Encoded size: the profile's (even, <= canvas) or the canvas.
    m_outW = (profile.width > 0 && profile.height > 0) ? qMin(width, profile.width) & ~1 : width;
    m_outH = (profile.width > 0 && profile.height > 0) ? qMin(height, profile.height) & ~1 : height;
    if (m_outW < 16 || m_outH < 16) { m_outW = width; m_outH = height; }
}

EncoderStage::~EncoderStage() {
    shutdown();
}

void EncoderStage::setError(const QString& msg) {
    { QMutexLocker l(&m_errMutex); m_lastError = msg; }
    qWarning() << "[Encoder]" << m_profile.name << msg;
    emit encoderError(msg);
}

EncoderStage::Stats EncoderStage::stats() const {
    QMutexLocker l(&m_statMutex);
    Stats s = m_stats;
    s.yuvAllocs += m_poolAllocs.load(std::memory_order_relaxed);
    { QMutexLocker q(&m_qmutex); s.queueDepth = int(m_queue.size()); }
    return s;
}

int EncoderStage::videoThreadCount(const QString& codecName, int profileThreads, int logicalCpus, const QString& env) {
    if (profileThreads != 0) return profileThreads;                                   // a profile's own count always wins
    if (codecName != "libx264" && codecName != "libx265") return 0;                    // mpeg2video / hardware: their default
    const QString e = env.trimmed().toLower();
    if (e == "auto" || e == "0") return 0;                                             // off switch: the codec's own count
    bool ok = false; const int n = e.toInt(&ok);
    if (ok && n > 0) return n;
    // x264's own count is 1.5 x logical CPUs (48 here): every frame thread holds pictures + lookahead and adds a frame of
    // latency (measured: +49 threads, +1.1 GB, first packet after 47 frames). Half the CPUs, at most 12, keeps 1080p
    // veryfast well inside real time on this class of box.
    return qBound(1, logicalCpus / 2, 12);
}

bool EncoderStage::initialize() {
    if (m_running.load()) return true;
    if (!openVideoEncoder()) { freeAll(); return false; }
    if (!openAudioEncoder()) { freeAll(); return false; }
    m_pkt = av_packet_alloc();
    m_stop = false;
    m_running = true;
    start(QThread::HighPriority);
    qInfo().noquote() << QString("[Encoder] %1 started: %2 %3 kbps %4, %5 %6 kbps, %7x%8 @%9%10")
        .arg(m_profile.name).arg(m_actualVideoCodec).arg(m_profile.videoBitrateKbps).arg(m_profile.rateControl)
        .arg(m_actualAudioCodec).arg(m_profile.audioBitrateKbps)
        .arg(m_outW).arg(m_outH).arg(m_rate.toString()).arg(m_outW != m_width || m_outH != m_height ? QString(" (scaled from %1x%2)").arg(m_width).arg(m_height) : QString());
    return true;
}

void EncoderStage::shutdown(bool drain) {
    if (!m_running.load() && !isRunning()) { freeAll(); return; }
    m_drainOnStop = drain;
    m_stop = true;
    { QMutexLocker l(&m_qmutex); m_qcond.wakeAll(); }
    if (drain) wait(); else wait(5000);
    m_running = false;
    {
        QMutexLocker l(&m_sinkMutex);
        for (PacketSink* s : m_sinks) s->stop();
        m_sinks.clear();
    }
    freeAll();
}

void EncoderStage::freeAll() {
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_swsAlt) { sws_freeContext(m_swsAlt); m_swsAlt = nullptr; m_altW = m_altH = 0; m_altFmt = -1; }
    if (m_yuv) av_frame_free(&m_yuv);
    if (m_lastYuv) av_frame_free(&m_lastYuv);
    if (m_aframe) av_frame_free(&m_aframe);
    if (m_afifo) { av_audio_fifo_free(m_afifo); m_afifo = nullptr; }
    if (m_pkt) av_packet_free(&m_pkt);
    if (m_vctx) avcodec_free_context(&m_vctx);
    if (m_actx) avcodec_free_context(&m_actx);
    if (m_vpar) avcodec_parameters_free(&m_vpar);
    if (m_apar) avcodec_parameters_free(&m_apar);
    // After the codec let go of its pictures; the pool itself is freed once its last buffer comes back.
    if (m_yuvPool) av_buffer_pool_uninit(&m_yuvPool);
}

// Requested encoder, else the software encoder of the same family (hevc -> libx265 -> libx264).
const AVCodec* EncoderStage::resolveVideoCodec() {
    const QString want = m_profile.videoCodec.isEmpty() ? QString("libx264") : m_profile.videoCodec;
    const AVCodec* codec = avcodec_find_encoder_by_name(want.toUtf8().constData());
    if (codec) return codec;
    const bool hevc = want.contains("265") || want.contains("hevc");
    const AVCodec* fb = hevc ? avcodec_find_encoder_by_name("libx265") : nullptr;
    if (!fb) fb = avcodec_find_encoder_by_name("libx264");
    if (!fb) fb = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (fb) { qWarning().noquote() << QString("[Encoder] %1: video codec '%2' is not in this FFmpeg build - falling back to %3").arg(m_profile.name, want, fb->name); emit encoderError(QString("codec %1 unavailable, using %2").arg(want, fb->name)); }
    return fb;
}

// Codec-specific option mapping from the profile fields (unknown options are ignored by avcodec_open2).
void EncoderStage::applyVideoOptions(const QString& cname, AVDictionary** opts, bool interlaced) {
    const int gop = m_vctx->gop_size; const int kmin = m_profile.keyintMin > 0 ? qMin(m_profile.keyintMin, gop) : gop;
    const QString rc = m_profile.rateControl;
    if (cname == "libx264") {
        av_dict_set(opts, "preset", m_profile.x264Preset.toUtf8().constData(), 0);
        av_dict_set(opts, "profile", m_profile.profile.toUtf8().constData(), 0);
        if (!m_profile.level.isEmpty()) av_dict_set(opts, "level", m_profile.level.toUtf8().constData(), 0);
        av_dict_set(opts, "a53cc", m_profile.ccPassthrough ? "1" : "0", 0);
        // HRD signalling + SPS/PPS before every keyframe for stream joiners; fixed GOP for the mux/splicers.
        QString params = QString("repeat-headers=1:force-cfr=1:keyint=%1:min-keyint=%2:scenecut=%3:open-gop=%4")
            .arg(gop).arg(kmin).arg(m_profile.scenecut ? 40 : 0).arg(m_profile.closedGop ? 0 : 1);
        if (rc == "cqp") { av_dict_set(opts, "qp", QByteArray::number(m_profile.qp), 0); }
        else params += QString(":nal-hrd=%1").arg(rc == "cbr" ? "cbr" : "vbr");
        if (interlaced) params += ":tff=1";
        if (m_profile.extra.contains("x264-params")) params += ":" + m_profile.extra.value("x264-params").toString();
        av_dict_set(opts, "x264-params", params.toUtf8().constData(), 0);
    } else if (cname == "libx265") {
        av_dict_set(opts, "preset", m_profile.x264Preset.toUtf8().constData(), 0);
        av_dict_set(opts, "profile", m_profile.profile == "high" ? "main" : m_profile.profile.toUtf8().constData(), 0);
        QString params = QString("repeat-headers=1:keyint=%1:min-keyint=%2:scenecut=%3:open-gop=%4:bframes=%5")
            .arg(gop).arg(kmin).arg(m_profile.scenecut ? 40 : 0).arg(m_profile.closedGop ? 0 : 1).arg(m_profile.bFrames);
        if (rc == "cqp") params += QString(":qp=%1").arg(m_profile.qp);
        else { params += QString(":vbv-maxrate=%1:vbv-bufsize=%2").arg(m_vctx->rc_max_rate / 1000).arg(m_vctx->rc_buffer_size / 1000); if (rc == "cbr") params += ":strict-cbr=1"; }
        if (levelIdc(m_profile.level)) params += QString(":level-idc=%1").arg(levelIdc(m_profile.level));
        if (interlaced) params += ":interlace=tff";
        // PE-09: x265 sizes its worker pool from every CPU unless `pools` says otherwise: same cap as x264 when the count
        // came from the cap (not from the profile), unless the profile's own x265-params name the pools.
        if (m_profile.threads == 0 && m_videoThreads > 0 && !m_profile.extra.value("x265-params").toString().contains("pools"))
            params += QString(":pools=%1").arg(m_videoThreads);
        if (m_profile.extra.contains("x265-params")) params += ":" + m_profile.extra.value("x265-params").toString();
        av_dict_set(opts, "x265-params", params.toUtf8().constData(), 0);
    } else if (cname == "mpeg2video") {
        if (rc == "cbr") m_vctx->rc_min_rate = m_vctx->rc_max_rate;          // CBR
        if (m_profile.bufsizeKbps <= 0) m_vctx->rc_buffer_size = m_outH > 576 ? 1835008 : 1835008; // MP@HL / MP@ML VBV (both 1,835,008 bits)
        // qmax 28: mpeg2video only supports non_linear_quant (set below) up to qmax 28. With 31 avcodec_open2 failed with
        // "Not yet implemented" and every MPEG-2 preset silently fell back to libx264 (H.264 that ATSC 1.0 receivers cannot decode).
        m_vctx->qmin = 1; m_vctx->qmax = 28;
#ifdef AV_PROFILE_MPEG2_MAIN
        m_vctx->profile = AV_PROFILE_MPEG2_MAIN;
#else
        m_vctx->profile = 4;
#endif
        m_vctx->level = m_outH > 576 ? 4 /* HL */ : 8 /* ML */;
        if (rc == "cqp") { m_vctx->flags |= AV_CODEC_FLAG_QSCALE; m_vctx->global_quality = m_profile.qp * FF_QP2LAMBDA; }
        av_dict_set(opts, "sc_threshold", m_profile.scenecut ? "0" : "1000000000", 0);   // closed GOP is unsupported with scene-cut detection
        av_dict_set(opts, "intra_vlc", "1", 0);
        av_dict_set(opts, "non_linear_quant", "1", 0);
        av_dict_set(opts, "seq_disp_ext", "1", 0);
        av_dict_set(opts, "video_format", "component", 0);
        av_dict_set(opts, "a53cc", m_profile.ccPassthrough ? "1" : "0", 0);
        if (interlaced) av_dict_set(opts, "alternate_scan", "1", 0);
    } else if (cname.contains("nvenc")) {
        av_dict_set(opts, "preset", nvencPreset(m_profile.x264Preset).toUtf8().constData(), 0);
        av_dict_set(opts, "profile", (cname.contains("hevc") && m_profile.profile == "high") ? "main" : m_profile.profile.toUtf8().constData(), 0);
        if (!m_profile.level.isEmpty()) av_dict_set(opts, "level", m_profile.level.toUtf8().constData(), 0);
        av_dict_set(opts, "rc", rc == "cbr" ? "cbr" : rc == "cqp" ? "constqp" : "vbr", 0);
        if (rc == "cqp") av_dict_set(opts, "qp", QByteArray::number(m_profile.qp), 0);
        av_dict_set(opts, "zerolatency", "1", 0);
        av_dict_set(opts, "forced-idr", "1", 0);
        av_dict_set(opts, "no-scenecut", m_profile.scenecut ? "0" : "1", 0);
        av_dict_set(opts, "strict_gop", "1", 0);
        av_dict_set(opts, "a53cc", m_profile.ccPassthrough ? "1" : "0", 0);
        av_dict_set(opts, "repeat_headers", "1", 0);
    } else if (cname.contains("qsv")) {
        av_dict_set(opts, "preset", m_profile.x264Preset.toUtf8().constData(), 0);
        av_dict_set(opts, "profile", (cname.contains("hevc") && m_profile.profile == "high") ? "main" : m_profile.profile.toUtf8().constData(), 0);
        av_dict_set(opts, "look_ahead", "0", 0);
        av_dict_set(opts, "forced_idr", "1", 0);
        av_dict_set(opts, "a53cc", m_profile.ccPassthrough ? "1" : "0", 0);
        if (rc == "cqp") { m_vctx->flags |= AV_CODEC_FLAG_QSCALE; m_vctx->global_quality = m_profile.qp; m_vctx->bit_rate = 0; m_vctx->rc_max_rate = 0; }
        else if (rc == "cbr") m_vctx->rc_min_rate = m_vctx->rc_max_rate;    // qsv picks CBR when min = max = bitrate
    } else if (cname.contains("amf") || cname.contains("_mf")) {
        av_dict_set(opts, "profile", m_profile.profile.toUtf8().constData(), 0);
        av_dict_set(opts, "rc", rc == "cbr" ? "cbr" : rc == "cqp" ? "cqp" : "vbr_peak", 0);
    }
}

bool EncoderStage::openVideoEncoder() {
    const AVCodec* codec = resolveVideoCodec();
    if (!codec) { setError("no H.264/HEVC/MPEG-2 encoder in this FFmpeg build"); return false; }
    m_actualVideoCodec = codec->name;
    const QString cname = codec->name;
    // Field coding only for the interlaced broadcast rasters (1080i, 480i, 576i); 720p/1080p stay progressive.
    const bool interlaced = m_profile.interlaced && (m_outH >= 1080 || m_outH == 480 || m_outH == 576);
    if (m_profile.interlaced && !interlaced) qWarning().noquote() << QString("[Encoder] %1: interlaced requested on %2 lines - encoding progressive").arg(m_profile.name).arg(m_outH);

    m_vctx = avcodec_alloc_context3(codec);
    if (m_profile.globalHeader) m_vctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    m_vctx->width = m_outW;
    m_vctx->height = m_outH;
    m_vctx->time_base = AVRational{m_rate.den, m_rate.num};
    m_vctx->framerate = AVRational{m_rate.num, m_rate.den};
    m_vctx->pix_fmt = av_get_pix_fmt(m_profile.pixelFormat.toUtf8().constData());
    if (m_vctx->pix_fmt == AV_PIX_FMT_NONE) m_vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (cname.contains("qsv")) m_vctx->pix_fmt = AV_PIX_FMT_NV12;
    m_vctx->bit_rate = qint64(m_profile.videoBitrateKbps) * 1000;
    const int maxrate = (m_profile.rateControl == "vbr" && m_profile.maxrateKbps > 0) ? m_profile.maxrateKbps : m_profile.videoBitrateKbps;
    m_vctx->rc_max_rate = qint64(maxrate) * 1000;
    m_vctx->rc_buffer_size = int(qint64(m_profile.bufsizeKbps > 0 ? m_profile.bufsizeKbps : 2 * maxrate) * 1000);
    m_vctx->gop_size = m_profile.gopFrames > 0 ? m_profile.gopFrames : qMax(1, m_rate.nominalFps());
    m_vctx->keyint_min = m_profile.keyintMin > 0 ? qMin(m_profile.keyintMin, m_vctx->gop_size) : m_vctx->gop_size;
    m_vctx->max_b_frames = m_profile.bFrames;
    // PE-09 switches, read once per open (the libx264 fallback below re-runs this with its own codec name).
    m_videoThreads = videoThreadCount(cname, m_profile.threads, QThread::idealThreadCount(), qEnvironmentVariable("PLAYPLAY_ENC_THREADS"));
    m_pooled = !envOn("PLAYPLAY_ENC_LEGACY_FRAMES");
    m_fastRnd = envOn("PLAYPLAY_ENC_FAST_RND");
    // libx265 turns thread_count into x265 *frame* threads (12 frame threads = 12 frames more latency than x265's own 3-6),
    // so a capped count goes to its worker pool instead (pools=N in applyVideoOptions) and x265 picks its frame threads
    // from that pool. A profile's own `threads` still reaches thread_count as before.
    m_vctx->thread_count = (cname == "libx265" && m_profile.threads == 0) ? 0 : m_videoThreads;
    m_vctx->color_range = AVCOL_RANGE_MPEG;
    m_vctx->colorspace = AVCOL_SPC_BT709;
    m_vctx->color_primaries = AVCOL_PRI_BT709;
    m_vctx->color_trc = AVCOL_TRC_BT709;
    m_vctx->sample_aspect_ratio = m_outW == 704 && m_outH == 480 ? AVRational{40, 33} : AVRational{1, 1};   // 704x480 anamorphic 16:9
    if (m_profile.closedGop) m_vctx->flags |= AV_CODEC_FLAG_CLOSED_GOP;
    if (interlaced) {
        m_vctx->flags |= AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME;
        m_vctx->field_order = AV_FIELD_TT;
    }

    AVDictionary* opts = nullptr;
    applyVideoOptions(cname, &opts, interlaced);
    for (auto it = m_profile.extra.begin(); it != m_profile.extra.end(); ++it) {
        if (it.key() == "x264-params" || it.key() == "x265-params") continue;   // merged above
        av_dict_set(&opts, it.key().toUtf8().constData(), it.value().toVariant().toString().toUtf8().constData(), 0);
    }

    int ret = avcodec_open2(m_vctx, codec, &opts);
    if (ret < 0 && cname != "libx264") {
        // A hardware encoder that is present but cannot open (no GPU / driver): software fallback.
        qWarning().noquote() << QString("[Encoder] %1: %2 open failed (%3) - falling back to libx264").arg(m_profile.name, cname, avErr(ret));
        av_dict_free(&opts); avcodec_free_context(&m_vctx);
        const AVCodec* sw = avcodec_find_encoder_by_name("libx264");
        if (sw) {
            EncoderProfile saved = m_profile; m_profile.videoCodec = "libx264"; if (cname.contains("hevc") || cname.contains("265")) m_profile.profile = "high";
            const bool ok = openVideoEncoder(); m_profile.videoCodec = saved.videoCodec; m_profile.profile = saved.profile;
            if (ok) emit encoderError(QString("%1 could not open (%2), using libx264").arg(cname, avErr(ret)));
            return ok;
        }
        setError("video encoder open failed: " + avErr(ret)); return false;
    }
    av_dict_free(&opts);
    if (ret < 0) { setError("video encoder open failed: " + avErr(ret)); return false; }

    m_vpar = avcodec_parameters_alloc();
    avcodec_parameters_from_context(m_vpar, m_vctx);

    m_yuv = av_frame_alloc();
    m_lastYuv = av_frame_alloc();
    if (!m_yuv || !m_lastYuv) { setError("yuv frame alloc failed"); return false; }
    m_yuv->format = m_vctx->pix_fmt;
    m_yuv->width = m_outW;
    m_yuv->height = m_outH;
    if (m_pooled && !setupYuvPool()) {
        qWarning().noquote() << QString("[Encoder] %1: no picture pool for %2 - using av_frame_make_writable").arg(m_profile.name, av_get_pix_fmt_name(m_vctx->pix_fmt));
        m_pooled = false;
    }
    if (m_pooled ? !takePooledPicture() : av_frame_get_buffer(m_yuv, 32) < 0) { setError("yuv frame alloc failed"); return false; }
    if (!m_pooled) { QMutexLocker s(&m_statMutex); m_stats.yuvAllocs++; }
    {   // start black: a picture repeated before the first real one (encodeVideo) must not be uninitialised memory
        ptrdiff_t ls[4] = {m_yuv->linesize[0], m_yuv->linesize[1], m_yuv->linesize[2], m_yuv->linesize[3]};
        av_image_fill_black(m_yuv->data, ls, AVPixelFormat(m_yuv->format), AVCOL_RANGE_MPEG, m_yuv->width, m_yuv->height);
    }
    if (m_pooled && av_frame_ref(m_lastYuv, m_yuv) < 0) { setError("yuv frame ref failed"); return false; }
    const bool scaled = (m_outW != m_width || m_outH != m_height);
    const int rnd = m_fastRnd ? 0 : SWS_ACCURATE_RND;
    // RGB -> YUV on several cores: one core needs ~10 ms per 1080p picture (a third of the frame budget), six need ~2 ms.
    m_sws = sws_alloc_context();
    if (!m_sws) { setError("sws context failed"); return false; }
    av_opt_set_int(m_sws, "srcw", m_width, 0); av_opt_set_int(m_sws, "srch", m_height, 0); av_opt_set_int(m_sws, "src_format", kMixerPixelFormat, 0);
    av_opt_set_int(m_sws, "dstw", m_outW, 0); av_opt_set_int(m_sws, "dsth", m_outH, 0); av_opt_set_int(m_sws, "dst_format", m_vctx->pix_fmt, 0);
    av_opt_set_int(m_sws, "sws_flags", (scaled ? SWS_BICUBIC : SWS_BILINEAR) | rnd, 0);
    av_opt_set_int(m_sws, "threads", qBound(1, QThread::idealThreadCount() / 4, 6), 0);
    if (sws_init_context(m_sws, nullptr, nullptr) < 0) { sws_freeContext(m_sws); m_sws = nullptr; setError("sws context failed"); return false; }
    // BT.709 limited range for the RGB->YUV matrix.
    {
        int* inv = nullptr; int* tbl = nullptr; int srcRange, dstRange, br, con, sat;
        if (sws_getColorspaceDetails(m_sws, &inv, &srcRange, &tbl, &dstRange, &br, &con, &sat) >= 0) {
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                     sws_getCoefficients(SWS_CS_ITU709), 0, br, con, sat);
        }
    }
    return true;
}

bool EncoderStage::openAudioEncoder() {
    const AVCodec* codec = avcodec_find_encoder_by_name(m_profile.audioCodec.toUtf8().constData());
    if (!codec) {
        qWarning() << "[Encoder] audio codec not available:" << m_profile.audioCodec << "- falling back to aac";
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    }
    if (!codec) { setError("no audio encoder"); return false; }
    m_actualAudioCodec = codec->name;

    m_actx = avcodec_alloc_context3(codec);
    m_actx->sample_rate = m_profile.audioSampleRate;
    av_channel_layout_default(&m_actx->ch_layout, m_profile.audioChannels);
    m_actx->bit_rate = qint64(m_profile.audioBitrateKbps) * 1000;
    m_actx->time_base = AVRational{1, m_actx->sample_rate};
    // Prefer planar float (our native format); otherwise the codec's first format.
    m_actx->sample_fmt = AV_SAMPLE_FMT_NONE;
    const enum AVSampleFormat* fmts = nullptr;
    int nfmts = 0;
    if (avcodec_get_supported_config(m_actx, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     reinterpret_cast<const void**>(&fmts), &nfmts) >= 0 && fmts) {
        for (int i = 0; i < nfmts; ++i) if (fmts[i] == AV_SAMPLE_FMT_FLTP) m_actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (m_actx->sample_fmt == AV_SAMPLE_FMT_NONE && nfmts > 0) m_actx->sample_fmt = fmts[0];
    }
    if (m_actx->sample_fmt == AV_SAMPLE_FMT_NONE) m_actx->sample_fmt = AV_SAMPLE_FMT_FLTP;

    AVDictionary* opts = nullptr;
    const QString an = codec->name;
    if (an == "ac3" || an == "eac3" || an == "ac3_fixed") {
        // ATSC A/52 metadata: dialogue level (A/85 recommends -27 for typical program), optional mixing level.
        av_dict_set(&opts, "dialnorm", QByteArray::number(qBound(-31, m_profile.dialnorm, -1)), 0);
        if (m_profile.mixingLevel >= 0) av_dict_set(&opts, "mixing_level", QByteArray::number(qBound(0, m_profile.mixingLevel, 111)), 0);
        av_dict_set(&opts, "dsur_mode", m_profile.audioChannels == 2 ? "notindicated" : "off", 0);
    } else if (an == "aac") {
        av_dict_set(&opts, "profile", m_profile.aacProfile.isEmpty() ? "aac_low" : m_profile.aacProfile.toUtf8().constData(), 0);
        av_dict_set(&opts, "aac_coder", "twoloop", 0);
    }
    int ret = avcodec_open2(m_actx, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) { setError("audio encoder open failed: " + avErr(ret)); return false; }
    if (m_actx->frame_size <= 0) m_actx->frame_size = 1024;

    m_apar = avcodec_parameters_alloc();
    avcodec_parameters_from_context(m_apar, m_actx);

    m_afifo = av_audio_fifo_alloc(m_actx->sample_fmt, m_actx->ch_layout.nb_channels, m_actx->frame_size * 4);
    m_aframe = av_frame_alloc();
    m_aframe->format = m_actx->sample_fmt;
    m_aframe->sample_rate = m_actx->sample_rate;
    av_channel_layout_copy(&m_aframe->ch_layout, &m_actx->ch_layout);
    m_aframe->nb_samples = m_actx->frame_size;
    if (av_frame_get_buffer(m_aframe, 0) < 0) { setError("audio frame alloc failed"); return false; }
    return true;
}

bool EncoderStage::addSink(PacketSink* sink) {
    if (!sink || !m_vpar) return false;
    if (!sink->start(m_vpar, m_vctx->time_base, m_apar, m_actx ? m_actx->time_base : AVRational{1, 48000})) {
        qWarning() << "[Encoder] sink failed to start:" << sink->name();
        return false;
    }
    QMutexLocker l(&m_sinkMutex);
    m_sinks.append(sink);
    return true;
}

void EncoderStage::removeSink(PacketSink* sink) {
    QMutexLocker l(&m_sinkMutex);
    m_sinks.removeAll(sink);
}

void EncoderStage::pushFrame(const MediaFrame& frame) {
    QMutexLocker l(&m_qmutex);
    if (int(m_queue.size()) >= kMaxQueue) {
        m_queue.pop_front();
        m_skipped.fetch_add(1);          // run() moves the video clock past it, so the stream stays on real time
        QMutexLocker s(&m_statMutex);
        m_stats.framesDropped++;
    }
    m_queue.push_back(frame);
    { QMutexLocker s(&m_statMutex); m_stats.framesIn++; }
    m_qcond.wakeOne();
}

void EncoderStage::run() {
    while (true) {
        MediaFrame frame;
        {
            QMutexLocker l(&m_qmutex);
            while (m_queue.empty() && !m_stop.load()) m_qcond.wait(&m_qmutex, 100);
            if (m_stop.load() && (!m_drainOnStop.load() || m_queue.empty())) break;
            frame = std::move(m_queue.front());
            m_queue.pop_front();
        }
        // Frames the queue dropped (encoder too slow for a moment) leave a gap in the video timestamps instead of compressing
        // the timeline: otherwise every drop puts the stream one frame further behind the muxer's clock ("dts < pcr" once
        // the drops add up to the mux delay). encodeAudio then pads the matching silence.
        if (const int skip = m_skipped.exchange(0); skip > 0 && m_vctx) m_videoPts += skip;
        encodeFrame(frame);
    }
    flush();
}

void EncoderStage::encodeFrame(const MediaFrame& frame) {
    QElapsedTimer t; t.start();
    encodeVideo(frame.video);          // even without a picture: the video clock must advance with the audio (repeats the last picture)
    encodeAudio(frame.audio);          // even without samples: keeps the audio on the video clock (pads silence)
    const double ms = t.nsecsElapsed() / 1e6;
    QMutexLocker s(&m_statMutex);
    m_stats.framesEncoded++;
    m_stats.lastEncodeMs = ms;
    m_stats.avgEncodeMs = m_stats.avgEncodeMs * 0.95 + ms * 0.05;
}

void EncoderStage::encodeVideo(const FramePtr& video) {
    // Every program frame gets a video timestamp. Audio pts counts samples, so a picture skipped here puts audio ahead of
    // video for good: on 2026-10-23 main's UDP output carried a 1280x720 NDI pass-through frame that was skipped for ~32 s,
    // and the TS muxer logged "dts < pcr, TS is invalid" for every video packet after it. A picture of another size or
    // format is scaled; one that cannot be used repeats the previous picture (m_lastYuv, or legacy m_yuv, still holds it).
    if (!m_vctx || !m_yuv) return;
    bool ok = false;
    if (m_pooled) {
        // PE-09: a fresh pool buffer per picture, so the one the codec may still hold is never written or copied. Our own
        // references are m_yuv and (when it is the same picture) m_lastYuv; any beyond those belong to the codec.
        if (m_yuv->buf[0]) {
            const int ours = (m_lastYuv->buf[0] && m_lastYuv->buf[0]->data == m_yuv->buf[0]->data) ? 2 : 1;
            if (av_buffer_get_ref_count(m_yuv->buf[0]) > ours) { QMutexLocker s(&m_statMutex); m_stats.yuvCodecHeld++; }
        }
        if (video && video->width > 0 && video->height > 0 && takePooledPicture()) ok = convertPicture(video);
        if (ok) { av_frame_unref(m_lastYuv); av_frame_ref(m_lastYuv, m_yuv); }
        else {
            // Repeat: a second reference to the last good picture, no copy (a failed conversion leaves nothing half-written
            // on air either: its buffer just goes back to the pool).
            av_frame_unref(m_yuv);
            if (!m_lastYuv->buf[0] || av_frame_ref(m_yuv, m_lastYuv) < 0) {
                m_videoPts++;                    // keep the video clock on time even without a picture to send
                setError("no picture to repeat");
                return;
            }
        }
    } else {
        // Legacy (PLAYPLAY_ENC_LEGACY_FRAMES=1): one frame, made writable again; that reallocates and copies the previous
        // picture whenever the codec still references it.
        const bool held = !av_frame_is_writable(m_yuv);
        const bool writable = av_frame_make_writable(m_yuv) >= 0;
        if (held) { QMutexLocker s(&m_statMutex); m_stats.yuvCodecHeld++; if (writable) { m_stats.yuvCopies++; m_stats.yuvAllocs++; } }
        ok = writable && convertPicture(video);
    }
    if (!ok) { QMutexLocker s(&m_statMutex); m_stats.videoRepeated++; }
    m_yuv->pts = m_videoPts++;
    m_yuv->pict_type = AV_PICTURE_TYPE_NONE;
    if (m_vctx->flags & AV_CODEC_FLAG_INTERLACED_DCT) {
        m_yuv->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    }
    int ret = avcodec_send_frame(m_vctx, m_yuv);
    if (ret < 0 && ret != AVERROR(EAGAIN)) { setError("video send_frame: " + avErr(ret)); return; }
    drainPackets(m_vctx, 0);
}

bool EncoderStage::convertPicture(const FramePtr& video) {
    if (!video || video->width <= 0 || video->height <= 0) return false;
    if (video->width == m_width && video->height == m_height && video->format == kMixerPixelFormat)
        return sws_scale_frame(m_sws, m_yuv, video.get()) >= 0;
    if (!m_swsAlt || m_altW != video->width || m_altH != video->height || m_altFmt != video->format) {
        if (m_swsAlt) { sws_freeContext(m_swsAlt); m_swsAlt = nullptr; }
        m_swsAlt = sws_getContext(video->width, video->height, AVPixelFormat(video->format), m_outW, m_outH, m_vctx->pix_fmt,
                                  SWS_BICUBIC | (m_fastRnd ? 0 : SWS_ACCURATE_RND), nullptr, nullptr, nullptr);
        m_altW = video->width; m_altH = video->height; m_altFmt = video->format;
        if (m_swsAlt) {
            int* inv = nullptr; int* tbl = nullptr; int srcRange, dstRange, br, con, sat;
            if (sws_getColorspaceDetails(m_swsAlt, &inv, &srcRange, &tbl, &dstRange, &br, &con, &sat) >= 0)
                sws_setColorspaceDetails(m_swsAlt, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709), 0, br, con, sat);
        }
    }
    const bool ok = m_swsAlt && sws_scale_frame(m_swsAlt, m_yuv, video.get()) >= 0;
    if (ok) { QMutexLocker s(&m_statMutex); m_stats.videoRescaled++; }
    return ok;
}

// PE-09: one pooled buffer per picture, laid out as av_frame_get_buffer(frame, 32) lays out the legacy frame: linesizes
// aligned to 32 bytes (identical strides, so the codec sees the same frames), the height padded to 32 rows (codecs and
// SIMD read past the last row), each plane 64-byte aligned inside the buffer.
bool EncoderStage::setupYuvPool() {
    const AVPixelFormat fmt = m_vctx->pix_fmt;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM))) return false;
    int ls[4] = {0, 0, 0, 0};
    if (av_image_fill_linesizes(ls, fmt, int(alignUp(size_t(m_outW), 32))) < 0) return false;
    ptrdiff_t lsp[4];
    for (int i = 0; i < 4; ++i) { ls[i] = int(alignUp(size_t(ls[i]), 32)); lsp[i] = ls[i]; }
    size_t sizes[4] = {0, 0, 0, 0};
    if (av_image_fill_plane_sizes(sizes, fmt, int(alignUp(size_t(m_outH), 32)), lsp) < 0) return false;
    size_t off = 0;
    for (int i = 0; i < 4; ++i) {
        m_poolLinesize[i] = sizes[i] ? ls[i] : 0;
        m_poolOffset[i] = off;
        off = alignUp(off + sizes[i], 64);
    }
    if (off == 0) return false;
    m_poolAllocs.store(0);
    m_yuvPool = av_buffer_pool_init2(off + 64, this, &EncoderStage::poolAlloc, nullptr);
    return m_yuvPool != nullptr;
}

bool EncoderStage::takePooledPicture() {
    AVBufferRef* buf = av_buffer_pool_get(m_yuvPool);    // refcount 1: either new or returned by everyone who held it
    if (!buf) return false;
    av_frame_unref(m_yuv);
    m_yuv->buf[0] = buf;
    m_yuv->format = m_vctx->pix_fmt;
    m_yuv->width = m_outW;
    m_yuv->height = m_outH;
    for (int i = 0; i < 4; ++i) {
        m_yuv->linesize[i] = m_poolLinesize[i];
        m_yuv->data[i] = m_poolLinesize[i] ? buf->data + m_poolOffset[i] : nullptr;
    }
    m_yuv->extended_data = m_yuv->data;
    return true;
}

// Pool growth only (the encoder thread is the only caller of av_buffer_pool_get, so `opaque` is alive whenever it runs).
AVBufferRef* EncoderStage::poolAlloc(void* opaque, size_t size) {
    static_cast<EncoderStage*>(opaque)->m_poolAllocs.fetch_add(1, std::memory_order_relaxed);
    return av_buffer_alloc(size);
}

void EncoderStage::encodeAudio(const FramePtr& audio) {
    if (!m_actx || !m_afifo) return;
    if (audio) writeAudio(audio);
    // Keep the audio on the video clock. Audio pts counts samples, so a tick that brought less audio than its frame lasts
    // (a source switch, a live input reconnecting) would put audio behind video for good: pad the shortfall with silence
    // when it passes two frames, and trim an excess that passes four (normal per-tick jitter never reaches either).
    if (m_vctx && m_rate.num > 0) {
        const double perFrameD = double(m_actx->sample_rate) * m_rate.den / m_rate.num;
        const qint64 perFrame = qMax<qint64>(1, qint64(perFrameD));
        const qint64 expected = qint64(double(m_videoPts) * perFrameD);
        const qint64 have = m_audioPts + av_audio_fifo_size(m_afifo);
        if (expected - have > 2 * perFrame) {
            qint64 left = qMin<qint64>(expected - have, qint64(m_actx->sample_rate) * 10);
            const int chunk = 4096, ch = m_actx->ch_layout.nb_channels;
            uint8_t** buf = nullptr;
            if (av_samples_alloc_array_and_samples(&buf, nullptr, ch, chunk, m_actx->sample_fmt, 0) >= 0) {
                av_samples_set_silence(buf, 0, chunk, ch, m_actx->sample_fmt);
                QMutexLocker s(&m_statMutex); m_stats.audioPaddedSamples += left; s.unlock();
                while (left > 0) { const int n = int(qMin<qint64>(left, chunk)); av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(buf), n); left -= n; }
                av_freep(&buf[0]); av_freep(&buf);
            }
        } else if (have - expected > 4 * perFrame) {
            const int drop = int(qMin<qint64>(av_audio_fifo_size(m_afifo), have - expected - perFrame));
            if (drop > 0) { av_audio_fifo_drain(m_afifo, drop); QMutexLocker s(&m_statMutex); m_stats.audioDroppedSamples += drop; }
        }
    }
    while (av_audio_fifo_size(m_afifo) >= m_actx->frame_size) {
        if (av_frame_make_writable(m_aframe) < 0) return;
        av_audio_fifo_read(m_afifo, reinterpret_cast<void**>(m_aframe->data), m_actx->frame_size);
        m_aframe->nb_samples = m_actx->frame_size;
        m_aframe->pts = m_audioPts;
        m_audioPts += m_actx->frame_size;
        int ret = avcodec_send_frame(m_actx, m_aframe);
        if (ret < 0 && ret != AVERROR(EAGAIN)) { setError("audio send_frame: " + avErr(ret)); return; }
        drainPackets(m_actx, 1);
    }
}

void EncoderStage::writeAudio(const FramePtr& audio) {
    // Our audio arrives as FLTP at the engine's channel count. Convert if the
    // codec wants something else (e.g. mp2 wants S16).
    const int inCh = audio->ch_layout.nb_channels;
    const int outCh = m_actx->ch_layout.nb_channels;
    if (audio->format == m_actx->sample_fmt && inCh == outCh && audio->sample_rate == m_actx->sample_rate) {
        av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(audio->data), audio->nb_samples);
    } else {
        static thread_local SwrContext* swr = nullptr;
        if (!swr) {
            swr_alloc_set_opts2(&swr, &m_actx->ch_layout, m_actx->sample_fmt, m_actx->sample_rate,
                                &audio->ch_layout, AVSampleFormat(audio->format), audio->sample_rate, 0, nullptr);
            if (!swr || swr_init(swr) < 0) { setError("audio swr init failed"); return; }
        }
        AVFrame* tmp = av_frame_alloc();
        tmp->format = m_actx->sample_fmt; tmp->sample_rate = m_actx->sample_rate;
        av_channel_layout_copy(&tmp->ch_layout, &m_actx->ch_layout);
        tmp->nb_samples = audio->nb_samples + 64;
        av_frame_get_buffer(tmp, 0);
        const int got = swr_convert(swr, tmp->data, tmp->nb_samples,
                                    const_cast<const uint8_t**>(audio->extended_data), audio->nb_samples);
        if (got > 0) av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(tmp->data), got);
        av_frame_free(&tmp);
    }
}

void EncoderStage::drainPackets(AVCodecContext* ctx, int streamKind) {
    for (;;) {
        int ret = avcodec_receive_packet(ctx, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { setError("receive_packet: " + avErr(ret)); break; }
        {
            QMutexLocker s(&m_statMutex);
            if (streamKind == 0) { m_stats.videoPackets++; m_stats.videoBytes += m_pkt->size; }
            else { m_stats.audioPackets++; m_stats.audioBytes += m_pkt->size; }
        }
        QMutexLocker l(&m_sinkMutex);
        for (PacketSink* sink : m_sinks) sink->writePacket(m_pkt, streamKind);
        av_packet_unref(m_pkt);
    }
}

void EncoderStage::flush() {
    if (m_vctx) { avcodec_send_frame(m_vctx, nullptr); drainPackets(m_vctx, 0); }
    if (m_actx) { avcodec_send_frame(m_actx, nullptr); drainPackets(m_actx, 1); }
}
