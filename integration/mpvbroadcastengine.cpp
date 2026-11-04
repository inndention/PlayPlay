#include "mpvbroadcastengine.h"
#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>

MPVBroadcastEngine::MPVBroadcastEngine(QObject* parent)
    : QObject(parent)
    , m_process(new QProcess(this))
    , m_client(nullptr)
    , m_updateTimer(new QTimer(this))
    , m_isRunning(false)
{
    m_updateTimer->setInterval(100); // Update every 100ms
    connect(m_updateTimer, &QTimer::timeout, this, &MPVBroadcastEngine::updatePlaybackState);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MPVBroadcastEngine::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &MPVBroadcastEngine::onProcessError);
}

MPVBroadcastEngine::~MPVBroadcastEngine()
{
    stop();
}

bool MPVBroadcastEngine::initialize(const MPVBroadcastConfig& config)
{
    m_config = config;
    buildMPVArgs();
    return true;
}

bool MPVBroadcastEngine::start()
{
    if (m_isRunning) {
        return true;
    }
    
    if (m_config.ipcPath.isEmpty()) {
        QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        m_config.ipcPath = tempDir + "/mpv_ipc_" + QString::number(QDateTime::currentMSecsSinceEpoch());
    }
    
    // Add IPC path to args
    m_mpvArgs << "--input-ipc-server=" + m_config.ipcPath;
    
    m_process->start("mpv", m_mpvArgs);
    
    if (!m_process->waitForStarted(5000)) {
        emit errorOccurred("Failed to start MPV process");
        return false;
    }
    
    // Create MPV client for IPC
    m_client = new MpvClient(this);
    if (!m_client->connect(m_config.ipcPath)) {
        emit errorOccurred("Failed to connect to MPV IPC");
        return false;
    }
    
    m_isRunning = true;
    m_updateTimer->start();
    emit started();
    return true;
}

void MPVBroadcastEngine::stop()
{
    if (!m_isRunning) {
        return;
    }
    
    m_updateTimer->stop();
    
    if (m_client) {
        m_client->disconnect();
        m_client->deleteLater();
        m_client = nullptr;
    }
    
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000)) {
            m_process->kill();
        }
    }
    
    m_isRunning = false;
    emit stopped();
}

bool MPVBroadcastEngine::isRunning() const
{
    return m_isRunning;
}

void MPVBroadcastEngine::loadFile(const QString& filePath)
{
    if (m_client) {
        m_client->loadFile(filePath);
    }
}

void MPVBroadcastEngine::play()
{
    if (m_client) {
        m_client->play();
    }
}

void MPVBroadcastEngine::pause()
{
    if (m_client) {
        m_client->pause();
    }
}

void MPVBroadcastEngine::seek(double time)
{
    if (m_client) {
        m_client->seek(time);
    }
}

void MPVBroadcastEngine::setVolume(int volume)
{
    if (m_client) {
        m_client->setVolume(volume);
    }
    m_config.volume = volume;
}

void MPVBroadcastEngine::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode);
    Q_UNUSED(exitStatus);
    m_isRunning = false;
    m_updateTimer->stop();
    emit stopped();
}

void MPVBroadcastEngine::onProcessError(QProcess::ProcessError error)
{
    QString errorMsg = QString("MPV process error: %1").arg(error);
    qWarning() << errorMsg;
    emit errorOccurred(errorMsg);
}

void MPVBroadcastEngine::updatePlaybackState()
{
    if (!m_client) {
        return;
    }
    
    // Update position and duration
    double position = m_client->getPosition();
    double duration = m_client->getDuration();
    
    emit positionChanged(position);
    emit durationChanged(duration);
    
    // Check if playback ended
    if (position >= duration && duration > 0) {
        emit playbackEnded();
    }
}

void MPVBroadcastEngine::buildMPVArgs()
{
    m_mpvArgs.clear();
    
    // Window configuration
    if (!m_config.windowVisible) {
        m_mpvArgs << "--vo=null";
    } else {
        if (m_config.borderless) {
            m_mpvArgs << "--border=no";
        }
        if (m_config.fullscreen) {
            m_mpvArgs << "--fullscreen";
        }
        if (m_config.alwaysOnTop) {
            m_mpvArgs << "--ontop";
        }
        if (m_config.width > 0 && m_config.height > 0) {
            m_mpvArgs << QString("--geometry=%1x%2").arg(m_config.width).arg(m_config.height);
        }
    }
    
    // Profile
    if (m_config.profile == "gpu-hq") {
        m_mpvArgs << "--profile=gpu-hq";
    } else if (m_config.profile == "low-latency") {
        m_mpvArgs << "--profile=low-latency";
    }
    
    // Video sync
    if (!m_config.videoSync.isEmpty()) {
        m_mpvArgs << "--video-sync=" + m_config.videoSync;
    }
    
    // Hardware acceleration
    if (!m_config.hwdec.isEmpty()) {
        m_mpvArgs << "--hwdec=" + m_config.hwdec;
    }
    
    // Video output
    if (!m_config.vo.isEmpty()) {
        m_mpvArgs << "--vo=" + m_config.vo;
    }
    
    // Audio
    if (!m_config.audioDevice.isEmpty()) {
        m_mpvArgs << "--audio-device=" + m_config.audioDevice;
    }
    m_mpvArgs << QString("--volume=%1").arg(m_config.volume);
    
    // Additional args
    m_mpvArgs << m_config.additionalArgs;
}

QJsonObject MPVBroadcastConfig::toJson() const
{
    QJsonObject json;
    json["windowTitle"] = windowTitle;
    json["width"] = width;
    json["height"] = height;
    json["x"] = x;
    json["y"] = y;
    json["alwaysOnTop"] = alwaysOnTop;
    json["borderless"] = borderless;
    json["fullscreen"] = fullscreen;
    json["windowVisible"] = windowVisible;
    json["profile"] = profile;
    json["videoSync"] = videoSync;
    json["interpolation"] = interpolation;
    json["hwdec"] = hwdec;
    json["vo"] = vo;
    json["audioDevice"] = audioDevice;
    json["volume"] = volume;
    json["ipcPath"] = ipcPath;
    QJsonArray argsArray;
    for (const QString& arg : additionalArgs) {
        argsArray.append(arg);
    }
    json["additionalArgs"] = argsArray;
    return json;
}

MPVBroadcastConfig MPVBroadcastConfig::fromJson(const QJsonObject& json)
{
    MPVBroadcastConfig config;
    config.windowTitle = json["windowTitle"].toString("PLAY PLAY - Broadcast Output");
    config.width = json["width"].toInt(1920);
    config.height = json["height"].toInt(1080);
    config.x = json["x"].toInt(0);
    config.y = json["y"].toInt(0);
    config.alwaysOnTop = json["alwaysOnTop"].toBool(false);
    config.borderless = json["borderless"].toBool(true);
    config.fullscreen = json["fullscreen"].toBool(false);
    config.windowVisible = json["windowVisible"].toBool(true);
    config.profile = json["profile"].toString("gpu-hq");
    config.videoSync = json["videoSync"].toString("display-resample");
    config.interpolation = json["interpolation"].toBool(false);
    config.hwdec = json["hwdec"].toString();
    config.vo = json["vo"].toString();
    config.audioDevice = json["audioDevice"].toString();
    config.volume = json["volume"].toInt(100);
    config.ipcPath = json["ipcPath"].toString();
    QJsonArray argsArray = json["additionalArgs"].toArray();
    for (const QJsonValue& val : argsArray) {
        config.additionalArgs.append(val.toString());
    }
    return config;
}

