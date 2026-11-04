#include "ingestmanager.h"
#include "../schedule/schedulestore.h"
#include "../core/loopwatchdog.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QPointer>
#include <QElapsedTimer>
#include <QDebug>

using namespace sched;

static const QStringList kVideoExts = {"mp4","mov","mxf","mkv","ts","m2ts","mpg","mpeg","avi","wmv","webm","m4v","mts"};
static const QStringList kAudioExts = {"mp3","wav","aac","flac","m4a","ogg"};
static const QStringList kImageExts = {"png","jpg","jpeg","bmp","gif","webp"};

IngestManager::IngestManager(ScheduleStore* store, const QString& dataDir, const QString& uploadDir, QObject* parent)
    : QObject(parent), m_store(store), m_dataDir(dataDir), m_uploadDir(uploadDir) {
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, [this]() { emit jobsChanged(); });
    loadConfig();
    QDir().mkpath(thumbDir());
    connect(&m_watchTimer, &QTimer::timeout, this, &IngestManager::pollWatchFolders);
    connect(&m_fsw, &QFileSystemWatcher::directoryChanged, this, [this](const QString&) { m_watchTimer.start(1500); });
    m_watchTimer.setInterval(3000);
    connect(&m_sweepTimer, &QTimer::timeout, this, [this]() { sweepMissingAsync(); });
    m_sweepTimer.start(qMax(1, m_cfg.value("sweepMinutes").toInt(5)) * 60 * 1000);
    connect(&m_driveTimer, &QTimer::timeout, this, &IngestManager::pollDrives);
    m_driveTimer.start(5000);
    if (m_store) connect(m_store, &ScheduleStore::mediaChanged, this, [this](const QString&) { m_knownDirty = true; });
    pollDrives();                          // primes m_knownVolumes on the worker (mountedVolumes() can hang on a dead share)
    setWatchFolders(watchFolders());       // arms the watcher from settings
    QTimer::singleShot(4000, this, &IngestManager::pollWatchFolders);
    QTimer::singleShot(20000, this, [this]() { sweepMissingAsync(); });
}

IngestManager::~IngestManager() { if (m_proc) { m_proc->kill(); } qDeleteAll(m_jobs); }

QString IngestManager::ffprobePath() const {
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffprobe.exe");
    if (QFileInfo::exists(local)) return local;
    const QString onPath = QStandardPaths::findExecutable("ffprobe");
    return onPath.isEmpty() ? "ffprobe" : onPath;
}
QString IngestManager::ffmpegPath() const {
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (QFileInfo::exists(local)) return local;
    const QString onPath = QStandardPaths::findExecutable("ffmpeg");
    return onPath.isEmpty() ? "ffmpeg" : onPath;
}
QString IngestManager::thumbDir() const { return QDir(m_dataDir).filePath("thumbs"); }

void IngestManager::loadConfig() {
    m_cfg = m_store ? m_store->settingJson("ingest.config") : QJsonObject();
    if (!m_cfg.contains("review")) m_cfg["review"] = "auto";
    if (!m_cfg.contains("thumbnails")) m_cfg["thumbnails"] = true;
    if (!m_cfg.contains("fullHash")) m_cfg["fullHash"] = false;
    if (!m_cfg.contains("sweepMinutes")) m_cfg["sweepMinutes"] = 5;
    if (!m_cfg.contains("removable")) { QJsonObject r; r["enabled"] = true; r["marker"] = "PLAYPLAY_INGEST"; r["type"] = "program"; r["copy"] = true; m_cfg["removable"] = r; }
}
QJsonObject IngestManager::config() const { return m_cfg; }
void IngestManager::setConfig(const QJsonObject& cfg) {
    for (auto it = cfg.begin(); it != cfg.end(); ++it) m_cfg[it.key()] = it.value();
    if (m_store) m_store->setSettingJson("ingest.config", m_cfg);
    m_sweepTimer.start(qMax(1, m_cfg.value("sweepMinutes").toInt(5)) * 60 * 1000);
}

// ---------------------------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------------------------
bool IngestManager::isMediaFile(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    return kVideoExts.contains(ext) || kAudioExts.contains(ext) || kImageExts.contains(ext);
}

