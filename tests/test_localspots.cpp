// Local spots: cinema showtimes, 511 traffic, MaxPreps scores and the featured-home rotation. Fixtures are cut from the real pages.
#include <QtTest>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimeZone>
#include "../playout/supervisor/localspots.h"

using namespace localspots;

static QDateTime at(int y, int mo, int d, int h, int mi) { return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone("America/New_York")); }
static QStringList narrations(const QJsonObject& script) { QStringList o; for (const QJsonValue& v : script.value("scenes").toArray()) o << v.toObject().value("narration").toString(); return o; }
static QStringList kinds(const QJsonObject& script) { QStringList o; for (const QJsonValue& v : script.value("scenes").toArray()) o << v.toObject().value("kind").toString(); return o; }

// ---- cinema fixture: the markup hollywoodcinema.net serves (two movies, blank rating on the first, one with a Saturday-only show)
static QString cinemaHtml() {
    auto screening = [](const char* day, std::initializer_list<const char*> times) {
        QString s = QString("<div itemscope itemtype=\"https://schema.org/ScreeningEvent\"><meta itemprop=\"name\" content=\"x - %1\"><span itemprop=\"dayOfWeek\">%1</span>: ").arg(day);
        for (const char* t : times) s += QString("<span itemprop=\"startDate\" content=\"2026-10-23T%1\"> %1, </span>").arg(t);
        return s + "</div>";
    };
    auto movie = [&](const char* pos, const char* title, const char* poster, const char* rating, const char* runtime, const char* desc, const QString& days) {
        return QString("<div class=\"movie\" itemprop=\"itemListElement\" itemscope itemtype=\"https://schema.org/ListItem\"><meta itemprop=\"position\" content=\"%1\">"
                       "<div itemscope itemtype=\"https://schema.org/Movie\"><img src=\"%3\" alt=\"poster\" itemprop=\"image\"><meta itemprop=\"name\" content=\"%2\">"
                       "<div class=\"movie-info\"><span itemprop=\"contentRating\"><i class=\"fas fa-star\"></i> %4</span><span itemprop=\"duration\"><i></i> %5</span></div>"
                       "<div class=\"movie-description\"><p itemprop=\"description\">%6</p></div><div class=\"movie-metadata\"><span itemprop=\"datePublished\">October 19, 2026</span></div>"
                       "<div class=\"showtimes-container\"><h3 itemprop=\"name\">%2</h3><div class=\"showtimes\">%7</div></div></div></div>")
            .arg(pos, title, poster, rating, runtime, desc, days);
    };
    const QString week = screening("Wednesday", {"05:00 PM", "07:00 PM"}) + screening("Thursday", {"05:00 PM", "07:00 PM"}) + screening("Friday", {"05:00 PM", "07:00 PM"})
                         + screening("Saturday", {"01:00 PM", "03:00 PM", "05:00 PM", "07:00 PM", "09:00 PM"}) + screening("Sunday", {"01:00 PM", "03:00 PM", "05:00 PM", "07:00 PM"});
    const QString m1 = movie("1", "Clayface", "https://image.tmdb.org/t/p/w500/9l1NuFjel2j89hiuiJbsxfblqjy.jpg", "", "1h 49m",
                             "Matt Hagen&#039;s meteoric rise from Gotham street kid to Hollywood leading man is cut short. Abandoned by his team, he seeks a treatment.", week);
    const QString m2 = movie("2", "Whalefall", "https://image.tmdb.org/t/p/w500/abc.jpg", "PG-13", "1h 46m", "A diver is swallowed by a sperm whale.", week);
    const QString m3 = movie("3", "Old Show", "https://image.tmdb.org/t/p/w500/old.jpg", "R", "1h 33m", "Gone by now.", screening("Wednesday", {"05:00 PM"}) + screening("Thursday", {"05:00 PM"}));
    return "<html><body><main><div class=\"movies\" itemscope itemtype=\"https://schema.org/ItemList\">" + m1 + m2 + m3 + "</div></main>"
           "<footer itemscope itemtype=\"https://schema.org/MovieTheater\"><div class=\"address-info\" itemprop=\"address\" itemscope itemtype=\"https://schema.org/PostalAddress\">"
           "<span itemprop=\"streetAddress\">909 Commonwealth Blvd</span>, <span itemprop=\"addressLocality\">Martinsville</span>, <span itemprop=\"addressRegion\">VA</span> "
           "<span itemprop=\"postalCode\">24112</span><br>\n            Beside Wal-Mart\n        </div>"
           "<div class=\"ticket-info\" itemprop=\"priceRange\">Group Rate Available - All Tickets $5 except 9PM shows $8</div></footer></body></html>";
}

