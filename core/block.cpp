#include "block.h"
#include "types.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>

QJsonObject Block::toJson() const
{
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["startTime"] = startTime.toString(Qt::ISODate);
    obj["endTime"] = endTime.toString(Qt::ISODate);
    obj["blockType"] = blockType;
    obj["dayOfWeek"] = dayOfWeek;
    obj["hour"] = hour;
    obj["isRepeating"] = isRepeating;
    
    QJsonArray itemsArray;
    for (const PlaylistItem& item : items) {
        itemsArray.append(item.toJson());
    }
    obj["items"] = itemsArray;
    obj["metadata"] = metadata;
    
    return obj;
}

Block Block::fromJson(const QJsonObject& json)
{
    Block block;
    block.id = json["id"].toString();
    block.name = json["name"].toString();
    block.startTime = QDateTime::fromString(json["startTime"].toString(), Qt::ISODate);
    block.endTime = QDateTime::fromString(json["endTime"].toString(), Qt::ISODate);
    block.blockType = json["blockType"].toString();
    block.dayOfWeek = json["dayOfWeek"].toInt();
    block.hour = json["hour"].toInt();
    block.isRepeating = json["isRepeating"].toBool();
    block.metadata = json["metadata"].toObject();
    
    QJsonArray itemsArray = json["items"].toArray();
    for (const QJsonValue& value : itemsArray) {
        block.items.append(PlaylistItem::fromJson(value.toObject()));
    }
    
    return block;
}

QList<Block> BlockSchedule::getBlocksForTimeRange(const QDateTime& start, const QDateTime& end) const
{
    QList<Block> result;
    
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        const Block& block = it.value();
        
        // Check if block overlaps with time range
        if (block.startTime <= end && block.endTime >= start) {
            result.append(block);
        }
    }
    
    return result;
}

Block BlockSchedule::getBlock(const QString& blockId) const
{
    if (blocks.contains(blockId)) {
        return blocks[blockId];
    }
    return Block();
}

void BlockSchedule::addBlock(const Block& block)
{
    blocks[block.id] = block;
    lastModified = QDateTime::currentDateTime();
}

void BlockSchedule::removeBlock(const QString& blockId)
{
    blocks.remove(blockId);
    lastModified = QDateTime::currentDateTime();
}

QJsonObject BlockSchedule::toJson() const
{
    QJsonObject obj;
    obj["scheduleName"] = scheduleName;
    obj["lastModified"] = lastModified.toString(Qt::ISODate);
    
    QJsonObject blocksObj;
    for (auto it = blocks.begin(); it != blocks.end(); ++it) {
        blocksObj[it.key()] = it.value().toJson();
    }
    obj["blocks"] = blocksObj;
    
    return obj;
}

BlockSchedule BlockSchedule::fromJson(const QJsonObject& json)
{
    BlockSchedule schedule;
    schedule.scheduleName = json["scheduleName"].toString();
    schedule.lastModified = QDateTime::fromString(json["lastModified"].toString(), Qt::ISODate);
    
    QJsonObject blocksObj = json["blocks"].toObject();
    for (auto it = blocksObj.begin(); it != blocksObj.end(); ++it) {
        schedule.blocks[it.key()] = Block::fromJson(it.value().toObject());
    }
    
    return schedule;
}

