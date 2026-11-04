#include "dektecsdisink.h"
#include "../../core/frameclock.h"
#include <QDebug>
#include <QDateTime>
#include <cmath>
#include <cstring>

#ifdef HAS_DEKTEC
#include <DTAPI.h>
#endif

// ---------------------------------------------------------------------------
// Hardware wrapper
// ---------------------------------------------------------------------------

struct DekTecSdiSink::Hw {
#ifdef HAS_DEKTEC
    DtDevice device;
    DtMxProcess matrix;
    bool deviceAttached = false;
    bool matrixStarted = false;
    int endToEndDelayFrames = 0;
    qint64 skippedFrames = 0;
#endif
};

#ifdef HAS_DEKTEC
namespace {
QString dtErr(DTAPI_RESULT r) { return QString::fromLatin1(DtapiResult2Str(r)); }
}
#endif

DekTecSdiSink::DekTecSdiSink(const QString& name, const QJsonObject& options, QObject* parent)
    : BasebandSink(name, parent), m_hw(new Hw), m_opts(options)
{
}

DekTecSdiSink::~DekTecSdiSink() {
    stop();
    delete m_hw;
}

bool DekTecSdiSink::isAvailable() {
#ifdef HAS_DEKTEC
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Video standards
// ---------------------------------------------------------------------------

int DekTecSdiSink::videoStandardFromName(const QString& n) {
#ifdef HAS_DEKTEC
    const QString s = n.toLower().remove(' ');
    if (s == "1080p29.97" || s == "1080p2997") return DTAPI_VIDSTD_1080P29_97;
    if (s == "1080p30") return DTAPI_VIDSTD_1080P30;
    if (s == "1080p25") return DTAPI_VIDSTD_1080P25;
    if (s == "1080p24") return DTAPI_VIDSTD_1080P24;
    if (s == "1080p23.98" || s == "1080p2398") return DTAPI_VIDSTD_1080P23_98;
    if (s == "1080i59.94" || s == "1080i5994") return DTAPI_VIDSTD_1080I59_94;
    if (s == "1080i60") return DTAPI_VIDSTD_1080I60;
    if (s == "1080i50") return DTAPI_VIDSTD_1080I50;
    if (s == "720p59.94" || s == "720p5994") return DTAPI_VIDSTD_720P59_94;
    if (s == "720p60") return DTAPI_VIDSTD_720P60;
    if (s == "720p50") return DTAPI_VIDSTD_720P50;
    if (s == "525i59.94" || s == "ntsc") return DTAPI_VIDSTD_525I59_94;
    if (s == "625i50" || s == "pal") return DTAPI_VIDSTD_625I50;
#else
    Q_UNUSED(n);
#endif
    return -1;
}

int DekTecSdiSink::videoStandardFor(int width, int height, const FrameRate& rate, bool interlaced, QString* name) {
    QString n;
    const double fps = rate.fps();
    auto near = [&](double v) { return std::fabs(fps - v) < 0.02; };
    if (height == 1080 && width == 1920) {
        if (near(29.97)) n = interlaced ? "1080i59.94" : "1080p29.97";
        else if (near(30.0)) n = interlaced ? "1080i60" : "1080p30";
        else if (near(25.0)) n = interlaced ? "1080i50" : "1080p25";
        else if (near(24.0)) n = "1080p24";
        else if (near(23.976)) n = "1080p23.98";
        else if (near(59.94)) n = "1080i59.94";   // 59.94 progressive needs 3G; use the interlaced carrier
        else if (near(50.0)) n = "1080i50";
        else if (near(60.0)) n = "1080i60";
    } else if (height == 720 && width == 1280) {
        if (near(59.94)) n = "720p59.94"; else if (near(60.0)) n = "720p60"; else if (near(50.0)) n = "720p50";
    } else if (height == 486 || height == 480) {
        n = "525i59.94";
    } else if (height == 576) {
        n = "625i50";
    }
    if (name) *name = n;
    return n.isEmpty() ? -1 : videoStandardFromName(n);
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool DekTecSdiSink::openHardware(QString* err) {
#ifdef HAS_DEKTEC
    const int type = m_opts.value("deviceType").toInt(2154);
    const int index = m_opts.value("deviceIndex").toInt(0);
    const int port = m_opts.value("port").toInt(2);
    DTAPI_RESULT r;
    if (m_opts.contains("serial")) r = m_hw->device.AttachToSerial(m_opts["serial"].toString().toLongLong());
    else r = m_hw->device.AttachToType(type, index);
    if (r != DTAPI_OK) { *err = QString("DtDevice attach (type %1 #%2): %3").arg(type).arg(index).arg(dtErr(r)); return false; }
    m_hw->deviceAttached = true;

    if (m_opts.value("configurePort").toBool(true)) {
        int v = 0, sub = 0;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IODIR, v, sub) == DTAPI_OK && v != DTAPI_IOCONFIG_OUTPUT) {
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_OUTPUT);
            qInfo() << "[DekTec SDI] port" << port << "set to output:" << dtErr(r);
        }
        const int wantStd = (m_fmt.height >= 720) ? DTAPI_IOCONFIG_HDSDI : DTAPI_IOCONFIG_SDI;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IOSTD, v, sub) == DTAPI_OK && (v != wantStd || sub != m_vidStd)) {
            // I/O standard (HD-SDI or SD-SDI) with the video standard as sub-value.
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IOSTD, wantStd, m_vidStd);
            qInfo() << "[DekTec SDI] port" << port << "set to" << (wantStd == DTAPI_IOCONFIG_HDSDI ? "HD-SDI" : "SD-SDI") << "std" << m_vidStd << ":" << dtErr(r);
        }
    }

    // Genlock: lock this output to the card's reference input when asked (genlock: true,
    // genlockRefPort: N marks that port as the reference). The engine's clock then follows the
    // card through the output-locked trim, so the whole chain runs on house reference.
    if (m_opts.value("genlock").toBool(false)) {
        const int refPort = m_opts.value("genlockRefPort").toInt(0);
        if (refPort > 0) { r = m_hw->device.SetIoConfig(refPort, DTAPI_IOCONFIG_GENREF, DTAPI_IOCONFIG_TRUE); qInfo() << "[DekTec SDI] port" << refPort << "as genlock reference:" << dtErr(r); }
        r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_GENLOCKED, DTAPI_IOCONFIG_TRUE);
        qInfo() << "[DekTec SDI] port" << port << "genlocked output:" << dtErr(r);
    }
    DtMxPort mxPort(&m_hw->device, port);
    r = m_hw->matrix.AttachRowToOutput(0, mxPort);
    if (r != DTAPI_OK) { *err = QString("AttachRowToOutput port %1: %2").arg(port).arg(dtErr(r)); return false; }

    DtMxRowConfig cfg;
    cfg.m_Enable = true;
    cfg.m_RowSize = 1;
    cfg.m_pOpaq = this;
    cfg.m_RawDataEnable = false;
    cfg.m_VideoEnable = true;
    cfg.m_Video.m_StartLine1 = 1; cfg.m_Video.m_NumLines1 = -1;
    cfg.m_Video.m_StartLine2 = 1; cfg.m_Video.m_NumLines2 = -1;
    cfg.m_Video.m_Scaling = DTAPI_SCALING_OFF;
    cfg.m_Video.m_LineAlignment = 16;
    cfg.m_Video.m_BufAlignment = -1;
    cfg.m_Video.m_PixelFormat = DT_PXFMT_UYVY422_8B;
    cfg.m_Video.m_UserBuffer = false;
    cfg.m_Video.m_WeaveInterlaced = true;     // interlaced standards: one weaved buffer, we write every line
    cfg.m_Video.m_UseZeroCopyMode = false;
    cfg.m_AudioEnable = true;
    cfg.m_AudioDef.m_DeEmbed = false;
    cfg.m_AudioDef.m_OutputMode = DT_OUTPUT_MODE_ADD;
    cfg.m_AudioDef.m_Format = DT_AUDIO_SAMPLE_PCM;
    for (int c = 0; c < m_audioChannels; ++c) {
        DtMxAudioConfig ac = cfg.m_AudioDef;
        ac.m_Index = c;
        cfg.m_Audio.push_back(ac);
    }
    cfg.m_AuxDataEnable = false;
    if ((r = m_hw->matrix.SetRowConfig(0, cfg)) != DTAPI_OK) { *err = "SetRowConfig: " + dtErr(r); return false; }
    if ((r = m_hw->matrix.SetVidStd(0, DtVideoStandard(m_vidStd))) != DTAPI_OK) { *err = QString("SetVidStd %1: %2").arg(m_vidStd).arg(dtErr(r)); return false; }
    if ((r = m_hw->matrix.AddMatrixCbFunc(&DekTecSdiSink::matrixCallback, this)) != DTAPI_OK) { *err = "AddMatrixCbFunc: " + dtErr(r); return false; }
    double cbFrames = 0.0;
    m_hw->matrix.GetDefEndToEndDelay(m_hw->endToEndDelayFrames, cbFrames);
    if ((r = m_hw->matrix.Start()) != DTAPI_OK) { *err = "DtMxProcess::Start: " + dtErr(r); return false; }
    m_hw->matrixStarted = true;
    qInfo().noquote() << QString("[DekTec SDI] port %1 running, video std %2, %3 audio ch, end-to-end delay %4 frames")
        .arg(port).arg(m_vidStd).arg(m_audioChannels).arg(m_hw->endToEndDelayFrames);
    return true;
