#include "wavio.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace wavio {

namespace {
constexpr quint16 kTagPcm = 1, kTagFloat = 3, kTagExtensible = 0xFFFE;

struct Fmt { quint16 tag = 0, channels = 0, bits = 0; quint32 rate = 0; };

bool setErr(QString* err, const QString& m) { if (err) *err = m; return false; }
quint16 u16(const uchar* p) { return qFromLittleEndian<quint16>(p); }
quint32 u32(const uchar* p) { return qFromLittleEndian<quint32>(p); }
double f32(const uchar* p) { const quint32 v = u32(p); float f; std::memcpy(&f, &v, 4); return double(f); }
double f64(const uchar* p) { const quint64 v = qFromLittleEndian<quint64>(p); double d; std::memcpy(&d, &v, 8); return d; }

bool readFmt(const uchar* p, quint32 len, Fmt* f, QString* err) {
    if (len < 16) return setErr(err, "bad fmt chunk");
    f->tag = u16(p); f->channels = u16(p + 2); f->rate = u32(p + 4); f->bits = u16(p + 14);
    if (f->tag == kTagExtensible && len >= 26) f->tag = u16(p + 24);      // first two bytes of the SubFormat GUID
    return true;
}
bool checkFmt(const Fmt& f, QString* err) {
    if (f.channels < 1 || f.channels > 64 || f.rate < 1 || f.rate > 1000000) return setErr(err, QString("bad WAV header: %1 channel(s) at %2 Hz").arg(f.channels).arg(f.rate));
    const bool pcm = f.tag == kTagPcm && (f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32);
    const bool flt = f.tag == kTagFloat && (f.bits == 32 || f.bits == 64);
    if (!pcm && !flt) return setErr(err, QString("unsupported WAV format: tag %1, %2-bit").arg(f.tag).arg(f.bits));
    return true;
}
// Python's (x * 32767.0).astype(int16): scale, truncate towards zero.
qint16 fromUnit(double v) { return qint16(std::clamp(v, -1.0, 1.0) * 32767.0); }
}   // namespace

// ----------------------------------------------------------------------------- read / write
bool parse(const QByteArray& bytes, Wav* out, QString* err) {
    const qint64 size = bytes.size();
    const uchar* b = reinterpret_cast<const uchar*>(bytes.constData());
    if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0) return setErr(err, "not a RIFF/WAVE file");
    Fmt f; bool haveFmt = false, haveData = false; qint64 dataOff = 0, dataLen = 0;
    for (qint64 pos = 12; pos + 8 <= size && !(haveFmt && haveData);) {
        const quint32 len = u32(b + pos + 4); const qint64 body = pos + 8;
        if (std::memcmp(b + pos, "fmt ", 4) == 0) {
            if (body + 16 > size) return setErr(err, "bad fmt chunk");
            if (!readFmt(b + body, quint32(std::min<qint64>(len, size - body)), &f, err)) return false;
            haveFmt = true;
        } else if (std::memcmp(b + pos, "data", 4) == 0) {
            // Streamed writers leave the size at 0 or 0xFFFFFFFF; a cut-off file is shorter than it says: take what is there.
            dataOff = body; dataLen = (len == 0 || len == 0xFFFFFFFFu) ? size - body : std::min<qint64>(len, size - body);
            haveData = true;
        }
        pos = body + qint64(len) + (len & 1);                 // chunks are word-aligned
    }
    if (!haveFmt) return setErr(err, "no fmt chunk");
    if (!haveData) return setErr(err, "no data chunk");
    if (!checkFmt(f, err)) return false;

    const int bps = f.bits / 8, ch = f.channels;
    const qint64 frames = dataLen / (qint64(bps) * ch);
    Wav w; w.sampleRate = int(f.rate); w.channels = ch; w.sourceBits = f.bits; w.sourceFloat = f.tag == kTagFloat;
    w.samples.resize(size_t(frames * ch));
    const uchar* p = b + dataOff;
    for (size_t i = 0; i < w.samples.size(); ++i, p += bps) {
        qint16 s = 0;
        if (f.tag == kTagFloat) { const double v = bps == 4 ? f32(p) : f64(p); s = fromUnit(std::isfinite(v) ? v : 0.0); }
        else if (bps == 1) s = qint16((int(*p) - 128) * 256);
        else if (bps == 2) s = qint16(u16(p));
        else if (bps == 3) s = qint16(qint32(quint32(p[0]) << 8 | quint32(p[1]) << 16 | quint32(p[2]) << 24) >> 16);
        else s = qint16(qint32(u32(p)) >> 16);
        w.samples[i] = s;
    }
    *out = std::move(w);
    return true;
}

