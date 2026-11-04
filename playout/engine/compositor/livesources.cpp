#include "livesources.h"
#include <chrono>
#include <QDebug>
#include <QDateTime>
#include <QJsonArray>
#include <cmath>
#include <cstring>

#ifdef HAS_NDI
#include <Processing.NDI.Lib.h>
#endif
#ifdef HAS_DEKTEC
#include <DTAPI.h>
#endif

// ===========================================================================
// LiveSourceBase
// ===========================================================================

LiveSourceBase::~LiveSourceBase() {
    if (m_afifo) av_audio_fifo_free(m_afifo);
    if (m_swr) swr_free(&m_swr);
}

void LiveSourceBase::resetAudio() {
    QMutexLocker p(&m_producerMutex);                       // lock order everywhere: producer, then audio
    QMutexLocker l(&m_audioMutex);
    if (m_afifo) av_audio_fifo_free(m_afifo);
    m_afifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, m_fmt.channels, m_fmt.sampleRate / 2);
    if (m_swr) swr_free(&m_swr);
    m_swrInChannels = m_swrInRate = 0;
    m_audioPlanes.assign(size_t(m_fmt.channels), nullptr);
}

void LiveSourceBase::dropBufferedAudio() {
    QMutexLocker l(&m_audioMutex);
    if (m_afifo) av_audio_fifo_reset(m_afifo);
}

void LiveSourceBase::setConnected(bool on) { m_connected = on; }

void LiveSourceBase::setStatusText(const QString& text) {
    QMutexLocker l(&m_mutex);
    m_statusText = text;
}

void LiveSourceBase::clearVideo() {
    FramePtr old;                                           // declared first: destroyed after the locker unlocks
    QMutexLocker l(&m_videoMutex);
    old = std::move(m_latest);
    m_servedLatest = false;
}

void LiveSourceBase::publishVideo(const FramePtr& bgra) {
    if (!bgra) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    FramePtr old;                                           // the replaced picture is released after the unlock:
    {                                                       // a frame free never runs under the slot lock
        QMutexLocker l(&m_videoMutex);
        if (m_latest && !m_servedLatest) m_videoDropped++;  // compositor never saw the previous one
        old = std::move(m_latest);
        m_latest = bgra;
        m_servedLatest = false;
        m_nativeSize = QSize(bgra->width, bgra->height);
        m_lastVideoMs = now;
    }
    m_videoIn++;
}

void LiveSourceBase::publishAudio(const float* const* planes, bool planar, int channels, int rate, int nSamples) {
    if (!m_wantAudio || nSamples <= 0 || channels <= 0 || rate <= 0 || !planes || !planes[0]) return;
    // Resample / re-layout to the raster's channel count and rate under the producer lock only: the clock thread's
    // audio() competes just for the FIFO write below, never for swr_convert.
    QMutexLocker p(&m_producerMutex);
    if (!m_afifo) return;                                   // the pointer only changes under this lock too
    if (!m_swr || m_swrInChannels != channels || m_swrInRate != rate) {
        if (m_swr) swr_free(&m_swr);
        AVChannelLayout in, out;
        av_channel_layout_default(&in, channels);
        av_channel_layout_default(&out, m_fmt.channels);
        swr_alloc_set_opts2(&m_swr, &out, AV_SAMPLE_FMT_FLTP, m_fmt.sampleRate, &in, planar ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT, rate, 0, nullptr);
        av_channel_layout_uninit(&in); av_channel_layout_uninit(&out);
        if (!m_swr || swr_init(m_swr) < 0) { ++m_audioConvertErrors; if (m_swr) swr_free(&m_swr); return; }
        m_swrInChannels = channels; m_swrInRate = rate;
    }
    const int outMax = int(av_rescale_rnd(nSamples, m_fmt.sampleRate, rate, AV_ROUND_UP)) + 64;
    m_audioScratch.resize(size_t(outMax) * m_fmt.channels);
    for (int c = 0; c < m_fmt.channels; ++c) m_audioPlanes[size_t(c)] = m_audioScratch.data() + size_t(c) * outMax;
    m_audioInPtrs.resize(size_t(planar ? channels : 1));
    for (size_t i = 0; i < m_audioInPtrs.size(); ++i) m_audioInPtrs[i] = reinterpret_cast<const uint8_t*>(planes[i]);
    const int got = swr_convert(m_swr, reinterpret_cast<uint8_t**>(m_audioPlanes.data()), outMax, m_audioInPtrs.data(), nSamples);
    if (got < 0) { ++m_audioConvertErrors; return; }
    if (got == 0) return;
    QMutexLocker l(&m_audioMutex);                          // one FIFO write (a memcpy of one block)
    const int cap = m_fmt.sampleRate / 4;
    const int over = av_audio_fifo_size(m_afifo) + got - cap;
    if (over > 0) { av_audio_fifo_drain(m_afifo, over); m_audioOverflow += over; }
    av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(m_audioPlanes.data()), got);
}

FramePtr LiveSourceBase::video(qint64, double) {
    QMutexLocker legacy(m_legacyLock ? &m_mutex : nullptr);  // A/B switch only: the old shared lock
    QMutexLocker l(&m_videoMutex);                          // a FramePtr copy; the receive thread holds it as briefly
    if (m_latest && m_servedLatest) m_videoRepeats++;
    m_servedLatest = true;
    return m_latest;
}

