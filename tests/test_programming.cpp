#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTimeZone>
#include <memory>
#include "playout/schedule/schedulemodel.h"
#include "playout/schedule/schedulestore.h"
#include "playout/schedule/scheduleexecutor.h"
#include "playout/schedule/queryblocks.h"
#include "playout/schedule/formatclock.h"
#include "playout/schedule/runlistbuilder.h"
#include "playout/schedule/fillpolicy.h"

using namespace sched;

namespace {
/// In-memory engine: records every playlist the executor pushes.
struct FakeLink : EngineLink {
    struct Push { QString ch; QJsonArray items; bool replace; };
    QList<Push> pushes;
    bool pushPlaylist(const QString& c, const QJsonArray& i, bool r) override { pushes.append({c, i, r}); return true; }
    bool transport(const QString&, const QString&, const QJsonObject&) override { return true; }
    bool sendConfig(const QString&, const QJsonObject&) override { return true; }
};

/// A Supervisor-in-a-box: temp store + executor with a settable clock, one channel "main" (America/New_York).
struct ExecEnv {
    QTemporaryDir dir;
    ScheduleStore store;
    FakeLink link;
    std::unique_ptr<ScheduleExecutor> ex;
    qint64 now = 0;
    QTimeZone tz{"America/New_York"};

    bool init(const QList<MediaRecord>& pool, const QJsonObject& channelExtra) {
        if (!dir.isValid() || !store.open(dir.filePath("t.db"))) return false;
        for (const MediaRecord& m : pool) store.saveMedia(m);
        ChannelProfile c; c.id = "main"; c.name = "Main"; c.timezone = "America/New_York"; c.extra = channelExtra; c.armed = true;
        if (!store.saveChannel(c)) return false;
        ex.reset(new ScheduleExecutor(&store, &link));
        ex->setNowProvider([this]() { return now; });
        ex->addChannel("main");
        return true;
    }
    qint64 at(int h, int m, int s = 0, int ms = 0) const { return QDateTime(QDate(2026, 10, 22), QTime(h, m, s, ms), tz).toMSecsSinceEpoch(); }
    QJsonArray items(const char* which) const { return ex->runlistJson("main").value(which).toObject().value("items").toArray(); }
    static QStringList mediaIds(const QJsonArray& a, qint64 startedBeforeMs = 0) {
        QStringList l;
        for (const QJsonValue& v : a) { const QJsonObject o = v.toObject(); if (startedBeforeMs == 0 || qint64(o.value("plannedStartMs").toDouble()) <= startedBeforeMs) l << o.value("mediaId").toString(); }
        return l;
    }
};
} // namespace

/**
 * Programming tests (PlayPlay OBS parity): query blocks (prefer orders, no-repeat ranking,
 * target coverage, series positional picks, exclusions), folder blocks (ordered round robin,
 * freshness random), hour recipes (cycle until full, locks carried), the ad-break builder,
 * weighted fill (airtime shares, anchors, one program per hour with carry), contract pacing,
 * and the RunlistBuilder expansion with the per-hour pick cache and soft anchors.
 *
 * Regression tests for the Phase 9 review fixes: stable autofill ids (no re-open at :00), lazy block
 * pool, fixed-source channels, overrun cut and recipe closing block, real re-roll, lockAtSec -1,
 * weighted carry through the executor, hard-start anchors, rotation, multi-creative pacing,
 * least_played ordering, unplayed ordered folders and the validated pick cache.
 */
class TestProgramming : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void queryPreferOrders();
    void queryTargetCoverageAndExclusion();
    void queryRepeatsRankLast();
    void seriesBlockPositional();
    void folderBlockOrdered();
    void recipeLaysUntilFull();
    void adBreakRecipeShape();
    void carryLocksSeatsAnchors();
    void weightedFillShares();
    void weightedFillCarriesLongProgram();
    void contractPacing();
    void builderExpandsBlocksWithCache();
    void builderSoftAnchorNearestBoundary();
    // --- regressions ---
    void leastPlayedLongestSinceAiredWins();
    void orderedFolderStartsAtItsFirstItem();
    void lockAtSecNegativeMeansNoAnchor();
    void synthesizedIdsAreDeterministic();
    void autofillRecipeKeepsItsIdsAcrossRebuilds();
    void autofillWeightedIsFrozenPerHour();
    void autofillWeightedCarriesLongProgramIntoNextHour();
    void fixedSourceChannelAirsOneEndlessItem();
    void overrunIsCutToTheBlockTarget();
    void recipeClosesTheHourWithAnInterstitial();
    void rerollMovesOnFromPreviousPicks();
    void executorRerollKeepsStartedBlocks();
    void pickCacheIsValidated();
    void cachedBlocksDoNotReadTheLibrary();
    void weightedHardStartLockIsPlacedAtItsOffset();
    void weightedRotationDoesNotBurnPrograms();
    void multiCreativeContractPacesAsOne();
    void autofillWeightedRerollMovesTheNextHour();
    void typedPoolMatchesTypesCaseInsensitively();
    void conditionsAgeAndStackedRules();
    void evenRotationCoversPoolBeforeRepeating();
    void evenRotationSpreadsAcrossHours();
    void recipeFolderPartWithAgeAndAdBreaks();
    void builtinRecipesFillTheHour();
    void exhaustedBlockRepeatsInsteadOfGap();
    void overrunPlanContinueCutSplit();
    void continuationHourStartsAfterTheCarry();
    void overrunKeepsAiringAcrossTheHour();
    void smallFillPoolFillsTheWholeGap();
    void operatorTakeQueuesTheScheduleBehindIt();
    void hardStartInterruptionPolicies();
    void backtimingEndsOnTheHardStart();
    void gapFillCanUseExtraTypes();

private:
    QList<MediaRecord> m_pool;
    qint64 m_now = 0;
    QList<MediaRecord> deepPool();
    MediaRecord mk(const QString& id, const QString& type, double dur, int plays, qint64 lastMs, qint64 addedMs, const QString& cat = QString(), const QString& series = QString(), const QDate& ep = QDate());
    MediaLookup lookup() const;
};

MediaRecord TestProgramming::mk(const QString& id, const QString& type, double dur, int plays, qint64 lastMs, qint64 addedMs, const QString& cat, const QString& series, const QDate& ep) {
    MediaRecord m; m.id = id; m.title = id; m.path = "D:/media/" + type + "/" + id + ".mp4"; m.type = type; m.durationSec = dur;
    m.playCount = plays; m.lastPlayedMs = lastMs; m.addedAtMs = addedMs; m.series = series; m.episodeDate = ep;
    if (!cat.isEmpty()) m.extra["category"] = cat;
    return m;
}

static MediaLookup lookupOf(const QList<MediaRecord>& pool) {
    MediaLookup ml;
    ml.byId = [pool](const QString& id, bool* f) { for (const auto& m : pool) if (m.id == id) { if (f) *f = true; return m; } if (f) *f = false; return MediaRecord(); };
    ml.bySeries = [pool](const QString& s) { QList<MediaRecord> o; for (const auto& m : pool) if (m.series == s) o.append(m); return o; };
    ml.byTypes = [pool](const QStringList& t) { QList<MediaRecord> o; for (const auto& m : pool) if (t.contains(m.type)) o.append(m); return o; };
    ml.all = [pool]() { return pool; };
    return ml;
}

MediaLookup TestProgramming::lookup() const { return lookupOf(m_pool); }

void TestProgramming::initTestCase() {
    m_now = QDateTime(QDate(2026, 10, 22), QTime(12, 0), QTimeZone("America/New_York")).toMSecsSinceEpoch();
    const qint64 day = 86400000LL;
    // programs: 3 x 28 min, 1 x 55 min (long), with varied play counts / ages
    m_pool << mk("p1", "program", 1680, 5, m_now - 3 * day, m_now - 40 * day) << mk("p2", "program", 1680, 1, m_now - 10 * day, m_now - 2 * day)
           << mk("p3", "program", 1680, 3, m_now - 1 * day, m_now - 20 * day) << mk("plong", "program", 3300, 0, 0, m_now - 1 * day);
    // ads 30 s x 8
    for (int i = 0; i < 8; ++i) m_pool << mk(QString("ad%1").arg(i), "ad", 30, i, i < 2 ? m_now - 600000 : m_now - (i + 1) * day, m_now - (i + 5) * day);
    // ids / promos
    m_pool << mk("id1", "station_id", 10, 2, m_now - day, m_now - 100 * day) << mk("promo1", "promo", 45, 1, m_now - 2 * day, m_now - 30 * day) << mk("bump1", "bumper", 8, 0, 0, m_now - 10 * day);
    // autogen rotation (news)
    m_pool << mk("w_old", "weather", 90, 0, 0, m_now - 7200000, "autogen rotation") << mk("w_new", "weather", 95, 0, 0, m_now - 600000, "autogen rotation");
    // series GNB
    m_pool << mk("gnb1", "program", 1700, 0, 0, m_now - 14 * day, "", "GNB", QDate(2026, 10, 11)) << mk("gnb2", "program", 1700, 0, 0, m_now - 7 * day, "", "GNB", QDate(2026, 10, 18));
}

void TestProgramming::queryPreferOrders() {
    QueryContext ctx; ctx.nowMs = m_now;
    QueryBlock b; b.types = {"program"}; b.prefer = "least_played"; b.noRepeatMin = 0;
    auto c = QueryBlocks::candidates(m_pool, b, ctx, 10);
    QVERIFY(c.size() >= 4);
    QCOMPARE(c.first().media.playCount, 0);                 // plong / gnb never played
    b.prefer = "most_played"; c = QueryBlocks::candidates(m_pool, b, ctx, 10); QCOMPARE(c.first().media.id, QString("p1"));
    b.prefer = "newest"; c = QueryBlocks::candidates(m_pool, b, ctx, 10); QCOMPARE(c.first().media.id, QString("plong"));
    b.prefer = "oldest"; c = QueryBlocks::candidates(m_pool, b, ctx, 10); QCOMPARE(c.first().media.id, QString("p1"));
    b.prefer = "longest"; c = QueryBlocks::candidates(m_pool, b, ctx, 10); QCOMPARE(c.first().media.id, QString("plong"));
    // category filter reaches the autogen rotation regardless of type list
    QueryBlock g; g.types = {"news", "weather"}; g.categories = {"Autogen Rotation"}; g.prefer = "newest";
    c = QueryBlocks::candidates(m_pool, g, ctx, 10); QCOMPARE(c.size(), 2); QCOMPARE(c.first().media.id, QString("w_new"));
}

