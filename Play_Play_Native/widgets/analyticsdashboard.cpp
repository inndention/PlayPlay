#include "analyticsdashboard.h"
#include "services/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QDateTime>
#include <QTableWidgetItem>

AnalyticsDashboard::AnalyticsDashboard(QWidget *parent)
    : QWidget(parent)
    , m_statsTable(new QTableWidget(this))
{
    setWindowTitle(tr("Analytics Dashboard"));
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Title
    QLabel* titleLabel = new QLabel(tr("Analytics Dashboard"), this);
    titleLabel->setStyleSheet("font-size: 24px; font-weight: bold; color: white;");
    mainLayout->addWidget(titleLabel);
    
    // Stats table
    m_statsTable->setColumnCount(4);
    m_statsTable->setHorizontalHeaderLabels({tr("Metric"), tr("Today"), tr("This Week"), tr("This Month")});
    m_statsTable->setRowCount(10);
    
    // Populate with sample data
    QStringList metrics = {
        tr("Media Items Played"),
        tr("Total Play Time"),
        tr("Ads Played"),
        tr("Programs Played"),
        tr("Station IDs Played"),
        tr("Bumpers Played"),
        tr("Schedule Compliance"),
        tr("Late Starts"),
        tr("Overruns"),
        tr("Errors")
    };
    
    for (int i = 0; i < metrics.size(); ++i) {
        m_statsTable->setItem(i, 0, new QTableWidgetItem(metrics[i]));
        m_statsTable->setItem(i, 1, new QTableWidgetItem("0"));
        m_statsTable->setItem(i, 2, new QTableWidgetItem("0"));
        m_statsTable->setItem(i, 3, new QTableWidgetItem("0"));
    }
    
    m_statsTable->resizeColumnsToContents();
    mainLayout->addWidget(m_statsTable);
    
    // TODO: Connect to actual analytics service
    Logger::instance()->info("Analytics dashboard initialized");
}

void AnalyticsDashboard::refreshData()
{
    // TODO: Refresh analytics data from service
    Logger::instance()->info("Refreshing analytics data");
}

