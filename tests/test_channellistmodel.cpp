#include <QtTest/QtTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonDocument>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>
#include "playout/channellistmodel.h"
#include "playout/supervisorclient.h"
#include "playout/ui_ipc/ipctransport.h"

#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#endif

// perf-app PA-3 (W10): finer change signals and a channel list model whose rows are updated in place.
//   - ChannelListModel: an identical snapshot gives no reset and no signal; one changed field gives one dataChanged;
//     added / removed / reordered channels are inserts / removes / moves; QML delegates survive snapshots.
//   - SupervisorClient: activeMeters / metersChanged at most 15 Hz (they followed every preview frame of every source),
//     the active* properties notify only for the active channel, an unchanged snapshot tells QML nothing.
// A QML binding counter (EvalCounter) measures binding evaluations with the new NOTIFY next to one that emulates the old
// one (previewSerial = previewChanged). Pipe names are unique to this process ("pptm-..."); no live IPC name or port.
class EvalCounter : public QObject {
    Q_OBJECT
public:
    QHash<QString, int> n;
    Q_INVOKABLE void hit(const QString& tag) { ++n[tag]; }
};

class TestChannelListModel : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void identicalSnapshotNoSignals();
    void oneFieldOneDataChanged();
    void insertRemoveMoveInPlace();
    void upsertAndDuplicateIds();
    void qmlDelegatesUpdatedInPlace();
    void metersChangedAtMost15Hz();
    void activeSignalsFollowOnlyActiveChannel();
    void disconnectDropsTelemetryOutLoud();
};

namespace {
QJsonObject chan(const QString& id, bool connected = true, double heartbeat = 0, bool armed = true, const QString& name = {}) {
    return QJsonObject{
        {"profile", QJsonObject{{"id", id}, {"name", name.isEmpty() ? id.toUpper() : name}, {"timezone", "America/New_York"}, {"audioChannels", 2}}},
        {"process", QJsonObject{{"connected", connected}, {"running", connected}, {"pid", 100}, {"restarts", 0}, {"lastHeartbeatMs", heartbeat}}},
        {"executor", QJsonObject{{"armed", armed}}},
        {"engine", QJsonObject{{"state", connected ? "ON_AIR" : "OFFLINE"}}},
        {"engineState", connected ? "ON_AIR" : "OFFLINE"},
        {"hasPreview", true}, {"exists", true}};
}
QJsonArray arr(std::initializer_list<QJsonObject> l) { QJsonArray a; for (const QJsonObject& o : l) a.append(o); return a; }
QJsonObject with(QJsonObject o, const QString& part, const QString& key, const QJsonValue& v) {
    QJsonObject p = o.value(part).toObject(); p[key] = v; o[part] = p; return o;
}

struct ModelSpy {
    QSignalSpy reset, inserted, removed, moved, layout, changed;
    explicit ModelSpy(QAbstractItemModel* m)
        : reset(m, &QAbstractItemModel::modelReset), inserted(m, &QAbstractItemModel::rowsInserted), removed(m, &QAbstractItemModel::rowsRemoved),
          moved(m, &QAbstractItemModel::rowsMoved), layout(m, &QAbstractItemModel::layoutChanged), changed(m, &QAbstractItemModel::dataChanged) {}
    void clear() { reset.clear(); inserted.clear(); removed.clear(); moved.clear(); layout.clear(); changed.clear(); }
    int structural() const { return int(reset.count() + inserted.count() + removed.count() + moved.count() + layout.count()); }
};

QString uniqueName(const char* tag) {
    static int n = 0;
    return QString("pptm-%1-%2-%3").arg(tag).arg(QCoreApplication::applicationPid()).arg(++n);
}

SupervisorClient::Options opts(const QString& name) {
    SupervisorClient::Options o;
    o.ipcName = name; o.launchIfMissing = false; o.restPort = 1; o.healthPort = 0;
    o.executable = QDir::temp().filePath("pptm-no-such-dir/PlayPlaySupervisor.exe");
    return o;
}

// A Supervisor stand-in: an IpcTransport server on its own thread (as in test_supervisorclient).
struct FakeSupervisor {
    QThread thread;
    QObject* ctx = nullptr;
    IpcTransport* server = nullptr;
    std::atomic<bool> streamDone{false};
    bool start(const QString& name) {
        thread.setObjectName("fake-supervisor");
        thread.start();
        ctx = new QObject; ctx->moveToThread(&thread);
        bool ok = false;
        QMetaObject::invokeMethod(ctx, [&]() { server = new IpcTransport(IpcTransport::ServerRole, ctx); ok = server->startServer(name); }, Qt::BlockingQueuedConnection);
        return ok;
    }
    void broadcast(const IpcProtocol::Message& m) { QMetaObject::invokeMethod(ctx, [this, m]() { server->broadcast(m); }, Qt::QueuedConnection); }
    void stopServer() { QMetaObject::invokeMethod(ctx, [this]() { delete server; server = nullptr; }, Qt::BlockingQueuedConnection); }   // the Supervisor goes away
    void stream(int ratePerSec, int durationMs, std::function<IpcProtocol::Message(int)> gen) {
        streamDone = false;
        QMetaObject::invokeMethod(ctx, [this, ratePerSec, durationMs, gen]() {
            auto* timer = new QTimer(ctx);
            timer->setTimerType(Qt::PreciseTimer); timer->setInterval(2);
            auto clock = std::make_shared<QElapsedTimer>(); clock->start();
            auto sent = std::make_shared<int>(0);
            const int total = ratePerSec * durationMs / 1000;
            QObject::connect(timer, &QTimer::timeout, ctx, [this, timer, clock, sent, total, ratePerSec, gen]() {
                const int due = int(qMin<qint64>(total, clock->elapsed() * ratePerSec / 1000 + 1));
                while (*sent < due) server->broadcast(gen((*sent)++), IpcTransport::Normal);
                if (*sent >= total) { timer->stop(); timer->deleteLater(); streamDone = true; }
            });
            timer->start();
        }, Qt::QueuedConnection);
    }
    ~FakeSupervisor() {
        if (ctx) { QMetaObject::invokeMethod(ctx, [this]() { delete server; server = nullptr; }, Qt::BlockingQueuedConnection); ctx->deleteLater(); }
        thread.quit(); thread.wait();
    }
};

QString tinyJpegBase64() {
    QImage img(64, 36, QImage::Format_RGB32);
    img.fill(qRgb(30, 60, 120));
    QByteArray jpeg; QBuffer b(&jpeg); b.open(QIODevice::WriteOnly);
    if (!img.save(&b, "JPEG", 80)) return {};
    return QString::fromLatin1(jpeg.toBase64());
}

QObject* objectAt(QObject* inst, int i) {
    QObject* o = nullptr;
    QMetaObject::invokeMethod(inst, "objectAt", Q_RETURN_ARG(QObject*, o), Q_ARG(int, i));
    return o;
}
} // namespace

