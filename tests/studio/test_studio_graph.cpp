// test_studio_graph (ST-09): the dynamic graph scheduler.
//
// Part 1 is the TS scheduler suite (starmedia test/concurrency-exec.ts, "the scheduler on a virtual clock") on a
// FakeClock: dependency order, steps, per-kind caps, lanes, failure isolation, Stop, a cycle, deterministic starts, the
// budget ceiling with reserve(), 150 random graphs, the process defaults and the metrics. Part 2 covers what the C++
// port adds: priority / orderKey order, defer, expand() at run time, the rebuild of the ready queue from persisted
// states, afterRound ending a run, settleLater from a worker, the state transitions, and 10k nodes under 1 s.
// Hermetic: no network, no files, no processes.
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QThread>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "core/clock.h"
#include "provider/gates.h"
#include "sched/graph.h"

using namespace studio;
using namespace studio::sched;
using studio::provider::AbortController;
using namespace Qt::Literals::StringLiterals;

namespace {

struct N {
    QString id;
    int dur = 10;
    QStringList deps;
    QString kind = u"text"_s;
    QString lane;
    bool fail = false;
    qint64 est = 0;                        // micro-USD
    bool optional = false;
    QStringList requires;
    int priority = 0;
    QString orderKey;
    NodeState state = NodeState::Pending;
};

N n(const QString& id, int dur, const QStringList& deps = {}, const QString& kind = u"text"_s) {
    N x;
    x.id = id;
    x.dur = dur;
    x.deps = deps;
    x.kind = kind;
    return x;
}

GraphNode toNode(const N& x) {
    GraphNode g;
    g.id = x.id;
    g.deps = x.deps;
    g.kind = x.kind;
    g.lane = x.lane;
    g.priority = x.priority;
    g.orderKey = x.orderKey;
    g.state = x.state;
    return g;
}

struct Ev {
    qint64 t;
    QString id;
    QString ev;                            // start | end | skip | fail
};

struct DriveOptions {
    int steps = 4;
    QHash<QString, int> perKind;
    qint64 abortAt = -1;
    std::function<std::optional<QString>(const N&)> skip;
    std::function<std::optional<QString>(const N&, qint64 spent, qint64 inflight)> admit;
    std::function<bool(const N&)> defer;
    std::function<void(const N&)> settled;
    bool useReserve = true;
};

struct Drive {
    QList<Ev> log;
    std::optional<Outcome> out;
    int peak = 0;
    QHash<QString, int> peakKind;
    qint64 now = 0;
    qint64 spent = 0;
    QSet<QString> finished;
    QSet<QString> failedIds;
    int starts() const {
        int c = 0;
        for (const Ev& e : log) c += e.ev == u"start";
        return c;
    }
    QStringList startOrder() const {
        QStringList o;
        for (const Ev& e : log) if (e.ev == u"start") o << e.id;
        return o;
    }
    int count(const QString& ev) const {
        int c = 0;
        for (const Ev& e : log) c += e.ev == ev;
        return c;
    }
    bool has(const QString& id, const QString& ev) const {
        for (const Ev& e : log) if (e.id == id && e.ev == ev) return true;
        return false;
    }
    qint64 start(const QString& id) const {
        for (const Ev& e : log) if (e.id == id && e.ev == u"start") return e.t;
        return -1;
    }
    qint64 end(const QString& id) const {
        for (const Ev& e : log) if (e.id == id && (e.ev == u"end" || e.ev == u"fail")) return e.t;
        return -1;
    }
    QString logText() const {
        QStringList s;
        for (const Ev& e : log) s << u"%1@%2:%3"_s.arg(e.id).arg(e.t).arg(e.ev);
        return s.join(u' ');
    }
};

/// The TS drive(): runs a graph on a FakeClock; a stopped step winds down at once, as a provider call does.
Drive drive(const QList<N>& nodes, const DriveOptions& o) {
    Drive r;
    FakeClock clock;
    AbortController ctl;
    QHash<QString, N> byId;
    for (const N& x : nodes) byId.insert(x.id, x);
    QObject ctx;
    int running = 0;
    QHash<QString, int> liveKind;
    QHash<QString, qint64> committed;
    QHash<QString, Clock::TimerId> timers;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.skip = [&](const GraphNode& gn) -> std::optional<QString> {
        const N& x = byId[gn.id];
        if (o.skip) {
            if (auto w = o.skip(x)) return w;
        }
        for (const QString& req : x.requires) {
            if (r.failedIds.contains(req) || r.has(req, u"skip"_s)) return u"requires %1"_s.arg(req);
        }
        return std::nullopt;
    };
    h.defer = [&](const GraphNode& gn) { return o.defer ? o.defer(byId[gn.id]) : false; };
    h.admit = [&](const GraphNode& gn) -> std::optional<QString> {
        if (!o.admit) return std::nullopt;
        qint64 inflight = 0;
        for (qint64 v : committed) inflight += v;
        return o.admit(byId[gn.id], r.spent, inflight);
    };
    if (o.useReserve) h.reserve = [&](const GraphNode& gn) { committed.insert(gn.id, byId[gn.id].est); };
    h.run = [&](const GraphNode& gn) {
        const N x = byId[gn.id];
        r.log.append({clock.steadyMs(), x.id, u"start"_s});
        ++running;
        r.peak = std::max(r.peak, running);
        liveKind[x.kind] += 1;
        r.peakKind[x.kind] = std::max(r.peakKind.value(x.kind), liveKind[x.kind]);
        timers.insert(x.id, clock.callAfter(x.dur, &ctx, [&, x] {
            timers.remove(x.id);
            --running;
            liveKind[x.kind] -= 1;
            committed.remove(x.id);
            if (x.fail) {
                r.failedIds.insert(x.id);
                r.log.append({clock.steadyMs(), x.id, u"fail"_s});
                r.finished.insert(x.id);
                g->settle(x.id, NodeState::Failed, u"step failed"_s);
                return;
            }
            r.spent += x.est;
            r.log.append({clock.steadyMs(), x.id, u"end"_s});
            r.finished.insert(x.id);
            g->settle(x.id);
        }));
    };
    h.skipped = [&](const GraphNode& gn, const QString&) {
        r.log.append({clock.steadyMs(), gn.id, u"skip"_s});
        r.finished.insert(gn.id);
    };
    h.failed = [&](const GraphNode& gn, const QString&) {
        r.failedIds.insert(gn.id);
        r.log.append({clock.steadyMs(), gn.id, u"fail"_s});
        r.finished.insert(gn.id);
    };
    h.stopped = [] { return u"AbortError"_s; };
    h.settled = [&](const GraphNode& gn) { if (o.settled) o.settled(byId[gn.id]); };
    h.finished = [&](const Outcome& out) { r.out = out; };
    GraphLimits lim;
    lim.steps = o.steps;
    lim.perKind = o.perKind;
    g = std::make_unique<Graph>(std::move(h), lim, ctl.signal());
    QList<GraphNode> defs;
    for (const N& x : nodes) defs << toNode(x);
    QString err;
    if (!g->add(defs, &err)) qFatal("add failed: %s", qPrintable(err));
    g->start();
    if (o.abortAt >= 0) {
        clock.advance(o.abortAt);
        ctl.abort();
        // the steps in flight hold the signal too: they wind down at once
        const QStringList live = timers.keys();
        for (const QString& id : live) {
            clock.cancel(timers.take(id));
            --running;
            liveKind[byId[id].kind] -= 1;
            committed.remove(id);
            r.log.append({clock.steadyMs(), id, u"end"_s});
            r.finished.insert(id);
            g->settle(id, NodeState::Cancelled);
        }
    }
    int guard = 0;
    while (!r.out && guard++ < 100000) {
        const qint64 next = clock.nextDueSteadyMs();
        if (next < 0) break;
        clock.advance(next - clock.steadyMs());
    }
    r.now = clock.steadyMs();
    return r;
}

} // namespace

