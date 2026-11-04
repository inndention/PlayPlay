// test_studio_runqueue (ST-10): the run queue and the concurrency profile.
//
// Part 1 is the TS queue suite (starmedia test/runqueue.ts "the pure queue" and test/concurrency-queue.ts) on a
// FakeClock: pools, priority, round robin across sessions, abort / cancel / cancelSession, group limits, status,
// listeners, live limits, the watchdog (fake and real timers), five projects sharing three slots, the board's
// Cancel / Run next, 300-job stress, determinism and a 60-run fuzz. Part 2 is test/concurrency-profile.ts: tiers from
// cores and free RAM, the safe tier, the other tiers, validation and clamping, the layers, the heavy cap and per-kind
// step caps, workers, the machine, publish / adopt, applying a profile to the gates / queue / graph defaults, a
// 400-input fuzz, and the server ranges this port adds. Hermetic: no network, no files, no processes.
#include <QtTest>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <functional>
#include <memory>
#include <optional>

#include "core/clock.h"
#include "provider/gates.h"
#include "sched/graph.h"
#include "sched/profile.h"
#include "sched/runqueue.h"

using namespace studio;
using namespace studio::sched;
using studio::provider::AbortController;
using studio::provider::AbortSignal;
using namespace Qt::Literals::StringLiterals;

namespace {

// ---------------- the TS job(): a run that remembers when its slot was granted ----------------

struct Job {
    QString name;
    bool started = false;
    QString err;
    quint64 id = 0;
    int order = 0;
    QList<int> ahead;
};
int g_started = 0;

std::shared_ptr<Job> job(RunQueue& q, SlotKind kind, const QString& session, const QString& name = {},
                         SlotMeta extra = {}, const AbortSignal& signal = {}) {
    auto j = std::make_shared<Job>();
    j->name = name.isEmpty() ? session + u"-"_s + slotKindName(kind) : name;
    SlotMeta m = extra;
    m.session = session;
    m.label = j->name;
    RunQueue::AcquireOptions o;
    o.onWait = [j](int a) { j->ahead << a; };
    const quint64 id = q.acquire(
        kind, m,
        [j](quint64 gid) {
            j->started = true;
            j->order = ++g_started;
            j->id = gid;
        },
        [j](const QString& why) { j->err = why; }, signal, o);
    if (id) j->id = id;
    return j;
}

QString names(const QList<std::shared_ptr<Job>>& js) {
    QList<std::shared_ptr<Job>> s;
    for (const auto& j : js) if (j->started) s << j;
    std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) { return a->order < b->order; });
    QStringList out;
    for (const auto& j : s) out << j->name;
    return out.join(u',');
}

RunQueueOptions opts(int heavy, int light, qint64 maxHoldMs = kDefaultMaxHoldMs) {
    RunQueueOptions o;
    o.heavy = heavy;
    o.light = light;
    o.maxHoldMs = maxHoldMs;
    return o;
}

// ---------------- the TS harness(): runs that hold a slot for `dur` virtual ms ----------------

struct VJob {
    QString session;
    SlotKind kind = SlotKind::Light;
    QString name;
    quint64 id = 0;
    qint64 startedAt = -1, endedAt = -1;
    QString err;
};

struct Harness {
    FakeClock clock;
    QObject ctx;
    std::unique_ptr<RunQueue> q;
    QList<std::shared_ptr<VJob>> starts, all;
    int serial = 0;
    int peakHeavy = 0, peakLight = 0;

    explicit Harness(int heavy = 3, int light = 8) : q(std::make_unique<RunQueue>(clock, opts(heavy, light, 0))) {}

    std::shared_ptr<VJob> submit(const QString& session, SlotKind kind, qint64 dur, QString name = {}, SlotMeta meta = {},
                                 const AbortSignal& signal = {}) {
        auto j = std::make_shared<VJob>();
        j->session = session;
        j->kind = kind;
        j->name = name.isEmpty() ? session + QString::number(++serial) : name;
        all << j;
        meta.session = session;
        meta.label = j->name;
        const quint64 id = q->acquire(
            kind, meta,
            [this, j, dur](quint64 gid) {
                j->id = gid;
                j->startedAt = clock.steadyMs();
                starts << j;
                peakHeavy = std::max(peakHeavy, q->runningCount(SlotKind::Heavy));
                peakLight = std::max(peakLight, q->runningCount(SlotKind::Light));
                clock.callAfter(dur, &ctx, [this, j] {   // the run "sleeps" on the fake clock, then gives the slot back
                    j->endedAt = clock.steadyMs();
                    q->release(j->id);
                });
            },
            [this, j](const QString& why) {
                j->err = why;
                j->endedAt = clock.steadyMs();
            },
            signal);
        if (id) j->id = id;
        return j;
    }

    void finishAll() {
        int guard = 0;
        while (guard++ < 1000000) {
            bool left = false;
            for (const auto& j : all) left = left || j->endedAt < 0;
            if (!left) return;
            const qint64 next = clock.nextDueSteadyMs();
            if (next < 0) return;
            clock.advance(next - clock.steadyMs());
        }
    }
    QStringList queuedSessions() const {
        QStringList s;
        for (const WaitInfo& w : q->queued()) s << w.meta.session;
        return s;
    }
};

Hardware hw(std::optional<int> cores, std::optional<qint64> freeRamMb) {
    Hardware h;
    if (cores && *cores > 0) h.cores = cores;
    if (freeRamMb && *freeRamMb > 0) h.freeRamMb = freeRamMb;
    h.source = u"test"_s;
    return h;
}

ProfileInput in(const Hardware& h) {
    ProfileInput i;
    i.hardware = h;
    return i;
}

QString shape(const Profile& p) {
    return u"q:%1/%2 g:%3/%4/%5/%6 s:%7 w:%8"_s.arg(p.queue.heavy).arg(p.queue.light).arg(p.gates.videos).arg(p.gates.images)
        .arg(p.gates.audio).arg(p.gates.chat).arg(p.steps).arg(p.workers);
}

bool anyWarning(const QStringList& w, const QString& needle) {
    for (const QString& s : w) if (s.contains(needle)) return true;
    return false;
}

QJsonObject obj(std::initializer_list<QPair<QString, QJsonValue>> l) { return QJsonObject(l); }

} // namespace

class TestStudioRunQueue : public QObject {
    Q_OBJECT
private slots:
    void init() { g_started = 0; resetProfile(); }

    // ---------------- part 1: the queue ----------------
    void basics();
    void priorities();
    void fairness();
    void abortAndCancel();
    void groupLimits();
    void statusReport();
    void changeListeners();
    void limitsChange();
    void stressAndDeterminism();
    void watchdog();
    void watchdogRealTimer();
    void fiveProjectsShareThreeSlots();
    void poolsIndependent();
    void boardCancelAndRunNext();
    void sessionCancelGroupAndWhy();
    void liveLimitsFromProfile();
    void fuzzRandomOperations();

    // ---------------- part 2: the profile ----------------
    void tiers();
    void safeTierExact();
    void otherTiersScale();
    void validationAndClamping();
    void layers();
    void heavyCapAndKindSteps();
    void workers();
    void readingTheMachine();
    void applyToTargets();
    void fuzzAnyInput();
    void serverRanges();
};

// =====================================================================================================================

void TestStudioRunQueue::basics() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 2));
    auto a = job(q, SlotKind::Heavy, u"s1"_s);
    QVERIFY(a->started);
    QCOMPARE(q.runningTotal(), 1);
    QVERIFY(a->ahead.isEmpty());                         // the first job was never announced as waiting
    auto b = job(q, SlotKind::Heavy, u"s1"_s, u"b"_s), c = job(q, SlotKind::Heavy, u"s1"_s, u"c"_s);
    QVERIFY(!b->started && !c->started);
    QCOMPARE(q.queuedTotal(), 2);
    QCOMPARE(b->ahead.first(), 0);                       // queued jobs are told how many are ahead (0 = next)
    QCOMPARE(c->ahead.first(), 1);
    q.release(a->id);
    QVERIFY(b->started && !c->started);                  // release starts the next in line (FIFO)
    QCOMPARE(c->ahead.last(), 0);                        // the one behind moves up and is told
    q.release(b->id);
    QVERIFY(c->started);
    QCOMPARE(q.queuedTotal(), 0);
    q.release(c->id);
    q.release(c->id);
    q.release(c->id);
    QCOMPARE(q.runningTotal(), 0);                       // releasing twice (or three times) is harmless
    QCOMPARE(q.metrics().released, 3);
    // light is independent of heavy
    auto h = job(q, SlotKind::Heavy, u"x"_s), l1 = job(q, SlotKind::Light, u"x"_s, u"l1"_s),
         l2 = job(q, SlotKind::Light, u"x"_s, u"l2"_s), l3 = job(q, SlotKind::Light, u"x"_s, u"l3"_s);
    QVERIFY(h->started && l1->started && l2->started && !l3->started);
    QCOMPARE(q.runningCount(SlotKind::Light), 2);
    q.release(l1->id);
    QVERIFY(l3->started);                                // a finished light job frees a light slot only
    QCOMPARE(q.runningCount(SlotKind::Heavy), 1);
    for (const auto& j : {h, l2, l3}) q.release(j->id);
    QCOMPARE(q.runningTotal(), 0);
}

