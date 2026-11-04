// FlightPlan: time debt per interval, corrections ("best plan"), ghost-preview diff, protection, readiness, why.
#include <QtTest>
#include <QTemporaryFile>
#include "../playout/schedule/flightplan.h"

using namespace sched;

class TestFlightPlan : public QObject {
    Q_OBJECT
    QList<MediaRecord> m_pool;
    qint64 h0 = 0;
    MediaLookup lookup() const {
        const QList<MediaRecord> pool = m_pool; MediaLookup ml;
        ml.byId = [pool](const QString& id, bool* f) { for (const auto& m : pool) if (m.id == id) { if (f) *f = true; return m; } if (f) *f = false; return MediaRecord(); };
        ml.bySeries = [pool](const QString& s) { QList<MediaRecord> o; for (const auto& m : pool) if (m.series == s) o << m; return o; };
        ml.byTypes = [pool](const QStringList& t) { QList<MediaRecord> o; for (const auto& m : pool) if (t.contains(m.type)) o << m; return o; };
        ml.all = [pool]() { return pool; };
        return ml;
    }
    MediaRecord mk(const QString& id, const QString& type, double dur) { MediaRecord m; m.id = id; m.title = id; m.type = type; m.durationSec = dur; m.path = "D:/nope/" + id + ".mp4"; return m; }
    SlotItem item(const QString& id, const QString& type, const char* media = nullptr) { SlotItem s; s.id = id; s.type = type; s.mediaId = media ? QString(media) : id; return s; }
    FlightOptions opts(qint64 nowOffsetSec) {
        FlightOptions o; o.channelId = "main"; o.nowMs = h0 + nowOffsetSec * 1000; o.tz = QTimeZone("UTC"); o.media = lookup(); o.checkFiles = false; return o;
    }
    HourSlot overrunHour() {
        HourSlot s; s.channelId = "main"; s.id = "0-0";
        s.items << item("a", "program") << item("promo", "promo") << item("filler", "filler") << item("b", "program");
        SlotItem n = item("n", "program"); n.timingMode = "hard_start"; n.fixedOffsetSec = 1800; s.items << n;
        return s;
    }
    Runlist build(const HourSlot& s, bool fill = false) {
        BuildOptions bo; bo.fillGaps = fill; bo.nowMs = h0; return RunlistBuilder::build(s, h0, lookup(), bo);
    }
private slots:
    void initTestCase() {
        h0 = 1790000000000LL - (1790000000000LL % 3600000LL);
        m_pool = {mk("a", "program", 1200), mk("b", "program", 600), mk("n", "program", 300), mk("promo", "promo", 300), mk("filler", "filler", 120),
                  mk("f30", "filler", 30), mk("f90", "filler", 90), mk("f600", "filler", 600), mk("spot", "ad", 30)};
        m_pool[3].lastPlayedMs = 5; m_pool[5].lastPlayedMs = 1; m_pool[6].lastPlayedMs = 2; m_pool[7].lastPlayedMs = 3;
    }

    void timeDebtBeforeAnAnchor() {
        const Runlist rl = build(overrunHour());
        const QJsonObject fp = FlightPlan::analyze({rl}, opts(10));
        const QJsonArray iv = fp.value("intervals").toArray();
        QVERIFY(iv.size() >= 2);
        const QJsonObject first = iv[0].toObject();
        QCOMPARE(first.value("state").toString(), QString("over"));
        QVERIFY2(qAbs(first.value("debtSec").toDouble() - 420.0) < 1.5, qPrintable(QString::number(first.value("debtSec").toDouble())));   // 1190 + 300 + 120 + 600 - 1790 = 420
        QVERIFY(first.value("label").toString().contains("over before"));
        QCOMPARE(first.value("endsAtName").toString(), QString("n"));
        QCOMPARE(fp.value("anchors").toArray().size(), 1);
        QCOMPARE(fp.value("now").toObject().value("id").toString(), QString("a"));
        QCOMPARE(fp.value("next").toObject().value("id").toString(), QString("promo"));
        QVERIFY(fp.value("summary").toObject().value("overSec").toDouble() > 400.0);
        // the on-air event and the window after now are protected; the rest is editable
        QVERIFY(fp.value("now").toObject().value("protected").toBool());
        QVERIFY(!fp.value("next").toObject().value("protected").toBool());
    }

