#include "copilotservice.h"
#include "services/logger.h"
#include <QJsonDocument>
#include <QDebug>
#include <QSettings>

static CopilotService* g_copilotService = nullptr;

CopilotService::CopilotService(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_enabled(true)
{
    // OpenRouter key: the OPENROUTER_API_KEY environment variable; a key saved under openrouter/api_key overrides it
    QSettings settings;
    m_apiKey = settings.value("openrouter/api_key").toString();
    if (m_apiKey.isEmpty()) {
        m_apiKey = qEnvironmentVariable("OPENROUTER_API_KEY");
    }
}

void CopilotService::setEnabled(bool enabled)
{
    if (m_enabled != enabled) {
        m_enabled = enabled;
        emit enabledChanged(enabled);
    }
}

void CopilotService::ask(const QString& prompt)
{
    if (!m_enabled || m_apiKey.isEmpty()) {
        emit errorOccurred("Copilot disabled or API key missing");
        return;
    }
    callOpenAI(prompt);
}

void CopilotService::handlePlayoutEvent(const QString& type, const QJsonObject& data)
{
    if (!m_enabled) return;
    
    QString eventDescription;
    if (type == "media_change") {
        eventDescription = QString("The current media just changed to: %1").arg(data["title"].toString());
    } else if (type == "error") {
        eventDescription = QString("An error occurred: %1").arg(data["message"].toString());
    } else {
        return; // Don't respond to every tiny thing
    }
    
    // Auto-ask but with lower priority
    ask(eventDescription);
}

void CopilotService::callOpenAI(const QString& prompt)
{
    QNetworkRequest request(QUrl("https://openrouter.ai/api/v1/chat/completions"));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());

    QJsonObject body;
    body["model"] = QSettings().value("openrouter/model", QStringLiteral("anthropic/claude-sonnet-5.5")).toString();   // same default as aimodels (suitelogic::defaultAiModel)
    
    QJsonArray messages;
    
    // System message
    QJsonObject system;
    system["role"] = "system";
    system["content"] = buildSystemPrompt();
    messages.append(system);
    
    // Add history (last 5 messages)
    int start = qMax(0, m_chatHistory.size() - 5);
    for (int i = start; i < m_chatHistory.size(); ++i) {
        QJsonObject msg;
        msg["role"] = m_chatHistory[i].role;
        msg["content"] = m_chatHistory[i].content;
        messages.append(msg);
    }
    
    // User message
    QJsonObject user;
    user["role"] = "user";
    user["content"] = prompt;
    messages.append(user);
    
    body["messages"] = messages;
    body["max_tokens"] = 150;
    body["temperature"] = 0.8;

    QNetworkReply* reply = m_networkManager->post(request, QJsonDocument(body).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onReplyFinished(reply);
    });
}

void CopilotService::onReplyFinished(QNetworkReply* reply)
{
    reply->deleteLater();
    
    if (reply->error() != QNetworkReply::NoError) {
        emit errorOccurred(reply->errorString());
        return;
    }
    
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
    QJsonObject obj = doc.object();
    QJsonArray choices = obj["choices"].toArray();
    
    if (!choices.isEmpty()) {
        QString content = choices[0].toObject()["message"].toObject()["content"].toString().trimmed();
        m_lastResponse = content;
        
        // Add to history
        CopilotMessage msg;
        msg.role = "assistant";
        msg.content = content;
        msg.timestamp = QDateTime::currentDateTime();
        m_chatHistory.append(msg);
        
        emit responseReceived(content);
        Logger::instance()->info("AI Copilot: " + content);
    }
}

QString CopilotService::buildSystemPrompt() const
{
    return "You are 'Control', the AI control room assistant for a broadcast station. "
           "You are a bit clumsy, witty, and sometimes sarcastic. "
           "You help the user manage their playout. Keep responses short (max 2 sentences).";
}

CopilotService* getCopilotService()
{
    if (!g_copilotService) {
        g_copilotService = new CopilotService();
    }
    return g_copilotService;
}
