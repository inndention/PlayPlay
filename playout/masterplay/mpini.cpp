#include "mpini.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringDecoder>
#include <algorithm>

namespace mp::ini {

namespace {

// Windows-1252 0x80..0x9F; the five undefined bytes (81 8D 8F 90 9D) map to the C1 control of the same value, as
// MultiByteToWideChar does. The playlist parser and the log reader keep private copies of the same table.
const char16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

QString fromCp1252(const QByteArray& b) {
    QString s(b.size(), Qt::Uninitialized);
    QChar* out = s.data();
    for (qsizetype i = 0; i < b.size(); ++i) {
        const uchar c = uchar(b.at(i));
        out[i] = (c >= 0x80 && c < 0xA0) ? QChar(kCp1252High[c - 0x80]) : QChar(c);
    }
    return s;
}

// Decodes with a strict decoder; *bad = the decoder reported an error (the text then carries U+FFFD there).
QString decodeStrict(QStringConverter::Encoding enc, const QByteArray& b, bool* bad) {
    QStringDecoder dec(enc, QStringConverter::Flag::Stateless);
    const QString s = dec(b);
    *bad = dec.hasError() || ((enc == QStringConverter::Utf16LE || enc == QStringConverter::Utf16BE) && b.size() % 2);
    return s;
}

Diagnostic diag(int line, int col, const char* code, const QString& message) {
    const QString c = QString::fromLatin1(code);
    return Diagnostic{line, col, diagnosticInfo(c).severity, c, message};
}

void sortDiagnostics(QList<Diagnostic>& ds) {
    std::stable_sort(ds.begin(), ds.end(), [](const Diagnostic& a, const Diagnostic& b) {
        return a.line != b.line ? a.line < b.line : a.col < b.col;
    });
}

bool isBlank(QChar c) { return c == QLatin1Char(' ') || c == QLatin1Char('\t'); }

// Spaces and TABs only: QString::trimmed() would also eat U+00A0 (cp1252 0xA0), which Windows keeps.
QString trimBlanks(const QString& s, qsizetype* lead = nullptr) {
    qsizetype a = 0, b = s.size();
    while (a < b && isBlank(s.at(a))) ++a;
    while (b > a && isBlank(s.at(b - 1))) --b;
    if (lead) *lead = a;
    return s.mid(a, b - a);
}

QString unquote(const QString& v) {
    if (v.size() >= 2 && (v.front() == QLatin1Char('"') || v.front() == QLatin1Char('\'')) && v.back() == v.front())
        return v.mid(1, v.size() - 2);
    return v;
}

// Lines split on CRLF, LF or a lone CR (TStringList and the profile API both break there). The empty piece after a
// final terminator is no line.
QStringList splitLines(const QString& text) {
    QStringList lines;
    qsizetype start = 0;
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\n') && c != QLatin1Char('\r')) continue;
        lines << text.mid(start, i - start);
        if (c == QLatin1Char('\r') && i + 1 < n && text.at(i + 1) == QLatin1Char('\n')) ++i;
        start = i + 1;
    }
    if (start < n) lines << text.mid(start);
    return lines;
}

int lineOfOffset(const QString& text, qsizetype at) {   // 1-based line holding text[at], same breaks as splitLines
    int line = 1;
    for (qsizetype i = 0; i < at && i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) continue;
        if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) ++line;
    }
    return line;
}

int parseInt(const QString& text, bool* ok) {
    const QString t = trimBlanks(text);
    bool good = false;
    int v = 0;
    if (t.startsWith(QLatin1Char('$'))) v = t.mid(1).toInt(&good, 16);                       // Delphi StrToInt hex
    else if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) v = t.mid(2).toInt(&good, 16);
    else v = t.toInt(&good, 10);
    if (ok) *ok = good;
    return good ? v : 0;
}

double parseDouble(const QString& text, bool* ok) {
    const QString t = trimBlanks(text);
    bool good = false;
    double v = t.toDouble(&good);
    if (!good) v = QString(t).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&good);   // German decimal comma
    if (ok) *ok = good;
    return good ? v : 0;
}

// "hh:mm" / "h:mm" / "hh:mm:ss"
QTime parseClock(const QString& text) {
    const QStringList p = trimBlanks(text).split(QLatin1Char(':'));
    if (p.size() < 2 || p.size() > 3) return {};
    int v[3] = {0, 0, 0};
    for (int i = 0; i < p.size(); ++i) {
        bool ok = false;
        v[i] = p[i].toInt(&ok);
        if (!ok || p[i].isEmpty()) return {};
    }
    return QTime(v[0], v[1], v[2]);
}

QString stem(const QString& path) {   // normalised, trailing separators dropped ("D:\" -> "D:")
    QString s = normalizePath(path);
    while (s.endsWith(QLatin1Char('\\'))) s.chop(1);
    return s;
}

QString withSlash(const QString& path) {
    if (path.isEmpty()) return path;
    QString s = normalizePath(path);
    if (!s.endsWith(QLatin1Char('\\'))) s += QLatin1Char('\\');
    return s;
}

QString parentDir(const QString& path) {   // "D:\Masterplay\Playlist" -> "D:\Masterplay"; "D:\Playlist" -> "D:\"
    const QString s = stem(path);
    const qsizetype at = s.lastIndexOf(QLatin1Char('\\'));
    if (at < 0) return {};
    const QString p = s.left(at);
    if (p.size() == 2 && p.at(1) == QLatin1Char(':')) return p + QLatin1Char('\\');
    if (p.isEmpty() || p == QLatin1String("\\")) return {};
    return p;
}

QString joinPath(const QString& dir, const QString& file) {
    if (dir.isEmpty()) return {};
    if (file.isEmpty()) return normalizePath(dir);
    return withSlash(dir) + file;
}

