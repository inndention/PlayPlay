#include "transcriptionmanager.h"
#include "transcripttools.h"
#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"
#include "../../tools/ppcommon/toolswitch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QStandardPaths>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QDebug>

using namespace sched;

struct TranscriptionManager::ToolCaps { toolswitch::CapsCache cache; };
static QList<TranscriptionManager*> s_alive;      // primary(): the Supervisor's instance, for the async route below

// POST /api/v1/transcribe/probe used to run Python with a 20 s waitForFinished on the Supervisor's event loop. This replaces that
// handler (laneext installs after supervisorcore_library.cpp registered it) with one that replies when the async probe finishes.
static laneext::Registrar s_transcribeProbeRoute("transcribe-probe-async", 20, [](const laneext::Context& ctx) {
    if (!ctx.rest || !TranscriptionManager::primary()) return;
    QPointer<TranscriptionManager> tm(TranscriptionManager::primary());
    ctx.rest->registerAsync("POST", "/api/v1/transcribe/probe", [tm](const QJsonObject&, const QHash<QString, QString>&, RestApiServer::Reply reply) {
        if (!tm) { reply(503, QJsonObject{{"error", "transcription manager is gone"}}); return; }
        tm->probeEnvironmentAsync([reply](const QJsonObject& o) { reply(200, o); });
    });
});

TranscriptionManager::TranscriptionManager(ScheduleStore* store, const QString& dataDir, const QString& scriptsDir, QObject* parent)
    : QObject(parent), m_store(store), m_dataDir(dataDir), m_scriptsDir(scriptsDir), m_toolCaps(std::make_unique<ToolCaps>()) {
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, [this]() { emit jobsChanged(); });
    loadConfig();
    QDir().mkpath(QDir(m_dataDir).filePath("transcripts"));
    s_alive.append(this);
}
TranscriptionManager::~TranscriptionManager() {
    s_alive.removeAll(this);
    if (m_proc) m_proc->kill();
    if (m_envProc) { m_envProc->disconnect(); m_envProc->kill(); }
    qDeleteAll(m_jobs);
}
TranscriptionManager* TranscriptionManager::primary() { return s_alive.isEmpty() ? nullptr : s_alive.first(); }

void TranscriptionManager::loadConfig() {
    m_cfg = m_store ? m_store->settingJson("transcribe.config") : QJsonObject();
    if (!m_cfg.contains("defaultLevel")) m_cfg["defaultLevel"] = "balanced";
    if (!m_cfg.contains("device")) m_cfg["device"] = "auto";
    if (!m_cfg.contains("autoTranscribe")) m_cfg["autoTranscribe"] = false;
    if (!m_cfg.contains("autoLevel")) m_cfg["autoLevel"] = "fast";
    if (!m_cfg.contains("language")) m_cfg["language"] = "en";
}
QJsonObject TranscriptionManager::config() const {
    QJsonObject o = m_cfg; o["pythonResolved"] = pythonPath(); o["script"] = QDir::toNativeSeparators(QDir(m_scriptsDir).filePath("transcribe.py"));
    if (!o.contains("engine")) o["engine"] = "python";
    o["engineResolved"] = engineResolved();
    return o;
}
void TranscriptionManager::setConfig(const QJsonObject& cfg) {
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
        if (it.key() == "engineResolved" || it.key() == "pythonResolved" || it.key() == "script") continue;     // reported, not stored
        if (it.key() == "engine" && !toolswitch::validModeValue(it.value().toString())) continue;               // a typo keeps the engine it had
        m_cfg[it.key()] = it.value();
    }
    if (cfg.contains("toolsDir")) m_toolCaps->cache.clear();
    if (m_store) m_store->setSettingJson("transcribe.config", m_cfg);
}
QString TranscriptionManager::toolPath() const { return toolswitch::findTool("PlayPlaySpeech", {m_cfg.value("toolsDir").toString(), QCoreApplication::applicationDirPath()}); }
QJsonObject TranscriptionManager::engineResolved() const {
    TranscriptionManager* self = const_cast<TranscriptionManager*>(this);     // the caps cache starts an async `version` probe; nothing here blocks
    const toolswitch::Mode m = toolswitch::parseMode(m_cfg.value("engine").toString());
    const QString exe = m == toolswitch::Mode::Legacy ? QString() : toolPath();
    static const toolswitch::Caps none;
    const toolswitch::Caps& caps = (m == toolswitch::Mode::Auto && !exe.isEmpty()) ? m_toolCaps->cache.get(self, exe, [self]() { self->notify(); }) : none;
    const toolswitch::Choice c = toolswitch::choose(m, exe, caps, "asr", "PlayPlaySpeech");
    QJsonObject o; o["mode"] = toolswitch::modeName(m, "python"); o["uses"] = c.native ? "native" : "python"; o["reason"] = c.reason;
    if (m != toolswitch::Mode::Legacy) { o["exe"] = exe; o["found"] = !exe.isEmpty(); }
    if (caps.probed) { QJsonArray a; for (const QString& s : caps.commands) a.append(s); o["commands"] = a; }
    return o;
}