QByteArray serialize(const Wav& w) {
    const quint32 dataLen = quint32(w.samples.size() * 2);
    QByteArray out(44 + int(dataLen), Qt::Uninitialized);
    uchar* p = reinterpret_cast<uchar*>(out.data());
    const quint16 ch = quint16(std::max(1, w.channels));
    std::memcpy(p, "RIFF", 4); qToLittleEndian<quint32>(36 + dataLen, p + 4); std::memcpy(p + 8, "WAVE", 4);
    std::memcpy(p + 12, "fmt ", 4); qToLittleEndian<quint32>(16, p + 16);
    qToLittleEndian<quint16>(kTagPcm, p + 20); qToLittleEndian<quint16>(ch, p + 22);
    qToLittleEndian<quint32>(quint32(w.sampleRate), p + 24); qToLittleEndian<quint32>(quint32(w.sampleRate) * ch * 2, p + 28);
    qToLittleEndian<quint16>(quint16(ch * 2), p + 32); qToLittleEndian<quint16>(16, p + 34);
    std::memcpy(p + 36, "data", 4); qToLittleEndian<quint32>(dataLen, p + 40);
    qToLittleEndian<qint16>(w.samples.data(), qsizetype(w.samples.size()), p + 44);
    return out;
}

bool readFile(const QString& path, Wav* out, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return setErr(err, "cannot open " + path + ": " + f.errorString());
    return parse(f.readAll(), out, err);
}

bool writeFile(const QString& path, const Wav& w, QString* err) {
    if (w.sampleRate <= 0 || w.channels <= 0) return setErr(err, "no audio format to write");
    QSaveFile f(path);
    f.setDirectWriteFallback(true);
    const QByteArray b = serialize(w);
    if (!f.open(QIODevice::WriteOnly) || f.write(b) != b.size() || !f.commit()) return setErr(err, "cannot write " + path + ": " + f.errorString());
    return true;
}

bool info(const QString& path, int* sampleRate, int* channels, qint64* frames, QString* err) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return setErr(err, "cannot open " + path + ": " + file.errorString());
    const qint64 size = file.size();
    uchar h[12];
    if (file.read(reinterpret_cast<char*>(h), 12) != 12 || std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVE", 4) != 0) return setErr(err, "not a RIFF/WAVE file");
    Fmt f; bool haveFmt = false; qint64 dataLen = -1;
    for (qint64 pos = 12; pos + 8 <= size && !(haveFmt && dataLen >= 0);) {
        uchar c[8];
        if (!file.seek(pos) || file.read(reinterpret_cast<char*>(c), 8) != 8) break;
        const quint32 len = u32(c + 4);
        if (std::memcmp(c, "fmt ", 4) == 0) {
            const QByteArray body = file.read(std::min<quint32>(len, 40));
            if (!readFmt(reinterpret_cast<const uchar*>(body.constData()), quint32(body.size()), &f, err)) return false;
            haveFmt = true;
        } else if (std::memcmp(c, "data", 4) == 0) {
            dataLen = (len == 0 || len == 0xFFFFFFFFu) ? size - pos - 8 : std::min<qint64>(len, size - pos - 8);
        }
        pos += 8 + qint64(len) + (len & 1);
    }
    if (!haveFmt) return setErr(err, "no fmt chunk");
    if (dataLen < 0) return setErr(err, "no data chunk");
    if (!checkFmt(f, err)) return false;
    if (sampleRate) *sampleRate = int(f.rate);
    if (channels) *channels = f.channels;
    if (frames) *frames = dataLen / (qint64(f.bits / 8) * f.channels);
    return true;
}

