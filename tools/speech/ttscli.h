#ifndef TTSCLI_H
#define TTSCLI_H

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * ttscli: `PlayPlaySpeech tts`, the drop-in for `python -I scripts/spot_tts.py` (docs/plans/inhouse-deps.md §6.4). Same
 * arguments, same text rules, same one-line JSON on stdout, same exit codes, so SpotGenerator only changes the program:
 *
 *   tts --text-file in.txt --out voice.wav [--voice af_heart] [--speed 1.0] [--engine auto|kokoro|sapi]
 *       [--kokoro-dir D] [--kokoro-model D] [--sherpa-node D] [--max-chars 12000]
 *     -> {"ok": true, "engine": "sapi", "voiceUsed": "...", "durationSec": 5.8, "path": "...", "sampleRate": 48000,
 *         "words": 12, "tried": ["sapi"]}
 *   tts --batch voices.json [same options]        voices.json = {"items": [{"text": "...", "out": "voice_00.wav"}, ...]}
 *     -> {"ok": true, "engine": "sapi", "voiceUsed": "...", "items": [{"path", "durationSec", "words"}], "tried": [...]}
 *   failure -> {"ok": false, "error": "...", ["tried": [...] | "engine": "..."]}, exit 2.
 *
 * Text: whitespace runs collapse to one space (Python's \s), single mode cuts at --max-chars on the last ". " in the
 * second half, batch items are cut hard; under 2 characters is an error. Output WAVs are 48 kHz 16-bit mono, levelled
 * like the script (batch: trim -42 dBFS / 80 ms, then voiced-RMS -19 dB with the -1.5 dBFS limiter; single: level only),
 * and must last at least 0.3 s.
 *
 * Engines in this build (DEP-10): SAPI 5 through COM. Kokoro arrives with DEP-11; until then a run whose engine order
 * starts with Kokoro exits 64 "unsupported" before touching any file, so the Supervisor runs spot_tts.py, which voices
 * with Kokoro, instead of silently airing a different voice. That is `--engine kokoro`, and `auto` while any Kokoro the
 * script would try is here: the sherpa-onnx model + C API DLL, or a layout only the script runs (kokoro_onnx's model
 * files or the PlayPlay OBS venv under --kokoro-dir, the sherpa model + sherpa-onnx-node). `auto` without any of them
 * voices with SAPI and reports tried ["kokoro", "sapi"]. PLAYPLAY_SPEECH_KOKORO=0 treats every Kokoro as absent. The
 * `kokoro` pip package in the caller's Python cannot be seen from here (see docs/plans/impl-log/inhouse-media.md).
 *
 * A SAPI speak past its deadline (120 s + 0.2 s per character, or $PLAYPLAY_SPEECH_TIMEOUT_MS) ends the process at
 * once with the failure line and exit 2, as does a watchdog over the whole SAPI run: the engine DLLs run in this
 * process, and releasing a voice whose engine thread is stuck, or CoUninitialize, can block as long as it does.
 */
namespace ttscli {

struct Options {
    QString textFile, out, batch;
    QString voice = QStringLiteral("af_heart");
    double speed = 1.0;
    QString engine = QStringLiteral("auto");
    QString kokoroDir, kokoroModel, sherpaNode;
    int maxChars = 12000;
};

/// argparse rules: unknown option -> unsupported (64); bad value / choice -> 2; --help -> *helpShown, exit 0.
bool parseOptions(const QStringList& args, Options* o, QJsonObject* result, int* exitCode, bool* helpShown = nullptr);
QString usage();

/// re.sub(r"\s+", " ", s).strip()
QString collapseWhitespace(const QString& s);
/// s[:n] counted in code points, Python slice semantics (negative n counts from the end).
QString pyHead(const QString& s, int n);
/// Single-mode cut (spot_tts.py main): at most maxChars code points, ending after the last ". " when one is in the
/// second half.
QString cutNarration(const QString& text, int maxChars);
/// len(text.split())
int wordCount(const QString& text);
/// round(x, 3): correctly rounded, ties to even.
double round3(double x);
/// Python float() / int() for argument values.
bool parsePyFloat(const QString& text, double* out);
bool parsePyInt(const QString& text, int* out);

/// pathlib's Path(p).parent, lexically as on Windows: '/' and '\' alike, empty and "." parts dropped, ".." kept, and
/// the anchor (drive, root, UNC share) is its own parent. "." for a bare relative name.
QString pyParent(const QString& p);

/**
 * Where Kokoro would come from.
 *  - modelDir + sherpaDir: the sherpa-onnx model (find_kokoro_sherpa's lookup) and the sherpa-onnx C API DLL, what a
 *    native engine (DEP-11) runs.
 *  - The rest are layouts only spot_tts.py runs, all tried before its SAPI under `auto`: kokoro_onnx's model files
 *    (find_kokoro_models: --kokoro-dir, dir/kokoro_models, dir/../kokoro_models, dir/models), the PlayPlay OBS venv
 *    (try_kokoro_venv: a python.exe and kokoro_synth.py around --kokoro-dir) and the sherpa model through
 *    sherpa-onnx-node (package.json; node.exe and the helper script are not checked, so this errs towards the script).
 */
struct KokoroAssets {
    QString modelDir, sherpaDir;
    QString onnxModelDir, venvPython, venvSynth, sherpaNodeDir;
    bool disabled = false;                     // PLAYPLAY_SPEECH_KOKORO=0
    bool found() const { return !disabled && !modelDir.isEmpty() && !sherpaDir.isEmpty(); }
    /// spot_tts.py's Kokoro engines whose files are here, in the script's order: kokoro_onnx, kokoro_venv, kokoro_sherpa.
    QStringList scriptEngines() const;
    QJsonObject toJson() const;
};
KokoroAssets findKokoro(const Options& o);

/// The engine names `o.engine` resolves to in this build, or false with the unsupported result (exit 64).
bool planEngines(const Options& o, QStringList* order, QJsonObject* refusal);
/// The same with the Kokoro lookup done by the caller (tests build the assets by hand).
bool planEngines(const Options& o, const KokoroAssets& k, QStringList* order, QJsonObject* refusal);

QJsonObject run(const Options& o);
int cli(const QStringList& args);

}   // namespace ttscli

#endif // TTSCLI_H
