#include "scheduleticker.h"
#include "services/scheduleengine.h"
#include "services/logger.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>

ScheduleTicker::ScheduleTicker(QWidget *parent)
    : QWidget(parent)
    , m_label(new QLabel(this))
    , m_timer(new QTimer(this))
    , m_scrollAnimation(new QPropertyAnimation(this))
    , m_scheduleEngine(new ScheduleEngine(this))
{
    setStyleSheet(
        "QWidget { background-color: #0f172a; }"
        "QLabel { color: white; font-size: 18px; font-weight: bold; }"
    );
    
    setFixedHeight(60);
    
    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->setContentsMargins(20, 0, 20, 0);
    layout->addWidget(m_label);
    
    m_label->setText(tr("No schedule"));
    
    // Setup scroll animation
    m_scrollAnimation->setTargetObject(m_label);
    m_scrollAnimation->setPropertyName("pos");
    m_scrollAnimation->setDuration(20000); // 20 seconds for full scroll
    m_scrollAnimation->setLoopCount(-1); // Infinite loop
    
    // Update timer - check for schedule changes every second
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &ScheduleTicker::updateSchedule);
    m_timer->start();
    
    connect(m_scheduleEngine, &ScheduleEngine::scheduleChanged, this, &ScheduleTicker::updateSchedule);
    
    updateSchedule();
}

void ScheduleTicker::updateSchedule()
{
    QDateTime now = QDateTime::currentDateTime();
    int dayOfWeek = now.date().dayOfWeek() - 1; // Qt uses 1-7, we need 0-6
    int hour = now.time().hour();
    
    QList<PlaylistItem> items = m_scheduleEngine->getItemsForSlot(dayOfWeek, hour);
    
    if (items.isEmpty()) {
        m_label->setText(tr("No schedule for this hour"));
        return;
    }
    
    // Find current item based on time
    PlaylistItem* currentItem = nullptr;
    QTime currentTime = now.time();
    
    for (auto& item : items) {
        if (item.startTime.isValid() && item.endTime.isValid()) {
            QTime itemStart = item.startTime.time();
            QTime itemEnd = item.endTime.time();
            
            if (currentTime >= itemStart && currentTime <= itemEnd) {
                currentItem = &item;
                break;
            }
        }
    }
    
    if (currentItem) {
        QString text = QString("%1 - %2")
            .arg(currentItem->media.title)
            .arg(currentTime.toString("hh:mm:ss"));
        m_label->setText(text);
        
        // Start scroll animation if text is too long
        if (m_label->fontMetrics().boundingRect(text).width() > width() - 40) {
            startScrollAnimation();
        } else {
            stopScrollAnimation();
        }
    } else {
        // Show next upcoming item
        PlaylistItem* nextItem = nullptr;
        for (auto& item : items) {
            if (item.startTime.isValid() && item.startTime.time() > currentTime) {
                if (!nextItem || item.startTime < nextItem->startTime) {
                    nextItem = &item;
                }
            }
        }
        
        if (nextItem) {
            QString text = QString("Up next: %1 at %2")
                .arg(nextItem->media.title)
                .arg(nextItem->startTime.time().toString("hh:mm"));
            m_label->setText(text);
        } else {
            m_label->setText(tr("No upcoming items"));
        }
        
        stopScrollAnimation();
    }
}

void ScheduleTicker::startScrollAnimation()
{
    if (m_scrollAnimation->state() == QAbstractAnimation::Running) {
        return;
    }
    
    int textWidth = m_label->fontMetrics().boundingRect(m_label->text()).width();
    int widgetWidth = width();
    
    if (textWidth <= widgetWidth - 40) {
        return; // No need to scroll
    }
    
    m_scrollAnimation->setStartValue(QPoint(20, m_label->y()));
    m_scrollAnimation->setEndValue(QPoint(widgetWidth - textWidth - 20, m_label->y()));
    m_scrollAnimation->start();
}

void ScheduleTicker::stopScrollAnimation()
{
    if (m_scrollAnimation->state() == QAbstractAnimation::Running) {
        m_scrollAnimation->stop();
    }
    m_label->move(20, m_label->y());
}

