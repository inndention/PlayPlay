#include "provider/backpressure.h"

#include "core/clock.h"
#include "core/datafiles.h"
#include "core/json.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace studio::provider {

// ---------------------------------------------------------------------------------------------------------------
// The TS expressions, verbatim (src/backpressure.ts). Do not reformat: test_studio_backpressure finds each of these
// texts in the checkout's backpressure.ts when it is on disk.
namespace rx {
const char16_t* const kQuota = uR"RX(per[\s-]?day\b|tokens? per day|requests? per day|\b(?:tpd|rpd)\b|daily (?:token |request |usage |free )?(?:quota|limit|cap|allowance|usage)|free-models-per-day|(?:quota|credits?|allowance)\b[^.]{0,60}\b(?:exhausted|exceeded|used up|depleted)|exceeded your (?:current )?quota|insufficient[_ ]quota|out of (?:free )?(?:credits|quota))RX";
const char16_t* const kCap = uR"RX(daily spend cap|spend(?:ing)? cap|budget cap|spend cap reached|raise it in settings)RX";
const char16_t* const kRate = uR"RX(rate[\s-]?limit|too many requests|requests? per (?:minute|second)|\b(?:rpm|tpm)\b|throttl|slow down|resource has been exhausted|overloaded)RX";
const char16_t* const kShortWindow = uR"RX(\bper[\s_-]*(?:minute|second|hour)\b)RX";
const char16_t* const kDailyWords = uR"RX(per[\s-]?day\b|\bdaily\b|\b(?:tpd|rpd)\b|free-models-per-day)RX";
const char16_t* const kRetryText = uR"RX((?:try again|retry|wait|resets?)\s*(?:after|in)?\s*(?:about\s*)?((?:\d+(?:\.\d+)?\s*(?:ms|hours?|hrs?|h|minutes?|mins?|m|seconds?|secs?|s)\s*)+))RX";
const char16_t* const kDurationPart = uR"RX((\d+(?:\.\d+)?)\s*(ms|seconds?|secs?|s|minutes?|mins?|m|hours?|hrs?|h)(?![a-z]))RX";

namespace {
QRegularExpression compile(const char16_t* src, QStringView flags) {
    QString why;
    QRegularExpression re = data::regex(QStringView(src), flags, &why);
    Q_ASSERT_X(re.isValid(), "backpressure", qPrintable(why));
    return re;
}
const QRegularExpression& shortWindow() { static const QRegularExpression re = compile(kShortWindow, u"i"); return re; }
const QRegularExpression& dailyWords() { static const QRegularExpression re = compile(kDailyWords, u"i"); return re; }
const QRegularExpression& retryText() { static const QRegularExpression re = compile(kRetryText, u"i"); return re; }
const QRegularExpression& durationPart() { static const QRegularExpression re = compile(kDurationPart, u"g"); return re; }
const QRegularExpression& numeric() { static const QRegularExpression re = compile(uR"RX(^\d+(?:\.\d+)?$)RX", u""); return re; }
const QRegularExpression& anyDigit() { static const QRegularExpression re = compile(uR"RX(\d)RX", u""); return re; }
} // namespace

const QRegularExpression& quota() { static const QRegularExpression re = compile(kQuota, u"i"); return re; }
const QRegularExpression& cap() { static const QRegularExpression re = compile(kCap, u"i"); return re; }
const QRegularExpression& rate() { static const QRegularExpression re = compile(kRate, u"i"); return re; }
bool test(const QRegularExpression& re, QStringView text) { return re.isValid() && re.matchView(text).hasMatch(); }
} // namespace rx

QString limitKindName(LimitKind k) {
    switch (k) {
    case LimitKind::Rate: return QStringLiteral("rate");
    case LimitKind::Quota: return QStringLiteral("quota");
    case LimitKind::Cap: return QStringLiteral("cap");
    }
    return QStringLiteral("rate");
}

std::optional<LimitKind> limitKindFromName(QStringView s) {
    if (s == u"rate") return LimitKind::Rate;
    if (s == u"quota") return LimitKind::Quota;
    if (s == u"cap") return LimitKind::Cap;
    return std::nullopt;
}

