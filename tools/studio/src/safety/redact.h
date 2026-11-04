#pragma once
// Secret redaction of structured values (ST-15): port of StarMedia mcp/core/util.ts redactValue / redactText /
// truncate. Text redaction itself is log::scrub (core/log.h: registered secret values, key / token shapes, data: URIs,
// base64 runs), so a value redacted here and a log line agree; this adds the structure: secret-NAMED fields
// (log::isSecretKey: apiKey, token, password, authorization, approve …) are replaced whatever their value looks like,
// long strings are cut to a head plus a hash, deep or huge structures are cut off.
//
// Use it for anything that is logged, stored as a memory / audit note or returned in diagnostics: tool inputs, request
// bodies (forLog), headers. Never for what goes on the wire.
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace studio::safety {

/// util.ts REDACTED.
inline constexpr QStringView kRedacted = u"[redacted]";

struct RedactOptions {
    int maxString = 2000;          // longer strings: head + "…[truncated N chars, sha256:<12 hex>]"
    QStringList extraSecrets;      // exact values to remove too (shorter than 8 chars ignored)
};

/// Free text with secrets, data: URIs and base64 runs removed (util.ts redactText = extra secrets, then log::scrub).
QString redactText(const QString& s, const QStringList& extraSecrets = {});
/// util.ts truncate: s itself when it fits, else the first `max` UTF-16 units + "…[truncated N chars, sha256:<12 hex>]".
QString truncate(const QString& s, int max);
/// A loggable copy of any JSON value: secret-named fields → "[redacted]" (unless null / "" / boolean), strings
/// redacted and truncated, at most 50 array items (+ "[N more items]") and 80 object fields (+ "…": "N more fields"),
/// "[nested too deep]" below depth 6.
QJsonValue redactValue(const QJsonValue& v, const RedactOptions& o = {});
QJsonObject redactObject(const QJsonObject& o, const RedactOptions& opt = {});
/// "[binary N bytes]" (util.ts for Uint8Array / ArrayBuffer).
QString redactBinary(const QByteArray& b);
/// HTTP headers for a log line: secret-named headers (Authorization, X-Api-Key, Cookie …) → "[redacted]", others
/// redactText'd.
QList<QPair<QByteArray, QByteArray>> redactHeaders(const QList<QPair<QByteArray, QByteArray>>& headers);
/// One line for a request body in a log: the compact JSON of redactValue(body) with strings cut to `maxString`.
QString forLog(const QJsonObject& body, int maxString = 300);

} // namespace studio::safety
