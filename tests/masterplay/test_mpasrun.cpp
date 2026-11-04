// MasterPlay log readers (MP-04, playout/masterplay/mpasrun.h): the as-run LogFile.log (000 started, 118 missed,
// durations derived from the next boundary with commands transparent, capped at 3600 s, End Of Play closing the airing
// before it), Status.log / Operator.log (one message format), missed entries paired with "File not found" lines, the
// stamp forms, encodings, time zones (DST gap and repeated hour), stable dedup keys and the content-type rules.
// Fixtures: LogFile.log, Status.log and Operator.log in the MasterPlay 6.x formats (manifest.tsv; titles, paths and the
// computer name are placeholders) and the synthetic plant_asrun.log (plant-shaped lines: prefetch copies, bare names,
// commands around stories, End Of Play; invented names). PLAYPLAY_MP_SAMPLES, when set, also runs every *.log in that folder through the readers (plant logs are never
// committed).
// MP-09: the as-run DB writer: the row mapping (START / SKIP / CMD, the dedup id "mp:<key>", derived ends, assetId only
// with countTowardContracts, the range), a temp ScheduleStore database written through the writer's own connection
// (dry run, insert, re-runs inserting 0, commands added later, batch transactions, contract pacing, errors), and a
// 20,000-row bulk insert while a ScheduleStore on another thread keeps writing the same file.
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <atomic>
#include <limits>
#include "../../playout/masterplay/mpasrun.h"
#include "../../playout/schedule/schedulestore.h"

using namespace mp;
using namespace mp::logs;

namespace {

QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
QByteArray crlf(const QStringList& lines) { return (lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8(); }

const AsRunEntry* entryAt(const AsRunLog& log, int line) {
    for (const AsRunEntry& e : log.entries) if (e.line == line) return &e;
    return nullptr;
}
QString diagList(const QList<Diagnostic>& ds) {   // "line:code:col ..."
    QStringList o;
    for (const Diagnostic& d : ds) o << QStringLiteral("%1:%2:%3").arg(d.line).arg(d.code).arg(d.col);
    return o.join(' ');
}
qint64 utcMs(const char* iso) { return QDateTime::fromString(QString::fromLatin1(iso), Qt::ISODate).toMSecsSinceEpoch(); }
QByteArray utf16(const QString& s, bool le) {
    QByteArray b = le ? QByteArray("\xFF\xFE", 2) : QByteArray("\xFE\xFF", 2);
    for (QChar c : s) {
        const char lo = char(c.unicode() & 0xFF), hi = char(c.unicode() >> 8);
        b += le ? lo : hi;
        b += le ? hi : lo;
    }
    return b;
}

} // namespace

class TestMpAsRun : public QObject {
    Q_OBJECT
private slots:
    // ---- LogFile.log --------------------------------------------------------------------------------

    void logFileCounts() {
        const AsRunLog log = parseAsRun(readAll(fx("LogFile.log")));
        QCOMPARE(log.encoding, Encoding::Utf8);
        QCOMPARE(log.physicalLines, 883);
        QCOMPARE(log.entries.size(), 883);
        QCOMPARE(log.unparsed, 0);
        QCOMPARE(log.blank, 0);
        QVERIFY2(log.diagnostics.isEmpty(), qPrintable(diagList(log.diagnostics)));
        const QJsonObject s = log.stats();
        QCOMPARE(s.value("played").toInt(), 621);      // code 000: airings, executed instructions, End Of Play
        QCOMPARE(s.value("missed").toInt(), 262);      // code 118
        QCOMPARE(s.value("other").toInt(), 0);
        QCOMPARE(s.value("byCode").toObject(), QJsonObject({{"000", 621}, {"118", 262}}));
        QCOMPARE(s.value("airings").toInt(), 473);
        QCOMPARE(s.value("commands").toInt(), 142);
        QCOMPARE(s.value("endOfPlay").toInt(), 6);
        QCOMPARE(s.value("durationKnown").toInt(), 473);
        QCOMPARE(s.value("truncated").toInt(), 0);
        QCOMPARE(s.value("open").toInt(), 0);          // the last airing is closed by the missed-entry burst
        QCOMPARE(s.value("playedSec").toDouble(), 3894.0);
        QCOMPARE(s.value("files").toInt(), 11);
        QCOMPARE(s.value("missedFiles").toInt(), 1);
        QCOMPARE(s.value("first").toString(), QStringLiteral("2025-06-19T18:21:16"));
        QCOMPARE(s.value("last").toString(), QStringLiteral("2025-06-21T02:35:27"));
        QCOMPARE(s.value("naive").toBool(), true);
        QCOMPARE(s.value("capSec").toInt(), 3600);
        QCOMPARE(s.value("byInstruction").toObject(),
                 QJsonObject({{"ShowCrawl", 35}, {"Repeat", 32}, {"ShowLogo", 31}, {"ShowClock", 29}, {"Video", 3}, {"TTS", 3},
                              {"DTMF", 3}, {"Macro", 3}, {"ChangePlaylist", 3}}));
        int played = 0, missed = 0;
        for (const AsRunEntry& e : log.entries) {
            if (e.started()) ++played;
            if (e.missed()) { ++missed; QCOMPARE(e.durationMs, qint64(-1)); QVERIFY(!e.airing()); }
            if (!e.airing()) QCOMPARE(e.durationMs, qint64(-1));
            if (e.kind == AsRunKind::Command) QVERIFY2(e.knownInstruction, qPrintable(e.text));
        }
        QCOMPARE(played, 621);
        QCOMPARE(missed, 262);
    }

    void logFileDurations_data() {
        QTest::addColumn<int>("line"); QTest::addColumn<int>("sec"); QTest::addColumn<int>("boundary"); QTest::addColumn<bool>("eop");
        QTest::newRow("first still, 3 commands after it") << 1 << 10 << 5 << false;
        QTest::newRow("next still") << 5 << 10 << 6 << false;
        QTest::newRow("operator skip, same second") << 84 << 0 << 85 << false;
        QTest::newRow("movie closed by End Of Play") << 85 << 2075 << 86 << true;
        QTest::newRow("playlist change") << 274 << 3 << 276 << true;
        QTest::newRow("End Of Play 288") << 287 << 10 << 288 << true;
        QTest::newRow("End Of Play 304") << 303 << 10 << 304 << true;
        QTest::newRow("End Of Play 320") << 319 << 10 << 320 << true;
        QTest::newRow("End Of Play 330") << 329 << 1 << 330 << true;
        QTest::newRow("closed by the missed burst") << 621 << 100 << 622 << false;
    }
    void logFileDurations() {
        QFETCH(int, line); QFETCH(int, sec); QFETCH(int, boundary); QFETCH(bool, eop);
        static const AsRunLog log = parseAsRun(readAll(fx("LogFile.log")));
        const AsRunEntry* e = entryAt(log, line);
        QVERIFY(e);
        QVERIFY(e->airing());
        QCOMPARE(e->durationMs, qint64(sec) * 1000);
        QCOMPARE(e->boundaryLine, boundary);
        QCOMPARE(e->closedByEndOfPlay, eop);
        QVERIFY(!e->truncated && !e->open);
    }

    void logFileLines() {
        const AsRunLog log = parseAsRun(readAll(fx("LogFile.log")));
        const AsRunEntry* first = entryAt(log, 1);
        QCOMPARE(first->path, QStringLiteral("D:\\media\\testcards\\Card-6.bmp"));
        QCOMPARE(first->file, QStringLiteral("Card-6.bmp"));
        QCOMPARE(first->title, QStringLiteral("Card-6"));
        QCOMPARE(first->fields, QStringList{QString()});   // the trailing TAB
        QVERIFY(!first->bare && !first->prefetch);
        QCOMPARE(first->stamp(), QStringLiteral("2025-06-19T18:21:16"));
        QCOMPARE(entryAt(log, 2)->instruction, QStringLiteral("Repeat"));
        const AsRunEntry* crawl = entryAt(log, 3);
        QCOMPARE(crawl->kind, AsRunKind::Command);
        QCOMPARE(crawl->instruction, QStringLiteral("ShowCrawl"));
        QCOMPARE(crawl->arg, QStringLiteral("Sample Crawl Text"));
        QCOMPARE(crawl->fields, QStringList{QStringLiteral("cfg=D:\\Masterplay\\OSD\\DefaultCrawl.osd")});
        const AsRunEntry* logo = entryAt(log, 7);
        QCOMPARE(logo->instruction, QStringLiteral("ShowLogo"));
        QCOMPARE(logo->arg, QString());
        QCOMPARE(logo->fields.value(0), QStringLiteral("f=D:\\Masterplay\\OSD\\Logo\\Masterplay Logo.bmp"));
        QCOMPARE(entryAt(log, 86)->kind, AsRunKind::EndOfPlay);
        const AsRunEntry* mkv = entryAt(log, 74);             // a bare name without the trailing TAB
        QCOMPARE(mkv->body, QStringLiteral("Sample.Show.S01E03.1080p.x265-GROUP.mkv"));
        QVERIFY(mkv->bare);
        QVERIFY(mkv->fields.isEmpty());
        QCOMPARE(mkv->title, QStringLiteral("Sample.Show.S01E03.1080p.x265-GROUP"));
        const AsRunEntry* video = entryAt(log, 289);          // "!Video " with its trailing space
        QCOMPARE(video->body, QStringLiteral("!Video"));
        QCOMPARE(video->instruction, QStringLiteral("Video"));
        const AsRunEntry* miss = entryAt(log, 622);
        QVERIFY(miss->missed());
        QCOMPARE(miss->kind, AsRunKind::Media);
        QCOMPARE(miss->file, QStringLiteral("Sample.Show.S01E03.1080p.x265-GROUP.mpg"));
    }

