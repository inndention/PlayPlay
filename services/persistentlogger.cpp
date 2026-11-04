#include "persistentlogger.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QTimer>

PersistentLogger* PersistentLogger::s_instance = nullptr;

PersistentLogger::PersistentLogger(QObject* parent)
    : QObject(parent)
    , m_lastSaveTime(0)
    , m_saveTimer(new QTimer(this))
{
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(500); // 500ms debounce
    connect(m_saveTimer, &QTimer::timeout, this, &PersistentLogger::saveLogs);
    
    loadLogs();
}

PersistentLogger::~PersistentLogger()
{
    saveLogs();
}

PersistentLogger* PersistentLogger::instance()
{
    if (!s_instance) {
        s_instance = new PersistentLogger();
    }
    return s_instance;
}

void PersistentLogger::debug(const QString& message, const QJsonObject& data, const QString& component)
{
    addLog(PersistentLogLevel::Debug, message, data, component);
}

void PersistentLogger::info(const QString& message, const QJsonObject& data, const QString& component)
{
    addLog(PersistentLogLevel::Info, message, data, component);
}

void PersistentLogger::warn(const QString& message, const QJsonObject& data, const QString& component)
{
    addLog(PersistentLogLevel::Warn, message, data, component);
}

void PersistentLogger::error(const QString& message, const QJsonObject& data, const QString& component)
{
    addLog(PersistentLogLevel::Error, message, data, component);
}

QList<LogEntry> PersistentLogger::getLogs() const
{
    return m_logs;
}

QList<LogEntry> PersistentLogger::getLogs(PersistentLogLevel level) const
{
    QList<LogEntry> filtered;
    for (const LogEntry& entry : m_logs) {
        if (entry.level == level) {
            filtered.append(entry);
        }
    }
    return filtered;
}

QList<LogEntry> PersistentLogger::getLogs(const QString& component) const
{
    QList<LogEntry> filtered;
    for (const LogEntry& entry : m_logs) {
        if (entry.component == component) {
            filtered.append(entry);
        }
    }
    return filtered;
}

void PersistentLogger::clearLogs()
{
    m_logs.clear();
    saveLogs();
    emit logsChanged();
}

QJsonArray PersistentLogger::getLogsJson() const
{
    QJsonArray array;
    for (const LogEntry& entry : m_logs) {
        array.append(entry.toJson());
    }
    return array;
}

void PersistentLogger::addLog(PersistentLogLevel level, const QString& message, const QJsonObject& data, const QString& component)
{
    LogEntry entry;
    entry.timestamp = QDateTime::currentMSecsSinceEpoch();
    entry.level = level;
    entry.message = message;
    entry.data = data;
    entry.component = component;
    
    // Capture stack trace if available (Qt doesn't have built-in stack trace, so this is simplified)
    // In a real implementation, you might want to use a library like backward-cpp
    
    m_logs.append(entry);
    
    // Keep only last MAX_ENTRIES
    if (m_logs.size() > MAX_ENTRIES) {
        m_logs.removeFirst();
    }
    
    // Debounced save
    m_saveTimer->start();
    
    emit logAdded(entry);
    emit logsChanged();
}

void PersistentLogger::saveLogs()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + "/persistent_logs.json";
    
    // Keep only recent entries
    QList<LogEntry> logsToSave = m_logs;
    if (logsToSave.size() > MAX_ENTRIES) {
        logsToSave = logsToSave.mid(logsToSave.size() - MAX_ENTRIES);
    }
    
    QJsonArray array;
    for (const LogEntry& entry : logsToSave) {
        array.append(entry.toJson());
    }
    
    QJsonDocument doc(array);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << doc.toJson(QJsonDocument::Compact);
        file.close();
    }
    
    m_lastSaveTime = QDateTime::currentMSecsSinceEpoch();
}

void PersistentLogger::loadLogs()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = dataPath + "/persistent_logs.json";
    
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse persistent logs:" << error.errorString();
        return;
    }
    
    QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        LogEntry entry = LogEntry::fromJson(value.toObject());
        m_logs.append(entry);
    }
    
    // Keep only last MAX_ENTRIES
    if (m_logs.size() > MAX_ENTRIES) {
        m_logs = m_logs.mid(m_logs.size() - MAX_ENTRIES);
    }
}

QJsonObject LogEntry::toJson() const
{
    QJsonObject json;
    json["timestamp"] = timestamp;
    json["level"] = static_cast<int>(level);
    json["message"] = message;
    json["data"] = data;
    json["component"] = component;
    json["stack"] = stack;
    return json;
}

LogEntry LogEntry::fromJson(const QJsonObject& json)
{
    LogEntry entry;
    entry.timestamp = json["timestamp"].toVariant().toLongLong();
    entry.level = static_cast<PersistentLogLevel>(json["level"].toInt());
    entry.message = json["message"].toString();
    entry.data = json["data"].toObject();
    entry.component = json["component"].toString();
    entry.stack = json["stack"].toString();
    return entry;
}

