#include "areamarket.h"

#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace areamarket {

// ---------------------------------------------------------------------------------------------
// The built-in markets, as data. One raw JSON document each (MSVC caps a string literal at ~16 KB).
// Sources were checked on 2026-10-23: the RSS feeds answered, every MaxPreps school page below returned that school's own
// schedule, and the VDOT 511 layers cover the Virginia markets. NCDOT's DriveNC feed needs an API key, so the Reidsville traffic recipe
// says so instead of airing an all-clear it cannot know ("provider": "drivenc").
// ---------------------------------------------------------------------------------------------
static const char* kMartinsville = R"JSON({
  "key": "martinsville", "name": "Martinsville", "state": "VA", "stateName": "Virginia", "county": "Henry County",
  "area": "Martinsville and Henry County", "region": "Southside Virginia and the Smith River valley",
  "lat": 36.69, "lon": -79.87, "tz": "America/New_York",
  "towns": ["Martinsville", "Ridgeway", "Bassett", "Fieldale", "Collinsville", "Stanleytown", "Axton", "Laurel Park", "Irisburg", "Stuart"],
  "counties": ["Henry County", "Patrick County", "Franklin County"],
  "weather": {"place": "Martinsville", "nwsOffice": "RNK", "nwsPoint": "36.69,-79.87"},
  "wording": {"signOff": "That's your {area} update.", "weatherSignOff": "Stay with us for {area} weather updates."},
  "voice": {"style": "Warm, plainspoken Southside Virginia anchor. Call the city Martinsville and the county Henry County; name the town (Ridgeway, Bassett, Fieldale, Collinsville) when the material does.",
            "pronunciations": {},
            "avoid": ["Danville", "Reidsville", "Pittsylvania County", "Rockingham County"]},
  "news": {
    "feeds": ["https://martinsvillebulletin.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc", "https://www.wsls.com/arc/outboundfeeds/rss/category/news/local/?outputType=xml", "https://www.getstarnews.com/feed/"],
    "sportsFeeds": ["https://www.wsls.com/arc/outboundfeeds/rss/category/sports/?outputType=xml", "https://www.getstarnews.com/feed/"],
    "keywords": "martinsville|henry county|ridgeway|bassett|fieldale|collinsville|stanleytown|axton|laurel park|irisburg|patrick county|philpott|smith river",
    "placeSlugs": ["martinsville"]},
  "recipes": {
    "headlines": {"introText": "Here are the top {area} stories at this hour from {site}.", "series": "{place} Headline News"},
    "story": {"series": "{place} Stories"},
    "community": {"intro": "In {area} community news.", "outro": "That's your {area} community bulletin from {station}.", "series": "{place} Community Bulletin"},
    "events": {"intro": "Here's what's coming up around {area}.", "outro": "Find more {place} area events at {station}.", "series": "{place} Events"},
    "obituaries": {"intro": "We pause to remember members of our {area} community.", "outro": "Our thoughts are with their families.", "series": "{place} Remembrances"},
    "sports": {"intro": "Here's your {area} sports update.", "outro": "That's {place} area sports from {station}."},
    "roads": {"intro": "Road watch for your {place} area drive.", "outro": "Drive carefully, and we'll keep you posted."},
    "weather,weather_am,weather_pm,weather_week,almanac": {"signOff": "Stay with us for {area} weather updates.", "series": "{place} Weather"},
    "cinema": {"url": "https://hollywoodcinema.net/", "siteName": "Hollywood Cinema", "site": "hollywoodcinema.net", "place": "Martinsville"},
    "traffic": {"provider": "vdot", "place": "Martinsville", "area": "Henry County", "lat": 36.69, "lon": -79.87, "radiusMi": 45, "siteName": "VDOT 511", "site": "511virginia.org",
                "outroText": "For the latest road conditions before you go, visit five eleven virginia dot org.", "series": "{place} Traffic Alerts"},
    "sports_local": {"area": "Martinsville and Henry County", "series": "{place} Sports Desk",
      "schools": [
        {"name": "Martinsville", "state": "VA", "bases": ["https://www.maxpreps.com/va/martinsville/martinsville-bulldogs/"]},
        {"name": "Magna Vista", "state": "VA", "bases": ["https://www.maxpreps.com/va/ridgeway/magna-vista-warriors/"]},
        {"name": "Bassett", "state": "VA", "bases": ["https://www.maxpreps.com/va/bassett/bassett-bengals/"]},
        {"name": "Patrick County", "state": "VA", "bases": ["https://www.maxpreps.com/va/stuart/patrick-county-cougars/"]},
        {"name": "Carlisle", "state": "VA", "bases": ["https://www.maxpreps.com/va/martinsville/carlisle-chiefs/", "https://www.maxpreps.com/va/martinsville/carlisle-school-chiefs/"]}]},
    "realty": {"area": "Martinsville and Henry County"}
  }
})JSON";

