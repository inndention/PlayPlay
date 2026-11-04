#ifndef WEBRTCSESSIONMANAGER_H
#define WEBRTCSESSIONMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>

class WebRtcSessionManager : public QObject
{
    Q_OBJECT

public:
    explicit WebRtcSessionManager(QObject* parent = nullptr);
    
    void createSession(const QString& sessionId);
    void closeSession(const QString& sessionId);
    QStringList getActiveSessions() const;

signals:
    void sessionCreated(const QString& sessionId);
    void sessionClosed(const QString& sessionId);
    void sessionError(const QString& sessionId, const QString& error);
};

#endif // WEBRTCSESSIONMANAGER_H

