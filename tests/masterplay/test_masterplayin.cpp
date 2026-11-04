// MasterPlay .IN parser core (MP-02a): line classification, both stamp forms with the weekday table, durations, the
// TAB-only field model with ordered duplicate keys, the Document / Event model, and byte-identical serialize of
// untouched documents (the 5 D:\Masterplay samples and the synthetic grammar_basic.in). The plant corpus check runs
// only when PLAYPLAY_MP_SAMPLES points at a folder holding a scratch copy of the station playlist saved as plant_playlist.in.
// MP-02b: encoding detection (grammar_encodings_*.in: UTF-8 without BOM, UTF-16 LE / BE, mixed EOL, cp1252), the
// file-level diagnostics (MP003 / MP004 / MP007 / MP009 / MP010 / MP012), WriteOptions (canonical form, stamp form,
// encoding / EOL / final-newline overrides), semanticDiff and the diagnostic catalog.
// MP-03: the instruction catalog (masterplayinstructions.h): identify() with two-word names and aliases, the typed
// specs read from the fixture lines (grammar_instructions.in holds every catalog instruction), the in-band play mode
// beating the header (MP014), MP011 for unknown names / bad values / unknown keys, unknown instructions round-tripping
// verbatim, makeInstruction() and the instruction stats.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include "../../playout/schedule/masterplayin.h"
#include "../../playout/schedule/masterplayinstructions.h"

using namespace mp;

namespace {

QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const QStringList kInFixtures = {"FirstPlaylist.in", "OSD-Instructions.in", "Saturday_0200.in", "Testfiles.in", "sample.in",
                                 "grammar_basic.in", "grammar_encodings_utf8.in", "grammar_encodings_utf16le.in",
                                 "grammar_encodings_utf16be.in", "grammar_encodings_mixed.in", "grammar_encodings_cp1252.in",
                                 "grammar_instructions.in"};
// The four grammar_encodings files holding the same playlist (cp1252 has its own, representable content).
const QStringList kSharedEncodings = {"grammar_encodings_utf8.in", "grammar_encodings_utf16le.in", "grammar_encodings_utf16be.in",
                                      "grammar_encodings_mixed.in"};

const Diagnostic* findDiag(const Document& d, const QString& code) {
    for (const Diagnostic& g : d.diagnostics) if (g.code == code) return &g;
    return nullptr;
}
QString diagList(const Document& d) {   // "line:code:col ..." in order
    QStringList o;
    for (const Diagnostic& g : d.diagnostics) o << QString("%1:%2:%3").arg(g.line).arg(g.code).arg(g.col);
    return o.join(' ');
}
QByteArray utf16(const QString& s, bool le) {
    QByteArray b = le ? QByteArray("\xFF\xFE", 2) : QByteArray("\xFE\xFF", 2);
    for (QChar c : s) {
        const char lo = char(c.unicode() & 0xFF), hi = char(c.unicode() >> 8);
        b += le ? lo : hi;
        b += le ? hi : lo;
    }
    return b;
}

QList<int> lines(const Document& d, const QList<int>& idx) {
    QList<int> out;
    for (int i : idx) out << d.entries.at(i).line;
    return out;
}
QStringList keys(const QList<Param>& ps) { QStringList o; for (const Param& p : ps) o << p.key; return o; }
QStringList vals(const QList<Param>& ps) { QStringList o; for (const Param& p : ps) o << p.value; return o; }
const Entry& entryAtLine(const Document& d, int line) { return d.entries.at(line - 1); }   // a BOM is not a line

} // namespace

class TestMasterplayIn : public QObject {
    Q_OBJECT
private slots:
    // ---- byte-exact round trip ----------------------------------------------------------------------------------

    void roundTrip_data() {
        QTest::addColumn<QString>("file");
        for (const QString& f : kInFixtures) QTest::newRow(qPrintable(f)) << f;
    }
    void roundTrip() {
        QFETCH(QString, file);
        const QByteArray bytes = readAll(fx(file));
        QVERIFY2(!bytes.isEmpty(), qPrintable("missing fixture " + file));
        const Document d = parse(bytes);
        QCOMPARE(serialize(d), bytes);
        // The field model is lossless: every line renders back to its own text from the parsed fields...
        for (const Entry& e : d.entries)
            QVERIFY2(renderEntry(e) == e.raw, qPrintable(QString("%1:%2 renders as \"%3\"").arg(file).arg(e.line).arg(renderEntry(e))));
        // ...so a document with every line marked dirty (all of them re-rendered and re-encoded) is still byte-identical.
        Document all = d;
        for (Entry& e : all.entries) e.dirty = true;
        QCOMPARE(serialize(all), bytes);
        // And parsing the output again gives the same classification and events.
        const Document again = parse(serialize(all));
        QCOMPARE(again.entries.size(), d.entries.size());
        for (int i = 0; i < d.entries.size(); ++i) QCOMPARE(again.entries.at(i).kind, d.entries.at(i).kind);
        QCOMPARE(again.events.size(), d.events.size());
    }

    // ---- the five samples ---------------------------------------------------------------------------------------

    void sampleFirstPlaylist() {
        const Document d = parse(readAll(fx("FirstPlaylist.in")));
        QCOMPARE(d.encoding, Encoding::Utf8Bom);
        QVERIFY(d.hasBom());
        QCOMPARE(d.eolStyle(), QStringLiteral("crlf"));
        QVERIFY(d.finalNewline());
        QCOMPARE(d.entries.size(), 1);
        QCOMPARE(d.events.size(), 1);
        const Entry& h = d.header(d.events.first());
        QCOMPARE(h.kind, LineKind::Header);
        QCOMPARE(h.stamp.kind, DayKind::EveryDay);
        QCOMPARE(h.stamp.time, QTime(0, 0));
        QCOMPARE(keys(h.params), (QStringList{"EventDuration", "PlayMode"}));
        QCOMPARE(h.playMode, 1);
        QCOMPARE(h.eventDurationSec, 0.0);
        QVERIFY(d.events.first().items.isEmpty());
        QVERIFY(d.diagnostics.isEmpty());
    }

    void sampleOsdInstructions() {
        const Document d = parse(readAll(fx("OSD-Instructions.in")));
        QCOMPARE(d.encoding, Encoding::Utf8);                // pure ASCII, no BOM
        QVERIFY(!d.hasBom());
        QCOMPARE(d.events.size(), 1);
        const Event& ev = d.events.first();
        QVERIFY(d.header(ev).params.isEmpty());              // a bare stamp line is a header too
        QCOMPARE(d.header(ev).playMode, -1);
        QCOMPARE(ev.effectivePlayMode, 2);                   // MP-03: no header PlayMode, so the in-band !Repeat decides
        QCOMPARE(ev.repeatAt, 0);
        QStringList names;
        for (int i : ev.items) names << d.entries.at(i).instr;
        QCOMPARE(names, (QStringList{"Repeat", "ShowCrawl", "InsertDirectory", "ShowLogo", "InsertDirectory", "ShowClock", "InsertDirectory"}));
        const Entry& crawl = d.entries.at(ev.items.at(1));
        QCOMPARE(crawl.arg, QStringLiteral("Sample Crawl Text"));
        QCOMPARE(crawl.value("cfg"), QStringLiteral("D:\\Masterplay\\OSD\\DefaultCrawl.osd"));
        const Entry& dir = d.entries.at(ev.items.at(2));
        QCOMPARE(dir.arg, QStringLiteral("D:\\Media\\TestCards"));
        QCOMPARE(dir.value("SelectRandom"), QStringLiteral("2"));
        const Entry& logo = d.entries.at(ev.items.at(3));   // "!ShowLogo f=...": the head after the space is a field
        QVERIFY(logo.argWasField);
        QCOMPARE(logo.pad, QStringLiteral(" "));
        QVERIFY(logo.arg.isEmpty());
        QCOMPARE(logo.value("f"), QStringLiteral("D:\\Masterplay\\OSD\\Logo\\Masterplay Logo.bmp"));
        QCOMPARE(logo.value("cfg"), QStringLiteral("D:\\Masterplay\\OSD\\MasterPlay Logo.osd"));
        QCOMPARE(d.entries.at(ev.items.at(5)).value("t"), QStringLiteral("0"));
        QVERIFY(d.diagnostics.isEmpty());
    }

    void sampleSaturday() {
        const Document d = parse(readAll(fx("Saturday_0200.in")));
        QVERIFY(!d.finalNewline());
        QCOMPARE(d.entries.last().eol, QByteArray());
        QCOMPARE(d.eolStyle(), QStringLiteral("crlf"));
        const Entry& h = d.header(d.events.first());
        QCOMPARE(h.playMode, 2);
        QCOMPARE(h.eventDurationSec, 3600.0);
        QCOMPARE(d.events.first().effectivePlayMode, 2);
        QCOMPARE(d.events.first().items.size(), 1);
        const Entry& m = d.entries.at(d.events.first().items.first());
        QCOMPARE(m.kind, LineKind::Media);
        QVERIFY(m.path.startsWith("C:/Media/"));   // forward slashes are kept as written
        QVERIFY(m.path.endsWith(".mpg"));
        QVERIFY(m.bracketRaw.isEmpty() && m.params.isEmpty());
    }

    void sampleTestfiles() {
        const Document d = parse(readAll(fx("Testfiles.in")));
        const Entry& dir = d.entries.at(d.events.first().items.at(1));
        QCOMPARE(dir.instr, QStringLiteral("InsertDirectory"));
        QCOMPARE(dir.arg, QStringLiteral("D:\\Media\\TestCards"));
        QCOMPARE(dir.params.size(), 1);                       // the trailing TAB is an empty bare last field
        QVERIFY(dir.params.first().bare);
        QVERIFY(dir.params.first().value.isEmpty());
    }

    void sampleSample() {
        const Document d = parse(readAll(fx("sample.in")));
        QCOMPARE(d.encoding, Encoding::Utf8Bom);
        QCOMPARE(d.events.size(), 1);
        const Event& ev = d.events.first();
        QCOMPARE(ev.items.size(), 13);
        QCOMPARE(ev.effectivePlayMode, 2);
        const Entry& logo = d.entries.at(ev.items.at(3));   // "!ShowLogo<TAB>f=...": TAB form, same fields
        QCOMPARE(logo.instr, QStringLiteral("ShowLogo"));
        QVERIFY(logo.pad.isEmpty() && !logo.argWasField);
        QCOMPARE(logo.value("f"), QStringLiteral("D:\\Masterplay\\OSD\\Logo\\Masterplay Logo.bmp"));
        QCOMPARE(d.entries.at(ev.items.at(5)).value("t"), QStringLiteral("0"));
        const Entry& video = d.entries.at(ev.items.at(7));  // "!Video " keeps its trailing space
        QCOMPARE(video.instr, QStringLiteral("Video"));
        QCOMPARE(video.pad, QStringLiteral(" "));
        QVERIFY(video.arg.isEmpty() && video.params.isEmpty());
        QCOMPARE(d.entries.at(ev.items.at(8)).value("Name"), QStringLiteral("Announcement"));
        QCOMPARE(d.entries.at(ev.items.at(12)).instr, QStringLiteral("ChangePlaylist"));
        QVERIFY(d.diagnostics.isEmpty());
    }

    // ---- stamps -------------------------------------------------------------------------------------------------

