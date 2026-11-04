#include "core/json.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <vector>

namespace studio::json {

namespace {

// Array.prototype.sort() without a comparator compares the strings' UTF-16 code units. Spelled out instead of relying
// on QString::operator< so a surrogate pair (0xD800-0xDBFF) sorts before U+E000-U+FFFF exactly as in JS.
bool utf16Less(const QString& a, const QString& b) {
    const char16_t* pa = reinterpret_cast<const char16_t*>(a.utf16());
    const char16_t* pb = reinterpret_cast<const char16_t*>(b.utf16());
    return std::lexicographical_compare(pa, pa + a.size(), pb, pb + b.size());
}

void writeStable(QByteArray& out, const QJsonValue& v) {
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:            // JSON.stringify(undefined) ?? 'null' (top level and array items)
        out += "null";
        break;
    case QJsonValue::Bool:
        out += v.toBool() ? "true" : "false";
        break;
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (!std::isfinite(d)) out += "null";
        else out += jsNumber(d).toLatin1();
        break;
    }
    case QJsonValue::String:
        appendJsString(out, v.toString());
        break;
    case QJsonValue::Array: {
        const QJsonArray a = v.toArray();
        out += '[';
        for (qsizetype i = 0; i < a.size(); ++i) {
            if (i) out += ',';
            writeStable(out, a.at(i));
        }
        out += ']';
        break;
    }
    case QJsonValue::Object: {
        // Pairs from the iterator, never o.value(key): in Qt 6.9 an object parsed by QJsonDocument keeps its keys in
        // code-point order but looks them up in UTF-16 order, so a key with a surrogate pair next to U+E000..U+FFFF
        // keys (an emoji beside "！") reads back as Undefined and its member would silently vanish from the hash.
        const QJsonObject o = v.toObject();
        std::vector<std::pair<QString, QJsonValue>> members;
        members.reserve(size_t(o.size()));
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) members.emplace_back(it.key(), it.value());
        std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return utf16Less(a.first, b.first); });
        out += '{';
        bool first = true;
        for (const auto& [k, x] : members) {
            if (x.isUndefined()) continue; // filter((k) => o[k] !== undefined)
            if (!first) out += ',';
            first = false;
            appendJsString(out, k);
            out += ':';
            writeStable(out, x);
        }
        out += '}';
        break;
    }
    }
}

void appendUtf8(QByteArray& out, char32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

void appendEscape(QByteArray& out, char16_t c) {
    static const char hex[] = "0123456789abcdef";   // JSON.stringify writes lower-case hex
    out += "\\u";
    out += hex[(c >> 12) & 0xF];
    out += hex[(c >> 8) & 0xF];
    out += hex[(c >> 4) & 0xF];
    out += hex[c & 0xF];
}

} // namespace

QByteArray stable(const QJsonValue& v) { QByteArray out; writeStable(out, v); return out; }
QByteArray stable(const QJsonObject& o) { return stable(QJsonValue(o)); }
QByteArray stable(const QJsonArray& a) { return stable(QJsonValue(a)); }

QString jsNumber(double d) {
    if (std::isnan(d)) return QStringLiteral("NaN");
    if (d == 0) return QStringLiteral("0");                 // +0 and -0
    if (std::isinf(d)) return d < 0 ? QStringLiteral("-Infinity") : QStringLiteral("Infinity");
    // Shortest round-trip digits (Ryu in MSVC's <charconv>; V8 also picks the shortest, closest digit string), then
    // ECMAScript's layout rules (Number::toString, ECMA-262 §6.1.6.1.20) over digits s (k of them) and exponent n.
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, std::fabs(d), std::chars_format::scientific);
    const std::string_view sci(buf, size_t(r.ptr - buf));   // "d.ddde+XX" or "de-XX"
    const size_t ePos = sci.find('e');
    std::string digits;
    for (char c : sci.substr(0, ePos)) if (c != '.') digits += c;
    int exp10 = 0;
    std::from_chars(sci.data() + ePos + 1 + (sci[ePos + 1] == '+' ? 1 : 0), sci.data() + sci.size(), exp10);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();   // to_chars never pads, but be safe
    const int k = int(digits.size());
    const int n = exp10 + 1;
    std::string s = d < 0 ? "-" : "";
    if (k <= n && n <= 21) {
        s += digits;
        s.append(size_t(n - k), '0');
    } else if (0 < n && n <= 21) {
        s += digits.substr(0, size_t(n));
        s += '.';
        s += digits.substr(size_t(n));
    } else if (-6 < n && n <= 0) {
        s += "0.";
        s.append(size_t(-n), '0');
        s += digits;
    } else {
        s += digits[0];
        if (k > 1) { s += '.'; s += digits.substr(1); }
        s += 'e';
        s += n - 1 >= 0 ? '+' : '-';
        s += std::to_string(std::abs(n - 1));
    }
    return QString::fromLatin1(s.data(), qsizetype(s.size()));
}

