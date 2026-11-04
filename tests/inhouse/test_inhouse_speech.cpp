// test_inhouse_speech (DEP-10): PlayPlaySpeech `tts` with SAPI 5 through COM against scripts/spot_tts.py.
//  - voicemap: sapi_voice_for (Kokoro id -> installed SAPI voice; exact names pass through) on fixed voice lists and,
//    when Python is here, against the script's own function on the whole cross product; the kokoro-en-v0_19 speaker
//    table / nearest voice of kokoro_sherpa.js; speed clamp and SAPI rate (values printed by CPython 3.11).
//  - text rules: whitespace collapse with Python's \s, the single-mode cut at the last ". ", word counts, round(x, 3).
//  - the exe, single and --batch (3 items): 48 kHz 16-bit mono WAVs, levelled (voiced RMS -19 dB, peaks <= -1.5 dBFS),
//    batch trimmed, longer than 0.3 s; stdout keys exactly the script's; af_heart -> Zira, am_michael -> David,
//    bm_* -> David (no George here); OneCore "Microsoft Mark" selectable; speed changes the rate; non-ASCII paths.
//  - the contract: --engine kokoro and auto-with-Kokoro-installed exit 64 before touching files, Kokoro being anything
//    the script would try (sherpa model + DLL, kokoro_onnx files / the OBS venv under --kokoro-dir, sherpa-onnx-node),
//    looked up as the script's own functions do; auto without Kokoro voices with SAPI and reports tried ["kokoro",
//    "sapi"]; argparse-style errors exit 2 / 64 with one JSON line; a speak past its deadline, or the run watchdog,
//    ends the process at once with exit 2 and the failure line.
//  - service-like run: no console (DETACHED_PROCESS), PATH without any PowerShell, inside a job object that must end with
//    exactly one process (nothing was spawned), and no process-spawning call anywhere in tools/speech.
//  - with Python + numpy + PowerShell: the same text through spot_tts.py --engine sapi gives the same keys, voice, words
//    and duration, the same level, and (desktop Zira) the same waveform.
#include "ppcli.h"
#include "sapitts.h"
#include "ttscli.h"
#include "voicemap.h"
#include "wavio.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <windows.h>

#include <climits>
#include <cmath>
#include <cstdlib>

namespace {

const QString kExe = QStringLiteral(PP_SPEECH_EXE);
const QString kScripts = QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("scripts");

struct Run { int code = -1; QByteArray out, err; QJsonObject json; qint64 ms = 0; };
Run runTool(const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment(), int timeoutMs = 120000) {
    Run r; QProcess p; QElapsedTimer t; t.start();
    if (!env.isEmpty()) p.setProcessEnvironment(env);
    p.start(kExe, args);
    if (!p.waitForStarted(20000)) { r.err = "could not start " + kExe.toUtf8(); return r; }
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(5000); r.err = "timeout"; return r; }
    r.ms = t.elapsed();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError();
    r.json = ppcli::parseResult(r.out);
    return r;
}
QByteArray describe(const Run& r) { return "exit " + QByteArray::number(r.code) + "\nstdout: " + r.out + "\nstderr: " + r.err.right(1500); }

QProcessEnvironment envWith(const QString& key, const QString& value) {
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    e.insert(key, value);
    return e;
}
QProcessEnvironment noKokoro() { return envWith("PLAYPLAY_SPEECH_KOKORO", "0"); }

bool writeText(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

bool voiceInstalled(const QString& name) {
    sapitts::ComScope com;
    return com.ok() && sapitts::voiceNames(sapitts::installedVoices()).contains(name);
}

QStringList sortedKeys(const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; }
QStringList sorted(QStringList l) { l.sort(); return l; }
QString nativeAbs(const QString& p) { return QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath()); }

const QStringList kSingleKeys = sorted({"ok", "engine", "voiceUsed", "durationSec", "path", "sampleRate", "words", "tried"});
const QStringList kBatchKeys = sorted({"ok", "engine", "voiceUsed", "items", "tried"});
const QStringList kItemKeys = sorted({"path", "durationSec", "words"});

/// A spot WAV as the pipeline needs it: 48 kHz mono 16-bit, at least 0.3 s, levelled like spot_tts.py.
void checkVoiceWav(const QString& path, double reportedSec, wavio::Wav* out = nullptr) {
    wavio::Wav w; QString err;
    QVERIFY2(wavio::readFile(path, &w, &err), qPrintable(path + ": " + err));
    QCOMPARE(w.sampleRate, 48000);
    QCOMPARE(w.channels, 1);
    QVERIFY(w.isPcm16());
    QVERIFY2(w.durationSec() > 0.3, qPrintable(QString::number(w.durationSec())));
    QCOMPARE(reportedSec, ttscli::round3(w.durationSec()));
    const double rms = wavio::voicedRmsDb(w), peak = wavio::peakDbfs(w);
    // normalize_wav moves the voiced RMS to -19 dB and then limits peaks to -1.5 dBFS (the limiter may take a little
    // more level off). Within its 0.5 dB dead band it leaves the engine's samples alone, peaks included: SAPI's David
    // often lands there with peaks near -0.3 dBFS, in the script exactly as here.
    QVERIFY2(std::fabs(rms + 19.0) <= 0.75, qPrintable(QString("voiced RMS %1 dB").arg(rms)));
    QVERIFY2(peak <= -1.5 + 0.01 || std::fabs(rms + 19.0) < 0.5, qPrintable(QString("peak %1 dBFS with voiced RMS %2 dB").arg(peak).arg(rms)));
    QVERIFY2(peak < -0.05, qPrintable(QString("clipped: peak %1 dBFS").arg(peak)));
    if (out) *out = std::move(w);
}

/// Samples before the first / after the last one above -42 dBFS (trim's threshold), in ms.
void quietEdgesMs(const wavio::Wav& w, double* leadMs, double* tailMs) {
    const int thr = int(32767 * std::pow(10.0, -42.0 / 20.0));
    qint64 a = 0, b = qint64(w.samples.size()) - 1;
    while (a <= b && std::abs(int(w.samples[size_t(a)])) <= thr) ++a;
    while (b >= a && std::abs(int(w.samples[size_t(b)])) <= thr) --b;
    *leadMs = a * 1000.0 / w.sampleRate;
    *tailMs = (qint64(w.samples.size()) - 1 - b) * 1000.0 / w.sampleRate;
}

double correlation(const wavio::Wav& a, const wavio::Wav& b) {
    const qint64 n = qMin(a.frames(), b.frames());
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (qint64 i = 0; i < n; ++i) { const double x = a.samples[size_t(i)], y = b.samples[size_t(i)]; sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y; }
    const double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    return va > 0 && vb > 0 ? cov / std::sqrt(va * vb) : 0.0;
}
/// Correlation of the 50 ms RMS envelopes: same words, same timing, whatever the resampler did to the waveform.
double envelopeCorrelation(const wavio::Wav& a, const wavio::Wav& b) {
    const qint64 block = 2400, n = qMin(a.frames(), b.frames()) / block;
    std::vector<double> ea(static_cast<size_t>(n)), eb(static_cast<size_t>(n));
    for (qint64 k = 0; k < n; ++k) {
        double x = 0, y = 0;
        for (qint64 i = k * block; i < (k + 1) * block; ++i) { x += double(a.samples[size_t(i)]) * a.samples[size_t(i)]; y += double(b.samples[size_t(i)]) * b.samples[size_t(i)]; }
        ea[size_t(k)] = std::sqrt(x); eb[size_t(k)] = std::sqrt(y);
    }
    double ma = 0, mb = 0; for (qint64 k = 0; k < n; ++k) { ma += ea[size_t(k)]; mb += eb[size_t(k)]; }
    ma /= n; mb /= n;
    double c = 0, va = 0, vb = 0;
    for (qint64 k = 0; k < n; ++k) { const double x = ea[size_t(k)] - ma, y = eb[size_t(k)] - mb; c += x * y; va += x * x; vb += y * y; }
    return va > 0 && vb > 0 ? c / std::sqrt(va * vb) : 0.0;
}

