#include "mediadb.h"
#include "core/types.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QDate>
#include <atomic>

MediaDb::MediaDb() {
}

MediaDb::~MediaDb() {
    close();
}

bool MediaDb::initialize() {
    if (db.isValid() && db.isOpen()) return true;       // already open: a second call must not register a second connection
    // Several objects (the models, the importer, main) each own a MediaDb: every instance gets its own connection name,
    // otherwise addDatabase() replaces the connection another instance is still using.
    static std::atomic<int> serial{0};
    QString userDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(userDataPath);
    
    QString dbFile = userDataPath + "/media.db";
    db = QSqlDatabase::addDatabase("QSQLITE", QString("MediaDb-%1").arg(++serial));
    db.setDatabaseName(dbFile);
    
    if (!db.open()) {
        qWarning() << "Failed to open media database:" << db.lastError().text();
        return false;
    }
    
    // Enable WAL mode
    QSqlQuery query(db);
    query.exec("PRAGMA journal_mode = WAL");
    
    return ensureTables();
}

void MediaDb::close() {
    if (!db.isValid()) return;
    const QString name = db.connectionName();
    if (db.isOpen()) db.close();
    db = QSqlDatabase();                                 // drop our handle first: removeDatabase() complains while one is alive
    QSqlDatabase::removeDatabase(name);
}

bool MediaDb::ensureTables() {
    QSqlQuery query(db);
    
    // Media table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS media (
            id TEXT PRIMARY KEY,
            title TEXT,
            duration REAL,
            src TEXT,
            thumbnail TEXT,
            liveSource TEXT,
            filePath TEXT,
            filename TEXT,
            type TEXT,
            client TEXT,
            programSeries TEXT,
            episodeDate TEXT,
            notes TEXT,
            addedAt TEXT,
            dateMade TEXT,
            lastPlayedAt TEXT,
            playCount INTEGER DEFAULT 0,
            totalPlayTime REAL DEFAULT 0,
            priority INTEGER DEFAULT 0,
            archived INTEGER DEFAULT 0
        )
    )");
    
    if (query.lastError().isValid()) {
        qWarning() << "Failed to create media table:" << query.lastError().text();
        return false;
    }
    
    // Create indexes
    query.exec("CREATE INDEX IF NOT EXISTS idx_media_filename ON media(filename)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_media_title ON media(title)");
    
    // Schedule slots table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS schedule_slots (
            id TEXT PRIMARY KEY,
            dayOfWeek INTEGER,
            hour INTEGER,
            items TEXT,
            isLooping INTEGER DEFAULT 0,
            randomizeAds INTEGER DEFAULT 1,
            keepProgramOrder INTEGER DEFAULT 1,
            updatedAt TEXT
        )
    )");
    
    // Schedule meta table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS schedule_meta (
            key TEXT PRIMARY KEY,
            value TEXT
        )
    )");
    
    // Free playlists table
    query.exec(R"(
        CREATE TABLE IF NOT EXISTS free_playlists (
            id TEXT PRIMARY KEY,
            name TEXT,
            items TEXT,
            isLocked INTEGER DEFAULT 0,
            createdAt INTEGER,
            createdFrom TEXT
        )
    )");
    
    // Migration: Check for thumbnail column
    QSqlQuery pragmaQuery(db);
    pragmaQuery.exec("PRAGMA table_info(media)");
    bool hasThumbnail = false;
    while (pragmaQuery.next()) {
        if (pragmaQuery.value("name").toString().toLower() == "thumbnail") {
            hasThumbnail = true;
            break;
        }
    }
    
    if (!hasThumbnail) {
        query.exec("ALTER TABLE media ADD COLUMN thumbnail TEXT");
    }
    
    return true;
}

