#include "masterplayin.h"
#include "masterplayinstructions.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QStringEncoder>
#include <algorithm>
#include <cmath>
#include <functional>

namespace mp {

namespace {

const QByteArray kUtf8Bom("\xEF\xBB\xBF", 3);

// Windows-1252 0x80..0x9F. The five bytes cp1252 leaves undefined (81 8D 8F 90 9D) map to the C1 control of the same
// value, as MultiByteToWideChar does, so every byte decodes and encodes back to itself.
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

// Characters cp1252 cannot hold become '?' and set *lossy (the exporter reports those lines).
QByteArray toCp1252(const QString& s, bool* lossy) {
    QByteArray b(s.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < s.size(); ++i) {
        const char16_t u = s.at(i).unicode();
        char c = '?';
        bool ok = false;
        if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) { c = char(u); ok = true; }
        else for (int k = 0; k < 32; ++k) if (kCp1252High[k] == u) { c = char(0x80 + k); ok = true; break; }
        if (!ok && lossy) *lossy = true;
        b[i] = c;
    }
    return b;
}

// Strict UTF-8: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequence. Returns the offset of
// the first byte that breaks it, -1 when all of b is valid. Written out rather than read from
// QStringDecoder::hasError() so the encoding verdict does not depend on decoder flags or state handling, and so MP010
// can point at the byte.
qsizetype firstInvalidUtf8(const QByteArray& b) {
    const auto* p = reinterpret_cast<const uchar*>(b.constData());
    const qsizetype n = b.size();
    for (qsizetype i = 0; i < n;) {
        const uchar c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len = 0; uint cp = 0;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return i;
        if (i + len > n) return i;
        for (int k = 1; k < len; ++k) {
            const uchar cc = p[i + k];
            if ((cc & 0xC0) != 0x80) return i;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return i;
        i += len;
    }
    return -1;
}

bool isWide(Encoding e) { return e == Encoding::Utf16LE || e == Encoding::Utf16BE; }
bool isUtf8(Encoding e) { return e == Encoding::Utf8Bom || e == Encoding::Utf8; }
bool hasBomFor(Encoding e) { return e == Encoding::Utf8Bom || isWide(e); }

QString decodeText(const QByteArray& b, Encoding e) {
    switch (e) {
    case Encoding::Ansi1252: return fromCp1252(b);
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        // ConvertInitialBom: the file BOM is stripped before splitting, so a U+FEFF here is content and is kept.
        QStringDecoder dec(e == Encoding::Utf16LE ? QStringConverter::Utf16LE : QStringConverter::Utf16BE,
                           QStringConverter::Flag::Stateless | QStringConverter::Flag::ConvertInitialBom);
        return dec(b);
    }
    default: return QString::fromUtf8(b);
    }
}

QByteArray encodeText(const QString& s, Encoding e, bool* lossy = nullptr) {
    switch (e) {
    case Encoding::Ansi1252: return toCp1252(s, lossy);
    case Encoding::Utf16LE:
    case Encoding::Utf16BE: {
        QStringEncoder enc(e == Encoding::Utf16LE ? QStringConverter::Utf16LE : QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        return enc(s);
    }
    default: return s.toUtf8();
    }
}

QByteArray bomBytes(Encoding e) {
    switch (e) {
    case Encoding::Utf8Bom: return kUtf8Bom;
    case Encoding::Utf16LE: return QByteArray("\xFF\xFE", 2);
    case Encoding::Utf16BE: return QByteArray("\xFE\xFF", 2);
    default: return {};
    }
}

// ASCII digits only: QChar::isDigit() also accepts other scripts' digits, which a stamp never has.
bool digitsAt(const QString& s, int from, int len) {
    for (int k = 0; k < len; ++k) { const char16_t c = s.at(from + k).unicode(); if (c < u'0' || c > u'9') return false; }
    return true;
}
int numAt(const QString& s, int from, int len) {
    int v = 0;
    for (int k = 0; k < len; ++k) v = v * 10 + (s.at(from + k).unicode() - u'0');
    return v;
}
// Both stamp forms are 19 characters with the time at 11..18: "YYYY-MM-DDThh:mm:ss" and "NN/NN/NNNN hh:mm:ss".
bool timeShape(const QString& s) {
    return digitsAt(s, 11, 2) && s.at(13) == u':' && digitsAt(s, 14, 2) && s.at(16) == u':' && digitsAt(s, 17, 2);
}
bool isoShape(const QString& s) {
    return s.size() >= 19 && digitsAt(s, 0, 4) && s.at(4) == u'-' && digitsAt(s, 5, 2) && s.at(7) == u'-' && digitsAt(s, 8, 2)
        && s.at(10) == u'T' && timeShape(s);
}
bool legacyShape(const QString& s) {
    return s.size() >= 19 && digitsAt(s, 0, 2) && s.at(2) == u'/' && digitsAt(s, 3, 2) && s.at(5) == u'/' && digitsAt(s, 6, 4)
        && s.at(10) == u' ' && timeShape(s);
}

// Fields from `from` to the end of the line, split on TAB only. A line ending in TAB gets an empty bare last field
// (Testfiles.in), which is what makes the trailing TAB round-trip. cols receives each field's 1-based column.
QList<Param> splitFields(const QString& s, qsizetype from, QList<int>* cols) {
    QList<Param> out;
    qsizetype p = from;
    for (;;) {
        const qsizetype t = s.indexOf(u'\t', p);
        out << parseField(s.mid(p, t < 0 ? -1 : t - p));
        if (cols) *cols << int(p) + 1;
        if (t < 0) break;
        p = t + 1;
    }
    return out;
}

// What a rendered field may not contain: TAB would add a field, CR or LF a line (MasterPlay's TStringList also breaks
// on a lone CR). Parsed fields never hold TAB, so this only changes edited text.
QString clean(QString s, bool tabs) {
    for (QChar& c : s)
        if (c == u'\r' || c == u'\n' || (tabs && c == u'\t')) c = u' ';
    return s;
}

using DiagFn = std::function<void(int col, const char* severity, const char* code, const QString& message)>;

void parseHeader(Entry& e, const QString& text, const ParseOptions& o, const DiagFn& diag) {
    e.kind = LineKind::Header;
    QString why;
    if (!parseStamp(text.left(19), o, &e.stamp, &why))
        diag(1, "error", "MP002", QStringLiteral("bad event stamp \"%1\": %2").arg(text.left(19), why));
    else if (e.stamp.ambiguous)   // "03/04/2025": 3 April or 4 March; the player's own debug log is MM/DD (design §2.3)
        diag(1, "warning", "MP007", QStringLiteral("legacy date %1 is valid day-first and month-first: read as %2 (day first); "
                                                  "set legacyDateOrder to dmy or mdy to choose")
                                        .arg(text.left(10), e.stamp.date.toString(Qt::ISODate)));
    QList<int> cols;
    if (text.size() > 19) e.params = splitFields(text, 20, &cols);

    // Enum keys: a value the editor could not have written is reported and left at its default, so the importer never
    // acts on PlayMode=9 as if it meant something.
    auto intKey = [&](const QString& key, int lo, int hi, int* out) {
        const int i = paramIndex(e.params, key);
        if (i < 0) return;
        bool ok = false;
        const QString v = e.params.at(i).value;
        const int n = v.trimmed().toInt(&ok);
        if (!ok) diag(cols.value(i), "warning", "MP011", QStringLiteral("%1=%2 is not a number").arg(key, v));
        else if (n < lo || n > hi)   // one arg() pass: the value is file text and may hold "%3"
            diag(cols.value(i), "warning", "MP011", QStringLiteral("%1=%2 is outside %3..%4").arg(key, v, QString::number(lo), QString::number(hi)));
        else *out = n;
    };
    intKey(QStringLiteral("PlayMode"), 0, 4, &e.playMode);
    intKey(QStringLiteral("EventType"), 0, 2, &e.eventType);
    auto durKey = [&](const QString& key, double* out) {
        const int i = paramIndex(e.params, key);
        if (i < 0 || e.params.at(i).value.trimmed().isEmpty()) return;
        bool ok = false;
        const double d = parseDuration(e.params.at(i).value, o.fps, &ok);
        if (ok) *out = d;
        else diag(cols.value(i), "warning", "MP013", QStringLiteral("bad duration %1=%2").arg(key, e.params.at(i).value));
    };
    durKey(QStringLiteral("EventDuration"), &e.eventDurationSec);
    durKey(QStringLiteral("ExpectedDuration"), &e.expectedSec);
    e.name = paramValue(e.params, QStringLiteral("Name"));
}

void parseInstruction(Entry& e, const QString& text) {
    const qsizetype n = text.size();
    qsizetype i = 1;
    while (i < n && text.at(i) != u' ' && text.at(i) != u'\t') ++i;
    e.kind = LineKind::Instruction;
    e.instr = text.mid(1, i - 1);
    qsizetype j = i;
    while (j < n && text.at(j) == u' ') ++j;
    e.pad = text.mid(i, j - i);
    if (e.pad.isEmpty()) {                                   // TAB form ("!ShowLogo<TAB>f=...") or the name alone
        if (i < n) e.params = splitFields(text, i + 1, nullptr);
        return;
    }
    // Space form: the head runs to the first TAB. "f=..." / "t=0" / "#=1" written after the space are fields; anything
    // else (a folder, a playlist, crawl text, "off") is the argument. pad + head keeps the exact spacing.
    static const QRegularExpression kFieldHead(QStringLiteral("^[A-Za-z0-9_#]+="));
    const qsizetype tab = text.indexOf(u'\t', j);
    const QString head = text.mid(j, tab < 0 ? -1 : tab - j);
    if (kFieldHead.match(head).hasMatch()) { e.argWasField = true; e.params << parseField(head); }
    else e.arg = head;
    if (tab >= 0) e.params += splitFields(text, tab + 1, nullptr);
}

void parseMedia(Entry& e, const QString& text, const ParseOptions& o, const DiagFn& diag) {
    e.kind = LineKind::Media;
    const qsizetype tab = text.indexOf(u'\t');
    const QString head = text.left(tab < 0 ? text.size() : tab);
    // "file.rtf[00:00:10;00]": a playlist-level forced duration after the name (the file on disk has no bracket). A
    // bracket inside the basename ("IntroWetter[5].TIF") is part of the real filename and stays in the path. Any other
    // token closing the name ("x.mpg[2025-01-01..2025-02-01]": MasterPlay's date windows, syntax unverified) stays in
    // the path too, but is reported (MP012) because ignoring a window would air the file outside it.
    static const QRegularExpression kBracket(QStringLiteral("\\[\\d{2}:\\d{2}:\\d{2}[;:.]\\d{2}\\]$"));
    static const QRegularExpression kToken(QStringLiteral("\\[[^\\[\\]]*\\]$"));
    const QRegularExpressionMatch m = kBracket.match(head);
    if (m.hasMatch() && m.capturedStart() > 0) {
        e.path = head.left(m.capturedStart());
        e.bracketRaw = m.captured(0);
        bool ok = false;
        const double d = parseDuration(e.bracketRaw.mid(1, e.bracketRaw.size() - 2), o.fps, &ok);
        if (ok) e.forcedDurationSec = d;
        else diag(int(m.capturedStart()) + 1, "warning", "MP013", QStringLiteral("bad bracket duration %1 (fps %2)").arg(e.bracketRaw).arg(o.fps));
    } else {
        e.path = head;
        const QRegularExpressionMatch t = kToken.match(head);
        if (t.hasMatch() && t.capturedStart() > 0)
            diag(int(t.capturedStart()) + 1, "warning", "MP012",
                 QStringLiteral("bracket token %1 after the name is not a forced duration [HH:MM:SS;FF]: kept as part of the path").arg(t.captured(0)));
    }
    if (tab >= 0) e.params = splitFields(text, tab + 1, nullptr);
}

QJsonObject paramJson(const Param& p) {
    if (p.bare) return {{"value", p.value}, {"bare", true}};
    return {{"key", p.key}, {"value", p.value}};
}

QJsonArray paramsJson(const QList<Param>& ps) {
    QJsonArray a;
    for (const Param& p : ps) a.append(paramJson(p));
    return a;
}

QJsonObject stampJson(const Stamp& s) {
    QJsonObject o{{"kind", dayKindName(s.kind)}, {"raw", s.raw}, {"legacy", s.legacy}};
    if (s.kind == DayKind::Weekly) { o["weekday"] = s.weekday; o["weekdayName"] = weekdayName(s.weekday); }
    if (s.kind == DayKind::Dated) o["date"] = s.date.toString(Qt::ISODate);
    if (s.time.isValid()) o["time"] = s.time.toString(QStringLiteral("hh:mm:ss"));
    if (s.ambiguous) o["ambiguous"] = true;
    return o;
}

QJsonObject itemJson(const Entry& e, bool withRaw) {
    QJsonObject o{{"line", e.line}, {"kind", lineKindName(e.kind)}};
    if (e.kind == LineKind::Instruction) {
        o["instr"] = e.instr;
        if (!e.arg.isEmpty()) o["arg"] = e.arg;
        const Instruction ins = identify(e);
        o["name"] = ins.name;
        if (!ins.known()) o["unknown"] = true;
    } else if (e.kind == LineKind::Media) {
        o["path"] = e.path;
        if (!e.bracketRaw.isEmpty()) o["bracket"] = e.bracketRaw;
        if (e.forcedDurationSec >= 0) o["forcedDurationSec"] = e.forcedDurationSec;
    }
    if (!e.params.isEmpty()) o["params"] = paramsJson(e.params);
    if (withRaw) o["raw"] = e.raw;
    return o;
}

} // namespace

// ---- fields ------------------------------------------------------------------------------------------------------

int paramIndex(const QList<Param>& ps, const QString& key) {
    for (int i = 0; i < ps.size(); ++i)
        if (!ps.at(i).bare && ps.at(i).key.compare(key, Qt::CaseInsensitive) == 0) return i;
    return -1;
}

QString paramValue(const QList<Param>& ps, const QString& key, const QString& def) {
    const int i = paramIndex(ps, key);
    return i < 0 ? def : ps.at(i).value;
}

QStringList paramValues(const QList<Param>& ps, const QString& key) {
    QStringList out;
    for (const Param& p : ps)
        if (!p.bare && p.key.compare(key, Qt::CaseInsensitive) == 0) out << p.value;
    return out;
}

void setParam(QList<Param>& ps, const QString& key, const QString& value) {
    const int i = paramIndex(ps, key);
    if (i >= 0) ps[i].value = value;
    else ps.append(Param{key, value, false});
}

Param parseField(const QString& field) {
    const qsizetype eq = field.indexOf(u'=');
    if (eq < 0) return Param{QString(), field, true};
    return Param{field.left(eq), field.mid(eq + 1), false};
}

QJsonObject Diagnostic::toJson() const {
    QJsonObject o{{"line", line}, {"severity", severity}, {"code", code}, {"message", message}};
    if (col > 0) o["col"] = col;
    return o;
}

// ---- stamps and durations ----------------------------------------------------------------------------------------

bool parseStamp(const QString& text, const ParseOptions& o, Stamp* out, QString* why) {
    Stamp st;
    st.raw = text;
    auto fail = [&](const QString& w) {
        st.kind = DayKind::Invalid;
        if (why) *why = w;
        if (out) *out = st;
        return false;
    };
    if (text.size() != 19) return fail(QStringLiteral("not a stamp"));
    const bool iso = isoShape(text);
    if (!iso && !legacyShape(text)) return fail(QStringLiteral("not a stamp"));
    st.legacy = !iso;

    if (iso) {
        const int y = numAt(text, 0, 4), m = numAt(text, 5, 2), d = numAt(text, 8, 2);
        if (y == 9999 && m == 99 && d == 99) st.kind = DayKind::EndOfList;
        else if (y == 0 && m == 0) {
            if (d == 0) st.kind = DayKind::EveryDay;
            else if (d >= 1 && d <= 7) { st.kind = DayKind::Weekly; st.weekday = d; }
            else if (d == 80) st.kind = DayKind::DayChange;
            else return fail(QStringLiteral("unknown day code 0000-00-%1").arg(d, 2, 10, QLatin1Char('0')));
        } else {
            st.date = QDate(y, m, d);   // year 0 is invalid in QDate, as it is here
            if (!st.date.isValid()) return fail(QStringLiteral("no such date"));
            st.kind = DayKind::Dated;
        }
    } else {
        // Legacy form: the weekday code sits in the first field (the ISO day field moved to the front), so dated stamps
        // are DD/MM/YYYY; "auto" reads MM/DD/YYYY when the second number cannot be a month (design §2.3).
        const int a = numAt(text, 0, 2), b = numAt(text, 3, 2), y = numAt(text, 6, 4);
        if (a == 99 && b == 99 && y == 9999) st.kind = DayKind::EndOfList;
        else if (y == 0 && b == 0) {
            if (a == 0) st.kind = DayKind::EveryDay;
            else if (a >= 1 && a <= 7) { st.kind = DayKind::Weekly; st.weekday = a; }
            else if (a == 80) st.kind = DayKind::DayChange;
            else return fail(QStringLiteral("unknown day code %1/00/0000").arg(a, 2, 10, QLatin1Char('0')));
        } else {
            const QDate dmy(y, b, a), mdy(y, a, b);
            const QString order = o.legacyDateOrder.toLower();
            if (order == u"dmy") st.date = dmy;
            else if (order == u"mdy") st.date = mdy;
            else {
                st.date = dmy.isValid() ? dmy : mdy;
                st.ambiguous = dmy.isValid() && mdy.isValid() && a != b;
            }
            if (!st.date.isValid()) return fail(QStringLiteral("no such date"));
            st.kind = DayKind::Dated;
        }
    }

    const QTime t(numAt(text, 11, 2), numAt(text, 14, 2), numAt(text, 17, 2));
    if (t.isValid()) st.time = t;
    else if (st.kind != DayKind::EndOfList) return fail(QStringLiteral("no such time"));   // the sentinel's time is free
    if (out) *out = st;
    return true;
}

QString formatStamp(const Stamp& s, bool iso) {
    const QLatin1Char z('0');
    QString day;
    switch (s.kind) {
    case DayKind::EveryDay: day = iso ? QStringLiteral("0000-00-00") : QStringLiteral("00/00/0000"); break;
    case DayKind::Weekly: day = iso ? QStringLiteral("0000-00-%1").arg(s.weekday, 2, 10, z) : QStringLiteral("%1/00/0000").arg(s.weekday, 2, 10, z); break;
    case DayKind::DayChange: day = iso ? QStringLiteral("0000-00-80") : QStringLiteral("80/00/0000"); break;
    case DayKind::EndOfList: day = iso ? QStringLiteral("9999-99-99") : QStringLiteral("99/99/9999"); break;
    case DayKind::Dated: day = s.date.toString(iso ? QStringLiteral("yyyy-MM-dd") : QStringLiteral("dd/MM/yyyy")); break;
    case DayKind::Invalid: return s.raw;
    }
    day += iso ? u'T' : u' ';
    // The end-of-list sentinel's time is not validated: keep what was written when it is not a clock time.
    day += s.time.isValid() ? s.time.toString(QStringLiteral("hh:mm:ss")) : s.raw.size() == 19 ? s.raw.mid(11) : QStringLiteral("00:00:00");
    return day;
}

double parseDuration(const QString& text, int fps, bool* ok) {
    if (ok) *ok = false;
    if (fps <= 0) fps = 30;
    QString t = text.trimmed();
    const bool neg = t.startsWith(u'-');
    if (neg) t.remove(0, 1);
    static const QRegularExpression kSeconds(QStringLiteral("^\\d{1,9}(?:\\.\\d+)?$"));
    static const QRegularExpression kClock(QStringLiteral("^(\\d{1,6}):(\\d{1,2}):(\\d{1,2})(?:[;:.](\\d{1,2}))?$"));
    double v = 0;
    if (kSeconds.match(t).hasMatch()) {
        v = t.toDouble();
    } else {
        const QRegularExpressionMatch m = kClock.match(t);
        if (!m.hasMatch()) return 0;
        const int h = m.captured(1).toInt(), mi = m.captured(2).toInt(), s = m.captured(3).toInt();
        const int f = m.capturedLength(4) ? m.captured(4).toInt() : 0;
        if (mi > 59 || s > 59 || f >= fps) return 0;
        v = h * 3600.0 + mi * 60.0 + s + double(f) / fps;
    }
    if (ok) *ok = true;
    return neg ? -v : v;
}

QString formatDuration(double sec, bool frames, int fps, QChar frameSep) {
    if (fps <= 0) fps = 30;
    const bool neg = sec < 0;
    const double a = std::abs(sec);
    qint64 total = 0, f = 0;
    if (frames) { const qint64 tf = qRound64(a * fps); total = tf / fps; f = tf % fps; }
    else total = qRound64(a);
    const QLatin1Char z('0');
    QString out = QStringLiteral("%1:%2:%3").arg(total / 3600, 2, 10, z).arg((total / 60) % 60, 2, 10, z).arg(total % 60, 2, 10, z);
    if (frames) { out += frameSep; out += QStringLiteral("%1").arg(f, 2, 10, z); }
    if (neg && (total || f)) out.prepend(u'-');
    return out;
}

QString formatBracket(double sec, int fps) { return QStringLiteral("[") + formatDuration(sec, true, fps, QLatin1Char(';')) + QStringLiteral("]"); }

int qtDayOfWeek(int code) { return code >= 1 && code <= 7 ? (code == 1 ? 7 : code - 1) : 0; }
int weekdayCode(int qtDow) { return qtDow >= 1 && qtDow <= 7 ? (qtDow == 7 ? 1 : qtDow + 1) : 0; }
QString weekdayName(int code) {
    static const char* const kNames[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    return code >= 1 && code <= 7 ? QString::fromLatin1(kNames[code - 1]) : QString();
}

QString encodingName(Encoding e) {
    switch (e) {
    case Encoding::Utf8Bom: return QStringLiteral("utf8bom");
    case Encoding::Utf8: return QStringLiteral("utf8");
    case Encoding::Ansi1252: return QStringLiteral("cp1252");
    case Encoding::Utf16LE: return QStringLiteral("utf16le");
    case Encoding::Utf16BE: return QStringLiteral("utf16be");
    }
    return {};
}

std::optional<Encoding> encodingFromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n == u"utf8bom" || n == u"utf-8-bom") return Encoding::Utf8Bom;
    if (n == u"utf8" || n == u"utf-8") return Encoding::Utf8;
    if (n == u"cp1252" || n == u"ansi" || n == u"windows-1252") return Encoding::Ansi1252;
    if (n == u"utf16le" || n == u"utf-16le") return Encoding::Utf16LE;
    if (n == u"utf16be" || n == u"utf-16be") return Encoding::Utf16BE;
    return std::nullopt;
}

