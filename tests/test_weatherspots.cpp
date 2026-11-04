// Weather spot: conditions, moon, burn safety, the source parsers and the six-segment script. Fixtures mirror the real payloads.
#include <QtTest>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimeZone>
#include "../playout/supervisor/weatherspots.h"

using namespace weatherspots;

static QDateTime at(int y, int mo, int d, int h, int mi) { return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone("America/New_York")); }
static QStringList kinds(const QJsonObject& s) { QStringList o; for (const QJsonValue& v : s.value("scenes").toArray()) o << v.toObject().value("kind").toString(); return o; }
static QJsonObject sceneWith(const QJsonObject& s, const QString& type) {
    for (const QJsonValue& v : s.value("scenes").toArray()) if (v.toObject().value("anim").toObject().value("type").toString() == type) return v.toObject();
    return QJsonObject();
}

/// Open-Meteo shape: 3 past days + today (2026-10-23) + 7 forecast days; hourly humidity with a 31% afternoon low today.
static QJsonObject forecast(double rainPast = 0.0) {
    QJsonArray time, code, hi, lo, pop, rain, gust, sunrise, sunset, daylight, htime, hrh;
    const QList<int> codes{0, 0, 0, 0, 3, 3, 0, 2, 3, 3, 1};
    const QList<double> his{73.7, 70.3, 71.3, 65.7, 61.5, 65.4, 71.2, 71.4, 63.5, 67.8, 74.8}, los{34.9, 37.6, 39.3, 36.5, 40.1, 41.1, 42.2, 46.9, 45.8, 42.9, 44.9};
    for (int i = 0; i < 11; ++i) {
        const QDate d = QDate(2026, 10, 20).addDays(i); time.append(d.toString(Qt::ISODate)); code.append(codes[i]); hi.append(his[i]); lo.append(los[i]);
        pop.append(i == 8 ? 60 : 0); rain.append(i < 3 ? rainPast : 0.0); gust.append(i == 3 ? 20.4 : 8.0);
        sunrise.append(d.toString(Qt::ISODate) + "T07:2" + QString::number(i % 10)); sunset.append(d.toString(Qt::ISODate) + QString("T18:%1").arg(35 - i));   // 18:32 today, one minute earlier each day after
        daylight.append(40260.0);
        for (int h = 0; h < 24; ++h) { htime.append(d.toString(Qt::ISODate) + QString("T%1:00").arg(h, 2, 10, QChar('0'))); hrh.append(i == 3 && h == 15 ? 31 : (h >= 10 && h <= 18 ? 48 : 70)); }
    }
    QJsonObject daily{{"time", time}, {"weather_code", code}, {"temperature_2m_max", hi}, {"temperature_2m_min", lo}, {"precipitation_probability_max", pop}, {"precipitation_sum", rain},
                      {"wind_gusts_10m_max", gust}, {"sunrise", sunrise}, {"sunset", sunset}, {"daylight_duration", daylight}};
    QJsonObject current{{"time", "2026-10-23T15:45"}, {"temperature_2m", 65.5}, {"apparent_temperature", 60.3}, {"relative_humidity_2m", 45}, {"weather_code", 0}, {"wind_speed_10m", 8.5}, {"wind_gusts_10m", 11.4}, {"is_day", 1}};
    return QJsonObject{{"current", current}, {"daily", daily}, {"hourly", QJsonObject{{"time", htime}, {"relative_humidity_2m", hrh}}}};
}
static QJsonObject lakes() {
    return QJsonObject{{"smithMountain", QJsonObject{{"actual", 790.64}, {"fullPond", 795.0}}}, {"leesville", QJsonObject{{"actual", 603.75}, {"fullPond", 613.0}}},
                       {"philpott", QJsonObject{{"actual", 970.71}, {"changeFt", 0.10}}}};
}
static QJsonObject noAlert() { QJsonObject a; a["alert"] = ""; return a; }

class TestWeatherSpots : public QObject {
    Q_OBJECT
private slots:
    void conditions() {
        QCOMPARE(conditionOf(0), QString("clear")); QCOMPARE(conditionOf(2), QString("partly")); QCOMPARE(conditionOf(45), QString("fog"));
        QCOMPARE(conditionOf(63), QString("rain")); QCOMPARE(conditionOf(65), QString("heavy_rain")); QCOMPARE(conditionOf(73), QString("snow")); QCOMPARE(conditionOf(95), QString("storm"));
        QCOMPARE(conditionText(0, true), QString("sunny")); QCOMPARE(conditionText(0, false), QString("clear")); QCOMPARE(conditionText(80, true), QString("scattered showers"));
    }