namespace {

// ECMAScript WhiteSpace + LineTerminator (String.prototype.trim and \s). QChar::isSpace differs (U+0085 yes, U+FEFF no).
bool jsSpace(char16_t c) {
    return c == 9 || c == 10 || c == 11 || c == 12 || c == 13 || c == 32 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000
        || c == 0xFEFF;
}

QString jsTrim(QStringView s) {
    qsizetype a = 0, b = s.size();
    while (a < b && jsSpace(s[a].unicode())) ++a;
    while (b > a && jsSpace(s[b - 1].unicode())) --b;
    return s.mid(a, b - a).toString();
}

// `.replace(/\s+/g, ' ').trim()`
QString jsCollapse(QStringView s) {
    QString out;
    out.reserve(s.size());
    bool inSpace = false;
    for (QChar c : s) {
        if (jsSpace(c.unicode())) { inSpace = true; continue; }
        if (inSpace && !out.isEmpty()) out += QLatin1Char(' ');
        inSpace = false;
        out += c;
    }
    return out;
}

// Number(s) for text the numeric expressions already accepted (ASCII digits and one dot); overflow is +Infinity as in JS.
double jsNumberOf(QStringView s) {
    const QByteArray a = s.toLatin1();
    return std::strtod(a.constData(), nullptr);
}

// Math.round
double jsRound(double x) { return std::floor(x + 0.5); }

qint64 toMs(double x) {
    if (!(x == x)) return 0;
    if (x >= 9.2e18) return std::numeric_limits<qint64>::max();
    if (x <= -9.2e18) return std::numeric_limits<qint64>::min();
    return static_cast<qint64>(jsRound(x));
}

double capHint(double x) { return std::min<double>(double(kMaxHintMs), x); }

QJsonValue member(const QJsonValue& v, QLatin1StringView key) {
    return v.isObject() ? v.toObject().value(key) : QJsonValue(QJsonValue::Undefined);
}

// String(v) of a JSON value
QString jsStringOf(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::String: return v.toString();
    case QJsonValue::Double: return json::jsNumber(v.toDouble());
    case QJsonValue::Bool: return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Object: return QStringLiteral("[object Object]");
    case QJsonValue::Array: {
        QStringList parts;
        for (const QJsonValue& x : v.toArray()) parts << (x.isNull() || x.isUndefined() ? QString() : jsStringOf(x));
        return parts.join(QLatin1Char(','));
    }
    case QJsonValue::Undefined: return QStringLiteral("undefined");
    }
    return {};
}

// Headers.get(name): every value of the header joined with ", "; a null QString when absent
QString headerOf(const HeaderList& h, QLatin1StringView name) {
    QString out;
    bool any = false;
    for (const auto& kv : h) {
        if (QLatin1StringView(kv.first).compare(name, Qt::CaseInsensitive) != 0) continue;
        if (any) out += QStringLiteral(", ");
        out += QString::fromLatin1(kv.second);
        any = true;
    }
    return any ? out : QString();
}

// a plain object bag (error.metadata.headers): the first key whose lower case is `name`
QString headerOf(const QJsonObject& h, QLatin1StringView name) {
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (it.key().toLower() != name) continue;
        const QJsonValue v = it.value();
        if (v.isNull() || v.isUndefined()) return {};
        return jsStringOf(v);
    }
    return {};
}