QString stampFormName(StampForm f) {
    switch (f) {
    case StampForm::Keep: return QStringLiteral("keep");
    case StampForm::Iso: return QStringLiteral("iso");
    case StampForm::Legacy: return QStringLiteral("legacy");
    }
    return {};
}

std::optional<StampForm> stampFormFromName(const QString& name) {
    const QString n = name.trimmed().toLower();
    if (n == u"keep") return StampForm::Keep;
    if (n == u"iso") return StampForm::Iso;
    if (n == u"legacy") return StampForm::Legacy;
    return std::nullopt;
}

const QList<DiagnosticInfo>& diagnosticCatalog() {
    // Design §15. The parser's severities are the ones it emits; later stages use theirs as the default.
    static const char* const kRows[][4] = {
        {"MP001", "warning", "parser", "unknown line kind"},
        {"MP002", "error", "parser", "bad stamp"},
        {"MP003", "warning", "parser", "duplicate event time"},
        {"MP004", "info", "parser", "out-of-order events"},
        {"MP005", "warning", "resolver", "file not found"},
        {"MP006", "warning", "importer", "instruction mapped partially"},
        {"MP007", "warning", "parser", "ambiguous legacy date"},
        {"MP008", "warning", "parser", "items before the first header (preamble)"},
        {"MP009", "warning", "parser", "mixed EOL (or a lone CR)"},
        {"MP010", "warning", "parser", "cp1252 fallback (or bytes not valid in the BOM's encoding)"},
        {"MP011", "warning", "catalog", "unknown instruction / parameter"},
        {"MP012", "warning", "parser", "unknown bracket token"},
        {"MP013", "warning", "parser", "bad duration"},
        {"MP014", "warning", "catalog", "header PlayMode disagrees with the in-band mode"},
        {"MP015", "warning", "timeline", "lower-priority event dropped at the same time"},
        {"MP016", "warning", "importer", "content far from the window length"},
        {"MP017", "warning", "timeline", "ChangePlaylist not followed"},
        {"MP018", "warning", "resolver", "ambiguous filename match"},
        {"MP019", "warning", "resolver", "folder has no library media"},
        {"MP020", "warning", "importer", "search-term directory"},
        {"MP021", "info", "importer", "folder option approximated"},
        {"MP022", "warning", "importer", "unsupported instruction"},
        {"MP023", "error", "timeline", "sub-playlist cycle"},
        {"MP024", "warning", "timeline", "sub-playlist missing"},
        {"MP025", "warning", "importer", "break mode needs GPI"},
        {"MP026", "info", "importer", "acts collapsed (no segments)"},
        {"MP027", "error", "importer", "act without segment (would air whole)"},
        {"MP028", "info", "importer", "crawl text snapshot"},
        {"MP030", "warning", "exporter", "query block not exportable"},
        {"MP031", "info", "exporter", "item approximated or left out (no .IN form)"},
        {"MP032", "warning", "exporter", "characters the target encoding cannot hold"},
    };
    static const QList<DiagnosticInfo> list = [] {
        QList<DiagnosticInfo> l;
        for (const auto& r : kRows)
            l.append(DiagnosticInfo{QString::fromLatin1(r[0]), QString::fromLatin1(r[1]), QString::fromLatin1(r[2]), QString::fromLatin1(r[3])});
        return l;
    }();
    return list;
}

