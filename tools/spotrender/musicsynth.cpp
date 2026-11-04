#include "musicsynth.h"

#include "ppcli.h"
#include "pyrandom.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QSaveFile>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <system_error>
#include <tuple>

// Line references (Lxx) are scripts/spot_music.py. Arithmetic in compose() keeps the script's operand order and types
// (Python float = IEEE double, int() truncates, round() ties to even) because the tempo and the velocities must match
// to the bit for the MIDI bytes to match.
namespace music {
namespace {

constexpr double kPi = 3.14159265358979323846;
const char* const kNoteNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};

const std::vector<int>& scaleSteps(const QString& name) {     // SCALES (L33-36)
    static const std::map<QString, std::vector<int>> s = {
        {"major", {0, 2, 4, 5, 7, 9, 11}}, {"minor", {0, 2, 3, 5, 7, 8, 10}}, {"dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"lydian", {0, 2, 4, 6, 7, 9, 11}}, {"mixolydian", {0, 2, 4, 5, 7, 9, 10}}, {"pent", {0, 2, 4, 7, 9}}};
    return s.at(name);
}

struct MoodDef {               // MOODS (L38-52)
    const char* name;
    std::vector<QString> scales;
    int bpmLo, bpmHi;
    std::vector<std::vector<int>> progs;
    const char* bass;
    int arpStride;
    std::vector<int> arpSeq;
    const char* drums;         // nullptr = none
    bool lead;
    std::array<int, 4> gm;     // pad bass arp lead
};

const std::vector<MoodDef>& moodTable() {
    static const std::vector<MoodDef> t = {
        {"calm",      {"major"},               68, 78,   {{0, 5, 3, 4}, {0, 3, 0, 4}}, "whole",     4, {0, 1, 2, 1},             nullptr, true,  {89, 32, 4, 73}},
        {"warm",      {"major"},               84, 96,   {{0, 4, 5, 3}, {0, 3, 4, 4}}, "half",      2, {0, 1, 2, 1},             "soft",  true,  {88, 33, 4, 73}},
        {"bright",    {"major", "lydian"},     100, 112, {{0, 4, 5, 3}, {0, 3, 4, 3}}, "eighth",    2, {0, 2, 1, 2},             "pop",   true,  {88, 33, 11, 80}},
        {"news",      {"dorian", "minor"},     104, 116, {{0, 5, 3, 4}, {0, 2, 3, 4}}, "eighth",    1, {0, 2, 1, 2, 0, 2, 1, 2}, "drive", false, {89, 38, 5, 81}},
        {"urgent",    {"minor"},               122, 134, {{0, 0, 5, 4}, {0, 5, 0, 4}}, "sixteenth", 1, {0, 1, 2, 1},             "drive", false, {89, 38, 5, 81}},
        {"sports",    {"major", "mixolydian"}, 118, 130, {{0, 3, 4, 3}, {0, 4, 5, 4}}, "eighth",    2, {0, 1, 2, 1},             "big",   true,  {61, 34, 29, 80}},
        {"upbeat",    {"major"},               108, 120, {{0, 4, 5, 3}, {0, 3, 0, 4}}, "eighth",    2, {0, 2, 1, 2},             "pop",   true,  {88, 33, 4, 73}},
        {"solemn",    {"minor"},               54, 62,   {{0, 5, 3, 4}, {0, 3, 5, 4}}, "whole",     4, {0, 1, 2, 1},             nullptr, true,  {48, 32, 0, 71}},
        {"cinematic", {"minor"},               72, 84,   {{0, 5, 2, 4}, {0, 3, 5, 4}}, "pulse",     2, {0, 1, 2, 1},             "soft",  true,  {48, 33, 46, 70}},
        {"steady",    {"dorian"},              98, 108,  {{0, 3, 0, 4}, {0, 2, 3, 2}}, "eighth",    2, {0, 1, 2, 1},             "soft",  false, {89, 38, 4, 73}},
        {"quirky",    {"pent"},                112, 124, {{0, 2, 3, 1}, {0, 3, 2, 1}}, "bounce",    1, {0, 2, 1, 3},             "pop",   true,  {88, 33, 12, 80}},
        {"night",     {"lydian"},              58, 66,   {{0, 4, 5, 3}, {0, 1, 4, 0}}, "whole",     4, {2, 1, 0, 1},             nullptr, true,  {89, 32, 98, 88}},
    };
    return t;
}
const MoodDef& moodDef(const QString& name) {
    for (const MoodDef& m : moodTable()) if (name == QLatin1String(m.name)) return m;
    return moodTable()[1];    // "warm" (pickMood never returns anything else)
}

struct DrumDef { std::vector<int> kick, snare, hat; double vel; };    // DRUMS (L56-61)
const DrumDef& drumDef(const char* name) {
    static const std::vector<int> even = {0, 2, 4, 6, 8, 10, 12, 14};
    static const std::map<QString, DrumDef> d = {
        {"soft", {{0, 8}, {}, {4, 12}, 0.7}}, {"pop", {{0, 8}, {4, 12}, even, 0.85}},
        {"drive", {{0, 6, 8, 14}, {4, 12}, even, 0.95}}, {"big", {{0, 4, 8, 12}, {4, 12}, even, 1.0}}};
    return d.at(QString::fromLatin1(name));
}

struct BassStep { int step, offset, length; };                       // BASS (L62-69)
const std::vector<BassStep>& bassDef(const char* name) {
    static const std::map<QString, std::vector<BassStep>> b = [] {
        std::map<QString, std::vector<BassStep>> m;
        m["whole"] = {{0, 0, 16}};
        m["half"] = {{0, 0, 8}, {8, 0, 8}};
        for (int s = 0; s < 16; s += 2) m["pulse"].push_back({s, 0, 2});
        m["eighth"] = {{0, 0, 2}, {2, 0, 2}, {4, 12, 2}, {6, 0, 2}, {8, 0, 2}, {10, 0, 2}, {12, 7, 2}, {14, 0, 2}};
        for (int s = 0; s < 16; ++s) m["sixteenth"].push_back({s, s % 8 == 6 ? 12 : 0, 1});
        m["bounce"] = {{0, 0, 2}, {3, 12, 1}, {6, 7, 2}, {8, 0, 2}, {11, 12, 1}, {14, 7, 2}};
        return m;
    }();
    return b.at(QString::fromLatin1(name));
}

int floorDiv(int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }
int floorMod(int a, int b) { const int r = a % b; return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r; }

// Piece.deg / Piece.chord (L101-106): Python's // and % floor towards -inf.
int deg(const Piece& p, int d, int octave = 0) {
    const int n = int(p.scale.size());
    return p.root + 12 * (octave + floorDiv(d, n)) + p.scale[size_t(floorMod(d, n))];
}
std::array<int, 3> chord(const Piece& p, int d) { return {deg(p, d), deg(p, d + 2), deg(p, d + 4)}; }

// Python's float(text) as argparse `type=float` applies it.
bool parsePyFloat(const QString& text, double* out) {
    QString s = text.trimmed().toLower();
    if (s.isEmpty()) return false;
    QString sign;
    if (s[0] == '+' || s[0] == '-') { sign = s.left(1); s = s.mid(1); }
    if (s == "inf" || s == "infinity") { *out = sign == "-" ? -INFINITY : INFINITY; return true; }
    if (s == "nan") { *out = NAN; return true; }
    // PEP 515: single underscores between digits only
    QString t;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (i == 0 || i + 1 >= s.size() || !s[i - 1].isDigit() || !s[i + 1].isDigit()) return false;
            continue;
        }
        if (s[i].unicode() > 127) return false;
        t += s[i];
    }
    if (t.isEmpty() || t.contains("inf") || t.contains("nan")) return false;
    const QByteArray b = (sign == "-" ? QStringLiteral("-") + t : t).toLatin1();
    double v = 0.0;
    const auto r = std::from_chars(b.constData(), b.constData() + b.size(), v, std::chars_format::general);
    if (r.ec == std::errc::result_out_of_range) {                 // Python: overflow -> inf, underflow -> 0.0
        *out = std::strtod(b.constData(), nullptr);
        return true;
    }
    if (r.ec != std::errc() || r.ptr != b.constData() + b.size()) return false;
    *out = v;
    return true;
}

// ----------------------------------------------------------------------------- synthesizer helpers (L208-268)
constexpr int SR = kSampleRate;

double hz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// adsr(n, a, d, s, rel, held) for one sample
struct Adsr {
    double a, d, s; int held, n, relN;
    Adsr(int n_, double a_, double d_, double s_, double rel, int held_) : a(a_), d(d_), s(s_), held(held_), n(n_), relN(int(rel * SR)) {}
    double at(int i) const {
        const double t = double(i) / SR;
        double env = std::min(1.0, t / std::max(a, 1e-4));
        if (t > a) env = 1.0 - (1.0 - s) * std::min(1.0, (t - a) / std::max(d, 1e-4));
        if (held < n && i >= held) env *= std::clamp(1.0 - double(i - held) / std::max(relN, 1), 0.0, 1.0);
        return env;
    }
};

// sin(w * i + phase) by rotating a unit phasor: one complex multiply per sample, renormalised so the amplitude never
// drifts. Double precision keeps the phase exact to ~1e-10 rad over minutes (numpy computed these phases in float32).
struct Phasor {
    double c, s, cr, sr; int k = 0;
    Phasor(double w, double phase) : c(std::cos(phase)), s(std::sin(phase)), cr(std::cos(w)), sr(std::sin(w)) {}
    double next() {
        const double v = s;
        const double nc = c * cr - s * sr, ns = s * cr + c * sr;
        c = nc; s = ns;
        if (++k == 4096) { k = 0; const double g = 1.0 / std::sqrt(c * c + s * s); c *= g; s *= g; }
        return v;
    }
};

// Standard normal noise for the drums: numpy's default_rng(12345) is PCG64 + ziggurat; we only need a fixed, portable
// sequence, so mt19937_64 (fully specified by the standard) + Box-Muller written out (std::normal_distribution is not
// specified bit for bit).
struct Gauss {
    std::mt19937_64 g{12345};
    bool hasSpare = false; double spare = 0.0;
    double next() {
        if (hasSpare) { hasSpare = false; return spare; }
        const double u1 = double((g() >> 11) + 1) * 0x1p-53;     // (0, 1]
        const double u2 = double(g() >> 11) * 0x1p-53;           // [0, 1)
        const double r = std::sqrt(-2.0 * std::log(u1));
        spare = r * std::sin(2.0 * kPi * u2); hasSpare = true;
        return r * std::cos(2.0 * kPi * u2);
    }
};

std::vector<float> voiceWave(int kind, int note, double lengthS) {      // voice() L224-252
    const double rel = kind == Pad ? 0.7 : kind == Bass ? 0.10 : kind == Arp ? 0.35 : 0.22;
    const int n = std::max(0, int((lengthS + rel) * SR));
    const int held = int(lengthS * SR);
    const double f = hz(note);
    std::vector<double> w(size_t(n), 0.0);
    std::vector<float> out(static_cast<size_t>(n));
    if (kind == Pad) {             // three detuned band-limited saws: a warm string-like pad
        for (double det : {-0.07, 0.0, 0.07}) {
            const double fd = f * std::pow(2.0, det / 12);
            for (int k = 1; k < 9; ++k) {
                if (!(fd * k < 5200)) continue;
                Phasor ph(2 * kPi * fd * k / SR, double(k));
                for (int i = 0; i < n; ++i) w[size_t(i)] += ph.next() / k;
            }
        }
        const Adsr env(n, 0.35, 0.4, 0.8, rel, held);
        for (int i = 0; i < n; ++i) out[size_t(i)] = float(w[size_t(i)] * 0.22 * env.at(i));
    } else if (kind == Bass) {     // sine + octave + a little square: round and punchy
        Phasor p1(2 * kPi * f / SR, 0.0), p2(2 * kPi * 2 * f / SR, 0.0);
        const Adsr env(n, 0.004, 0.18, 0.55, rel, held);
        for (int i = 0; i < n; ++i) {
            const double s1 = p1.next(), s2 = p2.next();
            const double sq = s1 > 0 ? 1.0 : s1 < 0 ? -1.0 : 0.0;           // np.sign
            out[size_t(i)] = float((s1 + 0.45 * s2 + 0.18 * sq) * 0.55 * env.at(i));
        }
    } else if (kind == Arp) {      // electric-piano style FM: bright attack, soft decay
        Phasor p2(2 * kPi * 2 * f / SR, 0.0), p4(2 * kPi * 4 * f / SR, 0.0);
        const double wc = 2 * kPi * f / SR;
        const Adsr env(n, 0.003, 0.25, 0.35, rel, held);
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / SR, dec = std::exp(-t * 4.0);
            const double s2 = p2.next(), s4 = p4.next();
            const double v = std::sin(wc * i + 1.6 * dec * s2) + 0.3 * s4 * dec;
            out[size_t(i)] = float(v * 0.5 * env.at(i));
        }
    } else {                       // lead: triangle + a little vibrato, soft attack
        const Adsr env(n, 0.02, 0.2, 0.7, rel, held);
        double cum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / SR;
            cum += 1.0 + 0.004 * std::sin(2 * kPi * 5.2 * t) * std::min(1.0, t / 0.4);     // np.cumsum(vib)
            const double ph = 2 * kPi * f * cum / SR;
            const double v = (2 / kPi) * std::asin(std::sin(ph)) * 0.8 + 0.25 * std::sin(2 * ph);
            out[size_t(i)] = float(v * 0.5 * env.at(i));
        }
    }
    return out;
}

