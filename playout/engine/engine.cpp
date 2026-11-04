#include "engine.h"
#include "../supervisor/livestream.h"
#include "decoderpipeline.h"
#include "compositor/compositor.h"
#include "compositor/basicsources.h"
#include <cmath>
#include <chrono>
#include "encoderstage.h"
#include "sinks/ffmpegmuxsink.h"
#include "sinks/ndisink.h"
#include "sinks/nullsink.h"
#include "sinks/dektecasisink.h"
#include "sinks/dektecsdisink.h"
#include "sinks/decklinksink.h"
#include "previewtap.h"
#include "compositor/livesources.h"
#include "../core/frameclock.h"
#include <QHash>
#include <QSet>
#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTimeZone>
#include <QThread>
#include <QDir>
#include <QStandardPaths>
#include <QUuid>

// ---------------------------------------------------------------------------
// Config / Item JSON
// ---------------------------------------------------------------------------

PlayoutEngine::Config PlayoutEngine::Config::fromJson(const QJsonObject& o) {
    Config c;
    c.channelId = o.value("channelId").toString(o.value("id").toString("main"));
    c.channelName = o.value("channelName").toString(o.value("name").toString(c.channelId));
    if (o.contains("frameRate")) {
        const QJsonValue v = o["frameRate"];
        c.frameRate = v.isString() ? FrameRate::fromString(v.toString()) : FrameRate::fromDouble(v.toDouble(29.97));
    }
    c.width = o.value("width").toInt(1920);
    c.height = o.value("height").toInt(1080);
    c.audioChannels = o.value("audioChannels").toInt(2);
    c.sampleRate = o.value("sampleRate").toInt(48000);
    {
        // true / false as before; the string "auto" keeps hw as a candidate and lets the decoder pick by measured cost (PE-12).
        const QJsonValue hv = o.value("hwDecode");
        const QString hs = hv.isString() ? hv.toString().trimmed().toLower() : QString();
        c.hwDecodeAuto = (hs == "auto");
        c.hwDecode = hv.isString() ? (hs != "off" && hs != "false" && hs != "0") : hv.toBool(true);
    }
    c.prerollSec = o.value("prerollSec").toDouble(3.0);
    for (const auto& v : o.value("slateCaptions").toArray()) c.slateCaptions << v.toString();
    c.gpu = o.value("gpu").toBool(false);
    c.ducking = o.value("ducking").toObject();
    c.mixes = o.value("mixes").toArray();
    c.gpuMode = o.value("gpuMode").toString("auto");
    c.clockSource = o.value("clockSource").toString("auto");
    c.stallSkipSec = o.value("stallSkipSec").toDouble(3.0);
    c.identity = o.value("identity").toObject();
    c.outputs = o.value("outputs").toArray();
    return c;
}

QJsonObject PlayoutEngine::Config::toJson() const {
    QJsonObject o;
    o["channelId"] = channelId; o["channelName"] = channelName;
    o["frameRate"] = frameRate.toString(); o["width"] = width; o["height"] = height;
    o["audioChannels"] = audioChannels; o["sampleRate"] = sampleRate; o["hwDecode"] = hwDecodeAuto ? QJsonValue("auto") : QJsonValue(hwDecode);
    o["prerollSec"] = prerollSec; o["outputs"] = outputs;
    QJsonArray caps; for (const auto& s : slateCaptions) caps << s; o["slateCaptions"] = caps;
    o["gpu"] = gpu; if (!ducking.isEmpty()) o["ducking"] = ducking; if (!mixes.isEmpty()) o["mixes"] = mixes; if (!identity.isEmpty()) o["identity"] = identity;
    return o;
}

PlayoutEngine::Item PlayoutEngine::Item::fromJson(const QJsonObject& o) {
    Item i;
    i.id = o.value("id").toString();
    i.assetId = o.value("assetId").toString(i.id);
    if (i.id.isEmpty()) i.id = i.assetId;
    i.path = o.value("path").toString(o.value("filePath").toString());
    i.title = o.value("title").toString(o.value("name").toString(i.assetId));
    i.durationSec = o.value("duration").toDouble(o.value("durationSec").toDouble(0.0));
    i.inPointSec = o.value("inPoint").toDouble(o.value("timeIn").toDouble(0.0));
    i.outPointSec = o.value("outPoint").toDouble(o.value("timeOut").toDouble(0.0));
    i.loop = o.value("loop").toBool(false);
    i.hardStartUtcMs = qint64(o.value("hardStartUtcMs").toDouble(0.0));
    if (i.hardStartUtcMs == 0 && o.contains("hardStart")) {
        const QDateTime dt = QDateTime::fromString(o["hardStart"].toString(), Qt::ISODateWithMs);
        if (dt.isValid()) i.hardStartUtcMs = dt.toMSecsSinceEpoch();
    }
    if (o.value("transition").isObject()) i.transition = o["transition"].toObject();
    else if (o.value("transition").isString()) { i.transition["type"] = o["transition"].toString(); i.transition["durationSec"] = o.value("transitionSec").toDouble(0.5); }
    i.cgOnStart = o.value("cgOnStart").toArray(); i.cgOnEnd = o.value("cgOnEnd").toArray();
    i.audioMute = o.value("audioMute").toBool(false);
    // Live inputs and streams: a LayerSource spec instead of a file. Accept PlayPlayClean's
    // liveSource {ndiName|url|port} and normalise it to {type, name|url|port}.
    i.source = o.value("source").toObject();
    if (i.source.isEmpty() && o.value("liveSource").isObject()) i.source = o.value("liveSource").toObject();
    if (i.source.isEmpty() && i.path.startsWith("ndi://")) { i.source["type"] = "ndi"; i.source["name"] = i.path.mid(6); }
    if (i.source.isEmpty() && i.path.startsWith("sdi://")) { i.source["type"] = "sdi_in"; i.source["port"] = i.path.mid(6).toInt(); }
    if (!i.source.isEmpty()) {
        if (i.source.contains("ndiName") && !i.source.contains("name")) i.source["name"] = i.source["ndiName"];
        if (!i.source.contains("type")) i.source["type"] = i.source.contains("url") ? "stream" : i.source.contains("port") ? "sdi_in" : "ndi";
        if (i.source.value("type").toString() == "stream" && !i.source.contains("realtime")) i.source["realtime"] = true;
    }
    return i;
}

QJsonObject PlayoutEngine::Item::toJson() const {
    QJsonObject o;
    o["id"] = id; o["assetId"] = assetId; o["path"] = path; o["title"] = title;
    o["duration"] = durationSec; o["inPoint"] = inPointSec; o["outPoint"] = outPointSec;
    o["loop"] = loop;
    if (hardStartUtcMs > 0) o["hardStartUtcMs"] = double(hardStartUtcMs);
    if (!transition.isEmpty()) o["transition"] = transition;
    if (!cgOnStart.isEmpty()) o["cgOnStart"] = cgOnStart;
    if (!cgOnEnd.isEmpty()) o["cgOnEnd"] = cgOnEnd;
    if (!source.isEmpty()) o["source"] = source;
    if (audioMute) o["audioMute"] = true;
    return o;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// PE-04 switches, read once per engine. PLAYPLAY_TELEMETRY_MAIN=0 builds the 1 Hz telemetry (output stats, hardening,
// the state JSON) on the clock thread as before; PLAYPLAY_CMD_LOG=0 drops the per-command debug line, `all` logs every
// command (pre-PE-04), unset / 1 keeps at most 10 lines a second and counts the rest.
static bool telemetryOnMainByEnv() {
    const QByteArray v = qgetenv("PLAYPLAY_TELEMETRY_MAIN").trimmed().toLower();
    return !(v == "0" || v == "off" || v == "false" || v == "no");
}
static int cmdLogModeByEnv() {
    const QByteArray v = qgetenv("PLAYPLAY_CMD_LOG").trimmed().toLower();
    if (v == "0" || v == "off" || v == "false" || v == "no") return 0;
    if (v == "all" || v == "2") return 2;
    return 1;
}

PlayoutEngine::PlayoutEngine(const Config& cfg, QObject* parent)
    : QObject(parent), m_cfg(cfg), m_telemetryOnMain(telemetryOnMainByEnv()), m_cmdLogMode(cmdLogModeByEnv())
{
    // PE-06 harness / diagnosis aid, off by default: PLAYPLAY_TICK_PROFILE_LOG=1 logs every telemetry's tickPhases as
    // one compact JSON line. Queued to this object's thread (main), never logged on the clock thread that emits it.
    if (m_tickProf.enabled() && qEnvironmentVariableIntValue("PLAYPLAY_TICK_PROFILE_LOG") > 0)
        connect(this, &PlayoutEngine::telemetry, this, [](const QJsonObject& st) {
            const QJsonObject tp = st.value("telemetry").toObject().value("tickPhases").toObject();
            if (!tp.isEmpty()) qInfo().noquote() << "[Engine] tickPhases" << QJsonDocument(tp).toJson(QJsonDocument::Compact);
        }, Qt::QueuedConnection);
    // PE-14 harness aid, off by default: PLAYPLAY_CLOCK_STATS_LOG=1 logs the frame clock's stats with each telemetry.
    if (qEnvironmentVariableIntValue("PLAYPLAY_CLOCK_STATS_LOG") > 0)
        connect(this, &PlayoutEngine::telemetry, this, [](const QJsonObject& st) {
            const QJsonObject c = st.value("telemetry").toObject().value("clock").toObject();
            if (!c.isEmpty()) qInfo().noquote() << "[Engine] clockStats" << QJsonDocument(c).toJson(QJsonDocument::Compact);
        }, Qt::QueuedConnection);
}

PlayoutEngine::~PlayoutEngine() {
    shutdown();
}

bool PlayoutEngine::initialize() {
    if (m_initialized.load()) return true;
    OutputFormat of;
    of.width = m_cfg.width; of.height = m_cfg.height; of.rate = m_cfg.frameRate;
    of.sampleRate = m_cfg.sampleRate; of.channels = m_cfg.audioChannels;
    m_comp = std::make_unique<Compositor>(of);
    m_comp->setRetireHandler([this](std::unique_ptr<LayerSource> s) { retireSource(std::move(s)); });
    rebuildSlate();
    if (!m_cfg.ducking.isEmpty()) {
        Compositor::DuckSettings d;
        d.depthDb = m_cfg.ducking.value("depthDb").toDouble(d.depthDb); d.attackSec = m_cfg.ducking.value("attackSec").toDouble(d.attackSec);
        d.releaseSec = m_cfg.ducking.value("releaseSec").toDouble(d.releaseSec); d.thresholdDbfs = m_cfg.ducking.value("thresholdDbfs").toDouble(d.thresholdDbfs);
        m_comp->setDucking(d);
    }
    if (m_cfg.gpu) {
        // "gpuMode": "auto" (default) picks the cheaper path on the live content; "on" always composites on the GPU.
        m_comp->setGpuAuto(m_cfg.gpuMode != "on");
        m_comp->setGpuEnabled(true);
        // GPU readback lands later (the clock thread never waits on the GPU); audio is delayed by the same frames.
        // gpuMode "on": 2 frames; auto: 1 (auto usually settles on whichever path is cheaper, and that may be the CPU).
        if (m_comp->gpuActive()) m_comp->setPipelineDepth(m_cfg.gpuMode == "on" ? 2 : 1);
    }

    // Confidence pictures: base 480x270 at ~10 fps for every tile; 640x360 at ~15 fps while a UI boosts it (On Air PGM).
    m_preview = std::make_unique<PreviewTap>();
    m_preview->setChannelFps(m_cfg.frameRate.nominalFps());
    m_preview->setEnabled(true);
    m_monitor = std::make_unique<InputMonitor>();
    m_monitor->setFormat(of);
    m_monitor->setRetireHandler([this](std::unique_ptr<LayerSource> s) { retireSource(std::move(s)); });
    applyMixes(m_cfg.mixes);

    if (!buildOutputs(m_cfg.outputs)) {
        qWarning() << "[Engine] some outputs failed to start";
    }
    m_initialized = true;
    transitionTo("IDLE", "engine_initialized", 0);
    qInfo().noquote() << QString("[Engine] %1 initialised %2x%3 @%4, %5ch audio, %6 encoder stage(s), %7 baseband sink(s)")
        .arg(m_cfg.channelId).arg(m_cfg.width).arg(m_cfg.height).arg(m_cfg.frameRate.toString())
        .arg(m_cfg.audioChannels).arg(m_stages.size()).arg(m_basebandSinks.size());
    return true;
}

void PlayoutEngine::shutdown() {
    if (!m_initialized.load()) return;
    m_initialized = false;
    if (m_comp) m_comp->setCaptureHandler({});
    { QMutexLocker l(&m_recordMutex); if (m_recorder) m_recorder->finish(); }
    discardPrepared();
    if (m_comp) { m_comp->programClear(Compositor::Transition(), m_lastFrame); m_comp->clearLayers(); }
    m_decoder = nullptr;
    m_monitor.reset();           // its receivers close through retireSource, which the wait below covers
    m_mixPreviews.clear();
    waitForBackgroundWork();
    teardownOutputs();
    m_preview.reset();
    m_comp.reset();
    transitionTo("STOPPED", "engine_shutdown", m_lastFrame);
}

// ---------------------------------------------------------------------------
// Outputs: one slot per configured entry, started/stopped individually
// ---------------------------------------------------------------------------

void PlayoutEngine::recordingCommand(const QJsonObject& params) {
    if (!m_comp) return;
    const QString op = params.value("op").toString("start");
    QMutexLocker l(&m_recordMutex);
    if (op == "stop") {
        m_comp->setCaptureHandler({});
        if (m_recorder) m_recorder->finish();
        return;
    }
    if (op != "start") { emit error("Unknown recording operation: " + op); return; }
    if (m_recorder && (m_recorder->status().value("active").toBool() || m_recorder->isRunning())) { emit error("A recording is already running or finalizing"); return; }
    QString directory = params.value("directory").toString();
    if (directory.isEmpty()) directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("recordings/" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8));
    auto recorder = std::make_shared<SceneRecorder>(m_comp->format()); QString why;
    if (!recorder->begin(directory, &why)) { emit error(why); return; }
    m_recorder = recorder;
    m_comp->setCaptureHandler([recorder](SceneCapture c) { recorder->submit(std::move(c)); });
}

