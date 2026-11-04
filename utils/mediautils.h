#ifndef MEDIAUTILS_H
#define MEDIAUTILS_H

#include <QString>
#include "core/types.h"

class MediaUtils
{
public:
    static QString getMediaTypeString(MediaType type);
    static double getMediaDuration(const QString& filePath);
    static bool isValidMediaFile(const QString& filePath);
};

#endif // MEDIAUTILS_H