bool LiveSourceBase::audio(const FramePtr& dst, qint64, double) {
    if (!m_wantAudio || !dst) return false;
    QMutexLocker legacy(m_legacyLock ? &m_mutex : nullptr);
    const int n = dst->nb_samples;
    int take = 0;
    {
        QMutexLocker l(&m_audioMutex);                      // the producer holds it for one FIFO write only
        if (!m_afifo) return false;
        take = qMin(av_audio_fifo_size(m_afifo), n);
        if (take > 0) av_audio_fifo_read(m_afifo, reinterpret_cast<void**>(dst->data), take);
    }
    if (take <= 0) { m_audioUnderrun += n; return false; }
    if (take < n) { m_audioUnderrun += (n - take); for (int c = 0; c < m_fmt.channels; ++c) memset(reinterpret_cast<float*>(dst->data[c]) + take, 0, size_t(n - take) * sizeof(float)); }
    return true;
}

QSize LiveSourceBase::nativeSize() const {
    if (m_fillRaster) return QSize();
    QMutexLocker l(&m_videoMutex);
    return m_nativeSize;
}

QJsonObject LiveSourceBase::status() const {
    // Copy what is shared under the short locks, build the JSON after: the receive thread never waits on a status call
    // and (since PE-03) never sleeps holding m_mutex, so this returns in microseconds while a sender is being searched.
    qint64 lastVideoMs = 0; QSize size; QString text;
    { QMutexLocker l(&m_videoMutex); lastVideoMs = m_lastVideoMs; size = m_nativeSize; }
    { QMutexLocker l(&m_mutex); text = m_statusText; }
    QJsonObject o = LayerSource::status();
    o["connected"] = m_connected.load();
    o["videoIn"] = double(m_videoIn.load()); o["videoRepeats"] = double(m_videoRepeats.load()); o["videoDropped"] = double(m_videoDropped.load());
    o["audioOverflow"] = double(m_audioOverflow.load()); o["audioUnderrun"] = double(m_audioUnderrun.load());
    o["audioEnabled"] = m_wantAudio;
    o["audioFramesIn"] = double(m_audioFramesIn.load()); o["audioSamplesIn"] = double(m_audioSamplesIn.load());
    o["audioInvalidFrames"] = double(m_audioInvalid.load());
    o["audioConvertErrors"] = double(m_audioConvertErrors.load());
    o["audioInputRate"] = m_audioInputRate.load(); o["audioInputChannels"] = m_audioInputChannels.load();
    o["audioInputPeak"] = double(m_audioInputPeak.load());
    const qint64 lastAudio = m_lastAudioMs.load();
    if (lastAudio) o["sinceLastAudioMs"] = double(QDateTime::currentMSecsSinceEpoch() - lastAudio);
    if (lastVideoMs) o["sinceLastVideoMs"] = double(QDateTime::currentMSecsSinceEpoch() - lastVideoMs);
    if (!size.isEmpty()) { o["width"] = size.width(); o["height"] = size.height(); }
    if (!text.isEmpty()) o["detail"] = text;
    if (m_legacyLock) o["legacyLock"] = true;
    return o;
}

// ===========================================================================
// NdiSource
// ===========================================================================

#ifdef HAS_NDI
namespace {
QMutex g_ndiRxInitMutex; int g_ndiRxRefs = 0;
bool ndiRxAcquire() { QMutexLocker l(&g_ndiRxInitMutex); if (g_ndiRxRefs == 0 && !NDIlib_initialize()) return false; ++g_ndiRxRefs; return true; }
void ndiRxRelease() { QMutexLocker l(&g_ndiRxInitMutex); if (g_ndiRxRefs > 0 && --g_ndiRxRefs == 0) NDIlib_destroy(); }
}
#endif

struct NdiSource::Impl {
#ifdef HAS_NDI
    NDIlib_recv_instance_t recv = nullptr;
    NDIlib_find_instance_t find = nullptr;
    bool ndiUp = false;
#endif
    QString connectedName;
};

class NdiSource::Worker : public QThread {
public:
    explicit Worker(NdiSource* s) : m_s(s) {}
    void run() override { m_s->receiveLoop(); }
private:
    NdiSource* m_s;
};

NdiSource::NdiSource(const QJsonObject& spec, QObject* parent) : LiveSourceBase(parent), m_impl(new Impl), m_spec(spec) {
    m_wantAudio = spec.value("audio").toBool(true);
    m_fillRaster = spec.value("fillRaster").toBool(true);
}

NdiSource::~NdiSource() { close(); delete m_impl; }

bool NdiSource::isAvailable() {
#ifdef HAS_NDI
    return true;
#else
    return false;
#endif
}

QStringList NdiSource::discover(int waitMs, const QString& groups, const QString& extraIps) {
    QStringList out;
#ifdef HAS_NDI
    if (!ndiRxAcquire()) return out;
    const QByteArray g = groups.toUtf8(), e = extraIps.toUtf8();
    NDIlib_find_create_t fc; fc.show_local_sources = true; fc.p_groups = groups.isEmpty() ? nullptr : g.constData(); fc.p_extra_ips = extraIps.isEmpty() ? nullptr : e.constData();
    NDIlib_find_instance_t f = NDIlib_find_create_v2(&fc);
    if (f) {
        NDIlib_find_wait_for_sources(f, uint32_t(waitMs));
        uint32_t n = 0; const NDIlib_source_t* src = NDIlib_find_get_current_sources(f, &n);
        for (uint32_t i = 0; i < n; ++i) out << QString::fromUtf8(src[i].p_ndi_name);
        NDIlib_find_destroy(f);
    }
    ndiRxRelease();
#else
    Q_UNUSED(waitMs); Q_UNUSED(groups); Q_UNUSED(extraIps);
#endif
    return out;
}

