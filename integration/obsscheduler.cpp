#include "obsscheduler.h"
#include "services/persistentlogger.h"
#include "services/settingsmanager.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QRandomGenerator>
#include <QDateTime>

const QString OBSScheduler::STORAGE_KEY = "scheduled_obs_commands";

OBSScheduler::OBSScheduler(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
    , m_client(nullptr)
{
    m_timer->setInterval(1000); // Check every second
    connect(m_timer, &QTimer::timeout, this, &OBSScheduler::checkSchedule);
    loadSchedule();
}

OBSScheduler::~OBSScheduler()
{
    stop();
}

void OBSScheduler::scheduleCommand(const QString& commandId, qint64 runAt, const QString& label)
{
    ScheduledOBSCommand cmd;
    cmd.id = QString("%1_%2")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(QString::number(QRandomGenerator::global()->generate()).left(4));
    cmd.commandId = commandId;
    cmd.runAt = runAt;
    cmd.label = label.isEmpty() ? commandId : label;
    
    m_scheduledCommands.append(cmd);
    saveSchedule();
    
    PersistentLogger::instance()->info(
        QString("Scheduled OBS command: %1").arg(cmd.label),
        QJsonObject({
            {"commandId", commandId},
            {"runAt", runAt},
            {"label", cmd.label}
        }),
        "OBS"
    );
    
    emit commandScheduled(cmd);
}

void OBSScheduler::start(ObsWebSocketClient* client)
{
    if (m_timer->isActive()) {
        return;
    }
    
    m_client = client;
    m_timer->start();
}

void OBSScheduler::stop()
{
    m_timer->stop();
    m_client = nullptr;
}

QList<ScheduledOBSCommand> OBSScheduler::getScheduledCommands() const
{
    return m_scheduledCommands;
}

void OBSScheduler::removeCommand(const QString& id)
{
    for (int i = 0; i < m_scheduledCommands.size(); ++i) {
        if (m_scheduledCommands[i].id == id) {
            m_scheduledCommands.removeAt(i);
            saveSchedule();
            return;
        }
    }
}

void OBSScheduler::clear()
{
    m_scheduledCommands.clear();
    saveSchedule();
}

void OBSScheduler::checkSchedule()
{
    if (!m_client || !m_client->isConnected()) {
        return;
    }
    
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<ScheduledOBSCommand> due;
    QList<ScheduledOBSCommand> remaining;
    
    for (const ScheduledOBSCommand& cmd : m_scheduledCommands) {
        if (cmd.runAt <= now) {
            due.append(cmd);
        } else {
            remaining.append(cmd);
        }
    }
    
    if (due.isEmpty()) {
        return;
    }
    
    QJsonArray commands = loadOBSCommands();
    
    for (const ScheduledOBSCommand& sched : due) {
        // Find command in loaded commands
        QJsonObject command;
        for (const QJsonValue& val : commands) {
            QJsonObject cmd = val.toObject();
            if (cmd["id"].toString() == sched.commandId) {
                command = cmd;
                break;
            }
        }
        
        if (command.isEmpty()) {
            PersistentLogger::instance()->warn(
                QString("Scheduled OBS command not found: %1").arg(sched.commandId),
                QJsonObject({{"commandId", sched.commandId}}),
                "OBS"
            );
            continue;
        }
        
        // Execute command (simplified - would need full OBS command executor)
        // For now, just emit signal
        emit commandExecuted(sched.commandId);
        
        PersistentLogger::instance()->info(
            QString("Executed scheduled OBS command: %1").arg(sched.commandId),
            QJsonObject({{"commandId", sched.commandId}}),
            "OBS"
        );
    }
    
    m_scheduledCommands = remaining;
    saveSchedule();
}

void OBSScheduler::loadSchedule()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + "/" + STORAGE_KEY + ".json";
    
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return;
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        qWarning() << "Failed to parse OBS schedule:" << error.errorString();
        return;
    }
    
    QJsonArray array = doc.array();
    for (const QJsonValue& value : array) {
        m_scheduledCommands.append(ScheduledOBSCommand::fromJson(value.toObject()));
    }
}

void OBSScheduler::saveSchedule()
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataPath);
    QString filePath = dataPath + "/" + STORAGE_KEY + ".json";
    
    QJsonArray array;
    for (const ScheduledOBSCommand& cmd : m_scheduledCommands) {
        array.append(cmd.toJson());
    }
    
    QJsonDocument doc(array);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file);
        stream << doc.toJson(QJsonDocument::Compact);
        file.close();
    }
}

QJsonArray OBSScheduler::loadOBSCommands()
{
    // Load from settings or storage
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString filePath = dataPath + "/obs_commands.json";
    
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return QJsonArray();
    }
    
    QByteArray data = file.readAll();
    file.close();
    
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        return QJsonArray();
    }
    
    return doc.array();
}

QJsonObject ScheduledOBSCommand::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["commandId"] = commandId;
    json["runAt"] = runAt;
    json["label"] = label;
    return json;
}

ScheduledOBSCommand ScheduledOBSCommand::fromJson(const QJsonObject& json)
{
    ScheduledOBSCommand cmd;
    cmd.id = json["id"].toString();
    cmd.commandId = json["commandId"].toString();
    cmd.runAt = json["runAt"].toVariant().toLongLong();
    cmd.label = json["label"].toString();
    return cmd;
}

