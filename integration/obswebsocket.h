#ifndef OBSWEBSOCKET_H
#define OBSWEBSOCKET_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QWebSocket>

class ObsWebSocketClient : public QObject
{
    Q_OBJECT

public:
    explicit ObsWebSocketClient(QObject* parent = nullptr);
    
    bool connectToObs(const QString& host = "localhost", int port = 4455, const QString& password = QString());
    void disconnectFromObs();
    
    bool setCurrentScene(const QString& sceneName);
    bool setSourceVisibility(const QString& sourceName, bool visible);
    QStringList getSceneList();
    
    bool isConnected() const { return m_isConnected; }

signals:
    void connected();
    void disconnected();
    void sceneChanged(const QString& sceneName);
    void sourceVisibilityChanged(const QString& sourceName, bool visible);
    void errorOccurred(const QString& error);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString& message);
    void onError(QAbstractSocket::SocketError error);

private:
    bool authenticate(const QString& password);
    
    QWebSocket* m_webSocket;
    bool m_isConnected;
    QString m_host;
    int m_port;
    QString m_password;
    int m_messageId;
};

#endif // OBSWEBSOCKET_H