    void stampTable_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("order");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<int>("kind");
        QTest::addColumn<int>("weekday");
        QTest::addColumn<QDate>("date");
        QTest::addColumn<bool>("legacy");
        QTest::addColumn<bool>("ambiguous");
        QTest::addColumn<QString>("formatted");     // formatStamp(stamp, !legacy); "" = same as text
        auto row = [](const char* name, const QString& text, int kind, int weekday = 0, QDate date = {}, bool legacy = false,
                      const QString& order = "auto", bool ambiguous = false, const QString& formatted = {}) {
            QTest::newRow(name) << text << order << true << kind << weekday << date << legacy << ambiguous << formatted;
        };
        auto bad = [](const char* name, const QString& text, const QString& order = "auto") {
            QTest::newRow(name) << text << order << false << int(DayKind::Invalid) << 0 << QDate() << false << false << QString();
        };
        row("iso every day", "0000-00-00T00:00:00", int(DayKind::EveryDay));
        row("legacy every day", "00/00/0000 00:00:00", int(DayKind::EveryDay), 0, {}, true);
        static const char* const kIso[] = {"iso 1 Sunday", "iso 2 Monday", "iso 3 Tuesday", "iso 4 Wednesday", "iso 5 Thursday", "iso 6 Friday", "iso 7 Saturday"};
        static const char* const kLeg[] = {"legacy 1 Sunday", "legacy 2 Monday", "legacy 3 Tuesday", "legacy 4 Wednesday", "legacy 5 Thursday", "legacy 6 Friday", "legacy 7 Saturday"};
        for (int c = 1; c <= 7; ++c) {
            row(kIso[c - 1], QString("0000-00-0%1T06:30:00").arg(c), int(DayKind::Weekly), c);
            row(kLeg[c - 1], QString("0%1/00/0000 06:30:00").arg(c), int(DayKind::Weekly), c, {}, true);
        }
        row("iso day change", "0000-00-80T04:00:00", int(DayKind::DayChange));
        row("legacy day change", "80/00/0000 04:00:00", int(DayKind::DayChange), 0, {}, true);
        row("iso end of list", "9999-99-99T00:00:00", int(DayKind::EndOfList));
        row("legacy end of list", "99/99/9999 00:00:00", int(DayKind::EndOfList), 0, {}, true);
        row("iso dated", "2025-09-10T09:07:00", int(DayKind::Dated), 0, QDate(2025, 9, 10));
        row("legacy dated dmy", "25/12/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 12, 25), true);
        row("legacy dated mdy by value", "12/25/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 12, 25), true, "auto", false, "25/12/2025 10:00:00");
        row("legacy ambiguous auto", "03/04/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 4, 3), true, "auto", true);
        row("legacy ambiguous dmy", "03/04/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 4, 3), true, "dmy");
        row("legacy ambiguous mdy", "03/04/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 3, 4), true, "mdy", false, "04/03/2025 10:00:00");
        row("legacy day equals month", "04/04/2025 10:00:00", int(DayKind::Dated), 0, QDate(2025, 4, 4), true);
        bad("iso day code 8", "0000-00-08T00:00:00");
        bad("iso day code 79", "0000-00-79T00:00:00");
        bad("iso month 13", "2025-13-01T00:00:00");
        bad("iso Feb 30", "2025-02-30T00:00:00");
        bad("iso hour 24", "0000-00-00T24:00:00");
        bad("iso minute 60", "0000-00-01T10:60:00");
        bad("legacy day code 8", "08/00/0000 00:00:00");
        bad("legacy year 0 with a month", "05/03/0000 00:00:00");
        bad("legacy no order fits", "31/31/2025 00:00:00");
        bad("legacy dmy forced on mdy", "12/25/2025 10:00:00", "dmy");
        bad("legacy mdy forced on dmy", "25/12/2025 10:00:00", "mdy");
        bad("iso with a space", "0000-00-00 00:00:00");
        bad("short", "0000-00-00T00:00");
        bad("lowercase t", "0000-00-00t00:00:00");
    }
    void stampTable() {
        QFETCH(QString, text); QFETCH(QString, order); QFETCH(bool, valid); QFETCH(int, kind); QFETCH(int, weekday);
        QFETCH(QDate, date); QFETCH(bool, legacy); QFETCH(bool, ambiguous); QFETCH(QString, formatted);
        ParseOptions o; o.legacyDateOrder = order;
        Stamp s; QString why;
        QCOMPARE(parseStamp(text, o, &s, &why), valid);
        QCOMPARE(int(s.kind), kind);
        QCOMPARE(s.raw, text);
        if (!valid) { QVERIFY(!why.isEmpty()); return; }
        QCOMPARE(s.weekday, weekday);
        QCOMPARE(s.date, date);
        QCOMPARE(s.legacy, legacy);
        QCOMPARE(s.ambiguous, ambiguous);
        QVERIFY(s.time.isValid());
        QCOMPARE(s.secondOfDay(), s.time.msecsSinceStartOfDay() / 1000);
        QCOMPARE(formatStamp(s, !s.legacy), formatted.isEmpty() ? text : formatted);
        // The same stamp as an event header line: TAB fields after it, nothing else changes.
        QList<Diagnostic> diags;
        const Entry h = parseLine(text + "\tPlayMode=1", o, &diags, 7);
        QCOMPARE(h.kind, LineKind::Header);
        QCOMPARE(int(h.stamp.kind), kind);
        QCOMPARE(h.stamp.weekday, weekday);
        QCOMPARE(h.playMode, 1);
        // An ambiguous legacy date is reported on its header (MP007, MP-02b); nothing else is.
        QCOMPARE(diags.size(), ambiguous ? 1 : 0);
        if (ambiguous) { QCOMPARE(diags.first().code, QStringLiteral("MP007")); QCOMPARE(diags.first().line, 7); QCOMPARE(diags.first().col, 1); }
    }

    void stampForms() {
        // ISO <-> legacy: the weekday code moves from the day field to the front.
        Stamp s;
        QVERIFY(parseStamp("0000-00-03T16:00:00", {}, &s));
        QCOMPARE(formatStamp(s, false), QStringLiteral("03/00/0000 16:00:00"));
        QVERIFY(parseStamp("07/00/0000 23:00:00", {}, &s));
        QCOMPARE(formatStamp(s, true), QStringLiteral("0000-00-07T23:00:00"));
        QVERIFY(parseStamp("2025-09-10T09:07:00", {}, &s));
        QCOMPARE(formatStamp(s, false), QStringLiteral("10/09/2025 09:07:00"));
        QVERIFY(parseStamp("80/00/0000 04:00:00", {}, &s));
        QCOMPARE(formatStamp(s, true), QStringLiteral("0000-00-80T04:00:00"));
        // A bad stamp is a header with MP002 (its items stay with it rather than joining the previous event).
        QList<Diagnostic> diags;
        const Entry h = parseLine("2025-13-01T00:00:00\tPlayMode=1", {}, &diags, 12);
        QCOMPARE(h.kind, LineKind::Header);
        QCOMPARE(h.stamp.kind, DayKind::Invalid);
        QCOMPARE(diags.size(), 1);
        QCOMPARE(diags.first().code, QStringLiteral("MP002"));
        QCOMPARE(diags.first().severity, QStringLiteral("error"));
        QCOMPARE(diags.first().line, 12);
        QCOMPARE(renderEntry(h), h.raw);
    }

    void weekdayCodes() {
        // MasterPlay: 1 Sunday .. 7 Saturday. Qt: 1 Monday .. 7 Sunday.
        QCOMPARE(qtDayOfWeek(1), int(Qt::Sunday));
        QCOMPARE(qtDayOfWeek(2), int(Qt::Monday));
        QCOMPARE(qtDayOfWeek(7), int(Qt::Saturday));
        QCOMPARE(weekdayCode(Qt::Sunday), 1);
        QCOMPARE(weekdayCode(Qt::Monday), 2);
        QCOMPARE(weekdayCode(Qt::Saturday), 7);
        for (int c = 1; c <= 7; ++c) QCOMPARE(weekdayCode(qtDayOfWeek(c)), c);
        QCOMPARE(weekdayName(1), QStringLiteral("Sunday"));
        QCOMPARE(weekdayName(7), QStringLiteral("Saturday"));
        QCOMPARE(qtDayOfWeek(0), 0); QCOMPARE(qtDayOfWeek(8), 0); QCOMPARE(weekdayCode(0), 0); QVERIFY(weekdayName(8).isEmpty());
        // The design's cross-check: plant code 02 airs the "MAR 31" and "APR 7" 2025 folders, both Mondays.
        QCOMPARE(weekdayCode(QDate(2025, 3, 31).dayOfWeek()), 2);
        QCOMPARE(weekdayCode(QDate(2025, 4, 7).dayOfWeek()), 2);
    }

    // ---- durations ----------------------------------------------------------------------------------------------

    void durations_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("fps");
        QTest::addColumn<bool>("ok");
        QTest::addColumn<double>("sec");
        auto good = [](const char* n, const QString& t, int fps, double sec) { QTest::newRow(n) << t << fps << true << sec; };
        auto bad = [](const char* n, const QString& t, int fps = 30) { QTest::newRow(n) << t << fps << false << 0.0; };
        good("hh:mm:ss", "00:47:20", 30, 2840);
        good("over 24 h", "24:23:59", 30, 87839);
        good("one-digit hour", "1:00:00", 30, 3600);
        good("bracket ;00", "00:00:10;00", 30, 10);
        good("bracket ;15 at 30", "00:00:10;15", 30, 10.5);
        good("dot frames at 25", "00:00:10.12", 25, 10.48);
        good("colon frames", "00:00:10:15", 30, 10.5);
        good("show value", "0:01:30:00", 30, 90);
        good("negative show value", "-0:00:10:00", 30, -10);
        good("seconds", "10", 30, 10);
        good("decimal seconds", "2.5", 30, 2.5);
        good("trimmed", " 00:00:05 ", 30, 5);
        good("fps 0 means 30", "00:00:01;29", 0, 1 + 29.0 / 30);
        bad("empty", "");
        bad("text", "abc");
        bad("minute 60", "00:60:00");
        bad("second 60", "00:00:60");
        bad("frames at fps", "00:00:10;30", 30);
        bad("frames at 25", "00:00:10;25", 25);
        bad("two parts", "1:2");
        bad("double minus", "--1");
        bad("minus only", "-");
        bad("junk frames", "00:00:10;5x");
        bad("five parts", "1:00:00:00:00");
        bad("xx:yy", "xx:yy");
    }
    void durations() {
        QFETCH(QString, text); QFETCH(int, fps); QFETCH(bool, ok); QFETCH(double, sec);
        bool got = !ok;
        const double v = parseDuration(text, fps, &got);
        QCOMPARE(got, ok);
        if (ok) QVERIFY2(qAbs(v - sec) < 1e-9, qPrintable(QString("%1 -> %2").arg(text).arg(v, 0, 'f', 6)));
        else QCOMPARE(v, 0.0);
    }

    void durationFormat() {
        QCOMPARE(formatDuration(2840), QStringLiteral("00:47:20"));
        QCOMPARE(formatDuration(87839), QStringLiteral("24:23:59"));
        QCOMPARE(formatDuration(0), QStringLiteral("00:00:00"));
        QCOMPARE(formatDuration(59.6), QStringLiteral("00:01:00"));
        QCOMPARE(formatDuration(-10), QStringLiteral("-00:00:10"));
        QCOMPARE(formatDuration(10.5, true, 30), QStringLiteral("00:00:10;15"));
        QCOMPARE(formatDuration(10.48, true, 25, QLatin1Char('.')), QStringLiteral("00:00:10.12"));
        QCOMPARE(formatBracket(10), QStringLiteral("[00:00:10;00]"));
        for (double s : {0.0, 1.0, 59.0, 61.5, 3599.0, 3600.0, 86399.0, 90000.0}) {
            bool ok = false;
            QCOMPARE(parseDuration(formatDuration(s, true, 30), 30, &ok), s);
            QVERIFY(ok);
        }
    }

    // ---- instruction heads: space vs TAB --------------------------------------------------------------------------

    void instructionHeads_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("instr");
        QTest::addColumn<QString>("pad");
        QTest::addColumn<QString>("arg");
        QTest::addColumn<bool>("argWasField");
        QTest::addColumn<QStringList>("keys");
        QTest::addColumn<QStringList>("values");
        auto row = [](const char* n, const QString& line, const QString& instr, const QString& pad, const QString& arg, bool f,
                      const QStringList& k, const QStringList& v) { QTest::newRow(n) << line << instr << pad << arg << f << k << v; };
        const QStringList logoKeys{"f", "cfg"}, logoVals{"D:\\Logo\\A B.bmp", "D:\\OSD\\L.osd"};
        row("logo space form", "!ShowLogo f=D:\\Logo\\A B.bmp\tcfg=D:\\OSD\\L.osd", "ShowLogo", " ", "", true, logoKeys, logoVals);
        row("logo tab form", "!ShowLogo\tf=D:\\Logo\\A B.bmp\tcfg=D:\\OSD\\L.osd", "ShowLogo", "", "", false, logoKeys, logoVals);
        row("clock space form", "!ShowClock t=0\tcfg=x.osd", "ShowClock", " ", "", true, {"t", "cfg"}, {"0", "x.osd"});
        row("clock tab form", "!ShowClock\tt=0\tcfg=x.osd", "ShowClock", "", "", false, {"t", "cfg"}, {"0", "x.osd"});
        row("folder argument", "!InsertDirectory C:\\Sync\\NEWS HUB\\TOP STORY\tLoadingMode=3\tToSelect=1\tMaxAge=", "InsertDirectory", " ",
            "C:\\Sync\\NEWS HUB\\TOP STORY", false, {"LoadingMode", "ToSelect", "MaxAge"}, {"3", "1", ""});
        row("text argument", "!ShowCrawl Sample Crawl Text\tcfg=c.osd", "ShowCrawl", " ", "Sample Crawl Text", false, {"cfg"}, {"c.osd"});
        row("trailing tab", "!InsertDirectory D:\\Masterplay\\Testfiles\\PAL\t", "InsertDirectory", " ", "D:\\Masterplay\\Testfiles\\PAL", false, {""}, {""});
        row("two spaces before a field", "!Break  #=1", "Break", "  ", "", true, {"#"}, {"1"});
        row("break tab form", "!Break\tgpicfg=1\t#=2", "Break", "", "", false, {"gpicfg", "#"}, {"1", "2"});
        row("break space form", "!Break gpicfg=1\t#=2", "Break", " ", "", true, {"gpicfg", "#"}, {"1", "2"});
        row("two-word name", "!OSD off", "OSD", " ", "off", false, {}, {});
        row("trailing space", "!Video ", "Video", " ", "", false, {}, {});
        row("name only", "!Repeat", "Repeat", "", "", false, {}, {});
        row("alias", "!Wiederholen", "Wiederholen", "", "", false, {}, {});
        row("name then tab", "!Normal\t", "Normal", "", "", false, {""}, {""});
        row("value with '='", "!Info\tName=a=b", "Info", "", "", false, {"Name"}, {"a=b"});
        row("space then tab", "!Macro \tInstructions=x", "Macro", " ", "", false, {"Instructions"}, {"x"});
    }
    void instructionHeads() {
        QFETCH(QString, line); QFETCH(QString, instr); QFETCH(QString, pad); QFETCH(QString, arg); QFETCH(bool, argWasField);
        QFETCH(QStringList, keys); QFETCH(QStringList, values);
        QList<Diagnostic> diags;
        const Entry e = parseLine(line, {}, &diags, 1);
        QCOMPARE(e.kind, LineKind::Instruction);
        QCOMPARE(e.instr, instr);
        QCOMPARE(e.pad, pad);
        QCOMPARE(e.arg, arg);
        QCOMPARE(e.argWasField, argWasField);
        QCOMPARE(::keys(e.params), keys);
        QCOMPARE(vals(e.params), values);
        QCOMPARE(renderEntry(e), line);                       // written back in the form it was read
        QVERIFY(diags.isEmpty());
    }

    // ---- field model --------------------------------------------------------------------------------------------

    void fieldModel() {
        const QString line = "\\\\server\\share\\Clips\\clip one.mpg\tName=First\tName=Second\tParameter=\tk=a=b\tbare\t=v";
        Entry e = parseLine(line);
        QCOMPARE(e.kind, LineKind::Media);
        QCOMPARE(e.path, QStringLiteral("\\\\server\\share\\Clips\\clip one.mpg"));
        QCOMPARE(e.params.size(), 6);
        QCOMPARE(e.values("Name"), (QStringList{"First", "Second"}));    // duplicates kept in order
        QCOMPARE(e.value("name"), QStringLiteral("First"));             // case-insensitive, first match
        QVERIFY(e.has("Parameter"));
        QCOMPARE(e.value("Parameter", "absent"), QString());            // present and empty
        QCOMPARE(e.value("Missing", "absent"), QStringLiteral("absent"));
        QCOMPARE(e.value("k"), QStringLiteral("a=b"));                   // split on the first '=' only
        QVERIFY(e.params.at(4).bare);
        QCOMPARE(e.params.at(4).value, QStringLiteral("bare"));
        QVERIFY(!e.params.at(5).bare);
        QCOMPARE(e.params.at(5).key, QString());
        QCOMPARE(e.params.at(5).value, QStringLiteral("v"));
        QCOMPARE(e.value("bare", "absent"), QStringLiteral("absent"));   // a bare field is not a key
        QCOMPARE(renderEntry(e), line);

        setParam(e.params, "NAME", "Changed");                          // first match, its key spelling kept
        QCOMPARE(e.params.at(0).key, QStringLiteral("Name"));
        QCOMPARE(e.values("Name"), (QStringList{"Changed", "Second"}));
        setParam(e.params, "actinout", "2");
        QCOMPARE(e.params.last().text(), QStringLiteral("actinout=2"));

        // Spaces, double spaces, '&', apostrophes and U+2019 are path characters, never separators.
        const QString path = QString("S:\\COMMERCIALS 2025\\Joe") + QChar(0x2019) + "s Diner  30 & Up's.mpg";
        const Entry m = parseLine(path + "\tactinout=0");
        QCOMPARE(m.path, path);
        QCOMPARE(m.value("actinout"), QStringLiteral("0"));
        // A header's fields are the same model: unknown keys and order are kept.
        const Entry h = parseLine("0000-00-00T00:00:00\tExpectedDuration=23:59:59\tEventType=2\tFutureKey=x\t");
        QCOMPARE(h.kind, LineKind::Header);
        QCOMPARE(h.expectedSec, 86399.0);
        QCOMPARE(h.eventType, 2);
        QCOMPARE(::keys(h.params), (QStringList{"ExpectedDuration", "EventType", "FutureKey", ""}));
        QCOMPARE(renderEntry(h), h.raw);
    }

    void mediaBrackets_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("bracket");
        QTest::addColumn<double>("forced");
        QTest::addColumn<QString>("code");          // the one diagnostic expected (at col 6), "" = none
        QTest::newRow("forced ;") << "D:\\Promos\\Weather.rtf[00:00:10;00]" << "D:\\Promos\\Weather.rtf" << "[00:00:10;00]" << 10.0 << "";
        QTest::newRow("forced :") << "x.rtf[00:00:10:15]" << "x.rtf" << "[00:00:10:15]" << 10.5 << "";
        QTest::newRow("forced .") << "x.rtf[00:00:10.15]" << "x.rtf" << "[00:00:10.15]" << 10.5 << "";
        QTest::newRow("with field") << "x.rtf[00:00:10;00]\tName=y" << "x.rtf" << "[00:00:10;00]" << 10.0 << "";
        QTest::newRow("seconds in the name") << "D:\\Wetter\\IntroWetter[5].TIF" << "D:\\Wetter\\IntroWetter[5].TIF" << "" << -1.0 << "";
        QTest::newRow("other bracket") << "x.mpg[foo]" << "x.mpg[foo]" << "" << -1.0 << "MP012";   // kept in the path, reported
        QTest::newRow("bracket alone") << "[00:00:10;00]" << "[00:00:10;00]" << "" << -1.0 << "";
        QTest::newRow("bad bracket") << "x.rtf[00:61:00;00]" << "x.rtf" << "[00:61:00;00]" << -1.0 << "MP013";
    }
    void mediaBrackets() {
        QFETCH(QString, line); QFETCH(QString, path); QFETCH(QString, bracket); QFETCH(double, forced); QFETCH(QString, code);
        QList<Diagnostic> dl;
        const Entry e = parseLine(line, {}, &dl, 3);
        QCOMPARE(e.kind, LineKind::Media);
        QCOMPARE(e.path, path);
        QCOMPARE(e.bracketRaw, bracket);
        QCOMPARE(e.forcedDurationSec, forced);
        QCOMPARE(dl.size(), code.isEmpty() ? 0 : 1);
        if (!code.isEmpty()) { QCOMPARE(dl.first().code, code); QCOMPARE(dl.first().col, 6); QCOMPARE(dl.first().line, 3); }
        QCOMPARE(renderEntry(e), line);
    }

    void headerFields() {
        QList<Diagnostic> dl;
        Entry h = parseLine("02/00/0000 06:00:00\tEventType=1\tName=Morning Show\tEventDuration=01:00:00\tPlayMode=1", {}, &dl, 10);
        QCOMPARE(h.kind, LineKind::Header);
        QCOMPARE(h.stamp.kind, DayKind::Weekly);
        QCOMPARE(h.stamp.weekday, 2);
        QVERIFY(h.stamp.legacy);
        QCOMPARE(h.stamp.time, QTime(6, 0));
        QCOMPARE(h.eventType, 1);
        QCOMPARE(h.name, QStringLiteral("Morning Show"));
        QCOMPARE(h.eventDurationSec, 3600.0);
        QCOMPARE(h.playMode, 1);
        QVERIFY(dl.isEmpty());
        // Values the editor could not have written are reported (with the field's column) and not acted on.
        h = parseLine("0000-00-00T00:00:00\tPlayMode=x", {}, &dl, 4);
        QCOMPARE(h.playMode, -1);
        QCOMPARE(dl.size(), 1);
        QCOMPARE(dl.last().code, QStringLiteral("MP011"));
        QCOMPARE(dl.last().col, 21);
        h = parseLine("0000-00-00T00:00:00\tEventType=5\tPlayMode=9", {}, &dl, 5);
        QCOMPARE(h.playMode, -1);
        QCOMPARE(h.eventType, 0);
        QCOMPARE(dl.size(), 3);
        h = parseLine("0000-00-00T00:00:00\tEventDuration=xx\tMaxAge=", {}, &dl, 6);
        QCOMPARE(h.eventDurationSec, -1.0);
        QCOMPARE(dl.last().code, QStringLiteral("MP013"));
        QCOMPARE(dl.last().col, 21);
        QCOMPARE(dl.last().line, 6);
        dl.clear();
        h = parseLine("0000-00-00T00:00:00\tEventDuration=\tPlayMode=0", {}, &dl, 7);   // empty: absent, no diagnostic
        QCOMPARE(h.eventDurationSec, -1.0);
        QCOMPARE(h.playMode, 0);
        QVERIFY(dl.isEmpty());
        h = parseLine("0000-00-00T00:00:00");
        QCOMPARE(h.kind, LineKind::Header);
        QVERIFY(h.params.isEmpty());
        QCOMPARE(renderEntry(h), QStringLiteral("0000-00-00T00:00:00"));
    }

    // ---- unknown lines ------------------------------------------------------------------------------------------

    void unknownLines_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<int>("col");
        QTest::newRow("hash") << "# comment" << 1;
        QTest::newRow("semicolon") << "; comment" << 1;
        QTest::newRow("double slash") << "// comment" << 1;
        QTest::newRow("indented instruction") << "  !Repeat" << 3;
        QTest::newRow("tab-indented") << "\t!Repeat" << 2;
        QTest::newRow("bang only") << "!" << 1;
        QTest::newRow("bang space") << "! Repeat" << 1;
        QTest::newRow("bang tab") << "!\tx=1" << 1;
        QTest::newRow("stamp then space") << "0000-00-00T00:00:00 PlayMode=1" << 20;
        QTest::newRow("legacy stamp then space") << "01/00/0000 00:00:00 PlayMode=1" << 20;
        QTest::newRow("stray BOM") << QString(QChar(0xFEFF)) + "0000-00-00T00:00:00" << 1;
    }
    void unknownLines() {
        QFETCH(QString, line); QFETCH(int, col);
        QList<Diagnostic> dl;
        const Entry e = parseLine(line, {}, &dl, 42);
        QCOMPARE(e.kind, LineKind::Unknown);
        QCOMPARE(dl.size(), 1);
        QCOMPARE(dl.first().code, QStringLiteral("MP001"));
        QCOMPARE(dl.first().line, 42);
        QCOMPARE(dl.first().col, col);
        QCOMPARE(renderEntry(e), line);                       // kept verbatim
    }

    void blankLines() {
        for (const QString& s : {QString(), QString(" "), QString("\t"), QString(" \t ")}) {
            QList<Diagnostic> dl;
            const Entry e = parseLine(s, {}, &dl, 1);
            QCOMPARE(e.kind, LineKind::Blank);
            QVERIFY(dl.isEmpty());
            QCOMPARE(renderEntry(e), s);
        }
    }

    // ---- the synthetic grammar file -----------------------------------------------------------------------------