    // A missed entry closes the airing before it by default; transparent, the last airing has nothing after it.
    void missedAsBoundaryOption() {
        AsRunOptions o;
        o.missedIsBoundary = false;
        const AsRunLog log = parseAsRun(readAll(fx("LogFile.log")), o);
        const AsRunEntry* last = entryAt(log, 621);
        QVERIFY(last->open);
        QCOMPARE(last->durationMs, qint64(-1));
        QCOMPARE(log.stats().value("open").toInt(), 1);
        QCOMPARE(log.stats().value("durationKnown").toInt(), 472);
    }

    // Status.log names each missed file at the same second: all 262 pair up, each status line once, in order.
    void missedReasonsFromStatus() {
        AsRunLog log = parseAsRun(readAll(fx("LogFile.log")));
        const MessageLog status = parseMessages(readAll(fx("Status.log")));
        QCOMPARE(attachReasons(log, status), 262);
        QCOMPARE(attachReasons(log, status), 262);            // idempotent
        QCOMPARE(log.stats().value("missedWithReason").toInt(), 262);
        QSet<int> used;
        int prev = 0;
        for (const AsRunEntry& e : log.entries) {
            if (!e.missed()) { QVERIFY(e.reason.isEmpty()); continue; }
            QCOMPARE(e.reason, QStringLiteral("File not found"));
            QVERIFY(e.reasonLine > prev);
            prev = e.reasonLine;
            used.insert(e.reasonLine);
            const MessageEntry* m = nullptr;
            for (const MessageEntry& x : status.entries) if (x.line == e.reasonLine) m = &x;
            QVERIFY(m);
            QCOMPARE(m->kind, MessageKind::FileNotFound);
            QCOMPARE(m->subject, e.file);
            QCOMPARE(m->stamp(), e.stamp());
        }
        QCOMPARE(used.size(), 262);
        // No status line for another file or outside the window: no reason.
        AsRunLog other = parseAsRun(crlf({"2025-06-21T02:34:54 118 other.mpg\t", "2025-06-21T02:44:54 118 Sample.Show.S01E03.1080p.x265-GROUP.mpg\t"}));
        QCOMPARE(attachReasons(other, status), 0);
        // A directory on either side still pairs by file name; the window is configurable.
        const MessageLog st = parseMessages(crlf({"2025-06-21T02:34:55 002 File not found: S:\\SPOTS\\Spot A.mpg"}));
        AsRunLog a = parseAsRun(crlf({"2025-06-21T02:34:54 118 C:\\prefetch\\spot a.MPG\t"}));
        QCOMPARE(attachReasons(a, st, 0), 0);
        QCOMPARE(attachReasons(a, st, 1), 1);
        QCOMPARE(a.entries.first().reasonLine, 1);
    }

    // ---- plant-shaped synthetic lines (plant_asrun.log) ----------------------------------------------------------------

    void plantLines_data() {
        QTest::addColumn<int>("line"); QTest::addColumn<int>("sec"); QTest::addColumn<int>("boundary");
        QTest::addColumn<bool>("truncated"); QTest::addColumn<bool>("open"); QTest::addColumn<bool>("eop");
        QTest::addColumn<bool>("prefetch"); QTest::addColumn<bool>("bare"); QTest::addColumn<QString>("type"); QTest::addColumn<QString>("title");
        QTest::newRow("prefetch, two commands inside") << 1 << 30 << 4 << false << false << false << true << false << "unknown" << "ACME TIRE 30";
        QTest::newRow("bare name, logo 20 s in") << 4 << 45 << 6 << false << false << false << false << true << "unknown" << "LOCAL HEADLINES INTRO 03";
        QTest::newRow("commercial folder") << 6 << 15 << 7 << false << false << false << false << false << "ad" << "CORNER CAFE 15";
        QTest::newRow("UNC news, End Of Play") << 7 << 132 << 9 << false << false << true << false << false << "news" << "TOP STORY 0807";
        QTest::newRow("same second") << 11 << 0 << 12 << false << false << false << true << false << "weather" << "WEATHER NOW 0807";
        QTest::newRow("code 110 transparent") << 12 << 10 << 14 << false << false << false << true << false << "bumper" << "STATION ID LEGAL";
        QTest::newRow("info fields, closed by a missed entry") << 14 << 30 << 15 << false << false << false << false << true << "unknown" << "The.Big.Game.S01E02";
        QTest::newRow("forward slashes, exactly the cap") << 17 << 3600 << 18 << false << false << false << true << false << "unknown" << "forward slash spot";
        QTest::newRow("one second over the cap") << 18 << -1 << 19 << true << false << false << true << false << "unknown" << "LONG SHOW PART 1";
        QTest::newRow("overnight stop") << 19 << -1 << 22 << true << false << false << true << false << "news" << "LATE NEWS TIP LINE";
        QTest::newRow("across midnight") << 22 << 45 << 23 << false << false << false << false << true << "bumper" << "MIDNIGHT BUMPER";
        QTest::newRow("clock went back") << 23 << -1 << 24 << true << false << false << true << false << "promo" << "PROMO MORNING SHOW";
        QTest::newRow("after the step back") << 24 << 90 << 26 << false << false << false << true << false << "unknown" << "BACKWARDS CLOCK";
        QTest::newRow("last airing, a command after it") << 26 << -1 << 0 << false << true << false << true << false << "unknown" << "LAST SPOT 30";
    }
    void plantLines() {
        QFETCH(int, line); QFETCH(int, sec); QFETCH(int, boundary); QFETCH(bool, truncated); QFETCH(bool, open); QFETCH(bool, eop);
        QFETCH(bool, prefetch); QFETCH(bool, bare); QFETCH(QString, type); QFETCH(QString, title);
        static const AsRunLog log = parseAsRun(readAll(fx("plant_asrun.log")));
        const AsRunEntry* e = entryAt(log, line);
        QVERIFY(e);
        QVERIFY(e->airing());
        QCOMPARE(e->durationMs, sec < 0 ? qint64(-1) : qint64(sec) * 1000);
        QCOMPARE(e->boundaryLine, boundary);
        QCOMPARE(e->truncated, truncated);
        QCOMPARE(e->open, open);
        QCOMPARE(e->closedByEndOfPlay, eop);
        QCOMPARE(e->prefetch, prefetch);
        QCOMPARE(e->bare, bare);
        QCOMPARE(e->type, type);
        QCOMPARE(e->title, title);
    }