double fileDurationSec(const QString& path, int* sampleRate) {
    int rate = 0, ch = 0; qint64 n = 0;
    if (!info(path, &rate, &ch, &n)) { if (sampleRate) *sampleRate = 0; return 0.0; }
    if (sampleRate) *sampleRate = rate;
    return rate > 0 ? double(n) / rate : 0.0;
}

Wav fromFloat(const float* x, size_t n, int sampleRate, int channels, FloatMode mode) {
    Wav w; w.sampleRate = sampleRate; w.channels = std::max(1, channels);
    n -= n % size_t(w.channels);
    w.samples.resize(n);
    if (mode == FloatMode::PeakScale) {
        float peak = 0.0f;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(x[i]));
        const float k = peak > 1.0f ? 1.0f / peak : 1.0f;
        for (size_t i = 0; i < n; ++i) w.samples[i] = qint16(std::clamp(x[i] * k, -1.0f, 1.0f) * 32767.0f);
    } else {
        for (size_t i = 0; i < n; ++i) w.samples[i] = fromUnit(double(x[i]));
    }
    return w;
}

std::vector<float> toFloat(const Wav& w) {
    std::vector<float> y(w.samples.size());
    for (size_t i = 0; i < y.size(); ++i) y[i] = float(w.samples[i]) / 32768.0f;
    return y;
}

// ----------------------------------------------------------------------------- levelling (spot_tts.py ports)
bool trim(Wav& w, double thresholdDb, int keepMs) {
    const qint64 n = w.frames(); const int ch = w.channels;
    if (n == 0 || ch <= 0 || w.sampleRate <= 0) return false;
    const int threshold = int(32767.0 * std::pow(10.0, thresholdDb / 20.0));   // int(): truncation, as in Python
    qint64 first = -1, last = -1;
    for (qint64 i = 0; i < n; ++i) {
        int level = 0;
        for (int c = 0; c < ch; ++c) level = std::max(level, std::abs(int(w.samples[size_t(i * ch + c)])));
        if (level > threshold) { if (first < 0) first = i; last = i; }
    }
    if (first < 0) return false;                                                // all silence: leave it to the caller
    const qint64 keep = qint64(w.sampleRate * qint64(keepMs) / 1000);
    const qint64 s0 = std::max<qint64>(0, first - keep), s1 = std::min<qint64>(n, last + keep + 1);
    if (s0 == 0 && s1 == n) return false;
    w.samples.erase(w.samples.begin() + s1 * ch, w.samples.end());
    w.samples.erase(w.samples.begin(), w.samples.begin() + s0 * ch);
    return true;
}

void limitPeaks(double* y, size_t n, int rate, double ceiling) {
    if (n == 0 || rate <= 0 || ceiling <= 0) return;
    const size_t blk = std::max<size_t>(1, size_t(rate * 0.002));
    const size_t m = (n + blk - 1) / blk;
    std::vector<double> need(m), early(m), g(m);
    for (size_t b = 0; b < m; ++b) {
        double peak = 0.0;
        for (size_t i = b * blk, e = std::min(n, i + blk); i < e; ++i) peak = std::max(peak, std::fabs(y[i]));
        need[b] = std::min(1.0, ceiling / std::max(peak, 1e-9));
    }
    for (size_t b = 0; b < m; ++b) early[b] = std::min(need[b], need[std::min(b + 1, m - 1)]);     // attack one block early
    const double rel = std::exp(-double(blk) / (rate * 0.06));                                       // ~60 ms release
    double cur = 1.0;
    for (size_t b = 0; b < m; ++b) { cur = early[b] < cur ? early[b] : early[b] + (cur - early[b]) * rel; g[b] = cur; }
    // np.interp(arange(n), centres, g): linear between block centres, held flat before the first / after the last.
    const double half = 0.5 * double(blk);
    for (size_t i = 0; i < n; ++i) {
        const double x = double(i);
        double gi;
        if (x <= half) gi = g[0];
        else if (x >= (double(m) - 0.5) * double(blk)) gi = g[m - 1];
        else {
            const size_t b = size_t((x - half) / double(blk));
            const double t = (x - (double(b) + 0.5) * double(blk)) / double(blk);
            gi = g[b] + (g[std::min(b + 1, m - 1)] - g[b]) * t;
        }
        y[i] *= std::min(gi, need[i / blk]);                // the cap that keeps every sample under the ceiling
    }
}

