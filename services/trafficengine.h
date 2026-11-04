#ifndef TRAFFICENGINE_H
#define TRAFFICENGINE_H

#include <QObject>
#include <QList>
#include <QMap>
#include <QDateTime>
#include "core/types.h"

struct TrafficLogEntry {
    QString entryId;
    QString assetId;
    QString assetTitle;
    QString clientId;
    QString clientName;
    QString campaignId;
    QString campaignName;
    QDateTime scheduledTime;
    QDateTime actualTime;
    double duration;
    QString status; // "SCHEDULED", "PLAYED", "SKIPPED", "FAILED"
    double rate;    // Rate for this spot
    QJsonObject metadata;
    
    QJsonObject toJson() const;
    static TrafficLogEntry fromJson(const QJsonObject& json);
};

struct TrafficReport {
    QDateTime reportDate;
    QDateTime startTime;
    QDateTime endTime;
    QList<TrafficLogEntry> entries;
    double totalRevenue;
    int totalSpots;
    int spotsPlayed;
    int spotsSkipped;
    int spotsFailed;
    QMap<QString, double> revenueByClient; // clientId -> revenue
    QMap<QString, int> spotsByClient;      // clientId -> count
    
    QJsonObject toJson() const;
    static TrafficReport fromJson(const QJsonObject& json);
};

class TrafficEngine : public QObject
{
    Q_OBJECT

public:
    explicit TrafficEngine(QObject* parent = nullptr);
    
    // Client management
    void addClient(const AdClient& client);
    void updateClient(const AdClient& client);
    void removeClient(const QString& clientId);
    QList<AdClient> getClients() const;
    AdClient getClient(const QString& clientId) const;
    
    // Traffic logging
    void logTrafficEntry(const TrafficLogEntry& entry);
    QList<TrafficLogEntry> getTrafficLog(const QDateTime& start, const QDateTime& end) const;
    QList<TrafficLogEntry> getTrafficLogByClient(const QString& clientId, const QDateTime& start, const QDateTime& end) const;
    QList<TrafficLogEntry> getTrafficLogByCampaign(const QString& campaignId, const QDateTime& start, const QDateTime& end) const;
    
    // Reporting
    TrafficReport generateReport(const QDateTime& start, const QDateTime& end);
    TrafficReport generateReportForDate(const QDate& date);
    QList<TrafficReport> generateReportsForDateRange(const QDate& start, const QDate& end);
    
    // Revenue tracking
    double calculateRevenue(const QDateTime& start, const QDateTime& end) const;
    double calculateRevenueByClient(const QString& clientId, const QDateTime& start, const QDateTime& end) const;
    QMap<QString, double> getRevenueByClient(const QDateTime& start, const QDateTime& end) const;

    // Probabilistic Selection (NEW - Ported from TS)
    struct SelectionResult {
        MediaItem item;
        double probability;
        QString reason;
        double weight;
    };

    struct SelectionContext {
        qint64 now;
        QString daypart;
        int hour;
        double availableDuration;
        QList<TrafficLogEntry> recentPlays;
        QMap<QString, MediaStats> dayStats;
    };

    SelectionResult selectNextAd(const QList<MediaItem>& pool, const SelectionContext& context);
    double calculateWeight(const MediaItem& item, const SelectionContext& context, QString& outReason) const;
    QString getDaypart(int hour) const;

signals:
    void clientsChanged();
    void campaignUpdated(const QString& campaignId);
    void trafficEntryLogged(const TrafficLogEntry& entry);
    void reportGenerated(const TrafficReport& report);

private:
    void saveTrafficLog(const TrafficLogEntry& entry);
    double calculateEntryRevenue(const TrafficLogEntry& entry) const;
    
    QList<AdClient> m_clients;
    QList<TrafficLogEntry> m_trafficLog;
    QMap<QString, QList<TrafficLogEntry>> m_logByClient; // clientId -> entries
    QMap<QString, QList<TrafficLogEntry>> m_logByCampaign; // campaignId -> entries
    QMap<QString, MediaStats> m_mediaStats; // assetId -> stats
};

#endif // TRAFFICENGINE_H

