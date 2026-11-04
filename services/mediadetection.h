#ifndef MEDIADETECTION_H
#define MEDIADETECTION_H

#include <QObject>
#include <QString>
#include "core/types.h"

class MediaDetection : public QObject
{
    Q_OBJECT
    
public:
    explicit MediaDetection(QObject* parent = nullptr);
    
    // Auto-detect media type based on duration and filename
    static MediaType detectMediaType(const QString& filename, double durationSeconds);
    
    // Auto-detect client from filename by matching against known client names
    static QString detectClientFromFilename(const QString& filename, const QJsonArray& clients);
    
    // Detect bumper subtype from filename
    static BumperSubtype detectBumperSubtype(const QString& filename);
    
    // Detect program series from filename
    static QString detectProgramSeries(const QString& filename);
    
    // Convert MediaType to string
    static QString mediaTypeToString(MediaType type);
    
    // Convert string to MediaType
    static MediaType stringToMediaType(const QString& str);
    
private:
    static const QStringList ID_PROMO_KEYWORDS;
    static bool containsKeyword(const QString& filename, const QStringList& keywords);
};

#endif // MEDIADETECTION_H

