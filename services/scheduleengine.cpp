#include "scheduleengine.h"
#include "services/logger.h"
#include "core/schedule.h"
#include "database/scheduledb.h"
#include <QDebug>
#include <QDateTime>

ScheduleEngine::ScheduleEngine(QObject* parent)
    : QObject(parent)
    , m_scheduleDb(new ScheduleDb(this))
{
    m_scheduleDb->initialize();
    loadSchedule();
}

void ScheduleEngine::loadSchedule()
{
    // Load schedule from database
    m_currentSchedule = m_scheduleDb->getWeeklySchedule();
    
    if (m_currentSchedule.empty()) {
        // Initialize with empty schedule if none exists
        m_currentSchedule = ScheduleUtils::createEmptyWeeklySchedule();
    }
    
    Logger::instance()->info("Schedule loaded");
    emit scheduleChanged();
}

void ScheduleEngine::saveSchedule(const WeeklySchedule& schedule)
{
    m_currentSchedule = schedule;
    m_scheduleDb->saveWeeklySchedule(schedule);
    
    Logger::instance()->info("Schedule saved");
    emit scheduleChanged();
}

WeeklySchedule ScheduleEngine::getSchedule() const
{
    return m_currentSchedule;
}

QList<PlaylistItem> ScheduleEngine::getItemsForSlot(int dayOfWeek, int hour) const
{
    QString slotId = ScheduleUtils::generateSlotId(dayOfWeek, hour);
    const HourSlot* slot = m_currentSchedule.findSlot(slotId);
    if (slot) {
        return slot->items;
    }
    
    // Try to get from database if not in memory
    HourSlot dbSlot = m_scheduleDb->getHourSlot(slotId);
    return dbSlot.items;
}

void ScheduleEngine::updateSlot(int dayOfWeek, int hour, const HourSlot& slot)
{
    QString slotId = ScheduleUtils::generateSlotId(dayOfWeek, hour);
    HourSlot updatedSlot = slot;
    updatedSlot.id = slotId;
    updatedSlot.dayOfWeek = dayOfWeek;
    updatedSlot.hour = hour;
    m_currentSchedule[slotId] = updatedSlot;
    m_scheduleDb->saveHourSlot(updatedSlot);
    emit scheduleChanged();
}

QList<PlannedEvent> ScheduleEngine::generatePlannedEvents(const QDateTime& baseDate)
{
    QList<PlannedEvent> events;
    
    // Generate planned events from weekly schedule
    // This is a simplified version - in production, this would be more complex
    
    for (int i = 0; i < m_currentSchedule.slotKeys.size() && i < m_currentSchedule.slotValues.size(); ++i) {
        const HourSlot& slot = m_currentSchedule.slotValues[i];
        
        // Calculate start time for this slot
        QDateTime slotStart = baseDate;
        slotStart.setTime(QTime(slot.hour, 0, 0));
        
        // Add events for each item in the slot
        qint64 currentTime = slotStart.toMSecsSinceEpoch();
        
        for (const PlaylistItem& item : slot.items) {
            PlannedEvent event;
            event.eventId = QString("event_%1_%2").arg(slot.id).arg(item.id);
            event.startTime = currentTime;
            event.duration = static_cast<qint64>((item.timeOut - item.timeIn) * 1000);
            event.endTime = currentTime + event.duration;
            event.assetId = item.media.id;
            event.engineId = "main"; // TODO: Get from configuration
            event.priority = item.priority;
            
            QJsonObject metadata;
            metadata["playlistItemId"] = item.id;
            metadata["slotId"] = slot.id;
            event.metadata = metadata;
            
            events.append(event);
            
            currentTime += event.duration;
        }
    }
    
    return events;
}