// Date.parse for the shapes a provider sends: IMF-fixdate, RFC 850, asctime (RFC 9110 §5.6.7), RFC 2822 with a
// numeric zone, ISO 8601. nullopt = NaN.
std::optional<qint64> jsDateParse(const QString& t) {
    static const QRegularExpression imf(
        QStringLiteral("^(?:[A-Za-z]{3,9},\\s*)?(\\d{1,2})[ -]([A-Za-z]{3})[ -](\\d{2,4})\\s+(\\d{1,2}):(\\d{2})(?::(\\d{2}))?\\s*(?:GMT|UTC|UT|Z)?$"));
    static const QRegularExpression asc(
        QStringLiteral("^[A-Za-z]{3}\\s+([A-Za-z]{3})\\s+(\\d{1,2})\\s+(\\d{1,2}):(\\d{2}):(\\d{2})\\s+(\\d{4})$"));
    static const QStringList months = QStringLiteral("jan feb mar apr may jun jul aug sep oct nov dec").split(QLatin1Char(' '));
    auto make = [](int y, int mo, int d, int h, int mi, int s) -> std::optional<qint64> {
        const QDate date(y, mo, d);
        const QTime time(h, mi, s);
        if (!date.isValid() || !time.isValid()) return std::nullopt;
        return QDateTime(date, time, QTimeZone::UTC).toMSecsSinceEpoch();
    };
    if (const auto m = imf.match(t); m.hasMatch()) {
        const int mo = int(months.indexOf(m.captured(2).toLower())) + 1;
        int y = m.captured(3).toInt();
        if (m.captured(3).size() == 2) y += y < 50 ? 2000 : 1900;
        if (mo > 0) return make(y, mo, m.captured(1).toInt(), m.captured(4).toInt(), m.captured(5).toInt(),
                                m.captured(6).isEmpty() ? 0 : m.captured(6).toInt());
    }
    if (const auto m = asc.match(t); m.hasMatch()) {
        const int mo = int(months.indexOf(m.captured(1).toLower())) + 1;
        if (mo > 0) return make(m.captured(6).toInt(), mo, m.captured(2).toInt(), m.captured(3).toInt(),
                                m.captured(4).toInt(), m.captured(5).toInt());
    }
    QDateTime d = QDateTime::fromString(t, Qt::ISODateWithMs);
    if (!d.isValid()) d = QDateTime::fromString(t, Qt::RFC2822Date);
    if (d.isValid()) return d.toMSecsSinceEpoch();
    return std::nullopt;
}

// a reset value that may be a duration ("6m0s", "20"), an epoch in seconds or in milliseconds, or an HTTP date
std::optional<qint64> resetToMs(QStringView v, qint64 now) {
    const QString t = jsTrim(v);
    if (rx::test(rx::numeric(), t)) {
        const double n = jsNumberOf(t);
        if (n >= 1e12) return toMs(std::max(0.0, capHint(n - double(now))));          // epoch ms
        if (n >= 1e9) return toMs(std::max(0.0, capHint(n * 1000 - double(now))));    // epoch s
        return toMs(capHint(jsRound(n * 1000)));                                      // seconds
    }
    if (const auto d = parseDurationMs(t)) return d;
    if (const auto date = jsDateParse(t)) return toMs(std::max(0.0, capHint(double(*date - now))));
    return std::nullopt;
}

int unitMs(QStringView u) {
    if (u == u"ms") return 1;
    if (u == u"s" || u == u"sec" || u == u"secs" || u == u"second" || u == u"seconds") return 1000;
    if (u == u"m" || u == u"min" || u == u"mins" || u == u"minute" || u == u"minutes") return 60000;
    if (u == u"h" || u == u"hr" || u == u"hrs" || u == u"hour" || u == u"hours") return 3600000;
    return 0;
}

} // namespace

std::optional<qint64> parseDurationMs(QStringView text) {
    const QString s = jsTrim(text).toLower();
    if (s.isEmpty()) return std::nullopt;
    if (rx::test(rx::numeric(), s)) return toMs(capHint(jsRound(jsNumberOf(s) * 1000)));
    double total = 0;
    bool any = false;
    QString rest = s;
    auto it = rx::durationPart().globalMatchView(s);
    while (it.hasNext()) {
        const auto m = it.next();
        const int f = unitMs(m.capturedView(2));
        if (!f) continue;
        total += jsNumberOf(m.capturedView(1)) * f;
        any = true;
        const qsizetype at = rest.indexOf(m.capturedView(0));                         // String.replace: first only
        if (at >= 0) rest.remove(at, m.capturedLength(0));
    }
    if (any && !rx::test(rx::anyDigit(), rest)) return toMs(capHint(jsRound(total)));
    return std::nullopt;
}

