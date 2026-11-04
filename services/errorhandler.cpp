#include "errorhandler.h"
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDebug>
#include <QTimer>
#include <QRandomGenerator>

ErrorHandler* ErrorHandler::s_instance = nullptr;

ErrorHandler::ErrorHandler(QObject* parent)
    : QObject(parent)
    , m_buzzerEnabled(true)
{
    loadErrors();
}

ErrorHandler::~ErrorHandler()
{
    saveErrors();
}

ErrorHandler* ErrorHandler::instance()
{
    if (!s_instance) {
        s_instance = new ErrorHandler();
    }
    return s_instance;
}

QString ErrorHandler::logError(const QString& instanceId, const QString& severity,
                                const QString& message, const QString& source,
                                const QJsonObject& details)
{
    // Throttle repeated errors
    QString errorKey = QString("%1:%2").arg(source, message);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    
    if (m_errorThrottle.contains(errorKey)) {
        QPair<int, qint64> throttle = m_errorThrottle[errorKey];
        // If same error within 5 seconds, increment count but don't create new entry
        if (now - throttle.second < 5000) {
            throttle.first++;
            throttle.second = now;
            m_errorThrottle[errorKey] = throttle;
            
            // Update existing error with count
            for (auto it = m_errors.begin(); it != m_errors.end(); ++it) {
                if (it.value().source == source && it.value().message == message && !it.value().acknowledged) {
                    QJsonObject newDetails = it.value().details;
                    newDetails["repeatCount"] = throttle.first;
                    it.value().details = newDetails;
                    notifyListeners();
                    return it.key();
                }
            }
        }
    } else {
        m_errorThrottle[errorKey] = QPair<int, qint64>(1, now);
    }
    
    // Check if error is suppressed
    if (m_suppressedErrors.contains(errorKey)) {
        return QString(); // Don't log suppressed errors
    }
    
    QString errorId = QString("err_%1_%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QString::number(QRandomGenerator::global()->generate()).left(9));
    
    ErrorRecord error;
    error.id = errorId;
    error.instanceId = instanceId;
    error.timestamp = now;
    error.severity = severity;
    error.message = message;
    error.source = source;
    error.acknowledged = false;
    error.details = details;
    if (m_errorThrottle.contains(errorKey)) {
        error.details["repeatCount"] = m_errorThrottle[errorKey].first;
    }
    
    m_errors[errorId] = error;
    
    // Group by instance
    if (!m_instanceErrors.contains(instanceId)) {
        m_instanceErrors[instanceId] = QList<ErrorRecord>();
    }
    m_instanceErrors[instanceId].append(error);
    
    // Play buzzer for critical errors (if enabled)
    if (severity == "critical" && m_buzzerEnabled) {
        playBuzzer();
    }
    
    // Auto-dismiss low-severity errors after timeout
    if (severity == "warning") {
        QTimer::singleShot(30000, [this, errorId]() {
            acknowledgeError(errorId);
        });
    }
    
    notifyListeners();
    saveErrors();
    emit errorLogged(error);
    return errorId;
}

QList<ErrorRecord> ErrorHandler::getAllErrors() const
{
    return m_errors.values();
}

QList<ErrorRecord> ErrorHandler::getErrorsByInstance(const QString& instanceId) const
{
    return m_instanceErrors.value(instanceId);
}

QList<ErrorRecord> ErrorHandler::getUnacknowledgedErrors() const
{
    QList<ErrorRecord> unacknowledged;
    for (const ErrorRecord& error : m_errors.values()) {
        if (!error.acknowledged) {
            unacknowledged.append(error);
        }
    }
    return unacknowledged;
}

void ErrorHandler::acknowledgeError(const QString& errorId)
{
    if (m_errors.contains(errorId)) {
        m_errors[errorId].acknowledged = true;
        notifyListeners();
        saveErrors();
        emit errorAcknowledged(errorId);
    }
}

void ErrorHandler::suppressError(const QString& source, const QString& message)
{
    QString errorKey = QString("%1:%2").arg(source, message);
    if (!m_suppressedErrors.contains(errorKey)) {
        m_suppressedErrors.append(errorKey);
    }
}

void ErrorHandler::clearSuppressedErrors()
{
    m_suppressedErrors.clear();
}

void ErrorHandler::setBuzzerEnabled(bool enabled)
{
    m_buzzerEnabled = enabled;
}

bool ErrorHandler::isBuzzerEnabled() const
{
    return m_buzzerEnabled;
}

QJsonArray ErrorHandler::getErrorsJson() const
{
    QJsonArray array;
    for (const ErrorRecord& error : m_errors.values()) {
        array.append(error.toJson());
    }
    return array;
}

void ErrorHandler::playBuzzer()
{
    // Use QMediaPlayer to play a beep sound
    // Note: This is a simple implementation. For actual beep, you might want to use QSound or system beep
    emit buzzerPlayed();
    // In a real implementation, you would play an actual sound file or use system beep
}

void ErrorHandler::notifyListeners()
{
    emit errorsChanged();
}

void ErrorHandler::saveErrors()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + "/error_handler.json";
    
    QJsonArray array;
    for (const ErrorRecord& error : m_errors.values()) {
        array.append(error.toJson());
    }
    
    QJsonDocument doc(array);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << doc.toJson(QJsonDocument::Compact);
        file.close();
    }
}

void ErrorHandler::loadErrors()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = dataPath + "/error_handler.json";
    
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse error handler log:" << error.errorString();
        return;
    }
    
    QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        ErrorRecord entry = ErrorRecord::fromJson(value.toObject());
        m_errors[entry.id] = entry;
        
        // Group by instance
        if (!m_instanceErrors.contains(entry.instanceId)) {
            m_instanceErrors[entry.instanceId] = QList<ErrorRecord>();
        }
        m_instanceErrors[entry.instanceId].append(entry);
    }
}

QJsonObject ErrorRecord::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["instanceId"] = instanceId;
    json["timestamp"] = timestamp;
    json["severity"] = severity;
    json["message"] = message;
    json["source"] = source;
    json["acknowledged"] = acknowledged;
    json["details"] = details;
    return json;
}

ErrorRecord ErrorRecord::fromJson(const QJsonObject& json)
{
    ErrorRecord entry;
    entry.id = json["id"].toString();
    entry.instanceId = json["instanceId"].toString();
    entry.timestamp = json["timestamp"].toVariant().toLongLong();
    entry.severity = json["severity"].toString();
    entry.message = json["message"].toString();
    entry.source = json["source"].toString();
    entry.acknowledged = json["acknowledged"].toBool();
    entry.details = json["details"].toObject();
    return entry;
}

