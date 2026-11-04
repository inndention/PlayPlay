#include "compositor.h"
#include "gpucompositor.h"
#include "basicsources.h"
#include "metersnapshot.h"
#include "../tickprofiler.h"
#include <QPainter>
#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <cmath>
#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Spec JSON
// ---------------------------------------------------------------------------

Compositor::Transition Compositor::Transition::fromJson(const QJsonObject& o) {
    Transition t;
    if (o.isEmpty()) return t;
    t.type = o.value("type").toString("cut");
    t.durationSec = o.value("durationSec").toDouble(o.value("duration").toDouble(0.0));
    const QString c = o.value("dipColor").toString();
    if (!c.isEmpty()) t.dipColor = QColor(c);
    if (t.type != "cut" && t.durationSec <= 0.0) t.durationSec = 0.5;
    return t;
}

QJsonObject Compositor::Transition::toJson() const {
    QJsonObject o; o["type"] = type; o["durationSec"] = durationSec; o["dipColor"] = dipColor.name(); return o;
}

Compositor::Placement Compositor::Placement::fromJson(const QJsonObject& o) {
    Placement p;
    if (o.contains("x") || o.contains("width")) {
        p.rect = QRectF(o.value("x").toDouble(0), o.value("y").toDouble(0), o.value("width").toDouble(0), o.value("height").toDouble(0));
    }
    p.anchor = o.value("anchor").toString("top_left");
    p.marginX = o.value("marginX").toDouble(o.value("margin").toDouble(0.0));
    p.marginY = o.value("marginY").toDouble(o.value("margin").toDouble(0.0));
    return p;
}

QJsonObject Compositor::Placement::toJson() const {
    QJsonObject o;
    if (!rect.isNull()) { o["x"] = rect.x(); o["y"] = rect.y(); o["width"] = rect.width(); o["height"] = rect.height(); }
    o["anchor"] = anchor; o["marginX"] = marginX; o["marginY"] = marginY;
    return o;
}

Compositor::LayerSpec Compositor::LayerSpec::fromJson(const QJsonObject& o) {
    LayerSpec s;
    s.id = o.value("id").toInt(o.value("layer").toInt(0));
    s.role = o.value("role").toString();
    s.source = o.value("source").toObject();
    if (s.source.isEmpty() && o.contains("template")) { s.source = o["template"].toObject(); s.source["type"] = "template"; }
    s.placement = Placement::fromJson(o.value("placement").toObject(o));
    s.opacity = o.value("opacity").toDouble(1.0);
    s.audioGain = o.contains("audioGainDb") ? std::pow(10.0, o["audioGainDb"].toDouble() / 20.0) : o.value("audioGain").toDouble(1.0);
    s.audioMute = o.value("audioMute").toBool(false);
    s.duck = o.value("duck").toBool(false);
    s.in = Transition::fromJson(o.value("in").toObject(o.value("transitionIn").toObject()));
    s.out = Transition::fromJson(o.value("out").toObject(o.value("transitionOut").toObject()));
    s.holdSec = o.value("holdSec").toDouble(o.value("duration").toDouble(0.0));
    for (const QJsonValue& v : o.value("mixes").toArray()) { const QString m = v.toString().trimmed(); if (!m.isEmpty() && !s.mixes.contains(m)) s.mixes << m; }
    if (!s.role.isEmpty() && s.id == 0) {
        static const QMap<QString, int> roles = {{"program", 10}, {"secondary", 15}, {"bug", 20}, {"lower_third", 30}, {"ticker", 40}, {"legal_id", 80}, {"emergency", 90}};
        s.id = roles.value(s.role, 50);
    }
    return s;
}

QJsonObject Compositor::LayerSpec::toJson() const {
    QJsonObject o;
    o["id"] = id; o["role"] = role; o["source"] = source; o["placement"] = placement.toJson();
    o["opacity"] = opacity; o["audioGain"] = audioGain; o["audioMute"] = audioMute; o["duck"] = duck;
    o["in"] = in.toJson(); o["out"] = out.toJson(); o["holdSec"] = holdSec;
    if (!mixes.isEmpty()) o["mixes"] = QJsonArray::fromStringList(mixes);
    return o;
}

// ---------------------------------------------------------------------------
// PE-05 reader state
// ---------------------------------------------------------------------------

/// The published meter snapshot and render's staging copy of it (filled during the tick under m_mutex, stored once).
struct Compositor::MeterSnap {
    metersnap::SeqLock<metersnap::Snapshot> lock;
    metersnap::Snapshot stage;
};

/// What a reader copies under m_mutex: plain values and implicitly shared Qt values (reference counts, no deep copy, no
/// JSON) plus raw source pointers, which stay valid while the reader holds m_statusMutex (see retire()).
struct Compositor::StatusCtl {
    struct L { int id = 0; LayerSpec spec; LayerState state = LayerState::Hidden; bool audible = false; LayerSource* src = nullptr; };
    struct M { quint32 serial = 0; QString id, name, solo; bool follow = true; double masterGain = 1.0; };
    std::vector<L> layers;
    std::vector<M> mixes;               // [0] = main
    int layerCount = 0;
    LayerSource* progA = nullptr;
    LayerSource* progB = nullptr;
    bool transitioning = false;
    QString solo;
    double programGain = 1.0, masterGain = 1.0;
    bool programMute = false, progItemMute = false;
    int depth = 0;
    bool gpu = false, gpuAuto = true, useGpu = true;
    QString gpuAdapter, autoPhase;
    double gpuCostMs = -1, cpuCostMs = -1;
    GpuCompositor::Counters gpuCounters;
};

// ---------------------------------------------------------------------------
// PE-13 audio buffer pool
// ---------------------------------------------------------------------------

/// Planar-float frames whose planes share one pooled buffer (FFmpeg allows a single AVBufferRef for all planes), laid out
/// like av_frame_get_buffer's (linesize[0] = samples rounded up to 32, planes 128-byte aligned). The capacity has room for
/// the 1601/1602 cadence; a larger frame or another channel count starts a new pool, and the old one frees itself once its
/// last buffer comes back (av_buffer_pool_uninit), so frames still held by an encoder queue stay valid.
struct Compositor::AudioPool {
    AVBufferPool* pool = nullptr;
    int capSamples = 0, channels = 0, stride = 0;
    ~AudioPool() { if (pool) av_buffer_pool_uninit(&pool); }
    FramePtr get(int n, int ch, int rate) {
        if (n <= 0 || ch <= 0 || ch > AV_NUM_DATA_POINTERS) return nullptr;     // caller falls back to allocAudioFrame
        if (!pool || n > capSamples || ch != channels) {
            if (pool) av_buffer_pool_uninit(&pool);
            capSamples = ((n + 31) & ~31) + 32; channels = ch; stride = capSamples * int(sizeof(float));
            pool = av_buffer_pool_init(size_t(stride) * size_t(ch), nullptr);
            if (!pool) return nullptr;
        }
        FramePtr f = makeFramePtr();
        f->buf[0] = av_buffer_pool_get(pool);
        if (!f->buf[0]) return nullptr;
        f->nb_samples = n; f->format = AV_SAMPLE_FMT_FLTP; f->sample_rate = rate;
        av_channel_layout_default(&f->ch_layout, ch);
        for (int c = 0; c < ch; ++c) f->data[c] = f->buf[0]->data + size_t(c) * size_t(stride);
        f->extended_data = f->data;
        f->linesize[0] = ((n + 31) & ~31) * int(sizeof(float));
        return f;
    }
};

FramePtr Compositor::audioFrame(int nbSamples) {
    if (m_poolAudio) {
        if (!m_audioPool) m_audioPool = std::make_unique<AudioPool>();
        if (FramePtr f = m_audioPool->get(nbSamples, m_fmt.channels, m_fmt.sampleRate)) return f;
    }
    return allocAudioFrame(nbSamples, m_fmt.channels, m_fmt.sampleRate);
}