DiagnosticInfo diagnosticInfo(const QString& code) {
    for (const DiagnosticInfo& i : diagnosticCatalog())
        if (i.code.compare(code, Qt::CaseInsensitive) == 0) return i;
    return {};
}

QString lineKindName(LineKind k) {
    switch (k) {
    case LineKind::Blank: return QStringLiteral("blank");
    case LineKind::Header: return QStringLiteral("header");
    case LineKind::Instruction: return QStringLiteral("instruction");
    case LineKind::Media: return QStringLiteral("media");
    case LineKind::Unknown: return QStringLiteral("unknown");
    }
    return {};
}

QString dayKindName(DayKind k) {
    switch (k) {
    case DayKind::EveryDay: return QStringLiteral("everyDay");
    case DayKind::Weekly: return QStringLiteral("weekly");
    case DayKind::Dated: return QStringLiteral("dated");
    case DayKind::DayChange: return QStringLiteral("dayChange");
    case DayKind::EndOfList: return QStringLiteral("endOfList");
    case DayKind::Invalid: return QStringLiteral("invalid");
    }
    return {};
}

// ---- lines -------------------------------------------------------------------------------------------------------

Entry parseLine(const QString& text, const ParseOptions& o, QList<Diagnostic>* diags, int line) {
    Entry e;
    e.line = line;
    e.raw = text;
    const DiagFn diag = [&](int col, const char* severity, const char* code, const QString& message) {
        if (diags) diags->append(Diagnostic{line, col, QString::fromLatin1(severity), QString::fromLatin1(code), message});
    };
    // Unknown lines are kept verbatim and reported; the importer ignores them. There is no comment syntax in the
    // format, so '#', ';' and '//' lines (the Python editor's invention) land here too.
    auto unknown = [&](int col, const QString& why) {
        e.kind = LineKind::Unknown;
        diag(col, "warning", "MP001", why);
        return e;
    };
    const qsizetype n = text.size();
    qsizetype k = 0;
    while (k < n && (text.at(k) == u' ' || text.at(k) == u'\t')) ++k;
    if (k == n) { e.kind = LineKind::Blank; return e; }
    if (k > 0) return unknown(int(k) + 1, QStringLiteral("leading whitespace: not a header, instruction or media line"));
    const QChar c0 = text.at(0);
    if (c0 == QChar(0xFEFF)) return unknown(1, QStringLiteral("stray byte-order mark at the start of the line"));
    if (c0 == u'#' || c0 == u';' || text.startsWith(QStringLiteral("//")))
        return unknown(1, QStringLiteral("MasterPlay has no comment syntax: line kept verbatim and ignored"));
    if (c0 == u'!') {
        if (n == 1 || text.at(1) == u' ' || text.at(1) == u'\t') return unknown(1, QStringLiteral("instruction without a name"));
        parseInstruction(e, text);
        return e;
    }
    if (isoShape(text) || legacyShape(text)) {
        if (n > 19 && text.at(19) != u'\t') return unknown(20, QStringLiteral("event stamp not followed by a TAB"));
        parseHeader(e, text, o, diag);
        return e;
    }
    parseMedia(e, text, o, diag);
    return e;
}

