// ST-18a composer part 1 (docs/plans/starmedia-studio.md §4.10, §8): the Timeline / Clip / AudioPlan model and the
// deterministic filtergraph builder, a port of StarMedia mcp/spot/compose.ts.
//
// Golden parity: tools/studio/src/compose/golden/compose-golden.json was recorded from the REAL compose.ts (run under a
// stub ffmpeg that records instead of running; golden/gen-compose-golden.mjs) for the compose_video cases of
// starmedia test/mcp-spot.ts and test/mcp-fix-spot.ts, every transition (cut|fade|dissolve|dip|slide|wipe|zoom), every
// Ken Burns mode, ducking / no ducking / music only / several voices, the spot pipeline's shape, headline and caption
// edge cases, padding and trimming to the exact length, fallbacks and an end card alone. Every case must give the same
// ffmpeg argument lists, the same join graph, the same drawtext files, the same staged files and the same result
// timeline and warnings, byte for byte. The pure helpers are compared as tables.
//
// Hand-written checks: the xfade names, Ken Burns expressions, the ducking chain and the exact-length tail as readable
// strings; the ads/types.ts extras (focus, keyframed moves, trimmed / sped-up clips, the clip's own sound, audio
// "from", master fades, per-clip transition length) whose text has no TS precedent; JSON round trips and validation.
// Then a plan is RUN with the staged ffmpeg.exe in a temp dir (stills, clips, voice, music, captions, end card with
// logo, every transition, moves, fades, natural sound) to show the graphs are accepted and the length is exact.
//
// ST-18b composer part 2: loudness targets (presets, custom, the two-pass plan and the analysis parser), the
// segment-parallel plan (windows, part graphs, captions per window, concat), SRT / VTT byte for byte against
// golden/captions-golden.json (recorded from the REAL src/media.ts), the sidecar cues, and the Renderer: its step
// graph and lanes against a fake backend (dependencies, lane width, the measured second pass reaching the join,
// failure, cancel, refusals), FfmpegBackend on its own (progress, failure text, timeout, cancel, a missing program),
// a 6 s test card rendered end to end (length ±0.05 s, R128 within 1 LU, sidecars, poster), and a segment-parallel
// render against a single pass (same frame count and length, same pictures, captions drawn in the parts).
// The renders are heavy tests: run this binary under the lane build lock.
// Hermetic: one QTemporaryDir; the only child processes are our own ffmpeg.exe.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <QEventLoop>
#include <QPointer>
#include <QTimer>

#include <cmath>
#include <limits>

#include "compose/captions.h"
#include "compose/filtergraph.h"
#include "compose/renderer.h"
#include "compose/timeline.h"
#include "media/frames.h"
#include "media/probe.h"

using namespace studio::compose;

namespace {

QJsonObject golden() {
    static const QJsonObject g = [] {
        QFile f(QStringLiteral(STUDIO_SOURCE_DIR "/src/compose/golden/compose-golden.json"));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return g;
}

QStringList strings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray()) out << x.toString();
    return out;
}

QString fileBase(const QString& p) { return p.mid(std::max(p.lastIndexOf(QLatin1Char('/')), p.lastIndexOf(QLatin1Char('\\'))) + 1); }

/// The options the golden fixture was recorded with (the TS stub probe: names containing "junk" are unreadable).
PlanOptions goldenOptions() {
    PlanOptions o;
    o.workDir = QStringLiteral("{WORK}");
    o.outFile = QStringLiteral("{OUT}/v.mp4");
    o.posterFile = QStringLiteral("{OUT}/v.png");
    o.font = QStringLiteral("fontfile=font.ttf");
    o.probe = [](const QString& p) { return SourceInfo{!fileBase(p).contains(QLatin1String("junk"), Qt::CaseInsensitive), false}; };
    o.fileOnly = false;                                            // compose.ts has no -protocol_whitelist
    o.limitedRange = false;                                        // ... and ends segments with a plain format=yuv420p
    return o;
}

/// The files a step opens (the value after each -i), checking that each one sits behind "-protocol_whitelist file".
QStringList inputsOf(const QStringList& args, bool fileOnly = true) {
    QStringList out;
    for (qsizetype i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != QLatin1String("-i")) continue;
        const bool guarded = i >= 2 && args[i - 2] == QLatin1String("-protocol_whitelist") && args[i - 1] == QLatin1String("file");
        out << (guarded == fileOnly ? args[i + 1] : QStringLiteral("<unguarded> ") + args[i + 1]);
    }
    return out;
}

/// First difference between two argument lists, for a readable failure.
QString listDiff(const QStringList& got, const QStringList& want) {
    for (qsizetype i = 0; i < std::max(got.size(), want.size()); ++i) {
        const QString g = got.value(i, QStringLiteral("<none>")), w = want.value(i, QStringLiteral("<none>"));
        if (g != w) return QStringLiteral("arg %1:\n  got  %2\n  want %3").arg(i).arg(g, w);
    }
    return {};
}
QString textDiff(const QString& got, const QString& want) {
    qsizetype i = 0;
    while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
    return QStringLiteral("first difference at %1:\n  got  ...%2\n  want ...%3").arg(i).arg(got.mid(std::max<qsizetype>(0, i - 60), 160), want.mid(std::max<qsizetype>(0, i - 60), 160));
}

Clip colorClip(double d, const QString& color = QStringLiteral("#102030")) {
    Clip c;
    c.kind = ClipKind::Color;
    c.color = color;
    c.duration = d;
    return c;
}
Clip imageClip(const QString& src, double d) {
    Clip c;
    c.kind = ClipKind::Image;
    c.src = src;
    c.duration = d;
    return c;
}
QString graphLine(const Plan& p, const QString& startsWith) {
    for (const QString& l : p.join.graph.split(QStringLiteral(";\n")))
        if (l.startsWith(startsWith)) return l;
    return {};
}
QString argAfter(const QStringList& args, const QString& flag) {
    const qsizetype i = args.indexOf(flag);
    return i >= 0 ? args.value(i + 1) : QString();
}

QJsonObject goldenCaptions() {
    static const QJsonObject g = [] {
        QFile f(QStringLiteral(STUDIO_SOURCE_DIR "/src/compose/golden/captions-golden.json"));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return g;
}
/// A golden time: a number, or "NaN" / "Infinity" / "-Infinity" / "-0" (what JSON cannot hold).
double goldenTime(const QJsonValue& v) {
    if (v.isDouble()) return v.toDouble();
    const QString s = v.toString();
    if (s == u"NaN") return std::numeric_limits<double>::quiet_NaN();
    if (s == u"Infinity") return std::numeric_limits<double>::infinity();
    if (s == u"-Infinity") return -std::numeric_limits<double>::infinity();
    if (s == u"-0") return -0.0;
    return 0;
}

/// A loudnorm analysis as ffmpeg 9 prints it at -loglevel info (print_format=json).
QString loudnormLog(const QString& i, const QString& tp = QStringLiteral("-6.10"), const QString& lra = QStringLiteral("3.20")) {
    return QStringLiteral("Input #0, wav, from 'mix.wav':\n  Duration: 00:00:06.00\n[Parsed_loudnorm_0 @ 0000027f564ace40] \n{\n"
                          "\t\"input_i\" : \"%1\",\n\t\"input_tp\" : \"%2\",\n\t\"input_lra\" : \"%3\",\n\t\"input_thresh\" : \"-37.80\",\n"
                          "\t\"output_i\" : \"-22.95\",\n\t\"output_tp\" : \"-2.00\",\n\t\"output_lra\" : \"3.00\",\n\t\"output_thresh\" : \"-33.00\",\n"
                          "\t\"normalization_type\" : \"dynamic\",\n\t\"target_offset\" : \"-0.05\"\n}\n"
                          "[out#0/null @ 0000027f584e4380] video:0KiB audio:3000KiB\n").arg(i, tp, lra);
}

/// A render backend that runs nothing: each job "takes" delayMs, writes its output file and answers. It records the
/// order jobs start and end, the most that run at once, and can fail, refuse or hang a job by its label.
class FakeBackend;
class FakeRun final : public RenderRun {
public:
    FakeRun(FakeBackend* b, RenderJob job, QObject* parent);
    void cancel() override;
    void complete();
    RenderJob job;

private:
    void answer(const RenderJobResult& r);
    FakeBackend* m_b;
    QTimer m_timer;
    bool m_done = false;
};

class FakeBackend final : public IRenderBackend {
public:
    QString name() const override { return QStringLiteral("fake"); }
    QString program() const override { return QStringLiteral("fake"); }
    bool available(QString* why) const override { if (!ok && why) *why = QStringLiteral("fake is off"); return ok; }
    QString unsupported(const RenderJob& j) const override { return j.label == refuseLabel ? QStringLiteral("not in the subset") : QString(); }
    RenderRun* start(const RenderJob& j, QObject* parent) override {
        started << j.label;
        events << u'+' + j.label;
        jobs << j;
        peak = std::max(peak, ++current);
        auto* r = new FakeRun(this, j, parent);
        runs << r;
        return r;
    }
    bool ok = true;
    int delayMs = 25;
    QString failLabel, refuseLabel, hangLabel;
    QString mixLoudness = QStringLiteral("-27.50"), outLoudness = QStringLiteral("-23.10");
    QStringList started, ended, events;                           // events: "+label" at start, "-label" at the end
    QVector<RenderJob> jobs;
    QVector<QPointer<FakeRun>> runs;
    int current = 0, peak = 0, cancelled = 0;
    QStringList graphsSeen;                                       // graph files as they were when their step started
};

FakeRun::FakeRun(FakeBackend* b, RenderJob j, QObject* parent) : RenderRun(parent), job(std::move(j)), m_b(b) {
    // the renderer has written the step's graph file before the job starts: keep what it says
    const qsizetype gi = job.args.indexOf(QStringLiteral("-/filter_complex"));
    if (gi >= 0) {
        QFile f(job.cwd + u'/' + job.args.value(gi + 1));
        if (f.open(QIODevice::ReadOnly)) m_b->graphsSeen << QString::fromUtf8(f.readAll());
    }
    m_timer.setSingleShot(true);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { complete(); });
    if (job.label != m_b->hangLabel) m_timer.start(m_b->delayMs);
}
void FakeRun::cancel() {
    if (m_done) return;
    ++m_b->cancelled;
    RenderJobResult r;
    r.cancelled = true;
    r.error = QStringLiteral("cancelled");
    answer(r);
}
void FakeRun::complete() {
    RenderJobResult r;
    if (job.label == m_b->failLabel) {
        r.exitCode = 1;
        r.error = QStringLiteral("ffmpeg failed (exit 1): ") + stderrTail(u"frame=  1\n[vost#0] Error while filtering: Invalid argument\nConversion failed!");
        answer(r);
        return;
    }
    const QString out = job.args.value(job.args.size() - 1);
    if (out != u"-") {
        QFile f(QDir::isAbsolutePath(out) ? out : job.cwd + u'/' + out);
        if (f.open(QIODevice::WriteOnly)) f.write("fake");
    }
    if (job.args.join(u' ').contains(QLatin1String("print_format=json")))   // a loudness analysis: the mix or the output
        r.log = loudnormLog(job.args.contains(QStringLiteral("mix.wav")) ? m_b->mixLoudness : m_b->outLoudness);
    r.ok = true;
    r.exitCode = 0;
    answer(r);
}
void FakeRun::answer(const RenderJobResult& r) {
    if (m_done) return;
    m_done = true;
    m_timer.stop();
    --m_b->current;
    m_b->ended << job.label;
    m_b->events << u'-' + job.label;
    QMetaObject::invokeMethod(this, [this, r] { emit finished(r); }, Qt::QueuedConnection);
}

/// Runs one render to its end; the progress it reported goes to *progress.
RenderResult renderNow(Renderer& r, const Timeline& t, const RenderOptions& o, QVector<QPair<double, QString>>* progress = nullptr,
                       int timeoutMs = 90000) {
    RenderResult res;
    int finishedCount = 0;
    QEventLoop loop;
    const auto c1 = QObject::connect(&r, &Renderer::finished, &loop, [&](const RenderResult& x) { res = x; ++finishedCount; loop.quit(); });
    const auto c2 = QObject::connect(&r, &Renderer::progress, &loop, [&](double f, const QString& m) { if (progress) progress->push_back({f, m}); });
    QTimer::singleShot(timeoutMs, &loop, [&] { res.error = QStringLiteral("test timeout"); loop.quit(); });
    if (!r.start(t, o)) { res.error = QStringLiteral("start refused"); return res; }
    loop.exec();
    QObject::disconnect(c1);
    QObject::disconnect(c2);
    QCoreApplication::processEvents();                             // a second finished() would land here
    if (finishedCount != 1 && res.error != u"test timeout") res.error = QStringLiteral("finished %1 times").arg(finishedCount);
    return res;
}

/// Mean absolute difference of two RGB24 thumbnails (0..255).
double meanDiff(const QByteArray& a, const QByteArray& b) {
    if (a.size() != b.size() || a.isEmpty()) return 255;
    double s = 0;
    for (qsizetype i = 0; i < a.size(); ++i) s += std::abs(int(uchar(a[i])) - int(uchar(b[i])));
    return s / double(a.size());
}

} // namespace