class TestStudioGraph : public QObject {
    Q_OBJECT

private slots:
    void init() { resetMetrics(); }

    void depsAndCaps();
    void perKindCaps();
    void lanes();
    void failureIsolation();
    void runThrows();
    void stop();
    void stopBeforeStart();
    void cycleStalls();
    void deterministic();
    void budgetCeilingWithReserve();
    void fuzz();
    void defaultsAndMetrics();
    void priorityAndOrderKey();
    void deferUntilNextSettle();
    void expandAtRunTime();
    void expandTwiceSameParent();
    void expandRefusals();
    void stopFromInsideHook();
    void finishedDeletesGraph();
    void rebuildFromPersistedState();
    void afterRoundEndsRun();
    void settleFromAnotherThread();
    void stateTransitions();
    void perf10k();
};

// ---------------- part 1: the TS suite ----------------

void TestStudioGraph::depsAndCaps() {
    const QList<N> g{n(u"a"_s, 10), n(u"b"_s, 10), n(u"c"_s, 10), n(u"d"_s, 10, {u"a"_s, u"b"_s}),
                     n(u"e"_s, 10, {u"d"_s}), n(u"f"_s, 5, {u"c"_s})};
    DriveOptions o;
    o.steps = 3;
    const Drive r = drive(g, o);
    QVERIFY2(r.starts() == 6 && r.finished.size() == 6 && r.out && r.out->ok, qPrintable(r.logText()));
    for (const N& x : g) {
        for (const QString& d : x.deps) QVERIFY2(r.start(x.id) >= r.end(d), qPrintable(r.logText()));
    }
    QCOMPARE(r.peak, 3);
    QVERIFY(r.start(u"a"_s) == 0 && r.start(u"b"_s) == 0 && r.start(u"c"_s) == 0);
    QCOMPARE(r.startOrder().mid(0, 3).join(u','), u"a,b,c"_s);
    QVERIFY2(r.start(u"d"_s) == 10 && r.start(u"f"_s) == 10 && r.start(u"e"_s) == 20, qPrintable(r.logText()));

    o.steps = 1;
    const Drive one = drive(g, o);
    QCOMPARE(one.peak, 1);
    QCOMPARE(one.now, 55);
    QCOMPARE(one.startOrder().join(u','), u"a,b,c,d,e,f"_s);

    o.steps = 16;
    const Drive wide = drive(g, o);
    QCOMPARE(wide.now, 30);
    QVERIFY(wide.peak <= 6);

    o.steps = 2;
    const Drive gone = drive({n(u"x"_s, 5, {u"gone"_s})}, o);
    QVERIFY(gone.finished.contains(u"x"_s) && gone.out && gone.out->ok);
}

void TestStudioGraph::perKindCaps() {
    QList<N> nodes;
    for (int i = 1; i <= 12; ++i) nodes << n(u"vo%1"_s.arg(i), 5, {}, u"tts"_s);
    for (int i = 1; i <= 6; ++i) nodes << n(u"key%1"_s.arg(i), 10, {}, u"image"_s);
    for (int i = 1; i <= 6; ++i) nodes << n(u"vid%1"_s.arg(i), 30, {u"key%1"_s.arg(i)}, u"video"_s);
    DriveOptions o;
    o.steps = 6;
    o.perKind = {{u"tts"_s, 2}, {u"image"_s, 3}, {u"video"_s, 2}};
    const Drive r = drive(nodes, o);
    QVERIFY2(r.peakKind.value(u"tts"_s) <= 2 && r.peakKind.value(u"image"_s) <= 3 && r.peakKind.value(u"video"_s) <= 2,
             qPrintable(r.logText()));
    QVERIFY(r.peak <= 6);
    QVERIFY2(r.start(u"key1"_s) == 0 && r.start(u"key2"_s) == 0 && r.start(u"key3"_s) == 0, qPrintable(r.logText()));
    QVERIFY(r.peak >= 5);
    QVERIFY(r.out && r.out->ok && r.finished.size() == 24);
    DriveOptions none;
    none.steps = 6;
    const Drive blocked = drive(nodes, none);
    QVERIFY(blocked.start(u"key1"_s) > 0);
    QVERIFY(r.start(u"vid1"_s) >= r.end(u"key1"_s) && r.start(u"vid1"_s) < r.end(u"key3"_s) + 31);
}

void TestStudioGraph::lanes() {
    QList<N> g{n(u"r1"_s, 10), n(u"r2"_s, 10), n(u"o1"_s, 10), n(u"o2"_s, 10), n(u"r3"_s, 10, {u"o1"_s})};
    g[0].lane = g[1].lane = g[4].lane = u"render"_s;
    DriveOptions o;
    o.steps = 5;
    const Drive r = drive(g, o);
    const QStringList rs{u"r1"_s, u"r2"_s, u"r3"_s};
    for (const QString& a : rs) {
        for (const QString& b : rs) {
            if (a == b) continue;
            QVERIFY2(r.end(a) <= r.start(b) || r.end(b) <= r.start(a), qPrintable(r.logText()));
        }
    }
    QVERIFY(r.start(u"o1"_s) == 0 && r.start(u"o2"_s) == 0);
    QVERIFY(r.out && r.out->ok);
}

void TestStudioGraph::failureIsolation() {
    QList<N> g{n(u"s1"_s, 10), n(u"s2"_s, 10), n(u"s3"_s, 10), n(u"needs2"_s, 5, {u"s2"_s}), n(u"after2"_s, 5, {u"s2"_s}),
               n(u"after13"_s, 5, {u"s1"_s, u"s3"_s})};
    g[1].fail = true;
    g[3].requires = {u"s2"_s};
    DriveOptions o;
    o.steps = 3;
    const Drive r = drive(g, o);
    QVERIFY2(r.end(u"s1"_s) == 10 && r.end(u"s3"_s) == 10 && r.has(u"s2"_s, u"fail"_s), qPrintable(r.logText()));
    QVERIFY(r.has(u"needs2"_s, u"skip"_s) && !r.has(u"needs2"_s, u"start"_s));
    QVERIFY(r.has(u"after2"_s, u"end"_s) && r.has(u"after13"_s, u"end"_s));
    QVERIFY(r.out && r.out->ok && r.finished.size() == 6);
}