QString renderEntry(const Entry& e, const WriteOptions& w) {
    auto fields = [](QString& out, const QList<Param>& ps, int from) {
        for (int i = from; i < ps.size(); ++i) { out += QLatin1Char('\t'); out += clean(ps.at(i).text(), true); }
    };
    QString out;
    switch (e.kind) {
    case LineKind::Blank:
        return w.canonical ? QString() : clean(e.raw, false);
    case LineKind::Unknown:
        return clean(e.raw, false);
    case LineKind::Header: {
        // Keep: the stamp as written while raw is set (an edit clears it). Iso / Legacy: from the parsed fields; a bad
        // stamp has nothing to convert and keeps its text.
        const Stamp& s = e.stamp;
        const bool convert = s.isValid() && w.stamps != StampForm::Keep;
        out = clean(convert ? formatStamp(s, w.stamps == StampForm::Iso) : s.raw.isEmpty() ? formatStamp(s, !s.legacy) : s.raw, true);
        fields(out, e.params, 0);
        return out;
    }
    case LineKind::Instruction: {
        out = QStringLiteral("!") + clean(e.instr, true);
        const bool fieldHead = e.argWasField && !e.params.isEmpty();
        QString pad = e.pad;
        if (pad.count(u' ') != pad.size()) pad = QStringLiteral(" ");          // only spaces separate the head
        if (pad.isEmpty() && (fieldHead || !e.arg.isEmpty())) pad = QStringLiteral(" ");
        out += pad;
        out += clean(fieldHead ? e.params.first().text() : e.arg, true);
        fields(out, e.params, fieldHead ? 1 : 0);
        return out;
    }
    case LineKind::Media:
        out = clean(e.path, true) + clean(e.bracketRaw, true);
        fields(out, e.params, 0);
        return out;
    }
    return clean(e.raw, false);
}

