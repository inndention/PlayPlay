#include "compose/renderer.h"

#include "media/probe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <climits>
#include <cmath>

namespace studio::compose {

namespace {

bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size();
}

/// One ffmpeg.exe child process. Everything is reported through signals on the owner's thread; finished() is queued so
/// a caller that cancels from inside its own loop is never re-entered.
class FfmpegRun final : public RenderRun {
public:
    FfmpegRun(const QString& program, const RenderJob& job, QObject* parent) : RenderRun(parent), m_program(program), m_job(job) {
        m_timer.setSingleShot(true);
        connect(&m_timer, &QTimer::timeout, this, [this] { m_timedOut = true; m_p.kill(); });
        connect(&m_p, &QProcess::readyReadStandardOutput, this, [this] { readProgress(); });
        connect(&m_p, &QProcess::readyReadStandardError, this, [this] {
            m_err += m_p.readAllStandardError();
            if (m_err.size() > 200000) m_err = m_err.right(100000);   // compose.ts keeps the last 100-200 KB
        });
        connect(&m_p, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) { onExit(code, st); });
        connect(&m_p, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart) return;                // a crash or a kill ends in finished() too
            RenderJobResult r;
            r.cancelled = m_cancelled;
            r.error = m_cancelled ? QStringLiteral("cancelled")
                                  : QStringLiteral("ffmpeg could not start (%1): %2").arg(m_program, m_p.errorString());
            done(r);
        });
    }
    ~FfmpegRun() override {
        // the member process outlives this body: cut its signals first, then make sure no child is left behind
        QObject::disconnect(&m_p, nullptr, this, nullptr);
        m_done = true;
        if (m_p.state() != QProcess::NotRunning) {
            m_p.kill();
            m_p.waitForFinished(3000);
        }
    }

    void begin() {
        if (m_done) return;
        if (m_cancelled) {
            RenderJobResult r;
            r.cancelled = true;
            r.error = QStringLiteral("cancelled");
            done(r);
            return;
        }
        m_clock.start();
        m_p.setProgram(m_program);
        m_p.setArguments(FfmpegBackend::prefix(m_job) + m_job.args);
        m_p.setWorkingDirectory(m_job.cwd);
        m_p.setProcessChannelMode(QProcess::SeparateChannels);
        m_p.setStandardInputFile(QProcess::nullDevice());
        if (m_job.timeoutMs > 0) m_timer.start(m_job.timeoutMs);
        m_p.start();
    }

    void cancel() override {
        if (m_done || m_cancelled) return;
        m_cancelled = true;
        if (m_p.state() != QProcess::NotRunning) m_p.kill();       // finished() follows; not started yet: begin() reports it
    }

private:
    void readProgress() {
        m_out += m_p.readAllStandardOutput();
        qsizetype nl;
        while ((nl = m_out.indexOf('\n')) >= 0) {
            const QByteArray line = m_out.left(nl).trimmed();
            m_out.remove(0, nl + 1);
            if (!line.startsWith("out_time_us=")) continue;
            bool ok = false;
            const qint64 us = line.mid(12).toLongLong(&ok);       // "N/A" before the first frame
            if (ok && us >= 0) emit progress(double(us) / 1e6);
        }
        if (m_out.size() > 65536) m_out.clear();
    }
    void onExit(int code, QProcess::ExitStatus st) {
        RenderJobResult r;
        r.exitCode = code;
        m_err += m_p.readAllStandardError();
        r.log = QString::fromUtf8(m_err);
        if (m_cancelled) {
            r.cancelled = true;
            r.error = QStringLiteral("cancelled");
        } else if (m_timedOut) {
            r.timedOut = true;
            r.error = QStringLiteral("ffmpeg timed out after %1 s").arg(qRound(m_job.timeoutMs / 1000.0));
        } else if (st != QProcess::NormalExit || code != 0) {
            const QString tail = stderrTail(r.log);
            r.error = QStringLiteral("ffmpeg failed (exit %1): %2").arg(code).arg(tail.isEmpty() ? QStringLiteral("no error output") : tail);
        } else {
            r.ok = true;
        }
        done(r);
    }
    void done(RenderJobResult r) {
        if (m_done) return;
        m_done = true;
        m_timer.stop();
        r.elapsedMs = m_clock.isValid() ? m_clock.elapsed() : 0;
        QMetaObject::invokeMethod(this, [this, r] { emit finished(r); }, Qt::QueuedConnection);
    }

    QString m_program;
    RenderJob m_job;
    QProcess m_p;
    QTimer m_timer;
    QElapsedTimer m_clock;
    QByteArray m_err, m_out;
    bool m_done = false, m_cancelled = false, m_timedOut = false;
};

