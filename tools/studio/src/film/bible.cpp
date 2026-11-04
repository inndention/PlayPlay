#include "film/bible.h"

#include "core/json.h"

#include <QSet>

#include <array>
#include <cmath>

namespace studio::film {

namespace {

constexpr std::array<const char*, 5> kBibleKinds{"character", "location", "prop", "style", "voice"};

FieldSpec text(const char* name, int maxLen, bool hashed, QStringList aliases = {}) {
    FieldSpec f;
    f.name = QString::fromLatin1(name);
    f.type = FieldType::Text;
    f.maxLen = maxLen;
    f.hashed = hashed;
    f.aliases = std::move(aliases);
    return f;
}
FieldSpec number(const char* name, double min, double max, bool hashed, QStringList aliases = {}) {
    FieldSpec f;
    f.name = QString::fromLatin1(name);
    f.type = FieldType::Number;
    f.min = min;
    f.max = max;
    f.hashed = hashed;
    f.aliases = std::move(aliases);
    return f;
}
FieldSpec ref(const char* name, BibleKind k, bool hashed, QStringList aliases = {}) {
    FieldSpec f;
    f.name = QString::fromLatin1(name);
    f.type = FieldType::Ref;
    f.refKinds = {k};
    f.hashed = hashed;
    f.aliases = std::move(aliases);
    return f;
}

QString kindWord(BibleKind k) { return bibleKindName(k); }
QString kindsWord(const QList<BibleKind>& ks) {
    QStringList w;
    for (BibleKind k : ks) w << kindWord(k);
    return w.join(QStringLiteral(" or "));
}

// JS Number(v) for what an agent sends: a number, or a string holding one.
std::optional<double> numberOf(const QJsonValue& v) {
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        if (ok) return d;
    }
    return std::nullopt;
}

QString numText(double d) { return json::jsNumber(d); }

} // namespace

QString bibleKindName(BibleKind k) { return QString::fromLatin1(kBibleKinds[size_t(k)]); }

std::optional<BibleKind> parseBibleKind(QStringView s) {
    const QString t = s.trimmed().toString().toLower();
    for (size_t i = 0; i < kBibleKinds.size(); ++i) {
        const QString n = QString::fromLatin1(kBibleKinds[i]);
        if (t == n || t == n + QLatin1Char('s')) return BibleKind(i);
    }
    return std::nullopt;
}

const QList<BibleKind>& allBibleKinds() {
    static const QList<BibleKind> all{BibleKind::Character, BibleKind::Location, BibleKind::Prop, BibleKind::Style,
                                      BibleKind::Voice};
    return all;
}

const FieldSpec* findField(const QList<FieldSpec>& specs, QStringView key) {
    for (const FieldSpec& f : specs)
        if (key == f.name) return &f;
    for (const FieldSpec& f : specs)
        for (const QString& a : f.aliases)
            if (key == a) return &f;
    return nullptr;
}

QString fieldList(const QList<FieldSpec>& specs) {
    QStringList names;
    for (const FieldSpec& f : specs)
        if (!f.output) names << f.name;
    return names.join(QStringLiteral(", "));
}

QString normaliseKey(QStringView key) {
    QString out;
    out.reserve(key.size() + 4);
    const QString k = key.trimmed().toString();
    for (qsizetype i = 0; i < k.size(); ++i) {
        const QChar c = k[i];
        if (c.isUpper()) {
            // camelCase boundary: "afterSceneId" -> "after_scene_id" (not inside an all-caps run)
            if (i > 0 && !out.isEmpty() && out.back() != QLatin1Char('_') && (k[i - 1].isLower() || k[i - 1].isDigit()))
                out += QLatin1Char('_');
            out += c.toLower();
        } else if (c == QLatin1Char('-') || c == QLatin1Char(' ') || c == QLatin1Char('.')) {
            if (!out.isEmpty() && out.back() != QLatin1Char('_')) out += QLatin1Char('_');
        } else {
            out += c;
        }
    }
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    return out;
}

