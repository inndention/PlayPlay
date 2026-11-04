#include "asrunlogger.h"
#include <QStandardPaths>
#include <QDir>
#include <QSqlQuery>
#include <QSqlError>
#include <QSqlRecord>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariant>
#include <QDateTime>
#include <QtMath>

static AsRunLogger* g_asRunLogger = nullptr;

AsRunLogger::AsRunLogger(QObject* parent)
    : QObject(parent)
{
}

AsRunLogger::~AsRunLogger()
{
    close();
}

bool AsRunLogger::initialize()
{
    if (m_initialized && m_db.isOpen()) return true;    // main, ControlService and ActivityLogModel all call this on the one singleton
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir dir;
    if (!dir.exists(dataPath)) {
        dir.mkpath(dataPath);
    }
    
    QString dbPath = dataPath + "/asrun.db";
    
    m_db = QSqlDatabase::addDatabase("QSQLITE", "asrun");
    m_db.setDatabaseName(dbPath);
    
    if (!m_db.open()) {
        qWarning() << "Failed to open as-run database:" << m_db.lastError().text();
        return false;
    }
    
    m_initialized = true;
    
    if (!ensureTables()) {
        return false;
    }
    
    return true;
}

void AsRunLogger::close()
{
    if (m_db.isValid()) {
        const QString name = m_db.connectionName();
        if (m_db.isOpen()) m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    m_initialized = false;
}

bool AsRunLogger::ensureTables()
{
    QSqlQuery query(m_db);
    
    // Check if table exists and has old schema
    query.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name='asrun_log'");
    bool tableExists = query.exec() && query.next();
    
    if (tableExists) {
        // Check if we need to migrate schema
        query.prepare("PRAGMA table_info(asrun_log)");
        if (query.exec()) {
            QStringList columns;
            while (query.next()) {
                columns << query.value(1).toString();
            }
            
            // Add missing columns if they don't exist
            if (!columns.contains("planned_end")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN planned_end INTEGER");
            }
            if (!columns.contains("actual_end")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN actual_end INTEGER");
            }
            if (!columns.contains("planned_duration")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN planned_duration INTEGER");
            }
            if (!columns.contains("actual_duration")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN actual_duration INTEGER");
            }
            if (!columns.contains("media_title")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN media_title TEXT");
            }
            if (!columns.contains("frame_offset")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN frame_offset INTEGER");
            }
            if (!columns.contains("timing_accuracy")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN timing_accuracy REAL");
            }
            if (!columns.contains("planned_start_frame")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN planned_start_frame INTEGER");
            }
            if (!columns.contains("actual_start_frame")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN actual_start_frame INTEGER");
            }
            if (!columns.contains("frame_rate")) {
                query.exec("ALTER TABLE asrun_log ADD COLUMN frame_rate INTEGER");
            }
        }
    } else {
        // Create new table with full schema
        QString createTable = R"(
            CREATE TABLE IF NOT EXISTS asrun_log (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id TEXT NOT NULL,
                planned_start INTEGER NOT NULL,
                actual_start INTEGER NOT NULL,
                planned_end INTEGER,
                actual_end INTEGER,
                planned_duration INTEGER,
                actual_duration INTEGER,
                engine_id TEXT,
                asset_id TEXT,
                media_title TEXT,
                status TEXT NOT NULL,
                reason TEXT,
                frame_offset INTEGER,
                timing_accuracy REAL,
                planned_start_frame INTEGER,
                actual_start_frame INTEGER,
                frame_rate INTEGER,
                metadata TEXT,
                created_at INTEGER NOT NULL
            )
        )";
        
        if (!query.exec(createTable)) {
            qWarning() << "Failed to create asrun_log table:" << query.lastError().text();
            return false;
        }
    }
    
    // Create indexes
    query.exec("CREATE INDEX IF NOT EXISTS idx_event_id ON asrun_log(event_id)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_planned_start ON asrun_log(planned_start)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_actual_start ON asrun_log(actual_start)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_asset_id ON asrun_log(asset_id)");
    query.exec("CREATE INDEX IF NOT EXISTS idx_status ON asrun_log(status)");
    
    return true;
}

