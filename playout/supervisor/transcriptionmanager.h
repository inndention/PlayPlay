#ifndef TRANSCRIPTIONMANAGER_H
#define TRANSCRIPTIONMANAGER_H

#include <QObject>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QTimer>
#include <functional>
#include <memory>

namespace sched { class ScheduleStore; }

/**
 * TranscriptionManager — speech-to-text for library media with faster-whisper (scripts/transcribe.py),
 * plus the segment tools that build on a transcript.
 *
 * Accuracy levels: fast (tiny), balanced (small), accurate (medium), best (large-v3); each level fixes
 * the model, beam size and VAD. Jobs run one at a time in a Python subprocess; progress lines are
 * parsed into the job. The transcript JSON (segments with word timestamps) is kept at
 * <dataDir>/transcripts/<mediaId>.json and summarised on the media record
 * (extra.transcript = {status, level, model, language, words, segments, atMs, elapsedSec, error},
 *  extra.transcriptText = the first 20 kB of text so library search finds spoken words).
 *
 * Segments live on MediaRecord.segments ([{id, label, startTime, endTime, source, text}]) and in named
 * collections (extra.segmentCollections [{id, name, segments}] with extra.activeCollection). Auto split
 * can follow transcript pauses (silence between whisper segments) or a fixed interval with overlap.
 *
 * Engine switch (DEP-13): transcribe.config.engine "python" (default, transcribe.py) | "native" (PlayPlaySpeech asr with the
 * same arguments whenever the exe is found) | "auto" (only once its `version` lists asr); toolsDir overrides where the exe is
 * (default next to the Supervisor). A native run that exits non-zero (64 = unsupported), crashes or cannot start is re-run
 * with transcribe.py; only a run killed by cancel() ends "cancelled".
 * The environment probe (POST /transcribe/probe) never waits on the event loop: it runs Python asynchronously and the route
 * replies when it finishes (registered through laneext, replacing the old blocking handler).
 */
class TranscriptionManager : public QObject {
    Q_OBJECT
public:
    explicit TranscriptionManager(sched::ScheduleStore* store, const QString& dataDir, const QString& scriptsDir, QObject* parent = nullptr);
    ~TranscriptionManager() override;

    QJsonObject config() const;                  // {python, defaultLevel, device, computeType, language, autoTranscribe, autoLevel, threads}
    void setConfig(const QJsonObject& cfg);
    QJsonArray levels() const;

    QJsonObject enqueue(const QString& mediaId, const QString& level, const QJsonObject& opts = QJsonObject());
    bool cancel(const QString& jobId);
    QJsonArray jobsJson() const;
    void clearFinished();
    QString transcriptPath(const QString& mediaId) const;
    QJsonObject transcript(const QString& mediaId, bool* found = nullptr) const;
    bool removeTranscript(const QString& mediaId);
    QJsonObject probeEnvironment();              // never blocks: the last probe's result ({pending: true} before the first), and a refresh starts
    void probeEnvironmentAsync(std::function<void(const QJsonObject&)> cb);   // python + faster-whisper availability (+ the native tool), cb once
    static TranscriptionManager* primary();      // the first one alive (the Supervisor's), for the async probe route

    // segments
    QJsonObject segments(const QString& mediaId) const;          // {segments, collections, activeCollection}
    QJsonObject setSegments(const QString& mediaId, const QJsonArray& segments, const QString& collectionId = QString());
    QJsonObject setCollections(const QString& mediaId, const QJsonArray& collections, const QString& activeId);
    QJsonObject autoSplit(const QString& mediaId, const QJsonObject& opts);   // {mode: transcript|interval, intervalSec, overlapSec, minPauseSec, minSegmentSec, maxSegmentSec, label}
    QJsonObject segmentFromWords(const QString& mediaId, int firstWord, int lastWord, const QString& label); // transcript selection → segment

signals:
    void jobsChanged();
    void transcriptReady(const QString& mediaId);

private:
    struct Job { QString id, mediaId, title, level, state = "queued", error, path, engine; double progress = 0, total = 0; qint64 queuedMs = 0, startedMs = 0, finishedMs = 0; QJsonObject opts; QJsonObject toJson() const; };
    sched::ScheduleStore* m_store;
    QString m_dataDir, m_scriptsDir;
    QJsonObject m_cfg;
    QList<Job*> m_jobs;
    Job* m_current = nullptr;
    QProcess* m_proc = nullptr;
    bool m_cancelRequested = false;              // cancel() killed m_proc: its crash exit means "cancelled", not a tool crash to fall back from
    QTimer m_notify;
    QByteArray m_errBuf;

    struct ToolCaps;                             // toolswitch::CapsCache (kept out of this header)
    std::unique_ptr<ToolCaps> m_toolCaps;
    QJsonObject m_env;                           // last environment probe ({} = none yet)
    QProcess* m_envProc = nullptr;
    QList<std::function<void(const QJsonObject&)>> m_envWaiters;

    void pump();
    void startJob(Job* j, bool native);          // native: PlayPlaySpeech asr; else python transcribe.py
    void finish(Job* j, const QString& state, const QString& error = QString());
    void loadConfig();
    QString pythonPath() const;
    QString toolPath() const;                    // PlayPlaySpeech in toolsDir, else next to the Supervisor ("" = none)
    QJsonObject engineResolved() const;          // {mode, uses: native|python, exe, reason}
    void notify() { if (!m_notify.isActive()) m_notify.start(400); }
    QJsonArray wordsFlat(const QJsonObject& t) const;
};

#endif // TRANSCRIPTIONMANAGER_H
