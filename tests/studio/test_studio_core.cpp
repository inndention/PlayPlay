// ST-02 core utilities (docs/plans/starmedia-studio.md §8): int64 micro-USD money (rounding, parsing, formats, 128-bit
// rate math), ids, stable JSON + hashes equal to the TypeScript (golden values in fixtures/core-golden.json, generated
// from the real StarMedia code by tools/studio/scripts/golden-core.mjs), the injectable clock (fake timers), config
// precedence (env > config.json > default; the OpenRouter key never shows) and the non-blocking, scrubbing log.
// ST-25: the embedded data files exported from StarMedia (core/datafiles: schema check, the JS-meaning regex
// translation) and their drift against the starmedia checkout (source hashes; a fresh Node export into a temp dir when
// Node and the checkout are on this machine).
// Hermetic: temp dirs only; environment variables it changes are restored, also when a check fails. The starmedia
// checkout is only read.
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/config.h"
#include "core/datafiles.h"
#include "core/ids.h"
#include "core/json.h"
#include "core/log.h"
#include "core/money.h"

using namespace studio;
namespace M = studio::money;

namespace {

QJsonObject golden() {
    static const QJsonObject g = [] {
        QFile f(QStringLiteral(STUDIO_TEST_DIR "/fixtures/core-golden.json"));
        if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return g;
}

double fromBits(const QString& hex) {
    const quint64 b = hex.toULongLong(nullptr, 16);
    double d;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

// Sets (or unsets, with a null value) an environment variable and puts the old state back when destroyed.
class EnvGuard {
public:
    EnvGuard(const char* name, const QByteArray& value) : m_name(name), m_had(qEnvironmentVariableIsSet(name)), m_old(qgetenv(name)) {
        set(value);
    }
    ~EnvGuard() { if (m_had) qputenv(m_name.constData(), m_old); else qunsetenv(m_name.constData()); }
    void set(const QByteArray& value) { if (value.isNull()) qunsetenv(m_name.constData()); else qputenv(m_name.constData(), value); }

private:
    QByteArray m_name;
    bool m_had;
    QByteArray m_old;
};

QString readAll(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// UTF-8 text with every non-ASCII UTF-16 unit as \uXXXX, so a failure message survives the console code page.
QString escaped(const QByteArray& utf8) {
    QString out;
    for (const QChar c : QString::fromUtf8(utf8))
        out += c.unicode() < 0x80 ? QString(c) : QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0'));
    return out;
}

// A 2-, 3- or 4-decimal display of m is an exact tie when the dropped digits are exactly half a unit.
bool isTie(qint64 m, int decimals) {
    const qint64 unit = decimals == 2 ? 10000 : decimals == 3 ? 1000 : 100;
    return m % unit == unit / 2;
}

} // namespace

class TestStudioCore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    // json
    void stableJsonGolden();
    void stableJsonSpecials();
    void jsNumberGolden();
    void argsHashGolden();
    void jsonHelpers();
    // money
    void moneyFromUsdGolden();
    void moneyFormatGolden();
    void moneyParse();
    void moneyArithmetic();
    void moneyRates();
    // ids
    void ids();
    // clock
    void fakeClock();
    void systemClock();
    // config
    void configKeyPrecedence();
    void configSettings();
    void configFile();
    // log
    void logScrub();
    void logWriter();
    void logTruncation();
    void logDropsAndThreads();
    void logQtHandler();
    // data files (ST-25)
    void dataEmbedded();
    void dataSchemaValidator();
    void dataRegex();
    void dataPatternsFromStarmedia();
    void dataMatchesStarmedia();
    void cleanupTestCase();
};

void TestStudioCore::initTestCase() {
    const QJsonObject g = golden();
    QVERIFY2(!g.isEmpty(), "fixtures/core-golden.json missing or unreadable");
    QVERIFY(g.value(QStringLiteral("source")).toString().contains(QStringLiteral("mcp/core/util.ts")));
}

// ---------------- json ----------------

void TestStudioCore::stableJsonGolden() {
    const QJsonArray cases = golden().value(QStringLiteral("stableJson")).toArray();
    QVERIFY(cases.size() >= 30);
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value(QStringLiteral("name")).toString();
        QString err;
        const QJsonValue v = json::parse(c.value(QStringLiteral("input")).toString().toUtf8(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(name + QStringLiteral(": ") + err));
        const QByteArray s = json::stable(v);
        const QByteArray want = c.value(QStringLiteral("stable")).toString().toUtf8();
        QVERIFY2(s == want, qPrintable(QStringLiteral("%1:\n got  %2\n want %3").arg(name, escaped(s.left(300)), escaped(want.left(300)))));
        QCOMPARE(json::stableHash(v), c.value(QStringLiteral("sha256")).toString().toLatin1());
    }
}

void TestStudioCore::stableJsonSpecials() {
    // Values JSON text cannot carry identically, built here the way the generator built them in JS.
    QHash<QString, QJsonValue> built;
    built[QStringLiteral("lone_high")] = QString(QChar(0xD800));
    built[QStringLiteral("lone_low")] = QStringLiteral("a") + QChar(0xDC00) + QStringLiteral("b");
    built[QStringLiteral("pair")] = QString::fromUcs4(U"\U0001F600");
    built[QStringLiteral("reversed_pair")] = QString(QChar(0xDE00)) + QChar(0xD83D);
    built[QStringLiteral("undefined_in_array")] = QJsonArray{1, QJsonValue(QJsonValue::Undefined), 2};
    built[QStringLiteral("undefined_member")] =
        QJsonObject{{QStringLiteral("b"), 1}, {QStringLiteral("a"), QJsonValue(QJsonValue::Undefined)},
                    {QStringLiteral("c"), QJsonArray{QJsonValue(QJsonValue::Undefined)}}};
    built[QStringLiteral("nan")] = qQNaN();
    built[QStringLiteral("infinity")] = qInf();
    built[QStringLiteral("neg_infinity")] = -qInf();
    built[QStringLiteral("nan_in_object")] = QJsonObject{{QStringLiteral("x"), qQNaN()}, {QStringLiteral("y"), -0.0}};
    built[QStringLiteral("top_undefined")] = QJsonValue(QJsonValue::Undefined);
    const QJsonArray cases = golden().value(QStringLiteral("special")).toArray();
    QCOMPARE(cases.size(), built.size());
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString id = c.value(QStringLiteral("id")).toString();
        QVERIFY2(built.contains(id), qPrintable(id));
        const QByteArray s = json::stable(built.value(id));
        QVERIFY2(s == c.value(QStringLiteral("stable")).toString().toUtf8(), qPrintable(id + QStringLiteral(": got ") + QString::fromUtf8(s)));
        QCOMPARE(json::sha256Hex(s), c.value(QStringLiteral("sha256")).toString().toLatin1());
    }
    // Key order never matters, nesting included.
    QJsonObject a{{QStringLiteral("z"), 1}, {QStringLiteral("a"), QJsonObject{{QStringLiteral("y"), 2}, {QStringLiteral("b"), 3}}}};
    QJsonObject b;
    b[QStringLiteral("a")] = QJsonObject{{QStringLiteral("b"), 3}, {QStringLiteral("y"), 2}};
    b[QStringLiteral("z")] = 1;
    QCOMPARE(json::stable(a), json::stable(b));
    QCOMPARE(json::stable(a), QByteArray(R"({"a":{"b":3,"y":2},"z":1})"));
    // Non-BMP keys built in C++ keep their members and sort by UTF-16 units (QJsonObject's own key lookup misses them).
    const QString emoji = QString::fromUcs4(U"\U0001F600");
    const QJsonObject e{{QStringLiteral("a"), 1}, {emoji, 2}, {QString(QChar(0xFF01)), 3}, {QString(QChar(0xE9)), 4}};
    QCOMPARE(json::stable(e), QByteArray("{\"a\":1,\"\xC3\xA9\":4,\"\xF0\x9F\x98\x80\":2,\"\xEF\xBC\x81\":3}"));
    // Qt 6.9: an object parsed from this text keeps its keys in code-point order but looks keys up in UTF-16 order, so
    // value(emoji) is Undefined although keys() lists it. stable() reads members through the iterator and keeps it.
    const QJsonObject parsed = json::parse(QByteArray("{\"\xC3\xA9\":1,\"\xF0\x9F\x98\x80\":4,\"\\uff01\":5,\"~\":11}")).toObject();
    QCOMPARE(parsed.size(), 4);
    QCOMPARE(json::stable(parsed), QByteArray("{\"~\":11,\"\xC3\xA9\":1,\"\xF0\x9F\x98\x80\":4,\"\xEF\xBC\x81\":5}"));
    // A qint64 beyond 2^53 is written as the double JS would hold.
    QCOMPARE(json::stable(QJsonValue(qint64(9007199254740993LL))), QByteArray("9007199254740992"));
}

void TestStudioCore::jsNumberGolden() {
    const QJsonArray cases = golden().value(QStringLiteral("numbers")).toArray();
    QVERIFY(cases.size() >= 400);
    int bad = 0;
    QStringList first;
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const double d = fromBits(c.value(QStringLiteral("bits")).toString());
        const QString got = json::jsNumber(d);
        if (got != c.value(QStringLiteral("js")).toString()) {
            ++bad;
            if (first.size() < 5) first << QStringLiteral("%1: got %2 want %3").arg(c.value(QStringLiteral("bits")).toString(), got, c.value(QStringLiteral("js")).toString());
        }
    }
    QVERIFY2(bad == 0, qPrintable(QStringLiteral("%1 mismatches: ").arg(bad) + first.join(QStringLiteral("; "))));
    QCOMPARE(json::jsNumber(-0.0), QStringLiteral("0"));
    QCOMPARE(json::jsNumber(qQNaN()), QStringLiteral("NaN"));
    QCOMPARE(json::jsNumber(-qInf()), QStringLiteral("-Infinity"));
    QCOMPARE(json::jsNumber(1e21), QStringLiteral("1e+21"));
    QCOMPARE(json::jsNumber(1.5e-7), QStringLiteral("1.5e-7"));
    QCOMPARE(json::jsNumber(123456789012345680000.0), QStringLiteral("123456789012345680000"));
    QCOMPARE(json::jsNumber(-0.000001), QStringLiteral("-0.000001"));
}

void TestStudioCore::argsHashGolden() {
    const QJsonObject g = golden();
    QStringList want;
    for (const QJsonValue& v : g.value(QStringLiteral("controlKeys")).toArray()) want << v.toString();
    QCOMPARE(json::controlKeys(), want);
    const QJsonArray cases = g.value(QStringLiteral("argsHash")).toArray();
    QCOMPARE(cases.size(), 5);
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const QJsonObject args = json::parseObject(c.value(QStringLiteral("args")).toString().toUtf8());
        QCOMPARE(json::argsHash(c.value(QStringLiteral("tool")).toString(), args), c.value(QStringLiteral("hash")).toString().toLatin1());
    }
    // The control keys never change the hash (the first two golden cases are the same call with and without them).
    QCOMPARE(cases.at(0).toObject().value(QStringLiteral("hash")), cases.at(1).toObject().value(QStringLiteral("hash")));
}

