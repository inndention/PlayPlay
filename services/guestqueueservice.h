#ifndef GUESTQUEUESERVICE_H
#define GUESTQUEUESERVICE_H

#include <QObject>
#include <QList>
#include <QJsonObject>

class GuestQueueService : public QObject
{
    Q_OBJECT

public:
    explicit GuestQueueService(QObject* parent = nullptr);
    
    void addGuest(const QString& guestId, const QString& name);
    void removeGuest(const QString& guestId);
    void moveGuest(const QString& guestId, int newPosition);
    QList<QJsonObject> getQueue() const;

signals:
    void queueChanged();
    void guestAdded(const QString& guestId);
    void guestRemoved(const QString& guestId);

private:
    QList<QJsonObject> m_queue;
};

#endif // GUESTQUEUESERVICE_H

