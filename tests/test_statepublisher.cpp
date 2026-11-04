#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QLocalSocket>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QUuid>
#include <cstring>
#include <memory>
#include "playout/supervisor/statepublisher.h"
#include "playout/schedule/schedulestore.h"
#include "playout/schedule/runlistbuilder.h"
#include "playout/ui_ipc/ipctransport.h"

using namespace sched;
using statepub::Verdict;

// State publisher and ScheduleStore caches (perf-supervisor PS-6 / W12). The Supervisor's snapshot is built once per tick,
// hashed without its fast gauges and sent only when it changed (plus a keepalive); a status query gets the snapshot and
// only the runlists the client has not had; channels()/channel() answer from memory; setSetting no longer moves the
// schedule revision. Everything runs on temp databases and private pipe names; simulated time drives the 10 minute runs.

namespace {

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray compactArr(const QJsonArray& a) { return QJsonDocument(a).toJson(QJsonDocument::Compact); }

// --- a snapshot shaped like SupervisorCore::fullSnapshot() --------------------------------------------------------------

QJsonObject profileJson(const QString& id) {
    ChannelProfile p; p.id = id; p.name = id.toUpper(); p.callLetters = "WSTR"; p.brandName = "Star News";
    QJsonObject ndi; ndi["type"] = "ndi"; ndi["name"] = "perftest-" + id; ndi["enabled"] = true;
    QJsonObject udp; udp["type"] = "udp"; udp["url"] = "udp://127.0.0.1:50010"; udp["bitrateKbps"] = 8000; udp["enabled"] = false;
    p.outputs = QJsonArray{ndi, udp};
    p.identity = QJsonObject{{"bugAsset", "C:/media/bug.png"}, {"slateCaptions", QJsonArray{"STAR NEWS", "STAND BY"}}};
    p.fill = QJsonObject{{"fillGapsWithIds", true}, {"maxFillSec", 120}, {"emergencyTypes", QJsonArray{"filler", "promo"}}};
    p.extra = QJsonObject{{"gpu", false}, {"hwDecode", true}, {"mixes", QJsonArray{QJsonObject{{"id", "air"}, {"name", "Air"}}}}};
    return p.toJson();
}

// One channel row; `t` is the simulated time in ms, `onAir` whether an engine is up and sending telemetry.
QJsonObject channelRow(const QString& id, qint64 t, bool onAir) {
    QJsonObject o;
    o["profile"] = profileJson(id); o["exists"] = true;
    o["process"] = QJsonObject{{"running", onAir}, {"connected", onAir}, {"status", onAir ? "running" : "none"},
                               {"pid", onAir ? 4242.0 : 0.0}, {"restarts", 0}, {"lastHeartbeatMs", onAir ? double(1761460000000LL + t - t % 100) : 0.0}};
    o["executor"] = QJsonObject{{"channelId", id}, {"armed", true}, {"connected", onAir}, {"emergency", false}, {"currentSlotId", "d2026-10-26h03"},
                                {"hourStartMs", 1761462000000.0}, {"lastPushMs", onAir ? 1761462001000.0 : 0.0}, {"lastPushReason", onAir ? "engine_connected" : ""},
                                {"pushedItems", onAir ? 41 : 0}, {"engineState", onAir ? "ON_AIR" : ""}, {"onAirRunlistId", onAir ? "d2026-10-26h03#a7#3" : ""},
                                {"onAirTitle", onAir ? "Morning Report" : ""},
                                {"driftSec", onAir ? ((t / 1000) % 3 - 1) * 0.033 : 0.0}};   // frame jitter around zero
    QJsonObject eng;
    if (onAir) {
        eng = QJsonObject{{"state", "ON_AIR"}, {"itemId", "d2026-10-26h03#a7#3"}, {"title", "Morning Report"}, {"path", "C:/media/morning.mp4"},
                          {"position", double(t % 600000) / 1000.0}, {"duration", 600.0}, {"frame", double(t * 30 / 1000)}, {"fps", 29.97},
                          {"droppedFrames", 0}, {"bufferDepth", 6}, {"audioLevels", QJsonArray{-18.2, -17.9}}, {"outputs", QJsonArray{QJsonObject{{"name", "ndi"}, {"ok", true}}}}};
    }
    o["engine"] = eng;
    o["engineState"] = onAir ? "ON_AIR" : "OFFLINE";
    o["hasPreview"] = onAir;
    return o;
}

QJsonObject snapshotAt(qint64 t, bool onAir, int alerts = 20) {
    QJsonObject o;
    o["supervisor"] = true; o["uptimeMs"] = double(t); o["utcMs"] = double(1761460000000LL + t); o["ntpOffsetMs"] = 3.0;
    o["restPort"] = 7797; o["wsPort"] = 7798; o["ipcName"] = "ppsp-test"; o["dataDir"] = "C:/data/supervisor";
    o["channels"] = QJsonArray{channelRow("ch2", t, onAir), channelRow("main", t, onAir)};
    QJsonArray a;
    for (int i = 0; i < alerts; ++i) a.append(QJsonObject{{"channelId", i % 2 ? "main" : "ch2"}, {"level", "info"}, {"message", QString("engine connected #%1").arg(i)}, {"utcMs", 1761450000000.0 + i}});
    o["alerts"] = a;
    o["mesh"] = QJsonObject{{"url", "http://127.0.0.1:18081"}, {"hasToken", false}, {"hasCookie", false}, {"lastOkMs", double(1761460000000LL + t - t % 30000)},
                            {"lastError", ""}, {"nodes", 3}, {"downloadRoot", "C:/media/mesh"}, {"activeJobs", 0}};
    o["cameras"] = 2;
    o["mux"] = QJsonObject{{"state", "stopped"}, {"services", QJsonArray()}, {"muxBitrate", double((t / 1000) % 7) * 1000.0}};
    o["ingestPending"] = 0; o["cgHtml"] = QJsonObject(); o["htmlCg"] = true;
    o["health"] = QJsonObject{{"score", 100}, {"status", "healthy"}};
    return o;
}

// A runlist like ScheduleExecutor::runlistJson(): two hours of RunlistItem::toJson() plus the status.
QJsonObject runlistFor(const QString& ch, int itemsPerHour = 40) {
    auto hour = [&](qint64 start, const QString& slot) {
        Runlist r; r.channelId = ch; r.slotId = slot; r.hourStartMs = start; r.hourEndMs = start + 3600000;
        qint64 at = start;
        for (int i = 0; i < itemsPerHour; ++i) {
            RunlistItem it; it.runlistId = QString("%1#i%2#%3").arg(slot).arg(i).arg(i);
            it.item.id = QString("i%1").arg(i); it.item.mediaId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            it.item.title = QString("Program segment %1").arg(i); it.item.path = QString("D:/media/programs/segment_%1.mp4").arg(i);
            it.item.type = i % 5 == 4 ? "ad" : "program"; it.item.durationSec = 90.0;
            it.durationSec = 90.0; it.plannedStartMs = at; it.plannedEndMs = at + 90000; at += 90000; it.slotId = slot;
            r.items.append(it);
        }
        r.scheduledSec = 90.0 * itemsPerHour;
        return r.toJson();
    };
    QJsonObject o;
    o["channelId"] = ch; o["current"] = hour(1761462000000LL, "d2026-10-26h03"); o["next"] = hour(1761465600000LL, "d2026-10-26h04");
    o["status"] = QJsonObject{{"channelId", ch}, {"armed", true}};
    return o;
}

// --- the Supervisor's UI wiring, in the two shapes the IPC test compares ----------------------------------------------
//
// legacy = supervisorcore.cpp before PS-6: the 2 s tick broadcasts fullSnapshot() whenever a UI listens; every status
// query gets fullSnapshot() + every channel's telemetry + every runlist. ps6 = the same calls as SupervisorCore now makes:
// StatePublisher::tick() with broadcastUi as the sink; sendUiSnapshot() = queryReply(); broadcastUi() notes runlists.
struct Wiring {
    bool legacy = false;
    bool onAir = false;
    qint64 now = 0;
    IpcTransport server{IpcTransport::ServerRole};
    QSet<QString> ui;
    QHash<QString, QJsonObject> telemetry, runlists;
    qint64 sentBytes = 0;
    int queries = 0;
    StatePublisher pub{[this]() { return snapshotAt(now, onAir); }, [this](const QJsonObject& s) { broadcastUi(IpcProtocol::MSG_UI_STATUS, QString(), s, "snapshot"); }};

