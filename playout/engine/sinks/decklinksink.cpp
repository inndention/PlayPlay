#include "decklinksink.h"
#include <QDebug>
#include <QDateTime>
#include <QThread>
#include <QElapsedTimer>
#include <cstring>

#ifdef HAS_DECKLINK
#include <windows.h>
#include <objbase.h>
#include "DeckLinkAPI_h.h"
#endif

// ---------------------------------------------------------------------------
// Hardware wrapper
// ---------------------------------------------------------------------------

#ifdef HAS_DECKLINK
namespace {

template <class T> void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

QString bstrToQString(BSTR s) {
    if (!s) return {};
    QString r = QString::fromWCharArray(s);
    SysFreeString(s);
    return r;
}

QString hrText(HRESULT hr) { return QString("0x%1").arg(quint32(hr), 8, 16, QChar('0')); }

/// Initialises COM for the calling thread when it is not already, undoing it on scope exit.
struct ComScope {
    bool owned = false;
    ComScope() { const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); owned = (hr == S_OK || hr == S_FALSE); }
    ~ComScope() { if (owned) CoUninitialize(); }
};

IDeckLinkIterator* createIterator() {
    IDeckLinkIterator* it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void**>(&it))))
        return nullptr;
    return it;
}

/// Completion callback object: forwards to the sink, ref-counted the COM way.
class OutputCallback : public IDeckLinkVideoOutputCallback {
public:
    explicit OutputCallback(DeckLinkSink* s) : m_sink(s) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDeckLinkVideoOutputCallback) { *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --m_refs; if (r == 0) delete this; return r; }
    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* f, BMDOutputFrameCompletionResult result) override {
        if (DeckLinkSink* s = m_sink.load()) s->onFrameCompleted(f, int(result));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override {
        if (DeckLinkSink* s = m_sink.load()) s->onPlaybackStopped();
        return S_OK;
    }
    void detach() { m_sink.store(nullptr); }
private:
    std::atomic<DeckLinkSink*> m_sink;
    std::atomic<ULONG> m_refs{1};
};

} // namespace
#endif

struct DeckLinkSink::Hw {
#ifdef HAS_DECKLINK
    ComScope* com = nullptr;
    IDeckLink* device = nullptr;
    IDeckLinkOutput* output = nullptr;
    IDeckLinkKeyer* keyer = nullptr;
    OutputCallback* callback = nullptr;
    std::vector<IDeckLinkMutableVideoFrame*> frames;   // all frames we created
    std::vector<IDeckLinkMutableVideoFrame*> freeFrames;
    QString deviceName, modeName;
    BMDDisplayMode mode = bmdModeUnknown;
    bool videoEnabled = false, audioEnabled = false, playing = false, keyerOn = false;
    std::atomic<bool> stopped{false};
#endif
};

DeckLinkSink::DeckLinkSink(const QString& name, const QJsonObject& options, QObject* parent)
    : BasebandSink(name, parent), m_opts(options) {}

DeckLinkSink::~DeckLinkSink() { stop(); }

bool DeckLinkSink::isAvailable() {
#ifdef HAS_DECKLINK
    ComScope com;
    IDeckLinkIterator* it = createIterator();
    if (!it) return false;
    it->Release();
    return true;
#else
    return false;
#endif
}