void TestStudioCore::jsonHelpers() {
    QString err;
    QCOMPARE(json::parse("42", &err).toDouble(), 42.0);
    QVERIFY(err.isEmpty());
    QCOMPARE(json::parse(R"("x")").toString(), QStringLiteral("x"));
    QVERIFY(json::parse("null", &err).isNull());
    QVERIFY(err.isEmpty());
    QVERIFY(json::parse("1,2", &err).isUndefined());
    QVERIFY(!err.isEmpty());
    QVERIFY(json::parse("{\"a\":1} x", &err).isUndefined());
    QVERIFY(!err.isEmpty());
    QVERIFY(json::parse("", &err).isUndefined());
    QVERIFY(json::parse("[1]", &err, 2).isUndefined());              // over the byte limit
    QVERIFY(err.contains(QStringLiteral("limit")));
    QVERIFY(json::parseObject("[1]", &err).isEmpty());
    QVERIFY(err.contains(QStringLiteral("object")));
    QCOMPARE(json::parseObject(R"({"a":{"b":2}})", &err).value(QStringLiteral("a")).toObject().value(QStringLiteral("b")).toInt(), 2);
    QVERIFY(err.isEmpty());
    // A leading UTF-8 BOM is skipped, as QJsonDocument does (Notepad / PowerShell 5.1 "UTF8"); only at the start.
    QCOMPARE(json::parseObject("\xEF\xBB\xBF{\"a\":3}", &err).value(QStringLiteral("a")).toInt(), 3);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(json::parse("\xEF\xBB\xBF" "7", &err).toDouble(), 7.0);
    QVERIFY(err.isEmpty());
    QVERIFY(json::parse("\xEF\xBB\xBF", &err).isUndefined());
    QVERIFY(!err.isEmpty());
    QVERIFY(json::parse("{\"a\":1}\xEF\xBB\xBF", &err).isUndefined());
    {                                      // offsets count the BOM bytes
        const QRegularExpression offsetRe(QStringLiteral("offset (\\d+)"));
        QVERIFY(json::parse("{\"a\":}", &err).isUndefined());
        const int plain = offsetRe.match(err).captured(1).toInt();
        QVERIFY(json::parse("\xEF\xBB\xBF{\"a\":}", &err).isUndefined());
        QVERIFY2(offsetRe.match(err).hasMatch(), qPrintable(err));
        QCOMPARE(offsetRe.match(err).captured(1).toInt(), plain + 3);
    }
    // compact / round trip
    const QJsonValue v = json::parse(R"({"b":[1,2.5,"é"],"a":null})");
    QCOMPARE(json::stable(json::parse(json::compact(v))), json::stable(v));
    QCOMPARE(json::compact(QJsonValue(1.5)), QByteArray("1.5"));
    // toInt64
    bool ok = false;
    QCOMPARE(json::toInt64(json::parse("9223372036854775807"), 0, &ok), std::numeric_limits<qint64>::max());
    QVERIFY(ok);
    QCOMPARE(json::toInt64(QJsonValue(2.5), -1, &ok), qint64(-1));
    QVERIFY(!ok);
    QCOMPARE(json::toInt64(QJsonValue(QStringLiteral("5")), -1, &ok), qint64(-1));
    QVERIFY(!ok);
    QCOMPARE(json::toInt64(QJsonValue(1e300), -1, &ok), qint64(-1));
    QVERIFY(!ok);
    QCOMPARE(json::toInt64(QJsonValue(70000), -1, &ok, 0, 65535), qint64(-1));
    QVERIFY(!ok);
    QCOMPARE(json::toInt64(QJsonValue(-3.0), 0, &ok), qint64(-3));
    QVERIFY(ok);
    // dotted paths
    QJsonObject o;
    QVERIFY(json::setAt(o, u"spend.dailyCapUsd", 7.5));
    QVERIFY(json::setAt(o, u"spend.autoApproveUsd", 1));
    QCOMPARE(json::at(o, u"spend.dailyCapUsd").toDouble(), 7.5);
    QVERIFY(json::at(o, u"spend.nope").isUndefined());
    QVERIFY(json::at(o, u"spend.dailyCapUsd.deeper").isUndefined());
    QVERIFY(!json::setAt(o, u"spend..x", 1));
    QVERIFY(json::setAt(o, u"spend.dailyCapUsd", QJsonValue(QJsonValue::Undefined)));
    QVERIFY(json::setAt(o, u"spend.autoApproveUsd", QJsonValue(QJsonValue::Undefined)));
    QVERIFY(o.isEmpty());                                                 // empty parents removed with the last value
    QCOMPARE(json::jsString(u"a\"b\\c\n\u0001"), QByteArray("\"a\\\"b\\\\c\\n\\u0001\""));
}

// ---------------- money ----------------

void TestStudioCore::moneyFromUsdGolden() {
    const QJsonArray cases = golden().value(QStringLiteral("fromUsd")).toArray();
    QVERIFY(cases.size() >= 100);
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const double usd = fromBits(c.value(QStringLiteral("bits")).toString());
        bool ok = false;
        const M::Micro m = M::fromUsd(usd, &ok);
        QVERIFY(ok);
        QVERIFY2(m == qint64(c.value(QStringLiteral("micro")).toDouble()),
                 qPrintable(QStringLiteral("%1 USD: got %2 want %3").arg(json::jsNumber(usd)).arg(m).arg(c.value(QStringLiteral("micro")).toDouble())));
    }
    bool ok = true;
    QCOMPARE(M::fromUsd(qQNaN(), &ok), M::Micro(0));
    QVERIFY(!ok);
    QCOMPARE(M::fromUsd(qInf(), &ok), M::Micro(0));
    QVERIFY(!ok);
    QCOMPARE(M::fromUsd(1e13, &ok), M::Micro(0));                       // 1e19 micro: beyond int64
    QVERIFY(!ok);
    // 4503599627.370497 USD: the product is the odd integer 2^52 + 1, where floor(x + 0.5) would give 2^52 + 2.
    QCOMPARE(M::fromUsd(fromBits(QStringLiteral("41f0c6f7a0b5ed8e")), &ok), M::Micro(4503599627370497LL));
    QVERIFY(ok);
    QCOMPARE(M::toUsd(250000), 0.25);
    QCOMPARE(M::toUsd(-1), -0.000001);
}

void TestStudioCore::moneyFormatGolden() {
    const QJsonArray cases = golden().value(QStringLiteral("usd")).toArray();
    QVERIFY(cases.size() >= 100);
    int ties = 0;
    for (const QJsonValue& cv : cases) {
        const QJsonObject c = cv.toObject();
        const M::Micro m = qint64(c.value(QStringLiteral("micro")).toDouble());
        const int mcpDec = m > 0 && m < 10000 ? 4 : 2;
        const int planDec = m >= 1000000 ? 2 : m >= 10000 ? 3 : 4;
        // Exact ties are where JS toFixed rounds the binary double (sometimes down); here they round half away from
        // zero. Everything else must equal the TS text.
        if (isTie(m, mcpDec)) {
            ++ties;
            QCOMPARE(M::format(m), QLatin1Char('$') + M::formatFixed(m, mcpDec));
        } else {
            QCOMPARE(M::format(m), c.value(QStringLiteral("mcp")).toString());
        }
        if (isTie(m, planDec)) QCOMPARE(M::formatPlan(m), QLatin1Char('$') + M::formatFixed(m, planDec));
        else QCOMPARE(M::formatPlan(m), c.value(QStringLiteral("plan")).toString());
    }
    QVERIFY(ties > 0);                     // the fixture does cover ties
    QCOMPARE(M::format(1005000), QStringLiteral("$1.01"));               // JS: (1.005).toFixed(2) == "1.00"
    QCOMPARE(M::format(0), QStringLiteral("$0.00"));
    QCOMPARE(M::format(5000), QStringLiteral("$0.0050"));
    QCOMPARE(M::format(-500000), QStringLiteral("-$0.50"));
    QCOMPARE(M::format(-4000), QStringLiteral("$0.00"));                // TS usd(-0.004) == "$-0.00": 2 decimals
    QCOMPARE(M::format(-6000), QStringLiteral("-$0.01"));
    QCOMPARE(M::formatPlan(-1500000), QStringLiteral("-$1.5000"));      // TS fmtUsd(-1.5) == "$-1.5000"
    QCOMPARE(M::formatPlan(-4000), QStringLiteral("-$0.0040"));
    QCOMPARE(M::formatPlan(0), QStringLiteral("$0.0000"));
    QCOMPARE(M::formatPlan(999999), QStringLiteral("$1.000"));
    QCOMPARE(M::formatPlan(12345678), QStringLiteral("$12.35"));
    QCOMPARE(M::formatFixed(1234567, 2), QStringLiteral("1.23"));
    QCOMPARE(M::formatFixed(1235000, 2), QStringLiteral("1.24"));
    QCOMPARE(M::formatFixed(-1235000, 2), QStringLiteral("-1.24"));
    QCOMPARE(M::formatFixed(-4000, 2), QStringLiteral("0.00"));         // no "-0.00"
    QCOMPARE(M::formatFixed(500000, 0), QStringLiteral("1"));
    QCOMPARE(M::formatFixed(-500000, 0), QStringLiteral("-1"));
    QCOMPARE(M::formatFixed(M::kMaxMicro, 6), QStringLiteral("9223372036854.775807"));
    QCOMPARE(M::formatFixed(std::numeric_limits<qint64>::min(), 6), QStringLiteral("-9223372036854.775808"));
    QCOMPARE(M::toDecimal(250000), QStringLiteral("0.25"));
    QCOMPARE(M::toDecimal(1000000), QStringLiteral("1"));
    QCOMPARE(M::toDecimal(-1), QStringLiteral("-0.000001"));
    QCOMPARE(M::toDecimal(0), QStringLiteral("0"));
    for (const M::Micro m : {M::Micro(0), M::Micro(1), M::Micro(-1), M::Micro(250000), M::Micro(19990000), M::kMaxMicro, M::kMinMicro}) {
        M::Micro back = 0;
        bool exact = false;
        QVERIFY(M::parseUsd(M::toDecimal(m), &back, &exact));
        QCOMPARE(back, m);
        QVERIFY(exact);
    }
}

void TestStudioCore::moneyParse() {
    struct Case { const char* text; bool ok; qint64 micro; bool exact; };
    const Case cases[] = {
        {"0.25", true, 250000, true}, {"$1.50", true, 1500000, true}, {"-$3", true, -3000000, true},
        {"$-3", true, -3000000, true}, {"+1.5", true, 1500000, true}, {"1e-3", true, 1000, true},
        {"1E2", true, 100000000, true}, {".5", true, 500000, true}, {"2.", true, 2000000, true},
        {"  7  ", true, 7000000, true}, {"0", true, 0, true}, {"-0", true, 0, true}, {"000.000100", true, 100, true},
        {"0.0000005", true, 1, false}, {"0.00000049", true, 0, false}, {"-0.0000005", true, -1, false},
        {"1.9999995", true, 2000000, false}, {"999999.9999995", true, 1000000000000, false},
        {"1e-400", true, 0, false}, {"5e-7", true, 1, false}, {"0.0000015", true, 2, false},
        {"9223372036854.775807", true, M::kMaxMicro, true}, {"-9223372036854.775807", true, M::kMinMicro, true},
        {"9223372036854.775808", false, 0, false}, {"1e400", false, 0, false}, {"1,000", false, 0, false},
        {"", false, 0, false}, {"   ", false, 0, false}, {"$", false, 0, false}, {"abc", false, 0, false},
        {"1e", false, 0, false}, {"--1", false, 0, false}, {"$$1", false, 0, false}, {"1.2.3", false, 0, false},
        {".", false, 0, false}, {"1 000", false, 0, false}, {"0x10", false, 0, false}, {"1$", false, 0, false},
    };
    for (const Case& c : cases) {
        M::Micro m = -42;
        bool exact = false;
        const bool ok = M::parseUsd(QString::fromUtf8(c.text), &m, &exact);
        QVERIFY2(ok == c.ok, c.text);
        if (!ok) continue;
        QVERIFY2(m == c.micro, qPrintable(QStringLiteral("%1 -> %2").arg(QString::fromUtf8(c.text)).arg(m)));
        QVERIFY2(exact == c.exact, c.text);
    }
    M::Micro m = 0;
    QVERIFY(!M::parseUsd(QString::fromUtf8("\u0661"), &m));              // a non-ASCII digit is not a digit here
}

