#include "decoderpipeline.h"
#include <QElapsedTimer>
#include <QDebug>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <map>
#include <limits>
#include <chrono>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libavdevice/avdevice.h>
}

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <ctime>
#endif

namespace {
QString avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

/// An env switch set to 0 / off / false / no. Unset (or anything else) leaves the default alone.
bool envOff(const char* name) {
    if (!qEnvironmentVariableIsSet(name)) return false;
    const QByteArray v = qgetenv(name).trimmed().toLower();
    return v == "0" || v == "off" || v == "false" || v == "no";
}

// PE-12 hw/sw auto: process-wide measurements per "codec WxH pixfmt". Written by decode threads at the end of their
// measurement window, read when the next item of that kind opens.
struct AutoEntry {
    double hwMs = -1.0, swMs = -1.0;   // CPU ms per picture (-1: not measured yet)
    bool hwKeepsUp = true, swKeepsUp = true;
    bool hwUnavailable = false;        // no hw decoder for this codec: nothing to compare
    int opens = 0;
    QString choice;                    // what the last open used ("hw" / "sw")
};
QMutex g_autoMutex;
std::map<QString, AutoEntry> g_autoTable;

/// CPU ms (kernel + user) of every thread of this process, by thread id. Used twice per measured item, never per picture.
void threadCpuMs(std::unordered_map<unsigned long, double>& out) {
    out.clear();
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ULARGE_INTEGER ku, uu;
                ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime;
                uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
                out[te.th32ThreadID] = double(ku.QuadPart + uu.QuadPart) / 1e4;   // 100 ns units -> ms
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
#else
    // Portable fallback: the calling (decode) thread only.
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) out[0] = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}

qint64 nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
} // namespace

// ---------------------------------------------------------------------------
// Construction / lifecycle
// ---------------------------------------------------------------------------

DecoderPipeline::DecoderPipeline(const Config& cfg, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_pool(cfg.outWidth, cfg.outHeight, kMixerPixelFormat)
{
    // PE-12 switches (read per instance so tests can flip them; the cost is three getenv per item).
    const QByteArray hw = qgetenv("PLAYPLAY_DECODE_HW").trimmed().toLower();
    if (hw == "auto") { m_cfg.hwAccel = true; m_cfg.hwAuto = true; }
    else if (hw == "on" || hw == "1") { m_cfg.hwAccel = true; m_cfg.hwAuto = false; }
    else if (hw == "off" || hw == "0") { m_cfg.hwAccel = false; m_cfg.hwAuto = false; }
    if (envOff("PLAYPLAY_DECODE_POOL")) m_cfg.pooledDownload = false;
    if (envOff("PLAYPLAY_DECODE_SKIP")) m_cfg.skipUnshown = false;
    m_pooledDownload = m_cfg.pooledDownload;
}

DecoderPipeline::~DecoderPipeline() {
    close();
}

void DecoderPipeline::setError(const QString& msg) {
    { QMutexLocker l(&m_errMutex); m_lastError = msg; }
    qWarning() << "[Decoder]" << m_path << msg;
    emit error(msg);
}

DecoderPipeline::Stats DecoderPipeline::stats() const {
    QMutexLocker l(&m_statMutex);
    Stats s = m_stats;
    { QMutexLocker lv(&m_vmutex); s.queueDepth = int(m_vqueue.size()); }
    return s;
}

QJsonObject DecoderPipeline::hwAutoTable() {
    QMutexLocker l(&g_autoMutex);
    QJsonObject o;
    for (const auto& kv : g_autoTable) {
        const AutoEntry& e = kv.second;
        QJsonObject r;
        r["hwMs"] = e.hwMs; r["swMs"] = e.swMs; r["hwKeepsUp"] = e.hwKeepsUp; r["swKeepsUp"] = e.swKeepsUp;
        r["hwUnavailable"] = e.hwUnavailable; r["opens"] = e.opens; r["choice"] = e.choice;
        o[kv.first] = r;
    }
    return o;
}

void DecoderPipeline::resetHwAutoTable() {
    QMutexLocker l(&g_autoMutex);
    g_autoTable.clear();
}

bool DecoderPipeline::wouldShow(const FrameRate& outRate, double posSec, double nextPosSec) {
    // videoFrameAt(t) shows the newest queued picture with pos <= t + tol, t on the output grid k*d. The picture at
    // posSec is therefore shown iff some tick t satisfies posSec - tol <= t < nextPosSec - tol; the first candidate is the
    // smallest grid point >= posSec - tol (never before the item's own tick 0).
    const double d = outRate.frameDurationNs() / 1e9;
    if (d <= 0.0) return true;
    const double tol = d / 4.0;
    const double k = std::ceil((posSec - tol) / d - 1e-9);
    const double t = qMax(0.0, k) * d;
    return t < nextPosSec - tol;
}

bool DecoderPipeline::open(const QString& path, double inPointSec, double outPointSec) {
    close();
    m_path = path;
    m_inPointSec = qMax(0.0, inPointSec);
    m_outPointSec = outPointSec;
    m_stop = false;
    m_eof = false;
    m_firstFrameReady = false;
    m_audioAligned = false;
    m_audioSamplesPushed = 0;
    m_audioOutLimit = -1;
    m_loopOffsetSec = 0.0;
    m_lastShown.reset();
    m_lastShownPos = -1.0;
    m_hasHeld = false;
    m_skipActive = false;
    m_autoMeasuring = false;
    m_autoWallStartNs = 0;
    m_autoWaitNs = 0;
    { QMutexLocker l(&m_statMutex); m_stats = Stats(); m_stats.downloadPooled = m_pooledDownload; }

    if (!openInput(path)) { freeFfmpeg(); return false; }
    if (!openVideoDecoder()) { freeFfmpeg(); return false; }
    openAudioDecoder(); // optional: silence if the file has no audio

    // Timeline bookkeeping
    m_streamStartSec = (m_fmt->start_time != AV_NOPTS_VALUE)
        ? m_fmt->start_time / double(AV_TIME_BASE) : 0.0;
    const double streamDur = (m_fmt->duration > 0) ? m_fmt->duration / double(AV_TIME_BASE) : 0.0;
    if (m_outPointSec > 0.0 && m_outPointSec > m_inPointSec) {
        m_durationSec = m_outPointSec - m_inPointSec;
    } else {
        m_outPointSec = 0.0;
        m_durationSec = qMax(0.0, streamDur - m_inPointSec);
    }
    if (m_cfg.realtime) { m_durationSec = 0.0; m_outPointSec = 0.0; }
    if (m_outPointSec > 0.0) {
        m_audioOutLimit = qint64(std::llround(m_durationSec * m_cfg.sampleRate));
    }

    // PE-12: a source at >= 1.5x the output rate converts only the pictures the output will pick. Below that every
    // picture is shown at least once (or the drop pattern is too sparse to pay for the one-picture hold).
    m_skipActive = m_cfg.skipUnshown && !m_cfg.realtime && m_srcRate.isValid() && m_cfg.outRate.isValid()
                   && m_srcRate.fps() >= 1.5 * m_cfg.outRate.fps();
    if (m_skipActive && !m_held) m_held = av_frame_alloc();
    if (!m_held) m_skipActive = false;
    { QMutexLocker l(&m_statMutex); m_stats.skipActive = m_skipActive; }

    m_afifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, m_cfg.channels, m_cfg.sampleRate);
    if (!m_afifo) { setError("av_audio_fifo_alloc failed"); freeFfmpeg(); return false; }

    if (m_inPointSec > 0.0 && !seekTo(m_streamStartSec + m_inPointSec)) {
        qWarning() << "[Decoder] seek to in-point failed; decoding from start";
    }

    m_worker = new Worker(this);
    m_worker->start(QThread::HighPriority);

    // Wait for the first picture so the engine can cut cleanly.
    {
        QMutexLocker l(&m_vmutex);
        QDeadlineTimer dl(m_cfg.openTimeoutMs);
        while (!m_firstFrameReady.load() && !m_eof.load() && !dl.hasExpired()) {
            m_vfirst.wait(&m_vmutex, dl);
        }
    }
    if (!m_firstFrameReady.load()) {
        setError(m_eof.load() ? "no decodable video frames" : "timed out waiting for first frame");
        close();
        return false;
    }

    m_opened = true;
    Stats st; { QMutexLocker l(&m_statMutex); st = m_stats; }
    qInfo().noquote() << QString("[Decoder] opened %1 %2x%3 @%4 in=%5 out=%6 dur=%7s audio=%8 hw=%9%10%11")
        .arg(path).arg(m_srcWidth).arg(m_srcHeight).arg(m_srcRate.toString())
        .arg(m_inPointSec).arg(m_outPointSec).arg(m_durationSec)
        .arg(hasAudio() ? "yes" : "no")
        .arg(m_hwPixFmt != AV_PIX_FMT_NONE ? av_hwdevice_get_type_name(
            m_hwDevice ? reinterpret_cast<AVHWDeviceContext*>(m_hwDevice->data)->type : AV_HWDEVICE_TYPE_NONE) : "sw")
        .arg(st.autoReason.isEmpty() ? QString() : QString(" auto=%1").arg(st.autoReason))
        .arg(m_skipActive ? " skipUnshown" : "");
    return true;
}

