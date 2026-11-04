#ifndef SCHEDULEDB_H
#define SCHEDULEDB_H

#include <QObject>
#include <QSqlDatabase>
#include "core/types.h"
#include "core/schedule.h"
#include "core/block.h"

class ScheduleDb : public QObject
{
    Q_OBJECT

public:
    explicit ScheduleDb(QObject* parent = nullptr);
    ~ScheduleDb();

    bool initialize();
    void close();

    void saveHourSlot(const HourSlot& slot);
    HourSlot getHourSlot(const QString& slotId) const;
    void deleteHourSlot(const QString& slotId);

    void saveWeeklySchedule(const WeeklySchedule& schedule);
    WeeklySchedule getWeeklySchedule() const;

    void saveFreePlaylist(const QString& playlistId, const QList<PlaylistItem>& items);
    QList<PlaylistItem> getFreePlaylist(const QString& playlistId) const;
    void deleteFreePlaylist(const QString& playlistId);

    // BlockSchedule persistence
    void saveBlockSchedule(const BlockSchedule& schedule);
    BlockSchedule getBlockSchedule() const;

private:
    QSqlDatabase m_db;
    bool m_initialized;
    bool ensureTables();
};

#endif // SCHEDULEDB_H

