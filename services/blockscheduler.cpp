#include "blockscheduler.h"
#include "database/scheduledb.h"
#include "services/logger.h"
#include "core/schedule.h"
#include "playout/stateengine.h"
#include <QDebug>
#include <QDate>
#include <algorithm>

BlockScheduler::BlockScheduler(QObject* parent)
    : QObject(parent)
    , m_scheduleDb(new ScheduleDb(this))
    , m_tickTimer(new QTimer(this))
    , m_initialized(false)
{
    connect(m_tickTimer, &QTimer::timeout, this, &BlockScheduler::onTick);
    m_tickTimer->setInterval(1000); // 1 second tick
}

BlockScheduler::~BlockScheduler()
{
    shutdown();
}

bool BlockScheduler::initialize()
{
    if (m_initialized) {
        return true;
    }
    
    if (!m_scheduleDb->initialize()) {
        Logger::instance()->error("BlockScheduler: Failed to initialize ScheduleDb");
        return false;
    }
    
    loadBlockSchedule();
    loadWeeklySchedule();
    
    m_tickTimer->start();
    m_initialized = true;
    
    Logger::instance()->info("BlockScheduler initialized");
    return true;
}

void BlockScheduler::shutdown()
{
    if (!m_initialized) {
        return;
    }
    
    m_tickTimer->stop();
    saveBlockSchedule();
    saveWeeklySchedule();
    m_initialized = false;
}

void BlockScheduler::addBlock(const Block& block)
{
    m_blockSchedule.addBlock(block);
    saveBlockSchedule();
    emit blockAdded(block.id);
    Logger::instance()->info(QString("Block added: %1").arg(block.name));
}

void BlockScheduler::removeBlock(const QString& blockId)
{
    m_blockSchedule.removeBlock(blockId);
    saveBlockSchedule();
    emit blockRemoved(blockId);
    Logger::instance()->info(QString("Block removed: %1").arg(blockId));
}

void BlockScheduler::updateBlock(const Block& block)
{
    m_blockSchedule.addBlock(block); // addBlock updates if exists
    saveBlockSchedule();
    emit blockUpdated(block.id);
    Logger::instance()->info(QString("Block updated: %1").arg(block.name));
}

Block BlockScheduler::getBlock(const QString& blockId) const
{
    return m_blockSchedule.getBlock(blockId);
}

QList<Block> BlockScheduler::getAllBlocks() const
{
    QList<Block> result;
    for (auto it = m_blockSchedule.blocks.begin(); it != m_blockSchedule.blocks.end(); ++it) {
        result.append(it.value());
    }
    return result;
}

QList<PlannedEvent> BlockScheduler::generateEventsForDate(const QDate& date)
{
    QDateTime startOfDay(date, QTime(0, 0, 0));
    QDateTime endOfDay(date, QTime(23, 59, 59));
    return generateEventsForTimeRange(startOfDay, endOfDay);
}

QList<PlannedEvent> BlockScheduler::generateEventsForTimeRange(const QDateTime& start, const QDateTime& end)
{
    QList<PlannedEvent> events;
    
    // Get blocks for time range
    QList<Block> blocks = m_blockSchedule.getBlocksForTimeRange(start, end);
    
    // Also check weekly schedule
    for (int i = 0; i < m_weeklySchedule.slotKeys.size() && i < m_weeklySchedule.slotValues.size(); ++i) {
        const HourSlot& slot = m_weeklySchedule.slotValues[i];
        
        // Calculate if this slot applies to the time range
        QDateTime slotStart = start;
        slotStart.setTime(QTime(slot.hour, 0, 0));
        
        // Adjust for day of week
        int daysToAdd = (slot.dayOfWeek - slotStart.date().dayOfWeek() + 7) % 7;
        slotStart = slotStart.addDays(daysToAdd);
        
        if (slotStart >= start && slotStart <= end) {
            // Create events from slot items
            qint64 currentTime = slotStart.toMSecsSinceEpoch();
            
            for (const PlaylistItem& item : slot.items) {
                PlannedEvent event;
                event.eventId = QString("event_%1_%2").arg(slot.id).arg(item.id);
                event.startTime = currentTime;
                event.duration = static_cast<qint64>((item.timeOut - item.timeIn) * 1000);
                event.endTime = currentTime + event.duration;
                event.assetId = item.media.id;
                event.engineId = "main";
                event.priority = item.priority;
                
                QJsonObject metadata;
                metadata["playlistItemId"] = item.id;
                metadata["slotId"] = slot.id;
                event.metadata = metadata;
                
                events.append(event);
                currentTime += event.duration;
            }
        }
    }
    
    // Process blocks
    for (const Block& block : blocks) {
        qint64 currentTime = block.startTime.toMSecsSinceEpoch();
        
        for (const PlaylistItem& item : block.items) {
            PlannedEvent event;
            event.eventId = QString("event_%1_%2").arg(block.id).arg(item.id);
            event.startTime = currentTime;
            double duration = item.media.duration;
            if (duration <= 0) duration = 30.0; // Default 30 seconds
            event.duration = static_cast<qint64>(duration * 1000);
            event.endTime = currentTime + event.duration;
            event.assetId = item.media.id;
            event.engineId = "main";
            event.priority = item.priority;
            
            QJsonObject metadata;
            metadata["playlistItemId"] = item.id;
            metadata["blockId"] = block.id;
            metadata["blockName"] = block.name;
            event.metadata = metadata;
            
            events.append(event);
            currentTime += event.duration;
        }
    }
    
    // Sort by start time
    std::sort(events.begin(), events.end(), [](const PlannedEvent& a, const PlannedEvent& b) {
        return a.startTime < b.startTime;
    });
    
    emit scheduleGenerated(events);
    return events;
}