void TestProgramming::queryTargetCoverageAndExclusion() {
    QueryContext ctx; ctx.nowMs = m_now; ctx.exclude = {"ad0"};
    QueryBlock b; b.types = {"ad"}; b.targetSec = 120; b.prefer = "least_played"; b.noRepeatMin = 0;
    QString why; auto picks = QueryBlocks::resolve(m_pool, b, ctx, &why);
    QCOMPARE(picks.size(), 4);                               // 4 x 30 s = 120 s
    double tot = 0; QSet<QString> ids; for (const auto& p : picks) { tot += p.media.durationSec; QVERIFY(!ids.contains(p.media.id)); ids.insert(p.media.id); QVERIFY(p.media.id != "ad0"); }
    QCOMPARE(tot, 120.0);
    // a program block of 10 minutes takes the best program even though it overruns (first pick), flagged
    QueryBlock pb; pb.types = {"program"}; pb.targetSec = 600; pb.prefer = "least_played"; pb.noRepeatMin = 0;
    picks = QueryBlocks::resolve(m_pool, pb, ctx, &why); QCOMPARE(picks.size(), 1); QVERIFY(picks.first().overrun);
}

void TestProgramming::queryRepeatsRankLast() {
    QueryContext ctx; ctx.nowMs = m_now;
    QueryBlock b; b.types = {"ad"}; b.prefer = "least_played"; b.noRepeatMin = 30;   // ad0/ad1 aired 10 min ago
    auto c = QueryBlocks::candidates(m_pool, b, ctx, 10);
    QCOMPARE(c.size(), 8);
    QVERIFY(!c.first().repeat);
    QVERIFY(c[6].repeat && c[7].repeat);                     // the two recent ones sink to the bottom but stay eligible
}

void TestProgramming::seriesBlockPositional() {
    QueryContext ctx; ctx.nowMs = m_now; ctx.exclude = {"gnb2"};  // exclusions are ignored for series blocks
    QueryBlock latest; latest.series = {"GNB"}; latest.nthLatest = 0;
    QueryBlock prev = latest; prev.nthLatest = 1;
    QCOMPARE(QueryBlocks::resolve(m_pool, latest, ctx).first().media.id, QString("gnb2"));
    QCOMPARE(QueryBlocks::resolve(m_pool, prev, ctx).first().media.id, QString("gnb1"));
}

void TestProgramming::folderBlockOrdered() {
    QueryContext ctx; ctx.nowMs = m_now;
    FolderBlock f; f.folder = "D:/media/ad"; f.order = "name"; f.targetSec = 0;
    QString why; auto p = QueryBlocks::resolveFolder(m_pool, f, ctx, &why);
    QCOMPARE(p.size(), 1);
    // round robin: the item after the most recently played (ad0 at -10 min is the latest... ad1 also -10 min; name order ad0..ad7 → after the latest-played index)
    QVERIFY(p.first().media.id.startsWith("ad"));
    FolderBlock byName; byName.folder = "ad"; byName.order = "random"; byName.targetSec = 90;      // bare folder name matches the parent dir
    p = QueryBlocks::resolveFolder(m_pool, byName, ctx, &why); QCOMPARE(p.size(), 3);
    FolderBlock none; none.folder = "D:/nowhere"; QVERIFY(QueryBlocks::resolveFolder(m_pool, none, ctx, &why).isEmpty()); QVERIFY(!why.isEmpty());
}

void TestProgramming::recipeLaysUntilFull() {
    HourRecipe r = FormatClock::defaultRecipe();              // 10+5+3+1 = 19 min cycle
    HourSlot empty;
    HourLayout lay = FormatClock::layRecipe(r, empty, true);
    QVERIFY(lay.placedSec >= 3540.0 && lay.placedSec <= 3630.0);
    QVERIFY(lay.items.size() >= 12);
    for (const auto& it : lay.items) QVERIFY(it.isQuery());
    QCOMPARE(QueryBlock::fromJson(lay.items.first().queryBlock).types, QStringList{"program"});
    // locks survive: a locked anchor at :30 stays, at the boundary nearest 1800 s
    HourSlot ex; SlotItem locked; locked.id = "gen"; locked.mediaId = "w_new"; locked.title = "GEN 1"; locked.type = "weather"; locked.durationSec = 95; locked.locked = true; locked.lockAtSec = 1800; ex.items << locked;
    lay = FormatClock::layRecipe(r, ex, true);
    int at = -1; double run = 0; for (int i = 0; i < lay.items.size(); ++i) { if (lay.items[i].id == "gen") { at = i; break; } run += FormatClock::itemSec(lay.items[i]); }
    QVERIFY(at > 0); QVERIFY(std::fabs(run - 1800.0) <= 300.0 + 1.0);   // within half a block of :30
}

void TestProgramming::adBreakRecipeShape() {
    QJsonObject spec; spec["programMin"] = 12; spec["adMin"] = 3; spec["idEveryBreak"] = true; spec["idAtTop"] = true;
    HourRecipe r = FormatClock::adBreakRecipe(spec);
    QCOMPARE(r.parts.size(), 3);                              // program, ID, break
    QCOMPARE(r.parts[0].types, QStringList{"program"}); QCOMPARE(r.parts[0].minutes, 12.0);
    QCOMPARE(r.parts[2].minutes, 3.0); QVERIFY(r.parts[2].types.contains("ad"));
    QVERIFY(r.idAtTop);
    HourLayout lay = FormatClock::layRecipe(r, HourSlot(), false);
    QVERIFY(lay.items.first().title.contains("Station ID"));
    QVERIFY(lay.placedSec >= 3540.0);
}

void TestProgramming::carryLocksSeatsAnchors() {
    QList<SlotItem> dest; for (int i = 0; i < 6; ++i) { SlotItem it; it.id = QString("d%1").arg(i); it.mediaId = QString("p%1").arg(i); it.type = "program"; it.durationSec = 600; dest << it; }
    QList<SlotItem> src; SlotItem lead; lead.id = "lead"; lead.mediaId = "id1"; lead.durationSec = 10; lead.locked = true; src << lead;
    SlotItem anc; anc.id = "anc"; anc.mediaId = "w_new"; anc.durationSec = 95; anc.locked = true; anc.lockAtSec = 1250; src << anc;
    const QList<SlotItem> out = FormatClock::carryLocks(dest, src);
    QCOMPARE(out.first().id, QString("lead"));
    int idx = -1; for (int i = 0; i < out.size(); ++i) if (out[i].id == "anc") idx = i;
    QCOMPARE(idx, 3);                                        // 10 + 600 + 600 = 1210 is the boundary nearest 1250
}

void TestProgramming::weightedFillShares() {
    QList<WeightBucket> b = FormatClock::defaultBuckets();  // 60 programs / 30 ads / 10 ids
    // the shared pool is too small to fill an hour at these shares: give ads and IDs enough depth
    QList<MediaRecord> pool = m_pool;
    for (int i = 0; i < 40; ++i) pool << mk(QString("fadl%1").arg(i), "ad", 30, 0, 0, m_now - (i + 1) * 3600000LL);
    for (int i = 0; i < 20; ++i) pool << mk(QString("fid%1").arg(i), i % 2 ? "promo" : "station_id", 15, 0, 0, m_now - (i + 1) * 3600000LL);
    auto st = FormatClock::beginWeighted(b, pool, "main", m_now, 7);
    HourLayout lay = FormatClock::fillWeighted(b, st, HourSlot(), true, true, 2400.0);
    QVERIFY(lay.placedSec >= 3550.0 && lay.placedSec <= 3640.0);
    int programs = 0; double adSec = 0, idSec = 0, progSec = 0; for (const auto& it : lay.items) { const double d = FormatClock::itemSec(it); if (it.type == "program") { ++programs; progSec += d; } else if (it.type == "ad") adSec += d; else idSec += d; }
    QCOMPARE(programs, 1);                                   // one program per hour
    QVERIFY(adSec > 0 && idSec > 0);
    QVERIFY(progSec <= 2400.0 + 0.5);                        // capped, never a whole 55-minute program
    // the long program never enters whole: either it is split (outPoint set) or a 28-min one leads
    for (const auto& it : lay.items) if (it.mediaId == "plong") QVERIFY(it.outPointSec > 0 && it.outPointSec <= 2400.0);
}

void TestProgramming::weightedFillCarriesLongProgram() {
    QList<WeightBucket> b; WeightBucket p; p.label = "Long"; p.types = {"program"}; p.series = ""; p.pct = 100; b << p;
    QList<MediaRecord> pool; pool << mk("plong", "program", 3300, 0, 0, m_now - 86400000LL);
    auto st = FormatClock::beginWeighted(b, pool, "main", m_now, 1);
    HourLayout h1 = FormatClock::fillWeighted(b, st, HourSlot(), true, true, 2400.0);
    QCOMPARE(h1.items.size(), 1); QCOMPARE(h1.items.first().outPointSec, 2400.0); QVERIFY(!h1.carry.isEmpty());
    HourLayout h2 = FormatClock::fillWeighted(b, st, HourSlot(), true, true, 2400.0);
    QVERIFY(!h2.items.isEmpty()); QCOMPARE(h2.items.first().inPointSec, 2400.0); QVERIFY(h2.items.first().title.contains("cont."));
    QVERIFY(h2.carry.isEmpty());
}

void TestProgramming::contractPacing() {
    AdContract c; c.id = "c1"; c.mediaIds = {"ad3"}; c.spotsTotal = 100; c.startDate = QDate(2026, 10, 13); c.endDate = QDate(2026, 10, 22); c.status = "active";
    const QTimeZone tz("America/New_York");
    int aired = 20;
    auto count = [&](const QString&, qint64, qint64) { return aired; };
    QHash<QString, double> p = FormatClock::contractPacing({c}, "main", QDate(2026, 10, 22), tz, count);
    QVERIFY(p.value("ad3") > 0);                               // behind: 20 of 100 on the last day
    aired = 100; p = FormatClock::contractPacing({c}, "main", QDate(2026, 10, 22), tz, count);
    QVERIFY(p.value("ad3") < 0);                               // fulfilled: back off
    c.channels = {"other"}; p = FormatClock::contractPacing({c}, "main", QDate(2026, 10, 22), tz, count); QVERIFY(!p.contains("ad3"));
}

void TestProgramming::builderExpandsBlocksWithCache() {
    HourSlot s; s.channelId = "main"; s.id = "3-12"; s.dayOfWeek = 3; s.hour = 12;
    SlotItem q; q.id = "blk"; q.title = "ads"; q.type = "ad"; q.durationSec = 120; QJsonObject qb; qb["types"] = QJsonArray{"ad"}; qb["targetSec"] = 120; qb["prefer"] = "least_played"; qb["noRepeatMin"] = 0; q.queryBlock = qb; s.items << q;
    SlotItem fixed; fixed.id = "fx"; fixed.mediaId = "p2"; fixed.type = "program"; fixed.durationSec = 1680; s.items << fixed;
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, QTimeZone("America/New_York"));
    Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), o);
    int queryItems = 0; for (const auto& r : rl.items) if (r.origin == "query") ++queryItems;
    QCOMPARE(queryItems, 4);
    QVERIFY(cache.contains(QString::number(hourStart) + "#blk"));
    const QStringList first = cache.value(QString::number(hourStart) + "#blk");
    // a rebuild with a different seed situation keeps the same picks (cache)
    Runlist rl2 = RunlistBuilder::build(s, hourStart, lookup(), o);
    QStringList second; for (const auto& r : rl2.items) if (r.origin == "query") second << r.item.mediaId;
    QCOMPARE(second, first);
    // the fixed program follows the block on the clock
    QCOMPARE(rl.items.last().item.mediaId, QString("p2"));
    QCOMPARE(rl.items.last().plannedStartMs, hourStart + 120000);
}

