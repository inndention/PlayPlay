#include "mpasrun.h"
#include "masterplayinstructions.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QUuid>
#include <algorithm>
#include <climits>
#include <cstdio>

namespace mp::logs {

namespace {

constexpr qint64 kNoPrev = LLONG_MIN;
constexpr qint64 kMsPerDay = 86400000;
constexpr qint64 kUnixEpochJulianDay = 2440588;

// Windows-1252 0x80..0x9F; the five undefined bytes (81 8D 8F 90 9D) map to the C1 control of the same value, as
// MultiByteToWideChar does (same table as the playlist parser, which keeps its copy private).
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

bool validUtf8(const QByteArray& b) {
    QStringDecoder dec(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString s = dec(b);
    Q_UNUSED(s);
    return !dec.hasError();
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

QString excerpt(const QString& s) { return s.size() > 60 ? s.left(57) + QStringLiteral("...") : s; }

// Text lines without terminators (LF splits; a CR before it, or several, belong to the terminator; the empty piece
// after a final LF is no line). UTF-8 and cp1252 are split as bytes (0x0A is never inside a character there), UTF-16
// after decoding. badLine: the first line that is not valid in the detected encoding (MP010), 0 when none.
struct Decoded {
    Encoding encoding = Encoding::Utf8;
    QStringList lines;
    int badLine = 0;
};

void chopCr(QString& s) { while (s.endsWith(QLatin1Char('\r'))) s.chop(1); }

Decoded decodeLines(const QByteArray& bytes) {
    Decoded d;
    if (bytes.startsWith("\xFF\xFE") || bytes.startsWith("\xFE\xFF")) {
        const bool le = uchar(bytes.at(0)) == 0xFF;
        d.encoding = le ? Encoding::Utf16LE : Encoding::Utf16BE;
        const QByteArray body = bytes.mid(2);
        QStringDecoder dec(le ? QStringConverter::Utf16LE : QStringConverter::Utf16BE, QStringConverter::Flag::Stateless);
        const QString text = dec(body);
        d.lines = text.split(QLatin1Char('\n'));
        if (!d.lines.isEmpty() && d.lines.last().isEmpty()) d.lines.removeLast();
        for (QString& l : d.lines) chopCr(l);
        if (dec.hasError() || body.size() % 2) {
            const qsizetype at = text.indexOf(QChar(0xFFFD));
            d.badLine = at < 0 ? int(qMax<qsizetype>(1, d.lines.size())) : int(text.left(at).count(QLatin1Char('\n'))) + 1;
        }
        return d;
    }
    QByteArray body = bytes;
    if (bytes.startsWith("\xEF\xBB\xBF")) { d.encoding = Encoding::Utf8Bom; body = bytes.mid(3); }
    else d.encoding = validUtf8(bytes) ? Encoding::Utf8 : Encoding::Ansi1252;
    QList<QByteArray> raw = body.split('\n');
    if (!raw.isEmpty() && raw.last().isEmpty()) raw.removeLast();
    d.lines.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        const QByteArray& b = raw.at(i);
        const bool ascii = std::all_of(b.cbegin(), b.cend(), [](char c) { return uchar(c) < 0x80; });
        if (!ascii && !d.badLine && d.encoding != Encoding::Utf8 && !validUtf8(b)) d.badLine = int(i) + 1;
        QString s = ascii ? QString::fromLatin1(b) : d.encoding == Encoding::Ansi1252 ? fromCp1252(b) : QString::fromUtf8(b);
        chopCr(s);
        d.lines << s;
    }
    return d;
}

qint64 wallMs(QDate d, QTime t) {   // the wall clock read as UTC
    return (d.toJulianDay() - kUnixEpochJulianDay) * kMsPerDay + t.msecsSinceStartOfDay();
}

QString isoStamp(QDate d, QTime t, bool ms) {   // once or twice per line: snprintf, not QDate::toString's format parser
    char buf[32];
    const int n = ms ? std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03d", d.year(), d.month(), d.day(), t.hour(), t.minute(), t.second(), t.msec())
                     : std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d", d.year(), d.month(), d.day(), t.hour(), t.minute(), t.second());
    return QString::fromLatin1(buf, qMax(0, n));
}

QString codeText(int code) { return QStringLiteral("%1").arg(code, 3, 10, QLatin1Char('0')); }

bool isDigit(QChar c) { return c.unicode() >= u'0' && c.unicode() <= u'9'; }   // ASCII only

int digits(const QString& s, qsizetype from, int len) {   // -1 unless s[from, from+len) are ASCII digits
    if (from + len > s.size()) return -1;
    int v = 0;
    for (int k = 0; k < len; ++k) { const QChar c = s.at(from + k); if (!isDigit(c)) return -1; v = v * 10 + (c.unicode() - u'0'); }
    return v;
}

// ".zzz" (1..3 digits, also ',') after a stamp: milliseconds, the fraction padded right ("0.6" = 600 ms).
qsizetype fraction(const QString& s, qsizetype at, int* ms) {
    *ms = 0;
    if (at >= s.size() || (s.at(at) != QLatin1Char('.') && s.at(at) != QLatin1Char(','))) return at;
    qsizetype k = at + 1;
    int n = 0, v = 0;
    while (k < s.size() && n < 3 && isDigit(s.at(k))) { v = v * 10 + (s.at(k).unicode() - u'0'); ++k; ++n; }
    if (!n) return at;
    while (n < 3) { v *= 10; ++n; }
    *ms = v;
    return k;
}

struct StampRead {
    qsizetype end = -1;           // index after the stamp; -1 when the line does not start with one
    QDate date;
    QTime time;
    bool hasMs = false, legacy = false, ambiguous = false;
};

// "YYYY-MM-DDThh:mm:ss[.zzz]" (a space instead of T is accepted too).
StampRead readIso(const QString& s) {
    StampRead r;
    if (s.size() < 19 || s.at(4) != QLatin1Char('-') || s.at(7) != QLatin1Char('-') || (s.at(10) != QLatin1Char('T') && s.at(10) != QLatin1Char(' '))
        || s.at(13) != QLatin1Char(':') || s.at(16) != QLatin1Char(':')) return r;
    const int y = digits(s, 0, 4), mo = digits(s, 5, 2), d = digits(s, 8, 2), h = digits(s, 11, 2), mi = digits(s, 14, 2), se = digits(s, 17, 2);
    if (y < 0 || mo < 0 || d < 0 || h < 0 || mi < 0 || se < 0) return r;
    int ms = 0;
    const qsizetype end = fraction(s, 19, &ms);
    const QDate date(y, mo, d);
    const QTime time(h, mi, se, ms);
    if (!date.isValid() || !time.isValid()) return r;
    r.end = end; r.date = date; r.time = time; r.hasMs = end > 19;
    return r;
}

// "DD/MM/YYYY hh:mm:ss[.zzz][ AM|PM]" (AsRunLogIsoDateTime=0) [UNVERIFIED: no sample]. Day / month order as a legacy
// header stamp: auto reads DMY unless the second number cannot be a month.
StampRead readLegacy(const QString& s, const QString& order) {
    static const QRegularExpression kLegacy(QStringLiteral(
        "^(\\d{1,2})/(\\d{1,2})/(\\d{4})[ T](\\d{1,2}):(\\d{2}):(\\d{2})(?:[.,](\\d{1,3}))?(?: ?([AaPp][Mm]))?(?=[ \\t])"));
    StampRead r;
    if (s.size() < 10 || !isDigit(s.at(0))) return r;
    const QRegularExpressionMatch m = kLegacy.match(s);
    if (!m.hasMatch()) return r;
    const int a = m.captured(1).toInt(), b = m.captured(2).toInt(), y = m.captured(3).toInt();
    int h = m.captured(4).toInt();
    const int mi = m.captured(5).toInt(), se = m.captured(6).toInt();
    int ms = 0;
    if (m.capturedLength(7)) { QString f = m.captured(7); while (f.size() < 3) f += QLatin1Char('0'); ms = f.toInt(); }
    if (m.capturedLength(8)) {
        if (h < 1 || h > 12) return r;
        const bool pm = m.captured(8).at(0).toLower() == QLatin1Char('p');
        h = (h % 12) + (pm ? 12 : 0);
    }
    const QDate dmy(y, b, a), mdy(y, a, b);
    QDate date;
    if (order.compare(QLatin1String("mdy"), Qt::CaseInsensitive) == 0) date = mdy;
    else if (order.compare(QLatin1String("dmy"), Qt::CaseInsensitive) == 0) date = dmy;
    else {
        date = dmy.isValid() ? dmy : mdy;
        r.ambiguous = dmy.isValid() && mdy.isValid() && a != b;
    }
    const QTime time(h, mi, se, ms);
    if (!date.isValid() || !time.isValid()) { r.ambiguous = false; return r; }
    r.end = m.capturedEnd(0); r.date = date; r.time = time; r.hasMs = m.capturedLength(7) > 0; r.legacy = true;
    return r;
}

// Wall clock -> UTC in a zone. Most hours have one fixed offset, cached per (day, hour) so a 74k-line log does not
// build two zone-aware QDateTimes per line; only an hour holding a transition is resolved line by line.
class ZoneClock {
public:
    explicit ZoneClock(const QTimeZone& tz) : m_tz(tz), m_valid(tz.isValid()) {}
    bool valid() const { return m_valid; }

    qint64 resolve(QDate d, QTime t, qint64 prev, bool* ambiguous, bool* gap) {
        *ambiguous = *gap = false;
        const qint64 wall = wallMs(d, t);
        if (!m_valid) return wall;
        const qint64 hourKey = d.toJulianDay() * 24 + t.hour();
        auto it = m_fixed.constFind(hourKey);
        if (it == m_fixed.cend()) it = m_fixed.insert(hourKey, fixedOffset(d, t.hour()));
        if (*it != kNoFixed) return wall - qint64(*it) * 1000;
        using TR = QDateTime::TransitionResolution;
        const QDateTime before(d, t, m_tz, TR::PreferBefore), after(d, t, m_tz, TR::PreferAfter);
        if (before.isValid() && after.isValid() && before.toMSecsSinceEpoch() == after.toMSecsSinceEpoch()) return before.toMSecsSinceEpoch();
        if (before.isValid() && after.isValid() && before.date() == d && after.date() == d && before.time() == t && after.time() == t) {
            // Fall back: the wall clock happens twice. The first occurrence, unless it runs backwards from the line
            // above while the second does not (the log has moved into the repeated hour).
            *ambiguous = true;
            const qint64 first = before.toMSecsSinceEpoch(), second = after.toMSecsSinceEpoch();
            return (prev != kNoPrev && first < prev && second >= prev) ? second : first;
        }
        // Spring forward: the wall clock does not exist; read it with the offset in force before the jump.
        *gap = true;
        const QDateTime rel(d, t, m_tz, TR::RelativeToBefore);
        return rel.isValid() ? rel.toMSecsSinceEpoch() : wall - qint64(m_tz.offsetFromUtc(QDateTime(d, t, QTimeZone::UTC))) * 1000;
    }

private:
    static constexpr int kNoFixed = INT_MIN;
    int fixedOffset(QDate d, int h) const {
        using TR = QDateTime::TransitionResolution;
        const QTime t0(h, 0), t1(h, 59, 59, 999);
        const QDateTime a0(d, t0, m_tz, TR::PreferBefore), b0(d, t0, m_tz, TR::PreferAfter);
        const QDateTime a1(d, t1, m_tz, TR::PreferBefore), b1(d, t1, m_tz, TR::PreferAfter);
        if (!a0.isValid() || !b0.isValid() || !a1.isValid() || !b1.isValid()) return kNoFixed;
        if (a0.toMSecsSinceEpoch() != b0.toMSecsSinceEpoch() || a1.toMSecsSinceEpoch() != b1.toMSecsSinceEpoch()) return kNoFixed;
        if (a0.time() != t0 || a1.time() != t1 || a0.date() != d || a1.date() != d) return kNoFixed;
        const int off = a0.offsetFromUtc();
        return a1.offsetFromUtc() == off ? off : kNoFixed;
    }
    QTimeZone m_tz;
    bool m_valid = false;
    QHash<qint64, int> m_fixed;
};

QString lastComponent(const QString& path) {
    qsizetype end = path.size();
    while (end > 0 && (path.at(end - 1) == QLatin1Char('\\') || path.at(end - 1) == QLatin1Char('/'))) --end;
    qsizetype start = end;
    while (start > 0 && path.at(start - 1) != QLatin1Char('\\') && path.at(start - 1) != QLatin1Char('/')) --start;
    return path.mid(start, end - start);
}

QString parentFolder(const QString& path) {
    QStringList parts = QString(path).replace(QLatin1Char('/'), QLatin1Char('\\')).split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    return parts.size() >= 2 ? parts.at(parts.size() - 2) : QString();
}

bool isBoundary(const AsRunEntry& e, bool missedIsBoundary) {
    if (e.kind == AsRunKind::EndOfPlay) return true;
    return e.kind == AsRunKind::Media && (e.code == 0 || (e.code == 118 && missedIsBoundary));
}

void outOfOrder(QList<Diagnostic>& ds, int line, qint64 ms, qint64 prevMs, int prevLine, const QString& at, const QString& prevAt) {
    if (prevMs == kNoPrev || ms >= prevMs) return;
    ds << diag(line, 1, "MP004", QStringLiteral("stamped %1, earlier than line %2 (%3): the clock went back").arg(at, QString::number(prevLine), prevAt));
}

QJsonObject countsJson(const QHash<QString, int>& h) {
    QJsonObject o;
    for (auto it = h.cbegin(); it != h.cend(); ++it) o.insert(it.key(), it.value());
    return o;
}

QJsonObject severityCounts(const QList<Diagnostic>& ds) {
    int e = 0, w = 0, i = 0;
    for (const Diagnostic& d : ds) (d.severity == u"error" ? e : d.severity == u"warning" ? w : i)++;
    return {{"error", e}, {"warning", w}, {"info", i}};
}

QJsonArray diagnosticsJson(const QList<Diagnostic>& ds) {
    QJsonArray a;
    for (const Diagnostic& d : ds) a.append(d.toJson());
    return a;
}

bool startsCI(const QString& s, QLatin1String p) { return s.startsWith(p, Qt::CaseInsensitive); }
bool equalsCI(const QString& s, QLatin1String p) { return s.compare(p, Qt::CaseInsensitive) == 0; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

QString LogLine::stamp() const { return isoStamp(date, time, hasMs); }
QString AsRunEntry::stamp() const { return isoStamp(date, time, hasMs); }
QString MessageEntry::stamp() const { return isoStamp(date, time, hasMs); }

LogText readLog(const QByteArray& bytes, const ReadOptions& o) {
    LogText t;
    const Decoded d = decodeLines(bytes);
    t.encoding = d.encoding;
    t.physicalLines = int(d.lines.size());
    if (d.badLine) {
        t.diagnostics << diag(d.badLine, 0, "MP010", d.encoding == Encoding::Ansi1252
            ? QStringLiteral("not UTF-8 and no BOM: decoded as Windows-1252 (cp1252)")
            : QStringLiteral("bytes not valid in the BOM's encoding (%1): shown as U+FFFD").arg(encodingName(d.encoding)));
    }
    bool ambiguousReported = false;
    for (int i = 0; i < d.lines.size(); ++i) {
        const QString& s = d.lines.at(i);
        const int lineNo = i + 1;
        if (s.trimmed().isEmpty()) { ++t.blank; continue; }
        StampRead st = readIso(s);
        if (st.end < 0) st = readLegacy(s, o.legacyDateOrder);
        // <stamp> <whitespace> <3 digits> (end | one separator, then the text)
        qsizetype k = st.end;
        bool ok = k > 0 && k < s.size() && (s.at(k) == QLatin1Char(' ') || s.at(k) == QLatin1Char('\t'));
        while (ok && k < s.size() && (s.at(k) == QLatin1Char(' ') || s.at(k) == QLatin1Char('\t'))) ++k;
        const int code = ok ? digits(s, k, 3) : -1;
        ok = code >= 0 && (k + 3 == s.size() || s.at(k + 3) == QLatin1Char(' ') || s.at(k + 3) == QLatin1Char('\t'));
        if (!ok) {
            ++t.unparsed;
            t.diagnostics << diag(lineNo, 1, "MP001", QStringLiteral("not a MasterPlay log line (<stamp> <code> <text>): \"%1\"").arg(excerpt(s)));
            continue;
        }
        if (st.ambiguous && !ambiguousReported) {
            ambiguousReported = true;
            t.diagnostics << diag(lineNo, 1, "MP007", QStringLiteral("legacy stamp valid as DD/MM and MM/DD: read as DD/MM "
                                                                     "(set legacyDateOrder dmy or mdy); reported once"));
        }
        LogLine l;
        l.line = lineNo; l.date = st.date; l.time = st.time; l.hasMs = st.hasMs; l.legacyStamp = st.legacy; l.code = code;
        l.text = k + 3 < s.size() ? s.mid(k + 4) : QString();
        l.raw = s;
        t.lines << l;
    }
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------
// As-run

QString asRunKindName(AsRunKind k) {
    switch (k) {
    case AsRunKind::Media: return QStringLiteral("media");
    case AsRunKind::Command: return QStringLiteral("command");
    case AsRunKind::EndOfPlay: return QStringLiteral("endOfPlay");
    }
    return {};
}

QString fileOf(const QString& path) { return lastComponent(path); }

QString titleOf(const QString& file) {   // the ANFS rule: drop /\.[^.]+$/
    const qsizetype dot = file.lastIndexOf(QLatin1Char('.'));
    return dot >= 0 && dot < file.size() - 1 ? file.left(dot) : file;
}

QString inferType(const QString& path) {
    // playout.js TYPE_RULES, in order (JavaScript \b is ASCII word-based, as PCRE's default is), without the folder names
    // of any one station: "ads" and "commercial" cover the usual ad folders.
    static const QList<QPair<QRegularExpression, QString>> kRules = [] {
        const auto ci = QRegularExpression::CaseInsensitiveOption;
        return QList<QPair<QRegularExpression, QString>>{
            {QRegularExpression(QStringLiteral("\\bcommercial|\\bads?\\b"), ci), QStringLiteral("ad")},
            {QRegularExpression(QStringLiteral("\\bnews\\b|top story|crime report|news tip"), ci), QStringLiteral("news")},
            {QRegularExpression(QStringLiteral("\\bpromo|\\bpromos\\b"), ci), QStringLiteral("promo")},
            {QRegularExpression(QStringLiteral("\\bbumper|\\bid\\b|legal id|station id"), ci), QStringLiteral("bumper")},
            {QRegularExpression(QStringLiteral("\\bweather\\b|road report"), ci), QStringLiteral("weather")},
            {QRegularExpression(QStringLiteral("\\btrailer"), ci), QStringLiteral("promo")},
        };
    }();
    for (const auto& r : kRules) if (r.first.match(path).hasMatch()) return r.second;
    return QStringLiteral("unknown");
}

QJsonObject AsRunEntry::toJson() const {
    QJsonObject o{{"line", line}, {"at", stamp()}, {"startMs", startMs}, {"code", codeText(code)}, {"kind", asRunKindName(kind)},
                  {"body", body}, {"key", key}};
    if (!fields.isEmpty() && !(fields.size() == 1 && fields.first().isEmpty())) o.insert("fields", QJsonArray::fromStringList(fields));
    if (kind == AsRunKind::Media) {
        o.insert("path", path); o.insert("file", file); o.insert("title", title); o.insert("type", type);
        if (bare) o.insert("bare", true);
        if (prefetch) o.insert("prefetch", true);
    } else if (kind == AsRunKind::Command) {
        o.insert("instruction", instruction);
        if (!arg.isEmpty()) o.insert("arg", arg);
        o.insert("known", knownInstruction);
    }
    if (airing()) {
        o.insert("durationSec", durationMs >= 0 ? QJsonValue(double(durationMs) / 1000.0) : QJsonValue(QJsonValue::Null));
        o.insert("boundaryLine", boundaryLine);
        if (closedByEndOfPlay) o.insert("closedByEndOfPlay", true);
        if (truncated) o.insert("truncated", true);
        if (open) o.insert("open", true);
    }
    if (!reason.isEmpty()) { o.insert("reason", reason); o.insert("reasonLine", reasonLine); }
    if (dstAmbiguous) o.insert("dstAmbiguous", true);
    if (dstGap) o.insert("dstGap", true);
    return o;
}

AsRunLog parseAsRun(const QByteArray& bytes, const AsRunOptions& o) {
    ReadOptions ro;
    ro.legacyDateOrder = o.legacyDateOrder;
    LogText t = readLog(bytes, ro);
    AsRunLog log;
    log.encoding = t.encoding;
    log.physicalLines = t.physicalLines; log.blank = t.blank; log.unparsed = t.unparsed;
    log.diagnostics = t.diagnostics;
    log.capSec = o.capSec;
    ZoneClock clock(o.tz);
    log.naive = !clock.valid();
    if (!log.naive) log.tzId = QString::fromUtf8(o.tz.id());

    QHash<QString, int> seen;   // identical (stamp, code, text) lines so far: the key's ordinal
    struct Named { QString instruction, arg; bool known = false; };
    QHash<QString, Named> named; // command text -> catalog reading: a plant log repeats a handful of commands thousands of times
    QHash<QString, QString> types; // path -> inferType (six regexes; the same spots air all day)
    qint64 prevMs = kNoPrev;
    int prevLine = 0;
    QString prevAt;
    log.entries.reserve(t.lines.size());
    for (const LogLine& l : t.lines) {
        AsRunEntry e;
        e.line = l.line; e.date = l.date; e.time = l.time; e.hasMs = l.hasMs; e.code = l.code; e.text = l.text;
        const QStringList parts = l.text.split(QLatin1Char('\t'));
        e.body = parts.first().trimmed();
        if (e.body.isEmpty()) {
            ++log.unparsed;
            log.diagnostics << diag(l.line, 1, "MP001", QStringLiteral("as-run line with code %1 and nothing after it").arg(codeText(l.code)));
            continue;
        }
        e.fields = parts.mid(1);
        if (equalsCI(e.body, QLatin1String("End Of Play"))) e.kind = AsRunKind::EndOfPlay;
        else if (e.body.startsWith(QLatin1Char('!'))) {
            e.kind = AsRunKind::Command;
            // The text after the code is the instruction line as the playlist had it: read it with the playlist parser
            // so "!OSD off", "!Wiederholen" and both head forms name the same catalog row.
            auto it = named.constFind(l.text);
            if (it == named.cend()) {
                QString line = l.text;
                while (!line.isEmpty() && (line.at(0) == QLatin1Char(' ') || line.at(0) == QLatin1Char('\t'))) line.remove(0, 1);
                const Entry ent = parseLine(line);
                Named n;
                if (ent.kind == LineKind::Instruction) {
                    const Instruction ins = identify(ent);
                    n = {ins.name, ins.head, ins.known()};
                } else {
                    n.instruction = e.body.mid(1).trimmed();
                }
                it = named.insert(l.text, n);
            }
            e.instruction = it->instruction; e.arg = it->arg; e.knownInstruction = it->known;
        } else {
            e.kind = AsRunKind::Media;
            e.path = e.body;
            e.file = lastComponent(e.path);
            e.title = titleOf(e.file);
            e.bare = !e.path.contains(QLatin1Char('\\')) && !e.path.contains(QLatin1Char('/'));
            const QString parent = parentFolder(e.path);
            e.prefetch = !parent.isEmpty() && o.prefetchDirs.contains(parent, Qt::CaseInsensitive);
            auto ty = types.constFind(e.path);
            if (ty == types.cend()) ty = types.insert(e.path, inferType(e.path));
            e.type = *ty;
        }
        e.startMs = clock.resolve(e.date, e.time, prevMs, &e.dstAmbiguous, &e.dstGap);
        const QString at = e.stamp();
        outOfOrder(log.diagnostics, e.line, e.startMs, prevMs, prevLine, at, prevAt);
        prevMs = e.startMs; prevLine = e.line; prevAt = at;

        const QString base = at + QLatin1Char('-') + codeText(e.code) + QLatin1Char('-')
                           + QString::fromLatin1(QCryptographicHash::hash(e.text.toUtf8(), QCryptographicHash::Sha1).toHex().left(10));
        e.key = base + QLatin1Char('-') + QString::number(seen[base]++);
        log.entries << e;
    }

    // Derived durations, walking back so each airing sees the nearest boundary after it in one pass.
    const qint64 cap = qint64(qMax(0, o.capSec)) * 1000;
    int next = -1;
    for (qsizetype i = log.entries.size() - 1; i >= 0; --i) {
        AsRunEntry& e = log.entries[i];
        if (e.airing()) {
            if (next < 0) e.open = true;
            else {
                const AsRunEntry& b = log.entries.at(next);
                e.boundaryLine = b.line;
                e.closedByEndOfPlay = b.kind == AsRunKind::EndOfPlay;
                const qint64 gap = b.startMs - e.startMs;
                if (gap < 0 || gap > cap) e.truncated = true;
                else e.durationMs = gap;
            }
        }
        if (isBoundary(e, o.missedIsBoundary)) next = int(i);
    }
    sortDiagnostics(log.diagnostics);
    return log;
}

QJsonObject AsRunLog::stats() const {
    int played = 0, missed = 0, other = 0, airings = 0, commands = 0, eop = 0, known = 0, trunc = 0, open = 0, withReason = 0;
    qint64 playedMs = 0;
    QHash<QString, int> byCode, byInstruction;
    QSet<QString> files, missedFiles;
    for (const AsRunEntry& e : entries) {
        ++byCode[codeText(e.code)];
        if (e.code == 0) ++played; else if (e.code == 118) ++missed; else ++other;
        if (e.kind == AsRunKind::Command) { ++commands; ++byInstruction[e.instruction.isEmpty() ? QStringLiteral("!") : e.instruction]; }
        if (e.kind == AsRunKind::EndOfPlay) ++eop;
        if (e.airing()) {
            ++airings;
            files.insert(e.file.toLower());
            if (e.durationMs >= 0) { ++known; playedMs += e.durationMs; }
            if (e.truncated) ++trunc;
            if (e.open) ++open;
        }
        if (e.missed() && e.kind == AsRunKind::Media) {
            missedFiles.insert(e.file.toLower());
            if (!e.reason.isEmpty()) ++withReason;
        }
    }
    return {{"lines", physicalLines}, {"entries", int(entries.size())}, {"blank", blank}, {"unparsed", unparsed},
            {"played", played}, {"missed", missed}, {"other", other}, {"byCode", countsJson(byCode)},
            {"airings", airings}, {"commands", commands}, {"endOfPlay", eop},
            {"durationKnown", known}, {"truncated", trunc}, {"open", open}, {"playedSec", double(playedMs) / 1000.0},
            {"files", int(files.size())}, {"missedFiles", int(missedFiles.size())}, {"missedWithReason", withReason},
            {"byInstruction", countsJson(byInstruction)},
            {"first", entries.isEmpty() ? QString() : entries.first().stamp()},
            {"last", entries.isEmpty() ? QString() : entries.last().stamp()},
            {"naive", naive}, {"tz", tzId}, {"capSec", capSec}, {"diagnostics", severityCounts(diagnostics)}};
}

QJsonObject AsRunLog::toJson(int limit) const {
    QJsonArray rows;
    const qsizetype n = limit < 0 ? entries.size() : qMin<qsizetype>(limit, entries.size());
    for (qsizetype i = 0; i < n; ++i) rows.append(entries.at(i).toJson());
    return {{"encoding", encodingName(encoding)}, {"stats", stats()}, {"entries", rows}, {"diagnostics", diagnosticsJson(diagnostics)}};
}

// ---------------------------------------------------------------------------------------------------------------------
// Status.log / Operator.log / DebugFile.log

QString messageKindName(MessageKind k) {
    switch (k) {
    case MessageKind::Start: return QStringLiteral("start");
    case MessageKind::Close: return QStringLiteral("close");
    case MessageKind::OnAir: return QStringLiteral("onAir");
    case MessageKind::Output: return QStringLiteral("output");
    case MessageKind::FileNotFound: return QStringLiteral("fileNotFound");
    case MessageKind::PlaylistNotFound: return QStringLiteral("playlistNotFound");
    case MessageKind::Exception: return QStringLiteral("exception");
    case MessageKind::PlaylistChange: return QStringLiteral("playlistChange");
    case MessageKind::PlaylistChanged: return QStringLiteral("playlistChanged");
    case MessageKind::Button: return QStringLiteral("button");
    case MessageKind::ConfigSaved: return QStringLiteral("configSaved");
    case MessageKind::Other: return QStringLiteral("other");
    }
    return {};
}

QString severityName(int code) {
    switch (code) {
    case 1: return QStringLiteral("info");
    case 2: return QStringLiteral("minor");
    case 3: return QStringLiteral("major");
    case 4: return QStringLiteral("debug");
    case 9: return QStringLiteral("operator");
    case 10: return QStringLiteral("app");
    default: return {};
    }
}

namespace {

// MasterPlay 6.2's wording (Status.log, Operator.log, DebugFile.log), compared case-insensitively.
void classify(MessageEntry& m) {
    const QString s = m.text.trimmed();
    if (startsCI(s, QLatin1String("Start of Mplay"))) {
        m.kind = MessageKind::Start;
        const QString rest = s.mid(14).trimmed();
        const qsizetype c = rest.indexOf(QLatin1String("Computer:"), 0, Qt::CaseInsensitive);
        m.subject = (c < 0 ? rest : rest.left(c)).trimmed();
        if (c >= 0) m.computer = rest.mid(c + 9).trimmed();
    } else if (equalsCI(s, QLatin1String("Close of Mplay"))) m.kind = MessageKind::Close;
    else if (startsCI(s, QLatin1String("OnAir "))) { m.kind = MessageKind::OnAir; m.subject = s.mid(6).trimmed().toLower(); }
    else if (equalsCI(s, QLatin1String("Start Output"))) { m.kind = MessageKind::Output; m.subject = QStringLiteral("start"); }
    else if (equalsCI(s, QLatin1String("Stop Output"))) { m.kind = MessageKind::Output; m.subject = QStringLiteral("stop"); }
    else if (startsCI(s, QLatin1String("Failed to set device")) || startsCI(s, QLatin1String("FAILED TO ENABLE OUTPUT"))) {
        m.kind = MessageKind::Output; m.subject = QStringLiteral("error");
    } else if (startsCI(s, QLatin1String("File not found:"))) { m.kind = MessageKind::FileNotFound; m.subject = s.mid(15).trimmed(); }
    else if (startsCI(s, QLatin1String("Playlist not found:"))) { m.kind = MessageKind::PlaylistNotFound; m.subject = s.mid(19).trimmed(); }
    else if (startsCI(s, QLatin1String("Exception:"))) { m.kind = MessageKind::Exception; m.subject = s.mid(10).trimmed(); }
    else if (startsCI(s, QLatin1String("Change Playlist from "))) {
        // "Change Playlist from A to: B". A Windows path holds no ':' after its drive letter, so the last " to:" splits.
        const QString rest = s.mid(21);
        const qsizetype to = rest.lastIndexOf(QLatin1String(" to:"), -1, Qt::CaseInsensitive);
        m.kind = MessageKind::PlaylistChange;
        m.from = (to < 0 ? rest : rest.left(to)).trimmed();
        if (to >= 0) m.to = rest.mid(to + 4).trimmed();
    } else if (equalsCI(s, QLatin1String("Playlist changed"))) m.kind = MessageKind::PlaylistChanged;
    else if (s.endsWith(QLatin1String(" Button pushed"), Qt::CaseInsensitive)) { m.kind = MessageKind::Button; m.subject = s.chopped(14).trimmed(); }
    else if (equalsCI(s, QLatin1String("Configuration saved"))) m.kind = MessageKind::ConfigSaved;
}

} // namespace

QJsonObject MessageEntry::toJson() const {
    QJsonObject o{{"line", line}, {"at", stamp()}, {"code", codeText(code)}, {"severity", severityName(code)},
                  {"kind", messageKindName(kind)}, {"text", text}};
    if (!subject.isEmpty()) o.insert("subject", subject);
    if (kind == MessageKind::PlaylistChange) { o.insert("from", from); o.insert("to", to); }
    if (!computer.isEmpty()) o.insert("computer", computer);
    return o;
}

MessageLog parseMessages(const QByteArray& bytes, const ReadOptions& o) {
    const LogText t = readLog(bytes, o);
    MessageLog log;
    log.encoding = t.encoding;
    log.physicalLines = t.physicalLines; log.blank = t.blank; log.unparsed = t.unparsed;
    log.diagnostics = t.diagnostics;
    qint64 prevMs = kNoPrev;
    int prevLine = 0;
    QString prevAt;
    log.entries.reserve(t.lines.size());
    for (const LogLine& l : t.lines) {
        MessageEntry m;
        m.line = l.line; m.date = l.date; m.time = l.time; m.hasMs = l.hasMs; m.code = l.code; m.text = l.text;
        classify(m);
        const qint64 ms = wallMs(m.date, m.time);
        const QString at = m.stamp();
        outOfOrder(log.diagnostics, m.line, ms, prevMs, prevLine, at, prevAt);
        prevMs = ms; prevLine = m.line; prevAt = at;
        log.entries << m;
    }
    sortDiagnostics(log.diagnostics);
    return log;
}

QJsonObject MessageLog::stats() const {
    QHash<QString, int> byCode, bySeverity, byKind;
    for (const MessageEntry& m : entries) {
        ++byCode[codeText(m.code)];
        const QString sev = severityName(m.code);
        ++bySeverity[sev.isEmpty() ? QStringLiteral("unknown") : sev];
        ++byKind[messageKindName(m.kind)];
    }
    return {{"lines", physicalLines}, {"entries", int(entries.size())}, {"blank", blank}, {"unparsed", unparsed},
            {"byCode", countsJson(byCode)}, {"bySeverity", countsJson(bySeverity)}, {"byKind", countsJson(byKind)},
            {"first", entries.isEmpty() ? QString() : entries.first().stamp()},
            {"last", entries.isEmpty() ? QString() : entries.last().stamp()},
            {"diagnostics", severityCounts(diagnostics)}};
}

QJsonObject MessageLog::report(int maxList) const {
    QJsonArray sessions, playlistsNotFound, exceptions, changes, onAir, outputErrors;
    int nPlaylists = 0, nExceptions = 0, nChanges = 0, nOnAir = 0, nOutputErrors = 0, configSaved = 0;
    QHash<QString, int> buttons;
    struct Missing { QString file, first, last; int count = 0; };
    QList<Missing> missing;
    QHash<QString, int> missingAt;   // case-folded name -> index in missing
    QJsonObject session;
    auto keep = [maxList](QJsonArray& a, int& n, const QJsonObject& row) { if (maxList < 0 || n < maxList) a.append(row); ++n; };
    auto flush = [&] { if (!session.isEmpty()) sessions.append(session); session = {}; };
    for (const MessageEntry& m : entries) {
        const QString at = m.stamp();
        switch (m.kind) {
        case MessageKind::Start:
            flush();
            session = {{"start", at}, {"version", m.subject}, {"computer", m.computer}, {"line", m.line}};
            break;
        case MessageKind::Close:
            if (session.isEmpty() || session.contains("close")) { flush(); session = {{"line", m.line}}; }
            session.insert("close", at);
            flush();
            break;
        case MessageKind::FileNotFound: {
            const QString k = m.subject.toLower();
            auto it = missingAt.constFind(k);
            if (it == missingAt.cend()) { it = missingAt.insert(k, int(missing.size())); missing << Missing{m.subject, at, at, 0}; }
            Missing& x = missing[*it];
            ++x.count; x.last = at;
            break;
        }
        case MessageKind::PlaylistNotFound: keep(playlistsNotFound, nPlaylists, {{"at", at}, {"name", m.subject}, {"line", m.line}}); break;
        case MessageKind::Exception: keep(exceptions, nExceptions, {{"at", at}, {"text", m.subject}, {"line", m.line}}); break;
        case MessageKind::PlaylistChange: keep(changes, nChanges, {{"at", at}, {"from", m.from}, {"to", m.to}, {"line", m.line}}); break;
        case MessageKind::OnAir: keep(onAir, nOnAir, {{"at", at}, {"enabled", m.subject == u"enabled"}, {"line", m.line}}); break;
        case MessageKind::Output:
            if (m.subject == u"error") keep(outputErrors, nOutputErrors, {{"at", at}, {"text", m.text.trimmed()}, {"line", m.line}});
            break;
        case MessageKind::Button: ++buttons[m.subject]; break;
        case MessageKind::ConfigSaved: ++configSaved; break;
        default: break;
        }
    }
    flush();
    QJsonArray files;
    for (qsizetype i = 0; i < missing.size() && (maxList < 0 || i < maxList); ++i) {
        const Missing& x = missing.at(i);
        files.append(QJsonObject{{"file", x.file}, {"count", x.count}, {"first", x.first}, {"last", x.last}});
    }
    return {{"sessions", sessions}, {"filesNotFound", files}, {"filesNotFoundTotal", int(missing.size())},
            {"playlistsNotFound", playlistsNotFound}, {"playlistsNotFoundTotal", nPlaylists},
            {"exceptions", exceptions}, {"exceptionsTotal", nExceptions},
            {"playlistChanges", changes}, {"playlistChangesTotal", nChanges},
            {"onAir", onAir}, {"onAirTotal", nOnAir}, {"outputErrors", outputErrors}, {"outputErrorsTotal", nOutputErrors},
            {"buttons", countsJson(buttons)}, {"configSaved", configSaved}};
}

int attachReasons(AsRunLog& asrun, const MessageLog& status, int windowSec) {
    QHash<QString, QList<int>> byName;   // file name (case-folded) -> "File not found:" entries, file order
    for (int i = 0; i < status.entries.size(); ++i) {
        const MessageEntry& m = status.entries.at(i);
        if (m.kind == MessageKind::FileNotFound) byName[lastComponent(m.subject).toLower()] << i;
    }
    const qint64 window = qint64(qMax(0, windowSec)) * 1000;
    QHash<QString, int> cursor;
    int n = 0;
    for (AsRunEntry& e : asrun.entries) {
        if (!e.missed() || e.kind != AsRunKind::Media) continue;
        e.reason.clear(); e.reasonLine = 0;
        const QString k = e.file.toLower();
        const auto it = byName.constFind(k);
        if (it == byName.cend()) continue;
        // Both logs are written by the same process on the same clock: compare wall clocks, whatever zone the as-run
        // was read in. Status lines too early for this entry can match no later one either (file order is time order).
        const qint64 t = wallMs(e.date, e.time);
        int& c = cursor[k];
        const QList<int>& q = *it;
        auto ms = [&](int idx) { const MessageEntry& m = status.entries.at(q.at(idx)); return wallMs(m.date, m.time); };
        while (c < q.size() && ms(c) < t - window) ++c;
        if (c < q.size() && qAbs(ms(c) - t) <= window) {
            e.reason = QStringLiteral("File not found");
            e.reasonLine = status.entries.at(q.at(c)).line;
            ++c; ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// As-run DB writer (MP-09)

QJsonObject AsRunWriteResult::toJson() const {
    QJsonObject o{{"dryRun", dryRun},
                  {"rows", QJsonObject{{"played", played}, {"missed", missed}, {"commands", commands}}},
                  {"outOfRange", outOfRange},
                  {"inserted", inserted},
                  {"duplicates", duplicates},
                  {"transactions", transactions},
                  {"sample", sample}};
    if (dryRun) o["wouldInsert"] = wouldInsert;
    if (!error.isEmpty()) o["error"] = error;
    return o;
}

QList<sched::AsRunRow> asRunRows(const AsRunLog& log, const AsRunWriteOptions& o, AsRunWriteResult* counts) {
    QList<sched::AsRunRow> out;
    for (const AsRunEntry& e : log.entries) {
        const bool airing = e.airing();
        const bool missed = o.includeMissed && e.missed() && e.kind == AsRunKind::Media;
        const bool command = o.includeCommands && e.started() && e.kind == AsRunKind::Command;
        if (!airing && !missed && !command) continue;   // End Of Play and other codes: boundaries only
        if ((o.fromMs > 0 && e.startMs < o.fromMs) || (o.toMs > 0 && e.startMs > o.toMs)) {
            if (counts) ++counts->outOfRange;
            continue;
        }
        sched::AsRunRow r;
        r.channelId = o.channelId;
        r.runlistItemId = QStringLiteral("mp:") + e.key;
        r.origin = QStringLiteral("masterplay");
        r.plannedStartMs = r.actualStartMs = e.startMs;   // variance 0: the log knows no plan
        QJsonObject j{{"mpCode", QStringLiteral("%1").arg(e.code, 3, 10, QLatin1Char('0'))}, {"mpBody", e.body}, {"mpSource", o.source},
                      {"mpKey", e.key}, {"mpLine", e.line}};
        if (command) {
            r.eventType = QStringLiteral("CMD");
            r.type = QStringLiteral("command");
            r.title = QLatin1Char('!') + (e.instruction.isEmpty() ? e.body.mid(1).trimmed() : e.instruction);
            j["mpInstruction"] = e.instruction;
            if (!e.arg.isEmpty()) j["mpArg"] = e.arg;
            if (!e.fields.isEmpty()) j["mpFields"] = QJsonArray::fromStringList(e.fields);
            j["known"] = e.knownInstruction;
            if (counts) ++counts->commands;
        } else {
            r.title = e.title;
            QPair<QString, QString> hit;
            if (o.resolve) hit = o.resolve(e);
            r.type = hit.second.isEmpty() ? e.type : hit.second;
            j["typeDerived"] = hit.second.isEmpty();
            if (!hit.first.isEmpty()) {
                // Contract pacing: airedCount() counts START rows by asset_id on every channel.
                if (o.countTowardContracts) r.assetId = hit.first;
                else j["mpMediaId"] = hit.first;
            }
            j["mpPath"] = e.path;
            if (e.prefetch) j["prefetch"] = true;
            if (airing) {
                r.eventType = QStringLiteral("START");
                r.status = QStringLiteral("ON_TIME");
                j["durationDerived"] = e.durationMs >= 0;
                if (e.durationMs >= 0) {
                    r.plannedEndMs = r.actualEndMs = e.startMs + e.durationMs;
                    r.plannedDurationMs = r.actualDurationMs = e.durationMs;
                }
                if (e.truncated) j["truncated"] = true;
                if (e.open) j["open"] = true;
                if (e.closedByEndOfPlay) j["closedByEndOfPlay"] = true;
                if (counts) ++counts->played;
            } else {
                r.eventType = QStringLiteral("SKIP");
                r.status = QStringLiteral("SKIPPED");
                r.reason = e.reason.isEmpty() ? QStringLiteral("MasterPlay missed entry") : e.reason;
                if (e.reasonLine > 0) j["reasonLine"] = e.reasonLine;
                if (counts) ++counts->missed;
            }
        }
        if (e.dstAmbiguous) j["dstAmbiguous"] = true;
        if (e.dstGap) j["dstGap"] = true;
        r.extra = j;
        out << r;
    }
    return out;
}

namespace {

/// The writer proper; returns "" or the error. Every QSqlQuery dies here, before the caller removes the connection.
QString runAsRunWriter(QSqlDatabase& db, const QList<sched::AsRunRow>& rows, const AsRunWriteOptions& o, bool dryRun, AsRunWriteResult& res) {
    if (!db.open()) return QStringLiteral("cannot open %1: %2").arg(db.databaseName(), db.lastError().text());
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(qMax(0, o.busyTimeoutMs)));   // the connect option sets it too
    if (!q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name='as_run'")) || !q.next())
        return QStringLiteral("no as_run table in %1").arg(db.databaseName());
    q.finish();
    // The store opens the file in WAL mode already; only a file that is not gets switched (no lock needed when it is).
    if (!dryRun && q.exec(QStringLiteral("PRAGMA journal_mode")) && q.next() &&
        q.value(0).toString().compare(QLatin1String("wal"), Qt::CaseInsensitive) != 0) {
        q.finish();
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    }
    q.finish();

    QSqlQuery sel(db), ins(db);
    sel.prepare(QStringLiteral("SELECT 1 FROM as_run WHERE channel_id = ? AND runlist_item_id = ? AND event_type = ? LIMIT 1"));
    ins.prepare(QStringLiteral("INSERT INTO as_run (channel_id, runlist_item_id, item_id, asset_id, title, type, event_type, origin, planned_start, actual_start,"
                               " planned_end, actual_end, planned_duration, actual_duration, variance, frame, timecode, status, reason, slot_id, json, created_at)"
                               " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    QSet<QString> seen;   // keys are unique per log by construction; this guards a caller that concatenated logs
    const int batch = std::clamp(o.batchRows, 1, 100000);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < rows.size();) {
        // BEGIN IMMEDIATE takes the write lock up front: a concurrent store write waits on its busy handler, and this
        // side never needs a read-to-write lock upgrade (in WAL a stale snapshot fails that at once, busy handler or not).
        if (!dryRun && !q.exec(QStringLiteral("BEGIN IMMEDIATE")))
            return QStringLiteral("could not lock the database within %1 ms: %2").arg(o.busyTimeoutMs).arg(q.lastError().text());
        const int ins0 = res.inserted, dup0 = res.duplicates;
        auto fail = [&](const QString& what, const QSqlQuery& failed) {
            const QString e = QStringLiteral("%1: %2 (that transaction was rolled back; the %3 rows committed before it stay)")
                                  .arg(what, failed.lastError().text())
                                  .arg(ins0);
            if (!dryRun) q.exec(QStringLiteral("ROLLBACK"));
            res.inserted = ins0;
            res.duplicates = dup0;
            return e;
        };
        for (int n = 0; i < rows.size() && n < batch; ++i, ++n) {
            const sched::AsRunRow& r = rows.at(i);
            const QString sk = r.runlistItemId + QLatin1Char('|') + r.eventType;
            bool dup = seen.contains(sk);
            if (!dup) {
                sel.addBindValue(r.channelId);
                sel.addBindValue(r.runlistItemId);
                sel.addBindValue(r.eventType);
                if (!sel.exec()) return fail(QStringLiteral("dedup query failed"), sel);
                dup = sel.next();
                sel.finish();
            }
            if (dup) { ++res.duplicates; continue; }
            seen.insert(sk);
            if (dryRun) { ++res.wouldInsert; continue; }
            ins.addBindValue(r.channelId); ins.addBindValue(r.runlistItemId); ins.addBindValue(r.itemId); ins.addBindValue(r.assetId);
            ins.addBindValue(r.title); ins.addBindValue(r.type); ins.addBindValue(r.eventType); ins.addBindValue(r.origin);
            ins.addBindValue(r.plannedStartMs); ins.addBindValue(r.actualStartMs); ins.addBindValue(r.plannedEndMs); ins.addBindValue(r.actualEndMs);
            ins.addBindValue(r.plannedDurationMs); ins.addBindValue(r.actualDurationMs); ins.addBindValue(r.varianceMs); ins.addBindValue(r.frame);
            ins.addBindValue(r.timecode); ins.addBindValue(r.status); ins.addBindValue(r.reason); ins.addBindValue(r.slotId);
            ins.addBindValue(QString::fromUtf8(QJsonDocument(r.extra).toJson(QJsonDocument::Compact))); ins.addBindValue(now);
            if (!ins.exec()) return fail(QStringLiteral("insert failed at %1").arg(r.runlistItemId), ins);
            ++res.inserted;
        }
        if (!dryRun) {
            if (!q.exec(QStringLiteral("COMMIT"))) return fail(QStringLiteral("commit failed"), q);
            ++res.transactions;
        }
    }
    return {};
}

} // namespace

AsRunWriteResult writeAsRun(const QString& dbPath, const AsRunLog& log, const AsRunWriteOptions& o, bool dryRun) {
    AsRunWriteResult res;
    res.dryRun = dryRun;
    if (o.channelId.trimmed().isEmpty()) { res.error = QStringLiteral("no channel for the as-run rows"); return res; }
    const QList<sched::AsRunRow> rows = asRunRows(log, o, &res);
    for (int i = 0; i < qMin(qMax(0, o.sampleRows), int(rows.size())); ++i) res.sample.append(rows.at(i).toJson());
    if (dbPath.isEmpty()) {
        if (!dryRun) res.error = QStringLiteral("no database");
        else res.wouldInsert = int(rows.size());   // nothing to compare with
        return res;
    }
    // QSQLITE would create a missing file: an as-run import never creates a database.
    if (!QFileInfo(dbPath).isFile()) { res.error = QStringLiteral("no such database: %1").arg(dbPath); return res; }
    const QString conn = QStringLiteral("mpasrun_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        QString opts = QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(qMax(0, o.busyTimeoutMs));
        if (dryRun) opts += QStringLiteral(";QSQLITE_OPEN_READONLY");
        db.setConnectOptions(opts);
        res.error = runAsRunWriter(db, rows, o, dryRun, res);
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return res;
}

} // namespace mp::logs
