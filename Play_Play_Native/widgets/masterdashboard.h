#ifndef MASTERDASHBOARD_H
#define MASTERDASHBOARD_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QProgressBar>
#include "playout/stateengine.h"

class ControlService;

class MasterDashboard : public QWidget
{
    Q_OBJECT

public:
    explicit MasterDashboard(QWidget *parent = nullptr);

private slots:
    void onPlay();
    void onPause();
    void onStop();
    void onStateTransition(const StateTransition& transition);
    void updateDisplay();

private:
    ControlService* m_controlService;
    QLabel* m_statusLabel;
    QLabel* m_timeLabel;
    QLabel* m_engineStatusLabel;
    QLabel* m_currentItemLabel;
    QLabel* m_nextItemLabel;
    QLabel* m_scheduleStatusLabel;
    QLabel* m_timeRemainingLabel;
    QPushButton* m_playButton;
    QPushButton* m_pauseButton;
    QPushButton* m_stopButton;
    QProgressBar* m_progressBar;
    QTimer* m_timer;
};

#endif // MASTERDASHBOARD_H

