// Market update spots: Yahoo chart parsing and the wrap's script. Payloads mirror the real endpoint's shape.
#include <QtTest>
#include <QDateTime>
#include <QJsonDocument>
#include <QTimeZone>
#include "../playout/supervisor/marketspots.h"

using namespace marketspots;

static QDateTime at(int y, int mo, int d, int h, int mi) { return QDateTime(QDate(y, mo, d), QTime(h, mi), QTimeZone("America/New_York")); }

/// A Yahoo chart payload: bars at 09:30 ET (stocks) or 00:00 UTC (crypto) with the given closes; the last trade at lastTradeLocal.
static QByteArray chart(const char* symbol, bool crypto, const QDate& firstDay, const QList<double>& closes, const QTime& lastTrade, double livePrice, int skipEvery = 0) {
    const int off = crypto ? 0 : -4 * 3600;
    QJsonArray ts, cl; QDate d = firstDay;
    for (int i = 0; i < closes.size(); ++i, d = d.addDays(1)) {
        if (!crypto) while (d.dayOfWeek() >= 6) d = d.addDays(1);
        const QDateTime bar(d, crypto ? QTime(0, 0) : QTime(9, 30), QTimeZone::fromSecondsAheadOfUtc(off));
        ts.append(double(bar.toSecsSinceEpoch()));
        cl.append(skipEvery && i == skipEvery ? QJsonValue() : QJsonValue(closes[i]));
    }
    const QDate lastDay = d.addDays(-1);
    const QDateTime lt(lastDay.dayOfWeek() >= 6 && !crypto ? lastDay.addDays(-(lastDay.dayOfWeek() - 5)) : lastDay, lastTrade, QTimeZone::fromSecondsAheadOfUtc(off));
    QJsonObject meta; meta["symbol"] = symbol; meta["gmtoffset"] = off; meta["exchangeTimezoneName"] = crypto ? "UTC" : "America/New_York";
    meta["regularMarketPrice"] = livePrice; meta["regularMarketTime"] = double(lt.toSecsSinceEpoch());
    QJsonObject quote; quote["close"] = cl; QJsonObject ind; ind["quote"] = QJsonArray{quote};
    QJsonObject res; res["meta"] = meta; res["timestamp"] = ts; res["indicators"] = ind;
    QJsonObject root; root["chart"] = QJsonObject{{"result", QJsonArray{res}}, {"error", QJsonValue()}};
    return QJsonDocument(root).toJson();
}
static QJsonObject spec(const char* sym, const char* label, const char* kind, const char* spoken = nullptr) {
    QJsonObject o; o["symbol"] = sym; o["label"] = label; o["kind"] = kind; if (spoken) o["spoken"] = spoken; return o;
}
// Friday 2026-10-23, shortly after the close
static QJsonArray closeQuotes(double spPrice = 7628.55) {
    const QDate d0(2026, 10, 15);
    QJsonArray q;
    q.append(parseChart(chart("^GSPC", false, d0, {7744.06, 7659.44, 7690.73, 7712.57, 7757.17, 7663.43, spPrice}, QTime(16, 0), spPrice), spec("^GSPC", "S&P 500", "index", "the S and P five hundred")));
    q.append(parseChart(chart("AMZN", false, d0, {254.76, 250.0, 262.45, 262.63, 262.37, 250.38, 248.28}, QTime(16, 0), 248.28), spec("AMZN", "Amazon", "stock")));
    q.append(parseChart(chart("META", false, d0, {736.72, 709.33, 705.44, 694.25, 697.52, 692.0, 644.29}, QTime(16, 0), 644.29), spec("META", "Meta", "stock")));
    q.append(parseChart(chart("GOOGL", false, d0, {349.22, 340.59, 344.18, 340.28, 334.76, 324.22, 321.22}, QTime(16, 0), 321.22), spec("GOOGL", "Google", "stock")));
    q.append(parseChart(chart("MSFT", false, d0, {539.44, 529.05, 533.52, 531.87, 532.95, 521.08, 523.83}, QTime(16, 0), 523.83), spec("MSFT", "Microsoft", "stock")));
    q.append(parseChart(chart("BTC-USD", true, QDate(2026, 10, 17), {81382.15, 82017.6, 81853.54, 82941.0, 84011.97, 81311.89, 81153.6}, QTime(16, 20), 81153.6), spec("BTC-USD", "Bitcoin", "crypto")));
    q.append(parseChart(chart("ETH-USD", true, QDate(2026, 10, 17), {2399.8, 2408.74, 2404.82, 2446.73, 2470.77, 2386.19, 2392.24}, QTime(16, 20), 2392.24), spec("ETH-USD", "Ethereum", "crypto")));
    return q;
}

