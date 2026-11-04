#include <QtTest/QtTest>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QSemaphore>
#include <QThread>
#include <atomic>
#include "playout/supervisor/websocketbroker.h"

// WebSocketBroker (perf-supervisor PS-10 / W16): one serialization per broadcast, low-rate clients get telemetry /
// snapshot at most once per interval (latest wins), and a client that stops reading is capped and then disconnected
// while every other client keeps receiving. Everything listens on 127.0.0.1 with an ephemeral port.

namespace {

// A dashboard-like client on the test thread: records every message it gets.
class Rx : public QObject {
public:
    QWebSocket ws;
    QList<QJsonObject> got;
    QString id;
    Rx() {
        connect(&ws, &QWebSocket::textMessageReceived, this, [this](const QString& t) {
            const QJsonObject o = QJsonDocument::fromJson(t.toUtf8()).object();
            if (o.value("type").toString() == "connected") id = o.value("clientId").toString();
            got.append(o);
        });
    }
    QList<QJsonObject> of(const QString& type, const QString& ch = QString()) const {
        QList<QJsonObject> l;
        for (const QJsonObject& o : got) if (o.value("type").toString() == type && (ch.isEmpty() || o.value("channelId").toString() == ch)) l << o;
        return l;
    }
    int count(const QString& type, const QString& ch = QString()) const { return of(type, ch).size(); }
};

// A client on its own thread that we park: its socket stops reading, so the server's queue for it grows.
class Parked : public QObject {
public:
    QWebSocket* ws = nullptr;
    QSemaphore gate;
    std::atomic<bool> up{false};
    void open(const QUrl& url) {
        ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(ws, &QWebSocket::connected, this, [this]() { up = true; });
        ws->open(url);
    }
};

QJsonObject msg(const QString& type, const QString& ch = QString(), int frame = -1) {
    QJsonObject o; o["type"] = type;
    if (!ch.isEmpty()) o["channelId"] = ch;
    if (frame >= 0) o["frame"] = frame;
    return o;
}

QJsonObject clientStats(const WebSocketBroker& b, const QString& id) {
    for (const QJsonValue& v : b.stats().value("clients").toArray()) if (v.toObject().value("clientId").toString() == id) return v.toObject();
    return {};
}

} // namespace

class TestWsBroker : public QObject {
    Q_OBJECT

    QUrl urlFor(const WebSocketBroker& b) const { return QUrl(QString("ws://127.0.0.1:%1/").arg(b.port())); }

    // Opens n clients and waits until each has its welcome (so the server lists them all).
    QList<Rx*> openClients(WebSocketBroker& b, int n) {
        QList<Rx*> l;
        for (int i = 0; i < n; ++i) { auto* r = new Rx; r->setParent(this); r->ws.open(urlFor(b)); l << r; }
        for (Rx* r : l) if (!QTest::qWaitFor([r]() { return !r->id.isEmpty(); }, 5000)) return {};
        return l;
    }
    void closeClients(QList<Rx*>& l) { for (Rx* r : l) { r->ws.abort(); delete r; } l.clear(); }

private slots:
    void lanPassword();
    void decidePolicy();
    void fanOutSameText();
    void lowClientsThrottled();
    void firstSnapshotNeverHeld();
    void sendToReachesOnlyThatClient();
    void stalledClientCappedOthersKeepReceiving();
    void broadcastFromWorkerThread();
};

void TestWsBroker::lanPassword() {
    QHostAddress local;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto& a : iface.addressEntries()) if (a.ip().protocol() == QAbstractSocket::IPv4Protocol) { local = a.ip(); break; }
        if (!local.isNull()) break;
    }
    if (local.isNull()) QSKIP("No LAN IPv4 interface available");
    WebSocketBroker b; QVERIFY(b.start(0, QHostAddress::AnyIPv4)); b.setLanPassword("test-password");
    Rx localClient; localClient.ws.open(urlFor(b)); QTRY_VERIFY_WITH_TIMEOUT(!localClient.id.isEmpty(), 5000);
    Rx denied; denied.ws.open(QUrl(QString("ws://%1:%2/?token=wrong").arg(local.toString()).arg(b.port())));
    QTest::qWait(350); QVERIFY(denied.id.isEmpty());
    Rx allowed; allowed.ws.open(QUrl(QString("ws://%1:%2/?token=test-password").arg(local.toString()).arg(b.port())));
    QTRY_VERIFY_WITH_TIMEOUT(!allowed.id.isEmpty(), 5000);
    b.setLanPassword("another-password");
    QTRY_VERIFY_WITH_TIMEOUT(allowed.ws.state() == QAbstractSocket::UnconnectedState, 5000);
    QVERIFY(localClient.ws.state() == QAbstractSocket::ConnectedState);
}

