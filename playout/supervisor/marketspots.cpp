// Market update spots: Yahoo daily bars -> a four-scene wrap (index, big tech, crypto, closing card). Pure QtCore: see marketspots.h.
#include "marketspots.h"
#include "localspots.h"

#include <QJsonDocument>
#include <QLocale>
#include <QTimeZone>
#include <QUrl>
#include <cmath>

namespace marketspots {

static QLocale us() { return QLocale(QLocale::English, QLocale::UnitedStates); }

QJsonArray defaultSymbols() {
    auto sym = [](const char* symbol, const char* label, const char* spoken, const char* kind) {
        QJsonObject o; o["symbol"] = symbol; o["label"] = label; o["spoken"] = spoken; o["kind"] = kind; return o;
    };
    return {sym("^GSPC", "S&P 500", "the S and P five hundred", "index"), sym("AMZN", "Amazon", "Amazon", "stock"), sym("META", "Meta", "Meta", "stock"),
            sym("GOOGL", "Google", "Google", "stock"), sym("MSFT", "Microsoft", "Microsoft", "stock"), sym("BTC-USD", "Bitcoin", "Bitcoin", "crypto"), sym("ETH-USD", "Ethereum", "Ethereum", "crypto")};
}

QString chartUrl(const QString& symbol) {
    return "https://query1.finance.yahoo.com/v8/finance/chart/" + QString::fromUtf8(QUrl::toPercentEncoding(symbol)) + "?range=10d&interval=1d";
}

// ---------------------------------------------------------------------------------------------
// Speech
// ---------------------------------------------------------------------------------------------
QString spokenDecimal(double v, int decimals) {
    const double a = std::fabs(v);
    const double scale = std::pow(10.0, decimals);
    const qint64 scaled = qint64(std::llround(a * scale));
    const qint64 whole = scaled / qint64(scale), frac = scaled % qint64(scale);
    QString s = localspots::spokenNumber(whole);
    if (decimals > 0) {
        static const char* w[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
        const QString digits = QString("%1").arg(frac, decimals, 10, QChar('0'));
        QStringList ds; for (const QChar c : digits) ds << w[c.digitValue()];
        s += " point " + ds.join(' ');
    }
    return (v < 0 ? "minus " : "") + s;
}

QString spokenPrice(double v, bool cents) {
    const double a = std::fabs(v);
    if (!cents || a >= 10000.0) { const qint64 d = qint64(std::llround(a)); return localspots::spokenNumber(d) + (d == 1 ? " dollar" : " dollars"); }
    const qint64 total = qint64(std::llround(a * 100.0)), d = total / 100, c = total % 100;
    QString s = localspots::spokenNumber(d) + (d == 1 ? " dollar" : " dollars");
    if (c) s += " and " + localspots::spokenNumber(c) + (c == 1 ? " cent" : " cents");
    return s;
}

QString spokenMove(double pct) {
    if (std::fabs(pct) < 0.005) return "unchanged";
    return QString(pct > 0 ? "up " : "down ") + spokenDecimal(std::fabs(pct), std::fabs(pct) < 1.0 ? 2 : 1) + " percent";
}

/// "rose", "gained", "fell", "slipped": a verb that matches the size of the move.
static QString moveVerb(double pct) {
    if (std::fabs(pct) < 0.005) return "was unchanged";
    if (pct >= 2.0) return "jumped";
    if (pct >= 1.0) return "gained";
    if (pct > 0) return "rose";
    if (pct <= -2.0) return "tumbled";
    if (pct <= -1.0) return "dropped";
    return "slipped";
}

// ---------------------------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------------------------
QJsonObject parseChart(const QByteArray& json, const QJsonObject& spec) {
    const QJsonArray results = QJsonDocument::fromJson(json).object().value("chart").toObject().value("result").toArray();
    if (results.isEmpty()) return QJsonObject();
    const QJsonObject r = results.first().toObject(), meta = r.value("meta").toObject();
    const QJsonArray ts = r.value("timestamp").toArray();
    const QJsonArray cl = r.value("indicators").toObject().value("quote").toArray().first().toObject().value("close").toArray();
    const int off = meta.value("gmtoffset").toInt(0);
    const QTimeZone tz = QTimeZone::fromSecondsAheadOfUtc(off);
    QJsonArray closes, dates;
    for (int i = 0; i < ts.size() && i < cl.size(); ++i) {
        if (!cl.at(i).isDouble()) continue;                                              // a null close: no trade that day
        closes.append(cl.at(i).toDouble());
        dates.append(QDateTime::fromSecsSinceEpoch(qint64(ts.at(i).toDouble()), tz).date().toString(Qt::ISODate));
    }
    if (closes.size() < 2) return QJsonObject();
    const QDateTime asOf = QDateTime::fromSecsSinceEpoch(qint64(meta.value("regularMarketTime").toDouble()), tz);
    double price = closes.last().toDouble();
    if (meta.value("regularMarketPrice").isDouble() && dates.last().toString() == asOf.date().toString(Qt::ISODate)) { price = meta.value("regularMarketPrice").toDouble(); closes[closes.size() - 1] = price; }   // the last bar is live until the close
    const double prev = closes.at(closes.size() - 2).toDouble();
    const QString kind = spec.value("kind").toString("stock");
    QJsonObject o; o["symbol"] = spec.value("symbol"); o["label"] = spec.value("label"); o["spoken"] = spec.value("spoken").toString(spec.value("label").toString()); o["kind"] = kind;
    o["price"] = price; o["prevClose"] = prev; o["change"] = price - prev; o["pct"] = prev != 0 ? (price - prev) / prev * 100.0 : 0.0;
    QJsonArray tail, tailDates; for (int i = qMax(0, int(closes.size()) - 7); i < closes.size(); ++i) { tail.append(closes[i]); tailDates.append(dates[i]); }
    o["closes"] = tail; o["dates"] = tailDates; o["lastDate"] = dates.last(); o["asOfMs"] = double(asOf.toMSecsSinceEpoch());
    o["closed"] = kind == "crypto" || asOf.time() >= QTime(16, 0);                       // a US session is finished once the last trade is at or after 4 PM Eastern
    o["tz"] = meta.value("exchangeTimezoneName");
    return o;
}

// ---------------------------------------------------------------------------------------------
// The script
// ---------------------------------------------------------------------------------------------
static QString fmtPrice(double v, bool dollar) {
    const QString n = std::fabs(v) >= 10000.0 ? us().toString(qlonglong(std::llround(v))) : us().toString(v, 'f', 2);
    return (dollar ? "$" : "") + n;
}
static QString fmtPct(double pct) { return QString("%1%2%").arg(pct > 0.005 ? "+" : pct < -0.005 ? "-" : "").arg(QString::number(std::fabs(pct), 'f', 2)); }

QJsonObject marketsScript(const QJsonArray& quotes, const QDateTime& now, const QJsonObject& o) {
    const QString siteName = o.value("siteName").toString("Yahoo Finance"), site = o.value("site").toString();
    QJsonObject index; QList<QJsonObject> stocks, cryptos;
    for (const QJsonValue& v : quotes) {
        const QJsonObject q = v.toObject(); if (q.isEmpty()) continue;
        const QString kind = q.value("kind").toString();
        if (kind == "index") { if (index.isEmpty()) index = q; }
        else if (kind == "crypto") cryptos << q; else stocks << q;
    }
    if (index.isEmpty()) return localspots::errorResult("no S&P 500 quote could be read");
    // the market date is the index's last session; a spot is only made on a day it traded (weekend, holiday: nothing new to say)
    const QDate marketDay = QDate::fromString(index.value("lastDate").toString(), Qt::ISODate);
    const QDate today = now.toTimeZone(QTimeZone("America/New_York")).date();
    if (o.value("requireToday").toBool(true) && marketDay != today) return localspots::skipResult(QString("the US market did not trade today (last session %1)").arg(index.value("lastDate").toString()));
    const bool closed = index.value("closed").toBool();
    const QString dayLabel = us().toString(marketDay, "dddd, MMMM d"), weekday = us().toString(marketDay, "dddd");
    const double ipct = index.value("pct").toDouble();
    QJsonArray scenes, sources;
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    // 1) the index
    {
        const double price = index.value("price").toDouble(), change = index.value("change").toDouble();
        QString nar = QString("Here's your market wrap for %1. The S and P five hundred %2 at %3, %4").arg(dayLabel, closed ? "closed" : "is trading", spokenDecimal(price, 2), spokenMove(ipct));
        if (std::fabs(change) >= 0.5) nar += QString(", %1 %2 points").arg(change > 0 ? "a gain of" : "a loss of", spokenDecimal(std::fabs(change), 0));
        nar += ".";
        QJsonObject sc = localspots::makeScene("story", localspots::speech(nar));
        sc["title"] = "S&P 500"; sc["category"] = closed ? "MARKET CLOSE" : "MARKETS NOW"; sc["place"] = dayLabel.toUpper();
        sc["detail"] = fmtPrice(price, false) + "   " + fmtPct(ipct); sc["tone"] = ipct > 0.005 ? "up" : ipct < -0.005 ? "down" : "flat";
        sc["summary"] = QString("%1 points from the last close of %2").arg(QString(change >= 0 ? "+" : "-") + QString::number(std::fabs(change), 'f', 2), fmtPrice(index.value("prevClose").toDouble(), false));
        sc["spark"] = index.value("closes"); sc["sparkLabel"] = "Last " + QString::number(index.value("closes").toArray().size()) + " sessions";
        sc["counter"] = QString(); scenes.append(sc);
        sources.append(localspots::makeSource("S&P 500", "https://finance.yahoo.com/quote/%5EGSPC", QString(), "Yahoo Finance"));
    }
    auto rowOf = [&](const QJsonObject& q, bool dollar, const QString& suffix) {
        QJsonObject r; r["label"] = q.value("label"); r["sub"] = q.value("symbol").toString().remove("-USD"); r["value"] = fmtPrice(q.value("price").toDouble(), dollar);
        const double p = q.value("pct").toDouble(); r["change"] = fmtPct(p) + suffix; r["tone"] = p > 0.005 ? "up" : p < -0.005 ? "down" : "flat"; r["spark"] = q.value("closes");
        return r;
    };
    // 2) big tech and 3) crypto: a table scene each
    auto groupScene = [&](const QList<QJsonObject>& group, const QString& title, const QString& cat, bool crypto) {
        if (group.isEmpty()) return;
        QJsonArray rows; QStringList parts;
        for (const QJsonObject& q : group) {
            rows.append(rowOf(q, true, crypto ? " 24h" : QString()));
            const double p = q.value("pct").toDouble(), price = q.value("price").toDouble();
            const QString who = q.value("spoken").toString();
            if (std::fabs(p) < 0.005) parts << QString("%1 was unchanged at %2").arg(who, spokenPrice(price, !crypto));
            else parts << QString("%1 %2 %3 percent%4 to %5").arg(who, moveVerb(p), spokenDecimal(std::fabs(p), std::fabs(p) < 1.0 ? 2 : 1), crypto ? QString(" over the past day") : QString(), spokenPrice(price, !crypto));
        }
        QString nar = (crypto ? QString("In crypto, ") : QString("In big tech, ")) + parts.join(". ") + ".";
        QJsonObject sc = localspots::makeScene("story", localspots::speech(nar));
        sc["title"] = title; sc["category"] = cat; sc["place"] = dayLabel.toUpper(); sc["rows"] = rows; sc["counter"] = QString();
        scenes.append(sc);
        for (const QJsonObject& q : group) sources.append(localspots::makeSource(q.value("label").toString(), "https://finance.yahoo.com/quote/" + q.value("symbol").toString(), QString(), "Yahoo Finance"));
    };
    groupScene(stocks, "Big Tech", closed ? "STOCKS AT THE CLOSE" : "STOCKS NOW", false);
    groupScene(cryptos, "Crypto", "TRADING 24 HOURS", true);
    // count the table scenes for "1 / 3"
    int storyCount = 0; for (const QJsonValue& v : scenes) if (v.toObject().value("kind").toString() == "story") ++storyCount;
    int k = 0; for (int i = 0; i < scenes.size(); ++i) { QJsonObject sc = scenes[i].toObject(); if (sc.value("kind").toString() == "story") { sc["counter"] = QString("%1 / %2").arg(++k).arg(storyCount); scenes[i] = sc; } }
    // 4) the closing card: where the numbers came from, and that this is information
    QJsonObject outro = localspots::makeScene("outro", localspots::speech(QString("Market data from %1, delayed up to fifteen minutes. This report is for information only and is not investment advice.").arg(siteName)));
    outro["eyebrow"] = "MARKET DATA"; outro["big"] = siteName; outro["sub"] = "Quotes may be delayed. For information only, not investment advice.";
    scenes.append(outro);
    QStringList lines; lines << QString("S&P 500 %1 %2").arg(fmtPrice(index.value("price").toDouble(), false), fmtPct(ipct));
    for (const QJsonObject& q : stocks.mid(0, 2)) lines << QString("%1 %2").arg(q.value("label").toString(), fmtPct(q.value("pct").toDouble()));
    QJsonObject local; local["source"] = "markets"; local["marketDay"] = marketDay.toString(Qt::ISODate); local["closed"] = closed; local["sp500Pct"] = ipct; local["symbols"] = int(1 + stocks.size() + cryptos.size());
    Q_UNUSED(site); Q_UNUSED(weekday); Q_UNUSED(dot);
    return localspots::makeScript(scenes, QString("S&P 500 %1 %2").arg(closed ? (ipct >= 0 ? "closes up" : "closes down") : (ipct >= 0 ? "up" : "down"), QString::number(std::fabs(ipct), 'f', 2) + "%"), lines, "Market Close", o.value("energy").toInt(6), local, sources);
}

} // namespace marketspots