QString TranscriptionManager::pythonPath() const {
    const QString c = m_cfg.value("python").toString();
    if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    for (const QString& cand : {QString("C:/Program Files/Python311/python.exe"), QString("C:/Program Files/Python312/python.exe"), QString("C:/Program Files/Python313/python.exe")}) if (QFileInfo::exists(cand)) return cand;
    const QString p = QStandardPaths::findExecutable("python"); return p.isEmpty() ? "python" : p;
}

QJsonArray TranscriptionManager::levels() const {
    QJsonArray a;
    auto add = [&](const char* id, const char* model, int beam, bool vad, const char* note) { QJsonObject o; o["id"] = id; o["model"] = model; o["beam"] = beam; o["vad"] = vad; o["note"] = note; a.append(o); };
    add("fast", "base (base.en)", 1, false, "a few seconds per minute of audio on the CPU; good for search and rough segments");
    add("balanced", "small", 2, true, "about real time on 8 cores; punctuation and names mostly right");
    add("accurate", "medium", 5, true, "2-3x slower than real time on the CPU; broadcast captions quality");
    add("best", "large-v3", 5, true, "needs a GPU to be practical; the most accurate model");
    return a;
}

QJsonObject TranscriptionManager::probeEnvironment() {
    QJsonObject o = m_env;
    if (o.isEmpty()) { o["pending"] = true; o["python"] = pythonPath(); }
    else o["cached"] = true;
    probeEnvironmentAsync({});
    o["probing"] = m_envProc != nullptr;
    return o;
}
/// Python + faster-whisper check without waiting: the process reports through signals; callers that arrive while one runs share it.
void TranscriptionManager::probeEnvironmentAsync(std::function<void(const QJsonObject&)> cb) {
    if (cb) m_envWaiters.append(cb);
    if (m_envProc) return;
    QProcess* p = new QProcess(this); m_envProc = p;
    auto timedOut = std::make_shared<bool>(false);
    const QString python = pythonPath();
    auto finish = [this, p, python](QJsonObject o) {
        if (m_envProc != p) return;
        m_envProc = nullptr; p->deleteLater();
        o["python"] = python; o["probedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        if (o.value("ok").toBool()) { QJsonArray cached; const QDir hub(QDir::homePath() + "/.cache/huggingface/hub"); for (const QString& d : hub.entryList({"models--*whisper*"}, QDir::Dirs)) cached.append(d.section("--", -1)); o["cachedModels"] = cached; }
        o["engine"] = engineResolved();       // what a job would run now (native tool / transcribe.py)
        m_env = o;
        const auto waiters = m_envWaiters; m_envWaiters.clear();
        for (const auto& w : waiters) if (w) w(o);
    };
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [p, finish, timedOut](int code, QProcess::ExitStatus st) {
        QJsonObject o;
        if (*timedOut) { o["ok"] = false; o["error"] = "python did not answer"; finish(o); return; }
        const QStringList lines = QString::fromUtf8(p->readAllStandardOutput()).trimmed().split('\n');
        if (st != QProcess::NormalExit || code != 0 || lines.size() < 3) { o["ok"] = false; o["error"] = QString::fromUtf8(p->readAllStandardError()).trimmed().section('\n', -1); finish(o); return; }
        o["ok"] = true; o["fasterWhisper"] = lines[0].trimmed(); o["cpuComputeTypes"] = lines[1].trimmed(); o["cudaDevices"] = lines[2].trimmed().toInt();
        finish(o);
    });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { QJsonObject o; o["ok"] = false; o["error"] = "python did not start"; finish(o); } });
    QTimer::singleShot(20000, p, [p, timedOut]() { if (p->state() != QProcess::NotRunning) { *timedOut = true; p->kill(); } });
    p->start(python, {"-I", "-c", "import faster_whisper, ctranslate2, sys; print(faster_whisper.__version__); print(','.join(sorted(ctranslate2.get_supported_compute_types('cpu')))); print(ctranslate2.get_cuda_device_count())"});
}

