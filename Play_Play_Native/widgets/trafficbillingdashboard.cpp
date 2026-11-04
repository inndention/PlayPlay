#include "trafficbillingdashboard.h"
#include "services/trafficengine.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QPushButton>
#include <QDate>

TrafficBillingDashboard::TrafficBillingDashboard(QWidget *parent)
    : QWidget(parent)
    , m_trafficEngine(new TrafficEngine(this))
    , m_clientsTable(new QTableWidget(this))
    , m_refreshButton(new QPushButton(tr("Refresh"), this))
{
    setWindowTitle(tr("Traffic & Billing Dashboard"));
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Title and controls
    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(tr("Traffic & Billing Dashboard"), this);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: white;");
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(m_refreshButton);
    mainLayout->addLayout(headerLayout);
    
    // Clients table
    m_clientsTable->setColumnCount(8);
    m_clientsTable->setHorizontalHeaderLabels({
        tr("Client Name"),
        tr("Status"),
        tr("Contract Start"),
        tr("Contract End"),
        tr("Budget"),
        tr("Rate/Play"),
        tr("Plays This Month"),
        tr("Revenue")
    });
    
    mainLayout->addWidget(m_clientsTable);
    
    connect(m_refreshButton, &QPushButton::clicked, this, &TrafficBillingDashboard::refreshData);
    connect(m_trafficEngine, &TrafficEngine::clientsChanged, this, &TrafficBillingDashboard::refreshData);
    
    refreshData();
}

void TrafficBillingDashboard::refreshData()
{
    m_clientsTable->setRowCount(0);
    
    QList<AdClient> clients = m_trafficEngine->getClients();
    
    for (const AdClient& client : clients) {
        int row = m_clientsTable->rowCount();
        m_clientsTable->insertRow(row);
        
        m_clientsTable->setItem(row, 0, new QTableWidgetItem(client.name));
        m_clientsTable->setItem(row, 1, new QTableWidgetItem(client.status));
        m_clientsTable->setItem(row, 2, new QTableWidgetItem(
            client.contractStart.isValid() ? client.contractStart.toString(Qt::ISODate) : ""));
        m_clientsTable->setItem(row, 3, new QTableWidgetItem(
            client.contractEnd.isValid() ? client.contractEnd.toString(Qt::ISODate) : ""));
        m_clientsTable->setItem(row, 4, new QTableWidgetItem(QString::number(client.budget, 'f', 2)));
        m_clientsTable->setItem(row, 5, new QTableWidgetItem(QString::number(client.ratePerPlay, 'f', 2)));
        m_clientsTable->setItem(row, 6, new QTableWidgetItem("0")); // TODO: Get actual play count
        m_clientsTable->setItem(row, 7, new QTableWidgetItem("0.00")); // TODO: Calculate revenue
    }
    
    m_clientsTable->resizeColumnsToContents();
    Logger::instance()->info("Traffic billing data refreshed");
}