QString IngestManager::typeFromPath(const QString& path, const QString& def) {
    const QString lower = QString(path).replace('\\', '/').toLower();
    if (lower.contains("/ads/") || lower.contains("/ad/") || lower.contains("commercial") || lower.contains("/spots/")) return "ad";
    if (lower.contains("promo")) return "promo";
    if (lower.contains("bumper")) return "bumper";
    if (lower.contains("station_id") || lower.contains("stationid") || lower.contains("/ids/") || lower.contains("/station ids/")) return "station_id";
    if (lower.contains("filler") || lower.contains("interstitial")) return "filler";
    if (lower.contains("/psa")) return "psa";
    const QString ext = QFileInfo(path).suffix().toLower();
    if (kImageExts.contains(ext)) return "image";
    return def.isEmpty() ? "program" : def;
}

// ---------------------------------------------------------------------------------------------
// Queue
// ---------------------------------------------------------------------------------------------
QJsonObject IngestManager::Job::toJson() const {
    QJsonObject o; o["id"] = id; o["path"] = path; o["name"] = QFileInfo(path).fileName(); o["type"] = type; o["origin"] = origin; o["state"] = state; o["error"] = error;
    o["mediaId"] = mediaId; o["batchId"] = batchId; o["queuedMs"] = double(queuedMs); o["finishedMs"] = double(finishedMs); return o;
}

int IngestManager::pending() const { int n = 0; for (Job* j : m_jobs) if (j->state == "queued" || j->state == "probing" || j->state == "hashing" || j->state == "thumbnail") ++n; return n; }

QJsonObject IngestManager::enqueue(const QString& rawPath, const QString& type, const QString& origin, const QJsonObject& meta, bool force, const QString& batchId) {
    const QString path = QDir::toNativeSeparators(QFileInfo(rawPath).absoluteFilePath());
    if (Job* ex = m_byPath.value(path)) { if (ex->state == "queued" || ex->state == "probing" || ex->state == "hashing" || ex->state == "thumbnail") return ex->toJson(); }
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->path = path; j->type = type.isEmpty() ? typeFromPath(path, "program") : type;
    j->origin = origin; j->meta = meta; j->queuedMs = QDateTime::currentMSecsSinceEpoch(); j->batchId = batchId;
    if (!QFileInfo(path).isFile()) { j->state = "failed"; j->error = "no such file"; j->finishedMs = j->queuedMs; m_jobs.append(j); m_byPath[path] = j; notify(); return j->toJson(); }
    bool found = false; const MediaRecord existing = m_store->mediaByPath(path, &found);
    if (found && !force && !existing.extra.value("missing").toBool()) {
        // Already in the library: apply any metadata (API ingests may carry it) and report it.
        if (!meta.isEmpty()) { QJsonObject merged = existing.toJson(); for (auto it = meta.begin(); it != meta.end(); ++it) merged[it.key()] = it.value(); MediaRecord m = MediaRecord::fromJson(merged); m.id = existing.id; m_store->saveMedia(m); }
        j->state = "skipped"; j->error = "already in the library"; j->mediaId = existing.id; j->finishedMs = j->queuedMs;
        m_jobs.append(j); m_byPath[path] = j; notify(); return j->toJson();
    }
    if (found) j->mediaId = existing.id;      // re-probe in place (missing or forced)
    m_jobs.append(j); m_byPath[path] = j;
    if (m_jobs.size() > 2000) { Job* old = m_jobs.takeFirst(); if (old != m_current) { m_byPath.remove(old->path); delete old; } else m_jobs.prepend(old); }
    notify();
    pump();
    return j->toJson();
}

int IngestManager::scanFolder(const QString& folderIn, bool recursive, const QString& defaultType, const QString& origin, const QString& batchId) {
    int queued = 0;
    QString folder = QDir::fromNativeSeparators(folderIn.trimmed()); if (folder.startsWith('"') && folder.endsWith('"') && folder.size() > 1) folder = folder.mid(1, folder.size() - 2);
    if (!QFileInfo(folder).isDir()) { qWarning().noquote() << "[Ingest] scan: not a folder" << folderIn; return 0; }
    QDirIterator it(folder, QDir::Files | QDir::Readable, recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!isMediaFile(path) || QFileInfo(path).fileName().startsWith('.')) continue;
        bool found = false; const MediaRecord ex = m_store->mediaByPath(QDir::toNativeSeparators(path), &found);
        if (found && !ex.extra.value("missing").toBool()) continue;
        enqueue(path, typeFromPath(path, defaultType), origin, QJsonObject(), false, batchId);
        ++queued;
    }
    return queued;
}

