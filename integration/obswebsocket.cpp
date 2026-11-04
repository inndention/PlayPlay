#include "obswebsocket.h"
#include "services/logger.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>

ObsWebSocketClient::ObsWebSocketClient(QObject* parent)
    : QObject(parent)
    , m_webSocket(new QWebSocket("", QWebSocketProtocol::VersionLatest, this))
    , m_isConnected(false)
    , m_messageId(0)
{
    connect(m_webSocket, &QWebSocket::connected, this, &ObsWebSocketClient::onConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &ObsWebSocketClient::onDisconnected);
    connect(m_webSocket, &QWebSocket::textMessageReceived, this, &ObsWebSocketClient::onTextMessageReceived);
    connect(m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &ObsWebSocketClient::onError);
}

bool ObsWebSocketClient::connectToObs(const QString& host, int port, const QString& password)
{
    if (m_isConnected) {
        disconnectFromObs();
    }
    
    m_host = host;
    m_port = port;
    m_password = password;
    
    QUrl url;
    url.setScheme("ws");
    url.setHost(host);
    url.setPort(port);
    
    Logger::instance()->info(QString("Connecting to OBS at %1:%2").arg(host).arg(port));
    
    m_webSocket->open(url);
    
    // Wait for connection (with timeout)
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);
    
    connect(m_webSocket, &QWebSocket::connected, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeout.start();
    loop.exec();
    
    if (!m_isConnected) {
        Logger::instance()->error("Failed to connect to OBS WebSocket");
        return false;
    }
    
    // Authenticate if password is provided
    if (!password.isEmpty()) {
        return authenticate(password);
    }
    
    return true;
}

void ObsWebSocketClient::disconnectFromObs()
{
    if (m_webSocket->state() == QAbstractSocket::ConnectedState) {
        m_webSocket->close();
    }
    m_isConnected = false;
}

bool ObsWebSocketClient::authenticate(const QString& password)
{
    // Get authentication challenge
    QJsonObject request;
    request["request-type"] = "GetAuthRequired";
    request["message-id"] = QString::number(++m_messageId);
    
    QJsonDocument doc(request);
    m_webSocket->sendTextMessage(doc.toJson());
    
    // TODO: Handle authentication challenge/response
    // This is a simplified version
    Q_UNUSED(password)
    
    return true;
}

bool ObsWebSocketClient::setCurrentScene(const QString& sceneName)
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject request;
    request["request-type"] = "SetCurrentScene";
    request["scene-name"] = sceneName;
    request["message-id"] = QString::number(++m_messageId);
    
    QJsonDocument doc(request);
    m_webSocket->sendTextMessage(doc.toJson());
    
    Logger::instance()->info(QString("Set OBS scene to: %1").arg(sceneName));
    return true;
}

bool ObsWebSocketClient::setSourceVisibility(const QString& sourceName, bool visible)
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject request;
    request["request-type"] = "SetSourceRender";
    request["source"] = sourceName;
    request["render"] = visible;
    request["message-id"] = QString::number(++m_messageId);
    
    QJsonDocument doc(request);
    m_webSocket->sendTextMessage(doc.toJson());
    
    return true;
}

QStringList ObsWebSocketClient::getSceneList()
{
    if (!m_isConnected) {
        return QStringList();
    }
    
    QJsonObject request;
    request["request-type"] = "GetSceneList";
    request["message-id"] = QString::number(++m_messageId);
    
    QJsonDocument doc(request);
    m_webSocket->sendTextMessage(doc.toJson());
    
    // TODO: Wait for response and parse
    return QStringList();
}

void ObsWebSocketClient::onConnected()
{
    m_isConnected = true;
    Logger::instance()->info("Connected to OBS WebSocket");
    emit connected();
}

void ObsWebSocketClient::onDisconnected()
{
    m_isConnected = false;
    Logger::instance()->info("Disconnected from OBS WebSocket");
    emit disconnected();
}

void ObsWebSocketClient::onTextMessageReceived(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (doc.isNull()) {
        return;
    }
    
    QJsonObject obj = doc.object();
    
    // Handle different message types
    if (obj.contains("update-type")) {
        QString updateType = obj["update-type"].toString();
        
        if (updateType == "SwitchScenes") {
            QString sceneName = obj["scene-name"].toString();
            emit sceneChanged(sceneName);
        } else if (updateType == "SceneItemVisibilityChanged") {
            QString sourceName = obj["item-name"].toString();
            bool visible = obj["item-visible"].toBool();
            emit sourceVisibilityChanged(sourceName, visible);
        }
    }
}

void ObsWebSocketClient::onError(QAbstractSocket::SocketError error)
{
    QString errorString = m_webSocket->errorString();
    Logger::instance()->error(QString("OBS WebSocket error: %1 (code: %2)").arg(errorString).arg(error));
    emit errorOccurred(errorString);
}