std::vector<float> drumWave(int note, Gauss& rng) {                      // drum_hit() L255-268
    if (note == 36) {              // kick: sine sweep down
        const int n = int(0.32 * SR);
        std::vector<float> o(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / SR;
            const double ph = 2 * kPi * (45 * t + 75 * (1 - std::exp(-t * 28)) / 28);
            o[size_t(i)] = float(std::sin(ph) * std::exp(-t * 11) * 1.1);
        }
        return o;
    }
    if (note == 38) {              // snare: noise + a body tone
        const int n = int(0.22 * SR);
        std::vector<double> noise(static_cast<size_t>(n));
        for (double& v : noise) v = rng.next();
        std::vector<float> o(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double t = double(i) / SR;
            const double nz = noise[size_t(i)] - (i > 0 ? noise[size_t(i - 1)] : 0.0) * 0.6;
            o[size_t(i)] = float(nz * std::exp(-t * 20) * 0.5 + std::sin(2 * kPi * 190 * t) * std::exp(-t * 28) * 0.45);
        }
        return o;
    }
    const int n = int(0.07 * SR);  // hat: high-passed noise
    std::vector<double> noise(static_cast<size_t>(n));
    for (double& v : noise) v = rng.next();
    std::vector<float> o(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = double(i) / SR;
        o[size_t(i)] = float((noise[size_t(i)] - (i > 0 ? noise[size_t(i - 1)] : 0.0)) * std::exp(-t * 70) * 0.22);
    }
    return o;
}

