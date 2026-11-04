#ifndef MARKETSPOTS_H
#define MARKETSPOTS_H

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/**
 * Market update spots (SpotGenerator recipe `markets_close`, render "scenes"): the S&P 500, four large-cap stocks, Bitcoin and Ethereum
 * as a short market wrap, generated once after the US close and aired every few hours until the next open.
 *
 * Quotes come from Yahoo's public chart endpoint (one daily-bar series per symbol, the same source for stocks and crypto). Pure
 * (QtCore only): the SpotGenerator fetches the bytes, these functions parse them and write the scenes. The wording reports what
 * happened and never recommends anything; the closing card says the data is for information only.
 */
namespace marketspots {

/// The default watch list: [{symbol, label, spoken, kind: index|stock|crypto}] - S&P 500, Amazon, Meta, Google (Alphabet), Microsoft, Bitcoin, Ethereum.
QJsonArray defaultSymbols();
/// https://query1.finance.yahoo.com/v8/finance/chart/<symbol>?range=10d&interval=1d
QString chartUrl(const QString& symbol);

/// One symbol's chart JSON -> {symbol, label, spoken, kind, price, prevClose, change, pct, closes[], dates[] (yyyy-MM-dd, exchange time),
/// lastDate, asOfMs, closed (the last bar is a finished session), tz}. Empty object when the payload has no usable bars.
QJsonObject parseChart(const QByteArray& json, const QJsonObject& spec);

/// opts: siteName, site, requireToday (true: a spot is only made on a day the US market traded), maxRows.
/// Returns {skip} when the market did not trade today (weekend, holiday) or a quote set is missing the index, {error} on no data.
QJsonObject marketsScript(const QJsonArray& quotes, const QDateTime& now, const QJsonObject& opts);

/// 7628.55 -> "seven thousand six hundred twenty eight point five five"; decimals = digits after the point (0 = none).
QString spokenDecimal(double v, int decimals);
/// 248.28 -> "two hundred forty eight dollars and twenty eight cents"; 81153.6 -> "eighty one thousand one hundred fifty four dollars".
QString spokenPrice(double v, bool cents);
/// -0.456 -> "down zero point four six percent"; +1.234 -> "up one point two percent"; ~0 -> "unchanged".
QString spokenMove(double pct);

} // namespace marketspots

#endif // MARKETSPOTS_H
