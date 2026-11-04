// PlayPlayMediaTool `--serve`: see serve.h. The loop (threads, timeouts, ping/quit, hang exit) is ppcli::ServeLoop;
// this file maps the ops onto AvKit's probe and the thumbnailer.
#include "serve.h"

#include "avcaps.h"
#include "avprobe.h"
#include "probecli.h"
#include "thumbnailer.h"
#include "ppcli.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>

#include <algorithm>
#include <atomic>

extern "C" {
#include <libavutil/log.h>
}

namespace mediatool {
namespace {

// Small stable worker numbers for the replies (diagnostics: shows the work really spreads over the threads).
int workerIndex() {
    static std::atomic<int> next{0};
    thread_local const int idx = next.fetch_add(1);
    return idx;
}

QJsonObject probeFailure(const avkit::ProbeResult& r) {
    QJsonObject e = ppcli::failure(r.error);
    e["averror"] = r.averror;
    if (r.timedOut) e["timeout"] = true;
    if (r.cancelled) e["cancelled"] = true;
    return e;
}

QJsonObject run(const QJsonObject& req, int timeoutMs, const std::atomic<bool>* cancel, const ServeConfig& cfg) {
    const QString op = req.value("op").toString();
    if (op == "version")
        return QJsonObject{{"ok", true}, {"tool", "PlayPlayMediaTool"}, {"ops", QJsonArray{"probe", "duration", "thumb", "version", "ping", "quit"}},
                           {"ffmpeg", avkit::versionInfo()}, {"workers", cfg.workers}};
    if (op != "probe" && op != "duration" && op != "thumb") return ppcli::ServeLoop::unsupportedOp(op);
    const QString path = req.value("path").toString();
    if (path.isEmpty()) return ppcli::failure("missing \"path\"");
    if (isNonFileInput(path)) return ppcli::unsupportedResult("input " + path);

    if (op == "probe" || op == "duration") {
        avkit::ProbeOptions po;
        po.timeoutMs = timeoutMs;                 // the probe stops itself about when the loop sends the timeout reply,
        po.cancel = cancel;                       // and at once when the loop cancels the job (timeout, closed stdout)
        if (op == "duration") { po.showStreams = false; po.openDecoders = false; }
        else { po.showFormat = req.value("format").toBool(true); po.showStreams = req.value("streams").toBool(true); }
        if (!po.showFormat && !po.showStreams) return ppcli::failure("nothing to show (format and streams both false)");
        const avkit::ProbeResult r = avkit::probe(path, po);
        if (!r.ok) return probeFailure(r);
        if (op == "probe") return QJsonObject{{"ok", true}, {"probe", r.json}};
        const QByteArray v = avkit::formatDurationValue(r);
        return QJsonObject{{"ok", true}, {"value", QString::fromLatin1(v)}, {"duration", v == "N/A" ? 0.0 : v.toDouble()}};
    }

    // thumb
    const QString out = req.value("out").toString();
    if (out.isEmpty()) return ppcli::failure("missing \"out\"");
    if (isNonFileInput(out)) return ppcli::unsupportedResult("output " + out);
    ThumbOptions o;
    o.seekSec = req.contains("at") ? std::max(0.0, req.value("at").toDouble()) : -1.0;
    o.width = req.value("width").toInt(320);
    o.height = req.value("height").toInt(-2);
    o.qscale = req.value("q").toDouble(4.0);
    if (!(o.qscale >= 1 && o.qscale <= 31)) return ppcli::failure("\"q\" must be 1..31");
    o.overwrite = req.value("overwrite").toBool(true);
    o.threads = cfg.decoderThreads;
    o.timeoutMs = timeoutMs;
    if (cancel) o.cancelled = [cancel] { return cancel->load(std::memory_order_relaxed); };
    return thumbJson(makeThumbnail(path, out, o), out, o);
}

} // namespace

QJsonObject handleRequest(const QJsonObject& req, int timeoutMs, const std::atomic<bool>* cancel, const ServeConfig& cfg) {
    QElapsedTimer t; t.start();
    QJsonObject r = run(req, timeoutMs, cancel, cfg);
    r["ms"] = double(t.elapsed());
    r["worker"] = workerIndex();
    return r;
}

int serveCli(const QStringList& args) {
    const ppcli::Args a(args, {"--workers=", "--timeout-ms=", "--idle-exit-ms=", "--max-pending=", "--hang-grace-ms=", "--threads=", "-v=", "-loglevel="});
    if (!a.ok()) return a.report();
    ServeConfig cfg;
    bool ok1 = true, ok2 = true, ok3 = true, ok4 = true, ok5 = true, ok6 = true;
    cfg.workers = a.integer("--workers", cfg.workers, &ok1);
    cfg.timeoutMs = a.integer("--timeout-ms", cfg.timeoutMs, &ok2);
    cfg.idleExitMs = a.integer("--idle-exit-ms", cfg.idleExitMs, &ok3);
    cfg.maxPending = a.integer("--max-pending", cfg.maxPending, &ok4);
    cfg.hangGraceMs = a.integer("--hang-grace-ms", cfg.hangGraceMs, &ok5);
    cfg.decoderThreads = a.integer("--threads", cfg.decoderThreads, &ok6);
    if (!(ok1 && ok2 && ok3 && ok4 && ok5 && ok6)) return ppcli::fail("serve options take integers");
    if (cfg.workers < 1 || cfg.workers > 32) return ppcli::fail("--workers must be 1..32");
    if (cfg.timeoutMs < 1 || cfg.idleExitMs < 0 || cfg.maxPending < 1 || cfg.hangGraceMs < 0 || cfg.decoderThreads < 1 || cfg.decoderThreads > 16)
        return ppcli::fail("serve option out of range");
    const QString lvl = a.has("-v") ? a.value("-v") : a.value("-loglevel");
    if (!lvl.isEmpty() && !parseLogLevel(lvl, &cfg.logLevel)) return ppcli::fail("Invalid loglevel \"" + lvl + "\"");
    av_log_set_level(cfg.logLevel);

    ppcli::ServeLoop::Options so;
    so.workers = cfg.workers;
    so.defaultTimeoutMs = cfg.timeoutMs;
    so.idleExitMs = cfg.idleExitMs;
    so.maxPending = cfg.maxPending;
    so.hangGraceMs = cfg.hangGraceMs;
    const int maxTimeout = std::max(so.defaultTimeoutMs, so.maxTimeoutMs);
    ppcli::log(QString("PlayPlayMediaTool serve: %1 workers, timeout %2 ms, pid %3").arg(cfg.workers).arg(cfg.timeoutMs).arg(QCoreApplication::applicationPid()));
    return ppcli::ServeLoop(so).run([&cfg, maxTimeout](const QJsonObject& req, ppcli::ServeContext& ctx) {
        const int want = req.value("timeoutMs").toInt(0);       // the same effective timeout the loop arms
        const int timeoutMs = want > 0 ? std::min(want, maxTimeout) : cfg.timeoutMs;
        return handleRequest(req, timeoutMs, ctx.cancelFlag(), cfg);
    });
}

} // namespace mediatool