// ---------------------------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------------------------
QJsonObject TranscriptionManager::Job::toJson() const {
    QJsonObject o; o["id"] = id; o["mediaId"] = mediaId; o["title"] = title; o["level"] = level; o["state"] = state; o["error"] = error; o["progress"] = progress; o["total"] = total;
    o["pct"] = total > 0 ? qMin(100.0, 100.0 * progress / total) : 0.0; o["queuedMs"] = double(queuedMs); o["startedMs"] = double(startedMs); o["finishedMs"] = double(finishedMs); if (!engine.isEmpty()) o["engine"] = engine; return o;
}
QJsonArray TranscriptionManager::jobsJson() const { QJsonArray a; for (Job* j : m_jobs) a.append(j->toJson()); return a; }
QString TranscriptionManager::transcriptPath(const QString& mediaId) const { return QDir(m_dataDir).filePath("transcripts/" + mediaId + ".json"); }

QJsonObject TranscriptionManager::enqueue(const QString& mediaId, const QString& level, const QJsonObject& opts) {
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    if (!QFileInfo(m.path).isFile()) { QJsonObject e; e["error"] = "media file is missing"; e["status"] = 409; return e; }
    for (Job* j : m_jobs) if (j->mediaId == mediaId && (j->state == "queued" || j->state == "running")) return j->toJson();
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->mediaId = mediaId; j->title = m.title; j->path = m.path;
    j->level = level.isEmpty() ? m_cfg.value("defaultLevel").toString("balanced") : level; j->opts = opts; j->queuedMs = QDateTime::currentMSecsSinceEpoch(); j->total = m.durationSec;
    m_jobs.append(j);
    MediaRecord mm = m; QJsonObject t = mm.extra.value("transcript").toObject(); t["status"] = "queued"; t["level"] = j->level; mm.extra["transcript"] = t; m_store->saveMedia(mm);
    notify(); pump();
    return j->toJson();
}

bool TranscriptionManager::cancel(const QString& jobId) {
    for (Job* j : m_jobs) if (j->id == jobId) {
        if (j == m_current && m_proc) { m_cancelRequested = true; m_proc->kill(); return true; }
        if (j->state == "queued") { finish(j, "cancelled", "cancelled"); return true; }
        m_jobs.removeAll(j); delete j; notify(); return true;
    }
    return false;
}
void TranscriptionManager::clearFinished() { for (int i = m_jobs.size() - 1; i >= 0; --i) { Job* j = m_jobs[i]; if (j != m_current && j->state != "queued") { m_jobs.removeAt(i); delete j; } } notify(); }