void TestWsBroker::decidePolicy() {
    using wsbroker::Send;
    const qint64 soft = 1000, hard = 8000;
    QCOMPARE(wsbroker::decide(0, true, soft, hard), Send::Now);
    QCOMPARE(wsbroker::decide(999, true, soft, hard), Send::Now);
    QCOMPARE(wsbroker::decide(1000, true, soft, hard), Send::Skip);      // droppable state over the soft cap
    QCOMPARE(wsbroker::decide(1000, false, soft, hard), Send::Now);      // events still queue
    QCOMPARE(wsbroker::decide(7999, false, soft, hard), Send::Now);
    QCOMPARE(wsbroker::decide(8000, false, soft, hard), Send::Drop);     // stalled: disconnect
    QCOMPARE(wsbroker::decide(8000, true, soft, hard), Send::Drop);
    QCOMPARE(wsbroker::decide(1 << 30, true, 0, 0), Send::Now);          // caps off
    QVERIFY(WebSocketBroker::isDroppable("telemetry") && WebSocketBroker::isDroppable("snapshot") && WebSocketBroker::isDroppable("heartbeat"));
    QVERIFY(!WebSocketBroker::isDroppable("alert") && !WebSocketBroker::isDroppable("asrun") && !WebSocketBroker::isDroppable("runlist"));
}

void TestWsBroker::fanOutSameText() {
    WebSocketBroker b;
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QList<Rx*> rx = openClients(b, 5);
    QCOMPARE(rx.size(), 5);
    QCOMPARE(b.clientCount(), 5);
    QJsonObject a = msg("alert", "main"); a["message"] = QString::fromUtf8("caf\xc3\xa9 \xe2\x9c\x93 on air");   // non-ASCII survives the one conversion
    b.broadcast(a);
    b.broadcastToChannel("ch2", msg("runlist"));
    QVERIFY(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("alert") < 1 || r->count("runlist") < 1) return false; return true; }, 5000));
    for (Rx* r : rx) {
        QCOMPARE(r->of("alert").first().value("message").toString(), a.value("message").toString());
        QCOMPARE(r->of("runlist").first().value("_channelId").toString(), QString("ch2"));
    }
    closeClients(rx);
    QVERIFY(QTest::qWaitFor([&]() { return b.clientCount() == 0; }, 5000));
}

void TestWsBroker::lowClientsThrottled() {
    WebSocketBroker b;
    b.setLowRateInterval("telemetry", 300);
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QList<Rx*> rx = openClients(b, 4);
    QCOMPARE(rx.size(), 4);
    Rx* high = rx[0];
    high->ws.sendTextMessage(R"({"type":"subscribe","rate":"high"})");
    QVERIFY(QTest::qWaitFor([&]() { return clientStats(b, high->id).value("rate").toString() == "high"; }, 5000));

    // A burst: 10 telemetry per channel in one go, plus events that are never throttled.
    for (int f = 1; f <= 10; ++f) { b.broadcast(msg("telemetry", "a", f)); b.broadcast(msg("telemetry", "b", f)); }
    for (int i = 0; i < 5; ++i) b.broadcast(msg("alert", "a"));

    QVERIFY(QTest::qWaitFor([&]() { return high->count("telemetry") == 20; }, 5000));
    // Low clients: the first of each channel at once, then the newest one when the interval ends; nothing in between.
    QVERIFY(QTest::qWaitFor([&]() { for (int i = 1; i < 4; ++i) if (rx[i]->count("telemetry") < 4) return false; return true; }, 3000));
    QTest::qWait(400);                         // longer than the interval: nothing else may trickle in
    for (int i = 1; i < 4; ++i) {
        for (const QString ch : {QString("a"), QString("b")}) {
            const QList<QJsonObject> l = rx[i]->of("telemetry", ch);
            QCOMPARE(l.size(), 2);
            QCOMPARE(l[0].value("frame").toInt(), 1);
            QCOMPARE(l[1].value("frame").toInt(), 10);        // latest wins
        }
        QCOMPARE(rx[i]->count("alert"), 5);
    }
    QCOMPARE(high->count("alert"), 5);
    { const QList<QJsonObject> l = high->of("telemetry", "a"); for (int f = 0; f < 10; ++f) QCOMPARE(l[f].value("frame").toInt(), f + 1); }
    QCOMPARE(b.stats().value("coalesced").toInt(), 16);      // 8 replaced per channel

    // Once the interval has passed, the next one goes out straight away.
    b.broadcast(msg("telemetry", "a", 11));
    QVERIFY(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("telemetry", "a") < (r == high ? 11 : 3)) return false; return true; }, 250));
    closeClients(rx);
}

