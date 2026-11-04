#include <QtTest/QtTest>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QRandomGenerator>
#include <cmath>
#include <memory>
#include "playout/meterticker.h"

#ifndef PP_SOURCE_DIR
#  error "PP_SOURCE_DIR must name the source tree (cmake/lane-perf-app.cmake sets it)"
#endif

// perf-app PA-5 (W18): one shared C++ meter ticker instead of a 150 ms QML Timer per VuMeter / AudioStrip.
//   - one timer for every meter (N meters: 6.7 fires/s, not N x 6.7), started by the first loud feed, never restarted
//     by later feeds, stopped when the last hold reaches the floor (an idle meter costs no fire at all);
//   - the decay is the old one (150 ms, 1.5 dB, -100 dBFS floor), checked value for value against the old JS code;
//   - the real VuMeter.qml / AudioStrip.qml from the source tree use it and carry no Timer any more; the old timer next to
//     them counts what it cost (it kept firing on silence).
// No IPC, no ports, no window: QQuickItems without a QQuickWindow.

class FireCounter : public QObject {
    Q_OBJECT
public:
    int n = 0;
    Q_INVOKABLE void fire() { ++n; }
};

class TestMeterTicker : public QObject {
    Q_OBJECT
private slots:
    void decayMatchesOldQml();
    void oneTimerStopsWhenIdle();
    void deletedHoldsAndTicker();
    void nonNumbersAreFloor();
    void qmlMetersShareOneTicker();
};

static MeterHold* holdOf(MeterTicker& t, QObject* owner) { return qobject_cast<MeterHold*>(t.createHold(owner)); }
static QVariantList list(std::initializer_list<double> v) { QVariantList l; for (double d : v) l << d; return l; }
static int qmlTimersUnder(QQuickItem* item) {   // Timers are QObject children of their item; delegates hang off the item tree
    int n = 0;
    for (QObject* o : item->children()) if (QByteArray(o->metaObject()->className()).startsWith("QQmlTimer")) ++n;
    for (QQuickItem* c : item->childItems()) n += qmlTimersUnder(c);
    return n;
}
static void meterRows(QQuickItem* item, QList<QQuickItem*>* rows) {   // the bars' RowLayouts (they carry db / hd), in order
    for (QQuickItem* c : item->childItems()) {
        if (c->property("hd").isValid() && c->property("db").isValid()) rows->append(c);
        else meterRows(c, rows);
    }
}

// A random mix of feeds (1..8 channels, peaks from -110 to 0, exactly -100 sometimes) and ticks, run through the C++ hold
// and through the old QML code (onPeaksChanged + the Timer's onTriggered, verbatim) in a QJSEngine. What the bars show is
// hd = hold[i] (or -100 when missing), clamped to the -100 floor by the old timer: those must be equal at every step.
void TestMeterTicker::decayMatchesOldQml() {
    MeterTicker t;
    QObject owner;
    MeterHold* h = holdOf(t, &owner);
    QJSEngine js;
    js.evaluate(R"(
        var hold = []
        function feed(peaks) { var h = hold.slice(); for (var i = 0; i < peaks.length; ++i) { if (h[i] === undefined || peaks[i] >= h[i]) h[i] = peaks[i] } hold = h }
        function tick() { var h = hold.slice(); for (var i = 0; i < h.length; ++i) h[i] = Math.max(-100, h[i] - 1.5); hold = h }
    )");
    QRandomGenerator rng(20261026);
    int feeds = 0, ticks = 0, oldAssignments = 0, exact = 0, values = 0;
    QSignalSpy changes(h, &MeterHold::holdChanged);
    for (int step = 0; step < 3000; ++step) {
        if (rng.bounded(100) < 40) {
            const int n = 1 + rng.bounded(8);
            QVariantList peaks;
            for (int i = 0; i < n; ++i) {
                const int r = rng.bounded(20);
                peaks << (r == 0 ? -100.0 : r == 1 ? -100.0 - rng.bounded(10.0) : -rng.bounded(70.0));
            }
            h->feed(peaks);
            js.globalObject().property("feed").call({js.toScriptValue(peaks)});
            ++feeds;
        } else {
            t.tick();   // stepped by hand: same arithmetic as the timer's tick
            js.globalObject().property("tick").call();
            ++ticks;
        }
        ++oldAssignments;   // the old code reassigned the whole array on every feed and every tick
        const QJSValue old = js.globalObject().property("hold");
        const int oldLen = old.property("length").toInt();
        QCOMPARE(h->values().size(), oldLen);
        const QVariantList shown = h->hold();
        QCOMPARE(shown.size(), oldLen);
        for (int i = 0; i < oldLen; ++i) {
            const double was = std::max(-100.0, old.property(quint32(i)).toNumber());
            const double now = shown[i].toDouble();
            QVERIFY2(std::fabs(was - now) < 1e-9, qPrintable(QString("step %1 ch %2: old %3 new %4").arg(step).arg(i).arg(was).arg(now)));
            exact += (was == now); ++values;
        }
    }
    qInfo().noquote() << QString("%1 feeds, %2 ticks: %3 of %4 shown values bit-identical; hold notifications %5 (the old code reassigned %6 times)")
                             .arg(feeds).arg(ticks).arg(exact).arg(values).arg(changes.count()).arg(oldAssignments);
    QCOMPARE(exact, values);
    QVERIFY(changes.count() < oldAssignments);
}

