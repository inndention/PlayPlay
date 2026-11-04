#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTimeZone>
#include "playout/schedule/schedulemodel.h"
#include "playout/schedule/schedulestore.h"
#include "playout/schedule/runlistbuilder.h"
#include "playout/schedule/fillpolicy.h"
#include "playout/schedule/schedulevalidator.h"
#include "playout/schedule/scheduleexecutor.h"

using namespace sched;

/**
 * Scheduling core tests: store round trip, wall-clock layout rules (soft follow,
 * hard start truncation and fill, fill_until, hour-end truncation, hour loop),
 * fill policy, validator codes, time-zone hour boundaries, and the executor's
 * push decisions (join in progress, hard start at :00, emergency loop, as-run).
 */
class TestSchedule : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void hourStartRespectsTimeZone();
    void storeRoundTrip();
    void datedOverrideWins();
    void softItemsChain();
    void hardStartTruncatesAndFills();
    void fillUntilStretches();
    void hourEndTruncation();
    void hourLoopFills();
    void smartSlotLatestEpisode();
    void fillPolicyPrefersFreshAndId();
    void validatorCodes();
    void executorPushesWithJoinInProgress();
    void executorEmergencyLoopAndAsRun();
    void executorEmergencyNoFlapAndRefill();

private:
    QTemporaryDir m_dir;
    ScheduleStore* m_store = nullptr;
    QTimeZone m_tz{"America/New_York"};
    qint64 m_hour = 0;     // a fixed hour start (local 2026-10-22 14:00 ET)

    MediaRecord mk(const QString& id, const QString& type, double dur, const QString& series = {}, const QDate& ep = {}) {
        MediaRecord m; m.id = id; m.path = "C:/media/" + id + ".mp4"; m.title = id; m.type = type; m.durationSec = dur;
        m.series = series; m.episodeDate = ep; m.addedAtMs = 1;
        return m;
    }
    SlotItem it(const QString& id, double dur, const QString& type = "program") {
        SlotItem i; i.id = id; i.mediaId = id; i.durationSec = dur; i.type = type; return i;
    }
    MediaLookup lookup() {
        MediaLookup ml;
        ml.byId = [this](const QString& id, bool* f) { return m_store->media(id, f); };
        ml.bySeries = [this](const QString& s) { return m_store->mediaBySeries(s); };
        ml.byTypes = [this](const QStringList& t) { return m_store->mediaByType(t); };
        return ml;
    }
};

class FakeLink : public EngineLink {
public:
    struct Push { QString ch; QJsonArray items; bool replace; };
    QList<Push> pushes;
    QList<QPair<QString, QString>> transports;
    bool pushPlaylist(const QString& ch, const QJsonArray& items, bool replace) override { pushes.append({ch, items, replace}); return true; }
    bool transport(const QString& ch, const QString& a, const QJsonObject&) override { transports.append({ch, a}); return true; }
    bool sendConfig(const QString&, const QJsonObject&) override { return true; }
};

void TestSchedule::initTestCase() {
    QVERIFY(m_dir.isValid());
    m_store = new ScheduleStore(this);
    QVERIFY(m_store->open(m_dir.filePath("test.db")));
    ChannelProfile c; c.id = "main"; c.name = "Main"; c.timezone = "America/New_York";
    QVERIFY(m_store->saveChannel(c));
    for (const auto& m : { mk("p60", "program", 60), mk("p120", "program", 120), mk("p1800", "program", 1800),
                           mk("p3000", "program", 3000), mk("ad30", "ad", 30), mk("id10", "station_id", 10),
                           mk("bump15", "bumper", 15), mk("fill300", "filler", 300), mk("promo20", "promo", 20),
                           mk("ep1", "program", 600, "News", QDate(2026, 10, 20)), mk("ep2", "program", 600, "News", QDate(2026, 10, 22)) })
        QVERIFY(m_store->saveMedia(m));
    m_hour = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 14, m_tz);
}

