#ifndef MEDIADB_H
#define MEDIADB_H

#include <QSqlDatabase>
#include <QList>
#include "../core/types.h"

class MediaDb {
public:
    MediaDb();
    ~MediaDb();
    
    bool initialize();
    void close();
    
    // Media operations
    bool saveMedia(const MediaItem& item);
    bool deleteMedia(const QString& id);
    QList<MediaItem> getAllMedia();
    MediaItem getMediaById(const QString& id);
    QList<MediaItem> findByFilename(const QString& query);
    bool incrementPlay(const QString& mediaId, double durationSeconds);
    
    // Schedule operations
    bool saveScheduleSlot(const HourSlot& slot);
    HourSlot getScheduleSlot(const QString& slotId);
    WeeklySchedule getAllSlots();
    bool deleteScheduleSlot(const QString& slotId);
    
    // Settings
    void setMeta(const QString& key, const QString& value);
    QString getMeta(const QString& key);
    QMap<QString, QString> getAllMeta();
    
private:
    QSqlDatabase db;
    bool ensureTables();
    void migrate();
};

#endif // MEDIADB_H





