#ifndef INGESTMANAGER_H
#define INGESTMANAGER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QPair>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QFileSystemWatcher>
#include <QProcess>
#include <functional>
#include "../schedule/schedulemodel.h"
#include "blockingworker.h"

namespace sched { class ScheduleStore; }

/**
 * IngestManager — how files become library records.
 *
 * One queue, several feeders:
 *   - explicit scans (POST /media/scan: folder or file list), drag-and-drop, web uploads
 *   - watch folders (settings `ingest.watchFolders`: [{path, type, recursive, enabled}]) — a file is
 *     picked up once its size has been stable for two polls and it can be opened for reading
 *   - removable drives (settings `ingest.removable`: {enabled, marker, type}) — a volume that appears
 *     with a `<marker>` folder (default PLAYPLAY_INGEST) is copied into the upload area and probed
 *   - mesh / Star fetches and the station-compatible POST /api/media/ingest
 *
 * Every job runs: probe (ffprobe → duration, raster, rate, codecs) → quick hash (md5 of the first and
 * last 4 MiB + size; `ingest.fullHash` switches to a full md5 like AN-FS) → thumbnail (ffmpeg, 320 px,
 * at 10 % of the duration) → library record with status approved|review, playout_status ready.
 * A probed file whose hash (or size + filename) matches a record marked `missing` relinks that record
 * instead of creating a duplicate. A sweep every 5 minutes marks records whose file is gone.
 *
 * Threads (perf-supervisor PS-8 / W14): the three things here that touch the file system in bulk, the sweep's stat of
 * every library file, the recursive walk of every watch folder (every 3 s, and 1.5 s after a change) and
 * QStorageInfo::mountedVolumes() plus the removable-drive copy (every 5 s), ran on the Supervisor's loop and could
 * hold it for seconds (an unreachable UNC watch path: ~21 s per stat). Each now runs on its own BlockingWorker as a
 * pure function of plain inputs copied on the loop (sweepStat, walkWatchFolders, enumerateDrives); the result comes
 * back to the loop, which re-checks against the live state and applies it (applySweep, applyWatch, applyDrives). The
 * store, the job list and every QObject stay loop-only. The synchronous sweepMissing() (an operator's REST call) runs
 * the same two functions in place, so both paths produce the same records.
 */
class IngestManager : public QObject {
    Q_OBJECT
public:
    struct Job {
        QString id, path, type, origin;           // origin: scan|watch|upload|removable|mesh|star|api|relink
        QJsonObject meta;                          // extra fields to merge (client, series, status, allowedChannels, ...)
        QString state = "queued";                  // queued|probing|hashing|thumbnail|done|failed|skipped
        QString error, mediaId, batchId;
        qint64 queuedMs = 0, finishedMs = 0;
        QJsonObject toJson() const;
    };

    explicit IngestManager(sched::ScheduleStore* store, const QString& dataDir, const QString& uploadDir, QObject* parent = nullptr);
    ~IngestManager() override;

    QString ffprobePath() const;
    QString ffmpegPath() const;
    QString thumbDir() const;

    /// Queue one file; returns the job (existing record → state "skipped" unless `force`).
    QJsonObject enqueue(const QString& path, const QString& type, const QString& origin, const QJsonObject& meta = QJsonObject(), bool force = false, const QString& batchId = QString());
    int scanFolder(const QString& folder, bool recursive, const QString& defaultType, const QString& origin = "scan", const QString& batchId = QString());
    static QString typeFromPath(const QString& path, const QString& def);
    static bool isMediaFile(const QString& path);

    QJsonArray jobsJson(int limit = 400) const;
    QJsonObject batchJson(const QString& batchId) const;
    void clearFinished();
    int pending() const;

    // watch folders / removable drives / options (settings-backed)
    QJsonArray watchFolders() const;
    void setWatchFolders(const QJsonArray& folders);
    QJsonObject config() const;                    // {review, fullHash, thumbnails, removable:{enabled,marker,type}, sweepMinutes}
    void setConfig(const QJsonObject& cfg);
    void rescanWatchFolders();                     // clears the settle state and walks again (on the worker)
    QJsonArray removableVolumes() const;           // as of the last drive poll (<= 5 s old; never stats on the loop)