struct FinalInfo {
    media::MediaInfo info;
    QString srt, vtt, error;
};

} // namespace

// ---------------------------------------------------------------- backend

FfmpegBackend::FfmpegBackend(QString program) : m_program(program.isEmpty() ? findFfmpeg() : std::move(program)) {}

bool FfmpegBackend::available(QString* why) const {
    if (m_program.isEmpty()) {
        if (why) *why = QStringLiteral("ffmpeg was not found (put ffmpeg.exe next to the Studio or set PLAYPLAY_FFMPEG)");
        return false;
    }
    if (!QFileInfo(m_program).isFile()) {
        if (why) *why = QStringLiteral("ffmpeg was not found at %1").arg(m_program);
        return false;
    }
    return true;
}

RenderRun* FfmpegBackend::start(const RenderJob& job, QObject* parent) {
    auto* run = new FfmpegRun(m_program, job, parent);
    // start on the next turn of the loop: a process that cannot start reports at once, and the caller connects first
    QTimer::singleShot(0, run, [run] { run->begin(); });
    return run;
}

QStringList FfmpegBackend::prefix(const RenderJob& job) {
    const QString level = job.logLevel.isEmpty() ? QStringLiteral("error") : job.logLevel;
    QStringList p{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), level};
    if (job.progress) p << QStringLiteral("-progress") << QStringLiteral("pipe:1") << QStringLiteral("-nostats");
    else if (level != u"error" && level != u"quiet" && level != u"fatal" && level != u"panic") p << QStringLiteral("-nostats");
    return p;
}

QString findFfmpeg() {
    for (const char* env : {"PLAYPLAY_STUDIO_FFMPEG", "PLAYPLAY_FFMPEG", "STARMEDIA_FFMPEG"}) {
        const QString v = qEnvironmentVariable(env);
        if (!v.isEmpty() && QFileInfo(v).isFile()) return QDir::cleanPath(v);
    }
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral("ffmpeg.exe");
#else
    const QString exe = QStringLiteral("ffmpeg");
#endif
    if (QCoreApplication::instance()) {
        const QString local = QCoreApplication::applicationDirPath() + QLatin1Char('/') + exe;
        if (QFileInfo(local).isFile()) return local;
    }
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

QString findFont() {
    for (const char* env : {"PLAYPLAY_STUDIO_FONT", "STARMEDIA_MCP_FONT"}) {
        const QString v = qEnvironmentVariable(env);
        if (!v.isEmpty() && QFileInfo(v).isFile()) return v;
    }
    static const char* const kCandidates[] = {   // compose.ts / ffmpeg.ts FONT_CANDIDATES
        "C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf"};
    for (const char* f : kCandidates)
        if (QFileInfo(QLatin1String(f)).isFile()) return QLatin1String(f);
    return {};
}

QString stderrTail(QStringView log, int lines) {
    static const QRegularExpression noise(QStringLiteral("^(frame=|size=|video:|\\[out#|Press \\[q\\])"));
    static const QRegularExpression eol(QStringLiteral("\\r?\\n"));
    QStringList keep;
    for (const QString& l : log.toString().split(eol)) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !noise.match(t).hasMatch()) keep << t;
    }
    return keep.mid(std::max<qsizetype>(0, keep.size() - lines)).join(QStringLiteral(" | ")).left(900);
}

int defaultLanes() { return std::min(4, std::max(1, QThread::idealThreadCount() / 8)); }

// ---------------------------------------------------------------- results

