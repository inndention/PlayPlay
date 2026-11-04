#include "settingsmanager.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

SettingsManager* SettingsManager::s_instance = nullptr;

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configPath);
    m_settings = new QSettings(configPath + "/settings.ini", QSettings::IniFormat, this);
    m_cachedSettings = loadAll();
}

SettingsManager::~SettingsManager()
{
}

SettingsManager* SettingsManager::instance()
{
    if (!s_instance) {
        s_instance = new SettingsManager();
    }
    return s_instance;
}

QJsonObject SettingsManager::loadAll()
{
    QJsonObject settings;
    
    // Load automation settings
    settings["automation"] = getAutomationSettings();
    
    // Load connection settings
    settings["connections"] = getConnections();
    
    // Load engine paths
    settings["enginePaths"] = getEnginePaths();
    
    // Load other settings
    settings["hostname"] = getHostname();
    settings["instanceMapping"] = getInstanceMapping();
    settings["devNotes"] = getDevNotes();
    
    m_cachedSettings = settings;
    return settings;
}

QJsonObject SettingsManager::getSettings()
{
    if (m_cachedSettings.isEmpty()) {
        return loadAll();
    }
    return m_cachedSettings;
}

QJsonObject SettingsManager::getConnections()
{
    QJsonObject defaultConnections;
    defaultConnections["obs"] = QJsonObject({
        {"host", "127.0.0.1"},
        {"port", 4455},
        {"password", ""}
    });
    defaultConnections["ffplayout"] = QJsonObject({
        {"apiUrl", "http://127.0.0.1:8787"},
        {"username", "admin"},
        {"password", "admin"}
    });
    defaultConnections["phpApi"] = QJsonObject({
        {"url", "http://yoursite.com/tracker.php"},
        {"apiKey", ""},
        {"enabled", false}
    });
    
    return loadJson("connections", defaultConnections);
}

void SettingsManager::setConnections(const QJsonObject& connections)
{
    saveJson("connections", connections);
    m_cachedSettings["connections"] = connections;
    emit connectionSettingsChanged();
    emit settingsChanged();
}

QJsonObject SettingsManager::getEnginePaths()
{
    return loadJson("enginePaths", QJsonObject());
}

void SettingsManager::setEnginePaths(const QJsonObject& paths)
{
    saveJson("enginePaths", paths);
    m_cachedSettings["enginePaths"] = paths;
    emit settingsChanged();
}

QJsonObject SettingsManager::getAutomationSettings()
{
    QJsonObject defaults;
    defaults["enabled"] = false;
    defaults["autoAdvance"] = true;
    defaults["autoFill"] = true;
    defaults["respectSchedule"] = true;
    
    return loadJson("automation", defaults);
}

void SettingsManager::setAutomationSettings(const QJsonObject& settings)
{
    saveJson("automation", settings);
    m_cachedSettings["automation"] = settings;
    emit automationSettingsChanged();
    emit settingsChanged();
}

QString SettingsManager::getHostname()
{
    return m_settings->value("hostname", "").toString();
}

void SettingsManager::setHostname(const QString& hostname)
{
    m_settings->setValue("hostname", hostname);
    m_settings->sync();
    m_cachedSettings["hostname"] = hostname;
    emit settingsChanged();
}

QJsonObject SettingsManager::getInstanceMapping()
{
    return loadJson("instanceMapping", QJsonObject());
}

void SettingsManager::setInstanceMapping(const QJsonObject& mapping)
{
    saveJson("instanceMapping", mapping);
    m_cachedSettings["instanceMapping"] = mapping;
    emit settingsChanged();
}

QString SettingsManager::getDevNotes()
{
    return m_settings->value("devNotes", "").toString();
}

void SettingsManager::setDevNotes(const QString& notes)
{
    m_settings->setValue("devNotes", notes);
    m_settings->sync();
    m_cachedSettings["devNotes"] = notes;
    emit settingsChanged();
}

QString SettingsManager::getOBSHost()
{
    QJsonObject connections = getConnections();
    QJsonObject obs = connections["obs"].toObject();
    return obs["host"].toString("127.0.0.1");
}

int SettingsManager::getOBSPort()
{
    QJsonObject connections = getConnections();
    QJsonObject obs = connections["obs"].toObject();
    return obs["port"].toInt(4455);
}

QString SettingsManager::getOBSPassword()
{
    QJsonObject connections = getConnections();
    QJsonObject obs = connections["obs"].toObject();
    return obs["password"].toString("");
}

QString SettingsManager::getFFPlayoutApiUrl()
{
    QJsonObject connections = getConnections();
    QJsonObject ffplayout = connections["ffplayout"].toObject();
    return ffplayout["apiUrl"].toString("http://127.0.0.1:8787");
}

QString SettingsManager::getFFPlayoutUsername()
{
    QJsonObject connections = getConnections();
    QJsonObject ffplayout = connections["ffplayout"].toObject();
    return ffplayout["username"].toString("admin");
}

QString SettingsManager::getFFPlayoutPassword()
{
    QJsonObject connections = getConnections();
    QJsonObject ffplayout = connections["ffplayout"].toObject();
    return ffplayout["password"].toString("admin");
}

QJsonObject SettingsManager::getSettingsJson() const
{
    return m_cachedSettings;
}

void SettingsManager::saveSettings()
{
    m_settings->sync();
}

QJsonObject SettingsManager::loadJson(const QString& key, const QJsonObject& defaultValue)
{
    QString jsonStr = m_settings->value(key, "").toString();
    if (jsonStr.isEmpty()) {
        return defaultValue;
    }
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse JSON for key" << key << ":" << error.errorString();
        return defaultValue;
    }
    
    return doc.object();
}

void SettingsManager::saveJson(const QString& key, const QJsonObject& value)
{
    QJsonDocument doc(value);
    m_settings->setValue(key, doc.toJson(QJsonDocument::Compact));
    m_settings->sync();
}