void TestWsBroker::firstSnapshotNeverHeld() {
    WebSocketBroker b;
    b.setLowRateInterval("snapshot", 2500);    // long enough that the second connect always lands inside it
    // As the Supervisor did before PS-6 (every connect broadcast a snapshot to everyone; now see sendToReachesOnlyThatClient).
    int n = 0;
    connect(&b, &WebSocketBroker::clientConnected, &b, [&]() { QJsonObject s = msg("snapshot"); s["seq"] = ++n; b.broadcast(s); });
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QList<Rx*> first = openClients(b, 1);
    QCOMPARE(first.size(), 1);
    Rx* a = first[0];
    QVERIFY(QTest::qWaitFor([&]() { return a->count("snapshot") == 1; }, 3000));
    QList<Rx*> second = openClients(b, 1);
    QCOMPARE(second.size(), 1);
    Rx* c = second[0];
    QVERIFY(QTest::qWaitFor([&]() { return c->count("snapshot") == 1; }, 3000));   // the newcomer's first: not held
    QCOMPARE(c->of("snapshot").first().value("seq").toInt(), 2);
    QCOMPARE(a->count("snapshot"), 1);                                            // the existing low client: held
    QVERIFY(QTest::qWaitFor([&]() { return a->count("snapshot") == 2; }, 6000));                        // flushed at the interval's end
    QCOMPARE(a->of("snapshot").last().value("seq").toInt(), 2);
    closeClients(first); closeClients(second);
}

void TestWsBroker::sendToReachesOnlyThatClient() {
    // PS-6: the Supervisor sends the connect snapshot to the newcomer only (it used to broadcast one to every client).
    WebSocketBroker b;
    int n = 0;
    connect(&b, &WebSocketBroker::clientConnected, &b, [&](const QString& cid) { QJsonObject s = msg("snapshot"); s["seq"] = ++n; QVERIFY(b.sendTo(cid, s)); });
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QList<Rx*> rx = openClients(b, 3);
    QCOMPARE(rx.size(), 3);
    QVERIFY(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("snapshot") < 1) return false; return true; }, 3000));
    QTest::qWait(200);
    QSet<int> seqs;
    for (Rx* r : rx) { QCOMPARE(r->count("snapshot"), 1); seqs.insert(r->of("snapshot").first().value("seq").toInt()); }
    QCOMPARE(seqs, (QSet<int>{1, 2, 3}));                    // each got its own, nobody got another's
    QVERIFY(!b.sendTo("client_nope", msg("snapshot")));
    // The next broadcast snapshot still reaches every client (the low-rate fold was not touched by the direct sends).
    b.broadcast(msg("snapshot"));
    QVERIFY(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("snapshot") < 2) return false; return true; }, 3000));
    closeClients(rx);
}