QJsonArray NdiSource::discoverFull(int waitMs, const QString& groups, const QString& extraIps) {
    QJsonArray out;
#ifdef HAS_NDI
    if (!ndiRxAcquire()) return out;
    const QByteArray g = groups.toUtf8(), e = extraIps.toUtf8();
    NDIlib_find_create_t fc; fc.show_local_sources = true; fc.p_groups = groups.isEmpty() ? nullptr : g.constData(); fc.p_extra_ips = extraIps.isEmpty() ? nullptr : e.constData();
    NDIlib_find_instance_t f = NDIlib_find_create_v2(&fc);
    if (f) {
        // wait_for_sources returns as soon as the list *changes*, so a single wait yields a partial
        // list while mDNS answers are still arriving. Wait for the first change, then keep waiting
        // until the list has been stable for 400 ms (bounded by 2x waitMs overall).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2 * waitMs);
        NDIlib_find_wait_for_sources(f, uint32_t(waitMs));
        while (std::chrono::steady_clock::now() < deadline && NDIlib_find_wait_for_sources(f, 400)) {}
        uint32_t n = 0; const NDIlib_source_t* src = NDIlib_find_get_current_sources(f, &n);
        for (uint32_t i = 0; i < n; ++i) {
            QJsonObject o; o["type"] = "ndi";
            const QString name = QString::fromUtf8(src[i].p_ndi_name ? src[i].p_ndi_name : "");
            const QString url = QString::fromUtf8(src[i].p_url_address ? src[i].p_url_address : "");
            o["name"] = name; o["url"] = url;
            const int paren = name.indexOf(" (");
            o["host"] = paren > 0 ? name.left(paren) : QString();
            o["source"] = paren > 0 && name.endsWith(')') ? name.mid(paren + 2, name.size() - paren - 3) : name;
            const int colon = url.lastIndexOf(':');
            o["ip"] = colon > 0 ? url.left(colon) : url; o["port"] = colon > 0 ? url.mid(colon + 1).toInt() : 0;
            out.append(o);
        }
        NDIlib_find_destroy(f);
    }
    ndiRxRelease();
#else
    Q_UNUSED(waitMs); Q_UNUSED(groups); Q_UNUSED(extraIps);
#endif
    return out;
}

bool NdiSource::open(const OutputFormat& fmt) {
    if (m_open) return true;
    m_fmt = fmt;
    resetAudio();
#ifdef HAS_NDI
    if (!ndiRxAcquire()) { setError("NDIlib_initialize failed"); return false; }
    m_impl->ndiUp = true;
    NDIlib_recv_create_v3_t rc;
    rc.source_to_connect_to = NDIlib_source_t();
    rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    rc.bandwidth = m_spec.value("bandwidth").toString("highest") == "lowest" ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
    rc.allow_video_fields = false;      // we want progressive BGRA
    const QByteArray recvName = m_spec.value("recvName").toString("PlayPlay").toUtf8();
    rc.p_ndi_recv_name = recvName.constData();
    m_impl->recv = NDIlib_recv_create_v3(&rc);
    if (!m_impl->recv) { setError("NDIlib_recv_create_v3 failed"); ndiRxRelease(); m_impl->ndiUp = false; return false; }
    m_stop = false;
    m_open = true;
    // const reads, before the receive thread starts (a non-const m_spec[...] would insert into the spec it copies)
    const QString lookingFor = m_spec.contains("url") ? m_spec.value("url").toString() : m_spec.value("name").toString();
    m_worker = new Worker(this);
    m_worker->start(QThread::TimeCriticalPriority);
    qInfo().noquote() << "[NdiSource] looking for" << lookingFor;
    return true;
#else
    setError("NDI support not compiled in");
    return false;
#endif
}

void NdiSource::close() {
    if (!m_open && !m_worker) return;
    { QMutexLocker l(&m_stopMutex); m_stop = true; m_stopCv.wakeAll(); }   // ends a search pause at once
    if (m_worker) { m_worker->wait(3000); delete m_worker; m_worker = nullptr; }
#ifdef HAS_NDI
    if (m_impl->find) { NDIlib_find_destroy(m_impl->find); m_impl->find = nullptr; }
    if (m_impl->recv) { NDIlib_recv_destroy(m_impl->recv); m_impl->recv = nullptr; }
    if (m_impl->ndiUp) { ndiRxRelease(); m_impl->ndiUp = false; }
#endif
    clearVideo();
    m_open = false;
    setConnected(false);
}

void NdiSource::update(const QJsonObject& params) {
    // Switching the source: reconnect in the receive loop. m_spec is read by the receive thread, so it changes under m_mutex.
    QMutexLocker l(&m_mutex);
    bool changed = false;
    for (const char* k : {"name", "url"}) if (params.contains(k) && params[k] != m_spec.value(k)) { m_spec[k] = params[k]; changed = true; }
    if (changed) { m_impl->connectedName.clear(); setConnected(false); }
}

QJsonObject NdiSource::status() const {
    QJsonObject o = LiveSourceBase::status();
    QString connectedName; QJsonObject spec;
    { QMutexLocker l(&m_mutex); connectedName = m_impl->connectedName; spec = m_spec; }   // implicitly shared copies
    o["source"] = connectedName.isEmpty() ? spec.value("name").toString(spec.value("url").toString()) : connectedName;
    return o;
}

