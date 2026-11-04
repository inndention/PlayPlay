#ifndef ANALYTICSDASHBOARD_H
#define ANALYTICSDASHBOARD_H

#include <QWidget>
#include <QTableWidget>

class AnalyticsDashboard : public QWidget
{
    Q_OBJECT

public:
    explicit AnalyticsDashboard(QWidget *parent = nullptr);
    
    void refreshData();

private:
    QTableWidget* m_statsTable;
};

#endif // ANALYTICSDASHBOARD_H