#else
    *err = "DekTec support not compiled in";
    return false;
#endif
}

void DekTecSdiSink::closeHardware() {
#ifdef HAS_DEKTEC
    if (m_hw->matrixStarted) { m_hw->matrix.Stop(); m_hw->matrixStarted = false; }
    m_hw->matrix.Reset();
    if (m_hw->deviceAttached) { m_hw->device.Detach(); m_hw->deviceAttached = false; }
#endif
}

bool DekTecSdiSink::start(const Format& fmt) {
    if (m_running.load()) return true;
    m_fmt = fmt;
    m_audioChannels = qBound(1, m_opts.value("audioChannels").toInt(qMin(fmt.channels, 16)), 16);
    const QString stdName = m_opts.value("videoStd").toString("auto");
    QString chosen;
    if (stdName == "auto" || stdName.isEmpty()) {
        m_vidStd = videoStandardFor(fmt.width, fmt.height, fmt.rate, m_opts.value("interlaced").toBool(false), &chosen);
    } else {
        m_vidStd = videoStandardFromName(stdName); chosen = stdName;
    }
    if (m_vidStd < 0) { setError(QString("no SDI standard for %1x%2 @%3").arg(fmt.width).arg(fmt.height).arg(fmt.rate.toString())); return false; }
    m_interlaced = chosen.contains('i');

    m_sws = sws_getContext(fmt.width, fmt.height, kMixerPixelFormat, fmt.width, fmt.height, AV_PIX_FMT_UYVY422,
                           SWS_BILINEAR | SWS_ACCURATE_RND, nullptr, nullptr, nullptr);
    if (!m_sws) { setError("sws context failed"); return false; }
    {
        int* inv = nullptr; int* tbl = nullptr; int srcRange, dstRange, br, con, sat;
        if (sws_getColorspaceDetails(m_sws, &inv, &srcRange, &tbl, &dstRange, &br, &con, &sat) >= 0)
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709), 0, br, con, sat);
    }
    m_uyvy.assign(size_t(fmt.width) * 2 * fmt.height, 0);
    // Black in UYVY: U=V=128, Y=16
    for (size_t i = 0; i + 1 < m_uyvy.size(); i += 2) { m_uyvy[i] = 128; m_uyvy[i + 1] = 16; }
    m_afifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, m_audioChannels, 48000);
    m_audioScratch.assign(size_t(m_audioChannels) * 4096, 0.0f);
    m_audioPlanes.resize(m_audioChannels);
    for (int c = 0; c < m_audioChannels; ++c) m_audioPlanes[c] = m_audioScratch.data() + size_t(c) * 4096;

    QString err;
    if (!openHardware(&err)) { setError(err); closeHardware(); return false; }
    m_running = true;
    { QMutexLocker l(&m_statMutex); m_stats.connected = true; m_stats.extra["videoStd"] = chosen; }
    qInfo().noquote() << QString("[DekTec SDI] %1: %2 %3x%4 @%5").arg(m_name, chosen).arg(fmt.width).arg(fmt.height).arg(fmt.rate.toString());
    return true;
}

