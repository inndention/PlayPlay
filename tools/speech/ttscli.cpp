#include "ttscli.h"

#include "ppcli.h"
#include "sapitts.h"
#include "voicemap.h"
#include "wavio.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>

#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace ttscli {
namespace {

struct Job { QString text, out; };

struct Synth { bool ok = false; QString engine, voiceUsed, error; };

/**
 * The last resort against a wedged voice engine: SAPI loads the engine DLLs (OneCore, third-party voices) into this
 * process, and once one of its threads is stuck, releasing the voice or CoUninitialize can block as long as it does.
 * When `budgetMs` passes, or at once through fire(), the failure line is printed and the process ends with exit 2,
 * skipping those destructors, so the caller gets a prompt failure instead of waiting out its own timeout (as ppcli's
 * ServeLoop does with hardExitOnHang). Firing and disarming take the same mutex: exactly one result line is printed.
 */
class HardStop {
public:
    HardStop(qint64 budgetMs, QJsonObject result) : m_result(std::move(result)), m_thread([this, budgetMs] {
        std::unique_lock<std::mutex> l(m_mutex);
        if (!m_cv.wait_for(l, std::chrono::milliseconds(budgetMs), [this] { return m_disarmed; }))
            exitNow(QStringLiteral("speech watchdog: the SAPI run took longer than %1 s").arg(budgetMs / 1000.0, 0, 'f', 1));
    }) {}
    ~HardStop() {
        { std::lock_guard<std::mutex> l(m_mutex); m_disarmed = true; }
        m_cv.notify_all();
        m_thread.join();
    }
    HardStop(const HardStop&) = delete;
    HardStop& operator=(const HardStop&) = delete;
    [[noreturn]] void fire(const QString& why) { std::unique_lock<std::mutex> l(m_mutex); exitNow(why); }
private:
    [[noreturn]] void exitNow(const QString& why) {
        ppcli::log(why + "; exiting without releasing the voice");
        ppcli::finish(m_result);                    // flushed: the line is in the pipe before the process goes
        // TerminateProcess rather than ExitProcess (std::_Exit): ExitProcess takes the loader lock first, and a voice
        // engine stuck in its DllMain would hold it forever
        TerminateProcess(GetCurrentProcess(), UINT(ppcli::ExitFailed));
        std::_Exit(ppcli::ExitFailed);
    }
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_disarmed = false;
    const QJsonObject m_result;
    std::thread m_thread;                           // last: starts once everything above exists
};

/// SAPI renders far faster than real time; the deadline only catches an engine that stopped answering.
/// $PLAYPLAY_SPEECH_TIMEOUT_MS replaces it (as $PLAYPLAY_MEDIATOOL_TIMEOUT_MS does for MediaTool).
int itemTimeoutMs(const QString& text) {
    bool ok = false;
    const int env = qEnvironmentVariableIntValue("PLAYPLAY_SPEECH_TIMEOUT_MS", &ok);
    if (ok && env > 0) return env;
    return int(qMin<qint64>(INT_MAX, 120000 + 200 * qint64(text.size())));
}

/// The whole SAPI run: 60 s to create the voice and load its engine, then each item's deadline plus 15 s to close and
/// rename its file. $PLAYPLAY_SPEECH_WATCHDOG_MS replaces it.
qint64 runBudgetMs(const QList<Job>& jobs) {
    bool ok = false;
    const qint64 env = qEnvironmentVariable("PLAYPLAY_SPEECH_WATCHDOG_MS").toLongLong(&ok);
    if (ok && env > 0) return env;
    qint64 ms = 60000;
    for (const Job& j : jobs) ms += itemTimeoutMs(j.text) + 15000;
    return ms;
}

/// Python's len() and slice bounds work on code points; QString holds UTF-16.
qsizetype utf16Offset(const QString& s, qsizetype n) {
    qsizetype i = 0;
    while (n > 0 && i < s.size()) { i += (s[i].isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) ? 2 : 1; --n; }
    return i;
}
qsizetype codePoints(const QString& s) {
    qsizetype n = 0;
    for (qsizetype i = 0; i < s.size(); ++n) i += (s[i].isHighSurrogate() && i + 1 < s.size() && s[i + 1].isLowSurrogate()) ? 2 : 1;
    return n;
}
/// s[start:] / s[:stop] bound for a code-point index (negative counts from the end, clamped).
qsizetype pyIndex(qsizetype i, qsizetype len) { if (i < 0) i += len; return i < 0 ? 0 : (i > len ? len : i); }

QString nativeAbs(const QString& p) { return QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath()); }
QString fileName(const QString& p) { return QFileInfo(p).fileName(); }

