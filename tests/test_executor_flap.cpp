#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <memory>
#include "playout/schedule/schedulemodel.h"
#include "playout/schedule/schedulestore.h"
#include "playout/schedule/scheduleexecutor.h"
#include "playout/schedule/runlistbuilder.h"

using namespace sched;

namespace {
/// In-memory engine: records every playlist the executor pushes and every transport command.
struct FakeLink : EngineLink {
    struct Push { QString ch; QJsonArray items; bool replace; };
    QList<Push> pushes;
    QStringList transports;
    bool pushPlaylist(const QString& c, const QJsonArray& i, bool r) override { pushes.append({c, i, r}); return true; }
    bool transport(const QString&, const QString& a, const QJsonObject&) override { transports << a; return true; }
    bool sendConfig(const QString&, const QJsonObject&) override { return true; }
};

/// A Supervisor-in-a-box: temp store + executor with a settable clock, one armed channel "main" (America/New_York),
/// alerts and push reasons recorded.
struct ExecEnv {
    QTemporaryDir dir;
    ScheduleStore store;
    FakeLink link;
    std::unique_ptr<ScheduleExecutor> ex;
    qint64 now = 0;
    QTimeZone tz{"America/New_York"};
    QStringList alerts;         // "level:message"
    QStringList reasons;        // push reasons in order

    static MediaRecord mk(const QString& id, const QString& type, double dur) {
        MediaRecord m; m.id = id; m.path = "C:/media/" + id + ".mp4"; m.title = id; m.type = type; m.durationSec = dur; m.addedAtMs = 1;
        return m;
    }
    static SlotItem it(const QString& id, double dur, const QString& type = "program") {
        SlotItem i; i.id = id; i.mediaId = id; i.durationSec = dur; i.type = type; return i;
    }
    /// The usual pool: programs plus enough interstitials for an emergency loop.
    static QList<MediaRecord> defaultPool() {
        QList<MediaRecord> p;
        p << mk("p60", "program", 60) << mk("p120", "program", 120) << mk("p1800", "program", 1800) << mk("ad30", "ad", 30)
          << mk("id10", "station_id", 10) << mk("bump15", "bumper", 15) << mk("fill300", "filler", 300) << mk("promo20", "promo", 20);
        return p;
    }
    bool init(const QList<MediaRecord>& pool, const QJsonObject& channelExtra = QJsonObject(), const QJsonObject& fill = QJsonObject()) {
        if (!dir.isValid() || !store.open(dir.filePath("t.db"))) return false;
        for (const MediaRecord& m : pool) store.saveMedia(m);
        ChannelProfile c; c.id = "main"; c.name = "Main"; c.timezone = "America/New_York"; c.extra = channelExtra; c.fill = fill; c.armed = true;
        if (!store.saveChannel(c)) return false;
        ex.reset(new ScheduleExecutor(&store, &link));
        ex->setNowProvider([this]() { return now; });
        QObject::connect(ex.get(), &ScheduleExecutor::alert, [this](const QString&, const QString& lvl, const QString& msg) { alerts << lvl + ":" + msg; });
        QObject::connect(ex.get(), &ScheduleExecutor::pushed, [this](const QString&, int, bool, const QString& why) { reasons << why; });
        ex->addChannel("main");
        return true;
    }
    qint64 hour(int h, const QDate& d = QDate(2026, 10, 22)) const { return RunlistBuilder::hourStartUtcMs(d, h, tz); }
    int rebuilds() const { return ex->status("main").rebuilds; }
    QString firstPushedId() const { return link.pushes.isEmpty() || link.pushes.first().items.isEmpty() ? QString() : link.pushes.first().items[0].toObject().value("id").toString(); }
    /// Engine telemetry for an item on air.
    void onAir(const QString& itemId, double pos) { ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", itemId}, {"position", pos}}); }
    void idle() { ex->onTelemetry("main", QJsonObject{{"state", "IDLE"}}); }
    /// One second passes and the executor ticks.
    void second() { now += 1000; ex->tick(); }
    /// What every module that persists state does: a setting write the executor does not read.
    void unrelatedSetting(int i) {
        static const char* keys[] = {"ui.lastPage", "ingest.lastSweepMs", "onvif.hosts", "telemetry.lastSeen", "mux.state", "spots.jobs"};
        store.setSetting(QString::fromLatin1(keys[i % 6]), QString::number(i));
    }
};
} // namespace

/**
 * PS-7 (perf-supervisor W13): the executor's planned-list hash is computed once per rebuild instead of serializing and
 * SHA-1-hashing both hours per channel per second; a rebuild happens on a schedule revision (channel + slot writes) or a
 * write of one of the settings the build reads, never on the setSetting storm of every other module; and the emergency
 * loop's visible list no longer moves the hash, so a channel with nothing scheduled is pushed its loop once, not again on
 * the next tick (the startup emergency/join flap of 2026-10-25 22:20).
 */
class TestExecutorFlap : public QObject {
    Q_OBJECT
private slots:
    void emptyScheduleOnePushInSixtySeconds();
    void expiredHourDoesNotRepushTheLoop();
    void scheduleEditRebuildsOnce();
    void settingStormDoesNotRebuild();
    void relevantSettingsRebuild();
    void fixedSourceKeepsItsHashAcrossHoursAndReload();
    void tickCostIndependentOfRunlistSize();
};

void TestExecutorFlap::emptyScheduleOnePushInSixtySeconds() {
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool()));
    env.now = env.hour(14) + 1000;
    env.ex->onEngineConnected("main");                   // the Supervisor's wiring: connect = tick
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.reasons, QStringList{"emergency_loop"});
    QVERIFY(env.ex->status("main").emergency);
    QCOMPARE(env.rebuilds(), 1);
    const QString first = env.firstPushedId();
    QVERIFY(first.startsWith("emergency#"));
    // 60 simulated seconds: the engine takes two seconds to report the loop on air (no telemetry, then IDLE while it loads),
    // and every module in the Supervisor persists something meanwhile.
    for (int s = 1; s <= 60; ++s) {
        if (s == 2) env.idle();
        else if (s >= 3) env.onAir(first, s - 2);
        env.unrelatedSetting(s);
        env.second();
    }
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.rebuilds(), 1);
    QCOMPARE(env.alerts.filter("emergency loop").size(), 1);
    QVERIFY(env.alerts.filter("schedule resumed").isEmpty());
    QVERIFY(env.ex->status("main").emergency);
    QCOMPARE(env.ex->status("main").toJson().value("rebuilds").toInt(), 1);
    // The operator still sees the loop as the current runlist.
    QCOMPARE(env.ex->runlistJson("main").value("current").toObject().value("slotId").toString(), QString("emergency"));
}

