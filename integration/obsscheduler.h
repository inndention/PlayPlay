#ifndef OBSSCHEDULER_H
#define OBSSCHEDULER_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QDateTime>
#include "obswebsocket.h"

struct ScheduledOBSCommand {
    QString id;
    QString commandId;
    qint64 runAt;
    QString label;
    
    QJsonObject toJson() const;
    static ScheduledOBSCommand fromJson(const QJsonObject& json);
};

class OBSScheduler : public QObject
{
    Q_OBJECT
    
public:
    explicit OBSScheduler(QObject* parent = nullptr);
    ~OBSScheduler();
    
    // Schedule a command
    void scheduleCommand(const QString& commandId, qint64 runAt, const QString& label = QString());
    
    // Start/stop scheduler
    void start(ObsWebSocketClient* client);
    void stop();
    
    // Get scheduled commands
    QList<ScheduledOBSCommand> getScheduledCommands() const;
    
    // Remove a scheduled command
    void removeCommand(const QString& id);
    
    // Clear all scheduled commands
    void clear();
    
signals:
    void commandScheduled(const ScheduledOBSCommand& command);
    void commandExecuted(const QString& commandId);
    void commandFailed(const QString& commandId, const QString& error);
    
private slots:
    void checkSchedule();
    
private:
    void loadSchedule();
    void saveSchedule();
    QJsonArray loadOBSCommands();
    
    QTimer* m_timer;
    ObsWebSocketClient* m_client;
    QList<ScheduledOBSCommand> m_scheduledCommands;
    static const QString STORAGE_KEY;
};

#endif // OBSSCHEDULER_H

