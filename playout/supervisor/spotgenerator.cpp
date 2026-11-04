// SpotGenerator — auto-generated spots: recipes, deterministic script writer (+ optional LLM),
// two-plane card render (Pillow furniture over an ffmpeg-moved bed), TTS, ffmpeg composition,
// ingest, perishable expiry, recurring jobs with GEN markers, and the news scatter.
#include "spotgenerator.h"
#include "aimodels.h"
#include "areamarket.h"
#include "headlinenews.h"
#include "localspots.h"
#include "marketspots.h"
#include "weatherspots.h"
#include "../schedule/schedulestore.h"
#include "../../tools/ppcommon/toolswitch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QLocale>
#include <QUuid>
#include <QUrl>
#include <QSet>
#include <QDebug>
#include <algorithm>
#include <memory>
#include <cmath>

using namespace sched;

static const char* kAutogenCategory = "autogen rotation";
/// The library category a recipe's spots carry. The hour recipes' generic "Autogen" blocks play the NEWEST spot of "autogen rotation", so a
/// recipe with its own cadence (cinema every 4 hours, a different home each hour) names another category and airs only through its GEN marker.
static QString mediaCategoryOf(const QJsonObject& recipe) { return recipe.value("mediaCategory").toString(kAutogenCategory); }
static const char* kScatterTag = "newsScatter";                           // media tag on scatter stories; scatter markers filter on it
static const int kIngestWaitMs = 5 * 60 * 1000;                           // how long a generation waits for the shared ingest queue to probe its MP4
static const qint64 kMaxPhotoBytes = 12LL * 1024 * 1024, kMaxApiBytes = 4LL * 1024 * 1024;   // response caps: story photo / LLM reply (feeds and forecast JSON use httpGet's 6 MB default)
/// Tag that ties a library spot and its GEN marker to one job.
static QString genTag(const QString& jobId) { return "gen:" + jobId; }
static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QString shortId() { return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); }

