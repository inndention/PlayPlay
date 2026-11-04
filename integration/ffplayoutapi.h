#ifndef FFPLAYOUTAPI_H
#define FFPLAYOUTAPI_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFuture>
#include <QtConcurrent>
#include <QUrl>

class FFPlayoutAPI : public QObject
{
    Q_OBJECT
    
public:
    explicit FFPlayoutAPI(const QString& baseUrl = "http://127.0.0.1:8787", int channelId = 1, QObject* parent = nullptr);
    
    // Authentication
    QFuture<bool> authenticate(const QString& username = "admin", const QString& password = "admin");
    
    // Channels
    QFuture<QJsonArray> getChannels();
    
    // Playlist operations
    QFuture<QJsonObject> getPlaylist(const QString& date);
    QFuture<bool> setPlaylist(const QString& date, const QJsonArray& playlist);
    
    // Media operations
    QFuture<QJsonArray> getMedia();
    QFuture<bool> addMedia(const QJsonObject& media);
    
    // Control
    QFuture<bool> play();
    QFuture<bool> pause();
    QFuture<bool> stop();
    QFuture<QJsonObject> getStatus();
    
    bool isAuthenticated() const { return !m_token.isEmpty(); }
    
signals:
    void authenticated(bool success);
    void errorOccurred(const QString& error);
    
private:
    QNetworkRequest createRequest(const QString& endpoint);
    QFuture<QJsonObject> makeRequest(const QString& method, const QString& endpoint, const QJsonObject& data = QJsonObject());
    
    QString m_baseUrl;
    QString m_token;
    int m_channelId;
    QNetworkAccessManager* m_networkManager;
};

#endif // FFPLAYOUTAPI_H

