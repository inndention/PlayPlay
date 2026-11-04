#ifndef SCHEDULEENGINE_H
#define SCHEDULEENGINE_H

#include <QObject>
#include "core/types.h"
#include "core/schedule.h"
#include "playout/stateengine.h" // For PlannedEvent

class ScheduleDb;

class ScheduleEngine : public QObject
{
    Q_OBJECT

public:
    explicit ScheduleEngine(QObject* parent = nullptr);
    
    void loadSchedule();
    void saveSchedule(const WeeklySchedule& schedule);
    WeeklySchedule getSchedule() const;
    
    QList<PlaylistItem> getItemsForSlot(int dayOfWeek, int hour) const;
    void updateSlot(int dayOfWeek, int hour, const HourSlot& slot);
    QList<PlannedEvent> generatePlannedEvents(const QDateTime& baseDate);

signals:
    void scheduleChanged();

private:
    ScheduleDb* m_scheduleDb;
    WeeklySchedule m_currentSchedule;
};

#endif // SCHEDULEENGINE_H