void TestStudioRunQueue::priorities() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 2));
    auto hold = job(q, SlotKind::Heavy, u"a"_s, u"hold"_s);
    SlotMeta p5, p2, pNeg;
    p5.priority = 5;
    p2.priority = 2;
    pNeg.priority = -1;
    auto lo = job(q, SlotKind::Heavy, u"a"_s, u"lo"_s), hi = job(q, SlotKind::Heavy, u"b"_s, u"hi"_s, p5),
         mid = job(q, SlotKind::Heavy, u"c"_s, u"mid"_s, p2), lo2 = job(q, SlotKind::Heavy, u"a"_s, u"lo2"_s);
    QCOMPARE(hi->ahead.last(), 0);                       // positions respect priority
    QCOMPARE(mid->ahead.last(), 1);
    QCOMPARE(lo->ahead.last(), 2);
    QCOMPARE(lo2->ahead.last(), 3);
    q.release(hold->id);
    QVERIFY(hi->started && !mid->started && !lo->started);
    q.release(hi->id);
    QVERIFY(mid->started && !lo->started);
    q.release(mid->id);
    QVERIFY(lo->started && !lo2->started);               // equal priority within one session: first come first served
    q.release(lo->id);
    QVERIFY(lo2->started);
    q.release(lo2->id);
    RunQueue n(clock, opts(1, 2));
    auto h2 = job(n, SlotKind::Heavy, u"a"_s, u"h2"_s);
    auto neg = job(n, SlotKind::Heavy, u"a"_s, u"neg"_s, pNeg), zero = job(n, SlotKind::Heavy, u"a"_s, u"zero"_s);
    n.release(h2->id);
    QVERIFY(zero->started && !neg->started);             // negative priority goes last
    n.release(zero->id);
    QVERIFY(neg->started);
    QCOMPARE(n.topPriority(), 0);
}

void TestStudioRunQueue::fairness() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 2));
    auto hold = job(q, SlotKind::Heavy, u"a"_s, u"hold"_s);
    QList<std::shared_ptr<Job>> all;
    for (int i = 1; i <= 4; ++i) all << job(q, SlotKind::Heavy, u"A"_s, u"A%1"_s.arg(i));
    for (int i = 1; i <= 2; ++i) all << job(q, SlotKind::Heavy, u"B"_s, u"B%1"_s.arg(i));
    all << job(q, SlotKind::Heavy, u"C"_s, u"C1"_s);
    q.release(hold->id);
    QStringList order;
    for (int i = 0; i < 7; ++i) {
        std::shared_ptr<Job> cur;
        for (const auto& j : all) if (j->started && !order.contains(j->name)) { cur = j; break; }
        if (!cur) break;
        order << cur->name;
        q.release(cur->id);
    }
    QCOMPARE(order.join(u','), u"A1,B1,C1,A2,B2,A3,A4"_s);   // sessions take turns
    // a session that was served long ago goes before one served just now
    RunQueue q2(clock, opts(1, 2));
    auto x = job(q2, SlotKind::Heavy, u"X"_s, u"x0"_s);
    auto y = job(q2, SlotKind::Heavy, u"Y"_s, u"y0"_s);
    q2.release(x->id);                                   // Y served now
    auto x2 = job(q2, SlotKind::Heavy, u"X"_s, u"x1"_s), y2 = job(q2, SlotKind::Heavy, u"Y"_s, u"y1"_s);
    q2.release(y->id);
    QVERIFY2(x2->started && !y2->started, qPrintable(names({x2, y2})));
    // a new session overtakes sessions that were just served
    RunQueue q3(clock, opts(1, 2));
    auto p1 = job(q3, SlotKind::Heavy, u"P"_s, u"p1"_s);
    auto p2 = job(q3, SlotKind::Heavy, u"P"_s, u"p2"_s), p3 = job(q3, SlotKind::Heavy, u"P"_s, u"p3"_s),
         n1 = job(q3, SlotKind::Heavy, u"N"_s, u"n1"_s);
    q3.release(p1->id);
    QVERIFY(n1->started && !p2->started && !p3->started);
    // fairness is per pool
    RunQueue q4(clock, opts(1, 1));
    auto lh = job(q4, SlotKind::Heavy, u"P"_s, u"lh"_s), ll = job(q4, SlotKind::Light, u"P"_s, u"ll"_s);
    QVERIFY(lh->started && ll->started);
}

void TestStudioRunQueue::abortAndCancel() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 2));
    auto hold = job(q, SlotKind::Heavy, u"a"_s, u"hold"_s);
    AbortController ac;
    auto w = job(q, SlotKind::Heavy, u"b"_s, u"w"_s, {}, ac.signal());
    auto w2 = job(q, SlotKind::Heavy, u"c"_s, u"w2"_s);
    QCOMPARE(q.queuedTotal(), 2);
    ac.abort();                                          // the default reason says nothing: the queue's own words
    QVERIFY2(w->err.startsWith(u"Cancelled while waiting") && !w->started, qPrintable(w->err));
    QCOMPARE(q.queuedTotal(), 1);
    QCOMPARE(q.queued().first().meta.label, u"w2"_s);
    QCOMPARE(w2->ahead.last(), 0);                       // the one behind is told it moved up
    q.release(hold->id);
    QVERIFY(w2->started && !w->started);                 // the aborted job never takes a slot
    QCOMPARE(q.runningTotal(), 1);
    // already aborted
    AbortController dead;
    dead.abort();
    auto d = job(q, SlotKind::Heavy, u"z"_s, u"dead"_s, {}, dead.signal());
    QVERIFY(d->err.startsWith(u"Cancelled while waiting"));
    QCOMPARE(d->id, 0ULL);
    QCOMPARE(q.queuedTotal(), 0);
    QCOMPARE(q.runningTotal(), 1);
    // aborting after the slot was granted does not steal it
    AbortController ac2;
    RunQueue q2(clock, opts(1, 2));
    auto r = job(q2, SlotKind::Heavy, u"a"_s, u"r"_s, {}, ac2.signal());
    ac2.abort();
    QVERIFY(r->started && r->err.isEmpty());
    QCOMPARE(q2.runningTotal(), 1);
    q2.release(r->id);
    QCOMPARE(q2.runningTotal(), 0);
    // abort reason text: a reason the caller gave is kept, the engine's generic one is not
    AbortController ac3;
    RunQueue q3(clock, opts(1, 2));
    auto hh = job(q3, SlotKind::Heavy, u"a"_s);
    auto rj = job(q3, SlotKind::Heavy, u"b"_s, u"rj"_s, {}, ac3.signal());
    ac3.abort(u"Stopped by you"_s);
    QCOMPARE(rj->err, u"Stopped by you"_s);
    AbortController ac4;
    auto rk = job(q3, SlotKind::Heavy, u"b"_s, u"rk"_s, {}, ac4.signal());
    ac4.abort(u"signal is aborted without reason"_s);
    QVERIFY(rk->err.startsWith(u"Cancelled while waiting"));
    q3.release(hh->id);
    QCOMPARE(q3.metrics().aborted, 2);
    // cancelSession
    RunQueue q5(clock, opts(1, 2));
    auto h5 = job(q5, SlotKind::Heavy, u"keep"_s, u"h5"_s);
    auto c1 = job(q5, SlotKind::Heavy, u"gone"_s, u"c1"_s), c2 = job(q5, SlotKind::Light, u"gone"_s, u"c2"_s),
         c3 = job(q5, SlotKind::Heavy, u"keep"_s, u"c3"_s), g1 = job(q5, SlotKind::Heavy, u"gone"_s, u"g-run"_s);
    QCOMPARE(q5.cancelSession(u"gone"_s), 2);            // the two heavy ones; the light one already runs
    QVERIFY(c2->started && c2->err.isEmpty());
    QVERIFY(c1->err.startsWith(u"The project was closed") && g1->err.startsWith(u"The project was closed"));
    QVERIFY(c3->err.isEmpty() && q5.isWaiting(c3->id) && h5->started);
    QCOMPARE(q5.cancelSession(u"nobody"_s), 0);
    QCOMPARE(q5.metrics().cancelled, 2);
    RunQueue q6(clock, opts(1, 2));
    auto run6 = job(q6, SlotKind::Heavy, u"S"_s, u"run6"_s);
    auto wait6 = job(q6, SlotKind::Heavy, u"S"_s, u"wait6"_s);
    q6.cancelSession(u"S"_s);
    QVERIFY(run6->started && run6->err.isEmpty() && q6.runningTotal() == 1 && !wait6->err.isEmpty());
    q6.release(run6->id);
    QCOMPARE(q6.runningTotal(), 0);
    // a queue destroyed with waiters refuses them
    QString closed;
    {
        RunQueue q7(clock, opts(1, 2));
        auto hold7 = job(q7, SlotKind::Heavy, u"a"_s);
        q7.acquire(SlotKind::Heavy, SlotMeta{u"b"_s, u"w7"_s}, [](quint64) {}, [&](const QString& why) { closed = why; });
    }
    QCOMPARE(closed, u"the run queue was closed"_s);
}

