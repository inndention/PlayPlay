// test_inhouse_music (DEP-07): PlayPlaySpotRender `music` against scripts/spot_music.py.
//  - pyrandom equals CPython 3.11's random.Random (values below were printed once by CPython 3.11.9: seeding with ints of
//    1..101 bits incl. negative ones, random(), getrandbits(1..64), uniform(), choice() incl. a 34-bit range, a 5000-word
//    run across the 624-word regenerations) and round() (ties to even, round(x, 3) correctly rounded).
//  - the composer writes MIDI byte-identical to the goldens in tests/inhouse/data/music (made with
//    `python -I scripts/spot_music.py`, see make_goldens.py there: 12 moods x 3 seeds x energies 2/5/9 + edge cases),
//    and, when Python + numpy are here, to the script for 400 random argument sets.
//  - the exe: the stdout JSON has the script's keys and values; the WAV is 48 kHz stereo 16-bit with exactly
//    ceil(seconds * 48000) frames, -12 dBFS RMS +-1, no sample over 0.95; the same seed gives the same bytes; errors and
//    unknown options follow the in-house contract (exit 2 / 64).
//  - with Python: our synth against the script's WAV (same length, waveform correlation for a drum-less bed, level).
#include "musicsynth.h"
#include "ppcli.h"
#include "pyrandom.h"
#include "wavio.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