bool isAbsolutePath(const QString& p) {
    const QString s = normalizePath(trimBlanks(p));
    return s.startsWith(QLatin1String("\\\\")) || (s.size() >= 3 && s.at(0).isLetter() && s.at(1) == QLatin1Char(':') && s.at(2) == QLatin1Char('\\'));
}

QString uncHost(const QString& path) {   // "\\host\share\..." -> "host"
    const QString s = normalizePath(path);
    if (!s.startsWith(QLatin1String("\\\\")) || s.startsWith(QLatin1String("\\\\?\\")) || s.startsWith(QLatin1String("\\\\.\\"))) return {};
    const qsizetype end = s.indexOf(QLatin1Char('\\'), 2);
    return end < 0 ? s.mid(2) : s.mid(2, end - 2);
}

QString lastPart(const QString& path) {   // "\\nas\configs\mp" -> "mp"; "D:\Masterplay\" -> "Masterplay"
    const QString s = stem(path);
    return s.mid(s.lastIndexOf(QLatin1Char('\\')) + 1);
}

// List values are file text of any length (a pasted or corrupt INI reaches the REST route): past kMaxListItems items
// the rest is dropped with one MP011 at the key, so the subset stays small and the path mapping over it (paths x rules)
// stays cheap. Real lists hold a handful of entries.
constexpr int kMaxListItems = 256;

QStringList capped(QStringList items, const Key* k, QList<Diagnostic>* diags, int max = kMaxListItems) {
    if (items.size() <= max) return items;
    if (diags && k)   // one arg() pass: the key name is file text
        *diags << diag(k->line, k->col, "MP011", QStringLiteral("%1 holds %2 items: only the first %3 are read").arg(k->name, QString::number(items.size()), QString::number(max)));
    items.resize(max);
    return items;
}

// "*.mpg;*.M2P" / ".txt;.jpg" -> [".mpg", ".m2p"]; de-duplicated through a set (the item count is file text).
QStringList extensions(const Key* k, QList<Diagnostic>* diags) {
    QStringList out;
    if (!k) return out;
    QSet<QString> seen;
    for (QString e : capped(splitList(k->value), k, diags)) {
        while (e.startsWith(QLatin1Char('*'))) e.remove(0, 1);
        if (e.isEmpty()) continue;
        if (!e.startsWith(QLatin1Char('.'))) e.prepend(QLatin1Char('.'));
        e = e.toLower();
        if (seen.contains(e)) continue;
        seen.insert(e);
        out << e;
    }
    return out;
}

QChar driveLetter(const Key* k, QList<Diagnostic>* diags) {
    if (!k) return {};
    QString v = k->value;
    if (v.endsWith(QLatin1Char('\\'))) v.chop(1);
    if (v.endsWith(QLatin1Char(':'))) v.chop(1);
    if (v.isEmpty()) return {};
    if (v.size() == 1 && v.at(0).unicode() < 0x80 && v.at(0).isLetter()) return v.at(0).toUpper();
    if (diags) *diags << diag(k->line, k->col, "MP011", QStringLiteral("%1=%2 is no drive letter (A to Z): ignored").arg(k->name, k->value));
    return {};
}

// ExchangeList: items are "editor;playlist" pairs. Items come from CommaText (quotes), '|' or ',' separators; a value
// that is only ';'-separated tokens is read pairwise.
QList<PathPair> exchangePairs(const Key* k, QList<Diagnostic>* diags) {
    QList<PathPair> out;
    if (!k || k->value.isEmpty()) return out;
    const QString v = k->value;
    QStringList items;
    if (v.contains(QLatin1Char('"'))) items = splitList(v);
    else if (v.contains(QLatin1Char('|'))) items = v.split(QLatin1Char('|'));
    else if (v.contains(QLatin1Char(','))) items = v.split(QLatin1Char(','));
    else items << v;
    QStringList tokens;
    for (const QString& item : items) {
        QStringList parts = trimBlanks(item).split(QLatin1Char(';'));
        for (QString& p : parts) p = trimBlanks(p);
        parts.removeAll(QString());
        tokens << parts;
    }
    if (tokens.size() > 2 * kMaxListItems) {   // every pair becomes a path rule
        if (diags) *diags << diag(k->line, k->col, "MP011", QStringLiteral("ExchangeList holds %1 paths: only the first %2 pairs are read").arg(tokens.size()).arg(kMaxListItems));
        tokens.resize(2 * kMaxListItems);
    } else if (tokens.size() % 2) {
        if (diags) *diags << diag(k->line, k->col, "MP011", QStringLiteral("ExchangeList has an odd number of paths (%1): the last one is ignored").arg(tokens.size()));
        tokens.removeLast();
    }
    for (qsizetype i = 0; i + 1 < tokens.size(); i += 2) out << PathPair{tokens.at(i), tokens.at(i + 1)};
    return out;
}

QJsonArray toArray(const QStringList& l) {
    QJsonArray a;
    for (const QString& s : l) a.append(s);
    return a;
}

QString slug(const QString& s) {
    QString o;
    for (const QChar c : s.toLower()) {
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) o += c;
        else if (!o.endsWith(QLatin1Char('-'))) o += QLatin1Char('-');
    }
    while (o.endsWith(QLatin1Char('-'))) o.chop(1);
    while (o.startsWith(QLatin1Char('-'))) o.remove(0, 1);
    return o;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Profile reader

const Key* Section::key(const QString& n) const {
    for (const Key& k : keys) if (k.name.compare(n, Qt::CaseInsensitive) == 0) return &k;
    return nullptr;
}

QString Section::value(const QString& n, const QString& def) const {
    const Key* k = key(n);
    return k ? k->value : def;
}

const Section* IniFile::section(const QString& name) const {
    for (const Section& s : sections) if (s.name.compare(name, Qt::CaseInsensitive) == 0) return &s;
    return nullptr;
}

QString IniFile::value(const QString& sec, const QString& key, const QString& def) const {
    const Section* s = section(sec);
    return s ? s->value(key, def) : def;
}