void TestStudioRunQueue::groupLimits() {
    FakeClock clock;
    SlotMeta render;
    render.group = u"render"_s;
    render.groupLimit = 1;
    RunQueue q(clock, opts(3, 3));
    auto r1 = job(q, SlotKind::Heavy, u"A"_s, u"r1"_s, render);
    auto r2 = job(q, SlotKind::Heavy, u"B"_s, u"r2"_s, render);
    auto other = job(q, SlotKind::Heavy, u"C"_s, u"other"_s);
    QVERIFY(r1->started && !r2->started);                // only one of the group runs
    QVERIFY(other->started);                             // a job queued behind the blocked one is not blocked by it
    QCOMPARE(q.queued().size(), 1);
    QCOMPARE(q.queued().first().meta.label, u"r2"_s);
    q.release(r1->id);
    QVERIFY(r2->started);                                // the group slot frees the next of the group
    RunQueue q2(clock, opts(3, 3));
    SlotMeta two;
    two.group = u"x"_s;
    two.groupLimit = 2;
    int started = 0;
    for (int i = 1; i <= 3; ++i) started += job(q2, SlotKind::Heavy, u"A"_s, u"g%1"_s.arg(i), two)->started;
    QCOMPARE(started, 2);                                // groupLimit 2 lets two in
    RunQueue q3(clock, opts(1, 1));
    SlotMeta shared;
    shared.group = u"shared"_s;
    auto gh = job(q3, SlotKind::Heavy, u"A"_s, u"gh"_s, shared), gl = job(q3, SlotKind::Light, u"A"_s, u"gl"_s, shared);
    QVERIFY(gh->started && !gl->started);                // a group spans both pools
    q3.release(gh->id);
    QVERIFY(gl->started);
    // groupLimit 0 = unlimited, negative = 1
    for (const auto& [gl2, expect] : QList<QPair<int, int>>{{0, 5}, {-7, 1}, {2, 2}, {1, 1}}) {
        RunQueue g(clock, opts(10, 10));
        SlotMeta m;
        m.group = u"render"_s;
        m.groupLimit = gl2;
        int n = 0;
        for (int i = 0; i < 5; ++i) n += job(g, SlotKind::Heavy, u"g%1"_s.arg(i), u"g%1"_s.arg(i), m)->started;
        QCOMPARE(n, expect);
    }
}

void TestStudioRunQueue::statusReport() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 1));
    SlotMeta assets, chat;
    assets.phase = u"assets"_s;
    chat.phase = u"chat"_s;
    auto a = job(q, SlotKind::Heavy, u"T1"_s, u"Coffee: generating shots"_s, assets);
    clock.advance(4200);
    auto b = job(q, SlotKind::Heavy, u"T2"_s, u"Pizza: generating shots"_s, assets);
    auto l = job(q, SlotKind::Light, u"T1"_s, u"Coffee: thinking"_s, chat);
    auto l2 = job(q, SlotKind::Light, u"T3"_s, u"Dogs: writing the script"_s);
    clock.advance(2600);
    const QJsonObject s = q.status();
    const QJsonArray running = s.value(u"running"_s).toArray(), queued = s.value(u"queued"_s).toArray();
    QCOMPARE(running.size(), 2);
    QCOMPARE(queued.size(), 2);
    QJsonObject heavy;
    for (const QJsonValue& v : running) if (v.toObject().value(u"kind"_s) == u"heavy"_s) heavy = v.toObject();
    QCOMPARE(heavy.value(u"label"_s).toString(), u"Coffee: generating shots"_s);
    QCOMPARE(heavy.value(u"phase"_s).toString(), u"assets"_s);
    QCOMPARE(heavy.value(u"heldS"_s).toInt(), 7);
    QJsonObject qb, dogs;
    for (const QJsonValue& v : queued) {
        if (v.toObject().value(u"label"_s).toString().startsWith(u"Pizza")) qb = v.toObject();
        if (v.toObject().value(u"label"_s).toString().startsWith(u"Dogs")) dogs = v.toObject();
    }
    QCOMPARE(qb.value(u"waitedS"_s).toInt(), 3);
    QCOMPARE(qb.value(u"ahead"_s).toInt(), 0);
    QCOMPARE(qb.value(u"kind"_s).toString(), u"heavy"_s);
    QCOMPARE(qb.value(u"why"_s).toString(), u"pool"_s);
    QCOMPARE(dogs.value(u"ahead"_s).toInt(), 0);         // the light queue is ordered separately
    const QJsonObject bt = s.value(u"byThread"_s).toObject();
    QCOMPARE(bt.value(u"T1"_s).toObject().value(u"running"_s).toInt(), 2);
    QCOMPARE(bt.value(u"T1"_s).toObject().value(u"queued"_s).toInt(), 0);
    QCOMPARE(bt.value(u"T2"_s).toObject().value(u"queued"_s).toInt(), 1);
    QCOMPARE(bt.value(u"T2"_s).toObject().value(u"running"_s).toInt(), 0);
    QCOMPARE(bt.value(u"T2"_s).toObject().value(u"label"_s).toString(), u"Pizza: generating shots"_s);
    QCOMPARE(bt.value(u"T3"_s).toObject().value(u"queued"_s).toInt(), 1);
    const QString t1Label = bt.value(u"T1"_s).toObject().value(u"label"_s).toString();
    QVERIFY(t1Label == u"Coffee: generating shots"_s || t1Label == u"Coffee: thinking"_s);
    QCOMPARE(s.value(u"limits"_s).toObject().value(u"heavy"_s).toInt(), 1);
    QCOMPARE(s.value(u"limits"_s).toObject().value(u"light"_s).toInt(), 1);
    q.release(a->id);
    QCOMPARE(s.value(u"running"_s).toArray().size(), 2); // status is a snapshot
    QVERIFY(b->started);
    const QueueMetrics mm = q.metrics();
    QCOMPARE(mm.acquired, 3);
    QCOMPARE(mm.heavy.acquired, 2);
    QCOMPARE(mm.waitMsMax, 2600);
    QCOMPARE(mm.peakHeavy, 1);
    QVERIFY(mm.peakQueued >= 2);
    QCOMPARE(mm.runningNow, 2);
    QCOMPARE(mm.queuedNow, 1);
    QVERIFY(mm.toJson().value(u"byKind"_s).toObject().value(u"heavy"_s).toObject().value(u"acquired"_s).toInt() == 2);
    Q_UNUSED(l);
    Q_UNUSED(l2);
}

void TestStudioRunQueue::changeListeners() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 2));
    struct Seen { int running, queued; QString label; };
    QList<Seen> seen;
    const auto id = q.onChange([&] {
        const auto r = q.running();
        seen << Seen{q.runningTotal(), q.queuedTotal(), r.isEmpty() ? QString() : r.first().meta.label};
    });
    auto a = job(q, SlotKind::Heavy, u"a"_s);
    QVERIFY(seen.size() >= 1 && seen.last().running == 1);   // a change is reported when a job starts
    auto b = job(q, SlotKind::Heavy, u"b"_s);
    QCOMPARE(seen.last().queued, 1);                     // ... and when one queues
    q.release(a->id);
    QVERIFY(seen.last().running == 1 && seen.last().queued == 0 && seen.last().label == u"b-heavy"_s);
    const int n0 = seen.size();
    q.removeChangeListener(id);
    q.release(b->id);
    QCOMPARE(seen.size(), n0);                           // unsubscribe stops the notifications
    int boom = 0, ok = 0;
    q.onChange([&] { ++boom; throw std::runtime_error("listener bug"); });
    q.onChange([&] { ++ok; });
    auto c = job(q, SlotKind::Heavy, u"c"_s);
    QVERIFY(c->started && boom >= 1 && ok >= 1);         // a listener that throws does not break the queue or the others
    // a listener that queues more work while being notified
    RunQueue q2(clock, opts(1, 2));
    std::shared_ptr<Job> extra;
    bool made = false;
    q2.onChange([&] {
        if (q2.runningTotal() == 1 && !made) {
            made = true;
            extra = job(q2, SlotKind::Heavy, u"again"_s, u"extra"_s);
        }
    });
    auto first = job(q2, SlotKind::Heavy, u"first"_s);
    QVERIFY(first->started && extra && !extra->started && q2.queuedTotal() == 1);
    q2.release(first->id);
    QVERIFY(extra->started);
}

void TestStudioRunQueue::limitsChange() {
    FakeClock clock;
    RunQueue q(clock, opts(1, 1));
    auto a = job(q, SlotKind::Heavy, u"a"_s), b = job(q, SlotKind::Heavy, u"a"_s, u"b"_s), c = job(q, SlotKind::Heavy, u"a"_s, u"c"_s);
    q.setLimits(3, std::nullopt);
    QVERIFY(a->started && b->started && c->started);     // raising the limit starts waiting jobs
    q.setLimits(1, std::nullopt);
    auto d = job(q, SlotKind::Heavy, u"a"_s, u"d"_s);
    QVERIFY(!d->started && q.runningTotal() == 3);       // lowering it does not stop running jobs
    q.release(a->id);
    q.release(b->id);
    QVERIFY(!d->started);
    q.release(c->id);
    QVERIFY(d->started);
    RunQueue z(clock, opts(0, -4));
    QCOMPARE(z.limits().heavy, 3);                       // nonsense limits fall back to sensible ones
    QCOMPARE(z.limits().light, 8);
    RunQueue dflt(clock);
    QCOMPARE(dflt.limits().heavy, kBaseQueueHeavy);
    QCOMPARE(dflt.limits().light, kBaseQueueLight);
    z.setLimits(0, -1);
    QCOMPARE(z.limits().heavy, 3);                       // invalid limits are ignored
    QCOMPARE(z.limits().light, 8);
    QCOMPARE(slotKindFromName(u"nonsense"), SlotKind::Light);   // an unknown kind is treated as light
    QCOMPARE(slotKindFromName(u"heavy"), SlotKind::Heavy);
    RunQueue inf(clock, opts(kUnlimited, kUnlimited));
    int started = 0;
    for (int i = 0; i < 30; ++i) started += job(inf, SlotKind::Heavy, u"s%1"_s.arg(i))->started;
    QCOMPARE(started, 30);                               // unlimited: 30 started, not 3
    QVERIFY(inf.status().value(u"limits"_s).toObject().value(u"heavy"_s).isNull());
}