    void grammarBasic() {
        const QByteArray bytes = readAll(fx("grammar_basic.in"));
        // The fixture's own properties first, so an autocrlf checkout or an editor save fails here with a reason.
        QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
        QCOMPARE(bytes.count("\r\n"), 50);
        QCOMPARE(bytes.count('\n'), 50);
        QVERIFY(bytes.endsWith("9999-99-99T00:00:00\r\n"));

        const Document d = parse(bytes);
        QCOMPARE(d.encoding, Encoding::Utf8Bom);
        QCOMPARE(d.eolStyle(), QStringLiteral("crlf"));
        QCOMPARE(d.defaultEol(), QByteArray("\r\n"));
        QVERIFY(d.finalNewline());
        QCOMPARE(d.entries.size(), 50);
        for (int i = 0; i < d.entries.size(); ++i) QCOMPARE(d.entries.at(i).line, i + 1);

        // Diagnostics: exact codes and line numbers, sorted by line then column. MP004 (MP-02b): line 36 (weekly 1)
        // follows weekly 7, line 48 (every day) follows the dated events. MP014 (MP-03): line 32 says PlayMode=1 but its
        // first play-mode instruction is !Break (35), line 41 says PlayMode=1 above !Wiederholen (42).
        QList<QPair<int, QString>> got;
        for (const Diagnostic& g : d.diagnostics) got << qMakePair(g.line, g.code);
        const QList<QPair<int, QString>> want = {{1, "MP008"}, {2, "MP001"}, {30, "MP001"}, {31, "MP001"}, {32, "MP014"}, {36, "MP004"},
                                                 {41, "MP014"}, {44, "MP001"}, {45, "MP001"}, {46, "MP001"}, {47, "MP002"}, {48, "MP004"},
                                                 {48, "MP013"}};
        QCOMPARE(got, want);
        QCOMPARE(d.diagnostics.at(12).col, 21);                 // the EventDuration field
        QCOMPARE(d.diagnostics.at(8).col, 3);                   // the first non-blank character
        QVERIFY(d.diagnostics.at(11).message.contains("line 41"));  // MP004 names the header above it
        QCOMPARE(d.diagnostics.at(4).col, 56);                  // MP014 points at the header's PlayMode field
        QVERIFY(d.diagnostics.at(4).message.contains("\"!Break\" (line 35)"));
        QCOMPARE(d.diagnostics.at(6).col, 47);
        QVERIFY(d.diagnostics.at(6).message.contains("\"!Wiederholen\" (line 42) sets 2 (repeat)"));

        // Events and their items.
        QList<int> headerLines, itemCounts;
        for (const Event& ev : d.events) { headerLines << d.header(ev).line; itemCounts << int(ev.items.size()); }
        QCOMPARE(headerLines, (QList<int>{3, 7, 10, 14, 20, 24, 28, 32, 36, 37, 38, 39, 41, 47, 48, 50}));
        QCOMPARE(itemCounts, (QList<int>{2, 2, 3, 5, 3, 3, 1, 3, 0, 0, 0, 1, 2, 0, 1, 0}));
        QCOMPARE(lines(d, d.preamble), QList<int>{1});
        QCOMPARE(d.events.at(3).end, 19);                       // index of line 20's header: one past line 19
        QCOMPARE(d.events.last().end, 50);
        QCOMPARE(lines(d, d.events.at(3).items), (QList<int>{15, 16, 17, 18, 19}));
        QCOMPARE(lines(d, d.events.at(6).items), QList<int>{29});   // the ';' and '//' lines are not items
        QCOMPARE(lines(d, d.events.at(12).items), (QList<int>{42, 43}));

        // Stats.
        const QJsonObject st = d.stats();
        QCOMPARE(st["lines"].toInt(), 50);
        QCOMPARE(st["headers"].toInt(), 16);
        QCOMPARE(st["events"].toInt(), 16);
        QCOMPARE(st["instructions"].toInt(), 16);
        QCOMPARE(st["media"].toInt(), 11);
        QCOMPARE(st["unknown"].toInt(), 6);
        QCOMPARE(st["blank"].toInt(), 1);
        QCOMPARE(st["preamble"].toInt(), 1);
        QCOMPARE(st["items"].toInt(), 27);
        const QJsonObject byDay = st["byDayKind"].toObject();
        QCOMPARE(byDay["everyDay"].toInt(), 2);
        QCOMPARE(byDay["weekly"].toInt(), 9);
        QCOMPARE(byDay["dated"].toInt(), 2);
        QCOMPARE(byDay["dayChange"].toInt(), 1);
        QCOMPARE(byDay["endOfList"].toInt(), 1);
        QCOMPARE(byDay["invalid"].toInt(), 1);
        const QJsonObject byWd = st["byWeekday"].toObject();
        const int wantWd[] = {2, 1, 1, 1, 1, 1, 2};
        for (int c = 1; c <= 7; ++c) QCOMPARE(byWd[QString::number(c)].toInt(), wantWd[c - 1]);
        const QJsonObject byIns = st["byInstruction"].toObject();
        QCOMPARE(byIns["Break"].toInt(), 3);
        QCOMPARE(byIns["InsertDirectory"].toInt(), 2);
        QCOMPARE(byIns["ShowLogo"].toInt(), 2);
        QCOMPARE(byIns["OSD"].toInt(), 1);
        QCOMPARE(byIns["Wiederholen"].toInt(), 1);
        QCOMPARE(byIns.size(), 12);
        const QJsonObject sev = st["diagnostics"].toObject();
        QCOMPARE(sev["error"].toInt(), 1);
        QCOMPARE(sev["warning"].toInt(), 10);
        QCOMPARE(sev["info"].toInt(), 2);
        QCOMPARE(sev["byCode"].toObject()["MP001"].toInt(), 6);
        QCOMPARE(sev["byCode"].toObject()["MP004"].toInt(), 2);
        QCOMPARE(sev["byCode"].toObject()["MP014"].toInt(), 2);

        // Individual lines.
        const Entry& morning = entryAtLine(d, 10);
        QCOMPARE(morning.eventType, 1);
        QCOMPARE(morning.name, QStringLiteral("Morning Show"));
        QCOMPARE(morning.stamp.weekday, 2);
        QCOMPARE(entryAtLine(d, 14).playMode, 4);
        QCOMPARE(d.events.at(3).effectivePlayMode, 4);
        QCOMPARE(entryAtLine(d, 15).value("#"), QStringLiteral("1"));
        QCOMPARE(entryAtLine(d, 18).value("#"), QStringLiteral("2"));
        QCOMPARE(entryAtLine(d, 18).value("gpicfg"), QStringLiteral("1"));
        QCOMPARE(entryAtLine(d, 35).value("#"), QStringLiteral("3"));
        QCOMPARE(entryAtLine(d, 8).path, QString("S:\\COMMERCIALS 2025\\Joe") + QChar(0x2019) + "s Diner  30 & Up.mpg");
        QCOMPARE(entryAtLine(d, 13).path, QString("D:\\Promos\\Weather 72") + QChar(0x00B0) + ".rtf");
        QCOMPARE(entryAtLine(d, 13).forcedDurationSec, 10.0);
        QCOMPARE(entryAtLine(d, 22).values("Name"), (QStringList{"First", "Second"}));
        QCOMPARE(entryAtLine(d, 12).value("MaxAge", "absent"), QString());
        QCOMPARE(entryAtLine(d, 23).arg, QStringLiteral("off"));
        QCOMPARE(entryAtLine(d, 32).eventType, 2);
        QCOMPARE(entryAtLine(d, 37).value("IncludeInComingUp"), QStringLiteral("1"));
        QCOMPARE(entryAtLine(d, 38).stamp.kind, DayKind::DayChange);
        QCOMPARE(entryAtLine(d, 39).stamp.date, QDate(2025, 9, 10));
        QCOMPARE(entryAtLine(d, 41).stamp.date, QDate(2025, 12, 25));
        QCOMPARE(entryAtLine(d, 41).expectedSec, 300.0);
        QCOMPARE(entryAtLine(d, 50).stamp.kind, DayKind::EndOfList);
        QCOMPARE(entryAtLine(d, 6).kind, LineKind::Blank);
    }

    void jsonView() {
        const Document d = parse(readAll(fx("grammar_basic.in")));
        const QJsonObject j = d.toJson();
        QCOMPARE(j["encoding"].toString(), QStringLiteral("utf8bom"));
        QCOMPARE(j["bom"].toBool(), true);
        QCOMPARE(j["eol"].toString(), QStringLiteral("crlf"));
        const QJsonArray evs = j["events"].toArray();
        QCOMPARE(evs.size(), 16);
        const QJsonObject e2 = evs.at(1).toObject();
        QCOMPARE(e2["line"].toInt(), 7);
        QCOMPARE(e2["stamp"].toObject()["kind"].toString(), QStringLiteral("weekly"));
        QCOMPARE(e2["stamp"].toObject()["weekdayName"].toString(), QStringLiteral("Sunday"));
        QCOMPARE(e2["durationSec"].toDouble(), 1800.0);
        QCOMPARE(e2["items"].toArray().size(), 2);
        QCOMPARE(e2["items"].toArray().at(0).toObject()["params"].toArray().at(0).toObject()["key"].toString(), QStringLiteral("actinout"));
        QCOMPARE(evs.at(2).toObject()["items"].toArray().at(2).toObject()["bracket"].toString(), QStringLiteral("[00:00:10;00]"));
        QCOMPARE(j["preamble"].toArray().size(), 1);
        QCOMPARE(j["diagnostics"].toArray().size(), 13);
        // MP-03: instruction items carry the catalog name; events their in-band mode and loop start.
        const QJsonObject osdOff = evs.at(4).toObject()["items"].toArray().at(2).toObject();
        QCOMPARE(osdOff["instr"].toString(), QStringLiteral("OSD"));
        QCOMPARE(osdOff["name"].toString(), QStringLiteral("OSD off"));
        QVERIFY(!osdOff.contains("unknown"));
        const QJsonObject held = evs.at(12).toObject();       // line 41: PlayMode=1 over !Wiederholen
        QCOMPARE(held["items"].toArray().at(0).toObject()["name"].toString(), QStringLiteral("Repeat"));
        QCOMPARE(held["playMode"].toInt(), 1);
        QCOMPARE(held["inBandPlayMode"].toInt(), 2);
        QCOMPARE(held["effectivePlayMode"].toInt(), 2);
        QCOMPARE(held["repeatAt"].toInt(), 0);
        QVERIFY(!evs.at(1).toObject().contains("inBandPlayMode") && !evs.at(1).toObject().contains("repeatAt"));
        QCOMPARE(j["diagnostics"].toArray().at(0).toObject()["code"].toString(), QStringLiteral("MP008"));
        QVERIFY(!j.contains("unknown"));
        const QJsonObject r = d.toJson(true);
        QCOMPARE(r["unknown"].toArray().size(), 6);
        QCOMPARE(r["unknown"].toArray().at(1).toObject()["line"].toInt(), 30);
        QCOMPARE(r["events"].toArray().at(0).toObject()["items"].toArray().at(0).toObject()["raw"].toString(), QStringLiteral("!Repeat"));
    }

    // ---- edits and encodings ------------------------------------------------------------------------------------

    void dirtyEditsTouchOnlyTheirLine() {
        const QByteArray bytes = readAll(fx("grammar_basic.in"));
        Document d = parse(bytes);
        Entry& h = d.entries[2];                             // line 3
        setParam(h.params, "EventDuration", "23:00:00");
        h.dirty = true;
        Entry& w = d.entries[35];                            // line 36: move the ISO Sunday event to Tuesday
        w.stamp.weekday = 3;
        w.stamp.raw.clear();
        w.dirty = true;
        const QList<QByteArray> before = bytes.split('\n'), after = serialize(d).split('\n');
        QCOMPARE(after.size(), before.size());
        for (int i = 0; i < before.size(); ++i) {
            if (i == 2) QCOMPARE(after.at(i), QByteArray("0000-00-00T00:00:00\tEventDuration=23:00:00\tPlayMode=2\r"));
            else if (i == 35) QCOMPARE(after.at(i), QByteArray("0000-00-03T05:00:00\tEventDuration=00:10:00\tPlayMode=1\r"));
            else QCOMPARE(after.at(i), before.at(i));
        }
        // A legacy header re-renders in its own form.
        Entry& leg = d.entries[6];                           // line 7
        leg.stamp.weekday = 4;
        leg.stamp.raw.clear();
        leg.dirty = true;
        QCOMPARE(renderEntry(leg), QStringLiteral("04/00/0000 06:00:00\tEventDuration=00:30:00\tPlayMode=1"));
    }

    void dirtyEditsCannotInjectLines() {
        Document d = parse(readAll(fx("Saturday_0200.in")));
        Entry& m = d.entries[1];
        m.path = "evil\tPlayMode=9\r\n0000-00-00T00:00:00\nx";
        setParam(m.params, "Name", "a\tb");
        m.dirty = true;
        const QByteArray out = serialize(d);
        const Document again = parse(out);
        QCOMPARE(again.entries.size(), 2);                   // no field and no line was added
        QCOMPARE(again.entries.at(1).kind, LineKind::Media);
        QCOMPARE(again.entries.at(1).path, QStringLiteral("evil PlayMode=9  0000-00-00T00:00:00 x"));
        QCOMPARE(again.entries.at(1).value("Name"), QStringLiteral("a b"));
        QCOMPARE(again.entries.at(1).params.size(), 1);
    }

    void appendAfterLastLineWithoutNewline() {
        const QByteArray bytes = readAll(fx("Saturday_0200.in"));
        Document d = parse(bytes);
        d.entries.append(parseLine("!OSD off", {}, nullptr, 3));   // built by parseLine: no original bytes, eol ""
        QCOMPARE(serialize(d), bytes + "\r\n!OSD off");      // the old last line gets the document's terminator
        d.entries.last().eol = "\r\n";
        QCOMPARE(serialize(d), bytes + "\r\n!OSD off\r\n");
    }

    void encodingBasics() {
        QCOMPARE(parse(QByteArray()).entries.size(), 0);
        QCOMPARE(serialize(parse(QByteArray())), QByteArray());
        QVERIFY(!parse(QByteArray()).finalNewline());
        QCOMPARE(parse(QByteArray()).eolStyle(), QString());

        const QByteArray bomOnly("\xEF\xBB\xBF");
        const Document b = parse(bomOnly);
        QCOMPARE(b.entries.size(), 0);
        QVERIFY(b.hasBom());
        QCOMPARE(serialize(b), bomOnly);

        const QByteArray utf8 = "S:\\Caf\xC3\xA9.mpg\r\n";       // valid UTF-8 without a BOM
        Document u = parse(utf8);
        QCOMPARE(u.encoding, Encoding::Utf8);
        QCOMPARE(u.entries.first().path, QString("S:\\Caf") + QChar(0xE9) + ".mpg");
        u.entries.first().dirty = true;
        QCOMPARE(serialize(u), utf8);

        // Not UTF-8: decoded as cp1252 (0x92 is U+2019), written back byte for byte, edits re-encode to cp1252. MP010
        // points at the first byte that broke UTF-8.
        const QByteArray ansi = "S:\\Joe\x92s \x81 \xE9.mpg\tName=\x93x\x94\r\n";
        Document a = parse(ansi);
        QCOMPARE(a.encoding, Encoding::Ansi1252);
        QVERIFY(!a.hasBom());
        QVERIFY(findDiag(a, "MP010"));
        QCOMPARE(findDiag(a, "MP010")->col, 7);
        QVERIFY(findDiag(a, "MP010")->message.contains("0x92"));
        QCOMPARE(a.entries.first().path, QString("S:\\Joe") + QChar(0x2019) + "s " + QChar(0x81) + " " + QChar(0xE9) + ".mpg");
        QCOMPARE(a.entries.first().value("Name"), QString(QChar(0x201C)) + "x" + QChar(0x201D));
        QCOMPARE(serialize(a), ansi);
        a.entries.first().dirty = true;
        QCOMPARE(serialize(a), ansi);
        // Invalid UTF-8 after a BOM still round-trips: the original bytes are what gets written.
        const QByteArray broken = QByteArray("\xEF\xBB\xBF") + "S:\\bad\xFF.mpg\r\n";
        QCOMPARE(serialize(parse(broken)), broken);
        QCOMPARE(parse(broken).encoding, Encoding::Utf8Bom);         // the BOM decides; the bad byte is reported
        QVERIFY(findDiag(parse(broken), "MP010"));
        QCOMPARE(findDiag(parse(broken), "MP010")->col, 7);

        // Per-line terminators: LF-only, a mix, and a lone CR (part of the text: only LF ends a line).
        const QByteArray lf = "0000-00-00T00:00:00\tPlayMode=1\nS:\\a.mpg\n";
        QCOMPARE(parse(lf).eolStyle(), QStringLiteral("lf"));
        QCOMPARE(parse(lf).defaultEol(), QByteArray("\n"));
        QCOMPARE(serialize(parse(lf)), lf);
        const QByteArray mixed = "0000-00-00T00:00:00\nS:\\a.mpg\r\nS:\\b.mpg";
        QCOMPARE(parse(mixed).eolStyle(), QStringLiteral("mixed"));
        QCOMPARE(serialize(parse(mixed)), mixed);
        const Document cr = parse("S:\\a\rb.mpg\r\n");
        QCOMPARE(cr.entries.size(), 1);
        QCOMPARE(cr.entries.first().path, QStringLiteral("S:\\a\rb.mpg"));
        QCOMPARE(serialize(cr), QByteArray("S:\\a\rb.mpg\r\n"));
        QVERIFY(findDiag(cr, "MP009"));                               // MasterPlay itself would break the line there
        QVERIFY(!findDiag(parse(lf), "MP009"));
        QVERIFY(findDiag(parse(mixed), "MP009"));
    }

    void legacyDateOrderOption() {
        ParseOptions mdy; mdy.legacyDateOrder = "MDY";       // case-insensitive
        const Document d = parse("03/04/2025 10:00:00\tPlayMode=1\r\n", mdy);
        QCOMPARE(d.header(d.events.first()).stamp.date, QDate(2025, 3, 4));
        QVERIFY(!d.header(d.events.first()).stamp.ambiguous);
        const Document a = parse("03/04/2025 10:00:00\tPlayMode=1\r\n");
        QCOMPARE(a.header(a.events.first()).stamp.date, QDate(2025, 4, 3));
        QVERIFY(a.header(a.events.first()).stamp.ambiguous);
    }

    // ---- MP-02b: encodings, terminators, file-level diagnostics, write options -------------------------------------