static const char* kReidsville = R"JSON({
  "key": "reidsville", "name": "Reidsville", "state": "NC", "stateName": "North Carolina", "county": "Rockingham County",
  "area": "Reidsville and Rockingham County", "region": "the northern Piedmont Triad",
  "lat": 36.3544, "lon": -79.6628, "tz": "America/New_York",
  "towns": ["Reidsville", "Eden", "Madison", "Mayodan", "Stoneville", "Wentworth", "Ruffin"],
  "counties": ["Rockingham County", "Caswell County", "Guilford County"],
  "weather": {"place": "Reidsville", "nwsOffice": "RNK", "nwsPoint": "36.3544,-79.6628"},
  "wording": {"signOff": "That's your {area} update.", "weatherSignOff": "Stay with us for {area} weather updates."},
  "voice": {"style": "Friendly North Carolina Piedmont anchor. Call the city Reidsville and the county Rockingham County; say the town (Eden, Madison, Mayodan, Stoneville) when the material does. Speak of the Triad only when the material does.",
            "pronunciations": {"Reidsville": "Reedsville", "Mayodan": "May oh dan"},
            "avoid": ["Martinsville", "Henry County", "Danville", "Pittsylvania County"]},
  "news": {
    "feeds": ["https://www.wxii12.com/topstories-rss", "https://myfox8.com/feed/", "https://www.wfmynews2.com/feeds/syndication/rss/news/local", "https://www.getstarnews.com/feed/"],
    "sportsFeeds": ["https://www.getstarnews.com/feed/"],
    "keywords": "reidsville|rockingham county|mayodan|stoneville|wentworth|ruffin|city of eden|eden,? n\\.?c|madison,? n\\.?c|morehead high|mcmichael high",
    "placeSlugs": ["reidsville"]},
  "recipes": {
    "headlines": {"introText": "Here are the top {area} stories at this hour from {site}.", "series": "{place} Headline News"},
    "story": {"series": "{place} Stories"},
    "community": {"intro": "In {area} community news.", "outro": "That's your {area} community bulletin from {station}.", "series": "{place} Community Bulletin"},
    "events": {"intro": "Here's what's coming up around {area}.", "outro": "Find more {place} area events at {station}.", "series": "{place} Events"},
    "obituaries": {"intro": "We pause to remember members of our {area} community.", "outro": "Our thoughts are with their families.", "series": "{place} Remembrances"},
    "sports": {"intro": "Here's your {area} sports update.", "outro": "That's {place} area sports from {station}."},
    "roads": {"intro": "Road watch for your {place} area drive.", "outro": "Drive carefully, and we'll keep you posted."},
    "weather,weather_am,weather_pm,weather_week,almanac": {"signOff": "Stay with us for {area} weather updates.", "series": "{place} Weather"},
    "cinema": {"url": "", "place": "Reidsville"},
    "traffic": {"provider": "drivenc", "place": "Reidsville", "area": "Rockingham County", "lat": 36.3544, "lon": -79.6628, "radiusMi": 30, "siteName": "NCDOT DriveNC", "site": "drivenc.gov",
                "outroText": "For the latest road conditions before you go, visit drive N C dot gov.", "series": "{place} Traffic Alerts"},
    "sports_local": {"area": "Reidsville and Rockingham County", "series": "{place} Sports Desk",
      "schools": [
        {"name": "Reidsville", "state": "NC", "bases": ["https://www.maxpreps.com/nc/reidsville/reidsville-rams/"]},
        {"name": "Rockingham County", "state": "NC", "bases": ["https://www.maxpreps.com/nc/wentworth/rockingham-county-cougars/"]},
        {"name": "Morehead", "state": "NC", "bases": ["https://www.maxpreps.com/nc/eden/morehead-panthers/"]},
        {"name": "McMichael", "state": "NC", "bases": ["https://www.maxpreps.com/nc/mayodan/mcmichael-phoenix/"]}]},
    "realty": {"url": "", "area": "Reidsville and Rockingham County"},
    "weather_full": {"series": "{place} Weather", "segments": ["now", "cards", "sunset", "moon", "burn"], "burnLaw": false}
  }
})JSON";

