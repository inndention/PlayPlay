#include <QtTest/QtTest>
#include <QJsonArray>
#include <QSignalSpy>
#include "playout/ui_ipc/ipcprotocol.h"
#include "playout/ui_ipc/ipctransport.h"

Q_DECLARE_METATYPE(IpcProtocol::Message)

// IPC protocol round-trip tests: every builder must survive toJson/fromJson
// with type, channelId, timestamp and payload intact.
class TestIpcProtocol : public QObject {
    Q_OBJECT

private slots:
    void transportCommandRoundTrip();
    void playlistPushRoundTrip();
    void heartbeatRoundTrip();
    void asRunRoundTrip();
    void transportFramingRoundTrip();
};

void TestIpcProtocol::transportCommandRoundTrip() {
    QJsonObject params; params["seconds"] = 12.5;
    auto msg = IpcProtocol::makeTransportCommand("ch1", "seek", params);
    auto back = IpcProtocol::Message::fromJson(msg.toJson());
    QCOMPARE(back.type, IpcProtocol::MSG_TRANSPORT_COMMAND);
    QCOMPARE(back.channelId, QString("ch1"));
    QCOMPARE(back.payload.value("action").toString(), QString("seek"));
    QCOMPARE(back.payload.value("params").toObject().value("seconds").toDouble(), 12.5);
}

void TestIpcProtocol::playlistPushRoundTrip() {
    QJsonArray list;
    QJsonObject a; a["assetId"] = "A"; a["path"] = "C:/media/a.mxf"; a["duration"] = 30.0;
    QJsonObject b; b["assetId"] = "B"; b["path"] = "C:/media/b.mxf"; b["duration"] = 60.0;
    list << a << b;
    auto msg = IpcProtocol::makePlaylistPush("ch1", list);
    auto back = IpcProtocol::Message::fromJson(msg.toJson());
    QCOMPARE(back.type, IpcProtocol::MSG_PLAYLIST_PUSH);
    QCOMPARE(back.payload.value("playlist").toArray().size(), 2);
    QCOMPARE(back.payload.value("playlist").toArray()[1].toObject().value("assetId").toString(), QString("B"));
}

void TestIpcProtocol::heartbeatRoundTrip() {
    auto msg = IpcProtocol::makeHeartbeat("ch1", 123456789LL, 4, true);
    auto back = IpcProtocol::Message::fromJson(msg.toJson());
    QCOMPARE(back.type, IpcProtocol::MSG_HEARTBEAT);
    QCOMPARE(static_cast<qint64>(back.payload.value("frameNumber").toDouble()), 123456789LL);
    QCOMPARE(back.payload.value("isHealthy").toBool(), true);
    QVERIFY(back.timestampNs > 0);
}

void TestIpcProtocol::asRunRoundTrip() {
    auto msg = IpcProtocol::makeAsRunLog("ch1", "START", "asset-42", 99LL);
    auto back = IpcProtocol::Message::fromJson(msg.toJson());
    QCOMPARE(back.type, IpcProtocol::MSG_ASRUN_LOG);
    QCOMPARE(back.payload.value("eventType").toString(), QString("START"));
    QCOMPARE(back.payload.value("assetId").toString(), QString("asset-42"));
}

// The same messages through the real pipe transport (4-byte length framing), with the engine's blocking connect on
// one side and the Supervisor's server on the other, plus a 2 MB playlist: framing survives the PS-1 transport changes.
void TestIpcProtocol::transportFramingRoundTrip() {
    const QString name = QString("ppproto-%1").arg(QCoreApplication::applicationPid());
    IpcTransport server(IpcTransport::ServerRole);
    QVERIFY(server.startServer(name));
    QSignalSpy conn(&server, &IpcTransport::clientConnected), srv(&server, &IpcTransport::messageReceived);
    IpcTransport engine(IpcTransport::ClientRole);
    QSignalSpy eng(&engine, &IpcTransport::messageReceived);
    QVERIFY(engine.connectToServer(name));
    QTRY_COMPARE_WITH_TIMEOUT(conn.count(), 1, 3000);
    const QString id = conn.at(0).at(0).toString();

    QJsonArray big;
    for (int i = 0; i < 64; ++i) { QJsonObject it; it["assetId"] = QString("A%1").arg(i); it["path"] = QString(32 * 1024, QLatin1Char('x')); big << it; }
    const QList<IpcProtocol::Message> up = {
        IpcProtocol::makeHeartbeat("ch1", 123456789LL, 4, true),
        IpcProtocol::makeStateChange("ch1", "ON_AIR", "take"),
        IpcProtocol::makeAsRunLog("ch1", "START", "asset-42", 99LL),
        IpcProtocol::make(IpcProtocol::MSG_PREVIEW_FRAME, "ch1", QJsonObject{{"jpegBase64", QString(50000, QLatin1Char('Q'))}, {"frame", 7}}),
    };
    for (const auto& m : up) QVERIFY(engine.send(m));
    QTRY_COMPARE_WITH_TIMEOUT(srv.count(), up.size(), 3000);
    for (int i = 0; i < up.size(); ++i) {
        const auto back = srv.at(i).at(0).value<IpcProtocol::Message>();
        QCOMPARE(back.type, up[i].type);
        QCOMPARE(back.channelId, up[i].channelId);
        QVERIFY(back.timestampNs > 0);   // carried as a JSON number (double precision), so not compared exactly
        QCOMPARE(back.payload, up[i].payload);
        QCOMPARE(srv.at(i).at(1).toString(), id);
    }

    const QList<IpcProtocol::Message> down = {
        IpcProtocol::makePlaylistPush("ch1", big, true),
        IpcProtocol::makeTransportCommand("ch1", "seek", QJsonObject{{"seconds", 12.5}}),
        IpcProtocol::makeTimeSync(10, 20, 30),
    };
    QVERIFY(server.sendTo(id, down[0]));
    QVERIFY(server.sendTo(id, down[1]));
    server.broadcast(down[2]);
    QTRY_COMPARE_WITH_TIMEOUT(eng.count(), down.size(), 5000);
    for (int i = 0; i < down.size(); ++i) {
        const auto back = eng.at(i).at(0).value<IpcProtocol::Message>();
        QCOMPARE(back.type, down[i].type);
        QCOMPARE(back.payload, down[i].payload);
    }
    QCOMPARE(eng.at(0).at(0).value<IpcProtocol::Message>().payload.value("playlist").toArray().size(), 64);
}

QTEST_GUILESS_MAIN(TestIpcProtocol)
#include "test_ipcprotocol.moc"
