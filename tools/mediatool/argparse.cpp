// The ffmpeg-argument subset parser of the PlayPlayMediaTool graph runner (argparse.h, DEP-05a).
#include "argparse.h"
#include "ppcli.h"
#include "probecli.h"      // parseLogLevel, isNonFileInput

#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/avutil.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace mediatool {
namespace {

// ---- value parsers, each as ffmpeg reads the value ------------------------------------------------------------------
bool parseDuration(const QString& v, double* sec) {        // -ss / -t: av_parse_time duration form (1.5, 00:01:02.5, 2s)
    int64_t us = 0;
    if (av_parse_time(&us, v.toUtf8().constData(), 1) < 0 || us < 0) return false;
    *sec = double(us) / AV_TIME_BASE; return true;
}
bool parseRate(const QString& v, double* fps) {            // -r / -framerate: av_parse_video_rate (30, 30000/1001, ntsc)
    AVRational r{};
    if (av_parse_video_rate(&r, v.toUtf8().constData()) < 0 || r.num <= 0 || r.den <= 0) return false;
    *fps = av_q2d(r); return true;
}
bool parseBitrate(const QString& v, qint64* bps) {         // -b:a: a number with ffmpeg's k / M suffix (192k = 192000)
    static const QRegularExpression rx("^(\\d+(?:\\.\\d+)?)([kKmM])?$");
    const QRegularExpressionMatch m = rx.match(v);
    if (!m.hasMatch()) return false;
    double n = m.captured(1).toDouble();
    const QString s = m.captured(2).toLower();
    if (s == "k") n *= 1000; else if (s == "m") n *= 1000000;
    if (n <= 0 || n > 1e12) return false;
    *bps = qint64(n + 0.5); return true;
}
bool parseIntMin(const QString& v, int min, int* out) {
    bool ok = false; const int n = v.toInt(&ok);
    if (!ok || n < min) return false;
    *out = n; return true;
}
bool isX264Preset(const QString& p) {
    static const char* const k[] = {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo"};
    for (const char* s : k) if (p == QLatin1String(s)) return true;
    return false;
}
QString containerOf(const QString& out) {
    const QString ext = QFileInfo(out).suffix().toLower();
    if (ext == "mp4" || ext == "m4v") return "mp4";
    if (ext == "mov") return "mov";
    if (ext == "wav") return "wav";
    if (ext == "jpg" || ext == "jpeg") return "jpg";
    return {};
}

// ---- filtergraph tokeniser (libavfilter/graphparser.c) --------------------------------------------------------------
// av_get_token: leading whitespace skipped, '...' literal, '\' escapes the next char, stops at a terminator outside
// quotes; trailing unescaped whitespace is dropped. Returns the unescaped token; `raw` gets the text as written.
QString getToken(const QString& s, int* pos, const QString& term, QString* raw = nullptr) {
    int i = *pos;
    while (i < s.size() && s[i].isSpace()) ++i;
    const int start = i;
    QString out; int lastNonSpace = 0;
    while (i < s.size() && !term.contains(s[i])) {
        const QChar c = s[i];
        if (c == '\'') {                                   // quoted run, literal until the closing quote
            ++i;
            while (i < s.size() && s[i] != '\'') out += s[i++];
            if (i < s.size()) ++i;
            lastNonSpace = out.size();
        } else if (c == '\\') {
            ++i;
            if (i < s.size()) out += s[i++];
            lastNonSpace = out.size();
        } else {
            out += c; ++i;
            if (!c.isSpace()) lastNonSpace = out.size();
        }
    }
    if (raw) *raw = s.mid(start, i - start).trimmed();
    *pos = i;
    return out.left(lastNonSpace);
}

// "[label]" runs at *pos; false on an unterminated or empty label.
bool parseLabels(const QString& s, int* pos, QStringList* labels, QString* err) {
    int i = *pos;
    for (;;) {
        while (i < s.size() && s[i].isSpace()) ++i;
        if (i >= s.size() || s[i] != '[') break;
        ++i;
        const QString name = getToken(s, &i, "]");
        if (i >= s.size() || s[i] != ']') { if (err) *err = "unterminated link label"; return false; }
        if (name.isEmpty()) { if (err) *err = "empty link label"; return false; }
        ++i; *labels << name;
    }
    *pos = i; return true;
}

}   // namespace

bool tokenizeGraph(const QString& graph, QVector<QVector<FilterNode>>* chains, QString* err) {
    chains->clear();
    QVector<FilterNode> chain;
    int i = 0;
    for (;;) {
        FilterNode f;
        if (!parseLabels(graph, &i, &f.inputs, err)) return false;
        f.name = getToken(graph, &i, "=,;[");
        if (f.name.isEmpty()) { if (err) *err = QString("no filter name at offset %1").arg(i); return false; }
        const int at = f.name.indexOf('@'); if (at > 0) f.name.truncate(at);
        if (i < graph.size() && graph[i] == '=') { ++i; getToken(graph, &i, "[],;", &f.args); }
        if (!parseLabels(graph, &i, &f.outputs, err)) return false;
        chain << f;
        while (i < graph.size() && graph[i].isSpace()) ++i;
        if (i >= graph.size()) break;
        const QChar sep = graph[i++];
        if (sep == ';') { chains->append(chain); chain.clear(); }
        else if (sep != ',') { if (err) *err = QString("unexpected '%1' at offset %2").arg(sep).arg(i - 1); return false; }
        // libavfilter tolerates a trailing separator; so does this
        int j = i; while (j < graph.size() && graph[j].isSpace()) ++j;
        if (j >= graph.size()) { i = j; break; }
    }
    if (!chain.isEmpty()) chains->append(chain);
    return true;
}

QStringList filterNamesIn(const QString& graph) {
    QVector<QVector<FilterNode>> chains; QStringList names;
    if (!tokenizeGraph(graph, &chains)) return names;
    for (const auto& c : chains) for (const FilterNode& f : c) names << f.name;
    return names;
}

bool filterExists(const QString& name) { return avfilter_get_by_name(name.toUtf8().constData()) != nullptr; }

QString GraphOutput::videoEncoder() const {
    if (noVideo || container == "wav") return {};
    if (!videoCodec.isEmpty()) return videoCodec;
    if (copyAll) return "copy";
    return container == "jpg" ? "mjpeg" : "libx264";
}
QString GraphOutput::audioEncoder() const {
    if (noAudio || container == "jpg") return {};
    if (!audioCodec.isEmpty()) return audioCodec;
    if (copyAll) return "copy";
    return container == "wav" ? "pcm_s16le" : "aac";
}

namespace {

struct Opt { QString name, value; bool hasValue = false; };

// The per-file option sets. "both" options (-ss, -t) take the meaning of the file that closes the group.
bool isInputOpt(const QString& n) { return n == "-loop" || n == "-stream_loop" || n == "-framerate" || n == "-f" || n == "-safe"; }
bool isOutputOpt(const QString& n) {
    static const QSet<QString> k = {"-map", "-vf", "-filter:v", "-af", "-filter:a", "-vn", "-an", "-c", "-codec", "-c:v", "-codec:v", "-vcodec",
                                    "-c:a", "-codec:a", "-acodec", "-preset", "-crf", "-g", "-pix_fmt", "-color_range", "-r", "-b:a", "-ab", "-ar", "-ac",
                                    "-movflags", "-avoid_negative_ts", "-frames:v", "-vframes", "-q:v", "-qscale:v"};
    return k.contains(n);
}
bool isBothOpt(const QString& n) { return n == "-ss" || n == "-t"; }
bool takesValue(const QString& n) { return n != "-vn" && n != "-an"; }

struct Parser {
    GraphCommand c;
    const GraphParseOptions& o;
    QVector<Opt> group;                   // per-file options waiting for their -i / output file
    bool sawFilterComplex = false;
    explicit Parser(const GraphParseOptions& opt) : o(opt) {}
    bool fine() const { return c.ok(); }
    void unsupported(const QString& what) { if (fine()) c.unsupported = what; }
    void error(const QString& what) { if (fine()) c.error = what; }

