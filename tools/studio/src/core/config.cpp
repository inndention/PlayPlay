#include "core/config.h"

#include "core/json.h"
#include "core/log.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace studio {

QDebug operator<<(QDebug d, const Secret& s) {
    QDebugStateSaver saver(d);
    d.nospace() << (s.isEmpty() ? "Secret(empty)" : "Secret(hidden)");
    return d;
}

QString sourceName(Source s) {
    switch (s) {
    case Source::Env: return QStringLiteral("env");
    case Source::Config: return QStringLiteral("config");
    case Source::Default: break;
    }
    return QStringLiteral("default");
}

namespace settings {
const Setting openRouterBaseUrl{QStringLiteral("openrouter.baseUrl"), {QStringLiteral("OPENROUTER_BASE_URL")}, Setting::String,
                                QStringLiteral("https://openrouter.ai/api/v1")};
const Setting autoApproveUsd{QStringLiteral("spend.autoApproveUsd"), {QStringLiteral("PLAYPLAY_STUDIO_AUTO_APPROVE_USD")},
                             Setting::Usd, 0.25, 0, 10000};
const Setting sessionCapUsd{QStringLiteral("spend.sessionCapUsd"), {QStringLiteral("PLAYPLAY_STUDIO_SESSION_CAP_USD")},
                            Setting::Usd, 5, 0, 100000};
// The daily cap is shared with Claude's MCP servers through the ledger (§4.4), so their variable applies here too.
const Setting dailyCapUsd{QStringLiteral("spend.dailyCapUsd"),
                          {QStringLiteral("PLAYPLAY_STUDIO_DAILY_CAP_USD"), QStringLiteral("STARMEDIA_MCP_DAILY_CAP_USD")},
                          Setting::Usd, 20, 0, 1000000};
const Setting starmediaHome{QStringLiteral("spend.starmediaHome"), {QStringLiteral("STARMEDIA_MCP_HOME")}, Setting::String, QString()};
const Setting restPort{QStringLiteral("net.restPort"), {QStringLiteral("PLAYPLAY_STUDIO_PORT")}, Setting::Integer, 7760, 0, 65535};
const Setting wsPort{QStringLiteral("net.wsPort"), {QStringLiteral("PLAYPLAY_STUDIO_WS_PORT")}, Setting::Integer, 7761, 0, 65535};
const Setting logLevel{QStringLiteral("log.level"), {QStringLiteral("PLAYPLAY_STUDIO_LOG_LEVEL")}, Setting::String, QStringLiteral("info")};

const QList<const Setting*>& all() {
    static const QList<const Setting*> l{&openRouterBaseUrl, &autoApproveUsd, &sessionCapUsd, &dailyCapUsd, &starmediaHome,
                                         &restPort, &wsPort, &logLevel};
    return l;
}
} // namespace settings

namespace {

const QString kKeyPath = QStringLiteral("openrouter.apiKey");
const QString kKeyEnv = QStringLiteral("OPENROUTER_API_KEY");

QString rangeText(const Setting& s) {
    return QStringLiteral("%1..%2").arg(json::jsNumber(s.min), json::jsNumber(s.max));
}

// One candidate (env text or a config value) checked against the setting. Empty `err` and a value = usable.
QJsonValue convert(const Setting& s, const QJsonValue& in, QString* err) {
    err->clear();
    const bool isText = in.isString();
    const QString text = in.toString().trimmed();
    switch (s.kind) {
    case Setting::Number:
    case Setting::Integer: {
        double d = 0;
        bool ok = false;
        if (in.isDouble()) { d = in.toDouble(); ok = true; }
        else if (isText) d = text.toDouble(&ok);
        if (!ok || !std::isfinite(d)) { *err = QStringLiteral("not a number"); return {}; }
        if (s.kind == Setting::Integer && (d != std::trunc(d) || std::fabs(d) > 9007199254740991.0)) {   // integer() casts it
            *err = QStringLiteral("not a whole number within ±2^53");
            return {};
        }
        if (d < s.min || d > s.max) { *err = QStringLiteral("%1 is outside %2").arg(json::jsNumber(d), rangeText(s)); return {}; }
        return d;
    }
    case Setting::Usd: {
        money::Micro m = 0;
        bool ok = false;
        if (in.isDouble()) m = money::fromUsd(in.toDouble(), &ok);
        else if (isText) ok = money::parseUsd(text, &m);
        if (!ok) { *err = QStringLiteral("not a USD amount"); return {}; }
        bool loOk = false, hiOk = false;   // a bound beyond the int64 range means "unbounded"
        const money::Micro lo = money::fromUsd(s.min, &loOk), hi = money::fromUsd(s.max, &hiOk);
        if ((loOk && m < lo) || (hiOk && m > hi)) {
            *err = QStringLiteral("%1 is outside %2 USD").arg(money::format(m), rangeText(s));
            return {};
        }
        return double(m);                  // micro-USD (exact in a double below 2^53)
    }
    case Setting::Bool: {
        if (in.isBool()) return in.toBool();
        const QString t = text.toLower();
        if (t == QLatin1String("1") || t == QLatin1String("true") || t == QLatin1String("yes") || t == QLatin1String("on")) return true;
        if (t == QLatin1String("0") || t == QLatin1String("false") || t == QLatin1String("no") || t == QLatin1String("off")) return false;
        *err = QStringLiteral("not a boolean");
        return {};
    }
    case Setting::String:
        if (!isText) { *err = QStringLiteral("not a string"); return {}; }
        if (text.isEmpty()) { *err = QStringLiteral("empty"); return {}; }
        return text;
    }
    *err = QStringLiteral("unknown kind");
    return {};
}

bool secretPath(QStringView path) {
    for (const QStringView seg : path.split(u'.'))
        if (log::isSecretKey(seg)) return true;
    return false;
}

QJsonValue redact(const QJsonValue& v) {
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue& x : v.toArray()) out.append(redact(x));
        return out;
    }
    if (!v.isObject()) return v;
    QJsonObject out;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QJsonValue x = it.value();
        const bool hide = log::isSecretKey(it.key()) && !x.isNull() && !x.isBool() && !(x.isString() && x.toString().isEmpty());
        out.insert(it.key(), hide ? QJsonValue(QStringLiteral("(hidden)")) : redact(x));   // mcp redactValue rule
    }
    return out;
}

} // namespace