void TestProgramming::builderSoftAnchorNearestBoundary() {
    HourSlot s; s.channelId = "main"; s.id = "3-13"; s.dayOfWeek = 3; s.hour = 13;
    for (int i = 1; i <= 3; ++i) { SlotItem it; it.id = QString("p%1").arg(i); it.mediaId = QString("p%1").arg(i); it.type = "program"; it.durationSec = 1680; s.items << it; }   // 0, 1680, 3360
    SlotItem anc; anc.id = "wx"; anc.mediaId = "w_new"; anc.type = "weather"; anc.durationSec = 95; anc.locked = true; anc.lockAtSec = 1500; anc.lockLabel = "GEN wx"; s.items << anc;
    BuildOptions o; o.nowMs = m_now; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 13, QTimeZone("America/New_York"));
    Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), o);
    int at = -1; for (int i = 0; i < rl.items.size(); ++i) if (rl.items[i].item.id == "wx") at = i;
    QVERIFY(at >= 0);
    QCOMPARE(rl.items[at].plannedStartMs, hourStart + 1680000);   // boundary nearest 1500 is 1680, nothing is cut
    QVERIFY(!rl.items[at - 1].truncated);
    QVERIFY(rl.items[at].note.startsWith("anchor"));
    QJsonObject j = rl.items[at].toJson(); QVERIFY(j.value("locked").toBool()); QCOMPARE(j.value("lockLabel").toString(), QString("GEN wx"));
}

// ---------------------------------------------------------------------------
// Regression tests for the Phase 9 review fixes
// ---------------------------------------------------------------------------

QList<MediaRecord> TestProgramming::deepPool() {
    QList<MediaRecord> pool = m_pool;
    for (int i = 0; i < 100; ++i) pool << mk(QString("fadl%1").arg(i), "ad", 30, 0, 0, m_now - (i + 10) * 3600000LL);
    for (int i = 0; i < 20; ++i) pool << mk(QString("fid%1").arg(i), i % 2 ? "promo" : "station_id", 15, 0, 0, m_now - (i + 1) * 3600000LL);
    return pool;
}

static SlotItem adBlock(const QString& id, const QString& type, double targetSec, const QString& prefer = "least_played") {
    SlotItem q; q.id = id; q.title = id; q.type = type; q.durationSec = targetSec;
    QJsonObject qb; qb["types"] = QJsonArray{type}; qb["targetSec"] = targetSec; qb["prefer"] = prefer; qb["noRepeatMin"] = 0; q.queryBlock = qb;
    return q;
}

void TestProgramming::leastPlayedLongestSinceAiredWins() {
    // Equal play counts: the one that aired longest ago comes first, whatever its import date (the added-date term must not
    // swamp hours of recency: A was added 3 years ago, B yesterday).
    const qint64 day = 86400000LL, hour = 3600000LL;
    QList<MediaRecord> pool;
    pool << mk("A", "ad", 30, 1, m_now - 5 * hour, m_now - 3 * 365 * day) << mk("B", "ad", 30, 1, m_now - 1 * hour, m_now - 1 * day);
    QueryBlock b; b.types = {"ad"}; b.prefer = "least_played"; b.noRepeatMin = 0;
    QueryContext ctx; ctx.nowMs = m_now; ctx.seed = 5;
    auto c = QueryBlocks::candidates(pool, b, ctx, 10);
    QCOMPARE(c.size(), 2);
    QCOMPARE(c[0].media.id, QString("A"));
    // ... and a never-aired item still beats both, newer imports win the remaining tie
    pool << mk("N1", "ad", 30, 1, 0, m_now - 10 * day) << mk("N2", "ad", 30, 1, 0, m_now - 2 * day);
    c = QueryBlocks::candidates(pool, b, ctx, 10);
    QCOMPARE(c[0].media.id, QString("N2")); QCOMPARE(c[1].media.id, QString("N1")); QCOMPARE(c[2].media.id, QString("A"));
}

void TestProgramming::orderedFolderStartsAtItsFirstItem() {
    QList<MediaRecord> pool;
    for (const char* n : {"a_first", "b_second", "c_third"}) { MediaRecord m = mk(n, "ad", 30, 0, 0, m_now); m.path = QString("D:/media/fold/%1.mp4").arg(n); pool << m; }
    FolderBlock f; f.folder = "D:/media/fold"; f.order = "name";
    QueryContext ctx; ctx.nowMs = m_now; ctx.seed = 3;
    auto p = QueryBlocks::resolveFolder(pool, f, ctx);
    QCOMPARE(p.size(), 1); QCOMPARE(p[0].media.id, QString("a_first"));            // nothing ever aired: episode 1 goes first
    pool[0].lastPlayedMs = m_now - 1000; pool[0].playCount = 1;
    p = QueryBlocks::resolveFolder(pool, f, ctx); QCOMPARE(p[0].media.id, QString("b_second"));   // then the round robin moves on
    pool[1].lastPlayedMs = m_now - 500; pool[2].lastPlayedMs = m_now - 100;
    p = QueryBlocks::resolveFolder(pool, f, ctx); QCOMPARE(p[0].media.id, QString("a_first"));    // after the last one it wraps
}

void TestProgramming::lockAtSecNegativeMeansNoAnchor() {
    QJsonObject o; o["id"] = "x"; o["mediaId"] = "ad0"; o["type"] = "ad"; o["duration"] = 30; o["isLocked"] = false; o["lockAtSec"] = -1;
    SlotItem i = SlotItem::fromJson(o);
    QVERIFY(!i.locked); QCOMPARE(i.lockAtSec, -1.0); QCOMPARE(i.anchorSec(), -1.0);   // what both editors send to unlock
    SlotItem j = SlotItem::fromJson(i.toJson());
    QVERIFY(!j.locked); QCOMPARE(j.lockAtSec, -1.0);
    o["lockAtSec"] = 600;                                                              // explicit unlock beats a remembered anchor
    i = SlotItem::fromJson(o); QVERIFY(!i.locked); QCOMPARE(i.lockAtSec, 600.0); QCOMPARE(i.anchorSec(), -1.0);
    o.remove("isLocked");                                                              // an anchor from a producer that sent no flag locks
    i = SlotItem::fromJson(o); QVERIFY(i.locked); QCOMPARE(i.anchorSec(), 600.0);
    o["isLocked"] = true; o["lockAtSec"] = -1;
    i = SlotItem::fromJson(o); QVERIFY(i.locked); QCOMPARE(i.lockAtSec, -1.0);         // locked without an anchor keeps its place
    // an item the operator un-locked and left last stays last: it is not re-seated at :00
    QJsonObject un; un["id"] = "x"; un["mediaId"] = "ad0"; un["type"] = "ad"; un["duration"] = 30; un["isLocked"] = false; un["lockAtSec"] = -1;
    HourSlot s; s.channelId = "main"; s.id = "3-16"; s.dayOfWeek = 3; s.hour = 16;
    for (int k = 1; k <= 2; ++k) { SlotItem p; p.id = QString("p%1").arg(k); p.mediaId = QString("p%1").arg(k); p.type = "program"; p.durationSec = 1680; s.items << p; }
    s.items << SlotItem::fromJson(un);
    BuildOptions bo; bo.nowMs = m_now; bo.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 16, QTimeZone("America/New_York"));
    const Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), bo);
    QCOMPARE(rl.items.last().item.id, QString("x"));
    QCOMPARE(rl.items.first().item.id, QString("p1"));
}

void TestProgramming::synthesizedIdsAreDeterministic() {
    HourRecipe r = FormatClock::defaultRecipe();
    const HourLayout a = FormatClock::layRecipe(r, HourSlot(), false, 200, "af-1");
    const HourLayout b = FormatClock::layRecipe(r, HourSlot(), false, 200, "af-1");
    QStringList ia, ib; for (const auto& it : a.items) ia << it.id; for (const auto& it : b.items) ib << it.id;
    QCOMPARE(ia, ib);
    QVERIFY(ia.first().startsWith("af-1-"));
    QCOMPARE(QSet<QString>(ia.begin(), ia.end()).size(), ia.size());                   // unique within the hour
    const HourLayout c = FormatClock::layRecipe(r, HourSlot(), false);                 // no prefix: random ids, as before
    QStringList ic; for (const auto& it : c.items) ic << it.id;
    QVERIFY(ic != ia);
    // weighted fill: same inputs, same prefix -> same ids (and the same media)
    const QList<WeightBucket> wb = FormatClock::defaultBuckets();
    const QList<MediaRecord> pool = deepPool();
    auto s1 = FormatClock::beginWeighted(wb, pool, "main", m_now, 7), s2 = FormatClock::beginWeighted(wb, pool, "main", m_now, 7);
    const HourLayout w1 = FormatClock::fillWeighted(wb, s1, HourSlot(), true, true, 2400.0, "af-2");
    const HourLayout w2 = FormatClock::fillWeighted(wb, s2, HourSlot(), true, true, 2400.0, "af-2");
    QCOMPARE(w1.items.size(), w2.items.size());
    for (int i = 0; i < w1.items.size(); ++i) { QCOMPARE(w1.items[i].id, w2.items[i].id); QCOMPARE(w1.items[i].mediaId, w2.items[i].mediaId); QVERIFY(w1.items[i].id.startsWith("af-2-w")); }
}

