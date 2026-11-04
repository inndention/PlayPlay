#include "assethasher.h"
#include <QFile>
#include <QDebug>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QHttpMultiPart>
#include <QHttpPart>

AssetHasher::AssetHasher(QObject* parent)
    : QObject(parent)
{
}

QFuture<QString> AssetHasher::generateFileHash(const QString& filePath)
{
    return QtConcurrent::run([this, filePath]() {
        return generateFileHashSync(filePath);
    });
}

QString AssetHasher::generateFileHashSync(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open file for hashing:" << filePath;
        return QString();
    }
    
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        QString hashString = hash.result().toHex();
        emit hashGenerated(filePath, hashString);
        return hashString;
    }
    
    return QString();
}

QFuture<QString> AssetHasher::hashMediaItem(const QJsonObject& mediaItem)
{
    return QtConcurrent::run([this, mediaItem]() {
        QString filePath = mediaItem["filePath"].toString();
        if (!filePath.isEmpty()) {
            return generateFileHashSync(filePath);
        }
        
        // Fallback: generate hash from item properties
        QString data = QString("%1-%2-%3-%4")
            .arg(mediaItem["id"].toString())
            .arg(mediaItem["title"].toString())
            .arg(mediaItem["duration"].toDouble())
            .arg(mediaItem["src"].toString());
        return hashString(data);
    });
}

QFuture<AssetHash> AssetHasher::createAssetHash(const QJsonObject& mediaItem)
{
    return QtConcurrent::run([this, mediaItem]() {
        QString hash = hashMediaItem(mediaItem).result();
        
        AssetHash assetHash;
        assetHash.hash = hash;
        assetHash.mediaId = mediaItem["id"].toString();
        assetHash.metadata = QJsonObject({
            {"title", mediaItem["title"].toString()},
            {"duration", mediaItem["duration"].toDouble()},
            {"type", mediaItem["type"].toString()},
            {"filePath", mediaItem["filePath"].toString()},
            {"filename", mediaItem["filename"].toString()}
        });
        
        return assetHash;
    });
}

QFuture<bool> AssetHasher::reportAsset(const AssetHash& hash, const QString& endpointUrl)
{
    return QtConcurrent::run([hash, endpointUrl, this]() {
        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(endpointUrl + "/api/assets"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        
        QJsonObject payload;
        payload["hash"] = hash.hash;
        payload["metadata"] = hash.metadata;
        
        QJsonDocument doc(payload);
        QByteArray data = doc.toJson();
        
        QNetworkReply* reply = manager.post(request, data);
        
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        
        bool success = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        
        emit assetReported(hash, success);
        return success;
    });
}

QString AssetHasher::hashString(const QString& data)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(data.toUtf8());
    return hash.result().toHex();
}

QJsonObject AssetHash::toJson() const
{
    QJsonObject json;
    json["hash"] = hash;
    json["mediaId"] = mediaId;
    json["metadata"] = metadata;
    return json;
}

AssetHash AssetHash::fromJson(const QJsonObject& json)
{
    AssetHash hash;
    hash.hash = json["hash"].toString();
    hash.mediaId = json["mediaId"].toString();
    hash.metadata = json["metadata"].toObject();
    return hash;
}