void TestExecutorFlap::expiredHourDoesNotRepushTheLoop() {
    // The hour has content, but all of it aired before the engine connected; the next hour is empty. Before PS-7 the first
    // push (the loop) replaced ch.current with the loop's visible list, the next tick saw a different planned hash and, with
    // the engine not yet on air, pushed a fresh loop (a cut) and did so again after every rebuild.
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool(), QJsonObject(), QJsonObject{{"maxFillSec", 0}}));   // no gap fill: the hour really ends at 14:01
    HourSlot s; s.channelId = "main"; s.date = QDate(2026, 10, 22); s.hour = 14; s.isLooping = false; s.items << ExecEnv::it("p60", 60);
    QVERIFY(env.store.saveSlot(s));
    env.now = env.hour(14) + 10 * 60000;               // 14:10: p60 ended at 14:01
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.reasons, QStringList{"emergency_loop"});
    const QString first = env.firstPushedId();
    // Engine not on air yet for two ticks (the flap window), then on air; a setting storm all along.
    for (int t = 1; t <= 30; ++t) {
        if (t >= 3) env.onAir(first, t - 2);
        env.unrelatedSetting(t);
        env.second();
    }
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.rebuilds(), 1);
    QCOMPARE(env.alerts.filter("emergency loop").size(), 1);
    QVERIFY(env.alerts.filter("schedule resumed").isEmpty());
    // A real edit (something due later this hour) rebuilds once and resumes the schedule once.
    SlotItem late = ExecEnv::it("p1800", 1800); late.timingMode = "hard_start"; late.fixedOffsetSec = 20 * 60;
    s.items << late;
    QVERIFY(env.store.saveSlot(s));
    env.second();
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 2);
    // Queued behind the loop item on air (reason "continuation") or replacing it: either way the schedule is back.
    QVERIFY2(env.reasons.last() == "schedule_changed" || env.reasons.last() == "continuation", qPrintable(env.reasons.last()));
    QCOMPARE(env.alerts.filter("schedule resumed").size(), 1);
    QVERIFY(!env.ex->status("main").emergency);
    for (int t = 0; t < 10; ++t) { env.unrelatedSetting(t); env.second(); }
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 2);
}