void TestStudioCore::moneyArithmetic() {
    bool of = false;
    QCOMPARE(M::add(M::usd(2), M::cents(50), &of), M::Micro(2500000));
    QVERIFY(!of);
    QCOMPARE(M::add(M::kMaxMicro, 1, &of), M::kMaxMicro);
    QVERIFY(of);
    QCOMPARE(M::add(M::kMinMicro, -1, &of), M::kMinMicro);
    QVERIFY(of);
    QCOMPARE(M::sub(M::usd(1), M::cents(1), &of), M::Micro(990000));
    QVERIFY(!of);
    QCOMPARE(M::sub(M::kMinMicro, 1, &of), M::kMinMicro);
    QVERIFY(of);
    QCOMPARE(M::sub(0, std::numeric_limits<qint64>::min(), &of), M::kMaxMicro);
    QVERIFY(!of);

    QCOMPARE(M::mulDiv(7, 3, 2, &of), qint64(11));                        // 10.5 -> 11
    QVERIFY(!of);
    QCOMPARE(M::mulDiv(-7, 3, 2), qint64(-11));                           // half away from zero
    QCOMPARE(M::mulDiv(7, -3, -2), qint64(11));
    QCOMPARE(M::mulDiv(5, 1, 3), qint64(2));                              // 1.67 -> 2
    QCOMPARE(M::mulDiv(4, 1, 3), qint64(1));
    QCOMPARE(M::mulDiv(M::kMaxMicro, 2, 2, &of), M::kMaxMicro);           // the 128-bit intermediate keeps it exact
    QVERIFY(!of);
    QCOMPARE(M::mulDiv(M::kMaxMicro, M::kMaxMicro, M::kMaxMicro, &of), M::kMaxMicro);
    QVERIFY(!of);
    QCOMPARE(M::mulDiv(M::kMaxMicro, 3, 2, &of), M::kMaxMicro);
    QVERIFY(of);
    QCOMPARE(M::mulDiv(-M::kMaxMicro, 3, 2, &of), M::kMinMicro);
    QVERIFY(of);
    QCOMPARE(M::mulDiv(1, 1, 0, &of), qint64(0));
    QVERIFY(of);
    QCOMPARE(M::mulDiv(4000000000LL, 4000000000LL, 1000000LL, &of), qint64(16000000000000LL));   // 1.6e19 intermediate
    QVERIFY(!of);
    QCOMPARE(M::mulDiv(3, 1, std::numeric_limits<qint64>::min(), &of), qint64(0));
    QVERIFY(!of);
    // Budget ceiling ×1.15 (executor.ts L23).
    QCOMPARE(M::scale(M::usd(5), 115, 100), M::Micro(5750000));
    QCOMPARE(M::scale(10, 115, 100), M::Micro(12));                       // 11.5 -> 12
    QCOMPARE(M::scale(1, 115, 100), M::Micro(1));
}

void TestStudioCore::moneyRates() {
    M::Pico r = 0;
    bool exact = false;
    QVERIFY(M::parseRate(u"0.0000003", &r, &exact));                      // $0.30 per million tokens
    QCOMPARE(r, M::Pico(300000));
    QVERIFY(exact);
    QVERIFY(M::parseRate(u"0.000000075", &r, &exact));
    QCOMPARE(r, M::Pico(75000));
    QVERIFY(M::parseRate(u"0.04", &r, &exact));                           // $0.04 per image
    QCOMPARE(r, M::Pico(40000000000LL));
    QVERIFY(M::parseRate(u"1e-13", &r, &exact));                          // 0.1 pico: below the unit
    QCOMPARE(r, M::Pico(0));
    QVERIFY(!exact);
    QVERIFY(M::parseRate(u"0.0000000000005", &r, &exact));
    QCOMPARE(r, M::Pico(1));
    QVERIFY(!M::parseRate(u"free", &r));
    bool ok = false;
    QCOMPARE(M::rateFromUsd(3e-7, &ok), M::Pico(300000));
    QVERIFY(ok);
    QCOMPARE(M::rateFromUsd(qQNaN(), &ok), M::Pico(0));
    QVERIFY(!ok);

    bool of = false;
    QCOMPARE(M::cost(1000000, 300000, &of), M::Micro(300000));            // 1 M tokens at $0.30/M = $0.30
    QVERIFY(!of);
    QCOMPARE(M::cost(1, 300000), M::Micro(0));                            // 0.3 micro
    QCOMPARE(M::cost(2, 250000), M::Micro(1));                            // 0.5 micro -> 1
    QCOMPARE(M::cost(3, 40000000000LL), M::Micro(120000));                // 3 images at $0.04
    QCOMPARE(M::cost(1000000000LL, 500000000000LL, &of), M::Micro(500000000000000LL));   // 5e20 pico intermediate
    QVERIFY(!of);
    QCOMPARE(M::cost(M::kMaxMicro, M::kMaxMicro, &of), M::kMaxMicro);
    QVERIFY(of);
}

// ---------------- ids ----------------

void TestStudioCore::ids() {
    const QRegularExpression jobRe(QStringLiteral("^job_[0-9a-f]{16}$"));
    QSet<QString> seen;
    for (int i = 0; i < 20000; ++i) {
        const QString id = ids::random(u"job", 8);
        QVERIFY2(jobRe.match(id).hasMatch(), qPrintable(id));
        seen.insert(id);
    }
    QCOMPARE(seen.size(), 20000);
    QCOMPARE(ids::random(u"spl").size(), 4 + 12);
    QCOMPARE(ids::randomHex(1).size(), 2);
    QCOMPARE(ids::randomHex(64).size(), 128);
    QCOMPARE(ids::randomHex(1000).size(), 128);                           // clamped

    const QString tok = ids::approvalToken();
    QVERIFY(QRegularExpression(QStringLiteral("^apv_[0-9a-f]{24}$")).match(tok).hasMatch());
    QVERIFY(!log::scrub(QStringLiteral("approve with ") + tok).contains(tok));   // the TS apv_ pattern hides it

    FakeClock clock(1793016000000LL);                                    // 2026-10-26T12:00:00Z
    const QString r1 = ids::timeOrdered(u"run", clock);
    QVERIFY2(QRegularExpression(QStringLiteral("^run_20261026120000_[0-9a-f]{6}$")).match(r1).hasMatch(), qPrintable(r1));
    clock.advance(1000);
    const QString r2 = ids::timeOrdered(u"run", clock);
    QVERIFY(r1 < r2);
    clock.advance(86400000LL * 400);
    QVERIFY(r2 < ids::timeOrdered(u"run", clock));

    const QString k1 = ids::idemKey(u"st_vid_12", u"ab12", 1);
    QCOMPARE(k1, QString::fromLatin1(json::sha256Hex(R"(["st_vid_12","ab12",1])")));
    QCOMPARE(ids::idemKey(u"st_vid_12", u"ab12", 1), k1);
    QVERIFY(ids::idemKey(u"st_vid_12", u"ab12", 2) != k1);
    QVERIFY(ids::idemKey(u"st_vid_1", u"2ab12", 1) != k1);               // no concatenation ambiguity

    for (const char* good : {"job_3f9c", "a", "run_20261026120000_a1b2c3", "x.y-z_1", "Z9"})
        QVERIFY2(ids::isSafe(QString::fromLatin1(good)), good);
    for (const char* bad : {"", "_x", "-x", ".x", "a..b", "a.", "a/b", "a\\b", "a b", "a:b", "con", "NUL.txt", "com1", "Lpt9.log", "a%2e",
                            "COM0", "lpt0.txt"})
        QVERIFY2(!ids::isSafe(QString::fromLatin1(bad)), bad);
    QVERIFY(!ids::isSafe(QString(129, QLatin1Char('a'))));
    QVERIFY(ids::isSafe(QString(128, QLatin1Char('a'))));
    QVERIFY(!ids::isSafe(QString::fromUtf8("caf\u00e9")));
}

// ---------------- clock ----------------

void TestStudioCore::fakeClock() {
    FakeClock c(1793016000000LL);
    QCOMPARE(c.nowMs(), 1793016000000LL);
    QCOMPARE(c.steadyMs(), 0LL);
    QCOMPARE(c.isoNow(), QStringLiteral("2026-10-26T12:00:00.000Z"));
    QCOMPARE(c.localDate(), QDateTime::fromMSecsSinceEpoch(1793016000000LL).toLocalTime().date().toString(Qt::ISODate));

    QObject ctx;
    QStringList order;
    QList<qint64> seenAt;
    c.callAfter(100, &ctx, [&]() { order << QStringLiteral("b100"); seenAt << c.steadyMs(); });
    c.callAfter(50, &ctx, [&]() {
        order << QStringLiteral("a50");
        seenAt << c.steadyMs();
        c.callAfter(10, &ctx, [&]() { order << QStringLiteral("nested60"); seenAt << c.steadyMs(); });   // due inside this advance
    });
    c.callAfter(100, &ctx, [&]() { order << QStringLiteral("c100"); });   // same due time: set order
    const Clock::TimerId cancelled = c.callAfter(70, &ctx, [&]() { order << QStringLiteral("cancelled"); });
    c.callAfter(500, &ctx, [&]() { order << QStringLiteral("late"); });
    QCOMPARE(c.pendingTimers(), 5);
    QCOMPARE(c.nextDueSteadyMs(), 50LL);
    QVERIFY(c.cancel(cancelled));
    QVERIFY(!c.cancel(cancelled));
    QCOMPARE(c.advance(49), 0);
    QCOMPARE(c.advance(151), 4);
    QCOMPARE(order, (QStringList{QStringLiteral("a50"), QStringLiteral("nested60"), QStringLiteral("b100"), QStringLiteral("c100")}));
    QCOMPARE(seenAt, (QList<qint64>{50, 60, 100}));
    QCOMPARE(c.steadyMs(), 200LL);
    QCOMPARE(c.nowMs(), 1793016000200LL);
    QCOMPARE(c.pendingTimers(), 1);

    // A timer whose context is gone never runs.
    {
        auto gone = std::make_unique<QObject>();
        c.callAfter(10, gone.get(), [&]() { order << QStringLiteral("gone"); });
    }
    // Zero delay runs on runDue() without moving time.
    c.callAfter(0, &ctx, [&]() { order << QStringLiteral("now"); });
    QCOMPARE(c.runDue(), 1);
    QCOMPARE(c.steadyMs(), 200LL);
    QCOMPARE(c.advance(1000), 1);          // "late" (500); "gone" skipped
    QVERIFY(!order.contains(QStringLiteral("gone")));
    QCOMPARE(order.last(), QStringLiteral("late"));
    QCOMPARE(c.pendingTimers(), 0);
    QCOMPARE(c.nextDueSteadyMs(), -1LL);

    // A wall-clock jump (day roll, NTP step) moves neither steady time nor timers.
    c.callAfter(100, &ctx, [&]() { order << QStringLiteral("after-jump"); });
    const qint64 steady = c.steadyMs();
    c.setNowMs(1793016000000LL + 86400000LL);
    QCOMPARE(c.steadyMs(), steady);
    QCOMPARE(c.isoNow(), QStringLiteral("2026-10-27T12:00:00.000Z"));
    QCOMPARE(c.advance(99), 0);
    QCOMPARE(c.advance(1), 1);
    QCOMPARE(c.nowMs(), 1793016000000LL + 86400000LL + 100);

    // Through the base interface, as the engine modules take it.
    Clock& base = c;
    QCOMPARE(base.now().toMSecsSinceEpoch(), c.nowMs());
    QCOMPARE(Clock::isoUtc(0), QStringLiteral("1970-01-01T00:00:00.000Z"));
}

void TestStudioCore::systemClock() {
    Clock& c = Clock::system();
    const qint64 wall = QDateTime::currentMSecsSinceEpoch();
    QVERIFY(qAbs(c.nowMs() - wall) < 1000);
    const qint64 s0 = c.steadyMs();
    QObject ctx;
    bool fired = false, cancelledFired = false, goneFired = false;
    qint64 firedAt = 0;
    c.callAfter(30, &ctx, [&]() { fired = true; firedAt = c.steadyMs(); });
    const Clock::TimerId cancelled = c.callAfter(10, &ctx, [&]() { cancelledFired = true; });
    QVERIFY(c.cancel(cancelled));
    {
        auto gone = std::make_unique<QObject>();
        c.callAfter(5, gone.get(), [&]() { goneFired = true; });
    }
    QVERIFY(QTest::qWaitFor([&]() { return fired; }, 3000));
    QVERIFY(firedAt - s0 >= 25);          // QTimer precision on Windows: a few ms either side
    QTest::qWait(50);
    QVERIFY(!cancelledFired);
    QVERIFY(!goneFired);
    QVERIFY(!c.cancel(cancelled));
    QVERIFY(c.steadyMs() >= s0);
}

// ---------------- config ----------------