QString findPython(bool needNumpy) {
    QStringList c;
    if (qEnvironmentVariableIsSet("PLAYPLAY_TEST_PYTHON")) c << qEnvironmentVariable("PLAYPLAY_TEST_PYTHON");
    c << "C:/Program Files/Python311/python.exe" << "C:/Program Files/Python312/python.exe" << "C:/Program Files/Python313/python.exe";   // spotgenerator.cpp's list
    const QString onPath = QStandardPaths::findExecutable("python");
    if (!onPath.isEmpty()) c << onPath;
    for (const QString& p : c) {
        if (!QFileInfo::exists(p)) continue;
        QProcess q; q.start(p, {"-I", "-c", needNumpy ? "import numpy" : "pass"});
        if (q.waitForFinished(30000) && q.exitStatus() == QProcess::NormalExit && q.exitCode() == 0) return p;
    }
    return {};
}

// the installed-voice lists the map is checked against
const QStringList kHere = {"Microsoft David Desktop", "Microsoft Zira Desktop", "Microsoft David", "Microsoft Mark", "Microsoft Zira"};
const QStringList kDesktopOnly = {"Microsoft David Desktop", "Microsoft Zira Desktop"};       // what System.Speech lists here
const QStringList kUk = {"Microsoft Hazel Desktop", "Microsoft George", "Microsoft David Desktop", "Microsoft Zira Desktop"};
const QStringList kOneCoreOnly = {"Microsoft David", "Microsoft Mark", "Microsoft Zira"};
const QStringList kNeural = {"Microsoft Aria Online (Natural)", "Microsoft Guy Online (Natural)"};
const QStringList kVoiceProbe = {"af_heart", "af_bella", "am_michael", "am_adam", "bf_emma", "bf_isabella", "bm_george", "bm_lewis",
                                 "ef_dora", "em_alex", "ff_siwis", "fm_x", "f", "m", "Mark", "zf_xiaobei", "jf_alpha", "hf_alpha",
                                 "Microsoft Mark", "microsoft mark", "  MICROSOFT ZIRA  ", "Microsoft Hazel Desktop", "Microsoft Zira Desktop",
                                 "", "   ", "xyz", "AF_HEART", "\tam_echo\n"};

}   // namespace

class TestInhouseSpeech : public QObject {
    Q_OBJECT
    QTemporaryDir m_tmp;
    QString m_dir;                              // a non-ASCII folder inside m_tmp
private slots:
    void initTestCase() {
        QVERIFY2(QFileInfo::exists(kExe), qPrintable("missing " + kExe));
        QVERIFY(m_tmp.isValid());
        m_dir = m_tmp.filePath(QString::fromUtf8("sp\xC3\xB6t \xC3\xBCn\xC3\xAF\xE5\xA3\xB0"));   // "spöt ünï声"
        QVERIFY(QDir().mkpath(m_dir));
    }

    // ------------------------------------------------------------------------------------------ voicemap
    void sapiVoiceMap_data() {
        QTest::addColumn<QStringList>("installed");
        QTest::addColumn<QString>("voice");
        QTest::addColumn<QString>("expected");
        QTest::newRow("here af_heart -> Zira") << kHere << "af_heart" << "Microsoft Zira Desktop";
        QTest::newRow("here am_michael -> David") << kHere << "am_michael" << "Microsoft David Desktop";
        QTest::newRow("here bm_george -> David (no George)") << kHere << "bm_george" << "Microsoft David Desktop";
        QTest::newRow("here bm_lewis -> David") << kHere << "bm_lewis" << "Microsoft David Desktop";
        QTest::newRow("here bf_emma -> Zira (no Hazel)") << kHere << "bf_emma" << "Microsoft Zira Desktop";
        QTest::newRow("here exact OneCore Mark") << kHere << "Microsoft Mark" << "Microsoft Mark";
        QTest::newRow("here exact, any case") << kHere << "microsoft mark" << "Microsoft Mark";
        QTest::newRow("here exact, stripped") << kHere << "  MICROSOFT ZIRA  " << "Microsoft Zira";
        QTest::newRow("here empty") << kHere << "" << "";
        QTest::newRow("here blank") << kHere << "   " << "";
        QTest::newRow("here unknown") << kHere << "xyz" << "";
        QTest::newRow("here 'Mark' is male -> David") << kHere << "Mark" << "Microsoft David Desktop";
        QTest::newRow("here missing Hazel name starts with m") << kHere << "Microsoft Hazel Desktop" << "Microsoft David Desktop";
        QTest::newRow("here ef_ -> female") << kHere << "ef_dora" << "Microsoft Zira Desktop";
        QTest::newRow("here fm_ -> male") << kHere << "fm_x" << "Microsoft David Desktop";
        QTest::newRow("here zf_ -> none") << kHere << "zf_xiaobei" << "";
        QTest::newRow("desktop af_heart") << kDesktopOnly << "af_heart" << "Microsoft Zira Desktop";
        QTest::newRow("desktop Mark not installed -> David") << kDesktopOnly << "Microsoft Mark" << "Microsoft David Desktop";
        QTest::newRow("uk bf_emma -> Hazel") << kUk << "bf_emma" << "Microsoft Hazel Desktop";
        QTest::newRow("uk bm_george -> George") << kUk << "bm_george" << "Microsoft George";
        QTest::newRow("uk af_heart -> Zira") << kUk << "af_heart" << "Microsoft Zira Desktop";
        QTest::newRow("uk am_adam -> David") << kUk << "am_adam" << "Microsoft David Desktop";
        QTest::newRow("onecore af_heart") << kOneCoreOnly << "af_heart" << "Microsoft Zira";
        QTest::newRow("onecore am_michael") << kOneCoreOnly << "am_michael" << "Microsoft David";
        QTest::newRow("neural af_heart -> Aria") << kNeural << "af_heart" << "Microsoft Aria Online (Natural)";
        QTest::newRow("neural am_michael -> Guy") << kNeural << "am_michael" << "Microsoft Guy Online (Natural)";
        QTest::newRow("none") << QStringList() << "af_heart" << "";
    }
    void sapiVoiceMap() {
        QFETCH(QStringList, installed); QFETCH(QString, voice); QFETCH(QString, expected);
        QCOMPARE(voicemap::sapiVoiceFor(voice, installed), expected);
    }

    // the same function in the script, over every probe voice x every list
    void sapiVoiceMapMatchesPython() {
        const QString py = findPython(false);
        if (py.isEmpty()) QSKIP("no Python here (set PLAYPLAY_TEST_PYTHON)");
        const QList<QStringList> lists = {kHere, kDesktopOnly, kUk, kOneCoreOnly, kNeural, {}};
        QJsonArray jl; for (const QStringList& l : lists) jl.append(QJsonArray::fromStringList(l));
        const QString job = m_tmp.filePath("map.json");
        QVERIFY(writeText(job, QJsonDocument(QJsonObject{{"lists", jl}, {"voices", QJsonArray::fromStringList(kVoiceProbe)}}).toJson()));
        QProcess q;
        q.start(py, {"-I", "-c", "import sys, json; sys.path.insert(0, sys.argv[1]); import spot_tts as t; j = json.load(open(sys.argv[2], encoding='utf-8'));"
                                 " print(json.dumps([[t.sapi_voice_for(v, l) for v in j['voices']] for l in j['lists']]))", kScripts, job});
        QVERIFY(q.waitForFinished(60000));
        QCOMPARE(q.exitCode(), 0);
        const QJsonArray got = QJsonDocument::fromJson(q.readAllStandardOutput()).array();
        QCOMPARE(got.size(), lists.size());
        int n = 0;
        for (int i = 0; i < lists.size(); ++i)
            for (int k = 0; k < kVoiceProbe.size(); ++k, ++n)
                QVERIFY2(voicemap::sapiVoiceFor(kVoiceProbe[k], lists[i]) == got[i].toArray()[k].toString(),
                         qPrintable(QString("list %1 voice '%2': native '%3', python '%4'").arg(i).arg(kVoiceProbe[k], voicemap::sapiVoiceFor(kVoiceProbe[k], lists[i]), got[i].toArray()[k].toString())));
        QCOMPARE(n, 6 * int(kVoiceProbe.size()));
    }

    void kokoroVoiceTable() {
        const QStringList v = voicemap::kokoroVoices();
        QCOMPARE(v.size(), qsizetype(11));
        QCOMPARE(voicemap::kokoroSpeakerId("af"), 0);
        QCOMPARE(voicemap::kokoroSpeakerId("am_michael"), 6);
        QCOMPARE(voicemap::kokoroSpeakerId(" BM_LEWIS "), 10);
        QCOMPARE(voicemap::kokoroSpeakerId("af_heart"), -1);
        const QList<QPair<QString, QString>> picks = {{"af_heart", "af_bella"}, {"af_sky", "af_sky"}, {"AF_Nicole", "af_nicole"}, {"am_echo", "am_michael"},
                                                      {"am_adam", "am_adam"}, {"bf_alice", "bf_emma"}, {"bf_isabella", "bf_isabella"}, {"bm_daniel", "bm_george"},
                                                      {"bm_lewis", "bm_lewis"}, {"", "am_michael"}, {"zf_xiaobei", "am_michael"}, {"Microsoft Zira", "am_michael"}};
        for (const auto& p : picks) QCOMPARE(voicemap::kokoroPickVoice(p.first), p.second);
    }

