#include "pillarpanels.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>
#include <QDebug>
#include <algorithm>
#include <cmath>

using namespace sched;
using J = QJsonObject;

namespace {

QStringList lowerList(const QJsonValue& v) {
    QStringList out;
    if (v.isString()) { for (const QString& s : v.toString().split(',', Qt::SkipEmptyParts)) if (!s.trimmed().isEmpty()) out << s.trimmed().toLower(); }
    else for (const QJsonValue& x : v.toArray()) { const QString s = x.toVariant().toString().trimmed().toLower(); if (!s.isEmpty()) out << s; }
    return out;
}

// "9:16", "9/16", "0.5625", 0.5625 -> width / height (0 when unreadable)
double ratioOf(const QJsonValue& v) {
    if (v.isDouble()) return v.toDouble() > 0 ? v.toDouble() : 0.0;
    const QString s = v.toString().trimmed();
    if (s.isEmpty()) return 0.0;
    static const QRegularExpression re("^(\\d+(?:\\.\\d+)?)\\s*[:/x]\\s*(\\d+(?:\\.\\d+)?)$");
    const QRegularExpressionMatch m = re.match(s);
    if (m.hasMatch()) { const double a = m.captured(1).toDouble(), b = m.captured(2).toDouble(); return a > 0 && b > 0 ? a / b : 0.0; }
    bool ok = false; const double d = s.toDouble(&ok); return ok && d > 0 ? d : 0.0;
}

QJsonArray strings(const QStringList& l) { QJsonArray a; for (const QString& s : l) a.append(s); return a; }

} // namespace

PillarPanels::PillarPanels(ScheduleStore* store, ScheduleExecutor* exec, QObject* parent)
    : QObject(parent), m_store(store), m_exec(exec) {
    m_timer.setInterval(5000);
    connect(&m_timer, &QTimer::timeout, this, &PillarPanels::tick);
}

void PillarPanels::start() { m_timer.start(); QTimer::singleShot(1500, this, &PillarPanels::tick); }

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

QJsonObject PillarPanels::defaults() {
    J d;
    d["enabled"] = false; d["channels"] = QJsonArray(); d["maxAspect"] = 1.5;
    d["types"] = QJsonArray(); d["excludeTypes"] = QJsonArray(); d["live"] = false;
    d["template"] = "pillar_panels"; d["station"] = ""; d["accent"] = "#ffc94a";
    J p; p["weather"] = true; p["sponsors"] = true; p["history"] = true; p["upNext"] = true; d["panels"] = p;
    J w; w["place"] = "Martinsville"; w["lat"] = 36.6915; w["lon"] = -79.8725; w["refreshMin"] = 15; d["weather"] = w;
    d["sponsors"] = QJsonArray(); d["sponsorSec"] = 9;
    J h; h["source"] = "wikipedia"; h["items"] = QJsonArray(); h["count"] = 8; h["avoid"] = strings(defaultAvoid()); d["history"] = h; d["historySec"] = 11;
    J u; u["count"] = 3; u["skipTypes"] = strings({"ad", "promo", "psa", "bumper", "station_id", "filler", "gap", "cg", "image"}); d["upNext"] = u;
    J s; s["speed"] = 1.0; s["density"] = 1.0; s["fps"] = 24; d["stars"] = s;
    return d;
}

