#ifndef PLAYOUTCONTROLLER_H
#define PLAYOUTCONTROLLER_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QTimer>

class PlayoutController : public QWidget
{
    Q_OBJECT

public:
    explicit PlayoutController(QWidget *parent = nullptr);

private slots:
    void onPlay();
    void onPause();
    void onStop();
    void onUpdateTime();

private:
    QPushButton* m_playButton;
    QPushButton* m_pauseButton;
    QPushButton* m_stopButton;
    QLabel* m_timeLabel;
    QSlider* m_positionSlider;
    QTimer* m_updateTimer;
    bool m_isPlaying;
    double m_currentTime;
    double m_duration;
};

#endif // PLAYOUTCONTROLLER_H

