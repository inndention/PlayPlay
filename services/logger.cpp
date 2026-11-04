#include "logger.h"
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QDebug>

Logger* Logger::s_instance = nullptr;

Logger::Logger(QObject* parent)
    : QObject(parent)
    , m_logFile(nullptr)
    , m_logStream(nullptr)
{
}

Logger::~Logger()
{
    shutdown();
}

Logger* Logger::instance()
{
    if (!s_instance) {
        s_instance = new Logger();
    }
    return s_instance;
}

void Logger::initialize()
{
    QMutexLocker locker(&m_mutex);
    
    QString logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/logs";
    QDir().mkpath(logDir);
    
    QString logFile = logDir + "/playplay_" + QDateTime::currentDateTime().toString("yyyyMMdd") + ".log";
    m_logFile = new QFile(logFile);
    
    if (m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_logStream = new QTextStream(m_logFile);
    }
}

void Logger::shutdown()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_logStream) {
        delete m_logStream;
        m_logStream = nullptr;
    }
    
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
}

QString Logger::logLevelToString(LogLevel level)
{
    switch(level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Critical: return "CRITICAL";
    }
    return "UNKNOWN";
}

void Logger::writeToFile(const QString& message)
{
    if (m_logStream && m_logFile && m_logFile->isOpen()) {
        *m_logStream << message << Qt::endl;
        m_logStream->flush();
    }
}

void Logger::log(LogLevel level, const QString& message, const QString& category)
{
    QMutexLocker locker(&m_mutex);
    
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString categoryStr = category.isEmpty() ? "General" : category;
    QString logMessage = QString("[%1] [%2] [%3] %4")
        .arg(timestamp)
        .arg(logLevelToString(level))
        .arg(categoryStr)
        .arg(message);
    
    // Write to console
    switch(level) {
        case LogLevel::Debug:
            qDebug() << logMessage;
            break;
        case LogLevel::Info:
            qInfo() << logMessage;
            break;
        case LogLevel::Warning:
            qWarning() << logMessage;
            break;
        case LogLevel::Error:
        case LogLevel::Critical:
            qCritical() << logMessage;
            break;
    }
    
    // Write to file
    writeToFile(logMessage);
}

void Logger::debug(const QString& message, const QString& category)
{
    log(LogLevel::Debug, message, category);
}

void Logger::info(const QString& message, const QString& category)
{
    log(LogLevel::Info, message, category);
}

void Logger::warning(const QString& message, const QString& category)
{
    log(LogLevel::Warning, message, category);
}

void Logger::error(const QString& message, const QString& category)
{
    log(LogLevel::Error, message, category);
}

void Logger::critical(const QString& message, const QString& category)
{
    log(LogLevel::Critical, message, category);
}