std::optional<qint64> retryAfterMs(const HeaderList& headers, const QJsonValue& body, QStringView text, qint64 nowMs) {
    const QString ms = headerOf(headers, QLatin1StringView("retry-after-ms"));
    if (!ms.isEmpty() && rx::test(rx::numeric(), jsTrim(ms)))
        return toMs(capHint(jsRound(jsNumberOf(jsTrim(ms)))));
    const QString ra = headerOf(headers, QLatin1StringView("retry-after"));
    if (!ra.isEmpty()) {
        if (const auto v = resetToMs(ra, nowMs)) return v;
    }
    static const QLatin1StringView names[] = {QLatin1StringView("retry-after"), QLatin1StringView("x-ratelimit-reset"),
                                              QLatin1StringView("x-ratelimit-reset-requests"),
                                              QLatin1StringView("x-ratelimit-reset-tokens")};
    for (QLatin1StringView name : names) {
        const QString v = headerOf(headers, name);
        if (v.isEmpty()) continue;
        if (const auto r = resetToMs(v, nowMs)) return r;
    }
    const QJsonValue meta = member(member(member(body, QLatin1StringView("error")), QLatin1StringView("metadata")),
                                   QLatin1StringView("headers"));
    if (meta.isObject()) {
        const QJsonObject bag = meta.toObject();
        for (QLatin1StringView name : names) {
            const QString v = headerOf(bag, name);
            if (v.isEmpty()) continue;
            if (const auto r = resetToMs(v, nowMs)) return r;
        }
    }
    const auto m = rx::retryText().matchView(text);
    return m.hasMatch() ? parseDurationMs(m.capturedView(1)) : std::nullopt;
}

std::optional<LimitSignal> classifyLimit(int status, const HeaderList& headers, const QJsonValue& body, QStringView text,
                                         qint64 nowMs) {
    const QJsonValue error = member(body, QLatin1StringView("error"));
    const QJsonValue md = member(error, QLatin1StringView("metadata"));
    QString provider;
    if (const QJsonValue pn = member(md, QLatin1StringView("provider_name")); pn.isString() && !jsTrim(pn.toString()).isEmpty())
        provider = jsTrim(pn.toString()).left(60);
    QString raw;
    if (const QJsonValue r = member(md, QLatin1StringView("raw")); r.isString()) raw = r.toString();
    else if (r.isObject()) raw = QString::fromUtf8(QJsonDocument(r.toObject()).toJson(QJsonDocument::Compact));
    else if (r.isArray()) raw = QString::fromUtf8(QJsonDocument(r.toArray()).toJson(QJsonDocument::Compact));
    const QJsonValue message = member(error, QLatin1StringView("message"));
    const QString msgText = message.isUndefined() || message.isNull() ? QString() : jsStringOf(message);
    // what the provider said, one line: its error message and the upstream text OpenRouter wraps; the raw answer only
    // when there is nothing else
    QString said = jsCollapse(msgText + QLatin1Char(' ') + raw);
    if (said.isEmpty()) said = jsCollapse(text);
    const QString words = said + QLatin1Char(' ') + text;
    QString msg = said.size() > 300 ? said.left(299) + QChar(0x2026) : said;
    if (msg.isEmpty()) msg = QStringLiteral("HTTP %1").arg(status);
    const bool withWords = rx::test(rx::rate(), words) || rx::test(rx::quota(), words) || rx::test(rx::cap(), words);
    const bool daily = rx::test(rx::quota(), words)
        && !(rx::test(rx::shortWindow(), words) && !rx::test(rx::dailyWords(), words));
    static const int wordy[] = {400, 402, 403, 500, 502, 503, 504};
    const bool wordyStatus = std::find(std::begin(wordy), std::end(wordy), status) != std::end(wordy);
    if (status != 429 && !(withWords && wordyStatus)) return std::nullopt;
    // 402 is "out of credits", which is a quota answer only when it says so; a plain 402 stays a payment error
    if (status == 402 && !daily) return std::nullopt;
    const auto retry = retryAfterMs(headers, body, words, nowMs);
    LimitSignal s;
    s.provider = provider;
    s.message = msg;
    if (rx::test(rx::cap(), words)) { s.kind = LimitKind::Cap; return s; }
    if (daily) { s.kind = LimitKind::Quota; s.retryAfterMs = retry; return s; }
    if (status != 429 && !rx::test(rx::rate(), words)) return std::nullopt;
    s.kind = LimitKind::Rate;
    s.retryAfterMs = retry;
    return s;
}