// Real time, default options. Six meters fed at 15 Hz share one 150 ms timer; feeds do not restart it; it stops when the
// last hold is on the floor and then nothing fires until a loud feed; floor-level (silent) feeds never start it.
void TestMeterTicker::oneTimerStopsWhenIdle() {
    MeterTicker t;
    QCOMPARE(t.intervalMs(), 150);
    QCOMPARE(t.decayDb(), 1.5);
    QCOMPARE(t.floorDb(), -100.0);
    QObject owners[6];
    QList<MeterHold*> holds;
    for (auto& o : owners) holds << holdOf(t, &o);
    QCOMPARE(t.counters().holdsLive, 6);
    for (MeterHold* h : holds) QVERIFY(h->findChildren<QTimer*>().isEmpty());   // the holds carry no timer of their own

    QTest::qWait(400);
    QCOMPARE(t.counters().ticks, quint64(0));   // nothing to decay: no fire at all
    QVERIFY(!t.running());

    QElapsedTimer el;
    el.start();
    const quint64 t0 = t.counters().ticks;
    int rounds = 0;
    while (el.elapsed() < 1200) {
        for (MeterHold* h : holds) h->feed(list({-91.0, -93.0}));
        ++rounds;
        QTest::qWait(67);
    }
    const qint64 ms = el.elapsed();
    const quint64 during = t.counters().ticks - t0;
    qInfo().noquote() << QString("6 meters fed %1 times each over %2 ms: %3 ticks of the one timer (%4/s); 6 QML timers would have fired ~%5")
                             .arg(rounds).arg(ms).arg(during).arg(during * 1000.0 / ms, 0, 'f', 1).arg(6 * ms / 150);
    QVERIFY2(during >= 5 && during <= 10, qPrintable(QString::number(during)));   // ~8 in 1.2 s: one timer, not six
    QCOMPARE(t.counters().starts, quint64(1));                                     // the 15 Hz feeds never restarted it
    QVERIFY(t.running());
    QCOMPARE(t.decayingCount(), 6);
    for (MeterHold* h : holds) { QVERIFY(h->values()[0] <= -91.0 && h->values()[0] >= -94.0); QVERIFY(h->decaying()); }

    // Silence: -91 reaches the floor in 6 ticks (0.9 s), then the timer stops.
    el.restart();
    QVERIFY(QTest::qWaitFor([&]() { return !t.running(); }, 3000));
    qInfo().noquote() << QString("stopped %1 ms after the last feed").arg(el.elapsed());
    QCOMPARE(t.counters().stops, quint64(1));
    for (MeterHold* h : holds) { QCOMPARE(h->values(), QVector<double>({-100.0, -100.0})); QVERIFY(!h->decaying()); }
    const quint64 idleTicks = t.counters().ticks, idleChanges = t.counters().holdChanges;
    QTest::qWait(600);
    QCOMPARE(t.counters().ticks, idleTicks);   // idle: no fire

    // Floor-level feeds (a silent channel at 15 Hz) neither start the timer nor notify.
    for (int i = 0; i < 6; ++i) { for (MeterHold* h : holds) h->feed(list({-100.0, -120.0})); QTest::qWait(50); }
    QVERIFY(!t.running());
    QCOMPARE(t.counters().ticks, idleTicks);
    QCOMPARE(t.counters().holdChanges, idleChanges);

    // One loud feed wakes it; one tick later (-98.5 -> -100) it is idle again.
    holds[2]->feed(list({-98.5}));
    QVERIFY(t.running());
    QCOMPARE(t.counters().starts, quint64(2));
    QVERIFY(QTest::qWaitFor([&]() { return !t.running(); }, 1000));
    QCOMPARE(t.counters().ticks, idleTicks + 1);
    QCOMPARE(t.counters().stops, quint64(2));
}

