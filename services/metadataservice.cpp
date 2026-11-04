#include "metadataservice.h"
#include "utils/mediautils.h"
#include "tools/avkit/avprobe.h"
#include <QProcess>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QDebug>

namespace {

constexpr int kProbeTimeoutMs = 30000;   // the old ffprobe wait, kept for the in-process deadline

// PLAYPLAY_PROBE_ENGINE=ffprobe brings back the old spawn of the ffprobe found on PATH (DEP-02a fallback switch; read per
// call so it can be flipped without a rebuild). Anything else, or unset, probes in-process with PlayPlayAvKit.
bool useLegacyFfprobe()
{
    return qEnvironmentVariable("PLAYPLAY_PROBE_ENGINE").trimmed().compare(QLatin1String("ffprobe"), Qt::CaseInsensitive) == 0;
}

} // namespace

MetadataService::MetadataService(QObject* parent)
    : QObject(parent)
{
}

QFuture<MediaMetadata*> MetadataService::extractMetadata(const QString& filePath)
{
    return QtConcurrent::run([this, filePath]() {
        return extractMetadataSync(filePath);
    });
}

MediaMetadata* MetadataService::extractMetadataSync(const QString& filePath)
{
    QJsonObject info = probeMediaFile(filePath);
    if (info.isEmpty()) {
        emit metadataError(filePath, "Failed to probe media file");
        return nullptr;
    }
    
    MediaMetadata* metadata = new MediaMetadata();
    
    QJsonObject format = info["format"].toObject();
    if (!format.isEmpty()) {
        metadata->duration = format["duration"].toString().toDouble();
        metadata->fileSize = format["size"].toString().toLongLong();
        metadata->format = format["format_name"].toString();
        metadata->bitrate = format["bit_rate"].toString().toInt();
        
        QJsonObject tags = format["tags"].toObject();
        metadata->tags = tags;
    }
    
    QJsonArray streams = info["streams"].toArray();
    for (const QJsonValue& streamVal : streams) {
        QJsonObject stream = streamVal.toObject();
        QString codecType = stream["codec_type"].toString();
        
        if (codecType == "video") {
            metadata->videoCodec = stream["codec_name"].toString();
            metadata->width = stream["width"].toInt();
            metadata->height = stream["height"].toInt();
            
            // Parse framerate
            QString fpsStr = stream["avg_frame_rate"].toString();
            if (fpsStr.isEmpty()) {
                fpsStr = stream["r_frame_rate"].toString();
            }
            if (!fpsStr.isEmpty() && fpsStr.contains('/')) {
                QStringList parts = fpsStr.split('/');
                if (parts.size() == 2) {
                    double num = parts[0].toDouble();
                    double den = parts[1].toDouble();
                    if (den > 0) {
                        metadata->framerate = num / den;
                    }
                }
            }
        } else if (codecType == "audio") {
            metadata->audioCodec = stream["codec_name"].toString();
            metadata->audioChannels = stream["channels"].toInt();
            metadata->audioSampleRate = stream["sample_rate"].toString().toInt();
        }
    }
    
    emit metadataExtracted(filePath, *metadata);
    return metadata;
}

QFuture<QJsonObject> MetadataService::enrichMediaItem(const QJsonObject& mediaItem)
{
    return QtConcurrent::run([this, mediaItem]() {
        QString filePath = mediaItem["filePath"].toString();
        if (filePath.isEmpty()) {
            return mediaItem;
        }
        
        MediaMetadata* metadata = extractMetadataSync(filePath);
        if (!metadata) {
            return mediaItem;
        }
        
        QJsonObject enriched = mediaItem;
        enriched["duration"] = metadata->duration;
        enriched["width"] = metadata->width;
        enriched["height"] = metadata->height;
        enriched["framerate"] = metadata->framerate;
        enriched["bitrate"] = metadata->bitrate;
        enriched["videoCodec"] = metadata->videoCodec;
        enriched["audioCodec"] = metadata->audioCodec;
        enriched["audioChannels"] = metadata->audioChannels;
        enriched["audioSampleRate"] = metadata->audioSampleRate;
        enriched["fileSize"] = metadata->fileSize;
        enriched["format"] = metadata->format;
        enriched["tags"] = metadata->tags;
        
        delete metadata;
        return enriched;
    });
}

QJsonObject MetadataService::probeMediaFile(const QString& filePath)
{
    // In-process probe (PlayPlayAvKit, docs/plans/inhouse-deps.md DEP-02a): the same JSON
    // `ffprobe -v quiet -print_format json -show_format -show_streams` prints, from the FFmpeg build the app ships and links,
    // instead of a process per file running whatever "ffprobe" the Windows search finds (PATH, the exe's folder: on the
    // station PC PATH holds other FFmpeg builds; where the tools are not installed, nothing).
    // Local files only, with the old 30 s deadline. Files the operator picks for import are trusted work (§6.1); bulk
    // probing of untrusted folders belongs in the MediaTool process (DEP-03/04).
    if (!useLegacyFfprobe()) {
        avkit::ProbeOptions opts;
        opts.timeoutMs = kProbeTimeoutMs;
        QString error;
        const QJsonObject info = avkit::probeJson(filePath, opts, &error);
        if (info.isEmpty())
            qWarning() << "media probe failed for" << filePath << ":" << error;
        return info;
    }

    // Legacy path (PLAYPLAY_PROBE_ENGINE=ffprobe): use ffprobe to get media information
    QProcess process;
    QStringList args;
    args << "-v" << "quiet"
         << "-print_format" << "json"
         << "-show_format"
         << "-show_streams"
         << filePath;
    
    process.start("ffprobe", args);
    if (!process.waitForFinished(30000)) { // 30 second timeout
        qWarning() << "ffprobe timeout for" << filePath;
        return QJsonObject();
    }
    
    if (process.exitCode() != 0) {
        qWarning() << "ffprobe error:" << process.readAllStandardError();
        return QJsonObject();
    }
    
    QByteArray output = process.readAllStandardOutput();
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(output, &error);
    
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse ffprobe output:" << error.errorString();
        return QJsonObject();
    }
    
    return doc.object();
}

QJsonObject MediaMetadata::toJson() const
{
    QJsonObject json;
    json["duration"] = duration;
    json["videoCodec"] = videoCodec;
    json["audioCodec"] = audioCodec;
    json["width"] = width;
    json["height"] = height;
    json["framerate"] = framerate;
    json["bitrate"] = bitrate;
    json["audioChannels"] = audioChannels;
    json["audioSampleRate"] = audioSampleRate;
    json["fileSize"] = fileSize;
    json["format"] = format;
    json["tags"] = tags;
    return json;
}

MediaMetadata MediaMetadata::fromJson(const QJsonObject& json)
{
    MediaMetadata metadata;
    metadata.duration = json["duration"].toDouble();
    metadata.videoCodec = json["videoCodec"].toString();
    metadata.audioCodec = json["audioCodec"].toString();
    metadata.width = json["width"].toInt();
    metadata.height = json["height"].toInt();
    metadata.framerate = json["framerate"].toDouble();
    metadata.bitrate = json["bitrate"].toInt();
    metadata.audioChannels = json["audioChannels"].toInt();
    metadata.audioSampleRate = json["audioSampleRate"].toInt();
    metadata.fileSize = json["fileSize"].toVariant().toLongLong();
    metadata.format = json["format"].toString();
    metadata.tags = json["tags"].toObject();
    return metadata;
}