    void plantSummary() {
        const QByteArray bytes = readAll(fx("plant_asrun.log"));
        QVERIFY2(bytes.size() == 1427 && bytes.count("\r\n") == 27 && bytes.endsWith("!OSD off\r\n"), "plant_asrun.log: 27 CRLF lines, 1427 bytes");
        const AsRunLog log = parseAsRun(bytes);
        QCOMPARE(log.physicalLines, 27);
        QCOMPARE(log.entries.size(), 25);
        QCOMPARE(log.unparsed, 1);
        QCOMPARE(log.blank, 1);
        QCOMPARE(diagList(log.diagnostics), QStringLiteral("20:MP001:1 24:MP004:1"));
        QCOMPARE(log.diagnostics.at(0).severity, QStringLiteral("warning"));
        QCOMPARE(log.diagnostics.at(1).severity, QStringLiteral("info"));
        const QJsonObject s = log.stats();
        QCOMPARE(s.value("played").toInt(), 22);
        QCOMPARE(s.value("missed").toInt(), 2);
        QCOMPARE(s.value("other").toInt(), 1);
        QCOMPARE(s.value("airings").toInt(), 14);
        QCOMPARE(s.value("commands").toInt(), 7);
        QCOMPARE(s.value("endOfPlay").toInt(), 1);
        QCOMPARE(s.value("durationKnown").toInt(), 10);
        QCOMPARE(s.value("truncated").toInt(), 3);
        QCOMPARE(s.value("open").toInt(), 1);
        QCOMPARE(s.value("playedSec").toDouble(), 3997.0);
        QCOMPARE(s.value("files").toInt(), 14);
        QCOMPARE(s.value("missedFiles").toInt(), 1);
        QCOMPARE(s.value("byInstruction").toObject(), QJsonObject({{"OSD off", 2}, {"ShowLogo", 2}, {"Repeat", 2}, {"ShowCrawl", 1}}));

        const AsRunEntry* osd = entryAt(log, 2);
        QCOMPARE(osd->instruction, QStringLiteral("OSD off"));
        QVERIFY(osd->knownInstruction);
        const AsRunEntry* logoTab = entryAt(log, 3);
        QCOMPARE(logoTab->instruction, QStringLiteral("ShowLogo"));
        QCOMPARE(logoTab->fields, (QStringList{"f=C:\\Logos\\STAR BUG.png", "Name=bug", "Layer=1"}));
        const AsRunEntry* logoSpace = entryAt(log, 5);
        QCOMPARE(logoSpace->instruction, QStringLiteral("ShowLogo"));
        QCOMPARE(logoSpace->body, QStringLiteral("!ShowLogo f=C:\\Logos\\STAR BUG.png"));
        QCOMPARE(logoSpace->arg, QString());                   // f= is a key of ShowLogo: a field, not a head
        QCOMPARE(entryAt(log, 8)->instruction, QStringLiteral("Repeat"));   // !Wiederholen
        QCOMPARE(entryAt(log, 25)->arg, QStringLiteral("Weather alert for Henry County"));
        QCOMPARE(entryAt(log, 9)->kind, AsRunKind::EndOfPlay);
        const AsRunEntry* other = entryAt(log, 13);
        QCOMPARE(other->code, 110);
        QVERIFY(!other->airing() && !other->missed());
        QCOMPARE(other->durationMs, qint64(-1));
        QCOMPARE(entryAt(log, 14)->fields, (QStringList{"info1", "info2"}));
        const AsRunEntry* miss = entryAt(log, 15);
        QVERIFY(miss->missed() && miss->bare);
        QCOMPARE(miss->type, QStringLiteral("promo"));
        QCOMPARE(entryAt(log, 7)->path, QStringLiteral("\\\\NAS\\Media\\EVENING HOUR\\NEWS A\\TOP STORY 0807.mpg"));

        // Missed entries transparent: the airing before them runs to the next airing.
        AsRunOptions o;
        o.missedIsBoundary = false;
        const AsRunLog t = parseAsRun(bytes, o);
        QCOMPARE(entryAt(t, 14)->durationMs, qint64(31000));
        QCOMPARE(entryAt(t, 14)->boundaryLine, 17);
        QCOMPARE(entryAt(t, 12)->durationMs, qint64(10000));
    }

    // ---- derivation rules on small logs -----------------------------------------------------------------------------

    void commandsTransparent() {
        const AsRunLog log = parseAsRun(crlf({
            "2026-02-01T21:16:00 000 C:\\prefetch\\STORY ONE.mpg\t",
            "2026-02-01T21:16:20 000 !ShowLogo\tf=C:\\logo.png",
            "2026-02-01T21:16:30 000 !OSD off",
            "2026-02-01T21:16:45 000 STORY TWO.mpg",
            "2026-02-01T21:17:00 000 End Of Play",
            "2026-02-01T21:17:00 000 !Repeat",
        }));
        QCOMPARE(log.entries.at(0).durationMs, qint64(45000));      // not 20 s: the logo fired inside the story
        QCOMPARE(log.entries.at(0).boundaryLine, 4);
        QCOMPARE(log.entries.at(3).durationMs, qint64(15000));
        QVERIFY(log.entries.at(3).closedByEndOfPlay);
        for (int i : {1, 2, 4, 5}) QCOMPARE(log.entries.at(i).durationMs, qint64(-1));
        QCOMPARE(log.stats().value("open").toInt(), 0);              // End Of Play closed the last airing
        // "end of play" in any case is the marker, not a file.
        const AsRunLog lower = parseAsRun(crlf({"2026-02-01T21:16:00 000 A.mpg", "2026-02-01T21:16:09 000 end of play"}));
        QCOMPARE(lower.entries.at(1).kind, AsRunKind::EndOfPlay);
        QCOMPARE(lower.entries.at(0).durationMs, qint64(9000));
    }

    void capOption() {
        const QByteArray b = crlf({"2026-02-01T10:00:00 000 A.mpg\t", "2026-02-01T10:01:40 000 B.mpg\t", "2026-02-01T10:01:40 000 C.mpg\t",
                                   "2026-02-01T10:02:00 000 End Of Play"});
        QCOMPARE(parseAsRun(b).entries.at(0).durationMs, qint64(100000));
        AsRunOptions o;
        o.capSec = 60;
        const AsRunLog capped = parseAsRun(b, o);
        QVERIFY(capped.entries.at(0).truncated);
        QCOMPARE(capped.entries.at(0).durationMs, qint64(-1));
        QCOMPARE(capped.entries.at(2).durationMs, qint64(20000));
        QCOMPARE(capped.stats().value("capSec").toInt(), 60);
        o.capSec = 0;                                                // only zero gaps survive
        const AsRunLog zero = parseAsRun(b, o);
        QCOMPARE(zero.entries.at(1).durationMs, qint64(0));
        QVERIFY(zero.entries.at(2).truncated);
    }

