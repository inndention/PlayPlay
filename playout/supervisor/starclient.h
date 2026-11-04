#ifndef STARCLIENT_H
#define STARCLIENT_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QTimer>
#include <functional>

class QFile;
class QNetworkReply;
namespace sched { class ScheduleStore; }

/**
 * StarClient — talks to StarAgent daemons (D:\StarMedia\StarAgent, port 8780) on the Star mesh.
 *
 * A StarAgent is the per-node command daemon: bearer-token HTTP, bound to loopback and the tailnet.
 * We use its file-system surface for the multi-node media library and transfers:
 *   GET  /health /status /nowplaying /routes
 *   GET  /fs/list?path=   /fs/stat?path=   /fs/search?path=&q=&max=   /fs/hash?path=&algo=
 *   GET  /fs/download?path=            (Range supported → resumable fetch)
 *   POST /fs/upload/init {path,size,sha256?,overwrite?,mkdirs?} → PUT /fs/upload/:id?index=N → POST /fs/upload/:id/complete
 *   POST /exec {command, shell?, cwd?, timeoutMs?}   (used for ffprobe-on-the-node when asked)
 * Replies are {ok, data} or {ok:false, error:{code,message}}.
 *
 * Agents come from settings `star.agents` [{node, url, token, name}] merged with what this box can see:
 * D:\StarMedia\mesh\nodes.json (agent urls) and D:\StarMedia\StarAgent\agent-token (the local token).
 * Jobs share the shape of MeshClient's so the UI shows one transfer list.
 */
class StarClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;

    explicit StarClient(sched::ScheduleStore* store, const QString& downloadRoot, QObject* parent = nullptr);
    ~StarClient() override;

    QJsonArray agents() const;                      // configured + discovered (tokens hidden)
    void setAgents(const QJsonArray& agents);       // replaces the configured list (tokens kept when omitted)
    QJsonObject discoverLocal();                    // reads the StarMedia home on this box
    QString starHome() const { return m_home; }

    void call(const QString& node, const QString& method, const QString& path, const QJsonObject& body, Reply reply, int timeoutMs = 15000);
    void health(const QString& node, Reply reply);
    void status(const QString& node, Reply reply);
    void nowPlaying(const QString& node, Reply reply);
    void list(const QString& node, const QString& path, Reply reply);
    void search(const QString& node, const QString& path, const QString& q, Reply reply);
    void probeAll(Reply reply);                     // health of every agent, merged

    QJsonObject startFetch(const QString& node, const QString& path, const QString& type);
    QJsonObject startPush(const QString& localPath, const QString& node, const QString& remotePath, bool overwrite = false);
    QJsonArray jobs() const;
    bool cancel(const QString& jobId);
    void clearFinished();

signals:
    void fetched(const QString& localPath, const QString& type, const QString& node, const QString& remotePath);
    void jobsChanged();
    void agentsChanged();

private:
    struct Agent { QString node, name, url, token; bool configured = false; bool online = false; QString version, hostname, error; qint64 seenMs = 0; };
    struct Job {
        QString id, kind, node, nodeName, remotePath, localPath, type;
        QString state = "queued"; qint64 bytes = 0, total = 0, startedMs = 0, finishedMs = 0; double rateBps = 0; QString error;
        QNetworkReply* reply = nullptr; QFile* file = nullptr;
        QString uploadId, partPath; int chunk = 0, totalChunks = 0; qint64 chunkSize = 0; QList<int> have;
        QJsonObject toJson() const;
    };

    sched::ScheduleStore* m_store;
    QString m_downloadRoot, m_home;
    QNetworkAccessManager m_nam;
    QHash<QString, Agent> m_agents;
    QList<Job*> m_jobs;
    QTimer m_notify;

    void load();
    void persist();
    const Agent* agentFor(const QString& node) const;
    QNetworkRequest request(const Agent& a, const QString& path, int timeoutMs) const;
    static QJsonObject unwrap(const QByteArray& data, int http, const QString& netError, int* statusOut);
    void runFetch(Job* j);
    void runPush(Job* j);
    void pushChunk(Job* j);
    void finish(Job* j, const QString& state, const QString& error = QString());
    void notify() { if (!m_notify.isActive()) m_notify.start(400); }
};

#endif // STARCLIENT_H