void TestStudioCore::configKeyPrecedence() {
    // The real key of this machine (if any) is out of the process environment for this test and comes back after it.
    EnvGuard envKey("OPENROUTER_API_KEY", QByteArray());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Config cfg(dir.path());
    QVERIFY(cfg.load());

    QVERIFY(!cfg.hasOpenRouterKey());
    QCOMPARE(cfg.keySource(), QStringLiteral("none"));
    QCOMPARE(cfg.describe().value(QStringLiteral("openrouter")).toObject().value(QStringLiteral("hasKey")).toBool(), false);

    const QString cfgKey = QStringLiteral("sk-or-v1-") + QStringLiteral("c0nf1gk3y").repeated(6);
    const QString envValue = QStringLiteral("sk-or-v1-") + QStringLiteral("3nvk3y00").repeated(6);
    cfg.setOpenRouterKey(QStringLiteral("  ") + cfgKey + QStringLiteral("\n"));   // trimmed
    QCOMPARE(cfg.keySource(), QStringLiteral("config"));
    QVERIFY(cfg.openRouterKey().key.reveal() == cfgKey);

    envKey.set(envValue.toUtf8());                                       // env wins over config
    Config::KeyInfo k = cfg.openRouterKey();
    QVERIFY(k.key.reveal() == envValue);
    QCOMPARE(sourceName(k.source), QStringLiteral("env"));
    QVERIFY(k.key.bearer() == "Bearer " + envValue.toUtf8());

    envKey.set("   ");                                                   // blank env = not set
    QCOMPARE(cfg.keySource(), QStringLiteral("config"));

    // Never in describe(), storedRedacted() or a qDebug line; in config.json only (the user's private data dir).
    envKey.set(envValue.toUtf8());
    QVERIFY(cfg.save());
    const QString file = readAll(cfg.filePath());
    QVERIFY(file.contains(cfgKey));
    QVERIFY(!file.contains(envValue));                                   // the env key is never written anywhere
    const QByteArray described = QJsonDocument(cfg.describe()).toJson();
    const QByteArray redacted = QJsonDocument(cfg.storedRedacted()).toJson();
    for (const QString& secret : {cfgKey, envValue}) {
        QVERIFY(!described.contains(secret.toUtf8()));
        QVERIFY(!described.contains(secret.mid(9, 12).toUtf8()));
        QVERIFY(!redacted.contains(secret.toUtf8()));
        QVERIFY(!redacted.contains(secret.mid(9, 12).toUtf8()));
    }
    QCOMPARE(cfg.storedRedacted().value(QStringLiteral("openrouter")).toObject().value(QStringLiteral("apiKey")).toString(), QStringLiteral("(hidden)"));
    const QJsonObject orInfo = cfg.describe().value(QStringLiteral("openrouter")).toObject();
    QCOMPARE(orInfo.value(QStringLiteral("hasKey")).toBool(), true);
    QCOMPARE(orInfo.value(QStringLiteral("keySource")).toString(), QStringLiteral("env"));
    QString dbg;
    QDebug(&dbg) << cfg.openRouterKey().key << Secret();
    QVERIFY(!dbg.contains(envValue));
    QVERIFY(dbg.contains(QStringLiteral("Secret(hidden)")));
    QVERIFY(dbg.contains(QStringLiteral("Secret(empty)")));
    // Once read, a key is scrubbed from every log line even when it matches none of the token shapes.
    const QString oddKey = QStringLiteral("orkey") + QStringLiteral("7q").repeated(20);
    QVERIFY(log::scrub(QStringLiteral("auth ") + oddKey).contains(oddKey));     // not registered yet
    envKey.set(oddKey.toUtf8());
    QVERIFY(cfg.openRouterKey().key.reveal() == oddKey);
    QVERIFY(!log::scrub(QStringLiteral("auth ") + oddKey).contains(oddKey));
    const QString oddCfg = QStringLiteral("cfgkey") + QStringLiteral("9z").repeated(20);
    cfg.setOpenRouterKey(oddCfg);                                        // the setter registers it at once
    QVERIFY(!log::scrub(QStringLiteral("cfg=") + oddCfg).contains(oddCfg));
    envKey.set(envValue.toUtf8());

    // The key cannot be written through the generic setter.
    QString err;
    QVERIFY(!cfg.set(u"openrouter.apiKey", QStringLiteral("x"), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!cfg.set(u"bridge.token", QStringLiteral("x")));
    QVERIFY(!cfg.set(u"x.client_secret", QStringLiteral("x")));
    QVERIFY(cfg.set(u"openrouter.model", QStringLiteral("anthropic/claude-sonnet-5.5")));

    // Cleared config key: env only; without env: none.
    cfg.setOpenRouterKey(QString());
    QCOMPARE(cfg.keySource(), QStringLiteral("env"));
    envKey.set(QByteArray());
    QCOMPARE(cfg.keySource(), QStringLiteral("none"));
    QVERIFY(cfg.stored(u"openrouter.apiKey").isUndefined());
    QCOMPARE(cfg.stored(u"openrouter.model").toString(), QStringLiteral("anthropic/claude-sonnet-5.5"));
}

void TestStudioCore::configSettings() {
    EnvGuard e1("PLAYPLAY_STUDIO_DAILY_CAP_USD", QByteArray());
    EnvGuard e2("STARMEDIA_MCP_DAILY_CAP_USD", QByteArray());
    EnvGuard e3("PLAYPLAY_STUDIO_PORT", QByteArray());
    EnvGuard e4("OPENROUTER_BASE_URL", QByteArray());
    EnvGuard e5("STARMEDIA_MCP_HOME", QByteArray());
    EnvGuard e6("PLAYPLAY_STUDIO_AUTO_APPROVE_USD", QByteArray());
    EnvGuard e7("PLAYPLAY_STUDIO_SESSION_CAP_USD", QByteArray());
    Config cfg;                            // memory only

    // Defaults (mcp/core/config.ts numbers).
    QCOMPARE(cfg.dailyCap(), M::usd(20));
    QCOMPARE(cfg.sessionCap(), M::usd(5));
    QCOMPARE(cfg.autoApprove(), M::cents(25));
    QCOMPARE(cfg.resolve(settings::dailyCapUsd).source, Source::Default);
    QCOMPARE(cfg.integer(settings::restPort), qint64(7760));
    QCOMPARE(cfg.integer(settings::wsPort), qint64(7761));
    QCOMPARE(cfg.openRouterBaseUrl(), QStringLiteral("https://openrouter.ai/api/v1"));
    QCOMPARE(cfg.string(settings::logLevel), QStringLiteral("info"));
    QVERIFY(cfg.starmediaHome().endsWith(QStringLiteral("/StarMedia/mcp")));

    // config.json beats the default; env beats config; the first env variable beats the second.
    QVERIFY(cfg.set(settings::dailyCapUsd.key, 7.5));
    QCOMPARE(cfg.dailyCap(), M::Micro(7500000));
    QCOMPARE(cfg.resolve(settings::dailyCapUsd).source, Source::Config);
    e2.set("3");
    QCOMPARE(cfg.dailyCap(), M::usd(3));
    QCOMPARE(cfg.resolve(settings::dailyCapUsd).envName, QStringLiteral("STARMEDIA_MCP_DAILY_CAP_USD"));
    e1.set("2.25");
    Config::Resolved r = cfg.resolve(settings::dailyCapUsd);
    QCOMPARE(r.source, Source::Env);
    QCOMPARE(r.envName, QStringLiteral("PLAYPLAY_STUDIO_DAILY_CAP_USD"));
    QCOMPARE(cfg.dailyCap(), M::Micro(2250000));
    bool ok = false;
    QCOMPARE(cfg.dailyCap(&ok), M::Micro(2250000));
    QVERIFY(ok);
    QVERIFY(!r.failedClosed);
    QVERIFY(cfg.spendLimitErrors().isEmpty());
    // An invalid money limit is never guessed at and never falls through to a source that may allow more (here the
    // $3 env and $7.50 config): it fails closed at $0 (mcp/core/config.ts refuses to start on the same variables).
    for (const char* bad : {"abc", "2,5", "5O", "-1", "1e400", "1000001"}) {             // junk, separators, < 0, > max
        e1.set(bad);
        r = cfg.resolve(settings::dailyCapUsd);
        QVERIFY2(r.failedClosed, bad);
        QCOMPARE(r.source, Source::Env);
        QCOMPARE(r.envName, QStringLiteral("PLAYPLAY_STUDIO_DAILY_CAP_USD"));
        QCOMPARE(r.value.toDouble(), 0.0);
        QCOMPARE(r.errors.size(), 1);
        QVERIFY(r.errors.first().contains(QStringLiteral("PLAYPLAY_STUDIO_DAILY_CAP_USD")));
        QCOMPARE(cfg.dailyCap(&ok), M::Micro(0));
        QVERIFY(!ok);
    }
    QStringList spendErrors = cfg.spendLimitErrors();
    QCOMPARE(spendErrors.size(), 1);
    QVERIFY2(spendErrors.first().startsWith(QStringLiteral("spend.dailyCapUsd: env PLAYPLAY_STUDIO_DAILY_CAP_USD")), qPrintable(spendErrors.first()));
    {
        const QJsonObject desc = cfg.describe();
        const QJsonObject cap = desc.value(QStringLiteral("settings")).toObject().value(QStringLiteral("spend.dailyCapUsd")).toObject();
        QCOMPARE(cap.value(QStringLiteral("failedClosed")).toBool(), true);
        QCOMPARE(cap.value(QStringLiteral("micro")).toDouble(), 0.0);
        QCOMPARE(desc.value(QStringLiteral("spendLimitErrors")).toArray().size(), 1);
        QVERIFY(!desc.contains(QStringLiteral("loadError")));
    }
    // The shared MCP variable, invalid while the Studio's own is unset: closed as well.
    e1.set(QByteArray());
    e2.set("abc");
    r = cfg.resolve(settings::dailyCapUsd);
    QVERIFY(r.failedClosed);
    QCOMPARE(r.envName, QStringLiteral("STARMEDIA_MCP_DAILY_CAP_USD"));
    QCOMPARE(cfg.dailyCap(), M::Micro(0));
    // A valid higher-precedence value still wins; the invalid lower one is never reached.
    e1.set("1.5");
    QCOMPARE(cfg.dailyCap(&ok), M::Micro(1500000));
    QVERIFY(ok);
    e1.set(QByteArray());
    e2.set(QByteArray());
    QVERIFY(cfg.set(settings::dailyCapUsd.key, QStringLiteral("$12.50")));   // text in config.json, parsed exactly
    QCOMPARE(cfg.dailyCap(), M::Micro(12500000));
    QVERIFY(cfg.set(settings::dailyCapUsd.key, QJsonValue(true)));          // invalid in config.json: closed, not $20
    r = cfg.resolve(settings::dailyCapUsd);
    QVERIFY(r.failedClosed);
    QCOMPARE(r.source, Source::Config);
    QCOMPARE(r.errors.size(), 1);
    QCOMPARE(cfg.dailyCap(), M::Micro(0));
    QVERIFY(cfg.set(settings::dailyCapUsd.key, QStringLiteral("20 USD")));
    QCOMPARE(cfg.dailyCap(&ok), M::Micro(0));
    QVERIFY(!ok);
    QVERIFY(cfg.set(settings::dailyCapUsd.key, QJsonValue(QJsonValue::Undefined)));
    QVERIFY(cfg.stored(settings::dailyCapUsd.key).isUndefined());
    QCOMPARE(cfg.dailyCap(&ok), M::usd(20));
    QVERIFY(ok);
    e6.set("0.1");
    QCOMPARE(cfg.autoApprove(), M::cents(10));
    e6.set("ten cents");                                                 // auto-approve fails closed too: ask for all
    QCOMPARE(cfg.autoApprove(&ok), M::Micro(0));
    QVERIFY(!ok);
    spendErrors = cfg.spendLimitErrors();
    QCOMPARE(spendErrors.size(), 1);
    QVERIFY(spendErrors.first().startsWith(QStringLiteral("spend.autoApproveUsd: ")));
    e6.set("0.1");
    QVERIFY(cfg.spendLimitErrors().isEmpty());

    // Integers. Not a money limit: an invalid value is skipped (falls through), never failed closed.
    e3.set("70000");
    r = cfg.resolve(settings::restPort);
    QCOMPARE(r.source, Source::Default);
    QCOMPARE(r.errors.size(), 1);
    QVERIFY(!r.failedClosed);
    QCOMPARE(cfg.integer(settings::restPort), qint64(7760));
    QVERIFY(cfg.set(settings::restPort.key, 7770));
    QCOMPARE(cfg.integer(settings::restPort), qint64(7770));
    QVERIFY(cfg.set(settings::restPort.key, QStringLiteral("7771")));
    QCOMPARE(cfg.integer(settings::restPort), qint64(7771));
    QVERIFY(cfg.set(settings::restPort.key, 7770.5));
    QCOMPARE(cfg.integer(settings::restPort), qint64(7760));
    e3.set(" 7772 ");
    QCOMPARE(cfg.integer(settings::restPort), qint64(7772));

    // Strings and paths.
    e4.set("http://127.0.0.1:9/api/v1//");
    QCOMPARE(cfg.openRouterBaseUrl(), QStringLiteral("http://127.0.0.1:9/api/v1"));
    e5.set("D:\\Somewhere\\StarMedia\\mcp");
    QCOMPARE(cfg.starmediaHome(), QStringLiteral("D:/Somewhere/StarMedia/mcp"));

    // describe(): every well-known setting with value and source, USD as dollars and micro.
    const QJsonObject d = cfg.describe().value(QStringLiteral("settings")).toObject();
    QCOMPARE(d.size(), settings::all().size());
    const QJsonObject cap = d.value(QStringLiteral("spend.dailyCapUsd")).toObject();
    QCOMPARE(cap.value(QStringLiteral("value")).toDouble(), 20.0);
    QCOMPARE(cap.value(QStringLiteral("micro")).toDouble(), 20000000.0);
    QCOMPARE(cap.value(QStringLiteral("source")).toString(), QStringLiteral("default"));
    QCOMPARE(d.value(QStringLiteral("net.restPort")).toObject().value(QStringLiteral("env")).toString(), QStringLiteral("PLAYPLAY_STUDIO_PORT"));
    QCOMPARE(d.value(QStringLiteral("spend.starmediaHome")).toObject().value(QStringLiteral("value")).toString(), QStringLiteral("D:/Somewhere/StarMedia/mcp"));
    QVERIFY(cfg.describe().value(QStringLiteral("file")).isNull());

    // A setting defined by a later module, with a bool and an unbounded USD amount.
    const Setting flag{QStringLiteral("x.enabled"), {}, Setting::Bool, false};
    QCOMPARE(cfg.flag(flag), false);
    QVERIFY(cfg.set(flag.key, QStringLiteral("yes")));
    QCOMPARE(cfg.flag(flag), true);
    const Setting big{QStringLiteral("x.budget"), {}, Setting::Usd, 1};
    QVERIFY(cfg.set(big.key, 1e9));
    QCOMPARE(cfg.usd(big), M::usd(1000000000));
    QVERIFY(cfg.set(big.key, 1e300));                                    // beyond int64 micro-USD: refused, closed
    QCOMPARE(cfg.usd(big, &ok), M::Micro(0));
    QVERIFY(!ok);
    QVERIFY(cfg.spendLimitErrors().isEmpty());                           // lists the well-known limits only
    const Setting count{QStringLiteral("x.count"), {}, Setting::Integer, 3};
    QVERIFY(cfg.set(count.key, 1e300));                                  // unbounded range, still never a UB cast
    QCOMPARE(cfg.integer(count), qint64(3));
    QCOMPARE(cfg.resolve(count).errors.size(), 1);
}

void TestStudioCore::configFile() {
    EnvGuard envKey("OPENROUTER_API_KEY", QByteArray());
    EnvGuard cap("PLAYPLAY_STUDIO_DAILY_CAP_USD", QByteArray());
    EnvGuard cap2("STARMEDIA_MCP_DAILY_CAP_USD", QByteArray());
    EnvGuard session("PLAYPLAY_STUDIO_SESSION_CAP_USD", QByteArray());
    EnvGuard autoApprove("PLAYPLAY_STUDIO_AUTO_APPROVE_USD", QByteArray());
    EnvGuard port("PLAYPLAY_STUDIO_PORT", QByteArray());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sub = dir.filePath(QStringLiteral("data"));            // save() creates the dir
    {
        Config cfg(sub);
        QVERIFY(cfg.load());                                             // missing file: defaults
        QCOMPARE(cfg.dailyCap(), M::usd(20));
        QVERIFY(cfg.set(settings::dailyCapUsd.key, 9));
        cfg.setOpenRouterKey(QStringLiteral("sk-or-v1-") + QString(48, QLatin1Char('f')));
        QVERIFY(cfg.save());
        QVERIFY(QFile::exists(QDir(sub).filePath(QStringLiteral("config.json"))));
    }
    {
        Config cfg(sub);
        QVERIFY(cfg.load());
        QCOMPARE(cfg.dailyCap(), M::usd(9));
        QCOMPARE(cfg.keySource(), QStringLiteral("config"));
        // A corrupt file is an error. Ordinary values held so far stay, but the money limits fail closed: the file
        // may hold a lower cap than the one held ($3 here), and must not be overwritten by save().
        QVERIFY(cfg.set(settings::restPort.key, 7770));
        QFile f(cfg.filePath());
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{\"spend\": {\"dailyCapUsd\": 3,");
        f.close();
        QString err;
        QVERIFY(!cfg.load(&err));
        QVERIFY(err.contains(QStringLiteral("config.json")));
        QCOMPARE(cfg.loadError(), err);
        bool ok = true;
        QCOMPARE(cfg.dailyCap(&ok), M::Micro(0));
        QVERIFY(!ok);
        QCOMPARE(cfg.sessionCap(), M::Micro(0));
        QCOMPARE(cfg.autoApprove(), M::Micro(0));
        QCOMPARE(cfg.spendLimitErrors().size(), 3);
        QVERIFY(cfg.spendLimitErrors().first().contains(QStringLiteral("could not be read")));
        QCOMPARE(cfg.describe().value(QStringLiteral("loadError")).toString(), err);
        QCOMPARE(cfg.integer(settings::restPort), qint64(7770));                 // not money: the held value stays
        QCOMPARE(cfg.stored(settings::dailyCapUsd.key).toDouble(), 9.0);
        QVERIFY(!cfg.save(&err));
        QVERIFY(err.contains(QStringLiteral("not saved")));
        QCOMPARE(readAll(cfg.filePath()), QStringLiteral("{\"spend\": {\"dailyCapUsd\": 3,"));   // left as the operator wrote it
        cap2.set("2");                     // a valid env value still overrides the unreadable file
        QCOMPARE(cfg.dailyCap(&ok), M::usd(2));
        QVERIFY(ok);
        cap.set("x");                      // ... but an invalid one does not fall through to it
        QCOMPARE(cfg.dailyCap(&ok), M::Micro(0));
        QVERIFY(!ok);
        cap.set(QByteArray());
        cap2.set(QByteArray());
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("[1,2]");
        f.close();
        QVERIFY(!cfg.load(&err));
        QVERIFY(err.contains(QStringLiteral("object")));
        QCOMPARE(cfg.dailyCap(), M::Micro(0));
        // A good file replaces everything and lifts the hold; a UTF-8 BOM (Notepad, PowerShell 5.1) is fine.
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("\xEF\xBB\xBF{\"spend\": {\"dailyCapUsd\": \"4.5\"}}");
        f.close();
        QVERIFY2(cfg.load(&err), qPrintable(err));
        QVERIFY(cfg.loadError().isEmpty());
        QCOMPARE(cfg.dailyCap(&ok), M::Micro(4500000));
        QVERIFY(ok);
        QCOMPARE(cfg.autoApprove(), M::cents(25));
        QVERIFY(cfg.spendLimitErrors().isEmpty());
        QVERIFY(!cfg.describe().contains(QStringLiteral("loadError")));
        QCOMPARE(cfg.keySource(), QStringLiteral("none"));
        QVERIFY(cfg.save(&err));
        // A file that disappears is "defaults" again, not an error.
        QVERIFY(QFile::remove(cfg.filePath()));
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{");
        f.close();
        QVERIFY(!cfg.load());
        QVERIFY(QFile::remove(cfg.filePath()));
        QVERIFY(cfg.load());
        QVERIFY(cfg.loadError().isEmpty());
        QCOMPARE(cfg.dailyCap(), M::usd(20));
    }
    // Memory-only config: load/save are no-ops.
    Config mem;
    QVERIFY(mem.load());
    QVERIFY(mem.save());
    QVERIFY(mem.filePath().isEmpty());
}

// ---------------- log ----------------

void TestStudioCore::logScrub() {
    const QString key = QStringLiteral("sk-or-v1-") + QStringLiteral("0123456789abcdef").repeated(4);
    const struct { QString in; QString mustNotContain; } cases[] = {
        {QStringLiteral("key ") + key + QStringLiteral(" end"), key.mid(9)},
        {QStringLiteral("Authorization: Bearer abcdefghijklmnop.qrstuv"), QStringLiteral("abcdefghijklmnop")},
        {QStringLiteral("token apv_0123456789abcdef01234567"), QStringLiteral("0123456789abcdef0123")},
        {QStringLiteral("stripe sk_live_ABCDEFGH12345678"), QStringLiteral("ABCDEFGH1234")},
        {QStringLiteral("gh ghp_ABCDEFGHIJKLMNOPQRSTUVWX"), QStringLiteral("ABCDEFGHIJKLMNOPQRST")},
        {QStringLiteral("google AIzaSyA-0123456789abcdefghijklmnopqrstu"), QStringLiteral("0123456789abcdefghij")},
        {QStringLiteral("aws AKIAABCDEFGHIJKLMNOP done"), QStringLiteral("ABCDEFGHIJKLMNOP")},
        {QStringLiteral("jwt eyJhbGciOi.eyJzdWIiOi.c2lnbmF0dXJl"), QStringLiteral("eyJzdWIiOi")},
        {QStringLiteral("cfg api_key=hunter2hunter2"), QStringLiteral("hunter2")},
        {QStringLiteral("PASSWORD: correcthorse"), QStringLiteral("correcthorse")},
        {QStringLiteral("img data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAAB"), QStringLiteral("iVBORw0KGgo")},
    };
    for (const auto& c : cases) {
        const QString out = log::scrub(c.in);
        QVERIFY2(!out.contains(c.mustNotContain), qPrintable(out));
    }
    QVERIFY(log::scrub(cases[10].in).contains(QStringLiteral("[data:image/png removed")));
    const QString b64 = QString(300, QLatin1Char('A'));
    QCOMPARE(log::scrub(QStringLiteral("x ") + b64 + QStringLiteral(" y")), QStringLiteral("x [base64 removed, 300 chars] y"));
    QCOMPARE(log::scrub(QStringLiteral("model x-ai/grok-4 took 1234 ms, $0.0012")), QStringLiteral("model x-ai/grok-4 took 1234 ms, $0.0012"));
    // Registered values (shorter than 8 chars are ignored, as in the TS).
    log::addSecret(QStringLiteral("plain-secret-value-42"));
    log::addSecret(QStringLiteral("short"));
    QCOMPARE(log::scrub(QStringLiteral("a plain-secret-value-42 b short")), QStringLiteral("a [redacted] b short"));
    // Field names (mcp SECRET_KEY).
    for (const char* k : {"apiKey", "api_key", "x-api-key", "client_secret", "Authorization", "password", "my_token", "privateKey", "secretSauce", "credentials", "approve"})
        QVERIFY2(log::isSecretKey(QString::fromLatin1(k)), k);
    for (const char* k : {"hasKey", "keySource", "model", "monkey", "myToken", "tokens", "dailyCapUsd", "baseUrl"})
        QVERIFY2(!log::isSecretKey(QString::fromLatin1(k)), k);
    QCOMPARE(log::urlForLog(QUrl(QStringLiteral("https://user:pw@openrouter.ai/api/v1/chat/completions?key=abc&x=1#frag"))),
             QStringLiteral("https://openrouter.ai/api/v1/chat/completions"));
}

void TestStudioCore::logWriter() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logDir = dir.filePath(QStringLiteral("logs"));
    QVERIFY(QDir().mkpath(logDir));
    // Retention: an old file of ours goes, an unrelated old file stays.
    const QString oldOurs = QDir(logDir).filePath(QStringLiteral("studio-2020-01-01.log"));
    const QString oldOther = QDir(logDir).filePath(QStringLiteral("other-2020-01-01.log"));
    for (const QString& p : {oldOurs, oldOther}) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("old\n");
        QVERIFY(f.setFileTime(QDateTime::currentDateTime().addDays(-40), QFileDevice::FileModificationTime));
        f.close();
    }

    FakeClock clock(QDateTime(QDate(2026, 10, 26), QTime(12, 0)).toMSecsSinceEpoch());   // local noon
    auto stopLog = qScopeGuard([]() { log::stop(); });                   // also drops the fake clock again
    log::Options o;
    o.dir = logDir;
    o.clock = &clock;
    o.maxFileBytes = 4096;
    o.keepDays = 14;
    QString err;
    QVERIFY2(log::start(o, &err), qPrintable(err));
    QVERIFY(log::running());
    QCOMPARE(log::minLevel(), log::Level::Info);

    const QString key = QStringLiteral("sk-or-v1-") + QStringLiteral("feedfacecafebeef").repeated(4);
    log::info(u"net", QStringLiteral("listening on 127.0.0.1:7770"));
    log::debug(u"net", QStringLiteral("filtered out"));
    log::warn(u"provider", QStringLiteral("429 from x-ai with key ") + key);
    log::error(u"store", QStringLiteral("line one\nline two\r\n2026-10-26T00:00:00.000Z INFO  forged: entry"));
    QVERIFY(log::flush());

    const QString today = QDir(logDir).filePath(QStringLiteral("studio-2026-10-26.log"));
    QTRY_COMPARE(log::currentFile(), today);
    const QString text = readAll(today);
    QVERIFY(text.contains(QStringLiteral("Z INFO  net: listening on 127.0.0.1:7770\n")));
    QVERIFY(text.startsWith(Clock::isoUtc(clock.nowMs())));
    QVERIFY(!text.contains(QStringLiteral("filtered out")));
    QVERIFY(text.contains(QStringLiteral("WARN  provider: 429 from x-ai with key [redacted]")));
    QVERIFY(!text.contains(key.mid(9)));
    QVERIFY(text.contains(QStringLiteral("ERROR store: line one\n    line two\n    2026-10-26T00:00:00.000Z INFO  forged")));
    QVERIFY(!QFile::exists(oldOurs));
    QVERIFY(QFile::exists(oldOther));
    const QStringList recent = log::recent(3);
    QCOMPARE(recent.size(), 3);
    QVERIFY(recent.at(0).contains(QStringLiteral("listening")));
    QVERIFY(recent.at(1).contains(QStringLiteral("[redacted]")));

    // Level change at run time.
    log::setMinLevel(log::Level::Debug);
    log::debug(u"net", QStringLiteral("now visible"));
    log::setMinLevel(log::Level::Info);
    // Size roll-over: past maxFileBytes the day continues in .1, .2 ...
    for (int i = 0; i < 80; ++i) log::info(u"bulk", QStringLiteral("line %1 ").arg(i) + QString(80, QLatin1Char('x')));
    QVERIFY(log::flush());
    QVERIFY(readAll(today).contains(QStringLiteral("DEBUG net: now visible")));
    QVERIFY(QFile::exists(QDir(logDir).filePath(QStringLiteral("studio-2026-10-26.1.log"))));
    // Day roll-over follows the (injected) local calendar day.
    clock.setNowMs(QDateTime(QDate(2026, 10, 27), QTime(0, 0, 5)).toMSecsSinceEpoch());
    log::info(u"net", QStringLiteral("after midnight"));
    QVERIFY(log::flush());
    QVERIFY(readAll(QDir(logDir).filePath(QStringLiteral("studio-2026-10-27.log"))).contains(QStringLiteral("after midnight")));

    log::stop();                           // drains first
    QVERIFY(!log::running());
    QVERIFY(log::flush(10));               // nothing left pending
    // Bytes are counted per line (no QFile::size() per line): no file grew past the limit by more than one line.
    const QFileInfoList files = QDir(logDir).entryInfoList({QStringLiteral("studio-2026-10-2*.log")}, QDir::Files);
    QVERIFY(files.size() >= 3);
    for (const QFileInfo& fi : files) QVERIFY2(fi.size() < 4096 + 200, qPrintable(fi.fileName()));
}

