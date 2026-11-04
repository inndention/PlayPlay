#include <atomic>
#include "scheduledb.h"
#include "core/types.h"
#include "core/schedule.h"
#include "core/block.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

ScheduleDb::ScheduleDb(QObject* parent)
    : QObject(parent)
    , m_initialized(false)
{
}

ScheduleDb::~ScheduleDb()
{
    close();
}

bool ScheduleDb::initialize()
{
    if (m_initialized) {
        return true;
    }

    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);

    QString dbFile = dataPath + "/schedule.db";
    static std::atomic<int> serial{0};
    m_db = QSqlDatabase::addDatabase("QSQLITE", QString("ScheduleDb-%1").arg(++serial));
    m_db.setDatabaseName(dbFile);

    if (!m_db.open()) {
        qWarning() << "Failed to open schedule database:" << m_db.lastError().text();
        return false;
    }

    m_initialized = ensureTables();
    return m_initialized;
}

void ScheduleDb::close()
{
    if (m_db.isValid()) {
        const QString name = m_db.connectionName();
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    m_initialized = false;
}

bool ScheduleDb::ensureTables()
{
    QSqlQuery query(m_db);

    // Hour slots table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS hour_slots (
            id TEXT PRIMARY KEY,
            day_of_week INTEGER NOT NULL,
            hour INTEGER NOT NULL,
            items TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        )
    )");

    // Free playlists table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS free_playlists (
            id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            items TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL
        )
    )");

    // Weekly schedule metadata
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS schedule_metadata (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
    )");

    // Block schedule table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS block_schedule (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            schedule_name TEXT NOT NULL,
            last_modified INTEGER NOT NULL,
            blocks_data TEXT NOT NULL
        )
    )");

    return true;
}

void ScheduleDb::saveHourSlot(const HourSlot& slot)
{
    if (!m_initialized) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT OR REPLACE INTO hour_slots 
        (id, day_of_week, hour, items, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
    )");

    QJsonArray itemsArray;
    for (const PlaylistItem& item : slot.items) {
        itemsArray.append(item.toJson());
    }

    query.addBindValue(slot.id);
    query.addBindValue(slot.dayOfWeek);
    query.addBindValue(slot.hour);
    query.addBindValue(QJsonDocument(itemsArray).toJson());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());

    if (!query.exec()) {
        qWarning() << "Failed to save hour slot:" << query.lastError().text();
    }
}

HourSlot ScheduleDb::getHourSlot(const QString& slotId) const
{
    HourSlot slot;
    
    if (!m_initialized) {
        return slot;
    }

    QSqlQuery query(m_db);
    query.prepare("SELECT * FROM hour_slots WHERE id = ?");
    query.addBindValue(slotId);

    if (query.exec() && query.next()) {
        slot.id = query.value("id").toString();
        slot.dayOfWeek = query.value("day_of_week").toInt();
        slot.hour = query.value("hour").toInt();

        QJsonDocument doc = QJsonDocument::fromJson(query.value("items").toByteArray());
        if (doc.isArray()) {
            QJsonArray itemsArray = doc.array();
            for (const QJsonValue& value : itemsArray) {
                slot.items.append(PlaylistItem::fromJson(value.toObject()));
            }
        }
    }

    return slot;
}

void ScheduleDb::deleteHourSlot(const QString& slotId)
{
    if (!m_initialized) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM hour_slots WHERE id = ?");
    query.addBindValue(slotId);
    query.exec();
}

void ScheduleDb::saveWeeklySchedule(const WeeklySchedule& schedule)
{
    if (!m_initialized) {
        return;
    }

    // Save all slots
    for (int i = 0; i < schedule.slotKeys.size() && i < schedule.slotValues.size(); ++i) {
        saveHourSlot(schedule.slotValues[i]);
    }

    // Save metadata
    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT OR REPLACE INTO schedule_metadata (key, value)
        VALUES ('weekly_schedule_version', ?)
    )");
    query.addBindValue(QString::number(QDateTime::currentMSecsSinceEpoch()));
    query.exec();
}