// ----------------------------------------------------------------------------- MIDI (L168-205)
void vlq(QByteArray& out, quint32 n) {
    unsigned char b[5]; int k = 0;
    b[k++] = n & 0x7F; n >>= 7;
    while (n) { b[k++] = (n & 0x7F) | 0x80; n >>= 7; }
    while (k) out.append(char(b[--k]));
}
void be32(QByteArray& out, quint32 v) { for (int s = 24; s >= 0; s -= 8) out.append(char((v >> s) & 0xFF)); }
void be16(QByteArray& out, quint32 v) { out.append(char((v >> 8) & 0xFF)); out.append(char(v & 0xFF)); }

QByteArray chunk(const QByteArray& data) { QByteArray c("MTrk"); be32(c, quint32(data.size())); return c + data; }

QByteArray midiTrack(const std::vector<NoteEvent>& events, int channel, int program, const char* name) {
    struct Ev { qint64 t; int on; unsigned char msg[3]; };
    std::vector<Ev> ev;
    ev.reserve(events.size() * 2);
    for (const NoteEvent& e : events) {
        const qint64 t0 = qint64(e.start) * kPpq / 4, t1 = qint64(e.start + e.length) * kPpq / 4;
        ev.push_back({t0, 1, {quint8(0x90 | channel), quint8(e.note & 0x7F), quint8(std::max(1, std::min(127, e.velocity)))}});
        ev.push_back({t1, 0, {quint8(0x80 | channel), quint8(e.note & 0x7F), 0}});
    }
    // ev.sort(key=(t, on)): note-offs before note-ons at the same tick; Python's sort is stable
    std::stable_sort(ev.begin(), ev.end(), [](const Ev& a, const Ev& b) { return a.t != b.t ? a.t < b.t : a.on < b.on; });
    const QByteArray nm(name);
    QByteArray data;
    data.append(char(0x00)); data.append(char(0xFF)); data.append(char(0x03)); data.append(char(nm.size())); data.append(nm);
    if (program >= 0) { data.append(char(0x00)); data.append(char(0xC0 | channel)); data.append(char(program)); }
    qint64 last = 0;
    for (const Ev& e : ev) {
        vlq(data, quint32(e.t - last));
        data.append(reinterpret_cast<const char*>(e.msg), 3);
        last = e.t;
    }
    data.append(QByteArray::fromRawData("\x00\xFF\x2F\x00", 4));
    return chunk(data);
}

}   // namespace

