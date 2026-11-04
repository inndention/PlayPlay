#pragma once
// Embedded data files (ST-25): tools/studio/data/<name>.json, exported from the StarMedia TypeScript by the dev-time
// tools/studio/scripts/export-starmedia-data.mjs and compiled into PlayPlayStudioLib through resources.qrc
// (:/studio/data/<name>.json). At run time these embedded copies are the only source: Node is never needed, and no file
// on disk can replace them (a dropped-in realpeople.json must not be able to weaken the safety lists).
//
//   model-limits   per-model input limits (src/model-limits.data.ts), platform caps, retired models, whole-request caps
//                  and the request-size constants of src/payload.ts                          → ST-07 provider/limits
//   realpeople     the public-figure lists, look-alike letters and picture-rule patterns of mcp/media/realpeople.ts
//                                                                                              → ST-15 safety
//   model-prompts  researched prompting guides (src/studio/model-prompts.data.ts), the enhancePrompt / recommendModels
//                  tables of src/studio/modelprompts.ts and the spot fallback guides of mcp/spot/guides.ts → ST-19
//   styles         style presets (src/studio/styles.ts)                                       → ST-19, studio-film
//
// Each file carries `_comment` (what its members mean), `version` (kFormatVersion) and `source` (the TS files, their
// last commit and the sha256 of their text with CRLF folded to LF; test_studio_core compares that with the checkout).
// tools/studio/data/schema.json describes all four; load() hands out a file only after it passed that schema.
//
// Patterns in the files are ECMAScript regular expressions ({source, flags}). regex() compiles one with its JavaScript
// meaning kept where PCRE2 (QRegularExpression) differs, so a ported rule matches what the TS rule matches.
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace studio::data {

/// The `version` every embedded file must carry; a file of another version is refused (the layout changed).
constexpr int kFormatVersion = 1;

/// The embedded data files by name: "model-limits", "model-prompts", "realpeople", "styles".
QStringList names();
/// The embedded bytes of tools/studio/data/<name>.json for a name of names() or "schema"; empty for any other name.
QByteArray raw(QStringView name);
/// tools/studio/data/schema.json, parsed (cached). Empty when it is missing or not a JSON object.
QJsonObject schema();

/// Checks `value` against a JSON Schema subset: $ref ("#/$defs/<name>" in `root`), type, enum, const, anyOf, minLength,
/// maxLength, pattern (ECMAScript, unanchored, compiled through regex() with the u flag), minimum, maximum,
/// exclusiveMinimum, items, minItems, maxItems, uniqueItems, properties, required, additionalProperties, propertyNames,
/// minProperties, plus the annotations $schema / $id / $comment / $defs / title / description. Any other keyword, an
/// unresolved $ref or an invalid pattern is itself an error, so a schema this validator does not understand never
/// passes silently. Lengths count UTF-16 code units (as the JS exporter does). Returns one "<path>: <reason>" message
/// per violation (paths like $.models[3].id), at most `maxErrors`; empty means valid.
QStringList validate(const QJsonValue& value, const QJsonValue& schema, const QJsonObject& root, int maxErrors = 50);

/// Parses the embedded <name>.json and checks its version and schema.json $defs/<name>. Empty means valid.
QStringList check(QStringView name);

/// The embedded file once check() passed (parsed and checked once, then cached; thread-safe). Fails closed: an unknown
/// name or a file that does not pass gives an empty object and `error`, and is logged once. Callers treat an empty
/// object as "no data" and refuse the work that needs it (the safety lists above all) instead of running without it.
QJsonObject load(QStringView name, QString* error = nullptr);

/// A data-file pattern ({source, flags}) as a QRegularExpression with the JavaScript meaning (see jsToPcre). The JS
/// flag i maps to CaseInsensitiveOption; m and s are translated into the pattern; g, y and d (match state and indices)
/// do not change what matches and are ignored. Anything it cannot translate exactly, or that does not compile, gives
/// an invalid expression (isValid() false; it never matches) and `error`. Check isValid() before relying on a result.
QRegularExpression regex(const QJsonValue& pattern, QString* error = nullptr);
QRegularExpression regex(QStringView source, QStringView flags, QString* error = nullptr);

/// The PCRE2 pattern for an ECMAScript pattern with `flags` (d g i m s u y; v is refused). Where the engines differ it
/// writes the JS meaning out:
///   - `$` is end of input (`\z`; PCRE2's `$` also matches before a final newline); with m, `^` / `$` are line starts /
///     ends at \n, \r, U+2028 and U+2029;
///   - `.` excludes \n, \r, U+2028 and U+2029 (all of them with s);
///   - `\s` adds the Unicode spaces JS counts (U+00A0, U+1680, U+2000-200A, U+2028/9, U+202F, U+205F, U+3000,
///     U+FEFF), `\S` is its complement; `\S` inside a class cannot be written this way and is refused;
///   - `\uXXXX` (surrogate pairs joined) and, with u, `\u{X…}` become `\x{…}`; a lone surrogate is refused;
///   - `[^]` (any character) and `[]` (nothing) are written out; `[` inside a class is a literal;
///   - without u, an escaped letter PCRE2 would give a meaning (`\a`, `\e`, `\z`, `\Q` …) is that literal letter, as
///     in JS; `\p` without u is refused (it would be a literal "p", certainly not what the pattern means); with u an
///     unknown escape is refused, as JS refuses it.
/// `\b`, `\w` and `\d` are ASCII in both engines and copied. Without u, JS matches UTF-16 units and PCRE2 code points:
/// the same for every BMP character. Returns an empty string and sets `error` for what it refuses.
QString jsToPcre(QStringView source, QStringView flags, QString* error = nullptr);

} // namespace studio::data