// ---------------------------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------------------------
static QString unescapeEntities(QString s) {
    static const QHash<QString, QString> named = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "}, {"rsquo", "\u2019"}, {"lsquo", "\u2018"}, {"rdquo", "\u201d"}, {"ldquo", "\u201c"}, {"mdash", "\u2014"}, {"ndash", "\u2013"}, {"hellip", "\u2026"}, {"deg", "\u00b0"}, {"copy", "\u00a9"}};
    static const QRegularExpression rx("&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);");
    QString out; int last = 0;
    auto it = rx.globalMatch(s);
    while (it.hasNext()) {
        const auto m = it.next(); out += s.mid(last, m.capturedStart() - last); last = m.capturedEnd();
        const QString e = m.captured(1);
        if (e.startsWith("#x", Qt::CaseInsensitive)) out += QString(QChar(e.mid(2).toUInt(nullptr, 16)));
        else if (e.startsWith('#')) out += QString(QChar(e.mid(1).toUInt()));
        else if (named.contains(e.toLower())) out += named.value(e.toLower());
        else out += m.captured(0);
    }
    out += s.mid(last);
    return out;
}
static QString stripHtml(QString s) {
    static const QRegularExpression cdata("<!\\[CDATA\\[(.*?)\\]\\]>", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression scripts("<(script|style)[^>]*>.*?</\\1>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags("<[^>]+>");
    static const QRegularExpression ws("\\s+");
    s.replace(cdata, "\\1"); s.replace(scripts, " "); s.replace(tags, " ");
    return unescapeEntities(s).replace(ws, " ").trimmed();
}
static QString stripEmoji(const QString& s) {
    QString out; out.reserve(s.size());
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c.isHighSurrogate()) { ++i; continue; }               // every non-BMP glyph (emoji) goes
        if (c.isLowSurrogate()) continue;
        const ushort u = c.unicode();
        if ((u >= 0x2600 && u <= 0x27BF) || u == 0xFE0F || u == 0x200D || u == 0x2B50 || u == 0x2B55) continue;
        out += c;
    }
    return out;
}
// Abbreviation-aware ("Oct. 23", "J. Smith", "3 p.m. Saturday" stay inside their sentence): every recipe reads first sentences.
static QStringList sentences(const QString& s) { return headline::sentences(s); }
static int wordCount(const QString& s) { return s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).size(); }
static QString capWords(const QString& s, int cap) {
    if (cap <= 0 || wordCount(s) <= cap) return s;
    QStringList out; int n = 0;
    for (const QString& sent : sentences(s)) { const int w = wordCount(sent); if (!out.isEmpty() && n + w > cap) break; out << sent; n += w; }
    if (out.isEmpty()) return s.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).mid(0, cap).join(' ');
    return out.join(' ');
}
/// Spoken-copy hygiene (newsroom._scrub_narration): no URLs, markdown, bracketed directions, "read more" tails, emoji, symbols.
static QString scrubNarration(QString s) {
    static const QRegularExpression fences("```.*?```", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression urls("(https?://\\S+|www\\.\\S+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression labels("^\\s*(anchor|narrator|host|voice(over)?)\\s*:\\s*", QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    static const QRegularExpression brackets("\\[[^\\]]{0,80}\\]|\\((?:pause|music|sfx|beat|photo|image|video|credit)[^)]*\\)", QRegularExpression::CaseInsensitiveOption);
    // Feed footers ("The post X appeared first on Y.") end the item, so they go to the end of the text, final period included;
    // call-to-action phrases go through the end of their own sentence. (A trailing \b after the footer alternation never matched a text ending in ".")
    static const QRegularExpression footer("\\b(?:the post\\b.*?\\bappeared first on|appeared first on)\\b.*$", QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression tails("\\b(?:read more|continue reading|click here|subscribe(?: now)?|sign up for)\\b[^.!?]*[.!?]?", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ws("\\s+");
    s = stripHtml(s);
    s.replace(fences, " "); s.replace(urls, " "); s.replace(labels, ""); s.replace(brackets, " "); s.replace(footer, " "); s.replace(tails, " ");
    s = stripEmoji(s);
    s.replace('*', ' ').replace('_', ' ').replace('#', " number ").replace('%', " percent").replace('&', " and ").replace(QChar(0x00B0), " degrees ").replace('|', ' ');
    s.replace(QChar(0x2019), '\'').replace(QChar(0x2018), '\'').replace(QChar(0x201C), '"').replace(QChar(0x201D), '"').replace(QChar(0x2014), ", ").replace(QChar(0x2013), "-").replace(QChar(0x2026), "...");
    return s.replace(ws, " ").trimmed();
}
static QString screenText(QString s, int maxLen) {
    s = stripEmoji(stripHtml(s)).replace(QChar(0x2019), '\'').replace(QChar(0x2018), '\'').replace(QChar(0x201C), '"').replace(QChar(0x201D), '"').replace(QChar(0x2014), " - ").replace(QChar(0x2013), "-").replace(QChar(0x2026), "...").simplified();
    static const QRegularExpression urls("(https?://\\S+|www\\.\\S+)", QRegularExpression::CaseInsensitiveOption); s.replace(urls, "");
    s = s.simplified();
    if (s.size() > maxLen) { s = s.left(maxLen); const int sp = s.lastIndexOf(' '); if (sp > maxLen / 2) s = s.left(sp); s += "..."; }
    return s;
}
static QString firstSentence(const QString& s, int maxWords = 40) { const QStringList ss = sentences(s); return ss.isEmpty() ? QString() : capWords(ss.first(), maxWords); }
static QString wmoText(int code, bool night = false) {
    switch (code) {
    case 0: return night ? "clear skies" : "sunny"; case 1: return night ? "mostly clear" : "mostly sunny"; case 2: return "partly cloudy"; case 3: return "overcast";
    case 45: case 48: return "foggy"; case 51: case 53: case 55: return "drizzle"; case 56: case 57: return "freezing drizzle";
    case 61: return "light rain"; case 63: return "rain"; case 65: return "heavy rain"; case 66: case 67: return "freezing rain";
    case 71: return "light snow"; case 73: return "snow"; case 75: return "heavy snow"; case 77: return "snow grains";
    case 80: return "scattered showers"; case 81: return "showers"; case 82: return "heavy showers"; case 85: case 86: return "snow showers";
    case 95: return "thunderstorms"; case 96: case 99: return "thunderstorms with hail"; default: return "mixed conditions";
    }
}
static bool wmoSnow(int c) { return (c >= 71 && c <= 77) || c == 85 || c == 86; }
static QString capFirst(QString s) { if (!s.isEmpty()) s[0] = s[0].toUpper(); return s; }
static QString degS(double v) { return QString::number(qRound(v)) + " degrees"; }
static QString degSym(double v) { return QString::number(qRound(v)) + QChar(0x00B0); }
static QString moonPhase(double* illum) {
    const double age = std::fmod((nowMs() / 1000.0 - 947182440.0) / 86400.0, 29.53058867);     // days since the 2000-01-06 18:14 UTC new moon
    if (illum) *illum = 50.0 * (1.0 - std::cos(2 * 3.14159265358979323846 * age / 29.53058867));
    const double lim[] = {1.0, 6.4, 8.4, 13.4, 15.4, 21.1, 23.1, 28.5, 30.0};
    const char* names[] = {"a new moon", "a waxing crescent moon", "a first quarter moon", "a waxing gibbous moon", "a full moon", "a waning gibbous moon", "a last quarter moon", "a waning crescent moon", "a new moon"};
    for (int i = 0; i < 9; ++i) if (age <= lim[i]) return names[i];
    return "a new moon";
}
static QString clock12(const QString& iso) { const QDateTime t = QDateTime::fromString(iso, Qt::ISODate); return t.isValid() ? QLocale("en_US").toString(t.time(), "h:mm AP").toLower() : QString(); }

/// RSS 2.0 / Atom → material items [{title, text, link, image, pubMs, source}]. Regex on purpose: feeds are often not well-formed XML.
static QJsonArray parseFeed(const QByteArray& raw, const QString& source, int limit) {
    const QString x = QString::fromUtf8(raw);
    static const QRegularExpression itemRx("<(item|entry)[\\s>](.*?)</\\1>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression titleRx("<title[^>]*>(.*?)</title>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression descRx("<(description|summary|content:encoded|content)[^>]*>(.*?)</\\1>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression linkRx("<link>(.*?)</link>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression linkHrefRx("<link[^>]*href=[\"']([^\"']+)[\"']", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression dateRx("<(pubDate|published|updated|dc:date)[^>]*>(.*?)</\\1>", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression catRx("<category[^>]*?(?:term=[\"']([^\"']+)[\"'][^>]*)?(?:/>|>(.*?)</category>)", QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    // Media tags qualify as the story photo only when they are images (image extension, type="image/*", medium="image" or a
    // thumbnail): an audio/video enclosure must never become the "photo" (fetchPhoto would download the whole file to reject it).
    static const QRegularExpression mediaTagRx("<(enclosure|media:content|media:thumbnail)\\b([^>]*)>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrUrlRx("\\burl=[\"']([^\"']+)[\"']", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrTypeRx("\\btype=[\"']([^\"']*)[\"']", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression attrMediumRx("\\bmedium=[\"']([^\"']*)[\"']", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression imgExtRx("\\.(?:jpe?g|png|webp)(?:\\?.*)?$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression imgTagRx("<img[^>]*src=[\"']([^\"']+)[\"']", QRegularExpression::CaseInsensitiveOption);
    QJsonArray out;
    auto it = itemRx.globalMatch(x);
    while (it.hasNext() && out.size() < limit) {
        const QString item = it.next().captured(2);
        const QString title = stripHtml(titleRx.match(item).captured(1)).simplified();
        if (title.isEmpty()) continue;
        QString text; const auto dm = descRx.match(item); if (dm.hasMatch()) text = stripHtml(unescapeEntities(dm.captured(2)));
        QString link = stripHtml(linkRx.match(item).captured(1)).trimmed(); if (link.isEmpty()) link = linkHrefRx.match(item).captured(1);
        QString image;
        { auto mt = mediaTagRx.globalMatch(item);
          while (mt.hasNext() && image.isEmpty()) {
              const auto m = mt.next(); const QString attrs = m.captured(2); const QString u = attrUrlRx.match(attrs).captured(1); if (u.isEmpty()) continue;       // unescaped once, when stored below
              const QString type = attrTypeRx.match(attrs).captured(1).toLower(), medium = attrMediumRx.match(attrs).captured(1).toLower();
              if (type.startsWith("audio/") || type.startsWith("video/") || medium == "audio" || medium == "video") continue;
              if (type.startsWith("image/") || medium == "image" || m.captured(1).compare("media:thumbnail", Qt::CaseInsensitive) == 0 || imgExtRx.match(u).hasMatch()) image = u;
          } }
        if (image.isEmpty()) image = imgTagRx.match(unescapeEntities(item)).captured(1);
        qint64 pub = 0; const auto pm = dateRx.match(item);
        if (pm.hasMatch()) { const QString d = pm.captured(2).trimmed(); QDateTime dt = QDateTime::fromString(d, Qt::RFC2822Date); if (!dt.isValid()) dt = QDateTime::fromString(d, Qt::ISODate); if (dt.isValid()) pub = dt.toMSecsSinceEpoch(); }
        QJsonObject o; o["title"] = title; o["text"] = text.left(4000); o["link"] = link; o["image"] = unescapeEntities(image); o["pubMs"] = double(pub); o["source"] = source;
        { const auto cm = catRx.match(item); const QString c = stripHtml(cm.captured(2).isEmpty() ? cm.captured(1) : cm.captured(2)).simplified(); if (!c.isEmpty()) o["category"] = c; }   // RSS <category> / Atom term=
        out.append(o);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Recipes
// ---------------------------------------------------------------------------------------------
static QJsonObject mkRecipe(const char* key, const char* label, const char* voice, const char* subcat, const char* category, const char* kicker, const char* series, int ttlMin, int targetSec, const char* source, const QStringList& feeds, int energy, bool skipIfNone, const char* prompt, const char* filter = "") {
    QJsonObject r; r["key"] = key; r["label"] = label; r["voice"] = voice; r["subcategory"] = subcat; r["category"] = category; r["kicker"] = kicker; r["series"] = series;
    r["ttlMin"] = ttlMin; r["targetSec"] = targetSec; r["source"] = source; QJsonArray f; for (const QString& u : feeds) f.append(u); r["feeds"] = f; r["energy"] = energy; r["skipIfNone"] = skipIfNone; r["prompt"] = prompt;
    r["type"] = QString(category) == "weather" ? "weather" : "news"; if (*filter) r["filter"] = filter; r["builtin"] = true;
    return r;
}
static const QStringList kLocalNews = {"https://www.wsls.com/arc/outboundfeeds/rss/category/news/local/?outputType=xml", "https://www.getstarnews.com/feed/"};
static const QStringList kStarNews = {"https://www.getstarnews.com/feed/"};
static const QStringList kTech = {"https://feeds.arstechnica.com/arstechnica/index", "https://www.engadget.com/rss.xml", "https://www.gamespot.com/feeds/game-news/", "https://medicalxpress.com/rss-feed/"};
static QJsonArray builtinRecipes() {
    static QJsonArray a;
    if (!a.isEmpty()) return a;
    a.append(mkRecipe("weather", "Weather", "af_heart", "Weather", "weather", "WEATHER", "Weather", 200, 60, "weather", {}, 5, false, "Write a short local TV WEATHER update from the forecast data. Warm, clear, conversational anchor tone."));
    a.append(mkRecipe("weather_am", "Weather - AM", "af_bella", "Weather", "weather", "MORNING WEATHER", "Weather", 220, 75, "weather", {}, 6, false, "You are the bright MORNING meteorologist: how the overnight went, what right now feels like, and today hour by hour (commute, lunchtime, evening)."));
    a.append(mkRecipe("weather_pm", "Weather - PM", "af_nicole", "Weather", "weather", "EVENING WEATHER", "Weather", 220, 75, "weather", {}, 4, false, "You are the calm EVENING meteorologist: how today turned out, what tonight brings, and the setup for tomorrow morning."));
    a.append(mkRecipe("weather_week", "Weather - 7-Day", "af_heart", "Weather", "weather", "7-DAY FORECAST", "Weather", 225, 120, "weather", {}, 5, false, "Walk viewers through the FULL WEEK ahead, day by day: sky, high and low, rain chances; flag the best and roughest days."));
    {   // Headline News: the newest getstarnews.com posts, one scene per story over the post's own photo (headlinenews.h)
        QJsonObject h = mkRecipe("headlines", "Headline News", "am_michael", "News", "local_news", "HEADLINE NEWS", "Headline News", 420, 100, "rss", kStarNews, 7, true, "Write a tight top-of-the-hour headlines read from the items given. Crisp, authoritative, no editorializing.");
        h["render"] = "stories"; h["stories"] = 5; h["pageImages"] = true; h["site"] = "getstarnews.com"; h["siteName"] = "GetStarNews";
        a.append(h);
    }
    a.append(mkRecipe("story", "Story Spot", "am_michael", "News", "local_news", "STAR NEWS", "Stories", 1440, 60, "rss", kStarNews, 5, true, "Write a broadcast story spot from the material: warm, local, conversational anchor tone; keep names, places, dates and times."));
    a.append(mkRecipe("community", "Community Bulletin", "af_heart", "News", "local_news", "COMMUNITY", "Community Bulletin", 2880, 75, "rss", kStarNews, 5, true, "Write a friendly community bulletin from the listings. Upbeat local-station tone."));
    a.append(mkRecipe("events", "Events Board", "af_heart", "News", "local_news", "HAPPENING SOON", "Events", 1440, 75, "rss", kStarNews, 5, true, "From event announcements write a what's-coming-up spot: each event with its day, time and place, soonest first.", "\\b(event|festival|concert|fair|parade|fundraiser|tournament|market|show|meeting|game|race|class|workshop|open house)\\b"));
    a.append(mkRecipe("obituaries", "Obituaries", "am_michael", "Obits", "local_news", "IN MEMORIAM", "Community Remembrances", 2880, 75, "rss", kStarNews, 2, true, "Write a brief, respectful remembrance notice from the details given. Dignified, gentle, unhurried. Never invent facts.", "\\b(obituar|passed away|died|funeral|memorial service|celebration of life|survived by)"));
    a.append(mkRecipe("sports", "Sports", "am_adam", "Sports", "sports", "SPORTS", "Sports Desk", 720, 75, "rss", {"https://www.wsls.com/arc/outboundfeeds/rss/category/sports/?outputType=xml", "https://www.getstarnews.com/feed/"}, 8, true, "Write a punchy local sports update from the scores and notes. Energetic but concise.", "\\b(score|win|won|loss|lost|game|team|coach|season|playoff|tournament|football|basketball|baseball|soccer|race|speedway|volleyball|softball|track)\\b"));
    a.append(mkRecipe("roads", "Road Watch", "am_adam", "Road Reports", "local_news", "ROAD WATCH", "Road Watch", 90, 45, "rss", kLocalNews, 8, true, "Report ONLY road/traffic items: crashes, closures, detours, roadwork, delays. Always give the road and the timeframe.", "\\b(road|traffic|crash|wreck|closure|closed|detour|lane|interstate|highway|route|i-\\d+|us-\\d+|bridge|roadwork|construction|tractor-trailer|vdot)\\b"));
    {   // Local spots (localspots.h): scene renders fed by a structured page instead of RSS; the script is written from the page's own data
        auto scenes = [](const char* key, const char* label, const char* voice, const char* subcat, const char* category, const char* kicker, const char* series, int ttlMin, int targetSec, const char* src, int energy, bool skipIfNone) {
            QJsonObject r = mkRecipe(key, label, voice, subcat, category, kicker, series, ttlMin, targetSec, src, {}, energy, skipIfNone, "Scene render: the narration is written from the source page's own data (no model).");
            r["render"] = "scenes"; r["storyLayout"] = "box"; r["mediaCategory"] = "autogen local"; return r;      // own category: the generic Autogen blocks must not replay them
        };
        QJsonObject c = scenes("cinema", "Now Playing - Hollywood Cinema", "af_bella", "Entertainment", "local_news", "NOW PLAYING", "Hollywood Cinema", 300, 90, "cinema", 7, true);
        c["url"] = "https://hollywoodcinema.net/"; c["siteName"] = "Hollywood Cinema"; c["site"] = "hollywoodcinema.net"; c["place"] = "Martinsville"; c["maxMovies"] = 6; a.append(c);
        QJsonObject t = scenes("traffic", "Traffic Alerts", "am_adam", "Road Reports", "local_news", "TRAFFIC ALERT", "Traffic Alerts", 90, 45, "traffic", 8, false);   // never skips: no alerts is an "all clear" spot
        t["base"] = "https://511.vdot.virginia.gov/services/511/map/layers/map/"; t["layers"] = QJsonArray{"incidents", "construction", "weatherClosures"}; t["radiusMi"] = 45; t["maxScenes"] = 5;
        t["siteName"] = "VDOT 511"; t["site"] = "511virginia.org"; t["brand"] = "GetStarNews"; a.append(t);
        QJsonObject s = scenes("sports_local", "Local Sports Scoreboard", "am_michael", "Sports", "sports", "SPORTS", "Sports Desk", 300, 80, "sports", 8, true);
        s["recentDays"] = 4; s["upcomingHours"] = 48; s["maxResults"] = 5; s["maxUpcoming"] = 4; s["siteName"] = "GetStarNews"; s["site"] = "getstarnews.com"; a.append(s);
        // Market wrap: the S&P 500, four large caps, Bitcoin and Ethereum; made once after the US close (job: weekdays 16:20), aired every 3 hours until the next open
        QJsonObject m = scenes("markets_close", "Market Close", "am_adam", "Markets", "local_news", "MARKETS", "Market Close", 5400, 55, "markets", 6, true);
        m["siteName"] = "Yahoo Finance"; m["requireToday"] = true; m["brand"] = "GetStarNews"; a.append(m);
        // Weather, full: animated daily cards + sunset, moon phase, burn safety, lake levels. Same "autogen rotation" category as the other weather recipes.
        QJsonObject wx = scenes("weather_full", "Weather - Forecast & Almanac", "af_heart", "Weather", "weather", "WEATHER", "Weather", 200, 100, "weather_full", 5, false);
        wx["siteName"] = "GetStarNews"; wx["site"] = "getstarnews.com"; wx["brand"] = "GetStarNews"; wx["cardDays"] = 7; wx.remove("mediaCategory"); a.append(wx);
        QJsonObject h = scenes("realty", "Featured Home - Haven More", "af_sarah", "Real Estate", "local_news", "FEATURED HOME", "Haven More Featured Home", 90, 70, "realty", 4, true);
        h["url"] = "https://havenmore.com/"; h["siteName"] = "Haven More Real Estate"; h["site"] = "havenmore.com"; h["minPhotos"] = 4; h["maxPhotos"] = 8; h["statuses"] = QJsonArray{"Active"}; a.append(h);
    }
    a.append(mkRecipe("almanac", "Sky & Almanac", "af_nicole", "Weather", "weather", "ALMANAC", "Almanac", 840, 60, "weather", {}, 3, false, "Write an evening almanac: moon phase and illumination, sunrise and sunset, a one-line stargazing outlook. Calm, factual."));
    a.append(mkRecipe("history", "This Day in History", "bm_george", "History", "educational", "THIS DAY IN HISTORY", "Almanac", 1560, 120, "history", {}, 4, true, "You are a warm, well-versed historian. From the events logged for this calendar day pick a global, a national and a regional event; storyteller cadence; genuine facts only."));
    a.append(mkRecipe("economics", "Economics / Markets", "am_adam", "News", "local_news", "MARKETS", "Money Minute", 720, 60, "rss", {"https://www.cnbc.com/id/20910258/device/rss/rss.html", "https://feeds.content.dowjones.io/public/rss/mw_topstories"}, 6, true, "Write a concise markets and economics brief. Confident business-anchor tone; round figures naturally; say what it means."));
    a.append(mkRecipe("tech_review", "Tech in Review", "am_michael", "Tech", "educational", "TECH IN REVIEW", "Tech in Review", 1680, 180, "rss", kTech, 7, true, "You are a witty tech correspondent. Review the genuinely NEW developments of the last 72 hours in medical, AI, computers and video games for a non-technical audience; say why each matters to a regular person."));
    // maxAgeHours: feed items older than this are never read as current news (0 = no limit; undated items are kept). Events keep a week
    // because an announcement can predate the event by days; road reports go stale in hours.
    static const QHash<QString, int> maxAge = {{"headlines", 24}, {"story", 72}, {"community", 72}, {"events", 168}, {"obituaries", 72}, {"sports", 36}, {"roads", 3}, {"economics", 24}, {"tech_review", 72}};
    for (int i = 0; i < a.size(); ++i) { QJsonObject r = a[i].toObject(); r["maxAgeHours"] = maxAge.value(r.value("key").toString(), 0); a[i] = r; }
    return a;
}

// ---------------------------------------------------------------------------------------------
// Construction / configuration
// ---------------------------------------------------------------------------------------------
struct SpotGenerator::ToolCaps { toolswitch::CapsCache cache; };

SpotGenerator::SpotGenerator(ScheduleStore* store, const QString& dataDir, const QString& scriptsDir, QObject* parent)
    : QObject(parent), m_store(store), m_dataDir(QDir::fromNativeSeparators(dataDir)), m_scriptsDir(QDir::fromNativeSeparators(scriptsDir)), m_toolCaps(std::make_unique<ToolCaps>()) {
    m_outDir = QDir(m_dataDir).filePath("spots/out");
    for (const QString& d : {QString("spots/out"), QString("spots/work"), QString("spots/music"), QString("spots/backgrounds")}) QDir().mkpath(QDir(m_dataDir).filePath(d));
    m_notify.setSingleShot(true); connect(&m_notify, &QTimer::timeout, this, [this]() { emit event("spots", status()); });
    m_procTimer.setSingleShot(true);
    m_scatterSoon.setSingleShot(true); connect(&m_scatterSoon, &QTimer::timeout, this, [this]() { const QJsonObject c = scatterCfg(); if (c.value("enabled").toBool()) scatterApply(c.value("channel").toString(), c.value("perHour").toInt(3)); });
    m_enforceSoon.setSingleShot(true); connect(&m_enforceSoon, &QTimer::timeout, this, [this]() { enforceMarkersNow(); });
    connect(&m_tick, &QTimer::timeout, this, &SpotGenerator::tick);
    connect(&m_sweep, &QTimer::timeout, this, [this]() { expireSweep(); });
    seedJobs();
}
SpotGenerator::~SpotGenerator() { if (m_proc) { m_proc->kill(); } qDeleteAll(m_gens); }

void SpotGenerator::setOutputDir(const QString& dir) { m_outDir = QDir::fromNativeSeparators(dir); QDir().mkpath(m_outDir); }
void SpotGenerator::start() {
    m_tick.start(30000); m_sweep.start(10 * 60 * 1000);
    QTimer::singleShot(90000, this, [this]() { expireSweep(); });
    QTimer::singleShot(20000, this, [this]() { enforceMarkersNow(); });
    qInfo().noquote() << "[Spots] ready:" << jobs().size() << "job(s), output" << QDir::toNativeSeparators(m_outDir) << "python" << pythonPath() << "ffmpeg" << ffmpegPath();
    // engine switches (DEP-13): resolving each step once starts the async `version` probes "auto" needs, so the first spot sees them
    QStringList eng; for (const QString& s : {QString("card"), QString("tts"), QString("music"), QString("compose")}) { const QJsonObject e = engineFor(s); eng << s + "=" + e.value("mode").toString(); }
    qInfo().noquote() << "[Spots] engines:" << eng.join(' ');
}
void SpotGenerator::log(const QString& level, const QString& msg) {
    if (level == "warning" || level == "critical") qWarning().noquote() << "[Spots]" << msg; else qInfo().noquote() << "[Spots]" << msg;
    emit alert(level, msg);
}

QString SpotGenerator::pythonPath() const {
    const QString c = m_store->settingJson("spots.tts").value("python").toString();
    if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    for (const QString& cand : {QString("C:/Program Files/Python311/python.exe"), QString("C:/Program Files/Python312/python.exe"), QString("C:/Program Files/Python313/python.exe")}) if (QFileInfo::exists(cand)) return cand;
    const QString p = QStandardPaths::findExecutable("python"); return p.isEmpty() ? "python" : p;
}
QString SpotGenerator::ffmpegPath() const {
    const QString c = QDir::fromNativeSeparators(m_store->setting("spots.ffmpeg"));
    if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    // The FFmpeg 9 build staged next to the executables first (the same build the engine links), then PATH.
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (QFileInfo::exists(local)) return local;
    const QString p = QStandardPaths::findExecutable("ffmpeg"); return p.isEmpty() ? "ffmpeg" : p;
}
// ---------------------------------------------------------------------------------------------
// Engine switches (DEP-13, docs/plans/inhouse-deps.md §5.3): settings spots.engines {card, tts, music, compose, dir}.
// Default = the old programs (python scripts, ffmpeg.exe); "native" runs the in-house tool whenever it is found, "auto" only once
// its `version` lists the command. Same arguments either way; a native run that fails (exit 64 = unsupported) is run again with
// the old program, so turning a switch on can never cost a spot that the old path would have made.
// ---------------------------------------------------------------------------------------------
namespace {
struct StepDef { const char* step; const char* tool; const char* capSub; const char* argSub; const char* legacy; const char* script; };
const StepDef kSteps[] = {
    {"card", "PlayPlaySpotRender", "card", "card", "python", "spot_card.py"},
    {"tts", "PlayPlaySpeech", "tts", "tts", "python", "spot_tts.py"},
    {"music", "PlayPlaySpotRender", "music", "music", "python", "spot_music.py"},
    // the MediaTool takes ffmpeg's own command line (no sub-command); its graph runner (DEP-05) lists "graph" in `version`
    {"compose", "PlayPlayMediaTool", "graph", "", "ffmpeg", ""},
};
const StepDef* stepDef(const QString& step) { for (const StepDef& d : kSteps) if (step == QLatin1String(d.step)) return &d; return nullptr; }
}
QString SpotGenerator::toolPath(const QString& tool) const {
    return toolswitch::findTool(tool, {m_store->settingJson("spots.engines").value("dir").toString(), QCoreApplication::applicationDirPath()});
}
QJsonObject SpotGenerator::engineFor(const QString& step) const {
    QJsonObject o; const StepDef* d = stepDef(step); if (!d) return o;
    SpotGenerator* self = const_cast<SpotGenerator*>(this);          // the caps cache / kokoro probe start async work; nothing here blocks
    const toolswitch::Mode m = toolswitch::parseMode(m_store->settingJson("spots.engines").value(step).toString());
    const QString exe = m == toolswitch::Mode::Legacy ? QString() : toolPath(d->tool);
    static const toolswitch::Caps none;
    const toolswitch::Caps& caps = (m == toolswitch::Mode::Auto && !exe.isEmpty()) ? m_toolCaps->cache.get(self, exe, [self]() { self->notify(); }) : none;
    toolswitch::Choice c = toolswitch::choose(m, exe, caps, d->capSub, d->tool);
    // spot_tts.py's first engine is a `kokoro` pip package in this Python, which the tool cannot see: "auto" keeps the script then
    if (c.native && m == toolswitch::Mode::Auto && step == "tts" && m_pyKokoro != 0) {
        if (m_pyKokoro < 0) self->probeKokoroPackage();
        c = toolswitch::Choice(); c.reason = m_pyKokoro < 0 ? "checking the Supervisor's Python for the kokoro package" : "the kokoro package in the Supervisor's Python voices this (spot_tts.py)";
    }
    o["mode"] = toolswitch::modeName(m, d->legacy); o["tool"] = d->tool; o["uses"] = c.native ? QString("native") : QString(d->legacy); o["reason"] = c.reason;
    if (m != toolswitch::Mode::Legacy) { o["exe"] = exe; o["found"] = !exe.isEmpty(); }
    if (caps.probed) { QJsonArray cmds; for (const QString& s : caps.commands) cmds.append(s); o["commands"] = cmds; }
    const bool legacyOk = !*d->script ? (QFileInfo(ffmpegPath()).exists() || ffmpegPath() == "ffmpeg")
                                      : ((QFileInfo(pythonPath()).exists() || pythonPath() == "python") && QFileInfo(QDir(m_scriptsDir).filePath(d->script)).exists());
    o["ready"] = c.native || legacyOk;                                 // a native step still needs the old program only for its fallback
    return o;
}
void SpotGenerator::probeKokoroPackage() {
    if (m_pyKokoroBusy || m_pyKokoro >= 0) return;
    m_pyKokoroBusy = true;
    auto* p = new QProcess(this); auto done = std::make_shared<bool>(false);
    // find_spec only looks the package up (no import, no model load); -I as the script itself runs
    auto finish = [this, p, done](int v) { if (*done) return; *done = true; p->deleteLater(); m_pyKokoroBusy = false; m_pyKokoro = v; notify(); qInfo().noquote() << "[Spots] python kokoro package:" << (v ? "present (tts auto stays on spot_tts.py)" : "absent"); };
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [p, finish](int code, QProcess::ExitStatus st) {
        const QByteArray out = p->readAllStandardOutput().trimmed();
        finish(st == QProcess::NormalExit && code == 0 && out.endsWith('1') ? 1 : 0);     // no Python (or a broken one) = the script cannot voice Kokoro either
    });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(0); });
    QTimer::singleShot(20000, p, [p]() { if (p->state() != QProcess::NotRunning) p->kill(); });
    p->start(pythonPath(), {"-I", "-c", "import importlib.util as u; print(1 if u.find_spec('kokoro') else 0)"});
}
void SpotGenerator::runStep(Gen* g, const QString& step, const QString& legacyProgram, const QStringList& legacyArgs, int timeoutMs, std::function<bool(int, const QByteArray&)> okFn, ProcCb cb) {
    const StepDef* d = stepDef(step); const QString legacyName = d ? QString(d->legacy) : QString("python");
    const QJsonObject e = d ? engineFor(step) : QJsonObject();
    if (e.value("uses").toString() != "native") { g->engines[step] = legacyName; runProcess(g, legacyProgram, legacyArgs, timeoutMs, cb); return; }
    g->engines[step] = "native";
    const QString gid = g->id;
    runProcess(g, e.value("exe").toString(), toolswitch::nativeArgs(d->argSub, legacyArgs), timeoutMs,
               [this, gid, step, legacyName, legacyProgram, legacyArgs, timeoutMs, okFn, cb](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const bool ok = code == 0 && (!okFn || okFn(code, out));
        if (!toolswitch::shouldFallBack(code, ok)) { cb(code, out, err); return; }
        const QJsonObject r = toolswitch::lastJsonLine(out);
        const QString why = code == toolswitch::kFailedToStart ? QString("could not start") : code == toolswitch::kUnsupported ? r.value("error").toString("unsupported")
                          : QString("exit %1: %2").arg(code).arg(r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(160).simplified() : r.value("error").toString());
        g->engines[step] = legacyName + " (fallback: " + why.left(160) + ")";
        const qint64 now = nowMs();
        if (now - m_fallbackLogMs.value(step) > 3600000) {             // once per hour per step: a station on "auto" with an old tool must not flood the alerts
            m_fallbackLogMs[step] = now;
            const QString msg = QString("%1: the native %2 step fell back to %3 (%4)").arg(g->recipeKey, step, legacyName, why.left(200));
            if (toolswitch::expectedFallBack(code)) qInfo().noquote() << "[Spots]" << msg; else log("warning", msg);
        }
        runProcess(g, legacyProgram, legacyArgs, timeoutMs, cb);
    });
}
QString SpotGenerator::workRoot() const { return QDir(m_dataDir).filePath("spots/work"); }
QString SpotGenerator::musicDir() const { const QString c = QDir::fromNativeSeparators(m_store->setting("spots.musicDir")); return c.isEmpty() ? QDir(m_dataDir).filePath("spots/music") : c; }
QString SpotGenerator::backgroundsDir() const { const QString c = QDir::fromNativeSeparators(m_store->setting("spots.backgroundsDir")); return c.isEmpty() ? QDir(m_dataDir).filePath("spots/backgrounds") : c; }
QTimeZone SpotGenerator::tzFor(const QString& channel) const { if (channel.isEmpty()) return QTimeZone::systemTimeZone(); bool f = false; const ChannelProfile p = m_store->channel(channel, &f); return f ? p.tz() : QTimeZone::systemTimeZone(); }
QDateTime SpotGenerator::nowIn(const QString& channel) const { return QDateTime::currentDateTimeUtc().toTimeZone(tzFor(channel)); }
QString SpotGenerator::firstChannel() const { const auto cs = m_store->channels(); return cs.isEmpty() ? QString("main") : cs.first().id; }
QString SpotGenerator::defaultChannelFor(const QJsonObject& job) const { const QString c = job.value("channel").toString(); return c.isEmpty() ? firstChannel() : c; }

QJsonArray SpotGenerator::recipes() const { return recipesIn(QString()); }
/// The recipe list with the area market applied under the operator's overrides: built-in -> market patch -> settings spots.recipes.
/// An override's own "market" field is the recipe's market PICK (reported as marketOverride); it never replaces the market in effect.
QJsonArray SpotGenerator::recipesIn(const QString& market) const {
    const QJsonObject over = m_store->settingJson("spots.recipes"), mk = market.isEmpty() ? QJsonObject() : marketData(market);
    QJsonArray out;
    for (const QJsonValue& v : builtinRecipes()) {
        QJsonObject r = areamarket::localizeRecipe(v.toObject(), mk); const QJsonObject o = over.value(r.value("key").toString()).toObject();
        for (auto it = o.begin(); it != o.end(); ++it) { if (it.key() == "market") { r["marketOverride"] = it.value(); continue; } if (it.key() != "key" && it.key() != "builtin") r[it.key()] = it.value(); }
        if (!o.isEmpty()) r["overridden"] = true;
        out.append(r);
    }
    for (auto it = over.begin(); it != over.end(); ++it) {          // operator-defined extra recipes
        bool have = false; for (const QJsonValue& v : builtinRecipes()) if (v.toObject().value("key").toString() == it.key()) have = true;
        if (have) continue;
        QJsonObject r = it.value().toObject(); if (r.value("label").toString().isEmpty()) continue;
        const QJsonObject own = r; r["key"] = it.key(); r["builtin"] = false; if (!r.contains("source")) r["source"] = "rss"; if (!r.contains("ttlMin")) r["ttlMin"] = 720; if (!r.contains("targetSec")) r["targetSec"] = 60;
        if (!r.contains("type")) r["type"] = r.value("category").toString() == "weather" ? "weather" : "news"; if (!r.contains("voice")) r["voice"] = "af_heart"; if (!r.contains("subcategory")) r["subcategory"] = "News"; if (!r.contains("kicker")) r["kicker"] = r.value("label").toString().toUpper(); if (!r.contains("energy")) r["energy"] = 5; if (!r.contains("maxAgeHours")) r["maxAgeHours"] = r.value("source").toString() == "rss" ? 48 : 0;
        if (!mk.isEmpty()) { r = areamarket::localizeRecipe(r, mk); for (auto f = own.begin(); f != own.end(); ++f) if (f.key() != "market") r[f.key()] = f.value(); }      // a custom recipe's own fields still win over the market's
        if (own.contains("market")) r["marketOverride"] = own.value("market");
        out.append(r);
    }
    return out;
}
QJsonObject SpotGenerator::recipe(const QString& key, bool* found) const {
    for (const QJsonValue& v : recipes()) if (v.toObject().value("key").toString() == key) { if (found) *found = true; return v.toObject(); }
    if (found) *found = false; return QJsonObject();
}

QJsonObject SpotGenerator::recipeIn(const QString& key, const QString& market, bool* found) const {
    for (const QJsonValue& v : recipesIn(market)) if (v.toObject().value("key").toString() == key) { if (found) *found = true; return v.toObject(); }
    if (found) *found = false; return QJsonObject();
}

// ---------------------------------------------------------------------------------------------
// Area markets (areamarket.h): the three built-ins plus whatever the operator defines in settings spots.markets
// ---------------------------------------------------------------------------------------------
QJsonArray SpotGenerator::markets() const { return areamarket::allMarkets(m_store->settingJson("spots.markets")); }
QJsonObject SpotGenerator::marketData(const QString& key) const { return areamarket::find(key, m_store->settingJson("spots.markets")); }
QString SpotGenerator::stationMarket() const {
    const QString k = areamarket::normalizeKey(m_store->settingJson("spots.market").value("default").toString());
    return (k.isEmpty() || k == "none" || marketData(k).isEmpty()) ? QString() : k;
}
QString SpotGenerator::resolveMarket(const QString& requested, const QJsonObject& job, const QString& recipeKey) const {
    const QJsonObject over = m_store->settingJson("spots.recipes").value(recipeKey).toObject();
    return areamarket::resolve(requested, job.value("market").toString(), over.value("market").toString(), stationMarket(), m_store->settingJson("spots.markets"));
}
QJsonObject SpotGenerator::marketInfo() const {
    QJsonObject o; const QJsonObject custom = m_store->settingJson("spots.markets");
    o["default"] = areamarket::normalizeKey(m_store->settingJson("spots.market").value("default").toString()); o["effectiveDefault"] = stationMarket();
    QJsonArray list; for (const QJsonValue& v : markets()) list.append(areamarket::summary(v.toObject(), custom));
    o["markets"] = list; return o;
}
void SpotGenerator::bindMarket(Gen* g, const QString& requested, const QJsonObject& job) {
    g->market = resolveMarket(requested, job, g->recipeKey);
    if (g->market.isEmpty()) { g->marketJson = QJsonObject(); return; }
    g->marketJson = marketData(g->market);
    bool f = false; const QJsonObject r = recipeIn(g->recipeKey, g->market, &f); if (f) g->recipe = r;
}
QJsonObject SpotGenerator::weatherFor(const Gen* g) const {
    QJsonObject w = config().value("weather").toObject();
    if (g && !g->marketJson.isEmpty()) { const QJsonObject mw = areamarket::weatherOf(g->marketJson); for (auto it = mw.begin(); it != mw.end(); ++it) w[it.key()] = it.value(); }   // an explicit market wins; units stay the station's
    return w;
}

QJsonObject SpotGenerator::config() const {
    QJsonObject o;
    QJsonObject w = m_store->settingJson("spots.weather"); if (!w.contains("lat")) w["lat"] = 36.69; if (!w.contains("lon")) w["lon"] = -79.87; if (!w.contains("place")) w["place"] = "Martinsville"; if (!w.contains("units")) w["units"] = "us"; o["weather"] = w;
    QJsonObject l = m_store->settingJson("spots.llm"); l["hasKey"] = !AiModels::apiKey(m_store).isEmpty(); l["keySource"] = AiModels::keyStatus(m_store).value("keySource"); l.remove("apiKey"); if (!l.contains("baseUrl")) l["baseUrl"] = "https://openrouter.ai/api/v1"; l["model"] = AiModels::model(m_store, l.value("model").toString()); if (!l.contains("enabled")) l["enabled"] = false; o["llm"] = l;
    QJsonObject sc = scatterCfg(); sc["seenCount"] = sc.value("seen").toArray().size(); sc.remove("seen"); sc.remove("fails"); o["scatter"] = sc;
    QJsonObject t = m_store->settingJson("spots.tts"); if (!t.contains("engine")) t["engine"] = "auto"; if (!t.contains("speed")) t["speed"] = 1.0; t["pythonResolved"] = pythonPath(); o["tts"] = t;
    o["ffmpeg"] = m_store->setting("spots.ffmpeg"); o["ffmpegResolved"] = ffmpegPath();
    o["keepDays"] = m_store->setting("spots.keepDays", "3").toDouble();
    QJsonObject b = m_store->settingJson("spots.brand"); if (!b.contains("accent")) b["accent"] = "#d80018"; if (!b.contains("ink")) b["ink"] = "#0b1220"; o["brand"] = b;
    o["musicDir"] = QDir::toNativeSeparators(musicDir()); o["backgroundsDir"] = QDir::toNativeSeparators(backgroundsDir()); o["outputDir"] = QDir::toNativeSeparators(m_outDir); o["scriptsDir"] = QDir::toNativeSeparators(m_scriptsDir);
    QJsonObject env; env["ttsScript"] = QFileInfo(QDir(m_scriptsDir).filePath("spot_tts.py")).exists(); env["cardScript"] = QFileInfo(QDir(m_scriptsDir).filePath("spot_card.py")).exists(); env["ffmpeg"] = QFileInfo(ffmpegPath()).exists() || ffmpegPath() == "ffmpeg"; env["python"] = QFileInfo(pythonPath()).exists() || pythonPath() == "python";
    { QJsonObject mu = m_store->settingJson("spots.music"); if (!mu.contains("mode")) mu["mode"] = "auto"; o["music"] = mu; env["musicScript"] = QFileInfo(QDir(m_scriptsDir).filePath("spot_music.py")).exists(); }
    env["music"] = QDir(musicDir()).entryList({"*.mp3", "*.wav", "*.m4a", "*.flac", "*.ogg"}, QDir::Files).size(); env["backgrounds"] = QDir(backgroundsDir()).entryList({"*.jpg", "*.jpeg", "*.png"}, QDir::Files).size();
    {   // DEP-13: the switch settings as stored (unset = the old program) and what each step resolves to right now
        QJsonObject es = m_store->settingJson("spots.engines"), er;
        for (const StepDef& d : kSteps) { if (!es.contains(d.step)) es[d.step] = d.legacy; er[d.step] = engineFor(d.step); }
        es["dirResolved"] = QDir::toNativeSeparators(es.value("dir").toString().isEmpty() ? QCoreApplication::applicationDirPath() : es.value("dir").toString());
        o["engines"] = es; env["engines"] = er;
        env["kokoroPackage"] = m_pyKokoro < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(m_pyKokoro > 0);
    }
    o["env"] = env;
    o["market"] = marketInfo();
    return o;
}
QJsonObject SpotGenerator::setConfig(const QJsonObject& p) {
    auto merge = [this](const QString& key, const QJsonObject& patch, const QStringList& keepEmpty = {}) { QJsonObject cur = m_store->settingJson(key); for (auto it = patch.begin(); it != patch.end(); ++it) { if (keepEmpty.contains(it.key()) && it.value().toString().isEmpty()) continue; cur[it.key()] = it.value(); } m_store->setSettingJson(key, cur); };
    if (p.contains("weather")) merge("spots.weather", p.value("weather").toObject());
    if (p.contains("llm")) merge("spots.llm", p.value("llm").toObject(), {"apiKey"});
    if (p.contains("scatter")) { QJsonObject sc = p.value("scatter").toObject(); sc.remove("seen"); sc.remove("fails"); sc.remove("seenCount"); merge("spots.scatter", sc); }
    if (p.contains("tts")) { merge("spots.tts", p.value("tts").toObject()); if (p.value("tts").toObject().contains("python")) m_pyKokoro = -1; }   // another Python: ask it again
    if (p.contains("engines")) {     // {card, tts, music, compose: auto|native|python|ffmpeg|"" (= default), dir}; an unknown mode is refused (kept as it was)
        const QJsonObject in = p.value("engines").toObject(); QJsonObject cur = m_store->settingJson("spots.engines");
        for (const StepDef& d : kSteps) {
            if (!in.contains(d.step)) continue;
            const QString v = in.value(d.step).toString().trimmed().toLower();
            if (!toolswitch::validModeValue(v)) continue;
            if (v.isEmpty()) cur.remove(d.step); else cur[d.step] = v;
        }
        if (in.contains("dir")) { const QString dir = in.value("dir").toString().trimmed(); if (dir.isEmpty()) cur.remove("dir"); else cur["dir"] = QDir::fromNativeSeparators(dir); }
        m_store->setSettingJson("spots.engines", cur); m_toolCaps->cache.clear();
    }
    if (p.contains("music")) { QJsonObject mu = p.value("music").toObject(); const QString m = mu.value("mode").toString(); if (!m.isEmpty() && m != "auto" && m != "generated" && m != "files" && m != "off") mu.remove("mode"); merge("spots.music", mu); }
    if (p.contains("brand")) merge("spots.brand", p.value("brand").toObject());
    if (p.contains("recipes")) m_store->setSettingJson("spots.recipes", p.value("recipes").toObject());
    if (p.contains("market")) {      // station default market: {default: "danville" | "none" | ""}; an unknown key is refused (kept as it was)
        const QString k = areamarket::normalizeKey(p.value("market").toObject().value("default").toString());
        if (k.isEmpty() || k == "none" || !marketData(k).isEmpty()) { QJsonObject cur = m_store->settingJson("spots.market"); cur["default"] = k; m_store->setSettingJson("spots.market", cur); }
    }
    if (p.contains("markets")) m_store->setSettingJson("spots.markets", p.value("markets").toObject());      // operator-defined markets / amendments to the built-ins
    if (p.contains("ffmpeg")) m_store->setSetting("spots.ffmpeg", p.value("ffmpeg").toString());
    if (p.contains("musicDir")) m_store->setSetting("spots.musicDir", p.value("musicDir").toString());
    if (p.contains("backgroundsDir")) m_store->setSetting("spots.backgroundsDir", p.value("backgroundsDir").toString());
    if (p.contains("keepDays")) m_store->setSetting("spots.keepDays", QString::number(qMax(0.5, p.value("keepDays").toDouble(3))));
    notify();
    return config();
}

// ---------------------------------------------------------------------------------------------
// Jobs (settings spots.jobs = {jobs: [...]})
// ---------------------------------------------------------------------------------------------
QJsonArray SpotGenerator::loadJobs() const { return m_store->settingJson("spots.jobs").value("jobs").toArray(); }
void SpotGenerator::storeJobs(const QJsonArray& a) { QJsonObject o; o["jobs"] = a; m_store->setSettingJson("spots.jobs", o); }
QJsonArray SpotGenerator::jobs() const {
    QJsonArray out; const qint64 now = nowMs();
    for (const QJsonValue& v : loadJobs()) { QJsonObject j = v.toObject(); bool due = false; j["nextRunMs"] = double(nextDueMs(j, now, &due)); j["due"] = due; j["ttlMin"] = ttlForJob(j); j["cadenceMin"] = effectiveCadenceMin(j); out.append(j); }
    return out;
}
QJsonObject SpotGenerator::job(const QString& id, bool* found) const { for (const QJsonValue& v : jobs()) if (v.toObject().value("id").toString() == id) { if (found) *found = true; return v.toObject(); } if (found) *found = false; return QJsonObject(); }

QJsonObject SpotGenerator::normalizeJob(const QJsonObject& in) const {
    QJsonObject j = in;
    if (j.value("id").toString().isEmpty()) j["id"] = shortId();
    bool rf = false; const QJsonObject r = recipe(j.value("recipe").toString(), &rf); if (!rf) j["recipe"] = "weather";
    j["enabled"] = j.value("enabled").toBool(false);
    if (j.value("label").toString().isEmpty()) j["label"] = (rf ? r : recipe("weather")).value("label").toString();
    QJsonObject ev = j.value("every").toObject(); const QString kind = ev.value("kind").toString("interval") == "clock" ? "clock" : "interval";
    ev["kind"] = kind; ev["minutes"] = qMax(5, ev.value("minutes").toInt(60));
    QJsonArray times; for (const QJsonValue& t : ev.value("times").toArray()) { const QString s = t.toString().trimmed(); static const QRegularExpression rx("^(\\d{1,2}):?(\\d{2})$"); const auto m = rx.match(s); if (!m.hasMatch()) continue; const int h = m.captured(1).toInt(), mi = m.captured(2).toInt(); if (h > 23 || mi > 59) continue; const QString norm = QString("%1:%2").arg(h, 2, 10, QChar('0')).arg(mi, 2, 10, QChar('0')); if (!times.contains(norm)) times.append(norm); }
    QStringList tl; for (const QJsonValue& t : times) tl << t.toString(); std::sort(tl.begin(), tl.end()); times = QJsonArray(); for (const QString& t : tl) times.append(t);
    ev["times"] = times; if (kind == "clock" && times.isEmpty()) ev["kind"] = "interval"; j["every"] = ev;
    QJsonArray dows; for (const QJsonValue& d : j.value("dows").toArray()) { const int x = d.toInt(-1); if (x >= 0 && x <= 6 && !dows.contains(x)) dows.append(x); } j["dows"] = dows;
    j["voice"] = j.value("voice").toString(); j["data"] = j.value("data").toString().left(8000); j["channel"] = j.value("channel").toString();
    { const QString mk = areamarket::normalizeKey(j.value("market").toString()); j["market"] = (mk == "none" || (!mk.isEmpty() && !marketData(mk).isEmpty())) ? mk : QString(); }      // "" = inherit (recipe override, then the station default)
    QJsonArray feeds; for (const QJsonValue& f : j.value("feeds").toArray()) if (f.toString().startsWith("http")) feeds.append(f.toString().trimmed()); j["feeds"] = feeds;
    QJsonArray pls; for (const QJsonValue& pv : j.value("placements").toArray()) {
        QJsonObject p = pv.toObject(); QJsonObject c; c["channel"] = p.value("channel").toString(j.value("channel").toString());
        if (p.value("hours").isString() || p.value("hours").toArray().isEmpty()) c["hours"] = "all"; else { QJsonArray hs; for (const QJsonValue& h : p.value("hours").toArray()) { const int x = h.toInt(-1); if (x >= 0 && x <= 23 && !hs.contains(x)) hs.append(x); } c["hours"] = hs.isEmpty() ? QJsonValue("all") : QJsonValue(hs); }
        QJsonArray ds; for (const QJsonValue& d : p.value("dows").toArray()) { const int x = d.toInt(-1); if (x >= 0 && x <= 6 && !ds.contains(x)) ds.append(x); } c["dows"] = ds;
        c["atMin"] = qBound(0, p.value("atMin").toInt(0), 59); pls.append(c); if (pls.size() >= 12) break;
    }
    j["placements"] = pls;
    j["targetSec"] = qMax(0, j.value("targetSec").toInt(0)); j["ttlMin"] = qMax(0.0, j.value("ttlMinOverride").toDouble(j.value("ttlMin").toDouble(0))); j.remove("ttlMinOverride");
    j["expirePrior"] = j.value("expirePrior").toBool(true);
    if (!j.contains("createdMs")) j["createdMs"] = double(nowMs());
    if (!j.contains("lastRunMs")) j["lastRunMs"] = 0.0; if (!j.contains("lastMediaId")) j["lastMediaId"] = ""; if (!j.contains("lastError")) j["lastError"] = ""; if (!j.contains("lastHeadline")) j["lastHeadline"] = "";
    j.remove("nextRunMs"); j.remove("due"); j.remove("cadenceMin");
    return j;
}
void SpotGenerator::seedJobs() {
    if (m_store->setting("spots.seeded") == "1" || !loadJobs().isEmpty()) return;
    const QString ch = firstChannel(); QJsonArray a;
    QJsonObject w; w["recipe"] = "weather"; w["label"] = "Weather"; w["enabled"] = false; w["channel"] = ch; QJsonObject ev; ev["kind"] = "interval"; ev["minutes"] = 60; w["every"] = ev; QJsonObject p; p["channel"] = ch; p["hours"] = "all"; p["atMin"] = 5; w["placements"] = QJsonArray{p}; w["genNum"] = 1; a.append(normalizeJob(w));
    QJsonObject h; h["recipe"] = "headlines"; h["label"] = "Headlines"; h["enabled"] = false; h["channel"] = ch; QJsonObject ev2; ev2["kind"] = "interval"; ev2["minutes"] = 120; h["every"] = ev2; QJsonObject p2; p2["channel"] = ch; p2["hours"] = "all"; p2["atMin"] = 35; h["placements"] = QJsonArray{p2}; h["genNum"] = 2; a.append(normalizeJob(h));
    storeJobs(a); m_store->setSetting("spots.seeded", "1");
    qInfo() << "[Spots] seeded 2 default jobs (disabled): weather every 60 min, headlines every 120 min";
}
QJsonObject SpotGenerator::saveJob(const QJsonObject& body, const QString& id) {
    QJsonArray all = loadJobs(); int idx = -1;
    for (int i = 0; i < all.size(); ++i) if (!id.isEmpty() && all[i].toObject().value("id").toString() == id) idx = i;
    if (!id.isEmpty() && idx < 0) { QJsonObject e; e["error"] = "no such job"; e["status"] = 404; return e; }
    QJsonObject j = idx >= 0 ? all[idx].toObject() : QJsonObject();
    for (auto it = body.begin(); it != body.end(); ++it) { if (it.key() == "id" || it.key() == "genNum" || it.key() == "lastMediaId" || it.key() == "lastRunMs" || it.key() == "createdMs") continue; j[it.key()] = it.value(); }
    if (idx < 0) { int n = 1; bool used = true; while (used) { used = false; for (const QJsonValue& v : all) if (v.toObject().value("genNum").toInt() == n) used = true; if (used) ++n; } j["genNum"] = n; j.remove("id"); }
    else j["id"] = id;
    j = normalizeJob(j);
    if (idx >= 0) all[idx] = j; else all.append(j);
    storeJobs(all);
    applyPlacementsFor(j);                        // placements changed? mirror them into the grid now
    notify();
    bool f = false; return job(j.value("id").toString(), &f);
}
bool SpotGenerator::deleteJob(const QString& id) {
    QJsonArray all = loadJobs(), out; QJsonObject gone;
    for (const QJsonValue& v : all) { if (v.toObject().value("id").toString() == id) gone = v.toObject(); else out.append(v); }
    if (gone.isEmpty()) return false;
    storeJobs(out); applyPlacementsFor(gone, true);
    for (Gen* g : m_gens) if (g->jobId == id && g->step == "queued") { g->step = "cancelled"; }
    notify(); return true;
}

double SpotGenerator::effectiveCadenceMin(const QJsonObject& j) const {
    const QJsonObject ev = j.value("every").toObject();
    if (ev.value("kind").toString() != "clock") return double(qMax(5, ev.value("minutes").toInt(60)));
    const QJsonArray times = ev.value("times").toArray(); if (times.isEmpty()) return double(qMax(5, ev.value("minutes").toInt(60)));
    QList<int> dows; for (const QJsonValue& d : j.value("dows").toArray()) dows << d.toInt(); if (dows.isEmpty()) dows = {0, 1, 2, 3, 4, 5, 6};
    QList<int> mins; for (int d : dows) for (const QJsonValue& t : times) mins << d * 1440 + t.toString().left(2).toInt() * 60 + t.toString().mid(3, 2).toInt();
    std::sort(mins.begin(), mins.end()); if (mins.size() == 1) return 10080.0;
    int best = mins.first() + 10080 - mins.last(); for (int i = 1; i < mins.size(); ++i) best = qMax(best, mins[i] - mins[i - 1]);
    return double(best);
}
double SpotGenerator::ttlForJob(const QJsonObject& j) const {
    const QJsonObject r = recipe(j.value("recipe").toString());
    double ttl = j.value("ttlMin").toDouble(0); if (ttl <= 0) ttl = r.value("ttlMin").toDouble(720);
    return qMax(ttl, effectiveCadenceMin(j) * 1.25);        // a window never closes before the successor is due
}
/// Next due instant for a job; *dueNow when it should fire on this tick (clock jobs get a 20-minute grace window).
qint64 SpotGenerator::nextDueMs(const QJsonObject& j, qint64 now, bool* dueNow) const {
    if (dueNow) *dueNow = false;
    const QJsonObject ev = j.value("every").toObject(); const qint64 last = qint64(j.value("lastRunMs").toDouble());
    if (ev.value("kind").toString() != "clock") {
        const qint64 next = last > 0 ? last + qint64(qMax(5, ev.value("minutes").toInt(60))) * 60000LL : now;
        if (dueNow) *dueNow = next <= now; return next;
    }
    const QJsonArray times = ev.value("times").toArray(); if (times.isEmpty()) return 0;
    QList<int> dows; for (const QJsonValue& d : j.value("dows").toArray()) dows << d.toInt();
    const QTimeZone tz = tzFor(defaultChannelFor(j)); const QDateTime nowL = QDateTime::fromMSecsSinceEpoch(now, tz);
    const qint64 grace = 20 * 60000LL; qint64 next = 0;
    for (int day = -1; day <= 7; ++day) {
        const QDate d = nowL.date().addDays(day); if (!dows.isEmpty() && !dows.contains(scheduleDow(d))) continue;
        for (const QJsonValue& t : times) {
            const QDateTime slot(d, QTime(t.toString().left(2).toInt(), t.toString().mid(3, 2).toInt()), tz); const qint64 sm = slot.toMSecsSinceEpoch();
            if (sm <= now && now <= sm + grace && last < sm) { if (dueNow) *dueNow = true; return sm; }
            if (sm > now && (next == 0 || sm < next)) next = sm;
        }
    }
    return next;
}
void SpotGenerator::tick() {
    if (m_deferEnforceMs > 0 && nowMs() >= m_deferEnforceMs) {      // the hour that held a placement change back is over: apply it now
        m_deferEnforceMs = 0; const QSet<QString> ids = m_deferredJobs; m_deferredJobs.clear();
        for (const QString& id : ids) applyPlacements(id);
        enforceMarkersNow();
    }
    runDueJobs(); scatterTick();
}
void SpotGenerator::runDueJobs() {
    const qint64 now = nowMs();
    for (const QJsonValue& v : loadJobs()) {
        const QJsonObject j = v.toObject(); if (!j.value("enabled").toBool()) continue;
        bool due = false; nextDueMs(j, now, &due); if (!due) continue;
        bool busy = false; for (Gen* g : m_gens) if (g->jobId == j.value("id").toString() && g->step != "cancelled") busy = true;
        if (!busy) runJob(j.value("id").toString(), Done());
    }
}

// ---------------------------------------------------------------------------------------------
// Generation queue
// ---------------------------------------------------------------------------------------------
QJsonObject SpotGenerator::Gen::toJson() const {
    QJsonObject o; o["id"] = id; o["jobId"] = jobId; o["recipe"] = recipeKey; o["channel"] = channel; o["origin"] = origin; o["step"] = step; o["error"] = error; o["headline"] = headline; o["voice"] = voice;
    o["market"] = market; o["queuedMs"] = double(queuedMs); o["startedMs"] = double(startedMs); o["finishedMs"] = double(finishedMs); o["durationSec"] = durSec; o["targetSec"] = targetSec; o["preview"] = previewOnly; o["ttsEngine"] = ttsEngine; o["storyUrl"] = storyUrl; if (!engines.isEmpty()) o["engines"] = engines;
    static const QStringList steps = {"queued", "fetch", "write", "photo", "card", "tts", "music", "compose", "ingest", "done"}; const int i = steps.indexOf(step); o["pct"] = i < 0 ? 0 : int(100.0 * i / (steps.size() - 1));
    return o;
}
SpotGenerator::Gen* SpotGenerator::findGen(const QString& id) const { for (Gen* g : m_gens) if (g->id == id) return g; return nullptr; }
SpotGenerator::Gen* SpotGenerator::enqueue(Gen* g) { g->id = shortId(); g->queuedMs = nowMs(); if (g->step.isEmpty()) g->step = "queued"; m_gens.append(g); notify(); QTimer::singleShot(0, this, &SpotGenerator::pump); return g; }
void SpotGenerator::pump() {
    if (m_current) return;
    Gen* next = nullptr; for (Gen* g : m_gens) if (g->step == "queued") { next = g; break; }
    for (int i = m_gens.size() - 1; i >= 0; --i) if (m_gens[i]->step == "cancelled") { Gen* g = m_gens.takeAt(i); for (const Done& d : g->done) if (d) { QJsonObject e; e["error"] = "cancelled"; d(false, e); } delete g; }
    if (!next) return;
    m_current = next; startGen(next);
}
bool SpotGenerator::cancel(const QString& genId) {
    Gen* g = findGen(genId); if (!g) return false;
    if (g == m_current) { if (m_proc) m_proc->kill(); failGen(g, "cancelled"); return true; }
    if (g->step == "queued") { g->step = "cancelled"; pump(); return true; }
    return false;
}

QJsonObject SpotGenerator::runJob(const QString& id, Done done) {
    bool f = false; const QJsonObject j = job(id, &f);
    if (!f) { QJsonObject e; e["error"] = "no such job"; e["status"] = 404; return e; }
    for (Gen* g : m_gens) if (g->jobId == id && g->step != "cancelled" && !g->previewOnly) { if (done) g->done.append(done); QJsonObject o; o["queued"] = true; o["genId"] = g->id; o["already"] = true; return o; }
    bool rf = false; const QJsonObject r = recipe(j.value("recipe").toString(), &rf);
    if (!rf) { QJsonObject e; e["error"] = "job recipe is unknown"; e["status"] = 400; return e; }
    // stamp lastRun first so a slow render cannot double-fire on the next tick
    QJsonArray all = loadJobs(); for (int i = 0; i < all.size(); ++i) if (all[i].toObject().value("id").toString() == id) { QJsonObject jj = all[i].toObject(); jj["lastRunMs"] = double(nowMs()); all[i] = jj; } storeJobs(all);
    Gen* g = new Gen; g->jobId = id; g->recipeKey = r.value("key").toString(); g->recipe = r; g->channel = defaultChannelFor(j); g->data = j.value("data").toString(); g->voice = j.value("voice").toString(); g->feeds = j.value("feeds").toArray();
    bindMarket(g, QString(), j);
    g->origin = "job"; g->ttlMin = ttlForJob(j); g->targetSec = j.value("targetSec").toInt() > 0 ? j.value("targetSec").toInt() : r.value("targetSec").toInt(60); g->genNum = j.value("genNum").toInt(); if (done) g->done.append(done);
    enqueue(g); QJsonObject o; o["queued"] = true; o["genId"] = g->id; return o;
}
QJsonObject SpotGenerator::generate(const QJsonObject& req, Done done) {
    bool rf = false; const QJsonObject r = recipe(req.value("recipe").toString(), &rf);
    if (!rf) { QJsonObject e; e["error"] = "unknown recipe"; e["status"] = 400; return e; }
    Gen* g = new Gen; g->recipeKey = r.value("key").toString(); g->recipe = r; g->channel = req.value("channel").toString(); g->data = req.value("data").toString().left(12000); g->voice = req.value("voice").toString(); g->origin = "manual"; bindMarket(g, req.value("market").toString(), QJsonObject());
    for (const QJsonValue& f : req.value("feeds").toArray()) if (f.toString().startsWith("http")) g->feeds.append(f.toString());
    g->ttlMin = req.value("ttlMin").toDouble(r.value("ttlMin").toDouble(720)); g->targetSec = req.value("targetSec").toInt(r.value("targetSec").toInt(60)); g->jobId = req.value("jobId").toString(); g->notes = req.value("notes").toString().left(2000).trimmed(); g->extraMeta = req.value("meta").toObject(); if (done) g->done.append(done);
    enqueue(g); QJsonObject o; o["queued"] = true; o["genId"] = g->id; return o;
}
void SpotGenerator::previewScript(const QJsonObject& req, std::function<void(const QJsonObject&)> cb) {
    bool rf = false; const QJsonObject r = recipe(req.value("recipe").toString(), &rf);
    if (!rf) { QJsonObject e; e["error"] = "unknown recipe"; e["status"] = 400; cb(e); return; }
    Gen* g = new Gen; g->recipeKey = r.value("key").toString(); g->recipe = r; g->channel = req.value("channel").toString(); g->data = req.value("data").toString().left(12000); g->origin = "preview"; g->previewOnly = true; bindMarket(g, req.value("market").toString(), QJsonObject());
    for (const QJsonValue& f : req.value("feeds").toArray()) if (f.toString().startsWith("http")) g->feeds.append(f.toString());
    g->targetSec = req.value("targetSec").toInt(r.value("targetSec").toInt(60));
    g->done.append([cb](bool ok, const QJsonObject& res) { QJsonObject o = res; if (!ok && !o.contains("status")) o["status"] = 500; cb(o); });
    enqueue(g);
}

// ---------------------------------------------------------------------------------------------
// Pipeline: fetch -> write -> photo -> card -> tts -> compose -> ingest
// ---------------------------------------------------------------------------------------------
void SpotGenerator::startGen(Gen* g) {
    g->startedMs = nowMs(); g->workDir = QDir(workRoot()).filePath(g->id); QDir().mkpath(g->workDir);
    g->step = "fetch"; notify();
    if (g->materialReady) { onFetched(g); return; }
    fetchSources(g);
}
QString SpotGenerator::weatherUrl(const Gen* g) const {
    const QString& channel = g->channel; const QJsonObject w = weatherFor(g); const bool us = w.value("units").toString("us") != "metric";
    // ONE multi-argument arg(): a chained .arg() rescans the whole string, and the percent-encoded zone ("America%2FNew_York")
    // carries a "%2" that the next .arg() would fill, leaving "%4" in the URL and an invalid timezone for Open-Meteo.
    return QString("https://api.open-meteo.com/v1/forecast?latitude=%1&longitude=%2&timezone=%3%4&current=temperature_2m,apparent_temperature,precipitation,weather_code,wind_speed_10m,relative_humidity_2m,is_day&hourly=temperature_2m,precipitation_probability,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset&forecast_days=7")
        .arg(QString::number(w.value("lat").toDouble(36.69), 'f', 4), QString::number(w.value("lon").toDouble(-79.87), 'f', 4),
             QString::fromUtf8(QUrl::toPercentEncoding(QString::fromUtf8(tzFor(channel).id()))),
             us ? QString("&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch") : QString());
}
QStringList SpotGenerator::feedsFor(const QJsonObject& r, const QJsonArray& jobFeeds) const {
    QStringList out; for (const QJsonValue& f : jobFeeds) out << f.toString(); if (!out.isEmpty()) return out;
    for (const QJsonValue& f : r.value("feeds").toArray()) out << f.toString(); return out;
}
void SpotGenerator::fetchSources(Gen* g) {
    const QString kind = g->recipe.value("source").toString("rss"); const QString gid = g->id;
    if (sceneRender(g)) { fetchLocal(g); return; }
    QStringList urls;
    if (kind == "weather") urls << weatherUrl(g);
    else if (kind == "history") { const QDate d = nowIn(g->channel).date(); urls << QString("https://en.wikipedia.org/api/rest_v1/feed/onthisday/events/%1/%2").arg(d.month(), 2, 10, QChar('0')).arg(d.day(), 2, 10, QChar('0')); }
    else if (kind == "rss" && (g->data.trimmed().isEmpty() || !g->feeds.isEmpty())) urls = feedsFor(g->recipe, g->feeds);
    if (urls.isEmpty()) { onFetched(g); return; }
    g->pending = urls.size();
    for (const QString& u : urls) {
        httpGet(u, 25000, [this, gid, u, kind](bool ok, const QByteArray& body, const QString& ctype, const QString& err) {
            Gen* g = findGen(gid); if (!g || g->step != "fetch") return;
            if (!ok) { log("warning", QString("%1: source unavailable: %2 (%3)").arg(g->recipeKey, u, err)); }
            else if (kind == "weather") g->weather = QJsonDocument::fromJson(body).object();
            else if (kind == "history") g->wiki = QJsonDocument::fromJson(body).object();
            else { const bool json = ctype.contains("json") || body.trimmed().startsWith('{'); if (!json) { for (const QJsonValue& it : parseFeed(body, QUrl(u).host(), 40)) g->items.append(it); } }
            if (--g->pending <= 0) onFetched(g);
        });
    }
}
void SpotGenerator::onFetched(Gen* g) {
    // filter + order the material: recipe keyword filter, newest first, cap
    const QString filter = g->recipe.value("filter").toString();
    // stale material is dropped before anything else: a feed that stopped updating must not air its last crash / score / headline as current
    const double maxAgeH = g->recipe.value("maxAgeHours").toDouble(0);
    if (maxAgeH > 0 && !g->items.isEmpty()) { const double cutoff = double(nowMs()) - maxAgeH * 3600000.0; QJsonArray fresh; for (const QJsonValue& v : g->items) { const double pub = v.toObject().value("pubMs").toDouble(); if (pub <= 0 || pub >= cutoff) fresh.append(v); } g->items = fresh; }
    // not headlines: a bare clock time ("10:38 p.m.") or a short photo caption with nothing behind it ("Welcome Race Fans!")
    if (!g->items.isEmpty()) { QJsonArray kept; for (const QJsonValue& v : g->items) { const QJsonObject it = v.toObject(); if (!headline::isJunk(it.value("title").toString(), it.value("text").toString())) kept.append(it); } g->items = kept; }
    if (!filter.isEmpty() && !g->items.isEmpty()) { const QRegularExpression rx(filter, QRegularExpression::CaseInsensitiveOption); QJsonArray kept; for (const QJsonValue& v : g->items) { const QJsonObject it = v.toObject(); if (rx.match(it.value("title").toString() + " " + it.value("text").toString()).hasMatch()) kept.append(it); } g->items = kept; }
    // an area market narrows corridor-wide feeds to its own towns and counties (title, text, category and link all count: getstarnews links carry /local/<place>/)
    const QString localFilter = g->recipe.value("localFilter").toString();
    if (!localFilter.isEmpty() && !g->items.isEmpty()) { const QRegularExpression rx(localFilter, QRegularExpression::CaseInsensitiveOption); QJsonArray kept; for (const QJsonValue& v : g->items) { const QJsonObject it = v.toObject(); if (rx.match(it.value("title").toString() + " " + it.value("text").toString() + " " + it.value("category").toString() + " " + it.value("link").toString()).hasMatch()) kept.append(it); } g->items = kept; }
    QList<QJsonObject> list; for (const QJsonValue& v : g->items) list << v.toObject();
    std::stable_sort(list.begin(), list.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("pubMs").toDouble() > b.value("pubMs").toDouble(); });
    const int cap = storiesMode(g) ? qMax(12, 3 * storyCount(g)) : 12;    // Headline News checks more pages: some posts have no picture of their own
    QSet<QString> seen; g->items = QJsonArray(); for (const QJsonObject& o : list) { const QString k = o.value("title").toString().toLower(); if (seen.contains(k)) continue; seen.insert(k); g->items.append(o); if (g->items.size() >= cap) break; }
    const QString kind = g->recipe.value("source").toString("rss");
    const bool nothing = (kind == "rss" && g->items.isEmpty() && g->data.trimmed().size() < 40) || (kind == "weather" && g->weather.isEmpty()) || (kind == "history" && g->wiki.value("events").toArray().isEmpty());
    if (nothing) {
        if (g->recipe.value("skipIfNone").toBool() && g->data.trimmed().isEmpty()) { g->skipped = true; QJsonObject o; o["skipped"] = true; o["error"] = g->recipe.value("label").toString() + ": nothing to report this cycle"; finishGen(g, false, o); return; }
        if (kind != "rss" || g->data.trimmed().isEmpty()) { failGen(g, "no material: source fetch failed or returned nothing"); return; }
    }
    if (storiesMode(g) && !g->items.isEmpty()) { enrichStories(g); return; }
    writeScript(g);
}

// ---------------------------------------------------------------------------------------------
// Headline News (render "stories"): one scene per story, over the post's own photo
// ---------------------------------------------------------------------------------------------
bool SpotGenerator::sceneRender(const Gen* g) const {
    static const QSet<QString> kinds = {"cinema", "traffic", "sports", "realty", "markets", "weather_full"};
    return g->recipe.value("render").toString() == "scenes" && kinds.contains(g->recipe.value("source").toString());
}
bool SpotGenerator::storiesMode(const Gen* g) const { return sceneRender(g) || (g->recipe.value("render").toString() == "stories" && g->recipe.value("source").toString("rss") == "rss"); }
int SpotGenerator::storyCount(const Gen* g) const { return qBound(1, g->recipe.value("stories").toInt(5), 8); }

// ---------------------------------------------------------------------------------------------
// Local spots (render "scenes"): cinema showtimes, 511 traffic, MaxPreps scores, a featured home.
// The page is parsed by localspots.*; the scenes it writes carry their own narration and photo URLs, then
// the Headline News scene pipeline takes over (photos -> furniture cards -> voices -> ffmpeg).
// ---------------------------------------------------------------------------------------------
/// Open-Meteo with everything the weather spot reads: current, 8 days (3 past, for the rain total), hourly humidity; US units.
QString SpotGenerator::weatherFullUrl(const Gen* g) const {
    const QString& channel = g->channel; const QJsonObject w = weatherFor(g);
    return QString("https://api.open-meteo.com/v1/forecast?latitude=%1&longitude=%2&timezone=%3&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch&past_days=3&forecast_days=8"
                   "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,weather_code,wind_speed_10m,wind_gusts_10m,is_day"
                   "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum,wind_speed_10m_max,wind_gusts_10m_max,sunrise,sunset,daylight_duration,uv_index_max"
                   "&hourly=relative_humidity_2m,wind_gusts_10m,precipitation,temperature_2m")
        .arg(QString::number(w.value("lat").toDouble(36.69), 'f', 4), QString::number(w.value("lon").toDouble(-79.87), 'f', 4), QString::fromUtf8(QUrl::toPercentEncoding(QString::fromUtf8(tzFor(channel).id()))));
}

/// Lake readings for the weather spot. AEP's page gives only the present level, so each run appends one to a short history (settings spots.lakes,
/// 8 days) and the 24-hour change is read from the sample nearest to a day ago; Philpott's change comes with the USGS series itself.
QJsonObject SpotGenerator::lakesFrom(const QJsonObject& aep, const QJsonObject& usgs, bool persist) {
    QJsonObject hist = m_store->settingJson("spots.lakes"), out; const qint64 now = nowMs();
    auto track = [&](const QString& key, const QJsonObject& src) {
        if (!src.value("actual").isDouble()) return;
        const double actual = src.value("actual").toDouble(); QJsonArray h = hist.value(key).toArray();
        double prev = qQNaN(); qint64 best = 6LL * 3600000LL;
        for (const QJsonValue& v : h) { const QJsonArray p = v.toArray(); const qint64 d = qAbs(qint64(p.at(0).toDouble()) - (now - 86400000LL)); if (d < best) { best = d; prev = p.at(1).toDouble(); } }
        if (persist && (h.isEmpty() || now - qint64(h.last().toArray().at(0).toDouble()) >= 30 * 60000LL)) {
            QJsonArray pt; pt.append(double(now)); pt.append(actual); h.append(pt);
            while (!h.isEmpty() && now - qint64(h.first().toArray().at(0).toDouble()) > 8LL * 86400000LL) h.removeFirst();
            hist[key] = h;
        }
        QJsonObject o; o["actual"] = actual; if (src.value("fullPond").toDouble() > 0) o["fullPond"] = src.value("fullPond"); if (!std::isnan(prev)) o["prev24"] = prev;
        out[key] = o;
    };
    track("smithMountain", aep.value("smith mountain").toObject());
    track("leesville", aep.value("leesville").toObject());
    if (usgs.value("elevation").isDouble()) { QJsonObject o; o["actual"] = usgs.value("elevation"); if (usgs.contains("changeFt")) o["changeFt"] = usgs.value("changeFt"); out["philpott"] = o; }
    if (persist) m_store->setSettingJson("spots.lakes", hist);
    return out;
}

void SpotGenerator::fetchLocal(Gen* g) {
    const QString kind = g->recipe.value("source").toString(), gid = g->id;
    struct Req { QString url, layer, school, state, sport; QJsonObject spec; };
    QList<Req> reqs;
    const QString marketName = g->marketJson.value("name").toString(), label = g->recipe.value("label").toString();
    // A market names a source of its own (recipes.cinema.url, recipes.realty.url). A blank one means there is none for that market: skip, never borrow another town's page.
    if ((kind == "cinema" || kind == "realty") && g->recipe.contains("url") && g->recipe.value("url").toString().trimmed().isEmpty()) {
        g->skipped = true; QJsonObject o; o["skipped"] = true;
        o["error"] = QString("%1: nothing to report this cycle (no %2 source is set for %3: add recipes.%4.url to spots.markets.%5)").arg(label, kind == "cinema" ? "cinema" : "realty listings", marketName.isEmpty() ? QString("this recipe") : marketName, g->recipeKey, g->market.isEmpty() ? QString("<market>") : g->market);
        finishGen(g, false, o); return;
    }
    // NCDOT's DriveNC feed needs an API key this install does not hold: say so rather than air an all-clear built from another state's feed.
    if (kind == "traffic" && g->recipe.value("provider").toString("vdot") != "vdot") {
        failGen(g, QString("%1: traffic for %2 comes from %3, which needs an API key that is not set up (the VDOT 511 feed covers Virginia only)").arg(label, marketName.isEmpty() ? QString("this market") : marketName, g->recipe.value("siteName").toString(g->recipe.value("provider").toString()))); return;
    }
    if (kind == "cinema") reqs << Req{g->recipe.value("url").toString("https://hollywoodcinema.net/"), {}, {}, {}, {}};
    else if (kind == "realty") reqs << Req{g->recipe.value("url").toString("https://havenmore.com/"), {}, {}, {}, {}};
    else if (kind == "weather_full") {
        const QJsonObject w = weatherFor(g);
        const QString lat = QString::number(w.value("lat").toDouble(36.69), 'f', 4), lon = QString::number(w.value("lon").toDouble(-79.87), 'f', 4);
        Req f; f.url = weatherFullUrl(g); f.layer = "forecast"; reqs << f;
        Req n; n.url = QString("https://api.weather.gov/alerts/active?point=%1,%2").arg(lat, lon); n.layer = "alerts"; reqs << n;                    // fire-weather alerts for the burn rating
        QSet<QString> segs; for (const QJsonValue& sv : g->recipe.value("segments").toArray()) segs.insert(sv.toString());
        if (segs.isEmpty() || segs.contains("lakes")) {                                                                                              // a market without lake data (Danville, Reidsville) names its segments without "lakes"
        Req a; a.url = "https://www.aep.com/recreation/hydro/"; a.layer = "aep"; reqs << a;                                                         // Smith Mountain and Leesville
        Req u; u.url = "https://waterservices.usgs.gov/nwis/iv/?format=json&sites=02071900&parameterCd=62614,00052,00045,00021&siteStatus=all&period=P2D"; u.layer = "usgs"; reqs << u; }   // Philpott
    } else if (kind == "markets") {
        QJsonArray syms = g->recipe.value("symbols").toArray(); if (syms.isEmpty()) syms = marketspots::defaultSymbols();
        for (const QJsonValue& v : syms) { const QJsonObject sp = v.toObject(); if (sp.value("symbol").toString().isEmpty()) continue; Req r; r.url = marketspots::chartUrl(sp.value("symbol").toString()); r.spec = sp; reqs << r; }
    } else if (kind == "traffic") {
        const QString base = g->recipe.value("base").toString("https://511.vdot.virginia.gov/services/511/map/layers/map/");
        QJsonArray layers = g->recipe.value("layers").toArray(); if (layers.isEmpty()) layers = QJsonArray{"incidents"};
        for (const QJsonValue& l : layers) reqs << Req{base + l.toString(), l.toString(), {}, {}, {}};
    } else {
        QJsonArray schools = g->recipe.value("schools").toArray(); if (schools.isEmpty()) schools = localspots::defaultSchools();
        QJsonArray sports = g->recipe.value("sports").toArray(); if (sports.isEmpty()) sports = localspots::defaultSports();
        for (const QJsonValue& sv : schools) {
            const QJsonObject school = sv.toObject();
            for (const QJsonValue& pv : sports) {
                const QJsonObject sport = pv.toObject();
                for (const QJsonValue& bv : school.value("bases").toArray()) reqs << Req{bv.toString() + sport.value("slug").toString() + "/schedule/", {}, school.value("name").toString(), school.value("state").toString("VA"), sport.value("label").toString()};
            }
        }
    }
    if (reqs.isEmpty()) { failGen(g, "the recipe names no source to read"); return; }
    const QString season = localspots::activeSeasonLabel(nowIn(g->channel).date());
    // results are kept per request and merged in request order: which page answered first must not change the script
    auto results = std::make_shared<QVector<QJsonArray>>(reqs.size());
    auto answered = std::make_shared<QSet<QString>>(); auto pagesOk = std::make_shared<int>(0);
    g->pending = int(reqs.size());
    for (int i = 0; i < reqs.size(); ++i) {
        const Req r = reqs[i];
        httpGet(r.url, 25000, [this, gid, i, r, kind, season, results, answered, pagesOk](bool ok, const QByteArray& body, const QString&, const QString& err) {
            Gen* g = findGen(gid); if (!g || g->step != "fetch") return;
            if (!ok) qInfo().noquote() << "[Spots]" << kind << "source unavailable:" << r.url << err;
            else if (kind == "cinema") g->local["page"] = localspots::parseCinema(QString::fromUtf8(body));
            else if (kind == "realty") g->local["listings"] = localspots::parseListings(QString::fromUtf8(body));
            else if (kind == "traffic") { (*results)[i] = localspots::parseIncidents(body, r.layer); answered->insert(r.layer); }
            else if (kind == "weather_full") {
                if (r.layer == "forecast") g->local["forecast"] = QJsonDocument::fromJson(body).object();
                else if (r.layer == "alerts") g->local["alerts"] = weatherspots::parseNwsAlerts(body);
                else if (r.layer == "aep") g->local["aep"] = weatherspots::parseAepHydro(QString::fromUtf8(body));
                else if (r.layer == "usgs") g->local["usgs"] = weatherspots::parseUsgsLake(body);
            }
            else if (kind == "markets") { const QJsonObject q = marketspots::parseChart(body, r.spec); if (!q.isEmpty()) { QJsonArray one; one.append(q); (*results)[i] = one; ++*pagesOk; } else qInfo().noquote() << "[Spots] markets: no bars for" << r.spec.value("symbol").toString(); }
            else { (*results)[i] = localspots::parseTeamSchedule(body, r.school, r.sport, r.state, season); ++*pagesOk; }
            if (--g->pending > 0) return;
            if (kind == "traffic" || kind == "sports" || kind == "markets") { QJsonArray all; for (const QJsonArray& a : *results) for (const QJsonValue& v : a) all.append(v); g->local[kind == "traffic" ? "incidents" : kind == "markets" ? "quotes" : "games"] = all; }
            g->local["feedOk"] = answered->contains("incidents"); g->local["pagesOk"] = *pagesOk;      // a dead incident feed must never read as "roads clear"
            buildLocal(g);
        }, 8 * 1024 * 1024);
    }
}

void SpotGenerator::buildLocal(Gen* g) {
    const QString kind = g->recipe.value("source").toString(); const QDateTime now = nowIn(g->channel);
    QJsonObject opts = g->recipe, script;
    if (kind == "cinema") {
        const QJsonObject page = g->local.value("page").toObject();
        if (page.value("movies").toArray().isEmpty()) { failGen(g, "the cinema page could not be read (no movies found)"); return; }
        script = localspots::cinemaScript(page, now, opts);
    } else if (kind == "traffic") {
        const QJsonObject w = weatherFor(g);                       // the market's weather point, else the station's weather settings (default Martinsville)
        if (!opts.contains("lat")) opts["lat"] = w.value("lat"); if (!opts.contains("lon")) opts["lon"] = w.value("lon"); if (!opts.contains("place")) opts["place"] = w.value("place");
        script = localspots::trafficScript(g->local.value("incidents").toArray(), g->local.value("feedOk").toBool(), now, opts);
    } else if (kind == "weather_full") {
        const QJsonObject fc = g->local.value("forecast").toObject();
        if (fc.isEmpty()) { failGen(g, "the forecast could not be read (Open-Meteo)"); return; }
        const QJsonObject w = weatherFor(g); if (!opts.contains("place")) opts["place"] = w.value("place");
        const QJsonObject lakes = lakesFrom(g->local.value("aep").toObject(), g->local.value("usgs").toObject(), !g->previewOnly);
        script = weatherspots::weatherScript(fc, g->local.value("alerts").toObject(), lakes, now, opts);
    } else if (kind == "markets") {
        if (g->local.value("pagesOk").toInt() == 0) { failGen(g, "Yahoo Finance could not be reached (no quote came back)"); return; }
        script = marketspots::marketsScript(g->local.value("quotes").toArray(), now, opts);
    } else if (kind == "sports") {
        if (g->local.value("pagesOk").toInt() == 0) { failGen(g, "MaxPreps could not be reached (no schedule page answered)"); return; }
        script = localspots::sportsScript(g->local.value("games").toArray(), now, opts);
    } else {
        const QJsonArray listings = g->local.value("listings").toArray();
        if (listings.isEmpty()) { failGen(g, "no listings found on the page (the site is down, or its listing data moved)"); return; }
        const QJsonObject pick = localspots::pickListing(listings, m_store->settingJson("spots.realty"), nowMs(), opts);
        if (!g->previewOnly) m_store->setSettingJson("spots.realty", pick.value("state").toObject());       // the daily look: what is new, what is gone
        const QJsonArray fresh = pick.value("newIds").toArray();
        if (!fresh.isEmpty() && !g->previewOnly) log("info", QString("%1 new listing(s) on %2 will air next").arg(fresh.size()).arg(g->recipe.value("site").toString()));
        if (pick.value("listing").isNull()) script["skip"] = QString("no Active listing with %1 or more photos among the %2 on the page").arg(g->recipe.value("minPhotos").toInt(4)).arg(listings.size());
        else {
            const QJsonObject listing = pick.value("listing").toObject(); g->pickMls = listing.value("mls").toString();
            script = localspots::realtyScript(listing, now, opts);
            if (script.contains("scenes")) { QJsonObject lo = script.value("local").toObject(); lo["eligible"] = pick.value("eligible"); lo["newListings"] = fresh; script["local"] = lo; }
        }
    }
    if (script.contains("error")) { failGen(g, script.value("error").toString()); return; }
    if (script.contains("skip")) {
        const QString why = script.value("skip").toString(), label = g->recipe.value("label").toString();
        if (g->recipe.value("skipIfNone").toBool()) { g->skipped = true; QJsonObject o; o["skipped"] = true; o["error"] = label + ": nothing to report this cycle (" + why + ")"; finishGen(g, false, o); return; }
        failGen(g, label + ": " + why); return;
    }
    g->prebuilt = script;
    writeScript(g);
}

/// The feed gives titles and dates; each story page gives the post's picture (og:image), its full text and its place.
void SpotGenerator::enrichStories(Gen* g) {
    const QString gid = g->id; const int n = storyCount(g);
    auto pick = [this, gid, n]() {
        Gen* g = findGen(gid); if (!g || g->step != "fetch") return;
        const QString siteName = g->recipe.value("siteName").toString();
        QJsonArray stories; for (const QJsonValue& v : g->items) { const QJsonObject it = v.toObject(); stories.append(headline::mergeStory(it, it.value("page").toObject(), siteName)); }
        g->stories = headline::pickStories(stories, n, true);
        if (g->stories.isEmpty()) {
            if (g->recipe.value("skipIfNone").toBool()) { g->skipped = true; QJsonObject o; o["skipped"] = true; o["error"] = g->recipe.value("label").toString() + ": nothing to report this cycle"; finishGen(g, false, o); return; }
            failGen(g, "no stories in the feed"); return;
        }
        writeScript(g);
    };
    if (!g->recipe.value("pageImages").toBool(true)) { pick(); return; }
    QList<int> todo; for (int i = 0; i < g->items.size(); ++i) if (g->items[i].toObject().value("link").toString().startsWith("http")) todo << i;
    if (todo.isEmpty()) { pick(); return; }
    g->pending = todo.size();
    for (int i : todo) {
        const QString link = g->items[i].toObject().value("link").toString();
        httpGet(link, 20000, [this, gid, i, link, pick](bool ok, const QByteArray& body, const QString&, const QString& err) {
            Gen* g = findGen(gid); if (!g || g->step != "fetch") return;
            if (ok && i < g->items.size()) { QJsonObject it = g->items[i].toObject(); it["page"] = headline::parseStoryPage(QString::fromUtf8(body), link); g->items[i] = it; }
            else if (!ok) qInfo().noquote() << "[Spots] story page unavailable:" << link << err;
            if (--g->pending <= 0) pick();
        }, 3 * 1024 * 1024);
    }
}

/// Scenes in air order: intro (the rundown), one per story (headline + its first new sentence), outro (the site).
QJsonObject SpotGenerator::storiesScript(Gen* g) const {
    const ChannelProfile p = m_store->channel(g->channel.isEmpty() ? firstChannel() : g->channel);
    const QString siteName = g->recipe.value("siteName").toString(p.brandName.isEmpty() ? QString("the newsroom") : p.brandName);
    const QString site = g->recipe.value("site").toString(), voiceName = headline::spokenName(siteName);
    QJsonArray scenes, lines, sources; QString all;
    auto add = [&](QJsonObject sc) { const QString nar = headline::speakable(scrubNarration(sc.value("narration").toString())); sc["narration"] = nar; sc["words"] = wordCount(nar); all += nar + " "; scenes.append(sc); };
    QJsonObject intro; intro["kind"] = "intro"; intro["narration"] = QString(g->recipe.value("introText").toString("Here are the top stories at this hour from {site}.")).replace("{site}", voiceName); add(intro);
    const int count = g->stories.size(); int k = 0;
    for (const QJsonValue& v : g->stories) {
        const QJsonObject s = v.toObject(); ++k;
        const QString title = s.value("title").toString(), summary = s.value("summary").toString();
        QString nar = title + (title.endsWith('.') || title.endsWith('?') || title.endsWith('!') ? " " : ". ");
        const QString more = headline::firstSentence(summary, 34);
        if (!more.isEmpty() && wordCount(title) + wordCount(more) <= 64) nar += more;
        QJsonObject sc; sc["kind"] = "story"; sc["narration"] = nar; sc["index"] = k; sc["count"] = count; sc["title"] = title; sc["summary"] = summary;
        sc["place"] = s.value("place"); sc["category"] = s.value("category"); sc["image"] = s.value("image"); sc["link"] = s.value("link");
        add(sc);
        if (k > 1 && lines.size() < 3) lines.append(title);
        QJsonObject src; src["title"] = title; src["link"] = s.value("link"); src["image"] = s.value("image"); src["source"] = s.value("source"); sources.append(src);
    }
    QJsonObject outro; outro["kind"] = "outro";
    outro["narration"] = site.isEmpty() ? QString("That's the latest. Stay with %1 for more.").arg(voiceName) : QString("For more on these stories, visit %1.").arg(headline::spokenSite(site, siteName));
    add(outro);
    QJsonObject s;
    s["headline"] = count ? g->stories.first().toObject().value("title").toString() : g->recipe.value("label").toString();
    s["lines"] = lines; s["narration"] = all.simplified(); s["sources"] = sources; s["scenes"] = scenes; s["stories"] = g->stories;
    s["energy"] = g->recipe.value("energy").toInt(7); s["writer"] = "template";
    return s;
}

void SpotGenerator::fetchStoryPhotos(Gen* g) {
    g->step = "photo"; notify(); const QString gid = g->id;
    const QJsonArray scenes = g->script.value("scenes").toArray();
    auto next = [this, gid]() { Gen* g = findGen(gid); if (!g || g->step != "photo") return; renderStoryCards(g); };
    QList<int> todo; for (int i = 0; i < scenes.size(); ++i) if (scenes[i].toObject().value("image").toString().startsWith("http")) todo << i;
    if (todo.isEmpty()) { next(); return; }
    g->pending = todo.size();
    for (int i : todo) {
        const QString url = scenes[i].toObject().value("image").toString();
        httpGet(url, 25000, [this, gid, i, url, next](bool ok, const QByteArray& body, const QString& ctype, const QString& err) {
            Gen* g = findGen(gid); if (!g || g->step != "photo") return;
            const bool png = body.startsWith("\x89PNG"), webp = body.mid(8, 4) == "WEBP";
            if (ok && body.size() >= 2000 && (ctype.startsWith("image/") || body.startsWith("\xFF\xD8") || png || webp)) {
                const QString ext = png || ctype.contains("png") ? "png" : webp || ctype.contains("webp") ? "webp" : "jpg";
                const QString path = QDir(g->workDir).filePath(QString("photo_%1.").arg(i, 2, 10, QChar('0')) + ext);
                QFile f(path);
                if (f.open(QIODevice::WriteOnly)) { f.write(body); f.close(); QJsonArray sc = g->script.value("scenes").toArray(); QJsonObject o = sc[i].toObject(); o["photo"] = path; sc[i] = o; g->script["scenes"] = sc; }
            } else qInfo().noquote() << "[Spots] story photo unavailable:" << url << (ok ? QString("not an image") : err);
            if (--g->pending <= 0) next();
        }, kMaxPhotoBytes, true);
    }
}

/// One spot_card.py run paints every scene: furniture PNG + the still bed ffmpeg moves (photo framed for 16:9, or the gradient).
void SpotGenerator::renderStoryCards(Gen* g) {
    g->step = "card"; notify();
    const ChannelProfile p = m_store->channel(g->channel.isEmpty() ? firstChannel() : g->channel); const QJsonObject brand = config().value("brand").toObject();
    const QDateTime now = nowIn(g->channel);
    QJsonArray scenes = g->script.value("scenes").toArray();
    QString firstPhoto, lastPhoto; QJsonArray rundown;
    for (const QJsonValue& v : scenes) { const QJsonObject sc = v.toObject(); if (sc.value("kind").toString() != "story") continue; rundown.append(sc.value("title")); const QString ph = sc.value("photo").toString(); if (ph.isEmpty()) continue; if (firstPhoto.isEmpty()) firstPhoto = ph; lastPhoto = ph; }
    for (int i = 0; i < scenes.size(); ++i) {
        QJsonObject sc = scenes[i].toObject(); const QString kind = sc.value("kind").toString();
        // a local-spot scene may bring its own backdrop photo, rundown and counter (a lone alert shows no "1 / 1")
        if (kind == "intro") { if (sc.value("photo").toString().isEmpty()) sc["photo"] = firstPhoto; if (!sc.contains("rundown")) sc["rundown"] = rundown; }
        else if (kind == "outro") { if (sc.value("photo").toString().isEmpty()) sc["photo"] = lastPhoto; }
        else if (!sc.contains("counter")) sc["counter"] = QString("%1 / %2").arg(sc.value("index").toInt()).arg(sc.value("count").toInt());
        sc["furniture"] = QDir::toNativeSeparators(QDir(g->workDir).filePath(QString("scene_%1.png").arg(i, 2, 10, QChar('0'))));
        sc["bedBase"] = QDir::toNativeSeparators(QDir(g->workDir).filePath(QString("scene_%1_bed").arg(i, 2, 10, QChar('0'))));
        if (!sc.value("photo").toString().isEmpty()) sc["photo"] = QDir::toNativeSeparators(sc.value("photo").toString());
        scenes[i] = sc;
    }
    QJsonObject spec; spec["width"] = 1920; spec["height"] = 1080; spec["kicker"] = g->recipe.value("kicker").toString("HEADLINE NEWS");
    const QString siteName = g->recipe.value("siteName").toString();
    // the strap names the station; a channel without call letters or a brand shows the news site rather than its channel id
    spec["brand"] = !p.callLetters.isEmpty() ? p.callLetters : !p.brandName.isEmpty() ? p.brandName : !siteName.isEmpty() ? siteName : p.name;
    // a recipe whose siteName credits its DATA (VDOT 511, Yahoo Finance) names the station here so the strap never reads as the data source's own report
    if (!g->recipe.value("brand").toString().isEmpty() && p.callLetters.isEmpty() && p.brandName.isEmpty()) spec["brand"] = g->recipe.value("brand").toString();
    spec["brandName"] = p.brandName; spec["series"] = g->recipe.value("series").toString();
    spec["site"] = g->recipe.value("site").toString(); spec["siteName"] = siteName; spec["date"] = QLocale("en_US").toString(now.date(), "dddd, MMMM d");
    spec["storyLayout"] = g->recipe.value("storyLayout").toString("box");         // box: the post's picture framed whole; full: it fills the frame
    spec["accent"] = brand.value("accent").toString("#d80018"); spec["ink"] = brand.value("ink").toString("#0b1220"); spec["scenes"] = scenes;
    QFile sf(QDir(g->workDir).filePath("scenes.json")); if (!sf.open(QIODevice::WriteOnly)) { failGen(g, "cannot write scene spec"); return; } sf.write(QJsonDocument(spec).toJson()); sf.close();
    g->script["scenes"] = scenes;
    const QStringList args = {"-I", QDir(m_scriptsDir).filePath("spot_card.py"), "--scenes", QDir::toNativeSeparators(sf.fileName())};
    const QString gid = g->id; const int nScenes = scenes.size();
    // native: PlayPlaySpotRender card --scenes (an animated weather scene answers 64 before writing anything: the script paints the whole spot)
    runStep(g, "card", pythonPath(), args, 90000 + 30000 * int(scenes.size()), [nScenes](int, const QByteArray& out) { const QJsonObject r = toolswitch::lastJsonLine(out); return r.value("ok").toBool() && r.value("scenes").toArray().size() == nScenes; },
            [this, gid](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const QJsonObject r = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        QJsonArray scenes = g->script.value("scenes").toArray(); const QJsonArray made = r.value("scenes").toArray();
        if (code != 0 || !r.value("ok").toBool() || made.size() != scenes.size()) { failGen(g, "scene render failed: " + (r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(300) : r.value("error").toString())); return; }
        for (int i = 0; i < scenes.size(); ++i) {
            const QJsonObject m = made[i].toObject(); QJsonObject sc = scenes[i].toObject();
            if (!QFileInfo(m.value("furniture").toString()).isFile() || !QFileInfo(m.value("bed").toString()).isFile()) { failGen(g, QString("scene %1 render is missing").arg(i)); return; }
            sc["furniture"] = m.value("furniture"); sc["bed"] = m.value("bed"); sc["photoUsed"] = m.value("photo");
            if (!m.value("bedSeq").toString().isEmpty()) { sc["bedSeq"] = m.value("bedSeq"); sc["bedFps"] = m.value("fps"); }      // an animated scene (weather): a looping frame sequence
            scenes[i] = sc;
        }
        g->script["scenes"] = scenes;
        synthStoryVoices(g);
    });
}

/// One spot_tts.py run voices every scene (the engine loads once); each WAV's length sets its scene's length.
void SpotGenerator::synthStoryVoices(Gen* g) {
    g->step = "tts"; notify();
    const QJsonObject tts = m_store->settingJson("spots.tts");
    QJsonArray scenes = g->script.value("scenes").toArray(), items; int words = 0;
    for (int i = 0; i < scenes.size(); ++i) {
        QJsonObject sc = scenes[i].toObject(); const QString wav = QDir::toNativeSeparators(QDir(g->workDir).filePath(QString("voice_%1.wav").arg(i, 2, 10, QChar('0'))));
        sc["wav"] = wav; scenes[i] = sc; words += sc.value("words").toInt();
        QJsonObject it; it["text"] = areamarket::pronounce(sc.value("narration").toString(), g->marketJson); it["out"] = wav; items.append(it);
    }
    g->script["scenes"] = scenes;
    QJsonObject batch; batch["items"] = items;
    QFile bf(QDir(g->workDir).filePath("voices.json")); if (!bf.open(QIODevice::WriteOnly)) { failGen(g, "cannot write narration batch"); return; } bf.write(QJsonDocument(batch).toJson()); bf.close();
    const QString voice = g->voice.isEmpty() ? tts.value("defaultVoice").toString(g->recipe.value("voice").toString("af_heart")) : g->voice;
    QStringList args = {"-I", QDir(m_scriptsDir).filePath("spot_tts.py"), "--batch", QDir::toNativeSeparators(bf.fileName()), "--voice", voice, "--speed", QString::number(tts.value("speed").toDouble(1.0)), "--engine", tts.value("engine").toString("auto")};
    if (!tts.value("kokoroDir").toString().isEmpty()) args << "--kokoro-dir" << QDir::toNativeSeparators(tts.value("kokoroDir").toString());
    const QString gid = g->id;
    runStep(g, "tts", pythonPath(), args, qMax(300000, words * 2500 + 30000 * int(scenes.size())), [](int, const QByteArray& out) { return toolswitch::lastJsonLine(out).value("ok").toBool(); },
            [this, gid](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const QJsonObject r = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        QJsonArray scenes = g->script.value("scenes").toArray(); const QJsonArray got = r.value("items").toArray();
        if (code != 0 || !r.value("ok").toBool() || got.size() != scenes.size()) { failGen(g, "TTS failed: " + (r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(300) : r.value("error").toString())); return; }
        double total = 0;
        for (int i = 0; i < scenes.size(); ++i) { QJsonObject sc = scenes[i].toObject(); const double d = got[i].toObject().value("durationSec").toDouble(); if (d < 0.3) { failGen(g, QString("TTS produced no audio for scene %1").arg(i)); return; } sc["wavSec"] = d; total += d; scenes[i] = sc; }
        g->script["scenes"] = scenes; g->wavSec = total; g->ttsEngine = r.value("engine").toString(); g->script["ttsEngine"] = g->ttsEngine; g->script["voiceUsed"] = r.value("voiceUsed").toString();
        makeMusic(g, [this, gid = g->id]() { if (Gen* g = findGen(gid)) composeStories(g); });
    });
}

void SpotGenerator::composeStories(Gen* g) {
    g->step = "compose"; notify();
    QJsonArray scenes = g->script.value("scenes").toArray(); const int K = scenes.size();
    QVector<double> wav; QStringList kinds; for (const QJsonValue& v : scenes) { wav << v.toObject().value("wavSec").toDouble(); kinds << v.toObject().value("kind").toString(); }
    const headline::SceneTiming t = headline::sceneTimings(wav, kinds, 30.0);
    const double D = double(t.totalFrames) / t.fps; g->durSec = D;
    const QString stamp = nowIn(g->channel).toString("yyyyMMdd_HHmm");
    g->outPath = QFileInfo(QDir(m_outDir).filePath(QString("%1_%2_%3.mp4").arg(g->recipeKey, stamp, g->id.left(5)))).absoluteFilePath();
    const QJsonObject music = musicFor(g);
    QStringList args = {"-y", "-nostdin", "-v", "error"};
    QVector<bool> animated;
    for (const QJsonValue& v : scenes) {
        const QJsonObject sc = v.toObject(); const bool anim = !sc.value("bedSeq").toString().isEmpty(); animated << anim;
        if (anim) args << "-stream_loop" << "-1" << "-framerate" << QString::number(sc.value("bedFps").toInt(15)) << "-i" << sc.value("bedSeq").toString();     // the frame loop, repeated for as long as the voice runs
        else args << "-i" << sc.value("bed").toString();
        args << "-loop" << "1" << "-i" << sc.value("furniture").toString();
    }
    for (const QJsonValue& v : scenes) args << "-i" << v.toObject().value("wav").toString();
    if (!music.isEmpty()) args << "-i" << QDir::toNativeSeparators(music.value("path").toString());
    args << "-filter_complex" << headline::storiesFilter(t, !music.isEmpty(), animated) << "-map" << "[v]" << "-map" << "[a]" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-color_range" << "tv" << "-r" << "30"
         << "-c:a" << "aac" << "-b:a" << "160k" << "-ar" << "48000" << "-ac" << "2" << "-movflags" << "+faststart" << "-t" << QString::number(D, 'f', 3) << QDir::toNativeSeparators(g->outPath);
    // where each scene sits in the finished spot (kept on the library record)
    QJsonArray timeline; int at = 0;
    for (int i = 0; i < K; ++i) { const QJsonObject sc = scenes[i].toObject(); QJsonObject e; e["kind"] = sc.value("kind"); e["title"] = sc.value("title"); e["startSec"] = double(at) / t.fps; at += t.slotFrames[i]; e["endSec"] = double(at) / t.fps; timeline.append(e); }
    g->script["timeline"] = timeline; g->script["music"] = music.isEmpty() ? QString() : music.value("generated").toBool() ? "MIDI: " + g->musicInfo : QFileInfo(music.value("path").toString()).fileName(); g->script["durationSec"] = D;
    const QString gid = g->id;
    const QString outPath = g->outPath;
    runStep(g, "compose", ffmpegPath(), args, int(qMax(600.0, D * 12.0) * 1000), [outPath](int, const QByteArray&) { return QFileInfo(outPath).isFile(); },
            [this, gid](int code, const QByteArray&, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        if (code != 0 || !QFileInfo(g->outPath).isFile()) { failGen(g, "ffmpeg failed: " + QString::fromUtf8(err).right(400).simplified()); return; }
        keepMidi(g);
        ingest(g);
    });
}
/// The composed tune's MIDI file is kept next to the spot (spots/out/music/<spot>.mid): it can be re-rendered or opened in any MIDI tool.
void SpotGenerator::keepMidi(Gen* g) {
    if (g->musicMid.isEmpty() || !QFileInfo::exists(g->musicMid)) return;
    QDir(m_outDir).mkpath("music");
    const QString dst = QDir(m_outDir).filePath("music/" + QFileInfo(g->outPath).completeBaseName() + ".mid");
    QFile::remove(dst); if (QFile::copy(g->musicMid, dst)) g->script["musicMid"] = QDir::toNativeSeparators(dst);
}
void SpotGenerator::writeScript(Gen* g) {
    g->step = "write"; notify();
    const bool stories = storiesMode(g);
    g->script = !g->prebuilt.isEmpty() ? g->prebuilt : stories ? storiesScript(g) : templateScript(g);      // local spots arrive with their scenes already written
    if (g->script.value("narration").toString().size() < 20) { failGen(g, "the script came out empty"); return; }
    auto next = [this, gid = g->id, stories]() {
        Gen* g = findGen(gid); if (!g) return;
        QString narr = scrubNarration(g->script.value("narration").toString());
        if (!stories) narr = capWords(narr, int(g->targetSec * 2.6));         // Headline News caps each story, not the whole read
        g->script["narration"] = narr; g->script["words"] = wordCount(narr);
        g->headline = screenText(g->script.value("headline").toString(), stories ? 110 : 70); g->script["headline"] = g->headline;
        QJsonArray lines; for (const QJsonValue& l : g->script.value("lines").toArray()) { const QString s = screenText(l.toString(), 48); if (!s.isEmpty()) lines.append(s); if (lines.size() >= 4) break; } g->script["lines"] = lines;
        if (g->previewOnly) {
            QJsonObject o = g->script; o["recipe"] = g->recipeKey; o["items"] = g->items.size();
            if (!g->market.isEmpty()) { o["market"] = areamarket::recordMeta(g->marketJson); o["marketContext"] = areamarket::promptContext(g->marketJson, m_store->settingJson("spots.markets")); }
            double est = double(wordCount(narr)) / 2.6;
            if (stories) {   // the scene timing the render would use, with ~2.6 spoken words per second
                QVector<double> w; QStringList kinds; for (const QJsonValue& v : g->script.value("scenes").toArray()) { w << v.toObject().value("words").toInt() / 2.6; kinds << v.toObject().value("kind").toString(); }
                const headline::SceneTiming t = headline::sceneTimings(w, kinds, 30.0); est = double(t.totalFrames) / t.fps;
            }
            o["estimatedSec"] = est; finishGen(g, true, o); return;
        }
        if (stories) fetchStoryPhotos(g); else fetchPhoto(g);
    };
    const QJsonObject llm = m_store->settingJson("spots.llm");
    if (!stories && llm.value("enabled").toBool(false) && !AiModels::apiKey(m_store).isEmpty()) llmScript(g, next); else next();   // the story writer reads each post verbatim
}

/// Deterministic writer: the recipe's sentence templates over the fetched material (no network, no model).
QJsonObject SpotGenerator::templateScript(Gen* g) const {
    const QString kind = g->recipe.value("source").toString("rss");
    if (kind == "weather") return weatherScript(g);
    QJsonObject s; QJsonArray lines, sources; QString headline, narr;
    const QString key = g->recipeKey; const QString brand = m_store->channel(g->channel.isEmpty() ? firstChannel() : g->channel).brandName;
    const QString station = brand.isEmpty() ? "your local station" : brand;
    if (kind == "history") {
        const QDate d = nowIn(g->channel).date(); const QString dayName = QLocale("en_US").toString(d, "MMMM d");
        QList<QJsonObject> evs; for (const QJsonValue& v : g->wiki.value("events").toArray()) { const QJsonObject e = v.toObject(); if (!e.value("text").toString().isEmpty()) evs << e; }
        std::sort(evs.begin(), evs.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("year").toInt() < b.value("year").toInt(); });
        QList<QJsonObject> pick; if (evs.size() <= 3) pick = evs; else { pick << evs[evs.size() / 5] << evs[evs.size() / 2] << evs[evs.size() - 1 - evs.size() / 6]; }
        narr = QString("This day in history, %1. ").arg(dayName);
        for (const QJsonObject& e : pick) { const QString t = scrubNarration(e.value("text").toString()); narr += QString("In %1, %2%3 ").arg(e.value("year").toInt()).arg(t.left(1).toLower() + t.mid(1), t.endsWith('.') ? "" : "."); lines.append(QString("%1: %2").arg(e.value("year").toInt()).arg(t)); QJsonObject src; src["title"] = t.left(120); src["link"] = "https://en.wikipedia.org/wiki/" + QLocale("en_US").toString(d, "MMMM_d"); sources.append(src); }
        narr += QString("That's your look back for %1.").arg(dayName); headline = "This Day in History: " + dayName;
        s["energy"] = g->recipe.value("energy").toInt(4);
    } else if (kind == "text" || (g->items.isEmpty() && !g->data.trimmed().isEmpty())) {
        const QString text = scrubNarration(g->data); const QStringList ss = sentences(text);
        headline = ss.isEmpty() ? g->recipe.value("label").toString() : ss.first(); for (int i = 1; i < ss.size() && lines.size() < 3; ++i) lines.append(ss[i]); narr = text;
    } else if (key == "story" || g->origin == "scatter") {
        const QJsonObject it = g->items.isEmpty() ? QJsonObject() : g->items.first().toObject();
        const QString title = it.value("title").toString(), body = scrubNarration(it.value("text").toString());
        headline = title; narr = body.startsWith(title.left(24), Qt::CaseInsensitive) ? body : (title.endsWith('.') ? title + " " : title + ". ") + body;
        if (!g->data.trimmed().isEmpty()) narr += " " + scrubNarration(g->data);
        for (const QString& sent : sentences(body)) { if (lines.size() >= 3) break; lines.append(sent); }
        QJsonObject src; src["title"] = title; src["link"] = it.value("link"); src["image"] = it.value("image"); sources.append(src); if (!it.value("image").toString().isEmpty()) g->photoUrl = it.value("image").toString();
    } else {
        static const QHash<QString, QPair<QString, QString>> intro = {
            {"headlines", {"Here are the top stories at this hour.", "That's the latest. Stay with %1 for more."}}, {"community", {"In community news.", "That's your community bulletin from %1."}},
            {"events", {"Here's what's coming up around the area.", "Find more events at %1."}}, {"obituaries", {"We pause to remember members of our community.", "Our thoughts are with their families."}},
            {"sports", {"Here's your sports update.", "That's sports from %1."}}, {"roads", {"Road watch for your drive.", "Drive carefully, and we'll keep you posted."}},
            {"economics", {"Here's your money minute.", "That's the business brief from %1."}}, {"tech_review", {"Here's your tech in review, the developments of the last few days that matter to regular people.", "That's tech in review from %1."}}};
        auto io = intro.value(key, {"Here's the latest.", "That's the latest from %1."});
        // a market (or the operator) words the intro / outro; a recipe the table does not know closes with the market's own sign-off
        if (!g->recipe.value("intro").toString().isEmpty()) io.first = g->recipe.value("intro").toString();
        if (!g->recipe.value("outro").toString().isEmpty()) io.second = g->recipe.value("outro").toString();
        else if (!intro.contains(key) && !g->marketJson.isEmpty() && !g->marketJson.value("wording").toObject().value("signOff").toString().isEmpty()) io.second = areamarket::fill(g->marketJson.value("wording").toObject().value("signOff").toString(), g->marketJson);
        io.first = areamarket::fill(io.first, QJsonObject(), station);
        const int maxItems = key == "tech_review" ? 6 : key == "roads" ? 4 : 4;
        narr = io.first + " ";
        int n = 0; bool photoSet = false;
        for (const QJsonValue& v : g->items) {
            const QJsonObject it = v.toObject(); const QString t = scrubNarration(it.value("title").toString()), d = firstSentence(scrubNarration(it.value("text").toString()), 45);
            narr += t + (t.endsWith('.') || t.endsWith('?') || t.endsWith('!') ? " " : ". ") + (d.isEmpty() ? "" : d + (d.endsWith('.') ? " " : ". "));
            if (n == 0) headline = it.value("title").toString(); else if (lines.size() < 3) lines.append(it.value("title").toString());
            QJsonObject src; src["title"] = it.value("title"); src["link"] = it.value("link"); src["source"] = it.value("source"); sources.append(src);
            if (!photoSet && !it.value("image").toString().isEmpty()) { g->photoUrl = it.value("image").toString(); photoSet = true; }
            if (++n >= maxItems) break;
        }
        if (!g->data.trimmed().isEmpty()) narr += scrubNarration(g->data) + " ";
        narr += io.second.contains("%1") ? io.second.arg(station) : areamarket::fill(io.second, QJsonObject(), station);
    }
    s["headline"] = headline; s["lines"] = lines; s["narration"] = narr.simplified(); s["sources"] = sources; if (!s.contains("energy")) s["energy"] = g->recipe.value("energy").toInt(5); s["writer"] = "template";
    return s;
}

/// Open-Meteo JSON → spoken forecast. Relies on: current{temperature_2m, apparent_temperature, weather_code, wind_speed_10m, relative_humidity_2m, is_day},
/// hourly{time[], temperature_2m[], precipitation_probability[], weather_code[]} (local ISO times), daily{time[], weather_code[], temperature_2m_max[], temperature_2m_min[], precipitation_probability_max[], sunrise[], sunset[]}.
QJsonObject SpotGenerator::weatherScript(Gen* g) const {
    const QJsonObject w = g->weather, cur = w.value("current").toObject(), hourly = w.value("hourly").toObject(), daily = w.value("daily").toObject();
    const QString place = weatherFor(g).value("place").toString("the area"); const QString key = g->recipeKey;
    const QDateTime now = nowIn(g->channel); const QString today = now.date().toString(Qt::ISODate);
    auto dayVal = [&](const char* k, int i) { return daily.value(k).toArray().at(i).toDouble(); };
    auto dayStr = [&](const char* k, int i) { return daily.value(k).toArray().at(i).toString(); };
    auto hourAt = [&](const QDate& d, int h, const char* k, double* out) -> bool { const QJsonArray t = hourly.value("time").toArray(); const QString want = d.toString(Qt::ISODate) + "T" + QString("%1:00").arg(h, 2, 10, QChar('0')); for (int i = 0; i < t.size(); ++i) if (t[i].toString() == want) { *out = hourly.value(k).toArray().at(i).toDouble(); return true; } return false; };
    auto precipWord = [&](int code) { return wmoSnow(code) ? QString("snow") : QString("rain"); };
    const int days = daily.value("time").toArray().size();
    const double tNow = cur.value("temperature_2m").toDouble(), feels = cur.value("apparent_temperature").toDouble(), wind = cur.value("wind_speed_10m").toDouble(), hum = cur.value("relative_humidity_2m").toDouble();
    const int codeNow = cur.value("weather_code").toInt(); const bool night = cur.value("is_day").toInt(1) == 0;
    const double hi0 = days > 0 ? dayVal("temperature_2m_max", 0) : tNow, lo0 = days > 0 ? dayVal("temperature_2m_min", 0) : tNow, p0 = days > 0 ? dayVal("precipitation_probability_max", 0) : 0; const int c0 = days > 0 ? int(dayVal("weather_code", 0)) : codeNow;
    const double hi1 = days > 1 ? dayVal("temperature_2m_max", 1) : hi0, lo1 = days > 1 ? dayVal("temperature_2m_min", 1) : lo0, p1 = days > 1 ? dayVal("precipitation_probability_max", 1) : 0; const int c1 = days > 1 ? int(dayVal("weather_code", 1)) : c0;
    QString narr, headline; QJsonArray lines;
    const QString nowSent = QString("Right now in %1 it's %2 and %3%4. Winds are around %5 miles per hour%6. ").arg(place, degS(tNow), wmoText(codeNow, night), std::fabs(feels - tNow) >= 5 ? QString(", and it feels like %1").arg(degS(feels)) : "", QString::number(qRound(wind)), hum > 0 ? QString(", with humidity near %1 percent").arg(qRound(hum)) : "");
    const QString todaySent = QString("Today's high reaches %1, with a low tonight of %2%3. ").arg(degS(hi0), degS(lo0), p0 >= 15 ? QString(", and a %1 percent chance of %2").arg(qRound(p0)).arg(precipWord(c0)) : QString(", and it stays dry"));
    const QString tomorrowSent = QString("Tomorrow brings %1, with a high of %2 and a low of %3%4. ").arg(wmoText(c1), degS(hi1), degS(lo1), p1 >= 15 ? QString(", and %1 percent odds of %2").arg(qRound(p1)).arg(precipWord(c1)) : "");
    const QString nowLine = QString("Now: %1 %2 - wind %3 mph").arg(degSym(tNow), capFirst(wmoText(codeNow, night)), QString::number(qRound(wind)));
    const QString todayLine = QString("Today: High %1 / Low %2 - %3 %4%").arg(QString::number(qRound(hi0)), QString::number(qRound(lo0)), precipWord(c0), QString::number(qRound(p0)));
    const QString tomorrowLine = QString("Tomorrow: %1 - High %2 / Low %3").arg(capFirst(wmoText(c1)), QString::number(qRound(hi1)), QString::number(qRound(lo1)));
    if (key == "weather_week") {
        narr = QString("Here's your seven day forecast for %1. ").arg(place); double mn = 1e9, mx = -1e9; QString best, worst; double bestP = 1e9, worstP = -1;
        for (int i = 0; i < qMin(days, 7); ++i) {
            const QDate d = QDate::fromString(dayStr("time", i), Qt::ISODate); const QString name = i == 0 ? "Today" : i == 1 ? "Tomorrow" : QLocale("en_US").toString(d, "dddd");
            const double hi = dayVal("temperature_2m_max", i), lo = dayVal("temperature_2m_min", i), p = dayVal("precipitation_probability_max", i); const int c = int(dayVal("weather_code", i));
            narr += QString("%1, %2 with a high of %3 and a low of %4%5. ").arg(name, wmoText(c), degS(hi), degS(lo), p >= 20 ? QString(", %1 percent chance of %2").arg(qRound(p)).arg(precipWord(c)) : "");
            if (lines.size() < 4 && i >= 1) lines.append(QString("%1: %2 %3/%4").arg(QLocale("en_US").toString(d, "ddd"), capFirst(wmoText(c)), QString::number(qRound(hi)), QString::number(qRound(lo))));
            mn = qMin(mn, lo); mx = qMax(mx, hi); if (p < bestP) { bestP = p; best = name; } if (p > worstP) { worstP = p; worst = name; }
        }
        if (worstP >= 40 && best != worst) narr += QString("%1 looks like the best bet for outdoor plans, and %2 is the day to watch. ").arg(best, worst);
        headline = QString("7-Day: highs %1 to %2").arg(qRound(mn)).arg(qRound(mx)); lines.prepend(QString("Today: %1 - H %2 / L %3").arg(capFirst(wmoText(c0)), QString::number(qRound(hi0)), QString::number(qRound(lo0))));
    } else if (key == "weather_am") {
        double lo = 1e9, t8 = 0, t12 = 0, t17 = 0, p17 = 0; for (int h = 0; h <= 6; ++h) { double t; if (hourAt(now.date(), h, "temperature_2m", &t)) lo = qMin(lo, t); } if (lo > 1e8) lo = lo0;
        const bool h8 = hourAt(now.date(), 8, "temperature_2m", &t8), h12 = hourAt(now.date(), 12, "temperature_2m", &t12), h17 = hourAt(now.date(), 17, "temperature_2m", &t17); hourAt(now.date(), 17, "precipitation_probability", &p17);
        narr = QString("Good morning from %1. The overnight dipped to %2. ").arg(place, degS(lo)) + nowSent + QString("For the morning commute expect %1%2%3, with a high this afternoon of %4%5. ").arg(h8 ? QString("around %1").arg(degS(t8)) : "cool conditions", h12 ? QString(", %1 by lunchtime").arg(degS(t12)) : "", h17 ? QString(" and %1 for the drive home").arg(degS(t17)) : "", degS(hi0), p17 >= 20 ? QString(", and %1 percent odds of %2 by this evening").arg(qRound(p17)).arg(precipWord(c0)) : "") + tomorrowSent;
        headline = QString("Morning: %1, high %2").arg(capFirst(wmoText(c0)), QString::number(qRound(hi0))); lines.append(nowLine); lines.append(todayLine); lines.append(tomorrowLine);
    } else if (key == "weather_pm") {
        double t22 = 0; int c22 = c0; { double c; if (hourAt(now.date(), 22, "weather_code", &c)) c22 = int(c); hourAt(now.date(), 22, "temperature_2m", &t22); }
        narr = QString("Good evening from %1. Today topped out at %2 with %3. ").arg(place, degS(hi0), wmoText(c0)) + nowSent + QString("Tonight, %1 with a low of %2. ").arg(wmoText(c22, true), degS(lo0)) + tomorrowSent + QString("Have a good night.");
        headline = QString("Tonight: %1, low %2").arg(capFirst(wmoText(c22, true)), QString::number(qRound(lo0))); lines.append(nowLine); lines.append(QString("Tonight: %1 - Low %2").arg(capFirst(wmoText(c22, true)), QString::number(qRound(lo0)))); lines.append(tomorrowLine);
    } else if (key == "almanac") {
        double illum = 0; const QString phase = moonPhase(&illum); const QString rise = days > 1 ? clock12(dayStr("sunrise", 1)) : "", set = days > 0 ? clock12(dayStr("sunset", 0)) : "";
        narr = QString("Here's your almanac for %1. Tonight brings %2, about %3 percent illuminated. ").arg(place, phase, QString::number(qRound(illum))) + (set.isEmpty() ? "" : QString("Sunset this evening is at %1").arg(set)) + (rise.isEmpty() ? ". " : QString(", and sunrise tomorrow at %1. ").arg(rise))
             + (codeNow <= 1 ? QString("Skies are %1, so it's a fine night for stargazing. ").arg(wmoText(codeNow, true)) : QString("With %1 overhead, stargazing will be a challenge tonight. ").arg(wmoText(codeNow, true))) + QString("The low tonight is %1.").arg(degS(lo0));
        headline = capFirst(phase).replace("A ", "").replace("a ", "") + QString(", %1% lit").arg(qRound(illum)); lines.append("Moon: " + phase.mid(phase.indexOf(' ') + 1) + QString(" - %1%").arg(qRound(illum))); if (!set.isEmpty()) lines.append("Sunset " + set + (rise.isEmpty() ? "" : " - Sunrise " + rise)); lines.append(QString("Tonight: %1 - Low %2").arg(capFirst(wmoText(codeNow, true)), QString::number(qRound(lo0))));
    } else {
        narr = QString("Here's your %1 weather. ").arg(place) + nowSent + todaySent + tomorrowSent + g->recipe.value("signOff").toString("Stay with us for updates.");
        headline = QString("%1, high %2").arg(capFirst(wmoText(c0)), QString::number(qRound(hi0))); lines.append(nowLine); lines.append(todayLine); lines.append(tomorrowLine);
    }
    if (key == "weather_week" || key == "weather_am" || key == "weather_pm" || key == "almanac") { const QString so = g->recipe.value("signOff").toString(); if (!so.isEmpty()) narr += " " + so; }      // the market's weather sign-off (the plain forecast carries it above)
    QJsonObject s; s["headline"] = headline; s["lines"] = lines; s["narration"] = narr.simplified(); s["energy"] = g->recipe.value("energy").toInt(5); s["writer"] = "template";
    QJsonArray sources; QJsonObject src; src["title"] = "Open-Meteo forecast"; src["link"] = "https://open-meteo.com/"; sources.append(src); s["sources"] = sources;
    return s;
}

/// Optional LLM writer (OpenRouter-compatible chat completions). Any failure keeps the template script.
void SpotGenerator::llmScript(Gen* g, std::function<void()> next) {
    const QJsonObject llm = m_store->settingJson("spots.llm"); const QString gid = g->id;
    QString material; if (!g->weather.isEmpty()) material = QString::fromUtf8(QJsonDocument(g->weather).toJson(QJsonDocument::Compact)).left(6000);
    else if (!g->wiki.isEmpty()) { for (const QJsonValue& v : g->wiki.value("events").toArray()) { const QJsonObject e = v.toObject(); material += QString("- %1: %2\n").arg(e.value("year").toInt()).arg(e.value("text").toString()); if (material.size() > 7000) break; } }
    else { for (const QJsonValue& v : g->items) { const QJsonObject it = v.toObject(); material += "- " + it.value("title").toString() + " - " + it.value("text").toString().left(400) + "\n"; } }
    if (!g->data.trimmed().isEmpty()) material += "\nOPERATOR NOTES / MATERIAL:\n" + g->data.left(6000);
    if (!g->notes.isEmpty()) material += "\nEDITOR'S NOTES (apply them to the copy; facts still come only from the material above):\n" + g->notes;
    const int words = int(g->targetSec * 2.4);
    const QString prompt = QString("You are a broadcast writer at a local TV station. %1\n\nReturn ONLY one JSON object: {\"headline\": \"one short on-screen headline, <= 48 chars\", \"lines\": [\"2 to 3 SHORT on-screen bullet lines, <= 40 chars each\"], \"narration\": \"the full spoken script, about %2 words\", \"energy\": 5}\n"
                                   "Rules: the narration is read aloud by a text-to-speech anchor, so spell numbers as words, no symbols, no URLs, no markdown, no stage directions, plain flowing sentences. Use ONLY the material; never invent facts. No commentary outside the JSON.\n\nMaterial:\n%3")
                               .arg(g->recipe.value("prompt").toString() + (g->marketJson.isEmpty() ? QString() : "\n\n" + areamarket::promptContext(g->marketJson, m_store->settingJson("spots.markets"))), QString::number(words), material);       // one atomic arg(): operator prompt / feed text may contain "%2"
    QJsonObject body; body["model"] = AiModels::model(m_store, llm.value("model").toString()); body["max_tokens"] = qMax(600, words * 3); body["temperature"] = 0.6;
    QJsonObject msg; msg["role"] = "user"; msg["content"] = prompt; body["messages"] = QJsonArray{msg};
    const QString url = AiModels::baseUrl(m_store) + "/chat/completions";
    httpPost(url, body, {"Authorization: Bearer " + AiModels::apiKey(m_store), "HTTP-Referer: https://playplay.local", "X-Title: PLAY PLAY spots"}, 90000, [this, gid, next](bool ok, const QByteArray& resp, const QString&, const QString& err) {
        Gen* g = findGen(gid); if (!g) return;
        if (ok) {
            const QString content = QJsonDocument::fromJson(resp).object().value("choices").toArray().at(0).toObject().value("message").toObject().value("content").toString();
            const int a = content.indexOf('{'), b = content.lastIndexOf('}');
            const QJsonObject d = a >= 0 && b > a ? QJsonDocument::fromJson(content.mid(a, b - a + 1).toUtf8()).object() : QJsonObject();
            if (!d.value("narration").toString().trimmed().isEmpty()) { QJsonObject s = g->script; s["headline"] = d.value("headline").toString(g->script.value("headline").toString()); s["lines"] = d.value("lines").toArray().isEmpty() ? g->script.value("lines") : d.value("lines"); s["narration"] = d.value("narration").toString(); if (d.value("energy").toInt() > 0) s["energy"] = qBound(1, d.value("energy").toInt(), 10); s["writer"] = "llm"; g->script = s; }
            else log("warning", g->recipeKey + ": LLM returned no usable script; using the template");
        } else log("warning", g->recipeKey + ": LLM call failed (" + err + "); using the template");
        next();
    });
}

void SpotGenerator::fetchPhoto(Gen* g) {
    g->step = "photo"; notify(); const QString gid = g->id;
    auto fallback = [this, gid]() {
        Gen* g = findGen(gid); if (!g) return;
        const QDir bg(backgroundsDir()); for (const QString& n : {g->recipeKey + ".jpg", g->recipeKey + ".jpeg", g->recipeKey + ".png", QString("default.jpg"), QString("default.jpeg"), QString("default.png")}) if (QFileInfo(bg.filePath(n)).isFile()) { g->photoPath = bg.filePath(n); break; }
        renderCard(g);
    };
    if (g->photoUrl.isEmpty() || !g->photoUrl.startsWith("http")) { fallback(); return; }
    httpGet(g->photoUrl, 25000, [this, gid, fallback](bool ok, const QByteArray& body, const QString& ctype, const QString&) {
        Gen* g = findGen(gid); if (!g || g->step != "photo") return;
        if (!ok || body.size() < 2000 || !(ctype.startsWith("image/") || body.startsWith("\xFF\xD8") || body.startsWith("\x89PNG"))) { fallback(); return; }
        const QString ext = ctype.contains("png") || body.startsWith("\x89PNG") ? "png" : ctype.contains("webp") ? "webp" : "jpg";
        const QString p = QDir(g->workDir).filePath("photo." + ext); QFile f(p); if (f.open(QIODevice::WriteOnly)) { f.write(body); f.close(); g->photoPath = p; }
        renderCard(g);
    }, kMaxPhotoBytes, true);
}
void SpotGenerator::renderCard(Gen* g) {
    g->step = "card"; notify();
    const ChannelProfile p = m_store->channel(g->channel.isEmpty() ? firstChannel() : g->channel); const QJsonObject brand = config().value("brand").toObject();
    const QDateTime now = nowIn(g->channel);
    QJsonObject spec; spec["width"] = 1920; spec["height"] = 1080; spec["kicker"] = g->recipe.value("kicker").toString(); spec["headline"] = g->script.value("headline"); spec["lines"] = g->script.value("lines");
    spec["brand"] = p.callLetters.isEmpty() ? (p.brandName.isEmpty() ? p.name : p.brandName) : p.callLetters; spec["brandName"] = p.brandName; spec["series"] = g->recipe.value("series").toString(); spec["footer"] = QLocale("en_US").toString(now, "dddd, MMMM d - h:mm AP");
    spec["accent"] = brand.value("accent").toString("#d80018"); spec["ink"] = brand.value("ink").toString("#0b1220"); spec["photo"] = !g->photoPath.isEmpty();
    QFile sf(QDir(g->workDir).filePath("spec.json")); if (!sf.open(QIODevice::WriteOnly)) { failGen(g, "cannot write card spec"); return; } sf.write(QJsonDocument(spec).toJson()); sf.close();
    g->furniturePath = QDir(g->workDir).filePath("furniture.png"); g->bedPath = g->photoPath.isEmpty() ? QDir(g->workDir).filePath("bed.png") : QString();
    QStringList args = {"-I", QDir(m_scriptsDir).filePath("spot_card.py"), "--spec", QDir::toNativeSeparators(sf.fileName()), "--out", QDir::toNativeSeparators(g->furniturePath)}; if (!g->bedPath.isEmpty()) args << "--bed" << QDir::toNativeSeparators(g->bedPath);
    const QString gid = g->id;
    const QString furniture = g->furniturePath;
    runStep(g, "card", pythonPath(), args, 90000, [furniture](int, const QByteArray& out) { return toolswitch::lastJsonLine(out).value("ok").toBool() && QFileInfo(furniture).isFile(); },
            [this, gid](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const QJsonObject r = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        if (code != 0 || !r.value("ok").toBool() || !QFileInfo(g->furniturePath).isFile()) { failGen(g, "card render failed: " + (r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(300) : r.value("error").toString())); return; }
        synthVoice(g);
    });
}
void SpotGenerator::synthVoice(Gen* g) {
    g->step = "tts"; notify();
    const QJsonObject tts = m_store->settingJson("spots.tts");
    QFile tf(QDir(g->workDir).filePath("narration.txt")); if (!tf.open(QIODevice::WriteOnly)) { failGen(g, "cannot write narration"); return; } tf.write(areamarket::pronounce(g->script.value("narration").toString(), g->marketJson).toUtf8()); tf.close();
    g->wavPath = QDir(g->workDir).filePath("voice.wav");
    const QString voice = g->voice.isEmpty() ? tts.value("defaultVoice").toString(g->recipe.value("voice").toString("af_heart")) : g->voice;
    QStringList args = {"-I", QDir(m_scriptsDir).filePath("spot_tts.py"), "--text-file", QDir::toNativeSeparators(tf.fileName()), "--out", QDir::toNativeSeparators(g->wavPath), "--voice", voice, "--speed", QString::number(tts.value("speed").toDouble(1.0)), "--engine", tts.value("engine").toString("auto")};
    if (!tts.value("kokoroDir").toString().isEmpty()) args << "--kokoro-dir" << QDir::toNativeSeparators(tts.value("kokoroDir").toString());
    const QString gid = g->id; const int words = wordCount(g->script.value("narration").toString());
    runStep(g, "tts", pythonPath(), args, qMax(300000, words * 2500), [](int, const QByteArray& out) { return toolswitch::lastJsonLine(out).value("ok").toBool(); },
            [this, gid](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const QJsonObject r = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        if (code != 0 || !r.value("ok").toBool()) { failGen(g, "TTS failed: " + (r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(300) : r.value("error").toString())); return; }
        g->wavSec = r.value("durationSec").toDouble(); g->ttsEngine = r.value("engine").toString(); g->script["ttsEngine"] = g->ttsEngine; g->script["voiceUsed"] = r.value("voiceUsed").toString();
        if (g->wavSec < 1.0) { failGen(g, "TTS produced no audio"); return; }
        makeMusic(g, [this, gid = g->id]() { if (Gen* g = findGen(gid)) compose(g); });
    });
}
QJsonObject SpotGenerator::pickMusic(int energy) const {
    const QDir d(musicDir()); QList<QPair<int, QString>> tracks; static const QRegularExpression rx("energy\\s*(\\d{1,2})", QRegularExpression::CaseInsensitiveOption);
    for (const QString& n : d.entryList({"*.mp3", "*.wav", "*.m4a", "*.flac", "*.ogg"}, QDir::Files)) { const auto m = rx.match(n); tracks << qMakePair(m.hasMatch() ? m.captured(1).toInt() : 5, d.filePath(n)); }
    if (tracks.isEmpty()) return QJsonObject();
    int best = 1000; for (const auto& t : tracks) best = qMin(best, qAbs(t.first - energy));
    QStringList ties; for (const auto& t : tracks) if (qAbs(t.first - energy) == best) ties << t.second;
    QJsonObject o; o["path"] = ties.at(QRandomGenerator::global()->bounded(ties.size())); o["energy"] = energy; return o;
}
/// Mood of the music bed: the recipe's own "mood", else by recipe key, else by energy.
QString SpotGenerator::moodFor(const Gen* g) const {
    const QString own = g->recipe.value("mood").toString().trimmed(); if (!own.isEmpty()) return own;
    static const QHash<QString, QString> byKey = {
        {"weather", "calm"}, {"weather_am", "bright"}, {"weather_pm", "calm"}, {"weather_week", "calm"}, {"weather_full", "calm"}, {"almanac", "night"},
        {"headlines", "news"}, {"story", "news"}, {"community", "warm"}, {"events", "upbeat"}, {"obituaries", "solemn"}, {"sports", "sports"}, {"sports_local", "sports"},
        {"roads", "urgent"}, {"traffic", "urgent"}, {"cinema", "cinematic"}, {"realty", "warm"}, {"history", "cinematic"}, {"economics", "steady"},
        {"markets_close", "steady"}, {"tech_review", "quirky"}};
    if (byKey.contains(g->recipeKey)) return byKey.value(g->recipeKey);
    const int e = g->script.value("energy").toInt(g->recipe.value("energy").toInt(5));
    return e <= 3 ? "calm" : e <= 5 ? "warm" : e <= 7 ? "news" : "upbeat";
}
/// settings spots.music.mode: "auto" (default) = music files in spots/music when there are any, else a composed MIDI-style bed;
/// "generated" = always composed; "files" = files only (the old behaviour); "off" = no music.
QJsonObject SpotGenerator::musicFor(const Gen* g) const {
    const QString mode = m_store->settingJson("spots.music").value("mode").toString("auto");
    if (mode == "off") return QJsonObject();
    if (mode != "files" && !g->musicPath.isEmpty() && QFileInfo::exists(g->musicPath)) { QJsonObject o; o["path"] = g->musicPath; o["generated"] = true; return o; }
    if (mode == "generated") return QJsonObject();
    return pickMusic(g->script.value("energy").toInt(g->recipe.value("energy").toInt(5)));
}
/// Composes this spot's music bed (scripts/spot_music.py: a General MIDI file rendered by a built-in synthesizer, fitted to the spot's mood,
/// energy and length; the same headline on the same day gives the same tune). Any failure continues without music.
void SpotGenerator::makeMusic(Gen* g, std::function<void()> next) {
    const QJsonObject mc = m_store->settingJson("spots.music"); const QString mode = mc.value("mode").toString("auto");
    const int energy = g->script.value("energy").toInt(g->recipe.value("energy").toInt(5));
    if (mode == "off" || mode == "files" || (mode == "auto" && !pickMusic(energy).isEmpty())) { next(); return; }
    // the length compose will give the spot, plus a margin (the mixer trims it)
    double D;
    const QJsonArray scenes = g->script.value("scenes").toArray();
    if (!scenes.isEmpty()) {
        QVector<double> wav; QStringList kinds; for (const QJsonValue& v : scenes) { wav << v.toObject().value("wavSec").toDouble(); kinds << v.toObject().value("kind").toString(); }
        const headline::SceneTiming t = headline::sceneTimings(wav, kinds, 30.0); D = double(t.totalFrames) / t.fps;
    } else D = qMax(g->origin == "scatter" ? qMax(8.0, scatterCfg().value("minSec").toDouble(30)) : 10.0, g->wavSec + 0.8);
    g->step = "music"; notify();
    g->musicPath = QDir::toNativeSeparators(QDir(g->workDir).filePath("music.wav")); g->musicMid = QDir::toNativeSeparators(QDir(g->workDir).filePath("music.mid"));
    const quint32 seed = quint32(qHash(g->recipeKey + "|" + g->headline + "|" + QString::number(nowMs() / 86400000LL)) & 0x7fffffffu);
    const QString mood = moodFor(g);
    const QStringList args = {"-I", QDir(m_scriptsDir).filePath("spot_music.py"), "--out", g->musicPath, "--mid", g->musicMid, "--seconds", QString::number(D + 1.5, 'f', 2),
                              "--mood", mood, "--energy", QString::number(energy), "--seed", QString::number(seed)};
    const QString gid = g->id;
    const QString musicPath = g->musicPath;
    runStep(g, "music", pythonPath(), args, 90000, [musicPath](int, const QByteArray& out) { return toolswitch::lastJsonLine(out).value("ok").toBool() && QFileInfo(musicPath).isFile(); },
            [this, gid, next, mood](int code, const QByteArray& out, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        const QJsonObject r = QJsonDocument::fromJson(out.trimmed().split('\n').last()).object();
        if (code != 0 || !r.value("ok").toBool() || !QFileInfo(g->musicPath).isFile()) {
            log("warning", QString("%1: music could not be composed (%2); the spot goes without").arg(g->recipeKey, r.value("error").toString().isEmpty() ? QString::fromUtf8(err).right(160).simplified() : r.value("error").toString()));
            g->musicPath.clear(); g->musicMid.clear();
        } else g->musicInfo = QString("%1 bpm, %2 (%3)").arg(r.value("bpm").toInt()).arg(r.value("key").toString(), r.value("mood").toString(mood));
        next();
    });
}
void SpotGenerator::compose(Gen* g) {
    g->step = "compose"; notify();
    const double minSec = g->origin == "scatter" ? qMax(8.0, scatterCfg().value("minSec").toDouble(30)) : 10.0;
    const double D = qMax(minSec, g->wavSec + 0.8); g->durSec = D; const int frames = int(std::ceil(D * 30.0));
    const QString stamp = nowIn(g->channel).toString("yyyyMMdd_HHmm");
    g->outPath = QFileInfo(QDir(m_outDir).filePath(QString("%1_%2_%3.mp4").arg(g->recipeKey, stamp, g->id.left(5)))).absoluteFilePath();
    const QString bed = g->photoPath.isEmpty() ? g->bedPath : g->photoPath;
    const QJsonObject music = musicFor(g);
    const QString dS = QString::number(D, 'f', 2), foS = QString::number(qMax(0.0, D - 0.6), 'f', 2);
    QString fc = QString("[0:v]scale=2880:1620:force_original_aspect_ratio=increase:out_range=tv,crop=2880:1620,zoompan=z='min(1+0.00035*on,1.22)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=%1:s=1920x1080:fps=30,format=yuv420p[bg];[bg][1:v]overlay=0:0:format=auto,fade=t=in:d=0.4,fade=t=out:st=%2:d=0.6,format=yuv420p[v];").arg(frames).arg(foS);
    QStringList args = {"-y", "-nostdin", "-v", "error", "-i", QDir::toNativeSeparators(bed), "-loop", "1", "-i", QDir::toNativeSeparators(g->furniturePath), "-i", QDir::toNativeSeparators(g->wavPath)};
    if (!music.isEmpty()) {
        args << "-i" << QDir::toNativeSeparators(music.value("path").toString());
        fc += QString("[2:a]aformat=sample_rates=48000:channel_layouts=stereo,apad,atrim=0:%1,asetpts=PTS-STARTPTS,asplit[n1][n2];[3:a]aformat=sample_rates=48000:channel_layouts=stereo,aloop=loop=-1:size=2000000000,volume=0.16,atrim=0:%1,asetpts=PTS-STARTPTS,afade=t=in:d=0.8,afade=t=out:st=%2:d=1.6[bed];[bed][n1]sidechaincompress=threshold=0.03:ratio=8:attack=40:release=600:level_sc=1[duck];[duck][n2]amix=inputs=2:duration=first:dropout_transition=0:normalize=0[a]").arg(dS, QString::number(qMax(0.0, D - 1.6), 'f', 2));
    } else fc += QString("[2:a]aformat=sample_rates=48000:channel_layouts=stereo,apad,atrim=0:%1,asetpts=PTS-STARTPTS[a]").arg(dS);
    args << "-filter_complex" << fc << "-map" << "[v]" << "-map" << "[a]" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "20" << "-pix_fmt" << "yuv420p" << "-color_range" << "tv" << "-r" << "30" << "-c:a" << "aac" << "-b:a" << "160k" << "-ar" << "48000" << "-ac" << "2" << "-movflags" << "+faststart" << "-t" << dS << QDir::toNativeSeparators(g->outPath);
    g->script["music"] = music.isEmpty() ? QString() : music.value("generated").toBool() ? "MIDI: " + g->musicInfo : QFileInfo(music.value("path").toString()).fileName(); g->script["durationSec"] = D;
    const QString gid = g->id;
    const QString outPath = g->outPath;
    runStep(g, "compose", ffmpegPath(), args, int(qMax(300.0, D * 8.0) * 1000), [outPath](int, const QByteArray&) { return QFileInfo(outPath).isFile(); },
            [this, gid](int code, const QByteArray&, const QByteArray& err) {
        Gen* g = findGen(gid); if (!g) return;
        if (code != 0 || !QFileInfo(g->outPath).isFile()) { failGen(g, "ffmpeg failed: " + QString::fromUtf8(err).right(400).simplified()); return; }
        keepMidi(g);
        ingest(g);
    });
}
/// Channel scope for the library record. A job on "(all channels)" makes unscoped spots (its markers may sit on any channel); a job on one channel
/// scopes them to that channel plus every channel one of its placements targets, so a marker on a second channel can still resolve the spot.
QStringList SpotGenerator::allowedChannelsFor(const Gen* g) const {
    QStringList out;
    if (g->origin == "job" && !g->jobId.isEmpty()) {
        for (const QJsonValue& v : loadJobs()) {
            const QJsonObject j = v.toObject(); if (j.value("id").toString() != g->jobId) continue;
            const QString jc = j.value("channel").toString(); if (jc.isEmpty()) return out;
            out << jc;
            for (const QJsonValue& pv : j.value("placements").toArray()) { const QString c = pv.toObject().value("channel").toString(); if (!c.isEmpty() && !out.contains(c)) out << c; }
            return out;
        }
    }
    if (!g->channel.isEmpty()) out << g->channel;
    return out;
}
void SpotGenerator::ingest(Gen* g) {
    g->step = "ingest"; notify();
    if (!m_ingest) { failGen(g, "no ingest hook"); return; }
    const QString type = g->recipe.value("type").toString("news"); const qint64 now = nowMs();
    QJsonObject meta; meta["category"] = mediaCategoryOf(g->recipe); meta["subcategory"] = g->recipe.value("subcategory").toString("News"); meta["series"] = g->recipe.value("series").toString(); meta["perishable"] = true; meta["status"] = "approved";
    meta["title"] = (g->recipe.value("label").toString() + " \u2014 " + g->headline).left(140); meta["availableEndMs"] = double(now + qint64(g->ttlMin * 60000.0));
    const QStringList allowed = allowedChannelsFor(g); if (!allowed.isEmpty()) { QJsonArray ac; for (const QString& c : allowed) ac.append(c); meta["allowedChannels"] = ac; }
    // The tags are what pins a GEN marker to ITS job's spot (and a scatter marker to the scatter stories): every recipe of one
    // subcategory ("News", "Weather") shares category/subcategory, so those alone would let any newer spot air in the wrong marker.
    QJsonArray tags; if (!g->jobId.isEmpty()) tags.append(genTag(g->jobId)); if (g->origin == "scatter") tags.append(kScatterTag); if (!tags.isEmpty()) meta["tags"] = tags;
    QJsonObject ag; ag["jobId"] = g->jobId; ag["recipe"] = g->recipeKey; ag["genId"] = g->id; ag["generatedAt"] = double(now); ag["headline"] = g->headline; ag["script"] = g->script.value("narration"); ag["lines"] = g->script.value("lines"); ag["sources"] = g->script.value("sources"); ag["writer"] = g->script.value("writer"); ag["ttsEngine"] = g->ttsEngine; ag["voice"] = g->script.value("voiceUsed"); ag["music"] = g->script.value("music"); if (g->script.contains("musicMid")) ag["musicMid"] = g->script.value("musicMid"); ag["durationSec"] = g->durSec; ag["ttlMin"] = g->ttlMin; ag["origin"] = g->origin; ag["genNum"] = g->genNum;
    if (!g->photoUrl.isEmpty()) ag["photo"] = g->photoUrl; if (g->origin == "scatter") { ag["scatter"] = true; ag["storyUrl"] = g->storyUrl; meta["newsScatter"] = true; }
    if (!g->notes.isEmpty()) ag["notes"] = g->notes;
    if (!g->engines.isEmpty()) ag["engines"] = g->engines;   // which program made each step (native tool / script / fallback)
    if (!g->market.isEmpty()) {      // the record says which market it was made for; the tag lets a query block or a library filter pick by market
        ag["market"] = g->market; meta["market"] = areamarket::recordMeta(g->marketJson);
        QJsonArray mt = meta.value("tags").toArray(); mt.append("market:" + g->market); meta["tags"] = mt;
    }
    if (g->script.contains("stories")) { ag["render"] = "stories"; ag["stories"] = g->script.value("stories"); ag["timeline"] = g->script.value("timeline"); }   // Headline News: what aired, in order
    if (g->script.contains("local")) { ag["local"] = g->script.value("local"); ag["package"] = g->script.value("package"); }                                    // local spots: what the page said (listing, counts)
    // realty: the home is "featured" once its spot exists, so a render that fails does not skip it in the rotation
    if (!g->pickMls.isEmpty() && !g->previewOnly) m_store->setSettingJson("spots.realty", localspots::markFeatured(m_store->settingJson("spots.realty"), g->pickMls, now));
    for (auto it = g->extraMeta.begin(); it != g->extraMeta.end(); ++it) if (it.key() != "autogen") meta[it.key()] = it.value();   // caller fields win (spot studio, revisions)
    if (g->extraMeta.value("autogen").isObject()) { const QJsonObject x = g->extraMeta.value("autogen").toObject(); for (auto it = x.begin(); it != x.end(); ++it) ag[it.key()] = it.value(); }
    meta["autogen"] = ag;
    m_ingest(QDir::toNativeSeparators(g->outPath), type, meta);
    m_current = nullptr;                                     // the probe runs in the ingest queue; the next spot can start
    const QString gid = g->id;
    // The ingest queue is one shared serial FIFO (a folder scan can sit ahead of the spot), so the wait is generous; a record that still
    // lands after the generation gave up is adopted by onMediaIngested (adoptLateSpot) instead of being lost to the job.
    QTimer::singleShot(kIngestWaitMs, this, [this, gid]() { Gen* g = findGen(gid); if (!g || g->step != "ingest") return; bool f = false; const MediaRecord m = m_store->mediaByPath(QDir::toNativeSeparators(g->outPath), &f); if (f) { QJsonObject o; o["mediaId"] = m.id; finishGen(g, true, o); } else failGen(g, "ingest did not complete (ffprobe?)"); });
    QTimer::singleShot(0, this, &SpotGenerator::pump);
}
void SpotGenerator::onMediaIngested(const QString& mediaId, const QString& origin) {
    if (origin != "autogen") return;
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f); if (!f) return;
    for (Gen* g : m_gens) if (g->step == "ingest" && QDir::toNativeSeparators(g->outPath).compare(m.path, Qt::CaseInsensitive) == 0) { QJsonObject o; o["mediaId"] = mediaId; finishGen(g, true, o); return; }
    adoptLateSpot(m);                                        // nobody is waiting for it any more: the job still has to learn about its new spot
}
/// A spot whose ingest outlasted the generation's wait: finishGen already recorded "ingest did not complete" and kept the prior spot
/// alive. When the record finally lands, give the job what a normal finish would have: lastMediaId, the prior window closed, the markers.
void SpotGenerator::adoptLateSpot(const MediaRecord& m) {
    const QJsonObject ag = m.extra.value("autogen").toObject(); const QString jid = ag.value("jobId").toString();
    if (jid.isEmpty() || ag.value("scatter").toBool()) return;
    QJsonArray all = loadJobs();
    for (int i = 0; i < all.size(); ++i) {
        QJsonObject j = all[i].toObject(); if (j.value("id").toString() != jid) continue;
        const QString prior = j.value("lastMediaId").toString(); if (prior == m.id) return;
        const double thisAt = ag.value("generatedAt").toDouble(double(m.addedAtMs));
        bool pf = false; const MediaRecord pm = prior.isEmpty() ? MediaRecord() : m_store->media(prior, &pf);
        if (pf && pm.extra.value("autogen").toObject().value("generatedAt").toDouble(double(pm.addedAtMs)) >= thisAt) return;      // the job already points at something newer
        promoteJobSpot(j, m.id, ag.value("headline").toString(), nowMs());
        all[i] = j; storeJobs(all); applyPlacementsFor(j); notify();
        log("info", QString("late spot adopted for %1: %2").arg(j.value("label").toString(), ag.value("headline").toString()));      // atomic arg(): headline text may hold "%2"
        return;
    }
}
/// The job now points at `mediaId`: newest generation wins the markers and the prior one's window closes (expirePrior).
void SpotGenerator::promoteJobSpot(QJsonObject& j, const QString& mediaId, const QString& headline, qint64 whenMs) {
    const QString prior = j.value("lastMediaId").toString();
    j["lastMediaId"] = mediaId; j["lastError"] = ""; j["lastHeadline"] = headline; j["lastGeneratedMs"] = double(whenMs);
    if (j.value("expirePrior").toBool(true) && !prior.isEmpty() && prior != mediaId) { bool f = false; MediaRecord pm = m_store->media(prior, &f); if (f) { pm.availableEndMs = nowMs(); m_store->saveMedia(pm); } }
}
void SpotGenerator::finishGen(Gen* g, bool ok, const QJsonObject& resultIn) {
    QJsonObject result = resultIn; g->finishedMs = nowMs(); g->step = ok ? "done" : (g->skipped ? "skipped" : "failed"); g->error = ok ? QString() : result.value("error").toString();
    if (g == m_current) { m_current = nullptr; if (m_proc) { m_proc->kill(); } }
    result["genId"] = g->id; result["recipe"] = g->recipeKey; result["jobId"] = g->jobId; result["headline"] = g->headline; result["ok"] = ok; if (!g->previewOnly) { result["path"] = QDir::toNativeSeparators(g->outPath); result["durationSec"] = g->durSec; }
    if (!g->engines.isEmpty()) result["engines"] = g->engines;
    const QString mediaId = result.value("mediaId").toString();
    // job bookkeeping: newest generation wins the markers, the prior one's window closes
    if (!g->jobId.isEmpty() && !g->previewOnly) {
        QJsonArray all = loadJobs();
        for (int i = 0; i < all.size(); ++i) {
            QJsonObject j = all[i].toObject(); if (j.value("id").toString() != g->jobId) continue;
            const QString prior = j.value("lastMediaId").toString();
            if (ok) {
                promoteJobSpot(j, mediaId, g->headline, g->finishedMs);
            } else {
                j["lastError"] = g->skipped ? "" : g->error.left(300);
                if (!prior.isEmpty()) { bool f = false; MediaRecord pm = m_store->media(prior, &f); if (f && !pm.archived) { pm.availableEndMs = qMax(pm.availableEndMs, nowMs() + qint64(ttlForJob(j) * 60000.0)); m_store->saveMedia(pm); } }   // keep-alive: the old spot stays airable until the next cycle
            }
            all[i] = j; storeJobs(all); if (ok) applyPlacementsFor(j);
            break;
        }
    }
    if (g->origin == "scatter") {
        QJsonObject sc = scatterCfg(); QJsonArray seen = sc.value("seen").toArray(); QJsonObject fails = sc.value("fails").toObject();
        if (ok) { seen.append(g->storyUrl); fails.remove(g->storyUrl); }
        else { const int n = fails.value(g->storyUrl).toInt() + 1; if (n >= 3) { seen.append(g->storyUrl); fails.remove(g->storyUrl); } else fails[g->storyUrl] = n; }
        while (seen.size() > 500) seen.removeFirst(); if (fails.size() > 300) { QJsonObject trimmed; int k = 0; for (auto it = fails.begin(); it != fails.end(); ++it) if (++k > 100) trimmed[it.key()] = it.value(); fails = trimmed; }
        sc["seen"] = seen; sc["fails"] = fails; setScatterCfg(sc);
        if (ok && sc.value("enabled").toBool()) m_scatterSoon.start(8000);
    }
    if (ok && !g->previewOnly) log("info", QString("spot generated: %1 \u2014 %2 (%3 s, %4)").arg(g->recipe.value("label").toString(), g->headline, QString::number(g->durSec, 'f', 1), g->ttsEngine));   // atomic: a headline like "up 5%3" must not eat %3
    else if (!ok && !g->skipped && !g->previewOnly) log("warning", QString("spot %1 failed: %2").arg(g->recipeKey, g->error));
    else if (g->skipped) qInfo().noquote() << "[Spots]" << g->error;
    if (!g->previewOnly) { QJsonObject r = g->toJson(); for (auto it = result.begin(); it != result.end(); ++it) r[it.key()] = it.value(); m_recent.append(r); while (m_recent.size() > 30) m_recent.removeFirst(); }
    const QList<Done> dones = g->done; g->done.clear();
    for (const Done& d : dones) if (d) d(ok, result);
    if (!g->workDir.isEmpty() && (ok || g->previewOnly || g->skipped)) QDir(g->workDir).removeRecursively();   // failed renders keep their work dir for inspection
    m_gens.removeAll(g); delete g;
    notify(); QTimer::singleShot(0, this, &SpotGenerator::pump);
}

// ---------------------------------------------------------------------------------------------
// Process + HTTP plumbing (callbacks look the generation up by id: a cancelled one is simply gone)
// ---------------------------------------------------------------------------------------------
void SpotGenerator::runProcess(Gen* g, const QString& program, const QStringList& args, int timeoutMs, ProcCb cb) {
    if (m_proc) { m_proc->kill(); m_proc->deleteLater(); m_proc = nullptr; }
    QProcess* p = new QProcess(this); m_proc = p; p->setWorkingDirectory(g->workDir);
    const QString gid = g->id;
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, p, gid, cb](int code, QProcess::ExitStatus st) {
        m_procTimer.stop(); const QByteArray out = p->readAllStandardOutput(), err = p->readAllStandardError(); if (m_proc == p) m_proc = nullptr; p->deleteLater();
        if (!findGen(gid)) return;
        // -3 = killed by the step's timeout (runStep never retries that one), -1 = crashed / killed otherwise; every call site treats both as failure
        cb(p->property("ppTimedOut").toBool() ? toolswitch::kTimedOut : st == QProcess::NormalExit ? code : -1, out, err);
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, gid, cb, program](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return; m_procTimer.stop(); if (m_proc == p) m_proc = nullptr; p->deleteLater();
        if (!findGen(gid)) return; cb(toolswitch::kFailedToStart, QByteArray(), ("could not start " + program).toUtf8());
    });
    m_procTimer.disconnect(); connect(&m_procTimer, &QTimer::timeout, this, [p]() { qWarning() << "[Spots] process timed out, killing"; p->setProperty("ppTimedOut", true); p->kill(); });
    m_procTimer.start(timeoutMs);
    p->start(program, args);
}
/// Size / type guard for a download: aborts the transfer once more than maxBytes have arrived (the transfer timeout only catches STALLED
/// transfers), and for imageOnly as soon as the headers say the body is not an image. *why carries the reason out to the finished handler.
static void guardDownload(QObject* ctx, QNetworkReply* r, qint64 maxBytes, bool imageOnly, const std::shared_ptr<QString>& why) {
    QObject::connect(r, &QNetworkReply::downloadProgress, ctx, [r, maxBytes, imageOnly, why](qint64 got, qint64 total) {
        if (!why->isEmpty()) return;
        if (maxBytes > 0 && (got > maxBytes || total > maxBytes)) { *why = QString("response larger than %1 MB").arg(maxBytes / (1024 * 1024)); r->abort(); return; }
        if (imageOnly) { const QString ct = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower(); if (!ct.isEmpty() && !ct.startsWith("image/") && !ct.startsWith("application/octet-stream") && !ct.startsWith("binary/")) { *why = "not an image (" + ct + ")"; r->abort(); } }
    });
}
void SpotGenerator::httpGet(const QString& url, int timeoutMs, HttpCb cb, qint64 maxBytes, bool imageOnly) {
    QNetworkRequest req{QUrl(url)}; req.setHeader(QNetworkRequest::UserAgentHeader, "PlayPlay-Spots/1.0 (+playout; contact station)"); req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy); req.setTransferTimeout(timeoutMs);
    req.setRawHeader("Accept", "application/rss+xml, application/atom+xml, application/json, text/xml, image/*, */*");
    QNetworkReply* r = m_nam.get(req);
    const auto why = std::make_shared<QString>(); guardDownload(this, r, maxBytes, imageOnly, why);
    connect(r, &QNetworkReply::finished, this, [r, cb, why]() {
        const bool ok = r->error() == QNetworkReply::NoError && why->isEmpty(); const QByteArray body = ok ? r->readAll() : QByteArray(); const QString ct = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        const QString err = ok ? QString() : (why->isEmpty() ? r->errorString() : *why);
        r->deleteLater(); cb(ok, body, ct, err);
    });
}
void SpotGenerator::httpPost(const QString& url, const QJsonObject& body, const QStringList& headers, int timeoutMs, HttpCb cb) {
    QNetworkRequest req{QUrl(url)}; req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); req.setHeader(QNetworkRequest::UserAgentHeader, "PlayPlay-Spots/1.0"); req.setTransferTimeout(timeoutMs);
    for (const QString& h : headers) { const int c = h.indexOf(':'); if (c > 0) req.setRawHeader(h.left(c).trimmed().toUtf8(), h.mid(c + 1).trimmed().toUtf8()); }
    QNetworkReply* r = m_nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const auto why = std::make_shared<QString>(); guardDownload(this, r, kMaxApiBytes, false, why);
    connect(r, &QNetworkReply::finished, this, [r, cb, why]() {
        const bool ok = r->error() == QNetworkReply::NoError && why->isEmpty(); const QByteArray resp = r->readAll(); const QString ct = r->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        QString err; if (!ok) { err = why->isEmpty() ? r->errorString() : *why; const QString m = QJsonDocument::fromJson(resp).object().value("error").toObject().value("message").toString(); if (!m.isEmpty()) err += ": " + m; }
        r->deleteLater(); cb(ok, resp, ct, err);
    });
}

// ---------------------------------------------------------------------------------------------
// Placements: locked GEN markers (query blocks) in the weekly template + this week's overrides
// ---------------------------------------------------------------------------------------------
QString SpotGenerator::genTitle(const QJsonObject& job, const QJsonObject& recipe) const {
    const QString label = job.value("label").toString().isEmpty() ? recipe.value("label").toString() : job.value("label").toString();
    return QString("GEN %1: %2").arg(job.value("genNum").toInt()).arg(label);
}
SlotItem SpotGenerator::makeMarker(const QJsonObject& job, const QJsonObject& recipe, int atMin, const QString& existingId) const {
    SlotItem it; it.id = existingId.isEmpty() ? "gen-" + job.value("id").toString() + "-" + shortId().left(4) : existingId;
    it.title = genTitle(job, recipe).left(140); it.type = recipe.value("type").toString("news");
    const int target = job.value("targetSec").toInt() > 0 ? job.value("targetSec").toInt() : recipe.value("targetSec").toInt(60);
    // A SOFT locked anchor, never a hard start (PlayPlay OBS lock_at_sec): RunlistBuilder::seatAnchors seats it at the item boundary nearest the
    // minute and cuts nothing. A hard start would truncate the airing program at the marker minute and drop every later soft item of the hour.
    it.durationSec = target; it.locked = true; it.lockAtSec = atMin * 60.0;
    QJsonObject qb; qb["types"] = QJsonArray{"news", "weather"}; qb["targetSec"] = target; qb["prefer"] = "newest"; qb["categories"] = QJsonArray{mediaCategoryOf(recipe)}; qb["subcategories"] = QJsonArray{recipe.value("subcategory").toString("News")}; qb["series"] = QJsonArray(); qb["nthLatest"] = 0; qb["noRepeatMin"] = 0;
    qb["tags"] = QJsonArray{genTag(job.value("id").toString())};      // only THIS job's spots: recipes of one subcategory ("News", "Weather") would otherwise swap spots
    // autoSpot carries identity only: the marker must not change when a new spot is generated (a lastMediaId in here rewrote every marker slot per generation)
    QJsonObject as; as["jobId"] = job.value("id").toString(); as["recipe"] = recipe.value("key").toString(); as["genNum"] = job.value("genNum").toInt();
    it.extra["queryBlock"] = qb; it.lockLabel = QString("GEN %1 %2").arg(job.value("genNum").toInt()).arg(recipe.value("kicker").toString()).left(24); it.extra["autoSpot"] = as;
    return it;
}
/// Where a soft-anchored item sits in the stored list: the item boundary nearest its offset, counting un-anchored items and earlier
/// anchors (PlayPlay OBS locks.index_at_offset; the same rule RunlistBuilder::seatAnchors applies at build time, so the grid reads in air order).
static int seatIndex(const QList<SlotItem>& items, double offsetSec) {
    int best = 0; double bestD = std::fabs(offsetSec), run = 0.0;
    for (int i = 0; i < items.size(); ++i) {
        const SlotItem& it = items[i];
        if (it.anchorSec() >= 0.0 && !it.isHardStart() && it.anchorSec() > offsetSec) continue;       // a later anchor is not seated yet
        run += it.isQuery() && it.durationSec > 0 ? it.durationSec : it.effectiveDurationSec();
        const double d = std::fabs(run - offsetSec); if (d < bestD) { best = i + 1; bestD = d; }
    }
    return best;
}
/// A spot can be pinned: it exists, is not archived / missing and its window is open.
static bool airableAt(const MediaRecord& m, qint64 now) { return !m.archived && !m.extra.value("missing").toBool() && (m.availableEndMs <= 0 || m.availableEndMs > now); }
bool SpotGenerator::airableSpot(const QJsonObject& job, MediaRecord* out) const {
    const QString mid = job.value("lastMediaId").toString(); if (mid.isEmpty()) return false;
    bool f = false; const MediaRecord m = m_store->media(mid, &f); if (!f || !airableAt(m, nowMs())) return false;
    if (out) *out = m; return true;
}
void SpotGenerator::ensureTag(MediaRecord& m, const QString& tag) {
    const QJsonValue v = m.extra.value("tags"); QStringList cur;
    if (v.isString()) cur = v.toString().split(',', Qt::SkipEmptyParts); else for (const QJsonValue& x : v.toArray()) cur << x.toString();
    QJsonArray tags; for (const QString& t : cur) { if (t.trimmed().compare(tag, Qt::CaseInsensitive) == 0) return; tags.append(t.trimmed()); }
    tags.append(tag); m.extra["tags"] = tags; m_store->saveMedia(m);
}
void SpotGenerator::ensureCategory(MediaRecord& m, const QString& category) {
    if (m.extra.value("category").toString() == category) return;
    m.extra["category"] = category; m_store->saveMedia(m);
}
void SpotGenerator::saveSlotQuiet(const HourSlot& s, QSet<QString>& touched) {
    { QSignalBlocker block(m_store); m_store->saveSlot(s); }          // the store emits slotChanged per save (a full-week placement is 168): announce once instead
    touched.insert(s.channelId);
}
void SpotGenerator::announceSlots(const QSet<QString>& touched) {
    for (const QString& ch : touched) { QJsonObject p; p["channelId"] = ch; p["slotId"] = QString(); p["bulk"] = true; emit event("schedule", p); }
}
QJsonObject SpotGenerator::applyPlacements(const QString& jobId) { bool f = false; const QJsonObject j = job(jobId, &f); if (!f) { QJsonObject e; e["error"] = "no such job"; e["status"] = 404; return e; } return applyPlacementsFor(j); }
QJsonObject SpotGenerator::applyPlacementsFor(const QJsonObject& job, bool remove) {
    const QString jid = job.value("id").toString(); const QJsonObject r = recipe(job.value("recipe").toString());
    QJsonObject res; res["jobId"] = jid; res["placed"] = 0; res["updated"] = 0; res["removed"] = 0;
    // Nothing airable behind the pin (a never-run or seeded job, or one whose window closed): leave the grid untouched. A marker with no spot
    // resolves to nothing, and a marker-only hour never autofills (PlayPlay OBS apply_placements: "fresh spot not ready - grid untouched").
    // Markers appear with the job's first successful generation (finishGen -> here) and are removed again when the job is removed or de-configured.
    MediaRecord spot; const bool haveSpot = !remove && airableSpot(job, &spot);
    if (!remove && !haveSpot && !job.value("placements").toArray().isEmpty()) { res["note"] = "no airable spot yet: grid untouched"; return res; }
    if (haveSpot) { ensureTag(spot, genTag(jid)); ensureCategory(spot, mediaCategoryOf(r)); }   // a spot generated before the job tag / its recipe's category existed joins its marker's query
    QHash<QString, QHash<QPair<int, int>, int>> wanted;                     // channel -> (dow, hour) -> atMin
    if (!remove) for (const QJsonValue& pv : job.value("placements").toArray()) {          // a disabled job keeps its markers (state "off": its hours air whatever was last generated)
        const QJsonObject p = pv.toObject(); const QString ch = p.value("channel").toString().isEmpty() ? defaultChannelFor(job) : p.value("channel").toString();
        QList<int> hours; if (p.value("hours").isString()) for (int h = 0; h < 24; ++h) hours << h; else for (const QJsonValue& h : p.value("hours").toArray()) hours << h.toInt();
        QList<int> dows; for (const QJsonValue& d : p.value("dows").toArray()) dows << d.toInt(); if (dows.isEmpty()) dows = {0, 1, 2, 3, 4, 5, 6};
        for (int d : dows) for (int h : hours) wanted[ch][qMakePair(d, h)] = p.value("atMin").toInt(0);
    }
    int placed = 0, updated = 0, removed = 0, heldBack = 0; QSet<QString> touched;
    // holdUntilMs > 0: the slot is the hour ON AIR. Adding, moving or removing a marker there re-indexes the airing runlist (the executor then sees a
    // different item on air and replaces it), so that hour is left alone and re-applied by tick() once it is over.
    auto reconcile = [&](HourSlot& s, bool want, int atMin, qint64 holdUntilMs) -> bool {
        int idx = -1, dup = 0; QString keepId; QList<SlotItem> items;
        for (const SlotItem& it : s.items) { if (it.extra.value("autoSpot").toObject().value("jobId").toString() == jid) { if (idx < 0) { idx = items.size(); keepId = it.id; items.append(it); } else ++dup; continue; } items.append(it); }
        bool changed = dup > 0; int pl = 0, up = 0, rm = dup;
        if (want) {
            const SlotItem m = makeMarker(job, r, atMin, keepId);
            if (idx >= 0) { if (QJsonDocument(items[idx].toJson()) != QJsonDocument(m.toJson())) { items[idx] = m; changed = true; ++up; } }
            else { items.insert(seatIndex(items, m.lockAtSec), m); changed = true; ++pl; }
        } else if (idx >= 0) { items.removeAt(idx); changed = true; ++rm; }
        if (!changed) return false;
        if (holdUntilMs > 0) { ++heldBack; m_deferredJobs.insert(jid); if (m_deferEnforceMs <= 0 || holdUntilMs < m_deferEnforceMs) m_deferEnforceMs = holdUntilMs; return false; }
        placed += pl; updated += up; removed += rm;
        s.items = items; s.showBlock.remove("_override"); saveSlotQuiet(s, touched);
        return true;
    };
    for (const ChannelProfile& p : m_store->channels()) {
        const auto want = wanted.value(p.id);
        const QDateTime nowL = nowIn(p.id); const QDate today = nowL.date(); const int curHour = nowL.time().hour();
        const QPair<int, int> onAirKey = qMakePair(scheduleDow(today), curHour);
        const qint64 holdUntil = QDateTime(today, QTime(curHour, 0), tzFor(p.id)).addSecs(3600).toMSecsSinceEpoch() + 2000;
        QHash<QPair<int, int>, HourSlot> tmpl; for (const HourSlot& s : m_store->templateSlots(p.id)) tmpl[qMakePair(s.dayOfWeek, s.hour)] = s;
        QSet<QPair<int, int>> covered;
        for (auto it = tmpl.begin(); it != tmpl.end(); ++it) { HourSlot s = it.value(); s.channelId = p.id; const bool w = want.contains(it.key()); reconcile(s, w, w ? want.value(it.key()) : 0, it.key() == onAirKey ? holdUntil : 0); covered.insert(it.key()); }
        for (auto it = want.begin(); it != want.end(); ++it) { if (covered.contains(it.key())) continue; HourSlot s; s.channelId = p.id; s.dayOfWeek = it.key().first; s.hour = it.key().second; s.id = HourSlot::templateId(s.dayOfWeek, s.hour); reconcile(s, true, it.value(), it.key() == onAirKey ? holdUntil : 0); }
        // dated overrides of the current week win over the template: keep their markers in step too (never today's past or current hours: they are gone / on air)
        const QDate sunday = today.addDays(-scheduleDow(today));
        for (const HourSlot& eff : m_store->week(p.id, sunday)) {
            if (!eff.showBlock.value("_override").toBool() || eff.date < today || (eff.date == today && eff.hour <= curHour)) continue;
            HourSlot s = eff; s.channelId = p.id; const auto key = qMakePair(scheduleDow(s.date), s.hour); reconcile(s, want.contains(key), want.value(key, 0), 0);
        }
    }
    announceSlots(touched);
    res["placed"] = placed; res["updated"] = updated; res["removed"] = removed; if (heldBack) res["heldBack"] = heldBack; return res;
}
QJsonObject SpotGenerator::enforceMarkers() {
    // The slot-PUT hook asks for this after EVERY saved slot (copy-day = 24 back-to-back requests): one pass ~300 ms after the first covers them all.
    if (!m_enforceSoon.isActive()) m_enforceSoon.start(300);
    QJsonObject o; o["queued"] = true; return o;
}
QJsonObject SpotGenerator::enforceMarkersNow() {
    m_enforceSoon.stop();
    int placed = 0, updated = 0, removed = 0; QSet<QString> ids, touched;
    for (const QJsonValue& v : loadJobs()) {
        const QJsonObject j = v.toObject(); ids.insert(j.value("id").toString());
        if (!j.value("enabled").toBool()) continue;                     // a switched-off job keeps the markers it has but is not re-asserted (PlayPlay OBS enforce_markers: enabled jobs only)
        const QJsonObject r = applyPlacementsFor(j); placed += r.value("placed").toInt(); updated += r.value("updated").toInt(); removed += r.value("removed").toInt();
    }
    // orphans: markers whose job no longer exists (the hour on air waits, like every other placement change)
    for (const ChannelProfile& p : m_store->channels()) {
        const QDateTime nowL = nowIn(p.id); const QDate today = nowL.date(); const int curHour = nowL.time().hour();
        const qint64 holdUntil = QDateTime(today, QTime(curHour, 0), tzFor(p.id)).addSecs(3600).toMSecsSinceEpoch() + 2000;
        for (HourSlot s : m_store->templateSlots(p.id)) {
            QList<SlotItem> keep; int gone = 0; for (const SlotItem& it : s.items) { const QString jid = it.extra.value("autoSpot").toObject().value("jobId").toString(); if (!jid.isEmpty() && !ids.contains(jid)) { ++gone; continue; } keep.append(it); }
            if (!gone) continue;
            if (s.dayOfWeek == scheduleDow(today) && s.hour == curHour) { if (m_deferEnforceMs <= 0 || holdUntil < m_deferEnforceMs) m_deferEnforceMs = holdUntil; continue; }
            removed += gone; s.items = keep; s.channelId = p.id; saveSlotQuiet(s, touched);
        }
    }
    announceSlots(touched);
    QJsonObject o; o["placed"] = placed; o["updated"] = updated; o["removed"] = removed; o["jobs"] = ids.size(); notify(); return o;
}
QJsonArray SpotGenerator::markers() const {
    QJsonArray out;
    auto add = [&](const HourSlot& s, const QString& ch, bool dated) {
        for (const SlotItem& it : s.items) {
            const QJsonObject as = it.extra.value("autoSpot").toObject(); const bool sc = it.extra.value("newsScatter").toBool();
            if (as.isEmpty() && !sc) continue;
            QJsonObject o; o["channelId"] = ch; o["slotId"] = s.id; o["dated"] = dated; o["dayOfWeek"] = dated ? scheduleDow(s.date) : s.dayOfWeek; o["hour"] = s.hour; o["itemId"] = it.id; o["title"] = it.title; o["type"] = it.type; o["durationSec"] = it.durationSec; o["fixedOffsetSec"] = it.anchorSec() >= 0.0 ? it.anchorSec() : it.fixedOffsetSec; o["lockAtSec"] = it.anchorSec(); o["locked"] = it.locked;      // fixedOffsetSec keeps meaning "minute of the hour" for the Spots views (markers are soft anchors now)
            o["jobId"] = as.value("jobId"); o["recipe"] = as.value("recipe"); o["newsScatter"] = sc; o["queryBlock"] = it.extra.value("queryBlock"); out.append(o);
        }
    };
    for (const ChannelProfile& p : m_store->channels()) {
        for (const HourSlot& s : m_store->templateSlots(p.id)) add(s, p.id, false);
        const QDate today = nowIn(p.id).date(); const QDate sunday = today.addDays(-scheduleDow(today));
        for (const HourSlot& s : m_store->week(p.id, sunday)) if (s.showBlock.value("_override").toBool()) add(s, p.id, true);
    }
    return out;
}
QJsonObject SpotGenerator::markerStates() const {
    QJsonObject out; const qint64 now = nowMs();
    for (const QJsonValue& v : jobs()) {
        const QJsonObject j = v.toObject(); const QJsonObject r = recipe(j.value("recipe").toString());
        const QString mid = j.value("lastMediaId").toString(); bool f = false; const MediaRecord m = mid.isEmpty() ? MediaRecord() : m_store->media(mid, &f);
        const bool airable = f && airableAt(m, now);
        const double ageMin = f ? (now - (m.extra.value("autogen").toObject().value("generatedAt").toDouble(double(m.addedAtMs)))) / 60000.0 : -1;
        const double cad = effectiveCadenceMin(j); QString st, reason;
        if (!j.value("enabled").toBool()) { st = "off"; reason = "switched off: its hours air whatever was last generated"; }
        else if (!airable) { st = "stale"; reason = f ? "the last spot's window has closed" : "no spot generated yet"; }
        else if (ageMin > cad) { st = "due"; reason = !j.value("lastError").toString().isEmpty() ? "the last generation failed: " + j.value("lastError").toString().left(120) : "the newest spot is older than the cadence"; }
        else st = "fresh";
        QJsonObject o; o["state"] = st; o["reason"] = reason; o["recipe"] = j.value("recipe"); o["enabled"] = j.value("enabled"); o["genNum"] = j.value("genNum"); o["genTitle"] = genTitle(j, r); o["label"] = j.value("label"); o["mediaId"] = mid; o["title"] = f ? m.title : QString(); o["ageMin"] = ageMin; o["cadenceMin"] = cad; o["lastRunMs"] = j.value("lastRunMs"); o["nextRunMs"] = j.value("nextRunMs"); o["lastError"] = j.value("lastError"); o["expiresMs"] = f ? double(m.availableEndMs) : 0.0;
        int places = 0; for (const QJsonValue& pv : j.value("placements").toArray()) { const QJsonObject p = pv.toObject(); const int hs = p.value("hours").isString() ? 24 : p.value("hours").toArray().size(); const int ds = p.value("dows").toArray().isEmpty() ? 7 : p.value("dows").toArray().size(); places += hs * ds; } o["slotsPerWeek"] = places;
        out[j.value("id").toString()] = o;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// News scatter
// ---------------------------------------------------------------------------------------------
QJsonObject SpotGenerator::scatterCfg() const {
    QJsonObject c = m_store->settingJson("spots.scatter");
    if (!c.contains("enabled")) c["enabled"] = false; if (c.value("channel").toString().isEmpty()) c["channel"] = firstChannel(); if (!c.contains("perHour")) c["perHour"] = 3; if (c.value("feeds").toArray().isEmpty()) { QJsonArray f; for (const QString& u : kStarNews) f.append(u); c["feeds"] = f; }
    if (!c.contains("voice")) c["voice"] = "am_michael"; if (!c.contains("ttlMin")) c["ttlMin"] = 1440; if (!c.contains("maxNewPerRun")) c["maxNewPerRun"] = 4; if (!c.contains("everyMin")) c["everyMin"] = 45; if (!c.contains("minSec")) c["minSec"] = 30; if (!c.contains("seen")) c["seen"] = QJsonArray(); if (!c.contains("fails")) c["fails"] = QJsonObject();
    return c;
}
void SpotGenerator::setScatterCfg(const QJsonObject& o) { m_store->setSettingJson("spots.scatter", o); }
void SpotGenerator::scatterTick() {
    const QJsonObject c = scatterCfg(); if (!c.value("enabled").toBool()) return;
    const qint64 now = nowMs(), every = qint64(qMax(5, c.value("everyMin").toInt(45))) * 60000LL;
    if (now - m_scatterGenMs >= every && !m_scatterBusy) { m_scatterGenMs = now; scatterGenerate(Done()); }
    if (now - m_scatterApplyMs >= every) { m_scatterApplyMs = now; scatterApply(c.value("channel").toString(), c.value("perHour").toInt(3)); }
}
QJsonObject SpotGenerator::scatterGenerate(Done done) {
    const QJsonObject c = scatterCfg(); QStringList feeds; for (const QJsonValue& f : c.value("feeds").toArray()) if (f.toString().startsWith("http")) feeds << f.toString();
    if (feeds.isEmpty()) { QJsonObject e; e["error"] = "no scatter feeds configured"; e["status"] = 400; if (done) done(false, e); return e; }
    if (m_scatterBusy) { QJsonObject e; e["error"] = "scatter fetch already running"; e["status"] = 409; if (done) done(false, e); return e; }
    m_scatterBusy = true; m_scatterGenMs = nowMs();
    auto items = std::make_shared<QJsonArray>(); auto left = std::make_shared<int>(feeds.size());
    for (const QString& u : feeds) httpGet(u, 25000, [this, u, items, left, done](bool ok, const QByteArray& body, const QString&, const QString& err) {
        if (ok) { for (const QJsonValue& it : parseFeed(body, QUrl(u).host(), 40)) items->append(it); } else log("warning", "scatter feed unavailable: " + u + " (" + err + ")");
        if (--*left <= 0) { m_scatterBusy = false; onScatterFeeds(*items, done); }
    });
    QJsonObject o; o["started"] = true; o["feeds"] = feeds.size(); return o;
}
void SpotGenerator::onScatterFeeds(const QJsonArray& items, Done done) {
    const QJsonObject c = scatterCfg(); QSet<QString> seen; for (const QJsonValue& s : c.value("seen").toArray()) seen.insert(s.toString()); const QJsonObject fails = c.value("fails").toObject();
    QSet<QString> queued; for (Gen* g : m_gens) if (g->origin == "scatter") queued.insert(g->storyUrl);
    QList<QJsonObject> list; for (const QJsonValue& v : items) list << v.toObject();
    std::stable_sort(list.begin(), list.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("pubMs").toDouble() > b.value("pubMs").toDouble(); });
    int made = 0, fresh = 0; const int cap = qMax(1, c.value("maxNewPerRun").toInt(4)); const QJsonObject story = recipe("story");
    const double maxAgeH = story.value("maxAgeHours").toDouble(0), cutoff = maxAgeH > 0 ? double(nowMs()) - maxAgeH * 3600000.0 : 0.0;
    const QJsonObject stationMk = marketData(stationMarket());
    for (const QJsonObject& it : list) {
        if (!stationMk.isEmpty() && !areamarket::mentionsMarket(it.value("title").toString() + " " + it.value("text").toString() + " " + it.value("category").toString() + " " + it.value("link").toString(), stationMk)) continue;      // not about the station's market
        const QString url = it.value("link").toString().isEmpty() ? it.value("title").toString() : it.value("link").toString();
        if (seen.contains(url) || fails.value(url).toInt() >= 3 || queued.contains(url)) continue;
        if (it.value("title").toString().size() + it.value("text").toString().size() < 40) continue;
        { const double pub = it.value("pubMs").toDouble(); if (cutoff > 0 && pub > 0 && pub < cutoff) continue; }       // a story older than the recipe's maxAgeHours is not news any more
        ++fresh; if (made >= cap) continue;
        Gen* g = new Gen; g->origin = "scatter"; g->recipeKey = "story"; g->recipe = story; g->channel = c.value("channel").toString(); g->voice = c.value("voice").toString("am_michael"); g->ttlMin = c.value("ttlMin").toDouble(1440); g->targetSec = qMax(30, c.value("minSec").toInt(30)) + 30;
        g->items = QJsonArray{it}; g->materialReady = true; g->storyUrl = url; g->photoUrl = it.value("image").toString(); bindMarket(g, QString(), QJsonObject());
        enqueue(g); ++made;
    }
    m_lastScatter["considered"] = items.size(); m_lastScatter["fresh"] = fresh; m_lastScatter["queued"] = made; m_lastScatter["atMs"] = double(nowMs());
    QJsonObject o = m_lastScatter; o["ok"] = true; if (done) done(true, o);
    qInfo().noquote() << "[Spots] scatter: considered" << items.size() << "fresh" << fresh << "queued" << made; notify();
}
QJsonObject SpotGenerator::scatterApply(const QString& channelIn, int perHourIn) {
    const QJsonObject c = scatterCfg(); const QString ch = channelIn.isEmpty() ? c.value("channel").toString() : channelIn; const int perHour = qBound(0, perHourIn > 0 ? perHourIn : c.value("perHour").toInt(3), 12);
    const qint64 now = nowMs(); QList<MediaRecord> pool;
    for (const MediaRecord& m : m_store->mediaByType({"news"})) {
        if (!m.extra.value("newsScatter").toBool() && !m.extra.value("autogen").toObject().value("scatter").toBool()) continue;
        if (m.archived || m.extra.value("missing").toBool() || m.durationSec < 2 || (m.availableEndMs > 0 && m.availableEndMs <= now)) continue;
        if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(ch)) continue;
        MediaRecord pm = m; ensureTag(pm, kScatterTag); pool << pm;        // stories rendered before the tag existed join the scatter markers' query
    }
    std::sort(pool.begin(), pool.end(), [](const MediaRecord& a, const MediaRecord& b) { return a.addedAtMs > b.addedAtMs; });
    const QDateTime nowL = nowIn(ch); const QDate today = nowL.date(); const int curHour = nowL.time().hour(); int placed = 0, cleared = 0, hoursTouched = 0;
    const int minSec = qMax(8, c.value("minSec").toInt(30)); QSet<QString> touched;
    for (int h = 0; h < 24; ++h) {
        if (h <= curHour) continue;                               // never touch the hour on air, past hours are gone
        bool ov = false; HourSlot s = m_store->effectiveSlot(ch, today, h, &ov);
        s.channelId = ch; s.hour = h; s.date = today; s.dayOfWeek = -1; s.id = HourSlot::dateId(today, h); s.showBlock.remove("_override");
        QList<SlotItem> items; for (const SlotItem& it : s.items) { if (it.extra.value("newsScatter").toBool()) { ++cleared; continue; } items.append(it); }
        const int n = qMin(perHour, pool.size());
        if (n <= 0 && items.size() == s.items.size() && !ov) continue;    // nothing to place and nothing of ours in the template copy
        QList<int> idx; for (int i = 0; i < pool.size(); ++i) idx << i; std::shuffle(idx.begin(), idx.end(), *QRandomGenerator::global());
        const double span = 54.0 / qMax(1, n);
        for (int i = 0; i < n; ++i) {
            const MediaRecord& m = pool[idx[i]]; const int minute = qBound(3, int(3 + i * span + QRandomGenerator::global()->bounded(span)), 57);
            SlotItem it; it.id = "scatter-" + shortId(); it.title = ("NEWS: " + m.extra.value("autogen").toObject().value("headline").toString(m.title)).left(140); it.type = "news"; it.durationSec = qMax(double(minSec), m.durationSec); it.locked = true; it.lockAtSec = minute * 60.0;     // soft anchor (see makeMarker): nothing is cut
            QJsonObject qb; qb["types"] = QJsonArray{"news"}; qb["targetSec"] = it.durationSec; qb["prefer"] = "newest"; qb["categories"] = QJsonArray{kAutogenCategory}; qb["subcategories"] = QJsonArray{"News"}; qb["series"] = QJsonArray(); qb["nthLatest"] = i; qb["noRepeatMin"] = 30;
            qb["tags"] = QJsonArray{kScatterTag};                      // scatter stories only: headlines / economics / community reads share subcategory "News"
            it.extra["queryBlock"] = qb; it.lockLabel = "NEWS"; it.extra["newsScatter"] = true; it.extra["intendedMediaId"] = m.id;
            items.insert(seatIndex(items, it.lockAtSec), it); ++placed;
        }
        s.items = items; saveSlotQuiet(s, touched); ++hoursTouched;
    }
    announceSlots(touched);
    m_scatterApplyMs = now; m_lastScatter["placed"] = placed; m_lastScatter["cleared"] = cleared; m_lastScatter["pool"] = pool.size(); m_lastScatter["slots"] = hoursTouched; m_lastScatter["appliedMs"] = double(now); m_lastScatter["channel"] = ch;
    qInfo().noquote() << "[Spots] scatter: placed" << placed << "story spot(s) across" << hoursTouched << "hour(s) on" << ch << "from a pool of" << pool.size(); notify();
    QJsonObject o = m_lastScatter; o["ok"] = true; o["perHour"] = perHour; return o;
}
QJsonObject SpotGenerator::scatterStatus() const {
    QJsonObject c = scatterCfg(); QJsonObject o; for (const QString& k : {"enabled", "channel", "perHour", "feeds", "voice", "ttlMin", "maxNewPerRun", "everyMin", "minSec"}) o[k] = c.value(k);
    o["seenCount"] = c.value("seen").toArray().size(); o["failCount"] = c.value("fails").toObject().size(); o["busy"] = m_scatterBusy; o["last"] = m_lastScatter; o["lastGenMs"] = double(m_scatterGenMs); o["lastApplyMs"] = double(m_scatterApplyMs);
    int pending = 0; for (Gen* g : m_gens) if (g->origin == "scatter") ++pending; o["pending"] = pending;
    const qint64 now = nowMs(); int pool = 0; for (const MediaRecord& m : m_store->mediaByType({"news"})) if ((m.extra.value("newsScatter").toBool()) && !m.archived && (m.availableEndMs <= 0 || m.availableEndMs > now)) ++pool; o["pool"] = pool;
    return o;
}

// ---------------------------------------------------------------------------------------------
// Status / spot info / expiry
// ---------------------------------------------------------------------------------------------
QJsonObject SpotGenerator::status() const {
    QJsonObject o; o["current"] = m_current ? m_current->toJson() : QJsonValue(QJsonValue::Null);
    QJsonArray q; for (Gen* g : m_gens) if (g != m_current && !g->previewOnly) q.append(g->toJson()); o["queue"] = q;
    QJsonArray rec; for (int i = m_recent.size() - 1; i >= 0; --i) rec.append(m_recent[i]); o["recent"] = rec;
    o["markers"] = markerStates(); o["scatter"] = scatterStatus(); o["jobs"] = jobs().size(); o["env"] = config().value("env"); o["nowMs"] = double(nowMs());
    const QJsonObject rs = m_store->settingJson("spots.realty");                // the featured-home rotation: when the listings were last checked, which home aired last
    if (!rs.isEmpty()) { QJsonObject r; r["lastCheckMs"] = rs.value("lastCheckMs"); r["lastPick"] = rs.value("lastPick"); r["lastPickMs"] = rs.value("lastPickMs"); r["featured"] = rs.value("featured").toObject().size(); r["seen"] = rs.value("seen").toObject().size(); o["realty"] = r; }
    return o;
}
QJsonObject SpotGenerator::spotInfo(const QString& mediaId) const {
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    if (!f) { QJsonObject e; e["error"] = "no such media"; e["status"] = 404; return e; }
    QJsonObject o = m.toJson(); o["autogen"] = m.extra.value("autogen"); o["isAutogen"] = m.extra.contains("autogen") || m.extra.value("category").toString() == kAutogenCategory;
    for (const QJsonValue& v : loadJobs()) if (v.toObject().value("lastMediaId").toString() == mediaId) o["currentForJob"] = v.toObject().value("id");
    return o;
}
int SpotGenerator::expireSweep() {
    const qint64 now = nowMs(); const double keepDays = m_store->setting("spots.keepDays", "3").toDouble(); const qint64 keepMs = qint64(qMax(0.5, keepDays) * 86400000.0);
    QSet<QString> pinned; for (const QJsonValue& v : loadJobs()) pinned.insert(v.toObject().value("lastMediaId").toString());
    const QString outNative = QDir::toNativeSeparators(QDir(m_outDir).absolutePath()).toLower(); int archived = 0, deleted = 0;
    for (MediaRecord m : m_store->allMedia(true)) {
        const bool autogen = m.extra.contains("autogen") || m.extra.value("category").toString() == kAutogenCategory; if (!autogen) continue;
        const bool expired = m.availableEndMs > 0 && now > m.availableEndMs + 3600000LL;
        const qint64 made = qint64(m.extra.value("autogen").toObject().value("generatedAt").toDouble(double(m.addedAtMs)));
        const bool old = made > 0 && now - made > keepMs && !pinned.contains(m.id);
        if (old && QDir::toNativeSeparators(m.path).toLower().startsWith(outNative)) {
            if (QFileInfo(m.path).isFile()) QFile::remove(m.path); if (!m.thumbnailPath.isEmpty()) QFile::remove(m.thumbnailPath);
            m_store->deleteMedia(m.id); ++deleted; continue;
        }
        if (expired && !m.archived) { m.archived = true; m.extra["playoutStatus"] = "retired"; m.extra["retiredMs"] = double(now); m_store->saveMedia(m); ++archived; }
    }
    if (archived || deleted) qInfo().noquote() << "[Spots] sweep: archived" << archived << "expired spot(s), deleted" << deleted << "older than" << keepDays << "day(s)";
    return archived + deleted;
}
