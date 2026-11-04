#pragma once
// Supervisor lane extensions: self-contained modules (MasterPlay import, ...) register their REST routes and services here
// instead of editing supervisorcore*.cpp. A module adds its .cpp to PlayPlaySupervisor from its own cmake/lane-<name>.cmake
// (target_sources) and registers itself with a static registrar:
//
//   static laneext::Registrar reg("masterplay", 100, [](const laneext::Context& ctx) { ... ctx.rest->registerRoute(...); });
//
// installAll() runs once, after every Supervisor manager exists and before the station listener copies the route table.
// Lower order runs first. A module must not block the event loop (no waits, no long synchronous work on this thread).
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

class RestApiServer;
class IngestManager;
namespace sched { class ScheduleStore; }

namespace laneext {

struct Context {
    QObject* parent = nullptr;                    // the SupervisorCore (parent for QObjects / timers a module creates)
    RestApiServer* rest = nullptr;
    sched::ScheduleStore* store = nullptr;
    IngestManager* ingest = nullptr;
    QString dataDir;                              // the Supervisor's data dir (a module keeps its files under <dataDir>/<module>)
    int restPort = 7700;
    quint16 lanDiscoveryPort = 17896;              // override for isolated tests
    std::function<QString()> restToken;           // bearer token for calls to this Supervisor's own REST (empty = none)
    std::function<void(const QString& password)> setLanPassword; // apply LAN access password to REST and WebSocket
    std::function<void(const QString& channelId, const QString& level, const QString& message)> alert;   // operator alert
    std::function<void(const QString& wsType, const QJsonObject& payload)> broadcast;                    // WebSocket event {type: wsType, ...}
    std::function<void(const QString& channelId)> reloadChannel;  // re-plan a channel after its schedule changed
    std::function<QJsonObject(const QString& channelId)> runlist;
    std::function<QJsonObject(const QString& channelId)> engineState;
};

using InstallFn = std::function<void(const Context&)>;

struct Registrar {
    Registrar(const char* name, int order, InstallFn fn);
};

/// Runs every registered module's install function (ordered by `order`, then name). Returns the module names installed.
QStringList installAll(const Context& ctx);
/// Names of registered modules (for /api/v1/status or diagnostics).
QStringList registered();

} // namespace laneext