    void speedAndRate() {
        // int(round((max(0.5, min(2.0, float(s or 1.0))) - 1.0) * 10.0)) printed by CPython 3.11
        const QList<QPair<double, int>> rates = {{0.5, -5}, {0.55, -4}, {0.75, -2}, {0.8, -2}, {0.85, -2}, {0.95, -1}, {1.0, 0}, {1.05, 1}, {1.1, 1},
                                                 {1.15, 1}, {1.2, 2}, {1.25, 2}, {1.35, 4}, {1.45, 4}, {1.5, 5}, {1.65, 6}, {1.75, 8}, {1.95, 10}, {2.0, 10}};
        for (const auto& r : rates) QVERIFY2(voicemap::sapiRate(voicemap::clampSpeed(r.first)) == r.second, qPrintable(QString::number(r.first)));
        QCOMPARE(voicemap::clampSpeed(0.0), 1.0);          // `a.speed or 1.0`
        QCOMPARE(voicemap::clampSpeed(-0.0), 1.0);
        QCOMPARE(voicemap::clampSpeed(NAN), 2.0);          // min(2.0, nan) keeps 2.0
        QCOMPARE(voicemap::clampSpeed(INFINITY), 2.0);
        QCOMPARE(voicemap::clampSpeed(-INFINITY), 0.5);
        QCOMPARE(voicemap::clampSpeed(-3.0), 0.5);
        QCOMPARE(voicemap::clampSpeed(7.0), 2.0);
        QCOMPARE(voicemap::sapiRate(9.0), 10);             // SAPI's range
        QCOMPARE(voicemap::sapiRate(-9.0), -10);
    }

    // ------------------------------------------------------------------------------------------ text rules
    void textRules() {
        using namespace ttscli;
        QCOMPARE(collapseWhitespace(QString::fromUtf8(" \t Hello,\r\n\r\n  world.\xC2\xA0\xE3\x80\x80Next\x1c" "one \x0b")), QString("Hello, world. Next one"));
        QCOMPARE(collapseWhitespace(QString::fromUtf8("\xE2\x80\x8B" "zero\xE2\x80\x8Bwidth")), QString::fromUtf8("\xE2\x80\x8B" "zero\xE2\x80\x8Bwidth"));   // U+200B is not \s
        QCOMPARE(collapseWhitespace("   "), QString());
        QCOMPARE(wordCount("Hello, world. Next one"), 4);
        QCOMPARE(wordCount(""), 0);
        QCOMPARE(pyHead(QString::fromUtf8("ab\xF0\x9F\x98\x80" "cd"), 3), QString::fromUtf8("ab\xF0\x9F\x98\x80"));      // code points, not UTF-16 units
        QCOMPARE(pyHead("abcdef", -2), QString("abcd"));
        QCOMPARE(pyHead("abc", 10), QString("abc"));
        // cut at the last ". " when one is in the second half, else hard
        QCOMPARE(cutNarration("One two. Three four. Five six seven", 24), QString("One two. Three four."));
        QCOMPARE(cutNarration("One. Two three four five six seven eight", 20), QString("One. Two three four "));
        QCOMPARE(cutNarration("Short. Text.", 100), QString("Short. Text."));
        QCOMPARE(round3(7.5745), 7.574);                   // 7.5745 is just below the tie as a double
        QCOMPARE(round3(0.0625), 0.062);                   // an exact tie: to even
        QCOMPARE(round3(0.0635), 0.064);
        QCOMPARE(round3(363576.0 / 48000.0), 7.574);
        double d = 0; int i = 0;
        QVERIFY(parsePyFloat(" 1.25 ", &d) && d == 1.25);
        QVERIFY(parsePyFloat("1_0", &d) && d == 10.0);
        QVERIFY(parsePyFloat("-inf", &d) && std::isinf(d) && d < 0);
        QVERIFY(parsePyFloat("NaN", &d) && std::isnan(d));
        QVERIFY(parsePyFloat("1e0", &d) && d == 1.0);
        QVERIFY(!parsePyFloat("fast", &d));
        QVERIFY(!parsePyFloat("+-1", &d));
        QVERIFY(!parsePyFloat("1__0", &d));
        QVERIFY(!parsePyFloat("", &d));
        QVERIFY(parsePyInt(" 12_000 ", &i) && i == 12000);
        QVERIFY(parsePyInt("-5", &i) && i == -5);
        QVERIFY(parsePyInt("99999999999999999999", &i) && i == INT_MAX);
        QVERIFY(!parsePyInt("1.5", &i));
        QVERIFY(!parsePyInt("_1", &i));
    }

    void textRulesMatchPython() {
        const QString py = findPython(false);
        if (py.isEmpty()) QSKIP("no Python here (set PLAYPLAY_TEST_PYTHON)");
        const QStringList texts = {QString::fromUtf8(" \t Hello,\r\n\r\n  world.\xC2\xA0\xE3\x80\x80Next\x1c" "one \x0b"),
                                   "One two. Three four. Five six seven", "One. Two three four five six seven eight",
                                   QString::fromUtf8("Caf\xC3\xA9 \xF0\x9F\x98\x80 news. Weather. Sports\xE2\x80\x83scores. The end of the hour. Goodnight."),
                                   "A. B. C. D. E. F. G. H. I. J. K. L. M. N. O. P.", "no-period-at-all-in-this-text-just-words-and-dashes"};
        const QList<int> limits = {5, 12, 20, 24, 37, 100, 1, 0, -3};
        const QString job = m_tmp.filePath("text.json");
        QJsonArray li; for (int l : limits) li.append(l);
        QVERIFY(writeText(job, QJsonDocument(QJsonObject{{"texts", QJsonArray::fromStringList(texts)}, {"limits", li}}).toJson()));
        // the lines of spot_tts.py main() / run_batch(), verbatim
        const char* code =
            "import sys, json, re\n"
            "j = json.load(open(sys.argv[1], encoding='utf-8')); out = []\n"
            "for raw in j['texts']:\n"
            "  for mc in j['limits']:\n"
            "    text = re.sub(r'\\s+', ' ', raw).strip()\n"
            "    batch = text[: mc]\n"
            "    if len(text) > mc:\n"
            "        cut = text[: mc]\n"
            "        text = cut[: cut.rfind('. ') + 1] if '. ' in cut[mc // 2:] else cut\n"
            "    out.append([text, len(text.split()), batch, len(batch.split())])\n"
            "sys.stdout.buffer.write(json.dumps(out).encode('utf-8'))\n";
        QProcess q; q.start(py, {"-I", "-c", QString::fromLatin1(code), job});
        QVERIFY(q.waitForFinished(60000));
        QCOMPARE(q.exitCode(), 0);
        const QJsonArray got = QJsonDocument::fromJson(q.readAllStandardOutput()).array();
        QCOMPARE(got.size(), qsizetype(texts.size() * limits.size()));
        int k = 0;
        for (const QString& raw : texts)
            for (int mc : limits) {
                const QJsonArray e = got[k++].toArray();
                const QString collapsed = ttscli::collapseWhitespace(raw);
                const QString single = ttscli::cutNarration(collapsed, mc), batch = ttscli::pyHead(collapsed, mc);
                const QByteArray tag = (raw.left(20) + " / " + QString::number(mc)).toUtf8();
                QVERIFY2(single == e[0].toString(), tag.constData());
                QVERIFY2(ttscli::wordCount(single) == e[1].toInt(), tag.constData());
                QVERIFY2(batch == e[2].toString(), tag.constData());
                QVERIFY2(ttscli::wordCount(batch) == e[3].toInt(), tag.constData());
            }
    }