Config::Config(const QString& dataDir)
    : m_file(dataDir.isEmpty() ? QString() : QDir(dataDir).filePath(QStringLiteral("config.json"))) {}

bool Config::load(QString* error) {
    if (error) error->clear();
    if (m_file.isEmpty()) return true;
    QFile f(m_file);
    if (!f.exists()) {
        QWriteLocker g(&m_lock);
        m_values = QJsonObject();
        m_loadError.clear();
        return true;
    }
    QString err;
    QJsonObject o;
    if (f.size() > 4 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot read %1").arg(QDir::toNativeSeparators(m_file));
    } else {
        QString perr;                      // never echoes the text (the key may be in it)
        o = json::parseObject(f.readAll(), &perr);
        if (!perr.isEmpty()) err = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(m_file), perr);
    }
    QWriteLocker g(&m_lock);
    m_loadError = err;                     // money limits fail closed while it is set (resolve())
    if (!err.isEmpty()) {
        if (error) *error = err;
        return false;
    }
    m_values = o;
    return true;
}

QString Config::loadError() const {
    QReadLocker g(&m_lock);
    return m_loadError;
}

bool Config::save(QString* error) const {
    if (error) error->clear();
    if (m_file.isEmpty()) return true;
    QByteArray body;
    {
        QReadLocker g(&m_lock);
        if (!m_loadError.isEmpty()) {
            if (error) *error = QStringLiteral("not saved: the last load failed (%1); fix or remove the file first").arg(m_loadError);
            return false;
        }
        body = QJsonDocument(m_values).toJson(QJsonDocument::Indented);
    }
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile f(m_file);                   // temp file + rename: a reader never sees half a file
    if (!f.open(QIODevice::WriteOnly) || f.write(body) != body.size() || !f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(m_file));
        return false;
    }
    return true;
}

Config::Resolved Config::resolve(const Setting& s) const {
    Resolved r;
    QString err;
    // Money limits fail closed: the first set-but-invalid source ends the chain at $0. Falling through could land on a
    // larger cap (a lower-precedence variable, config.json, the default) than the one the operator meant to set.
    const bool failClosed = s.kind == Setting::Usd;
    const auto closed = [&r](Source src, const QString& envName) {
        r.value = 0.0;
        r.source = src;
        r.envName = envName;
        r.failedClosed = true;
        return r;
    };
    for (const QString& name : s.env) {
        const QString raw = qEnvironmentVariable(name.toLocal8Bit().constData());
        if (raw.trimmed().isEmpty()) continue;             // unset or blank = not set (mcp/core/config.ts num())
        const QJsonValue v = convert(s, QJsonValue(raw), &err);
        if (err.isEmpty()) { r.value = v; r.source = Source::Env; r.envName = name; return r; }
        r.errors << log::scrub(QStringLiteral("env %1: %2").arg(name, err));
        if (failClosed) return closed(Source::Env, name);
    }
    if (failClosed) {
        const QString loadErr = loadError();
        if (!loadErr.isEmpty()) {          // the unreadable file may hold a lower cap
            r.errors << log::scrub(QStringLiteral("config.json could not be read: %1").arg(loadErr));
            return closed(Source::Config, QString());
        }
    }
    const QJsonValue stored = this->stored(s.key);
    if (!stored.isUndefined() && !stored.isNull()) {
        const QJsonValue v = convert(s, stored, &err);
        if (err.isEmpty()) { r.value = v; r.source = Source::Config; return r; }
        r.errors << log::scrub(QStringLiteral("config %1: %2").arg(s.key, err));
        if (failClosed) return closed(Source::Config, QString());
    }
    if (s.def.isUndefined() || s.def.isNull() || (s.def.isString() && s.def.toString().isEmpty())) {
        r.value = s.kind == Setting::String ? QJsonValue(QString()) : s.def;
        return r;
    }
    const QJsonValue v = convert(s, s.def, &err);
    Q_ASSERT_X(err.isEmpty(), "Config::resolve", "invalid built-in default");
    r.value = v;
    return r;
}

