// MasterPlay export tests (the lane design notes §7; slice MP-10): playout/masterplay/mpexporter.h.
// Round trips: export(import(fixture)) writes every event of import_features.in (the whole weekly grid: a loop with
// unrolled copies and a looping hour, folder picks, OSD spans, acts, a break pod, unsupported lines) and of the dated
// Monday of import_times.in with the original lines, in the original order, under their original stamps; the legacy
// date style and the ANSI encoding parse back to the same document; an every-day event laid on all seven days folds
// back into one 0000-00-00 event. Edits render canonically (a replaced file, a pick removed, rawLines off, the reversed
// path rules). Hand-made slots: headers at hard starts, continuation hours, a looping hour with its !Repeat, folder
// blocks merged into ToSelect=N, brackets, acts through segmentIndex, live lines, query blocks (MP030, or the picks
// given), cues as !ShowLogo / !ShowCrawl / !OSD off, notes as !Info, cg items left out (MP031). The store helpers:
// template slots, dated slots with an override, segmentIndexFrom. Options JSON.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QTemporaryDir>
#include <QTimeZone>
#include "../../playout/masterplay/mpexporter.h"
#include "../../playout/masterplay/mpimporter.h"
#include "../../playout/masterplay/mpresolver.h"
#include "../../playout/masterplay/mptimeline.h"
#include "../../playout/schedule/schedulestore.h"

using namespace mp;
using sched::HourSlot;
using sched::MediaRecord;
using sched::SlotItem;