    void moonPhases() {
        // 2000-01-06 18:14 UTC is the reference new moon; 2000-01-21 04:40 UTC was a full moon (the total lunar eclipse)
        const QJsonObject nm = moonInfo(QDateTime(QDate(2000, 1, 6), QTime(18, 14), QTimeZone::UTC));
        QCOMPARE(nm.value("phase").toString(), QString("New Moon")); QVERIFY(nm.value("illumination").toDouble() < 1.0);
        const QJsonObject fm = moonInfo(QDateTime(QDate(2000, 1, 21), QTime(4, 40), QTimeZone::UTC));
        QCOMPARE(fm.value("phase").toString(), QString("Full Moon")); QVERIFY(fm.value("illumination").toDouble() > 99.0);
        const QJsonObject wx = moonInfo(QDateTime(QDate(2000, 1, 12), QTime(0, 0), QTimeZone::UTC));         // ~5.2 days old
        QCOMPARE(wx.value("phase").toString(), QString("Waxing Crescent")); QVERIFY(wx.value("waxing").toBool());
        const QJsonObject wn = moonInfo(QDateTime(QDate(2000, 1, 27), QTime(0, 0), QTimeZone::UTC));         // ~20.2 days old
        QCOMPARE(wn.value("phase").toString(), QString("Waning Gibbous")); QVERIFY(!wn.value("waxing").toBool());
        // the next full and new moons are in the future, a half cycle apart at most a month out
        const QDateTime when = at(2026, 10, 23, 20, 0); const QJsonObject m = moonInfo(when);
        QVERIFY(QDate::fromString(m.value("nextFull").toString(), Qt::ISODate) >= when.date()); QVERIFY(QDate::fromString(m.value("nextFull").toString(), Qt::ISODate) <= when.date().addDays(30));
        QVERIFY(QDate::fromString(m.value("nextNew").toString(), Qt::ISODate) > when.date());
        QVERIFY(m.value("illumination").toDouble() >= 0 && m.value("illumination").toDouble() <= 100);
    }

    void burnRules() {
        const QDate oct(2026, 10, 23);
        QJsonObject dry = burnSafety(20.4, 31, 0.0, 0.0, 66, "", oct);
        QCOMPARE(dry.value("rating").toString(), QString("High")); QCOMPARE(dry.value("score").toInt(), 6);
        QVERIFY(dry.value("law").toString().isEmpty());                                         // the 4 PM law is a Feb 15 - Apr 30 rule
        QCOMPARE(dry.value("reasons").toArray()[0].toString(), QString("gusts to 20 mph"));
        // the gauge needle stays inside its rating's segment: Low 0-.2, Moderate .2-.4, High .4-.6, Very High .6-.8, Extreme .8-1
        QVERIFY(dry.value("level").toDouble() >= 0.4 && dry.value("level").toDouble() < 0.6);
        const QJsonObject lowB = burnSafety(5, 70, 1.2, 0.5, 60, "", oct), modB = burnSafety(12, 40, 0.15, 0.0, 70, "", oct), vhB = burnSafety(30, 20, 0.0, 0.0, 90, "", oct), exB = burnSafety(5, 70, 1.2, 0.5, 60, "warning", oct);
        QVERIFY(lowB.value("level").toDouble() < 0.2); QVERIFY(modB.value("level").toDouble() >= 0.2 && modB.value("level").toDouble() < 0.4);
        QVERIFY(vhB.value("level").toDouble() >= 0.6 && vhB.value("level").toDouble() < 0.8); QVERIFY(exB.value("level").toDouble() >= 0.8);
        QCOMPARE(burnSafety(5, 70, 1.2, 0.5, 60, "", oct).value("rating").toString(), QString("Low"));
        QCOMPARE(burnSafety(12, 40, 0.15, 0.0, 70, "", oct).value("rating").toString(), QString("Moderate"));         // 1 + 1 + 1 = 3
        QCOMPARE(burnSafety(30, 20, 0.0, 0.0, 90, "", oct).value("rating").toString(), QString("Very High"));
        QCOMPARE(burnSafety(5, 70, 1.2, 0.5, 60, "warning", oct).value("rating").toString(), QString("Extreme"));      // a Red Flag Warning outranks everything
        QCOMPARE(burnSafety(5, 70, 1.2, 0.5, 60, "watch", oct).value("rating").toString(), QString("High"));
        QVERIFY(burnSafety(5, 70, 1.2, 0.5, 60, "warning", oct).value("advice").toString().contains("Do not burn"));
        QVERIFY(!burnSafety(5, 70, 1.2, 0.5, 60, "", QDate(2026, 3, 10)).value("law").toString().isEmpty());
        QVERIFY(burnSafety(5, 70, 1.2, 0.5, 60, "", QDate(2026, 2, 14)).value("law").toString().isEmpty());
        QVERIFY(!burnSafety(5, 70, 1.2, 0.5, 60, "", QDate(2026, 2, 15)).value("law").toString().isEmpty());
        QVERIFY(burnSafety(5, 70, 1.2, 0.5, 60, "", QDate(2026, 5, 1)).value("law").toString().isEmpty());
        QVERIFY(!burnSafety(5, 70, 1.2, 0.5, 60, "", QDate(2026, 4, 30)).value("law").toString().isEmpty());
    }

