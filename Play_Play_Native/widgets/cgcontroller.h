#ifndef CGCONTROLLER_H
#define CGCONTROLLER_H

#include <QWidget>
#include <QListWidget>
#include <QStackedWidget>
#include <QPushButton>

class CGController : public QWidget
{
    Q_OBJECT

public:
    explicit CGController(QWidget *parent = nullptr);

private slots:
    void onAddElement();
    void onRemoveElement();
    void onShow();
    void onHide();
    void onElementSelected();

private:
    QListWidget* m_elementList;
    QStackedWidget* m_stackedWidget;
    QPushButton* m_addButton;
    QPushButton* m_removeButton;
    QPushButton* m_showButton;
    QPushButton* m_hideButton;
};

#endif // CGCONTROLLER_H