void NdiSource::waitForStop(int ms) {
    QMutexLocker l(&m_stopMutex);
    if (!m_stop.load()) m_stopCv.wait(&m_stopMutex, QDeadlineTimer(ms));
}

void NdiSource::receiveLoop() {
#ifdef HAS_NDI
    QString wantName, wantUrl;
    QJsonObject spec;
    QByteArray urlUtf8, nameUtf8;
    bool connected = false;
    qint64 lastFrameMs = 0;
    while (!m_stop.load()) {
        QString currentConnected;
        { QMutexLocker l(&m_mutex); currentConnected = m_impl->connectedName; spec = m_spec; }
        wantName = spec.value("name").toString(); wantUrl = spec.value("url").toString();
        if (currentConnected.isEmpty()) connected = false;
        if (!connected) {
            NDIlib_source_t src;
            bool found = false;
            if (!wantUrl.isEmpty()) {
                urlUtf8 = wantUrl.toUtf8(); src.p_url_address = urlUtf8.constData(); src.p_ndi_name = nullptr; found = true;
                nameUtf8 = wantUrl.toUtf8();
            } else {
                if (!m_impl->find) {
                    const QByteArray g = spec.value("groups").toString().toUtf8(), e = spec.value("extraIps").toString().toUtf8();
                    NDIlib_find_create_t fc; fc.show_local_sources = true; fc.p_groups = g.isEmpty() ? nullptr : g.constData(); fc.p_extra_ips = e.isEmpty() ? nullptr : e.constData();
                    m_impl->find = NDIlib_find_create_v2(&fc);
                }
                if (m_impl->find) {
                    NDIlib_find_wait_for_sources(m_impl->find, 500);
                    uint32_t n = 0; const NDIlib_source_t* list = NDIlib_find_get_current_sources(m_impl->find, &n);
                    for (uint32_t i = 0; i < n && !found; ++i) {
                        const QString nm = QString::fromUtf8(list[i].p_ndi_name);
                        if (nm == wantName || (!wantName.isEmpty() && nm.contains(wantName, Qt::CaseInsensitive))) {
                            nameUtf8 = nm.toUtf8(); urlUtf8 = QByteArray(list[i].p_url_address ? list[i].p_url_address : "");
                            src.p_ndi_name = nameUtf8.constData(); src.p_url_address = urlUtf8.isEmpty() ? nullptr : urlUtf8.constData(); found = true;
                        }
                    }
                }
            }
            if (!found) {
                if (m_legacyLock) {                     // PLAYPLAY_LIVESRC_LEGACY_LOCK=1 (A/B runs only): the pre-PE-03 code that
                    QMutexLocker l(&m_mutex);           // slept holding the lock, stalling video()/status() on the clock thread
                    m_statusText = "waiting for source"; QThread::msleep(200); continue;
                }
                setStatusText("waiting for source");
                waitForStop(200);                       // no lock held; close() wakes it
                continue;
            }
            NDIlib_recv_connect(m_impl->recv, &src);
            connected = true;
            const QString connName = QString::fromUtf8(nameUtf8);
            { QMutexLocker l(&m_mutex); m_impl->connectedName = connName; m_statusText = "connecting"; }
            qInfo().noquote() << "[NdiSource] connecting to" << connName;
        }
        NDIlib_video_frame_v2_t v; NDIlib_audio_frame_v3_t a;
        const NDIlib_frame_type_e t = NDIlib_recv_capture_v3(m_impl->recv, &v, &a, nullptr, 100);
        switch (t) {
        case NDIlib_frame_type_video: {
            if (v.p_data && v.xres > 0 && v.yres > 0) {
                if (!m_pool || m_pool->width() != v.xres || m_pool->height() != v.yres) m_pool = std::make_unique<FramePool>(v.xres, v.yres, kMixerPixelFormat);
                FramePtr f = m_pool->get();
                if (f) {
                    const int rowBytes = v.xres * 4;
                    for (int y = 0; y < v.yres; ++y) memcpy(f->data[0] + size_t(y) * f->linesize[0], v.p_data + size_t(y) * v.line_stride_in_bytes, size_t(rowBytes));
                    if (v.FourCC == NDIlib_FourCC_video_type_BGRX) {   // force opaque alpha
                        for (int y = 0; y < v.yres; ++y) { uint8_t* row = f->data[0] + size_t(y) * f->linesize[0]; for (int x = 3; x < rowBytes; x += 4) row[x] = 0xFF; }
                    }
                    publishVideo(f);
                }
                lastFrameMs = QDateTime::currentMSecsSinceEpoch();
                if (!m_connected.load()) { setConnected(true); setStatusText(QString("%1x%2 %3/%4").arg(v.xres).arg(v.yres).arg(v.frame_rate_N).arg(v.frame_rate_D)); }
            }
            NDIlib_recv_free_video_v2(m_impl->recv, &v);
            break;
        }
        case NDIlib_frame_type_audio: {
            if (a.p_data && a.no_samples > 0 && a.no_channels > 0 && a.no_channels <= 64 && a.sample_rate > 0 && a.FourCC == NDIlib_FourCC_audio_type_FLTP && qint64(a.channel_stride_in_bytes) >= qint64(a.no_samples) * qint64(sizeof(float))) {
                std::vector<const float*> planes(size_t(a.no_channels));
                for (int c = 0; c < a.no_channels; ++c) planes[size_t(c)] = reinterpret_cast<const float*>(a.p_data + size_t(c) * a.channel_stride_in_bytes);
                float peak = 0;
                for (int c = 0; c < a.no_channels; ++c) for (int i = 0; i < a.no_samples; ++i) peak = qMax(peak, std::abs(planes[size_t(c)][i]));
                m_audioFramesIn++; m_audioSamplesIn += a.no_samples; m_lastAudioMs = QDateTime::currentMSecsSinceEpoch();
                m_audioInputRate = a.sample_rate; m_audioInputChannels = a.no_channels; m_audioInputPeak = peak;
                publishAudio(planes.data(), true, a.no_channels, a.sample_rate, a.no_samples);
            } else m_audioInvalid++;
            NDIlib_recv_free_audio_v3(m_impl->recv, &a);
            break;
        }
        case NDIlib_frame_type_status_change:
        case NDIlib_frame_type_none:
        default:
            if (connected && lastFrameMs && QDateTime::currentMSecsSinceEpoch() - lastFrameMs > 3000 && m_connected.load()) {
                setConnected(false); setStatusText("no frames (sender stopped?)");
            }
            break;
        }
    }
#endif
}