void TestProgramming::autofillRecipeKeepsItsIdsAcrossRebuilds() {
    ExecEnv env;
    QVERIFY(env.init(deepPool(), QJsonObject{{"autofill", QJsonObject{{"enabled", true}, {"mode", "recipe"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    const qint64 h11 = env.at(11, 0);
    QString id11;
    for (const QJsonValue& v : env.link.pushes[0].items) { const QJsonObject o = v.toObject(); if (qint64(o.value("plannedUtcMs").toDouble()) == h11) { id11 = o.value("id").toString(); break; } }
    QVERIFY(!id11.isEmpty());
    // 11:00:00.3: the engine already cut to the first item of hour 11 (pushed at 10:30 with a hard start); the rebuild must
    // recognise it, not re-open it with a join-in-progress replace
    env.now = env.at(11, 0, 0, 300);
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", id11}});
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), 2);
    QVERIFY2(!env.link.pushes.last().replace, "the on-air item must keep airing, not be replaced");
    for (const QJsonValue& v : env.link.pushes.last().items) QVERIFY(v.toObject().value("id").toString() != id11);
    // a settings write bumps the store revision for every channel: the rebuild must produce the very same playlist
    QVERIFY(env.store.setSetting("spots.jobs", "{}"));
    env.now = env.at(11, 20);
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), 2);
}

void TestProgramming::autofillWeightedIsFrozenPerHour() {
    ExecEnv env;
    QVERIFY(env.init(deepPool(), QJsonObject{{"autofill", QJsonObject{{"enabled", true}, {"mode", "weighted"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    // items air: the rotation sorts on play counts, which move. The hour as laid must not.
    for (const QJsonValue& v : env.link.pushes[0].items) env.store.recordPlay(v.toObject().value("assetId").toString(), env.now);
    QVERIFY(env.store.setSetting("spots.jobs", "{}"));
    env.now = env.at(10, 40);
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), 1);
}

void TestProgramming::autofillWeightedCarriesLongProgramIntoNextHour() {
    QList<MediaRecord> pool; for (const MediaRecord& m : deepPool()) if (m.type != "program") pool << m;
    pool << mk("movie", "program", 3300, 0, 0, m_now - 86400000LL);
    ExecEnv env;
    QVERIFY(env.init(pool, QJsonObject{{"autofill", QJsonObject{{"enabled", true}, {"mode", "weighted"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    bool leading = false;
    for (const QJsonValue& v : env.items("current")) { const QJsonObject o = v.toObject(); if (o.value("mediaId").toString() == "movie") { leading = true; QCOMPARE(o.value("outPoint").toDouble(), 2400.0); } }
    QVERIFY(leading);
    const QJsonArray next = env.items("next");
    QVERIFY(!next.isEmpty());
    const QJsonObject first = next.first().toObject();
    QCOMPARE(first.value("mediaId").toString(), QString("movie"));                     // the remainder leads the next hour, not lost
    QCOMPARE(first.value("inPoint").toDouble(), 2400.0);
    QVERIFY(first.value("title").toString().contains("cont."));
    int movies = 0; for (const QJsonValue& v : next) if (v.toObject().value("mediaId").toString() == "movie") ++movies;
    QCOMPARE(movies, 1);                                                               // and it is not picked again from 0
}

void TestProgramming::fixedSourceChannelAirsOneEndlessItem() {
    ExecEnv env;
    QVERIFY(env.init(m_pool, QJsonObject{{"fixedSource", QJsonObject{{"type", "ndi"}, {"name", "STUDIO (CAM1)"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    QCOMPARE(env.link.pushes.size(), 1);
    QVERIFY(env.link.pushes[0].replace);
    QCOMPARE(env.link.pushes[0].items.size(), 1);          // not a second item with a hard start at :00 (a second receiver and a cut)
    const QString id = env.link.pushes[0].items[0].toObject().value("id").toString();
    QCOMPARE(env.link.pushes[0].items[0].toObject().value("source").toObject().value("ndiName").toString(), QString("STUDIO (CAM1)"));
    // 11:00:00.3: the hour changed, the looping source is still the same item
    env.now = env.at(11, 0, 0, 300);
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", id}, {"position", 1800.3}});
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), 1);                   // the clock is not a new playlist
    bool replace = true;
    QVERIFY(env.ex->plannedPlaylist("main", env.now, &replace).isEmpty());
    QVERIFY(!replace);
    QCOMPARE(env.items("current").first().toObject().value("id").toString(), id);
    QCOMPARE(env.ex->status("main").driftSec, 0.0);        // an endless loop has no schedule position to drift from
    // a changed source is a different item and replaces the on-air one
    ChannelProfile p = env.store.channel("main");
    QJsonObject fs = p.extra.value("fixedSource").toObject(); fs["name"] = "STUDIO (CAM2)"; p.extra["fixedSource"] = fs;
    QVERIFY(env.store.saveChannel(p));
    env.ex->reloadChannel("main");
    QCOMPARE(env.link.pushes.size(), 2);
    QVERIFY(env.link.pushes.last().replace);
    QCOMPARE(env.link.pushes.last().items.size(), 1);
    QCOMPARE(env.link.pushes.last().items[0].toObject().value("source").toObject().value("ndiName").toString(), QString("STUDIO (CAM2)"));
}

void TestProgramming::overrunIsCutToTheBlockTarget() {
    // Every program is 28+ minutes: a "10 minute programs" block takes the best one anyway, flagged, and cuts it to 10 minutes
    HourSlot s; s.channelId = "main"; s.id = "3-14"; s.dayOfWeek = 3; s.hour = 14;
    for (int i = 0; i < 3; ++i) s.items << adBlock(QString("pb%1").arg(i), "program", 600);
    s.items << adBlock("ads", "ad", 120);
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 14, QTimeZone("America/New_York"));
    for (int pass = 0; pass < 2; ++pass) {                 // pass 1 re-uses the cached picks: they must be cut the same way
        const Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), o);
        QCOMPARE(rl.items.size(), 7);                      // 3 programs + 4 ads, nothing pushed past the hour
        for (int i = 0; i < 3; ++i) {
            const RunlistItem& r = rl.items[i];
            QVERIFY(r.item.extra.value("overrun").toBool());
            QCOMPARE(r.durationSec, 600.0); QCOMPARE(r.outSec, 600.0);
            QCOMPARE(r.plannedStartMs, hourStart + i * 600000LL);
            QVERIFY2(r.note.startsWith("cut to the block target"), qPrintable(r.note));
        }
        QCOMPARE(rl.items[3].plannedStartMs, hourStart + 1800000LL);
        for (const QString& n : rl.notes) QVERIFY2(!n.contains("do not fit"), qPrintable(n));
    }
    // fitToTarget=false opts out of fitting: the whole item airs (flagged, not cut)
    HourSlot s2; s2.channelId = "main"; s2.id = "3-15"; s2.dayOfWeek = 3; s2.hour = 15;
    SlotItem loose = adBlock("loose", "program", 600); QJsonObject qb = loose.queryBlock; qb["fitToTarget"] = false; loose.queryBlock = qb; s2.items << loose;
    const Runlist rl2 = RunlistBuilder::build(s2, hourStart + 3600000LL, lookup(), o);
    QVERIFY(!rl2.items.isEmpty()); QVERIFY(rl2.items[0].durationSec > 1000.0);
}

void TestProgramming::recipeClosesTheHourWithAnInterstitial() {
    const HourRecipe r = FormatClock::defaultRecipe();
    const HourLayout lay = FormatClock::layRecipe(r, HourSlot(), false);
    QVERIFY(!lay.items.isEmpty());
    const QueryBlock last = QueryBlock::fromJson(lay.items.last().queryBlock);
    QVERIFY2(!last.types.contains("program"), "the last block must not be a program that is cut at :00");
    QCOMPARE(last.targetSec, 180.0);                       // the 3-minute gap is closed by the 3-minute autogen part
    QVERIFY(lay.placedSec >= 3590.0 && lay.placedSec <= 3610.0);
    // a programs-only recipe has nothing else to close with: it keeps its own part
    HourRecipe only; only.id = "p"; only.name = "p"; RecipePart a; a.label = "Programs"; a.types = {"program"}; a.minutes = 25; only.parts << a;
    const HourLayout lay2 = FormatClock::layRecipe(only, HourSlot(), false);
    QVERIFY(QueryBlock::fromJson(lay2.items.last().queryBlock).types.contains("program"));
    // the whole hour fits on the clock: no "do not fit" tail
    HourSlot s; s.channelId = "main"; s.id = "3-15"; s.dayOfWeek = 3; s.hour = 15; s.items = lay.items;
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const Runlist rl = RunlistBuilder::build(s, RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 15, QTimeZone("America/New_York")), lookupOf(deepPool()), o);
    for (const QString& n : rl.notes) QVERIFY2(!n.contains("do not fit"), qPrintable(n));
    QVERIFY(rl.scheduledSec <= 3600.0);
}

void TestProgramming::rerollMovesOnFromPreviousPicks() {
    HourSlot s; s.channelId = "main"; s.id = "3-12"; s.dayOfWeek = 3; s.hour = 12;
    s.items << adBlock("blk", "ad", 120);                  // 4 of the 8 ads, least played
    QHash<QString, QStringList> cache, avoid;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.avoidPicks = &avoid; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, QTimeZone("America/New_York"));
    const QString key = QString::number(hourStart) + "#blk";
    RunlistBuilder::build(s, hourStart, lookup(), o);
    const QStringList first = cache.value(key);
    QCOMPARE(first.size(), 4);
    // exactly what ScheduleExecutor::rerollBlocks does: forget the picks, remember them, bump the salt
    avoid.insert(key, first); cache.clear(); o.seedSalt = 1;
    RunlistBuilder::build(s, hourStart, lookup(), o);
    const QStringList second = cache.value(key);
    QCOMPARE(second.size(), 4);
    for (const QString& id : second) QVERIFY2(!first.contains(id), qPrintable(id));   // a deterministic ranking moved on
    QVERIFY(!avoid.contains(key));                          // consumed once the block re-resolved
    // the picks are stable again afterwards
    RunlistBuilder::build(s, hourStart, lookup(), o);
    QCOMPARE(cache.value(key), second);
    // a tiny pool still resolves: when everything is avoided the previous picks come back rather than a hole in the hour
    HourSlot t; t.channelId = "main"; t.id = "3-13"; t.dayOfWeek = 3; t.hour = 13; t.items << adBlock("only", "bumper", 8);
    QHash<QString, QStringList> c2, a2; BuildOptions o2; o2.nowMs = m_now; o2.pickCache = &c2; o2.avoidPicks = &a2; o2.fillGaps = false;
    const qint64 h13 = hourStart + 3600000LL; const QString k2 = QString::number(h13) + "#only";
    RunlistBuilder::build(t, h13, lookup(), o2);
    QCOMPARE(c2.value(k2), QStringList{"bump1"});
    a2.insert(k2, c2.value(k2)); c2.clear();
    RunlistBuilder::build(t, h13, lookup(), o2);
    QCOMPARE(c2.value(k2), QStringList{"bump1"});
}

void TestProgramming::executorRerollKeepsStartedBlocks() {
    ExecEnv env;
    QVERIFY(env.init(deepPool(), QJsonObject{{"autofill", QJsonObject{{"enabled", true}, {"mode", "recipe"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    const QStringList curStartedBefore = ExecEnv::mediaIds(env.items("current"), env.now);
    const QStringList nextBefore = ExecEnv::mediaIds(env.items("next"));
    QVERIFY(curStartedBefore.size() >= 4);
    QVERIFY(nextBefore.size() >= 8);
    env.ex->rerollBlocks("main");
    env.now += 1000;
    env.ex->tick();
    const QStringList curStartedAfter = ExecEnv::mediaIds(env.items("current"), env.at(10, 30));
    const QStringList nextAfter = ExecEnv::mediaIds(env.items("next"));
    QCOMPARE(curStartedAfter, curStartedBefore);            // what is on air / aired keeps its picks
    QVERIFY2(nextAfter != nextBefore, "re-roll must give the upcoming blocks different picks");
    // ... and it is stable until the next re-roll
    env.store.setSetting("spots.jobs", "{}");
    env.now += 1000;
    env.ex->tick();
    QCOMPARE(ExecEnv::mediaIds(env.items("next")), nextAfter);
}

void TestProgramming::pickCacheIsValidated() {
    HourSlot s; s.channelId = "main"; s.id = "3-12"; s.dayOfWeek = 3; s.hour = 12;
    s.items << adBlock("blk", "ad", 120);
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, QTimeZone("America/New_York"));
    const QString key = QString::number(hourStart) + "#blk";
    Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), o);
    const QStringList ads = cache.value(key);
    QCOMPARE(ads.size(), 4);
    // an explicit item with the same media added later: the block must not air it a second time
    HourSlot t = s; SlotItem fx; fx.id = "fx"; fx.mediaId = ads.first(); fx.type = "ad"; fx.durationSec = 30; t.items << fx;
    rl = RunlistBuilder::build(t, hourStart, lookup(), o);
    int n = 0; for (const auto& r : rl.items) if (r.item.mediaId == ads.first()) ++n;
    QCOMPARE(n, 1);
    // the block edited in place (same item id, new spec): the old ad picks must not shadow it
    HourSlot u = s; SlotItem edited = adBlock("blk", "promo", 60); u.items[0] = edited;
    rl = RunlistBuilder::build(u, hourStart, lookup(), o);
    QVERIFY(!rl.items.isEmpty());
    for (const auto& r : rl.items) QCOMPARE(r.item.type, QString("promo"));
    // an unchanged block keeps its cache entry (and so its picks) across rebuilds
    Runlist again = RunlistBuilder::build(u, hourStart, lookup(), o);
    QCOMPARE(again.items.size(), rl.items.size());
    for (int i = 0; i < again.items.size(); ++i) QCOMPARE(again.items[i].item.mediaId, rl.items[i].item.mediaId);
}

void TestProgramming::cachedBlocksDoNotReadTheLibrary() {
    HourSlot s; s.channelId = "main"; s.id = "3-12"; s.dayOfWeek = 3; s.hour = 12;
    s.items << adBlock("a", "ad", 120) << adBlock("b", "promo", 45) << adBlock("c", "program", 600);
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, QTimeZone("America/New_York"));
    int allCalls = 0;      // any read of the library: the whole pool or a typed subset
    MediaLookup ml = lookup(); const auto all = ml.all; const auto byTypes = ml.byTypes;
    ml.all = [&allCalls, all]() { ++allCalls; return all(); };
    ml.byTypes = [&allCalls, byTypes](const QStringList& t) { ++allCalls; return byTypes(t); };
    const Runlist cold = RunlistBuilder::build(s, hourStart, ml, o);
    QVERIFY(allCalls >= 1);
    allCalls = 0;
    const Runlist warm = RunlistBuilder::build(s, hourStart, ml, o);
    QCOMPARE(allCalls, 0);                                  // every block resolved from its cached ids, no library scan
    QCOMPARE(warm.items.size(), cold.items.size());
    for (int i = 0; i < warm.items.size(); ++i) QCOMPARE(warm.items[i].runlistId, cold.items[i].runlistId);
}

void TestProgramming::weightedHardStartLockIsPlacedAtItsOffset() {
    // A locked hard-start item without lockAtSec (what "Lock" does to a hard-start item) is an anchor at its fixed offset,
    // not "the first thing in the hour".
    const QList<WeightBucket> b = FormatClock::defaultBuckets();
    auto st = FormatClock::beginWeighted(b, deepPool(), "main", m_now, 7);
    HourSlot ex; SlotItem news; news.id = "news"; news.mediaId = "w_new"; news.type = "weather"; news.durationSec = 95; news.locked = true;
    news.timingMode = "hard_start"; news.fixedOffsetSec = 1800; ex.items << news;
    SlotItem news2 = news; news2.id = "news2"; news2.mediaId = "w_old"; news2.fixedOffsetSec = 900; ex.items << news2;   // listed after, offset earlier
    const HourLayout lay = FormatClock::fillWeighted(b, st, ex, true, true, 2400.0);
    int at1 = -1, at2 = -1; double run1 = 0, run2 = 0, run = 0;
    for (int i = 0; i < lay.items.size(); ++i) {
        if (lay.items[i].id == "news2") { at2 = i; run2 = run; }
        if (lay.items[i].id == "news") { at1 = i; run1 = run; }
        run += FormatClock::itemSec(lay.items[i]);
    }
    QVERIFY(at1 > 0 && at2 > 0);
    QVERIFY(at2 < at1);                                     // ordered by offset
    QVERIFY2(std::fabs(run2 - 900.0) <= 120.0, qPrintable(QString::number(run2)));
    QVERIFY2(std::fabs(run1 - 1800.0) <= 150.0, qPrintable(QString::number(run1)));
    QCOMPARE(FormatClock::lockOffset(news), 1800.0);
    news.timingMode = "soft_start"; news.fixedOffsetSec = -1; news.lockAtSec = 300;
    QCOMPARE(FormatClock::lockOffset(news), 300.0);
}

void TestProgramming::weightedRotationDoesNotBurnPrograms() {
    QList<WeightBucket> b; WeightBucket p; p.label = "Programs"; p.types = {"program"}; p.pct = 60; b << p;
    WeightBucket a; a.label = "Ads"; a.types = {"ad"}; a.pct = 40; b << a;
    QList<MediaRecord> pool;
    for (int i = 1; i <= 4; ++i) pool << mk(QString("s%1").arg(i), "program", 300, i - 1, 0, m_now - i * 3600000LL);   // play counts 0..3
    for (int i = 0; i < 120; ++i) pool << mk(QString("fad%1").arg(i), "ad", 30, 0, 0, m_now - (i + 1) * 3600000LL);
    auto st = FormatClock::beginWeighted(b, pool, "main", m_now, 7);
    QStringList aired;
    for (int h = 0; h < 8; ++h) {
        const HourLayout lay = FormatClock::fillWeighted(b, st, HourSlot(), true, true, 2400.0);
        int programs = 0;
        for (const auto& it : lay.items) if (it.type == "program") { aired << it.mediaId; ++programs; }
        QCOMPARE(programs, 1);
    }
    QCOMPARE(aired, (QStringList{"s1", "s2", "s3", "s4", "s1", "s2", "s3", "s4"}));   // every program gets its turn, none is skipped
}

void TestProgramming::multiCreativeContractPacesAsOne() {
    AdContract c; c.id = "c3"; c.mediaIds = {"a", "b", "c"}; c.spotsTotal = 90; c.spotsPerDay = 9;
    c.startDate = QDate(2026, 10, 20); c.endDate = QDate(2026, 10, 24); c.status = "active";
    const QTimeZone tz("America/New_York");
    // 30 airings per creative = 90 for the contract, 3 per creative today = 9: the contract is delivered
    int perCreativeTotal = 30, perCreativeToday = 3;
    auto count = [&](const QString&, qint64 from, qint64 to) { return (to - from) > 86400000LL ? perCreativeTotal : perCreativeToday; };
    QHash<QString, double> p = FormatClock::contractPacing({c}, "main", QDate(2026, 10, 22), tz, count);
    for (const char* id : {"a", "b", "c"}) QVERIFY2(p.value(id) < 0, qPrintable(QString("%1 %2").arg(id).arg(p.value(id))));
    // 10 per creative (30 of 90, 1 per creative = 3 of 9 today): behind pace, all three float up together
    perCreativeTotal = 10; perCreativeToday = 1;
    p = FormatClock::contractPacing({c}, "main", QDate(2026, 10, 22), tz, count);
    for (const char* id : {"a", "b", "c"}) QVERIFY(p.value(id) > 0);
    QCOMPARE(p.value("a"), p.value("b"));
}

void TestProgramming::autofillWeightedRerollMovesTheNextHour() {
    // The weighted layout is frozen per hour; a re-roll drops the upcoming one and lays it again without what it had.
    ExecEnv env;
    QVERIFY(env.init(deepPool(), QJsonObject{{"autofill", QJsonObject{{"enabled", true}, {"mode", "weighted"}}}}));
    env.now = env.at(10, 30);
    env.ex->onEngineConnected("main");
    const QStringList curBefore = ExecEnv::mediaIds(env.items("current"));
    const QStringList nextBefore = ExecEnv::mediaIds(env.items("next"));
    QVERIFY(curBefore.size() >= 4);
    QVERIFY(nextBefore.size() >= 4);
    env.ex->rerollBlocks("main");
    env.now += 1000;
    env.ex->tick();
    QCOMPARE(ExecEnv::mediaIds(env.items("current")), curBefore);   // the airing hour is concrete media on air: untouched
    const QStringList nextAfter = ExecEnv::mediaIds(env.items("next"));
    QVERIFY2(nextAfter != nextBefore, "re-roll must lay the upcoming hour with different media");
    for (const QString& id : nextAfter) if (id.startsWith("ad") || id.startsWith("fadl")) { QVERIFY2(!nextBefore.contains(id), qPrintable(id)); break; }   // the first ad moved on
    // ... and it is stable until the next re-roll
    env.store.setSetting("spots.jobs", "{}");
    env.now += 1000;
    env.ex->tick();
    QCOMPARE(ExecEnv::mediaIds(env.items("next")), nextAfter);
    // a Supervisor-level re-roll that only wants fresh inputs (fresh = false) keeps the layout
    env.ex->rerollBlocks("main", false);
    env.now += 1000;
    env.ex->tick();
    QCOMPARE(ExecEnv::mediaIds(env.items("next")), nextAfter);
}

void TestProgramming::typedPoolMatchesTypesCaseInsensitively() {
    // The library keeps `type` as the producer wrote it; a block's types match case-insensitively (QueryBlocks::inList), so the
    // typed read the builder uses instead of loading the whole library must too.
    ExecEnv env;
    QList<MediaRecord> pool;
    pool << mk("UP1", "Ad", 30, 0, 0, m_now - 3600000) << mk("UP2", "AD", 30, 0, 0, m_now - 7200000) << mk("lo1", "ad", 30, 1, 0, m_now - 86400000LL);
    QVERIFY(env.init(pool, QJsonObject()));
    QStringList got; for (const MediaRecord& m : env.store.mediaByType({"ad"})) got << m.id;
    got.sort(); QCOMPARE(got, (QStringList{"UP1", "UP2", "lo1"}));
    HourSlot s; s.channelId = "main"; s.id = "3-12"; s.dayOfWeek = 3; s.hour = 12;
    s.items << adBlock("blk", "ad", 60);
    const Runlist rl = env.ex->previewSlot("main", s, RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, QTimeZone("America/New_York")));
    QStringList ids; for (const RunlistItem& r : rl.items) if (r.origin == "query") ids << r.item.mediaId;
    QCOMPARE(ids.size(), 2);                                 // two 30 s ads cover the 60 s block, never zero because of the case
    QVERIFY2(ids.contains("UP1") || ids.contains("UP2"), qPrintable(ids.join(",")));
}

void TestProgramming::conditionsAgeAndStackedRules() {
    const qint64 day = 86400000LL;
    QueryContext ctx; ctx.nowMs = m_now;
    // newer than 14 days: p2 (2 d) and plong (1 d), not p1 (40 d) or p3 (20 d); gnb1 is exactly 14 d (inclusive)
    QueryBlock b; b.types = {"program"}; b.noRepeatMin = 0; b.cond.maxAgeDays = 14;
    QSet<QString> ids; for (const auto& c : QueryBlocks::candidates(m_pool, b, ctx, 50)) ids.insert(c.media.id);
    QCOMPARE(ids, (QSet<QString>{"p2", "plong", "gnb1", "gnb2"}));
    // older than 14 days only
    b.cond = Conditions(); b.cond.minAgeDays = 15;
    ids.clear(); for (const auto& c : QueryBlocks::candidates(m_pool, b, ctx, 50)) ids.insert(c.media.id);
    QCOMPARE(ids, (QSet<QString>{"p1", "p3"}));

    // stacked + nested: (weather OR station_id) AND shorter than 60 s ... OR-group; then AND NOT tagged "expired"
    QList<MediaRecord> pool = m_pool;
    for (auto& m : pool) if (m.id == "w_old") m.extra["tags"] = QJsonArray{"expired"};
    QueryBlock r; r.noRepeatMin = 0;
    QJsonObject g; g["match"] = "any"; g["rules"] = QJsonArray{QJsonObject{{"field", "type"}, {"op", "is"}, {"value", "weather"}}, QJsonObject{{"field", "type"}, {"op", "is"}, {"value", "station_id"}}};
    r.cond.rules = QJsonArray{g, QJsonObject{{"field", "tag"}, {"op", "is"}, {"value", "expired"}, {"not", true}}, QJsonObject{{"field", "duration"}, {"op", "lt"}, {"value", 100}}};
    QVERIFY(r.isValid());                                    // rules alone make a valid block
    QVERIFY(r.cond.errors().isEmpty());
    ids.clear(); for (const auto& c : QueryBlocks::candidates(pool, r, ctx, 50)) ids.insert(c.media.id);
    QCOMPARE(ids, (QSet<QString>{"w_new", "id1"}));          // w_old expired; everything else wrong type
    // match any at the top level: weather OR played more than 4 times
    QueryBlock a; a.noRepeatMin = 0; a.cond.match = "any";
    a.cond.rules = QJsonArray{QJsonObject{{"field", "type"}, {"op", "is"}, {"value", "weather"}}, QJsonObject{{"field", "playCount"}, {"op", "gt"}, {"value", 4}}};
    ids.clear(); for (const auto& c : QueryBlocks::candidates(m_pool, a, ctx, 50)) ids.insert(c.media.id);
    QCOMPARE(ids, (QSet<QString>{"w_old", "w_new", "p1", "ad5", "ad6", "ad7"}));
    // JSON round trip keeps the conditions; bad rules are reported
    const QueryBlock rt = QueryBlock::fromJson(r.toJson());
    QCOMPARE(rt.cond.rules.size(), 3);
    Conditions bad; bad.rules = QJsonArray{QJsonObject{{"field", "colour"}, {"op", "is"}, {"value", "red"}}, QJsonObject{{"field", "title"}, {"op", "gt"}, {"value", 3}}};
    QCOMPARE(bad.errors().size(), 2);
    Q_UNUSED(day)
}

void TestProgramming::evenRotationCoversPoolBeforeRepeating() {
    // Six promos with very different lifetime play counts: "even" must still play each once before any plays twice.
    const qint64 day = 86400000LL;
    QList<MediaRecord> pool;
    for (int i = 0; i < 6; ++i) pool << mk(QString("pr%1").arg(i), "promo", 30, i == 0 ? 0 : 50 * i, i == 0 ? 0 : m_now - i * day, m_now - 100 * day);
    QueryBlock b; b.types = {"promo"}; b.prefer = "even"; b.noRepeatMin = 0;
    QStringList order;
    qint64 t = m_now;
    for (int k = 0; k < 12; ++k) {
        QueryContext ctx; ctx.nowMs = t; ctx.seed = 7;
        const auto picks = QueryBlocks::resolve(pool, b, ctx);
        QCOMPARE(picks.size(), 1);
        order << picks.first().media.id;
        for (auto& m : pool) if (m.id == picks.first().media.id) { m.playCount++; m.lastPlayedMs = t; }   // it aired
        t += 600000;
    }
    QCOMPARE(QSet<QString>(order.begin(), order.begin() + 6).size(), 6);       // first round: all six, no repeats
    QCOMPARE(order.mid(6), order.mid(0, 6));                                    // second round: same rotation
    // least_played would have kept replaying the never-played file first; even does not
    QCOMPARE(order.count("pr0"), 2);
}

void TestProgramming::evenRotationSpreadsAcrossHours() {
    // Two upcoming hours built back to back, before anything airs: the second hour must not take what the first one holds.
    QList<MediaRecord> pool;
    for (int i = 0; i < 4; ++i) pool << mk(QString("pp%1").arg(i), "promo", 30, 0, 0, m_now - 10 * 86400000LL);
    auto hourWith = [&](int h) {
        HourSlot s; s.channelId = "main"; s.id = QString("3-%1").arg(h); s.dayOfWeek = 3; s.hour = h;
        SlotItem q; q.id = "blk"; q.type = "promo"; q.durationSec = 60;
        QJsonObject qb; qb["types"] = QJsonArray{"promo"}; qb["targetSec"] = 60; qb["prefer"] = "even"; qb["noRepeatMin"] = 0; q.queryBlock = qb; s.items << q;
        return s;
    };
    QHash<QString, QStringList> cache;
    BuildOptions o; o.nowMs = m_now; o.pickCache = &cache; o.fillGaps = false;
    const QTimeZone tz("America/New_York");
    const Runlist a = RunlistBuilder::build(hourWith(12), RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 12, tz), lookupOf(pool), o);
    const Runlist b = RunlistBuilder::build(hourWith(13), RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 13, tz), lookupOf(pool), o);
    QSet<QString> ia, ib;
    for (const auto& r : a.items) if (r.origin == "query") ia.insert(r.item.mediaId);
    for (const auto& r : b.items) if (r.origin == "query") ib.insert(r.item.mediaId);
    QCOMPARE(ia.size(), 2); QCOMPARE(ib.size(), 2);
    QVERIFY2(!ia.intersects(ib), "the next hour repeated a file the airing hour already holds");
}

void TestProgramming::recipeFolderPartWithAgeAndAdBreaks() {
    // "Fill the hour from D:/media/program, newer than 14 days, with a 2-minute ad break every 28 minutes"
    QJsonObject prog; prog["label"] = "Shows"; prog["folder"] = "D:\\media\\program"; prog["minutes"] = 28; prog["prefer"] = "even"; prog["maxAgeDays"] = 14;
    QJsonObject brk; brk["label"] = "Break"; brk["types"] = QJsonArray{"ad"}; brk["minutes"] = 2; brk["prefer"] = "even";
    QJsonObject ro; ro["name"] = "Fill"; ro["parts"] = QJsonArray{prog, brk};
    const HourRecipe rec = HourRecipe::fromJson(ro);
    QCOMPARE(rec.parts.size(), 2);                               // a folder-only part is a real part
    QCOMPARE(rec.parts.first().cond.maxAgeDays, 14.0);
    const HourLayout lay = FormatClock::layRecipe(rec, HourSlot(), false);
    int shows = 0, breaks = 0;
    for (const SlotItem& it : lay.items) {
        const QueryBlock qb = QueryBlock::fromJson(it.queryBlock);
        if (qb.types.contains("ad")) { ++breaks; QCOMPARE(qb.targetSec, 120.0); continue; }
        ++shows;
        QCOMPARE(qb.cond.maxAgeDays, 14.0);
        QueryContext ctx; ctx.nowMs = m_now;
        for (const auto& c : QueryBlocks::candidates(m_pool, qb, ctx, 50)) {
            QVERIFY(c.media.path.startsWith("D:/media/program/"));
            QVERIFY(m_now - c.media.addedAtMs <= 14 * 86400000LL);
        }
    }
    QCOMPARE(shows, 2); QVERIFY(breaks >= 2);                    // 28 + 2 twice fills the hour

    // 15 + 2 leaves 9 min after three cycles: close with a shorter programme block and a normal 2-minute break,
    // never a 9-minute break.
    prog["minutes"] = 15; ro["parts"] = QJsonArray{prog, brk};
    const HourLayout lay2 = FormatClock::layRecipe(HourRecipe::fromJson(ro), HourSlot(), false);
    double total = 0; double longestBreak = 0;
    for (const SlotItem& it : lay2.items) { const QueryBlock qb = QueryBlock::fromJson(it.queryBlock); total += qb.targetSec; if (qb.types.contains("ad")) longestBreak = qMax(longestBreak, qb.targetSec); }
    QCOMPARE(longestBreak, 120.0);
    QVERIFY(QueryBlock::fromJson(lay2.items.last().queryBlock).types.contains("ad"));
    QVERIFY(qAbs(total - 3600.0) < 1.0);
}

void TestProgramming::builtinRecipesFillTheHour() {
    const QList<HourRecipe> all = FormatClock::builtinRecipes();
    QVERIFY(all.size() >= 10);
    QSet<QString> ids;
    for (const HourRecipe& r : all) {
        QVERIFY2(r.isValid(), qPrintable(r.name));
        QVERIFY2(r.id.startsWith("builtin-"), qPrintable(r.id));
        QVERIFY2(!ids.contains(r.id), qPrintable("duplicate id " + r.id)); ids.insert(r.id);
        // a JSON round trip keeps every part (age limits included)
        const HourRecipe rt = HourRecipe::fromJson(r.toJson());
        QCOMPARE(rt.parts.size(), r.parts.size());
        for (int i = 0; i < r.parts.size(); ++i) { QCOMPARE(rt.parts[i].cond.maxAgeDays, r.parts[i].cond.maxAgeDays); QCOMPARE(rt.parts[i].cond.minAgeDays, r.parts[i].cond.minAgeDays); }
        // every built-in lays a full hour (within the closing tolerance)
        const HourLayout lay = FormatClock::layRecipe(r, HourSlot(), false);
        double total = 0; for (const SlotItem& it : lay.items) total += it.durationSec;
        QVERIFY2(total >= 3570.0 && total <= 3631.0, qPrintable(QString("%1 lays %2 s").arg(r.name).arg(total)));
    }
    HourRecipe found;
    QVERIFY(FormatClock::findRecipe("builtin-fresh-14", {}, &found)); QCOMPARE(found.parts.first().cond.maxAgeDays, 14.0);
    QVERIFY(FormatClock::findRecipe("long form (55 + 5)", {}, &found)); QCOMPARE(found.id, QString("builtin-long-form"));
    QVERIFY(FormatClock::findRecipe("default", {}, &found)); QCOMPARE(found.id, QString("default"));
    QVERIFY(!FormatClock::findRecipe("no such recipe", {}, &found));
    // a saved recipe with a built-in's name wins over the built-in
    HourRecipe mine = all.first(); mine.id = "mine"; QVERIFY(FormatClock::findRecipe(all.first().name, {mine}, &found)); QCOMPARE(found.id, QString("mine"));
}

void TestProgramming::exhaustedBlockRepeatsInsteadOfGap() {
    // Two promos, both already placed this hour: the next promo block repeats (flagged) instead of resolving to nothing.
    QList<MediaRecord> pool;
    pool << mk("pa", "promo", 30, 3, m_now - 86400000LL, m_now - 30 * 86400000LL) << mk("pb", "promo", 30, 1, m_now - 2 * 86400000LL, m_now - 30 * 86400000LL);
    QueryContext ctx; ctx.nowMs = m_now; ctx.exclude = {"pa", "pb"};
    QueryBlock b; b.types = {"promo"}; b.prefer = "even"; b.targetSec = 60; b.noRepeatMin = 0;
    QString why;
    const auto picks = QueryBlocks::resolve(pool, b, ctx, &why);
    QCOMPARE(picks.size(), 2);
    for (const auto& p : picks) QVERIFY(p.repeat);
    QCOMPARE(picks.first().media.id, QString("pb"));        // even: longest since aired first, repeats keep the rotation order
    // a block whose filters match nothing still reports why
    QueryBlock none; none.types = {"station_id"};
    QVERIFY(QueryBlocks::resolve(pool, none, ctx, &why).isEmpty());
    QVERIFY(why.contains("no media matches"));
    // folder blocks fall back the same way
    FolderBlock f; f.folder = "D:/media/promo"; f.order = "even"; f.targetSec = 30;
    const auto fp = QueryBlocks::resolveFolder(pool, f, ctx, &why);
    QCOMPARE(fp.size(), 1); QVERIFY(fp.first().repeat);
}

namespace {
SlotItem mediaItem(const QString& id, const QString& title, double sec) {
    SlotItem it; it.id = id; it.title = title; it.type = "program"; it.path = "D:/media/program/" + id + ".mp4"; it.durationSec = sec; return it;
}
double hourSec(const HourSlot& s) { double t = 0; for (const SlotItem& it : s.items) t += FormatClock::slotItemSec(it); return t; }
}

void TestProgramming::overrunPlanContinueCutSplit() {
    HourSlot cur; cur.channelId = "main"; cur.id = "2026-10-22-12"; cur.date = QDate(2026, 10, 22); cur.hour = 12;
    cur.items << mediaItem("a", "Opener", 1200) << mediaItem("b", "Feature", 3000);    // b ends 4200 s in: 600 s over
    HourSlot next; next.channelId = "main"; next.id = "2026-10-22-13"; next.date = QDate(2026, 10, 22); next.hour = 13;
    next.items << mediaItem("n1", "Next show", 1680);
    SlotItem lockId = mediaItem("id1", "Legal ID", 10); lockId.locked = true; lockId.lockAtSec = 3590; next.items << lockId;

    FormatClock::OverrunPlan plan;
    // continue + keep: b plays in full, the next hour is a continuation 600 s in, its own items stay
    plan.mode = "continue"; plan.next = "keep";
    auto r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY2(r.ok, qPrintable(r.error));
    QCOMPARE(r.overrunSec, 600.0); QCOMPARE(r.carrySec, 600.0);
    QVERIFY(r.next.isContinuation());
    QCOMPARE(r.next.mergedEvent.value("carrySec").toDouble(), 600.0);
    QCOMPARE(r.next.mergedEvent.value("rootSlotId").toString(), cur.id);
    QCOMPARE(r.next.items.size(), 2);
    QCOMPARE(r.cur.items[1].extra.value("overrunMode").toString(), QString("continue"));
    // continue + ads: the rest of the next hour (3000 s) is one break block; the locked ID stays
    plan.next = "ads";
    r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY(r.ok);
    QCOMPARE(r.next.items.size(), 2);
    QCOMPARE(QueryBlock::fromJson(r.next.items[0].queryBlock).targetSec, 3000.0);
    QVERIFY(QueryBlock::fromJson(r.next.items[0].queryBlock).types.contains("ad"));
    QVERIFY(r.next.items[1].locked);
    // continue + recipe: the remainder is laid from the recipe, no station ID at :00 (the programme is still on)
    plan.next = "recipe"; plan.recipe = FormatClock::builtinRecipes().first();
    r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY(r.ok);
    double recipeSec = 0; for (const SlotItem& it : r.next.items) if (!it.locked) recipeSec += FormatClock::slotItemSec(it);
    QVERIFY2(recipeSec >= 2970 && recipeSec <= 3031, qPrintable(QString::number(recipeSec)));
    for (const SlotItem& it : r.next.items) QVERIFY(!it.title.contains("Station ID"));
    // cut: b ends at the top of the hour
    plan.mode = "cut"; plan.next = "keep";
    r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY(r.ok);
    QCOMPARE(r.cur.items[1].outPointSec, 2400.0);
    QCOMPARE(hourSec(r.cur), 3600.0);
    QVERIFY(!r.next.isContinuation());
    // split at segments (ends 900, 1800, 2700): parts + 2-minute breaks fill the hour exactly; the rest opens the next hour
    plan.mode = "split"; plan.next = "keep"; plan.breakSec = 120;
    plan.segments = QJsonArray{QJsonObject{{"startTime", 0}, {"endTime", 900}}, QJsonObject{{"startTime", 900}, {"endTime", 1800}},
                               QJsonObject{{"startTime", 1800}, {"endTime", 2700}}, QJsonObject{{"startTime", 2700}, {"endTime", 3000}}};
    r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY2(r.ok, qPrintable(r.error));
    QVERIFY2(qAbs(hourSec(r.cur) - 3600.0) < 1.0, qPrintable(QString::number(hourSec(r.cur))));
    // cur: Opener, part1 900, break, part2 900, break, part3 cut to 360 s
    QCOMPARE(r.cur.items.size(), 6);
    QCOMPARE(r.cur.items[5].inPointSec, 1800.0); QCOMPARE(r.cur.items[5].outPointSec, 2160.0);
    // next: part3 rest (2160-2700), break, part4 (2700-3000), then the next hour's own items
    QCOMPARE(r.next.items[0].inPointSec, 2160.0); QCOMPARE(r.next.items[0].outPointSec, 2700.0);
    QVERIFY(r.next.items[1].isQuery());
    QCOMPARE(r.next.items[2].inPointSec, 2700.0); QCOMPARE(r.next.items[2].outPointSec, 3000.0);
    QCOMPARE(r.next.items[3].id, QString("n1"));
    QCOMPARE(r.next.mergedEvent.value("continuedFrom").toString(), cur.id);
    QVERIFY(!r.next.isContinuation());                         // the next hour starts at :00 with the rest: no carry
    // split without segments: a break every 15 minutes of programme
    plan.segments = QJsonArray(); plan.breakEverySec = 900;
    r = FormatClock::planOverrun(cur, "b", next, plan);
    QVERIFY(r.ok);
    QVERIFY(qAbs(hourSec(r.cur) - 3600.0) < 1.0);
    // an item that fits is refused
    QVERIFY(!FormatClock::planOverrun(cur, "a", next, plan).ok);
}

void TestProgramming::continuationHourStartsAfterTheCarry() {
    HourSlot s; s.channelId = "main"; s.id = "3-13"; s.dayOfWeek = 3; s.hour = 13;
    SlotItem it; it.id = "p2"; it.mediaId = "p2"; it.type = "program"; it.durationSec = 1680; s.items << it;
    BuildOptions o; o.nowMs = m_now; o.fillGaps = false; o.continuesFromPreviousHour = true; o.startOffsetSec = 1800;
    const qint64 hourStart = RunlistBuilder::hourStartUtcMs(QDate(2026, 10, 22), 13, QTimeZone("America/New_York"));
    const Runlist rl = RunlistBuilder::build(s, hourStart, lookup(), o);
    QVERIFY(!rl.items.isEmpty());
    QCOMPARE(rl.items.first().plannedStartMs, hourStart + 1800000);   // after the carried-in programme, not on top of it
}

void TestProgramming::overrunKeepsAiringAcrossTheHour() {
    ExecEnv env;
    QVERIFY(env.init(m_pool, QJsonObject()));
    HourSlot h12; h12.channelId = "main"; h12.id = "2026-10-22-12"; h12.date = QDate(2026, 10, 22); h12.dayOfWeek = -1; h12.hour = 12;
    SlotItem longItem; longItem.id = "movie"; longItem.title = "Movie"; longItem.type = "program"; longItem.path = "D:/media/program/movie.mp4"; longItem.durationSec = 5400;
    h12.items << longItem;
    QVERIFY(env.store.saveSlot(h12));
    HourSlot h13; h13.channelId = "main"; h13.id = "2026-10-22-13"; h13.date = QDate(2026, 10, 22); h13.dayOfWeek = -1; h13.hour = 13;
    h13.mergedEvent = QJsonObject{{"isContinuation", true}, {"rootSlotId", h12.id}, {"carrySec", 1800}};
    SlotItem after; after.id = "after"; after.mediaId = "p2"; after.type = "program"; after.durationSec = 1680; h13.items << after;
    QVERIFY(env.store.saveSlot(h13));

    env.now = env.at(12, 0, 5);
    env.ex->onEngineConnected("main");
    QVERIFY(!env.link.pushes.isEmpty());
    const QJsonArray first = env.link.pushes.last().items;
    QVERIFY(first.size() >= 2);
    const QString movieId = first[0].toObject().value("id").toString();
    QCOMPARE(first[0].toObject().value("duration").toDouble() > 5390.0, true);         // not cut at 13:00
    QCOMPARE(qint64(first[1].toObject().value("plannedUtcMs").toDouble()), env.at(13, 30));   // the next hour waits for it
    // 13:00:02: the hour changes while the movie is still on air
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", movieId}, {"position", 3597.0}});
    const int before = env.link.pushes.size();
    env.now = env.at(13, 0, 2);
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", movieId}, {"position", 3602.0}});
    env.ex->tick();
    bool replace = true;
    const QJsonArray planned = env.ex->plannedPlaylist("main", env.now, &replace);
    QVERIFY2(!replace, "the new hour's playlist would cut the movie that is still on air");
    QVERIFY(!planned.isEmpty());
    QCOMPARE(qint64(planned[0].toObject().value("plannedUtcMs").toDouble()), env.at(13, 30));
    for (int i = before; i < env.link.pushes.size(); ++i) QVERIFY2(!env.link.pushes[i].replace, "a push replaced the movie mid-air");
}

void TestProgramming::smallFillPoolFillsTheWholeGap() {
    // One 10-second promo is the whole fill pool: an hour-long gap is still filled (repeats), not eight minutes of it.
    QList<MediaRecord> pool; pool << mk("tiny", "promo", 10, 0, 0, m_now - 86400000LL);
    FillSettings fs;
    FillContext fc; fc.nowMs = m_now; fc.seed = 3;
    const QList<MediaRecord> picks = FillPolicy::fillGap(3473.7, pool, fc, fs, false);
    double got = 0; for (const auto& m : picks) got += m.durationSec;
    QVERIFY2(got >= 3473.7 - qMax(fs.minGapSec, 10.0), qPrintable(QString("filled only %1 s of 3473.7").arg(got)));
    QVERIFY(got <= 3473.7 + 0.01);
}

void TestProgramming::operatorTakeQueuesTheScheduleBehindIt() {
    ExecEnv env;
    QVERIFY(env.init(m_pool, QJsonObject()));
    HourSlot h; h.channelId = "main"; h.id = "2026-10-22-12"; h.date = QDate(2026, 10, 22); h.dayOfWeek = -1; h.hour = 12;
    for (const char* id : {"p1", "p2", "p3"}) { SlotItem it; it.id = id; it.mediaId = id; it.type = "program"; it.durationSec = 1680; h.items << it; }
    QVERIFY(env.store.saveSlot(h));
    env.now = env.at(12, 10);
    env.ex->onEngineConnected("main");
    QVERIFY(!env.link.pushes.isEmpty());
    const int before = env.link.pushes.size();
    // 12:20: the operator takes a 60-second clip
    env.now = env.at(12, 20);
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", "take-1792700000000"}, {"position", 0.0}, {"duration", 60.0}});
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), before + 1);
    QVERIFY2(!env.link.pushes.last().replace, "the schedule push cut the operator's take");
    QJsonArray items = env.link.pushes.last().items;
    QVERIFY(!items.isEmpty());
    // the schedule resumes where the hour will be when the take ends (12:21), joined in progress
    const QJsonObject first = items[0].toObject();
    QCOMPARE(first.value("assetId").toString(), QString("p1"));
    QVERIFY2(qAbs(first.value("inPoint").toDouble() - 1260.0) < 1.0, qPrintable(QString::number(first.value("inPoint").toDouble())));
    // the same take keeps reporting: no further pushes
    env.now = env.at(12, 20, 30);
    env.ex->onTelemetry("main", QJsonObject{{"state", "ON_AIR"}, {"itemId", "take-1792700000000"}, {"position", 30.0}, {"duration", 60.0}});
    env.ex->tick();
    QCOMPARE(env.link.pushes.size(), before + 1);
    // the operator leaves the take early (skip): the schedule is queued from now, still without cutting anything
    env.ex->returnToSchedule("main");
    QCOMPARE(env.link.pushes.size(), before + 2);
    QVERIFY(!env.link.pushes.last().replace);
    items = env.link.pushes.last().items;
    QVERIFY2(qAbs(items[0].toObject().value("inPoint").toDouble() - 1230.0) < 1.0, qPrintable(QString::number(items[0].toObject().value("inPoint").toDouble())));
}

// Hard start vs the clip still playing: the item decides (extra.conflictPolicy), "exact start" and "finish the program" never both win.
void TestProgramming::hardStartInterruptionPolicies() {
    const QList<MediaRecord> pool = {mk("pa", "program", 1500, 0, 0, 1), mk("pb", "program", 600, 0, 0, 2), mk("news", "program", 300, 0, 0, 3)};
    const MediaLookup ml = lookupOf(pool);
    const qint64 h0 = 1790000000000LL - (1790000000000LL % 3600000LL);
    auto build = [&](const QString& policy, double maxLate) {
        HourSlot s; s.channelId = "main"; s.id = "0-0";
        SlotItem a; a.id = "a"; a.mediaId = "pa"; a.type = "program"; s.items << a;                 // 0 .. 1500 s
        SlotItem b; b.id = "b"; b.mediaId = "pb"; b.type = "program"; s.items << b;                 // 1500 .. 2100 s
        SlotItem n; n.id = "n"; n.mediaId = "news"; n.type = "program"; n.timingMode = "hard_start"; n.fixedOffsetSec = 1800;   // lands inside b
        if (policy != "truncate") n.extra["conflictPolicy"] = policy;
        if (maxLate >= 0) n.extra["maxLateSec"] = maxLate;
        s.items << n;
        BuildOptions o; o.fillGaps = false; o.nowMs = h0;
        return RunlistBuilder::build(s, h0, ml, o);
    };
    auto find = [](const Runlist& rl, const QString& id) -> const RunlistItem* { for (const auto& r : rl.items) if (r.item.id == id) return &r; return nullptr; };
    // truncate (default): the exact start wins, b is cut to 300 s
    Runlist rl = build("truncate", -1);
    QCOMPARE(find(rl, "n")->plannedStartMs, h0 + 1800000LL); QVERIFY(find(rl, "n")->hard);
    QVERIFY(find(rl, "b")->truncated); QCOMPARE(find(rl, "b")->plannedEndMs, h0 + 1800000LL);
    // finish_previous: b finishes, news starts 300 s late and is not an exact (engine-cut) start
    rl = build("finish_previous", -1);
    QVERIFY(!find(rl, "b")->truncated); QCOMPARE(find(rl, "n")->plannedStartMs, h0 + 2100000LL); QVERIFY(!find(rl, "n")->hard);
    // finish_previous with a 120 s limit: late by 120 s, the other 180 s of overlap are cut off b
    rl = build("finish_previous", 120);
    QCOMPARE(find(rl, "n")->plannedStartMs, h0 + 1920000LL); QVERIFY(find(rl, "b")->truncated); QCOMPARE(find(rl, "b")->plannedEndMs, h0 + 1920000LL);
    // drop_previous: b is not played at all; the hour has a hole (no fill here) before the hard start
    rl = build("drop_previous", -1);
    QVERIFY(!find(rl, "b")); QCOMPARE(find(rl, "n")->plannedStartMs, h0 + 1800000LL); QVERIFY(find(rl, "n")->hard);
    QVERIFY2(rl.gapSec >= 299.0, qPrintable(QString::number(rl.gapSec)));
}

// Backtiming: the events before a hard start are timed backward from it, so they end exactly on the anchor (the gap goes in front).
void TestProgramming::backtimingEndsOnTheHardStart() {
    const QList<MediaRecord> pool = {mk("pa", "program", 600, 0, 0, 1), mk("pb", "program", 600, 0, 0, 2), mk("news", "program", 300, 0, 0, 3), mk("fl", "filler", 60, 0, 0, 4)};
    const MediaLookup ml = lookupOf(pool);
    const qint64 h0 = 1790000000000LL - (1790000000000LL % 3600000LL);
    HourSlot s; s.channelId = "main"; s.id = "0-0";
    SlotItem a; a.id = "a"; a.mediaId = "pa"; a.type = "program"; s.items << a;
    SlotItem b; b.id = "b"; b.mediaId = "pb"; b.type = "program"; s.items << b;
    SlotItem n; n.id = "n"; n.mediaId = "news"; n.type = "program"; n.timingMode = "hard_start"; n.fixedOffsetSec = 2400; n.extra["backtime"] = true; s.items << n;
    BuildOptions o; o.fillGaps = false; o.nowMs = h0;
    Runlist rl = RunlistBuilder::build(s, h0, ml, o);
    QCOMPARE(rl.items[0].item.id, QString("a")); QCOMPARE(rl.items[1].item.id, QString("b")); QCOMPARE(rl.items[2].item.id, QString("n"));
    QCOMPARE(rl.items[1].plannedEndMs, h0 + 2400000LL);                 // b ends on the anchor
    QCOMPARE(rl.items[0].plannedStartMs, h0 + 1200000LL);               // the 1200 s gap sits in front of the block
    QVERIFY(rl.items[0].hard);                                          // nothing fills the front: the block holds for its own exact start
    QVERIFY2(rl.gapSec >= 1199.0, qPrintable(QString::number(rl.gapSec)));
    // without the flag the same hour plays from the top and leaves the gap just before the news
    s.items[2].extra.remove("backtime");
    rl = RunlistBuilder::build(s, h0, ml, o);
    QCOMPARE(rl.items[0].plannedStartMs, h0); QCOMPARE(rl.items[2].plannedStartMs, h0 + 2400000LL);
    // with a filler pool the front is filled and the programs still end on the anchor
    s.items[2].extra["backtime"] = true;
    BuildOptions of = o; of.fillGaps = true; of.fill.fillWithFiller = true;
    rl = RunlistBuilder::build(s, h0, ml, of);
    const RunlistItem* before = nullptr; for (const auto& r : rl.items) if (r.item.id == "b") before = &r;
    QVERIFY(before); QCOMPARE(before->plannedEndMs, h0 + 2400000LL);
    for (int i = 1; i < rl.items.size(); ++i) QVERIFY2(rl.items[i].plannedStartMs >= rl.items[i - 1].plannedEndMs, "items overlap");
}

// A station with no filler / promo / bumper clips: gaps inside an hour stay empty unless the channel lets gap fill use other types (news, weather spots).
void TestProgramming::gapFillCanUseExtraTypes() {
    QList<MediaRecord> pool = {mk("pa", "program", 600, 0, 0, 1), mk("news", "program", 300, 0, 0, 3)};
    for (int i = 0; i < 6; ++i) pool << mk(QString("nw%1").arg(i), "news", 60, 0, 0, 10 + i);
    for (int i = 0; i < 4; ++i) pool << mk(QString("wx%1").arg(i), "weather", 25, 0, 0, 20 + i);
    const MediaLookup ml = lookupOf(pool);
    const qint64 h0 = 1790000000000LL - (1790000000000LL % 3600000LL);
    HourSlot s; s.channelId = "main"; s.id = "0-0";
    SlotItem a; a.id = "a"; a.mediaId = "pa"; a.type = "program"; s.items << a;
    SlotItem n; n.id = "n"; n.mediaId = "news"; n.type = "program"; n.timingMode = "hard_start"; n.fixedOffsetSec = 1800; s.items << n;   // 600 s program, 300 s news at 1800 s: a 1200 s gap
    BuildOptions o; o.nowMs = h0;
    Runlist rl = RunlistBuilder::build(s, h0, ml, o);
    QVERIFY2(rl.gapSec >= 1199.0, qPrintable(QString::number(rl.gapSec)));                    // default fill types: nothing eligible, the gap stays empty
    QCOMPARE(FillPolicy::fillTypes(o.fill), (QStringList{"station_id", "bumper", "filler", "promo", "psa"}));
    // allow news and weather (settings JSON round trip, as the channel profile stores it)
    o.fill = FillSettings::fromJson(QJsonObject{{"fillExtraTypes", QJsonArray{"News", "weather", "news"}}});
    QCOMPARE(o.fill.fillExtraTypes, (QStringList{"news", "weather"}));
    QCOMPARE(FillSettings::fromJson(o.fill.toJson()).fillExtraTypes, o.fill.fillExtraTypes);
    QVERIFY(FillPolicy::fillTypes(o.fill).contains("news") && FillPolicy::fillTypes(o.fill).contains("weather"));
    rl = RunlistBuilder::build(s, h0, ml, o);
    QVERIFY2(rl.gapSec < 60.0, qPrintable(QString::number(rl.gapSec)));                       // filled (to within one clip)
    int fills = 0; for (const auto& r : rl.items) if (r.origin == "fill") { ++fills; QVERIFY(r.item.type == "news" || r.item.type == "weather"); }
    QVERIFY(fills >= 12);
    // programs are never used as gap fill
    for (const auto& r : rl.items) if (r.origin == "fill") QVERIFY(r.item.mediaId != "pa" && r.item.mediaId != "news");
}

QTEST_GUILESS_MAIN(TestProgramming)
#include "test_programming.moc"