// ----------------------------------------------------------------------------- public
const char* trackName(int t) {
    static const char* const n[TrackCount] = {"pad", "bass", "arp", "lead", "drums"};
    return t >= 0 && t < TrackCount ? n[t] : "";
}

int Piece::noteCount() const { int n = 0; for (const auto& t : tracks) n += int(t.size()); return n; }
QStringList Piece::trackNames() const {
    QStringList l;
    for (int t = 0; t < TrackCount; ++t) if (!tracks[size_t(t)].empty()) l << QString::fromLatin1(trackName(t));
    return l;
}

QStringList moodNames() { QStringList l; for (const MoodDef& m : moodTable()) l << QString::fromLatin1(m.name); return l; }

QString pickMood(const QString& name) {                       // L73-76
    static const std::map<QString, QString> aliases = {
        {"driving", "news"}, {"tense", "urgent"}, {"energetic", "sports"}, {"happy", "upbeat"}, {"gentle", "calm"},
        {"sad", "solemn"}, {"dramatic", "cinematic"}, {"light", "bright"}, {"friendly", "warm"}, {"money", "steady"},
        {"fun", "quirky"}};
    QString n = name.trimmed().toLower();
    const auto a = aliases.find(n);
    if (a != aliases.end()) n = a->second;
    return moodNames().contains(n) ? n : QStringLiteral("warm");
}