QJsonArray IngestManager::jobsJson(int limit) const {
    QJsonArray a; const int start = qMax(0, m_jobs.size() - limit);
    for (int i = start; i < m_jobs.size(); ++i) a.append(m_jobs[i]->toJson());
    return a;
}

QJsonObject IngestManager::batchJson(const QString& batchId) const {
    QJsonObject o; o["id"] = batchId; QJsonArray items; int done = 0, failed = 0, pendingN = 0;
    for (Job* j : m_jobs) if (j->batchId == batchId) { items.append(j->toJson()); if (j->state == "done" || j->state == "skipped") ++done; else if (j->state == "failed") ++failed; else ++pendingN; }
    o["items"] = items; o["done"] = done; o["failed"] = failed; o["pending"] = pendingN; o["status"] = pendingN ? "running" : (failed ? "failed" : "done"); return o;
}

void IngestManager::clearFinished() {
    for (int i = m_jobs.size() - 1; i >= 0; --i) { Job* j = m_jobs[i]; if (j != m_current && (j->state == "done" || j->state == "failed" || j->state == "skipped")) { m_byPath.remove(j->path); m_jobs.removeAt(i); delete j; } }
    notify();
}

void IngestManager::pump() {
    if (m_current || m_proc) return;
    Job* next = nullptr;
    for (Job* j : m_jobs) if (j->state == "queued") { next = j; break; }
    if (!next) { emit scanDone(); return; }
    m_current = next; next->state = "probing"; notify();
    m_proc = new QProcess(this);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &IngestManager::onProbeFinished);
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { if (m_current && m_proc && m_proc->state() == QProcess::NotRunning) onProbeFinished(-1, QProcess::CrashExit); });
    m_proc->start(ffprobePath(), {"-v", "quiet", "-print_format", "json", "-show_format", "-show_streams", m_current->path});
}

void IngestManager::onProbeFinished(int code, QProcess::ExitStatus st) {
    if (!m_proc || !m_current) return;
    const QJsonObject j = QJsonDocument::fromJson(m_proc->readAllStandardOutput()).object();
    m_proc->deleteLater(); m_proc = nullptr;
    Job* job = m_current;
    if (st != QProcess::NormalExit || (code != 0 && j.isEmpty())) { finishJob(job, "failed", "ffprobe failed (exit " + QString::number(code) + ")"); return; }
    applyRecord(job, j);
}