// ===========================================================================
// DekTecSdiInSource
// ===========================================================================

struct DekTecSdiInSource::Hw {
#ifdef HAS_DEKTEC
    DtDevice device;
    DtMxProcess matrix;
    bool deviceAttached = false, matrixStarted = false;
#endif
};

#ifdef HAS_DEKTEC
namespace {
QString dtInErr(DTAPI_RESULT r) { return QString::fromLatin1(DtapiResult2Str(r)); }
int sdiInStdFromName(const QString& s0) {
    const QString s = s0.toLower();
    if (s == "1080p29.97") return DTAPI_VIDSTD_1080P29_97;  if (s == "1080p30") return DTAPI_VIDSTD_1080P30;
    if (s == "1080p25") return DTAPI_VIDSTD_1080P25;        if (s == "1080p24") return DTAPI_VIDSTD_1080P24;
    if (s == "1080p23.98") return DTAPI_VIDSTD_1080P23_98;  if (s == "1080i59.94") return DTAPI_VIDSTD_1080I59_94;
    if (s == "1080i60") return DTAPI_VIDSTD_1080I60;        if (s == "1080i50") return DTAPI_VIDSTD_1080I50;
    if (s == "720p59.94") return DTAPI_VIDSTD_720P59_94;    if (s == "720p60") return DTAPI_VIDSTD_720P60;
    if (s == "720p50") return DTAPI_VIDSTD_720P50;          if (s == "525i59.94" || s == "ntsc") return DTAPI_VIDSTD_525I59_94;
    if (s == "625i50" || s == "pal") return DTAPI_VIDSTD_625I50;
    return -1;
}
int sdiInStdAuto(const OutputFormat& f, bool* interlaced) {
    *interlaced = false;
    const double fps = double(f.rate.num) / f.rate.den;
    const bool ntsc = std::fabs(fps - 29.97) < 0.05 || std::fabs(fps - 59.94) < 0.05;
    if (f.height >= 1080) { if (std::fabs(fps - 25) < 0.05) return DTAPI_VIDSTD_1080P25; if (std::fabs(fps - 50) < 0.05) { *interlaced = true; return DTAPI_VIDSTD_1080I50; }
                            if (std::fabs(fps - 59.94) < 0.05) { *interlaced = true; return DTAPI_VIDSTD_1080I59_94; } return ntsc ? DTAPI_VIDSTD_1080P29_97 : DTAPI_VIDSTD_1080P30; }
    if (f.height >= 720) return ntsc ? DTAPI_VIDSTD_720P59_94 : (std::fabs(fps - 50) < 0.05 ? DTAPI_VIDSTD_720P50 : DTAPI_VIDSTD_720P60);
    *interlaced = true; return ntsc ? DTAPI_VIDSTD_525I59_94 : DTAPI_VIDSTD_625I50;
}
void sdiStdGeometry(int vidStd, int* w, int* h, bool* interlaced) {
    *interlaced = (vidStd == DTAPI_VIDSTD_1080I59_94 || vidStd == DTAPI_VIDSTD_1080I60 || vidStd == DTAPI_VIDSTD_1080I50 || vidStd == DTAPI_VIDSTD_525I59_94 || vidStd == DTAPI_VIDSTD_625I50);
    if (vidStd == DTAPI_VIDSTD_525I59_94) { *w = 720; *h = 486; }
    else if (vidStd == DTAPI_VIDSTD_625I50) { *w = 720; *h = 576; }
    else if (vidStd == DTAPI_VIDSTD_720P59_94 || vidStd == DTAPI_VIDSTD_720P60 || vidStd == DTAPI_VIDSTD_720P50) { *w = 1280; *h = 720; }
    else { *w = 1920; *h = 1080; }
}
}
#endif

DekTecSdiInSource::DekTecSdiInSource(const QJsonObject& spec, QObject* parent) : LiveSourceBase(parent), m_hw(new Hw), m_spec(spec) {
    m_fillRaster = spec.value("fillRaster").toBool(true);
    m_audioChannels = qBound(0, spec.value("audioChannels").toInt(2), 16);
    m_wantAudio = m_audioChannels > 0;
}

DekTecSdiInSource::~DekTecSdiInSource() { close(); delete m_hw; if (m_sws) sws_freeContext(m_sws); }

