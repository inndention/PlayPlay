#include "mediautils.h"
#include <QFileInfo>
#include <QDir>

QString MediaUtils::getMediaTypeString(MediaType type)
{
    return mediaTypeToString(type);
}

double MediaUtils::getMediaDuration(const QString& filePath)
{
    // TODO: Use ffprobe or similar to get media duration
    Q_UNUSED(filePath)
    return 0.0;
}

bool MediaUtils::isValidMediaFile(const QString& filePath)
{
    QFileInfo info(filePath);
    if (!info.exists()) {
        return false;
    }
    
    QString ext = info.suffix().toLower();
    QStringList videoExts = {"mp4", "mov", "avi", "mkv", "m4v", "webm"};
    QStringList imageExts = {"jpg", "jpeg", "png", "gif", "bmp"};
    
    return videoExts.contains(ext) || imageExts.contains(ext);
}