void DecoderPipeline::close() {
    m_stop = true;
    m_opened = false;
    // Wake the decode thread out of any back-pressure wait.
    { QMutexLocker l(&m_vmutex); m_vspace.wakeAll(); m_vfirst.wakeAll(); }
    { QMutexLocker l(&m_amutex); m_aspace.wakeAll(); }
    if (m_worker) {
        if (!m_worker->wait(3000)) {
            qWarning() << "[Decoder] worker did not stop in time; terminating";
            m_worker->terminate();
            m_worker->wait(1000);
        }
        delete m_worker;
        m_worker = nullptr;
    }
    { QMutexLocker l(&m_vmutex); m_vqueue.clear(); }
    freeFfmpeg();
}

void DecoderPipeline::freeFfmpeg() {
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_cvtSrcFmt = -1;
    if (m_held) { av_frame_free(&m_held); }
    m_hasHeld = false;
    if (m_swr) { swr_free(&m_swr); }
    if (m_vctx) { avcodec_free_context(&m_vctx); }
    if (m_actx) { avcodec_free_context(&m_actx); }
    if (m_fmt) { avformat_close_input(&m_fmt); }
    if (m_hwDevice) { av_buffer_unref(&m_hwDevice); }
    // Pooled download buffers still referenced (none after the worker stopped) are freed by their last owner.
    if (m_dlPool) { av_buffer_pool_uninit(&m_dlPool); }
    m_dlFmt = AV_PIX_FMT_NONE; m_dlW = m_dlH = 0;
    if (m_afifo) { av_audio_fifo_free(m_afifo); m_afifo = nullptr; }
    m_hwPixFmt = AV_PIX_FMT_NONE;
    m_videoStreamIndex = m_audioStreamIndex = -1;
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

bool DecoderPipeline::openInput(const QString& path) {
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "probesize", "10000000", 0);
    if (m_cfg.realtime) {
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "flags", "low_delay", 0);
        av_dict_set(&opts, "analyzeduration", "1000000", 0);
    }
    for (auto it = m_cfg.inputOptions.begin(); it != m_cfg.inputOptions.end(); ++it)
        av_dict_set(&opts, it.key().toUtf8().constData(), it.value().toVariant().toString().toUtf8().constData(), 0);
    const AVInputFormat* ifmt = nullptr;
    if (!m_cfg.inputFormat.isEmpty()) {
        static bool devicesRegistered = false;
        if (!devicesRegistered) { avdevice_register_all(); devicesRegistered = true; }
        ifmt = av_find_input_format(m_cfg.inputFormat.toUtf8().constData());
        if (!ifmt) { setError("input format not available: " + m_cfg.inputFormat); av_dict_free(&opts); return false; }
    }
    int ret = avformat_open_input(&m_fmt, path.toUtf8().constData(), ifmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) { setError("open failed: " + avErr(ret)); return false; }
    ret = avformat_find_stream_info(m_fmt, nullptr);
    if (ret < 0) { setError("stream info failed: " + avErr(ret)); return false; }

    m_videoStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_audioStreamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) { setError("no video stream"); return false; }
    if (m_audioStreamIndex < 0) m_audioStreamIndex = -1;
    return true;
}

