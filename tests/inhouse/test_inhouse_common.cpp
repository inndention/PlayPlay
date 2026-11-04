// DEP-00: the in-house tool contract (ppcli) and WAV levelling (wavio).
// The same executable doubles as the child process for the stdio tests: --oneshot-child <mode> and --serve-child.
#include "ppcli.h"
#include "toolswitch.h"
#include "wavio.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

constexpr double kPi = 3.14159265358979323846;

namespace {

std::atomic<int> g_running{0};      // handlers currently inside "sleep" (tests wait for 0 before leaving detached work)

// The handler both the in-process loops and --serve-child use.
QJsonObject testHandler(const QJsonObject& req, ppcli::ServeContext& ctx) {
    const QString op = req.value("op").toString();
    if (op == "echo") return QJsonObject{{"ok", true}, {"echo", req.value("text")}};
    if (op == "plain") return QJsonObject{{"value", 5}};                       // no "ok": the loop adds ok:true
    if (op == "fail") return ppcli::failure("nope");
    if (op == "throw") throw std::runtime_error("boom");
    if (op == "progress") { for (int i = 1; i <= 3; ++i) ctx.progress(i, 3); return QJsonObject{{"ok", true}}; }
    if (op == "sleep") {
        ++g_running;
        const int ms = req.value("ms").toInt(); const bool coop = req.value("coop").toBool();
        QElapsedTimer t; t.start();
        bool cancelled = false;
        while (t.elapsed() < ms) {
            if (coop && ctx.cancelled()) { cancelled = true; break; }
            std::this_thread::sleep_for(5ms);
        }
        --g_running;
        if (cancelled) return ppcli::failure("cancelled");
        return QJsonObject{{"ok", true}, {"slept", ms}};
    }
    return ppcli::ServeLoop::unsupportedOp(op);
}

// In-process line source / sink for ServeLoop.
struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<QByteArray> in;
    bool closed = false;
    QList<QByteArray> out;
    void push(const QByteArray& l) { std::lock_guard<std::mutex> g(m); in.push_back(l); cv.notify_all(); }
    void close() { std::lock_guard<std::mutex> g(m); closed = true; cv.notify_all(); }
    bool read(QByteArray& l) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return closed || !in.empty(); });
        if (in.empty()) return false;
        l = in.front(); in.pop_front();
        return true;
    }
    bool write(const QByteArray& b) { std::lock_guard<std::mutex> g(m); out << b; return true; }
    QList<QJsonObject> lines() {
        std::lock_guard<std::mutex> g(m);
        QList<QJsonObject> r;
        for (const QByteArray& b : out) r << QJsonDocument::fromJson(b).object();
        return r;
    }
    QList<QByteArray> raw() { std::lock_guard<std::mutex> g(m); return out; }
};

struct Harness {
    std::shared_ptr<Pipe> pipe = std::make_shared<Pipe>();
    std::shared_ptr<std::atomic<int>> code = std::make_shared<std::atomic<int>>(-1);
    std::thread th;
    void start(ppcli::ServeLoop::Options o) {
        auto p = pipe; auto c = code;
        o.readLine = [p](QByteArray& l) { return p->read(l); };
        o.cancelRead = [p] { p->close(); };
        o.writeLine = [p](const QByteArray& b) { return p->write(b); };
        o.hardExitOnHang = false;
        th = std::thread([o, c] { ppcli::ServeLoop loop(o); c->store(loop.run(&testHandler)); });
    }
    void send(const QJsonObject& o) { pipe->push(QJsonDocument(o).toJson(QJsonDocument::Compact)); }
    // The reply (a line with "ok") for `id`, waiting up to `ms`.
    QJsonObject reply(const QJsonValue& id, int ms = 5000) {
        QElapsedTimer t; t.start();
        do {
            for (const QJsonObject& o : pipe->lines()) if (o.value("id") == id && o.contains("ok")) return o;
            std::this_thread::sleep_for(2ms);
        } while (t.elapsed() < ms);
        return {};
    }
    int repliesFor(const QJsonValue& id) { int n = 0; for (const QJsonObject& o : pipe->lines()) if (o.value("id") == id && o.contains("ok")) ++n; return n; }
    bool waitExit(int ms) {
        QElapsedTimer t; t.start();
        while (code->load() < 0 && t.elapsed() < ms) std::this_thread::sleep_for(2ms);
        if (code->load() < 0) return false;
        if (th.joinable()) th.join();
        return true;
    }
    ~Harness() { pipe->close(); if (th.joinable() && !waitExit(15000)) th.detach(); }
};

bool waitIdle(int ms) { QElapsedTimer t; t.start(); while (g_running.load() > 0 && t.elapsed() < ms) std::this_thread::sleep_for(5ms); return g_running.load() == 0; }

// ---- child process side
QStringList childArgs(int argc, char** argv) { QStringList a; for (int i = 2; i < argc; ++i) a << QString::fromLocal8Bit(argv[i]); return a; }

int oneshotChild(int argc, char** argv) {
    ppcli::initStdio();
    const QStringList args = childArgs(argc, argv);
    const QString mode = args.value(0);
    if (mode == "ok") {
        ppcli::log("diagnostics go to stderr   ");
        ppcli::progress(1, 2); ppcli::progress(2, 2);
        return ppcli::finish(QJsonObject{{"ok", true}, {"value", 42}, {"text", "two\nlines"}});
    }
    if (mode == "fail") return ppcli::fail("it broke", QJsonObject{{"engine", "x"}});
    if (mode == "args") {
        const ppcli::Args a(args.mid(1), {"--text-file=", "--out=", "--serve"});
        if (!a.ok()) return a.report();
        return ppcli::finish(QJsonObject{{"ok", true}, {"out", a.value("--out")}, {"serve", a.has("--serve")}});
    }
    return ppcli::unsupported("mode " + mode);
}

int serveChild(int argc, char** argv) {
    ppcli::initStdio();
    const ppcli::Args a(childArgs(argc, argv), {"--hang-grace-ms=", "--workers="});
    if (!a.ok()) return a.report();
    ppcli::ServeLoop::Options o;
    o.workers = a.integer("--workers", 2);
    o.defaultTimeoutMs = 5000;
    o.hangGraceMs = a.integer("--hang-grace-ms", 2000);
    return ppcli::ServeLoop(o).run(&testHandler);
}

// ---- test-side process helpers
struct Child {
    QProcess p;
    QByteArray buf;
    bool start(const QStringList& args) {
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(args);
        p.start();
        return p.waitForStarted(10000);
    }
    void send(const QJsonObject& o) { p.write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'); p.waitForBytesWritten(2000); }
    QJsonObject reply(const QJsonValue& id, int ms = 8000) {
        QElapsedTimer t; t.start();
        for (;;) {
            int nl;
            while ((nl = buf.indexOf('\n')) >= 0) {
                const QJsonObject o = QJsonDocument::fromJson(buf.left(nl)).object(); buf.remove(0, nl + 1);
                if (o.value("id") == id && o.contains("ok")) return o;
            }
            if (t.elapsed() > ms) return {};
            if (!p.waitForReadyRead(50)) { if (p.state() == QProcess::NotRunning) { buf += p.readAllStandardOutput(); if (buf.indexOf('\n') < 0) return {}; } }
            buf += p.readAllStandardOutput();
        }
    }
    ~Child() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } }    // only the child this test started
};

