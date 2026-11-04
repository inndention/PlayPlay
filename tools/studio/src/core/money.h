#pragma once
// Money for the Studio engine (ST-02; docs/plans/starmedia-studio.md §3 item 5 and §4.4): every amount is an int64
// count of micro-USD (1 USD = 1,000,000). The TS harness keeps dollars in doubles and rounds with round(n, 6); the
// ledger, budgets, caps and reservations here never touch a double, so sums cannot drift and a cap check is exact.
//
// Doubles appear only at the edges:
//   - fromUsd(): a USD number read from JSON (provider `usage.cost`, the MCP ledger's `usd`, a Jobs API cost) becomes
//     micro-USD exactly as the TS rounds it (Math.round(n * 1e6)), so both sides book the same amount;
//   - toUsd(): micro-USD written into such a JSON field.
// Decimal text (config values, OpenRouter's string prices) is parsed exactly, without a double in between.
//
// Unit prices are finer than a micro-dollar (OpenRouter lists $/token such as "0.0000003" = 0.3 micro-USD), so rates
// are pico-USD (1e-12) per unit and cost() turns quantity × rate into micro-USD with a 128-bit intermediate.
//
// Rounding: everything here that drops digits rounds half away from zero on the exact decimal value. JS toFixed rounds
// the binary double instead, so for an exact tie ($1.005 → JS "$1.00", here "$1.01") the display differs; every other
// value formats exactly like the TS (tested against TS golden values). Negative amounts keep the TS's choice of
// decimals (format(): 2, as usd() uses 4 only for 0 < n < 0.01; formatPlan(): 4, as fmtUsd() does for anything under a
// cent) but put the sign first: "-$0.50" (TS "$-0.50"), and one that rounds to zero has no sign: "$0.00" (TS "$-0.00").
#include <QString>
#include <QStringView>
#include <limits>

namespace studio::money {

using Micro = qint64;                      // micro-USD
using Pico = qint64;                       // pico-USD (unit prices)
constexpr Micro kMicroPerUsd = 1000000;
constexpr Pico kPicoPerMicro = 1000000;
constexpr Micro kMaxMicro = std::numeric_limits<qint64>::max();
constexpr Micro kMinMicro = -kMaxMicro;    // symmetric, so negation never overflows

constexpr Micro usd(qint64 dollars) { return dollars * kMicroPerUsd; }
constexpr Micro cents(qint64 c) { return c * 10000; }

/// Math.round(usd * 1e6) (the TS round(n, 6) before its division): half-micro ties go towards +infinity, like JS. NaN,
/// ±inf and amounts beyond int64 are refused (0, ok=false): money from untrusted input never saturates silently.
Micro fromUsd(double usd, bool* ok = nullptr);
/// micro / 1e6 as the closest double (for JSON fields that carry USD numbers).
double toUsd(Micro m);

/// Exact decimal text → micro-USD. Accepts surrounding spaces, a sign, a "$" (before or after the sign), digits with
/// an optional fraction and an optional exponent: "0.25", "$1.50", "-$3", "1e-3", ".5", "2.". More than 6 decimals
/// round half away from zero (exact=false). Refuses empty text, junk, thousands separators and overflow (false).
bool parseUsd(QStringView text, Micro* out, bool* exact = nullptr);

/// The MCP spend gate's usd() (mcp/core/util.ts): "$0.0050" for amounts under one cent (but above zero), else "$1.23"
/// (negatives too: "-$0.50", "$0.00" for -$0.004).
QString format(Micro m);
/// The harness pricing fmtUsd() (src/pricing.ts): "$12.34" from $1, "$0.123" from one cent, else "$0.0012" (negatives
/// too: "-$1.5000").
QString formatPlan(Micro m);
/// Fixed decimals (0..6), no "$": formatFixed(1234567, 2) == "1.23".
QString formatFixed(Micro m, int decimals);
/// Exact value, trailing zeros trimmed: "0.25", "1", "-0.000001". Round-trips through parseUsd().
QString toDecimal(Micro m);

/// Saturating arithmetic: the result is clamped to [kMinMicro, kMaxMicro] and `overflow` set when it had to be.
Micro add(Micro a, Micro b, bool* overflow = nullptr);
Micro sub(Micro a, Micro b, bool* overflow = nullptr);
/// round(a * b / d), half away from zero, with a 128-bit intermediate (d != 0; d == 0 → 0 and overflow).
qint64 mulDiv(qint64 a, qint64 b, qint64 d, bool* overflow = nullptr);
/// m * num / den rounded: the budget ceiling is scale(budget, 115, 100) (executor.ts ×1.15).
inline Micro scale(Micro m, qint64 num, qint64 den, bool* overflow = nullptr) { return mulDiv(m, num, den, overflow); }

/// A USD-per-unit price as text ("0.0000003", "$0.04", "1e-7") → pico-USD per unit, exactly (rounded half away from
/// zero beyond 12 decimals, exact=false).
bool parseRate(QStringView usdPerUnit, Pico* out, bool* exact = nullptr);
/// Math.round(usdPerUnit * 1e12) for a price that arrives as a JSON number.
Pico rateFromUsd(double usdPerUnit, bool* ok = nullptr);
/// units × rate in micro-USD, rounded half away from zero (saturating; `overflow` set when clamped).
Micro cost(qint64 units, Pico ratePerUnit, bool* overflow = nullptr);

} // namespace studio::money