namespace {

QString S(const char* s) { return QString::fromUtf8(s); }
QString fx(const QString& name) { return QDir(QStringLiteral(MP_FIXTURES)).filePath(name); }
QByteArray readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

const qint64 kDay = 86400000;
const qint64 kImportNow = QDateTime(QDate(2025, 9, 1), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();   // a Monday

MediaRecord rec(const char* id, const QString& path, const QString& title, const char* type, double dur, qint64 added = kImportNow - 30 * kDay) {
    MediaRecord m;
    m.id = QString::fromLatin1(id);
    m.path = path;
    m.title = title;
    m.type = QString::fromLatin1(type);
    m.durationSec = dur;
    m.addedAtMs = added;
    return m;
}
/// The library of test_mpimport: import_times.in and import_features.in name these files.
QList<MediaRecord> featureLibrary() {
    QList<MediaRecord> l{
        rec("movie", S(R"(D:\Media\Prog\Movie.mpg)"), S("Movie"), "program", 5400),
        rec("news", S(R"(D:\Media\Prog\News Hour.mpg)"), S("News Hour"), "news", 2700),
        rec("morning", S(R"(D:\Media\Prog\Morning Show.mpg)"), S("Morning Show"), "program", 1800),
        rec("doc", S(R"(D:\Media\Prog\Doc.mpg)"), S("Doc"), "program", 1500),
        rec("talk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), "program", 600),
        rec("weather", S(R"(D:\Media\Prog\Weather.mpg)"), S("Weather"), "weather", 300),
        rec("spotA", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), "ad", 30),
        rec("spotB", S(R"(D:\Media\Spots\Spot B.mpg)"), S("Spot B"), "ad", 60),
        rec("card", S(R"(D:\Media\Stills\Card.png)"), S("Card"), "image", 10),
        rec("promo1", S(R"(D:\Media\Promos\Promo One.mpg)"), S("Promo One"), "promo", 20, kImportNow - kDay),
        rec("promo2", S(R"(D:\Media\Promos\Promo Two.mpg)"), S("Promo Two"), "promo", 40, kImportNow - 10 * kDay),
        rec("spring", S(R"(D:\Media\Promos\Spring Spot.mpg)"), S("Spring Spot"), "promo", 30, kImportNow - kDay),
        rec("promoOld", S(R"(D:\Media\Promos\Old\Promo Old.mpg)"), S("Promo Old"), "promo", 60, kImportNow - 100 * kDay),
        rec("show", S(R"(D:\Media\Prog\Show Open.mpg)"), S("Show Open"), "program", 1500),
    };
    MediaRecord seg = rec("segshow", S(R"(D:\Media\Prog\Seg Show.mpg)"), S("Seg Show"), "program", 1800);
    seg.segments = QJsonArray{QJsonObject{{"id", "seg-a"}, {"label", "Act 1"}, {"startTime", 0}, {"endTime", 600}},
                              QJsonObject{{"id", "seg-b"}, {"label", "Act 2"}, {"startTime", 600}, {"endTime", 1200}},
                              QJsonObject{{"id", "seg-c"}, {"label", "Act 3"}, {"startTime", 1200}, {"endTime", 1800}}};
    l << seg;
    return l;
}
res::Options importResOptions() {   // no file-system access: the library is all there is
    res::Options o;
    o.useHash = false;
    o.probeFiles = false;
    o.nowMs = kImportNow;
    return o;
}
imp::Options gridOptions() {
    imp::Options o;
    o.channelId = S("mptest");
    o.mode = S("grid");
    o.timeZone = S("UTC");
    o.nowMs = kImportNow;
    return o;
}
imp::Options dayOptions(const QDate& day) {
    imp::Options o = gridOptions();
    o.mode = S("dates");
    o.from = o.to = day;
    return o;
}
tl::ReadFn featureReader() { return tl::memoryReader({{S(R"(D:\Crawl\today.txt)"), QByteArray("Storm warning\r\nuntil 9 pm\r\n")}}); }

imp::Plan planOf(const QString& fixture, const imp::Options& o) {
    const Document doc = parse(readAll(fx(fixture)));
    const res::Resolver rv(featureLibrary(), importResOptions());
    return imp::Importer(nullptr, &rv, featureReader()).plan(doc, fixture, o);
}

/// Event key -> its item lines (raw text). byTime: "HH:mm:ss" (dated exports of weekly events), else the ISO stamp.
QMap<QString, QStringList> eventLines(const Document& d, bool byTime) {
    QMap<QString, QStringList> m;
    for (const Event& ev : d.events) {
        const Entry& h = d.header(ev);
        const QString key = byTime ? h.stamp.time.toString(QStringLiteral("HH:mm:ss")) : formatStamp(h.stamp, true);
        QStringList ls;
        for (int i : ev.items) ls << d.entries.at(i).raw;
        m[key] = ls;
    }
    return m;
}
QStringList headerLines(const Document& d) {
    QStringList o;
    for (const Event& ev : d.events) o << d.header(ev).raw;
    return o;
}
QStringList allLines(const QByteArray& bytes) {
    QStringList o;
    for (const Entry& e : parse(bytes).entries) o << e.raw;
    return o;
}
QString codes(const QList<ex::Note>& ns) {
    QStringList o;
    for (const ex::Note& n : ns) o << QStringLiteral("%1:%2").arg(n.diag.line).arg(n.diag.code);
    return o.join(' ');
}
SlotItem media(const char* id, const QString& path, const QString& title, double dur, const char* type = "program") {
    SlotItem it;
    it.id = QString::fromLatin1(id);
    it.path = path;
    it.title = title;
    it.durationSec = dur;
    it.type = QString::fromLatin1(type);
    return it;
}
QJsonObject bugCue() {
    return QJsonObject{{"id", "cue-bug"}, {"filePath", S(R"(D:\Media\Logos\bug.png)")}, {"role", "bug"}, {"startOffset", 0}};
}

} // namespace

class TestMpExport : public QObject {
    Q_OBJECT

private slots:
    void roundTripFeaturesGrid() {
        // The whole fixture as a weekly grid: every event comes back with its lines, under its stamp, in its order.
        const imp::Plan p = planOf(S("import_features.in"), gridOptions());
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        QCOMPARE(p.hourSlots.size(), 168);
        ex::Options o;
        o.channelId = S("mptest");
        const ex::Result r = ex::Exporter(o).run(p.hourSlots);
        QVERIFY2(r.ok(), qPrintable(r.errors.join(S("; "))));
        QCOMPARE(r.filename, S("mptest-grid.in"));
        QVERIFY(r.bytes.startsWith("\xEF\xBB\xBF"));
        const Document out = parse(r.bytes);
        const Document in = parse(readAll(fx("import_features.in")));
        for (const Diagnostic& d : out.diagnostics) QVERIFY2(d.code != u"MP001" && d.code != u"MP002", qPrintable(d.message));
        const QMap<QString, QStringList> want = eventLines(in, false), got = eventLines(out, false);
        QCOMPARE(got.keys(), want.keys());   // 12 events, the same stamps
        for (auto it = want.cbegin(); it != want.cend(); ++it) QCOMPARE(got.value(it.key()), it.value());
        // Headers: the editor's field order; the loop's PlayMode and its EventDuration up to the next event; the
        // break-mode event keeps PlayMode=4; EventType 0 is omitted.
        const QStringList hs = headerLines(out);
        QCOMPARE(hs.first(), S("0000-00-02T06:00:00\tName=Loop Block\tEventDuration=02:20:00\tPlayMode=2"));
        QCOMPARE(hs.at(1), S("0000-00-02T08:20:00\tEventDuration=00:45:00\tPlayMode=1"));
        QVERIFY2(hs.contains(S("0000-00-06T06:00:00\tName=Network Breaks\tEventDuration=00:02:00\tPlayMode=4")), qPrintable(hs.join('\n')));
        QVERIFY2(hs.contains(S("0000-00-04T06:00:00\tName=Graphics Block\tEventDuration=00:48:00\tPlayMode=1")), qPrintable(hs.join('\n')));
        // Every written item line came from its provenance; the loop copies and the later picks wrote nothing.
        QCOMPARE(r.stats.value("events").toInt(), 12);
        QCOMPARE(r.stats.value("canonical").toInt(), 0);
        QCOMPARE(r.stats.value("new").toInt(), 0);
        int copies = 0;   // the loop's unrolled copies (extra.masterplay.copy > 0) wrote nothing
        for (const HourSlot& hs : p.hourSlots)
            for (const SlotItem& it : hs.items)
                if (it.extra.value("masterplay").toObject().value("copy").toInt() > 0) ++copies;
        QVERIFY(copies > 30);
        QCOMPARE(r.stats.value("skipped").toObject().value("copies").toInt(), copies);
        QCOMPARE(r.stats.value("skipped").toObject().value("picks").toInt(), 2);   // ToSelect=2 and SelectRandom=2
        QCOMPARE(r.stats.value("raw").toInt(), r.stats.value("items").toInt());
        QCOMPARE(codes(r.diagnostics), QString());
        QCOMPARE(r.doc.events.size(), out.events.size());
        QCOMPARE(r.text.toUtf8(), r.bytes.mid(3));
        // Byte-exact for the format too: the export of the export is the same file.
        QCOMPARE(serialize(out), r.bytes);
    }