void TestExecutorFlap::scheduleEditRebuildsOnce() {
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool()));
    HourSlot s0; s0.channelId = "main"; s0.date = QDate(2026, 10, 22); s0.hour = 14; s0.items << ExecEnv::it("p1800", 1800) << ExecEnv::it("p120", 120);
    HourSlot s1; s1.channelId = "main"; s1.date = QDate(2026, 10, 22); s1.hour = 15; s1.items << ExecEnv::it("p60", 60);
    QVERIFY(env.store.saveSlot(s0)); QVERIFY(env.store.saveSlot(s1));
    env.now = env.hour(14) + 10 * 60000;
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.reasons, QStringList{"join_in_progress"});
    QCOMPARE(env.rebuilds(), 1);
    const QString first = env.firstPushedId();
    for (int t = 1; t <= 30; ++t) { env.onAir(first, 600 + t); env.second(); }
    QCOMPARE(env.rebuilds(), 1);
    QCOMPARE(env.link.pushes.size(), 1);
    // One schedule edit: one rebuild, one push that keeps the item on air.
    s0.items << ExecEnv::it("ad30", 30, "ad");
    QVERIFY(env.store.saveSlot(s0));
    env.second();
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 2);
    QVERIFY(!env.link.pushes.last().replace);
    QCOMPARE(env.reasons.last(), QString("schedule_changed"));
    for (int t = 1; t <= 30; ++t) { env.onAir(first, 640 + t); env.second(); }
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 2);
    // A channel profile write is a schedule revision (today's behaviour): it rebuilds, and the same plan is not pushed again.
    QVERIFY(env.store.saveChannel(env.store.channel("main")));
    env.second();
    QCOMPARE(env.rebuilds(), 3);
    QCOMPARE(env.link.pushes.size(), 2);
    // The hour change rebuilds and pushes the next hour (hard start at :00 already queued, so the on-air item is kept).
    env.now = env.hour(15) + 500; env.onAir(first, 3000); env.ex->tick();
    QCOMPARE(env.rebuilds(), 4);
    QCOMPARE(env.ex->status("main").hourStartMs, env.hour(15));
}

void TestExecutorFlap::settingStormDoesNotRebuild() {
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool()));
    HourSlot s0; s0.channelId = "main"; s0.date = QDate(2026, 10, 22); s0.hour = 14; s0.items << ExecEnv::it("p1800", 1800) << ExecEnv::it("p120", 120);
    QVERIFY(env.store.saveSlot(s0));
    env.now = env.hour(14) + 60000;
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    const QString first = env.firstPushedId();
    const quint64 rev0 = env.store.revision();
    // 200 setSetting calls over 60 s (the Supervisor's modules persisting state at startup and on every change).
    for (int t = 1; t <= 60; ++t) {
        for (int k = 0; k < 3; ++k) env.unrelatedSetting(t * 3 + k);
        if (t % 7 == 0) env.store.setSettingJson("ui.layout", QJsonObject{{"t", t}});
        env.onAir(first, 60 + t);
        env.second();
    }
    QVERIFY(env.store.revision() > rev0 + 180);           // the combined counter still moved (PS-6 compatibility)...
    QCOMPARE(env.rebuilds(), 1);                           // ...and the executor ignored it
    QCOMPARE(env.link.pushes.size(), 1);
}

void TestExecutorFlap::relevantSettingsRebuild() {
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool()));
    HourSlot s0; s0.channelId = "main"; s0.date = QDate(2026, 10, 22); s0.hour = 14; s0.items << ExecEnv::it("p1800", 1800);
    QVERIFY(env.store.saveSlot(s0));
    env.now = env.hour(14) + 60000;
    env.ex->onEngineConnected("main");
    const QString first = env.firstPushedId();
    env.onAir(first, 60); env.second();
    QCOMPARE(env.rebuilds(), 1);
    // The settings a build reads: hour recipes and ad contracts. Each write is one rebuild (the plan is unchanged: no push).
    QVERIFY(env.store.setSettingJson("programming.recipes", QJsonObject{{"recipes", QJsonArray()}}));
    env.second();
    QCOMPARE(env.rebuilds(), 2);
    QVERIFY(env.store.setSettingJson("traffic.contracts", QJsonObject{{"contracts", QJsonArray()}}));
    env.second();
    QCOMPARE(env.rebuilds(), 3);
    // A key that only shares the "traffic." family is not read by the build.
    QVERIFY(env.store.setSetting("traffic.lastImport", "x"));
    env.second();
    QCOMPARE(env.rebuilds(), 3);
    // A rebuild for the same plan never pushes.
    QCOMPARE(env.link.pushes.size(), 1);
    // A re-roll still forces a rebuild (it zeroes the stored revision) and an operator push.
    env.ex->rerollBlocks("main", true);
    env.second();
    QCOMPARE(env.rebuilds(), 4);
    QCOMPARE(env.reasons.last(), QString("operator"));
}

