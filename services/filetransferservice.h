#ifndef FILETRANSFERSERVICE_H
#define FILETRANSFERSERVICE_H

#include <QObject>
#include <QString>
#include <QUrl>
#include <QNetworkAccessManager>

class FileTransferService : public QObject
{
    Q_OBJECT

public:
    explicit FileTransferService(QObject* parent = nullptr);
    
    void uploadFile(const QString& localPath, const QUrl& remoteUrl);
    void downloadFile(const QUrl& remoteUrl, const QString& localPath);

signals:
    void transferProgress(const QString& filePath, qint64 bytesTransferred, qint64 bytesTotal);
    void transferCompleted(const QString& filePath);
    void transferFailed(const QString& filePath, const QString& error);

private:
    QNetworkAccessManager* m_networkManager;
};

#endif // FILETRANSFERSERVICE_H