bool DekTecSdiInSource::isAvailable() {
#ifdef HAS_DEKTEC
    return true;
#else
    return false;
#endif
}

bool DekTecSdiInSource::open(const OutputFormat& fmt) {
    if (m_open) return true;
    m_fmt = fmt;
    resetAudio();
#ifdef HAS_DEKTEC
    const QString stdName = m_spec.value("videoStd").toString("auto");
    m_vidStd = stdName == "auto" ? sdiInStdAuto(fmt, &m_interlaced) : sdiInStdFromName(stdName);
    if (m_vidStd < 0) { setError("unknown videoStd " + stdName); return false; }
    sdiStdGeometry(m_vidStd, &m_inWidth, &m_inHeight, &m_interlaced);
    const int port = m_spec.value("port").toInt(1);
    const int type = m_spec.value("deviceType").toInt(2154), index = m_spec.value("deviceIndex").toInt(0);
    DTAPI_RESULT r;
    if (m_spec.contains("serial")) r = m_hw->device.AttachToSerial(m_spec["serial"].toString().toLongLong());
    else r = m_hw->device.AttachToType(type, index);
    if (r != DTAPI_OK) { setError(QString("DtDevice attach (type %1 #%2): %3").arg(type).arg(index).arg(dtInErr(r))); return false; }
    m_hw->deviceAttached = true;
    if (m_spec.value("configurePort").toBool(true)) {
        int v = 0, sub = 0;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IODIR, v, sub) == DTAPI_OK && v != DTAPI_IOCONFIG_INPUT) {
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_INPUT);
            qInfo() << "[DekTec SDI-in] port" << port << "set to input:" << dtInErr(r);
        }
        const int wantStd = (m_inHeight >= 720) ? DTAPI_IOCONFIG_HDSDI : DTAPI_IOCONFIG_SDI;
        if (m_hw->device.GetIoConfig(port, DTAPI_IOCONFIG_IOSTD, v, sub) == DTAPI_OK && (v != wantStd || sub != m_vidStd)) {
            r = m_hw->device.SetIoConfig(port, DTAPI_IOCONFIG_IOSTD, wantStd, m_vidStd);
            qInfo() << "[DekTec SDI-in] port" << port << "std" << m_vidStd << ":" << dtInErr(r);
        }
    }
    DtMxPort mxPort(&m_hw->device, port);
    if ((r = m_hw->matrix.AttachRowToInput(0, mxPort, true)) != DTAPI_OK) { setError(QString("AttachRowToInput port %1: %2").arg(port).arg(dtInErr(r))); close(); return false; }
    DtMxRowConfig cfg;
    cfg.m_Enable = true; cfg.m_RowSize = 1; cfg.m_pOpaq = this;
    cfg.m_RawDataEnable = false;
    cfg.m_VideoEnable = true;
    cfg.m_Video.m_StartLine1 = 1; cfg.m_Video.m_NumLines1 = -1;
    cfg.m_Video.m_StartLine2 = 1; cfg.m_Video.m_NumLines2 = -1;
    cfg.m_Video.m_Scaling = DTAPI_SCALING_OFF;
    cfg.m_Video.m_LineAlignment = 16; cfg.m_Video.m_BufAlignment = -1;
    cfg.m_Video.m_PixelFormat = DT_PXFMT_UYVY422_8B;
    cfg.m_Video.m_UserBuffer = false;
    cfg.m_Video.m_WeaveInterlaced = true;
    cfg.m_Video.m_UseZeroCopyMode = false;
    cfg.m_AudioEnable = m_wantAudio;
    cfg.m_AudioDef.m_DeEmbed = true;
    cfg.m_AudioDef.m_OutputMode = DT_OUTPUT_MODE_DROP;
    cfg.m_AudioDef.m_Format = DT_AUDIO_SAMPLE_PCM;
    for (int c = 0; c < m_audioChannels; ++c) { DtMxAudioConfig ac = cfg.m_AudioDef; ac.m_Index = c; cfg.m_Audio.push_back(ac); }
    cfg.m_AuxDataEnable = false;
    if ((r = m_hw->matrix.SetRowConfig(0, cfg)) != DTAPI_OK) { setError("SetRowConfig: " + dtInErr(r)); close(); return false; }
    if ((r = m_hw->matrix.SetVidStd(0, DtVideoStandard(m_vidStd))) != DTAPI_OK) { setError(QString("SetVidStd %1: %2").arg(m_vidStd).arg(dtInErr(r))); close(); return false; }
    if ((r = m_hw->matrix.AddMatrixCbFunc(reinterpret_cast<void(*)(DtMxData*, void*)>(&DekTecSdiInSource::matrixCallback), this)) != DTAPI_OK) { setError("AddMatrixCbFunc: " + dtInErr(r)); close(); return false; }
    if ((r = m_hw->matrix.Start()) != DTAPI_OK) { setError("DtMxProcess::Start: " + dtInErr(r)); close(); return false; }
    m_hw->matrixStarted = true;
    m_pool = std::make_unique<FramePool>(m_inWidth, m_inHeight, kMixerPixelFormat);
    m_sws = sws_getContext(m_inWidth, m_inHeight, AV_PIX_FMT_UYVY422, m_inWidth, m_inHeight, AV_PIX_FMT_BGRA, SWS_POINT, nullptr, nullptr, nullptr);
    m_pcm.assign(size_t(m_audioChannels) * 2048, 0.0f);
    m_pcmPlanes.resize(size_t(m_audioChannels));
    for (int c = 0; c < m_audioChannels; ++c) m_pcmPlanes[size_t(c)] = m_pcm.data() + size_t(c) * 2048;
    m_open = true;
    qInfo().noquote() << QString("[DekTec SDI-in] port %1 std %2 (%3x%4%5) audio ch %6").arg(port).arg(m_vidStd).arg(m_inWidth).arg(m_inHeight).arg(m_interlaced ? "i" : "p").arg(m_audioChannels);
    return true;
