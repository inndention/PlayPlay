#ifndef AREAMARKET_H
#define AREAMARKET_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * Area markets: the place a generated spot is made for. (Not the financial "markets" of marketspots.h.)
 *
 * A market is DATA: display name, state, county, nearby towns, the weather point, news / sports / traffic source hints, the
 * wording of the intros and sign-offs, and notes on how the voice should sound. Three are built in (Martinsville / Henry County VA,
 * Reidsville / Rockingham County NC, Danville / Pittsylvania County VA); an operator adds or amends markets in settings
 * `spots.markets` ({<key>: {...partial or full market...}}), merged field by field over the built-ins.
 *
 * The SpotGenerator resolves ONE market per generation (request > job > recipe override > station default `spots.market.default`;
 * none = the generic Martinsville-flavoured defaults the recipes always had) and then:
 *   - localizeRecipe()  patches the recipe: place names, feeds, school list, traffic centre, intro / outro wording, series, and a
 *                       `localFilter` regex so a corridor-wide feed only yields stories about the market's own towns;
 *   - promptContext()   is the paragraph added to the LLM writer's prompt;
 *   - pronounce()       respells place names the speech engines get wrong (narration only, never the screen text);
 *   - recordMeta()      is what lands on the library record (`extra.market`).
 * Everything here is pure (QtCore only).
 *
 * Market fields: key, name, label, state, stateName, county, area, region, lat, lon, tz, towns[], counties[],
 *   weather{place, lat, lon, nwsOffice, nwsPoint}, wording{signOff, weatherSignOff, tagline},
 *   voice{style, pronunciations{word: spoken}, avoid[]},
 *   news{feeds[], sportsFeeds[], keywords (regex), placeSlugs[]},
 *   recipes{"<key>[,<key>...]": {<recipe fields to override>}}  (string values may use the {placeholders} below).
 * Placeholders: {place} {PLACE} {area} {AREA} {county} {state} {stateName} {region} {towns} {station}; unknown ones stay as written.
 */
namespace areamarket {

/// Built-in market keys, in picker order.
QStringList builtinKeys();
/// "Danville, VA" / " DANVILLE " -> "danville". Not validated against the known markets.
QString normalizeKey(const QString& s);

/// The built-ins, expanded (derived fields filled in, `news` shorthand turned into per-recipe feed patches).
QJsonArray builtinMarkets();
/// Built-ins merged with the operator's `spots.markets` object ({key: partial|full}); an unknown key with a `name` becomes a new market.
QJsonArray allMarkets(const QJsonObject& custom);
/// One expanded market, or an empty object when the key is not defined.
QJsonObject find(const QString& key, const QJsonObject& custom = QJsonObject());

/// First valid of: requested, job, recipe override, station default. "none" at any level means "no market" (the generic defaults) and stops the search.
/// Returns the normalized key, or "" for no market.
QString resolve(const QString& requested, const QString& jobMarket, const QString& recipeMarket, const QString& stationDefault, const QJsonObject& custom = QJsonObject());

/// {placeholders} filled from the market (+ the station / brand name when given).
QString fill(const QString& text, const QJsonObject& market, const QString& station = QString());

/// The recipe with the market applied: matching `recipes` patches, `market` / `marketName`, and the news `localFilter`. Empty market = the recipe unchanged.
QJsonObject localizeRecipe(const QJsonObject& recipe, const QJsonObject& market);
/// Recipes that read national material (markets, history, tech ...) and so are never narrowed to the market's towns.
bool isNationalRecipe(const QJsonObject& recipe);

/// True when `text` mentions one of the market's towns / counties (its `news.keywords` regex; case-insensitive).
bool mentionsMarket(const QString& text, const QJsonObject& market);
/// Narration with the market's respellings applied ("Pittsylvania" -> "Pit sill vane ya"); screen text is never passed through this.
QString pronounce(const QString& narration, const QJsonObject& market);

/// The writer's paragraph: where the viewers are, which names to use, how the voice should sound, and which other markets NOT to name.
QString promptContext(const QJsonObject& market, const QJsonObject& custom = QJsonObject());

/// {place, lat, lon, nwsOffice, nwsPoint} - the weather point of the market.
QJsonObject weatherOf(const QJsonObject& market);
/// {key, name, label, state, county, area} - what the library record carries (`extra.market`).
QJsonObject recordMeta(const QJsonObject& market);
/// The compact form the pickers list: recordMeta + region, towns, counties, voice style, sources (feeds, schools, traffic provider), builtin flag.
QJsonObject summary(const QJsonObject& market, const QJsonObject& custom = QJsonObject());

} // namespace areamarket

#endif // AREAMARKET_H
