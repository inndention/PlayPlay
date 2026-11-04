#ifndef SUPERVISORCORE_H
#define SUPERVISORCORE_H

#include <QObject>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QProcess>
#include <memory>
#include "../ui_ipc/ipctransport.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

class QTcpSocket;
class TimeAuthority;
class NtpClient;
class ChannelManager;
class RestApiServer;
class WebSocketBroker;
class FailoverController;
class OnvifManager;
class MeshClient;
class IngestManager;
class StarClient;
class MuxManager;
class TranscriptionManager;
class SpotGenerator;
class ReportBuilder;
class SessionsManager;
class FailoverPair;
class OverlayManager;
class RenderFarm;
class StarNodes;
class Publisher;
class SpotStudio;
class AgentSuite;
class AiModels;
class StatePublisher;

/**
 * SupervisorCore — the headless automation host.
 *
 * Owns: master TimeAuthority + NTP, the IPC hub (engines and UIs connect
 * here), ChannelManager (engine processes), ScheduleStore (SQLite),
 * ScheduleExecutor (what plays when), REST + WebSocket API, preview relay.
 *
 * Engines are spawned from the channel profiles in the store; the operator
 * UI and the web dashboard are clients and can be closed without affecting
 * playout.
 */
class SupervisorCore : public QObject, public sched::EngineLink {
    Q_OBJECT
public:
    struct Options {
        QString ipcName = "PlayPlaySupervisor";
        QString dataDir = "./data";
        QString enginePath;                 // default: next to this executable
        QString ntpServer = "pool.ntp.org";
        int ntpIntervalSec = 300;
        int restPort = 7700;
        int wsPort = 7701;
        bool autoStartChannels = true;
        QString webRoot;                    // static dashboard files (optional)
        QString mediaRoot;                  // default scan folder (optional)
        QString logDir;                     // JSON-line logs for supervisor + engines (default <dataDir>/logs)
    };

    explicit SupervisorCore(QObject* parent = nullptr);
    ~SupervisorCore() override;

    bool start(const Options& opt);
    void stop();

    // EngineLink
    bool pushPlaylist(const QString& channelId, const QJsonArray& items, bool replaceCurrent) override;
    bool transport(const QString& channelId, const QString& action, const QJsonObject& params) override;
    bool sendConfig(const QString& channelId, const QJsonObject& config) override;

    sched::ScheduleStore* store() const { return m_store.get(); }
    sched::ScheduleExecutor* executor() const { return m_exec.get(); }
    ChannelManager* channels() const { return m_channels.get(); }

    QJsonObject channelSnapshot(const QString& channelId) const;
    QJsonObject fullSnapshot() const;

    /// Media library scan (ffprobe) — asynchronous, returns number of files queued.
    int scanFolder(const QString& folder, bool recursive, const QString& defaultType = "program");

signals:
    void alert(const QString& channelId, const QString& level, const QString& message);

private:
    Options m_opt;
    QElapsedTimer m_uptime;
    TimeAuthority* m_time = nullptr;
    std::unique_ptr<NtpClient> m_ntp;
    std::unique_ptr<IpcTransport> m_ipc;
    std::unique_ptr<ChannelManager> m_channels;
    std::unique_ptr<RestApiServer> m_rest;
    std::unique_ptr<WebSocketBroker> m_ws;
    std::unique_ptr<FailoverController> m_failover;
    std::unique_ptr<sched::ScheduleStore> m_store;
    std::unique_ptr<sched::ScheduleExecutor> m_exec;
    std::unique_ptr<OnvifManager> m_onvif;            // cameras: WS-Discovery + PTZ
    std::unique_ptr<MeshClient> m_mesh;               // AN-FS mesh: nodes, browse, fetch/push
    std::unique_ptr<IngestManager> m_ingest;          // library build pipeline: probe/hash/thumbs, watch folders, removable drives
    std::unique_ptr<StarClient> m_star;               // StarAgent daemons on the Star mesh
    std::unique_ptr<MuxManager> m_mux;
    std::unique_ptr<TranscriptionManager> m_transcribe;   // faster-whisper jobs + segment tools                // ATSC multiplexer (TSDuck) with statmux + PSIP from the schedule
    std::unique_ptr<SpotGenerator> m_spots;             // auto-generated spots (news/weather/community...) + placements
    std::unique_ptr<ReportBuilder> m_reports;           // HTML/CSV reports over as-run + media
    std::unique_ptr<SessionsManager> m_sessions;        // station state bundles: capture/clone/export/apply/push
    std::unique_ptr<FailoverPair> m_redundancy;         // primary/backup pairing with heartbeat + replication
    std::unique_ptr<OverlayManager> m_overlays;         // overlay layers per channel: deploy/hide, presets, defaults, rules, cues (docs/overlays.md)
    std::unique_ptr<RenderFarm> m_render;              // render nodes + jobs (docs/render-nodes.md)
    std::unique_ptr<StarNodes> m_starNodes;            // Star node registry + allowlisted actions (docs/star-nodes.md)
    std::unique_ptr<Publisher> m_publisher;            // social / web publishing (docs/publisher.md)
    std::unique_ptr<SpotStudio> m_studio;              // guided spots -> review -> ad rotation (docs/spot-studio.md)
    std::unique_ptr<AiModels> m_ai;                    // shared OpenRouter key/model + model catalog (aimodels.h)
    std::unique_ptr<AgentSuite> m_agents;              // StarAgent suite: agents + tasks (docs/agents.md)
    std::unique_ptr<RestApiServer> m_stationRest;     // optional second listener (PlayPlay OBS station port, default 7860)
    QTimer m_muxNotify;
    QHash<QString, QJsonObject> m_ingestBatches;      // station-compatible ingest batches (POST /api/media/ingest)
    QHash<QString, QJsonObject> m_cgHtml;             // channelId -> deployed HTML CG template state
    QJsonObject m_cgManifest; qint64 m_cgManifestMs = 0;
    QTimer m_deviceTimer;                             // scheduled device commands (PTZ presets etc.)
    QSet<QString> m_firedDeviceCmds;
    QTimer m_ntpTimer, m_timeSyncTimer, m_uiSnapshotTimer;
    std::unique_ptr<StatePublisher> m_state;          // UI snapshot: built once per tick, sent on change + keepalive (PS-6)