    // ------------------------------------------------------------------------------------------ voices
    void voicesListed() {
        const Run r = runTool({"voices"});
        QVERIFY2(r.code == 0, describe(r).constData());
        const QJsonArray v = r.json.value("voices").toArray();
        QVERIFY(!v.isEmpty());
        QStringList names, cats;
        for (const QJsonValue& x : v) {
            const QJsonObject o = x.toObject();
            QVERIFY(!o.value("name").toString().isEmpty());
            QVERIFY(o.value("id").toString().startsWith("HKEY_"));
            names << o.value("name").toString(); cats << o.value("category").toString();
        }
        QVERIFY(cats.contains("desktop"));
        QVERIFY(!r.json.value("defaultVoice").toString().isEmpty());
        // desktop voices are listed first (the order sapiVoiceFor walks, as System.Speech's list did)
        QCOMPARE(cats.first(), QString("desktop"));
        QVERIFY(cats.lastIndexOf("desktop") < (cats.indexOf("onecore") < 0 ? int(cats.size()) : int(cats.indexOf("onecore"))));
        // in-process enumeration agrees with the exe
        sapitts::ComScope com; QVERIFY(com.ok());
        QCOMPARE(sapitts::voiceNames(sapitts::installedVoices()), names);
        if (!names.contains("Microsoft Mark")) QSKIP("no OneCore 'Microsoft Mark' voice on this machine");
        QCOMPARE(cats[names.indexOf("Microsoft Mark")], QString("onecore"));
    }

    // ------------------------------------------------------------------------------------------ tts single
    void ttsSingle_data() {
        QTest::addColumn<QString>("voice");
        QTest::addColumn<QString>("speed");
        QTest::addColumn<QString>("expectVoice");
        QTest::newRow("af_heart -> Zira") << "af_heart" << "1" << "Microsoft Zira Desktop";
        QTest::newRow("am_michael -> David") << "am_michael" << "1.0" << "Microsoft David Desktop";
        QTest::newRow("bm_george -> David") << "bm_george" << "1.0" << "Microsoft David Desktop";
        QTest::newRow("OneCore Mark") << "Microsoft Mark" << "1.0" << "Microsoft Mark";
        QTest::newRow("OneCore Zira, any case") << "microsoft zira" << "1.0" << "Microsoft Zira";
    }
    void ttsSingle() {
        QFETCH(QString, voice); QFETCH(QString, speed); QFETCH(QString, expectVoice);
        sapitts::ComScope com; QVERIFY(com.ok());
        if (!sapitts::voiceNames(sapitts::installedVoices()).contains(expectVoice)) QSKIP(qPrintable(expectVoice + " is not installed here"));
        const QString txt = QDir(m_dir).filePath("narration.txt");
        QVERIFY(writeText(txt, "Good evening, Martinsville.\r\n  Here is tonight's   weather:\tclear skies and a low of 41 degrees."));
        const QString out = QDir(m_dir).filePath("sub/voice " + QString(QTest::currentDataTag()).replace(QRegularExpression("[^A-Za-z0-9]+"), "_") + ".wav");
        const Run r = runTool({"tts", "--text-file", txt, "--out", out, "--voice", voice, "--speed", speed, "--engine", "sapi"});
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(int(r.out.count('\n')), 1);                 // exactly one stdout line: the result
        QCOMPARE(sortedKeys(r.json), kSingleKeys);
        QCOMPARE(r.json.value("engine").toString(), QString("sapi"));
        QCOMPARE(r.json.value("voiceUsed").toString(), expectVoice);
        QCOMPARE(r.json.value("tried").toArray(), QJsonArray{"sapi"});
        QCOMPARE(r.json.value("path").toString(), nativeAbs(out));
        QCOMPARE(r.json.value("sampleRate").toInt(), 48000);
        QCOMPARE(r.json.value("words").toInt(), 15);
        checkVoiceWav(out, r.json.value("durationSec").toDouble());
        QVERIFY(QDir(QFileInfo(out).absolutePath()).entryList({"*.part.wav"}, QDir::Files).isEmpty());   // no temp file left
    }

    void ttsSpeedChangesRate() {
        const QString txt = QDir(m_dir).filePath("speed.txt");
        QVERIFY(writeText(txt, "The quick brown fox jumps over the lazy dog, and then the weather turns to rain by morning."));
        QMap<QString, double> dur;
        for (const QString& s : {QString("0.75"), QString("1.0"), QString("1.5")}) {
            const QString out = QDir(m_dir).filePath("speed_" + s + ".wav");
            const Run r = runTool({"tts", "--text-file", txt, "--out", out, "--voice", "af_heart", "--speed", s, "--engine", "sapi"});
            QVERIFY2(r.code == 0, describe(r).constData());
            dur[s] = r.json.value("durationSec").toDouble();
        }
        QVERIFY2(dur["0.75"] > dur["1.0"] * 1.1 && dur["1.5"] < dur["1.0"] * 0.8,
                 qPrintable(QString("0.75: %1 s, 1.0: %2 s, 1.5: %3 s").arg(dur["0.75"]).arg(dur["1.0"]).arg(dur["1.5"])));
    }

    // ------------------------------------------------------------------------------------------ tts batch
    void ttsBatch() {
        if (!voiceInstalled("Microsoft David Desktop")) QSKIP("Microsoft David Desktop is not installed here");
        const QString bdir = QDir(m_dir).filePath("batch");
        const QStringList texts = {"First story:\n the city council met tonight\tand voted on the budget.",
                                   "  Second story: schools open two hours late tomorrow.  ",
                                   "And finally, sports."};
        QJsonArray items;
        for (int i = 0; i < texts.size(); ++i) items.append(QJsonObject{{"text", texts[i]}, {"out", QDir(bdir).filePath(QString("voice_%1.wav").arg(i, 2, 10, QChar('0')))}});
        const QString job = QDir(m_dir).filePath("voices.json");
        QVERIFY(writeText(job, "\xEF\xBB\xBF" + QJsonDocument(QJsonObject{{"items", items}}).toJson()));   // a BOM, like PowerShell writes
        QVERIFY(writeText(QDir(bdir).filePath("voice_01.wav"), "stale"));                                  // removed before synthesis
        const Run r = runTool({"tts", "--batch", job, "--voice", "am_michael", "--speed", "1.0", "--engine", "sapi"});
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(sortedKeys(r.json), kBatchKeys);
        QCOMPARE(r.json.value("engine").toString(), QString("sapi"));
        QCOMPARE(r.json.value("voiceUsed").toString(), QString("Microsoft David Desktop"));
        QCOMPARE(r.json.value("tried").toArray(), QJsonArray{"sapi"});
        const QJsonArray got = r.json.value("items").toArray();
        QCOMPARE(got.size(), qsizetype(3));
        const int words[] = {12, 8, 3};
        for (int i = 0; i < 3; ++i) {
            const QJsonObject it = got[i].toObject();
            QCOMPARE(sortedKeys(it), kItemKeys);
            QCOMPARE(it.value("path").toString(), nativeAbs(items[i].toObject().value("out").toString()));
            QCOMPARE(it.value("words").toInt(), words[i]);
            wavio::Wav w;
            checkVoiceWav(it.value("path").toString(), it.value("durationSec").toDouble(), &w);
            if (QTest::currentTestFailed()) return;
            double lead = 0, tail = 0; quietEdgesMs(w, &lead, &tail);
            // trimmed to 80 ms margins (levelling may move the -42 dBFS crossing by a few ms either way)
            QVERIFY2(lead > 40 && lead < 130 && tail > 40 && tail < 130, qPrintable(QString("item %1: lead %2 ms, tail %3 ms").arg(i).arg(lead).arg(tail)));
        }
        // the single mode does not trim: the same text voiced alone keeps SAPI's padding and is longer
        const QString txt = QDir(m_dir).filePath("one.txt");
        QVERIFY(writeText(txt, texts[2].toUtf8()));
        const Run s = runTool({"tts", "--text-file", txt, "--out", QDir(m_dir).filePath("one.wav"), "--voice", "am_michael", "--engine", "sapi"});
        QVERIFY2(s.code == 0, describe(s).constData());
        QVERIFY2(s.json.value("durationSec").toDouble() > got[2].toObject().value("durationSec").toDouble() + 0.1,
                 qPrintable(QString("single %1 s, batch %2 s").arg(s.json.value("durationSec").toDouble()).arg(got[2].toObject().value("durationSec").toDouble())));
    }