double clampSeconds(double s) {
    const double m = s < 900.0 ? s : 900.0;     // min(900.0, s): keeps 900.0 unless s compares smaller (NaN does not)
    return m > 3.0 ? m : 3.0;                   // max(3.0, m)
}

Piece compose(double seconds, const QString& mood, int energy, qint64 seed) { return compose(seconds, mood, energy, PyRandom::wordsFor(seed < 0 ? quint64(0) - quint64(seed) : quint64(seed))); }

Piece compose(double seconds, const QString& moodName, int energy, const std::vector<quint32>& seedWords) {
    // Piece.__init__ (L81-99); the order of the rng calls is the script's
    Piece p;
    p.mood = pickMood(moodName);
    const MoodDef& m = moodDef(p.mood);
    PyRandom r(seedWords);
    p.energy = std::max(1, std::min(10, energy));
    const int lo = m.bpmLo, hi = m.bpmHi;
    const double u = r.uniform(0.85, 1.0);
    p.bpm = int(pyRound(lo + (hi - lo) * (0.25 + 0.07 * p.energy) * u));
    p.bpm = std::max(lo - 4, std::min(hi + 6, p.bpm));
    p.scaleName = r.choice(m.scales);
    p.scale = scaleSteps(p.scaleName);
    static const std::vector<int> roots = {48, 50, 51, 53, 55, 57};      // C3 .. A3
    p.root = r.choice(roots);
    p.key = QString("%1 %2").arg(QLatin1String(kNoteNames[p.root % 12]), p.scaleName);
    p.step = 60.0 / p.bpm / 4.0;
    p.bars = std::max(2, int(std::ceil(seconds / (p.step * 16))) + 1);
    p.progression = r.choice(m.progs);
    p.gm = m.gm;

    // _compose (L108-164)
    const int e = p.energy;
    const double vscale = 0.7 + 0.03 * e;
    std::vector<int> degs(size_t(p.bars));
    for (int b = 0; b < p.bars; ++b) degs[size_t(b)] = p.progression[size_t(b) % p.progression.size()];
    degs.back() = 0;                            // the last bar resolves to the tonic
    auto& pad = p.tracks[Pad]; auto& bass = p.tracks[Bass]; auto& arp = p.tracks[Arp];
    auto& lead = p.tracks[Lead]; auto& drums = p.tracks[Drums];
    for (int b = 0; b < p.bars; ++b) {
        const int s0 = b * 16, d = degs[size_t(b)];
        const std::array<int, 3> ch = chord(p, d);
        const bool last = b == p.bars - 1;
        for (int n : ch) pad.push_back({s0, 16, n, int(52 * vscale)});
        for (const BassStep& bs : (last ? bassDef("whole") : bassDef(m.bass)))
            bass.push_back({s0 + bs.step, bs.length, ch[0] - 12 + bs.offset, int(86 * vscale)});
        if (!last) {                            // arpeggio over the chord tones (skipped in the resolving bar)
            const int tones[4] = {ch[0] + 12, ch[1] + 12, ch[2] + 12, ch[1] + 24};
            int i = 0;
            for (int st = 0; st < 16; st += m.arpStride, ++i) {
                const int k = m.arpSeq[size_t(i) % m.arpSeq.size()];
                const double accent = st % 8 == 0 ? 1.0 : 0.8;
                arp.push_back({s0 + st, std::max(1, m.arpStride), tones[k % 4], int(64 * vscale * accent)});
            }
        }
    }
    if (m.drums && e >= 4) {                    // hats from energy 4, kick and snare from 5
        const DrumDef& pat = drumDef(m.drums);
        for (int b = 0; b < p.bars - 1; ++b) {
            const int s0 = b * 16;
            if (e >= 5) {
                for (int st : pat.kick) drums.push_back({s0 + st, 1, 36, int(105 * pat.vel)});
                for (int st : pat.snare) drums.push_back({s0 + st, 1, 38, int(95 * pat.vel)});
            }
            for (int st : pat.hat) drums.push_back({s0 + st, 1, 42, int((st % 4 == 0 ? 70 : 52) * pat.vel)});
            if (e >= 7 && (b % 4 == 3))         // a snare fill at the end of every fourth bar
                for (int st = 12; st < 16; ++st) drums.push_back({s0 + st, 1, 38, 70 + 8 * (st - 12)});
        }
        drums.push_back({(p.bars - 1) * 16, 1, 36, 100});     // a last kick on the resolving bar
    }
    if (m.lead && e >= 3) {                     // a two-bar motif, varied every 4 bars
        static const std::vector<std::vector<int>> rhythms = {{4, 4, 8}, {6, 2, 4, 4}, {4, 2, 2, 8}, {8, 4, 4}, {2, 2, 4, 8}};
        static const std::vector<int> start = {4, 5, 6}, moves = {-2, -1, -1, 0, 1, 1, 2};
        std::vector<std::pair<int, int>> motif;  // (length, scale degree)
        int pos = r.choice(start);
        for (int rep = 0; rep < 2; ++rep) {
            const std::vector<int>& rh = r.choice(rhythms);
            for (int ln : rh) { motif.push_back({ln, pos}); pos = std::max(2, std::min(11, pos + r.choice(moves))); }
        }
        const int end = (p.bars - 1) * 16;
        for (int b = 0; b < p.bars - 1; b += 2) {
            if ((b / 2) % 2 == 1 && e < 7) continue;     // every other phrase is left open
            int st = b * 16;
            for (size_t k = 0; k < motif.size(); ++k) {
                if (st >= end) break;
                const int ln = motif[k].first;
                int dg = motif[k].second;
                const int d = degs[size_t(st / 16)];
                static const int chordTone[3] = {0, 2, 4};
                if (st % 16 == 0) dg = d + chordTone[k % 3] + 7;   // a chord tone on the downbeat
                lead.push_back({st, std::max(1, ln - 1), dg > 6 ? deg(p, dg, 0) : deg(p, dg + 7), int(74 * vscale)});
                st += ln;
            }
        }
        lead.push_back({end, 12, deg(p, 7), int(72 * vscale)});   // the tune ends on the tonic
    }
    return p;
}