void TranscriptionManager::pump() {
    if (m_current) return;
    Job* next = nullptr; for (Job* j : m_jobs) if (j->state == "queued") { next = j; break; }
    if (!next) return;
    m_current = next; next->state = "running"; next->startedMs = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject e = engineResolved();
    startJob(next, e.value("uses").toString() == "native");
}

void TranscriptionManager::startJob(Job* next, bool native) {
    m_errBuf.clear(); m_cancelRequested = false;
    QStringList args = {"-I", QDir(m_scriptsDir).filePath("transcribe.py"), "--input", next->path, "--output", transcriptPath(next->mediaId), "--level", next->level, "--device", m_cfg.value("device").toString("auto")};
    const QString lang = next->opts.value("language").toString(m_cfg.value("language").toString()); if (!lang.isEmpty() && lang != "auto") args << "--language" << lang;
    if (!m_cfg.value("computeType").toString().isEmpty()) args << "--compute" << m_cfg.value("computeType").toString();
    if (m_cfg.value("threads").toInt() > 0) args << "--threads" << QString::number(m_cfg.value("threads").toInt());
    if (!next->opts.value("model").toString().isEmpty()) args << "--model" << next->opts.value("model").toString();
    if (!next->opts.value("initialPrompt").toString().isEmpty()) args << "--initial-prompt" << next->opts.value("initialPrompt").toString();
    // DEP-13: the same arguments to PlayPlaySpeech asr (the transcribe.py contract); without the exe there is nothing native to run
    const QString exe = native ? toolPath() : QString();
    if (native && exe.isEmpty()) native = false;
    const QString program = native ? exe : pythonPath();
    if (native) args = toolswitch::nativeArgs("asr", args);
    next->engine = native ? "native" : (next->engine.startsWith("python (fallback") ? next->engine : QString("python"));
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        if (!m_current) return;
        m_errBuf += m_proc->readAllStandardError(); int nl;
        while ((nl = m_errBuf.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_errBuf.left(nl)).trimmed(); m_errBuf.remove(0, nl + 1);
            static const QRegularExpression rx("^PROGRESS\\s+([\\d.]+)\\s+([\\d.]+)");
            if (const auto m = rx.match(line); m.hasMatch()) { m_current->progress = m.captured(1).toDouble(); if (m.captured(2).toDouble() > 0) m_current->total = m.captured(2).toDouble(); notify(); }
            else if (line.startsWith("ERROR")) m_current->error = line.mid(6);
            else if (!line.isEmpty() && !line.startsWith("LOAD") && !line.startsWith("DONE")) m_current->error = line.left(400);
        }
    });
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, native](int code, QProcess::ExitStatus st) {
        Job* j = m_current; const QByteArray out = m_proc->readAllStandardOutput(); m_proc->deleteLater(); m_proc = nullptr;
        if (!j) return;
        // a native run that ended on its own with an error (64 = unsupported level / option / model), or died (a crash exit covers a
        // missing DLL next to the tool), is run again with transcribe.py; only the kill cancel() asked for ends the job "cancelled"
        const bool cancelled = m_cancelRequested; m_cancelRequested = false;
        const int rc = toolswitch::exitCode(st == QProcess::NormalExit, code, cancelled);
        const bool wrote = QFileInfo::exists(transcriptPath(j->mediaId));       // the step's success test (exit 0 without a transcript is a failure too)
        if (native && toolswitch::shouldFallBack(rc, wrote)) {
            const QString why = rc == toolswitch::kUnsupported ? toolswitch::lastJsonLine(out).value("error").toString("unsupported")
                              : rc == toolswitch::kCrashed ? QString("crashed (0x%1)").arg(uint(code), 0, 16)
                              : rc == 0 ? QString("no transcript written") : (j->error.isEmpty() ? QString("exit %1").arg(code) : j->error);
            if (toolswitch::expectedFallBack(rc)) qInfo().noquote() << "[Transcribe] native asr fell back to transcribe.py:" << why.left(200);
            else qWarning().noquote() << "[Transcribe] native asr fell back to transcribe.py:" << why.left(200);
            j->engine = "python (fallback: " + why.left(160) + ")"; j->error.clear(); j->progress = 0;
            startJob(j, false); return;
        }
        if (cancelled) { finish(j, "cancelled", "cancelled"); return; }
        if (rc != 0 || !wrote) { finish(j, "failed", j->error.isEmpty() ? (rc == toolswitch::kCrashed ? QString("transcriber crashed (0x%1)").arg(uint(code), 0, 16) : QString("transcriber exited %1").arg(code)) : j->error); return; }
        bool f = false; MediaRecord m = m_store->media(j->mediaId, &f);
        const QJsonObject t = transcript(j->mediaId);
        if (f) {
            int words = 0; for (const QJsonValue& s : t.value("segments").toArray()) words += s.toObject().value("words").toArray().size();
            QJsonObject tr; tr["status"] = "done"; tr["level"] = j->level; tr["model"] = t.value("model"); tr["language"] = t.value("language"); tr["words"] = words; tr["segments"] = t.value("segments").toArray().size(); tr["atMs"] = double(QDateTime::currentMSecsSinceEpoch()); tr["elapsedSec"] = t.value("elapsedSec"); tr["path"] = QDir::toNativeSeparators(transcriptPath(j->mediaId)); tr["engine"] = j->engine;
            m.extra["transcript"] = tr; m.extra["transcriptText"] = t.value("text").toString().left(20000); m_store->saveMedia(m);
            // segments made before this transcript existed get their spoken text now
            const QJsonArray cols = m.extra.value("segmentCollections").toArray();
            if (!cols.isEmpty()) setCollections(j->mediaId, cols, m.extra.value("activeCollection").toString());
            else if (!m.segments.isEmpty()) setSegments(j->mediaId, m.segments);
        }
        finish(j, "done"); emit transcriptReady(j->mediaId);
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this, native, program](QProcess::ProcessError e) {
        if (!m_current || e != QProcess::FailedToStart) return;
        Job* j = m_current; m_proc->deleteLater(); m_proc = nullptr;
        if (native) { j->engine = "python (fallback: could not start " + program + ")"; startJob(j, false); return; }
        finish(j, "failed", "python did not start: " + program);
    });
    m_proc->start(program, args);
    notify();
}