// ---- documents ---------------------------------------------------------------------------------------------------

Document parse(const QByteArray& bytes, const ParseOptions& o) {
    Document d;
    const auto diag = [&d](int line, int col, const char* severity, const char* code, const QString& message) {
        d.diagnostics.append(Diagnostic{line, col, QString::fromLatin1(severity), QString::fromLatin1(code), message});
    };
    // Encoding: UTF-8 BOM, UTF-16 BOM, strict UTF-8, else cp1252 (design §2.1). bad is the offset (in body) of the first
    // byte that is not valid UTF-8, for MP010; UTF-16 problems are found per line below.
    qsizetype skip = 0;
    if (bytes.startsWith(kUtf8Bom)) { d.encoding = Encoding::Utf8Bom; skip = kUtf8Bom.size(); }
    else if (bytes.startsWith(QByteArrayView("\xFF\xFE", 2))) { d.encoding = Encoding::Utf16LE; skip = 2; }
    else if (bytes.startsWith(QByteArrayView("\xFE\xFF", 2))) { d.encoding = Encoding::Utf16BE; skip = 2; }
    const QByteArray body = skip ? bytes.mid(skip) : bytes;
    qsizetype bad = -1;
    if (!skip) { bad = firstInvalidUtf8(body); d.encoding = bad < 0 ? Encoding::Utf8 : Encoding::Ansi1252; }
    else if (d.encoding == Encoding::Utf8Bom) bad = firstInvalidUtf8(body);

    // Split on LF; a CR right before it belongs to the terminator, a lone CR stays in the text (MP009). UTF-16 is split
    // on whole code units, so a 0x0A or 0x0D byte inside another character (U+0A0D, U+0D0A, U+010A) ends nothing. Each
    // entry keeps its own bytes and terminator, so the file can be written back exactly whatever mix it holds.
    const bool wide = isWide(d.encoding), le = d.encoding == Encoding::Utf16LE;
    const qsizetype w = wide ? 2 : 1, n = body.size();
    auto unit = [&](qsizetype k) -> char16_t {
        if (!wide) return uchar(body.at(k));
        const uint a = uchar(body.at(k)), b = uchar(body.at(k + 1));
        return char16_t(le ? (a | (b << 8)) : ((a << 8) | b));
    };
    auto findLf = [&](qsizetype from) -> qsizetype {
        if (!wide) return body.indexOf('\n', from);
        for (qsizetype k = from; k + 1 < n; k += 2) if (unit(k) == u'\n') return k;
        return -1;
    };
    qsizetype pos = 0;
    int line = 0;
    while (pos < n) {
        const qsizetype nl = findLf(pos);
        qsizetype end = nl < 0 ? n : nl;
        QByteArray eol;
        if (nl >= 0) {
            if (end - pos >= w && unit(end - w) == u'\r') { end -= w; eol = QByteArrayLiteral("\r\n"); }
            else eol = QByteArrayLiteral("\n");
        }
        const QByteArray raw = body.mid(pos, end - pos);
        Entry e = parseLine(decodeText(raw, d.encoding), o, &d.diagnostics, ++line);
        if (bad >= pos && bad < end) {   // never in the terminator: CR and LF are ASCII
            const qsizetype off = bad - pos;
            const QString byte = QString::number(uchar(body.at(bad)), 16).toUpper();
            if (d.encoding == Encoding::Ansi1252)
                diag(line, int(off) + 1, "warning", "MP010",
                     QStringLiteral("not UTF-8 and no BOM (byte 0x%1): decoded as Windows-1252 (cp1252) and written back in it").arg(byte));
            else
                diag(line, int(QString::fromUtf8(raw.left(off)).size()) + 1, "warning", "MP010",
                     QStringLiteral("invalid UTF-8 after the BOM (byte 0x%1): shown as U+FFFD; the line's bytes are written back "
                                    "unchanged until it is edited").arg(byte));
        }
        if (wide) {
            for (qsizetype k = 0; k + 1 < raw.size(); k += 2) {
                const char16_t u = unit(pos + k);
                if (QChar::isHighSurrogate(u) && k + 3 < raw.size() && QChar::isLowSurrogate(unit(pos + k + 2))) { k += 2; continue; }
                if (QChar::isSurrogate(u)) {
                    diag(line, int(k / 2) + 1, "warning", "MP010",
                         QStringLiteral("unpaired UTF-16 surrogate 0x%1: the line's bytes are written back unchanged until it is edited")
                             .arg(QString::number(uint(u), 16).toUpper()));
                    break;
                }
            }
            if (raw.size() % 2)   // only the last line can end on half a code unit
                diag(line, int(raw.size() / 2) + 1, "warning", "MP010",
                     QStringLiteral("odd byte count: the last byte is not a whole UTF-16 code unit (kept and written back while the "
                                    "line is not edited)"));
        }
        const qsizetype cr = e.raw.indexOf(u'\r');
        if (cr >= 0)   // TStringList (MasterPlay) breaks lines on a lone CR too; the design splits on \r?\n only
            diag(line, int(cr) + 1, "warning", "MP009",
                 QStringLiteral("lone CR inside the line: MasterPlay starts a new line here; kept as text (a space when the line "
                                "is re-rendered)"));
        e.bytes = raw;
        e.eol = eol;
        d.entries.append(std::move(e));
        pos = nl < 0 ? n : nl + w;
    }

    // Mixed terminators (plant files with LF-only headers and CRLF items, design §2.1): reported once, at the first
    // line using the minority terminator. Every line keeps its own on write.
    int crlf = 0, lf = 0;
    for (const Entry& e : d.entries) { if (e.eol == "\r\n") ++crlf; else if (e.eol == "\n") ++lf; }
    if (crlf && lf) {
        const QByteArray minority = lf <= crlf ? QByteArrayLiteral("\n") : QByteArrayLiteral("\r\n");
        for (const Entry& e : d.entries)
            if (e.eol == minority) {
                diag(e.line, 0, "warning", "MP009",
                     QStringLiteral("mixed line endings: %1 CRLF, %2 LF (the first %3 is here); each line keeps its own terminator on write")
                         .arg(crlf).arg(lf).arg(minority == "\n" ? QStringLiteral("LF") : QStringLiteral("CRLF")));
                break;
            }
    }

    for (int i = 0; i < d.entries.size(); ++i) {
        const Entry& e = d.entries.at(i);
        if (e.kind == LineKind::Header) {
            if (!d.events.isEmpty()) d.events.last().end = i;
            Event ev;
            ev.entry = i;
            ev.effectivePlayMode = e.playMode > 0 ? e.playMode : 1;
            d.events.append(ev);
        } else if (e.kind == LineKind::Instruction || e.kind == LineKind::Media) {
            if (d.events.isEmpty()) d.preamble << i;
            else d.events.last().items << i;
        }
    }
    if (!d.events.isEmpty()) d.events.last().end = d.entries.size();
    // The instruction catalog (MP-03): in-band play modes beat the header's cached PlayMode, and unknown names, bad
    // typed values and header / in-band disagreements are reported (MP011 / MP013 / MP014).
    resolvePlayModes(d);
    d.diagnostics += instructionDiagnostics(d, o);
    if (!d.preamble.isEmpty())
        d.diagnostics.append(Diagnostic{d.entries.at(d.preamble.first()).line, 0, QStringLiteral("warning"), QStringLiteral("MP008"),
                                        QStringLiteral("%1 item line(s) before the first event header").arg(d.preamble.size())});

    // Event order. The ISO text sorts the way MasterPlay's editor keeps the list (every day, weekly 1..7, the day-change
    // marker, dated, end of list), and a legacy header is compared by its ISO form, so "01/00/0000 06:00:00" and
    // "0000-00-01T06:00:00" are the same time. A duplicate (MP003) leaves the player to pick one; an out-of-order
    // header (MP004, each step down once) changes nothing for the importer, which builds its own day timeline.
    QHash<QString, int> seen;
    QString prevKey;
    int prevLine = 0;
    for (const Event& ev : d.events) {
        const Entry& h = d.entries.at(ev.entry);
        if (!h.stamp.isValid()) continue;
        const QString key = formatStamp(h.stamp, true);
        const auto it = seen.constFind(key);
        if (it != seen.cend()) diag(h.line, 1, "warning", "MP003", QStringLiteral("event time %1 is already used by the event at line %2").arg(key).arg(*it));
        else seen.insert(key, h.line);
        if (!prevKey.isEmpty() && key < prevKey)
            diag(h.line, 1, "info", "MP004", QStringLiteral("event %1 sorts before the event above it (%2, line %3)").arg(key, prevKey).arg(prevLine));
        prevKey = key;
        prevLine = h.line;
    }
    std::stable_sort(d.diagnostics.begin(), d.diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) { return a.line != b.line ? a.line < b.line : a.col < b.col; });
    return d;
}

