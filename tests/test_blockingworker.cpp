// PS-8 (perf-supervisor-app W14, ingest part): BlockingWorker and the IngestManager work that moved onto it.
// The worker: results on the owner's thread, FIFO, coalescing by key, cancel of queued and running jobs, the loop
// stays responsive while a job blocks, destruction with a hung job does not block, a result for a destroyed worker is
// dropped. The ingest paths: the async sweep produces the same records as the synchronous one; a 10,000-file watch
// folder is walked on the worker with the loop lag under 100 ms and the same files queued as the old rules; an
// unreachable UNC watch path (a ~21 s stat on this box) holds the worker, not the loop; the drive poll primes off the
// loop. Nothing here touches the live stack: temp dirs, temp SQLite stores, no engines, no ports.
#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include "playout/supervisor/blockingworker.h"
#include "playout/supervisor/ingestmanager.h"
#include "playout/schedule/schedulestore.h"
#include <atomic>

using namespace sched;

namespace {

/// Max gap between 10 ms timer fires while events are processed for `ms`: the loop lag a watchdog would see.
struct LoopGauge {
    QTimer timer; QElapsedTimer clock; qint64 last = 0, maxGap = 0; int fires = 0;
    LoopGauge() { timer.setTimerType(Qt::PreciseTimer); timer.setInterval(10); QObject::connect(&timer, &QTimer::timeout, [this]() { const qint64 now = clock.elapsed(); maxGap = qMax(maxGap, now - last - 10); last = now; ++fires; }); }
    void start() { clock.start(); last = 0; maxGap = 0; fires = 0; timer.start(); }
    void stop() { timer.stop(); }
};

void writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path); QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path)); f.write(bytes); f.close();
}

/// The media table as a comparable JSON array: sorted by id, missingSinceMs reduced to a flag (it is a timestamp),
/// the fixture's folder replaced by a placeholder (two fixtures live in two temp dirs).
QJsonArray mediaSnapshot(ScheduleStore& store, const QString& filesDir) {
    QList<MediaRecord> all = store.allMedia(true);
    std::sort(all.begin(), all.end(), [](const MediaRecord& a, const MediaRecord& b) { return a.id < b.id; });
    QJsonArray out;
    for (const MediaRecord& m : all) {
        MediaRecord n = m; n.path.replace(QDir::toNativeSeparators(filesDir), "<files>");
        QJsonObject o = n.toJson();
        QJsonObject extra = o.value("extra").toObject();
        if (extra.contains("missingSinceMs")) extra["missingSinceMs"] = extra.value("missingSinceMs").toDouble() > 0;
        o["extra"] = extra;
        if (o.contains("missingSinceMs")) o["missingSinceMs"] = o.value("missingSinceMs").toDouble() > 0;   // toJson() flattens extra
        o.remove("addedAtMs"); o.remove("added_at");
        out.append(o);
    }
    return out;
}

struct SweepFixture {
    QTemporaryDir dir; ScheduleStore store; QString files;
    bool seed(const QString& tag) {
        if (!dir.isValid()) return false;
        files = dir.filePath("files"); QDir().mkpath(files);
        if (!store.open(dir.filePath("t.db"))) return false;
        auto rec = [&](const QString& id, const QString& name, bool exists, bool missing, const QString& type = "program") {
            MediaRecord m; m.id = id; m.title = name; m.type = type; m.path = QDir::toNativeSeparators(QDir(files).filePath(name)); m.durationSec = 10;
            m.extra["missing"] = missing; if (missing) m.extra["missingSinceMs"] = 1234.0;
            if (exists) { QFile f(m.path); f.open(QIODevice::WriteOnly); f.write("x"); }
            return store.saveMedia(m);
        };
        bool ok = rec("a-" + tag, "a.mp4", true, false) && rec("b-" + tag, "b.mp4", false, false) && rec("c-" + tag, "c.mp4", true, true) && rec("d-" + tag, "d.mp4", false, true)
               && rec("e-" + tag, "e.mp4", false, false, "live");
        MediaRecord live; live.id = "f-" + tag; live.title = "cam"; live.type = "program"; live.path = "live://cam1"; ok = ok && store.saveMedia(live);
        return ok;
    }
};

} // namespace

