#include <QtTest/QtTest>
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QUuid>
#include "playout/schedule/schedulestore.h"
#include "playout/supervisor/laneext.h"
#include "playout/supervisor/restapiserver.h"

class TestLanPeers : public QObject {
    Q_OBJECT
private slots:
    void remoteApiRequiresPassword() {
        QHostAddress local;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            if (!iface.flags().testFlag(QNetworkInterface::IsUp) || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
            for (const auto& a : iface.addressEntries()) if (a.ip().protocol() == QAbstractSocket::IPv4Protocol) { local = a.ip(); break; }
            if (!local.isNull()) break;
        }
        if (local.isNull()) QSKIP("No local IPv4 interface available");
        RestApiServer rest(nullptr); QVERIFY(rest.start(0)); rest.setLanPassword("test-password");
        rest.registerRoute("GET", "/api/v1/probe", [](const QJsonObject&, const QHash<QString,QString>&) { return QJsonObject{{"ok",true}}; });
        auto request = [&](const QByteArray& header, bool loopback) {
            QTcpSocket socket;
            if (!loopback && !socket.bind(local, 0)) return 0;
            socket.connectToHost(loopback ? QHostAddress::LocalHost : local, rest.port());
            if (!socket.waitForConnected(3000)) return 0;
            socket.write("GET /api/v1/probe HTTP/1.1\r\nHost: test\r\nConnection: close\r\n" + header + "\r\n");
            QByteArray result;
            for (int i=0; i<100 && !result.contains("\r\n\r\n"); ++i) { QCoreApplication::processEvents(); socket.waitForReadyRead(10); result += socket.readAll(); }
            return result.mid(9, 3).toInt();
        };
        QCOMPARE(request({}, true), 200);
        QCOMPARE(request({}, false), 401);
        QCOMPARE(request("X-PP-Pass: wrong\r\n", false), 401);
        QCOMPARE(request("X-PP-Pass: test-password\r\n", false), 200);
        QCOMPARE(request("Authorization: Bearer test-password\r\n", false), 200);
    }
    void acceptsOnlySignedLocalAnnouncements() {
        QHostAddress local;
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            if (!iface.flags().testFlag(QNetworkInterface::IsUp) || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
            for (const auto& address : iface.addressEntries())
                if (address.ip().protocol() == QAbstractSocket::IPv4Protocol && address.prefixLength() > 0) { local = address.ip(); break; }
            if (!local.isNull()) break;
        }
        if (local.isNull()) QSKIP("No local IPv4 interface available");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        sched::ScheduleStore store; QVERIFY(store.open(dir.filePath("test.db")));
        const QString pass = QUuid::createUuid().toString(QUuid::WithoutBraces);
        store.setSettingJson("remote.lan.discovery", {{"enabled",true},{"password",pass}});
        QObject parent;
        RestApiServer rest(nullptr);
        QUdpSocket portProbe; QVERIFY(portProbe.bind(QHostAddress::LocalHost, 0));
        const quint16 discoveryPort = portProbe.localPort(); portProbe.close();
        laneext::Context c; c.parent = &parent; c.rest = &rest; c.store = &store; c.restPort = 17949; c.lanDiscoveryPort = discoveryPort;
        QVERIFY(laneext::installAll(c).contains("lan-peers"));
        QUdpSocket sender; QVERIFY(sender.bind(local, 0));
        QJsonObject p{{"kind","playplay-lan-v1"}, {"id","test-peer"}, {"name","Test peer"}, {"port",17950},
                      {"time",QString::number(QDateTime::currentMSecsSinceEpoch())}, {"address",local.toString()}};
        auto send = [&](const QString& key) {
            const QByteArray msg = (p.value("id").toString()+"\n"+p.value("name").toString()+"\n"+
                QString::number(p.value("port").toInt())+"\n"+p.value("time").toString()+"\n"+p.value("address").toString()).toUtf8();
            p["mac"] = QString::fromLatin1(QMessageAuthenticationCode::hash(msg, key.toUtf8(), QCryptographicHash::Sha256).toHex());
            const QByteArray datagram = QJsonDocument(p).toJson(QJsonDocument::Compact);
            return sender.writeDatagram(datagram, local, discoveryPort) == datagram.size();
        };
        QVERIFY(send("wrong-password")); QTest::qWait(100);
        QVERIFY(store.settingJson("remote.nodes").value("nodes").toArray().isEmpty());
        QVERIFY(send(pass));
        QTRY_COMPARE_WITH_TIMEOUT(store.settingJson("remote.nodes").value("nodes").toArray().size(), 1, 1500);
        const QJsonObject node = store.settingJson("remote.nodes").value("nodes").toArray().first().toObject();
        QCOMPARE(node.value("id").toString(), QString("test-peer"));
        QCOMPARE(node.value("url").toString(), QString("http://%1:17950").arg(local.toString()));
        QCOMPARE(node.value("token").toString(), pass);
        p["id"] = "expired-peer"; p["time"] = QString::number(QDateTime::currentMSecsSinceEpoch() - 120000);
        QVERIFY(send(pass)); QTest::qWait(100);
        QCOMPARE(store.settingJson("remote.nodes").value("nodes").toArray().size(), 1);
    }
};
QTEST_GUILESS_MAIN(TestLanPeers)
#include "test_lanpeers.moc"
