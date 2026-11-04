#ifndef SUPERVISORCLIENT_H
#define SUPERVISORCLIENT_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QSet>
#include <QElapsedTimer>
#include <QVariantMap>
#include "ui_ipc/ipctransport.h"
#include "supervisorclientworker.h"
#include "channellistmodel.h"

class QThread;
class QNetworkAccessManager;

/**
 * SupervisorClient — the operator app's connection to the Supervisor.
 *
 * - Connects to the Supervisor over IPC as a UI client (MSG_UI_QUERY_STATUS).
 * - If no Supervisor is running, launches one from next to the app
 *   (--standalone single-box install) and reconnects automatically.
 * - Mirrors channel telemetry, runlists, alerts and preview frames as Qt
 *   properties/signals for QML and the ControlService adapter.
 * - Sends transport/schedule commands (MSG_UI_COMMAND).
 *
 * The pipe itself lives on a worker thread (SupervisorClientWorker, perf-app PA-1): connecting and parsing never run
 * on the GUI thread, which takes finished messages in short slices. Preview pictures (PA-2, W8 + W11) arrive as binary
 * frames from a PS-5 Supervisor (JSON/base64 from an older one) and are kept as JPEG bytes (previewJpeg); the QML image
 * provider decodes each one at the size of the tile that shows it, on its own loader thread. supervisorStalled is true while
 * the pipe is up but the Supervisor has sent nothing for 3 s (or no pipe instance ever accepts our connect): main.qml
 * shows a banner and pauses the api.* GETs; one status query goes out when it answers again.
 *
 * No periodic status query (perf-app PA-4, W12 client side): the app used to send MSG_UI_QUERY_STATUS every 4 s, and
 * each one came back as a full snapshot plus every telemetry and both hours of every runlist (a Supervisor from before
 * PS-6), on top of the snapshot the Supervisor pushes anyway (every 2 s; since PS-6 on change and every 10 s). Now it
 * asks only when it needs to: at connect (the full first reply), after a stall, when no whole snapshot came for
 * snapshotStaleMs (a safety net that a pushing Supervisor never trips) and when QML calls refresh() after an edit.
 * A Supervisor with PS-6 answers a repeated query with the snapshot and only the runlists that changed since this
 * client last got them; runlist, telemetry, alert and schedule changes reach us as they happen. statusPollMs (or
 * PLAYPLAY_STATUS_POLL_MS) brings the old periodic query back. ipcCounters() counts what the link carries.
 *
 * Change signals are per thing that changed (perf-app PA-3, W10): the active* properties notify only for the active
 * channel's own data (activeState / activeRunlist used to follow every channel's telemetry / runlist, and activeMeters
 * every preview frame of every source, 50-100 re-conversions a second); activeMeters / metersChanged(ch) at most 15 Hz
 * per channel; `channels` only when the list changed. Getters hand out cached QVariantMaps (converted once per change),
 * and channelModel is the channel list as a model whose rows are updated in place (delegates are no longer rebuilt on
 * every snapshot).
 */
class SupervisorClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    Q_PROPERTY(bool launchedByUs READ launchedByUs NOTIFY connectedChanged)
    Q_PROPERTY(QVariantMap snapshot READ snapshot NOTIFY snapshotChanged)
    Q_PROPERTY(QString activeChannelId READ activeChannelId WRITE setActiveChannelId NOTIFY activeChannelChanged)
    Q_PROPERTY(QVariantMap activeState READ activeState NOTIFY activeStateChanged)
    Q_PROPERTY(QString restUrl READ restUrl NOTIFY connectedChanged)
    Q_PROPERTY(int previewSerial READ previewSerial NOTIFY previewChanged)
    Q_PROPERTY(QVariantList alerts READ alertsList NOTIFY alertsChanged)
    Q_PROPERTY(QVariantList recentAsRun READ recentAsRunList NOTIFY asRunChanged)
    Q_PROPERTY(QVariantMap activeRunlist READ activeRunlist NOTIFY activeRunlistChanged)
    Q_PROPERTY(QVariantList channels READ channelsList NOTIFY channelsChanged)
    Q_PROPERTY(QVariantMap activeMeters READ activeMeters NOTIFY activeMetersChanged)
    // The active channel's output mixes as [{id, name, follow}] without their levels: changes only when a mix is added,
    // removed or renamed, so Repeaters over it (MIX tiles, the graphics target picker) are not rebuilt at the meter rate.
    Q_PROPERTY(QVariantList activeMixes READ activeMixes NOTIFY activeMixesChanged)
    // The active channel's snapshot entry and two parts of it, each notified only when that part changed.
    Q_PROPERTY(QVariantMap activeChannelData READ activeChannelData NOTIFY activeChannelDataChanged)
    Q_PROPERTY(QVariantMap activeProfile READ activeProfile NOTIFY activeProfileChanged)
    Q_PROPERTY(QVariantMap activeExecutor READ activeExecutor NOTIFY activeExecutorChanged)
    Q_PROPERTY(QObject* channelModel READ channelModelObject CONSTANT)
    Q_PROPERTY(bool supervisorStalled READ supervisorStalled NOTIFY supervisorStalledChanged)
    // While stalled: {reason: "silent"|"busy", silentMs, sinceMs (epoch), health: {state, activity, lagMs, ...} from the
    // Supervisor's /healthz when it answers, healthError otherwise}. Refreshed once a second while stalled.
    Q_PROPERTY(QVariantMap stallInfo READ stallInfo NOTIFY stallInfoChanged)

