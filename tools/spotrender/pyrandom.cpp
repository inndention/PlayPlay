#include "pyrandom.h"

#include <charconv>
#include <cmath>
#include <system_error>

// Constants and the two init routines are Modules/_randommodule.c (MT19937 by Matsumoto & Nishimura, as CPython ships it).
namespace {
constexpr int kM = 397;
constexpr quint32 kMatrixA = 0x9908b0dfu, kUpperMask = 0x80000000u, kLowerMask = 0x7fffffffu;

int bitLength(quint64 n) { int k = 0; while (n) { ++k; n >>= 1; } return k; }
}   // namespace

PyRandom::PyRandom(qint64 s) { seed(s); }
PyRandom::PyRandom(const std::vector<quint32>& magnitudeWords) { seedWords(magnitudeWords); }

std::vector<quint32> PyRandom::wordsFor(quint64 magnitude) {
    std::vector<quint32> w{quint32(magnitude)};
    if (magnitude >> 32) w.push_back(quint32(magnitude >> 32));
    return w;
}

void PyRandom::seed(qint64 s) {
    // abs() without overflow for INT64_MIN: the magnitude as unsigned
    const quint64 mag = s < 0 ? quint64(0) - quint64(s) : quint64(s);
    seedWords(wordsFor(mag));
}

void PyRandom::initGenrand(quint32 s) {
    m_mt[0] = s;
    for (int i = 1; i < N; ++i) m_mt[i] = 1812433253u * (m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) + quint32(i);
    m_index = N;
}

void PyRandom::seedWords(std::vector<quint32> key) {
    // random_seed(): keyused = the significant words (at least one). High zero words would change the state, so drop them.
    while (key.size() > 1 && key.back() == 0) key.pop_back();
    if (key.empty()) key.push_back(0);
    const size_t len = key.size();
    initGenrand(19650218u);
    size_t i = 1, j = 0;
    for (size_t k = (size_t(N) > len ? size_t(N) : len); k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1664525u)) + key[j] + quint32(j);   // non linear
        ++i; ++j;
        if (i >= size_t(N)) { m_mt[0] = m_mt[N - 1]; i = 1; }
        if (j >= len) j = 0;
    }
    for (size_t k = N - 1; k; --k) {
        m_mt[i] = (m_mt[i] ^ ((m_mt[i - 1] ^ (m_mt[i - 1] >> 30)) * 1566083941u)) - quint32(i);          // non linear
        ++i;
        if (i >= size_t(N)) { m_mt[0] = m_mt[N - 1]; i = 1; }
    }
    m_mt[0] = 0x80000000u;    // MSB is 1; assuring non-zero initial array
}

quint32 PyRandom::genrand() {
    static const quint32 mag01[2] = {0x0u, kMatrixA};
    if (m_index >= N) {       // generate N words at one time
        int kk = 0;
        for (; kk < N - kM; ++kk) {
            const quint32 y = (m_mt[kk] & kUpperMask) | (m_mt[kk + 1] & kLowerMask);
            m_mt[kk] = m_mt[kk + kM] ^ (y >> 1) ^ mag01[y & 0x1u];
        }
        for (; kk < N - 1; ++kk) {
            const quint32 y = (m_mt[kk] & kUpperMask) | (m_mt[kk + 1] & kLowerMask);
            m_mt[kk] = m_mt[kk + (kM - N)] ^ (y >> 1) ^ mag01[y & 0x1u];
        }
        const quint32 y = (m_mt[N - 1] & kUpperMask) | (m_mt[0] & kLowerMask);
        m_mt[N - 1] = m_mt[kM - 1] ^ (y >> 1) ^ mag01[y & 0x1u];
        m_index = 0;
    }
    quint32 y = m_mt[m_index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= (y >> 18);
    return y;
}

double PyRandom::random() {
    const quint32 a = genrand() >> 5, b = genrand() >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

quint64 PyRandom::getrandbits(int k) {
    if (k <= 0 || k > 64) return 0;
    if (k <= 32) return genrand() >> (32 - k);                     // fast path
    // words from least to most significant; only the last (partial) word drops its low bits
    const quint64 lo = genrand();
    quint64 hi = genrand();
    if (k < 64) hi >>= (64 - k);
    return lo | (hi << 32);
}

quint64 PyRandom::randbelow(quint64 n) {
    if (n == 0) return 0;
    const int k = bitLength(n);       // n.bit_length(), not (n - 1): n can be 1, and Python keeps it for reproducibility
    quint64 r = getrandbits(k);
    while (r >= n) r = getrandbits(k);
    return r;
}

double PyRandom::uniform(double a, double b) { return a + (b - a) * random(); }

bool PyRandom::parseInt(const QString& text, bool* negative, std::vector<quint32>* words, QString* err) {
    auto bad = [&](const QString& why) { if (err) *err = why; return false; };
    const QString s = text.trimmed();
    int i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; ++i; }
    std::vector<quint32> mag{0};
    int digits = 0;
    bool lastUnderscore = false;
    for (; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c == '_') {
            if (digits == 0 || lastUnderscore) return bad(QString("invalid literal for int() with base 10: '%1'").arg(text));
            lastUnderscore = true;
            continue;
        }
        const int d = c.digitValue();     // Unicode decimal digits count, as in Python
        if (d < 0 || !c.isDigit()) return bad(QString("invalid literal for int() with base 10: '%1'").arg(text));
        lastUnderscore = false;
        ++digits;
        quint64 carry = quint64(d);
        for (quint32& w : mag) { const quint64 v = quint64(w) * 10u + carry; w = quint32(v); carry = v >> 32; }
        if (carry) mag.push_back(quint32(carry));
    }
    if (digits == 0 || lastUnderscore) return bad(QString("invalid literal for int() with base 10: '%1'").arg(text));
    if (digits > 4300) return bad(QString("Exceeds the limit (4300 digits) for integer string conversion: value has %1 digits").arg(digits));
    while (mag.size() > 1 && mag.back() == 0) mag.pop_back();
    if (negative) *negative = neg && !(mag.size() == 1 && mag[0] == 0);
    if (words) *words = std::move(mag);
    return true;
}

double pyRound(double x) {
    // float.__round__(None): round() then fix exact halves to the even neighbour
    double r = std::round(x);
    if (std::fabs(x - r) == 0.5) r = 2.0 * std::round(x / 2.0);
    return r;
}

double pyRound(double x, int ndigits) {
    if (!std::isfinite(x) || ndigits < 0 || ndigits > 17) return x;
    // to_chars(fixed, precision) prints the exact binary value correctly rounded with ties to even (Ryu printf), which is
    // what CPython's _Py_dg_dtoa(x, 3, ndigits) produces; from_chars reads it back correctly rounded like _Py_dg_strtod.
    char buf[400];
    const auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, ndigits);
    if (res.ec != std::errc()) return x;
    double out = x;
    if (std::from_chars(buf, res.ptr, out).ec != std::errc()) return x;    // "-0.000" -> -0.0, as Python keeps the sign
    return out;
}