qint64 retryDelayMs(int attempt, std::optional<qint64> hintMs, const std::function<double()>& rand) {
    const double base = hintMs && *hintMs >= 0 ? double(*hintMs) : 1000.0 * std::pow(2.0, std::min(attempt, 6));
    const double r = rand ? rand() : QRandomGenerator::global()->generateDouble();
    return toMs(base + r * std::max(250.0, base * 0.25));
}

// ---------------------------------------------------------------------------------------------------------------

QJsonObject AdaptiveStatus::toJson() const {
    QJsonObject o{{QStringLiteral("name"), name},
                  {QStringLiteral("configured"), configured},
                  {QStringLiteral("effective"), effective},
                  {QStringLiteral("limited"), limited},
                  {QStringLiteral("paused"), paused},
                  {QStringLiteral("pausedForMs"), double(pausedForMs)},
                  {QStringLiteral("cuts"), cuts},
                  {QStringLiteral("rateAnswers"), rateAnswers}};
    if (lastSignal) {
        QJsonObject s{{QStringLiteral("kind"), limitKindName(lastSignal->kind)},
                      {QStringLiteral("message"), lastSignal->message},
                      {QStringLiteral("at"), double(lastSignalAtMs)}};
        if (!lastSignal->provider.isEmpty()) s.insert(QStringLiteral("provider"), lastSignal->provider);
        if (lastSignal->retryAfterMs) s.insert(QStringLiteral("retryAfterMs"), double(*lastSignal->retryAfterMs));
        o.insert(QStringLiteral("lastSignal"), s);
    }
    return o;
}

AdaptiveLimit::AdaptiveLimit(QString name, std::function<int()> max, Clock& clock, int floor, AdaptiveTuning tuning)
    : m_name(std::move(name)), m_max(std::move(max)), m_clock(clock), m_floor(std::max(1, floor)),
      m_tuning(tuning), m_holdUntil(std::numeric_limits<qint64>::min()) {}

qint64 AdaptiveLimit::now() const { return m_clock.steadyMs(); }
int AdaptiveLimit::configuredMax() const { return m_max ? m_max() : 1; }

// additive increase by time: one slot back per recoverMs without a rate answer
void AdaptiveLimit::creep() {
    if (!m_cur) return;
    const qint64 t = now();
    const qint64 recover = std::max<qint64>(1, m_tuning.recoverMs.value_or(20000));
    while (m_cur && t - m_changedAt >= recover && t >= m_pausedUntil) {
        m_cur = *m_cur + 1;
        m_changedAt += recover;
        m_wins = 0;
        if (*m_cur >= configuredMax()) m_cur.reset();
    }
}

int AdaptiveLimit::limit() {
    const int max = std::max(1, configuredMax());
    creep();
    return !m_cur ? max : std::max(m_floor, std::min(*m_cur, max));
}

bool AdaptiveLimit::paused() const { return now() < m_pausedUntil; }
qint64 AdaptiveLimit::pausedForMs() const { return std::max<qint64>(0, m_pausedUntil - now()); }

std::optional<qint64> AdaptiveLimit::nextStepMs() const {
    if (!m_cur) return std::nullopt;
    const qint64 recover = std::max<qint64>(1, m_tuning.recoverMs.value_or(20000));
    return std::max({pausedForMs(), m_changedAt + recover - now(), qint64(0)});
}

AdaptiveLimit::LimitResult AdaptiveLimit::onLimit(const LimitSignal& sig) {
    const qint64 t = now();
    m_lastSignal = sig;
    m_lastSignalAtMs = m_clock.nowMs();
    if (sig.kind != LimitKind::Rate) return {};
    ++m_rateAnswers;
    const qint64 maxPause = m_tuning.maxPauseMs.value_or(kMaxInlineWaitMs);
    const qint64 pauseMs = std::min(maxPause, std::max<qint64>(0, sig.retryAfterMs.value_or(0)));
    if (pauseMs > 0) m_pausedUntil = std::max(m_pausedUntil, t + pauseMs);
    bool cut = false;
    if (t >= m_holdUntil) {
        const int cur = limit();
        m_cur = std::max(m_floor, cur / 2);
        ++m_cuts;
        m_changedAt = t;
        m_wins = 0;
        cut = true;
        m_holdUntil = t + m_tuning.holdMs.value_or(std::max<qint64>(1500, pauseMs));
    } else if (pauseMs > 0) {
        m_holdUntil = std::max(m_holdUntil, t + pauseMs);
    }
    if (m_cur && *m_cur >= configuredMax()) m_cur.reset();
    return {cut, pauseMs};
}