void TestStudioRunQueue::stressAndDeterminism() {
    FakeClock clock;
    QObject ctx;
    quint32 seed = 12345;
    auto rnd = [&] { seed = (seed * 1103515245u + 12345u) & 0x7fffffffu; return double(seed) / 0x7fffffff; };
    RunQueue q(clock, opts(3, 4, 0));
    int maxH = 0, maxL = 0, done = 0, aborted = 0, maxGroup = 0;
    const int total = 300;
    std::vector<std::unique_ptr<AbortController>> ctls;
    for (int i = 0; i < total; ++i) {
        const SlotKind kind = rnd() < 0.5 ? SlotKind::Heavy : SlotKind::Light;
        const bool grouped = rnd() < 0.15;
        auto ctl = std::make_unique<AbortController>();
        SlotMeta m;
        m.session = u"s%1"_s.arg(int(rnd() * 6));
        m.label = u"j%1"_s.arg(i);
        m.priority = int(rnd() * 3);
        if (grouped) { m.group = u"render"_s; m.groupLimit = 1; }
        const bool twice = rnd() < 0.2;
        const qint64 dur = rnd() < 0.3 ? 2 : 0;
        const quint64 id = q.acquire(
            kind, m,
            [&, twice, dur](quint64 gid) {
                maxH = std::max(maxH, q.runningCount(SlotKind::Heavy));
                maxL = std::max(maxL, q.runningCount(SlotKind::Light));
                int g = 0;
                for (const SlotInfo& s : q.running()) g += s.meta.group == u"render"_s;
                maxGroup = std::max(maxGroup, g);
                clock.callAfter(dur, &ctx, [&, gid, twice] {
                    q.release(gid);
                    if (twice) q.release(gid);
                    ++done;
                });
            },
            [&](const QString&) { ++aborted; }, ctl->signal());
        if (id && rnd() < 0.15 && q.isWaiting(id)) ctl->abort();
        ctls.push_back(std::move(ctl));
        if (i % 25 == 0) clock.advance(1);
    }
    for (int guard = 0; guard < 100000 && q.runningTotal() + q.queuedTotal() > 0; ++guard) {
        const qint64 next = clock.nextDueSteadyMs();
        if (next < 0) break;
        clock.advance(next - clock.steadyMs());
    }
    QVERIFY2(maxH <= 3 && maxL <= 4 && maxGroup <= 1, qPrintable(u"%1 %2 %3"_s.arg(maxH).arg(maxL).arg(maxGroup)));
    QCOMPARE(done + aborted, total);
    QCOMPARE(q.runningTotal(), 0);
    QCOMPARE(q.queuedTotal(), 0);
    const QueueMetrics mm = q.metrics();
    QCOMPARE(mm.acquired, qint64(done));
    QCOMPARE(mm.released, qint64(done));
    QCOMPARE(mm.aborted, qint64(aborted));
    // determinism: the same sequence of calls gives the same order
    auto runOnce = [&] {
        RunQueue qq(clock, opts(1, 8, 0));
        QStringList order;
        quint64 holdId = 0;
        qq.acquire(SlotKind::Heavy, SlotMeta{u"h"_s, u"h"_s}, [&](quint64 id) { holdId = id; });
        const QStringList sessions{u"A"_s, u"B"_s, u"A"_s, u"C"_s, u"B"_s, u"A"_s};
        for (int i = 0; i < sessions.size(); ++i) {
            SlotMeta m;
            m.session = sessions[i];
            m.label = sessions[i] + QString::number(i);
            m.priority = i == 4 ? 2 : 0;
            qq.acquire(SlotKind::Heavy, m, [&, label = m.label](quint64 id) {
                order << label;
                qq.release(id);
            });
        }
        qq.release(holdId);
        return order.join(u',');
    };
    const QString o1 = runOnce(), o2 = runOnce();
    QCOMPARE(o1, o2);
    QCOMPARE(o1, u"B4,A0,C3,B1,A2,A5"_s);                 // priority first, then the sessions in turn
}

void TestStudioRunQueue::watchdog() {
    FakeClock clock;
    struct Warned { QString msg, label; SlotKind kind; qint64 heldMs; };
    QList<Warned> warned;
    RunQueueOptions o = opts(1, 1, 10 * 60000);
    o.onWarning = [&](const QString& msg, const SlotInfo& s) { warned << Warned{msg, s.meta.label, s.kind, s.heldMs}; };
    RunQueue wd(clock, o);
    auto stuck = job(wd, SlotKind::Heavy, u"sA"_s, u"stuck video"_s);
    auto next = job(wd, SlotKind::Heavy, u"sB"_s, u"next"_s);
    QVERIFY(stuck->started && !next->started);           // the second job waits behind the stuck one
    clock.advance(9 * 60000);
    QCOMPARE(wd.sweep(), 0);                             // nothing is taken back before maxHoldMs
    QVERIFY(!next->started);
    QCOMPARE(clock.advance(2 * 60000), 1);               // the slot's own timer fires at 10 min and sweeps
    QVERIFY(next->started);                              // and the next job starts
    QCOMPARE(warned.size(), 1);
    QCOMPARE(warned.first().label, u"stuck video"_s);
    QCOMPARE(warned.first().kind, SlotKind::Heavy);
    QCOMPARE(warned.first().heldMs, 10 * 60000);
    QVERIFY2(warned.first().msg.contains(u"stuck video") && warned.first().msg.contains(u"10 min"), qPrintable(warned.first().msg));
    QCOMPARE(wd.metrics().expired, 1);
    QCOMPARE(wd.sweep(), 0);
    wd.release(stuck->id);
    QCOMPARE(wd.runningTotal(), 1);                      // the stuck job's own release afterwards is harmless
    QCOMPARE(wd.metrics().released, 1);
    wd.release(next->id);
    QCOMPARE(wd.runningTotal(), 0);
    // maxHoldMs 0 means never; a job's own meta.maxHoldMs still applies
    RunQueue w3(clock, opts(2, 1, 0));
    SlotMeta shortHold;
    shortHold.maxHoldMs = 1000;
    auto a3 = job(w3, SlotKind::Heavy, u"x"_s, u"never"_s), b3 = job(w3, SlotKind::Heavy, u"y"_s, u"short"_s, shortHold);
    QVERIFY(a3->started && b3->started);
    clock.advance(5000);
    QCOMPARE(w3.metrics().expired, 1);
    QCOMPARE(w3.running().size(), 1);
    QCOMPARE(w3.running().first().meta.label, u"never"_s);
    // a manual sweep after the clock jumped past several limits takes every overdue slot at once
    RunQueue w4(clock, opts(3, 1, 1000));
    auto s1 = job(w4, SlotKind::Heavy, u"a"_s, u"s1"_s), s2 = job(w4, SlotKind::Heavy, u"b"_s, u"s2"_s);
    QVERIFY(s1->started && s2->started);
    QCOMPARE(w4.sweep(), 0);
    clock.setNowMs(clock.nowMs() + 60000);               // a wall-clock jump alone changes nothing (steady time)
    QCOMPARE(w4.sweep(), 0);
    clock.advance(1000);
    QCOMPARE(w4.metrics().expired, 2);
    QCOMPARE(w4.runningTotal(), 0);
    QCOMPARE(kDefaultMaxHoldMs, 45LL * 60000);           // the default is 45 minutes
}

void TestStudioRunQueue::watchdogRealTimer() {
    // the real clock: a short limit fires on its own
    int warned = 0;
    RunQueueOptions o = opts(1, 1, 150);
    o.onWarning = [&](const QString&, const SlotInfo&) { ++warned; };
    RunQueue real(Clock::system(), o);
    auto hog = job(real, SlotKind::Heavy, u"r1"_s, u"hog"_s);
    auto after = job(real, SlotKind::Heavy, u"r2"_s, u"after"_s);
    QVERIFY(hog->started && !after->started);
    QTRY_VERIFY_WITH_TIMEOUT(after->started, 3000);
    QCOMPARE(real.metrics().expired, 1);
    QCOMPARE(warned, 1);
    real.release(after->id);
    real.release(hog->id);
    QCOMPARE(real.runningTotal(), 0);
}

void TestStudioRunQueue::fiveProjectsShareThreeSlots() {
    Harness h;
    // three short jobs hold every slot while the projects queue up, so the order afterwards is the queue's choice
    for (int i = 0; i < 3; ++i) h.submit(u"H"_s, SlotKind::Heavy, 5);
    for (int i = 0; i < 12; ++i) h.submit(u"A"_s, SlotKind::Heavy, 10);
    for (const QString& s : {u"B"_s, u"C"_s, u"D"_s, u"E"_s})
        for (int i = 0; i < 3; ++i) h.submit(s, SlotKind::Heavy, 10);
    QCOMPARE(h.q->runningTotal(), 3);
    QCOMPARE(h.q->queuedTotal(), 24);
    h.finishAll();
    for (const auto& j : h.all) QVERIFY(j->endedAt >= 0 && j->err.isEmpty());
    QCOMPARE(h.starts.size(), 27);
    QCOMPARE(h.peakHeavy, 3);                            // the heavy limit held for the whole run
    QString seq;
    for (const auto& j : h.starts) if (j->session != u"H"_s) seq += j->session;
    // every project gets its turn: whenever a project starts a job, no other project still waiting has fewer starts
    QHash<QString, int> counts, waiting{{u"A"_s, 12}, {u"B"_s, 3}, {u"C"_s, 3}, {u"D"_s, 3}, {u"E"_s, 3}};
    bool fair = true;
    for (const auto& j : h.starts) {
        if (j->session == u"H"_s) continue;
        const int mine = counts.value(j->session);
        for (auto it = waiting.cbegin(); it != waiting.cend(); ++it)
            if (it.key() != j->session && it.value() > 0 && counts.value(it.key()) + 1 < mine) fair = false;
        counts[j->session] = mine + 1;
        waiting[j->session] -= 1;
    }
    QVERIFY2(fair, qPrintable(seq));
    QVERIFY2(seq.startsWith(u"ABCDEABCDEABCDE") && seq.mid(15).count(u'A') == seq.mid(15).size(), qPrintable(seq));
    auto lastOf = [&](const QString& s) { qint64 m = -1; for (const auto& j : h.all) if (j->session == s) m = std::max(m, j->endedAt); return m; };
    for (const QString& s : {u"B"_s, u"C"_s, u"D"_s, u"E"_s}) QVERIFY(lastOf(s) < lastOf(u"A"_s));
    auto lastStart = [&](const QString& s) { qint64 m = -1; for (const auto& j : h.all) if (j->session == s) m = std::max(m, j->startedAt); return m; };
    for (const QString& s : {u"B"_s, u"C"_s, u"D"_s, u"E"_s}) QVERIFY(lastStart(s) < lastStart(u"A"_s) - 20);
}

