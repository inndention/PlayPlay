#ifndef PERSISTENTLOGGER_H
#define PERSISTENTLOGGER_H

#include <QObject>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QDateTime>

enum class PersistentLogLevel {
    Debug,
    Info,
    Warn,
    Error
};

struct LogEntry {
    qint64 timestamp;
    PersistentLogLevel level;
    QString message;
    QJsonObject data;
    QString component;
    QString stack;
    
    QJsonObject toJson() const;
    static LogEntry fromJson(const QJsonObject& json);
};

class PersistentLogger : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<LogEntry> logs READ getLogs NOTIFY logsChanged)
    
public:
    static PersistentLogger* instance();
    
    // Log methods
    void debug(const QString& message, const QJsonObject& data = QJsonObject(), const QString& component = QString());
    void info(const QString& message, const QJsonObject& data = QJsonObject(), const QString& component = QString());
    void warn(const QString& message, const QJsonObject& data = QJsonObject(), const QString& component = QString());
    void error(const QString& message, const QJsonObject& data = QJsonObject(), const QString& component = QString());
    
    // Get logs
    QList<LogEntry> getLogs() const;
    QList<LogEntry> getLogs(PersistentLogLevel level) const;
    QList<LogEntry> getLogs(const QString& component) const;
    
    // Clear logs
    void clearLogs();
    
    // Get logs as JSON (for QML)
    QJsonArray getLogsJson() const;
    
signals:
    void logAdded(const LogEntry& entry);
    void logsChanged();
    
private:
    PersistentLogger(QObject* parent = nullptr);
    ~PersistentLogger();
    PersistentLogger(const PersistentLogger&) = delete;
    PersistentLogger& operator=(const PersistentLogger&) = delete;
    
    void addLog(PersistentLogLevel level, const QString& message, const QJsonObject& data, const QString& component);
    void saveLogs();
    void loadLogs();
    
    QList<LogEntry> m_logs;
    static const int MAX_ENTRIES = 1000;
    static PersistentLogger* s_instance;
    
    qint64 m_lastSaveTime;
    QTimer* m_saveTimer;
};

#endif // PERSISTENTLOGGER_H

