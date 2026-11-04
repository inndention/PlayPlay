#pragma once
// Studio configuration (ST-02): <data>/config.json plus environment overrides.
//
// Precedence for every setting: environment (the first of its variables that is set) > config.json > built-in
// default. An invalid value is never guessed at; it is reported (Resolved::errors, describe()) and then:
//   - money limits (Setting::Usd: auto-approve, session and daily caps) FAIL CLOSED. A set but invalid value stops the
//     chain and the limit resolves to $0 (failedClosed), and so does a config.json that could not be read while no
//     valid env value overrides it. Falling through could pick a larger cap than the one the operator set, and the
//     MCP servers that share the daily ledger refuse to start on the same variables (mcp/core/config.ts num()).
//     spendLimitErrors() lists them so the spend gate / start-up (ST-11, ST-14) can refuse and say why.
//   - every other setting skips the invalid value and falls through to the next source.
//
// The OpenRouter key (docs/plans/starmedia-studio.md §2.7, §4.7) is read at call time: env OPENROUTER_API_KEY, else
// config "openrouter.apiKey". It is handed out only as a Secret (no implicit conversion to text, "(hidden)" in qDebug),
// registered with the log scrubber every time it is read, and never part of describe(), storedRedacted() or any reply.
// describe() carries hasKey / keySource ("env" | "config" | "none") instead, the same contract as PlayPlay's AiModels;
// storedRedacted() shows secret-named fields as "(hidden)".
//
// Thread-safe: settings may be read from the event loop and the worker pool while the API changes them.
#include "core/money.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>

class QDebug;

namespace studio {

/// A secret string (API key, token). Use reveal() exactly where it goes on the wire; nothing else can print it.
class Secret {
public:
    Secret() = default;
    explicit Secret(QString v) : m_v(std::move(v)) {}
    bool isEmpty() const { return m_v.isEmpty(); }
    const QString& reveal() const { return m_v; }
    /// "Bearer <key>" for an Authorization header.
    QByteArray bearer() const { return "Bearer " + m_v.toUtf8(); }
    bool operator==(const Secret& o) const { return m_v == o.m_v; }

private:
    QString m_v;
};
QDebug operator<<(QDebug d, const Secret& s);

enum class Source { Default, Config, Env };
QString sourceName(Source s);              // "default" | "config" | "env"

/// One setting: its config.json path, the environment variables that override it (first set wins) and its default.
struct Setting {
    enum Kind { Number, Integer, Usd, String, Bool };   // Usd = a spending limit: $0 is its fail-closed value
    QString key;                           // dotted path in config.json, e.g. "spend.dailyCapUsd"
    QStringList env;
    Kind kind = String;
    QJsonValue def;                        // Usd: a number of dollars (or decimal text)
    double min = -1e300, max = 1e300;      // Number / Integer / Usd (in dollars)
};

namespace settings {
extern const Setting openRouterBaseUrl;    // "openrouter.baseUrl", env OPENROUTER_BASE_URL, https://openrouter.ai/api/v1
extern const Setting autoApproveUsd;       // "spend.autoApproveUsd", 0.25 (mcp/core/config.ts defaults)
extern const Setting sessionCapUsd;        // "spend.sessionCapUsd", 5
extern const Setting dailyCapUsd;          // "spend.dailyCapUsd", 20 (shared with the MCP servers through the ledger)
extern const Setting starmediaHome;        // "spend.starmediaHome", env STARMEDIA_MCP_HOME, default %USERPROFILE%/StarMedia/mcp
extern const Setting restPort;             // "net.restPort", env PLAYPLAY_STUDIO_PORT, 7760
extern const Setting wsPort;               // "net.wsPort", env PLAYPLAY_STUDIO_WS_PORT, 7761
extern const Setting logLevel;             // "log.level", env PLAYPLAY_STUDIO_LOG_LEVEL, "info"
const QList<const Setting*>& all();        // the well-known ones (describe() lists them)
} // namespace settings

class Config {
public:
    /// Settings file <dataDir>/config.json; an empty dataDir keeps everything in memory (tests, --no-config).
    explicit Config(const QString& dataDir = QString());