    void droppedEventsCountAsOverrun() {
        // 5 programs of 300 s before a hard start at 900 s: the last two do not fit and are dropped by the builder
        HourSlot s; s.channelId = "main"; s.id = "0-0";
        for (int i = 0; i < 5; ++i) { m_pool << mk(QString("d%1").arg(i), "program", 300); s.items << item(QString("d%1").arg(i), "program"); }
        SlotItem n = item("n", "program"); n.timingMode = "hard_start"; n.fixedOffsetSec = 900; s.items << n;
        const Runlist rl = build(s);
        QCOMPARE(rl.dropped.size(), 2);
        const QJsonObject first = FlightPlan::analyze({rl}, opts(0)).value("intervals").toArray()[0].toObject();
        QVERIFY2(qAbs(first.value("debtSec").toDouble() - 600.0) < 1.5, qPrintable(QString::number(first.value("debtSec").toDouble())));
        QVERIFY(first.value("label").toString().contains("2 events will not air"));
    }

    void correctionDrawerProposesTheCheapestCover() {
        const Runlist rl = build(overrunHour());
        const QJsonObject c = FlightPlan::corrections({rl}, opts(10), 0);
        QCOMPARE(c.value("kind").toString(), QString("over"));
        const QJsonObject best = c.value("best").toObject();
        QVERIFY(best.value("covers").toBool());
        QVERIFY2(best.value("overshootSec").toDouble() < 30.0, qPrintable(QString::number(best.value("overshootSec").toDouble())));
        QStringList ops; for (const QJsonValue& v : best.value("ops").toArray()) ops << v.toObject().value("op").toString() + ":" + v.toObject().value("itemId").toString();
        QVERIFY2(ops.contains("remove:promo") && ops.contains("remove:filler"), qPrintable(ops.join(",")));
        QVERIFY(!ops.join(",").contains(":a") && !ops.join(",").contains(":b"));          // programs are never proposed
        // the drawer lists the single options too, with their savings
        bool sawPromo = false; for (const QJsonValue& v : c.value("options").toArray()) if (v.toObject().value("itemId").toString() == "promo" && v.toObject().value("kind").toString() == "drop") sawPromo = true;
        QVERIFY(sawPromo);
    }

    void ghostPreviewShowsWhatMovesAndApplyFixesTheHour() {
        HourSlot s = overrunHour(); const Runlist before = build(s);
        const FlightOptions o = opts(10);
        const QJsonArray ops = FlightPlan::corrections({before}, o, 0).value("best").toObject().value("ops").toArray();
        QStringList log, err;
        QVERIFY(FlightPlan::applyOps(s, ops, before, o, &log, &err));
        QVERIFY2(err.isEmpty(), qPrintable(err.join("; ")));
        const Runlist after = build(s);
        const QJsonObject d = FlightPlan::diff({before}, {after});
        QCOMPARE(d.value("removed").toInt(), 2);
        QVERIFY(d.value("moved").toInt() >= 1);                                           // b now starts earlier and is not cut
        QVERIFY(d.value("summary").toString().contains("2 removed"));
        const QJsonObject fp = FlightPlan::analyze({after}, o);
        QVERIFY2(fp.value("intervals").toArray()[0].toObject().value("debtSec").toDouble() < 5.0, "debt must be gone");
        QVERIFY(!after.items[after.items.size() - 2].truncated);
    }

    void protectedAndMandatoryEventsAreRefused() {
        HourSlot s = overrunHour(); s.items.insert(1, item("spot", "ad"));
        const Runlist rl = build(s); const FlightOptions o = opts(10);
        QStringList log, err;
        QVERIFY(!FlightPlan::applyOps(s, QJsonArray{QJsonObject{{"op", "remove"}, {"itemId", "a"}}}, rl, o, &log, &err));          // on air
        QVERIFY(err.last().contains("protected"));
        err.clear();
        QVERIFY(!FlightPlan::applyOps(s, QJsonArray{QJsonObject{{"op", "remove"}, {"itemId", "spot"}}}, rl, o, &log, &err));       // a paid spot
        QVERIFY2(err.last().contains("mandatory"), qPrintable(err.join(";")));
        QCOMPARE(s.items.size(), 6);
        // and no correction ever lists a spot
        const QJsonObject c = FlightPlan::corrections({rl}, o, 0);
        for (const QJsonValue& v : c.value("options").toArray()) QVERIFY(v.toObject().value("itemId").toString() != "spot");
    }