QJsonArray DeckLinkSink::listDevices() {
    QJsonArray out;
#ifdef HAS_DECKLINK
    ComScope com;
    IDeckLinkIterator* it = createIterator();
    if (!it) return out;
    IDeckLink* dl = nullptr;
    for (int idx = 0; it->Next(&dl) == S_OK; ++idx) {
        QJsonObject d; d["index"] = idx;
        BSTR s = nullptr;
        if (dl->GetDisplayName(&s) == S_OK) d["name"] = bstrToQString(s);
        if (dl->GetModelName(&s) == S_OK) d["model"] = bstrToQString(s);
        IDeckLinkOutput* o = nullptr;
        if (dl->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&o)) == S_OK) {
            QJsonArray modes;
            IDeckLinkDisplayModeIterator* mi = nullptr;
            if (o->GetDisplayModeIterator(&mi) == S_OK) {
                IDeckLinkDisplayMode* m = nullptr;
                while (mi->Next(&m) == S_OK) { BSTR n = nullptr; if (m->GetName(&n) == S_OK) modes.append(bstrToQString(n)); m->Release(); }
                mi->Release();
            }
            d["outputs"] = modes;
            o->Release();
        }
        IDeckLinkProfileAttributes* a = nullptr;
        if (dl->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&a)) == S_OK) {
            BOOL b = FALSE; LONGLONG v = 0;
            if (a->GetFlag(BMDDeckLinkSupportsInternalKeying, &b) == S_OK) d["internalKeying"] = bool(b);
            if (a->GetFlag(BMDDeckLinkSupportsExternalKeying, &b) == S_OK) d["externalKeying"] = bool(b);
            if (a->GetInt(BMDDeckLinkMaximumAudioChannels, &v) == S_OK) d["maxAudioChannels"] = int(v);
            a->Release();
        }
        out.append(d);
        dl->Release();
    }
    it->Release();
#endif
    return out;
}

int DeckLinkSink::samplesForFrame(qint64 n) const {
    // Exact 48 kHz cadence: samples up to frame n+1 minus samples up to frame n.
    const qint64 perNum = 48000LL * m_frameDuration;
    return int(((n + 1) * perNum) / m_timeScale - (n * perNum) / m_timeScale);
}

// ---------------------------------------------------------------------------
// Start / stop
// ---------------------------------------------------------------------------

bool DeckLinkSink::start(const Format& fmt) {
    if (m_running.load()) return true;
    m_fmt = fmt;
    m_srcChannels = qMax(1, fmt.channels);
    const int want = m_opts.value("audioChannels").toInt(qMax(2, fmt.channels));
    m_audioChannels = want <= 2 ? 2 : (want <= 8 ? 8 : 16);
    m_frameIndex = 0; m_audioSamples = 0;
    m_underruns = m_overruns = m_late = m_dropped = m_framesOut = 0;
    m_stopping.store(false);
    QString err;
    if (!openHardware(&err)) {
        closeHardware();
        setError(err);
        return false;
    }
    m_running.store(true);
    return true;
}

void DeckLinkSink::stop() {
    if (!m_hw && !m_running.load()) return;
    m_stopping.store(true);
    m_running.store(false);
    closeHardware();
    QMutexLocker l(&m_qmutex);
    m_video.clear(); m_lastVideo.reset();
    if (m_afifo) { av_audio_fifo_free(m_afifo); m_afifo = nullptr; }
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    QMutexLocker s(&m_statMutex); m_stats.connected = false;
}