// ---- 511 GeoJSON, as the incidents / weatherClosures layers return it
static QByteArray geo(std::initializer_list<QJsonObject> features) {
    QJsonArray a; for (const QJsonObject& f : features) a.append(f);
    QJsonObject o; o["type"] = "FeatureCollection"; o["features"] = a; return QJsonDocument(o).toJson();
}
static QJsonObject feature(const char* id, const char* route, const char* type, const char* location, const char* message, double lon, double lat) {
    QJsonObject p; p["id"] = id; p["priority"] = "Minor"; p["route"] = route; p["type"] = type; p["location"] = location; p["message_511"] = message;
    QJsonObject g; g["type"] = "Point"; g["coordinates"] = QJsonArray{lon, lat};
    QJsonObject f; f["type"] = "Feature"; f["id"] = id; f["properties"] = p; f["geometry"] = g; return f;
}
static const char* kPatrick = "On Rt. 672 in Patrick County, in the vicinity of Ararat Hwy, Rte 773, motorists can expect delays due to weather. All north lanes are closed. All south lanes are closed. Crews are assessing the bridge and the estimated repair for this closure will be updated as information becomes available.";

// ---- MaxPreps: a team page whose __NEXT_DATA__ holds contests as index-addressed arrays
static QJsonArray participant(const QString& teamId, const QString& name, const QJsonValue& score, const QString& result) {
    QJsonArray p; for (int i = 0; i < 16; ++i) p.append(QJsonValue()); p[1] = teamId; p[3] = result; p[6] = score; p[14] = name; return p;
}
static QJsonArray contest(const QString& start, const QString& ourId, const QString& ourName, const QString& oppName, const QJsonValue& ourScore, const QJsonValue& oppScore, const QString& result, const QString& sentence) {
    QJsonArray c; for (int i = 0; i < 41; ++i) c.append(QJsonValue()); c[11] = start; c[29] = sentence;
    c[37] = participant(ourId, ourName, ourScore, result); c[38] = participant("opp-" + oppName, oppName, oppScore, QString());
    return c;
}
static QByteArray teamPage(const QString& school, const QString& state, const QString& year, const QJsonArray& contests) {
    QJsonObject data; data["teamId"] = "team-" + school; data["schoolName"] = school; data["stateCode"] = state; data["year"] = year;
    QJsonObject tc; tc["data"] = data; QJsonObject pp; pp["teamContext"] = tc; pp["contests"] = contests;
    QJsonObject props; props["pageProps"] = pp; QJsonObject root; root["props"] = props;
    return "<html><head><script id=\"__NEXT_DATA__\" type=\"application/json\" crossorigin=\"anonymous\">" + QJsonDocument(root).toJson(QJsonDocument::Compact) + "</script></head></html>";
}

// ---- Showcase IDX: listing objects embedded in a script
static QJsonObject listing(const char* mls, const char* address, const char* city, double price, const char* status, int photos, const char* firstSeen, const char* description = "") {
    QJsonArray imgs; for (int i = 0; i < photos; ++i) imgs.append(QString("https://photos.example.net/1874/%1/%2.jpeg?d=s").arg(mls).arg(1000 + i));
    QJsonObject o; o["orig_mls_number"] = mls; o["address"] = address; o["city"] = city; o["state"] = "VA"; o["zip"] = "24112"; o["county"] = "Henry"; o["price"] = price; o["status"] = status;
    o["bedrooms"] = 3; o["bathrooms"] = 2; o["halfbaths"] = QJsonValue(); o["sqft"] = 1548; o["acres"] = 0.71; o["year_built"] = 1995; o["images"] = imgs; o["images_total"] = photos; o["first_seen_dts"] = firstSeen;
    o["listing_agent"] = "Pat Example"; o["listing_agent_phone"] = "540-555-0100"; o["listing_office"] = "KW Example Realty"; o["description"] = description; o["subdivision"] = "Anytown - Southside";
    return o;
}
static const char* kDesc = "Welcome to 100 Example Road in Anytown! This home offers approximately 1,548 square feet of one-level living and an easy commute. "
    "A covered front porch is a pleasant spot to sit and watch the seasons change, while the back deck has room for grilling, entertaining, or simply unwinding. "
    "The open living room and kitchen share a bright dining area made for cooking and company. The laundry is on the main floor, so there are no stairs to carry baskets! "
    "The primary bedroom has two closets and a private bath with a walk-in shower. Perfect for a young family! "
    "Close to shopping, dining, medical offices, and other amenities around the county. Taxes to be determined on new construction. Lot size and sq ft are estimated. Schedule your showing today!";
static QString idxPage(const QJsonArray& listings) {
    QJsonObject wrap; wrap["listings"] = listings; wrap["note"] = "{braces } inside strings \" are fine";
    return "<html><script>window.__idx=" + QString::fromUtf8(QJsonDocument(wrap).toJson(QJsonDocument::Compact)) + ";</script><div>other</div></html>";
}