QByteArray serialize(const Document& d, const WriteOptions& w, QList<int>* lossyLines) {
    const Encoding enc = w.encoding.value_or(d.encoding);
    // Untouched lines are copied as bytes while the target encodes text the same way (UTF-8 with or without a BOM is
    // the same bytes), and decoded / re-encoded otherwise, so their text survives an encoding change.
    const bool sameBytes = enc == d.encoding || (isUtf8(enc) && isUtf8(d.encoding));
    const bool wide = isWide(enc);
    const QByteArray forced = (w.eol == "\r\n" || w.eol == "\n") ? w.eol : QByteArray();
    const QByteArray def = forced.isEmpty() ? d.defaultEol() : forced;
    auto eolBytes = [&](const QByteArray& eol) { return wide ? encodeText(QString::fromLatin1(eol), enc) : eol; };
    qsizetype est = 4;
    for (const Entry& e : d.entries) est += e.bytes.size() + 2;
    QByteArray out;
    out.reserve(wide && !isWide(d.encoding) ? est * 2 : est);
    if (hasBomFor(enc)) out += bomBytes(enc);
    const int n = int(d.entries.size());
    for (int i = 0; i < n; ++i) {
        const Entry& e = d.entries.at(i);
        bool lossy = false;
        // Rendered: canonical, edited, or built by parseLine (no original bytes; an empty line renders as "").
        if (w.canonical || e.dirty || e.bytes.isEmpty()) out += encodeText(renderEntry(e, w), enc, &lossy);
        else if (sameBytes) out += e.bytes;
        else out += encodeText(e.raw, enc, &lossy);
        if (lossy && lossyLines) lossyLines->append(i + 1);
        // Only the last line may go without a terminator, so an entry appended after it cannot join its line.
        const bool last = i + 1 == n;
        const bool term = (last && w.finalNewline.has_value()) ? *w.finalNewline : (!last || !e.eol.isEmpty());
        if (!term) continue;
        const bool known = e.eol == "\r\n" || e.eol == "\n";
        out += eolBytes(!forced.isEmpty() ? forced : known ? e.eol : def);
    }
    return out;
}