void TestStudioGraph::runThrows() {
    // run() throwing (a bug in a step) is recorded through failed() and the others go on
    FakeClock clock;
    QObject ctx;
    QStringList seen;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        if (x.id == u"boom") throw std::runtime_error("step bug");
        clock.callAfter(5, &ctx, [&, id = x.id] {
            seen << id;
            g->settle(id);
        });
    };
    h.failed = [&](const GraphNode& x, const QString& e) { seen << u"failed:%1:%2"_s.arg(x.id, e); };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 3;
    g = std::make_unique<Graph>(std::move(h), lim);
    QVERIFY(g->add({toNode(n(u"ok1"_s, 5)), toNode(n(u"boom"_s, 5)), toNode(n(u"ok2"_s, 5)),
                    toNode(n(u"last"_s, 5, {u"boom"_s, u"ok1"_s}))}));
    g->start();
    clock.advance(100);
    QVERIFY2(out && out->ok, qPrintable(seen.join(u' ')));
    QVERIFY2(seen.contains(u"failed:boom:step bug"_s) && seen.contains(u"ok1"_s) && seen.contains(u"ok2"_s)
                 && seen.contains(u"last"_s),
             qPrintable(seen.join(u' ')));
    QCOMPARE(g->nodeState(u"boom"_s), std::optional<NodeState>(NodeState::Failed));
    QCOMPARE(metrics().failed, qint64(1));
}

void TestStudioGraph::stop() {
    QList<N> g;
    for (int i = 0; i < 10; ++i) g << n(u"n%1"_s.arg(i), 20);
    DriveOptions o;
    o.steps = 3;
    o.abortAt = 25;
    const Drive r = drive(g, o);
    int late = 0;
    for (const Ev& e : r.log) late += e.ev == u"start" && e.t > 25;
    QVERIFY2(r.starts() == 6 && late == 0, qPrintable(r.logText()));
    QVERIFY(r.out && !r.out->ok && r.out->stopped && r.out->error == u"AbortError");
    QVERIFY(r.now <= 45);
    QCOMPARE(metrics().runsActive, 0);
    QCOMPARE(metrics().stepsRunning, 0);
    // the nodes never started stay pending (a restart may resume them)
    QCOMPARE(r.finished.size(), 6);
}

void TestStudioGraph::stopBeforeStart() {
    AbortController pre;
    pre.abort();
    int started = 0;
    Hooks h;
    h.run = [&](const GraphNode&) { ++started; };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 3;
    Graph g(std::move(h), lim, pre.signal());
    QVERIFY(g.add({toNode(n(u"a"_s, 1)), toNode(n(u"b"_s, 1))}));
    g.start();
    QVERIFY(out && out->stopped && out->error == u"aborted");
    QCOMPARE(started, 0);
    QVERIFY(g.isFinished());
    QCOMPARE(metrics().runsActive, 0);
}

void TestStudioGraph::cycleStalls() {
    DriveOptions o;
    o.steps = 2;
    const Drive r = drive({n(u"x"_s, 5, {u"y"_s}), n(u"y"_s, 5, {u"x"_s})}, o);
    QVERIFY(r.out && !r.out->ok && r.out->error.contains(u"stalled"));
    QCOMPARE(metrics().runsActive, 0);
    // a self-dependency stalls too (never silently satisfied)
    const Drive self = drive({n(u"s"_s, 5, {u"s"_s})}, o);
    QVERIFY(self.out && !self.out->ok && self.out->error.contains(u"stalled"));
}

namespace {
QList<N> randomGraph(quint32 seed) {
    QRandomGenerator q(seed);
    QList<N> out;
    const QStringList kinds{u"image"_s, u"tts"_s, u"text"_s};
    for (int i = 0; i < 24; ++i) {
        N x = n(u"k%1"_s.arg(i), 1 + int(q.bounded(20)), {}, kinds[i % 3]);
        if (i > 4 && q.bounded(2) == 0) x.deps = {u"k%1"_s.arg(q.bounded(i))};
        out << x;
    }
    return out;
}
} // namespace

void TestStudioGraph::deterministic() {
    DriveOptions o;
    o.steps = 4;
    o.perKind = {{u"image"_s, 2}};
    const Drive a = drive(randomGraph(3), o), b = drive(randomGraph(3), o);
    QCOMPARE(a.logText(), b.logText());
    QVERIFY(a.out && a.out->ok);
    DriveOptions one;
    one.steps = 1;
    const Drive s = drive(randomGraph(3), one);
    const QStringList order = s.startOrder();
    QCOMPARE(order.size(), 24);
    QCOMPARE(QSet<QString>(order.cbegin(), order.cend()).size(), 24);
    // with one step the start order IS the list order for ready nodes: at every start, no node listed earlier was
    // ready (every dependency ended by then) and still waiting
    const QList<N> g = randomGraph(3);
    QHash<QString, int> listPos;
    for (int i = 0; i < g.size(); ++i) listPos.insert(g[i].id, i);
    for (const Ev& e : s.log) {
        if (e.ev != u"start") continue;
        for (int i = 0; i < listPos.value(e.id); ++i) {
            const N& x = g[i];
            if (s.start(x.id) < e.t) continue;   // started before
            bool ready = true;
            for (const QString& d : x.deps) ready = ready && s.end(d) >= 0 && s.end(d) <= e.t;
            QVERIFY2(!ready, qPrintable(u"%1 started at %2 while %3 was ready: %4"_s.arg(e.id).arg(e.t).arg(x.id, s.logText())));
        }
    }
}

void TestStudioGraph::budgetCeilingWithReserve() {
    auto mk = [] {
        QList<N> out;
        for (int i = 0; i < 10; ++i) {
            N x = n(u"opt%1"_s.arg(i), 10);
            x.est = 300000;                // $0.30
            x.optional = true;
            out << x;
        }
        return out;
    };
    const qint64 ceiling = 1150000;        // $1.15
    DriveOptions o;
    o.admit = [&](const N& x, qint64 spent, qint64 inflight) -> std::optional<QString> {
        if (x.optional && x.est > 0 && spent + inflight + x.est > ceiling) return u"budget ceiling would be exceeded"_s;
        return std::nullopt;
    };
    o.steps = 10;
    const Drive par = drive(mk(), o);
    o.steps = 1;
    const Drive ser = drive(mk(), o);
    QVERIFY2(par.count(u"end"_s) == 3 && par.count(u"skip"_s) == 7, qPrintable(par.logText()));
    QVERIFY(par.spent <= ceiling && ser.spent <= ceiling);
    QVERIFY(ser.count(u"end"_s) == 3 && ser.spent == par.spent);
    QStringList parEnded;
    for (const Ev& e : par.log) if (e.ev == u"end") parEnded << e.id;
    parEnded.sort();
    QCOMPARE(parEnded.join(u','), u"opt0,opt1,opt2"_s);
    QStringList serEnded;
    for (const Ev& e : ser.log) if (e.ev == u"end") serEnded << e.id;
    QCOMPARE(serEnded.join(u','), u"opt0,opt1,opt2"_s);
    // (control) without reserve() the second node does not see the first one's estimate: all 10 start and spend 3.0
    DriveOptions naive;
    naive.steps = 10;
    naive.useReserve = false;
    naive.admit = [&](const N& x, qint64 spent, qint64) -> std::optional<QString> {
        if (spent + x.est > ceiling) return u"over"_s;
        return std::nullopt;
    };
    const Drive ctl = drive(mk(), naive);
    QVERIFY2(ctl.starts() == 10 && ctl.spent == 3000000, qPrintable(ctl.logText()));
}

