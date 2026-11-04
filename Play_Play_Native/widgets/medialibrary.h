#ifndef MEDIALIBRARY_H
#define MEDIALIBRARY_H

#include <QWidget>
#include <QListView>
#include <QLineEdit>
#include <QPushButton>
#include "Play_Play_Native/models/medialibrarymodel.h"

class MediaLibrary : public QWidget
{
    Q_OBJECT

public:
    explicit MediaLibrary(QWidget *parent = nullptr);

private slots:
    void onSearchTextChanged(const QString& text);
    void onAddMedia();
    void onDeleteMedia();
    void onMediaDoubleClicked(const QModelIndex& index);

private:
    QListView* m_listView;
    QLineEdit* m_searchEdit;
    QPushButton* m_addButton;
    QPushButton* m_deleteButton;
    MediaLibraryModel* m_model;
};

#endif // MEDIALIBRARY_H