    void applyInput(const QString& path) {
        GraphInput in; in.path = path;
        for (const Opt& g : group) {
            if (isOutputOpt(g.name)) { unsupported(g.name + " before -i (ffmpeg would apply it to the input)"); return; }
            const QString& v = g.value;
            if (g.name == "-ss") { if (!parseDuration(v, &in.seekSec)) error("Invalid duration specification for ss: " + v); }
            else if (g.name == "-t") { if (!parseDuration(v, &in.durationSec)) error("Invalid duration specification for t: " + v); }
            else if (g.name == "-loop") { if (v == "1") in.loop = true; else if (v != "0") unsupported("-loop " + v); }
            else if (g.name == "-stream_loop") { if (v == "-1") in.streamLoop = true; else unsupported("-stream_loop " + v + " (only -1)"); }
            else if (g.name == "-framerate") { if (!parseRate(v, &in.framerate)) error("Invalid framerate: " + v); }
            else if (g.name == "-f") { if (v == "concat") in.concat = true; else unsupported("-f " + v + " (input format)"); }
            else if (g.name == "-safe") { if (v == "0") in.safe0 = true; else unsupported("-safe " + v); }
            if (!fine()) return;
        }
        if (in.safe0 && !in.concat) { unsupported("-safe without -f concat"); return; }
        if (in.loop && in.streamLoop) { unsupported("-loop with -stream_loop on one input"); return; }
        group.clear();
        c.inputs << in;
    }