void TestWsBroker::stalledClientCappedOthersKeepReceiving() {
    const qint64 soft = 64 * 1024, hard = 1024 * 1024, chunk = 32 * 1024;
    WebSocketBroker b;
    b.setPendingCaps(soft, hard);
    b.setLowRateInterval("telemetry", 0);       // every state update is attempted, so the soft cap is what skips them
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QString parkedId, droppedId;
    connect(&b, &WebSocketBroker::clientConnected, this, [&](const QString& id) { if (parkedId.isEmpty()) parkedId = id; });
    connect(&b, &WebSocketBroker::clientDisconnected, this, [&](const QString& id) { if (droppedId.isEmpty()) droppedId = id; });

    QThread t;
    auto* parked = new Parked;
    parked->moveToThread(&t);
    t.start();
    const QUrl url = urlFor(b);
    QMetaObject::invokeMethod(parked, [parked, url]() { parked->open(url); });
    QVERIFY(QTest::qWaitFor([&]() { return parked->up.load() && b.clientCount() == 1; }, 5000));
    QMetaObject::invokeMethod(parked, [parked]() { parked->gate.acquire(); });   // park its thread: no more reads

    QList<Rx*> rx = openClients(b, 19);
    QCOMPARE(rx.size(), 19);
    QCOMPARE(b.clientCount(), 20);

    QJsonObject big = msg("alert", "main"); big["message"] = QString(int(chunk), QChar('x'));
    const int perRound = 8;                     // 256 KB a round: healthy clients drain between rounds
    int rounds = 0;
    qint64 maxQueued = 0, skipped = 0;
    QElapsedTimer clock; clock.start();
    while (droppedId.isEmpty() && rounds < 2000 && clock.elapsed() < 45000) {
        ++rounds;
        b.broadcast(msg("telemetry", "main", rounds));
        for (int i = 0; i < perRound; ++i) b.broadcast(big);
        const QJsonObject ps = clientStats(b, parkedId);
        if (!ps.isEmpty()) { maxQueued = qMax(maxQueued, qint64(ps.value("queuedBytes").toDouble())); skipped = qMax(skipped, qint64(ps.value("skipped").toDouble())); }
        const int want = rounds * perRound;
        QVERIFY2(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("alert") < want) return false; return true; }, 10000),
                 qPrintable(QString("healthy clients fell behind in round %1").arg(rounds)));
    }
    qInfo().noquote() << QString("parked client dropped after %1 rounds (%2 MB sent to each client), peak queue %3 KB, %4 state updates skipped")
                             .arg(rounds).arg(double(rounds) * perRound * chunk / 1048576.0, 0, 'f', 1).arg(maxQueued / 1024).arg(skipped);
    QCOMPARE(droppedId, parkedId);                                   // the stalled client, and only it, was cut
    QVERIFY2(maxQueued <= hard + 2 * chunk, qPrintable(QString("queue reached %1 bytes").arg(maxQueued)));   // memory bounded
    QVERIFY(skipped > 0);                                            // state updates skipped above the soft cap first
    QCOMPARE(b.clientCount(), 19);
    QCOMPARE(b.stats().value("droppedSlowClients").toInt(), 1);
    for (Rx* r : rx) {                                               // everyone else got every message
        QCOMPARE(r->count("alert"), rounds * perRound);
        QCOMPARE(r->count("telemetry"), rounds);
        QCOMPARE(r->ws.state(), QAbstractSocket::ConnectedState);
    }
    // Still serving: a new broadcast reaches all 19.
    b.broadcast(msg("asrun", "main"));
    QVERIFY(QTest::qWaitFor([&]() { for (Rx* r : rx) if (r->count("asrun") < 1) return false; return true; }, 5000));

    parked->gate.release();
    QMetaObject::invokeMethod(parked, [parked]() { delete parked; }, Qt::BlockingQueuedConnection);
    t.quit(); t.wait();
    closeClients(rx);
}

void TestWsBroker::broadcastFromWorkerThread() {
    WebSocketBroker b;
    QVERIFY(b.start(0, QHostAddress::LocalHost));
    QList<Rx*> rx = openClients(b, 2);
    QCOMPARE(rx.size(), 2);
    QThread* w = QThread::create([&b]() { b.broadcast(msg("alert", "main")); });   // marshalled onto the broker's thread
    w->start(); w->wait(); delete w;
    QVERIFY(QTest::qWaitFor([&]() { return rx[0]->count("alert") == 1 && rx[1]->count("alert") == 1; }, 5000));
    closeClients(rx);
}

QTEST_GUILESS_MAIN(TestWsBroker)
#include "test_wsbroker.moc"
