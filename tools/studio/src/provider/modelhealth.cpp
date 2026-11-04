#include "provider/modelhealth.h"

#include "core/clock.h"
#include "core/datafiles.h"

#include <QDateTime>
#include <QLocale>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::Literals::StringLiterals;

namespace studio::provider {

namespace {

// The TS expressions of src/model-health.ts, verbatim (flags in the comment of each).
const char16_t* const kAccount = uR"RX(insufficient (credits?|funds|balance)|credit limit|billing|spend(ing)? limit|free[\s-]tier.*(limit|exceed)|key limit)RX";   // i
const char16_t* const kContext = uR"RX(context[\s_-]*(length|window|limit)|maximum (context|input)|too (long|large)|prompt is too long|input (is )?too|exceeds? (the )?(model'?s? )?(max|maximum|context|limit)|reduce the length|token limit|tokens? (exceed|over)|payload too large|request too large|\b413\b)RX";   // i
const char16_t* const kRateWords = uR"RX(rate[\s_-]*limit|too many requests|slow down|throttl|try again (in|after)|retry[\s_-]*after|\b429\b)RX";   // i
const char16_t* const kLooseWait = uR"RX((?:in|after|retry[\s_-]*after[:=]?)\s*(\d+(?:\.\d+)?)\s*(ms|milliseconds?|s|sec|secs|seconds?|m|min|mins|minutes?|h|hours?)\b)RX";   // i
const char16_t* const kLeadingStatus = uR"RX(^\s*(\d{3})\b)RX";
const char16_t* const kCancelled = uR"RX(\b(aborted|cancelled|canceled|stopped)\b)RX";   // i
const char16_t* const kGone = uR"RX(no endpoints? found|model (is )?(not found|unavailable|no longer)|not a valid model|does not exist)RX";   // i
const char16_t* const kJson = uR"RX(did not return valid json|invalid json|not valid json|unexpected token|json (parse|at position))RX";   // i
const char16_t* const kServerWords = uR"RX(timed out|timeout|econnreset|econnrefused|fetch failed|socket hang up|network|overloaded|service unavailable|bad gateway)RX";   // i

QRegularExpression compile(const char16_t* src, QStringView flags) {
    QString why;
    QRegularExpression re = data::regex(QStringView(src), flags, &why);
    Q_ASSERT_X(re.isValid(), "modelhealth", qPrintable(why));
    return re;
}
#define STUDIO_RX(fn, src, flags) \
    const QRegularExpression& fn() { static const QRegularExpression re = compile(src, flags); return re; }
STUDIO_RX(accountRx, kAccount, u"i")
STUDIO_RX(contextRx, kContext, u"i")
STUDIO_RX(rateWordsRx, kRateWords, u"i")
STUDIO_RX(looseWaitRx, kLooseWait, u"i")
STUDIO_RX(leadingStatusRx, kLeadingStatus, u"")
STUDIO_RX(cancelledRx, kCancelled, u"i")
STUDIO_RX(goneRx, kGone, u"i")
STUDIO_RX(jsonRx, kJson, u"i")
STUDIO_RX(serverWordsRx, kServerWords, u"i")
#undef STUDIO_RX

bool has(const QRegularExpression& re, QStringView s) { return rx::test(re, s); }

// the words for a model that rests, by kind: what a saved entry is shown as is always one of these
QString whyOf(ErrorKind k) {
    switch (k) {
    case ErrorKind::Quota: return QStringLiteral("out of quota");
    case ErrorKind::Rate: return QStringLiteral("rate limited");
    case ErrorKind::Server: return QStringLiteral("not answering right now");
    case ErrorKind::Gone: return QStringLiteral("not available");
    default: return {};
    }
}

double jsNumberOfValue(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Double: return v.toDouble();
    case QJsonValue::Bool: return v.toBool() ? 1 : 0;
    case QJsonValue::Null: return 0;
    case QJsonValue::String: {
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) return 0;
        bool ok = false;
        const double d = s.toDouble(&ok);
        return ok ? d : std::nan("");
    }
    default: return std::nan("");
    }
}