bool MediaDb::saveMedia(const MediaItem& item) {
    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO media (
            id, title, duration, src, thumbnail, liveSource, filePath, filename, type, client,
            programSeries, episodeDate, notes, addedAt, dateMade,
            lastPlayedAt, playCount, totalPlayTime, priority, archived
        ) VALUES (
            :id, :title, :duration, :src, :thumbnail, :liveSource, :filePath, :filename, :type, :client,
            :programSeries, :episodeDate, :notes, :addedAt, :dateMade,
            :lastPlayedAt, :playCount, :totalPlayTime, :priority, :archived
        )
        ON CONFLICT(id) DO UPDATE SET
            title = excluded.title,
            duration = excluded.duration,
            src = excluded.src,
            thumbnail = excluded.thumbnail,
            liveSource = excluded.liveSource,
            filePath = excluded.filePath,
            filename = excluded.filename,
            type = excluded.type,
            client = excluded.client,
            programSeries = excluded.programSeries,
            episodeDate = excluded.episodeDate,
            notes = excluded.notes,
            addedAt = excluded.addedAt,
            dateMade = excluded.dateMade,
            lastPlayedAt = excluded.lastPlayedAt,
            playCount = excluded.playCount,
            totalPlayTime = excluded.totalPlayTime,
            priority = excluded.priority,
            archived = excluded.archived
    )");
    
    query.bindValue(":id", item.id);
    query.bindValue(":title", item.title);
    query.bindValue(":duration", item.duration);
    query.bindValue(":src", item.src.toString());
    query.bindValue(":thumbnail", item.thumbnail);
    
    if (item.liveSource.has_value()) {
        QJsonObject liveObj;
        liveObj["kind"] = static_cast<int>(item.liveSource->kind);
        liveObj["name"] = item.liveSource->name;
        liveObj["ndiName"] = item.liveSource->ndiName;
        liveObj["device"] = item.liveSource->device;
        liveObj["url"] = item.liveSource->url.toString();
        liveObj["obsScene"] = item.liveSource->obsScene;
        liveObj["pipeline"] = item.liveSource->pipeline;
        query.bindValue(":liveSource", QJsonDocument(liveObj).toJson());
    } else {
        query.bindValue(":liveSource", QVariant());
    }
    
    query.bindValue(":filePath", item.filePath);
    query.bindValue(":filename", item.filename);
    query.bindValue(":type", mediaTypeToString(item.type));
    query.bindValue(":client", item.client);
    query.bindValue(":programSeries", item.programSeries);
    query.bindValue(":episodeDate", item.episodeDate.isValid() ? item.episodeDate.toString(Qt::ISODate) : QString());
    query.bindValue(":notes", item.notes);
    query.bindValue(":addedAt", item.addedAt.isValid() ? item.addedAt.toString(Qt::ISODate) : QString());
    query.bindValue(":dateMade", item.dateMade.isValid() ? item.dateMade.toString(Qt::ISODate) : QString());
    query.bindValue(":lastPlayedAt", item.lastPlayedAt.isValid() ? item.lastPlayedAt.toString(Qt::ISODate) : QString());
    query.bindValue(":playCount", item.playCount);
    query.bindValue(":totalPlayTime", item.duration * item.playCount);
    query.bindValue(":priority", item.priority);
    query.bindValue(":archived", item.archived ? 1 : 0);
    
    if (!query.exec()) {
        qWarning() << "Failed to save media:" << query.lastError().text();
        return false;
    }
    
    return true;
}

bool MediaDb::deleteMedia(const QString& id) {
    QSqlQuery query(db);
    query.prepare("DELETE FROM media WHERE id = ?");
    query.addBindValue(id);
    return query.exec();
}

