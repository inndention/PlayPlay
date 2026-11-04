#include "filetransferservice.h"
#include "services/logger.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

FileTransferService::FileTransferService(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    // TODO: Port from TypeScript file transfer service
    // Key features:
    // - File upload/download
    // - Progress tracking
    // - Resume capability
    // - Multiple transfer queue
}

void FileTransferService::uploadFile(const QString& localPath, const QUrl& remoteUrl)
{
    QFileInfo fileInfo(localPath);
    if (!fileInfo.exists()) {
        emit transferFailed(localPath, "File does not exist");
        return;
    }
    
    QFile* file = new QFile(localPath, this);
    if (!file->open(QIODevice::ReadOnly)) {
        emit transferFailed(localPath, "Cannot open file for reading");
        file->deleteLater();
        return;
    }
    
    QNetworkRequest request(remoteUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    
    QNetworkReply* reply = m_networkManager->put(request, file);
    file->setParent(reply);
    
    connect(reply, &QNetworkReply::uploadProgress, this, [this, localPath](qint64 bytesSent, qint64 bytesTotal) {
        emit transferProgress(localPath, bytesSent, bytesTotal);
    });
    
    connect(reply, &QNetworkReply::finished, this, [this, localPath, reply]() {
        if (reply->error() == QNetworkReply::NoError) {
            emit transferCompleted(localPath);
        } else {
            emit transferFailed(localPath, reply->errorString());
        }
        reply->deleteLater();
    });
    
    Logger::instance()->info(QString("Upload started: %1").arg(localPath));
}

void FileTransferService::downloadFile(const QUrl& remoteUrl, const QString& localPath)
{
    QNetworkRequest request(remoteUrl);
    QNetworkReply* reply = m_networkManager->get(request);
    
    QFile* file = new QFile(localPath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        emit transferFailed(localPath, "Cannot open file for writing");
        reply->deleteLater();
        file->deleteLater();
        return;
    }
    
    connect(reply, &QNetworkReply::downloadProgress, this, [this, localPath](qint64 bytesReceived, qint64 bytesTotal) {
        emit transferProgress(localPath, bytesReceived, bytesTotal);
    });
    
    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
        file->write(reply->readAll());
    });
    
    connect(reply, &QNetworkReply::finished, this, [this, localPath, reply, file]() {
        file->close();
        
        if (reply->error() == QNetworkReply::NoError) {
            emit transferCompleted(localPath);
        } else {
            emit transferFailed(localPath, reply->errorString());
            file->remove();
        }
        reply->deleteLater();
        file->deleteLater();
    });
    
    Logger::instance()->info(QString("Download started: %1").arg(localPath));
}