    Wiring() {
        pub.setClock([this]() { return now; });
        pub.setListenerCheck([this]() { return !ui.isEmpty(); });
        QObject::connect(&server, &IpcTransport::messageReceived, &server, [this](const IpcProtocol::Message& m, const QString& cid) {
            if (m.type == IpcProtocol::MSG_UI_QUERY_STATUS) { ++queries; ui.insert(cid); sendUiSnapshot(cid); }
        });
        QObject::connect(&server, &IpcTransport::clientDisconnected, &server, [this](const QString& cid) { ui.remove(cid); pub.forgetClient(cid); });
    }
    static qint64 framed(const IpcProtocol::Message& m) { return 4 + m.toJson().size(); }
    void sendSet(const IpcProtocol::Message& m, IpcTransport::Priority p) { const int n = server.sendTo(ui, m, p); sentBytes += n * framed(m); }
    void sendOne(const QString& cid, const IpcProtocol::Message& m) { if (server.sendTo(cid, m)) sentBytes += framed(m); }

    void broadcastUi(IpcProtocol::MessageType type, const QString& ch, const QJsonObject& payload, const QString& wsType) {
        if (ui.isEmpty()) return;
        QJsonObject p = payload; p["_type"] = wsType;
        const bool droppable = type == IpcProtocol::MSG_UI_TELEMETRY || wsType == QLatin1String("snapshot");
        sendSet(IpcProtocol::make(type, ch, p), droppable ? IpcTransport::Droppable : IpcTransport::Normal);
        if (!legacy && type == IpcProtocol::MSG_UI_RUNLIST) pub.noteRunlistBroadcast(ch, ui);
    }
    void sendUiSnapshot(const QString& cid) {
        if (legacy) {
            sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_STATUS, QString(), snapshotAt(now, onAir)));
            for (auto it = telemetry.begin(); it != telemetry.end(); ++it) sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_TELEMETRY, it.key(), it.value()));
            for (auto it = runlists.begin(); it != runlists.end(); ++it) sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_RUNLIST, it.key(), it.value()));
            return;
        }
        const StatePublisher::Reply r = pub.queryReply(cid, runlists.keys());
        sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_STATUS, QString(), r.snapshot));
        if (r.telemetry) for (auto it = telemetry.begin(); it != telemetry.end(); ++it) sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_TELEMETRY, it.key(), it.value()));
        for (const QString& ch : r.runlists) sendOne(cid, IpcProtocol::make(IpcProtocol::MSG_UI_RUNLIST, ch, runlists.value(ch)));
    }
    void snapshotTick() {
        if (legacy) { if (!ui.isEmpty()) broadcastUi(IpcProtocol::MSG_UI_STATUS, QString(), snapshotAt(now, onAir), "snapshot"); return; }
        pub.tick();
    }
};

