#ifndef AIMODELS_H
#define AIMODELS_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * AiModels — the one place every OpenRouter caller gets its key, base URL and model (agent assistant, spot studio
 * writer, spot generator LLM writer), plus the model catalog behind GET /api/v1/ai/models.
 *
 * Key: the OPENROUTER_API_KEY environment variable; a key saved in settings ai.config.apiKey overrides it
 * (keySource "settings"). Keys are never returned by any route. Model: a feature's own setting if it names a current
 * model, else ai.config.model, else suitelogic::defaultAiModel(); legacy ids (gpt-4o, claude-3, ...) are never used.
 * Catalog: https://openrouter.ai/api/v1/models, shaped by suitelogic::filterModelCatalog, cached in memory and in
 * settings ai.catalog, refreshed every 6 h or on demand; unreachable → cached copy → built-in list.
 */
class AiModels : public QObject {
    Q_OBJECT
public:
    using JsonCb = std::function<void(const QJsonObject&)>;
    explicit AiModels(sched::ScheduleStore* store, QObject* parent = nullptr);

    static QString apiKey(const sched::ScheduleStore* store, QString* source = nullptr);
    static QString baseUrl(const sched::ScheduleStore* store);
    static QString model(const sched::ScheduleStore* store, const QString& featureModel = QString());
    static QJsonObject keyStatus(const sched::ScheduleStore* store);   // {hasKey, keySource: env|settings|none, envVar, model, baseUrl}
    static void migrate(sched::ScheduleStore* store);                 // legacy models -> default; per-feature saved keys -> ai.config

    QJsonObject config() const;
    QJsonObject setConfig(const QJsonObject& patch);
    void models(bool all, bool refresh, JsonCb cb);

private:
    sched::ScheduleStore* m_store;
    SuiteHttp* m_http;
    QJsonArray m_catalog;          // shaped, all ages (legacy already removed)
    qint64 m_fetchedMs = 0;
    QString m_source;              // live | cache | builtin
    QList<std::function<void()>> m_waiting;
    bool m_fetching = false;
    QJsonObject answer(bool all) const;
};

#endif // AIMODELS_H
