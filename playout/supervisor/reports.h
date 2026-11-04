#ifndef REPORTS_H
#define REPORTS_H
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QByteArray>
#include <QStringList>
#include <QDate>
#include <QTimeZone>

namespace sched { class ScheduleStore; struct ChannelProfile; }

/**
 * ReportBuilder — branded HTML / CSV / JSON reports over the as-run log, the media library, the
 * effective schedule and the traffic contracts (port of PlayPlay OBS server/reports.py).
 *
 * Every report is a pure function over the ScheduleStore: `run(name, q)` builds a QJsonObject of
 * data (what `/api/v1/reports/<name>` returns), `html(name, data)` renders that data into the shared
 * print-ready shell and `csv(name, data)` flattens its main table (UTF-8 BOM, CRLF, Excel-safe).
 * Nothing here touches the executor; it is safe to call from the REST thread at any time, including
 * against an empty database (every section degrades to a "no data" line, never a crash). It runs on the
 * Supervisor's event-loop thread (the ScheduleStore connection lives there), so every as-run scan is bounded
 * (row cap + wall-clock budget, `truncated: true` when hit) and goes channel by channel through the
 * (channel_id, actual_start) index; media are looked up by id on demand, never loaded wholesale.
 *
 * Reports: asplayed · asset · assets · full · adherence · contracts · top · compare (see docs/reports.md).
 * Settings read: `station.brand` {name, tagline, logoUrl, footer, accent}; `traffic.contracts` {contracts:[...]}.
 */
class ReportBuilder : public QObject {
    Q_OBJECT
public:
    using Q = QHash<QString, QString>;
    explicit ReportBuilder(sched::ScheduleStore* store, QObject* parent = nullptr);

    /// Report names understood by run()/html()/csv().
    static QStringList names();
    /// Dispatch by name. Returns {error, status} on a bad request (unknown name, unknown asset…).
    QJsonObject run(const QString& name, const Q& q) const;
    QByteArray html(const QString& name, const QJsonObject& data) const;
    QByteArray csv(const QString& name, const QJsonObject& data) const;
    /// Landing page: one form per report.
    QByteArray indexHtml() const;
    /// JSON catalogue of the reports (name, title, params, html/csv URL templates).
    QJsonObject catalogue() const;

    // --- individual reports (q = query-string parameters, see docs/reports.md) -------------------
    QJsonObject asPlayed(const Q& q) const;      // channel, date | from,to, type, origin, q, limit
    QJsonObject asset(const Q& q) const;         // id, days, channel
    QJsonObject assets(const Q& q) const;        // ids | q,type,category,client,channel ; days, limit
    QJsonObject full(const Q& q) const;          // period, date | from,to, group, channel
    QJsonObject adherence(const Q& q) const;     // channel, date, toleranceSec
    QJsonObject contracts(const Q& q) const;     // id, date (as-of)
    QJsonObject top(const Q& q) const;           // days, n, type, channel
    QJsonObject compare(const Q& q) const;       // ids, days, channel

    /// Branding block merged with defaults (`station.brand`).
    QJsonObject brand() const;

private:
    sched::ScheduleStore* m_store;

    // --- shared helpers --------------------------------------------------------------------------
    QTimeZone tzFor(const QString& channelId) const;
    QString defaultChannel() const;
    QString channelLabel(const QString& channelId) const;
    QJsonArray channelsJson() const;
    /// Period/date-range resolution shared by asplayed/full: fills from/to (inclusive local dates) + label.
    void resolveRange(const Q& q, const QTimeZone& tz, QDate* from, QDate* to, QString* label, QString* period) const;
    /// Wrap a body in the branded, print-ready document.
    QByteArray shell(const QString& title, const QString& subtitle, const QString& body, const QString& kicker = QString()) const;

    // --- renderers -------------------------------------------------------------------------------
    QString htmlAsPlayed(const QJsonObject& d) const;
    QString htmlAsset(const QJsonObject& d) const;
    QString htmlAssets(const QJsonObject& d) const;
    QString htmlFull(const QJsonObject& d) const;
    QString htmlAdherence(const QJsonObject& d) const;
    QString htmlContracts(const QJsonObject& d) const;
    QString htmlTop(const QJsonObject& d) const;
    QString htmlCompare(const QJsonObject& d) const;
};
#endif
