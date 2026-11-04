#include "trafficengine.h"
#include <QRandomGenerator>
#include <QtMath>
#include "services/logger.h"
#include "core/types.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <algorithm>

TrafficEngine::TrafficEngine(QObject* parent)
    : QObject(parent)
{
    Logger::instance()->info("TrafficEngine initialized");
}

void TrafficEngine::addClient(const AdClient& client)
{
    // Remove existing client if present
    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].id == client.id) {
            m_clients[i] = client;
            emit clientsChanged();
            Logger::instance()->info(QString("Client updated: %1").arg(client.name));
            return;
        }
    }
    
    m_clients.append(client);
    emit clientsChanged();
    Logger::instance()->info(QString("Client added: %1").arg(client.name));
}

void TrafficEngine::updateClient(const AdClient& client)
{
    addClient(client); // addClient handles updates
}

void TrafficEngine::removeClient(const QString& clientId)
{
    for (int i = 0; i < m_clients.size(); ++i) {
        if (m_clients[i].id == clientId) {
            m_clients.removeAt(i);
            emit clientsChanged();
            Logger::instance()->info(QString("Client removed: %1").arg(clientId));
            return;
        }
    }
}

QList<AdClient> TrafficEngine::getClients() const
{
    return m_clients;
}

AdClient TrafficEngine::getClient(const QString& clientId) const
{
    for (const AdClient& client : m_clients) {
        if (client.id == clientId) {
            return client;
        }
    }
    return AdClient();
}

void TrafficEngine::logTrafficEntry(const TrafficLogEntry& entry)
{
    m_trafficLog.append(entry);
    
    // Index by client
    if (!m_logByClient.contains(entry.clientId)) {
        m_logByClient[entry.clientId] = QList<TrafficLogEntry>();
    }
    m_logByClient[entry.clientId].append(entry);
    
    // Index by campaign
    if (!entry.campaignId.isEmpty()) {
        if (!m_logByCampaign.contains(entry.campaignId)) {
            m_logByCampaign[entry.campaignId] = QList<TrafficLogEntry>();
        }
        m_logByCampaign[entry.campaignId].append(entry);
    }
    
    saveTrafficLog(entry);
    emit trafficEntryLogged(entry);
    Logger::instance()->info(QString("Traffic entry logged: %1 - %2").arg(entry.assetTitle, entry.status));
}

QList<TrafficLogEntry> TrafficEngine::getTrafficLog(const QDateTime& start, const QDateTime& end) const
{
    QList<TrafficLogEntry> result;
    qint64 startMs = start.toMSecsSinceEpoch();
    qint64 endMs = end.toMSecsSinceEpoch();
    
    for (const TrafficLogEntry& entry : m_trafficLog) {
        qint64 entryMs = entry.scheduledTime.toMSecsSinceEpoch();
        if (entryMs >= startMs && entryMs <= endMs) {
            result.append(entry);
        }
    }
    
    return result;
}

QList<TrafficLogEntry> TrafficEngine::getTrafficLogByClient(const QString& clientId, const QDateTime& start, const QDateTime& end) const
{
    QList<TrafficLogEntry> result;
    qint64 startMs = start.toMSecsSinceEpoch();
    qint64 endMs = end.toMSecsSinceEpoch();
    
    if (m_logByClient.contains(clientId)) {
        for (const TrafficLogEntry& entry : m_logByClient[clientId]) {
            qint64 entryMs = entry.scheduledTime.toMSecsSinceEpoch();
            if (entryMs >= startMs && entryMs <= endMs) {
                result.append(entry);
            }
        }
    }
    
    return result;
}

QList<TrafficLogEntry> TrafficEngine::getTrafficLogByCampaign(const QString& campaignId, const QDateTime& start, const QDateTime& end) const
{
    QList<TrafficLogEntry> result;
    qint64 startMs = start.toMSecsSinceEpoch();
    qint64 endMs = end.toMSecsSinceEpoch();
    
    if (m_logByCampaign.contains(campaignId)) {
        for (const TrafficLogEntry& entry : m_logByCampaign[campaignId]) {
            qint64 entryMs = entry.scheduledTime.toMSecsSinceEpoch();
            if (entryMs >= startMs && entryMs <= endMs) {
                result.append(entry);
            }
        }
    }
    
    return result;
}