// ---- WAV helpers
wavio::Wav burst(int rate, int ch, double pre, double sec, double post, double amp, double freq = 440.0) {
    wavio::Wav w; w.sampleRate = rate; w.channels = ch;
    const qint64 a = qint64(pre * rate), b = a + qint64(sec * rate), n = b + qint64(post * rate);
    w.samples.assign(size_t(n * ch), 0);
    for (qint64 i = a; i < b; ++i) {
        const double v = amp * std::cos(2.0 * kPi * freq * double(i - a) / rate);                 // starts at its peak
        for (int c = 0; c < ch; ++c) w.samples[size_t(i * ch + c)] = qint16(std::lround(std::clamp(v, -32767.0, 32767.0)));
    }
    return w;
}
double ampForRmsDb(double db) { return std::sqrt(2.0) * std::pow(10.0, db / 20.0) * 32768.0; }   // sine: RMS = A / sqrt(2)
int maxAbs(const wavio::Wav& w) { int m = 0; for (qint16 s : w.samples) m = std::max(m, std::abs(int(s))); return m; }

// A speech-like take: syllable bursts of varying loudness with sharp clicks, between silences (deterministic).
wavio::Wav speechLike(int rate, double clickAmp) {
    wavio::Wav w; w.sampleRate = rate; w.channels = 1;
    const qint64 n = qint64(3.0 * rate);
    w.samples.assign(size_t(n), 0);
    quint32 lcg = 12345u;
    auto rnd = [&] { lcg = lcg * 1664525u + 1013904223u; return double(lcg >> 8) / double(1u << 24) - 0.5; };
    for (qint64 i = qint64(0.3 * rate); i < qint64(2.5 * rate); ++i) {
        const double t = double(i) / rate, ph = std::fmod(t, 0.2);
        if (ph > 0.15) continue;                                                // 150 ms syllable, 50 ms gap
        const double env = std::sin(kPi * ph / 0.15) * (0.6 + 0.4 * std::sin(2.0 * kPi * 0.7 * t));
        double v = env * (0.06 * std::sin(2 * kPi * 180 * t) + 0.036 * std::sin(2 * kPi * 410 * t) + 0.01 * rnd());
        if (i % (rate * 3 / 10) == 0) v = clickAmp;                             // a click every 300 ms
        w.samples[size_t(i)] = qint16(std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
    }
    return w;
}

QString findPythonWithNumpy() {
    QStringList c;
    if (qEnvironmentVariableIsSet("PLAYPLAY_TEST_PYTHON")) c << qEnvironmentVariable("PLAYPLAY_TEST_PYTHON");
    c << "C:/Program Files/Python311/python.exe" << "C:/Program Files/Python312/python.exe" << "C:/Program Files/Python313/python.exe";   // spotgenerator.cpp's list
    const QString onPath = QStandardPaths::findExecutable("python");
    if (!onPath.isEmpty()) c << onPath;
    for (const QString& p : c) {
        if (!QFileInfo::exists(p)) continue;
        QProcess q; q.start(p, {"-I", "-c", "import numpy"});
        if (q.waitForFinished(30000) && q.exitStatus() == QProcess::NormalExit && q.exitCode() == 0) return p;
    }
    return {};
}

}   // namespace