    void encodingFixtures_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<int>("encoding");
        QTest::addColumn<QString>("eol");
        QTest::addColumn<bool>("finalNewline");
        QTest::addColumn<int>("lines");
        QTest::addColumn<QString>("diags");
        // The shared playlist: line 10 date-window bracket, line 11 the time of line 4 in ISO form and after weekly 2,
        // line 13 03/04/2025.
        const QString shared = "10:MP012:21 11:MP003:1 11:MP004:1 13:MP007:1";
        QTest::newRow("utf8") << "grammar_encodings_utf8.in" << int(Encoding::Utf8) << "crlf" << true << 16 << shared;
        QTest::newRow("utf16le") << "grammar_encodings_utf16le.in" << int(Encoding::Utf16LE) << "crlf" << true << 16 << shared;
        QTest::newRow("utf16be") << "grammar_encodings_utf16be.in" << int(Encoding::Utf16BE) << "lf" << false << 16 << shared;
        QTest::newRow("mixed") << "grammar_encodings_mixed.in" << int(Encoding::Utf8Bom) << "mixed" << true << 16 << "1:MP009:0 " + shared;
        QTest::newRow("cp1252") << "grammar_encodings_cp1252.in" << int(Encoding::Ansi1252) << "crlf" << true << 10 << "4:MP010:29 7:MP007:1";
    }
    void encodingFixtures() {
        QFETCH(QString, file); QFETCH(int, encoding); QFETCH(QString, eol); QFETCH(bool, finalNewline); QFETCH(int, lines);
        QFETCH(QString, diags);
        const QByteArray bytes = readAll(fx(file));
        QVERIFY2(!bytes.isEmpty(), qPrintable("missing fixture " + file));
        const Document d = parse(bytes);
        QCOMPARE(int(d.encoding), encoding);
        QCOMPARE(d.eolStyle(), eol);
        QCOMPARE(d.finalNewline(), finalNewline);
        QCOMPARE(d.entries.size(), lines);
        QCOMPARE(d.events.size(), file.contains("cp1252") ? 4 : 6);
        QCOMPARE(diagList(d), diags);
        for (const Diagnostic& g : d.diagnostics) QCOMPARE(g.severity, diagnosticInfo(g.code).severity);
        QCOMPARE(serialize(d), bytes);
        // Canonical: one form (UTF-8 BOM, CRLF on every line, ISO stamps); parsing it gives the same meaning, it is a
        // fixed point, and only the representation diagnostics (MP007 / MP009 / MP010) go away.
        const QByteArray canon = serialize(d, WriteOptions::canonicalForm());
        QVERIFY(canon.startsWith("\xEF\xBB\xBF"));
        QCOMPARE(int(canon.count("\r\n")), lines);
        QCOMPARE(int(canon.count('\n')), lines);
        const Document c = parse(canon);
        QCOMPARE(c.encoding, Encoding::Utf8Bom);
        QCOMPARE(c.eolStyle(), QStringLiteral("crlf"));
        QVERIFY(c.finalNewline());
        QCOMPARE(semanticDiff(d, c), QString());
        for (const Entry& e : c.entries)
            if (e.kind == LineKind::Header && e.stamp.isValid()) QVERIFY2(!e.stamp.legacy, qPrintable(e.raw));
        QCOMPARE(serialize(c, WriteOptions::canonicalForm()), canon);
        QStringList kept;
        for (const QString& g : diags.split(' '))
            if (!g.contains("MP007") && !g.contains("MP009") && !g.contains("MP010")) kept << g;
        QCOMPARE(diagList(c), kept.join(' '));
    }

    void encodingsAgree() {
        // The same playlist in UTF-8, UTF-16 LE, UTF-16 BE and UTF-8 BOM with mixed EOL: the same text line for line,
        // the same meaning, and one canonical form (with ISO stamps and with the stamps kept).
        const Document ref = parse(readAll(fx(kSharedEncodings.first())));
        const QByteArray canon = serialize(ref, WriteOptions::canonicalForm());
        WriteOptions keep = WriteOptions::canonicalForm();
        keep.stamps = StampForm::Keep;
        const QByteArray canonKeep = serialize(ref, keep);
        for (const QString& f : kSharedEncodings) {
            const Document d = parse(readAll(fx(f)));
            QCOMPARE(d.entries.size(), ref.entries.size());
            for (int i = 0; i < d.entries.size(); ++i) QCOMPARE(d.entries.at(i).raw, ref.entries.at(i).raw);
            QCOMPARE(semanticDiff(ref, d), QString());
            QCOMPARE(serialize(d, WriteOptions::canonicalForm()), canon);
            QCOMPARE(serialize(d, keep), canonKeep);
        }
        // The decoded text: 2, 3 and 4-byte UTF-8 (a surrogate pair in UTF-16), and line 6's split traps (U+0D15 U+010A
        // is "15 0D 0A 01" in LE, U+0D0A is "0D 0A" in BE: a byte-wise splitter would see a line break in both).
        QCOMPARE(entryAtLine(ref, 3).path, QString("S:\\Fill\\Caf") + QChar(0xE9) + " Loop " + QChar(0x2013) + " " + QChar(0xBD) + ".mpg");
        QCOMPARE(entryAtLine(ref, 5).path, QString("S:\\COMMERCIALS 2025\\Joe") + QChar(0x2019) + "s Diner " + QString::fromUcs4(U"\U0001F3AC") + ".mpg");
        QCOMPARE(entryAtLine(ref, 6).path, QString("S:\\Specials\\") + QChar(0x0D15) + QChar(0x010A) + " " + QChar(0x0D0A) + " trap.mpg");
        QCOMPARE(entryAtLine(ref, 7).arg, QString("Wetter: 72") + QChar(0xB0) + " " + QChar(0xFC) + "ber M" + QChar(0xFC) + "nchen");
        QCOMPARE(entryAtLine(ref, 8).name, QString("Ma") + QChar(0xF1) + "ana Show");
        QCOMPARE(entryAtLine(ref, 9).forcedDurationSec, 10.0);
        QCOMPARE(entryAtLine(ref, 12).value("f"), QStringLiteral("D:\\Bugs\\BUG.png"));
        QCOMPARE(entryAtLine(ref, 14).path, QString("S:\\Specials\\") + QChar(0xFFFC) + " object.mpg");
        QCOMPARE(entryAtLine(ref, 15).kind, LineKind::Blank);
        // The canonical text: legacy stamps became ISO, the blank line lost its spaces, every line ends in CRLF.
        const QList<QByteArray> cl = canon.mid(3).split('\n');
        QCOMPARE(cl.size(), 17);
        QCOMPARE(cl.at(3), QByteArray("0000-00-01T06:00:00\tEventDuration=00:30:00\tPlayMode=1\r"));
        QCOMPARE(cl.at(7), QByteArray("0000-00-02T06:00:00\tEventType=1\tName=Ma\xC3\xB1" "ana Show\tEventDuration=01:00:00\tPlayMode=1\r"));
        QCOMPARE(cl.at(11), QByteArray("!ShowLogo f=D:\\Bugs\\BUG.png\tcfg=D:\\Bugs\\bug.osd\r"));   // the head keeps its form
        QCOMPARE(cl.at(12), QByteArray("2025-04-03T10:00:00\tEventDuration=00:05:00\tPlayMode=1\r"));
        QCOMPARE(cl.at(14), QByteArray("\r"));
        QCOMPARE(cl.at(15), QByteArray("9999-99-99T00:00:00\r"));
        QCOMPARE(cl.at(16), QByteArray());
    }

    void cp1252Fixture() {
        const QByteArray bytes = readAll(fx("grammar_encodings_cp1252.in"));
        const Document d = parse(bytes);
        QCOMPARE(d.encoding, Encoding::Ansi1252);
        QVERIFY(!d.hasBom());
        QCOMPARE(entryAtLine(d, 4).name, QString("Caf") + QChar(0xE9) + " " + QChar(0x2013) + " Fr" + QChar(0xFC) + "h");
        QCOMPARE(entryAtLine(d, 5).path, QString("S:\\COMMERCIALS 2025\\Joe") + QChar(0x2019) + "s Diner " + QChar(0x20AC) + " 5.mpg");
        QCOMPARE(entryAtLine(d, 6).arg, QString("Wetter: 72") + QChar(0xB0) + " " + QChar(0xFC) + "ber M" + QChar(0xFC) + "nchen " + QChar(0x2026));
        QCOMPARE(entryAtLine(d, 8).path, QString("S:\\Specials\\Ma") + QChar(0xF1) + "ana " + QChar(0x201C) + "quoted" + QChar(0x201D) + ".mpg");
        // The five bytes cp1252 leaves undefined decode to the C1 control of the same value and come back as themselves.
        QCOMPARE(entryAtLine(d, 9).path, QString("S:\\Specials\\undefined ") + QChar(0x81) + QChar(0x8D) + QChar(0x8F) + QChar(0x90) + QChar(0x9D) + " bytes.mpg");
        QVERIFY(entryAtLine(d, 7).stamp.ambiguous);
        QCOMPARE(entryAtLine(d, 7).stamp.date, QDate(2025, 4, 3));
        QCOMPARE(entryAtLine(d, 10).stamp.kind, DayKind::EndOfList);
        QVERIFY(d.diagnostics.first().message.contains("0xE9"));
        // To UTF-8 with the stamps kept, then back to cp1252: the original bytes.
        WriteOptions keep = WriteOptions::canonicalForm();
        keep.stamps = StampForm::Keep;
        const Document u = parse(serialize(d, keep));
        QCOMPARE(u.encoding, Encoding::Utf8Bom);
        QVERIFY(!findDiag(u, "MP010"));
        QCOMPARE(semanticDiff(d, u), QString());
        WriteOptions ansi;
        ansi.encoding = Encoding::Ansi1252;
        QList<int> lossy;
        QCOMPARE(serialize(u, ansi, &lossy), bytes);
        QVERIFY(lossy.isEmpty());
        // legacyDateOrder picks the reading and so silences MP007.
        ParseOptions mdy;
        mdy.legacyDateOrder = "mdy";
        const Document m = parse(bytes, mdy);
        QCOMPARE(entryAtLine(m, 7).stamp.date, QDate(2025, 3, 4));
        QVERIFY(!findDiag(m, "MP007"));
    }

    void utf8Detection_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("encoding");
        QTest::addColumn<int>("line");                 // the MP010 line, 0 = none
        QTest::addColumn<int>("col");
        const int u8 = int(Encoding::Utf8), ansi = int(Encoding::Ansi1252);
        QTest::newRow("ascii") << QByteArray("S:\\a.mpg\r\n") << u8 << 0 << 0;
        QTest::newRow("2-byte") << QByteArray("S:\\Caf\xC3\xA9.mpg\r\n") << u8 << 0 << 0;
        QTest::newRow("3-byte") << QByteArray("S:\\Joe\xE2\x80\x99s.mpg\r\n") << u8 << 0 << 0;
        QTest::newRow("4-byte") << QByteArray("S:\\\xF0\x9F\x8E\xAC.mpg\r\n") << u8 << 0 << 0;
        QTest::newRow("cp1252 on line 3") << QByteArray("0000-00-00T00:00:00\r\nS:\\a.mpg\r\nS:\\Joe\x92s.mpg\r\n") << ansi << 3 << 7;
        QTest::newRow("overlong") << QByteArray("S:\\\xC0\xAF.mpg") << ansi << 1 << 4;
        QTest::newRow("encoded surrogate") << QByteArray("S:\\\xED\xA0\x80.mpg") << ansi << 1 << 4;
        QTest::newRow("above U+10FFFF") << QByteArray("S:\\\xF4\x90\x80\x80.mpg") << ansi << 1 << 4;
        QTest::newRow("F5 lead byte") << QByteArray("S:\\\xF5\x80\x80\x80") << ansi << 1 << 4;
        QTest::newRow("lone continuation") << QByteArray("S:\\\x80.mpg") << ansi << 1 << 4;
        QTest::newRow("truncated at the end") << QByteArray("S:\\a.mpg\r\nS:\\\xE2\x80") << ansi << 2 << 4;
        QTest::newRow("bad after the BOM") << QByteArray("\xEF\xBB\xBFS:\\Caf\xC3\xA9\xFF.mpg\r\n") << int(Encoding::Utf8Bom) << 1 << 8;
    }
    void utf8Detection() {
        QFETCH(QByteArray, bytes); QFETCH(int, encoding); QFETCH(int, line); QFETCH(int, col);
        const Document d = parse(bytes);
        QCOMPARE(int(d.encoding), encoding);
        const Diagnostic* g = findDiag(d, "MP010");
        QCOMPARE(g != nullptr, line > 0);
        if (g) { QCOMPARE(g->line, line); QCOMPARE(g->col, col); QCOMPARE(g->severity, QStringLiteral("warning")); }
        QCOMPARE(serialize(d), bytes);
    }

    void utf16Edges() {
        // BOM only.
        const Document e = parse(QByteArray("\xFE\xFF", 2));
        QCOMPARE(e.encoding, Encoding::Utf16BE);
        QCOMPARE(e.entries.size(), 0);
        QCOMPARE(serialize(e), QByteArray("\xFE\xFF", 2));
        // A second U+FEFF after the BOM is text: a stray BOM line (MP001), as in UTF-8.
        const QByteArray dbl = utf16(QString(QChar(0xFEFF)) + "0000-00-00T00:00:00\r\n", true);
        Document d = parse(dbl);
        QCOMPARE(d.encoding, Encoding::Utf16LE);
        QCOMPARE(d.entries.first().kind, LineKind::Unknown);
        QCOMPARE(diagList(d), QStringLiteral("1:MP001:1"));
        QCOMPARE(serialize(d), dbl);
        // An odd byte count: the half code unit stays with the last line, is written back, and is reported.
        const QByteArray odd = utf16("0000-00-00T00:00:00\r\nS:\\a.mpg", true) + QByteArray("\x41", 1);
        d = parse(odd);
        QCOMPARE(d.entries.size(), 2);
        QCOMPARE(diagList(d), QStringLiteral("2:MP010:9"));
        QCOMPARE(serialize(d), odd);
        const QByteArray odd2 = utf16("0000-00-00T00:00:00\n", false) + QByteArray("\x41", 1);   // after the last LF
        d = parse(odd2);
        QCOMPARE(d.entries.size(), 2);
        QCOMPARE(diagList(d), QStringLiteral("2:MP010:1"));
        QCOMPARE(serialize(d), odd2);
        // An unpaired surrogate: reported at its column, bytes kept. A valid pair (also at the end of a line) is not.
        const QByteArray sur = utf16(QString("0000-00-00T00:00:00\r\nS:\\x") + QChar(0xD800) + "y.mpg\r\n", true);
        d = parse(sur);
        QCOMPARE(diagList(d), QStringLiteral("2:MP010:5"));
        QCOMPARE(serialize(d), sur);
        const QByteArray low = utf16(QString("0000-00-00T00:00:00\nS:\\x") + QChar(0xDC00) + "\n", false);
        QCOMPARE(diagList(parse(low)), QStringLiteral("2:MP010:5"));
        const QByteArray pair = utf16(QString("0000-00-00T00:00:00\nS:\\") + QString::fromUcs4(U"\U0001F3AC") + "\n", false);
        d = parse(pair);
        QVERIFY(d.diagnostics.isEmpty());
        QCOMPARE(serialize(d), pair);
        // A lone CR inside a UTF-16 line: kept as text, MP009 at its column.
        const QByteArray cr = utf16("0000-00-00T00:00:00\nS:\\a\rb.mpg\n", false);
        d = parse(cr);
        QCOMPARE(d.entries.size(), 2);
        QCOMPARE(diagList(d), QStringLiteral("2:MP009:5"));
        QCOMPARE(serialize(d), cr);
        // Edits re-encode in the document's own UTF-16 flavour; the untouched header keeps its bytes.
        d = parse(utf16("0000-00-00T00:00:00\tPlayMode=1\nS:\\a.mpg\n", false));
        d.entries[1].path = QString("S:\\") + QChar(0x2019) + QString::fromUcs4(U"\U0001F3AC") + ".mpg";
        d.entries[1].dirty = true;
        QCOMPARE(serialize(d), utf16(QString("0000-00-00T00:00:00\tPlayMode=1\nS:\\") + QChar(0x2019) + QString::fromUcs4(U"\U0001F3AC") + ".mpg\n", false));
    }

    void mixedEol() {
        // The ANFS plant pattern, LF headers and CRLF items: one MP009, at the first line using the minority terminator.
        const QByteArray m = "0000-00-00T00:00:00\tPlayMode=1\nS:\\a.mpg\r\nS:\\b.mpg\r\n01/00/0000 06:00:00\tPlayMode=1\nS:\\c.mpg\r\n";
        Document d = parse(m);
        QCOMPARE(d.eolStyle(), QStringLiteral("mixed"));
        QCOMPARE(d.defaultEol(), QByteArray("\r\n"));
        QCOMPARE(diagList(d), QStringLiteral("1:MP009:0"));
        QVERIFY(d.diagnostics.first().message.contains("3 CRLF, 2 LF"));
        QCOMPARE(serialize(d), m);
        // Majority LF: the first CRLF line is the one reported.
        d = parse("0000-00-00T00:00:00\nS:\\b.mpg\r\nS:\\c.mpg\n");
        QCOMPARE(diagList(d), QStringLiteral("2:MP009:0"));
        QVERIFY(d.diagnostics.first().message.contains("1 CRLF, 2 LF"));
        // One terminator only, or none: nothing to report.
        for (const char* s : {"0000-00-00T00:00:00\r\nS:\\a.mpg\r\n", "0000-00-00T00:00:00\nS:\\a.mpg", "0000-00-00T00:00:00"})
            QVERIFY2(parse(s).diagnostics.isEmpty(), s);
        // A lone CR: kept in the text, MP009 at its column. "\r\r\n": the second CR is the terminator's.
        const QByteArray lone = "0000-00-00T00:00:00\tPlayMode=1\r\nS:\\a\rb.mpg\r\n";
        d = parse(lone);
        QCOMPARE(d.entries.size(), 2);
        QCOMPARE(diagList(d), QStringLiteral("2:MP009:5"));
        QCOMPARE(serialize(d), lone);
        d = parse("0000-00-00T00:00:00\tPlayMode=1\r\r\n");
        QCOMPARE(d.entries.first().raw, QStringLiteral("0000-00-00T00:00:00\tPlayMode=1\r"));
        QCOMPARE(d.entries.first().eol, QByteArray("\r\n"));
        QCOMPARE(diagList(d), QStringLiteral("1:MP009:31"));
        // Forcing one terminator: every line takes it, the content bytes are untouched.
        WriteOptions crlf;
        crlf.eol = "\r\n";
        QCOMPARE(serialize(parse(m), crlf), QByteArray(m).replace("\r\n", "\n").replace("\n", "\r\n"));
        QVERIFY(parse(serialize(parse(m), crlf)).diagnostics.isEmpty());
        WriteOptions lf;
        lf.eol = "\n";
        QCOMPARE(serialize(parse("0000-00-00T00:00:00\r\nS:\\a.mpg"), lf), QByteArray("0000-00-00T00:00:00\nS:\\a.mpg"));   // no final newline stays none
        WriteOptions bogus;
        bogus.eol = "\r";                                    // only CRLF or LF can be forced
        QCOMPARE(serialize(parse(m), bogus), m);
        // Final newline: added with the majority terminator (or the forced one), or dropped.
        WriteOptions nl;
        nl.finalNewline = true;
        QCOMPARE(serialize(parse("a.mpg\nb.mpg"), nl), QByteArray("a.mpg\nb.mpg\n"));
        QCOMPARE(serialize(parse("a.mpg\r\nb.mpg"), nl), QByteArray("a.mpg\r\nb.mpg\r\n"));
        QCOMPARE(serialize(parse("a.mpg\r\nb.mpg\r\n"), nl), QByteArray("a.mpg\r\nb.mpg\r\n"));
        nl.eol = "\n";
        QCOMPARE(serialize(parse("a.mpg\r\nb.mpg"), nl), QByteArray("a.mpg\nb.mpg\n"));
        WriteOptions none;
        none.finalNewline = false;
        QCOMPARE(serialize(parse("a.mpg\r\nb.mpg\r\n"), none), QByteArray("a.mpg\r\nb.mpg"));
        QCOMPARE(serialize(parse("a.mpg\r\nb.mpg"), none), QByteArray("a.mpg\r\nb.mpg"));
    }

    void eventOrder() {
        const QByteArray in =
            "0000-00-00T05:00:00\tPlayMode=1\r\n"            // 1
            "01/00/0000 06:00:00\tPlayMode=1\r\n"            // 2
            "0000-00-02T06:00:00\tPlayMode=1\r\n"            // 3
            "0000-00-01T06:00:00\tPlayMode=1\r\n"            // 4  line 2's time in ISO form (MP003), after weekly 2 (MP004)
            "0000-00-01T07:00:00\tPlayMode=1\r\n"            // 5  in order after line 4
            "2025-13-01T00:00:00\tPlayMode=1\r\n"            // 6  MP002, left out of the order checks
            "0000-00-80T04:00:00\tPlayMode=1\r\n"            // 7  day change after weekly: in order
            "0000-00-00T23:00:00\tPlayMode=1\r\n"            // 8  every day after the day change: MP004
            "2025-12-25T10:00:00\tPlayMode=1\r\n"            // 9
            "25/12/2025 10:00:00\tPlayMode=1\r\n"            // 10 line 9 in legacy form: MP003, not MP004
            "9999-99-99T00:00:00\r\n"                        // 11
            "0000-00-07T00:00:00\tPlayMode=1\r\n";           // 12 after the end of list: MP004
        const Document d = parse(in);
        QCOMPARE(diagList(d), QStringLiteral("4:MP003:1 4:MP004:1 6:MP002:1 8:MP004:1 10:MP003:1 12:MP004:1"));
        QVERIFY(d.diagnostics.at(0).message.contains("line 2"));
        QCOMPARE(d.diagnostics.at(0).severity, QStringLiteral("warning"));
        QVERIFY(d.diagnostics.at(1).message.contains("line 3"));
        QCOMPARE(d.diagnostics.at(1).severity, QStringLiteral("info"));
        QVERIFY(d.diagnostics.at(3).message.contains("line 7"));   // the bad stamp on line 6 is skipped
        QVERIFY(d.diagnostics.at(4).message.contains("line 9"));
        // The samples and the plant-shaped grammar fixture are in order.
        for (const QString& f : {QString("sample.in"), QString("OSD-Instructions.in"), QString("Saturday_0200.in")})
            QVERIFY(!findDiag(parse(readAll(fx(f))), "MP004") && !findDiag(parse(readAll(fx(f))), "MP003"));
    }

    void ambiguousDateDiagnostic() {
        const QByteArray in = "03/04/2025 10:00:00\tPlayMode=1\r\n"      // DMY and MDY both valid: MP007
                              "04/04/2025 10:00:00\tPlayMode=1\r\n"      // the same either way
                              "25/12/2025 10:00:00\tPlayMode=1\r\n"      // only DMY
                              "12/25/2025 11:00:00\tPlayMode=1\r\n";     // only MDY
        const Document a = parse(in);
        QCOMPARE(diagList(a), QStringLiteral("1:MP007:1"));
        QVERIFY(a.diagnostics.first().message.contains("2025-04-03"));
        ParseOptions dmy;
        dmy.legacyDateOrder = "dmy";
        QVERIFY(!findDiag(parse(in), "MP002") && !findDiag(parse(in, dmy), "MP007"));
        QCOMPARE(diagList(parse(in, dmy)), QStringLiteral("4:MP002:1"));   // forced DMY: 12/25 has no month 25
        // Canonical ISO writing settles the reading: the re-parsed file has nothing ambiguous left.
        const Document c = parse(serialize(a, WriteOptions::canonicalForm()));
        QVERIFY(c.diagnostics.isEmpty());
        QCOMPARE(entryAtLine(c, 1).stamp.date, QDate(2025, 4, 3));
        QCOMPARE(entryAtLine(c, 1).raw, QStringLiteral("2025-04-03T10:00:00\tPlayMode=1"));
        QCOMPARE(entryAtLine(c, 4).raw, QStringLiteral("2025-12-25T11:00:00\tPlayMode=1"));
    }

    void bracketTokens_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<QString>("code");
        QTest::addColumn<int>("col");
        QTest::newRow("date window") << "S:\\x.mpg[2025-01-01..2025-02-01]" << "MP012" << 9;
        QTest::newRow("seconds without frames") << "x.mpg[00:00:10]" << "MP012" << 6;
        QTest::newRow("empty") << "x.mpg[]" << "MP012" << 6;
        QTest::newRow("token then field") << "x.mpg[foo]\tName=y" << "MP012" << 6;
        QTest::newRow("last of two") << "x[a].mpg[b]" << "MP012" << 9;
        QTest::newRow("inside the basename") << "D:\\Wetter\\IntroWetter[5].TIF" << "" << 0;
        QTest::newRow("forced duration") << "x.rtf[00:00:10;00]" << "" << 0;
        QTest::newRow("bad duration value") << "x.rtf[00:61:00;00]" << "MP013" << 6;
        QTest::newRow("nothing before it") << "[foo]" << "" << 0;
        QTest::newRow("unclosed") << "x.mpg[foo" << "" << 0;
    }
    void bracketTokens() {
        QFETCH(QString, line); QFETCH(QString, code); QFETCH(int, col);
        QList<Diagnostic> dl;
        const Entry e = parseLine(line, {}, &dl, 5);
        QCOMPARE(e.kind, LineKind::Media);
        QCOMPARE(dl.size(), code.isEmpty() ? 0 : 1);
        if (!code.isEmpty()) { QCOMPARE(dl.first().code, code); QCOMPARE(dl.first().col, col); QCOMPARE(dl.first().line, 5); }
        if (code == u"MP012") { QVERIFY(e.bracketRaw.isEmpty()); QCOMPARE(e.forcedDurationSec, -1.0); QVERIFY(e.path.endsWith(u']')); }
        QCOMPARE(renderEntry(e), line);
    }

    void writeOptions() {
        const QByteArray bytes = readAll(fx("grammar_basic.in"));
        const Document p = parse(bytes);
        const WriteOptions c = WriteOptions::canonicalForm();
        QVERIFY(c.canonical);
        QCOMPARE(c.stamps, StampForm::Iso);
        QCOMPARE(*c.encoding, Encoding::Utf8Bom);
        QCOMPARE(c.eol, QByteArray("\r\n"));
        QVERIFY(*c.finalNewline);
        // The stamp form applies to rendered headers only: untouched ones keep their bytes, a dirty one takes the form.
        WriteOptions legacy;
        legacy.stamps = StampForm::Legacy;
        QCOMPARE(serialize(p, legacy), bytes);
        Document d = p;
        setParam(d.entries[35].params, "EventDuration", "00:20:00");   // line 36, ISO weekly 1
        d.entries[35].dirty = true;
        QList<QByteArray> out = serialize(d, legacy).split('\n');
        QCOMPARE(out.at(35), QByteArray("01/00/0000 05:00:00\tEventDuration=00:20:00\tPlayMode=1\r"));
        QCOMPARE(out.at(2), bytes.split('\n').at(2));                  // untouched ISO header
        WriteOptions iso;
        iso.stamps = StampForm::Iso;
        d.entries[6].dirty = true;                                     // line 7, legacy weekly 1
        QCOMPARE(serialize(d, iso).split('\n').at(6), QByteArray("0000-00-01T06:00:00\tEventDuration=00:30:00\tPlayMode=1\r"));
        // Canonical with legacy stamps: every valid header converted, a bad stamp and unknown lines kept as written.
        WriteOptions cl = WriteOptions::canonicalForm();
        cl.stamps = StampForm::Legacy;
        const QByteArray legacyCanon = serialize(p, cl);
        out = legacyCanon.mid(3).split('\n');
        QCOMPARE(out.at(2), QByteArray("00/00/0000 00:00:00\tEventDuration=24:00:00\tPlayMode=2\r"));
        QCOMPARE(out.at(35), QByteArray("01/00/0000 05:00:00\tEventDuration=00:10:00\tPlayMode=1\r"));
        QCOMPARE(out.at(37), QByteArray("80/00/0000 04:00:00\tPlayMode=1\r"));
        QCOMPARE(out.at(38), QByteArray("10/09/2025 09:07:00\tEventDuration=00:05:00\tPlayMode=1\r"));
        QCOMPARE(out.at(43), QByteArray("0000-00-00T13:00:00 PlayMode=1\r"));
        QCOMPARE(out.at(46), QByteArray("2025-13-01T00:00:00\tPlayMode=1\r"));
        QCOMPARE(out.at(49), QByteArray("99/99/9999 00:00:00\r"));
        QCOMPARE(semanticDiff(p, parse(legacyCanon)), QString());   // 10/09/2025 reads day first again (MP007 says so)
        QVERIFY(findDiag(parse(legacyCanon), "MP007"));
        // The end-of-list sentinel's free time survives a form change.
        Stamp s;
        QVERIFY(parseStamp("99/99/9999 99:99:99", {}, &s));
        QCOMPARE(formatStamp(s, true), QStringLiteral("9999-99-99T99:99:99"));
        // Encoding override: UTF-8 BOM -> UTF-16 BE -> UTF-8 BOM is the same bytes (untouched lines keep their text);
        // UTF-8 with and without a BOM differ by the BOM only.
        WriteOptions u16;
        u16.encoding = Encoding::Utf16BE;
        const QByteArray wide = serialize(p, u16);
        QVERIFY(wide.startsWith("\xFE\xFF"));
        const Document w = parse(wide);
        QCOMPARE(w.encoding, Encoding::Utf16BE);
        QCOMPARE(semanticDiff(p, w), QString());
        WriteOptions back;
        back.encoding = Encoding::Utf8Bom;
        QCOMPARE(serialize(w, back), bytes);
        WriteOptions nobom;
        nobom.encoding = Encoding::Utf8;
        QCOMPARE(serialize(p, nobom), bytes.mid(3));
        // To cp1252: U+2019 and U+00B0 exist there; U+1F3AC, Malayalam and U+FFFC do not ('?', and the lines are named).
        WriteOptions ansi;
        ansi.encoding = Encoding::Ansi1252;
        QList<int> lossy;
        const QByteArray a = serialize(p, ansi, &lossy);
        QVERIFY(lossy.isEmpty());
        QVERIFY(!a.startsWith("\xEF\xBB\xBF"));
        QVERIFY(a.contains("Joe\x92s Diner"));
        QVERIFY(a.contains("Weather 72\xB0.rtf"));
        serialize(parse(readAll(fx("grammar_encodings_utf8.in"))), ansi, &lossy);
        QCOMPARE(lossy, (QList<int>{5, 6, 14}));
        // Name helpers (REST / CLI options).
        QVERIFY(encodingFromName("ANSI") == Encoding::Ansi1252);
        QVERIFY(encodingFromName("utf-16le") == Encoding::Utf16LE);
        for (Encoding e : {Encoding::Utf8Bom, Encoding::Utf8, Encoding::Ansi1252, Encoding::Utf16LE, Encoding::Utf16BE})
            QVERIFY2(encodingFromName(encodingName(e)) == e, qPrintable(encodingName(e)));
        QVERIFY(!encodingFromName("latin9").has_value());
        for (StampForm f : {StampForm::Keep, StampForm::Iso, StampForm::Legacy})
            QVERIFY2(stampFormFromName(stampFormName(f).toUpper()) == f, qPrintable(stampFormName(f)));
        QVERIFY(!stampFormFromName("us").has_value());
    }

    void semanticDiffs() {
        const QByteArray a1 = "0000-00-01T06:00:00\tPlayMode=1\r\n!ShowLogo f=x.png\tcfg=y.osd\r\n!Video \r\nS:\\a.rtf[00:00:10;15]\r\n\r\n";
        const Document a = parse(a1);
        // The same meaning in other clothes: legacy stamp, TAB-form head, no trailing space, '.' frames, LF, no blank
        // line, UTF-16 LE.
        const Document b = parse(utf16("01/00/0000 06:00:00\tPlayMode=1\n!ShowLogo\tf=x.png\tcfg=y.osd\n!Video\nS:\\a.rtf[00:00:10.15]", true));
        QCOMPARE(semanticDiff(a, b), QString());
        QCOMPARE(semanticDiff(b, a), QString());
        // A changed path, time or field, or an extra line, is a difference named with both line numbers.
        Document c = a;
        c.entries[3].path = "S:\\b.rtf";
        QString diff = semanticDiff(a, c);
        QVERIFY2(diff.contains("line 4") && diff.contains("S:\\b.rtf"), qPrintable(diff));
        c = a;
        c.entries[0].stamp.time = QTime(7, 0);
        QVERIFY(semanticDiff(a, c).contains("0000-00-01T07:00:00"));
        c = a;
        setParam(c.entries[1].params, "cfg", "z.osd");
        QVERIFY(semanticDiff(a, c).contains("cfg=z.osd"));
        diff = semanticDiff(a, parse(a1 + "S:\\c.mpg\r\n"));
        QVERIFY2(diff.contains("second document has 1 more line(s), from line 6"), qPrintable(diff));
        QVERIFY(semanticDiff(parse(a1 + "S:\\c.mpg\r\n"), a).contains("first document"));
        // '%' in a path is shown as written (one arg() pass), an argument is not a bare first field, unknown lines count.
        QVERIFY(semanticDiff(parse("S:\\%1%\\a.mpg"), parse("S:\\%1%\\b.mpg")).contains("S:\\%1%\\a.mpg"));
        QVERIFY(!semanticDiff(parse("!InsertDirectory D:\\x"), parse("!InsertDirectory\tD:\\x")).isEmpty());
        QVERIFY(!semanticDiff(parse("# a"), parse("# b")).isEmpty());
        QCOMPARE(semanticDiff(parse(""), parse(" \r\n\t\r\n")), QString());
    }

    void diagnosticCatalogTable() {
        const QList<DiagnosticInfo>& cat = diagnosticCatalog();
        QCOMPARE(cat.size(), 31);                             // design §15: MP001..MP028 and MP030; MP031 / MP032 (exporter, MP-10)
        const QStringList severities{"error", "warning", "info"}, stages{"parser", "catalog", "resolver", "timeline", "importer", "exporter"};
        static const QRegularExpression kCode("^MP0\\d\\d$");
        QString prev;
        for (const DiagnosticInfo& i : cat) {
            QVERIFY(kCode.match(i.code).hasMatch());
            QVERIFY2(i.code > prev, qPrintable(i.code));
            prev = i.code;
            QVERIFY(severities.contains(i.severity));
            QVERIFY(stages.contains(i.stage));
            QVERIFY(!i.title.isEmpty());
        }
        QCOMPARE(diagnosticInfo("mp002").severity, QStringLiteral("error"));
        QCOMPARE(diagnosticInfo("MP027").stage, QStringLiteral("importer"));
        QVERIFY(diagnosticInfo("MP029").code.isEmpty());
        // Every diagnostic the fixtures produce has the catalog severity, and together they cover every parser code.
        QSet<QString> seen;
        for (const QString& f : kInFixtures)
            for (const Diagnostic& g : parse(readAll(fx(f))).diagnostics) {
                QVERIFY2(g.severity == diagnosticInfo(g.code).severity, qPrintable(f + ": " + g.code));
                seen << g.code;
            }
        for (const DiagnosticInfo& i : cat)
            if (i.stage == u"parser") QVERIFY2(seen.contains(i.code), qPrintable(i.code + " is not exercised by any fixture"));
    }

    // ---- MP-03: instruction catalog --------------------------------------------------------------------------------

    void catalogTable() {
        const QList<InstrInfo>& cat = instructionCatalog();
        QSet<QString> names;
        QSet<int> ids;
        for (const InstrInfo& i : cat) {
            QVERIFY2(!i.name.isEmpty() && !i.summary.isEmpty() && !i.evidence.isEmpty(), qPrintable(i.name));
            QVERIFY(!groupName(i.group).isEmpty() && !supportName(i.support).isEmpty() && !headKindName(i.head).isEmpty());
            QVERIFY2(!i.checkKeys || !i.keys.isEmpty(), qPrintable(i.name));
            for (const QString& n : QStringList{i.name} + i.aliases) {
                QVERIFY2(!names.contains(n.toLower()), qPrintable("duplicate name " + n));
                names << n.toLower();
                QCOMPARE(instructionInfo(n), &i);                       // by name or alias, any case
                QCOMPARE(instructionInfo(n.toUpper()), &i);
                const Instruction ins = identify(parseLine("!" + n));   // a line written with it names this row
                QVERIFY2(ins.info == &i, qPrintable(n));
                QCOMPARE(ins.id, i.id);
                QCOMPARE(ins.name, i.name);
                QCOMPARE(ins.written, n);
                QCOMPARE(ins.alias, n != i.name);
                QVERIFY(ins.head.isEmpty());
            }
            ids << int(i.id);
            if (i.id != Instr::Legacy) QCOMPARE(instructionInfo(i.id), &i);   // one row per id, Legacy aside
        }
        for (int id = int(Instr::Normal); id <= int(Instr::Legacy); ++id) QVERIFY2(ids.contains(id), qPrintable(QString::number(id)));
        QVERIFY(!instructionInfo(Instr::Unknown));
        QVERIFY(!instructionInfo(QStringLiteral("OSD")) && !instructionInfo(QStringLiteral("FooBar")));
        QCOMPARE(instructionInfo(QStringLiteral(" osd   OFF "))->id, Instr::OsdOff);
        // The play-mode markers of design §2.3.
        QMap<QString, int> markers;
        for (const InstrInfo& i : cat) if (i.isMarker()) markers[i.name] = i.playMode;
        QCOMPARE(markers, (QMap<QString, int>{{"Normal", 1}, {"Repeat", 2}, {"Fillup", 3}, {"WaitForGPI", 4}, {"Break", 4},
                                             {"Package", 0}, {"InsertPlaylist", 0}}));
        // Keys: case-insensitive, "X*" prefixes, and the common timing keys on every row.
        const InstrInfo* dir = instructionInfo(Instr::InsertDirectory);
        for (const char* k : {"loadingmode", "ReplaceWithDirectoryMaxAge", "EWStartDate", "Seg#", "AutoFillText", "showvalue", "NAME"})
            QVERIFY2(dir->knowsKey(k), k);
        QVERIFY(!dir->knowsKey("Frobnicate") && !dir->knowsKey("ReplaceWit") && !dir->knowsKey(""));
        QVERIFY(instructionInfo(Instr::Break)->knowsKey("#") && instructionInfo(Instr::Repeat)->knowsKey("Layer"));
        QCOMPARE(playModeName(4), QStringLiteral("waitForGpi"));
        QVERIFY(playModeName(5).isEmpty() && playModeName(-1).isEmpty());
        QCOMPARE(loadingModeName(LoadingMode::SelectRandom), QStringLiteral("selectRandom"));
        QSet<QString> lm;
        for (int m = 0; m <= 8; ++m) lm << loadingModeName(LoadingMode(m));
        QCOMPARE(lm.size(), 9);
        QVERIFY(!lm.contains(QString()));
        // The JSON a REST catalog route serves.
        const QJsonArray j = catalogJson();
        QCOMPARE(j.size(), cat.size());
        for (const QJsonValue& v : j) {
            const QJsonObject o = v.toObject();
            if (o["name"].toString() == u"Repeat") {
                QCOMPARE(o["aliases"].toArray(), QJsonArray{"Wiederholen"});
                QCOMPARE(o["playMode"].toInt(), 2);
                QCOMPARE(o["group"].toString(), QStringLiteral("playMode"));
                QCOMPARE(o["support"].toString(), QStringLiteral("structural"));
            }
            if (o["name"].toString() == u"ShowLogo") {
                QVERIFY(!o.contains("playMode"));
                QCOMPARE(o["head"].toString(), QStringLiteral("field"));
                QVERIFY(o["checkKeys"].toBool());
                QVERIFY(o["keys"].toArray().contains(QJsonValue("f")));
            }
        }
    }

    void identifyNames_data() {
        QTest::addColumn<QString>("line");
        QTest::addColumn<int>("id");
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("written");
        QTest::addColumn<bool>("alias");
        QTest::addColumn<QString>("head");
        QTest::addColumn<int>("headCol");
        QTest::addColumn<int>("headParam");
        auto row = [](const char* n, const QString& line, Instr id, const QString& name, const QString& written, bool alias, const QString& head,
                      int col, int param) { QTest::newRow(n) << line << int(id) << name << written << alias << head << col << param; };
        row("name", "!Repeat", Instr::Repeat, "Repeat", "Repeat", false, "", 0, -1);
        row("name any case", "!repeat", Instr::Repeat, "Repeat", "repeat", false, "", 0, -1);
        row("alias", "!Wiederholen", Instr::Repeat, "Repeat", "Wiederholen", true, "", 0, -1);
        row("alias any case", "!WIEDERHOLEN", Instr::Repeat, "Repeat", "WIEDERHOLEN", true, "", 0, -1);
        row("osd off", "!OSD off", Instr::OsdOff, "OSD off", "OSD off", false, "", 0, -1);
        row("osd off any case", "!osd OFF", Instr::OsdOff, "OSD off", "osd OFF", false, "", 0, -1);
        row("osd alone", "!OSD", Instr::Unknown, "OSD", "OSD", false, "", 0, -1);
        row("osd on", "!OSD on", Instr::Unknown, "OSD", "OSD", false, "on", 6, -1);
        row("osd tab off", "!OSD\toff", Instr::Unknown, "OSD", "OSD", false, "", 0, -1);   // two-word names: space form only
        row("set config", "!Set Config Crawl\tName=1", Instr::SetConfig, "Set Config", "Set Config", false, "Crawl", 13, -1);
        row("setconfig alias", "!SetConfig Logo", Instr::SetConfig, "Set Config", "SetConfig", true, "Logo", 12, -1);
        row("countdown next event", "!CountDown To NextEvent", Instr::CountDownTo, "CountDown To", "CountDown To", false, "NextEvent", 15, -1);
        row("countdown two spaces", "!CountDown To  18:00:00", Instr::CountDownTo, "CountDown To", "CountDown To", false, "18:00:00", 16, -1);
        row("countup", "!CountUp From 06:00:00", Instr::CountUpFrom, "CountUp From", "CountUp From", false, "06:00:00", 15, -1);
        row("show tv alias", "!Show TV ESPN", Instr::WinTV, "WinTV", "Show TV", true, "ESPN", 10, -1);
        row("write error", "!Write Error", Instr::Legacy, "Write Error", "Write Error", false, "", 0, -1);
        row("legacy", "!Camera 2", Instr::Legacy, "Camera", "Camera", false, "2", 9, -1);
        row("folder", "!InsertDirectory D:\\x\tLoadingMode=3", Instr::InsertDirectory, "InsertDirectory", "InsertDirectory", false, "D:\\x", 18, -1);
        row("folder tab form", "!InsertDirectory\tD:\\x\tLoadingMode=3", Instr::InsertDirectory, "InsertDirectory", "InsertDirectory", false, "D:\\x", 18, 0);
        row("trailing tab", "!InsertDirectory D:\\Masterplay\\Testfiles\\PAL\t", Instr::InsertDirectory, "InsertDirectory", "InsertDirectory", false,
            "D:\\Masterplay\\Testfiles\\PAL", 18, -1);
        row("text shaped like a field", "!ShowCrawl Temp=72 today\tcfg=c.osd", Instr::ShowCrawl, "ShowCrawl", "ShowCrawl", false, "Temp=72 today", 12, 0);
        row("known key after the name", "!ShowCrawl cfg=c.osd", Instr::ShowCrawl, "ShowCrawl", "ShowCrawl", false, "", 0, -1);
        row("logo field head", "!ShowLogo f=x.png", Instr::ShowLogo, "ShowLogo", "ShowLogo", false, "", 0, -1);
        row("logo path head", "!ShowLogo D:\\x.png", Instr::ShowLogo, "ShowLogo", "ShowLogo", false, "D:\\x.png", 11, -1);
        row("bare video", "!Video ", Instr::Video, "Video", "Video", false, "", 0, -1);
        row("marker has no head", "!Repeat\tx", Instr::Repeat, "Repeat", "Repeat", false, "", 0, -1);
        row("unknown", "!FooBar some argument\tx=1", Instr::Unknown, "FooBar", "FooBar", false, "some argument", 9, -1);
    }
    void identifyNames() {
        QFETCH(QString, line); QFETCH(int, id); QFETCH(QString, name); QFETCH(QString, written); QFETCH(bool, alias);
        QFETCH(QString, head); QFETCH(int, headCol); QFETCH(int, headParam);
        const Entry e = parseLine(line);
        QCOMPARE(e.kind, LineKind::Instruction);
        const Instruction ins = identify(e);
        QCOMPARE(int(ins.id), id);
        QCOMPARE(ins.known(), id != int(Instr::Unknown));
        QCOMPARE(ins.name, name);
        QCOMPARE(ins.written, written);
        QCOMPARE(ins.alias, alias);
        QCOMPARE(ins.head, head);
        QCOMPARE(ins.headCol, headCol);
        if (headCol) QCOMPARE(line.mid(headCol - 1, head.size()), head);   // the column really points at it
        QCOMPARE(ins.headParam, headParam);
        QCOMPARE(renderEntry(e), line);                       // identifying changes nothing
        QVERIFY(!identify(parseLine("S:\\a.mpg")).known() && identify(parseLine("S:\\a.mpg")).name.isEmpty());
    }

    void insertDirectorySpecs() {
        // The samples' older form: SelectRandom=N without LoadingMode= is select-random with N picks.
        const Document s = parse(readAll(fx("sample.in")));
        auto dir = insertDirectorySpec(s.entries.at(3));
        QVERIFY(dir);
        QCOMPARE(dir->directory, QStringLiteral("D:\\Media\\TestCards"));
        QCOMPARE(dir->folder, dir->directory);
        QVERIFY(dir->mask.isEmpty() && !dir->searchTerm && !dir->topin);
        QCOMPARE(dir->loadingMode, LoadingMode::SelectRandom);
        QVERIFY(!dir->loadingModeWritten && dir->legacySelectRandom);
        QCOMPARE(dir->toSelect, 2);
        QCOMPARE(dir->picks(), 2);
        QCOMPARE(dir->subDirs, 0);
        QCOMPARE(dir->maxAgeHours, -1.0);
        QVERIFY(dir->unmodelled.isEmpty() && dir->unknownKeys.isEmpty());
        // Testfiles.in: the trailing TAB is an empty bare field, not an option; no options = the whole folder by name.
        dir = insertDirectorySpec(parse(readAll(fx("Testfiles.in"))).entries.at(2));
        QCOMPARE(dir->directory, QStringLiteral("D:\\Media\\TestCards"));
        QCOMPARE(dir->loadingMode, LoadingMode::Normal);
        QCOMPARE(dir->picks(), 0);
        QVERIFY(dir->unknownKeys.isEmpty());
        // The plant's form (grammar_basic line 12): select-random 1, "MaxAge=" empty is no limit, ReplaceWith...=0 is off.
        const Document g = parse(readAll(fx("grammar_basic.in")));
        dir = insertDirectorySpec(entryAtLine(g, 12));
        QCOMPARE(dir->folder, QStringLiteral("C:\\Sync\\NEWS HUB\\TOP STORY"));
        QCOMPARE(dir->loadingMode, LoadingMode::SelectRandom);
        QVERIFY(dir->loadingModeWritten && !dir->legacySelectRandom);
        QCOMPARE(dir->toSelect, 1);
        QCOMPARE(dir->picks(), 1);
        QCOMPARE(dir->maxAgeHours, -1.0);
        QVERIFY(dir->unmodelled.isEmpty());
        // grammar_instructions line 4: a mask, select-normal, recursion, max age, still time, unmodelled options in effect.
        const Document d = parse(readAll(fx("grammar_instructions.in")));
        dir = insertDirectorySpec(entryAtLine(d, 4));
        QCOMPARE(dir->folder, QStringLiteral("C:\\Sync\\Promos"));
        QCOMPARE(dir->mask, QStringLiteral("Promo*.mpg"));
        QCOMPARE(dir->loadingMode, LoadingMode::SelectNormal);
        QCOMPARE(dir->picks(), 3);
        QCOMPARE(dir->subDirs, 1);
        QVERIFY(dir->recursive());
        QCOMPARE(dir->maxAgeHours, 72.0);
        QCOMPARE(dir->stillSec, 8.0);
        QCOMPARE(::keys(dir->unmodelled), (QStringList{"ReplaceWithDirectory", "EWStartDate"}));
        QCOMPARE(vals(dir->unmodelled), (QStringList{"D:\\Fallback", "2025-01-01"}));
        // Line 5: a %...% search term; the older count wins over Random=1.
        dir = insertDirectorySpec(entryAtLine(d, 5));
        QVERIFY(dir->searchTerm);
        QCOMPARE(dir->folder, dir->directory);
        QCOMPARE(dir->loadingMode, LoadingMode::SelectRandom);
        QCOMPARE(dir->picks(), 2);
        // Line 36: !InsertTopinDirectory reads the same way.
        dir = insertDirectorySpec(entryAtLine(d, 36));
        QVERIFY(dir->topin && dir->loadingModeWritten);
        QCOMPARE(dir->loadingMode, LoadingMode::Normal);
        QCOMPARE(dir->folder, QStringLiteral("D:\\Topin\\Weather"));
        // Random=1 alone (older files): all, shuffled. A mask at the drive root, and forward slashes.
        QCOMPARE(insertDirectorySpec(parseLine("!InsertDirectory D:\\x\tRandom=1"))->loadingMode, LoadingMode::Random);
        dir = insertDirectorySpec(parseLine("!InsertDirectory C:/Media/News_??.mpg\tLoadingMode=8"));
        QCOMPARE(dir->folder, QStringLiteral("C:/Media"));
        QCOMPARE(dir->mask, QStringLiteral("News_??.mpg"));
        QCOMPARE(dir->loadingMode, LoadingMode::SelectEventNo);
        QCOMPARE(dir->picks(), 0);
        // Line 65: values the editor could not have written are reported at their columns and left at the defaults.
        QList<Diagnostic> dl;
        dir = insertDirectorySpec(entryAtLine(d, 65), {}, &dl);
        QCOMPARE(dir->loadingMode, LoadingMode::Normal);
        QVERIFY(!dir->loadingModeWritten);
        QCOMPARE(dir->toSelect, 0);
        QCOMPARE(dir->subDirs, 0);
        QCOMPARE(dir->maxAgeHours, -1.0);
        QCOMPARE(dir->unknownKeys, QStringList{"Frobnicate"});
        QStringList got;
        for (const Diagnostic& x : dl) got << QString("%1:%2:%3").arg(x.line).arg(x.code).arg(x.col);
        got.sort();
        QCOMPARE(got, (QStringList{"65:MP011:27", "65:MP011:42", "65:MP011:52", "65:MP011:63"}));
        QVERIFY(dl.first().message.contains("LoadingMode=12") && dl.first().message.contains("0..8"));
        for (const Diagnostic& x : dl) QCOMPARE(x.severity, QStringLiteral("warning"));
        // A bad still time is a bad duration (MP013); empty values are absent, not errors.
        dl.clear();
        insertDirectorySpec(parseLine("!InsertDirectory D:\\x\tDefPTime=soon\tLoadingMode=\tSubDirs=", {}, nullptr, 9), {}, &dl);
        QCOMPARE(dl.size(), 1);
        QCOMPARE(QString("%1:%2:%3").arg(dl.first().line).arg(dl.first().code).arg(dl.first().col), QStringLiteral("9:MP013:23"));
        // Other lines are not folders.
        QVERIFY(!insertDirectorySpec(entryAtLine(d, 15)) && !insertDirectorySpec(entryAtLine(d, 8)) && !insertDirectorySpec(entryAtLine(d, 1)));
    }

    void insertPlaylistSpecs() {
        const Document g = parse(readAll(fx("grammar_basic.in")));
        auto pl = insertPlaylistSpec(entryAtLine(g, 11));
        QVERIFY(pl);
        QCOMPARE(pl->path, QStringLiteral("C:\\Sync\\NEWS HUB\\NEWS 00 TEST.in"));
        QCOMPARE(pl->loadingMode, LoadingMode::Normal);
        QVERIFY(!pl->loadingModeWritten && !pl->backtime && !pl->untilNextEvent());
        QVERIFY(pl->unmodelled.isEmpty() && pl->unknownKeys.isEmpty());   // ReplaceWithDirectoryMaxAge=0 is off
        const Document d = parse(readAll(fx("grammar_instructions.in")));
        pl = insertPlaylistSpec(entryAtLine(d, 15));
        QVERIFY(pl->untilNextEvent() && pl->loadingModeWritten);
        QCOMPARE(pl->timeOffset, QStringLiteral("1"));
        QVERIFY(pl->backtime);
        // TimeOffset=0 is off; Valid and ReplaceWith* in effect are kept for the report; the flags read.
        pl = insertPlaylistSpec(parseLine("!InsertPlaylist D:\\n.in\tTimeOffset=0\tValid=2025-01-01\tReplaceWithName=X\tsublfe=1\tbc=3\tToSelect=2"));
        QVERIFY(!pl->backtime && pl->subLfe && !pl->manual);
        QCOMPARE(pl->bc, QStringLiteral("3"));
        QCOMPARE(pl->toSelect, 2);
        QCOMPARE(::keys(pl->unmodelled), (QStringList{"Valid", "ReplaceWithName"}));
        QVERIFY(!insertPlaylistSpec(entryAtLine(d, 4)));
    }

    void breakSpecs() {
        // The three head forms of the plant / binary (grammar_basic 15, 18, 35) read the same.
        const Document g = parse(readAll(fx("grammar_basic.in")));
        auto b = breakSpec(entryAtLine(g, 15));
        QVERIFY(b);
        QCOMPARE(b->number, 1);
        QCOMPARE(b->gpiConfig, QStringLiteral("1"));
        QCOMPARE(b->schedSec, -1);
        b = breakSpec(entryAtLine(g, 18));
        QCOMPARE(b->number, 2);
        QCOMPARE(b->gpiConfig, QStringLiteral("1"));
        b = breakSpec(entryAtLine(g, 35));
        QCOMPARE(b->number, 3);
        QVERIFY(b->gpiConfig.isEmpty() && b->unknownKeys.isEmpty());
        // A fixed-time pod, and the flags.
        const Document d = parse(readAll(fx("grammar_instructions.in")));
        b = breakSpec(entryAtLine(d, 9));
        QCOMPARE(b->schedTime, QStringLiteral("06:10:00"));
        QCOMPARE(b->schedSec, 6 * 3600 + 600);
        QVERIFY(b->clockTriggered && !b->manual);
        b = breakSpec(entryAtLine(d, 11));
        QCOMPARE(b->number, 2);
        QVERIFY(b->manual && b->outOfOrder && !b->clockTriggered);
        // schedtime=soon: not understood, reported, and the pod follows the event start.
        QList<Diagnostic> dl;
        b = breakSpec(entryAtLine(d, 66), {}, &dl);
        QCOMPARE(b->schedSec, -1);
        QCOMPARE(b->schedTime, QStringLiteral("soon"));
        QCOMPARE(dl.size(), 1);
        QCOMPARE(dl.first().code, QStringLiteral("MP011"));
        QCOMPARE(dl.first().col, 12);
        dl.clear();
        QCOMPARE(breakSpec(parseLine("!Break\t#=x\tschedtime=16:00:00:15", {}, nullptr, 3), {}, &dl)->number, -1);
        QCOMPARE(dl.size(), 1);                               // "#=x"; a frame count on schedtime is fine
        QCOMPARE(breakSpec(parseLine("!Break\tschedtime=16:00:00:15"))->schedSec, 57600);
        QVERIFY(!breakSpec(entryAtLine(d, 10)));
    }

    void osdSpecs() {
        // !ShowLogo in both head forms (sample.in TAB form, OSD-Instructions.in space form) reads the same.
        const Document s = parse(readAll(fx("sample.in"))), o = parse(readAll(fx("OSD-Instructions.in")));
        const auto a = showLogoSpec(s.entries.at(4)), b = showLogoSpec(o.entries.at(4));
        QVERIFY(a && b);
        QCOMPARE(a->file, QStringLiteral("D:\\Masterplay\\OSD\\Logo\\Masterplay Logo.bmp"));
        QCOMPARE(b->file, a->file);
        QCOMPARE(a->cfg, QStringLiteral("D:\\Masterplay\\OSD\\MasterPlay Logo.osd"));
        QCOMPARE(b->cfg, a->cfg);
        QCOMPARE(a->layer, -1);
        QVERIFY(a->unknownKeys.isEmpty() && b->unknownKeys.isEmpty());
        // The plant's form (grammar_basic 21): name and layer 1 (CG1).
        const Document g = parse(readAll(fx("grammar_basic.in")));
        auto logo = showLogoSpec(entryAtLine(g, 21));
        QCOMPARE(logo->name, QStringLiteral("BUG"));
        QCOMPARE(logo->layer, 1);
        QVERIFY(!logo->cg2());
        // Timing keys (grammar_instructions 2): a negative show value runs early.
        const Document d = parse(readAll(fx("grammar_instructions.in")));
        logo = showLogoSpec(entryAtLine(d, 2));
        QCOMPARE(logo->timing.show, QStringLiteral("at item"));
        QVERIFY(logo->timing.hasShowValue && logo->timing.hasHideValue);
        QCOMPARE(logo->timing.showValueSec, -10.0);
        QCOMPARE(logo->timing.hide, QStringLiteral("in time from show"));
        QCOMPARE(logo->timing.hideValueSec, 60.0);
        QCOMPARE(showLogoSpec(parseLine("!ShowLogo D:\\x.png"))->file, QStringLiteral("D:\\x.png"));   // a path head for f=
        QList<Diagnostic> dl;
        logo = showLogoSpec(entryAtLine(d, 67), {}, &dl);     // Layer=top, showvalue=1:2
        QCOMPARE(logo->layer, -1);
        QVERIFY(!logo->timing.hasShowValue);
        QCOMPARE(logo->file, QStringLiteral("D:\\Bugs\\BUG.png"));
        QCOMPARE(dl.size(), 2);
        QCOMPARE(dl.at(0).code + ":" + QString::number(dl.at(0).col), QStringLiteral("MP011:29"));
        QCOMPARE(dl.at(1).code + ":" + QString::number(dl.at(1).col), QStringLiteral("MP013:39"));

        // Crawls.
        auto c = showCrawlSpec(s.entries.at(2));
        QCOMPARE(c->text, QStringLiteral("Sample Crawl Text"));
        QCOMPARE(c->cfg, QStringLiteral("D:\\Masterplay\\OSD\\DefaultCrawl.osd"));
        QVERIFY(!c->fromFile);
        QCOMPARE(c->lines(), QStringList{"Sample Crawl Text"});
        c = showCrawlSpec(entryAtLine(g, 34));
        QCOMPARE(c->lines(), (QStringList{"Breaking news", "second line"}));
        QCOMPARE(c->layer, 6);
        QVERIFY(c->cg2());
        c = showCrawlSpec(entryAtLine(d, 17));
        QCOMPARE(c->lines(), (QStringList{"Breaking: Main St closed", "Use Route 58"}));
        QCOMPARE(c->maxLines, 2);
        QCOMPARE(c->direction, QStringLiteral("left"));
        c = showCrawlSpec(entryAtLine(d, 18));
        QVERIFY(c->fromFile && c->text.isEmpty());
        QCOMPARE(c->file, QStringLiteral("D:\\Crawl\\today.txt"));
        c = showCrawlSpec(parseLine("!ShowCrawl\tsource=D:\\t.txt\tcfg=c.osd"));
        QVERIFY(c->fromFile);
        QCOMPARE(c->file, QStringLiteral("D:\\t.txt"));
        c = showCrawlSpec(entryAtLine(d, 69));                // text that looks like key=value is still the text
        QCOMPARE(c->text, QStringLiteral("Temp=72 today"));
        QVERIFY(c->unknownKeys.isEmpty());
        ShowCrawlSpec br;
        br.text = "a<BR>b<br />c<br/>";
        QCOMPARE(br.lines(), (QStringList{"a", "b", "c", ""}));
        // Text.
        auto t = showTextSpec(entryAtLine(d, 19));
        QCOMPARE(t->text, QStringLiteral("Lower third text"));
        QCOMPARE(t->activeRow, 2);
        QCOMPARE(t->layer, 3);
        t = showTextSpec(entryAtLine(d, 20));
        QVERIFY(t->fromFile);
        QCOMPARE(t->file, QStringLiteral("D:\\Subs\\show.srt"));
        // Clocks: ShowClock in both head forms, the real-time clock, count down to a time / the next event, count up.
        for (const Entry* e : {&s.entries.at(6), &o.entries.at(6)}) {
            const auto k = clockSpec(*e);
            QVERIFY(k);
            QCOMPARE(k->kind, ClockKind::Clock);
            QCOMPARE(k->type, 0);
            QCOMPARE(k->cfg, QStringLiteral("D:\\Masterplay\\OSD\\DefaultRealtimeClock.osd"));
        }
        QVERIFY(clockSpec(entryAtLine(d, 22))->realTime);
        auto k = clockSpec(entryAtLine(d, 23));
        QCOMPARE(k->kind, ClockKind::CountDown);
        QVERIFY(k->toNextEvent);
        QCOMPARE(k->targetSec, -1);
        QCOMPARE(k->cfg, QStringLiteral("D:\\Masterplay\\OSD\\DefaultCountDown.osd"));
        QCOMPARE(clockSpec(entryAtLine(d, 24))->targetSec, 18 * 3600 + 30 * 60);
        k = clockSpec(entryAtLine(d, 25));
        QCOMPARE(k->kind, ClockKind::CountUp);
        QCOMPARE(k->targetSec, 6 * 3600);
        dl.clear();
        k = clockSpec(entryAtLine(d, 68), {}, &dl);           // "!CountDown To lunch"
        QCOMPARE(k->targetSec, -1);
        QVERIFY(!k->toNextEvent);
        QCOMPARE(dl.size(), 1);
        QCOMPARE(dl.first().col, 15);
        QVERIFY(dl.first().message.contains("\"lunch\""));
        // OSD off, repeat, change playlist, video, live stream, info.
        QVERIFY(isOsdOff(entryAtLine(g, 23)) && isOsdOff(entryAtLine(d, 6)) && !isOsdOff(entryAtLine(d, 64)));
        QVERIFY(isRepeat(entryAtLine(g, 4)) && isRepeat(entryAtLine(g, 42)) && !isRepeat(entryAtLine(g, 49)));
        QCOMPARE(changePlaylistTarget(entryAtLine(d, 40)), QStringLiteral("D:\\Lists\\Evening.in"));
        QVERIFY(changePlaylistTarget(s.entries.at(13)).isEmpty());   // sample.in's bare !ChangePlaylist
        auto v = videoSpec(entryAtLine(d, 29));
        QCOMPARE(v->source, QStringLiteral("Live source"));
        QVERIFY(v->forceLength && !v->setOnly && !v->audioJukebox);
        QVERIFY(videoSpec(s.entries.at(8))->source.isEmpty());       // "!Video "
        QVERIFY(videoSpec(parseLine("!Video Router 3 set only"))->setOnly);
        const auto ls = liveStreamSpec(entryAtLine(d, 30));
        QCOMPARE(ls->path, QStringLiteral("srt://192.0.2.10:9000"));
        QCOMPARE(ls->param, QStringLiteral("latency=200"));
        auto info = infoSpec(entryAtLine(d, 31));
        QCOMPARE(info->text, QStringLiteral("Morning news block"));
        QCOMPARE(info->episode, QStringLiteral("12"));
        QCOMPARE(info->expectedSec, 28 * 60 + 30.0);
        QVERIFY(info->programGuide && !info->includeOnWeb);
        QCOMPARE(infoSpec(entryAtLine(d, 32))->name, QStringLiteral("secondary event not defined"));
        QCOMPARE(infoSpec(entryAtLine(g, 1))->name, QStringLiteral("preamble: an item before the first event"));
        dl.clear();
        info = infoSpec(entryAtLine(d, 70), {}, &dl);
        QCOMPARE(info->expectedSec, -1.0);
        QCOMPARE(dl.size(), 1);
        QCOMPARE(dl.first().code + ":" + QString::number(dl.first().col), QStringLiteral("MP013:7"));
        // Each reader refuses other lines.
        const Entry& crawl = entryAtLine(d, 17);
        QVERIFY(!showLogoSpec(crawl) && !clockSpec(crawl) && !videoSpec(crawl) && !liveStreamSpec(crawl) && !infoSpec(crawl)
                && !showTextSpec(crawl) && !breakSpec(crawl) && !insertPlaylistSpec(crawl));
        QVERIFY(!showCrawlSpec(entryAtLine(d, 2)) && !showCrawlSpec(entryAtLine(d, 8)));
    }

    void playModes_data() {
        QTest::addColumn<QString>("header");   // the header's fields
        QTest::addColumn<QString>("items");    // the event's lines, '\n'-separated
        QTest::addColumn<int>("effective");
        QTest::addColumn<int>("inBand");
        QTest::addColumn<int>("modeAt");
        QTest::addColumn<int>("repeatAt");
        QTest::addColumn<int>("loop");         // loopStart()
        QTest::addColumn<bool>("mp014");
        auto row = [](const char* n, const QString& h, const QString& i, int eff, int band, int at, int rep, int loop, bool w) {
            QTest::newRow(n) << h << i << eff << band << at << rep << loop << w;
        };
        row("repeat beats normal", "PlayMode=1", "!Repeat\nS:\\a.mpg", 2, 2, 0, 0, 1, true);
        row("alias after an OSD line", "PlayMode=1", "!ShowLogo\tf=x.png\n!Wiederholen\nS:\\a.mpg", 2, 2, 1, 1, 2, true);
        row("normal beats repeat", "PlayMode=2", "!Normal\nS:\\a.mpg", 1, 1, 0, -1, -1, true);
        row("fill up", "PlayMode=1", "!Fillup\nS:\\a.mpg", 3, 3, 0, -1, -1, true);
        row("wait for GPI", "PlayMode=1", "!WaitForGPI", 4, 4, 0, -1, -1, true);
        row("agree", "PlayMode=2", "!Repeat\nS:\\a.mpg", 2, 2, 0, 0, 1, false);
        row("no header mode", "EventDuration=00:10:00", "!Wiederholen\nS:\\a.mpg", 2, 2, 0, 0, 1, false);
        row("header not defined", "PlayMode=0", "!Break\nS:\\a.mpg", 4, 4, 0, -1, -1, false);
        row("header unusable", "PlayMode=9", "!Repeat", 2, 2, 0, 0, 1, false);   // MP011 from the parser instead
        row("break after media", "PlayMode=4", "S:\\a.mpg\n!Break\t#=1\nS:\\b.mpg", 4, 4, 1, -1, -1, false);
        row("sub-playlist decides", "PlayMode=1", "!InsertPlaylist D:\\n.in\n!Repeat\nS:\\a.mpg", 1, -1, 0, 1, 2, false);
        row("package decides", "PlayMode=2", "!Package\n!Normal", 2, -1, 0, -1, 0, false);
        row("header repeat, no marker", "PlayMode=2", "S:\\a.mpg\nS:\\b.mpg", 2, -1, -1, -1, 0, false);   // Saturday_0200
        row("first marker wins", "PlayMode=1", "!Normal\n!Repeat\nS:\\a.mpg", 1, 1, 0, 1, 2, false);
        row("after ChangePlaylist", "PlayMode=1", "!ChangePlaylist D:\\x.in\n!Repeat", 1, -1, -1, -1, -1, false);
        row("unknown is no marker", "PlayMode=1", "!Repeatt\n!Normal", 1, 1, 1, -1, -1, false);
        row("repeat on the last line", "PlayMode=2", "S:\\a.mpg\n!Repeat", 2, 2, 1, 1, 2, false);
        row("no items", "PlayMode=3", "", 3, -1, -1, -1, -1, false);
    }
    void playModes() {
        QFETCH(QString, header); QFETCH(QString, items); QFETCH(int, effective); QFETCH(int, inBand); QFETCH(int, modeAt);
        QFETCH(int, repeatAt); QFETCH(int, loop); QFETCH(bool, mp014);
        QString text = "0000-00-00T06:00:00\t" + header + "\r\n";
        for (const QString& l : items.split('\n', Qt::SkipEmptyParts)) text += l + "\r\n";
        const QByteArray bytes = text.toUtf8();
        const Document d = parse(bytes);
        QCOMPARE(d.events.size(), 1);
        const Event& ev = d.events.first();
        QCOMPARE(ev.effectivePlayMode, effective);
        QCOMPARE(ev.inBandPlayMode, inBand);
        QCOMPARE(ev.modeAt, modeAt);
        QCOMPARE(ev.repeatAt, repeatAt);
        QCOMPARE(loopStart(ev), loop);
        const Diagnostic* w = findDiag(d, "MP014");
        QCOMPARE(w != nullptr, mp014);
        if (w) {
            QCOMPARE(w->line, 1);
            QCOMPARE(w->col, 21);                             // the PlayMode field
            QCOMPARE(w->severity, QStringLiteral("warning"));
            QVERIFY2(w->message.contains(QString("(line %1)").arg(d.entries.at(ev.items.at(ev.modeAt)).line)), qPrintable(w->message));
        }
        QCOMPARE(serialize(d), bytes);                        // the decision changes nothing on disk
        // resolvePlayModes() is repeatable (after edits) and gives the same answer.
        Document again = d;
        for (Event& e : again.events) { e.effectivePlayMode = 9; e.inBandPlayMode = e.modeAt = e.repeatAt = 7; }
        resolvePlayModes(again);
        QCOMPARE(again.events.first().effectivePlayMode, effective);
        QCOMPARE(again.events.first().modeAt, modeAt);
        QCOMPARE(again.events.first().repeatAt, repeatAt);
        // instructionDiagnostics() is the catalog's share of parse()'s list (all of it here, but the parser's MP011 for
        // PlayMode=9).
        const QList<Diagnostic> mine = instructionDiagnostics(d);
        for (const Diagnostic& g : mine)
            QVERIFY(std::any_of(d.diagnostics.cbegin(), d.diagnostics.cend(),
                                [&](const Diagnostic& x) { return x.line == g.line && x.col == g.col && x.code == g.code; }));
        QCOMPARE(mine.size(), d.diagnostics.size() - (header == u"PlayMode=9" ? 1 : 0));
    }

    void playModesInFixtures() {
        auto modes = [](const Document& d, auto field) { QList<int> out; for (const Event& e : d.events) out << field(e); return out; };
        auto eff = [](const Event& e) { return e.effectivePlayMode; };
        auto band = [](const Event& e) { return e.inBandPlayMode; };
        auto at = [](const Event& e) { return e.modeAt; };
        auto rep = [](const Event& e) { return e.repeatAt; };
        auto loop = [](const Event& e) { return loopStart(e); };
        const Document s = parse(readAll(fx("sample.in")));   // PlayMode=2 and !Repeat first: agreed
        QCOMPARE(modes(s, eff), QList<int>{2});
        QCOMPARE(modes(s, rep), QList<int>{0});
        QCOMPARE(modes(s, loop), QList<int>{1});
        QCOMPARE(modes(parse(readAll(fx("Testfiles.in"))), eff), QList<int>{2});
        const Document sat = parse(readAll(fx("Saturday_0200.in")));   // PlayMode=2, no !Repeat: the whole event loops
        QCOMPARE(modes(sat, eff), QList<int>{2});
        QCOMPARE(modes(sat, band), QList<int>{-1});
        QCOMPARE(modes(sat, loop), QList<int>{0});
        QCOMPARE(modes(parse(readAll(fx("FirstPlaylist.in"))), loop), QList<int>{-1});
        for (const QString& f : {QString("sample.in"), QString("OSD-Instructions.in"), QString("Testfiles.in"), QString("Saturday_0200.in"),
                                 QString("FirstPlaylist.in")})
            QVERIFY2(!findDiag(parse(readAll(fx(f))), "MP014") && !findDiag(parse(readAll(fx(f))), "MP011"), qPrintable(f));

        const Document g = parse(readAll(fx("grammar_basic.in")));
        QCOMPARE(modes(g, eff), (QList<int>{2, 1, 1, 4, 1, 1, 1, 4, 1, 1, 1, 1, 2, 1, 1, 1}));
        QCOMPARE(modes(g, band), (QList<int>{2, -1, -1, 4, -1, -1, -1, 4, -1, -1, -1, -1, 2, -1, 1, -1}));
        QCOMPARE(modes(g, at), (QList<int>{0, -1, 0, 0, -1, -1, -1, 2, -1, -1, -1, -1, 0, -1, 0, -1}));
        QCOMPARE(modes(g, rep), (QList<int>{0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, -1, -1, -1}));

        const Document d = parse(readAll(fx("grammar_instructions.in")));
        QCOMPARE(modes(d, eff), (QList<int>{2, 4, 1, 1, 1, 3, 4, 1}));
        QCOMPARE(modes(d, band), (QList<int>{2, 4, -1, 1, -1, -1, 4, -1}));
        QCOMPARE(modes(d, at), (QList<int>{1, 1, 0, 0, 0, -1, 3, -1}));
        QCOMPARE(modes(d, rep), (QList<int>{1, -1, 1, -1, -1, -1, -1, -1}));
        QCOMPARE(modes(d, loop), (QList<int>{2, -1, 2, -1, -1, -1, -1, -1}));
    }

    void grammarInstructions() {
        const QByteArray bytes = readAll(fx("grammar_instructions.in"));
        QVERIFY(bytes.startsWith("\xEF\xBB\xBF"));
        QCOMPARE(bytes.count("\r\n"), 71);
        QCOMPARE(bytes.count('\n'), 71);
        const Document d = parse(bytes);
        QCOMPARE(d.entries.size(), 71);
        QCOMPARE(d.events.size(), 8);
        QCOMPARE(serialize(d), bytes);
        // Exactly the planned diagnostics: MP014 on the two headers whose first marker disagrees, MP011 for the two unknown
        // names, five bad / unknown keys on line 65, the bad schedtime, the bad Layer, the bad countdown target; MP013 for
        // the two bad durations.
        QCOMPARE(diagList(d), QStringLiteral("1:MP014:44 26:MP014:44 63:MP011:2 64:MP011:2 65:MP011:27 65:MP011:42 65:MP011:52 "
                                             "65:MP011:63 65:MP011:74 66:MP011:12 67:MP011:29 67:MP013:39 68:MP011:15 70:MP013:7"));
        QCOMPARE(instructionDiagnostics(d).size(), 14);
        QVERIFY(findDiag(d, "MP014")->message.contains("\"!Wiederholen\" (line 3)"));
        QVERIFY(d.diagnostics.at(1).message.contains("\"!Normal\" (line 27) sets 1 (normal)"));
        QVERIFY(d.diagnostics.at(2).message.contains("\"!FooBar\""));
        QVERIFY(d.diagnostics.at(8).message.contains("\"Frobnicate\""));
        // Every catalog instruction id appears, so the round trip above covers each one's written form.
        QSet<int> seen;
        for (const Entry& e : d.entries) if (e.kind == LineKind::Instruction) seen << int(identify(e).id);
        for (int id = int(Instr::Unknown); id <= int(Instr::Legacy); ++id) QVERIFY2(seen.contains(id), qPrintable(QString::number(id)));
        // Selected lines.
        QCOMPARE(identify(entryAtLine(d, 48)).head, QStringLiteral("Out of network indicator"));
        QCOMPARE(identify(entryAtLine(d, 58)).id, Instr::WinTV);
        QCOMPARE(identify(entryAtLine(d, 55)).head, QString());   // "VCR#=1" is a field head
        QCOMPARE(entryAtLine(d, 55).value("VCR#"), QStringLiteral("1"));
        QCOMPARE(identify(entryAtLine(d, 51)).head, QStringLiteral("00000001"));
        QCOMPARE(unknownKeys(entryAtLine(d, 65)), QStringList{"Frobnicate"});
        QVERIFY(unknownKeys(entryAtLine(d, 45)).isEmpty());   // TTS keys are not checked (unsupported device line)
    }

    void unknownInstructionsRoundTrip() {
        const QByteArray in = "0000-00-00T06:00:00\tPlayMode=2\r\n"
                              "!FooBar some argument\tx=1\r\n"                         // 2 unknown name
                              "!OSD\toff\r\n"                                           // 3 unknown in the TAB form
                              "!Zebra\r\n"                                              // 4
                              "!InsertDirectory D:\\x\tFrobnicate=1\tLoadingMode=3\tstray\r\n"   // 5 unknown key, stray bare field
                              "!TTS\tWhatever=1\r\n"                                    // 6 device keys are not checked
                              "!Repeat\tx=1\r\n";                                       // 7 nor marker keys
        const Document d = parse(in);
        QCOMPARE(diagList(d), QStringLiteral("2:MP011:2 3:MP011:2 4:MP011:2 5:MP011:23 5:MP011:50"));
        for (const Diagnostic& g : d.diagnostics) QCOMPARE(g.severity, QStringLiteral("warning"));
        QVERIFY(d.diagnostics.at(0).message.contains("\"!FooBar\""));
        QVERIFY(d.diagnostics.at(4).message.contains("\"stray\""));
        // Kept verbatim: byte for byte, every line renders back, a canonical write changes only the BOM.
        QCOMPARE(serialize(d), in);
        for (const Entry& e : d.entries) QCOMPARE(renderEntry(e), e.raw);
        Document all = d;
        for (Entry& e : all.entries) e.dirty = true;
        QCOMPARE(serialize(all), in);
        const QByteArray canon = serialize(d, WriteOptions::canonicalForm());
        QCOMPARE(canon, QByteArray("\xEF\xBB\xBF") + in);
        QCOMPARE(semanticDiff(d, parse(canon)), QString());
        QCOMPARE(diagList(parse(canon)), diagList(d));
        // They count as unknown, by their written name, and the JSON flags them.
        const QJsonObject st = instructionStats(d);
        QCOMPARE(st["instructions"].toInt(), 6);
        QCOMPARE(st["unknown"].toInt(), 3);
        QCOMPARE(st["unknownNames"].toObject(), (QJsonObject{{"FooBar", 1}, {"OSD", 1}, {"Zebra", 1}}));
        const QJsonArray items = d.toJson()["events"].toArray().at(0).toObject()["items"].toArray();
        QCOMPARE(items.at(0).toObject()["name"].toString(), QStringLiteral("FooBar"));
        QVERIFY(items.at(0).toObject()["unknown"].toBool());
        QVERIFY(!items.at(3).toObject().contains("unknown"));
        QCOMPARE(unknownKeys(entryAtLine(d, 5)), (QStringList{"Frobnicate", "stray"}));
        // The spec still reads what it knows.
        QCOMPARE(insertDirectorySpec(entryAtLine(d, 5))->loadingMode, LoadingMode::SelectRandom);
    }

    void makeInstructions() {
        QCOMPARE(makeInstruction(Instr::Repeat).raw, QStringLiteral("!Repeat"));
        QCOMPARE(makeInstruction(Instr::OsdOff).raw, QStringLiteral("!OSD off"));
        const QList<Entry> made = {
            makeInstruction(Instr::Break, {}, {{"gpicfg", "1"}, {"#", "1"}}),
            makeInstruction(Instr::ShowLogo, {}, {{"f", "D:\\Bugs\\BUG.png"}, {"Layer", "1"}}),
            makeInstruction(Instr::InsertDirectory, "C:\\Sync\\NEWS HUB\\TOP STORY",
                            {{"LoadingMode", "3"}, {"ToSelect", "1"}, {"MaxAge", ""}, {"ReplaceWithDirectoryMaxAge", "0"}}),
            makeInstruction(Instr::CountDownTo, "NextEvent"),
            makeInstruction(Instr::SetConfig, "Logo", {{"Name", "1"}}),
            makeInstruction("Wiederholen"),
            makeInstruction(Instr::ShowCrawl, "Temp=72 today", {{"cfg", "c.osd"}}),
            makeInstruction(Instr::OsdOff),
            makeInstruction(Instr::Video),
        };
        const QStringList want = {"!Break\tgpicfg=1\t#=1", "!ShowLogo\tf=D:\\Bugs\\BUG.png\tLayer=1",
                                  entryAtLine(parse(readAll(fx("grammar_basic.in"))), 12).raw,   // what the editor wrote
                                  "!CountDown To NextEvent", "!Set Config Logo\tName=1", "!Wiederholen", "!ShowCrawl Temp=72 today\tcfg=c.osd",
                                  "!OSD off", "!Video"};
        QCOMPARE(made.size(), want.size());
        for (int i = 0; i < made.size(); ++i) {
            const Entry& e = made.at(i);
            QCOMPARE(e.raw, want.at(i));
            QCOMPARE(renderEntry(e), e.raw);
            QVERIFY(e.dirty && e.eol.isEmpty() && e.bytes.isEmpty());
            QCOMPARE(e.kind, LineKind::Instruction);
            // It parses back to the same instruction, head and fields.
            const Entry back = parseLine(e.raw);
            const Instruction a = identify(e), b = identify(back);
            QCOMPARE(b.id, a.id);
            QCOMPARE(b.name, a.name);
            QCOMPARE(b.head, a.head);
            QCOMPARE(renderEntry(back), e.raw);
        }
        QCOMPARE(identify(made.at(5)).id, Instr::Repeat);
        QCOMPARE(insertDirectorySpec(made.at(2))->picks(), 1);
        QCOMPARE(showCrawlSpec(parseLine(made.at(6).raw))->text, QStringLiteral("Temp=72 today"));
        QCOMPARE(makeInstruction(Instr::Unknown).kind, LineKind::Blank);
        // Inserted into a document (Saturday_0200: no final newline) it takes the document's terminator and the event
        // then loops after it.
        const QByteArray sat = readAll(fx("Saturday_0200.in"));
        Document doc = parse(sat);
        Entry rep = makeInstruction(Instr::Repeat);
        rep.eol = doc.defaultEol();
        doc.entries.insert(1, rep);
        const QByteArray out = serialize(doc);
        const qsizetype nl = sat.indexOf('\n') + 1;
        QCOMPARE(out, sat.left(nl) + "!Repeat\r\n" + sat.mid(nl));
        const Document again = parse(out);
        QCOMPARE(again.events.first().repeatAt, 0);
        QCOMPARE(loopStart(again.events.first()), 1);
        QVERIFY(again.diagnostics.isEmpty());
    }

    void instructionStatsView() {
        const QJsonObject g = instructionStats(parse(readAll(fx("grammar_basic.in"))));
        QCOMPARE(g["instructions"].toInt(), 16);
        QCOMPARE(g["known"].toInt(), 16);
        QCOMPARE(g["unknown"].toInt(), 0);
        QCOMPARE(g["byName"].toObject(), (QJsonObject{{"Info", 1}, {"Repeat", 2}, {"InsertPlaylist", 1}, {"InsertDirectory", 2}, {"Break", 3},
                                                      {"ShowLogo", 2}, {"OSD off", 1}, {"ShowClock", 1}, {"Video", 1}, {"ShowCrawl", 1},
                                                      {"Normal", 1}}));
        QCOMPARE(g["aliases"].toObject(), (QJsonObject{{"Wiederholen", 1}}));
        QCOMPARE(g["bySupport"].toObject(), (QJsonObject{{"structural", 6}, {"mapped", 10}, {"partial", 0}, {"unsupported", 0}, {"unknown", 0}}));
        const QJsonObject d = instructionStats(parse(readAll(fx("grammar_instructions.in"))));
        QCOMPARE(d["instructions"].toInt(), 59);
        QCOMPARE(d["known"].toInt(), 57);
        QCOMPARE(d["unknownNames"].toObject(), (QJsonObject{{"FooBar", 1}, {"OSD", 1}}));
        QCOMPARE(d["aliases"].toObject(), (QJsonObject{{"Wiederholen", 1}, {"SetConfig", 1}, {"Show TV", 1}}));
        QCOMPARE(d["bySupport"].toObject(), (QJsonObject{{"structural", 10}, {"mapped", 21}, {"partial", 4}, {"unsupported", 22}, {"unknown", 2}}));
        const QJsonObject byName = d["byName"].toObject();
        QCOMPARE(byName["CountDown To"].toInt(), 3);
        QCOMPARE(byName["Set Config"].toInt(), 2);
        QCOMPARE(byName["WinTV"].toInt(), 2);
        QCOMPARE(byName["Write Error"].toInt(), 1);
        int sum = 0;
        for (const QJsonValue& v : byName) sum += v.toInt();
        QCOMPARE(sum, 57);
    }

    // ---- the plant corpus (opt-in) ------------------------------------------------------------------------------

    void plantCorpus() {
        const QString dir = qEnvironmentVariable("PLAYPLAY_MP_SAMPLES");
        if (dir.isEmpty()) QSKIP("PLAYPLAY_MP_SAMPLES is not set (point it at a folder holding plant_playlist.in)");
        const QByteArray bytes = readAll(QDir(dir).filePath("plant_playlist.in"));
        if (bytes.isEmpty()) QSKIP("plant_playlist.in not found under PLAYPLAY_MP_SAMPLES");
        const Document d = parse(bytes);
        QCOMPARE(serialize(d), bytes);
        for (const Entry& e : d.entries) QVERIFY2(renderEntry(e) == e.raw, qPrintable(QString("line %1").arg(e.line)));
        QCOMPARE(d.encoding, Encoding::Utf8Bom);
        QCOMPARE(d.eolStyle(), QStringLiteral("crlf"));
        const QJsonObject st = d.stats();
        QCOMPARE(st["events"].toInt(), 374);
        QCOMPARE(st["media"].toInt(), 2068);
        QCOMPARE(st["instructions"].toInt(), 519);
        QCOMPARE(st["unknown"].toInt(), 0);
        QCOMPARE(st["byDayKind"].toObject()["weekly"].toInt(), 374);
        const int perCode[] = {58, 54, 52, 52, 52, 56, 50};
        for (int c = 1; c <= 7; ++c) QCOMPARE(st["byWeekday"].toObject()[QString::number(c)].toInt(), perCode[c - 1]);
        const QJsonObject ins = st["byInstruction"].toObject();
        QCOMPARE(ins["InsertPlaylist"].toInt(), 272);
        QCOMPARE(ins["InsertDirectory"].toInt(), 211);
        QCOMPARE(ins["Break"].toInt(), 25);
        QCOMPARE(ins["OSD"].toInt(), 6);
        QCOMPARE(ins["ShowLogo"].toInt(), 5);
        int acts = 0, brackets = 0, legacy = 0;
        for (const Entry& e : d.entries) {
            if (e.kind == LineKind::Media && e.has("actinout")) ++acts;
            if (e.kind == LineKind::Media && e.forcedDurationSec >= 0) ++brackets;
            if (e.kind == LineKind::Header && e.stamp.legacy) ++legacy;
        }
        QCOMPARE(acts, 209);
        QCOMPARE(brackets, 3);
        QCOMPARE(legacy, 374);
        QCOMPARE(st["diagnostics"].toObject()["error"].toInt(), 0);
        // MP-02b: nothing at all to report (sorted by weekday code then time, so no MP003 / MP004; CRLF only; UTF-8
        // BOM; the 3 brackets are forced durations, so no MP012).
        QVERIFY2(d.diagnostics.isEmpty(), qPrintable(diagList(d).left(400)));
        // The plant file is already in canonical form with its legacy stamps kept; it converts to ISO and back exactly.
        WriteOptions keep = WriteOptions::canonicalForm();
        keep.stamps = StampForm::Keep;
        QCOMPARE(serialize(d, keep), bytes);
        const Document iso = parse(serialize(d, WriteOptions::canonicalForm()));
        QCOMPARE(semanticDiff(d, iso), QString());
        QCOMPARE(iso.stats()["events"].toInt(), 374);
        QVERIFY(iso.diagnostics.isEmpty());
        WriteOptions legacyForm = keep;
        legacyForm.stamps = StampForm::Legacy;
        QCOMPARE(serialize(iso, legacyForm), bytes);

        // MP-03: every instruction is in the catalog, "!OSD off" is one instruction, and the typed specs read the plant's
        // forms (design §2.8): InsertDirectory (SelectRandom, 1) x205, (SelectNormal, 1) x2, the whole folder x4, with
        // nothing unmodelled or unknown; Break pods 1 / 2 / 3; every InsertPlaylist plain (no back-timing).
        const QJsonObject is = instructionStats(d);
        QCOMPARE(is["unknown"].toInt(), 0);
        QCOMPARE(is["byName"].toObject(), (QJsonObject{{"InsertPlaylist", 272}, {"InsertDirectory", 211}, {"Break", 25}, {"OSD off", 6},
                                                       {"ShowLogo", 5}}));
        QMap<QString, int> folderModes, pods;
        int playlists = 0, logos = 0;
        for (const Entry& e : d.entries) {
            if (const auto dir = insertDirectorySpec(e)) {
                folderModes[loadingModeName(dir->loadingMode) + "/" + QString::number(dir->picks())]++;
                QVERIFY2(dir->unmodelled.isEmpty() && dir->unknownKeys.isEmpty() && dir->maxAgeHours < 0, qPrintable(QString::number(e.line)));
            }
            if (const auto b = breakSpec(e)) pods[QString::number(b->number)]++;
            if (const auto pl = insertPlaylistSpec(e)) { ++playlists; QVERIFY(!pl->backtime && pl->unmodelled.isEmpty() && pl->path.endsWith(".in", Qt::CaseInsensitive)); }
            if (const auto logo = showLogoSpec(e)) { ++logos; QVERIFY(!logo->file.isEmpty() && !logo->cfg.isEmpty() && logo->layer == 1); }
        }
        QCOMPARE(folderModes, (QMap<QString, int>{{"selectRandom/1", 205}, {"selectNormal/1", 2}, {"normal/0", 4}}));
        QCOMPARE(pods, (QMap<QString, int>{{"1", 9}, {"2", 8}, {"3", 8}}));
        QCOMPARE(playlists, 272);
        QCOMPARE(logos, 5);
        // Play modes: the 9 PlayMode=4 events start with a !Break pod, 272 events are decided by an !InsertPlaylist (the
        // sub-playlist), the rest have no marker; header and in-band agree everywhere (no MP014 above).
        int byMode[5] = {}, byPlaylist = 0, loops = 0;
        for (const Event& ev : d.events) {
            ++byMode[ev.effectivePlayMode];
            if (ev.modeAt >= 0 && identify(d.entries.at(ev.items.at(ev.modeAt))).id == Instr::InsertPlaylist) ++byPlaylist;
            if (loopStart(ev) >= 0) ++loops;
        }
        QCOMPARE(byMode[1], 365);
        QCOMPARE(byMode[4], 9);
        QCOMPARE(byPlaylist, 272);
        QCOMPARE(loops, 0);
        qInfo("plant corpus: %d lines, %d diagnostics", st["lines"].toInt(), int(d.diagnostics.size()));
    }
};

QTEST_GUILESS_MAIN(TestMasterplayIn)
#include "test_masterplayin.moc"