// A UI on the other end of the pipe: counts every byte and message it reads, keeps the last snapshot.
struct FakeUi {
    QLocalSocket sock;
    QByteArray buf;
    qint64 bytes = 0;
    QHash<int, int> count;
    QHash<int, qint64> bytesByType;
    QJsonObject lastSnapshot;
    FakeUi() {
        QObject::connect(&sock, &QLocalSocket::readyRead, &sock, [this]() {
            const QByteArray in = sock.readAll(); bytes += in.size(); buf += in;
            while (buf.size() >= 4) {
                quint32 len = 0; memcpy(&len, buf.constData(), 4);
                if (buf.size() < 4 + qint64(len)) break;
                const IpcProtocol::Message m = IpcProtocol::Message::fromJson(buf.mid(4, int(len)));
                ++count[int(m.type)]; bytesByType[int(m.type)] += 4 + qint64(len);
                if (m.type == IpcProtocol::MSG_UI_STATUS && m.channelId.isEmpty()) lastSnapshot = m.payload;
                buf.remove(0, 4 + int(len));
            }
        });
    }
    void query() {
        const QByteArray data = IpcProtocol::make(IpcProtocol::MSG_UI_QUERY_STATUS, QString(), {}).toJson();
        const quint32 len = quint32(data.size());
        sock.write(reinterpret_cast<const char*>(&len), 4); sock.write(data);
    }
};

QString pipeName(const char* tag) { static int n = 0; return QString("ppsp-%1-%2-%3").arg(tag).arg(QCoreApplication::applicationPid()).arg(++n); }

struct RunResult { qint64 bytes = 0; QHash<int, int> count; QHash<int, qint64> byType; QJsonObject lastSnapshot; };

// 10 simulated minutes: time sync every second (to every client, as the Supervisor sends it), the 2 s snapshot tick,
// engine telemetry at 1 Hz per channel when on air, and optionally the app's 4 s status poll (supervisorclient.cpp).
RunResult runTenMinutes(bool legacy, bool poll, bool onAir, QString* error) {
    RunResult res;
    auto w = std::make_unique<Wiring>();
    w->legacy = legacy; w->onAir = onAir;
    w->runlists.insert("ch2", runlistFor("ch2")); w->runlists.insert("main", runlistFor("main"));
    if (onAir) { for (const QString ch : {QString("ch2"), QString("main")}) w->telemetry.insert(ch, channelRow(ch, 0, true).value("engine").toObject()); }
    const QString name = pipeName(legacy ? "legacy" : "ps6");
    if (!w->server.startServer(name)) { *error = "server did not start"; return res; }
    FakeUi ui;
    ui.sock.connectToServer(name);
    if (!ui.sock.waitForConnected(3000)) { *error = "fake UI did not connect"; return res; }
    ui.query();
    if (!QTest::qWaitFor([&]() { return w->ui.size() == 1; }, 3000)) { *error = "query not seen"; return res; }
    const QString cid = *w->ui.begin();
    int asked = 1;
    for (int sec = 1; sec <= 600; ++sec) {
        w->now = qint64(sec) * 1000;
        w->sendSet(IpcProtocol::makeTimeSync(sec * 30, w->now * 1000000LL, 0), IpcTransport::Droppable);
        if (onAir) for (auto it = w->telemetry.begin(); it != w->telemetry.end(); ++it) {
            *it = channelRow(it.key(), w->now, true).value("engine").toObject();
            w->broadcastUi(IpcProtocol::MSG_UI_TELEMETRY, it.key(), it.value(), "telemetry");
        }
        if (sec % 2) continue;
        w->snapshotTick();
        if (poll && sec % 4 == 0) {   // the app's poll, answered at this simulated time
            ui.query(); ++asked;
            if (!QTest::qWaitFor([&]() { return w->queries == asked; }, 3000)) { *error = "a poll was not answered"; return res; }
        }
        // Let the pipe move: nothing may pile up toward the 4 MB cap, or Droppable messages would be skipped.
        if (!QTest::qWaitFor([&]() { return w->server.queuedBytes(cid) == 0 && ui.bytes == w->sentBytes; }, 3000)) {
            *error = QString("pipe did not drain at %1 s (queued %2, read %3 of %4)").arg(sec).arg(w->server.queuedBytes(cid)).arg(ui.bytes).arg(w->sentBytes);
            return res;
        }
    }
    if (!QTest::qWaitFor([&]() { return ui.bytes == w->sentBytes; }, 5000)) { *error = QString("fake UI read %1 of %2 bytes").arg(ui.bytes).arg(w->sentBytes); return res; }
    res.bytes = ui.bytes; res.count = ui.count; res.byType = ui.bytesByType; res.lastSnapshot = ui.lastSnapshot;
    ui.sock.disconnectFromServer();
    w->server.disconnect();
    return res;
}

