#include "core/datafiles.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "core/json.h"
#include "core/log.h"

// AUTORCC compiles tools/studio/resources.qrc into the static PlayPlayStudioLib, where nothing references the
// initializer and the linker would drop it (see studioservice.cpp). Q_INIT_RESOURCE must run from the global namespace.
static void studioDataInitResources() { Q_INIT_RESOURCE(resources); }

namespace studio::data {
namespace {

constexpr auto kCat = u"data";

void ensureResources() {
    static std::once_flag once;
    std::call_once(once, [] { studioDataInitResources(); });
}

// ---------------- ECMAScript → PCRE2 ----------------

// What JS \s adds to PCRE2's ASCII \s ([\t\n\v\f\r ]), written for use inside a character class.
const QString& jsExtraSpace() {
    static const QString s = QStringLiteral("\\x{a0}\\x{1680}\\x{2000}-\\x{200a}\\x{2028}\\x{2029}\\x{202f}\\x{205f}\\x{3000}\\x{feff}");
    return s;
}
constexpr QStringView kLineEnds = u"\\n\\r\\x{2028}\\x{2029}";

bool isHex(QChar c) { return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F'); }

// \uXXXX at s[i] (i points at the backslash): the code unit, or -1.
int unit4(QStringView s, qsizetype i) {
    if (i + 6 > s.size() || s[i + 1] != u'u') return -1;
    for (int k = 2; k < 6; ++k)
        if (!isHex(s[i + k])) return -1;
    return s.mid(i + 2, 4).toString().toInt(nullptr, 16);
}

QString hexEscape(char32_t cp) { return QStringLiteral("\\x{%1}").arg(uint(cp), 0, 16); }

} // namespace

QString jsToPcre(QStringView src, QStringView flags, QString* error) {
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return QString();
    };
    bool unicode = false, multiline = false, dotAll = false;
    QSet<QChar> seen;
    for (const QChar f : flags) {
        if (seen.contains(f)) return fail(QStringLiteral("flag %1 given twice").arg(f));
        seen.insert(f);
        switch (f.unicode()) {
        case u'u': unicode = true; break;
        case u'm': multiline = true; break;
        case u's': dotAll = true; break;
        case u'i': case u'g': case u'y': case u'd': break;
        case u'v': return fail(QStringLiteral("the v flag (set notation) is not supported"));
        default: return fail(QStringLiteral("unknown flag %1").arg(f));
        }
    }
    QString out;
    out.reserve(src.size() + 16);
    bool inClass = false;
    const qsizetype n = src.size();
    for (qsizetype i = 0; i < n;) {
        const QChar c = src[i];
        if (c == u'\\') {
            if (i + 1 >= n) return fail(QStringLiteral("a trailing backslash"));
            const QChar d = src[i + 1];
            switch (d.unicode()) {
            case u'u': {
                if (unicode && i + 2 < n && src[i + 2] == u'{') {
                    qsizetype j = i + 3;
                    while (j < n && isHex(src[j])) ++j;
                    if (j == i + 3 || j >= n || src[j] != u'}' || j - (i + 3) > 6) return fail(QStringLiteral("a bad \\u{...} escape"));
                    const uint cp = src.mid(i + 3, j - (i + 3)).toString().toUInt(nullptr, 16);
                    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail(QStringLiteral("\\u{%1} is not a scalar value").arg(cp, 0, 16));
                    out += hexEscape(cp);
                    i = j + 1;
                    continue;
                }
                const int u1 = unit4(src, i);
                if (u1 < 0) {
                    if (unicode) return fail(QStringLiteral("a bad \\u escape"));
                    out += u'u'; // JS without u: an identity escape
                    i += 2;
                    continue;
                }
                if (u1 >= 0xD800 && u1 <= 0xDBFF) {
                    const int u2 = unit4(src, i + 6);
                    if (u2 < 0xDC00 || u2 > 0xDFFF) return fail(QStringLiteral("a lone surrogate escape (PCRE2 matches code points)"));
                    out += hexEscape(0x10000 + ((char32_t(u1) - 0xD800) << 10) + (char32_t(u2) - 0xDC00));
                    i += 12;
                    continue;
                }
                if (u1 >= 0xDC00 && u1 <= 0xDFFF) return fail(QStringLiteral("a lone surrogate escape (PCRE2 matches code points)"));
                out += hexEscape(char32_t(u1));
                i += 6;
                continue;
            }
            case u's':
                out += inClass ? QStringLiteral("\\s") + jsExtraSpace() : QStringLiteral("[\\s") + jsExtraSpace() + u']';
                i += 2;
                continue;
            case u'S':
                if (inClass) return fail(QStringLiteral("\\S inside a character class has no exact PCRE2 form"));
                out += QStringLiteral("[^\\s") + jsExtraSpace() + u']';
                i += 2;
                continue;
            case u'v': // JS: vertical tab only; PCRE2: any vertical space
                out += QStringLiteral("\\x{b}");
                i += 2;
                continue;
            case u'x':
                if (i + 3 < n && isHex(src[i + 2]) && isHex(src[i + 3])) {
                    out += src.mid(i, 4);
                    i += 4;
                    continue;
                }
                if (unicode) return fail(QStringLiteral("a bad \\x escape"));
                out += u'x';
                i += 2;
                continue;
            case u'0':
                if (i + 2 < n && src[i + 2].isDigit()) return fail(QStringLiteral("a legacy octal escape"));
                out += QStringLiteral("\\x{0}");
                i += 2;
                continue;
            case u'p': case u'P':
                if (!unicode) return fail(QStringLiteral("\\%1 without the u flag is a literal letter in JavaScript").arg(d));
                out += src.mid(i, 2);
                i += 2;
                continue;
            case u'/':
                out += u'/';
                i += 2;
                continue;
            case u'b': case u'B': case u'd': case u'D': case u'w': case u'W': case u't': case u'n': case u'f': case u'r':
            case u'c': case u'k':
                out += src.mid(i, 2); // same meaning in both engines (\c and \k take what follows as plain text)
                i += 2;
                continue;
            default:
                if (d.isDigit()) { // a back-reference in both
                    out += src.mid(i, 2);
                } else if ((d >= u'a' && d <= u'z') || (d >= u'A' && d <= u'Z')) {
                    if (unicode) return fail(QStringLiteral("\\%1 is not a JavaScript escape").arg(d));
                    out += d; // JS without u: the letter itself; PCRE2 would read \a \e \z \Q ... as something else
                } else {
                    out += src.mid(i, 2); // escaped punctuation is literal in both
                }
                i += 2;
                continue;
            }
        }
        if (inClass) {
            if (c == u']') inClass = false;
            out += c == u'[' ? QStringLiteral("\\[") : QString(c); // PCRE2 would read [: :] / [. .] / [= =] specially
            ++i;
            continue;
        }
        if (c == u'[') {
            if (src.mid(i, 3) == QStringView(u"[^]")) { out += QStringLiteral("[\\s\\S]"); i += 3; continue; } // JS: any character
            if (src.mid(i, 2) == QStringView(u"[]")) { out += QStringLiteral("(?!)"); i += 2; continue; }     // JS: never matches
            inClass = true;
            out += u'[';
            ++i;
            if (i < n && src[i] == u'^') { out += u'^'; ++i; }
            continue;
        }
        if (c == u'$') {
            out += multiline ? QStringLiteral("(?=[%1]|\\z)").arg(kLineEnds) : QStringLiteral("\\z");
        } else if (c == u'^' && multiline) {
            out += QStringLiteral("(?:^|(?<=[%1]))").arg(kLineEnds);
        } else if (c == u'.') {
            out += dotAll ? QStringLiteral("[\\s\\S]") : QStringLiteral("[^%1]").arg(kLineEnds);
        } else {
            out += c;
        }
        ++i;
    }
    if (inClass) return fail(QStringLiteral("an unterminated character class"));
    if (error) error->clear();
    return out;
}

QRegularExpression regex(QStringView source, QStringView flags, QString* error) {
    QString why;
    const QString p = jsToPcre(source, flags, &why);
    if (p.isNull() || !why.isEmpty()) {
        if (error) *error = why;
        return QRegularExpression(QStringLiteral("(")); // invalid: never matches
    }
    QRegularExpression re(p, flags.contains(u'i') ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    if (!re.isValid()) {
        if (error) *error = QStringLiteral("PCRE2: %1 at %2").arg(re.errorString()).arg(re.patternErrorOffset());
        return re;
    }
    if (error) error->clear();
    return re;
}

QRegularExpression regex(const QJsonValue& pattern, QString* error) {
    const QJsonObject o = pattern.toObject();
    const QJsonValue s = o.value(QStringLiteral("source")), f = o.value(QStringLiteral("flags"));
    if (!s.isString() || !(f.isString() || f.isUndefined())) {
        if (error) *error = QStringLiteral("a pattern is an object {source, flags}");
        return QRegularExpression(QStringLiteral("("));
    }
    return regex(s.toString(), f.toString(), error);
}

// ---------------- JSON Schema subset ----------------

namespace {

const QSet<QString>& knownKeywords() {
    static const QSet<QString> k{
        QStringLiteral("$schema"), QStringLiteral("$id"), QStringLiteral("$comment"), QStringLiteral("$defs"), QStringLiteral("title"), QStringLiteral("description"), QStringLiteral("$ref"), QStringLiteral("type"),
        QStringLiteral("enum"), QStringLiteral("const"), QStringLiteral("anyOf"), QStringLiteral("minLength"), QStringLiteral("maxLength"), QStringLiteral("pattern"), QStringLiteral("minimum"),
        QStringLiteral("maximum"), QStringLiteral("exclusiveMinimum"), QStringLiteral("items"), QStringLiteral("minItems"), QStringLiteral("maxItems"), QStringLiteral("uniqueItems"),
        QStringLiteral("properties"), QStringLiteral("required"), QStringLiteral("additionalProperties"), QStringLiteral("propertyNames"), QStringLiteral("minProperties")};
    return k;
}

QString typeOf(const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Null: return QStringLiteral("null");
    case QJsonValue::Bool: return QStringLiteral("boolean");
    case QJsonValue::String: return QStringLiteral("string");
    case QJsonValue::Array: return QStringLiteral("array");
    case QJsonValue::Object: return QStringLiteral("object");
    case QJsonValue::Double: {
        const double d = v.toDouble();
        return std::isfinite(d) && std::floor(d) == d ? QStringLiteral("integer") : QStringLiteral("number");
    }
    default: return QStringLiteral("undefined");
    }
}

// Deep JSON equality with numbers compared by value (1 == 1.0), as JSON.stringify comparison does in the exporter.
bool same(const QJsonValue& a, const QJsonValue& b) {
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case QJsonValue::Double: return a.toDouble() == b.toDouble();
    case QJsonValue::Array: {
        const QJsonArray x = a.toArray(), y = b.toArray();
        if (x.size() != y.size()) return false;
        for (qsizetype i = 0; i < x.size(); ++i)
            if (!same(x.at(i), y.at(i))) return false;
        return true;
    }
    case QJsonValue::Object: {
        const QJsonObject x = a.toObject(), y = b.toObject();
        if (x.size() != y.size()) return false;
        for (auto it = x.begin(), jt = y.begin(); it != x.end(); ++it, ++jt) // both iterate in key order
            if (it.key() != jt.key() || !same(it.value(), jt.value())) return false;
        return true;
    }
    default: return a == b;
    }
}

QString shortJson(const QJsonValue& v) {
    QString t = QString::fromUtf8(json::compact(v));
    if (t.size() > 80) t = t.left(77) + u"...";
    return t;
}

QString num(double d) { return json::jsNumber(d); }

struct Validator {
    const QJsonObject& root;
    int maxErrors;
    QStringList errs;
    QHash<QString, QRegularExpression> patterns;
    int depth = 0;