QJsonObject LoudnessReport::toJson() const {
    QJsonObject o{{"on", on}, {"target", QJsonObject{{"i", target.i}, {"tp", target.tp}, {"lra", target.lra}}}, {"passes", passes}};
    if (mix.ok) o.insert(QStringLiteral("mix"), mix.toJson());
    if (output.ok) o.insert(QStringLiteral("output"), output.toJson());
    return o;
}

QJsonObject RenderResult::toJson() const {
    QJsonObject o{{"ok", ok}, {"file", file}, {"poster", poster}, {"duration", duration}, {"width", width}, {"height", height},
                  {"fps", fps}, {"timeline", timeline}, {"warnings", QJsonArray::fromStringList(warnings)},
                  {"loudness", loudness.toJson()}, {"parts", parts}, {"steps", steps}, {"lanes", lanes},
                  {"peakParallel", peakParallel}, {"elapsedMs", elapsedMs}};
    if (!error.isEmpty()) o.insert(QStringLiteral("error"), error);
    if (cancelled) o.insert(QStringLiteral("cancelled"), true);
    if (!srt.isEmpty()) o.insert(QStringLiteral("srt"), srt);
    if (!vtt.isEmpty()) o.insert(QStringLiteral("vtt"), vtt);
    if (!workDir.isEmpty()) o.insert(QStringLiteral("workDir"), workDir);
    return o;
}

// ---------------------------------------------------------------- the renderer

struct Renderer::Setup {
    Plan plan;
    QString work;
    bool ownsWork = false;           // a folder this render made (removed at the end unless keepWork)
    QString error;
};

struct Renderer::Job {
    struct Node {
        enum class St { Waiting, Preparing, Running, Done };
        Step step;
        QString label;                   // "scene 3", "part 2 of 4", "join" ... (messages and errors)
        QVector<int> dependents;
        int waiting = 0;                 // unfinished dependencies
        St st = St::Waiting;
        double weight = 1, frac = 0;     // share of the whole render; 0..1 done
        QPointer<RenderRun> run;
        bool moveOutput = false;         // the poster's: move the join output to outFile first
        bool verify = false;             // the loudness check of the finished file
    };
    quint64 gen = 0;
    Timeline timeline;
    RenderOptions opt;
    QString outPath, posterFile;
    Plan plan;
    QString work;
    bool ownsWork = false;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    QVector<Node> nodes;
    int active = 0, running = 0, lanes = 1, peak = 0;
    bool failing = false, cancelled = false, finalizing = false, outWritten = false;
    QString error;
    QString secondPass;                  // replaces kLoudnormToken once the mix is measured
    LoudnessMeasure mixMeasure, outMeasure;
    QStringList warnings;
    QElapsedTimer clock;
    double lastFrac = 0;
    QString lastMsg;
    FinalInfo final;
};

Renderer::Renderer(QObject* parent) : Renderer(std::make_shared<FfmpegBackend>(), parent) {}

Renderer::Renderer(std::shared_ptr<IRenderBackend> backend, QObject* parent) : QObject(parent), m_backend(std::move(backend)) {
    qRegisterMetaType<RenderJobResult>();
    qRegisterMetaType<RenderResult>();
}

Renderer::~Renderer() {
    if (!m_job) return;
    m_job->cancel->store(true);
    for (Job::Node& n : m_job->nodes)
        if (n.run) delete n.run.data();                            // kills and reaps the process before the folder goes
    if (m_job->ownsWork && !m_job->opt.keepWork && !m_job->work.isEmpty()) {
        const QString w = m_job->work;
        pool()->start([w] { QDir(w).removeRecursively(); });
    }
}

QThreadPool* Renderer::pool() const {
    return m_job && m_job->opt.pool ? m_job->opt.pool : QThreadPool::globalInstance();
}

