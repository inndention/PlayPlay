#include "unifiedscheduler.h"
#include "Play_Play_Native/models/schedulemodel.h"
#include "services/scheduleengine.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableView>
#include <QPushButton>
#include <QComboBox>
#include <QCalendarWidget>
#include <QMessageBox>

UnifiedScheduler::UnifiedScheduler(QWidget *parent)
    : QWidget(parent)
    , m_model(new ScheduleModel(this))
    , m_scheduleEngine(new ScheduleEngine(this))
    , m_tableView(new QTableView(this))
    , m_calendar(new QCalendarWidget(this))
    , m_dayCombo(new QComboBox(this))
    , m_hourCombo(new QComboBox(this))
    , m_addButton(new QPushButton(tr("Add Slot"), this))
    , m_editButton(new QPushButton(tr("Edit Slot"), this))
    , m_deleteButton(new QPushButton(tr("Delete Slot"), this))
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    
    // Left panel: Calendar and controls
    QVBoxLayout* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(m_calendar);
    
    QHBoxLayout* comboLayout = new QHBoxLayout();
    comboLayout->addWidget(m_dayCombo);
    comboLayout->addWidget(m_hourCombo);
    leftLayout->addLayout(comboLayout);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_editButton);
    buttonLayout->addWidget(m_deleteButton);
    leftLayout->addLayout(buttonLayout);
    
    // Right panel: Schedule table
    QVBoxLayout* rightLayout = new QVBoxLayout();
    rightLayout->addWidget(m_tableView);
    
    mainLayout->addLayout(leftLayout, 1);
    mainLayout->addLayout(rightLayout, 2);
    
    // Setup day/hour combos
    QStringList days = {tr("Sunday"), tr("Monday"), tr("Tuesday"), tr("Wednesday"), 
                       tr("Thursday"), tr("Friday"), tr("Saturday")};
    m_dayCombo->addItems(days);
    
    for (int i = 0; i < 24; i++) {
        m_hourCombo->addItem(QString("%1:00").arg(i, 2, 10, QChar('0')));
    }
    
    m_tableView->setModel(m_model);
    
    connect(m_addButton, &QPushButton::clicked, this, &UnifiedScheduler::onAddSlot);
    connect(m_editButton, &QPushButton::clicked, this, &UnifiedScheduler::onEditSlot);
    connect(m_deleteButton, &QPushButton::clicked, this, &UnifiedScheduler::onDeleteSlot);
    connect(m_calendar, &QCalendarWidget::selectionChanged, this, &UnifiedScheduler::onDateSelected);
}

void UnifiedScheduler::onAddSlot()
{
    int day = m_dayCombo->currentIndex();
    int hour = m_hourCombo->currentIndex();
    
    // TODO: Open slot editor
    Logger::instance()->info(QString("Add slot: Day %1, Hour %2").arg(day).arg(hour));
    QMessageBox::information(this, tr("Add Slot"), tr("Slot editor to be implemented"));
}

void UnifiedScheduler::onEditSlot()
{
    QModelIndexList selected = m_tableView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("No Selection"), tr("Please select a slot to edit"));
        return;
    }
    
    // TODO: Open slot editor
    Logger::instance()->info("Edit slot");
}

void UnifiedScheduler::onDeleteSlot()
{
    QModelIndexList selected = m_tableView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("No Selection"), tr("Please select a slot to delete"));
        return;
    }
    
    // TODO: Delete slot
    Logger::instance()->info("Delete slot");
}

void UnifiedScheduler::onDateSelected()
{
    QDate date = m_calendar->selectedDate();
    // TODO: Load schedule for selected date
    Logger::instance()->info(QString("Date selected: %1").arg(date.toString(Qt::ISODate)));
}