public:
    explicit SupervisorClient(QObject* parent = nullptr);
    ~SupervisorClient() override;

    struct Options {
        QString ipcName = "PlayPlaySupervisor";
        QString executable;          // default: <appdir>/PlayPlaySupervisor.exe
        QStringList arguments;       // extra args for a launched supervisor
        QString dataDir;             // --data-dir for a launched supervisor
        int restPort = 7700;
        bool launchIfMissing = true;
        int connectTimeoutMs = 3000; // async connect budget while every pipe instance is busy
        int stallMs = SupervisorClientWorker::kDefaultStallMs;   // no IPC message for this long with the pipe up = stalled
        // Supervisor watchdog health endpoint (GET /healthz), asked only while stalled. -1 = auto: 7709 for the default
        // REST port 7700 (setting watchdog.port), REST port + 2 otherwise (the lane test Supervisor: 7797 -> 7799); 0 = off.
        int healthPort = -1;
        // Preview JPEGs as binary IPC frames from a PS-5 Supervisor (PA-2, W8); false = ask for the JSON/base64 form
        // (an older Supervisor sends that whatever we ask). PLAYPLAY_IPC_BINARY=0 in the environment turns it off.
        bool binaryPreviews = true;
        // Periodic MSG_UI_QUERY_STATUS: 0 = none (PA-4), > 0 = every that many ms (the old 4000; a rollback knob), -1 =
        // from the environment variable PLAYPLAY_STATUS_POLL_MS (unset or invalid = 0).
        int statusPollMs = -1;
        // With the pipe up and not stalled, no whole snapshot for this long -> one query (0 = never). The Supervisor
        // pushes one at least every 10 s (2 s before PS-6), so this only catches a snapshot that never came.
        int snapshotStaleMs = 30000;
    };
    enum QueryReason { QueryConnect, QueryRecover, QueryStale, QueryPoll, QueryOperator, QueryReasonCount };

    void start(const Options& opt);
    void stop();

    bool isConnected() const { return m_connected; }
    bool launchedByUs() const { return m_process != nullptr; }
    QVariantMap snapshot() const;
    QString activeChannelId() const { return m_active; }
    void setActiveChannelId(const QString& id);
    QVariantMap activeState() const { ++m_reads.activeState; return mapOf(m_telemetry, m_active); }
    QString restUrl() const { return QString("http://127.0.0.1:%1").arg(m_opt.restPort); }
    bool supervisorStalled() const { return m_stalled; }
    QVariantMap stallInfo() const { return m_stallInfo; }
    int healthPort() const;

    QJsonObject telemetry(const QString& channelId) const { return jsonOf(m_telemetry, channelId); }
    QJsonObject runlist(const QString& channelId) const { return jsonOf(m_runlists, channelId); }
    QJsonObject meters(const QString& channelId) const { return jsonOf(m_meters, channelId); }
    // Thread-safe: the QML image provider asks from its loader thread (ProgramImage uses asynchronous images).
    QByteArray previewJpeg(const QString& channelId) const { QMutexLocker lk(&m_previewMu); return m_previewJpegs.value(channelId); }
    QImage preview(const QString& channelId) const;   // the latest picture decoded at full size (tests, tools); tiles use previewJpeg
    QStringList channelIds() const { return m_model->ids(); }
    int previewSerial() const { return m_previewSerial; }
    QVariantList alertsList() const { return m_alerts; }
    QVariantList recentAsRunList() const { return m_asRun; }
    QVariantMap activeRunlist() const { ++m_reads.activeRunlist; return mapOf(m_runlists, m_active); }
    QVariantList channelsList() const { ++m_reads.channels; return m_model->toVariantList(); }
    QVariantMap activeMeters() const { ++m_reads.activeMeters; return mapOf(m_meters, m_active); }
    QVariantList activeMixes() const { return m_activeMixes; }
    QVariantMap activeChannelData() const { ++m_reads.activeChannelData; return m_model->channel(m_active); }
    QVariantMap activeProfile() const { return m_model->channel(m_active).value(QStringLiteral("profile")).toMap(); }
    QVariantMap activeExecutor() const { return m_model->channel(m_active).value(QStringLiteral("executor")).toMap(); }
    ChannelListModel* channelModel() const { return m_model; }
    QObject* channelModelObject() const { return m_model; }
    Q_INVOKABLE QVariantMap metersOf(const QString& channelId) const { return mapOf(m_meters, channelId); }
    Q_INVOKABLE QVariantMap telemetryOf(const QString& channelId) const { return mapOf(m_telemetry, channelId); }
    Q_INVOKABLE QVariantMap runlistOf(const QString& channelId) const { return mapOf(m_runlists, channelId); }
    Q_INVOKABLE QVariantMap channelOf(const QString& channelId) const { return m_model->channel(channelId); }
    Q_INVOKABLE void cg(const QVariantMap& cmd, const QString& channelId = {}) { command("cg", cmd, channelId); }
    Q_INVOKABLE void clearAlerts() { m_alerts.clear(); emit alertsChanged(); }
    // One status query now (QML calls it after an edit); skipped while stalled or disconnected.
    Q_INVOKABLE void refresh();

    // Commands (QML-callable). channelId empty = active channel.
    Q_INVOKABLE void command(const QString& action, const QVariantMap& params = {}, const QString& channelId = {});
    Q_INVOKABLE void play(const QString& channelId = {}) { command("play", {}, channelId); }
    Q_INVOKABLE void pause(const QString& channelId = {}) { command("pause", {}, channelId); }
    Q_INVOKABLE void stop(const QString& channelId) { command("stop", {}, channelId); }
    Q_INVOKABLE void skip(const QString& channelId = {}) { command("skip", {}, channelId); }
    Q_INVOKABLE void hold(const QString& channelId = {}) { command("hold", {}, channelId); }
    Q_INVOKABLE void take(const QString& path, const QString& title = {}, double duration = 0.0, const QString& channelId = {});
    Q_INVOKABLE void arm(bool armed, const QString& channelId = {});
    Q_INVOKABLE void loadHour(const QString& channelId = {}) { command("loadhour", {}, channelId); }
    Q_INVOKABLE void snap(const QString& channelId = {}) { command("snap", {}, channelId); }
    Q_INVOKABLE void seek(double seconds, const QString& channelId = {});

    // Tests and diagnostics: the worker's counters (received, coalesced, ...), 0s when stopped.
    SupervisorClientWorker::Stats ipcStats() const { return m_worker ? m_worker->stats() : SupervisorClientWorker::Stats{}; }
    quint64 messagesApplied() const { return m_applied; }
    // What the GUI side told QML (PA-3): notifications per property and getter reads (each read is one binding
    // evaluation or one JS call). With PLAYPLAY_UI_STATS_SEC=<n> the app logs the rates every n seconds.
    struct UiStats {
        quint64 previews = 0;           // previewChanged (the old NOTIFY of activeMeters)
        quint64 metersIn = 0, metersSame = 0, metersEmitted = 0;   // meters received / identical to the last / metersChanged
        quint64 activeMeters = 0, activeMixes = 0, activeState = 0, activeRunlist = 0;
        quint64 channels = 0, activeChannelData = 0, snapshots = 0, snapshotsSame = 0;
    };
    struct UiReads { quint64 activeMeters = 0, activeState = 0, activeRunlist = 0, channels = 0, activeChannelData = 0, snapshot = 0; };
    UiStats uiStats() const { return m_ui; }
    UiReads uiReads() const { return m_reads; }
    Q_INVOKABLE QVariantMap uiCounters() const;
    // The link (PA-4): status queries sent, per reason and in all, and what the Supervisor sent (the worker's message
    // and byte counters, time sync included).
    quint64 queriesSent(QueryReason why) const { return why >= 0 && why < QueryReasonCount ? m_queries[why] : 0; }
    quint64 queriesSent() const { quint64 n = 0; for (quint64 q : m_queries) n += q; return n; }
    int statusPollMs() const { return m_opt.statusPollMs; }
    Q_INVOKABLE QVariantMap ipcCounters() const;

    static constexpr int kMeterIntervalMs = 67;   // metersChanged / activeMetersChanged at most 15 Hz per channel