TrafficReport TrafficEngine::generateReport(const QDateTime& start, const QDateTime& end)
{
    TrafficReport report;
    report.reportDate = QDateTime::currentDateTime();
    report.startTime = start;
    report.endTime = end;
    report.entries = getTrafficLog(start, end);
    
    report.totalSpots = report.entries.size();
    report.totalRevenue = 0.0;
    report.spotsPlayed = 0;
    report.spotsSkipped = 0;
    report.spotsFailed = 0;
    
    for (const TrafficLogEntry& entry : report.entries) {
        double revenue = calculateEntryRevenue(entry);
        report.totalRevenue += revenue;
        
        if (entry.status == "PLAYED") {
            report.spotsPlayed++;
        } else if (entry.status == "SKIPPED") {
            report.spotsSkipped++;
        } else if (entry.status == "FAILED") {
            report.spotsFailed++;
        }
        
        // Revenue by client
        if (!report.revenueByClient.contains(entry.clientId)) {
            report.revenueByClient[entry.clientId] = 0.0;
            report.spotsByClient[entry.clientId] = 0;
        }
        report.revenueByClient[entry.clientId] += revenue;
        report.spotsByClient[entry.clientId]++;
    }
    
    emit reportGenerated(report);
    return report;
}

TrafficReport TrafficEngine::generateReportForDate(const QDate& date)
{
    QDateTime start(date, QTime(0, 0, 0));
    QDateTime end(date, QTime(23, 59, 59));
    return generateReport(start, end);
}

QList<TrafficReport> TrafficEngine::generateReportsForDateRange(const QDate& start, const QDate& end)
{
    QList<TrafficReport> reports;
    QDate current = start;
    
    while (current <= end) {
        reports.append(generateReportForDate(current));
        current = current.addDays(1);
    }
    
    return reports;
}

double TrafficEngine::calculateRevenue(const QDateTime& start, const QDateTime& end) const
{
    double total = 0.0;
    QList<TrafficLogEntry> entries = getTrafficLog(start, end);
    
    for (const TrafficLogEntry& entry : entries) {
        total += calculateEntryRevenue(entry);
    }
    
    return total;
}

double TrafficEngine::calculateRevenueByClient(const QString& clientId, const QDateTime& start, const QDateTime& end) const
{
    double total = 0.0;
    QList<TrafficLogEntry> entries = getTrafficLogByClient(clientId, start, end);
    
    for (const TrafficLogEntry& entry : entries) {
        total += calculateEntryRevenue(entry);
    }
    
    return total;
}

QMap<QString, double> TrafficEngine::getRevenueByClient(const QDateTime& start, const QDateTime& end) const
{
    QMap<QString, double> revenue;
    QList<TrafficLogEntry> entries = getTrafficLog(start, end);
    
    for (const TrafficLogEntry& entry : entries) {
        if (!revenue.contains(entry.clientId)) {
            revenue[entry.clientId] = 0.0;
        }
        revenue[entry.clientId] += calculateEntryRevenue(entry);
    }
    
    return revenue;
}

void TrafficEngine::saveTrafficLog(const TrafficLogEntry& entry)
{
    // TODO: Persist to database or file
    // For now, just keep in memory
    Q_UNUSED(entry)
}

double TrafficEngine::calculateEntryRevenue(const TrafficLogEntry& entry) const
{
    if (entry.status == "PLAYED") {
        return entry.rate * entry.duration;
    }
    return 0.0;
}

QJsonObject TrafficLogEntry::toJson() const
{
    QJsonObject obj;
    obj["entryId"] = entryId;
    obj["assetId"] = assetId;
    obj["assetTitle"] = assetTitle;
    obj["clientId"] = clientId;
    obj["clientName"] = clientName;
    obj["campaignId"] = campaignId;
    obj["campaignName"] = campaignName;
    obj["scheduledTime"] = scheduledTime.toString(Qt::ISODate);
    obj["actualTime"] = actualTime.toString(Qt::ISODate);
    obj["duration"] = duration;
    obj["status"] = status;
    obj["rate"] = rate;
    obj["metadata"] = metadata;
    return obj;
}

TrafficLogEntry TrafficLogEntry::fromJson(const QJsonObject& json)
{
    TrafficLogEntry entry;
    entry.entryId = json["entryId"].toString();
    entry.assetId = json["assetId"].toString();
    entry.assetTitle = json["assetTitle"].toString();
    entry.clientId = json["clientId"].toString();
    entry.clientName = json["clientName"].toString();
    entry.campaignId = json["campaignId"].toString();
    entry.campaignName = json["campaignName"].toString();
    entry.scheduledTime = QDateTime::fromString(json["scheduledTime"].toString(), Qt::ISODate);
    entry.actualTime = QDateTime::fromString(json["actualTime"].toString(), Qt::ISODate);
    entry.duration = json["duration"].toDouble();
    entry.status = json["status"].toString();
    entry.rate = json["rate"].toDouble();
    entry.metadata = json["metadata"].toObject();
    return entry;
}