class TestBlockingWorker : public QObject {
    Q_OBJECT
private slots:
    void deliversOnOwnerThreadInOrder();
    void coalescesQueuedKeys();
    void cancelQueuedAndRunning();
    void loopStaysResponsiveWhileBlocked();
    void destroyWithHungJobDoesNotBlock();
    void resultForDestroyedWorkerIsDropped();
    void ingestSweepAsyncMatchesSync();
    void ingestDrivePollPrimesOffLoop();
    void ingestWatchWalkOffLoop();
    void ingestUncWatchPathDoesNotBlockLoop();
};

void TestBlockingWorker::deliversOnOwnerThreadInOrder() {
    BlockingWorker w("t-order");
    QThread* main = QThread::currentThread();
    QList<int> order; QList<QThread*> workThreads; std::atomic<int> doneCount{0};
    for (int i = 0; i < 5; ++i) {
        const quint64 id = w.post<int>(QByteArray("k") + QByteArray::number(i),
            [i, &workThreads](const BlockingWorker::Token&) { workThreads << QThread::currentThread(); QThread::msleep(5); return i * 10; },
            [&, main](int v) { QVERIFY(QThread::currentThread() == main); order << v; ++doneCount; });
        QVERIFY(id > 0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(doneCount.load(), 5, 5000);
    QCOMPARE(order, (QList<int>{0, 10, 20, 30, 40}));
    for (QThread* t : workThreads) QVERIFY(t != main);
    QCOMPARE(workThreads.size(), 5);
    const BlockingWorker::Stats s = w.stats();
    QCOMPARE(s.done, quint64(5)); QCOMPARE(s.queued, 0); QVERIFY(!s.running); QCOMPARE(s.cancelled, quint64(0));
    QVERIFY(w.isIdle());
    QCOMPARE(w.statsJson().value("name").toString(), QString("t-order"));
}

void TestBlockingWorker::coalescesQueuedKeys() {
    BlockingWorker w("t-coalesce");
    QSemaphore gate; std::atomic<int> ran{0}; QList<int> seen;
    // Block the worker, then post the same key five times: one waits, with the newest closure.
    w.post("block", [&gate](const BlockingWorker::Token&) -> BlockingWorker::Done { gate.acquire(); return BlockingWorker::Done(); });
    QTRY_VERIFY_WITH_TIMEOUT(w.stats().running, 2000);
    quint64 lastId = 0;
    for (int i = 1; i <= 5; ++i) lastId = w.post<int>("walk", [i, &ran](const BlockingWorker::Token&) { ++ran; return i; }, [&seen](int v) { seen << v; });
    BlockingWorker::Stats s = w.stats();
    QCOMPARE(s.queued, 1); QCOMPARE(s.coalesced, quint64(4)); QVERIFY(w.isBusy("walk")); QVERIFY(lastId > 0);
    // A different key queues on its own.
    w.post<int>("other", [](const BlockingWorker::Token&) { return 99; }, [&seen](int v) { seen << v; });
    QCOMPARE(w.stats().queued, 2);
    gate.release();
    QTRY_COMPARE_WITH_TIMEOUT(seen.size(), 2, 5000);
    QCOMPARE(seen, (QList<int>{5, 99}));     // the newest "walk" closure ran, once, before "other" (FIFO slot kept)
    QCOMPARE(ran.load(), 1);
    QVERIFY(!w.isBusy("walk"));
    // The queue cap refuses, counted.
    BlockingWorker::Options o; o.name = "t-cap"; o.maxQueue = 2;
    BlockingWorker c(o);
    c.post("b", [&gate](const BlockingWorker::Token&) -> BlockingWorker::Done { gate.acquire(); return BlockingWorker::Done(); });
    QTRY_VERIFY_WITH_TIMEOUT(c.stats().running, 2000);
    QVERIFY(c.post("1", [](const BlockingWorker::Token&) { return BlockingWorker::Done(); }) > 0);
    QVERIFY(c.post("2", [](const BlockingWorker::Token&) { return BlockingWorker::Done(); }) > 0);
    QCOMPARE(c.post("3", [](const BlockingWorker::Token&) { return BlockingWorker::Done(); }), quint64(0));
    QCOMPARE(c.stats().refused, quint64(1));
    gate.release();
    QTRY_VERIFY_WITH_TIMEOUT(c.isIdle(), 5000);
}

void TestBlockingWorker::cancelQueuedAndRunning() {
    BlockingWorker w("t-cancel");
    QSemaphore started, gate; std::atomic<bool> sawCancel{false}; std::atomic<int> runs{0}; int delivered = 0;
    const quint64 running = w.post<int>("run", [&](const BlockingWorker::Token& t) {
        ++runs; started.release();
        for (int i = 0; i < 400 && !t->load(); ++i) QThread::msleep(5);
        sawCancel = t->load(); return 1; }, [&delivered](int) { ++delivered; });
    QVERIFY(started.tryAcquire(1, 3000));
    const quint64 queued = w.post<int>("q", [&runs](const BlockingWorker::Token&) { ++runs; return 2; }, [&delivered](int) { ++delivered; });
    QVERIFY(w.cancel(queued));                    // dropped before it ran
    QVERIFY(w.cancel(running));                   // flagged: the job sees the token, its Done is not delivered
    QVERIFY(!w.cancel(queued));                   // gone
    QVERIFY(!w.cancel(123456));                   // unknown
    QTRY_VERIFY_WITH_TIMEOUT(w.isIdle(), 5000);
    QTest::qWait(50);
    QVERIFY(sawCancel.load());
    QCOMPARE(runs.load(), 1);
    QCOMPARE(delivered, 0);
    const BlockingWorker::Stats s = w.stats();
    QCOMPARE(s.cancelled, quint64(2)); QCOMPARE(s.done, quint64(0));
    // After the cancels the worker still works.
    int v = 0; w.post<int>("after", [](const BlockingWorker::Token&) { return 7; }, [&v](int x) { v = x; });
    QTRY_COMPARE_WITH_TIMEOUT(v, 7, 3000);
    // cancelAll drops a queue.
    w.post("b", [&gate](const BlockingWorker::Token&) -> BlockingWorker::Done { gate.acquire(); return BlockingWorker::Done(); });
    QTRY_VERIFY_WITH_TIMEOUT(w.stats().running, 2000);
    for (int i = 0; i < 3; ++i) w.post(QByteArray("x") + QByteArray::number(i), [](const BlockingWorker::Token&) { return BlockingWorker::Done(); });
    QCOMPARE(w.stats().queued, 3);
    w.cancelAll();
    QCOMPARE(w.stats().queued, 0);
    gate.release();
    QTRY_VERIFY_WITH_TIMEOUT(w.isIdle(), 5000);
}

void TestBlockingWorker::loopStaysResponsiveWhileBlocked() {
    BlockingWorker w("t-block");
    bool done = false;
    w.post<int>("sleep", [](const BlockingWorker::Token&) { QThread::msleep(1500); return 1; }, [&done](int) { done = true; });
    LoopGauge g; g.start();
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    g.stop();
    qInfo() << "loop max gap while the worker slept 1.5 s:" << g.maxGap << "ms over" << g.fires << "fires";
    QVERIFY2(g.maxGap < 100, qPrintable(QString("max gap %1 ms").arg(g.maxGap)));
    QVERIFY(g.fires > 50);
    QVERIFY(w.stats().lastMs >= 1400);
    QVERIFY(w.stats().maxMs >= 1400);
}

void TestBlockingWorker::destroyWithHungJobDoesNotBlock() {
    BlockingWorker::Options o; o.name = "t-hung"; o.detachMs = 300;
    auto* w = new BlockingWorker(o);
    QSemaphore started; std::atomic<bool> finished{false}; bool delivered = false;
    w->post<int>("hang", [&](const BlockingWorker::Token&) { started.release(); QThread::msleep(2500); finished = true; return 1; }, [&delivered](int) { delivered = true; });
    w->post("never", [](const BlockingWorker::Token&) { return BlockingWorker::Done(); });
    QVERIFY(started.tryAcquire(1, 3000));
    QElapsedTimer t; t.start();
    delete w;                                         // waits detachMs, then leaves the thread
    const qint64 ms = t.elapsed();
    qInfo() << "destructor with a hung job returned after" << ms << "ms (detachMs 300)";
    QVERIFY2(ms < 1500, qPrintable(QString("destructor took %1 ms").arg(ms)));
    QTest::qWait(3000);                               // let the thread finish on its own; nothing must be delivered
    QVERIFY(finished.load());
    QVERIFY(!delivered);
}

void TestBlockingWorker::resultForDestroyedWorkerIsDropped() {
    auto* w = new BlockingWorker("t-drop");
    bool delivered = false;
    w->post<int>("quick", [](const BlockingWorker::Token&) { return 1; }, [&delivered](int) { delivered = true; });
    QElapsedTimer t0; t0.start();
    while (w->stats().done < 1 && t0.elapsed() < 3000) QThread::msleep(5);   // no event processing: the result stays queued
    QCOMPARE(w->stats().done, quint64(1));
    delete w;                                              // the queued call dies with the QObject
    QTest::qWait(100);
    QVERIFY(!delivered);
    // The idle destructor is immediate.
    auto* idle = new BlockingWorker("t-idle");
    QElapsedTimer t; t.start(); delete idle; QVERIFY(t.elapsed() < 200);
}

void TestBlockingWorker::ingestSweepAsyncMatchesSync() {
    SweepFixture A, B;
    QVERIFY(A.seed("x")); QVERIFY(B.seed("x"));
    QTemporaryDir dataA, dataB; QVERIFY(dataA.isValid() && dataB.isValid());
    IngestManager ma(&A.store, dataA.path(), dataA.filePath("up"));
    IngestManager mb(&B.store, dataB.path(), dataB.filePath("up"));
    // Sync path (the operator's REST call) on A.
    QSignalSpy doneA(&ma, &IngestManager::sweepDone);
    QCOMPARE(ma.sweepMissing(), 2);                       // b (gone) + d (still gone); c came back; a present; live rows skipped
    QCOMPARE(doneA.size(), 1); QCOMPARE(doneA.at(0).at(1).toInt(), 2);
    QCOMPARE(ma.workerStats().value("sweep").toObject().value("done").toInt(), 0);   // ran in place
    // Async path (the timer) on B.
    QSignalSpy doneB(&mb, &IngestManager::sweepDone);
    int cb = -1; LoopGauge g; g.start();
    mb.sweepMissingAsync([&cb](int missing) { cb = missing; });
    QVERIFY(doneB.wait(10000));
    g.stop();
    QCOMPARE(cb, 2); QCOMPARE(doneB.at(0).at(0).toInt(), 2); QCOMPARE(doneB.at(0).at(1).toInt(), 2);
    QCOMPARE(mb.workerStats().value("sweep").toObject().value("done").toInt(), 1);
    QVERIFY2(g.maxGap < 100, qPrintable(QString("max gap %1 ms").arg(g.maxGap)));
    // Identical records.
    const QJsonArray sa = mediaSnapshot(A.store, A.files), sb = mediaSnapshot(B.store, B.files);
    QCOMPARE(sa.size(), 6); QCOMPARE(sb.size(), 6);
    for (int i = 0; i < sa.size(); ++i) {
        const QJsonObject oa = sa[i].toObject(), ob = sb[i].toObject();
        for (auto it = oa.begin(); it != oa.end(); ++it)
            QVERIFY2(ob.value(it.key()) == it.value(), qPrintable(QString("%1 %2: sync %3 vs async %4").arg(oa.value("id").toString(), it.key(),
                QString::fromUtf8(QJsonDocument(QJsonObject{{"v", it.value()}}).toJson(QJsonDocument::Compact)), QString::fromUtf8(QJsonDocument(QJsonObject{{"v", ob.value(it.key())}}).toJson(QJsonDocument::Compact)))));
        QCOMPARE(ob.keys(), oa.keys());
    }
    bool f = false;
    MediaRecord b = B.store.media("b-x", &f); QVERIFY(f); QVERIFY(b.extra.value("missing").toBool()); QVERIFY(b.extra.value("missingSinceMs").toDouble() > 1234.0);
    MediaRecord c = B.store.media("c-x", &f); QVERIFY(f); QVERIFY(!c.extra.value("missing").toBool()); QCOMPARE(c.extra.value("missingSinceMs").toDouble(), 0.0);
    MediaRecord d = B.store.media("d-x", &f); QVERIFY(f); QVERIFY(d.extra.value("missing").toBool()); QCOMPARE(d.extra.value("missingSinceMs").toDouble(), 1234.0);   // untouched
    MediaRecord e = B.store.media("e-x", &f); QVERIFY(f); QVERIFY(!e.extra.value("missing").toBool());   // live type: never swept
    // A relink while the stats run is not undone: c.mp4 goes away (the worker will find it gone), and before the result
    // is applied the record is relinked to a.mp4. The apply step sees a different path and leaves it alone.
    QVERIFY(QFile::remove(QDir(B.files).filePath("c.mp4")));
    QSignalSpy done2(&mb, &IngestManager::sweepDone);
    mb.sweepMissingAsync();
    MediaRecord cc = B.store.media("c-x"); cc.path = QDir::toNativeSeparators(QDir(B.files).filePath("a.mp4")); cc.extra["missing"] = false; QVERIFY(B.store.saveMedia(cc));
    QVERIFY(done2.wait(10000));
    QCOMPARE(done2.at(0).at(0).toInt(), 3);               // b, c (as stat'd), d gone
    QCOMPARE(done2.at(0).at(1).toInt(), 0);               // nothing changed: c's change was skipped
    cc = B.store.media("c-x"); QVERIFY(!cc.extra.value("missing").toBool()); QCOMPARE(cc.path, QDir::toNativeSeparators(QDir(B.files).filePath("a.mp4")));
    // One sweep at a time: a second request while one is queued/running is dropped, not stacked.
    mb.sweepMissingAsync(); mb.sweepMissingAsync(); mb.sweepMissingAsync();
    QTRY_VERIFY_WITH_TIMEOUT(mb.workerStats().value("sweep").toObject().value("done").toInt() >= 3, 10000);
    QTest::qWait(200);
    QCOMPARE(mb.workerStats().value("sweep").toObject().value("done").toInt(), 3);
}

void TestBlockingWorker::ingestDrivePollPrimesOffLoop() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ScheduleStore store; QVERIFY(store.open(dir.filePath("t.db")));
    QElapsedTimer t; t.start();
    IngestManager m(&store, dir.path(), dir.filePath("up"));
    const qint64 ctorMs = t.elapsed();
    QSignalSpy drives(&m, &IngestManager::drivesPolled);
    QVERIFY(m.removableVolumes().isEmpty());             // nothing until the first poll came back
    QVERIFY(drives.wait(10000));
    qInfo() << "ctor" << ctorMs << "ms; first drive poll" << drives.at(0).at(1).toLongLong() << "ms on the worker;" << drives.at(0).at(0).toInt() << "volume(s)";
    QVERIFY(ctorMs < 1000);
    const QJsonObject ws = m.workerStats();
    QVERIFY(ws.value("drives").toObject().value("done").toInt() >= 1);
    QCOMPARE(ws.value("volumes").toInt(), m.removableVolumes().size());
    for (const QJsonValue& v : m.removableVolumes()) { const QJsonObject o = v.toObject(); QVERIFY(o.contains("root")); QVERIFY(o.contains("hasMarker")); QVERIFY(o.contains("bytesTotal")); }
}

void TestBlockingWorker::ingestWatchWalkOffLoop() {
    const int nFiles = qEnvironmentVariableIntValue("PP_WALK_FILES") > 0 ? qEnvironmentVariableIntValue("PP_WALK_FILES") : 10000;
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString watch = dir.filePath("watch");
    QElapsedTimer t; t.start();
    for (int i = 0; i < nFiles; ++i) writeFile(QDir(watch).filePath(QString("sub%1/note%2.txt").arg(i % 50).arg(i)), "n");   // not media: walked, filtered
    QStringList media;
    for (int i = 0; i < 10; ++i) { const QString p = QDir::toNativeSeparators(QDir(watch).filePath(QString("sub%1/clip%2.mp4").arg(i).arg(i))); writeFile(p, "x"); media << p; }
    writeFile(QDir(watch).filePath("sub1/.hidden.mp4"), "x");          // dotfile: skipped
    writeFile(QDir(watch).filePath("sub1/upload.mp4.part"), "x");      // in flight: skipped
    writeFile(QDir(watch).filePath("sub2/empty.mp4"), "");             // size 0: never settles
    qInfo() << "created" << nFiles + 13 << "files in" << t.elapsed() << "ms";
    ScheduleStore store; QVERIFY(store.open(dir.filePath("t.db")));
    // Two records: one already in the library (skipped), one marked missing (picked up again).
    MediaRecord known; known.id = "known"; known.title = "clip0"; known.path = media[0]; known.durationSec = 1; QVERIFY(store.saveMedia(known));
    MediaRecord miss; miss.id = "miss"; miss.title = "clip1"; miss.path = media[1]; miss.durationSec = 1; miss.extra["missing"] = true; QVERIFY(store.saveMedia(miss));
    QJsonObject cfg; cfg["thumbnails"] = false; store.setSettingJson("ingest.config", cfg);
    IngestManager m(&store, dir.path(), dir.filePath("up"));
    QSignalSpy polled(&m, &IngestManager::watchPolled);
    LoopGauge g; g.start();
    QJsonObject w; w["path"] = QDir::toNativeSeparators(watch); w["type"] = "promo"; w["recursive"] = true; w["client"] = "ACME";
    t.restart();
    m.setWatchFolders(QJsonArray{w});                     // no stat on the loop
    QVERIFY(t.elapsed() < 50);
    // Poll 1 (timer, 3 s): every clip is seen once (pending). Poll 2 (the 4 s start-up shot, or the 6 s timer when the
    // first walk is still running at 4 s): same size -> settled -> queued.
    QVERIFY(polled.wait(15000));
    QCOMPARE(polled.at(0).at(0).toInt(), 10);             // candidates: 8 new clips + the missing one + the empty one; known and dot/part excluded
    QVERIFY(polled.wait(15000));
    g.stop();
    const qint64 walkMs = polled.at(0).at(1).toLongLong(), walk2Ms = polled.at(1).at(1).toLongLong();
    qInfo() << "walk of" << nFiles + 13 << "files:" << walkMs << "ms /" << walk2Ms << "ms on the worker; loop max gap" << g.maxGap << "ms over" << g.fires << "fires";
    QVERIFY2(g.maxGap < 100, qPrintable(QString("max gap %1 ms").arg(g.maxGap)));
    // The old rules' outcome: clips 1..9 queued as watch jobs with the folder's type and client; clip0 (known), the
    // dotfile, the .part and the empty file are not.
    QSet<QString> queued; QHash<QString, QJsonObject> byPath;
    for (const QJsonValue& v : m.jobsJson()) { const QJsonObject j = v.toObject(); if (j.value("origin").toString() == "watch") { queued.insert(j.value("path").toString()); byPath[j.value("path").toString()] = j; } }
    QSet<QString> expected; for (int i = 1; i < 10; ++i) expected.insert(media[i]);
    QCOMPARE(queued, expected);
    QCOMPARE(byPath.value(media[2]).value("type").toString(), QString("promo"));
    QCOMPARE(byPath.value(media[1]).value("mediaId").toString(), QString("miss"));   // re-probe of the missing record in place
    const QJsonObject ws = m.workerStats();
    QVERIFY(ws.value("watch").toObject().value("done").toInt() >= 2);
    QCOMPARE(ws.value("knownPaths").toInt(), 1);
    QVERIFY(ws.value("watchPending").toInt() >= 1);       // the empty file keeps waiting for a size
    // A file that appears later is picked up after it settles (two polls).
    const QString late = QDir::toNativeSeparators(QDir(watch).filePath("sub3/late.mp4")); writeFile(late, "xx");
    auto hasJob = [&m, &late]() { for (const QJsonValue& v : m.jobsJson()) if (v.toObject().value("path").toString() == late) return true; return false; };
    QTRY_VERIFY_WITH_TIMEOUT(hasJob(), 20000);
    QCOMPARE(m.workerStats().value("watch").toObject().value("refused").toInt(), 0);
}

void TestBlockingWorker::ingestUncWatchPathDoesNotBlockLoop() {
    // 192.0.2.1 is TEST-NET-1 (never routed): a stat of \\192.0.2.1\share blocks ~21 s on this box. Last test on purpose:
    // the walk worker stays blocked in that stat after the manager is destroyed (detached), and process exit ends it.
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString local = dir.filePath("watch"); writeFile(QDir(local).filePath("a.mp4"), "x");
    ScheduleStore store; QVERIFY(store.open(dir.filePath("t.db")));
    IngestManager* m = new IngestManager(&store, dir.path(), dir.filePath("up"));
    QJsonObject unc; unc["path"] = "\\\\192.0.2.1\\pp-lane-test"; unc["type"] = "program";
    QJsonObject loc; loc["path"] = QDir::toNativeSeparators(local); loc["type"] = "program";
    LoopGauge g; g.start();
    QElapsedTimer t; t.start();
    m->setWatchFolders(QJsonArray{unc, loc});
    m->rescanWatchFolders();                               // walk now
    QVERIFY(t.elapsed() < 50);
    QTest::qWait(5000);
    g.stop();
    const QJsonObject ws = m->workerStats().value("watch").toObject();
    qInfo() << "UNC watch path: loop max gap" << g.maxGap << "ms over" << g.fires << "fires; walk running for" << ws.value("runningMs").toDouble() << "ms, queued" << ws.value("queued").toInt();
    QVERIFY2(g.maxGap < 100, qPrintable(QString("max gap %1 ms").arg(g.maxGap)));
    // How long the stat blocks depends on the SMB redirector's negative cache (instant right after an earlier attempt,
    // 7-21 s cold), so the "still hung" checks only apply when the walk really is still running; the loop gap above
    // is the slice's requirement and is checked either way.
    if (ws.value("running").toBool()) {
        QVERIFY(ws.value("runningMs").toDouble() >= 4000); // still inside the UNC stat
        QCOMPARE(ws.value("queued").toInt(), 0);           // the 3 s timer's polls were skipped while the walk hung
    } else {
        qInfo() << "UNC stat returned early (redirector negative cache); hung-walk checks skipped";
    }
    // GET-style calls stay instant.
    t.restart(); m->removableVolumes(); m->watchFolders(); QVERIFY(t.elapsed() < 50);
    // Destruction: cancel, wait detachMs (2 s default), leave the thread.
    t.restart(); delete m; const qint64 dtorMs = t.elapsed();
    qInfo() << "manager destroyed with the walk hung after" << dtorMs << "ms";
    QVERIFY(dtorMs < 4000);
}

QTEST_GUILESS_MAIN(TestBlockingWorker)
#include "test_blockingworker.moc"