QJsonObject PillarPanels::normalize(const QJsonObject& in) {
    J d = defaults();
    for (auto it = in.begin(); it != in.end(); ++it) {
        if (!d.contains(it.key())) continue;
        if (d.value(it.key()).isObject() && it.value().isObject()) { J sub = d.value(it.key()).toObject(); const J src = it.value().toObject(); for (auto s = src.begin(); s != src.end(); ++s) sub[s.key()] = s.value(); d[it.key()] = sub; }
        else d[it.key()] = it.value();
    }
    d["enabled"] = d.value("enabled").toBool(false); d["live"] = d.value("live").toBool(false);
    d["maxAspect"] = qBound(0.3, d.value("maxAspect").toDouble(1.5), 1.76);     // never 16:9 itself: a full-frame picture has no bars
    d["channels"] = strings(lowerList(d.value("channels"))); d["types"] = strings(lowerList(d.value("types"))); d["excludeTypes"] = strings(lowerList(d.value("excludeTypes")));
    if (d.value("template").toString().trimmed().isEmpty()) d["template"] = "pillar_panels";
    d["sponsorSec"] = qBound(3, d.value("sponsorSec").toInt(9), 120); d["historySec"] = qBound(4, d.value("historySec").toInt(11), 120);
    QJsonArray sp; for (const QJsonValue& v : d.value("sponsors").toArray()) { const J o = v.toObject(); if (!o.value("name").toString().trimmed().isEmpty() || !o.value("image").toString().trimmed().isEmpty()) sp.append(o); }
    d["sponsors"] = sp;
    J w = d.value("weather").toObject(); w["refreshMin"] = qBound(5, w.value("refreshMin").toInt(15), 180); d["weather"] = w;
    J u = d.value("upNext").toObject(); u["count"] = qBound(1, u.value("count").toInt(3), 6); u["skipTypes"] = strings(lowerList(u.value("skipTypes"))); d["upNext"] = u;
    J h = d.value("history").toObject(); h["count"] = qBound(1, h.value("count").toInt(8), 20); d["history"] = h;
    return d;
}

QJsonObject PillarPanels::cfg() const { return normalize(m_store ? m_store->settingJson("pillar.config") : J()); }
QJsonObject PillarPanels::config() const { J o; o["config"] = cfg(); o["defaults"] = defaults(); return o; }

QJsonObject PillarPanels::setConfig(const QJsonObject& body) {
    const J in = body.contains("config") && body.value("config").isObject() ? body.value("config").toObject() : body;
    J merged = cfg();
    for (auto it = in.begin(); it != in.end(); ++it) merged[it.key()] = it.value();          // PUT replaces top-level keys it names
    const J n = normalize(merged);
    if (m_store) m_store->setSettingJson("pillar.config", n);
    if (in.contains("weather")) { m_weatherAtMs = 0; m_weatherTryMs = 0; QTimer::singleShot(0, this, &PillarPanels::fetchWeather); }
    if (in.contains("history")) { m_historyDate.clear(); m_historyTryMs = 0; QTimer::singleShot(0, this, &PillarPanels::fetchHistory); }
    emit changed(QString());
    J o; o["config"] = n; return o;
}

QTimeZone PillarPanels::tzFor(const QString& ch) const {
    if (m_store) { bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (f) return p.tz(); }
    return QTimeZone::systemTimeZone();
}

// ---------------------------------------------------------------------------------------------
// Decision
// ---------------------------------------------------------------------------------------------

double PillarPanels::displayAspect(const QJsonObject& item) {
    const J x = item.value("extra").toObject();
    for (const char* k : {"displayAspect", "display_aspect", "dar", "aspect"}) { const double r = ratioOf(x.value(k)); if (r > 0) return r; }
    double w = item.value("width").toDouble(), h = item.value("height").toDouble();
    if (w <= 0 || h <= 0) { w = x.value("width").toDouble(); h = x.value("height").toDouble(); }
    if (w <= 0 || h <= 0) return 0.0;
    const double sar = ratioOf(x.value("sar"));
    if (sar > 0) w *= sar;
    const int rot = qAbs(x.value("rotation").toInt()) % 360;
    if (rot == 90 || rot == 270) std::swap(w, h);
    return w / h;
}

bool PillarPanels::qualifies(const QJsonObject& c, const QString& ch, const QJsonObject& item, double* aspectOut, QString* why) {
    auto no = [&](const QString& w) { if (why) *why = w; return false; };
    if (item.isEmpty()) return no("nothing on air");
    if (!c.value("enabled").toBool(false)) return no("pillar panels are off");
    const QStringList chs = lowerList(c.value("channels"));
    if (!chs.isEmpty() && !chs.contains(ch.toLower())) return no("not enabled on this channel");
    const J x = item.value("extra").toObject();
    const QString type = item.value("type").toString().toLower(), mtype = item.value("mediaType").toString().toLower();
    const QJsonValue force = x.value("pillarPanels");
    if (force.isBool() && !force.toBool()) return no("turned off for this media (extra.pillarPanels)");
    const bool forced = force.isBool() && force.toBool();
    if (!forced) {
        const QStringList ex = lowerList(c.value("excludeTypes"));
        if (ex.contains(type) || (!mtype.isEmpty() && ex.contains(mtype))) return no("type '" + type + "' is excluded");
        const QStringList only = lowerList(c.value("types"));
        if (!only.isEmpty() && !only.contains(type) && !only.contains(mtype)) return no("type '" + type + "' is not in the list");
    }
    double a = displayAspect(item);
    if (forced && a <= 0) a = 9.0 / 16.0;
    if (item.value("live").toBool(false) && !forced && ratioOf(x.value("displayAspect")) <= 0 && !c.value("live").toBool(false))
        return no("live input (its picture size is not known; set extra.displayAspect on its library entry)");
    if (a <= 0) return no("picture size unknown");
    if (aspectOut) *aspectOut = a;
    const double maxA = c.value("maxAspect").toDouble(1.5);
    if (!forced && a >= maxA) return no(QString("picture is %1:1, panels start below %2:1").arg(a, 0, 'f', 2).arg(maxA, 0, 'f', 2));
    if (why) *why = forced ? QString("forced for this media") : QString("picture is %1:1").arg(a, 0, 'f', 2);
    return true;
}

