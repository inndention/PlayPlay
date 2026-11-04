#ifndef PUBLISHER_H
#define PUBLISHER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <functional>

#include "pubrails.h"

class SuiteHttp;
namespace sched { class ScheduleStore; }

/**
 * Publisher — social / web publishing: drafts, the review ladder, scheduling, sending, corrections, retractions,
 * metrics and a kill switch (docs/publisher.md). Port of PlayPlay OBS social.py + publisher.py.
 *
 * Nothing leaves the station without an approved item; video items also need their library media approved;
 * absolute-tier subjects always wait for a person (suitelogic::classifyDraft). A published item is never
 * edited: a correction is a new item that points at it (`corrects`).
 *
 * Hard rails (pubrails.h, ST-30): the approval is bound to {revision, content sha256}; per-channel hour / day caps and
 * a minimum gap (Facebook 4/h, 15/day, 10 min, code constants); every channel is persisted as 'publishing' before its
 * network call; an unsure outcome (no answer, 5xx, transient Graph error, restart mid-call) puts the item in
 * 'reconcile', which looks at the Page / site instead of posting again; only definitive refusals retry, with backoff.
 */
class Publisher : public QObject {
    Q_OBJECT
public:
    using Reply = std::function<void(int status, const QJsonObject& body)>;
    explicit Publisher(sched::ScheduleStore* store, QObject* parent = nullptr);
    ~Publisher() override;
    void start();
    static Publisher* instance();                      // the Supervisor's Publisher (rails routes are added through laneext)

    void reconcile(const QString& id, const QString& actor, Reply reply);   // look at the platform for a channel whose outcome is unknown
    QJsonObject rails() const;                         // the hard rails: limits, per-channel counts, when the next post may go

    QJsonObject config(bool withSecrets = false) const;
    QJsonObject setConfig(const QJsonObject& patch);
    QJsonObject status() const;
    void verify(Reply reply);

    QJsonArray queue(const QString& status = QString(), const QString& kind = QString(), const QString& source = QString(), const QString& channel = QString(), int limit = 300) const;
    QJsonObject item(const QString& id, bool* found = nullptr) const;
    QJsonObject draft(const QJsonObject& body, const QString& actor = "operator");
    QJsonObject act(const QString& id, const QString& action, const QJsonObject& body, const QString& actor = "operator");
    void send(const QString& id, const QString& actor, Reply reply);
    void metrics(const QString& id, Reply reply);
    QJsonObject stop(const QString& reason, const QString& scope, const QString& actor = "operator");
    QJsonObject release(const QString& scope, const QString& actor = "operator");
    QJsonArray audit(int limit = 200) const;
    void considerSpot(const QString& mediaId);     // ingest hook: finished auto spots, per mode / recipes / day cap
    QJsonObject pulse() const;

    // PlayPlay OBS station-compatible shapes
    QJsonObject socialConfig() const;
    QJsonObject saveSocialConfig(const QJsonObject& body);
    QJsonArray socialQueue(bool includeDone) const;
    QJsonObject socialEnqueue(const QJsonObject& body);
    QJsonObject publishConfig() const;
    QJsonArray publications(int limit = 200) const;
    static QJsonObject socialRow(const QJsonObject& item);

signals:
    void event(const QString& type, const QJsonObject& payload);
    void alert(const QString& level, const QString& message);

private:
    sched::ScheduleStore* m_store;
    SuiteHttp* m_http;
    QTimer m_worker;
    QSet<QString> m_sending;
    QSet<QString> m_reconciling;                        // items with a reconcile read in flight
    QJsonObject m_page;                                 // last verified page {name, followers}

    QJsonArray items() const;
    void saveItems(const QJsonArray& a);
    int indexOf(const QJsonArray& a, const QString& id) const;
    QJsonObject putItem(const QJsonObject& item);
    void history(QJsonObject& item, const QString& actor, const QString& action, const QString& note = QString()) const;
    void logAudit(const QString& id, const QString& actor, const QString& action, const QString& note, int revision);
    QString mediaBlock(const QString& mediaId) const;  // why a video item cannot go out, empty when it can
    bool killed(const QString& channel) const;
    void tick();
    void sendChannel(const QString& id, const QString& channel, std::function<void(const pubrails::SendResult& r, const QJsonObject& res)> cb);
    void finishSend(const QString& id, const QString& actor, Reply reply);
    void notify(const QJsonObject& item = QJsonObject());

    // rails (pubrails.h)
    QString mediaFingerprint(const QJsonObject& item) const;                       // media file name + size: a new render is new content
    QString outgoingText(const QJsonObject& item, const QString& channel, const QJsonObject& cfg) const;   // exactly what a channel receives
    /// Why the item may not be sent to `channels` now (empty when it may). *code: 409 busy / stale approval, 429 rate rail; *stale: approval no longer covers it.
    QString railBlock(const QJsonObject& item, const QStringList& channels, int* code, bool* stale) const;
    bool inGroup(const QString& status, const QString& group) const;               // suitelogic groups plus 'reconcile' (open, needs_attention)
    void recoverInterrupted();                                                     // start-up: 'publishing' left by a stop -> unknown -> reconcile
    void reconcileTick();
    void reconcileChannel(const QString& id, const QString& channel, const QString& actor, std::function<void(int status, const QJsonObject& body)> done);
    void settleItem(QJsonObject& item, const QString& actor, const QString& action, const QString& note);   // status from the channels after reconcile / resolve
    QJsonObject resolve(const QString& id, const QJsonObject& body, const QString& actor);
};

#endif // PUBLISHER_H