void TestChannelListModel::initTestCase() {
    QCoreApplication::setOrganizationName("PlayPlayLaneTest");
    QCoreApplication::setApplicationName("test_channellistmodel");
#ifdef Q_OS_WIN
    timeBeginPeriod(1);
#endif
}

void TestChannelListModel::cleanupTestCase() {
#ifdef Q_OS_WIN
    timeEndPeriod(1);
#endif
}

// The same snapshot again (a fresh copy, equal content: the 4 s poll after the 2 s push) changes nothing at all.
void TestChannelListModel::identicalSnapshotNoSignals() {
    ChannelListModel m;
    ModelSpy s(&m);
    QSignalSpy count(&m, &ChannelListModel::countChanged);
    const QJsonArray a = arr({chan("main"), chan("ch2"), chan("ch3", false)});
    const QHash<QString, int> first = m.sync(a);
    QCOMPARE(m.count(), 3);
    QCOMPARE(m.rowCount(), 3);
    QCOMPARE(m.ids(), QStringList({"main", "ch2", "ch3"}));
    QCOMPARE(first.size(), 3);
    QCOMPARE(first.value("ch2"), int(ChannelListModel::Inserted));
    QCOMPARE(int(s.inserted.count()), 3);
    QCOMPARE(int(s.reset.count()), 0);
    QCOMPARE(int(count.count()), 1);
    QCOMPARE(m.roleNames(), (QHash<int, QByteArray>{{ChannelListModel::ChannelDataRole, "channelData"}}));
    QCOMPARE(m.data(m.index(1), ChannelListModel::ChannelDataRole).toMap().value("profile").toMap().value("id").toString(), QString("ch2"));
    QVERIFY(!m.data(m.index(1), Qt::DisplayRole).isValid());
    QCOMPARE(m.get(2).value("engineState").toString(), QString("OFFLINE"));
    QCOMPARE(m.channel("main").value("profile").toMap().value("name").toString(), QString("MAIN"));
    QCOMPARE(m.toVariantList().size(), 3);

    s.clear(); count.clear();
    for (int i = 0; i < 10; ++i) {
        const QJsonArray copy = QJsonDocument::fromJson(QJsonDocument(a).toJson()).array();
        QVERIFY(m.sync(copy).isEmpty());
    }
    qInfo().noquote() << QString("identical snapshot x10: resets %1, inserts %2, removes %3, moves %4, dataChanged %5")
                             .arg(s.reset.count()).arg(s.inserted.count()).arg(s.removed.count()).arg(s.moved.count()).arg(s.changed.count());
    QCOMPARE(s.structural(), 0);
    QCOMPARE(int(s.changed.count()), 0);
    QCOMPARE(int(count.count()), 0);
    QCOMPARE(m.stats().unchanged, quint64(10));
    QCOMPARE(m.stats().resets, quint64(0));
}

// One field of one channel: one dataChanged for exactly that row and role; the Part bits name what changed.
void TestChannelListModel::oneFieldOneDataChanged() {
    ChannelListModel m;
    const QJsonArray a = arr({chan("main"), chan("ch2"), chan("ch3")});
    m.sync(a);
    ModelSpy s(&m);
    QJsonArray b = a; b[1] = with(a[1].toObject(), "process", "connected", false);
    const QHash<QString, int> ch = m.sync(b);
    QCOMPARE(int(s.changed.count()), 1);
    const QList<QVariant> args = s.changed.first();
    QCOMPARE(args.at(0).toModelIndex().row(), 1);
    QCOMPARE(args.at(1).toModelIndex().row(), 1);
    QCOMPARE(args.at(2).value<QList<int>>(), QList<int>({ChannelListModel::ChannelDataRole}));
    QCOMPARE(s.structural(), 0);
    QCOMPARE(ch.size(), 1);
    QCOMPARE(ch.value("ch2"), int(ChannelListModel::Process));
    QCOMPARE(m.channel("ch2").value("process").toMap().value("connected").toBool(), false);
    QCOMPARE(m.toVariantList().at(1).toMap().value("process").toMap().value("connected").toBool(), false);   // the cached list follows

    // Which part: profile, executor, engine, anything else.
    s.clear();
    QJsonArray c = b;
    c[0] = with(b[0].toObject(), "profile", "name", "Main 2");
    c[2] = with(b[2].toObject(), "executor", "armed", false);
    QHash<QString, int> ch2 = m.sync(c);
    QCOMPARE(int(s.changed.count()), 2);
    QCOMPARE(ch2.value("main"), int(ChannelListModel::Profile));
    QCOMPARE(ch2.value("ch3"), int(ChannelListModel::Executor));
    QJsonArray d = c;
    QJsonObject e = c[0].toObject(); e["engineState"] = "PAUSED"; d[0] = e;
    QJsonObject f = c[1].toObject(); f["hasPreview"] = false; d[1] = f;
    ch2 = m.sync(d);
    QCOMPARE(ch2.value("main"), int(ChannelListModel::Engine));
    QCOMPARE(ch2.value("ch2"), int(ChannelListModel::Other));
    QCOMPARE(ChannelListModel::diff(chan("x"), chan("x")), 0);
    QCOMPARE(ChannelListModel::diff(chan("x", true, 1), chan("x", true, 2)), int(ChannelListModel::Process));
}