bool PlayoutEngine::isBasebandType(const QString& type) {
    return type == "ndi" || type == "dektec_sdi" || type == "sdi" || type == "decklink" || type == "nullbaseband";
}

PlayoutEngine::OutputSlot* PlayoutEngine::slotByName(const QString& name) const {
    for (OutputSlot* s : m_slots) if (s->name == name) return s;
    return nullptr;
}

// Definition fields that matter to the running sink (everything but the runtime policy knobs).
static QJsonObject outputCore(const QJsonObject& def) {
    QJsonObject c = def; c.remove("autoStart"); c.remove("retry"); c.remove("enabled"); c.remove("label"); c.remove("notes");
    return c;
}

// Create the slots for every configured entry and start the ones that want to start with the engine.
// Called with the main thread's lists empty (initialize) — configureOutputs does the incremental diff.
bool PlayoutEngine::buildOutputs(const QJsonArray& outputs) {
    bool allOk = true;
    QSet<QString> names;
    for (const QJsonValue& v : outputs) {
        const QJsonObject o = v.toObject();
        auto* s = new OutputSlot;
        s->def = o; s->type = o.value("type").toString().toLower(); s->url = o.value("url").toString();
        s->name = o.value("name").toString(s->type);
        for (int n = 2; names.contains(s->name); ++n) s->name = o.value("name").toString(s->type) + "-" + QString::number(n);
        names.insert(s->name);
        s->enabled = o.value("enabled").toBool(true); s->autoStart = o.value("autoStart").toBool(true);
        s->state = !s->enabled ? "disabled" : "stopped";
        { QMutexLocker l(&m_outputMutex); m_slots.append(s); }
        if (s->enabled && s->autoStart) { if (!startSlot(s)) allOk = false; }
        else qInfo().noquote() << QString("[Engine] output %1 (%2) defined, %3").arg(s->name, s->type, s->enabled ? "autoStart off" : "disabled");
    }
    return allOk;
}

// Open the sink (and the shared encoder stage when needed) for one slot. Blocking work (encoder open,
// network/hardware) happens outside m_outputMutex; the lists are mutated under it.
bool PlayoutEngine::startSlot(OutputSlot* s) {
    if (s->state == "running") return true;
    { QMutexLocker l(&m_outputMutex); s->state = "starting"; s->manualStop = false; }
    const QJsonObject o = s->def; const QString type = s->type, name = s->name, url = s->url;

    if (isBasebandType(type)) {
        BasebandSink* sink = nullptr;
        if (type == "ndi") {
            auto* ns = new NdiSink(name, o.value("sourceName").toString(o.value("options").toObject().value("sourceName").toString()), o.value("groups").toString(o.value("options").toObject().value("groups").toString()), this);
            ns->setFourcc(o.value("fourcc").toString(o.value("options").toObject().value("fourcc").toString()));   // PE-11; empty: env / bgrx
            sink = ns;
        }
        else if (type == "nullbaseband") sink = new NullBasebandSink(name, this);
        else if (type == "decklink") {
            QJsonObject opts = o.value("options").toObject();
            for (const char* k : {"deviceIndex", "deviceName", "videoMode", "interlaced", "audioChannels", "keyer", "keyLevel", "prerollFrames"})
                if (o.contains(k) && !opts.contains(k)) opts[k] = o[k];
            sink = new DeckLinkSink(name, opts, this);
        } else {
            QJsonObject opts = o.value("options").toObject();
            for (const char* k : {"port", "deviceType", "deviceIndex", "serial", "videoStd", "interlaced", "audioChannels", "configurePort"})
                if (o.contains(k) && !opts.contains(k)) opts[k] = o[k];
            sink = new DekTecSdiSink(name, opts, this);
        }
        connect(sink, &BasebandSink::sinkError, this, [this, name](const QString&, const QString& msg) {
            QMutexLocker l(&m_outputMutex); if (OutputSlot* x = slotByName(name)) x->lastError = msg; l.unlock(); emit error(name + ": " + msg); });
        BasebandSink::Format f; f.width = m_cfg.width; f.height = m_cfg.height; f.rate = m_cfg.frameRate;
        f.sampleRate = m_cfg.sampleRate; f.channels = m_cfg.audioChannels;
        if (!sink->start(f)) { const QString e = sink->stats().lastError; delete sink; scheduleRetry(s, e.isEmpty() ? "sink failed to start" : e); return false; }
        QMutexLocker l(&m_outputMutex);
        s->bsink = sink; m_basebandSinks.append(sink); m_sinkMix.insert(sink, o.value("mix").toString("main").trimmed());
        s->state = "running"; s->lastError.clear(); s->attempts = 0; s->startedMs = QDateTime::currentMSecsSinceEpoch(); s->lastBytes = 0; s->lastBytesMs = s->startedMs; s->stalledSec = 0;
        qInfo().noquote() << QString("[Engine] output %1 (%2) running").arg(name, type);
        return true;
    }

    // Encoded outputs: share an EncoderStage per identical profile.
    EncoderProfile profile;
    const QJsonValue pv = o.value("profile");
    if (pv.isObject()) profile = EncoderProfile::fromJson(pv.toObject());
    else profile = EncoderProfile::preset(pv.toString("h264_aac"));
    profile.audioChannels = m_cfg.audioChannels;
    profile.audioSampleRate = m_cfg.sampleRate;
    // One encoder per (mix, profile): two outputs of the same mix and profile share it; another mix gets its own.
    const QString mix = o.value("mix").toString("main").trimmed().isEmpty() ? QString("main") : o.value("mix").toString().trimmed();
    const QString key = "mix=" + mix + ";" + QString::fromUtf8(QJsonDocument(profile.toJson()).toJson(QJsonDocument::Compact));

    EncoderStage* stage = nullptr;
    { QMutexLocker l(&m_outputMutex); stage = m_stageByKey.value(key, nullptr); }
    bool newStage = false;
    if (!stage) {
        stage = new EncoderStage(profile, m_cfg.width, m_cfg.height, m_cfg.frameRate, this);
        if (!stage->initialize()) { const QString e = stage->lastError(); delete stage; scheduleRetry(s, "encoder: " + (e.isEmpty() ? QString("could not open") : e)); return false; }
        connect(stage, &EncoderStage::encoderError, this, &PlayoutEngine::error);
        newStage = true;
    }

    PacketSink* sink = nullptr;
    if (type == "null") sink = new NullPacketSink(name, this);
    else if (type == "udp" || type == "srt" || type == "rtmp" || type == "rtmps" || type == "file" || type == "ffmpeg" || type == "rtp" || type == "rist" || type == "http") {
        sink = new FfmpegMuxSink(name, url, o.value("options").toObject(), this);
    } else if (type == "dektec_asi" || type == "asi" || type == "dektec") {
        QJsonObject opts = o.value("options").toObject();
        for (const char* k : {"port", "deviceType", "deviceIndex", "serial", "muxrate", "prefillMs", "configurePort"})
            if (o.contains(k) && !opts.contains(k)) opts[k] = o[k];
        sink = new DekTecAsiSink(name, opts, this);
    } else {
        if (newStage) { stage->shutdown(); delete stage; }
        { QMutexLocker l(&m_outputMutex); s->state = "error"; s->lastError = "unknown output type " + type; s->nextRetryMs = 0; }
        qWarning() << "[Engine] unknown output type" << type;
        emit error(QString("output %1: unknown type %2").arg(name, type));
        return false;
    }
    connect(sink, &PacketSink::sinkError, this, [this, name](const QString&, const QString& msg) {
        QMutexLocker l(&m_outputMutex); if (OutputSlot* x = slotByName(name)) x->lastError = msg; l.unlock(); emit error(name + ": " + msg); });
    if (!stage->addSink(sink)) {
        const QString e = sink->stats().lastError; delete sink;
        if (newStage) { stage->shutdown(); delete stage; }
        scheduleRetry(s, e.isEmpty() ? "sink failed to start" : e);
        return false;
    }
    QMutexLocker l(&m_outputMutex);
    if (newStage) { m_stageByKey.insert(key, stage); m_stages.append(stage); m_sinkMix.insert(stage, mix); }
    s->psink = sink; s->stageKey = key; m_packetSinks.append(sink);
    s->state = "running"; s->lastError.clear(); s->attempts = 0; s->startedMs = QDateTime::currentMSecsSinceEpoch(); s->lastBytes = 0; s->lastBytesMs = s->startedMs; s->stalledSec = 0;
    qInfo().noquote() << QString("[Engine] output %1 (%2) running on encoder %3%4").arg(name, type, stage->profile().name, stage->actualVideoCodec() != stage->profile().videoCodec ? " [" + stage->actualVideoCodec() + "]" : QString());
    return true;
}

