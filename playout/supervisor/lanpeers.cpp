#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QNetworkInterface>
#include <QSysInfo>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

namespace {
const QString configKey = "remote.lan.discovery";

class LanPeers : public QObject {
public:
    explicit LanPeers(const laneext::Context& c) : QObject(c.parent), ctx(c), socket(this), timer(this) {
        const QJsonObject saved = ctx.store->settingJson(configKey);
        applyPassword(saved.value("enabled").toBool() ? saved.value("password").toString() : QString());
        if (!socket.bind(QHostAddress::AnyIPv4, ctx.lanDiscoveryPort, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            error = socket.errorString(); return;
        }
        connect(&socket, &QUdpSocket::readyRead, this, [this] { receive(); });
        connect(&timer, &QTimer::timeout, this, [this] { advertise(); });
        timer.start(10000);
        QTimer::singleShot(1000, this, [this] { advertise(); });
    }
    QJsonObject status() const {
        const QJsonObject c = ctx.store->settingJson(configKey);
        return {{"enabled", c.value("enabled").toBool()}, {"hasPassword", !c.value("password").toString().isEmpty()},
                {"listening", socket.state() == QAbstractSocket::BoundState}, {"error", error}};
    }
    QJsonObject configure(const QJsonObject& body) {
        QJsonObject c = ctx.store->settingJson(configKey);
        if (body.contains("enabled")) c["enabled"] = body.value("enabled").toBool();
        const QString pass = body.value("password").toString();
        if (!pass.isEmpty()) {
            if (pass.size() < 8 || pass.contains('\n') || pass.contains('\r')) return {{"error", "Password must be at least eight characters and contain no line breaks"}, {"status", 400}};
            c["password"] = pass;
        }
        if (c.value("enabled").toBool() && c.value("password").toString().isEmpty()) return {{"error", "Enter a shared password first"}, {"status", 400}};
        if (c.value("enabled").toBool() && socket.state() != QAbstractSocket::BoundState) return {{"error", "LAN discovery is unavailable: " + error}, {"status", 503}};
        ctx.store->setSettingJson(configKey, c);
        applyPassword(c.value("enabled").toBool() ? c.value("password").toString() : QString());
        advertise(); return status();
    }
private:
    void applyPassword(const QString& password) { if (ctx.setLanPassword) ctx.setLanPassword(password); else ctx.rest->setLanPassword(password); }
    static QByteArray mac(const QJsonObject& packet, const QString& pass) {
        const QByteArray msg = (packet.value("id").toString() + "\n" + packet.value("name").toString() + "\n" +
                                QString::number(packet.value("port").toInt()) + "\n" + packet.value("time").toString() + "\n" + packet.value("address").toString()).toUtf8();
        return QMessageAuthenticationCode::hash(msg, pass.toUtf8(), QCryptographicHash::Sha256).toHex();
    }
    QString localId() {
        QString id = ctx.store->setting("node.id");
        if (id.isEmpty()) { id = QUuid::createUuid().toString(QUuid::WithoutBraces); ctx.store->setSetting("node.id", id); }
        return id;
    }
    void advertise() {
        const QJsonObject c = ctx.store->settingJson(configKey);
        const QString pass = c.value("password").toString();
        if (!c.value("enabled").toBool() || pass.isEmpty() || socket.state() != QAbstractSocket::BoundState) return;
        QJsonObject p{{"kind", "playplay-lan-v1"}, {"id", localId()},
                      {"name", ctx.store->setting("node.name", QSysInfo::machineHostName())},
                      {"port", ctx.restPort}, {"time", QString::number(QDateTime::currentMSecsSinceEpoch())}};
        for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
            const auto flags = iface.flags();
            if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) || flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
            for (const QNetworkAddressEntry& addr : iface.addressEntries()) {
                if (addr.ip().protocol() != QAbstractSocket::IPv4Protocol || addr.broadcast().isNull()) continue;
                p["address"] = addr.ip().toString(); p["mac"] = QString::fromLatin1(mac(p, pass));
                socket.writeDatagram(QJsonDocument(p).toJson(QJsonDocument::Compact), addr.broadcast(), ctx.lanDiscoveryPort);
            }
        }
    }
    void receive() {
        while (socket.hasPendingDatagrams()) {
            QByteArray data; data.resize(int(qMin<qint64>(socket.pendingDatagramSize(), 4096)));
            QHostAddress sender; quint16 sourcePort = 0; socket.readDatagram(data.data(), data.size(), &sender, &sourcePort);
            const QJsonObject c = ctx.store->settingJson(configKey); const QString pass = c.value("password").toString();
            if (!c.value("enabled").toBool() || pass.isEmpty() || sender.protocol() != QAbstractSocket::IPv4Protocol) continue;
            bool sameLan = false;
            for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
                if (!iface.flags().testFlag(QNetworkInterface::IsUp) || iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
                for (const QNetworkAddressEntry& a : iface.addressEntries())
                    if (a.ip().protocol() == QAbstractSocket::IPv4Protocol && a.prefixLength() > 0 && sender.isInSubnet(a.ip(), a.prefixLength())) sameLan = true;
            }
            if (!sameLan) continue;
            const QJsonObject p = QJsonDocument::fromJson(data).object(); const QString id = p.value("id").toString();
            bool timeOk = false; const qint64 sent = p.value("time").toString().toLongLong(&timeOk); const int port = p.value("port").toInt();
            if (p.value("kind").toString() != "playplay-lan-v1" || id.isEmpty() || id.size() > 64 || id == localId() ||
                p.value("name").toString().size() > 128 || p.value("address").toString() != sender.toString() || !timeOk || qAbs(QDateTime::currentMSecsSinceEpoch() - sent) > 60000 || port < 1 || port > 65535) continue;
            const QByteArray actual = p.value("mac").toString().toLatin1(), expected = mac(p, pass);
            if (actual.size() != expected.size()) continue;
            unsigned char diff = 0; for (int i = 0; i < actual.size(); ++i) diff |= static_cast<unsigned char>(actual[i] ^ expected[i]);
            if (diff) continue;
            QJsonObject config = ctx.store->settingJson("remote.nodes"); QJsonArray list = config.value("nodes").toArray();
            const QString url = QString("http://%1:%2").arg(sender.toString()).arg(port); bool found = false, changed = false;
            for (int i = 0; i < list.size(); ++i) {
                QJsonObject n = list[i].toObject(); if (n.value("id").toString() != id) continue; found = true;
                if (n.value("discovered").toBool() && (n.value("url").toString() != url || n.value("token").toString() != pass || n.value("name").toString() != p.value("name").toString())) {
                    n["url"] = url; n["token"] = pass; n["name"] = p.value("name"); list[i] = n; changed = true;
                }
                break;
            }
            if (!found) { list.append(QJsonObject{{"id",id},{"name",p.value("name")},{"url",url},{"token",pass},{"discovered",true}}); changed = true; }
            if (changed) { config["nodes"] = list; ctx.store->setSettingJson("remote.nodes", config); if (ctx.broadcast) ctx.broadcast("remoteNodes", {{"event", "discovered"}}); }
        }
    }
    laneext::Context ctx; QUdpSocket socket; QTimer timer; QString error;
};

void install(const laneext::Context& ctx) {
    if (!ctx.rest || !ctx.store) return;
    auto* service = new LanPeers(ctx);
    ctx.rest->registerRoute("GET", "/api/v1/remote/discovery", [service](const QJsonObject&, const QHash<QString,QString>&) { return service->status(); });
    ctx.rest->registerRoute("PUT", "/api/v1/remote/discovery", [service](const QJsonObject& body, const QHash<QString,QString>&) { return service->configure(body); });
}
static laneext::Registrar registrar("lan-peers", 110, install);
}
