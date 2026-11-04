#ifndef SCHEDULETICKER_H
#define SCHEDULETICKER_H

#include <QWidget>
#include <QLabel>
#include <QTimer>
#include <QPropertyAnimation>
#include "core/types.h"

class ScheduleEngine;

class ScheduleTicker : public QWidget
{
    Q_OBJECT

public:
    explicit ScheduleTicker(QWidget *parent = nullptr);

private slots:
    void updateSchedule();

private:
    void startScrollAnimation();
    void stopScrollAnimation();
    
    QLabel* m_label;
    QTimer* m_timer;
    QPropertyAnimation* m_scrollAnimation;
    ScheduleEngine* m_scheduleEngine;
};

#endif // SCHEDULETICKER_H

