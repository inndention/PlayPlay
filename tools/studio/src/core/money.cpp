#include "core/money.h"

#include <cmath>
#include <string>

namespace studio::money {

namespace {

constexpr quint64 kPow10[] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL, 100000000ULL,
                              1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL};

quint64 uabs(qint64 v) { return v < 0 ? 0ULL - quint64(v) : quint64(v); }   // also right for INT64_MIN

// JS Math.round(x * scale): the integral value nearest to x, ties towards +infinity. floor() + the exact fraction
// instead of floor(x + 0.5), which is wrong for 0.49999999999999994.
bool jsRoundScaled(double x, double scale, qint64* out) {
    if (!std::isfinite(x)) return false;
    const double y = x * scale;
    if (!std::isfinite(y) || std::fabs(y) >= 9223372036854775807.0) return false;   // that literal is 2^63 as a double
    double r = std::floor(y);
    if (y - r >= 0.5) r += 1.0;
    *out = qint64(r);
    return true;
}

// Exact decimal text → integer count of 10^-decimals units, rounded half away from zero.
bool parseScaled(QStringView text, int decimals, qint64* out, bool* exact) {
    const QStringView t = text.trimmed();
    qsizetype i = 0;
    const qsizetype n = t.size();
    bool neg = false, dollar = false;
    auto takeDollar = [&]() { if (i < n && t[i] == u'$' && !dollar) { dollar = true; ++i; } };
    takeDollar();
    if (i < n && (t[i] == u'-' || t[i] == u'+')) { neg = t[i] == u'-'; ++i; }
    takeDollar();
    std::string digits;                    // integer digits then fraction digits
    int fracDigits = 0;
    bool any = false;
    auto isDigit = [&](qsizetype k) { return k < n && t[k].unicode() >= u'0' && t[k].unicode() <= u'9'; };
    while (isDigit(i)) { digits += char(t[i].unicode()); ++i; any = true; }
    if (i < n && t[i] == u'.') {
        ++i;
        while (isDigit(i)) { digits += char(t[i].unicode()); ++fracDigits; ++i; any = true; }
    }
    if (!any) return false;
    long long exp10 = 0;
    if (i < n && (t[i] == u'e' || t[i] == u'E')) {
        ++i;
        bool eneg = false;
        if (i < n && (t[i] == u'-' || t[i] == u'+')) { eneg = t[i] == u'-'; ++i; }
        bool edig = false;
        while (isDigit(i)) {
            if (exp10 < 100000) exp10 = exp10 * 10 + (t[i].unicode() - u'0');
            ++i;
            edig = true;
        }
        if (!edig) return false;
        if (eneg) exp10 = -exp10;
    }
    if (i != n) return false;              // junk, thousands separators, a second "$" ...

    const size_t nz = digits.find_first_not_of('0');
    if (nz == std::string::npos) { *out = 0; if (exact) *exact = true; return true; }
    digits.erase(0, nz);
    const long long shift = exp10 - fracDigits + decimals;   // value = digits × 10^shift units
    bool isExact = true;
    if (shift >= 0) {
        if (shift > 19) return false;
        digits.append(size_t(shift), '0');
    } else {
        const long long drop = -shift;
        if (drop > (long long)digits.size()) {             // all digits fall below one unit: rounds to 0
            *out = 0;
            if (exact) *exact = false;
            return true;
        }
        const size_t keep = digits.size() - size_t(drop);
        const bool up = digits[keep] >= '5';
        isExact = digits.find_first_not_of('0', keep) == std::string::npos;
        digits.resize(keep);
        if (up) {                          // + 1 on the kept digits (as text, so 999.. carries correctly)
            int k = int(digits.size()) - 1;
            while (k >= 0 && digits[size_t(k)] == '9') { digits[size_t(k)] = '0'; --k; }
            if (k < 0) digits.insert(digits.begin(), '1');
            else ++digits[size_t(k)];
        }
        if (digits.empty()) digits = "0";
    }
    if (digits.size() > 19) return false;
    quint64 v = 0;
    for (char c : digits) {
        const quint64 d = quint64(c - '0');
        if (v > (quint64(kMaxMicro) - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = neg ? -qint64(v) : qint64(v);
    if (exact) *exact = isExact;
    return true;
}

} // namespace

Micro fromUsd(double usd, bool* ok) {
    qint64 v = 0;
    const bool good = jsRoundScaled(usd, 1e6, &v);   // 10 ** 6 === 1e6 exactly, so the product is the TS one
    if (ok) *ok = good;
    return good ? v : 0;
}

double toUsd(Micro m) { return double(m) / 1e6; }

bool parseUsd(QStringView text, Micro* out, bool* exact) { return parseScaled(text, 6, out, exact); }

QString formatFixed(Micro m, int decimals) {
    decimals = qBound(0, decimals, 6);
    const quint64 unit = kPow10[6 - decimals];
    const quint64 a = uabs(m);
    quint64 q = a / unit;
    const quint64 r = a % unit;
    if (r >= unit - r) ++q;                // half away from zero (on the magnitude)
    const quint64 scaleDiv = kPow10[decimals];
    QString s;
    if (m < 0 && q != 0) s += QLatin1Char('-');                 // "-0.00" is written "0.00"
    s += QString::number(q / scaleDiv);
    if (decimals > 0) s += QLatin1Char('.') + QString::number(q % scaleDiv).rightJustified(decimals, QLatin1Char('0'));
    return s;
}

namespace {
QString dollars(Micro m, int decimals) {
    QString s = formatFixed(m, decimals);
    return s.startsWith(QLatin1Char('-')) ? QStringLiteral("-$") + s.mid(1) : QLatin1Char('$') + s;
}
} // namespace

// The decimals follow the TS on the signed amount (usd(): 4 only for 0 < n < 0.01; fmtUsd(): 4 for anything under a
// cent, negatives included), so the digit count matches the TS text for every amount; only the sign position differs.
QString format(Micro m) { return dollars(m, m > 0 && m < 10000 ? 4 : 2); }

QString formatPlan(Micro m) { return dollars(m, m >= kMicroPerUsd ? 2 : m >= 10000 ? 3 : 4); }

QString toDecimal(Micro m) {
    QString s = formatFixed(m, 6);
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    return s;
}

Micro add(Micro a, Micro b, bool* overflow) {
    bool of = false;
    Micro r;
    if (b > 0 && a > kMaxMicro - b) { r = kMaxMicro; of = true; }
    else if (b < 0 && a < kMinMicro - b) { r = kMinMicro; of = true; }
    else r = a + b;
    if (r > kMaxMicro) { r = kMaxMicro; of = true; }
    if (r < kMinMicro) { r = kMinMicro; of = true; }
    if (overflow) *overflow = of;
    return r;
}

Micro sub(Micro a, Micro b, bool* overflow) {
    return add(a, b < kMinMicro ? kMaxMicro : -b, overflow);   // INT64_MIN is outside the money range: treated as kMinMicro
}

qint64 mulDiv(qint64 a, qint64 b, qint64 d, bool* overflow) {
    if (overflow) *overflow = false;
    if (d == 0) { if (overflow) *overflow = true; return 0; }
    const bool neg = ((a < 0) ^ (b < 0) ^ (d < 0)) != 0;
    const quint64 ua = uabs(a), ub = uabs(b), ud = uabs(d);
    // 128-bit product from 32-bit halves.
    const quint64 aLo = ua & 0xFFFFFFFFULL, aHi = ua >> 32, bLo = ub & 0xFFFFFFFFULL, bHi = ub >> 32;
    const quint64 p0 = aLo * bLo, p1 = aLo * bHi, p2 = aHi * bLo, p3 = aHi * bHi;
    const quint64 mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    const quint64 lo = (p0 & 0xFFFFFFFFULL) | (mid << 32);
    const quint64 hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    if (hi >= ud) { if (overflow) *overflow = true; return neg ? kMinMicro : kMaxMicro; }   // quotient >= 2^64
    // Restoring division of hi:lo by ud; hi < ud keeps the quotient within 64 bits.
    quint64 rem = hi, q = 0;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | ((lo >> i) & 1ULL);
        q <<= 1;
        if (carry || rem >= ud) { rem -= ud; q |= 1ULL; }
    }
    if (rem >= ud - rem) {                 // 2·rem >= ud: half away from zero
        if (q == ~0ULL) { if (overflow) *overflow = true; return neg ? kMinMicro : kMaxMicro; }
        ++q;
    }
    if (q > quint64(kMaxMicro)) { if (overflow) *overflow = true; return neg ? kMinMicro : kMaxMicro; }
    return neg ? -qint64(q) : qint64(q);
}

bool parseRate(QStringView usdPerUnit, Pico* out, bool* exact) { return parseScaled(usdPerUnit, 12, out, exact); }

Pico rateFromUsd(double usdPerUnit, bool* ok) {
    qint64 v = 0;
    const bool good = jsRoundScaled(usdPerUnit, 1e12, &v);
    if (ok) *ok = good;
    return good ? v : 0;
}

Micro cost(qint64 units, Pico ratePerUnit, bool* overflow) { return mulDiv(units, ratePerUnit, kPicoPerMicro, overflow); }

} // namespace studio::money
