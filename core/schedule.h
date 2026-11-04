#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "types.h"

// WeeklySchedule and HourSlot are defined in types.h
// This file provides additional schedule-specific utilities

class ScheduleUtils
{
public:
    static QString generateSlotId(int dayOfWeek, int hour);
    static QString generateDateSlotId(const QDate& date, int hour);
    static HourSlot createEmptySlot(int dayOfWeek, int hour);
    static WeeklySchedule createEmptyWeeklySchedule();
    // Note: scheduleToPlannedEvents moved to avoid circular dependency with playout/stateengine.h
};

#endif // SCHEDULE_H