QJsonObject PillarPanels::specFor(const QString& ch, const QJsonObject& item) {
    const J c = cfg();
    double aspect = 0.0; QString why;
    bool on = qualifies(c, ch, item, &aspect, &why);
    const auto f = m_force.constFind(ch);
    if (f != m_force.constEnd() && f->untilMs > QDateTime::currentMSecsSinceEpoch()) { on = true; aspect = f->aspect; why = "preview"; }
    J last; last["on"] = on; last["why"] = why; last["aspect"] = aspect; last["title"] = item.value("title").toString(item.value("itemTitle").toString()); last["atMs"] = double(QDateTime::currentMSecsSinceEpoch());
    m_last[ch] = last;
    if (!on) return J();
    J params; params["aspect"] = QString::number(qBound(0.3, aspect, 1.76), 'f', 4);
    params["data"] = QString::fromUtf8(QJsonDocument(data(ch)).toJson(QJsonDocument::Compact));
    J s; s["template"] = c.value("template").toString("pillar_panels"); s["params"] = params; s["mode"] = "html";
    return s;
}

// ---------------------------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------------------------

QString PillarPanels::iconFor(int c) {
    if (c == 0) return "sun";
    if (c == 1 || c == 2) return "partly";
    if (c == 3) return "cloud";
    if (c == 45 || c == 48) return "fog";
    if (c >= 51 && c <= 57) return "drizzle";
    if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "rain";
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "snow";
    if (c >= 95) return "storm";
    return "cloud";
}

QString PillarPanels::condFor(int c, bool day) {
    if (c == 0) return day ? "Sunny" : "Clear";
    if (c == 1) return day ? "Mostly Sunny" : "Mostly Clear";
    if (c == 2) return "Partly Cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c == 61 || c == 80) return "Light Rain";
    if (c == 63 || c == 81) return "Rain";
    if (c == 65 || c == 82) return "Heavy Rain";
    if (c == 66 || c == 67) return "Freezing Rain";
    if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "Snow";
    if (c >= 95) return "Thunderstorms";
    return "Cloudy";
}

QJsonObject PillarPanels::parseWeather(const QJsonObject& om, const QTimeZone& tz) {
    J o;
    const J cur = om.value("current").toObject();
    if (cur.isEmpty() || !cur.contains("temperature_2m")) return o;
    const int code = cur.value("weather_code").toInt();
    const bool day = cur.value("is_day").toInt(1) != 0;
    o["tempF"] = qRound(cur.value("temperature_2m").toDouble());
    if (cur.contains("apparent_temperature")) o["feelsF"] = qRound(cur.value("apparent_temperature").toDouble());
    if (cur.contains("wind_speed_10m")) o["windMph"] = qRound(cur.value("wind_speed_10m").toDouble());
    o["isDay"] = day; o["code"] = code;
    QString icon = iconFor(code); if (!day && icon == "sun") icon = "moon"; if (!day && icon == "partly") icon = "partly_night";
    o["icon"] = icon; o["cond"] = condFor(code, day);
    const J d = om.value("daily").toObject();
    const QJsonArray t = d.value("time").toArray(), hi = d.value("temperature_2m_max").toArray(), lo = d.value("temperature_2m_min").toArray(), wc = d.value("weather_code").toArray(), pp = d.value("precipitation_probability_max").toArray();
    QJsonArray days;
    const QDate today = QDateTime::currentDateTime(tz).date();
    for (int i = 0; i < t.size() && i < hi.size() && i < lo.size(); ++i) {
        const QDate dt = QDate::fromString(t[i].toString(), Qt::ISODate);
        if (i == 0) { o["hiF"] = qRound(hi[i].toDouble()); o["loF"] = qRound(lo[i].toDouble()); if (i < pp.size()) o["rainPct"] = pp[i].toInt(); continue; }
        J x; x["dow"] = dt.isValid() ? (dt == today.addDays(1) ? QString("Tomorrow") : QLocale(QLocale::English).toString(dt, "ddd")) : QString();
        x["hiF"] = qRound(hi[i].toDouble()); x["loF"] = qRound(lo[i].toDouble());
        const int c = i < wc.size() ? wc[i].toInt() : 3; x["icon"] = iconFor(c); x["cond"] = condFor(c, true);
        if (i < pp.size()) x["rainPct"] = pp[i].toInt();
        days.append(x);
        if (days.size() >= 3) break;
    }
    o["days"] = days;
    return o;
}