namespace {
const QString& programStrip() { static const QString s = QStringLiteral("program"); return s; }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Compositor::Compositor(const OutputFormat& fmt, QObject* parent)
    : QObject(parent), m_fmt(fmt), m_pool(fmt.width, fmt.height, kMixerPixelFormat)
{
    m_mixes.push_back(std::make_unique<MixState>("main", fmt.sampleRate, fmt.channels));
    m_mixes.front()->name = "Main";
    m_mixes.front()->serial = ++m_mixSerial;
    if (!m_legacyStatus) m_snap = std::make_unique<MeterSnap>();
}

Compositor::~Compositor() = default;

void Compositor::setGpuEnabled(bool on) {
    QMutexLocker l(&m_mutex);
    m_gpuWanted = on;
    if (on && !m_gpu) {
        QString err;
        m_gpu = GpuCompositor::create(m_fmt.width, m_fmt.height, &err);
        if (m_gpu) {
            // Warm up: first render pays for pipeline state and raster-size textures
            // (tens of ms); do it now rather than on the first on-air frame.
            std::vector<GpuCompositor::DrawOp> ops;
            ops.push_back({blackFrame(), QRectF(0, 0, m_fmt.width, m_fmt.height), QRectF(), 1.0, QRectF(), false});
            ops.push_back({blackFrame(), QRectF(0, 0, m_fmt.width, m_fmt.height), QRectF(), 0.5, QRectF(0, 0, m_fmt.width / 2, m_fmt.height), false});
            FramePtr scratch = m_pool.get();
            if (scratch) m_gpu->render(ops, scratch);
            qInfo() << "[Compositor] GPU path on:" << m_gpu->adapterName() << "warm-up" << m_gpu->lastRenderMs() << "ms";
        } else qWarning() << "[Compositor] GPU path unavailable:" << err;
    } else if (!on) {
        m_gpu.reset();
    }
}

bool Compositor::gpuActive() const { QMutexLocker l(&m_mutex); return m_gpu != nullptr; }

void Compositor::retire(std::unique_ptr<LayerSource> s) {
    if (!s) return;
    if (m_legacyStatus) { if (m_retire) m_retire(std::move(s)); return; }   // else: destroyed here
    // PE-05: a reader may be inside s->status() right now (it took the pointer under m_mutex and calls it outside, holding
    // m_statusMutex). Park the source until no reader is in; usually that is now, and it goes on exactly as before.
    m_graveyard.push_back(std::move(s));
    flushGraveyard();
}

void Compositor::flushGraveyard() {
    if (m_graveyard.empty() || !m_statusMutex.tryLock()) return;     // a reader is in: the next render tries again
    // No reader holds a pointer now, and none can get one to these: they left every structure a reader copies from, and
    // a reader copies only while holding both locks (we hold m_mutex).
    std::vector<std::unique_ptr<LayerSource>> out;
    out.swap(m_graveyard);
    m_statusMutex.unlock();
    for (auto& src : out) if (m_retire) m_retire(std::move(src));    // else: destroyed when `out` goes
}

void Compositor::applyDeferredUpdates() {
    // src->update() and src->status() touch the same members (the spec, the text): they must not overlap. A reader is
    // only ever briefly inside status(); render and the mutators never wait for it: the update then waits for a tick.
    if (m_deferredUpdates.empty() || !m_statusMutex.tryLock()) return;
    for (const DeferredUpdate& u : m_deferredUpdates) {
        auto it = m_layers.find(u.layer);
        if (it != m_layers.end() && it->second.src && it->second.srcGen == u.gen) it->second.src->update(u.params);
    }
    m_deferredUpdates.clear();
    m_statusMutex.unlock();
}

FramePtr Compositor::blackFrame() {
    if (!m_black) {
        m_black = m_pool.get();
        for (int y = 0; y < m_fmt.height; ++y) {
            uint32_t* row = reinterpret_cast<uint32_t*>(m_black->data[0] + y * m_black->linesize[0]);
            for (int x = 0; x < m_fmt.width; ++x) row[x] = 0xFF000000u;
        }
    }
    return m_black;
}

// ---------------------------------------------------------------------------
// Program bus
// ---------------------------------------------------------------------------

void Compositor::hideProgramTimeLayers() {
    for (auto& kv : m_layers) {
        Layer& layer = kv.second;
        if (layer.state != LayerState::Hidden && layer.src && layer.src->wantsProgramTime()) layer.state = LayerState::Hidden;
    }
}

void Compositor::programTake(std::unique_ptr<LayerSource> next, const Transition& t, qint64 frame) {
    QMutexLocker l(&m_mutex);
    // The position handed to render() restarts for the new item: captions synced to the old one would
    // replay its opening lines over the new picture, so they go on the same frame as the take.
    hideProgramTimeLayers();
    // A pending transition is finished immediately (hard cut to its target).
    if (m_progB) { retire(std::move(m_progA)); m_progA = std::move(m_progB); m_progLastA = m_progLastB; m_progLastB.reset(); }
    m_progClearing = false;
    if (!m_progA || t.isCut()) {
        retire(std::move(m_progA));
        m_progA = std::move(next);
        m_progLastA.reset();
        return;
    }
    m_progB = std::move(next);
    m_progTrans = t;
    m_progTransStart = frame;
    m_progBStartFrame = frame;
    m_progPosA = m_lastProgramPos;
    m_progLastB.reset();
}

void Compositor::programClear(const Transition& t, qint64 frame) {
    QMutexLocker l(&m_mutex);
    hideProgramTimeLayers();
    if (m_progB) { retire(std::move(m_progA)); m_progA = std::move(m_progB); m_progLastA = m_progLastB; m_progLastB.reset(); }
    if (!m_progA) return;
    if (t.isCut()) { retire(std::move(m_progA)); m_progLastA.reset(); return; }
    m_progTrans = t;
    m_progTransStart = frame;
    m_progPosA = m_lastProgramPos;
    m_progClearing = true;      // A fades/wipes to slate
}

void Compositor::setSlate(std::unique_ptr<LayerSource> slate) {
    QMutexLocker l(&m_mutex);
    retire(std::move(m_slate));
    m_slate = std::move(slate);
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

bool Compositor::addLayer(const LayerSpec& spec, qint64 frame, QString* error) {
    std::unique_ptr<LayerSource> src = LayerSource::create(spec.source, error);
    if (!src) return false;
    if (!src->open(m_fmt)) { if (error) *error = src->lastError(); return false; }
    QMutexLocker l(&m_mutex);
    Layer& layer = m_layers[spec.id];
    retire(std::move(layer.src));
    layer.spec = spec;
    applyPendingMixes(spec.id, layer.spec);
    layer.src = std::move(src);
    layer.srcGen = ++m_srcGen;
    layer.state = spec.in.isCut() ? LayerState::Shown : LayerState::AnimIn;
    layer.shownFrame = frame;
    layer.animStart = frame;
    layer.anim = spec.in;
    layer.lastVideo.reset();
    return true;
}

void Compositor::addLayer(const LayerSpec& spec, std::unique_ptr<LayerSource> opened, qint64 frame) {
    if (!opened) return;
    QMutexLocker l(&m_mutex);
    Layer& layer = m_layers[spec.id];
    retire(std::move(layer.src));
    layer.spec = spec;
    applyPendingMixes(spec.id, layer.spec);
    layer.src = std::move(opened);
    layer.srcGen = ++m_srcGen;
    layer.state = spec.in.isCut() ? LayerState::Shown : LayerState::AnimIn;
    layer.shownFrame = frame; layer.animStart = frame; layer.anim = spec.in;
    layer.lastVideo.reset();
}

bool Compositor::showLayer(int id, const Transition& t, qint64 frame) {
    QMutexLocker l(&m_mutex);
    auto it = m_layers.find(id);
    if (it == m_layers.end()) return false;
    Layer& layer = it->second;
    if (layer.state == LayerState::Shown || layer.state == LayerState::AnimIn) return true;
    layer.state = t.isCut() ? LayerState::Shown : LayerState::AnimIn;
    layer.shownFrame = frame; layer.animStart = frame; layer.anim = t;
    return true;
}

bool Compositor::hideLayer(int id, const Transition& t, qint64 frame) {
    QMutexLocker l(&m_mutex);
    auto it = m_layers.find(id);
    if (it == m_layers.end()) return false;
    Layer& layer = it->second;
    if (layer.state == LayerState::Hidden) return true;
    if (t.isCut()) { layer.state = LayerState::Hidden; return true; }
    layer.state = LayerState::AnimOut; layer.animStart = frame; layer.anim = t;
    return true;
}

void Compositor::applyPendingMixes(int id, LayerSpec& spec) {
    auto it = m_pendingMixes.find(id);
    if (it == m_pendingMixes.end()) return;
    if (QDateTime::currentMSecsSinceEpoch() - it->second.second < 5000 && spec.mixes.isEmpty()) spec.mixes = it->second.first;
    m_pendingMixes.erase(it);
}

bool Compositor::updateLayer(int id, const QJsonObject& params) {
    QMutexLocker l(&m_mutex);
    auto it = m_layers.find(id);
    if (it == m_layers.end()) {
        // A graphic being opened: keep its mix assignment for when it attaches.
        if (params.contains("mixes")) {
            QStringList mx; for (const QJsonValue& v : params["mixes"].toArray()) { const QString m = v.toString().trimmed(); if (!m.isEmpty() && !mx.contains(m)) mx << m; }
            m_pendingMixes[id] = {mx, QDateTime::currentMSecsSinceEpoch()};
        }
        return false;
    }
    Layer& layer = it->second;
    if (params.contains("opacity")) layer.spec.opacity = params["opacity"].toDouble(1.0);
    if (params.contains("audioGain")) layer.spec.audioGain = params["audioGain"].toDouble(1.0);
    if (params.contains("audioGainDb")) layer.spec.audioGain = std::pow(10.0, params["audioGainDb"].toDouble() / 20.0);
    if (params.contains("audioMute")) layer.spec.audioMute = params["audioMute"].toBool();
    if (params.contains("placement")) layer.spec.placement = Placement::fromJson(params["placement"].toObject());
    if (params.contains("x") || params.contains("anchor") || params.contains("width")) layer.spec.placement = Placement::fromJson(params);
    if (params.contains("holdSec")) layer.spec.holdSec = params["holdSec"].toDouble();
    if (params.contains("mixes")) {
        layer.spec.mixes.clear(); for (const QJsonValue& v : params["mixes"].toArray()) { const QString m = v.toString().trimmed(); if (!m.isEmpty() && !layer.spec.mixes.contains(m)) layer.spec.mixes << m; }
        m_pendingMixes[id] = {layer.spec.mixes, QDateTime::currentMSecsSinceEpoch()};   // a re-show of this id that is still opening keeps it too
    }
    const QJsonObject sp = params.contains("source") ? params["source"].toObject() : params;
    static const QSet<QString> layerKeys = {"action", "layer", "id", "role", "opacity", "audioGain", "audioGainDb", "audioMute", "placement", "x", "y", "width", "height", "anchor", "margin", "marginX", "marginY", "holdSec", "in", "out", "transition", "duck", "mixes", "mix"};
    for (auto it = sp.begin(); it != sp.end(); ++it) if (params.contains("source") || !layerKeys.contains(it.key())) layer.spec.source[it.key()] = it.value();
    if (layer.src) {
        if (m_legacyStatus) layer.src->update(sp);
        else { m_deferredUpdates.push_back({id, layer.srcGen, sp}); applyDeferredUpdates(); }   // PE-05: now, unless a reader is in status()
    }
    return true;
}

bool Compositor::removeLayer(int id) {
    QMutexLocker l(&m_mutex);
    auto it = m_layers.find(id);
    if (it == m_layers.end()) return false;
    retire(std::move(it->second.src));
    m_layers.erase(it);
    return true;
}

void Compositor::clearLayers(const QString& role) {
    QMutexLocker l(&m_mutex);
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        // audio layers (mixer line-ins) are not graphics: only an explicit role clears them
        if ((role.isEmpty() && it->second.spec.role != "audio") || (!role.isEmpty() && it->second.spec.role == role)) { retire(std::move(it->second.src)); it = m_layers.erase(it); }
        else ++it;
    }
}

// The telemetry copy of a layer's source spec. layersJson() rides the 1 Hz engine state to every client
// (and into GET /channels/{id}), so bulky payloads (a karaoke transcript is one entry per word, ~85 bytes
// each) are replaced by counts; the full spec stays in Layer::spec. Clients read type/kind/title/name only.
static QJsonObject telemetrySource(const QJsonObject& src) {
    QJsonObject o = src;
    for (const char* key : {"segments", "words", "lines", "items"}) {
        if (!o.value(key).isArray()) continue;
        o[QString(key) + "Count"] = o.value(key).toArray().size();
        o.remove(key);
    }
    return o;
}

/// Copies what a reader needs from the locked state. Caller holds m_mutex; nothing here builds JSON or calls a source.
void Compositor::copyStatusCtl(StatusCtl& c, unsigned parts) const {
    c.layerCount = int(m_layers.size());
    c.transitioning = (m_progB != nullptr) || m_progClearing;
    c.solo = m_solo;
    c.programGain = m_programGain; c.masterGain = m_masterGain; c.programMute = m_programMute; c.progItemMute = m_progItemMute.load();
    c.depth = m_depth;
    if (parts & CtlProgram) { c.progA = m_progA.get(); c.progB = m_progB.get(); }
    if (parts & CtlLayers)
        for (const auto& kv : m_layers) c.layers.push_back({kv.first, kv.second.spec, kv.second.state, kv.second.audible, kv.second.src.get()});
    if (parts & CtlMixes)
        for (const auto& m : m_mixes) c.mixes.push_back({m->serial, m->id, m->name, m->solo, m->follow, m->masterGain});
    if ((parts & CtlGpu) && m_gpu) {
        c.gpu = true; c.gpuAdapter = m_gpu->adapterName(); c.gpuCounters = m_gpu->counters();
        c.gpuAuto = m_gpuAuto; c.useGpu = m_useGpu; c.autoPhase = m_autoPhase; c.gpuCostMs = m_gpuCostMs; c.cpuCostMs = m_cpuCostMs;
    }
}

QJsonArray Compositor::layersJson() const {
    if (m_legacyStatus) return layersJsonLegacy();
    StatusCtl c;
    c.layers.reserve(16);
    std::vector<QJsonObject> srcStatus;
    {
        QMutexLocker s(&m_statusMutex);         // the sources stay alive, and get no update(), until we are done asking them
        { QMutexLocker l(&m_mutex); copyStatusCtl(c, CtlLayers); }
        srcStatus.resize(c.layers.size());
        for (size_t i = 0; i < c.layers.size(); ++i) if (c.layers[i].src) srcStatus[i] = c.layers[i].src->status();
    }
    QJsonArray a;
    for (size_t i = 0; i < c.layers.size(); ++i) {
        const StatusCtl::L& L = c.layers[i];
        QJsonObject o = L.spec.toJson();
        o["state"] = L.state == LayerState::Hidden ? "hidden" : L.state == LayerState::AnimIn ? "in" : L.state == LayerState::AnimOut ? "out" : "shown";
        o["audible"] = L.audible;
        o["source"] = telemetrySource(L.spec.source);
        if (L.src) o["sourceStatus"] = srcStatus[i];
        a.append(o);
    }
    return a;
}

QJsonArray Compositor::layersJsonLegacy() const {
    QMutexLocker l(&m_mutex);
    QJsonArray a;
    for (const auto& kv : m_layers) {
        const Layer& layer = kv.second;
        QJsonObject o = layer.spec.toJson();
        o["state"] = layer.state == LayerState::Hidden ? "hidden" : layer.state == LayerState::AnimIn ? "in" : layer.state == LayerState::AnimOut ? "out" : "shown";
        o["audible"] = layer.audible;
        o["source"] = telemetrySource(layer.spec.source);
        if (layer.src) o["sourceStatus"] = layer.src->status();
        a.append(o);
    }
    return a;
}

// ---------------------------------------------------------------------------
// Geometry and timing helpers
// ---------------------------------------------------------------------------

double Compositor::ease(double t) { t = qBound(0.0, t, 1.0); return t * t * (3.0 - 2.0 * t); }

double Compositor::progress(qint64 frame, qint64 start, const Transition& t, const FrameRate& rate) {
    if (t.isCut()) return 1.0;
    const double elapsed = rate.frameToNs(frame - start) / 1e9;
    return qBound(0.0, elapsed / t.durationSec, 1.0);
}

QRectF Compositor::placeRect(const Placement& p, const QSize& natural) const {
    if (!p.rect.isNull() && p.rect.width() > 0 && p.rect.height() > 0) return p.rect;
    const double w = natural.isEmpty() ? m_fmt.width : natural.width();
    const double h = natural.isEmpty() ? m_fmt.height : natural.height();
    double x = p.marginX, y = p.marginY;
    const QString a = p.anchor;
    if (a.endsWith("right")) x = m_fmt.width - w - p.marginX;
    else if (a == "top" || a == "center" || a == "bottom") x = (m_fmt.width - w) / 2.0;
    if (a.startsWith("bottom")) y = m_fmt.height - h - p.marginY;
    else if (a == "left" || a == "center" || a == "right") y = (m_fmt.height - h) / 2.0;
    return QRectF(x, y, w, h);
}

// ---------------------------------------------------------------------------
// Draw list
// ---------------------------------------------------------------------------

void Compositor::buildProgramDraws(qint64 frame, double programPosSec, std::vector<DrawItem>& out, FramePtr* passthrough) {
    const QRectF full(0, 0, m_fmt.width, m_fmt.height);
    // PE-08: each picture is drawn with the alpha convention of the source it came from (a and m_progLastA are program A's,
    // b and m_progLastB program B's, slate frames the slate's, black / dip are straight).
    auto pm = [](const std::unique_ptr<LayerSource>& s) { return s && s->deliversPremultiplied(); };
    auto item = [](const FramePtr& f, const QRectF& dst, double opacity, const QRectF& clip, bool premul) {
        DrawItem d{f, dst, QRectF(), opacity, clip}; d.premultiplied = premul; return d;
    };
    FramePtr a, b;
    if (m_progA && !m_programHold) { a = m_progA->video(frame, m_progClearing || m_progB ? m_progPosA + m_fmt.rate.frameToNs(frame - m_progTransStart) / 1e9 : programPosSec); if (a) m_progLastA = a; }
    if (!a) a = m_progLastA;
    bool aPm = a && pm(m_progA);
    const bool inTransition = (m_progB || m_progClearing);
    if (inTransition) {
        if (m_progB) { b = m_progB->video(frame, programPosSec); if (b) m_progLastB = b; if (!b) b = m_progLastB; }
        const double p = ease(progress(frame, m_progTransStart, m_progTrans, m_fmt.rate));
        FramePtr target = b;
        bool tPm = b && pm(m_progB);
        if (!target) { target = (m_slate && m_slateVisible) ? m_slate->video(frame, m_fmt.rate.frameToNs(frame) / 1e9) : nullptr; tPm = target && pm(m_slate); }
        if (!target) target = blackFrame();
        if (!a) { if (m_slate && m_slateVisible) { a = m_slate->video(frame, 0.0); aPm = a && pm(m_slate); } else a = blackFrame(); }
        const QString t = m_progTrans.type;
        if (t == "mix" || t == "fade") {
            out.push_back(item(a, full, 1.0, QRectF(), aPm));
            out.push_back(item(target, full, p, QRectF(), tPm));
        } else if (t == "dip") {
            FramePtr dip = m_pool.get();   // solid dip colour
            if (dip) { QImage img(dip->data[0], m_fmt.width, m_fmt.height, dip->linesize[0], QImage::Format_ARGB32); img.fill(m_progTrans.dipColor); }
            if (p < 0.5) { out.push_back(item(a, full, 1.0, QRectF(), aPm)); out.push_back({dip ? dip : blackFrame(), full, QRectF(), p * 2.0, QRectF()}); }
            else { out.push_back({dip ? dip : blackFrame(), full, QRectF(), 1.0, QRectF()}); out.push_back(item(target, full, (p - 0.5) * 2.0, QRectF(), tPm)); }
        } else if (t.startsWith("wipe")) {
            QRectF clip = full;
            if (t == "wipe_left") clip.setWidth(full.width() * p);
            else if (t == "wipe_right") clip.setLeft(full.width() * (1.0 - p));
            else if (t == "wipe_up") clip.setTop(full.height() * (1.0 - p));
            else clip.setHeight(full.height() * p);
            out.push_back(item(a, full, 1.0, QRectF(), aPm));
            out.push_back(item(target, full, 1.0, clip, tPm));
        } else if (t.startsWith("push")) {
            // The outgoing picture slides out by `off`; the incoming one follows it
            // from the opposite edge, so at p = 1 it sits exactly on the raster.
            QPointF off(0, 0), from(0, 0);
            if (t == "push_left") { off = QPointF(-full.width() * p, 0); from = QPointF(full.width(), 0); }
            else if (t == "push_right") { off = QPointF(full.width() * p, 0); from = QPointF(-full.width(), 0); }
            else if (t == "push_up") { off = QPointF(0, -full.height() * p); from = QPointF(0, full.height()); }
            else { off = QPointF(0, full.height() * p); from = QPointF(0, -full.height()); }
            out.push_back(item(a, full.translated(off), 1.0, QRectF(), aPm));
            out.push_back(item(target, full.translated(from + off), 1.0, QRectF(), tPm));
        } else if (t == "squeeze") {
            const double s = 1.0 - p;
            QRectF r(full.width() * (1 - s) / 2, full.height() * (1 - s) / 2, full.width() * s, full.height() * s);
            out.push_back(item(target, full, 1.0, QRectF(), tPm));
            if (s > 0.01) out.push_back(item(a, r, 1.0, QRectF(), aPm));
        } else {
            out.push_back(item(target, full, 1.0, QRectF(), tPm));
        }
        if (p >= 1.0) {
            // Transition finished: B becomes A.
            if (m_progB) { retire(std::move(m_progA)); m_progA = std::move(m_progB); m_progLastA = m_progLastB; m_progLastB.reset(); }
            else if (m_progClearing) { retire(std::move(m_progA)); m_progLastA.reset(); }
            m_progClearing = false;
        }
        return;
    }
    if (!a) {
        if (m_slate && m_slateVisible) { a = m_slate->video(frame, m_fmt.rate.frameToNs(frame) / 1e9); aPm = a && pm(m_slate); }
        if (!a) { a = blackFrame(); aPm = false; }
    }
    // A picture of another shape (a vertical 720x1280 NDI feed, a 4:3 camera) keeps its aspect, centred on black, instead of
    // being stretched to the raster. Decoded files arrive letterboxed at the raster size already and take the full rect.
    QRectF dst = full;
    if (a->width > 0 && a->height > 0 && (a->width != m_fmt.width || a->height != m_fmt.height)) {
        const double fa = double(a->width) / a->height, ra = double(m_fmt.width) / m_fmt.height;
        if (std::fabs(fa / ra - 1.0) > 0.01) {
            const double w = fa < ra ? m_fmt.height * fa : m_fmt.width, h = fa < ra ? m_fmt.height : m_fmt.width / fa;
            dst = QRectF((m_fmt.width - w) / 2.0, (m_fmt.height - h) / 2.0, w, h);
        }
    }
    out.push_back(item(a, dst, 1.0, QRectF(), aPm));
    // Zero copy hands the picture to every output untouched, so only a straight one may go (outputs are straight BGRA;
    // a premultiplied template / HTML program picture is composited onto black instead).
    if (passthrough) *passthrough = aPm ? FramePtr() : a;
}

void Compositor::buildLayerDraws(qint64 frame, double programPosSec, std::vector<DrawItem>& out) {
    const QRectF full(0, 0, m_fmt.width, m_fmt.height);
    for (auto it = m_layers.begin(); it != m_layers.end();) {
        Layer& layer = it->second;
        if (layer.state == LayerState::Hidden) { ++it; continue; }
        // Auto-hide after holdSec.
        if (layer.state == LayerState::Shown && layer.spec.holdSec > 0.0 &&
            m_fmt.rate.frameToNs(frame - layer.shownFrame) / 1e9 >= layer.spec.holdSec) {
            layer.state = layer.spec.out.isCut() ? LayerState::Hidden : LayerState::AnimOut;
            layer.animStart = frame; layer.anim = layer.spec.out;
            if (layer.state == LayerState::Hidden) { ++it; continue; }
        }
        // Overlays run on their own timeline from the moment shown; sources that ask for it
        // (karaoke captions) get the program item position so they stay in sync with the audio.
        const double pos = (layer.src && layer.src->wantsProgramTime()) ? programPosSec : m_fmt.rate.frameToNs(frame - layer.shownFrame) / 1e9;
        FramePtr v;
        if (layer.src) { tickprof::SourceCall sc(it->first, layer.src.get(), "video"); v = layer.src->video(frame, pos); }   // PE-06: names the slowest layer
        if (v) layer.lastVideo = v; else v = layer.lastVideo;
        const bool blank = layer.src && layer.src->isBlank();      // asked after video(): nothing to draw this frame
        double p = 1.0;
        if (layer.state == LayerState::AnimIn) { p = ease(progress(frame, layer.animStart, layer.anim, m_fmt.rate)); if (p >= 1.0) layer.state = LayerState::Shown; }
        else if (layer.state == LayerState::AnimOut) { p = 1.0 - ease(progress(frame, layer.animStart, layer.anim, m_fmt.rate)); if (p <= 0.0) { layer.state = LayerState::Hidden; ++it; continue; } }
        if (v && !blank) {
            const QSize natural = layer.src && !layer.src->nativeSize().isEmpty() ? layer.src->nativeSize() : QSize(v->width, v->height);
            QRectF dst = placeRect(layer.spec.placement, natural);
            // A source that paints a sub-rectangle of its canvas (karaoke caption box): place the whole
            // canvas as usual, then map that rectangle into it (the picture is just the rectangle).
            const QRect content = layer.src ? layer.src->contentRect() : QRect();
            if (!content.isEmpty() && natural.width() > 0 && natural.height() > 0) {
                const double sx = dst.width() / natural.width(), sy = dst.height() / natural.height();
                dst = QRectF(dst.x() + content.x() * sx, dst.y() + content.y() * sy, content.width() * sx, content.height() * sy);
            }
            double opacity = layer.spec.opacity;
            QRectF clip;
            const QString t = layer.anim.type;
            if (p < 1.0) {
                if (t == "fade" || t == "mix") opacity *= p;
                else if (t == "slide_left") dst.translate(-dst.right() * (1.0 - p), 0);
                else if (t == "slide_right") dst.translate((full.width() - dst.left()) * (1.0 - p), 0);
                else if (t == "slide_up") dst.translate(0, -dst.bottom() * (1.0 - p));
                else if (t == "slide_down") dst.translate(0, (full.height() - dst.top()) * (1.0 - p));
                else if (t == "wipe_left") { clip = dst; clip.setWidth(dst.width() * p); }
                else if (t == "wipe_right") { clip = dst; clip.setLeft(dst.right() - dst.width() * p); }
                else if (t == "wipe_up") { clip = dst; clip.setTop(dst.bottom() - dst.height() * p); }
                else if (t == "wipe_down") { clip = dst; clip.setHeight(dst.height() * p); }
                else opacity *= p;
            }
            QRectF srcRect(0, 0, v->width, v->height);
            QRect upload;
            // Large overlay pictures (HTML pages, full-raster templates) are mostly transparent: draw and upload only the box
            // that has pixels. The box is found once per picture buffer (a lower third at rest is scanned once).
            if (m_cropOverlays && qint64(v->width) * v->height >= 256 * 256 && v->format == kMixerPixelFormat) {
                if (layer.bboxKey != v->data[0] || layer.bboxHeld.get() == nullptr) { layer.bbox = alphaBox(v); layer.bboxKey = v->data[0]; layer.bboxHeld = v; }
                else layer.bboxHeld = v;
                if (layer.bbox.isEmpty()) { ++it; continue; }                     // nothing visible this frame
                if (layer.bbox != QRect(0, 0, v->width, v->height)) {
                    const double sx = dst.width() / v->width, sy = dst.height() / v->height;
                    srcRect = QRectF(layer.bbox);
                    dst = QRectF(dst.x() + layer.bbox.x() * sx, dst.y() + layer.bbox.y() * sy, layer.bbox.width() * sx, layer.bbox.height() * sy);
                    upload = layer.bbox.adjusted(-1, -1, 1, 1).intersected(QRect(0, 0, v->width, v->height));   // 1 px apron for filtering
                }
            }
            // lastVideo comes from the same source (it is reset when a layer gets a new one), so the flag holds for repeats too.
            if (opacity > 0.0) out.push_back({v, dst, srcRect, opacity, clip, it->first, upload, layer.src && layer.src->deliversPremultiplied()});
        }
        ++it;
    }
}

/// Auto path state machine, once per tick that composited: trial each path for 60 frames, run the cheaper one, and every
/// ~1800 composited frames try the other one again for 60 (it switches only when that is at least 15 % cheaper).
void Compositor::autoAccount(bool gpu, double ms) {
    m_trialAcc += ms; m_trialN++;
    const int trial = 60;
    auto finishTrial = [&](double& cost) { cost = m_trialAcc / qMax(1, m_trialN); m_trialAcc = 0; m_trialN = 0; };
    if (m_autoPhase == "trial_gpu" && m_trialN >= trial) { finishTrial(m_gpuCostMs); m_autoPhase = "trial_cpu"; m_useGpu = false; }
    else if (m_autoPhase == "trial_cpu" && m_trialN >= trial) {
        finishTrial(m_cpuCostMs); m_autoPhase = "run"; m_autoCount = 0;
        m_useGpu = m_gpuCostMs >= 0 && m_gpuCostMs < m_cpuCostMs;
        qInfo().noquote() << QString("[Compositor] auto path: GPU %1 ms, CPU %2 ms per composited frame -> %3").arg(m_gpuCostMs, 0, 'f', 2).arg(m_cpuCostMs, 0, 'f', 2).arg(m_useGpu ? "GPU" : "CPU");
    } else if (m_autoPhase == "run") {
        m_trialAcc = 0; m_trialN = 0;
        if (++m_autoCount >= 1800) { m_autoPhase = "recheck"; m_useGpu = !m_useGpu; }
    } else if (m_autoPhase == "recheck" && m_trialN >= trial) {
        double& other = gpu ? m_gpuCostMs : m_cpuCostMs;
        finishTrial(other);
        const double cur = gpu ? m_cpuCostMs : m_gpuCostMs;          // the path we left for the recheck
        const bool keepOther = other < cur * 0.85;
        if (!keepOther) m_useGpu = !m_useGpu;
        m_autoPhase = "run"; m_autoCount = 0;
        if (keepOther) qInfo().noquote() << QString("[Compositor] auto path: switching to %1 (%2 ms vs %3 ms)").arg(gpu ? "GPU" : "CPU").arg(other, 0, 'f', 2).arg(cur, 0, 'f', 2);
    }
}

QRect Compositor::alphaBox(const FramePtr& f) {
    const int w = f->width, h = f->height;
    // Rows: 8 bytes = 2 pixels at a time, only the alpha bytes are tested.
    auto rowHasAlpha = [&](int y) {
        const uint8_t* r = f->data[0] + size_t(y) * f->linesize[0];
        const uint64_t* q = reinterpret_cast<const uint64_t*>(r);
        const int pairs = w / 2;
        for (int i = 0; i < pairs; ++i) if (q[i] & 0xFF000000FF000000ULL) return true;
        return (w & 1) && r[size_t(w - 1) * 4 + 3];
    };
    int top = 0; while (top < h && !rowHasAlpha(top)) ++top;
    if (top == h) return QRect();
    int bottom = h - 1; while (bottom > top && !rowHasAlpha(bottom)) --bottom;
    int left = w, right = -1;
    for (int y = top; y <= bottom; ++y) {
        const uint8_t* r = f->data[0] + size_t(y) * f->linesize[0] + 3;
        for (int x = 0; x < left; ++x) if (r[size_t(x) * 4]) { left = x; break; }
        for (int x = w - 1; x > right; --x) if (r[size_t(x) * 4]) { right = x; break; }
    }
    if (right < left) return QRect();
    return QRect(left, top, right - left + 1, bottom - top + 1);
}

// ---------------------------------------------------------------------------
// CPU render
// ---------------------------------------------------------------------------

FramePtr Compositor::renderCpu(const std::vector<DrawItem>& draws, const FramePtr& passthrough) {
    if (passthrough && draws.size() == 1 && passthrough->width == m_fmt.width && passthrough->height == m_fmt.height) return passthrough;   // zero copy
    FramePtr out = m_pool.get();
    if (!out) return passthrough;
    const QRectF full(0, 0, m_fmt.width, m_fmt.height);
    size_t first = 0;
    // A full-raster first picture is copied instead of drawn. Not a premultiplied one (PE-08): it may carry alpha, and it
    // must come out composited onto black (opaque), as everything the compositor outputs.
    if (!draws.empty() && draws[0].dst == full && draws[0].opacity >= 1.0 && draws[0].clip.isNull() && !draws[0].premultiplied &&
        draws[0].frame && draws[0].frame->width == m_fmt.width && draws[0].frame->height == m_fmt.height) {
        for (int y = 0; y < m_fmt.height; ++y) memcpy(out->data[0] + y * out->linesize[0], draws[0].frame->data[0] + y * draws[0].frame->linesize[0], size_t(m_fmt.width) * 4);
        first = 1;
    } else {
        for (int y = 0; y < m_fmt.height; ++y) { uint32_t* row = reinterpret_cast<uint32_t*>(out->data[0] + y * out->linesize[0]); for (int x = 0; x < m_fmt.width; ++x) row[x] = 0xFF000000u; }
    }
    QImage canvas(out->data[0], m_fmt.width, m_fmt.height, out->linesize[0], QImage::Format_ARGB32_Premultiplied);
    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    for (size_t i = first; i < draws.size(); ++i) {
        const DrawItem& d = draws[i];
        if (!d.frame) continue;
        // Program/decoded frames are opaque BGRA; overlays carry straight alpha (QPainter premultiplies them per draw),
        // or premultiplied alpha (PE-08: blended as they are).
        const QImage src(d.frame->data[0], d.frame->width, d.frame->height, d.frame->linesize[0],
                         d.premultiplied ? QImage::Format_ARGB32_Premultiplied : QImage::Format_ARGB32);
        p.save();
        p.setOpacity(d.opacity);
        if (!d.clip.isNull()) p.setClipRect(d.clip);
        const QRectF sr = d.srcRect.isNull() ? QRectF(0, 0, d.frame->width, d.frame->height) : d.srcRect;
        p.drawImage(d.dst, src, sr);
        p.restore();
    }
    p.end();
    return out;
}

// ---------------------------------------------------------------------------
// Mixes
// ---------------------------------------------------------------------------

Compositor::MixState* Compositor::mixById(const QString& id) const {
    for (const auto& m : m_mixes) if (m->id == id) return m.get();
    return nullptr;
}

QStringList Compositor::mixIds() const {
    QMutexLocker l(&m_mutex);
    QStringList ids; for (const auto& m : m_mixes) ids << m->id;
    return ids;
}

void Compositor::setMix(const QString& id0, const QJsonObject& p) {
    const QString id = id0.trimmed().isEmpty() ? QString("main") : id0.trimmed().left(32);
    auto lin = [](double db) { return db <= -90.0 ? 0.0 : std::pow(10.0, db / 20.0); };
    QMutexLocker l(&m_mutex);
    MixState* m = mixById(id);
    if (!m) {
        if (m_mixes.size() >= 8) { qWarning() << "[Compositor] mix limit reached; not adding" << id; return; }
        m_mixes.push_back(std::make_unique<MixState>(id, m_fmt.sampleRate, m_fmt.channels));
        m = m_mixes.back().get();
        m->serial = ++m_mixSerial;
    }
    const bool main = (m == m_mixes.front().get());
    if (p.contains("name")) m->name = p.value("name").toString(id).left(60);
    if (p.value("reset").toBool(false)) { m->stripGain.clear(); m->stripMute.clear(); }
    if (p.contains("follow")) m->follow = p.value("follow").toBool(true);
    if (p.value("master").isObject()) { const double g = lin(p.value("master").toObject().value("gainDb").toDouble(0.0)); if (main) m_masterGain = g; else m->masterGain = g; }
    if (p.contains("solo")) { if (main) m_solo = p.value("solo").toString(); else m->solo = p.value("solo").toString(); }
    const QJsonObject strips = p.value("strips").toObject();
    for (auto it = strips.begin(); it != strips.end(); ++it) {
        const QJsonObject st = it.value().toObject();
        const QString strip = it.key();
        if (main) {
            if (strip == "program") { if (st.contains("gainDb")) m_programGain = lin(st.value("gainDb").toDouble()); if (st.contains("mute")) m_programMute = st.value("mute").toBool(); }
            else if (strip.startsWith('L')) {
                auto lit = m_layers.find(strip.mid(1).toInt());
                if (lit != m_layers.end()) { if (st.contains("gainDb")) lit->second.spec.audioGain = lin(st.value("gainDb").toDouble()); if (st.contains("mute")) lit->second.spec.audioMute = st.value("mute").toBool(); }
            }
        } else {
            if (st.contains("gainDb")) m->stripGain[strip] = lin(st.value("gainDb").toDouble());
            if (st.contains("mute")) m->stripMute[strip] = st.value("mute").toBool();
            if (st.value("follow").toBool(false)) { m->stripGain.erase(strip); m->stripMute.erase(strip); }
        }
    }
}

void Compositor::removeMix(const QString& id) {
    QMutexLocker l(&m_mutex);
    if (id == "main") return;
    for (auto it = m_mixes.begin(); it != m_mixes.end(); ++it) if ((*it)->id == id) { m_mixes.erase(it); return; }
}

/// The fader of `strip` in `mix` (linear) and whether it is muted, before solo.
double Compositor::stripGain(const MixState& mix, const QString& strip, bool* mute, bool* explicitSet) const {
    double g = 1.0; bool mu = false, ex = false;
    if (strip == programStrip()) { g = m_programGain; mu = m_programMute; }
    else if (strip.startsWith('L')) { auto it = m_layers.find(QStringView(strip).mid(1).toInt()); if (it != m_layers.end()) { g = it->second.spec.audioGain; mu = it->second.spec.audioMute; } }
    if (&mix != m_mixes.front().get()) {
        if (!mix.follow) { g = 1.0; mu = false; }
        auto gi = mix.stripGain.find(strip); if (gi != mix.stripGain.end()) { g = gi->second; ex = true; }
        auto mi = mix.stripMute.find(strip); if (mi != mix.stripMute.end()) { mu = mi->second; ex = true; }
    }
    if (mute) *mute = mu;
    if (explicitSet) *explicitSet = ex;
    return g;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

FramePtr Compositor::silentAudio(qint64 frame) {
    const int n = int(m_fmt.rate.samplesForFrame(frame, m_fmt.sampleRate));
    FramePtr out = audioFrame(n);
    if (out) { silenceAudioFrame(out); out->pts = m_fmt.rate.sampleBoundary(frame, m_fmt.sampleRate); }
    return out;
}

/// Pull every source's sound once (decoders and live FIFOs must drain exactly one frame per tick, however many mixes
/// listen), with transition / animation gains applied, and meter each pre-fader.
void Compositor::pullAudio(qint64 frame, double programPosSec) {
    // PE-13: last tick's entries are recycled (their peak / rms vectors keep their capacity); their buffers go back now.
    for (SrcAudio& s : m_srcAudio) { s.buf.reset(); if (m_poolAudio) m_srcSpare.push_back(std::move(s)); }
    m_srcAudio.clear();
    auto newSrc = [&]() {
        SrcAudio s;
        if (!m_srcSpare.empty()) { s = std::move(m_srcSpare.back()); m_srcSpare.pop_back(); s.layer = 0; s.key = kProgramKey; s.duck = s.audible = false; }
        return s;
    };
    const int n = int(m_fmt.rate.samplesForFrame(frame, m_fmt.sampleRate));
    auto meterSrc = [&](SrcAudio& sa) {
        sa.prePeak.assign(size_t(m_fmt.channels), 0.0f); sa.preRms.assign(size_t(m_fmt.channels), 0.0f);
        double e = 0.0;
        for (int c = 0; c < m_fmt.channels; ++c) {
            const float* x = reinterpret_cast<const float*>(sa.buf->data[c]); float pk = 0.0f; double sq = 0.0;
            for (int i = 0; i < n; ++i) { const float a = x[i] < 0 ? -x[i] : x[i]; if (a > pk) pk = a; sq += double(x[i]) * x[i]; }
            sa.prePeak[size_t(c)] = pk; sa.preRms[size_t(c)] = float(std::sqrt(sq / qMax(1, n))); e += sq;
        }
        e /= qMax(1, n * m_fmt.channels);
        sa.audible = (e > 1e-12 ? 10.0 * std::log10(e) : -120.0) > m_duck.thresholdDbfs;
    };
    auto scaled = [&](FramePtr& dst, const FramePtr& src, double g, bool add) {
        for (int c = 0; c < m_fmt.channels; ++c) {
            float* o = reinterpret_cast<float*>(dst->data[c]); const float* x = reinterpret_cast<const float*>(src->data[c]);
            if (add) for (int i = 0; i < n; ++i) o[i] += x[i] * float(g); else for (int i = 0; i < n; ++i) o[i] = x[i] * float(g);
        }
    };

    // Program: A (and B with crossfade gains during a transition) into one pre-fader buffer.
    {
        SrcAudio sa = newSrc(); sa.strip = programStrip();
        FramePtr tmp = audioFrame(n);
        sa.buf = audioFrame(n);
        if (tmp && sa.buf) {
            silenceAudioFrame(sa.buf);
            if (m_programAudio && !m_programHold) {
                const bool inTrans = (m_progB || m_progClearing);
                const double p = inTrans ? ease(progress(frame, m_progTransStart, m_progTrans, m_fmt.rate)) : 0.0;
                if (m_progA && m_progA->hasAudio()) {
                    silenceAudioFrame(tmp);
                    const double posA = inTrans ? m_progPosA + m_fmt.rate.frameToNs(frame - m_progTransStart) / 1e9 : programPosSec;
                    if (m_progA->audio(tmp, frame, posA)) scaled(sa.buf, tmp, inTrans ? 1.0 - p : 1.0, true);
                }
                if (m_progB && m_progB->hasAudio()) {
                    silenceAudioFrame(tmp);
                    if (m_progB->audio(tmp, frame, programPosSec)) scaled(sa.buf, tmp, p, true);
                }
            } else if (m_slate && m_slateVisible && !m_progA && m_slate->hasAudio()) {
                silenceAudioFrame(tmp);
                if (m_slate->audio(tmp, frame, 0.0)) scaled(sa.buf, tmp, 1.0, true);
            }
            meterSrc(sa);
            m_srcAudio.push_back(std::move(sa));
        }
    }
    // Layers.
    for (auto& kv : m_layers) {
        Layer& layer = kv.second;
        layer.audible = false;
        if (layer.state == LayerState::Hidden || !layer.src || !layer.src->hasAudio()) continue;
        if (layer.strip.isEmpty()) layer.strip = QStringLiteral("L") + QString::number(kv.first);
        SrcAudio sa = newSrc(); sa.strip = layer.strip; sa.layer = kv.first; sa.key = kv.first; sa.duck = layer.spec.duck;
        sa.buf = audioFrame(n);
        if (!sa.buf) { if (m_poolAudio) m_srcSpare.push_back(std::move(sa)); continue; }
        silenceAudioFrame(sa.buf);
        const double pos = layer.src->wantsProgramTime() ? programPosSec : m_fmt.rate.frameToNs(frame - layer.shownFrame) / 1e9;
        bool got = false;
        { tickprof::SourceCall sc(kv.first, layer.src.get(), "audio"); got = layer.src->audio(sa.buf, frame, pos); }
        if (!got) { meterSrc(sa); m_srcAudio.push_back(std::move(sa)); continue; }
        double g = 1.0;
        if (layer.state == LayerState::AnimIn) g = ease(progress(frame, layer.animStart, layer.anim, m_fmt.rate));
        else if (layer.state == LayerState::AnimOut) g = 1.0 - ease(progress(frame, layer.animStart, layer.anim, m_fmt.rate));
        if (g < 1.0) scaled(sa.buf, sa.buf, g, false);
        meterSrc(sa);
        layer.audible = sa.audible;
        m_srcAudio.push_back(std::move(sa));
    }
}

/// One mix's sound from the pulled sources: its faders, mutes, solo, ducker, master, clip and meter.
FramePtr Compositor::mixAudio(MixState& mix, qint64 frame, metersnap::Mix* snap) {
    const int n = int(m_fmt.rate.samplesForFrame(frame, m_fmt.sampleRate));
    FramePtr out = silentAudio(frame);
    if (!out) return nullptr;
    const bool isMain = (&mix == m_mixes.front().get());
    const QString& solo = isMain ? m_solo : mix.solo;
    auto inMix = [&](const SrcAudio& sa) {
        if (sa.layer == 0) return true;
        auto it = m_layers.find(sa.layer); return it != m_layers.end() && it->second.spec.inMix(mix.id);
    };
    // Ducker: program dips while a `duck` layer that is in this mix and not muted here is audible.
    bool duckActive = false;
    for (const SrcAudio& sa : m_srcAudio) {
        if (!sa.duck || !sa.audible || !inMix(sa)) continue;
        bool mu = false; stripGain(mix, sa.strip, &mu);
        if (!mu) duckActive = true;
    }
    const double target = duckActive ? std::pow(10.0, m_duck.depthDb / 20.0) : 1.0;
    const double frameSec = m_fmt.rate.frameDurationNs() / 1e9;
    const double tc = (target < mix.duckGain) ? m_duck.attackSec : m_duck.releaseSec;
    mix.duckGain += (target - mix.duckGain) * (tc > 0.0 ? 1.0 - std::exp(-frameSec / tc) : 1.0);

    const int snapCh = qMin(m_fmt.channels, metersnap::kMaxChannels);
    for (size_t si = 0; si < m_srcAudio.size(); ++si) {
        const SrcAudio& sa = m_srcAudio[si];
        std::vector<float>& pk = mix.stripPeak[sa.key];
        pk.assign(size_t(m_fmt.channels), 0.0f);
        const bool in = inMix(sa);
        // PE-05 snapshot: the strip as metersJson reports it (its fader before solo / item mute / ducking, also for a
        // source that is not in this mix) and its post-fader peak below.
        metersnap::MixStrip* ms = (snap && si < size_t(metersnap::kMaxSources)) ? &snap->strips[si] : nullptr;
        bool mu = false, ex = false;
        const double fader = (in || ms) ? stripGain(mix, sa.strip, &mu, &ex) : 1.0;
        if (ms) { ms->gain = fader; ms->mute = mu; ms->explicitSet = ex; ms->inMix = in; std::fill(ms->peak, ms->peak + snapCh, 0.0f); }
        if (!in) continue;
        double g = fader;
        if (mu || (!solo.isEmpty() && solo != sa.strip)) g = 0.0;
        if (sa.layer == 0) { if (m_progItemMute.load()) g = 0.0; g *= mix.duckGain; }
        if (g <= 0.0) continue;
        for (int c = 0; c < m_fmt.channels; ++c) {
            float* o = reinterpret_cast<float*>(out->data[c]); const float* x = reinterpret_cast<const float*>(sa.buf->data[c]);
            for (int i = 0; i < n; ++i) o[i] += x[i] * float(g);
            pk[size_t(c)] = sa.prePeak[size_t(c)] * float(g);
            if (ms && c < snapCh) ms->peak[c] = pk[size_t(c)];
        }
    }
    if (isMain && m_legacyStatus) {
        // legacy fields read by the legacy metersJson (PE-05 readers use the snapshot)
        auto pit = mix.stripPeak.find(kProgramKey); m_progPeak = pit != mix.stripPeak.end() ? pit->second : std::vector<float>(size_t(m_fmt.channels), 0.0f);
        for (auto& kv : m_layers) { auto it = mix.stripPeak.find(kv.first); kv.second.peak = it != mix.stripPeak.end() ? it->second : std::vector<float>(size_t(m_fmt.channels), 0.0f); }
    }
    // Master gain, soft clip, meter.
    const double master = isMain ? m_masterGain : mix.masterGain;
    std::vector<const float*>& planes = m_planes;      // PE-13: reused, no allocation per mix per tick
    planes.assign(size_t(m_fmt.channels), nullptr);
    if (int(mix.chPeak.size()) != m_fmt.channels) mix.chPeak.assign(size_t(m_fmt.channels), 0.0f);
    for (int c = 0; c < m_fmt.channels; ++c) {
        float* o = reinterpret_cast<float*>(out->data[c]);
        float pk = 0.0f;
        for (int i = 0; i < n; ++i) {
            float v = o[i] * float(master);
            if (v > 1.0f || v < -1.0f) { ++mix.clips; v = v > 0 ? 1.0f : -1.0f; }
            o[i] = v;
            const float a = v < 0 ? -v : v; if (a > pk) pk = a;
        }
        mix.chPeak[size_t(c)] = pk;
        planes[size_t(c)] = o;
    }
    mix.meter.process(planes.data(), n);
    return out;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

/// The picture of a result: CPU frames as they are, GPU tickets read back now (blocking only if the GPU is behind).
FramePtr Compositor::resolve(Result& r) {
    if (r.video || r.ticket < 0) return r.video;
    tickprof::ScopedPhase fetchPhase(tickprof::Fetch);
    FramePtr out = m_pool.get();
    QElapsedTimer t; t.start();
    const bool ok = out && m_gpu && m_gpu->fetch(r.ticket, out);
    m_tickFetchMs += t.nsecsElapsed() / 1e6;
    if (ok) { r.video = out; r.ticket = -1; return out; }
    if (m_gpu) m_gpu->release(r.ticket);
    r.ticket = -1;
    r.video = blackFrame();
    return r.video;
}

MediaFrame Compositor::render(qint64 frame, double programPosSec, std::map<QString, MediaFrame>* mixesOut) {
    // PE-06 phases (no-ops unless the engine bound its tick profiler to this thread). Lock = a reader holding m_mutex.
    using tickprof::ScopedPhase;
    ScopedPhase lockWait(tickprof::Lock);
    QMutexLocker l(&m_mutex);
    lockWait.stop();
    // PE-05: sources parked while a reader was inside their status(), update()s that waited for it. Both only tryLock the
    // readers' mutex (never wait) and are usually empty.
    if (!m_legacyStatus) { flushGraveyard(); applyDeferredUpdates(); }
    // While the program is held (pause) the picture is frozen; so are the layers that follow the program
    // position (karaoke captions), instead of running on with the position the engine keeps counting.
    if (m_programHold) { if (!m_holdActive) { m_holdActive = true; m_holdPos = m_lastProgramPos; } programPosSec = m_holdPos; }
    else m_holdActive = false;
    const bool inTrans = (m_progB != nullptr) || m_progClearing;

    // Pipelined: read the oldest tick's pictures back FIRST, before this tick's uploads and draws are queued (mapping a
    // staging texture behind new work makes the driver wait for it).
    m_tickFetchMs = 0;
    PendingTick due;
    bool haveDue = false;
    if (m_depth > 0 && int(m_pending.size()) >= m_depth) {
        due = std::move(m_pending.front()); m_pending.pop_front(); haveDue = true;
        for (Result& r : due.results) resolve(r);
    }

    // Draw lists: the program bus and the layer stack are evaluated once (sources advance once per tick); each mix
    // then takes the program plus the layers that are in it.
    std::vector<DrawItem> progDraws, layerDraws;
    FramePtr passthrough;
    { ScopedPhase ph(tickprof::Program); buildProgramDraws(frame, programPosSec, progDraws, &passthrough); }
    { ScopedPhase ph(tickprof::Layers); buildLayerDraws(frame, programPosSec, layerDraws); }

    std::vector<Result> results;
    std::vector<std::vector<int>> resultKeys;          // the layer ids each result was drawn with
    int composed = 0; double composedGpuMs = 0, composedCpuMs = 0;    // auto path accounting for this tick
    std::vector<int> mixResult(m_mixes.size(), -1);
    ScopedPhase compositePhase(tickprof::Composite);
    for (size_t mi = 0; mi < m_mixes.size(); ++mi) {
        const QString& mid = m_mixes[mi]->id;
        std::vector<int> key;
        for (const DrawItem& d : layerDraws) { auto it = m_layers.find(d.layer); if (it != m_layers.end() && it->second.spec.inMix(mid)) key.push_back(d.layer); }
        for (size_t r = 0; r < resultKeys.size(); ++r) if (resultKeys[r] == key) { mixResult[mi] = int(r); break; }
        if (mixResult[mi] >= 0) continue;                  // same picture as an earlier mix: shared
        std::vector<DrawItem> draws = progDraws;
        for (const DrawItem& d : layerDraws) if (std::find(key.begin(), key.end(), d.layer) != key.end()) draws.push_back(d);
        Result res;
        // Zero copy only when the source picture already is the raster: a live source (NDI / SDI / stream) arrives at its own
        // size (a 1280x720 NDI feed on a 1080p channel) and must be drawn scaled, or every output gets the wrong size.
        const bool rasterSized = passthrough && passthrough->width == m_fmt.width && passthrough->height == m_fmt.height && passthrough->format == kMixerPixelFormat;
        const FramePtr pass = draws.size() == 1 && rasterSized ? passthrough : FramePtr();
        QElapsedTimer ct; ct.start();
        if (pass) res.video = pass;                                   // zero copy
        else if (m_gpu && (!m_gpuAuto || m_useGpu)) {
            std::vector<GpuCompositor::DrawOp> ops; ops.reserve(draws.size());
            for (const auto& d : draws) ops.push_back({d.frame, d.dst, d.srcRect, d.opacity, d.clip, d.premultiplied, d.upload});
            res.ticket = m_gpu->submit(ops);
            if (res.ticket < 0) { qWarning() << "[Compositor] GPU submit failed; falling back to CPU"; m_gpu.reset(); res.video = renderCpu(draws, pass); }
            else composedGpuMs += ct.nsecsElapsed() / 1e6;
            composed++;
        } else { res.video = renderCpu(draws, pass); if (m_gpu) { composedCpuMs += ct.nsecsElapsed() / 1e6; composed++; } }
        results.push_back(std::move(res));
        resultKeys.push_back(std::move(key));
        mixResult[mi] = int(results.size() - 1);
    }
    compositePhase.stop();

    // Auto path: charge this tick's compositing (GPU: submits now + the readbacks done at the top of this tick).
    if (m_gpu && m_gpuAuto && composed > 0) {
        if (m_useGpu) autoAccount(true, composedGpuMs + m_tickFetchMs);
        else autoAccount(false, composedCpuMs);
    }

    std::vector<FramePtr> audio(m_mixes.size());
    {
        ScopedPhase ph(tickprof::Audio);
        pullAudio(frame, programPosSec);
        for (size_t mi = 0; mi < m_mixes.size(); ++mi)
            audio[mi] = mixAudio(*m_mixes[mi], frame, (m_snap && mi < size_t(metersnap::kMaxMixes)) ? &m_snap->stage.mixes[mi] : nullptr);
        if (m_snap) publishSnapshot(frame);           // PE-05: the readers' copy of this tick's meters
    }
    if (!inTrans || (m_progB == nullptr && !m_progClearing)) m_lastProgramPos = programPosSec;

    if (m_capture) {
        SceneCapture capture; capture.frame = frame; capture.audio = audio.front();
        auto add = [&](const DrawItem& d, int layer) { capture.draws.push_back({d.frame, d.dst, d.srcRect, d.clip, d.opacity, layer, d.premultiplied}); };
        for (const auto& d : progDraws) add(d, 0);
        for (const auto& d : layerDraws) { auto it = m_layers.find(d.layer); if (it != m_layers.end() && it->second.spec.inMix("main")) add(d, d.layer); }
        QJsonObject strips;
        const auto& main = *m_mixes.front();
        for (const auto& a : m_srcAudio) {
            capture.tracks.push_back({a.strip, a.buf});
            bool muted = false; double gain = stripGain(main, a.strip, &muted);
            if (a.layer != 0) { auto it = m_layers.find(a.layer); if (it == m_layers.end() || !it->second.spec.inMix("main")) gain = 0; }
            if (muted || (!m_solo.isEmpty() && m_solo != a.strip)) gain = 0;
            if (a.layer == 0) gain *= m_progItemMute.load() ? 0.0 : main.duckGain;
            strips[a.strip] = gain;
        }
        capture.controls = {{"masterGain", m_masterGain}, {"strips", strips}};
        m_capture(std::move(capture));
    }

    auto emitMix = [&](size_t mi, const FramePtr& v, const FramePtr& a) {
        MediaFrame mf; mf.frameNumber = frame; mf.video = v; mf.audio = a;
        return mf;
    };
    MediaFrame mainOut;
    if (m_depth == 0) {
        for (size_t mi = 0; mi < m_mixes.size(); ++mi) {
            MediaFrame mf = emitMix(mi, resolve(results[size_t(mixResult[mi])]), audio[mi]);
            if (mi == 0) mainOut = mf; else if (mixesOut) (*mixesOut)[m_mixes[mi]->id] = mf;
        }
        return mainOut;
    }
    // Pipelined: hand out what each mix produced `m_depth` ticks ago (its GPU readback long done), queue this tick's.
    for (size_t mi = 0; mi < m_mixes.size(); ++mi) {
        const MixState& m = *m_mixes[mi];
        FramePtr v, a;
        auto it = haveDue ? due.mixes.find(m.id) : due.mixes.end();
        if (it != due.mixes.end() && it->second.first >= 0 && size_t(it->second.first) < due.results.size()) { v = due.results[size_t(it->second.first)].video; a = it->second.second; }
        if (!v) v = blackFrame();
        if (!a) a = silentAudio(frame - m_depth);          // start-up (or a mix added since): black and silence
        MediaFrame mf = emitMix(mi, v, a);
        if (mi == 0) mainOut = mf; else if (mixesOut) (*mixesOut)[m.id] = mf;
    }
    PendingTick cur; cur.frame = frame; cur.results = std::move(results);
    for (size_t mi = 0; mi < m_mixes.size(); ++mi) cur.mixes[m_mixes[mi]->id] = {mixResult[mi], audio[mi]};
    m_pending.push_back(std::move(cur));
    while (int(m_pending.size()) > m_depth) {              // depth lowered at runtime: drop the oldest, releasing GPU tickets
        for (Result& r : m_pending.front().results) if (r.ticket >= 0 && m_gpu) m_gpu->release(r.ticket);
        m_pending.pop_front();
    }
    return mainOut;
}

// ---------------------------------------------------------------------------
// PE-05: meter snapshot and the lock-free readers
// ---------------------------------------------------------------------------

/// End of the audio phase (m_mutex held): this tick's sources and mix buses into the staging snapshot (mixAudio already
/// wrote each mix's strips), then one seqlock store of the part in use (~7 KB with one mix, about a microsecond).
void Compositor::publishSnapshot(qint64 frame) {
    metersnap::Snapshot& s = m_snap->stage;
    const int ch = qBound(0, m_fmt.channels, metersnap::kMaxChannels);
    s.frame = frame;
    s.channels = ch;
    s.nSources = int(qMin(m_srcAudio.size(), size_t(metersnap::kMaxSources)));
    for (int i = 0; i < s.nSources; ++i) {
        const SrcAudio& sa = m_srcAudio[size_t(i)];
        metersnap::Source& d = s.sources[i];
        d.layer = sa.layer; d.audible = sa.audible ? 1 : 0; d.duck = sa.duck ? 1 : 0;
        for (int c = 0; c < ch; ++c) {
            d.prePeak[c] = c < int(sa.prePeak.size()) ? sa.prePeak[size_t(c)] : 0.0f;
            d.preRms[c] = c < int(sa.preRms.size()) ? sa.preRms[size_t(c)] : 0.0f;
        }
    }
    s.nMixes = int(qMin(m_mixes.size(), size_t(metersnap::kMaxMixes)));
    for (int mi = 0; mi < s.nMixes; ++mi) {
        const MixState& m = *m_mixes[size_t(mi)];
        metersnap::Mix& d = s.mixes[mi];
        d.serial = m.serial;
        d.momentaryLufs = m.meter.momentaryLufs(); d.shortTermLufs = m.meter.shortTermLufs(); d.peakDbfs = m.meter.peakDbfs();
        d.duckGain = m.duckGain; d.clips = double(m.clips);
        for (int c = 0; c < ch; ++c) d.chPeak[c] = c < int(m.chPeak.size()) ? m.chPeak[size_t(c)] : 0.0f;
    }
    // Integrated loudness scans the whole gated block history (up to an hour, 36000 blocks): once a second at most, and
    // only when a block was added. status() used to do that scan on every call, under the lock.
    const LoudnessMeter& meter = m_mixes.front()->meter;
    const qint64 blocks = meter.blocks();
    const qint64 every = qMax<qint64>(1, qRound64(m_fmt.rate.fps()));
    if (blocks != s.integratedBlocks && (s.integratedFrame < 0 || frame < s.integratedFrame || frame - s.integratedFrame >= every)) {
        s.integratedLufs = meter.integratedLufs(); s.integratedBlocks = blocks; s.integratedFrame = frame;
    }
    m_snap->lock.store(s, metersnap::Snapshot::bytesFor(s.nMixes));
}

void Compositor::loadSnapshot(metersnap::Snapshot& out) const {
    if (m_snap) m_snap->lock.load(out);
    else out = metersnap::Snapshot();
}

static QString snapStripName(int layer) { return layer == 0 ? QStringLiteral("program") : "L" + QString::number(layer); }

QJsonObject Compositor::metersJson() const {
    if (m_legacyStatus) return metersJsonLegacy();
    // The measured sound: the last tick's snapshot, copied without any lock (it never waits for render, never delays it).
    auto snapBuf = std::make_unique<metersnap::Snapshot>();      // ~26 KB: heap, not the caller's stack
    const metersnap::Snapshot& snap = *snapBuf;
    loadSnapshot(*snapBuf);
    // Names, faders and the layer list: copied under a short lock. Source status() calls: outside it.
    StatusCtl c;
    c.layers.reserve(16); c.mixes.reserve(metersnap::kMaxMixes);
    std::vector<std::pair<int, bool>> connected;                    // layer -> its source's status().connected
    {
        QMutexLocker s(&m_statusMutex);
        { QMutexLocker l(&m_mutex); copyStatusCtl(c, CtlLayers | CtlMixes); }
        for (const StatusCtl::L& L : c.layers)
            if (L.src && L.state != LayerState::Hidden && snap.findSource(L.id) >= 0) connected.push_back({L.id, L.src->status().value("connected").toBool(true)});
    }

    const int ch = snap.channels;
    auto db = [](double lin) { return lin > 1e-6 ? 20.0 * std::log10(lin) : -100.0; };
    auto peaksOf = [&](const float* v, int count) { QJsonArray a; for (int i = 0; i < count; ++i) a.append(db(double(v[i]))); return a; };
    static const float kZeros[metersnap::kMaxChannels] = {};
    const metersnap::Mix defaults{};                                 // a mix not rendered yet: the meters' initial values
    const metersnap::Mix* mainSnap = c.mixes.empty() ? nullptr : snap.findMix(c.mixes.front().serial);
    const metersnap::Mix& main = mainSnap ? *mainSnap : defaults;

    QJsonObject o;
    QJsonArray peaks;
    if (mainSnap) for (int i = 0; i < ch; ++i) { const float p = main.chPeak[i]; peaks.append(p > 1e-6f ? 20.0 * std::log10(double(p)) : -100.0); }
    o["channelPeaksDbfs"] = peaks;
    o["momentaryLufs"] = main.momentaryLufs; o["shortTermLufs"] = main.shortTermLufs;
    o["peakDbfs"] = main.peakDbfs; o["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, main.duckGain));
    o["transitioning"] = c.transitioning;
    // mixer strips: the program item's audio and every audio-capable layer (line-ins, PiP sources)
    QJsonArray strips;
    {
        QJsonObject s; s["id"] = "program"; s["name"] = "Program"; s["kind"] = "program";
        s["gainDb"] = db(c.programGain); s["mute"] = c.programMute; s["itemMute"] = c.progItemMute; s["solo"] = (c.solo == "program");
        const int pi = snap.findSource(0);
        s["peaks"] = !mainSnap ? QJsonArray() : peaksOf(pi >= 0 ? main.strips[pi].peak : kZeros, ch);
        strips.append(s);
    }
    for (const StatusCtl::L& L : c.layers) {
        const int si = snap.findSource(L.id);                       // pulled audio last tick = has audio and is not hidden
        if (!L.src || L.state == LayerState::Hidden || si < 0) continue;
        QJsonObject s; s["id"] = "L" + QString::number(L.id); s["layer"] = L.id; s["role"] = L.spec.role; s["kind"] = L.spec.role == "audio" ? "line" : "layer";
        s["name"] = L.spec.source.value("label").toString(L.spec.source.value("device").toString(L.spec.source.value("name").toString(L.spec.source.value("type").toString("layer"))));
        s["gainDb"] = db(L.spec.audioGain); s["mute"] = L.spec.audioMute; s["solo"] = (c.solo == s["id"].toString());
        s["peaks"] = mainSnap ? peaksOf(main.strips[si].peak, ch) : QJsonArray();
        for (const auto& cn : connected) if (cn.first == L.id) { s["connected"] = cn.second; break; }
        strips.append(s);
    }
    // Pre-fader level of every source (the per-source VU), the same for every mix.
    QJsonObject pre;
    for (int i = 0; i < snap.nSources; ++i) {
        const metersnap::Source& sa = snap.sources[i];
        QJsonObject v; v["peaks"] = peaksOf(sa.prePeak, ch); v["rms"] = peaksOf(sa.preRms, ch); v["audible"] = sa.audible != 0;
        pre[snapStripName(sa.layer)] = v;
    }
    for (int i = 0; i < strips.size(); ++i) { QJsonObject s = strips[i].toObject(); s["pre"] = pre.value(s["id"].toString()).toObject(); strips[i] = s; }
    o["strips"] = strips; o["solo"] = c.solo; o["masterGainDb"] = db(c.masterGain);
    o["sources"] = pre;
    // Every mix: its own faders, post-fader strip peaks, bus peaks and loudness.
    QJsonArray mixes;
    for (size_t mi = 0; mi < c.mixes.size(); ++mi) {
        const StatusCtl::M& m = c.mixes[mi];
        const bool isMain = (mi == 0);
        const metersnap::Mix* sm = snap.findMix(m.serial);
        const metersnap::Mix& mm = sm ? *sm : defaults;
        QJsonObject mo; mo["id"] = m.id; mo["name"] = m.name; mo["follow"] = m.follow;
        QJsonArray cp; if (sm) for (int i = 0; i < ch; ++i) cp.append(db(double(mm.chPeak[i])));
        mo["channelPeaksDbfs"] = cp; mo["momentaryLufs"] = mm.momentaryLufs; mo["shortTermLufs"] = mm.shortTermLufs;
        mo["masterGainDb"] = db(isMain ? c.masterGain : m.masterGain); mo["solo"] = isMain ? c.solo : m.solo;
        mo["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, mm.duckGain)); mo["clips"] = mm.clips;
        QJsonArray ms;
        if (sm) for (int i = 0; i < snap.nSources; ++i) {
            const metersnap::MixStrip& st = mm.strips[i];
            QJsonObject s; s["id"] = snapStripName(snap.sources[i].layer);
            s["gainDb"] = db(st.gain); s["mute"] = st.mute != 0;
            s["explicit"] = st.explicitSet != 0;
            s["inMix"] = st.inMix != 0;
            s["peaks"] = peaksOf(st.peak, ch);
            ms.append(s);
        }
        mo["strips"] = ms;
        mixes.append(mo);
    }
    o["mixes"] = mixes;
    return o;
}

QJsonObject Compositor::status() const {
    if (m_legacyStatus) return statusLegacy();
    StatusCtl c;
    c.mixes.reserve(metersnap::kMaxMixes);
    QJsonObject program, incoming;
    {
        QMutexLocker s(&m_statusMutex);         // the program sources stay alive until we are done asking them
        { QMutexLocker l(&m_mutex); copyStatusCtl(c, CtlMixes | CtlProgram | CtlGpu); }
        if (c.progA) program = c.progA->status();
        if (c.progB) incoming = c.progB->status();
    }
    auto snapBuf = std::make_unique<metersnap::Snapshot>();
    loadSnapshot(*snapBuf);
    const metersnap::Snapshot& snap = *snapBuf;
    QJsonObject o;
    o["program"] = program;
    o["incoming"] = incoming;
    o["transitioning"] = c.transitioning;
    o["layers"] = c.layerCount;
    o["gpu"] = c.gpu;
    if (c.gpu) { o["gpuAdapter"] = c.gpuAdapter; o["gpuRenderMs"] = c.gpuCounters.lastRenderMs; o["gpuFetchMs"] = c.gpuCounters.lastFetchMs; o["gpuStats"] = GpuCompositor::countersJson(c.gpuCounters); }
    o["pipelineDepth"] = c.depth;
    if (c.gpu) {
        QJsonObject a; a["auto"] = c.gpuAuto; a["path"] = (!c.gpuAuto || c.useGpu) ? "gpu" : "cpu"; a["phase"] = c.autoPhase;
        a["gpuMsPerFrame"] = c.gpuCostMs; a["cpuMsPerFrame"] = c.cpuCostMs;
        o["composite"] = a;
    }
    QJsonArray mixIds; for (const StatusCtl::M& m : c.mixes) mixIds.append(m.id);
    o["mixes"] = mixIds;
    const metersnap::Mix defaults{};
    const metersnap::Mix* mainSnap = c.mixes.empty() ? nullptr : snap.findMix(c.mixes.front().serial);
    const metersnap::Mix& main = mainSnap ? *mainSnap : defaults;
    QJsonObject audio;
    audio["momentaryLufs"] = main.momentaryLufs; audio["shortTermLufs"] = main.shortTermLufs;
    audio["integratedLufs"] = snap.integratedLufs; audio["peakDbfs"] = main.peakDbfs;
    audio["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, main.duckGain)); audio["clips"] = main.clips;
    o["audio"] = audio;
    return o;
}

// The pre-PE-05 readers, kept verbatim for PLAYPLAY_COMP_LEGACY_STATUS=1 (A/B runs): they hold m_mutex while they build
// the JSON and while they call src->status(), so render waits for them.
QJsonObject Compositor::metersJsonLegacy() const {
    QMutexLocker l(&m_mutex);
    QJsonObject o;
    QJsonArray peaks;
    for (float p : m_mixes.front()->chPeak) peaks.append(p > 1e-6f ? 20.0 * std::log10(double(p)) : -100.0);
    o["channelPeaksDbfs"] = peaks;
    const MixState& main = *m_mixes.front();
    o["momentaryLufs"] = main.meter.momentaryLufs(); o["shortTermLufs"] = main.meter.shortTermLufs();
    o["peakDbfs"] = main.meter.peakDbfs(); o["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, main.duckGain));
    o["transitioning"] = (m_progB != nullptr) || m_progClearing;
    // mixer strips: the program item's audio and every audio-capable layer (line-ins, PiP sources)
    auto db = [](double lin) { return lin > 1e-6 ? 20.0 * std::log10(lin) : -100.0; };
    auto peaksOf = [&](const std::vector<float>& v) { QJsonArray a; for (float p : v) a.append(db(double(p))); return a; };
    QJsonArray strips;
    {
        QJsonObject s; s["id"] = "program"; s["name"] = "Program"; s["kind"] = "program";
        s["gainDb"] = db(m_programGain); s["mute"] = m_programMute; s["itemMute"] = m_progItemMute.load(); s["solo"] = (m_solo == "program");
        s["peaks"] = peaksOf(m_progPeak);
        strips.append(s);
    }
    for (const auto& kv : m_layers) {
        const Layer& L = kv.second;
        if (!L.src || !L.src->hasAudio() || L.state == LayerState::Hidden) continue;
        QJsonObject s; s["id"] = "L" + QString::number(kv.first); s["layer"] = kv.first; s["role"] = L.spec.role; s["kind"] = L.spec.role == "audio" ? "line" : "layer";
        s["name"] = L.spec.source.value("label").toString(L.spec.source.value("device").toString(L.spec.source.value("name").toString(L.spec.source.value("type").toString("layer"))));
        s["gainDb"] = db(L.spec.audioGain); s["mute"] = L.spec.audioMute; s["solo"] = (m_solo == s["id"].toString());
        s["peaks"] = peaksOf(L.peak);
        if (L.src) s["connected"] = L.src->status().value("connected").toBool(true);
        strips.append(s);
    }
    // Pre-fader level of every source (the per-source VU), the same for every mix.
    QJsonObject pre;
    for (const SrcAudio& sa : m_srcAudio) { QJsonObject v; v["peaks"] = peaksOf(sa.prePeak); v["rms"] = peaksOf(sa.preRms); v["audible"] = sa.audible; pre[sa.strip] = v; }
    for (int i = 0; i < strips.size(); ++i) { QJsonObject s = strips[i].toObject(); s["pre"] = pre.value(s["id"].toString()).toObject(); strips[i] = s; }
    o["strips"] = strips; o["solo"] = m_solo; o["masterGainDb"] = db(m_masterGain);
    o["sources"] = pre;
    // Every mix: its own faders, post-fader strip peaks, bus peaks and loudness.
    QJsonArray mixes;
    for (const auto& mp : m_mixes) {
        const MixState& m = *mp;
        const bool isMain = (&m == m_mixes.front().get());
        QJsonObject mo; mo["id"] = m.id; mo["name"] = m.name; mo["follow"] = m.follow;
        QJsonArray cp; for (float p : m.chPeak) cp.append(db(double(p)));
        mo["channelPeaksDbfs"] = cp; mo["momentaryLufs"] = m.meter.momentaryLufs(); mo["shortTermLufs"] = m.meter.shortTermLufs();
        mo["masterGainDb"] = db(isMain ? m_masterGain : m.masterGain); mo["solo"] = isMain ? m_solo : m.solo;
        mo["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, m.duckGain)); mo["clips"] = double(m.clips);
        QJsonArray ms;
        for (const SrcAudio& sa : m_srcAudio) {
            QJsonObject s; s["id"] = sa.strip;
            bool mu = false; s["gainDb"] = db(stripGain(m, sa.strip, &mu)); s["mute"] = mu;
            s["explicit"] = !isMain && (m.stripGain.count(sa.strip) || m.stripMute.count(sa.strip));
            bool in = true; if (sa.layer) { auto it = m_layers.find(sa.layer); in = it != m_layers.end() && it->second.spec.inMix(m.id); }
            s["inMix"] = in;
            auto pit = m.stripPeak.find(sa.key); s["peaks"] = peaksOf(pit != m.stripPeak.end() ? pit->second : std::vector<float>());
            ms.append(s);
        }
        mo["strips"] = ms;
        mixes.append(mo);
    }
    o["mixes"] = mixes;
    return o;
}

QJsonObject Compositor::statusLegacy() const {
    QMutexLocker l(&m_mutex);
    QJsonObject o;
    o["program"] = m_progA ? m_progA->status() : QJsonObject();
    o["incoming"] = m_progB ? m_progB->status() : QJsonObject();
    o["transitioning"] = (m_progB != nullptr) || m_progClearing;
    o["layers"] = int(m_layers.size());
    o["gpu"] = m_gpu != nullptr;
    if (m_gpu) { o["gpuAdapter"] = m_gpu->adapterName(); o["gpuRenderMs"] = m_gpu->lastRenderMs(); o["gpuFetchMs"] = m_gpu->lastFetchMs(); o["gpuStats"] = m_gpu->stats(); }
    o["pipelineDepth"] = m_depth;
    if (m_gpu) {
        QJsonObject a; a["auto"] = m_gpuAuto; a["path"] = (!m_gpuAuto || m_useGpu) ? "gpu" : "cpu"; a["phase"] = m_autoPhase;
        a["gpuMsPerFrame"] = m_gpuCostMs; a["cpuMsPerFrame"] = m_cpuCostMs;
        o["composite"] = a;
    }
    QJsonArray mixIds; for (const auto& m : m_mixes) mixIds.append(m->id);
    o["mixes"] = mixIds;
    const MixState& main = *m_mixes.front();
    QJsonObject audio;
    audio["momentaryLufs"] = main.meter.momentaryLufs(); audio["shortTermLufs"] = main.meter.shortTermLufs();
    audio["integratedLufs"] = main.meter.integratedLufs(); audio["peakDbfs"] = main.meter.peakDbfs();
    audio["duckGainDb"] = 20.0 * std::log10(qMax(1e-6, main.duckGain)); audio["clips"] = double(main.clips);
    o["audio"] = audio;
    return o;
}