bool DeckLinkSink::openHardware(QString* err) {
#ifndef HAS_DECKLINK
    *err = "DeckLink support not built (DeckLink SDK not found at configure time)";
    return false;
#else
    m_hw = new Hw;
    m_hw->com = new ComScope;
    IDeckLinkIterator* it = createIterator();
    if (!it) { *err = "DeckLink driver not installed (Desktop Video): no DeckLink COM iterator"; return false; }

    const QString wantName = m_opts.value("deviceName").toString().trimmed();
    const int wantIndex = m_opts.value("deviceIndex").toInt(0);
    IDeckLink* dl = nullptr; int idx = 0; int found = 0;
    while (it->Next(&dl) == S_OK) {
        BSTR s = nullptr; QString name;
        if (dl->GetDisplayName(&s) == S_OK) name = bstrToQString(s);
        ++found;
        const bool match = wantName.isEmpty() ? (idx == wantIndex) : name.contains(wantName, Qt::CaseInsensitive);
        if (match && !m_hw->device) { m_hw->device = dl; m_hw->deviceName = name; }
        else dl->Release();
        ++idx;
    }
    it->Release();
    if (!m_hw->device) {
        *err = found == 0 ? QString("no DeckLink device found")
                          : QString("DeckLink device %1 not found (%2 installed)").arg(wantName.isEmpty() ? QString::number(wantIndex) : "'" + wantName + "'").arg(found);
        return false;
    }
    if (m_hw->device->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&m_hw->output)) != S_OK) {
        *err = m_hw->deviceName + " has no output"; return false;
    }

    // Pick the display mode: by name, or the one matching our raster and frame rate.
    const QString modeOpt = m_opts.value("videoMode").toString("auto").trimmed();
    const bool isHd1080 = m_fmt.height == 1080 && (m_fmt.rate.nominalFps() == 25 || m_fmt.rate.nominalFps() == 30);
    const bool preferInterlaced = m_opts.value("interlaced").toBool(isHd1080);
    IDeckLinkDisplayModeIterator* mi = nullptr;
    if (m_hw->output->GetDisplayModeIterator(&mi) != S_OK) { *err = "cannot list DeckLink display modes"; return false; }
    IDeckLinkDisplayMode* m = nullptr;
    BMDDisplayMode best = bmdModeUnknown, fallback = bmdModeUnknown; QString bestName, fallbackName;
    BMDTimeValue bestDur = 0, fbDur = 0; BMDTimeScale bestScale = 0, fbScale = 0;
    while (mi->Next(&m) == S_OK) {
        BSTR n = nullptr; QString mname; if (m->GetName(&n) == S_OK) mname = bstrToQString(n);
        BMDTimeValue dur = 0; BMDTimeScale scale = 0; m->GetFrameRate(&dur, &scale);
        const bool interlaced = m->GetFieldDominance() == bmdLowerFieldFirst || m->GetFieldDominance() == bmdUpperFieldFirst;
        if (modeOpt.compare("auto", Qt::CaseInsensitive) != 0) {
            if (mname.compare(modeOpt, Qt::CaseInsensitive) == 0) { best = m->GetDisplayMode(); bestName = mname; bestDur = dur; bestScale = scale; }
        } else if (m->GetWidth() == m_fmt.width && m->GetHeight() == m_fmt.height && scale > 0
                   && qint64(dur) * m_fmt.rate.num == qint64(scale) * m_fmt.rate.den) {
            if (interlaced == preferInterlaced && best == bmdModeUnknown) { best = m->GetDisplayMode(); bestName = mname; bestDur = dur; bestScale = scale; }
            else if (fallback == bmdModeUnknown) { fallback = m->GetDisplayMode(); fallbackName = mname; fbDur = dur; fbScale = scale; }
        }
        m->Release();
    }
    mi->Release();
    if (best == bmdModeUnknown && fallback != bmdModeUnknown) { best = fallback; bestName = fallbackName; bestDur = fbDur; bestScale = fbScale; }
    if (best == bmdModeUnknown) {
        *err = modeOpt.compare("auto", Qt::CaseInsensitive) == 0
            ? QString("%1 has no output mode for %2x%3 @ %4 fps").arg(m_hw->deviceName).arg(m_fmt.width).arg(m_fmt.height).arg(m_fmt.rate.fps(), 0, 'f', 2)
            : QString("%1 has no output mode named '%2'").arg(m_hw->deviceName, modeOpt);
        return false;
    }
    m_hw->mode = best; m_hw->modeName = bestName;
    m_frameDuration = bestDur; m_timeScale = bestScale;

    // BGRA first (keeps the key channel); 8-bit YUV when the card refuses BGRA for this mode.
    BOOL supported = FALSE; BMDDisplayMode actual = bmdModeUnknown;
    m_bgra = m_hw->output->DoesSupportVideoMode(bmdVideoConnectionUnspecified, best, bmdFormat8BitBGRA, bmdNoVideoOutputConversion,
                                               bmdSupportedVideoModeDefault, &actual, &supported) == S_OK && supported;
    if (!m_bgra) {
        supported = FALSE;
        if (m_hw->output->DoesSupportVideoMode(bmdVideoConnectionUnspecified, best, bmdFormat8BitYUV, bmdNoVideoOutputConversion,
                                               bmdSupportedVideoModeDefault, &actual, &supported) != S_OK || !supported) {
            *err = QString("%1 does not support output mode %2").arg(m_hw->deviceName, bestName); return false;
        }
        m_sws = sws_getContext(m_fmt.width, m_fmt.height, AV_PIX_FMT_BGRA, m_fmt.width, m_fmt.height, AV_PIX_FMT_UYVY422,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) { *err = "cannot create BGRA->UYVY converter"; return false; }
    }

    HRESULT hr = m_hw->output->EnableVideoOutput(best, bmdVideoOutputFlagDefault);
    if (hr != S_OK) { *err = QString("EnableVideoOutput(%1) failed %2 (device busy?)").arg(bestName, hrText(hr)); return false; }
    m_hw->videoEnabled = true;
    hr = m_hw->output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, unsigned(m_audioChannels), bmdAudioOutputStreamTimestamped);
    if (hr != S_OK) { *err = QString("EnableAudioOutput(%1 ch) failed %2").arg(m_audioChannels).arg(hrText(hr)); return false; }
    m_hw->audioEnabled = true;

    // Keyer
    const QString keyer = m_opts.value("keyer").toString("off").toLower();
    if (keyer == "internal" || keyer == "external") {
        if (m_hw->device->QueryInterface(IID_IDeckLinkKeyer, reinterpret_cast<void**>(&m_hw->keyer)) == S_OK) {
            if (m_hw->keyer->Enable(keyer == "external") == S_OK) {
                m_hw->keyer->SetLevel(static_cast<unsigned char>(qBound(0, m_opts.value("keyLevel").toInt(255), 255)));
                m_hw->keyerOn = true;
            } else qWarning().noquote() << "[DeckLink]" << m_name << "keyer" << keyer << "not supported on" << m_hw->deviceName;
        }
    }

    // Frame pool
    const int rowBytes = m_bgra ? m_fmt.width * 4 : m_fmt.width * 2;
    const BMDPixelFormat pf = m_bgra ? bmdFormat8BitBGRA : bmdFormat8BitYUV;
    const int preroll = qBound(2, m_opts.value("prerollFrames").toInt(3), 8);
    for (int i = 0; i < preroll + kMaxQueue + 2; ++i) {
        IDeckLinkMutableVideoFrame* f = nullptr;
        if (m_hw->output->CreateVideoFrame(m_fmt.width, m_fmt.height, rowBytes, pf, bmdFrameFlagDefault, &f) != S_OK) {
            *err = "CreateVideoFrame failed"; return false;
        }
        // Start black (BGRA zero = transparent black; YUV black is 0x10/0x80)
        IDeckLinkVideoBuffer* vb = nullptr;
        if (f->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&vb)) == S_OK) {
            void* bytes = nullptr;
            if (vb->StartAccess(bmdBufferAccessWrite) == S_OK) {
                if (vb->GetBytes(&bytes) == S_OK && bytes) {
                    if (m_bgra) std::memset(bytes, 0, size_t(rowBytes) * m_fmt.height);
                    else { auto* p = static_cast<uint32_t*>(bytes); const size_t n = size_t(rowBytes) * m_fmt.height / 4; for (size_t k = 0; k < n; ++k) p[k] = 0x10801080u; }
                }
                vb->EndAccess(bmdBufferAccessWrite);
            }
            vb->Release();
        }
        m_hw->frames.push_back(f); m_hw->freeFrames.push_back(f);
    }

    m_afifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, m_srcChannels, 48000);
    m_hw->callback = new OutputCallback(this);
    if (m_hw->output->SetScheduledFrameCompletionCallback(m_hw->callback) != S_OK) { *err = "SetScheduledFrameCompletionCallback failed"; return false; }

    {
        QMutexLocker l(&m_qmutex);
        for (int i = 0; i < preroll; ++i) if (!scheduleNext()) { *err = "could not preroll DeckLink output"; return false; }
    }
    hr = m_hw->output->StartScheduledPlayback(0, m_timeScale, 1.0);
    if (hr != S_OK) { *err = QString("StartScheduledPlayback failed %1").arg(hrText(hr)); return false; }
    m_hw->playing = true;

    {
        QMutexLocker l(&m_statMutex);
        m_stats.connected = true;
        m_stats.extra["device"] = m_hw->deviceName;
        m_stats.extra["videoMode"] = bestName;
        m_stats.extra["pixelFormat"] = m_bgra ? "BGRA" : "8-bit YUV";
        m_stats.extra["audioChannels"] = m_audioChannels;
        m_stats.extra["keyer"] = m_hw->keyerOn ? keyer : QString("off");
    }
    qInfo().noquote() << QString("[DeckLink] %1 on '%2' %3 %4, %5 ch audio%6").arg(m_name, m_hw->deviceName, bestName,
        m_bgra ? "BGRA" : "YUV").arg(m_audioChannels).arg(m_hw->keyerOn ? ", keyer " + keyer : QString());
    return true;
