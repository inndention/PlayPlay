#ifndef CONTROLSERVICE_H
#define CONTROLSERVICE_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include "timeauthority.h"
#include "stateengine.h"
#include "cuedispatcher.h"
#include "engineadapter.h"
#include "database/asrunlogger.h"
#include <QJsonObject>
#include <functional>

class TrafficEngine; // NEW

class ControlService : public QObject
{
    Q_OBJECT

public:
    explicit ControlService(QObject* parent = nullptr);
    ~ControlService();
    
    bool initialize();
    void start();
    void stop();
    void shutdown();
    
    void registerEngine(const QString& engineId, const QString& engineType, const QJsonObject& config);
    void unregisterEngine(const QString& engineId);
    
    void updateSchedule(const QList<PlannedEvent>& events);
    
    // Manual Playout Controls
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stopPlayback();
    Q_INVOKABLE void skipNext();
    Q_INVOKABLE void playItem(const QString& assetId, const QString& path);
    void setSkipHandler(std::function<void()> f) { m_skipHandler = std::move(f); }

    // Current playout state for UI (e.g. ON AIR row)
    Q_PROPERTY(QString currentPlayingAssetId READ currentPlayingAssetId NOTIFY currentPlayingAssetIdChanged)
    QString currentPlayingAssetId() const { return m_currentPlayingAssetId; }

signals:
    void currentPlayingAssetIdChanged();
    void stateTransition(const StateTransition& transition);
    void cueExecuted(const QString& cueId, const QString& engineId);
    void engineRegistered(const QString& engineId);
    void engineUnregistered(const QString& engineId);

private slots:
    void onObservationTimer();
    void onEvaluationTimer();
    void onCueProcessingTimer();
    void onStateTransition(const StateTransition& transition);

private:
    TimeAuthority* m_timeAuthority;
    StateEngine* m_stateEngine;
    CueDispatcher* m_cueDispatcher;
    AsRunLogger* m_asRunLogger;
    TrafficEngine* m_trafficEngine; // NEW
    
    QMap<QString, EngineAdapter*> m_engineAdapters;
    
    QTimer* m_observationTimer;      // 10Hz (100ms)
    QTimer* m_evaluationTimer;        // 20Hz (50ms)
    QTimer* m_cueProcessingTimer;     // 20Hz (50ms)
    
    bool m_isRunning;
    QString m_currentPlayingAssetId;
    std::function<void()> m_skipHandler;
    
    void observeEngines();
    void evaluateState();
    void processCues();
};

ControlService* getControlService();

#endif // CONTROLSERVICE_H