#else
    setError("DekTec support not compiled in");
    return false;
#endif
}

void DekTecSdiInSource::close() {
#ifdef HAS_DEKTEC
    if (m_hw->matrixStarted) { m_hw->matrix.Stop(); m_hw->matrixStarted = false; }
    m_hw->matrix.Reset();
    if (m_hw->deviceAttached) { m_hw->device.Detach(); m_hw->deviceAttached = false; }
#endif
    clearVideo();
    m_open = false;
    setConnected(false);
}

void DekTecSdiInSource::matrixCallback(void* data, void* opaque) {
    static_cast<DekTecSdiInSource*>(opaque)->onMatrixFrame(data);
}

void DekTecSdiInSource::onMatrixFrame(void* dtMxData) {
#ifdef HAS_DEKTEC
    DtMxData* d = static_cast<DtMxData*>(dtMxData);
    if (!m_open || !d || d->m_Rows.size() == 0) return;
    DtMxFrame* f = d->m_Rows[0].m_CurFrame;
    if (!f || f->m_Status == DT_FRMSTATUS_DISABLED) return;
    if (f->m_Status == DT_FRMSTATUS_NO_SIGNAL) { m_noSignal++; if (m_connected.load()) { setConnected(false); setStatusText("no signal"); } return; }
    if (f->m_Status == DT_FRMSTATUS_WRONG_VIDSTD) { m_wrongStd++; if (m_connected.load()) { setConnected(false); setStatusText("wrong video standard at input"); } return; }
    if (f->m_Status != DT_FRMSTATUS_OK && f->m_Status != DT_FRMSTATUS_DUPLICATE) return;

    if (f->m_VideoValid && m_pool && m_sws) {
        const DtMxVideoBuf& vb = (m_interlaced && f->m_VideoWeavedValid) ? f->m_VideoWeaved : f->m_Video[0];
        const DtMxVideoPlaneBuf& plane = vb.m_Planes[0];
        if (plane.m_pBuf && plane.m_NumLines > 0) {
            FramePtr out = m_pool->get();
            if (out) {
                const uint8_t* src[1] = { plane.m_pBuf };
                const int srcStride[1] = { plane.m_Stride };
                uint8_t* dst[1] = { out->data[0] };
                const int dstStride[1] = { out->linesize[0] };
                sws_scale(m_sws, src, srcStride, 0, qMin(m_inHeight, plane.m_NumLines), dst, dstStride);
                publishVideo(out);
            }
        }
        if (!m_connected.load()) { setConnected(true); setStatusText(QString("%1x%2%3").arg(m_inWidth).arg(m_inHeight).arg(m_interlaced ? "i" : "p")); }
    }
    if (m_wantAudio && f->m_AudioValid) {
        const int nch = qMin(int(f->m_Audio.m_Channels.size()), m_audioChannels);
        int n = 0;
        for (int c = 0; c < nch; ++c) {
            const DtMxAudioChannel& ch = f->m_Audio.m_Channels[size_t(c)];
            const int k = qMin(ch.m_NumValidSamples, 2048);
            if (c == 0) n = k;
            float* dst = m_pcm.data() + size_t(c) * 2048;
            for (int i = 0; i < k; ++i) dst[i] = float(int32_t(ch.m_pBuf[i])) / 2147483648.0f;   // PCM left-justified in 32 bits
            for (int i = k; i < n; ++i) dst[i] = 0.0f;
        }
        if (n > 0) publishAudio(m_pcmPlanes.data(), true, nch, 48000, n);
    }
#else
    Q_UNUSED(dtMxData);
#endif
}

QJsonObject DekTecSdiInSource::status() const {
    QJsonObject o = LiveSourceBase::status();
    o["port"] = m_spec.value("port").toInt(1); o["videoStd"] = m_vidStd;
    o["noSignalFrames"] = double(m_noSignal.load()); o["wrongStdFrames"] = double(m_wrongStd.load());
    return o;
}

// ===========================================================================
// HtmlSource: HTML/CSS/JS overlay rendered by a headless Chromium-family browser (browserhost.*)
// ===========================================================================

#include "browserhost.h"
#include <QDir>
#include <QUrl>
#include <QUrlQuery>

/**
 * spec: { type:"html", url | path, width, height, fps, nth, fields:{...}, reloadOnMiss:true, firstFrameTimeoutMs:8000,
 *         transparent:true, label }
 * update(): { url } re-take | { fields:{name:value} } live text | { animateOut|animateIn|replay:true } | { eval:"js" }
 * (legacy: data -> fields, js -> eval, action:"stop" -> animateOut). Every call is non-blocking (clock thread).
 */
class HtmlSource : public LayerSource {
public:
    explicit HtmlSource(const QJsonObject& spec, QObject* parent = nullptr) : LayerSource(parent), m_spec(spec) {}
    ~HtmlSource() override { close(); }
    QString type() const override { return "html"; }