void TestSchedule::hourStartRespectsTimeZone() {
    // 14:00 EDT = 18:00 UTC on 2026-10-22 (DST in effect).
    QCOMPARE(QDateTime::fromMSecsSinceEpoch(m_hour, QTimeZone::utc()).time().hour(), 18);
    QDate d; int h = 0;
    const qint64 back = RunlistBuilder::hourStartUtcMsForTime(m_hour + 1234567, m_tz, &d, &h);
    QCOMPARE(back, m_hour); QCOMPARE(h, 14); QCOMPARE(d, QDate(2026, 10, 22));
    // DST end: 2026-11-01 01:00 local occurs twice; the hour before and after are distinct.
    const qint64 a = RunlistBuilder::hourStartUtcMs(QDate(2026, 11, 1), 0, m_tz);
    const qint64 b = RunlistBuilder::hourStartUtcMs(QDate(2026, 11, 1), 3, m_tz);
    QCOMPARE((b - a) / 3600000LL, 4LL);   // 00:00 -> 03:00 spans four real hours that night
}

void TestSchedule::storeRoundTrip() {
    HourSlot s; s.channelId = "main"; s.dayOfWeek = 4; s.hour = 14; s.id = HourSlot::templateId(4, 14);
    s.blockType = "show"; s.showBlock["title"] = "Afternoon"; s.loopOverride["enabled"] = true; s.loopOverride["loopUntilHour"] = 16;
    QJsonObject dc; dc["commandId"] = "obs-scene-2"; dc["offsetSec"] = -5; s.deviceCommands.append(dc);
    SlotItem a = it("p60", 60); a.timingMode = "hard_start"; a.fixedOffsetSec = 120; a.cgOnStart << "lower_third"; a.smartConfig["type"] = "latest"; a.smartConfig["seriesName"] = "News";
    a.extra["customField"] = "kept";
    s.items << a << it("ad30", 30, "ad");
    QVERIFY(m_store->saveSlot(s));
    bool found = false;
    HourSlot r = m_store->slot("main", "4-14", &found);
    QVERIFY(found);
    QCOMPARE(r.items.size(), 2);
    QCOMPARE(r.blockType, QString("show"));
    QCOMPARE(r.showBlock["title"].toString(), QString("Afternoon"));
    QCOMPARE(r.loopOverride["loopUntilHour"].toInt(), 16);
    QCOMPARE(r.deviceCommands.size(), 1);
    QCOMPARE(r.items[0].timingMode, QString("hard_start"));
    QCOMPARE(r.items[0].fixedOffsetSec, 120.0);
    QCOMPARE(r.items[0].cgOnStart, QStringList{"lower_third"});
    QCOMPARE(r.items[0].smartConfig["seriesName"].toString(), QString("News"));
    QCOMPARE(r.items[0].extra["customField"].toString(), QString("kept"));
    QVERIFY(m_store->deleteSlot("main", "4-14"));
}

void TestSchedule::datedOverrideWins() {
    HourSlot t; t.channelId = "main"; t.dayOfWeek = scheduleDow(QDate(2026, 10, 22)); t.hour = 9; t.items << it("p60", 60);
    QVERIFY(m_store->saveSlot(t));
    HourSlot d; d.channelId = "main"; d.date = QDate(2026, 10, 22); d.hour = 9; d.items << it("p120", 120) << it("p60", 60);
    QVERIFY(m_store->saveSlot(d));
    bool ov = false;
    HourSlot e = m_store->effectiveSlot("main", QDate(2026, 10, 22), 9, &ov);
    QVERIFY(ov); QCOMPARE(e.items.size(), 2);
    e = m_store->effectiveSlot("main", QDate(2026, 10, 29), 9, &ov);   // next week: template
    QVERIFY(!ov); QCOMPARE(e.items.size(), 1);
    QCOMPARE(m_store->week("main", QDate(2026, 10, 18)).size(), 168);
    m_store->deleteSlot("main", t.id); m_store->deleteSlot("main", d.id);
}