/// Every output starts from nothing: the parent exists and a stale WAV from an earlier run is gone (the script unlinks it).
void prepareOutput(const QString& out) {
    QDir().mkpath(QFileInfo(out).absolutePath());
    if (QFileInfo::exists(out)) QFile::remove(out);
}

/// str(it.get("text") or ""): what Python makes of a JSON value used as text.
QString pyStr(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::String: return v.toString();
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("True") : QString();
    case QJsonValue::Double: return v.toDouble() == 0.0 ? QString() : QString::number(v.toDouble(), 'g', QLocale::FloatingPointShortest);
    case QJsonValue::Array: return v.toArray().isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    case QJsonValue::Object: return v.toObject().isEmpty() ? QString() : QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    default: return {};
    }
}
bool pyTruthy(const QJsonValue& v) { return !pyStr(v).isEmpty(); }

// ----------------------------------------------------------------------------- engines
Synth synthSapi(const QList<Job>& jobs, const QString& voice, double speed, const QJsonArray& tried) {
    Synth r; r.engine = QStringLiteral("sapi");
    // declared first, so it is destroyed last: it also covers the voice's release and CoUninitialize below
    HardStop stop(runBudgetMs(jobs), ppcli::failure(QStringLiteral("no TTS engine produced audio"), {{"tried", tried}}));
    sapitts::ComScope com;
    if (!com.ok()) { r.error = com.error(); return r; }
    sapitts::Speaker sp; QString err;
    if (!sp.open(&err)) { r.error = err; return r; }
    const QString name = voicemap::sapiVoiceFor(voice, sapitts::voiceNames(sp.voices()));
    if (!name.isEmpty() && !sp.selectVoice(name, &err)) ppcli::log(err);     // the script went on with the default voice
    const int rate = voicemap::sapiRate(speed);
    if (!sp.setRate(rate, &err)) ppcli::log(err);
    r.voiceUsed = sp.voiceName();
    if (r.voiceUsed.isEmpty()) r.voiceUsed = name;
    ppcli::log(QStringLiteral("sapi: voice \"%1\" for \"%2\", rate %3, %4 item(s)").arg(r.voiceUsed, voice).arg(rate).arg(jobs.size()));
    QElapsedTimer t; t.start();
    for (const Job& j : jobs) {
        if (!sp.speakToFile(j.text, j.out, itemTimeoutMs(j.text), &err)) {
            if (sp.hung()) stop.fire("SAPI failed: " + fileName(j.out) + ": " + err);   // its engine thread is still busy
            r.error = fileName(j.out) + ": " + err;
            return r;
        }
    }
    ppcli::log(QStringLiteral("sapi: %1 file(s) in %2 ms").arg(jobs.size()).arg(t.elapsed()));
    r.ok = true;
    return r;
}

/// Runs the planned engines in order (DEP-10: SAPI is the only one that can run; "kokoro" stands for "checked, absent").
Synth synthesize(const QList<Job>& jobs, const QStringList& order, const Options& o, double speed, QJsonArray* tried) {
    Synth last;
    for (const QString& e : order) {
        tried->append(e);
        if (e == QLatin1String("kokoro")) {
            const KokoroAssets k = findKokoro(o);
            ppcli::log(k.disabled ? QStringLiteral("kokoro: disabled (PLAYPLAY_SPEECH_KOKORO)")
                                  : QStringLiteral("kokoro (sherpa-onnx) not available: model %1, sherpa-onnx runtime %2")
                                        .arg(k.modelDir.isEmpty() ? "missing" : "ok", k.sherpaDir.isEmpty() ? "missing" : "ok"));
            continue;
        }
        if (e == QLatin1String("sapi")) {
            last = synthSapi(jobs, o.voice, speed, *tried);
            bool allThere = last.ok;
            for (const Job& j : jobs) allThere = allThere && QFileInfo(j.out).isFile();
            if (allThere) return last;
            ppcli::log("SAPI failed: " + (last.error.isEmpty() ? QStringLiteral("no audio written") : last.error));
            last.ok = false;
        }
    }
    return last;
}