QByteArray midiFile(const Piece& p) {
    const qint64 mpqn = qint64(60000000.0 / p.bpm);
    QByteArray cond = QByteArray::fromRawData("\x00\xFF\x03\x09" "conductor", 13);
    cond.append(QByteArray::fromRawData("\x00\xFF\x51\x03", 4));
    cond.append(char((mpqn >> 16) & 0xFF)); cond.append(char((mpqn >> 8) & 0xFF)); cond.append(char(mpqn & 0xFF));
    cond.append(QByteArray::fromRawData("\x00\xFF\x58\x04\x04\x02\x18\x08", 8));
    cond.append(QByteArray::fromRawData("\x00\xFF\x59\x02\x00", 5));
    cond.append(char(p.scaleName == "minor" || p.scaleName == "dorian" ? 1 : 0));
    cond.append(QByteArray::fromRawData("\x00\xFF\x2F\x00", 4));
    std::vector<QByteArray> chunks{chunk(cond)};
    for (int ch = 0; ch < 4; ++ch)               // pad bass arp lead on channels 0..3 with their GM programs
        if (!p.tracks[size_t(ch)].empty()) chunks.push_back(midiTrack(p.tracks[size_t(ch)], ch, p.gm[size_t(ch)], trackName(ch)));
    if (!p.tracks[Drums].empty()) chunks.push_back(midiTrack(p.tracks[Drums], 9, -1, "drums"));
    QByteArray f("MThd");
    be32(f, 6); be16(f, 1); be16(f, quint32(chunks.size())); be16(f, kPpq);
    for (const QByteArray& c : chunks) f.append(c);
    return f;
}

