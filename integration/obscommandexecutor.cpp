#include "obscommandexecutor.h"
#include <QtConcurrent>
#include <QDebug>
#include <QRegularExpression>

OBSCommandExecutor::OBSCommandExecutor(QObject* parent)
    : QObject(parent)
{
}

QFuture<bool> OBSCommandExecutor::executeCommand(const OBSCommand& command, ObsWebSocketClient* client)
{
    return QtConcurrent::run([this, command, client]() {
        return executeCommandSync(command, client, QJsonObject());
    });
}

QFuture<bool> OBSCommandExecutor::executeCommandWithContext(const OBSCommand& command, 
                                                              ObsWebSocketClient* client,
                                                              const QJsonObject& context)
{
    return QtConcurrent::run([this, command, client, context]() {
        return executeCommandSync(command, client, context);
    });
}

bool OBSCommandExecutor::executeCommandSync(const OBSCommand& command, ObsWebSocketClient* client, const QJsonObject& context)
{
    if (!client || !client->isConnected()) {
        qWarning() << "[OBSCommand] Client not connected, skipping command:" << command.label;
        emit commandFailed(command.id, "Client not connected");
        return false;
    }
    
    qDebug() << "[OBSCommand] Executing:" << command.label << "(" << command.commandType << ")";
    
    try {
        if (command.commandType == "change_scene") {
            QString sceneName = substituteVariables(command.sceneName, context);
            if (!sceneName.isEmpty()) {
                bool result = client->setCurrentScene(sceneName);
                if (result) {
                    emit commandExecuted(command.id);
                    return true;
                }
            }
        } else if (command.commandType == "show_source" || 
                   command.commandType == "hide_source" || 
                   command.commandType == "set_source_visibility") {
            QString sceneName = substituteVariables(command.sceneName, context);
            QString sourceName = substituteVariables(command.sourceName, context);
            
            if (!sceneName.isEmpty() && !sourceName.isEmpty()) {
                bool visible = (command.commandType == "show_source") ? true :
                              (command.commandType == "hide_source") ? false :
                              command.visible;
                
                // Note: This would need setSceneItemEnabled method in ObsWebSocketClient
                // For now, use setSourceVisibility as fallback
                bool result = client->setSourceVisibility(sourceName, visible);
                if (result) {
                    emit commandExecuted(command.id);
                    return true;
                }
            }
        } else if (command.commandType == "start_record") {
            // Note: Would need startRecord methods in ObsWebSocketClient
            qWarning() << "[OBSCommand] start_record not yet implemented";
        } else if (command.commandType == "stop_record") {
            // Note: Would need stopRecord methods in ObsWebSocketClient
            qWarning() << "[OBSCommand] stop_record not yet implemented";
        } else if (command.commandType == "start_stream") {
            // Note: Would need startStream methods in ObsWebSocketClient
            qWarning() << "[OBSCommand] start_stream not yet implemented";
        } else if (command.commandType == "stop_stream") {
            // Note: Would need stopStream methods in ObsWebSocketClient
            qWarning() << "[OBSCommand] stop_stream not yet implemented";
        } else if (command.commandType == "custom") {
            if (!command.requestType.isEmpty()) {
                // Note: Would need call method in ObsWebSocketClient
                qWarning() << "[OBSCommand] custom command not yet fully implemented";
            }
        }
        
        emit commandFailed(command.id, "Command type not implemented");
        return false;
    } catch (const std::exception& e) {
        QString error = QString("Failed to execute %1: %2").arg(command.label, e.what());
        qWarning() << "[OBSCommand]" << error;
        emit commandFailed(command.id, error);
        return false;
    }
}

QString OBSCommandExecutor::substituteVariables(const QString& str, const QJsonObject& context)
{
    QString result = str;
    
    // Replace ${variable} with context values
    QRegularExpression re(R"(\$\{(\w+)\})");
    QRegularExpressionMatchIterator it = re.globalMatch(result);
    
    QStringList replacements;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        QString varName = match.captured(1);
        QString value = context[varName].toString();
        if (!value.isEmpty()) {
            result.replace(match.captured(0), value);
        }
    }
    
    // Replace common variables
    result.replace("${date}", QDateTime::currentDateTime().toString("yyyy-MM-dd"));
    result.replace("${time}", QDateTime::currentDateTime().toString("hh:mm:ss"));
    result.replace("${datetime}", QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"));
    
    return result;
}

QJsonObject OBSCommand::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["label"] = label;
    json["commandType"] = commandType;
    json["sceneName"] = sceneName;
    json["sourceName"] = sourceName;
    json["sceneCollection"] = sceneCollection;
    json["visible"] = visible;
    json["requestType"] = requestType;
    json["requestData"] = requestData;
    json["recordDirectory"] = recordDirectory;
    json["recordFileName"] = recordFileName;
    return json;
}

OBSCommand OBSCommand::fromJson(const QJsonObject& json)
{
    OBSCommand cmd;
    cmd.id = json["id"].toString();
    cmd.label = json["label"].toString();
    cmd.commandType = json["commandType"].toString();
    cmd.sceneName = json["sceneName"].toString();
    cmd.sourceName = json["sourceName"].toString();
    cmd.sceneCollection = json["sceneCollection"].toString();
    cmd.visible = json["visible"].toBool();
    cmd.requestType = json["requestType"].toString();
    cmd.requestData = json["requestData"].toString();
    cmd.recordDirectory = json["recordDirectory"].toString();
    cmd.recordFileName = json["recordFileName"].toString();
    return cmd;
}