/// trim_wav (batch only) + normalize_wav on the engine's file; a failure is logged and the file kept, as in the script.
void level(const QString& out, bool trim) {
    wavio::Wav w; QString err;
    if (!wavio::readFile(out, &w, &err)) { ppcli::log(fileName(out) + ": trim / level skipped (" + err + ")"); return; }
    if (!w.isPcm16()) return;
    const bool trimmed = trim && wavio::trim(w);
    const double gain = wavio::normalize(w);
    if ((trimmed || gain != 0.0) && !wavio::writeFile(out, w, &err)) ppcli::log(fileName(out) + ": trim / level skipped (" + err + ")");
}

bool readDuration(const QString& out, double* dur, int* rate, QString* err) {
    int ch = 0; qint64 frames = 0; *rate = 0;
    if (!wavio::info(out, rate, &ch, &frames, err)) return false;
    *dur = *rate > 0 ? double(frames) / *rate : 0.0;
    return true;
}

// ----------------------------------------------------------------------------- modes
QJsonObject runBatch(const Options& o) {
    QFile f(o.batch);
    if (!f.open(QIODevice::ReadOnly)) return ppcli::failure(QStringLiteral("cannot read batch: %1: %2").arg(o.batch, f.errorString()));
    QByteArray bytes = f.readAll();
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);                 // encoding="utf-8-sig"
    QJsonParseError pe; const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError) return ppcli::failure("cannot read batch: " + pe.errorString() + QStringLiteral(" at offset %1").arg(pe.offset));
    if (!doc.isObject()) return ppcli::failure(QStringLiteral("cannot read batch: not a JSON object"));
    const QJsonArray items = doc.object().value("items").toArray();
    QList<Job> jobs;
    for (int i = 0; i < items.size(); ++i) {
        const QJsonObject it = items[i].toObject();
        const QString text = pyHead(collapseWhitespace(pyStr(it.value("text"))), o.maxChars);
        if (codePoints(text) < 2 || !pyTruthy(it.value("out"))) return ppcli::failure(QStringLiteral("item %1: empty text or no output path").arg(i));
        jobs.append({text, nativeAbs(pyStr(it.value("out")))});
    }
    if (jobs.isEmpty()) return ppcli::failure(QStringLiteral("no items"));
    const double speed = voicemap::clampSpeed(o.speed);
    QStringList order; QJsonObject refusal;
    if (!planEngines(o, &order, &refusal)) return refusal;     // before any file is touched: the fallback starts clean
    for (const Job& j : jobs) prepareOutput(j.out);
    QJsonArray tried;
    const Synth s = synthesize(jobs, order, o, speed, &tried);
    if (!s.ok) return ppcli::failure(QStringLiteral("no TTS engine produced audio"), {{"tried", tried}});
    QJsonArray out;
    for (const Job& j : jobs) {
        level(j.out, true);
        double dur = 0; int rate = 0; QString err;
        if (!readDuration(j.out, &dur, &rate, &err)) return ppcli::failure(fileName(j.out) + " is not a WAV: " + err, {{"engine", s.engine}});
        if (dur < 0.3) return ppcli::failure(QStringLiteral("%1: audio too short (%2s)").arg(fileName(j.out)).arg(dur, 0, 'f', 2), {{"engine", s.engine}});
        out.append(QJsonObject{{"path", j.out}, {"durationSec", round3(dur)}, {"words", wordCount(j.text)}});
    }
    return QJsonObject{{"ok", true}, {"engine", s.engine}, {"voiceUsed", s.voiceUsed}, {"items", out}, {"tried", tried}};
}