wavio::Wav render(const Piece& p, double seconds) {     // render() L271-313
    const qint64 n = qint64(std::ceil(seconds * SR));
    const qint64 padLen = qint64(p.bars * 16 * p.step * SR) + SR * 2;
    const qint64 len = std::max(padLen, n);
    std::vector<float> L(size_t(len), 0.0f), R(size_t(len), 0.0f), dry(static_cast<size_t>(len));
    static const double pan[TrackCount] = {0.0, 0.0, 0.25, -0.2, 0.0};
    static const double gain[TrackCount] = {0.55, 0.9, 0.55, 0.5, 0.8};
    std::map<std::tuple<int, int, double>, std::vector<float>> voices;
    std::map<int, std::vector<float>> hits;
    Gauss rng;
    const qint64 haas = qint64(0.012 * SR);
    for (int kind = 0; kind < TrackCount; ++kind) {
        const auto& evs = p.tracks[size_t(kind)];
        if (evs.empty()) continue;              // an empty track adds nothing
        std::fill(dry.begin(), dry.end(), 0.0f);
        for (const NoteEvent& ev : evs) {
            const qint64 pos = qint64(ev.start * p.step * SR);
            const std::vector<float>* w;
            if (kind == Drums) {
                auto it = hits.find(ev.note);
                if (it == hits.end()) it = hits.emplace(ev.note, drumWave(ev.note, rng)).first;
                w = &it->second;
            } else {
                const double dur = pyRound(ev.length * p.step, 3);
                const auto key = std::make_tuple(kind, ev.note, dur);
                auto it = voices.find(key);
                if (it == voices.end()) it = voices.emplace(key, voiceWave(kind, ev.note, dur)).first;
                w = &it->second;
            }
            if (pos < 0 || pos >= padLen) continue;
            const qint64 end = std::min(padLen, pos + qint64(w->size()));
            const float vel = float(ev.velocity / 100.0);
            for (qint64 i = pos; i < end; ++i) dry[size_t(i)] += (*w)[size_t(i - pos)] * vel;
        }
        if (kind == Arp) {                      // a dotted-eighth echo: arp[d:] += 0.38 dry[:-d]; arp[2d:] += 0.16 dry[:-2d]
            const qint64 d = qint64(p.step * 3 * SR);
            if (d > 0)
                for (qint64 i = padLen - 1; i >= d; --i)        // descending, so dry[i - d] is still the dry signal
                    dry[size_t(i)] += 0.38f * dry[size_t(i - d)] + (i >= 2 * d ? 0.16f * dry[size_t(i - 2 * d)] : 0.0f);
        }
        const double g = gain[kind] * 0.5;
        const float lp = float((1 - std::max(0.0, pan[kind])) * g), rp = float((1 - std::max(0.0, -pan[kind])) * g);
        for (qint64 i = 0; i < padLen; ++i) L[size_t(i)] += dry[size_t(i)] * lp;
        if (kind == Pad) { for (qint64 i = 0; i + haas < padLen; ++i) R[size_t(i + haas)] += dry[size_t(i)] * rp; }   // Haas widening
        else { for (qint64 i = 0; i < padLen; ++i) R[size_t(i)] += dry[size_t(i)] * rp; }
    }
    std::vector<float>().swap(dry);

    // Level to -12 dBFS RMS, soft-limit at 0.95, fade in / out. The script scales the active part to -12 dBFS and then
    // applies tanh(0.9 x) / 0.9 * 0.95, a curve that goes past full scale (np.clip then hard-clips at 1.0) and leaves the
    // bed at about -13.2 dBFS. Here the limiter is 0.95 * tanh(x / 0.95) (never over 0.95) and the gain is corrected in
    // three passes for what the limiter and the fades take off, so the finished WAV sits at -12 dBFS RMS.
    const qint64 act = std::max<qint64>(1, std::min(n, qint64(seconds * SR)));
    const qint64 fi = qint64(0.05 * SR), fo = std::min(n, qint64(std::min(1.2, seconds / 3) * SR));
    auto fadeAt = [&](qint64 i) {
        double f = 1.0;
        if (i < fi && fi > 1) f *= double(i) / double(fi - 1);                           // np.linspace(0, 1, fi)
        if (i >= n - fo && fo > 1) f *= 1.0 - double(i - (n - fo)) / double(fo - 1);      // np.linspace(1, 0, fo)
        return f;
    };
    auto finalSample = [&](float x, double gn, double fade) { return 0.95 * std::tanh(double(x) * gn / 0.95) * fade; };
    // The gain passes read every 3rd frame (a level estimate within ~0.01 dB; tanh over every sample four times was most
    // of the run time on long beds); the raw level, like the script's, reads them all.
    auto rmsOf = [&](double gn, bool finished) {
        double acc = 0.0; qint64 cnt = 0;
        const qint64 stride = finished ? 3 : 1;
        for (qint64 i = 0; i < act; i += stride, ++cnt) {
            if (finished) {
                const double f = fadeAt(i), l = finalSample(L[size_t(i)], gn, f), r = finalSample(R[size_t(i)], gn, f);
                acc += l * l + r * r;
            } else acc += double(L[size_t(i)]) * L[size_t(i)] + double(R[size_t(i)]) * R[size_t(i)];
        }
        return std::sqrt(acc / double(2 * std::max<qint64>(1, cnt)));
    };
    const double target = std::pow(10.0, -12.0 / 20.0);
    double rms = rmsOf(1.0, false);
    if (!(rms > 0.0)) rms = 1e-6;              // silence: the script's `or 1e-6`
    const double g0 = target / rms;
    double gn = g0;
    for (int pass = 0; pass < 3; ++pass) {
        const double got = rmsOf(gn, true);
        if (!(got > 0.0)) break;
        gn = std::min(g0 * 2.0, gn * target / got);      // at most +6 dB of make-up (a very peaky bed stays quieter)
    }
    wavio::Wav w;
    w.sampleRate = SR; w.channels = 2;
    w.samples.resize(size_t(n) * 2);
    for (qint64 i = 0; i < n; ++i) {
        const double f = fadeAt(i);
        for (int c = 0; c < 2; ++c) {
            const double v = finalSample(c == 0 ? L[size_t(i)] : R[size_t(i)], gn, f);
            w.samples[size_t(i) * 2 + size_t(c)] = qint16(std::clamp(v, -1.0, 1.0) * 32767.0);   // np.clip * 32767 -> <i2
        }
    }
    return w;
}