void TestStudioGraph::fuzz() {
    int bad = 0;
    QStringList firstBad;
    const QStringList kinds{u"image"_s, u"tts"_s, u"video"_s, u"text"_s, u"json"_s};
    for (quint32 seed = 1; seed <= 150; ++seed) {
        QRandomGenerator q(seed);
        const int size = 5 + int(q.bounded(40));
        QList<N> nodes;
        for (int i = 0; i < size; ++i) {
            N x = n(u"z%1"_s.arg(i), 1 + int(q.bounded(30)));
            const int nd = int(q.bounded(3));
            for (int k = 0; k < nd; ++k) {
                if (i > 0 && int(q.bounded(10)) < 6) x.deps << u"z%1"_s.arg(q.bounded(i));
            }
            x.kind = kinds[int(q.bounded(kinds.size()))];
            x.fail = int(q.bounded(100)) < 12;
            if (q.bounded(10) == 0) x.lane = u"L"_s;
            nodes << x;
        }
        DriveOptions o;
        o.steps = 1 + int(q.bounded(8));
        for (const QString& k : kinds) {
            if (q.bounded(2) == 0) o.perKind.insert(k, 1 + int(q.bounded(4)));
        }
        if (q.bounded(5) == 0) o.abortAt = qint64(q.bounded(60));
        const Drive r = drive(nodes, o);
        const QStringList ids = r.startOrder();
        bool ok = QSet<QString>(ids.cbegin(), ids.cend()).size() == ids.size();   // started at most once
        ok = ok && r.peak <= o.steps;
        for (auto it = r.peakKind.cbegin(); it != r.peakKind.cend(); ++it) {
            if (o.perKind.contains(it.key())) ok = ok && it.value() <= o.perKind.value(it.key());
        }
        for (const N& x : nodes) {   // dependency order
            const qint64 s = r.start(x.id);
            if (s < 0) continue;
            for (const QString& d : x.deps) {
                const qint64 e = r.end(d);
                ok = ok && e >= 0 && e <= s;
            }
        }
        if (o.abortAt < 0) ok = ok && r.out && r.out->ok && r.finished.size() == nodes.size();
        else {   // nothing started after Stop (the graph may have finished on its own before it)
            int late = 0;
            for (const Ev& e : r.log) late += e.ev == u"start" && e.t > o.abortAt;
            ok = ok && late == 0 && r.out.has_value();
        }
        QStringList laned;
        for (const N& x : nodes) if (!x.lane.isEmpty() && r.start(x.id) >= 0) laned << x.id;
        for (const QString& a : laned) {
            for (const QString& b : laned) {
                if (a != b) ok = ok && (r.end(a) <= r.start(b) || r.end(b) <= r.start(a));
            }
        }
        if (!ok) {
            ++bad;
            if (firstBad.size() < 4) firstBad << u"seed %1 steps %2 abortAt %3: %4"_s.arg(seed).arg(o.steps).arg(o.abortAt).arg(r.logText());
        }
    }
    QVERIFY2(bad == 0, qPrintable(firstBad.join(u'\n')));
    QCOMPARE(metrics().runsActive, 0);
    QCOMPARE(metrics().stepsRunning, 0);
}

void TestStudioGraph::defaultsAndMetrics() {
    const GraphLimits was = defaults();
    GraphLimits d;
    d.steps = 2;
    d.perKind = {{u"text"_s, 1}};
    setDefaults(d);
    FakeClock clock;
    QObject ctx;
    int live = 0, peak = 0;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        ++live;
        peak = std::max(peak, live);
        clock.callAfter(5, &ctx, [&, id = x.id] {
            --live;
            g->settle(id);
        });
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    g = std::make_unique<Graph>(std::move(h));   // no limits of its own: the process defaults
    QList<GraphNode> defs;
    for (int i = 0; i < 6; ++i) defs << toNode(n(u"d%1"_s.arg(i), 5));
    QVERIFY(g->add(defs));
    g->start();
    clock.advance(100);
    QVERIFY(out && out->ok);
    QCOMPARE(peak, 1);
    const Metrics m = metrics();
    QCOMPARE(m.started, qint64(6));
    QCOMPARE(m.finished, qint64(6));
    QCOMPARE(m.byKind.value(u"text"_s).started, 6);
    QCOMPARE(m.byKind.value(u"text"_s).running, 0);
    QCOMPARE(m.peakStepsRunning, 1);
    GraphLimits junk;
    junk.steps = 0;
    setDefaults(junk);
    QCOMPARE(defaults().steps, 4);
    setDefaults(was);
    resetMetrics();
    QCOMPARE(metrics().started, qint64(0));
    QCOMPARE(metrics().finished, qint64(0));
    QCOMPARE(clampSteps(-3, 7), 7);
    QCOMPARE(clampSteps(3, 7), 3);
}

// ---------------- part 2: what the port adds ----------------

void TestStudioGraph::priorityAndOrderKey() {
    QList<N> g;
    for (int i = 0; i < 6; ++i) g << n(u"p%1"_s.arg(i), 10);
    g[3].priority = 5;                     // higher first
    g[5].priority = 5;
    g[5].orderKey = u"0001"_s;             // then the order key
    g[3].orderKey = u"0002"_s;
    g[1].orderKey = u"b"_s;
    g[2].orderKey = u"a"_s;
    DriveOptions o;
    o.steps = 1;
    const Drive r = drive(g, o);
    // priority 5 first (p5 by key, then p3); then key "" (p0, p4 in list order), "a" (p2), "b" (p1)
    QCOMPARE(r.startOrder().join(u','), u"p5,p3,p0,p4,p2,p1"_s);
}

void TestStudioGraph::deferUntilNextSettle() {
    // the executor's unproven wave: paid optional nodes wait while two are in flight and none has finished yet
    QList<N> g;
    for (int i = 0; i < 6; ++i) {
        N x = n(u"paid%1"_s.arg(i), 10);
        x.optional = true;
        x.est = 1;
        g << x;
    }
    g << n(u"free"_s, 10);
    QSet<QString> paidInFlight;
    bool proven = false;
    int deferCalls = 0;
    DriveOptions o;
    o.steps = 8;
    o.defer = [&](const N& x) {
        ++deferCalls;
        return x.optional && !proven && paidInFlight.size() >= 2;
    };
    // admit() runs just before the start (the executor books the wave in reserve(); the drive owns that hook)
    o.admit = [&](const N& x, qint64, qint64) -> std::optional<QString> {
        if (x.optional) paidInFlight.insert(x.id);
        return std::nullopt;
    };
    o.settled = [&](const N& x) { paidInFlight.remove(x.id); };
    const Drive r = drive(g, o);
    // paid0, paid1 and the free node start at 0; the deferred ones are passed over, not waited for, and go two per
    // settle: paid2 / paid3 at 10, paid4 / paid5 at 20 (the wave is never proven here)
    QVERIFY2(r.start(u"paid0"_s) == 0 && r.start(u"paid1"_s) == 0 && r.start(u"free"_s) == 0, qPrintable(r.logText()));
    QVERIFY2(r.start(u"paid2"_s) == 10 && r.start(u"paid3"_s) == 10, qPrintable(r.logText()));
    QVERIFY2(r.start(u"paid4"_s) == 20 && r.start(u"paid5"_s) == 20, qPrintable(r.logText()));
    QVERIFY(deferCalls >= 4);
    QVERIFY(r.out && r.out->ok && r.finished.size() == 7);
    QCOMPARE(r.now, 30);
    // once proven (a first paid node finished at about its estimate) the wave widens: everything left starts at 10
    proven = false;
    paidInFlight.clear();
    o.settled = [&](const N& x) {
        paidInFlight.remove(x.id);
        if (x.optional) proven = true;
    };
    const Drive w = drive(g, o);
    QVERIFY2(w.start(u"paid2"_s) == 10 && w.start(u"paid5"_s) == 10, qPrintable(w.logText()));
    QCOMPARE(w.now, 20);
}

