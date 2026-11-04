#include "systemmonitormodel.h"
#include <QRandomGenerator>

SystemMonitorModel::SystemMonitorModel(QObject* parent)
    : QObject(parent)
    , m_cpuUsage(0.0)
    , m_ramUsage(0.0)
    , m_engineState("IDLE")
    , m_timer(new QTimer(this))
{
    connect(m_timer, &QTimer::timeout, this, &SystemMonitorModel::updateStats);
    m_timer->start(1000); // Update every second
}

void SystemMonitorModel::updateStats()
{
    // Simulate CPU and RAM usage for now
    // In a real app, use Windows API or a cross-platform library like libhal/psutil equivalent
    double newCpu = 5.0 + QRandomGenerator::global()->bounded(15.0);
    double newRam = 30.0 + QRandomGenerator::global()->bounded(10.0);
    
    if (qAbs(m_cpuUsage - newCpu) > 0.1) {
        m_cpuUsage = newCpu;
        emit cpuUsageChanged();
    }
    
    if (qAbs(m_ramUsage - newRam) > 0.1) {
        m_ramUsage = newRam;
        emit ramUsageChanged();
    }

    // Mock engine state rotation
    static int counter = 0;
    counter++;
    if (counter % 10 == 0) {
        m_engineState = (m_engineState == "IDLE") ? "PLAYING" : "IDLE";
        emit engineStateChanged();
    }
}
