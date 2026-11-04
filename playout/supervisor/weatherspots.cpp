// Weather spot: forecast cards, sunset, moon phase, burn safety and lake levels as animated scenes. Pure QtCore: see weatherspots.h.
#include "weatherspots.h"
#include "localspots.h"
#include "marketspots.h"
#include "headlinenews.h"

#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <cmath>

namespace weatherspots {

using RX = QRegularExpression;
static QLocale us() { return QLocale(QLocale::English, QLocale::UnitedStates); }
static QString cap1(QString s) { if (!s.isEmpty()) s[0] = s[0].toUpper(); return s; }
static QString say(const QString& s) { return localspots::speech(s); }
static QString num(qint64 n) { return localspots::spokenNumber(n); }
static QString signedNum(int n) { return n < 0 ? "minus " + num(-n) : num(n); }
static QString clock12(const QTime& t) { return us().toString(t, "h:mm AP"); }
static QString plural(int n, const char* one, const char* many) { return num(n) + " " + (n == 1 ? one : many); }

// ---------------------------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------------------------
QString conditionOf(int c) {
    switch (c) {
    case 0: return "clear"; case 1: return "mostly_clear"; case 2: return "partly"; case 3: return "cloudy";
    case 45: case 48: return "fog";
    case 51: case 53: case 55: case 56: case 57: return "drizzle";
    case 61: case 63: case 66: case 67: case 80: case 81: return "rain";
    case 65: case 82: return "heavy_rain";
    case 71: case 73: case 75: case 77: case 85: case 86: return "snow";
    case 95: case 96: case 99: return "storm";
    default: return "cloudy";
    }
}

QString conditionText(int c, bool day) {
    switch (c) {
    case 0: return day ? "sunny" : "clear"; case 1: return day ? "mostly sunny" : "mostly clear"; case 2: return "partly cloudy"; case 3: return "cloudy";
    case 45: case 48: return "foggy"; case 51: case 53: case 55: return "drizzly"; case 56: case 57: return "freezing drizzle";
    case 61: return "light rain"; case 63: return "rainy"; case 65: return "heavy rain"; case 66: case 67: return "freezing rain";
    case 71: return "light snow"; case 73: return "snowy"; case 75: return "heavy snow"; case 77: return "flurries";
    case 80: return "scattered showers"; case 81: return "showers"; case 82: return "heavy showers"; case 85: case 86: return "snow showers";
    case 95: return "stormy"; case 96: case 99: return "stormy with hail"; default: return "mixed";
    }
}

// ---------------------------------------------------------------------------------------------
// Moon
// ---------------------------------------------------------------------------------------------
static const double kSynodic = 29.53058867;
static const double kNewMoonRef = 947182440.0;                 // 2000-01-06 18:14 UTC, seconds since the epoch

QJsonObject moonInfo(const QDateTime& when) {
    const double secs = double(when.toMSecsSinceEpoch()) / 1000.0;
    double age = std::fmod((secs - kNewMoonRef) / 86400.0, kSynodic); if (age < 0) age += kSynodic;
    const double illum = 50.0 * (1.0 - std::cos(2.0 * M_PI * age / kSynodic));
    QString phase;
    if (age < 1.0 || age >= 28.5) phase = "New Moon"; else if (age < 6.38) phase = "Waxing Crescent"; else if (age < 8.38) phase = "First Quarter";
    else if (age < 13.38) phase = "Waxing Gibbous"; else if (age < 15.38) phase = "Full Moon"; else if (age < 21.1) phase = "Waning Gibbous";
    else if (age < 23.1) phase = "Last Quarter"; else phase = "Waning Crescent";
    // the next full (age 14.77) and new (age 29.53) moon after now
    const double toFull = std::fmod(kSynodic / 2.0 - age + kSynodic, kSynodic), toNew = kSynodic - age;
    const QTimeZone tz = when.timeZone();
    QJsonObject o; o["ageDays"] = age; o["illumination"] = illum; o["phase"] = phase; o["waxing"] = age < kSynodic / 2.0; o["fraction"] = age / kSynodic;
    o["nextFull"] = when.addMSecs(qint64(toFull * 86400000.0)).toTimeZone(tz).date().toString(Qt::ISODate);
    o["nextNew"] = when.addMSecs(qint64(toNew * 86400000.0)).toTimeZone(tz).date().toString(Qt::ISODate);
    return o;
}

// ---------------------------------------------------------------------------------------------
// Burn safety
// ---------------------------------------------------------------------------------------------
QJsonObject burnSafety(double gust, double minRh, double rain3, double rainToday, double highF, const QString& alert, const QDate& date) {
    int score = 0; QStringList reasons;
    if (gust >= 25) { score += 3; reasons << QString("gusts to %1 mph").arg(qRound(gust)); } else if (gust >= 15) { score += 2; reasons << QString("gusts to %1 mph").arg(qRound(gust)); } else if (gust >= 10) { score += 1; reasons << QString("breezy, gusts to %1 mph").arg(qRound(gust)); } else reasons << "light winds";
    if (minRh <= 25) { score += 3; reasons << QString("humidity as low as %1%").arg(qRound(minRh)); } else if (minRh <= 35) { score += 2; reasons << QString("humidity as low as %1%").arg(qRound(minRh)); } else if (minRh <= 45) { score += 1; reasons << QString("humidity near %1%").arg(qRound(minRh)); } else reasons << QString("humidity %1%").arg(qRound(minRh));
    if (rain3 < 0.10) { score += 2; reasons << "no rain in the last 3 days"; } else if (rain3 < 0.25) { score += 1; reasons << "little rain lately"; } else reasons << QString("%1 in. of rain in 3 days").arg(QString::number(rain3, 'f', 2));
    if (rainToday >= 0.10) { score -= 2; reasons << "rain expected"; }
    if (highF >= 85) { score += 1; reasons << "hot"; }
    score = qMax(0, score);
    QString rating = score <= 2 ? "Low" : score <= 4 ? "Moderate" : score <= 6 ? "High" : "Very High";
    if (alert == "watch" && score < 5) { rating = "High"; score = qMax(score, 5); }
    if (alert == "warning") { rating = "Extreme"; score = qMax(score, 8); }
    QString advice;
    if (rating == "Extreme") advice = "A Red Flag Warning is in effect. Do not burn outdoors.";
    else if (rating == "Very High") advice = "Fire can spread fast. Postpone outdoor burning.";
    else if (rating == "High") advice = "Fire could spread quickly. Burning is not recommended today.";
    else if (rating == "Moderate") advice = "If you burn, keep it small, stay with it, and have water ready.";
    else advice = "Burn only when needed, keep it small, and never leave it unattended.";
    // Virginia's 4 PM Burning Law: no open burning before 4 PM within 300 feet of woods or dry grass, every day from Feb. 15 through April 30
    const bool law = (date.month() == 2 && date.day() >= 15) || date.month() == 3 || date.month() == 4;
    QJsonObject o; // the needle sits inside its rating's gauge segment (five equal segments: Low, Moderate, High, Very High, Extreme)
    const double level = rating == "Extreme" ? 0.90 : rating == "Very High" ? qMin(0.78, 0.62 + (score - 7) * 0.07) : rating == "High" ? 0.42 + (qBound(5, score, 6) - 5) * 0.14
                       : rating == "Moderate" ? 0.22 + (qBound(3, score, 4) - 3) * 0.14 : 0.06 + qBound(0, score, 2) * 0.06;
    o["rating"] = rating; o["score"] = score; o["level"] = level; o["reasons"] = QJsonArray::fromStringList(reasons); o["advice"] = advice;
    o["law"] = law ? "Virginia's 4 p.m. burning law is in effect through April 30: no open burning before 4 p.m. near woods or dry grass." : "";
    return o;
}

// ---------------------------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------------------------
QJsonObject parseAepHydro(const QString& html) {
    QJsonObject out;
    const int at = html.indexOf("Most Recent Conditions");
    const QString seg = at >= 0 ? html.mid(at, 14000) : html;
    static const RX row("<tr[^>]*>(.*?)</tr>", RX::DotMatchesEverythingOption);
    static const RX cell("<t[hd][^>]*>(.*?)</t[hd]>", RX::DotMatchesEverythingOption);
    auto it = row.globalMatch(seg);
    while (it.hasNext()) {
        QStringList cells; auto ci = cell.globalMatch(it.next().captured(1));
        while (ci.hasNext()) cells << headline::plainText(ci.next().captured(1));
        if (cells.size() < 3 || cells[0] == "Project") continue;
        // "613.00.00" (a typo the page has shipped): take the first well-formed number
        static const RX numRx("\\d+(?:\\.\\d+)?");
        const auto full = numRx.match(cells[1]), actual = numRx.match(cells[2]);
        if (!full.hasMatch() || !actual.hasMatch()) continue;
        QJsonObject p; p["fullPond"] = full.captured(0).toDouble(); p["actual"] = actual.captured(0).toDouble();
        if (cells.size() > 3) { const auto tw = numRx.match(cells[3]); if (tw.hasMatch()) p["tailwater"] = tw.captured(0).toDouble(); }
        out[cells[0].toLower()] = p;
    }
    return out;
}

QJsonObject parseUsgsLake(const QByteArray& json) {
    QJsonObject o;
    const QJsonArray series = QJsonDocument::fromJson(json).object().value("value").toObject().value("timeSeries").toArray();
    for (const QJsonValue& sv : series) {
        const QJsonObject ts = sv.toObject();
        const QString code = ts.value("variable").toObject().value("variableCode").toArray().first().toObject().value("value").toString();
        const QJsonArray vals = ts.value("values").toArray().first().toObject().value("value").toArray();
        if (vals.isEmpty()) continue;
        const double last = vals.last().toObject().value("value").toString().toDouble();
        const QString when = vals.last().toObject().value("dateTime").toString();
        if (code == "62614" || code == "00062") {
            o["elevation"] = last; o["time"] = when;
            const QDateTime lt = QDateTime::fromString(when, Qt::ISODate);
            double ref = vals.first().toObject().value("value").toString().toDouble(); qint64 best = Q_INT64_C(1) << 60;
            for (const QJsonValue& v : vals) {                                              // the reading closest to 24 h before the latest
                const QDateTime t = QDateTime::fromString(v.toObject().value("dateTime").toString(), Qt::ISODate);
                const qint64 d = qAbs(t.secsTo(lt) - 86400); if (d < best) { best = d; ref = v.toObject().value("value").toString().toDouble(); }
            }
            if (best < 4 * 3600) o["changeFt"] = last - ref;
        } else if (code == "00052") o["humidity"] = last;
        else if (code == "00021") o["airTempF"] = last;
        else if (code == "00045") o["precipIn"] = last;
    }
    return o;
}

QJsonObject parseNwsAlerts(const QByteArray& json) {
    QJsonObject out; QString level; QStringList events; QString head;
    for (const QJsonValue& fv : QJsonDocument::fromJson(json).object().value("features").toArray()) {
        const QJsonObject p = fv.toObject().value("properties").toObject(); const QString ev = p.value("event").toString();
        if (ev.isEmpty()) continue; events << ev; if (head.isEmpty()) head = p.value("headline").toString();
        if (ev == "Red Flag Warning") level = "warning"; else if (ev == "Fire Weather Watch" && level != "warning") level = "watch";
    }
    out["alert"] = level; out["events"] = QJsonArray::fromStringList(events); out["headline"] = head;
    return out;
}

// ---------------------------------------------------------------------------------------------
// The script
// ---------------------------------------------------------------------------------------------
namespace {
struct Day { QString date, cond; int code = 0, hi = 0, lo = 0, pop = 0; double rain = 0, gust = 0; QString sunrise, sunset; double daylight = 0; };
}
static QString dayLabelOf(const QDate& d, const QDate& today) { return d == today ? "Today" : d == today.addDays(1) ? "Tomorrow" : us().toString(d, "dddd"); }

QJsonObject weatherScript(const QJsonObject& fc, const QJsonObject& alerts, const QJsonObject& lakes, const QDateTime& now, const QJsonObject& o) {
    const QString place = o.value("place").toString("Martinsville"), siteName = o.value("siteName").toString(), site = o.value("site").toString();
    QSet<QString> segs; for (const QJsonValue& v : o.value("segments").toArray()) segs.insert(v.toString());
    if (segs.isEmpty()) segs = {"now", "cards", "sunset", "moon", "burn", "lakes"};
    const int cardDays = qBound(3, o.value("cardDays").toInt(7), 8);
    const QJsonObject cur = fc.value("current").toObject(), dl = fc.value("daily").toObject(), hr = fc.value("hourly").toObject();
    const QJsonArray dates = dl.value("time").toArray();
    if (dates.isEmpty() || cur.isEmpty()) return localspots::errorResult("the forecast came back without data");
    const QDate today = now.date(); int t = -1;
    for (int i = 0; i < dates.size(); ++i) if (QDate::fromString(dates[i].toString(), Qt::ISODate) == today) t = i;
    if (t < 0) return localspots::errorResult("the forecast does not include today");
    QList<Day> days;
    for (int i = 0; i < dates.size(); ++i) {
        Day d; d.date = dates[i].toString(); d.code = dl.value("weather_code").toArray().at(i).toInt(); d.cond = conditionOf(d.code);
        d.hi = qRound(dl.value("temperature_2m_max").toArray().at(i).toDouble()); d.lo = qRound(dl.value("temperature_2m_min").toArray().at(i).toDouble());
        d.pop = qRound(dl.value("precipitation_probability_max").toArray().at(i).toDouble()); d.rain = dl.value("precipitation_sum").toArray().at(i).toDouble();
        d.gust = dl.value("wind_gusts_10m_max").toArray().at(i).toDouble(); d.sunrise = dl.value("sunrise").toArray().at(i).toString(); d.sunset = dl.value("sunset").toArray().at(i).toString();
        d.daylight = dl.value("daylight_duration").toArray().at(i).toDouble(); days << d;
    }
    const bool evening = now.time().hour() >= 17;
    const bool isDay = cur.value("is_day").toInt(1) == 1;
    const int code = cur.value("weather_code").toInt();
    const QString dateLong = us().toString(today, "dddd, MMMM d").toUpper();
    QJsonArray scenes, sources; QStringList lines;
    auto addScene = [&](QJsonObject sc, const QString& title, const QString& category, const QJsonObject& anim) {
        sc["title"] = title; sc["category"] = category; sc["place"] = dateLong; sc["anim"] = anim; scenes.append(sc);
    };
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    // 1) right now
    if (segs.contains("now")) {
        const int temp = qRound(cur.value("temperature_2m").toDouble()), feels = qRound(cur.value("apparent_temperature").toDouble());
        const Day& td = days[t]; const Day& tm = days[qMin(t + 1, int(days.size()) - 1)];
        QString nar = QString("Here's your forecast for %1. Right now it's %2 degrees and %3").arg(place, signedNum(temp), conditionText(code, isDay));
        if (qAbs(feels - temp) >= 3) nar += QString(", but it feels like %1").arg(signedNum(feels));
        nar += ". ";
        const int pop = evening ? tm.pop : td.pop;
        nar += evening ? QString("Tonight's low will be %1, and tomorrow's high is %2. ").arg(signedNum(td.lo), signedNum(tm.hi)) : QString("Today's high will reach %1, with a low of %2 tonight. ").arg(signedNum(td.hi), signedNum(td.lo));
        if (pop >= 30) nar += QString("There's a %1 percent chance of rain%2. ").arg(num(pop), evening ? " tomorrow" : "");
        else nar += evening ? "No rain is expected tomorrow. " : "No rain is expected today. ";
        const double gust = cur.value("wind_gusts_10m").toDouble(); if (gust >= 20) nar += QString("Winds are gusting to %1 miles per hour.").arg(num(qRound(gust)));
        QJsonObject sc = localspots::makeScene("story", say(nar));
        QJsonObject anim; anim["type"] = "now"; anim["cond"] = conditionOf(code); anim["isDay"] = isDay; anim["temp"] = temp; anim["feels"] = feels; anim["hi"] = td.hi; anim["lo"] = td.lo;
        anim["pop"] = pop; anim["humidity"] = qRound(cur.value("relative_humidity_2m").toDouble()); anim["wind"] = qRound(cur.value("wind_speed_10m").toDouble()); anim["gust"] = qRound(gust);
        anim["label"] = cap1(conditionText(code, isDay)); anim["tomorrowHi"] = tm.hi; anim["evening"] = evening;
        addScene(sc, place + " Weather", "RIGHT NOW", anim);
        lines << QString("%1\xC2\xB0 %2").arg(temp).arg(conditionText(code, isDay)) << QString("High %1  Low %2").arg(td.hi).arg(td.lo);
    }
    // 2) daily cards
    if (segs.contains("cards")) {
        QJsonArray cards; QStringList parts;
        for (int i = t; i < days.size() && i < t + cardDays; ++i) {
            const Day& d = days[i]; const QDate dd = QDate::fromString(d.date, Qt::ISODate);
            QJsonObject c; c["dow"] = us().toString(dd, "ddd").toUpper(); c["date"] = us().toString(dd, "MMM d"); c["cond"] = d.cond; c["label"] = cap1(conditionText(d.code, true));
            c["hi"] = d.hi; c["lo"] = d.lo; c["pop"] = d.pop; c["gust"] = qRound(d.gust); cards.append(c);
            if (i > t) {
                QString s = QString("%1, %2, high %3, low %4").arg(dayLabelOf(dd, today), conditionText(d.code, true), signedNum(d.hi), signedNum(d.lo));
                if (d.pop >= 40) s += QString(", %1 percent chance of rain").arg(num(d.pop));
                parts << s;
            }
        }
        QJsonObject sc = localspots::makeScene("story", say("Looking ahead. " + parts.join(". ") + "."));
        QJsonObject anim; anim["type"] = "cards"; anim["cond"] = conditionOf(code); anim["isDay"] = isDay; anim["days"] = cards;
        addScene(sc, QString("%1-Day Forecast").arg(cards.size()), "FORECAST", anim);
    }
    // 3) sunset
    if (segs.contains("sunset")) {
        const Day& td = days[t]; const Day& tm = days[qMin(t + 1, int(days.size()) - 1)];
        const QDateTime ss = QDateTime::fromString(td.sunset, Qt::ISODate), sr = QDateTime::fromString(td.sunrise, Qt::ISODate), ss2 = QDateTime::fromString(tm.sunset, Qt::ISODate);
        if (ss.isValid()) {
            const bool passed = now.time() > ss.time();
            const QTime golden = ss.time().addSecs(-3600);
            const int delta = ss2.isValid() ? int(std::lround((ss2.time().msecsSinceStartOfDay() - ss.time().msecsSinceStartOfDay()) / 60000.0)) : 0;
            const int dayMin = td.daylight > 0 ? int(std::lround(td.daylight / 60.0)) : (sr.isValid() ? int(sr.secsTo(ss) / 60) : 0);
            QString nar = passed ? QString("The sun set at %1 today. ").arg(localspots::spokenClock(ss.time())) : QString("Sunset today is at %1. Golden hour begins around %2. ").arg(localspots::spokenClock(ss.time()), localspots::spokenClock(golden));
            if (ss2.isValid()) nar += delta == 0 ? QString("Tomorrow's sunset is at about the same time. ") : QString("Tomorrow the sun sets at %1, %2 %3. ").arg(localspots::spokenClock(ss2.time()), plural(qAbs(delta), "minute", "minutes"), delta < 0 ? "earlier" : "later");
            if (!passed && dayMin > 0) nar += QString("We get %1 and %2 of daylight today.").arg(plural(dayMin / 60, "hour", "hours"), plural(dayMin % 60, "minute", "minutes"));
            QJsonObject sc = localspots::makeScene("story", say(nar));
            QJsonObject anim; anim["type"] = "sunset"; anim["cond"] = conditionOf(code); anim["passed"] = passed; anim["sunset"] = clock12(ss.time()); anim["sunrise"] = sr.isValid() ? clock12(sr.time()) : QString();
            anim["golden"] = clock12(golden); anim["tomorrow"] = ss2.isValid() ? clock12(ss2.time()) : QString(); anim["delta"] = delta;
            anim["daylight"] = dayMin > 0 ? QString("%1 h %2 m").arg(dayMin / 60).arg(dayMin % 60) : QString();
            addScene(sc, "Sunset", "ALMANAC", anim);
        }
    }
    // 4) moon
    if (segs.contains("moon")) {
        const QJsonObject m = moonInfo(now); const QString phase = m.value("phase").toString(); const int ill = qRound(m.value("illumination").toDouble());
        auto dateSay = [&](const QString& iso) { return us().toString(QDate::fromString(iso, Qt::ISODate), "dddd, MMMM d"); };
        QString nar = phase == "Full Moon" ? QString("Tonight brings a full moon. ") : phase == "New Moon" ? QString("Tonight is a new moon, so skies will be dark. ") : QString("Tonight's moon is a %1, %2 percent illuminated. ").arg(phase.toLower(), num(ill));
        nar += QString("The next full moon arrives around %1, and the next new moon around %2.").arg(dateSay(m.value("nextFull").toString()), dateSay(m.value("nextNew").toString()));
        QJsonObject sc = localspots::makeScene("story", say(nar));
        QJsonObject anim; anim["type"] = "moon"; anim["cond"] = conditionOf(code); anim["phase"] = phase; anim["illum"] = ill; anim["waxing"] = m.value("waxing"); anim["fraction"] = m.value("fraction");
        anim["nextFull"] = us().toString(QDate::fromString(m.value("nextFull").toString(), Qt::ISODate), "ddd, MMM d"); anim["nextNew"] = us().toString(QDate::fromString(m.value("nextNew").toString(), Qt::ISODate), "ddd, MMM d");
        addScene(sc, "Moon Phase", "ALMANAC", anim);
    }
    // 5) burn safety (after 6 PM the useful answer is tomorrow's)
    if (segs.contains("burn")) {
        const int bi = now.time().hour() >= 18 ? qMin(t + 1, int(days.size()) - 1) : t; const QString when = bi == t ? "today" : "tomorrow";
        double minRh = 100.0; const QJsonArray ht = hr.value("time").toArray(), hrh = hr.value("relative_humidity_2m").toArray();
        for (int i = 0; i < ht.size() && i < hrh.size(); ++i) { const QString ts = ht[i].toString(); if (ts.left(10) == days[bi].date && ts.mid(11, 2).toInt() >= 10 && ts.mid(11, 2).toInt() <= 18) minRh = qMin(minRh, hrh[i].toDouble()); }
        if (minRh > 99.0) minRh = cur.value("relative_humidity_2m").toDouble(50);
        double rain3 = 0; for (int i = qMax(0, bi - 3); i < bi; ++i) rain3 += days[i].rain;
        const QJsonObject b = burnSafety(days[bi].gust, minRh, rain3, days[bi].rain, days[bi].hi, alerts.value("alert").toString(), QDate::fromString(days[bi].date, Qt::ISODate));
        QStringList spoken; for (const QJsonValue& r : b.value("reasons").toArray()) { QString x = r.toString(); x.replace("mph", "miles per hour").replace(" in. of", " inches of").replace("%", " percent"); spoken << x; }
        QString nar = QString("Burn safety %1 is %2. %3. %4 ").arg(when, b.value("rating").toString().toLower(), cap1(spoken.mid(0, 3).join(", ")), b.value("advice").toString());
        const QString law = o.value("burnLaw").toBool(true) ? b.value("law").toString() : QString();      // Virginia's 4 PM law: a North Carolina market sets burnLaw false
        if (!law.isEmpty()) nar += law + " ";
        nar += "Check for local burn bans before you light anything.";
        QJsonObject sc = localspots::makeScene("story", say(nar));
        QJsonObject anim; anim["type"] = "burn"; anim["cond"] = conditionOf(code); anim["rating"] = b.value("rating"); anim["score"] = b.value("score"); anim["level"] = b.value("level"); anim["reasons"] = b.value("reasons");
        anim["advice"] = b.value("advice"); anim["law"] = law; anim["when"] = when; anim["alert"] = alerts.value("alert");
        addScene(sc, "Burn Safety", when == "today" ? "FIRE WEATHER TODAY" : "FIRE WEATHER TOMORROW", anim);
        lines << QString("Burn safety: %1").arg(b.value("rating").toString());
    }
    // 6) lake levels
    if (segs.contains("lakes")) {
        struct L { const char* key; const char* name; };
        static const L order[] = {{"smithMountain", "Smith Mountain Lake"}, {"leesville", "Leesville Lake"}, {"philpott", "Philpott Lake"}};
        QJsonArray rows; QStringList parts;
        for (const L& l : order) {
            const QJsonObject d = lakes.value(l.key).toObject(); if (!d.value("actual").isDouble()) continue;
            const double level = d.value("actual").toDouble(), full = d.value("fullPond").toDouble(0);
            double chg = d.contains("changeFt") ? d.value("changeFt").toDouble() : d.contains("prev24") ? level - d.value("prev24").toDouble() : qQNaN();
            QJsonObject r; r["label"] = l.name; r["value"] = QString::number(level, 'f', 2) + " ft";
            QString speak = QString("%1 is at %2 feet").arg(l.name, marketspots::spokenDecimal(level, 1));      // the card shows two decimals, the voice one
            if (full > 0) { const double diff = level - full; r["sub"] = QString("%1 ft %2 full pond (%3)").arg(QString::number(std::fabs(diff), 'f', 2), diff < 0 ? "below" : "above", QString::number(full, 'f', 0));
                speak += QString(", %1 feet %2 full pond").arg(marketspots::spokenDecimal(std::fabs(diff), 1), diff < 0 ? "below" : "above"); }
            else r["sub"] = "reading from USGS";
            if (!std::isnan(chg)) { r["change"] = QString("%1%2 ft 24h").arg(chg > 0.004 ? "+" : chg < -0.004 ? "-" : "").arg(QString::number(std::fabs(chg), 'f', 2)); r["tone"] = chg > 0.004 ? "up" : chg < -0.004 ? "down" : "flat";
                if (std::fabs(chg) >= 0.05) speak += QString(", %1 %2 feet over the past day").arg(chg > 0 ? "up" : "down", marketspots::spokenDecimal(std::fabs(chg), 1)); }
            rows.append(r); parts << speak;
        }
        if (!rows.isEmpty()) {
            QJsonObject sc = localspots::makeScene("story", say(parts.join(". ") + "."));
            sc["rows"] = rows;
            QJsonObject anim; anim["type"] = "lakes"; anim["cond"] = conditionOf(code); anim["isDay"] = isDay;
            addScene(sc, "Lake Levels", "ALMANAC", anim);
        }
    }
    if (scenes.isEmpty()) return localspots::skipResult("no weather segment had data");
    int k = 0; const int n = int(scenes.size());
    for (int i = 0; i < scenes.size(); ++i) { QJsonObject sc = scenes[i].toObject(); sc["counter"] = QString("%1 / %2").arg(++k).arg(n); scenes[i] = sc; }
    const QString closing = siteName.isEmpty() ? QString("That's your forecast and almanac.") : QString("That's your forecast and almanac from %1.").arg(siteName);
    QJsonObject outro = localspots::makeScene("outro", say(closing + (site.isEmpty() ? QString() : QString(" For more, visit %1.").arg(headline::spokenSite(site, siteName)))));
    outro["eyebrow"] = "FORECAST DATA"; outro["big"] = site.isEmpty() ? place : site; outro["sub"] = segs.contains("lakes") ? QString("Open-Meteo%1National Weather Service%1USGS%1Appalachian Power").arg(dot) : QString("Open-Meteo%1National Weather Service").arg(dot);
    scenes.append(outro);
    sources.append(localspots::makeSource("Forecast", "https://open-meteo.com/", QString(), "Open-Meteo"));
    QJsonObject local; local["source"] = "weather"; local["segments"] = n; local["fireAlert"] = alerts.value("alert");
    const Day& td = days[t];
    return localspots::makeScript(scenes, QString("%1\xC2\xB0 and %2 in %3").arg(qRound(cur.value("temperature_2m").toDouble())).arg(conditionText(code, isDay), place), lines.mid(0, 3), "Weather", o.value("energy").toInt(5), local, sources);
    Q_UNUSED(td);
}

} // namespace weatherspots