Classified make(ErrorKind k, std::optional<int> status, bool sw, qint64 cd, const QString& why,
                std::optional<qint64> retry = std::nullopt) {
    Classified c;
    c.kind = k;
    c.status = status;
    c.switchModel = sw;
    c.cooldownMs = cd;
    c.why = why;
    c.retryAfterMs = retry;
    return c;
}

} // namespace

QString errorKindName(ErrorKind k) {
    switch (k) {
    case ErrorKind::Quota: return QStringLiteral("quota");
    case ErrorKind::Rate: return QStringLiteral("rate");
    case ErrorKind::Server: return QStringLiteral("server");
    case ErrorKind::Gone: return QStringLiteral("gone");
    case ErrorKind::Context: return QStringLiteral("context");
    case ErrorKind::Json: return QStringLiteral("json");
    case ErrorKind::Auth: return QStringLiteral("auth");
    case ErrorKind::Cap: return QStringLiteral("cap");
    case ErrorKind::Cancelled: return QStringLiteral("cancelled");
    case ErrorKind::Other: return QStringLiteral("other");
    }
    return QStringLiteral("other");
}

std::optional<ErrorKind> errorKindFromName(QStringView s) {
    for (ErrorKind k : {ErrorKind::Quota, ErrorKind::Rate, ErrorKind::Server, ErrorKind::Gone, ErrorKind::Context,
                        ErrorKind::Json, ErrorKind::Auth, ErrorKind::Cap, ErrorKind::Cancelled, ErrorKind::Other})
        if (s == errorKindName(k)) return k;
    return std::nullopt;
}

QList<QPair<QString, QString>> errorPatterns() {
    const auto s = [](const char16_t* p) { return QString::fromUtf16(p); };
    return {{s(kAccount), u"i"_s}, {s(kContext), u"i"_s}, {s(kRateWords), u"i"_s}, {s(kLooseWait), u"i"_s},
            {s(kLeadingStatus), QString()}, {s(kCancelled), u"i"_s}, {s(kGone), u"i"_s}, {s(kJson), u"i"_s},
            {s(kServerWords), u"i"_s}};
}

std::optional<qint64> retryAfterFromText(QStringView text) {
    if (const auto r = retryAfterMs({}, QJsonValue(QJsonValue::Undefined), text, Clock::system().nowMs())) return r;
    const auto m = looseWaitRx().matchView(text);
    if (!m.hasMatch()) return std::nullopt;
    const double n = m.capturedView(1).toString().toDouble();
    const QString u = m.captured(2).toLower();
    const double ms = u.startsWith(QLatin1String("ms")) || u.startsWith(QLatin1String("milli")) ? n
        : u.startsWith(QLatin1Char('h'))                                                         ? n * cooldown::kHour
        : u == QLatin1String("m") || u.startsWith(QLatin1String("min"))                          ? n * cooldown::kMinute
                                                                                                 : n * 1000;
    if (!std::isfinite(ms) || ms <= 0) return std::nullopt;
    return qint64(std::floor(ms + 0.5));
}