// The old ScheduleStore::channels()/channel(): one query per call on its own connection (the reference the cache must match).
struct LegacyChannels {
    QString conn;
    QSqlDatabase db;
    explicit LegacyChannels(const QString& path) : conn("legacy_" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)) {
        db = QSqlDatabase::addDatabase("QSQLITE", conn); db.setDatabaseName(path); db.open();
    }
    ~LegacyChannels() { db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(conn); }
    QList<ChannelProfile> channels() {
        QList<ChannelProfile> out; QSqlQuery q(db); q.exec("SELECT json FROM channels ORDER BY id");
        while (q.next()) out.append(ChannelProfile::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object()));
        return out;
    }
    ChannelProfile channel(const QString& id, bool* found) {
        QSqlQuery q(db); q.prepare("SELECT json FROM channels WHERE id = ?"); q.addBindValue(id);
        if (q.exec() && q.next()) { *found = true; return ChannelProfile::fromJson(QJsonDocument::fromJson(q.value(0).toByteArray()).object()); }
        *found = false; ChannelProfile c; c.id = id; c.name = id; return c;
    }
};

// What fullSnapshot() takes from the store: the channel list, each row's profile and "exists" via channel(id).
template <class Src> QJsonArray channelPart(Src& s) {
    QJsonArray a;
    for (const ChannelProfile& p : s.channels()) { bool f = false; const ChannelProfile c = s.channel(p.id, &f); a.append(QJsonObject{{"profile", c.toJson()}, {"exists", f}}); }
    bool f = true; const ChannelProfile missing = s.channel("no-such-channel", &f);
    a.append(QJsonObject{{"profile", missing.toJson()}, {"exists", f}});
    return a;
}

ChannelProfile makeChannel(const QString& id, const QString& name) {
    ChannelProfile p = ChannelProfile::fromJson(profileJson(id)); p.name = name; return p;
}

} // namespace

class TestStatePublisher : public QObject {
    Q_OBJECT
private slots:
    void stableViewIgnoresGaugesOnly();
    void decideTable();
    void idleScheduleNoChangeSends();
    void changePublishedOnNextTickUnmodified();
    void nobodyListeningNothingBuilt();
    void replySnapshotAndRunlists();
    void channelCacheMatchesSqlAndInvalidates();
    void channelCacheIsFaster();
    void settingsRevisionSplit();
    void ipcBytesIdleTenMinutes();
};