void TestSchedule::softItemsChain() {
    HourSlot s; s.channelId = "main"; s.id = "4-14"; s.items << it("p60", 60) << it("ad30", 30, "ad") << it("p120", 120);
    BuildOptions o; o.fillGaps = false; o.nowMs = m_hour;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QCOMPARE(rl.items.size(), 3);
    QCOMPARE(rl.items[0].plannedStartMs, m_hour);
    QVERIFY(rl.items[0].hard);                       // first item anchors the hour
    QCOMPARE(rl.items[1].plannedStartMs, m_hour + 60000);
    QVERIFY(!rl.items[1].hard);
    QCOMPARE(rl.items[2].plannedEndMs, m_hour + 210000);
    QCOMPARE(rl.scheduledSec, 210.0);
    QCOMPARE(rl.gapSec, 3600.0 - 210.0);
}

void TestSchedule::hardStartTruncatesAndFills() {
    HourSlot s; s.channelId = "main"; s.id = "4-14";
    SlotItem a = it("p120", 120);
    SlotItem b = it("p60", 60); b.timingMode = "hard_start"; b.fixedOffsetSec = 90;     // cuts into a
    SlotItem c = it("p60", 60); c.timingMode = "hard_start"; c.fixedOffsetSec = 300;    // 150 s gap before it
    s.items << a << b << c;
    BuildOptions o; o.fillGaps = true; o.nowMs = m_hour; o.fillSeed = 7;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QVERIFY(rl.items.size() >= 4);
    QCOMPARE(rl.items[0].durationSec, 90.0);
    QVERIFY(rl.items[0].truncated);
    QCOMPARE(rl.items[1].plannedStartMs, m_hour + 90000);
    QVERIFY(rl.items[1].hard);
    // fill between 150 s and 300 s
    int fills = 0; qint64 lastEnd = m_hour + 150000;
    for (int i = 2; i < rl.items.size(); ++i) {
        if (rl.items[i].origin == "fill" && rl.items[i].plannedStartMs < m_hour + 300000) { ++fills; QVERIFY(rl.items[i].plannedStartMs >= m_hour + 150000); QVERIFY(rl.items[i].plannedEndMs <= m_hour + 300000); lastEnd = rl.items[i].plannedEndMs; }
        if (rl.items[i].runlistId.contains("#p60#") && rl.items[i].plannedStartMs == m_hour + 300000) { QVERIFY(rl.items[i].hard); }
    }
    QVERIFY(fills > 0);
    QVERIFY(rl.filledSec > 0.0);
}

void TestSchedule::fillUntilStretches() {
    HourSlot s; s.channelId = "main"; s.id = "4-14";
    SlotItem a = it("p60", 60);
    SlotItem f = it("fill300", 300, "filler"); f.timingMode = "fill_until";
    SlotItem c = it("p60", 60); c.timingMode = "hard_start"; c.fixedOffsetSec = 1800;
    s.items << a << f << c;
    BuildOptions o; o.fillGaps = false; o.nowMs = m_hour;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QCOMPARE(rl.items.size(), 3);
    QCOMPARE(rl.items[1].durationSec, 1740.0);        // 60 s .. 1800 s
    QVERIFY(rl.items[1].loop);
    QCOMPARE(rl.items[2].plannedStartMs, m_hour + 1800000);
}

void TestSchedule::hourEndTruncation() {
    HourSlot s; s.channelId = "main"; s.id = "4-14"; s.items << it("p3000", 3000) << it("p1800", 1800) << it("p60", 60);
    BuildOptions o; o.fillGaps = false; o.nowMs = m_hour;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QCOMPARE(rl.items.size(), 2);
    QCOMPARE(rl.items[1].durationSec, 600.0);
    QVERIFY(rl.items[1].truncated);
    QCOMPARE(rl.items[1].plannedEndMs, m_hour + 3600000);
    QVERIFY(!rl.notes.filter("do not fit").isEmpty());
    // A continuation into the next hour is not truncated.
    o.continuesIntoNextHour = true;
    rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QCOMPARE(rl.items.size(), 3);
    QVERIFY(!rl.items[1].truncated);
}

void TestSchedule::hourLoopFills() {
    HourSlot s; s.channelId = "main"; s.id = "4-14"; s.isLooping = true; s.items << it("p1800", 1800) << it("ad30", 30, "ad");
    BuildOptions o; o.fillGaps = false; o.nowMs = m_hour;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QVERIFY(rl.items.size() > 2);
    QCOMPARE(rl.items.last().plannedEndMs, m_hour + 3600000);
    QVERIFY(rl.items.last().truncated || rl.items.last().durationSec > 0);
    QCOMPARE(rl.gapSec, 0.0);
}