    // Keys: stable across re-reads, unique even for identical lines in one second, the same from a tail read.
    void keys() {
        const QByteArray bytes = readAll(fx("LogFile.log"));
        const AsRunLog log = parseAsRun(bytes);
        static const QRegularExpression kShape(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d-\\d{3}-[0-9a-f]{10}-\\d+$"));
        QSet<QString> keys;
        for (const AsRunEntry& e : log.entries) { QVERIFY2(kShape.match(e.key).hasMatch(), qPrintable(e.key)); keys.insert(e.key); }
        QCOMPARE(keys.size(), 883);
        QVERIFY(entryAt(log, 622)->key.endsWith("-0"));
        QVERIFY(entryAt(log, 627)->key.endsWith("-5"));             // six identical lines at 02:34:54
        QCOMPARE(entryAt(log, 622)->key.chopped(2), entryAt(log, 627)->key.chopped(2));
        const AsRunLog again = parseAsRun(bytes);
        for (int i = 0; i < log.entries.size(); ++i) QCOMPARE(again.entries.at(i).key, log.entries.at(i).key);
        // The time zone changes startMs, never the key.
        AsRunOptions o;
        o.tz = QTimeZone(QByteArrayLiteral("UTC+05:00"));
        if (o.tz.isValid()) QCOMPARE(parseAsRun(bytes, o).entries.at(100).key, log.entries.at(100).key);
        // A tail starting where the second changes gives the same keys as the full read.
        const QList<QByteArray> lines = bytes.split('\n');
        int from = 400;
        while (lines.at(from).left(19) == lines.at(from - 1).left(19)) ++from;
        const AsRunLog tail = parseAsRun(lines.mid(from).join('\n'));
        QVERIFY(tail.entries.size() > 400);
        for (const AsRunEntry& e : tail.entries) QCOMPARE(e.key, entryAt(log, e.line + from)->key);
    }

    // ---- time zones -------------------------------------------------------------------------------------------------

    void timeZones() {
        const QTimeZone ny(QByteArrayLiteral("America/New_York"));
        if (!ny.isValid()) QSKIP("America/New_York is not available in this Qt build");
        AsRunOptions o;
        o.tz = ny;
        const AsRunLog summer = parseAsRun(crlf({"2025-06-19T18:21:16 000 A.mpg\t"}), o);
        QCOMPARE(summer.entries.at(0).startMs, utcMs("2025-06-19T22:21:16Z"));
        QVERIFY(!summer.naive);
        QCOMPARE(summer.tzId, QStringLiteral("America/New_York"));
        QCOMPARE(parseAsRun(crlf({"2025-06-19T18:21:16 000 A.mpg\t"})).entries.at(0).startMs, utcMs("2025-06-19T18:21:16Z"));   // naive

        // Spring forward (2025-03-09 02:00 EST -> 03:00 EDT): 20 s of air, not 3620.
        const QByteArray spring = crlf({"2025-03-09T01:59:50 000 A.mpg\t", "2025-03-09T03:00:10 000 B.mpg\t", "2025-03-09T03:00:40 000 End Of Play"});
        const AsRunLog sp = parseAsRun(spring, o);
        QCOMPARE(sp.entries.at(0).startMs, utcMs("2025-03-09T06:59:50Z"));
        QCOMPARE(sp.entries.at(1).startMs, utcMs("2025-03-09T07:00:10Z"));
        QCOMPARE(sp.entries.at(0).durationMs, qint64(20000));
        QCOMPARE(sp.entries.at(1).durationMs, qint64(30000));
        QVERIFY(!sp.entries.at(0).dstGap && !sp.entries.at(0).dstAmbiguous);
        QVERIFY(parseAsRun(spring).entries.at(0).truncated);         // naive: 3620 s > cap
        const AsRunLog gap = parseAsRun(crlf({"2025-03-09T02:30:00 000 C.mpg\t"}), o);
        QVERIFY(gap.entries.at(0).dstGap);
        QCOMPARE(gap.entries.at(0).startMs, utcMs("2025-03-09T07:30:00Z"));   // read with EST, the offset before the jump

        // Fall back (2025-11-02 02:00 EDT -> 01:00 EST): the repeated hour runs forward.
        const QByteArray fall = crlf({"2025-11-02T00:59:50 000 A.mpg\t", "2025-11-02T01:00:10 000 B.mpg\t", "2025-11-02T01:59:50 000 C.mpg\t",
                                      "2025-11-02T01:00:20 000 D.mpg\t", "2025-11-02T01:30:00 000 E.mpg\t", "2025-11-02T02:00:05 000 F.mpg\t",
                                      "2025-11-02T02:00:35 000 End Of Play"});
        const AsRunLog fb = parseAsRun(fall, o);
        const QList<const char*> utc = {"2025-11-02T04:59:50Z", "2025-11-02T05:00:10Z", "2025-11-02T05:59:50Z", "2025-11-02T06:00:20Z",
                                        "2025-11-02T06:30:00Z", "2025-11-02T07:00:05Z", "2025-11-02T07:00:35Z"};
        const QList<int> secs = {20, 3580, 30, 1780, 1805, 30};
        for (int i = 0; i < 7; ++i) QCOMPARE(fb.entries.at(i).startMs, utcMs(utc.at(i)));
        for (int i = 0; i < 6; ++i) QCOMPARE(fb.entries.at(i).durationMs, qint64(secs.at(i)) * 1000);
        for (int i = 0; i < 7; ++i) QCOMPARE(fb.entries.at(i).dstAmbiguous, i >= 1 && i <= 4);
        QVERIFY2(fb.diagnostics.isEmpty(), qPrintable(diagList(fb.diagnostics)));
        QVERIFY(fb.entries.at(1).toJson().value("dstAmbiguous").toBool());
        // Naive, the wall clock steps back an hour: MP004 and an unknown duration.
        const AsRunLog naive = parseAsRun(fall);
        QCOMPARE(diagList(naive.diagnostics), QStringLiteral("4:MP004:1"));
        QVERIFY(naive.entries.at(2).truncated);
    }

    // 74k lines, the size of a month of plant log, across a fall-back night: every duration is reconstructed with the
    // zone (the per-hour offset cache keeps it fast), the naive read loses exactly the one at the step back.
    void largeLogAcrossDst() {
        const QTimeZone ny(QByteArrayLiteral("America/New_York"));
        if (!ny.isValid()) QSKIP("America/New_York is not available in this Qt build");
        const int airings = 37000;
        const QDateTime start(QDate(2025, 10, 29), QTime(0, 0), QTimeZone::UTC);
        QByteArray b;
        b.reserve(airings * 2 * 60);
        auto wall = [&](qint64 sec) { return start.addSecs(sec).toTimeZone(ny).toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")).toLatin1(); };
        for (int i = 0; i < airings; ++i) {
            b += wall(qint64(i) * 20) + " 000 C:\\prefetch\\SPOT " + QByteArray::number(i % 97) + ".mpg\t\r\n";
            b += wall(qint64(i) * 20 + 5) + " 000 !ShowLogo\tf=C:\\logo.png\r\n";
        }
        AsRunOptions o;
        o.tz = ny;
        QElapsedTimer timer;
        timer.start();
        const AsRunLog log = parseAsRun(b, o);
        const qint64 zoned = timer.restart();
        const AsRunLog naive = parseAsRun(b);
        const qint64 plain = timer.elapsed();
        qInfo("74,000 lines: %lld ms with America/New_York, %lld ms naive", zoned, plain);
        QCOMPARE(log.entries.size(), airings * 2);
        QVERIFY2(log.diagnostics.isEmpty(), qPrintable(diagList(log.diagnostics.mid(0, 5))));
        const QJsonObject s = log.stats();
        QCOMPARE(s.value("durationKnown").toInt(), airings - 1);
        QCOMPARE(s.value("open").toInt(), 1);
        QCOMPARE(s.value("playedSec").toDouble(), double(airings - 1) * 20.0);
        for (int i = 1; i < log.entries.size(); ++i) QVERIFY(log.entries.at(i).startMs > log.entries.at(i - 1).startMs);
        QCOMPARE(naive.stats().value("durationKnown").toInt(), airings - 2);
        QCOMPARE(naive.stats().value("truncated").toInt(), 1);
        QCOMPARE(naive.diagnostics.size(), 1);
        QCOMPARE(naive.diagnostics.first().code, QStringLiteral("MP004"));
        QVERIFY2(zoned < 20000 && plain < 20000, "74k lines should parse in seconds");
    }

    // ---- stamps, encodings, damaged lines ---------------------------------------------------------------------------

    void stampForms() {
        LogText t = readLog(crlf({"2025-06-19T18:21:12.060 004 UpdateOutputRes in", "2025-06-19T18:21:12.6 004 x", "2025-06-19 18:21:13 001 space form",
                                  "2025-06-19T18:21:14 000", "2025-06-19T18:21:15\t009\tTAB separated"}));
        QCOMPARE(t.lines.size(), 5);
        QCOMPARE(t.lines.at(0).time, QTime(18, 21, 12, 60));
        QVERIFY(t.lines.at(0).hasMs);
        QCOMPARE(t.lines.at(0).stamp(), QStringLiteral("2025-06-19T18:21:12.060"));
        QCOMPARE(t.lines.at(0).code, 4);
        QCOMPARE(t.lines.at(0).text, QStringLiteral("UpdateOutputRes in"));
        QCOMPARE(t.lines.at(1).time.msec(), 600);
        QCOMPARE(t.lines.at(2).stamp(), QStringLiteral("2025-06-19T18:21:13"));
        QCOMPARE(t.lines.at(3).text, QString());
        QCOMPARE(t.lines.at(4).code, 9);
        QCOMPARE(t.lines.at(4).text, QStringLiteral("TAB separated"));
        QVERIFY(t.diagnostics.isEmpty());
        // A code with nothing after it is a log line, but no as-run entry.
        const AsRunLog empty = parseAsRun(crlf({"2025-06-19T18:21:14 000"}));
        QCOMPARE(empty.entries.size(), 0);
        QCOMPARE(diagList(empty.diagnostics), QStringLiteral("1:MP001:1"));

        // Legacy stamps (AsRunLogIsoDateTime=0): DMY by default, MDY by value, MP007 once, the order options.
        const QByteArray legacy = crlf({"19/06/2025 18:21:16 000 a.mpg", "06/19/2025 18:21:17 000 b.mpg", "05/06/2025 18:21:18 000 c.mpg",
                                        "07/08/2025 18:21:19 000 d.mpg", "6/19/2025 6:21:16 PM 000 e.mpg", "6/19/2025 12:05:00 AM 000 f.mpg"});
        t = readLog(legacy);
        QCOMPARE(t.lines.size(), 6);
        QCOMPARE(t.lines.at(0).date, QDate(2025, 6, 19));
        QVERIFY(t.lines.at(0).legacyStamp);
        QCOMPARE(t.lines.at(1).date, QDate(2025, 6, 19));
        QCOMPARE(t.lines.at(2).date, QDate(2025, 6, 5));
        QCOMPARE(t.lines.at(4).time, QTime(18, 21, 16));
        QCOMPARE(t.lines.at(5).time, QTime(0, 5));
        QCOMPARE(diagList(t.diagnostics), QStringLiteral("3:MP007:1"));
        ReadOptions mdy;
        mdy.legacyDateOrder = "mdy";
        t = readLog(legacy, mdy);
        QCOMPARE(t.lines.at(1).line, 3);
        QCOMPARE(t.lines.at(1).date, QDate(2025, 5, 6));
        QCOMPARE(t.unparsed, 1);                                     // 19/06 is no MM/DD date
        QCOMPARE(diagList(t.diagnostics), QStringLiteral("1:MP001:1"));
        ReadOptions dmy;
        dmy.legacyDateOrder = "dmy";
        t = readLog(legacy, dmy);                                    // the MM/DD-only dates fail, no MP007
        QCOMPARE(diagList(t.diagnostics), QStringLiteral("2:MP001:1 5:MP001:1 6:MP001:1"));
        QCOMPARE(t.lines.at(1).date, QDate(2025, 6, 5));

        // Damaged lines are MP001 and skipped; the rest still parse.
        t = readLog(crlf({"2025-13-01T00:00:00 000 bad month", "2025-06-19T25:00:00 000 bad hour", "2025-06-19T18:21:16 00 short code",
                          "2025-06-19T18:21:16 0000 long code", "2025-06-19T18:21:16000 glued", "2025-06-19T18:2", "  2025-06-19T18:21:16 000 indented",
                          "2025-06-19T18:21:16 000 ok"}));
        QCOMPARE(t.lines.size(), 1);
        QCOMPARE(t.unparsed, 7);
        QCOMPARE(diagList(t.diagnostics), QStringLiteral("1:MP001:1 2:MP001:1 3:MP001:1 4:MP001:1 5:MP001:1 6:MP001:1 7:MP001:1"));
        QVERIFY(t.diagnostics.first().message.contains(QStringLiteral("bad month")));
    }

    void encodings() {
        const QString line = QString::fromUtf8("2025-06-19T18:21:16 000 D:\\Caf\xC3\xA9 \xE2\x80\x93 spot.mpg\t");
        const QString want = QString::fromUtf8("Caf\xC3\xA9 \xE2\x80\x93 spot.mpg");
        // UTF-8 with a BOM, LF only, no final newline.
        AsRunLog l = parseAsRun(QByteArray("\xEF\xBB\xBF") + line.toUtf8() + "\n" + line.toUtf8());
        QCOMPARE(l.encoding, Encoding::Utf8Bom);
        QCOMPARE(l.entries.size(), 2);
        QCOMPARE(l.entries.at(0).file, want);
        QVERIFY(l.diagnostics.isEmpty());
        // UTF-16 LE / BE with their BOMs.
        for (bool le : {true, false}) {
            l = parseAsRun(utf16(line + QStringLiteral("\r\n") + line + QStringLiteral("\r\n"), le));
            QCOMPARE(l.encoding, le ? Encoding::Utf16LE : Encoding::Utf16BE);
            QCOMPARE(l.entries.size(), 2);
            QCOMPARE(l.entries.at(1).file, want);
            QCOMPARE(l.entries.at(1).line, 2);
            QVERIFY(l.diagnostics.isEmpty());
        }
        // cp1252 (an older MasterPlay writing ANSI): decoded, MP010 at the first non-UTF-8 line.
        l = parseAsRun(QByteArray("2025-06-19T18:21:15 000 plain.mpg\t\r\n2025-06-19T18:21:16 000 D:\\Caf\xE9 \x96 spot.mpg\t\r\n"));
        QCOMPARE(l.encoding, Encoding::Ansi1252);
        QCOMPARE(l.entries.at(1).file, want);
        QCOMPARE(diagList(l.diagnostics), QStringLiteral("2:MP010:0"));
        // Bad bytes after a UTF-8 BOM: the encoding stays, MP010 names the line.
        l = parseAsRun(QByteArray("\xEF\xBB\xBF" "2025-06-19T18:21:15 000 a.mpg\r\n2025-06-19T18:21:16 000 b\xFF.mpg\r\n"));
        QCOMPARE(l.encoding, Encoding::Utf8Bom);
        QCOMPARE(l.entries.size(), 2);
        QCOMPARE(diagList(l.diagnostics), QStringLiteral("2:MP010:0"));
        // Blank and whitespace-only lines, doubled CR, empty input.
        l = parseAsRun(QByteArray("\r\n \t\r\n2025-06-19T18:21:16 000 a.mpg\t\r\r\n\r\n"));
        QCOMPARE(l.physicalLines, 4);
        QCOMPARE(l.blank, 3);
        QCOMPARE(l.entries.size(), 1);
        QCOMPARE(l.entries.at(0).line, 3);
        QCOMPARE(l.entries.at(0).fields, QStringList{QString()});
        QCOMPARE(parseAsRun(QByteArray()).entries.size(), 0);
        QCOMPARE(parseAsRun(QByteArray()).physicalLines, 0);
        QCOMPARE(parseAsRun(QByteArray("\xEF\xBB\xBF")).physicalLines, 0);
    }

    // ---- Status.log / Operator.log ----------------------------------------------------------------------------------

    void statusLog() {
        const MessageLog log = parseMessages(readAll(fx("Status.log")));
        QCOMPARE(log.physicalLines, 286);
        QCOMPARE(log.entries.size(), 286);
        QCOMPARE(log.unparsed, 0);
        QVERIFY(log.diagnostics.isEmpty());
        const QJsonObject s = log.stats();
        QCOMPARE(s.value("byCode").toObject(), QJsonObject({{"001", 16}, {"002", 262}, {"003", 8}}));
        QCOMPARE(s.value("bySeverity").toObject(), QJsonObject({{"info", 16}, {"minor", 262}, {"major", 8}}));
        QCOMPARE(s.value("byKind").toObject(), QJsonObject({{"start", 3}, {"close", 3}, {"onAir", 3}, {"output", 9}, {"fileNotFound", 262},
                                                           {"playlistNotFound", 3}, {"exception", 3}}));
        const MessageEntry& first = log.entries.first();
        QCOMPARE(first.kind, MessageKind::Start);
        QCOMPARE(first.subject, QStringLiteral("6.2.0.80 PE2"));
        QCOMPARE(first.computer, QStringLiteral("TESTPC"));
        QCOMPARE(first.toJson().value("severity").toString(), QStringLiteral("info"));
        QCOMPARE(log.entries.at(3).kind, MessageKind::Output);   // "Failed to set device: -2147023728"
        QCOMPARE(log.entries.at(3).subject, QStringLiteral("error"));

        const QJsonObject r = log.report();
        const QJsonArray sessions = r.value("sessions").toArray();
        QCOMPARE(sessions.size(), 3);
        QCOMPARE(sessions.at(0).toObject().value("start").toString(), QStringLiteral("2025-06-19T18:21:12"));
        QCOMPARE(sessions.at(0).toObject().value("close").toString(), QStringLiteral("2025-06-19T19:02:32"));
        QCOMPARE(sessions.at(2).toObject().value("close").toString(), QStringLiteral("2025-06-21T02:35:27"));
        QCOMPARE(sessions.at(1).toObject().value("version").toString(), QStringLiteral("6.2.0.80 PE2"));
        const QJsonArray missing = r.value("filesNotFound").toArray();
        QCOMPARE(missing.size(), 1);
        QCOMPARE(missing.at(0).toObject(), QJsonObject({{"file", "Sample.Show.S01E03.1080p.x265-GROUP.mpg"}, {"count", 262},
                                                        {"first", "2025-06-21T02:34:54"}, {"last", "2025-06-21T02:35:27"}}));
        QCOMPARE(r.value("playlistsNotFoundTotal").toInt(), 3);
        QCOMPARE(r.value("playlistsNotFound").toArray().at(0).toObject().value("name").toString(), QString());
        QCOMPARE(r.value("exceptionsTotal").toInt(), 3);
        QCOMPARE(r.value("exceptions").toArray().at(0).toObject().value("text").toString(),
                 QStringLiteral("The parameter is incorrect in speech engine (TextToSpeech instruction)"));
        const QJsonArray onAir = r.value("onAir").toArray();
        QCOMPARE(onAir.size(), 3);
        QVERIFY(onAir.at(0).toObject().value("enabled").toBool());
        QVERIFY(!onAir.at(1).toObject().value("enabled").toBool());
        QCOMPARE(r.value("outputErrorsTotal").toInt(), 2);
        const QJsonObject capped = log.report(1);
        QCOMPARE(capped.value("exceptions").toArray().size(), 1);
        QCOMPARE(capped.value("exceptionsTotal").toInt(), 3);
    }

    void operatorLog() {
        const MessageLog log = parseMessages(readAll(fx("Operator.log")));
        QCOMPARE(log.entries.size(), 32);
        QVERIFY(log.diagnostics.isEmpty());
        const QJsonObject s = log.stats();
        QCOMPARE(s.value("byCode").toObject(), QJsonObject({{"009", 26}, {"010", 6}}));
        QCOMPARE(s.value("bySeverity").toObject(), QJsonObject({{"operator", 26}, {"app", 6}}));
        QCOMPARE(s.value("byKind").toObject(), QJsonObject({{"start", 3}, {"close", 3}, {"configSaved", 9}, {"playlistChanged", 4},
                                                           {"button", 11}, {"playlistChange", 2}}));
        const QJsonObject r = log.report();
        QCOMPARE(r.value("buttons").toObject(), QJsonObject({{"Next", 10}, {"Play", 1}}));
        QCOMPARE(r.value("configSaved").toInt(), 9);
        QCOMPARE(r.value("sessions").toArray().size(), 3);
        const QJsonArray changes = r.value("playlistChanges").toArray();
        QCOMPARE(changes.size(), 2);
        QCOMPARE(changes.at(0).toObject().value("from").toString(), QStringLiteral("D:\\Masterplay\\Playlist\\OSD-Instructions.in"));
        QCOMPARE(changes.at(0).toObject().value("to").toString(), QStringLiteral("D:\\Masterplay\\Playlist\\sample.in"));
        QCOMPARE(changes.at(1).toObject().value("to").toString(), QStringLiteral("D:\\Masterplay\\Playlist\\Saturday_0200.in"));

        // Cross-check with the as-run: each operator playlist change is an End Of Play at the same second, the
        // boundary that closes the airing on air at that moment.
        const AsRunLog asrun = parseAsRun(readAll(fx("LogFile.log")));
        for (const QJsonValue& v : changes) {
            const QString at = v.toObject().value("at").toString();
            bool found = false;
            for (const AsRunEntry& e : asrun.entries) if (e.kind == AsRunKind::EndOfPlay && e.stamp() == at) found = true;
            QVERIFY2(found, qPrintable("no End Of Play at " + at));
        }
    }

    void messageKinds_data() {
        QTest::addColumn<QString>("text"); QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("subject");
        QTest::newRow("start") << "Start of Mplay 6.2.0.80 PE2 Computer: MASTERPLAY-PC" << "start" << "6.2.0.80 PE2";
        QTest::newRow("start, no computer") << "Start of Mplay 6.1" << "start" << "6.1";
        QTest::newRow("close") << "Close of Mplay" << "close" << "";
        QTest::newRow("on air") << "OnAir enabled" << "onAir" << "enabled";
        QTest::newRow("output stop") << "Stop Output" << "output" << "stop";
        QTest::newRow("output failed") << "FAILED TO ENABLE OUTPUT, Verify ..." << "output" << "error";
        QTest::newRow("file not found, path") << "File not found: S:\\SPOTS\\A B.mpg" << "fileNotFound" << "S:\\SPOTS\\A B.mpg";
        QTest::newRow("playlist not found") << "Playlist not found: D:\\x.in" << "playlistNotFound" << "D:\\x.in";
        QTest::newRow("exception") << "Exception: Access violation" << "exception" << "Access violation";
        QTest::newRow("button") << "Next Button pushed" << "button" << "Next";
        QTest::newRow("button, two words") << "Skip Event Button pushed" << "button" << "Skip Event";
        QTest::newRow("config") << "Configuration saved" << "configSaved" << "";
        QTest::newRow("changed") << "Playlist changed" << "playlistChanged" << "";
        QTest::newRow("case") << "file NOT found: x.mpg" << "fileNotFound" << "x.mpg";
        QTest::newRow("debug") << "ReadPlayliste: 18:21:15 D:\\Masterplay\\Playlist\\OSD-Instructions.in" << "other" << "";
    }
    void messageKinds() {
        QFETCH(QString, text); QFETCH(QString, kind); QFETCH(QString, subject);
        const MessageLog log = parseMessages(("2025-06-19T18:21:12 001 " + text + "\r\n").toUtf8());
        QCOMPARE(log.entries.size(), 1);
        QCOMPARE(messageKindName(log.entries.first().kind), kind);
        QCOMPARE(log.entries.first().subject, subject);
    }

    void debugFileLines() {
        const MessageLog log = parseMessages(crlf({"2025-06-19T18:21:12.060 004 UpdateOutputRes in", "2025-06-19T18:21:12.986 001 Start of Mplay 6.2.0.80 PE2 Computer: X",
                                                   "2025-06-19T18:21:12.500 004 stamped before the line above", "2025-06-19T18:24:00 009 Configuration saved",
                                                   "2025-06-19T18:24:01 042 a code nobody documented"}));
        QCOMPARE(log.entries.size(), 5);
        QCOMPARE(log.entries.at(0).toJson().value("severity").toString(), QStringLiteral("debug"));
        QCOMPARE(log.entries.at(0).toJson().value("at").toString(), QStringLiteral("2025-06-19T18:21:12.060"));
        QCOMPARE(log.entries.at(1).computer, QStringLiteral("X"));
        QCOMPARE(diagList(log.diagnostics), QStringLiteral("3:MP004:1"));
        QCOMPARE(log.stats().value("bySeverity").toObject().value("unknown").toInt(), 1);
        QCOMPARE(severityName(42), QString());
        QCOMPARE(log.report().value("sessions").toArray().size(), 1);   // a start without a close is still a session
    }

    // ---- helpers ----------------------------------------------------------------------------------------------------

    void typeRules_data() {
        QTest::addColumn<QString>("path"); QTest::addColumn<QString>("type");
        QTest::newRow("commercials folder") << "S:\\COMMERCIALS 2025\\x.mpg" << "ad";
        QTest::newRow("ads word") << "D:\\LOCAL ADS\\x.mpg" << "ad";
        QTest::newRow("ad word") << "D:\\x ad 30.mpg" << "ad";
        QTest::newRow("ads folder") << "D:\\Local Ads\\x.mpg" << "ad";
        QTest::newRow("a plain folder name is not special") << "D:\\MOVIES\\x.mpg" << "unknown";
        QTest::newRow("headlines is not ad") << "D:\\HEADLINES.mpg" << "unknown";
        QTest::newRow("news folder") << "\\\\nas\\EVENING HOUR\\NEWS A\\x.mpg" << "news";
        QTest::newRow("top story") << "D:\\Top Story 1.mpg" << "news";
        QTest::newRow("crime report") << "D:\\CRIME REPORT.mpg" << "news";
        QTest::newRow("newsroom is not news") << "D:\\newsroom.mpg" << "unknown";
        QTest::newRow("news_a: _ is a word character") << "D:\\NEWS_A.mpg" << "unknown";
        QTest::newRow("promos") << "D:\\PROMOS\\x.mpg" << "promo";
        QTest::newRow("promotion (prefix)") << "D:\\Promotion.mpg" << "promo";
        QTest::newRow("bumpers") << "D:\\INTERNAL FILES\\BUMPERS\\x.mpg" << "bumper";
        QTest::newRow("legal id") << "D:\\LEGAL ID.mpg" << "bumper";
        QTest::newRow("video is not id") << "D:\\video.mpg" << "unknown";
        QTest::newRow("weather") << "D:\\WEATHER\\x.mpg" << "weather";
        QTest::newRow("road report") << "D:\\Road Report.mpg" << "weather";
        QTest::newRow("trailers") << "D:\\TRAILERS\\x.mpg" << "promo";
        QTest::newRow("first rule wins") << "D:\\COMMERCIAL NEWS\\x.mpg" << "ad";
        QTest::newRow("empty") << "" << "unknown";
    }
    void typeRules() {
        QFETCH(QString, path); QFETCH(QString, type);
        QCOMPARE(inferType(path), type);
    }

    void fileAndTitle_data() {
        QTest::addColumn<QString>("path"); QTest::addColumn<QString>("file"); QTest::addColumn<QString>("title");
        QTest::newRow("drive") << "C:\\prefetch\\A B.mpg" << "A B.mpg" << "A B";
        QTest::newRow("forward slashes") << "C:/a/b.MPG" << "b.MPG" << "b";
        QTest::newRow("bare") << "b" << "b" << "b";
        QTest::newRow("dotted") << "Sample.Show.S01E03.1080p.x265-GROUP.mpg" << "Sample.Show.S01E03.1080p.x265-GROUP.mpg" << "Sample.Show.S01E03.1080p.x265-GROUP";
        QTest::newRow("trailing separator") << "D:\\dir\\" << "dir" << "dir";
        QTest::newRow("UNC") << "\\\\server\\share\\x.y.z" << "x.y.z" << "x.y";
        QTest::newRow("dot only at start") << ".mpg" << ".mpg" << "";
        QTest::newRow("dot at end") << "name." << "name." << "name.";
    }
    void fileAndTitle() {
        QFETCH(QString, path); QFETCH(QString, file); QFETCH(QString, title);
        QCOMPARE(fileOf(path), file);
        QCOMPARE(titleOf(fileOf(path)), title);
    }

    void jsonShapes() {
        AsRunLog log = parseAsRun(readAll(fx("plant_asrun.log")));
        const QJsonObject j = log.toJson(3);
        QCOMPARE(j.value("encoding").toString(), QStringLiteral("utf8"));
        QCOMPARE(j.value("entries").toArray().size(), 3);
        QCOMPARE(log.toJson().value("entries").toArray().size(), 25);
        QCOMPARE(j.value("diagnostics").toArray().size(), 2);
        QCOMPARE(j.value("stats").toObject().value("airings").toInt(), 14);
        const QJsonObject a = entryAt(log, 1)->toJson();
        QCOMPARE(a.value("at").toString(), QStringLiteral("2026-08-07T16:55:00"));
        QCOMPARE(a.value("code").toString(), QStringLiteral("000"));
        QCOMPARE(a.value("kind").toString(), QStringLiteral("media"));
        QCOMPARE(a.value("file").toString(), QStringLiteral("ACME TIRE 30.mpg"));
        QCOMPARE(a.value("durationSec").toDouble(), 30.0);
        QCOMPARE(a.value("boundaryLine").toInt(), 4);
        QVERIFY(a.value("prefetch").toBool());
        QVERIFY(!a.contains("fields"));                              // only the trailing TAB
        QVERIFY(!a.contains("instruction"));
        QCOMPARE(entryAt(log, 14)->toJson().value("fields").toArray().size(), 2);
        const QJsonObject open = entryAt(log, 26)->toJson();
        QVERIFY(open.value("durationSec").isNull());
        QVERIFY(open.value("open").toBool());
        QVERIFY(entryAt(log, 18)->toJson().value("truncated").toBool());
        const QJsonObject cmd = entryAt(log, 25)->toJson();
        QCOMPARE(cmd.value("instruction").toString(), QStringLiteral("ShowCrawl"));
        QCOMPARE(cmd.value("arg").toString(), QStringLiteral("Weather alert for Henry County"));
        QVERIFY(cmd.value("known").toBool());
        QVERIFY(!cmd.contains("durationSec"));
        const MessageLog st = parseMessages(crlf({"2026-08-07T17:00:40 002 File not found: MISSING PROMO 10.mpg"}));
        QCOMPARE(attachReasons(log, st), 1);                         // two identical missed lines, one status line
        const QJsonObject miss = entryAt(log, 15)->toJson();
        QCOMPARE(miss.value("reason").toString(), QStringLiteral("File not found"));
        QCOMPARE(miss.value("reasonLine").toInt(), 1);
        QVERIFY(!entryAt(log, 16)->toJson().contains("reason"));
        QCOMPARE(asRunKindName(AsRunKind::EndOfPlay), QStringLiteral("endOfPlay"));
        const QJsonObject m = st.entries.first().toJson();
        QCOMPARE(m.value("kind").toString(), QStringLiteral("fileNotFound"));
        QCOMPARE(m.value("severity").toString(), QStringLiteral("minor"));
        QCOMPARE(m.value("code").toString(), QStringLiteral("002"));
    }

    // ---- MP-09: the as-run DB writer -------------------------------------------------------------------------------------

    void asRunRowsMapping() {
        const AsRunLog log = parseAsRun(readAll(fx("plant_asrun.log")));
        const QJsonObject st = log.stats();
        AsRunWriteOptions o;
        o.channelId = QStringLiteral("mp-plant");
        o.source = QStringLiteral("plant");
        AsRunWriteResult c;
        QList<sched::AsRunRow> rows = asRunRows(log, o, &c);
        QCOMPARE(c.played, st.value("airings").toInt());
        QCOMPARE(c.missed, st.value("missed").toInt());
        QCOMPARE(c.commands, 0);                                   // commands are off by default
        QCOMPARE(rows.size(), c.played + c.missed);
        QVERIFY(c.played > 0 && c.missed > 0);
        int truncated = 0, open = 0;
        for (const sched::AsRunRow& r : rows) {
            const AsRunEntry* e = entryAt(log, r.extra.value("mpLine").toInt());
            QVERIFY(e);
            QCOMPARE(r.channelId, QStringLiteral("mp-plant"));
            QCOMPARE(r.runlistItemId, QStringLiteral("mp:") + e->key);
            QCOMPARE(r.extra.value("mpKey").toString(), e->key);
            QCOMPARE(r.extra.value("mpSource").toString(), QStringLiteral("plant"));
            QCOMPARE(r.origin, QStringLiteral("masterplay"));
            QCOMPARE(r.plannedStartMs, e->startMs);
            QCOMPARE(r.actualStartMs, e->startMs);
            QCOMPARE(r.varianceMs, qint64(0));
            QVERIFY(r.assetId.isEmpty());                         // contract pacing untouched by default
            QCOMPARE(r.title, e->title);
            QCOMPARE(r.type, e->type);
            QVERIFY(r.extra.value("typeDerived").toBool());
            if (e->airing()) {
                QCOMPARE(r.eventType, QStringLiteral("START"));
                QCOMPARE(r.status, QStringLiteral("ON_TIME"));
                QCOMPARE(r.extra.value("mpCode").toString(), QStringLiteral("000"));
                QCOMPARE(r.extra.value("durationDerived").toBool(), e->durationMs >= 0);
                if (e->durationMs >= 0) {
                    QCOMPARE(r.actualDurationMs, e->durationMs);
                    QCOMPARE(r.actualEndMs, e->startMs + e->durationMs);
                    QCOMPARE(r.plannedEndMs, r.actualEndMs);
                } else {
                    QCOMPARE(r.actualEndMs, qint64(0));            // unknown end: never guessed
                    QCOMPARE(r.actualDurationMs, qint64(0));
                }
                truncated += r.extra.value("truncated").toBool();
                open += r.extra.value("open").toBool();
            } else {
                QVERIFY(e->missed());
                QCOMPARE(r.eventType, QStringLiteral("SKIP"));
                QCOMPARE(r.status, QStringLiteral("SKIPPED"));
                QCOMPARE(r.reason, QStringLiteral("MasterPlay missed entry"));
                QCOMPARE(r.extra.value("mpCode").toString(), QStringLiteral("118"));
            }
        }
        QCOMPARE(truncated, st.value("truncated").toInt());
        QCOMPARE(open, st.value("open").toInt());

        // Commands as CMD rows; missed entries left out.
        o.includeCommands = true;
        o.includeMissed = false;
        c = {};
        rows = asRunRows(log, o, &c);
        QCOMPARE(c.commands, st.value("commands").toInt());
        QCOMPARE(c.missed, 0);
        int cmd = 0;
        for (const sched::AsRunRow& r : rows) {
            if (r.eventType != QLatin1String("CMD")) continue;
            ++cmd;
            QVERIFY(r.title.startsWith(QLatin1Char('!')));
            QCOMPARE(r.type, QStringLiteral("command"));
            QVERIFY(r.extra.contains("mpInstruction"));
        }
        QCOMPARE(cmd, c.commands);

        // The library lookup: its type wins and its id goes to json.mpMediaId, or to assetId with countTowardContracts.
        o = {};
        o.channelId = QStringLiteral("mp-plant");
        const QString first = asRunRows(log, AsRunWriteOptions{QStringLiteral("x")}).first().title;
        o.resolve = [&](const AsRunEntry& e) { return e.title == first ? qMakePair(QStringLiteral("lib-1"), QStringLiteral("program")) : QPair<QString, QString>(); };
        rows = asRunRows(log, o);
        QCOMPARE(rows.first().assetId, QString());
        QCOMPARE(rows.first().extra.value("mpMediaId").toString(), QStringLiteral("lib-1"));
        QCOMPARE(rows.first().type, QStringLiteral("program"));
        QVERIFY(!rows.first().extra.value("typeDerived").toBool());
        o.countTowardContracts = true;
        rows = asRunRows(log, o);
        QCOMPARE(rows.first().assetId, QStringLiteral("lib-1"));
        QVERIFY(!rows.first().extra.contains("mpMediaId"));

        // The range: [from, to] on startMs.
        const qint64 mid = rows.at(rows.size() / 2).actualStartMs;
        o = {};
        o.channelId = QStringLiteral("mp-plant");
        o.fromMs = mid;
        c = {};
        const int after = int(asRunRows(log, o, &c).size());
        o.fromMs = 0;
        o.toMs = mid - 1;
        AsRunWriteResult c2;
        const int before = int(asRunRows(log, o, &c2).size());
        QCOMPARE(after + before, int(asRunRows(log, AsRunWriteOptions{QStringLiteral("x")}).size()));
        QCOMPARE(c.outOfRange, before);
        QCOMPARE(c2.outOfRange, after);
    }

    void asRunWriterDedup() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("asrun.db"));
        sched::ScheduleStore store;   // creates the tables; stays open beside the writer's own connection
        QVERIFY(store.open(db));
        AsRunOptions ro;
        ro.tz = QTimeZone("America/New_York");
        AsRunLog log = parseAsRun(readAll(fx("LogFile.log")), ro);
        QCOMPARE(attachReasons(log, parseMessages(readAll(fx("Status.log")))), 262);
        AsRunWriteOptions o;
        o.channelId = QStringLiteral("mp-plant");
        o.source = QStringLiteral("plant");
        constexpr qint64 kAll = std::numeric_limits<qint64>::max();

        // Dry run: counted, nothing written.
        AsRunWriteResult r = writeAsRun(db, log, o, true);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.played, 473);
        QCOMPARE(r.missed, 262);
        QCOMPARE(r.wouldInsert, 735);
        QCOMPARE(r.inserted, 0);
        QCOMPARE(r.duplicates, 0);
        QCOMPARE(r.sample.size(), 20);
        QCOMPARE(r.toJson().value("rows").toObject().value("played").toInt(), 473);
        QVERIFY(store.asRun(o.channelId, 0, kAll).isEmpty());