// Added, removed and reordered channels: inserts / removes / moves, never a reset; persistent indexes (the delegates)
// follow their rows; the model ends up equal to the new list.
void TestChannelListModel::insertRemoveMoveInPlace() {
    ChannelListModel m;
    m.sync(arr({chan("a"), chan("b"), chan("c"), chan("d")}));
    const QPersistentModelIndex pb(m.index(1)), pd(m.index(3));
    ModelSpy s(&m);
    QSignalSpy count(&m, &ChannelListModel::countChanged);
    const QJsonArray next = arr({chan("d"), chan("b", false), chan("e"), chan("a")});   // c gone, d first, b changed, e new
    const QHash<QString, int> ch = m.sync(next);
    QCOMPARE(m.ids(), QStringList({"d", "b", "e", "a"}));
    for (int i = 0; i < next.size(); ++i) QCOMPARE(m.get(i), next[i].toObject().toVariantMap());
    qInfo().noquote() << QString("reorder: resets %1, inserts %2, removes %3, moves %4, dataChanged %5")
                             .arg(s.reset.count()).arg(s.inserted.count()).arg(s.removed.count()).arg(s.moved.count()).arg(s.changed.count());
    QCOMPARE(int(s.reset.count()), 0);
    QCOMPARE(int(s.layout.count()), 0);
    QCOMPARE(int(s.removed.count()), 1);
    QCOMPARE(int(s.inserted.count()), 1);
    QVERIFY(s.moved.count() >= 1);
    QCOMPARE(int(s.changed.count()), 1);   // only b's content changed
    QCOMPARE(int(count.count()), 0);       // 4 -> 4
    QCOMPARE(pb.row(), 1);
    QCOMPARE(pd.row(), 0);
    QCOMPARE(ch.value("c"), int(ChannelListModel::Removed));
    QVERIFY(ch.value("e") & ChannelListModel::Inserted);
    QVERIFY(ch.value("b") & ChannelListModel::Process);
    QVERIFY(ch.value("d") & ChannelListModel::Moved);
    QVERIFY(!ch.contains("a") || !(ch.value("a") & ~int(ChannelListModel::Moved)));   // a's content did not change

    // Shrink to one, then empty: removes and countChanged, still no reset.
    s.clear(); count.clear();
    m.sync(arr({chan("e")}));
    QCOMPARE(m.ids(), QStringList({"e"}));
    m.sync(QJsonArray());
    QCOMPARE(m.count(), 0);
    QCOMPARE(int(s.reset.count()), 0);
    QCOMPARE(int(s.removed.count()), 4);
    QCOMPARE(int(count.count()), 2);

    // A handler that reads the list while rows change (QML reading supervisor.channels in onRowsRemoved) gets the new
    // list, not the one cached before the change.
    ChannelListModel m2;
    m2.sync(arr({chan("a"), chan("b"), chan("c")}));
    QCOMPARE(m2.toVariantList().size(), 3);   // cached
    QStringList seen;
    auto look = [&](const char* what) {
        QStringList ids;
        for (const QVariant& v : m2.toVariantList()) ids << v.toMap().value("profile").toMap().value("id").toString();
        seen << QString("%1:%2").arg(what, ids.join(','));
    };
    connect(&m2, &QAbstractItemModel::rowsRemoved, &m2, [&]() { look("removed"); });
    connect(&m2, &QAbstractItemModel::rowsMoved, &m2, [&]() { look("moved"); });
    connect(&m2, &QAbstractItemModel::rowsInserted, &m2, [&]() { look("inserted"); });
    m2.sync(arr({chan("c"), chan("a"), chan("d")}));   // b gone, c up, d new
    m2.upsert(chan("e"));
    QCOMPARE(seen, QStringList({"removed:a,c", "moved:c,a", "inserted:c,a,d", "inserted:c,a,d,e"}));
}