QStringList PillarPanels::defaultAvoid() {
    // whole words, or a prefix when the entry ends with * ("kill*" = kill, killed, killing)
    return {"kill*", "dead", "death*", "die", "dies", "died", "murder*", "massacre*", "bomb*", "airstrike*", "terror*", "shooting*", "gunman", "gunmen", "assassin*",
            "execut*", "genocide*", "suicide*", "crash*", "hostage*", "explosion*", "earthquake*", "tsunami*", "famine*", "casualt*", "lynch*", "rape*", "abuse*"};
}

QJsonArray PillarPanels::parseHistory(const QJsonObject& wiki, int count, const QStringList& avoid) {
    QString pat;
    for (QString w : avoid) {
        w = w.trimmed().toLower(); if (w.isEmpty() || w == "*") continue;
        const bool prefix = w.endsWith('*'); if (prefix) w.chop(1);
        pat += (pat.isEmpty() ? "" : "|") + QRegularExpression::escape(w) + (prefix ? QString() : QString("\\b"));
    }
    const QRegularExpression bad(pat.isEmpty() ? QString("(?!)") : "\\b(?:" + pat + ")", QRegularExpression::CaseInsensitiveOption);
    QList<QPair<int, QString>> all;
    for (const char* key : {"selected", "events"}) {
        for (const QJsonValue& v : wiki.value(key).toArray()) {
            const J e = v.toObject(); QString text = e.value("text").toString().simplified();
            const int year = e.value("year").toInt();
            if (text.isEmpty() || year == 0) continue;
            if (text.size() < 25 || text.size() > 170) continue;          // a side bar holds a sentence, not a paragraph
            if (bad.match(text).hasMatch()) continue;
            all.append({year, text});
        }
        if (all.size() >= count * 2) break;
    }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    QJsonArray out;
    if (all.isEmpty()) return out;
    const int n = qMin(count, int(all.size()));
    for (int i = 0; i < n; ++i) {                                         // spread across the centuries, newest first
        const auto& e = all[int(std::floor(double(i) * all.size() / n))];
        J x; x["year"] = e.first; x["text"] = e.second; out.append(x);
    }
    return out;
}

QJsonArray PillarPanels::upNextFrom(const QJsonObject& rl, qint64 nowMs, int count, const QStringList& skip, const QString& cur, const QTimeZone& tz) {
    QJsonArray out; QString lastTitle;
    for (const char* key : {"current", "next"}) {
        for (const QJsonValue& v : rl.value(key).toObject().value("items").toArray()) {
            const J it = v.toObject();
            const qint64 start = qint64(it.value("plannedStartMs").toDouble());
            const QString type = it.value("type").toString().toLower(), title = it.value("title").toString().trimmed();
            if (it.value("id").toString() == cur || start <= nowMs + 5000 || title.isEmpty() || skip.contains(type)) continue;
            if (title.compare(lastTitle, Qt::CaseInsensitive) == 0) continue;   // segments of one show list once
            lastTitle = title;
            J x; x["title"] = title; x["type"] = type; x["startMs"] = double(start);
            x["time"] = QLocale(QLocale::English).toString(QDateTime::fromMSecsSinceEpoch(start, tz), "h:mm AP");
            out.append(x);
            if (out.size() >= count) return out;
        }
    }
    return out;
}

