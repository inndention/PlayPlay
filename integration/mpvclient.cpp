#include "mpvclient.h"
#include "services/logger.h"
#include <QProcess>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

MpvClient::MpvClient(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_isConnected(false)
{
    connect(m_process, &QProcess::readyReadStandardOutput, this, &MpvClient::onStdout);
    connect(m_process, &QProcess::readyReadStandardError, this, &MpvClient::onStderr);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MpvClient::onProcessFinished);
}

bool MpvClient::connect(const QString& ipcPath)
{
    m_ipcSocketPath = ipcPath;
    m_isConnected = true;
    return true;
}

bool MpvClient::initialize()
{
    // Find MPV executable
    QString mpvPath = findMpvExecutable();
    if (mpvPath.isEmpty()) {
        Logger::instance()->error("MPV executable not found");
        return false;
    }
    
    // Start MPV with IPC
    QStringList arguments;
    arguments << "--idle"
              << "--input-ipc-server=" + m_ipcSocketPath
              << "--no-video"
              << "--no-audio"; // We'll control playback programmatically
    
    m_process->start(mpvPath, arguments);
    
    if (!m_process->waitForStarted(3000)) {
        Logger::instance()->error("Failed to start MPV process");
        return false;
    }
    
    m_isConnected = true;
    Logger::instance()->info("MPV client initialized");
    return true;
}

void MpvClient::shutdown()
{
    if (m_process->state() != QProcess::NotRunning) {
        sendCommand("quit");
        m_process->waitForFinished(3000);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
        }
    }
    m_isConnected = false;
}

bool MpvClient::loadFile(const QString& filePath)
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject command;
    command["command"] = QJsonArray{"loadfile", filePath};
    command["request_id"] = ++m_requestId;
    
    return sendCommand(QJsonDocument(command).toJson());
}

bool MpvClient::play()
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject command;
    command["command"] = QJsonArray{"set_property", "pause", false};
    command["request_id"] = ++m_requestId;
    
    return sendCommand(QJsonDocument(command).toJson());
}

bool MpvClient::pause()
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject command;
    command["command"] = QJsonArray{"set_property", "pause", true};
    command["request_id"] = ++m_requestId;
    
    return sendCommand(QJsonDocument(command).toJson());
}

bool MpvClient::stop()
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject command;
    command["command"] = QJsonArray{"stop"};
    command["request_id"] = ++m_requestId;
    
    return sendCommand(QJsonDocument(command).toJson());
}

bool MpvClient::seek(double timeSeconds)
{
    if (!m_isConnected) {
        return false;
    }
    
    QJsonObject command;
    command["command"] = QJsonArray{"seek", timeSeconds, "absolute"};
    command["request_id"] = ++m_requestId;
    
    return sendCommand(QJsonDocument(command).toJson());
}

QString MpvClient::findMpvExecutable() const
{
    // Try common paths
    QStringList paths;
    
#ifdef Q_OS_WIN
    paths << "mpv.exe"
          << QStandardPaths::findExecutable("mpv")
          << "C:/Program Files/mpv/mpv.exe"
          << "C:/mpv/mpv.exe";
#else
    paths << "mpv"
          << QStandardPaths::findExecutable("mpv")
          << "/usr/bin/mpv"
          << "/usr/local/bin/mpv";
#endif
    
    for (const QString& path : paths) {
        if (!path.isEmpty() && QFileInfo::exists(path)) {
            return path;
        }
    }
    
    return QString();
}

bool MpvClient::sendCommand(const QByteArray& command)
{
    if (!m_isConnected || m_process->state() != QProcess::Running) {
        return false;
    }
    
    // TODO: Send command via IPC socket
    // For now, this is a stub
    Q_UNUSED(command)
    return true;
}

void MpvClient::onStdout()
{
    QByteArray data = m_process->readAllStandardOutput();
    // TODO: Parse MPV JSON responses
    Q_UNUSED(data)
}

void MpvClient::onStderr()
{
    QByteArray data = m_process->readAllStandardError();
    Logger::instance()->warning(QString("MPV stderr: %1").arg(QString::fromUtf8(data)));
}

void MpvClient::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_isConnected = false;
    Logger::instance()->info(QString("MPV process finished: exit code %1").arg(exitCode));
    Q_UNUSED(exitStatus)
    emit disconnected();
}