#endif
}

void DeckLinkSink::closeHardware() {
#ifdef HAS_DECKLINK
    if (!m_hw) return;
    if (m_hw->output) {
        if (m_hw->playing) {
            BMDTimeValue actual = 0;
            m_hw->output->StopScheduledPlayback(0, &actual, m_timeScale);
            QElapsedTimer t; t.start();
            while (!m_hw->stopped.load() && t.elapsed() < 500) QThread::msleep(5);
            m_hw->playing = false;
        }
        m_hw->output->SetScheduledFrameCompletionCallback(nullptr);
        if (m_hw->audioEnabled) m_hw->output->DisableAudioOutput();
        if (m_hw->videoEnabled) m_hw->output->DisableVideoOutput();
    }
    if (m_hw->callback) { m_hw->callback->detach(); m_hw->callback->Release(); m_hw->callback = nullptr; }
    if (m_hw->keyer) { if (m_hw->keyerOn) m_hw->keyer->Disable(); safeRelease(m_hw->keyer); }
    {
        QMutexLocker l(&m_qmutex);
        for (auto* f : m_hw->frames) f->Release();
        m_hw->frames.clear(); m_hw->freeFrames.clear();
    }
    safeRelease(m_hw->output);
    safeRelease(m_hw->device);
    delete m_hw->com;
    delete m_hw; m_hw = nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Engine side
// ---------------------------------------------------------------------------

void DeckLinkSink::pushFrame(const MediaFrame& frame) {
    if (!m_running.load()) return;
    QMutexLocker l(&m_qmutex);
    if (frame.video) {
        if (int(m_video.size()) >= kMaxQueue) { m_video.pop_front(); m_overruns++; }
        m_video.push_back(frame.video);
    }
    if (frame.audio && m_afifo && frame.audio->ch_layout.nb_channels == m_srcChannels) {
        const int maxSamples = 48000 / 4;   // bounded so audio cannot drift away from video
        if (av_audio_fifo_size(m_afifo) + frame.audio->nb_samples > maxSamples) av_audio_fifo_drain(m_afifo, frame.audio->nb_samples);
        av_audio_fifo_write(m_afifo, reinterpret_cast<void**>(frame.audio->data), frame.audio->nb_samples);
    }
    QMutexLocker s(&m_statMutex); m_stats.framesIn++;
}

// ---------------------------------------------------------------------------
// Card side (DeckLink callback thread)
// ---------------------------------------------------------------------------

void DeckLinkSink::onFrameCompleted(void* completed, int result) {
#ifdef HAS_DECKLINK
    if (result == bmdOutputFrameDisplayedLate) m_late++;
    else if (result == bmdOutputFrameDropped) m_dropped++;
    QMutexLocker l(&m_qmutex);
    if (!m_hw) return;
    auto* f = static_cast<IDeckLinkVideoFrame*>(completed);
    for (auto* mf : m_hw->frames) if (static_cast<IDeckLinkVideoFrame*>(mf) == f) { m_hw->freeFrames.push_back(mf); break; }
    if (m_stopping.load() || result == bmdOutputFrameFlushed) return;
    scheduleNext();
    l.unlock();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastStatMs >= 1000) { m_lastStatMs = now; publishStats(); }
#else
    Q_UNUSED(completed) Q_UNUSED(result)
#endif
}