void TestStudioRunQueue::poolsIndependent() {
    Harness h(1, 2);
    h.submit(u"A"_s, SlotKind::Heavy, 100);
    h.submit(u"B"_s, SlotKind::Heavy, 100);
    auto l1 = h.submit(u"C"_s, SlotKind::Light, 5), l2 = h.submit(u"D"_s, SlotKind::Light, 5), l3 = h.submit(u"E"_s, SlotKind::Light, 5);
    QVERIFY(l1->startedAt == 0 && l2->startedAt == 0 && l3->startedAt < 0);   // a full heavy pool does not delay light work
    h.clock.advance(6);
    QCOMPARE(l3->startedAt, 5);                          // light work flows through its own two slots
    QVERIFY(h.queuedSessions().contains(u"B"_s));        // the second heavy job still waits (the pool is its own)
    h.finishAll();
}

void TestStudioRunQueue::boardCancelAndRunNext() {
    Harness h(1, 1);
    auto hold = h.submit(u"A"_s, SlotKind::Heavy, 50);
    auto b = h.submit(u"B"_s, SlotKind::Heavy, 10), c = h.submit(u"C"_s, SlotKind::Heavy, 10), d = h.submit(u"D"_s, SlotKind::Heavy, 10);
    QCOMPARE(h.queuedSessions().join(QString()), u"BCD"_s);   // B, C, D wait in arrival order
    QVERIFY(h.q->setPriority(d->id, h.q->topPriority() + 1));
    QCOMPARE(h.queuedSessions().join(QString()), u"DBC"_s);   // "run next" moves it to the front
    QCOMPARE(h.q->queued().first().ahead, 0);
    QCOMPARE(h.q->queued().first().meta.priority, 1);
    QVERIFY(h.q->cancel(c->id));                          // cancel(id) drops one waiting job
    QCOMPARE(c->err, u"Cancelled while waiting for a free slot"_s);
    QCOMPARE(h.queuedSessions().join(QString()), u"DB"_s);
    QCOMPARE(h.q->metrics().cancelled, 1);
    QVERIFY(!h.q->cancel(hold->id));                      // of a job that already runs: nothing
    QCOMPARE(h.q->runningTotal(), 1);
    QVERIFY(!h.q->cancel(99999) && !h.q->setPriority(99999, 5));
    h.finishAll();
    QVERIFY(d->startedAt < b->startedAt && c->startedAt < 0);
    QCOMPARE(h.q->runningTotal(), 0);
    QCOMPARE(h.q->metrics().released, 3);
    // a cancel must not let a second job slip in beyond the limit
    Harness g(1, 1);
    g.submit(u"A"_s, SlotKind::Heavy, 20);
    g.submit(u"B"_s, SlotKind::Heavy, 20);
    g.submit(u"C"_s, SlotKind::Heavy, 20);
    g.q->cancel(g.q->queued().first().id);
    QCOMPARE(g.q->runningTotal(), 1);
    QCOMPARE(g.peakHeavy, 1);
    g.finishAll();
}

void TestStudioRunQueue::sessionCancelGroupAndWhy() {
    Harness h(3, 8);
    SlotMeta render, assets;
    render.group = u"render"_s;
    render.groupLimit = 1;
    render.phase = u"render"_s;
    assets.phase = u"assets"_s;
    auto r1 = h.submit(u"A"_s, SlotKind::Heavy, 30, u"render A"_s, render);
    auto r2 = h.submit(u"B"_s, SlotKind::Heavy, 30, u"render B"_s, render);
    auto o1 = h.submit(u"C"_s, SlotKind::Heavy, 30, u"assets C"_s, assets);
    QCOMPARE(h.q->runningTotal(), 2);                    // two renders never run together even with free heavy slots
    int renders = 0;
    for (const SlotInfo& s : h.q->running()) renders += s.meta.group == u"render"_s;
    QCOMPARE(renders, 1);
    const QList<WaitInfo> st = h.q->queued();
    QCOMPARE(st.size(), 1);
    QCOMPARE(st.first().why, u"group"_s);                // the second render waits for its group, and says so
    QCOMPARE(st.first().meta.session, u"B"_s);
    const QJsonObject groups = h.q->status().value(u"groups"_s).toObject();
    QCOMPARE(groups.value(u"render"_s).toObject().value(u"running"_s).toInt(), 1);
    QCOMPARE(groups.value(u"render"_s).toObject().value(u"limit"_s).toInt(), 1);
    QCOMPARE(o1->startedAt, 0);                          // the other heavy job was not blocked behind the waiting render
    h.submit(u"D"_s, SlotKind::Heavy, 30);
    h.submit(u"E"_s, SlotKind::Heavy, 30);
    bool pool = false;
    for (const WaitInfo& w : h.q->queued()) pool = pool || ((w.meta.session == u"D"_s || w.meta.session == u"E"_s) && w.why == u"pool"_s);
    QVERIFY(pool);                                       // when every heavy slot is taken the reason is the pool
    QCOMPARE(h.q->cancelSession(u"B"_s), 1);             // drops everything a project has waiting, not what runs
    bool aRuns = false;
    for (const SlotInfo& s : h.q->running()) aRuns = aRuns || s.meta.session == u"A"_s;
    QVERIFY(aRuns && !h.queuedSessions().contains(u"B"_s));
    QVERIFY(r2->err.startsWith(u"The project was closed"));
    h.finishAll();
    QVERIFY(r1->endedAt >= 0 && o1->endedAt >= 0);
}

void TestStudioRunQueue::liveLimitsFromProfile() {
    Harness h(1, 2);
    QList<std::shared_ptr<VJob>> jobs;
    for (int i = 0; i < 8; ++i) jobs << h.submit(u"P%1"_s.arg(i % 4), SlotKind::Heavy, 20);
    QCOMPARE(h.q->runningTotal(), 1);
    QCOMPARE(h.q->queuedTotal(), 7);
    ApplyTargets t;
    t.queue = h.q.get();
    applyProfile(resolveProfile(in(hw(16, 20000))), t);  // the fast tier: 6 heavy
    QCOMPARE(h.q->runningTotal(), 6);
    QCOMPARE(h.q->limits().heavy, 6);
    applyProfile(resolveProfile(in(hw(2, 1000))), t);    // a smaller one never takes a running job's slot away
    QCOMPARE(h.q->runningTotal(), 6);
    QCOMPARE(h.q->limits().heavy, 1);
    h.clock.advance(21);
    QVERIFY(h.q->runningTotal() <= 2);                   // nothing new starts until fewer than the new limit are running
    h.finishAll();
    for (const auto& j : jobs) QVERIFY(j->endedAt >= 0 && j->err.isEmpty());
    resetProfile(t);
    QCOMPARE(h.q->limits().heavy, 3);
}

void TestStudioRunQueue::fuzzRandomOperations() {
    int badRuns = 0;
    for (int seed = 1; seed <= 60; ++seed) {
        QRandomGenerator rnd(quint32(seed * 7919));
        auto r = [&] { return rnd.generateDouble(); };
        Harness h(1 + int(r() * 4), 1 + int(r() * 5));
        const QStringList sessions{u"p1"_s, u"p2"_s, u"p3"_s, u"p4"_s, u"p5"_s};
        std::vector<std::unique_ptr<AbortController>> ctls;
        QString bad;
        int maxHeavy = h.q->limits().heavy, maxLight = h.q->limits().light;
        auto audit = [&] {
            // limits can be lowered under running jobs, so the check is against the highest limit ever set
            if (h.q->runningCount(SlotKind::Heavy) > maxHeavy && bad.isEmpty()) bad = u"heavy over"_s;
            if (h.q->runningCount(SlotKind::Light) > maxLight && bad.isEmpty()) bad = u"light over"_s;
            QSet<quint64> ids;
            for (const SlotInfo& s : h.q->running()) ids.insert(s.id);
            if (ids.size() != h.q->runningTotal() && bad.isEmpty()) bad = u"a slot is listed twice"_s;
        };
        const int steps = 40 + int(r() * 60);
        for (int i = 0; i < steps; ++i) {
            const double roll = r();
            if (roll < 0.45) {
                auto ctl = std::make_unique<AbortController>();
                const SlotKind kind = r() < 0.6 ? SlotKind::Heavy : SlotKind::Light;
                const QString s = sessions[int(r() * sessions.size())];
                SlotMeta m;
                const double x = r();
                if (x < 0.15) { m.group = u"render"_s; m.groupLimit = 1; }
                else if (x < 0.25) m.priority = int(r() * 3);
                h.submit(s, kind, 1 + int(r() * 30), {}, m, ctl->signal());
                ctls.push_back(std::move(ctl));
            } else if (roll < 0.55 && !ctls.empty()) {
                ctls[int(r() * ctls.size())]->abort();
            } else if (roll < 0.62) {
                const auto w = h.q->queued();
                if (!w.isEmpty()) h.q->cancel(w[int(r() * w.size())].id);
            } else if (roll < 0.68) {
                const auto w = h.q->queued();
                if (!w.isEmpty()) h.q->setPriority(w[int(r() * w.size())].id, int(r() * 5));
            } else if (roll < 0.72) {
                h.q->cancelSession(sessions[int(r() * sessions.size())]);
            } else if (roll < 0.78) {
                const int heavy = 1 + int(r() * 5), light = 1 + int(r() * 6);
                h.q->setLimits(heavy, light);
                maxHeavy = std::max(maxHeavy, heavy);
                maxLight = std::max(maxLight, light);
            } else if (roll < 0.84 && !h.starts.isEmpty()) {
                const auto j = h.starts[int(r() * h.starts.size())];   // an early and a double release
                h.q->release(j->id);
                h.q->release(j->id);
            } else {
                h.clock.advance(int(r() * 15));
            }
            audit();
        }
        h.finishAll();
        if (h.q->runningTotal() || h.q->queuedTotal()) bad = bad.isEmpty() ? u"left over"_s : bad;
        for (const auto& j : h.all) if (j->endedAt < 0 && bad.isEmpty()) bad = u"a job never finished or was refused"_s;
        const QueueMetrics m = h.q->metrics();
        if ((m.released > m.acquired || m.acquired != h.starts.size()) && bad.isEmpty()) bad = u"metrics"_s;
        if ((m.runningNow != 0 || m.queuedNow != 0) && bad.isEmpty()) bad = u"metrics are not zero at the end"_s;
        if (!bad.isEmpty()) { ++badRuns; if (badRuns < 4) qWarning("seed %d: %s", seed, qPrintable(bad)); }
    }
    QCOMPARE(badRuns, 0);
}