static const char* kDanville = R"JSON({
  "key": "danville", "name": "Danville", "state": "VA", "stateName": "Virginia", "county": "Pittsylvania County",
  "area": "Danville and Pittsylvania County", "region": "Southside Virginia and the Dan River Region",
  "lat": 36.586, "lon": -79.395, "tz": "America/New_York",
  "towns": ["Danville", "Gretna", "Chatham", "Ringgold", "Dry Fork", "Blairs", "Cascade", "Yanceyville"],
  "counties": ["Pittsylvania County", "Caswell County", "Halifax County"],
  "weather": {"place": "Danville", "nwsOffice": "RNK", "nwsPoint": "36.586,-79.395"},
  "wording": {"signOff": "That's your {area} update.", "weatherSignOff": "Stay with us for {area} weather updates."},
  "voice": {"style": "Steady, neighborly Southside Virginia anchor. Call the city Danville and the county Pittsylvania County (say the full county name on first reference); name the town (Gretna, Chatham, Ringgold) when the material does; the Dan River Region is the area.",
            "pronunciations": {"Pittsylvania": "Pit sill vane ya", "Yanceyville": "Yan see ville"},
            "avoid": ["Martinsville", "Henry County", "Reidsville", "Rockingham County"]},
  "news": {
    "feeds": ["https://www.godanriver.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc", "https://www.getstarnews.com/feed/"],
    "sportsFeeds": ["https://www.godanriver.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc", "https://www.getstarnews.com/feed/"],
    "keywords": "danville|pittsylvania|gretna|chatham|ringgold|dry fork|blairs|cascade|yanceyville|caswell county|dan river|averett",
    "placeSlugs": ["danville"]},
  "recipes": {
    "headlines": {"introText": "Here are the top {area} stories at this hour from {site}.", "series": "{place} Headline News"},
    "story": {"series": "{place} Stories"},
    "community": {"intro": "In {area} community news.", "outro": "That's your {area} community bulletin from {station}.", "series": "{place} Community Bulletin"},
    "events": {"intro": "Here's what's coming up around {area}.", "outro": "Find more {place} area events at {station}.", "series": "{place} Events"},
    "obituaries": {"intro": "We pause to remember members of our {area} community.", "outro": "Our thoughts are with their families.", "series": "{place} Remembrances"},
    "sports": {"intro": "Here's your {area} sports update.", "outro": "That's {place} area sports from {station}."},
    "roads": {"intro": "Road watch for your {place} area drive.", "outro": "Drive carefully, and we'll keep you posted."},
    "weather,weather_am,weather_pm,weather_week,almanac": {"signOff": "Stay with us for {area} weather updates.", "series": "{place} Weather"},
    "cinema": {"url": "", "place": "Danville"},
    "traffic": {"provider": "vdot", "place": "Danville", "area": "Pittsylvania County", "lat": 36.586, "lon": -79.395, "radiusMi": 35, "siteName": "VDOT 511", "site": "511virginia.org",
                "outroText": "For the latest road conditions before you go, visit five eleven virginia dot org.", "series": "{place} Traffic Alerts"},
    "sports_local": {"area": "Danville and Pittsylvania County", "series": "{place} Sports Desk",
      "schools": [
        {"name": "George Washington", "state": "VA", "bases": ["https://www.maxpreps.com/va/danville/george-washington-eagles/"]},
        {"name": "Dan River", "state": "VA", "bases": ["https://www.maxpreps.com/va/ringgold/dan-river-wildcats/"]},
        {"name": "Tunstall", "state": "VA", "bases": ["https://www.maxpreps.com/va/dry-fork/tunstall-trojans/"]},
        {"name": "Gretna", "state": "VA", "bases": ["https://www.maxpreps.com/va/gretna/gretna-hawks/"]},
        {"name": "Chatham", "state": "VA", "bases": ["https://www.maxpreps.com/va/chatham/chatham-cavaliers/"]}]},
    "realty": {"url": "", "area": "Danville and Pittsylvania County"},
    "weather_full": {"series": "{place} Weather", "segments": ["now", "cards", "sunset", "moon", "burn"]}
  }
})JSON";