signals:
    void connectedChanged();
    void snapshotChanged();
    void activeChannelChanged();
    void telemetryChanged(const QString& channelId);
    void stateChanged(const QString& channelId, const QString& state, const QString& reason);
    void runlistChanged(const QString& channelId);
    void asRun(const QString& channelId, const QJsonObject& row);
    void alert(const QString& channelId, const QString& level, const QString& message);
    void previewChanged(const QString& channelId);
    void metersChanged(const QString& channelId);   // that channel's meters changed; at most 15 Hz per channel, newest value
    void activeStateChanged();
    void activeRunlistChanged();
    void activeMetersChanged();
    void activeMixesChanged();
    void channelsChanged();
    void activeChannelDataChanged();
    void activeProfileChanged();
    void activeExecutorChanged();
    void scheduleChanged(const QString& channelId, const QString& slotId);
    void alertsChanged();
    void asRunChanged();
    void mediaScanDone();
    void transcriptReady(const QString& mediaId);
    void uiEvent(const QString& type, const QJsonObject& payload);   // ingest, meshJobs, mux, cameras, cgHtml, transcribe...
    void supervisorStalledChanged();
    void stallInfoChanged();

private:
    static constexpr int kGuiSliceMs = 8;   // longest stretch of message handling per GUI loop turn

    Options m_opt;
    QThread* m_thread = nullptr;
    SupervisorClientWorker* m_worker = nullptr;   // lives on m_thread
    QProcess* m_process = nullptr;
    QTimer m_reconnect;
    bool m_connected = false;
    bool m_connecting = false;
    bool m_triedLaunch = false;
    QString m_lastConnectError;
    QString m_active = "main";
    // A JSON object and its QVariantMap, converted on the first read after a change: QML reads the same map from many
    // bindings, and every getter used to convert it again on each read.
    struct CachedJson {
        QJsonObject json;
        mutable QVariantMap map;
        mutable bool valid = false;
        void set(const QJsonObject& o) { json = o; valid = false; map.clear(); }
        const QVariantMap& variant() const { if (!valid) { map = json.toVariantMap(); valid = true; } return map; }
    };
    static QVariantMap mapOf(const QHash<QString, CachedJson>& h, const QString& id) { const auto it = h.constFind(id); return it == h.cend() ? QVariantMap() : it->variant(); }
    static QJsonObject jsonOf(const QHash<QString, CachedJson>& h, const QString& id) { const auto it = h.constFind(id); return it == h.cend() ? QJsonObject() : it->json; }
    QJsonObject m_snapshot;
    mutable QVariantMap m_snapshotMap;
    mutable bool m_snapshotMapValid = false;
    ChannelListModel* m_model = nullptr;   // child; rows follow m_snapshot["channels"]
    QHash<QString, CachedJson> m_telemetry;
    QHash<QString, CachedJson> m_runlists;
    mutable QMutex m_previewMu;   // guards m_previewJpegs only
    QHash<QString, QByteArray> m_previewJpegs;   // the latest JPEG per preview key, as sent (decoded by PreviewImageProvider)
    QHash<QString, CachedJson> m_meters;
    // metersChanged throttle: a channel's newest meters go out at once when its last emission is >= 67 ms old, else
    // when its slot comes (one precise timer for all channels), so the latest value always arrives.
    QElapsedTimer m_clock;
    QHash<QString, qint64> m_meterLastMs;
    QSet<QString> m_meterDue;
    QTimer m_meterTimer;
    QJsonArray m_activeMixesJson;
    QVariantList m_activeMixes;
    UiStats m_ui;
    mutable UiReads m_reads;
    QTimer m_uiStatsLog;
    UiStats m_uiLogged;
    UiReads m_readsLogged;
    SupervisorClientWorker::Stats m_ipcLogged;
    // Status queries (PA-4): the periodic poll when statusPollMs > 0, else the stale-snapshot check.
    QTimer m_refresh;
    qint64 m_lastSnapshotMs = 0;          // m_clock ms of the last whole snapshot applied (or query sent)
    quint64 m_queries[QueryReasonCount] = {};
    quint64 m_queriesLogged = 0;
    int m_previewSerial = 0;
    QVariantList m_alerts, m_asRun;
    quint64 m_applied = 0;

    // Stall state (GUI side mirror of the worker's verdict) and the /healthz probe.
    bool m_stalled = false;
    bool m_stallBusy = false;
    qint64 m_stallSinceMs = 0;
    QVariantMap m_stallInfo;
    QTimer m_stallTick;
    int m_stallTicks = 0;
    QNetworkAccessManager* m_nam = nullptr;
    bool m_healthInFlight = false;
    QVariantMap m_health;
    QString m_healthError;

    void startWorker();
    void stopWorker();
    void tryConnect();
    void launchSupervisor();
    void drainInbox();
    void apply(const SupervisorClientWorker::Delivery& d);
    void onConnectionState(bool up);
    void onConnectFailed(const QString& reason, bool busy);
    void onStalledChanged(bool stalled, qint64 silentMs, bool busy);
    void updateStallInfo();
    void probeHealth();
    void sendToWorker(const IpcProtocol::Message& msg);
    void query(QueryReason why);
    void onRefreshTimer();
    void applyChannelChanges(const QHash<QString, int>& changes);
    void onMeters(const QString& channelId, const QJsonObject& meters);
    void emitMeters(const QString& channelId, qint64 nowMs);
    void onMeterTimer();
    void updateActiveMixes();
    void logUiStats();
};

#endif // SUPERVISORCLIENT_H