void TestSchedule::smartSlotLatestEpisode() {
    HourSlot s; s.channelId = "main"; s.id = "4-14";
    SlotItem a; a.id = "news"; a.type = "program"; a.smartConfig["type"] = "latest"; a.smartConfig["seriesName"] = "News";
    SlotItem b; b.id = "news-prev"; b.type = "program"; b.smartConfig["type"] = "previous"; b.smartConfig["seriesName"] = "News";
    s.items << a << b;
    BuildOptions o; o.fillGaps = false; o.nowMs = m_hour;
    Runlist rl = RunlistBuilder::build(s, m_hour, lookup(), o);
    QCOMPARE(rl.items.size(), 2);
    QCOMPARE(rl.items[0].item.mediaId, QString("ep2"));
    QCOMPARE(rl.items[1].item.mediaId, QString("ep1"));
    QCOMPARE(rl.items[0].origin, QString("smart"));
    QCOMPARE(rl.items[0].durationSec, 600.0);
}

void TestSchedule::fillPolicyPrefersFreshAndId() {
    FillSettings fs; FillContext ctx; ctx.nowMs = m_hour; ctx.seed = 3;
    const QList<MediaRecord> pool = m_store->mediaByType(FillPolicy::fillTypes(fs));
    QVERIFY(pool.size() >= 4);
    QList<MediaRecord> picks = FillPolicy::fillGap(100.0, pool, ctx, fs, true);
    QVERIFY(!picks.isEmpty());
    QCOMPARE(picks.first().type, QString("station_id"));   // ID first at top of hour
    double total = 0; for (const auto& m : picks) total += m.durationSec;
    QVERIFY(total <= 100.0);
    // Deterministic with a seed.
    QList<MediaRecord> again = FillPolicy::fillGap(100.0, pool, ctx, fs, true);
    QCOMPARE(again.size(), picks.size());
    for (int i = 0; i < picks.size(); ++i) QCOMPARE(again[i].id, picks[i].id);
    // Emergency loop covers the requested time.
    QList<MediaRecord> loop = FillPolicy::emergencyLoop(600.0, pool, fs);
    double lt = 0; for (const auto& m : loop) lt += m.durationSec;
    QVERIFY(lt >= 600.0);
}

void TestSchedule::validatorCodes() {
    HourSlot s; s.channelId = "main"; s.id = "4-14";
    SlotItem missing = it("nope", 30); missing.title = "missing";
    SlotItem a = it("p3000", 3000); SlotItem b = it("p1800", 1800);
    SlotItem h1 = it("p60", 60); h1.timingMode = "hard_start"; h1.fixedOffsetSec = 100;
    SlotItem h2 = it("p60", 60); h2.timingMode = "hard_start"; h2.fixedOffsetSec = 50;   // conflict: earlier than h1
    s.items << missing << a << b << h1 << h2;
    ValidatorOptions vo; vo.media = lookup(); vo.checkFilesExist = false; vo.requireLegalId = true; vo.build.nowMs = m_hour;
    ValidationResult vr = ScheduleValidator::validate(s, m_hour, vo);
    QStringList codes; for (const auto& i : vr.issues) codes << i.code;
    QVERIFY(codes.contains("missing_media"));
    QVERIFY(codes.contains("overfill"));
    QVERIFY(codes.contains("timing_conflict"));
    QVERIFY(codes.contains("missing_legal_id"));
    QVERIFY(!vr.ok);
    // Clean hour validates ok (warnings allowed).
    HourSlot ok; ok.channelId = "main"; ok.id = "4-15"; ok.items << it("id10", 10, "station_id") << it("p1800", 1800) << it("p1800", 1800);
    vr = ScheduleValidator::validate(ok, m_hour, vo);
    QVERIFY(vr.ok);
}