static const QStringList kBuiltinKeys = {"martinsville", "reidsville", "danville"};
static const QStringList kLocalFeedRecipes = {"community", "events", "obituaries", "story", "roads"};
static const QSet<QString> kNationalKeys = {"economics", "tech_review", "history", "markets_close"};

// ---------------------------------------------------------------------------------------------
static QJsonObject parseObject(const char* json) { return QJsonDocument::fromJson(QByteArray(json)).object(); }
static QJsonObject rawBuiltin(const QString& key) {
    static const QHash<QString, QJsonObject> all = {{"martinsville", parseObject(kMartinsville)}, {"reidsville", parseObject(kReidsville)}, {"danville", parseObject(kDanville)}};
    return all.value(key);
}
/// Field-wise merge of two objects, `over` winning; nested objects are merged one level down.
static QJsonObject mergeObj(const QJsonObject& base, const QJsonObject& over) {
    QJsonObject r = base;
    for (auto it = over.begin(); it != over.end(); ++it) {
        if (it.value().isObject() && r.value(it.key()).isObject()) { QJsonObject b = r.value(it.key()).toObject(); const QJsonObject o = it.value().toObject(); for (auto j = o.begin(); j != o.end(); ++j) b[j.key()] = j.value(); r[it.key()] = b; }
        else r[it.key()] = it.value();
    }
    return r;
}
/// {"a,b": patch} -> {"a": patch, "b": patch}; later specs of the same recipe merge over earlier ones.
static QJsonObject explode(const QJsonObject& specs, QJsonObject into = QJsonObject()) {
    for (auto it = specs.begin(); it != specs.end(); ++it) {
        const QJsonObject patch = it.value().toObject();
        for (const QString& name : it.key().split(',', Qt::SkipEmptyParts)) {
            const QString k = name.trimmed(); if (k.isEmpty()) continue;
            QJsonObject cur = into.value(k).toObject(); for (auto p = patch.begin(); p != patch.end(); ++p) cur[p.key()] = p.value(); into[k] = cur;
        }
    }
    return into;
}
static QString defaultKeywords(const QJsonObject& m) {
    QStringList words; words << m.value("name").toString();
    for (const QJsonValue& v : m.value("towns").toArray()) words << v.toString();
    const QString county = m.value("county").toString(); if (!county.isEmpty()) words << county;
    QStringList esc; QSet<QString> seen; for (const QString& w : words) { const QString t = w.trimmed().toLower(); if (t.isEmpty() || seen.contains(t)) continue; seen.insert(t); esc << QRegularExpression::escape(t); }
    return esc.join('|');
}

