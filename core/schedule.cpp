#include "schedule.h"
#include "playout/stateengine.h"

QString ScheduleUtils::generateSlotId(int dayOfWeek, int hour)
{
    return QString("%1-%2").arg(dayOfWeek).arg(hour);
}

QString ScheduleUtils::generateDateSlotId(const QDate& date, int hour)
{
    return QString("%1-%2").arg(date.toString(Qt::ISODate)).arg(hour);
}

HourSlot ScheduleUtils::createEmptySlot(int dayOfWeek, int hour)
{
    HourSlot slot;
    slot.id = generateSlotId(dayOfWeek, hour);
    slot.dayOfWeek = dayOfWeek;
    slot.hour = hour;
    slot.isLooping = false;
    slot.randomizeAds = true;
    slot.keepProgramOrder = true;
    slot.updatedAt = QDateTime::currentDateTimeUtc();
    return slot;
}

WeeklySchedule ScheduleUtils::createEmptyWeeklySchedule()
{
    WeeklySchedule schedule;
    schedule.lastModified = QDateTime::currentDateTimeUtc();
    
    for (int day = 0; day < 7; day++) {
        for (int hour = 0; hour < 24; hour++) {
            QString slotId = generateSlotId(day, hour);
            HourSlot slot = createEmptySlot(day, hour);
            schedule.slotKeys.append(slotId);
            schedule.slotValues.append(slot);
        }
    }
    
    return schedule;
}

// scheduleToPlannedEvents moved to avoid circular dependency with playout/stateengine.h