// ---------------------------------------------------------------------------------------------- CPython 3.11.9 values
struct RandCase { const char* seed; quint32 words[3]; double randoms[3]; quint64 bits[10]; double uniforms[2]; quint64 choices[9]; };
const int kBitsK[10] = {1, 2, 3, 7, 31, 32, 33, 48, 63, 64};
const quint64 kChoiceN[9] = {1, 2, 3, 5, 6, 7, 100, 1000, (1ull << 33) + 5};
// r = random.Random(int(seed)) afresh for each list: 3 x getrandbits(32); 3 x random(); getrandbits(k) for kBitsK;
// 2 x uniform(0.85, 1.0); choice(range(n)) for kChoiceN.
const RandCase kRandCases[] = {
    {"0", {3626764237u, 1654615998u, 3255389356u}, {0x1.b0580f98a7dbep-1, 0x1.84129978f9c1ap-1, 0x1.aeaa51052e978p-2}, {1ull, 1ull, 6ull, 113ull, 903170602ull, 173879092ull, 5407006266ull, 136775429449314ull, 8469216845598671736ull, 15308084094301570617ull}, {0x1.f40d358a192dcp-1, 0x1.ed69309ef2437p-1}, {0ull, 1ull, 0ull, 2ull, 4ull, 3ull, 51ull, 940ull, 6341935620ull}},
    {"1", {577090037u, 2444712010u, 3639700191u}, {0x1.132d8f91b7584p-3, 0x1.b1e2d5b3584f8p-1, 0x1.870d778409f13p-1}, {0ull, 2ull, 6ull, 102ull, 1640193506ull, 271041745ull, 1095513148ull, 214192146929019ull, 4355693531291048099ull, 7002664860023442459ull}, {0x1.bd84e89576e02p-1, 0x1.f4486cdae6d8cp-1}, {0ull, 0ull, 1ull, 0ull, 3ull, 6ull, 57ull, 483ull, 7093537819ull}},
    {"7", {1390851128u, 4071050724u, 647892279u}, {0x1.4b9ad0f953a6ep-2, 0x1.34f0696513270p-3, 0x1.4d474883171ffp-1}, {0ull, 3ull, 1ull, 50ull, 1397871144ull, 207388624ull, 4606078771ull, 26493659877419ull, 5375270654777870840ull, 16781078052021535861ull}, {0x1.cc1202df7fdfbp-1, 0x1.bec903f3ca517p-1}, {0ull, 0ull, 1ull, 0ull, 0ull, 6ull, 68ull, 96ull, 2179419893ull}},
    {"12345", {1789368711u, 3146859322u, 43676229u}, {0x1.aa9e665dc8a18p-2, 0x1.4d392d1f6f840p-7, 0x1.a68177b361de3p-1}, {0ull, 2ull, 0ull, 104ull, 1772117478ull, 3448207591ull, 5577615682ull, 262273760246375ull, 2492568693218986580ull, 8046993897209325893ull}, {0x1.d33247ad6f0c2p-1, 0x1.b3fb224e460fbp-1}, {0ull, 0ull, 1ull, 2ull, 1ull, 2ull, 72ull, 446ull, 4989411211ull}},
    {"-12345", {1789368711u, 3146859322u, 43676229u}, {0x1.aa9e665dc8a18p-2, 0x1.4d392d1f6f840p-7, 0x1.a68177b361de3p-1}, {0ull, 2ull, 0ull, 104ull, 1772117478ull, 3448207591ull, 5577615682ull, 262273760246375ull, 2492568693218986580ull, 8046993897209325893ull}, {0x1.d33247ad6f0c2p-1, 0x1.b3fb224e460fbp-1}, {0ull, 0ull, 1ull, 2ull, 1ull, 2ull, 72ull, 446ull, 4989411211ull}},
    {"1234567890", {3954301776u, 2504761754u, 2727734613u}, {0x1.d763b6a552e86p-1, 0x1.452bda8a48445p-1, 0x1.c62a681c12176p-2}, {1ull, 2ull, 5ull, 20ull, 952454400ull, 941895369ull, 7760509556ull, 212278297689716ull, 2825459752566365625ull, 16290641578492502945ull}, {0x1.f9e88e98cc6fap-1, 0x1.e3f9c72e57a3ep-1}, {0ull, 1ull, 0ull, 4ull, 2ull, 2ull, 10ull, 451ull, 1854258025ull}},
    {"2147483647", {1364760256u, 4023463762u, 3510513048u}, {0x1.45625b77e897ap-2, 0x1.a27c5b07b407ap-1, 0x1.921e874a55860p-3}, {0ull, 3ull, 6ull, 15ull, 421652596ull, 1247118883ull, 8091207418ull, 2105961045717ull, 4965222608359641614ull, 2836238442801599920ull}, {0x1.cb9a93a8fe3e9p-1, 0x1.f1f90da78e346p-1}, {0ull, 0ull, 0ull, 2ull, 5ull, 2ull, 0ull, 808ull, 2312111951ull}},
    {"4294967296", {485306839u, 1508871100u, 1794561286u}, {0x1.ced31cb3df170p-4, 0x1.abdb5477a4f26p-2, 0x1.1139c02ebd380p-6}, {0ull, 1ull, 3ull, 119ull, 35812237ull, 98019085ull, 6922290953ull, 214580026335744ull, 7727741787315235553ull, 18349438830298068874ull}, {0x1.bbe0c2235f620p-1, 0x1.d34a0cbc2c5f0p-1}, {0ull, 1ull, 1ull, 0ull, 0ull, 4ull, 97ull, 339ull, 8567278949ull}},
    {"4294967297", {991850117u, 2151679444u, 2200792185u}, {0x1.d8f374804005cp-3, 0x1.065ad8f5e2a27p-1, 0x1.a8885ce825b6ap-2}, {0ull, 2ull, 4ull, 107ull, 890309527ull, 3494604116ull, 393686508ull, 70023451151390ull, 2059724949259382118ull, 15476826839305122659ull}, {0x1.c4ef87919c003p-1, 0x1.da8da08b48652p-1}, {0ull, 1ull, 0ull, 2ull, 4ull, 1ull, 87ull, 228ull, 5345941616ull}},
    {"1100499282097", {2758663008u, 451945725u, 2339970321u}, {0x1.48dbb6c6bc093p-1, 0x1.16f23a1e6b8c4p-1, 0x1.c42db14ca941dp-1}, {1ull, 0ull, 4ull, 60ull, 1896574046ull, 849676112ull, 8199548170ull, 42405018221715ull, 4390543691269261661ull, 5572330267221236598ull}, {0x1.e4875b6a9c349p-1, 0x1.dd0abbeaf6884p-1}, {0ull, 1ull, 0ull, 1ull, 1ull, 5ull, 60ull, 787ull, 8194358273ull}},
    {"18446744073709551615", {93740670u, 1068495656u, 1452108352u}, {0x1.659799fd7f980p-6, 0x1.5a35a94f333d8p-2, 0x1.b21c0274c09a4p-3}, {0ull, 0ull, 2ull, 79ull, 455196712ull, 1958779474ull, 2641946051ull, 253557343846644ull, 7083482816045695211ull, 12830555127327471795ull}, {0x1.b4e04f8596992p-1, 0x1.cd2a6cb2bd716p-1}, {0ull, 0ull, 1ull, 4ull, 1ull, 3ull, 78ull, 97ull, 3189129643ull}},
    {"18446744073709551616", {4198958755u, 3158798261u, 1593907883u}, {0x1.f48e0d6f11dbep-1, 0x1.7c046a8bc68eep-2, 0x1.ffa695fe066a8p-2}, {1ull, 2ull, 2ull, 11ull, 1073009342ull, 4228699401ull, 2682463428ull, 145013653965373ull, 7483523207054416996ull, 6859588370208602058ull}, {0x1.fe488203dc476p-1, 0x1.cfb387fdaee45p-1}, {0ull, 0ull, 1ull, 4ull, 1ull, 4ull, 65ull, 99ull, 7779754346ull}},
    {"1267650600228229401496703205379", {1532279558u, 1283910471u, 4172443581u}, {0x1.6d52ec264375ap-2, 0x1.f164df7deb449p-1, 0x1.c69168da45063p-1}, {0ull, 1ull, 7ull, 123ull, 1906596405ull, 1762924757ull, 6317004064ull, 62933076156370ull, 8589573443940742337ull, 14214302979369258223ull}, {0x1.ce996b4fab75ap-1, 0x1.fdcf2186167d8p-1}, {0ull, 1ull, 1ull, 3ull, 4ull, 5ull, 28ull, 680ull, 3309525311ull}},
    {"1000000000000000000000000000000", {4012147334u, 2100903950u, 2256175901u}, {0x1.de49051f4e500p-1, 0x1.0cf50607ee08fp-1, 0x1.658c0982c641ap-2}, {1ull, 1ull, 4ull, 15ull, 749830463ull, 93094754ull, 5712077971ull, 243392124565833ull, 4197200927149042122ull, 2489821699343110023ull}, {0x1.faf15a5e4bbf3p-1, 0x1.db8b274dfd4e2p-1}, {0ull, 0ull, 1ull, 0ull, 2ull, 1ull, 52ull, 465ull, 3613904775ull}},
};
// seed 12345, 5000 x getrandbits(32): xor of v * (i + 1) mod 2^32, sum mod 2^64, then random()
constexpr quint32 kLongXor = 1357528634u;
constexpr quint64 kLongSum = 10711592836339ull;
constexpr double kLongNextRandom = 0x1.65f92128eb890p-1;