void TestExecutorFlap::fixedSourceKeepsItsHashAcrossHoursAndReload() {
    ExecEnv env;
    QVERIFY(env.init(ExecEnv::defaultPool(), QJsonObject{{"fixedSource", QJsonObject{{"type", "ndi"}, {"name", "STUDIO (CAM1)"}}}}));
    env.now = env.hour(10) + 30 * 60000;
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.link.pushes[0].items.size(), 1);
    const QString id = env.firstPushedId();
    // The hour changes: a rebuild, the same endless item, no push (nothing queued at :00).
    env.now = env.hour(11) + 300; env.onAir(id, 1800.3); env.ex->tick();
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 1);
    for (int t = 0; t < 10; ++t) { env.unrelatedSetting(t); env.onAir(id, 1801 + t); env.second(); }
    QCOMPARE(env.rebuilds(), 2);
    QCOMPARE(env.link.pushes.size(), 1);
    // A changed source spec, applied the way the Supervisor does (save + reloadChannel): one push of the new item...
    ChannelProfile p = env.store.channel("main");
    QJsonObject fs = p.extra.value("fixedSource").toObject(); fs["name"] = "STUDIO (CAM2)"; p.extra["fixedSource"] = fs;
    QVERIFY(env.store.saveChannel(p));
    env.ex->reloadChannel("main");
    QCOMPARE(env.link.pushes.size(), 2);
    QCOMPARE(env.link.pushes[1].items[0].toObject().value("source").toObject().value("ndiName").toString(), QString("STUDIO (CAM2)"));
    const QString id2 = env.link.pushes[1].items[0].toObject().value("id").toString();
    QVERIFY(id2 != id);
    // ...and the next hour is again not a new playlist.
    env.onAir(id2, 5.0); env.second();
    env.now = env.hour(12) + 300; env.onAir(id2, 3000.0); env.ex->tick();
    QCOMPARE(env.link.pushes.size(), 2);
}

void TestExecutorFlap::tickCostIndependentOfRunlistSize() {
    // Two full hours of short items (what a music or interstitial channel plans): the per-tick cost no longer serializes and
    // hashes them. Compared, on this machine, with what the old path did every second (build the engine JSON of both hours,
    // serialize, SHA-1): the tick must be several times cheaper. Numbers are printed for the log.
    ExecEnv env;
    QList<MediaRecord> pool = ExecEnv::defaultPool();
    for (int i = 0; i < 40; ++i) pool << ExecEnv::mk(QString("clip%1").arg(i), "program", 15 + (i % 5));
    QVERIFY(env.init(pool));
    for (int h : {14, 15}) {
        HourSlot s; s.channelId = "main"; s.date = QDate(2026, 10, 22); s.hour = h;
        for (int i = 0; i < 200; ++i) s.items << ExecEnv::it(QString("clip%1").arg(i % 40), 15 + (i % 5));
        QVERIFY(env.store.saveSlot(s));
    }
    env.now = env.hour(14) + 60000;
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    const int planned = env.link.pushes[0].items.size();
    QVERIFY2(planned >= 300, qPrintable(QString::number(planned)));
    const QString first = env.firstPushedId();
    env.onAir(first, 1.0);
    const int N = 1000;
    QElapsedTimer t; t.start();
    for (int i = 0; i < N; ++i) { env.now += 10; env.ex->tick(); }
    const double tickUs = t.nsecsElapsed() / 1000.0 / N;
    t.restart();
    for (int i = 0; i < N; ++i) {
        const QJsonArray a = env.ex->plannedPlaylist("main", env.now);
        volatile int keep = QCryptographicHash::hash(QJsonDocument(a).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).size();
        (void)keep;
    }
    const double oldUs = t.nsecsElapsed() / 1000.0 / N;
    qInfo().noquote() << QString("[bench] %1 planned items: tick %2 us, old per-tick serialize+hash %3 us (%4x)").arg(planned).arg(tickUs, 0, 'f', 1).arg(oldUs, 0, 'f', 1).arg(oldUs / qMax(tickUs, 0.01), 0, 'f', 1);
    QCOMPARE(env.link.pushes.size(), 1);
    QCOMPARE(env.rebuilds(), 1);
    QVERIFY2(tickUs * 3.0 < oldUs, qPrintable(QString("tick %1 us vs old %2 us").arg(tickUs).arg(oldUs)));
}

QTEST_GUILESS_MAIN(TestExecutorFlap)
#include "test_executor_flap.moc"