static QJsonObject expand(const QJsonObject& rawIn, const QJsonObject& customRecipes = QJsonObject()) {
    QJsonObject m = rawIn;
    m["key"] = normalizeKey(m.value("key").toString(m.value("name").toString()));
    const QString name = m.value("name").toString();
    if (m.value("area").toString().isEmpty()) m["area"] = name;
    if (m.value("label").toString().isEmpty()) m["label"] = m.value("state").toString().isEmpty() ? name : name + ", " + m.value("state").toString();
    if (m.value("tz").toString().isEmpty()) m["tz"] = "America/New_York";
    QJsonObject w = m.value("weather").toObject();
    if (w.value("place").toString().isEmpty()) w["place"] = name;
    if (!w.contains("lat")) w["lat"] = m.value("lat"); if (!w.contains("lon")) w["lon"] = m.value("lon");
    if (w.value("nwsPoint").toString().isEmpty() && m.contains("lat")) w["nwsPoint"] = QString("%1,%2").arg(m.value("lat").toDouble()).arg(m.value("lon").toDouble());
    m["weather"] = w;
    QJsonObject news = m.value("news").toObject();
    if (news.value("keywords").toString().isEmpty()) news["keywords"] = defaultKeywords(m);
    m["news"] = news;
    // recipes: one patch per recipe, then the news feeds the local-news recipes read (unless a patch names its own)
    QJsonObject byRecipe = explode(rawIn.value("recipes").toObject());
    if (!customRecipes.isEmpty()) byRecipe = explode(customRecipes, byRecipe);
    auto inject = [&](const QString& recipe, const QJsonArray& feeds) {
        if (feeds.isEmpty()) return; QJsonObject p = byRecipe.value(recipe).toObject(); if (!p.contains("feeds")) { p["feeds"] = feeds; byRecipe[recipe] = p; }
    };
    for (const QString& r : kLocalFeedRecipes) inject(r, news.value("feeds").toArray());
    inject("sports", news.value("sportsFeeds").toArray());
    m["recipes"] = byRecipe;
    return m;
}

// ---------------------------------------------------------------------------------------------
QStringList builtinKeys() { return kBuiltinKeys; }
QString normalizeKey(const QString& s) {
    QString k = s.trimmed().toLower(); const int comma = k.indexOf(','); if (comma > 0) k = k.left(comma);
    k.replace(QRegularExpression("[^a-z0-9]+"), "_"); while (k.startsWith('_')) k.remove(0, 1); while (k.endsWith('_')) k.chop(1);
    return k;
}
QJsonArray builtinMarkets() { QJsonArray a; for (const QString& k : kBuiltinKeys) a.append(expand(rawBuiltin(k))); return a; }
QJsonArray allMarkets(const QJsonObject& custom) {
    QJsonArray a; QSet<QString> done;
    for (const QString& k : kBuiltinKeys) { const QJsonObject m = find(k, custom); if (!m.isEmpty()) { a.append(m); done.insert(k); } }
    for (auto it = custom.begin(); it != custom.end(); ++it) { const QString k = normalizeKey(it.key()); if (k.isEmpty() || done.contains(k)) continue; const QJsonObject m = find(k, custom); if (!m.isEmpty()) { a.append(m); done.insert(k); } }
    return a;
}
QJsonObject find(const QString& keyIn, const QJsonObject& custom) {
    const QString key = normalizeKey(keyIn); if (key.isEmpty()) return QJsonObject();
    const QJsonObject base = rawBuiltin(key);
    QJsonObject over; for (auto it = custom.begin(); it != custom.end(); ++it) if (normalizeKey(it.key()) == key) over = it.value().toObject();
    if (base.isEmpty() && over.value("name").toString().trimmed().isEmpty()) return QJsonObject();       // a new market needs at least a name
    QJsonObject raw = base; QJsonObject overRecipes = over.value("recipes").toObject(); over.remove("recipes");
    raw = mergeObj(raw, over); raw["key"] = key;
    QJsonObject m = expand(raw, overRecipes);
    m["builtin"] = !base.isEmpty(); m["customized"] = !over.isEmpty() || !overRecipes.isEmpty();
    return m;
}

