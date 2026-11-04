#include "mposd.h"

#include <QHash>
#include <QJsonArray>
#include <algorithm>
#include <cstdio>

namespace mp::osd {

namespace {

Diagnostic diag(int line, int col, const char* code, const QString& message) {
    const QString c = QString::fromLatin1(code);
    return Diagnostic{line, col, diagnosticInfo(c).severity, c, message};
}

// Delphi Graphics.pas colour constants (TColor values, $00BBGGRR) that StringToColor accepts, plus the system colours
// a config is most likely to hold.
const QHash<QString, qint64>& colorNames() {
    static const QHash<QString, qint64> names = {
        {"clblack", 0x000000},   {"clmaroon", 0x000080},  {"clgreen", 0x008000},  {"clolive", 0x008080},
        {"clnavy", 0x800000},    {"clpurple", 0x800080},  {"clteal", 0x808000},   {"clgray", 0x808080},
        {"clsilver", 0xC0C0C0},  {"clred", 0x0000FF},     {"cllime", 0x00FF00},   {"clyellow", 0x00FFFF},
        {"clblue", 0xFF0000},    {"clfuchsia", 0xFF00FF}, {"claqua", 0xFFFF00},   {"clwhite", 0xFFFFFF},
        {"clmoneygreen", 0xC0DCC0}, {"clskyblue", 0xF0CAA6}, {"clcream", 0xF0FBFF}, {"clmedgray", 0xA4A0A0},
        {"clltgray", 0xC0C0C0},  {"cldkgray", 0x808080},  {"clnone", 0x1FFFFFFF}, {"cldefault", 0x20000000},
        {"clwindow", 0xFF000005}, {"clwindowtext", 0xFF000008}, {"clhighlight", 0xFF00000D},
        {"clhighlighttext", 0xFF00000E}, {"clbtnface", 0xFF00000F}, {"clbtntext", 0xFF000012}};
    return names;
}

bool parseIntValue(const QString& text, int* out) {
    const QString t = text.trimmed();
    bool ok = false;
    int v = 0;
    if (t.startsWith(QLatin1Char('$'))) v = t.mid(1).toInt(&ok, 16);
    else if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) v = t.mid(2).toInt(&ok, 16);
    else v = t.toInt(&ok, 10);
    if (ok) *out = v;
    return ok;
}

// Integer key: -1 when absent or empty; MP011 (and -1) when it is not a number.
int intKey(const ini::Section& s, const char* name, QList<Diagnostic>& diags) {
    const ini::Key* k = s.key(QString::fromLatin1(name));
    if (!k || k->value.isEmpty()) return -1;
    int v = -1;
    if (parseIntValue(k->value, &v)) return v;
    diags << diag(k->line, k->col, "MP011", QStringLiteral("%1=%2 is not a number: ignored").arg(k->name, k->value));
    return -1;
}

Color colorKey(const ini::Section& s, const char* name, QList<Diagnostic>& diags) {
    Color c;
    const ini::Key* k = s.key(QString::fromLatin1(name));
    if (!k) return c;
    c.raw = k->value;
    c.present = !k->value.isEmpty();
    if (!c.present) return c;
    if (const auto v = parseTColor(k->value)) {
        c.tcolor = *v;
        c.valid = true;
        c.hex = tColorToHex(*v).value_or(QString());
    } else {
        diags << diag(k->line, k->col, "MP011", QStringLiteral("%1=%2 is not a TColor: ignored").arg(k->name, k->value));
    }
    return c;
}

const QStringList& modelledKeys() {
    static const QStringList keys = {"Type", "XPos", "YPos", "Trans1", "Trans2", "Trans3", "BTrans", "DTrans", "FontName",
                                     "FontSize", "Bold", "FontColor", "BFarbe", "Farbe1", "Farbe2", "TextAlignment",
                                     "TextFieldWidth", "TextFieldHeight", "CrawlTime", "RollTime", "FieldMode",
                                     "OSDFieldMode", "Time", "Text"};
    return keys;
}

QJsonObject place(const char* anchor, int mx, int my) {
    return QJsonObject{{"anchor", anchor}, {"marginX", mx}, {"marginY", my}, {"fromOsd", false}};
}

} // namespace

QString kindName(Kind k) {
    switch (k) {
    case Kind::Logo: return QStringLiteral("logo");
    case Kind::Clock: return QStringLiteral("clock");
    case Kind::Crawl: return QStringLiteral("crawl");
    case Kind::Other: return QStringLiteral("other");
    case Kind::Absent: break;
    }
    return QStringLiteral("absent");
}