QJsonObject PillarPanels::data(const QString& ch) const {
    const J c = cfg();
    const QTimeZone tz = tzFor(ch);
    J d;
    QString station = c.value("station").toString().trimmed();
    if (station.isEmpty() && m_store) { bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (f) station = p.callLetters.isEmpty() ? p.name : p.callLetters; }
    d["station"] = station; d["accent"] = c.value("accent").toString("#ffc94a");
    d["panels"] = c.value("panels").toObject();
    J w = m_weather; w.remove("raw"); w["place"] = c.value("weather").toObject().value("place").toString(); d["weather"] = w;
    J h; h["date"] = QLocale(QLocale::English).toString(QDateTime::currentDateTime(tz).date(), "MMMM d");
    const J hc = c.value("history").toObject();
    const QJsonArray local = hc.value("items").toArray(), wiki = m_history.value("items").toArray();
    const bool useLocal = hc.value("source").toString() == "local" || wiki.isEmpty();      // the station's own list, or the fallback
    h["items"] = useLocal ? local : wiki;
    h["source"] = useLocal ? QString() : QString("Wikipedia");
    d["history"] = h;
    if (m_exec) {
        const J u = c.value("upNext").toObject();          // the item on air started in the past: the 5 s guard in upNextFrom leaves it out
        d["upNext"] = upNextFrom(m_exec->runlistJson(ch), QDateTime::currentMSecsSinceEpoch(), u.value("count").toInt(3), lowerList(u.value("skipTypes")), QString(), tz);
    } else d["upNext"] = QJsonArray();
    d["sponsors"] = c.value("sponsors").toArray();
    J r; r["sponsorSec"] = c.value("sponsorSec").toInt(9); r["historySec"] = c.value("historySec").toInt(11); d["rotate"] = r;
    d["stars"] = c.value("stars").toObject();
    d["tz"] = QString::fromUtf8(tz.id());
    return d;
}

// ---------------------------------------------------------------------------------------------
// Fetching (Supervisor thread, asynchronous)
// ---------------------------------------------------------------------------------------------

void PillarPanels::tick() {
    const qint64 t = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_force.begin(); it != m_force.end();) {
        if (it->untilMs <= t) { const QString ch = it.key(); it = m_force.erase(it); emit changed(ch); } else ++it;
    }
    const J c = cfg();
    if (!c.value("enabled").toBool(false) && m_force.isEmpty()) return;      // nothing to feed: no network traffic
    const qint64 every = qint64(c.value("weather").toObject().value("refreshMin").toInt(15)) * 60000;
    if (c.value("panels").toObject().value("weather").toBool(true) && t - m_weatherAtMs >= every && t - m_weatherTryMs >= 120000) fetchWeather();
    const QString today = QDateTime::currentDateTime().toString("MM-dd");
    if (c.value("panels").toObject().value("history").toBool(true) && c.value("history").toObject().value("source").toString() != "local"
        && m_historyDate != today && t - m_historyTryMs >= 600000) fetchHistory();
}

void PillarPanels::fetchWeather() {
    const J w = cfg().value("weather").toObject();
    m_weatherTryMs = QDateTime::currentMSecsSinceEpoch();
    const QString url = QString("https://api.open-meteo.com/v1/forecast?latitude=%1&longitude=%2&timezone=auto&temperature_unit=fahrenheit&wind_speed_unit=mph"
                                "&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m"
                                "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max&forecast_days=4")
                            .arg(w.value("lat").toDouble(36.6915), 0, 'f', 4).arg(w.value("lon").toDouble(-79.8725), 0, 'f', 4);
    QNetworkRequest rq{QUrl(url)}; rq.setTransferTimeout(15000); rq.setHeader(QNetworkRequest::UserAgentHeader, "PlayPlay-Supervisor/1.0 (pillar panels)");
    QNetworkReply* r = m_nam.get(rq);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        const QByteArray body = r->readAll();
        J parsed = r->error() == QNetworkReply::NoError ? parseWeather(QJsonDocument::fromJson(body).object(), QTimeZone::systemTimeZone()) : J();
        if (parsed.isEmpty()) {
            m_weather["error"] = r->error() != QNetworkReply::NoError ? r->errorString() : QString("unreadable forecast");
            qWarning().noquote() << "[Pillar] weather fetch failed:" << m_weather.value("error").toString();
            return;                                                        // keep the last good copy
        }
        parsed["updatedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        m_weather = parsed; m_weatherAtMs = QDateTime::currentMSecsSinceEpoch();
        emit changed(QString());
    });
}