void TestStudioGraph::expandAtRunTime() {
    // a writing node adds its scenes at run time; "assemble" waited on the writer and now waits on the scenes too
    FakeClock clock;
    QObject ctx;
    QList<Ev> log;
    std::unique_ptr<Graph> g;
    QHash<QString, int> dur{{u"write"_s, 10}, {u"assemble"_s, 5}, {u"other"_s, 3}};
    Hooks h;
    h.run = [&](const GraphNode& x) {
        log.append({clock.steadyMs(), x.id, u"start"_s});
        if (x.id == u"write") {
            QList<GraphNode> scenes;
            for (int i = 1; i <= 3; ++i) {
                GraphNode s;
                s.id = u"scene%1"_s.arg(i);
                s.kind = u"text"_s;
                s.deps = {u"write"_s};
                if (i == 3) s.deps << u"scene1"_s;   // children may depend on each other
                scenes << s;
                dur.insert(s.id, 4);
            }
            QString err;
            QVERIFY2(g->expand(u"write"_s, scenes, &err), qPrintable(err));
            QCOMPARE(g->indegree(u"assemble"_s), 4);       // write + 3 scenes
            QCOMPARE(g->indegree(u"scene3"_s), 2);
            QCOMPARE(g->size(), 6);
        }
        clock.callAfter(dur.value(x.id, 1), &ctx, [&, id = x.id] {
            log.append({clock.steadyMs(), id, u"end"_s});
            g->settle(id);
        });
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 4;
    g = std::make_unique<Graph>(std::move(h), lim);
    QVERIFY(g->add({toNode(n(u"write"_s, 10)), toNode(n(u"assemble"_s, 5, {u"write"_s})), toNode(n(u"other"_s, 3))}));
    g->start();
    clock.advance(1000);
    QVERIFY(out && out->ok);
    auto start = [&](const QString& id) { for (const Ev& e : log) if (e.id == id && e.ev == u"start") return e.t; return qint64(-1); };
    auto end = [&](const QString& id) { for (const Ev& e : log) if (e.id == id && e.ev == u"end") return e.t; return qint64(-1); };
    QCOMPARE(start(u"write"_s), 0);
    QCOMPARE(start(u"other"_s), 0);
    QCOMPARE(start(u"scene1"_s), 10);
    QCOMPARE(start(u"scene2"_s), 10);
    QCOMPARE(start(u"scene3"_s), 14);      // after scene1
    QCOMPARE(start(u"assemble"_s), 18);    // after every scene
    QCOMPARE(end(u"assemble"_s), 23);
    QCOMPARE(g->unfinished(), 0);
    QCOMPARE(g->nodeState(u"scene3"_s), std::optional<NodeState>(NodeState::Done));
}

void TestStudioGraph::expandTwiceSameParent() {
    // the §4.6 shape: a scene expands twice (its shots, then the dialogue lines that depend on the shots). The shots
    // are siblings of the lines, not the tail: they get no edge from the lines (that edge plus line -> shot was a
    // cycle, and the run ended "stalled"). "assemble" waited on the scene and waits on both batches. A grandchild
    // expansion (a shot adds an effect) holds back the line that depends on that shot, and assemble.
    FakeClock clock;
    QObject ctx;
    QList<Ev> log;
    std::unique_ptr<Graph> g;
    QHash<QString, int> dur{{u"scene"_s, 10}, {u"assemble"_s, 5}, {u"shot1"_s, 4}, {u"shot2"_s, 4}, {u"line1"_s, 4},
                            {u"line2"_s, 4}, {u"fx1"_s, 2}};
    auto child = [](const QString& id, const QStringList& deps) {
        GraphNode c;
        c.id = id;
        c.kind = u"text"_s;
        c.deps = deps;
        return c;
    };
    Hooks h;
    h.run = [&](const GraphNode& x) {
        log.append({clock.steadyMs(), x.id, u"start"_s});
        QString err;
        if (x.id == u"scene") {
            QVERIFY2(g->expand(u"scene"_s, {child(u"shot1"_s, {u"scene"_s}), child(u"shot2"_s, {u"scene"_s})}, &err), qPrintable(err));
            QCOMPARE(g->indegree(u"assemble"_s), 3);       // scene + 2 shots
            QVERIFY2(g->expand(u"scene"_s, {child(u"line1"_s, {u"scene"_s, u"shot1"_s}), child(u"line2"_s, {u"scene"_s, u"shot2"_s})}, &err),
                     qPrintable(err));
            QCOMPARE(g->indegree(u"shot1"_s), 1);          // the scene only: no edge from the lines
            QCOMPARE(g->indegree(u"shot2"_s), 1);
            QCOMPARE(g->indegree(u"line1"_s), 2);          // scene + shot1
            QCOMPARE(g->indegree(u"line2"_s), 2);
            QCOMPARE(g->indegree(u"assemble"_s), 5);       // scene + 2 shots + 2 lines
            QCOMPARE(g->node(u"shot1"_s)->parent, u"scene"_s);
            QCOMPARE(g->node(u"line2"_s)->parent, u"scene"_s);
            QVERIFY(g->node(u"assemble"_s)->parent.isEmpty());
        } else if (x.id == u"shot1") {
            QVERIFY2(g->expand(u"shot1"_s, {child(u"fx1"_s, {u"shot1"_s})}, &err), qPrintable(err));
            QCOMPARE(g->indegree(u"line1"_s), 2);          // shot1 (running) + fx1: a dependent under another parent waits
            QCOMPARE(g->indegree(u"line2"_s), 1);          // untouched
            QCOMPARE(g->indegree(u"assemble"_s), 5);       // shot1 + shot2 + line1 + line2 + fx1 (the scene is done)
            QCOMPARE(g->node(u"fx1"_s)->parent, u"shot1"_s);
        }
        clock.callAfter(dur.value(x.id, 1), &ctx, [&, id = x.id] {
            log.append({clock.steadyMs(), id, u"end"_s});
            g->settle(id);
        });
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 8;
    g = std::make_unique<Graph>(std::move(h), lim);
    QVERIFY(g->add({toNode(n(u"scene"_s, 10)), toNode(n(u"assemble"_s, 5, {u"scene"_s}))}));
    g->start();
    clock.advance(1000);
    auto text = [&] { QStringList s; for (const Ev& e : log) s << u"%1@%2:%3"_s.arg(e.id).arg(e.t).arg(e.ev); return s.join(u' '); };
    QVERIFY2(out && out->ok, qPrintable(out ? out->error : u"(no outcome): "_s + text()));
    auto start = [&](const QString& id) { for (const Ev& e : log) if (e.id == id && e.ev == u"start") return e.t; return qint64(-1); };
    auto end = [&](const QString& id) { for (const Ev& e : log) if (e.id == id && e.ev == u"end") return e.t; return qint64(-1); };
    QCOMPARE(start(u"shot1"_s), 10);
    QCOMPARE(start(u"shot2"_s), 10);
    QCOMPARE(start(u"fx1"_s), 14);         // after shot1
    QCOMPARE(start(u"line2"_s), 14);       // after shot2
    QCOMPARE(start(u"line1"_s), 16);       // after shot1 and fx1
    QCOMPARE(start(u"assemble"_s), 20);    // after everything
    QCOMPARE(end(u"assemble"_s), 25);
    QCOMPARE(g->size(), 7);
    QCOMPARE(g->unfinished(), 0);
    // a persisted row carries its parent: add() resolves it, so a rebuilt graph keeps the sibling distinction
    Hooks h2;
    h2.run = [](const GraphNode&) {};
    Graph re(std::move(h2), lim);
    GraphNode p = child(u"p"_s, {});
    GraphNode c1 = child(u"c1"_s, {u"p"_s});
    c1.parent = u"p"_s;
    GraphNode tail = child(u"tail"_s, {u"p"_s});
    GraphNode orphan = child(u"orphan"_s, {u"p"_s});
    orphan.parent = u"nope"_s;             // unknown parent id: counts as none
    QVERIFY(re.add({p, c1, tail, orphan}));
    QString err;
    QVERIFY2(re.expand(u"p"_s, {child(u"c2"_s, {u"p"_s, u"c1"_s})}, &err), qPrintable(err));
    QCOMPARE(re.indegree(u"c1"_s), 1);     // a sibling from the rows: no edge from c2
    QCOMPARE(re.indegree(u"tail"_s), 2);   // p + c2
    QCOMPARE(re.indegree(u"orphan"_s), 2); // p + c2
    QCOMPARE(re.indegree(u"c2"_s), 2);
}

void TestStudioGraph::expandRefusals() {
    FakeClock clock;
    QObject ctx;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        clock.callAfter(5, &ctx, [&, id = x.id] { g->settle(id); });
    };
    g = std::make_unique<Graph>(std::move(h));
    QString err;
    QVERIFY(g->add({toNode(n(u"a"_s, 5)), toNode(n(u"b"_s, 5, {u"a"_s}))}));
    QVERIFY(!g->add({toNode(n(u"a"_s, 5))}, &err));             // duplicate
    QVERIFY(err.contains(u"duplicate"));
    QVERIFY(!g->add({toNode(n(u""_s, 5))}, &err));              // no id
    QVERIFY(!g->add({toNode(n(u"c"_s, 5)), toNode(n(u"c"_s, 5))}, &err));   // duplicate within the list: nothing added
    QCOMPARE(g->size(), 2);
    QVERIFY(!g->expand(u"nope"_s, {toNode(n(u"c"_s, 5))}, &err));
    QVERIFY(err.contains(u"unknown parent"));
    g->start();
    clock.advance(5);                      // a done
    QCOMPARE(g->nodeState(u"a"_s), std::optional<NodeState>(NodeState::Done));
    QVERIFY(!g->expand(u"a"_s, {toNode(n(u"c"_s, 5))}, &err));   // finished parent
    QVERIFY(err.contains(u"finished"));
    // without dependentsWait the tail does not wait for the children
    QVERIFY(g->expand(u"b"_s, {toNode(n(u"c"_s, 5))}, &err, false));
    QCOMPARE(g->indegree(u"c"_s), 0);
    clock.advance(100);
    QVERIFY(g->isFinished() && g->outcome().ok);
    QVERIFY(!g->add({toNode(n(u"d"_s, 5))}, &err));             // finished graph
    QVERIFY(!g->settle(u"zzz"_s));
    QVERIFY(!g->settle(u"a"_s));                                 // settled already
}

void TestStudioGraph::stopFromInsideHook() {
    // Stop pressed from inside a hook of a round, before anything started in it and with nothing in flight: the run
    // ends with stopped(), not "scheduler stalled" (the TS re-checks aborted() at the loop head)
    AbortController ctl;
    QStringList ran;
    Hooks h;
    h.admit = [&](const GraphNode& x) -> std::optional<QString> {
        if (x.id == u"a") {
            ctl.abort();
            return u"cut"_s;
        }
        return std::nullopt;
    };
    h.run = [&](const GraphNode& x) { ran << x.id; };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 2;
    Graph g(std::move(h), lim, ctl.signal());
    QVERIFY(g.add({toNode(n(u"a"_s, 1)), toNode(n(u"b"_s, 1))}));
    g.start();
    QVERIFY2(out.has_value(), "no outcome");
    QVERIFY2(out->stopped && out->error == u"aborted", qPrintable(out->error));
    QVERIFY(ran.isEmpty());
    QCOMPARE(g.nodeState(u"a"_s), std::optional<NodeState>(NodeState::Skipped));
    QCOMPARE(g.nodeState(u"b"_s), std::optional<NodeState>(NodeState::Ready));   // never started: a restart may resume it
    QCOMPARE(metrics().runsActive, 0);
    // the same from skip() with a node in flight: the running one winds down, then stopped()
    AbortController ctl2;
    Hooks h2;
    std::unique_ptr<Graph> g2;
    FakeClock clock;
    QObject ctx;
    h2.run = [&](const GraphNode& x) { clock.callAfter(5, &ctx, [&, id = x.id] { g2->settle(id, NodeState::Cancelled); }); };
    h2.skip = [&](const GraphNode& x) -> std::optional<QString> {
        if (x.id == u"c2" && g2 && g2->running() > 0) {   // the re-check as c2 is about to start, c1 in flight
            ctl2.abort();
            return u"cut"_s;
        }
        return std::nullopt;
    };
    std::optional<Outcome> out2;
    h2.finished = [&](const Outcome& o) { out2 = o; };
    g2 = std::make_unique<Graph>(std::move(h2), lim, ctl2.signal());
    QVERIFY(g2->add({toNode(n(u"c1"_s, 5)), toNode(n(u"c2"_s, 5)), toNode(n(u"c3"_s, 5))}));
    g2->start();
    QVERIFY(!out2);                        // c1 runs; nothing ended yet
    QCOMPARE(g2->running(), 1);
    clock.advance(10);
    QVERIFY(out2 && out2->stopped);
    QCOMPARE(g2->nodeState(u"c3"_s), std::optional<NodeState>(NodeState::Ready));
    QCOMPARE(metrics().runsActive, 0);
    QCOMPARE(metrics().stepsRunning, 0);
}

void TestStudioGraph::finishedDeletesGraph() {
    // the executor cleaning up a run from finished(): a synchronous delete must not touch the graph afterwards
    Graph* g = nullptr;
    std::optional<Outcome> seen;
    int deleted = 0;
    Hooks h;
    h.run = [&](const GraphNode& x) { g->settle(x.id); };   // synchronous: the whole graph runs inside start()
    h.finished = [&](const Outcome& o) {
        seen = o;
        delete g;
        g = nullptr;
        ++deleted;
    };
    GraphLimits lim;
    lim.steps = 2;
    g = new Graph(h, lim);
    QVERIFY(g->add({toNode(n(u"a"_s, 1)), toNode(n(u"b"_s, 1)), toNode(n(u"c"_s, 1, {u"a"_s, u"b"_s}))}));
    g->start();
    QVERIFY(g == nullptr);
    QCOMPARE(deleted, 1);
    QVERIFY(seen && seen->ok);
    QCOMPARE(metrics().runsActive, 0);
    // and from a settle() made later (the common case): the graph goes away under pump()
    FakeClock clock;
    QObject ctx;
    Hooks h2;
    h2.run = [&](const GraphNode& x) { clock.callAfter(3, &ctx, [&, id = x.id] { g->settle(id); }); };
    h2.finished = [&](const Outcome& o) {
        seen = o;
        delete g;
        g = nullptr;
        ++deleted;
    };
    g = new Graph(h2, lim);
    QVERIFY(g->add({toNode(n(u"x"_s, 3)), toNode(n(u"y"_s, 3, {u"x"_s}))}));
    g->start();
    clock.advance(10);
    QVERIFY(g == nullptr);
    QCOMPARE(deleted, 2);
    QVERIFY(seen && seen->ok);
    QCOMPARE(metrics().runsActive, 0);
    QCOMPARE(metrics().stepsRunning, 0);
}

void TestStudioGraph::rebuildFromPersistedState() {
    // the rows of a run the process died in: done / failed / skipped never run again, the polling node is offered
    // to run() with its persisted state visible, and the ready queue holds exactly the nodes whose deps finished
    QList<N> rows;
    rows << n(u"w"_s, 1);          rows.last().state = NodeState::Done;
    rows << n(u"k1"_s, 1, {u"w"_s}); rows.last().state = NodeState::Done;
    rows << n(u"k2"_s, 1, {u"w"_s}); rows.last().state = NodeState::Failed;
    rows << n(u"k3"_s, 1, {u"w"_s}); rows.last().state = NodeState::Skipped;
    rows << n(u"v1"_s, 1, {u"k1"_s}); rows.last().state = NodeState::Polling;     // in flight when it died
    rows << n(u"v2"_s, 1, {u"k2"_s}); rows.last().state = NodeState::Ready;       // was ready
    rows << n(u"v3"_s, 1, {u"k3"_s}); rows.last().state = NodeState::Running;     // in flight too
    rows << n(u"mix"_s, 1, {u"v1"_s, u"v2"_s, u"v3"_s});                           // pending
    rows << n(u"late"_s, 1, {u"w"_s, u"gone"_s});                                   // dep not in the graph
    rows[5].priority = 1;
    QStringList ran;
    QHash<QString, NodeState> seenState;
    QList<QPair<QString, NodeState>> transitions;
    FakeClock clock;
    QObject ctx;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        ran << x.id;
        seenState.insert(x.id, x.state);
        clock.callAfter(1, &ctx, [&, id = x.id] { g->settle(id); });
    };
    h.stateChanged = [&](const GraphNode& x, NodeState s) { transitions.append({x.id, s}); };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 1;
    g = std::make_unique<Graph>(std::move(h), lim);
    QList<GraphNode> defs;
    for (const N& x : rows) defs << toNode(x);
    QVERIFY(g->add(defs));
    QCOMPARE(g->unfinished(), 5);
    QCOMPARE(g->indegree(u"v1"_s), 0);
    QCOMPARE(g->indegree(u"mix"_s), 3);
    QCOMPARE(g->indegree(u"late"_s), 0);
    QVERIFY(g->readyIds().isEmpty());      // ready only from start()
    QVERIFY(transitions.isEmpty());        // terminal rows fed in are not reported
    g->start();
    // one step: v2 (priority 1) starts first; the rest wait in the queue in list order
    QCOMPARE(ran, QStringList{u"v2"_s});
    QCOMPARE(g->readyIds(), (QStringList{u"v1"_s, u"v3"_s, u"late"_s}));
    clock.advance(100);
    QVERIFY(out && out->ok);
    // v3 ends at 3 and makes mix ready; mix (listed before late) goes first
    QCOMPARE(ran, (QStringList{u"v2"_s, u"v1"_s, u"v3"_s, u"mix"_s, u"late"_s}));
    QCOMPARE(seenState.value(u"v1"_s), NodeState::Polling);
    QCOMPARE(seenState.value(u"v3"_s), NodeState::Running);
    QCOMPARE(seenState.value(u"v2"_s), NodeState::Ready);
    QCOMPARE(seenState.value(u"mix"_s), NodeState::Pending);
    for (const N& x : rows) {
        if (isTerminal(x.state)) QVERIFY(!ran.contains(x.id));
    }
    QCOMPARE(g->nodeState(u"k2"_s), std::optional<NodeState>(NodeState::Failed));
}