class TestMarketSpots : public QObject {
    Q_OBJECT
private slots:
    void speechForms() {
        QCOMPARE(spokenDecimal(7628.55, 2), QString("seven thousand six hundred twenty eight point five five"));
        QCOMPARE(spokenDecimal(0.46, 2), QString("zero point four six"));
        QCOMPARE(spokenDecimal(1.234, 1), QString("one point two"));
        QCOMPARE(spokenDecimal(90.0, 0), QString("ninety"));
        QCOMPARE(spokenDecimal(5.05, 2), QString("five point zero five"));                  // a leading zero in the fraction is spoken
        QCOMPARE(spokenPrice(248.28, true), QString("two hundred forty eight dollars and twenty eight cents"));
        QCOMPARE(spokenPrice(100.0, true), QString("one hundred dollars"));
        QCOMPARE(spokenPrice(81153.6, true), QString("eighty one thousand one hundred fifty four dollars"));   // no cents on five-figure prices
        QCOMPARE(spokenPrice(2392.24, false), QString("two thousand three hundred ninety two dollars"));
        QCOMPARE(spokenMove(-0.456), QString("down zero point four six percent"));
        QCOMPARE(spokenMove(1.234), QString("up one point two percent"));
        QCOMPARE(spokenMove(0.001), QString("unchanged"));
        QCOMPARE(chartUrl("^GSPC"), QString("https://query1.finance.yahoo.com/v8/finance/chart/%5EGSPC?range=10d&interval=1d"));
        QCOMPARE(defaultSymbols().size(), 7);
    }

    void chartParses() {
        const QJsonObject q = parseChart(chart("AMZN", false, QDate(2026, 10, 15), {254.76, 250.0, 262.45, 262.63, 262.37, 250.38, 248.28}, QTime(16, 0), 248.28), spec("AMZN", "Amazon", "stock"));
        QCOMPARE(q.value("symbol").toString(), QString("AMZN"));
        QCOMPARE(q.value("price").toDouble(), 248.28);
        QCOMPARE(q.value("prevClose").toDouble(), 250.38);
        QVERIFY(qAbs(q.value("change").toDouble() - (-2.10)) < 1e-9);
        QVERIFY(qAbs(q.value("pct").toDouble() - (-2.10 / 250.38 * 100.0)) < 1e-9);
        QCOMPARE(q.value("closes").toArray().size(), 7);
        QCOMPARE(q.value("lastDate").toString(), QString("2026-10-23"));
        QVERIFY(q.value("closed").toBool());                                                 // last trade at 4:00 PM ET
        // mid-session: the last bar is live and the session is not closed
        const QJsonObject live = parseChart(chart("AMZN", false, QDate(2026, 10, 15), {254.76, 250.0, 262.45, 262.63, 262.37, 250.38, 248.28}, QTime(13, 5), 251.0), spec("AMZN", "Amazon", "stock"));
        QVERIFY(!live.value("closed").toBool());
        QCOMPARE(live.value("price").toDouble(), 251.0);
        QCOMPARE(live.value("closes").toArray().last().toDouble(), 251.0);                   // the series ends on the live price
        // a null close (no trade that day) is dropped; crypto is always "closed"; junk yields nothing
        const QJsonObject gap = parseChart(chart("X", false, QDate(2026, 10, 15), {10, 11, 12, 13, 14}, QTime(16, 0), 14, 2), spec("X", "X", "stock"));
        QCOMPARE(gap.value("closes").toArray().size(), 4);
        QVERIFY(parseChart(chart("BTC-USD", true, QDate(2026, 10, 17), {1, 2, 3}, QTime(3, 0), 3), spec("BTC-USD", "Bitcoin", "crypto")).value("closed").toBool());
        QVERIFY(parseChart("not json", spec("X", "X", "stock")).isEmpty());
        QVERIFY(parseChart("{\"chart\":{\"result\":null}}", spec("X", "X", "stock")).isEmpty());
    }