void TestSchedule::executorPushesWithJoinInProgress() {
    // Schedule 14:00 = p1800, p120 (rest filled) ; 15:00 = p60.
    HourSlot s0; s0.channelId = "main"; s0.date = QDate(2026, 10, 22); s0.hour = 14; s0.items << it("p1800", 1800) << it("p120", 120);
    HourSlot s1; s1.channelId = "main"; s1.date = QDate(2026, 10, 22); s1.hour = 15; s1.items << it("p60", 60);
    QVERIFY(m_store->saveSlot(s0)); QVERIFY(m_store->saveSlot(s1));

    FakeLink link;
    ScheduleExecutor ex(m_store, &link);
    qint64 now = m_hour + 600000;     // 14:10:00
    ex.setNowProvider([&now]() { return now; });
    ex.addChannel("main");
    ex.onEngineConnected("main");     // triggers a tick + push

    QCOMPARE(link.pushes.size(), 1);
    const auto& p = link.pushes.first();
    QVERIFY(p.replace);
    QVERIFY(p.items.size() >= 3);
    const QJsonObject first = p.items[0].toObject();
    QCOMPARE(first["inPoint"].toDouble(), 600.0);                 // joined 10 minutes in
    QCOMPARE(first["duration"].toDouble(), 1200.0);
    QVERIFY(!first.contains("hardStartUtcMs"));                   // starts now, not anchored
    // Second item of the hour keeps its soft follow; first of next hour is hard at 15:00.
    bool sawNextHourHard = false;
    for (const auto& v : p.items) {
        const QJsonObject o = v.toObject();
        if (qint64(o["plannedUtcMs"].toDouble()) == m_hour + 3600000) { sawNextHourHard = o.contains("hardStartUtcMs"); }
    }
    QVERIFY(sawNextHourHard);
    QVERIFY(link.transports.contains(qMakePair(QString("main"), QString("play"))));

    // Engine reports it is playing the first item: a tick must not re-push.
    QJsonObject st; st["state"] = "ON_AIR"; st["itemId"] = first["id"].toString(); st["position"] = 5.0;
    ex.onTelemetry("main", st);
    now += 1000; ex.tick();
    QCOMPARE(link.pushes.size(), 1);

    // Schedule edit while on air: re-push keeps the current item (replaceCurrent=false).
    s0.items << it("ad30", 30, "ad");
    QVERIFY(m_store->saveSlot(s0));
    now += 1000; ex.tick();
    QCOMPARE(link.pushes.size(), 2);
    QVERIFY(!link.pushes.last().replace);
    bool containsOnAir = false;
    for (const auto& v : link.pushes.last().items) if (v.toObject()["id"].toString() == first["id"].toString()) containsOnAir = true;
    QVERIFY(!containsOnAir);

    // Hour change: next hour rebuilds and pushes again.
    now = m_hour + 3600000 + 500; ex.tick();
    QCOMPARE(link.pushes.size(), 3);
    QCOMPARE(ex.status("main").hourStartMs, m_hour + 3600000);

    m_store->deleteSlot("main", s0.id); m_store->deleteSlot("main", s1.id);
}

void TestSchedule::executorEmergencyNoFlapAndRefill() {
    // Nothing scheduled: the emergency loop's visible runlist must not be taken for schedule (no "schedule resumed" flap),
    // and a loop that played out (engine IDLE at its playlist end) is pushed again instead of leaving the channel IDLE.
    FakeLink link;
    ScheduleExecutor ex(m_store, &link);
    const qint64 h2 = RunlistBuilder::hourStartUtcMs(QDate(2026, 11, 3), 2, m_tz);
    qint64 now = h2 + 1000;
    ex.setNowProvider([&now]() { return now; });
    QStringList alerts;
    connect(&ex, &ScheduleExecutor::alert, [&](const QString&, const QString& lvl, const QString& msg) { alerts << lvl + ":" + msg; });
    ex.addChannel("main");
    ex.onEngineConnected("main");
    QCOMPARE(link.pushes.size(), 1);
    QVERIFY(ex.status("main").emergency);
    QJsonObject st; st["state"] = "ON_AIR"; st["itemId"] = link.pushes.first().items[0].toObject()["id"].toString(); st["position"] = 1.0;
    for (int i = 0; i < 5; ++i) { now += 1000; ex.onTelemetry("main", st); ex.tick(); }
    QCOMPARE(link.pushes.size(), 1);
    QVERIFY(ex.status("main").emergency);
    QVERIFY(alerts.filter("schedule resumed").isEmpty());
    // Two hours later the loop has played out and the engine is IDLE: one tick pushes a fresh loop.
    QJsonObject idle; idle["state"] = "IDLE";
    now += 2 * 3600000LL + 60000; ex.onTelemetry("main", idle); ex.tick();
    QCOMPARE(link.pushes.size(), 2);
    QCOMPARE(link.pushes.last().items[0].toObject()["origin"].toString(), QString("emergency"));
    QVERIFY(link.pushes.last().replace);
    QVERIFY(ex.status("main").emergency);
}

