#pragma once
// Renderer (ST-18b): runs a compose plan (compose/filtergraph.h) and delivers the MP4, its poster PNG and the SRT /
// WebVTT caption sidecars (compose/captions.h), as StarMedia compose.ts renderComposition does, but:
//   - behind IRenderBackend: a backend runs one ffmpeg-argument job and reports it once, so the staged ffmpeg.exe
//     (FfmpegBackend, today; the "proven path" of SpotGenerator / RenderFarm) and PlayPlayMediaTool's graph runner
//     (inhouse-deps DEP-05, later: the same argument lists through libavfilter inside that tool) are interchangeable.
//     A backend may decline a job it cannot run (unsupported(): MediaTool accepts an ffmpeg-argument subset), which
//     fails the render with its reason instead of running something else;
//   - in LANES: the plan's steps form a small graph (segments, the mix and the parts are independent of each other;
//     a part needs only its own segments; the join or concat waits for what it reads) and at most `lanes` jobs run at
//     once, lowest step first (default min(4, cores / 8): the design's render lane width; memory is tight while the
//     live stack runs on the same box);
//   - SEGMENT-PARALLEL at film scale (RenderOptions::partSeconds, automatic above 90 s or 200 clips): the join is cut
//     into windows that render in parallel and are joined by stream copy (filtergraph.h), the split / concat idea of
//     RenderFarm::startSplit / concatSegments; the output has the single join's frame count exactly;
//   - with LOUDNESS TARGETS (AudioPlan::loudness: social -16, broadcast -24 (compose.ts / ATSC A/85), r128 -23 LUFS,
//     custom), in two passes by default (measure the mix, then a linear gain to the target), and the finished file
//     measured once more (RenderResult::loudness.output) so the caller sees the loudness it got, not the one it asked.
// Planning (buildPlan, which probes every input with media::probe), staging (font, stills, drawtext files, graphs),
// moving the output into place, the final probe, the sidecars and removing the work folder run on a worker pool;
// ffmpeg runs as child processes whose stderr and progress pipes are read asynchronously. Nothing blocks the thread
// the Renderer lives on, so it can live on the service's event loop. One render at a time per Renderer; finished() is
// emitted exactly once per accepted start(), also for refusals and cancels (queued, never from inside start()).
#include "compose/captions.h"
#include "compose/filtergraph.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <memory>

class QThreadPool;

namespace studio::compose {

// ---------------------------------------------------------------- backend

/// One ffmpeg-argument job.
struct RenderJob {
    QStringList args;                // after the common prefix (Step::args)
    QString cwd;                     // the work folder (steps name their files relative to it)
    bool progress = false;           // -progress pipe:1: out_time_us → RenderRun::progress
    QString logLevel = QStringLiteral("error");   // "info" for loudnorm's analysis (its JSON is an info message)
    int timeoutMs = 10 * 60 * 1000;  // compose.ts's per-step default
    QString label;                   // "scene 3", "join" ... (messages)
};

struct RenderJobResult {
    bool ok = false;
    bool cancelled = false;
    bool timedOut = false;
    int exitCode = -1;
    QString error;                   // "ffmpeg failed (exit 1): <stderr tail>", "cancelled", "ffmpeg timed out after 600 s" ...
    QString log;                     // the end of stderr (at most ~200 KB): loudnorm's JSON, the error lines
    qint64 elapsedMs = 0;
};

/// A running job. finished() is emitted exactly once and never before IRenderBackend::start() returned; the owner may
/// delete the run (deleteLater) once it fired.
class RenderRun : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    /// Stops the job; finished() follows with cancelled = true (unless it had already finished).
    virtual void cancel() = 0;
signals:
    void progress(double outputSeconds);
    void finished(const studio::compose::RenderJobResult& result);
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual QString name() const = 0;        // "ffmpeg" (staged ffmpeg.exe); "mediatool" later
    virtual QString program() const = 0;     // the executable ("" when there is none)
    virtual bool available(QString* why = nullptr) const = 0;
    /// "" when this backend can run `job`, else why not (an argument outside its subset ...).
    virtual QString unsupported(const RenderJob& job) const { Q_UNUSED(job); return {}; }
    /// Starts `job`; the run is a child of `parent` and reports on the thread that called start().
    virtual RenderRun* start(const RenderJob& job, QObject* parent) = 0;
};

/// The staged ffmpeg.exe (FFmpeg 9, next to the executables; PLAYPLAY_FFMPEG_TOOLS in CMakeLists.txt) through QProcess.
class FfmpegBackend final : public IRenderBackend {
public:
    /// `program` "" = findFfmpeg().
    explicit FfmpegBackend(QString program = QString());
    QString name() const override { return QStringLiteral("ffmpeg"); }
    QString program() const override { return m_program; }
    bool available(QString* why = nullptr) const override;
    RenderRun* start(const RenderJob& job, QObject* parent) override;
    /// compose.ts's common prefix: -hide_banner -nostdin -y -loglevel <level>, then -progress pipe:1 -nostats when
    /// asked (else -nostats alone above "error", so status lines stay out of the log).
    static QStringList prefix(const RenderJob& job);

private:
    QString m_program;
};

