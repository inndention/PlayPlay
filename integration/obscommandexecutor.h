#ifndef OBSCOMMANDEXECUTOR_H
#define OBSCOMMANDEXECUTOR_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QFuture>
#include <QtConcurrent>
#include "obswebsocket.h"

struct OBSCommand {
    QString id;
    QString label;
    QString commandType; // "change_scene", "show_source", "hide_source", etc.
    QString sceneName;
    QString sourceName;
    QString sceneCollection;
    bool visible;
    QString requestType;
    QString requestData;
    QString recordDirectory;
    QString recordFileName;
    
    QJsonObject toJson() const;
    static OBSCommand fromJson(const QJsonObject& json);
};

class OBSCommandExecutor : public QObject
{
    Q_OBJECT
    
public:
    explicit OBSCommandExecutor(QObject* parent = nullptr);
    
    // Execute an OBS command
    QFuture<bool> executeCommand(const OBSCommand& command, ObsWebSocketClient* client);
    
    // Execute command with context (for variable substitution)
    QFuture<bool> executeCommandWithContext(const OBSCommand& command, 
                                             ObsWebSocketClient* client,
                                             const QJsonObject& context = QJsonObject());
    
signals:
    void commandExecuted(const QString& commandId);
    void commandFailed(const QString& commandId, const QString& error);
    
private:
    bool executeCommandSync(const OBSCommand& command, ObsWebSocketClient* client, const QJsonObject& context);
    QString substituteVariables(const QString& str, const QJsonObject& context);
};

#endif // OBSCOMMANDEXECUTOR_H