// A hold deleted with its meter while decaying is dropped (and the timer stops when it was the last); a hold that
// outlives the ticker keeps working (no decay, no crash).
void TestMeterTicker::deletedHoldsAndTicker() {
    MeterTicker t;
    auto owner = std::make_unique<QObject>();
    QObject keep;
    MeterHold* a = holdOf(t, owner.get());
    MeterHold* b = holdOf(t, &keep);
    a->feed(list({-40.0}));
    QVERIFY(t.running());
    QCOMPARE(t.counters().holdsLive, 2);
    owner.reset();   // the meter item goes away with its hold
    QCOMPARE(t.counters().holdsLive, 1);
    t.tick();        // prunes it; nothing else decays, so the timer stops
    QVERIFY(!t.running());
    QCOMPARE(t.decayingCount(), 0);

    auto t2 = std::make_unique<MeterTicker>();
    QObject keep2;
    MeterHold* c = holdOf(*t2, &keep2);
    c->feed(list({-10.0}));
    QVERIFY(t2->running());
    t2.reset();      // ticker first (application shutdown order is not guaranteed)
    c->feed(list({-5.0, -7.0}));
    QCOMPARE(c->values(), QVector<double>({-5.0, -7.0}));
    c->clear();
    QCOMPARE(c->values(), QVector<double>({-100.0, -100.0}));
    QVERIFY(b->values().isEmpty());

    // Without an owner the hold belongs to the JS engine (collectable), with one it never does.
    QObject owned;
    QCOMPARE(QJSEngine::objectOwnership(t.createHold(&owned)), QJSEngine::CppOwnership);
    QObject* loose = t.createHold(nullptr);
    QCOMPARE(QJSEngine::objectOwnership(loose), QJSEngine::JavaScriptOwnership);
    delete loose;
}

// null / undefined / NaN / text count as the floor, never as 0 dBFS (the old JS read null as 0 and held a full-scale peak).
void TestMeterTicker::nonNumbersAreFloor() {
    MeterTicker t;
    QObject owner;
    MeterHold* h = holdOf(t, &owner);
    h->feed({QVariant(), QVariant::fromValue(nullptr), qQNaN(), QString("loud"), -20.0, std::numeric_limits<double>::infinity()});
    QCOMPARE(h->values(), QVector<double>({-100.0, -100.0, -100.0, -100.0, -20.0, -100.0}));

    QJSEngine js;
    MeterHold* j = holdOf(t, &owner);
    js.globalObject().setProperty("h", js.newQObject(j));
    const QJSValue r = js.evaluate("h.feed([null, undefined, -10, -30.5]); h.hold");
    QVERIFY2(!r.isError(), qPrintable(r.toString()));
    QCOMPARE(j->values(), QVector<double>({-100.0, -100.0, -10.0, -30.5}));
    QCOMPARE(r.property("length").toInt(), 4);
    QCOMPARE(r.property(2).toNumber(), -10.0);
    QVERIFY(r.property(9).isUndefined());   // out of range reads undefined, as the bars expect (they fall back to -100)
}

