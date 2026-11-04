#ifndef MEDIAITEM_H
#define MEDIAITEM_H

#include "types.h"

// MediaItem is defined in types.h
// This file provides additional MediaItem-specific utilities if needed

class MediaItemUtils
{
public:
    static QString generateId();
    static bool isValid(const MediaItem& item);
    static QString formatDuration(double seconds);
};

#endif // MEDIAITEM_H

