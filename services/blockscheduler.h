#ifndef BLOCKSCHEDULER_H
#define BLOCKSCHEDULER_H

#include <QObject>
#include <QTimer>
#include <QDateTime>
#include "core/block.h"
#include "core/types.h"
#include "core/schedule.h"
#include "playout/stateengine.h" // For PlannedEvent

class ScheduleDb;

class BlockScheduler : public QObject
{
    Q_OBJECT

public:
    explicit BlockScheduler(QObject* parent = nullptr);
    ~BlockScheduler();
    
    bool initialize();
    void shutdown();
    
    // Block management
    void addBlock(const Block& block);
    void removeBlock(const QString& blockId);
    void updateBlock(const Block& block);
    Block getBlock(const QString& blockId) const;
    QList<Block> getAllBlocks() const;
    
    // Schedule generation
    QList<PlannedEvent> generateEventsForDate(const QDate& date);
    QList<PlannedEvent> generateEventsForTimeRange(const QDateTime& start, const QDateTime& end);
    
    // Block resolution
    void resolveBlocksForDate(const QDate& date);
    QList<PlaylistItem> getResolvedItemsForTime(const QDateTime& time) const;
    
    // BlockSchedule persistence
    void loadBlockSchedule();
    void saveBlockSchedule();

    // Weekly schedule integration
    void loadWeeklySchedule();
    void saveWeeklySchedule();
    WeeklySchedule getWeeklySchedule() const;
    void setWeeklySchedule(const WeeklySchedule& schedule);

signals:
    void blockAdded(const QString& blockId);
    void blockRemoved(const QString& blockId);
    void blockUpdated(const QString& blockId);
    void scheduleGenerated(const QList<PlannedEvent>& events);
    void blocksResolved(const QDate& date);

private slots:
    void onTick();

private:
    void processBlocks();
    Block resolveRepeatingBlock(const Block& templateBlock, const QDate& targetDate) const;
    QDateTime calculateBlockStartTime(const Block& block, const QDate& targetDate) const;
    
    ScheduleDb* m_scheduleDb;
    BlockSchedule m_blockSchedule;
    WeeklySchedule m_weeklySchedule;
    QMap<QString, QList<PlaylistItem>> m_resolvedItems; // timeKey -> items
    QTimer* m_tickTimer;
    bool m_initialized;
};

#endif // BLOCKSCHEDULER_H

