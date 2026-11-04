#ifndef MPVBROADCASTENGINE_H
#define MPVBROADCASTENGINE_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include "integration/mpvclient.h"

struct MPVBroadcastConfig {
    QString windowTitle;
    int width;
    int height;
    int x;
    int y;
    bool alwaysOnTop;
    bool borderless;
    bool fullscreen;
    bool windowVisible;
    QString profile; // "gpu-hq", "low-latency", "high-quality"
    QString videoSync;
    bool interpolation;
    QString hwdec;
    QString vo;
    QString audioDevice;
    int volume;
    QString ipcPath;
    QStringList additionalArgs;
    
    QJsonObject toJson() const;
    static MPVBroadcastConfig fromJson(const QJsonObject& json);
};

class MPVBroadcastEngine : public QObject
{
    Q_OBJECT
    
public:
    explicit MPVBroadcastEngine(QObject* parent = nullptr);
    ~MPVBroadcastEngine();
    
    // Initialize with config
    bool initialize(const MPVBroadcastConfig& config);
    
    // Control
    bool start();
    void stop();
    bool isRunning() const;
    
    // Playback control
    void loadFile(const QString& filePath);
    void play();
    void pause();
    void seek(double time);
    void setVolume(int volume);
    
    // Get status
    MPVBroadcastConfig getConfig() const { return m_config; }
    
signals:
    void started();
    void stopped();
    void playbackStarted();
    void playbackPaused();
    void playbackEnded();
    void errorOccurred(const QString& error);
    void positionChanged(double position);
    void durationChanged(double duration);
    
private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void updatePlaybackState();
    
private:
    void buildMPVArgs();
    QStringList m_mpvArgs;
    QProcess* m_process;
    MpvClient* m_client;
    MPVBroadcastConfig m_config;
    QTimer* m_updateTimer;
    bool m_isRunning;
};

#endif // MPVBROADCASTENGINE_H