QJsonObject runSingle(const Options& o) {
    if (o.textFile.isEmpty() || o.out.isEmpty()) return ppcli::failure(QStringLiteral("--text-file and --out are required (or --batch)"));
    QFile f(o.textFile);
    if (!f.open(QIODevice::ReadOnly)) return ppcli::failure(QStringLiteral("cannot read text: %1: %2").arg(o.textFile, f.errorString()));
    QString text = collapseWhitespace(QString::fromUtf8(f.readAll()));       // errors="replace": invalid UTF-8 -> U+FFFD
    text = cutNarration(text, o.maxChars);
    if (codePoints(text) < 2) return ppcli::failure(QStringLiteral("empty narration"));
    const QString out = nativeAbs(o.out);
    const double speed = voicemap::clampSpeed(o.speed);
    QStringList order; QJsonObject refusal;
    if (!planEngines(o, &order, &refusal)) return refusal;
    prepareOutput(out);
    QJsonArray tried;
    const Synth s = synthesize({{text, out}}, order, o, speed, &tried);
    if (!s.ok || !QFileInfo(out).isFile()) return ppcli::failure(QStringLiteral("no TTS engine produced audio"), {{"tried", tried}});
    level(out, false);                                                        // one speech level for every engine
    double dur = 0; int rate = 0; QString err;
    if (!readDuration(out, &dur, &rate, &err)) return ppcli::failure("output is not a WAV: " + err, {{"engine", s.engine}});
    if (dur < 0.3) return ppcli::failure(QStringLiteral("synthesised audio too short (%1s)").arg(dur, 0, 'f', 2), {{"engine", s.engine}});
    return QJsonObject{{"ok", true}, {"engine", s.engine}, {"voiceUsed", s.voiceUsed}, {"durationSec", round3(dur)}, {"path", out},
                       {"sampleRate", rate}, {"words", wordCount(text)}, {"tried", tried}};
}

}   // namespace

// ----------------------------------------------------------------------------- text rules
QString collapseWhitespace(const QString& s) {
    QString r; r.reserve(s.size());
    bool pending = false;
    for (const QChar c : s) {
        if (voicemap::isPySpace(c.unicode())) { pending = true; continue; }    // surrogates are never whitespace
        if (pending && !r.isEmpty()) r += QLatin1Char(' ');
        pending = false;
        r += c;
    }
    return r;
}

QString pyHead(const QString& s, int n) { return s.left(utf16Offset(s, pyIndex(n, codePoints(s)))); }

QString cutNarration(const QString& text, int maxChars) {
    const qsizetype len = codePoints(text);
    if (len <= maxChars) return text;
    const QString cut = pyHead(text, maxChars);
    const qsizetype half = qsizetype(std::floor(maxChars / 2.0));                // max_chars // 2 (floor division)
    const QString tail = cut.mid(utf16Offset(cut, pyIndex(half, codePoints(cut))));
    return tail.contains(QLatin1String(". ")) ? cut.left(cut.lastIndexOf(QLatin1String(". ")) + 1) : cut;
}

int wordCount(const QString& text) {
    int n = 0; bool in = false;
    for (const QChar c : text) {
        const bool sp = voicemap::isPySpace(c.unicode());
        if (!sp && !in) ++n;
        in = !sp;
    }
    return n;
}

double round3(double x) {
    if (!std::isfinite(x)) return x;
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, 3);   // correctly rounded, ties to even
    if (r.ec != std::errc()) return x;
    double v = x;
    std::from_chars(buf, r.ptr, v);
    return v;
}

bool parsePyFloat(const QString& text, double* out) {
    QString s = voicemap::pyStrip(text).toLower();
    if (s.isEmpty()) return false;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; s = s.mid(1); }
    if (s == QLatin1String("inf") || s == QLatin1String("infinity")) { *out = neg ? -INFINITY : INFINITY; return true; }
    if (s == QLatin1String("nan")) { *out = NAN; return true; }
    if (s.isEmpty() || s[0] == '+' || s[0] == '-') return false;                // one sign only
    QByteArray b;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '_') {                                                       // PEP 515: single underscores between digits
            if (i == 0 || i + 1 >= s.size() || !s[i - 1].isDigit() || !s[i + 1].isDigit()) return false;
            continue;
        }
        if (c.unicode() > 127 || !(c.isDigit() || c == '.' || c == 'e' || c == '+' || c == '-')) return false;
        b += char(c.unicode());
    }
    if (b.isEmpty()) return false;
    if (neg) b.prepend('-');
    double v = 0.0;
    const auto r = std::from_chars(b.constData(), b.constData() + b.size(), v, std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) { *out = std::strtod(b.constData(), nullptr); return true; }   // overflow -> inf
    if (r.ec != std::errc() || r.ptr != b.constData() + b.size()) return false;
    *out = v;
    return true;
}

