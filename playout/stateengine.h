#ifndef STATEENGINE_H
#define STATEENGINE_H

#include <QObject>
#include <QMap>
#include <QList>
#include <QTimer>
#include "timeauthority.h"
#include "engineadapter.h"
#include <QJsonObject>

struct PlannedEvent {
    QString eventId;
    qint64 startTime;      // UTC milliseconds
    qint64 duration;       // milliseconds
    qint64 endTime;
    QString assetId;
    QString engineId;
    int priority;
    QJsonObject metadata;
    
    bool isActiveAt(qint64 time) const;
    bool isOverdue(qint64 time) const;
    double secondsUntilStart(qint64 time) const;
};

enum class SystemState {
    ExpectedPlaying,
    LateStart,
    Overrun,
    Desync,
    EngineUnresponsive,
    FailoverActive,
    Idle,
    Error
};

struct StateTransition {
    SystemState fromState;
    SystemState toState;
    QString reason;
    qint64 timestamp;
    QJsonObject metadata;
};

class StateEngine : public QObject
{
    Q_OBJECT

public:
    explicit StateEngine(QObject* parent = nullptr);
    
    void updatePlannedEvents(const QList<PlannedEvent>& events);
    void updateEngineState(const QString& engineId, const EngineState& state);
    void evaluate(qint64 currentTime);
    
    SystemState currentState() const { return m_currentState; }
    const PlannedEvent* currentEvent() const { return m_currentEvent; }
    
    void setToleranceFrames(int frames) { m_toleranceFrames = frames; }
    void setMaxLateStartMs(qint64 ms) { m_maxLateStartMs = ms; }

signals:
    void stateChanged(const StateTransition& transition);

private:
    TimeAuthority* m_timeAuthority;
    SystemState m_currentState;
    PlannedEvent* m_currentEvent;
    QMap<QString, EngineState> m_engineStates;
    QMap<QString, PlannedEvent> m_plannedEvents;
    QList<StateTransition> m_transitionHistory;
    int m_toleranceFrames;
    qint64 m_maxLateStartMs;
    
    void transitionTo(SystemState newState, const QString& reason, const QJsonObject& metadata = QJsonObject());
    SystemState determineState(qint64 currentTime);
};

#endif // STATEENGINE_H