// A single-channel status: in place, appended when new, nothing when equal. Duplicate ids cannot be matched row by row:
// the only case that resets (once going in, once coming out), then matching resumes.
void TestChannelListModel::upsertAndDuplicateIds() {
    ChannelListModel m;
    m.sync(arr({chan("a"), chan("b")}));
    ModelSpy s(&m);
    QSignalSpy count(&m, &ChannelListModel::countChanged);
    QCOMPARE(m.upsert(chan("b")), 0);
    QCOMPARE(s.structural() + int(s.changed.count()), 0);
    QCOMPARE(m.upsert(chan("b", true, 0, false)), int(ChannelListModel::Executor));
    QCOMPARE(int(s.changed.count()), 1);
    QCOMPARE(m.upsert(chan("c")), int(ChannelListModel::Inserted));
    QCOMPARE(int(s.inserted.count()), 1);
    QCOMPARE(m.ids(), QStringList({"a", "b", "c"}));
    QCOMPARE(int(count.count()), 1);

    s.clear();
    m.sync(arr({chan("a"), chan("a"), chan("b")}));
    QCOMPARE(int(s.reset.count()), 1);
    QCOMPARE(m.count(), 3);
    m.sync(arr({chan("a"), chan("b")}));
    QCOMPARE(int(s.reset.count()), 2);
    QVERIFY(m.sync(arr({chan("a"), chan("b")})).isEmpty());
    QCOMPARE(int(s.reset.count()), 2);
    QCOMPARE(m.stats().resets, quint64(2));
}

// The QML side: delegates over the model are created once and updated in place (modelData, index, count / length work as
// with the JS array); delegates over a JS array re-assigned per snapshot (the old `supervisor.channels`) are rebuilt.
void TestChannelListModel::qmlDelegatesUpdatedInPlace() {
    ChannelListModel m;
    m.sync(arr({chan("a"), chan("b"), chan("c")}));
    QQmlEngine eng;
    eng.rootContext()->setContextProperty("channelModel", &m);
    eng.rootContext()->setContextProperty("channelArray", m.toVariantList());
    QQmlComponent comp(&eng);
    comp.setData(R"(
import QtQml
import QtQml.Models
QtObject {
    id: root
    property int created: 0
    property int createdOld: 0
    property int len: channelModel.length
    property int cnt: channelModel.count
    property Instantiator inst: Instantiator {
        model: channelModel
        delegate: QtObject {
            property string cid: modelData.profile.id
            property bool on: !!(modelData.process && modelData.process.connected)
            property double hb: modelData.process.lastHeartbeatMs
            property int row: index
            Component.onCompleted: root.created++
        }
    }
    property Instantiator old: Instantiator {
        model: channelArray
        delegate: QtObject { property string cid: modelData.profile.id; Component.onCompleted: root.createdOld++ }
    }
}
)", QUrl::fromLocalFile(QDir::temp().filePath("pptm_delegates.qml")));   // a local URL: imports resolve synchronously (never written)
    std::unique_ptr<QObject> root(comp.create());
    QVERIFY2(root, qPrintable(QString("status %1: %2").arg(int(comp.status())).arg(comp.errorString())));
    QObject* inst = root->property("inst").value<QObject*>();
    QVERIFY(inst);
    QCOMPARE(root->property("created").toInt(), 3);
    QCOMPARE(root->property("createdOld").toInt(), 3);
    QCOMPARE(root->property("len").toInt(), 3);
    QCOMPARE(root->property("cnt").toInt(), 3);
    QObject* b0 = objectAt(inst, 1);
    QVERIFY(b0);
    QCOMPARE(b0->property("cid").toString(), QString("b"));
    QCOMPARE(b0->property("on").toBool(), true);
    QCOMPARE(b0->property("row").toInt(), 1);

    const int snapshots = 5;
    for (int i = 1; i <= snapshots; ++i) {   // every snapshot carries new heartbeats (as the Supervisor's do); b drops on the 3rd
        m.sync(arr({chan("a", true, i), chan("b", i < 3, i), chan("c", true, i)}));
        eng.rootContext()->setContextProperty("channelArray", m.toVariantList());   // what NOTIFY on `channels` did to the array models
    }
    const int created = root->property("created").toInt(), createdOld = root->property("createdOld").toInt();
    qInfo().noquote() << QString("%1 changed snapshots, 3 channels: delegates created over the model %2, over a JS array %3").arg(snapshots).arg(created).arg(createdOld);
    QCOMPARE(created, 3);                       // never rebuilt
    QCOMPARE(createdOld, 3 + 3 * snapshots);    // rebuilt on every snapshot
    QCOMPARE(objectAt(inst, 1), b0);            // the same delegate object ...
    QCOMPARE(b0->property("on").toBool(), false);   // ... with the new data
    QCOMPARE(b0->property("hb").toDouble(), double(snapshots));

    m.sync(arr({chan("a", true, 9), chan("b", false, 9), chan("c", true, 9), chan("d")}));
    QCOMPARE(root->property("created").toInt(), 4);
    QCOMPARE(root->property("len").toInt(), 4);
    QCOMPARE(objectAt(inst, 3)->property("cid").toString(), QString("d"));
    QCOMPARE(objectAt(inst, 1), b0);
    root.reset();
}

