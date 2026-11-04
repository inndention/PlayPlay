#include "mediaitem.h"
#include <QUuid>
#include <QTime>

QString MediaItemUtils::generateId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool MediaItemUtils::isValid(const MediaItem& item)
{
    return !item.id.isEmpty() && !item.title.isEmpty() && item.duration > 0;
}

QString MediaItemUtils::formatDuration(double seconds)
{
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;
    
    if (hours > 0) {
        return QString("%1:%2:%3")
            .arg(hours, 2, 10, QChar('0'))
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    
    return QString("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