Kind kindOfType(int type) {
    switch (type) {
    case -1: return Kind::Absent;
    case 1: return Kind::Logo;
    case 5: return Kind::Clock;
    case 6: return Kind::Crawl;
    default: return Kind::Other;
    }
}

std::optional<QString> tColorToHex(qint64 tcolor) {
    if (tcolor < -2147483648LL || tcolor > 0xFFFFFFFFLL) return std::nullopt;
    const quint32 u = quint32(tcolor & 0xFFFFFFFF);
    if (u == 0x1FFFFFFFu || u == 0x20000000u) return std::nullopt;            // clNone, clDefault
    const quint32 high = u >> 24;
    if (high != 0x00 && high != 0x02) return std::nullopt;                     // system colour, palette index
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", unsigned(u & 0xFF), unsigned((u >> 8) & 0xFF), unsigned((u >> 16) & 0xFF));
    return QString::fromLatin1(buf, 7);
}

std::optional<qint64> hexToTColor(const QString& hex) {
    QString h = hex.trimmed();
    if (h.startsWith(QLatin1Char('#'))) h.remove(0, 1);
    if (h.size() != 6) return std::nullopt;
    bool ok = false;
    const uint rgb = h.toUInt(&ok, 16);
    if (!ok) return std::nullopt;
    const uint r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return qint64((b << 16) | (g << 8) | r);
}

std::optional<qint64> parseTColor(const QString& text) {
    const QString t = text.trimmed();
    if (t.isEmpty()) return std::nullopt;
    if (t.startsWith(QLatin1Char('#'))) return hexToTColor(t);
    const auto named = colorNames().constFind(t.toLower());
    if (named != colorNames().constEnd()) return named.value();
    bool ok = false;
    qint64 v = 0;
    if (t.startsWith(QLatin1Char('$'))) v = t.mid(1).toLongLong(&ok, 16);
    else if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) v = t.mid(2).toLongLong(&ok, 16);
    else v = t.toLongLong(&ok, 10);
    if (!ok || v < -2147483648LL || v > 0xFFFFFFFFLL) return std::nullopt;
    return v;
}

QJsonValue Color::toJson() const {
    if (!present) return QJsonValue::Null;
    if (!hex.isEmpty()) return hex;
    QJsonObject o{{"raw", raw}};
    if (valid) o["tcolor"] = double(tcolor);
    return o;
}

Kind OsdConfig::effectiveKind(bool* fromName) const {
    if (fromName) *fromName = false;
    if (kind != Kind::Absent) return kind;
    const QString n = name.toLower();
    Kind k = Kind::Absent;
    if (n.contains(QLatin1String("countdown")) || n.contains(QLatin1String("countup")) || n.contains(QLatin1String("clock"))) k = Kind::Clock;
    else if (n.contains(QLatin1String("crawl")) || n.contains(QLatin1String("laufschrift"))) k = Kind::Crawl;
    else if (n.contains(QLatin1String("logo"))) k = Kind::Logo;
    if (fromName && k != Kind::Absent) *fromName = true;
    return k;
}

QJsonObject OsdConfig::placement() const {
    switch (effectiveKind()) {
    case Kind::Logo: return place("top_right", 60, 60);
    case Kind::Clock: return place("top_left", 60, 60);
    case Kind::Crawl: return place("bottom", 0, 0);
    default: return place("bottom_left", 120, 90);
    }
}

QJsonObject OsdConfig::toJson() const {
    QJsonObject o{{"name", name}, {"encoding", encodingName(encoding)}, {"hasGlobal", hasGlobal}};
    bool fromName = false;
    const Kind ek = effectiveKind(&fromName);
    o["kind"] = kindName(ek);
    if (fromName) o["kindFromName"] = true;
    auto num = [&](const char* key, int v) { if (v >= 0) o[QString::fromLatin1(key)] = v; };
    num("type", type); num("x", x); num("y", y);
    num("trans1", trans1); num("trans2", trans2); num("trans3", trans3); num("bTrans", bTrans); num("dTrans", dTrans);
    if (!fontName.isEmpty()) o["fontName"] = fontName;
    num("fontSize", fontSize);
    if (bold) o["bold"] = true;
    auto col = [&](const char* key, const Color& c) { if (c.present) o[QString::fromLatin1(key)] = c.toJson(); };
    col("fontColor", fontColor); col("backColor", backColor); col("color1", color1); col("color2", color2);
    num("textAlignment", textAlignment); num("textFieldWidth", textFieldWidth); num("textFieldHeight", textFieldHeight);
    num("crawlTime", crawlTime); num("rollTime", rollTime); num("fieldMode", fieldMode); num("timeSec", timeSec);
    if (!text.isEmpty()) o["text"] = text;
    if (!extra.isEmpty()) {
        QJsonObject e;
        for (const ini::Key& k : extra) if (!e.contains(k.name)) e[k.name] = k.value;
        o["extra"] = e;
    }
    o["placement"] = placement();
    QJsonArray d;
    for (const Diagnostic& x : diagnostics) d.append(x.toJson());
    o["diagnostics"] = d;
    return o;
}