QString resolve(const QString& requested, const QString& jobMarket, const QString& recipeMarket, const QString& stationDefault, const QJsonObject& custom) {
    for (const QString& c : {requested, jobMarket, recipeMarket, stationDefault}) {
        const QString k = normalizeKey(c);
        if (k.isEmpty() || k == "default" || k == "inherit") continue;       // not chosen at this level: look further up
        if (k == "none" || k == "generic") return QString();                  // explicitly no market
        if (!find(k, custom).isEmpty()) return k;                             // an unknown key is ignored rather than failing the spot
    }
    return QString();
}

// ---------------------------------------------------------------------------------------------
static QString naturalList(const QStringList& v, int max) {
    QStringList t = v.mid(0, max); if (t.size() <= 1) return t.join(QString());
    const QString last = t.takeLast(); return t.join(", ") + " and " + last;
}
/// keepStation: leave {station} as written (a recipe patch is filled once here and again by the generator, which knows the channel's brand).
static QString fillImpl(const QString& text, const QJsonObject& m, const QString& station, bool keepStation) {
    if (text.indexOf('{') < 0) return text;
    QStringList towns; for (const QJsonValue& v : m.value("towns").toArray()) towns << v.toString();
    const QString place = m.value("name").toString(), area = m.value("area").toString();
    const QHash<QString, QString> vars = {{"place", place}, {"PLACE", place.toUpper()}, {"area", area}, {"AREA", area.toUpper()}, {"county", m.value("county").toString()}, {"state", m.value("state").toString()},
        {"stateName", m.value("stateName").toString()}, {"region", m.value("region").toString()}, {"towns", naturalList(towns, 4)}, {"station", station.isEmpty() ? QString("your local station") : station}};
    QString out; int i = 0;
    while (i < text.size()) {
        if (text[i] == '{') { const int e = text.indexOf('}', i + 1); if (e > i) { const QString k = text.mid(i + 1, e - i - 1); if (vars.contains(k) && !(keepStation && k == "station") && (!m.isEmpty() || k == "station")) { out += vars.value(k); i = e + 1; continue; } } }
        out += text[i++];
    }
    return out;
}

QString fill(const QString& text, const QJsonObject& m, const QString& station) { return fillImpl(text, m, station, false); }

bool isNationalRecipe(const QJsonObject& r) {
    if (r.value("national").toBool(false)) return true;
    const QString src = r.value("source").toString("rss");
    return kNationalKeys.contains(r.value("key").toString()) || src == "history" || src == "markets";
}
QJsonObject localizeRecipe(const QJsonObject& recipe, const QJsonObject& m) {
    if (m.isEmpty()) return recipe;
    QJsonObject r = recipe; const QString key = recipe.value("key").toString();
    r["market"] = m.value("key"); r["marketName"] = m.value("name");
    const QJsonObject patches = m.value("recipes").toObject();
    for (const QString& spec : {QString("*"), key}) {
        const QJsonObject p = patches.value(spec).toObject();
        for (auto it = p.begin(); it != p.end(); ++it) r[it.key()] = it.value().isString() ? QJsonValue(fillImpl(it.value().toString(), m, QString(), true)) : it.value();
    }
    if (r.value("source").toString("rss") == "rss" && !r.contains("localFilter") && !isNationalRecipe(r)) {
        const QString kw = m.value("news").toObject().value("keywords").toString(); if (!kw.isEmpty()) r["localFilter"] = kw;
    }
    return r;
}

bool mentionsMarket(const QString& text, const QJsonObject& m) {
    const QString kw = m.value("news").toObject().value("keywords").toString(); if (kw.isEmpty()) return true;
    return QRegularExpression(kw, QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}
QString pronounce(const QString& narration, const QJsonObject& m) {
    const QJsonObject map = m.value("voice").toObject().value("pronunciations").toObject(); if (map.isEmpty()) return narration;
    QString out = narration;
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it.key().trimmed().isEmpty() || !it.value().isString()) continue;
        out.replace(QRegularExpression("\\b" + QRegularExpression::escape(it.key()) + "\\b"), it.value().toString());
    }
    return out;
}