class TestStudioCompose : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_ffmpeg;

    bool runFfmpeg(const QStringList& args, const QString& cwd, QString* err, bool progress = false) {
        QProcess p;
        p.setWorkingDirectory(cwd);
        p.setProcessChannelMode(QProcess::SeparateChannels);
        QStringList full{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error")};
        if (progress) full << QStringLiteral("-progress") << QStringLiteral("pipe:1") << QStringLiteral("-nostats");
        p.start(m_ffmpeg, full + args);
        if (!p.waitForStarted(10000)) { *err = QStringLiteral("ffmpeg did not start"); return false; }
        if (!p.waitForFinished(90000)) { p.kill(); p.waitForFinished(); *err = QStringLiteral("ffmpeg timed out"); return false; }
        const QByteArray out = p.readAllStandardOutput();
        if (progress && !out.contains("out_time_us=")) { *err = QStringLiteral("no -progress output"); return false; }
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            *err = QString::fromUtf8(p.readAllStandardError()).right(1500);
            return false;
        }
        return true;
    }

    /// The renderer's job in miniature (ST-18b does it for real): stage, write the text files and the graph, run the
    /// segments, the join and the poster.
    bool runPlan(const Plan& plan, const QString& work, const QString& finalFile, QString* err) {
        for (const Step* s : plan.steps()) {
            for (const StagedFile& f : s->staged) {
                QFile::remove(work + u'/' + f.name);
                if (!QFile::copy(f.from, work + u'/' + f.name)) { *err = QStringLiteral("stage ") + f.from; return false; }
            }
            for (const TextFile& t : s->texts) {
                QFile f(work + u'/' + t.name);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(t.text.toUtf8()) < 0) { *err = QStringLiteral("write ") + t.name; return false; }
            }
            if (!s->graphFile.isEmpty()) {
                QFile f(work + u'/' + s->graphFile);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(s->graph.toUtf8()) < 0) { *err = QStringLiteral("write graph"); return false; }
            }
            if (s->kind == Step::Kind::Poster) {
                QFile::remove(finalFile);
                if (!QFile::copy(plan.join.output, finalFile)) { *err = QStringLiteral("copy the output"); return false; }
            }
            QString e;
            if (!runFfmpeg(s->args, work, &e, s->progress)) {
                *err = QStringLiteral("step %1 (slot %2) failed: %3\nargs: %4").arg(int(s->kind)).arg(s->slot).arg(e, s->args.join(u' ').left(3000));
                return false;
            }
        }
        return true;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        if (qEnvironmentVariableIsSet("STUDIO_COMPOSE_KEEP")) {   // keep the rendered files to look at them
            m_tmp.setAutoRemove(false);
            qInfo("keeping %s", qPrintable(m_tmp.path()));
        }
        QVERIFY2(!golden().isEmpty(), "compose-golden.json missing or unreadable");
        QVERIFY(golden().value(QStringLiteral("source")).toString().startsWith(QLatin1String("mcp/spot/compose.ts")));
        const QStringList candidates{
            QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe"),
            qEnvironmentVariable("PLAYPLAY_FFMPEG"),
            QStringLiteral("C:/SDKs/ffmpeg-9.0-win64-gpl-shared/bin/ffmpeg.exe"),
        };
        for (const QString& c : candidates) if (!c.isEmpty() && QFile::exists(c)) { m_ffmpeg = c; break; }
    }

    // ================================================================ enums, sizes, numbers

    void enumsAndSizes() {
        const QJsonObject g = golden();
        QStringList tr, kb, as;
        for (Transition t : allTransitions()) tr << transitionName(t);
        for (KenBurns k : allKenBurns()) kb << kenburnsName(k);
        for (Aspect a : allAspects()) as << aspectName(a);
        QCOMPARE(tr, strings(g[QStringLiteral("transitions")]));
        QCOMPARE(kb, strings(g[QStringLiteral("kenburns")]));
        QCOMPARE(as, strings(g[QStringLiteral("aspects")]));
        for (const QJsonValue& v : g[QStringLiteral("frameSize")].toArray()) {
            const QJsonObject o = v.toObject();
            const FrameSize f = frameSize(*parseAspect(o[QStringLiteral("aspect")].toString()), *parseQuality(o[QStringLiteral("quality")].toString()));
            QCOMPARE(f.w, o[QStringLiteral("w")].toInt());
            QCOMPARE(f.h, o[QStringLiteral("h")].toInt());
        }
        for (const QJsonValue& v : g[QStringLiteral("safeArea")].toArray()) {
            const QJsonObject o = v.toObject();
            const SafeArea s = safeArea(*parseAspect(o[QStringLiteral("aspect")].toString()));
            QCOMPARE(s.marginX, o[QStringLiteral("marginX")].toDouble());
            QCOMPARE(s.headTop, o[QStringLiteral("headTop")].toDouble());
            QCOMPARE(s.capBottom, o[QStringLiteral("capBottom")].toDouble());
        }
        // aliases from ads/types.ts
        QCOMPARE(*parseTransition(u"crossfade"), Transition::Fade);
        QCOMPARE(*parseFit(u"blur"), Fit::Contain);
        QVERIFY(!parseKenBurns(u"spin"));
        QVERIFY(!parseAspect(u"3:2"));
    }

    void numbers() {
        QCOMPARE(jsRound(2.5), 3.0);
        QCOMPARE(jsRound(-2.5), -2.0);                 // Math.round rounds half towards +inf
        QCOMPARE(jsRound(0.49999999999999994), 0.0);   // floor(x + 0.5) would give 1
        QCOMPARE(jsRound(-0.4), -0.0);
        QCOMPARE(num(jsRound(-0.4)), QStringLiteral("0"));
        QCOMPARE(r3(1.23456), 1.235);
        QCOMPARE(r3(1.0005), 1.001);                   // 1.0005 * 1000 rounds to exactly 1000.5 (node prints 1.001 too)
        QCOMPARE(num(0.15 + 0.3), QStringLiteral("0.44999999999999996"));   // the end card's fade-in times, as the TS prints them
        QCOMPARE(num(0.15 + 0.12 + 0.3), QStringLiteral("0.5700000000000001"));
        QCOMPARE(num(1e21), QStringLiteral("1e+21"));
        QCOMPARE(num(12), QStringLiteral("12"));
    }

    // ================================================================ text helpers vs the TS

    void cleanTextParity() {
        for (const QJsonValue& v : golden()[QStringLiteral("cleanText")].toArray()) {
            const QJsonArray p = v.toArray();
            QCOMPARE(cleanText(p[0].toString()), p[1].toString());
        }
    }

    void extendedPictographicTable() {
        // the built-in table (used when PCRE2 lacks the property) agrees with PCRE2 on every code point up to U+3FFFF
        if (!detail::extPictRegexAvailable()) QSKIP("this PCRE2 has no \\p{Extended_Pictographic}; the table is in use (the cleanText parity above covers it)");
        static const QRegularExpression re(QStringLiteral("^\\p{Extended_Pictographic}$"));
        int disagree = 0;
        QString first;
        for (char32_t cp = 0; cp < 0x40000; ++cp) {
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            const bool rx = re.match(QString::fromUcs4(&cp, 1)).hasMatch();
            if (rx != detail::extPictTable(cp)) {
                if (!disagree) first = QStringLiteral("U+%1 pcre2=%2").arg(uint(cp), 4, 16, QLatin1Char('0')).arg(rx);
                ++disagree;
            }
        }
        QVERIFY2(disagree == 0, qPrintable(QStringLiteral("%1 code points differ, first %2").arg(disagree).arg(first)));
        QVERIFY(isExtendedPictographic(0x1F389));
        QVERIFY(isExtendedPictographic(0xA9));
        QVERIFY(!isExtendedPictographic(0x1F1FA));    // regional indicators (flags) are not
        QVERIFY(!isExtendedPictographic(u'A'));
    }

    void wrapTextParity() {
        for (const QJsonValue& v : golden()[QStringLiteral("wrapText")].toArray()) {
            const QJsonObject o = v.toObject();
            const Wrapped w = wrapText(o[QStringLiteral("text")].toString(), o[QStringLiteral("maxChars")].toInt(), o[QStringLiteral("maxLines")].toInt());
            QCOMPARE(w.lines, strings(o[QStringLiteral("lines")]));
            QCOMPARE(w.truncated, o[QStringLiteral("truncated")].toBool());
        }
        // test/mcp-spot.ts: "wrapText wraps and marks truncation with an ellipsis"
        const Wrapped w = wrapText(u"Every morning starts with a choice and this one is a very good choice indeed", 16, 2);
        QCOMPARE(w.lines, (QStringList{QStringLiteral("Every morning"), QStringLiteral("starts with a\u2026")}));
        QVERIFY(w.truncated);
    }

    void hexColorParity() {
        for (const QJsonValue& v : golden()[QStringLiteral("hexColor")].toArray()) {
            const QJsonObject o = v.toObject();
            QCOMPARE(hexColor(o[QStringLiteral("input")].toString(), o[QStringLiteral("fallback")].toString()), o[QStringLiteral("out")].toString());
        }
        QCOMPARE(textOn(QStringLiteral("0xFFFFFF")), QStringLiteral("0x111111"));
        QCOMPARE(textOn(QStringLiteral("0x0B3D91")), QStringLiteral("0xFFFFFF"));
        QCOMPARE(textOn(QString()), QStringLiteral("0xFFFFFF"));   // parseInt NaN in the TS
    }

    void cuesForLineParity() {
        for (const QJsonValue& v : golden()[QStringLiteral("cuesForLine")].toArray()) {
            const QJsonObject o = v.toObject();
            const QVector<CaptionCue> cues = cuesForLine(o[QStringLiteral("text")].toString(), o[QStringLiteral("start")].toDouble(),
                                                         o[QStringLiteral("seconds")].toDouble(), o[QStringLiteral("maxWords")].toInt());
            const QJsonArray want = o[QStringLiteral("cues")].toArray();
            QCOMPARE(cues.size(), want.size());
            for (qsizetype i = 0; i < cues.size(); ++i) {
                const QJsonObject w = want[i].toObject();
                QCOMPARE(cues[i].start, w[QStringLiteral("start")].toDouble());
                QCOMPARE(cues[i].end, w[QStringLiteral("end")].toDouble());
                QCOMPARE(cues[i].text, w[QStringLiteral("text")].toString());
            }
        }
    }

    // ================================================================ whole plans vs the TS

    void goldenPlans_data() {
        QTest::addColumn<QJsonObject>("c");
        for (const QJsonValue& v : golden()[QStringLiteral("cases")].toArray())
            QTest::newRow(qPrintable(v.toObject()[QStringLiteral("name")].toString())) << v.toObject();
        QVERIFY(golden()[QStringLiteral("cases")].toArray().size() >= 30);
    }

    void goldenPlans() {
        QFETCH(QJsonObject, c);
        QString err;
        const Timeline t = Timeline::fromJson(c[QStringLiteral("spec")].toObject(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const Plan p = buildPlan(t, goldenOptions());
        if (c.contains(QStringLiteral("error"))) {
            QVERIFY(!p.ok);
            QCOMPARE(p.error, c[QStringLiteral("error")].toString());
            return;
        }
        QVERIFY2(p.ok, qPrintable(p.error));
        const QJsonArray calls = c[QStringLiteral("calls")].toArray();
        const QVector<const Step*> steps = p.steps();
        QCOMPARE(steps.size(), calls.size());
        for (qsizetype k = 0; k < steps.size(); ++k) {
            const QJsonObject call = calls[k].toObject();
            const QStringList want = strings(call[QStringLiteral("args")]);
            QVERIFY2(steps[k]->args == want, qPrintable(QStringLiteral("step %1 ").arg(k) + listDiff(steps[k]->args, want)));
            QCOMPARE(steps[k]->progress, call[QStringLiteral("progress")].toBool());
            QCOMPARE(steps[k]->output, want.last());
        }
        const QString graph = c[QStringLiteral("graph")].toString();
        QVERIFY2(p.join.graph == graph, qPrintable(textDiff(p.join.graph, graph)));
        // drawtext files: numbered across the plan in the TS order, same text
        QStringList names, texts;
        for (const Step* s : steps)
            for (const TextFile& f : s->texts) { names << f.name; texts << f.text; }
        QCOMPARE(texts, strings(c[QStringLiteral("texts")]));
        for (qsizetype i = 0; i < names.size(); ++i) QCOMPARE(names[i], QStringLiteral("t%1.txt").arg(i));
        // staged stills and logo
        QJsonArray staged;
        for (const Step* s : steps)
            for (const StagedFile& f : s->staged) staged.append(QJsonArray{f.from, f.name});
        QCOMPARE(staged, c[QStringLiteral("staged")].toArray());
        // the result the TS returns
        const QJsonObject r = c[QStringLiteral("result")].toObject();
        QCOMPARE(p.width, r[QStringLiteral("width")].toInt());
        QCOMPARE(p.height, r[QStringLiteral("height")].toInt());
        QCOMPARE(p.fps, r[QStringLiteral("fps")].toDouble());
        QCOMPARE(p.total, r[QStringLiteral("duration")].toDouble());
        QCOMPARE(p.warnings, strings(r[QStringLiteral("warnings")]));
        const QJsonArray tl = p.timelineJson(), wantTl = r[QStringLiteral("timeline")].toArray();
        QCOMPARE(tl.size(), wantTl.size());
        for (qsizetype i = 0; i < tl.size(); ++i) {
            const QJsonObject a = tl[i].toObject(), b = wantTl[i].toObject();
            QCOMPARE(a.keys(), b.keys());
            QCOMPARE(a[QStringLiteral("start")].toDouble(), b[QStringLiteral("start")].toDouble());
            QCOMPARE(a[QStringLiteral("end")].toDouble(), b[QStringLiteral("end")].toDouble());
            QCOMPARE(a[QStringLiteral("kind")].toString(), b[QStringLiteral("kind")].toString());
            QCOMPARE(a[QStringLiteral("label")].toString(), b[QStringLiteral("label")].toString());
        }
        // the same plan after a JSON round trip of the model
        QString err2;
        const Timeline t2 = Timeline::fromJson(t.toJson(), &err2);
        QVERIFY2(err2.isEmpty(), qPrintable(err2));
        const Plan p2 = buildPlan(t2, goldenOptions());
        QCOMPARE(p2.join.args, p.join.args);
        QCOMPARE(p2.join.graph, p.join.graph);
        for (qsizetype k = 0; k < p.segments.size(); ++k) QCOMPARE(p2.segments[k].args, p.segments[k].args);
        QCOMPARE(t2.toJson(), t.toJson());
    }

    // ================================================================ readable golden strings

    void transitionStrings() {
        // three cards 2 / 1.5 / 1 s, 0.8 s asked: overlaps 0.6 (40 % of 1.5) and 0.4 (40 % of 1), offsets at the slot starts
        const QMap<Transition, QString> xf{{Transition::Fade, "fade"}, {Transition::Dissolve, "dissolve"}, {Transition::Dip, "fadeblack"},
                                            {Transition::Slide, "slideleft"}, {Transition::Wipe, "wipeleft"}, {Transition::Zoom, "zoomin"}};
        for (Transition tr : allTransitions()) {
            Timeline t;
            t.aspect = Aspect::Landscape;
            t.transitionSeconds = 0.8;
            t.clips = {colorClip(2), colorClip(1.5, QStringLiteral("#304050")), colorClip(1, QStringLiteral("#506070"))};
            t.clips[0].transition = t.clips[1].transition = tr;
            const Plan p = buildPlan(t);
            QVERIFY(p.ok);
            if (tr == Transition::Cut) {
                QCOMPARE(xfadeName(tr), QString());
                QCOMPARE(graphLine(p, QStringLiteral("[s0][s1]")), QStringLiteral("[s0][s1]concat=n=2:v=1:a=0,settb=AVTB[j1]"));
                QCOMPARE(graphLine(p, QStringLiteral("[j1][s2]")), QStringLiteral("[j1][s2]concat=n=2:v=1:a=0,settb=AVTB[j2]"));
                QCOMPARE(argAfter(p.segments[0].args, QStringLiteral("-t")), QStringLiteral("2"));
                continue;
            }
            QCOMPARE(xfadeName(tr), xf.value(tr));
            QCOMPARE(graphLine(p, QStringLiteral("[s0][s1]")), QStringLiteral("[s0][s1]xfade=transition=%1:duration=0.6:offset=2[j1]").arg(xf.value(tr)));
            QCOMPARE(graphLine(p, QStringLiteral("[j1][s2]")), QStringLiteral("[j1][s2]xfade=transition=%1:duration=0.4:offset=3.5[j2]").arg(xf.value(tr)));
            // each segment is its slot + the overlap into the next one
            QCOMPARE(argAfter(p.segments[0].args, QStringLiteral("-t")), QStringLiteral("2.6"));
            QCOMPARE(argAfter(p.segments[1].args, QStringLiteral("-t")), QStringLiteral("1.9"));
            QCOMPARE(argAfter(p.segments[2].args, QStringLiteral("-t")), QStringLiteral("1"));
            QCOMPARE(p.total, 4.5);
        }
        // a per-clip length (ads/types.ts transition.dur) wins over the timeline's, still capped at 40 %
        Timeline t;
        t.clips = {colorClip(3), colorClip(3)};
        t.clips[0].transition = Transition::Dip;
        t.clips[0].transitionSeconds = 1.0;
        QCOMPARE(graphLine(buildPlan(t), QStringLiteral("[s0][s1]")), QStringLiteral("[s0][s1]xfade=transition=fadeblack:duration=1:offset=3[j1]"));
        t.clips[0].transitionSeconds = 9.0;   // clamped to 1.5
        QCOMPARE(graphLine(buildPlan(t), QStringLiteral("[s0][s1]")), QStringLiteral("[s0][s1]xfade=transition=fadeblack:duration=1.2:offset=3[j1]"));
    }

    void kenburnsStrings() {
        const auto kb = [](KenBurns k) { const ZoomExpr e = kenburnsExpr(k, 96); return QStringList{e.z, e.x, e.y}; };
        QCOMPARE(kb(KenBurns::In), (QStringList{"1+0.12*on/95", "iw/2-(iw/zoom/2)", "ih/2-(ih/zoom/2)"}));
        QCOMPARE(kb(KenBurns::Out), (QStringList{"1.12-0.12*on/95", "iw/2-(iw/zoom/2)", "ih/2-(ih/zoom/2)"}));
        QCOMPARE(kb(KenBurns::Left), (QStringList{"1.12", "(iw-iw/zoom)*(1-on/95)", "ih/2-(ih/zoom/2)"}));
        QCOMPARE(kb(KenBurns::Right), (QStringList{"1.12", "(iw-iw/zoom)*on/95", "ih/2-(ih/zoom/2)"}));
        QCOMPARE(kb(KenBurns::Up), (QStringList{"1.12", "iw/2-(iw/zoom/2)", "(ih-ih/zoom)*(1-on/95)"}));
        QCOMPARE(kb(KenBurns::Down), (QStringList{"1.12", "iw/2-(iw/zoom/2)", "(ih-ih/zoom)*on/95"}));
        QCOMPARE(kb(KenBurns::None), (QStringList{"1", "0", "0"}));
        QCOMPARE(kenburnsExpr(KenBurns::In, 1).z, QStringLiteral("1+0.12*on/1"));   // never divides by zero
        // a 3.2 s still at 30 fps, 9:16 draft: 96 frames on a 1.5x frame (540x960) down to 360x640
        Timeline t;
        t.quality = Quality::Draft;
        t.clips = {imageClip(QStringLiteral("C:/media/a still.png"), 3.2)};
        t.clips[0].kenburns = KenBurns::In;
        const Plan p = buildPlan(t);
        QCOMPARE(p.segments[0].graph, QStringLiteral(
            "[0:v]scale=540:960:force_original_aspect_ratio=increase,crop=540:960,setsar=1[fit];"
            "[fit]zoompan=z='1+0.12*on/95':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=96:s=360x640:fps=30,setsar=1,"
            "scale=out_range=tv,format=yuv420p[v]"));
        // limited range by default (a JPEG decodes full range); compose.ts's plain format=yuv420p only for the golden test
        PlanOptions tsRange;
        tsRange.limitedRange = false;
        QVERIFY(buildPlan(t, tsRange).segments[0].graph.endsWith(QLatin1String(",setsar=1,format=yuv420p[v]")));
        QCOMPARE(p.segments[0].staged.value(0).name, QStringLiteral("img0.png"));
        QCOMPARE(argAfter(p.segments[0].args, QStringLiteral("-frames:v")), QStringLiteral("96"));
        // auto cycles in, right, out, left, in, up by clip index; final quality upscales 2x
        Timeline a;
        for (int i = 0; i < 7; ++i) a.clips << imageClip(QStringLiteral("a.jpg"), 1);
        const Plan pa = buildPlan(a);
        const QStringList want{"z='1+0.12*on/29'", "x='(iw-iw/zoom)*on/29'", "z='1.12-0.12*on/29'", "x='(iw-iw/zoom)*(1-on/29)'",
                               "z='1+0.12*on/29'", "y='(ih-ih/zoom)*(1-on/29)'", "z='1+0.12*on/29'"};
        for (int i = 0; i < 7; ++i) {
            QVERIFY2(pa.segments[i].graph.contains(want[i]), qPrintable(pa.segments[i].graph));
            QVERIFY(pa.segments[i].graph.startsWith(QLatin1String("[0:v]scale=2160:3840:")));
        }
    }

    void duckingStrings() {
        Timeline t;
        t.quality = Quality::Draft;
        t.clips = {colorClip(4, QStringLiteral("#000000"))};
        t.audio.voiceover = AudioPlacement{QStringLiteral("vo.wav"), 0.5};
        t.audio.music = MusicBed{QStringLiteral("music.wav"), 0.3, true};
        const Plan p = buildPlan(t);
        const QStringList audio = p.join.graph.split(QStringLiteral(";\n")).mid(2);
        QCOMPARE(audio, (QStringList{
            "[1:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,adelay=500:all=1[vo0]",
            "[vo0]anull[vo]",
            "[2:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,atrim=0:4,asetpts=PTS-STARTPTS,volume=0.3,afade=t=in:d=0.3,afade=t=out:st=3:d=1[mus]",
            "[vo]asplit=2[vo1][vosc]",
            "[mus][vosc]sidechaincompress=threshold=0.015:ratio=12:attack=20:release=400[duck]",
            "[vo1][duck]amix=inputs=2:normalize=0:duration=longest[mix]",
            "[mix]apad,atrim=0:4,loudnorm=I=-16:TP=-1.5:LRA=11,aresample=48000,asetpts=N/SR/TB,apad=whole_dur=4,atrim=0:4,asetpts=PTS-STARTPTS[aout]"}));
        // every input behind a file-only protocol whitelist (the default); the music bed looped
        QCOMPARE(p.join.args.mid(0, 14), (QStringList{"-protocol_whitelist", "file", "-i", "seg000.mp4", "-protocol_whitelist", "file", "-i", "vo.wav",
                                                      "-stream_loop", "-1", "-protocol_whitelist", "file", "-i", "music.wav"}));
        QCOMPARE(inputsOf(p.poster.args), QStringList{"out.mp4"});
        PlanOptions bare;
        bare.fileOnly = false;
        QCOMPARE(buildPlan(t, bare).join.args.mid(0, 8), (QStringList{"-i", "seg000.mp4", "-i", "vo.wav", "-stream_loop", "-1", "-i", "music.wav"}));
        // duck off: a plain mix; broadcast loudness
        t.audio.music->duck = false;
        t.audio.loudness = Loudness::Broadcast;
        const Plan q = buildPlan(t);
        QCOMPARE(graphLine(q, QStringLiteral("[vo][mus]")), QStringLiteral("[vo][mus]amix=inputs=2:normalize=0:duration=longest[mix]"));
        QVERIFY(graphLine(q, QStringLiteral("[mix]")).contains(QLatin1String(",loudnorm=I=-24:TP=-2:LRA=7,")));
        QVERIFY(!q.join.graph.contains(QLatin1String("sidechaincompress")));
        // no sound at all: a silent track of the exact length, no loudness pass
        Timeline s;
        s.clips = {colorClip(2.5)};
        const Plan ps = buildPlan(s);
        QCOMPARE(graphLine(ps, QStringLiteral("anullsrc")), QStringLiteral("anullsrc=r=48000:cl=stereo,atrim=0:2.5[mix]"));
        QVERIFY(!ps.join.graph.contains(QLatin1String("loudnorm")));
    }

    void exactLength() {
        // longer than the clips: the last picture is cloned, then everything is trimmed to the exact length
        Timeline t;
        t.clips = {colorClip(1.2)};
        t.targetSeconds = 4.75;
        Plan p = buildPlan(t);
        QCOMPARE(p.total, 4.75);
        QCOMPARE(graphLine(p, QStringLiteral("[s0]")), QStringLiteral("[s0]tpad=stop_mode=clone:stop_duration=5,trim=duration=4.75,setpts=PTS-STARTPTS,format=yuv420p[vout]"));
        QCOMPARE(argAfter(p.join.args, QStringLiteral("-t")), QStringLiteral("4.75"));
        // shorter: trimmed, and captions past the end are dropped / cut at the end
        t.clips = {colorClip(3), colorClip(3)};
        t.targetSeconds = 4.5;
        t.captions = {{4, 9, QStringLiteral("runs past")}, {5, 6, QStringLiteral("after the end")}};
        p = buildPlan(t);
        QVERIFY(p.join.graph.contains(QLatin1String("enable='between(t,4,4.5)'")));
        QVERIFY(!p.join.graph.contains(QLatin1String("between(t,5,")));
        QCOMPARE(p.join.texts.size(), 1);
        QVERIFY(graphLine(p, QStringLiteral("[j1]")).endsWith(QLatin1String("tpad=stop_mode=clone:stop_duration=1,trim=duration=4.5,setpts=PTS-STARTPTS,format=yuv420p[vout]")));
        QCOMPARE(p.timelineJson()[1].toObject()[QStringLiteral("end")].toDouble(), 4.5);
        // the audio is padded and trimmed to the same length after loudness normalisation
        t.audio.voiceover = AudioPlacement{QStringLiteral("long.wav")};
        p = buildPlan(t);
        QVERIFY(graphLine(p, QStringLiteral("[vo]")).endsWith(QLatin1String("loudnorm=I=-16:TP=-1.5:LRA=11,aresample=48000,asetpts=N/SR/TB,apad=whole_dur=4.5,atrim=0:4.5,asetpts=PTS-STARTPTS[aout]")));
        // poster: 40 % into the first slot, at least 0.5 s, never past the end
        QCOMPARE(argAfter(p.poster.args, QStringLiteral("-ss")), QStringLiteral("1.2"));
        t.posterAt = 99;
        QCOMPARE(argAfter(buildPlan(t).poster.args, QStringLiteral("-ss")), QStringLiteral("4.45"));
    }

    void safeAreaCaptions() {
        // captions sit on the caption line of the safe area (above the bottom UI band on 9:16), two lines at most
        Timeline t;
        t.clips = {colorClip(4)};
        t.captions = {{0, 2, QStringLiteral("A caption that runs long enough to wrap")}};
        const Plan p = buildPlan(t);
        const SafeArea sa = safeArea(Aspect::Vertical);
        QCOMPARE(sa.capBottom, 0.76);
        QCOMPARE(p.join.texts.size(), 2);
        // 1080 x 1920: size round(1080 * 0.058) = 63, 25 characters a line, line height 81; the block ends at 1920 * 0.76
        const QString line = graphLine(p, QStringLiteral("[s0]"));
        QVERIFY2(line.contains(QLatin1String("fontsize=63")) && line.contains(QLatin1String(":y=1315:")) && line.contains(QLatin1String(":y=1396:")), qPrintable(line));
        QCOMPARE(p.join.texts.value(0).text, QStringLiteral("A caption that runs long"));
        QCOMPARE(p.join.texts.value(1).text, QStringLiteral("enough to wrap"));
        QVERIFY(line.contains(QLatin1String("box=1:boxcolor=black@0.5")));
        // headlines: a lower third sits above the caption band
        Timeline h;
        h.clips = {imageClip(QStringLiteral("x.png"), 2)};
        h.clips[0].text = QStringLiteral("Lower third");
        h.clips[0].textPosition = TextPosition::Bottom;
        const QString seg = buildPlan(h).segments[0].graph;
        QVERIFY2(seg.contains(QLatin1String(":y=1074:")), qPrintable(seg));   // 1920 * (0.76 - 0.16) - 78
    }

    // ================================================================ ads/types.ts extras

    void adsExtras() {
        // focus: the cover crop centres on the subject, kept inside the picture
        QCOMPARE(fitChain(QStringLiteral("[0:v]"), 540, 960, Fit::Cover, QStringLiteral("[fit]"), 0.2, 0.5),
                 QStringLiteral("[0:v]scale=540:960:force_original_aspect_ratio=increase,crop=540:960:'clip(iw*0.2-ow/2,0,iw-ow)':'clip(ih*0.5-oh/2,0,ih-oh)',setsar=1[fit]"));
        QCOMPARE(fitChain(QStringLiteral("[0:v]"), 540, 960, Fit::Cover, QStringLiteral("[fit]")),
                 QStringLiteral("[0:v]scale=540:960:force_original_aspect_ratio=increase,crop=540:960,setsar=1[fit]"));
        // keyframed moves
        ClipMove m;
        m.from = {0.3, 0.3, 1.2};
        m.to = {0.7, 0.6, 1.0};
        m.ease = Ease::EaseOut;
        ZoomExpr e = moveExpr(m, 31);
        QCOMPARE(e.z, QStringLiteral("1.2-0.2*((on/30)*(2-on/30))"));
        QCOMPARE(e.x, QStringLiteral("clip((0.3+0.4*((on/30)*(2-on/30)))*iw-iw/zoom/2,0,iw-iw/zoom)"));
        QCOMPARE(e.y, QStringLiteral("clip((0.3+0.3*((on/30)*(2-on/30)))*ih-ih/zoom/2,0,ih-ih/zoom)"));
        m.ease = Ease::Linear;
        m.from = {0.5, 0.5, 1};
        m.to = {0.5, 0.5, 20};   // zoom clamped to zoompan's 10
        e = moveExpr(m, 61);
        QCOMPARE(e.z, QStringLiteral("1+9*(on/60)"));
        QCOMPARE(e.x, QStringLiteral("clip((0.5)*iw-iw/zoom/2,0,iw-iw/zoom)"));
        m.ease = Ease::EaseInOut;
        QCOMPARE(moveExpr(m, 61).z, QStringLiteral("1+9*(if(lt(on/60,0.5),2*(on/60)*(on/60),1-2*(1-on/60)*(1-on/60)))"));
        m.ease = Ease::EaseIn;
        QCOMPARE(moveExpr(m, 61).z, QStringLiteral("1+9*((on/60)*(on/60))"));
        // audio "from" (a span of an uploaded narration)
        AudioPlacement a{QStringLiteral("vo.wav"), 0.2};
        a.tempo = 1.2;
        a.from = 0.5;
        QCOMPARE(audioChain(QStringLiteral("[5:a]"), a, 0.2, QStringLiteral("[vo0]")),
                 QStringLiteral("[5:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,atrim=start=0.5,asetpts=PTS-STARTPTS,atempo=1.2,asetpts=N/SR/TB,adelay=200:all=1[vo0]"));

        // a whole timeline with the extras
        Timeline t;
        t.quality = Quality::Draft;
        Clip v;
        v.kind = ClipKind::Video;
        v.src = QStringLiteral("D:/footage/clip.mp4");
        v.duration = 2;
        v.srcStart = 0.5;
        v.speed = 1.5;
        v.focusX = 0.25;
        Clip nat = v;
        nat.speed = 1;
        nat.srcStart = 0.3;
        nat.audioGain = 0.8;
        nat.focusX = 0.5;
        nat.move = ClipMove{{0.5, 0.5, 1}, {0.5, 0.5, 1.3}, Ease::Linear};
        Clip still = imageClip(QStringLiteral("D:/stills/b.webp"), 1.5);
        still.move = ClipMove{{0.3, 0.3, 1.2}, {0.7, 0.6, 1.0}, Ease::EaseOut};
        t.clips = {v, nat, still};
        t.audio.fadeIn = 0.2;
        t.audio.fadeOut = 0.5;
        t.audio.events = {AudioPlacement{QStringLiteral("sting.wav"), 3.0}};
        int probes = 0;
        PlanOptions counted;
        counted.probe = [&probes](const QString&) { ++probes; return SourceInfo{}; };
        const Plan p = buildPlan(t, counted);
        QVERIFY(p.ok);
        QCOMPARE(probes, 2);   // clip.mp4 once (picture for two clips, then its sound), b.webp once
        // trimmed, sped-up clip with a focus crop; with an in-point it holds its last frame instead of looping (a loop
        // would seek back to the file start, not to -ss)
        QCOMPARE(p.segments[0].args.mid(0, 6), (QStringList{"-ss", "0.5", "-protocol_whitelist", "file", "-i", "D:/footage/clip.mp4"}));
        QVERIFY(!p.segments[0].args.contains(QStringLiteral("-stream_loop")));
        QCOMPARE(p.segments[0].graph, QStringLiteral("[0:v]setpts=(PTS-STARTPTS)/1.5,scale=360:640:force_original_aspect_ratio=increase,"
                                                     "crop=360:640:'clip(iw*0.25-ow/2,0,iw-ow)':'clip(ih*0.5-oh/2,0,ih-oh)',setsar=1[fit];"
                                                     "[fit]fps=30,tpad=stop_mode=clone:stop_duration=2,scale=out_range=tv,format=yuv420p[v]"));
        // a push-in across a clip: upscaled, one zoompan frame per input frame
        QCOMPARE(p.segments[1].graph, QStringLiteral("[0:v]scale=540:960:force_original_aspect_ratio=increase,crop=540:960,setsar=1[fit];[fit]fps=30,"
                                                     "tpad=stop_mode=clone:stop_duration=2,"
                                                     "zoompan=z='1+0.3*(on/59)':x='clip((0.5)*iw-iw/zoom/2,0,iw-iw/zoom)':y='clip((0.5)*ih-ih/zoom/2,0,ih-ih/zoom)':d=1:s=360x640:fps=30,setsar=1,scale=out_range=tv,format=yuv420p[v]"));
        // the still's move replaces the Ken Burns preset; the extension is kept for staging
        QVERIFY(p.segments[2].graph.contains(QLatin1String("zoompan=z='1.2-0.2*((on/44)*(2-on/44))':x='clip((0.3+0.4*")));
        QCOMPARE(p.segments[2].staged.value(0).name, QStringLiteral("img2.webp"));
        QCOMPARE(inputsOf(p.segments[2].args), QStringList{"img2.webp"});
        // inputs: segments, the event, the clip's own sound
        QCOMPARE(inputsOf(p.join.args), (QStringList{"seg000.mp4", "seg001.mp4", "seg002.mp4", "sting.wav", "D:/footage/clip.mp4"}));
        // without an in-point a clip that runs out loops, as in compose.ts
        Timeline lp;
        Clip l = v;
        l.srcStart = 0;
        lp.clips = {l};
        QCOMPARE(buildPlan(lp).segments[0].args.mid(0, 6), (QStringList{"-stream_loop", "-1", "-protocol_whitelist", "file", "-i", "D:/footage/clip.mp4"}));
        QVERIFY(!buildPlan(lp).segments[0].graph.contains(QLatin1String("tpad")));
        QCOMPARE(graphLine(p, QStringLiteral("[3:a]")), QStringLiteral("[3:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,adelay=3000:all=1[vo0]"));
        QCOMPARE(graphLine(p, QStringLiteral("[4:a]")), QStringLiteral("[4:a]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,atrim=start=0.3,asetpts=PTS-STARTPTS,"
                                                                       "atrim=0:2,afade=t=out:st=1.85:d=0.15,volume=0.8,adelay=2000:all=1[vo1]"));
        QVERIFY(graphLine(p, QStringLiteral("[vo]")).endsWith(QLatin1String("atrim=0:5.5,asetpts=PTS-STARTPTS,afade=t=in:d=0.2,afade=t=out:st=5:d=0.5[aout]")));
        QVERIFY(p.warnings.isEmpty());
        // natural sound at another speed, or from a clip without sound, is left out with a warning
        t.clips[1].speed = 1.25;
        PlanOptions o;
        o.probe = [](const QString&) { return SourceInfo{true, false}; };
        const Plan q = buildPlan(t, o);
        QCOMPARE(q.warnings, QStringList{"scene 2: the clip's own sound is left out at speed 1.25"});
        t.clips[1].speed = 1;
        QCOMPARE(buildPlan(t, o).warnings, QStringList{"scene 2: the clip has no sound (clip.mp4)"});
    }

    void adsJson() {
        // the ads/types.ts shapes: srcEnd, focus, zoom, fit blur, transition objects (entered from the previous clip),
        // an AudioPlan with events / gain / dur / from and master fades
        const QByteArray text = R"({
          "aspect": "16:9", "fps": 25,
          "clips": [
            {"kind": "video", "src": "C:/m/master.mp4", "srcStart": 2, "srcEnd": 5, "speed": 1.5, "focus": {"x": 0.3, "y": 0.4}, "fit": "blur"},
            {"kind": "image", "src": "C:/m/a.png", "duration": 2, "zoom": {"from": 1, "to": 1.2}, "focus": {"x": 0.3, "y": 0.4},
             "transition": {"type": "crossfade", "dur": 0.8}},
            {"kind": "image", "src": "C:/m/b.png", "duration": 2, "move": {"from": {"x": 0.2, "y": 0.5, "z": 1.3}, "to": {"x": 0.8, "y": 0.5, "z": 1.1}, "ease": "easeInOut"},
             "focus": {"x": 0.9, "y": 0.9}, "transition": {"type": "dip", "dur": 0.4}},
            {"image": "C:/m/c.png", "duration": 1, "voiceover": "C:/m/line.wav"}
          ],
          "audio": {"vo": {"path": "C:/m/vo.wav", "start": 0.5, "gain": 0.9}, "events": [{"path": "C:/m/e.wav", "start": 4, "gain": 0.5, "from": 1, "dur": 2}],
                    "music": {"path": "C:/m/bed.mp3", "gain": 0.4, "duck": true}, "fadeIn": 0.3, "fadeOut": 1, "loudness": "broadcast"}
        })";
        QString err;
        const Timeline t = Timeline::fromJson(QJsonDocument::fromJson(text).object(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(t.aspect, Aspect::Landscape);
        QCOMPARE(t.fps, 25.0);
        QCOMPARE(t.clips.size(), 4);
        QCOMPARE(t.clips[0].duration, 2.0);                // (5 - 2) / 1.5
        QCOMPARE(t.clips[0].fit, Fit::Contain);
        QCOMPARE(t.clips[0].focusX, 0.3);
        QCOMPARE(*t.clips[0].transition, Transition::Fade);  // clip 1 is entered with a crossfade
        QCOMPARE(*t.clips[0].transitionSeconds, 0.8);
        QCOMPARE(*t.clips[1].transition, Transition::Dip);
        QVERIFY(t.clips[1].move);
        QCOMPARE(t.clips[1].move->to.z, 1.2);
        QCOMPARE(t.clips[1].move->to.x, 0.5);
        QCOMPARE(t.clips[1].focusX, 0.3);                  // a zoom keeps the focus crop and pushes in across it
        QCOMPARE(t.clips[1].focusY, 0.4);
        QCOMPARE(t.clips[2].move->ease, Ease::EaseInOut);
        QCOMPARE(t.clips[2].focusX, 0.5);                  // an explicit move replaces focus
        QVERIFY(!t.clips[2].transition);
        QCOMPARE(t.clips[3].kind, ClipKind::Image);        // compose_video scene shape
        QCOMPARE(t.clips[3].src, QStringLiteral("C:/m/c.png"));
        QCOMPARE(*t.clips[3].audio->start, 0.0);
        QCOMPARE(t.audio.voiceover->volume, 0.9);
        QCOMPARE(t.audio.events.value(0).maxSeconds, 2.0);
        QCOMPARE(t.audio.events.value(0).from, 1.0);
        QCOMPARE(*t.audio.music->volume, 0.4);
        QCOMPARE(t.audio.loudness, Loudness::Broadcast);
        QCOMPARE(t.audio.fadeOut, 1.0);
        QVERIFY(t.validate().isEmpty());
        const Plan p = buildPlan(t);
        QVERIFY(p.ok);
        QCOMPARE(graphLine(p, QStringLiteral("[s0][s1]")), QStringLiteral("[s0][s1]xfade=transition=fade:duration=0.8:offset=2[j1]"));
        QCOMPARE(graphLine(p, QStringLiteral("[j1][s2]")), QStringLiteral("[j1][s2]xfade=transition=fadeblack:duration=0.4:offset=4[j2]"));
        QVERIFY(p.segments[0].graph.startsWith(QLatin1String("[0:v]setpts=(PTS-STARTPTS)/1.5,split=2[fa][fb];")));
        QVERIFY(graphLine(p, QStringLiteral("[5:a]")).contains(QLatin1String("atrim=start=1,asetpts=PTS-STARTPTS,atrim=0:2,afade=t=out:st=1.85:d=0.15,volume=0.5,adelay=4000:all=1")));
        QVERIFY2(p.segments[1].graph.contains(QLatin1String("crop=3840:2160:'clip(iw*0.3-ow/2,0,iw-ow)':'clip(ih*0.4-oh/2,0,ih-oh)'")), qPrintable(p.segments[1].graph));
        // round trip: the same model and the same plan, segments included (the zoom clip keeps its focus crop: toJson
        // writes a focus beside a move as move.focus, since a top-level focus next to "move" means "replaced" in ads)
        QString err2;
        const QJsonObject j = t.toJson();
        const Timeline t2 = Timeline::fromJson(j, &err2);
        QVERIFY2(err2.isEmpty(), qPrintable(err2));
        QCOMPARE(t2.toJson(), j);
        const QJsonObject j1 = j[QStringLiteral("clips")].toArray()[1].toObject();
        QVERIFY(!j1.contains(QStringLiteral("focus")));
        QCOMPARE(j1[QStringLiteral("move")].toObject()[QStringLiteral("focus")].toObject(), (QJsonObject{{"x", 0.3}, {"y", 0.4}}));
        QCOMPARE(t2.clips[1].focusX, 0.3);
        QCOMPARE(t2.clips[1].focusY, 0.4);
        const Plan p2 = buildPlan(t2);
        QCOMPARE(p2.join.graph, p.join.graph);
        QCOMPARE(p2.segments.size(), p.segments.size());
        for (qsizetype k = 0; k < p.segments.size(); ++k) QCOMPARE(p2.segments[k].args, p.segments[k].args);
        // a C++-built clip with a non-centred move AND a focus survives too; without a move the focus stays top level
        Timeline c;
        Clip s = imageClip(QStringLiteral("C:/m/s.png"), 2);
        s.focusX = 0.2;
        s.focusY = 0.7;
        s.move = ClipMove{{0.3, 0.3, 1.2}, {0.7, 0.6, 1.0}, Ease::EaseOut};
        Clip f = imageClip(QStringLiteral("C:/m/f.png"), 2);
        f.focusX = 0.8;
        c.clips = {s, f};
        const Timeline c2 = Timeline::fromJson(c.toJson(), &err2);
        QVERIFY2(err2.isEmpty(), qPrintable(err2));
        QCOMPARE(c2.clips[0].focusX, 0.2);
        QCOMPARE(c2.clips[0].focusY, 0.7);
        QCOMPARE(c2.clips[0].move->to.x, 0.7);
        QCOMPARE(c2.clips[1].focusX, 0.8);
        QVERIFY(!c2.clips[1].move);
        QCOMPARE(c2.toJson(), c.toJson());
        QCOMPARE(buildPlan(c2).segments[0].graph, buildPlan(c).segments[0].graph);
        QCOMPARE(c.toJson()[QStringLiteral("clips")].toArray()[1].toObject()[QStringLiteral("focus")].toObject(), (QJsonObject{{"x", 0.8}, {"y", 0.5}}));
    }

    // ================================================================ validation

    void validation() {
        const auto errOf = [](const char* json) {
            QString err;
            Timeline::fromJson(QJsonDocument::fromJson(json).object(), &err);
            return err;
        };
        QCOMPARE(errOf(R"({"scenes":[{"kind":"image","src":"a.png","duration":2,"kenburns":"spin"}]})"), QStringLiteral("scenes[0].kenburns: unknown value \"spin\""));
        QCOMPARE(errOf(R"({"scenes":[{"kind":"color","duration":"2"}]})"), QStringLiteral("scenes[0].duration: not a number"));
        QCOMPARE(errOf(R"({"scenes":[{"kind":"color"}]})"), QStringLiteral("scenes[0].duration: missing"));
        QCOMPARE(errOf(R"({"aspect":"3:2","scenes":[]})"), QStringLiteral("timeline.aspect: unknown value \"3:2\""));
        QCOMPARE(errOf(R"({"clips":[{"kind":"color","duration":1,"transition":{"dur":1}}]})"), QStringLiteral("clips[0].transition.type: missing"));
        QCOMPARE(errOf(R"({"clips":[],"captions":[{"start":1,"text":"x"}]})"), QStringLiteral("captions[0].end: missing"));
        QCOMPARE(errOf(R"({"clips":[],"audio":{"music":{"volume":1}}})"), QStringLiteral("audio.music.path: missing"));
        QCOMPARE(errOf(R"({"clips":[{"kind":"color","duration":1}],"endCard":{"colors":["#fff",3]}})"), QStringLiteral("endCard.colors[1]: not a string"));
        QCOMPARE(errOf(R"({"clips":[{"kind":"color","duration":1,"textPosition":""}]})"), QString());
        // a src without a kind is not quietly a colour card (validate() never looks at a card's src)
        QCOMPARE(errOf(R"({"scenes":[{"src":"C:/a.png","duration":2}]})"), QStringLiteral("scenes[0].kind: missing (a clip with a src is \"image\" or \"video\")"));
        QCOMPARE(errOf(R"({"scenes":[{"color":"#0b3d91","duration":2},{"image":"C:/a.png","duration":1},{"src":"C:/b.mp4","video":"","duration":1}]})"), QString());

        const auto problems = [](const Timeline& t) { return t.validate().join(QStringLiteral(" | ")); };
        Timeline t;
        QCOMPARE(problems(t), QStringLiteral("nothing to render: no clips"));
        t.clips = {imageClip(QStringLiteral("https://example.com/a.png"), 2)};
        QVERIFY2(problems(t).contains(QLatin1String("clips[0].src: only local files are accepted")), qPrintable(problems(t)));
        t.clips = {imageClip(QStringLiteral("data:image/png;base64,AAAA"), 2)};
        QVERIFY(problems(t).contains(QLatin1String("only local files")));
        t.clips = {imageClip(QStringLiteral("file:///C:/a.png"), 2)};
        QVERIFY(problems(t).contains(QLatin1String("only local files")));
        t.clips = {imageClip(QString(), 2)};
        QVERIFY(problems(t).contains(QLatin1String("clips[0].src: no file given")));
        t.clips = {imageClip(QStringLiteral("C:\\media\\a.png"), 2), imageClip(QStringLiteral("D:/b%d.png"), 1), colorClip(1)};
        QCOMPARE(problems(t), QString());
        t.captions = {{2, 1, QStringLiteral("x")}};
        QCOMPARE(problems(t), QStringLiteral("captions[0]: end must be after start."));
        t.captions.clear();
        t.clips[2].duration = 0;
        t.clips[1].speed = 8;
        QCOMPARE(problems(t), QStringLiteral("clips[1].speed: must be 0.25..4 | clips[2].duration: must be > 0"));
        t.clips[1].speed = 1;
        t.clips[2].duration = std::nan("");
        QCOMPARE(problems(t), QStringLiteral("clips[2].duration: must be > 0"));
        t.clips[2].duration = 1;
        t.targetSeconds = Timeline::kMaxSeconds + 1;
        QVERIFY(problems(t).startsWith(QLatin1String("targetSeconds: must be 0.2..")));
        t.targetSeconds.reset();
        t.audio.voiceover = AudioPlacement{QStringLiteral("ftp://x/vo.wav")};
        QVERIFY(problems(t).contains(QLatin1String("audio.voiceover.path: only local files")));
        // absolute local paths only: ffmpeg protocol syntax, stdin, relative names, UNC / device paths are refused
        // wherever a path goes to -i unprobed (sound, clips, the music bed, the logo)
        const auto pathVerdict = [&](const QString& path) {
            Timeline v;
            v.clips = {colorClip(1)};
            v.audio.voiceover = AudioPlacement{path};
            return v.validate().join(QStringLiteral(" | "));
        };
        for (const char* bad : {"subfile,,start,0,end,9,,:http://host/x.wav", "crypto+http://host/x.wav", "concat:C:/a.wav|C:/b.wav",
                                "\\\\host\\share\\vo.wav", "//host/share/vo.wav", "\\\\?\\C:\\vo.wav", "\\\\.\\pipe\\vo",
                                "C:/media/nul.wav", "C:\\media\\CON", "D:/x/com1.mp3", "C:/x/aux .wav", "C:/x/conout$"})
            QVERIFY2(pathVerdict(QLatin1String(bad)).startsWith(QLatin1String("audio.voiceover.path: only local files are accepted")),
                     qPrintable(QLatin1String(bad) + QStringLiteral(" -> ") + pathVerdict(QLatin1String(bad))));
        for (const char* rel : {"-", "vo.wav", "./vo.wav", "..\\vo.wav", "C:vo.wav", " C:/vo.wav"})
            QVERIFY2(pathVerdict(QLatin1String(rel)).startsWith(QLatin1String("audio.voiceover.path: needs an absolute local path")),
                     qPrintable(QLatin1String(rel) + QStringLiteral(" -> ") + pathVerdict(QLatin1String(rel))));
        for (const char* ok : {"C:\\media\\vo.wav", "D:/b%d.wav", "C:/media/nullable.wav", "C:/media/console.wav", "C:/con-ed/vo.wav",
                               "E:/a,b:c.wav"})
            QVERIFY2(pathVerdict(QLatin1String(ok)).isEmpty(), qPrintable(QLatin1String(ok) + QStringLiteral(" -> ") + pathVerdict(QLatin1String(ok))));
#ifdef Q_OS_WIN
        // a rooted path without a drive is on the current drive: the Studio's and ffmpeg's (cwd = work dir) can differ
        for (const char* rooted : {"/srv/media/vo.wav", "\\media\\vo.wav"})
            QCOMPARE(pathVerdict(QLatin1String(rooted)),
                     QStringLiteral("audio.voiceover.path: needs an absolute local path with a drive letter (got \"%1\")").arg(QLatin1String(rooted)));
#else
        QVERIFY(pathVerdict(QStringLiteral("/srv/media/vo.wav")).isEmpty());
#endif
        t.audio.voiceover.reset();
        t.clips[0].src = QStringLiteral("-");
        t.clips[1].kind = ClipKind::Video;
        t.clips[1].src = QStringLiteral("subfile,,start,0,end,9,,:http://h/x.mp4");
        t.clips[1].fallbackImage = QStringLiteral("still.png");
        t.audio.music = MusicBed{QStringLiteral("\\\\host\\bed.mp3")};
        t.endCard = EndCard{};
        t.endCard->logo = QStringLiteral("logo.png");
        QCOMPARE(problems(t), QStringLiteral("clips[0].src: needs an absolute local path (got \"-\") | "
                                             "clips[1].src: only local files are accepted (got \"subfile,,start,0,end,9,,:http://h/x.mp4\") | "
                                             "clips[1].fallbackImage: needs an absolute local path (got \"still.png\") | "
                                             "audio.music.path: only local files are accepted (got \"\\\\host\\bed.mp3\") | "
                                             "endCard.logo: needs an absolute local path (got \"logo.png\")"));
        // the builder refuses an empty timeline the TS way
        const Plan p = buildPlan(Timeline());
        QVERIFY(!p.ok);
        QCOMPARE(p.error, QStringLiteral("nothing to render: no scenes"));
        QVERIFY(p.steps().isEmpty());

        // ... and a join too big for one ffmpeg (inputs open at once, CreateProcess' 32,767 characters): split it
        Timeline big;
        for (int i = 0; i < 200; ++i) big.clips << colorClip(0.5);
        QVERIFY(big.validate().isEmpty());
        QVERIFY(buildPlan(big).ok);                                   // 200 inputs, ~9 k characters
        for (int i = 0; i < 100; ++i) big.clips << colorClip(0.5);
        Plan bp = buildPlan(big);
        QVERIFY(!bp.ok);
        QVERIFY2(bp.error.startsWith(QLatin1String("too many inputs for one join (300 files, ")), qPrintable(bp.error));
        QVERIFY(bp.error.endsWith(QLatin1String("at most 256 and 30000): split the timeline into shorter parts")));
        QVERIFY(bp.steps().isEmpty());
        Timeline longPaths;                                           // 100 clips with a narration line each: 200 inputs, but long paths
        for (int i = 0; i < 100; ++i) {
            longPaths.clips << colorClip(1);
            longPaths.clips.last().audio = AudioPlacement{QStringLiteral("C:/") + QString(300, u'n') + QStringLiteral("/line%1.wav").arg(i)};
        }
        QVERIFY(longPaths.validate().isEmpty());
        bp = buildPlan(longPaths);
        QVERIFY2(!bp.ok && bp.error.startsWith(QLatin1String("too many inputs for one join (200 files, ")), qPrintable(bp.error));
        QVERIFY(bp.segments.isEmpty());
    }

    // ================================================================ ST-18b: loudness targets

    void loudnessModel() {
        // names, aliases and presets (social / broadcast are compose.ts's strings)
        QCOMPARE(parseLoudness(u"web"), Loudness::Social);
        QCOMPARE(parseLoudness(u"atsc"), Loudness::Broadcast);
        QCOMPARE(parseLoudness(u"ebu"), Loudness::R128);
        QCOMPARE(parseLoudness(u"r128"), Loudness::R128);
        QVERIFY(!parseLoudness(u"custom"));                         // a custom target is an object with its numbers
        QCOMPARE(loudnessName(Loudness::R128), QStringLiteral("r128"));
        QCOMPARE(loudnormFilter(loudnessPreset(Loudness::Social)), QStringLiteral("loudnorm=I=-16:TP=-1.5:LRA=11"));
        QCOMPARE(loudnormFilter(loudnessPreset(Loudness::Broadcast)), QStringLiteral("loudnorm=I=-24:TP=-2:LRA=7"));
        QCOMPARE(loudnormFilter(loudnessPreset(Loudness::R128)), QStringLiteral("loudnorm=I=-23:TP=-1:LRA=20"));
        QCOMPARE(loudnormMeasureFilter(loudnessPreset(Loudness::R128)), QStringLiteral("loudnorm=I=-23:TP=-1:LRA=20:print_format=json"));

        // JSON: a name or a custom target; custom round-trips; validation of the ranges
        QString err;
        Timeline t = Timeline::fromJson(QJsonDocument::fromJson(R"({"clips":[{"kind":"color","duration":1}],
            "audio":{"loudness":{"i":-20.5,"tp":-2}}})").object(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(t.audio.loudness, Loudness::Custom);
        QVERIFY(t.audio.loudnessTarget() == (LoudnessTarget{-20.5, -2, 20}));   // lra defaults to R128's
        QVERIFY(t.validate().isEmpty());
        const Timeline back = Timeline::fromJson(t.toJson(), &err);
        QVERIFY(err.isEmpty() && back.audio.loudness == Loudness::Custom && back.audio.loudnessTarget() == t.audio.loudnessTarget());
        t = Timeline::fromJson(QJsonDocument::fromJson(R"({"clips":[{"kind":"color","duration":1}],"loudness":"ebu"})").object(), &err);
        QVERIFY(err.isEmpty() && t.audio.loudness == Loudness::R128);
        QCOMPARE(t.toJson().value(QStringLiteral("audio")).toObject().value(QStringLiteral("loudness")).toString(), QStringLiteral("r128"));
        Timeline::fromJson(QJsonDocument::fromJson(R"({"clips":[{"kind":"color","duration":1}],"loudness":{"tp":-1}})").object(), &err);
        QCOMPARE(err, QStringLiteral("timeline.loudness.i: missing"));
        Timeline::fromJson(QJsonDocument::fromJson(R"({"clips":[{"kind":"color","duration":1}],"loudness":"loud"})").object(), &err);
        QCOMPARE(err, QStringLiteral("timeline.loudness: unknown value \"loud\""));
        t.audio.loudness = Loudness::Custom;
        t.audio.target = LoudnessTarget{-3, -1, 7};
        QCOMPARE(t.validate(), QStringList{QStringLiteral("audio.loudness: integrated loudness must be -70..-5 LUFS")});
        t.audio.target = LoudnessTarget{-23, 1, 7};
        QCOMPARE(t.validate(), QStringList{QStringLiteral("audio.loudness: true peak must be -9..0 dBTP")});
        t.audio.target.reset();
        QCOMPARE(t.validate(), QStringList{QStringLiteral("audio.loudness: a custom target needs its numbers {i, tp, lra}")});

        // the analysis as ffmpeg 9 prints it, and the second pass built from it
        const LoudnessMeasure m = parseLoudnormLog(loudnormLog(QStringLiteral("-27.50")));
        QVERIFY2(m.ok, qPrintable(m.error));
        QCOMPARE(m.i, -27.5);
        QCOMPARE(m.tp, -6.1);
        QCOMPARE(m.lra, 3.2);
        QCOMPARE(m.thresh, -37.8);
        QCOMPARE(m.offset, -0.05);
        QCOMPARE(loudnormSecondPass(loudnessPreset(Loudness::R128), m),
                 QStringLiteral("loudnorm=I=-23:TP=-1:LRA=20:measured_I=-27.5:measured_TP=-6.1:measured_LRA=3.2:measured_thresh=-37.8:offset=-0.05:linear=true"));
        const LoudnessMeasure silent = parseLoudnormLog(loudnormLog(QStringLiteral("-inf"), QStringLiteral("-inf"), QStringLiteral("0.00")));
        QVERIFY(silent.ok && std::isinf(silent.i) && silent.i < 0);
        QCOMPARE(loudnormSecondPass(loudnessPreset(Loudness::Social), silent), QStringLiteral("anull"));   // silence stays silence
        QVERIFY(silent.toJson().value(QStringLiteral("i")).isNull());
        const LoudnessMeasure none = parseLoudnormLog(u"[out#0/null] video:0KiB audio:3000KiB");
        QVERIFY(!none.ok);
        QCOMPARE(none.error, QStringLiteral("no loudness measurement in the ffmpeg log"));
        QCOMPARE(loudnormSecondPass(loudnessPreset(Loudness::Social), none), QStringLiteral("loudnorm=I=-16:TP=-1.5:LRA=11"));
        // a huge measured value is clamped into loudnorm's option range
        const LoudnessMeasure loud = parseLoudnormLog(loudnormLog(QStringLiteral("3.00"), QStringLiteral("120.0")));
        QVERIFY(loudnormSecondPass(loudnessPreset(Loudness::Social), loud).contains(QLatin1String("measured_I=0:measured_TP=99:")));
    }

    void twoPassPlan() {
        Timeline t;
        t.aspect = Aspect::Landscape;
        t.quality = Quality::Draft;
        t.clips = {colorClip(1.5), colorClip(1.5, QStringLiteral("#ff8a3d"))};
        t.audio.voiceover = AudioPlacement{QStringLiteral("C:/v/vo.wav"), 0.2};
        t.audio.music = MusicBed{QStringLiteral("C:/v/bed.wav"), 0.3, true};
        t.audio.fadeIn = 0.2;
        t.audio.fadeOut = 0.5;
        t.audio.loudness = Loudness::R128;
        PlanOptions o;
        o.loudnessPasses = 2;
        const Plan p = buildPlan(t, o);
        QVERIFY2(p.ok, qPrintable(p.error));
        QVERIFY(p.hasMix() && p.twoPass() && !p.split());
        QCOMPARE(p.loudnessPasses, 2);
        QVERIFY(p.loudnessOn);
        QVERIFY(p.loudness == loudnessPreset(Loudness::R128));
        // the mix: the sound alone, inputs from 0, no loudnorm, the master fades in before the measurement
        QCOMPARE(inputsOf(p.mix.args), (QStringList{QStringLiteral("C:/v/vo.wav"), QStringLiteral("C:/v/bed.wav")}));
        QCOMPARE(p.mix.args.mid(p.mix.args.indexOf(QStringLiteral("-/filter_complex"))),
                 (QStringList{"-/filter_complex", "mix.txt", "-map", "[aout]", "-c:a", "pcm_f32le", "-ar", "48000", "-ac", "2", "-rf64", "auto", "-t", "3", "mix.wav"}));
        QVERIFY(p.mix.graph.startsWith(QLatin1String("[0:a]aresample=48000")));
        QVERIFY(!p.mix.graph.contains(QLatin1String("loudnorm")));
        QVERIFY(p.mix.graph.endsWith(QLatin1String("[mix]apad,atrim=0:3,aresample=48000,asetpts=N/SR/TB,apad=whole_dur=3,atrim=0:3,asetpts=PTS-STARTPTS,"
                                                   "afade=t=in:d=0.2,afade=t=out:st=2.5:d=0.5[aout]")));
        // the analysis, then the join reads mix.wav and carries the token for the measured second pass
        QCOMPARE(p.measure.args, (QStringList{"-protocol_whitelist", "file", "-i", "mix.wav", "-af", "loudnorm=I=-23:TP=-1:LRA=20:print_format=json",
                                              "-f", "null", "-"}));
        QCOMPARE(p.measure.logLevel, QStringLiteral("info"));
        QCOMPARE(inputsOf(p.join.args), (QStringList{QStringLiteral("seg000.mp4"), QStringLiteral("seg001.mp4"), QStringLiteral("mix.wav")}));
        QVERIFY(p.join.loudnessToken);
        QVERIFY(p.join.graph.endsWith(QLatin1String("[2:a]{LOUDNORM},aresample=48000,asetpts=N/SR/TB,apad=whole_dur=3,atrim=0:3,asetpts=PTS-STARTPTS[aout]")));
        QVERIFY(!p.join.graph.contains(QLatin1String("[vo0]")) && !p.join.graph.contains(QLatin1String("amix")));
        QVERIFY(!p.join.graph.contains(QLatin1String("afade")));    // the fades are in mix.wav already
        const QVector<const Step*> steps = p.steps();
        QCOMPARE(steps.size(), 6);
        QCOMPARE(steps[0], &p.mix);
        QCOMPARE(steps[1], &p.measure);
        QCOMPARE(steps[4], &p.join);
        // one pass (the default) is compose.ts's plan; loudness off or no sound never takes two passes
        const Plan one = buildPlan(t);
        QVERIFY(!one.hasMix() && !one.twoPass() && one.join.graph.contains(QLatin1String("loudnorm=I=-23:TP=-1:LRA=20,aresample=48000")));
        t.audio.loudness = Loudness::Off;
        const Plan off = buildPlan(t, o);
        QVERIFY(!off.hasMix() && !off.twoPass() && !off.loudnessOn);
        QCOMPARE(off.join.args, buildPlan(t).join.args);
        t.audio = AudioPlan();
        const Plan quiet = buildPlan(t, o);
        QVERIFY(!quiet.hasMix() && !quiet.loudnessOn);
        QVERIFY(quiet.join.graph.contains(QLatin1String("anullsrc=r=48000:cl=stereo,atrim=0:3[mix]")));
    }

    // ================================================================ ST-18b: segment-parallel join

    void partPlan() {
        // 10 one-second cards, transitions into slots 1, 3, 5, 7, 9; an end card; held 0.5 s past the slots
        Timeline t;
        t.aspect = Aspect::Landscape;
        t.quality = Quality::Draft;
        t.transitionSeconds = 0.3;
        const QVector<Transition> tr{Transition::Fade, Transition::Cut, Transition::Dissolve, Transition::Cut, Transition::Wipe,
                                     Transition::Cut, Transition::Fade, Transition::Cut, Transition::Slide, Transition::Cut};
        for (int i = 0; i < 10; ++i) {
            Clip c = colorClip(1.0, i % 2 ? QStringLiteral("#203040") : QStringLiteral("#f0a020"));
            c.transition = tr[i];
            t.clips << c;
        }
        EndCard ec;
        ec.brand = QStringLiteral("Star News");
        ec.duration = 1;
        t.endCard = ec;
        t.targetSeconds = 11.5;
        t.captions = cuesForLine(u"A long caption line that runs across the whole timeline and several parts of it, from start to end.", 0.2, 10.5);
        t.audio.voiceover = AudioPlacement{QStringLiteral("C:/v/vo.wav"), 0.0};
        t.audio.music = MusicBed{QStringLiteral("C:/v/bed.wav"), std::nullopt, true};
        PlanOptions o;
        o.partSeconds = 3;
        const Plan p = buildPlan(t, o);
        QVERIFY2(p.ok, qPrintable(p.error));
        QCOMPARE(p.frames, qint64(345));                            // ceil(11.5 * 30)
        QVERIFY(p.split() && p.hasMix() && !p.twoPass());
        // windows end where the transition into a slot is over: 3.3 s (99), 6 s (180), 9.3 s (279)
        QCOMPARE(p.parts.size(), 4);
        const QVector<std::array<qint64, 4>> want{{0, 99, 0, 3}, {99, 81, 3, 5}, {180, 99, 6, 9}, {279, 66, 9, 10}};
        qint64 sum = 0;
        for (int k = 0; k < 4; ++k) {
            const Step& s = p.parts[k];
            QCOMPARE(s.kind, Step::Kind::Part);
            QCOMPARE((std::array<qint64, 4>{s.frameStart, s.frames, s.first, s.last}), want[k]);
            sum += s.frames;
            QCOMPARE(s.output, QStringLiteral("part%1.mp4").arg(k, 3, 10, QLatin1Char('0')));
            QCOMPARE(argAfter(s.args, QStringLiteral("-frames:v")), QString::number(s.frames));
            QVERIFY(s.args.contains(QStringLiteral("-an")));
            QCOMPARE(argAfter(s.args, QStringLiteral("-preset")), QStringLiteral("veryfast"));   // the join's encoder settings
            QCOMPARE(argAfter(s.args, QStringLiteral("-g")), QStringLiteral("60"));
            QStringList ins;
            for (int i = s.first; i <= s.last; ++i) ins << QStringLiteral("seg%1.mp4").arg(i, 3, 10, QLatin1Char('0'));
            QCOMPARE(inputsOf(s.args), ins);
        }
        QCOMPARE(sum, p.frames);
        // part 2 (slots 3-5): skips the 0.3 s blend into slot 3 (part 1 drew it), a cut, then a wipe at 2 s local time
        const Step& p1 = p.parts[1];
        QVERIFY(p1.graph.contains(QLatin1String("[s0][s1]concat=n=2:v=1:a=0,settb=AVTB[j1]")));
        QVERIFY(p1.graph.contains(QLatin1String("[j1][s2]xfade=transition=wipeleft:duration=0.3:offset=2[j2]")));
        QVERIFY2(p1.graph.contains(QLatin1String("[j2]tpad=stop_mode=clone:stop_duration=2,trim=start_frame=9:end_frame=90,setpts=(N+99)/(30*TB),")),
                 qPrintable(p1.graph));
        QVERIFY(p1.graph.endsWith(QLatin1String("setpts=PTS-STARTPTS,format=yuv420p[vout]")));
        // part 1 (slots 0-3) blends INTO slot 3 and stops at the window end
        QVERIFY(p.parts[0].graph.contains(QLatin1String("[j2][s3]xfade=transition=dissolve:duration=0.3:offset=3[j3]")));
        QVERIFY(p.parts[0].graph.contains(QLatin1String("trim=start_frame=0:end_frame=99,setpts=(N+0)/(30*TB)")));
        QVERIFY(p.parts[3].graph.contains(QLatin1String("trim=start_frame=9:end_frame=75,setpts=(N+279)/(30*TB)")));
        // each caption is drawn in exactly the parts whose window it shows in, on absolute times
        for (const CaptionCue& cue : std::as_const(t.captions)) {
            const QString en = QStringLiteral("enable='between(t,%1,%2)'").arg(num(r3(cue.start)), num(r3(std::min(cue.end, p.total))));
            for (const Step& s : std::as_const(p.parts)) {
                const bool shows = cue.start < double(s.frameStart + s.frames) / 30 && std::min(cue.end, p.total) > double(s.frameStart) / 30;
                QVERIFY2(s.graph.contains(en) == shows, qPrintable(QStringLiteral("part %1, cue %2").arg(s.slot).arg(cue.text)));
            }
        }
        // the parts' text files are numbered after the segments' and never collide
        QSet<QString> names;
        for (const Step* s : p.steps())
            for (const TextFile& f : s->texts) { QVERIFY2(!names.contains(f.name), qPrintable(f.name)); names.insert(f.name); }
        // the sound for the whole programme (one loudness pass), then the concat by stream copy
        QVERIFY(p.mix.graph.contains(QLatin1String("loudnorm=I=-16:TP=-1.5:LRA=11")));
        QCOMPARE(argAfter(p.mix.args, QStringLiteral("-t")), QStringLiteral("11.5"));
        QCOMPARE(p.join.kind, Step::Kind::Concat);
        QCOMPARE(p.join.texts.size(), 1);
        QCOMPARE(p.join.texts[0].name, QStringLiteral("parts.txt"));
        QCOMPARE(p.join.texts[0].text, QStringLiteral("ffconcat version 1.0\nfile 'part000.mp4'\nfile 'part001.mp4'\nfile 'part002.mp4'\nfile 'part003.mp4'\n"));
        QCOMPARE(p.join.args, (QStringList{"-f", "concat", "-protocol_whitelist", "file", "-i", "parts.txt", "-protocol_whitelist", "file", "-i", "mix.wav",
                                           "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2",
                                           "-movflags", "+faststart", "-t", "11.5", "out.mp4"}));
        const QVector<const Step*> steps = p.steps();
        QCOMPARE(steps.size(), 1 + 11 + 4 + 2);
        QCOMPARE(steps.first(), &p.mix);
        QCOMPARE(steps[12], &p.parts[0]);
        // two passes: the concat applies the measured second pass to mix.wav
        o.loudnessPasses = 2;
        const Plan p2 = buildPlan(t, o);
        QVERIFY(p2.split() && p2.twoPass());
        QVERIFY(!p2.mix.graph.contains(QLatin1String("loudnorm")));
        QCOMPARE(p2.join.graphFile, QStringLiteral("mux.txt"));
        QCOMPARE(p2.join.graph, QStringLiteral("[1:a]{LOUDNORM},aresample=48000,asetpts=N/SR/TB,apad=whole_dur=11.5,atrim=0:11.5,asetpts=PTS-STARTPTS[aout]"));
        QVERIFY(p2.join.args.join(u' ').contains(QLatin1String("-/filter_complex mux.txt -map 0:v -map [aout] -c:v copy")));
        // nothing to cut (shorter than 1.5 windows): one join, the same as without parts
        Timeline shortT = t;
        shortT.clips.resize(2);
        shortT.endCard.reset();
        shortT.targetSeconds.reset();
        PlanOptions o2;
        o2.partSeconds = 3;
        const Plan single = buildPlan(shortT, o2);
        QVERIFY(!single.split() && !single.hasMix());
        QCOMPARE(single.join.args, buildPlan(shortT).join.args);
        // a mix with too many sounds for one ffmpeg is refused with a way out
        Timeline busy = t;
        for (int i = 0; i < 300; ++i) busy.audio.events << AudioPlacement{QStringLiteral("C:/v/e%1.wav").arg(i), i * 0.03};
        const Plan refused = buildPlan(busy, o);
        QVERIFY(!refused.ok && refused.parts.isEmpty() && refused.segments.isEmpty());
        QVERIFY2(refused.error.startsWith(QLatin1String("too many sound inputs for one mix (302 files, ")), qPrintable(refused.error));
    }

    // ================================================================ ST-18b: SRT / VTT

    void captionsGolden() {
        const QJsonObject g = goldenCaptions();
        QVERIFY2(!g.isEmpty(), "captions-golden.json missing or unreadable");
        QVERIFY(g.value(QStringLiteral("source")).toString().startsWith(QLatin1String("src/media.ts")));
        int n = 0;
        for (const QJsonValue& v : g.value(QStringLiteral("times")).toArray()) {
            const QJsonObject o = v.toObject();
            const double t = goldenTime(o.value(QStringLiteral("t")));
            QCOMPARE(subtitleTime(t, u','), o.value(QStringLiteral("srt")).toString());
            QCOMPARE(subtitleTime(t, u'.'), o.value(QStringLiteral("vtt")).toString());
            ++n;
        }
        QVERIFY(n >= 30);
        int cases = 0;
        for (const QJsonValue& v : g.value(QStringLiteral("cases")).toArray()) {
            const QJsonObject o = v.toObject();
            QVector<CaptionCue> cues;
            for (const QJsonValue& c : o.value(QStringLiteral("cues")).toArray())
                cues.push_back({goldenTime(c[QStringLiteral("start")]), goldenTime(c[QStringLiteral("end")]), c[QStringLiteral("text")].toString()});
            const QString name = o.value(QStringLiteral("name")).toString();
            const QString srt = toSrt(cues), vtt = toVtt(cues);
            QVERIFY2(srt == o.value(QStringLiteral("srt")).toString(), qPrintable(name + u'\n' + textDiff(srt, o.value(QStringLiteral("srt")).toString())));
            QVERIFY2(vtt == o.value(QStringLiteral("vtt")).toString(), qPrintable(name + u'\n' + textDiff(vtt, o.value(QStringLiteral("vtt")).toString())));
            ++cases;
        }
        QCOMPARE(cases, 9);
        // the TS quirks are kept on purpose (one caption file for both engines)
        QCOMPARE(subtitleTime(2.3, u','), QStringLiteral("00:00:02,299"));
        QCOMPARE(toVtt({}), QStringLiteral("WEBVTT\n\n"));
        QCOMPARE(toSrt({}), QString());
    }

    void sidecars() {
        Timeline t;
        t.clips = {colorClip(2), colorClip(2)};
        t.targetSeconds = 3.5;
        t.captions = {
            {2.0, 2.0, QStringLiteral("zero length")},              // never drawn
            {1.0, 2.5, QStringLiteral("second\n\n  line   two ")},
            {0.2, 1.0, QStringLiteral("first --> arrow \x01 cue")},
            {3.0, 9.0, QStringLiteral("clipped at the end \U0001F389")},
            {2.5, 3.0, QStringLiteral("\U0001F389\U0001F389")},     // emoji only: not drawn, not listed
            {3.5, 4.0, QStringLiteral("after the end")},
        };
        const QVector<CaptionCue> cues = sidecarCues(t);
        QCOMPARE(cues.size(), 3);
        QCOMPARE(cues[0].start, 0.2);
        QCOMPARE(cues[0].text, QStringLiteral("first -> arrow cue"));
        QCOMPARE(cues[1].text, QStringLiteral("second\nline two"));
        QCOMPARE(cues[2].end, 3.5);
        QCOMPARE(cues[2].text, QStringLiteral("clipped at the end \U0001F389"));
        QCOMPARE(toSrt(cues), QStringLiteral("1\n00:00:00,200 --> 00:00:01,000\nfirst -> arrow cue\n\n2\n00:00:01,000 --> 00:00:02,500\nsecond\nline two\n\n"
                                             "3\n00:00:03,000 --> 00:00:03,500\nclipped at the end \U0001F389\n"));
        // the same cues the composer burns in: one drawtext block per listed cue
        const Plan p = buildPlan(t);
        for (const CaptionCue& c : cues)
            QVERIFY(p.join.graph.contains(QStringLiteral("enable='between(t,%1,%2)'").arg(num(r3(c.start)), num(r3(c.end)))));
        QVERIFY(!p.join.graph.contains(QLatin1String("between(t,2.5,3)")));
        // a sequence's captions on the film's timeline
        const QVector<CaptionCue> moved = shiftCues(cues, 10, 10.5, 13.2);
        QCOMPARE(moved.size(), 3);
        QCOMPARE(moved[0].start, 10.5);                             // clipped at the window start
        QCOMPARE(moved[1].end, 12.5);
        QCOMPARE(moved[2].end, 13.2);                               // and at its end
        QCOMPARE(shiftCues(cues, 10, 10.5, 13).size(), 2);          // the third would be empty
        QCOMPARE(shiftCues(cues, -5).size(), 0);
        // written atomically as UTF-8; a folder in the way is an error, not a crash
        const QString dir = m_tmp.filePath(QStringLiteral("sidecars"));
        QString err;
        QVERIFY2(writeSidecars(cues, dir + QStringLiteral("/a.srt"), dir + QStringLiteral("/a.vtt"), &err), qPrintable(err));
        QFile s(dir + QStringLiteral("/a.srt")), v(dir + QStringLiteral("/a.vtt"));
        QVERIFY(s.open(QIODevice::ReadOnly) && v.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(s.readAll()), toSrt(cues));
        QCOMPARE(QString::fromUtf8(v.readAll()), toVtt(cues));
        QVERIFY(QDir().mkpath(dir + QStringLiteral("/b.srt")));
        QVERIFY(!writeSidecars(cues, dir + QStringLiteral("/b.srt"), QString(), &err));
        QVERIFY2(err.contains(QLatin1String("b.srt")), qPrintable(err));
    }

    // ================================================================ ST-18b: the renderer's step graph (no ffmpeg)

    void rendererScheduling() {
        Timeline t;
        t.aspect = Aspect::Landscape;
        t.quality = Quality::Draft;
        t.transitionSeconds = 0.3;
        const QVector<Transition> tr{Transition::Fade, Transition::Cut, Transition::Dissolve, Transition::Cut, Transition::Wipe,
                                     Transition::Cut, Transition::Fade, Transition::Cut, Transition::Slide, Transition::Cut};
        for (int i = 0; i < 10; ++i) {
            Clip c = colorClip(1.0);
            c.transition = tr[i];
            t.clips << c;
        }
        EndCard ec;
        ec.brand = QStringLiteral("Star News");
        ec.duration = 1;
        t.endCard = ec;
        t.targetSeconds = 11.5;
        t.captions = cuesForLine(u"Captions go to the picture and to the sidecar files.", 0.5, 6);
        t.audio.voiceover = AudioPlacement{m_tmp.filePath(QStringLiteral("fake/vo.wav")), 0.0};
        t.audio.music = MusicBed{m_tmp.filePath(QStringLiteral("fake/bed.wav")), 0.3, true};
        t.audio.loudness = Loudness::R128;
        const auto readable = [](const QString&) { return SourceInfo{true, true}; };

        // a split render with two loudness passes: dependencies, lane width, the measured pass reaching the concat
        auto fake = std::make_shared<FakeBackend>();
        Renderer r(fake);
        RenderOptions o;
        o.outFile = m_tmp.filePath(QStringLiteral("fake/out/spot.mp4"));
        o.workDir = m_tmp.filePath(QStringLiteral("fake/work"));
        o.lanes = 3;
        o.partSeconds = 3;
        o.probe = readable;
        QVector<QPair<double, QString>> prog;
        RenderResult res = renderNow(r, t, o, &prog);
        QVERIFY2(res.ok, qPrintable(res.error));
        QCOMPARE(res.parts, 4);
        QCOMPARE(res.steps, 1 + 1 + 11 + 4 + 1 + 1 + 1);            // mix, loudness, scenes, parts, concat, poster, check
        QCOMPARE(fake->jobs.size(), res.steps);
        QCOMPARE(fake->peak, 3);
        QCOMPARE(res.peakParallel, 3);
        QCOMPARE(res.lanes, 3);
        QVERIFY(!r.isRunning());
        // order: a step starts only after everything it reads has ended ("+x" / "-x" in time order)
        const QStringList& ev = fake->events;
        const auto after = [&ev](const QString& dep, const QString& step) {
            const qsizetype e = ev.indexOf(u'-' + dep), s = ev.indexOf(u'+' + step);
            return e >= 0 && s >= 0 && e < s;
        };
        QVERIFY(after(QStringLiteral("mix"), QStringLiteral("loudness")));
        const QVector<QPair<int, int>> partScenes{{1, 4}, {4, 6}, {7, 10}, {10, 11}};   // slots 0-3, 3-5, 6-9, 9-10
        for (int k = 0; k < 4; ++k) {
            const QString part = QStringLiteral("part %1 of 4").arg(k + 1);
            for (int s = partScenes[k].first; s <= partScenes[k].second; ++s) {
                const QString scene = s == 11 ? QStringLiteral("end card") : QStringLiteral("scene %1").arg(s);
                QVERIFY2(after(scene, part), qPrintable(scene + QStringLiteral(" -> ") + part + u'\n' + ev.join(u' ')));
            }
            QVERIFY(after(part, QStringLiteral("concat")));
        }
        QVERIFY(after(QStringLiteral("mix"), QStringLiteral("concat")) && after(QStringLiteral("loudness"), QStringLiteral("concat")));
        QVERIFY(after(QStringLiteral("concat"), QStringLiteral("poster")) && after(QStringLiteral("poster"), QStringLiteral("loudness check")));
        QCOMPARE(ev.mid(ev.size() - 6), (QStringList{"+concat", "-concat", "+poster", "-poster", "+loudness check", "-loudness check"}));
        // the second pass, measured from the mix, is what the concat's graph says
        bool sawPass = false;
        for (const QString& g : std::as_const(fake->graphsSeen)) {
            QVERIFY2(!g.contains(QLatin1String("{LOUDNORM}")), qPrintable(g));
            if (g.contains(QLatin1String("measured_I=-27.5")) && g.contains(QLatin1String("linear=true"))) sawPass = true;
        }
        QVERIFY(sawPass);
        QCOMPARE(res.loudness.passes, 2);
        QCOMPARE(res.loudness.mix.i, -27.5);
        QCOMPARE(res.loudness.output.i, -23.1);
        QVERIFY(res.loudness.toJson().value(QStringLiteral("output")).toObject().value(QStringLiteral("i")).toDouble() == -23.1);
        // jobs: loudness analysis at info level without progress; renders with progress; generous timeouts
        for (const RenderJob& j : std::as_const(fake->jobs)) {
            const bool measure = j.args.join(u' ').contains(QLatin1String("print_format=json"));
            QCOMPARE(j.logLevel, measure ? QStringLiteral("info") : QStringLiteral("error"));
            QCOMPARE(j.progress, !measure && j.label != u"poster");
            QVERIFY(j.timeoutMs >= 600000);
            QCOMPARE(QDir::cleanPath(j.cwd), QDir::cleanPath(o.workDir));
        }
        // the output was moved into place; the sidecars sit next to it; the work files stay (a caller's folder)
        QVERIFY(QFileInfo::exists(o.outFile));
        QVERIFY(!QFileInfo::exists(o.workDir + QStringLiteral("/out.mp4")));
        QCOMPARE(res.file, o.outFile);
        QCOMPARE(res.poster, m_tmp.filePath(QStringLiteral("fake/out/spot.png")));
        QCOMPARE(res.srt, m_tmp.filePath(QStringLiteral("fake/out/spot.srt")));
        QFile srt(res.srt);
        QVERIFY(srt.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(srt.readAll()), toSrt(sidecarCues(t)));
        QFile parts(o.workDir + QStringLiteral("/parts.txt"));
        QVERIFY(parts.open(QIODevice::ReadOnly) && parts.readAll().startsWith("ffconcat version 1.0\nfile 'part000.mp4'"));
        QCOMPARE(QDir::cleanPath(res.workDir), QDir::cleanPath(o.workDir));   // a caller's folder is left in place
        QCOMPARE(res.duration, 11.5);                              // the fake file is unreadable: the plan's length
        QCOMPARE(res.width, 640);
        // progress: from "planning" at 0 to "done" at 1, never backwards
        QVERIFY(prog.size() >= 5);
        QCOMPARE(prog.first().second, QStringLiteral("planning"));
        QCOMPARE(prog.last(), (QPair<double, QString>{1.0, QStringLiteral("done")}));
        bool scenesMsg = false;
        for (qsizetype i = 1; i < prog.size(); ++i) {
            QVERIFY2(prog[i].first >= prog[i - 1].first, qPrintable(QStringLiteral("%1 then %2").arg(prog[i - 1].first).arg(prog[i].first)));
            scenesMsg = scenesMsg || prog[i].second.startsWith(QLatin1String("rendering scenes ("));
        }
        QVERIFY(scenesMsg);
        const QJsonObject rj = res.toJson();
        QVERIFY(rj.value(QStringLiteral("ok")).toBool() && rj.value(QStringLiteral("timeline")).toArray().size() == 11);

        // a failing scene stops the render: no join, no deliverable, one finished()
        auto fail = std::make_shared<FakeBackend>();
        fail->failLabel = QStringLiteral("scene 3");
        Renderer r2(fail);
        RenderOptions o2 = o;
        o2.outFile = m_tmp.filePath(QStringLiteral("fake/out2/spot.mp4"));
        o2.workDir.clear();                                         // a temp folder of its own
        o2.lanes = 2;
        o2.partSeconds = -1;
        const auto tempFolders = [] { return QDir::temp().entryList({QStringLiteral("studio-render-*")}, QDir::Dirs).size(); };
        const qsizetype foldersBefore = tempFolders();
        res = renderNow(r2, t, o2);
        QTRY_COMPARE(tempFolders(), foldersBefore);                // its temp folder is removed (on the pool)
        QVERIFY(res.workDir.isEmpty());
        QVERIFY(!res.ok && !res.cancelled);
        QVERIFY2(res.error.startsWith(QLatin1String("scene 3: ffmpeg failed (exit 1): ")) && res.error.contains(QLatin1String("Invalid argument")),
                 qPrintable(res.error));
        QVERIFY(!fail->started.contains(QStringLiteral("join")));
        QVERIFY(!QFileInfo::exists(o2.outFile));
        QVERIFY(res.file.isEmpty() && res.srt.isEmpty());

        // keepWork leaves the temp folder and says where it is
        auto keep = std::make_shared<FakeBackend>();
        Renderer r5(keep);
        RenderOptions k = o2;
        k.keepWork = true;
        k.outFile = m_tmp.filePath(QStringLiteral("fake/out3/spot.mp4"));
        res = renderNow(r5, t, k);
        QVERIFY2(res.ok, qPrintable(res.error));
        QVERIFY(!res.workDir.isEmpty() && QFileInfo::exists(res.workDir + QStringLiteral("/graph.txt")));
        QVERIFY(QDir(res.workDir).removeRecursively());

        // cancel while a scene hangs: running jobs are stopped, finished() says cancelled
        auto hang = std::make_shared<FakeBackend>();
        hang->hangLabel = QStringLiteral("scene 2");
        Renderer r3(hang);
        QTimer::singleShot(300, &r3, [&r3] { r3.cancel(); });
        res = renderNow(r3, t, o2);
        QVERIFY(res.cancelled && !res.ok);
        QCOMPARE(res.error, QStringLiteral("cancelled"));
        QVERIFY(hang->cancelled >= 1);
        QCOMPARE(hang->current, 0);
        QVERIFY(!hang->started.contains(QStringLiteral("join")));

        // refusals arrive through finished(), never start a job
        auto idle = std::make_shared<FakeBackend>();
        Renderer r4(idle);
        RenderOptions bad = o2;
        bad.outFile = QStringLiteral("relative/spot.mp4");
        res = renderNow(r4, t, bad);
        QCOMPARE(res.error, QStringLiteral("the output file needs an absolute path"));
        Timeline badT = t;
        badT.clips[0] = imageClip(QStringLiteral("still.png"), 1);
        res = renderNow(r4, badT, o2);
        QVERIFY2(res.error.startsWith(QLatin1String("clips[0].src: needs an absolute local path")), qPrintable(res.error));
        idle->ok = false;
        res = renderNow(r4, t, o2);
        QCOMPARE(res.error, QStringLiteral("fake is off"));
        idle->ok = true;
        idle->refuseLabel = QStringLiteral("join");
        res = renderNow(r4, t, o2);
        QCOMPARE(res.error, QStringLiteral("join: fake cannot run this step: not in the subset"));
        QVERIFY(!idle->started.contains(QStringLiteral("join")));
        // one render at a time
        idle->refuseLabel.clear();
        idle->hangLabel = QStringLiteral("mix");
        QSignalSpy done(&r4, &Renderer::finished);
        QVERIFY(r4.start(t, o2));
        QVERIFY(!r4.start(t, o2));
        QTRY_VERIFY(idle->started.contains(QStringLiteral("mix")));
        r4.cancel();
        QTRY_COMPARE(done.size(), 1);
        QVERIFY(done.first().first().value<RenderResult>().cancelled);
    }

    // ================================================================ the plan runs in ffmpeg

    void rendersWithFfmpeg() {
        if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg.exe not found");
        QString font;
        for (const char* f : {"C:/Windows/Fonts/segoeuib.ttf", "C:/Windows/Fonts/arialbd.ttf", "C:/Windows/Fonts/arial.ttf"})
            if (QFile::exists(QLatin1String(f))) { font = QLatin1String(f); break; }
        if (font.isEmpty()) QSKIP("no Windows font for drawtext");
        const QString fx = m_tmp.filePath(QStringLiteral("fx")), work = m_tmp.filePath(QStringLiteral("work"));
        QVERIFY(QDir().mkpath(fx) && QDir().mkpath(work));
        QVERIFY(QFile::copy(font, work + QStringLiteral("/font.ttf")));
        QString err;
        const auto make = [&](const QStringList& args) { return runFfmpeg(args, fx, &err); };
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc2=s=320x480:d=1", "-frames:v", "1", "a still.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "mandelbrot=s=400x300", "-frames:v", "1", "-update", "1", "b%d.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc=s=320x240:d=3:r=25", "-f", "lavfi", "-i", "sine=f=500:d=3", "-pix_fmt", "yuv420p",
                       "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", "-shortest", "clip.mp4"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc=s=320x240:d=2:r=25", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "ultrafast", "silent.mp4"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=440:d=2", "vo.wav"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=220:d=3", "music.wav"}), qPrintable(err));
        // JPEGs over 2 KB for a still and the logo: image2 used to win media::probe on the .jpg name and be refused, so
        // the still became a colour card and the logo was left out (both with a warning)
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc2=s=320x480", "-frames:v", "1", "-q:v", "3", "c still.jpg"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc=s=240x120", "-frames:v", "1", "-q:v", "2", "logo.jpg"}), qPrintable(err));

        const auto F = [&](const char* n) { return fx + u'/' + QLatin1String(n); };
        QVERIFY(QFileInfo(F("c still.jpg")).size() > 4096 && QFileInfo(F("logo.jpg")).size() > 2048);
        Timeline t;
        t.quality = Quality::Draft;
        t.transition = Transition::Fade;
        t.transitionSeconds = 0.3;
        Clip c0 = imageClip(F("a still.png"), 1.0);
        c0.text = QStringLiteral("Fresh coffee \U0001F389 roasted in town");
        c0.audio = AudioPlacement{F("vo.wav"), 0.1};
        Clip c1;
        c1.kind = ClipKind::Video;
        c1.src = F("clip.mp4");
        c1.duration = 1.0;
        c1.srcStart = 0.3;
        c1.audioGain = 0.8;
        c1.move = ClipMove{{0.5, 0.5, 1}, {0.4, 0.6, 1.3}, Ease::EaseInOut};
        c1.transition = Transition::Dip;
        Clip c2 = colorClip(1.0, QStringLiteral("#0b3d91"));
        c2.accent = QStringLiteral("#ff8a3d");
        c2.text = QStringLiteral("Baked daily from 6 AM");
        c2.transition = Transition::Slide;
        Clip c3 = imageClip(F("b%d.png"), 1.0);
        c3.fit = Fit::Contain;
        c3.move = ClipMove{{0.3, 0.3, 1.2}, {0.7, 0.6, 1.0}, Ease::EaseOut};
        c3.transition = Transition::Wipe;
        Clip c4 = imageClip(F("c still.jpg"), 1.0);
        c4.focusX = 0.2;
        c4.kenburns = KenBurns::Right;
        c4.text = QStringLiteral("Lower third");
        c4.textPosition = TextPosition::Bottom;
        c4.transition = Transition::Zoom;
        Clip c5;
        c5.kind = ClipKind::Video;
        c5.src = F("silent.mp4");
        c5.duration = 1.0;
        c5.speed = 2;
        c5.srcStart = 0.5;   // (1 + 0.3 overlap) * 2 = 2.6 s from 0.5 s of a 2 s clip: the last frame is held, not looped
        c5.transition = Transition::Dissolve;
        Clip c6 = colorClip(0.8, QStringLiteral("#222222"));
        c6.transition = Transition::Cut;
        t.clips = {c0, c1, c2, c3, c4, c5, c6};
        EndCard ec;
        ec.brand = QStringLiteral("Main Street Coffee");
        ec.cta = QStringLiteral("Come say hi");
        ec.url = QStringLiteral("mainstreet.coffee");
        ec.logo = F("logo.jpg");
        ec.colors = {QStringLiteral("#0b3d91"), QStringLiteral("#ff8a3d")};
        ec.duration = 1.2;
        t.endCard = ec;
        AudioPlacement vo{F("vo.wav"), 2.0};
        vo.tempo = 1.2;
        vo.from = 0.5;
        vo.maxSeconds = 1.0;
        t.audio.voiceover = vo;
        AudioPlacement ev{F("vo.wav"), 5.0};
        ev.maxSeconds = 0.6;
        ev.volume = 0.7;
        t.audio.events = {ev};
        t.audio.music = MusicBed{F("music.wav"), 0.3, true};
        t.audio.fadeIn = 0.2;
        t.audio.fadeOut = 0.5;
        t.captions = cuesForLine(u"Every morning starts with a choice. Make it a good one. Come say hi.", 0.2, 6);
        t.targetSeconds = 8.5;   // the slots add up to 8.0: the last frame is held
        QVERIFY2(t.validate().isEmpty(), qPrintable(t.validate().join(u'|')));

        PlanOptions o;
        o.workDir = work;
        o.outFile = m_tmp.filePath(QStringLiteral("spot.mp4"));
        o.posterFile = m_tmp.filePath(QStringLiteral("spot.png"));
        o.font = QStringLiteral("fontfile=font.ttf");
        o.probe = [](const QString& p) {
            const studio::media::MediaInfo m = studio::media::probe(p);
            return SourceInfo{m.ok && m.hasVideo(), m.ok && m.hasAudio()};
        };
        const Plan plan = buildPlan(t, o);
        QVERIFY2(plan.ok, qPrintable(plan.error));
        QVERIFY2(plan.warnings.isEmpty(), qPrintable(plan.warnings.join(u'|')));
        QVERIFY(plan.join.graph.contains(QLatin1String("sidechaincompress")));
        QCOMPARE(plan.join.args.count(F("clip.mp4")), 1);   // the clip's own sound is an input of the join
        for (const Step* s : plan.steps())                   // ffmpeg runs every input behind -protocol_whitelist file
            QVERIFY2(!inputsOf(s->args).join(u'|').contains(QLatin1String("<unguarded>")), qPrintable(s->args.join(u' ').left(400)));
        QVERIFY(plan.segments[5].graph.contains(QLatin1String("tpad=stop_mode=clone:stop_duration=2,")));
        QCOMPARE(plan.segments[4].staged.value(0).name, QStringLiteral("img4.jpg"));   // the JPEG still is a still
        QCOMPARE(plan.segments[7].staged.value(0).name, QStringLiteral("logo.jpg"));   // ... and the JPEG logo is on the card
        for (const Step& s : plan.segments)   // JPEGs decode full range: every segment converts to limited range
            QVERIFY2(s.graph.endsWith(QLatin1String(",scale=out_range=tv,format=yuv420p[v]")), qPrintable(s.graph.right(80)));
        QElapsedTimer timer;
        timer.start();
        QVERIFY2(runPlan(plan, work, o.outFile, &err), qPrintable(err));
        qInfo("rendered %d steps in %lld ms", int(plan.steps().size()), qint64(timer.elapsed()));

        studio::media::ProbeOptions po;
        po.exactDuration = true;
        const studio::media::MediaInfo m = studio::media::probe(o.outFile, po);
        QVERIFY2(m.ok, qPrintable(m.error));
        QVERIFY2(std::fabs(m.durationSec - 8.5) <= 0.05, qPrintable(QString::number(m.durationSec, 'f', 3)));
        QCOMPARE(m.video.codec, QStringLiteral("h264"));
        QCOMPARE(m.video.width, 360);
        QCOMPARE(m.video.height, 640);
        QCOMPARE(m.video.pixFmt, QStringLiteral("yuv420p"));
        QCOMPARE(m.audio.codec, QStringLiteral("aac"));
        QCOMPARE(m.audio.sampleRate, 48000);
        QCOMPARE(m.audio.channels, 2);
        const QImage poster(o.posterFile);
        QCOMPARE(poster.size(), QSize(360, 640));
        // every segment is its slot + overlap long, and limited range (the JPEG still's and the logo card's were yuvj420p)
        for (const Step& s : plan.segments) {
            const studio::media::MediaInfo sm = studio::media::probe(s.output, po);
            QVERIFY2(sm.ok && std::fabs(sm.durationSec - s.seconds) <= 0.05 && sm.video.pixFmt == QLatin1String("yuv420p"),
                     qPrintable(QStringLiteral("slot %1: %2 s, expected %3, %4").arg(s.slot).arg(sm.durationSec).arg(s.seconds).arg(sm.video.pixFmt)));
        }

        // a short 16:9 composition with no sound at all still has an AAC track of the exact length
        Timeline s;
        s.aspect = Aspect::Landscape;
        s.quality = Quality::Draft;
        s.clips = {colorClip(1.5, QStringLiteral("#ff8a3d")), colorClip(1.5, QStringLiteral("#111111"))};
        s.clips[0].text = QStringLiteral("Square deal");
        s.clips[0].transition = Transition::Dissolve;
        const QString work2 = m_tmp.filePath(QStringLiteral("work2"));
        QVERIFY(QDir().mkpath(work2));
        QVERIFY(QFile::copy(font, work2 + QStringLiteral("/font.ttf")));
        PlanOptions o2 = o;
        o2.workDir = work2;
        o2.outFile = m_tmp.filePath(QStringLiteral("card.mp4"));
        o2.posterFile = m_tmp.filePath(QStringLiteral("card.png"));
        const Plan p2 = buildPlan(s, o2);
        QVERIFY2(runPlan(p2, work2, o2.outFile, &err), qPrintable(err));
        const studio::media::MediaInfo m2 = studio::media::probe(o2.outFile, po);
        QVERIFY2(m2.ok && std::fabs(m2.durationSec - 3.0) <= 0.05, qPrintable(QString::number(m2.durationSec, 'f', 3)));
        QCOMPARE(m2.video.width, 640);
        QCOMPARE(m2.audio.codec, QStringLiteral("aac"));
    }

    // ================================================================ ST-18b: the renderer on the staged ffmpeg

    void ffmpegBackend() {
        if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg.exe not found");
        RenderJob p;
        QCOMPARE(FfmpegBackend::prefix(p), (QStringList{"-hide_banner", "-nostdin", "-y", "-loglevel", "error"}));
        p.progress = true;
        QCOMPARE(FfmpegBackend::prefix(p).mid(5), (QStringList{"-progress", "pipe:1", "-nostats"}));
        p.progress = false;
        p.logLevel = QStringLiteral("info");
        QCOMPARE(FfmpegBackend::prefix(p).mid(3), (QStringList{"-loglevel", "info", "-nostats"}));
        QCOMPARE(stderrTail(u"frame=  10 fps=0\nsize=1kB time=1\n[in] bad thing\nPress [q] to stop\n  last error  \n"),
                 QStringLiteral("[in] bad thing | last error"));
        QVERIFY(defaultLanes() >= 1 && defaultLanes() <= 4);

        FfmpegBackend b(m_ffmpeg);
        QVERIFY(b.available());
        const QString cwd = m_tmp.filePath(QStringLiteral("backend"));
        QVERIFY(QDir().mkpath(cwd));
        const auto run = [](IRenderBackend& be, const RenderJob& j, int cancelAfterMs = -1, QVector<double>* prog = nullptr) {
            QObject ctx;
            RenderJobResult res;
            int n = 0;
            QEventLoop loop;
            RenderRun* r = be.start(j, &ctx);
            QObject::connect(r, &RenderRun::finished, &loop, [&](const RenderJobResult& x) { res = x; ++n; loop.quit(); });
            if (prog) QObject::connect(r, &RenderRun::progress, &loop, [prog](double s) { prog->push_back(s); });
            if (cancelAfterMs >= 0) QTimer::singleShot(cancelAfterMs, r, [r] { r->cancel(); });
            QTimer::singleShot(60000, &loop, &QEventLoop::quit);
            loop.exec();
            QCoreApplication::processEvents();
            if (n != 1) res.error = QStringLiteral("finished %1 times").arg(n);
            return res;
        };
        // a job that works, reporting its progress from -progress pipe:1
        RenderJob j;
        j.cwd = cwd;
        j.progress = true;
        j.args = QStringList{"-f", "lavfi", "-i", "testsrc=s=160x90:d=2:r=25", "-c:v", "libx264", "-preset", "ultrafast", "ok.mp4"};
        QVector<double> prog;
        RenderJobResult res = run(b, j, -1, &prog);
        QVERIFY2(res.ok, qPrintable(res.error));
        QCOMPARE(res.exitCode, 0);
        QVERIFY(QFileInfo::exists(cwd + QStringLiteral("/ok.mp4")));
        QVERIFY(!prog.isEmpty() && prog.last() >= 1.5);
        // a failure carries compose.ts's message with the stderr tail
        j.progress = false;
        j.args = QStringList{"-protocol_whitelist", "file", "-i", "missing.mp4", "-f", "null", "-"};
        res = run(b, j);
        QVERIFY(!res.ok && !res.cancelled && res.exitCode != 0);
        QVERIFY2(res.error.startsWith(QLatin1String("ffmpeg failed (exit ")) && res.error.contains(QLatin1String("missing.mp4")), qPrintable(res.error));
        // a step that runs too long is stopped; a cancel stops one at once
        j.args = QStringList{"-re", "-f", "lavfi", "-i", "testsrc=s=160x90:d=30:r=25", "-f", "null", "-"};
        j.timeoutMs = 600;
        res = run(b, j);
        QVERIFY(res.timedOut && !res.ok);
        QCOMPARE(res.error, QStringLiteral("ffmpeg timed out after 1 s"));
        j.timeoutMs = 60000;
        QElapsedTimer t;
        t.start();
        res = run(b, j, 300);
        QVERIFY(res.cancelled && !res.ok);
        QCOMPARE(res.error, QStringLiteral("cancelled"));
        QVERIFY(t.elapsed() < 10000);
        // no such program
        FfmpegBackend none(QStringLiteral("C:/nowhere/ffmpeg.exe"));
        QString why;
        QVERIFY(!none.available(&why) && why.contains(QLatin1String("not found")));
        res = run(none, j);
        QVERIFY2(!res.ok && res.error.startsWith(QLatin1String("ffmpeg could not start")), qPrintable(res.error));
    }

    void rendersTestCard() {
        if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg.exe not found");
        if (findFont().isEmpty()) QSKIP("no font for drawtext");
        const QString fx = m_tmp.filePath(QStringLiteral("card-fx"));
        QVERIFY(QDir().mkpath(fx));
        QString err;
        const auto make = [&](const QStringList& args) { return runFfmpeg(args, fx, &err); };
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc2=s=640x360", "-frames:v", "1", "card.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "smptehdbars=s=640x360", "-frames:v", "1", "bars.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc=s=320x240:d=2:r=30", "-f", "lavfi", "-i", "sine=f=500:d=2", "-pix_fmt", "yuv420p",
                       "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", "-shortest", "count.mp4"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=880:d=3", "-af", "volume=0.3", "voice.wav"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=220:d=4", "bed.wav"}), qPrintable(err));
        const auto F = [&](const char* n) { return fx + u'/' + QLatin1String(n); };

        // a 6 s test card: still with a push and a headline, a colour card, bars (contain), a clip with its own sound,
        // an end card; captions, a voice over ducked music; EBU R128 in two passes
        Timeline t;
        t.aspect = Aspect::Landscape;
        t.quality = Quality::Draft;
        t.transitionSeconds = 0.4;
        Clip c0 = imageClip(F("card.png"), 1.5);
        c0.kenburns = KenBurns::In;
        c0.text = QStringLiteral("PlayPlay Studio test card");
        c0.transition = Transition::Fade;
        Clip c1 = colorClip(1.5, QStringLiteral("#0b3d91"));
        c1.accent = QStringLiteral("#ff8a3d");
        c1.text = QStringLiteral("Segment two");
        c1.transition = Transition::Cut;
        Clip c2 = imageClip(F("bars.png"), 1.0);
        c2.fit = Fit::Contain;
        c2.kenburns = KenBurns::None;
        c2.transition = Transition::Wipe;
        Clip c3;
        c3.kind = ClipKind::Video;
        c3.src = F("count.mp4");
        c3.duration = 1.0;
        c3.audioGain = 0.5;
        c3.transition = Transition::Dissolve;
        t.clips = {c0, c1, c2, c3};
        EndCard ec;
        ec.brand = QStringLiteral("Star News");
        ec.cta = QStringLiteral("Watch tonight");
        ec.duration = 1.0;
        t.endCard = ec;
        t.audio.voiceover = AudioPlacement{F("voice.wav"), 0.3};
        t.audio.music = MusicBed{F("bed.wav"), 0.25, true};
        t.audio.loudness = Loudness::R128;
        t.captions = cuesForLine(u"This is the six second test card for the Studio renderer.", 0.3, 5.0);
        QCOMPARE(t.totalSeconds(), 6.0);
        QVERIFY(t.validate().isEmpty());

        Renderer r(std::make_shared<FfmpegBackend>(m_ffmpeg));
        RenderOptions o;
        o.outFile = m_tmp.filePath(QStringLiteral("card/out/testcard.mp4"));
        o.workDir = m_tmp.filePath(QStringLiteral("card/work"));
        o.lanes = 3;
        o.partSeconds = -1;
        QVector<QPair<double, QString>> prog;
        const RenderResult res = renderNow(r, t, o, &prog, 110000);
        QVERIFY2(res.ok, qPrintable(res.error));
        qInfo("test card: %lld ms, %d steps, %d at once, mix %.2f LUFS, output %.2f LUFS / %.2f dBTP",
              qint64(res.elapsedMs), res.steps, res.peakParallel, res.loudness.mix.i, res.loudness.output.i, res.loudness.output.tp);
        QVERIFY2(res.warnings.isEmpty(), qPrintable(res.warnings.join(u'|')));
        QCOMPARE(res.parts, 0);
        QVERIFY(res.peakParallel >= 2 && res.peakParallel <= 3);
        // length exact to ±0.05 s, H.264 / AAC, the poster
        studio::media::ProbeOptions po;
        po.exactDuration = true;
        const studio::media::MediaInfo m = studio::media::probe(o.outFile, po);
        QVERIFY2(m.ok, qPrintable(m.error));
        QVERIFY2(std::fabs(m.durationSec - 6.0) <= 0.05, qPrintable(QString::number(m.durationSec, 'f', 3)));
        QVERIFY(std::fabs(res.duration - 6.0) <= 0.05);
        QCOMPARE(m.video.codec, QStringLiteral("h264"));
        QCOMPARE(m.video.width, 640);
        QCOMPARE(m.video.height, 360);
        QCOMPARE(m.audio.codec, QStringLiteral("aac"));
        QCOMPARE(m.audio.sampleRate, 48000);
        QCOMPARE(m.audio.channels, 2);
        QCOMPARE(res.width, 640);
        QCOMPARE(QImage(res.poster).size(), QSize(640, 360));
        // the sidecars are the burned-in cues, in media.ts's format
        QFile srt(res.srt), vtt(res.vtt);
        QVERIFY(srt.open(QIODevice::ReadOnly) && vtt.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(srt.readAll()), toSrt(sidecarCues(t)));
        QCOMPARE(QString::fromUtf8(vtt.readAll()), toVtt(sidecarCues(t)));
        QCOMPARE(res.srt, m_tmp.filePath(QStringLiteral("card/out/testcard.srt")));
        // loudness: measured, normalised linearly, and the finished file is at the target
        QVERIFY(res.loudness.on && res.loudness.passes == 2 && res.loudness.mix.ok && res.loudness.output.ok);
        QVERIFY2(std::fabs(res.loudness.output.i - (-23)) <= 1.0, qPrintable(QString::number(res.loudness.output.i)));
        QVERIFY2(res.loudness.output.tp <= -0.5, qPrintable(QString::number(res.loudness.output.tp)));
        QFile graph(o.workDir + QStringLiteral("/graph.txt"));
        QVERIFY(graph.open(QIODevice::ReadOnly));
        const QString g = QString::fromUtf8(graph.readAll());
        QVERIFY2(g.contains(QLatin1String(":measured_I=")) && g.contains(QLatin1String("linear=true")) && !g.contains(QLatin1String("{LOUDNORM}")),
                 qPrintable(g.right(400)));
        // progress went forward to done
        QVERIFY(prog.size() >= 4);
        for (qsizetype i = 1; i < prog.size(); ++i) QVERIFY(prog[i].first >= prog[i - 1].first);
        QCOMPARE(prog.last().second, QStringLiteral("done"));
    }

    void segmentParallelMatchesSinglePass() {
        if (m_ffmpeg.isEmpty()) QSKIP("ffmpeg.exe not found");
        if (findFont().isEmpty()) QSKIP("no font for drawtext");
        const QString fx = m_tmp.filePath(QStringLiteral("par-fx"));
        QVERIFY(QDir().mkpath(fx));
        QString err;
        const auto make = [&](const QStringList& args) { return runFfmpeg(args, fx, &err); };
        QVERIFY2(make({"-f", "lavfi", "-i", "testsrc2=s=640x360", "-frames:v", "1", "card.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "smptehdbars=s=640x360", "-frames:v", "1", "bars.png"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=660:d=4", "-af", "volume=0.3", "voice.wav"}), qPrintable(err));
        QVERIFY2(make({"-f", "lavfi", "-i", "sine=f=330:d=6", "bed.wav"}), qPrintable(err));
        const auto F = [&](const char* n) { return fx + u'/' + QLatin1String(n); };

        // 8 slots of 0.75 s: stills held still, colour cards; blends and cuts; captions that change on slot boundaries
        Timeline t;
        t.aspect = Aspect::Landscape;
        t.quality = Quality::Draft;
        t.transitionSeconds = 0.3;
        const QVector<Transition> tr{Transition::Fade, Transition::Dissolve, Transition::Cut, Transition::Wipe,
                                     Transition::Fade, Transition::Cut, Transition::Slide, Transition::Cut};
        for (int i = 0; i < 8; ++i) {
            Clip c = i % 3 == 1 ? colorClip(0.75, i % 2 ? QStringLiteral("#0b3d91") : QStringLiteral("#2a7a3a")) : imageClip(F(i % 3 ? "bars.png" : "card.png"), 0.75);
            c.kenburns = KenBurns::None;
            if (i % 3 == 2) c.fit = Fit::Contain;
            c.transition = tr[i];
            t.clips << c;
        }
        t.captions = {{0.0, 2.25, QStringLiteral("First caption")}, {2.25, 4.5, QStringLiteral("Second caption")}, {4.5, 6.0, QStringLiteral("Third caption")}};
        t.audio.voiceover = AudioPlacement{F("voice.wav"), 0.5};
        t.audio.music = MusicBed{F("bed.wav"), 0.3, true};
        QCOMPARE(t.totalSeconds(), 6.0);

        RenderOptions a;
        a.outFile = m_tmp.filePath(QStringLiteral("par/single.mp4"));
        a.workDir = m_tmp.filePath(QStringLiteral("par/w1"));
        a.lanes = 3;
        a.partSeconds = -1;
        RenderOptions b = a;
        b.outFile = m_tmp.filePath(QStringLiteral("par/split.mp4"));
        b.workDir = m_tmp.filePath(QStringLiteral("par/w2"));
        b.partSeconds = 1.5;                                        // windows at 1.8 s (54), 3.3 s (99), 4.5 s (135)
        Renderer ra(std::make_shared<FfmpegBackend>(m_ffmpeg)), rb(std::make_shared<FfmpegBackend>(m_ffmpeg));
        const RenderResult A = renderNow(ra, t, a, nullptr, 110000);
        QVERIFY2(A.ok, qPrintable(A.error));
        const RenderResult B = renderNow(rb, t, b, nullptr, 110000);
        QVERIFY2(B.ok, qPrintable(B.error));
        qInfo("single pass %lld ms (%d steps), segment-parallel %lld ms (%d steps, %d parts)", qint64(A.elapsedMs), A.steps,
              qint64(B.elapsedMs), B.steps, B.parts);
        QCOMPARE(A.parts, 0);
        QCOMPARE(B.parts, 4);
        QVERIFY(QFileInfo::exists(b.workDir + QStringLiteral("/part003.mp4")));
        // the same length to the frame
        studio::media::ProbeOptions po;
        po.exactDuration = true;
        const studio::media::MediaInfo ma = studio::media::probe(a.outFile, po), mb = studio::media::probe(b.outFile, po);
        QVERIFY2(ma.ok && mb.ok, qPrintable(ma.error + mb.error));
        qInfo("single %.4f s / %lld frames, split %.4f s / %lld frames", ma.durationSec, ma.video.frames, mb.durationSec, mb.video.frames);
        QVERIFY2(std::fabs(ma.durationSec - 6.0) <= 0.05, qPrintable(QString::number(ma.durationSec, 'f', 4)));
        QVERIFY2(std::fabs(mb.durationSec - ma.durationSec) <= 0.0011, qPrintable(QString::number(mb.durationSec, 'f', 4)));
        QCOMPARE(mb.video.frames, ma.video.frames);
        QCOMPARE(ma.video.frames, qint64(180));
        QCOMPARE(mb.audio.codec, QStringLiteral("aac"));
        QVERIFY(std::fabs(B.loudness.output.i - A.loudness.output.i) <= 0.3);
        // the same pictures: mid-slot samples (away from blends and caption changes) differ only by encoding noise
        const std::vector<double> times{0.35, 1.3, 2.05, 2.8, 3.55, 4.3, 5.05, 5.8};
        const studio::media::ThumbScan sa = studio::media::sampleThumbs(a.outFile, times, 96, 54);
        const studio::media::ThumbScan sb = studio::media::sampleThumbs(b.outFile, times, 96, 54);
        QVERIFY(sa.ok && sb.ok && sa.rgb.size() == times.size() && sb.rgb.size() == times.size());
        double worst = 0;
        for (size_t i = 0; i < times.size(); ++i) worst = std::max(worst, meanDiff(sa.rgb[i], sb.rgb[i]));
        qInfo("largest mean picture difference %.2f / 255", worst);
        QVERIFY2(worst <= 6, qPrintable(QString::number(worst)));
        // and a caption really is in the parts' pictures: a captioned frame differs from the uncaptioned render
        Timeline bare = t;
        bare.captions.clear();
        RenderOptions c = b;
        c.outFile = m_tmp.filePath(QStringLiteral("par/bare.mp4"));
        c.workDir = m_tmp.filePath(QStringLiteral("par/w3"));
        c.verifyLoudness = false;
        Renderer rc(std::make_shared<FfmpegBackend>(m_ffmpeg));
        const RenderResult C = renderNow(rc, bare, c, nullptr, 110000);
        QVERIFY2(C.ok, qPrintable(C.error));
        const studio::media::ThumbScan sc = studio::media::sampleThumbs(c.outFile, times, 96, 54);
        QVERIFY(sc.ok && sc.rgb.size() == times.size());
        for (size_t i = 0; i < times.size(); ++i)
            QVERIFY2(meanDiff(sb.rgb[i], sc.rgb[i]) > 0.5, qPrintable(QStringLiteral("no caption at %1 s").arg(times[i])));
    }
};

QTEST_GUILESS_MAIN(TestStudioCompose)
#include "test_studio_compose.moc"