money::Micro Config::usd(const Setting& s, bool* ok) const {
    const Resolved r = resolve(s);
    if (ok) *ok = !r.failedClosed;
    return qint64(r.value.toDouble());
}

QStringList Config::spendLimitErrors() const {
    QStringList out;
    for (const Setting* s : settings::all()) {
        if (s->kind != Setting::Usd) continue;
        const Resolved r = resolve(*s);
        if (r.failedClosed) out << QStringLiteral("%1: %2 (limit held at $0)").arg(s->key, r.errors.join(QStringLiteral("; ")));
    }
    return out;
}

QJsonValue Config::stored(QStringView path) const {
    QReadLocker g(&m_lock);
    return json::at(m_values, path);
}

bool Config::set(QStringView path, const QJsonValue& v, QString* error) {
    if (error) error->clear();
    if (secretPath(path)) {
        if (error) *error = QStringLiteral("%1 holds a secret; use the key setter").arg(path.toString());
        return false;
    }
    QWriteLocker g(&m_lock);
    if (!json::setAt(m_values, path, v)) {
        if (error) *error = QStringLiteral("bad setting path '%1'").arg(path.toString());
        return false;
    }
    return true;
}

Config::KeyInfo Config::openRouterKey() const {
    KeyInfo k;
    const QString env = qEnvironmentVariable(kKeyEnv.toLatin1().constData()).trimmed();
    if (!env.isEmpty()) {
        k = {Secret(env), Source::Env};
    } else {
        const QString cfg = stored(kKeyPath).toString().trimmed();
        if (!cfg.isEmpty()) k = {Secret(cfg), Source::Config};
    }
    if (!k.key.isEmpty()) log::addSecret(k.key.reveal());   // from now on it cannot appear in any log line
    return k;
}

QString Config::keySource() const {
    const KeyInfo k = openRouterKey();
    return k.key.isEmpty() ? QStringLiteral("none") : sourceName(k.source);
}

void Config::setOpenRouterKey(const QString& key) {
    const QString k = key.trimmed();
    if (!k.isEmpty()) log::addSecret(k);
    QWriteLocker g(&m_lock);
    json::setAt(m_values, kKeyPath, k.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(k));
}

QString Config::openRouterBaseUrl() const {
    QString u = string(settings::openRouterBaseUrl);
    while (u.endsWith(QLatin1Char('/'))) u.chop(1);
    return u;
}

QString Config::starmediaHome() const {
    const QString h = string(settings::starmediaHome);
    if (!h.isEmpty()) return QDir::cleanPath(QDir::fromNativeSeparators(h));
    QString profile = qEnvironmentVariable("USERPROFILE");
    if (profile.isEmpty()) profile = QDir::homePath();
    return QDir::cleanPath(QDir::fromNativeSeparators(profile) + QStringLiteral("/StarMedia/mcp"));   // mcp/core/config.ts
}

QJsonObject Config::describe() const {
    QJsonObject out;
    QJsonObject values;
    for (const Setting* s : settings::all()) {
        const Resolved r = resolve(*s);
        QJsonObject e;
        if (s->kind == Setting::Usd) {
            const money::Micro m = qint64(r.value.toDouble());
            e[QStringLiteral("value")] = money::toUsd(m);
            e[QStringLiteral("micro")] = double(m);
        } else if (s == &settings::starmediaHome) {
            e[QStringLiteral("value")] = starmediaHome();
        } else {
            e[QStringLiteral("value")] = r.value;
        }
        e[QStringLiteral("source")] = sourceName(r.source);
        if (!r.envName.isEmpty()) e[QStringLiteral("env")] = r.envName;
        if (!r.errors.isEmpty()) e[QStringLiteral("errors")] = QJsonArray::fromStringList(r.errors);
        if (r.failedClosed) e[QStringLiteral("failedClosed")] = true;
        values[s->key] = e;
    }
    out[QStringLiteral("settings")] = values;
    const QStringList spendErrors = spendLimitErrors();
    if (!spendErrors.isEmpty()) out[QStringLiteral("spendLimitErrors")] = QJsonArray::fromStringList(spendErrors);
    const QString loadErr = loadError();
    if (!loadErr.isEmpty()) out[QStringLiteral("loadError")] = log::scrub(loadErr);
    const KeyInfo k = openRouterKey();
    out[QStringLiteral("openrouter")] = QJsonObject{{QStringLiteral("hasKey"), !k.key.isEmpty()},
                                                    {QStringLiteral("keySource"), k.key.isEmpty() ? QStringLiteral("none") : sourceName(k.source)}};
    out[QStringLiteral("file")] = m_file.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QDir::toNativeSeparators(m_file));
    return out;
}

QJsonObject Config::storedRedacted() const {
    QReadLocker g(&m_lock);
    return redact(m_values).toObject();
}

} // namespace studio