void TestStudioCore::logTruncation() {
    auto stopLog = qScopeGuard([]() { log::stop(); });
    log::Options o;                        // no dir: recent() only
    QVERIFY(log::start(o));
    const qsizetype lim = log::kMaxMessageChars;
    const QChar ellipsis(0x2026);
    // 1. A registered secret that matches no shape, straddling the cut (it starts 11 chars before it): dropped whole,
    //    not left as a prefix that no longer equals the registered value.
    const QString head = QStringLiteral("word ").repeated(int((lim - 10) / 5));
    QCOMPARE(head.size(), lim - 11);
    const QString odd = QStringLiteral("Kw4") + QStringLiteral("Qz8").repeated(12);
    log::addSecret(odd);
    const QString m1 = head + odd + QStringLiteral(" tail");
    log::info(u"cut", m1);
    // 2. An unregistered OpenRouter key whose first 16 chars ("sk-or-v1-" + 7) fall before the cut: too short for
    //    every shape pattern, so before the fix they reached the file.
    const QString lead = QStringLiteral("word ").repeated(int((lim - 21) / 5)) + QStringLiteral("auth ");
    QCOMPARE(lead.size(), lim - 16);
    const QString key = QStringLiteral("sk-or-v1-") + QStringLiteral("0123456789abcdef").repeated(4);
    QVERIFY(log::scrub(lead + key.left(16)).contains(QStringLiteral("sk-or-v1-0123456")));   // what a plain cut leaked
    log::info(u"cut", lead + key + QStringLiteral(" tail"));
    // 3. A cut on a word boundary keeps everything up to the limit.
    const QString words = QStringLiteral("abc ").repeated(int(lim / 4) + 10);
    log::info(u"cut", words);
    // 4. No break at all before the limit: nothing of the run is kept.
    log::info(u"cut", QString(lim + 100, QLatin1Char('z')));
    QVERIFY(log::flush());
    const QStringList r = log::recent(4);
    QCOMPARE(r.size(), 4);
    QVERIFY(!r.at(0).contains(QStringLiteral("Kw4")));
    QVERIFY(!r.at(0).contains(QStringLiteral("Qz8")));
    QVERIFY2(r.at(0).endsWith(QStringLiteral("word ") + ellipsis + QStringLiteral("[truncated %1 chars]").arg(m1.size() - head.size())),
             qPrintable(r.at(0).right(60)));
    QVERIFY(!r.at(1).contains(QStringLiteral("sk-or")));
    QVERIFY2(r.at(1).endsWith(QStringLiteral("auth ") + ellipsis + QStringLiteral("[truncated %1 chars]").arg(key.size() + 5)),
             qPrintable(r.at(1).right(60)));
    QVERIFY2(r.at(2).endsWith(QStringLiteral("abc ") + ellipsis + QStringLiteral("[truncated %1 chars]").arg(words.size() - lim)),
             qPrintable(r.at(2).right(60)));
    QVERIFY2(r.at(3).endsWith(QStringLiteral("cut: ") + ellipsis + QStringLiteral("[truncated %1 chars]").arg(lim + 100)),
             qPrintable(r.at(3).right(60)));
    // A message at the limit is not touched.
    log::info(u"cut", QStringLiteral("x ").repeated(int(lim / 2)));
    QVERIFY(log::flush());
    QVERIFY(!log::recent(1).first().contains(QStringLiteral("[truncated")));
}