void PillarPanels::fetchHistory() {
    m_historyTryMs = QDateTime::currentMSecsSinceEpoch();
    const QDate d = QDate::currentDate();
    const QString url = QString("https://en.wikipedia.org/api/rest_v1/feed/onthisday/selected/%1/%2").arg(d.month(), 2, 10, QChar('0')).arg(d.day(), 2, 10, QChar('0'));
    QNetworkRequest rq{QUrl(url)}; rq.setTransferTimeout(20000);
    rq.setHeader(QNetworkRequest::UserAgentHeader, "PlayPlay-Supervisor/1.0 (pillar panels; broadcast automation)");
    QNetworkReply* r = m_nam.get(rq);
    const QString key = d.toString("MM-dd");
    const J hc = cfg().value("history").toObject();
    const int count = hc.value("count").toInt(8);
    const QStringList avoid = lowerList(hc.value("avoid"));
    connect(r, &QNetworkReply::finished, this, [this, r, key, count, avoid]() {
        r->deleteLater();
        const QJsonArray items = r->error() == QNetworkReply::NoError ? parseHistory(QJsonDocument::fromJson(r->readAll()).object(), count, avoid) : QJsonArray();
        if (items.isEmpty()) {
            m_history["error"] = r->error() != QNetworkReply::NoError ? r->errorString() : QString("no usable entries");
            qWarning().noquote() << "[Pillar] history fetch failed:" << m_history.value("error").toString();
            return;
        }
        J h; h["items"] = items; h["updatedMs"] = double(QDateTime::currentMSecsSinceEpoch()); h["source"] = "Wikipedia";
        m_history = h; m_historyDate = key;
        emit changed(QString());
    });
}

QJsonObject PillarPanels::refresh() {
    m_weatherAtMs = 0; m_weatherTryMs = 0; m_historyDate.clear(); m_historyTryMs = 0;
    fetchWeather();
    if (cfg().value("history").toObject().value("source").toString() != "local") fetchHistory();
    J o; o["refreshing"] = true; return o;
}

QJsonObject PillarPanels::preview(const QString& ch, const QJsonObject& body) {
    const bool on = body.value("on").toBool(true);
    if (!on) { m_force.remove(ch); }
    else {
        Force f; f.aspect = ratioOf(body.value("aspect")); if (f.aspect <= 0 || f.aspect > 1.76) f.aspect = 9.0 / 16.0;
        f.untilMs = QDateTime::currentMSecsSinceEpoch() + qint64(qBound(5, body.value("sec").toInt(60), 900)) * 1000;
        m_force[ch] = f;
        if (m_weather.isEmpty() || m_weather.contains("error")) { if (m_weatherTryMs == 0 || QDateTime::currentMSecsSinceEpoch() - m_weatherTryMs > 30000) fetchWeather(); }
        if (m_history.value("items").toArray().isEmpty() && (m_historyTryMs == 0 || QDateTime::currentMSecsSinceEpoch() - m_historyTryMs > 30000)) fetchHistory();
    }
    emit changed(ch);
    J o; o["channelId"] = ch; o["preview"] = on;
    if (on) { o["aspect"] = m_force.value(ch).aspect; o["untilMs"] = double(m_force.value(ch).untilMs); }
    return o;
}

QJsonObject PillarPanels::status() const {
    J o; o["config"] = cfg();
    J w = m_weather; o["weather"] = w;
    J h = m_history; h["date"] = m_historyDate; o["history"] = h;
    J chs; for (auto it = m_last.begin(); it != m_last.end(); ++it) chs[it.key()] = it.value(); o["channels"] = chs;
    J pv; const qint64 t = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_force.begin(); it != m_force.end(); ++it) if (it->untilMs > t) { J x; x["aspect"] = it->aspect; x["untilMs"] = double(it->untilMs); pv[it.key()] = x; }
    o["previews"] = pv;
    return o;
}