Classified classifyError(const ProviderError& e) {
    const QString text = [&] {
        QStringList parts;
        if (!e.message.isEmpty()) parts << e.message;
        if (!e.providerMessage.isEmpty()) parts << e.providerMessage;
        return parts.join(QLatin1Char(' '));
    }();
    std::optional<int> status;
    if (e.status && *e.status >= 100 && *e.status < 600) status = *e.status;
    else if (const auto m = leadingStatusRx().matchView(text); m.hasMatch()) status = m.captured(1).toInt();

    if (e.name == QLatin1String("AbortError") || (has(cancelledRx(), text) && !status))
        return make(ErrorKind::Cancelled, std::nullopt, false, 0, QStringLiteral("stopped"));

    // the request layer already read this answer (status, headers, body) and acted on it: its verdict stands; an error
    // that never went through it is read with the very same words and rules (classifyLimit)
    struct Sig { LimitKind kind; std::optional<qint64> retryAfterMs; };
    std::optional<Sig> sig;
    const bool said = e.verdict.has_value();
    if (said) {
        sig = Sig{e.verdict->kind, e.verdict->retryAfterMs && *e.verdict->retryAfterMs > 0 ? e.verdict->retryAfterMs
                                                                                           : std::nullopt};
    } else if (const auto s = classifyLimit(status.value_or(503), {}, QJsonValue(QJsonValue::Undefined), text,
                                            Clock::system().nowMs())) {
        sig = Sig{s->kind, s->retryAfterMs};
    }
    // StarMedia's OWN daily spend cap: not a provider's limit, no other model helps, nothing rests, nothing is retried
    if ((sig && sig->kind == LimitKind::Cap) || (!said && has(rx::cap(), text)))
        return make(ErrorKind::Cap, status, false, 0, QStringLiteral("the daily spend cap was reached"));
    const std::optional<qint64> wait = sig && sig->retryAfterMs ? sig->retryAfterMs : retryAfterFromText(text);
    const std::optional<qint64> asked = wait && *wait != 0 ? wait : std::nullopt;
    // a daily quota or an empty account: another model (or provider) is the only way forward, and this one rests for hours
    if ((sig && sig->kind == LimitKind::Quota)
        || (!sig && (status == 402 || ((status == 403 || status == 503 || !status) && has(accountRx(), text))))) {
        return make(ErrorKind::Quota, status, true,
                    std::min(cooldown::kMax, std::max(cooldown::kQuota, wait.value_or(0))),
                    QStringLiteral("out of quota"), asked);
    }
    if (status == 401 || (status == 403 && !sig))
        return make(ErrorKind::Auth, status, false, 0, QStringLiteral("not allowed (check the key)"));
    if ((sig && sig->kind == LimitKind::Rate) || (!sig && !status && has(rateWordsRx(), text))) {
        return make(ErrorKind::Rate, status, true,
                    std::min(10 * cooldown::kMinute, std::max(cooldown::kRate, wait.value_or(0))),
                    QStringLiteral("rate limited"), asked);
    }
    if (status == 404 || has(goneRx(), text))
        return make(ErrorKind::Gone, status, true, cooldown::kGone, QStringLiteral("not available"));
    if ((status == 400 || status == 413 || status == 422 || !status) && has(contextRx(), text))
        return make(ErrorKind::Context, status, true, 0, QStringLiteral("the conversation is too long for it"));
    if (has(jsonRx(), text))
        return make(ErrorKind::Json, status, true, 0, QStringLiteral("answered in a format that could not be read"));
    if ((status && *status >= 500) || has(serverWordsRx(), text))
        return make(ErrorKind::Server, status, true, cooldown::kServer, QStringLiteral("not answering right now"));
    return make(ErrorKind::Other, status, false, 0, QStringLiteral("failed"));
}

// ---------------------------------------------------------------------------------------------------------------

ModelHealth::ModelHealth(Clock& clock) : m_clock(clock) {}

qint64 ModelHealth::now() const { return m_clock.nowMs(); }

ModelHealth::ListenerId ModelHealth::onChange(std::function<void(const HealthEvent&)> fn) {
    std::lock_guard lock(m_listenerMutex);
    const ListenerId id = ++m_nextListener;
    m_listeners.emplace(id, std::move(fn));
    return id;
}

void ModelHealth::removeListener(ListenerId id) {
    std::lock_guard lock(m_listenerMutex);
    m_listeners.erase(id);
}

void ModelHealth::notify(const HealthEvent& e) {
    std::vector<std::function<void(const HealthEvent&)>> fns;
    {
        std::lock_guard lock(m_listenerMutex);
        for (const auto& [id, fn] : m_listeners) fns.push_back(fn);
    }
    for (const auto& fn : fns) {
        try { fn(e); } catch (...) { /* a listener must never break a model call */ }
    }
}

std::optional<Cooling> ModelHealth::coolingOf(const QString& model) {
    const qint64 t = now();
    std::lock_guard lock(m_mutex);
    auto it = m_cold.find(model);
    if (it == m_cold.end()) return std::nullopt;
    if (it->second.until <= t) { m_cold.erase(it); return std::nullopt; }
    return it->second;
}