WeeklySchedule ScheduleDb::getWeeklySchedule() const
{
    WeeklySchedule schedule;

    if (!m_initialized) {
        return schedule;
    }

    QSqlQuery query(m_db);
    query.exec("SELECT id FROM hour_slots");

    while (query.next()) {
        QString slotId = query.value("id").toString();
        HourSlot slot = getHourSlot(slotId);
        schedule.slotKeys.append(slotId);
        schedule.slotValues.append(slot);
    }

    return schedule;
}

void ScheduleDb::saveFreePlaylist(const QString& playlistId, const QList<PlaylistItem>& items)
{
    if (!m_initialized) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT OR REPLACE INTO free_playlists 
        (id, name, items, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?)
    )");

    QJsonArray itemsArray;
    for (const PlaylistItem& item : items) {
        QJsonObject itemObj;
        itemObj["id"] = item.id;
        itemObj["mediaId"] = item.media.id;
        itemObj["timeIn"] = item.timeIn;
        itemObj["timeOut"] = item.timeOut;
        itemObj["priority"] = item.priority;
        itemsArray.append(itemObj);
    }

    query.addBindValue(playlistId);
    query.addBindValue(playlistId); // Use ID as name for now
    query.addBindValue(QJsonDocument(itemsArray).toJson());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());

    query.exec();
}

QList<PlaylistItem> ScheduleDb::getFreePlaylist(const QString& playlistId) const
{
    QList<PlaylistItem> items;

    if (!m_initialized) {
        return items;
    }

    QSqlQuery query(m_db);
    query.prepare("SELECT items FROM free_playlists WHERE id = ?");
    query.addBindValue(playlistId);

    if (query.exec() && query.next()) {
        QJsonDocument doc = QJsonDocument::fromJson(query.value("items").toByteArray());
        if (doc.isArray()) {
            QJsonArray itemsArray = doc.array();
            for (const QJsonValue& value : itemsArray) {
                QJsonObject itemObj = value.toObject();
                PlaylistItem item;
                item.id = itemObj["id"].toString();
                item.media.id = itemObj["mediaId"].toString();
                item.timeIn = itemObj["timeIn"].toDouble();
                item.timeOut = itemObj["timeOut"].toDouble();
                item.priority = itemObj["priority"].toInt();
                items.append(item);
            }
        }
    }

    return items;
}

void ScheduleDb::deleteFreePlaylist(const QString& playlistId)
{
    if (!m_initialized) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare("DELETE FROM free_playlists WHERE id = ?");
    query.addBindValue(playlistId);
    query.exec();
}

void ScheduleDb::saveBlockSchedule(const BlockSchedule& schedule)
{
    if (!m_initialized) {
        return;
    }

    QSqlQuery query(m_db);
    query.prepare(R"(
        DELETE FROM block_schedule
    )");
    query.exec();

    query.prepare(R"(
        INSERT INTO block_schedule (schedule_name, last_modified, blocks_data)
        VALUES (?, ?, ?)
    )");

    QJsonObject scheduleJson = schedule.toJson();
    query.addBindValue(schedule.scheduleName);
    query.addBindValue(schedule.lastModified.toMSecsSinceEpoch());
    query.addBindValue(QJsonDocument(scheduleJson).toJson());

    if (!query.exec()) {
        qWarning() << "Failed to save block schedule:" << query.lastError().text();
    }
}

BlockSchedule ScheduleDb::getBlockSchedule() const
{
    BlockSchedule schedule;

    if (!m_initialized) {
        return schedule;
    }

    QSqlQuery query(m_db);
    query.exec("SELECT * FROM block_schedule ORDER BY id DESC LIMIT 1");

    if (query.next()) {
        schedule.scheduleName = query.value("schedule_name").toString();
        schedule.lastModified = QDateTime::fromMSecsSinceEpoch(query.value("last_modified").toLongLong());

        QJsonDocument doc = QJsonDocument::fromJson(query.value("blocks_data").toByteArray());
        if (doc.isObject()) {
            schedule = BlockSchedule::fromJson(doc.object());
        }
    }

    return schedule;
}