    void wrapForADayTheMarketTraded() {
        const QJsonObject s = marketsScript(closeQuotes(), at(2026, 10, 23, 16, 20), QJsonObject());
        QVERIFY(!s.contains("skip") && !s.contains("error"));
        const QJsonArray sc = s.value("scenes").toArray();
        QStringList kinds; for (const QJsonValue& v : sc) kinds << v.toObject().value("kind").toString();
        QCOMPARE(kinds, (QStringList{"story", "story", "story", "outro"}));
        const QJsonObject idx = sc[0].toObject();
        QCOMPARE(idx.value("title").toString(), QString("S&P 500"));
        QCOMPARE(idx.value("category").toString(), QString("MARKET CLOSE"));
        QCOMPARE(idx.value("detail").toString(), QString("7,628.55   -0.46%"));
        QCOMPARE(idx.value("tone").toString(), QString("down"));
        QCOMPARE(idx.value("spark").toArray().size(), 7);
        QVERIFY(idx.value("narration").toString().startsWith("Here's your market wrap for Friday, October 23. The S and P five hundred closed at seven thousand six hundred twenty eight point five five, down zero point four six percent, a loss of thirty five points."));
        const QJsonObject tech = sc[1].toObject();
        QCOMPARE(tech.value("title").toString(), QString("Big Tech"));
        const QJsonArray rows = tech.value("rows").toArray();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[0].toObject().value("label").toString(), QString("Amazon"));
        QCOMPARE(rows[0].toObject().value("value").toString(), QString("$248.28"));
        QCOMPARE(rows[0].toObject().value("change").toString(), QString("-0.84%"));
        QCOMPARE(rows[0].toObject().value("tone").toString(), QString("down"));
        QCOMPARE(rows[2].toObject().value("label").toString(), QString("Google"));
        QVERIFY(rows[0].toObject().value("spark").toArray().size() == 7);
        const QString tn = tech.value("narration").toString();
        QVERIFY(tn.startsWith("In big tech, Amazon slipped zero point eight four percent to two hundred forty eight dollars and twenty eight cents."));
        QVERIFY(tn.contains("Meta tumbled six point nine percent to six hundred forty four dollars and twenty nine cents."));     // past 2% is "tumbled", written with one decimal
        QVERIFY(tn.contains("Microsoft rose zero point five three percent"));
        const QJsonObject cr = sc[2].toObject();
        QCOMPARE(cr.value("rows").toArray()[0].toObject().value("value").toString(), QString("$81,154"));
        QCOMPARE(cr.value("rows").toArray()[0].toObject().value("change").toString(), QString("-0.19% 24h"));
        QVERIFY(cr.value("narration").toString().startsWith("In crypto, Bitcoin slipped zero point one nine percent over the past day to eighty one thousand one hundred fifty four dollars."));
        QCOMPARE(sc[0].toObject().value("counter").toString(), QString("1 / 3"));
        QCOMPARE(sc[2].toObject().value("counter").toString(), QString("3 / 3"));
        const QString outro = sc.last().toObject().value("narration").toString();
        QVERIFY(outro.contains("not investment advice"));                                       // information, never a recommendation
        QVERIFY(!s.value("narration").toString().contains("buy", Qt::CaseInsensitive) && !s.value("narration").toString().contains("sell", Qt::CaseInsensitive));
        QCOMPARE(s.value("headline").toString(), QString("S&P 500 closes down 0.46%"));
        QCOMPARE(s.value("package").toString(), QString("Market Close"));
    }

    void noSpotWhenTheMarketDidNotTrade() {
        // Saturday: the index's last session was Friday, so there is nothing new to report; Monday's holiday case is the same
        QVERIFY(marketsScript(closeQuotes(), at(2026, 10, 24, 16, 20), QJsonObject()).contains("skip"));
        // a manual run can ask for the last session anyway
        QJsonObject opts; opts["requireToday"] = false;
        QVERIFY(!marketsScript(closeQuotes(), at(2026, 10, 24, 16, 20), opts).contains("skip"));
        // no index quote: an error, never a half wrap
        QJsonArray none; QVERIFY(marketsScript(none, at(2026, 10, 23, 16, 20), QJsonObject()).contains("error"));
    }

    void midSessionRunSaysTradingNotClosed() {
        const QDate d0(2026, 10, 15);
        QJsonArray q;
        q.append(parseChart(chart("^GSPC", false, d0, {7744.06, 7659.44, 7690.73, 7712.57, 7757.17, 7663.43, 7700.0}, QTime(13, 5), 7700.0), spec("^GSPC", "S&P 500", "index", "the S and P five hundred")));
        const QJsonObject s = marketsScript(q, at(2026, 10, 23, 13, 10), QJsonObject());
        QVERIFY(!s.contains("skip"));
        QVERIFY(s.value("scenes").toArray()[0].toObject().value("narration").toString().contains("is trading at seven thousand seven hundred point zero zero"));
        QCOMPARE(s.value("scenes").toArray()[0].toObject().value("category").toString(), QString("MARKETS NOW"));
        QVERIFY(s.value("headline").toString().startsWith("S&P 500 up"));
        // only the index came back: a one-table wrap still ends on the disclaimer
        QCOMPARE(s.value("scenes").toArray().size(), 2);
    }
};

QTEST_APPLESS_MAIN(TestMarketSpots)
#include "test_marketspots.moc"
