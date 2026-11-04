#ifndef AUTOPUBLISHSERVICE_H
#define AUTOPUBLISHSERVICE_H

#include <QObject>
#include <QNetworkAccessManager>

class AutoPublishService : public QObject
{
    Q_OBJECT

public:
    explicit AutoPublishService(QObject* parent = nullptr);
    
    void publishToFacebook(const QString& message, const QString& link = QString());
    void startFacebookLive();
    void stopFacebookLive();
    
    // TODO: Implement more auto-publish functionality

signals:
    void publishSucceeded(const QString& platform, const QString& postId);
    void publishFailed(const QString& platform, const QString& error);

private:
    QNetworkAccessManager* m_networkManager;
};

#endif // AUTOPUBLISHSERVICE_H