void AsRunLogger::logEvent(const QString& eventId, qint64 plannedStart, qint64 actualStart,
                           const QString& engineId, const QString& assetId,
                           const QString& status, const QString& reason,
                           const QJsonObject& metadata)
{
    QSqlQuery query(m_db);
    query.prepare(R"(
        INSERT INTO asrun_log 
        (event_id, planned_start, actual_start, engine_id, asset_id, status, reason, metadata, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");
    
    query.addBindValue(eventId);
    query.addBindValue(plannedStart);
    query.addBindValue(actualStart);
    query.addBindValue(engineId);
    query.addBindValue(assetId);
    query.addBindValue(status);
    query.addBindValue(reason);
    query.addBindValue(QJsonDocument(metadata).toJson());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    
    if (!query.exec()) {
        qWarning() << "Failed to log event:" << query.lastError().text();
    }
}

QList<AsRunEntry> AsRunLogger::queryLog(const QJsonObject& filters) const
{
    QList<AsRunEntry> entries;
    
    QSqlQuery query(m_db);
    
    QString whereClause = "1=1";
    QVariantList bindValues;
    
    if (filters.contains("startTime")) {
        whereClause += " AND planned_start >= ?";
        bindValues.append(filters["startTime"].toVariant().toLongLong());
    }
    if (filters.contains("endTime")) {
        whereClause += " AND planned_start <= ?";
        bindValues.append(filters["endTime"].toVariant().toLongLong());
    }
    if (filters.contains("assetId")) {
        whereClause += " AND asset_id = ?";
        bindValues.append(filters["assetId"].toString());
    }
    if (filters.contains("status")) {
        whereClause += " AND status = ?";
        bindValues.append(filters["status"].toString());
    }
    query.prepare(QString(R"(
        SELECT event_id, planned_start, actual_start, planned_end, actual_end,
               planned_duration, actual_duration, engine_id, asset_id, media_title,
               status, reason, frame_offset, timing_accuracy,
               planned_start_frame, actual_start_frame, frame_rate, metadata, created_at
        FROM asrun_log
        WHERE %1
        ORDER BY planned_start ASC
    )").arg(whereClause));
    
    for (const QVariant& value : bindValues) {
        query.addBindValue(value);
    }
    
    if (!query.exec()) {
        qWarning() << "Failed to query as-run log:" << query.lastError().text();
        return entries;
    }
    
    while (query.next()) {
        AsRunEntry entry;
        entry.eventId = query.value(0).toString();
        entry.plannedStart = query.value(1).toLongLong();
        entry.actualStart = query.value(2).toLongLong();
        entry.engineId = query.value(3).toString();
        entry.assetId = query.value(4).toString();
        entry.status = query.value(5).toString();
        entry.reason = query.value(6).toString();
        
        QJsonDocument doc = QJsonDocument::fromJson(query.value(7).toByteArray());
        if (doc.isObject()) {
            entry.metadata = doc.object();
        }
        
        // Load frame-accurate fields if available
        if (query.record().count() > 8) {
            entry.plannedEnd = query.value(8).toLongLong();
            entry.actualEnd = query.value(9).toLongLong();
            entry.plannedDuration = query.value(10).toLongLong();
            entry.actualDuration = query.value(11).toLongLong();
            entry.mediaTitle = query.value(12).toString();
            entry.frameOffset = query.value(13).toInt();
            entry.timingAccuracy = query.value(14).toDouble();
            entry.plannedStartFrame = query.value(15).toInt();
            entry.actualStartFrame = query.value(16).toInt();
            entry.frameRate = query.value(17).toInt();
        }
        
        entries.append(entry);
    }
    
    return entries;
}

QList<AsRunEntry> AsRunLogger::queryLogByDateRange(const QDateTime& start, const QDateTime& end) const
{
    QJsonObject filters;
    filters["startTime"] = start.toMSecsSinceEpoch();
    filters["endTime"] = end.toMSecsSinceEpoch();
    return queryLog(filters);
}

QList<AsRunEntry> AsRunLogger::queryLogByAsset(const QString& assetId) const
{
    QJsonObject filters;
    filters["assetId"] = assetId;
    return queryLog(filters);
}

QList<AsRunEntry> AsRunLogger::queryLogByStatus(const QString& status) const
{
    QJsonObject filters;
    filters["status"] = status;
    return queryLog(filters);
}

void AsRunLogger::logEventFrameAccurate(const QString& eventId, qint64 plannedStart, qint64 actualStart,
                                        qint64 plannedEnd, qint64 actualEnd,
                                        int plannedStartFrame, int actualStartFrame,
                                        int frameRate, const QString& engineId, const QString& assetId,
                                        const QString& mediaTitle, const QString& status,
                                        const QString& reason, const QJsonObject& metadata)
{
    QSqlQuery query(m_db);
    
    qint64 plannedDuration = plannedEnd - plannedStart;
    qint64 actualDuration = actualEnd - actualStart;
    int frameOffset = actualStartFrame - plannedStartFrame;
    double timingAccuracy = static_cast<double>(actualStart - plannedStart);
    
    query.prepare(R"(
        INSERT INTO asrun_log 
        (event_id, planned_start, actual_start, planned_end, actual_end,
         planned_duration, actual_duration, engine_id, asset_id, media_title,
         status, reason, frame_offset, timing_accuracy,
         planned_start_frame, actual_start_frame, frame_rate, metadata, created_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )");
    
    query.addBindValue(eventId);
    query.addBindValue(plannedStart);
    query.addBindValue(actualStart);
    query.addBindValue(plannedEnd);
    query.addBindValue(actualEnd);
    query.addBindValue(plannedDuration);
    query.addBindValue(actualDuration);
    query.addBindValue(engineId);
    query.addBindValue(assetId);
    query.addBindValue(mediaTitle);
    query.addBindValue(status);
    query.addBindValue(reason);
    query.addBindValue(frameOffset);
    query.addBindValue(timingAccuracy);
    query.addBindValue(plannedStartFrame);
    query.addBindValue(actualStartFrame);
    query.addBindValue(frameRate);
    query.addBindValue(QJsonDocument(metadata).toJson());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    
    if (!query.exec()) {
        qWarning() << "Failed to log frame-accurate event:" << query.lastError().text();
    }
}

QList<QJsonObject> AsRunLogger::generateTimingReport(qint64 startTime, qint64 endTime)
{
    QList<QJsonObject> report;
    QList<AsRunEntry> entries = queryLogByDateRange(QDateTime::fromMSecsSinceEpoch(startTime), 
                                                     QDateTime::fromMSecsSinceEpoch(endTime));
    
    for (const AsRunEntry& entry : entries) {
        QJsonObject item;
        item["eventId"] = entry.eventId;
        item["assetId"] = entry.assetId;
        item["mediaTitle"] = entry.mediaTitle;
        item["plannedStart"] = entry.plannedStart;
        item["actualStart"] = entry.actualStart;
        item["timingDifference"] = entry.actualStart - entry.plannedStart;
        item["frameOffset"] = entry.frameOffset;
        item["timingAccuracy"] = entry.timingAccuracy;
        item["status"] = entry.status;
        report.append(item);
    }
    
    return report;
}

QList<QJsonObject> AsRunLogger::generateFrameAccuracyReport(qint64 startTime, qint64 endTime)
{
    QList<QJsonObject> report;
    QList<AsRunEntry> entries = queryLogByDateRange(QDateTime::fromMSecsSinceEpoch(startTime), 
                                                     QDateTime::fromMSecsSinceEpoch(endTime));
    
    for (const AsRunEntry& entry : entries) {
        QJsonObject item;
        item["eventId"] = entry.eventId;
        item["assetId"] = entry.assetId;
        item["plannedStartFrame"] = entry.plannedStartFrame;
        item["actualStartFrame"] = entry.actualStartFrame;
        item["frameOffset"] = entry.frameOffset;
        item["frameRate"] = entry.frameRate;
        item["frameOffsetMs"] = entry.frameOffset * (1000.0 / entry.frameRate);
        item["status"] = entry.status;
        report.append(item);
    }
    
    return report;
}

QJsonObject AsRunLogger::generateSummaryReport(qint64 startTime, qint64 endTime)
{
    QList<AsRunEntry> entries = queryLogByDateRange(QDateTime::fromMSecsSinceEpoch(startTime), 
                                                     QDateTime::fromMSecsSinceEpoch(endTime));
    
    int totalEvents = entries.size();
    int onTime = 0;
    int lateStart = 0;
    int overrun = 0;
    int underrun = 0;
    int failed = 0;
    
    double totalTimingError = 0.0;
    double totalFrameOffset = 0.0;
    
    for (const AsRunEntry& entry : entries) {
        if (entry.status == "ON_TIME") onTime++;
        else if (entry.status == "LATE_START") lateStart++;
        else if (entry.status == "OVERRUN") overrun++;
        else if (entry.status == "UNDERRUN") underrun++;
        else if (entry.status == "FAILED") failed++;
        
        totalTimingError += qAbs(entry.timingAccuracy);
        totalFrameOffset += qAbs(entry.frameOffset);
    }
    
    QJsonObject summary;
    summary["startTime"] = startTime;
    summary["endTime"] = endTime;
    summary["totalEvents"] = totalEvents;
    summary["onTime"] = onTime;
    summary["lateStart"] = lateStart;
    summary["overrun"] = overrun;
    summary["underrun"] = underrun;
    summary["failed"] = failed;
    summary["averageTimingError"] = totalEvents > 0 ? totalTimingError / totalEvents : 0.0;
    summary["averageFrameOffset"] = totalEvents > 0 ? totalFrameOffset / totalEvents : 0.0;
    summary["onTimePercentage"] = totalEvents > 0 ? (onTime * 100.0 / totalEvents) : 0.0;
    
    return summary;
}

QList<QJsonObject> AsRunLogger::generateDiscrepancyReport(qint64 startTime, qint64 endTime)
{
    QList<QJsonObject> report;
    QList<AsRunEntry> entries = queryLogByDateRange(QDateTime::fromMSecsSinceEpoch(startTime), 
                                                     QDateTime::fromMSecsSinceEpoch(endTime));
    
    for (const AsRunEntry& entry : entries) {
        if (entry.status != "ON_TIME") {
            QJsonObject item;
            item["eventId"] = entry.eventId;
            item["assetId"] = entry.assetId;
            item["mediaTitle"] = entry.mediaTitle;
            item["plannedStart"] = entry.plannedStart;
            item["actualStart"] = entry.actualStart;
            item["timingDifference"] = entry.actualStart - entry.plannedStart;
            item["frameOffset"] = entry.frameOffset;
            item["status"] = entry.status;
            item["reason"] = entry.reason;
            report.append(item);
        }
    }
    
    return report;
}

QJsonObject AsRunEntry::toJson() const
{
    QJsonObject obj;
    obj["eventId"] = eventId;
    obj["plannedStart"] = plannedStart;
    obj["actualStart"] = actualStart;
    obj["plannedEnd"] = plannedEnd;
    obj["actualEnd"] = actualEnd;
    obj["plannedDuration"] = plannedDuration;
    obj["actualDuration"] = actualDuration;
    obj["engineId"] = engineId;
    obj["assetId"] = assetId;
    obj["mediaTitle"] = mediaTitle;
    obj["status"] = status;
    obj["reason"] = reason;
    obj["frameOffset"] = frameOffset;
    obj["timingAccuracy"] = timingAccuracy;
    obj["plannedStartFrame"] = plannedStartFrame;
    obj["actualStartFrame"] = actualStartFrame;
    obj["frameRate"] = frameRate;
    obj["metadata"] = metadata;
    obj["timestamp"] = timestamp;
    return obj;
}

AsRunEntry AsRunEntry::fromJson(const QJsonObject& json)
{
    AsRunEntry entry;
    entry.eventId = json["eventId"].toString();
    entry.plannedStart = json["plannedStart"].toVariant().toLongLong();
    entry.actualStart = json["actualStart"].toVariant().toLongLong();
    entry.plannedEnd = json["plannedEnd"].toVariant().toLongLong();
    entry.actualEnd = json["actualEnd"].toVariant().toLongLong();
    entry.plannedDuration = json["plannedDuration"].toVariant().toLongLong();
    entry.actualDuration = json["actualDuration"].toVariant().toLongLong();
    entry.engineId = json["engineId"].toString();
    entry.assetId = json["assetId"].toString();
    entry.mediaTitle = json["mediaTitle"].toString();
    entry.status = json["status"].toString();
    entry.reason = json["reason"].toString();
    entry.frameOffset = json["frameOffset"].toInt();
    entry.timingAccuracy = json["timingAccuracy"].toDouble();
    entry.plannedStartFrame = json["plannedStartFrame"].toInt();
    entry.actualStartFrame = json["actualStartFrame"].toInt();
    entry.frameRate = json["frameRate"].toInt();
    entry.metadata = json["metadata"].toObject();
    entry.timestamp = json["timestamp"].toVariant().toLongLong();
    return entry;
}

AsRunLogger* getAsRunLogger()
{
    if (!g_asRunLogger) {
        g_asRunLogger = new AsRunLogger();
    }
    return g_asRunLogger;
}

