#ifndef BLOCK_H
#define BLOCK_H

#include "types.h"
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QJsonObject>

// Block represents a scheduled time block (e.g., hour block, daypart block)
struct Block {
    QString id;
    QString name;
    QDateTime startTime;      // Absolute start time
    QDateTime endTime;        // Absolute end time
    QList<PlaylistItem> items; // Items in this block
    QString blockType;        // "hour", "daypart", "custom"
    int dayOfWeek;            // 0-6 (Monday-Sunday)
    int hour;                 // 0-23
    bool isRepeating;         // Weekly repeating block
    QJsonObject metadata;     // Additional block metadata
    
    QJsonObject toJson() const;
    static Block fromJson(const QJsonObject& json);
};

// Block Schedule manages blocks for scheduling
struct BlockSchedule {
    QMap<QString, Block> blocks; // blockId -> Block
    QDateTime lastModified;
    QString scheduleName;
    
    QList<Block> getBlocksForTimeRange(const QDateTime& start, const QDateTime& end) const;
    Block getBlock(const QString& blockId) const;
    void addBlock(const Block& block);
    void removeBlock(const QString& blockId);
    
    QJsonObject toJson() const;
    static BlockSchedule fromJson(const QJsonObject& json);
};

#endif // BLOCK_H

