#ifndef WEATHERSPOTS_H
#define WEATHERSPOTS_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/**
 * Weather spot (SpotGenerator recipe `weather_full`, render "scenes"): the forecast as animated daily cards plus the almanac and
 * safety segments a local station carries: sunset, moon phase, burn safety and lake levels.
 *
 * Inputs (fetched by the SpotGenerator, parsed here, all pure QtCore):
 *   Open-Meteo forecast JSON  current + daily + hourly (3 past days, 8 forecast days)
 *   NWS alerts JSON           api.weather.gov/alerts/active?point=lat,lon  (Red Flag / Fire Weather)
 *   AEP hydro HTML            Smith Mountain and Leesville lake levels (the page's "Most Recent Conditions" table)
 *   USGS instantaneous JSON   Philpott Lake elevation (parameter 62614) with its local humidity / rain
 *
 * A scene carries an `anim` object that scripts/weather_cards.py turns into a looping animated background (sun rays, drifting clouds,
 * rain, snow, twinkling stars, a phase-accurate moon, a burn gauge, lake waves) with the card layout over it:
 *   {type: now|cards|sunset|moon|burn|lakes, cond, isDay, ...type fields}.
 */
namespace weatherspots {

// ---------------------------------------------------------------- conditions
/// WMO weather code -> icon kind: clear, mostly_clear, partly, cloudy, fog, drizzle, rain, heavy_rain, storm, snow, sleet.
QString conditionOf(int wmoCode);
/// "sunny" / "clear skies" (night), "partly cloudy", "light rain", ...
QString conditionText(int wmoCode, bool day);

// ---------------------------------------------------------------- moon
/// {ageDays, illumination (0-100), phase ("Waxing Crescent"), waxing, fraction (0-1 through the cycle), nextFull, nextNew (ISO dates)}.
/// Mean synodic month from the 2000-01-06 18:14 UTC new moon: dates can be a day off the astronomical instant, the phase is accurate to a few hours.
QJsonObject moonInfo(const QDateTime& when);

// ---------------------------------------------------------------- burn safety
/// A transparent, rule-based fire-spread rating for open burning (not an official fire-danger index):
/// gust (mph), min relative humidity (%), rain in the last three days (in), rain expected today (in), high (F), NWS fire alert ("", "watch", "warning").
/// Returns {rating: Low|Moderate|High|Very High|Extreme, score, level (0-1), reasons[], advice, law ("" outside Virginia's Feb 15 - Apr 30 4 PM burning law season)}.
QJsonObject burnSafety(double gustMph, double minHumidity, double rainPast3Days, double rainToday, double highF, const QString& alert, const QDate& date);

// ---------------------------------------------------------------- sources
/// AEP "Most Recent Conditions" table -> {<project>: {fullPond, actual, tailwater}} keyed by lower-case name ("smith mountain", "leesville", ...).
QJsonObject parseAepHydro(const QString& html);
/// USGS iv JSON (period P1D or longer) -> {elevation, changeFt (latest minus the reading ~24 h earlier, when there is one), time, humidity, airTempF, precipIn}.
QJsonObject parseUsgsLake(const QByteArray& json);
/// NWS alerts JSON -> {alert: ""|"watch"|"warning", events[], headline} (Red Flag Warning / Fire Weather Watch map to the burn rating; others are listed).
QJsonObject parseNwsAlerts(const QByteArray& json);

// ---------------------------------------------------------------- the script
/// forecast: Open-Meteo JSON object; alerts: parseNwsAlerts result; lakes: {smithMountain|leesville|philpott: {actual, fullPond, prev24?, changeFt?}}.
/// opts: place, siteName, site, segments (["now","cards","sunset","moon","burn","lakes"]), cardDays (7), energy.
QJsonObject weatherScript(const QJsonObject& forecast, const QJsonObject& alerts, const QJsonObject& lakes, const QDateTime& now, const QJsonObject& opts);

} // namespace weatherspots

#endif // WEATHERSPOTS_H