class TestLocalSpots : public QObject {
    Q_OBJECT
private slots:
    void speechForms() {
        QCOMPARE(spokenNumber(159900), QString("one hundred fifty nine thousand nine hundred"));
        QCOMPARE(spokenNumber(1548), QString("one thousand five hundred forty eight"));
        QCOMPARE(spokenNumber(75000), QString("seventy five thousand"));
        QCOMPARE(spokenClock(QTime(17, 0)), QString("five P M"));
        QCOMPARE(spokenClock(QTime(9, 5)), QString("nine oh five A M"));
        QCOMPARE(spokenClock(QTime(0, 30)), QString("twelve thirty A M"));
        QCOMPARE(spokenTimeList({QTime(13, 0), QTime(15, 0), QTime(17, 0), QTime(19, 0), QTime(21, 0)}), QString("one, three, five, seven and nine P M"));
        QCOMPARE(spokenTimeList({QTime(11, 0), QTime(13, 0)}), QString("eleven A M and one P M"));
        QCOMPARE(displayTimeList({QTime(13, 0), QTime(15, 30), QTime(19, 0)}), QString::fromUtf8("1:00 \xC2\xB7 3:30 \xC2\xB7 7:00 PM"));
        QCOMPARE(spokenPhone("540-555-0100"), QString("five four zero, five five five, zero one zero zero"));
        QCOMPARE(spokenPhone("5405550100"), QString("five four zero, five five five, zero one zero zero"));
        QCOMPARE(spokenAddress("39 Example Dr"), QString("39 Example Drive"));
        QCOMPARE(spokenAddress("101 Sample Lake Rd"), QString("101 Sample Lake Road"));
        QCOMPARE(spokenAddress("404 Sample Street"), QString("404 Sample Street"));
        QCOMPARE(spokenRuntime("1h 49m"), QString("one hour forty nine minutes"));
        QCOMPARE(spokenRuntime("2h 0m"), QString("two hours"));
        QCOMPARE(speech("Tickets are $5 &amp; popcorn is $12."), QString("Tickets are five dollars and popcorn is twelve dollars."));
        QCOMPARE(speech("Listed at $159,900"), QString("Listed at one hundred fifty nine thousand nine hundred dollars"));
        QCOMPARE(spokenRoad("On I-81 North at mile marker 80.5, Rt. 672, Rte 773, US-19 and VA-57"), QString("On Interstate 81 North at mile marker 80.5, Route 672, Route 773, U S 19 and Virginia 57"));
        // Martinsville to the Patrick County closure is about 38 miles; to Richmond about 170
        const double mi = milesBetween(36.69, -79.87, 36.581908, -80.539848);
        QVERIFY(mi > 36 && mi < 40);
        QVERIFY(milesBetween(36.69, -79.87, 37.54, -77.43) > 140);
    }

    void cinemaPageParses() {
        const QJsonObject page = parseCinema(cinemaHtml());
        const QJsonArray movies = page.value("movies").toArray();
        QCOMPARE(movies.size(), 3);
        const QJsonObject m = movies[0].toObject();
        QCOMPARE(m.value("title").toString(), QString("Clayface"));
        QCOMPARE(m.value("rating").toString(), QString());
        QCOMPARE(m.value("runtime").toString(), QString("1h 49m"));
        QVERIFY(m.value("description").toString().startsWith("Matt Hagen's meteoric rise"));               // entity decoded
        QCOMPARE(m.value("poster").toString(), QString("https://image.tmdb.org/t/p/w780/9l1NuFjel2j89hiuiJbsxfblqjy.jpg"));   // larger rendition
        QCOMPARE(movies[1].toObject().value("rating").toString(), QString("PG-13"));
        const QJsonArray days = m.value("days").toArray();
        QCOMPARE(days.size(), 5);
        QCOMPARE(days[2].toObject().value("day").toString(), QString("Friday"));
        QCOMPARE(days[2].toObject().value("times").toArray(), (QJsonArray{"17:00", "19:00"}));
        QCOMPARE(days[3].toObject().value("times").toArray(), (QJsonArray{"13:00", "15:00", "17:00", "19:00", "21:00"}));
        QCOMPARE(page.value("address").toString(), QString("909 Commonwealth Blvd"));
        QCOMPARE(page.value("landmark").toString(), QString("Beside Wal-Mart"));
        const QJsonObject price = page.value("price").toObject();
        QCOMPARE(price.value("base").toInt(), 5); QCOMPARE(price.value("exceptHour").toInt(), 9); QCOMPARE(price.value("exceptPrice").toInt(), 8);
    }