void DekTecSdiSink::stop() {
    if (!m_running.load()) return;
    m_running = false;
    closeHardware();
    { QMutexLocker l(&m_qmutex); m_video.clear(); m_lastVideo.reset(); m_primed = false; if (m_afifo) { av_audio_fifo_free(m_afifo); m_afifo = nullptr; } }
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    QMutexLocker l(&m_statMutex); m_stats.connected = false;
}

// ---------------------------------------------------------------------------
// Engine side
// ---------------------------------------------------------------------------

void DekTecSdiSink::pushFrame(const MediaFrame& frame) {
    if (!m_running.load()) return;
    QMutexLocker l(&m_qmutex);
    m_lastPushNs = FrameClock::nowNs();
    if (frame.video) {
        if (int(m_video.size()) >= kMaxQueue) { m_video.pop_front(); m_overruns++; }
        m_video.push_back(frame.video);
    }
    if (frame.audio && m_afifo) {
        // Keep the audio FIFO bounded to a few frames so it cannot drift away from video.
        const int maxSamples = 48000 / 4;
        if (av_audio_fifo_size(m_afifo) + frame.audio->nb_samples > maxSamples) av_audio_fifo_drain(m_afifo, frame.audio->nb_samples);
        av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(frame.audio->data), frame.audio->nb_samples);
    }
    QMutexLocker s(&m_statMutex); m_stats.framesIn++;
}