        // Write.
        r = writeAsRun(db, log, o, false);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.inserted, 735);
        QCOMPARE(r.duplicates, 0);
        QCOMPARE(r.transactions, 1);
        const QList<sched::AsRunRow> rows = store.asRun(o.channelId, 0, kAll, 100000);
        QCOMPARE(rows.size(), 735);
        QSet<QString> ids;
        int starts = 0, skips = 0;
        qint64 airedMs = 0;
        for (const sched::AsRunRow& x : rows) {
            QVERIFY(x.assetId.isEmpty());
            QVERIFY(x.runlistItemId.startsWith(QLatin1String("mp:")));
            QCOMPARE(x.origin, QStringLiteral("masterplay"));
            QCOMPARE(x.extra.value("mpSource").toString(), QStringLiteral("plant"));
            ids.insert(x.runlistItemId);
            if (x.eventType == QLatin1String("START")) { ++starts; airedMs += x.actualDurationMs; }
            if (x.eventType == QLatin1String("SKIP")) {
                ++skips;
                QCOMPARE(x.reason, QStringLiteral("File not found"));
            }
        }
        QCOMPARE(ids.size(), 735);
        QCOMPARE(starts, 473);
        QCOMPARE(skips, 262);
        QCOMPARE(airedMs, qint64(3894000));
        // The first airing, 2025-06-19T18:21:16 New York (EDT, UTC-4).
        QCOMPARE(rows.first().actualStartMs, utcMs("2025-06-19T22:21:16Z"));
        const QJsonObject sum = store.asRunSummary(o.channelId, 0, kAll);
        QCOMPARE(sum.value("events").toInt(), 473);
        QCOMPARE(sum.value("skipped").toInt(), 262);
        QCOMPARE(sum.value("onTimePct").toDouble(), 100.0);