    void roundTripTimesDated() {
        // The dated Monday of import_times.in: exact and floating events, a programme crossing hours, a missing file
        // and an !Info line in the provenance, a still with a forced length, Name= on a media line.
        const imp::Plan p = planOf(S("import_times.in"), dayOptions(QDate(2025, 9, 8)));
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        ex::Options o;
        o.channelId = S("mptest");
        o.mode = S("dates");
        const ex::Result r = ex::Exporter(o).run(p.hourSlots);
        QVERIFY2(r.ok(), qPrintable(r.errors.join(S("; "))));
        QCOMPARE(r.filename, S("mptest-2025-09-08.in"));
        const Document out = parse(r.bytes);
        const Document in = parse(readAll(fx("import_times.in")));
        QMap<QString, QStringList> want = eventLines(in, true);
        want.remove(S("23:30:00"));   // Saturday's Late Movie is not on a Monday
        // The 13:20 event cuts the Afternoon Movie at 13:20, so the spot after it (13:30) never airs: the importer drops it
        // (cutByEvent) and keeps no provenance line for it on any slot, so the export cannot carry it (impl-log, Left).
        want[S("12:00:00")].removeLast();
        QCOMPARE(eventLines(out, true), want);
        for (const Event& ev : out.events) {
            QCOMPARE(out.header(ev).stamp.kind, DayKind::Dated);
            QCOMPARE(out.header(ev).stamp.date, QDate(2025, 9, 8));
        }
        const QStringList hs = headerLines(out);
        QCOMPARE(hs.first(), S("2025-09-08T00:00:00\tEventType=2\tName=Overnight\tEventDuration=01:40:30\tPlayMode=1"));
        QVERIFY2(hs.contains(S("2025-09-08T10:07:00\tEventType=1\tName=Midday\tEventDuration=00:56:30\tPlayMode=1")), qPrintable(hs.join('\n')));
        QCOMPARE(codes(r.diagnostics), QString());

        // The legacy date style and ANSI: the same document, a DMY stamp, no BOM, CRLF; LF when asked.
        ex::Options legacy = o;
        legacy.dateStyle = StampForm::Legacy;
        legacy.encoding = Encoding::Ansi1252;
        const ex::Result l = ex::Exporter(legacy).run(p.hourSlots);
        QVERIFY(l.ok());
        QVERIFY(!l.bytes.startsWith("\xEF\xBB\xBF"));
        QVERIFY(l.bytes.startsWith("08/09/2025 00:00:00\t"));
        QVERIFY(l.bytes.endsWith("\r\n"));
        QVERIFY(!l.bytes.contains('\n') || l.bytes.count("\r\n") == l.bytes.count('\n'));
        ParseOptions po;
        po.legacyDateOrder = S("dmy");
        const Document ld = parse(l.bytes, po);
        QCOMPARE(l.doc.encoding, Encoding::Ansi1252);   // (the bytes are pure ASCII, so parse() cannot tell cp1252 from UTF-8)
        QVERIFY(ld.encoding == Encoding::Ansi1252 || ld.encoding == Encoding::Utf8);
        QCOMPARE(semanticDiff(out, ld), QString());
        QCOMPARE(codes(l.diagnostics), QString());
        ex::Options lf = o;
        lf.eol = "\n";
        lf.finalNewline = false;
        const ex::Result f = ex::Exporter(lf).run(p.hourSlots);
        QVERIFY(!f.bytes.contains("\r\n"));
        QVERIFY(!f.bytes.endsWith('\n'));
        QCOMPARE(semanticDiff(out, parse(f.bytes)), QString());
    }

    void everyDayFold() {
        // The every-day Overnight event is laid on all seven days (Sunday late through Saturday's Late Movie) and comes
        // out identical: one 0000-00-00 event, before the weekly ones; foldEveryDay off writes it seven times.
        const imp::Plan p = planOf(S("import_times.in"), gridOptions());
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        ex::Options o;
        o.channelId = S("mptest");
        const ex::Result r = ex::Exporter(o).run(p.hourSlots);
        QVERIFY2(r.ok(), qPrintable(r.errors.join(S("; "))));
        const Document out = parse(r.bytes);
        const QStringList hs = headerLines(out);
        QCOMPARE(hs.first(), S("0000-00-00T00:00:00\tEventType=2\tName=Overnight\tEventDuration=01:40:30\tPlayMode=1"));
        int everyDay = 0, midnight = 0;
        for (const Event& ev : out.events) {
            if (out.header(ev).stamp.kind == DayKind::EveryDay) ++everyDay;
            if (out.header(ev).stamp.time == QTime(0, 0)) ++midnight;
        }
        QCOMPARE(everyDay, 1);
        QCOMPARE(midnight, 1);
        QCOMPARE(r.stats.value("folded").toInt(), 6);
        QCOMPARE(out.events.size(), 9);   // Overnight + 7 Monday events + Saturday's Late Movie
        QCOMPARE(eventLines(out, false).value(S("0000-00-07T23:30:00")), (QStringList{S(R"(D:\Media\Prog\Movie.mpg)")}));
        // The original lines of every event, as in the fixture (but the spot the 13:20 event cuts off: see roundTripTimesDated).
        QMap<QString, QStringList> want = eventLines(parse(readAll(fx("import_times.in"))), false);
        want[S("0000-00-02T12:00:00")].removeLast();
        QCOMPARE(eventLines(out, false), want);
        // The output is in MasterPlay's order and parses back to the same file.
        QCOMPARE(serialize(out), r.bytes);
        ex::Options noFold = o;
        noFold.foldEveryDay = false;
        const Document seven = parse(ex::Exporter(noFold).run(p.hourSlots).bytes);
        everyDay = 0;
        for (const Event& ev : seven.events) if (seven.header(ev).stamp.kind == DayKind::EveryDay) ++everyDay;
        QCOMPARE(everyDay, 0);
        QCOMPARE(seven.events.size(), 15);
        QCOMPARE(headerLines(seven).first(), S("0000-00-01T00:00:00\tEventType=2\tName=Overnight\tEventDuration=01:40:30\tPlayMode=1"));
    }

