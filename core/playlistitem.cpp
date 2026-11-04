#include "playlistitem.h"
#include <QUuid>

QString PlaylistItemUtils::generateId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

double PlaylistItemUtils::calculateDuration(const PlaylistItem& item)
{
    double duration = item.media.duration;
    
    if (item.timeIn > 0 || item.timeOut > 0) {
        double start = item.timeIn > 0 ? item.timeIn : 0;
        double end = item.timeOut > 0 ? item.timeOut : duration;
        return end - start;
    }
    
    return duration;
}

bool PlaylistItemUtils::isResolved(const PlaylistItem& item)
{
    return item.isResolved && !item.media.id.isEmpty();
}

