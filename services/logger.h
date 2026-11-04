#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

class Logger : public QObject
{
    Q_OBJECT
    
public:
    static Logger* instance();
    
    void initialize();
    void shutdown();
    
    void log(LogLevel level, const QString& message, const QString& category = QString());
    void debug(const QString& message, const QString& category = QString());
    void info(const QString& message, const QString& category = QString());
    void warning(const QString& message, const QString& category = QString());
    void error(const QString& message, const QString& category = QString());
    void critical(const QString& message, const QString& category = QString());
    
private:
    Logger(QObject* parent = nullptr);
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    QString logLevelToString(LogLevel level);
    void writeToFile(const QString& message);
    
    QFile* m_logFile;
    QTextStream* m_logStream;
    QMutex m_mutex;
    static Logger* s_instance;
};

#endif // LOGGER_H