QString cleanText(QStringView s, int maxLen) {
    const bool singleLine = maxLen <= 300;
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t c = s[i].unicode();
        if (c == u'\r') {
            if (i + 1 < s.size() && s[i + 1] == u'\n') continue;
            out += singleLine ? QLatin1Char(' ') : QLatin1Char('\n');
        } else if (c == u'\n') {
            out += singleLine ? QLatin1Char(' ') : QLatin1Char('\n');
        } else if (c < 0x20 || c == 0x7F) {
            out += QLatin1Char(' ');
        } else {
            out += QChar(c);
        }
    }
    if (singleLine) return out.simplified();
    return out.trimmed();
}

FieldCheck checkField(const FieldSpec& f, const QJsonValue& v, const RefResolver& refs, QStringView what) {
    FieldCheck r;
    auto fail = [&](const QString& e) {
        r.ok = false;
        r.error = e;
        return r;
    };
    if (f.output)
        return fail(QStringLiteral("\"%1\" is written by generation, not by an edit").arg(f.name));
    if (v.isNull() || v.isUndefined()) {
        r.given = false;
        return r;
    }
    // "-" clears an optional field (ops.ts on_screen rule, generalised); "" means "not given" (ops.ts nonEmpty)
    if (v.isString()) {
        const QString t = v.toString().trimmed();
        if (t.isEmpty()) {
            r.given = false;
            return r;
        }
        if (t == QLatin1String("-")) {
            r.clear = true;
            return r;
        }
    }
    switch (f.type) {
    case FieldType::Text: {
        QString s;
        if (v.isString()) s = v.toString();
        else if (v.isDouble()) s = numText(v.toDouble());
        else if (v.isBool()) s = v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        else return fail(QStringLiteral("%1 of a %2 must be text").arg(f.name, what));
        s = cleanText(s, f.maxLen);
        if (s.isEmpty()) {
            r.given = false;
            return r;
        }
        if (s.size() > f.maxLen)
            return fail(QStringLiteral("%1 of a %2 is too long (%3 characters; at most %4)")
                            .arg(f.name, what).arg(s.size()).arg(f.maxLen));
        r.value = s;
        return r;
    }
    case FieldType::Number: {
        const std::optional<double> d = numberOf(v);
        if (!d || !std::isfinite(*d) || *d < f.min || *d > f.max)
            return fail(QStringLiteral("%1 of a %2 must be a number from %3 to %4")
                            .arg(f.name, what, numText(f.min), numText(f.max)));
        r.value = std::round(*d * 100) / 100;
        return r;
    }
    case FieldType::Enum: {
        if (!v.isString()) return fail(QStringLiteral("%1 must be one of: %2").arg(f.name, f.values.join(QStringLiteral(", "))));
        const QString t = v.toString().trimmed();
        for (const QString& e : f.values)
            if (t.compare(e, Qt::CaseInsensitive) == 0) {
                r.value = e;
                return r;
            }
        return fail(QStringLiteral("%1 \"%2\" is not one of: %3").arg(f.name, t.left(40), f.values.join(QStringLiteral(", "))));
    }
    case FieldType::Ref: {
        if (!v.isString()) return fail(QStringLiteral("%1 must name a %2 of the bible").arg(f.name, kindsWord(f.refKinds)));
        QString err;
        const QString id = refs ? refs(v.toString().trimmed(), f.refKinds, &err) : QString();
        if (id.isEmpty()) return fail(err.isEmpty() ? QStringLiteral("%1: no such %2").arg(f.name, kindsWord(f.refKinds)) : err);
        r.value = id;
        return r;
    }
    case FieldType::Refs: {
        QStringList items;
        if (v.isArray()) {
            const QJsonArray a = v.toArray();
            if (a.isEmpty()) {
                r.clear = true;
                return r;
            }
            for (const QJsonValue& e : a) {
                if (!e.isString() || e.toString().trimmed().isEmpty())
                    return fail(QStringLiteral("%1 must be a list of %2 names or ids").arg(f.name, kindsWord(f.refKinds)));
                items << e.toString().trimmed();
            }
        } else if (v.isString()) {
            for (const QString& p : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
                if (!p.trimmed().isEmpty()) items << p.trimmed();
        } else {
            return fail(QStringLiteral("%1 must be a list of %2 names or ids").arg(f.name, kindsWord(f.refKinds)));
        }
        QStringList ids;
        for (const QString& item : items) {
            QString err;
            const QString id = refs ? refs(item, f.refKinds, &err) : QString();
            if (id.isEmpty()) return fail(err.isEmpty() ? QStringLiteral("%1: no %2 \"%3\"").arg(f.name, kindsWord(f.refKinds), item) : err);
            if (!ids.contains(id)) ids << id;
        }
        if (ids.size() > f.maxLen)
            return fail(QStringLiteral("%1 names %2 entries; at most %3").arg(f.name).arg(ids.size()).arg(f.maxLen));
        r.value = QJsonArray::fromStringList(ids);
        return r;
    }
    case FieldType::Asset: {
        const QString t = v.toString().trimmed().toLower();
        if (!isSha256(t)) return fail(QStringLiteral("%1 must be an artifact sha256 (64 hex characters)").arg(f.name));
        r.value = t;
        return r;
    }
    }
    return fail(QStringLiteral("%1: unsupported field").arg(f.name));
}

QJsonObject hashedFields(const QList<FieldSpec>& specs, const QJsonObject& data) {
    QJsonObject out;
    for (const FieldSpec& f : specs)
        if (f.hashed && data.contains(f.name)) out.insert(f.name, data.value(f.name));
    return out;
}

QJsonObject withoutOutputs(const QList<FieldSpec>& specs, const QJsonObject& data) {
    QJsonObject out = data;
    for (const FieldSpec& f : specs)
        if (f.output) out.remove(f.name);
    return out;
}

bool sameEntry(const BibleEntry& a, const BibleEntry& b) {
    return a.id == b.id && a.projectId == b.projectId && a.kind == b.kind && a.name == b.name && a.data == b.data
        && a.refs == b.refs;
}

const QList<FieldSpec>& bibleFields(BibleKind k) {
    static const QList<FieldSpec> character{
        text("description", 2000, true, {QStringLiteral("appearance"), QStringLiteral("look"), QStringLiteral("looks")}),
        text("wardrobe", 1000, true, {QStringLiteral("costume"), QStringLiteral("clothes"), QStringLiteral("outfit")}),
        text("age", 40, true),
        ref("voice", BibleKind::Voice, false, {QStringLiteral("voice_id")}),
        text("notes", 2000, false, {QStringLiteral("note")}),
    };
    static const QList<FieldSpec> location{
        text("description", 2000, true, {QStringLiteral("look"), QStringLiteral("setting")}),
        text("notes", 2000, false, {QStringLiteral("note")}),
    };
    static const QList<FieldSpec> prop{
        text("description", 2000, true, {QStringLiteral("look")}),
        text("notes", 2000, false, {QStringLiteral("note")}),
    };
    static const QList<FieldSpec> style = [] {
        FieldSpec preset;
        preset.name = QStringLiteral("preset");
        preset.type = FieldType::Enum;
        preset.values = stylePresets();
        preset.values << QStringLiteral("custom");
        preset.hashed = true;
        preset.aliases = {QStringLiteral("style_id"), QStringLiteral("style")};
        return QList<FieldSpec>{
            preset,
            text("custom", 2000, true, {QStringLiteral("custom_style"), QStringLiteral("description")}),
            text("palette", 300, true, {QStringLiteral("colours"), QStringLiteral("colors")}),
            text("notes", 2000, false, {QStringLiteral("note")}),
        };
    }();
    static const QList<FieldSpec> voice{
        text("model", 120, true, {QStringLiteral("model_id"), QStringLiteral("tts_model")}),
        text("voice", 80, true, {QStringLiteral("preset"), QStringLiteral("voice_name")}),
        text("profile", 80, true, {QStringLiteral("profile_id"), QStringLiteral("clone")}),
        number("speed", 0.5, 2, true),
        text("notes", 2000, false, {QStringLiteral("note")}),
    };
    switch (k) {
    case BibleKind::Character: return character;
    case BibleKind::Location: return location;
    case BibleKind::Prop: return prop;
    case BibleKind::Style: return style;
    case BibleKind::Voice: return voice;
    }
    return character;
}

const QStringList& stylePresets() {
    // StarMedia src/studio/styles.ts STYLES, in order (2026-10-26)
    static const QStringList ids{
        QStringLiteral("cinematic"), QStringLiteral("realistic"), QStringLiteral("documentary"),
        QStringLiteral("product-clean"), QStringLiteral("cyberpunk"), QStringLiteral("vlog-social"),
        QStringLiteral("lifestyle"), QStringLiteral("interview"), QStringLiteral("aerial"),
        QStringLiteral("stage-play"), QStringLiteral("original"), QStringLiteral("fps-game"),
        QStringLiteral("claymation"), QStringLiteral("pixar-3d"), QStringLiteral("disney-classic"),
        QStringLiteral("toon-live"), QStringLiteral("folk-3d"), QStringLiteral("anime"),
        QStringLiteral("vintage-comic"), QStringLiteral("comic-hero"), QStringLiteral("tex-avery"),
        QStringLiteral("watercolor"), QStringLiteral("flat-2d"), QStringLiteral("paper-cutout"),
        QStringLiteral("low-poly-3d"), QStringLiteral("pixel-art"), QStringLiteral("flat-vector"),
        QStringLiteral("infographic"), QStringLiteral("pencil"), QStringLiteral("stick-figure"),
        QStringLiteral("rubber-hose"), QStringLiteral("noir"), QStringLiteral("retro-70s"),
        QStringLiteral("retro-vhs"), QStringLiteral("vhs-90s"), QStringLiteral("news-broadcast"),
        QStringLiteral("weather-graphics"),
    };
    return ids;
}

bool isSha256(QStringView s) {
    if (s.size() != 64) return false;
    for (QChar c : s)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    return true;
}

bool checkRefs(const QJsonValue& v, QStringList* out, QString* error) {
    QStringList items;
    if (v.isArray()) {
        for (const QJsonValue& e : v.toArray()) {
            if (!e.isString()) {
                if (error) *error = QStringLiteral("refs must be a list of artifact sha256s");
                return false;
            }
            items << e.toString().trimmed().toLower();
        }
    } else if (v.isString()) {
        for (const QString& p : v.toString().split(QLatin1Char(','), Qt::SkipEmptyParts))
            if (!p.trimmed().isEmpty()) items << p.trimmed().toLower();
    } else if (!v.isNull() && !v.isUndefined()) {
        if (error) *error = QStringLiteral("refs must be a list of artifact sha256s");
        return false;
    }
    QStringList ids;
    for (const QString& s : items) {
        if (!isSha256(s)) {
            if (error) *error = QStringLiteral("refs: \"%1\" is not an artifact sha256 (64 hex characters)").arg(s.left(70));
            return false;
        }
        if (!ids.contains(s)) ids << s;
    }
    if (ids.size() > kMaxRefs) {
        if (error) *error = QStringLiteral("refs has %1 reference images; at most %2").arg(ids.size()).arg(kMaxRefs);
        return false;
    }
    if (out) *out = ids;
    return true;
}

QString entryHash(const BibleEntry& e) {
    QJsonObject o{
        {QStringLiteral("k"), bibleKindName(e.kind)},
        {QStringLiteral("n"), e.name},
        {QStringLiteral("f"), hashedFields(bibleFields(e.kind), e.data)},
        {QStringLiteral("r"), QJsonArray::fromStringList(e.refs)},
    };
    return QString::fromLatin1(json::stableHash(o));
}

QJsonObject entryJson(const BibleEntry& e) {
    return QJsonObject{
        {QStringLiteral("id"), e.id},
        {QStringLiteral("kind"), bibleKindName(e.kind)},
        {QStringLiteral("name"), e.name},
        {QStringLiteral("data"), e.data},
        {QStringLiteral("refs"), QJsonArray::fromStringList(e.refs)},
        {QStringLiteral("updatedMs"), double(e.updatedMs)},
    };
}

} // namespace studio::film