AVPixelFormat DecoderPipeline::getHwFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    auto* self = static_cast<DecoderPipeline*>(ctx->opaque);
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == self->m_hwPixFmt) return *p;
    }
    // Hardware format not offered (e.g. unsupported profile): fall back to software.
    qWarning() << "[Decoder] hw pixel format not available for this stream; software decode";
    self->m_hwPixFmt = AV_PIX_FMT_NONE;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return fmts[0];
}

bool DecoderPipeline::openVideoDecoder() {
    AVStream* st = m_fmt->streams[m_videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { setError("no decoder for video codec"); return false; }

    // PE-12 auto mode: which of hw / sw this item uses, from what earlier items of the same kind measured.
    m_hwAuto = m_cfg.hwAccel && m_cfg.hwAuto;
    bool wantHw = m_cfg.hwAccel;
    QString reason;
    if (m_hwAuto) {
        const char* pf = av_get_pix_fmt_name(AVPixelFormat(st->codecpar->format));
        m_autoKey = QString("%1 %2x%3 %4").arg(codec->name).arg(st->codecpar->width).arg(st->codecpar->height).arg(pf ? pf : "?");
        QMutexLocker l(&g_autoMutex);
        AutoEntry& e = g_autoTable[m_autoKey];
        e.opens++;
        if (e.hwUnavailable) { wantHw = false; reason = "only"; }
        else if (e.hwMs < 0.0) { wantHw = true; reason = "probe"; }
        else if (e.swMs < 0.0) { wantHw = false; reason = "probe"; }
        else {
            const double hwCost = e.hwKeepsUp ? e.hwMs : std::numeric_limits<double>::infinity();
            const double swCost = e.swKeepsUp ? e.swMs : std::numeric_limits<double>::infinity();
            wantHw = hwCost <= swCost;
            reason = "cheaper";
        }
        m_autoMeasuring = (reason == "probe");
        if (m_autoMeasuring) threadCpuMs(m_autoBefore);   // threads alive before this decoder: excluded from its cost
    }

    m_vctx = avcodec_alloc_context3(codec);
    if (!m_vctx || avcodec_parameters_to_context(m_vctx, st->codecpar) < 0) {
        setError("video codec params failed"); return false;
    }
    m_vctx->pkt_timebase = st->time_base;
    // Frame threading adds one frame of latency per thread; on a 32-thread box
    // "auto" means ~1 s before the first picture. Four threads decode 1080p
    // H.264 well above real time and keep start-up latency at 4 frames.
    m_vctx->thread_count = qBound(1, QThread::idealThreadCount() / 4, 4);
    m_vctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    m_vctx->opaque = this;

    // Hardware acceleration: try d3d11va, then dxva2, then cuda.
    if (wantHw) {
        const AVHWDeviceType prefs[] = { AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA };
        for (AVHWDeviceType type : prefs) {
            AVPixelFormat hwFmt = AV_PIX_FMT_NONE;
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* hc = avcodec_get_hw_config(codec, i);
                if (!hc) break;
                if ((hc->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && hc->device_type == type) {
                    hwFmt = hc->pix_fmt; break;
                }
            }
            if (hwFmt == AV_PIX_FMT_NONE) continue;
            if (av_hwdevice_ctx_create(&m_hwDevice, type, nullptr, nullptr, 0) < 0) continue;
            m_vctx->hw_device_ctx = av_buffer_ref(m_hwDevice);
            m_vctx->get_format = &DecoderPipeline::getHwFormat;
            m_hwPixFmt = hwFmt;
            QMutexLocker l(&m_statMutex);
            m_stats.hwAccelActive = true;
            m_stats.hwDevice = av_hwdevice_get_type_name(type);
            break;
        }
        if (m_hwPixFmt == AV_PIX_FMT_NONE && m_hwAuto) {
            // No hw decoder for this codec on this box: nothing to measure, sw is the only mode.
            QMutexLocker l(&g_autoMutex);
            g_autoTable[m_autoKey].hwUnavailable = true;
            reason = "only"; m_autoMeasuring = false;
        }
    }
    {
        QMutexLocker l(&m_statMutex);
        m_stats.hwChoice = (m_hwPixFmt != AV_PIX_FMT_NONE) ? "hw" : "sw";
        m_stats.autoReason = reason;
    }
    if (m_hwAuto) { QMutexLocker l(&g_autoMutex); g_autoTable[m_autoKey].choice = (m_hwPixFmt != AV_PIX_FMT_NONE) ? "hw" : "sw"; }

    int ret = avcodec_open2(m_vctx, codec, nullptr);
    if (ret < 0) { setError("video codec open failed: " + avErr(ret)); return false; }

    m_srcWidth = m_vctx->width;
    m_srcHeight = m_vctx->height;
    AVRational fr = st->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) fr = av_inv_q(st->time_base);
    m_srcRate = FrameRate(fr.num, fr.den);

    // Letterbox / pillarbox placement honouring the sample aspect ratio.
    AVRational sar = st->codecpar->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
    const double displayW = double(m_srcWidth) * sar.num / sar.den;
    const double displayH = double(m_srcHeight);
    const double scale = qMin(m_cfg.outWidth / displayW, m_cfg.outHeight / displayH);
    m_dstW = int(displayW * scale + 0.5) & ~1;
    m_dstH = int(displayH * scale + 0.5) & ~1;
    m_dstW = qBound(2, m_dstW, m_cfg.outWidth);
    m_dstH = qBound(2, m_dstH, m_cfg.outHeight);
    m_dstX = ((m_cfg.outWidth - m_dstW) / 2) & ~1;
    m_dstY = ((m_cfg.outHeight - m_dstH) / 2) & ~1;
    return true;
}