bool parsePyInt(const QString& text, int* out) {
    QString s = voicemap::pyStrip(text);
    if (s.isEmpty()) return false;
    bool neg = false;
    if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; s = s.mid(1); }
    if (s.isEmpty()) return false;
    qint64 v = 0;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == '_') { if (i == 0 || i + 1 >= s.size() || s[i + 1] == '_') return false; continue; }
        const int d = s[i].digitValue();                                       // int() takes any Unicode decimal digit
        if (d < 0) return false;
        v = qMin<qint64>(v * 10 + d, qint64(INT_MAX) + 1);                    // a slice bound past the text is the text
    }
    *out = int(neg ? qMax<qint64>(-v, INT_MIN) : qMin<qint64>(v, INT_MAX));
    return true;
}

// ----------------------------------------------------------------------------- Kokoro presence
QString pyParent(const QString& p) {
    const QString s = QDir::fromNativeSeparators(p);
    qsizetype i = 0;
    if (s.startsWith(QLatin1String("//"))) {                                  // \\server\share\ is the anchor
        const qsizetype a = s.indexOf('/', 2), b = a < 0 ? -1 : s.indexOf('/', a + 1);
        i = b < 0 ? s.size() : b + 1;
    } else {
        if (s.size() >= 2 && s[1] == ':') i = 2;                              // drive
        if (i < s.size() && s[i] == '/') ++i;                                 // root
    }
    QStringList parts;
    for (const QString& x : s.mid(i).split('/', Qt::SkipEmptyParts)) if (x != QLatin1String(".")) parts << x;
    if (!parts.isEmpty()) parts.removeLast();
    const QString r = s.left(i) + parts.join('/');
    return r.isEmpty() ? QStringLiteral(".") : r;
}

QStringList KokoroAssets::scriptEngines() const {
    QStringList e;
    if (disabled) return e;
    if (!onnxModelDir.isEmpty()) e << QStringLiteral("kokoro_onnx");
    if (!venvPython.isEmpty() && !venvSynth.isEmpty()) e << QStringLiteral("kokoro_venv");
    if (!modelDir.isEmpty() && !sherpaNodeDir.isEmpty()) e << QStringLiteral("kokoro_sherpa");
    return e;
}

QJsonObject KokoroAssets::toJson() const {
    const auto n = [](const QString& p) { return QDir::toNativeSeparators(p); };
    return QJsonObject{{"found", found()}, {"disabled", disabled}, {"modelDir", n(modelDir)}, {"sherpaDir", n(sherpaDir)}, {"built", false},
                       {"scriptEngines", QJsonArray::fromStringList(scriptEngines())}, {"onnxModelDir", n(onnxModelDir)},
                       {"venvPython", n(venvPython)}, {"venvSynth", n(venvSynth)}, {"sherpaNodeDir", n(sherpaNodeDir)}};
}

