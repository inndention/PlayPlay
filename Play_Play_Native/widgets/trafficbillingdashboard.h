#ifndef TRAFFICBILLINGDASHBOARD_H
#define TRAFFICBILLINGDASHBOARD_H

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>

class TrafficEngine;

class TrafficBillingDashboard : public QWidget
{
    Q_OBJECT

public:
    explicit TrafficBillingDashboard(QWidget *parent = nullptr);

private slots:
    void refreshData();

private:
    TrafficEngine* m_trafficEngine;
    QTableWidget* m_clientsTable;
    QPushButton* m_refreshButton;
};

#endif // TRAFFICBILLINGDASHBOARD_H