std::optional<Cooling> ModelHealth::mark(const QString& model, const Classified& c, const QString& to) {
    if (model.isEmpty() || c.cooldownMs <= 0) return std::nullopt;
    const qint64 t = now();
    const qint64 until = t + std::min(cooldown::kMax, c.cooldownMs);
    HealthEvent ev;
    {
        std::lock_guard lock(m_mutex);
        std::optional<Cooling> old;
        if (auto it = m_cold.find(model); it != m_cold.end()) {
            if (it->second.until <= t) m_cold.erase(it);
            else old = it->second;
        }
        if (old && old->until >= until) return old;      // never shortens a longer rest, and has nothing new to say
        const Cooling entry{until, c.kind, c.why};
        m_cold[model] = entry;
        // calls in flight when a model ran out all fail one after another, each asking for a rest a little later
        // than the last: that is ONE rest (extended quietly), not one notice per failed call
        ev.type = HealthEvent::Cooled;
        ev.model = model;
        ev.cooling = entry;
        ev.to = to;
        ev.renewed = old && old->kind == c.kind;
    }
    notify(ev);
    return ev.cooling;
}

QStringList ModelHealth::order(const QStringList& models) {
    QStringList ok, seen;
    std::vector<std::pair<QString, qint64>> cold;
    for (const QString& id : models) {
        if (id.isEmpty() || seen.contains(id)) continue;
        seen << id;
        if (const auto c = coolingOf(id)) cold.emplace_back(id, c->until);
        else ok << id;
    }
    std::stable_sort(cold.begin(), cold.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [id, until] : cold) ok << id;
    return ok;
}

void ModelHealth::clear(const QString& model) {
    {
        std::lock_guard lock(m_mutex);
        if (model.isEmpty()) m_cold.clear();
        else m_cold.erase(model);
    }
    HealthEvent ev;
    ev.type = HealthEvent::Cleared;
    ev.model = model;
    notify(ev);
}

QJsonObject ModelHealth::snapshot() {
    const qint64 t = now();
    QJsonObject out;
    std::lock_guard lock(m_mutex);
    for (auto it = m_cold.begin(); it != m_cold.end();) {
        if (it->second.until <= t) { it = m_cold.erase(it); continue; }
        out.insert(it->first, QJsonObject{{QStringLiteral("until"), double(it->second.until)},
                                          {QStringLiteral("kind"), errorKindName(it->second.kind)},
                                          {QStringLiteral("why"), it->second.why}});
        ++it;
    }
    return out;
}

int ModelHealth::load(const QJsonValue& data) {
    if (!data.isObject()) return 0;
    const QJsonObject o = data.toObject();
    const qint64 t = now();
    int n = 0;
    std::lock_guard lock(m_mutex);
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString id = it.key();
        const QJsonValue v = it.value();
        const double untilD = v.isObject() ? jsNumberOfValue(v.toObject().value(QLatin1String("until"))) : std::nan("");
        if (id.isEmpty() || id.size() > 200 || !std::isfinite(untilD) || untilD <= double(t)
            || untilD > double(t + cooldown::kMax))
            continue;
        const qint64 until = qint64(std::floor(untilD + 0.5));
        if (auto old = m_cold.find(id); old != m_cold.end() && old->second.until >= until) { ++n; continue; }
        const QJsonValue kv = v.toObject().value(QLatin1String("kind"));
        ErrorKind kind = ErrorKind::Quota;
        if (kv.isString()) {
            if (const auto k = errorKindFromName(kv.toString()); k && !whyOf(*k).isEmpty()) kind = *k;
        }
        m_cold[id] = Cooling{until, kind, whyOf(kind)};
        ++n;
    }
    return n;
}

QString backAt(qint64 untilMs, qint64 nowMs) {
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(untilMs).toLocalTime();
    const QString t = QLocale::system().toString(d.time(), QLocale::ShortFormat);
    const qint64 mins = qint64(std::floor(double(untilMs - nowMs) / cooldown::kMinute + 0.5));
    if (mins <= 1) return QStringLiteral("in a minute");
    if (mins < 60) return QStringLiteral("in about %1 minutes").arg(mins);
    const bool same = QDateTime::fromMSecsSinceEpoch(nowMs).toLocalTime().date() == d.date();
    return same ? QStringLiteral("at about %1").arg(t) : QStringLiteral("tomorrow at about %1").arg(t);
}

} // namespace studio::provider
