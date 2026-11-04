#ifndef ENGINEADAPTER_H
#define ENGINEADAPTER_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QDateTime>

enum class EngineStatus {
    Idle,
    Loading,
    Playing,
    Paused,
    Stopped,
    Error
};

struct EngineState {
    QString engineId;
    EngineStatus status;
    QString currentAssetId;
    double position;      // Current playback position (seconds)
    double duration;      // Total duration (seconds)
    bool isReady;
    QString errorMessage;
    QJsonObject metadata;
};

class EngineAdapter : public QObject
{
    Q_OBJECT

public:
    explicit EngineAdapter(const QString& engineId, QObject* parent = nullptr);
    virtual ~EngineAdapter() = default;
    
    QString engineId() const { return m_engineId; }
    EngineState state() const { return m_state; }
    
    // Engine operations
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool loadAsset(const QString& assetId, const QString& assetPath) = 0;
    virtual bool play() = 0;
    virtual bool pause() = 0;
    virtual bool stop() = 0;
    virtual bool seek(double timeSeconds) = 0;
    
    // State observation
    virtual EngineState observe() = 0;
    
    // Update state (called by control service)
    void updateState(const EngineState& state);

signals:
    void stateChanged(const EngineState& state);
    void error(const QString& error);

protected:
    QString m_engineId;
    EngineState m_state;
};

// Factory function
EngineAdapter* createEngineAdapter(const QString& engineId, const QString& engineType, const QJsonObject& config);
class SupervisorClient;
void setSupervisorClientForAdapters(SupervisorClient* client);

#endif // ENGINEADAPTER_H

