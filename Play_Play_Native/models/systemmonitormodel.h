#ifndef SYSTEMMONITORMODEL_H
#define SYSTEMMONITORMODEL_H

#include <QObject>
#include <QTimer>

class SystemMonitorModel : public QObject
{
    Q_OBJECT
    Q_PROPERTY(double cpuUsage READ cpuUsage NOTIFY cpuUsageChanged)
    Q_PROPERTY(double ramUsage READ ramUsage NOTIFY ramUsageChanged)
    Q_PROPERTY(QString engineState READ engineState NOTIFY engineStateChanged)

public:
    explicit SystemMonitorModel(QObject* parent = nullptr);

    double cpuUsage() const { return m_cpuUsage; }
    double ramUsage() const { return m_ramUsage; }
    QString engineState() const { return m_engineState; }

signals:
    void cpuUsageChanged();
    void ramUsageChanged();
    void engineStateChanged();

private slots:
    void updateStats();

private:
    double m_cpuUsage;
    double m_ramUsage;
    QString m_engineState;
    QTimer* m_timer;
};

#endif // SYSTEMMONITORMODEL_H