    bool open(const OutputFormat& fmt) override {
        if (m_page) return true;
        m_fmt = fmt;
        m_size = QSize(m_spec.value("width").toInt(fmt.width), m_spec.value("height").toInt(fmt.height));
        QString url = normalizeUrl(m_spec.value("url").toString(m_spec.value("path").toString()));
        if (url.isEmpty()) { setError("html source needs a url or path"); return false; }
        QJsonObject fields = m_spec.value("fields").toObject();
        if (fields.isEmpty() && m_spec.value("data").isObject()) fields = m_spec.value("data").toObject();
        if (!fields.isEmpty()) url = withQuery(url, fields);
        HtmlPageOptions o;
        o.url = url; o.width = m_size.width(); o.height = m_size.height();
        o.transparent = m_spec.value("transparent").toBool(true);
        o.everyNthFrame = m_spec.value("nth").toInt(qEnvironmentVariableIntValue("PLAYPLAY_HTML_NTH_FRAME"));
        o.maxFps = m_spec.value("fps").toDouble(double(fmt.rate.num) / fmt.rate.den);
        o.fields = fields;
        o.label = m_spec.value("label").toString(m_spec.value("role").toString("html"));
        o.premultiplied = m_premul;              // PE-08: the page decodes straight into what deliversPremultiplied() says
        o.pngPremultiplied = m_pngPremul;
        m_reloadOnMiss = m_spec.value("reloadOnMiss").toBool(true);
        QString err;
        m_page = BrowserHost::instance()->createPage(o, &err);
        if (!m_page) { setError(err); return false; }
        int timeout = m_spec.value("firstFrameTimeoutMs").toInt(0);
        if (timeout <= 0) timeout = qEnvironmentVariableIntValue("PLAYPLAY_HTML_FIRST_FRAME_MS");
        if (timeout <= 0) timeout = 8000;
        if (!m_page->waitForFirstFrame(timeout, &err)) { setError("html overlay: " + err); m_page->close(); m_page.reset(); return false; }
        const QSize fs = m_page->frameSize();
        if (!fs.isEmpty()) m_size = fs;
        m_open = true;
        return true;
    }
    void close() override { m_open = false; if (m_page) { m_page->close(); m_page.reset(); } }
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64, double) override { return m_page ? m_page->latestFrame() : nullptr; }
    QSize nativeSize() const override { return m_size == QSize(m_fmt.width, m_fmt.height) ? QSize() : m_size; }
    bool deliversPremultiplied() const override { return m_premul; }

    void update(const QJsonObject& p) override {
        if (!m_page) return;
        if (p.contains("url")) m_page->navigate(normalizeUrl(p.value("url").toString()));
        QJsonObject fields = p.value("fields").toObject();
        if (fields.isEmpty() && p.value("data").isObject()) fields = p.value("data").toObject();
        if (!fields.isEmpty()) m_page->setFields(fields, p.value("reloadOnMiss").toBool(m_reloadOnMiss));
        const QString js = p.value("eval").toString(p.value("js").toString());
        if (!js.isEmpty()) m_page->evaluate(js);
        if (p.value("animateOut").toBool() || p.value("action").toString() == "stop") m_page->animate("out");
        if (p.value("animateIn").toBool()) m_page->animate("in");
        if (p.value("replay").toBool()) m_page->animate("replay");
    }

    QJsonObject status() const override {
        QJsonObject o = LayerSource::status();
        if (m_page) { const QJsonObject ps = m_page->status(); for (auto it = ps.begin(); it != ps.end(); ++it) o[it.key()] = it.value(); }
        o["browserPath"] = BrowserHost::instance()->browserPath();
        o["browserVersion"] = BrowserHost::instance()->browserVersion();
        o["browser"] = BrowserHost::instance()->status();
        return o;
    }

private:
    static QString normalizeUrl(QString u) {
        u = u.trimmed();
        if (u.isEmpty()) return u;
        if (u.contains("://") || u.startsWith("data:") || u.startsWith("about:")) return u;
        return QUrl::fromLocalFile(QDir::fromNativeSeparators(u)).toString();
    }
    static QString withQuery(const QString& url, const QJsonObject& fields) {
        QUrl u(url);
        if (!u.isValid() || u.scheme() == "data" || u.scheme() == "about") return url;
        QUrlQuery q(u);
        for (auto it = fields.begin(); it != fields.end(); ++it) { q.removeAllQueryItems(it.key()); q.addQueryItem(it.key(), it.value().isString() ? it.value().toString() : it.value().toVariant().toString()); }
        u.setQuery(q);
        return u.toString(QUrl::FullyEncoded);
    }
    QJsonObject m_spec; OutputFormat m_fmt; QSize m_size; bool m_open = false, m_reloadOnMiss = true;
    const bool m_premul = premultipliedFromEnv();   // PE-08 (PLAYPLAY_PREMUL=1 when created), fixed for the page's life
    // PE-08, opt-in: the browser's screencast PNG holds premultiplied colours (see HtmlPageOptions::pngPremultiplied).
    // A look change for translucent HTML (it gets the colours the page asked for), so it has its own switch.
    const bool m_pngPremul = [] { const QByteArray v = qgetenv("PLAYPLAY_HTML_PNG_PREMULTIPLIED").trimmed().toLower(); return v == "1" || v == "on" || v == "true" || v == "yes"; }();
    std::shared_ptr<HtmlPage> m_page;
};

std::unique_ptr<LayerSource> createHtmlSource(const QJsonObject& spec, QString* error) {
    Q_UNUSED(error);
    return std::make_unique<HtmlSource>(spec);
}

#include "livesources.moc"
