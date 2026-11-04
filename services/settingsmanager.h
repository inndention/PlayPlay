#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <QObject>
#include <QSettings>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <QStringList>

class SettingsManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QJsonObject settings READ getSettingsJson NOTIFY settingsChanged)
    
public:
    static SettingsManager* instance();
    
    // Load all settings
    QJsonObject loadAll();
    QJsonObject getSettings();
    
    // Connection settings
    QJsonObject getConnections();
    void setConnections(const QJsonObject& connections);
    
    // Engine paths
    QJsonObject getEnginePaths();
    void setEnginePaths(const QJsonObject& paths);
    
    // Automation settings
    QJsonObject getAutomationSettings();
    void setAutomationSettings(const QJsonObject& settings);
    
    // Other settings
    QString getHostname();
    void setHostname(const QString& hostname);
    
    QJsonObject getInstanceMapping();
    void setInstanceMapping(const QJsonObject& mapping);
    
    QString getDevNotes();
    void setDevNotes(const QString& notes);
    
    // Convenience getters
    QString getOBSHost();
    int getOBSPort();
    QString getOBSPassword();
    
    
    QString getFFPlayoutApiUrl();
    QString getFFPlayoutUsername();
    QString getFFPlayoutPassword();
    
    // Get settings as JSON (for QML)
    QJsonObject getSettingsJson() const;
    
signals:
    void settingsChanged();
    void connectionSettingsChanged();
    void automationSettingsChanged();
    
private:
    SettingsManager(QObject* parent = nullptr);
    ~SettingsManager();
    SettingsManager(const SettingsManager&) = delete;
    SettingsManager& operator=(const SettingsManager&) = delete;
    
    QSettings* m_settings;
    QJsonObject m_cachedSettings;
    static SettingsManager* s_instance;
    
    void saveSettings();
    QJsonObject loadJson(const QString& key, const QJsonObject& defaultValue = QJsonObject());
    void saveJson(const QString& key, const QJsonObject& value);
};

#endif // SETTINGSMANAGER_H