void TestStudioCore::logDropsAndThreads() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto stopLog = qScopeGuard([]() { log::stop(); });
    log::Options o;
    o.dir = dir.path();
    o.maxQueue = 16;
    QVERIFY(log::start(o));
    QVERIFY(log::flush());
    log::stop();
    // Without a writer the queue fills; the rest is dropped and counted, and write() never waits.
    const quint64 before = log::droppedLines();
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 100000; ++i) log::info(u"q", QStringLiteral("queued %1").arg(i));
    QVERIFY2(t.elapsed() < 3000, qPrintable(QString::number(t.elapsed())));
    QCOMPARE(log::droppedLines() - before, quint64(100000 - 16));
    QVERIFY(log::start(o));                // the queued 16 are written, plus one notice
    QVERIFY(log::flush());
    const QString text = readAll(log::currentFile());
    QVERIFY(text.contains(QStringLiteral("queued 15\n")));
    QVERIFY(!text.contains(QStringLiteral("queued 16\n")));
    QVERIFY(text.contains(QStringLiteral("%1 log line(s) dropped").arg(100000 - 16)));

    // Many threads at once with a roomy queue: every line arrives once.
    o.maxQueue = 200000;
    QVERIFY(log::start(o));
    const QString file = log::currentFile();
    const int threads = 4, per = 5000;
    std::vector<std::thread> pool;
    for (int k = 0; k < threads; ++k)
        pool.emplace_back([k]() { for (int i = 0; i < per; ++i) log::info(u"mt", QStringLiteral("t%1 n%2").arg(k).arg(i)); });
    for (auto& th : pool) th.join();
    QVERIFY(log::flush(10000));
    const QString all = readAll(log::currentFile().isEmpty() ? file : log::currentFile());
    QCOMPARE(int(all.count(QStringLiteral(" mt: t"))), threads * per);
    QVERIFY(all.contains(QStringLiteral("t3 n4999\n")));
}

void TestStudioCore::logQtHandler() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    auto stopLog = qScopeGuard([]() { log::removeQtMessageHandler(); log::stop(); });
    log::Options o;
    o.dir = dir.path();
    QVERIFY(log::start(o));
    log::installQtMessageHandler();
    const QString key = QStringLiteral("sk-or-v1-") + QStringLiteral("0badc0de0badc0de").repeated(3);
    qWarning("provider said no for %s", qPrintable(key));
    qInfo() << "plain info line";
    log::removeQtMessageHandler();
    QVERIFY(log::flush());
    const QString text = readAll(log::currentFile());
    QVERIFY(text.contains(QStringLiteral("WARN  qt: provider said no for [redacted]")));
    QVERIFY(text.contains(QStringLiteral("INFO  qt: plain info line")));
    QVERIFY(!text.contains(key.mid(9)));
}

// ---------------- data files (ST-25) ----------------

namespace {

// git's text=auto may check the committed JSON out with CRLF; JSON.stringify never writes a raw CR inside a value, so
// folding CRLF to LF compares content only.
QByteArray lfText(QByteArray b) { return b.replace("\r\n", "\n"); }

QByteArray fileBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString firstDifference(const QByteArray& committed, const QByteArray& fresh) {
    const QList<QByteArray> a = committed.split('\n'), b = fresh.split('\n');
    qsizetype i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return QStringLiteral("first difference at line %1: committed %2 | exported %3")
        .arg(i + 1).arg(escaped(a.value(i).left(120)), escaped(b.value(i).left(120)));
}

// JSON written in raw strings spells '#' as the JSON escape \u0023: moc's preprocessor misreads '#' in a raw string.
QJsonValue J(const char* text) {
    QString err;
    const QJsonValue v = json::parse(QByteArray(text), &err);
    if (v.isUndefined()) qWarning("bad JSON in the test: %s", qPrintable(err));
    return v;
}

QString defRef(const char* def) { return QChar(0x23) + QStringLiteral("/$defs/") + QString::fromLatin1(def); }

bool matches(const QRegularExpression& re, const QString& s) { return re.isValid() && re.match(s).hasMatch(); }

// The first entry of `list` whose patterns (the JS sources in member `key`, or {source, flags} objects) match `id`.
QJsonObject firstMatching(const QJsonArray& list, const QString& id, const QString& kind = QString()) {
    for (const QJsonValue& v : list) {
        const QJsonObject g = v.toObject();
        if (!kind.isEmpty() && g.value(u"kind").toString() != kind) continue;
        const QJsonValue p = g.contains(u"match") ? g.value(u"match") : g.value(u"pattern");
        const QJsonArray pats = p.isArray() ? p.toArray() : QJsonArray{p};
        for (const QJsonValue& m : pats)
            if (matches(m.isString() ? data::regex(m.toString(), u"i") : data::regex(m), id)) return g;
    }
    return {};
}

} // namespace