OsdConfig fromSection(const ini::Section& s, const QString& name) {
    OsdConfig c;
    c.name = name.isEmpty() ? s.name : name;
    c.hasGlobal = true;
    QList<Diagnostic>& d = c.diagnostics;
    c.type = intKey(s, "Type", d);
    c.kind = kindOfType(c.type);
    c.x = intKey(s, "XPos", d);
    c.y = intKey(s, "YPos", d);
    c.trans1 = intKey(s, "Trans1", d);
    c.trans2 = intKey(s, "Trans2", d);
    c.trans3 = intKey(s, "Trans3", d);
    c.bTrans = intKey(s, "BTrans", d);
    c.dTrans = intKey(s, "DTrans", d);
    c.fontName = s.value(QStringLiteral("FontName"));
    c.fontSize = intKey(s, "FontSize", d);
    c.bold = intKey(s, "Bold", d) > 0;
    c.fontColor = colorKey(s, "FontColor", d);
    c.backColor = colorKey(s, "BFarbe", d);
    c.color1 = colorKey(s, "Farbe1", d);
    c.color2 = colorKey(s, "Farbe2", d);
    c.textAlignment = intKey(s, "TextAlignment", d);
    c.textFieldWidth = intKey(s, "TextFieldWidth", d);
    c.textFieldHeight = intKey(s, "TextFieldHeight", d);
    c.crawlTime = intKey(s, "CrawlTime", d);
    c.rollTime = intKey(s, "RollTime", d);
    c.fieldMode = intKey(s, "FieldMode", d);
    if (c.fieldMode < 0) c.fieldMode = intKey(s, "OSDFieldMode", d);
    c.timeSec = intKey(s, "Time", d);
    c.text = s.value(QStringLiteral("Text"));
    for (const ini::Key& k : s.keys) {
        const bool modelled = std::any_of(modelledKeys().cbegin(), modelledKeys().cend(),
                                          [&](const QString& m) { return m.compare(k.name, Qt::CaseInsensitive) == 0; });
        if (!modelled) c.extra << k;
    }
    std::stable_sort(d.begin(), d.end(), [](const Diagnostic& a, const Diagnostic& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
    return c;
}

OsdConfig readOsd(const QByteArray& bytes, const QString& name) {
    const ini::IniFile f = ini::readIni(bytes);
    OsdConfig c;
    if (const ini::Section* g = f.section(QStringLiteral("Global"))) {
        c = fromSection(*g, name.isEmpty() ? g->name : name);
    } else {
        c.name = name;
        c.diagnostics << diag(0, 0, "MP011", QStringLiteral("no [Global] section: the OSD defaults apply"));
    }
    c.hasGlobal = f.section(QStringLiteral("Global")) != nullptr;
    c.encoding = f.encoding;
    c.diagnostics = f.diagnostics + c.diagnostics;
    std::stable_sort(c.diagnostics.begin(), c.diagnostics.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
    return c;
}

QString matchOsdName(const QString& cfgPath, const QStringList& names) {
    QString file = cfgPath.trimmed();
    const qsizetype cut = qMax(file.lastIndexOf(QLatin1Char('\\')), file.lastIndexOf(QLatin1Char('/')));
    if (cut >= 0) file = file.mid(cut + 1);
    if (file.isEmpty()) return {};
    for (const QString& n : names) if (n.compare(file, Qt::CaseInsensitive) == 0) return n;
    if (!file.contains(QLatin1Char('.'))) {
        const QString withExt = file + QStringLiteral(".osd");
        for (const QString& n : names) if (n.compare(withExt, Qt::CaseInsensitive) == 0) return n;
    }
    return {};
}

} // namespace mp::osd
