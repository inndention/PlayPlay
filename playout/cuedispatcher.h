#ifndef CUEDISPATCHER_H
#define CUEDISPATCHER_H

#include <QObject>
#include <QMap>
#include <QQueue>
#include <QTimer>
#include <QString>
#include <QDateTime>
#include <QJsonObject>

struct Cue {
    QString cueId;
    QString targetEngine;
    qint64 earliestTime;  // UTC milliseconds
    qint64 expiryTime;    // UTC milliseconds
    QString command;
    QJsonObject metadata;
    bool executed;
    qint64 executedAt;
};

class CueDispatcher : public QObject
{
    Q_OBJECT

public:
    explicit CueDispatcher(QObject* parent = nullptr);
    
    void queueCue(const QString& cueId, const QString& targetEngine,
                  qint64 earliestTime, qint64 expiryTime,
                  const QString& command, const QJsonObject& metadata = QJsonObject());
    void cancelCue(const QString& cueId);
    void processCues(qint64 currentTime);
    
    QList<Cue> pendingCues() const;
    QList<Cue> executedCues() const;

signals:
    void cueExecuted(const QString& cueId, const QString& engineId, const QString& command);
    void cueExpired(const QString& cueId);

private slots:
    void onProcessTimer();

private:
    QQueue<Cue> m_cueQueue;
    QMap<QString, Cue> m_executedCues;
    QTimer* m_processTimer;
    qint64 m_lastProcessTime;
    
    void executeCue(const Cue& cue);
};

#endif // CUEDISPATCHER_H