void TestStudioCore::dataEmbedded() {
    QCOMPARE(data::names(), (QStringList{QStringLiteral("model-limits"), QStringLiteral("model-prompts"), QStringLiteral("realpeople"),
                                         QStringLiteral("styles")}));
    // The embedded copies are the committed files (a stale rcc output would hide a re-export).
    const QString dir = QStringLiteral(STUDIO_SOURCE_DIR "/data/");
    for (const QString& name : data::names() + QStringList{QStringLiteral("schema")}) {
        const QByteArray embedded = data::raw(name);
        QVERIFY2(!embedded.isEmpty(), qPrintable(name));
        QVERIFY2(lfText(embedded) == lfText(fileBytes(dir + name + u".json")), qPrintable(name + u": embedded copy differs from tools/studio/data"));
    }
    QVERIFY(!data::schema().isEmpty());

    for (const QString& name : data::names()) {
        const QStringList errs = data::check(name);
        QVERIFY2(errs.isEmpty(), qPrintable(name + u": " + errs.join(u"; ")));
        QString err = QStringLiteral("unset");
        const QJsonObject o = data::load(name, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(o.value(u"version").toInt(), data::kFormatVersion);
        QVERIFY(o.value(u"_comment").toString().contains(QStringLiteral("export-starmedia-data.mjs")));
        QVERIFY(!o.value(u"source").toObject().value(u"files").toArray().isEmpty());
        QCOMPARE(data::load(name), o); // cached
    }
    // Only the listed names exist: nothing a caller passes becomes a resource path.
    for (const char* bad : {"schema", "nope", "../data/styles", "styles.json", "STYLES", ""}) {
        QString err;
        QVERIFY2(data::load(QString::fromLatin1(bad), &err).isEmpty(), bad);
        QVERIFY2(err.contains(QStringLiteral("unknown data file")), qPrintable(err));
        if (qstrcmp(bad, "schema") != 0) QVERIFY2(data::raw(QString::fromLatin1(bad)).isEmpty(), bad);
    }

    // model-limits: what the provider layer (ST-07) relies on
    const QJsonObject ml = data::load(u"model-limits");
    const QJsonArray models = ml.value(u"models").toArray();
    QVERIFY(models.size() >= 100);
    QSet<QString> ids;
    for (const QJsonValue& m : models) {
        const QString id = m[u"id"].toString();
        QVERIFY2(!ids.contains(id), qPrintable(id));
        ids.insert(id);
    }
    QVERIFY(ids.contains(QStringLiteral("google/veo-3.1")));
    const QJsonObject pay = ml.value(u"payload").toObject();
    QCOMPARE(pay.value(u"openrouterMaxBody").toInteger(), 50LL * 1024 * 1024); // the measured 413 limit
    QCOMPARE(pay.value(u"videosMaxBody").toInteger(), 32505856LL);
    QCOMPARE(pay.value(u"videosBody")[u"cap"].toInteger(), 32505856LL);
    QVERIFY(pay.value(u"videosBody")[u"safe"].toInteger() < pay.value(u"videosBody")[u"cap"].toInteger());
    QVERIFY(pay.value(u"defaultBody")[u"safe"].toInteger() < pay.value(u"defaultBody")[u"cap"].toInteger());
    QVERIFY(ml.value(u"retired").toObject().contains(u"openai/sora-2"));

    // realpeople: the safety lists (ST-15) are there and normalised
    const QJsonObject rp = data::load(u"realpeople");
    QVERIFY(rp.value(u"publicFigures").toArray().size() >= 150);
    QVERIFY(rp.value(u"publicFigures").toArray().contains(QStringLiteral("barack obama")));
    QVERIFY(rp.value(u"ambiguousNames").toArray().contains(QStringLiteral("trump")));
    QVERIFY(rp.value(u"stageNames").toArray().contains(QStringLiteral("rihanna")));
    QCOMPARE(rp.value(u"confusables").toObject().value(QString(QChar(0x0430))).toString(), QStringLiteral("a")); // Cyrillic a

    // model-prompts and styles (ST-19)
    const QJsonObject mp = data::load(u"model-prompts");
    QVERIFY(mp.value(u"guides").toArray().size() >= 40);
    QCOMPARE(mp.value(u"recommend")[u"presets"][u"tv-ad"][u"seconds"].toInt(), 30);
    const QJsonObject st = data::load(u"styles");
    const QJsonArray styles = st.value(u"styles").toArray();
    QCOMPARE(st.value(u"defaultId").toString(), styles.at(0)[u"id"].toString());
    QSet<QString> styleIds, groups;
    for (const QJsonValue& g : st.value(u"groups").toArray()) groups.insert(g.toString());
    for (const QJsonValue& s : styles) {
        QVERIFY2(!styleIds.contains(s[u"id"].toString()), qPrintable(s[u"id"].toString()));
        styleIds.insert(s[u"id"].toString());
        QVERIFY(groups.contains(s[u"group"].toString()));
    }
    QVERIFY(!styleIds.contains(QStringLiteral("custom"))); // reserved by styleOf()
}

void TestStudioCore::dataSchemaValidator() {
    const QJsonObject root = J(R"({"$defs": {
        "name": {"type": "string", "minLength": 1, "maxLength": 3, "pattern": "^[a-z\\u00e9]+$"},
        "loop": {"$ref": "\u0023/$defs/loop"}
    }})").toObject();
    QVERIFY(!root.isEmpty());
    struct Case { const char* value; const char* schema; const char* expect; }; // expect "" = valid
    const Case cases[] = {
        {"5", R"({"type": "string"})", "$: expected string, got integer"},
        {"1.5", R"({"type": "integer"})", "$: expected integer, got number"},
        {"2", R"({"type": "number"})", ""},
        {"null", R"({"type": ["string", "null"]})", ""},
        {R"({"a": 1})", R"({"type": "object", "required": ["a", "b"]})", "$: missing b"},
        {R"({"a": 1, "z": 2})", R"({"properties": {"a": {"type": "integer"}}, "additionalProperties": false})", "$.z: not allowed"},
        {R"({"a": 1, "z": "x"})", R"({"properties": {"a": {}}, "additionalProperties": {"type": "integer"}})", "$.z: expected integer, got string"},
        {R"({"ab": 1})", R"({"propertyNames": {"maxLength": 1}})", "$.ab (name): longer than 1"},
        {"{}", R"({"minProperties": 1})", "$: fewer than 1 members"},
        {"1.0", R"({"const": 1})", ""},
        {R"("1")", R"({"const": 1})", "$: expected 1"},
        {R"("c")", R"({"enum": ["a", "b"]})", R"($: "c" is not one of ["a","b"])"},
        {R"("\ud83d\ude00")", R"({"maxLength": 1})", "$: longer than 1"}, // UTF-16 units, as in JS
        {R"("xbx")", R"({"pattern": "b"})", ""},                          // unanchored
        {R"("a\n")", R"({"pattern": "^a$"})", "does not match ^a$"},      // JS $: PCRE2 alone would accept "a\n"
        {R"("\u00e9")", R"({"$ref": "\u0023/$defs/name"})", ""},
        {R"("abcd")", R"({"$ref": "\u0023/$defs/name"})", "$: longer than 3"},
        {R"("A")", R"({"$ref": "\u0023/$defs/name"})", "does not match"},
        {"0", R"({"minimum": 1})", "$: below 1"},
        {"5", R"({"maximum": 4})", "$: above 4"},
        {"0", R"({"exclusiveMinimum": 0})", "$: not above 0"},
        {"[1, 2]", R"({"minItems": 3})", "$: fewer than 3 items"},
        {"[1, 2]", R"({"maxItems": 1})", "$: more than 1 items"},
        {"[1, 1.0]", R"({"uniqueItems": true})", "$: duplicate item 1"},
        {R"([{"a": 1, "b": [2]}, {"b": [2], "a": 1}])", R"({"uniqueItems": true})", "$: duplicate item"},
        {R"([1, "x"])", R"({"items": {"type": "integer"}})", "$[1]: expected integer, got string"},
        {R"({"a": [{"b": "x"}, {"b": 3}]})", R"({"properties": {"a": {"items": {"properties": {"b": {"type": "string"}}}}}})",
         "$.a[1].b: expected string, got integer"},
        {"null", R"({"anyOf": [{"type": "string"}, {"type": "integer"}]})", "$: matches none of anyOf"},
        {"3", R"({"anyOf": [{"type": "string"}, {"type": "integer"}]})", ""},
        {"1", "false", "$: not allowed"},
        {"1", "true", ""},
        {"1", R"({"title": "t", "description": "d", "$comment": "c"})", ""},
        {"1", R"({"format": "date"})", "schema at $: unsupported keyword format"},
        {"1", R"({"$ref": "\u0023/$defs/missing"})", "schema at $: unresolved $ref"},
        {"1", R"({"$ref": "\u0023/$defs/loop"})", "nested too deeply"},
        {R"("x")", R"({"pattern": "("})", "schema at $: pattern ("},
    };
    for (const Case& c : cases) {
        const QStringList errs = data::validate(J(c.value), J(c.schema), root);
        const QString expect = QString::fromUtf8(c.expect);
        if (expect.isEmpty()) QVERIFY2(errs.isEmpty(), qPrintable(QStringLiteral("%1 vs %2: %3").arg(c.value, c.schema, errs.join(u"; "))));
        else QVERIFY2(errs.join(u"\n").contains(expect), qPrintable(QStringLiteral("%1 vs %2: want '%3', got '%4'").arg(c.value, c.schema, expect, errs.join(u"; "))));
    }
    // maxErrors caps the list
    QJsonArray many;
    for (int i = 0; i < 100; ++i) many.append(QStringLiteral("x"));
    QCOMPARE(data::validate(many, J(R"({"items": {"type": "integer"}})"), root, 5).size(), 5);

    // A damaged copy of a real file fails with paths that say where.
    QJsonObject styles = data::load(u"styles");
    QJsonArray list = styles.value(u"styles").toArray();
    QJsonObject first = list.at(0).toObject();
    first.insert(QStringLiteral("transition"), QStringLiteral("wipe"));
    first.insert(QStringLiteral("extra"), 1);
    list.replace(0, first);
    styles.insert(QStringLiteral("styles"), list);
    styles.remove(QStringLiteral("_comment"));
    styles.insert(QStringLiteral("version"), 2);
    const QString errs = data::validate(styles, QJsonObject{{QStringLiteral("$ref"), defRef("styles")}}, data::schema()).join(u"\n");
    for (const char* want : {"$.styles[0].transition: \"wipe\" is not one of", "$.styles[0].extra: not allowed", "$: missing _comment", "$.version: expected 1"})
        QVERIFY2(errs.contains(QLatin1StringView(want)), qPrintable(errs));
    // and the untouched file passes the same call
    QVERIFY(data::validate(data::load(u"styles"), QJsonObject{{QStringLiteral("$ref"), defRef("styles")}}, data::schema()).isEmpty());
}