void TestStatePublisher::stableViewIgnoresGaugesOnly() {
    const QJsonObject base = snapshotAt(10000, true);
    const QByteArray h = statepub::stableHash(base);
    QCOMPARE(statepub::stableHash(base), h);                                  // deterministic
    // Gauges: clocks, heartbeats, engine telemetry, frame jitter in the drift, mux bitrate, mesh poll time.
    QCOMPARE(statepub::stableHash(snapshotAt(11000, true)), h);
    QCOMPARE(statepub::stableHash(snapshotAt(47000, true)), h);
    auto edit = [&](const std::function<void(QJsonObject&)>& f) { QJsonObject s = base; f(s); return statepub::stableHash(s); };
    auto editCh = [&](int i, const std::function<void(QJsonObject&)>& f) {
        return edit([&](QJsonObject& s) { QJsonArray a = s["channels"].toArray(); QJsonObject c = a[i].toObject(); f(c); a[i] = c; s["channels"] = a; });
    };
    auto editSub = [&](int i, const char* sub, const char* key, const QJsonValue& v) {
        return editCh(i, [&](QJsonObject& c) { QJsonObject o = c[sub].toObject(); o[key] = v; c[sub] = o; });
    };
    QCOMPARE(editSub(0, "executor", "driftSec", -0.033), h);                  // -0.033 and +0.033 round to the same 0 (no -0)
    QCOMPARE(editSub(0, "executor", "driftSec", 0.49), h);
    QCOMPARE(editSub(1, "process", "lastHeartbeatMs", 1.0), h);
    QCOMPARE(editCh(1, [](QJsonObject& c) { c["engine"] = QJsonObject{{"state", "PAUSED"}}; }), h);
    // Meaningful changes: each one changes the hash.
    QVERIFY(editSub(0, "executor", "driftSec", 1.4) != h);                    // a real drift still shows
    QVERIFY(editSub(0, "executor", "armed", false) != h);
    QVERIFY(editSub(0, "executor", "onAirTitle", "News at Noon") != h);
    QVERIFY(editSub(0, "executor", "lastPushMs", 1.0) != h);
    QVERIFY(editSub(1, "process", "status", "restarting") != h);
    QVERIFY(editSub(1, "process", "pid", 99.0) != h);
    QVERIFY(editCh(1, [](QJsonObject& c) { c["engineState"] = "IDLE"; }) != h);
    QVERIFY(editCh(1, [](QJsonObject& c) { c["hasPreview"] = false; }) != h);
    QVERIFY(editSub(0, "profile", "name", "Renamed") != h);
    QVERIFY(edit([](QJsonObject& s) { s["alerts"] = snapshotAt(0, true, 21).value("alerts"); }) != h);
    QVERIFY(edit([](QJsonObject& s) { s["ingestPending"] = 3; }) != h);
    QVERIFY(edit([](QJsonObject& s) { s["health"] = QJsonObject{{"score", 50}, {"status", "degraded"}}; }) != h);
    QVERIFY(edit([](QJsonObject& s) { QJsonObject m = s["mux"].toObject(); m["state"] = "running"; s["mux"] = m; }) != h);
    QVERIFY(edit([](QJsonObject& s) { s["ntpOffsetMs"] = 4.0; }) != h);
    QVERIFY(edit([](QJsonObject& s) { QJsonArray a = s["channels"].toArray(); a.removeLast(); s["channels"] = a; }) != h);
    // The view never edits the caller's object.
    const QByteArray before = compact(base);
    statepub::stableView(base);
    QCOMPARE(compact(base), before);
}

void TestStatePublisher::decideTable() {
    const int tick = 2000, ka = 10000;
    QCOMPARE(statepub::decide(false, false, 0, ka, tick, false), Verdict::First);
    QCOMPARE(statepub::decide(false, true, 0, ka, tick, false), Verdict::First);
    QCOMPARE(statepub::decide(true, true, 2000, ka, tick, true), Verdict::Forced);
    QCOMPARE(statepub::decide(true, false, 2000, ka, tick, false), Verdict::Changed);
    QCOMPARE(statepub::decide(true, true, 2000, ka, tick, false), Verdict::Skip);
    QCOMPARE(statepub::decide(true, true, 8000, ka, tick, false), Verdict::Skip);
    QCOMPARE(statepub::decide(true, true, 8999, ka, tick, false), Verdict::Skip);
    QCOMPARE(statepub::decide(true, true, 9000, ka, tick, false), Verdict::Keepalive);   // within half a tick of due (timer jitter)
    QCOMPARE(statepub::decide(true, true, 9990, ka, tick, false), Verdict::Keepalive);
    QCOMPARE(statepub::decide(true, true, 600000, ka, tick, false), Verdict::Keepalive);
    QCOMPARE(statepub::decide(true, true, 2000, 0, tick, false), Verdict::Keepalive);    // 0: every tick (pre-PS-6)
    QCOMPARE(statepub::decide(true, true, 2000, -1, tick, false), Verdict::Keepalive);
    QCOMPARE(QString(statepub::verdictName(Verdict::Changed)), QString("changed"));
}

void TestStatePublisher::idleScheduleNoChangeSends() {
    // An idle schedule with engines on air: only gauges move. 300 ticks = 10 minutes.
    qint64 now = 0; int published = 0;
    StatePublisher p([&]() { return snapshotAt(now, true); }, [&](const QJsonObject&) { ++published; });
    p.setClock([&]() { return now; });
    int v[5] = {};
    for (int i = 1; i <= 300; ++i) { now = i * 2000LL; ++v[int(p.tick())]; }
    QCOMPARE(v[int(Verdict::First)], 1);
    QCOMPARE(v[int(Verdict::Changed)], 0);                                    // nothing meaningful changed: no change sends
    QCOMPARE(v[int(Verdict::Keepalive)], 59);                                 // one every 10 s after the first
    QCOMPARE(v[int(Verdict::Skip)], 240);
    QCOMPARE(published, 60);                                                  // was 300 (every tick)
    QCOMPARE(p.stats().value("built").toInt(), 300);                          // built once per tick
    // Keepalive off (a day): the first and nothing else.
    published = 0; now = 0;
    StatePublisher q([&]() { return snapshotAt(now, true); }, [&](const QJsonObject&) { ++published; });
    q.setClock([&]() { return now; }); q.setKeepaliveMs(24 * 3600 * 1000);
    for (int i = 1; i <= 300; ++i) { now = i * 2000LL; q.tick(); }
    QCOMPARE(published, 1);
    // Keepalive 0 restores the old cadence: every tick.
    published = 0; now = 0;
    StatePublisher r([&]() { return snapshotAt(now, true); }, [&](const QJsonObject&) { ++published; });
    r.setClock([&]() { return now; }); r.setKeepaliveMs(0);
    for (int i = 1; i <= 300; ++i) { now = i * 2000LL; r.tick(); }
    QCOMPARE(published, 300);
}

