#ifndef ONVIFMANAGER_H
#define ONVIFMANAGER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTimer>
#include <functional>

class QUdpSocket;
namespace sched { class ScheduleStore; }

/**
 * OnvifManager — camera discovery (WS-Discovery) and PTZ control (ONVIF SOAP) for the Supervisor.
 *
 * Discovery multicasts a WS-Discovery Probe (239.255.255.250:3702) on every IPv4 interface and
 * collects ProbeMatch replies for a few seconds. Devices the operator saves (with credentials) are
 * kept in the settings table under `onvif.devices`; discovery results are merged with them by
 * endpoint address / XAddr, so a saved camera stays listed while it is offline.
 *
 * Per device we resolve the Media and PTZ service addresses (GetCapabilities), the first PTZ-capable
 * media profile (GetProfiles), the RTSP stream URI (GetStreamUri, so the camera can be registered as
 * a live `stream` source for the schedule) and the preset table (GetPresets).
 *
 * PTZ commands: {action: move, x, y, z} (ContinuousMove velocities, -1..1), {action: stop},
 * {action: preset, preset}, {action: setPreset, name, preset?}, {action: home}, {action: absolute, x, y, z},
 * {action: relative, x, y, z}. Each is one SOAP call with a WS-UsernameToken digest header (HTTP
 * Digest auth is answered too when the camera insists).
 */
class OnvifManager : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;

    explicit OnvifManager(sched::ScheduleStore* store, QObject* parent = nullptr);
    ~OnvifManager() override;

    void discover(int waitMs = 3000);
    bool discovering() const { return m_discovering; }
    qint64 lastDiscoveryMs() const { return m_lastDiscoveryMs; }

    QJsonArray devicesJson() const;
    QJsonObject deviceJson(const QString& id, bool* found = nullptr) const;
    /// Save/update a device: {id?, xaddr, name?, user?, pass?}. Returns the stored record (without the password).
    QJsonObject saveDevice(const QJsonObject& dev);
    bool removeDevice(const QString& id);

    void refresh(const QString& id);                                  // capabilities + profile + stream + presets
    void ptz(const QString& id, const QJsonObject& cmd, Reply reply);
    void presets(const QString& id, Reply reply);
    void streamUri(const QString& id, Reply reply);

signals:
    void devicesChanged();

private:
    struct Device {
        QString id;            // urn:uuid or a hash of the XAddr
        QString name, hardware, location;
        QString xaddr;         // device service
        QString user, pass;
        QString mediaUrl, ptzUrl, profileToken, streamUri;
        QJsonArray presets;
        bool configured = false;
        bool online = false;
        bool hasPtz = false;
        qint64 lastSeenMs = 0;
        qint64 refreshedMs = 0;
        QString error;
        QJsonObject toJson(bool withSecrets) const;
    };

    sched::ScheduleStore* m_store;
    QNetworkAccessManager m_nam;
    QHash<QString, Device> m_devices;
    QList<QUdpSocket*> m_probeSockets;
    QTimer m_probeTimer;
    bool m_discovering = false;
    qint64 m_lastDiscoveryMs = 0;

    void load();
    void persist();
    void onProbeReply();
    void finishDiscovery();
    static QString idForXaddr(const QString& xaddr);

    QString envelope(const Device& d, const QString& body) const;
    void soap(const QString& id, const QString& url, const QString& body,
              std::function<void(bool ok, const QString& xml, const QString& error)> done);
    void fetchProfiles(const QString& id, std::function<void()> then);
    void fetchStream(const QString& id, std::function<void()> then);
    void fetchPresets(const QString& id, std::function<void()> then);
    static QString tag(const QString& xml, const QString& localName, int from = 0, int* end = nullptr);
    static QString attr(const QString& xml, const QString& name);
    static QString xmlEscape(const QString& s);
};

#endif // ONVIFMANAGER_H
