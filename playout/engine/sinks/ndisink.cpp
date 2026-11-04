#include "ndisink.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QMutex>
#include <cstring>

extern "C" {
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------------------------------------------------------------
// NdiUyvyConverter (PE-11)
// ---------------------------------------------------------------------------------------------------------------------

NdiUyvyConverter::~NdiUyvyConverter() {
    if (m_sws) sws_freeContext(m_sws);
}

int NdiUyvyConverter::defaultThreads() {
    const int env = qEnvironmentVariableIntValue("PLAYPLAY_NDI_SWS_THREADS");
    if (env > 0) return qMin(env, 16);
    // 1080p BGRA -> UYVY is a few ms on one core; up to 4 slices keep it ~1 ms without crowding the encoders' cores.
    return qBound(1, QThread::idealThreadCount() / 8, 4);
}

bool NdiUyvyConverter::reinit(int w, int h) {
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    m_ring.clear();
    m_w = m_h = 0;
    m_threads = defaultThreads();
    m_sws = sws_alloc_context();
    if (!m_sws) return false;
    av_opt_set_int(m_sws, "srcw", w, 0); av_opt_set_int(m_sws, "srch", h, 0); av_opt_set_int(m_sws, "src_format", AV_PIX_FMT_BGRA, 0);
    av_opt_set_int(m_sws, "dstw", w, 0); av_opt_set_int(m_sws, "dsth", h, 0); av_opt_set_int(m_sws, "dst_format", AV_PIX_FMT_UYVY422, 0);
    av_opt_set_int(m_sws, "sws_flags", SWS_BILINEAR | SWS_ACCURATE_RND, 0);
    av_opt_set_int(m_sws, "threads", m_threads, 0);
    if (sws_init_context(m_sws, nullptr, nullptr) < 0) { sws_freeContext(m_sws); m_sws = nullptr; return false; }
    // Full-range RGB in, limited-range YCbCr out, in the matrix NDI decodes UYVY with for this raster size.
    const int matrix = h >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    int* inv = nullptr; int* tbl = nullptr; int srcRange, dstRange, br, con, sat;
    if (sws_getColorspaceDetails(m_sws, &inv, &srcRange, &tbl, &dstRange, &br, &con, &sat) >= 0)
        sws_setColorspaceDetails(m_sws, sws_getCoefficients(matrix), 1, sws_getCoefficients(matrix), 0, br, con, sat);
    m_w = w; m_h = h;
    return true;
}

FramePtr NdiUyvyConverter::convert(const AVFrame* bgra) {
    if (!bgra || bgra->format != AV_PIX_FMT_BGRA || bgra->width <= 0 || bgra->height <= 0 || (bgra->width & 1)) return nullptr;
    if ((bgra->width != m_w || bgra->height != m_h || !m_sws) && !reinit(bgra->width, bgra->height)) return nullptr;
    QElapsedTimer t; t.start();
    // A free ring picture: held only by the ring (the in-flight one is also held by the sink until NDI lets go).
    FramePtr dst;
    for (const FramePtr& p : m_ring) if (p.use_count() == 1) { dst = p; break; }
    if (!dst) {
        if (m_ring.size() >= 3) return nullptr;    // everything still referenced: never write under NDI's feet
        dst = allocVideoFrame(m_w, m_h, AV_PIX_FMT_UYVY422);
        if (!dst) return nullptr;
        dst->color_range = AVCOL_RANGE_MPEG;
        dst->colorspace = m_h >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        m_ring.push_back(dst);
    }
    if (sws_scale_frame(m_sws, dst.get(), bgra) < 0) return nullptr;
    m_lastMs = t.nsecsElapsed() / 1e6;
    return dst;
}

// ---------------------------------------------------------------------------------------------------------------------
// NdiSink
// ---------------------------------------------------------------------------------------------------------------------

#ifdef HAS_NDI
namespace {
QMutex g_ndiInitMutex;
int g_ndiRefs = 0;
bool ndiAcquire() {
    QMutexLocker l(&g_ndiInitMutex);
    if (g_ndiRefs == 0 && !NDIlib_initialize()) return false;
    ++g_ndiRefs;
    return true;
}
void ndiRelease() {
    QMutexLocker l(&g_ndiInitMutex);
    if (g_ndiRefs > 0 && --g_ndiRefs == 0) NDIlib_destroy();
}
} // namespace
#endif

NdiSink::NdiSink(const QString& name, const QString& ndiSourceName, const QString& groups, QObject* parent)
    : BasebandSink(name, parent)
    , m_sourceName(ndiSourceName.isEmpty() ? name : ndiSourceName)
    , m_groups(groups)
{
    setFourcc(QString());      // PLAYPLAY_NDI_FOURCC or the default
}

NdiSink::VideoFourcc NdiSink::parseFourcc(const QString& s, bool* ok) {
    const QString v = s.trimmed().toLower();
    if (ok) *ok = true;
    if (v == "bgra") return VideoFourcc::Bgra;
    if (v == "bgrx") return VideoFourcc::Bgrx;
    if (v == "uyvy") return VideoFourcc::Uyvy;
    if (ok) *ok = false;
    return VideoFourcc::Bgrx;
}

QString NdiSink::fourccName(VideoFourcc f) {
    switch (f) {
    case VideoFourcc::Bgra: return "bgra";
    case VideoFourcc::Uyvy: return "uyvy";
    case VideoFourcc::Bgrx: break;
    }
    return "bgrx";
}

void NdiSink::setFourcc(const QString& requested) {
    // The output's own option wins; the environment is the station-wide switch (PLAYPLAY_NDI_FOURCC=bgra restores the
    // pre-PE-11 sender without a rebuild).
    const QString env = qEnvironmentVariable("PLAYPLAY_NDI_FOURCC").trimmed();
    const QString want = !requested.trimmed().isEmpty() ? requested : env;
    bool ok = true;
    m_fourcc = want.isEmpty() ? VideoFourcc::Bgrx : parseFourcc(want, &ok);
    if (!ok) qWarning().noquote() << QString("[NdiSink] %1: unknown fourcc '%2', sending bgrx").arg(m_sourceName, want);
}

NdiSink::~NdiSink() {
    stop();
}

bool NdiSink::isAvailable() {
#ifdef HAS_NDI
    return true;
#else
    return false;
#endif
}

bool NdiSink::start(const Format& fmt) {
    if (m_running.load()) return true;
    m_fmt = fmt;
#ifdef HAS_NDI
    if (!ndiAcquire()) { setError("NDIlib_initialize failed (CPU unsupported?)"); return false; }
    NDIlib_send_create_t desc;
    const QByteArray nameUtf8 = m_sourceName.toUtf8();
    const QByteArray groupsUtf8 = m_groups.toUtf8();
    desc.p_ndi_name = nameUtf8.constData();
    desc.p_groups = m_groups.isEmpty() ? nullptr : groupsUtf8.constData();
    desc.clock_video = false;   // we are clocked by the FrameClock, not by NDI
    desc.clock_audio = false;
    m_send = NDIlib_send_create(&desc);
    if (!m_send) { setError("NDIlib_send_create failed"); ndiRelease(); return false; }

    m_stop = false;
    m_running = true;
    m_uyvy.reset(m_fourcc == VideoFourcc::Uyvy ? new NdiUyvyConverter : nullptr);
    m_sentFourcc = fourccName(m_fourcc);
    m_convMsAvg = m_convMsMax = 0.0; m_convFrames = 0;
    {
        QMutexLocker l(&m_statMutex);
        m_stats.connected = true;
        m_stats.extra["fourcc"] = m_sentFourcc;
        m_stats.extra["fourccRequested"] = fourccName(m_fourcc);
    }
    m_worker = new Worker(this);
    m_worker->start(QThread::HighPriority);
    qInfo().noquote() << QString("[NdiSink] sending '%1' %2x%3 @%4 as %5%6").arg(m_sourceName)
        .arg(fmt.width).arg(fmt.height).arg(fmt.rate.toString()).arg(fourccName(m_fourcc))
        .arg(m_uyvy ? QString(" (sws %1 threads)").arg(NdiUyvyConverter::defaultThreads()) : QString());
    return true;
#else
    setError("NDI support not compiled in");
    return false;
#endif
}

void NdiSink::stop() {
    if (!m_running.load() && !m_worker) return;
    m_stop = true;
    { QMutexLocker l(&m_qmutex); m_qcond.wakeAll(); }
    if (m_worker) { m_worker->wait(3000); delete m_worker; m_worker = nullptr; }
    { QMutexLocker l(&m_qmutex); m_queue.clear(); }
#ifdef HAS_NDI
    if (m_send) {
        NDIlib_send_send_video_async_v2(m_send, nullptr); // flush in-flight buffer
        NDIlib_send_destroy(m_send);
        m_send = nullptr;
        ndiRelease();
    }
    m_inFlight.reset();
    m_uyvy.reset();            // after the flush above: NDI no longer reads a ring picture
#endif
    m_running = false;
    QMutexLocker l(&m_statMutex); m_stats.connected = false;
}

void NdiSink::pushFrame(const MediaFrame& frame) {
    if (!m_running.load()) return;
    QMutexLocker l(&m_qmutex);
    if (int(m_queue.size()) >= kMaxQueue) {
        m_queue.pop_front();
        QMutexLocker s(&m_statMutex); m_stats.dropped++;
    }
    m_queue.push_back(frame);
    { QMutexLocker s(&m_statMutex); m_stats.framesIn++; }
    m_qcond.wakeOne();
}

void NdiSink::sendLoop() {
    while (!m_stop.load()) {
        MediaFrame f;
        {
            QMutexLocker l(&m_qmutex);
            while (m_queue.empty() && !m_stop.load()) m_qcond.wait(&m_qmutex, 100);
            if (m_queue.empty()) continue;
            f = std::move(m_queue.front());
            m_queue.pop_front();
        }
        sendOne(f);
    }
}

void NdiSink::sendOne(const MediaFrame& f) {
#ifdef HAS_NDI
    if (!m_send) return;
    if (f.video) {
        // What goes out: the program picture as BGRX (default) / BGRA, or a UYVY picture converted here. A picture that
        // cannot be converted (odd width, not BGRA, sws failure) still goes out, as BGRX, rather than freezing NDI.
        FramePtr pic = f.video;
        NDIlib_FourCC_video_type_e cc = m_fourcc == VideoFourcc::Bgra ? NDIlib_FourCC_video_type_BGRA : NDIlib_FourCC_video_type_BGRX;
        QString sent = m_fourcc == VideoFourcc::Bgra ? QStringLiteral("bgra") : QStringLiteral("bgrx");
        if (m_uyvy) {
            if (FramePtr u = m_uyvy->convert(f.video.get())) {
                pic = u; cc = NDIlib_FourCC_video_type_UYVY; sent = QStringLiteral("uyvy");
                const double ms = m_uyvy->lastMs();
                m_convMsAvg = m_convFrames++ ? m_convMsAvg * 0.95 + ms * 0.05 : ms;
                m_convMsMax = qMax(m_convMsMax, ms);
            }
        }
        NDIlib_video_frame_v2_t v;
        v.xres = pic->width;
        v.yres = pic->height;
        v.FourCC = cc;
        v.frame_rate_N = m_fmt.rate.num;
        v.frame_rate_D = m_fmt.rate.den;
        v.picture_aspect_ratio = float(v.xres) / float(v.yres);
        v.frame_format_type = NDIlib_frame_format_type_progressive;
        v.timecode = NDIlib_send_timecode_synthesize;
        v.p_data = pic->data[0];
        v.line_stride_in_bytes = pic->linesize[0];
        v.p_metadata = nullptr;
        v.timestamp = 0;
        NDIlib_send_send_video_async_v2(m_send, &v);
        m_inFlight = pic;        // keep alive until the next async call returns (a UYVY ring picture stays out of reuse)
        if (sent != m_sentFourcc || (m_uyvy && m_convFrames % 30 == 1)) {
            if (sent != m_sentFourcc)
                qInfo().noquote() << QString("[NdiSink] '%1' now sending %2 (requested %3)").arg(m_sourceName, sent, fourccName(m_fourcc));
            m_sentFourcc = sent;
            QMutexLocker s(&m_statMutex);
            m_stats.extra["fourcc"] = sent;
            if (m_uyvy) {
                m_stats.extra["convertMs"] = qRound(m_convMsAvg * 100.0) / 100.0;
                m_stats.extra["convertMsMax"] = qRound(m_convMsMax * 100.0) / 100.0;
                m_stats.extra["swsThreads"] = m_uyvy->threads();
            }
        }
    }
    if (f.audio && f.audio->nb_samples > 0) {
        const int ch = f.audio->ch_layout.nb_channels;
        const int n = f.audio->nb_samples;
        m_audioScratch.resize(size_t(ch) * n);
        for (int c = 0; c < ch; ++c) {
            memcpy(m_audioScratch.data() + size_t(c) * n, f.audio->data[c], size_t(n) * sizeof(float));
        }
        NDIlib_audio_frame_v3_t a;
        a.sample_rate = f.audio->sample_rate;
        a.no_channels = ch;
        a.no_samples = n;
        a.timecode = NDIlib_send_timecode_synthesize;
        a.FourCC = NDIlib_FourCC_audio_type_FLTP;
        a.p_data = reinterpret_cast<uint8_t*>(m_audioScratch.data());
        a.channel_stride_in_bytes = n * int(sizeof(float));
        a.p_metadata = nullptr;
        a.timestamp = 0;
        NDIlib_send_send_audio_v3(m_send, &a);
    }
    QMutexLocker s(&m_statMutex);
    m_stats.framesOut++;
#else
    Q_UNUSED(f);
#endif
}