void TestStatePublisher::changePublishedOnNextTickUnmodified() {
    qint64 now = 0; bool armed = true;
    QJsonObject built, sent; int published = 0;
    StatePublisher p([&]() {
        QJsonObject s = snapshotAt(now, true);
        QJsonArray a = s["channels"].toArray(); QJsonObject c = a[0].toObject(); QJsonObject e = c["executor"].toObject();
        e["armed"] = armed; c["executor"] = e; a[0] = c; s["channels"] = a;
        built = s; return s;
    }, [&](const QJsonObject& s) { sent = s; ++published; });
    p.setClock([&]() { return now; });
    now = 2000; QCOMPARE(p.tick(), Verdict::First);
    QCOMPARE(compact(sent), compact(built));                                  // exactly the builder's object, gauges included
    now = 4000; QCOMPARE(p.tick(), Verdict::Skip);
    armed = false;
    now = 6000; QCOMPARE(p.tick(), Verdict::Changed);                         // the operator's disarm goes out on the next tick
    QCOMPARE(compact(sent), compact(built));
    QCOMPARE(compact(p.lastPublished()), compact(built));
    now = 8000; QCOMPARE(p.tick(), Verdict::Skip);
    p.forcePublish();
    now = 10000; QCOMPARE(p.tick(), Verdict::Forced);
    now = 12000; QCOMPARE(p.tick(), Verdict::Skip);
    QCOMPARE(published, 3);
    // The keepalive counts from the last publish of any kind.
    now = 19000; QCOMPARE(p.tick(), Verdict::Keepalive);
}

void TestStatePublisher::nobodyListeningNothingBuilt() {
    int builds = 0, published = 0; bool listening = false; qint64 now = 0;
    StatePublisher p([&]() { ++builds; return snapshotAt(now, false); }, [&](const QJsonObject&) { ++published; });
    p.setClock([&]() { return now; });
    p.setListenerCheck([&]() { return listening; });
    for (int i = 1; i <= 10; ++i) { now = i * 2000LL; QCOMPARE(p.tick(), Verdict::Skip); }
    QCOMPARE(builds, 0); QCOMPARE(published, 0);
    listening = true;
    now = 22000; QCOMPARE(p.tick(), Verdict::First);
    listening = false;
    for (int i = 12; i <= 40; ++i) { now = i * 2000LL; p.tick(); }
    QCOMPARE(builds, 1);
    listening = true;                                                          // back after a long gap: the keepalive is long due
    now = 82000; QCOMPARE(p.tick(), Verdict::Keepalive);
    QCOMPARE(p.stats().value("idleTicks").toInt(), 39);
}

void TestStatePublisher::replySnapshotAndRunlists() {
    qint64 now = 2000; QString title = "Morning Report";
    StatePublisher p([&]() { QJsonObject s = snapshotAt(now, true); s["probe"] = title; return s; }, [](const QJsonObject&) {});
    p.setClock([&]() { return now; });
    QCOMPARE(p.tick(), Verdict::First);
    const QByteArray published = compact(p.lastPublished());
    // First query of a client: a fresh build (its gauges are newer than the published copy).
    now = 3000;
    StatePublisher::Reply r = p.queryReply("ui-1", {"ch2", "main"});
    QVERIFY(r.telemetry);
    QCOMPARE(r.runlists, (QStringList{"ch2", "main"}));                         // every runlist on the first query
    QCOMPARE(r.snapshot.value("uptimeMs").toDouble(), 3000.0);
    // Repeated query, nothing meaningful changed: the published object itself, byte for byte (the app sees "same").
    now = 5000;
    r = p.queryReply("ui-1", {"ch2", "main"});
    QVERIFY(!r.telemetry);
    QVERIFY(r.runlists.isEmpty());
    QCOMPARE(compact(r.snapshot), published);
    // A meaningful change since the publish: the fresh one.
    title = "News at Noon"; now = 6000;
    r = p.queryReply("ui-1", {"ch2", "main"});
    QCOMPARE(r.snapshot.value("probe").toString(), QString("News at Noon"));
    // Runlists: a broadcast that reached ui-1 is not sent again; one ui-1 missed (it was not a recipient) is.
    QVERIFY(p.noteClient("ui-2"));
    QVERIFY(!p.noteClient("ui-2"));
    QCOMPARE(p.runlistsDue("ui-2", {"ch2", "main"}), (QStringList{"ch2", "main"}));
    p.noteRunlistBroadcast("main", {"ui-1", "ui-2"});
    p.noteRunlistBroadcast("ch2", {"ui-2"});
    QVERIFY(p.runlistsDue("ui-2", {"ch2", "main"}).isEmpty());
    QCOMPARE(p.runlistsDue("ui-1", {"ch2", "main"}), QStringList{"ch2"});
    QVERIFY(p.runlistsDue("ui-1", {"ch2", "main"}).isEmpty());
    QCOMPARE(p.runlistsDue("ui-1", {"ch2", "main", "ch3"}), QStringList{"ch3"});   // a channel added later
    // A client that left and came back (same id) starts over.
    p.forgetClient("ui-1");
    r = p.queryReply("ui-1", {"ch2", "main"});
    QVERIFY(r.telemetry);
    QCOMPARE(r.runlists, (QStringList{"ch2", "main"}));
    QCOMPARE(p.stats().value("clients").toInt(), 2);
}