    void editsRenderCanonically() {
        // Wednesday's Graphics Block: a replaced file, a title, a removed pick; rawLines off; the path rules reversed.
        const imp::Plan p = planOf(S("import_features.in"), dayOptions(QDate(2025, 9, 10)));
        QVERIFY2(p.ok(), qPrintable(p.errors.join(S("; "))));
        QList<HourSlot> wed = p.hourSlots;   // not "slots": a Qt keyword macro
        const int si = p.indexOf(S("2025-09-10-6"));
        QVERIFY(si >= 0);
        HourSlot& s = wed[si];
        QCOMPARE(s.items.size(), 8);
        QVERIFY(ex::Exporter::untouched(s.items[1]));
        s.items[1].path = S(R"(D:\Media\Prog\Doc.mpg)");    // Weather -> Doc: the line no longer describes it
        s.items[1].mediaId = S("doc");
        s.items[1].title = S("Documentary");
        QVERIFY(!ex::Exporter::untouched(s.items[1]));
        s.items[3].title = S("Spot B");                      // the same title: still untouched
        QVERIFY(ex::Exporter::untouched(s.items[3]));
        s.items[4].outPointSec = 120;                        // a new forced length
        QVERIFY(!ex::Exporter::untouched(s.items[4]));
        ex::Options o;
        o.channelId = S("mptest");
        o.mode = S("dates");
        ini::PathRule rule;
        rule.from = S(R"(P:\)");
        rule.to = S(R"(D:\)");
        o.pathMap = {rule};
        const ex::Result r = ex::Exporter(o).run(wed);
        QVERIFY2(r.ok(), qPrintable(r.errors.join(S("; "))));
        const Document out = parse(r.bytes);
        const QStringList lines = eventLines(out, true).value(S("06:00:00"));
        QCOMPARE(lines, (QStringList{
            S(R"(!ShowLogo	f=D:\Media\Logos\bug.png	cfg=D:\Masterplay\OSD\DefaultLogo.osd	Layer=1)"),
            S(R"(D:\Media\Prog\Talk.mpg)"),
            S(R"(P:\Media\Prog\Doc.mpg	Name=Documentary)"),           // canonical: the reversed rule, Name= (not the base name)
            S(R"(!ShowCrawl Breaking: Main St closed<br/>Use Route 58	cfg=D:\Masterplay\OSD\DefaultCrawl.osd)"),
            S(R"(D:\Media\Spots\Spot A.mpg)"),
            S(R"(!OSD off)"),
            S(R"(D:\Media\Spots\Spot B.mpg)"),
            S(R"(!ShowLogo	f=D:\Media\Logos\live.png	Layer=2)"),
            S(R"(P:\Media\Prog\Doc.mpg[00:02:00;00])"),                // canonical: the new forced length
            S(R"(!OSD off)"),
            S(R"(!ShowClock t=0	cfg=D:\Masterplay\OSD\DefaultRealtimeClock.osd)"),
            S(R"(D:\Media\Prog\Weather.mpg)"),
            S(R"(!ShowCrawlFromFile D:\Crawl\today.txt)"),
            S(R"(D:\Media\Spots\Spot A.mpg)"),
            S(R"(!ShowTextFromFile D:\Subs\missing.srt)"),
            S(R"(!OSD off)"),
            S(R"(D:\Media\Spots\Spot B.mpg)")}));
        QCOMPARE(r.stats.value("canonical").toInt(), 2);
        QCOMPARE(r.stats.value("raw").toInt(), 7);   // 6 of this event + the 07:00 Talk
        QCOMPARE(codes(r.diagnostics), QString());

        // rawLines off: every media line from its fields (the rules reversed), the provenance instructions still raw.
        ex::Options canon = o;
        canon.rawLines = false;
        const ex::Result c = ex::Exporter(canon).run(p.hourSlots);
        const QStringList cl = eventLines(parse(c.bytes), true).value(S("06:00:00"));
        QCOMPARE(cl.at(1), S(R"(P:\Media\Prog\Talk.mpg)"));
        QCOMPARE(cl.at(2), S(R"(P:\Media\Prog\Weather.mpg)"));
        QCOMPARE(cl.at(0), lines.at(0));
        QCOMPARE(cl.size(), lines.size());
        QCOMPARE(c.stats.value("raw").toInt(), 0);
        QCOMPARE(c.stats.value("canonical").toInt(), 9);

        // Tuesday's folders: the ToSelect=2 block with one pick removed renders with ToSelect=1; the others stay raw.
        const imp::Plan t = planOf(S("import_features.in"), dayOptions(QDate(2025, 9, 9)));
        QVERIFY(t.ok());
        const int ti = t.indexOf(S("2025-09-09-6"));
        auto pickAt = [&](const HourSlot& tue, int pick) {   // the item of pick N of the "LoadingMode=2 ToSelect=2" line
            for (int i = 0; i < tue.items.size(); ++i) {
                const QJsonObject mp = tue.items[i].extra.value("masterplay").toObject();
                if (mp.value("raw").toString().endsWith(S("LoadingMode=2\tToSelect=2")) && mp.value("folder").toObject().value("pick").toInt() == pick) return i;
            }
            return -1;
        };
        const QStringList orig = eventLines(parse(readAll(fx("import_features.in"))), false).value(S("0000-00-03T06:00:00"));
        o.pathMap.clear();   // a canonical folder line would go through the reversed rule (P:\) like any rendered path
        QList<HourSlot> ts = t.hourSlots;
        int at = pickAt(ts[ti], 1);
        QVERIFY(at > 0);
        ts[ti].items.removeAt(at);
        const ex::Result tr = ex::Exporter(o).run(ts);
        QVERIFY(tr.ok());
        const QStringList tl = eventLines(parse(tr.bytes), true).value(S("06:00:00"));
        QCOMPARE(tl.size(), orig.size());
        QCOMPARE(tl.at(4), S(R"(!InsertDirectory D:\Media\Promos	LoadingMode=2	ToSelect=1)"));
        for (int i = 0; i < orig.size(); ++i) if (i != 4) QCOMPARE(tl.at(i), orig.at(i));
        QCOMPARE(tr.stats.value("canonical").toInt(), 1);
        QCOMPARE(codes(tr.diagnostics), QString());
        // The FIRST pick removed: the surviving pick is still a line (ToSelect=1), with an MP031, not lost.
        QList<HourSlot> t0 = t.hourSlots;
        at = pickAt(t0[ti], 0);
        QVERIFY(at > 0);
        t0[ti].items.removeAt(at);
        const ex::Result r0 = ex::Exporter(o).run(t0);
        QVERIFY(r0.ok());
        const QStringList l0 = eventLines(parse(r0.bytes), true).value(S("06:00:00"));
        QCOMPARE(l0.size(), orig.size());
        QCOMPARE(l0.at(4), S(R"(!InsertDirectory D:\Media\Promos	LoadingMode=2	ToSelect=1)"));
        for (int i = 0; i < orig.size(); ++i) if (i != 4) QCOMPARE(l0.at(i), orig.at(i));
        QCOMPARE(r0.stats.value("canonical").toInt(), 1);
        QCOMPARE(r0.stats.value("skipped").toObject().value("picks").toInt(), 1);   // SelectRandom=2's second pick only
        QCOMPARE(r0.diagnostics.size(), 1);
        QCOMPARE(r0.diagnostics.first().diag.code, S("MP031"));
        QCOMPARE(r0.diagnostics.first().diag.line, 6);   // header + 4 lines before it
        QVERIFY2(r0.diagnostics.first().diag.message.contains(S("first 1 pick")), qPrintable(r0.diagnostics.first().diag.message));
        // A spot put between the two picks: two ToSelect=1 lines around it (both canonical), nothing lost.
        QList<HourSlot> tb = t.hourSlots;
        at = pickAt(tb[ti], 1);
        tb[ti].items.insert(at, media("mid", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), 30, "ad"));
        const ex::Result rb = ex::Exporter(o).run(tb);
        QVERIFY(rb.ok());
        const QStringList lb = eventLines(parse(rb.bytes), true).value(S("06:00:00"));
        QCOMPARE(lb.size(), orig.size() + 2);
        QCOMPARE(lb.at(4), S(R"(!InsertDirectory D:\Media\Promos	LoadingMode=2	ToSelect=1)"));
        QCOMPARE(lb.at(5), S(R"(D:\Media\Spots\Spot A.mpg)"));
        QCOMPARE(lb.at(6), lb.at(4));
        for (int i = 5; i < orig.size(); ++i) QCOMPARE(lb.at(i + 2), orig.at(i));
        QCOMPARE(rb.stats.value("canonical").toInt(), 2);
        QCOMPARE(rb.stats.value("new").toInt(), 1);
        QCOMPARE(rb.stats.value("skipped").toObject().value("picks").toInt(), 1);
    }