bool IniFile::has(const QString& sec, const QString& key) const {
    const Section* s = section(sec);
    return s && s->has(key);
}

int IniFile::intValue(const QString& sec, const QString& key, int def, bool* ok) const {
    bool good = false;
    const int v = has(sec, key) ? parseInt(value(sec, key), &good) : 0;
    if (ok) *ok = good;
    return good ? v : def;
}

bool IniFile::boolValue(const QString& sec, const QString& key, bool def) const {
    const QString v = value(sec, key).toLower();
    if (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes") || v == QLatin1String("on")) return true;
    if (v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("no") || v == QLatin1String("off")) return false;
    bool ok = false;
    const int n = parseInt(v, &ok);
    return ok ? n != 0 : def;
}

QStringList IniFile::sectionNames() const {
    QStringList out;
    for (const Section& s : sections) out << s.name;
    return out;
}

IniFile readIni(const QByteArray& bytes) {
    IniFile f;
    QString text;
    bool bad = false;
    if (bytes.startsWith("\xEF\xBB\xBF")) { f.encoding = Encoding::Utf8Bom; text = decodeStrict(QStringConverter::Utf8, bytes.mid(3), &bad); }
    else if (bytes.startsWith("\xFF\xFE")) { f.encoding = Encoding::Utf16LE; text = decodeStrict(QStringConverter::Utf16LE, bytes.mid(2), &bad); }
    else if (bytes.startsWith("\xFE\xFF")) { f.encoding = Encoding::Utf16BE; text = decodeStrict(QStringConverter::Utf16BE, bytes.mid(2), &bad); }
    else {
        bool notUtf8 = false;
        text = decodeStrict(QStringConverter::Utf8, bytes, &notUtf8);
        if (notUtf8) { f.encoding = Encoding::Ansi1252; text = fromCp1252(bytes); }
        else f.encoding = Encoding::Utf8;
    }
    const QStringList lines = splitLines(text);
    f.physicalLines = int(lines.size());
    if (bad) {   // at the first U+FFFD, else (a lone trailing UTF-16 byte) the last line
        const qsizetype at = text.indexOf(QChar(0xFFFD));
        f.diagnostics << diag(at < 0 ? qMax(1, f.physicalLines) : lineOfOffset(text, at), 0, "MP010",
                              QStringLiteral("bytes not valid %1: shown as U+FFFD").arg(encodingName(f.encoding)));
    }

    Section* cur = nullptr;
    for (int i = 0; i < lines.size(); ++i) {
        const int ln = i + 1;
        qsizetype lead = 0;
        const QString t = trimBlanks(lines.at(i), &lead);
        if (t.isEmpty()) { ++f.blank; continue; }
        if (t.startsWith(QLatin1Char(';'))) { ++f.comments; continue; }
        if (t.startsWith(QLatin1Char('['))) {
            const qsizetype close = t.indexOf(QLatin1Char(']'));
            if (close < 0) {
                ++f.ignored;
                f.diagnostics << diag(ln, int(lead) + 1, "MP001", QStringLiteral("section header without ']': ignored"));
                continue;
            }
            Section s;
            s.name = trimBlanks(t.mid(1, close - 1));
            s.line = ln;
            f.sections << s;
            cur = &f.sections.last();
            continue;
        }
        const qsizetype eq = t.indexOf(QLatin1Char('='));
        if (eq < 0) {
            ++f.ignored;
            f.diagnostics << diag(ln, int(lead) + 1, "MP001", QStringLiteral("no '=' and no [section]: ignored"));
            continue;
        }
        Key k;
        k.name = trimBlanks(t.left(eq));
        k.line = ln;
        const qsizetype rawAt = lead + eq + 1;          // the raw value runs from after '=' to the end of the line
        k.raw = lines.at(i).mid(rawAt);
        k.col = int(rawAt) + 1;
        k.value = unquote(trimBlanks(k.raw));
        if (k.name.isEmpty()) {
            ++f.ignored;
            f.diagnostics << diag(ln, int(lead) + 1, "MP001", QStringLiteral("key without a name: ignored"));
            continue;
        }
        if (!cur) {
            ++f.ignored;
            f.diagnostics << diag(ln, int(lead) + 1, "MP001", QStringLiteral("%1= before the first [section]: ignored, as Windows does").arg(k.name));
            continue;
        }
        cur->keys << k;
    }
    sortDiagnostics(f.diagnostics);
    return f;
}

QStringList splitList(const QString& value) {
    QStringList out;
    const QString v = trimBlanks(value);
    if (v.isEmpty()) return out;
    if (v.contains(QLatin1Char('"'))) {
        // Delphi TStrings.SetDelimitedText with Delimiter ',' and QuoteChar '"' (CommaText): blanks and control
        // characters outside quotes separate too; "" inside quotes is one quote.
        const qsizetype n = v.size();
        qsizetype i = 0;
        auto isSep = [](QChar c) { return c.unicode() <= ' '; };
        while (i < n && isSep(v.at(i))) ++i;
        while (i < n) {
            QString item;
            if (v.at(i) == QLatin1Char('"')) {
                ++i;
                while (i < n) {
                    if (v.at(i) == QLatin1Char('"')) {
                        if (i + 1 < n && v.at(i + 1) == QLatin1Char('"')) { item += QLatin1Char('"'); i += 2; continue; }
                        ++i;
                        break;
                    }
                    item += v.at(i++);
                }
            } else {
                const qsizetype a = i;
                while (i < n && !isSep(v.at(i)) && v.at(i) != QLatin1Char(',')) ++i;
                item = v.mid(a, i - a);
            }
            if (!item.isEmpty()) out << item;
            while (i < n && isSep(v.at(i))) ++i;
            if (i < n && v.at(i) == QLatin1Char(',')) {
                ++i;
                while (i < n && isSep(v.at(i))) ++i;
            }
        }
        return out;
    }
    const bool semi = v.contains(QLatin1Char(';')) || v.contains(QLatin1Char('|'));
    qsizetype start = 0;
    for (qsizetype i = 0; i <= v.size(); ++i) {
        const bool end = i == v.size();
        const QChar c = end ? QChar() : v.at(i);
        if (!end && (semi ? (c != QLatin1Char(';') && c != QLatin1Char('|')) : c != QLatin1Char(','))) continue;
        const QString t = trimBlanks(v.mid(start, i - start));
        if (!t.isEmpty()) out << t;
        start = i + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mplay.INI subset

QString exchangeModeName(ExchangeMode m) {
    switch (m) {
    case ExchangeMode::Disabled: return QStringLiteral("disabled");
    case ExchangeMode::Enabled: return QStringLiteral("enabled");
    case ExchangeMode::Automatic: return QStringLiteral("automatic");
    case ExchangeMode::Absent: break;
    }
    return QStringLiteral("absent");
}

int MplayIni::suggestedFps() const {
    if (editorFrameRate > 0) return qMax(1, qRound(editorFrameRate));
    if (editorFramesPerSecond > 0) return editorFramesPerSecond;
    return 30;
}

QJsonObject MplayIni::toJson() const {
    QJsonObject player{
        {"iniVersion", iniVersion}, {"playlistDir", playlistDir}, {"logDir", logDir}, {"osdDir", osdDir}, {"tempDir", tempDir},
        {"asRunLog", asRunLog}, {"statusLog", statusLog}, {"operatorLog", operatorLog},
        {"asRunWithDirectories", asRunWithDirectories}, {"asRunAddInfoFields", asRunAddInfoFields},
        {"asRunIsoDateTime", asRunIsoDateTime}, {"asRunNaming", asRunNaming}, {"statusLogNaming", statusLogNaming},
        {"searchPaths", toArray(searchPaths)}, {"searchIncludeSubDirs", searchIncludeSubDirs},
        {"searchUseWildChar", searchUseWildChar}, {"searchNewProgInSearchPaths", searchNewProgInSearchPaths},
        {"searchExcludedExtensions", toArray(searchExcludedExtensions)},
        {"videoExtensions", toArray(videoExtensions)}, {"videoOnlyExtensions", toArray(videoOnlyExtensions)},
        {"audioExtensions", toArray(audioExtensions)}, {"dailyPlaylistMode", dailyPlaylistMode},
        {"autoReloadPlaylist", autoReloadPlaylist}, {"loadPassedRepeatLoop", loadPassedRepeatLoop},
        {"eventsSamePriority", eventsSamePriority}, {"osdClearEndOfExact", osdClearEndOfExact},
        {"stationName", stationName}};
    QJsonArray subs;
    for (const Substitute& s : substitutes) subs.append(QJsonObject{{"name", s.name}, {"value", s.value}});
    player["substitutes"] = subs;
    if (loadPassedDays >= 0) player["loadPassedDays"] = loadPassedDays;
    if (defPictureTimeSec >= 0) player["defPictureTimeSec"] = defPictureTimeSec;
    if (defaultFrameRate > 0) player["defaultFrameRate"] = defaultFrameRate;

    QJsonArray pairs;
    for (const PathPair& p : exchangeList) pairs.append(QJsonObject{{"editor", p.editor}, {"playlist", p.playlist}});
    QJsonObject editor{
        {"isoDateTimes", isoDateTimes}, {"exchangeDriveLetters", exchangeModeName(exchangeMode)},
        {"localDriveLetter", localDriveLetter.isNull() ? QString() : QString(localDriveLetter)},
        {"mappedDriveLetter", mappedDriveLetter.isNull() ? QString() : QString(mappedDriveLetter)},
        {"exchangeList", pairs}, {"generateUuids", generateUuids}, {"templateDir", templateDir},
        {"commandListsDir", commandListsDir}, {"defaultSelectionDir", defaultSelectionDir},
        {"dayChange", dayChange.isValid() ? dayChange.toString(QStringLiteral("HH:mm")) : QString()}};
    if (editorFramesPerSecond > 0) editor["framesPerSecond"] = editorFramesPerSecond;
    if (editorFrameRate > 0) editor["frameRate"] = editorFrameRate;

    QJsonArray diags;
    for (const Diagnostic& d : diagnostics) diags.append(d.toJson());
    return QJsonObject{
        {"encoding", encodingName(encoding)}, {"isMplayIni", isMplayIni}, {"player", player}, {"editor", editor},
        {"onAir", QJsonObject{{"lastPlaylist", lastPlaylist}, {"startupPlaylist", startupPlaylist}, {"recentPlaylists", toArray(recentPlaylists)}}},
        {"output", QJsonObject{{"format", outputFormat}, {"device", outputDevice}, {"channelName", channelName}, {"ndi", outputNdi}}},
        {"installDir", installDir}, {"suggestedFps", suggestedFps()}, {"diagnostics", diags}};
}

MplayIni readMplayIni(const QByteArray& bytes) { return readMplayIni(readIni(bytes)); }

MplayIni readMplayIni(const IniFile& f) {
    MplayIni m;
    m.encoding = f.encoding;
    m.diagnostics = f.diagnostics;
    const QString P = QStringLiteral("MPEGPlay"), E = QStringLiteral("PlayListEditor");
    m.isMplayIni = f.section(P) || f.section(E);
    auto str = [&](const QString& sec, const char* key) { return f.value(sec, QString::fromLatin1(key)); };
    auto either = [&](const char* key) {   // the editor's value first, the player's when the editor has none
        const QString k = QString::fromLatin1(key);
        return f.has(E, k) && !f.value(E, k).isEmpty() ? f.value(E, k) : f.value(P, k);
    };
    auto flag = [&](const QString& sec, const char* key, bool def) { return f.boolValue(sec, QString::fromLatin1(key), def); };
    auto keyOf = [&](const QString& sec, const char* key) -> const Key* {
        const Section* s = f.section(sec);
        return s ? s->key(QString::fromLatin1(key)) : nullptr;
    };

    // [MPEGPlay]
    m.iniVersion = str(P, "Version");
    m.playlistDir = str(P, "InFilesDirectory");
    if (m.playlistDir.isEmpty()) m.playlistDir = str(E, "InFilesDirectory");
    m.logDir = str(P, "LogFilesDirectory");
    m.osdDir = str(P, "OSDFilesDirectory");
    m.tempDir = str(P, "TempPfad");
    if (!str(P, "LogFile").isEmpty()) m.asRunLog = str(P, "LogFile");
    if (!str(P, "StatusFile").isEmpty()) m.statusLog = str(P, "StatusFile");
    if (!str(P, "OperatorLogFilename").isEmpty()) m.operatorLog = str(P, "OperatorLogFilename");
    m.asRunWithDirectories = flag(P, "AsRunLogWithDirectories", true);
    m.asRunAddInfoFields = flag(P, "AsRunLogAddInfoFields", true);
    m.asRunIsoDateTime = flag(P, "AsRunLogIsoDateTime", true);
    m.asRunNaming = f.intValue(P, QStringLiteral("NamingAsRun"), 0);
    m.statusLogNaming = f.intValue(P, QStringLiteral("NamingStatusLog"), 0);
    // Search paths de-duplicated case-insensitively by stem through a set (not a rescan per item: the count is file
    // text), at most kMaxListItems in all, with one MP011 at the key that overflowed.
    QSet<QString> seenPaths;
    bool pathsFull = false;
    auto addSearch = [&](const QString& p, const Key* k) {
        const QString s = stem(p).toCaseFolded();
        if (s.isEmpty() || seenPaths.contains(s)) return;
        if (m.searchPaths.size() >= kMaxListItems) {
            if (!pathsFull) m.diagnostics << diag(k->line, k->col, "MP011", QStringLiteral("more than %1 search paths: the rest are ignored").arg(kMaxListItems));
            pathsFull = true;
            return;
        }
        seenPaths.insert(s);
        m.searchPaths << normalizePath(p);
    };
    for (const QString& sec : {P, E})
        if (const Key* k = keyOf(sec, "SuchPfade"))
            for (const QString& p : capped(splitList(k->value), k, &m.diagnostics)) addSearch(p, k);
    // [SearchDirs] [UNVERIFIED section]: one path per key (Dir0=E:\More), or a list in one value (SuchPfade=D:\A;E:\B:
    // the names sit together in one string table of both binaries). A value splits only when every item is an absolute
    // path, so one path holding ',' stays whole; Count=2 and relative values are skipped.
    if (const Section* sd = f.section(QStringLiteral("SearchDirs")))
        for (const Key& k : sd->keys) {
            if (pathsFull) break;
            const QStringList items = splitList(k.value);
            if (items.size() > 1 && std::all_of(items.begin(), items.end(), [](const QString& p) { return isAbsolutePath(p); }))
                for (const QString& p : capped(items, &k, &m.diagnostics)) addSearch(p, &k);
            else if (isAbsolutePath(k.value)) addSearch(k.value, &k);
        }
    m.searchIncludeSubDirs = flag(P, "SearchDirsIncludeSubDirs", false);
    m.searchUseWildChar = flag(P, "SearchDirsUseWildChar", false);
    m.searchNewProgInSearchPaths = flag(P, "SearchNewProgInSearchPaths", false);
    m.searchExcludedExtensions = extensions(keyOf(P, "SearchDirsExcludedExtensions"), &m.diagnostics);
    if (const Section* ps = f.section(P)) {
        if (const Key* k = ps->key(QStringLiteral("SubstituteTable"))) {
            for (const QString& item : capped(splitList(k->value), k, &m.diagnostics)) {
                const qsizetype eq = item.indexOf(QLatin1Char('='));
                if (eq <= 0) {
                    m.diagnostics << diag(k->line, k->col, "MP011", QStringLiteral("SubstituteTable item \"%1\" has no name=value: ignored").arg(item));
                    continue;
                }
                QString name = trimBlanks(item.left(eq));
                if (name.size() >= 2 && name.startsWith(QLatin1Char('%')) && name.endsWith(QLatin1Char('%'))) name = name.mid(1, name.size() - 2);
                m.substitutes << Substitute{name, trimBlanks(item.mid(eq + 1))};
            }
        }
    }
    m.videoExtensions = extensions(keyOf(P, "VideoFileExtensions"), &m.diagnostics);
    m.videoOnlyExtensions = extensions(keyOf(P, "VideoOnlyFileExtensions"), &m.diagnostics);
    m.audioExtensions = extensions(keyOf(P, "AudioFileExtensions"), &m.diagnostics);
    m.dailyPlaylistMode = flag(P, "DailyPlaylistMode", false) || flag(E, "DailyPlaylistMode", false);
    m.autoReloadPlaylist = flag(P, "AutoReloadPL", false);
    m.loadPassedRepeatLoop = flag(P, "LoadPassedRepeatLoop", false);
    m.eventsSamePriority = flag(P, "EventsDoesHaveSamePriority", false);
    m.osdClearEndOfExact = flag(P, "OSDClearEOForExactEvent", false);
    m.loadPassedDays = f.intValue(P, QStringLiteral("LoadPassedDays"), -1);
    bool ok = false;
    const double pic = parseDouble(either("DefPictureTime"), &ok);
    if (ok && pic >= 0) m.defPictureTimeSec = pic;
    const double dfr = parseDouble(str(P, "DefaultFrameRate"), &ok);
    if (ok && dfr > 0) m.defaultFrameRate = dfr;
    m.stationName = str(P, "KopfStellenName");
    if (m.stationName.compare(QLatin1String("Noch kein Name"), Qt::CaseInsensitive) == 0) m.stationName.clear();   // "no name yet"

    // [PlayListEditor]
    m.isoDateTimes = flag(E, "ISODateTimes", true);
    m.editorFramesPerSecond = f.intValue(E, QStringLiteral("AnzFramesPerSecound"), -1);
    const double efr = parseDouble(str(E, "Videoformat"), &ok);
    if (ok && efr > 0) m.editorFrameRate = efr;
    m.dayChange = parseClock(either("TagesWechsel"));
    if (const Section* es = f.section(E)) {
        bool good = false;
        const int mode = es->has(QStringLiteral("ExchangeDriveLetters")) ? parseInt(es->value(QStringLiteral("ExchangeDriveLetters")), &good) : -1;
        if (good && mode >= 0 && mode <= 2) m.exchangeMode = ExchangeMode(mode);
        else if (es->has(QStringLiteral("ExchangeDriveLetters"))) {
            const Key* k = es->key(QStringLiteral("ExchangeDriveLetters"));
            m.diagnostics << diag(k->line, k->col, "MP011", QStringLiteral("ExchangeDriveLetters=%1 is not 0, 1 or 2: treated as absent").arg(k->value));
        }
        m.localDriveLetter = driveLetter(es->key(QStringLiteral("MasterPlayLocalDriveLetter")), &m.diagnostics);
        m.mappedDriveLetter = driveLetter(es->key(QStringLiteral("MappedDriveLetter")), &m.diagnostics);
        m.exchangeList = exchangePairs(es->key(QStringLiteral("ExchangeList")), &m.diagnostics);
    }
    m.generateUuids = flag(E, "GenerateUUIDsForItems", false);
    m.templateDir = str(E, "TemplateDirectory");
    m.commandListsDir = str(E, "CommandListsDirectory");
    m.defaultSelectionDir = str(E, "DefaultSelectionDirectory");

    // [PlayCart0] / [LastUsedOnAirPlaylists0]
    m.lastPlaylist = f.value(QStringLiteral("PlayCart0"), QStringLiteral("LastInFilename"));
    m.startupPlaylist = f.value(QStringLiteral("PlayCart0"), QStringLiteral("OnApplicationStartAlwaysLoadThisPlaylist"));
    if (const Section* lu = f.section(QStringLiteral("LastUsedOnAirPlaylists0"))) {
        // One pass over the keys instead of a loop to ItemCount: the count is file text, and a corrupt ItemCount=2000000000
        // (or $7FFFFFFF) must not become 2e9 lookups on the thread serving the REST route. Item<n> is the name "Item%1"
        // gives (no sign, no leading zero, any case); the first of a repeated name wins, as in every lookup here.
        QMap<int, const Key*> items;
        for (const Key& k : lu->keys) {
            if (!k.name.startsWith(QLatin1String("Item"), Qt::CaseInsensitive)) continue;
            const QStringView digits = QStringView(k.name).mid(4);
            bool num = false;
            const int n = digits.toInt(&num);
            if (num && n >= 0 && digits == QString::number(n) && !items.contains(n)) items.insert(n, &k);
        }
        bool counted = false;
        const int count = lu->has(QStringLiteral("ItemCount")) ? parseInt(lu->value(QStringLiteral("ItemCount")), &counted) : 0;
        int next = 0;   // without ItemCount: Item0, Item1 ... up to the first gap
        for (auto it = items.cbegin(); it != items.cend(); ++it) {
            if (counted ? it.key() >= count : it.key() != next++) break;
            if (it.value()->value.isEmpty()) continue;
            if (m.recentPlaylists.size() >= kMaxListItems) {
                m.diagnostics << diag(it.value()->line, it.value()->col, "MP011", QStringLiteral("more than %1 recent playlists: the rest are ignored").arg(kMaxListItems));
                break;
            }
            m.recentPlaylists << it.value()->value;
        }
    }

    // [Medialooks 1]
    const QString ML = QStringLiteral("Medialooks 1");
    m.outputFormat = f.value(ML, QStringLiteral("Format"));
    m.outputDevice = f.value(ML, QStringLiteral("OutputDevice"));
    m.channelName = f.value(ML, QStringLiteral("ChannelName"));
    m.outputNdi = f.boolValue(ML, QStringLiteral("OutputNDI"), false);

    // The MasterPlay folder: MasterPlay creates Playlist / Logfiles / Osd / Temp ... under it, and a few keys default to
    // it. A vote, so one folder moved elsewhere does not move the answer.
    QStringList votes;
    for (const QString& d : {m.playlistDir, m.logDir, m.osdDir, m.tempDir, str(E, "InFilesDirectory"), str(E, "TempPfad"), m.templateDir, m.commandListsDir})
        if (isAbsolutePath(d)) votes << parentDir(d);
    for (const QString& d : {m.defaultSelectionDir, str(P, "BackgroundDir")})
        if (isAbsolutePath(d)) votes << stem(d) + (stem(d).size() == 2 ? QStringLiteral("\\") : QString());
    QHash<QString, int> count;
    int best = 0;
    for (const QString& v : votes) {
        if (v.isEmpty()) continue;
        const int c = ++count[v.toLower()];
        if (c > best) best = c;
    }
    for (const QString& v : votes) if (!v.isEmpty() && count.value(v.toLower()) == best) { m.installDir = v; break; }

    sortDiagnostics(m.diagnostics);
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// Path rules

QJsonObject PathRule::toJson() const {
    QJsonObject o{{"from", from}, {"to", to}, {"origin", origin}};
    if (!note.isEmpty()) o["note"] = note;
    if (guess) o["guess"] = true;
    return o;
}

PathRule PathRule::fromJson(const QJsonObject& o) {
    PathRule r;
    r.from = o.value("from").toString();
    r.to = o.value("to").toString();
    r.origin = o.value("origin").toString(QStringLiteral("user"));
    r.note = o.value("note").toString();
    r.guess = o.value("guess").toBool(false);
    return r;
}

QString normalizePath(const QString& path) {
    QString s = path;
    s.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const bool unc = s.startsWith(QLatin1String("\\\\"));
    QString out = unc ? QStringLiteral("\\\\") : QString();
    out.reserve(s.size());
    for (qsizetype i = unc ? 2 : 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\') && out.endsWith(QLatin1Char('\\'))) continue;   // runs collapse; "\\\host" -> "\\host"
        out += c;
    }
    if (out.size() == 2 && out.at(0).isLetter() && out.at(1) == QLatin1Char(':')) out += QLatin1Char('\\');
    return out;
}

QString mapPath(const QString& path, const QList<PathRule>& rules, int* used) {
    if (used) *used = -1;
    const QString p = normalizePath(path);
    int best = -1;
    qsizetype bestLen = -1;
    for (int i = 0; i < rules.size(); ++i) {
        if (rules[i].to.isEmpty()) continue;
        const QString from = stem(rules[i].from);
        if (from.isEmpty() || from.size() <= bestLen || p.size() < from.size()) continue;
        if (!p.startsWith(from, Qt::CaseInsensitive)) continue;
        if (p.size() != from.size() && p.at(from.size()) != QLatin1Char('\\')) continue;   // whole components only
        best = i;
        bestLen = from.size();
    }
    if (best < 0) return path;
    if (used) *used = best;
    const QString to = stem(rules[best].to);
    const QString rest = p.mid(bestLen);                     // "" (the rule's own folder) or "\..."
    if (rest.isEmpty()) return to.size() == 2 && to.at(1) == QLatin1Char(':') ? to + QLatin1Char('\\') : to;
    return to + rest;
}

QList<PathRule> normalizeRules(const QList<PathRule>& rules) {
    QList<PathRule> out;
    QSet<QString> seen;   // case-folded stems; a set, not a rescan per rule: ExchangeList pairs come from the INI
    for (PathRule r : rules) {
        const QString key = stem(r.from).toCaseFolded();
        if (key.isEmpty() || seen.contains(key)) continue;
        seen.insert(key);
        r.from = withSlash(r.from);
        r.to = withSlash(r.to);
        out << r;
    }
    std::stable_sort(out.begin(), out.end(), [](const PathRule& a, const PathRule& b) { return stem(a.from).size() > stem(b.from).size(); });
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Source suggestion

QJsonObject SourceSuggestion::toJson(const MplayIni& ini) const {
    QJsonArray rules;
    for (const PathRule& r : pathMap) rules.append(r.toJson());
    QJsonArray search;
    for (const QString& p : ini.searchPaths) search.append(QJsonObject{{"path", p}, {"mapped", normalizePath(mapPath(p, pathMap))}});
    QJsonArray subs;
    for (const Substitute& s : ini.substitutes) subs.append(QJsonObject{{"name", s.name}, {"value", s.value}, {"mapped", mapPath(s.value, pathMap)}});
    QJsonObject defaults{{"fps", ini.suggestedFps()}};
    if (ini.defPictureTimeSec >= 0) defaults["defPictureTimeSec"] = ini.defPictureTimeSec;
    return QJsonObject{{"source", source}, {"pathMap", rules}, {"searchPaths", search}, {"substitutes", subs},
                       {"defaults", defaults}, {"notes", toArray(notes)}, {"ini", ini.toJson()}};
}

SourceSuggestion suggestSource(const MplayIni& ini, const SuggestOptions& o) {
    SourceSuggestion s;
    QList<PathRule> rules;
    const QString host = uncHost(o.iniPath);
    if (!ini.isMplayIni) s.notes << QStringLiteral("No [MPEGPlay] or [PlayListEditor] section: this is not an Mplay.INI; the suggestion is empty.");

    // 1. The editor's own path pairs: a playlist path becomes the editor's path when loading, and this station reads
    // playlists the way the editor loads them.
    for (const PathPair& p : ini.exchangeList) {
        rules << PathRule{p.playlist, p.editor, QStringLiteral("ExchangeList"),
                          QStringLiteral("PlaylistEditor exchange pair: playlists hold %1, the editor reads it as %2").arg(p.playlist, p.editor), false};
    }

    // 2. Drive letters. Playlists hold MasterPlay's local drive (saving replaces the mapped letter with it); loading
    // puts the mapped letter back, so the rule runs local -> mapped.
    const QChar L = ini.localDriveLetter, M = ini.mappedDriveLetter;
    const QString local = L.isNull() ? QString() : QString(L) + QStringLiteral(":\\");
    switch (ini.exchangeMode) {
    case ExchangeMode::Absent:
        break;
    case ExchangeMode::Disabled:
        s.notes << QStringLiteral("ExchangeDriveLetters=0 (disabled): playlists hold the paths as the editor saw them; no drive rule.");
        break;
    case ExchangeMode::Enabled:
    case ExchangeMode::Automatic:
        if (local.isEmpty()) {
            s.notes << QStringLiteral("ExchangeDriveLetters=%1 without MasterPlayLocalDriveLetter: no drive rule.").arg(int(ini.exchangeMode));
        } else if (!M.isNull() && M != L) {
            rules << PathRule{local, QString(M) + QStringLiteral(":\\"), QStringLiteral("ExchangeDriveLetters"),
                              QStringLiteral("MasterPlay's local drive %1: is %2: on the editor workstation (ExchangeDriveLetters=%3); right when this station maps it the same way")
                                  .arg(QString(L), QString(M), QString::number(int(ini.exchangeMode))), false};
        } else if (!host.isEmpty()) {
            rules << PathRule{local, QStringLiteral("\\\\%1\\%2$\\").arg(host, QString(L)), QStringLiteral("MasterPlayLocalDriveLetter"),
                              QStringLiteral("guess: the administrative share of MasterPlay's local drive %1: on %2; replace it with the share this station can read").arg(QString(L), host), true};
        } else {
            rules << PathRule{local, local, QStringLiteral("MasterPlayLocalDriveLetter"),
                              QStringLiteral("MasterPlay's local drive %1: (ExchangeDriveLetters=%2, no mapped letter). Identity: right when PlayPlay runs on the MasterPlay machine; otherwise set where this station reaches that drive")
                                  .arg(QString(L), QString::number(int(ini.exchangeMode))), true};
        }
        break;
    }

    // 3. Where the INI was read from: a UNC folder is the MasterPlay folder as this station sees it, because Mplay.INI
    // lives in the MasterPlay folder. A copy kept elsewhere (\\nas\configs\station\Mplay.INI) breaks that, so the
    // rule is taken as stated only when the share folder has the MasterPlay folder's name; otherwise it is a guess (and
    // so is the host behind the D$ rule above).
    if (!host.isEmpty()) {
        QString iniDir = normalizePath(o.iniPath);
        if (iniDir.endsWith(QLatin1String(".ini"), Qt::CaseInsensitive)) iniDir = parentDir(iniDir);
        const QString inst = ini.installDir;
        if (inst.isEmpty() || stem(inst).size() <= 2) {
            s.notes << QStringLiteral("Read from %1, but the MasterPlay folder could not be inferred from the INI: no location rule.").arg(normalizePath(o.iniPath));
        } else if (stem(inst).compare(stem(iniDir), Qt::CaseInsensitive) != 0) {
            const bool sameName = lastPart(inst).compare(lastPart(iniDir), Qt::CaseInsensitive) == 0;
            rules << PathRule{inst, iniDir, QStringLiteral("iniLocation"),
                              sameName ? QStringLiteral("Mplay.INI was read from %1, so the MasterPlay folder %2 is reachable there").arg(normalizePath(o.iniPath), inst)
                                       : QStringLiteral("guess: Mplay.INI was read from %1, a folder not named like the MasterPlay folder %2; right only when that share "
                                                        "is the MasterPlay folder itself, not a copy of the INI kept elsewhere").arg(normalizePath(o.iniPath), inst),
                              !sameName};
            if (!sameName)
                s.notes << QStringLiteral("The INI may be a copy: confirm the iniLocation rule (and the host of any D$ rule) before importing, e.g. by checking that the mapped playlistDir exists.");
        }
    } else if (!o.iniPath.isEmpty()) {
        s.notes << QStringLiteral("Read from a local or copied path (%1): it says nothing about where the MasterPlay folder is; no location rule.").arg(normalizePath(o.iniPath));
    }
    s.pathMap = normalizeRules(rules);

    auto mapped = [&](const QString& p) { return p.isEmpty() ? QString() : normalizePath(mapPath(p, s.pathMap)); };
    const QString logPath = joinPath(ini.logDir, ini.asRunLog);
    QString label = !ini.stationName.isEmpty() ? ini.stationName : !ini.channelName.isEmpty() ? ini.channelName : QString();
    if (label.isEmpty()) label = host.isEmpty() ? QStringLiteral("MasterPlay") : QStringLiteral("MasterPlay %1").arg(host);
    const QString idPart = slug(!host.isEmpty() ? host : ini.stationName);
    QJsonArray recent;
    for (const QString& p : ini.recentPlaylists) recent.append(mapped(p));
    s.source = QJsonObject{
        {"id", idPart.isEmpty() ? QStringLiteral("masterplay") : QStringLiteral("masterplay-") + idPart},
        {"label", label},
        {"read", "fs"},
        {"playlistDir", mapped(ini.playlistDir)},
        {"logPath", mapped(logPath)},
        {"logDir", mapped(ini.logDir)},
        {"statusLog", mapped(joinPath(ini.logDir, ini.statusLog))},
        {"operatorLog", mapped(joinPath(ini.logDir, ini.operatorLog))},
        {"osdDir", mapped(ini.osdDir)},
        {"tz", QString()},
        {"dailyPlaylistMode", ini.dailyPlaylistMode},
        {"lastPlaylist", mapped(ini.lastPlaylist)},
        {"recentPlaylists", recent},
        {"ini", QJsonObject{{"playlistDir", ini.playlistDir}, {"logPath", logPath}, {"osdDir", ini.osdDir}}}};

    for (const PathRule& r : s.pathMap) s.notes << QStringLiteral("%1 -> %2: %3").arg(r.from, r.to, r.note);
    s.notes << QStringLiteral("tz: the INI has no time zone; set the MasterPlay machine's zone (as-run stamps are its wall clock).");
    if (ini.asRunNaming != 0)
        s.notes << QStringLiteral("NamingAsRun=%1: the as-run log is not always named %2 [UNVERIFIED naming]; check logPath.").arg(ini.asRunNaming).arg(ini.asRunLog);
    if (!ini.asRunIsoDateTime) s.notes << QStringLiteral("AsRunLogIsoDateTime=0: as-run stamps are in the legacy form (read with legacyDateOrder).");
    if (!ini.asRunWithDirectories) s.notes << QStringLiteral("AsRunLogWithDirectories=0: as-run lines carry bare file names; they join by name only.");
    if (ini.dailyPlaylistMode) s.notes << QStringLiteral("DailyPlaylistMode=1: one dated .in per day in the playlist folder.");
    if (!ini.searchPaths.isEmpty()) s.notes << QStringLiteral("Bare file names resolve through %1 search path(s) (SuchPfade).").arg(ini.searchPaths.size());
    if (!ini.substitutes.isEmpty()) s.notes << QStringLiteral("%1 %name% substitution(s) from SubstituteTable apply to directory paths.").arg(ini.substitutes.size());
    return s;
}

} // namespace mp::ini