    void gapOptionsAreEligibleClipsThatFit() {
        HourSlot s; s.channelId = "main"; s.id = "0-0";
        s.items << item("a", "program");
        SlotItem n = item("n", "program"); n.timingMode = "hard_start"; n.fixedOffsetSec = 1800; s.items << n;                      // 1200 s program + 300 s news: 600 s empty before the news
        const Runlist rl = build(s);
        const FlightOptions o = opts(10);
        const QJsonObject c = FlightPlan::corrections({rl}, o, 0);
        QCOMPARE(c.value("kind").toString(), QString("gap"));
        QVERIFY2(qAbs(c.value("gapSec").toDouble() - 600.0) < 2.0, qPrintable(QString::number(c.value("gapSec").toDouble())));
        QVERIFY(!c.value("options").toArray().isEmpty());
        for (const QJsonValue& v : c.value("options").toArray()) QVERIFY(v.toObject().value("savingSec").toDouble() <= 601.0);   // nothing longer than the gap
        QJsonObject top = c.value("best").toObject().value("ops").toArray()[0].toObject();
        QCOMPARE(top.value("op").toString(), QString("insert")); QCOMPARE(top.value("mediaId").toString(), QString("f600"));      // the clip that leaves the least empty
        QCOMPARE(top.value("beforeItemId").toString(), QString("n"));
        // applying it puts the clip in front of the news and closes the gap
        HourSlot edit = s; QStringList log, err;
        QVERIFY(FlightPlan::applyOps(edit, QJsonArray{top}, rl, o, &log, &err));
        QCOMPARE(edit.items[1].mediaId, QString("f600")); QCOMPARE(edit.items[2].id, QString("n"));
    }

    void readinessCatchesTheProblemsBeforeAir() {
        HourSlot s; s.channelId = "main"; s.id = "0-0"; s.items << item("a", "program") << item("b", "program") << item("c", "program");
        m_pool << mk("rej", "program", 100) << mk("exp", "program", 100) << mk("ok", "program", 100);
        m_pool.last().path = ""; m_pool[m_pool.size() - 3].extra["status"] = "rejected"; m_pool[m_pool.size() - 2].availableEndMs = h0 + 1000;
        s.items << item("r", "program", "rej") << item("e", "program", "exp") << item("ghost", "program", "nope");
        const Runlist rl = build(s);
        FlightOptions o = opts(10); o.checkFiles = true;
        auto state = [&](const QString& id) { for (const auto& r : rl.items) if (r.item.id == id) return FlightPlan::readiness(r, o, true); return QJsonObject(); };
        QCOMPARE(state("r").value("state").toString(), QString("fail"));
        QVERIFY(state("r").value("checks").toObject().value("approval").toObject().value("state").toString() == "fail");
        // a file that exists with the size the library recorded is ready; a different size is a warning (copy in progress or replaced)
        QTemporaryFile f; QVERIFY(f.open()); f.write(QByteArray(2000, 'x')); f.flush();
        for (auto& m : m_pool) if (m.id == "a") { m.path = f.fileName(); m.fileSizeBytes = 2000; }
        o.media = lookup();                                                           // the lookup holds a copy of the library
        const Runlist rl2 = build(s);
        const RunlistItem* a = nullptr; for (const auto& r : rl2.items) if (r.item.id == "a") a = &r;
        QVERIFY(a); QCOMPARE(FlightPlan::readiness(*a, o, true).value("state").toString(), QString("ok"));
        for (auto& m : m_pool) if (m.id == "a") m.fileSizeBytes = 999;
        o.media = lookup();
        const Runlist rl3 = build(s);
        for (const auto& r : rl3.items) if (r.item.id == "a") a = &r;
        QCOMPARE(FlightPlan::readiness(*a, o, true).value("state").toString(), QString("warn"));
        // the whole-hour analysis counts them
        const QJsonObject fp = FlightPlan::analyze({rl3}, o);
        QVERIFY(fp.value("summary").toObject().value("readinessFails").toInt() >= 1);
    }

    void whyExplainsPlacement() {
        HourSlot s = overrunHour(); s.items[4].extra["conflictPolicy"] = "finish_previous"; s.items[4].extra["backtime"] = true;
        const Runlist rl = build(s); const FlightOptions o = opts(10);
        QStringList why; for (const auto& r : rl.items) if (r.item.id == "n") why = FlightPlan::explain(r, o);
        QVERIFY(why.join("|").contains("Hard start at :30:00"));
        QVERIFY(why.join("|").contains("finishes and this starts late"));
        QVERIFY(why.join("|").contains("Back-timed"));
        for (const auto& r : rl.items) if (r.item.id == "filler") why = FlightPlan::explain(r, o);
        QVERIFY(why.join("|").contains("Optional"));
        QCOMPARE(FlightPlan::fmtDuration(42), QString("42 s")); QCOMPARE(FlightPlan::fmtDuration(185), QString("3 m 05 s")); QCOMPARE(FlightPlan::fmtDuration(3720), QString("1 h 02 m"));
    }
};

QTEST_GUILESS_MAIN(TestFlightPlan)
#include "test_flightplan.moc"