QString promptContext(const QJsonObject& m, const QJsonObject& custom) {
    if (m.isEmpty()) return QString();
    QStringList towns, others, avoid; for (const QJsonValue& v : m.value("towns").toArray()) towns << v.toString();
    for (const QJsonValue& v : allMarkets(custom)) { const QJsonObject o = v.toObject(); if (o.value("key").toString() != m.value("key").toString()) others << o.value("name").toString(); }
    const QJsonObject voice = m.value("voice").toObject(), wording = m.value("wording").toObject();
    for (const QJsonValue& v : voice.value("avoid").toArray()) avoid << v.toString();
    QString s = QString("LOCAL MARKET: %1, %2 (%3). Viewers live in %4.").arg(m.value("name").toString(), m.value("stateName").toString(m.value("state").toString()), m.value("county").toString(), m.value("area").toString());
    if (!m.value("region").toString().isEmpty()) s += " Region: " + m.value("region").toString() + ".";
    if (!towns.isEmpty()) s += " Nearby places: " + towns.join(", ") + ".";
    s += " Write for this market: use these place names, and the local way of saying them, wherever the material allows; do not add places the material does not mention.";
    if (!voice.value("style").toString().isEmpty()) s += " Voice and wording: " + voice.value("style").toString();
    if (!wording.value("signOff").toString().isEmpty()) s += " If the spot needs a closing line, use the market's own: \"" + fill(wording.value("signOff").toString(), m) + "\"";
    if (!avoid.isEmpty()) s += " Never name " + avoid.join(", ") + " unless the material is about them.";
    else if (!others.isEmpty()) s += " This station also serves " + others.join(" and ") + ", but this spot is not for them: do not name them unless the material does.";
    return s.simplified();
}

QJsonObject weatherOf(const QJsonObject& m) {
    if (m.isEmpty()) return QJsonObject();
    QJsonObject w = m.value("weather").toObject();
    if (!w.contains("lat")) w["lat"] = m.value("lat"); if (!w.contains("lon")) w["lon"] = m.value("lon");
    if (w.value("place").toString().isEmpty()) w["place"] = m.value("name");
    return w;
}
QJsonObject recordMeta(const QJsonObject& m) {
    QJsonObject o; if (m.isEmpty()) return o;
    for (const char* k : {"key", "name", "label", "state", "county", "area"}) o[k] = m.value(k);
    return o;
}
QJsonObject summary(const QJsonObject& m, const QJsonObject& custom) {
    QJsonObject o = recordMeta(m); if (m.isEmpty()) return o;
    for (const char* k : {"stateName", "region", "towns", "counties", "lat", "lon", "builtin", "customized"}) o[k] = m.value(k);
    o["voiceStyle"] = m.value("voice").toObject().value("style");
    o["pronunciations"] = m.value("voice").toObject().value("pronunciations");
    o["weather"] = weatherOf(m);
    const QJsonObject news = m.value("news").toObject(), rec = m.value("recipes").toObject();
    QJsonArray schools; for (const QJsonValue& v : rec.value("sports_local").toObject().value("schools").toArray()) schools.append(v.toObject().value("name"));
    const QJsonObject tr = rec.value("traffic").toObject(), cin = rec.value("cinema").toObject(), re = rec.value("realty").toObject();
    QJsonObject src;
    src["feeds"] = news.value("feeds"); src["sportsFeeds"] = news.value("sportsFeeds"); src["keywords"] = news.value("keywords"); src["schools"] = schools;
    src["traffic"] = tr.value("provider").toString("vdot") + (tr.value("siteName").toString().isEmpty() ? QString() : " (" + tr.value("siteName").toString() + ")");
    src["trafficConfigured"] = tr.value("provider").toString("vdot") == "vdot";
    src["cinema"] = cin.contains("url") ? cin.value("url") : QJsonValue("default"); src["realty"] = re.contains("url") ? re.value("url") : QJsonValue("default");   // "" = no source for this market; "default" = the recipe's own
    o["sources"] = src;
    o["context"] = promptContext(m, custom);
    return o;
}

} // namespace areamarket