    // library maintenance
    int sweepMissing();                            // marks records whose file is gone; returns the number missing (in place, blocks)
    /// The same sweep with the stats on the worker; `done(missing)` runs on this object's thread. A sweep already
    /// running or queued is kept (one at a time) and `done` is dropped.
    void sweepMissingAsync(std::function<void(int)> done = {});
    bool relink(const QString& mediaId, const QString& newPath);
    QJsonArray duplicates() const;                 // [{key, kind: hash|name, ids:[...]}]
    bool regenerateThumbnail(const QString& mediaId);

    /// {sweep, watch, drives: BlockingWorker::statsJson(), knownPaths, watchPending, volumes}: diagnostics.
    QJsonObject workerStats() const;

signals:
    void jobsChanged();
    void mediaIngested(const QString& mediaId, const QString& origin);
    void scanDone();
    void sweepDone(int missing, int changed);                  // after a sweep (either path) was applied
    void watchPolled(int candidates, qint64 workerMs);         // after a watch-folder walk was applied
    void drivesPolled(int volumes, qint64 workerMs);           // after a drive poll was applied

private:
    // --- work that crosses to the workers: plain values in, plain values out (no QObject, no store) ---
    struct SweepRow { QString id, path; bool was = false; };
    struct SweepChange { QString id, path; bool gone = false; };
    struct SweepResult { int missing = 0; int rows = 0; QList<SweepChange> changed; qint64 ms = 0; };
    struct WatchInput { QJsonArray folders; QSet<QString> known, jobs; QHash<QString, qint64> pendingSizes; };
    struct WatchCandidate { QString path, type; qint64 size = 0; bool openable = false; QJsonObject meta; };
    struct WatchResult { QStringList roots; QList<WatchCandidate> files; int walked = 0; qint64 ms = 0; };
    struct DriveInput { QSet<QString> known; QJsonObject removable; QString uploadDir; bool primed = false; };
    struct DriveVolume { QString root, name, src, dst; bool copy = false; bool ingest = false; QList<QPair<QString, QString>> files; };   // files: (path, type)
    struct DriveResult { QSet<QString> now; QJsonArray volumes; QList<DriveVolume> appeared; qint64 ms = 0; };
    static SweepResult sweepStat(const QList<SweepRow>& rows, const BlockingWorker::Token& cancel);
    static WatchResult walkWatchFolders(const WatchInput& in, const BlockingWorker::Token& cancel);
    static DriveResult enumerateDrives(const DriveInput& in, const BlockingWorker::Token& cancel);
    QList<SweepRow> sweepRows() const;
    int applySweep(const SweepResult& r);
    WatchInput watchInput();
    void applyWatch(const WatchResult& r);
    DriveInput driveInput() const;
    void applyDrives(const DriveResult& r);
    void refreshKnownPaths();                      // m_knownPaths from the store when a media write made it stale

    BlockingWorker m_sweepWorker{"ingest-sweep"}, m_watchWorker{"ingest-watch"}, m_driveWorker{"ingest-drives"};
    QSet<QString> m_knownPaths;                    // paths of records that are in the library and not missing
    bool m_knownDirty = true;
    QJsonArray m_volumesJson;                      // removableVolumes() as of the last drive poll
    bool m_volumesPrimed = false;                  // the first poll only records what is mounted
    sched::ScheduleStore* m_store;
    QString m_dataDir, m_uploadDir;
    QList<Job*> m_jobs;
    QHash<QString, Job*> m_byPath;
    Job* m_current = nullptr;
    QProcess* m_proc = nullptr;
    QTimer m_notify, m_watchTimer, m_sweepTimer, m_driveTimer;
    QFileSystemWatcher m_fsw;
    struct Pending { qint64 size = -1; int stable = 0; QString type; };
    QHash<QString, Pending> m_watchPending;        // files seen in watch folders, waiting to settle
    QSet<QString> m_knownVolumes;
    QJsonObject m_cfg;
    QString m_thumbTarget;

    void pump();
    void onProbeFinished(int code, QProcess::ExitStatus st);
    void onThumbFinished(int code, QProcess::ExitStatus st);
    void finishJob(Job* j, const QString& state, const QString& error = QString());
    void applyRecord(Job* j, const QJsonObject& probe);
    QString quickHash(const QString& path, bool full) const;
    bool makeThumbnail(const QString& path, const QString& mediaId, double durationSec, QString* outPath);
    void pollWatchFolders();                       // posts a walk unless one is running (the timer re-fires)
    void pollDrives();                             // posts a drive enumeration unless one is running
    void loadConfig();
    void notify() { if (!m_notify.isActive()) m_notify.start(300); }
};

#endif // INGESTMANAGER_H