// =====================================================================================================================

void TestStudioRunQueue::tiers() {
    auto t = [](std::optional<int> c, std::optional<qint64> r) { return tierFor(hw(c, r)); };
    QVERIFY(t(2, 16000) == Tier::Lean && t(8, 1500) == Tier::Lean && t(1, 500) == Tier::Lean);
    QVERIFY(t(4, 3000) == Tier::Safe && t(8, 4000) == Tier::Safe);
    QVERIFY(t(6, 8000) == Tier::Balanced && t(16, 6144) == Tier::Balanced);
    QVERIFY(t(12, 16000) == Tier::Fast && t(32, 12288) == Tier::Fast);
    QVERIFY(t(32, 5046) == Tier::Safe && t(3, 64000) == Tier::Lean);   // the smaller of cores and RAM decides
    QCOMPARE(t(std::nullopt, std::nullopt), Tier::Safe);               // nothing known = safe, never more
    QVERIFY(t(32, std::nullopt) == Tier::Safe && t(std::nullopt, 64000) == Tier::Safe);
    QVERIFY(t(-1, -5) == Tier::Safe && tierFor(Hardware{}) == Tier::Safe);   // junk figures are unknown
    QCOMPARE(tierName(Tier::Balanced), u"balanced"_s);
    Tier parsed = Tier::Lean;
    QVERIFY(parseTier(u"FAST", &parsed) && parsed == Tier::Fast && !parseTier(u"ludicrous", &parsed));
    QVERIFY(isMode(u"auto") && isMode(u"Lean") && !isMode(u"9"));
}

void TestStudioRunQueue::safeTierExact() {
    const Profile p = resolveProfile(in(hw(8, 4000)));
    QCOMPARE(shape(p), u"q:3/8 g:3/4/4/8 s:4 w:6"_s);
    QCOMPARE(p.gates.poll, 8);
    const QHash<QString, int>& b = baseValues();
    QVERIFY(b.value(u"queue.heavy"_s) == 3 && b.value(u"queue.light"_s) == 8 && b.value(u"gates.videos"_s) == 3 &&
            b.value(u"gates.images"_s) == 4 && b.value(u"gates.audio"_s) == 4 && b.value(u"gates.chat"_s) == 8 &&
            b.value(u"steps"_s) == 4 && b.value(u"workers"_s) == 6);
    for (auto it = p.sources.cbegin(); it != p.sources.cend(); ++it) QCOMPARE(it.value(), u"default"_s);
    QCOMPARE(shape(resolveProfile(in(hw(std::nullopt, std::nullopt)))), shape(p));   // an unknown machine gets the same
    // the run queue, the gates and the graph start from the same constants (nothing scattered disagrees)
    FakeClock clock;
    RunQueue q(clock);
    const provider::GateLimits g;
    QVERIFY(q.limits().heavy == p.queue.heavy && q.limits().light == p.queue.light);
    QVERIFY(g[provider::GateKind::Videos] == p.gates.videos && g[provider::GateKind::Images] == p.gates.images &&
            g[provider::GateKind::Audio] == p.gates.audio && g[provider::GateKind::Chat] == p.gates.chat &&
            g[provider::GateKind::Poll] == p.gates.poll);
    QCOMPARE(kBaseSteps, p.steps);
    QVERIFY2(p.warnings.isEmpty(), qPrintable(p.warnings.join(u'|')));
    QCOMPARE(p.mode, u"auto"_s);
    QCOMPARE(p.tier, Tier::Safe);
    QCOMPARE(p.hardware.source, u"test"_s);
    QCOMPARE(p.toJson().value(u"line"_s).toString(), describeProfile(p));
}

void TestStudioRunQueue::otherTiersScale() {
    const Profile lean = resolveProfile(in(hw(2, 1500))), bal = resolveProfile(in(hw(8, 8000))), fast = resolveProfile(in(hw(24, 24000)));
    QVERIFY2(shape(lean) == u"q:1/4 g:1/2/2/4 s:2 w:2"_s || shape(lean) == u"q:1/4 g:1/2/2/4 s:2 w:3"_s, qPrintable(shape(lean)));
    QCOMPARE(shape(bal), u"q:4/12 g:4/6/6/12 s:6 w:8"_s);
    QCOMPARE(shape(fast), u"q:6/16 g:6/8/8/16 s:8 w:12"_s);
    QCOMPARE(bal.gates.poll, 12);
    QVERIFY(lean.steps <= bal.steps && bal.steps <= fast.steps && lean.queue.heavy <= bal.queue.heavy &&
            bal.queue.heavy <= fast.queue.heavy && lean.gates.images <= bal.gates.images && bal.gates.images <= fast.gates.images);
    QVERIFY(resolveProfile(in(hw(4, 20000))).workers <= 6 && resolveProfile(in(hw(2, 20000))).workers >= 2);
    QVERIFY(bal.sources.value(u"steps"_s) == u"hardware"_s && fast.sources.value(u"gates.videos"_s) == u"hardware"_s);
    ProfileInput asked = in(hw(2, 1000));
    asked.settings = obj({{u"mode"_s, u"fast"_s}});
    const Profile pf = resolveProfile(asked);
    QVERIFY(pf.tier == Tier::Fast && pf.mode == u"fast"_s && pf.sources.value(u"steps"_s) == u"mode"_s && pf.steps == 8);
    QVERIFY(resolveProfile(in(hw(8, 8000))).mode == u"auto"_s && resolveProfile(in(hw(8, 8000))).tier == Tier::Balanced);
}

void TestStudioRunQueue::validationAndClamping() {
    ProfileInput i = in(hw(8, 4000));
    i.ranges = Ranges::Browser;                          // the TS ranges
    i.settings = obj({{u"queue"_s, obj({{u"heavy"_s, 99}, {u"light"_s, 0}})},
                      {u"gates"_s, obj({{u"videos"_s, -3}, {u"images"_s, 2.9}, {u"audio"_s, u"7"_s}, {u"chat"_s, u"many"_s}})},
                      {u"steps"_s, 1e9},
                      {u"workers"_s, true}});
    const Profile p = resolveProfile(i);
    QVERIFY2(p.queue.heavy == 12 && anyWarning(p.warnings, u"queue.heavy: 99 is outside 1..12, using 12"_s), qPrintable(p.warnings.join(u'|')));
    QVERIFY(p.queue.light == 1 && p.gates.videos == 1);  // zero and negative are clamped to the minimum 1
    QCOMPARE(p.gates.images, 2);                         // a fraction is floored
    QCOMPARE(p.gates.audio, 7);                          // a number in a string is read
    QVERIFY2(p.gates.chat == 8 && anyWarning(p.warnings, u"gates.chat: \"many\" is not a number"_s), qPrintable(p.warnings.join(u'|')));
    QCOMPARE(p.steps, 16);                               // steps are clamped to 16 in a browser
    QVERIFY(p.workers == 6 && anyWarning(p.warnings, u"workers"_s));   // a boolean is not a number
    ProfileInput j = in(hw(8, 4000));
    j.settings = u"rm -rf"_s;
    j.env = {{u"STARMEDIA_CONCURRENCY"_s, u"ludicrous"_s}, {u"STARMEDIA_GATE_IMAGES"_s, QString()}};
    const Profile junk = resolveProfile(j);
    QVERIFY2(shape(junk) == u"q:3/8 g:3/4/4/8 s:4 w:6"_s && junk.warnings.size() >= 2, qPrintable(junk.warnings.join(u'|')));
    QVERIFY(anyWarning(junk.warnings, u"settings: expected an object, ignored"_s));
    QVERIFY(anyWarning(junk.warnings, u"STARMEDIA_CONCURRENCY: \"ludicrous\" is not one of auto, lean, safe, balanced, fast, ignored"_s));
    ProfileInput k = in(hw(8, 4000));
    k.settings = QJsonArray{1, 2};
    k.server = QJsonValue::Null;
    k.mode = u"9"_s;
    k.env = {{u"STARMEDIA_EXEC_STEPS"_s, u"3.7e0"_s}};
    const Profile pk = resolveProfile(k);               // arrays, null and nonsense modes never throw
    QCOMPARE(pk.steps, 3);
    QVERIFY(anyWarning(pk.warnings, u"mode \"9\" ignored (use one of auto, lean, safe, balanced, fast)"_s));
    const Cleaned fl = cleanSettings(obj({{u"steps"_s, true}, {u"queue"_s, obj({{u"heavy"_s, u"x"_s}})}, {u"gates"_s, obj({{u"videos"_s, false}})}}));
    QVERIFY2(fl.values.isEmpty() && fl.warnings.size() == 3, qPrintable(fl.warnings.join(u'|')));
    QVERIFY(!readInt(QJsonValue(true)) && !readInt(u"Infinity"_s) && !readInt(u""_s) && readInt(u" 4 "_s) == 4 &&
            readInt(u"1e3"_s) == 1000 && readInt(QJsonValue(7.9)) == 7 && readInt(u"-2.5"_s) == -3);
    // a mode in the settings that is not a string, and an empty one
    const Cleaned m1 = cleanSettings(obj({{u"mode"_s, 5}})), m2 = cleanSettings(obj({{u"mode"_s, QString()}}));
    QVERIFY(m1.mode.isEmpty() && m1.warnings.size() == 1 && m2.mode.isEmpty() && m2.warnings.isEmpty());
}

