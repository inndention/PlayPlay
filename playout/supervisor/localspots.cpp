// Local spots: cinema showtimes, 511 traffic alerts, MaxPreps scores and a featured-home rotation, as scene scripts
// for the SpotGenerator's scene render. Pure QtCore: see localspots.h.
#include "localspots.h"
#include "headlinenews.h"

#include <QHash>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace localspots {

using RX = QRegularExpression;
static const RX::PatternOption CI = RX::CaseInsensitiveOption;
static const RX::PatternOption DOT = RX::DotMatchesEverythingOption;
static QLocale us() { return QLocale(QLocale::English, QLocale::UnitedStates); }

// ---------------------------------------------------------------------------------------------
// Speech + display forms
// ---------------------------------------------------------------------------------------------
QString spokenNumber(qint64 n) {
    static const char* ones[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
    static const char* tens[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    if (n < 0) return "minus " + spokenNumber(-n);
    if (n < 20) return ones[n];
    if (n < 100) return QString(tens[n / 10]) + (n % 10 ? QString(" ") + ones[n % 10] : QString());
    if (n < 1000) return QString(ones[n / 100]) + " hundred" + (n % 100 ? " " + spokenNumber(n % 100) : QString());
    if (n < 1000000) return spokenNumber(n / 1000) + " thousand" + (n % 1000 ? " " + spokenNumber(n % 1000) : QString());
    if (n < 1000000000LL) return spokenNumber(n / 1000000) + " million" + (n % 1000000 ? " " + spokenNumber(n % 1000000) : QString());
    return QString::number(n);
}

QString spokenClock(const QTime& t, bool withSuffix) {
    int h = t.hour() % 12; if (h == 0) h = 12;
    QString s = spokenNumber(h);
    if (t.minute()) s += (t.minute() < 10 ? " oh " : " ") + spokenNumber(t.minute());
    if (withSuffix) s += t.hour() < 12 ? " A M" : " P M";
    return s;
}

static QString joinList(const QStringList& parts) {
    if (parts.size() <= 1) return parts.value(0);
    return parts.mid(0, parts.size() - 1).join(", ") + " and " + parts.last();
}

QString spokenTimeList(const QList<QTime>& times) {
    QStringList parts;
    for (int i = 0; i < times.size(); ++i) {
        const bool last = i + 1 == times.size() || (times[i].hour() < 12) != (times[i + 1].hour() < 12);
        parts << spokenClock(times[i], last);
    }
    return joinList(parts);
}

QString displayTimeList(const QList<QTime>& times) {
    QStringList parts;
    for (int i = 0; i < times.size(); ++i) {
        const bool last = i + 1 == times.size() || (times[i].hour() < 12) != (times[i + 1].hour() < 12);
        int h = times[i].hour() % 12; if (h == 0) h = 12;
        QString s = QString("%1:%2").arg(h).arg(times[i].minute(), 2, 10, QChar('0'));
        if (last) s += times[i].hour() < 12 ? " AM" : " PM";
        parts << s;
    }
    return parts.join(QString::fromUtf8(" \xC2\xB7 "));
}

static QString spokenDigits(const QString& digits) {
    static const char* w[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    QStringList o; for (const QChar c : digits) if (c.isDigit()) o << w[c.digitValue()];
    return o.join(' ');
}

QString spokenPhone(const QString& phone) {
    QString d; for (const QChar c : phone) if (c.isDigit()) d += c;
    if (d.size() == 11 && d.startsWith('1')) d = d.mid(1);
    if (d.size() != 10) return spokenDigits(d);
    return spokenDigits(d.left(3)) + ", " + spokenDigits(d.mid(3, 3)) + ", " + spokenDigits(d.mid(6));
}

QString spokenAddress(const QString& address) {
    QString s = headline::plainText(address);
    static const QList<QPair<RX, QString>> rules = {
        {RX("\\bDr\\b\\.?"), "Drive"}, {RX("\\bCt\\b\\.?"), "Court"}, {RX("\\bCir\\b\\.?"), "Circle"}, {RX("\\bPl\\b\\.?"), "Place"}, {RX("\\bTrl\\b\\.?"), "Trail"},
        {RX("\\bLn\\b\\.?"), "Lane"}, {RX("\\bRd\\b\\.?"), "Road"}, {RX("\\bSt\\b\\.?"), "Street"}, {RX("\\bAve\\b\\.?"), "Avenue"}, {RX("\\bBlvd\\b\\.?"), "Boulevard"},
        {RX("\\bHwy\\b\\.?"), "Highway"}, {RX("\\bPkwy\\b\\.?"), "Parkway"}, {RX("\\bExt\\b\\.?"), "Extension"}, {RX("\\bMt\\b\\.?"), "Mount"},
        {RX("\\bN\\b\\.?(?=\\s)"), "North"}, {RX("\\bS\\b\\.?(?=\\s)"), "South"}, {RX("\\bE\\b\\.?(?=\\s)"), "East"}, {RX("\\bW\\b\\.?(?=\\s)"), "West"},
    };
    for (const auto& r : rules) s.replace(r.first, r.second);
    return s.simplified();
}

QString speech(const QString& text) {
    QString s = headline::plainText(text);
    s.replace(QChar(0xFFFD), ", ").replace(QChar(0x2019), '\'').replace(QChar(0x2018), '\'').replace(QChar(0x201C), '"').replace(QChar(0x201D), '"')
     .replace(QChar(0x2014), ", ").replace(QChar(0x2013), "-").replace(QChar(0x2026), "...").replace(QChar(0x00A0), ' ')
     .replace('&', " and ").replace('%', " percent").replace('#', " number ").replace('|', ',').replace('*', ' ').replace('_', ' ');
    s.replace(RX("(https?://\\S+|www\\.\\S+)", CI), " ");
    // "$5" -> "five dollars" (the voice reads "$159,900" unevenly)
    static const RX money("\\$(\\d[\\d,]*)(?:\\.(\\d{2}))?");
    QString o; int last = 0; auto it = money.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next(); QString d = m.captured(1); d.remove(',');
        const qint64 v = d.toLongLong(); QString w = spokenNumber(v) + (v == 1 ? " dollar" : " dollars");
        o += s.mid(last, m.capturedStart() - last) + w; last = m.capturedEnd();
    }
    s = o + s.mid(last);
    return headline::speakable(s).simplified();
}

QString spokenRoad(const QString& in) {
    QString s = in;
    s.replace(RX("\\bI-(\\d+)"), "Interstate \\1");
    s.replace(RX("\\bUS[- ](\\d+)"), "U S \\1");
    s.replace(RX("\\bVA-(\\d+)"), "Virginia \\1");
    s.replace(RX("\\bSR-(\\d+)"), "State Route \\1");
    s.replace(RX("\\bRte?\\.?\\s*(\\d+)", CI), "Route \\1");
    s.replace(RX("\\s+,"), ",");
    s.replace(RX("\\bmph\\b", CI), "miles per hour");
    return s;
}

QString spokenRuntime(const QString& runtime) {
    static const RX rx("(?:(\\d+)\\s*h)?\\s*(?:(\\d+)\\s*m)?", CI);
    const auto m = rx.match(runtime.trimmed());
    const int h = m.captured(1).toInt(), mi = m.captured(2).toInt();
    if (!h && !mi) return QString();
    QStringList p;
    if (h) p << spokenNumber(h) + (h == 1 ? " hour" : " hours");
    if (mi) p << spokenNumber(mi) + (mi == 1 ? " minute" : " minutes");
    return p.join(' ');
}

double milesBetween(double lat1, double lon1, double lat2, double lon2) {
    const double r = 3958.8, toRad = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * toRad, dLon = (lon2 - lon1) * toRad;
    const double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(lat1 * toRad) * std::cos(lat2 * toRad) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2 * r * std::asin(std::min(1.0, std::sqrt(a)));
}

// ---------------------------------------------------------------------------------------------
// Script helpers
// ---------------------------------------------------------------------------------------------
static int wordsIn(const QString& s) { return s.split(RX("\\s+"), Qt::SkipEmptyParts).size(); }
static QJsonObject skipped(const QString& why) { QJsonObject o; o["skip"] = why; return o; }
static QJsonObject failed(const QString& why) { QJsonObject o; o["error"] = why; return o; }
static QString cap1(QString s) { if (!s.isEmpty()) s[0] = s[0].toUpper(); return s; }
static QString trimScreenImpl(QString s, int maxLen) {
    s = s.simplified();
    if (s.size() <= maxLen) return s;
    s = s.left(maxLen); const int sp = s.lastIndexOf(' '); if (sp > maxLen / 2) s = s.left(sp);
    while (s.endsWith(',') || s.endsWith(';') || s.endsWith(':')) s.chop(1);
    return s + "...";
}
/// A scene: narration is exactly what the voice reads (run through speech() by the caller).
static QJsonObject mkScene(const QString& kind, const QString& narration) {
    QJsonObject s; s["kind"] = kind; s["narration"] = narration.simplified(); s["words"] = wordsIn(narration);
    return s;
}
/// Wraps scenes into the script shape the scene render reads.
static QJsonObject finishScript(const QJsonArray& scenes, const QString& headlineText, const QStringList& lines, const QString& package, int energy, const QJsonObject& local, const QJsonArray& sources) {
    QString all; QJsonArray stories;
    for (const QJsonValue& v : scenes) {
        const QJsonObject sc = v.toObject(); all += sc.value("narration").toString() + " ";
        if (sc.value("kind").toString() != "story") continue;
        QJsonObject st; st["title"] = sc.value("title"); st["summary"] = sc.value("summary"); st["image"] = sc.value("image"); st["place"] = sc.value("place");
        st["category"] = sc.value("category"); st["detail"] = sc.value("detail"); st["narration"] = sc.value("narration"); stories.append(st);
    }
    QJsonObject s; s["headline"] = headlineText; QJsonArray l; for (const QString& x : lines) l.append(x); s["lines"] = l;
    s["narration"] = all.simplified(); s["scenes"] = scenes; s["stories"] = stories; s["package"] = package; s["energy"] = energy; s["writer"] = "template";
    s["local"] = local; s["sources"] = sources;
    return s;
}
static QJsonObject source(const QString& title, const QString& link, const QString& image = QString(), const QString& src = QString()) {
    QJsonObject o; o["title"] = title; o["link"] = link; o["image"] = image; o["source"] = src; return o;
}
QJsonObject makeScene(const QString& kind, const QString& narration) { return mkScene(kind, narration); }
QJsonObject makeScript(const QJsonArray& scenes, const QString& headlineText, const QStringList& lines, const QString& package, int energy, const QJsonObject& local, const QJsonArray& sources) { return finishScript(scenes, headlineText, lines, package, energy, local, sources); }
QJsonObject makeSource(const QString& title, const QString& link, const QString& image, const QString& src) { return source(title, link, image, src); }
QJsonObject skipResult(const QString& why) { return skipped(why); }
QJsonObject errorResult(const QString& why) { return failed(why); }
QString trimScreen(QString s, int maxLen) { return trimScreenImpl(s, maxLen); }
static QString dayName(const QDate& d) { return us().dayName(d.dayOfWeek(), QLocale::LongFormat); }
static QString shortDate(const QDate& d) { return us().toString(d, "dddd, MMM. d"); }
static QString clockText(const QTime& t) { return us().toString(t, "h:mm AP"); }

// ---------------------------------------------------------------------------------------------
// Cinema
// ---------------------------------------------------------------------------------------------
static QString attr(const QString& block, const QString& pattern) { return headline::plainText(RX(pattern, DOT).match(block).captured(1)); }

QJsonObject parseCinema(const QString& html) {
    QJsonObject out; QJsonArray movies;
    static const RX start("<div class=\"movie\"[^>]*itemprop=\"itemListElement\"");
    QList<int> at; auto it = start.globalMatch(html); while (it.hasNext()) at << it.next().capturedStart();
    const int mainEnd = html.indexOf("</main>");
    static const RX screening("<div itemscope itemtype=\"https://schema\\.org/ScreeningEvent\">(.*?)</div>", DOT);
    static const RX clock("(\\d{1,2}):(\\d{2})\\s*([AP]M)", CI);
    for (int i = 0; i < at.size(); ++i) {
        const int end = i + 1 < at.size() ? at[i + 1] : (mainEnd > at[i] ? mainEnd : html.size());
        const QString b = html.mid(at[i], end - at[i]);
        QJsonObject m;
        m["title"] = attr(b, "<meta itemprop=\"name\" content=\"([^\"]+)\"");
        if (m.value("title").toString().isEmpty()) continue;
        QString poster = RX("<img\\s+src=\"([^\"]+)\"").match(b).captured(1);
        poster.replace("/t/p/w500/", "/t/p/w780/");                                  // TMDB serves a larger rendition at the same path
        m["poster"] = poster;
        m["rating"] = attr(b, "<span itemprop=\"contentRating\">(.*?)</span>").simplified();
        m["runtime"] = attr(b, "<span itemprop=\"duration\">(.*?)</span>");
        QString desc = attr(b, "<p itemprop=\"description\">(.*?)</p>");
        if (desc.isEmpty()) desc = attr(b, "<meta itemprop=\"description\" content=\"([^\"]*)\"");
        m["description"] = desc;
        m["release"] = attr(b, "itemprop=\"datePublished\">([^<]+)<");
        QJsonArray days;
        auto sit = screening.globalMatch(b);
        while (sit.hasNext()) {
            const QString s = sit.next().captured(1);
            const QString day = attr(s, "<span itemprop=\"dayOfWeek\">(\\w+)</span>");
            QJsonArray times; QSet<QString> seen;
            auto tit = RX("itemprop=\"startDate\"\\s+content=\"([^\"]*)\"").globalMatch(s);
            while (tit.hasNext()) {
                const auto cm = clock.match(tit.next().captured(1)); if (!cm.hasMatch()) continue;
                int h = cm.captured(1).toInt() % 12; if (cm.captured(3).toUpper() == "PM") h += 12;
                const QString hhmm = QString("%1:%2").arg(h, 2, 10, QChar('0')).arg(cm.captured(2).toInt(), 2, 10, QChar('0'));
                if (!seen.contains(hhmm)) { seen.insert(hhmm); times.append(hhmm); }
            }
            if (day.isEmpty() || times.isEmpty()) continue;
            QJsonObject d; d["day"] = day; d["times"] = times; days.append(d);
        }
        m["days"] = days;
        movies.append(m);
    }
    out["movies"] = movies;
    const QString street = attr(html, "itemprop=\"streetAddress\">([^<]+)<"), city = attr(html, "itemprop=\"addressLocality\">([^<]+)<");
    out["address"] = street; out["city"] = city;
    out["landmark"] = attr(html, "itemprop=\"postalCode\">[^<]*</span>\\s*<br>\\s*([^<]+)<").simplified();
    const QString price = attr(html, "itemprop=\"priceRange\">([^<]+)<");
    const auto pm = RX("\\$(\\d+)\\s+except\\s+(\\d{1,2})\\s*(?:AM|PM)?\\s*shows?\\s+\\$(\\d+)", CI).match(price);
    if (pm.hasMatch()) { QJsonObject p; p["base"] = pm.captured(1).toInt(); p["exceptHour"] = pm.captured(2).toInt(); p["exceptPrice"] = pm.captured(3).toInt(); out["price"] = p; }
    return out;
}

static QString spokenRating(const QString& r) {
    const QString u = r.trimmed().toUpper();
    if (u.isEmpty()) return QString();
    if (u == "NR" || u == "NOT RATED" || u == "UNRATED") return "not rated";
    QString s = u; s.replace("PG-13", "P G thirteen").replace("PG", "P G").replace("NC-17", "N C seventeen");
    return s;
}

QJsonObject cinemaScript(const QJsonObject& page, const QDateTime& now, const QJsonObject& o) {
    const QString siteName = o.value("siteName").toString("Hollywood Cinema"), place = o.value("place").toString("Martinsville"), site = o.value("site").toString();
    const int maxMovies = qBound(1, o.value("maxMovies").toInt(6), 8), leadMin = o.value("leadMin").toInt(10), synopsisWords = qBound(10, o.value("synopsisWords").toInt(24), 60);
    struct Show { QJsonObject movie; int offset; QList<QTime> times; QDate date; };
    QList<Show> shows;
    // The site lists ONE week (Wednesday to Sunday) and keeps days that are already over, so a day name cannot simply mean "the next such
    // weekday". The week starts on the day most movies list first; each listed day maps to a date in that week, and when the whole
    // listed week is behind us the page is taken as next week's.
    const QJsonArray movies = page.value("movies").toArray();
    auto dowOf = [](const QString& name) { for (int d = 1; d <= 7; ++d) if (us().dayName(d, QLocale::LongFormat).compare(name, Qt::CaseInsensitive) == 0) return d; return 0; };
    QHash<int, int> firstDays; int lastIdx = 0;
    for (const QJsonValue& mv : movies) { const QJsonArray days = mv.toObject().value("days").toArray(); if (!days.isEmpty()) firstDays[dowOf(days.first().toObject().value("day").toString())]++; }
    int startDow = 0, best = 0; for (const QJsonValue& mv : movies) { const QJsonArray days = mv.toObject().value("days").toArray(); if (days.isEmpty()) continue; const int d = dowOf(days.first().toObject().value("day").toString()); if (firstDays.value(d) > best) { best = firstDays.value(d); startDow = d; } }
    if (startDow == 0) return skipped("no showtimes are listed");
    for (const QJsonValue& mv : movies) for (const QJsonValue& dv : mv.toObject().value("days").toArray()) lastIdx = qMax(lastIdx, (dowOf(dv.toObject().value("day").toString()) - startDow + 7) % 7);
    const QDate today = now.date();
    QDate weekStart = today.addDays(-((today.dayOfWeek() - startDow + 7) % 7));
    if (weekStart.addDays(lastIdx) < today) weekStart = weekStart.addDays(7);
    const int cutoffMin = now.time().hour() * 60 + now.time().minute() + leadMin;                  // today's shows that start sooner than leadMin from now are not offered
    for (const QJsonValue& mv : movies) {
        const QJsonObject m = mv.toObject(); Show best1{m, 1 << 20, {}, QDate()};
        for (const QJsonValue& dv : m.value("days").toArray()) {
            const QJsonObject day = dv.toObject(); const int dow = dowOf(day.value("day").toString()); if (!dow) continue;
            const QDate date = weekStart.addDays((dow - startDow + 7) % 7); const int offset = int(today.daysTo(date)); if (offset < 0 || offset >= best1.offset) continue;
            QList<QTime> ts; for (const QJsonValue& t : day.value("times").toArray()) { const QTime tm = QTime::fromString(t.toString(), "HH:mm"); if (!tm.isValid()) continue; if (offset == 0 && tm.hour() * 60 + tm.minute() <= cutoffMin) continue; ts << tm; }
            std::sort(ts.begin(), ts.end());
            if (!ts.isEmpty()) best1 = {m, offset, ts, date};
        }
        if (best1.offset < (1 << 20)) shows.append(best1);
    }
    if (shows.isEmpty()) return skipped("no upcoming showtimes are listed");
    std::stable_sort(shows.begin(), shows.end(), [](const Show& a, const Show& b) { return a.offset != b.offset ? a.offset < b.offset : a.times.first() < b.times.first(); });
    if (shows.size() > maxMovies) shows = shows.mid(0, maxMovies);
    const bool anyToday = shows.first().offset == 0;
    QJsonArray scenes, rundown; QStringList titles;
    for (const Show& s : shows) { rundown.append(s.movie.value("title")); titles << s.movie.value("title").toString(); }
    QJsonObject intro = mkScene("intro", speech(QString("Now playing at %1 in %2. %3").arg(siteName, place, anyToday ? "Here's what's showing today." : "Here's what's showing next.")));
    intro["rundown"] = rundown; intro["eyebrow"] = anyToday ? "TODAY'S SHOWTIMES" : "UPCOMING SHOWTIMES"; intro["sub"] = QString("%1 \xC2\xB7 %2").arg(siteName, place);
    intro["image"] = shows.first().movie.value("poster");
    scenes.append(intro);
    QJsonArray sources;
    int k = 0;
    for (const Show& s : shows) {
        ++k; const QString title = s.movie.value("title").toString(), rating = s.movie.value("rating").toString(), runtime = s.movie.value("runtime").toString();
        const QString syn = headline::firstSentence(headline::plainText(s.movie.value("description").toString()).replace(QChar(0xFFFD), ", "), synopsisWords);
        QString nar = title;
        const QString rt = spokenRuntime(runtime);
        if (!rating.isEmpty()) nar += ", rated " + spokenRating(rating) + (rt.isEmpty() ? QString() : ", runs " + rt);
        else if (!rt.isEmpty()) nar += " runs " + rt;                                                 // a film the site has not rated yet
        nar += ". ";
        if (!syn.isEmpty()) nar += syn + (syn.endsWith('.') || syn.endsWith('!') || syn.endsWith('?') ? " " : ". ");
        nar += (s.offset == 0 ? "Showtimes today are " : s.offset == 1 ? "Showtimes tomorrow are " : "Showtimes on " + dayName(s.date) + " are ") + spokenTimeList(s.times) + ".";
        QJsonObject sc = mkScene("story", speech(nar));
        sc["title"] = title; sc["category"] = rating; sc["detail"] = displayTimeList(s.times);
        sc["place"] = (s.offset == 0 ? QString("Today") : s.offset == 1 ? QString("Tomorrow") : dayName(s.date)).toUpper() + (runtime.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ") + runtime.toUpper());
        sc["summary"] = trimScreen(syn, 150); sc["image"] = s.movie.value("poster"); sc["counter"] = QString("%1 / %2").arg(k).arg(shows.size());
        scenes.append(sc);
        sources.append(source(title, QString(), s.movie.value("poster").toString(), site));
    }
    // closing: where, what it costs, where to buy
    const QString addr = page.value("address").toString(), city = page.value("city").toString(place), landmark = page.value("landmark").toString();
    QString nar = siteName;
    if (!addr.isEmpty()) nar += " is at " + spokenAddress(addr) + (landmark.isEmpty() ? QString() : ", " + landmark.left(1).toLower() + landmark.mid(1));
    nar += ". ";
    const QJsonObject price = page.value("price").toObject();
    QString subLine = addr.isEmpty() ? QString() : addr + QString::fromUtf8(" \xC2\xB7 ") + city;
    if (!price.isEmpty()) {
        const QTime ex(price.value("exceptHour").toInt() % 12 + 12, 0);
        nar += QString("All tickets are %1 dollars, except %2 shows, which are %3 dollars. ").arg(spokenNumber(price.value("base").toInt()), spokenClock(ex), spokenNumber(price.value("exceptPrice").toInt()));
        subLine += QString::fromUtf8("  \xC2\xB7  All tickets $%1, %2 PM shows $%3").arg(price.value("base").toInt()).arg(price.value("exceptHour").toInt()).arg(price.value("exceptPrice").toInt());
    }
    if (!site.isEmpty()) nar += "Buy tickets online at " + headline::spokenSite(site, siteName) + ".";
    QJsonObject outro = mkScene("outro", speech(nar));
    outro["eyebrow"] = "BUY TICKETS ONLINE"; outro["big"] = site.isEmpty() ? siteName : site; outro["sub"] = subLine; outro["image"] = shows.first().movie.value("poster");
    scenes.append(outro);
    QJsonObject local; local["source"] = "cinema"; local["movies"] = titles.size(); local["titles"] = QJsonArray::fromStringList(titles);
    return finishScript(scenes, QString("Now playing: %1").arg(titles.mid(0, 2).join(", ")), titles.mid(0, 3), "Now Playing", o.value("energy").toInt(7), local, sources);
}

// ---------------------------------------------------------------------------------------------
// Traffic (VDOT 511)
// ---------------------------------------------------------------------------------------------
QJsonArray parseIncidents(const QByteArray& geojson, const QString& layer) {
    QJsonArray out;
    const QJsonObject doc = QJsonDocument::fromJson(geojson).object();
    for (const QJsonValue& fv : doc.value("features").toArray()) {
        const QJsonObject f = fv.toObject(), p = f.value("properties").toObject(), g = f.value("geometry").toObject();
        QJsonArray c = g.value("coordinates").toArray();
        if (g.value("type").toString() != "Point" && !c.isEmpty() && c.first().isArray()) c = c.first().toArray();   // lines: the first vertex
        if (c.size() < 2) continue;
        QJsonObject i; i["id"] = f.value("id").toString(p.value("id").toString()); i["layer"] = layer; i["priority"] = p.value("priority").toString();
        i["route"] = p.value("route").toString(); i["type"] = p.value("type").toString(); i["location"] = p.value("location").toString();
        i["message"] = p.value("message_511").toString(); i["lat"] = c.at(1).toDouble(); i["lon"] = c.at(0).toDouble();
        if (i.value("message").toString().isEmpty()) continue;
        out.append(i);
    }
    return out;
}

/// "I-81N" -> "I-81 North", "Rt. 672N (Patrick County)" -> "Route 672 North", "US-19N" -> "US 19 North".
static QString routeDisplay(const QString& route) {
    static const RX rx("^(I|US|VA|SR|Rt\\.?|Rte\\.?)[-\\s]?(\\d+)([NSEW])?\\b\\s*([^()]*)", CI);
    const auto m = rx.match(route.trimmed());
    if (!m.hasMatch()) return route.section('(', 0, 0).trimmed();
    QString kind = m.captured(1).toUpper().remove('.'); QString num = m.captured(2);
    QString head = kind == "I" ? "I-" + num : kind == "US" ? "US " + num : kind == "VA" ? "VA " + num : kind == "SR" ? "SR " + num : "Route " + num;
    static const QHash<QString, QString> dirs = {{"N", "North"}, {"S", "South"}, {"E", "East"}, {"W", "West"}};
    const QString dir = dirs.value(m.captured(3).toUpper());
    const QString rest = m.captured(4).trimmed();
    return head + (dir.isEmpty() ? QString() : " " + dir) + (rest.isEmpty() ? QString() : " " + rest);
}
static QString countyOf(const QString& message) {
    auto m = RX("\\bin (?:the )?([A-Z][A-Za-z.' -]*?\\s(?:County|City))\\b").match(message);
    if (m.hasMatch()) return m.captured(1);
    m = RX("\\bin the County of ([A-Z][A-Za-z.' -]+?)[,.]").match(message);
    return m.hasMatch() ? m.captured(1) + " County" : QString();
}
/// 3 road closed / 2 lanes closed / 1 delays / 0 shoulder only: what a driver should hear first.
static int severity(const QString& message, QString* label) {
    int sev = 1; QString l = "DELAYS POSSIBLE";
    if (RX("\\ball (?:\\w+ )?lanes? (?:are|is) closed|road (?:is )?closed|closed to traffic", CI).match(message).hasMatch()) { sev = 3; l = "ROAD CLOSED"; }
    else if (RX("\\blane closures? exist|\\b(?:left|right|center|middle|\\d+) lanes? (?:is|are) closed|lanes? (?:is|are) blocked|lane closed", CI).match(message).hasMatch()) { sev = 2; l = "LANE CLOSURES"; }
    else if (RX("shoulder", CI).match(message).hasMatch() && !RX("lane", CI).match(message).hasMatch()) { sev = 0; l = "SHOULDER CLOSED"; }
    if (label) *label = l;
    return sev;
}
/// The 511 message in speech form, whole sentences only, cut at a word budget.
static QString incidentNarration(const QString& message, int maxWords) {
    QStringList keep; int n = 0;
    for (const QString& s : headline::sentences(spokenRoad(headline::plainText(message)))) {      // "Rt. 672" -> "Route 672" first: the abbreviation dot must not split the sentence
        if (s.contains("...")) break;
        const int w = wordsIn(s); if (!keep.isEmpty() && n + w > maxWords) break;
        keep << s; n += w;
    }
    return speech(keep.join(' '));
}

QJsonObject trafficScript(const QJsonArray& all, bool feedOk, const QDateTime& now, const QJsonObject& o) {
    if (!feedOk) return failed("the 511 traffic feed could not be read");
    const double lat = o.value("lat").toDouble(36.69), lon = o.value("lon").toDouble(-79.87), radius = o.value("radiusMi").toDouble(45);
    const QString place = o.value("place").toString("Martinsville"), siteName = o.value("siteName").toString("VDOT 511"), site = o.value("site").toString("511virginia.org");
    const int maxScenes = qBound(1, o.value("maxScenes").toInt(5), 8);
    const QString area = o.value("area").toString("Henry County");                  // the county (or counties) the "all clear" names: an area market sets it
    struct Inc { QJsonObject i; double mi; int sev; QString label; };
    QList<Inc> nearby; QSet<QString> ids;
    for (const QJsonValue& v : all) {
        const QJsonObject i = v.toObject(); const double mi = milesBetween(lat, lon, i.value("lat").toDouble(), i.value("lon").toDouble());
        if (mi > radius) continue;
        const QString id = i.value("id").toString(); if (!id.isEmpty() && ids.contains(id)) continue; ids.insert(id);
        Inc c{i, mi, 0, QString()}; c.sev = severity(i.value("message").toString(), &c.label); nearby << c;
    }
    std::stable_sort(nearby.begin(), nearby.end(), [](const Inc& a, const Inc& b) { return a.sev != b.sev ? a.sev > b.sev : a.mi < b.mi; });
    const int total = nearby.size(); if (nearby.size() > maxScenes) nearby = nearby.mid(0, maxScenes);
    const QString asOfSay = spokenClock(now.time()), asOfShow = clockText(now.time());
    QJsonArray scenes, sources;
    QJsonObject local; local["source"] = "traffic"; local["incidents"] = total; local["radiusMi"] = radius;
    if (nearby.isEmpty()) {
        QJsonObject sc = mkScene("story", speech(QString("Traffic watch. As of %1, %2 reports no crashes, closures or weather closures near %3 and %4. Drive safely.").arg(asOfSay, siteName, place, area)));
        sc["title"] = "No traffic alerts right now"; sc["category"] = "ALL CLEAR"; sc["place"] = QString("%1 AREA").arg(place.toUpper()); sc["detail"] = "As of " + asOfShow;
        sc["summary"] = QString("%1 shows no crashes, closures or weather closures within %2 miles of %3.").arg(siteName).arg(qRound(radius)).arg(place); sc["counter"] = QString();
        scenes.append(sc);
        return finishScript(scenes, "No traffic alerts near " + place, {QString("As of %1, no alerts within %2 miles").arg(asOfShow).arg(qRound(radius))}, "Traffic Alerts", o.value("energy").toInt(8), local, sources);
    }
    QJsonArray rundown; QStringList routes;
    for (const Inc& c : nearby) { const QString r = routeDisplay(c.i.value("route").toString()); rundown.append(r); routes << r; }
    QJsonObject intro = mkScene("intro", speech(QString("Traffic alert. As of %1, here %2 near %3.").arg(asOfSay, total == 1 ? "is the one alert" : QString("are the %1 alerts").arg(spokenNumber(nearby.size())), place)));
    intro["rundown"] = rundown; intro["eyebrow"] = "ROAD CONDITIONS"; intro["sub"] = QString("%1 \xC2\xB7 as of %2").arg(siteName, asOfShow);
    scenes.append(intro);
    int k = 0;
    for (const Inc& c : nearby) {
        ++k; const QString msg = c.i.value("message").toString(), county = countyOf(msg);
        QJsonObject sc = mkScene("story", incidentNarration(msg, 44));
        sc["title"] = routeDisplay(c.i.value("route").toString()); sc["category"] = c.i.value("type").toString().toUpper();
        sc["place"] = (county.isEmpty() ? place : county).toUpper(); sc["detail"] = c.label;
        // on the card: where, then what the 511 message says beyond its first (location) sentence, e.g. which lanes are closed
        QString where = c.i.value("location").toString().simplified(); where.replace(RX("\\s+,"), ",");
        const QStringList parts = headline::sentences(spokenRoad(headline::plainText(msg)));
        QStringList rest; for (int p = 1; p < parts.size() && rest.size() < 2; ++p) { if (parts[p].contains("...")) break; rest << parts[p]; }
        sc["summary"] = trimScreen((where.isEmpty() ? QString() : "Near " + where + ". ") + rest.join(' '), 190);
        sc["counter"] = QString("%1 / %2").arg(k).arg(nearby.size());
        scenes.append(sc);
        sources.append(source(c.i.value("type").toString() + " - " + c.i.value("route").toString(), "https://511.vdot.virginia.gov/", QString(), c.i.value("layer").toString()));
    }
    QJsonObject outro = mkScene("outro", speech(o.value("outroText").toString("For the latest road conditions before you go, visit five eleven virginia dot org.")));
    outro["eyebrow"] = "BEFORE YOU GO"; outro["big"] = site; outro["sub"] = QString("Source: %1 \xC2\xB7 as of %2").arg(siteName, asOfShow);
    scenes.append(outro);
    const Inc& top = nearby.first();
    return finishScript(scenes, QString("%1: %2").arg(routeDisplay(top.i.value("route").toString()), top.i.value("type").toString()), routes.mid(0, 3), "Traffic Alerts", o.value("energy").toInt(8), local, sources);
}

// ---------------------------------------------------------------------------------------------
// Sports (MaxPreps)
// ---------------------------------------------------------------------------------------------
QJsonArray defaultSchools() {
    auto school = [](const char* name, const char* state, std::initializer_list<const char*> bases) {
        QJsonObject o; o["name"] = name; o["state"] = state; QJsonArray b; for (const char* x : bases) b.append(x); o["bases"] = b; return o;
    };
    return {school("Martinsville", "VA", {"https://www.maxpreps.com/va/martinsville/martinsville-bulldogs/"}),
            school("Magna Vista", "VA", {"https://www.maxpreps.com/va/ridgeway/magna-vista-warriors/"}),
            school("Bassett", "VA", {"https://www.maxpreps.com/va/bassett/bassett-bengals/"}),
            school("Patrick County", "VA", {"https://www.maxpreps.com/va/stuart/patrick-county-cougars/"}),
            school("Carlisle", "VA", {"https://www.maxpreps.com/va/martinsville/carlisle-chiefs/", "https://www.maxpreps.com/va/martinsville/carlisle-school-chiefs/"})};
}
QJsonArray defaultSports() {
    auto sport = [](const char* slug, const char* label) { QJsonObject o; o["slug"] = slug; o["label"] = label; return o; };
    return {sport("football", "Football"), sport("basketball", "Boys Basketball"), sport("girls-basketball", "Girls Basketball"), sport("baseball", "Baseball"), sport("softball", "Softball"),
            sport("soccer", "Boys Soccer"), sport("girls-soccer", "Girls Soccer"), sport("volleyball", "Girls Volleyball"), sport("wrestling", "Wrestling")};
}
QString activeSeasonLabel(const QDate& d) {
    const int start = d.month() >= 7 ? d.year() : d.year() - 1;
    return QString("%1-%2").arg(start % 100, 2, 10, QChar('0')).arg((start + 1) % 100, 2, 10, QChar('0'));
}
static QString normSchool(const QString& name) {
    QString s = name.toLower();
    s.replace(RX("\\([a-z]{2}\\)"), " "); s.replace(RX("\\b(high school|hs|school|academy)\\b|[^a-z0-9 ]"), " ");
    return s.simplified();
}
static QString cleanOpponent(QString s) { s.replace(RX("\\s*\\([^)]*\\)"), ""); return s.simplified(); }

QJsonArray parseTeamSchedule(const QByteArray& html, const QString& schoolName, const QString& sportLabel, const QString& stateCode, const QString& season) {
    static const RX nextData("<script\\b[^>]*\\bid=\"__NEXT_DATA__\"[^>]*>(.*?)</script>", DOT);
    QJsonArray out;
    const auto m = nextData.match(QString::fromUtf8(html)); if (!m.hasMatch()) return out;
    const QJsonObject page = QJsonDocument::fromJson(m.captured(1).toUtf8()).object().value("props").toObject().value("pageProps").toObject();
    const QJsonObject td = page.value("teamContext").toObject().value("data").toObject();
    const QString teamId = td.value("teamId").toString();
    if (teamId.isEmpty() || !page.value("contests").isArray()) return out;
    if (td.value("stateCode").toString().toUpper() != stateCode.toUpper()) return out;
    if (!season.isEmpty() && td.value("year").toString() != season) return out;
    const QString first = normSchool(schoolName).section(' ', 0, 0);
    if (!first.isEmpty() && !normSchool(td.value("schoolName").toString()).contains(first)) return out;
    static const RX scheduled("^The (.+?) (?:varsity|junior varsity|jv|freshman)\\b.*?\\bhas an? (away|home|neutral)\\b(?: ([a-z -]+?) (?:game|match|contest))?", CI);
    static const RX played("\\bthe (.+?) (?:varsity|junior varsity|jv|freshman)\\b.*?\\b(?:won|lost|tied) their (away|home|neutral)\\b", CI);
    for (const QJsonValue& cv : page.value("contests").toArray()) {
        const QJsonArray c = cv.toArray(); if (c.size() < 39) continue;
        const QJsonArray p1 = c.at(37).toArray(), p2 = c.at(38).toArray(); if (p1.isEmpty() || p2.isEmpty()) continue;
        const bool firstIsUs = p1.at(1).toString() == teamId, secondIsUs = p2.at(1).toString() == teamId; if (!firstIsUs && !secondIsUs) continue;
        const QJsonArray sel = firstIsUs ? p1 : p2, opp = firstIsUs ? p2 : p1;
        const QString opponent = cleanOpponent(opp.at(14).toString()); if (opponent.isEmpty() || opponent == "Unknown Opponent") continue;
        const QString start = c.at(11).toString(); if (start.isEmpty()) continue;
        const QDateTime st = QDateTime::fromString(start, Qt::ISODate); if (!st.isValid()) continue;
        const bool hasScores = sel.at(6).isDouble() && opp.at(6).isDouble();
        const QString result = sel.at(3).toString().trimmed();
        const bool isFinal = hasScores && !result.isEmpty() && QString("WLT").contains(result.at(0));
        const QString sentence = c.at(29).toString();
        int home = -1; bool neutral = false; QString kind;
        auto sm = scheduled.match(sentence), pm = played.match(sentence);
        const auto& hm = sm.hasMatch() ? sm : pm;
        if (hm.hasMatch()) {
            const QString venue = hm.captured(2).toLower(); const bool describesUs = normSchool(hm.captured(1)) == normSchool(schoolName);     // the sentence describes the VISITING team
            if (venue == "neutral") { neutral = true; home = 0; } else if (venue == "away") home = describesUs ? 0 : 1; else home = describesUs ? 1 : 0;
            if (sm.hasMatch()) kind = sm.captured(3).trimmed();
        }
        QJsonObject g; g["school"] = schoolName; g["sport"] = sportLabel; g["opponent"] = opponent; g["startLocal"] = st.toString("yyyy-MM-ddTHH:mm:ss");
        g["tbd"] = st.time() == QTime(0, 0); g["final"] = isFinal; g["home"] = home; g["neutral"] = neutral; g["kind"] = kind;
        if (isFinal) { g["ourScore"] = sel.at(6).toInt(); g["theirScore"] = opp.at(6).toInt(); g["result"] = result.left(1).toUpper(); }
        out.append(g);
    }
    return out;
}

namespace {
struct Game { QJsonObject o; qint64 startMs = 0; QDate date; QTime time; bool bothLocal = false; };
}
static QString venueWords(const Game& g) { return g.o.value("neutral").toBool() ? "neutral site" : g.o.value("home").toInt(-1) == 1 ? "home" : g.o.value("home").toInt(-1) == 0 ? "away" : QString(); }

QJsonObject sportsScript(const QJsonArray& games, const QDateTime& now, const QJsonObject& o) {
    const int recentDays = o.value("recentDays").toInt(4), upcomingHours = o.value("upcomingHours").toInt(48);
    const int maxResults = qBound(0, o.value("maxResults").toInt(5), 8), maxUpcoming = qBound(0, o.value("maxUpcoming").toInt(4), 8);
    const QString siteName = o.value("siteName").toString("Get Star News"), site = o.value("site").toString();
    const qint64 nowMs = now.toMSecsSinceEpoch();
    // local schools (for local-vs-local dedupe) and the games, one record per real game
    QSet<QString> locals; for (const QJsonValue& v : games) locals.insert(normSchool(v.toObject().value("school").toString()));
    QList<Game> list; QSet<QString> seen;
    for (const QJsonValue& v : games) {
        Game g; g.o = v.toObject(); const QDateTime st = QDateTime::fromString(g.o.value("startLocal").toString(), Qt::ISODate); if (!st.isValid()) continue;
        const QDateTime at(st.date(), st.time(), now.timeZone()); g.startMs = at.toMSecsSinceEpoch(); g.date = st.date(); g.time = st.time();
        const QString a = normSchool(g.o.value("school").toString()), b = normSchool(g.o.value("opponent").toString());
        g.bothLocal = locals.contains(b);
        const QString key = st.date().toString(Qt::ISODate) + "|" + g.o.value("sport").toString() + "|" + (a < b ? a + "|" + b : b + "|" + a);
        if (seen.contains(key)) continue; seen.insert(key); list << g;
    }
    QList<Game> results, upcoming;
    const qint64 day = 86400000LL;
    for (const Game& g : list) {
        if (g.o.value("final").toBool()) { if (g.startMs <= nowMs && g.startMs >= nowMs - recentDays * day) results << g; }
        else if (g.startMs >= nowMs - 3 * 3600000LL && g.startMs <= nowMs + upcomingHours * 3600000LL) upcoming << g;       // a game older than 3 h with no score is over, not "upcoming"
    }
    std::stable_sort(results.begin(), results.end(), [](const Game& a, const Game& b) { return a.startMs > b.startMs; });
    std::stable_sort(upcoming.begin(), upcoming.end(), [](const Game& a, const Game& b) { return a.startMs < b.startMs; });
    if (results.size() > maxResults) results = results.mid(0, maxResults);
    if (upcoming.size() > maxUpcoming) upcoming = upcoming.mid(0, maxUpcoming);
    if (results.isEmpty() && upcoming.isEmpty()) return skipped("no recent finals or games in the next two days");
    const QDate today = now.date();
    auto spokenSport = [](const QString& label) { return label.toLower(); };
    QJsonArray scenes, sources, rundown; QStringList lines;
    int total = results.size() + upcoming.size(), k = 0;
    // rundown lines + intro
    // A final from the point of view of the local school; between two local schools, the winner's.
    struct View { QString school, opp, res; int mine = 0, theirs = 0; };
    auto viewOf = [](const Game& g) {
        View v; v.school = g.o.value("school").toString(); v.opp = g.o.value("opponent").toString(); v.res = g.o.value("result").toString();
        v.mine = g.o.value("ourScore").toInt(); v.theirs = g.o.value("theirScore").toInt();
        if (g.bothLocal && v.res == "L") { std::swap(v.school, v.opp); std::swap(v.mine, v.theirs); v.res = "W"; }
        return v;
    };
    for (const Game& g : results) {
        const View v = viewOf(g);
        rundown.append(QString("%1 %2, %3 %4").arg(v.school).arg(v.mine).arg(v.opp).arg(v.theirs) + (v.res == "T" ? " (T)" : v.res == "W" ? " (W)" : " (L)"));
    }
    for (const Game& g : upcoming) rundown.append(QString("%1: %2 %3 %4").arg(g.date == today ? "Today" : g.date == today.addDays(1) ? "Tomorrow" : dayName(g.date), g.o.value("school").toString(), g.o.value("home").toInt(-1) == 1 ? "vs" : "at", g.o.value("opponent").toString()));
    QJsonArray rd; for (int i = 0; i < rundown.size() && i < 5; ++i) rd.append(rundown[i]);
    const QString areaName = o.value("area").toString();       // an area market words it "Danville and Pittsylvania County sports update"
    QString introNar = QString("Here's your %1 sports update from %2.").arg(areaName.isEmpty() ? QString("local") : areaName, siteName);
    if (!results.isEmpty()) introNar += QString(" %1 recent %2").arg(cap1(spokenNumber(results.size())), results.size() == 1 ? "result" : "results");
    if (!results.isEmpty() && !upcoming.isEmpty()) introNar += ", and "; else if (!results.isEmpty()) introNar += ".";
    if (!upcoming.isEmpty()) introNar += QString("%1 %2 coming up in the next two days.").arg(results.isEmpty() ? cap1(spokenNumber(upcoming.size())) : spokenNumber(upcoming.size()), upcoming.size() == 1 ? "game" : "games");
    QJsonObject intro = mkScene("intro", speech(introNar)); intro["rundown"] = rd; intro["eyebrow"] = "LOCAL SPORTS"; intro["sub"] = siteName; scenes.append(intro);
    for (const Game& g : results) {
        ++k; const View v = viewOf(g); const QString school = v.school, opp = v.opp, sport = g.o.value("sport").toString();
        const int mine = v.mine, theirs = v.theirs; const QString res = v.res;
        const int diff = today.toJulianDay() - g.date.toJulianDay(); const bool evening = g.time.hour() >= 17;
        const QString when = diff <= 0 ? (evening ? "Tonight" : "Today") : diff == 1 ? (evening ? "Last night" : "Yesterday") : "On " + dayName(g.date);
        QString nar;
        if (res == "T") nar = QString("%1, %2 and %3 tied in %4, %5 to %5.").arg(when, school, opp, spokenSport(sport), spokenNumber(mine));
        else if (res == "W") nar = QString("%1, %2 beat %3 in %4, %5 to %6.").arg(when, school, opp, spokenSport(sport), spokenNumber(mine), spokenNumber(theirs));
        else nar = QString("%1, %2 fell to %3 in %4, %5 to %6.").arg(when, school, opp, spokenSport(sport), spokenNumber(theirs), spokenNumber(mine));
        QJsonObject sc = mkScene("story", speech(nar));
        // winner first, the way a scoreboard reads: a loss by the local school shows the opponent's score first
        sc["title"] = res == "L" ? QString("%1 %2, %3 %4").arg(opp).arg(theirs).arg(school).arg(mine) : QString("%1 %2, %3 %4").arg(school).arg(mine).arg(opp).arg(theirs);
        sc["category"] = QString("FINAL \xC2\xB7 %1").arg(sport.toUpper()); sc["place"] = shortDate(g.date).toUpper();
        sc["detail"] = g.bothLocal ? QString("FINAL") : res == "W" ? QString("WIN") : res == "T" ? QString("TIE") : QString("LOSS");
        const QString venue = venueWords(g); sc["summary"] = sport + (venue.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ") + cap1(venue) + " game");
        sc["counter"] = QString("%1 / %2").arg(k).arg(total); scenes.append(sc);
        sources.append(source(sc.value("title").toString(), QString(), QString(), "MaxPreps")); lines << sc.value("title").toString();
    }
    for (const Game& g : upcoming) {
        ++k; const QString school = g.o.value("school").toString(), opp = g.o.value("opponent").toString(), sport = g.o.value("sport").toString(); const int home = g.o.value("home").toInt(-1);
        const int ahead = g.date.toJulianDay() - today.toJulianDay(); const bool evening = g.time.hour() >= 17;
        const QString when = ahead <= 0 ? (g.startMs < nowMs ? QString("Right now") : evening ? QString("Tonight") : QString("Today")) : ahead == 1 ? (evening ? QString("Tomorrow night") : QString("Tomorrow")) : "On " + dayName(g.date);
        const bool tbd = g.o.value("tbd").toBool();
        QString nar = tbd ? QString("%1, time to be announced: ").arg(when) : QString("%1 at %2: ").arg(when, spokenClock(g.time));
        if (g.o.value("neutral").toBool()) nar += QString("%1 faces %2 in %3.").arg(school, opp, spokenSport(sport));
        else if (home == 1) nar += QString("%1 hosts %2 in %3.").arg(school, opp, spokenSport(sport));
        else if (home == 0) nar += QString("%1 travels to %2 in %3.").arg(school, opp, spokenSport(sport));
        else nar += QString("%1 plays %2 in %3.").arg(school, opp, spokenSport(sport));
        QJsonObject sc = mkScene("story", speech(nar));
        const QString whenTag = (ahead <= 0 ? (evening ? QString("TONIGHT") : QString("TODAY")) : ahead == 1 ? QString("TOMORROW") : dayName(g.date).toUpper());
        sc["title"] = home == 1 || g.o.value("neutral").toBool() ? QString("%1 vs. %2").arg(school, opp) : QString("%1 at %2").arg(school, opp);
        sc["category"] = QString("%1 \xC2\xB7 %2").arg(whenTag, sport.toUpper()); sc["place"] = shortDate(g.date).toUpper();
        sc["detail"] = tbd ? QString("TIME TBA") : clockText(g.time);
        const QString kind = g.o.value("kind").toString(); const QString venue = venueWords(g);
        sc["summary"] = sport + (kind.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ") + cap1(kind) + " game") + (venue.isEmpty() ? QString() : QString::fromUtf8(" \xC2\xB7 ") + cap1(venue));
        sc["counter"] = QString("%1 / %2").arg(k).arg(total); scenes.append(sc);
        sources.append(source(sc.value("title").toString(), QString(), QString(), "MaxPreps")); lines << QString("%1: %2").arg(whenTag.toLower(), sc.value("title").toString());
    }
    QJsonObject outro = mkScene("outro", speech(site.isEmpty() ? QString("That's your %1 sports update.").arg(areaName.isEmpty() ? QString("local") : areaName) : QString("That's your %1 sports update. For more scores and schedules, visit %2.").arg(areaName.isEmpty() ? QString("local") : areaName, headline::spokenSite(site, siteName))));
    outro["eyebrow"] = "MORE SCORES & SCHEDULES"; outro["big"] = site.isEmpty() ? siteName : site; outro["sub"] = "Scores and schedules from MaxPreps"; scenes.append(outro);
    QJsonObject local; local["source"] = "sports"; local["results"] = results.size(); local["upcoming"] = upcoming.size();
    return finishScript(scenes, scenes[1].toObject().value("title").toString(), lines.mid(0, 3), "Local Sports", o.value("energy").toInt(8), local, sources);
}

// ---------------------------------------------------------------------------------------------
// Realty (Showcase IDX listing JSON)
// ---------------------------------------------------------------------------------------------
QJsonArray parseListings(const QString& html) {
    QJsonArray out; QSet<QString> seen;
    const QString key = "\"orig_mls_number\"";
    int from = 0;
    while (true) {
        const int at = html.indexOf(key, from); if (at < 0) break;
        from = at + key.size();
        // the listing object is the nearest unclosed '{' before its MLS number
        int depth = 0, i = at;
        while (i > 0) { const QChar c = html[i]; if (c == '}') ++depth; else if (c == '{') { if (depth == 0) break; --depth; } --i; }
        // ...and its matching '}' (string-aware: descriptions hold braces and quotes)
        int d = 0, j = i; bool inStr = false, esc = false;
        for (; j < html.size(); ++j) {
            const QChar c = html[j];
            if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; }
            else if (c == '"') inStr = true; else if (c == '{') ++d; else if (c == '}') { if (--d == 0) break; }
        }
        if (j >= html.size()) continue;
        const QJsonObject r = QJsonDocument::fromJson(html.mid(i, j - i + 1).toUtf8()).object();
        const QString mls = r.value("orig_mls_number").toString(); if (mls.isEmpty() || seen.contains(mls)) continue; seen.insert(mls);
        QJsonArray images; for (const QJsonValue& im : r.value("images").toArray()) { QString u = im.toString(); const int q = u.indexOf("?d="); if (q > 0) u = u.left(q); if (u.startsWith("http")) images.append(u); }
        if (images.isEmpty() && r.value("image").toString().startsWith("http")) images.append(r.value("image").toString());
        QJsonObject l; l["mls"] = mls; l["address"] = r.value("address").toString().simplified(); l["city"] = r.value("city").toString(); l["state"] = r.value("state").toString(); l["zip"] = r.value("zip").toString();
        l["county"] = r.value("county").toString(); l["subdivision"] = r.value("subdivision").toString(); l["price"] = r.value("price").toDouble(); l["status"] = r.value("status").toString();
        l["beds"] = r.value("bedrooms").toDouble(); l["baths"] = r.value("bathrooms").toDouble() + (r.value("halfbaths").isDouble() ? 0.5 * r.value("halfbaths").toDouble() : 0.0);
        l["sqft"] = r.value("sqft").toDouble(); l["acres"] = r.value("acres").toDouble(); l["yearBuilt"] = r.value("year_built").toDouble(); l["description"] = r.value("description").toString();
        l["images"] = images; const QDateTime fs = QDateTime::fromString(r.value("first_seen_dts").toString(), Qt::ISODate); l["firstSeenMs"] = fs.isValid() ? double(fs.toMSecsSinceEpoch()) : 0.0;
        l["agent"] = r.value("listing_agent").toString(); l["agentPhone"] = r.value("listing_agent_phone").toString(r.value("idx_contact_phone").toString());
        l["office"] = r.value("listing_office").toString(); l["officePhone"] = r.value("listing_office_phone").toString(); l["propertyType"] = r.value("property_type").toString();
        out.append(l);
    }
    return out;
}

QJsonObject pickListing(const QJsonArray& listings, const QJsonObject& state, qint64 nowMs, const QJsonObject& o) {
    const int minPhotos = o.value("minPhotos").toInt(4);
    QStringList statuses; for (const QJsonValue& s : o.value("statuses").toArray()) statuses << s.toString().toLower(); if (statuses.isEmpty()) statuses << "active";
    QJsonObject featured = state.value("featured").toObject(), seen = state.value("seen").toObject();
    QList<QJsonObject> eligible; QJsonArray newIds; QSet<QString> present;
    for (const QJsonValue& v : listings) {
        const QJsonObject l = v.toObject(); const QString mls = l.value("mls").toString(); present.insert(mls);
        const bool ok = statuses.contains(l.value("status").toString().toLower()) && l.value("images").toArray().size() >= minPhotos && l.value("price").toDouble() > 0 && !l.value("address").toString().isEmpty();
        if (!ok) continue;
        if (!seen.contains(mls)) { newIds.append(mls); seen[mls] = double(nowMs); }
        eligible << l;
    }
    for (const QString& k : seen.keys()) if (!present.contains(k) && nowMs - qint64(seen.value(k).toDouble()) > 60LL * 86400000LL) { seen.remove(k); featured.remove(k); }   // gone for two months: forget it
    QJsonObject ns = state; ns["seen"] = seen; ns["featured"] = featured; ns["lastCheckMs"] = double(nowMs);
    QJsonObject res; res["eligible"] = eligible.size(); res["newIds"] = newIds; res["listing"] = QJsonValue(QJsonValue::Null);
    if (!eligible.isEmpty()) {
        const QString last = state.value("lastPick").toString();
        // never featured first (newest listing first), then the one featured longest ago; the one just shown waits while another is eligible
        std::stable_sort(eligible.begin(), eligible.end(), [&](const QJsonObject& a, const QJsonObject& b) {
            const QString ma = a.value("mls").toString(), mb = b.value("mls").toString();
            const bool fa = featured.contains(ma), fb = featured.contains(mb);
            if (fa != fb) return !fa;
            if (!fa) return a.value("firstSeenMs").toDouble() > b.value("firstSeenMs").toDouble();
            const double ta = featured.value(ma).toDouble(), tb = featured.value(mb).toDouble(); if (ta != tb) return ta < tb;
            return a.value("firstSeenMs").toDouble() > b.value("firstSeenMs").toDouble();
        });
        QJsonObject pick = eligible.first();
        if (eligible.size() > 1 && pick.value("mls").toString() == last) pick = eligible.at(1);
        res["listing"] = pick;
    }
    res["state"] = ns;
    return res;
}

QJsonObject markFeatured(const QJsonObject& state, const QString& mls, qint64 nowMs) {
    QJsonObject s = state, f = s.value("featured").toObject(); f[mls] = double(nowMs); s["featured"] = f; s["lastPick"] = mls; s["lastPickMs"] = double(nowMs);
    return s;
}

namespace {
struct Hint { RX rx; const char* label; double pos; };
}
/// Where in a typical MLS photo order (front exterior first, living areas, kitchen, bedrooms, baths, extras, yard last) a sentence's subject sits.
static const QList<Hint>& photoHints() {
    static const QList<Hint> h = {
        {RX("\\b(curb appeal|exterior|front porch|facade|brick|vinyl|siding|welcome)\\b", CI), "Curb Appeal", 0.04},
        {RX("\\b(living room|family room|great room|foyer|entry|fireplace|open floor)\\b", CI), "Living Space", 0.16},
        {RX("\\b(kitchen|dining|breakfast|cabinet|countertop|granite|appliance|pantry)", CI), "Kitchen & Dining", 0.30},
        {RX("\\b(bedrooms?|primary|master|suite|closets?)\\b", CI), "Bedrooms", 0.52},
        {RX("\\b(baths?|bathrooms?|showers?|tub|vanity)\\b", CI), "Bathrooms", 0.66},
        {RX("\\b(laundry|basement|garage|utility|storage|workshop|attic|crawl)", CI), "Extras", 0.78},
        {RX("\\b(deck|patio|backyard|back yard|yard|fenced|outdoor|garden|pool|porch|lot|acres?|shed)\\b", CI), "Outdoor Living", 0.92},
        {RX("\\b(roof|hvac|heat pump|furnace|windows?|updated|renovated|remodeled|new paint|flooring)\\b", CI), "Updates", -1.0},
    };
    return h;
}
static const Hint* hintFor(const QString& text) {
    const Hint* best = nullptr; int bestAt = 1 << 30;
    for (const Hint& h : photoHints()) { const auto m = h.rx.match(text); if (m.hasMatch() && m.capturedStart() < bestAt) { bestAt = m.capturedStart(); best = &h; } }
    return best;
}
/// Sentences worth reading: complete, about the home, no calls to action, boilerplate, or steering language.
static QStringList descriptionSentences(const QString& desc) {
    // calls to action and legal boilerplate; "approximately 1,548 square feet" is information, "measurements are approximate" is a disclaimer
    static const RX junk("(\\bshowings?\\b|\\bappointment|\\bschedule\\b|\\b(?:call|text)\\b\\s+(?:me|us|now|today|the|your|\\d)|e-?mail|\\bcontact\\b|\\bphone\\b|\\.com|\\bmls\\b|listing agent|buyers? (?:to|should|must)|verif(?:y|ied)|(?:measurements|square footage|sizes|sq\\.? ?ft\\.?) (?:are |is )?(?:approximate|not guaranteed)|approximate (?:measurements|sizes)|not guaranteed|deemed reliable|equal housing|information is|(?:lot size|sq\\.? ?ft\\.?|square (?:feet|footage)|acreage|taxes|dimensions)\\b[^.]{0,40}\\b(?:estimated|approx|to be determined|tbd|not verified)|\\bsellers?\\b|\\bestate\\b|(?:submit|send|review|present(?:ed)?|accepting|multiple|all)\\s+offers?|offers?\\s+(?:will|are|must|due|reviewed|deadline)|\\bbids?\\b|open house|\\bvisit\\b|\\bclick\\b|\\blink\\b)", CI);
    static const RX steer("(famil(?:y|ies)[- ]friendly|(?:perfect|ideal|great) for (?:a )?(?:young |growing |small |first[- ]time )?(?:famil|couple|retire|student|single)|adults? only|no children|christian|church|safe (?:area|neighborhood)|exclusive (?:area|neighborhood))", CI);
    static const RX shouting("^[^a-z]{12,}$");
    QStringList out;
    for (const QString& s0 : headline::sentences(headline::plainText(desc).replace(QChar(0xFFFD), ", "))) {
        const QString s = s0.simplified();
        if (wordsIn(s) < 3 || s.size() > 330 || junk.match(s).hasMatch() || steer.match(s).hasMatch() || shouting.match(s).hasMatch()) continue;
        out << s;
    }
    return out;
}
static QString bathsSpoken(double b) { return b == std::floor(b) ? spokenNumber(int(b)) : (b < 1 ? QString("a half") : spokenNumber(int(b)) + " and a half"); }

QJsonObject realtyScript(const QJsonObject& l, const QDateTime& now, const QJsonObject& o) {
    Q_UNUSED(now);
    const QString siteName = o.value("siteName").toString("Haven More Real Estate"), site = o.value("site").toString("havenmore.com");
    const int minPhotos = o.value("minPhotos").toInt(4), maxPhotos = qBound(4, o.value("maxPhotos").toInt(8), 8);
    QStringList images; for (const QJsonValue& v : l.value("images").toArray()) images << v.toString();
    if (images.size() < minPhotos) return skipped("the listing has too few photos");
    const QString address = l.value("address").toString(), city = l.value("city").toString(), state = l.value("state").toString(); const qint64 price = qint64(l.value("price").toDouble());
    const int beds = int(l.value("beds").toDouble()); const double baths = l.value("baths").toDouble(); const qint64 sqft = qint64(l.value("sqft").toDouble()); const double acres = l.value("acres").toDouble(); const int built = int(l.value("yearBuilt").toDouble());
    const QString priceText = "$" + us().toString(price), cityState = city + (state.isEmpty() ? QString() : ", " + state);
    QStringList facts; if (beds > 0) facts << QString("%1 BD").arg(beds); if (baths > 0) facts << QString("%1 BA").arg(baths == std::floor(baths) ? QString::number(int(baths)) : QString::number(baths, 'f', 1));
    if (sqft > 0) facts << us().toString(sqft) + " SQ FT";
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString detail = priceText + (facts.isEmpty() ? QString() : dot + facts.mid(0, 2).join(dot));
    // 1) the facts scene: address, price, size
    QString f = QString("Today's featured home from %1. %2 in %3. Listed at %4 dollars.").arg(siteName, spokenAddress(address), city, spokenNumber(price));
    QStringList spec;
    if (beds > 0) spec << spokenNumber(beds) + (beds == 1 ? " bedroom" : " bedrooms");
    if (baths > 0) spec << bathsSpoken(baths) + (baths == 1 ? " bath" : " baths");
    if (sqft > 0) spec << "about " + spokenNumber(sqft) + " square feet";
    if (!spec.isEmpty()) f += " " + cap1(joinList(spec)) + (acres >= 0.15 ? QString(", on %1 acres").arg(acres >= 1 ? QString::number(acres, 'f', 1) : QString::number(acres, 'f', 2)) : QString()) + ".";
    QStringList summaryBits; if (acres >= 0.15) summaryBits << QString("%1 acres").arg(acres >= 1 ? QString::number(acres, 'f', 1) : QString::number(acres, 'f', 2)); if (built > 1800) summaryBits << QString("Built %1").arg(built);
    if (!l.value("subdivision").toString().isEmpty()) summaryBits << l.value("subdivision").toString();
    // 2) the description, in the order the listing tells it, one group per photo; short descriptions get plain facts as filler
    QStringList sents = descriptionSentences(l.value("description").toString());
    if (sents.size() < 3) {
        if (built > 1800) sents << QString("The home was built in %1.").arg(built);
        if (acres >= 0.15) sents << QString("It sits on about %1 acres in %2.").arg(acres >= 1 ? QString::number(acres, 'f', 1) : QString::number(acres, 'f', 2), l.value("county").toString().isEmpty() ? city : l.value("county").toString() + " County");
        if (!l.value("subdivision").toString().isEmpty()) sents << QString("It is located in the %1 area.").arg(l.value("subdivision").toString());
    }
    if (sents.size() < 3 && !city.isEmpty()) sents << QString("This home is located in %1%2.").arg(city, state.isEmpty() ? QString() : ", " + state);
    // the read stays about a minute: earliest sentences first, ~200 words at most
    { QStringList kept; int words = 0; for (const QString& s : sents) { const int w = wordsIn(s); if (!kept.isEmpty() && words + w > 200) break; kept << s; words += w; } sents = kept; }
    const int maxGroups = maxPhotos - 1;                                                   // the facts scene is photo one
    const int G0 = qBound(1, qMax(3, int(std::ceil(sents.size() / 2.0))), qMin(maxGroups, int(sents.size())));   // 3..7 groups, never more than there are sentences
    if (sents.isEmpty()) return skipped("the listing has nothing to say");
    // contiguous groups of about equal length, every group at least one sentence
    QList<QStringList> groups; QStringList cur; int curWords = 0, descWords = 0; for (const QString& s : sents) descWords += wordsIn(s);
    const double target = double(descWords) / G0;
    for (int i = 0; i < sents.size(); ++i) {
        cur << sents[i]; curWords += wordsIn(sents[i]);
        const int sentsLeft = int(sents.size()) - i - 1, groupsLeft = G0 - int(groups.size()) - 1;
        if (groupsLeft > 0 && (sentsLeft <= groupsLeft || (curWords >= target && sentsLeft >= groupsLeft))) { groups << cur; cur.clear(); curWords = 0; }
    }
    if (!cur.isEmpty()) groups << cur;
    const int G = groups.size(), n = images.size();
    // 3) one photo per group: the sentence's subject picks its place in the photo order, the rest are spread evenly; each photo once
    QSet<int> used{0}; QList<int> idx;
    for (int g = 0; g < G; ++g) {
        const Hint* h = hintFor(groups[g].join(' '));
        double t = h && h->pos >= 0 && (g > 0 || h->pos <= 0.3) ? h->pos : double(g + 1) / double(G + 1);
        int want = qBound(1, int(std::lround(t * (n - 1))), n - 1);
        for (int d = 0; d < n; ++d) { const int a = want + d, b = want - d; if (a < n && a >= 1 && !used.contains(a)) { want = a; break; } if (b >= 1 && b < n && !used.contains(b)) { want = b; break; } }
        used.insert(want); idx << want;
    }
    QJsonArray scenes, sources; int k = 1; const int total = G + 1; QSet<QString> usedLabels;
    QJsonObject s1 = mkScene("story", speech(f));
    s1["title"] = address; s1["category"] = "FOR SALE"; s1["place"] = cityState.toUpper(); s1["detail"] = detail; s1["summary"] = summaryBits.mid(0, 3).join(dot);
    s1["image"] = images[0]; s1["counter"] = QString("1 / %1").arg(total); scenes.append(s1);
    for (int g = 0; g < G; ++g) {
        ++k; const QString text = groups[g].join(' '); const Hint* h = hintFor(text);
        QJsonObject sc = mkScene("story", speech(text));
        QString label = g == 0 ? QString("About This Home") : h ? QString(h->label) : QString("Home Highlights");
        if (usedLabels.contains(label)) label = usedLabels.contains("Home Highlights") ? QString("More Features") : QString("Home Highlights");
        usedLabels.insert(label);
        sc["title"] = label; sc["category"] = "FOR SALE"; sc["place"] = address.toUpper(); sc["detail"] = detail;
        sc["summary"] = trimScreen(groups[g].first(), 150); sc["image"] = images[idx[g]]; sc["counter"] = QString("%1 / %2").arg(k).arg(total);
        scenes.append(sc);
    }
    // 4) closing: who to call, whose listing it is
    // the recipe's contactPhone (one line for every home) wins over the listing agent's own number
    const bool station = !o.value("contactPhone").toString().isEmpty();
    const QString agent = station ? QString() : l.value("agent").toString(), office = l.value("office").toString();
    const QString phone = station ? o.value("contactPhone").toString() : l.value("agentPhone").toString(l.value("officePhone").toString());
    QString nar;
    if (!agent.isEmpty() && !phone.isEmpty()) nar = QString("To see this home, contact %1 at %2, %3. ").arg(agent, siteName, spokenPhone(phone));
    else if (!phone.isEmpty()) nar = QString("To see this home, call %1 at %2. ").arg(siteName, spokenPhone(phone));
    else nar = QString("To see this home, contact %1. ").arg(siteName);
    nar += QString("Visit %1 for more homes.").arg(headline::spokenSite(site, siteName));
    if (!office.isEmpty()) nar += QString(" Listing courtesy of %1.").arg(office.contains(QRegularExpression("^KW\\b")) ? QString(office).replace(QRegularExpression("^KW\\b"), "Keller Williams") : office);
    QJsonObject outro = mkScene("outro", speech(nar));
    outro["eyebrow"] = "SEE IT IN PERSON"; outro["big"] = site;
    QStringList sub; if (!agent.isEmpty()) sub << agent; if (!phone.isEmpty()) { QString d; for (const QChar c : phone) if (c.isDigit()) d += c; if (d.size() == 10) sub << QString("%1-%2-%3").arg(d.left(3), d.mid(3, 3), d.mid(6)); }
    if (!office.isEmpty()) sub << "Listing courtesy of " + office; sub << "MLS " + l.value("mls").toString();
    outro["sub"] = sub.join(dot); outro["image"] = images[0];
    scenes.append(outro);
    sources.append(source(address, QString(), images[0], site));
    QJsonObject local; local["source"] = "realty"; local["mls"] = l.value("mls"); local["address"] = address; local["city"] = city; local["price"] = double(price); local["photos"] = G + 1; local["images"] = n;
    return finishScript(scenes, QString("%1 - %2").arg(address, priceText), {priceText + (facts.isEmpty() ? QString() : dot + facts.mid(0, 3).join(dot)), cityState}, "Featured Home", o.value("energy").toInt(4), local, sources);
}

} // namespace localspots