void TranscriptionManager::finish(Job* j, const QString& state, const QString& error) {
    j->state = state; j->error = error; j->finishedMs = QDateTime::currentMSecsSinceEpoch(); if (j == m_current) m_current = nullptr;
    if (state != "done") { bool f = false; MediaRecord m = m_store->media(j->mediaId, &f); if (f) { QJsonObject t = m.extra.value("transcript").toObject(); t["status"] = state; t["error"] = error; m.extra["transcript"] = t; m_store->saveMedia(m); } qWarning().noquote() << "[Transcribe]" << state << j->title << error; }
    else qInfo().noquote() << "[Transcribe] done" << j->title << j->level;
    notify(); QTimer::singleShot(0, this, &TranscriptionManager::pump);
}

QJsonObject TranscriptionManager::transcript(const QString& mediaId, bool* found) const {
    QFile f(transcriptPath(mediaId)); if (found) *found = false;
    if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object(); if (found) *found = !o.isEmpty(); return o;
}
bool TranscriptionManager::removeTranscript(const QString& mediaId) {
    QFile::remove(transcriptPath(mediaId));
    bool f = false; MediaRecord m = m_store->media(mediaId, &f); if (f) { m.extra.remove("transcript"); m.extra.remove("transcriptText"); m_store->saveMedia(m); }
    return true;
}

