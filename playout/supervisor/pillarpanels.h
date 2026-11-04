#ifndef PILLARPANELS_H
#define PILLARPANELS_H

#include <QObject>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QTimeZone>
#include <QTimer>

namespace sched { class ScheduleStore; class ScheduleExecutor; }

/**
 * PillarPanels - information panels in the side bars when a narrow picture is on air (docs/pillar-panels.md).
 *
 * A 9:16 phone video, a 4:3 archive clip or a square social post leaves black bars beside the picture on a 16:9 channel. When the
 * item on air is narrower than `maxAspect`, the OverlayManager puts the `pillar_panels` CG page (web/cg/pillar_panels.html) on
 * layer 11, under every other graphic: a slow animated starfield in both bars with weather, sponsor banners, this day in history
 * and coming up next. The page only draws in the bars; the picture in the middle is untouched.
 *
 * This class decides (`specFor`, the OverlayManager's Hooks::pillarFor) and gathers the data. The page never fetches anything:
 * weather (Open-Meteo) and history (Wikipedia "on this day") are fetched and cached here and handed to the page as the `data`
 * param, so a station without internet still gets the last good copy, and up next comes from the executor's runlists.
 *
 * Settings `pillar.config`:
 *   { enabled:false, channels:[] (empty = all), maxAspect:1.5, types:[] (only these item/media types; empty = any),
 *     excludeTypes:[], live:false (live inputs only with extra.displayAspect on their library entry),
 *     template:"pillar_panels", panels:{weather,sponsors,history,upNext}, station:"", accent:"#ffc94a",
 *     weather:{place, lat, lon, refreshMin:15}, sponsors:[{name, tagline, image, color}], sponsorSec:9,
 *     history:{source:"wikipedia"|"local", items:[{year,text}], count:8, avoid:[word stems]}, historySec:11,
 *     upNext:{count:3, skipTypes:[ad,promo,psa,bumper,station_id,filler,gap,cg,image]}, stars:{speed:1, density:1, fps:24} }
 * Per media `extra.pillarPanels` true/false forces panels on / off for that item; `extra.displayAspect` ("9:16", "4:3" or a number)
 * overrides width/height; `extra.rotation` 90/270 swaps them.
 */
class PillarPanels : public QObject {
    Q_OBJECT
public:
    PillarPanels(sched::ScheduleStore* store, sched::ScheduleExecutor* exec, QObject* parent = nullptr);

    void start();                                   // fetch timers (weather every refreshMin, history on date change)

    // --- the OverlayManager hook ----------------------------------------------------------------
    /// {template, params:{aspect, data}} for the item on air, or {} (and the reason is kept for status()).
    QJsonObject specFor(const QString& channelId, const QJsonObject& item);

    // --- REST -------------------------------------------------------------------------------------
    QJsonObject config() const;                                      // GET  /api/v1/pillar/config
    QJsonObject setConfig(const QJsonObject& body);                  // PUT  /api/v1/pillar/config
    QJsonObject status() const;                                      // GET  /api/v1/pillar
    QJsonObject data(const QString& channelId) const;                // GET  /api/v1/pillar/{ch}/data (what the page is given)
    QJsonObject preview(const QString& channelId, const QJsonObject& body);   // POST /api/v1/pillar/{ch}/preview {on, aspect?, sec?}
    QJsonObject refresh();                                           // POST /api/v1/pillar/refresh (fetch weather + history now)

    // --- pure helpers (public for tests) --------------------------------------------------------------
    static QJsonObject defaults();
    static QJsonObject normalize(const QJsonObject& cfg);
    /// Display aspect (width / height) of an item: extra.pillarPanels / displayAspect / rotation, then width x height. 0 = unknown.
    static double displayAspect(const QJsonObject& item);
    /// Does this item get panels? `why` says why or why not.
    static bool qualifies(const QJsonObject& cfg, const QString& channelId, const QJsonObject& item, double* aspect, QString* why);
    /// Open-Meteo JSON -> {tempF, feelsF, cond, icon, isDay, windMph, hiF, loF, days:[{dow, hiF, loF, icon, cond}]}.
    static QJsonObject parseWeather(const QJsonObject& openMeteo, const QTimeZone& tz);
    /// Wikipedia feed/onthisday JSON -> up to `count` [{year, text}], short entries spread across the years. Entries that contain a word
    /// starting with one of `avoid` (deaths, attacks, disasters: the wrong tone beside a feel-good clip) are left out.
    static QJsonArray parseHistory(const QJsonObject& wiki, int count, const QStringList& avoid = defaultAvoid());
    static QStringList defaultAvoid();
    /// Runlist JSON (ScheduleExecutor::runlistJson) -> up to `count` [{time, title, type}] after `nowMs`, skipping break types
    /// and the item on air (`currentRunlistId`).
    static QJsonArray upNextFrom(const QJsonObject& runlists, qint64 nowMs, int count, const QStringList& skipTypes, const QString& currentRunlistId, const QTimeZone& tz);
    static QString iconFor(int wmoCode);             // sun, moon, partly, partly_night, cloud, fog, drizzle, rain, storm, snow
    static QString condFor(int wmoCode, bool day);

signals:
    /// Data or settings changed: the Supervisor re-syncs the pillar layer of every channel.
    void changed(const QString& channelId);          // "" = every channel

private:
    sched::ScheduleStore* m_store;
    sched::ScheduleExecutor* m_exec;
    QNetworkAccessManager m_nam;
    QTimer m_timer;
    QJsonObject m_weather, m_history;               // caches: {..., updatedMs, error}
    QString m_historyDate;                          // MM-dd of the cached history
    qint64 m_weatherAtMs = 0, m_weatherTryMs = 0, m_historyTryMs = 0;
    struct Force { qint64 untilMs = 0; double aspect = 0.5625; };
    QHash<QString, Force> m_force;                  // preview: panels on regardless of the picture
    mutable QHash<QString, QJsonObject> m_last;     // channel -> last decision {on, aspect, why, title}

    QJsonObject cfg() const;
    QTimeZone tzFor(const QString& channelId) const;
    void tick();
    void fetchWeather();
    void fetchHistory();
};

#endif // PILLARPANELS_H
