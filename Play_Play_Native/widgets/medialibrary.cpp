#include "medialibrary.h"
#include "Play_Play_Native/models/medialibrarymodel.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QFileDialog>

MediaLibrary::MediaLibrary(QWidget *parent)
    : QWidget(parent)
    , m_listView(new QListView(this))
    , m_searchEdit(new QLineEdit(this))
    , m_addButton(new QPushButton(tr("Add Media"), this))
    , m_deleteButton(new QPushButton(tr("Delete"), this))
    , m_model(new MediaLibraryModel(this))
{
    m_listView->setModel(m_model);
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    QHBoxLayout* searchLayout = new QHBoxLayout();
    searchLayout->addWidget(m_searchEdit);
    searchLayout->addWidget(m_addButton);
    searchLayout->addWidget(m_deleteButton);
    
    layout->addLayout(searchLayout);
    layout->addWidget(m_listView);
    
    connect(m_searchEdit, &QLineEdit::textChanged, this, &MediaLibrary::onSearchTextChanged);
    connect(m_addButton, &QPushButton::clicked, this, &MediaLibrary::onAddMedia);
    connect(m_deleteButton, &QPushButton::clicked, this, &MediaLibrary::onDeleteMedia);
    connect(m_listView, &QListView::doubleClicked, this, &MediaLibrary::onMediaDoubleClicked);
}

void MediaLibrary::onSearchTextChanged(const QString& text)
{
    // TODO: Implement search filtering
}

void MediaLibrary::onAddMedia()
{
    // TODO: Implement add media dialog
    QMessageBox::information(this, tr("Add Media"), tr("Add media functionality to be implemented"));
}

void MediaLibrary::onDeleteMedia()
{
    // TODO: Implement delete media
    QMessageBox::information(this, tr("Delete Media"), tr("Delete media functionality to be implemented"));
}

void MediaLibrary::onMediaDoubleClicked(const QModelIndex& index)
{
    // TODO: Implement media preview/editing
}