void appendJsString(QByteArray& out, QStringView s) {
    out += '"';
    const qsizetype n = s.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t c = s[i].unicode();
        switch (c) {
        case u'"': out += "\\\""; continue;
        case u'\\': out += "\\\\"; continue;
        case u'\b': out += "\\b"; continue;
        case u'\f': out += "\\f"; continue;
        case u'\n': out += "\\n"; continue;
        case u'\r': out += "\\r"; continue;
        case u'\t': out += "\\t"; continue;
        default: break;
        }
        if (c < 0x20) { appendEscape(out, c); continue; }
        if (c < 0x80) { out += char(c); continue; }
        if (QChar::isHighSurrogate(c) && i + 1 < n && QChar::isLowSurrogate(s[i + 1].unicode())) {
            appendUtf8(out, QChar::surrogateToUcs4(c, s[i + 1].unicode()));
            ++i;
            continue;
        }
        if (QChar::isSurrogate(c)) { appendEscape(out, c); continue; }   // lone surrogate (well-formed JSON.stringify)
        appendUtf8(out, c);
    }
    out += '"';
}

QByteArray jsString(QStringView s) { QByteArray out; appendJsString(out, s); return out; }

QByteArray sha256Hex(QByteArrayView data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex(); }

QByteArray stableHash(const QJsonValue& v) { return sha256Hex(stable(v)); }

const QStringList& controlKeys() {
    static const QStringList k{QStringLiteral("dry_run"), QStringLiteral("approve"), QStringLiteral("wait")};
    return k;
}

QByteArray argsHash(QStringView tool, const QJsonObject& args) {
    QJsonObject kept = args;
    for (const QString& k : controlKeys()) kept.remove(k);
    return sha256Hex(tool.toUtf8() + stable(kept)).left(32);
}

QJsonValue parse(QByteArrayView text, QString* error, qsizetype maxBytes) {
    if (error) error->clear();
    if (text.size() > maxBytes) {
        if (error) *error = QStringLiteral("JSON text of %1 bytes exceeds the %2 byte limit").arg(text.size()).arg(maxBytes);
        return QJsonValue(QJsonValue::Undefined);
    }
    // QJsonDocument skips a UTF-8 BOM only at the very start, so it has to go before the '[' below (Notepad "UTF-8
    // with BOM", Windows PowerShell 5.1 Set-Content / Out-File -Encoding UTF8 write one).
    const qsizetype bom = text.startsWith(QByteArrayView("\xEF\xBB\xBF")) ? 3 : 0;
    text = text.sliced(bom);
    // Wrapped in an array so a scalar document parses too; "1,2" becomes two items and is refused below.
    QByteArray wrapped;
    wrapped.reserve(text.size() + 2);
    wrapped += '[';
    wrapped += text;
    wrapped += ']';
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(wrapped, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray() || doc.array().size() != 1) {
        if (error) {
            *error = pe.error != QJsonParseError::NoError
                ? QStringLiteral("invalid JSON at offset %1: %2").arg(qMax(0, pe.offset - 1) + bom).arg(pe.errorString())
                : QStringLiteral("invalid JSON: expected exactly one value");
        }
        return QJsonValue(QJsonValue::Undefined);
    }
    return doc.array().at(0);
}

QJsonObject parseObject(QByteArrayView text, QString* error, qsizetype maxBytes) {
    QString err;
    const QJsonValue v = parse(text, &err, maxBytes);
    if (err.isEmpty() && !v.isObject()) err = QStringLiteral("expected a JSON object");
    if (error) *error = err;
    return err.isEmpty() ? v.toObject() : QJsonObject();
}

QByteArray compact(const QJsonValue& v) {
    if (v.isObject()) return QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    if (v.isArray()) return QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    return stable(v);
}

qint64 toInt64(const QJsonValue& v, qint64 def, bool* ok, qint64 min, qint64 max) {
    if (ok) *ok = false;
    if (!v.isDouble()) return def;
    const QVariant var = v.toVariant();    // keeps a qint64 the parser stored exactly (beyond 2^53)
    qint64 n;
    if (var.typeId() == QMetaType::LongLong) {
        n = var.toLongLong();
    } else {
        const double d = v.toDouble();
        // 2^63 itself is not representable as qint64; the range check is on the double first.
        if (!std::isfinite(d) || d != std::trunc(d) || d < -9223372036854775808.0 || d >= 9223372036854775808.0) return def;
        n = qint64(d);
    }
    if (n < min || n > max) return def;
    if (ok) *ok = true;
    return n;
}

QJsonValue at(const QJsonObject& o, QStringView dottedPath) {
    const auto parts = dottedPath.split(u'.');
    QJsonValue cur = o;
    for (const QStringView p : parts) {
        if (!cur.isObject() || p.isEmpty()) return QJsonValue(QJsonValue::Undefined);
        cur = cur.toObject().value(p);
    }
    return cur;
}

namespace {
void setAtRec(QJsonObject& o, const QList<QStringView>& parts, qsizetype i, const QJsonValue& v) {
    const QString key = parts[i].toString();
    if (i + 1 == parts.size()) {
        if (v.isUndefined()) o.remove(key);
        else o.insert(key, v);
        return;
    }
    QJsonObject child = o.value(key).toObject();   // a non-object in the way becomes an object
    setAtRec(child, parts, i + 1, v);
    if (child.isEmpty() && v.isUndefined()) o.remove(key);
    else o.insert(key, child);
}
} // namespace

bool setAt(QJsonObject& o, QStringView dottedPath, const QJsonValue& v) {
    const QList<QStringView> parts = dottedPath.split(u'.');
    for (const QStringView p : parts) if (p.isEmpty()) return false;
    setAtRec(o, parts, 0, v);
    return true;
}

} // namespace studio::json