struct RoundCase { double x; qint64 r; };
const RoundCase kRound0[] = {{0x1.0000000000000p-1, 0}, {0x1.8000000000000p+0, 2}, {0x1.4000000000000p+1, 2}, {-0x1.0000000000000p-1, 0}, {-0x1.8000000000000p+0, -2}, {-0x1.4000000000000p+1, -2}, {0x1.fffffffffffffp-2, 0}, {0x1.5666666666666p+1, 3}, {0x1.ae00000000000p+6, 108}, {0x1.b200000000000p+6, 108}, {0x1.0000000000001p+52, 4503599627370497}, {0x1.89fffffffffffp+6, 98}, {0x1.3600000001b7dp+6, 78}, {-0x1.3600000000000p+6, -78}};
const double kRound3In[] = {0x1.0000000000000p-4, 0x1.8000000000000p-3, 0x1.4000000000000p-2, 0x1.5666666666666p+1, 0x1.0020c49ba5e35p+0, 0x1.0624dd2f1a9fcp-11, 0x1.89374bc6a7efap-10, 0x1.47ae147ae147bp-9, 0x1.0000000000000p-3, 0x1.8000000000000p-2, 0x1.3c083126e978dp+0, 0x1.ffbe76c8b4396p-1, 0x1.8000000000000p+1, 0x1.999999999999ap-4, 0x1.999999999999ap-3, 0x1.3333333333333p-2, 0x1.638d4fdf3b646p+2, 0x1.2000000000000p-1};
const double kRound3Out[] = {0x1.fbe76c8b43958p-5, 0x1.810624dd2f1aap-3, 0x1.3f7ced916872bp-2, 0x1.5666666666666p+1, 0x1.0000000000000p+0, 0x1.0624dd2f1a9fcp-10, 0x1.0624dd2f1a9fcp-9, 0x1.89374bc6a7efap-9, 0x1.0000000000000p-3, 0x1.8000000000000p-2, 0x1.3be76c8b43958p+0, 0x1.0000000000000p+0, 0x1.8000000000000p+1, 0x1.999999999999ap-4, 0x1.999999999999ap-3, 0x1.3333333333333p-2, 0x1.6395810624dd3p+2, 0x1.1fbe76c8b4396p-1};
// round(ln * (60.0 / bpm / 4.0), 3) for bpm 40..160, ln 1..16 (1936 values): sum in that order, xor of the bit patterns
constexpr double kStepRoundSum = 0x1.65808b4395814p+11;
constexpr quint64 kStepRoundXor = 0x0038c500f9e051a6ull;

// ---------------------------------------------------------------------------------------------- helpers
const QString kDataDir = QStringLiteral(PP_INHOUSE_DATA_DIR "/music");
const QString kExe = QStringLiteral(PP_SPOTRENDER_EXE);

PyRandom fromDecimal(const char* s) {
    bool neg = false; std::vector<quint32> w;
    const bool ok = PyRandom::parseInt(QString::fromLatin1(s), &neg, &w);
    Q_ASSERT(ok); Q_UNUSED(ok);
    return PyRandom(w);
}

struct Run { int code = -1; QByteArray out, err; QJsonObject json; qint64 ms = 0; };
Run runTool(const QStringList& args, int timeoutMs = 120000) {
    Run r; QProcess p; QElapsedTimer t; t.start();
    p.start(kExe, args);
    if (!p.waitForStarted(20000)) { r.err = "could not start " + kExe.toUtf8(); return r; }
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(5000); r.err = "timeout"; return r; }
    r.ms = t.elapsed();
    r.code = p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
    r.out = p.readAllStandardOutput(); r.err = p.readAllStandardError();
    r.json = ppcli::parseResult(r.out);
    return r;
}

QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

QJsonObject loadManifest() { return QJsonDocument::fromJson(readAll(kDataDir + "/manifest.json")).object(); }

QStringList argsFor(const QJsonObject& a, const QString& out, const QString& mid) {
    return {"--out", out, "--mid", mid, "--seconds", a.value("seconds").toString(), "--mood", a.value("mood").toString(),
            "--energy", a.value("energy").toString(), "--seed", a.value("seed").toString()};
}

int firstDiff(const QByteArray& a, const QByteArray& b) {
    const int n = int(qMin(a.size(), b.size()));
    for (int i = 0; i < n; ++i) if (a[i] != b[i]) return i;
    return a.size() == b.size() ? -1 : n;
}

QStringList sortedKeys(const QJsonObject& o) { QStringList k = o.keys(); k.sort(); return k; }

double rmsDb(const wavio::Wav& w) {
    double acc = 0.0;
    for (qint16 s : w.samples) { const double v = s / 32767.0; acc += v * v; }
    return w.samples.empty() ? -INFINITY : 10.0 * std::log10(acc / double(w.samples.size()));
}
int peakAbs(const wavio::Wav& w) { int p = 0; for (qint16 s : w.samples) p = qMax(p, std::abs(int(s))); return p; }

// Pearson correlation of one channel of two interleaved stereo files
double correlation(const wavio::Wav& a, const wavio::Wav& b, int ch) {
    const qint64 n = qMin(a.frames(), b.frames());
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (qint64 i = 0; i < n; ++i) {
        const double x = a.samples[size_t(i * 2 + ch)], y = b.samples[size_t(i * 2 + ch)];
        sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }
    const double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    return va > 0 && vb > 0 ? cov / std::sqrt(va * vb) : 0.0;
}
// correlation of 50 ms RMS envelopes (both channels): the shape of the bed when the drum noise differs sample by sample
double envelopeCorrelation(const wavio::Wav& a, const wavio::Wav& b) {
    const qint64 block = 2400, n = qMin(a.frames(), b.frames()) / block;
    std::vector<double> ea(static_cast<size_t>(n)), eb(static_cast<size_t>(n));
    for (qint64 k = 0; k < n; ++k) {
        double x = 0, y = 0;
        for (qint64 i = k * block * 2; i < (k + 1) * block * 2; ++i) { x += double(a.samples[size_t(i)]) * a.samples[size_t(i)]; y += double(b.samples[size_t(i)]) * b.samples[size_t(i)]; }
        ea[size_t(k)] = std::sqrt(x); eb[size_t(k)] = std::sqrt(y);
    }
    double ma = 0, mb = 0; for (qint64 k = 0; k < n; ++k) { ma += ea[size_t(k)]; mb += eb[size_t(k)]; }
    ma /= n; mb /= n;
    double c = 0, va = 0, vb = 0;
    for (qint64 k = 0; k < n; ++k) { const double x = ea[size_t(k)] - ma, y = eb[size_t(k)] - mb; c += x * y; va += x * x; vb += y * y; }
    return va > 0 && vb > 0 ? c / std::sqrt(va * vb) : 0.0;
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
QString scriptsDir() { return QDir(QStringLiteral(PP_SOURCE_DIR)).filePath("scripts"); }

}   // namespace