void TestSchedule::executorEmergencyLoopAndAsRun() {
    // Nothing scheduled at 20:00/21:00 -> emergency loop from typed pool.
    FakeLink link;
    ScheduleExecutor ex(m_store, &link);
    const qint64 h20 = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 20, m_tz);
    qint64 now = h20 + 1000;
    ex.setNowProvider([&now]() { return now; });
    QStringList alerts;
    connect(&ex, &ScheduleExecutor::alert, [&](const QString&, const QString& lvl, const QString& msg) { alerts << lvl + ":" + msg; });
    ex.addChannel("main");
    ex.onEngineConnected("main");
    QCOMPARE(link.pushes.size(), 1);
    QVERIFY(link.pushes.first().items.size() > 5);
    QCOMPARE(link.pushes.first().items[0].toObject()["origin"].toString(), QString("emergency"));
    QVERIFY(ex.status("main").emergency);
    QVERIFY(!alerts.filter("emergency loop").isEmpty());

    // As-run: a START from the engine for the first emergency item, then END.
    const QString rid = link.pushes.first().items[0].toObject()["id"].toString();
    QList<AsRunRow> stored;
    connect(&ex, &ScheduleExecutor::asRunStored, [&](const AsRunRow& r) { stored << r; });
    QJsonObject start; start["type"] = "START"; start["itemId"] = rid; start["assetId"] = "id10"; start["title"] = "id10"; start["utcMs"] = double(now); start["frame"] = 100; start["timecode"] = "20:00:01;00";
    ex.onEngineAsRun("main", start);
    QJsonObject end = start; end["type"] = "END"; end["utcMs"] = double(now + 10000);
    ex.onEngineAsRun("main", end);
    QCOMPARE(stored.size(), 2);
    QCOMPARE(stored[0].status, QString("FILL"));
    QCOMPARE(stored[0].origin, QString("emergency"));
    const QList<AsRunRow> rows = m_store->asRun("main", now - 1000, now + 20000);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].eventType, QString("START"));
    QCOMPARE(rows[0].actualDurationMs, 10000LL);
    QCOMPARE(m_store->media("id10").playCount, 1);

    // A scheduled item that starts 3 s late is LATE_START with variance.
    HourSlot s; s.channelId = "main"; s.date = QDate(2026, 10, 22); s.hour = 20; s.items << it("p60", 60);
    QVERIFY(m_store->saveSlot(s));
    now += 2000; ex.tick();
    QVERIFY(link.pushes.size() >= 2);
    const QString sid = link.pushes.last().items[0].toObject()["id"].toString();
    const RunlistItem* ri = ex.lookupRunlistItem("main", sid);
    QVERIFY(ri);
    QJsonObject s2; s2["type"] = "START"; s2["itemId"] = sid; s2["assetId"] = "p60"; s2["utcMs"] = double(ri->plannedStartMs + 3000);
    ex.onEngineAsRun("main", s2);
    QCOMPARE(stored.last().status, QString("LATE_START"));
    QCOMPARE(stored.last().varianceMs, 3000LL);
    QCOMPARE(m_store->asRunSummary("main", now - 100000, now + 100000)["late"].toInt(), 1);
    m_store->deleteSlot("main", s.id);
}

QTEST_GUILESS_MAIN(TestSchedule)
#include "test_schedule.moc"
