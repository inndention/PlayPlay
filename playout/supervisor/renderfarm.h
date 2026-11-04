#ifndef RENDERFARM_H
#define RENDERFARM_H

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QTimer>
#include <functional>

class QProcess;
class StarClient;
class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * RenderFarm — render jobs dispatched to render nodes, with the output delivered back into the media library.
 * Port of PlayPlay OBS mesh_render.py + studio_render.py and the Star split render (docs/render-nodes.md).
 *
 * Node kinds: spotnode (StarMedia Jobs API v1: POST /v1/jobs, poll, GET /v1/jobs/{id}/result), playplay (another
 * station: POST /api/newsroom/spot with a durable request_id, reconcile by GET /api/newsroom/render-request/{id}),
 * staragent (StarAgent /exec runs a split-render segment on the node, Star fetch brings it back) and local
 * (ffmpeg processes on this box, at most render.config.maxLocal at once).
 *
 * Job kinds: spot, transcode, split. Jobs persist in settings render.jobs; a spot job that was rendering on a
 * spot-node when the Supervisor stopped resumes polling at the next start (the Idempotency-Key is the requestId).
 */
class RenderFarm : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(bool ok, const QJsonObject& result)>;
    using IngestFn = std::function<void(const QString& path, const QString& type, const QJsonObject& meta)>;
    using LocalSpotFn = std::function<QJsonObject(const QJsonObject& req, Done done)>;   // SpotGenerator::generate
    using JsonCb = std::function<void(const QJsonObject&)>;

    RenderFarm(sched::ScheduleStore* store, StarClient* star, const QString& dataDir, const QString& outDir,
               const QString& ffmpeg, const QString& ffprobe, QObject* parent = nullptr);
    ~RenderFarm() override;
    void setIngest(IngestFn f) { m_ingest = std::move(f); }
    void setLocalSpot(LocalSpotFn f) { m_localSpot = std::move(f); }
    void start();

    // --- configuration + nodes ---------------------------------------------------------------------
    QJsonObject config() const;
    QJsonObject setConfig(const QJsonObject& patch);
    QJsonArray nodes(bool withSecrets = false) const;          // configured + discovered, with health; tokens hidden unless asked
    QJsonObject node(const QString& id, bool* found = nullptr, bool withSecrets = false) const;
    QJsonObject saveNode(const QJsonObject& body, const QString& id = QString());
    QJsonArray setNodes(const QJsonArray& nodes);
    bool deleteNode(const QString& id);
    void probe(const QString& id, JsonCb cb);
    void probeAll(JsonCb cb);
    void templates(JsonCb cb);
    QJsonObject status() const;

    // --- jobs ----------------------------------------------------------------------------------------
    QJsonArray jobs(const QString& state = QString(), int limit = 200) const;
    QJsonObject job(const QString& id, bool* found = nullptr) const;
    QJsonObject submit(const QJsonObject& body, Done done = Done());
    bool cancel(const QString& id);
    bool remove(const QString& id);
    int clearFinished();

    // --- PlayPlay OBS station-compatible newsroom routes ---------------------------------------------
    void studioHealth(JsonCb cb);                                // GET /api/newsroom/studio
    QJsonObject renderRequest(const QString& requestId) const;   // GET /api/newsroom/render-request/{id}
    void newsroomSpot(const QJsonObject& body, std::function<void(int status, const QJsonObject&)> reply);   // POST /api/newsroom/spot

    void onMediaIngested(const QString& mediaId, const QString& origin);

signals:
    void event(const QString& type, const QJsonObject& payload);
    void alert(const QString& level, const QString& message);

private:
    struct Seg { QJsonObject o; QProcess* proc = nullptr; };
    struct Job {
        QJsonObject o;                                 // the public record
        QProcess* proc = nullptr;
        QList<Seg> segs;
        QString workDir;
        double durationSec = 0;
        qint64 deadlineMs = 0;
        int pollFails = 0;
        Done done;
    };
    sched::ScheduleStore* m_store;
    StarClient* m_star;
    SuiteHttp* m_http;
    QString m_dataDir, m_outDir, m_ffmpeg, m_ffprobe;
    IngestFn m_ingest;
    LocalSpotFn m_localSpot;
    QHash<QString, QJsonObject> m_health;              // node id -> last health
    QList<Job*> m_jobs;                                // newest last
    QTimer m_tick, m_persist, m_notify, m_healthTimer;
    QJsonArray m_templates; qint64 m_templatesMs = 0; QString m_templatesNode;
    QHash<QString, QString> m_pendingIngest;           // output path -> job id (waiting for the library record)

    // nodes
    QJsonArray configuredNodes() const;
    QJsonObject discoveredSpotNode() const;
    QString tokenFor(const QJsonObject& node) const;
    QList<QPair<QByteArray, QByteArray>> authHeaders(const QJsonObject& node) const;
    QJsonArray nodesForChoice() const;
    // jobs
    Job* find(const QString& id) const;
    void load();
    void pump();
    int localRunning() const;
    void startJob(Job* j);
    void startSpot(Job* j);
    void startSpotNode(Job* j, const QJsonObject& node);
    void pollSpotNode(Job* j);
    void fetchSpotNodeResult(Job* j, const QJsonObject& remote);
    void startPeerSpot(Job* j, const QJsonObject& node);
    void reconcilePeer(Job* j, const QJsonObject& node, int attempt);
    void startLocalSpot(Job* j);
    void startTranscode(Job* j);
    void startSplit(Job* j);
    void runSegments(Job* j);
    void startSegment(Job* j, int index);
    void concatSegments(Job* j);
    void deliver(Job* j, const QString& path);
    void finish(Job* j, const QString& state, const QString& error = QString(), const QJsonObject& extra = QJsonObject());
    void setStep(Job* j, const QString& step, double pct = -1);
    void logLine(Job* j, const QString& line);
    void probeDuration(const QString& path, std::function<void(double)> cb);
    QString resolveInput(const QString& input) const;
    QJsonObject spotPayload(const QJsonObject& body, const QString& requestId) const;
    void changed() { if (!m_persist.isActive()) m_persist.start(1000); if (!m_notify.isActive()) m_notify.start(400); }
    void persist();
    QJsonObject counts() const;
};

#endif // RENDERFARM_H
