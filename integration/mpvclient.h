#ifndef MPVCLIENT_H
#define MPVCLIENT_H

#include <QObject>
#include <QProcess>
#include <QString>

class MpvClient : public QObject
{
    Q_OBJECT

public:
    explicit MpvClient(QObject* parent = nullptr);
    
    bool initialize();
    void shutdown();
    
    bool loadFile(const QString& filePath);
    bool play();
    bool pause();
    bool stop();
    bool seek(double timeSeconds);
    
    bool isConnected() const { return m_isConnected; }
    bool connect(const QString& ipcPath);

signals:
    void disconnected();
    void playbackStarted();
    void playbackPaused();
    void playbackStopped();
    void positionChanged(double position);
    void durationChanged(double duration);

private slots:
    void onStdout();
    void onStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString findMpvExecutable() const;
    bool sendCommand(const QByteArray& command);
    
    QProcess* m_process;
    bool m_isConnected;
    QString m_ipcSocketPath;
    int m_requestId;
};

#endif // MPVCLIENT_H