void TestStudioGraph::afterRoundEndsRun() {
    FakeClock clock;
    QObject ctx;
    std::unique_ptr<Graph> g;
    QStringList started, ended;
    int rounds = 0;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        started << x.id;
        clock.callAfter(10, &ctx, [&, id = x.id] {
            ended << id;
            g->settle(id, id.startsWith(u"draft") ? NodeState::Failed : NodeState::Done);
        });
    };
    h.afterRound = [&]() -> QString {
        ++rounds;
        if (ended.contains(u"draft1"_s) && ended.contains(u"draft2"_s)) return u"every draft failed"_s;
        return {};
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 2;
    g = std::make_unique<Graph>(std::move(h), lim);
    QVERIFY(g->add({toNode(n(u"draft1"_s, 10)), toNode(n(u"draft2"_s, 10)), toNode(n(u"slow"_s, 10)),
                    toNode(n(u"revise"_s, 10, {u"draft1"_s, u"draft2"_s}))}));
    g->start();
    QCOMPARE(started.size(), 2);
    clock.advance(10);                     // both drafts failed: afterRound ends the run; slow may have started
    clock.advance(100);
    QVERIFY(out && !out->ok && !out->stopped);
    QCOMPARE(out->error, u"every draft failed"_s);
    QVERIFY(!started.contains(u"revise"_s));
    QVERIFY(rounds >= 1);
    QCOMPARE(g->running(), 0);             // whatever ran wound down before the end
    QCOMPARE(metrics().runsActive, 0);
}