// Meters ride along with preview frames (here 100/s for the controlled channel, 50/s for another, 50/s input monitor
// frames without meters): activeMeters notifies at most 15 Hz and always ends on the newest value; a QML binding on it is
// evaluated that often instead of once per preview frame of any source (the old NOTIFY previewChanged).
void TestChannelListModel::metersChangedAtMost15Hz() {
    const QString b64 = tinyJpegBase64();
    if (b64.isEmpty()) QSKIP("no JPEG image plugin (set QT_PLUGIN_PATH to the Qt plugins folder)");
    FakeSupervisor fake;
    const QString name = uniqueName("meters");
    QVERIFY(fake.start(name));
    SupervisorClient c;
    c.setActiveChannelId("perftest-a");
    QQmlEngine eng;
    EvalCounter counter;
    eng.rootContext()->setContextProperty("supervisor", &c);
    eng.rootContext()->setContextProperty("counter", &counter);
    QQmlComponent comp(&eng);
    comp.setData(R"(
import QtQml
QtObject {
    property var m: { counter.hit("new"); return supervisor.activeMeters }
    property var old: { counter.hit("old"); var s = supervisor.previewSerial; return supervisor.metersOf(supervisor.activeChannelId) }
    property var mixes: { counter.hit("mixes"); return supervisor.activeMixes }
}
)", QUrl::fromLocalFile(QDir::temp().filePath("pptm_meters.qml")));
    std::unique_ptr<QObject> root(comp.create());
    QVERIFY2(root, qPrintable(QString("status %1: %2").arg(int(comp.status())).arg(comp.errorString())));
    counter.n.clear();

    QElapsedTimer clock; clock.start();
    QHash<QString, QList<qint64>> at;   // channel -> metersChanged times (ns)
    int notifiedSeq = -1;               // the meters QML saw at the last activeMetersChanged
    connect(&c, &SupervisorClient::metersChanged, &c, [&](const QString& ch) { at[ch].append(clock.nsecsElapsed()); });
    connect(&c, &SupervisorClient::activeMetersChanged, &c, [&]() { notifiedSeq = c.meters("perftest-a").value("seq").toInt(); });
    QSignalSpy active(&c, &SupervisorClient::activeMetersChanged), mixes(&c, &SupervisorClient::activeMixesChanged), previews(&c, &SupervisorClient::previewChanged);
    c.start(opts(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);

    auto meters = [](int i, const QString& webName = "Web") {
        return QJsonObject{{"L", -10.0 - (i % 17) * 0.5}, {"R", -11.0}, {"seq", i},
                           {"mixes", QJsonArray{QJsonObject{{"id", "main"}, {"name", "Main"}, {"channelPeaksDbfs", QJsonArray{-10.0 - i % 5}}},
                                                QJsonObject{{"id", "web"}, {"name", webName}, {"follow", true}, {"channelPeaksDbfs", QJsonArray{-20.0 - i % 3}}}}}};
    };
    const int rate = 200, ms = 2000, n = rate * ms / 1000;
    int lastA = -1;
    for (int i = 0; i < n; ++i) if (i % 4 == 0 || i % 4 == 2) lastA = i;
    fake.stream(rate, ms, [&](int i) {
        using namespace IpcProtocol;
        QJsonObject p{{"jpegBase64", b64}, {"width", 64}, {"height", 36}, {"frame", i}};
        QString ch = "perftest-a";
        if (i % 4 == 1) ch = "perftest-b";
        else if (i % 4 == 3) ch = "perftest-a:in-1";   // an input monitor: picture only
        if (i % 4 != 3) p["meters"] = meters(i);
        return make(MSG_PREVIEW_FRAME, ch, p);
    });
    QTRY_VERIFY_WITH_TIMEOUT(fake.streamDone.load(), ms + 2000);
    QTRY_COMPARE_WITH_TIMEOUT(c.meters("perftest-a").value("seq").toInt(), lastA, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(notifiedSeq, lastA, 500);   // the newest value was notified too (the booked slot went out)
    QCOMPARE(c.activeMeters().value("seq").toInt(), lastA);
    QTest::qWait(150);

    auto minGapMs = [&](const QString& ch) {
        const QList<qint64>& t = at[ch]; double g = 1e9;
        for (int i = 1; i < t.size(); ++i) g = qMin(g, (t[i] - t[i - 1]) / 1e6);
        return g;
    };
    const SupervisorClient::UiStats ui = c.uiStats();
    const int newEvals = counter.n.value("new"), oldEvals = counter.n.value("old");
    qInfo().noquote() << QString("meters for 2 s (100/s controlled channel, 50/s other, 50/s input pictures): previews shown %1, meters in %2; "
                                 "activeMetersChanged %3 (min gap %4 ms), metersChanged other channel %5 (min gap %6 ms); "
                                 "QML binding evaluations: new NOTIFY %7, old NOTIFY (previewChanged) %8; activeMixesChanged %9")
                             .arg(previews.count()).arg(ui.metersIn).arg(active.count()).arg(minGapMs("perftest-a"), 0, 'f', 1)
                             .arg(at.value("perftest-b").size()).arg(minGapMs("perftest-b"), 0, 'f', 1).arg(newEvals).arg(oldEvals).arg(mixes.count());
    QVERIFY(previews.count() >= n / 2);   // most frames shown (the worker coalesces per source only when the GUI lags)
    QVERIFY2(active.count() <= ms / SupervisorClient::kMeterIntervalMs + 3, qPrintable(QString("%1 activeMetersChanged").arg(active.count())));
    QVERIFY2(active.count() >= 10, qPrintable(QString("%1 activeMetersChanged").arg(active.count())));
    QVERIFY2(minGapMs("perftest-a") >= 65.0, qPrintable(QString("gap %1 ms").arg(minGapMs("perftest-a"))));
    QVERIFY2(minGapMs("perftest-b") >= 65.0, qPrintable(QString("gap %1 ms").arg(minGapMs("perftest-b"))));
    QVERIFY(!at.contains("perftest-a:in-1"));   // no meters, no meter signal
    QVERIFY2(newEvals <= active.count() + 1, qPrintable(QString("%1 evaluations for %2 notifications").arg(newEvals).arg(active.count())));
    QVERIFY2(oldEvals >= previews.count(), qPrintable(QString("old %1, previews %2").arg(oldEvals).arg(previews.count())));
    QVERIFY2(newEvals * 5 < oldEvals, qPrintable(QString("new %1 vs old %2").arg(newEvals).arg(oldEvals)));
    QCOMPARE(int(mixes.count()), 1);   // levels moved 30 times a second; the mix list did not
    QCOMPARE(counter.n.value("mixes"), 1);
    QCOMPARE(c.activeMixes().size(), 2);
    QCOMPARE(c.activeMixes().at(1).toMap(), (QVariantMap{{"id", "web"}, {"name", "Web"}, {"follow", true}}));

    // Silence: the same levels again and again tell QML nothing after the first.
    const int activeBefore = int(active.count());
    const SupervisorClient::UiStats uiBefore = c.uiStats();
    const quint64 receivedBefore = c.ipcStats().received;
    const QJsonObject still = meters(1000);
    for (int i = 0; i < 30; ++i) {
        fake.broadcast(IpcProtocol::make(IpcProtocol::MSG_PREVIEW_FRAME, "perftest-a", QJsonObject{{"jpegBase64", b64}, {"frame", 5000 + i}, {"meters", still}}));
        QTest::qWait(3);
    }
    QTRY_VERIFY_WITH_TIMEOUT(c.ipcStats().received >= receivedBefore + 30, 2000);
    QTest::qWait(150);
    const quint64 inDelta = c.uiStats().metersIn - uiBefore.metersIn;   // (the worker may coalesce a few before the GUI sees them)
    qInfo().noquote() << QString("same levels x30: %1 reached the GUI, %2 identical, activeMetersChanged +%3")
                             .arg(inDelta).arg(c.uiStats().metersSame - uiBefore.metersSame).arg(active.count() - activeBefore);
    QVERIFY(inDelta >= 2);
    QCOMPARE(c.uiStats().metersSame - uiBefore.metersSame, inDelta - 1);
    QCOMPARE(int(active.count()), activeBefore + 1);

    // A mix renamed: one activeMixesChanged.
    fake.broadcast(IpcProtocol::make(IpcProtocol::MSG_PREVIEW_FRAME, "perftest-a", QJsonObject{{"jpegBase64", b64}, {"meters", meters(2000, "Web 2")}}));
    QTRY_COMPARE_WITH_TIMEOUT(int(mixes.count()), 2, 2000);
    QCOMPARE(c.activeMixes().at(1).toMap().value("name").toString(), QString("Web 2"));
    root.reset();
    c.stop();
}

// The active* properties notify for the active channel only; an unchanged snapshot (or single-channel status) tells
// nobody anything; switching the active channel notifies each active* property once.
void TestChannelListModel::activeSignalsFollowOnlyActiveChannel() {
    using namespace IpcProtocol;
    FakeSupervisor fake;
    const QString name = uniqueName("active");
    QVERIFY(fake.start(name));
    SupervisorClient c;
    c.setActiveChannelId("perftest-a");
    c.start(opts(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);
    QSignalSpy snap(&c, &SupervisorClient::snapshotChanged), chans(&c, &SupervisorClient::channelsChanged),
        data(&c, &SupervisorClient::activeChannelDataChanged), prof(&c, &SupervisorClient::activeProfileChanged),
        exec(&c, &SupervisorClient::activeExecutorChanged), state(&c, &SupervisorClient::activeStateChanged),
        tel(&c, &SupervisorClient::telemetryChanged), run(&c, &SupervisorClient::activeRunlistChanged),
        stateEv(&c, &SupervisorClient::stateChanged), meters(&c, &SupervisorClient::activeMetersChanged);
    ModelSpy ms(c.channelModel());
    auto send = [&](const Message& m) {
        const quint64 before = c.messagesApplied();
        fake.broadcast(m);
        return QTest::qWaitFor([&]() { return c.messagesApplied() > before; }, 2000);
    };
    auto snapshotOf = [](double uptime, const QJsonArray& channels) { return make(MSG_UI_STATUS, QString(), QJsonObject{{"uptimeMs", uptime}, {"channels", channels}}); };
    auto counts = [&]() {
        return QString("snapshot %1 channels %2 data %3 profile %4 executor %5 state %6 runlist %7")
            .arg(snap.count()).arg(chans.count()).arg(data.count()).arg(prof.count()).arg(exec.count()).arg(state.count()).arg(run.count());
    };

    QVERIFY(send(snapshotOf(1, arr({chan("perftest-a"), chan("perftest-b")}))));
    QCOMPARE(counts(), QString("snapshot 1 channels 1 data 1 profile 1 executor 1 state 0 runlist 0"));
    QCOMPARE(int(ms.inserted.count()), 2);
    QCOMPARE(c.channelIds(), QStringList({"perftest-a", "perftest-b"}));
    QCOMPARE(c.activeChannelData().value("profile").toMap().value("id").toString(), QString("perftest-a"));
    QCOMPARE(c.activeProfile().value("name").toString(), QString("PERFTEST-A"));
    QCOMPARE(c.activeExecutor().value("armed").toBool(), true);
    QCOMPARE(c.channelOf("perftest-b").value("profile").toMap().value("id").toString(), QString("perftest-b"));
    QCOMPARE(c.channelsList().size(), 2);
    QCOMPARE(c.snapshot().value("uptimeMs").toDouble(), 1.0);

    QVERIFY(send(snapshotOf(1, arr({chan("perftest-a"), chan("perftest-b")}))));   // the same snapshot (the 4 s poll)
    QCOMPARE(counts(), QString("snapshot 1 channels 1 data 1 profile 1 executor 1 state 0 runlist 0"));
    QCOMPARE(c.uiStats().snapshotsSame, quint64(1));
    QVERIFY(send(snapshotOf(2, arr({chan("perftest-a"), chan("perftest-b")}))));   // only the clock moved
    QCOMPARE(counts(), QString("snapshot 2 channels 1 data 1 profile 1 executor 1 state 0 runlist 0"));
    QVERIFY(send(snapshotOf(3, arr({chan("perftest-a"), chan("perftest-b", true, 77)}))));   // the other channel's heartbeat
    QCOMPARE(counts(), QString("snapshot 3 channels 2 data 1 profile 1 executor 1 state 0 runlist 0"));
    QVERIFY(send(snapshotOf(4, arr({chan("perftest-a", true, 0, false), chan("perftest-b", true, 77)}))));   // active executor
    QCOMPARE(counts(), QString("snapshot 4 channels 3 data 2 profile 1 executor 2 state 0 runlist 0"));
    QCOMPARE(c.activeExecutor().value("armed").toBool(), false);
    QVERIFY(send(snapshotOf(5, arr({chan("perftest-a", true, 0, false, "Alpha"), chan("perftest-b", true, 77)}))));   // active profile
    QCOMPARE(counts(), QString("snapshot 5 channels 4 data 3 profile 2 executor 2 state 0 runlist 0"));
    QCOMPARE(c.activeProfile().value("name").toString(), QString("Alpha"));
    QCOMPARE(int(ms.reset.count()), 0);
    QCOMPARE(int(ms.changed.count()), 3);   // b heartbeat, a executor, a profile: rows updated in place

    // Single-channel status: the other channel changes -> channels only; the active one unchanged -> nothing.
    QVERIFY(send(make(MSG_UI_STATUS, "perftest-b", chan("perftest-b", false, 78))));
    QCOMPARE(counts(), QString("snapshot 6 channels 5 data 3 profile 2 executor 2 state 0 runlist 0"));
    QCOMPARE(c.snapshot().value("channels").toList().at(1).toMap().value("process").toMap().value("connected").toBool(), false);
    QVERIFY(send(make(MSG_UI_STATUS, "perftest-a", chan("perftest-a", true, 0, false, "Alpha"))));
    QCOMPARE(counts(), QString("snapshot 6 channels 5 data 3 profile 2 executor 2 state 0 runlist 0"));

    // Telemetry and runlists: the other channel never notifies activeState / activeRunlist.
    QVERIFY(send(make(MSG_UI_TELEMETRY, "perftest-b", QJsonObject{{"frame", 1}, {"state", "ON_AIR"}})));
    QCOMPARE(int(tel.count()), 1);
    QCOMPARE(int(state.count()), 0);
    QVERIFY(send(make(MSG_UI_TELEMETRY, "perftest-a", QJsonObject{{"frame", 2}, {"state", "ON_AIR"}, {"title", "News"}})));
    QCOMPARE(int(state.count()), 1);
    QCOMPARE(c.activeState().value("title").toString(), QString("News"));
    QVERIFY(send(make(MSG_UI_TELEMETRY, "perftest-a", QJsonObject{{"state", "ERROR"}, {"reason", "decoder"}})));   // a state-change event
    QCOMPARE(int(stateEv.count()), 1);
    QCOMPARE(int(state.count()), 1);
    QVERIFY(send(make(MSG_UI_RUNLIST, "perftest-b", QJsonObject{{"items", QJsonArray{}}})));
    QCOMPARE(int(run.count()), 0);
    QVERIFY(send(make(MSG_UI_RUNLIST, "perftest-a", QJsonObject{{"current", QJsonObject{{"title", "News"}}}})));
    QCOMPARE(int(run.count()), 1);
    QCOMPARE(c.activeRunlist().value("current").toMap().value("title").toString(), QString("News"));

    // Switching channels: each active* property once, now showing the other channel.
    const QString before = counts();
    const int metersBefore = int(meters.count());
    c.setActiveChannelId("perftest-b");
    QCOMPARE(counts(), QString("snapshot 6 channels 5 data 4 profile 3 executor 3 state 2 runlist 2"));
    QCOMPARE(int(meters.count()), metersBefore + 1);
    QCOMPARE(c.activeState().value("frame").toInt(), 1);
    QCOMPARE(c.activeProfile().value("id").toString(), QString("perftest-b"));
    QVERIFY(c.activeRunlist().value("items").isValid());
    c.setActiveChannelId("perftest-b");   // the same: nothing
    QCOMPARE(counts(), QString("snapshot 6 channels 5 data 4 profile 3 executor 3 state 2 runlist 2"));
    Q_UNUSED(before);

    // The active channel disappears from the snapshot: its data, profile and executor become empty, once.
    QVERIFY(send(snapshotOf(7, arr({chan("perftest-a", true, 0, false, "Alpha")}))));
    QCOMPARE(counts(), QString("snapshot 7 channels 6 data 5 profile 4 executor 4 state 2 runlist 2"));
    QVERIFY(c.activeChannelData().isEmpty());
    QVERIFY(c.activeProfile().isEmpty());
    QCOMPARE(c.channelIds(), QStringList({"perftest-a"}));
    QCOMPARE(int(ms.removed.count()), 1);
    QCOMPARE(int(ms.reset.count()), 0);
    qInfo().noquote() << "counters:" << QJsonDocument(QJsonObject::fromVariantMap(c.uiCounters())).toJson(QJsonDocument::Compact);
    c.stop();
}

// The Supervisor goes away: the telemetry the old connection delivered is dropped out loud (telemetryChanged per channel,
// activeStateChanged once, after isConnected() turned false). Delegates now outlive snapshots, so a tile that took its
// telemetry through a Connections handler (OnAirView's bank MonitorTile, ControlColumn's ChannelCard) falls back to the
// snapshot's engineState instead of keeping the old connection's ON AIR until that engine reports again (or for good).
void TestChannelListModel::disconnectDropsTelemetryOutLoud() {
    using namespace IpcProtocol;
    FakeSupervisor fake;
    const QString name = uniqueName("drop");
    QVERIFY(fake.start(name));
    SupervisorClient c;
    c.setActiveChannelId("perftest-a");
    QQmlEngine eng;
    eng.rootContext()->setContextProperty("supervisor", &c);
    QQmlComponent comp(&eng);
    comp.setData(R"(
import QtQml
import QtQml.Models
QtObject {
    readonly property string activeWord: supervisor.activeState && supervisor.activeState.state ? supervisor.activeState.state : "OFFLINE"
    property Instantiator inst: Instantiator {
        model: supervisor.channelModel
        delegate: QtObject {
            id: tile
            readonly property string cid: modelData.profile.id
            property var tel: supervisor.telemetryOf(cid)
            readonly property string cst: tel && tel.state ? tel.state : (modelData.engineState || "OFFLINE")
            property Connections watch: Connections { target: supervisor; function onTelemetryChanged(id) { if (id === tile.cid) tile.tel = supervisor.telemetryOf(id) } }
        }
    }
}
)", QUrl::fromLocalFile(QDir::temp().filePath("pptm_drop.qml")));
    std::unique_ptr<QObject> root(comp.create());
    QVERIFY2(root, qPrintable(QString("status %1: %2").arg(int(comp.status())).arg(comp.errorString())));
    c.start(opts(name));
    QTRY_VERIFY_WITH_TIMEOUT(c.isConnected(), 2000);
    auto send = [&](const Message& m) {
        const quint64 before = c.messagesApplied();
        fake.broadcast(m);
        return QTest::qWaitFor([&]() { return c.messagesApplied() > before; }, 2000);
    };
    // The channel list says OFFLINE (what a Supervisor shows for an engine that is not up); the engines' telemetry ON_AIR.
    QVERIFY(send(make(MSG_UI_STATUS, QString(), QJsonObject{{"uptimeMs", 1}, {"channels", arr({chan("perftest-a", false), chan("perftest-b", false)})}})));
    QVERIFY(send(make(MSG_UI_TELEMETRY, "perftest-a", QJsonObject{{"frame", 1}, {"state", "ON_AIR"}, {"title", "News"}})));
    QVERIFY(send(make(MSG_UI_TELEMETRY, "perftest-b", QJsonObject{{"frame", 1}, {"state", "ON_AIR"}})));
    QObject* inst = root->property("inst").value<QObject*>();
    QVERIFY(inst);
    QCOMPARE(inst->property("count").toInt(), 2);
    QObject* ta = objectAt(inst, 0);
    QObject* tb = objectAt(inst, 1);
    QVERIFY(ta && tb);
    QCOMPARE(ta->property("cst").toString(), QString("ON_AIR"));
    QCOMPARE(tb->property("cst").toString(), QString("ON_AIR"));
    QCOMPARE(root->property("activeWord").toString(), QString("ON_AIR"));

    QSignalSpy tel(&c, &SupervisorClient::telemetryChanged), state(&c, &SupervisorClient::activeStateChanged), conn(&c, &SupervisorClient::connectedChanged);
    int whileConnected = 0;   // the engine adapter re-reads observe() on telemetryChanged: it must already see "disconnected"
    connect(&c, &SupervisorClient::telemetryChanged, &c, [&]() { if (c.isConnected()) ++whileConnected; });
    fake.stopServer();
    QTRY_VERIFY_WITH_TIMEOUT(!c.isConnected(), 3000);
    QStringList ids;
    for (int i = 0; i < tel.count(); ++i) ids << tel.at(i).at(0).toString();
    ids.sort();
    qInfo().noquote() << QString("disconnect: telemetryChanged %1, activeStateChanged %2, tiles now %3 / %4, active %5")
                             .arg(ids.join(',')).arg(state.count()).arg(ta->property("cst").toString(), tb->property("cst").toString(), root->property("activeWord").toString());
    QCOMPARE(ids, QStringList({"perftest-a", "perftest-b"}));
    QCOMPARE(int(state.count()), 1);
    QCOMPARE(whileConnected, 0);
    QVERIFY(conn.count() >= 1);
    QVERIFY(c.telemetryOf("perftest-b").isEmpty());
    QVERIFY(c.activeState().isEmpty());
    QCOMPARE(objectAt(inst, 0), ta);   // the same delegates ...
    QCOMPARE(objectAt(inst, 1), tb);
    QCOMPARE(ta->property("cst").toString(), QString("OFFLINE"));   // ... no longer showing the old connection's ON AIR
    QCOMPARE(tb->property("cst").toString(), QString("OFFLINE"));
    QCOMPARE(root->property("activeWord").toString(), QString("OFFLINE"));
    root.reset();
    c.stop();
}

QTEST_GUILESS_MAIN(TestChannelListModel)
#include "test_channellistmodel.moc"
