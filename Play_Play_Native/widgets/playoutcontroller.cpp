#include "playoutcontroller.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTime>

PlayoutController::PlayoutController(QWidget *parent)
    : QWidget(parent)
    , m_playButton(new QPushButton("Play", this))
    , m_pauseButton(new QPushButton("Pause", this))
    , m_stopButton(new QPushButton("Stop", this))
    , m_timeLabel(new QLabel("00:00:00 / 00:00:00", this))
    , m_positionSlider(new QSlider(Qt::Horizontal, this))
    , m_updateTimer(new QTimer(this))
    , m_isPlaying(false)
    , m_currentTime(0.0)
    , m_duration(0.0)
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_playButton);
    buttonLayout->addWidget(m_pauseButton);
    buttonLayout->addWidget(m_stopButton);
    
    layout->addLayout(buttonLayout);
    layout->addWidget(m_timeLabel);
    layout->addWidget(m_positionSlider);
    
    m_positionSlider->setRange(0, 1000);
    m_positionSlider->setValue(0);
    
    m_updateTimer->setInterval(100); // Update 10 times per second
    connect(m_updateTimer, &QTimer::timeout, this, &PlayoutController::onUpdateTime);
    
    connect(m_playButton, &QPushButton::clicked, this, &PlayoutController::onPlay);
    connect(m_pauseButton, &QPushButton::clicked, this, &PlayoutController::onPause);
    connect(m_stopButton, &QPushButton::clicked, this, &PlayoutController::onStop);
}

void PlayoutController::onPlay()
{
    m_isPlaying = true;
    m_updateTimer->start();
    // TODO: Start playback
}

void PlayoutController::onPause()
{
    m_isPlaying = false;
    m_updateTimer->stop();
    // TODO: Pause playback
}

void PlayoutController::onStop()
{
    m_isPlaying = false;
    m_currentTime = 0.0;
    m_updateTimer->stop();
    m_positionSlider->setValue(0);
    // TODO: Stop playback
}

void PlayoutController::onUpdateTime()
{
    if (m_isPlaying) {
        m_currentTime += 0.1; // Increment by 100ms
    }
    
    if (m_duration > 0) {
        m_positionSlider->setValue(static_cast<int>((m_currentTime / m_duration) * 1000));
    }
    
    QTime current = QTime(0, 0).addMSecs(static_cast<int>(m_currentTime * 1000));
    QTime total = QTime(0, 0).addMSecs(static_cast<int>(m_duration * 1000));
    
    m_timeLabel->setText(current.toString("hh:mm:ss") + " / " + total.toString("hh:mm:ss"));
}