void TestStudioRunQueue::layers() {
    const Hardware h = hw(16, 20000);                    // fast
    const Profile base = resolveProfile(in(h));
    QVERIFY(base.gates.images == 8 && base.sources.value(u"gates.images"_s) == u"hardware"_s);
    ProfileInput si = in(h);
    si.server = obj({{u"gates"_s, obj({{u"images"_s, 5}})}});
    const Profile server = resolveProfile(si);
    QVERIFY(server.gates.images == 5 && server.sources.value(u"gates.images"_s) == u"server"_s);
    si.settings = obj({{u"gates"_s, obj({{u"images"_s, 3}})}});
    const Profile setting = resolveProfile(si);
    QVERIFY(setting.gates.images == 3 && setting.sources.value(u"gates.images"_s) == u"setting"_s);
    si.env = {{u"STARMEDIA_GATE_IMAGES"_s, u"2"_s}};
    const Profile env = resolveProfile(si);
    QVERIFY(env.gates.images == 2 && env.sources.value(u"gates.images"_s) == u"env"_s);   // the environment beats everything
    QVERIFY(env.gates.videos == 6 && env.queue.heavy == 6 && env.sources.value(u"steps"_s) == u"hardware"_s);
    ProfileInput bi = in(h);
    bi.settings = obj({{u"gates"_s, obj({{u"images"_s, 3}})}});
    bi.env = {{u"STARMEDIA_GATE_IMAGES"_s, u"lots"_s}};
    QCOMPARE(resolveProfile(bi).gates.images, 3);        // a bad value in a high layer falls through
    ProfileInput mi = in(h);
    mi.settings = obj({{u"mode"_s, u"lean"_s}});
    mi.env = {{u"STARMEDIA_CONCURRENCY"_s, u"balanced"_s}};
    const Profile mode = resolveProfile(mi);
    QVERIFY(mode.tier == Tier::Balanced && mode.mode == u"balanced"_s);
    // a mode chosen on the adopting side is not undone by numbers the publisher derived (those are not published)
    {
        const QJsonObject published = publishProfile(resolveProfile(in(h)));
        ProfileInput page = adoptPublished(published);
        page.settings = obj({{u"mode"_s, u"lean"_s}});
        const Profile pp = resolveProfile(page);
        QVERIFY2(pp.tier == Tier::Lean && pp.gates.images == 2 && published.value(u"overrides"_s).toObject().isEmpty(),
                 qPrintable(QString::fromUtf8(QJsonDocument(published).toJson())));
        QCOMPARE(pp.hardware.cores.value_or(0), 16);
    }
    // but a number somebody chose on the publishing side does reach the adopter
    {
        ProfileInput pi = in(h);
        pi.env = {{u"STARMEDIA_QUEUE_HEAVY"_s, u"2"_s}};
        const QJsonObject published = publishProfile(resolveProfile(pi));
        const Profile page = resolveProfile(adoptPublished(published));
        QVERIFY(page.queue.heavy == 2 && page.sources.value(u"queue.heavy"_s) == u"server"_s && page.gates.images == 8);
        QCOMPARE(published.value(u"line"_s).toString(), describeProfile(resolveProfile(pi)));
    }
    QCOMPARE(describeProfile(base), u"fast (auto): queue 6 heavy / 16 light, gates 6 video / 8 image / 8 audio / 16 text, 8 steps per run, 12 workers"_s);
    const ProfileInput junk = adoptPublished(QJsonValue(u"junk"_s));
    QVERIFY(!junk.hardware && !junk.server.isObject() && resolveProfile(junk).warnings.isEmpty());
}

void TestStudioRunQueue::heavyCapAndKindSteps() {
    const Hardware h = hw(16, 20000);
    ProfileInput ci = in(h);
    ci.heavyCap = 4;
    const Profile cap = resolveProfile(ci);
    QVERIFY(cap.gates.videos == 4 && cap.gates.images == 4 && cap.gates.audio == 4 && cap.gates.chat == 16);
    ci.settings = obj({{u"gates"_s, obj({{u"images"_s, 9}})}});
    const Profile explicitP = resolveProfile(ci);
    QVERIFY2(explicitP.gates.images == 9 && anyWarning(explicitP.warnings, u"gates.images is 9 but the server runs at most 4"_s),
             qPrintable(explicitP.warnings.join(u'|')));
    ProfileInput neg = in(h);
    neg.heavyCap = -2;
    QVERIFY(cap.heavyCap == 4 && resolveProfile(in(h)).heavyCap == 0 && resolveProfile(neg).heavyCap == 0);
    ProfileInput ki = in(hw(8, 4000));
    ki.settings = obj({{u"steps"_s, 3}, {u"gates"_s, obj({{u"images"_s, 2}, {u"videos"_s, 8}, {u"audio"_s, 1}})}});
    const Profile p = resolveProfile(ki);
    QVERIFY2(p.kindSteps.value(u"image"_s) == 2 && p.kindSteps.value(u"video"_s) == 3 && p.kindSteps.value(u"tts"_s) == 1 &&
                 p.kindSteps.value(u"stt"_s) == 1 && p.kindSteps.value(u"text"_s) == 3 && p.kindSteps.value(u"json"_s) == 3,
             qPrintable(QString::fromUtf8(QJsonDocument(p.toJson().value(u"kindSteps"_s).toObject()).toJson())));
    QVERIFY(!p.kindSteps.contains(u"local"_s));          // local stages are not capped by a gate
    // the gate limits a profile hands to Gates keep the sub-gate widths and, without a cap, the cap in force
    provider::GateLimits keep;
    keep.perProvider[size_t(provider::GateKind::Chat)] = 4;
    keep.heavyCap = 7;
    const provider::GateLimits gl = p.gateLimits(keep);
    QVERIFY(gl[provider::GateKind::Images] == 2 && gl[provider::GateKind::Videos] == 8 && gl.heavyCap == 7 &&
            gl.perProvider[size_t(provider::GateKind::Chat)] == 4);
    QCOMPARE(cap.gateLimits(keep).heavyCap, 4);
    const GraphLimits graph = p.graphLimits();
    QVERIFY(graph.steps == 3 && graph.perKind.value(u"image"_s) == 2);
}

void TestStudioRunQueue::workers() {
    const Hardware h = hw(8, 8000);
    QVERIFY(workersFor(4, in(h)).workers == 4 && workersFor(32, in(h)).workers == 32 && workersFor(u"5"_s, in(h)).workers == 5);
    QCOMPARE(workersFor(4, in(h)).source, u"config"_s);
    QCOMPARE(workersFor(QJsonValue(), in(hw(24, 30000))).workers, 6);   // nothing configured is the safe 6
    QVERIFY(workersFor(u"auto"_s, in(hw(24, 30000))).workers == 12 && workersFor(u"auto"_s, in(h)).workers == 8 &&
            workersFor(u"AUTO"_s, in(hw(2, 800))).workers <= 3);
    QCOMPARE(workersFor(u"auto"_s, in(hw(std::nullopt, std::nullopt))).workers, 6);
    ProfileInput e3 = in(h), e9 = in(h);
    e3.env = {{u"STARMEDIA_WORKERS"_s, u"3"_s}};
    e9.env = {{u"STARMEDIA_WORKERS"_s, u"9"_s}};
    QVERIFY(workersFor(6, e3).workers == 3 && workersFor(u"auto"_s, e9).workers == 9 && workersFor(6, e3).source == u"env"_s);
    const WorkersResult bad = workersFor(99, in(h));
    QVERIFY2(bad.workers == 6 && anyWarning(bad.warnings, u"workers 99"_s) && workersFor(u"lots"_s, in(h)).workers == 6,
             qPrintable(bad.warnings.join(u'|')));
    ProfileInput e99 = in(h);
    e99.env = {{u"STARMEDIA_WORKERS"_s, u"99"_s}};
    const WorkersResult r = workersFor(u"auto"_s, e99);
    QVERIFY2(r.workers == 32 && anyWarning(r.warnings, u"STARMEDIA_WORKERS"_s), qPrintable(r.warnings.join(u'|')));
    ProfileInput fm = in(hw(2, 800));
    fm.settings = obj({{u"mode"_s, u"fast"_s}});
    const int w = workersFor(u"auto"_s, fm).workers;
    QVERIFY(w == 2 || w == 3);
}

