#ifndef LOCALSPOTS_H
#define LOCALSPOTS_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>

/**
 * Local spots: the data adapters behind four scene-rendered SpotGenerator recipes (render "scenes").
 *
 *   cinema   hollywoodcinema.net: schema.org Movie / ScreeningEvent markup -> showtimes, one scene per movie (poster)
 *   traffic  VDOT 511 GeoJSON layers (incidents, construction, weatherClosures) near the station -> one scene per alert
 *   sports   MaxPreps team schedule pages (__NEXT_DATA__) -> recent finals + games in the next 48 hours
 *   realty   a brokerage site's Showcase IDX listing JSON -> one featured home per run, 4-8 of its own photos
 *
 * Everything is pure (QtCore only): the SpotGenerator fetches the pages and hands the bytes in, these functions parse them
 * and write the script. A script is {headline, lines[], narration, scenes[], sources[], stories[], package, energy, writer,
 * local{}} for the shared scene pipeline (photo -> furniture card -> voice -> ffmpeg), or {skip: reason} when there is nothing
 * worth airing, or {error: reason}. Scene fields: kind intro|story|outro, narration (the exact words the voice reads), title,
 * detail (a big line under the title), category / place (chips), summary, image (URL), counter, eyebrow / big / sub (intro and
 * outro lines), rundown[].
 */
namespace localspots {

// ---------------------------------------------------------------- speech + display forms
/// 159900 -> "one hundred fifty nine thousand nine hundred".
QString spokenNumber(qint64 n);
/// 5:00 PM -> "five P M", 17:05 -> "five oh five P M".
QString spokenClock(const QTime& t, bool withSuffix = true);
/// Same-meridiem runs share one suffix: 13:00 15:00 17:00 -> "one, three and five P M".
QString spokenTimeList(const QList<QTime>& times);
/// Same rule for the screen: "1:00 · 3:00 · 5:00 PM".
QString displayTimeList(const QList<QTime>& times);
/// "276-555-0100" / "2765550100" -> "two seven six, five five five, zero one zero zero".
QString spokenPhone(const QString& phone);
/// "1102 Smith Lake Rd" -> "1102 Smith Lake Road" (street suffixes expanded for the voice).
QString spokenAddress(const QString& address);
/// Speech hygiene for any scraped sentence: tags and entities out, symbols to words, abbreviations expanded.
QString speech(const QString& text);
/// A 511 incident message in speech form ("I-81" -> "Interstate 81", "Rt. 672" -> "Route 672", "US-19" -> "U S 19").
QString spokenRoad(const QString& text);
/// "1h 49m" -> "one hour forty nine minutes".
QString spokenRuntime(const QString& runtime);
/// Great-circle distance in miles.
double milesBetween(double lat1, double lon1, double lat2, double lon2);

// ---------------------------------------------------------------- script building blocks (shared by the other scene adapters)
/// A scene whose narration is exactly what the voice reads (kind intro | story | outro); the caller adds title, detail, image, ...
QJsonObject makeScene(const QString& kind, const QString& narration);
/// Wraps scenes into the script shape the scene render reads ({headline, lines, narration, scenes, stories, package, energy, writer, local, sources}).
QJsonObject makeScript(const QJsonArray& scenes, const QString& headline, const QStringList& lines, const QString& package, int energy, const QJsonObject& local, const QJsonArray& sources);
QJsonObject makeSource(const QString& title, const QString& link, const QString& image = QString(), const QString& src = QString());
QJsonObject skipResult(const QString& why);     // {skip: why}: nothing worth airing this cycle
QJsonObject errorResult(const QString& why);    // {error: why}
/// Screen text cut at a word boundary with "..." (never mid-word).
QString trimScreen(QString s, int maxLen);

// ---------------------------------------------------------------- cinema
/// {movies: [{title, rating, runtime, description, poster, release, days: [{day, times: ["17:00", ...]}]}], address, price: {base, exceptHour, exceptPrice}}.
QJsonObject parseCinema(const QString& html);
/// opts: siteName, place, site, maxMovies (6), leadMin (10: shows starting sooner than this are not offered).
QJsonObject cinemaScript(const QJsonObject& page, const QDateTime& now, const QJsonObject& opts);

// ---------------------------------------------------------------- traffic
/// A 511 GeoJSON layer -> [{id, layer, priority, route, type, location, message, lat, lon}].
QJsonArray parseIncidents(const QByteArray& geojson, const QString& layer);
/// opts: lat, lon, radiusMi (45), place, siteName, site, maxScenes (5). feedOk=false (nothing could be read) -> {error}: roads are never reported clear on a dead feed.
QJsonObject trafficScript(const QJsonArray& incidents, bool feedOk, const QDateTime& now, const QJsonObject& opts);

// ---------------------------------------------------------------- sports
/// [{name, short, state, bases[]}] - the Martinsville / Henry County schools; [{slug, label}] - the sports MaxPreps lists.
QJsonArray defaultSchools();
QJsonArray defaultSports();
/// MaxPreps school-year label for a date: July onwards belongs to the school year that starts that year ("26-27").
QString activeSeasonLabel(const QDate& d);
/// One team's schedule page -> games [{school, sport, opponent, startLocal, tbd, ourScore, theirScore, result, final, home, neutral, kind}].
/// home: 1 / 0 / -1 (unknown). A page for another state, season or school yields nothing (MaxPreps redirects unknown slugs).
QJsonArray parseTeamSchedule(const QByteArray& html, const QString& schoolName, const QString& sportLabel, const QString& stateCode, const QString& season);
/// opts: recentDays (4), upcomingHours (48), maxResults (5), maxUpcoming (4), siteName, site.
QJsonObject sportsScript(const QJsonArray& games, const QDateTime& now, const QJsonObject& opts);

// ---------------------------------------------------------------- realty
/// Showcase IDX listing objects embedded in a page -> [{mls, address, city, state, zip, county, subdivision, price, status, beds,
/// baths, sqft, acres, yearBuilt, description, images[], firstSeenMs, agent, agentPhone, office, officePhone, remarks}]. Photo URLs
/// are the full-size ones ("?d=s" thumbnails stripped).
QJsonArray parseListings(const QString& html);
/// Which home to feature next. state: {featured: {mls: ms}, seen: {mls: ms}, lastPick, lastCheckMs}. opts: minPhotos (4), statuses
/// (["Active"]). Returns {listing (null when none), eligible, newIds[], state}; "never featured" comes before "featured longest ago"
/// (so a new listing airs on the next run), the home just featured is never repeated while another is eligible.
QJsonObject pickListing(const QJsonArray& listings, const QJsonObject& state, qint64 nowMs, const QJsonObject& opts);
/// state with `mls` stamped as featured at nowMs.
QJsonObject markFeatured(const QJsonObject& state, const QString& mls, qint64 nowMs);
/// opts: siteName, site, minPhotos (4), maxPhotos (8).
QJsonObject realtyScript(const QJsonObject& listing, const QDateTime& now, const QJsonObject& opts);

} // namespace localspots

#endif // LOCALSPOTS_H