void IngestManager::applyRecord(Job* job, const QJsonObject& probe) {
    const QJsonObject fmt = probe.value("format").toObject();
    bool found = false;
    MediaRecord m = job->mediaId.isEmpty() ? MediaRecord() : m_store->media(job->mediaId, &found);
    if (!found) m = MediaRecord();
    const QFileInfo fi(job->path);
    m.path = job->path;
    if (m.title.isEmpty()) m.title = fi.completeBaseName();
    if (m.type.isEmpty() || m.type == "video") m.type = job->type;
    m.durationSec = fmt.value("duration").toString().toDouble();
    m.fileSizeBytes = fmt.value("size").toString().toLongLong(); if (m.fileSizeBytes <= 0) m.fileSizeBytes = fi.size();
    m.width = 0; m.height = 0; m.videoCodec.clear(); m.audioCodec.clear();
    int audioChannels = 0;
    for (const auto& sv : probe.value("streams").toArray()) {
        const QJsonObject s = sv.toObject();
        if (s.value("codec_type").toString() == "video" && m.width == 0 && s.value("disposition").toObject().value("attached_pic").toInt() == 0) {
            m.width = s.value("width").toInt(); m.height = s.value("height").toInt(); m.videoCodec = s.value("codec_name").toString(); m.frameRate = s.value("avg_frame_rate").toString();
        } else if (s.value("codec_type").toString() == "audio" && m.audioCodec.isEmpty()) { m.audioCodec = s.value("codec_name").toString(); audioChannels = s.value("channels").toInt(); }
    }
    const QString ext = fi.suffix().toLower();
    if (kImageExts.contains(ext)) { if (m.durationSec <= 0.0) m.durationSec = 10.0; m.type = job->meta.value("type").toString(m.type == "program" ? "image" : m.type); }
    if (m.durationSec <= 0.0 && !kImageExts.contains(ext)) { finishJob(job, "failed", "no duration (not a media file?)"); return; }
    // metadata from the request (API ingest / watch folder defaults)
    for (auto it = job->meta.begin(); it != job->meta.end(); ++it) {
        const QString k = it.key();
        if (k == "title" || k == "display_title") { if (!it.value().toString().isEmpty()) m.title = it.value().toString(); }
        else if (k == "type") m.type = it.value().toString(m.type);
        else if (k == "client") m.client = it.value().toString();
        else if (k == "series" || k == "programSeries") m.series = it.value().toString();
        else if (k == "priority") m.priority = it.value().toInt();
        else if (k == "allowedChannels") { m.allowedChannels.clear(); for (const auto& v : it.value().toArray()) m.allowedChannels << v.toString(); }
        else if (k == "availableStartMs") m.availableStartMs = qint64(it.value().toDouble());
        else if (k == "availableEndMs") m.availableEndMs = qint64(it.value().toDouble());
        else if (k == "episodeDate") m.episodeDate = QDate::fromString(it.value().toString().left(10), Qt::ISODate);
        else m.extra[k] = it.value();
    }
    m.extra["audioChannels"] = audioChannels;
    m.extra["bitrate"] = fmt.value("bit_rate").toString().toDouble();
    m.extra["container"] = fmt.value("format_name").toString();
    m.extra["missing"] = false;
    m.extra["origin"] = job->origin;
    m.extra["mtimeMs"] = double(fi.lastModified().toMSecsSinceEpoch());
    m.extra["filename"] = fi.fileName();
    if (!m.extra.contains("status")) m.extra["status"] = job->meta.value("status").toString(m_cfg.value("review").toString("auto") == "require" ? "review" : "approved");
    m.extra["playoutStatus"] = "ready";
    const QJsonObject tags = fmt.value("tags").toObject();
    if (!tags.value("title").toString().isEmpty() && m.title == fi.completeBaseName() && job->meta.value("title").toString().isEmpty()) m.extra["embeddedTitle"] = tags.value("title").toString();

    // hash (quick by default) + relink of a missing record with the same content
    job->state = "hashing"; notify();
    const QString h = quickHash(job->path, m_cfg.value("fullHash").toBool());
    m.extra["hash"] = h; m.extra["hashKind"] = m_cfg.value("fullHash").toBool() ? "md5" : "quick";
    if (m.id.isEmpty() && !h.isEmpty()) {
        for (const MediaRecord& r : m_store->allMedia(true)) {
            if (!r.extra.value("missing").toBool()) continue;
            const bool sameHash = r.extra.value("hash").toString() == h;
            const bool sameName = r.fileSizeBytes == m.fileSizeBytes && QFileInfo(r.path).fileName().compare(fi.fileName(), Qt::CaseInsensitive) == 0;
            if (sameHash || sameName) {
                MediaRecord keep = r; keep.path = m.path; keep.fileSizeBytes = m.fileSizeBytes; keep.durationSec = m.durationSec; keep.width = m.width; keep.height = m.height;
                keep.frameRate = m.frameRate; keep.videoCodec = m.videoCodec; keep.audioCodec = m.audioCodec;
                for (auto it = m.extra.begin(); it != m.extra.end(); ++it) keep.extra[it.key()] = it.value();
                keep.extra["missing"] = false; keep.extra["relinkedFrom"] = r.path; keep.extra["relinkedMs"] = double(QDateTime::currentMSecsSinceEpoch());
                m = keep; job->origin = "relink";
                qInfo().noquote() << "[Ingest] relinked" << r.title << "->" << m.path;
                break;
            }
        }
    }
    if (!m_store->saveMedia(m)) { finishJob(job, "failed", "database write failed"); return; }
    if (m.id.isEmpty()) m = m_store->mediaByPath(m.path);
    job->mediaId = m.id;

    // thumbnail
    if (m_cfg.value("thumbnails").toBool(true) && (m.width > 0 || kImageExts.contains(ext))) {
        job->state = "thumbnail"; notify();
        const QString out = QDir(thumbDir()).filePath(m.id + ".jpg");
        const double at = kImageExts.contains(ext) ? 0.0 : qMax(0.0, qMin(m.durationSec * 0.1, m.durationSec - 0.5));
        m_proc = new QProcess(this);
        m_thumbTarget = out;
        connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &IngestManager::onThumbFinished);
        QStringList args = {"-y", "-v", "quiet"};
        if (at > 0) args << "-ss" << QString::number(at, 'f', 2);
        args << "-i" << job->path << "-frames:v" << "1" << "-vf" << "scale=320:-2" << "-q:v" << "4" << out;
        m_proc->start(ffmpegPath(), args);
        if (!m_proc->waitForStarted(2000)) { m_proc->deleteLater(); m_proc = nullptr; finishJob(job, "done"); }
        return;
    }
    finishJob(job, "done");
}

