#ifndef MESHCLIENT_H
#define MESHCLIENT_H

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
 * MeshClient — the Supervisor's door to the Star AN-FS mesh (multi-node media library).
 *
 * The AN-FS bridge exposes a small JSON API:
 *   GET  /api/nodes                         agents with drives and shared folders
 *   GET  /api/browse?node=<id>&path=<dir>   directory listing on any agent
 *   GET  /api/search?q=<text>               file-name search across the mesh (hash + node per hit)
 *   GET  /api/download?node=<id>&path=<f>   file bytes (Range supported, so transfers resume)
 *   POST /api/upload/init|chunk|complete    2 MiB chunked, resumable upload (mesh login required)
 *
 * Fetch jobs stream a remote file into `<downloadRoot>/<node>/<name>`, resume from a `.part` file,
 * and emit `fetched` so the Supervisor can ffprobe it into the media library. Push jobs run the
 * chunked upload against a node's drop folder (or a chosen folder). Settings keys:
 *   mesh.anfsUrl    bridge base URL (default empty: the feature is off until set, e.g. http://192.0.2.10:8080)
 *   mesh.anfsToken  bearer token for authenticated routes (uploads)
 *   mesh.anfsCookie raw Cookie header value (portal session) — alternative to the token
 */
class MeshClient : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;

    explicit MeshClient(sched::ScheduleStore* store, const QString& downloadRoot, QObject* parent = nullptr);
    ~MeshClient() override;

    QString baseUrl() const { return m_baseUrl; }
    void setConfig(const QJsonObject& cfg);          // {url?, token?, cookie?}
    QJsonObject status() const;

    void nodes(Reply reply);
    void browse(const QString& node, const QString& path, Reply reply);
    void search(const QString& q, const QString& node, Reply reply);

    QJsonObject startFetch(const QString& node, const QString& path, const QString& type, const QString& folder = QString());
    QJsonObject startPush(const QString& localPath, const QString& node, const QString& folder);
    QJsonArray jobs() const;
    bool cancel(const QString& jobId);
    void clearFinished();

signals:
    void fetched(const QString& localPath, const QString& type, const QString& node, const QString& remotePath);
    void jobsChanged();

private:
    struct Job {
        QString id, kind;          // fetch | push
        QString node, nodeName, remotePath, localPath, type, folder;
        QString state = "queued";  // queued | running | done | failed | cancelled
        qint64 bytes = 0, total = 0, startedMs = 0, finishedMs = 0;
        double rateBps = 0.0;
        QString error;
        QNetworkReply* reply = nullptr;
        QFile* file = nullptr;
        // push
        QString uploadId; int chunk = 0, totalChunks = 0; QList<int> have;
        QJsonObject toJson() const;
    };

    sched::ScheduleStore* m_store;
    QString m_downloadRoot, m_baseUrl, m_token, m_cookie;
    QNetworkAccessManager m_nam;
    QList<Job*> m_jobs;
    QHash<QString, QString> m_nodeNames;   // id -> hostname (from the last nodes() call)
    QTimer m_notify;
    QString m_lastError; qint64 m_lastOkMs = 0; int m_nodeCount = 0;

    QNetworkRequest request(const QString& path) const;
    void getJson(const QString& path, Reply reply);
    void runFetch(Job* j);
    void runPush(Job* j);
    void pushChunk(Job* j);
    void finish(Job* j, const QString& state, const QString& error = QString());
    void notify() { if (!m_notify.isActive()) m_notify.start(400); }
    static QString safeName(const QString& s);
};

#endif // MESHCLIENT_H