    // ------------------------------------------------------------------------------------------ engine plan
    void engineContract() {
        const QString txt = QDir(m_dir).filePath("plan.txt");
        QVERIFY(writeText(txt, "Engine selection test."));
        const QString out = QDir(m_dir).filePath("plan.wav");
        QFile::remove(out);
        // kokoro is not in this build: refused (64) before any file is touched, so the caller can run spot_tts.py
        Run r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "kokoro"});
        QVERIFY2(r.code == 64, describe(r).constData());
        QVERIFY(r.json.value("unsupported").toBool());
        QCOMPARE(r.json.value("error").toString(), QString("unsupported: --engine kokoro"));
        QVERIFY(!QFileInfo::exists(out));
        // auto while a Kokoro model + the sherpa-onnx runtime are installed (fake ones here): refused the same way
        const QString fakeModel = m_tmp.filePath("kokoro-model"), fakeSherpa = m_tmp.filePath("sherpa");
        for (const char* f : {"model.int8.onnx", "voices.bin", "tokens.txt"}) QVERIFY(writeText(QDir(fakeModel).filePath(f), "x"));
        QVERIFY(QDir().mkpath(QDir(fakeModel).filePath("espeak-ng-data")));
        QVERIFY(writeText(QDir(fakeSherpa).filePath("sherpa-onnx-c-api.dll"), "x"));
        r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "auto", "--kokoro-model", fakeModel}, envWith("PLAYPLAY_SHERPA_ROOT", fakeSherpa));
        QVERIFY2(r.code == 64, describe(r).constData());
        QCOMPARE(r.json.value("error").toString(), QString("unsupported: --engine auto"));
        QVERIFY(r.json.value("kokoro").toObject().value("found").toBool());
        QVERIFY(!QFileInfo::exists(out));
        // in process: the same resolution
        ttscli::Options o; o.kokoroModel = fakeModel;
        qputenv("PLAYPLAY_SHERPA_ROOT", fakeSherpa.toLocal8Bit());
        ttscli::KokoroAssets k = ttscli::findKokoro(o);
        qunsetenv("PLAYPLAY_SHERPA_ROOT");
        QVERIFY(k.found());
        QCOMPARE(QDir::cleanPath(k.modelDir), QDir::cleanPath(fakeModel));
        QCOMPARE(QDir::cleanPath(k.sherpaDir), QDir::cleanPath(fakeSherpa));
        qputenv("PLAYPLAY_SPEECH_KOKORO", "0");
        k = ttscli::findKokoro(o);
        qunsetenv("PLAYPLAY_SPEECH_KOKORO");
        QVERIFY(k.disabled && !k.found());
        // auto without Kokoro: SAPI, and the script's tried list shape
        r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "auto", "--voice", "af_heart"}, noKokoro());
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(r.json.value("engine").toString(), QString("sapi"));
        QCOMPARE(r.json.value("tried").toArray(), (QJsonArray{"kokoro", "sapi"}));
        checkVoiceWav(out, r.json.value("durationSec").toDouble());
        // env reports the same decision
        r = runTool({"env", "--kokoro-model", fakeModel}, envWith("PLAYPLAY_SHERPA_ROOT", fakeSherpa));
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(r.json.value("auto").toString(), QString("unsupported"));
        QVERIFY(r.json.value("engines").toObject().value("sapi").toObject().value("available").toBool());
        r = runTool({"env"}, noKokoro());
        QCOMPARE(r.json.value("auto").toString(), QString("sapi"));
    }

    // pathlib's Path.parent, which spot_tts.py uses for dir/../kokoro_models and the OBS venv
    void pyParentIsPathlibs() {
        const QList<QPair<QString, QString>> cases = {
            {"C:/x/tools", "C:/x"}, {"C:\\x\\tools\\", "C:/x"}, {"C:/x/tools/.", "C:/x"}, {"C:/x/tools/..", "C:/x/tools"}, {"C:/x//y", "C:/x"},
            {"tools", "."}, {"a/b", "a"}, {".", "."}, {"..", "."}, {"C:/", "C:/"}, {"C:x", "C:"}, {"/x/y", "/x"},
            {"\\\\server\\share\\dir", "//server/share/"}, {"\\\\server\\share\\", "//server/share/"}};
        for (const auto& c : cases) QVERIFY2(ttscli::pyParent(c.first) == c.second, qPrintable(c.first + " -> " + ttscli::pyParent(c.first)));
        const QString py = findPython(false);
        if (py.isEmpty()) QSKIP("no Python here (set PLAYPLAY_TEST_PYTHON)");
        QStringList in; for (const auto& c : cases) in << c.first;
        const QString job = m_tmp.filePath("parents.json");
        QVERIFY(writeText(job, QJsonDocument(QJsonArray::fromStringList(in)).toJson()));
        QProcess q;
        q.start(py, {"-I", "-c", "import sys, json, pathlib; print(json.dumps([str(pathlib.PureWindowsPath(p).parent) for p in json.load(open(sys.argv[1], encoding='utf-8'))]))", job});
        QVERIFY(q.waitForFinished(60000));
        QCOMPARE(q.exitCode(), 0);
        const QJsonArray got = QJsonDocument::fromJson(q.readAllStandardOutput()).array();
        QCOMPARE(got.size(), in.size());
        for (int i = 0; i < in.size(); ++i)
            QVERIFY2(ttscli::pyParent(in[i]) == QDir::fromNativeSeparators(got[i].toString()), qPrintable(in[i] + ": native " + ttscli::pyParent(in[i]) + ", pathlib " + got[i].toString()));
    }

    // `auto` hands the run back (64) whenever spot_tts.py would try a Kokoro this tool cannot run: kokoro_onnx's model
    // files or the PlayPlay OBS venv under --kokoro-dir, or the sherpa model through sherpa-onnx-node
    void autoDefersToScriptKokoro() {
        const QString root = m_tmp.filePath("kokoro-layouts");
        // a PlayPlay OBS install: C:\PlayPlayOBS\tools (venv + worker) and C:\PlayPlayOBS\kokoro_models
        const QString obs = QDir(root).filePath("obs"), tools = QDir(obs).filePath("tools");
        QVERIFY(writeText(QDir(tools).filePath("ttsvenv/Scripts/python.exe"), "x"));
        QVERIFY(writeText(QDir(tools).filePath("kokoro_synth.py"), "x"));
        QVERIFY(writeText(QDir(obs).filePath("kokoro_models/kokoro-v1.0.onnx"), "x"));
        QVERIFY(writeText(QDir(obs).filePath("kokoro_models/voices-v1.0.bin"), "x"));
        // the venv found through dir/../tools, the models in dir/models
        const QString obs2 = QDir(root).filePath("obs2"), other = QDir(obs2).filePath("other");
        QVERIFY(writeText(QDir(obs2).filePath("tools/ttsvenv/Scripts/python.exe"), "x"));
        QVERIFY(writeText(QDir(obs2).filePath("tools/kokoro_synth.py"), "x"));
        QVERIFY(writeText(QDir(other).filePath("models/kokoro-v1.0.onnx"), "x"));
        QVERIFY(writeText(QDir(other).filePath("models/voices-v1.0.bin"), "x"));
        // incomplete: a model without its voices, a python without the worker
        const QString partial = QDir(root).filePath("partial");
        QVERIFY(writeText(QDir(partial).filePath("kokoro-v1.0.onnx"), "x"));
        QVERIFY(writeText(QDir(partial).filePath("Scripts/python.exe"), "x"));
        const QString empty = QDir(root).filePath("empty");
        QVERIFY(QDir().mkpath(empty));

        const auto find = [](const QString& dir) { ttscli::Options o; o.kokoroDir = dir; return ttscli::findKokoro(o); };
        const auto same = [](const QString& a, const QString& b) { return QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath() && !a.isEmpty(); };
        ttscli::KokoroAssets k = find(tools);
        QVERIFY2(same(k.onnxModelDir, QDir(obs).filePath("kokoro_models")), qPrintable(k.onnxModelDir));
        QVERIFY(same(k.venvPython, QDir(tools).filePath("ttsvenv/Scripts/python.exe")));
        QVERIFY(same(k.venvSynth, QDir(tools).filePath("kokoro_synth.py")));
        QCOMPARE(k.scriptEngines().mid(0, 2), QStringList({"kokoro_onnx", "kokoro_venv"}));   // kokoro_sherpa follows where StarMedia is installed
        k = find(other);
        QVERIFY(same(k.onnxModelDir, QDir(other).filePath("models")));
        QVERIFY(same(k.venvPython, QDir(obs2).filePath("tools/ttsvenv/Scripts/python.exe")));
        QVERIFY(same(k.venvSynth, QDir(obs2).filePath("tools/kokoro_synth.py")));
        k = find(partial);
        QVERIFY(k.onnxModelDir.isEmpty() && k.venvSynth.isEmpty() && !k.venvPython.isEmpty());
        QVERIFY(!k.scriptEngines().contains("kokoro_onnx") && !k.scriptEngines().contains("kokoro_venv"));
        k = find(empty);
        QVERIFY(k.onnxModelDir.isEmpty() && k.venvPython.isEmpty() && k.venvSynth.isEmpty());
        qputenv("PLAYPLAY_SPEECH_KOKORO", "0");
        k = find(tools);
        qunsetenv("PLAYPLAY_SPEECH_KOKORO");
        QVERIFY(k.disabled && k.scriptEngines().isEmpty());

        // the plan, with the lookup's result built by hand (this machine also has the StarMedia sherpa install)
        const auto plan = [](const ttscli::KokoroAssets& a, const QString& engine, QStringList* order) {
            ttscli::Options o; o.engine = engine; QJsonObject refusal;
            const bool ok = ttscli::planEngines(o, a, order, &refusal);
            return ok ? QString() : refusal.value("error").toString() + " | " + refusal.value("reason").toString();
        };
        QStringList order;
        ttscli::KokoroAssets a;
        QCOMPARE(plan(a, "auto", &order), QString());
        QCOMPARE(order, QStringList({"kokoro", "sapi"}));
        a.onnxModelDir = "m";
        QVERIFY(plan(a, "auto", &order).contains("unsupported: --engine auto | spot_tts.py would voice this with Kokoro (kokoro_onnx)"));
        QCOMPARE(plan(a, "sapi", &order), QString());
        QCOMPARE(order, QStringList({"sapi"}));
        a = {}; a.venvPython = "p";
        QCOMPARE(plan(a, "auto", &order), QString());                 // a python without the worker: the script cannot use it
        a.venvSynth = "s";
        QVERIFY(plan(a, "auto", &order).contains("(kokoro_venv)"));
        a = {}; a.modelDir = "m"; a.sherpaNodeDir = "n";               // the script's Node path, no C API DLL for DEP-11
        QVERIFY(plan(a, "auto", &order).contains("(kokoro_sherpa)"));
        a.sherpaNodeDir.clear(); a.sherpaDir = "d";
        QVERIFY(plan(a, "auto", &order).contains("Kokoro is installed"));
        a = {}; a.onnxModelDir = "m"; a.disabled = true;               // PLAYPLAY_SPEECH_KOKORO=0: SAPI
        QCOMPARE(plan(a, "auto", &order), QString());

        // through the exe: 64 before any file is touched, naming the layouts
        const QString txt = QDir(m_dir).filePath("obs.txt"), out = QDir(m_dir).filePath("obs.wav");
        QVERIFY(writeText(txt, "Kokoro layout test."));
        QFile::remove(out);
        Run r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "auto", "--kokoro-dir", tools});
        QVERIFY2(r.code == 64, describe(r).constData());
        QCOMPARE(r.json.value("error").toString(), QString("unsupported: --engine auto"));
        QVERIFY2(r.json.value("reason").toString().contains("kokoro_onnx, kokoro_venv"), describe(r).constData());
        const QJsonObject kj = r.json.value("kokoro").toObject();
        QVERIFY(same(kj.value("onnxModelDir").toString(), QDir(obs).filePath("kokoro_models")));
        QVERIFY(same(kj.value("venvPython").toString(), QDir(tools).filePath("ttsvenv/Scripts/python.exe")));
        QVERIFY(!QFileInfo::exists(out));
        r = runTool({"env", "--kokoro-dir", tools});
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(r.json.value("auto").toString(), QString("unsupported"));
        const QJsonArray se = r.json.value("engines").toObject().value("kokoro").toObject().value("scriptEngines").toArray();
        QVERIFY(se.contains(QJsonValue("kokoro_onnx")) && se.contains(QJsonValue("kokoro_venv")));
        // the batch form refuses the same way, before any output is touched
        const QString job = QDir(m_dir).filePath("obs.json"), bout = QDir(m_dir).filePath("obs_b.wav");
        QVERIFY(writeText(job, QJsonDocument(QJsonObject{{"items", QJsonArray{QJsonObject{{"text", "Batch layout test."}, {"out", bout}}}}}).toJson()));
        QVERIFY(writeText(bout, "stale"));
        r = runTool({"tts", "--batch", job, "--engine", "auto", "--kokoro-dir", other});
        QVERIFY2(r.code == 64, describe(r).constData());
        QFile stale(bout); QVERIFY(stale.open(QIODevice::ReadOnly)); QCOMPARE(stale.readAll(), QByteArray("stale"));

        // the script's own lookups agree: find_kokoro_models, and the python / worker try_kokoro_venv would start
        const QString py = findPython(false);
        if (py.isEmpty()) QSKIP("no Python here (set PLAYPLAY_TEST_PYTHON)");
        const QStringList dirs = {tools, tools + "/", tools + "/.", QDir(tools).filePath("ttsvenv/.."), other, partial, empty, QDir(root).filePath("missing")};
        const QString spec = m_tmp.filePath("layouts.json");
        QVERIFY(writeText(spec, QJsonDocument(QJsonArray::fromStringList(dirs)).toJson()));
        const char* code =
            "import sys, json, types\n"
            "sys.path.insert(0, sys.argv[1]); import spot_tts as t\n"
            "seen = []\n"
            "t.subprocess.run = lambda args, **kw: (seen.append(args), types.SimpleNamespace(returncode=1, stdout='', stderr=''))[1]\n"
            "out = []\n"
            "for d in json.load(open(sys.argv[2], encoding='utf-8')):\n"
            "    seen.clear(); m = t.find_kokoro_models(d); t.try_kokoro_venv('x', 'o.wav', 'af_heart', 1.0, d)\n"
            "    out.append([m[0] if m else '', str(seen[0][0]) if seen else '', str(seen[0][1]) if seen else ''])\n"
            "print(json.dumps(out))\n";
        QProcess q; q.start(py, {"-I", "-c", QString::fromLatin1(code), kScripts, spec});
        QVERIFY(q.waitForFinished(60000));
        QVERIFY2(q.exitCode() == 0, q.readAllStandardError().constData());
        const QJsonArray got = QJsonDocument::fromJson(q.readAllStandardOutput().trimmed().split('\n').last()).array();
        QCOMPARE(got.size(), dirs.size());
        const auto both = [&](const QString& native, const QString& script) { return native.isEmpty() ? script.isEmpty() : same(native, script); };
        for (int i = 0; i < dirs.size(); ++i) {
            const ttscli::KokoroAssets n = find(dirs[i]);
            const QJsonArray e = got[i].toArray();
            const QString pyModel = e[0].toString(), pyDir = pyModel.isEmpty() ? QString() : QFileInfo(pyModel).absolutePath();
            const QByteArray tag = (dirs[i] + ": native [" + n.onnxModelDir + ", " + n.venvPython + ", " + n.venvSynth + "] script ["
                                    + pyDir + ", " + e[1].toString() + ", " + e[2].toString() + "]").toUtf8();
            QVERIFY2(both(n.onnxModelDir, pyDir), tag.constData());
            const bool venv = !n.venvPython.isEmpty() && !n.venvSynth.isEmpty();
            QVERIFY2(venv ? same(n.venvPython, e[1].toString()) && same(n.venvSynth, e[2].toString()) : e[1].toString().isEmpty(), tag.constData());
        }
    }

    // a speak past its deadline ends the process at once with the failure line (no purge, no release, no CoUninitialize)
    void ttsDeadlineEndsTheProcess() {
        QString text;
        for (int i = 0; i < 40; ++i) text += QString("Sentence number %1 of a long narration that SAPI needs a while to render. ").arg(i + 1);
        const QString txt = QDir(m_dir).filePath("long.txt"), out = QDir(m_dir).filePath("hang/long.wav");
        QVERIFY(writeText(txt, text.toUtf8()));
        Run r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "sapi"}, envWith("PLAYPLAY_SPEECH_TIMEOUT_MS", "1"), 60000);
        QVERIFY2(r.code == 2, describe(r).constData());
        QCOMPARE(int(r.out.count('\n')), 1);
        QCOMPARE(r.json.value("error").toString(), QString("no TTS engine produced audio"));
        QCOMPARE(r.json.value("tried").toArray(), QJsonArray{"sapi"});
        QVERIFY2(r.err.contains("did not finish within") && r.err.contains("exiting without releasing the voice"), describe(r).constData());
        QVERIFY(!QFileInfo::exists(out));
        qInfo("deadline exit after %lld ms; temporary files left: %s", r.ms,
              qPrintable(QDir(QFileInfo(out).absolutePath()).entryList({"*.part.wav"}, QDir::Files).join(", ")));
        // batch: the first item's deadline ends the run; tried keeps the auto order
        const QString job = QDir(m_dir).filePath("hang.json");
        QVERIFY(writeText(job, QJsonDocument(QJsonObject{{"items", QJsonArray{QJsonObject{{"text", text}, {"out", QDir(m_dir).filePath("hang/b0.wav")}},
                                                                               QJsonObject{{"text", text}, {"out", QDir(m_dir).filePath("hang/b1.wav")}}}}}).toJson()));
        QProcessEnvironment env = noKokoro(); env.insert("PLAYPLAY_SPEECH_TIMEOUT_MS", "1");
        r = runTool({"tts", "--batch", job, "--engine", "auto"}, env, 60000);
        QVERIFY2(r.code == 2, describe(r).constData());
        QCOMPARE(int(r.out.count('\n')), 1);
        QCOMPARE(r.json.value("tried").toArray(), (QJsonArray{"kokoro", "sapi"}));
        QVERIFY(!QFileInfo::exists(QDir(m_dir).filePath("hang/b0.wav")) && !QFileInfo::exists(QDir(m_dir).filePath("hang/b1.wav")));
        // the run watchdog (a stuck voice set-up, a close or release that never returns): the same line, from its thread
        r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "sapi"}, envWith("PLAYPLAY_SPEECH_WATCHDOG_MS", "1"), 60000);
        QVERIFY2(r.code == 2, describe(r).constData());
        QCOMPARE(int(r.out.count('\n')), 1);
        QCOMPARE(r.json.value("error").toString(), QString("no TTS engine produced audio"));
        QVERIFY2(r.err.contains("speech watchdog"), describe(r).constData());
        QVERIFY(!QFileInfo::exists(out));
        // with neither knob the same text is voiced normally
        r = runTool({"tts", "--text-file", txt, "--out", out, "--engine", "sapi"});
        QVERIFY2(r.code == 0, describe(r).constData());
        checkVoiceWav(out, r.json.value("durationSec").toDouble());
    }

    // ------------------------------------------------------------------------------------------ errors
    void errorsFollowTheContract() {
        struct Case { QStringList args; int code; QString error; bool prefix; };
        const QString txt = QDir(m_dir).filePath("err.txt"), blank = QDir(m_dir).filePath("blank.txt");
        QVERIFY(writeText(txt, "Some narration."));
        QVERIFY(writeText(blank, " \r\n\t "));
        const QString b1 = QDir(m_dir).filePath("b_empty.json"), b2 = QDir(m_dir).filePath("b_blank.json"), b3 = QDir(m_dir).filePath("b_noout.json"), b4 = QDir(m_dir).filePath("b_bad.json");
        QVERIFY(writeText(b1, R"({"items": []})"));
        QVERIFY(writeText(b2, R"({"items": [{"text": "fine text", "out": "a.wav"}, {"text": "  ", "out": "b.wav"}]})"));
        QVERIFY(writeText(b3, R"({"items": [{"text": "fine text"}]})"));
        QVERIFY(writeText(b4, "not json"));
        const QList<Case> cases = {
            {{"tts"}, 2, "--text-file and --out are required (or --batch)", false},
            {{"tts", "--text-file", txt}, 2, "--text-file and --out are required (or --batch)", false},
            {{"tts", "--text-file", blank, "--out", QDir(m_dir).filePath("x.wav"), "--engine", "sapi"}, 2, "empty narration", false},
            {{"tts", "--text-file", QDir(m_dir).filePath("missing.txt"), "--out", QDir(m_dir).filePath("x.wav")}, 2, "cannot read text: ", true},
            {{"tts", "--text-file", txt, "--out", "x.wav", "--bogus", "1"}, 64, "unsupported: --bogus", false},
            {{"tts", "--serve"}, 64, "unsupported: --serve", false},
            {{"tts", "--text-file", txt, "--out", "x.wav", "--engine", "espeak"}, 2, "argument --engine: invalid choice: 'espeak' (choose from 'auto', 'kokoro', 'sapi')", false},
            {{"tts", "--text-file", txt, "--out", "x.wav", "--speed", "fast"}, 2, "argument --speed: invalid float value: 'fast'", false},
            {{"tts", "--text-file", txt, "--out", "x.wav", "--max-chars", "1.5"}, 2, "argument --max-chars: invalid int value: '1.5'", false},
            {{"tts", "--text-file", txt, "--out"}, 2, "--out needs a value", false},
            {{"tts", "--batch", QDir(m_dir).filePath("missing.json")}, 2, "cannot read batch: ", true},
            {{"tts", "--batch", b1, "--engine", "sapi"}, 2, "no items", false},
            {{"tts", "--batch", b2, "--engine", "sapi"}, 2, "item 1: empty text or no output path", false},
            {{"tts", "--batch", b3, "--engine", "sapi"}, 2, "item 0: empty text or no output path", false},
            {{"tts", "--batch", b4}, 2, "cannot read batch: ", true},
            {{"speak"}, 64, "unsupported: speak", false},
            {{"voices", "--all"}, 64, "unsupported: --all", false},
            {{}, 2, "no command given", false},
        };
        for (const Case& c : cases) {
            const Run r = runTool(c.args);
            const QByteArray tag = c.args.join(' ').toUtf8() + "\n" + describe(r);
            QVERIFY2(r.code == c.code, tag.constData());
            QVERIFY2(r.out.count('\n') == 1, tag.constData());                 // one JSON line, nothing else on stdout
            QVERIFY2(!r.json.value("ok").toBool(), tag.constData());
            const QString e = r.json.value("error").toString();
            QVERIFY2(c.prefix ? e.startsWith(c.error) : e == c.error, tag.constData());
            QCOMPARE(r.json.value("unsupported").toBool(), c.code == 64);
        }
        Run r = runTool({"--help"});
        QCOMPARE(r.code, 0); QVERIFY(r.out.contains("tts"));
        r = runTool({"tts", "--help"});
        QCOMPARE(r.code, 0); QVERIFY(r.out.contains("--batch"));
        r = runTool({"version"});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("tool").toString(), QString("PlayPlaySpeech"));
        QCOMPARE(r.json.value("commands").toArray(), (QJsonArray{"tts", "voices", "env", "version"}));
    }

    // ------------------------------------------------------------------------------------------ service-like context
    void serviceLikeRunSpawnsNothing() {
        if (!voiceInstalled("Microsoft Zira Desktop")) QSKIP("Microsoft Zira Desktop is not installed here");
        // PATH without any folder that holds a PowerShell; everything else (Qt DLLs, SystemRoot) kept
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QStringList path, dropped;
        for (const QString& d : env.value("PATH").split(';', Qt::SkipEmptyParts)) {
            const bool ps = QFileInfo::exists(QDir(d).filePath("powershell.exe")) || QFileInfo::exists(QDir(d).filePath("pwsh.exe"));
            (ps ? dropped : path) << d;
        }
        env.insert("PATH", path.join(';'));
        env.remove("PSModulePath");
        QJsonArray items;
        for (int i = 0; i < 2; ++i) items.append(QJsonObject{{"text", QString("Detached item %1, no console and no shell.").arg(i + 1)}, {"out", QDir(m_dir).filePath(QString("svc_%1.wav").arg(i))}});
        const QString job = QDir(m_dir).filePath("svc.json"), outLog = QDir(m_dir).filePath("svc.out"), errLog = QDir(m_dir).filePath("svc.err");
        QVERIFY(writeText(job, QJsonDocument(QJsonObject{{"items", items}}).toJson()));

        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE hOut = CreateFileW(reinterpret_cast<const wchar_t*>(outLog.utf16()), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hErr = CreateFileW(reinterpret_cast<const wchar_t*>(errLog.utf16()), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        QVERIFY(hOut != INVALID_HANDLE_VALUE && hErr != INVALID_HANDLE_VALUE && hIn != INVALID_HANDLE_VALUE);
        STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = hIn; si.hStdOutput = hOut; si.hStdError = hErr;
        QString cmd = "\"" + QDir::toNativeSeparators(kExe) + "\" tts --batch \"" + QDir::toNativeSeparators(job) + "\" --voice af_heart --engine sapi";
        std::wstring cmdW = cmd.toStdWString();
        std::wstring block;
        for (const QString& k : env.keys()) block += (k + "=" + env.value(k)).toStdWString() + L'\0';
        block += L'\0';
        HANDLE jobObj = CreateJobObjectW(nullptr, nullptr);
        QVERIFY(jobObj);
        PROCESS_INFORMATION pi = {};
        const BOOL started = CreateProcessW(nullptr, cmdW.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | DETACHED_PROCESS | CREATE_UNICODE_ENVIRONMENT,
                                            block.data(), reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(m_dir).utf16()), &si, &pi);
        CloseHandle(hOut); CloseHandle(hErr); CloseHandle(hIn);
        QVERIFY2(started, qPrintable(QString("CreateProcessW failed: %1").arg(GetLastError())));
        const bool assigned = AssignProcessToJobObject(jobObj, pi.hProcess);
        ResumeThread(pi.hThread);
        const DWORD wait = WaitForSingleObject(pi.hProcess, 60000);
        if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 99);   // only the process this test started
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct = {};
        const bool queried = QueryInformationJobObject(jobObj, JobObjectBasicAccountingInformation, &acct, sizeof(acct), nullptr);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(jobObj);
        QFile fo(outLog), fe(errLog); fo.open(QIODevice::ReadOnly); fe.open(QIODevice::ReadOnly);
        const QByteArray so = fo.readAll(), se = fe.readAll();
        const QByteArray tag = "exit " + QByteArray::number(int(code)) + "\nstdout: " + so + "\nstderr: " + se + "\nPATH dropped: " + dropped.join(';').toUtf8();
        QVERIFY2(wait == WAIT_OBJECT_0 && code == 0, tag.constData());
        QVERIFY(assigned && queried);
        QVERIFY2(acct.TotalProcesses == 1, qPrintable(QString("%1 processes ran in the job (the tool spawned something)").arg(acct.TotalProcesses)));
        const QJsonObject res = ppcli::parseResult(so);
        QCOMPARE(res.value("engine").toString(), QString("sapi"));
        QCOMPARE(res.value("voiceUsed").toString(), QString("Microsoft Zira Desktop"));
        for (const QJsonValue& it : res.value("items").toArray()) checkVoiceWav(it.toObject().value("path").toString(), it.toObject().value("durationSec").toDouble());
    }

    void noProcessSpawningInSources() {
        const QStringList banned = {"QProcess", "CreateProcess", "ShellExecute", "WinExec", "_popen", "_wpopen", "system(", "_wsystem", "spawnl", "spawnv"};
        QDirIterator it(QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("tools/speech"), {"*.cpp", "*.h"}, QDir::Files);
        int files = 0;
        while (it.hasNext()) {
            const QString f = it.next(); QFile src(f); QVERIFY(src.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(src.readAll());
            for (const QString& b : banned) QVERIFY2(!text.contains(b), qPrintable(QFileInfo(f).fileName() + " contains " + b));
            ++files;
        }
        QVERIFY(files >= 7);
    }

    // ------------------------------------------------------------------------------------------ parity with the script
    void parityWithSpotTtsPy() {
        const QString py = findPython(true);
        if (py.isEmpty()) QSKIP("no Python with numpy here (set PLAYPLAY_TEST_PYTHON)");
        if (QStandardPaths::findExecutable("powershell").isEmpty() && QStandardPaths::findExecutable("pwsh").isEmpty()) QSKIP("spot_tts.py's SAPI engine needs PowerShell");
        const QString pdir = QDir(m_dir).filePath("parity");
        QVERIFY(QDir().mkpath(pdir));
        const QString txt = QDir(pdir).filePath("t.txt");
        QVERIFY(writeText(txt, "Good evening, Martinsville. Here is tonight's weather: clear skies and a low of 41 degrees."));
        struct Pair { QString voice; bool sameWave; };
        // desktop Zira renders identically through System.Speech and raw SAPI; David Desktop runs on the OneCore engine,
        // whose output System.Speech converts differently (same words, timing and level, different fine waveform)
        for (const Pair& p : {Pair{"af_heart", true}, Pair{"am_michael", false}}) {
            const QString pw = QDir(pdir).filePath("py_" + p.voice + ".wav"), nw = QDir(pdir).filePath("native_" + p.voice + ".wav");
            QProcess q; q.start(py, {"-I", QDir(kScripts).filePath("spot_tts.py"), "--text-file", txt, "--out", pw, "--voice", p.voice, "--engine", "sapi"});
            QVERIFY(q.waitForFinished(120000));
            QCOMPARE(q.exitCode(), 0);
            const QJsonObject pj = ppcli::parseResult(q.readAllStandardOutput());
            const Run r = runTool({"tts", "--text-file", txt, "--out", nw, "--voice", p.voice, "--engine", "sapi"});
            QVERIFY2(r.code == 0, describe(r).constData());
            QCOMPARE(sortedKeys(r.json), sortedKeys(pj));
            for (const char* k : {"ok", "engine", "voiceUsed", "sampleRate", "words", "tried"}) QCOMPARE(r.json.value(k), pj.value(k));
            const double dn = r.json.value("durationSec").toDouble(), dp = pj.value("durationSec").toDouble();
            QVERIFY2(std::fabs(dn - dp) <= 0.02, qPrintable(QString("%1: native %2 s, python %3 s").arg(p.voice).arg(dn).arg(dp)));
            wavio::Wav a, b; QVERIFY(wavio::readFile(nw, &a)); QVERIFY(wavio::readFile(pw, &b));
            QVERIFY2(std::fabs(wavio::voicedRmsDb(a) - wavio::voicedRmsDb(b)) <= 0.3, qPrintable(QString("level native %1, python %2").arg(wavio::voicedRmsDb(a)).arg(wavio::voicedRmsDb(b))));
            const double env = envelopeCorrelation(a, b), wave = correlation(a, b);
            qInfo("%s: native %.3f s / python %.3f s, envelope corr %.4f, waveform corr %.4f", qPrintable(p.voice), dn, dp, env, wave);
            QVERIFY2(env >= 0.9, qPrintable(QString::number(env)));
            if (p.sameWave) QVERIFY2(wave >= 0.99, qPrintable(QString::number(wave)));
        }
        // batch: same item durations (both trimmed) and the script's result keys
        QJsonArray ni, pi2;
        const QStringList texts = {"First story: the city council met tonight.", "Second story: schools open late tomorrow.", "And finally, sports."};
        for (int i = 0; i < texts.size(); ++i) {
            ni.append(QJsonObject{{"text", texts[i]}, {"out", QDir(pdir).filePath(QString("n_%1.wav").arg(i))}});
            pi2.append(QJsonObject{{"text", texts[i]}, {"out", QDir(pdir).filePath(QString("p_%1.wav").arg(i))}});
        }
        const QString nj = QDir(pdir).filePath("n.json"), pj = QDir(pdir).filePath("p.json");
        QVERIFY(writeText(nj, QJsonDocument(QJsonObject{{"items", ni}}).toJson()));
        QVERIFY(writeText(pj, QJsonDocument(QJsonObject{{"items", pi2}}).toJson()));
        QProcess q; q.start(py, {"-I", QDir(kScripts).filePath("spot_tts.py"), "--batch", pj, "--voice", "af_heart", "--engine", "sapi"});
        QVERIFY(q.waitForFinished(120000));
        QCOMPARE(q.exitCode(), 0);
        const QJsonObject pr = ppcli::parseResult(q.readAllStandardOutput());
        const Run r = runTool({"tts", "--batch", nj, "--voice", "af_heart", "--engine", "sapi"});
        QVERIFY2(r.code == 0, describe(r).constData());
        QCOMPARE(sortedKeys(r.json), sortedKeys(pr));
        for (const char* k : {"ok", "engine", "voiceUsed", "tried"}) QCOMPARE(r.json.value(k), pr.value(k));
        for (int i = 0; i < 3; ++i) {
            const QJsonObject a = r.json.value("items").toArray()[i].toObject(), b = pr.value("items").toArray()[i].toObject();
            QCOMPARE(sortedKeys(a), sortedKeys(b));
            QCOMPARE(a.value("words"), b.value("words"));
            QVERIFY2(std::fabs(a.value("durationSec").toDouble() - b.value("durationSec").toDouble()) <= 0.02,
                     qPrintable(QString("item %1: native %2 s, python %3 s").arg(i).arg(a.value("durationSec").toDouble()).arg(b.value("durationSec").toDouble())));
        }
    }
};

QTEST_GUILESS_MAIN(TestInhouseSpeech)
#include "test_inhouse_speech.moc"