void IngestManager::onThumbFinished(int code, QProcess::ExitStatus) {
    if (!m_proc || !m_current) return;
    m_proc->deleteLater(); m_proc = nullptr;
    Job* job = m_current;
    if (code == 0 && QFileInfo::exists(m_thumbTarget)) {
        bool f = false; MediaRecord m = m_store->media(job->mediaId, &f);
        if (f) { m.thumbnailPath = QDir::toNativeSeparators(m_thumbTarget); m.extra["thumbnailUrl"] = "/thumbs/" + m.id + ".jpg"; m_store->saveMedia(m); }
    }
    finishJob(job, "done");
}

void IngestManager::finishJob(Job* j, const QString& state, const QString& error) {
    j->state = state; j->error = error; j->finishedMs = QDateTime::currentMSecsSinceEpoch();
    if (state == "done") { qInfo().noquote() << "[Ingest]" << j->origin << "added" << j->path; emit mediaIngested(j->mediaId, j->origin); }
    else qWarning().noquote() << "[Ingest]" << j->origin << state << j->path << error;
    m_current = nullptr;
    notify();
    QTimer::singleShot(0, this, &IngestManager::pump);
}

QString IngestManager::quickHash(const QString& path, bool full) const {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Md5);
    if (full) { h.addData(&f); return QString::fromLatin1(h.result().toHex()); }
    const qint64 size = f.size(); const qint64 chunk = 4 * 1024 * 1024;
    h.addData(f.read(chunk));
    if (size > chunk * 2) { f.seek(size - chunk); h.addData(f.read(chunk)); }
    else if (size > chunk) { h.addData(f.read(size - chunk)); }
    h.addData(QByteArray::number(size));
    return QString::fromLatin1(h.result().toHex());
}