namespace {
// Mean power of every 50 ms block (interleaved, as normalize_wav reshapes it) and of those above -50 dBFS.
bool voicedPower(const std::vector<float>& x, int rate, int ch, double* power) {
    const size_t blk = size_t(std::max(1, int(rate * 0.05))) * size_t(ch);
    const size_t m = x.size() / blk;
    double sum = 0.0; size_t count = 0;
    for (size_t b = 0; b < m; ++b) {
        double e = 0.0;
        for (size_t i = b * blk; i < (b + 1) * blk; ++i) e += double(x[i]) * double(x[i]);
        e /= double(blk);
        if (e > 1e-5) { sum += e; ++count; }
    }
    if (count == 0) return false;
    *power = sum / double(count);
    return true;
}
}   // namespace

double normalize(Wav& w, double targetDb, double ceilingDb) {
    if (w.samples.empty() || w.channels <= 0 || w.sampleRate <= 0) return 0.0;
    const std::vector<float> x = toFloat(w);
    double power = 0.0;
    if (!voicedPower(x, w.sampleRate, w.channels, &power)) return 0.0;
    const double gain = std::min(targetDb - 10.0 * std::log10(power), 24.0);
    if (std::fabs(gain) < 0.5) return 0.0;
    const float k = float(std::pow(10.0, gain / 20.0));
    const double ceiling = std::pow(10.0, ceilingDb / 20.0);
    std::vector<float> yf(x.size());
    float peak = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) { yf[i] = x[i] * k; peak = std::max(peak, std::fabs(yf[i])); }
    if (double(peak) > ceiling) {
        // Speech transients (sharper on male voices) are limited, not clipped. Same float64 path as numpy here.
        std::vector<double> y(yf.begin(), yf.end());
        limitPeaks(y.data(), y.size(), w.sampleRate * w.channels, ceiling);
        for (size_t i = 0; i < y.size(); ++i) w.samples[i] = fromUnit(y[i]);
    } else {
        for (size_t i = 0; i < yf.size(); ++i) w.samples[i] = qint16(std::clamp(yf[i], -1.0f, 1.0f) * 32767.0f);   // float32, as numpy
    }
    return gain;
}

double voicedRmsDb(const Wav& w) {
    double power = 0.0;
    if (w.channels <= 0 || w.sampleRate <= 0 || !voicedPower(toFloat(w), w.sampleRate, w.channels, &power)) return -std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(power);
}

double peakDbfs(const Wav& w) {
    int peak = 0;
    for (const qint16 s : w.samples) peak = std::max(peak, std::abs(int(s)));
    return peak > 0 ? 20.0 * std::log10(peak / 32768.0) : -std::numeric_limits<double>::infinity();
}

bool trimFile(const QString& path, double thresholdDb, int keepMs, QString* err) {
    Wav w;
    if (!readFile(path, &w, err) || !w.isPcm16() || !trim(w, thresholdDb, keepMs)) return false;
    return writeFile(path, w, err);
}

double normalizeFile(const QString& path, double targetDb, double ceilingDb, QString* err) {
    Wav w;
    if (!readFile(path, &w, err) || !w.isPcm16()) return 0.0;
    const double gain = normalize(w, targetDb, ceilingDb);
    if (gain == 0.0 || !writeFile(path, w, err)) return 0.0;
    return gain;
}

}   // namespace wavio
