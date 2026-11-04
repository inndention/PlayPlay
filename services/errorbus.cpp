#include "errorbus.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QRandomGenerator>

ErrorBus* ErrorBus::s_instance = nullptr;

ErrorBus::ErrorBus(QObject* parent)
    : QObject(parent)
{
    loadErrors();
}

ErrorBus::~ErrorBus()
{
    saveErrors();
}

ErrorBus* ErrorBus::instance()
{
    if (!s_instance) {
        s_instance = new ErrorBus();
    }
    return s_instance;
}

ErrorEntry ErrorBus::logError(const QString& message, const QJsonObject& detail)
{
    addError(ErrorLevel::Error, message, detail);
    return m_errors.last();
}

ErrorEntry ErrorBus::logWarning(const QString& message, const QJsonObject& detail)
{
    addError(ErrorLevel::Warning, message, detail);
    return m_errors.last();
}

ErrorEntry ErrorBus::logInfo(const QString& message, const QJsonObject& detail)
{
    addError(ErrorLevel::Info, message, detail);
    return m_errors.last();
}

QList<ErrorEntry> ErrorBus::getErrors() const
{
    return m_errors;
}

QList<ErrorEntry> ErrorBus::getErrors(ErrorLevel level) const
{
    QList<ErrorEntry> filtered;
    for (const ErrorEntry& entry : m_errors) {
        if (entry.level == level) {
            filtered.append(entry);
        }
    }
    return filtered;
}

void ErrorBus::clearErrors()
{
    m_errors.clear();
    saveErrors();
    emit errorsChanged();
    emit errorCleared(QString());
}

void ErrorBus::clearError(const QString& id)
{
    for (int i = 0; i < m_errors.size(); ++i) {
        if (m_errors[i].id == id) {
            m_errors.removeAt(i);
            saveErrors();
            emit errorsChanged();
            emit errorCleared(id);
            return;
        }
    }
}

QJsonArray ErrorBus::getErrorsJson() const
{
    QJsonArray array;
    for (const ErrorEntry& entry : m_errors) {
        array.append(entry.toJson());
    }
    return array;
}

void ErrorBus::addError(ErrorLevel level, const QString& message, const QJsonObject& detail)
{
    ErrorEntry entry;
    entry.id = QString("%1_%2")
        .arg(QDateTime::currentMSecsSinceEpoch(), 0, 36)
        .arg(QString::number(QRandomGenerator::global()->generate()).left(4));
    entry.message = message;
    entry.level = level;
    entry.detail = detail;
    entry.timestamp = QDateTime::currentMSecsSinceEpoch();
    
    m_errors.append(entry);
    
    // Keep only last MAX_ENTRIES
    if (m_errors.size() > MAX_ENTRIES) {
        m_errors.removeFirst();
    }
    
    saveErrors();
    emit errorLogged(entry);
    emit errorsChanged();
}

void ErrorBus::saveErrors()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + "/error_log.json";
    
    QJsonArray array;
    for (const ErrorEntry& entry : m_errors) {
        array.append(entry.toJson());
    }
    
    QJsonDocument doc(array);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << doc.toJson(QJsonDocument::Compact);
        file.close();
    }
}

void ErrorBus::loadErrors()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = dataPath + "/error_log.json";
    
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse error log:" << error.errorString();
        return;
    }
    
    QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        ErrorEntry entry = ErrorEntry::fromJson(value.toObject());
        m_errors.append(entry);
    }
    
    // Keep only last MAX_ENTRIES
    if (m_errors.size() > MAX_ENTRIES) {
        m_errors = m_errors.mid(m_errors.size() - MAX_ENTRIES);
    }
}

QJsonObject ErrorEntry::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["message"] = message;
    json["level"] = static_cast<int>(level);
    json["detail"] = detail;
    json["timestamp"] = timestamp;
    return json;
}

ErrorEntry ErrorEntry::fromJson(const QJsonObject& json)
{
    ErrorEntry entry;
    entry.id = json["id"].toString();
    entry.message = json["message"].toString();
    entry.level = static_cast<ErrorLevel>(json["level"].toInt());
    entry.detail = json["detail"].toObject();
    entry.timestamp = json["timestamp"].toVariant().toLongLong();
    return entry;
}

