#include "cuedispatcher.h"
#include "engineadapter.h"
#include <QDebug>

CueDispatcher::CueDispatcher(QObject* parent)
    : QObject(parent)
    , m_processTimer(new QTimer(this))
    , m_lastProcessTime(0)
{
    // Process cues at 20Hz (50ms intervals)
    m_processTimer->setInterval(50);
    connect(m_processTimer, &QTimer::timeout, this, &CueDispatcher::onProcessTimer);
    m_processTimer->start();
}

void CueDispatcher::queueCue(const QString& cueId, const QString& targetEngine,
                              qint64 earliestTime, qint64 expiryTime,
                              const QString& command, const QJsonObject& metadata)
{
    Cue cue;
    cue.cueId = cueId;
    cue.targetEngine = targetEngine;
    cue.earliestTime = earliestTime;
    cue.expiryTime = expiryTime;
    cue.command = command;
    cue.metadata = metadata;
    cue.executed = false;
    cue.executedAt = 0;
    
    m_cueQueue.enqueue(cue);
}

void CueDispatcher::cancelCue(const QString& cueId)
{
    // Remove from queue
    QQueue<Cue> newQueue;
    while (!m_cueQueue.isEmpty()) {
        Cue cue = m_cueQueue.dequeue();
        if (cue.cueId != cueId) {
            newQueue.enqueue(cue);
        }
    }
    m_cueQueue = newQueue;
}

void CueDispatcher::processCues(qint64 currentTime)
{
    m_lastProcessTime = currentTime;
    
    QQueue<Cue> remainingCues;
    
    while (!m_cueQueue.isEmpty()) {
        Cue cue = m_cueQueue.dequeue();
        
        // Check if expired
        if (currentTime > cue.expiryTime) {
            emit cueExpired(cue.cueId);
            continue;
        }
        
        // Check if ready to execute
        if (currentTime >= cue.earliestTime && !cue.executed) {
            executeCue(cue);
            cue.executed = true;
            cue.executedAt = currentTime;
            m_executedCues[cue.cueId] = cue;
        } else {
            remainingCues.enqueue(cue);
        }
    }
    
    m_cueQueue = remainingCues;
}

QList<Cue> CueDispatcher::pendingCues() const
{
    QList<Cue> cues;
    for (const auto& cue : m_cueQueue) {
        cues.append(cue);
    }
    return cues;
}

QList<Cue> CueDispatcher::executedCues() const
{
    QList<Cue> cues;
    for (auto it = m_executedCues.begin(); it != m_executedCues.end(); ++it) {
        cues.append(it.value());
    }
    return cues;
}

void CueDispatcher::executeCue(const Cue& cue)
{
    // TODO: Execute cue on target engine
    qDebug() << "[CueDispatcher] Executing cue:" << cue.cueId << "on" << cue.targetEngine << "command:" << cue.command;
    emit cueExecuted(cue.cueId, cue.targetEngine, cue.command);
}

void CueDispatcher::onProcessTimer()
{
    // Get current time from time authority
    // For now, use QDateTime
    qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
    processCues(currentTime);
}