    void applyOutput(const QString& path) {
        GraphOutput out; out.path = path; out.container = containerOf(path);
        if (out.container.isEmpty()) { unsupported("output " + path + " (extension)"); return; }
        bool sawVf = false, sawAf = false;
        for (const Opt& g : group) {
            if (isInputOpt(g.name)) { unsupported(g.name + " after the last -i (an input option)"); return; }
            const QString& n = g.name; const QString& v = g.value;
            if (n == "-ss") unsupported("-ss after the last -i (an output seek)");
            else if (n == "-t") { if (!parseDuration(v, &out.durationSec)) error("Invalid duration specification for t: " + v); }
            else if (n == "-map") {
                static const QRegularExpression rx("^\\[([^\\[\\]]+)\\]$");
                const QRegularExpressionMatch m = rx.match(v);
                if (!m.hasMatch()) unsupported("-map " + v + " (only a [label] of -filter_complex)"); else out.maps << m.captured(1);
            }
            else if (n == "-vf" || n == "-filter:v") { if (sawVf) unsupported(n + " given twice"); else { out.vf = v; sawVf = true; } }
            else if (n == "-af" || n == "-filter:a") { if (sawAf) unsupported(n + " given twice"); else { out.af = v; sawAf = true; } }
            else if (n == "-vn") out.noVideo = true;
            else if (n == "-an") out.noAudio = true;
            else if (n == "-c" || n == "-codec") { if (v == "copy") out.copyAll = true; else unsupported("-c " + v); }
            else if (n == "-c:v" || n == "-codec:v" || n == "-vcodec") {
                if (v == "libx264" || v == "copy" || v == "mjpeg") out.videoCodec = v; else unsupported(n + " " + v);
            }
            else if (n == "-c:a" || n == "-codec:a" || n == "-acodec") {
                if (v == "aac" || v == "pcm_s16le" || v == "copy") out.audioCodec = v; else unsupported(n + " " + v);
            }
            else if (n == "-preset") { if (isX264Preset(v)) out.preset = v; else unsupported("-preset " + v); }
            else if (n == "-crf") { bool ok = false; const double q = v.toDouble(&ok); if (ok && q >= 0 && q <= 51) out.crf = int(q); else error("-crf " + v + ": expected 0..51"); }
            else if (n == "-g") { if (!parseIntMin(v, 0, &out.gop)) error("-g " + v + ": expected an integer"); }
            else if (n == "-pix_fmt") { if (av_get_pix_fmt(v.toUtf8().constData()) == AV_PIX_FMT_NONE) error("Unknown pixel format requested: " + v); else out.pixFmt = v; }
            else if (n == "-color_range") { if (av_color_range_from_name(v.toUtf8().constData()) < 0) error("Unknown color range: " + v); else out.colorRange = v; }
            else if (n == "-r") { if (parseRate(v, &out.frameRateValue)) out.frameRate = v; else error("Invalid framerate value: " + v); }
            else if (n == "-b:a" || n == "-ab") { if (!parseBitrate(v, &out.audioBitrate)) error("Invalid audio bitrate: " + v); }
            else if (n == "-ar") { if (!parseIntMin(v, 1, &out.sampleRate)) error("Invalid sample rate: " + v); }
            else if (n == "-ac") { if (!parseIntMin(v, 1, &out.channels)) error("Invalid channel count: " + v); }
            else if (n == "-movflags") { if (v == "+faststart" || v == "faststart") out.faststart = true; else unsupported("-movflags " + v); }
            else if (n == "-avoid_negative_ts") { if (v == "make_zero") out.makeZeroTs = true; else unsupported("-avoid_negative_ts " + v); }
            else if (n == "-frames:v" || n == "-vframes") { if (!parseIntMin(v, 1, &out.frames)) error(n + " " + v + ": expected a positive integer"); }
            else if (n == "-q:v" || n == "-qscale:v") { bool ok = false; const double q = v.toDouble(&ok); if (ok && q >= 1 && q <= 31) out.qscale = q; else error(n + " " + v + ": expected 1..31"); }
            if (!fine()) return;
        }
        // Combinations ffmpeg takes but only warns about, or that this runner will not reproduce: refused, not reinterpreted.
        const bool videoCopy = out.copyAll ? out.videoCodec.isEmpty() || out.videoCodec == "copy" : out.videoCodec == "copy";
        if (videoCopy && (!out.preset.isEmpty() || out.crf >= 0 || out.gop >= 0 || !out.pixFmt.isEmpty() || !out.colorRange.isEmpty() || !out.frameRate.isEmpty() || out.frames >= 0 || out.qscale >= 0 || !out.vf.isEmpty()))
            { unsupported("video encoder options with -c:v copy"); return; }
        const bool audioCopy = out.copyAll ? out.audioCodec.isEmpty() || out.audioCodec == "copy" : out.audioCodec == "copy";
        if (audioCopy && (out.audioBitrate > 0 || out.sampleRate > 0 || out.channels > 0 || !out.af.isEmpty())) { unsupported("audio encoder options with -c:a copy"); return; }
        if (out.container == "jpg" && (out.videoEncoder() != "mjpeg" || !out.af.isEmpty() || !c.filterComplex.isEmpty())) { unsupported("output " + path + " (a jpg takes the thumbnail form)"); return; }
        if (out.container == "wav" && (out.videoEncoder() != QString() || !out.vf.isEmpty())) { unsupported("output " + path + " with video options"); return; }
        if (out.faststart && out.container != "mp4" && out.container != "mov") { unsupported("-movflags +faststart for a " + out.container); return; }
        if (!c.filterComplex.isEmpty() && (!out.vf.isEmpty() || !out.af.isEmpty())) { unsupported("-vf / -af together with -filter_complex"); return; }
        if (!out.maps.isEmpty() && c.filterComplex.isEmpty()) { error("Output with label '" + out.maps.first() + "' does not exist in any defined filter graph, or was already used elsewhere."); return; }
        if (out.noVideo && (!out.vf.isEmpty() || !out.videoCodec.isEmpty())) { unsupported("-vn with video options"); return; }
        if (out.noAudio && (!out.af.isEmpty() || !out.audioCodec.isEmpty())) { unsupported("-an with audio options"); return; }
        group.clear();
        c.outputs << out;
    }