void TestStatePublisher::channelCacheMatchesSqlAndInvalidates() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("t.db");
    ScheduleStore store; QVERIFY(store.open(path));
    QVERIFY(store.channels().isEmpty());
    QVERIFY(store.saveChannel(makeChannel("main", "Main")));
    QVERIFY(store.saveChannel(makeChannel("ch2", "Channel 2")));
    QVERIFY(store.saveChannel(makeChannel("Zed", "Upper-case id")));        // binary collation: "Zed" sorts before "ch2"
    LegacyChannels legacy(path);
    auto same = [&]() { return compactArr(channelPart(store)) == compactArr(channelPart(legacy)); };
    QVERIFY(same());                                                           // the snapshot's channel part is byte-identical
    QStringList ids; for (const ChannelProfile& p : store.channels()) ids << p.id;
    QCOMPARE(ids, (QStringList{"Zed", "ch2", "main"}));
    for (int i = 0; i < 3; ++i) QVERIFY(same());                               // served from memory, still identical

    // save -> the next read sees it
    ChannelProfile c = store.channel("ch2"); c.name = "Renamed"; c.outputs = QJsonArray{QJsonObject{{"type", "null"}}}; c.armed = false;
    QVERIFY(store.saveChannel(c));
    QCOMPARE(store.channel("ch2").name, QString("Renamed"));
    QCOMPARE(store.channel("ch2").armed, false);
    QCOMPARE(store.channels().at(1).name, QString("Renamed"));
    QVERIFY(same());
    // new channel
    QVERIFY(store.saveChannel(makeChannel("ch3", "Three")));
    QCOMPARE(store.channels().size(), 4);
    QVERIFY(same());
    // delete -> gone, and channel() answers "not found" with today's default
    QVERIFY(store.deleteChannel("main"));
    bool found = true; const ChannelProfile gone = store.channel("main", &found);
    QVERIFY(!found); QCOMPARE(gone.id, QString("main")); QCOMPARE(gone.name, QString("main"));
    QCOMPARE(store.channels().size(), 3);
    QVERIFY(same());
    // a copy a caller edits never reaches the cache
    QList<ChannelProfile> l = store.channels(); l[0].name = "edited by caller";
    QCOMPARE(store.channels().at(0).name, QString("Upper-case id"));
    // a second store on the same file (a restart) reads the same
    store.close();
    QVERIFY(store.channels().isEmpty());                                       // closed: nothing, as before
    ScheduleStore again; QVERIFY(again.open(path));
    QCOMPARE(compactArr(channelPart(again)), compactArr(channelPart(legacy)));
    QVERIFY(store.open(path));
    QCOMPARE(store.channels().size(), 3);
}

void TestStatePublisher::channelCacheIsFaster() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("t.db");
    ScheduleStore store; QVERIFY(store.open(path));
    for (const QString id : {QString("ch2"), QString("main"), QString("ch3")}) QVERIFY(store.saveChannel(makeChannel(id, id)));
    LegacyChannels legacy(path);
    const int rounds = 2000;                                                   // = 2000 snapshots' worth of store reads
    QElapsedTimer t; t.start();
    int n = 0;
    for (int i = 0; i < rounds; ++i) n += channelPart(legacy).size();
    const qint64 sqlUs = t.nsecsElapsed() / 1000;
    t.restart();
    for (int i = 0; i < rounds; ++i) n += channelPart(store).size();
    const qint64 cacheUs = t.nsecsElapsed() / 1000;
    qInfo().noquote() << QString("store reads per snapshot (3 channels): SQL %1 us, cache %2 us (%3x)")
                             .arg(double(sqlUs) / rounds, 0, 'f', 1).arg(double(cacheUs) / rounds, 0, 'f', 1).arg(double(sqlUs) / qMax<qint64>(1, cacheUs), 0, 'f', 1);
    QCOMPARE(n, 2 * rounds * 4);
    QVERIFY2(cacheUs * 2 < sqlUs, "the cached reads should be well under half the per-call SQL cost");
}