// Retry policy from the definition: retry {enabled (true), maxAttempts (0 = unlimited, default 10), backoffSec (5)}.
void PlayoutEngine::scheduleRetry(OutputSlot* s, const QString& why) {
    const QJsonObject r = s->def.value("retry").toObject();
    const bool enabled = r.value("enabled").toBool(true); const int maxAttempts = r.value("maxAttempts").toInt(10); const int backoff = qMax(1, r.value("backoffSec").toInt(5));
    QMutexLocker l(&m_outputMutex);
    s->state = "error"; s->lastError = why; s->attempts++;
    if (enabled && !s->manualStop && (maxAttempts <= 0 || s->attempts < maxAttempts)) {
        const int delay = backoff * qMin(s->attempts, 6);
        s->nextRetryMs = QDateTime::currentMSecsSinceEpoch() + qint64(delay) * 1000;
        l.unlock();
        qWarning().noquote() << QString("[Engine] output %1 failed (%2); retry #%3 in %4 s").arg(s->name, why).arg(s->attempts).arg(delay);
        emit error(QString("output %1 failed: %2 (retry in %3 s)").arg(s->name, why).arg(delay));
    } else {
        s->nextRetryMs = 0;
        l.unlock();
        qWarning().noquote() << QString("[Engine] output %1 failed (%2); no more retries").arg(s->name, why);
        emit error(QString("output %1 failed: %2").arg(s->name, why));
    }
}

// Drop a shared encoder stage once no running slot references it.
void PlayoutEngine::releaseStage(const QString& key) {
    EncoderStage* stage = nullptr;
    {
        QMutexLocker l(&m_outputMutex);
        for (OutputSlot* x : m_slots) if (x->psink && x->stageKey == key) return;
        stage = m_stageByKey.take(key);
        if (stage) { m_stages.removeAll(stage); m_sinkMix.remove(stage); }
    }
    if (stage) { stage->shutdown(); delete stage; }
}

void PlayoutEngine::stopSlot(OutputSlot* s, const QString& newState) {
    PacketSink* ps = nullptr; BasebandSink* bs = nullptr; QString key;
    {
        QMutexLocker l(&m_outputMutex);
        ps = s->psink; bs = s->bsink; key = s->stageKey;
        if (ps) m_packetSinks.removeAll(ps);
        if (bs) { m_basebandSinks.removeAll(bs); m_sinkMix.remove(bs); }
        s->psink = nullptr; s->bsink = nullptr; s->stageKey.clear();
        s->state = newState; s->nextRetryMs = 0; s->bitrateKbps = 0.0;
    }
    if (ps) {
        EncoderStage* stage = nullptr; { QMutexLocker l(&m_outputMutex); stage = m_stageByKey.value(key, nullptr); }
        if (stage) stage->removeSink(ps);     // no writePacket is in flight after this returns
        ps->stop(); delete ps;
        releaseStage(key);
    }
    if (bs) { bs->stop(); delete bs; }
    qInfo().noquote() << QString("[Engine] output %1 (%2) %3").arg(s->name, s->type, newState);
}

void PlayoutEngine::teardownOutputs() {
    QList<OutputSlot*> all; { QMutexLocker l(&m_outputMutex); all = m_slots; }
    for (OutputSlot* s : all) if (s->psink || s->bsink) stopSlot(s, "stopped");
    QMutexLocker l(&m_outputMutex);
    for (EncoderStage* s : m_stages) { s->shutdown(); delete s; }   // stops any sink still attached
    m_stages.clear(); m_stageByKey.clear();
    for (PacketSink* s : m_packetSinks) delete s;
    m_packetSinks.clear();
    for (BasebandSink* s : m_basebandSinks) { s->stop(); delete s; }
    m_basebandSinks.clear();
    qDeleteAll(m_slots); m_slots.clear();
}

// Incremental reconfiguration: diff by output name. Untouched definitions keep running (no on-air
// glitch on the ASI/NDI sinks when an operator toggles one RTMP stream).
void PlayoutEngine::configureOutputs(const QJsonArray& outputs) {
    m_cfg.outputs = outputs;
    QHash<QString, QJsonObject> wanted; QStringList order; QSet<QString> names;
    for (const QJsonValue& v : outputs) {
        const QJsonObject o = v.toObject(); const QString type = o.value("type").toString().toLower();
        QString name = o.value("name").toString(type);
        for (int n = 2; names.contains(name); ++n) name = o.value("name").toString(type) + "-" + QString::number(n);
        names.insert(name); wanted.insert(name, o); order << name;
    }
    // 1. remove slots that are gone; restart the ones whose core definition changed
    QList<OutputSlot*> all; { QMutexLocker l(&m_outputMutex); all = m_slots; }
    for (OutputSlot* s : all) {
        if (!wanted.contains(s->name)) {
            if (s->psink || s->bsink) stopSlot(s, "stopped");
            { QMutexLocker l(&m_outputMutex); m_slots.removeAll(s); }
            qInfo().noquote() << QString("[Engine] output %1 removed").arg(s->name);
            delete s; continue;
        }
        const QJsonObject n = wanted.value(s->name);
        const bool coreChanged = QJsonDocument(outputCore(n)) != QJsonDocument(outputCore(s->def));
        const bool wasRunning = s->state == "running" || s->state == "starting";
        { QMutexLocker l(&m_outputMutex); s->def = n; s->type = n.value("type").toString().toLower(); s->url = n.value("url").toString(); s->enabled = n.value("enabled").toBool(true); s->autoStart = n.value("autoStart").toBool(true); }
        if (!s->enabled) { if (wasRunning) stopSlot(s, "disabled"); else { QMutexLocker l(&m_outputMutex); s->state = "disabled"; s->nextRetryMs = 0; } continue; }
        if (coreChanged) { if (wasRunning) { stopSlot(s, "stopped"); QMutexLocker l(&m_outputMutex); s->restarts++; } if (wasRunning || s->autoStart) startSlot(s); }
        else if (!wasRunning && s->state == "disabled") { { QMutexLocker l(&m_outputMutex); s->state = "stopped"; } if (s->autoStart) startSlot(s); }
    }
    // 2. add the new ones
    for (const QString& name : order) {
        bool exists = false; { QMutexLocker l(&m_outputMutex); exists = slotByName(name) != nullptr; }
        if (exists) continue;
        const QJsonObject o = wanted.value(name);
        auto* s = new OutputSlot; s->def = o; s->name = name; s->type = o.value("type").toString().toLower(); s->url = o.value("url").toString();
        s->enabled = o.value("enabled").toBool(true); s->autoStart = o.value("autoStart").toBool(true); s->state = s->enabled ? "stopped" : "disabled";
        { QMutexLocker l(&m_outputMutex); m_slots.append(s); }
        if (s->enabled && s->autoStart) startSlot(s);
        else qInfo().noquote() << QString("[Engine] output %1 (%2) added, %3").arg(s->name, s->type, s->enabled ? "autoStart off" : "disabled");
    }
}

bool PlayoutEngine::startOutput(const QString& name, QString* error) {
    OutputSlot* s = nullptr; { QMutexLocker l(&m_outputMutex); s = slotByName(name); }
    if (!s) { if (error) *error = "no such output: " + name; return false; }
    if (s->state == "running") return true;
    { QMutexLocker l(&m_outputMutex); s->attempts = 0; s->manualStop = false; }
    const bool ok = startSlot(s);
    if (!ok && error) *error = s->lastError;
    return ok;
}

bool PlayoutEngine::stopOutput(const QString& name) {
    OutputSlot* s = nullptr; { QMutexLocker l(&m_outputMutex); s = slotByName(name); }
    if (!s) return false;
    { QMutexLocker l(&m_outputMutex); s->manualStop = true; s->nextRetryMs = 0; }
    if (s->psink || s->bsink) stopSlot(s, s->enabled ? "stopped" : "disabled");
    else { QMutexLocker l(&m_outputMutex); s->state = s->enabled ? "stopped" : "disabled"; }
    return true;
}

bool PlayoutEngine::restartOutput(const QString& name, QString* error) {
    OutputSlot* s = nullptr; { QMutexLocker l(&m_outputMutex); s = slotByName(name); }
    if (!s) { if (error) *error = "no such output: " + name; return false; }
    if (s->psink || s->bsink) stopSlot(s, "stopped");
    { QMutexLocker l(&m_outputMutex); s->restarts++; s->attempts = 0; s->manualStop = false; }
    const bool ok = startSlot(s);
    if (!ok && error) *error = s->lastError;
    return ok;
}

QJsonObject PlayoutEngine::slotJson(const OutputSlot* s) const {
    QJsonObject o;
    o["name"] = s->name; o["type"] = s->type; o["url"] = livestream::redactUrl(s->url); o["state"] = s->state; o["lastError"] = s->lastError;
    o["enabled"] = s->enabled; o["autoStart"] = s->autoStart; o["restarts"] = s->restarts; o["attempts"] = s->attempts;
    o["uptimeSec"] = s->state == "running" && s->startedMs > 0 ? double((QDateTime::currentMSecsSinceEpoch() - s->startedMs) / 1000) : 0.0;
    o["bitrateKbps"] = s->bitrateKbps;
    o["mix"] = s->def.value("mix").toString("main");
    if (s->nextRetryMs > 0) o["retryInSec"] = double(qMax<qint64>(0, (s->nextRetryMs - QDateTime::currentMSecsSinceEpoch()) / 1000));
    const QJsonValue pv = s->def.value("profile");
    if (pv.isObject()) o["profile"] = pv.toObject().value("name").toString(pv.toObject().value("preset").toString("h264_aac")); else if (!isBasebandType(s->type)) o["profile"] = pv.toString("h264_aac");
    if (s->psink) {
        const SinkStats ss = s->psink->stats();
        o["kind"] = s->psink->type(); o["packetsOut"] = double(ss.framesOut); o["dropped"] = double(ss.dropped); o["bytesOut"] = double(ss.bytesOut); o["connected"] = ss.connected; o["error"] = ss.lastError;
        for (auto it = ss.extra.begin(); it != ss.extra.end(); ++it) o[it.key()] = it.value();
        if (EncoderStage* st = m_stageByKey.value(s->stageKey, nullptr)) { o["profile"] = st->profile().name; o["videoCodec"] = st->actualVideoCodec(); o["audioCodec"] = st->actualAudioCodec(); o["encodedWidth"] = st->outputWidth(); o["encodedHeight"] = st->outputHeight(); }
    } else if (s->bsink) {
        const SinkStats ss = s->bsink->stats();
        o["kind"] = s->bsink->type(); o["framesOut"] = double(ss.framesOut); o["dropped"] = double(ss.dropped); o["connected"] = ss.connected; o["error"] = ss.lastError;
        for (auto it = ss.extra.begin(); it != ss.extra.end(); ++it) o[it.key()] = it.value();
    } else {
        o["kind"] = s->type; o["connected"] = false; o["dropped"] = 0; o["bytesOut"] = 0; if (isBasebandType(s->type)) o["framesOut"] = 0; else o["packetsOut"] = 0; o["error"] = s->lastError;
    }
    return o;
}

QJsonArray PlayoutEngine::outputsStatus() const {
    QMutexLocker l(&m_outputMutex);
    QJsonArray a; for (const OutputSlot* s : m_slots) a.append(slotJson(s));
    return a;
}

void PlayoutEngine::fanOut(const MediaFrame& mf, const std::map<QString, MediaFrame>& mixes) {
    QMutexLocker l(&m_outputMutex);
    // Each sink / encoder gets the frame of its mix; an output naming a mix that does not exist (yet) gets main.
    auto pick = [&](const void* sink) -> const MediaFrame& {
        if (mixes.empty()) return mf;
        const QString mix = m_sinkMix.value(sink);
        if (mix.isEmpty() || mix == "main") return mf;
        auto it = mixes.find(mix);
        return it != mixes.end() ? it->second : mf;
    };
    for (BasebandSink* s : m_basebandSinks) s->pushFrame(pick(s));
    for (EncoderStage* s : m_stages) s->pushFrame(pick(s));
}

