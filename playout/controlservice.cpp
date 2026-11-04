#include "controlservice.h"
#include "services/trafficengine.h"
#include <QDebug>

static ControlService* g_controlService = nullptr;

ControlService::ControlService(QObject* parent)
    : QObject(parent)
    , m_timeAuthority(getTimeAuthority())
    , m_stateEngine(new StateEngine(this))
    , m_cueDispatcher(new CueDispatcher(this))
    , m_asRunLogger(getAsRunLogger())
    , m_trafficEngine(new TrafficEngine(this)) // NEW
    , m_observationTimer(new QTimer(this))
    , m_evaluationTimer(new QTimer(this))
    , m_cueProcessingTimer(new QTimer(this))
    , m_isRunning(false)
{
    connect(m_stateEngine, &StateEngine::stateChanged, this, &ControlService::onStateTransition);
    
    // Observation loop: 10Hz (100ms)
    m_observationTimer->setInterval(100);
    connect(m_observationTimer, &QTimer::timeout, this, &ControlService::onObservationTimer);
    
    // Evaluation loop: 20Hz (50ms)
    m_evaluationTimer->setInterval(50);
    connect(m_evaluationTimer, &QTimer::timeout, this, &ControlService::onEvaluationTimer);
    
    // Cue processing loop: 20Hz (50ms)
    m_cueProcessingTimer->setInterval(50);
    connect(m_cueProcessingTimer, &QTimer::timeout, this, &ControlService::onCueProcessingTimer);
}

ControlService::~ControlService()
{
    shutdown();
}

bool ControlService::initialize()
{
    if (!m_asRunLogger->initialize()) {
        qWarning() << "[ControlService] Failed to initialize as-run logger";
        return false;
    }
    
    return true;
}

void ControlService::start()
{
    if (m_isRunning) {
        return;
    }
    
    m_isRunning = true;
    m_observationTimer->start();
    m_evaluationTimer->start();
    m_cueProcessingTimer->start();
    
    qDebug() << "[ControlService] Started";
}

void ControlService::stop()
{
    if (!m_isRunning) {
        return;
    }
    
    m_isRunning = false;
    m_observationTimer->stop();
    m_evaluationTimer->stop();
    m_cueProcessingTimer->stop();
    
    qDebug() << "[ControlService] Stopped";
}

void ControlService::shutdown()
{
    stop();
    
    // Disconnect all engines
    for (auto it = m_engineAdapters.begin(); it != m_engineAdapters.end(); ++it) {
        it.value()->shutdown();
        it.value()->deleteLater();
    }
    m_engineAdapters.clear();
    
    m_asRunLogger->close();
}

void ControlService::registerEngine(const QString& engineId, const QString& engineType, const QJsonObject& config)
{
    if (m_engineAdapters.contains(engineId)) {
        qWarning() << "[ControlService] Engine already registered:" << engineId;
        return;
    }
    
    EngineAdapter* adapter = createEngineAdapter(engineId, engineType, config);
    if (!adapter) {
        qWarning() << "[ControlService] Failed to create engine adapter:" << engineId;
        return;
    }
    
    if (!adapter->initialize()) {
        qWarning() << "[ControlService] Failed to initialize engine:" << engineId;
        adapter->deleteLater();
        return;
    }
    
    m_engineAdapters[engineId] = adapter;
    connect(adapter, &EngineAdapter::stateChanged, this, [this, engineId](const EngineState& state) {
        m_stateEngine->updateEngineState(engineId, state);
    });
    
    emit engineRegistered(engineId);
    qDebug() << "[ControlService] Registered engine:" << engineId << "type:" << engineType;
}

void ControlService::unregisterEngine(const QString& engineId)
{
    if (!m_engineAdapters.contains(engineId)) {
        return;
    }
    
    EngineAdapter* adapter = m_engineAdapters.take(engineId);
    adapter->shutdown();
    adapter->deleteLater();
    
    emit engineUnregistered(engineId);
    qDebug() << "[ControlService] Unregistered engine:" << engineId;
}

void ControlService::updateSchedule(const QList<PlannedEvent>& events)
{
    m_stateEngine->updatePlannedEvents(events);
}

void ControlService::observeEngines()
{
    for (auto it = m_engineAdapters.begin(); it != m_engineAdapters.end(); ++it) {
        EngineState state = it.value()->observe();
        m_stateEngine->updateEngineState(it.key(), state);
    }
    // Expose current playing asset for UI (e.g. playlist ON AIR row)
    if (!m_engineAdapters.isEmpty()) {
        EngineState primaryState = m_engineAdapters.first()->observe();
        QString id = (primaryState.status == EngineStatus::Playing) ? primaryState.currentAssetId : QString();
        if (id != m_currentPlayingAssetId) {
            m_currentPlayingAssetId = id;
            emit currentPlayingAssetIdChanged();
        }
    }
}