Renderer::Setup Renderer::setupRender(const Timeline& t, const RenderOptions& o, const QString& posterFile,
                                      const std::shared_ptr<std::atomic<bool>>& cancel) {
    Setup s;
    if (o.workDir.isEmpty()) {
        QTemporaryDir tmp(QDir::temp().filePath(QStringLiteral("studio-render-XXXXXX")));
        if (!tmp.isValid()) { s.error = QStringLiteral("could not create a work folder: ") + tmp.errorString(); return s; }
        tmp.setAutoRemove(false);                                   // the render removes it on the pool when done
        s.work = tmp.path();
        s.ownsWork = true;
    } else {
        s.work = QDir::cleanPath(QDir(o.workDir).absolutePath());
        if (!QDir().mkpath(s.work)) { s.error = QStringLiteral("could not create the work folder %1").arg(s.work); return s; }
    }
    for (const QString& f : {o.outFile, posterFile})
        if (!QDir().mkpath(QFileInfo(f).absolutePath())) { s.error = QStringLiteral("could not create the folder of %1").arg(f); return s; }
    // the font goes into the work folder so drawtext names it without path escaping (compose.ts stageFont)
    QString fontOpt = QStringLiteral("font=Sans");
    const QString font = o.font.isEmpty() ? findFont() : o.font;
    if (!font.isEmpty() && QFileInfo(font).isFile()) {
        const QString suffix = QFileInfo(font).suffix().toLower();
        const QString dest = s.work + QStringLiteral("/font.") + (suffix.isEmpty() ? QStringLiteral("ttf") : suffix);
        QFile::remove(dest);
        if (QFile::copy(font, dest)) fontOpt = QStringLiteral("fontfile=") + QFileInfo(dest).fileName();
    }
    PlanOptions po;
    po.workDir = s.work;
    po.outFile = QDir::cleanPath(o.outFile);
    po.posterFile = QDir::cleanPath(posterFile);
    po.font = fontOpt;
    po.fileOnly = true;
    po.loudnessPasses = o.loudnessPasses;
    po.partSeconds = o.partSeconds > 0 ? o.partSeconds
                     : o.partSeconds < 0 ? 0
                     : (t.totalSeconds() > 2 * kDefaultPartSeconds || t.clips.size() > 200) ? kDefaultPartSeconds : 0;
    po.probe = o.probe ? o.probe : ProbeFn([cancel](const QString& p) {
        media::ProbeOptions mo;
        mo.cancel = cancel.get();
        const media::MediaInfo m = media::probe(p, mo);
        return SourceInfo{m.ok && m.hasVideo(), m.ok && m.hasAudio()};
    });
    if (cancel->load()) { s.error = QStringLiteral("cancelled"); return s; }
    s.plan = buildPlan(t, po);
    if (cancel->load()) s.error = QStringLiteral("cancelled");
    else if (!s.plan.ok) s.error = s.plan.error;
    return s;
}

bool Renderer::start(const Timeline& t, const RenderOptions& o) {
    if (m_job) return false;
    auto job = std::make_shared<Job>();
    job->gen = ++m_gen;
    job->timeline = t;
    job->opt = o;
    job->clock.start();
    job->lanes = o.lanes > 0 ? o.lanes : defaultLanes();
    job->outPath = QDir::cleanPath(o.outFile);
    const QFileInfo of(o.outFile);
    job->posterFile = !o.posterFile.isEmpty() ? o.posterFile
                      : o.outFile.isEmpty()   ? QString()
                                              : of.absolutePath() + QLatin1Char('/') + of.completeBaseName() + QStringLiteral(".png");
    job->plan.loudness = t.audio.loudnessTarget();                // reported even when the render is refused
    m_job = job;
    const quint64 gen = job->gen;

    QString why;
    if (!m_backend) why = QStringLiteral("no render backend");
    else if (!m_backend->available(&why)) { if (why.isEmpty()) why = m_backend->name() + QStringLiteral(" is not available"); }
    else if (o.outFile.isEmpty() || !QDir::isAbsolutePath(o.outFile)) why = QStringLiteral("the output file needs an absolute path");
    else if (!QDir::isAbsolutePath(job->posterFile)) why = QStringLiteral("the poster file needs an absolute path");
    else if (QFileInfo(job->posterFile).absoluteFilePath().compare(of.absoluteFilePath(), Qt::CaseInsensitive) == 0)
        why = QStringLiteral("the poster and the video need different files");
    else if (const QStringList errs = t.validate(); !errs.isEmpty()) why = errs.join(QStringLiteral("; "));
    if (!why.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, gen, why] {
            if (!m_job || m_job->gen != gen) return;
            m_job->error = why;
            finishNow();
        }, Qt::QueuedConnection);
        return true;
    }
    QMetaObject::invokeMethod(this, [this, gen] { if (m_job && m_job->gen == gen) emit progress(0, QStringLiteral("planning")); },
                              Qt::QueuedConnection);
    const QString poster = job->posterFile;
    const auto cancel = job->cancel;
    QtConcurrent::run(pool(), [t, o, poster, cancel] { return setupRender(t, o, poster, cancel); })
        .then(this, [this, gen](const Setup& s) { onSetup(gen, s); });
    return true;
}