    void sourcesParse() {
        const QString aep = "<h2>Most Recent Conditions</h2><table><tr><th>Project</th><th>Normal Full Pond</th><th>Forebay Actual (Feet)</th><th>Tail Water (Feet)</th><th>Tail Water (CFS)</th></tr>"
                            "<tr><td>Niagara</td><td>885.00</td><td>884.86</td><td>821.64</td><td>184.00</td></tr>"
                            "<tr><td>Smith Mountain</td><td>795.00</td><td>790.64</td><td>603.72</td><td>N/A</td></tr>"
                            "<tr><td>Leesville</td><td>613.00.00</td><td>603.75</td><td>532.17</td><td>416</td></tr></table>";
        const QJsonObject h = parseAepHydro(aep);
        QCOMPARE(h.value("smith mountain").toObject().value("fullPond").toDouble(), 795.0);
        QCOMPARE(h.value("smith mountain").toObject().value("actual").toDouble(), 790.64);
        QCOMPARE(h.value("leesville").toObject().value("fullPond").toDouble(), 613.0);              // "613.00.00" typo on the page
        QCOMPARE(h.value("leesville").toObject().value("actual").toDouble(), 603.75);
        QVERIFY(!h.contains("project")); QCOMPARE(parseAepHydro("<html>nothing</html>").size(), 0);
        // USGS: hourly elevations over a day (970.61 -> 970.71), humidity and rain beside them
        QJsonArray ev, rh; const QDateTime t0(QDate(2026, 10, 22), QTime(15, 0), QTimeZone("America/New_York"));
        for (int i = 0; i <= 24; ++i) { const QString ts = t0.addSecs(i * 3600).toString(Qt::ISODate); ev.append(QJsonObject{{"value", QString::number(970.61 + i * 0.1 / 24, 'f', 3)}, {"dateTime", ts}}); rh.append(QJsonObject{{"value", "42.0"}, {"dateTime", ts}}); }
        auto series = [](const char* code, const QJsonArray& vals) { return QJsonObject{{"variable", QJsonObject{{"variableCode", QJsonArray{QJsonObject{{"value", code}}}}}}, {"values", QJsonArray{QJsonObject{{"value", vals}}}}}; };
        const QJsonObject root{{"value", QJsonObject{{"timeSeries", QJsonArray{series("62614", ev), series("00052", rh)}}}}};
        const QJsonObject u = parseUsgsLake(QJsonDocument(root).toJson());
        QVERIFY(qAbs(u.value("elevation").toDouble() - 970.71) < 1e-6); QVERIFY(qAbs(u.value("changeFt").toDouble() - 0.10) < 1e-6); QCOMPARE(u.value("humidity").toDouble(), 42.0);
        QVERIFY(parseUsgsLake("{}").isEmpty());
        // NWS
        QCOMPARE(parseNwsAlerts("{\"features\":[]}").value("alert").toString(), QString());
        const QByteArray red = "{\"features\":[{\"properties\":{\"event\":\"Wind Advisory\",\"headline\":\"Wind Advisory until 8 PM\"}},{\"properties\":{\"event\":\"Red Flag Warning\",\"headline\":\"Red Flag Warning\"}}]}";
        QCOMPARE(parseNwsAlerts(red).value("alert").toString(), QString("warning")); QCOMPARE(parseNwsAlerts(red).value("events").toArray().size(), 2);
        QCOMPARE(parseNwsAlerts("{\"features\":[{\"properties\":{\"event\":\"Fire Weather Watch\"}}]}").value("alert").toString(), QString("watch"));
    }

