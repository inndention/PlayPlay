#ifndef CGDESIGNER_H
#define CGDESIGNER_H

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QString>
#include <functional>

class RestApiServer;
class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * CgDesigner — the CG Designer's store, page files and AI jobs (docs/cg-designer.md); the pure helpers are cgdesign.h.
 *
 *   * Templates: settings cgdesigner.index {ids[]} + cgdesigner.t.<id> (the record, last 10 versions), cached in memory.
 *     Each save writes <webRoot>/cg/<id>.html; start() writes any page that is missing or stale (a wiped build\web).
 *   * Manifest: mergeInto() appends the designs to web/cg/templates.json as served by GET /api/v1/cg/templates
 *     (category "custom", custom:true), so the overlay manager deploys them like pack templates.
 *   * Generate: POST /cg/designer/generate starts an async job of 1-3 parallel chat/completions calls (OpenRouter key,
 *     base URL and model from AiModels); GET /cg/designer/jobs/{id} reports it. The last 20 jobs are kept in memory.
 */
class CgDesigner : public QObject {
    Q_OBJECT
public:
    CgDesigner(sched::ScheduleStore* store, const QString& webRoot, std::function<QJsonObject()> manifest, QObject* parent = nullptr);
    ~CgDesigner() override;

    static CgDesigner* instance();
    /// `manifest` with the designs appended (no-op without an instance). Cheap: reads the in-memory cache.
    static QJsonObject mergeInto(const QJsonObject& manifest);

    void start();
    void registerRoutes(RestApiServer* r);

    // --- REST (an object holding `error` + `status` is an error reply) ---
    QJsonObject overview() const;
    QJsonObject config() const;
    QJsonObject setConfig(const QJsonObject& body);
    QJsonObject get(const QString& id) const;
    QJsonObject save(const QJsonObject& body);
    QJsonObject remove(const QString& id);
    QJsonObject restore(const QString& id, const QJsonObject& body);
    QJsonObject generate(const QJsonObject& body);
    QJsonObject job(const QString& id) const;

signals:
    void changed(const QJsonObject& payload);     // {kind: saved|removed|job, id, ...}

private:
    struct Job {
        QString id, state = "running", model, error, mode;
        qint64 startedMs = 0, endedMs = 0;
        int pending = 0, total = 0;
        QJsonArray drafts, errors;
        QJsonObject usage;
    };
    QJsonObject manifestEntries(const QJsonObject& manifest) const;
    QJsonObject summary(const QJsonObject& rec) const;
    QString pagePath(const QString& id) const;
    bool writePage(const QString& id, const QString& html, QString* err = nullptr) const;
    void persist(const QJsonObject& rec);
    void persistIndex();
    QString exampleHtml(const QString& templateId) const;
    void finishDraft(const QString& jobId, int variation, int http, const QJsonObject& j, const QByteArray& raw, const QString& netErr);

    sched::ScheduleStore* m_store;
    QString m_webRoot;
    std::function<QJsonObject()> m_manifest;
    SuiteHttp* m_http;
    QStringList m_order;                           // ids, oldest first (cgdesigner.index)
    QHash<QString, QJsonObject> m_recs;            // id -> record
    QMap<qint64, QString> m_jobOrder;              // seq -> job id
    QHash<QString, Job> m_jobs;
    qint64 m_jobSeq = 0;
    static QPointer<CgDesigner> s_instance;
};

#endif // CGDESIGNER_H