/// ffmpeg for the Studio: env PLAYPLAY_STUDIO_FFMPEG, PLAYPLAY_FFMPEG or STARMEDIA_FFMPEG (an existing file), then
/// ffmpeg(.exe) next to this executable, then PATH. "" when none.
QString findFfmpeg();
/// A bold sans font file for drawtext: env PLAYPLAY_STUDIO_FONT or STARMEDIA_MCP_FONT, then compose.ts's candidates
/// (segoeuib, arialbd, segoeui, arial on Windows; Arial Bold / DejaVu / Liberation elsewhere). "" when none (then
/// drawtext asks fontconfig for "Sans").
QString findFont();
/// compose.ts stderrTail: the last `lines` meaningful lines of an ffmpeg log joined by " | ", at most 900 characters.
QString stderrTail(QStringView log, int lines = 6);
/// The render lane width: min(4, max(1, logical cores / 8)).
int defaultLanes();

constexpr double kDefaultPartSeconds = 45;   // the film-scale window (design: ~30-60 s)

// ---------------------------------------------------------------- the renderer

struct RenderOptions {
    QString outFile;                 // the finished MP4 (absolute; its folder is created)
    QString posterFile;              // "" = <outFile's folder>/<outFile's base name>.png
    QString workDir;                 // scratch folder (created; left in place, as compose.ts); "" = a temp folder
                                     // removed afterwards unless keepWork
    bool keepWork = false;
    bool sidecars = true;            // write the SRT and VTT next to the video when the timeline has captions
    QString srtFile, vttFile;        // "" = <outFile base>.srt / .vtt
    int lanes = 0;                   // ffmpeg processes at once; 0 = defaultLanes()
    /// > 0: segment-parallel join in windows of about this many seconds; 0 = automatic (kDefaultPartSeconds when the
    /// timeline is longer than twice that or has more than 200 clips, else one join); < 0 = always one join.
    double partSeconds = 0;
    int loudnessPasses = 2;          // 2 = measure, then linear (accurate targets); 1 = compose.ts's single pass
    bool verifyLoudness = true;      // measure the finished file's loudness (one more audio-only pass)
    int stepTimeoutMs = 10 * 60 * 1000;   // per step, plus 10 s per second of the step's output
    QString font;                    // drawtext font file; "" = findFont()
    ProbeFn probe;                   // input check while planning; empty = media::probe (cancelled with the render)
    QThreadPool* pool = nullptr;     // planning / staging / file work; nullptr = QThreadPool::globalInstance()
};

struct LoudnessReport {
    bool on = false;                 // the programme has sound and loudness is not off
    LoudnessTarget target;
    int passes = 1;
    LoudnessMeasure mix;             // the mix before the second pass (two passes)
    LoudnessMeasure output;          // the finished file (verifyLoudness)
    QJsonObject toJson() const;      // {on, target {i, tp, lra}, passes, mix?, output?}
};

struct RenderResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QString file, poster, srt, vtt;  // what was written ("" for what was not)
    QString workDir;                 // the work folder when it was kept (the caller's, or keepWork); "" when removed
    double duration = 0;             // r3 of the finished file's duration (the plan's total when unreadable)
    int width = 0, height = 0;
    double fps = 0;
    QJsonArray timeline;             // compose.ts ComposeResult.timeline
    QStringList warnings;
    LoudnessReport loudness;
    int parts = 0;                   // segment-parallel windows (0 = one join)
    int steps = 0;                   // ffmpeg jobs run (or planned, on failure)
    int lanes = 0, peakParallel = 0; // lane width and the most ffmpeg processes that ran at once
    qint64 elapsedMs = 0;
    /// compose.ts ComposeResult {file, poster, duration, width, height, fps, timeline, warnings} plus ok, error,
    /// cancelled, srt, vtt, loudness, parts, steps, lanes, peakParallel, elapsedMs.
    QJsonObject toJson() const;
};

class Renderer : public QObject {
    Q_OBJECT
public:
    /// With the staged ffmpeg (FfmpegBackend()).
    explicit Renderer(QObject* parent = nullptr);
    Renderer(std::shared_ptr<IRenderBackend> backend, QObject* parent = nullptr);
    ~Renderer() override;                    // stops running jobs; the work folder is removed on the pool

    /// Starts rendering `t`. False (and no finished()) only when a render is already running; every other problem
    /// (no backend, an invalid timeline, a bad output path, a plan error) arrives through finished().
    bool start(const Timeline& t, const RenderOptions& o);
    /// Stops the render: running ffmpeg processes are killed, finished() follows with cancelled = true.
    void cancel();
    bool isRunning() const { return bool(m_job); }
    IRenderBackend* backend() const { return m_backend.get(); }

signals:
    /// 0..1 and what is happening ("rendering scenes (3 of 9 done)", "joining scenes and mixing audio" ...); never
    /// goes backwards within one render.
    void progress(double fraction, const QString& message);
    void finished(const studio::compose::RenderResult& result);

private:
    struct Job;
    struct Setup;
    /// On the pool: the work folder, the staged font and the plan (buildPlan probes every input).
    static Setup setupRender(const Timeline& t, const RenderOptions& o, const QString& posterFile,
                             const std::shared_ptr<std::atomic<bool>>& cancel);
    void onSetup(quint64 gen, const Setup& s);
    void buildNodes();
    void schedule();
    void launch(int node);
    void onPrepared(quint64 gen, int node, const QString& error);
    void startRun(int node);
    void onRunDone(quint64 gen, int node, const RenderJobResult& r);
    void fail(const QString& error);
    void finalize();
    void finishNow();
    void emitProgress();
    QThreadPool* pool() const;

    std::shared_ptr<IRenderBackend> m_backend;
    std::shared_ptr<Job> m_job;
    quint64 m_gen = 0;
};

} // namespace studio::compose

Q_DECLARE_METATYPE(studio::compose::RenderJobResult)
Q_DECLARE_METATYPE(studio::compose::RenderResult)