void PlayoutEngine::ensureMixPreview(const QString& id) {
    if (id == "main" || m_mixPreviews.count(id)) return;
    auto tap = std::make_unique<PreviewTap>();
    tap->setChannelFps(m_cfg.frameRate.nominalFps());
    PreviewTap::Profile p; p.width = 384; p.height = 216; p.fps = 6; p.quality = 72;
    tap->setBaseProfile(p);
    connect(tap.get(), &PreviewTap::frameReady, this, [this, id](const QByteArray& jpeg, int w, int h, qint64) { emit mixPreviewReady(id, jpeg, w, h); }, Qt::QueuedConnection);
    tap->setEnabled(true);
    m_mixPreviews[id] = std::move(tap);
}

// Channel config "mixes": the list is authoritative (a mix not in it is removed); each entry is setMix params.
void PlayoutEngine::applyMixes(const QJsonArray& mixes) {
    if (!m_comp) return;
    QSet<QString> want;
    for (const QJsonValue& v : mixes) {
        const QJsonObject m = v.toObject();
        const QString id = m.value("id").toString().trimmed();
        if (id.isEmpty() || id == "main" || id.contains(':')) continue;
        want.insert(id);
        m_comp->setMix(id, m);
        ensureMixPreview(id);
    }
    for (const QString& id : m_comp->mixIds()) if (id != "main" && !want.contains(id)) { m_comp->removeMix(id); m_mixPreviews.erase(id); }
}

// ---------------------------------------------------------------------------
// Commands (producer side)
// ---------------------------------------------------------------------------

void PlayoutEngine::handleTransportCommand(const QString& action, const QJsonObject& params) {
    QMutexLocker l(&m_cmdMutex);
    Command c; c.action = action.toLower(); c.params = params;
    m_commands.push_back(std::move(c));
}

void PlayoutEngine::loadPlaylist(const QJsonArray& playlist, bool replaceCurrent) {
    QJsonObject p; p["items"] = playlist; p["replaceCurrent"] = replaceCurrent;
    handleTransportCommand("loadplaylist", p);
}

void PlayoutEngine::appendPlaylist(const QJsonArray& items) {
    QJsonObject p; p["items"] = items;
    handleTransportCommand("appendplaylist", p);
}

void PlayoutEngine::applyConfig(const QJsonObject& config) {
    handleTransportCommand("config", config);
}

void PlayoutEngine::handleCgCommand(const QJsonObject& cmd) {
    QMutexLocker l(&m_cmdMutex);
    Command c; c.action = "cg"; c.params = cmd;
    m_commands.push_back(std::move(c));
}

void PlayoutEngine::rebuildSlate() {
    QStringList caps = m_cfg.slateCaptions;
    if (caps.isEmpty()) caps << m_cfg.channelName.toUpper() << "NO PROGRAM";
    QJsonObject spec; spec["type"] = "bars"; QJsonArray a; for (const QString& s : caps) a << s; spec["captions"] = a;
    QString err;
    std::unique_ptr<LayerSource> slate = LayerSource::create(spec, &err);
    if (slate && slate->open(m_comp->format())) m_comp->setSlate(std::move(slate));
    else qWarning() << "[Engine] slate source failed:" << err;
}

