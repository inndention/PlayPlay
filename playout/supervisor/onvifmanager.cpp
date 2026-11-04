#include "onvifmanager.h"
#include "../schedule/schedulestore.h"

#include <QUdpSocket>
#include <QNetworkInterface>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QDateTime>
#include <QUuid>
#include <QUrl>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QDebug>

using namespace sched;

static const char* kNsSoap = "http://www.w3.org/2003/05/soap-envelope";
static const char* kNsTds  = "http://www.onvif.org/ver10/device/wsdl";
static const char* kNsTrt  = "http://www.onvif.org/ver10/media/wsdl";
static const char* kNsTptz = "http://www.onvif.org/ver20/ptz/wsdl";
static const char* kNsTt   = "http://www.onvif.org/ver10/schema";

OnvifManager::OnvifManager(ScheduleStore* store, QObject* parent) : QObject(parent), m_store(store) {
    m_probeTimer.setSingleShot(true);
    connect(&m_probeTimer, &QTimer::timeout, this, &OnvifManager::finishDiscovery);
    // Cameras that insist on HTTP Digest instead of (or in addition to) WS-Security.
    connect(&m_nam, &QNetworkAccessManager::authenticationRequired, this, [this](QNetworkReply* r, QAuthenticator* a) {
        const QString id = r->property("ppDevice").toString();
        if (!m_devices.contains(id) || r->property("ppAuthTried").toBool()) return;
        r->setProperty("ppAuthTried", true);
        a->setUser(m_devices[id].user); a->setPassword(m_devices[id].pass);
    });
    load();
}

OnvifManager::~OnvifManager() { qDeleteAll(m_probeSockets); }

// ---------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------
QJsonObject OnvifManager::Device::toJson(bool withSecrets) const {
    QJsonObject o;
    o["id"] = id; o["name"] = name; o["hardware"] = hardware; o["location"] = location; o["xaddr"] = xaddr;
    o["user"] = user; if (withSecrets) o["pass"] = pass; else o["hasPassword"] = !pass.isEmpty();
    o["mediaUrl"] = mediaUrl; o["ptzUrl"] = ptzUrl; o["profileToken"] = profileToken; o["streamUri"] = streamUri;
    o["presets"] = presets; o["configured"] = configured; o["online"] = online; o["hasPtz"] = hasPtz;
    o["lastSeenMs"] = double(lastSeenMs); o["refreshedMs"] = double(refreshedMs); o["error"] = error;
    QUrl u(xaddr); o["host"] = u.host();
    return o;
}

void OnvifManager::load() {
    if (!m_store) return;
    const QJsonArray arr = m_store->settingJson("onvif.devices").value("devices").toArray();
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        Device d; d.id = o.value("id").toString(); d.name = o.value("name").toString(); d.hardware = o.value("hardware").toString();
        d.location = o.value("location").toString(); d.xaddr = o.value("xaddr").toString(); d.user = o.value("user").toString();
        d.pass = o.value("pass").toString(); d.mediaUrl = o.value("mediaUrl").toString(); d.ptzUrl = o.value("ptzUrl").toString();
        d.profileToken = o.value("profileToken").toString(); d.streamUri = o.value("streamUri").toString(); d.presets = o.value("presets").toArray();
        d.hasPtz = o.value("hasPtz").toBool(); d.configured = true;
        if (d.id.isEmpty()) d.id = idForXaddr(d.xaddr);
        if (!d.xaddr.isEmpty()) m_devices[d.id] = d;
    }
}

void OnvifManager::persist() {
    if (!m_store) return;
    QJsonArray arr;
    for (const Device& d : m_devices) if (d.configured) arr.append(d.toJson(true));
    QJsonObject o; o["devices"] = arr;
    m_store->setSettingJson("onvif.devices", o);
    emit devicesChanged();
}