    bool full() const { return errs.size() >= maxErrors; }
    void add(const QString& e) { if (!full()) errs << e; }

    void run(const QJsonValue& v, const QJsonValue& sv, const QString& at) {
        if (full()) return;
        if (sv.isBool()) {
            if (!sv.toBool()) add(at + u": not allowed");
            return;
        }
        if (!sv.isObject()) { add(QStringLiteral("schema at %1: not an object or boolean").arg(at)); return; }
        if (depth > 64) { add(QStringLiteral("schema at %1: nested too deeply ($ref loop?)").arg(at)); return; }
        const QJsonObject s = sv.toObject();
        for (auto it = s.begin(); it != s.end(); ++it)
            if (!knownKeywords().contains(it.key())) add(QStringLiteral("schema at %1: unsupported keyword %2").arg(at, it.key()));

        if (s.contains(u"$ref")) {
            const QString ref = s.value(u"$ref").toString();
            const QString prefix = QStringLiteral("#/$defs/");
            const QJsonObject defs = root.value(u"$defs").toObject();
            const QString key = ref.startsWith(prefix) ? ref.mid(prefix.size()) : QString();
            if (key.isEmpty() || !defs.contains(key)) {
                add(QStringLiteral("schema at %1: unresolved $ref %2").arg(at, ref));
            } else {
                ++depth;
                run(v, defs.value(key), at);
                --depth;
            }
        }
        const QString t = typeOf(v);
        if (s.contains(u"type")) {
            const QJsonValue ts = s.value(u"type");
            QStringList types;
            if (ts.isArray()) for (const QJsonValue& x : ts.toArray()) types << x.toString();
            else types << ts.toString();
            bool ok = false;
            for (const QString& x : types) ok = ok || x == t || (x == u"number" && t == u"integer");
            if (!ok) { add(QStringLiteral("%1: expected %2, got %3").arg(at, types.join(u" or "), t)); return; }
        }
        if (s.contains(u"enum")) {
            const QJsonArray e = s.value(u"enum").toArray();
            if (std::none_of(e.begin(), e.end(), [&](const QJsonValue& x) { return same(x, v); }))
                add(QStringLiteral("%1: %2 is not one of %3").arg(at, shortJson(v), shortJson(e)));
        }
        if (s.contains(u"const") && !same(s.value(u"const"), v))
            add(QStringLiteral("%1: expected %2").arg(at, shortJson(s.value(u"const"))));
        if (s.contains(u"anyOf")) {
            bool any = false;
            for (const QJsonValue& alt : s.value(u"anyOf").toArray()) {
                Validator sub{root, 1, {}, {}, depth + 1};
                sub.run(v, alt, at);
                if (sub.errs.isEmpty()) { any = true; break; }
            }
            if (!any) add(at + u": matches none of anyOf");
        }
        if (t == u"string") {
            const QString str = v.toString();
            if (s.contains(u"minLength") && str.size() < s.value(u"minLength").toDouble())
                add(QStringLiteral("%1: shorter than %2").arg(at, num(s.value(u"minLength").toDouble())));
            if (s.contains(u"maxLength") && str.size() > s.value(u"maxLength").toDouble())
                add(QStringLiteral("%1: longer than %2").arg(at, num(s.value(u"maxLength").toDouble())));
            if (s.contains(u"pattern")) {
                const QString p = s.value(u"pattern").toString();
                auto it = patterns.find(p);
                if (it == patterns.end()) {
                    QString why;
                    it = patterns.insert(p, regex(p, u"u", &why));
                    if (!it->isValid()) add(QStringLiteral("schema at %1: pattern %2: %3").arg(at, p, why));
                }
                if (it->isValid() && !it->match(str).hasMatch())
                    add(QStringLiteral("%1: %2 does not match %3").arg(at, shortJson(v), p));
            }
        }
        if (t == u"number" || t == u"integer") {
            const double d = v.toDouble();
            if (s.contains(u"minimum") && d < s.value(u"minimum").toDouble())
                add(QStringLiteral("%1: below %2").arg(at, num(s.value(u"minimum").toDouble())));
            if (s.contains(u"maximum") && d > s.value(u"maximum").toDouble())
                add(QStringLiteral("%1: above %2").arg(at, num(s.value(u"maximum").toDouble())));
            if (s.contains(u"exclusiveMinimum") && d <= s.value(u"exclusiveMinimum").toDouble())
                add(QStringLiteral("%1: not above %2").arg(at, num(s.value(u"exclusiveMinimum").toDouble())));
        }
        if (t == u"array") {
            const QJsonArray a = v.toArray();
            if (s.contains(u"minItems") && a.size() < s.value(u"minItems").toDouble())
                add(QStringLiteral("%1: fewer than %2 items").arg(at, num(s.value(u"minItems").toDouble())));
            if (s.contains(u"maxItems") && a.size() > s.value(u"maxItems").toDouble())
                add(QStringLiteral("%1: more than %2 items").arg(at, num(s.value(u"maxItems").toDouble())));
            if (s.value(u"uniqueItems").toBool()) {
                for (qsizetype i = 1; i < a.size() && !full(); ++i)
                    for (qsizetype j = 0; j < i; ++j)
                        if (same(a.at(i), a.at(j))) { add(QStringLiteral("%1: duplicate item %2").arg(at, shortJson(a.at(i)))); break; }
            }
            if (s.contains(u"items"))
                for (qsizetype i = 0; i < a.size() && !full(); ++i) run(a.at(i), s.value(u"items"), QStringLiteral("%1[%2]").arg(at).arg(i));
        }
        if (t == u"object") {
            const QJsonObject o = v.toObject();
            if (s.contains(u"minProperties") && o.size() < s.value(u"minProperties").toDouble())
                add(QStringLiteral("%1: fewer than %2 members").arg(at, num(s.value(u"minProperties").toDouble())));
            for (const QJsonValue& r : s.value(u"required").toArray())
                if (!o.contains(r.toString())) add(QStringLiteral("%1: missing %2").arg(at, r.toString()));
            const QJsonObject props = s.value(u"properties").toObject();
            for (auto it = o.begin(); it != o.end() && !full(); ++it) { // members through the iterator (ST-02 Qt key-order note)
                const QString sub = at + u'.' + it.key();
                if (s.contains(u"propertyNames")) run(QJsonValue(it.key()), s.value(u"propertyNames"), sub + u" (name)");
                if (props.contains(it.key())) run(it.value(), props.value(it.key()), sub);
                else if (s.contains(u"additionalProperties")) run(it.value(), s.value(u"additionalProperties"), sub);
            }
        }
    }
};

struct Loaded {
    QJsonObject object;
    QString error;
};

QStringList checkInto(QStringView name, QJsonObject* out) {
    if (!names().contains(name.toString())) return {QStringLiteral("unknown data file %1").arg(name)};
    const QByteArray bytes = raw(name);
    if (bytes.isEmpty()) return {QStringLiteral("%1.json is not embedded").arg(name)};
    QString err;
    const QJsonValue v = json::parse(bytes, &err);
    if (v.isUndefined()) return {QStringLiteral("%1.json is not JSON: %2").arg(name, err)};
    if (!v.isObject()) return {QStringLiteral("%1.json is not a JSON object").arg(name)};
    const QJsonValue ver = v.toObject().value(u"version");
    if (!ver.isDouble() || ver.toDouble() != kFormatVersion)
        return {QStringLiteral("%1.json has format version %2; this build reads version %3").arg(name, shortJson(ver)).arg(kFormatVersion)};
    const QJsonObject sch = schema();
    if (sch.isEmpty()) return {QStringLiteral("schema.json is missing or unreadable")};
    if (!sch.value(u"$defs").toObject().contains(name.toString())) return {QStringLiteral("schema.json has no $defs/%1").arg(name)};
    const QStringList errs = validate(v, QJsonObject{{QStringLiteral("$ref"), QStringLiteral("#/$defs/%1").arg(name)}}, sch);
    if (errs.isEmpty() && out) *out = v.toObject();
    return errs;
}

} // namespace