void TestStudioCore::dataRegex() {
    // ECMAScript meaning, case by case (the JS results were checked with node).
    struct Case { const char16_t* src; const char16_t* flags; QString input; bool match; };
    const Case cases[] = {
        {u"a$", u"", QStringLiteral("a"), true},
        {u"a$", u"", QStringLiteral("a\n"), false},
        {u"^a$", u"m", QStringLiteral("x\na"), true},
        {u"^a$", u"m", QStringLiteral("x\ra\u2028y"), true},
        {u"^a$", u"m", QStringLiteral("xa"), false},
        {u"^.$", u"", QStringLiteral("\r"), false},
        {u"^.$", u"", QStringLiteral("\u2028"), false},
        {u"^.$", u"", QStringLiteral("x"), true},
        {u"^.$", u"s", QStringLiteral("\n"), true},
        {u"^\\s$", u"", QStringLiteral("\u00a0"), true},
        {u"^\\s$", u"", QStringLiteral("\ufeff"), true},
        {u"^\\s$", u"", QStringLiteral("\u3000"), true},
        {u"^\\S$", u"", QStringLiteral("\u00a0"), false},
        {u"^\\S$", u"", QStringLiteral("x"), true},
        {u"^[\\s,]+$", u"", QStringLiteral(",\u00a0 "), true},
        {u"^[^]$", u"", QStringLiteral("\n"), true},
        {u"[]", u"", QStringLiteral(""), false},
        {u"a[]", u"", QStringLiteral("a"), false},
        {u"^\\u00e9$", u"", QStringLiteral("\u00e9"), true},
        {u"^\\u{1F600}$", u"u", QStringLiteral("\U0001F600"), true},
        {u"^\\uD83D\\uDE00$", u"", QStringLiteral("\U0001F600"), true},
        {u"^\\p{Lu}$", u"u", QStringLiteral("\u00c9"), true},
        {u"^\\p{Lu}$", u"u", QStringLiteral("\u00e9"), false},
        {u"^[[]$", u"", QStringLiteral("["), true},
        {u"^[[:a]+$", u"", QStringLiteral(":a["), true},
        {u"^\\/$", u"", QStringLiteral("/"), true},
        {u"^\\a\\e\\z$", u"", QStringLiteral("aez"), true}, // identity escapes without u
        {u"^\\v$", u"", QStringLiteral("\v"), true},
        {u"^\\v$", u"", QStringLiteral("\n"), false},
        {u"^\\x41$", u"", QStringLiteral("A"), true},
        {u"^\\0$", u"", QString(QChar(0)), true},
        {u"^abc$", u"i", QStringLiteral("ABC"), true},
        {u"^abc$", u"gyd", QStringLiteral("ABC"), false},
        {u"\\bcat\\b", u"", QStringLiteral("a cat!"), true},
        {u"\\bcat\\b", u"", QStringLiteral("concat"), false},
        {u"(?<y>\\d{4})-\\k<y>", u"", QStringLiteral("2026-2026"), true},
        {u"(?<!x)y", u"", QStringLiteral("zy"), true},
        {u"(?<!x)y", u"", QStringLiteral("xy"), false},
    };
    for (const Case& c : cases) {
        QString err;
        const QRegularExpression re = data::regex(QStringView(c.src), QStringView(c.flags), &err);
        const QString what = QStringLiteral("/%1/%2 -> %3").arg(QStringView(c.src), QStringView(c.flags), re.pattern());
        QVERIFY2(re.isValid() && err.isEmpty(), qPrintable(what + u": " + err));
        QVERIFY2(re.match(c.input).hasMatch() == c.match, qPrintable(what + u" on " + escaped(c.input.toUtf8())));
    }
    QCOMPARE(data::jsToPcre(u"a$", u""), QStringLiteral("a\\z"));
    QCOMPARE(data::jsToPcre(u"[^]", u""), QStringLiteral("[\\s\\S]"));

    // What has no exact PCRE2 form, or is not a valid JS pattern, is refused: invalid, never matches, with a reason.
    const std::pair<const char16_t*, const char16_t*> refused[] = {
        {u"\\uD83D", u""}, {u"\\uDE00", u""}, {u"\\p{L}", u""}, {u"\\a", u"u"}, {u"\\u{110000}", u"u"}, {u"[\\S]", u""}, {u"[abc", u""},
        {u"abc\\", u""}, {u"a", u"v"}, {u"a", u"x"}, {u"a", u"ii"}, {u"(", u""}, {u"\\08", u""},
    };
    for (const auto& [src, flags] : refused) {
        QString err;
        const QRegularExpression re = data::regex(QStringView(src), QStringView(flags), &err);
        QVERIFY2(!re.isValid() && !err.isEmpty(), qPrintable(QStringLiteral("/%1/%2").arg(QStringView(src), QStringView(flags))));
        QVERIFY(!matches(re, QStringLiteral("a")));
    }
    QString err;
    QVERIFY(!data::regex(QJsonValue(QStringLiteral("a")), &err).isValid());
    QVERIFY(err.contains(QStringLiteral("{source, flags}")));
    QVERIFY(data::regex(J(R"({"source": "^a", "flags": "i"})"), &err).match(QStringLiteral("Ab")).hasMatch());
}

void TestStudioCore::dataPatternsFromStarmedia() {
    // Every pattern in the files compiles with its JS meaning ...
    int count = 0;
    QStringList bad;
    auto compile = [&](const QJsonValue& p, const QString& where) {
        QString err;
        if (!data::regex(p, &err).isValid()) bad << where + u": " + err;
        ++count;
    };
    const QJsonObject ml = data::load(u"model-limits"), rp = data::load(u"realpeople"), mp = data::load(u"model-prompts");
    for (const QJsonValue& c : ml.value(u"requestCaps").toArray()) compile(c[u"pattern"], QStringLiteral("requestCaps"));
    const QJsonObject pats = rp.value(u"patterns").toObject();
    for (auto it = pats.begin(); it != pats.end(); ++it) compile(it.value(), u"realpeople." + it.key());
    for (const QJsonValue& p : mp.value(u"enhance")[u"positive"].toArray()) compile(p[u"pattern"], QStringLiteral("positive"));
    for (const QJsonValue& g : mp.value(u"spotFallback")[u"guides"].toArray()) compile(g[u"pattern"], QStringLiteral("spotFallback"));
    for (const QJsonValue& g : mp.value(u"guides").toArray())
        for (const QJsonValue& m : g[u"match"].toArray()) {
            QString err;
            if (!data::regex(m.toString(), u"i", &err).isValid()) bad << g[u"family"].toString() + u": " + err;
            ++count;
        }
    QVERIFY2(bad.isEmpty(), qPrintable(bad.join(u"; ")));
    QVERIFY(count >= 70);

    // ... and answers what the TypeScript answers (results checked with node on the same inputs).
    const QRegularExpression title = data::regex(pats.value(u"titleName"));
    const QRegularExpressionMatch tm = title.match(QStringLiteral("yesterday Senator Jane Doe spoke"));
    QVERIFY(tm.hasMatch());
    QCOMPARE(tm.captured(1), QStringLiteral("Jane Doe spoke")); // the i flag also relaxes [A-Z], as in JS
    const QRegularExpression notPerson = data::regex(pats.value(u"notPersonWord"));
    QVERIFY(matches(notPerson, QStringLiteral("Street")));
    QVERIFY(!matches(notPerson, QStringLiteral("Street\n"))); // JS $ is the very end
    QVERIFY(!matches(notPerson, QStringLiteral("Streetcar")));
    QVERIFY(matches(data::regex(pats.value(u"photoWords")), QStringLiteral("shot on 35\u00a0mm"))); // JS \s takes U+00A0
    QCOMPARE(data::regex(pats.value(u"realWords")).match(QStringLiteral("a deep-fake of")).captured(0), QStringLiteral("deep-fake"));
    QVERIFY(matches(data::regex(pats.value(u"stylized")), QStringLiteral("a watercolour of a lake")));
    QVERIFY(!matches(data::regex(pats.value(u"stylized")), QStringLiteral("a photo of a lake")));

    QCOMPARE(firstMatching(mp.value(u"guides").toArray(), QStringLiteral("google/veo-3.1-fast"), QStringLiteral("video"))
                 .value(u"family").toString(), QStringLiteral("veo-3.1"));
    QCOMPARE(firstMatching(mp.value(u"guides").toArray(), QStringLiteral("openai/gpt-image-1"), QStringLiteral("image"))
                 .value(u"family").toString(), QStringLiteral("gpt-image"));
    QVERIFY(firstMatching(mp.value(u"guides").toArray(), QStringLiteral("nobody/unknown-model"), QStringLiteral("image")).isEmpty());
    QCOMPARE(firstMatching(mp.value(u"spotFallback")[u"guides"].toArray(), QStringLiteral("black-forest-labs/flux-1"))
                 .value(u"family").toString(), QStringLiteral("FLUX"));
    const QJsonObject cap = firstMatching(ml.value(u"requestCaps").toArray(), QStringLiteral("sourceful/riverflow-v2-pro"));
    QCOMPARE(cap.value(u"bytes").toInteger(), qint64(4.5 * 1024 * 1024));
    QVERIFY(firstMatching(ml.value(u"requestCaps").toArray(), QStringLiteral("openai/sourceful")).isEmpty());
}

void TestStudioCore::dataMatchesStarmedia() {
    const QString root = qEnvironmentVariable("STARMEDIA_ROOT");
    if (root.isEmpty() || !QFileInfo::exists(root + u"/src/limits.ts")) QSKIP("no starmedia checkout (STARMEDIA_ROOT): the export was not compared");

    // 1. Every TS file still has the text the committed export was made from (CRLF folded, as the exporter hashes it).
    QStringList drift;
    for (const QString& name : data::names())
        for (const QJsonValue& f : data::load(name).value(u"source")[u"files"].toArray()) {
            const QString rel = f[u"path"].toString();
            const QByteArray text = lfText(fileBytes(root + u'/' + rel));
            if (text.isEmpty() || json::sha256Hex(text) != f[u"sha256"].toString().toLatin1()) drift << name + u".json <- " + rel;
        }
    QVERIFY2(drift.isEmpty(), qPrintable(QStringLiteral("starmedia changed since the export (%1): re-run node --experimental-strip-types "
                                                        "tools/studio/scripts/export-starmedia-data.mjs and rebuild").arg(drift.join(u", "))));

    // 2. A fresh export into a scratch dir equals the committed files byte for byte (Node 22.6+ only, dev machines).
    const QString node = QStandardPaths::findExecutable(QStringLiteral("node"));
    QProcess version;
    if (!node.isEmpty()) {
        version.start(node, {QStringLiteral("--version")});
        version.waitForFinished(15000);
    }
    const QStringList v = QString::fromLatin1(version.readAllStandardOutput()).trimmed().mid(1).split(u'.');
    const int major = v.value(0).toInt(), minor = v.value(1).toInt();
    if (node.isEmpty() || major < 22 || (major == 22 && minor < 6)) {
        qWarning("no Node 22.6+ on PATH: the re-export comparison was not run (the source hashes matched)");
        return;
    }
    QTemporaryDir out;
    QVERIFY(out.isValid());
    QProcess p;
    auto reap = qScopeGuard([&p]() { if (p.state() != QProcess::NotRunning) { p.kill(); p.waitForFinished(5000); } }); // our own child only
    p.start(node, {QStringLiteral("--experimental-strip-types"), QStringLiteral("--no-warnings"),
                   QStringLiteral(STUDIO_SOURCE_DIR "/scripts/export-starmedia-data.mjs"), root, out.path()});
    QVERIFY2(p.waitForStarted(15000), qPrintable(p.errorString()));
    QVERIFY2(p.waitForFinished(60000), "the exporter did not finish within 60 s");
    const QString log = escaped(p.readAllStandardError().right(1500));
    QVERIFY2(p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0,
             qPrintable(QStringLiteral("the exporter failed (exit %1):\n%2").arg(p.exitCode()).arg(log)));
    QCOMPARE(QDir(out.path()).entryList(QDir::Files, QDir::Name),
             (QStringList{QStringLiteral("model-limits.json"), QStringLiteral("model-prompts.json"), QStringLiteral("realpeople.json"),
                          QStringLiteral("styles.json")}));
    for (const QString& name : data::names()) {
        const QByteArray fresh = fileBytes(out.filePath(name + u".json"));
        const QByteArray committed = lfText(fileBytes(QStringLiteral(STUDIO_SOURCE_DIR "/data/") + name + u".json"));
        QVERIFY2(!fresh.isEmpty() && fresh == committed,
                 qPrintable(name + u".json differs from a fresh export (re-run the exporter); " + firstDifference(committed, fresh)));
    }
}

void TestStudioCore::cleanupTestCase() { log::stop(); }

QTEST_GUILESS_MAIN(TestStudioCore)
#include "test_studio_core.moc"