namespace {
// One line's meaning, what semanticDiff compares; "" for a blank line. Fields are joined with TAB, which a parsed field
// never holds, so two different lines cannot read the same. Representation is left out: stamp form (headers compare
// in ISO), the instruction head form ("!ShowLogo f=" and "!ShowLogo<TAB>f=" both have f= as their first field), the
// spaces after the name, the forced-duration frame separator.
QString meaning(const Entry& e) {
    QString s;
    switch (e.kind) {
    case LineKind::Blank: return {};
    case LineKind::Unknown: return QStringLiteral("unknown ") + clean(e.raw, false);
    case LineKind::Header: s = e.stamp.isValid() ? formatStamp(e.stamp, true) : QStringLiteral("invalid ") + e.stamp.raw; break;
    case LineKind::Instruction: s = QLatin1Char('!') + e.instr + (e.arg.isEmpty() ? QString() : QLatin1Char(' ') + e.arg); break;
    case LineKind::Media:
        s = e.path + (e.forcedDurationSec >= 0 ? QStringLiteral("[%1 s]").arg(e.forcedDurationSec, 0, 'f', 3) : e.bracketRaw);
        break;
    }
    for (const Param& p : e.params) { s += QLatin1Char('\t'); s += p.text(); }
    return s;
}
} // namespace

