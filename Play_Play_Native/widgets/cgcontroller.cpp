#include "cgcontroller.h"
#include "core/cgelement.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QMessageBox>

CGController::CGController(QWidget *parent)
    : QWidget(parent)
    , m_elementList(new QListWidget(this))
    , m_stackedWidget(new QStackedWidget(this))
    , m_addButton(new QPushButton(tr("Add Element"), this))
    , m_removeButton(new QPushButton(tr("Remove Element"), this))
    , m_showButton(new QPushButton(tr("Show"), this))
    , m_hideButton(new QPushButton(tr("Hide"), this))
{
    QHBoxLayout* mainLayout = new QHBoxLayout(this);
    
    // Left panel: Element list
    QVBoxLayout* leftLayout = new QVBoxLayout();
    leftLayout->addWidget(m_elementList);
    
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_removeButton);
    leftLayout->addLayout(buttonLayout);
    
    // Right panel: Element editor/preview
    QVBoxLayout* rightLayout = new QVBoxLayout();
    rightLayout->addWidget(m_stackedWidget);
    
    QHBoxLayout* controlLayout = new QHBoxLayout();
    controlLayout->addWidget(m_showButton);
    controlLayout->addWidget(m_hideButton);
    rightLayout->addLayout(controlLayout);
    
    mainLayout->addLayout(leftLayout, 1);
    mainLayout->addLayout(rightLayout, 2);
    
    connect(m_addButton, &QPushButton::clicked, this, &CGController::onAddElement);
    connect(m_removeButton, &QPushButton::clicked, this, &CGController::onRemoveElement);
    connect(m_showButton, &QPushButton::clicked, this, &CGController::onShow);
    connect(m_hideButton, &QPushButton::clicked, this, &CGController::onHide);
    connect(m_elementList, &QListWidget::itemSelectionChanged, this, &CGController::onElementSelected);
}

void CGController::onAddElement()
{
    // TODO: Open element creation dialog
    Logger::instance()->info("Add CG element");
    QMessageBox::information(this, tr("Add Element"), tr("CG element creation dialog to be implemented"));
}

void CGController::onRemoveElement()
{
    QListWidgetItem* item = m_elementList->currentItem();
    if (!item) {
        QMessageBox::warning(this, tr("No Selection"), tr("Please select an element to remove"));
        return;
    }
    
    // TODO: Remove element
    Logger::instance()->info("Remove CG element");
}

void CGController::onShow()
{
    QListWidgetItem* item = m_elementList->currentItem();
    if (!item) {
        return;
    }
    
    // TODO: Show CG element
    Logger::instance()->info("Show CG element");
}

void CGController::onHide()
{
    QListWidgetItem* item = m_elementList->currentItem();
    if (!item) {
        return;
    }
    
    // TODO: Hide CG element
    Logger::instance()->info("Hide CG element");
}

void CGController::onElementSelected()
{
    // TODO: Load element editor for selected element
}

