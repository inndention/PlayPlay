#ifndef STARNODES_H
#define STARNODES_H

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <functional>

class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * StarNodes — the Star node registry: enrolment (verified against the node before it is stored), health,
 * and remote execution of an allowlisted set of actions (docs/star-nodes.md).
 *
 * A Star node runs the PlayPlay OBS star-node daemon (exec API on 8898 with x-agent-auth: fleet, sync, swarm,
 * browse, exec; watchdog state API on 8125, loopback only) and usually a PlayPlay station (7860 PlayPlay OBS,
 * 7700 C++). The station side is driven only through suitelogic::stationPathAllowed (port of stations.py).
 */
class StarNodes : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;
    using JsonCb = std::function<void(const QJsonObject&)>;
    explicit StarNodes(sched::ScheduleStore* store, QObject* parent = nullptr);
    void start();

    QJsonObject config() const;
    QJsonObject setConfig(const QJsonObject& patch);
    QJsonArray nodes(bool withSecrets = false) const;
    QJsonObject node(const QString& id, bool* found = nullptr, bool withSecrets = false) const;
    void enrol(const QJsonObject& body, Reply reply);
    QJsonObject update(const QString& id, const QJsonObject& patch);
    bool remove(const QString& id);
    void probe(const QString& id, JsonCb cb);
    void probeAll(JsonCb cb);
    void action(const QString& id, const QString& action, const QJsonObject& args, Reply reply);
    QJsonArray audit(int limit = 200) const;

    // stations (PlayPlay OBS stations.py shapes)
    QJsonArray stations() const;
    void stationCapabilities(const QString& sid, bool force, Reply reply);
    void stationCall(const QString& sid, const QString& method, const QString& path, const QJsonValue& body, Reply reply);
    // PlayPlay OBS /api/nodes registry rows
    QJsonArray stationNodeRows() const;
    void createStationNode(const QJsonObject& body, Reply reply);
    void refreshInventory(const QString& id, Reply reply);
    QJsonObject inventory(const QString& id) const;

signals:
    void event(const QString& type, const QJsonObject& payload);
    void alert(const QString& level, const QString& message);

private:
    sched::ScheduleStore* m_store;
    SuiteHttp* m_http;
    QHash<QString, QJsonObject> m_health;              // node id -> {exec, watchdog, playplay, ok, checkedMs}
    QHash<QString, QPair<qint64, QJsonObject>> m_caps;  // station capability cache
    QTimer m_poll;

    QJsonArray stored() const;
    void storeAll(const QJsonArray& a);
    QJsonObject normalize(const QJsonObject& in, const QJsonObject& prev) const;
    QList<QPair<QByteArray, QByteArray>> execHeaders(const QJsonObject& n) const;
    QList<QPair<QByteArray, QByteArray>> stationHeaders(const QJsonObject& n) const;
    void execCall(const QJsonObject& n, const QString& method, const QString& path, const QJsonObject& body, int timeoutMs, Reply reply);
    void logAudit(const QString& node, const QString& action, const QJsonObject& args, int status, const QString& error);
    void notify();
};

#endif // STARNODES_H
