#include "masterdashboard.h"
#include "playout/controlservice.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QProgressBar>
#include <QDateTime>

MasterDashboard::MasterDashboard(QWidget *parent)
    : QWidget(parent)
    , m_controlService(getControlService())
    , m_statusLabel(new QLabel(tr("Status: Ready"), this))
    , m_timeLabel(new QLabel("00:00:00", this))
    , m_playButton(new QPushButton(tr("Play"), this))
    , m_pauseButton(new QPushButton(tr("Pause"), this))
    , m_stopButton(new QPushButton(tr("Stop"), this))
    , m_timer(new QTimer(this))
{
    setStyleSheet(
        "QWidget { background-color: #09090b; color: white; }"
        "QLabel { color: white; }"
        "QPushButton { background-color: #3b82f6; color: white; border: none; padding: 10px; border-radius: 4px; font-weight: bold; }"
        "QPushButton:hover { background-color: #2563eb; }"
        "QPushButton:pressed { background-color: #1d4ed8; }"
        "QPushButton:disabled { background-color: #64748b; }"
        "QGroupBox { border: 1px solid #3b82f6; border-radius: 4px; margin-top: 10px; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
    );
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    
    // Header
    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(tr("Master Dashboard"), this);
    titleLabel->setStyleSheet("font-size: 28px; font-weight: bold;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_statusLabel);
    headerLayout->addWidget(m_timeLabel);
    mainLayout->addLayout(headerLayout);
    
    // Control buttons
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->addWidget(m_playButton);
    controlLayout->addWidget(m_pauseButton);
    controlLayout->addWidget(m_stopButton);
    controlLayout->addStretch();
    mainLayout->addLayout(controlLayout);
    
    // Status grid
    QGridLayout* statusGrid = new QGridLayout();
    statusGrid->setSpacing(15);
    
    // Engine status
    QGroupBox* engineGroup = new QGroupBox(tr("Engine Status"), this);
    QVBoxLayout* engineLayout = new QVBoxLayout(engineGroup);
    m_engineStatusLabel = new QLabel(tr("Idle"), engineGroup);
    m_engineStatusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #94a3b8;");
    engineLayout->addWidget(m_engineStatusLabel);
    statusGrid->addWidget(engineGroup, 0, 0);
    
    // Current item
    QGroupBox* currentGroup = new QGroupBox(tr("Current Item"), this);
    QVBoxLayout* currentLayout = new QVBoxLayout(currentGroup);
    m_currentItemLabel = new QLabel(tr("No item"), currentGroup);
    m_currentItemLabel->setStyleSheet("font-size: 16px;");
    currentLayout->addWidget(m_currentItemLabel);
    statusGrid->addWidget(currentGroup, 0, 1);
    
    // Progress
    QGroupBox* progressGroup = new QGroupBox(tr("Progress"), this);
    QVBoxLayout* progressLayout = new QVBoxLayout(progressGroup);
    m_progressBar = new QProgressBar(progressGroup);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    progressLayout->addWidget(m_progressBar);
    statusGrid->addWidget(progressGroup, 0, 2);
    
    // Schedule status
    QGroupBox* scheduleGroup = new QGroupBox(tr("Schedule"), this);
    QVBoxLayout* scheduleLayout = new QVBoxLayout(scheduleGroup);
    m_scheduleStatusLabel = new QLabel(tr("On schedule"), scheduleGroup);
    m_scheduleStatusLabel->setStyleSheet("font-size: 16px; color: #22c55e;");
    scheduleLayout->addWidget(m_scheduleStatusLabel);
    statusGrid->addWidget(scheduleGroup, 1, 0);
    
    // Next item
    QGroupBox* nextGroup = new QGroupBox(tr("Next Item"), this);
    QVBoxLayout* nextLayout = new QVBoxLayout(nextGroup);
    m_nextItemLabel = new QLabel(tr("No next item"), nextGroup);
    m_nextItemLabel->setStyleSheet("font-size: 16px;");
    nextLayout->addWidget(m_nextItemLabel);
    statusGrid->addWidget(nextGroup, 1, 1);
    
    // Time remaining
    QGroupBox* timeGroup = new QGroupBox(tr("Time Remaining"), this);
    QVBoxLayout* timeLayout = new QVBoxLayout(timeGroup);
    m_timeRemainingLabel = new QLabel("00:00:00", timeGroup);
    m_timeRemainingLabel->setStyleSheet("font-size: 18px; font-weight: bold; font-family: monospace;");
    timeLayout->addWidget(m_timeRemainingLabel);
    statusGrid->addWidget(timeGroup, 1, 2);
    
    mainLayout->addLayout(statusGrid);
    mainLayout->addStretch();
    
    // Connect signals
    connect(m_playButton, &QPushButton::clicked, this, &MasterDashboard::onPlay);
    connect(m_pauseButton, &QPushButton::clicked, this, &MasterDashboard::onPause);
    connect(m_stopButton, &QPushButton::clicked, this, &MasterDashboard::onStop);
    
    if (m_controlService) {
        connect(m_controlService, &ControlService::stateTransition, this, &MasterDashboard::onStateTransition);
    }
    
    // Update timer
    m_timer->setInterval(100); // Update 10 times per second
    connect(m_timer, &QTimer::timeout, this, &MasterDashboard::updateDisplay);
    m_timer->start();
    
    updateDisplay();
}

void MasterDashboard::onPlay()
{
    Logger::instance()->info("Master dashboard: Play");
    // TODO: Trigger play via control service
    m_engineStatusLabel->setText(tr("Playing"));
    m_engineStatusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #22c55e;");
}

void MasterDashboard::onPause()
{
    Logger::instance()->info("Master dashboard: Pause");
    // TODO: Trigger pause via control service
    m_engineStatusLabel->setText(tr("Paused"));
    m_engineStatusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #f59e0b;");
}

void MasterDashboard::onStop()
{
    Logger::instance()->info("Master dashboard: Stop");
    // TODO: Trigger stop via control service
    m_engineStatusLabel->setText(tr("Stopped"));
    m_engineStatusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #ef4444;");
    m_progressBar->setValue(0);
}

void MasterDashboard::onStateTransition(const StateTransition& transition)
{
    QString statusText;
    QString color;
    
    switch(transition.toState) {
        case SystemState::ExpectedPlaying:
            statusText = tr("On Schedule");
            color = "#22c55e";
            break;
        case SystemState::LateStart:
            statusText = tr("Late Start");
            color = "#f59e0b";
            break;
        case SystemState::Overrun:
            statusText = tr("Overrun");
            color = "#ef4444";
            break;
        case SystemState::Desync:
            statusText = tr("Desync");
            color = "#ef4444";
            break;
        default:
            statusText = tr("Unknown");
            color = "#94a3b8";
    }
    
    m_scheduleStatusLabel->setText(statusText);
    m_scheduleStatusLabel->setStyleSheet(QString("font-size: 16px; color: %1;").arg(color));
}

void MasterDashboard::updateDisplay()
{
    // Update time
    QDateTime now = QDateTime::currentDateTime();
    m_timeLabel->setText(now.toString("hh:mm:ss"));
    
    // TODO: Update current item, next item, progress, time remaining from control service
}