KokoroAssets findKokoro(const Options& o) {
    KokoroAssets k;
    const QString sw = qEnvironmentVariable("PLAYPLAY_SPEECH_KOKORO").trimmed().toLower();
    if (sw == QLatin1String("0") || sw == QLatin1String("off") || sw == QLatin1String("no") || sw == QLatin1String("false")) { k.disabled = true; return k; }
    const auto isFile = [](const QString& dir, const QString& rel) { return QFileInfo(QDir(dir).filePath(rel)).isFile(); };
    // the model dir rule of spot_tts.py find_kokoro_sherpa: --kokoro-model, a --kokoro-dir holding the model, the env, the default
    static const char* const kFiles[] = {"model.int8.onnx", "voices.bin", "tokens.txt", "espeak-ng-data"};
    for (const QString& d : {o.kokoroModel, o.kokoroDir, qEnvironmentVariable("STARMEDIA_KOKORO_DIR"), QStringLiteral("D:/StarMedia/models/kokoro-int8-en-v0_19")}) {
        if (d.isEmpty()) continue;
        bool all = true;
        for (const char* f : kFiles) all = all && QFileInfo::exists(QDir(d).filePath(QLatin1String(f)));
        if (all) { k.modelDir = d; break; }
    }
    // the sherpa-onnx C API: staged next to this exe (DEP-11), PLAYPLAY_SHERPA_ROOT, or the addon folder beside sherpa-onnx-node
    QStringList dirs;
    if (QCoreApplication::instance()) dirs << QCoreApplication::applicationDirPath();
    dirs << qEnvironmentVariable("PLAYPLAY_SHERPA_ROOT");
    const QStringList nodeDirs = {o.sherpaNode, qEnvironmentVariable("SHERPA_ONNX_NODE_DIR"), QStringLiteral("D:/StarMedia/motion-spots/node_modules/sherpa-onnx-node")};
    for (const QString& node : nodeDirs)
        if (!node.isEmpty()) dirs << QDir(node).filePath(QStringLiteral("../sherpa-onnx-win-x64"));
    for (const QString& d : dirs)
        if (!d.isEmpty() && isFile(d, QStringLiteral("sherpa-onnx-c-api.dll"))) { k.sherpaDir = QDir::cleanPath(d); break; }
    // what the script's Node path needs besides the model (find_kokoro_sherpa): a sherpa-onnx-node package
    for (const QString& node : nodeDirs)
        if (!node.isEmpty() && isFile(node, QStringLiteral("package.json"))) { k.sherpaNodeDir = node; break; }
    // the --kokoro-dir layouts spot_tts.py tries before SAPI (and before its sherpa path) and this tool never runs
    if (!o.kokoroDir.isEmpty()) {
        const QString base = o.kokoroDir, parent = pyParent(base);
        for (const QString& c : {base, QDir(base).filePath(QStringLiteral("kokoro_models")), QDir(parent).filePath(QStringLiteral("kokoro_models")),
                                 QDir(base).filePath(QStringLiteral("models"))})      // find_kokoro_models
            if (isFile(c, QStringLiteral("kokoro-v1.0.onnx")) && isFile(c, QStringLiteral("voices-v1.0.bin"))) { k.onnxModelDir = c; break; }
        for (const QString& c : {QDir(base).filePath(QStringLiteral("ttsvenv/Scripts/python.exe")), QDir(base).filePath(QStringLiteral("Scripts/python.exe")),
                                 QDir(parent).filePath(QStringLiteral("tools/ttsvenv/Scripts/python.exe"))})   // try_kokoro_venv
            if (QFileInfo(c).isFile()) { k.venvPython = c; break; }
        for (const QString& c : {QDir(base).filePath(QStringLiteral("kokoro_synth.py")), QDir(base).filePath(QStringLiteral("tools/kokoro_synth.py")),
                                 QDir(parent).filePath(QStringLiteral("tools/kokoro_synth.py"))})
            if (QFileInfo(c).isFile()) { k.venvSynth = c; break; }
    }
    return k;
}

bool planEngines(const Options& o, const KokoroAssets& k, QStringList* order, QJsonObject* refusal) {
    if (o.engine == QLatin1String("kokoro")) {
        *refusal = ppcli::unsupportedResult(QStringLiteral("--engine kokoro"), {{"reason", "this PlayPlaySpeech build has no Kokoro engine yet; run spot_tts.py"}});
        return false;
    }
    if (o.engine == QLatin1String("auto")) {
        // Any Kokoro the script would try before SAPI: hand the run back rather than air SAPI where today airs Kokoro.
        // DEP-11 (native sherpa) still has to refuse kokoro_onnx / kokoro_venv, which the script tries first with another
        // model, and kokoro_sherpa when the C API DLL is missing.
        const QStringList script = k.scriptEngines();
        if (k.found() || !script.isEmpty()) {
            const QString reason = script.isEmpty()
                ? QStringLiteral("Kokoro is installed but this PlayPlaySpeech build has no Kokoro engine yet; run spot_tts.py")
                : QStringLiteral("spot_tts.py would voice this with Kokoro (%1) and this PlayPlaySpeech build has no Kokoro engine yet; run spot_tts.py").arg(script.join(", "));
            *refusal = ppcli::unsupportedResult(QStringLiteral("--engine auto"), {{"reason", reason}, {"kokoro", k.toJson()}});
            return false;
        }
        *order = {QStringLiteral("kokoro"), QStringLiteral("sapi")};
        return true;
    }
    *order = {QStringLiteral("sapi")};
    return true;
}