void AdaptiveLimit::onSuccess() {
    if (!m_cur) return;
    const qint64 t = now();
    if (t < m_holdUntil || t < m_pausedUntil) return;   // stragglers in flight when the limit was cut
    creep();
    if (!m_cur) return;
    if (++m_wins >= *m_cur) {
        m_cur = *m_cur + 1;
        m_wins = 0;
        m_changedAt = t;
        if (*m_cur >= configuredMax()) m_cur.reset();
    }
}

void AdaptiveLimit::reset() {
    m_cur.reset();
    m_pausedUntil = 0;
    m_holdUntil = std::numeric_limits<qint64>::min();
    m_wins = 0;
}

void AdaptiveLimit::resetStats() {
    m_cuts = 0;
    m_rateAnswers = 0;
    m_lastSignal.reset();
    m_lastSignalAtMs = 0;
}

AdaptiveStatus AdaptiveLimit::status() {
    AdaptiveStatus s;
    s.name = m_name;
    s.configured = std::max(1, configuredMax());
    s.effective = limit();
    s.paused = paused();
    s.limited = s.effective < s.configured || s.paused;
    s.pausedForMs = pausedForMs();
    s.cuts = m_cuts;
    s.rateAnswers = m_rateAnswers;
    s.lastSignal = m_lastSignal;
    s.lastSignalAtMs = m_lastSignalAtMs;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------

QJsonObject QuotaBreaker::Entry::toJson() const {
    QJsonObject o{{QStringLiteral("key"), key},
                  {QStringLiteral("kind"), limitKindName(kind)},
                  {QStringLiteral("until"), double(untilMs)},
                  {QStringLiteral("message"), message}};
    if (!provider.isEmpty()) o.insert(QStringLiteral("provider"), provider);
    return o;
}

QuotaBreaker::QuotaBreaker(Clock& clock, int max) : m_clock(clock), m_max(std::max(1, max)) {}

void QuotaBreaker::trip(const QString& key, const LimitSignal& sig, qint64 ttlMs) {
    if (key.isEmpty()) return;
    const qint64 ttl = std::max<qint64>(200, ttlMs);
    Entry e{key, sig.kind, m_clock.steadyMs() + ttl, m_clock.nowMs() + ttl, sig.message, sig.provider};
    std::lock_guard lock(m_mutex);
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& x) { return x.key == key; });
    if (it != m_entries.end()) { *it = std::move(e); return; }     // a JS Map keeps the key's place
    if (int(m_entries.size()) >= m_max) m_entries.erase(m_entries.begin());
    m_entries.push_back(std::move(e));
}

std::optional<QuotaBreaker::Entry> QuotaBreaker::check(const QString& key) {
    if (key.isEmpty()) return std::nullopt;
    const qint64 now = m_clock.steadyMs();
    std::lock_guard lock(m_mutex);
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& x) { return x.key == key; });
    if (it == m_entries.end()) return std::nullopt;
    if (now >= it->untilSteadyMs) { m_entries.erase(it); return std::nullopt; }
    return *it;
}

void QuotaBreaker::clear(const QString& key) {
    std::lock_guard lock(m_mutex);
    if (key.isEmpty()) { m_entries.clear(); return; }
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(), [&](const Entry& x) { return x.key == key; }),
                    m_entries.end());
}

std::vector<QuotaBreaker::Entry> QuotaBreaker::list() const {
    const qint64 now = m_clock.steadyMs();
    std::lock_guard lock(m_mutex);
    std::vector<Entry> out;
    for (const Entry& e : m_entries)
        if (e.untilSteadyMs > now) out.push_back(e);
    return out;
}

int QuotaBreaker::size() const {
    std::lock_guard lock(m_mutex);
    return int(m_entries.size());
}

} // namespace studio::provider
