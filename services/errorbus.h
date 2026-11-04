#ifndef ERRORBUS_H
#define ERRORBUS_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QList>
#include <QDateTime>

enum class ErrorLevel {
    Error,
    Warning,
    Info
};

struct ErrorEntry {
    QString id;
    QString message;
    ErrorLevel level;
    QJsonObject detail;
    qint64 timestamp;
    
    QJsonObject toJson() const;
    static ErrorEntry fromJson(const QJsonObject& json);
};

class ErrorBus : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<ErrorEntry> errors READ getErrors NOTIFY errorsChanged)
    
public:
    static ErrorBus* instance();
    
    // Log errors
    ErrorEntry logError(const QString& message, const QJsonObject& detail = QJsonObject());
    ErrorEntry logWarning(const QString& message, const QJsonObject& detail = QJsonObject());
    ErrorEntry logInfo(const QString& message, const QJsonObject& detail = QJsonObject());
    
    // Get error log
    QList<ErrorEntry> getErrors() const;
    QList<ErrorEntry> getErrors(ErrorLevel level) const;
    
    // Clear errors
    void clearErrors();
    void clearError(const QString& id);
    
    // Get errors as JSON (for QML)
    QJsonArray getErrorsJson() const;
    
signals:
    void errorLogged(const ErrorEntry& entry);
    void errorsChanged();
    void errorCleared(const QString& id);
    
private:
    ErrorBus(QObject* parent = nullptr);
    ~ErrorBus();
    ErrorBus(const ErrorBus&) = delete;
    ErrorBus& operator=(const ErrorBus&) = delete;
    
    void addError(ErrorLevel level, const QString& message, const QJsonObject& detail);
    void saveErrors();
    void loadErrors();
    
    QList<ErrorEntry> m_errors;
    static const int MAX_ENTRIES = 200;
    static ErrorBus* s_instance;
};

#endif // ERRORBUS_H

