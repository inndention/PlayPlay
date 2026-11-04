#ifndef GUESTQUEUEMANAGER_H
#define GUESTQUEUEMANAGER_H

#include <QWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>

class GuestQueueService;

class GuestQueueManager : public QWidget
{
    Q_OBJECT

public:
    explicit GuestQueueManager(QWidget *parent = nullptr);

private slots:
    void onAddGuest();
    void onRemoveGuest();
    void onMoveUp();
    void onMoveDown();
    void updateList();

private:
    GuestQueueService* m_guestService;
    QListWidget* m_guestList;
    QLineEdit* m_nameEdit;
    QPushButton* m_addButton;
    QPushButton* m_removeButton;
    QPushButton* m_moveUpButton;
    QPushButton* m_moveDownButton;
};

#endif // GUESTQUEUEMANAGER_H