void TestStudioGraph::settleFromAnotherThread() {
    std::unique_ptr<Graph> g;
    Hooks h;
    QStringList order;
    h.run = [&](const GraphNode& x) {
        order << x.id;
        auto* t = QThread::create([&, id = x.id] { g->settleLater(id); });
        QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
        t->start();
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    GraphLimits lim;
    lim.steps = 2;
    g = std::make_unique<Graph>(std::move(h), lim);
    QVERIFY(g->add({toNode(n(u"a"_s, 1)), toNode(n(u"b"_s, 1)), toNode(n(u"c"_s, 1, {u"a"_s, u"b"_s}))}));
    g->start();
    QTRY_VERIFY_WITH_TIMEOUT(out.has_value(), 5000);
    QVERIFY(out->ok);
    QCOMPARE(order, (QStringList{u"a"_s, u"b"_s, u"c"_s}));
    QTest::qWait(20);                      // let the worker threads finish
}

void TestStudioGraph::stateTransitions() {
    QList<QPair<QString, NodeState>> tr;
    std::unique_ptr<Graph> g;
    Hooks h;
    h.run = [&](const GraphNode& x) {
        if (x.id == u"video") {
            QVERIFY(g->setPolling(x.id));   // the provider accepted; the executor persisted the job id first
            QVERIFY(!g->setPolling(x.id));  // only from running
        }
    };
    h.skip = [](const GraphNode& x) -> std::optional<QString> {
        if (x.id == u"cut") return u"not wanted"_s;
        return std::nullopt;
    };
    std::optional<Outcome> out;
    h.finished = [&](const Outcome& o) { out = o; };
    h.stateChanged = [&](const GraphNode& x, NodeState s) { tr.append({x.id, s}); };
    g = std::make_unique<Graph>(std::move(h));
    QVERIFY(g->add({toNode(n(u"video"_s, 1)), toNode(n(u"cut"_s, 1)), toNode(n(u"after"_s, 1, {u"cut"_s}))}));
    g->start();
    QCOMPARE(g->nodeState(u"video"_s), std::optional<NodeState>(NodeState::Polling));
    QCOMPARE(g->inFlightIds(), (QStringList{u"video"_s, u"after"_s}));
    QVERIFY(g->settle(u"video"_s, NodeState::Unknown, u"timed out"_s));
    QVERIFY(g->settle(u"after"_s));
    QVERIFY(out && out->ok);
    // start(): the ready pass (video ready; cut ready then skipped, which makes after ready), then the round
    QList<QPair<QString, NodeState>> want{
        {u"video"_s, NodeState::Ready}, {u"cut"_s, NodeState::Ready}, {u"cut"_s, NodeState::Skipped},
        {u"after"_s, NodeState::Ready},
        {u"video"_s, NodeState::Admitted}, {u"video"_s, NodeState::Running}, {u"video"_s, NodeState::Polling},
        {u"after"_s, NodeState::Admitted}, {u"after"_s, NodeState::Running},
        {u"video"_s, NodeState::Unknown}, {u"after"_s, NodeState::Done}};
    QStringList got, exp;
    for (const auto& p : tr) got << p.first + u':' + stateName(p.second);
    for (const auto& p : want) exp << p.first + u':' + stateName(p.second);
    QCOMPARE(got.join(u' '), exp.join(u' '));
    QVERIFY(parseState(u"polling", nullptr));
    NodeState s;
    QVERIFY(parseState(u"unknown", &s) && s == NodeState::Unknown && isTerminal(s));
    QVERIFY(!parseState(u"nope", &s));
    QVERIFY(isInFlight(NodeState::Polling) && !isInFlight(NodeState::Done));
}

void TestStudioGraph::perf10k() {
    // 100 layers x 100 nodes, each on two nodes of the layer before; four kinds with caps; a lane every 50th node
    const int layers = 100, width = 100;
    QList<GraphNode> defs;
    defs.reserve(layers * width);
    const QStringList kinds{u"image"_s, u"tts"_s, u"video"_s, u"text"_s};
    for (int l = 0; l < layers; ++l) {
        for (int i = 0; i < width; ++i) {
            GraphNode g;
            g.id = u"n%1_%2"_s.arg(l).arg(i);
            g.kind = kinds[(l * width + i) % 4];
            if (l > 0) g.deps = {u"n%1_%2"_s.arg(l - 1).arg(i), u"n%1_%2"_s.arg(l - 1).arg((i + 1) % width)};
            if ((l * width + i) % 50 == 0) g.lane = u"render"_s;
            defs << g;
        }
    }
    GraphLimits lim;
    lim.steps = 48;
    lim.perKind = {{u"image"_s, 16}, {u"tts"_s, 16}, {u"video"_s, 8}};
    // 1) completions through FakeClock timers (1..3 ticks)
    {
        FakeClock clock;
        QObject ctx;
        std::unique_ptr<Graph> g;
        int ran = 0;
        Hooks h;
        h.run = [&](const GraphNode& x) {
            ++ran;
            clock.callAfter(1 + (ran % 3), &ctx, [&, id = x.id] { g->settle(id); });
        };
        h.skip = [](const GraphNode&) -> std::optional<QString> { return std::nullopt; };
        h.admit = [](const GraphNode&) -> std::optional<QString> { return std::nullopt; };
        h.reserve = [](const GraphNode&) {};
        h.settled = [](const GraphNode&) {};
        h.stateChanged = [](const GraphNode&, NodeState) {};
        std::optional<Outcome> out;
        h.finished = [&](const Outcome& o) { out = o; };
        g = std::make_unique<Graph>(std::move(h), lim);
        QElapsedTimer t;
        t.start();
        QVERIFY(g->add(defs));
        g->start();
        int guard = 0;
        while (!out && guard++ < 1000000) {
            const qint64 next = clock.nextDueSteadyMs();
            if (next < 0) break;
            clock.advance(next - clock.steadyMs());
        }
        const qint64 ms = t.elapsed();
        qInfo("10k nodes through timers: %lld ms, %lld ticks", ms, clock.steadyMs());
        QVERIFY(out && out->ok);
        QCOMPARE(ran, layers * width);
        QCOMPARE(g->unfinished(), 0);
        QVERIFY2(ms < 1000, qPrintable(u"%1 ms"_s.arg(ms)));
    }
    // 2) synchronous completions (a cached result): the whole graph runs inside start()
    {
        std::unique_ptr<Graph> g;
        int ran = 0;
        Hooks h;
        h.run = [&](const GraphNode& x) {
            ++ran;
            g->settle(x.id);
        };
        std::optional<Outcome> out;
        h.finished = [&](const Outcome& o) { out = o; };
        g = std::make_unique<Graph>(std::move(h), lim);
        QElapsedTimer t;
        t.start();
        QVERIFY(g->add(defs));
        g->start();
        const qint64 ms = t.elapsed();
        qInfo("10k nodes synchronous: %lld ms", ms);
        QVERIFY(out && out->ok);
        QCOMPARE(ran, layers * width);
        QVERIFY2(ms < 1000, qPrintable(u"%1 ms"_s.arg(ms)));
    }
    QCOMPARE(metrics().runsActive, 0);
    QCOMPARE(metrics().stepsRunning, 0);
}

QTEST_GUILESS_MAIN(TestStudioGraph)
#include "test_studio_graph.moc"
