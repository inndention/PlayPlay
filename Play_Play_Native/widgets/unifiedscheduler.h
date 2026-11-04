#ifndef UNIFIEDSCHEDULER_H
#define UNIFIEDSCHEDULER_H

#include <QWidget>
#include <QTableView>
#include <QCalendarWidget>
#include <QComboBox>
#include <QPushButton>

class ScheduleModel;
class ScheduleEngine;

class UnifiedScheduler : public QWidget
{
    Q_OBJECT

public:
    explicit UnifiedScheduler(QWidget *parent = nullptr);

private slots:
    void onAddSlot();
    void onEditSlot();
    void onDeleteSlot();
    void onDateSelected();

private:
    ScheduleModel* m_model;
    ScheduleEngine* m_scheduleEngine;
    QTableView* m_tableView;
    QCalendarWidget* m_calendar;
    QComboBox* m_dayCombo;
    QComboBox* m_hourCombo;
    QPushButton* m_addButton;
    QPushButton* m_editButton;
    QPushButton* m_deleteButton;
};

#endif // UNIFIEDSCHEDULER_H