QJsonArray TranscriptionManager::wordsFlat(const QJsonObject& t) const {
    QJsonArray out; for (const QJsonValue& s : t.value("segments").toArray()) for (const QJsonValue& w : s.toObject().value("words").toArray()) out.append(w);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Segments
// ---------------------------------------------------------------------------------------------
QJsonObject TranscriptionManager::segments(const QString& mediaId) const {
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    QJsonObject o; if (!f) { o["error"] = "no such media"; o["status"] = 404; return o; }
    o["mediaId"] = mediaId; o["duration"] = m.durationSec; o["segments"] = m.segments; o["collections"] = m.extra.value("segmentCollections").toArray(); o["activeCollection"] = m.extra.value("activeCollection").toString("default");
    return o;
}

static QJsonArray normalizeSegments(const QJsonArray& in, double duration) {
    QJsonArray out; int n = 0;
    for (const QJsonValue& v : in) {
        QJsonObject s = v.toObject(); ++n;
        double a = s.value("startTime").toDouble(s.value("time_in").toDouble()), b = s.value("endTime").toDouble(s.value("time_out").toDouble());
        if (duration > 0) { a = qBound(0.0, a, duration); b = qBound(0.0, b, duration); }
        if (b <= a) continue;
        if (s.value("id").toString().isEmpty()) s["id"] = "seg-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        if (s.value("label").toString().isEmpty()) s["label"] = QString("Segment %1").arg(n);
        s["startTime"] = a; s["endTime"] = b; s["duration"] = b - a; s.remove("time_in"); s.remove("time_out");
        out.append(s);
    }
    return out;
}

QJsonObject TranscriptionManager::setSegments(const QString& mediaId, const QJsonArray& segs, const QString& collectionId) {
    bool f = false; MediaRecord m = m_store->media(mediaId, &f);
    if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    QJsonArray norm = normalizeSegments(segs, m.durationSec);
    { bool tf = false; const QJsonObject t = transcript(mediaId, &tf); if (tf) TranscriptTools::fillSegmentText(norm, t); }   // what is said in each
    const QString active = m.extra.value("activeCollection").toString("default");
    const QString target = collectionId.isEmpty() ? active : collectionId;
    QJsonArray cols = m.extra.value("segmentCollections").toArray(); bool found = false; QJsonArray outCols;
    for (const QJsonValue& v : cols) { QJsonObject c = v.toObject(); if (c.value("id").toString() == target) { c["segments"] = norm; found = true; } outCols.append(c); }
    if (!found) { QJsonObject c; c["id"] = target; c["name"] = target == "default" ? "Default" : target; c["segments"] = norm; outCols.append(c); }
    m.extra["segmentCollections"] = outCols; m.extra["activeCollection"] = active;
    if (target == active) m.segments = norm;
    m_store->saveMedia(m);
    return segments(mediaId);
}

QJsonObject TranscriptionManager::setCollections(const QString& mediaId, const QJsonArray& collections, const QString& activeId) {
    bool f = false; MediaRecord m = m_store->media(mediaId, &f);
    if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    bool tf = false; const QJsonObject t = transcript(mediaId, &tf);
    QJsonArray cols;
    for (const QJsonValue& v : collections) {
        QJsonObject c = v.toObject(); if (c.value("id").toString().isEmpty()) c["id"] = "col-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(6);
        QJsonArray segs = normalizeSegments(c.value("segments").toArray(), m.durationSec); if (tf) TranscriptTools::fillSegmentText(segs, t);
        c["segments"] = segs; cols.append(c);
    }
    QString active = activeId.isEmpty() ? m.extra.value("activeCollection").toString("default") : activeId;
    bool has = false; for (const QJsonValue& v : cols) if (v.toObject().value("id").toString() == active) { has = true; m.segments = v.toObject().value("segments").toArray(); }
    if (!has && !cols.isEmpty()) { active = cols[0].toObject().value("id").toString(); m.segments = cols[0].toObject().value("segments").toArray(); }
    if (cols.isEmpty()) m.segments = QJsonArray();
    m.extra["segmentCollections"] = cols; m.extra["activeCollection"] = active; m_store->saveMedia(m);
    return segments(mediaId);
}

QJsonObject TranscriptionManager::autoSplit(const QString& mediaId, const QJsonObject& opts) {
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    const QString mode = opts.value("mode").toString("interval"); const QString label = opts.value("label").toString("Part");
    const double dur = m.durationSec; QJsonArray segs;
    if (mode == "transcript") {
        bool tf = false; const QJsonObject t = transcript(mediaId, &tf);
        if (!tf) { QJsonObject e; e["error"] = "no transcript yet: transcribe the media first"; e["status"] = 409; return e; }
        const double minPause = opts.value("minPauseSec").toDouble(1.5), minSeg = opts.value("minSegmentSec").toDouble(20.0), maxSeg = opts.value("maxSegmentSec").toDouble(600.0);
        double segStart = -1, lastEnd = 0; QStringList texts; int n = 0;
        auto flush = [&](double end) { if (segStart < 0) return; QJsonObject s; s["label"] = QString("%1 %2").arg(label).arg(++n); s["startTime"] = qMax(0.0, segStart - 0.2); s["endTime"] = qMin(dur > 0 ? dur : end + 0.2, end + 0.2); s["source"] = "transcript"; s["text"] = texts.join(' ').left(200); segs.append(s); segStart = -1; texts.clear(); };
        for (const QJsonValue& v : t.value("segments").toArray()) {
            const QJsonObject ws = v.toObject(); const double a = ws.value("start").toDouble(), b = ws.value("end").toDouble();
            if (segStart >= 0 && ((a - lastEnd >= minPause && lastEnd - segStart >= minSeg) || (b - segStart > maxSeg))) flush(lastEnd);
            if (segStart < 0) segStart = a;
            texts << ws.value("text").toString(); lastEnd = b;
        }
        flush(lastEnd);
    } else {
        const double interval = qMax(5.0, opts.value("intervalSec").toDouble(600.0)), overlap = qMax(0.0, opts.value("overlapSec").toDouble(0.0));
        int n = 0; for (double a = 0; a < dur; a += interval) { QJsonObject s; s["label"] = QString("%1 %2").arg(label).arg(++n); s["startTime"] = qMax(0.0, a - (n > 1 ? overlap : 0.0)); s["endTime"] = qMin(dur, a + interval); s["source"] = "interval"; segs.append(s); }
    }
    QJsonArray merged = opts.value("replace").toBool(true) ? segs : m.segments; if (!opts.value("replace").toBool(true)) for (const QJsonValue& v : segs) merged.append(v);
    return setSegments(mediaId, merged, opts.value("collection").toString());
}

QJsonObject TranscriptionManager::segmentFromWords(const QString& mediaId, int firstWord, int lastWord, const QString& label) {
    bool tf = false; const QJsonObject t = transcript(mediaId, &tf);
    if (!tf) { QJsonObject e; e["error"] = "no transcript"; e["status"] = 409; return e; }
    const QJsonArray words = wordsFlat(t);
    if (words.isEmpty() || firstWord < 0 || lastWord >= words.size() || lastWord < firstWord) { QJsonObject e; e["error"] = "word range out of bounds"; e["status"] = 400; return e; }
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f); if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    QStringList text; for (int i = firstWord; i <= lastWord; ++i) text << words[i].toObject().value("word").toString().trimmed();
    QJsonObject s; s["label"] = label.isEmpty() ? text.join(' ').left(40) : label; s["startTime"] = qMax(0.0, words[firstWord].toObject().value("start").toDouble() - 0.15); s["endTime"] = words[lastWord].toObject().value("end").toDouble() + 0.15; s["source"] = "transcript"; s["text"] = text.join(' ').left(400); s["firstWord"] = firstWord; s["lastWord"] = lastWord;
    QJsonArray segs = m.segments; segs.append(s);
    return setSegments(mediaId, segs);
}
