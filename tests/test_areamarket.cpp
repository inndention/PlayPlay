// Area markets: the market data (Martinsville / Reidsville / Danville), how a market resolves, how it localizes a recipe, and what it
// injects into the writer prompt and the voice. Pure QtCore: the SpotGenerator feeds these functions, they never touch the network.
#include <QtTest>
#include <QJsonDocument>
#include <QRegularExpression>
#include "../playout/supervisor/areamarket.h"
#include "../playout/supervisor/localspots.h"

using namespace areamarket;

static QJsonObject recipe(const char* key, const char* source = "rss") { QJsonObject r; r["key"] = key; r["source"] = source; r["label"] = key; return r; }

class TestAreaMarket : public QObject {
    Q_OBJECT
private slots:
    void builtInMarketsAreComplete() {
        QCOMPARE(builtinKeys(), (QStringList{"martinsville", "reidsville", "danville"}));
        const struct { const char* key; const char* state; const char* county; const char* area; } want[] = {
            {"martinsville", "VA", "Henry County", "Martinsville and Henry County"},
            {"reidsville", "NC", "Rockingham County", "Reidsville and Rockingham County"},
            {"danville", "VA", "Pittsylvania County", "Danville and Pittsylvania County"}};
        for (const auto& w : want) {
            const QJsonObject m = find(w.key);
            QVERIFY2(!m.isEmpty(), w.key);
            QCOMPARE(m.value("state").toString(), QString(w.state));
            QCOMPARE(m.value("county").toString(), QString(w.county));
            QCOMPARE(m.value("area").toString(), QString(w.area));
            QVERIFY(!m.value("towns").toArray().isEmpty());
            QVERIFY(m.value("lat").toDouble() > 36.0 && m.value("lat").toDouble() < 37.0);
            QVERIFY(m.value("lon").toDouble() < -79.0 && m.value("lon").toDouble() > -80.5);
            QVERIFY(!m.value("voice").toObject().value("style").toString().isEmpty());
            QVERIFY(!m.value("news").toObject().value("feeds").toArray().isEmpty());
            QVERIFY(m.value("recipes").toObject().contains("traffic") && m.value("recipes").toObject().contains("sports_local"));
        }
        QCOMPARE(builtinMarkets().size(), 3);
    }
    void marketsDifferWhereTheyShould() {
        // coordinates, feeds, schools, traffic centre: no market borrows another's
        QSet<QString> feeds, schools; QSet<double> lats;
        for (const QJsonValue& v : builtinMarkets()) {
            const QJsonObject m = v.toObject();
            lats.insert(m.value("lat").toDouble());
            for (const QJsonValue& f : m.value("recipes").toObject().value("community").toObject().value("feeds").toArray()) feeds.insert(f.toString());
            for (const QJsonValue& s : m.value("recipes").toObject().value("sports_local").toObject().value("schools").toArray()) {
                const QString name = s.toObject().value("name").toString(); QVERIFY2(!schools.contains(name), qPrintable(name)); schools.insert(name);
                for (const QJsonValue& b : s.toObject().value("bases").toArray()) QVERIFY(b.toString().startsWith("https://www.maxpreps.com/" + m.value("state").toString().toLower() + "/"));      // the school's state
            }
        }
        QCOMPARE(lats.size(), 3);
        QVERIFY(feeds.contains("https://www.godanriver.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc"));
        QVERIFY(feeds.contains("https://myfox8.com/feed/"));
        QVERIFY(feeds.contains("https://martinsvillebulletin.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc"));
        QCOMPARE(schools.size(), 5 + 4 + 5);
    }
    void martinsvilleSchoolsMatchTheGeneratorDefaults() {
        // the Martinsville data is the old built-in list, so picking it explicitly changes wording, not the schools
        const QJsonArray def = localspots::defaultSchools(), mine = find("martinsville").value("recipes").toObject().value("sports_local").toObject().value("schools").toArray();
        QCOMPARE(QJsonDocument(mine).toJson(), QJsonDocument(def).toJson());
    }
    void keysAreNormalized() {
        QCOMPARE(normalizeKey(" Danville, VA "), QString("danville"));
        QCOMPARE(normalizeKey("REIDSVILLE"), QString("reidsville"));
        QCOMPARE(normalizeKey("Winston-Salem"), QString("winston_salem"));
        QVERIFY(find("Danville, VA").value("key").toString() == "danville");
        QVERIFY(find("nowhere").isEmpty());
        QVERIFY(find("").isEmpty());
    }
    void resolutionPrecedence() {
        // request > job > recipe override > station default; "default"/"" inherit, "none" = no market, unknown keys are ignored
        QCOMPARE(resolve("danville", "reidsville", "martinsville", "martinsville"), QString("danville"));
        QCOMPARE(resolve("", "reidsville", "martinsville", "martinsville"), QString("reidsville"));
        QCOMPARE(resolve("", "", "danville", "martinsville"), QString("danville"));
        QCOMPARE(resolve("", "", "", "reidsville"), QString("reidsville"));
        QCOMPARE(resolve("", "", "", ""), QString());
        QCOMPARE(resolve("default", "inherit", "", "danville"), QString("danville"));
        QCOMPARE(resolve("", "none", "danville", "danville"), QString());                   // the job opts out of the station default
        QCOMPARE(resolve("atlantis", "", "", "danville"), QString("danville"));            // unknown: fall through, never fail the spot
        QCOMPARE(resolve("Reidsville, NC", "", "", ""), QString("reidsville"));
    }
    void operatorCanAmendAndAddMarkets() {
        QJsonObject custom;
        custom["danville"] = QJsonObject{{"news", QJsonObject{{"feeds", QJsonArray{"https://example.test/feed"}}}}, {"voice", QJsonObject{{"style", "Custom style."}}},
                                         {"recipes", QJsonObject{{"cinema", QJsonObject{{"url", "https://cinema.example.test/"}}}}}};
        custom["winston-salem"] = QJsonObject{{"name", "Winston-Salem"}, {"state", "NC"}, {"county", "Forsyth County"}, {"towns", QJsonArray{"Kernersville"}}, {"lat", 36.0999}, {"lon", -80.2442}};
        custom["junk"] = QJsonObject{{"state", "XX"}};                                       // no name: not a market
        const QJsonObject d = find("danville", custom);
        QCOMPARE(d.value("news").toObject().value("feeds").toArray().at(0).toString(), QString("https://example.test/feed"));
        QCOMPARE(d.value("voice").toObject().value("style").toString(), QString("Custom style."));
        QVERIFY(!d.value("voice").toObject().value("pronunciations").toObject().isEmpty());     // fields the amendment did not name survive
        QCOMPARE(d.value("recipes").toObject().value("community").toObject().value("feeds").toArray().at(0).toString(), QString("https://example.test/feed"));      // the feed shorthand follows the amended feeds
        QCOMPARE(d.value("recipes").toObject().value("cinema").toObject().value("url").toString(), QString("https://cinema.example.test/"));
        QVERIFY(d.value("customized").toBool() && d.value("builtin").toBool());
        const QJsonObject w = find("winston-salem", custom);
        QCOMPARE(w.value("key").toString(), QString("winston_salem"));
        QCOMPARE(w.value("area").toString(), QString("Winston-Salem"));
        QVERIFY(w.value("news").toObject().value("keywords").toString().contains("kernersville"));
        QVERIFY(!w.value("builtin").toBool());
        QVERIFY(find("junk", custom).isEmpty());
        QCOMPARE(allMarkets(custom).size(), 4);
    }
    void fillPlaceholders() {
        const QJsonObject d = find("danville");
        QCOMPARE(fill("Top {area} stories", d), QString("Top Danville and Pittsylvania County stories"));
        QCOMPARE(fill("{PLACE} WEATHER", d), QString("DANVILLE WEATHER"));
        QCOMPARE(fill("{place}, {state} - {county} - {stateName}", d), QString("Danville, VA - Pittsylvania County - Virginia"));
        QCOMPARE(fill("from {station}", d, "WSTR"), QString("from WSTR"));
        QCOMPARE(fill("from {station}", QJsonObject()), QString("from your local station"));
        QCOMPARE(fill("{site} stays", d), QString("{site} stays"));                         // unknown placeholders are left for the caller
        QCOMPARE(fill("{area}", QJsonObject()), QString("{area}"));                         // no market: nothing to fill
        QVERIFY(fill("{towns}", d).startsWith("Danville, Gretna, Chatham and Ringgold"));
    }
    void localizeRecipePatchesTheRecipe() {
        const QJsonObject d = find("danville"), r = find("reidsville"), m = find("martinsville");
        // news recipes: the market's feeds, wording, series and the local filter
        const QJsonObject dc = localizeRecipe(recipe("community"), d);
        QCOMPARE(dc.value("market").toString(), QString("danville"));
        QCOMPARE(dc.value("intro").toString(), QString("In Danville and Pittsylvania County community news."));
        QVERIFY(dc.value("outro").toString().contains("{station}"));                       // the station is only known to the generator
        QCOMPARE(dc.value("series").toString(), QString("Danville Community Bulletin"));
        QCOMPARE(dc.value("feeds").toArray().at(0).toString(), QString("https://www.godanriver.com/search/?f=rss&t=article&c=news&l=25&s=start_time&sd=desc"));
        QVERIFY(dc.contains("localFilter"));
        const QJsonObject rs = localizeRecipe(recipe("sports"), r), ms = localizeRecipe(recipe("sports"), m);
        QCOMPARE(rs.value("intro").toString(), QString("Here's your Reidsville and Rockingham County sports update."));
        QCOMPARE(ms.value("intro").toString(), QString("Here's your Martinsville and Henry County sports update."));
        QVERIFY(rs.value("feeds").toArray().size() == 1 && rs.value("feeds").toArray().at(0).toString().contains("getstarnews"));
        // headlines read the Star News feed whatever the market; the intro names the area
        const QJsonObject dh = localizeRecipe(recipe("headlines"), d);
        QCOMPARE(dh.value("introText").toString(), QString("Here are the top Danville and Pittsylvania County stories at this hour from {site}."));
        QVERIFY(!dh.contains("feeds"));
        // weather: sign-off and series
        QCOMPARE(localizeRecipe(recipe("weather", "weather"), r).value("signOff").toString(), QString("Stay with us for Reidsville and Rockingham County weather updates."));
        QCOMPARE(localizeRecipe(recipe("weather_week", "weather"), d).value("series").toString(), QString("Danville Weather"));
        QVERIFY(!localizeRecipe(recipe("weather", "weather"), d).contains("localFilter"));      // weather is not a news feed
        // an empty market changes nothing
        QCOMPARE(QJsonDocument(localizeRecipe(recipe("community"), QJsonObject())).toJson(), QJsonDocument(recipe("community")).toJson());
    }
    void localSpotRecipesCarryTheirSources() {
        const QJsonObject d = find("danville"), r = find("reidsville"), m = find("martinsville");
        const QJsonObject dt = localizeRecipe(recipe("traffic", "traffic"), d), rt = localizeRecipe(recipe("traffic", "traffic"), r), mt = localizeRecipe(recipe("traffic", "traffic"), m);
        QCOMPARE(dt.value("provider").toString(), QString("vdot"));
        QCOMPARE(dt.value("area").toString(), QString("Pittsylvania County"));
        QVERIFY(qAbs(dt.value("lat").toDouble() - 36.586) < 0.01);
        QVERIFY(qAbs(mt.value("lat").toDouble() - 36.69) < 0.01);
        QCOMPARE(rt.value("provider").toString(), QString("drivenc"));                      // NC: a source this install has no key for
        QVERIFY(rt.value("outroText").toString().contains("drive N C"));
        // only Martinsville has a cinema; the others say "no source" (a blank url) instead of borrowing Hollywood Cinema
        QVERIFY(localizeRecipe(recipe("cinema", "cinema"), m).value("url").toString().contains("hollywoodcinema.net"));
        QVERIFY(localizeRecipe(recipe("cinema", "cinema"), d).contains("url") && localizeRecipe(recipe("cinema", "cinema"), d).value("url").toString().isEmpty());
        QVERIFY(localizeRecipe(recipe("realty", "realty"), r).value("url").toString().isEmpty());
        QVERIFY(!localizeRecipe(recipe("realty", "realty"), m).contains("url"));            // Haven More stays the recipe's own source
        // lakes are a Martinsville-area dataset; NC has no Virginia burning law
        QVERIFY(!localizeRecipe(recipe("weather_full", "weather_full"), d).value("segments").toArray().contains("lakes"));
        QVERIFY(localizeRecipe(recipe("weather_full", "weather_full"), r).contains("burnLaw") && !localizeRecipe(recipe("weather_full", "weather_full"), r).value("burnLaw").toBool(true));
        QVERIFY(!localizeRecipe(recipe("weather_full", "weather_full"), d).contains("burnLaw"));
    }
    void nationalRecipesAreNotNarrowedToTheMarket() {
        const QJsonObject d = find("danville");
        for (const char* k : {"economics", "tech_review"}) QVERIFY2(!localizeRecipe(recipe(k), d).contains("localFilter"), k);
        QVERIFY(!localizeRecipe(recipe("history", "history"), d).contains("localFilter"));
        QVERIFY(!localizeRecipe(recipe("markets_close", "markets"), d).contains("localFilter"));
        QJsonObject custom = recipe("my_feed"); custom["national"] = true;
        QVERIFY(!localizeRecipe(custom, d).contains("localFilter"));
        QVERIFY(localizeRecipe(recipe("my_feed"), d).contains("localFilter"));              // a custom local rss recipe is narrowed
    }
    void localFilterKeepsOnlyTheMarketsStories() {
        const QRegularExpression rx(localizeRecipe(recipe("headlines"), find("danville")).value("localFilter").toString(), QRegularExpression::CaseInsensitiveOption);
        QVERIFY(rx.match("Danville council approves budget").hasMatch());
        QVERIFY(rx.match("https://www.getstarnews.com/local/danville/story/anything/").hasMatch());
        QVERIFY(rx.match("Pittsylvania County schools close early").hasMatch());
        QVERIFY(rx.match("Gretna High School football wins").hasMatch());
        QVERIFY(!rx.match("Martinsville Speedway weekend").hasMatch());
        QVERIFY(!rx.match("Reidsville man charged").hasMatch());
        QVERIFY(mentionsMarket("Henry County Sheriff update", find("martinsville")));
        QVERIFY(!mentionsMarket("Henry County Sheriff update", find("danville")));
        QVERIFY(mentionsMarket("Mayodan town board meets", find("reidsville")));
        QVERIFY(!mentionsMarket("Madison Square Garden concert", find("reidsville")));       // bare "Madison" is not a Rockingham County match
        QVERIFY(mentionsMarket("Madison, N.C. water main break", find("reidsville")));
    }
    void pronunciationsReachOnlyTheNarration() {
        QCOMPARE(pronounce("Pittsylvania County schools and Danville", find("danville")), QString("Pit sill vane ya County schools and Danville"));
        QCOMPARE(pronounce("Reidsville's mayor spoke in Reidsville.", find("reidsville")), QString("Reedsville's mayor spoke in Reedsville."));
        QCOMPARE(pronounce("Martinsville Speedway", find("martinsville")), QString("Martinsville Speedway"));
        QCOMPARE(pronounce("Reidsville", QJsonObject()), QString("Reidsville"));
        QCOMPARE(pronounce("Pittsylvaniac", find("danville")), QString("Pittsylvaniac"));    // whole words only
    }
    void promptContextNamesTheMarketAndExcludesTheOthers() {
        const QString d = promptContext(find("danville")), r = promptContext(find("reidsville")), m = promptContext(find("martinsville"));
        QVERIFY(d.contains("Danville, Virginia") && d.contains("Pittsylvania County") && d.contains("Dan River Region"));
        QVERIFY(d.contains("Never name Martinsville, Henry County, Reidsville, Rockingham County"));
        QVERIFY(r.contains("Reidsville, North Carolina") && r.contains("Rockingham County") && r.contains("Mayodan"));
        QVERIFY(m.contains("Martinsville, Virginia") && m.contains("Henry County") && !m.contains("Never name Martinsville"));
        QVERIFY(d != r && r != m);
        QVERIFY(d.contains("That's your Danville and Pittsylvania County update."));
        QVERIFY(!d.contains('\n'));
        QCOMPARE(promptContext(QJsonObject()), QString());
        // a market with no avoid list names the others as "not for them"
        QJsonObject custom; custom["winston-salem"] = QJsonObject{{"name", "Winston-Salem"}, {"state", "NC"}, {"stateName", "North Carolina"}, {"county", "Forsyth County"}};
        const QString w = promptContext(find("winston-salem", custom), custom);
        QVERIFY(w.contains("Martinsville") && w.contains("do not name them"));
    }
    void injectionReachesTheLocalScripts() {
        // the localized recipe is the options object of the scene scripts: place names, county and outro change with the market, and the generic wording is unchanged
        const QDateTime now(QDate(2026, 10, 23), QTime(16, 13), QTimeZone("America/New_York"));
        auto traffic = [&](const QJsonObject& opts) { return localspots::trafficScript(QJsonArray(), true, now, opts).value("narration").toString(); };
        QVERIFY(traffic(localizeRecipe(recipe("traffic", "traffic"), find("danville"))).contains("near Danville and Pittsylvania County. Drive safely."));
        QVERIFY(traffic(localizeRecipe(recipe("traffic", "traffic"), find("martinsville"))).contains("near Martinsville and Henry County. Drive safely."));
        QVERIFY(traffic(recipe("traffic", "traffic")).contains("near Martinsville and Henry County. Drive safely."));              // no market: the old words
        // sports: the area names the update; a market's schools are what the generator fetches
        QJsonObject game; game["school"] = "Gretna"; game["sport"] = "Girls Volleyball"; game["opponent"] = "Dan River"; game["startLocal"] = "2026-10-22T18:00:00"; game["final"] = true;
        game["ourScore"] = 3; game["theirScore"] = 0; game["result"] = "W"; game["home"] = 1; game["neutral"] = false; game["tbd"] = false; game["kind"] = "";
        const QString d = localspots::sportsScript(QJsonArray{game}, now, localizeRecipe(recipe("sports_local", "sports"), find("danville"))).value("narration").toString();
        QVERIFY2(d.contains("Here's your Danville and Pittsylvania County sports update"), qPrintable(d));
        QVERIFY(d.contains("That's your Danville and Pittsylvania County sports update."));
        QVERIFY(localspots::sportsScript(QJsonArray{game}, now, QJsonObject()).value("narration").toString().contains("Here's your local sports update"));
        // the Reidsville and Danville recipes name MaxPreps pages in their own state
        for (const char* k : {"reidsville", "danville"}) {
            const QJsonObject m = find(k); const QString st = m.value("state").toString().toLower();
            for (const QJsonValue& sc : localizeRecipe(recipe("sports_local", "sports"), m).value("schools").toArray()) QVERIFY(sc.toObject().value("bases").toArray().at(0).toString().contains("/" + st + "/"));
        }
    }
    void recordMetaAndSummaryForTheUi() {
        const QJsonObject d = find("danville");
        const QJsonObject meta = recordMeta(d);
        QCOMPARE(meta.value("key").toString(), QString("danville"));
        QCOMPARE(meta.value("label").toString(), QString("Danville, VA"));
        QCOMPARE(meta.value("county").toString(), QString("Pittsylvania County"));
        QVERIFY(recordMeta(QJsonObject()).isEmpty());
        const QJsonObject w = weatherOf(d);
        QCOMPARE(w.value("place").toString(), QString("Danville"));
        QVERIFY(qAbs(w.value("lat").toDouble() - 36.586) < 0.001 && qAbs(w.value("lon").toDouble() + 79.395) < 0.001);
        QCOMPARE(w.value("nwsOffice").toString(), QString("RNK"));
        QVERIFY(weatherOf(QJsonObject()).isEmpty());
        const QJsonObject s = summary(d), sr = summary(find("reidsville"));
        QVERIFY(s.value("sources").toObject().value("trafficConfigured").toBool());
        QVERIFY(!sr.value("sources").toObject().value("trafficConfigured").toBool());
        QCOMPARE(s.value("sources").toObject().value("schools").toArray().size(), 5);
        QCOMPARE(s.value("sources").toObject().value("cinema").toString(), QString());
        QCOMPARE(summary(find("martinsville")).value("sources").toObject().value("cinema").toString(), QString("https://hollywoodcinema.net/"));
        QVERIFY(s.value("context").toString().contains("Danville"));
        QVERIFY(s.value("builtin").toBool());
    }
};

QTEST_APPLESS_MAIN(TestAreaMarket)
#include "test_areamarket.moc"