    void scriptTellsTheDay() {
        QJsonObject opts; opts["place"] = "Martinsville"; opts["siteName"] = "GetStarNews"; opts["site"] = "getstarnews.com";
        const QJsonObject s = weatherScript(forecast(), noAlert(), lakes(), at(2026, 10, 23, 15, 45), opts);
        QVERIFY(!s.contains("error") && !s.contains("skip"));
        QCOMPARE(kinds(s), (QStringList{"story", "story", "story", "story", "story", "story", "outro"}));
        const QJsonArray sc = s.value("scenes").toArray();
        // now
        const QJsonObject now = sc[0].toObject();
        QCOMPARE(now.value("title").toString(), QString("Martinsville Weather")); QCOMPARE(now.value("anim").toObject().value("type").toString(), QString("now"));
        QVERIFY(now.value("narration").toString().startsWith("Here's your forecast for Martinsville. Right now it's sixty six degrees and sunny, but it feels like sixty."));
        QVERIFY(now.value("narration").toString().contains("Today's high will reach sixty six, with a low of thirty seven tonight."));
        QVERIFY(now.value("narration").toString().contains("No rain is expected today."));
        QCOMPARE(now.value("anim").toObject().value("cond").toString(), QString("clear")); QCOMPARE(now.value("anim").toObject().value("hi").toInt(), 66);
        // cards
        const QJsonObject cards = sceneWith(s, "cards"); const QJsonArray days = cards.value("anim").toObject().value("days").toArray();
        QCOMPARE(days.size(), 7); QCOMPARE(days[0].toObject().value("dow").toString(), QString("FRI")); QCOMPARE(days[1].toObject().value("dow").toString(), QString("SAT"));
        QCOMPARE(days[1].toObject().value("cond").toString(), QString("cloudy")); QCOMPARE(days[1].toObject().value("hi").toInt(), 62); QCOMPARE(days[1].toObject().value("lo").toInt(), 40);
        QCOMPARE(cards.value("title").toString(), QString("7-Day Forecast"));
        QVERIFY(cards.value("narration").toString().startsWith("Looking ahead. Tomorrow, cloudy, high sixty two, low forty."));
        QVERIFY(cards.value("narration").toString().contains("60 percent chance of rain") || cards.value("narration").toString().contains("sixty percent chance of rain"));   // the rainy day mentions it
        // sunset
        const QJsonObject sun = sceneWith(s, "sunset");
        QVERIFY(sun.value("narration").toString().startsWith("Sunset today is at six thirty two P M. Golden hour begins around five thirty two P M."));
        QVERIFY(sun.value("narration").toString().contains("Tomorrow the sun sets at six thirty one P M, one minute earlier."));
        QVERIFY(sun.value("narration").toString().contains("eleven hours and eleven minutes of daylight"));
        QCOMPARE(sun.value("anim").toObject().value("sunset").toString(), QString("6:32 PM")); QCOMPARE(sun.value("anim").toObject().value("delta").toInt(), -1);
        // moon
        const QJsonObject moon = sceneWith(s, "moon"); QVERIFY(!moon.value("anim").toObject().value("phase").toString().isEmpty());
        QVERIFY(moon.value("narration").toString().contains("The next full moon arrives around")); QVERIFY(moon.value("anim").toObject().value("fraction").isDouble());
        // burn: gust 20 + humidity 31% + no rain in 3 days = 6
        const QJsonObject burn = sceneWith(s, "burn");
        QCOMPARE(burn.value("anim").toObject().value("rating").toString(), QString("High")); QCOMPARE(burn.value("title").toString(), QString("Burn Safety"));
        QVERIFY(burn.value("narration").toString().startsWith("Burn safety today is high. Gusts to 20 miles per hour, humidity as low as 31 percent, no rain in the last 3 days."));
        QVERIFY(burn.value("narration").toString().contains("Check for local burn bans before you light anything."));
        // lakes
        const QJsonObject lk = sceneWith(s, "lakes"); const QJsonArray rows = lk.value("rows").toArray();
        QCOMPARE(rows.size(), 3); QCOMPARE(rows[0].toObject().value("label").toString(), QString("Smith Mountain Lake")); QCOMPARE(rows[0].toObject().value("value").toString(), QString("790.64 ft"));
        QCOMPARE(rows[0].toObject().value("sub").toString(), QString("4.36 ft below full pond (795)"));
        QCOMPARE(rows[2].toObject().value("change").toString(), QString("+0.10 ft 24h")); QCOMPARE(rows[2].toObject().value("tone").toString(), QString("up"));
        QVERIFY(lk.value("narration").toString().contains("Smith Mountain Lake is at seven hundred ninety point six feet, four point four feet below full pond."));
        QVERIFY(lk.value("narration").toString().contains("Philpott Lake is at nine hundred seventy point seven feet, up zero point one feet over the past day."));
        // counters and the closing card
        QCOMPARE(sc[0].toObject().value("counter").toString(), QString("1 / 6")); QCOMPARE(sc[5].toObject().value("counter").toString(), QString("6 / 6"));
        QCOMPARE(sc.last().toObject().value("big").toString(), QString("getstarnews.com"));
        QCOMPARE(s.value("package").toString(), QString("Weather"));
    }