        // Re-runs insert nothing.
        r = writeAsRun(db, log, o, false);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.inserted, 0);
        QCOMPARE(r.duplicates, 735);
        r = writeAsRun(db, log, o, true);
        QCOMPARE(r.wouldInsert, 0);
        QCOMPARE(r.duplicates, 735);
        // Commands added later: only the CMD rows are new.
        o.includeCommands = true;
        r = writeAsRun(db, log, o, false);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.inserted, 142);
        QCOMPARE(r.duplicates, 735);
        QCOMPARE(store.asRun(o.channelId, 0, kAll, 100000).size(), 877);

        // Small transactions; another channel is not a duplicate.
        AsRunWriteOptions b;
        b.channelId = QStringLiteral("mp-batches");
        b.batchRows = 100;
        r = writeAsRun(db, log, b, false);
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.inserted, 735);
        QCOMPARE(r.transactions, 8);

        // Contract pacing: an asset id only with countTowardContracts.
        AsRunWriteOptions p;
        p.channelId = QStringLiteral("mp-pacing");
        p.resolve = [](const AsRunEntry& e) { return qMakePair(QStringLiteral("lib-") + e.title, QString()); };
        const QString someId = QStringLiteral("lib-") + rows.first().title;
        r = writeAsRun(db, log, p, false);
        QCOMPARE(r.inserted, 735);
        QCOMPARE(store.airedCount(someId, 0, kAll), 0);           // ids went to json.mpMediaId only
        p.channelId = QStringLiteral("mp-pacing2");
        p.countTowardContracts = true;
        r = writeAsRun(db, log, p, false);
        QCOMPARE(r.inserted, 735);
        int expect = 0;
        for (const sched::AsRunRow& x : rows) expect += x.eventType == QLatin1String("START") && x.title == rows.first().title;
        QVERIFY(expect > 0);
        QCOMPARE(store.airedCount(someId, 0, kAll), expect);

        // Errors: no channel, no file (and none created), a database without as_run.
        AsRunWriteOptions none;
        QVERIFY(!writeAsRun(db, log, none, false).ok());
        const QString missing = dir.filePath(QStringLiteral("missing.db"));
        r = writeAsRun(missing, log, o, false);
        QVERIFY(r.error.startsWith(QStringLiteral("no such database")));
        QVERIFY(!QFileInfo::exists(missing));
        const QString empty = dir.filePath(QStringLiteral("empty.db"));
        { QFile f(empty); QVERIFY(f.open(QIODevice::WriteOnly)); }
        r = writeAsRun(empty, log, o, false);
        QVERIFY2(r.error.startsWith(QStringLiteral("no as_run table")), qPrintable(r.error));
        QVERIFY(writeAsRun(QString(), log, o, true).ok());          // a dry run needs no DB
        QCOMPARE(writeAsRun(QString(), log, o, true).wouldInsert, 877);
        QVERIFY(!writeAsRun(QString(), log, o, false).ok());
    }

    void asRunWriterConcurrentStore() {
        // 20,000 airings in 2,000-row transactions while a ScheduleStore on the same file (its own thread and connection)
        // saves slots and as-run rows: neither side fails.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString db = dir.filePath(QStringLiteral("concurrent.db"));
        { sched::ScheduleStore s; QVERIFY(s.open(db)); }
        QByteArray text;
        const QDateTime t0(QDate(2025, 7, 1), QTime(0, 0), QTimeZone::UTC);
        for (int i = 0; i < 20000; ++i)
            text += t0.addSecs(i * 3).toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")).toLatin1() + " 000 D:\\Media\\clip" + QByteArray::number(i % 97) + ".mpg\t\r\n";
        const AsRunLog log = parseAsRun(text);
        QCOMPARE(log.stats().value("airings").toInt(), 20000);

        std::atomic<bool> stop{false}, started{false};
        std::atomic<int> ok{0}, failed{0};
        QThread* worker = QThread::create([&]() {
            sched::ScheduleStore st;
            if (!st.open(db)) { ++failed; started = true; return; }
            for (int i = 0; !stop || i < 20; ++i) {
                sched::HourSlot s;
                s.channelId = QStringLiteral("main");
                s.dayOfWeek = i % 7;
                s.hour = i % 24;
                sched::SlotItem it;
                it.id = QStringLiteral("w%1").arg(i);
                it.mediaId = QStringLiteral("m");
                it.durationSec = 30;
                s.items << it;
                (st.saveSlot(s) ? ok : failed)++;
                sched::AsRunRow r;
                r.channelId = QStringLiteral("main");
                r.runlistItemId = QStringLiteral("w:%1").arg(i);
                r.eventType = QStringLiteral("START");
                r.actualStartMs = r.plannedStartMs = 1000 + i;
                (st.insertAsRun(r) > 0 ? ok : failed)++;
                started = true;
                QThread::msleep(1);
            }
            st.close();
        });
        worker->start();
        QElapsedTimer w;
        w.start();
        while (!started && w.elapsed() < 5000) QThread::msleep(5);
        AsRunWriteOptions o;
        o.channelId = QStringLiteral("mp-load");
        QElapsedTimer t;
        t.start();
        const AsRunWriteResult r = writeAsRun(db, log, o, false);
        const qint64 ms = t.elapsed();
        const int during = ok;
        stop = true;
        QVERIFY(worker->wait(30000));
        delete worker;
        QVERIFY2(r.ok(), qPrintable(r.error));
        QCOMPARE(r.inserted, 20000);
        QCOMPARE(r.transactions, 10);
        QCOMPARE(failed.load(), 0);
        QVERIFY(during > 0);
        qInfo("bulk insert 20000 rows in %lld ms; %d store writes meanwhile, %d in all, none failed", ms, during, ok.load());
        sched::ScheduleStore check;
        QVERIFY(check.open(db));
        QCOMPARE(check.asRun(o.channelId, 0, std::numeric_limits<qint64>::max(), 100000).size(), 20000);
        QCOMPARE(check.asRun(QStringLiteral("main"), 0, std::numeric_limits<qint64>::max(), 100000).size(), ok.load() / 2);
    }

    // Opt-in: every *.log in PLAYPLAY_MP_SAMPLES (scratch copies of plant logs, never committed) reads without a damaged
    // line other than a cut-off last one, and an as-run log yields airings.
    void plantCorpus() {
        const QString dir = qEnvironmentVariable("PLAYPLAY_MP_SAMPLES");
        if (dir.isEmpty()) QSKIP("PLAYPLAY_MP_SAMPLES not set");
        const QStringList logs = QDir(dir).entryList({"*.log"}, QDir::Files);
        if (logs.isEmpty()) QSKIP("no *.log in PLAYPLAY_MP_SAMPLES");
        for (const QString& name : logs) {
            const QByteArray b = readAll(QDir(dir).filePath(name));
            if (name.contains(QStringLiteral("LogFile"), Qt::CaseInsensitive)) {
                const AsRunLog log = parseAsRun(b);
                qInfo("%s: %s", qPrintable(name), QJsonDocument(log.stats()).toJson(QJsonDocument::Compact).constData());
                QVERIFY2(log.unparsed <= 1, qPrintable(name + ": " + diagList(log.diagnostics.mid(0, 5))));
                QVERIFY(log.stats().value("airings").toInt() > 0);
            } else {
                const MessageLog log = parseMessages(b);
                qInfo("%s: %s", qPrintable(name), QJsonDocument(log.stats()).toJson(QJsonDocument::Compact).constData());
                QVERIFY2(log.unparsed <= 1, qPrintable(name + ": " + diagList(log.diagnostics.mid(0, 5))));
            }
        }
    }
};

QTEST_GUILESS_MAIN(TestMpAsRun)
#include "test_mpasrun.moc"