QList<MediaItem> MediaDb::getAllMedia() {
    QList<MediaItem> items;
    QSqlQuery query(db);
    query.exec("SELECT * FROM media ORDER BY addedAt DESC");
    
    while (query.next()) {
        MediaItem item;
        item.id = query.value("id").toString();
        item.title = query.value("title").toString();
        item.duration = query.value("duration").toDouble();
        item.src = QUrl(query.value("src").toString());
        item.type = stringToMediaType(query.value("type").toString());
        item.thumbnail = query.value("thumbnail").toString();
        item.filePath = query.value("filePath").toString();
        item.filename = query.value("filename").toString();
        item.client = query.value("client").toString();
        item.programSeries = query.value("programSeries").toString();
        item.episodeDate = QDate::fromString(query.value("episodeDate").toString(), Qt::ISODate);
        item.notes = query.value("notes").toString();
        
        QString liveSourceJson = query.value("liveSource").toString();
        if (!liveSourceJson.isEmpty()) {
            QJsonDocument doc = QJsonDocument::fromJson(liveSourceJson.toUtf8());
            QJsonObject liveObj = doc.object();
            LiveSourceConfig live;
            live.kind = static_cast<LiveSourceKind>(liveObj["kind"].toInt());
            live.name = liveObj["name"].toString();
            live.ndiName = liveObj["ndiName"].toString();
            live.device = liveObj["device"].toString();
            live.url = QUrl(liveObj["url"].toString());
            live.obsScene = liveObj["obsScene"].toString();
            live.pipeline = liveObj["pipeline"].toString();
            item.liveSource = live;
        }
        
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.dateMade = QDateTime::fromString(query.value("dateMade").toString(), Qt::ISODate);
        item.lastPlayedAt = QDateTime::fromString(query.value("lastPlayedAt").toString(), Qt::ISODate);
        item.playCount = query.value("playCount").toInt();
        item.priority = query.value("priority").toInt();
        item.archived = query.value("archived").toInt() == 1;
        
        items.append(item);
    }
    
    return items;
}

MediaItem MediaDb::getMediaById(const QString& id) {
    QSqlQuery query(db);
    query.prepare("SELECT * FROM media WHERE id = ?");
    query.addBindValue(id);
    query.exec();
    
    if (query.next()) {
        MediaItem item;
        item.id = query.value("id").toString();
        item.title = query.value("title").toString();
        item.duration = query.value("duration").toDouble();
        item.src = QUrl(query.value("src").toString());
        item.type = stringToMediaType(query.value("type").toString());
        item.thumbnail = query.value("thumbnail").toString();
        item.filePath = query.value("filePath").toString();
        item.filename = query.value("filename").toString();
        item.client = query.value("client").toString();
        item.programSeries = query.value("programSeries").toString();
        item.episodeDate = QDate::fromString(query.value("episodeDate").toString(), Qt::ISODate);
        item.notes = query.value("notes").toString();
        item.addedAt = QDateTime::fromString(query.value("addedAt").toString(), Qt::ISODate);
        item.dateMade = QDateTime::fromString(query.value("dateMade").toString(), Qt::ISODate);
        item.lastPlayedAt = QDateTime::fromString(query.value("lastPlayedAt").toString(), Qt::ISODate);
        item.playCount = query.value("playCount").toInt();
        item.priority = query.value("priority").toInt();
        item.archived = query.value("archived").toInt() == 1;
        return item;
    }
    
    return MediaItem();
}

QList<MediaItem> MediaDb::findByFilename(const QString& queryStr) {
    QList<MediaItem> items;
    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM media
        WHERE LOWER(filename) LIKE ? OR LOWER(title) LIKE ?
        ORDER BY addedAt DESC
    )");
    QString pattern = "%" + queryStr.toLower() + "%";
    query.addBindValue(pattern);
    query.addBindValue(pattern);
    query.exec();
    
    while (query.next()) {
        MediaItem item;
        item.id = query.value("id").toString();
        item.title = query.value("title").toString();
        item.duration = query.value("duration").toDouble();
        item.src = QUrl(query.value("src").toString());
        item.type = stringToMediaType(query.value("type").toString());
        item.filePath = query.value("filePath").toString();
        item.filename = query.value("filename").toString();
        items.append(item);
    }
    
    return items;
}

bool MediaDb::incrementPlay(const QString& mediaId, double durationSeconds) {
    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE media
        SET playCount = playCount + 1,
            totalPlayTime = totalPlayTime + ?,
            lastPlayedAt = ?
        WHERE id = ?
    )");
    query.addBindValue(durationSeconds);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    query.addBindValue(mediaId);
    return query.exec();
}