    void segmentsAndTimeOfDay() {
        QJsonObject opts; opts["segments"] = QJsonArray{"now", "burn"};
        QCOMPARE(kinds(weatherScript(forecast(), noAlert(), lakes(), at(2026, 10, 23, 15, 45), opts)), (QStringList{"story", "story", "outro"}));
        // evening: the low and tomorrow's high; the sun has set; burn safety is for tomorrow
        const QJsonObject e = weatherScript(forecast(), noAlert(), lakes(), at(2026, 10, 23, 19, 30), QJsonObject());
        QVERIFY(sceneWith(e, "now").value("narration").toString().contains("Tonight's low will be thirty seven, and tomorrow's high is sixty two."));
        QVERIFY(sceneWith(e, "sunset").value("narration").toString().startsWith("The sun set at six thirty two P M today."));
        QCOMPARE(sceneWith(e, "burn").value("title").toString(), QString("Burn Safety")); QCOMPARE(sceneWith(e, "burn").value("category").toString(), QString("FIRE WEATHER TOMORROW"));
        QVERIFY(sceneWith(e, "burn").value("narration").toString().startsWith("Burn safety tomorrow is"));
        // a missing segment's data drops just that segment; no lake data = no lakes scene
        const QJsonObject nolake = weatherScript(forecast(), noAlert(), QJsonObject(), at(2026, 10, 23, 15, 45), QJsonObject());
        QVERIFY(sceneWith(nolake, "lakes").isEmpty()); QVERIFY(!sceneWith(nolake, "burn").isEmpty());
        // wet past days lower the burn rating; a Red Flag Warning raises it
        QCOMPARE(sceneWith(weatherScript(forecast(0.5), noAlert(), lakes(), at(2026, 10, 23, 15, 45), QJsonObject()), "burn").value("anim").toObject().value("rating").toString(), QString("Moderate"));   // 2 + 2 + 0
        QJsonObject red; red["alert"] = "warning";
        const QJsonObject rf = weatherScript(forecast(), red, lakes(), at(2026, 10, 23, 15, 45), QJsonObject());
        QCOMPARE(sceneWith(rf, "burn").value("anim").toObject().value("rating").toString(), QString("Extreme"));
        QVERIFY(sceneWith(rf, "burn").value("narration").toString().contains("Do not burn outdoors"));
        // a forecast that does not reach today is an error, not a guess
        QVERIFY(weatherScript(forecast(), noAlert(), lakes(), at(2026, 11, 20, 12, 0), QJsonObject()).contains("error"));
        QVERIFY(weatherScript(QJsonObject(), noAlert(), lakes(), at(2026, 10, 23, 15, 45), QJsonObject()).contains("error"));
        // negative temperatures are spoken
        QJsonObject f = forecast(); QJsonObject cur = f.value("current").toObject(); cur["temperature_2m"] = -3.0; cur["apparent_temperature"] = -9.0; f["current"] = cur;
        QVERIFY(sceneWith(weatherScript(f, noAlert(), lakes(), at(2026, 10, 23, 15, 45), QJsonObject()), "now").value("narration").toString().contains("minus three degrees"));
    }
};

QTEST_APPLESS_MAIN(TestWeatherSpots)
#include "test_weatherspots.moc"