QJsonObject TrafficReport::toJson() const
{
    QJsonObject obj;
    obj["reportDate"] = reportDate.toString(Qt::ISODate);
    obj["startTime"] = startTime.toString(Qt::ISODate);
    obj["endTime"] = endTime.toString(Qt::ISODate);
    obj["totalRevenue"] = totalRevenue;
    obj["totalSpots"] = totalSpots;
    obj["spotsPlayed"] = spotsPlayed;
    obj["spotsSkipped"] = spotsSkipped;
    obj["spotsFailed"] = spotsFailed;
    
    QJsonArray entriesArray;
    for (const TrafficLogEntry& entry : entries) {
        entriesArray.append(entry.toJson());
    }
    obj["entries"] = entriesArray;
    
    QJsonObject revenueByClientObj;
    for (auto it = revenueByClient.begin(); it != revenueByClient.end(); ++it) {
        revenueByClientObj[it.key()] = it.value();
    }
    obj["revenueByClient"] = revenueByClientObj;
    
    QJsonObject spotsByClientObj;
    for (auto it = spotsByClient.begin(); it != spotsByClient.end(); ++it) {
        spotsByClientObj[it.key()] = it.value();
    }
    obj["spotsByClient"] = spotsByClientObj;
    
    return obj;
}

TrafficReport TrafficReport::fromJson(const QJsonObject& json)
{
    TrafficReport report;
    report.reportDate = QDateTime::fromString(json["reportDate"].toString(), Qt::ISODate);
    report.startTime = QDateTime::fromString(json["startTime"].toString(), Qt::ISODate);
    report.endTime = QDateTime::fromString(json["endTime"].toString(), Qt::ISODate);
    report.totalRevenue = json["totalRevenue"].toDouble();
    report.totalSpots = json["totalSpots"].toInt();
    report.spotsPlayed = json["spotsPlayed"].toInt();
    report.spotsSkipped = json["spotsSkipped"].toInt();
    report.spotsFailed = json["spotsFailed"].toInt();
    
    QJsonArray entriesArray = json["entries"].toArray();
    for (const QJsonValue& value : entriesArray) {
        report.entries.append(TrafficLogEntry::fromJson(value.toObject()));
    }
    
    QJsonObject revenueByClientObj = json["revenueByClient"].toObject();
    for (auto it = revenueByClientObj.begin(); it != revenueByClientObj.end(); ++it) {
        report.revenueByClient[it.key()] = it.value().toDouble();
    }
    
    QJsonObject spotsByClientObj = json["spotsByClient"].toObject();
    for (auto it = spotsByClientObj.begin(); it != spotsByClientObj.end(); ++it) {
        report.spotsByClient[it.key()] = it.value().toInt();
    }
    
    return report;
}

TrafficEngine::SelectionResult TrafficEngine::selectNextAd(const QList<MediaItem>& pool, const SelectionContext& context)
{
    QList<QPair<MediaItem, double>> candidates;
    double totalWeight = 0.0;
    
    for (const auto& item : pool) {
        QString reason;
        double weight = calculateWeight(item, context, reason);
        if (weight > 0) {
            candidates.append({item, weight});
            totalWeight += weight;
        }
    }
    
    if (candidates.isEmpty() || totalWeight <= 0) {
        return {MediaItem(), 0.0, "No eligible candidates", 0.0};
    }
    
    // Weighted random selection
    double r = QRandomGenerator::global()->generateDouble() * totalWeight;
    for (const auto& candidate : candidates) {
        r -= candidate.second;
        if (r <= 0) {
            return {candidate.first, candidate.second / totalWeight, "Selected", candidate.second};
        }
    }
    
    return {candidates.last().first, candidates.last().second / totalWeight, "Selected (fallback)", candidates.last().second};
}

double TrafficEngine::calculateWeight(const MediaItem& item, const SelectionContext& context, QString& outReason) const
{
    // C++ version of TS calculateWeight logic
    if (item.duration > context.availableDuration) {
        outReason = "Too long";
        return 0;
    }
    
    // Separation check
    for (const auto& log : context.recentPlays) {
        if (log.assetId == item.id) {
            qint64 elapsed = (context.now - log.actualTime.toMSecsSinceEpoch()) / 1000;
            if (elapsed < 3600) { // Default 1 hour separation for now
                outReason = "Separation violation";
                return 0;
            }
        }
    }
    
    double weight = (double)item.priority + 50.0; // Base weight around priority
    
    // Delivery boost (simplified)
    if (context.dayStats.contains(item.id)) {
        const auto& stats = context.dayStats[item.id];
        if (stats.playCount < 5) weight *= 2.0; // Boost new/underplayed
    } else {
        weight *= 3.0; // New items get high weight to start
    }
    
    outReason = "Eligible";
    return qMax(0.0, weight);
}

QString TrafficEngine::getDaypart(int hour) const
{
    if (hour >= 6 && hour < 9) return "early_morning";
    if (hour >= 9 && hour < 12) return "morning";
    if (hour >= 12 && hour < 15) return "midday";
    if (hour >= 15 && hour < 18) return "afternoon";
    if (hour >= 18 && hour < 23) return "primetime";
    return "night";
}
