#include "stateengine.h"
#include <QDebug>

StateEngine::StateEngine(QObject* parent)
    : QObject(parent)
    , m_timeAuthority(getTimeAuthority())
    , m_currentState(SystemState::Idle)
    , m_currentEvent(nullptr)
    , m_toleranceFrames(2)
    , m_maxLateStartMs(5000)
{
}

void StateEngine::updatePlannedEvents(const QList<PlannedEvent>& events)
{
    m_plannedEvents.clear();
    for (const auto& event : events) {
        m_plannedEvents[event.eventId] = event;
    }
}

void StateEngine::updateEngineState(const QString& engineId, const EngineState& state)
{
    m_engineStates[engineId] = state;
}

void StateEngine::evaluate(qint64 currentTime)
{
    SystemState newState = determineState(currentTime);
    
    if (newState != m_currentState) {
        QString reason;
        QJsonObject metadata;
        
        switch(newState) {
            case SystemState::LateStart:
                reason = "Event started late";
                break;
            case SystemState::Overrun:
                reason = "Event overran scheduled time";
                break;
            case SystemState::Desync:
                reason = "Engine desynchronized from schedule";
                break;
            case SystemState::EngineUnresponsive:
                reason = "Engine not responding";
                break;
            default:
                reason = "State transition";
        }
        
        transitionTo(newState, reason, metadata);
    }
}

SystemState StateEngine::determineState(qint64 currentTime)
{
    // Find active event
    const PlannedEvent* activeEvent = nullptr;
    for (auto it = m_plannedEvents.begin(); it != m_plannedEvents.end(); ++it) {
        if (it.value().isActiveAt(currentTime)) {
            activeEvent = &it.value();
            break;
        }
    }
    
    m_currentEvent = const_cast<PlannedEvent*>(activeEvent);
    
    if (!activeEvent) {
        return SystemState::Idle;
    }
    
    // Check engine state
    if (!m_engineStates.contains(activeEvent->engineId)) {
        return SystemState::EngineUnresponsive;
    }
    
    EngineState engineState = m_engineStates[activeEvent->engineId];
    
    // Check if event started on time
    qint64 lateBy = currentTime - activeEvent->startTime;
    if (lateBy > m_maxLateStartMs && engineState.status != EngineStatus::Playing) {
        return SystemState::LateStart;
    }
    
    // Check if event overran
    if (currentTime > activeEvent->endTime && engineState.status == EngineStatus::Playing) {
        return SystemState::Overrun;
    }
    
    // Check if engine is playing correct asset
    if (engineState.status == EngineStatus::Playing) {
        if (engineState.currentAssetId != activeEvent->assetId) {
            return SystemState::Desync;
        }
    }
    
    return SystemState::ExpectedPlaying;
}

void StateEngine::transitionTo(SystemState newState, const QString& reason, const QJsonObject& metadata)
{
    StateTransition transition;
    transition.fromState = m_currentState;
    transition.toState = newState;
    transition.reason = reason;
    transition.timestamp = m_timeAuthority->now();
    transition.metadata = metadata;
    
    m_transitionHistory.append(transition);
    if (m_transitionHistory.size() > 1000) {
        m_transitionHistory.removeFirst();
    }
    
    m_currentState = newState;
    emit stateChanged(transition);
}

bool PlannedEvent::isActiveAt(qint64 time) const
{
    return time >= startTime && time < endTime;
}

bool PlannedEvent::isOverdue(qint64 time) const
{
    return time > startTime;
}

double PlannedEvent::secondsUntilStart(qint64 time) const
{
    return (startTime - time) / 1000.0;
}