bool IngestManager::regenerateThumbnail(const QString& mediaId) {
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    if (!f || !QFileInfo(m.path).isFile()) return false;
    QJsonObject meta; meta["title"] = m.title;
    enqueue(m.path, m.type, "rethumb", meta, true);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Watch folders
// ---------------------------------------------------------------------------------------------
QJsonArray IngestManager::watchFolders() const { return m_store ? m_store->settingJson("ingest.watch").value("folders").toArray() : QJsonArray(); }

void IngestManager::setWatchFolders(const QJsonArray& folders) {
    QJsonObject o; o["folders"] = folders; if (m_store) m_store->setSettingJson("ingest.watch", o);
    // No stat here: a folder on a dead share would hold the loop. The next walk (on the worker) reports which roots
    // exist and applyWatch arms the QFileSystemWatcher for them.
    if (!m_fsw.directories().isEmpty()) m_fsw.removePaths(m_fsw.directories());
    if (!folders.isEmpty()) m_watchTimer.start(); else m_watchTimer.stop();   // poll even before the folder exists
    m_watchPending.clear();
}

void IngestManager::rescanWatchFolders() { m_watchPending.clear(); pollWatchFolders(); }

void IngestManager::refreshKnownPaths() {
    if (!m_knownDirty) return;
    m_knownPaths.clear();
    if (m_store) for (const MediaRecord& r : m_store->allMedia(true)) if (!r.extra.value("missing").toBool()) m_knownPaths.insert(r.path);
    m_knownDirty = false;
}

IngestManager::WatchInput IngestManager::watchInput() {
    WatchInput in;
    in.folders = watchFolders();
    refreshKnownPaths();
    in.known = m_knownPaths;                                   // implicitly shared: the worker keeps its own reference
    for (auto it = m_byPath.cbegin(); it != m_byPath.cend(); ++it) if (it.value()->state != "failed") in.jobs.insert(it.key());
    for (auto it = m_watchPending.cbegin(); it != m_watchPending.cend(); ++it) in.pendingSizes.insert(it.key(), it.value().size);
    return in;
}

// Worker thread. The walk of every enabled folder: media files that are not in the library (or are marked missing)
// and not already queued, with their size, type and, when the size matches what the loop saw last time (the file is
// about to count as settled), whether the file opens for reading. Roots that exist are reported so the loop can arm
// the watcher. Nothing here touches the store or the job list: those are the sets the loop copied in.
IngestManager::WatchResult IngestManager::walkWatchFolders(const WatchInput& in, const BlockingWorker::Token& cancel) {
    WatchResult r;
    QElapsedTimer t; t.start();
    for (const QJsonValue& v : in.folders) {
        if (cancel->load()) break;
        const QJsonObject w = v.toObject();
        if (!w.value("enabled").toBool(true)) continue;
        const QString root = QDir::fromNativeSeparators(w.value("path").toString().trimmed()); const QString type = w.value("type").toString("program");
        if (!QFileInfo(root).isDir()) continue;
        r.roots << root;
        QJsonObject meta; if (!w.value("client").toString().isEmpty()) meta["client"] = w.value("client"); if (!w.value("series").toString().isEmpty()) meta["series"] = w.value("series");
        QDirIterator it(root, QDir::Files | QDir::Readable, w.value("recursive").toBool(true) ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (it.hasNext()) {
            if ((++r.walked & 255) == 0 && cancel->load()) break;
            const QString path = QDir::toNativeSeparators(it.next());
            const QFileInfo fi(path);
            if (!isMediaFile(path) || fi.fileName().startsWith('.') || fi.fileName().endsWith(".part")) continue;
            if (in.known.contains(path)) continue;
            if (in.jobs.contains(path)) continue;
            WatchCandidate c; c.path = path; c.size = fi.size(); c.type = typeFromPath(path, type); c.meta = meta;
            if (c.size > 0 && in.pendingSizes.value(path, -1) == c.size) { QFile f(path); c.openable = f.open(QIODevice::ReadOnly); }   // settled: still being written / locked?
            r.files.append(c);
        }
    }
    r.ms = t.elapsed();
    return r;
}

void IngestManager::applyWatch(const WatchResult& r) {
    WatchdogScope tag("ingest:watch");
    for (const QString& root : r.roots) if (!m_fsw.directories().contains(root)) m_fsw.addPath(root);
    refreshKnownPaths();
    QSet<QString> seen;
    for (const WatchCandidate& c : r.files) {
        seen.insert(c.path);
        // Re-check against the live state: the walk saw a snapshot, and a file can be uploaded or queued meanwhile.
        if (m_knownPaths.contains(c.path)) continue;
        if (Job* j = m_byPath.value(c.path)) if (j->state != "failed") continue;
        Pending& p = m_watchPending[c.path];
        if (p.size == c.size && c.size > 0) ++p.stable; else { p.size = c.size; p.stable = 0; }
        p.type = c.type;
        if (p.stable >= 1) {
            if (!c.openable) continue;                       // still being written / locked
            enqueue(c.path, p.type, "watch", c.meta);
            m_watchPending.remove(c.path);
        }
    }
    for (auto it = m_watchPending.begin(); it != m_watchPending.end();) { if (!seen.contains(it.key())) it = m_watchPending.erase(it); else ++it; }
    emit watchPolled(r.files.size(), r.ms);
}

void IngestManager::pollWatchFolders() {
    // One walk at a time, and only from a snapshot that includes the last walk's outcome: a walk posted while one runs
    // would carry the settle sizes from before that result was applied. The timer fires again in 3 s.
    if (m_watchWorker.isBusy("walk")) return;
    const WatchInput in = watchInput();
    QPointer<IngestManager> self(this);
    m_watchWorker.post<WatchResult>("walk",
        [in](const BlockingWorker::Token& cancel) { return walkWatchFolders(in, cancel); },
        [self](WatchResult r) { if (self) self->applyWatch(r); });
}

// ---------------------------------------------------------------------------------------------
// Removable drives
// ---------------------------------------------------------------------------------------------
QJsonArray IngestManager::removableVolumes() const { return m_volumesJson; }

IngestManager::DriveInput IngestManager::driveInput() const {
    DriveInput in; in.known = m_knownVolumes; in.removable = m_cfg.value("removable").toObject(); in.uploadDir = m_uploadDir; in.primed = m_volumesPrimed; return in;
}

// Worker thread. mountedVolumes() (which can hang on a dead network or removable volume), the volume listing with its
// marker stat, and for a volume that was not mounted at the last poll the `<marker>` folder's media files: copied into
// the upload area when `copy` is on (the drive will be pulled, the library must not point at it), listed otherwise.
IngestManager::DriveResult IngestManager::enumerateDrives(const DriveInput& in, const BlockingWorker::Token& cancel) {
    DriveResult r;
    QElapsedTimer t; t.start();
    const QString marker = in.removable.value("marker").toString("PLAYPLAY_INGEST");
    const QString type = in.removable.value("type").toString("program");
    for (const QStorageInfo& si : QStorageInfo::mountedVolumes()) {
        if (cancel->load()) break;
        if (!si.isValid() || !si.isReady()) continue;
        r.now.insert(si.rootPath());
        if (!si.isRoot()) {
            QJsonObject o; o["root"] = si.rootPath(); o["name"] = si.displayName(); o["fs"] = QString::fromUtf8(si.fileSystemType()); o["bytesTotal"] = double(si.bytesTotal()); o["bytesFree"] = double(si.bytesAvailable());
            o["hasMarker"] = QFileInfo(QDir(si.rootPath()).filePath(marker)).isDir();
            r.volumes.append(o);
        }
        if (!in.primed || in.known.contains(si.rootPath())) continue;
        DriveVolume v; v.root = si.rootPath(); v.name = si.displayName();
        if (in.removable.value("enabled").toBool(true)) {
            v.src = QDir(si.rootPath()).filePath(marker);
            if (QFileInfo(v.src).isDir()) {
                v.ingest = true;
                v.copy = in.removable.value("copy").toBool(true);
                if (v.copy) {
                    v.dst = QDir(in.uploadDir).filePath("removable/" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmm") + "-" + QFileInfo(si.displayName().isEmpty() ? "drive" : si.displayName()).fileName());
                    QDir().mkpath(v.dst);
                }
                QDirIterator it(v.src, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    if (cancel->load()) break;
                    const QString p = it.next(); if (!isMediaFile(p)) continue;
                    if (v.copy) {
                        const QString rel = QDir(v.src).relativeFilePath(p); const QString target = QDir(v.dst).filePath(rel);
                        QDir().mkpath(QFileInfo(target).path());
                        if (QFile::copy(p, target)) v.files.append({target, typeFromPath(p, type)});
                    } else {
                        if (QFileInfo(p).fileName().startsWith('.')) continue;          // as scanFolder
                        v.files.append({QDir::toNativeSeparators(p), typeFromPath(p, type)});
                    }
                }
            }
        }
        r.appeared.append(v);
    }
    r.ms = t.elapsed();
    return r;
}

void IngestManager::applyDrives(const DriveResult& r) {
    WatchdogScope tag("ingest:drives");
    for (const DriveVolume& v : r.appeared) {
        qInfo().noquote() << "[Ingest] volume appeared" << v.root << v.name;
        if (!v.ingest) continue;
        if (v.copy) {
            for (const auto& f : v.files) enqueue(f.first, f.second, "removable");
            qInfo().noquote() << "[Ingest] removable: copied" << v.files.size() << "file(s) from" << v.src << "to" << v.dst;
        } else {
            refreshKnownPaths();                               // scanFolder's "already in the library and not missing" skip
            for (const auto& f : v.files) if (!m_knownPaths.contains(f.first)) enqueue(f.first, f.second, "removable");
        }
    }
    m_knownVolumes = r.now;
    m_volumesJson = r.volumes;
    m_volumesPrimed = true;
    emit drivesPolled(r.volumes.size(), r.ms);
}

void IngestManager::pollDrives() {
    // One poll at a time: a poll posted while a drive is being copied would not know that volume yet and ingest it
    // twice. The timer fires again in 5 s.
    if (m_driveWorker.isBusy("drives")) return;
    const DriveInput in = driveInput();
    QPointer<IngestManager> self(this);
    m_driveWorker.post<DriveResult>("drives",
        [in](const BlockingWorker::Token& cancel) { return enumerateDrives(in, cancel); },
        [self](DriveResult r) { if (self) self->applyDrives(r); });
}

// ---------------------------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------------------------
QList<IngestManager::SweepRow> IngestManager::sweepRows() const {
    QList<SweepRow> rows;
    if (!m_store) return rows;
    for (const MediaRecord& m : m_store->allMedia(true)) {
        if (m.type == "live" || m.path.startsWith("live://")) continue;
        rows.append({m.id, m.path, m.extra.value("missing").toBool()});
    }
    return rows;
}

// Worker thread (or in place for the synchronous call): one stat per library file.
IngestManager::SweepResult IngestManager::sweepStat(const QList<SweepRow>& rows, const BlockingWorker::Token& cancel) {
    SweepResult r;
    QElapsedTimer t; t.start();
    for (const SweepRow& row : rows) {
        if ((++r.rows & 63) == 0 && cancel->load()) break;
        const bool gone = !QFileInfo(row.path).isFile();
        if (gone) ++r.missing;
        if (gone != row.was) r.changed.append({row.id, row.path, gone});
    }
    r.ms = t.elapsed();
    return r;
}

int IngestManager::applySweep(const SweepResult& r) {
    WatchdogScope tag("ingest:sweep");
    int changed = 0;
    for (const SweepChange& c : r.changed) {
        bool f = false; MediaRecord m = m_store->media(c.id, &f);
        if (!f || m.path != c.path) continue;                  // deleted or relinked while the stats ran
        if (m.extra.value("missing").toBool() == c.gone) continue;
        m.extra["missing"] = c.gone; m.extra["missingSinceMs"] = c.gone ? double(QDateTime::currentMSecsSinceEpoch()) : 0.0;
        if (m_store->saveMedia(m)) ++changed;
    }
    if (changed) qInfo().noquote() << "[Ingest] sweep:" << r.missing << "missing," << changed << "changed" << "(" << r.rows << "files," << r.ms << "ms )";
    emit sweepDone(r.missing, changed);
    return r.missing;
}

int IngestManager::sweepMissing() {
    return applySweep(sweepStat(sweepRows(), std::make_shared<std::atomic<bool>>(false)));
}

void IngestManager::sweepMissingAsync(std::function<void(int)> done) {
    if (m_sweepWorker.isBusy("sweep")) return;                 // one at a time; the running one answers for this period
    const QList<SweepRow> rows = sweepRows();
    QPointer<IngestManager> self(this);
    m_sweepWorker.post<SweepResult>("sweep",
        [rows](const BlockingWorker::Token& cancel) { return sweepStat(rows, cancel); },
        [self, done](SweepResult r) { if (!self) return; const int missing = self->applySweep(r); if (done) done(missing); });
}

QJsonObject IngestManager::workerStats() const {
    QJsonObject o;
    o["sweep"] = m_sweepWorker.statsJson(); o["watch"] = m_watchWorker.statsJson(); o["drives"] = m_driveWorker.statsJson();
    o["knownPaths"] = m_knownPaths.size(); o["knownDirty"] = m_knownDirty; o["watchPending"] = m_watchPending.size(); o["volumes"] = m_volumesJson.size();
    return o;
}

bool IngestManager::relink(const QString& mediaId, const QString& newPath) {
    bool f = false; MediaRecord m = m_store->media(mediaId, &f);
    if (!f || !QFileInfo(newPath).isFile()) return false;
    QJsonObject meta; meta["title"] = m.title;
    Job* j = nullptr;
    const QString path = QDir::toNativeSeparators(QFileInfo(newPath).absoluteFilePath());
    m.path = path; m.extra["missing"] = false; m.extra["relinkedMs"] = double(QDateTime::currentMSecsSinceEpoch());
    m_store->saveMedia(m);
    enqueue(path, m.type, "relink", meta, true);
    Q_UNUSED(j);
    return true;
}

QJsonArray IngestManager::duplicates() const {
    QHash<QString, QStringList> byHash, byName;
    for (const MediaRecord& m : m_store->allMedia(true)) {
        if (m.type == "live") continue;
        const QString h = m.extra.value("hash").toString();
        if (!h.isEmpty()) byHash[h] << m.id;
        byName[QString::number(m.fileSizeBytes) + "|" + QFileInfo(m.path).fileName().toLower()] << m.id;
    }
    QJsonArray out; QSet<QString> covered;
    for (auto it = byHash.begin(); it != byHash.end(); ++it) if (it.value().size() > 1) { QJsonObject g; g["kind"] = "hash"; g["key"] = it.key(); QJsonArray ids; for (const QString& id : it.value()) { ids.append(id); covered.insert(id); } g["ids"] = ids; out.append(g); }
    for (auto it = byName.begin(); it != byName.end(); ++it) if (it.value().size() > 1) { bool allCovered = true; for (const QString& id : it.value()) if (!covered.contains(id)) allCovered = false; if (allCovered) continue; QJsonObject g; g["kind"] = "name"; g["key"] = it.key(); QJsonArray ids; for (const QString& id : it.value()) ids.append(id); g["ids"] = ids; out.append(g); }
    return out;
}
