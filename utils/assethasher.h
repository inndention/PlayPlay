#ifndef ASSETHASHER_H
#define ASSETHASHER_H

#include <QObject>
#include <QString>
#include <QCryptographicHash>
#include <QJsonObject>
#include <QFuture>
#include <QtConcurrent>

struct AssetHash {
    QString hash;
    QString mediaId;
    QJsonObject metadata;
    
    QJsonObject toJson() const;
    static AssetHash fromJson(const QJsonObject& json);
};

class AssetHasher : public QObject
{
    Q_OBJECT
    
public:
    explicit AssetHasher(QObject* parent = nullptr);
    
    // Generate hash for a file
    QFuture<QString> generateFileHash(const QString& filePath);
    
    // Generate hash for a media item
    QFuture<QString> hashMediaItem(const QJsonObject& mediaItem);
    
    // Create asset hash entry
    QFuture<AssetHash> createAssetHash(const QJsonObject& mediaItem);
    
    // Report asset to sales endpoint
    QFuture<bool> reportAsset(const AssetHash& hash, const QString& endpointUrl);
    
signals:
    void hashGenerated(const QString& filePath, const QString& hash);
    void assetReported(const AssetHash& hash, bool success);
    
private:
    QString generateFileHashSync(const QString& filePath);
    QString hashString(const QString& data);
};

#endif // ASSETHASHER_H