void TestStudioRunQueue::readingTheMachine() {
    const Hardware real = detectHardware();
    QVERIFY(real.cores && *real.cores > 0);
    QCOMPARE(real.source, u"os"_s);
#ifdef Q_OS_WIN
    QVERIFY(real.freeRamMb && *real.freeRamMb > 0 && real.totalRamMb && *real.totalRamMb >= *real.freeRamMb);
#endif
    const Profile p = resolveProfile();                  // the real machine gives a valid profile
    QVERIFY(p.steps >= 1 && p.steps <= 64 && p.workers >= 1 && p.workers <= 32 && p.hardware.source == u"os"_s);
    const Hardware fromJson = Hardware::fromJson(obj({{u"cores"_s, 12}, {u"freeRamMb"_s, 9216}, {u"totalRamMb"_s, QJsonValue::Null}}));
    QVERIFY(fromJson.cores == 12 && fromJson.freeRamMb == 9216 && !fromJson.totalRamMb && fromJson.source == u"given"_s);
    QCOMPARE(tierFor(fromJson), Tier::Balanced);
    QVERIFY(Hardware::fromJson(obj({{u"cores"_s, 0}, {u"freeRamMb"_s, -1}})).cores == std::nullopt);
    QVERIFY(real.toJson().value(u"cores"_s).toInt() == *real.cores);
    QVERIFY(!processEnv().contains(u"PATH"_s));         // only the profile's own variables
}

void TestStudioRunQueue::applyToTargets() {
    FakeClock clock;
    provider::Gates gates(clock);
    RunQueue q(clock);
    ApplyTargets t;
    t.gates = &gates;
    t.queue = &q;
    const Profile p = resolveProfile(in(hw(8, 8000)));   // balanced
    applyProfile(p, t);
    QVERIFY(q.limits().heavy == 4 && q.limits().light == 12);           // the run queue pools follow the profile
    QVERIFY(gates.gate(provider::GateKind::Videos).configured() == 4 && gates.gate(provider::GateKind::Images).configured() == 6 &&
            gates.gate(provider::GateKind::Audio).configured() == 6 && gates.gate(provider::GateKind::Chat).configured() == 12 &&
            gates.gate(provider::GateKind::Poll).configured() == 12);   // the request gates follow it
    QVERIFY(defaults().steps == 6 && defaults().perKind.value(u"image"_s) == 6 && defaults().perKind.value(u"video"_s) == 6);
    QCOMPARE(currentProfile().tier, Tier::Balanced);
    int told = 0;
    const auto id = onProfileChange([&](const Profile& x) { told += x.steps; });
    ProfileInput s3 = in(hw(8, 8000));
    s3.settings = obj({{u"steps"_s, 3}});
    applyProfile(resolveProfile(s3), t);
    QVERIFY(told == 3 && defaults().steps == 3);          // listeners are told
    removeProfileListener(id);
    applyProfile(p, t);
    QCOMPARE(told, 3);                                   // an unsubscribed listener is not told
    const auto boom = onProfileChange([](const Profile&) { throw std::runtime_error("listener bug"); });
    applyProfile(p, t);                                  // a listener that throws does not stop the others
    removeProfileListener(boom);
    resetProfile(t);
    QVERIFY(q.limits().heavy == 3 && q.limits().light == 8 && gates.gate(provider::GateKind::Images).configured() == 4 && defaults().steps == 4);
    QCOMPARE(currentProfile().tier, Tier::Safe);
    // a published profile with a heavy cap: the adopter's gates are trimmed
    ProfileInput adopted = adoptPublished(obj({{u"hardware"_s, hw(16, 20000).toJson()}, {u"mode"_s, u"auto"_s}, {u"overrides"_s, QJsonObject()}}));
    adopted.heavyCap = 3;
    const Profile ap = applyProfile(resolveProfile(adopted), t);
    QVERIFY(ap.tier == Tier::Fast && ap.heavyCap == 3 && ap.gates.images == 3 && gates.gate(provider::GateKind::Images).configured() == 3 &&
            gates.limits().heavyCap == 3);
    resetProfile(t);
}

void TestStudioRunQueue::fuzzAnyInput() {
    QRandomGenerator rnd(20261022u);
    auto r = [&] { return rnd.generateDouble(); };
    const QList<QJsonValue> junk{QJsonValue(), QJsonValue::Null, u""_s, u"abc"_s, u"7"_s, 7, 7.9, -1, 0, 1e12, true, QJsonArray(), QJsonObject(), u"1e3"_s, u" 4 "_s};
    auto pick = [&](const QList<QJsonValue>& xs) { return xs[int(r() * xs.size())]; };
    const QList<QJsonValue> modes{u"auto"_s, u"fast"_s, u"lean"_s, u"x"_s, 5, QJsonValue()};
    const QList<std::optional<int>> coresL{std::nullopt, 0, 1, 4, 16, 64};
    const QList<std::optional<qint64>> ramL{std::nullopt, 0, 500, 3000, 9000, 40000};
    const QList<int> caps{0, 0, 2, 5, -1};
    int bad = 0;
    for (int i = 0; i < 400; ++i) {
        QJsonObject settings;
        const QJsonValue mode = pick(modes);
        if (!mode.isUndefined()) settings.insert(u"mode"_s, mode);
        settings.insert(u"queue"_s, obj({{u"heavy"_s, pick(junk)}, {u"light"_s, pick(junk)}}));
        settings.insert(u"gates"_s, obj({{u"videos"_s, pick(junk)}, {u"images"_s, pick(junk)}, {u"audio"_s, pick(junk)}, {u"chat"_s, pick(junk)}, {u"poll"_s, pick(junk)}}));
        settings.insert(u"steps"_s, pick(junk));
        settings.insert(u"workers"_s, pick(junk));
        ProfileInput inp = in(hw(coresL[int(r() * coresL.size())], ramL[int(r() * ramL.size())]));
        inp.ranges = r() < 0.5 ? Ranges::Browser : Ranges::Server;
        inp.settings = settings;
        const int which = int(r() * 3);
        inp.server = which == 0 ? QJsonValue::Null : which == 1 ? QJsonValue(QJsonObject()) : QJsonValue(settings);
        inp.env = {{u"STARMEDIA_EXEC_STEPS"_s, QStringList{u""_s, u"x"_s, u"3"_s, u"99"_s}[int(r() * 4)]},
                   {u"STARMEDIA_CONCURRENCY"_s, QStringList{u""_s, u"fast"_s, u"nope"_s}[int(r() * 3)]}};
        inp.heavyCap = caps[int(r() * caps.size())];
        const Profile p = resolveProfile(inp);
        bool ok = true;
        for (const Field& f : fields(inp.ranges)) {
            const int v = p.value(f.key);
            ok = ok && v >= f.min && v <= f.max;
        }
        for (auto it = p.kindSteps.cbegin(); it != p.kindSteps.cend(); ++it) ok = ok && it.value() >= 1 && it.value() <= p.steps;
        ok = ok && p.kindSteps.value(u"image"_s) <= p.gates.images && p.kindSteps.value(u"video"_s) == p.steps &&
             p.kindSteps.value(u"tts"_s) <= p.gates.audio && p.kindSteps.value(u"text"_s) <= p.gates.chat;
        if (!ok) { ++bad; if (bad < 4) qWarning("bad: %s", qPrintable(shape(p))); }
    }
    QCOMPARE(bad, 0);
}

void TestStudioRunQueue::serverRanges() {
    // a server honours wider numbers than a browser page would (design §4.3); the defaults and tiers are the same
    ProfileInput s = in(hw(8, 4000));
    s.env = {{u"STARMEDIA_GATE_CHAT"_s, u"64"_s}, {u"STARMEDIA_GATE_POLL"_s, u"20"_s}, {u"STARMEDIA_QUEUE_LIGHT"_s, u"100"_s}};
    const Profile server = resolveProfile(s);
    QVERIFY(server.gates.chat == 64 && server.sources.value(u"gates.chat"_s) == u"env"_s && server.gates.poll == 20 && server.queue.light == 100);
    QVERIFY2(server.warnings.isEmpty(), qPrintable(server.warnings.join(u'|')));
    s.ranges = Ranges::Browser;
    const Profile browser = resolveProfile(s);
    QVERIFY2(browser.gates.chat == 32 && browser.queue.light == 32 && browser.gates.poll == 20 &&
                 anyWarning(browser.warnings, u"STARMEDIA_GATE_CHAT: 64 is outside 1..32, using 32"_s),
             qPrintable(browser.warnings.join(u'|')));
    for (const Field& f : fields(Ranges::Server)) {
        if (f.key.startsWith(u"gates.")) {
            const auto k = provider::gateKindFromName(QStringView(f.key).mid(6));
            QVERIFY(k.has_value());
            QCOMPARE(f.max, provider::GateLimits::maxWidth(*k));   // the gates never clamp what the profile let through
        }
        QVERIFY(baseValues().contains(f.key));
        QVERIFY(field(f.key, Ranges::Browser) && field(f.key, Ranges::Browser)->max <= f.max);
    }
    QVERIFY(!field(u"nope"));
    QCOMPARE(fields(Ranges::Server).size(), 9);
    // the widened numbers go through to the gates without a clamp warning
    FakeClock clock;
    provider::Gates gates(clock);
    ProfileInput wide = in(hw(32, 64000));
    wide.settings = obj({{u"gates"_s, obj({{u"chat"_s, 200}, {u"videos"_s, 40}})}});
    const Profile wp = resolveProfile(wide);
    QCOMPARE(gates.configure(wp.gateLimits(gates.limits())).size(), 0);
    QVERIFY(gates.gate(provider::GateKind::Chat).configured() == 200 && gates.gate(provider::GateKind::Videos).configured() == 40);
    QVERIFY(wp.toJson().value(u"ranges"_s).toString() == u"server"_s);
}

QTEST_GUILESS_MAIN(TestStudioRunQueue)
#include "test_studio_runqueue.moc"