QString semanticDiff(const Document& a, const Document& b) {
    auto collect = [](const Document& d) {
        QList<QPair<int, QString>> v;
        for (const Entry& e : d.entries)
            if (e.kind != LineKind::Blank) v.append(qMakePair(e.line, meaning(e)));
        return v;
    };
    const auto x = collect(a), y = collect(b);
    auto show = [](QString s) { return s.replace(u'\t', QStringLiteral(" | ")); };
    const qsizetype n = qMin(x.size(), y.size());
    for (qsizetype i = 0; i < n; ++i)
        if (x.at(i).second != y.at(i).second)   // one arg() pass: paths may hold '%' (MasterPlay %name% substitutions)
            return QStringLiteral("line %1 \"%2\" differs from line %3 \"%4\"")
                .arg(QString::number(x.at(i).first), show(x.at(i).second), QString::number(y.at(i).first), show(y.at(i).second));
    if (x.size() == y.size()) return {};
    const bool first = x.size() > y.size();
    const auto& extra = first ? x.at(n) : y.at(n);
    return QStringLiteral("the %1 document has %2 more line(s), from line %3 \"%4\"")
        .arg(first ? QStringLiteral("first") : QStringLiteral("second"), QString::number(qAbs(x.size() - y.size())),
             QString::number(extra.first), show(extra.second));
}

WriteOptions WriteOptions::canonicalForm() {
    WriteOptions w;
    w.canonical = true;
    w.stamps = StampForm::Iso;
    w.encoding = Encoding::Utf8Bom;
    w.eol = QByteArrayLiteral("\r\n");
    w.finalNewline = true;
    return w;
}

bool Document::hasBom() const { return hasBomFor(encoding); }

bool Document::finalNewline() const { return !entries.isEmpty() && !entries.last().eol.isEmpty(); }

QString Document::eolStyle() const {
    int crlf = 0, lf = 0;
    for (const Entry& e : entries) { if (e.eol == "\r\n") ++crlf; else if (e.eol == "\n") ++lf; }
    if (crlf && lf) return QStringLiteral("mixed");
    return crlf ? QStringLiteral("crlf") : lf ? QStringLiteral("lf") : QString();
}

QByteArray Document::defaultEol() const {
    int crlf = 0, lf = 0;
    for (const Entry& e : entries) { if (e.eol == "\r\n") ++crlf; else if (e.eol == "\n") ++lf; }
    return lf > crlf ? QByteArrayLiteral("\n") : QByteArrayLiteral("\r\n");
}

QJsonObject Document::stats() const {
    int blank = 0, headers = 0, instructions = 0, media = 0, unknown = 0;
    QHash<QString, int> insIndex;                            // case-folded name -> slot in insNames / insCounts
    QStringList insNames;                                    // first spelling seen
    QList<int> insCounts;
    for (const Entry& e : entries) {
        switch (e.kind) {
        case LineKind::Blank: ++blank; break;
        case LineKind::Header: ++headers; break;
        case LineKind::Media: ++media; break;
        case LineKind::Unknown: ++unknown; break;
        case LineKind::Instruction: {
            ++instructions;
            const QString key = e.instr.toLower();
            auto it = insIndex.find(key);
            if (it == insIndex.end()) { it = insIndex.insert(key, int(insNames.size())); insNames << e.instr; insCounts << 0; }
            ++insCounts[*it];
            break;
        }
        }
    }
    int dayKinds[6] = {}, week[8] = {}, items = int(preamble.size());
    for (const Event& ev : events) {
        const Stamp& s = header(ev).stamp;
        ++dayKinds[int(s.kind)];
        if (s.kind == DayKind::Weekly) ++week[s.weekday];
        items += int(ev.items.size());
    }
    QJsonObject byDay, byWeekday, byInstruction;
    for (int k = 0; k <= int(DayKind::Invalid); ++k) byDay[dayKindName(DayKind(k))] = dayKinds[k];
    for (int c = 1; c <= 7; ++c) byWeekday[QString::number(c)] = week[c];
    for (int i = 0; i < insNames.size(); ++i) byInstruction[insNames.at(i)] = insCounts.at(i);
    int errors = 0, warnings = 0, infos = 0;
    QJsonObject byCode;
    for (const Diagnostic& g : diagnostics) {
        if (g.severity == u"error") ++errors;
        else if (g.severity == u"warning") ++warnings;
        else ++infos;
        byCode[g.code] = byCode.value(g.code).toInt() + 1;
    }
    return {{"lines", int(entries.size())}, {"blank", blank}, {"headers", headers}, {"instructions", instructions},
            {"media", media}, {"unknown", unknown}, {"events", int(events.size())}, {"preamble", int(preamble.size())},
            {"items", items}, {"byDayKind", byDay}, {"byWeekday", byWeekday}, {"byInstruction", byInstruction},
            {"diagnostics", QJsonObject{{"error", errors}, {"warning", warnings}, {"info", infos}, {"byCode", byCode}}}};
}

QJsonObject Document::toJson(bool withRaw) const {
    QJsonArray evs;
    for (const Event& ev : events) {
        const Entry& h = header(ev);
        QJsonObject o{{"line", h.line}, {"stamp", stampJson(h.stamp)}, {"eventType", h.eventType},
                      {"effectivePlayMode", ev.effectivePlayMode}};
        if (h.playMode >= 0) o["playMode"] = h.playMode;
        if (ev.inBandPlayMode >= 0) o["inBandPlayMode"] = ev.inBandPlayMode;
        if (ev.repeatAt >= 0) o["repeatAt"] = ev.repeatAt;
        if (!h.name.isEmpty()) o["name"] = h.name;
        if (h.eventDurationSec >= 0) o["durationSec"] = h.eventDurationSec;
        if (h.expectedSec >= 0) o["expectedSec"] = h.expectedSec;
        if (withRaw) o["raw"] = h.raw;
        QJsonArray items;
        for (int i : ev.items) items.append(itemJson(entries.at(i), withRaw));
        o["items"] = items;
        evs.append(o);
    }
    QJsonArray pre, diags;
    for (int i : preamble) pre.append(itemJson(entries.at(i), withRaw));
    for (const Diagnostic& g : diagnostics) diags.append(g.toJson());
    QJsonObject out{{"encoding", encodingName(encoding)}, {"bom", hasBom()}, {"eol", eolStyle()}, {"finalNewline", finalNewline()},
                    {"stats", stats()}, {"events", evs}, {"preamble", pre}, {"diagnostics", diags}};
    if (withRaw) {
        QJsonArray unk;
        for (const Entry& e : entries)
            if (e.kind == LineKind::Unknown) unk.append(QJsonObject{{"line", e.line}, {"raw", e.raw}});
        out["unknown"] = unk;
    }
    return out;
}

} // namespace mp