void DeckLinkSink::onPlaybackStopped() {
#ifdef HAS_DECKLINK
    if (m_hw) m_hw->stopped.store(true);
#endif
}

bool DeckLinkSink::scheduleNext() {
#ifdef HAS_DECKLINK
    if (!m_hw || m_hw->freeFrames.empty()) return false;
    IDeckLinkMutableVideoFrame* f = m_hw->freeFrames.back(); m_hw->freeFrames.pop_back();

    FramePtr pic;
    const bool fresh = !m_video.empty();
    if (fresh) { pic = m_video.front(); m_video.pop_front(); m_lastVideo = pic; }
    else { pic = m_lastVideo; if (m_frameIndex > 0) m_underruns++; }

    if (pic && pic->width == m_fmt.width && pic->height == m_fmt.height) {
        IDeckLinkVideoBuffer* vb = nullptr;
        if (f->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&vb)) == S_OK) {
            void* bytes = nullptr;
            if (vb->StartAccess(bmdBufferAccessWrite) == S_OK) {
                if (vb->GetBytes(&bytes) == S_OK && bytes) {
                    const int rowBytes = int(f->GetRowBytes());
                    if (m_bgra) {
                        const int copy = qMin(rowBytes, m_fmt.width * 4);
                        for (int y = 0; y < m_fmt.height; ++y)
                            std::memcpy(static_cast<uint8_t*>(bytes) + size_t(y) * rowBytes, pic->data[0] + size_t(y) * pic->linesize[0], size_t(copy));
                    } else if (m_sws) {
                        const uint8_t* src[4] = { pic->data[0], nullptr, nullptr, nullptr };
                        const int srcStride[4] = { pic->linesize[0], 0, 0, 0 };
                        uint8_t* dst[4] = { static_cast<uint8_t*>(bytes), nullptr, nullptr, nullptr };
                        const int dstStride[4] = { rowBytes, 0, 0, 0 };
                        sws_scale(m_sws, src, srcStride, 0, m_fmt.height, dst, dstStride);
                    }
                }
                vb->EndAccess(bmdBufferAccessWrite);
            }
            vb->Release();
        }
    }

    const BMDTimeValue t = BMDTimeValue(m_frameIndex) * m_frameDuration;
    if (m_hw->output->ScheduleVideoFrame(f, t, m_frameDuration, m_timeScale) != S_OK) {
        m_hw->freeFrames.push_back(f);
        return false;
    }

    // This frame's audio: FLTP from the FIFO (silence when short), interleaved int32 for the card.
    const int n = samplesForFrame(m_frameIndex);
    m_audioScratch.assign(size_t(n) * m_srcChannels, 0.0f);
    m_audioPlanes.resize(size_t(m_srcChannels));
    for (int c = 0; c < m_srcChannels; ++c) m_audioPlanes[size_t(c)] = m_audioScratch.data() + size_t(c) * n;
    const int avail = (m_afifo && fresh) ? av_audio_fifo_size(m_afifo) : 0;
    const int take = qMin(avail, n);
    if (take > 0) av_audio_fifo_read(m_afifo, reinterpret_cast<void**>(m_audioPlanes.data()), take);
    m_interleaved.assign(size_t(n) * m_audioChannels, 0);
    const int chans = qMin(m_srcChannels, m_audioChannels);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < chans; ++c) {
            const float v = qBound(-1.0f, m_audioPlanes[size_t(c)][i], 1.0f);
            m_interleaved[size_t(i) * m_audioChannels + c] = int32_t(double(v) * 2147483647.0);
        }
    unsigned written = 0;
    m_hw->output->ScheduleAudioSamples(m_interleaved.data(), unsigned(n), BMDTimeValue(m_audioSamples), 48000, &written);
    m_audioSamples += n;
    m_frameIndex++;
    m_framesOut++;
    return true;
#else
    return false;
#endif
}

void DeckLinkSink::publishStats() {
#ifdef HAS_DECKLINK
    unsigned buffered = 0;
    if (m_hw && m_hw->output) m_hw->output->GetBufferedVideoFrameCount(&buffered);
    int queued = 0;
    { QMutexLocker l(&m_qmutex); queued = int(m_video.size()); }
    QMutexLocker s(&m_statMutex);
    m_stats.framesOut = m_framesOut;
    m_stats.dropped = m_overruns + m_dropped;
    m_stats.extra["underruns"] = double(m_underruns);
    m_stats.extra["overruns"] = double(m_overruns);
    m_stats.extra["late"] = double(m_late);
    m_stats.extra["cardDropped"] = double(m_dropped);
    m_stats.extra["bufferedFrames"] = int(buffered);
    m_stats.extra["queuedFrames"] = queued;
    m_stats.extra["framesOut"] = double(m_framesOut);
#endif
}