class TestInhouseMusic : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QVERIFY2(QFileInfo::exists(kExe), qPrintable("missing " + kExe));
        QVERIFY2(QFileInfo::exists(kDataDir + "/manifest.json"), "missing goldens (tests/inhouse/data/music)");
    }

    // ------------------------------------------------------------------------------------------ pyrandom
    void pyrandomMatchesCPython() {
        for (const RandCase& c : kRandCases) {
            const QByteArray tag = QByteArray("seed ") + c.seed;
            { PyRandom r = fromDecimal(c.seed); for (quint32 w : c.words) QCOMPARE(r.getrandbits(32), quint64(w)); }
            { PyRandom r = fromDecimal(c.seed); for (double v : c.randoms) QVERIFY2(r.random() == v, tag.constData()); }
            { PyRandom r = fromDecimal(c.seed); for (int i = 0; i < 10; ++i) QVERIFY2(r.getrandbits(kBitsK[i]) == c.bits[i], (tag + " k=" + QByteArray::number(kBitsK[i])).constData()); }
            { PyRandom r = fromDecimal(c.seed); for (double v : c.uniforms) QVERIFY2(r.uniform(0.85, 1.0) == v, tag.constData()); }
            { PyRandom r = fromDecimal(c.seed); for (int i = 0; i < 9; ++i) QVERIFY2(r.randbelow(kChoiceN[i]) == c.choices[i], (tag + " n=" + QByteArray::number(kChoiceN[i])).constData()); }
            // the qint64 constructor (what the composer's int path uses) agrees for every seed that fits
            bool neg = false; std::vector<quint32> w;
            QVERIFY(PyRandom::parseInt(QString::fromLatin1(c.seed), &neg, &w));
            if (w.size() <= 2 && !(w.size() == 2 && (w[1] & 0x80000000u))) {
                const qint64 mag = qint64(w[0]) | (w.size() == 2 ? qint64(w[1]) << 32 : 0);
                PyRandom a(neg ? -mag : mag), b(w);
                QVERIFY(a.state() == b.state());
                QVERIFY(a.random() == b.random());
            }
        }
        // choice() from a one-element list still consumes words (randbelow(1) = getrandbits(1) until 0), as in Python
        PyRandom a(7), b(7);
        std::vector<int> one{42};
        QCOMPARE(a.choice(one), 42);
        QCOMPARE(b.getrandbits(1), quint64(0));          // seed 7: the first bit is 0 (kRandCases "7": bits[0] == 0)
        QVERIFY(a.random() == b.random());
        // a seed with high zero words is the same seed (keyused counts significant words only)
        QVERIFY(PyRandom(std::vector<quint32>{5, 0, 0}).state() == PyRandom(5).state());
        QVERIFY(PyRandom(std::vector<quint32>{}).state() == PyRandom(0).state());
        // INT64_MIN: abs() without overflow = 2^63
        QVERIFY(PyRandom(std::numeric_limits<qint64>::min()).state() == PyRandom(std::vector<quint32>{0u, 0x80000000u}).state());
    }

    void pyrandomLongRun() {
        PyRandom r(12345);
        quint32 x = 0; quint64 s = 0;
        for (quint32 i = 0; i < 5000; ++i) { const quint32 v = r.genrand(); x ^= v * (i + 1); s += v; }
        QCOMPARE(x, kLongXor);
        QCOMPARE(s, kLongSum);
        QVERIFY(r.random() == kLongNextRandom);
    }

    void pyRoundMatchesPython() {
        for (const RoundCase& c : kRound0) QVERIFY2(pyRound(c.x) == double(c.r), qPrintable(QString::number(c.x, 'g', 17)));
        QCOMPARE(std::size(kRound3In), std::size(kRound3Out));
        for (size_t i = 0; i < std::size(kRound3In); ++i)
            QVERIFY2(pyRound(kRound3In[i], 3) == kRound3Out[i], qPrintable(QString("round(%1, 3) = %2, Python %3").arg(kRound3In[i], 0, 'g', 17).arg(pyRound(kRound3In[i], 3), 0, 'g', 17).arg(kRound3Out[i], 0, 'g', 17)));
        double sum = 0.0; quint64 x = 0;
        for (int bpm = 40; bpm <= 160; ++bpm) {
            const double step = 60.0 / bpm / 4.0;
            for (int ln = 1; ln <= 16; ++ln) { const double v = pyRound(ln * step, 3); sum += v; quint64 b; std::memcpy(&b, &v, 8); x ^= b; }
        }
        QVERIFY(sum == kStepRoundSum);
        QCOMPARE(x, kStepRoundXor);
        QVERIFY(std::signbit(pyRound(-0.0004, 3)));      // Python keeps the sign: round(-0.0004, 3) == -0.0
        QVERIFY(std::isnan(pyRound(NAN, 3)));
    }

    void parseIntLikePython() {
        auto val = [](const char* s, bool* neg = nullptr) -> int {
            bool n = false; std::vector<quint32> w;
            if (!PyRandom::parseInt(QString::fromUtf8(s), &n, &w)) return -999;
            if (neg) *neg = n;
            return w.size() == 1 ? int(w[0]) : -998;
        };
        QCOMPARE(val("42"), 42);
        QCOMPARE(val("  42 \t"), 42);
        QCOMPARE(val("+7"), 7);
        bool neg = false;
        QCOMPARE(val("-15", &neg), 15); QVERIFY(neg);
        QCOMPARE(val("-0", &neg), 0); QVERIFY(!neg);
        QCOMPARE(val("1_000"), 1000);
        QCOMPARE(val("007"), 7);
        QCOMPARE(val("\xef\xbc\x91\xef\xbc\x92"), 12);          // full-width digits: int('１２') == 12
        for (const char* badText : {"", " ", "-", "+", "_1", "1_", "1__0", "1.0", "0x10", "1e3", "- 1", "12a", "\xc2\xb2"})
            QVERIFY2(val(badText) == -999, badText);
        std::vector<quint32> w; bool n = false;
        QVERIFY(PyRandom::parseInt("1267650600228229401496703205379", &n, &w));       // 2**100 + 3
        QVERIFY(w == (std::vector<quint32>{3u, 0u, 0u, 16u}));
        QVERIFY(PyRandom::parseInt(QString(4300, '9'), &n, &w));
        QString err;
        QVERIFY(!PyRandom::parseInt(QString(4301, '9'), &n, &w, &err));
        QVERIFY(err.contains("4300"));
    }

    void moodsAndClamps() {
        QCOMPARE(int(music::moodNames().size()), 12);
        QCOMPARE(music::pickMood(" Driving "), QString("news"));
        QCOMPARE(music::pickMood("SAD"), QString("solemn"));
        QCOMPARE(music::pickMood("fun"), QString("quirky"));
        QCOMPARE(music::pickMood("Night"), QString("night"));
        QCOMPARE(music::pickMood("polka"), QString("warm"));
        QCOMPARE(music::pickMood(""), QString("warm"));
        QCOMPARE(music::clampSeconds(45.5), 45.5);
        QCOMPARE(music::clampSeconds(1.0), 3.0);
        QCOMPARE(music::clampSeconds(5000.0), 900.0);
        QCOMPARE(music::clampSeconds(NAN), 900.0);             // Python: max(3.0, min(900.0, nan)) == 900.0
        QCOMPARE(music::clampSeconds(INFINITY), 900.0);
        QCOMPARE(music::clampSeconds(-INFINITY), 3.0);
        // the CLI's value syntax is argparse's: Python int() / float()
        music::Options o; QJsonObject err; int code = 0;
        QVERIFY(music::parseOptions({"--out", "a.wav", "--seconds", " 1_2.5 ", "--energy", "-99999999999999999999", "--seed", "-5"}, &o, &err, &code));
        QCOMPARE(o.seconds, 12.5);
        QCOMPARE(o.energy, 1);
        QVERIFY(o.seed == (std::vector<quint32>{5u}));
        o = music::Options();
        QVERIFY(music::parseOptions({"--out", "a.wav", "--seconds", "1e1", "--energy", "99999999999999999999"}, &o, &err, &code));
        QCOMPARE(o.seconds, 10.0);
        QCOMPARE(o.energy, 10);
        QVERIFY(o.seed == (std::vector<quint32>{1u}));          // argparse default --seed 1
        QCOMPARE(o.mood, QString("warm"));
        QCOMPARE(o.mid, QString());                             // default --mid "": no MIDI file
        QVERIFY(music::parseOptions({"--out", "a.wav", "--seconds", "nan"}, &o, &err, &code));
        QVERIFY(std::isnan(o.seconds));
        QVERIFY(!music::parseOptions({"--out", "a.wav", "--seconds", "1__2"}, &o, &err, &code));
        QCOMPARE(code, 2);
        QVERIFY(!music::parseOptions({"--out", "a.wav", "--seconds", "5", "--energy", "5.0"}, &o, &err, &code));
        QCOMPARE(code, 2);
        QVERIFY(!music::parseOptions({"--out", "a.wav", "--seconds", "5", "--tempo", "90"}, &o, &err, &code));
        QCOMPARE(code, 64);
        QVERIFY(err.value("unsupported").toBool());
    }

    // ------------------------------------------------------------------------------------------ composer + MIDI
    void midiMatchesGoldens() {
        const QJsonObject man = loadManifest();
        const QJsonArray cases = man.value("cases").toArray();
        QVERIFY2(cases.size() >= 108, "expected 12 moods x 3 seeds x 3 energies + edge cases");
        int checked = 0;
        QSet<QString> moods;
        for (const QJsonValue& v : cases) {
            const QJsonObject c = v.toObject(), py = c.value("stdout").toObject();
            const QString file = c.value("file").toString();
            const QByteArray golden = readAll(kDataDir + "/" + file);
            QVERIFY2(!golden.isEmpty(), qPrintable("missing golden " + file));
            music::Options o; QJsonObject err; int code = 0;
            QVERIFY2(music::parseOptions(argsFor(c.value("args").toObject(), "x.wav", "x.mid"), &o, &err, &code), qPrintable(file));
            const double secs = music::clampSeconds(o.seconds);
            const music::Piece p = music::compose(secs, o.mood, o.energy, o.seed);
            const QByteArray mid = music::midiFile(p);
            const int d = firstDiff(mid, golden);
            QVERIFY2(d < 0, qPrintable(QString("%1: MIDI differs from the script's at byte %2 (ours %3 bytes, golden %4); bpm %5 vs %6, key %7 vs %8")
                                       .arg(file).arg(d).arg(mid.size()).arg(golden.size()).arg(p.bpm).arg(py.value("bpm").toInt())
                                       .arg(p.key, py.value("key").toString())));
            QCOMPARE(p.bpm, py.value("bpm").toInt());
            QCOMPARE(p.key, py.value("key").toString());
            QCOMPARE(p.mood, py.value("mood").toString());
            QCOMPARE(secs, py.value("seconds").toDouble());
            QCOMPARE(QJsonArray::fromStringList(p.trackNames()), py.value("tracks").toArray());
            QCOMPARE(p.noteCount(), py.value("notes").toInt());
            moods.insert(p.mood);
            ++checked;
        }
        QCOMPARE(int(moods.size()), 12);
        qInfo("%d MIDI goldens byte-identical (made with CPython %s)", checked, qPrintable(man.value("python").toString()));
    }

    // 400 random argument sets through the script's own composer (one Python process) and ours
    void midiMatchesLivePython() {
        const QString py = findPythonWithNumpy();
        if (py.isEmpty()) QSKIP("no Python with numpy here (set PLAYPLAY_TEST_PYTHON)");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QRandomGenerator g(20261026);
        const QStringList moodPool = music::moodNames() + QStringList{"driving", "Tense", " happy", "GENTLE ", "dramatic", "light", "friendly", "money", "fun", "polka", "", "News"};
        QJsonArray cases;
        for (int i = 0; i < 400; ++i) {
            const int dec = int(g.bounded(4));
            const double sec = 1.0 + g.bounded(130.0);
            QString seed;
            switch (g.bounded(5)) {
            case 0: seed = QString::number(g.bounded(1000)); break;
            case 1: seed = QString::number(g.bounded(0x7fffffff)); break;                       // what the Supervisor passes
            case 2: seed = QString::number(qint64(g.generate64() >> 1)); break;
            case 3: seed = "-" + QString::number(g.generate64()); break;
            default: seed = QString::number(g.generate64()) + QString::number(g.bounded(1000000000)).rightJustified(9, '0'); break;   // > 64 bits
            }
            cases.append(QJsonObject{{"seconds", QString::number(sec, 'f', dec)}, {"mood", moodPool[int(g.bounded(int(moodPool.size())))]},
                                     {"energy", QString::number(int(g.bounded(18)) - 3)}, {"seed", seed},
                                     {"mid", QDir(dir.path()).filePath(QString("c%1.mid").arg(i))}});
        }
        const QString casesPath = dir.filePath("cases.json"), outPath = dir.filePath("out.json");
        { QFile f(casesPath); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(QJsonDocument(cases).toJson()); }
        // exactly main()'s path: secs = max(3.0, min(900.0, float(seconds))); Piece(secs, mood, int(energy), int(seed)); write_midi
        const QString code = QStringLiteral(
            "import sys, json\n"
            "sys.path.insert(0, sys.argv[1])\n"
            "import spot_music as m\n"
            "cases = json.load(open(sys.argv[2], encoding='utf-8'))\n"
            "out = []\n"
            "for c in cases:\n"
            "    secs = max(3.0, min(900.0, float(c['seconds'])))\n"
            "    p = m.Piece(secs, c['mood'], int(c['energy']), int(c['seed']))\n"
            "    m.write_midi(p, c['mid'])\n"
            "    out.append({'bpm': p.bpm, 'key': p.key, 'mood': p.mood_name, 'notes': sum(len(v) for v in p.tracks.values())})\n"
            "json.dump(out, open(sys.argv[3], 'w'))\n");
        QProcess q; q.start(py, {"-I", "-c", code, scriptsDir(), casesPath, outPath});
        QVERIFY(q.waitForFinished(120000));
        QVERIFY2(q.exitCode() == 0, q.readAllStandardError().constData());
        const QJsonArray pyOut = QJsonDocument::fromJson(readAll(outPath)).array();
        QCOMPARE(pyOut.size(), cases.size());
        for (int i = 0; i < cases.size(); ++i) {
            const QJsonObject c = cases[i].toObject(), r = pyOut[i].toObject();
            music::Options o; QJsonObject err; int ec = 0;
            QVERIFY(music::parseOptions({"--out", "x.wav", "--seconds", c.value("seconds").toString(), "--mood", c.value("mood").toString(),
                                         "--energy", c.value("energy").toString(), "--seed", c.value("seed").toString()}, &o, &err, &ec));
            const music::Piece p = music::compose(music::clampSeconds(o.seconds), o.mood, o.energy, o.seed);
            const QByteArray golden = readAll(c.value("mid").toString());
            const int d = firstDiff(music::midiFile(p), golden);
            QVERIFY2(d < 0, qPrintable(QString("case %1 (%2): differs at byte %3").arg(i).arg(QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Compact))).arg(d)));
            QCOMPARE(p.bpm, r.value("bpm").toInt());
            QCOMPARE(p.key, r.value("key").toString());
            QCOMPARE(p.mood, r.value("mood").toString());
            QCOMPARE(p.noteCount(), r.value("notes").toInt());
        }
        qInfo("400 random argument sets: MIDI byte-identical to scripts/spot_music.py");
    }

    void midiStructure() {
        // a parse of our own file: format 1, 480 PPQ, conductor + pad/bass/arp/lead/drums, drums on channel 10
        const music::Piece p = music::compose(20.0, "sports", 9, 4242);
        const QByteArray f = music::midiFile(p);
        QVERIFY(f.startsWith("MThd"));
        QCOMPARE(int(quint8(f[9])), 1);
        QCOMPARE((int(quint8(f[10])) << 8) | quint8(f[11]), 6);                // conductor + 5 tracks
        QCOMPARE((int(quint8(f[12])) << 8) | quint8(f[13]), 480);
        QCOMPARE(p.trackNames(), (QStringList{"pad", "bass", "arp", "lead", "drums"}));
        QVERIFY(f.contains(QByteArray("\x00\xFF\x03\x05" "drums", 9)));
        QVERIFY(f.contains(QByteArray("\x99\x24", 2)));                        // note-on, channel 10, kick (36)
        const qint64 mpqn = qint64(60000000.0 / p.bpm);
        QVERIFY(f.contains(QByteArray("\x00\xFF\x51\x03", 4) + QByteArray(1, char(mpqn >> 16)) + QByteArray(1, char(mpqn >> 8)) + QByteArray(1, char(mpqn))));
        // calm has no drums and the resolving bar ends on the tonic in the lead
        const music::Piece c = music::compose(12.0, "calm", 5, 1);
        QVERIFY(c.tracks[music::Drums].empty());
        QCOMPARE(c.tracks[music::Lead].back().note % 12, c.root % 12);
        QCOMPARE(c.tracks[music::Lead].back().start, (c.bars - 1) * 16);
        // energy 2: no lead, no drums
        QCOMPARE(music::compose(12.0, "news", 2, 1).trackNames(), (QStringList{"pad", "bass", "arp"}));
    }

    // ------------------------------------------------------------------------------------------ the exe
    void cliMusicWav_data() {
        QTest::addColumn<QString>("seconds");
        QTest::addColumn<QString>("mood");
        QTest::addColumn<QString>("energy");
        QTest::addColumn<QString>("seed");
        QTest::newRow("news e9 46.5 s") << "46.5" << "news" << "9" << "1234567890";
        QTest::newRow("calm e5 10 s") << "10.00" << "calm" << "5" << "7";
        QTest::newRow("sports e8 22.25 s") << "22.25" << "sports" << "8" << "2024";
        QTest::newRow("quirky e2 1 s -> 3 s") << "1" << "quirky" << "2" << "99";
        QTest::newRow("urgent e10 31.4 s") << "31.40" << "urgent" << "10" << "1100499282097";
        QTest::newRow("solemn e3 61.77 s") << "61.77" << "solemn" << "3" << "-5";
    }
    void cliMusicWav() {
        QFETCH(QString, seconds); QFETCH(QString, mood); QFETCH(QString, energy); QFETCH(QString, seed);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString wav = dir.filePath(QString::fromUtf8("bed Ã©.wav")), mid = dir.filePath(QString::fromUtf8("bed Ã©.mid"));      // non-ASCII path on purpose
        const Run r = runTool({"music", "--out", wav, "--mid", mid, "--seconds", seconds, "--mood", mood, "--energy", energy, "--seed", seed});
        QVERIFY2(r.code == 0, r.err.constData());
        QCOMPARE(int(r.out.count('\n')), 1);                                      // stdout is the one JSON line
        QVERIFY(!r.out.contains('\r'));
        // the script's keys (from the goldens' stdout) and values
        const QJsonObject pyShape = loadManifest().value("cases").toArray().first().toObject().value("stdout").toObject();
        QCOMPARE(sortedKeys(r.json), sortedKeys(pyShape));
        music::Options o; QJsonObject err; int code = 0;
        QVERIFY(music::parseOptions({"--out", wav, "--seconds", seconds, "--mood", mood, "--energy", energy, "--seed", seed}, &o, &err, &code));
        const double secs = music::clampSeconds(o.seconds);
        const music::Piece p = music::compose(secs, o.mood, o.energy, o.seed);
        QCOMPARE(r.json.value("ok").toBool(), true);
        QCOMPARE(r.json.value("wav").toString(), wav);
        QCOMPARE(r.json.value("mid").toString(), mid);
        QCOMPARE(r.json.value("bpm").toInt(), p.bpm);
        QCOMPARE(r.json.value("key").toString(), p.key);
        QCOMPARE(r.json.value("mood").toString(), p.mood);
        QCOMPARE(r.json.value("seconds").toDouble(), secs);
        QCOMPARE(r.json.value("tracks").toArray(), QJsonArray::fromStringList(p.trackNames()));
        QCOMPARE(r.json.value("notes").toInt(), p.noteCount());
        QCOMPARE(readAll(mid), music::midiFile(p));
        // the WAV
        int rate = 0, ch = 0; qint64 frames = 0;
        QVERIFY(wavio::info(wav, &rate, &ch, &frames));
        QCOMPARE(rate, 48000);
        QCOMPARE(ch, 2);
        QCOMPARE(frames, qint64(std::ceil(secs * 48000)));
        wavio::Wav w; QVERIFY(wavio::readFile(wav, &w));
        QVERIFY(w.isPcm16());
        const double level = rmsDb(w);
        const int peak = peakAbs(w);
        QVERIFY2(std::fabs(level + 12.0) <= 1.0, qPrintable(QString("RMS %1 dBFS").arg(level)));
        QVERIFY2(peak <= int(0.95 * 32767), qPrintable(QString("peak %1").arg(peak / 32767.0)));
        QVERIFY(peak > 3000);
        QCOMPARE(w.samples[0], qint16(0));                                     // the fade-in starts at 0
        QCOMPARE(w.samples[w.samples.size() - 1], qint16(0));                  // the fade-out ends at 0
        qInfo("%s: %lld frames, RMS %.2f dBFS, peak %.3f, %lld ms", QTest::currentDataTag(), frames, level, peak / 32767.0, r.ms);
    }

    void cliDeterministic() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QByteArray wav[2], mid[2];
        for (int i = 0; i < 2; ++i) {
            const QString w = dir.filePath(QString("a%1.wav").arg(i)), m = dir.filePath(QString("a%1.mid").arg(i));
            const Run r = runTool({"music", "--out", w, "--mid", m, "--seconds", "14", "--mood", "sports", "--energy", "9", "--seed", "31337"});
            QCOMPARE(r.code, 0);
            wav[i] = readAll(w); mid[i] = readAll(m);
        }
        QVERIFY(!wav[0].isEmpty());
        QVERIFY(wav[0] == wav[1]);
        QVERIFY(mid[0] == mid[1]);
        // another seed gives another tune
        const QString w2 = dir.filePath("b.wav");
        QCOMPARE(runTool({"music", "--out", w2, "--seconds", "14", "--mood", "sports", "--energy", "9", "--seed", "31338"}).code, 0);
        QVERIFY(readAll(w2) != wav[0]);
        QVERIFY(!QFileInfo::exists(dir.filePath("b.mid")));                  // no --mid, no MIDI file
    }

    void cliErrors() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString wav = dir.filePath("e.wav");
        Run r = runTool({"music", "--out", wav, "--seconds", "5", "--bogus", "1"});
        QCOMPARE(r.code, 64);
        QVERIFY(r.json.value("unsupported").toBool());
        QCOMPARE(r.json.value("error").toString(), QString("unsupported: --bogus"));
        r = runTool({"music", "--seconds", "5"});
        QCOMPARE(r.code, 2);
        QCOMPARE(r.json.value("ok").toBool(true), false);
        QVERIFY(r.json.value("error").toString().contains("--out"));
        r = runTool({"music", "--out", wav, "--seconds", "abc"});
        QCOMPARE(r.code, 2);
        QVERIFY(r.json.value("error").toString().contains("invalid float value"));
        r = runTool({"music", "--out", wav, "--seconds", "5", "--seed", "1.5"});
        QCOMPARE(r.code, 2);
        r = runTool({"music", "--out", dir.filePath("no/such/dir/x.wav"), "--mid", dir.filePath("no/such/dir/x.mid"), "--seconds", "5"});
        QCOMPARE(r.code, 2);
        QCOMPARE(r.json.value("ok").toBool(true), false);
        QVERIFY(!r.json.value("error").toString().isEmpty());
        QVERIFY(!QFileInfo::exists(wav));
        r = runTool({"frobnicate", "--x"});
        QCOMPARE(r.code, 64);
        QVERIFY(r.json.value("unsupported").toBool());
        r = runTool({});
        QCOMPARE(r.code, 2);
        r = runTool({"version"});
        QCOMPARE(r.code, 0);
        QCOMPARE(r.json.value("tool").toString(), QString("PlayPlaySpotRender"));
        QVERIFY(r.json.value("commands").toArray().contains(QJsonValue("music")));
        r = runTool({"music", "--help"});
        QCOMPARE(r.code, 0);
        QVERIFY(r.out.contains("usage: PlayPlaySpotRender music"));
    }

    // ------------------------------------------------------------------------------------------ against the script's WAV
    void wavMatchesPythonSynth_data() {
        QTest::addColumn<QString>("mood");
        QTest::addColumn<QString>("energy");
        QTest::addColumn<bool>("drums");
        QTest::newRow("calm (no drums)") << "calm" << "6" << false;
        QTest::newRow("night (no drums)") << "night" << "8" << false;
        QTest::newRow("news (drums)") << "news" << "9" << true;
        QTest::newRow("sports (drums, lead)") << "sports" << "7" << true;
    }
    void wavMatchesPythonSynth() {
        QFETCH(QString, mood); QFETCH(QString, energy); QFETCH(bool, drums);
        const QString py = findPythonWithNumpy();
        if (py.isEmpty()) QSKIP("no Python with numpy here (set PLAYPLAY_TEST_PYTHON)");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QStringList common = {"--seconds", "17.5", "--mood", mood, "--energy", energy, "--seed", "8675309"};
        const QString pw = dir.filePath("py.wav"), pm = dir.filePath("py.mid"), nw = dir.filePath("native.wav"), nm = dir.filePath("native.mid");
        QProcess q; QElapsedTimer t; t.start();
        q.start(py, QStringList{"-I", QDir(scriptsDir()).filePath("spot_music.py"), "--out", pw, "--mid", pm} + common);
        QVERIFY(q.waitForFinished(120000));
        const qint64 pyMs = t.elapsed();
        QCOMPARE(q.exitCode(), 0);
        const QJsonObject pyJson = ppcli::parseResult(q.readAllStandardOutput());
        const Run r = runTool(QStringList{"music", "--out", nw, "--mid", nm} + common);
        QCOMPARE(r.code, 0);
        // same stdout keys and values (paths aside), same MIDI bytes
        QCOMPARE(sortedKeys(r.json), sortedKeys(pyJson));
        for (const QString& k : {"ok", "bpm", "key", "mood", "seconds", "tracks", "notes"}) QCOMPARE(r.json.value(k), pyJson.value(k));
        QCOMPARE(readAll(nm), readAll(pm));
        wavio::Wav a, b;
        QVERIFY(wavio::readFile(nw, &a)); QVERIFY(wavio::readFile(pw, &b));
        QCOMPARE(a.frames(), b.frames());
        QCOMPARE(a.sampleRate, b.sampleRate);
        QCOMPARE(a.channels, b.channels);
        const double ca = correlation(a, b, 0), cb = correlation(a, b, 1), env = envelopeCorrelation(a, b);
        const double la = rmsDb(a), lb = rmsDb(b);
        qInfo("%s: correlation L %.4f R %.4f, envelope %.4f; RMS native %.2f / script %.2f dBFS; peak native %.3f / script %.3f; %lld ms / %lld ms",
              QTest::currentDataTag(), ca, cb, env, la, lb, peakAbs(a) / 32767.0, peakAbs(b) / 32767.0, r.ms, pyMs);
        if (!drums) { QVERIFY2(ca > 0.98 && cb > 0.98, "the drum-less bed should be the same waveform"); }
        QVERIFY2(env > 0.9, "the bed's loudness contour should follow the script's");
        // the script lands about 1.2 dB lower (its limiter curve), ours on -12
        QVERIFY(std::fabs(la + 12.0) <= 1.0);
        QVERIFY(la - lb > 0.0 && la - lb < 2.5);
    }
};

QTEST_GUILESS_MAIN(TestInhouseMusic)
#include "test_inhouse_music.moc"