    QSet<QString> m_uiClients;                       // IPC client ids that registered as UIs
    QHash<QString, QJsonObject> m_telemetry;         // channelId -> last engine state
    QHash<QString, QString> m_engineState;           // channelId -> state name
    QHash<QString, QByteArray> m_preview;            // channelId -> last JPEG
    QHash<QString, QJsonObject> m_webMeters;          // last program preview's mixer state
    QHash<QString, QList<QTcpSocket*>> m_mjpeg;      // channelId -> streaming clients
    QList<QJsonObject> m_recentAlerts;
    QHash<QString, QJsonObject> m_slaveStatus;

    // Media scan queue
    struct ScanJob { QString path; QString type; };
    QList<ScanJob> m_scanQueue;
    QProcess* m_probe = nullptr;
    ScanJob m_probeJob;
    void pumpScan();
    QString ffprobePath() const;
    QString uploadDir() const;

    void onIpcMessage(const IpcProtocol::Message& msg, const QString& clientId);
    void onIpcClientDisconnected(const QString& clientId);
    void onUiCommand(const QString& channelId, const QString& action, const QJsonObject& params, const QString& clientId);
    /// An operator take of a library entry. Live entries (live://ndi/..., live://camera/..., or type "live") are resolved to
    /// what the engine opens (ndi://, sdi:// or the stream URL) plus their source spec. Empty result = not playable (`why`).
    QJsonObject resolveTake(const QJsonObject& params, QString* why) const;
    void broadcastUi(IpcProtocol::MessageType type, const QString& channelId, const QJsonObject& payload, const QString& wsType);
    void sendUiSnapshot(const QString& clientId);
    void ensureChannelsFromStore();
    bool spawnChannel(const sched::ChannelProfile& p);
    QJsonObject engineConfigFor(const sched::ChannelProfile& p) const;
    void registerRoutes();
    void registerChannelAutomationRoutes();
    void registerIntegrationRoutes();                 // supervisorcore_integrations.cpp
    void registerLibraryRoutes();                     // supervisorcore_library.cpp: library, ingest, Star agents, mux, station API
    void registerProgrammingRoutes();                 // supervisorcore_programming.cpp: query blocks, hour recipes, weighted fill, autofill, locks
    void registerSpotRoutes();                        // supervisorcore_spots.cpp: constructs m_spots; recipes, jobs, placements, scatter
    void registerReportRoutes();                      // supervisorcore_reports.cpp: constructs m_reports; /reports/*.html, /api/v1/reports/*
    void registerOverlayRoutes();                     // supervisorcore_overlays.cpp: constructs m_overlays; /api/v1/overlays/*
    void registerFlightPlanRoutes();                  // supervisorcore_flightplan.cpp: /api/v1/flightplan/*, /api/v1/readiness, pre-air readiness watcher
    void registerSuiteRoutes();                       // supervisorcore_suites.cpp: render, Star nodes, publisher, spot studio, agents
    void registerRedundancyRoutes();                // supervisorcore_redundancy.cpp: constructs m_sessions + m_redundancy; sessions, failover, remote nodes
    void applyMuxOutputs();                           // push UDP TS outputs for channel-fed mux services
    QJsonObject stationMediaRow(const sched::MediaRecord& m) const;
    QJsonObject stationChannelRow(const sched::ChannelProfile& p) const;
    QJsonObject cgManifest();
    void deviceCommandTick();
    void runDeviceCommand(const QString& channelId, const QJsonObject& cmd);
    void pushAlert(const QString& channelId, const QString& level, const QString& message);
    static QJsonObject err(int status, const QString& msg) { QJsonObject o; o["error"] = msg; o["status"] = status; return o; }
    static qint64 parseTimeMs(const QString& s, qint64 def);
};

#endif // SUPERVISORCORE_H
