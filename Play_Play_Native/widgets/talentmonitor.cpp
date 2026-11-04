#include "talentmonitor.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QDateTime>

TalentMonitor::TalentMonitor(QWidget *parent)
    : QWidget(parent)
    , m_nameLabel(new QLabel(tr("Talent Monitor"), this))
    , m_statusLabel(new QLabel(tr("OFF AIR"), this))
    , m_showLabel(new QLabel(tr("No show"), this))
    , m_timeLabel(new QLabel("00:00:00", this))
    , m_onAirButton(new QPushButton(tr("Go On Air"), this))
    , m_offAirButton(new QPushButton(tr("Go Off Air"), this))
    , m_timer(new QTimer(this))
    , m_isOnAir(false)
{
    setStyleSheet(
        "QWidget { background-color: #1e293b; }"
        "QLabel { color: white; }"
        "QPushButton { background-color: #3b82f6; color: white; border: none; padding: 8px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #2563eb; }"
        "QPushButton:pressed { background-color: #1d4ed8; }"
    );
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    
    // Name
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setStyleSheet("font-size: 24px; font-weight: bold;");
    mainLayout->addWidget(m_nameLabel);
    
    // Status indicator
    QHBoxLayout* statusLayout = new QHBoxLayout();
    statusLayout->addStretch();
    
    QLabel* statusIndicator = new QLabel(this);
    statusIndicator->setFixedSize(20, 20);
    statusIndicator->setStyleSheet("background-color: #ef4444; border-radius: 10px;");
    statusLayout->addWidget(statusIndicator);
    
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #ef4444;");
    statusLayout->addWidget(m_statusLabel);
    
    statusLayout->addStretch();
    mainLayout->addLayout(statusLayout);
    
    // Show name
    m_showLabel->setAlignment(Qt::AlignCenter);
    m_showLabel->setStyleSheet("font-size: 14px; color: #94a3b8;");
    mainLayout->addWidget(m_showLabel);
    
    // Time display
    m_timeLabel->setAlignment(Qt::AlignCenter);
    m_timeLabel->setStyleSheet("font-size: 16px; font-family: monospace; color: #94a3b8;");
    mainLayout->addWidget(m_timeLabel);
    
    mainLayout->addStretch();
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_onAirButton);
    buttonLayout->addWidget(m_offAirButton);
    mainLayout->addLayout(buttonLayout);
    
    m_offAirButton->setEnabled(false);
    
    connect(m_onAirButton, &QPushButton::clicked, this, &TalentMonitor::onGoOnAir);
    connect(m_offAirButton, &QPushButton::clicked, this, &TalentMonitor::onGoOffAir);
    
    // Timer for time updates
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &TalentMonitor::updateTime);
    m_timer->start();
    
    updateTime();
}

void TalentMonitor::setTalentName(const QString& name)
{
    m_nameLabel->setText(name);
    m_talentName = name;
    emit talentNameChanged(name);
}

void TalentMonitor::setShowName(const QString& showName)
{
    m_showLabel->setText(showName);
    m_showName = showName;
    emit showNameChanged(showName);
}

void TalentMonitor::setOnAir(bool onAir)
{
    if (m_isOnAir == onAir) {
        return;
    }
    
    m_isOnAir = onAir;
    
    if (onAir) {
        m_statusLabel->setText(tr("ON AIR"));
        m_statusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #22c55e;");
        m_onAirButton->setEnabled(false);
        m_offAirButton->setEnabled(true);
    } else {
        m_statusLabel->setText(tr("OFF AIR"));
        m_statusLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: #ef4444;");
        m_onAirButton->setEnabled(true);
        m_offAirButton->setEnabled(false);
    }
    
    emit onAirStatusChanged(onAir);
}

void TalentMonitor::onGoOnAir()
{
    setOnAir(true);
    Logger::instance()->info(QString("Talent %1 went on air").arg(m_talentName));
}

void TalentMonitor::onGoOffAir()
{
    setOnAir(false);
    Logger::instance()->info(QString("Talent %1 went off air").arg(m_talentName));
}

void TalentMonitor::updateTime()
{
    QDateTime now = QDateTime::currentDateTime();
    m_timeLabel->setText(now.toString("hh:mm:ss"));
}