QString OnvifManager::idForXaddr(const QString& xaddr) {
    return "cam-" + QString::fromLatin1(QCryptographicHash::hash(QUrl(xaddr).host().toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

QJsonArray OnvifManager::devicesJson() const {
    QJsonArray a;
    for (const Device& d : m_devices) a.append(d.toJson(false));
    return a;
}

QJsonObject OnvifManager::deviceJson(const QString& id, bool* found) const {
    if (found) *found = m_devices.contains(id);
    return m_devices.contains(id) ? m_devices[id].toJson(false) : QJsonObject();
}

/// What an operator typed as a camera address -> host[:port]. People paste stream addresses ("srt://192.168.1.65:4578",
/// "rtsp://192.168.1.65:554/stream1", "192.168.1.65:4578/live/av0"): the scheme, credentials and path are dropped.
static QString hostPortOf(QString in) {
    in = in.trimmed();
    const int scheme = in.indexOf("://");
    if (scheme >= 0) in = in.mid(scheme + 3);
    const int at = in.lastIndexOf('@'); if (at >= 0) in = in.mid(at + 1);
    const int slash = in.indexOf('/'); if (slash >= 0) in = in.left(slash);
    return in;
}

QJsonObject OnvifManager::saveDevice(const QJsonObject& dev) {
    QString id = dev.value("id").toString();
    QString xaddr = dev.value("xaddr").toString().trimmed();
    // Only an http(s) URL with "onvif" in its path is an ONVIF device address; anything else (a stream URL, an http page) is a host.
    const QUrl xu(xaddr);
    const bool explicitXaddr = !xaddr.isEmpty() && (xu.scheme() == "http" || xu.scheme() == "https") && xu.path().contains("onvif", Qt::CaseInsensitive);
    const QString host = explicitXaddr ? QString() : hostPortOf(dev.value("host").toString().isEmpty() ? xaddr : dev.value("host").toString());
    if (!explicitXaddr) xaddr = host.isEmpty() ? QString() : "http://" + host + "/onvif/device_service";
    const QString ip = QUrl(xaddr).host();
    if (id.isEmpty() || !m_devices.contains(id)) {
        // Match an already discovered device by its IP.
        for (const Device& d : m_devices) if (!ip.isEmpty() && QUrl(d.xaddr).host() == ip && d.online && !d.id.isEmpty()) { id = d.id; break; }
        if (id.isEmpty()) for (const Device& d : m_devices) if (!ip.isEmpty() && QUrl(d.xaddr).host() == ip) { id = d.id; break; }
        if (id.isEmpty()) id = idForXaddr(xaddr);
    }
    Device& d = m_devices[id];
    d.id = id;
    // A typed host never replaces the address discovery found for that camera (the ONVIF service is often on a port of its own,
    // 2000 / 2001 / 8080 ..., which a stream address does not tell): only a full http(s)://.../device_service URL does.
    const bool keepDiscovered = !explicitXaddr && !d.xaddr.isEmpty() && QUrl(d.xaddr).host() == ip && d.online;
    if (!xaddr.isEmpty() && !keepDiscovered) d.xaddr = xaddr;
    if (dev.contains("name")) d.name = dev.value("name").toString();
    if (dev.contains("user")) d.user = dev.value("user").toString();
    if (dev.contains("pass") && !dev.value("pass").toString().isEmpty()) d.pass = dev.value("pass").toString();
    if (d.name.isEmpty()) d.name = QUrl(d.xaddr).host();
    d.configured = true; d.error.clear();
    persist();
    refresh(id);
    return d.toJson(false);
}

bool OnvifManager::removeDevice(const QString& id) {
    if (!m_devices.contains(id)) return false;
    m_devices.remove(id);
    persist();
    return true;
}

// ---------------------------------------------------------------------------------------------
// WS-Discovery
// ---------------------------------------------------------------------------------------------
void OnvifManager::discover(int waitMs) {
    if (m_discovering) return;
    m_discovering = true;
    qDeleteAll(m_probeSockets); m_probeSockets.clear();
    const QString msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray probe = QString(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header><w:MessageID>uuid:%1</w:MessageID><w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header>"
        "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body></e:Envelope>").arg(msgId).toUtf8();

    // One socket per IPv4 interface so the multicast leaves on every NIC (the engine room LAN and the camera VLAN).
    for (const QNetworkInterface& ifc : QNetworkInterface::allInterfaces()) {
        if (!(ifc.flags() & QNetworkInterface::IsUp) || !(ifc.flags() & QNetworkInterface::IsRunning) || (ifc.flags() & QNetworkInterface::IsLoopBack)) continue;
        for (const QNetworkAddressEntry& e : ifc.addressEntries()) {
            if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
            auto* s = new QUdpSocket(this);
            if (!s->bind(e.ip(), 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) { delete s; continue; }
            s->setMulticastInterface(ifc);
            s->setSocketOption(QAbstractSocket::MulticastTtlOption, 4);
            connect(s, &QUdpSocket::readyRead, this, &OnvifManager::onProbeReply);
            s->writeDatagram(probe, QHostAddress("239.255.255.250"), 3702);
            m_probeSockets.append(s);
        }
    }
    // Repeat the probe once after 600 ms: cameras are allowed to answer with a random delay and some miss the first one.
    QTimer::singleShot(600, this, [this, probe]() { for (QUdpSocket* s : m_probeSockets) s->writeDatagram(probe, QHostAddress("239.255.255.250"), 3702); });
    for (Device& d : m_devices) d.online = false;
    m_probeTimer.start(qMax(500, waitMs));
    qInfo() << "[ONVIF] discovery probe on" << m_probeSockets.size() << "interface(s)";
}

static QString urlDecode(const QString& s) { return QUrl::fromPercentEncoding(s.toUtf8()); }

void OnvifManager::onProbeReply() {
    auto* s = qobject_cast<QUdpSocket*>(sender());
    if (!s) return;
    while (s->hasPendingDatagrams()) {
        QByteArray buf; buf.resize(int(s->pendingDatagramSize()));
        QHostAddress from; quint16 port = 0;
        s->readDatagram(buf.data(), buf.size(), &from, &port);
        const QString xml = QString::fromUtf8(buf);
        if (!xml.contains("ProbeMatch")) continue;
        const QString xaddrs = tag(xml, "XAddrs").trimmed();
        if (xaddrs.isEmpty()) continue;
        // Prefer the XAddr that matches the sender's address family/host; else the first.
        QString xaddr;
        for (const QString& x : xaddrs.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) { if (xaddr.isEmpty()) xaddr = x; if (QUrl(x).host() == from.toString()) { xaddr = x; break; } }
        QString epr = tag(xml, "Address").trimmed();       // EndpointReference/Address (urn:uuid:...)
        const QString scopes = tag(xml, "Scopes");
        QString name, hw, loc;
        static const QRegularExpression rxName("onvif://www\\.onvif\\.org/name/([^\\s]+)"), rxHw("onvif://www\\.onvif\\.org/hardware/([^\\s]+)"), rxLoc("onvif://www\\.onvif\\.org/location/([^\\s]+)");
        if (auto m = rxName.match(scopes); m.hasMatch()) name = urlDecode(m.captured(1));
        if (auto m = rxHw.match(scopes); m.hasMatch()) hw = urlDecode(m.captured(1));
        if (auto m = rxLoc.match(scopes); m.hasMatch()) loc = urlDecode(m.captured(1));

        // Merge: configured device with the same host wins its id; otherwise key by the endpoint urn / host hash.
        QString id;
        for (const Device& d : m_devices) if (QUrl(d.xaddr).host() == QUrl(xaddr).host()) { id = d.id; break; }
        if (id.isEmpty()) id = idForXaddr(xaddr);
        Device& d = m_devices[id];
        const bool isNew = d.id.isEmpty();
        d.id = id; d.xaddr = xaddr; d.online = true; d.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
        if (!epr.isEmpty() && d.hardware.isEmpty()) d.hardware = hw;
        if (!hw.isEmpty()) d.hardware = hw;
        if (!loc.isEmpty()) d.location = loc;
        if (d.name.isEmpty() || (!d.configured && !name.isEmpty())) d.name = name.isEmpty() ? QUrl(xaddr).host() : name;
        if (isNew) qInfo().noquote() << "[ONVIF] found" << d.name << hw << xaddr;
        // Pull capabilities for anonymous-friendly devices (and for configured ones with credentials).
        if (d.mediaUrl.isEmpty() && (d.configured || isNew)) refresh(id);
    }
}

void OnvifManager::finishDiscovery() {
    m_discovering = false;
    m_lastDiscoveryMs = QDateTime::currentMSecsSinceEpoch();
    int online = 0; for (const Device& d : m_devices) if (d.online) ++online;
    qInfo() << "[ONVIF] discovery done:" << online << "online," << m_devices.size() << "known";
    QTimer::singleShot(200, this, [this]() { qDeleteAll(m_probeSockets); m_probeSockets.clear(); });
    emit devicesChanged();
}

// ---------------------------------------------------------------------------------------------
// SOAP plumbing
// ---------------------------------------------------------------------------------------------
QString OnvifManager::xmlEscape(const QString& s) {
    QString o = s; o.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;"); return o;
}

QString OnvifManager::envelope(const Device& d, const QString& body) const {
    QString header;
    if (!d.user.isEmpty()) {
        QByteArray nonce(16, 0); for (char& c : nonce) c = char(QRandomGenerator::global()->bounded(256));
        const QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
        const QByteArray digest = QCryptographicHash::hash(nonce + created.toUtf8() + d.pass.toUtf8(), QCryptographicHash::Sha1).toBase64();
        header = QString(
            "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
            "<UsernameToken><Username>%1</Username>"
            "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%2</Password>"
            "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</Nonce>"
            "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">%4</Created>"
            "</UsernameToken></Security></s:Header>").arg(xmlEscape(d.user), QString::fromLatin1(digest), QString::fromLatin1(nonce.toBase64()), created);
    }
    return QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"%1\" xmlns:tds=\"%2\" xmlns:trt=\"%3\" xmlns:tptz=\"%4\" xmlns:tt=\"%5\">%6<s:Body>%7</s:Body></s:Envelope>")
        .arg(kNsSoap, kNsTds, kNsTrt, kNsTptz, kNsTt, header, body);
}

void OnvifManager::soap(const QString& id, const QString& url, const QString& body, std::function<void(bool, const QString&, const QString&)> done) {
    if (!m_devices.contains(id)) { done(false, QString(), "unknown device"); return; }
    if (url.isEmpty()) { done(false, QString(), "service address unknown (refresh the device)"); return; }
    const QByteArray payload = envelope(m_devices[id], body).toUtf8();
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml; charset=utf-8");
    req.setTransferTimeout(6000);
    QNetworkReply* r = m_nam.post(req, payload);
    r->setProperty("ppDevice", id);
    connect(r, &QNetworkReply::finished, this, [this, r, id, done]() {
        r->deleteLater();
        const QString xml = QString::fromUtf8(r->isOpen() ? r->readAll() : QByteArray());
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError && xml.isEmpty()) { done(false, xml, r->errorString()); return; }
        if (xml.contains("Fault")) {
            QString reason = tag(xml, "Text"); if (reason.isEmpty()) reason = tag(xml, "Reason"); if (reason.isEmpty()) reason = tag(xml, "faultstring");
            done(false, xml, reason.trimmed().isEmpty() ? QString("SOAP fault (HTTP %1)").arg(http) : reason.trimmed()); return;
        }
        if (http >= 400) { done(false, xml, QString("HTTP %1").arg(http)); return; }
        done(true, xml, QString());
    });
}

// First element with this local name (any namespace prefix); returns inner text.
QString OnvifManager::tag(const QString& xml, const QString& localName, int from, int* end) {
    const QRegularExpression rx("<(?:[\\w-]+:)?" + QRegularExpression::escape(localName) + "(?:\\s[^>]*)?>(.*?)</(?:[\\w-]+:)?" + QRegularExpression::escape(localName) + ">", QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = rx.match(xml, from);
    if (!m.hasMatch()) { if (end) *end = -1; return QString(); }
    if (end) *end = m.capturedEnd(0);
    return m.captured(1);
}

QString OnvifManager::attr(const QString& xml, const QString& name) {
    const QRegularExpression rx("\\b" + QRegularExpression::escape(name) + "=\"([^\"]*)\"");
    const QRegularExpressionMatch m = rx.match(xml);
    return m.hasMatch() ? m.captured(1) : QString();
}

// ---------------------------------------------------------------------------------------------
// Device information chain: capabilities -> profiles -> stream uri -> presets
// ---------------------------------------------------------------------------------------------
void OnvifManager::refresh(const QString& id) {
    if (!m_devices.contains(id)) return;
    soap(id, m_devices[id].xaddr, "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>",
         [this, id](bool ok, const QString& xml, const QString& err) {
        if (!m_devices.contains(id)) return;
        Device& d = m_devices[id];
        if (!ok) { d.error = err; qWarning().noquote() << "[ONVIF]" << d.name << "GetCapabilities:" << err; emit devicesChanged(); return; }
        d.error.clear();
        const QString media = tag(xml, "Media"); const QString ptz = tag(xml, "PTZ");
        d.mediaUrl = tag(media, "XAddr").trimmed();
        d.ptzUrl = tag(ptz, "XAddr").trimmed();
        d.hasPtz = !d.ptzUrl.isEmpty();
        if (d.mediaUrl.isEmpty()) d.mediaUrl = QUrl(d.xaddr).toString(QUrl::RemovePath) + "/onvif/media_service";
        fetchProfiles(id, [this, id]() { fetchStream(id, [this, id]() { fetchPresets(id, [this, id]() {
            if (m_devices.contains(id)) { m_devices[id].refreshedMs = QDateTime::currentMSecsSinceEpoch(); if (m_devices[id].configured) persist(); else emit devicesChanged(); }
        }); }); });
    });
}

void OnvifManager::fetchProfiles(const QString& id, std::function<void()> then) {
    soap(id, m_devices.value(id).mediaUrl, "<trt:GetProfiles/>", [this, id, then](bool ok, const QString& xml, const QString& err) {
        if (!m_devices.contains(id)) return;
        Device& d = m_devices[id];
        if (!ok) { d.error = err; then(); return; }
        // Prefer a profile that carries a PTZConfiguration; else the first one.
        const QRegularExpression rx("<(?:[\\w-]+:)?Profiles\\s[^>]*token=\"([^\"]+)\"[^>]*>(.*?)</(?:[\\w-]+:)?Profiles>", QRegularExpression::DotMatchesEverythingOption);
        QString first, ptzTok;
        auto it = rx.globalMatch(xml);
        while (it.hasNext()) { const auto m = it.next(); if (first.isEmpty()) first = m.captured(1); if (ptzTok.isEmpty() && m.captured(2).contains("PTZConfiguration")) ptzTok = m.captured(1); }
        d.profileToken = ptzTok.isEmpty() ? first : ptzTok;
        then();
    });
}

void OnvifManager::fetchStream(const QString& id, std::function<void()> then) {
    const Device d = m_devices.value(id);
    if (d.profileToken.isEmpty()) { then(); return; }
    const QString body = QString("<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>%1</trt:ProfileToken></trt:GetStreamUri>").arg(xmlEscape(d.profileToken));
    soap(id, d.mediaUrl, body, [this, id, then](bool ok, const QString& xml, const QString&) {
        if (m_devices.contains(id) && ok) {
            QString uri = tag(xml, "Uri").trimmed();
            // Embed credentials so FFmpeg can open the RTSP stream directly as a live `stream` source.
            const Device& d = m_devices[id];
            if (!d.user.isEmpty() && !uri.isEmpty()) { QUrl u(uri); if (u.userName().isEmpty()) { u.setUserName(d.user); u.setPassword(d.pass); uri = u.toString(); } }
            m_devices[id].streamUri = uri;
        }
        then();
    });
}

void OnvifManager::fetchPresets(const QString& id, std::function<void()> then) {
    const Device d = m_devices.value(id);
    if (!d.hasPtz || d.profileToken.isEmpty()) { then(); return; }
    soap(id, d.ptzUrl, QString("<tptz:GetPresets><tptz:ProfileToken>%1</tptz:ProfileToken></tptz:GetPresets>").arg(xmlEscape(d.profileToken)),
         [this, id, then](bool ok, const QString& xml, const QString&) {
        if (m_devices.contains(id) && ok) {
            QJsonArray arr;
            const QRegularExpression rx("<(?:[\\w-]+:)?Preset\\s[^>]*token=\"([^\"]+)\"[^>]*>(.*?)</(?:[\\w-]+:)?Preset>", QRegularExpression::DotMatchesEverythingOption);
            auto it = rx.globalMatch(xml);
            while (it.hasNext()) { const auto m = it.next(); QJsonObject p; p["token"] = m.captured(1); p["name"] = tag(m.captured(2), "Name").trimmed(); arr.append(p); }
            m_devices[id].presets = arr;
        }
        then();
    });
}

void OnvifManager::presets(const QString& id, Reply reply) {
    if (!m_devices.contains(id)) { QJsonObject e; e["error"] = "unknown device"; reply(404, e); return; }
    fetchPresets(id, [this, id, reply]() { QJsonObject o; o["id"] = id; o["presets"] = m_devices.value(id).presets; reply(200, o); });
}

void OnvifManager::streamUri(const QString& id, Reply reply) {
    if (!m_devices.contains(id)) { QJsonObject e; e["error"] = "unknown device"; reply(404, e); return; }
    auto answer = [this, id, reply]() {
        const Device& d = m_devices[id];
        QJsonObject o; o["id"] = id; o["streamUri"] = d.streamUri; o["profileToken"] = d.profileToken; o["error"] = d.error;
        reply(d.streamUri.isEmpty() ? 502 : 200, d.streamUri.isEmpty() ? QJsonObject{{"error", d.error.isEmpty() ? "no stream URI (check credentials)" : d.error}} : o);
    };
    if (m_devices[id].profileToken.isEmpty()) fetchProfiles(id, [this, id, answer]() { fetchStream(id, answer); });
    else fetchStream(id, answer);
}

// ---------------------------------------------------------------------------------------------
// PTZ
// ---------------------------------------------------------------------------------------------
void OnvifManager::ptz(const QString& id, const QJsonObject& cmd, Reply reply) {
    if (!m_devices.contains(id)) { QJsonObject e; e["error"] = "unknown device"; reply(404, e); return; }
    const Device& d = m_devices[id];
    if (d.ptzUrl.isEmpty() || d.profileToken.isEmpty()) {
        // Try once to learn the service addresses, then fail clearly.
        if (!cmd.value("_retried").toBool()) { refresh(id); QJsonObject c2 = cmd; c2["_retried"] = true; QTimer::singleShot(1500, this, [this, id, c2, reply]() { ptz(id, c2, reply); }); return; }
        // The usual reason is a camera that wants a login before it lists its profiles ("Sender not Authorized"): say so.
        const QString de = m_devices[id].error;
        const bool auth = de.contains("Authoriz", Qt::CaseInsensitive) || de.contains("Unauthoriz", Qt::CaseInsensitive) || de.contains("401") || de.contains("NotAuthorized", Qt::CaseInsensitive);
        QJsonObject e;
        if (auth) e["error"] = QString("%1 needs its ONVIF user name and password: on Inputs > Cameras select it, leave Host empty, enter them and Save").arg(m_devices[id].name.isEmpty() ? id : m_devices[id].name);
        else e["error"] = d.hasPtz ? (de.isEmpty() ? QString("PTZ profile unknown") : "PTZ profile unknown: " + de) : QString("device has no PTZ service");
        e["needsLogin"] = auth;
        reply(409, e); return;
    }
    const QString action = cmd.value("action").toString("stop");
    const QString tok = xmlEscape(d.profileToken);
    auto num = [&](const char* k, double def = 0.0) { return QString::number(qBound(-1.0, cmd.value(k).toDouble(def), 1.0), 'f', 3); };
    QString body;
    if (action == "move") {
        body = QString("<tptz:ContinuousMove><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:Velocity><tt:PanTilt x=\"%2\" y=\"%3\"/><tt:Zoom x=\"%4\"/></tptz:Velocity>%5</tptz:ContinuousMove>")
                   .arg(tok, num("x"), num("y"), num("z"), cmd.contains("timeoutSec") ? QString("<tptz:Timeout>PT%1S</tptz:Timeout>").arg(cmd.value("timeoutSec").toDouble(), 0, 'f', 1) : QString());
    } else if (action == "stop") {
        body = QString("<tptz:Stop><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop>").arg(tok);
    } else if (action == "preset" || action == "goto") {
        const QString p = cmd.value("preset").toVariant().toString();
        if (p.isEmpty()) { QJsonObject e; e["error"] = "preset token required"; reply(400, e); return; }
        body = QString("<tptz:GotoPreset><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:PresetToken>%2</tptz:PresetToken></tptz:GotoPreset>").arg(tok, xmlEscape(p));
    } else if (action == "setPreset") {
        const QString p = cmd.value("preset").toVariant().toString(), n = cmd.value("name").toString("PlayPlay");
        body = QString("<tptz:SetPreset><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:PresetName>%2</tptz:PresetName>%3</tptz:SetPreset>")
                   .arg(tok, xmlEscape(n), p.isEmpty() ? QString() : QString("<tptz:PresetToken>%1</tptz:PresetToken>").arg(xmlEscape(p)));
    } else if (action == "removePreset") {
        body = QString("<tptz:RemovePreset><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:PresetToken>%2</tptz:PresetToken></tptz:RemovePreset>").arg(tok, xmlEscape(cmd.value("preset").toVariant().toString()));
    } else if (action == "home") {
        body = QString("<tptz:GotoHomePosition><tptz:ProfileToken>%1</tptz:ProfileToken></tptz:GotoHomePosition>").arg(tok);
    } else if (action == "setHome") {
        body = QString("<tptz:SetHomePosition><tptz:ProfileToken>%1</tptz:ProfileToken></tptz:SetHomePosition>").arg(tok);
    } else if (action == "absolute") {
        body = QString("<tptz:AbsoluteMove><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:Position><tt:PanTilt x=\"%2\" y=\"%3\"/><tt:Zoom x=\"%4\"/></tptz:Position></tptz:AbsoluteMove>").arg(tok, num("x"), num("y"), num("z"));
    } else if (action == "relative") {
        body = QString("<tptz:RelativeMove><tptz:ProfileToken>%1</tptz:ProfileToken><tptz:Translation><tt:PanTilt x=\"%2\" y=\"%3\"/><tt:Zoom x=\"%4\"/></tptz:Translation></tptz:RelativeMove>").arg(tok, num("x"), num("y"), num("z"));
    } else { QJsonObject e; e["error"] = "unknown PTZ action: " + action; reply(400, e); return; }

    soap(id, d.ptzUrl, body, [this, id, action, cmd, reply](bool ok, const QString& xml, const QString& err) {
        QJsonObject o; o["id"] = id; o["action"] = action; o["ok"] = ok;
        if (!ok) { o["error"] = err; qWarning().noquote() << "[ONVIF] PTZ" << action << "on" << id << "failed:" << err; reply(502, o); return; }
        if (action == "setPreset") { o["preset"] = tag(xml, "PresetToken").trimmed(); fetchPresets(id, [this]() { emit devicesChanged(); }); }
        if (action == "removePreset") fetchPresets(id, [this]() { emit devicesChanged(); });
        reply(200, o);
    });
}