    QString filePath() const { return m_file; }
    /// A missing file is not an error (defaults). An unreadable file or one that is not a JSON object is: false with
    /// `error`; the values held so far stay for ordinary settings, but money limits fail closed until a load succeeds
    /// (the file may hold a lower cap). A leading UTF-8 BOM is accepted.
    bool load(QString* error = nullptr);
    /// Why the last load() failed (empty after a successful one).
    QString loadError() const;
    /// Atomic write (temp file + rename) of everything stored, the key included (the data dir is the user's own
    /// %LOCALAPPDATA% folder, like StarMedia's env-only key or PlayPlay's settings). Refused while loadError() is set:
    /// writing over a file that could not be read would replace the operator's settings (caps included).
    bool save(QString* error = nullptr) const;

    // ---------------- generic ----------------
    struct Resolved {
        QJsonValue value;                  // Usd settings resolve to a number of micro-USD
        Source source = Source::Default;
        QString envName;                   // the variable it came from (Source::Env)
        QStringList errors;                // invalid env / config values (skipped, or the reason it failed closed)
        bool failedClosed = false;         // a money limit forced to $0 by an invalid value or an unreadable file
    };
    Resolved resolve(const Setting& s) const;
    double number(const Setting& s) const { return resolve(s).value.toDouble(); }
    qint64 integer(const Setting& s) const { return qint64(resolve(s).value.toDouble()); }
    /// A money setting in micro-USD; `ok` false when it failed closed (the value is then 0).
    money::Micro usd(const Setting& s, bool* ok = nullptr) const;
    QString string(const Setting& s) const { return resolve(s).value.toString(); }
    bool flag(const Setting& s) const { return resolve(s).value.toBool(); }

    /// What config.json holds at a dotted path (Undefined when absent).
    QJsonValue stored(QStringView path) const;
    /// Stores a value (Undefined removes it). Secret-named paths (log::isSecretKey on any segment) are refused: the
    /// key goes through setOpenRouterKey(). Does not save; call save().
    bool set(QStringView path, const QJsonValue& v, QString* error = nullptr);

    // ---------------- the OpenRouter key ----------------
    struct KeyInfo { Secret key; Source source = Source::Default; };   // Default = none
    /// Read now: env OPENROUTER_API_KEY (trimmed, non-empty), else config openrouter.apiKey. Registers the value with
    /// log::addSecret, so it is scrubbed from every log line from then on.
    KeyInfo openRouterKey() const;
    bool hasOpenRouterKey() const { return !openRouterKey().key.isEmpty(); }
    /// "env" | "config" | "none".
    QString keySource() const;
    /// Stores (or with an empty key, clears) the config key. The env key still wins while it is set.
    void setOpenRouterKey(const QString& key);

    // ---------------- typed shortcuts ----------------
    QString openRouterBaseUrl() const;     // without a trailing "/"
    QString starmediaHome() const;
    money::Micro autoApprove(bool* ok = nullptr) const { return usd(settings::autoApproveUsd, ok); }
    money::Micro sessionCap(bool* ok = nullptr) const { return usd(settings::sessionCapUsd, ok); }
    money::Micro dailyCap(bool* ok = nullptr) const { return usd(settings::dailyCapUsd, ok); }
    /// "<key>: <reason>" for every well-known money limit that failed closed. Non-empty = do not spend: the spend gate
    /// (ST-11) and start-up (ST-14) refuse and show these, as the MCP servers refuse to start.
    QStringList spendLimitErrors() const;

    /// Safe to log or return: every well-known setting {value, source[, env][, errors][, failedClosed]} (USD settings
    /// as dollars and micro), spendLimitErrors (when any), loadError (when set), openrouter {hasKey, keySource}, the
    /// file path. Never a secret.
    QJsonObject describe() const;
    /// config.json as stored, with every secret-named field (log::isSecretKey) replaced by "(hidden)".
    QJsonObject storedRedacted() const;

private:
    QString m_file;
    mutable QReadWriteLock m_lock;
    QJsonObject m_values;
    QString m_loadError;                   // under m_lock
};

} // namespace studio