void DekTecSdiSink::convertVideo(const FramePtr& bgra) {
    if (!bgra || !m_sws) return;
    const uint8_t* src[4] = { bgra->data[0], nullptr, nullptr, nullptr };
    const int srcStride[4] = { bgra->linesize[0], 0, 0, 0 };
    uint8_t* dst[4] = { m_uyvy.data(), nullptr, nullptr, nullptr };
    const int dstStride[4] = { m_fmt.width * 2, 0, 0, 0 };
    sws_scale(m_sws, src, srcStride, 0, m_fmt.height, dst, dstStride);
}

// ---------------------------------------------------------------------------
// Matrix callback (card thread)
// ---------------------------------------------------------------------------

void DekTecSdiSink::matrixCallback(Dtapi::DtMxData* data, void* opaque) {
    static_cast<DekTecSdiSink*>(opaque)->onMatrixFrame(data);
}

void DekTecSdiSink::onMatrixFrame(Dtapi::DtMxData* d) {
#ifdef HAS_DEKTEC
    if (!m_running.load() || !d || d->m_Rows.size() == 0) return;
    if (d->m_NumSkippedFrames > 0) m_hw->skippedFrames += d->m_NumSkippedFrames;
    DtMxFrame* f = d->m_Rows[0].m_CurFrame;
    if (!f || f->m_Status == DT_FRMSTATUS_DISABLED) return;

    // --- pick the picture -----------------------------------------------------
    FramePtr pic;
    bool fresh = false;
    bool priming = false;
    {
        QMutexLocker l(&m_qmutex);
        if (!m_primed) {
            // The card's callback starts before the engine's first frames arrive; keep
            // two frames in hand so routine scheduling jitter never repeats a frame.
            if (int(m_video.size()) >= kPrimeFrames) m_primed = true; else priming = true;
        }
        if (!priming && !m_video.empty()) { pic = std::move(m_video.front()); m_video.pop_front(); fresh = true; m_lastVideo = pic; }
        else { pic = m_lastVideo; }
    }
    if (!fresh && !priming) m_underruns++;
    { QMutexLocker l(&m_qmutex); if (m_lastPushNs) { m_phaseSumNs += double(FrameClock::nowNs() - m_lastPushNs); ++m_phaseCount; } m_queueSum += double(m_video.size()); }
    if (fresh) convertVideo(pic);

    if (f->m_VideoValid) {
        DtMxVideoBuf& vb = (m_interlaced && f->m_VideoWeavedValid) ? f->m_VideoWeaved : f->m_Video[0];
        const DtMxVideoPlaneBuf& plane = vb.m_Planes[0];
        const int lineBytes = qMin(m_fmt.width * 2, plane.m_Stride);
        const int lines = qMin(m_fmt.height, plane.m_NumLines);
        if (plane.m_pBuf) {
            for (int y = 0; y < lines; ++y) memcpy(plane.m_pBuf + size_t(y) * plane.m_Stride, m_uyvy.data() + size_t(y) * m_fmt.width * 2, lineBytes);
        } else {
            m_callbackErrors++;
        }
    }

    // --- audio ---------------------------------------------------------------------
    if (f->m_AudioValid) {
        const int nch = int(f->m_Audio.m_Channels.size());
        for (int c = 0; c < nch && c < m_audioChannels; ++c) {
            DtMxAudioChannel& ch = f->m_Audio.m_Channels[size_t(c)];
            const int n = qMin(ch.m_NumSamplesHint > 0 ? ch.m_NumSamplesHint : 1602, ch.m_BufSizeSamples);
            if (c == 0) {
                QMutexLocker l(&m_qmutex);
                const int avail = (m_afifo && m_primed) ? av_audio_fifo_size(m_afifo) : 0;
                const int take = qMin(avail, n);
                if (take > 0) av_audio_fifo_read(m_afifo, reinterpret_cast<void**>(m_audioPlanes.data()), take);
                for (int k = 0; k < m_audioChannels; ++k) for (int i = take; i < n; ++i) m_audioPlanes[k][i] = 0.0f;
            }
            // 24-bit PCM left-justified in 32 bits.
            for (int i = 0; i < n; ++i) {
                const float v = qBound(-1.0f, m_audioPlanes[c][i], 1.0f);
                ch.m_pBuf[i] = static_cast<unsigned int>(static_cast<int>(std::lrint(v * 8388607.0f)) << 8);
            }
            ch.m_NumValidSamples = n;
            ch.m_Present = true;
            ch.m_Status.SetSampleRate(48000);
            ch.m_Status.SetPcmAudio(true);
            ch.m_Status.SetPcmNumBits(24);
        }
        f->m_Audio.InitChannelStatus();
    }

    m_framesOut++;
    if (!m_transmitting.load()) { m_transmitting = true; qInfo() << "[DekTec SDI]" << m_name << "first frame on air"; }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastStatMs >= 500) { m_lastStatMs = now; publishStats(); }