    void handMadeSlots() {
        QList<HourSlot> hand;   // not "slots": a Qt keyword macro
        HourSlot a;
        a.id = S("1-6");
        a.dayOfWeek = 1;
        a.hour = 6;
        a.showBlock = QJsonObject{{"title", "Morning"}};
        SlotItem talk = media("talk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), 600);
        talk.timingMode = S("hard_start");
        talk.fixedOffsetSec = 0;
        talk.cgOverlays.append(bugCue());
        talk.note = S("Open");
        SlotItem spotA = media("spotA", S(R"(D:\Comm\Spot A.mpg)"), S("Spot A"), 30, "ad");
        spotA.cgOverlays.append(bugCue());
        SlotItem folder;
        folder.id = S("f1");
        folder.title = S("Promos");
        folder.durationSec = 30;
        folder.folderBlock = QJsonObject{{"folder", S(R"(D:\Media\Promos)")}, {"order", "random"}, {"targetSec", 0}, {"recursive", false}};
        SlotItem folder2 = folder;
        folder2.id = S("f2");
        SlotItem card = media("card", S(R"(D:\Media\Stills\Card.png)"), S("Card"), 10, "image");
        card.outPointSec = 20;
        SlotItem seg = media("seg", S(R"(D:\Media\Prog\Seg Show.mpg)"), S("Seg Show"), 600);
        seg.mediaId = S("segshow");
        seg.segmentId = S("seg-b");
        SlotItem ndi = media("ndi", S("ndi://CAM1 (Studio)"), S("Studio"), 300, "live");
        SlotItem srt = media("srt", S("srt://10.0.0.5:9000"), S("Remote"), 300, "live");
        srt.liveSource = QJsonObject{{"url", "srt://10.0.0.5:9000"}};
        SlotItem query;
        query.id = S("q");
        query.title = S("Promo fill");
        query.durationSec = 120;
        query.queryBlock = QJsonObject{{"types", QJsonArray{"promo"}}, {"targetSec", 120}};
        SlotItem cg = media("cg", QString(), S("Lower third"), 10, "cg");
        SlotItem doc = media("doc", S(R"(D:\Media\Prog\Doc.mpg)"), S("Documentary"), 1500);
        doc.timingMode = S("hard_start");
        doc.fixedOffsetSec = 1800;
        doc.extra["conflictPolicy"] = S("finish_previous");
        doc.cgOnStart << S("clock");
        doc.cgOnEnd << S("clock");
        a.items << talk << spotA << folder << folder2 << card << seg << ndi << srt << query << cg << doc;
        HourSlot b;   // the continuation hour: its item joins the 06:30 event
        b.id = S("1-7");
        b.dayOfWeek = 1;
        b.hour = 7;
        b.mergedEvent = QJsonObject{{"isContinuation", true}, {"rootSlotId", "1-6"}, {"carrySec", 300}};
        b.items << media("weather", S(R"(D:\Media\Prog\Weather.mpg)"), S("Weather"), 300);
        HourSlot c;   // a looping hour
        c.id = S("2-6");
        c.dayOfWeek = 2;
        c.hour = 6;
        c.isLooping = true;
        c.items << media("sa", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), 30, "ad") << media("sb", S(R"(D:\Media\Spots\Spot B.mpg)"), S("Spot B"), 60, "ad");
        HourSlot d;   // an in point, a ticker cue, a show hook, a hide hook
        d.id = S("3-6");
        d.dayOfWeek = 3;
        d.hour = 6;
        SlotItem late = media("late", S(R"(D:\Media\Prog\Doc.mpg)"), S("Doc"), 1500);
        late.inPointSec = 100;
        SlotItem ticker = media("tk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), 600);
        ticker.cgOverlays.append(QJsonObject{{"id", "cue-tk"}, {"template", "ticker"}, {"params", QJsonObject{{"text", "Hello\nWorld"}}}});
        ticker.cgOnStart << S(R"({"action":"show","role":"secondary","source":{"type":"image","path":"D:\\Media\\Logos\\live.png"}})") << S("legal_id");
        ticker.cgOnEnd << S(R"({"action":"hide","role":"secondary"})");
        d.items << late << ticker;
        hand << c << b << a << d;   // any order

        ex::Options o;
        o.channelId = S("hand");
        ini::PathRule rule;
        rule.from = S(R"(S:\COMMERCIALS\)");
        rule.to = S(R"(D:\Comm\)");
        o.pathMap = {rule};
        o.segmentIndex = [](const QString& mediaId, const QString& segmentId) { return mediaId == u"segshow" && segmentId == u"seg-b" ? 1 : -1; };
        const ex::Result r = ex::Exporter(o).run(hand);
        QVERIFY2(r.ok(), qPrintable(r.errors.join(S("; "))));
        QCOMPARE(allLines(r.bytes), (QStringList{
            S("0000-00-02T06:00:00\tName=Morning\tEventDuration=00:33:50\tPlayMode=1"),
            S(R"(!ShowLogo	f=D:\Media\Logos\bug.png	Layer=1)"),
            S("!Info\tName=Open"),
            S(R"(D:\Media\Prog\Talk.mpg)"),
            S(R"(S:\COMMERCIALS\Spot A.mpg)"),
            S("!OSD off"),
            S(R"(!InsertDirectory D:\Media\Promos	LoadingMode=3	ToSelect=2)"),
            S(R"(D:\Media\Stills\Card.png[00:00:20;00])"),
            S(R"(D:\Media\Prog\Seg Show.mpg	actinout=1)"),
            S("!Video ndi://CAM1 (Studio)"),
            S("!LiveStream\tPath=srt://10.0.0.5:9000"),
            S("!Info\tName=query block Promo fill not exported"),
            S("0000-00-02T06:30:00\tEventType=1\tEventDuration=00:30:00\tPlayMode=1"),
            S("!ShowClock\tt=0"),
            S(R"(D:\Media\Prog\Doc.mpg	Name=Documentary)"),
            S("!OSD off"),
            S(R"(D:\Media\Prog\Weather.mpg)"),
            S("0000-00-03T06:00:00\tEventDuration=00:01:30\tPlayMode=2"),
            S("!Repeat"),
            S(R"(D:\Media\Spots\Spot A.mpg)"),
            S(R"(D:\Media\Spots\Spot B.mpg)"),
            S("0000-00-04T06:00:00\tEventDuration=00:33:20\tPlayMode=1"),
            S(R"(D:\Media\Prog\Doc.mpg)"),
            S("!ShowCrawl Hello<br/>World"),
            S(R"(!ShowLogo	f=D:\Media\Logos\live.png	Layer=2)"),
            S(R"(D:\Media\Prog\Talk.mpg)"),
            S("!OSD off")}));
        // Diagnostics name the output line, the slot and the item.
        QCOMPARE(codes(r.diagnostics), S("12:MP030 12:MP031 23:MP031 26:MP031"));
        QCOMPARE(r.diagnostics.at(0).slotId, S("1-6"));
        QCOMPARE(r.diagnostics.at(0).itemId, S("q"));
        QCOMPARE(r.diagnostics.at(0).diag.severity, S("warning"));
        QVERIFY(r.diagnostics.at(1).diag.message.contains(S("cg item")));     // the cg item: left out
        QVERIFY(r.diagnostics.at(2).diag.message.contains(S("in point")));
        QVERIFY(r.diagnostics.at(3).diag.message.contains(S("legal_id")));
        QCOMPARE(r.diagnostics.at(3).diag.severity, S("info"));
        QCOMPARE(r.stats.value("events").toInt(), 4);
        QCOMPARE(r.stats.value("new").toInt(), 15);
        QCOMPARE(r.stats.value("skipped").toObject().value("picks").toInt(), 1);
        QCOMPARE(r.stats.value("overlays").toObject().value("shows").toInt(), 4);
        QCOMPARE(r.stats.value("overlays").toObject().value("offs").toInt(), 3);
        QCOMPARE(r.stats.value("queryBlocks").toInt(), 1);
        QCOMPARE(r.stats.value("diagnostics").toObject().value("warning").toInt(), 1);
        QCOMPARE(r.stats.value("diagnostics").toObject().value("info").toInt(), 3);
        const QJsonObject j = r.toJson(true);
        QCOMPARE(j.value("ok").toBool(), true);
        QCOMPARE(j.value("diagnostics").toArray().size(), 4);
        QCOMPARE(j.value("diagnostics").toArray().first().toObject().value("itemId").toString(), S("q"));
        QCOMPARE(j.value("text").toString(), r.text);
        // Query picks given: media lines in the block's place.
        ex::Options withPicks = o;
        withPicks.queryPicks = [](const HourSlot&, const SlotItem& it) {
            return it.id == u"q" ? QList<SlotItem>{media("p1", S(R"(D:\Media\Promos\Promo One.mpg)"), S("Promo One"), 20),
                                                  media("p2", S(R"(D:\Media\Promos\Spring Spot.mpg)"), S("Spring Spot"), 30)}
                                 : QList<SlotItem>();
        };
        const ex::Result q = ex::Exporter(withPicks).run(hand);
        const QStringList ql = allLines(q.bytes);
        QCOMPARE(ql.at(11), S(R"(D:\Media\Promos\Promo One.mpg)"));
        QCOMPARE(ql.at(12), S(R"(D:\Media\Promos\Spring Spot.mpg)"));
        QVERIFY(!codes(q.diagnostics).contains(S("MP030")));
        QCOMPARE(ql.first(), S("0000-00-02T06:00:00\tName=Morning\tEventDuration=00:32:40\tPlayMode=1"));   // the picks' 50 s, not the block's 120
        // mergeFolderPicks off: two lines with ToSelect=1.
        ex::Options noMerge = o;
        noMerge.mergeFolderPicks = false;
        const QStringList nl = allLines(ex::Exporter(noMerge).run(hand).bytes);
        QCOMPARE(nl.at(6), S(R"(!InsertDirectory D:\Media\Promos	LoadingMode=3	ToSelect=1)"));
        QCOMPARE(nl.at(7), nl.at(6));
        // A folder block with a length, a mask, SubDirs and MaxAge.
        HourSlot f;
        f.id = S("5-6");
        f.dayOfWeek = 5;
        f.hour = 6;
        SlotItem whole;
        whole.id = S("w");
        whole.durationSec = 900;
        whole.folderBlock = QJsonObject{{"folder", S(R"(D:\Media\Promos)")}, {"order", "name"}, {"targetSec", 900}, {"recursive", true}, {"maxAgeDays", 2},
                                        {"rules", QJsonArray{QJsonObject{{"field", "filename"}, {"op", "starts_with"}, {"value", "Promo"}},
                                                             QJsonObject{{"field", "filename"}, {"op", "ends_with"}, {"value", ".mpg"}}}}};
        SlotItem shuffled = whole;
        shuffled.id = S("s");
        shuffled.folderBlock = QJsonObject{{"folder", S(R"(D:\Media\Promos\)")}, {"order", "random"}, {"targetSec", 900}, {"weight", "uniform"}};
        f.items << whole << shuffled;
        const QStringList fl = allLines(ex::Exporter(o).run({f}).bytes);
        QCOMPARE(fl.at(1), S(R"(!InsertDirectory D:\Media\Promos\Promo*.mpg	SubDirs=1	MaxAge=48)"));
        QCOMPARE(fl.at(2), S(R"(!InsertDirectory D:\Media\Promos\	LoadingMode=1	SubDirs=1)"));
        // An in point with an out point: the bracket keeps the planned length (out - in), the note says what plays.
        HourSlot h = f;
        h.items.clear();
        SlotItem clip = media("clip", S(R"(D:\Media\Prog\Doc.mpg)"), S("Doc"), 1500);
        clip.inPointSec = 100;
        clip.outPointSec = 700;
        h.items << clip;
        const ex::Result hr = ex::Exporter(o).run({h});
        QCOMPARE(allLines(hr.bytes), (QStringList{S("0000-00-06T06:00:00\tEventDuration=00:10:00\tPlayMode=1"), S(R"(D:\Media\Prog\Doc.mpg[00:10:00;00])")}));
        QCOMPARE(codes(hr.diagnostics), S("2:MP031"));
        QVERIFY2(hr.diagnostics.first().diag.message.contains(S("first 600 s")), qPrintable(hr.diagnostics.first().diag.message));
        // The target encoding cannot hold the title: MP032 on that line, '?' written.
        HourSlot g = f;
        g.showBlock = QJsonObject{{"title", QString::fromUtf8("\xCE\xA9mega")}};
        ex::Options ansi = o;
        ansi.encoding = Encoding::Ansi1252;
        const ex::Result ga = ex::Exporter(ansi).run({g});
        QCOMPARE(codes(ga.diagnostics), S("1:MP032"));
        QVERIFY(ga.bytes.startsWith("0000-00-06T06:00:00\tName=?mega\t"));
        QCOMPARE(ga.stats.value("lossyLines").toInt(), 1);
        // Errors: a dated slot in grid mode, a template slot in dates mode, bad options.
        HourSlot dated;
        dated.id = S("2025-09-08-6");
        dated.date = QDate(2025, 9, 8);
        dated.hour = 6;
        QVERIFY(!ex::Exporter(o).run({dated}).ok());
        ex::Options dm = o;
        dm.mode = S("dates");
        QVERIFY(!ex::Exporter(dm).run({f}).ok());
        QVERIFY(ex::Exporter(dm).run({dated}).ok());
        ex::Options badFps = o;
        badFps.fps = 0;
        QVERIFY(!ex::Exporter(badFps).run({f}).ok());
        QCOMPARE(ex::Exporter(o).run({}).stats.value("events").toInt(), 0);
    }

    void storeSlots() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        sched::ScheduleStore st;
        QVERIFY(st.open(dir.filePath(S("export.db"))));
        sched::ChannelProfile cp;
        cp.id = S("mptest");
        cp.timezone = S("UTC");
        QVERIFY(st.saveChannel(cp));
        auto slot = [](const QString& id, int dow, const QDate& date, int hour, const SlotItem& it) {
            HourSlot s;
            s.id = id;
            s.channelId = S("mptest");
            s.dayOfWeek = dow;
            s.date = date;
            s.hour = hour;
            s.items << it;
            return s;
        };
        QVERIFY(st.saveSlot(slot(S("1-6"), 1, QDate(), 6, media("talk", S(R"(D:\Media\Prog\Talk.mpg)"), S("Talk"), 600))));
        QVERIFY(st.saveSlot(slot(S("2-6"), 2, QDate(), 6, media("weather", S(R"(D:\Media\Prog\Weather.mpg)"), S("Weather"), 300))));
        QVERIFY(st.saveSlot(slot(S("2025-09-08-6"), -1, QDate(2025, 9, 8), 6, media("spotA", S(R"(D:\Media\Spots\Spot A.mpg)"), S("Spot A"), 30, "ad"))));
        MediaRecord seg = rec("segshow", S(R"(D:\Media\Prog\Seg Show.mpg)"), S("Seg Show"), "program", 1800);
        seg.segments = QJsonArray{QJsonObject{{"id", "seg-a"}}, QJsonObject{{"id", "seg-b"}}};
        QVERIFY(st.saveMedia(seg));

        const QList<HourSlot> grid = ex::Exporter::gridSlots(st, S("mptest"));
        QCOMPARE(grid.size(), 2);
        ex::Options o;
        o.channelId = S("mptest");
        const ex::Result g = ex::Exporter(o).run(grid);
        QVERIFY(g.ok());
        QCOMPARE(g.filename, S("mptest-grid.in"));
        QCOMPARE(allLines(g.bytes), (QStringList{S("0000-00-02T06:00:00\tEventDuration=00:10:00\tPlayMode=1"), S(R"(D:\Media\Prog\Talk.mpg)"),
                                                S("0000-00-03T06:00:00\tEventDuration=00:05:00\tPlayMode=1"), S(R"(D:\Media\Prog\Weather.mpg)")}));

        const QList<HourSlot> days = ex::Exporter::dateSlots(st, S("mptest"), QDate(2025, 9, 8), QDate(2025, 9, 9));   // Monday, Tuesday
        QCOMPARE(days.size(), 48);
        for (const HourSlot& s : days) { QVERIFY(s.isDated()); QCOMPARE(s.id, HourSlot::dateId(s.date, s.hour)); }
        o.mode = S("dates");
        const ex::Result d = ex::Exporter(o).run(days);
        QVERIFY2(d.ok(), qPrintable(d.errors.join(S("; "))));
        QCOMPARE(d.filename, S("mptest-2025-09-08_2025-09-09.in"));
        QCOMPARE(allLines(d.bytes), (QStringList{S("2025-09-08T06:00:00\tEventDuration=00:00:30\tPlayMode=1"), S(R"(D:\Media\Spots\Spot A.mpg)"),   // the override wins
                                                S("2025-09-09T06:00:00\tEventDuration=00:05:00\tPlayMode=1"), S(R"(D:\Media\Prog\Weather.mpg)")}));
        QVERIFY(ex::Exporter::dateSlots(st, S("mptest"), QDate(2025, 9, 9), QDate(2025, 9, 8)).isEmpty());
        QVERIFY(ex::Exporter::dateSlots(st, S("mptest"), QDate(2025, 1, 1), QDate(2026, 6, 1)).isEmpty());

        const auto idx = ex::Exporter::segmentIndexFrom(&st);
        QCOMPARE(idx(S("segshow"), S("seg-b")), 1);
        QCOMPARE(idx(S("segshow"), S("seg-x")), -1);
        QCOMPARE(idx(S("nope"), S("seg-a")), -1);
        QCOMPARE(ex::Exporter::segmentIndexFrom(nullptr)(S("segshow"), S("seg-a")), -1);
    }

