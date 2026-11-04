#ifndef TALENTMONITOR_H
#define TALENTMONITOR_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

class TalentMonitor : public QWidget
{
    Q_OBJECT

public:
    explicit TalentMonitor(QWidget *parent = nullptr);
    
    void setTalentName(const QString& name);
    void setShowName(const QString& showName);
    void setOnAir(bool onAir);
    bool isOnAir() const { return m_isOnAir; }

signals:
    void talentNameChanged(const QString& name);
    void showNameChanged(const QString& showName);
    void onAirStatusChanged(bool onAir);

private slots:
    void onGoOnAir();
    void onGoOffAir();
    void updateTime();

private:
    QLabel* m_nameLabel;
    QLabel* m_statusLabel;
    QLabel* m_showLabel;
    QLabel* m_timeLabel;
    QPushButton* m_onAirButton;
    QPushButton* m_offAirButton;
    QTimer* m_timer;
    
    bool m_isOnAir;
    QString m_talentName;
    QString m_showName;
};

#endif // TALENTMONITOR_H

