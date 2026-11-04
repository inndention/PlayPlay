#ifndef METADATASERVICE_H
#define METADATASERVICE_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QFuture>
#include <QtConcurrent>

struct MediaMetadata {
    double duration = 0.0;
    QString videoCodec;
    QString audioCodec;
    int width = 0;
    int height = 0;
    double framerate = 0.0;
    int bitrate = 0;
    int audioChannels = 0;
    int audioSampleRate = 0;
    qint64 fileSize = 0;
    QString format;
    QJsonObject tags;
    
    QJsonObject toJson() const;
    static MediaMetadata fromJson(const QJsonObject& json);
};

class MetadataService : public QObject
{
    Q_OBJECT
    
public:
    explicit MetadataService(QObject* parent = nullptr);
    
    // Extract metadata from media file using ffprobe
    QFuture<MediaMetadata*> extractMetadata(const QString& filePath);
    
    // Update MediaItem with extracted metadata (async)
    QFuture<QJsonObject> enrichMediaItem(const QJsonObject& mediaItem);
    
signals:
    void metadataExtracted(const QString& filePath, const MediaMetadata& metadata);
    void metadataError(const QString& filePath, const QString& error);
    
private:
    MediaMetadata* extractMetadataSync(const QString& filePath);
    QJsonObject probeMediaFile(const QString& filePath);
};

#endif // METADATASERVICE_H