void BlockScheduler::resolveBlocksForDate(const QDate& date)
{
    QDateTime startOfDay(date, QTime(0, 0, 0));
    QDateTime endOfDay(date, QTime(23, 59, 59));
    
    QList<Block> blocks = m_blockSchedule.getBlocksForTimeRange(startOfDay, endOfDay);
    
    // Resolve repeating blocks
    for (const Block& block : blocks) {
        if (block.isRepeating) {
            Block resolvedBlock = resolveRepeatingBlock(block, date);
            QString timeKey = resolvedBlock.startTime.toString("yyyyMMddHHmm");
            m_resolvedItems[timeKey] = resolvedBlock.items;
        } else {
            QString timeKey = block.startTime.toString("yyyyMMddHHmm");
            m_resolvedItems[timeKey] = block.items;
        }
    }
    
    // Resolve weekly schedule slots
    for (int i = 0; i < m_weeklySchedule.slotKeys.size() && i < m_weeklySchedule.slotValues.size(); ++i) {
        const HourSlot& slot = m_weeklySchedule.slotValues[i];
        
        if (slot.dayOfWeek == date.dayOfWeek()) {
            QDateTime slotTime(date, QTime(slot.hour, 0, 0));
            QString timeKey = slotTime.toString("yyyyMMddHHmm");
            m_resolvedItems[timeKey] = slot.items;
        }
    }
    
    emit blocksResolved(date);
}

QList<PlaylistItem> BlockScheduler::getResolvedItemsForTime(const QDateTime& time) const
{
    QString timeKey = time.toString("yyyyMMddHHmm");
    if (m_resolvedItems.contains(timeKey)) {
        return m_resolvedItems[timeKey];
    }
    return QList<PlaylistItem>();
}

void BlockScheduler::loadBlockSchedule()
{
    m_blockSchedule = m_scheduleDb->getBlockSchedule();
    if (m_blockSchedule.blocks.isEmpty()) {
        Logger::instance()->info("No existing block schedule found, initializing empty one");
        m_blockSchedule.scheduleName = "Default Block Schedule";
        m_blockSchedule.lastModified = QDateTime::currentDateTimeUtc();
    } else {
        Logger::instance()->info(QString("Block schedule loaded: %1 with %2 blocks")
                                 .arg(m_blockSchedule.scheduleName)
                                 .arg(m_blockSchedule.blocks.size()));
    }
}

void BlockScheduler::saveBlockSchedule()
{
    m_blockSchedule.lastModified = QDateTime::currentDateTimeUtc();
    m_scheduleDb->saveBlockSchedule(m_blockSchedule);
    Logger::instance()->info(QString("Block schedule saved: %1").arg(m_blockSchedule.scheduleName));
}

void BlockScheduler::loadWeeklySchedule()
{
    m_weeklySchedule = m_scheduleDb->getWeeklySchedule();
    Logger::instance()->info("Weekly schedule loaded");
}

void BlockScheduler::saveWeeklySchedule()
{
    m_scheduleDb->saveWeeklySchedule(m_weeklySchedule);
    Logger::instance()->info("Weekly schedule saved");
}

WeeklySchedule BlockScheduler::getWeeklySchedule() const
{
    return m_weeklySchedule;
}

void BlockScheduler::setWeeklySchedule(const WeeklySchedule& schedule)
{
    m_weeklySchedule = schedule;
    saveWeeklySchedule();
}

void BlockScheduler::onTick()
{
    processBlocks();
}

void BlockScheduler::processBlocks()
{
    QDateTime now = QDateTime::currentDateTime();
    QDate today = now.date();
    
    // Resolve blocks for today if needed
    QString todayKey = today.toString("yyyyMMdd");
    if (!m_resolvedItems.contains(todayKey + "0000")) {
        resolveBlocksForDate(today);
    }
}

Block BlockScheduler::resolveRepeatingBlock(const Block& templateBlock, const QDate& targetDate) const
{
    Block resolved = templateBlock;
    resolved.startTime = calculateBlockStartTime(templateBlock, targetDate);
    resolved.endTime = resolved.startTime.addMSecs(
        templateBlock.startTime.msecsTo(templateBlock.endTime)
    );
    return resolved;
}

QDateTime BlockScheduler::calculateBlockStartTime(const Block& block, const QDate& targetDate) const
{
    QDateTime result(targetDate, block.startTime.time());
    
    // Adjust for day of week if needed
    if (block.dayOfWeek >= 0) {
        int currentDayOfWeek = targetDate.dayOfWeek();
        int daysToAdd = (block.dayOfWeek - currentDayOfWeek + 7) % 7;
        result = result.addDays(daysToAdd);
    }
    
    return result;
}

