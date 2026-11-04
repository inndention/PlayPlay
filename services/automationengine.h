#ifndef AUTOMATIONENGINE_H
#define AUTOMATIONENGINE_H

#include <QObject>
#include <QTimer>
#include "core/types.h"

class AutomationEngine : public QObject
{
    Q_OBJECT

public:
    explicit AutomationEngine(QObject* parent = nullptr);
    
    void start();
    void stop();
    bool isRunning() const { return m_isRunning; }
    
    void setSettings(const AutomationSettings& settings);
    AutomationSettings getSettings() const;

signals:
    void started();
    void stopped();
    void settingsChanged(const AutomationSettings& settings);
    void eventTriggered(const QString& eventId);

private slots:
    void onTick();

private:
    QTimer* m_timer;
    bool m_isRunning;
    AutomationSettings m_settings;
};

#endif // AUTOMATIONENGINE_H

