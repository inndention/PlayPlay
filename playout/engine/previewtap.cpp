#include "previewtap.h"
#include "compositor/livesources.h"
#include <QImage>
#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDebug>
#include <QSet>
#include <cmath>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// ---------------------------------------------------------------------------
// PreviewEncoder
// ---------------------------------------------------------------------------

PreviewEncoder::~PreviewEncoder() { if (m_sws) sws_freeContext(m_sws); }

QSize PreviewEncoder::fit(int srcW, int srcH, int maxW, int maxH) {
    if (srcW <= 0 || srcH <= 0) return QSize(qMax(2, maxW & ~1), qMax(2, maxH & ~1));
    const double s = qMin(1.0, qMin(double(maxW) / srcW, double(maxH) / srcH));
    const int w = qMax(2, int(std::lround(srcW * s)) & ~1), h = qMax(2, int(std::lround(srcH * s)) & ~1);
    return QSize(w, h);
}

QByteArray PreviewEncoder::encode(const FramePtr& f, int maxW, int maxH, int quality, int* outW, int* outH) {
    if (!f || !f->data[0] || f->width <= 0 || f->height <= 0) return {};
    const QSize d = fit(f->width, f->height, maxW, maxH);
    const int fmt = f->format >= 0 ? f->format : int(AV_PIX_FMT_BGRA);
    if (!m_sws || m_srcW != f->width || m_srcH != f->height || m_srcFmt != fmt || m_dstW != d.width() || m_dstH != d.height()) {
        if (m_sws) sws_freeContext(m_sws);
        // SWS_AREA averages every source pixel into the thumbnail (a box filter): crisp text, no aliasing shimmer.
        m_sws = sws_getContext(f->width, f->height, AVPixelFormat(fmt), d.width(), d.height(), AV_PIX_FMT_BGRA, SWS_AREA, nullptr, nullptr, nullptr);
        m_srcW = f->width; m_srcH = f->height; m_srcFmt = fmt; m_dstW = d.width(); m_dstH = d.height();
    }
    QImage small(d.width(), d.height(), QImage::Format_RGB32);
    if (m_sws) {
        uint8_t* dst[4] = { small.bits(), nullptr, nullptr, nullptr };
        int dstStride[4] = { int(small.bytesPerLine()), 0, 0, 0 };
        sws_scale(m_sws, f->data, f->linesize, 0, f->height, dst, dstStride);
    } else {
        // No scaler for this pixel format: Qt's smooth scale on the BGRA view (slower, same look).
        const QImage src(f->data[0], f->width, f->height, f->linesize[0], QImage::Format_RGB32);
        small = src.scaled(d, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    small.save(&buf, "JPEG", qBound(10, quality, 95));
    if (outW) *outW = d.width();
    if (outH) *outH = d.height();
    return jpeg;
}

// ---------------------------------------------------------------------------
// PreviewTap
// ---------------------------------------------------------------------------

PreviewTap::Profile PreviewTap::Profile::fromJson(const QJsonObject& o, const Profile& fb) {
    Profile p = fb;
    p.width = qBound(64, o.value("width").toInt(fb.width), 1920);
    p.height = qBound(36, o.value("height").toInt(fb.height), 1080);
    p.fps = qBound(0.5, o.value("fps").toDouble(fb.fps), 60.0);
    p.quality = qBound(10, o.value("quality").toInt(fb.quality), 95);
    return p;
}

QJsonObject PreviewTap::Profile::toJson() const {
    QJsonObject o; o["width"] = width; o["height"] = height; o["fps"] = fps; o["quality"] = quality; return o;
}

PreviewTap::PreviewTap(QObject* parent) : QThread(parent) {
    m_boost.width = 640; m_boost.height = 360; m_boost.fps = 15.0; m_boost.quality = 80;
}

PreviewTap::~PreviewTap() {
    m_stop = true;
    { QMutexLocker l(&m_inMutex); m_inCond.wakeAll(); }
    wait(2000);
}

void PreviewTap::setChannelFps(double fps) { QMutexLocker l(&m_inMutex); m_channelFps = fps > 1.0 ? fps : 29.97; }
void PreviewTap::setBaseProfile(const Profile& p) { QMutexLocker l(&m_inMutex); m_base = p; }
void PreviewTap::setBoostProfile(const Profile& p) { QMutexLocker l(&m_inMutex); m_boost = p; }
void PreviewTap::boost(int seconds) { m_boostUntilMs = seconds > 0 ? nowMs() + qint64(qMin(seconds, 600)) * 1000 : 0; }
bool PreviewTap::boosted() const { return nowMs() < m_boostUntilMs.load(); }

void PreviewTap::configure(int width, int height, int everyNFrames, int jpegQuality) {
    QMutexLocker l(&m_inMutex);
    m_base.width = qMax(16, width); m_base.height = qMax(9, height);
    m_base.fps = m_channelFps / qMax(1, everyNFrames);
    m_base.quality = qBound(10, jpegQuality, 95);
}

void PreviewTap::setEnabled(bool on) {
    m_enabled = on;
    if (on && !isRunning()) { m_stop = false; start(QThread::LowestPriority); }
}

int PreviewTap::cadence(const Profile& p) const {
    return qMax(1, int(std::lround(m_channelFps / qMax(0.5, p.fps))));
}

QJsonObject PreviewTap::status() const {
    QMutexLocker l(&m_inMutex);
    QJsonObject o;
    o["enabled"] = m_enabled.load(); o["boosted"] = boosted();
    o["base"] = m_base.toJson(); o["boost"] = m_boost.toJson();
    o["encoded"] = double(m_encoded.load()); o["encodeMs"] = m_lastEncodeUs.load() / 1000.0;
    return o;
}

void PreviewTap::submit(const FramePtr& bgra, qint64 frameNumber) {
    if (!m_enabled.load() || !bgra) return;
    const bool hi = boosted();
    QMutexLocker l(&m_inMutex);
    const Profile& p = hi ? m_boost : m_base;
    // Cadence from the last submitted frame (not frame % n), so switching profiles never stalls a beat.
    if (frameNumber - m_lastSubmitFrame < cadence(p) && frameNumber >= m_lastSubmitFrame) return;
    m_lastSubmitFrame = frameNumber;
    m_pending = bgra;            // replaces any older pending frame
    m_pendingProfile = p;
    m_pendingFrame = frameNumber;
    m_inCond.wakeOne();
}

void PreviewTap::run() {
    PreviewEncoder enc;
    while (!m_stop.load()) {
        FramePtr f; qint64 n = 0; Profile p;
        {
            QMutexLocker l(&m_inMutex);
            while (!m_pending && !m_stop.load()) m_inCond.wait(&m_inMutex, 200);
            if (m_stop.load()) break;
            f = std::move(m_pending); m_pending.reset(); n = m_pendingFrame; p = m_pendingProfile;
        }
        if (!f) continue;
        QElapsedTimer t; t.start();
        int w = 0, h = 0;
        const QByteArray jpeg = enc.encode(f, p.width, p.height, p.quality, &w, &h);
        f.reset();                   // release the frame buffer back to the pool early
        m_lastEncodeUs = int(t.nsecsElapsed() / 1000);
        if (jpeg.isEmpty()) continue;
        m_encoded++;
        { QMutexLocker l(&m_outMutex); m_lastJpeg = jpeg; }
        emit frameReady(jpeg, w, h, n);
    }
}

// ---------------------------------------------------------------------------
// InputMonitor
// ---------------------------------------------------------------------------

namespace {
constexpr int kLowW = 320, kLowH = 180, kHighW = 480, kHighH = 270;
constexpr double kLowFps = 5.0, kHighFps = 10.0;
constexpr int kLowQ = 70, kHighQ = 78;
constexpr int kMaxInputs = 12;
constexpr int kRetryMs = 5000;
}

InputMonitor::InputMonitor(QObject* parent) : QObject(parent) {
    m_worker = std::thread([this]() { workerLoop(); });
}

InputMonitor::~InputMonitor() {
    { QMutexLocker l(&m_jobMutex); m_stop = true; m_jobCond.wakeAll(); }
    if (m_worker.joinable()) m_worker.join();
    { QMutexLocker l(&m_inbox->mutex); m_inbox->alive = false; }     // in-flight opens close their own receiver
    for (Entry& e : m_entries) if (e.src) retire(std::move(e.src));
    m_entries.clear();
}

void InputMonitor::setFormat(const OutputFormat& fmt) { m_fmt = fmt; }

bool InputMonitor::allowedType(const QString& t) {
    static const QSet<QString> ok = {"ndi", "stream", "udp", "bars", "color", "black", "slate", "image"};
    return ok.contains(t.toLower());
}

QJsonObject InputMonitor::normalize(const QJsonObject& in) {
    QJsonObject s = in;
    if (s.contains("ndiName") && !s.contains("name")) s["name"] = s["ndiName"];
    if (s.value("type").toString().isEmpty()) s["type"] = s.contains("url") ? "stream" : s.contains("port") ? "sdi_in" : "ndi";
    if (s.value("type").toString() == "stream" && !s.contains("realtime")) s["realtime"] = true;
    return s;
}

bool InputMonitor::sameSource(const QJsonObject& a0, const QJsonObject& b0) {
    const QJsonObject a = normalize(a0), b = normalize(b0);
    const QString ta = a.value("type").toString().toLower(), tb = b.value("type").toString().toLower();
    if (ta != tb) return false;
    if (ta == "ndi") {
        const QString ua = a.value("url").toString(), ub = b.value("url").toString();
        if (!ua.isEmpty() && !ub.isEmpty()) return ua.compare(ub, Qt::CaseInsensitive) == 0;
        const QString na = a.value("name").toString(a.value("ndiName").toString()), nb = b.value("name").toString(b.value("ndiName").toString());
        return !na.isEmpty() && na.compare(nb, Qt::CaseInsensitive) == 0;
    }
    if (ta == "stream" || ta == "udp") return a.value("url").toString() == b.value("url").toString();
    if (ta == "sdi_in" || ta == "dektec_sdi_in") return a.value("port").toInt() == b.value("port").toInt();
    // generators and images: the whole spec
    QJsonObject x = a, y = b;
    for (const char* k : {"bandwidth", "audio", "recvName"}) { x.remove(k); y.remove(k); }
    return x == y;
}

QJsonObject InputMonitor::openSpec(const QJsonObject& source, bool high) {
    QJsonObject s = source;
    if (s.value("type").toString().toLower() == "ndi") {
        s["bandwidth"] = high ? "highest" : "lowest";
        s["audio"] = high;
        if (!high) s["recvName"] = "PlayPlay Monitor";       // shows in NDI tools as a monitor, not as the program receiver
        else if (!s.contains("recvName")) s["recvName"] = "PlayPlay";
    }
    return s;
}

InputMonitor::Entry* InputMonitor::find(const QString& key) {
    for (Entry& e : m_entries) if (e.key == key) return &e;
    return nullptr;
}

int InputMonitor::count() const { QMutexLocker l(&m_statusMutex); return m_status.size(); }

void InputMonitor::retire(std::unique_ptr<LayerSource> s) {
    if (!s) return;
    if (m_retire) { m_retire(std::move(s)); return; }
    LayerSource* raw = s.release();
    std::thread([raw]() { raw->close(); delete raw; }).detach();
}

void InputMonitor::apply(const QJsonArray& inputs, int leaseSec, qint64 frame) {
    Q_UNUSED(frame);
    m_leaseUntilMs = inputs.isEmpty() ? 0 : nowMs() + qint64(qBound(5, leaseSec, 600)) * 1000;
    std::vector<Entry> next;
    QSet<QString> keys;
    for (const QJsonValue& v : inputs) {
        const QJsonObject o = v.toObject();
        const QString key = o.value("key").toString().left(32);
        const QJsonObject raw = o.value("source").toObject();
        if (key.isEmpty() || keys.contains(key) || raw.isEmpty() || int(next.size()) >= kMaxInputs) continue;
        const QJsonObject src = normalize(raw);
        keys.insert(key);
        const bool high = o.value("quality").toString() == "high";
        Entry* old = find(key);
        Entry e;
        if (old && sameSource(old->source, src)) {
            e = std::move(*old);
            old->key.clear();                                   // moved: not retired below
            e.source = src;
            if (e.high != high) {
                e.high = high;
                // NDI needs a new receiver for another bandwidth; the old one keeps the tile alive until it is replaced.
                if (src.value("type").toString().toLower() == "ndi" && e.openedHigh != high) { e.openingGen = 0; e.retryAtMs = 0; }
            }
        } else {
            e.key = key; e.source = src; e.high = high;
        }
        if (!allowedType(src.value("type").toString())) e.error = "not monitored: " + src.value("type").toString() + " inputs show as a card";
        next.push_back(std::move(e));
    }
    for (Entry& e : m_entries) if (!e.key.isEmpty() && e.src) retire(std::move(e.src));
    m_entries = std::move(next);
    for (Entry& e : m_entries) {
        if (!allowedType(e.source.value("type").toString())) continue;
        const bool ndi = e.source.value("type").toString().toLower() == "ndi";
        const bool needOpen = !e.openingGen && (!e.src || (ndi && e.openedHigh != e.high));
        if (needOpen) startOpen(e);
    }
    { QMutexLocker l(&m_jobMutex); for (auto it = m_jobs.begin(); it != m_jobs.end();) { if (!keys.contains(it.key())) it = m_jobs.erase(it); else ++it; } }
    publishStatus();
}

void InputMonitor::startOpen(Entry& e) {
    const quint64 gen = ++m_gen;
    e.openingGen = gen;
    e.error.clear();
    const QJsonObject spec = openSpec(e.source, e.high);
    const bool high = e.high;
    const QString key = e.key;
    const OutputFormat fmt = m_fmt;
    std::shared_ptr<Inbox> inbox = m_inbox;
    std::thread([inbox, spec, fmt, key, gen, high]() {
        QString err;
        std::unique_ptr<LayerSource> s = LayerSource::create(spec, &err);
        if (s && !s->open(fmt)) { err = s->lastError(); s->close(); s.reset(); }
        if (!s && err.isEmpty()) err = "could not open";
        QMutexLocker l(&inbox->mutex);
        if (!inbox->alive) { if (s) s->close(); return; }
        inbox->done.push_back(Opened{key, gen, std::move(s), err, high});
    }).detach();
}

void InputMonitor::tick(qint64 frame) {
    // Lease ran out (no UI renewed it): drop every receiver.
    if (!m_entries.empty() && m_leaseUntilMs > 0 && (frame & 15) == 0 && nowMs() > m_leaseUntilMs) {
        qInfo() << "[InputMonitor] lease expired: closing" << m_entries.size() << "input monitors";
        apply(QJsonArray(), 0, frame);
        return;
    }
    // Adopt receivers that finished opening.
    std::vector<Opened> done;
    { QMutexLocker l(&m_inbox->mutex); done.swap(m_inbox->done); }
    bool changed = !done.empty();
    for (Opened& o : done) {
        Entry* e = find(o.key);
        if (!e || e->openingGen != o.gen) { retire(std::move(o.src)); continue; }      // stale: the lease changed meanwhile
        e->openingGen = 0;
        if (!o.src) { e->error = o.error; e->retryAtMs = nowMs() + kRetryMs; continue; }
        if (e->src) retire(std::move(e->src));
        e->src = std::move(o.src);
        e->openedHigh = o.high;
        e->openedFrame = frame;
        e->error.clear();
    }
    const double fps = m_fmt.rate.nominalFps() > 0 ? double(m_fmt.rate.nominalFps()) : 30.0;
    const qint64 now = (frame & 7) == 0 || changed ? nowMs() : 0;
    for (Entry& e : m_entries) {
        if (!e.src) {
            if (!e.openingGen && e.retryAtMs > 0 && now > e.retryAtMs && allowedType(e.source.value("type").toString())) { e.retryAtMs = 0; startOpen(e); changed = true; }
            continue;
        }
        // Pull every tick like a compositor layer would (stream decoders pace themselves on it); encode on the cadence.
        const double pos = double(frame - e.openedFrame) / fps;
        FramePtr v = e.src->video(frame, pos);
        const bool hi = e.openedHigh || e.high;
        const int every = qMax(1, int(std::lround(fps / (hi ? kHighFps : kLowFps))));
        if (!v || (frame - e.openedFrame) % every != 0) continue;
        e.frames++;
        e.lastFrameMs = now ? now : nowMs();
        QMutexLocker l(&m_jobMutex);
        m_jobs[e.key] = Job{v, hi ? kHighW : kLowW, hi ? kHighH : kLowH, hi ? kHighQ : kLowQ};
        m_jobCond.wakeOne();
    }
    if (changed || (frame % 30) == 0) publishStatus();
}

std::unique_ptr<LayerSource> InputMonitor::takeWarm(const QJsonObject& spec) {
    for (Entry& e : m_entries) {
        if (!e.src || !sameSource(e.source, spec)) continue;
        // Only a full-quality receiver may go to air (a low-bandwidth NDI proxy would put a soft picture on program).
        const bool ndi = e.source.value("type").toString().toLower() == "ndi";
        if (ndi && !e.openedHigh) continue;
        if (!qobject_cast<LiveSourceBase*>(e.src.get())) continue;      // decoders run on their own clock: not handed over
        auto* live = static_cast<LiveSourceBase*>(e.src.get());
        if (!live->hasPicture() || !e.src->status().value("connected").toBool(false)) continue;
        live->dropBufferedAudio();                 // the take starts with fresh sound, not the ¼ s the monitor sat on
        std::unique_ptr<LayerSource> out = std::move(e.src);
        e.retryAtMs = nowMs() + 1500;              // reopen the monitor shortly (as a second receiver)
        e.frames = 0;
        { QMutexLocker l(&m_jobMutex); m_jobs.remove(e.key); }
        publishStatus();
        qInfo().noquote() << "[InputMonitor] warm take of" << e.key;
        return out;
    }
    return nullptr;
}

void InputMonitor::publishStatus() {
    QJsonArray a;
    const qint64 now = nowMs();
    for (const Entry& e : m_entries) {
        QJsonObject o;
        o["key"] = e.key; o["type"] = e.source.value("type").toString(); o["quality"] = e.high ? "high" : "low";
        const bool allowed = allowedType(e.source.value("type").toString());
        o["state"] = !allowed ? "card" : e.src ? "open" : e.openingGen ? "opening" : !e.error.isEmpty() ? "failed" : "waiting";
        bool connected = false;
        if (e.src) connected = e.src->status().value("connected").toBool(true);
        o["connected"] = connected;
        o["warm"] = e.src && e.openedHigh;
        o["frames"] = double(e.frames);
        o["stale"] = e.src && (e.lastFrameMs == 0 || now - e.lastFrameMs > 3000);
        if (!e.error.isEmpty()) o["error"] = e.error;
        a.append(o);
    }
    QMutexLocker l(&m_statusMutex);
    m_status = a;
}

QJsonArray InputMonitor::status() const { QMutexLocker l(&m_statusMutex); return m_status; }

void InputMonitor::workerLoop() {
    QHash<QString, PreviewEncoder*> perKey;
    for (;;) {
        QString key; Job job;
        {
            QMutexLocker l(&m_jobMutex);
            while (m_jobs.isEmpty() && !m_stop) m_jobCond.wait(&m_jobMutex, 250);
            if (m_stop) break;
            auto it = m_jobs.begin();
            key = it.key(); job = std::move(it.value());
            m_jobs.erase(it);
        }
        PreviewEncoder*& pe = perKey[key];
        if (!pe) pe = new PreviewEncoder();      // per input, so eight different source sizes never thrash one context
        int w = 0, h = 0;
        const QByteArray jpeg = pe->encode(job.frame, job.w, job.h, job.q, &w, &h);
        job.frame.reset();
        if (!jpeg.isEmpty()) emit frameReady(key, jpeg, w, h);
    }
    qDeleteAll(perKey);
}
