#pragma once
// JSON helpers shared by every Studio module (ST-02).
//
// stable() is byte for byte the TypeScript stableJson of the StarMedia MCP server (mcp/core/util.ts L19-24; the pub
// lane's copy in addons/station/src/pub/core/util.ts gives the same text for JSON values):
//   - object keys sorted like Array.prototype.sort() with no comparator: by UTF-16 code units (so "😀" < "！"),
//   - undefined members dropped, undefined array items written as null, NaN / ±Infinity written as null,
//   - numbers in ECMAScript Number::toString form ("1e+21", "1.5e-7", -0 → "0"),
//   - strings escaped exactly like JSON.stringify (\" \\ \b \f \n \r \t, other controls and lone surrogates as
//     lower-case \u00xx / \udxxx; everything else raw UTF-8, U+2028/2029 and "/" included).
// Hashes of tool arguments, plans and cache keys therefore equal the ones the TS side computes; the spend approval
// tokens and the MCP ledger interop (ST-11) and the content-addressed cache (§4.3) rely on that. Golden values come
// from the real TS (tests/studio/fixtures/core-golden.json, made by tools/studio/scripts/golden-core.mjs).
//
// Numbers: JS keeps every number as a double, so integers held as qint64 in a QJsonValue are written as the double
// they convert to (exact below 2^53, which covers any micro-USD amount under $9 billion).
#include <QByteArray>
#include <QByteArrayView>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <limits>

namespace studio::json {

QByteArray stable(const QJsonValue& v);
QByteArray stable(const QJsonObject& o);
QByteArray stable(const QJsonArray& a);

/// ECMAScript Number::toString(d) (what String(d) and JSON.stringify print for a finite number): "1", "0.1",
/// "0.30000000000000004", "1e+21", "1.5e-7", "123456789012345680000". -0 → "0"; NaN → "NaN"; ±inf → "Infinity".
QString jsNumber(double d);
/// JSON.stringify(s) as UTF-8, quotes included.
QByteArray jsString(QStringView s);
void appendJsString(QByteArray& out, QStringView s);

/// Lower-case hex SHA-256 (64 chars).
QByteArray sha256Hex(QByteArrayView data);
/// sha256Hex(stable(v)): the hash of a value that does not depend on key order.
QByteArray stableHash(const QJsonValue& v);
/// Tool-call arguments hash of the MCP spend gate (mcp/core/register.ts argsHash): sha256(tool + stableJson(args
/// without kControlKeys)), first 32 hex chars. An approval token is bound to it.
QByteArray argsHash(QStringView tool, const QJsonObject& args);
/// register.ts CONTROL_KEYS: arguments that steer the gate itself and never change what a call buys.
const QStringList& controlKeys();

// ---------------- parsing untrusted text ----------------

/// Any JSON value, scalars included (QJsonDocument alone accepts only objects and arrays). Text over `maxBytes` is
/// refused before parsing. A leading UTF-8 BOM is skipped (as QJsonDocument does). On failure returns Undefined and
/// sets `error` (offset and reason, never the text itself).
QJsonValue parse(QByteArrayView text, QString* error = nullptr, qsizetype maxBytes = 64 * 1024 * 1024);
/// parse() that must yield an object; anything else is an error and an empty object.
QJsonObject parseObject(QByteArrayView text, QString* error = nullptr, qsizetype maxBytes = 64 * 1024 * 1024);
/// Compact text of any value (objects and arrays through QJsonDocument, scalars as stable() writes them).
QByteArray compact(const QJsonValue& v);

/// An integral number in [min, max] (a double with a fraction, a string, NaN or out of range → `def`, ok=false).
qint64 toInt64(const QJsonValue& v, qint64 def = 0, bool* ok = nullptr,
               qint64 min = std::numeric_limits<qint64>::min(), qint64 max = std::numeric_limits<qint64>::max());

/// Value at a dotted path ("spend.dailyCapUsd") inside nested objects; Undefined when any step is missing.
QJsonValue at(const QJsonObject& o, QStringView dottedPath);
/// Sets (or, with an Undefined value, removes) the value at a dotted path, creating intermediate objects (a non-object
/// in the way is replaced). Empty path segments are refused (false).
bool setAt(QJsonObject& o, QStringView dottedPath, const QJsonValue& v);

} // namespace studio::json