    void optionsJson() {
        QStringList errs;
        ex::Options o = ex::Options::fromJson(QJsonObject{{"channel", "ch2"}, {"mode", "dates"}, {"dateStyle", "legacy"}, {"encoding", "ansi"}, {"eol", "lf"},
                                                          {"finalNewline", false}, {"rawLines", false}, {"foldEveryDay", false}, {"mergeFolderPicks", false}, {"fps", 25},
                                                          {"pathMap", QJsonArray{QJsonObject{{"from", "S:\\"}, {"to", "D:\\"}}, QJsonObject{{"from", "X:\\"}}}}},
                                              &errs);
        QVERIFY2(errs.isEmpty(), qPrintable(errs.join(S("; "))));
        QCOMPARE(o.channelId, S("ch2"));
        QCOMPARE(o.mode, S("dates"));
        QCOMPARE(o.dateStyle, StampForm::Legacy);
        QCOMPARE(o.encoding, Encoding::Ansi1252);
        QCOMPARE(o.eol, QByteArray("\n"));
        QVERIFY(!o.finalNewline && !o.rawLines && !o.foldEveryDay && !o.mergeFolderPicks);
        QCOMPARE(o.fps, 25);
        QCOMPARE(o.pathMap.size(), 1);   // a rule without `to` is dropped
        QCOMPARE(o.check(), QStringList());
        const QJsonObject j = o.toJson();
        QCOMPARE(j.value("dateStyle").toString(), S("legacy"));
        QCOMPARE(j.value("encoding").toString(), S("cp1252"));
        QCOMPARE(j.value("eol").toString(), S("lf"));
        QCOMPARE(j.value("pathMap").toArray().size(), 1);
        const ex::Options back = ex::Options::fromJson(j);
        QCOMPARE(back.toJson(), j);
        ex::Options::fromJson(QJsonObject{{"mode", "week"}, {"dateStyle", "keep"}, {"encoding", "ebcdic"}, {"eol", "cr"}, {"fps", 500}, {"rawLines", "yes"}, {"pathMap", 1}}, &errs);
        QCOMPARE(errs.size(), 7);
        const ex::Options defaults = ex::Options::fromJson({});
        QCOMPARE(defaults.mode, S("grid"));
        QCOMPARE(defaults.dateStyle, StampForm::Iso);
        QCOMPARE(defaults.encoding, Encoding::Utf8Bom);
        QCOMPARE(defaults.eol, QByteArray("\r\n"));
        QVERIFY(defaults.rawLines && defaults.foldEveryDay && defaults.mergeFolderPicks && defaults.finalNewline);
        // The catalog knows the exporter's codes.
        QCOMPARE(diagnosticInfo(S("MP030")).stage, S("exporter"));
        QCOMPARE(diagnosticInfo(S("MP031")).severity, S("info"));
        QCOMPARE(diagnosticInfo(S("MP032")).severity, S("warning"));
    }
};

QTEST_GUILESS_MAIN(TestMpExport)
#include "test_mpexport.moc"
