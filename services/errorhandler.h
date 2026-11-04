#ifndef ERRORHANDLER_H
#define ERRORHANDLER_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QList>
#include <QDateTime>
#include <QJsonObject>

struct ErrorRecord {
    QString id;
    QString instanceId;
    qint64 timestamp;
    QString severity; // "warning", "error", "critical"
    QString message;
    QString source;
    bool acknowledged;
    QJsonObject details;
    
    QJsonObject toJson() const;
    static ErrorRecord fromJson(const QJsonObject& json);
};

class ErrorHandler : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<ErrorRecord> errors READ getAllErrors NOTIFY errorsChanged)
    
public:
    static ErrorHandler* instance();
    
    // Log error
    QString logError(const QString& instanceId, const QString& severity, 
                     const QString& message, const QString& source, 
                     const QJsonObject& details = QJsonObject());
    
    // Get errors
    QList<ErrorRecord> getAllErrors() const;
    QList<ErrorRecord> getErrorsByInstance(const QString& instanceId) const;
    QList<ErrorRecord> getUnacknowledgedErrors() const;
    
    // Acknowledge error
    void acknowledgeError(const QString& errorId);
    
    // Suppress error (don't log same error again)
    void suppressError(const QString& source, const QString& message);
    
    // Clear suppressed errors
    void clearSuppressedErrors();
    
    // Buzzer control
    void setBuzzerEnabled(bool enabled);
    bool isBuzzerEnabled() const;
    
    // Get errors as JSON (for QML)
    QJsonArray getErrorsJson() const;
    
signals:
    void errorLogged(const ErrorRecord& error);
    void errorAcknowledged(const QString& errorId);
    void errorsChanged();
    void buzzerPlayed();
    
private:
    ErrorHandler(QObject* parent = nullptr);
    ~ErrorHandler();
    ErrorHandler(const ErrorHandler&) = delete;
    ErrorHandler& operator=(const ErrorHandler&) = delete;
    
    void playBuzzer();
    void notifyListeners();
    void saveErrors();
    void loadErrors();
    
    QMap<QString, ErrorRecord> m_errors;
    QMap<QString, QList<ErrorRecord>> m_instanceErrors;
    QMap<QString, QPair<int, qint64>> m_errorThrottle; // errorKey -> {count, lastTime}
    QList<QString> m_suppressedErrors; // errorKey list
    bool m_buzzerEnabled;
    static ErrorHandler* s_instance;
};

#endif // ERRORHANDLER_H