// The real VuMeter.qml and AudioStrip.qml (3 of each) with meterTicker as a context property, as main.cpp sets it up.
void TestMeterTicker::qmlMetersShareOneTicker() {
    QQmlEngine eng;
    MeterTicker t;
    FireCounter oldFires;
    eng.rootContext()->setContextProperty("meterTicker", &t);
    eng.rootContext()->setContextProperty("oldFires", &oldFires);
    QQmlComponent comp(&eng);
    comp.setData(R"(
        import QtQuick
        import "ui"
        Item {
            id: root
            property var m: ({})
            property int oldCount: 0
            Repeater { model: 3; VuMeter { width: 320; channels: 2; meters: root.m } }
            Repeater { model: 3; AudioStrip { width: 320; channels: 2; meters: root.m } }
            // The pre-PA-5 timer, verbatim but counted: one per meter, always running (the baseline being measured).
            Repeater {
                model: root.oldCount
                Item {
                    id: o
                    property var hold: []
                    Timer { interval: 150; running: true; repeat: true; onTriggered: { oldFires.fire(); var h = o.hold.slice(); for (var i = 0; i < h.length; ++i) h[i] = Math.max(-100, h[i] - 1.5); o.hold = h } }
                }
            }
        }
    )", QUrl::fromLocalFile(QStringLiteral(PP_SOURCE_DIR "/Play_Play_Native/qml/meterticker_test.qml")));
    std::unique_ptr<QObject> obj(comp.create());
    QVERIFY2(obj, qPrintable(comp.errorString()));
    auto* root = qobject_cast<QQuickItem*>(obj.get());
    QVERIFY(root);

    // Six meters, six holds, one ticker, and not one QML Timer among them.
    QCOMPARE(t.counters().holdsLive, 6);
    QCOMPARE(qmlTimersUnder(root), 0);
    QList<QQuickItem*> meters;
    for (QQuickItem* c : root->childItems()) if (c->property("peakHold").isValid()) meters << c;
    QCOMPARE(meters.size(), 6);
    for (QQuickItem* m : meters) QVERIFY(qobject_cast<MeterHold*>(m->property("peakHold").value<QObject*>()));
    QVERIFY(!t.running());

    // Meters at 15 Hz (a new object each time, as activeMeters is) for 1.5 s.
    QElapsedTimer el;
    el.start();
    const quint64 t0 = t.counters().ticks;
    int n = 0;
    while (el.elapsed() < 1500) {
        root->setProperty("m", QVariantMap{{"channelPeaksDbfs", list({n % 2 ? -79.0 : -79.5, -85.0})}, {"momentaryLufs", -23.0}});
        ++n;
        QTest::qWait(67);
    }
    const qint64 ms = el.elapsed();
    const quint64 newTicks = t.counters().ticks - t0;
    const double newRate = newTicks * 1000.0 / ms;
    QVERIFY2(newTicks >= 7 && newTicks <= 13, qPrintable(QString::number(newTicks)));   // ~10 in 1.5 s for all six meters
    QCOMPARE(t.counters().starts, quint64(1));
    for (QQuickItem* m : meters) {
        auto* h = qobject_cast<MeterHold*>(m->property("peakHold").value<QObject*>());
        QCOMPARE(h->values().size(), 2);
        QVERIFY2(h->values()[0] <= -79.0 && h->values()[0] >= -82.0, qPrintable(QString::number(h->values()[0])));
        QList<QQuickItem*> rows;
        meterRows(m, &rows);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0]->property("hd").toDouble(), h->values()[0]);   // the bar's hold marker follows the C++ hold
        QCOMPARE(rows[1]->property("hd").toDouble(), h->values()[1]);
        QCOMPARE(rows[0]->property("db").toDouble(), n % 2 ? -79.5 : -79.0);   // the last meters fed (n was bumped after)
    }

    // Silence (no more meters): -79 reaches the floor in 14 ticks (2.1 s), then the shared timer stops.
    QVERIFY(QTest::qWaitFor([&]() { return !t.running(); }, 4000));
    for (QQuickItem* m : meters) {
        QList<QQuickItem*> rows;
        meterRows(m, &rows);
        QCOMPARE(rows[0]->property("hd").toDouble(), -100.0);
    }
    const quint64 idle = t.counters().ticks;

    // The old timers next to the same six meters, over the same 1.5 s of silence.
    root->setProperty("oldCount", 6);
    QCOMPARE(qmlTimersUnder(root), 6);   // (and the counting itself works)
    el.restart();
    QTest::qWait(1500);
    const qint64 oldMs = el.elapsed();
    const double oldRate = oldFires.n * 1000.0 / oldMs;
    QCOMPARE(t.counters().ticks, idle);   // the shared ticker: nothing while silent
    qInfo().noquote() << QString("6 meters: shared ticker %1 fires/s while fed, 0 while silent; the old per-meter timers %2 fires/s (%3 in %4 ms), also while silent")
                             .arg(newRate, 0, 'f', 1).arg(oldRate, 0, 'f', 1).arg(oldFires.n).arg(oldMs);
    QVERIFY2(oldFires.n >= 40, qPrintable(QString::number(oldFires.n)));   // 6 x ~10
    QVERIFY(oldRate > 4.5 * newRate);
    root->setProperty("oldCount", 0);

    // The meters go away with their holds; the ticker is left with nothing.
    obj.reset();
    QCOMPARE(t.counters().holdsLive, 0);
    QVERIFY(!t.running());
}

QTEST_MAIN(TestMeterTicker)
#include "test_meterticker.moc"
