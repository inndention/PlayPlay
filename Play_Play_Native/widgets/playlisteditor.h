#ifndef PLAYLISTEDITOR_H
#define PLAYLISTEDITOR_H

#include <QWidget>
#include <QTableView>
#include <QPushButton>

class PlaylistModel;

class PlaylistEditor : public QWidget
{
    Q_OBJECT

public:
    explicit PlaylistEditor(QWidget *parent = nullptr);

private slots:
    void onAddItem();
    void onRemoveItem();
    void onMoveUp();
    void onMoveDown();
    void onSave();

private:
    PlaylistModel* m_model;
    QTableView* m_tableView;
    QPushButton* m_addButton;
    QPushButton* m_removeButton;
    QPushButton* m_moveUpButton;
    QPushButton* m_moveDownButton;
    QPushButton* m_saveButton;
};

#endif // PLAYLISTEDITOR_H

