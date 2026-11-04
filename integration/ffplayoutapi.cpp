#include "ffplayoutapi.h"
#include <QtConcurrent>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QEventLoop>

FFPlayoutAPI::FFPlayoutAPI(const QString& baseUrl, int channelId, QObject* parent)
    : QObject(parent)
    , m_baseUrl(baseUrl)
    , m_channelId(channelId)
    , m_networkManager(new QNetworkAccessManager(this))
{
    if (m_baseUrl.endsWith('/')) {
        m_baseUrl.chop(1);
    }
}

QFuture<bool> FFPlayoutAPI::authenticate(const QString& username, const QString& password)
{
    return QtConcurrent::run([this, username, password]() {
        QJsonObject data;
        data["username"] = username;
        data["password"] = password;
        
        QFuture<QJsonObject> future = makeRequest("POST", "/api/auth/login", data);
        QJsonObject response = future.result();
        
        if (response.contains("token")) {
            m_token = response["token"].toString();
            emit authenticated(true);
            return true;
        }
        
        emit authenticated(false);
        return false;
    });
}

QFuture<QJsonArray> FFPlayoutAPI::getChannels()
{
    return QtConcurrent::run([this]() {
        QFuture<QJsonObject> future = makeRequest("GET", "/api/channels");
        QJsonObject response = future.result();
        
        if (response.contains("data")) {
            return response["data"].toArray();
        }
        return QJsonArray();
    });
}

QFuture<QJsonObject> FFPlayoutAPI::getPlaylist(const QString& date)
{
    return QtConcurrent::run([this, date]() {
        QString endpoint = QString("/api/channels/%1/playlist/%2").arg(m_channelId).arg(date);
        return makeRequest("GET", endpoint).result();
    });
}

QFuture<bool> FFPlayoutAPI::setPlaylist(const QString& date, const QJsonArray& playlist)
{
    return QtConcurrent::run([this, date, playlist]() {
        QJsonObject data;
        data["channel"] = QString::number(m_channelId);
        data["date"] = date;
        data["program"] = playlist;
        
        QString endpoint = QString("/api/channels/%1/playlist/%2").arg(m_channelId).arg(date);
        QFuture<QJsonObject> future = makeRequest("PUT", endpoint, data);
        QJsonObject response = future.result();
        
        return response.contains("success") && response["success"].toBool();
    });
}

QFuture<QJsonArray> FFPlayoutAPI::getMedia()
{
    return QtConcurrent::run([this]() {
        QFuture<QJsonObject> future = makeRequest("GET", "/api/media");
        QJsonObject response = future.result();
        
        if (response.contains("data")) {
            return response["data"].toArray();
        }
        return QJsonArray();
    });
}

QFuture<bool> FFPlayoutAPI::addMedia(const QJsonObject& media)
{
    return QtConcurrent::run([this, media]() {
        QFuture<QJsonObject> future = makeRequest("POST", "/api/media", media);
        QJsonObject response = future.result();
        return response.contains("success") && response["success"].toBool();
    });
}

QFuture<bool> FFPlayoutAPI::play()
{
    return QtConcurrent::run([this]() {
        QString endpoint = QString("/api/channels/%1/control/play").arg(m_channelId);
        QFuture<QJsonObject> future = makeRequest("POST", endpoint);
        QJsonObject response = future.result();
        return response.contains("success") && response["success"].toBool();
    });
}

QFuture<bool> FFPlayoutAPI::pause()
{
    return QtConcurrent::run([this]() {
        QString endpoint = QString("/api/channels/%1/control/pause").arg(m_channelId);
        QFuture<QJsonObject> future = makeRequest("POST", endpoint);
        QJsonObject response = future.result();
        return response.contains("success") && response["success"].toBool();
    });
}

QFuture<bool> FFPlayoutAPI::stop()
{
    return QtConcurrent::run([this]() {
        QString endpoint = QString("/api/channels/%1/control/stop").arg(m_channelId);
        QFuture<QJsonObject> future = makeRequest("POST", endpoint);
        QJsonObject response = future.result();
        return response.contains("success") && response["success"].toBool();
    });
}

QFuture<QJsonObject> FFPlayoutAPI::getStatus()
{
    return QtConcurrent::run([this]() {
        QString endpoint = QString("/api/channels/%1/status").arg(m_channelId);
        return makeRequest("GET", endpoint).result();
    });
}

QNetworkRequest FFPlayoutAPI::createRequest(const QString& endpoint)
{
    QUrl url(m_baseUrl + endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    
    if (!m_token.isEmpty()) {
        request.setRawHeader("Authorization", QString("Bearer %1").arg(m_token).toUtf8());
    }
    
    return request;
}

QFuture<QJsonObject> FFPlayoutAPI::makeRequest(const QString& method, const QString& endpoint, const QJsonObject& data)
{
    return QtConcurrent::run([this, method, endpoint, data]() {
        QNetworkRequest request = createRequest(endpoint);
        QNetworkReply* reply = nullptr;
        
        if (method == "GET") {
            reply = m_networkManager->get(request);
        } else if (method == "POST") {
            QJsonDocument doc(data);
            reply = m_networkManager->post(request, doc.toJson());
        } else if (method == "PUT") {
            QJsonDocument doc(data);
            reply = m_networkManager->put(request, doc.toJson());
        } else if (method == "DELETE") {
            reply = m_networkManager->deleteResource(request);
        }
        
        if (!reply) {
            return QJsonObject();
        }
        
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        
        QByteArray responseData = reply->readAll();
        reply->deleteLater();
        
        if (reply->error() != QNetworkReply::NoError) {
            QString error = QString("Network error: %1").arg(reply->errorString());
            emit errorOccurred(error);
            return QJsonObject();
        }
        
        QJsonParseError parseError;
        QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            QString error = QString("Parse error: %1").arg(parseError.errorString());
            emit errorOccurred(error);
            return QJsonObject();
        }
        
        return doc.object();
    });
}