QStringList validate(const QJsonValue& value, const QJsonValue& schema, const QJsonObject& root, int maxErrors) {
    Validator v{root, qMax(1, maxErrors), {}, {}, 0};
    v.run(value, schema, QStringLiteral("$"));
    return v.errs;
}

// ---------------- the files ----------------

QStringList names() {
    return {QStringLiteral("model-limits"), QStringLiteral("model-prompts"), QStringLiteral("realpeople"), QStringLiteral("styles")};
}

QByteArray raw(QStringView name) {
    if (name != u"schema" && !names().contains(name.toString())) return {}; // never a path the caller made up
    ensureResources();
    QFile f(QStringLiteral(":/studio/data/%1.json").arg(name));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QJsonObject schema() {
    static const QJsonObject s = [] {
        QString err;
        const QJsonObject o = json::parseObject(raw(u"schema"), &err);
        if (o.isEmpty()) log::error(kCat, QStringLiteral("schema.json: %1").arg(err.isEmpty() ? QStringLiteral("empty") : err));
        return o;
    }();
    return s;
}

QStringList check(QStringView name) { return checkInto(name, nullptr); }

QJsonObject load(QStringView name, QString* error) {
    static QMutex mutex;
    static QHash<QString, Loaded> cache;
    QMutexLocker lock(&mutex);
    auto it = cache.find(name.toString());
    if (it == cache.end()) {
        Loaded l;
        const QStringList errs = checkInto(name, &l.object);
        if (!errs.isEmpty()) {
            l.object = QJsonObject();
            l.error = QStringLiteral("data/%1.json refused: %2").arg(name, errs.first());
            if (errs.size() > 1) l.error += QStringLiteral(" (and %1 more)").arg(errs.size() - 1);
            log::error(kCat, l.error);
        }
        it = cache.insert(name.toString(), l);
    }
    if (error) *error = it->error;
    return it->object;
}

} // namespace studio::data
