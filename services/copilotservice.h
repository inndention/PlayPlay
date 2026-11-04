#ifndef COPILOTSERVICE_H
#define COPILOTSERVICE_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QDateTime>

struct CopilotMessage {
    QString role; // "system", "user", "assistant"
    QString content;
    QDateTime timestamp;
};

class CopilotService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString lastResponse READ lastResponse NOTIFY responseReceived)

public:
    explicit CopilotService(QObject* parent = nullptr);
    
    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled);
    
    QString lastResponse() const { return m_lastResponse; }

    Q_INVOKABLE void ask(const QString& prompt);
    Q_INVOKABLE void handlePlayoutEvent(const QString& type, const QJsonObject& data);

signals:
    void enabledChanged(bool enabled);
    void responseReceived(const QString& response);
    void errorOccurred(const QString& error);

private slots:
    void onReplyFinished(QNetworkReply* reply);

private:
    void callOpenAI(const QString& prompt);
    QString buildSystemPrompt() const;
    
    QNetworkAccessManager* m_networkManager;
    bool m_enabled;
    QString m_apiKey;
    QString m_lastResponse;
    QList<CopilotMessage> m_chatHistory;
};

CopilotService* getCopilotService();

#endif // COPILOTSERVICE_H
