#ifndef ASRUNLOGGER_H
#define ASRUNLOGGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QJsonObject>
#include <QList>
#include <QDateTime>

struct AsRunEntry {
    QString eventId;
    qint64 plannedStart;      // UTC milliseconds
    qint64 actualStart;       // UTC milliseconds
    qint64 plannedEnd;        // UTC milliseconds
    qint64 actualEnd;         // UTC milliseconds
    qint64 plannedDuration;   // milliseconds
    qint64 actualDuration;    // milliseconds
    QString engineId;
    QString assetId;
    QString mediaTitle;
    QString status;           // 'ON_TIME' | 'LATE_START' | 'OVERRUN' | 'DESYNC' | 'FAILED' | 'CANCELLED' | 'EARLY_START' | 'UNDERRUN'
    QString reason;
    int frameOffset;          // Frame-accurate offset from planned start
    double timingAccuracy;    // Timing accuracy in milliseconds
    QJsonObject metadata;
    qint64 timestamp;
    
    // Frame-accurate timing
    int plannedStartFrame;    // Frame number at planned start
    int actualStartFrame;     // Frame number at actual start
    int frameRate;            // Frames per second
    
    QJsonObject toJson() const;
    static AsRunEntry fromJson(const QJsonObject& json);
};

class AsRunLogger : public QObject
{
    Q_OBJECT

public:
    explicit AsRunLogger(QObject* parent = nullptr);
    ~AsRunLogger();
    
    bool initialize();
    void close();
    
    void logEvent(const QString& eventId, qint64 plannedStart, qint64 actualStart,
                  const QString& engineId, const QString& assetId,
                  const QString& status, const QString& reason,
                  const QJsonObject& metadata = QJsonObject());
    
    // Frame-accurate logging
    void logEventFrameAccurate(const QString& eventId, qint64 plannedStart, qint64 actualStart,
                               qint64 plannedEnd, qint64 actualEnd,
                               int plannedStartFrame, int actualStartFrame,
                               int frameRate, const QString& engineId, const QString& assetId,
                               const QString& mediaTitle, const QString& status,
                               const QString& reason, const QJsonObject& metadata = QJsonObject());
    
    QList<AsRunEntry> queryLog(const QJsonObject& filters = QJsonObject()) const;
    QList<AsRunEntry> queryLogByDateRange(const QDateTime& start, const QDateTime& end) const;
    QList<AsRunEntry> queryLogByAsset(const QString& assetId) const;
    QList<AsRunEntry> queryLogByStatus(const QString& status) const;
    
    // Detailed reporting
    QList<QJsonObject> generateDiscrepancyReport(qint64 startTime, qint64 endTime);
    QList<QJsonObject> generateTimingReport(qint64 startTime, qint64 endTime);
    QList<QJsonObject> generateFrameAccuracyReport(qint64 startTime, qint64 endTime);
    QJsonObject generateSummaryReport(qint64 startTime, qint64 endTime);

private:
    QSqlDatabase m_db;
    bool m_initialized;
    bool ensureTables();
};

AsRunLogger* getAsRunLogger();

#endif // ASRUNLOGGER_H