void PlayoutEngine::retireSource(std::unique_ptr<LayerSource> src) {
    if (!src) return;
    m_activeLoaders++;
    LayerSource* raw = src.release();
    std::thread([this, raw]() { raw->close(); delete raw; m_activeLoaders--; }).detach();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void PlayoutEngine::onFrameTick(qint64 frame, qint64 timestampNs) {
    if (!m_initialized.load()) return;
    QElapsedTimer timer; timer.start();

    if (m_originUtcMs == 0) {
        // Anchor wall clock to the frame timeline on the first tick.
        m_originUtcMs = QDateTime::currentMSecsSinceEpoch() - m_cfg.frameRate.frameToNs(frame) / 1000000LL;
    }
    if (m_lastTickNs && timestampNs - m_lastTickNs > m_cfg.frameRate.frameDurationNs() * 3 / 2) m_lateTicks++;
    m_lastTickNs = timestampNs;
    m_lastFrame = frame;
    // Phase profiler (PE-06): bound to this thread for the tick, so the compositor times its own phases into it.
    using tickprof::ScopedPhase;
    tickprof::TickProfiler::Bind profBind(m_tickProf);
    m_tickProf.beginTick(frame);

    { ScopedPhase ph(m_tickProf, tickprof::Commands); processCommands(frame); }
    { ScopedPhase ph(m_tickProf, tickprof::Advance); advanceIfNeeded(frame); }

    // --- picture + sound: program bus, layers, audio mix in one pass ---------------
    const QString st = m_stateName;
    m_comp->setProgramHold(st == "PAUSED");
    m_comp->setProgramAudioEnabled(st == "ON_AIR");
    std::map<QString, MediaFrame> mixes;
    MediaFrame mf;
    { ScopedPhase ph(m_tickProf, tickprof::Render); mf = m_comp->render(frame, currentPosSec(frame), &mixes); }
    mf.timestampNs = timestampNs;
    for (auto& kv : mixes) kv.second.timestampNs = timestampNs;

    { ScopedPhase ph(m_tickProf, tickprof::FanOut); fanOut(mf, mixes); }
    {
        ScopedPhase ph(m_tickProf, tickprof::Preview);
        if (m_preview) m_preview->submit(mf.video, frame);
        for (auto& kv : mixes) { auto it = m_mixPreviews.find(kv.first); if (it != m_mixPreviews.end()) it->second->submit(kv.second.video, frame); }
    }
    { ScopedPhase ph(m_tickProf, tickprof::Monitor); if (m_monitor) m_monitor->tick(frame); }
    emit framePresented(frame, timestampNs);

    const double ms = timer.nsecsElapsed() / 1e6;
    if (ms > m_tickMsMax) m_tickMsMax = ms;
    { ScopedPhase ph(m_tickProf, tickprof::Snapshot); updateSnapshot(frame, ms); }
    m_tickProf.endTick();               // the profiler's tick includes the snapshot (tickMsLast does not)
}

double PlayoutEngine::currentPosSec(qint64 frame) const {
    if (!m_hasCurrent) return 0.0;
    const double pos = m_cfg.frameRate.frameToNs(frame - m_itemStartFrame) / 1e9;
    return (m_current.loop && m_itemDurationSec > 0.0) ? pos : qMax(0.0, pos);
}

qint64 PlayoutEngine::frameToUtcMs(qint64 frame) const {
    return m_originUtcMs + m_cfg.frameRate.frameToNs(frame) / 1000000LL;
}

qint64 PlayoutEngine::utcMsToFrame(qint64 utcMs) const {
    // First frame whose start is at or after the instant: a hard start is never early.
    const qint64 ns = (utcMs - m_originUtcMs) * 1000000LL;
    qint64 f = m_cfg.frameRate.nsToFrame(ns);
    if (m_cfg.frameRate.frameToNs(f) < ns) ++f;
    return f;
}

// ---------------------------------------------------------------------------
// Commands (consumer side, tick thread)
// ---------------------------------------------------------------------------

void PlayoutEngine::processCommands(qint64 frame) {
    std::deque<Command> cmds;
    { QMutexLocker l(&m_cmdMutex); cmds.swap(m_commands); }
    for (Command& c : cmds) executeCommand(c, frame);
}

void PlayoutEngine::executeCommand(Command& c, qint64 frame) {
    const QString& a = c.action;
    // PE-04: this runs on the clock thread. A flood (CG updates at frame rate, a fader drag) must not become a log
    // line per tick: a bucket of 10 lines refilled at 10/s; what it drops is counted and reported once a second.
    if (m_cmdLogMode == 2) qDebug().noquote() << "[Engine]" << m_cfg.channelId << "cmd" << a;
    else if (m_cmdLogMode == 1) {
        const qint64 now = FrameClock::nowNs();
        if (m_cmdLogNs == 0) m_cmdLogNs = now;
        const qint64 refill = (now - m_cmdLogNs) / 100000000LL;             // one line per 100 ms
        if (refill > 0) { m_cmdLogBudget = int(qMin<qint64>(10, m_cmdLogBudget + refill)); m_cmdLogNs += refill * 100000000LL; }
        if (m_cmdLogBudget > 0) { --m_cmdLogBudget; qDebug().noquote() << "[Engine]" << m_cfg.channelId << "cmd" << a; }
        else ++m_cmdLogSuppressed;             // reported by the next telemetry (publishTelemetry), off this thread
    }

    if (a == "loadplaylist") {
        QList<Item> items;
        for (const auto& v : c.params.value("items").toArray()) items << Item::fromJson(v.toObject());
        const bool replace = c.params.value("replaceCurrent").toBool(false);
        discardPrepared();
        m_playlist = items;
        if (replace || !m_hasCurrent) {
            if (m_hasCurrent) endCurrent(frame, "REPLACED");
            m_index = -1;
            if (!m_playlist.isEmpty()) {
                startPrepare(0, m_playlist[0]);
                transitionTo("PREROLL", "playlist_loaded", frame);
            } else {
                goIdle(frame, "playlist_empty");
            }
        } else {
            // Keep playing current; the new list starts after it.
            m_index = -1;
            ensureNextPrepared();
        }
    } else if (a == "appendplaylist") {
        for (const auto& v : c.params.value("items").toArray()) m_playlist << Item::fromJson(v.toObject());
        ensureNextPrepared();
    } else if (a == "play") {
        if (m_stateName == "PAUSED") {
            // Resume: shift the item start so position continues from where we paused.
            m_itemStartFrame = frame - m_cfg.frameRate.nsToFrame(qint64(m_pausedPosSec * 1e9));
            transitionTo("ON_AIR", "resume", frame);
            logAsRun("RESUME", m_current, frame);
        } else if (m_stateName == "IDLE" || m_stateName == "HOLD") {
            if (m_hasCurrent && m_decoder) {
                transitionTo("ON_AIR", "play", frame);
            } else if (!m_playlist.isEmpty()) {
                const int idx = (m_index < 0 || m_index >= m_playlist.size()) ? 0 : m_index;
                startPrepare(idx, m_playlist[idx]);
                transitionTo("PREROLL", "play", frame);
            }
        }
    } else if (a == "pause") {
        if (m_stateName == "ON_AIR") {
            m_pausedPosSec = currentPosSec(frame);
            m_pauseFrame = frame;
            transitionTo("PAUSED", "pause", frame);
            logAsRun("PAUSE", m_current, frame);
        }
    } else if (a == "stop") {
        if (m_hasCurrent) endCurrent(frame, "STOP");
        discardPrepared();
        m_index = -1;
        goIdle(frame, "stop");
    } else if (a == "hold") {
        if (m_hasCurrent) endCurrent(frame, "HOLD");
        discardPrepared();
        transitionTo("HOLD", "hold", frame);
    } else if (a == "skip" || a == "next") {
        if (m_hasCurrent) {
            logAsRun("SKIP", m_current, frame);
            if (m_prepared && !m_prepared->failed) {
                endCurrent(frame, "END", true);
                takePrepared(frame, "skip");
            } else {
                endCurrent(frame, "END");
                ensureNextPrepared();
                transitionTo("PREROLL", "skip_wait", frame);
            }
        } else if (!m_playlist.isEmpty()) {
            const int idx = nextIndexAfter(m_index);
            if (idx >= 0) { startPrepare(idx, m_playlist[idx]); transitionTo("PREROLL", "skip", frame); }
        }
    } else if (a == "goto") {
        const int idx = c.params.value("index").toInt(-1);
        if (idx >= 0 && idx < m_playlist.size()) {
            discardPrepared();
            if (m_hasCurrent) endCurrent(frame, "END");
            startPrepare(idx, m_playlist[idx]);
            transitionTo("PREROLL", "goto", frame);
        }
    } else if (a == "take" || a == "playitem") {
        // Immediate single item (operator take). Resumes playlist after it.
        Item it = Item::fromJson(c.params);
        if (it.path.isEmpty()) it.path = c.params.value("assetPath").toString();
        if (!it.path.isEmpty()) {
            discardPrepared();
            // The preview-bus input is already received at full quality (InputMonitor): hand that receiver to program
            // and cut on this very frame. Otherwise open a fresh one; the cut waits for its first picture.
            std::unique_ptr<LayerSource> warm = (m_monitor && !it.source.isEmpty()) ? m_monitor->takeWarm(it.source) : nullptr;
            if (warm) {
                auto prep = std::make_unique<Prepared>();
                prep->playlistIndex = -1; prep->item = it; prep->live = std::move(warm);
                QMutexLocker l(&m_prepMutex);
                m_prepared = std::move(prep);
                m_preparing = false;
            } else {
                startPrepare(-1, it);
            }
            m_takeNow = true;           // cut to it the moment it is loaded, even mid-item (it used to wait for the item to end)
            if (!m_hasCurrent) transitionTo("PREROLL", "take", frame);
        }
    } else if (a == "seek") {
        if (m_hasCurrent && m_decoder) {
            const double target = qBound(0.0, c.params.value("seconds").toDouble(0.0), m_itemDurationSec);
            // Re-open at the new position on the loader thread; cut when ready.
            Item it = m_current;
            it.inPointSec = m_current.inPointSec + target;
            discardPrepared();
            startPrepare(m_index, it);
            logAsRun("SEEK", m_current, frame, {{"seconds", target}});
        }
    } else if (a == "config") {
        const QJsonObject& o = c.params;
        if (o.contains("slateCaptions")) {
            QStringList caps; for (const auto& v : o["slateCaptions"].toArray()) caps << v.toString();
            m_cfg.slateCaptions = caps;
            rebuildSlate();
        }
        if (o.contains("gpuMode")) { m_cfg.gpuMode = o["gpuMode"].toString("auto"); m_comp->setGpuAuto(m_cfg.gpuMode != "on"); }
        // Turning the GPU on here (not at start) keeps the output pipeline depth as it is: changing it on air would drop frames.
        if (o.contains("gpu")) { m_cfg.gpu = o["gpu"].toBool(); m_comp->setGpuEnabled(m_cfg.gpu); }
        if (o.contains("masterGainDb")) m_comp->setMasterGain(std::pow(10.0, o["masterGainDb"].toDouble() / 20.0));
        if (o.contains("prerollSec")) m_cfg.prerollSec = o["prerollSec"].toDouble(3.0);
        if (o.contains("mixes")) { m_cfg.mixes = o["mixes"].toArray(); applyMixes(m_cfg.mixes); }
        if (o.contains("outputs")) {
            // Output rebuild touches encoder threads; do it off the tick thread.
            QJsonArray outs = o["outputs"].toArray();
            QMetaObject::invokeMethod(this, [this, outs]() { configureOutputs(outs); }, Qt::QueuedConnection);
        }
        if (o.value("op").isObject()) {
            // {outputs:[...], op:{name, action}} — MSG_OUTPUT_CONFIG carrying a one-output operation
            const QJsonObject op = o["op"].toObject(); const QString name = op.value("name").toString(), act = op.value("action").toString(op.value("op").toString());
            QMetaObject::invokeMethod(this, [this, name, act]() { QString e; if (act == "stop") stopOutput(name); else if (act == "restart") restartOutput(name, &e); else startOutput(name, &e); }, Qt::QueuedConnection);
        }
    } else if (a == "record") {
        const auto params = c.params;
        QMetaObject::invokeMethod(this, [this, params]() { recordingCommand(params); }, Qt::QueuedConnection);
    } else if (a == "output") {
        // {action:"output", name, op:"start|stop|restart"} — one output, the others untouched.
        const QString name = c.params.value("name").toString(), op = c.params.value("op").toString(c.params.value("action").toString("start")).toLower();
        QMetaObject::invokeMethod(this, [this, name, op]() {
            QString e; bool ok = true;
            if (op == "stop") ok = stopOutput(name); else if (op == "restart") ok = restartOutput(name, &e); else ok = startOutput(name, &e);
            if (!ok) emit error(QString("output %1 %2: %3").arg(name, op, e.isEmpty() ? QString("failed") : e));
        }, Qt::QueuedConnection);
    } else if (a == "cue") {
        // Legacy frame-offset cue: execute `action` after offsetFrames.
        Command inner; inner.action = c.params.value("action").toString(); inner.params = c.params.value("params").toObject();
        const int offset = c.params.value("offsetFrames").toInt(0);
        if (offset <= 0) executeCommand(inner, frame);
        else { QMutexLocker l(&m_cmdMutex); m_commands.push_back(std::move(inner)); } // next tick (approximation)
    } else if (a == "cg" || a == "cg_attach") {
        executeCg(c, frame);
    } else {
        qWarning() << "[Engine] unknown command" << a;
    }
}

// ---------------------------------------------------------------------------
// CG / layers
// ---------------------------------------------------------------------------

void PlayoutEngine::executeCg(Command& c, qint64 frame) {
    const QJsonObject& p = c.params;
    const QString act = c.action == "cg_attach" ? "attach" : p.value("action").toString("show");
    Compositor::LayerSpec spec = Compositor::LayerSpec::fromJson(p);
    const bool hasT = p.contains("transition");
    const Compositor::Transition t = Compositor::Transition::fromJson(p.value("transition").toObject());
    if (act == "attach") {
        if (!c.source) { emit error("CG layer " + QString::number(spec.id) + ": " + p.value("_error").toString()); return; }
        m_comp->addLayer(spec, std::move(c.source), frame);
        QJsonObject extra; extra["layer"] = spec.id; extra["role"] = spec.role; extra["source"] = spec.source.value("type").toString();
        logAsRun("CG_SHOW", m_current, frame, extra);
    } else if (act == "show" || act == "add" || act == "load" || act == "play") {
        if (!p.contains("source") && !p.contains("template")) { m_comp->showLayer(spec.id, hasT ? t : spec.in, frame); return; }
        // Opening a source may touch disk, network or a device: never on the clock thread.
        m_activeLoaders++;
        const QJsonObject params = p; const OutputFormat fmt = m_comp->format();
        std::thread([this, params, fmt]() {
            QString err;
            std::unique_ptr<LayerSource> src = LayerSource::create(Compositor::LayerSpec::fromJson(params).source, &err);
            if (src && !src->open(fmt)) { err = src->lastError(); src->close(); src.reset(); }
            if (!src) qWarning() << "[Engine] CG source failed:" << err;
            Command cmd; cmd.action = "cg_attach"; cmd.params = params; cmd.source = std::move(src);
            if (!cmd.source) cmd.params["_error"] = err;
            { QMutexLocker l(&m_cmdMutex); m_commands.push_back(std::move(cmd)); }
            m_activeLoaders--;
        }).detach();
    } else if (act == "hide" || act == "stop") {
        m_comp->hideLayer(spec.id, hasT ? t : spec.out, frame);
        logAsRun("CG_HIDE", m_current, frame, {{"layer", spec.id}});
    } else if (act == "update") {
        m_comp->updateLayer(spec.id, p);
    } else if (act == "remove") {
        m_comp->removeLayer(spec.id);
    } else if (act == "clear") {
        m_comp->clearLayers(p.value("role").toString());
    } else if (act == "program_clear") {
        m_comp->programClear(t, frame);
    } else if (act == "mix") {
        // Output mixes: {mix:"web", name, strips:{...}, master:{gainDb}, solo, follow, reset} creates / updates one;
        // {mix:"web", remove:true} drops it (its outputs fall back to main).
        const QString id = p.value("mix").toString(p.value("id").toString()).trimmed();
        if (id.isEmpty() || id.contains(':')) return;
        if (p.value("remove").toBool(false)) { m_comp->removeMix(id); m_mixPreviews.erase(id); }
        else { m_comp->setMix(id, p); ensureMixPreview(id); }
    } else if (act == "monitor") {
        // Switcher input monitors: {inputs:[{key, source, quality:"low"|"high"}], leaseSec}. An empty list closes them all.
        if (m_monitor) m_monitor->apply(p.value("inputs").toArray(), p.value("leaseSec").toInt(45), frame);
    } else if (act == "preview") {
        // Confidence picture profiles: {base:{width,height,fps,quality}, boost:{...}, boostSec}.
        if (m_preview) {
            if (p.value("base").isObject()) m_preview->setBaseProfile(PreviewTap::Profile::fromJson(p.value("base").toObject(), PreviewTap::Profile()));
            if (p.value("boost").isObject()) { PreviewTap::Profile b; b.width = 640; b.height = 360; b.fps = 15; b.quality = 80; m_preview->setBoostProfile(PreviewTap::Profile::fromJson(p.value("boost").toObject(), b)); }
            if (p.contains("boostSec")) m_preview->boost(p.value("boostSec").toInt(0));
        }
    } else if (act == "audio") {
        // Mixer: {program:{gainDb,mute}, master:{gainDb}, solo:"program"|"L<layer>"|""}. Line-in / layer strips use "update" (audioGainDb, audioMute).
        // With "mix":"<id>" (not main) the same fields, plus strips:{"L61":{gainDb,mute}}, set that mix's own faders.
        const QString mixId = p.value("mix").toString("main");
        if (mixId != "main") {
            QJsonObject mp; QJsonObject strips = p.value("strips").toObject();
            if (p.value("program").isObject()) strips["program"] = p.value("program").toObject();
            mp["strips"] = strips;
            if (p.contains("master")) mp["master"] = p.value("master");
            if (p.contains("solo")) mp["solo"] = p.value("solo");
            if (p.contains("follow")) mp["follow"] = p.value("follow");
            m_comp->setMix(mixId, mp);
            return;
        }
        if (p.value("strips").isObject()) { QJsonObject mp; mp["strips"] = p.value("strips"); m_comp->setMix("main", mp); }
        auto lin = [](double db) { return db <= -90.0 ? 0.0 : std::pow(10.0, db / 20.0); };
        const QJsonObject pr = p.value("program").toObject();
        if (pr.contains("gainDb")) m_comp->setProgramGain(lin(pr.value("gainDb").toDouble(0.0)));
        if (pr.contains("mute")) m_comp->setProgramMute(pr.value("mute").toBool());
        if (p.contains("master")) m_comp->setMasterGain(lin(p.value("master").toObject().value("gainDb").toDouble(0.0)));
        if (p.contains("solo")) m_comp->setSolo(p.value("solo").toString());
    } else {
        qWarning() << "[Engine] unknown cg action" << act;
    }
}

void PlayoutEngine::runCgHooks(const QJsonArray& hooks, bool ending) {
    if (hooks.isEmpty()) return;
    QMutexLocker l(&m_cmdMutex);
    for (const auto& v : hooks) {
        Command c; c.action = "cg";
        // PlayPlayClean schedules name identity graphics ("bug", "lower_third", "legal_id", "clock", "ticker");
        // full command objects are passed through as-is.
        c.params = v.isString() ? identityCg(v.toString(), ending) : v.toObject();
        if (!c.params.isEmpty()) m_commands.push_back(std::move(c));
    }
}

QJsonObject PlayoutEngine::identityCg(const QString& name0, bool ending) const {
    const QString name = name0.trimmed().toLower();
    const QJsonObject id = m_cfg.identity;
    const QString brand = id.value("brandName").toString(m_cfg.channelName);
    const QString calls = id.value("callLetters").toString(brand);
    QJsonObject c;
    auto tr = [](const QString& type, double sec) { QJsonObject t; t["type"] = type; t["durationSec"] = sec; return t; };
    auto place = [](const QString& anchor, double mx, double my) { QJsonObject p; p["anchor"] = anchor; p["marginX"] = mx; p["marginY"] = my; return p; };
    if (ending) {
        if (name == "bug" || name == "lower_third" || name == "ticker" || name == "legal_id" || name == "clock" || name == "emergency") {
            c["action"] = "hide"; c["role"] = name == "clock" ? "legal_id" : name; c["transition"] = tr("fade", 0.4);
        }
        return c;
    }
    if (name == "bug") {
        c["action"] = "show"; c["role"] = "bug";
        QJsonObject src;
        if (!id.value("bugAsset").toString().isEmpty()) { src["type"] = "image"; src["path"] = id.value("bugAsset").toString(); }
        else { src["type"] = "template"; src["kind"] = "bug"; src["text"] = id.value("bugText").toString(calls); }
        c["source"] = src; c["placement"] = place("top_right", 60, 60); c["in"] = tr("fade", 0.5); c["out"] = tr("fade", 0.5);
    } else if (name == "lower_third") {
        c["action"] = "show"; c["role"] = "lower_third";
        QJsonObject src; src["type"] = "template"; src["kind"] = "lower_third"; src["title"] = m_current.title; src["subtitle"] = brand;
        c["source"] = src; c["placement"] = place("bottom_left", 120, 90); c["in"] = tr("slide_left", 0.5); c["out"] = tr("fade", 0.4); c["holdSec"] = id.value("lowerThirdHoldSec").toDouble(8.0);
    } else if (name == "legal_id") {
        c["action"] = "show"; c["role"] = "legal_id";
        QJsonObject src;
        if (!id.value("legalIdAsset").toString().isEmpty()) { src["type"] = "file"; src["path"] = id.value("legalIdAsset").toString(); }
        else { src["type"] = "template"; src["kind"] = "lower_third"; src["title"] = calls; src["subtitle"] = brand; }
        c["source"] = src; c["placement"] = place("bottom", 0, 90); c["in"] = tr("fade", 0.3); c["out"] = tr("fade", 0.3); c["holdSec"] = 6.0;
    } else if (name == "clock") {
        c["action"] = "show"; c["role"] = "legal_id";
        QJsonObject src; src["type"] = "template"; src["kind"] = "clock"; src["format"] = "h:mm AP";
        c["source"] = src; c["placement"] = place("top_left", 60, 60); c["in"] = tr("fade", 0.3);
    } else if (name == "ticker") {
        c["action"] = "show"; c["role"] = "ticker";
        QJsonObject src; src["type"] = "template"; src["kind"] = "ticker"; src["text"] = id.value("tickerText").toString(brand);
        c["source"] = src; c["placement"] = place("bottom", 0, 0); c["in"] = tr("wipe_right", 0.5);
    } else if (name == "emergency" || name == "slate") {
        c["action"] = "show"; c["role"] = "emergency";
        QJsonObject src;
        if (!id.value("emergencySlate").toString().isEmpty()) { src["type"] = "file"; src["path"] = id.value("emergencySlate").toString(); src["loop"] = true; }
        else { src["type"] = "bars"; src["captions"] = id.value("slateCaptions").toArray(); }
        c["source"] = src; c["in"] = tr("cut", 0);
    } else {
        qWarning() << "[Engine] unknown CG hook" << name0;
    }
    return c;
}

// ---------------------------------------------------------------------------
// Playlist progression
// ---------------------------------------------------------------------------

int PlayoutEngine::nextIndexAfter(int idx) const {
    const int n = idx + 1;
    return (n >= 0 && n < m_playlist.size()) ? n : -1;
}

void PlayoutEngine::ensureNextPrepared() {
    if (m_preparing.load() || m_prepared) return;
    const int idx = nextIndexAfter(m_index);
    if (idx < 0) return;
    startPrepare(idx, m_playlist[idx]);
}

void PlayoutEngine::startPrepare(int playlistIndex, const Item& item) {
    discardPrepared();
    m_preparing = true;
    m_preparingIndex = playlistIndex;
    const quint64 gen = ++m_loadGeneration;

    DecoderPipeline::Config dc;
    dc.outWidth = m_cfg.width; dc.outHeight = m_cfg.height; dc.outRate = m_cfg.frameRate;
    dc.sampleRate = m_cfg.sampleRate; dc.channels = m_cfg.audioChannels;
    dc.hwAccel = m_cfg.hwDecode; dc.hwAuto = m_cfg.hwDecodeAuto; dc.loop = item.loop;

    m_activeLoaders++;
    const OutputFormat fmt = m_comp ? m_comp->format() : OutputFormat();
    std::thread([this, playlistIndex, item, dc, gen, fmt]() {
        auto prep = std::make_unique<Prepared>();
        prep->playlistIndex = playlistIndex;
        prep->item = item;
        if (!item.source.isEmpty()) {
            // Live input / stream: open the LayerSource here (NDI connect, SDI matrix, network pull).
            QString err;
            prep->live = LayerSource::create(item.source, &err);
            if (prep->live && !prep->live->open(fmt)) { err = prep->live->lastError(); prep->live->close(); prep->live.reset(); }
            if (!prep->live) { prep->failed = true; prep->error = err.isEmpty() ? "live source failed" : err; }
            // A receiver is "open" before it has connected (NDI finds the sender, then the first frame arrives). Cutting at that
            // point put black on air for up to a second or two; wait here for the first picture (bounded, then cut anyway).
            if (auto* live = qobject_cast<LiveSourceBase*>(prep->live.get())) {
                const int waitMs = qBound(0, item.source.value("firstFrameTimeoutMs").toInt(4000), 15000);
                QElapsedTimer w; w.start();
                while (!live->hasPicture() && w.elapsed() < waitMs && gen == m_loadGeneration.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (!live->hasPicture()) qWarning().noquote() << "[Engine] no picture from" << item.path << "after" << w.elapsed() << "ms: taking it anyway";
            }
        } else {
            prep->decoder = std::make_unique<DecoderPipeline>(dc);
            if (!prep->decoder->open(item.path, item.inPointSec, item.outPointSec)) {
                prep->failed = true;
                prep->error = prep->decoder->lastError();
                prep->decoder.reset();
            }
        }
        {
            QMutexLocker l(&m_prepMutex);
            if (gen == m_loadGeneration.load()) {
                m_prepared = std::move(prep);
                m_preparing = false;
            }
        }
        if (prep && prep->decoder) prep->decoder->close();   // stale load: throw away
        if (prep && prep->live) prep->live->close();
        m_activeLoaders--;
    }).detach();
}

void PlayoutEngine::discardPrepared() {
    ++m_loadGeneration;          // any in-flight load becomes stale
    m_takeNow = false;           // a pending take goes with it (skip / goto / a newer take)
    std::unique_ptr<Prepared> old;
    {
        QMutexLocker l(&m_prepMutex);
        old = std::move(m_prepared);
        m_preparing = false;
        m_preparingIndex = -1;
    }
    if (old && old->decoder) retireDecoder(std::move(old->decoder));
    if (old && old->live) retireSource(std::move(old->live));
}

void PlayoutEngine::retireDecoder(std::unique_ptr<DecoderPipeline> dec) {
    if (!dec) return;
    // Closing joins the decode thread and frees GPU/FFmpeg state (100 ms+);
    // never do that on the clock thread.
    m_activeLoaders++;
    DecoderPipeline* raw = dec.release();
    std::thread([this, raw]() {
        raw->close();
        delete raw;
        m_activeLoaders--;
    }).detach();
}

void PlayoutEngine::waitForBackgroundWork() {
    QElapsedTimer t; t.start();
    while (m_activeLoaders.load() > 0 && t.elapsed() < 10000) QThread::msleep(5);
}

void PlayoutEngine::advanceIfNeeded(qint64 frame) {
    const QString st = m_stateName;

    // Hard-start pre-emption: a fixed-time item cuts in when its time arrives.
    if (st == "ON_AIR" || st == "PREROLL" || st == "IDLE") {
        const int nidx = nextIndexAfter(m_index);
        if (nidx >= 0 && m_playlist[nidx].hardStartUtcMs > 0) {
            const qint64 due = utcMsToFrame(m_playlist[nidx].hardStartUtcMs);
            if (frame >= due) {
                if (!m_prepared && !m_preparing.load()) startPrepare(nidx, m_playlist[nidx]);
                if (m_prepared && !m_prepared->failed && m_prepared->playlistIndex == nidx) {
                    if (m_hasCurrent) { logAsRun("TRUNCATED", m_current, frame); endCurrent(frame, "END", true); }
                    takePrepared(frame, "hard_start");
                    return;
                }
            } else if (!m_prepared && !m_preparing.load() &&
                       frame >= due - m_cfg.frameRate.nsToFrame(qint64(m_cfg.prerollSec * 1e9))) {
                startPrepare(nidx, m_playlist[nidx]);
            }
        }
    }

    if (st == "ON_AIR" && m_hasCurrent) {
        // An operator take that has finished loading cuts in now; the playlist resumes after it.
        if (m_takeNow) {
            bool ready = false, failedTake = false;
            { QMutexLocker l(&m_prepMutex); if (m_prepared && m_prepared->playlistIndex < 0) { ready = !m_prepared->failed; failedTake = m_prepared->failed; } }
            if (ready) { m_takeNow = false; endCurrent(frame, "END", true); takePrepared(frame, "take"); return; }
            if (failedTake) m_takeNow = false;   // logged and dropped by the failed-item handling below
        }
        const double pos = currentPosSec(frame);
        const double remaining = m_itemDurationSec - pos;
        const bool decoderDone = m_decoder && m_decoder->atEnd() && !m_current.loop;
        const bool timeUp = (m_itemDurationSec > 0.0 && !m_current.loop && remaining <= 1e-6);

        // A prepared item that failed to open: log it, drop it from the list and
        // prepare the one after it while there is still time for a clean cut.
        {
            std::unique_ptr<Prepared> failed;
            { QMutexLocker l(&m_prepMutex); if (m_prepared && m_prepared->failed) failed = std::move(m_prepared); }
            if (failed) {
                qWarning() << "[Engine]" << m_cfg.channelId << "dropping unplayable item:" << failed->item.path << failed->error;
                logAsRun("FAILED", failed->item, frame, {{"error", failed->error}});
                emit error(QString("Cannot play %1: %2").arg(failed->item.path, failed->error));
                if (failed->playlistIndex > m_index && failed->playlistIndex < m_playlist.size()) {
                    m_playlist.removeAt(failed->playlistIndex);
                }
                ensureNextPrepared();
            }
        }

        // Prepare the next item ahead of time (not for hard-start items; handled above).
        if (!m_current.loop && remaining <= m_cfg.prerollSec && !m_prepared && !m_preparing.load()) {
            const int nidx = nextIndexAfter(m_index);
            if (nidx >= 0 && m_playlist[nidx].hardStartUtcMs == 0) startPrepare(nidx, m_playlist[nidx]);
        }

        if (timeUp || decoderDone) {
            const bool follows = m_prepared && !m_prepared->failed &&
                !(m_prepared->item.hardStartUtcMs > 0 && frame < utcMsToFrame(m_prepared->item.hardStartUtcMs));
            endCurrent(frame, "END", follows);
            const int nidx = nextIndexAfter(m_index);
            if (m_prepared && !m_prepared->failed) {
                // Hard-start items wait for their time; everything else cuts now.
                if (m_prepared->item.hardStartUtcMs > 0 && frame < utcMsToFrame(m_prepared->item.hardStartUtcMs)) {
                    transitionTo("PREROLL", "waiting_hard_start", frame);
                } else {
                    takePrepared(frame, "auto_follow");
                }
            } else if (nidx >= 0) {
                m_missedCuts++;
                qWarning() << "[Engine]" << m_cfg.channelId << "next item not ready at cut; holding slate";
                if (!m_preparing.load()) startPrepare(nidx, m_playlist[nidx]);
                transitionTo("PREROLL", "next_not_ready", frame);
            } else {
                m_index = -1;
                goIdle(frame, "playlist_end");
            }
        }
        return;
    }

    if (st == "PREROLL") {
        QMutexLocker l(&m_prepMutex);
        if (m_prepared) {
            if (m_prepared->failed) {
                const QString err = m_prepared->error;
                const int failedIdx = m_prepared->playlistIndex;
                const Item failedItem = m_prepared->item;
                m_prepared.reset();
                l.unlock();
                qWarning() << "[Engine]" << m_cfg.channelId << "item failed to open:" << failedItem.path << err;
                logAsRun("FAILED", failedItem, frame, {{"error", err}});
                emit error(QString("Cannot play %1: %2").arg(failedItem.path, err));
                const int nidx = nextIndexAfter(failedIdx);
                if (failedIdx >= 0 && nidx >= 0) { m_index = failedIdx; startPrepare(nidx, m_playlist[nidx]); }
                else goIdle(frame, "no_playable_items");
            } else if (m_prepared->item.hardStartUtcMs > 0 && frame < utcMsToFrame(m_prepared->item.hardStartUtcMs)) {
                // keep waiting on slate
            } else {
                l.unlock();
                takePrepared(frame, "ready");
            }
        }
    }
}

void PlayoutEngine::takePrepared(qint64 frame, const QString& reason) {
    std::unique_ptr<Prepared> p;
    { QMutexLocker l(&m_prepMutex); p = std::move(m_prepared); }
    if (!p || p->failed || (!p->decoder && !p->live)) return;

    std::unique_ptr<DecoderPipeline> dec = std::move(p->decoder);
    m_decoder = dec.get();
    m_current = p->item;
    m_hasCurrent = true;
    if (p->playlistIndex >= 0) m_index = p->playlistIndex;
    else m_takeNow = false;
    m_itemStartFrame = frame;
    m_itemDurationSec = m_current.durationSec > 0.0 ? m_current.durationSec : (m_decoder ? m_decoder->durationSec() : 0.0);
    if (m_itemDurationSec <= 0.0 && !m_current.loop) m_itemDurationSec = 0.0; // unknown: run to decoder EOF
    // Hand the decoder to the compositor's program bus; the transition runs from this frame.
    std::unique_ptr<LayerSource> src = p->live ? std::move(p->live) : std::unique_ptr<LayerSource>(new FileSource(std::move(dec), m_current.toJson()));
    m_comp->programTake(std::move(src), Compositor::Transition::fromJson(m_current.transition), frame);
    m_comp->setProgramItemMute(m_current.audioMute);      // mixer: audio follows video off lasts for this item only
    transitionTo("ON_AIR", reason, frame);
    logAsRun("START", m_current, frame, {{"durationSec", m_itemDurationSec}, {"transition", m_current.transition.value("type").toString("cut")}});
    runCgHooks(m_current.cgOnStart);

    // Start preparing the following item straight away for short items.
    if (!m_current.loop) ensureNextPrepared();
}

void PlayoutEngine::endCurrent(qint64 frame, const QString& eventType, bool takeFollows) {
    if (!m_hasCurrent) return;
    logAsRun(eventType, m_current, frame, {{"positionSec", currentPosSec(frame)}});
    runCgHooks(m_current.cgOnEnd, true);
    // When a take follows, the outgoing program stays on the bus so the transition can use it.
    if (!takeFollows && m_comp) m_comp->programClear(Compositor::Transition(), frame);
    m_decoder = nullptr;
    m_hasCurrent = false;
}

void PlayoutEngine::goIdle(qint64 frame, const QString& reason) {
    transitionTo("IDLE", reason, frame);
}

// ---------------------------------------------------------------------------
// State / telemetry / as-run
// ---------------------------------------------------------------------------

void PlayoutEngine::transitionTo(const QString& newState, const QString& reason, qint64 frame) {
    QString old;
    { QMutexLocker l(&m_stateMutex); old = m_stateName; m_stateName = newState; m_ident.stateName = newState; }
    if (old != newState) {
        qInfo().noquote() << QString("[Engine] %1 %2 -> %3 (%4) f=%5").arg(m_cfg.channelId, old, newState, reason).arg(frame);
        emit stateChanged(newState, reason);
    }
}

void PlayoutEngine::logAsRun(const QString& type, const Item& item, qint64 frame, const QJsonObject& extra) {
    QJsonObject e = extra;
    e["type"] = type;
    e["channelId"] = m_cfg.channelId;
    e["itemId"] = item.id;
    e["assetId"] = item.assetId;
    e["title"] = item.title;
    e["path"] = item.path;
    e["frame"] = double(frame);
    const qint64 utc = frameToUtcMs(frame);
    e["utcMs"] = double(utc);
    e["utc"] = QDateTime::fromMSecsSinceEpoch(utc, QTimeZone::utc()).toString(Qt::ISODateWithMs);
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(utc);
    e["timecode"] = Timecode::fromNsOfDay(qint64(local.time().msecsSinceStartOfDay()) * 1000000LL, m_cfg.frameRate);
    if (item.hardStartUtcMs > 0) e["plannedUtcMs"] = double(item.hardStartUtcMs);
    emit asRunEvent(e);
}

void PlayoutEngine::updateSnapshot(qint64 frame, double tickMs) {
    // Every tick: numbers only, into the seqlock (no mutex, no string, no JSON on the clock thread).
    TickPod p;
    p.frame = frame;
    p.utcMs = frameToUtcMs(frame);
    p.playlistSize = m_playlist.size();
    p.playlistIndex = m_index;
    p.hasCurrent = m_hasCurrent ? 1 : 0;
    if (m_hasCurrent) {
        p.positionSec = (m_stateName == "PAUSED") ? m_pausedPosSec : currentPosSec(frame);
        p.durationSec = m_itemDurationSec;
        p.remainingSec = m_itemDurationSec > 0 ? qMax(0.0, m_itemDurationSec - p.positionSec) : 0.0;
    }
    p.tickMsLast = tickMs;
    p.tickMsMax = m_tickMsMax;
    p.lateTicks = m_lateTicks;
    p.missedCuts = m_missedCuts;
    if (m_decoder) {
        auto ds = m_decoder->stats();
        p.decoderQueue = ds.queueDepth;
        p.audioUnderruns = ds.audioUnderruns;
    }
    p.healthy = (m_stateName != "ERROR") ? 1 : 0;

    // What is on air and next: implicitly shared copies (refcounts, no allocation), published only when they changed.
    Ident id;
    id.stateName = m_stateName;
    if (m_hasCurrent) { id.currentItemId = m_current.id; id.currentAssetId = m_current.assetId; id.currentAssetPath = m_current.path; id.currentTitle = m_current.title; }
    {
        QMutexLocker l(&m_prepMutex);     // the loader holds it for a pointer move only
        if (m_prepared && !m_prepared->failed) { id.nextItemId = m_prepared->item.id; id.nextTitle = m_prepared->item.title; p.nextReady = 1; }
        else {
            const int nidx = nextIndexAfter(m_index);
            if (nidx >= 0) { id.nextItemId = m_playlist[nidx].id; id.nextTitle = m_playlist[nidx].title; }
        }
    }
    m_pod.store(p);
    if (!(id == m_identClock)) m_identDirty = true;
    if (m_identDirty && m_stateMutex.tryLock()) {
        m_ident = id;
        m_stateMutex.unlock();
        m_identClock = std::move(id);
        m_identDirty = false;
    }

    // Once a second: the clock-thread-only pieces here (stall watchdog, closing the profiler window); everything that
    // takes locks, calls sinks or builds JSON goes to the engine's thread (PLAYPLAY_TELEMETRY_MAIN=0: stays here).
    const qint64 nowNs = FrameClock::nowNs();
    if ((nowNs - m_lastTelemetryNs) < 1000000000LL) return;
    m_lastTelemetryNs = nowNs;
    stallWatchdogTick(frame);
    // PE-06: close the phase window (the ticks completed since the last telemetry; this tick lands in the next one).
    tickprof::Window w;
    if (m_tickProf.enabled()) w = m_tickProf.takeWindow();
    const qint64 notLogged = m_cmdLogSuppressed; m_cmdLogSuppressed = 0;
    if (!m_telemetryOnMain) { publishTelemetry(w, notLogged); return; }
    QMetaObject::invokeMethod(this, [this, w = std::move(w), notLogged]() { publishTelemetry(w, notLogged); }, Qt::QueuedConnection);
}

// Once a second on the clock thread: a decoder that stops producing while ON_AIR holds the last frame, then skips.
void PlayoutEngine::stallWatchdogTick(qint64 frame) {
    if (m_stateName == "ON_AIR" && m_decoder && !m_decoder->atEnd()) {
        const qint64 decoded = m_decoder->stats().decodedVideo;
        const bool nearEnd = m_itemDurationSec > 0.0 && (m_itemDurationSec - currentPosSec(frame)) < 1.0;
        if (decoded == m_lastDecoded && !nearEnd) {
            const int stalled = m_stallSeconds.load(std::memory_order_relaxed) + 1;
            m_stallSeconds.store(stalled, std::memory_order_relaxed);
            if (stalled == 1) emit error(QString("decoder stalled on %1 (holding last frame)").arg(m_current.title.isEmpty() ? m_current.path : m_current.title));
            if (stalled >= int(qMax(1.0, m_cfg.stallSkipSec))) {
                logAsRun("FAILED", m_current, frame, {{"error", "decoder stall"}});
                emit error(QString("decoder stalled %1 s: skipping %2").arg(stalled).arg(m_current.title));
                Command c; c.action = "skip"; { QMutexLocker l(&m_cmdMutex); m_commands.push_back(std::move(c)); }
                m_stallSeconds.store(0, std::memory_order_relaxed);
            }
        } else m_stallSeconds.store(0, std::memory_order_relaxed);
        m_lastDecoded = decoded;
    } else { m_stallSeconds.store(0, std::memory_order_relaxed); m_lastDecoded = -1; }
}

// Per-output stats, cheap enough once a second only (telemetry thread).
void PlayoutEngine::collectOutputStats(State& s) {
    s.outputs = QJsonArray();
    s.framesDroppedByOutputs = 0;
    QMutexLocker l(&m_outputMutex);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (EncoderStage* st : m_stages) {
        auto es = st->stats();
        s.framesDroppedByOutputs += es.framesDropped;
        QJsonObject o; o["kind"] = "encoder"; o["profile"] = st->profile().name; o["name"] = "encoder:" + st->profile().name;
        o["videoCodec"] = st->actualVideoCodec(); o["audioCodec"] = st->actualAudioCodec(); o["width"] = st->outputWidth(); o["height"] = st->outputHeight();
        o["framesIn"] = double(es.framesIn); o["encoded"] = double(es.framesEncoded); o["dropped"] = double(es.framesDropped);
        o["avgEncodeMs"] = es.avgEncodeMs; o["queue"] = es.queueDepth;
        o["videoRescaled"] = double(es.videoRescaled); o["videoRepeated"] = double(es.videoRepeated);
        o["audioPaddedMs"] = double(es.audioPaddedSamples) * 1000.0 / 48000.0; o["audioDroppedMs"] = double(es.audioDroppedSamples) * 1000.0 / 48000.0;
        o["videoKbps"] = st->profile().videoBitrateKbps; o["sinks"] = st->sinkCount();
        s.outputs.append(o);
    }
    // One entry per configured output (running or not): sink statistics + slot state, bitrate from the byte delta.
    for (OutputSlot* sl : m_slots) {
        qint64 bytes = 0; bool haveBytes = false;
        if (sl->psink) { bytes = sl->psink->stats().bytesOut; haveBytes = true; }
        if (haveBytes && sl->lastBytesMs > 0 && nowMs > sl->lastBytesMs) {
            const double kbps = double(bytes - sl->lastBytes) * 8.0 / double(nowMs - sl->lastBytesMs);   // bytes/ms*8 = kbit/s
            sl->bitrateKbps = sl->bitrateKbps > 0 ? sl->bitrateKbps * 0.5 + kbps * 0.5 : kbps;
        }
        if (haveBytes) { sl->lastBytes = bytes; sl->lastBytesMs = nowMs; }
        s.outputs.append(slotJson(sl));
    }
}

// Once a second (telemetry thread): lock the clock to the SDI output's consumption, send dead sinks to error + retry,
// and raise alarms for sinks that drop, underrun or disconnect. Works on the JSON collectOutputStats() just built.
void PlayoutEngine::outputHardening(State& s) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    // --- output-locked clock ------------------------------------------------------------
    // The SDI card consumes frames at its own clock; the sink's queue depth is the phase error.
    bool lockedNow = false;
    if (m_clock && m_cfg.clockSource != "internal") {
        for (const auto& v : s.outputs) {
            const QJsonObject o = v.toObject();
            if (o.value("kind").toString() != "dektec_sdi" || !o.value("connected").toBool() || !o.value("transmitting").toBool()) continue;
            // Continuous phase error in frames: frames waiting in the queue plus the fraction of a
            // frame between the engine's push and the card's callback. Aim for the target depth + half a frame.
            // Depth measured inside the card callback (after it took its frame) and averaged over the
            // second, plus the push-to-callback phase: a consistent, continuous error signal.
            const double queued = o.value("queueAfterCb").toDouble(o.value("queuedFrames").toDouble());
            const double target = o.value("targetQueue").toDouble(2.0) - 1.0;   // after the callback consumed one
            const double phase = o.value("phaseFrac").toDouble(0.5);
            const double err = (queued + phase) - (target + 0.5);   // + = engine is fast (queue growing)
            // Plant: phase drifts by ppm * 30e-6 frames per second. Kp = 2000 ppm/frame gives a ~17 s
            // time constant; Ki = Kp^2 * g / 4 (~30) is critically damped and removes the PC-vs-card
            // oscillator offset. The trim is clamped to +-300 ppm (0.03 %, inaudible).
            m_lockIntegral = qBound(-10.0, m_lockIntegral + err, 10.0);
            m_lockPpm = qBound(-300.0, -(2000.0 * err + 30.0 * m_lockIntegral), 300.0);
            m_clock->setRateTrimPpm(m_lockPpm);
            m_lockSink = o.value("name").toString(); lockedNow = true;
            break;
        }
    }
    if (!lockedNow && m_lockActive && m_clock) { m_clock->setRateTrimPpm(0.0); m_lockIntegral = 0.0; m_lockPpm = 0.0; m_lockSink.clear(); }
    m_lockActive = lockedNow;

    // --- output supervision: dead sinks go to error + retry, due retries restart (queued: a start can block) --
    {
        QStringList toRetry, toFail;
        QMutexLocker l(&m_outputMutex);
        for (OutputSlot* sl : m_slots) {
            if (sl->state == "running") {
                bool dead = false;
                if (sl->psink) {
                    const SinkStats ss = sl->psink->stats();
                    if (!sl->psink->isRunning()) dead = true;
                    // packets are dropped (write failures) while nothing leaves for 5 s running: the sink is wedged
                    else if (ss.dropped > sl->lastDropped && ss.framesOut == sl->lastOut) { if (++sl->stalledSec >= 5) dead = true; }
                    else sl->stalledSec = 0;
                    sl->lastDropped = ss.dropped; sl->lastOut = ss.framesOut;
                } else if (sl->bsink && !sl->bsink->isRunning()) dead = true;
                if (dead) toFail << sl->name;
            } else if (sl->state == "error" && sl->nextRetryMs > 0 && nowMs >= sl->nextRetryMs && !sl->manualStop) {
                sl->nextRetryMs = 0; toRetry << sl->name;
            }
        }
        l.unlock();
        for (const QString& n : toFail) QMetaObject::invokeMethod(this, [this, n]() {
            OutputSlot* sl = nullptr; { QMutexLocker l2(&m_outputMutex); sl = slotByName(n); } if (!sl || sl->state != "running") return;
            const QString why = sl->lastError.isEmpty() ? QString("sink stopped delivering") : sl->lastError;
            stopSlot(sl, "error"); sl->restarts++; scheduleRetry(sl, why);
        }, Qt::QueuedConnection);
        for (const QString& n : toRetry) QMetaObject::invokeMethod(this, [this, n]() {
            OutputSlot* sl = nullptr; { QMutexLocker l2(&m_outputMutex); sl = slotByName(n); } if (!sl || sl->state == "running" || sl->manualStop) return;
            qInfo().noquote() << QString("[Engine] output %1: retry #%2").arg(n).arg(sl->attempts);
            startSlot(sl);
        }, Qt::QueuedConnection);
    }

    // --- output alarms (rate-limited per sink) ------------------------------------------------------
    for (const auto& v : s.outputs) {
        const QJsonObject o = v.toObject();
        if (o.value("kind").toString() == "encoder") continue;
        if (o.contains("state") && o.value("state").toString() != "running") continue;   // stopped/disabled/error slots are reported, not alarmed
        const QString name = o.value("name").toString();
        const QJsonObject last = m_sinkLast.value(name);
        QString why;
        if (!o.value("connected").toBool()) why = "not connected" + (o.value("error").toString().isEmpty() ? QString() : ": " + o.value("error").toString());
        else if (o.value("dropped").toDouble() > last.value("dropped").toDouble() + 2) why = QString("dropped %1 frames in the last second").arg(o.value("dropped").toDouble() - last.value("dropped").toDouble());
        else if (o.value("underruns").toDouble() > last.value("underruns").toDouble()) why = QString("%1 underrun(s)").arg(o.value("underruns").toDouble() - last.value("underruns").toDouble());
        else if (o.contains("fifoLoadPct") && o.value("fifoLoadPct").toDouble() < 1.0 && o.value("transmitting").toBool()) why = "ASI FIFO empty";
        if (!why.isEmpty() && !last.isEmpty() && nowMs - m_sinkAlarmMs.value(name) > 30000) {
            m_sinkAlarmMs[name] = nowMs;
            emit error(QString("output %1: %2").arg(name, why));
        }
        m_sinkLast[name] = o;
    }
}

// The 1 Hz telemetry: output stats, hardening, the closed profiler window, then stateJson() for the consumers.
// Runs on the engine's thread (queued from the telemetry tick), or on the clock thread with PLAYPLAY_TELEMETRY_MAIN=0.
void PlayoutEngine::publishTelemetry(const tickprof::Window& w, qint64 cmdsNotLogged) {
    if (cmdsNotLogged > 0) qDebug().noquote() << "[Engine]" << m_cfg.channelId << "cmd:" << cmdsNotLogged << "more command(s) not logged in the last second";
    State s;
    collectOutputStats(s);
    outputHardening(s);
    { QMutexLocker l(&m_stateMutex); m_outputsJson = s.outputs; m_outputDrops = s.framesDroppedByOutputs; m_tickPhases = w; }
    emit telemetry(stateJson());
}

QString PlayoutEngine::timecodeAt(qint64 utcMs) const {
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(utcMs);
    return Timecode::fromNsOfDay(qint64(local.time().msecsSinceStartOfDay()) * 1000000LL, m_cfg.frameRate);
}

PlayoutEngine::State PlayoutEngine::currentState() const {
    TickPod p; m_pod.load(p);
    State s;
    {
        QMutexLocker l(&m_stateMutex);
        s.stateName = m_ident.stateName;
        s.currentItemId = m_ident.currentItemId; s.currentAssetId = m_ident.currentAssetId;
        s.currentAssetPath = m_ident.currentAssetPath; s.currentTitle = m_ident.currentTitle;
        s.nextItemId = m_ident.nextItemId; s.nextTitle = m_ident.nextTitle;
        s.outputs = m_outputsJson; s.framesDroppedByOutputs = m_outputDrops;
    }
    s.nextReady = p.nextReady != 0;
    s.positionSeconds = p.positionSec; s.durationSeconds = p.durationSec; s.remainingSeconds = p.remainingSec;
    s.playlistIndex = p.playlistIndex; s.playlistSize = p.playlistSize;
    s.currentFrame = p.frame;
    s.timecode = timecodeAt(p.utcMs);
    s.isHealthy = p.healthy != 0;
    s.tickMsLast = p.tickMsLast; s.tickMsMax = p.tickMsMax; s.lateTicks = p.lateTicks;
    s.decoderQueue = p.decoderQueue; s.audioUnderruns = p.audioUnderruns; s.missedCuts = p.missedCuts;
    return s;
}

QJsonObject PlayoutEngine::stateJson() const {
    const State s = currentState();
    QJsonObject o;
    o["channelId"] = m_cfg.channelId;
    o["state"] = s.stateName;
    o["itemId"] = s.currentItemId; o["assetId"] = s.currentAssetId; o["path"] = s.currentAssetPath; o["title"] = s.currentTitle;
    o["nextItemId"] = s.nextItemId; o["nextTitle"] = s.nextTitle; o["nextReady"] = s.nextReady;
    o["position"] = s.positionSeconds; o["duration"] = s.durationSeconds; o["remaining"] = s.remainingSeconds;
    o["playlistIndex"] = s.playlistIndex; o["playlistSize"] = s.playlistSize;
    o["frame"] = double(s.currentFrame); o["timecode"] = s.timecode;
    o["frameRate"] = m_cfg.frameRate.toString(); o["width"] = m_cfg.width; o["height"] = m_cfg.height;
    o["healthy"] = s.isHealthy; o["error"] = s.errorMessage;
    QJsonObject t;
    t["tickMsLast"] = s.tickMsLast; t["tickMsMax"] = s.tickMsMax; t["lateTicks"] = double(s.lateTicks);
    t["decoderQueue"] = double(s.decoderQueue); t["audioUnderruns"] = double(s.audioUnderruns);
    t["outputDrops"] = double(s.framesDroppedByOutputs); t["missedCuts"] = double(s.missedCuts);
    t["clockTrimPpm"] = m_lockPpm; t["clockLocked"] = m_lockActive; t["clockLockSink"] = m_lockSink; t["stallSeconds"] = m_stallSeconds.load(std::memory_order_relaxed);
    if (m_clock) t["clock"] = m_clock->statsJson();   // PE-14: wait mode, tick lateness histogram, clock-thread CPU (atomics)
    if (m_tickProf.enabled()) {          // PE-06: copied under the lock, JSON built outside it
        tickprof::Window w; { QMutexLocker l(&m_stateMutex); w = m_tickPhases; }
        t["tickPhases"] = w.toJson();
    }
    o["telemetry"] = t;
    o["outputs"] = s.outputs;
    if (m_comp) { o["compositor"] = m_comp->status(); o["layers"] = m_comp->layersJson(); }
    { QMutexLocker l(&m_recordMutex); o["recording"] = m_recorder ? m_recorder->status() : QJsonObject{{"active", false}}; }
    if (m_preview) o["preview"] = m_preview->status();
    if (m_monitor) o["monitors"] = m_monitor->status();
    { QMutexLocker l(&m_stateMutex); o["liveInputs"] = m_liveInputs; o["liveInputsAtMs"] = double(m_liveInputsMs); }
    return o;
}

QJsonObject PlayoutEngine::meters() const { return m_comp ? m_comp->metersJson() : QJsonObject(); }

void PlayoutEngine::setDiscoveredInputs(const QJsonArray& inputs) {
    QMutexLocker l(&m_stateMutex);
    m_liveInputs = inputs;
    m_liveInputsMs = QDateTime::currentMSecsSinceEpoch();
}

QJsonArray PlayoutEngine::playlistJson() const {
    QJsonArray a;
    for (const Item& i : m_playlist) a.append(i.toJson());
    return a;
}