class TestInhouseCommon : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { QVERIFY(waitIdle(10000)); }

    // ------------------------------------------------------------------ one-shot contract
    void resultLineIsOneCompactLine() {
        const QByteArray l = ppcli::resultLine(QJsonObject{{"ok", true}, {"text", "a\nb"}, {"n", 1.5}});
        QVERIFY(l.endsWith('\n'));
        QCOMPARE(int(l.count('\n')), 1);
        QCOMPARE(QJsonDocument::fromJson(l).object().value("text").toString(), QString("a\nb"));
        QCOMPARE(ppcli::exitCodeFor(QJsonObject{{"ok", true}}), 0);
        QCOMPARE(ppcli::exitCodeFor(ppcli::failure("x")), 2);
        QCOMPARE(ppcli::exitCodeFor(QJsonObject{}), 2);
        const QJsonObject u = ppcli::unsupportedResult("--crf", QJsonObject{{"engine", "native"}});
        QCOMPARE(ppcli::exitCodeFor(u), 64);
        QCOMPARE(u.value("error").toString(), QString("unsupported: --crf"));
        QCOMPARE(u.value("unsupported").toBool(), true);
        QCOMPARE(u.value("engine").toString(), QString("native"));
        QVERIFY(ppcli::isUnsupported(64, {}));
        QVERIFY(ppcli::isUnsupported(2, u));
        QVERIFY(!ppcli::isUnsupported(2, ppcli::failure("x")));
    }

    void parseResultTakesTheLastLine() {
        QString err;
        QJsonObject r = ppcli::parseResult("loading model\r\n{\"ok\": false}\r\n{\"ok\": true, \"path\": \"a.wav\"}\r\n\r\n", &err);
        QCOMPARE(r.value("ok").toBool(), true);
        QCOMPARE(r.value("path").toString(), QString("a.wav"));
        r = ppcli::parseResult("{\"ok\": true}\nTraceback: boom\n", &err);
        QVERIFY(r.isEmpty());
        QVERIFY(err.contains("not a JSON object"));
        QVERIFY(ppcli::parseResult("  \n", &err).isEmpty());
        QCOMPARE(err, QString("no output"));
    }

    void progressLinesMatchTheSupervisorRegex() {
        QCOMPARE(ppcli::progressLine(12.34, 60), QByteArray("PROGRESS 12.3 60.0\n"));
        QCOMPARE(ppcli::progressLine(3, 7, 0), QByteArray("PROGRESS 3 7\n"));
        QCOMPARE(ppcli::progressLine(-1, std::nan(""), 1), QByteArray("PROGRESS 0.0 0.0\n"));
        static const QRegularExpression rx("^PROGRESS\\s+([\\d.]+)\\s+([\\d.]+)");      // transcriptionmanager.cpp
        const auto m = rx.match(QString::fromUtf8(ppcli::progressLine(5.25, 10, 2)).trimmed());
        QVERIFY(m.hasMatch());
        QCOMPARE(m.captured(1).toDouble(), 5.25);
        double d = 0, t = 0;
        QVERIFY(ppcli::parseProgress("PROGRESS 4.5 9.0\r\n", &d, &t));
        QCOMPARE(d, 4.5); QCOMPARE(t, 9.0);
        QVERIFY(!ppcli::parseProgress("PROGRESS x 9", &d, &t));
        QVERIFY(!ppcli::parseProgress("LOAD model", &d, &t));
    }

    void argsFollowArgparse() {
        const QStringList spec{"--text-file=", "--out=", "--speed=", "--voice=", "--serve"};
        ppcli::Args a({"--text-file", "in.txt", "--out=o.wav", "--speed", "-1.5", "--serve", "--voice", "a", "--voice", "b"}, spec);
        QVERIFY(a.ok());
        QCOMPARE(a.value("--text-file"), QString("in.txt"));
        QCOMPARE(a.value("--out"), QString("o.wav"));
        QCOMPARE(a.number("--speed", 1.0), -1.5);
        QVERIFY(a.has("--serve"));
        QVERIFY(!a.has("--batch"));
        QCOMPARE(a.value("--batch", "none"), QString("none"));
        QCOMPARE(a.values("--voice"), QStringList({"a", "b"}));
        QCOMPARE(a.value("--voice"), QString("b"));
        bool ok = true;
        QCOMPARE(ppcli::Args({"--speed", "fast"}, spec).number("--speed", 1.0, &ok), 1.0);
        QVERIFY(!ok);

        const ppcli::Args unknown({"--out", "o.wav", "--crf", "20"}, spec);
        QVERIFY(!unknown.ok());
        QVERIFY(unknown.unknownOption());
        QCOMPARE(unknown.unknownName(), QString("--crf"));
        const ppcli::Args missing({"--out"}, spec);
        QVERIFY(!missing.ok());
        QVERIFY(!missing.unknownOption());
        QVERIFY(missing.error().contains("needs a value"));
        QVERIFY(!ppcli::Args({"--serve=1"}, spec).ok());
        QVERIFY(ppcli::Args({"stray"}, spec).unknownOption());

        const ppcli::Args pos({"probe", "-5", "--out", "x", "--", "--not-an-option"}, spec, true);
        QVERIFY(pos.ok());
        QCOMPARE(pos.positional(), QStringList({"probe", "-5", "--not-an-option"}));
    }

    void oneShotChildProcess() {
        Child ok;
        QVERIFY(ok.start({"--oneshot-child", "ok"}));
        QVERIFY(ok.p.waitForFinished(15000));
        QCOMPARE(ok.p.exitCode(), 0);
        const QByteArray out = ok.p.readAllStandardOutput(), err = ok.p.readAllStandardError();
        QCOMPARE(int(out.count('\n')), 1);                                        // stdout holds the result only
        QVERIFY(!out.contains('\r'));                                           // binary stdout: plain "\n"
        const QJsonObject r = ppcli::parseResult(out);
        QCOMPARE(r.value("value").toInt(), 42);
        QCOMPARE(r.value("text").toString(), QString("two\nlines"));
        QVERIFY(err.contains("PROGRESS 1.0 2.0"));
        QVERIFY(err.contains("PROGRESS 2.0 2.0"));
        QVERIFY(QRegularExpression("diagnostics go to stderr\\r?\\n").match(QString::fromUtf8(err)).hasMatch());   // trailing blanks stripped

        Child fail;
        QVERIFY(fail.start({"--oneshot-child", "fail"}));
        QVERIFY(fail.p.waitForFinished(15000));
        QCOMPARE(fail.p.exitCode(), 2);
        QCOMPARE(ppcli::parseResult(fail.p.readAllStandardOutput()).value("error").toString(), QString("it broke"));

        Child unk;
        QVERIFY(unk.start({"--oneshot-child", "args", "--out", "o.wav", "--bogus"}));
        QVERIFY(unk.p.waitForFinished(15000));
        QCOMPARE(unk.p.exitCode(), 64);
        const QJsonObject u = ppcli::parseResult(unk.p.readAllStandardOutput());
        QCOMPARE(u.value("error").toString(), QString("unsupported: --bogus"));
        QVERIFY(u.value("unsupported").toBool());

        Child miss;
        QVERIFY(miss.start({"--oneshot-child", "args", "--out"}));
        QVERIFY(miss.p.waitForFinished(15000));
        QCOMPARE(miss.p.exitCode(), 2);

        Child good;
        QVERIFY(good.start({"--oneshot-child", "args", "--serve", "--out=x.wav"}));
        QVERIFY(good.p.waitForFinished(15000));
        QCOMPARE(good.p.exitCode(), 0);
        const QJsonObject g = ppcli::parseResult(good.p.readAllStandardOutput());
        QCOMPARE(g.value("out").toString(), QString("x.wav"));
        QVERIFY(g.value("serve").toBool());
    }

    // ------------------------------------------------------------------ --serve, in process
    void serveAnswersRequests() {
        Harness h;
        ppcli::ServeLoop::Options o; o.workers = 2;
        h.start(o);
        h.send({{"id", 1}, {"op", "echo"}, {"text", "hi"}});
        h.send({{"id", "abc"}, {"op", "echo"}, {"text", "there"}});
        h.pipe->push("{not json");
        h.pipe->push("[1, 2]");
        h.pipe->push("   ");
        h.send({{"id", 3}, {"op", "nosuch"}});
        h.send({{"id", 4}, {"op", "throw"}});
        h.send({{"id", 5}, {"op", "plain"}});
        h.send({{"id", 6}, {"op", "fail"}});
        h.send({{"id", 7}, {"op", "ping"}});
        QCOMPARE(h.reply(1).value("echo").toString(), QString("hi"));
        QCOMPARE(h.reply("abc").value("echo").toString(), QString("there"));
        const QJsonObject un = h.reply(3);
        QCOMPARE(un.value("ok").toBool(true), false);
        QVERIFY(un.value("unsupported").toBool());
        QCOMPARE(un.value("error").toString(), QString("unsupported: op nosuch"));
        QVERIFY(h.reply(4).value("error").toString().contains("boom"));
        QCOMPARE(h.reply(5).value("ok").toBool(), true);
        QCOMPARE(h.reply(5).value("value").toInt(), 5);
        QCOMPARE(h.reply(6).value("ok").toBool(true), false);
        const QJsonObject pong = h.reply(7);
        QVERIFY(pong.value("pong").toBool());
        QCOMPARE(qint64(pong.value("pid").toDouble()), QCoreApplication::applicationPid());
        int bad = 0;
        for (const QJsonObject& l : h.pipe->lines()) if (l.value("id").isNull() && l.value("error").toString().startsWith("bad request")) ++bad;
        QCOMPARE(bad, 2);
        for (const QByteArray& b : h.pipe->raw()) { QVERIFY(b.endsWith('\n')); QCOMPARE(int(b.count('\n')), 1); }
        h.pipe->close();                                                        // EOF: exit 0
        QVERIFY(h.waitExit(5000));
        QCOMPARE(h.code->load(), 0);
    }

    void serveTimesOutAndDropsLateResults() {
        Harness h;
        ppcli::ServeLoop::Options o; o.workers = 1; o.defaultTimeoutMs = 250; o.hangGraceMs = 3000;
        h.start(o);
        QElapsedTimer t; t.start();
        h.send({{"id", 1}, {"op", "sleep"}, {"ms", 3000}, {"coop", true}, {"timeoutMs", 150}});
        const QJsonObject r1 = h.reply(1);
        const qint64 at = t.elapsed();
        QVERIFY(r1.value("timeout").toBool());
        QCOMPARE(r1.value("ok").toBool(true), false);
        QVERIFY2(at >= 140 && at < 700, qPrintable(QString("timeout reply after %1 ms").arg(at)));
        QVERIFY(waitIdle(2000));                                                 // the handler saw ctx.cancelled()

        // A handler that ignores cancellation: timeout reply at the default timeout, its late result never written.
        h.send({{"id", 2}, {"op", "sleep"}, {"ms", 700}});
        QVERIFY(h.reply(2).value("timeout").toBool());
        QVERIFY(waitIdle(3000));
        std::this_thread::sleep_for(100ms);
        QCOMPARE(h.repliesFor(1), 1);
        QCOMPARE(h.repliesFor(2), 1);

        // still serving; a request's own timeoutMs beats the default
        h.send({{"id", 3}, {"op", "sleep"}, {"ms", 400}, {"timeoutMs", 5000}});
        QCOMPARE(h.reply(3).value("slept").toInt(), 400);
        h.send({{"id", 4}, {"op", "echo"}, {"text", "after"}});
        QCOMPARE(h.reply(4).value("echo").toString(), QString("after"));
        h.pipe->close();
        QVERIFY(h.waitExit(5000));
        QCOMPARE(h.code->load(), 0);
    }

    void serveRunsWorkersInParallel() {
        Harness h;
        ppcli::ServeLoop::Options o; o.workers = 3;
        h.start(o);
        QElapsedTimer t; t.start();
        for (int i = 1; i <= 3; ++i) h.send({{"id", i}, {"op", "sleep"}, {"ms", 600}});
        for (int i = 1; i <= 3; ++i) QCOMPARE(h.reply(i).value("slept").toInt(), 600);
        QVERIFY2(t.elapsed() < 1500, qPrintable(QString("3 x 600 ms on 3 workers took %1 ms").arg(t.elapsed())));
    }

    void serveEventsAndBusy() {
        Harness h;
        ppcli::ServeLoop::Options o; o.workers = 1; o.maxPending = 2;
        h.start(o);
        h.send({{"id", 9}, {"op", "progress"}});
        QCOMPARE(h.reply(9).value("ok").toBool(), true);
        QList<double> done;
        for (const QJsonObject& l : h.pipe->lines())
            if (l.value("id") == QJsonValue(9) && l.value("event") == QJsonValue("progress")) { done << l.value("done").toDouble(); QCOMPARE(l.value("total").toDouble(), 3.0); }
        QCOMPARE(done, QList<double>({1, 2, 3}));
        h.send({{"id", 1}, {"op", "sleep"}, {"ms", 300}});
        h.send({{"id", 2}, {"op", "sleep"}, {"ms", 300}});
        h.send({{"id", 3}, {"op", "sleep"}, {"ms", 300}});
        QVERIFY(h.reply(3).value("busy").toBool());
        QCOMPARE(h.reply(1).value("ok").toBool(), true);
        QCOMPARE(h.reply(2).value("ok").toBool(), true);
    }

    void serveQuitAndIdleExit() {
        {
            Harness h;
            h.start({});
            h.send({{"id", 1}, {"op", "sleep"}, {"ms", 300}});
            h.send({{"id", 2}, {"op", "quit"}});
            h.send({{"id", 3}, {"op", "echo"}, {"text", "late"}});
            QVERIFY(h.reply(2).value("bye").toBool());
            QCOMPARE(h.reply(3).value("error").toString(), QString("shutting down"));
            QVERIFY(h.waitExit(5000));                                          // after the in-flight sleep finished
            QCOMPARE(h.code->load(), 0);
            QCOMPARE(h.reply(1, 0).value("slept").toInt(), 300);
        }
        {
            Harness h;
            ppcli::ServeLoop::Options o; o.idleExitMs = 250;
            QElapsedTimer t; t.start();
            h.start(o);
            QVERIFY(h.waitExit(5000));
            QCOMPARE(h.code->load(), 0);
            QVERIFY2(t.elapsed() >= 240, qPrintable(QString("idle exit after %1 ms").arg(t.elapsed())));
        }
    }

    void serveExitsWhenAHandlerHangs() {
        Harness h;
        ppcli::ServeLoop::Options o; o.workers = 1; o.hangGraceMs = 300;
        h.start(o);
        QElapsedTimer t; t.start();
        h.send({{"id", 1}, {"op", "sleep"}, {"ms", 1500}, {"timeoutMs", 100}});    // ignores cancellation
        h.send({{"id", 2}, {"op", "echo"}, {"text", "queued behind it"}});
        QVERIFY(h.reply(1).value("timeout").toBool());
        QVERIFY(h.waitExit(5000));
        QCOMPARE(h.code->load(), 2);
        QVERIFY2(t.elapsed() < 1400, qPrintable(QString("hang detected after %1 ms").arg(t.elapsed())));
        const QJsonObject r2 = h.reply(2, 0);
        QVERIFY(r2.value("restart").toBool());
        QCOMPARE(r2.value("ok").toBool(true), false);
        QVERIFY(waitIdle(5000));                                                 // let the detached worker finish
        std::this_thread::sleep_for(50ms);
        QCOMPARE(h.repliesFor(1), 1);
    }

    // ------------------------------------------------------------------ --serve over real stdio
    void serveChildProcess() {
        {
            Child c;
            QVERIFY(c.start({"--serve-child"}));
            c.send({{"id", 1}, {"op", "echo"}, {"text", "über"}});
            QCOMPARE(c.reply(1).value("echo").toString(), QString("über"));
            c.send({{"id", 2}, {"op", "sleep"}, {"ms", 5000}, {"coop", true}, {"timeoutMs", 100}});
            QVERIFY(c.reply(2).value("timeout").toBool());
            c.send({{"id", 3}, {"op", "ping"}});
            QCOMPARE(qint64(c.reply(3).value("pid").toDouble()), c.p.processId());
            c.send({{"id", 4}, {"op", "quit"}});
            QVERIFY(c.reply(4).value("bye").toBool());
            QVERIFY(c.p.waitForFinished(5000));                                 // the stdin reader was cancelled, not waited for
            QCOMPARE(c.p.exitStatus(), QProcess::NormalExit);
            QCOMPARE(c.p.exitCode(), 0);
        }
        {
            Child c;
            QVERIFY(c.start({"--serve-child"}));
            c.send({{"id", 1}, {"op", "sleep"}, {"ms", 200}});
            c.p.closeWriteChannel();                                            // EOF: finish the work, then exit 0
            QCOMPARE(c.reply(1).value("slept").toInt(), 200);
            QVERIFY(c.p.waitForFinished(5000));
            QCOMPARE(c.p.exitCode(), 0);
        }
        {
            Child c;
            QVERIFY(c.start({"--serve-child", "--workers", "1", "--hang-grace-ms", "300"}));
            QElapsedTimer t; t.start();
            c.send({{"id", 1}, {"op", "sleep"}, {"ms", 20000}, {"timeoutMs", 100}});
            QVERIFY(c.reply(1).value("timeout").toBool());
            QVERIFY(c.p.waitForFinished(5000));                                 // hard exit, the stuck thread notwithstanding
            QCOMPARE(c.p.exitCode(), 2);
            QVERIFY2(t.elapsed() < 4000, qPrintable(QString("hung child exited after %1 ms").arg(t.elapsed())));
            QVERIFY(c.p.readAllStandardError().contains("exiting so the client restarts"));
        }
    }

    // ------------------------------------------------------------------ WAV I/O
    void wavRoundTrip() {
        wavio::Wav w; w.sampleRate = 48000; w.channels = 2;
        quint32 s = 7u;
        for (int i = 0; i < 48000; ++i) { s = s * 1103515245u + 12345u; w.samples.push_back(qint16(s >> 16)); }
        const QByteArray b = wavio::serialize(w);
        QCOMPARE(int(b.size()), 44 + 48000 * 2);
        QCOMPARE(b.left(4), QByteArray("RIFF"));
        QCOMPARE(b.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint32>(b.constData() + 4), quint32(b.size() - 8));
        QCOMPARE(qFromLittleEndian<quint32>(b.constData() + 28), quint32(48000 * 4));
        wavio::Wav r; QString err;
        QVERIFY2(wavio::parse(b, &r, &err), qPrintable(err));
        QCOMPARE(r.sampleRate, 48000); QCOMPARE(r.channels, 2); QVERIFY(r.isPcm16());
        QVERIFY(r.samples == w.samples);
        QCOMPARE(r.frames(), qint64(24000));
        QCOMPARE(r.durationSec(), 0.5);

        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString p = dir.filePath("rt.wav");
        QVERIFY2(wavio::writeFile(p, w, &err), qPrintable(err));
        wavio::Wav f; QVERIFY(wavio::readFile(p, &f, &err));
        QVERIFY(f.samples == w.samples);
        int rate = 0, ch = 0; qint64 frames = 0;
        QVERIFY(wavio::info(p, &rate, &ch, &frames, &err));
        QCOMPARE(rate, 48000); QCOMPARE(ch, 2); QCOMPARE(frames, qint64(24000));
        QCOMPARE(wavio::fileDurationSec(p, &rate), 0.5);
        QCOMPARE(wavio::fileDurationSec(dir.filePath("missing.wav")), 0.0);

        const std::vector<float> fl = wavio::toFloat(w);
        const wavio::Wav back = wavio::fromFloat(fl.data(), fl.size(), 48000, 2);
        int maxDiff = 0;
        for (size_t i = 0; i < w.samples.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(w.samples[i]) - int(back.samples[i])));
        QVERIFY(maxDiff <= 1);                                                  // /32768 then *32767: at most one step
        const float loud[4] = {2.0f, -1.0f, 0.5f, 0.0f};
        QCOMPARE(int(wavio::fromFloat(loud, 4, 24000, 1, wavio::FloatMode::Clip).samples[0]), 32767);
        QCOMPARE(int(wavio::fromFloat(loud, 4, 24000, 1, wavio::FloatMode::PeakScale).samples[2]), 8191);   // 0.25 * 32767, truncated
    }

    void wavParseVariants() {
        auto le16 = [](QByteArray& b, int v) { char c[2]; qToLittleEndian<quint16>(quint16(v), c); b.append(c, 2); };
        auto le32 = [](QByteArray& b, quint32 v) { char c[4]; qToLittleEndian<quint32>(v, c); b.append(c, 4); };
        auto fmt = [&](int tag, int ch, int rate, int bits, bool ext) {
            QByteArray f;
            le16(f, ext ? 0xFFFE : tag); le16(f, ch); le32(f, quint32(rate)); le32(f, quint32(rate * ch * bits / 8)); le16(f, ch * bits / 8); le16(f, bits);
            if (ext) { le16(f, 22); le16(f, bits); le32(f, 0); le16(f, tag); f.append(QByteArray(14, '\x01')); }
            QByteArray c("fmt "); le32(c, quint32(f.size())); return c + f;
        };
        auto chunk = [&](const char* id, const QByteArray& body, quint32 sizeField = 0xFFFFFFFEu) {
            QByteArray c(id, 4); le32(c, sizeField == 0xFFFFFFFEu ? quint32(body.size()) : sizeField); c += body; if (body.size() & 1) c += '\0'; return c;
        };
        auto riff = [&](const QByteArray& chunks) { QByteArray b("RIFF"); le32(b, quint32(4 + chunks.size())); return b + "WAVE" + chunks; };
        wavio::Wav w; QString err;

        // 24-bit with an odd-sized LIST chunk (padded) before the data
        QByteArray d24; for (int v : {0x123456, -0x123456, 0x7FFFFF}) { const quint32 u = quint32(v) & 0xFFFFFF; d24 += char(u & 0xFF); d24 += char((u >> 8) & 0xFF); d24 += char((u >> 16) & 0xFF); }
        QVERIFY2(wavio::parse(riff(fmt(1, 1, 44100, 24, false) + chunk("LIST", "abc") + chunk("data", d24)), &w, &err), qPrintable(err));
        QCOMPARE(w.samples.size(), size_t(3));
        QCOMPARE(int(w.samples[0]), 0x1234); QCOMPARE(int(w.samples[1]), -0x1235); QCOMPARE(int(w.samples[2]), 0x7FFF);
        QCOMPARE(w.sourceBits, 24); QVERIFY(!w.isPcm16());

        // float32 in WAVE_FORMAT_EXTENSIBLE, data before fmt
        QByteArray df; for (float v : {0.5f, -1.0f, 2.0f, 0.0f}) { char c[4]; std::memcpy(c, &v, 4); df.append(c, 4); }
        QVERIFY2(wavio::parse(riff(chunk("data", df) + fmt(3, 2, 24000, 32, true)), &w, &err), qPrintable(err));
        QCOMPARE(w.channels, 2); QCOMPARE(w.frames(), qint64(2)); QVERIFY(w.sourceFloat);
        QCOMPARE(int(w.samples[0]), 16383); QCOMPARE(int(w.samples[1]), -32767); QCOMPARE(int(w.samples[2]), 32767);

        // 8-bit; streamed size (0xFFFFFFFF); cut-off data
        QVERIFY(wavio::parse(riff(fmt(1, 1, 8000, 8, false) + chunk("data", QByteArray("\x80\xFF\x00", 3))), &w, &err));
        QCOMPARE(int(w.samples[0]), 0); QCOMPARE(int(w.samples[1]), 127 * 256); QCOMPARE(int(w.samples[2]), -32768);
        QByteArray d16; le16(d16, 100); le16(d16, -100); le16(d16, 7);
        QVERIFY(wavio::parse(riff(fmt(1, 1, 16000, 16, false) + chunk("data", d16, 0xFFFFFFFFu)), &w, &err));
        QCOMPARE(w.samples.size(), size_t(3));
        QByteArray cut = riff(fmt(1, 1, 16000, 16, false) + chunk("data", d16)); cut.chop(3);
        QVERIFY(wavio::parse(cut, &w, &err));
        QCOMPARE(w.samples.size(), size_t(1));

        QVERIFY(!wavio::parse("hello world, not a wav", &w, &err));
        QVERIFY(!wavio::parse(riff(fmt(1, 1, 16000, 16, false)), &w, &err));
        QCOMPARE(err, QString("no data chunk"));
        QVERIFY(!wavio::parse(riff(fmt(2, 1, 16000, 4, false) + chunk("data", d16)), &w, &err));     // ADPCM
        QVERIFY(err.startsWith("unsupported WAV format"));
        QVERIFY(!wavio::parse(riff(fmt(1, 0, 16000, 16, false) + chunk("data", d16)), &w, &err));
    }

    // ------------------------------------------------------------------ levelling
    void trimKeeps80msMargins() {
        wavio::Wav w = burst(48000, 1, 0.5, 1.0, 1.0, 8000);
        for (size_t i = 0; i < w.samples.size(); i += 7) if (w.samples[i] == 0) w.samples[i] = qint16((i % 2) ? 200 : -200);   // noise under -42 dBFS (260)
        qint64 first = -1, last = -1;
        for (qint64 i = 0; i < w.frames(); ++i) if (std::abs(int(w.samples[size_t(i)])) > 260) { if (first < 0) first = i; last = i; }
        QCOMPARE(first, qint64(24000));
        const std::vector<qint16> orig = w.samples;
        QVERIFY(wavio::trim(w));
        const qint64 keep = 3840;                                               // 80 ms at 48 kHz
        QCOMPARE(w.frames(), last - first + 1 + 2 * keep);
        QVERIFY(std::equal(w.samples.begin(), w.samples.end(), orig.begin() + (first - keep)));
        QCOMPARE(int(w.samples[size_t(keep)]), 8000);                          // the first loud sample sits 80 ms in
        QVERIFY(!wavio::trim(w));                                               // already trimmed

        wavio::Wav st = burst(44100, 2, 0.3, 0.5, 0.3, 0.0);
        for (qint64 i = qint64(0.3 * 44100); i < qint64(0.8 * 44100); ++i) st.samples[size_t(i * 2 + 1)] = 5000;   // right channel only
        QVERIFY(wavio::trim(st));
        QCOMPARE(st.frames(), qint64(0.5 * 44100) + 2 * 3528);

        wavio::Wav silent = burst(48000, 1, 1.0, 0.0, 0.0, 0.0);
        QVERIFY(!wavio::trim(silent));
        QCOMPARE(silent.frames(), qint64(48000));
        wavio::Wav full = burst(48000, 1, 0.0, 1.0, 0.0, 8000);
        QVERIFY(!wavio::trim(full));
    }

    void normalizeHitsMinus19() {
        for (int ch : {1, 2}) {
            for (double level : {-35.0, -10.0, -27.0}) {
                wavio::Wav w = burst(48000, ch, 0.5, 1.0, 0.5, ampForRmsDb(level));
                const double gain = wavio::normalize(w);
                QVERIFY2(std::fabs(gain - (-19.0 - level)) < 0.3, qPrintable(QString("gain %1 for %2 dB").arg(gain).arg(level)));
                const double got = wavio::voicedRmsDb(w);
                QVERIFY2(std::fabs(got + 19.0) <= 0.3, qPrintable(QString("%1 ch, %2 dB in -> %3 dB out").arg(ch).arg(level).arg(got)));
                QVERIFY(wavio::peakDbfs(w) <= -1.5);
            }
        }
        wavio::Wav near = burst(48000, 1, 0.2, 1.0, 0.2, ampForRmsDb(-19.2));
        const std::vector<qint16> before = near.samples;
        QCOMPARE(wavio::normalize(near), 0.0);                                  // under 0.5 dB: untouched
        QVERIFY(near.samples == before);
        wavio::Wav quiet = burst(48000, 1, 0.2, 1.0, 0.2, ampForRmsDb(-46.0));
        QCOMPARE(wavio::normalize(quiet), 24.0);                                // capped at +24 dB
        QVERIFY(std::fabs(wavio::voicedRmsDb(quiet) - (-22.0)) < 0.3);
        wavio::Wav silent = burst(48000, 1, 1.0, 0.0, 0.0, 0.0);
        QCOMPARE(wavio::normalize(silent), 0.0);                                // nothing voiced (all blocks under -50 dBFS)
    }

    void limiterNeverExceedsMinus1_5dBFS() {
        const double ceiling = std::pow(10.0, -1.5 / 20.0);
        // speech at about -30 dB voiced RMS with full-scale-ish clicks: +11 dB of gain would clip them without the limiter
        wavio::Wav w = speechLike(48000, 0.6);
        const double gain = wavio::normalize(w);
        QVERIFY2(gain > 5.0, qPrintable(QString("gain %1").arg(gain)));
        QVERIFY2(maxAbs(w) <= int(ceiling * 32767.0), qPrintable(QString("peak %1 dBFS").arg(wavio::peakDbfs(w))));
        QVERIFY(wavio::peakDbfs(w) <= -1.5);
        QVERIFY(std::fabs(wavio::voicedRmsDb(w) + 19.0) < 1.0);                 // limiting moves the voiced level only slightly

        // the limiter itself, on random spikes, at several ceilings and rates (sample-exact bound, incl. block edges)
        quint32 s = 99u;
        auto rnd = [&] { s = s * 1664525u + 1013904223u; return double(s >> 8) / double(1u << 24); };
        for (int rate : {8000, 48000, 96000}) {
            for (double c : {0.3, 0.84, 0.95}) {
                std::vector<double> y(size_t(rate / 2));
                for (double& v : y) v = (rnd() - 0.5) * 0.4 * (rnd() < 0.01 ? 6.0 : 1.0);
                const std::vector<double> x = y;
                wavio::limitPeaks(y.data(), y.size(), rate, c);
                double peak = 0.0;
                for (size_t i = 0; i < y.size(); ++i) { peak = std::max(peak, std::fabs(y[i])); QVERIFY(std::fabs(y[i]) <= std::fabs(x[i]) + 1e-12); }
                QVERIFY2(peak <= c + 1e-12, qPrintable(QString("rate %1 ceiling %2 peak %3").arg(rate).arg(c).arg(peak)));
            }
        }
        // a peak on the last sample of its 2 ms block with quiet after it: spot_tts.py's limit_peaks lets this through
        // 0.35 dB over the ceiling (checked against Python 3.11 + numpy here); wavio's gain cap holds it
        std::vector<double> e(4800, 0.01);
        e[10 * 96 + 95] = 3.0;
        wavio::limitPeaks(e.data(), e.size(), 48000, ceiling);
        QVERIFY2(std::fabs(e[10 * 96 + 95]) <= ceiling + 1e-12, qPrintable(QString("end-of-block peak %1").arg(e[10 * 96 + 95])));
        // released ~200 ms after the last spike: the gain is back near 1
        std::vector<double> y(48000, 0.1);
        y[1000] = 2.0;
        wavio::limitPeaks(y.data(), y.size(), 48000, ceiling);
        QVERIFY(std::fabs(y[1000]) <= ceiling + 1e-12);
        QVERIFY(y[1000 + 9600] > 0.095);
    }

    void fileLevellingInPlace() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString p = dir.filePath("voice.wav");
        QString err;
        QVERIFY(wavio::writeFile(p, burst(48000, 1, 1.0, 1.5, 1.0, ampForRmsDb(-30.0)), &err));
        QVERIFY(wavio::trimFile(p, -42.0, 80, &err));
        QVERIFY(std::fabs(wavio::fileDurationSec(p) - (1.5 + 0.16)) < 0.001);
        QVERIFY(!wavio::trimFile(p));                                           // nothing left to trim: file not rewritten
        const double g = wavio::normalizeFile(p);
        QVERIFY(std::fabs(g - 11.0) < 0.3);
        wavio::Wav w; QVERIFY(wavio::readFile(p, &w, &err));
        QVERIFY(std::fabs(wavio::voicedRmsDb(w) + 19.0) <= 0.3);
        QCOMPARE(wavio::normalizeFile(p), 0.0);                                 // already at -19

        // non-16-bit files are left alone, as spot_tts.py does
        QByteArray b24 = wavio::serialize(burst(48000, 1, 0.5, 0.5, 0.5, 8000));
        qToLittleEndian<quint16>(24, b24.data() + 34);
        qToLittleEndian<quint16>(3, b24.data() + 32);
        const QString p24 = dir.filePath("v24.wav");
        { QFile f(p24); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(b24); }
        QVERIFY(!wavio::trimFile(p24));
        QCOMPARE(wavio::normalizeFile(p24), 0.0);
        QFile f24(p24); QVERIFY(f24.open(QIODevice::ReadOnly)); QCOMPARE(f24.readAll(), b24);
    }

    // ------------------------------------------------------------------ toolswitch (DEP-13 engine switches)
    void toolSwitchModes() {
        using toolswitch::Mode;
        QCOMPARE(toolswitch::parseMode("native"), Mode::Native);
        QCOMPARE(toolswitch::parseMode(" AUTO "), Mode::Auto);
        for (const char* v : {"", "python", "ffmpeg", "script", "natve", "on", "true"}) QCOMPARE(toolswitch::parseMode(v), Mode::Legacy);   // a typo never turns a tool on
        QCOMPARE(toolswitch::modeName(Mode::Legacy, "ffmpeg"), QString("ffmpeg"));
        QCOMPARE(toolswitch::modeName(Mode::Auto, "python"), QString("auto"));
        for (const char* v : {"", "auto", "Native", "python", "ffmpeg", "script"}) QVERIFY(toolswitch::validModeValue(v));
        for (const char* v : {"natve", "kokoro", "1"}) QVERIFY(!toolswitch::validModeValue(v));
    }
    void toolSwitchNativeArgs() {
        // exactly what SpotGenerator / TranscriptionManager build for the scripts, and what the tools take
        QCOMPARE(toolswitch::nativeArgs("card", {"-I", "C:/x/scripts/spot_card.py", "--spec", "C:\\w\\spec.json", "--out", "C:\\w\\furniture.png", "--bed", "C:\\w\\bed.png"}),
                 QStringList({"card", "--spec", "C:\\w\\spec.json", "--out", "C:\\w\\furniture.png", "--bed", "C:\\w\\bed.png"}));
        QCOMPARE(toolswitch::nativeArgs("card", {"-I", "C:/x/scripts/spot_card.py", "--scenes", "s.json"}), QStringList({"card", "--scenes", "s.json"}));
        QCOMPARE(toolswitch::nativeArgs("tts", {"-I", "D:/s/spot_tts.py", "--batch", "v.json", "--voice", "af_heart", "--speed", "1", "--engine", "auto", "--kokoro-dir", "K"}),
                 QStringList({"tts", "--batch", "v.json", "--voice", "af_heart", "--speed", "1", "--engine", "auto", "--kokoro-dir", "K"}));
        QCOMPARE(toolswitch::nativeArgs("music", {"-I", "s/SPOT_MUSIC.PY", "--out", "m.wav", "--seed", "-5"}), QStringList({"music", "--out", "m.wav", "--seed", "-5"}));
        QCOMPARE(toolswitch::nativeArgs("asr", {"-I", "-u", "t/transcribe.py", "--input", "a.mp4", "--level", "fast"}), QStringList({"asr", "--input", "a.mp4", "--level", "fast"}));
        // an ffmpeg command line has no script: it passes through whole (the MediaTool takes ffmpeg's arguments without a sub-command)
        const QStringList ff = {"-y", "-nostdin", "-v", "error", "-i", "bed.png", "-loop", "1", "-i", "f.png", "-filter_complex", "[0:v]null[v]", "out.mp4"};
        QCOMPARE(toolswitch::nativeArgs("", ff), ff);
        QCOMPARE(toolswitch::nativeArgs("graph", ff), QStringList({"graph"}) + ff);
    }
    void toolSwitchFallbackRule() {
        QVERIFY(!toolswitch::shouldFallBack(0, true));                              // native made it
        QVERIFY(toolswitch::shouldFallBack(0, false));                              // exit 0 but no output / {ok:false}
        QVERIFY(toolswitch::shouldFallBack(64, false));                             // unsupported
        QVERIFY(toolswitch::shouldFallBack(2, false));                              // a failure the script may still survive
        QVERIFY(toolswitch::shouldFallBack(-1, false));                             // crash (e.g. no platform plugin for card)
        QVERIFY(toolswitch::shouldFallBack(toolswitch::kFailedToStart, false));
        QVERIFY(!toolswitch::shouldFallBack(toolswitch::kTimedOut, false));         // the step's budget is spent: no second run
        QVERIFY(toolswitch::expectedFallBack(64) && toolswitch::expectedFallBack(toolswitch::kFailedToStart) && !toolswitch::expectedFallBack(2));
        // QProcess::finished -> the code the rule judges: a crash exit falls back (unless the Supervisor itself killed the run)
        QCOMPARE(toolswitch::exitCode(true, 64), 64);
        QCOMPARE(toolswitch::exitCode(true, 0), 0);
        QCOMPARE(toolswitch::exitCode(false, int(0xC0000135)), toolswitch::kCrashed);              // STATUS_DLL_NOT_FOUND: Qt reports CrashExit
        QVERIFY(toolswitch::shouldFallBack(toolswitch::exitCode(false, int(0xC0000135)), true));
        QVERIFY(!toolswitch::expectedFallBack(toolswitch::kCrashed));                               // worth a warning
        QCOMPARE(toolswitch::exitCode(false, 0xf291, true), toolswitch::kTimedOut);                   // cancel()'s kill: final, no second run
        QVERIFY(!toolswitch::shouldFallBack(toolswitch::exitCode(false, 0xf291, true), true));
        QCOMPARE(toolswitch::lastJsonLine("noise\n{\"ok\":true,\"a\":1}\n\n").value("a").toInt(), 1);
        QVERIFY(toolswitch::lastJsonLine("PROGRESS 1 2\nnot json").isEmpty());
        QVERIFY(toolswitch::lastJsonLine("").isEmpty());
    }
    void toolSwitchFindAndProbe() {
        QTemporaryDir a, b; QVERIFY(a.isValid() && b.isValid());
        QCOMPARE(toolswitch::findTool("PlayPlaySpotRender", {a.path(), b.path()}), QString());
        // the fake tool is this test exe under the tool's name: `version` prints {ok, commands} from PP_FAKE_TOOL_COMMANDS
        const QString exeB = QDir(b.path()).filePath("PlayPlaySpotRender.exe");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), exeB));
        QCOMPARE(toolswitch::findTool("PlayPlaySpotRender", {"", a.path(), b.path()}), QDir::toNativeSeparators(exeB));
        QCOMPARE(toolswitch::findTool("PlayPlaySpotRender.exe", {b.path()}), QDir::toNativeSeparators(exeB));
        const QString exeA = QDir(a.path()).filePath("PlayPlaySpotRender.exe");
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), exeA));
        QCOMPARE(toolswitch::findTool("PlayPlaySpotRender", {a.path(), b.path()}), QDir::toNativeSeparators(exeA));   // the configured dir wins

        qputenv("PP_FAKE_TOOL_COMMANDS", "card,scenes,music,version");
        QObject ctx; toolswitch::CapsCache cache; int changed = 0;
        const toolswitch::Caps& first = cache.get(&ctx, exeA, [&]() { ++changed; });
        QVERIFY(first.running && !first.probed);
        // while the probe runs, auto keeps the old program; native does not need the probe
        QVERIFY(!toolswitch::choose(toolswitch::Mode::Auto, exeA, first, "card", "PlayPlaySpotRender").native);
        QVERIFY(toolswitch::choose(toolswitch::Mode::Native, exeA, first, "card", "PlayPlaySpotRender").native);
        QVERIFY(!toolswitch::choose(toolswitch::Mode::Legacy, exeA, first, "card", "PlayPlaySpotRender").native);
        QTRY_COMPARE_WITH_TIMEOUT(changed, 1, 15000);
        const toolswitch::Caps& c = cache.get(&ctx, exeA);
        QVERIFY(c.probed && c.ok);
        QCOMPARE(c.commands, QStringList({"card", "scenes", "music", "version"}));
        QCOMPARE(changed, 1);                                                       // answered from the cache: no second probe
        const toolswitch::Choice yes = toolswitch::choose(toolswitch::Mode::Auto, exeA, c, "card", "PlayPlaySpotRender");
        QVERIFY(yes.native); QCOMPARE(yes.exe, exeA); QVERIFY(yes.reason.isEmpty());
        const toolswitch::Choice no = toolswitch::choose(toolswitch::Mode::Auto, exeA, c, "graph", "PlayPlayMediaTool");
        QVERIFY(!no.native); QVERIFY(no.reason.contains("no 'graph' command"));
        const toolswitch::Choice gone = toolswitch::choose(toolswitch::Mode::Auto, QString(), c, "card", "PlayPlaySpotRender");
        QVERIFY(!gone.native); QCOMPARE(gone.reason, QString("PlayPlaySpotRender not found"));

        // a tool whose `version` fails: auto stays on the old program and says why
        qputenv("PP_FAKE_TOOL_COMMANDS", "!fail");
        toolswitch::CapsCache bad; int badChanged = 0;
        bad.get(&ctx, exeB, [&]() { ++badChanged; });
        QTRY_COMPARE_WITH_TIMEOUT(badChanged, 1, 15000);
        const toolswitch::Choice nb = toolswitch::choose(toolswitch::Mode::Auto, exeB, bad.get(&ctx, exeB), "card", "PlayPlaySpotRender");
        QVERIFY(!nb.native); QVERIFY(nb.reason.contains("version check failed"));
        // a missing exe answers "could not start" without blocking
        toolswitch::CapsCache none; int noneChanged = 0;
        none.get(&ctx, QDir(a.path()).filePath("Nope.exe"), [&]() { ++noneChanged; });
        QTRY_COMPARE_WITH_TIMEOUT(noneChanged, 1, 15000);
        QVERIFY(!none.get(&ctx, QDir(a.path()).filePath("Nope.exe")).ok);
        qunsetenv("PP_FAKE_TOOL_COMMANDS");
    }

    // DEP-21: gen_qrc.py (the Python generator of resources.qrc) is gone; the hand-edited qrcs of the PlayPlay target
    // are authoritative. This test takes over its job across all of them: resources.qrc (CMakeLists.txt) plus every
    // .qrc a lane fragment adds with target_sources(PlayPlay ...) (Play_Play_Native/ui-buttons.qrc from ux-buttons,
    // which also carries app-level qml/ui files such as Actions.qml). Every <file> exists, no file is listed twice in
    // one qrc or across them (Qt merges the trees, so a double listing is a silent override), and every .qml / .js /
    // qmldir under Play_Play_Native/qml is in one of them. Fallback for a module compiled through a qrc this test does
    // not see: a directory whose qmldir is in none of the parsed qrcs is skipped. The failure text is the lines to add.
    void qrcListsEveryQmlFile() {
        const QDir root(QStringLiteral(PP_SOURCE_DIR));
        QStringList qrcs{"resources.qrc"};
        const QRegularExpression addedQrc("target_sources\\(\\s*PlayPlay\\b[^)]*?([^\\s()]+\\.qrc)", QRegularExpression::DotMatchesEverythingOption);
        QDirIterator frags(root.filePath("cmake"), {"lane-*.cmake"}, QDir::Files);
        while (frags.hasNext()) {
            QFile f(frags.next()); QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
            const QString cm = QString::fromUtf8(f.readAll());
            for (auto it = addedQrc.globalMatch(cm); it.hasNext();) {
                QString p = it.next().captured(1);
                p.replace("${CMAKE_SOURCE_DIR}/", "").replace("${CMAKE_CURRENT_SOURCE_DIR}/", "");
                if (!qrcs.contains(p)) qrcs << p;
            }
        }
        QVERIFY2(qrcs.contains("Play_Play_Native/ui-buttons.qrc"), qPrintable("fragment scan missed ui-buttons.qrc; found: " + qrcs.join(", ")));
        // repo-relative path of every listed file -> the qrc(s) that list it
        QHash<QString, QStringList> listed; QStringList dupes, missing;
        const QRegularExpression entry("<file(?:\\s[^>]*)?>\\s*([^<]+?)\\s*</file>");
        for (const QString& q : qrcs) {
            QFile qrc(root.filePath(q));
            QVERIFY2(qrc.open(QIODevice::ReadOnly), qPrintable(q));
            const QDir base = QFileInfo(qrc).absoluteDir();                     // qrc entries are relative to the qrc's directory
            const QString xml = QString::fromUtf8(qrc.readAll());
            int n = 0;
            for (auto it = entry.globalMatch(xml); it.hasNext(); ++n) {
                const QString p = it.next().captured(1);
                const QString rel = QDir::cleanPath(root.relativeFilePath(base.filePath(p)));
                if (!listed[rel].isEmpty()) dupes << rel + " (" + listed[rel].join(", ") + ", " + q + ")";
                listed[rel] << q;
                if (!QFileInfo(root.filePath(rel)).isFile()) missing << q + ": " + p;
            }
            QVERIFY2(n > 0, qPrintable(q + " has no <file> entries"));         // parsed, not skipped
            qInfo("%s: %d entries", qPrintable(q), n);
        }
        QVERIFY(listed.size() > 50);
        QVERIFY2(dupes.isEmpty(), qPrintable("listed twice (a file belongs in exactly one qrc of the PlayPlay target): " + dupes.join("; ")));
        QVERIFY2(missing.isEmpty(), qPrintable("in a qrc but not on disk: " + missing.join(", ")));
        // Directories that are QML modules of their own (qmldir present, in none of the parsed qrcs) are compiled elsewhere.
        QSet<QString> moduleDirs;
        QDirIterator dirs(root.filePath("Play_Play_Native/qml"), {"qmldir"}, QDir::Files, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            const QString q = root.relativeFilePath(dirs.next());
            if (!listed.contains(q)) moduleDirs.insert(q.left(q.size() - int(qstrlen("qmldir"))));
        }
        QStringList unlisted;
        QDirIterator files(root.filePath("Play_Play_Native/qml"), {"*.qml", "*.js", "qmldir"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString rel = root.relativeFilePath(files.next());
            bool inModule = false;
            for (const QString& m : moduleDirs) if (rel.startsWith(m)) { inModule = true; break; }
            if (inModule || listed.contains(rel)) continue;
            unlisted << QString("    <file alias=\"%1\">%2</file>").arg(rel.mid(int(qstrlen("Play_Play_Native/qml/"))), rel);
        }
        qInfo("%d qrc(s) of PlayPlay, %d files, %d QML module dir(s) compiled elsewhere", int(qrcs.size()), int(listed.size()), int(moduleDirs.size()));
        QVERIFY2(unlisted.isEmpty(), qPrintable("QML files on disk but in no qrc of PlayPlay (add to resources.qrc under prefix /qml, "
                                                "or to the lane qrc that owns them with the path relative to that qrc):\n" + unlisted.join("\n")));
    }

    // Same WAVs through scripts/spot_tts.py (when Python + numpy are on this machine) and through wavio.
    void matchesSpotTtsPy() {
        const QString py = findPythonWithNumpy();
        if (py.isEmpty()) QSKIP("no Python with numpy here (set PLAYPLAY_TEST_PYTHON)");
        const QString scripts = QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("scripts");
        QVERIFY(QFileInfo::exists(QDir(scripts).filePath("spot_tts.py")));
        QTemporaryDir dir; QVERIFY(dir.isValid());
        struct Case { const char* name; wavio::Wav w; bool limiter; };
        QList<Case> cases;
        cases.append({"speech+clicks", speechLike(48000, 0.6), true});
        cases.append({"speech", speechLike(48000, 0.0), false});
        cases.append({"stereo-burst", burst(44100, 2, 0.4, 1.2, 0.6, ampForRmsDb(-28.0)), false});
        cases.append({"kokoro-24k-loud", burst(24000, 1, 0.3, 0.8, 0.3, ampForRmsDb(-12.0)), false});
        for (Case& c : cases) {
            const QString a = dir.filePath(QString(c.name) + "_py.wav"), b = dir.filePath(QString(c.name) + "_cpp.wav");
            QString err;
            QVERIFY(wavio::writeFile(a, c.w, &err)); QVERIFY(wavio::writeFile(b, c.w, &err));
            QProcess q;
            q.start(py, {"-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); import spot_tts as t; t.trim_wav(sys.argv[2]); print(t.normalize_wav(sys.argv[2]))", scripts, a});
            QVERIFY(q.waitForFinished(60000));
            QVERIFY2(q.exitCode() == 0, q.readAllStandardError().constData());
            const double pyGain = q.readAllStandardOutput().trimmed().toDouble();
            wavio::trimFile(b);
            const double cppGain = wavio::normalizeFile(b);
            wavio::Wav wp, wc;
            QVERIFY(wavio::readFile(a, &wp, &err)); QVERIFY(wavio::readFile(b, &wc, &err));
            QCOMPARE(wc.frames(), wp.frames());                                 // trim identical
            QVERIFY2(std::fabs(pyGain - cppGain) < 0.01, qPrintable(QString("%1: gain py %2 cpp %3").arg(c.name).arg(pyGain).arg(cppGain)));
            int off = 0, maxDiff = 0;
            for (size_t i = 0; i < wc.samples.size(); ++i) { const int d = std::abs(int(wc.samples[i]) - int(wp.samples[i])); maxDiff = std::max(maxDiff, d); if (d > 1) ++off; }
            const double offShare = double(off) / double(std::max<size_t>(1, wc.samples.size()));
            qInfo("%s: gain %.3f dB, peak py %.2f / cpp %.2f dBFS, %d samples off by >1 LSB (max %d)", c.name, cppGain, wavio::peakDbfs(wp), wavio::peakDbfs(wc), off, maxDiff);
            if (c.limiter) QVERIFY2(offShare < 0.01, qPrintable(QString("%1: %2 % of samples differ").arg(c.name).arg(offShare * 100)));   // only where the gain cap bites
            else QCOMPARE(off, 0);                                              // no limiter: identical within one LSB
            QVERIFY(std::fabs(wavio::voicedRmsDb(wc) - wavio::voicedRmsDb(wp)) < 0.05);
            QVERIFY(wavio::peakDbfs(wc) <= -1.5);
        }
    }
};

int main(int argc, char** argv) {
    if (argc >= 2 && qstrcmp(argv[1], "--oneshot-child") == 0) return oneshotChild(argc, argv);
    if (argc >= 2 && qstrcmp(argv[1], "--serve-child") == 0) return serveChild(argc, argv);
    if (argc >= 2 && qstrcmp(argv[1], "version") == 0) {          // the fake in-house tool of toolSwitchFindAndProbe
        const QByteArray cmds = qgetenv("PP_FAKE_TOOL_COMMANDS");
        if (cmds == "!fail") { std::fputs("{\"ok\": false, \"error\": \"broken\"}\n", stdout); return 2; }
        QJsonArray a; for (const QByteArray& c : cmds.split(',')) if (!c.isEmpty()) a.append(QString::fromUtf8(c));
        std::fputs((QJsonDocument(QJsonObject{{"ok", true}, {"tool", "fake"}, {"commands", a}}).toJson(QJsonDocument::Compact) + "\n").constData(), stdout);
        return 0;
    }
    QCoreApplication app(argc, argv);
    TestInhouseCommon t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_inhouse_common.moc"