    // A simple graph (-vf / -af): one chain, no labels, every filter known.
    void checkSimpleGraph(const QString& which, const QString& g) {
        QVector<QVector<FilterNode>> chains; QString err;
        if (!tokenizeGraph(g, &chains, &err)) { error(which + ": " + err); return; }
        if (chains.size() != 1) { unsupported(which + " with more than one chain"); return; }
        for (const FilterNode& f : chains.first()) {
            if (!f.inputs.isEmpty() || !f.outputs.isEmpty()) { unsupported(which + " with link labels"); return; }
            if (o.checkFilters && !filterExists(f.name)) { unsupported(which + ": filter '" + f.name + "' (not in this libavfilter)"); return; }
        }
    }

    // The complex graph: every filter known, every external input an existing [k], [k:v] or [k:a], every labelled output
    // mapped exactly once, no output left unlabelled (ffmpeg would map it on its own).
    void checkComplexGraph() {
        QVector<QVector<FilterNode>> chains; QString err;
        if (!tokenizeGraph(c.filterComplex, &chains, &err)) { error("-filter_complex: " + err); return; }
        static const QRegularExpression rxIn("^(\\d+)(?::([va]))?$");
        QStringList open;                       // labelled outputs not yet consumed
        for (const auto& chain : chains) {
            for (int k = 0; k < chain.size(); ++k) {
                const FilterNode& f = chain[k];
                if (o.checkFilters && !filterExists(f.name)) { unsupported("-filter_complex: filter '" + f.name + "' (not in this libavfilter)"); return; }
                for (const QString& in : f.inputs) {
                    if (open.removeOne(in)) continue;
                    const QRegularExpressionMatch m = rxIn.match(in);
                    if (!m.hasMatch()) { unsupported("-filter_complex: input label [" + in + "] (only [k], [k:v], [k:a])"); return; }
                    const int idx = m.captured(1).toInt();
                    if (idx >= c.inputs.size()) { error(QString("Invalid file index %1 in filtergraph description").arg(idx)); return; }
                    if (!c.graphInputs.contains(in)) c.graphInputs << in;
                }
                for (const QString& out : f.outputs) {
                    if (open.contains(out)) { error("-filter_complex: output label [" + out + "] defined twice"); return; }
                    open << out;
                }
                if (k == chain.size() - 1 && f.outputs.isEmpty()) { unsupported("-filter_complex: a chain ending in '" + f.name + "' without an output label (ffmpeg would map it on its own)"); return; }
            }
        }
        for (const GraphOutput& out : c.outputs)
            for (const QString& m : out.maps)
                if (!open.removeOne(m)) { error("Output with label '" + m + "' does not exist in any defined filter graph, or was already used elsewhere."); return; }
        if (!open.isEmpty()) {
            // ffmpeg: "Filter <x> has an unconnected output" (fatal) for a labelled, unmapped output
            error("Filter " + open.first() + " has an unconnected output");
            return;
        }
    }

