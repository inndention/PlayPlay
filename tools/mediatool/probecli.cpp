// PlayPlayMediaTool `probe`: see probecli.h. The probing itself is PlayPlayAvKit's (tools/avkit/avprobe.cpp, compared
// with ffprobe.exe by test_inhouse_avkit); this file is the ffprobe command line around it.
#include "probecli.h"

#include "avprobe.h"
#include "ppcli.h"

#include <QRegularExpression>

#include <cstdio>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/log.h>
}

namespace mediatool {
namespace {

QString avErr(int r) { char b[AV_ERROR_MAX_STRING_SIZE] = {0}; av_strerror(r, b, sizeof(b)); return QString::fromUtf8(b); }

// ffprobe.exe writes through a text-mode stdout; ours is binary (ppcli::initStdio), so the CR is added here.
QByteArray nativeLines(QByteArray b) {
#ifdef _WIN32
    b.replace("\n", "\r\n");     // JSON strings never hold a raw '\n' (the writer escapes it)
#endif
    return b;
}

// The default writer's options: "default", "default=nw=1:nk=1", long names, any order; values 0 / 1 only.
bool parseDefaultWriter(const QString& w, bool* nw, bool* nk) {
    *nw = *nk = false;
    if (w == "default") return true;
    if (!w.startsWith("default=")) return false;
    for (const QString& kv : w.mid(8).split(':')) {
        const int eq = kv.indexOf('=');
        if (eq <= 0) return false;
        const QString k = kv.left(eq), v = kv.mid(eq + 1);
        if (v != "0" && v != "1") return false;
        if (k == "nw" || k == "noprint_wrappers") *nw = v == "1";
        else if (k == "nk" || k == "nokey") *nk = v == "1";
        else return false;
    }
    return true;
}

} // namespace

bool parseLogLevel(const QString& v, int* level) {
    static const struct { const char* name; int level; } kLevels[] = {
        {"quiet", AV_LOG_QUIET}, {"panic", AV_LOG_PANIC}, {"fatal", AV_LOG_FATAL}, {"error", AV_LOG_ERROR},
        {"warning", AV_LOG_WARNING}, {"info", AV_LOG_INFO}, {"verbose", AV_LOG_VERBOSE}, {"debug", AV_LOG_DEBUG},
        {"trace", AV_LOG_TRACE}};
    for (const auto& l : kLevels) if (v == QLatin1String(l.name)) { *level = l.level; return true; }
    bool ok = false; const int n = v.toInt(&ok);
    if (ok) *level = n;
    return ok;
}

bool isNonFileInput(const QString& in) {
    if (in == "-" || in.startsWith("subfile,")) return true;               // libav's url_find_protocol special case
    static const QRegularExpression rx("^([A-Za-z0-9+.\\-]{2,}):");           // a one-letter scheme is a drive (is_dos_path)
    const QRegularExpressionMatch m = rx.match(in);
    return m.hasMatch() && m.captured(1).compare("file", Qt::CaseInsensitive) != 0;
}

int oneShotTimeoutMs() {
    bool ok = false; const int v = qEnvironmentVariableIntValue("PLAYPLAY_MEDIATOOL_TIMEOUT_MS", &ok);
    return ok && v > 0 ? v : 120000;
}

ProbeCommand parseProbeArgs(const QStringList& args) {
    ProbeCommand c;
    QString writer;
    bool showEntries = false;
    for (int i = 0; i < args.size() && c.unsupported.isEmpty() && c.error.isEmpty(); ++i) {
        const QString& a = args[i];
        auto value = [&](QString* v) {
            if (i + 1 >= args.size()) { c.error = QString("Missing argument for option '%1'.").arg(a.mid(1)); return false; }
            *v = args[++i];
            return true;
        };
        auto setInput = [&](const QString& in) {
            if (!c.input.isEmpty()) { c.error = QString("Argument '%1' provided as input filename, but '%2' was already specified.").arg(in, c.input); return; }
            if (isNonFileInput(in)) { c.unsupported = "input " + in; return; }
            c.input = in;
        };
        QString v;
        if (a == "-v" || a == "-loglevel") { if (value(&v) && !parseLogLevel(v, &c.logLevel)) c.error = "Invalid loglevel \"" + v + "\""; }
        else if (a == "-hide_banner") {}
        else if (a == "-print_format" || a == "-of" || a == "-output_format") { if (value(&v)) writer = v; }
        else if (a == "-show_format") c.showFormat = true;
        else if (a == "-show_streams") c.showStreams = true;
        else if (a == "-show_entries") { if (value(&v)) { if (v == "format=duration") showEntries = true; else c.unsupported = "-show_entries " + v; } }
        else if (a == "-i") { if (value(&v)) setInput(v); }
        else if (a.size() > 1 && a.startsWith('-')) c.unsupported = a;
        else setInput(a);
    }
    if (!c.unsupported.isEmpty() || !c.error.isEmpty()) return c;
    if (showEntries) {
        c.kind = ProbeCommand::Duration;
        if (c.showFormat || c.showStreams) { c.unsupported = c.showFormat ? "-show_format with -show_entries" : "-show_streams with -show_entries"; return c; }
        if (!parseDefaultWriter(writer.isEmpty() ? QStringLiteral("default") : writer, &c.noWrappers, &c.noKey)) { c.unsupported = "-print_format " + writer + " with -show_entries"; return c; }
    } else {
        c.kind = ProbeCommand::Json;
        if (writer != "json") { c.unsupported = writer.isEmpty() ? QStringLiteral("default writer for -show_format/-show_streams") : "-print_format " + writer; return c; }
        if (!c.showFormat && !c.showStreams) { c.unsupported = "json without -show_format/-show_streams"; return c; }
    }
    if (c.input.isEmpty()) c.error = "You have to specify one input file.";
    return c;
}

ProbeRun runProbe(const ProbeCommand& c, int timeoutMs) {
    avkit::ProbeOptions o;
    o.timeoutMs = timeoutMs;
    if (c.kind == ProbeCommand::Duration) { o.showFormat = true; o.showStreams = false; o.openDecoders = false; }   // decoders change nothing in format{}
    else { o.showFormat = c.showFormat; o.showStreams = c.showStreams; }
    const avkit::ProbeResult r = avkit::probe(c.input, o);
    ProbeRun run;
    if (!r.ok) {
        run.exitCode = 1;
        run.averror = r.averror;
        run.error = (r.timedOut || r.cancelled) ? r.error : c.input + ": " + avErr(r.averror);
    }
    if (c.kind == ProbeCommand::Json) {
        run.out = nativeLines(r.text);                    // "{\n\n}\n" on failure, as ffprobe's json writer leaves it
    } else if (r.ok) {                                    // the default writer prints nothing when the input fails
        QByteArray body = (c.noKey ? QByteArray() : QByteArray("duration=")) + avkit::formatDurationValue(r) + '\n';
        if (!c.noWrappers) body = "[FORMAT]\n" + body + "[/FORMAT]\n";
        run.out = nativeLines(body);
    }
    return run;
}

int probeCli(const QStringList& args) {
    const ProbeCommand c = parseProbeArgs(args);
    if (!c.unsupported.isEmpty()) return ppcli::unsupported(c.unsupported);
    if (!c.error.isEmpty()) return ppcli::fail(c.error);
    av_log_set_level(c.logLevel);                         // libav's own lines (e.g. "moov atom not found") as ffprobe -v shows them
    const ProbeRun r = runProbe(c, oneShotTimeoutMs());
    if (r.exitCode) av_log(nullptr, AV_LOG_ERROR, "%s\n", r.error.toUtf8().constData());
    std::fwrite(r.out.constData(), 1, size_t(r.out.size()), stdout);
    std::fflush(stdout);
    return r.exitCode;
}

} // namespace mediatool
