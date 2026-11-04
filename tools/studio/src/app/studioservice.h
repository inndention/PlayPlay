#pragma once
// PlayPlayStudio service (docs/plans/starmedia-studio.md §4): the process that hosts the Studio engine, apart from the
// on-air Supervisor. It resolves the data dir, loads (or creates) the API token <data>/studio-token, and serves:
//   - REST on the HTTP/1.1 server (ST-04, net/httpserver.h) with the router: GET /health in the spot-node shape
//     RenderFarm probes (renderfarm.cpp: {ok, service, version, capable, queue{running, queued, max}}) and the home page
//     are public, everything else needs the token (net/router.h); GET /api/v1/status is the first token route;
//   - the WS event stream with ?since= replay on its own port (net/wshub.h).
// Both bind loopback only, never a port another PlayPlay / StarMedia service owns, never inside the Supervisor's data.
// The REST API (ST-14) adds its routes to router() and attaches the store to hub(); the guards here stay.
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include "net/auth.h"
#include "net/httpserver.h"
#include "net/router.h"
#include "net/wshub.h"

namespace studio {

QString version();                       // PLAYPLAY_STUDIO_VERSION (the PlayPlay project version)
QString serviceName();                   // "playplay-studio" (the /health `service` field)
constexpr quint16 kDefaultRestPort = 7760;
constexpr quint16 kDefaultWsPort = 7761;

/// Application name, organization and version of PlayPlayStudio.exe. The organization is "PlayPlayStudio", never
/// "PlayPlay": QStandardPaths::AppDataLocation and a default QSettings would otherwise resolve to
/// %APPDATA%/PlayPlay/PlayPlayStudio, inside the Supervisor's data tree that insideSupervisorData() refuses.
void setAppIdentity();

/// Ports other PlayPlay / StarMedia services own on a station PC (Supervisor 7700/7701 and its watchdog 7709, test
/// Supervisors, engines 9000-9999, AN-FS, spot-node, StarAgent, StarMedia Local, Ollama, 443/8080). The Studio never
/// binds one: started first, it would keep the on-air Supervisor or a sibling service from binding.
bool reservedPort(quint16 port);
/// True when `dir` is the Supervisor's data tree (%APPDATA%/PlayPlay) or inside it, also through a junction, symlink or
/// 8.3 short name (the deepest existing ancestor of each side is resolved). Read at call time.
bool insideSupervisorData(const QString& dir);
/// env PLAYPLAY_STUDIO_DATA, else %LOCALAPPDATA%/PlayPlayStudio.
QString defaultDataDir();
/// The Host names a server bound to `bind` answers to: localhost, 127.0.0.1, ::1 and the bind address itself. Any other
/// Host header (a web page that rebinds its own name to 127.0.0.1) is refused with 421.
QStringList allowedHostsFor(const QHostAddress& bind);

struct ServiceOptions {
    QHostAddress bind = QHostAddress(QHostAddress::LocalHost);
    quint16 port = kDefaultRestPort;     // 0 = any free port (tests)
    int wsPort = kDefaultWsPort;         // event stream; 0 = any free port, -1 = none
    QString dataDir;                     // empty = defaultDataDir()
};

/// Empty when `opt` may be used; else why not (reserved port, non-loopback bind, Supervisor data dir, bad WS port).
QString checkOptions(const ServiceOptions& opt);

class StudioService : public QObject {
    Q_OBJECT
public:
    explicit StudioService(QObject* parent = nullptr);
    ~StudioService() override;

    /// Creates the data dir, loads or creates the token file and listens (REST, then WS). Non-blocking; false (and
    /// `error`) when refused, the token file is malformed or a port is taken (nothing keeps listening then).
    bool start(const ServiceOptions& opt, QString* error = nullptr);
    void stop();
    bool isListening() const;
    quint16 port() const;
    quint16 wsPort() const;
    QString dataDir() const { return m_dataDir; }
    QString tokenPath() const { return m_auth.tokenPath(); }
    QJsonObject health() const;
    QJsonObject status() const;

    net::Router& router() { return m_router; }
    net::HttpServer& http() { return m_http; }
    net::WsHub& hub() { return m_hub; }
    net::Auth& auth() { return m_auth; }

signals:
    void started(quint16 port);

private:
    net::Auth m_auth;
    net::Router m_router;
    net::HttpServer m_http;
    net::WsHub m_hub;
    QString m_dataDir;
    qint64 m_startedMs = 0;
};

} // namespace studio
