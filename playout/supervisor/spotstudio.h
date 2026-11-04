#ifndef SPOTSTUDIO_H
#define SPOTSTUDIO_H

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

class SpotGenerator;
class RenderFarm;
class Publisher;
class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * SpotStudio — guided spot creation (docs/spot-studio.md): brief → concepts + timed script → edits → render
 * (local SpotGenerator pipeline or a spot-node via RenderFarm) → review → publish into ad rotation
 * (media un-archived + ad contract in traffic.contracts) and optionally a social draft.
 * Also: revise an auto-generated spot with an editor's note (PlayPlay OBS newsroom revise).
 */
class SpotStudio : public QObject {
    Q_OBJECT
public:
    using JsonCb = std::function<void(const QJsonObject&)>;
    SpotStudio(sched::ScheduleStore* store, SpotGenerator* spots, RenderFarm* farm, const QString& dataDir, const QString& studioDir, QObject* parent = nullptr);
    void setPublisher(Publisher* p) { m_pub = p; }

    void options(JsonCb cb);
    QJsonArray projects() const;
    QJsonObject project(const QString& id, bool* found = nullptr) const;
    QJsonObject create(const QJsonObject& body, const QString& actor = "operator");
    QJsonObject update(const QString& id, const QJsonObject& body, const QString& actor = "operator");
    bool remove(const QString& id);
    QJsonObject generate(const QString& id, const QJsonObject& body, const QString& actor = "operator");
    QJsonObject review(const QString& id, const QString& action, const QString& notes, const QString& actor = "operator");
    QJsonObject publish(const QString& id, const QJsonObject& body, const QString& actor = "operator");
    QJsonObject reviseAutoSpot(const QString& mediaId, const QString& notes, const QString& voice);

    static QStringList voices();

signals:
    void event(const QString& type, const QJsonObject& payload);

private:
    sched::ScheduleStore* m_store;
    SpotGenerator* m_spots;
    RenderFarm* m_farm;
    Publisher* m_pub = nullptr;
    SuiteHttp* m_http;
    QString m_dataDir, m_studioDir;
    QHash<QString, QJsonObject> m_live;                // project id -> live render progress {pct, step} (not persisted)

    QJsonArray all() const;
    QJsonObject save(QJsonObject p);
    void history(QJsonObject& p, const QString& actor, const QString& action, const QString& note = QString()) const;
    void ensureRecipe();
    void redraft(QJsonObject& p, int concept) const;
    void llmRewrite(const QString& id, const QString& instruction, const QString& actor);
    void onRendered(const QString& id, bool ok, const QJsonObject& res, const QString& engine);
    QJsonArray musicBeds() const;
};

#endif // SPOTSTUDIO_H