void ControlService::evaluateState()
{
    qint64 currentTime = m_timeAuthority->now();
    m_stateEngine->evaluate(currentTime);
    
    SystemState state = m_stateEngine->currentState();
    const PlannedEvent* event = m_stateEngine->currentEvent();
    
    // Choose primary engine (simplified for now - take first registered)
    if (m_engineAdapters.isEmpty()) return;
    EngineAdapter* primary = m_engineAdapters.first();
    QString engineId = m_engineAdapters.firstKey();
    
    switch (state) {
        case SystemState::ExpectedPlaying:
        case SystemState::LateStart: {
            // Reconcile regular planned event
            if (event) {
                EngineState engineState = primary->observe();
                if (engineState.currentAssetId != event->assetId || engineState.status != EngineStatus::Playing) {
                    qDebug() << "[ControlService] AUTOMATION RECONCILIATION:" << event->assetId;
                    
                    // CRITICAL: Actually load the asset into the engine!
                    primary->loadAsset(event->assetId, event->metadata["src"].toString());
                    
                    // If late start, seek to correct position
                    qint64 seekPos = currentTime - event->startTime;
                    if (seekPos > 500) {
                        primary->seek(seekPos / 1000.0);
                    }
                    primary->play();
                }
            }
            break;
        }
        
        case SystemState::Idle: {
            // TRIGGER FILL (NEW - Ported from Electron)
            EngineState engineState = primary->observe();
            if (engineState.status != EngineStatus::Playing) {
                TrafficEngine::SelectionContext context;
                context.now = currentTime;
                context.availableDuration = 3600; // Look ahead 1 hour
                context.recentPlays = m_trafficEngine->getTrafficLog(QDateTime::fromMSecsSinceEpoch(currentTime).addSecs(-3600), QDateTime::fromMSecsSinceEpoch(currentTime));
                context.hour = QDateTime::fromMSecsSinceEpoch(currentTime).time().hour();
                context.daypart = m_trafficEngine->getDaypart(context.hour);
                
                // For demo, we need a list of fill candidates. 
                // Normally we'd fetch this from MediaLibraryModel or MediaDb.
                QList<MediaItem> allMedia; // TODO: Fetch from MediaDb
                
                TrafficEngine::SelectionResult fill = m_trafficEngine->selectNextAd(allMedia, context);
                if (!fill.item.id.isEmpty()) {
                    qDebug() << "[ControlService] Filling gap with:" << fill.item.title << "Reason:" << fill.reason;
                    primary->loadAsset(fill.item.id, fill.item.src.toString());
                    primary->play();
                }
            }
            break;
        }
        
        default:
            break;
    }
}

void ControlService::processCues()
{
    qint64 currentTime = m_timeAuthority->now();
    m_cueDispatcher->processCues(currentTime);
}

void ControlService::onObservationTimer()
{
    observeEngines();
}

void ControlService::onEvaluationTimer()
{
    evaluateState();
}

void ControlService::onCueProcessingTimer()
{
    processCues();
}

void ControlService::play()
{
    qDebug() << "[ControlService] Manual Play";
    for (auto adapter : m_engineAdapters) {
        adapter->play();
    }
}

void ControlService::pause()
{
    qDebug() << "[ControlService] Manual Pause";
    for (auto adapter : m_engineAdapters) {
        adapter->pause();
    }
}

void ControlService::stopPlayback()
{
    qDebug() << "[ControlService] Manual Stop Playback";
    for (auto adapter : m_engineAdapters) {
        adapter->stop();
    }
}

void ControlService::skipNext()
{
    qDebug() << "[ControlService] Skip Next";
    if (m_skipHandler) m_skipHandler();
}

void ControlService::playItem(const QString& assetId, const QString& path)
{
    if (m_engineAdapters.isEmpty()) {
        qWarning() << "[ControlService] playItem: no engine registered";
        return;
    }
    EngineAdapter* primary = m_engineAdapters.first();
    QString pathToUse = path;
    if (pathToUse.isEmpty()) {
        pathToUse = assetId; // fallback for URL or path-only
    }
    if (primary->loadAsset(assetId, pathToUse)) {
        primary->play();
        qDebug() << "[ControlService] playItem:" << assetId;
    } else {
        qWarning() << "[ControlService] playItem: loadAsset failed for" << assetId;
    }
}

void ControlService::onStateTransition(const StateTransition& transition)
{
    emit stateTransition(transition);
    
    // Log to as-run if event-related
    const PlannedEvent* event = m_stateEngine->currentEvent();
    if (event) {
        QString status;
        
        switch(transition.toState) {
            case SystemState::ExpectedPlaying:
                status = "ON_TIME";
                break;
            case SystemState::LateStart:
                status = "LATE_START";
                break;
            case SystemState::Overrun:
                status = "OVERRUN";
                break;
            case SystemState::Desync:
                status = "DESYNC";
                break;
            default:
                status = "UNKNOWN";
        }
        
        m_asRunLogger->logEvent(
            event->eventId,
            event->startTime,
            m_timeAuthority->now(),
            event->engineId,
            event->assetId,
            status,
            transition.reason,
            transition.metadata
        );
    }
}

ControlService* getControlService()
{
    if (!g_controlService) {
        g_controlService = new ControlService();
    }
    return g_controlService;
}

