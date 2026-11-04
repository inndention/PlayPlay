#include "guestqueuemanager.h"
#include "services/guestqueueservice.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QMessageBox>

GuestQueueManager::GuestQueueManager(QWidget *parent)
    : QWidget(parent)
    , m_guestService(new GuestQueueService(this))
    , m_guestList(new QListWidget(this))
    , m_nameEdit(new QLineEdit(this))
    , m_addButton(new QPushButton(tr("Add Guest"), this))
    , m_removeButton(new QPushButton(tr("Remove Guest"), this))
    , m_moveUpButton(new QPushButton(tr("Move Up"), this))
    , m_moveDownButton(new QPushButton(tr("Move Down"), this))
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Input area
    QHBoxLayout* inputLayout = new QHBoxLayout();
    inputLayout->addWidget(new QLabel(tr("Guest Name:"), this));
    inputLayout->addWidget(m_nameEdit);
    inputLayout->addWidget(m_addButton);
    mainLayout->addLayout(inputLayout);
    
    // Guest list
    mainLayout->addWidget(m_guestList);
    
    // Control buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_removeButton);
    buttonLayout->addWidget(m_moveUpButton);
    buttonLayout->addWidget(m_moveDownButton);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);
    
    connect(m_addButton, &QPushButton::clicked, this, &GuestQueueManager::onAddGuest);
    connect(m_removeButton, &QPushButton::clicked, this, &GuestQueueManager::onRemoveGuest);
    connect(m_moveUpButton, &QPushButton::clicked, this, &GuestQueueManager::onMoveUp);
    connect(m_moveDownButton, &QPushButton::clicked, this, &GuestQueueManager::onMoveDown);
    connect(m_guestService, &GuestQueueService::queueChanged, this, &GuestQueueManager::updateList);
    
    updateList();
}

void GuestQueueManager::onAddGuest()
{
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid Input"), tr("Please enter a guest name"));
        return;
    }
    
    QString guestId = QString("guest_%1").arg(QDateTime::currentMSecsSinceEpoch());
    m_guestService->addGuest(guestId, name);
    m_nameEdit->clear();
    updateList();
}

void GuestQueueManager::onRemoveGuest()
{
    QListWidgetItem* item = m_guestList->currentItem();
    if (!item) {
        QMessageBox::warning(this, tr("No Selection"), tr("Please select a guest to remove"));
        return;
    }
    
    QString guestId = item->data(Qt::UserRole).toString();
    m_guestService->removeGuest(guestId);
    updateList();
}

void GuestQueueManager::onMoveUp()
{
    QListWidgetItem* item = m_guestList->currentItem();
    if (!item) {
        return;
    }
    
    int currentRow = m_guestList->currentRow();
    if (currentRow > 0) {
        QString guestId = item->data(Qt::UserRole).toString();
        m_guestService->moveGuest(guestId, currentRow - 1);
        updateList();
        m_guestList->setCurrentRow(currentRow - 1);
    }
}

void GuestQueueManager::onMoveDown()
{
    QListWidgetItem* item = m_guestList->currentItem();
    if (!item) {
        return;
    }
    
    int currentRow = m_guestList->currentRow();
    if (currentRow < m_guestList->count() - 1) {
        QString guestId = item->data(Qt::UserRole).toString();
        m_guestService->moveGuest(guestId, currentRow + 1);
        updateList();
        m_guestList->setCurrentRow(currentRow + 1);
    }
}

void GuestQueueManager::updateList()
{
    m_guestList->clear();
    
    QList<QJsonObject> queue = m_guestService->getQueue();
    for (const QJsonObject& guest : queue) {
        QListWidgetItem* item = new QListWidgetItem(guest["name"].toString(), m_guestList);
        item->setData(Qt::UserRole, guest["id"].toString());
        m_guestList->addItem(item);
    }
}