bool MediaDb::saveScheduleSlot(const HourSlot& slot) {
    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO schedule_slots (
            id, dayOfWeek, hour, items, isLooping, randomizeAds, keepProgramOrder, updatedAt
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(id) DO UPDATE SET
            dayOfWeek = excluded.dayOfWeek,
            hour = excluded.hour,
            items = excluded.items,
            isLooping = excluded.isLooping,
            randomizeAds = excluded.randomizeAds,
            keepProgramOrder = excluded.keepProgramOrder,
            updatedAt = excluded.updatedAt
    )");
    
    QJsonArray itemsArray;
    for (const auto& item : slot.items) {
        itemsArray.append(item.toJson());
    }
    QJsonDocument doc(itemsArray);
    
    query.addBindValue(slot.id);
    query.addBindValue(slot.dayOfWeek);
    query.addBindValue(slot.hour);
    query.addBindValue(doc.toJson());
    query.addBindValue(slot.isLooping ? 1 : 0);
    query.addBindValue(slot.randomizeAds ? 1 : 0);
    query.addBindValue(slot.keepProgramOrder ? 1 : 0);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    
    return query.exec();
}

HourSlot MediaDb::getScheduleSlot(const QString& slotId) {
    QSqlQuery query(db);
    query.prepare("SELECT * FROM schedule_slots WHERE id = ?");
    query.addBindValue(slotId);
    query.exec();
    
    if (query.next()) {
        HourSlot slot;
        slot.id = query.value("id").toString();
        slot.dayOfWeek = query.value("dayOfWeek").toInt();
        slot.hour = query.value("hour").toInt();
        slot.isLooping = query.value("isLooping").toInt() == 1;
        slot.randomizeAds = query.value("randomizeAds").toInt() == 1;
        slot.keepProgramOrder = query.value("keepProgramOrder").toInt() == 1;
        slot.updatedAt = QDateTime::fromString(query.value("updatedAt").toString(), Qt::ISODate);
        
        QJsonDocument doc = QJsonDocument::fromJson(query.value("items").toString().toUtf8());
        QJsonArray itemsArray = doc.array();
        for (const auto& val : itemsArray) {
            slot.items.append(PlaylistItem::fromJson(val.toObject()));
        }
        
        return slot;
    }
    
    return HourSlot();
}

WeeklySchedule MediaDb::getAllSlots() {
    WeeklySchedule schedule;
    QSqlQuery query(db);
    query.exec("SELECT * FROM schedule_slots ORDER BY dayOfWeek, hour");
    
    while (query.next()) {
        HourSlot slot;
        slot.id = query.value("id").toString();
        slot.dayOfWeek = query.value("dayOfWeek").toInt();
        slot.hour = query.value("hour").toInt();
        slot.isLooping = query.value("isLooping").toInt() == 1;
        slot.randomizeAds = query.value("randomizeAds").toInt() == 1;
        slot.keepProgramOrder = query.value("keepProgramOrder").toInt() == 1;
        slot.updatedAt = QDateTime::fromString(query.value("updatedAt").toString(), Qt::ISODate);
        
        QJsonDocument doc = QJsonDocument::fromJson(query.value("items").toString().toUtf8());
        QJsonArray itemsArray = doc.array();
        for (int i = 0; i < itemsArray.size(); ++i) {
            slot.items.append(PlaylistItem::fromJson(itemsArray[i].toObject()));
        }
        
        schedule.slotKeys.append(slot.id);
        schedule.slotValues.append(slot);
    }
    
    return schedule;
}

bool MediaDb::deleteScheduleSlot(const QString& slotId) {
    QSqlQuery query(db);
    query.prepare("DELETE FROM schedule_slots WHERE id = ?");
    query.addBindValue(slotId);
    return query.exec();
}

void MediaDb::setMeta(const QString& key, const QString& value) {
    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO schedule_meta (key, value)
        VALUES (?, ?)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
    )");
    query.addBindValue(key);
    query.addBindValue(value);
    query.exec();
}

QString MediaDb::getMeta(const QString& key) {
    QSqlQuery query(db);
    query.prepare("SELECT value FROM schedule_meta WHERE key = ?");
    query.addBindValue(key);
    query.exec();
    
    if (query.next()) {
        return query.value("value").toString();
    }
    
    return QString();
}

QMap<QString, QString> MediaDb::getAllMeta() {
    QMap<QString, QString> meta;
    QSqlQuery query(db);
    query.exec("SELECT key, value FROM schedule_meta");
    
    while (query.next()) {
        meta[query.value("key").toString()] = query.value("value").toString();
    }
    
    return meta;
}