bool planEngines(const Options& o, QStringList* order, QJsonObject* refusal) {
    return planEngines(o, o.engine == QLatin1String("auto") ? findKokoro(o) : KokoroAssets(), order, refusal);
}

// ----------------------------------------------------------------------------- CLI
QString usage() {
    return QStringLiteral(
        "usage: PlayPlaySpeech tts --text-file in.txt --out voice.wav [--voice af_heart] [--speed 1.0]\n"
        "                          [--engine auto|kokoro|sapi] [--kokoro-dir D] [--kokoro-model D] [--sherpa-node D]\n"
        "                          [--max-chars 12000]\n"
        "       PlayPlaySpeech tts --batch voices.json [same options]\n"
        "voices.json = {\"items\": [{\"text\": \"...\", \"out\": \"voice_00.wav\"}, ...]}; one engine for every item.\n"
        "Drop-in for scripts/spot_tts.py: one JSON object on the last stdout line, diagnostics on stderr, exit 0 / 2;\n"
        "exit 64 when the run needs an engine this build lacks (Kokoro: DEP-11), so the caller can run the script.\n");
}

bool parseOptions(const QStringList& args, Options* o, QJsonObject* result, int* exitCode, bool* helpShown) {
    if (helpShown) *helpShown = false;
    const ppcli::Args a(args, {"--text-file=", "--out=", "--batch=", "--voice=", "--speed=", "--engine=", "--kokoro-dir=",
                               "--kokoro-model=", "--sherpa-node=", "--max-chars=", "--help", "-h"});
    if (!a.ok()) {
        *result = a.unknownOption() ? ppcli::unsupportedResult(a.unknownName()) : ppcli::failure(a.error());
        *exitCode = ppcli::exitCodeFor(*result);
        return false;
    }
    if (a.has("--help") || a.has("-h")) { if (helpShown) *helpShown = true; *exitCode = ppcli::ExitOk; return false; }
    auto bad = [&](const QString& msg) { *result = ppcli::failure(msg); *exitCode = ppcli::ExitFailed; return false; };
    o->textFile = a.value("--text-file"); o->out = a.value("--out"); o->batch = a.value("--batch");
    if (a.has("--voice")) o->voice = a.value("--voice");
    if (a.has("--speed") && !parsePyFloat(a.value("--speed"), &o->speed)) return bad(QStringLiteral("argument --speed: invalid float value: '%1'").arg(a.value("--speed")));
    if (a.has("--engine")) {
        o->engine = a.value("--engine");
        if (o->engine != QLatin1String("auto") && o->engine != QLatin1String("kokoro") && o->engine != QLatin1String("sapi"))
            return bad(QStringLiteral("argument --engine: invalid choice: '%1' (choose from 'auto', 'kokoro', 'sapi')").arg(o->engine));
    }
    o->kokoroDir = a.value("--kokoro-dir"); o->kokoroModel = a.value("--kokoro-model"); o->sherpaNode = a.value("--sherpa-node");
    if (a.has("--max-chars") && !parsePyInt(a.value("--max-chars"), &o->maxChars)) return bad(QStringLiteral("argument --max-chars: invalid int value: '%1'").arg(a.value("--max-chars")));
    return true;
}

QJsonObject run(const Options& o) {
    try {
        return o.batch.isEmpty() ? runSingle(o) : runBatch(o);
    } catch (const std::bad_alloc&) {
        return ppcli::failure(QStringLiteral("out of memory"));
    } catch (const std::exception& e) {
        return ppcli::failure(QString::fromLocal8Bit(e.what()));
    }
}

int cli(const QStringList& args) {
    Options o; QJsonObject err; int code = 0; bool help = false;
    if (!parseOptions(args, &o, &err, &code, &help)) {
        if (help) { const QByteArray u = usage().toUtf8(); fwrite(u.constData(), 1, size_t(u.size()), stdout); fflush(stdout); return code; }
        ppcli::log(err.value("error").toString());
        return ppcli::finish(err);
    }
    const QJsonObject r = run(o);
    if (!r.value("ok").toBool()) ppcli::log(r.value("error").toString() + (r.contains("reason") ? " (" + r.value("reason").toString() + ")" : QString()));
    return ppcli::finish(r);
}

}   // namespace ttscli