    void run(const QStringList& args) {
        QStringList trailing;
        for (int i = 0; i < args.size() && fine(); ++i) {
            const QString a = args[i];
            auto value = [&](QString* v) {
                if (i + 1 >= args.size()) { error(QString("Missing argument for option '%1'.").arg(a.mid(1))); return false; }
                *v = args[++i]; return true;
            };
            QString v;
            // global options: anywhere
            if (a == "-y") { c.overwrite = true; continue; }
            if (a == "-n") { c.noOverwrite = true; continue; }
            if (a == "-hide_banner") { c.hideBanner = true; continue; }
            if (a == "-nostdin") { c.nostdin = true; continue; }
            if (a == "-nostats") { c.nostats = true; continue; }
            if (a == "-v" || a == "-loglevel") { if (value(&v) && !parseLogLevel(v, &c.logLevel)) error("Invalid loglevel \"" + v + "\""); continue; }
            if (a == "-progress") { if (value(&v) && v != "pipe:1") unsupported("-progress " + v + " (only pipe:1)"); else c.progressPipe = true; continue; }
            if (a == "-filter_complex" || a == "-lavfi") {
                if (!value(&v)) continue;
                if (sawFilterComplex) { unsupported("-filter_complex given twice"); continue; }
                if (v.trimmed().isEmpty()) { error("-filter_complex: empty graph"); continue; }
                sawFilterComplex = true; c.filterComplex = v; continue;
            }
            if (!c.outputs.isEmpty() && a.startsWith('-') && a.size() > 1) { unsupported(a + " after the output file (ffmpeg ignores trailing options)"); break; }
            if (a == "-i") {
                if (!value(&v)) break;
                if (!c.outputs.isEmpty()) { unsupported("-i " + v + " after the output file"); break; }
                if (v.isEmpty()) { error("empty input file name"); break; }
                if (isNonFileInput(v)) { unsupported("input " + v); break; }
                applyInput(v); continue;
            }
            if (isInputOpt(a) || isOutputOpt(a) || isBothOpt(a)) {
                Opt g; g.name = a;
                if (takesValue(a)) { if (!value(&g.value)) break; g.hasValue = true; }
                group << g; continue;
            }
            if (a.startsWith('-') && a.size() > 1) { unsupported(a); break; }
            // an output file
            if (!c.outputs.isEmpty()) { unsupported("a second output " + a); break; }
            if (a.isEmpty()) { error("empty output file name"); break; }
            if (c.inputs.isEmpty()) { error("Output file " + a + " given before any input (-i)"); break; }
            if (isNonFileInput(a)) { unsupported("output " + a); break; }
            applyOutput(a);
        }
        if (!fine()) return;
        if (c.inputs.isEmpty()) { error("no input given (-i)"); return; }
        if (c.outputs.isEmpty()) {
            if (!group.isEmpty()) error(QString("Option %1 has no output file to apply to").arg(group.last().name)); else error("no output file given");
            return;
        }
        if (!group.isEmpty()) { unsupported(group.first().name + " after the output file (ffmpeg ignores trailing options)"); return; }
        if (c.overwrite && c.noOverwrite) { error("-y and -n cannot be used together"); return; }
        if (!c.filterComplex.isEmpty()) checkComplexGraph();
        const GraphOutput& out = c.outputs.first();
        if (fine() && !out.vf.isEmpty()) checkSimpleGraph("-vf", out.vf);
        if (fine() && !out.af.isEmpty()) checkSimpleGraph("-af", out.af);
        if (!fine()) return;
        // what the graph produces must be what the output takes
        if (!c.filterComplex.isEmpty() && out.maps.isEmpty()) { unsupported("-filter_complex without -map (ffmpeg would map its outputs on its own)"); return; }
        for (const GraphInput& in : c.inputs) {
            const QString ext = QFileInfo(in.path).suffix().toLower();
            if (in.loop && !(ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "webp" || ext == "tif" || ext == "tiff"))
                { unsupported("-loop 1 for " + in.path + " (an image2 option: a still only)"); return; }
            if (in.framerate > 0 && !in.path.contains('%') && !in.loop && !in.streamLoop) { unsupported("-framerate for " + in.path + " (an image2 option: a still or a frame sequence only)"); return; }
            if (in.concat && (in.loop || in.streamLoop || in.framerate > 0)) { unsupported("image options on a concat input"); return; }
        }
    }
};

}   // namespace

GraphCommand parseGraphArgs(const QStringList& args, const GraphParseOptions& o) {
    Parser p(o);
    p.run(args);
    return p.c;
}

QJsonObject GraphCommand::toJson() const {
    QJsonObject j;
    j["overwrite"] = overwrite; j["noOverwrite"] = noOverwrite; j["nostdin"] = nostdin; j["progress"] = progressPipe; j["logLevel"] = logLevel;
    if (!filterComplex.isEmpty()) { j["filterComplex"] = filterComplex; j["graphInputs"] = QJsonArray::fromStringList(graphInputs); j["filters"] = QJsonArray::fromStringList(filterNamesIn(filterComplex)); }
    QJsonArray ins;
    for (const GraphInput& in : inputs) {
        QJsonObject o; o["path"] = in.path;
        if (in.seekSec >= 0) o["ss"] = in.seekSec;
        if (in.durationSec >= 0) o["t"] = in.durationSec;
        if (in.loop) o["loop"] = true;
        if (in.streamLoop) o["streamLoop"] = true;
        if (in.framerate > 0) o["framerate"] = in.framerate;
        if (in.concat) { o["concat"] = true; o["safe0"] = in.safe0; }
        ins.append(o);
    }
    j["inputs"] = ins;
    QJsonArray outs;
    for (const GraphOutput& out : outputs) {
        QJsonObject o; o["path"] = out.path; o["container"] = out.container;
        o["videoEncoder"] = out.videoEncoder(); o["audioEncoder"] = out.audioEncoder();
        if (!out.maps.isEmpty()) o["maps"] = QJsonArray::fromStringList(out.maps);
        if (!out.vf.isEmpty()) { o["vf"] = out.vf; o["vfFilters"] = QJsonArray::fromStringList(filterNamesIn(out.vf)); }
        if (!out.af.isEmpty()) { o["af"] = out.af; o["afFilters"] = QJsonArray::fromStringList(filterNamesIn(out.af)); }
        if (out.noVideo) o["vn"] = true;
        if (out.noAudio) o["an"] = true;
        if (!out.preset.isEmpty()) o["preset"] = out.preset;
        if (out.crf >= 0) o["crf"] = out.crf;
        if (out.gop >= 0) o["g"] = out.gop;
        if (!out.pixFmt.isEmpty()) o["pixFmt"] = out.pixFmt;
        if (!out.colorRange.isEmpty()) o["colorRange"] = out.colorRange;
        if (!out.frameRate.isEmpty()) o["r"] = out.frameRateValue;
        if (out.audioBitrate > 0) o["audioBitrate"] = double(out.audioBitrate);
        if (out.sampleRate > 0) o["ar"] = out.sampleRate;
        if (out.channels > 0) o["ac"] = out.channels;
        if (out.faststart) o["faststart"] = true;
        if (out.makeZeroTs) o["avoidNegativeTs"] = "make_zero";
        if (out.frames >= 0) o["frames"] = out.frames;
        if (out.qscale >= 0) o["qscale"] = out.qscale;
        if (out.durationSec >= 0) o["t"] = out.durationSec;
        outs.append(o);
    }
    j["outputs"] = outs;
    return j;
}

int graphParseCli(const QStringList& args) {
    const GraphCommand c = parseGraphArgs(args);
    if (!c.unsupported.isEmpty()) return ppcli::unsupported(c.unsupported);
    if (!c.error.isEmpty()) return ppcli::fail(c.error);
    return ppcli::finish(QJsonObject{{"ok", true}, {"command", c.toJson()}});
}

} // namespace mediatool