bool DecoderPipeline::openAudioDecoder() {
    if (m_audioStreamIndex < 0) return false;
    AVStream* st = m_fmt->streams[m_audioStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) { m_audioStreamIndex = -1; return false; }
    m_actx = avcodec_alloc_context3(codec);
    if (!m_actx || avcodec_parameters_to_context(m_actx, st->codecpar) < 0) {
        avcodec_free_context(&m_actx); m_audioStreamIndex = -1; return false;
    }
    m_actx->pkt_timebase = st->time_base;
    if (avcodec_open2(m_actx, codec, nullptr) < 0) {
        avcodec_free_context(&m_actx); m_audioStreamIndex = -1; return false;
    }
    if (m_actx->ch_layout.nb_channels == 0) {
        av_channel_layout_default(&m_actx->ch_layout, 2);
    }

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, m_cfg.channels);
    int ret = swr_alloc_set_opts2(&m_swr,
        &outLayout, AV_SAMPLE_FMT_FLTP, m_cfg.sampleRate,
        &m_actx->ch_layout, m_actx->sample_fmt, m_actx->sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&outLayout);
    if (ret < 0 || swr_init(m_swr) < 0) {
        qWarning() << "[Decoder] swr init failed; audio disabled";
        swr_free(&m_swr);
        avcodec_free_context(&m_actx);
        m_audioStreamIndex = -1;
        return false;
    }
    return true;
}