// ----------------------------------------------------------------------------- command line
QString usage() {
    return QStringLiteral(
        "usage: PlayPlaySpotRender music --out OUT [--mid MID] --seconds SECONDS [--mood MOOD] [--energy ENERGY] [--seed SEED]\n"
        "\n"
        "MIDI-style background music for generated spots (no samples, no network); the C++ port of scripts/spot_music.py.\n"
        "A small composer writes a General MIDI file (conductor + pad, bass, arpeggio, lead and drums tracks) and a built-in\n"
        "synthesizer renders it to a 48 kHz stereo WAV of exactly --seconds (3..900).\n"
        "  mood    news urgent sports upbeat warm bright calm night solemn cinematic steady quirky (aliases: driving tense\n"
        "          energetic happy gentle sad dramatic light friendly money fun; anything else: warm)\n"
        "  energy  1..10: below 4 no hats, from 5 kick and snare and the lead line join, high energy raises tempo and velocities\n"
        "  seed    the same seed gives the same tune (key, tempo, progression, melody), byte-identical MIDI to the script\n"
        "Stdout: one JSON object {ok, wav, mid, bpm, key, mood, seconds, tracks, notes}. Exit 0 / 2 (64: unknown option).\n");
}

bool parseOptions(const QStringList& args, Options* o, QJsonObject* result, int* exitCode, bool* helpShown) {
    if (helpShown) *helpShown = false;
    const ppcli::Args a(args, {"--out=", "--mid=", "--seconds=", "--mood=", "--energy=", "--seed=", "--help", "-h"});
    if (!a.ok()) {
        *result = a.unknownOption() ? ppcli::unsupportedResult(a.unknownName()) : ppcli::failure(a.error());
        *exitCode = ppcli::exitCodeFor(*result);
        return false;
    }
    if (a.has("--help") || a.has("-h")) { if (helpShown) *helpShown = true; *exitCode = ppcli::ExitOk; return false; }
    QStringList missing;
    if (!a.has("--out")) missing << "--out";
    if (!a.has("--seconds")) missing << "--seconds";
    auto bad = [&](const QString& msg) { *result = ppcli::failure(msg); *exitCode = ppcli::ExitFailed; return false; };
    if (!missing.isEmpty()) return bad("the following arguments are required: " + missing.join(", "));
    o->out = a.value("--out");
    o->mid = a.value("--mid");
    if (!parsePyFloat(a.value("--seconds"), &o->seconds)) return bad(QString("argument --seconds: invalid float value: '%1'").arg(a.value("--seconds")));
    if (a.has("--mood")) o->mood = a.value("--mood");
    if (a.has("--energy")) {
        bool neg = false; std::vector<quint32> mag;
        if (!PyRandom::parseInt(a.value("--energy"), &neg, &mag)) return bad(QString("argument --energy: invalid int value: '%1'").arg(a.value("--energy")));
        // max(1, min(10, energy)) works on Python's unbounded int: clamp before it can overflow an int
        const bool big = mag.size() > 1 || mag[0] > 10u;
        o->energy = neg ? (big ? 1 : -int(mag[0])) : (big ? 10 : int(mag[0]));
    }
    if (a.has("--seed")) {
        bool neg = false;
        if (!PyRandom::parseInt(a.value("--seed"), &neg, &o->seed)) return bad(QString("argument --seed: invalid int value: '%1'").arg(a.value("--seed")));
    }
    return true;
}

QJsonObject run(const Options& o) {
    try {
        const double secs = clampSeconds(o.seconds);
        const Piece piece = compose(secs, o.mood, o.energy, o.seed);
        QString err;
        if (!o.mid.isEmpty()) {                 // the script writes the MIDI file first, then the WAV
            QSaveFile f(o.mid);
            if (!f.open(QIODevice::WriteOnly)) return ppcli::failure(QString("%1: %2").arg(o.mid, f.errorString()));
            f.write(midiFile(piece));
            if (!f.commit()) return ppcli::failure(QString("%1: %2").arg(o.mid, f.errorString()));
        }
        const wavio::Wav w = render(piece, secs);
        if (!wavio::writeFile(o.out, w, &err)) return ppcli::failure(err.isEmpty() ? QString("%1: could not write").arg(o.out) : err);
        return QJsonObject{{"ok", true}, {"wav", o.out}, {"mid", o.mid}, {"bpm", piece.bpm}, {"key", piece.key},
                           {"mood", piece.mood}, {"seconds", secs}, {"tracks", QJsonArray::fromStringList(piece.trackNames())},
                           {"notes", piece.noteCount()}};
    } catch (const std::bad_alloc&) {
        return ppcli::failure("out of memory");
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
    if (!r.value("ok").toBool()) ppcli::log(r.value("error").toString());
    return ppcli::finish(r);
}

}   // namespace music
