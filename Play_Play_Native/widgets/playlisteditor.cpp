#include "playlisteditor.h"
#include "Play_Play_Native/models/playlistmodel.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableView>
#include <QPushButton>
#include <QLineEdit>
#include <QMessageBox>
#include <QFileDialog>

PlaylistEditor::PlaylistEditor(QWidget *parent)
    : QWidget(parent)
    , m_model(new PlaylistModel(this))
    , m_tableView(new QTableView(this))
    , m_addButton(new QPushButton(tr("Add Item"), this))
    , m_removeButton(new QPushButton(tr("Remove Item"), this))
    , m_moveUpButton(new QPushButton(tr("Move Up"), this))
    , m_moveDownButton(new QPushButton(tr("Move Down"), this))
    , m_saveButton(new QPushButton(tr("Save"), this))
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Toolbar
    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    toolbarLayout->addWidget(m_addButton);
    toolbarLayout->addWidget(m_removeButton);
    toolbarLayout->addWidget(m_moveUpButton);
    toolbarLayout->addWidget(m_moveDownButton);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(m_saveButton);
    
    mainLayout->addLayout(toolbarLayout);
    mainLayout->addWidget(m_tableView);
    
    m_tableView->setModel(m_model);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    
    connect(m_addButton, &QPushButton::clicked, this, &PlaylistEditor::onAddItem);
    connect(m_removeButton, &QPushButton::clicked, this, &PlaylistEditor::onRemoveItem);
    connect(m_moveUpButton, &QPushButton::clicked, this, &PlaylistEditor::onMoveUp);
    connect(m_moveDownButton, &QPushButton::clicked, this, &PlaylistEditor::onMoveDown);
    connect(m_saveButton, &QPushButton::clicked, this, &PlaylistEditor::onSave);
}

void PlaylistEditor::onAddItem()
{
    // TODO: Open media selection dialog
    Logger::instance()->info("Add playlist item");
    QMessageBox::information(this, tr("Add Item"), tr("Media selection dialog to be implemented"));
}

void PlaylistEditor::onRemoveItem()
{
    QModelIndexList selected = m_tableView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("No Selection"), tr("Please select an item to remove"));
        return;
    }
    
    // TODO: Remove item from model
    Logger::instance()->info("Remove playlist item");
}

void PlaylistEditor::onMoveUp()
{
    QModelIndexList selected = m_tableView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return;
    }
    
    // TODO: Move item up
    Logger::instance()->info("Move playlist item up");
}

void PlaylistEditor::onMoveDown()
{
    QModelIndexList selected = m_tableView->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        return;
    }
    
    // TODO: Move item down
    Logger::instance()->info("Move playlist item down");
}

void PlaylistEditor::onSave()
{
    // TODO: Save playlist
    Logger::instance()->info("Save playlist");
    QMessageBox::information(this, tr("Save"), tr("Playlist saved"));
}