bool DecoderPipeline::seekTo(double absSec) {
    const int64_t ts = int64_t(absSec * AV_TIME_BASE);
    int ret = avformat_seek_file(m_fmt, -1, INT64_MIN, ts, ts, 0);
    if (ret < 0) ret = av_seek_frame(m_fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return false;
    if (m_vctx) avcodec_flush_buffers(m_vctx);
    if (m_actx) avcodec_flush_buffers(m_actx);
    return true;
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

void DecoderPipeline::decodeLoop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool stopReading = false;

    auto drain = [&](AVCodecContext* ctx, bool isVideo) {
        if (!ctx) return;
        avcodec_send_packet(ctx, nullptr);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (isVideo) handleVideoFrame(frame); else handleAudioFrame(frame);
            av_frame_unref(frame);
        }
    };

    while (!m_stop.load() && !stopReading) {
        int ret = av_read_frame(m_fmt, pkt);
        if (ret == AVERROR_EOF || (ret < 0 && ret != AVERROR(EAGAIN))) {
            drain(m_vctx, true);
            drain(m_actx, false);
            flushHeld();   // the last picture of the pass is always shown
            if (m_cfg.loop && !m_stop.load() && m_durationSec > 0.0) {
                // Seamless loop: restart at in-point; positions continue past
                // the previous pass so the engine's timeline never jumps back.
                m_loopOffsetSec += m_durationSec;
                if (m_audioOutLimit >= 0) {
                    m_audioOutLimit = qint64(std::llround((m_loopOffsetSec + m_durationSec) * m_cfg.sampleRate));
                }
                if (!seekTo(m_streamStartSec + m_inPointSec)) break;
                continue;
            }
            break;
        }
        if (ret < 0) { av_usleep(1000); continue; }

        if (pkt->stream_index == m_videoStreamIndex && m_vctx) {
            ret = avcodec_send_packet(m_vctx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                qWarning() << "[Decoder] video send_packet:" << avErr(ret);
            }
            while ((ret = avcodec_receive_frame(m_vctx, frame)) == 0) {
                if (!handleVideoFrame(frame)) stopReading = true;
                av_frame_unref(frame);
                if (stopReading) break;
            }
        } else if (pkt->stream_index == m_audioStreamIndex && m_actx) {
            ret = avcodec_send_packet(m_actx, pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                qWarning() << "[Decoder] audio send_packet:" << avErr(ret);
            }
            while ((ret = avcodec_receive_frame(m_actx, frame)) == 0) {
                handleAudioFrame(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    if (stopReading) flushHeld();   // out-point: the held picture precedes it and is still due
    if (m_autoMeasuring) autoWindowClose();   // short item: record what was measured (if enough pictures)
    if (m_hasHeld) { av_frame_unref(m_held); m_hasHeld = false; }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    m_eof = true;
    { QMutexLocker l(&m_vmutex); m_vfirst.wakeAll(); }
    emit endOfStream();
}

bool DecoderPipeline::handleVideoFrame(AVFrame* src) {
    AVStream* st = m_fmt->streams[m_videoStreamIndex];
    int64_t pts = src->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = src->pts;
    double posSec;
    if (pts == AV_NOPTS_VALUE) {
        // No timestamps at all: synthesize from frame count at source rate.
        qint64 n; { QMutexLocker l(&m_statMutex); n = m_stats.decodedVideo; }
        posSec = m_srcRate.frameToNs(n) / 1e9;
    } else {
        posSec = pts * av_q2d(st->time_base) - m_streamStartSec - m_inPointSec;
    }
    qint64 decoded;
    { QMutexLocker l(&m_statMutex); decoded = ++m_stats.decodedVideo; }
    if (m_autoMeasuring) {
        if (m_autoWallStartNs == 0) autoWindowStart();
        else if (decoded - m_autoFirstDecoded >= m_cfg.hwAutoFrames) autoWindowClose();
    }

    // Pre-roll after a keyframe seek: discard frames before the in-point.
    const double halfOut = m_cfg.outRate.frameDurationNs() / 2e9;
    if (!m_cfg.realtime && posSec < -halfOut) return true;
    // Out-point reached: stop decoding this item.
    if (m_outPointSec > 0.0 && posSec >= m_durationSec - halfOut) return false;

    const double outPos = qMax(0.0, posSec) + m_loopOffsetSec;
    if (!m_skipActive || !m_firstFrameReady.load()) {
        // The first picture goes straight through: open() waits for it, and the cut must not wait for a second one.
        FramePtr out = convertToOutput(src);
        if (!out) return true;
        return pushVideo(out, outPos);
    }

    // PE-12: hold this picture until the next one's position says whether the output would ever pick it; pictures the
    // output would skip are dropped here, before the download / sws work.
    if (m_hasHeld) {
        m_hasHeld = false;
        if (wouldShow(m_cfg.outRate, m_heldPos, outPos)) {
            FramePtr out = convertToOutput(m_held);
            av_frame_unref(m_held);
            if (out && !pushVideo(out, m_heldPos)) return false;
        } else {
            av_frame_unref(m_held);
            QMutexLocker l(&m_statMutex);
            m_stats.droppedVideo++;
            m_stats.skippedConvert++;
        }
    }
    if (av_frame_ref(m_held, src) < 0) {
        FramePtr out = convertToOutput(src);
        if (!out) return true;
        return pushVideo(out, outPos);
    }
    m_heldPos = outPos;
    m_hasHeld = true;
    return true;
}

bool DecoderPipeline::flushHeld() {
    if (!m_hasHeld) return true;
    m_hasHeld = false;
    FramePtr out = convertToOutput(m_held);
    av_frame_unref(m_held);
    if (!out) return true;
    return pushVideo(out, m_heldPos);
}

FramePtr DecoderPipeline::downloadFrame(AVFrame* hw) {
    FramePtr f = makeFramePtr();
    if (!f) return nullptr;
    auto* fc = hw->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(hw->hw_frames_ctx->data) : nullptr;
    const AVPixelFormat fmt = fc ? fc->sw_format : AV_PIX_FMT_NONE;
    bool pooled = m_pooledDownload && fmt != AV_PIX_FMT_NONE && hw->width > 0 && hw->height > 0;
    if (pooled) {
        if (!m_dlPool || m_dlFmt != fmt || m_dlW != hw->width || m_dlH != hw->height) {
            if (m_dlPool) av_buffer_pool_uninit(&m_dlPool);
            const int size = av_image_get_buffer_size(fmt, hw->width, hw->height, 64);
            m_dlPool = size > 0 ? av_buffer_pool_init2(size_t(size) + 64, this, [](void* opaque, size_t sz) -> AVBufferRef* {
                auto* self = static_cast<DecoderPipeline*>(opaque);
                { QMutexLocker l(&self->m_statMutex); self->m_stats.downloadAllocs++; }
                return av_buffer_alloc(sz);
            }, nullptr) : nullptr;
            m_dlFmt = fmt; m_dlW = hw->width; m_dlH = hw->height;
            av_image_fill_linesizes(m_dlLinesize, fmt, hw->width);
            for (int i = 0; i < 4; ++i) m_dlLinesize[i] = (m_dlLinesize[i] + 63) & ~63;
        }
        if (m_dlPool && (f->buf[0] = av_buffer_pool_get(m_dlPool)) != nullptr) {
            f->format = fmt; f->width = hw->width; f->height = hw->height;
            if (av_image_fill_arrays(f->data, f->linesize, f->buf[0]->data, fmt, hw->width, hw->height, 64) < 0) {
                av_buffer_unref(&f->buf[0]); pooled = false;
            }
        } else {
            pooled = false;
        }
    }
    if (!pooled) {
        // Legacy path: av_hwframe_transfer_data allocates a fresh sw frame (one 3 MB allocation per picture at 1080p).
        av_frame_unref(f.get());
        QMutexLocker l(&m_statMutex); m_stats.downloadAllocs++;
    }
    if (av_hwframe_transfer_data(f.get(), hw, 0) < 0) {
        qWarning() << "[Decoder] hw frame transfer failed";
        return nullptr;
    }
    f->pts = hw->pts;
    return f;
}

FramePtr DecoderPipeline::convertToOutput(AVFrame* src) {
    AVFrame* sw = src;
    FramePtr tmp;
    QElapsedTimer timer; timer.start();
    double transferMs = 0.0;
    if (m_hwPixFmt != AV_PIX_FMT_NONE && src->format == m_hwPixFmt) {
        tmp = downloadFrame(src);
        if (!tmp) return nullptr;
        sw = tmp.get();
        transferMs = timer.nsecsElapsed() / 1e6;
    }

    FramePtr out = m_pool.get();
    if (!out) return nullptr;

    // Black (opaque) letterbox / pillarbox bars: only the bar areas (the picture area is overwritten below).
    const bool fullFrame = (m_dstW == m_cfg.outWidth && m_dstH == m_cfg.outHeight);
    if (!fullFrame) {
        auto fill = [&](int y0, int y1, int x0, int x1) {
            for (int y = qMax(0, y0); y < qMin(m_cfg.outHeight, y1); ++y) std::fill_n(reinterpret_cast<uint32_t*>(out->data[0] + y * out->linesize[0]) + x0, qMax(0, x1 - x0), 0xFF000000u);
        };
        fill(0, m_dstY, 0, m_cfg.outWidth);                                         // top
        fill(m_dstY + m_dstH, m_cfg.outHeight, 0, m_cfg.outWidth);                  // bottom
        fill(m_dstY, m_dstY + m_dstH, 0, m_dstX);                                   // left
        fill(m_dstY, m_dstY + m_dstH, m_dstX + m_dstW, m_cfg.outWidth);             // right
    }

    // Colour conversion + scale, threaded (a 1080p conversion is ~10 ms on one core, ~2 ms on several). Same matrix and
    // filters as before; the context is rebuilt only when the source geometry or format changes.
    const int flags = (m_dstW == sw->width && m_dstH == sw->height) ? SWS_POINT : SWS_BICUBIC;
    if (!m_sws || m_cvtSrcW != sw->width || m_cvtSrcH != sw->height || m_cvtSrcFmt != sw->format || m_cvtDstW != m_dstW || m_cvtDstH != m_dstH) {
        if (m_sws) sws_freeContext(m_sws);
        m_sws = sws_alloc_context();
        if (!m_sws) return nullptr;
        av_opt_set_int(m_sws, "srcw", sw->width, 0); av_opt_set_int(m_sws, "srch", sw->height, 0); av_opt_set_int(m_sws, "src_format", sw->format, 0);
        av_opt_set_int(m_sws, "dstw", m_dstW, 0); av_opt_set_int(m_sws, "dsth", m_dstH, 0); av_opt_set_int(m_sws, "dst_format", kMixerPixelFormat, 0);
        av_opt_set_int(m_sws, "sws_flags", flags, 0);
        // Threaded: measured 3.2 ms vs 7.5 ms (NV12 from the hardware decoder) and 4.2 vs 6.1 ms (planar YUV) per 1080p picture.
        static const int envThreads = qEnvironmentVariableIntValue("PLAYPLAY_DECODE_SWS_THREADS");
        av_opt_set_int(m_sws, "threads", envThreads > 0 ? envThreads : qBound(1, QThread::idealThreadCount() / 4, 6), 0);
        if (sws_init_context(m_sws, nullptr, nullptr) < 0) { sws_freeContext(m_sws); m_sws = nullptr; return nullptr; }
        m_cvtSrcW = sw->width; m_cvtSrcH = sw->height; m_cvtSrcFmt = sw->format; m_cvtDstW = m_dstW; m_cvtDstH = m_dstH;
    }
    {
        // A view of the picture area inside the pooled raster frame. sws_scale_frame writes into caller buffers only when the
        // frame carries a buffer reference (otherwise it allocates its own), so the view gets a non-owning one.
        AVFrame* view = av_frame_alloc();
        if (!view) return nullptr;
        view->format = kMixerPixelFormat; view->width = m_dstW; view->height = m_dstH;
        view->data[0] = out->data[0] + m_dstY * out->linesize[0] + m_dstX * 4; view->linesize[0] = out->linesize[0];
        view->buf[0] = av_buffer_create(view->data[0], size_t(out->linesize[0]) * m_dstH, [](void*, uint8_t*) {}, nullptr, 0);
        const int r = view->buf[0] ? sws_scale_frame(m_sws, view, sw) : AVERROR(ENOMEM);
        av_frame_free(&view);
        if (r < 0) { qWarning() << "[Decoder] colour conversion failed:" << r; return nullptr; }
    }

    // Sources that carry alpha (ProRes 4444, PNG sequences...) would punch holes in the program: force opaque. Plain YUV
    // converts to opaque BGRA already, so the 8 MB pass is skipped for it.
    const AVPixFmtDescriptor* sd = av_pix_fmt_desc_get(AVPixelFormat(sw->format));
    if (sd && (sd->flags & AV_PIX_FMT_FLAG_ALPHA)) {
        for (int y = m_dstY; y < m_dstY + m_dstH; ++y) {
            uint8_t* row = out->data[0] + y * out->linesize[0] + m_dstX * 4 + 3;
            for (int x = 0; x < m_dstW; ++x) row[x * 4] = 0xFF;
        }
    }
    out->pts = src->pts;
    {
        const double convMs = timer.nsecsElapsed() / 1e6 - transferMs;
        QMutexLocker l(&m_statMutex);
        m_stats.transferMs = m_stats.transferMs * 0.9 + transferMs * 0.1;
        m_stats.convertMs = m_stats.convertMs * 0.9 + convMs * 0.1;
        m_stats.convertedVideo++;
    }
    return out;
}

bool DecoderPipeline::pushVideo(FramePtr frame, double posSec) {
    QMutexLocker l(&m_vmutex);
    if (int(m_vqueue.size()) >= m_cfg.videoQueueDepth && !m_stop.load()) {
        QElapsedTimer w; w.start();
        while (int(m_vqueue.size()) >= m_cfg.videoQueueDepth && !m_stop.load()) {
            m_vspace.wait(&m_vmutex, 50);
        }
        if (m_autoMeasuring) m_autoWaitNs += w.nsecsElapsed();   // back-pressure, not decoder work
    }
    if (m_stop.load()) return false;
    m_vqueue.push_back({std::move(frame), posSec});
    if (!m_firstFrameReady.load()) {
        m_firstFrameReady = true;
        m_vfirst.wakeAll();
    }
    return true;
}

// ---------------------------------------------------------------------------
// PE-12 hw/sw auto measurement (decode thread)
// ---------------------------------------------------------------------------

void DecoderPipeline::autoWindowStart() {
    threadCpuMs(m_autoBase);
    m_autoWallStartNs = nowNs();
    m_autoWaitNs = 0;
    { QMutexLocker l(&m_statMutex); m_autoFirstDecoded = m_stats.decodedVideo; }
}

void DecoderPipeline::autoWindowClose() {
    if (!m_autoMeasuring) return;
    m_autoMeasuring = false;
    qint64 pictures; { QMutexLocker l(&m_statMutex); pictures = m_stats.decodedVideo - m_autoFirstDecoded; }
    if (m_autoWallStartNs == 0 || pictures < 10) return;   // too short to say anything: the next item probes again
    std::unordered_map<unsigned long, double> now;
    threadCpuMs(now);
    double ms = 0.0;
    for (const auto& kv : now) {
        if (m_autoBefore.count(kv.first)) continue;          // existed before this decoder: not its cost
        auto b = m_autoBase.find(kv.first);
        ms += kv.second - (b != m_autoBase.end() ? b->second : 0.0);
    }
    const double perPicture = qMax(0.0, ms) / double(pictures);
    // Real time: the decode thread's busy time (wall minus back-pressure waits) must cover the pictures' source duration.
    const double busyNs = double(nowNs() - m_autoWallStartNs - m_autoWaitNs);
    const double dueNs = double(pictures) * double(m_srcRate.frameDurationNs());
    const bool keepsUp = dueNs <= 0.0 || busyNs <= dueNs * 1.25;
    const bool hw = (m_hwPixFmt != AV_PIX_FMT_NONE);
    {
        QMutexLocker l(&g_autoMutex);
        AutoEntry& e = g_autoTable[m_autoKey];
        if (hw) { e.hwMs = perPicture; e.hwKeepsUp = keepsUp; } else { e.swMs = perPicture; e.swKeepsUp = keepsUp; }
    }
    { QMutexLocker l(&m_statMutex); m_stats.autoCostMs = perPicture; m_stats.autoKeepsUp = keepsUp; }
    qInfo().noquote() << QString("[Decoder] hwDecode auto: %1 %2 = %3 ms CPU per picture over %4 pictures%5")
        .arg(m_autoKey).arg(hw ? "hw" : "sw").arg(perPicture, 0, 'f', 2).arg(pictures).arg(keepsUp ? "" : " (did not keep real time)");
}

bool DecoderPipeline::handleAudioFrame(AVFrame* src) {
    if (!m_swr || !m_afifo) return true;
    AVStream* st = m_fmt->streams[m_audioStreamIndex];

    // Resample to output format.
    const int outCap = int(av_rescale_rnd(swr_get_delay(m_swr, m_actx->sample_rate) + src->nb_samples,
                                          m_cfg.sampleRate, m_actx->sample_rate, AV_ROUND_UP)) + 32;
    FramePtr conv = allocAudioFrame(outCap, m_cfg.channels, m_cfg.sampleRate);
    if (!conv) return true;
    int got = swr_convert(m_swr, conv->data, outCap, const_cast<const uint8_t**>(src->extended_data), src->nb_samples);
    if (got <= 0) return true;

    uint8_t* planes[64];
    for (int c = 0; c < m_cfg.channels; ++c) planes[c] = conv->data[c];
    int offset = 0;

    if (!m_audioAligned && m_cfg.realtime) m_audioAligned = true;   // live: free-running
    if (!m_audioAligned) {
        // Align the first audio sample to the video timeline origin (in-point).
        int64_t pts = src->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = src->pts;
        double aPos = (pts == AV_NOPTS_VALUE) ? 0.0
            : pts * av_q2d(st->time_base) - m_streamStartSec - m_inPointSec;
        // swr delay: the converted block starts slightly earlier than src pts.
        aPos -= swr_get_delay(m_swr, m_cfg.sampleRate) / double(m_cfg.sampleRate);
        aPos += m_loopOffsetSec;
        if (aPos > 0.0) {
            pushAudioSilence(qint64(std::llround(aPos * m_cfg.sampleRate)));
        } else if (aPos < 0.0) {
            const int skip = int(std::llround(-aPos * m_cfg.sampleRate));
            if (skip >= got) return true;        // whole block is pre-roll
            offset = skip;
        }
        m_audioAligned = true;
    }

    int n = got - offset;
    if (m_audioOutLimit >= 0) {
        const qint64 room = m_audioOutLimit - m_audioSamplesPushed;
        if (room <= 0) return true;
        n = int(qMin<qint64>(n, room));
    }
    for (int c = 0; c < m_cfg.channels; ++c) planes[c] += offset * sizeof(float);
    pushAudioSamples(planes, n);
    return true;
}

void DecoderPipeline::pushAudioSamples(uint8_t** planes, int nSamples) {
    if (nSamples <= 0) return;
    QMutexLocker l(&m_amutex);
    if (m_cfg.realtime) {
        // Live: bound latency; drop the oldest audio instead of blocking the capture thread.
        const int cap = m_cfg.sampleRate / 4;
        const int have = av_audio_fifo_size(m_afifo);
        if (have + nSamples > cap) av_audio_fifo_drain(m_afifo, qMin(have, have + nSamples - cap));
        av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(planes), nSamples);
        m_audioSamplesPushed += nSamples;
        QMutexLocker s(&m_statMutex); m_stats.decodedAudioSamples += nSamples;
        return;
    }
    const int maxSamples = int(m_cfg.audioFifoMaxSec * m_cfg.sampleRate);
    if (av_audio_fifo_size(m_afifo) + nSamples > maxSamples && !m_stop.load()) {
        QElapsedTimer w; w.start();
        while (av_audio_fifo_size(m_afifo) + nSamples > maxSamples && !m_stop.load()) {
            m_aspace.wait(&m_amutex, 50);
        }
        if (m_autoMeasuring) m_autoWaitNs += w.nsecsElapsed();
    }
    if (m_stop.load()) return;
    av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(planes), nSamples);
    m_audioSamplesPushed += nSamples;
    QMutexLocker s(&m_statMutex);
    m_stats.decodedAudioSamples += nSamples;
}

void DecoderPipeline::pushAudioSilence(qint64 nSamples) {
    if (nSamples <= 0) return;
    const int chunk = 4096;
    FramePtr z = allocAudioFrame(chunk, m_cfg.channels, m_cfg.sampleRate);
    if (!z) return;
    silenceAudioFrame(z);
    while (nSamples > 0 && !m_stop.load()) {
        const int n = int(qMin<qint64>(chunk, nSamples));
        uint8_t* planes[64];
        for (int c = 0; c < m_cfg.channels; ++c) planes[c] = z->data[c];
        pushAudioSamples(planes, n);
        nSamples -= n;
    }
}

// ---------------------------------------------------------------------------
// Output side
// ---------------------------------------------------------------------------

FramePtr DecoderPipeline::videoFrameAt(double posSec) {
    QMutexLocker l(&m_vmutex);
    if (m_cfg.realtime) {
        // Live input: always show the newest decoded frame; count skipped ones.
        if (m_vqueue.empty()) {
            if (m_lastShown && !m_lastShownRepeated) { m_lastShownRepeated = true; QMutexLocker s(&m_statMutex); m_stats.repeatedVideo++; }
            return m_lastShown;
        }
        const int skipped = int(m_vqueue.size()) - 1;
        m_lastShown = std::move(m_vqueue.back().frame);
        m_vqueue.clear();
        m_lastShownRepeated = false;
        m_vspace.wakeAll();
        if (skipped > 0) { QMutexLocker s(&m_statMutex); m_stats.droppedVideo += skipped; }
        return m_lastShown;
    }
    const double tol = m_cfg.outRate.frameDurationNs() / 4e9; // quarter output frame
    FramePtr pick;
    int skipped = -1;
    while (!m_vqueue.empty() && (m_vqueue.front().posSec <= posSec + tol || !m_lastShown)) {
        pick = std::move(m_vqueue.front().frame);
        m_lastShownPos = m_vqueue.front().posSec;
        m_vqueue.pop_front();
        ++skipped;
        if (!m_lastShown && pick) { m_lastShown = pick; break; } // first frame: take it, do not skip
    }
    if (pick) {
        m_lastShown = pick;
        m_lastShownRepeated = false;
        m_vspace.wakeAll();
        if (skipped > 0) { QMutexLocker s(&m_statMutex); m_stats.droppedVideo += skipped; }
    } else if (m_lastShown && !m_lastShownRepeated) {
        m_lastShownRepeated = true;
        QMutexLocker s(&m_statMutex); m_stats.repeatedVideo++;
    }
    return m_lastShown;
}

bool DecoderPipeline::pullAudio(float* const* dst, int nSamples) {
    QMutexLocker l(&m_amutex);
    int avail = m_afifo ? av_audio_fifo_size(m_afifo) : 0;
    const int take = qMin(avail, nSamples);
    if (take > 0) {
        av_audio_fifo_read(m_afifo, reinterpret_cast<void* const*>(const_cast<float**>(dst)), take);
        m_aspace.wakeAll();
    }
    const int pad = nSamples - take;
    if (pad > 0) {
        for (int c = 0; c < m_cfg.channels; ++c) {
            memset(dst[c] + take, 0, size_t(pad) * sizeof(float));
        }
        // Silence after EOF is expected (tail of the item), not an underrun.
        if (!m_eof.load() || avail > 0) {
            QMutexLocker s(&m_statMutex);
            m_stats.audioUnderruns++;
            m_stats.audioSilenceSamples += pad;
        }
        return false;
    }
    return true;
}

bool DecoderPipeline::pullAudio(const FramePtr& audioFrame) {
    if (!audioFrame) return false;
    float* planes[64];
    for (int c = 0; c < m_cfg.channels && c < 64; ++c) planes[c] = reinterpret_cast<float*>(audioFrame->data[c]);
    return pullAudio(planes, audioFrame->nb_samples);
}

bool DecoderPipeline::atEnd() const {
    if (m_cfg.realtime) return false;
    if (!m_eof.load()) return false;
    QMutexLocker l(&m_vmutex);
    return m_vqueue.empty();
}