#else
    Q_UNUSED(d);
#endif
}

void DekTecSdiSink::publishStats() {
    QMutexLocker s(&m_statMutex);
    m_stats.framesOut = m_framesOut;
    m_stats.dropped = m_overruns;
    m_stats.extra["underruns"] = double(m_underruns);
    m_stats.extra["overruns"] = double(m_overruns);
    m_stats.extra["callbackErrors"] = double(m_callbackErrors);
    m_stats.extra["transmitting"] = m_transmitting.load();
#ifdef HAS_DEKTEC
    m_stats.extra["skippedFrames"] = double(m_hw->skippedFrames);
    m_stats.extra["endToEndDelayFrames"] = m_hw->endToEndDelayFrames;
#endif
    { QMutexLocker l(&m_qmutex); m_stats.extra["queuedFrames"] = int(m_video.size()); m_stats.extra["audioFifo"] = m_afifo ? av_audio_fifo_size(m_afifo) : 0; }
    m_stats.extra["targetQueue"] = kPrimeFrames;
    { QMutexLocker l(&m_qmutex); if (m_phaseCount > 0) { m_phaseFrac = (m_phaseSumNs / m_phaseCount) / double(m_fmt.rate.frameDurationNs()); m_queueAfterCb = m_queueSum / m_phaseCount; m_phaseSumNs = 0.0; m_queueSum = 0.0; m_phaseCount = 0; } }
    m_stats.extra["queueAfterCb"] = m_queueAfterCb;
    m_stats.extra["phaseFrac"] = m_phaseFrac;    // callback delay after the engine's push, in frames (continuous lock error)
    m_stats.extra["framesOut"] = double(m_framesOut);
#ifdef HAS_DEKTEC
    if (m_hw && m_hw->deviceAttached && m_opts.value("genlock").toBool(false)) {
        int state = 0, refStd = 0;
        if (m_hw->device.GetGenlockState(state, refStd) == DTAPI_OK) { m_stats.extra["genlock"] = state == DTAPI_GENLOCK_INLOCK ? "locked" : "no_lock"; m_stats.extra["genlockRefStd"] = refStd; }
    }
#endif
}