void Renderer::onSetup(quint64 gen, const Setup& s) {
    if (!m_job || m_job->gen != gen) {                           // cancelled while planning: only the folder is left
        if (s.ownsWork && !s.work.isEmpty()) {
            const QString w = s.work;
            QThreadPool::globalInstance()->start([w] { QDir(w).removeRecursively(); });
        }
        return;
    }
    Job& j = *m_job;
    j.work = s.work;
    j.ownsWork = s.ownsWork;
    j.plan = s.plan;
    if (!s.plan.ok) j.plan.loudness = j.timeline.audio.loudnessTarget();   // the result still names the target
    if (!s.error.isEmpty()) {
        j.error = s.error;
        j.cancelled = j.cancelled || s.error == u"cancelled";
        finishNow();
        return;
    }
    buildNodes();
    emitProgress();
    schedule();
}

void Renderer::buildNodes() {
    Job& j = *m_job;
    const Plan& p = j.plan;
    const double total = std::max(0.1, p.total);
    QVector<QVector<int>> deps;
    const auto add = [&](const Step& s, const QString& label, double weight, const QVector<int>& d) {
        Job::Node n;
        n.step = s;
        n.label = label;
        n.weight = std::max(0.01, weight);
        j.nodes.push_back(n);
        deps.push_back(d);
        return int(j.nodes.size() - 1);
    };
    // weights: rough relative cost (a segment encodes its seconds with zoompan; a part or the join re-encodes; the
    // concat copies the picture; audio-only passes are cheap)
    const int mix = p.hasMix() ? add(p.mix, QStringLiteral("mix"), total * 0.1, {}) : -1;
    const int measure = p.twoPass() ? add(p.measure, QStringLiteral("loudness"), total * 0.03, {mix}) : -1;
    QVector<int> segs;
    for (const Step& s : p.segments)
        segs << add(s, s.kind == Step::Kind::EndCard ? QStringLiteral("end card") : QStringLiteral("scene %1").arg(s.slot + 1), std::max(0.1, s.seconds), {});
    QVector<int> parts;
    for (const Step& s : p.parts) {
        QVector<int> d;
        for (int i = s.first; i <= s.last; ++i) d << segs.value(i, segs.isEmpty() ? 0 : segs.last());
        parts << add(s, QStringLiteral("part %1 of %2").arg(s.slot + 1).arg(p.parts.size()), s.seconds * 0.5, d);
    }
    QVector<int> jd = p.split() ? parts : segs;
    if (mix >= 0) jd << mix;
    if (measure >= 0) jd << measure;
    const int join = add(p.join, p.split() ? QStringLiteral("concat") : QStringLiteral("join"), p.split() ? total * 0.05 : total * 0.5, jd);
    const int poster = add(p.poster, QStringLiteral("poster"), 0.2, {join});
    j.nodes[poster].moveOutput = true;
    if (j.opt.verifyLoudness && p.loudnessOn) {
        Step v;
        v.kind = Step::Kind::Measure;
        v.logLevel = QStringLiteral("info");
        v.seconds = p.total;
        v.args = QStringList{QStringLiteral("-vn"), QStringLiteral("-protocol_whitelist"), QStringLiteral("file"), QStringLiteral("-i"), j.outPath,
                             QStringLiteral("-af"), loudnormMeasureFilter(p.loudness), QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")};
        const int check = add(v, QStringLiteral("loudness check"), total * 0.03, {poster});
        j.nodes[check].verify = true;
    }
    for (int i = 0; i < j.nodes.size(); ++i) {
        j.nodes[i].waiting = int(deps[i].size());
        for (int d : std::as_const(deps[i])) j.nodes[d].dependents << i;
    }
}

void Renderer::schedule() {
    if (!m_job) return;
    Job& j = *m_job;
    if (j.failing) {
        if (j.active == 0) finishNow();
        return;
    }
    bool allDone = true;
    for (int i = 0; i < j.nodes.size(); ++i) {
        if (j.nodes[i].st != Job::Node::St::Done) allDone = false;
        if (j.active < j.lanes && j.nodes[i].st == Job::Node::St::Waiting && j.nodes[i].waiting == 0) launch(i);
    }
    if (allDone && !j.finalizing) finalize();
}

void Renderer::launch(int i) {
    Job& j = *m_job;
    Job::Node& n = j.nodes[i];
    n.st = Job::Node::St::Preparing;
    ++j.active;
    if (n.moveOutput) j.outWritten = true;
    const Step st = n.step;
    const QString work = j.work, pass = j.secondPass.isEmpty() ? loudnormFilter(j.plan.loudness) : j.secondPass;
    const bool move = n.moveOutput;
    const QString from = j.plan.join.output, to = j.outPath;
    const auto cancel = j.cancel;
    const quint64 gen = j.gen;
    // staging on the pool: the stills (a plain name each, so image2 never reads "%d" as a pattern), the drawtext text
    // files, the graph (with the measured second loudness pass), and for the poster the finished video into place
    QtConcurrent::run(pool(), [st, work, pass, move, from, to, cancel]() -> QString {
        if (cancel->load()) return QStringLiteral("cancelled");
        for (const StagedFile& f : st.staged) {
            const QString dest = work + QLatin1Char('/') + f.name;
            QFile::remove(dest);
            if (!QFile::copy(f.from, dest)) return QStringLiteral("could not stage %1").arg(QFileInfo(f.from).fileName());
        }
        for (const TextFile& t : st.texts)
            if (!writeFile(work + QLatin1Char('/') + t.name, t.text.toUtf8())) return QStringLiteral("could not write %1").arg(t.name);
        if (!st.graphFile.isEmpty()) {
            QString g = st.graph;
            if (st.loudnessToken) g.replace(QString::fromUtf16(kLoudnormToken), pass);
            if (!writeFile(work + QLatin1Char('/') + st.graphFile, g.toUtf8())) return QStringLiteral("could not write %1").arg(st.graphFile);
        }
        if (move) {
            QFile::remove(to);
            if (!QFile::rename(from, to)) return QStringLiteral("could not move the video to %1").arg(to);   // rename copies across volumes
        }
        return QString();
    }).then(this, [this, gen, i](const QString& err) { onPrepared(gen, i, err); });
}

void Renderer::onPrepared(quint64 gen, int i, const QString& err) {
    if (!m_job || m_job->gen != gen) return;
    Job& j = *m_job;
    if (j.failing) {
        --j.active;
        j.nodes[i].st = Job::Node::St::Done;
        schedule();
        return;
    }
    if (!err.isEmpty()) {
        --j.active;
        j.nodes[i].st = Job::Node::St::Done;
        fail(j.nodes[i].label + QStringLiteral(": ") + err);
        return;
    }
    startRun(i);
}

void Renderer::startRun(int i) {
    Job& j = *m_job;
    Job::Node& n = j.nodes[i];
    RenderJob rj;
    rj.args = n.step.args;
    rj.cwd = j.work;
    rj.progress = n.step.kind != Step::Kind::Poster && n.step.kind != Step::Kind::Measure;
    rj.logLevel = n.step.logLevel.isEmpty() ? QStringLiteral("error") : n.step.logLevel;
    rj.timeoutMs = int(std::min<double>(INT_MAX / 2, j.opt.stepTimeoutMs + std::max(0.0, n.step.seconds) * 10000));
    rj.label = n.label;
    if (const QString no = m_backend->unsupported(rj); !no.isEmpty()) {
        --j.active;
        n.st = Job::Node::St::Done;
        fail(QStringLiteral("%1: %2 cannot run this step: %3").arg(n.label, m_backend->name(), no));
        return;
    }
    RenderRun* run = m_backend->start(rj, this);
    n.run = run;
    n.st = Job::Node::St::Running;
    ++j.running;
    j.peak = std::max(j.peak, j.running);
    const quint64 gen = j.gen;
    connect(run, &RenderRun::progress, this, [this, gen, i](double sec) {
        if (!m_job || m_job->gen != gen) return;
        Job::Node& nd = m_job->nodes[i];
        if (nd.step.seconds > 0) nd.frac = std::clamp(sec / nd.step.seconds, nd.frac, 0.99);
        emitProgress();
    });
    connect(run, &RenderRun::finished, this, [this, gen, i, run](const RenderJobResult& r) {
        run->deleteLater();
        onRunDone(gen, i, r);
    });
}

void Renderer::onRunDone(quint64 gen, int i, const RenderJobResult& r) {
    if (!m_job || m_job->gen != gen) return;
    Job& j = *m_job;
    Job::Node& n = j.nodes[i];
    --j.active;
    --j.running;
    n.run = nullptr;
    n.st = Job::Node::St::Done;
    if (j.failing) { schedule(); return; }
    if (!r.ok) { fail(n.label + QStringLiteral(": ") + r.error); return; }
    n.frac = 1;
    if (n.step.kind == Step::Kind::Measure) {
        const LoudnessMeasure m = parseLoudnormLog(r.log);
        if (n.verify) {
            j.outMeasure = m;
            if (!m.ok) j.warnings << QStringLiteral("loudness check: ") + m.error;
        } else {
            j.mixMeasure = m;
            if (!m.ok) j.warnings << QStringLiteral("loudness: %1; normalised in one pass instead").arg(m.error);
            j.secondPass = loudnormSecondPass(j.plan.loudness, m);
        }
    }
    for (int d : std::as_const(n.dependents)) --j.nodes[d].waiting;
    emitProgress();
    schedule();
}

void Renderer::fail(const QString& error) {
    Job& j = *m_job;
    if (j.failing) return;
    j.failing = true;
    j.error = error;
    j.cancel->store(true);
    for (Job::Node& n : j.nodes)
        if (n.run) n.run->cancel();
    if (j.active == 0) finishNow();
}

void Renderer::cancel() {
    if (!m_job || m_job->failing || m_job->finalizing) return;
    m_job->cancelled = true;
    fail(QStringLiteral("cancelled"));
}

void Renderer::finalize() {
    Job& j = *m_job;
    j.finalizing = true;
    const QString out = j.outPath;
    const QVector<CaptionCue> cues = j.opt.sidecars ? sidecarCues(j.timeline) : QVector<CaptionCue>();
    const QFileInfo of(out);
    const QString base = of.absolutePath() + QLatin1Char('/') + of.completeBaseName();
    const QString srt = cues.isEmpty() ? QString() : j.opt.srtFile.isEmpty() ? base + QStringLiteral(".srt") : j.opt.srtFile;
    const QString vtt = cues.isEmpty() ? QString() : j.opt.vttFile.isEmpty() ? base + QStringLiteral(".vtt") : j.opt.vttFile;
    const quint64 gen = j.gen;
    QtConcurrent::run(pool(), [out, cues, srt, vtt] {
        FinalInfo f;
        f.info = media::probe(out);
        if (!cues.isEmpty()) {
            QString e;
            if (writeSidecars(cues, srt, vtt, &e)) { f.srt = srt; f.vtt = vtt; }
            else f.error = e;
        }
        return f;
    }).then(this, [this, gen](const FinalInfo& f) {
        if (!m_job || m_job->gen != gen) return;
        m_job->final = f;
        if (!f.error.isEmpty()) m_job->warnings << QStringLiteral("captions: ") + f.error;
        finishNow();
    });
}

void Renderer::finishNow() {
    const std::shared_ptr<Job> job = std::move(m_job);
    m_job.reset();
    const Job& j = *job;
    RenderResult r;
    r.cancelled = j.cancelled;
    r.ok = j.error.isEmpty() && !j.cancelled;
    r.error = j.cancelled ? QStringLiteral("cancelled") : j.error;
    r.width = j.plan.width;
    r.height = j.plan.height;
    r.fps = j.plan.fps;
    r.timeline = j.plan.timelineJson();
    r.warnings = j.plan.warnings;
    for (const QString& w : j.warnings)
        if (!r.warnings.contains(w)) r.warnings << w;
    r.loudness.on = j.plan.loudnessOn;
    r.loudness.target = j.plan.loudness;
    r.loudness.passes = j.plan.loudnessPasses;
    r.loudness.mix = j.mixMeasure;
    r.loudness.output = j.outMeasure;
    r.parts = int(j.plan.parts.size());
    r.steps = int(j.nodes.size());
    r.lanes = j.lanes;
    r.peakParallel = j.peak;
    r.elapsedMs = j.clock.elapsed();
    QThreadPool* const p = j.opt.pool ? j.opt.pool : QThreadPool::globalInstance();
    if (r.ok) {
        r.file = j.opt.outFile;
        r.poster = j.posterFile;
        r.srt = j.final.srt;
        r.vtt = j.final.vtt;
        const media::MediaInfo& m = j.final.info;
        r.duration = m.ok && m.durationSec > 0 ? r3(m.durationSec) : j.plan.total;
        if (m.ok && m.hasVideo()) { r.width = m.video.width; r.height = m.video.height; }
    } else if (j.outWritten) {                                    // never leave a half-made deliverable behind
        const QString out = j.outPath, poster = j.posterFile;
        p->start([out, poster] { QFile::remove(out); QFile::remove(poster); });
    }
    if (j.ownsWork && !j.opt.keepWork && !j.work.isEmpty()) {
        const QString w = j.work;
        p->start([w] { QDir(w).removeRecursively(); });
    } else {
        r.workDir = j.work;
    }
    if (r.ok) emit progress(1.0, QStringLiteral("done"));
    emit finished(r);
}

void Renderer::emitProgress() {
    if (!m_job) return;
    Job& j = *m_job;
    double W = 0, D = 0;
    int segDone = 0, segN = 0, partDone = 0, partN = 0;
    int rank = 99;
    Step::Kind rankKind = Step::Kind::Segment;
    for (const Job::Node& n : std::as_const(j.nodes)) {
        const bool done = n.st == Job::Node::St::Done;
        W += n.weight;
        D += n.weight * (done ? 1.0 : n.frac);
        const Step::Kind k = n.step.kind;
        const bool seg = k == Step::Kind::Segment || k == Step::Kind::EndCard;
        if (seg) { ++segN; segDone += done; }
        if (k == Step::Kind::Part) { ++partN; partDone += done; }
        if (n.st != Job::Node::St::Running && n.st != Job::Node::St::Preparing) continue;
        // the message names the most telling stage at work: scenes, parts, the join, the sound, the poster
        const int r = seg ? 0 : k == Step::Kind::Part ? 1 : (k == Step::Kind::Join || k == Step::Kind::Concat) ? 2 : k == Step::Kind::Mix ? 3
                    : (k == Step::Kind::Measure && !n.verify) ? 4 : k == Step::Kind::Poster ? 5 : 6;
        if (r < rank) { rank = r; rankKind = k; }
    }
    QString msg = j.lastMsg;
    switch (rank) {
    case 0: msg = QStringLiteral("rendering scenes (%1 of %2 done)").arg(segDone).arg(segN); break;
    case 1: msg = QStringLiteral("rendering parts (%1 of %2 done)").arg(partDone).arg(partN); break;
    case 2: msg = rankKind == Step::Kind::Concat ? QStringLiteral("joining the parts") : QStringLiteral("joining scenes and mixing audio"); break;
    case 3: msg = QStringLiteral("mixing audio"); break;
    case 4: msg = QStringLiteral("measuring loudness"); break;
    case 5: msg = QStringLiteral("poster frame"); break;
    case 6: msg = QStringLiteral("checking loudness"); break;
    default: break;
    }
    const double f = std::max(j.lastFrac, 0.02 + 0.95 * (W > 0 ? D / W : 0));
    if (f - j.lastFrac >= 0.005 || msg != j.lastMsg) {
        j.lastFrac = f;
        j.lastMsg = msg;
        emit progress(f, msg);
    }
}

} // namespace studio::compose