    void cinemaScriptOffersOnlyUpcomingShows() {
        QJsonObject opts; opts["siteName"] = "Hollywood Cinema"; opts["place"] = "Martinsville"; opts["site"] = "hollywoodcinema.net";
        // Friday 6:30 PM (2026-10-23): the listed week is Wed-Sun, so Wed / Thu are already over. Friday's 5:00 is over, the 7:00 is next.
        // "Old Show" plays Wed / Thu only: it is gone.
        const QJsonObject s = cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 23, 18, 30), opts);
        QVERIFY(!s.contains("skip") && !s.contains("error"));
        QCOMPARE(kinds(s), (QStringList{"intro", "story", "story", "outro"}));
        const QJsonArray sc = s.value("scenes").toArray();
        const QJsonObject clay = sc[1].toObject();
        QCOMPARE(clay.value("title").toString(), QString("Clayface"));
        QCOMPARE(clay.value("detail").toString(), QString("7:00 PM"));                              // only the 7:00 is still ahead
        QVERIFY(clay.value("narration").toString().contains("Showtimes today are seven P M."));
        QVERIFY(clay.value("narration").toString().startsWith("Clayface runs one hour forty nine minutes."));      // no "Clayface, runs" for a film without a rating
        QVERIFY(!clay.value("narration").toString().contains("rated"));                             // no rating on the site
        QCOMPARE(clay.value("image").toString(), QString("https://image.tmdb.org/t/p/w780/9l1NuFjel2j89hiuiJbsxfblqjy.jpg"));
        QCOMPARE(clay.value("counter").toString(), QString("1 / 2"));
        QCOMPARE(clay.value("place").toString(), QString::fromUtf8("TODAY \xC2\xB7 1H 49M"));
        const QJsonObject whale = sc[2].toObject();
        QCOMPARE(whale.value("title").toString(), QString("Whalefall"));
        QVERIFY(whale.value("narration").toString().contains("rated P G thirteen"));
        QVERIFY(!narrations(s).join(' ').contains("Old Show"));
        QVERIFY(sc[0].toObject().value("narration").toString().contains("Here's what's showing today."));
        QCOMPARE(sc[0].toObject().value("rundown").toArray().size(), 2);
        const QString outro = sc.last().toObject().value("narration").toString();
        QVERIFY(outro.contains("Hollywood Cinema is at 909 Commonwealth Boulevard, beside Wal-Mart."));
        QVERIFY(outro.contains("All tickets are five dollars, except nine P M shows, which are eight dollars."));
        QVERIFY(outro.contains("Buy tickets online at Hollywood Cinema dot net."));
        QCOMPARE(sc.last().toObject().value("big").toString(), QString("hollywoodcinema.net"));
        // Saturday 9:55 PM: the day is over; Sunday's shows are "tomorrow"
        const QJsonObject sat = cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 24, 21, 55), opts);
        QVERIFY(narrations(sat)[1].contains("Showtimes tomorrow are one, three, five and seven P M."));
        QVERIFY(narrations(sat)[0].contains("Here's what's showing next."));
        // a show that starts inside the 10-minute lead is not offered (8:55 PM vs the 9:00: Sunday's shows are next), one 15 minutes out is (8:45 PM)
        QVERIFY(narrations(cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 24, 20, 55), opts))[1].contains("Showtimes tomorrow"));
        QCOMPARE(cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 24, 20, 45), opts).value("scenes").toArray()[1].toObject().value("detail").toString(), QString("9:00 PM"));
        // Sunday 8:30 PM: the listed week has nothing left: nothing airs
        QVERIFY(cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 25, 20, 30), opts).contains("skip"));
        // Monday: the whole listed week is behind us, so the page is next week's schedule and Wednesday is next
        const QJsonObject mon = cinemaScript(parseCinema(cinemaHtml()), at(2026, 10, 26, 12, 0), opts);
        QVERIFY(narrations(mon)[1].contains("Showtimes on Wednesday are five and seven P M."));
        QVERIFY(narrations(mon).join(' ').contains("Old Show"));
    }

    void incidentsFilterAndRank() {
        const QByteArray layer1 = geo({feature("WX1", "Rt. 672N (Patrick County)", "Closed", "Ararat Hwy , Rte 773", kPatrick, -80.539848, 36.581908),
                                       feature("WX2", "Rt. 738S (Campbell County)", "Closed", "Sunnymeade Rd , Rte 677", "On Rt. 738 in Campbell County, motorists can expect delays due to weather. All south lanes are closed.", -79.162568, 37.30216)});
        const QByteArray layer2 = geo({feature("IN1", "US-220N", "Disabled Vehicle", "Near Bassett", "On US-220 North at mile marker 4.1 in Henry County, motorists can expect delays due to a disabled vehicle. The north right shoulder is closed.", -79.99, 36.77),
                                       feature("IN2", "I-81N", "Tractor Trailer Accident", "Max Meadows", "On I-81 at mile marker 80.5 in Wythe County, motorists can expect delays due to a tractor trailer crash.", -80.943944, 36.946348)});
        QJsonArray all = parseIncidents(layer1, "weatherClosures"); for (const QJsonValue& v : parseIncidents(layer2, "incidents")) all.append(v);
        QCOMPARE(all.size(), 4);
        QCOMPARE(all[0].toObject().value("layer").toString(), QString("weatherClosures"));
        QVERIFY(qAbs(all[0].toObject().value("lat").toDouble() - 36.581908) < 1e-6);
        QJsonObject opts; opts["place"] = "Martinsville";
        const QJsonObject s = trafficScript(all, true, at(2026, 10, 23, 14, 5), opts);
        QVERIFY(!s.contains("error") && !s.contains("skip"));
        QCOMPARE(kinds(s), (QStringList{"intro", "story", "story", "outro"}));                 // Campbell (52 mi) and Wythe (60 mi) are outside 45 miles
        const QJsonArray sc = s.value("scenes").toArray();
        QCOMPARE(sc[1].toObject().value("title").toString(), QString("Route 672 North"));       // the road closure outranks the shoulder
        QCOMPARE(sc[1].toObject().value("detail").toString(), QString("ROAD CLOSED"));
        QCOMPARE(sc[1].toObject().value("place").toString(), QString("PATRICK COUNTY"));
        QCOMPARE(sc[1].toObject().value("category").toString(), QString("CLOSED"));
        QCOMPARE(sc[1].toObject().value("summary").toString(), QString("Near Ararat Hwy, Rte 773. All north lanes are closed. All south lanes are closed."));
        const QString nar = sc[1].toObject().value("narration").toString();
        QVERIFY(nar.startsWith("On Route 672 in Patrick County"));                              // "Rt." did not split the sentence
        QVERIFY(nar.contains("Ararat Highway, Route 773"));
        QVERIFY(nar.contains("All north lanes are closed. All south lanes are closed."));
        QVERIFY(!nar.contains("information becomes available"));                               // cut at the word budget on a sentence boundary
        QCOMPARE(sc[2].toObject().value("title").toString(), QString("US 220 North"));
        QCOMPARE(sc[2].toObject().value("detail").toString(), QString("SHOULDER CLOSED"));
        QVERIFY(sc[0].toObject().value("narration").toString().contains("two oh five P M"));
        QCOMPARE(sc[0].toObject().value("rundown").toArray().size(), 2);
        QVERIFY(sc.last().toObject().value("narration").toString().contains("five eleven virginia dot org"));
    }

    void trafficAllClearAndDeadFeed() {
        const QByteArray far = geo({feature("IN2", "I-81N", "Tractor Trailer Accident", "Max Meadows", "On I-81 at mile marker 80.5 in Wythe County, motorists can expect delays due to a tractor trailer crash.", -80.943944, 36.946348)});
        const QJsonObject clear = trafficScript(parseIncidents(far, "incidents"), true, at(2026, 10, 23, 14, 5), QJsonObject());
        QCOMPARE(kinds(clear), (QStringList{"story"}));
        const QJsonObject sc = clear.value("scenes").toArray()[0].toObject();
        QCOMPARE(sc.value("category").toString(), QString("ALL CLEAR"));
        QCOMPARE(sc.value("counter").toString(), QString());
        QVERIFY(sc.value("narration").toString().contains("reports no crashes, closures or weather closures near Martinsville"));
        // a feed that could not be read is never reported as clear roads
        const QJsonObject dead = trafficScript(QJsonArray(), false, at(2026, 10, 23, 14, 5), QJsonObject());
        QVERIFY(dead.contains("error") && !dead.contains("scenes"));
        QVERIFY(parseIncidents("not json", "incidents").isEmpty());
    }

    void teamPagesParse() {
        const QString id = "team-Martinsville";
        const QJsonArray cs{
            contest("2026-10-16T19:00:00", id, "Martinsville", "Tunstall", 59, 41, "W 59-41", "On 10/16, the Tunstall varsity football team lost their away game at Martinsville."),
            contest("2026-10-23T19:00:00", id, "Martinsville", "Bassett", QJsonValue(), QJsonValue(), "", "The Martinsville varsity football team has an away non-conference game @ Bassett."),
            contest("2026-10-30T00:00:00", id, "Martinsville", "Carroll County", QJsonValue(), QJsonValue(), "", "The Carroll County varsity football team has an away conference game @ Martinsville."),
            contest("2026-11-06T19:00:00", id, "Martinsville", "Unknown Opponent", QJsonValue(), QJsonValue(), "", "x")};
        const QByteArray html = teamPage("Martinsville", "VA", "26-27", cs);
        const QJsonArray g = parseTeamSchedule(html, "Martinsville", "Football", "VA", "26-27");
        QCOMPARE(g.size(), 3);
        const QJsonObject final = g[0].toObject();
        QVERIFY(final.value("final").toBool()); QCOMPARE(final.value("ourScore").toInt(), 59); QCOMPARE(final.value("theirScore").toInt(), 41); QCOMPARE(final.value("result").toString(), QString("W"));
        QCOMPARE(final.value("home").toInt(), 1);                                               // the visiting team lost "their away game": we were home
        const QJsonObject next = g[1].toObject();
        QVERIFY(!next.value("final").toBool()); QCOMPARE(next.value("home").toInt(), 0); QCOMPARE(next.value("kind").toString(), QString("non-conference")); QCOMPARE(next.value("opponent").toString(), QString("Bassett"));
        QCOMPARE(g[2].toObject().value("home").toInt(), 1); QVERIFY(g[2].toObject().value("tbd").toBool());
        // other season, other state, or a different school's page (MaxPreps redirects unknown slugs) all yield nothing
        QVERIFY(parseTeamSchedule(html, "Martinsville", "Football", "VA", "25-26").isEmpty());
        QVERIFY(parseTeamSchedule(html, "Martinsville", "Football", "NC", "26-27").isEmpty());
        QVERIFY(parseTeamSchedule(html, "Magna Vista", "Football", "VA", "26-27").isEmpty());
        QVERIFY(parseTeamSchedule("<html>no data</html>", "Martinsville", "Football", "VA", "26-27").isEmpty());
        QCOMPARE(activeSeasonLabel(QDate(2026, 10, 23)), QString("26-27"));
        QCOMPARE(activeSeasonLabel(QDate(2027, 3, 1)), QString("26-27"));
        QCOMPARE(activeSeasonLabel(QDate(2027, 7, 1)), QString("27-28"));
    }

    void sportsScriptWinsLossesAndNext48() {
        auto g = [](const char* school, const char* opp, const char* start, bool fin, int mine, int theirs, const char* res, int home, const char* kind = "") {
            QJsonObject o; o["school"] = school; o["sport"] = "Football"; o["opponent"] = opp; o["startLocal"] = start; o["final"] = fin; o["home"] = home; o["neutral"] = false; o["tbd"] = false; o["kind"] = kind;
            if (fin) { o["ourScore"] = mine; o["theirScore"] = theirs; o["result"] = res; }
            return o;
        };
        QJsonArray games{
            g("Martinsville", "Tunstall", "2026-10-20T19:00:00", true, 59, 41, "W", 1),
            g("Bassett", "Magna Vista", "2026-10-21T19:00:00", true, 20, 54, "L", 1),          // two local schools: reported once, winner first
            g("Magna Vista", "Bassett", "2026-10-21T19:00:00", true, 54, 20, "W", 0),
            g("Martinsville", "Floyd County", "2026-10-09T19:00:00", true, 13, 24, "L", 0),    // too old (7+ days)
            g("Patrick County", "Dan River", "2026-10-22T19:00:00", true, 7, 28, "L", 1),
            g("Martinsville", "Bassett", "2026-10-23T19:00:00", false, 0, 0, "", 0, "non-conference"),
            g("Magna Vista", "Tunstall", "2026-10-24T19:00:00", false, 0, 0, "", 1, "conference"),
            g("Carlisle", "Faith Christian", "2026-10-26T19:00:00", false, 0, 0, "", 1)};      // 3+ days out: beyond 48 hours
        QJsonObject opts; opts["siteName"] = "GetStarNews"; opts["site"] = "getstarnews.com";
        const QJsonObject s = sportsScript(games, at(2026, 10, 23, 12, 0), opts);
        QVERIFY(!s.contains("skip") && !s.contains("error"));
        QCOMPARE(kinds(s), (QStringList{"intro", "story", "story", "story", "story", "story", "outro"}));   // 3 finals, 2 upcoming
        const QJsonArray sc = s.value("scenes").toArray();
        QCOMPARE(sc[1].toObject().value("title").toString(), QString("Dan River 28, Patrick County 7"));   // newest first; a loss shows the winner first
        QCOMPARE(sc[1].toObject().value("detail").toString(), QString("LOSS"));
        QCOMPARE(sc[2].toObject().value("title").toString(), QString("Magna Vista 54, Bassett 20"));
        QCOMPARE(sc[2].toObject().value("detail").toString(), QString("FINAL"));
        QVERIFY(sc[2].toObject().value("narration").toString().contains("Magna Vista beat Bassett in football, fifty four to twenty."));
        QCOMPARE(sc[3].toObject().value("title").toString(), QString("Martinsville 59, Tunstall 41"));
        QVERIFY(sc[3].toObject().value("narration").toString().startsWith("On Tuesday, Martinsville beat Tunstall in football, fifty nine to forty one."));
        QVERIFY(sc[1].toObject().value("narration").toString().startsWith("Last night, Patrick County fell to Dan River in football, twenty eight to seven."));
        QCOMPARE(sc[4].toObject().value("title").toString(), QString("Martinsville at Bassett"));
        QCOMPARE(sc[4].toObject().value("category").toString(), QString::fromUtf8("TONIGHT \xC2\xB7 FOOTBALL"));
        QCOMPARE(sc[4].toObject().value("detail").toString(), QString("7:00 PM"));
        QVERIFY(sc[4].toObject().value("narration").toString().startsWith("Tonight at seven P M: Martinsville travels to Bassett in football."));
        QVERIFY(sc[4].toObject().value("summary").toString().contains("Non-conference game"));
        QVERIFY(sc[5].toObject().value("narration").toString().startsWith("Tomorrow night at seven P M: Magna Vista hosts Tunstall in football."));
        QCOMPARE(sc[5].toObject().value("title").toString(), QString("Magna Vista vs. Tunstall"));
        QVERIFY(sc[0].toObject().value("narration").toString().contains("Three recent results, and two games coming up in the next two days."));
        QVERIFY(sc.last().toObject().value("narration").toString().contains("Get Star News dot com"));
        // nothing to report: a quiet week
        QVERIFY(sportsScript(QJsonArray{g("Martinsville", "Floyd County", "2026-10-09T19:00:00", true, 13, 24, "L", 0)}, at(2026, 10, 23, 12, 0), opts).contains("skip"));
        // a game that started 5 hours ago with no score is over, not "upcoming"
        QVERIFY(sportsScript(QJsonArray{g("Martinsville", "Bassett", "2026-10-23T07:00:00", false, 0, 0, "", 0)}, at(2026, 10, 23, 12, 30), opts).contains("skip"));
    }

    void listingsParseAndRotate() {
        const QJsonArray raw{listing("900126", "101 Sample Lake Rd", "Martinsville", 159900, "Active", 29, "2026-10-21T17:43:48+00:00"),
                             listing("900115", "202 Sample Place", "Martinsville", 249900, "Active", 38, "2026-10-16T04:13:49+00:00"),
                             listing("900108", "303 Sample Street", "Martinsville", 75000, "Pending", 16, "2026-10-12T00:00:00+00:00"),
                             listing("900099", "9 Short Photo Ln", "Bassett", 99000, "Active", 2, "2026-10-12T00:00:00+00:00"),
                             listing("900076", "404 Sample Street", "Martinsville", 119900, "Active Under Contract", 35, "2026-10-07T11:18:33+00:00"),
                             listing("900075", "505 Sample Creek Dr", "Bassett", 339900, "Active", 61, "2026-10-06T19:37:49+00:00")};
        QJsonArray doubled = raw; for (const QJsonValue& v : raw) doubled.append(v);                  // the page repeats each listing (hot sheet + map)
        const QJsonArray ls = parseListings(idxPage(doubled));
        QCOMPARE(ls.size(), 6);
        const QJsonObject first = ls[0].toObject();
        QCOMPARE(first.value("mls").toString(), QString("900126")); QCOMPARE(first.value("price").toInt(), 159900);
        QCOMPARE(first.value("images").toArray().size(), 29);
        QCOMPARE(first.value("images").toArray()[0].toString(), QString("https://photos.example.net/1874/900126/1000.jpeg"));   // "?d=s" thumbnail flag stripped
        QCOMPARE(first.value("baths").toDouble(), 2.0);
        // rotation: Active only, 4+ photos; never-featured newest first, then longest ago; no immediate repeat
        QJsonObject state; qint64 now = QDateTime(QDate(2026, 10, 23), QTime(9, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        QJsonObject r = pickListing(ls, state, now, QJsonObject());
        QCOMPARE(r.value("eligible").toInt(), 3);                                                         // 900126, 900115, 900075
        QCOMPARE(r.value("newIds").toArray().size(), 3);
        QCOMPARE(r.value("listing").toObject().value("mls").toString(), QString("900126"));            // newest first_seen
        state = markFeatured(r.value("state").toObject(), "900126", now);
        r = pickListing(ls, state, now + 3600000, QJsonObject());
        QCOMPARE(r.value("newIds").toArray().size(), 0);                                                  // already seen
        QCOMPARE(r.value("listing").toObject().value("mls").toString(), QString("900115"));
        state = markFeatured(r.value("state").toObject(), "900115", now + 3600000);
        r = pickListing(ls, state, now + 7200000, QJsonObject());
        QCOMPARE(r.value("listing").toObject().value("mls").toString(), QString("900075"));
        state = markFeatured(r.value("state").toObject(), "900075", now + 7200000);
        r = pickListing(ls, state, now + 10800000, QJsonObject());
        QCOMPARE(r.value("listing").toObject().value("mls").toString(), QString("900126"));            // all shown: the one featured longest ago comes round again
        // a brand-new listing jumps the queue on the next run
        state = markFeatured(r.value("state").toObject(), "900126", now + 10800000);
        QJsonArray more = ls; more.append(parseListings(idxPage(QJsonArray{listing("900190", "88 New Listing Dr", "Collinsville", 210000, "Active", 12, "2026-10-23T08:00:00+00:00")}))[0]);
        r = pickListing(more, state, now + 14400000, QJsonObject());
        QCOMPARE(r.value("newIds").toArray(), (QJsonArray{"900190"}));
        QCOMPARE(r.value("listing").toObject().value("mls").toString(), QString("900190"));
        // a lone eligible home is repeated rather than leaving the hour empty; none eligible -> null
        QJsonObject solo = pickListing(QJsonArray{ls[0]}, markFeatured(QJsonObject(), "900126", now), now + 1000, QJsonObject());
        QCOMPARE(solo.value("listing").toObject().value("mls").toString(), QString("900126"));
        QVERIFY(pickListing(QJsonArray{ls[2].toObject()}, QJsonObject(), now, QJsonObject()).value("listing").isNull());
    }

    void realtyScriptTellsTheListing() {
        const QJsonArray ls = parseListings(idxPage(QJsonArray{listing("900102", "100 Example Rd", "Anytown", 185000, "Active", 33, "2026-10-14T11:03:46+00:00", kDesc)}));
        QCOMPARE(ls.size(), 1);
        QJsonObject opts; opts["siteName"] = "Example Realty"; opts["site"] = "example.com";
        const QJsonObject s = localspots::realtyScript(ls[0].toObject(), at(2026, 10, 23, 10, 0), opts);
        QVERIFY(!s.contains("skip") && !s.contains("error"));
        const QJsonArray sc = s.value("scenes").toArray();
        const int pictures = sc.size() - 1;
        QVERIFY2(pictures >= 4 && pictures <= 8, qPrintable(QString::number(pictures)));                 // 4-8 pictures
        QCOMPARE(sc.last().toObject().value("kind").toString(), QString("outro"));
        QSet<QString> imgs; for (int i = 0; i < pictures; ++i) { QCOMPARE(sc[i].toObject().value("kind").toString(), QString("story")); imgs.insert(sc[i].toObject().value("image").toString()); QVERIFY(sc[i].toObject().value("image").toString().startsWith("https://photos.example.net/")); }
        QCOMPARE(imgs.size(), pictures);                                                                  // each photo once
        const QJsonObject first = sc[0].toObject();
        QCOMPARE(first.value("title").toString(), QString("100 Example Rd"));
        QCOMPARE(first.value("detail").toString(), QString::fromUtf8("$185,000 \xC2\xB7 3 BD \xC2\xB7 2 BA"));
        QCOMPARE(first.value("image").toString(), QString("https://photos.example.net/1874/900102/1000.jpeg"));
        QVERIFY(first.value("narration").toString().contains("Today's featured home from Example Realty. 100 Example Road in Anytown. Listed at one hundred eighty five thousand dollars."));
        QVERIFY(first.value("narration").toString().contains("Three bedrooms, two baths and about one thousand five hundred forty eight square feet"));
        QCOMPARE(first.value("counter").toString(), QString("1 / %1").arg(pictures));
        const QString all = narrations(s).join(' ');
        QVERIFY(all.contains("covered front porch"));                                                     // the listing's own words
        QVERIFY(!all.contains("Schedule your showing"));                                                  // calls to action and steering language never air
        QVERIFY(!all.contains("young family"));
        QVERIFY(!all.contains("Lot size") && !all.contains("Taxes to be determined"));                  // estimate / tax disclaimers are not part of the tour
        QCOMPARE(sc[1].toObject().value("title").toString(), QString("About This Home"));               // the first group introduces the home
        QSet<QString> labels; for (int i = 1; i < pictures; ++i) labels.insert(sc[i].toObject().value("title").toString());
        QCOMPARE(labels.size(), pictures - 1);                                                           // no label twice
        // one station line for every home replaces the listing agent's own number
        QJsonObject withLine = opts; withLine["contactPhone"] = "276-555-0100";
        const QJsonObject s2 = localspots::realtyScript(ls[0].toObject(), at(2026, 10, 23, 10, 0), withLine);
        const QString outro2 = s2.value("scenes").toArray().last().toObject().value("narration").toString();
        QVERIFY(outro2.contains("call Example Realty at two seven six, five five five, zero one zero zero."));
        QVERIFY(!outro2.contains("Pat"));
        QVERIFY(s2.value("scenes").toArray().last().toObject().value("sub").toString().contains("276-555-0100"));
        // photos follow the discussion: the kitchen sentence is not shown before the front-porch sentence
        int porch = -1, kitchen = -1;
        for (int i = 1; i < pictures; ++i) { const QString n = sc[i].toObject().value("narration").toString(); if (porch < 0 && n.contains("front porch")) porch = i; if (kitchen < 0 && n.contains("kitchen")) kitchen = i; }
        QVERIFY(porch > 0 && kitchen > 0);
        const QString outro = sc.last().toObject().value("narration").toString();
        QVERIFY(outro.contains("contact Pat Example at Example Realty, five four zero, five five five, zero one zero zero."));
        QVERIFY(outro.contains("example dot com"));
        QVERIFY(outro.contains("Listing courtesy of Keller Williams Example Realty."));
        QCOMPARE(sc.last().toObject().value("big").toString(), QString("example.com"));
        QVERIFY(sc.last().toObject().value("sub").toString().contains("540-555-0100"));
        QVERIFY(sc.last().toObject().value("sub").toString().contains("MLS 900102"));
        QCOMPARE(s.value("local").toObject().value("mls").toString(), QString("900102"));
        // too few photos: nothing aired
        QVERIFY(localspots::realtyScript(parseListings(idxPage(QJsonArray{listing("1", "1 A St", "X", 1000, "Active", 3, "2026-10-01T00:00:00+00:00", kDesc)}))[0].toObject(), at(2026, 10, 23, 10, 0), opts).contains("skip"));
    }

    void shortDescriptionStillMakesFourPictures() {
        const QJsonArray ls = parseListings(idxPage(QJsonArray{listing("2", "12 Quiet Ln", "Bassett", 99000, "Active", 9, "2026-10-01T00:00:00+00:00", "Charming bungalow with a large kitchen. Updated flooring throughout.")}));
        const QJsonObject s = localspots::realtyScript(ls[0].toObject(), at(2026, 10, 23, 10, 0), QJsonObject());
        QVERIFY(!s.contains("skip"));
        QVERIFY(s.value("scenes").toArray().size() - 1 >= 4);                                           // facts fill in for a thin description
    }
};

QTEST_APPLESS_MAIN(TestLocalSpots)
#include "test_localspots.moc"