void TestStatePublisher::settingsRevisionSplit() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    ScheduleStore store; QVERIFY(store.open(dir.filePath("t.db")));
    const quint64 rev0 = store.revision(), sch0 = store.scheduleRevision(), set0 = store.settingsRevision();
    QVERIFY(store.setSetting("overlays.state", "{}"));
    QVERIFY(store.setSettingJson("programming.recipes", QJsonObject{{"recipes", QJsonArray()}}));
    for (int i = 0; i < 50; ++i) QVERIFY(store.setSetting("agents.tasks", QString::number(i)));   // a module persisting state
    QCOMPARE(store.scheduleRevision(), sch0);                                  // settings never move the schedule revision
    QCOMPARE(store.settingsRevision(), set0 + 52);
    QCOMPARE(store.revision(), rev0 + 52);                                     // the combined counter keeps today's meaning
    QCOMPARE(store.settingsRevision(QStringList{"programming."}), set0 + 2);
    QCOMPARE(store.settingsRevision(QStringList{"traffic."}), quint64(0));    // never written since open
    QCOMPARE(store.settingsRevision(QStringList{"traffic.", "programming.recipes"}), set0 + 2);
    QCOMPARE(store.settingsRevision(QStringList{"agents."}), set0 + 52);
    // schedule writes move both counters, settings stay
    ChannelProfile c = makeChannel("main", "Main");
    QVERIFY(store.saveChannel(c));
    HourSlot s; s.channelId = "main"; s.dayOfWeek = 1; s.hour = 9;
    QVERIFY(store.saveSlot(s));
    QVERIFY(store.deleteSlot("main", s.isDated() ? HourSlot::dateId(s.date, s.hour) : HourSlot::templateId(s.dayOfWeek, s.hour)));
    QVERIFY(store.deleteChannel("main"));
    QCOMPARE(store.scheduleRevision(), sch0 + 4);
    QCOMPARE(store.settingsRevision(), set0 + 52);
    QCOMPARE(store.revision(), rev0 + 56);
    // media writes move neither (unchanged)
    MediaRecord m; m.title = "x"; m.path = "C:/x.mp4"; QVERIFY(store.saveMedia(m));
    QCOMPARE(store.revision(), rev0 + 56);
}

void TestStatePublisher::ipcBytesIdleTenMinutes() {
    struct Case { const char* name; bool poll; bool onAir; double minDrop; };
    // registered once = a UI that asks on connect only (perf-app PA-4); polling = today's app (a query every 4 s).
    const Case cases[] = {{"idle, UI registered once", false, false, 0.70}, {"idle, UI polling every 4 s", true, false, 0.80},
                          {"engines on air (1 Hz telemetry), UI polling every 4 s", true, true, 0.50}};
    for (const Case& c : cases) {
        QString err;
        const RunResult before = runTenMinutes(true, c.poll, c.onAir, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const RunResult after = runTenMinutes(false, c.poll, c.onAir, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        const double drop = 1.0 - double(after.bytes) / double(before.bytes);
        const double snapDrop = 1.0 - double(after.byType.value(IpcProtocol::MSG_UI_STATUS)) / double(before.byType.value(IpcProtocol::MSG_UI_STATUS));
        qInfo().noquote() << QString("   snapshot bytes alone: drop %1 %").arg(snapDrop * 100.0, 0, 'f', 1);
        if (!c.poll) QVERIFY(snapDrop >= 0.75);              // 2 s ticks -> 10 s keepalive (a polling UI still gets a reply per poll)
        auto line = [](const RunResult& r) {
            return QString("%1 KB/min (snapshots %2 = %3 KB, runlists %4 = %5 KB, telemetry %6, time sync %7)")
                .arg(r.bytes / 10.0 / 1024.0, 0, 'f', 1)
                .arg(r.count.value(IpcProtocol::MSG_UI_STATUS)).arg(r.byType.value(IpcProtocol::MSG_UI_STATUS) / 1024.0, 0, 'f', 0)
                .arg(r.count.value(IpcProtocol::MSG_UI_RUNLIST)).arg(r.byType.value(IpcProtocol::MSG_UI_RUNLIST) / 1024.0, 0, 'f', 0)
                .arg(r.count.value(IpcProtocol::MSG_UI_TELEMETRY)).arg(r.count.value(IpcProtocol::MSG_TIME_SYNC));
        };
        qInfo().noquote() << QString("%1, 10 simulated minutes:\n   before PS-6: %2\n   PS-6:        %3\n   drop %4 %")
                                 .arg(c.name, line(before), line(after)).arg(drop * 100.0, 0, 'f', 1);
        QVERIFY2(drop >= c.minDrop, qPrintable(QString("%1: drop %2 % < %3 %").arg(c.name).arg(drop * 100.0, 0, 'f', 1).arg(c.minDrop * 100.0)));
        // Same content: the UI's last snapshot is equivalent under both wirings (same stable view) ...
        QCOMPARE(statepub::stableHash(after.lastSnapshot), statepub::stableHash(before.lastSnapshot));
        // ... and it got every runlist once (first query) and every time sync in both.
        QCOMPARE(after.count.value(IpcProtocol::MSG_UI_RUNLIST), 2);
        QCOMPARE(after.count.value(IpcProtocol::MSG_TIME_SYNC), before.count.value(IpcProtocol::MSG_TIME_SYNC));
        QCOMPARE(after.count.value(IpcProtocol::MSG_UI_TELEMETRY), before.count.value(IpcProtocol::MSG_UI_TELEMETRY) - (c.onAir && c.poll ? 2 * 150 : 0));
    }
}

QTEST_GUILESS_MAIN(TestStatePublisher)
#include "test_statepublisher.moc"
