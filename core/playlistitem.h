#ifndef PLAYLISTITEM_H
#define PLAYLISTITEM_H

#include "types.h"

// PlaylistItem is defined in types.h
// This file provides additional PlaylistItem-specific utilities if needed

class PlaylistItemUtils
{
public:
    static QString generateId();
    static double calculateDuration(const PlaylistItem& item);
    static bool isResolved(const PlaylistItem& item);
};

#endif // PLAYLISTITEM_H

