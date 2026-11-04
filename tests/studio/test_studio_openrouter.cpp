// test_studio_openrouter (docs/plans/starmedia-studio.md §8). ST-05 part: the mock OpenRouter itself
// (tests/studio/mockopenrouter.*), so the provider client tests that ST-07 adds to this file stand on a verified mock.
//   - the fixture catalog is the one starmedia test/mock.ts serves (ids in order, checked against the TS source when the
//     checkout is on disk; prices parse with money::parseRate, exponent strings included);
//   - TS parity: every answer recorded from the real TS mock (fixtures/mock-replies.json, export-mock-fixture.mjs) is
//     answered the same way by the C++ mock (every TS key present with the same value; binary answers by sha256);
//   - chat: canned json_schema answers, a test's own handler, answers synthesized from the request's schema, tool calls
//     (tool_choice auto / named / required / none, the final answer after a role:"tool" message), a scripted brain,
//     text answers, usage cost and /generation;
//   - media: images (n, custom bytes), video submit -> scripted poll statuses -> content (polling_url styles, failed
//     jobs, unknown ids), speech (byte-identical to the TS WAV), transcriptions (duration from the WAV, json vs
//     verbose_json, the verbose_json refusal), embeddings, rerank;
//   - scripted failures: 429 with Retry-After seconds / retry-after-ms / an HTTP date, the daily quota with
//     X-RateLimit-Reset, 402, 503, 502 with provider metadata, the spend cap, 401 with a required key, method and prefix
//     patterns, counts, failPath;
//   - delivery faults: slow bodies (timed), headers that come too late, a body that stalls (in flight until released or
//     the client gives up), a truncated body, a dropped and a reset connection;
//   - concurrency: peak in-flight from the server side, the concurrency limit (429 with retry-after-ms), a mock on a
//     thread of its own answering a client that blocks this thread;
//   - framing: a 20 MiB upload (recorded size, truncated log copy), chunked uploads, Expect: 100-continue, the body cap
//     (413, still recorded), pipelining and keep-alive, protocol errors.
// ST-07a part: the client (provider/wire, openrouter, limits, payload) against that mock: chat with tools and
// json_schema, JSON salvage (TS goldens), a timed-out / broken paid POST marked unknown and never re-sent (one-shot
// transport), oversize bodies refused before sending, limit answers and retries, Stop, key hygiene, HTTP/2 logging.
// Everything runs on ephemeral loopback ports; a test key, no network, no spend.
#include <QtTest>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>

#include <QSignalSpy>
#include <QTcpServer>

#include "core/clock.h"
#include "core/config.h"
#include "core/datafiles.h"
#include "core/json.h"
#include "core/log.h"
#include "core/money.h"
#include "mockopenrouter.h"
#include "provider/gates.h"
#include "provider/limits.h"
#include "provider/modelhealth.h"
#include "provider/openrouter.h"
#include "provider/payload.h"
#include "provider/wire.h"
#include "safety/consent.h"

using namespace studio;
using namespace studio::test;
using studio::provider::ChatRequest;
using studio::provider::ChatResult;
using studio::provider::Request;
using studio::provider::Response;
using studio::provider::WireError;

namespace {

// The ids of FIXTURE_MODELS in starmedia test/mock.ts (L18-35), in order.
const QStringList kTsIds = {
    QStringLiteral("acme/writer-pro"), QStringLiteral("beta/writer-2"), QStringLiteral("epsilon/deep"), QStringLiteral("gamma/fast-mini"),
    QStringLiteral("zeta/lite"), QStringLiteral("delta/free-model:free"), QStringLiteral("acme/img-1"), QStringLiteral("beta/img-free"),
    QStringLiteral("acme/vid-1"), QStringLiteral("acme/tts-1"), QStringLiteral("acme/tts-clone"), QStringLiteral("acme/audio-gen"),
    QStringLiteral("beta/tts-free:free"), QStringLiteral("acme/stt-1"), QStringLiteral("acme/embed-1"), QStringLiteral("acme/rerank-1")};

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QJsonObject jsonOf(const QByteArray& b) { return QJsonDocument::fromJson(b).object(); }
QByteArray sha(const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex(); }

struct Got {
    int status = 0;
    QByteArray body;
    QHash<QByteArray, QByteArray> headers;   // lower-case names
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    qint64 ms = 0;
    QJsonObject json() const { return jsonOf(body); }
    QByteArray h(const char* name) const { return headers.value(name); }
};

Got call(QNetworkAccessManager& nam, const QByteArray& verb, const QString& url, const QByteArray& body = {}, int timeoutMs = 10000,
         const HeaderList& extra = {}) {
    QNetworkRequest rq{QUrl(url)};
    rq.setTransferTimeout(timeoutMs);
    if (!body.isEmpty()) rq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    for (const auto& h : extra) rq.setRawHeader(h.first, h.second);
    QElapsedTimer t;
    t.start();
    QNetworkReply* r = nam.sendCustomRequest(rq, verb, body);
    (void)QTest::qWaitFor([r]() { return r->isFinished(); }, timeoutMs + 5000);
    Got g;
    g.status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    g.error = r->error();
    if (r->isOpen()) g.body = r->readAll();
    for (const auto& p : r->rawHeaderPairs()) g.headers.insert(p.first.toLower(), p.second);
    g.ms = t.elapsed();
    r->deleteLater();
    return g;
}
Got get(QNetworkAccessManager& nam, const QString& url, int timeoutMs = 10000) { return call(nam, "GET", url, {}, timeoutMs); }
Got post(QNetworkAccessManager& nam, const QString& url, const QJsonObject& body, int timeoutMs = 10000, const HeaderList& extra = {}) {
    return call(nam, "POST", url, compact(body), timeoutMs, extra);
}

struct Resp {
    int status = 0;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
    QByteArray h(const char* name) const { return headers.value(name); }
};

// One response from the front of `buf`; returns the bytes it used (0 = not complete yet). 1xx answers are skipped.
qsizetype parseResponse(const QByteArray& buf, Resp* r) {
    qsizetype start = 0;
    for (;;) {
        const qsizetype end = buf.indexOf("\r\n\r\n", start);
        if (end < 0) return 0;
        Resp out;
        const QList<QByteArray> lines = buf.mid(start, end - start).split('\n');
        out.status = lines.value(0).trimmed().split(' ').value(1).toInt();
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype c = lines[i].indexOf(':');
            if (c > 0) out.headers.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
        }
        if (out.status >= 100 && out.status < 200) {
            start = end + 4;
            continue;
        }
        const qint64 len = out.headers.value("content-length").toLongLong();
        if (buf.size() < end + 4 + len) return 0;
        out.body = buf.mid(end + 4, len);
        *r = out;
        return end + 4 + len;
    }
}

// A raw client connection; waits spin the event loop.
class Raw {
public:
    explicit Raw(quint16 port) {
        QObject::connect(&s, &QTcpSocket::readyRead, &s, [this]() { read(); });
        s.connectToHost(QHostAddress::LocalHost, port);
        (void)QTest::qWaitFor([this]() { return s.state() == QAbstractSocket::ConnectedState; }, 3000);
        clock.start();
    }
    void send(const QByteArray& b) {
        clock.restart();
        headAtMs = -1;
        s.write(b);
    }
    bool next(Resp* r, int ms = 5000) {
        qsizetype used = 0;
        const bool ok = QTest::qWaitFor([&]() {
            read();
            used = parseResponse(buf, r);
            return used > 0;
        }, ms);
        if (ok) buf.remove(0, used);
        return ok;
    }
    bool waitClosed(int ms = 5000) {
        return QTest::qWaitFor([this]() {
            read();
            return s.state() == QAbstractSocket::UnconnectedState;
        }, ms);
    }
    void read() {
        buf += s.readAll();
        if (headAtMs < 0 && buf.contains("\r\n\r\n")) headAtMs = clock.elapsed();
    }
    QTcpSocket s;
    QByteArray buf;
    QElapsedTimer clock;
    qint64 headAtMs = -1;
};

QByteArray rawGet(const QByteArray& path, const QByteArray& extra = {}) {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n";
}
QByteArray rawPost(const QByteArray& path, const QByteArray& body, const QByteArray& extra = {}) {
    return "POST " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n" +
           extra + "\r\n" + body;
}

// a 16-bit mono WAV of `seconds` at `rate`
QByteArray makeWav(double seconds, int rate = 16000) {
    const int n = int(seconds * rate);
    QByteArray b(44 + n * 2, '\0');
    char* p = b.data();
    memcpy(p, "RIFF", 4);
    qToLittleEndian<quint32>(quint32(36 + n * 2), p + 4);
    memcpy(p + 8, "WAVEfmt ", 8);
    qToLittleEndian<quint32>(16, p + 16);
    qToLittleEndian<quint16>(1, p + 20);
    qToLittleEndian<quint16>(1, p + 22);
    qToLittleEndian<quint32>(quint32(rate), p + 24);
    qToLittleEndian<quint32>(quint32(rate * 2), p + 28);
    qToLittleEndian<quint16>(2, p + 32);
    qToLittleEndian<quint16>(16, p + 34);
    memcpy(p + 36, "data", 4);
    qToLittleEndian<quint32>(quint32(n * 2), p + 40);
    return b;
}

// Every key of `ts` is in `cpp` with the same value; "{base}" in TS strings stands for the mock's url.
bool subset(const QJsonValue& ts, const QJsonValue& cpp, const QString& base, const QString& path, const QSet<QString>& ignore, QString* why) {
    if (ignore.contains(path)) return true;
    if (ts.isObject()) {
        if (!cpp.isObject()) {
            *why = path + QStringLiteral(": not an object");
            return false;
        }
        const QJsonObject a = ts.toObject(), b = cpp.toObject();
        for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
            if (!b.contains(it.key()) && !ignore.contains(path + QLatin1Char('/') + it.key())) {
                *why = path + QLatin1Char('/') + it.key() + QStringLiteral(": missing");
                return false;
            }
            if (!subset(it.value(), b.value(it.key()), base, path + QLatin1Char('/') + it.key(), ignore, why)) return false;
        }
        return true;
    }
    if (ts.isArray()) {
        const QJsonArray a = ts.toArray(), b = cpp.toArray();
        if (!cpp.isArray() || a.size() != b.size()) {
            *why = path + QStringLiteral(": array size %1 vs %2").arg(a.size()).arg(b.size());
            return false;
        }
        for (qsizetype i = 0; i < a.size(); ++i)
            if (!subset(a.at(i), b.at(i), base, path + QStringLiteral("/%1").arg(i), ignore, why)) return false;
        return true;
    }
    if (ts.isString()) {
        // JSON inside a string (a chat answer's content): compare it parsed, since JS keeps insertion order and Qt
        // writes keys sorted
        if (cpp.isString() && (ts.toString().startsWith(QLatin1Char('{')) || ts.toString().startsWith(QLatin1Char('[')))) {
            const QJsonDocument a = QJsonDocument::fromJson(ts.toString().toUtf8()), b = QJsonDocument::fromJson(cpp.toString().toUtf8());
            if (!a.isNull() && !b.isNull()) {
                const QJsonValue av = a.isObject() ? QJsonValue(a.object()) : QJsonValue(a.array());
                const QJsonValue bv = b.isObject() ? QJsonValue(b.object()) : QJsonValue(b.array());
                return subset(av, bv, base, path + QStringLiteral("(json)"), ignore, why);
            }
        }
        const QString want = ts.toString().replace(QLatin1String("{base}"), base);
        if (cpp.toString() != want || !cpp.isString()) {
            *why = path + QStringLiteral(": \"%1\" vs \"%2\"").arg(want.left(80), cpp.toString().left(80));
            return false;
        }
        return true;
    }
    if (ts.isDouble()) {
        if (!cpp.isDouble() || !(ts.toDouble() == cpp.toDouble() || qFuzzyCompare(ts.toDouble(), cpp.toDouble()))) {
            *why = path + QStringLiteral(": %1 vs %2").arg(ts.toDouble(), 0, 'g', 17).arg(cpp.toDouble(), 0, 'g', 17);
            return false;
        }
        return true;
    }
    if (ts != cpp) {
        *why = path + QStringLiteral(": value differs");
        return false;
    }
    return true;
}

QJsonObject chatBody(const QString& text, const QJsonObject& extra = {}) {
    QJsonObject b{{QStringLiteral("model"), QStringLiteral("acme/writer-pro")},
                  {QStringLiteral("messages"), QJsonArray{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), text}}}}};
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) b.insert(it.key(), it.value());
    return b;
}

QJsonObject schemaFormat(const QString& name, const QJsonObject& schema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                       {QStringLiteral("json_schema"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("strict"), true}, {QStringLiteral("schema"), schema}}}};
}

QJsonObject messageOf(const Got& g) { return g.json().value(QStringLiteral("choices")).toArray().at(0).toObject().value(QStringLiteral("message")).toObject(); }
QString finishOf(const Got& g) { return g.json().value(QStringLiteral("choices")).toArray().at(0).toObject().value(QStringLiteral("finish_reason")).toString(); }
QJsonObject contentJson(const Got& g) { return jsonOf(messageOf(g).value(QStringLiteral("content")).toString().toUtf8()); }

QJsonObject fn(const QString& name, const QJsonObject& params) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("function")},
                       {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("description"), QStringLiteral("test tool")}, {QStringLiteral("parameters"), params}}}};
}

// ---------------- ST-07a: the client ----------------

// a key-shaped test value (never a real key): the mock checks it arrives, the log scrubber knows its shape
const QByteArray kTestKey = "sk-or-v1-0000studio0test0key0000000000000000000000000000000000000000";

// One provider stack per test: health, gates, wire and client on the real clock, quick back-off and no jitter.
struct Client {
    explicit Client(Config& cfg, const QString& base, Clock& clock = Clock::system()) : health(clock), gates(clock), wire(cfg, gates, clock), api(wire) {
        provider::WireOptions o;
        o.baseUrl = base;
        o.backoffBaseMs = 20;
        o.backoffJitterMs = 0;
        o.busyWaitMs = 20;
        o.rand = []() { return 0.0; };
        wire.setOptions(o);
        wire.setHealth(&health);
        gates.setHealth(&health);
    }
    void options(const std::function<void(provider::WireOptions&)>& f) {
        provider::WireOptions o = wire.options();
        f(o);
        wire.setOptions(o);
    }
    provider::ModelHealth health;
    provider::Gates gates;
    provider::Wire wire;
    provider::OpenRouter api;
};

struct Outcome {
    bool done = false;
    std::optional<Response> resp;
    std::optional<WireError> err;
    ChatResult chat;
    QJsonValue data;
    qint64 ms = 0;
    QElapsedTimer t;
};
using OutcomeP = std::shared_ptr<Outcome>;

OutcomeP wireSend(provider::Wire& w, Request rq, QObject* ctx) {
    auto o = std::make_shared<Outcome>();
    o->t.start();
    w.request(std::move(rq), ctx, [o](const Response& r) { o->resp = r; o->ms = o->t.elapsed(); o->done = true; },
              [o](const WireError& e) { o->err = e; o->ms = o->t.elapsed(); o->done = true; });
    return o;
}
OutcomeP chatCall(provider::OpenRouter& api, const ChatRequest& rq, QObject* ctx) {
    auto o = std::make_shared<Outcome>();
    o->t.start();
    api.chat(rq, ctx, [o](const ChatResult& r) { o->chat = r; o->ms = o->t.elapsed(); o->done = true; },
             [o](const WireError& e) { o->err = e; o->ms = o->t.elapsed(); o->done = true; });
    return o;
}
bool wait(const OutcomeP& o, int ms = 15000) { return QTest::qWaitFor([&]() { return o->done; }, ms); }

Request postRq(const QString& path, const QJsonObject& body, qint64 timeoutMs = 10000, int retries = 3) {
    Request rq;
    rq.method = "POST";
    rq.path = path;
    rq.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    rq.timeoutMs = timeoutMs;
    rq.retries = retries;
    const QString model = body.value(QStringLiteral("model")).toString();
    rq.model = rq.quotaKey = model;
    return rq;
}

ChatRequest ask(const QString& text, const QString& model = QStringLiteral("acme/writer-pro")) {
    ChatRequest r;
    r.models = {model};
    r.user = text;
    return r;
}

// Sets an environment variable for the test and puts the old value back (or unsets it) afterwards.
class EnvGuard {
public:
    void set(const char* name, const QByteArray& value) {
        save(name);
        qputenv(name, value);
    }
    void unset(const char* name) {
        save(name);
        qunsetenv(name);
    }
    ~EnvGuard() {
        for (auto it = m_old.rbegin(); it != m_old.rend(); ++it) {
            if (it->had) qputenv(it->name.constData(), it->value);
            else qunsetenv(it->name.constData());
        }
    }

private:
    void save(const char* name) {
        for (const auto& o : m_old)
            if (o.name == name) return;
        m_old.push_back({name, qgetenv(name), qEnvironmentVariableIsSet(name)});
    }
    struct Old { QByteArray name; QByteArray value; bool had; };
    std::vector<Old> m_old;
};

quint16 closedPort() {
    QTcpServer s;
    if (!s.listen(QHostAddress::LocalHost, 0)) return 1;
    const quint16 p = s.serverPort();
    s.close();
    return p;
}


// ---------------- ST-07b helpers ----------------

// makeWav (a 16-bit mono WAV of `seconds`) is the helper above (line ~202); the ST-07b tests share it.

// A stored, confirmed consent record for a cloned voice (the shape test_studio_safety builds), valid at kConsentNow.
const qint64 kConsentNow = QDateTime(QDate(2026, 10, 26), QTime(12, 0), QTimeZone::UTC).toMSecsSinceEpoch();
safety::CloneUse consentingVoice(const QString& text) {
    safety::ConsentInput in{QStringLiteral("Jane Doe"), QStringLiteral("I, Jane Doe, agree to StarMedia cloning my voice for narration of our station promos."),
                            QStringLiteral("Jane Doe"), QStringLiteral("2026-09-30")};
    safety::ConsentRecord r;
    const auto refused = safety::checkConsent(in, kConsentNow, &r);
    if (refused) qFatal("test consent refused: %s", qPrintable(refused->message));
    r.recordedAt = QStringLiteral("2026-10-01T10:00:00.000Z");
    r.confirmedByUser = true;
    r.session = QStringLiteral("ui");
    return safety::CloneUse{QStringLiteral("Jane narration"), r.toJson(), QStringLiteral("Hello, this is my sample."), text, false};
}

// one media call's outcome
template <class T>
struct Got2 {
    bool done = false;
    T res;
    std::optional<WireError> err;
};

}   // namespace

class TestStudioOpenRouter : public QObject {
    Q_OBJECT

    MockOpenRouter m_mock;
    QNetworkAccessManager m_nam;
    QString u(const QString& path) const { return m_mock.url() + path; }
    // ST-07a: the client reads its key and base from the environment at call time. The machine's real key is replaced
    // by a test value for the whole run (restored in cleanupTestCase), and the base points at the mock, so nothing can
    // reach openrouter.ai with a real key.
    EnvGuard m_env;
    Config m_cfg;   // in memory (no data dir)

private slots:
    void initTestCase() {
        QVERIFY2(m_mock.start(), qPrintable(m_mock.errorString()));
        QVERIFY(m_mock.isRunning());
        QVERIFY(m_mock.port() > 0);
        QCOMPARE(m_mock.url(), QStringLiteral("http://127.0.0.1:%1").arg(m_mock.port()));
        QCOMPARE(m_mock.apiUrl(), m_mock.url() + QStringLiteral("/api/v1"));
        m_env.set("OPENROUTER_API_KEY", kTestKey);
        m_env.set("OPENROUTER_BASE_URL", m_mock.apiUrl().toUtf8());
        m_env.unset("OPENROUTER_TITLE");
        m_env.unset("OPENROUTER_REFERER");
        m_env.unset("STARMEDIA_PROVIDER_TOKEN");
        QCOMPARE(m_cfg.openRouterBaseUrl(), m_mock.apiUrl());
    }

    void init() {
        m_mock.clearScripts();
        m_mock.resetStats();
    }

    // ---------------- ST-05: the mock OpenRouter ----------------

    void fixtureMatchesTsMock() {
        QCOMPARE(m_mock.modelIds(), kTsIds);
        QJsonObject catalog;
        {
            QFile f(m_mock.fixturesDir() + QStringLiteral("/models.json"));
            QVERIFY(f.open(QIODevice::ReadOnly));
            catalog = QJsonDocument::fromJson(f.readAll()).object();
        }
        QCOMPARE(catalog.value(QStringLiteral("source")).toObject().value(QStringLiteral("file")).toString(), QStringLiteral("test/mock.ts"));
        QVERIFY(!catalog.value(QStringLiteral("source")).toObject().value(QStringLiteral("commit")).toString().isEmpty());
        QCOMPARE(m_mock.replies().value(QStringLiteral("source")).toObject(), catalog.value(QStringLiteral("source")).toObject());

        // Against the TS source itself when the starmedia checkout is on this machine (the ids, in order).
        const QString root = qEnvironmentVariable("STARMEDIA_ROOT");
        QFile ts(root + QStringLiteral("/test/mock.ts"));
        if (!root.isEmpty() && ts.open(QIODevice::ReadOnly)) {
            const QString src = QString::fromUtf8(ts.readAll());
            const qsizetype a = src.indexOf(QLatin1String("export const FIXTURE_MODELS = ["));
            const qsizetype b = src.indexOf(QLatin1String("\n];"), a);
            QVERIFY(a >= 0 && b > a);
            QStringList ids;
            static const QRegularExpression rx(QStringLiteral(R"((?:textModel\(|\{ id: )'([^']+)')"));
            for (auto it = rx.globalMatch(src.mid(a, b - a)); it.hasNext();) ids << it.next().captured(1);
            QCOMPARE(ids, kTsIds);
            // text model prices in the source: textModel('id', inPerMillion, outPerMillion)
            static const QRegularExpression px(QStringLiteral(R"(textModel\('([^']+)', ([\d.]+), ([\d.]+))"));
            QHash<QString, QPair<double, double>> perMillion;
            for (auto it = px.globalMatch(src.mid(a, b - a)); it.hasNext();) {
                const auto m = it.next();
                perMillion.insert(m.captured(1), {m.captured(2).toDouble(), m.captured(3).toDouble()});
            }
            QCOMPARE(perMillion.size(), 6);
            for (const QJsonValue& v : m_mock.models()) {
                const QJsonObject m = v.toObject();
                if (!perMillion.contains(m.value(QStringLiteral("id")).toString())) continue;
                money::Pico in = -1, out = -1;
                QVERIFY(money::parseRate(m.value(QStringLiteral("pricing")).toObject().value(QStringLiteral("prompt")).toString(), &in));
                QVERIFY(money::parseRate(m.value(QStringLiteral("pricing")).toObject().value(QStringLiteral("completion")).toString(), &out));
                const auto want = perMillion.value(m.value(QStringLiteral("id")).toString());
                QCOMPARE(in, money::Pico(std::llround(want.first * 1e6)));     // $/M tokens -> pico-USD per token
                QCOMPARE(out, money::Pico(std::llround(want.second * 1e6)));
            }
        } else {
            qInfo("starmedia checkout not found at %s; compared with the built-in id list only", qPrintable(root));
        }

        // Every price string parses (the TS writes small rates as "1e-7"), and every model has a modality.
        int exponent = 0;
        for (const QJsonValue& v : m_mock.models()) {
            const QJsonObject m = v.toObject();
            QVERIFY(!m.value(QStringLiteral("architecture")).toObject().value(QStringLiteral("output_modalities")).toArray().isEmpty());
            for (const char* k : {"prompt", "completion"}) {
                const QString s = m.value(QStringLiteral("pricing")).toObject().value(QLatin1String(k)).toString();
                money::Pico p = -1;
                QVERIFY2(money::parseRate(s, &p) && p >= 0, qPrintable(s));
                if (s.contains(QLatin1Char('e'))) ++exponent;
            }
        }
        QVERIFY(exponent >= 4);   // gamma/fast-mini and zeta/lite: 1e-7, 4e-7, 5e-8, 2e-7
        const QJsonObject canned = m_mock.cannedJson();
        for (const char* name : {"research", "outline", "factcheck", "legal", "headlines", "seo", "social", "imagePrompts", "altText", "videoScript", "newsletter",
                                 "ad_dna", "ad_ideate", "ad_translate", "concepts", "script", "script_legacy", "review", "ref_dna", "brand_research", "market_research", "stage_dramatize"})
            QVERIFY2(canned.value(QLatin1String(name)).isObject(), name);
    }

    void catalogRoutes() {
        const Got all = get(m_nam, u(QStringLiteral("/models")));
        QCOMPARE(all.status, 200);
        QCOMPARE(all.h("content-type"), QByteArray("application/json"));
        QJsonArray data = all.json().value(QStringLiteral("data")).toArray();
        QCOMPARE(data.size(), 16);
        // created = now - age (writer-pro: 20 days, as the TS mock computes it at start)
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const qint64 created = qint64(data.at(0).toObject().value(QStringLiteral("created")).toDouble());
        QVERIFY2(qAbs(created - (now - 20 * 86400)) <= 5, qPrintable(QString::number(created)));
        // the same under /api/v1 with a query, and pinned to the fixture's reference time = the fixture verbatim
        QFile f(m_mock.fixturesDir() + QStringLiteral("/models.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject catalog = QJsonDocument::fromJson(f.readAll()).object();
        m_mock.setCatalogNow(qint64(catalog.value(QStringLiteral("reference_now")).toDouble()));
        const Got pinned = get(m_nam, m_mock.apiUrl() + QStringLiteral("/models?output_modalities=all"));
        m_mock.setCatalogNow(0);
        QCOMPARE(pinned.status, 200);
        QCOMPARE(pinned.json().value(QStringLiteral("data")).toArray(), catalog.value(QStringLiteral("models")).toArray());
        QCOMPARE(m_mock.last(QStringLiteral("/models"))->query, QStringLiteral("output_modalities=all"));
        QCOMPARE(m_mock.last(QStringLiteral("/models"))->target, QStringLiteral("/api/v1/models?output_modalities=all"));

        const auto cloning = [&](const QString& path) {
            return get(m_nam, u(path)).json().value(QStringLiteral("data")).toObject().value(QStringLiteral("endpoints")).toArray().at(0).toObject().value(QStringLiteral("supports_voice_cloning"));
        };
        QCOMPARE(cloning(QStringLiteral("/models/acme/tts-clone/endpoints")), QJsonValue(true));
        QCOMPARE(cloning(QStringLiteral("/models/acme/audio-gen/endpoints")), QJsonValue(true));
        QCOMPARE(cloning(QStringLiteral("/models/acme/tts-1/endpoints")), QJsonValue(false));
        QCOMPARE(cloning(QStringLiteral("/api/v1/models/acme%2Ftts-clone/endpoints")), QJsonValue(true));   // an encoded slash

        QCOMPARE(get(m_nam, u(QStringLiteral("/images/models"))).json().value(QStringLiteral("data")).toArray().size(), 2);
        const auto imgCost = [&](const QString& id) {
            const QJsonObject o = get(m_nam, u(QStringLiteral("/images/models/%1/endpoints").arg(id))).json();
            return qMakePair(o.value(QStringLiteral("id")).toString(),
                             o.value(QStringLiteral("endpoints")).toArray().at(0).toObject().value(QStringLiteral("pricing")).toArray().at(0).toObject().value(QStringLiteral("cost_usd")).toDouble(-1));
        };
        QCOMPARE(imgCost(QStringLiteral("acme/img-1")), qMakePair(QStringLiteral("acme/img-1"), 0.04));
        QCOMPARE(imgCost(QStringLiteral("beta/img-free")), qMakePair(QStringLiteral("beta/img-free"), 0.0));
        QCOMPARE(imgCost(QStringLiteral("other/new-img")), qMakePair(QStringLiteral("other/new-img"), 0.04));   // any id, like TS

        const QJsonObject vid = get(m_nam, u(QStringLiteral("/videos/models"))).json().value(QStringLiteral("data")).toArray().at(0).toObject();
        QCOMPARE(vid.value(QStringLiteral("id")).toString(), QStringLiteral("acme/vid-1"));
        QCOMPARE(vid.value(QStringLiteral("supported_durations")).toArray(), (QJsonArray{4, 8}));
        QCOMPARE(vid.value(QStringLiteral("pricing_skus")).toObject().value(QStringLiteral("duration_seconds_720p")).toString(), QStringLiteral("0.10"));
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).json().value(QStringLiteral("data")).toObject().value(QStringLiteral("total_credits")).toDouble(), 10.0);
        QCOMPARE(m_mock.models().size(), 16);
    }

    void tsParity() {
        // A fresh mock, so job ids start at job1 as in the TS recording.
        MockOpenRouter mock;
        QVERIFY2(mock.start(), qPrintable(mock.errorString()));
        const QJsonArray cases = mock.replies().value(QStringLiteral("parity")).toArray();
        QVERIFY(cases.size() >= 20);
        for (const QJsonValue& v : cases) {
            const QJsonObject c = v.toObject();
            const QString name = c.value(QStringLiteral("name")).toString();
            const QByteArray body = c.contains(QStringLiteral("body")) ? compact(c.value(QStringLiteral("body")).toObject()) : QByteArray();
            const Got g = call(m_nam, c.value(QStringLiteral("method")).toString().toLatin1(), mock.url() + c.value(QStringLiteral("path")).toString(), body);
            QVERIFY2(g.status == c.value(QStringLiteral("status")).toInt(), qPrintable(name + QStringLiteral(": status %1").arg(g.status)));
            QVERIFY2(g.h("content-type") == c.value(QStringLiteral("content_type")).toString().toLatin1(), qPrintable(name + QStringLiteral(": ") + QString::fromLatin1(g.h("content-type"))));
            if (c.contains(QStringLiteral("json"))) {
                QString why;
                const QSet<QString> ignore = name.startsWith(QLatin1String("chat")) ? QSet<QString>{QStringLiteral("/id")} : QSet<QString>{};   // gen-<n> per mock
                QVERIFY2(subset(c.value(QStringLiteral("json")), QJsonDocument::fromJson(g.body).object(), mock.url(), QString(), ignore, &why), qPrintable(name + QStringLiteral(": ") + why));
            } else {
                QCOMPARE(qint64(g.body.size()), qint64(c.value(QStringLiteral("bytes")).toDouble()));
                QVERIFY2(sha(g.body) == c.value(QStringLiteral("sha256")).toString().toLatin1(), qPrintable(name));
            }
        }
        // the helpers behind the binary answers are byte-identical to the TS buffers too
        const QJsonObject r = mock.replies();
        QCOMPARE(sha(MockOpenRouter::speechWav()), r.value(QStringLiteral("speech")).toObject().value(QStringLiteral("sha256")).toString().toLatin1());
        QCOMPARE(MockOpenRouter::pixelPng(), QByteArray::fromBase64(r.value(QStringLiteral("image")).toObject().value(QStringLiteral("b64")).toString().toLatin1()));
        QCOMPARE(mock.count(), int(cases.size()));
    }

    void chatJsonSchema() {
        // canned TS answers by schema name
        Got g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("Write the script."), {{QStringLiteral("response_format"), schemaFormat(QStringLiteral("script"))}}));
        QCOMPARE(g.status, 200);
        QCOMPARE(finishOf(g), QStringLiteral("stop"));
        QJsonObject script = contentJson(g);
        QCOMPARE(script.value(QStringLiteral("title")).toString(), QStringLiteral("The Morning Ritual"));
        QCOMPARE(script.value(QStringLiteral("scenes")).toArray().size(), 6);
        QCOMPARE(script, m_mock.cannedJson().value(QStringLiteral("script")).toObject());
        // web-search style annotations on the research schemas
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("response_format"), schemaFormat(QStringLiteral("brand_research"))}}));
        QCOMPARE(messageOf(g).value(QStringLiteral("annotations")).toArray().at(0).toObject().value(QStringLiteral("type")).toString(), QStringLiteral("url_citation"));
        QCOMPARE(contentJson(g).value(QStringLiteral("phone")).toString(), QStringLiteral("(276) 555-0142"));

        // an unknown schema name: an answer synthesized from the request's schema, every field present
        // ("#" inside a raw string literal derails moc's preprocessor, which then finds no Q_OBJECT class in this file:
        // the JSON references spell it as the escape \u0023)
        const QJsonObject schema = QJsonDocument::fromJson(R"({
            "type": "object", "additionalProperties": false,
            "required": ["title","count","ratio","ok","tags","kind","nested","maybe","fixed","when","short","pad","refs"],
            "properties": {
              "title": {"type": "string"},
              "count": {"type": "integer", "minimum": 2, "maximum": 9},
              "ratio": {"type": "number", "exclusiveMinimum": 0},
              "ok": {"type": "boolean"},
              "tags": {"type": "array", "minItems": 2, "items": {"type": "string"}},
              "kind": {"type": "string", "enum": ["alpha", "beta"]},
              "nested": {"$ref": "\u0023/$defs/Nested"},
              "maybe": {"anyOf": [{"type": "null"}, {"type": "integer", "minimum": 5}]},
              "fixed": {"const": 7},
              "when": {"type": "string", "format": "date-time"},
              "short": {"type": "string", "maxLength": 3},
              "pad": {"type": "string", "minLength": 12},
              "refs": {"type": ["array", "null"], "items": {"$ref": "\u0023/$defs/Nested"}, "maxItems": 1}
            },
            "$defs": {"Nested": {"type": "object", "properties": {"x": {"type": ["string", "null"]}, "n": {"type": "number", "default": 2.5}}}}
        })").object();
        QVERIFY(!schema.isEmpty());
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("response_format"), schemaFormat(QStringLiteral("mine"), schema)}}));
        const QJsonObject mine = contentJson(g);
        QCOMPARE(mine.keys().size(), 13);
        QCOMPARE(mine.value(QStringLiteral("title")).toString(), QStringLiteral("mock title"));
        QCOMPARE(mine.value(QStringLiteral("count")).toDouble(), 2.0);
        QVERIFY(mine.value(QStringLiteral("ratio")).toDouble() > 0);
        QCOMPARE(mine.value(QStringLiteral("ok")), QJsonValue(false));
        QCOMPARE(mine.value(QStringLiteral("tags")).toArray().size(), 2);
        QCOMPARE(mine.value(QStringLiteral("kind")).toString(), QStringLiteral("alpha"));
        QCOMPARE(mine.value(QStringLiteral("nested")).toObject().value(QStringLiteral("x")).toString(), QStringLiteral("mock x"));
        QCOMPARE(mine.value(QStringLiteral("nested")).toObject().value(QStringLiteral("n")).toDouble(), 2.5);
        QCOMPARE(mine.value(QStringLiteral("maybe")).toDouble(), 5.0);
        QCOMPARE(mine.value(QStringLiteral("fixed")).toDouble(), 7.0);
        QCOMPARE(mine.value(QStringLiteral("when")).toString(), QStringLiteral("2026-01-01T00:00:00Z"));
        QCOMPARE(mine.value(QStringLiteral("short")).toString().size(), 3);
        QVERIFY(mine.value(QStringLiteral("pad")).toString().size() >= 12);
        QCOMPARE(mine.value(QStringLiteral("refs")).toArray().size(), 1);

        // a test's own handler sees the messages joined as the TS mock joins them (non-string content as JSON)
        m_mock.setJsonReply(QStringLiteral("mine"), [](const MockRequest& rq, const QString& text) -> QJsonValue {
            return QJsonObject{{QStringLiteral("echo"), text}, {QStringLiteral("model"), rq.json.value(QStringLiteral("model"))}};
        });
        QJsonObject two = chatBody(QStringLiteral("first"), {{QStringLiteral("response_format"), schemaFormat(QStringLiteral("mine"), schema)}});
        QJsonArray msgs = two.value(QStringLiteral("messages")).toArray();
        msgs.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("second")}}}}});
        two.insert(QStringLiteral("messages"), msgs);
        g = post(m_nam, u(QStringLiteral("/chat/completions")), two);
        QCOMPARE(contentJson(g).value(QStringLiteral("echo")).toString(), QStringLiteral("first\n[{\"text\":\"second\",\"type\":\"text\"}]"));
        QCOMPARE(contentJson(g).value(QStringLiteral("model")).toString(), QStringLiteral("acme/writer-pro"));
        m_mock.setJsonReply(QStringLiteral("mine"), {});
        g = post(m_nam, u(QStringLiteral("/chat/completions")), two);
        QCOMPARE(contentJson(g).value(QStringLiteral("title")).toString(), QStringLiteral("mock title"));
        // a handler also overrides a canned name
        m_mock.setJsonReply(QStringLiteral("review"), [](const MockRequest&, const QString&) -> QJsonValue { return QJsonObject{{QStringLiteral("score"), 10}}; });
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("response_format"), schemaFormat(QStringLiteral("review"))}}));
        QCOMPARE(contentJson(g), (QJsonObject{{QStringLiteral("score"), 10}}));
        m_mock.setJsonReply(QStringLiteral("review"), {});

        // text answers: the TS article, the read-aloud text, json_object; model echo and the models[] fallback
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("Write it.")));
        QVERIFY(messageOf(g).value(QStringLiteral("content")).toString().startsWith(QLatin1String("# Henry County board approves budget")));
        QCOMPARE(g.json().value(QStringLiteral("model")).toString(), QStringLiteral("acme/writer-pro"));
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("Make it read aloud.")));
        QCOMPARE(messageOf(g).value(QStringLiteral("content")).toString(), m_mock.replies().value(QStringLiteral("read_aloud")).toString());
        QJsonObject noModel = chatBody(QStringLiteral("x"), {{QStringLiteral("response_format"), QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}}});
        noModel.remove(QStringLiteral("model"));
        g = post(m_nam, u(QStringLiteral("/chat/completions")), noModel);
        QVERIFY(!g.json().contains(QStringLiteral("model")));
        QCOMPARE(contentJson(g), (QJsonObject{{QStringLiteral("result"), QStringLiteral("mock")}}));
        noModel.insert(QStringLiteral("models"), QJsonArray{QStringLiteral("zeta/lite"), QStringLiteral("acme/writer-pro")});
        QCOMPARE(post(m_nam, u(QStringLiteral("/chat/completions")), noModel).json().value(QStringLiteral("model")).toString(), QStringLiteral("zeta/lite"));

        // usage, the cost knob and /generation
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x")));
        const QJsonObject usage = g.json().value(QStringLiteral("usage")).toObject();
        QCOMPARE(usage.value(QStringLiteral("prompt_tokens")).toInt(), 1000);
        QCOMPARE(usage.value(QStringLiteral("completion_tokens")).toInt(), 500);
        QCOMPARE(usage.value(QStringLiteral("cost")).toDouble(), 0.002);
        const QString id1 = g.json().value(QStringLiteral("id")).toString();
        QVERIFY(id1.startsWith(QLatin1String("gen-")));
        m_mock.setUsageCost(QStringLiteral("/chat/completions"), 0.25);
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x")));
        m_mock.setUsageCost(QStringLiteral("/chat/completions"), -1);
        QCOMPARE(g.json().value(QStringLiteral("usage")).toObject().value(QStringLiteral("cost")).toDouble(), 0.25);
        const QString id2 = g.json().value(QStringLiteral("id")).toString();
        QVERIFY(id2 != id1);
        const auto genCost = [&](const QString& id) {
            return get(m_nam, u(QStringLiteral("/generation?id=") + id)).json().value(QStringLiteral("data")).toObject().value(QStringLiteral("total_cost")).toDouble();
        };
        QCOMPARE(genCost(id1), 0.002);
        QCOMPARE(genCost(id2), 0.25);
        QCOMPARE(genCost(QStringLiteral("gen-tts")), 0.05);
        m_mock.setGenerationCost(QStringLiteral("gen-tts"), 0.0123);
        QCOMPARE(genCost(QStringLiteral("gen-tts")), 0.0123);
        QCOMPARE(m_mock.last(QStringLiteral("/generation"))->queryValue(QStringLiteral("id")), QStringLiteral("gen-tts"));

        // streaming is refused loudly (the Studio client never streams)
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("stream"), true}}));
        QCOMPARE(g.status, 400);
        QVERIFY(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QLatin1String("stream")));
    }

    void chatTools() {
        const QJsonArray tools{
            fn(QStringLiteral("get_weather"), QJsonDocument::fromJson(R"({"type":"object","properties":{"place":{"type":"string"},"days":{"type":"integer","minimum":1}},"required":["place","days"]})").object()),
            fn(QStringLiteral("lookup"), QJsonDocument::fromJson(R"({"type":"object","properties":{"q":{"type":"string"}}})").object())};
        // auto: the first tool, arguments synthesized from its parameters
        Got g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("Weather?"), {{QStringLiteral("tools"), tools}}));
        QCOMPARE(g.status, 200);
        QCOMPARE(finishOf(g), QStringLiteral("tool_calls"));
        QJsonObject msg = messageOf(g);
        QVERIFY(msg.value(QStringLiteral("content")).isNull());
        QJsonObject call = msg.value(QStringLiteral("tool_calls")).toArray().at(0).toObject();
        QVERIFY(call.value(QStringLiteral("id")).toString().startsWith(QLatin1String("call_")));
        QCOMPARE(call.value(QStringLiteral("type")).toString(), QStringLiteral("function"));
        QCOMPARE(call.value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("get_weather"));
        const QJsonObject args = jsonOf(call.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments")).toString().toUtf8());   // a JSON string
        QCOMPARE(args, (QJsonObject{{QStringLiteral("place"), QStringLiteral("mock place")}, {QStringLiteral("days"), 1}}));
        // a named tool_choice
        g = post(m_nam, u(QStringLiteral("/chat/completions")),
                 chatBody(QStringLiteral("x"), {{QStringLiteral("tools"), tools},
                                               {QStringLiteral("tool_choice"), QJsonObject{{QStringLiteral("type"), QStringLiteral("function")}, {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), QStringLiteral("lookup")}}}}}}));
        QCOMPARE(messageOf(g).value(QStringLiteral("tool_calls")).toArray().at(0).toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("lookup"));
        // none: a text answer
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("tools"), tools}, {QStringLiteral("tool_choice"), QStringLiteral("none")}}));
        QCOMPARE(finishOf(g), QStringLiteral("stop"));
        QVERIFY(!messageOf(g).contains(QStringLiteral("tool_calls")));
        // after the tool result: the final answer
        QJsonObject loop = chatBody(QStringLiteral("Weather?"), {{QStringLiteral("tools"), tools}});
        QJsonArray m = loop.value(QStringLiteral("messages")).toArray();
        m.append(msg);
        m.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")}, {QStringLiteral("tool_call_id"), call.value(QStringLiteral("id"))}, {QStringLiteral("content"), QStringLiteral("72F and sunny")}});
        loop.insert(QStringLiteral("messages"), m);
        g = post(m_nam, u(QStringLiteral("/chat/completions")), loop);
        QCOMPARE(finishOf(g), QStringLiteral("stop"));
        QCOMPARE(messageOf(g).value(QStringLiteral("content")).toString(), QStringLiteral("[mock] tool result: 72F and sunny"));
        // tools + json_schema: auto answers the schema, "required" calls a tool
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("x"), {{QStringLiteral("tools"), tools}, {QStringLiteral("response_format"), schemaFormat(QStringLiteral("review"))}}));
        QCOMPARE(contentJson(g).value(QStringLiteral("score")).toDouble(), 7.0);
        g = post(m_nam, u(QStringLiteral("/chat/completions")),
                 chatBody(QStringLiteral("x"), {{QStringLiteral("tools"), tools}, {QStringLiteral("tool_choice"), QStringLiteral("required")}, {QStringLiteral("response_format"), schemaFormat(QStringLiteral("review"))}}));
        QCOMPARE(finishOf(g), QStringLiteral("tool_calls"));

        // a scripted brain: decides from the request, may set finish_reason, nullopt falls through
        int turns = 0;
        m_mock.setChatHandler([&turns](const MockRequest& rq) -> std::optional<QJsonObject> {
            const QJsonArray ms = rq.json.value(QStringLiteral("messages")).toArray();
            const QString last = ms.last().toObject().value(QStringLiteral("content")).toString();
            if (!last.contains(QLatin1String("brain"))) return std::nullopt;
            ++turns;
            if (ms.last().toObject().value(QStringLiteral("role")).toString() == QLatin1String("user"))
                return QJsonObject{{QStringLiteral("content"), QStringLiteral("```json\n{\"partial\": true\n```")}, {QStringLiteral("finish_reason"), QStringLiteral("length")}};
            return QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QStringLiteral("brain done")}};
        });
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("ask the brain")));
        QCOMPARE(finishOf(g), QStringLiteral("length"));
        QCOMPARE(messageOf(g).value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));   // added when the brain leaves it out
        QVERIFY(!messageOf(g).contains(QStringLiteral("finish_reason")));
        QVERIFY(messageOf(g).value(QStringLiteral("content")).toString().startsWith(QLatin1String("```json")));
        g = post(m_nam, u(QStringLiteral("/chat/completions")), chatBody(QStringLiteral("plain")));
        QVERIFY(messageOf(g).value(QStringLiteral("content")).toString().startsWith(QLatin1String("# Henry County")));
        QCOMPARE(turns, 1);
        m_mock.setChatHandler({});
    }

    void mediaRoutes() {
        // images
        Got g = post(m_nam, u(QStringLiteral("/images")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a red car")}});
        QCOMPARE(g.status, 200);
        QJsonArray data = g.json().value(QStringLiteral("data")).toArray();
        QCOMPARE(data.size(), 1);
        QCOMPARE(QByteArray::fromBase64(data.at(0).toObject().value(QStringLiteral("b64_json")).toString().toLatin1()), MockOpenRouter::pixelPng());
        QCOMPARE(data.at(0).toObject().value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
        QCOMPARE(g.json().value(QStringLiteral("usage")).toObject().value(QStringLiteral("cost")).toDouble(), 0.04);
        QByteArray big(3 << 20, '\0');
        for (int i = 0; i < big.size(); ++i) big[i] = char((i * 131) & 0xff);
        m_mock.setImageBytes(big);
        g = post(m_nam, u(QStringLiteral("/images")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("p")}, {QStringLiteral("n"), 3}});
        m_mock.setImageBytes({});
        data = g.json().value(QStringLiteral("data")).toArray();
        QCOMPARE(data.size(), 3);
        QCOMPARE(QByteArray::fromBase64(data.at(2).toObject().value(QStringLiteral("b64_json")).toString().toLatin1()), big);

        // video: submit -> pending -> in_progress -> completed (repeats) -> content
        m_mock.setVideoStatuses({QStringLiteral("pending"), QStringLiteral("in_progress"), QStringLiteral("completed")});
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), QStringLiteral("pan")}, {QStringLiteral("duration"), 4}});
        QCOMPARE(g.status, 202);
        const QString job = g.json().value(QStringLiteral("id")).toString();
        QVERIFY(job.startsWith(QLatin1String("job")));
        QCOMPARE(g.json().value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        const QString pollUrl = g.json().value(QStringLiteral("polling_url")).toString();
        QCOMPARE(pollUrl, u(QStringLiteral("/videos/") + job));
        QStringList seen;
        QJsonObject last;
        for (int i = 0; i < 4; ++i) {
            last = get(m_nam, pollUrl).json();
            seen << last.value(QStringLiteral("status")).toString();
        }
        QCOMPARE(seen, (QStringList{QStringLiteral("pending"), QStringLiteral("in_progress"), QStringLiteral("completed"), QStringLiteral("completed")}));
        QCOMPARE(last.value(QStringLiteral("usage")).toObject().value(QStringLiteral("cost")).toDouble(), 0.6);
        const QString content = last.value(QStringLiteral("unsigned_urls")).toArray().at(0).toString();
        QCOMPARE(content, pollUrl + QStringLiteral("/content"));
        g = get(m_nam, content);
        QCOMPARE(g.body, QByteArray("fakemp4"));
        QCOMPARE(g.h("content-type"), QByteArray("video/mp4"));
        // a failed job reports its error; new settings apply to new jobs only; polling_url styles
        m_mock.setVideoStatuses({QStringLiteral("queued"), QStringLiteral("failed")});
        m_mock.setVideoContent("MP4DATA");
        m_mock.setVideoCost(1.25);
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::ApiRelative);
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), QStringLiteral("x")}});
        const QString job2 = g.json().value(QStringLiteral("id")).toString();
        QCOMPARE(g.json().value(QStringLiteral("polling_url")).toString(), QStringLiteral("/api/v1/videos/") + job2);
        QCOMPARE(get(m_nam, u(QStringLiteral("/api/v1/videos/") + job2)).json().value(QStringLiteral("status")).toString(), QStringLiteral("queued"));
        last = get(m_nam, u(QStringLiteral("/videos/") + job2)).json();
        QCOMPARE(last.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QCOMPARE(last.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(), QStringLiteral("mock: video generation failed"));
        QVERIFY(!last.contains(QStringLiteral("unsigned_urls")));
        QCOMPARE(get(m_nam, u(QStringLiteral("/videos/") + job2 + QStringLiteral("/content"))).body, QByteArray("MP4DATA"));
        QCOMPARE(get(m_nam, content).body, QByteArray("fakemp4"));   // the first job kept its bytes
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::Relative);
        m_mock.setVideoStatuses({QStringLiteral("completed")});
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}});
        QCOMPARE(g.json().value(QStringLiteral("polling_url")).toString(), QStringLiteral("videos/") + g.json().value(QStringLiteral("id")).toString());
        QCOMPARE(get(m_nam, u(QStringLiteral("/videos/") + g.json().value(QStringLiteral("id")).toString())).json().value(QStringLiteral("usage")).toObject().value(QStringLiteral("cost")).toDouble(), 1.25);
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::None);
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}});
        QVERIFY(!g.json().contains(QStringLiteral("polling_url")));
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::Absolute);
        m_mock.setVideoContent("fakemp4");
        m_mock.setVideoCost(0.6);
        g = get(m_nam, u(QStringLiteral("/videos/job999")));
        QCOMPARE(g.status, 404);
        QCOMPARE(get(m_nam, u(QStringLiteral("/videos/job999/content"))).status, 404);

        // speech: the TS WAV, byte for byte
        g = post(m_nam, u(QStringLiteral("/audio/speech")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/tts-1")}, {QStringLiteral("input"), QStringLiteral("Hello.")}, {QStringLiteral("voice"), QStringLiteral("alloy")}});
        QCOMPARE(g.status, 200);
        QCOMPARE(g.h("content-type"), QByteArray("audio/mpeg"));
        QCOMPARE(g.h("x-generation-id"), QByteArray("gen-tts"));
        QCOMPARE(g.body, MockOpenRouter::speechWav());
        QCOMPARE(MockOpenRouter::wavSeconds(g.body), 3.0);
        m_mock.setSpeechBytes("ID3fake");
        QCOMPARE(post(m_nam, u(QStringLiteral("/audio/speech")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/tts-1")}}).body, QByteArray("ID3fake"));
        m_mock.setSpeechBytes({});

        // transcriptions: duration from the WAV that came in; json has no segments; verbose_json may be refused
        const auto stt = [&](const QByteArray& audio, const QString& format, const QString& rf) {
            return post(m_nam, u(QStringLiteral("/audio/transcriptions")),
                        QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/stt-1")},
                                    {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), QString::fromLatin1(audio.toBase64())}, {QStringLiteral("format"), format}}},
                                    {QStringLiteral("response_format"), rf}});
        };
        g = stt(makeWav(2.5), QStringLiteral("wav"), QStringLiteral("verbose_json"));
        QCOMPARE(g.status, 200);
        QCOMPARE(g.json().value(QStringLiteral("duration")).toDouble(), 2.5);
        const QJsonArray segs = g.json().value(QStringLiteral("segments")).toArray();
        QCOMPARE(segs.size(), 2);
        QCOMPARE(segs.at(1).toObject().value(QStringLiteral("start")).toDouble(), 1.25);
        QCOMPARE(segs.at(1).toObject().value(QStringLiteral("end")).toDouble(), 2.5);
        QVERIFY(g.json().value(QStringLiteral("text")).toString().startsWith(QLatin1String("The Henry County school board")));
        QCOMPARE(g.json().value(QStringLiteral("usage")).toObject().value(QStringLiteral("cost")).toDouble(), 0.003);
        g = stt(makeWav(1), QStringLiteral("wav"), QStringLiteral("json"));
        QVERIFY(g.json().contains(QStringLiteral("text")));
        QVERIFY(!g.json().contains(QStringLiteral("segments")));
        QVERIFY(!g.json().contains(QStringLiteral("duration")));
        g = stt("not audio", QStringLiteral("mp3"), QStringLiteral("verbose_json"));
        QCOMPARE(g.json().value(QStringLiteral("duration")).toDouble(), 12.0);
        m_mock.setRejectVerboseJson(true);
        QCOMPARE(stt(makeWav(1), QStringLiteral("wav"), QStringLiteral("verbose_json")).status, 400);
        QCOMPARE(stt(makeWav(1), QStringLiteral("wav"), QStringLiteral("json")).status, 200);
        m_mock.setRejectVerboseJson(false);
        QCOMPARE(MockOpenRouter::wavSeconds(makeWav(4.0, 24000)), 4.0);
        QCOMPARE(MockOpenRouter::wavSeconds("RIFF1234WAVEnope"), -1.0);

        // embeddings and rerank
        // ("hello" and "world" share bucket 18; "budget" is bucket 5)
        g = post(m_nam, u(QStringLiteral("/embeddings")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/embed-1")}, {QStringLiteral("input"), QJsonArray{QStringLiteral("Hello hello, budget"), QStringLiteral("")}}});
        data = g.json().value(QStringLiteral("data")).toArray();
        QCOMPARE(data.size(), 2);
        const QJsonArray v0 = data.at(0).toObject().value(QStringLiteral("embedding")).toArray();
        QCOMPARE(v0.size(), 64);
        quint32 h = 0;
        for (char c : QByteArray("hello")) h = h * 31u + uchar(c);
        QCOMPARE(v0.at(int(h % 64u)).toDouble(), 2.0);
        double sum = 0;
        for (const QJsonValue& x : v0) sum += x.toDouble();
        QCOMPARE(sum, 3.0);
        QCOMPARE(data.at(1).toObject().value(QStringLiteral("index")).toInt(), 1);
        const auto rerank = [&](const QJsonValue& topN) {
            QJsonObject b{{QStringLiteral("model"), QStringLiteral("acme/rerank-1")}, {QStringLiteral("query"), QStringLiteral("q")},
                          {QStringLiteral("documents"), QJsonArray{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}}};
            if (!topN.isUndefined()) b.insert(QStringLiteral("top_n"), topN);
            return post(m_nam, u(QStringLiteral("/rerank")), b).json().value(QStringLiteral("results")).toArray();
        };
        QCOMPARE(rerank(2).size(), 2);
        QCOMPARE(rerank(2).at(1).toObject().value(QStringLiteral("relevance_score")).toDouble(), 0.5);
        QCOMPARE(rerank(QJsonValue(QJsonValue::Undefined)).size(), 3);
        QCOMPARE(rerank(-1).size(), 2);   // JS slice(0, -1)
        QCOMPARE(rerank(10).size(), 3);
    }

    void scriptedFailures() {
        const QString chat = u(QStringLiteral("/chat/completions"));
        // 429 with Retry-After seconds, twice, then the normal answer
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::rateLimited(7), 2);
        QCOMPARE(m_mock.pendingScripts(), 1);
        for (int i = 0; i < 2; ++i) {
            const Got g = post(m_nam, chat, chatBody(QStringLiteral("x")));
            QCOMPARE(g.status, 429);
            QCOMPARE(g.h("retry-after"), QByteArray("7"));
            QVERIFY(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QLatin1String("Rate limit exceeded")));
            QCOMPARE(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toInt(), 429);
        }
        QCOMPARE(post(m_nam, chat, chatBody(QStringLiteral("x"))).status, 200);
        QCOMPARE(m_mock.pendingScripts(), 0);
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 3);
        // retry-after-ms, an HTTP date, the daily quota, 402, 503, 502 with provider metadata, the spend cap, 413, 500
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::rateLimitedMs(1500));
        QCOMPARE(post(m_nam, chat, chatBody(QStringLiteral("x"))).h("retry-after-ms"), QByteArray("1500"));
        const QDateTime when = QDateTime::currentDateTimeUtc().addSecs(90);
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::rateLimitedUntil(when));
        const QByteArray date = post(m_nam, chat, chatBody(QStringLiteral("x"))).h("retry-after");
        QVERIFY2(date.endsWith(" GMT"), date.constData());
        const QDateTime parsed = QLocale::c().toDateTime(QString::fromLatin1(date.chopped(4)), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss"));
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.toString(QStringLiteral("yyyyMMddhhmmss")), when.toString(QStringLiteral("yyyyMMddhhmmss")));   // the UTC fields
        const qint64 reset = QDateTime::currentMSecsSinceEpoch() + 3600000;
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::dailyQuota(reset));
        Got g = post(m_nam, chat, chatBody(QStringLiteral("x")));
        QCOMPARE(g.status, 429);
        QCOMPARE(g.h("x-ratelimit-reset"), QByteArray::number(reset));
        QCOMPARE(g.h("x-ratelimit-remaining"), QByteArray("0"));
        QVERIFY(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QLatin1String("free-models-per-day")));
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::dailyQuota());
        const qint64 midnight = post(m_nam, chat, chatBody(QStringLiteral("x"))).h("x-ratelimit-reset").toLongLong();
        QVERIFY(midnight > QDateTime::currentMSecsSinceEpoch() && midnight <= QDateTime::currentMSecsSinceEpoch() + 86400000);
        QCOMPARE(midnight % 86400000, 0);
        m_mock.script(QStringLiteral("/images"), MockReply::noCredit());
        g = post(m_nam, u(QStringLiteral("/images")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}});
        QCOMPARE(g.status, 402);
        QVERIFY(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().startsWith(QLatin1String("Insufficient credits")));
        m_mock.script(QStringLiteral("/videos"), MockReply::unavailable(3));
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}});
        QCOMPARE(g.status, 503);
        QCOMPARE(g.h("retry-after"), QByteArray("3"));
        m_mock.script(QStringLiteral("/videos"), MockReply::unavailable());
        g = post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}});
        QCOMPARE(g.status, 503);
        QVERIFY(!g.headers.contains("retry-after"));
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::providerError(502, QStringLiteral("Acme"), QStringLiteral("Rate limit reached for requests")));
        g = post(m_nam, chat, chatBody(QStringLiteral("x")));
        QCOMPARE(g.status, 502);
        const QJsonObject err = g.json().value(QStringLiteral("error")).toObject();
        QCOMPARE(err.value(QStringLiteral("message")).toString(), QStringLiteral("Provider returned error"));
        QCOMPARE(err.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("provider_name")).toString(), QStringLiteral("Acme"));
        QCOMPARE(err.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("raw")).toString(), QStringLiteral("Rate limit reached for requests"));
        m_mock.script(QStringLiteral("/chat/completions"), MockReply::spendCap());
        g = post(m_nam, chat, chatBody(QStringLiteral("x")));
        QCOMPARE(g.status, 429);
        QVERIFY(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString().contains(QLatin1String("Raise it in Settings")));
        m_mock.script(QStringLiteral("/images"), MockReply::tooLarge());
        QCOMPARE(post(m_nam, u(QStringLiteral("/images")), QJsonObject{}).status, 413);
        m_mock.script(QStringLiteral("/images"), MockReply::serverError().withHeader("X-Test", "1"));
        g = post(m_nam, u(QStringLiteral("/images")), QJsonObject{});
        QCOMPARE(g.status, 500);
        QCOMPARE(g.h("x-test"), QByteArray("1"));
        // a scripted body of a test's own
        m_mock.script(QStringLiteral("/chat/completions"),
                      MockReply::json(QJsonObject{{QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QStringLiteral("Sure! ```json\n{\"a\":1}\n``` hope it helps")}}}}}}}));
        QCOMPARE(messageOf(post(m_nam, chat, chatBody(QStringLiteral("x")))).value(QStringLiteral("content")).toString(), QStringLiteral("Sure! ```json\n{\"a\":1}\n``` hope it helps"));
        QCOMPARE(m_mock.pendingScripts(), 0);

        // patterns: the method, a prefix, the first match wins and is used up first
        m_mock.script(QStringLiteral("GET /chat/completions"), MockReply::serverError(), -1);
        QCOMPARE(post(m_nam, chat, chatBody(QStringLiteral("x"))).status, 200);
        QCOMPARE(get(m_nam, chat).status, 500);
        m_mock.clearScripts();
        m_mock.script(QStringLiteral("/videos/*"), MockReply::error(418, QStringLiteral("teapot")), 1);
        QCOMPARE(post(m_nam, u(QStringLiteral("/videos")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}}).status, 202);   // "/videos" is not under "/videos/"
        QCOMPARE(get(m_nam, u(QStringLiteral("/api/v1/videos/job1"))).status, 418);
        m_mock.script(QStringLiteral("/credits"), MockReply::error(400, QStringLiteral("first")), 1);
        m_mock.script(QStringLiteral("/credits"), MockReply::error(401, QStringLiteral("second")), 1);
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 400);
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 401);
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 200);
        m_mock.script(QStringLiteral("/credits"), MockReply::serverError(), 0);   // times 0: nothing queued
        QCOMPARE(m_mock.pendingScripts(), 0);
        // the TS mock's fail set
        m_mock.failPath(QStringLiteral("/embeddings"));
        for (int i = 0; i < 2; ++i) {
            g = post(m_nam, u(QStringLiteral("/embeddings")), QJsonObject{{QStringLiteral("input"), QStringLiteral("x")}});
            QCOMPARE(g.status, 500);
            QCOMPARE(g.json().value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString(), QStringLiteral("mock failure"));
        }
        m_mock.clearScripts();
        QCOMPARE(post(m_nam, u(QStringLiteral("/embeddings")), QJsonObject{{QStringLiteral("input"), QStringLiteral("x")}}).status, 200);
        // a required key: 401 without it or with another one (recorded either way; the mock never logs it)
        m_mock.setRequiredKey("sk-or-v1-mocktest");
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 401);
        QCOMPARE(call(m_nam, "GET", u(QStringLiteral("/credits")), {}, 5000, {{"Authorization", "Bearer sk-or-v1-other"}}).status, 401);
        QCOMPARE(call(m_nam, "GET", u(QStringLiteral("/credits")), {}, 5000, {{"Authorization", "Bearer sk-or-v1-mocktest"}}).status, 200);
        QCOMPARE(m_mock.last(QStringLiteral("/credits"))->header("AUTHORIZATION"), QByteArray("Bearer sk-or-v1-mocktest"));
        m_mock.setRequiredKey({});
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 200);
        QCOMPARE(get(m_nam, u(QStringLiteral("/nope"))).status, 404);
    }

    void slowAndBrokenBodies() {
        const QByteArray credits = compact(get(m_nam, u(QStringLiteral("/credits"))).json());
        // a slow body: headers at once, the body in 8-byte pieces every 60 ms
        m_mock.script(QStringLiteral("/credits"), MockReply::normal().slowBody(8, 60));
        {
            Raw c(m_mock.port());
            c.send(rawGet("/credits"));
            Resp r;
            QVERIFY(c.next(&r, 5000));
            const qint64 total = c.clock.elapsed();
            QCOMPARE(r.status, 200);
            QCOMPARE(compact(jsonOf(r.body)), credits);
            const int pieces = int((r.body.size() + 7) / 8);
            QVERIFY(pieces >= 5);
            QVERIFY2(total >= (pieces - 1) * 60 * 8 / 10, qPrintable(QStringLiteral("%1 ms for %2 pieces").arg(total).arg(pieces)));
            QVERIFY2(c.headAtMs >= 0 && c.headAtMs < 200, qPrintable(QString::number(c.headAtMs)));
        }
        // a scripted error trickled too
        m_mock.script(QStringLiteral("/credits"), MockReply::rateLimited(1).slowBody(16, 20));
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 429);

        // headers that come too late: the client times out, the request was seen once, nothing stays in flight
        m_mock.script(QStringLiteral("/credits"), MockReply::normal().delayed(1500));
        Got g = get(m_nam, u(QStringLiteral("/credits")), 300);
        QVERIFY2(g.error == QNetworkReply::OperationCanceledError || g.error == QNetworkReply::TimeoutError, qPrintable(QString::number(int(g.error))));
        QVERIFY2(g.ms < 1300, qPrintable(QString::number(g.ms)));
        QCOMPARE(m_mock.count(QStringLiteral("/credits")), 4);
        QVERIFY(QTest::qWaitFor([this]() { return m_mock.inFlight() == 0; }, 3000));

        // a body that stalls after 100 bytes: in flight until released
        m_mock.script(QStringLiteral("/models"), MockReply::normal().stallAfter(100));
        {
            Raw c(m_mock.port());
            c.send(rawGet("/models"));
            QVERIFY(QTest::qWaitFor([&]() { c.read(); return c.headAtMs >= 0; }, 3000));
            QTest::qWait(300);
            c.read();
            const qsizetype head = c.buf.indexOf("\r\n\r\n") + 4;
            QCOMPARE(c.buf.size() - head, qsizetype(100));
            QVERIFY(c.buf.contains("Content-Length: "));
            QCOMPARE(m_mock.inFlight(QStringLiteral("/models")), 1);
            m_mock.releaseStalled();
            QVERIFY(c.waitClosed(3000));
            QCOMPARE(c.buf.size() - head, qsizetype(100));
            QVERIFY(QTest::qWaitFor([this]() { return m_mock.inFlight(QStringLiteral("/models")) == 0; }, 3000));
        }
        // ... and with a real client, whose transfer timeout fires; the mock lets go when the client does
        m_mock.script(QStringLiteral("/models"), MockReply::normal().stallAfter(100));
        g = get(m_nam, u(QStringLiteral("/models")), 400);
        QVERIFY(g.error != QNetworkReply::NoError);
        QVERIFY(QTest::qWaitFor([this]() { return m_mock.inFlight() == 0; }, 3000));

        // a truncated body (closed after 50 bytes)
        m_mock.script(QStringLiteral("/models"), MockReply::normal().truncateAfter(50));
        {
            Raw c(m_mock.port());
            c.send(rawGet("/models"));
            QVERIFY(c.waitClosed(3000));
            const qsizetype head = c.buf.indexOf("\r\n\r\n") + 4;
            QVERIFY(head > 4);
            QCOMPARE(c.buf.size() - head, qsizetype(50));
            QVERIFY(c.buf.contains("Connection: close"));
        }
        m_mock.script(QStringLiteral("/models"), MockReply::normal().truncateAfter(50));
        QVERIFY(get(m_nam, u(QStringLiteral("/models"))).error != QNetworkReply::NoError);
        // dropped and reset without a byte
        m_mock.script(QStringLiteral("/credits"), MockReply::dropped());
        {
            Raw c(m_mock.port());
            c.send(rawGet("/credits"));
            QVERIFY(c.waitClosed(3000));
            QVERIFY(c.buf.isEmpty());
        }
        m_mock.script(QStringLiteral("/credits"), MockReply::resetConnection());
        {
            Raw c(m_mock.port());
            c.send(rawGet("/credits"));
            QVERIFY(c.waitClosed(3000));
            QVERIFY(c.buf.isEmpty());
        }
        // QNAM itself re-sends a request whose connection closed before any answer byte (it takes it for a stale
        // keep-alive connection). The mock counts every attempt, which is how ST-07 proves a paid POST is not re-sent.
        m_mock.script(QStringLiteral("POST /images"), MockReply::resetConnection(), -1);
        const int before = m_mock.count(QStringLiteral("/images"));
        g = post(m_nam, u(QStringLiteral("/images")), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}});
        QVERIFY(g.error != QNetworkReply::NoError);
        const int attempts = m_mock.count(QStringLiteral("/images")) - before;
        QVERIFY(attempts >= 1);
        qInfo("QNAM sent a POST %d time(s) when every attempt was reset before an answer", attempts);
        m_mock.clearScripts();
        QVERIFY(QTest::qWaitFor([this]() { return m_mock.inFlight() == 0; }, 3000));
        // the mock still answers normally afterwards
        QCOMPARE(get(m_nam, u(QStringLiteral("/credits"))).status, 200);
    }

    void concurrencyAndThreads() {
        // Peak in-flight seen by the server (raw sockets, so no client-side connection cap applies).
        MockOpenRouter::Options o;
        o.ownThread = true;
        MockOpenRouter mock;
        QVERIFY2(mock.start(o), qPrintable(mock.errorString()));
        mock.setLatency(QStringLiteral("/chat/completions"), 300);
        const QByteArray req = rawPost("/chat/completions", compact(chatBody(QStringLiteral("x"))));
        auto fire = [&](int n, QList<int>* statuses, QList<QByteArray>* retry) {
            std::vector<std::unique_ptr<Raw>> cs;
            for (int i = 0; i < n; ++i) cs.push_back(std::make_unique<Raw>(mock.port()));
            for (auto& c : cs) c->send(req);
            for (auto& c : cs) {
                Resp r;
                QVERIFY(c->next(&r, 5000));
                statuses->append(r.status);
                retry->append(r.h("retry-after-ms"));
            }
        };
        QList<int> st;
        QList<QByteArray> ra;
        QElapsedTimer t;
        t.start();
        fire(12, &st, &ra);
        QCOMPARE(st, QList<int>(12, 200));
        QCOMPARE(mock.peakInFlight(QStringLiteral("/chat/completions")), 12);
        QCOMPARE(mock.peakInFlight(), 12);
        QVERIFY2(t.elapsed() < 2500, qPrintable(QString::number(t.elapsed())));   // concurrent, not 12 x 300 ms
        QVERIFY(QTest::qWaitFor([&]() { return mock.inFlight() == 0; }, 3000));
        // the concurrency limit: 4 in flight, the rest 429 with retry-after-ms
        mock.resetStats();
        QCOMPARE(mock.peakInFlight(), 0);
        mock.setConcurrencyLimit(QStringLiteral("/chat/completions"), 4, 250);
        st.clear();
        ra.clear();
        fire(12, &st, &ra);
        QCOMPARE(st.count(200), 4);
        QCOMPARE(st.count(429), 8);
        QCOMPARE(ra.count("250"), 8);
        mock.setConcurrencyLimit(QStringLiteral("/chat/completions"), 0);
        mock.setLatency(QStringLiteral("/chat/completions"), 0);
        QCOMPARE(mock.count(QStringLiteral("/chat/completions")), 12);
        QVERIFY(mock.connectionsAccepted() >= 12);

        // a client that blocks this thread still gets its answer from the mock's own thread
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, mock.port());
        QVERIFY(s.waitForConnected(3000));
        s.write(rawGet("/credits", "Connection: close\r\n"));
        QByteArray buf;
        while (s.waitForReadyRead(3000)) buf += s.readAll();
        buf += s.readAll();
        Resp r;
        QVERIFY(parseResponse(buf, &r) > 0);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.h("connection"), QByteArray("close"));
        QVERIFY(mock.waitForRequests(QStringLiteral("/credits"), 1, 1000));
        // a stalled answer on the threaded mock is released from this thread
        mock.script(QStringLiteral("/models"), MockReply::normal().stallAfter(10));
        Raw stalled(mock.port());
        stalled.send(rawGet("/models"));
        QVERIFY(QTest::qWaitFor([&]() { return mock.inFlight(QStringLiteral("/models")) == 1; }, 3000));
        mock.releaseStalled();
        QVERIFY(stalled.waitClosed(3000));
        mock.stop();
        QVERIFY(!mock.isRunning());
        mock.stop();   // twice is fine

        // QNAM against the same-thread mock: its 6 connections per host are all in flight at once
        m_mock.setLatency(QStringLiteral("/embeddings"), 300);
        QList<QNetworkReply*> replies;
        for (int i = 0; i < 6; ++i) {
            QNetworkRequest rq{QUrl(u(QStringLiteral("/embeddings")))};
            rq.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
            replies << m_nam.post(rq, compact(QJsonObject{{QStringLiteral("input"), QStringLiteral("x %1").arg(i)}}));
        }
        QVERIFY(QTest::qWaitFor([&]() { return std::all_of(replies.begin(), replies.end(), [](QNetworkReply* r) { return r->isFinished(); }); }, 5000));
        for (QNetworkReply* rp : replies) {
            QCOMPARE(rp->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
            rp->deleteLater();
        }
        m_mock.setLatency(QStringLiteral("/embeddings"), 0);
        QCOMPARE(m_mock.peakInFlight(QStringLiteral("/embeddings")), 6);
    }

    void uploadsAndFraming() {
        // A 20 MiB JSON upload: answered, its size recorded, the log copy cut at 16 MiB. Sent over a raw socket: QNAM
        // uploads to loopback at about 1.5 MB/s on this machine (Qt 6.9, Windows; 20 MiB = 14 s whatever the variant),
        // while the mock reads the same 20 MiB in a few hundred ms. A smaller QNAM upload follows.
        const QString payload(20 << 20, QLatin1Char('A'));
        const QJsonObject bigBody{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("p")},
                                  {QStringLiteral("input_references"), QJsonArray{QJsonObject{{QStringLiteral("data"), payload}}}}};
        const QByteArray bytes = compact(bigBody);
        {
            QElapsedTimer t;
            t.start();
            Raw c(m_mock.port());
            c.send(rawPost("/images", bytes));
            Resp r;
            QVERIFY(c.next(&r, 20000));
            QCOMPARE(r.status, 200);
            QVERIFY2(t.elapsed() < 10000, qPrintable(QString::number(t.elapsed())));
        }
        auto rec = m_mock.last(QStringLiteral("/images"));
        QVERIFY(rec.has_value());
        QCOMPARE(rec->bodySize, qint64(bytes.size()));
        QVERIFY(rec->bodyTruncated);
        QCOMPARE(rec->body.size(), qsizetype(16 << 20));
        QCOMPARE(rec->json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/img-1"));   // parsed from the full body
        QCOMPARE(rec->method, QByteArray("POST"));
        QCOMPARE(rec->header("content-type"), QByteArray("application/json"));
        const QByteArray mib = compact(QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("data"), QString(1 << 20, QLatin1Char('B'))}});
        Got g = call(m_nam, "POST", u(QStringLiteral("/images")), mib, 30000);
        QCOMPARE(g.status, 200);
        QCOMPARE(m_mock.last(QStringLiteral("/images"))->body, mib);
        QVERIFY(!m_mock.last(QStringLiteral("/images"))->bodyTruncated);

        // chunked, with an extension and a trailer, split across writes
        const QByteArray body = compact(QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/embed-1")}, {QStringLiteral("input"), QStringLiteral("chunked words here")}});
        {
            Raw c(m_mock.port());
            const qsizetype half = body.size() / 2;
            c.send("POST /embeddings HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\n" + QByteArray::number(half, 16) + ";ext=1\r\n" + body.left(half) + "\r\n");
            QTest::qWait(50);
            c.s.write(QByteArray::number(body.size() - half, 16) + "\r\n" + body.mid(half) + "\r\n0\r\nX-Trailer: yes\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
            QCOMPARE(jsonOf(r.body).value(QStringLiteral("data")).toArray().size(), 1);
            const auto last = m_mock.last(QStringLiteral("/embeddings"));
            QVERIFY(last->chunked);
            QCOMPARE(last->body, body);
            // Expect: 100-continue on the same keep-alive connection
            c.send("POST /embeddings HTTP/1.1\r\nHost: 127.0.0.1\r\nExpect: 100-continue\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n");
            QVERIFY(QTest::qWaitFor([&]() { c.read(); return c.buf.contains("100 Continue"); }, 3000));
            c.s.write(body);
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
            QCOMPARE(m_mock.last(QStringLiteral("/embeddings"))->connection, last->connection);
            // a bad chunk size
            c.send("POST /embeddings HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 400);
            QVERIFY(c.waitClosed());
        }

        // the body cap: 413 at once, the attempt still recorded with its declared size
        m_mock.setMaxBodyBytes(1000);
        {
            Raw c(m_mock.port());
            c.send(rawPost("/images", QByteArray(2000, 'x')));
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 413);
            QVERIFY(c.waitClosed());
        }
        m_mock.setMaxBodyBytes(qint64(256) << 20);
        rec = m_mock.last(QStringLiteral("/images"));
        QCOMPARE(rec->bodySize, qint64(2000));
        QVERIFY(rec->bodyTruncated);
        QVERIFY(rec->body.isEmpty());

        // pipelining: answers in order even when the first is slower; one connection
        m_mock.setLatency(QStringLiteral("/models"), 200);
        {
            Raw c(m_mock.port());
            c.send(rawGet("/models") + rawGet("/credits"));
            Resp a, b;
            QVERIFY(c.next(&a));
            QVERIFY(c.next(&b));
            QCOMPARE(jsonOf(a.body).value(QStringLiteral("data")).toArray().size(), 16);
            QVERIFY(jsonOf(b.body).value(QStringLiteral("data")).toObject().contains(QStringLiteral("total_credits")));
            QCOMPARE(m_mock.last(QStringLiteral("/models"))->connection, m_mock.last(QStringLiteral("/credits"))->connection);
        }
        m_mock.setLatency(QStringLiteral("/models"), 0);
        // keep-alive with QNAM: sequential requests reuse one connection
        QNetworkAccessManager nam;
        for (int i = 0; i < 3; ++i) QCOMPARE(get(nam, u(QStringLiteral("/credits"))).status, 200);
        const QList<MockRequest> three = m_mock.requests(QStringLiteral("/credits")).mid(1);
        QCOMPARE(three.size(), 3);
        QCOMPARE(three.at(0).connection, three.at(2).connection);
        QCOMPARE(three.at(2).seq, three.at(1).seq + 1);
        // protocol errors
        for (const auto& [raw, status] : {std::pair<QByteArray, int>{"GARBAGE\r\n\r\n", 400}, {"GET /models HTTP/2.0\r\n\r\n", 505}, {"GET /models HTTP/1.1\r\nNoColon\r\n\r\n", 400}}) {
            Raw c(m_mock.port());
            c.send(raw);
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, status);
            QVERIFY(c.waitClosed());
        }
        // HTTP/1.0 closes after the answer
        {
            Raw c(m_mock.port());
            c.send("GET /credits HTTP/1.0\r\n\r\n");
            Resp r;
            QVERIFY(c.next(&r));
            QCOMPARE(r.status, 200);
            QVERIFY(c.waitClosed());
        }
    }

    void helpers() {
        // the TS embedText rules: lower-case ASCII words, everything else separates
        QCOMPARE(MockOpenRouter::embedText(QStringLiteral("ABC abc")), MockOpenRouter::embedText(QStringLiteral("abc, abc!")));
        const QVector<double> uni = MockOpenRouter::embedText(QStringLiteral("Ünïcode wörds 42"));
        QCOMPARE(std::accumulate(uni.begin(), uni.end(), 0.0), 5.0);   // n code w rds 42
        QCOMPARE(MockOpenRouter::embedText(QString()).size(), 64);
        // schema sampling edge cases
        QCOMPARE(MockOpenRouter::sampleFromSchema(QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("exclusiveMinimum"), 3}}).toDouble(), 4.0);
        QCOMPARE(MockOpenRouter::sampleFromSchema(QJsonObject{{QStringLiteral("type"), QJsonArray{QStringLiteral("null")}}}), QJsonValue(QJsonValue::Null));
        QCOMPARE(MockOpenRouter::sampleFromSchema(QJsonObject{{QStringLiteral("type"), QStringLiteral("array")}, {QStringLiteral("maxItems"), 0}}).toArray().size(), 0);
        const QJsonObject self = QJsonDocument::fromJson(R"({"type":"object","properties":{"child":{"$ref":"\u0023"}}})").object();
        QVERIFY(MockOpenRouter::sampleFromSchema(self).isObject());   // recursion is bounded
        const QJsonObject all = QJsonDocument::fromJson(R"({"allOf":[{"type":"object","properties":{"a":{"type":"string"}}},{"properties":{"b":{"type":"boolean"}}}]})").object();
        const QJsonObject merged = MockOpenRouter::sampleFromSchema(all).toObject();
        QCOMPARE(merged.value(QStringLiteral("a")).toString(), QStringLiteral("mock a"));
        QCOMPARE(merged.value(QStringLiteral("b")), QJsonValue(false));
        QCOMPARE(MockOpenRouter::httpDate(QDateTime(QDate(1994, 11, 6), QTime(8, 49, 37), QTimeZone::UTC)), QByteArray("Sun, 06 Nov 1994 08:49:37 GMT"));
        // a missing fixtures dir fails start() with a reason, and nothing listens
        MockOpenRouter bad;
        MockOpenRouter::Options o;
        o.fixturesDir = QStringLiteral("C:/definitely/not/here");
        QVERIFY(!bad.start(o));
        QVERIFY(bad.errorString().contains(QLatin1String("models.json")));
        QVERIFY(!bad.isRunning());
        QCOMPARE(bad.port(), quint16(0));
    }

    // ---------------- ST-07a: the OpenRouter client ----------------

    // payload.h / limits.h against the TS (values computed with starmedia src/payload.ts and src/limits.ts) and the
    // exported data file.
    void payloadAndLimits() {
        namespace P = provider::payload;
        namespace L = provider::limits;
        QVERIFY2(L::loadError().isEmpty(), qPrintable(L::loadError()));
        // the C++ constants equal model-limits.json `payload` (exported from src/payload.ts)
        const QJsonObject pay = data::load(u"model-limits").value(QStringLiteral("payload")).toObject();
        QCOMPARE(qint64(pay.value(QStringLiteral("mb")).toDouble()), P::kMB);
        QCOMPARE(qint64(pay.value(QStringLiteral("openrouterMaxBody")).toDouble()), P::kOpenRouterMaxBody);
        QCOMPARE(qint64(pay.value(QStringLiteral("safeBody")).toDouble()), P::kSafeBody);
        QCOMPARE(qint64(pay.value(QStringLiteral("videosMaxBody")).toDouble()), P::kVideosMaxBody);
        QCOMPARE(qint64(pay.value(QStringLiteral("videosBody")).toObject().value(QStringLiteral("safe")).toDouble()), P::kVideosSafeBody);
        QCOMPARE(qint64(pay.value(QStringLiteral("defaultBody")).toObject().value(QStringLiteral("cap")).toDouble()), P::kOpenRouterMaxBody);
        QCOMPARE(pay.value(QStringLiteral("referenceImage")).toObject().value(QStringLiteral("maxSide")).toInt(), P::kReferenceImage.maxSide);
        QCOMPARE(qint64(pay.value(QStringLiteral("referenceImage")).toObject().value(QStringLiteral("maxBytes")).toDouble()), P::kReferenceImage.maxBytes);
        QCOMPARE(pay.value(QStringLiteral("firstFrame")).toObject().value(QStringLiteral("maxSide")).toInt(), P::kFirstFrame.maxSide);
        QCOMPARE(qint64(pay.value(QStringLiteral("firstFrame")).toObject().value(QStringLiteral("maxBytes")).toDouble()), P::kFirstFrame.maxBytes);
        QCOMPARE(pay.value(QStringLiteral("sttPieceSeconds")).toInt(), P::kSttPieceSeconds);
        QCOMPARE(qint64(pay.value(QStringLiteral("sttMaxSingle")).toDouble()), P::kSttMaxSingle);
        QCOMPARE(qint64(pay.value(QStringLiteral("uploadChunk")).toDouble()), P::kUploadChunk);
        // bodyCap by path, with or without the origin and /api/v1
        QCOMPARE(P::bodyCap(u"/videos").cap, P::kVideosMaxBody);
        QCOMPARE(P::bodyCap(u"/videos/abc/content").safe, P::kVideosSafeBody);
        QCOMPARE(P::bodyCap(u"https://openrouter.ai/api/v1/videos").safe, P::kVideosSafeBody);
        QCOMPARE(P::bodyCap(u"/images").safe, P::kSafeBody);
        QCOMPARE(P::bodyCap(u"http://127.0.0.1:9/api/v1/images?x=1").cap, P::kOpenRouterMaxBody);
        // fmtMB (JS toFixed, ties up)
        const QList<QPair<qint64, QString>> fmt = {{0, "0.0 MB"}, {1, "0.0 MB"}, {1048576, "1.0 MB"}, {1572864, "1.5 MB"}, {10485759, "10.0 MB"},
                                                   {10485760, "10 MB"}, {11010048, "11 MB"}, {52428800, "50 MB"}, {104857, "0.1 MB"},
                                                   {157286, "0.1 MB"}, {262144, "0.3 MB"}, {786432, "0.8 MB"}, {13107200, "13 MB"}, {32505856, "31 MB"}};
        for (const auto& [n, s] : fmt) QCOMPARE(P::fmtMB(n), s);
        QCOMPARE(P::b64Size(0), 0);
        QCOMPARE(P::b64Size(1), 4);
        QCOMPARE(P::b64Size(3), 4);
        QCOMPARE(P::b64Size(4), 8);
        QCOMPARE(P::fitBudget({10, 20, 30}, 35), 2);
        QCOMPARE(P::fitBudget({10, 20, 30}, 60), 3);
        QCOMPARE(P::fitBudget({5}, 4), 0);
        QCOMPARE(P::fitBudget({}, 4), 0);
        // WAV split: 2.5 s at 16 kHz into 1 s pieces, joined back byte for byte
        const QByteArray wav = makeWav(2.5);
        const auto info = P::wavInfo(wav);
        QVERIFY(info);
        QCOMPARE(info->sampleRate, 16000);
        QCOMPARE(info->dataBytes, qint64(80000));
        const auto pieces = P::splitWav(wav, 1.0);
        QCOMPARE(pieces.size(), 3);
        QByteArray joined;
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(pieces[i].offset, double(i));
            const auto pi = P::wavInfo(pieces[i].bytes);
            QVERIFY(pi);
            QCOMPARE(pi->dataBytes, i < 2 ? qint64(32000) : qint64(16000));
            QCOMPARE(MockOpenRouter::wavSeconds(pieces[i].bytes), i < 2 ? 1.0 : 0.5);
            joined += pieces[i].bytes.mid(44);
        }
        QCOMPARE(joined, wav.mid(44));
        QCOMPARE(P::splitWav(wav, 300).size(), 1);
        QCOMPARE(P::splitWav(QByteArray("not a wav at all, not even close to one........"), 1).first().offset, 0.0);
        QVERIFY(!P::wavInfo(QByteArray(40, 'x')));
        // limits.ts (TS answers for the same ids)
        QCOMPARE(L::ttsCharsFor(u"canopylabs/orpheus-3b-0.1-ft"), 285);
        QCOMPARE(L::ttsCharsFor(u"deepgram/aura-2"), 1900);
        QCOMPARE(L::ttsCharsFor(u"bytedance-seed/seed-audio-1-0"), 2400);
        QCOMPARE(L::ttsCharsFor(u"acme/none"), 2400);
        QCOMPARE(L::ttsCharsFor(u""), 2400);
        QCOMPARE(L::promptCharsFor(u"alibaba/wan-2.6"), std::optional<int>(1500));
        QCOMPARE(L::promptCharsFor(u"alibaba/happyhorse-1.0"), std::optional<int>(5000));
        QVERIFY(!L::promptCharsFor(u"acme/none"));
        const QList<std::tuple<QString, int, qint64>> stt = {{"assemblyai/universal-3-5-pro", 114, 25165824}, {"fish-audio/transcribe-1", 300, 19922944},
                                                            {"alibaba/wan-2.6", 300, 14942208}, {"openai/gpt-4o-transcribe", 300, 24903680},
                                                            {"acme/none", 300, 25165824}};
        for (const auto& [id, secs, bytes] : stt) {
            QCOMPARE(L::sttFor(id).pieceSeconds, secs);
            QCOMPARE(L::sttFor(id).maxSingleBytes, bytes);
        }
        QCOMPARE(L::imageLimitFor(u"runway/gen-4.5", false).maxBytes, P::kReferenceImage.maxBytes);
        QCOMPARE(L::imageLimitFor(u"runway/gen-4.5", true).maxBytes, P::kFirstFrame.maxBytes);
        QCOMPARE(L::requestCapFor(u"sourceful/riverflow-v2"), std::optional<qint64>(4718592));
        QVERIFY(!L::requestCapFor(u"acme/x"));
        QCOMPARE(L::researched(u"deepgram/flux-tts:free").value(QStringLiteral("id")).toString(), QStringLiteral("deepgram/flux-tts:free"));
        QCOMPARE(L::researched(u"alibaba/wan-2.6:free").value(QStringLiteral("id")).toString(), QStringLiteral("alibaba/wan-2.6"));
        QVERIFY(L::researched(u"acme/none").isEmpty());
        QVERIFY(L::retiredReason(u"openai/sora-2").contains(QLatin1String("shut down")));
        QVERIFY(L::retiredReason(u"openai/sora-2:free").contains(QLatin1String("shut down")));
        QVERIFY(L::retiredReason(u"google/veo-3.1").isEmpty());
        // fitPrompt: a sentence end past 60 %, a word boundary, a hard cut (lengths and tails from the TS)
        auto fit = [](const QString& p) { return L::fitPrompt(p, u"alibaba/wan-2.6"); };
        QString f1 = fit(QString(10, u'A') + QStringLiteral(". ") + QStringLiteral("b ").repeated(1000));
        QCOMPARE(f1.size(), 1499);
        QVERIFY(f1.endsWith(QLatin1String(" b b b b b b")));
        QString f2 = fit(QStringLiteral("word ").repeated(400));
        QCOMPARE(f2.size(), 1499);
        QVERIFY(f2.endsWith(QLatin1String("rd word word")));
        QCOMPARE(fit(QString(1600, u'x')).size(), 1500);
        QCOMPARE(fit(QStringLiteral("short")), QStringLiteral("short"));
        QCOMPARE(L::fitPrompt(QString(9000, u'x'), u"acme/none").size(), 9000);
        const QString sentence = QString(1000, u'a') + QStringLiteral(". ") + QString(800, u'b');
        QCOMPARE(fit(sentence), QString(1000, u'a') + QLatin1Char('.'));
        // videoRules (TS answers)
        using VF = L::VideoFit;
        QCOMPARE(L::videoRules(u"google/veo-3.1", VF{4, "1080p"}, {4, 6, 8}, {"720p", "1080p"}), (VF{8, "1080p"}));
        QCOMPARE(L::videoRules(u"google/veo-3.1-fast", VF{4, "720p"}, {4, 6, 8}, {"720p", "1080p"}), (VF{4, "720p"}));
        QCOMPARE(L::videoRules(u"google/veo-3.1", VF{4, "4k"}, {4, 6}, {"720p", "4k"}), (VF{4, "4k"}));
        QCOMPARE(L::videoRules(u"minimax/hailuo-2.3", VF{10, "1080P"}, {6, 10}, {"768P", "1080P"}), (VF{6, "1080P"}));
        QCOMPARE(L::videoRules(u"minimax/hailuo-2.3", VF{10, "1080p"}, {10}, {"768p", "1080p"}), (VF{10, "768p"}));
        QCOMPARE(L::videoRules(u"minimax/hailuo-2.3", VF{10, "1080p"}, {10}, {"1080p"}), (VF{10, "1080p"}));
        // rejectsVerboseJson reads the researched notes
        QVERIFY(L::rejectsVerboseJson(u"openai/gpt-4o-transcribe"));
        QVERIFY(L::rejectsVerboseJson(u"microsoft/mai-transcribe-1.5"));
        QVERIFY(!L::rejectsVerboseJson(u"acme/stt-1"));
        // limitsFor: catalog values over researched ones, empty / null catalog values ignored
        const QJsonObject lf = L::limitsFor(u"alibaba/wan-2.6", QJsonObject{{QStringLiteral("maxPromptChars"), 999}, {QStringLiteral("durations"), QString()},
                                                                            {QStringLiteral("contextTokens"), QJsonValue::Null}});
        QCOMPARE(lf.value(QStringLiteral("maxPromptChars")).toInt(), 999);
        QVERIFY(lf.value(QStringLiteral("durations")).toString().size() > 0);
        QVERIFY(!lf.value(QStringLiteral("sources")).toArray().isEmpty());
        QCOMPARE(L::limitsFor(u"acme/none", QJsonObject{{QStringLiteral("contextTokens"), 8000}}).value(QStringLiteral("confidence")).toString(), QStringLiteral("api"));
        QVERIFY(!L::platformLimits().value(QStringLiteral("platform")).toArray().isEmpty());
    }

    // stripFences / salvageJson against the TS answers (fixtures/salvage-golden.json, export-salvage-golden.mjs)
    void salvageGolden() {
        QFile f(QStringLiteral(STUDIO_TEST_DIR "/fixtures/salvage-golden.json"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonArray cases = doc.value(QStringLiteral("cases")).toArray();
        QVERIFY(cases.size() >= 40);
        for (const QJsonValue& v : cases) {
            const QJsonObject c = v.toObject();
            const QString in = c.value(QStringLiteral("in")).toString();
            const QByteArray label = json::jsString(in);
            QVERIFY2(provider::stripFences(in) == c.value(QStringLiteral("strip")).toString(), label.constData());
            const QJsonValue got = provider::salvageJson(in);
            if (c.value(QStringLiteral("undefined")).toBool()) {
                QVERIFY2(got.isUndefined(), label.constData());
            } else {
                const QJsonValue want = json::parse(c.value(QStringLiteral("out")).toString().toUtf8());
                QVERIFY2(!want.isUndefined(), label.constData());
                QVERIFY2(got == want, (label + " -> " + json::compact(got)).constData());
            }
        }
    }

    void clientChatToolsAndSchema() {
        Client cl(m_cfg, m_mock.apiUrl());
        m_mock.setRequiredKey(kTestKey);
        m_mock.setUsageCost(QStringLiteral("/chat/completions"), 0.0123);
        auto restore = qScopeGuard([this]() {
            m_mock.setRequiredKey({});
            m_mock.setUsageCost(QStringLiteral("/chat/completions"), -1);
        });
        // 1. a tool call
        const QJsonObject params{{QStringLiteral("type"), QStringLiteral("object")},
                                 {QStringLiteral("properties"), QJsonObject{{QStringLiteral("city"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}}}},
                                 {QStringLiteral("required"), QJsonArray{QStringLiteral("city")}}};
        ChatRequest rq = ask(QStringLiteral("What is the weather in Danville?"));
        rq.system = QStringLiteral("You are terse.");
        rq.tools = QJsonArray{fn(QStringLiteral("lookup"), params)};
        rq.toolChoice = QStringLiteral("auto");
        auto o = chatCall(cl.api, rq, this);
        QVERIFY(wait(o));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(o->chat.toolCalls.size(), 1);
        QCOMPARE(o->chat.toolCalls[0].name, QStringLiteral("lookup"));
        QVERIFY(!o->chat.toolCalls[0].id.isEmpty());
        QVERIFY(json::parseObject(o->chat.toolCalls[0].arguments.toUtf8()).value(QStringLiteral("city")).isString());
        QCOMPARE(o->chat.finishReason, QStringLiteral("tool_calls"));
        QCOMPARE(o->chat.message.value(QStringLiteral("role")).toString(), QStringLiteral("assistant"));
        QCOMPARE(o->chat.model, QStringLiteral("acme/writer-pro"));
        QCOMPARE(o->chat.costUsd, std::optional<double>(0.0123));
        QCOMPARE(o->chat.costMicro, std::optional<money::Micro>(12300));
        QCOMPARE(o->chat.promptTokens, std::optional<qint64>(1000));
        QCOMPARE(o->chat.completionTokens, std::optional<qint64>(500));
        QVERIFY(o->chat.generationId.startsWith(QLatin1String("gen-")));
        QCOMPARE(o->chat.attempts, 1);
        QVERIFY(!o->chat.http2);   // plain http to loopback: HTTP/1.1
        // what went on the wire: the TS body and headers (the key is compared, never printed)
        auto sent = m_mock.last(QStringLiteral("POST /chat/completions"));
        QVERIFY(sent);
        QVERIFY(sent->header("authorization") == "Bearer " + kTestKey);
        QCOMPARE(sent->header("x-title"), QByteArray("PlayPlay Studio"));
        QVERIFY(sent->header("user-agent").startsWith("PlayPlayStudio/"));
        QCOMPARE(sent->json.value(QStringLiteral("max_tokens")).toInt(), 4000);
        QCOMPARE(sent->json.value(QStringLiteral("usage")).toObject().value(QStringLiteral("include")), QJsonValue(true));
        QCOMPARE(sent->json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/writer-pro"));
        QCOMPARE(sent->json.value(QStringLiteral("tool_choice")).toString(), QStringLiteral("auto"));
        QCOMPARE(sent->json.value(QStringLiteral("provider")).toObject().value(QStringLiteral("require_parameters")), QJsonValue(false));
        QCOMPARE(sent->json.value(QStringLiteral("messages")).toArray().size(), 2);
        QCOMPARE(sent->json.value(QStringLiteral("messages")).toArray().at(0).toObject().value(QStringLiteral("role")).toString(), QStringLiteral("system"));
        QVERIFY(!sent->json.contains(QStringLiteral("plugins")));
        // 2. the tool result, then the final answer
        ChatRequest rq2 = ask(QString());
        QJsonArray msgs{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), QStringLiteral("What is the weather in Danville?")}},
                        o->chat.message,
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")}, {QStringLiteral("tool_call_id"), o->chat.toolCalls[0].id},
                                    {QStringLiteral("content"), QStringLiteral("sunny, 21 C")}}};
        rq2.messages = msgs;
        rq2.tools = rq.tools;
        rq2.toolChoice = QStringLiteral("auto");
        auto o2 = chatCall(cl.api, rq2, this);
        QVERIFY(wait(o2));
        QVERIFY(!o2->err);
        QVERIFY2(o2->chat.text.startsWith(QLatin1String("[mock] tool result: ")), qPrintable(o2->chat.text));
        QVERIFY(o2->chat.text.contains(QLatin1String("sunny")));
        QVERIFY(o2->chat.toolCalls.isEmpty());
        QCOMPARE(m_mock.last(QStringLiteral("POST /chat/completions"))->json.value(QStringLiteral("messages")).toArray().size(), 3);
        // 3. json_schema: strict structured output, the response-healing plugin, require_parameters
        ChatRequest rq3 = ask(QStringLiteral("Give me a title."));
        rq3.jsonSchema = ChatRequest::JsonSchema{QStringLiteral("st07a_shape"),
                                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                                                             {QStringLiteral("properties"), QJsonObject{{QStringLiteral("title"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                                                                                                        {QStringLiteral("n"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 2}}}}},
                                                             {QStringLiteral("required"), QJsonArray{QStringLiteral("title"), QStringLiteral("n")}},
                                                             {QStringLiteral("additionalProperties"), false}}};
        rq3.reasoningEffort = QStringLiteral("none");
        rq3.temperature = 0.2;
        rq3.provider = QJsonObject{{QStringLiteral("sort"), QStringLiteral("price")}};
        auto o3 = chatCall(cl.api, rq3, this);
        QVERIFY(wait(o3));
        QVERIFY(!o3->err);
        QVERIFY(o3->chat.json.isObject());
        QVERIFY(!o3->chat.jsonSalvaged);
        QVERIFY(o3->chat.json.toObject().value(QStringLiteral("title")).isString());
        QVERIFY(o3->chat.json.toObject().value(QStringLiteral("n")).toDouble() >= 2);
        sent = m_mock.last(QStringLiteral("POST /chat/completions"));
        const QJsonObject rf = sent->json.value(QStringLiteral("response_format")).toObject();
        QCOMPARE(rf.value(QStringLiteral("type")).toString(), QStringLiteral("json_schema"));
        QCOMPARE(rf.value(QStringLiteral("json_schema")).toObject().value(QStringLiteral("strict")), QJsonValue(true));
        QCOMPARE(rf.value(QStringLiteral("json_schema")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("st07a_shape"));
        QCOMPARE(sent->json.value(QStringLiteral("plugins")).toArray().at(0).toObject().value(QStringLiteral("id")).toString(), QStringLiteral("response-healing"));
        QCOMPARE(sent->json.value(QStringLiteral("provider")).toObject().value(QStringLiteral("require_parameters")), QJsonValue(true));
        QCOMPARE(sent->json.value(QStringLiteral("provider")).toObject().value(QStringLiteral("sort")).toString(), QStringLiteral("price"));
        QCOMPARE(sent->json.value(QStringLiteral("reasoning")).toObject(), (QJsonObject{{QStringLiteral("enabled"), false}}));
        QCOMPARE(sent->json.value(QStringLiteral("temperature")).toDouble(), 0.2);
        // 4. json_object mode, a fallback list (sent as `models`, no quota key) and the web-search server tool
        ChatRequest rq4 = ask(QStringLiteral("json please"));
        rq4.models = {QStringLiteral("acme/writer-pro"), QStringLiteral("beta/writer-2")};
        rq4.jsonSchema = ChatRequest::JsonSchema{QStringLiteral("free_form"), QJsonObject{}};
        rq4.jsonMode = QStringLiteral("json_object");
        rq4.reasoningEffort = QStringLiteral("high");
        auto o4 = chatCall(cl.api, rq4, this);
        QVERIFY(wait(o4));
        QVERIFY(!o4->err);
        QCOMPARE(o4->chat.json.toObject().value(QStringLiteral("result")).toString(), QStringLiteral("mock"));
        sent = m_mock.last(QStringLiteral("POST /chat/completions"));
        QCOMPARE(sent->json.value(QStringLiteral("response_format")).toObject(), (QJsonObject{{QStringLiteral("type"), QStringLiteral("json_object")}}));
        QCOMPARE(sent->json.value(QStringLiteral("models")).toArray().size(), 2);
        QVERIFY(!sent->json.contains(QStringLiteral("model")));
        // (the web-search server tool runs inside OpenRouter; the mock answers it as a function call, so only the body is
        // checked here)
        rq4.webSearch = ChatRequest::WebSearch{};
        rq4.webSearch->maxUses = 2;
        rq4.toolChoice = QStringLiteral("auto");
        auto o5 = chatCall(cl.api, rq4, this);
        QVERIFY(wait(o5));
        QVERIFY(!o5->err);
        sent = m_mock.last(QStringLiteral("POST /chat/completions"));
        QVERIFY(!sent->json.contains(QStringLiteral("tool_choice")));   // only with function tools
        const QJsonObject ws = sent->json.value(QStringLiteral("tools")).toArray().at(0).toObject();
        QCOMPARE(ws.value(QStringLiteral("type")).toString(), QStringLiteral("openrouter:web_search"));
        QCOMPARE(ws.value(QStringLiteral("parameters")).toObject(), (QJsonObject{{QStringLiteral("max_results"), 5}, {QStringLiteral("max_uses"), 2}}));
        QCOMPARE(sent->json.value(QStringLiteral("reasoning")).toObject(), (QJsonObject{{QStringLiteral("effort"), QStringLiteral("high")}}));
        // the body for a local / pool provider: no routing prefs, plugins or fallback list
        const QJsonObject local = provider::chatBody(rq4, true);
        QVERIFY(!local.contains(QStringLiteral("plugins")));
        QVERIFY(!local.contains(QStringLiteral("models")));
        QVERIFY(!local.contains(QStringLiteral("provider")));
        QCOMPARE(local.value(QStringLiteral("model")).toString(), QStringLiteral("acme/writer-pro"));
        // parseChat details: content parts, citations (both shapes), call ids, finish reason, reasoning
        const QJsonObject data = json::parseObject(
            "{\"id\":\"gen-x\",\"model\":\"acme/writer-pro\",\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
            "\"content\":[{\"type\":\"text\",\"text\":\"{\\\"a\\\":\"},{\"type\":\"text\",\"text\":\"1}\"},{\"type\":\"image\"}],"
            "\"reasoning\":\"  thought  \",\"annotations\":[{\"type\":\"url_citation\",\"url_citation\":{\"url\":\"https://a.example/x\",\"title\":\"A\"}},"
            "{\"type\":\"url_citation\",\"url\":\"https://b.example/\"},{\"type\":\"url_citation\",\"url_citation\":{}},{\"type\":\"file\"}],"
            "\"tool_calls\":[{\"function\":{\"name\":\"f\",\"arguments\":{\"k\":1}}},{\"id\":\"x\",\"function\":{}},{\"id\":7,\"function\":{\"name\":\"g\"}}]}}],"
            "\"usage\":{\"cost\":0.5,\"completion_tokens_details\":{\"reasoning_tokens\":12}}}");
        ChatRequest withSchema = ask(QString());
        withSchema.jsonSchema = ChatRequest::JsonSchema{QStringLiteral("x"), QJsonObject{}};
        const ChatResult pr = provider::parseChat(data, withSchema);
        QCOMPARE(pr.text, QStringLiteral("{\"a\":1}"));
        QCOMPARE(pr.json, QJsonValue(QJsonObject{{QStringLiteral("a"), 1}}));
        QCOMPARE(pr.citations.size(), 2);
        QCOMPARE(pr.citations[0].title, QStringLiteral("A"));
        QCOMPARE(pr.citations[1].url, QStringLiteral("https://b.example/"));
        QCOMPARE(pr.toolCalls.size(), 2);
        QCOMPARE(pr.toolCalls[0].id, QStringLiteral("call_0"));
        QCOMPARE(pr.toolCalls[0].arguments, QStringLiteral("{\"k\":1}"));
        QCOMPARE(pr.toolCalls[1].id, QStringLiteral("7"));
        QCOMPARE(pr.toolCalls[1].arguments, QStringLiteral("{}"));
        QCOMPARE(pr.reasoning, QStringLiteral("  thought  "));
        QCOMPARE(pr.reasoningTokens, std::optional<qint64>(12));
        QCOMPARE(pr.costMicro, std::optional<money::Micro>(500000));
        QCOMPARE(pr.finishReason, QStringLiteral("stop"));
        QCOMPARE(pr.generationId, QStringLiteral("gen-x"));
        QVERIFY(provider::parseChat(data, ask(QString())).json.isUndefined());   // json only for schema requests
    }

    void clientSalvage() {
        Client cl(m_cfg, m_mock.apiUrl());
        // a model that wraps its JSON in prose and is cut off by the token limit
        m_mock.setChatHandler([](const MockRequest& rq) -> std::optional<QJsonObject> {
            if (!rq.json.contains(QStringLiteral("response_format"))) return std::nullopt;
            return QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                               {QStringLiteral("content"), QStringLiteral("Sure! Here it is:\n```json\n{\"shots\":[{\"n\":1},{\"n\":2},{\"n\":")},
                               {QStringLiteral("finish_reason"), QStringLiteral("length")}};
        });
        auto restore = qScopeGuard([this]() { m_mock.setChatHandler({}); });
        ChatRequest rq = ask(QStringLiteral("shots"));
        rq.jsonSchema = ChatRequest::JsonSchema{QStringLiteral("shots"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}};
        auto o = chatCall(cl.api, rq, this);
        QVERIFY(wait(o));
        QVERIFY(!o->err);
        QVERIFY(o->chat.jsonSalvaged);
        QCOMPARE(o->chat.finishReason, QStringLiteral("length"));
        QCOMPARE(o->chat.json, json::parse("{\"shots\":[{\"n\":1},{\"n\":2}]}"));
        // nothing salvageable: json stays Undefined, the text is still there
        m_mock.setChatHandler([](const MockRequest&) -> std::optional<QJsonObject> {
            return QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QStringLiteral("I cannot do that.")}};
        });
        auto o2 = chatCall(cl.api, rq, this);
        QVERIFY(wait(o2));
        QVERIFY(!o2->err);
        QVERIFY(o2->chat.json.isUndefined());
        QVERIFY(!o2->chat.jsonSalvaged);
        QCOMPARE(o2->chat.text, QStringLiteral("I cannot do that."));
    }

    // The money rule: a paid POST that may have reached the provider is never sent twice.
    void clientPaidPostNeverResent() {
        Client cl(m_cfg, m_mock.apiUrl());
        const QJsonObject img{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a red barn")}};
        // 1. a timed-out paid POST: outcome unknown, sent once, not retried (even with retries left)
        m_mock.script(QStringLiteral("POST /images"), MockReply::normal().delayed(2500));
        auto o = wireSend(cl.wire, postRq(QStringLiteral("/images"), img, 400, 3), this);
        QVERIFY(wait(o));
        QVERIFY(o->err);
        QCOMPARE(o->err->kind, WireError::Kind::Timeout);
        QVERIFY(o->err->outcomeUnknown);
        QCOMPARE(o->err->attempts, 1);
        QVERIFY2(o->err->message.contains(QLatin1String("timed out after 0 s waiting for /images")), qPrintable(o->err->message));
        QVERIFY(o->err->message.contains(QLatin1String("it was not sent again, so nothing is charged twice")));
        QVERIFY(o->ms < 2000);
        QTest::qWait(600);   // long past every back-off (20, 40, 80 ms): still one request
        QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 1);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("unknownOutcome")).toInt(), 1);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("oneShot")).toInt(), 1);
        QCOMPARE(cl.gates.gate(provider::GateKind::Images).running(), 0);   // the slot went back
        m_mock.releaseStalled();
        // the same timeout on chat (not a paid gate in TS terms) is retried, through QNAM
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::normal().delayed(2500));
        ChatRequest c = ask(QStringLiteral("hello"));
        c.timeoutMs = 400;
        c.retries = 1;
        auto oc = chatCall(cl.api, c, this);
        QVERIFY(wait(oc));
        QVERIFY2(!oc->err, oc->err ? qPrintable(oc->err->message) : "");
        QCOMPARE(oc->chat.attempts, 2);
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 2);
        // 2. a paid POST whose connection is dropped / reset after the request was written: unknown, sent once
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /videos"), MockReply::dropped(), 4);
        auto ov = wireSend(cl.wire, postRq(QStringLiteral("/videos"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), QStringLiteral("x")}}), this);
        QVERIFY(wait(ov));
        QCOMPARE(ov->err->kind, WireError::Kind::Network);
        QVERIFY(ov->err->outcomeUnknown);
        QTest::qWait(300);
        QCOMPARE(m_mock.count(QStringLiteral("POST /videos")), 1);
        m_mock.clearScripts();
        m_mock.script(QStringLiteral("POST /audio/speech"), MockReply::resetConnection(), 4);
        auto oa = wireSend(cl.wire, postRq(QStringLiteral("/audio/speech"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/tts-1")}, {QStringLiteral("input"), QStringLiteral("hi")}}), this);
        QVERIFY(wait(oa));
        QCOMPARE(oa->err->kind, WireError::Kind::Network);
        QVERIFY(oa->err->outcomeUnknown);
        QTest::qWait(300);
        QCOMPARE(m_mock.count(QStringLiteral("POST /audio/speech")), 1);
        m_mock.clearScripts();
        // why the one-shot transport exists: QNAM, given the same dropped connection, may send the POST again on its own
        {
            Client q(m_cfg, m_mock.apiUrl());
            q.options([](provider::WireOptions& w) { w.paidOneShot = false; });
            m_mock.clearRequests();
            m_mock.script(QStringLiteral("POST /videos"), MockReply::dropped(), 4);
            auto oq = wireSend(q.wire, postRq(QStringLiteral("/videos"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}}), this);
            QVERIFY(wait(oq));
            QVERIFY(oq->err && oq->err->outcomeUnknown);   // our own retry is still refused ...
            QTest::qWait(300);
            qInfo("QNAM (diagnostic mode) sent the dropped paid POST %d time(s); the one-shot transport sent it once",
                  m_mock.count(QStringLiteral("POST /videos")));   // ... but QNAM's own re-send is out of our hands
            m_mock.clearScripts();
        }
        // 3. nothing left this process (a TLS handshake against the plain-HTTP mock fails before the request is
        // written): a paid POST may be retried, and it is not "unknown"
        Client tls(m_cfg, m_mock.apiUrl());
        tls.options([this](provider::WireOptions& w) { w.providers.insert(QStringLiteral("tls"), QStringLiteral("https://127.0.0.1:%1/v1").arg(m_mock.port())); });
        m_mock.clearRequests();
        // (the mock never answers a ClientHello: each attempt times out before the request was written)
        auto od = wireSend(tls.wire, postRq(QStringLiteral("/images"), QJsonObject{{QStringLiteral("model"), QStringLiteral("tls/img")}}, 300, 2), this);
        QVERIFY(wait(od));
        QVERIFY(od->err->kind == WireError::Kind::Timeout || od->err->kind == WireError::Kind::Network);
        QVERIFY(!od->err->outcomeUnknown);
        QCOMPARE(od->err->attempts, 3);
        QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 0);
        // a closed port: nothing is sent either (Windows takes about 2 s to give up on a refused loopback connect, so
        // the 3 s timer may come first; a timeout before the connection was made is not "unknown" either)
        Client dead(m_cfg, m_mock.apiUrl());
        dead.options([](provider::WireOptions& w) { w.providers.insert(QStringLiteral("dead"), QStringLiteral("http://127.0.0.1:%1/v1").arg(closedPort())); });
        auto ox = wireSend(dead.wire, postRq(QStringLiteral("/images"), QJsonObject{{QStringLiteral("model"), QStringLiteral("dead/img")}}, 3000, 0), this);
        QVERIFY(wait(ox));
        QVERIFY(ox->err->kind == WireError::Kind::Network || ox->err->kind == WireError::Kind::Timeout);
        QVERIFY(!ox->err->outcomeUnknown);
        // 4. an ANSWERED 5xx on a paid POST is retried (the server said no; TS behaviour)
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /images"), MockReply::serverError());
        auto o5 = wireSend(cl.wire, postRq(QStringLiteral("/images"), img), this);
        QVERIFY(wait(o5));
        QVERIFY2(o5->resp, o5->err ? qPrintable(o5->err->message) : "");
        QCOMPARE(o5->resp->status, 200);
        QCOMPARE(o5->resp->attempts, 2);
        QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 2);
        QVERIFY(o5->resp->json().toObject().value(QStringLiteral("data")).toArray().size() == 1);
        // 5. Stop while a paid POST is on the wire: cancelled, and flagged unknown (it may run anyway)
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /videos"), MockReply::normal().delayed(3000));
        provider::AbortController stop;
        Request vr = postRq(QStringLiteral("/videos"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}});
        vr.signal = stop.signal();
        auto os = wireSend(cl.wire, vr, this);
        QVERIFY(m_mock.waitForRequests(QStringLiteral("POST /videos"), 1));
        stop.abort();
        QVERIFY(wait(os, 2000));
        QCOMPARE(os->err->kind, WireError::Kind::Cancelled);
        QVERIFY(os->err->outcomeUnknown);
        QCOMPARE(m_mock.count(QStringLiteral("POST /videos")), 1);
        m_mock.releaseStalled();
        QCOMPARE(cl.wire.pending(), 0);
    }

    void clientRefusesOversize() {
        Client cl(m_cfg, m_mock.apiUrl());
        namespace P = provider::payload;
        auto big = [](const QString& path, qint64 n, const QString& model = {}) {
            Request rq;
            rq.method = "POST";
            rq.path = path;
            rq.body = QByteArray(n, 'a');
            rq.model = model;
            return rq;
        };
        // over the safe budget of /images (48 MiB): 413 before a byte is sent, no gate slot used
        auto o = wireSend(cl.wire, big(QStringLiteral("/images"), P::kSafeBody + 1), this);
        QVERIFY(wait(o));
        QCOMPARE(o->err->kind, WireError::Kind::TooLarge);
        QCOMPARE(o->err->status, 413);
        QVERIFY(o->err->sentNothing);
        QCOMPARE(o->err->attempts, 0);
        QVERIFY2(o->err->message.startsWith(QLatin1String("413 Request too large (this request was 48 MB) for /images: OpenRouter accepts at most 50 MB")),
                 qPrintable(o->err->message));
        QVERIFY(o->err->message.endsWith(QLatin1String("Details: refused before sending")));
        // /videos has the smaller cap (29 MiB safe, 31 MiB OpenRouter)
        auto ov = wireSend(cl.wire, big(QStringLiteral("/videos"), P::kVideosSafeBody + 1), this);
        QVERIFY(wait(ov));
        QCOMPARE(ov->err->kind, WireError::Kind::TooLarge);
        QVERIFY(ov->err->message.contains(QLatin1String("at most 31 MB")));
        // a provider's own whole-request cap (Sourceful 4.5 MB)
        auto os = wireSend(cl.wire, big(QStringLiteral("/images"), 4718592 + 1, QStringLiteral("sourceful/riverflow-v2")), this);
        QVERIFY(wait(os));
        QCOMPARE(os->err->kind, WireError::Kind::TooLarge);
        QVERIFY2(os->err->message.contains(QLatin1String("sourceful/riverflow-v2 accepts at most 4.5 MB")), qPrintable(os->err->message));
        QCOMPARE(m_mock.count(), 0);
        QCOMPARE(cl.gates.gate(provider::GateKind::Images).served(), quint64(0));
        QCOMPARE(cl.gates.gate(provider::GateKind::Videos).served(), quint64(0));
        QCOMPARE(cl.wire.stats().value(QStringLiteral("refusedBeforeSend")).toInt(), 3);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("sent")).toInt(), 0);
        // just under the cap of the provider goes out
        auto ok = wireSend(cl.wire, big(QStringLiteral("/embeddings"), 4096, QStringLiteral("sourceful/riverflow-v2")), this);
        QVERIFY(wait(ok));
        QVERIFY(ok->resp || (ok->err && ok->err->status > 0));   // answered (the mock reads junk JSON as it can)
        // an upstream 413 answer: worded the TS way, not retried
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /embeddings"), MockReply::tooLarge());
        auto ou = wireSend(cl.wire, postRq(QStringLiteral("/embeddings"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/embed-1")}}), this);
        QVERIFY(wait(ou));
        QCOMPARE(ou->err->kind, WireError::Kind::TooLarge);
        QCOMPARE(ou->err->status, 413);
        QVERIFY2(ou->err->message.startsWith(QLatin1String("413 Request too large")), qPrintable(ou->err->message));
        QVERIFY(ou->err->message.contains(QLatin1String("Details: ")));
        QCOMPARE(m_mock.count(QStringLiteral("POST /embeddings")), 1);
    }

    // HTTP/2: every answer carries Http2WasUsedAttribute, and each origin's protocol is logged once at info.
    void clientHttp2Logged() {
        log::Options lo;
        lo.minLevel = log::Level::Debug;
        QVERIFY(log::start(lo));
        auto stopLog = qScopeGuard([]() { log::stop(); });
        // lines queued by earlier tests are written at start: look only at what follows this marker
        log::info(u"test", QStringLiteral("---- clientHttp2Logged ----"));
        Client cl(m_cfg, m_mock.apiUrl());
        auto o = std::make_shared<Outcome>();
        cl.api.getJson(QStringLiteral("/models?output_modalities=text&secret=sig123"), this,
                       [o](const QJsonValue& v, const Response& r) { o->data = v; o->resp = r; o->done = true; },
                       [o](const WireError& e) { o->err = e; o->done = true; });
        QVERIFY(wait(o));
        QVERIFY(!o->err);
        QVERIFY(o->data.toObject().value(QStringLiteral("data")).toArray().size() >= 16);
        QVERIFY(!o->resp->http2);
        QCOMPARE(o->resp->attempts, 1);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("http1")).toInt(), 1);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("http2")).toInt(), 0);
        // a second call: no second protocol line
        auto o2 = std::make_shared<Outcome>();
        cl.api.getJson(QStringLiteral("/credits"), this, [o2](const QJsonValue&, const Response&) { o2->done = true; },
                       [o2](const WireError&) { o2->done = true; });
        QVERIFY(wait(o2));
        QVERIFY(log::flush());
        QStringList lines = log::recent(500);
        qsizetype mark = -1;
        for (qsizetype i = 0; i < lines.size(); ++i)
            if (lines[i].contains(QLatin1String("---- clientHttp2Logged ----"))) mark = i;
        QVERIFY(mark >= 0);
        lines = lines.mid(mark);
        const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(m_mock.port());
        const auto protocolLines = lines.filter(QStringLiteral("answered over HTTP/1.1 (Http2WasUsedAttribute=false)"));
        QCOMPARE(protocolLines.filter(origin).size(), 1);
        QVERIFY(!lines.filter(QStringLiteral("/models <- 200")).filter(QStringLiteral("http/1.1")).isEmpty());
        // diagnostics carry the path only: no query string, no key
        for (const QString& l : lines) {
            QVERIFY2(!l.contains(QLatin1String("sig123")), qPrintable(l));
            QVERIFY(!l.contains(QString::fromLatin1(kTestKey)));
        }
    }

    void clientRetriesAndLimits() {
        Client cl(m_cfg, m_mock.apiUrl());
        QSignalSpy limits(&cl.wire, &provider::Wire::providerLimit);
        // a rate limit twice (retry-after-ms 40), then an answer: the gate was cut, the call succeeds on attempt 3
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::rateLimitedMs(40), 2);
        auto o = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(o));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(o->chat.attempts, 3);
        QCOMPARE(limits.count(), 2);
        QCOMPARE(limits.at(0).at(0).toJsonObject().value(QStringLiteral("willRetry")), QJsonValue(true));
        QCOMPARE(limits.at(0).at(0).toJsonObject().value(QStringLiteral("gate")).toString(), QStringLiteral("chat"));
        QVERIFY(cl.gates.gate(provider::GateKind::Chat).statusJson().value(QStringLiteral("cuts")).toInt() >= 1);
        QVERIFY(o->ms >= 60);
        // a used-up daily quota: not retried, remembered (breaker + model health), the next call is refused locally
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::dailyQuota(), 3);
        auto oq = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(oq));
        QCOMPARE(oq->err->kind, WireError::Kind::Limit);
        QVERIFY(oq->err->hopeless());
        QCOMPARE(oq->err->limit->kind, provider::LimitKind::Quota);
        QCOMPARE(oq->err->status, 429);
        QVERIFY2(oq->err->message.contains(QLatin1String("the daily quota for acme/writer-pro is used up")), qPrintable(oq->err->message));
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        QVERIFY(cl.gates.breaker().check(QStringLiteral("acme/writer-pro")));
        QVERIFY(cl.health.coolingOf(QStringLiteral("acme/writer-pro")));
        auto oq2 = chatCall(cl.api, ask(QStringLiteral("again")), this);
        QVERIFY(wait(oq2));
        QVERIFY(oq2->err->limit && oq2->err->limit->remembered);
        QVERIFY(oq2->err->sentNothing);
        QVERIFY(oq2->err->message.contains(QLatin1String("nothing was sent now")));
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        QCOMPARE(classifyError(oq2->err->toProviderError()).kind, provider::ErrorKind::Quota);
        cl.gates.clearQuota();
        QVERIFY(!cl.health.coolingOf(QStringLiteral("acme/writer-pro")));
        m_mock.clearScripts();
        // StarMedia's spend cap: not retried, its own words
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::spendCap());
        auto oc = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(oc));
        QCOMPARE(oc->err->limit->kind, provider::LimitKind::Cap);
        QVERIFY(oc->err->message.startsWith(QLatin1String("429 ")));
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        // 402 no credit: a payment error, not a limit and not retried
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::noCredit());
        auto on = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(on));
        QCOMPARE(on->err->kind, WireError::Kind::Http);
        QCOMPARE(on->err->status, 402);
        QVERIFY(!on->err->limit);
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        // 503 once, then fine
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::unavailable());
        auto ou = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(ou));
        QVERIFY(!ou->err);
        QCOMPARE(ou->chat.attempts, 2);
        // 502 "Provider returned error" four times: retried three times, then the provider's own words
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::providerError(502, QStringLiteral("Google"), QStringLiteral("upstream exploded")), 4);
        auto op = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(op));
        QCOMPARE(op->err->status, 502);
        QCOMPARE(op->err->attempts, 4);
        QCOMPARE(op->err->provider, QStringLiteral("Google"));
        QVERIFY2(op->err->message.contains(QLatin1String("(Google: upstream exploded)")), qPrintable(op->err->message));
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 4);
        // a wait too long to sit through (Retry-After 120 s): not retried, and the model is remembered
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::rateLimited(120));
        auto ol = chatCall(cl.api, ask(QStringLiteral("hi"), QStringLiteral("beta/writer-2")), this);
        QVERIFY(wait(ol));
        QCOMPARE(ol->err->limit->kind, provider::LimitKind::Rate);
        QVERIFY(ol->err->message.contains(QLatin1String("too long to sit through")));
        QVERIFY(ol->err->message.contains(QLatin1String("It asks for 2 min.")));
        QVERIFY(cl.gates.breaker().check(QStringLiteral("beta/writer-2")));
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        cl.gates.reset();
        // the local server's "busy" 503: one short wait, then said in words
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::unavailable().withHeader("x-local-busy", "1"), 3);
        auto ob = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(ob));
        QCOMPARE(ob->err->status, 503);
        QCOMPARE(ob->err->attempts, 2);
        QVERIFY(ob->err->message.contains(QLatin1String("The local server is busy")));
        m_mock.clearScripts();
        // an answer that is not JSON
        m_mock.script(QStringLiteral("GET /credits"), MockReply::raw("<html>oops</html>", "text/html"));
        auto ox = std::make_shared<Outcome>();
        cl.api.getJson(QStringLiteral("/credits"), this, [ox](const QJsonValue&, const Response&) { ox->done = true; },
                       [ox](const WireError& e) { ox->err = e; ox->done = true; });
        QVERIFY(wait(ox));
        QVERIFY(ox->err && ox->err->message.contains(QLatin1String("not JSON")));
        // GETs go through the poll gate
        QVERIFY(cl.gates.gate(provider::GateKind::Poll).served() >= 1);
    }

    void clientCancel() {
        Client cl(m_cfg, m_mock.apiUrl());
        // a Stop while waiting in the gate line: refused at once, nothing sent
        provider::GateLimits gl;
        gl[provider::GateKind::Chat] = 1;
        cl.gates.configure(gl);
        m_mock.setLatency(QStringLiteral("/chat/completions"), 800);
        auto restore = qScopeGuard([this]() { m_mock.setLatency(QStringLiteral("/chat/completions"), 0); });
        auto first = chatCall(cl.api, ask(QStringLiteral("one")), this);
        provider::AbortController stop;
        ChatRequest second = ask(QStringLiteral("two"));
        second.signal = stop.signal();
        auto o = chatCall(cl.api, second, this);
        QTest::qWait(100);
        QCOMPARE(cl.gates.gate(provider::GateKind::Chat).queued(), 1);
        stop.abort();
        QVERIFY(wait(o, 1000));
        QCOMPARE(o->err->kind, WireError::Kind::Cancelled);
        QCOMPARE(o->err->message, QStringLiteral("Cancelled"));
        QVERIFY(o->err->sentNothing);
        QVERIFY(!o->err->outcomeUnknown);
        QVERIFY(wait(first));
        QVERIFY(!first->err);
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        m_mock.setLatency(QStringLiteral("/chat/completions"), 0);
        // a Stop during the retry sleep ends it
        cl.options([](provider::WireOptions& w) { w.backoffBaseMs = 5000; });
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::serverError());
        provider::AbortController stop2;
        ChatRequest third = ask(QStringLiteral("three"));
        third.signal = stop2.signal();
        auto o3 = chatCall(cl.api, third, this);
        QVERIFY(m_mock.waitForRequests(QStringLiteral("POST /chat/completions"), 1));
        QTest::qWait(150);
        stop2.abort(QStringLiteral("operator pressed Stop"));
        QVERIFY(wait(o3, 1000));
        QCOMPARE(o3->err->kind, WireError::Kind::Cancelled);
        QCOMPARE(o3->err->message, QStringLiteral("operator pressed Stop"));
        QVERIFY(o3->ms < 2000);
        QCOMPARE(m_mock.count(QStringLiteral("POST /chat/completions")), 1);
        // already stopped: nothing at all
        m_mock.clearRequests();
        provider::AbortController stop3;
        stop3.abort();
        ChatRequest fourth = ask(QStringLiteral("four"));
        fourth.signal = stop3.signal();
        auto o4 = chatCall(cl.api, fourth, this);
        QVERIFY(wait(o4, 1000));
        QCOMPARE(o4->err->kind, WireError::Kind::Cancelled);
        QCOMPARE(o4->err->attempts, 0);
        // the context goes away: no callback, the call still ends
        cl.options([](provider::WireOptions& w) { w.backoffBaseMs = 20; });
        auto ctx = std::make_unique<QObject>();
        bool called = false;
        cl.api.chat(ask(QStringLiteral("five")), ctx.get(), [&called](const ChatResult&) { called = true; }, [&called](const WireError&) { called = true; });
        ctx.reset();
        QVERIFY(QTest::qWaitFor([&]() { return cl.wire.pending() == 0; }, 5000));
        QTest::qWait(50);
        QVERIFY(!called);
        // callbacks are never run inside request(), even for an answer known at once
        m_mock.clearRequests();
        bool inside = true;
        bool ran = false;
        cl.wire.request(Request{"GET", QStringLiteral("not a url")}, this, [&](const Response&) { ran = true; }, [&](const WireError& e) {
            ran = true;
            QVERIFY(!inside);
            QCOMPARE(e.kind, WireError::Kind::Invalid);
        });
        inside = false;
        QVERIFY(QTest::qWaitFor([&]() { return ran; }, 1000));
        QCOMPARE(m_mock.count(), 0);
    }

    // Review fix: a Stop whose queued stop() comes AFTER the timeout (or a drop) took the transfer down must still say
    // outcomeUnknown for a paid POST that was written. A fake clock fires the timeout in the same loop turn as the Stop.
    void clientStopRaceKeepsUnknown() {
        FakeClock fake;
        Client cl(m_cfg, m_mock.apiUrl(), fake);
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /videos"), MockReply::normal().delayed(5000));
        provider::AbortController stop;
        Request vr = postRq(QStringLiteral("/videos"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), QStringLiteral("x")}}, 1000, 3);
        vr.signal = stop.signal();
        auto o = wireSend(cl.wire, vr, this);
        QVERIFY(QTest::qWaitFor([&]() { fake.runDue(); return m_mock.count(QStringLiteral("POST /videos")) >= 1; }, 5000));
        stop.abort();            // queues stop() on the wire's thread ...
        fake.advance(1001);      // ... but the timeout runs first, right here, and clears the transfer
        QVERIFY(wait(o, 2000));
        QCOMPARE(o->err->kind, WireError::Kind::Cancelled);
        QVERIFY2(o->err->outcomeUnknown, qPrintable(o->err->message));
        QVERIFY(!o->err->sentNothing);
        QVERIFY(o->err->message.contains(QLatin1String("it may still run and be billed")));
        QTest::qWait(100);
        fake.advance(10000);     // every back-off long past: still one request
        QCOMPARE(m_mock.count(QStringLiteral("POST /videos")), 1);
        QCOMPARE(cl.wire.pending(), 0);
        m_mock.releaseStalled();
        m_mock.clearScripts();
        // a chat call in the same race is a plain cancel (not paid: nothing to reconcile)
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::normal().delayed(5000));
        provider::AbortController stop2;
        Request cr = postRq(QStringLiteral("/chat/completions"), chatBody(QStringLiteral("hi")), 1000, 3);
        cr.signal = stop2.signal();
        auto oc = wireSend(cl.wire, cr, this);
        QVERIFY(QTest::qWaitFor([&]() { fake.runDue(); return m_mock.count(QStringLiteral("POST /chat/completions")) >= 1; }, 5000));
        stop2.abort();
        fake.advance(1001);
        QVERIFY(wait(oc, 2000));
        QCOMPARE(oc->err->kind, WireError::Kind::Cancelled);
        QVERIFY(!oc->err->outcomeUnknown);
        m_mock.releaseStalled();
        m_mock.clearScripts();
    }

    // Review fix: an answered gateway timeout / bad gateway on a paid POST is not re-sent (the provider may be running
    // it); on chat it is still retried (TS behaviour); a plain 500 on a paid POST is still retried (covered above).
    void clientPaidGatewayTimeout() {
        Client cl(m_cfg, m_mock.apiUrl());
        const QJsonObject img{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a red barn")}};
        for (int status : {502, 504, 524}) {
            m_mock.clearRequests();
            m_mock.script(QStringLiteral("POST /images"), MockReply::error(status, QStringLiteral("upstream timed out")), 4);
            auto o = wireSend(cl.wire, postRq(QStringLiteral("/images"), img), this);
            QVERIFY(wait(o));
            QVERIFY(o->err);
            QCOMPARE(o->err->status, status);
            QVERIFY2(o->err->outcomeUnknown, qPrintable(o->err->message));
            QVERIFY(o->err->message.contains(QLatin1String("it was not sent again, so nothing is charged twice")));
            QCOMPARE(o->err->attempts, 1);
            QTest::qWait(200);
            QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 1);
            m_mock.clearScripts();
        }
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /chat/completions"), MockReply::error(504, QStringLiteral("gateway timeout")));
        auto oc = chatCall(cl.api, ask(QStringLiteral("hi")), this);
        QVERIFY(wait(oc));
        QVERIFY2(!oc->err, oc->err ? qPrintable(oc->err->message) : "");
        QCOMPARE(oc->chat.attempts, 2);
        m_mock.clearScripts();
    }

    // Review fix: header names / values with CR, LF or NUL never reach the raw HTTP/1.1 head (or QNAM).
    void clientHeaderInjection() {
        Client cl(m_cfg, m_mock.apiUrl());
        m_mock.clearRequests();
        for (const QString& path : {QStringLiteral("/images"), QStringLiteral("/embeddings")}) {   // one-shot and QNAM
            Request rq = postRq(path, QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a red barn")}});
            rq.headers.push_back({"X-Bad", "a\r\nX-Injected: 1"});
            rq.headers.push_back({"X-Bad2\r\nX-Injected2", "1"});
            rq.headers.push_back({"X-Nul", QByteArray("a\0b", 3)});
            rq.headers.push_back({"X-Good", "ok"});
            auto o = wireSend(cl.wire, rq, this);
            QVERIFY(wait(o));
            const auto got = m_mock.last(QStringLiteral("POST ") + path);
            QVERIFY(got);
            QCOMPARE(got->header("x-good"), QByteArray("ok"));
            QVERIFY(got->header("x-bad").isEmpty());
            QVERIFY(got->header("x-injected").isEmpty());
            QVERIFY(got->header("x-injected2").isEmpty());
            QVERIFY(got->header("x-nul").isEmpty());
        }
        QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 1);
    }

    // Review fix: the ST-15 provider hook runs in the send path; a refused body never leaves the process.
    void clientSafetyHook() {
        Client cl(m_cfg, m_mock.apiUrl());
        m_mock.clearRequests();
        const QString figure = QStringLiteral("a photo of Barack Obama shaking hands with a farmer");
        // a picture of a named public figure
        auto o = wireSend(cl.wire, postRq(QStringLiteral("/images"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), figure}}), this);
        QVERIFY(wait(o));
        QVERIFY(o->err);
        QCOMPARE(o->err->kind, WireError::Kind::Refused);
        QCOMPARE(o->err->safetyCode, QStringLiteral("real_person"));
        QVERIFY(o->err->sentNothing);
        QCOMPARE(o->err->attempts, 0);
        QVERIFY2(o->err->message.startsWith(QLatin1String("Refused")), qPrintable(o->err->message));
        QCOMPARE(o->err->toJson().value(QStringLiteral("kind")).toString(), QStringLiteral("refused"));
        // the same on /videos and on a chat call that asks for image output (also on an absolute URL)
        auto ov = wireSend(cl.wire, postRq(QStringLiteral("/videos"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/vid-1")}, {QStringLiteral("prompt"), figure}}), this);
        QVERIFY(wait(ov));
        QCOMPARE(ov->err->kind, WireError::Kind::Refused);
        auto oi = wireSend(cl.wire, postRq(m_mock.apiUrl() + QStringLiteral("/chat/completions"),
                                           chatBody(figure, QJsonObject{{QStringLiteral("modalities"), QJsonArray{QStringLiteral("image"), QStringLiteral("text")}}})), this);
        QVERIFY(wait(oi));
        QCOMPARE(oi->err->kind, WireError::Kind::Refused);
        // a picture body that is not JSON cannot be checked: refused (fail closed)
        Request raw;
        raw.method = "POST";
        raw.path = QStringLiteral("/images");
        raw.body = QByteArray("prompt=Barack+Obama");
        raw.contentType = "application/x-www-form-urlencoded";
        auto orw = wireSend(cl.wire, raw, this);
        QVERIFY(wait(orw));
        QCOMPARE(orw->err->kind, WireError::Kind::Refused);
        QCOMPARE(orw->err->safetyCode, QStringLiteral("unreadable_body"));
        // cloned speech without its voice profile: refused (consent cannot be re-checked)
        const QJsonObject clone{{QStringLiteral("model"), QStringLiteral("acme/tts-1")}, {QStringLiteral("input"), QStringLiteral("hello")},
                                {QStringLiteral("input_references"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("input_audio")},
                                                                                            {QStringLiteral("input_audio"), QJsonObject{{QStringLiteral("data"), QStringLiteral("AAAA")}}}}}}};
        auto oa = wireSend(cl.wire, postRq(QStringLiteral("/audio/speech"), clone), this);
        QVERIFY(wait(oa));
        QCOMPARE(oa->err->kind, WireError::Kind::Refused);
        QVERIFY(oa->err->safetyCode.startsWith(QLatin1String("voice_consent:")));
        QCOMPARE(m_mock.count(), 0);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("refusedBySafety")).toInt(), 5);
        QCOMPARE(cl.wire.stats().value(QStringLiteral("sent")).toInt(), 0);
        // text chat about the same person is not a picture: it goes out; so does an invented-people picture
        auto ot = chatCall(cl.api, ask(figure), this);
        QVERIFY(wait(ot));
        QVERIFY2(!ot->err, ot->err ? qPrintable(ot->err->message) : "");
        auto ok = wireSend(cl.wire, postRq(QStringLiteral("/images"), QJsonObject{{QStringLiteral("model"), QStringLiteral("acme/img-1")}, {QStringLiteral("prompt"), QStringLiteral("a red barn at dawn")}}), this);
        QVERIFY(wait(ok));
        QVERIFY2(ok->resp, ok->err ? qPrintable(ok->err->message) : "");
        QCOMPARE(m_mock.count(QStringLiteral("POST /images")), 1);
    }

    // The key goes to the OpenRouter origin only; another provider gets its own token; logs and errors carry no query.
    void clientKeyHygiene() {
        MockOpenRouter other;
        QVERIFY(other.start());
        Client cl(m_cfg, m_mock.apiUrl());
        // an absolute URL on the OpenRouter origin carries the key, one elsewhere does not
        Request same;
        same.path = m_mock.apiUrl() + QStringLiteral("/credits");
        auto o1 = wireSend(cl.wire, same, this);
        QVERIFY(wait(o1));
        QVERIFY(m_mock.last(QStringLiteral("/credits"))->header("authorization") == "Bearer " + kTestKey);
        Request elsewhere;
        elsewhere.path = other.apiUrl() + QStringLiteral("/credits");
        auto o2 = wireSend(cl.wire, elsewhere, this);
        QVERIFY(wait(o2));
        QVERIFY(o2->resp);
        QVERIFY(!other.last(QStringLiteral("/credits"))->hasHeader("authorization"));
        QVERIFY(!other.last(QStringLiteral("/credits"))->hasHeader("x-title"));
        // another provider by model prefix: its base, its own token, the OpenAI-compatible body
        m_env.set("STARMEDIA_PROVIDER_TOKEN", "provider-token-for-the-test-123");
        auto unsetTok = qScopeGuard([this]() { m_env.unset("STARMEDIA_PROVIDER_TOKEN"); });
        cl.options([&other](provider::WireOptions& w) { w.providers.insert(QStringLiteral("local"), other.apiUrl()); });
        QCOMPARE(cl.wire.providerBase(u"local/qwen"), other.apiUrl());
        QVERIFY(cl.wire.providerBase(u"acme/writer-pro").isEmpty());
        m_mock.clearRequests();
        other.clearRequests();
        ChatRequest rq = ask(QStringLiteral("hi"), QStringLiteral("local/qwen"));
        rq.jsonSchema = ChatRequest::JsonSchema{QStringLiteral("x"), QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}};
        auto o3 = chatCall(cl.api, rq, this);
        QVERIFY(wait(o3));
        QVERIFY2(!o3->err, o3->err ? qPrintable(o3->err->message) : "");
        QCOMPARE(m_mock.count(), 0);
        const auto got = other.last(QStringLiteral("POST /chat/completions"));
        QVERIFY(got);
        QCOMPARE(got->header("authorization"), QByteArray("Bearer provider-token-for-the-test-123"));
        QCOMPARE(got->json.value(QStringLiteral("model")).toString(), QStringLiteral("local/qwen"));
        QVERIFY(!got->json.contains(QStringLiteral("plugins")));
        QVERIFY(!got->json.contains(QStringLiteral("provider")));
        // an error answer for a path with a query: the message and the logs show the path only
        log::Options lo;
        lo.minLevel = log::Level::Debug;
        QVERIFY(log::start(lo));
        auto stopLog = qScopeGuard([]() { log::stop(); });
        Request q;
        q.path = QStringLiteral("/nowhere?token=zzz-secret-zzz");
        q.retries = 0;
        auto o4 = wireSend(cl.wire, q, this);
        QVERIFY(wait(o4));
        QCOMPARE(o4->err->status, 404);
        QVERIFY(!o4->err->message.contains(QLatin1String("zzz-secret-zzz")));
        QVERIFY(!o4->err->toJson().value(QStringLiteral("message")).toString().contains(QLatin1String("zzz-secret-zzz")));
        QVERIFY(log::flush());
        for (const QString& l : log::recent(500)) {
            QVERIFY2(!l.contains(QLatin1String("zzz-secret-zzz")), qPrintable(l));
            QVERIFY(!l.contains(QLatin1String("provider-token-for-the-test-123")));
        }
        QVERIFY(!log::recent(500).filter(QStringLiteral("/nowhere -> 404")).isEmpty());
        other.stop();
    }

    // providerDetail / limitMessage / fmtWait, worded as the TS words them
    void clientMessages() {
        using provider::providerDetail;
        auto body = [](const char* s) { return json::parse(QByteArray(s)); };
        auto d = providerDetail(body("{\"error\":{\"message\":\"Provider returned error\",\"metadata\":{\"provider_name\":\"  Google  \",\"raw\":\"{\\\"error\\\":{\\\"message\\\":\\\"Quota   exceeded\\\\nfor today\\\"}}\"}}}"));
        QCOMPARE(d.provider, QStringLiteral("Google"));
        QCOMPARE(d.raw, QStringLiteral("Quota exceeded for today"));
        QCOMPARE(providerDetail(body("{\"error\":{\"metadata\":{\"raw\":\"plain words\"}}}")).raw, QStringLiteral("plain words"));
        QCOMPARE(providerDetail(body("{\"error\":{\"metadata\":{\"raw\":{\"detail\":\"from detail\"}}}}")).raw, QStringLiteral("from detail"));
        QCOMPARE(providerDetail(body("{\"error\":{\"metadata\":{\"raw\":{\"code\":5}}}}")).raw, QStringLiteral("{\"code\":5}"));
        QVERIFY(providerDetail(body("{\"error\":{\"metadata\":{\"raw\":12}}}")).raw.isEmpty());
        QVERIFY(providerDetail(body("{\"error\":{}}")).provider.isEmpty());
        QCOMPARE(providerDetail(QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("metadata"), QJsonObject{{QStringLiteral("raw"), QString(400, u'x')}}}}}}).raw.size(), 300);
        QCOMPARE(provider::fmtWait(400), QStringLiteral("1 s"));
        QCOMPARE(provider::fmtWait(1500), QStringLiteral("2 s"));
        QCOMPARE(provider::fmtWait(89999), QStringLiteral("90 s"));
        QCOMPARE(provider::fmtWait(90000), QStringLiteral("2 min"));
        QCOMPARE(provider::fmtWait(120000), QStringLiteral("2 min"));
        QCOMPARE(provider::fmtWait(5400000), QStringLiteral("2 h"));
        provider::LimitSignal s;
        s.kind = provider::LimitKind::Rate;
        s.provider = QStringLiteral("Kling");
        s.message = QStringLiteral("slow down");
        s.retryAfterMs = 5000;
        QCOMPARE(provider::limitMessage(429, s, QStringLiteral("kling/v2"), 2, false, false),
                 QStringLiteral("429 The provider is limiting us (Kling): too many requests for kling/v2. The studio slowed down and retried 2 times; "
                                "try again in a moment. It asks for 5 s. The provider said: slow down"));
        QCOMPARE(provider::limitMessage(429, s, QString(), 1, false, true),
                 QStringLiteral("429 The provider is limiting us (Kling): too many requests. The studio slowed down and retried 1 time (the wait it asks "
                                "for is too long to sit through here); try again in a moment. It asks for 5 s. The provider said: slow down"));
        s.kind = provider::LimitKind::Quota;
        s.provider.clear();
        s.retryAfterMs.reset();
        QCOMPARE(provider::limitMessage(429, s, QStringLiteral("m/x"), 0, true, false),
                 QStringLiteral("429 The provider is limiting us: the daily quota for m/x is used up, so retrying will not help today and it was not "
                                "retried (an earlier answer said so; nothing was sent now). The fallback models a step lists are tried automatically; "
                                "otherwise choose another model for it. The provider said: slow down"));
        s.kind = provider::LimitKind::Cap;
        QCOMPARE(provider::limitMessage(429, s, QStringLiteral("m/x"), 0, false, false), QStringLiteral("429 slow down"));
        QCOMPARE(provider::tooLargeMessage(u"/images", 0, QStringLiteral("post_max_size exceeded on the web host")),
                 QStringLiteral("post_max_size exceeded on the web host"));
        // a path that holds "%2" survives the message builders (no chained arg())
        QVERIFY(provider::tooLargeMessage(u"/models/a%2Fb", 5, QStringLiteral("x")).contains(QLatin1String("for /models/a%2Fb:")));
    }


    // ---------------- ST-07b: media, transcription, embeddings, cost lookups ----------------

    // images: the answer is parsed, decoded and written on the worker pool (the event loop never blocks), files land
    // as <outDir>/<baseName>-<i>.<ext>; in-memory results; a refused file name; a non-JSON answer; a write failure
    // still reports the cost.
    void clientImages() {
        Client cl(m_cfg, m_mock.apiUrl());
        m_mock.clearRequests();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // a pool of one thread, held by a blocker: the decode cannot start, so the answer must wait there, not on the loop
        QThreadPool pool;
        pool.setMaxThreadCount(1);
        cl.api.setWorkerPool(&pool);
        QSemaphore hold;
        pool.start([&hold]() { hold.acquire(); });
        auto release = qScopeGuard([&]() { hold.release(); pool.waitForDone(); });
        provider::ImageRequest ir;
        ir.model = QStringLiteral("acme/img-1");
        ir.prompt = QStringLiteral("a red barn at dawn");
        ir.n = 3;
        ir.aspectRatio = QStringLiteral("16:9");
        ir.quality = QStringLiteral("high");
        ir.references = {QStringLiteral("https://example.test/ref.png")};
        ir.estUsd = 0.12;
        ir.outDir = dir.path() + QStringLiteral("/pics");
        ir.baseName = QStringLiteral("shot_1");
        auto send = [&cl, this](const provider::ImageRequest& r) {
            auto g = std::make_shared<Got2<provider::ImageResult>>();
            cl.api.generateImages(r, this, [g](const provider::ImageResult& res) { g->res = res; g->done = true; },
                                  [g](const WireError& e) { g->err = e; g->done = true; });
            return g;
        };
        auto o = send(ir);
        QVERIFY(m_mock.waitForRequests(QStringLiteral("POST /images"), 1));
        QVERIFY(QTest::qWaitFor([&]() { return cl.wire.pending() == 0; }, 5000));   // the wire is done; the decode waits
        QTest::qWait(100);
        QVERIFY(!o->done);
        QCOMPARE(cl.api.pending(), 1);
        bool ticked = false;   // the loop is free while the decode is held
        QTimer::singleShot(0, this, [&ticked]() { ticked = true; });
        QTest::qWait(20);
        QVERIFY(ticked);
        hold.release();
        QVERIFY(QTest::qWaitFor([&]() { return o->done; }, 10000));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(cl.api.pending(), 0);
        QCOMPARE(o->res.images.size(), 3);
        const QByteArray png = MockOpenRouter::pixelPng();
        for (int i = 0; i < 3; ++i) {
            const provider::ImageFile& f = o->res.images.at(i);
            QCOMPARE(f.path, QDir(ir.outDir).filePath(QStringLiteral("shot_1-%1.png").arg(i + 1)));
            QFile file(f.path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(f.path));
            QCOMPARE(file.readAll(), png);
            QCOMPARE(f.size, qint64(png.size()));
            QCOMPARE(f.sha256, sha(png));
            QCOMPARE(f.mediaType, QStringLiteral("image/png"));
            QVERIFY(f.data.isEmpty());
        }
        QVERIFY(o->res.writeError.isEmpty());
        QCOMPARE(o->res.costUsd.value_or(-1), 0.04);
        QCOMPARE(o->res.costMicro.value_or(-1), money::Micro(40000));
        QCOMPARE(o->res.attempts, 1);
        const MockRequest rq = *m_mock.last(QStringLiteral("POST /images"));
        QCOMPARE(rq.json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/img-1"));
        QCOMPARE(rq.json.value(QStringLiteral("prompt")).toString(), ir.prompt);
        QCOMPARE(rq.json.value(QStringLiteral("aspect_ratio")).toString(), QStringLiteral("16:9"));
        QCOMPARE(rq.json.value(QStringLiteral("quality")).toString(), QStringLiteral("high"));
        QCOMPARE(rq.json.value(QStringLiteral("n")).toInt(), 3);
        QCOMPARE(rq.json.value(QStringLiteral("input_references")).toArray().at(0).toObject().value(QStringLiteral("image_url")).toObject().value(QStringLiteral("url")).toString(),
                 QStringLiteral("https://example.test/ref.png"));
        QCOMPARE(rq.header("X-Est-Cost"), QByteArray("0.12"));
        QCOMPARE(rq.header("Authorization"), QByteArray("Bearer ") + kTestKey);
        cl.api.setWorkerPool(nullptr);

        // in memory: no outDir, n = 1 sends no "n", a zero / negative estimate sends no X-Est-Cost (TS: > 0 only)
        m_mock.clearRequests();
        provider::ImageRequest mem;
        mem.model = QStringLiteral("acme/img-1");
        mem.prompt = QStringLiteral("a quiet lake");
        mem.estUsd = -1;
        auto om = send(mem);
        QVERIFY(QTest::qWaitFor([&]() { return om->done; }, 10000));
        QVERIFY(!om->err);
        QCOMPARE(om->res.images.size(), 1);
        QCOMPARE(om->res.images.at(0).data, png);
        QVERIFY(om->res.images.at(0).path.isEmpty());
        QCOMPARE(om->res.images.at(0).sha256, sha(png));
        QVERIFY(!m_mock.last(QStringLiteral("POST /images"))->json.contains(QStringLiteral("n")));
        QVERIFY(!m_mock.last(QStringLiteral("POST /images"))->hasHeader("X-Est-Cost"));

        // a bigger answer (4 images of 1 MiB: ~5.6 MB of JSON) decodes byte for byte
        QByteArray big(1 << 20, '\0');
        for (int i = 0; i < big.size(); ++i) big[i] = char((i * 7919) & 0xff);
        m_mock.setImageBytes(big);
        auto restoreBytes = qScopeGuard([this]() { m_mock.setImageBytes({}); });
        provider::ImageRequest four = mem;
        four.n = 4;
        four.outDir = dir.path() + QStringLiteral("/big");
        auto ob = send(four);
        QVERIFY(QTest::qWaitFor([&]() { return ob->done; }, 20000));
        QVERIFY(!ob->err);
        QCOMPARE(ob->res.images.size(), 4);
        for (const auto& f : ob->res.images) {
            QCOMPARE(f.size, qint64(big.size()));
            QCOMPARE(f.sha256, sha(big));
        }
        m_mock.setImageBytes({});

        // a file name that cannot be used: refused before anything is sent
        m_mock.clearRequests();
        provider::ImageRequest bad = ir;
        bad.baseName = QStringLiteral("../escape");
        auto obad = send(bad);
        QVERIFY(QTest::qWaitFor([&]() { return obad->done; }, 5000));
        QVERIFY(obad->err);
        QCOMPARE(obad->err->kind, WireError::Kind::Invalid);
        QVERIFY(obad->err->sentNothing);
        QCOMPARE(m_mock.count(), 0);

        // an answer that is not JSON
        m_mock.script(QStringLiteral("POST /images"), MockReply::raw(QByteArrayLiteral("<html>nope</html>"), "text/html"));
        auto onj = send(mem);
        QVERIFY(QTest::qWaitFor([&]() { return onj->done; }, 10000));
        QVERIFY(onj->err);
        QCOMPARE(onj->err->kind, WireError::Kind::Http);
        QCOMPARE(onj->err->status, 200);
        QVERIFY2(onj->err->message.contains(QLatin1String("not JSON")), qPrintable(onj->err->message));

        // the folder cannot be made (a file is in the way): the images are missing, the cost is still reported
        QFile blocker(dir.path() + QStringLiteral("/afile"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("x");
        blocker.close();
        provider::ImageRequest unwritable = mem;
        unwritable.outDir = dir.path() + QStringLiteral("/afile/sub");
        auto ow = send(unwritable);
        QVERIFY(QTest::qWaitFor([&]() { return ow->done; }, 10000));
        QVERIFY2(!ow->err, ow->err ? qPrintable(ow->err->message) : "");
        QVERIFY(ow->res.images.isEmpty());
        QVERIFY2(ow->res.writeError.startsWith(QLatin1String("image 1:")), qPrintable(ow->res.writeError));
        QCOMPARE(ow->res.costUsd.value_or(-1), 0.04);
    }

    // video: submit -> onAccepted (the job id is handed over and nothing is polled until proceed(true)) -> polls ->
    // onCompleted before the download -> the clip on disk; unrecorded; polling_url styles; a failed job; a timeout; a
    // Stop while waiting; resume; a refused submit.
    void clientVideo() {
        Client cl(m_cfg, m_mock.apiUrl());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        m_mock.setVideoStatuses({QStringLiteral("pending"), QStringLiteral("processing"), QStringLiteral("completed")});
        m_mock.setVideoCost(0.6);
        m_mock.setVideoContent(QByteArrayLiteral("fakemp4"));
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::Absolute);
        auto restore = qScopeGuard([this]() {
            m_mock.setVideoStatuses({});
            m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::Absolute);
        });
        struct VOut : Got2<provider::VideoResult> { std::optional<provider::VideoError> verr; };
        auto send = [&cl, this](const provider::VideoRequest& r) {
            auto g = std::make_shared<VOut>();
            cl.api.generateVideo(r, this, [g](const provider::VideoResult& res) { g->res = res; g->done = true; },
                                 [g](const provider::VideoError& e) { g->verr = e; g->done = true; });
            return g;
        };
        provider::VideoRequest vr;
        vr.model = QStringLiteral("acme/vid-1");
        vr.prompt = QStringLiteral("a red kite in the wind over a field");
        vr.duration = 5;
        vr.resolution = QStringLiteral("720p");
        vr.aspectRatio = QStringLiteral("16:9");
        vr.generateAudio = true;
        vr.firstFrameUrl = QStringLiteral("data:image/png;base64,AAAA");
        vr.estUsd = 0.5;
        vr.pollPendingMs = 10;
        vr.pollMs = 10;
        vr.outPath = dir.path() + QStringLiteral("/clip.mp4");
        struct Acc { QString jobId, pollPath; int getsAtAccept = -1; std::function<void(bool)> proceed; } acc;
        QStringList statuses;
        std::optional<double> completedCost;
        int contentGetsAtCompleted = -1;
        vr.onAccepted = [&](const QString& jobId, const QString& pollPath, std::function<void(bool)> proceed) {
            acc.jobId = jobId;
            acc.pollPath = pollPath;
            acc.getsAtAccept = m_mock.count(QStringLiteral("GET /videos/*"));
            acc.proceed = std::move(proceed);
        };
        vr.onStatus = [&](const QString& s) { statuses << s; };
        vr.onCompleted = [&](std::optional<double> c) {
            completedCost = c;
            contentGetsAtCompleted = m_mock.count(QStringLiteral("GET /videos/") + acc.jobId + QStringLiteral("/content"));
        };
        m_mock.clearRequests();
        auto o = send(vr);
        QVERIFY(QTest::qWaitFor([&]() { return bool(acc.proceed); }, 5000));
        QVERIFY(acc.jobId.startsWith(QLatin1String("job")));
        QCOMPARE(acc.pollPath, m_mock.url() + QStringLiteral("/videos/") + acc.jobId);
        QCOMPARE(acc.getsAtAccept, 0);
        QTest::qWait(150);   // the first poll waits for the record to be committed
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/*")), 0);
        QVERIFY(statuses.isEmpty());
        QVERIFY(!o->done);
        acc.proceed(true);
        acc.proceed(true);   // a second call does nothing
        QVERIFY(QTest::qWaitFor([&]() { return o->done; }, 10000));
        QVERIFY2(!o->verr, o->verr ? qPrintable(o->verr->message()) : "");
        QCOMPARE(statuses, (QStringList{QStringLiteral("pending"), QStringLiteral("processing"), QStringLiteral("completed")}));
        QCOMPARE(completedCost.value_or(-1), 0.6);
        QCOMPARE(contentGetsAtCompleted, 0);   // reported before the download
        QCOMPARE(o->res.jobId, acc.jobId);
        QCOMPARE(o->res.path, vr.outPath);
        QVERIFY(o->res.bytes.isEmpty());
        QCOMPARE(o->res.size, qint64(7));
        {
            QFile f(vr.outPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), QByteArrayLiteral("fakemp4"));
        }
        QCOMPARE(o->res.costUsd.value_or(-1), 0.6);
        QCOMPARE(o->res.costMicro.value_or(-1), money::Micro(600000));
        QCOMPARE(o->res.polls, 3);
        QVERIFY(o->res.writeError.isEmpty());
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/") + acc.jobId), 3);
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/") + acc.jobId + QStringLiteral("/content")), 1);
        const MockRequest sub = *m_mock.last(QStringLiteral("POST /videos"));
        QCOMPARE(sub.json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/vid-1"));
        QCOMPARE(sub.json.value(QStringLiteral("duration")).toInt(), 5);
        QCOMPARE(sub.json.value(QStringLiteral("resolution")).toString(), QStringLiteral("720p"));
        QCOMPARE(sub.json.value(QStringLiteral("aspect_ratio")).toString(), QStringLiteral("16:9"));
        QCOMPARE(sub.json.value(QStringLiteral("generate_audio")).toBool(false), true);
        QCOMPARE(sub.json.value(QStringLiteral("frame_images")).toArray().at(0).toObject().value(QStringLiteral("frame_type")).toString(), QStringLiteral("first_frame"));
        QVERIFY(!sub.json.contains(QStringLiteral("input_references")));
        QCOMPARE(sub.header("X-Est-Cost"), QByteArray("0.5"));
        // the polls carry the key (the poll URL is on the base's own origin)
        QCOMPARE(m_mock.last(QStringLiteral("GET /videos/") + acc.jobId)->header("Authorization"), QByteArray("Bearer ") + kTestKey);

        // the record could not be committed: nothing is polled, the error says so and carries the job id
        m_mock.clearRequests();
        statuses.clear();
        acc = Acc{};
        auto o2 = send(vr);
        QVERIFY(QTest::qWaitFor([&]() { return bool(acc.proceed); }, 5000));
        acc.proceed(false);
        QVERIFY(QTest::qWaitFor([&]() { return o2->done; }, 5000));
        QVERIFY(o2->verr);
        QCOMPARE(o2->verr->jobStatus, QStringLiteral("unrecorded"));
        QCOMPARE(o2->verr->jobId, acc.jobId);
        QVERIFY(o2->verr->accepted);
        QCOMPARE(o2->verr->error.kind, WireError::Kind::Invalid);
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/*")), 0);

        // polling_url styles: an API-relative one has the /api/v1 prefix removed; none at all = /videos/<id>
        provider::VideoRequest plain = vr;
        plain.onAccepted = nullptr;
        plain.outPath.clear();
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::ApiRelative);
        acc = Acc{};
        auto o3 = send(vr);
        QVERIFY(QTest::qWaitFor([&]() { return bool(acc.proceed); }, 5000));
        QCOMPARE(acc.pollPath, QStringLiteral("/videos/") + acc.jobId);
        acc.proceed(true);
        QVERIFY(QTest::qWaitFor([&]() { return o3->done; }, 10000));
        QVERIFY2(!o3->verr, o3->verr ? qPrintable(o3->verr->message()) : "");
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::None);
        auto o4 = send(plain);   // no onAccepted: polled at once, the bytes come back in memory
        QVERIFY(QTest::qWaitFor([&]() { return o4->done; }, 10000));
        QVERIFY2(!o4->verr, o4->verr ? qPrintable(o4->verr->message()) : "");
        QCOMPARE(o4->res.bytes, QByteArrayLiteral("fakemp4"));
        QVERIFY(o4->res.path.isEmpty());
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/") + o4->res.jobId), 3);
        m_mock.setVideoPollUrl(MockOpenRouter::PollUrl::Absolute);

        // a job that fails: final at the first such poll, accepted, with the provider's words
        m_mock.setVideoStatuses({QStringLiteral("processing"), QStringLiteral("failed")});
        m_mock.clearRequests();
        auto of = send(plain);
        QVERIFY(QTest::qWaitFor([&]() { return of->done; }, 10000));
        QVERIFY(of->verr);
        QCOMPARE(of->verr->jobStatus, QStringLiteral("failed"));
        QVERIFY(of->verr->accepted);
        QVERIFY(!of->verr->jobId.isEmpty());
        QCOMPARE(of->verr->message(), QStringLiteral("video generation failed: mock: video generation failed"));
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/") + of->verr->jobId), 2);
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/*/content")), 0);

        // never completes: timed out after maxWaitMs, still accepted
        m_mock.setVideoStatuses({QStringLiteral("pending")});
        provider::VideoRequest slow = plain;
        slow.maxWaitMs = 80;
        auto ot = send(slow);
        QVERIFY(QTest::qWaitFor([&]() { return ot->done; }, 10000));
        QVERIFY(ot->verr);
        QCOMPARE(ot->verr->jobStatus, QStringLiteral("timeout"));
        QCOMPARE(ot->verr->error.kind, WireError::Kind::Timeout);
        QVERIFY(ot->verr->accepted);
        QVERIFY(m_mock.count(QStringLiteral("GET /videos/") + ot->verr->jobId) >= 1);

        // a Stop while waiting between polls: cancelled, accepted (the job runs on; reconcile)
        provider::AbortController stop;
        provider::VideoRequest stoppable = plain;
        stoppable.signal = stop.signal();
        stoppable.pollPendingMs = 2000;
        m_mock.clearRequests();
        auto os = send(stoppable);
        QVERIFY(m_mock.waitForRequests(QStringLiteral("GET /videos/*"), 1));
        QTest::qWait(50);
        stop.abort(QStringLiteral("operator pressed Stop"));
        QVERIFY(QTest::qWaitFor([&]() { return os->done; }, 5000));
        QVERIFY(os->verr);
        QCOMPARE(os->verr->error.kind, WireError::Kind::Cancelled);
        QCOMPARE(os->verr->message(), QStringLiteral("operator pressed Stop"));
        QVERIFY(os->verr->accepted);
        QVERIFY(!os->verr->jobId.isEmpty());
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/*")), 1);
        QCOMPARE(cl.api.pending(), 0);

        // resume: a job that was accepted but not recorded (a restart found it `polling`) is polled and downloaded
        // without a new submit; the mock's job keeps the statuses it was submitted with, so submit one that completes
        m_mock.setVideoStatuses({QStringLiteral("completed")});
        acc = Acc{};
        auto ou = send(vr);
        QVERIFY(QTest::qWaitFor([&]() { return bool(acc.proceed); }, 5000));
        acc.proceed(false);
        QVERIFY(QTest::qWaitFor([&]() { return ou->done; }, 5000));
        QCOMPARE(ou->verr->jobStatus, QStringLiteral("unrecorded"));
        const QString resumeId = acc.jobId;
        m_mock.clearRequests();
        bool acceptedAgain = false;
        provider::VideoRequest res = plain;
        res.onAccepted = [&](const QString&, const QString&, std::function<void(bool)> p) { acceptedAgain = true; p(true); };
        auto g = std::make_shared<VOut>();
        cl.api.resumeVideo(res, resumeId, QString(), this, [g](const provider::VideoResult& r) { g->res = r; g->done = true; },
                           [g](const provider::VideoError& e) { g->verr = e; g->done = true; });
        QVERIFY(QTest::qWaitFor([&]() { return g->done; }, 10000));
        QVERIFY2(!g->verr, g->verr ? qPrintable(g->verr->message()) : "");
        QVERIFY(!acceptedAgain);
        QCOMPARE(g->res.jobId, resumeId);
        QCOMPARE(g->res.bytes, QByteArrayLiteral("fakemp4"));
        QCOMPARE(g->res.polls, 1);
        QCOMPARE(m_mock.count(QStringLiteral("POST /videos")), 0);
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/") + resumeId), 1);
        // resume with nothing to poll
        auto g2 = std::make_shared<VOut>();
        cl.api.resumeVideo(res, QString(), QString(), this, [g2](const provider::VideoResult& r) { g2->res = r; g2->done = true; },
                           [g2](const provider::VideoError& e) { g2->verr = e; g2->done = true; });
        QVERIFY(QTest::qWaitFor([&]() { return g2->done; }, 5000));
        QVERIFY(g2->verr);
        QCOMPARE(g2->verr->jobStatus, QStringLiteral("no_job_id"));
        QVERIFY(g2->verr->error.sentNothing);

        // a refused submit: not accepted, no job id, nothing polled
        m_mock.clearRequests();
        m_mock.script(QStringLiteral("POST /videos"), MockReply::noCredit());
        auto on = send(plain);
        QVERIFY(QTest::qWaitFor([&]() { return on->done; }, 10000));
        QVERIFY(on->verr);
        QVERIFY(!on->verr->accepted);
        QVERIFY(on->verr->jobId.isEmpty());
        QVERIFY(on->verr->jobStatus.isEmpty());
        QCOMPARE(on->verr->error.status, 402);
        QCOMPARE(m_mock.count(QStringLiteral("GET /videos/*")), 0);
        QCOMPARE(cl.api.pending(), 0);
    }

    // speech: the TS body, the bytes and x-generation-id back, to disk when asked; cloned speech with a consenting
    // profile goes out (without "voice"), without one it is refused; an oversize sample is refused before sending.
    void clientSpeech() {
        Client cl(m_cfg, m_mock.apiUrl());
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto send = [&cl, this](const provider::SpeechRequest& r) {
            auto g = std::make_shared<Got2<provider::SpeechResult>>();
            cl.api.speak(r, this, [g](const provider::SpeechResult& res) { g->res = res; g->done = true; }, [g](const WireError& e) { g->err = e; g->done = true; });
            return g;
        };
        m_mock.clearRequests();
        provider::SpeechRequest sr;
        sr.model = QStringLiteral("acme/tts-1");
        sr.input = QStringLiteral("Fresh coffee on Main Street.");
        sr.voice = QStringLiteral("alloy");
        sr.speed = 1.1;
        sr.estUsd = 0.01;
        auto o = send(sr);
        QVERIFY(QTest::qWaitFor([&]() { return o->done; }, 10000));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(o->res.bytes, MockOpenRouter::speechWav());
        QCOMPARE(o->res.size, qint64(MockOpenRouter::speechWav().size()));
        QCOMPARE(o->res.format, QStringLiteral("mp3"));
        QCOMPARE(o->res.generationId, QStringLiteral("gen-tts"));
        QCOMPARE(o->res.attempts, 1);
        QVERIFY(o->res.path.isEmpty());
        MockRequest rq = *m_mock.last(QStringLiteral("POST /audio/speech"));
        QCOMPARE(rq.json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/tts-1"));
        QCOMPARE(rq.json.value(QStringLiteral("input")).toString(), sr.input);
        QCOMPARE(rq.json.value(QStringLiteral("voice")).toString(), QStringLiteral("alloy"));
        QCOMPARE(rq.json.value(QStringLiteral("response_format")).toString(), QStringLiteral("mp3"));
        QCOMPARE(rq.json.value(QStringLiteral("speed")).toDouble(), 1.1);
        QVERIFY(!rq.json.contains(QStringLiteral("input_references")));
        QCOMPARE(rq.header("X-Est-Cost"), QByteArray("0.01"));
        // to disk, as pcm, without speed or estimate
        provider::SpeechRequest disk = sr;
        disk.format = QStringLiteral("pcm");
        disk.speed.reset();
        disk.estUsd.reset();
        disk.outPath = dir.path() + QStringLiteral("/voice/line1.pcm");
        auto od = send(disk);
        QVERIFY(QTest::qWaitFor([&]() { return od->done; }, 10000));
        QVERIFY(!od->err);
        QCOMPARE(od->res.path, disk.outPath);
        QVERIFY(od->res.bytes.isEmpty());
        QCOMPARE(od->res.format, QStringLiteral("pcm"));
        {
            QFile f(disk.outPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            QCOMPARE(f.readAll(), MockOpenRouter::speechWav());
        }
        rq = *m_mock.last(QStringLiteral("POST /audio/speech"));
        QCOMPARE(rq.json.value(QStringLiteral("response_format")).toString(), QStringLiteral("pcm"));
        QVERIFY(!rq.json.contains(QStringLiteral("speed")));
        QVERIFY(!rq.hasHeader("X-Est-Cost"));
        // cloned speech with its consenting profile: the sample and its transcript go as input_references, no "voice"
        provider::SpeechRequest clone = sr;
        clone.model = QStringLiteral("acme/tts-clone");
        clone.reference = provider::SpeechRequest::Reference{QStringLiteral("data:audio/wav;base64,UklGRg=="), QStringLiteral("Hello, this is my sample.")};
        clone.safety.voice = consentingVoice(clone.input);
        clone.safety.nowMs = kConsentNow;
        auto oc = send(clone);
        QVERIFY(QTest::qWaitFor([&]() { return oc->done; }, 10000));
        QVERIFY2(!oc->err, oc->err ? qPrintable(oc->err->message) : "");
        rq = *m_mock.last(QStringLiteral("POST /audio/speech"));
        QVERIFY(!rq.json.contains(QStringLiteral("voice")));
        const QJsonArray refs = rq.json.value(QStringLiteral("input_references")).toArray();
        QCOMPARE(refs.size(), 2);
        QCOMPARE(refs.at(0).toObject().value(QStringLiteral("input_audio")).toObject().value(QStringLiteral("data")).toString(), clone.reference->audioDataUrl);
        QCOMPARE(refs.at(1).toObject().value(QStringLiteral("text")).toString(), clone.reference->transcript);
        // the same without the profile is refused by the hook (nothing sent)
        provider::SpeechRequest noProfile = clone;
        noProfile.safety = {};
        auto onp = send(noProfile);
        QVERIFY(QTest::qWaitFor([&]() { return onp->done; }, 10000));
        QVERIFY(onp->err);
        QCOMPARE(onp->err->kind, WireError::Kind::Refused);
        // an oversize sample: 413 before sending
        m_mock.clearRequests();
        provider::SpeechRequest huge = clone;
        huge.reference->audioDataUrl = QStringLiteral("data:audio/wav;base64,") + QString(provider::kMaxVoiceSample, u'A');
        auto oh = send(huge);
        QVERIFY(QTest::qWaitFor([&]() { return oh->done; }, 10000));
        QVERIFY(oh->err);
        QCOMPARE(oh->err->kind, WireError::Kind::TooLarge);
        QCOMPARE(oh->err->status, 413);
        QVERIFY(oh->err->sentNothing);
        QVERIFY2(oh->err->message.startsWith(QLatin1String("413 The voice sample is too large (20 MB encoded)")), qPrintable(oh->err->message));
        QCOMPARE(m_mock.count(), 0);
    }

    // transcription: one request (the TS body with diarization options), json vs verbose_json, the verbose_json
    // refusal asked once more as json, a non-JSON answer.
    void clientTranscribe() {
        Client cl(m_cfg, m_mock.apiUrl());
        auto send = [&cl, this](const QByteArray& audio, const QString& format, const provider::TranscribeOptions& opts) {
            auto g = std::make_shared<Got2<provider::TranscribeResult>>();
            cl.api.transcribe(QStringLiteral("acme/stt-1"), audio, format, opts, this,
                              [g](const provider::TranscribeResult& r) { g->res = r; g->done = true; },
                              [g](const WireError& e, const provider::TranscribeResult&) { g->err = e; g->done = true; });
            return g;
        };
        const QByteArray wav = makeWav(3.0);
        m_mock.clearRequests();
        provider::TranscribeOptions opts;
        opts.language = QStringLiteral("en");
        opts.diarize = true;
        auto o = send(wav, QStringLiteral("wav"), opts);
        QVERIFY(QTest::qWaitFor([&]() { return o->done; }, 10000));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(o->res.text, QStringLiteral("The Henry County school board approved a new budget on Tuesday. Teacher pay rises three percent. Maintenance funding is restored."));
        QCOMPARE(o->res.segments.size(), 2);
        QCOMPARE(o->res.segments.at(0).start, 0.0);
        QCOMPARE(o->res.segments.at(0).end, 1.5);
        QCOMPARE(o->res.segments.at(1).start, 1.5);
        QCOMPARE(o->res.segments.at(1).end, 3.0);
        QCOMPARE(o->res.segments.at(0).text, QStringLiteral("The Henry County school board approved a new budget on Tuesday."));
        QVERIFY(o->res.segments.at(0).speaker.isUndefined());
        QCOMPARE(o->res.duration.value_or(-1), 3.0);
        QCOMPARE(o->res.costUsd.value_or(-1), 0.003);
        QCOMPARE(o->res.costMicro, money::Micro(3000));
        QCOMPARE(o->res.responseFormat, QStringLiteral("verbose_json"));
        QCOMPARE(o->res.pieces, 1);
        const MockRequest rq = *m_mock.last(QStringLiteral("POST /audio/transcriptions"));
        QCOMPARE(rq.json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/stt-1"));
        const QJsonObject ia = rq.json.value(QStringLiteral("input_audio")).toObject();
        QCOMPARE(ia.value(QStringLiteral("format")).toString(), QStringLiteral("wav"));
        QCOMPARE(QByteArray::fromBase64(ia.value(QStringLiteral("data")).toString().toLatin1()), wav);
        QCOMPARE(rq.json.value(QStringLiteral("response_format")).toString(), QStringLiteral("verbose_json"));
        QCOMPARE(rq.json.value(QStringLiteral("language")).toString(), QStringLiteral("en"));
        const QJsonObject po = rq.json.value(QStringLiteral("provider")).toObject().value(QStringLiteral("options")).toObject();
        QCOMPARE(po.value(QStringLiteral("azure")).toObject().value(QStringLiteral("diarization")).toObject().value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(po.value(QStringLiteral("deepgram")).toObject().value(QStringLiteral("diarize")).toBool(), true);
        // plain json: no segments, no duration, no diarization options
        provider::TranscribeOptions plain;
        plain.responseFormat = QStringLiteral("json");
        plain.diarize = true;
        auto oj = send(wav, QStringLiteral("wav"), plain);
        QVERIFY(QTest::qWaitFor([&]() { return oj->done; }, 10000));
        QVERIFY(!oj->err);
        QVERIFY(oj->res.segments.isEmpty());
        QVERIFY(!oj->res.duration);
        QCOMPARE(oj->res.responseFormat, QStringLiteral("json"));
        QVERIFY(!m_mock.last(QStringLiteral("POST /audio/transcriptions"))->json.contains(QStringLiteral("provider")));
        // the model rejects verbose_json: asked once more with json (two requests, the answer says json)
        m_mock.setRejectVerboseJson(true);
        auto restore = qScopeGuard([this]() { m_mock.setRejectVerboseJson(false); });
        m_mock.clearRequests();
        auto ov = send(wav, QStringLiteral("wav"), provider::TranscribeOptions{});
        QVERIFY(QTest::qWaitFor([&]() { return ov->done; }, 10000));
        QVERIFY2(!ov->err, ov->err ? qPrintable(ov->err->message) : "");
        QCOMPARE(ov->res.responseFormat, QStringLiteral("json"));
        QCOMPARE(ov->res.text, o->res.text);
        const QList<MockRequest> reqs = m_mock.requests(QStringLiteral("POST /audio/transcriptions"));
        QCOMPARE(reqs.size(), 2);
        QCOMPARE(reqs.at(0).json.value(QStringLiteral("response_format")).toString(), QStringLiteral("verbose_json"));
        QCOMPARE(reqs.at(1).json.value(QStringLiteral("response_format")).toString(), QStringLiteral("json"));
        m_mock.setRejectVerboseJson(false);
        // an answer that is not JSON
        m_mock.script(QStringLiteral("POST /audio/transcriptions"), MockReply::raw(QByteArrayLiteral("nope"), "text/plain"));
        auto on = send(wav, QStringLiteral("wav"), provider::TranscribeOptions{});
        QVERIFY(QTest::qWaitFor([&]() { return on->done; }, 10000));
        QVERIFY(on->err);
        QVERIFY2(on->err->message.contains(QLatin1String("not JSON")), qPrintable(on->err->message));
        QCOMPARE(cl.api.pending(), 0);
    }

    // transcribeLong: a WAV in pieces sent one after another in order, segments shifted by each piece's offset, text
    // joined, costs summed; a json fallback sticks for the rest; a short WAV and a small non-WAV go whole; an oversize
    // non-WAV is refused before sending; a Stop between pieces keeps the pieces answered so far.
    void clientTranscribeLong() {
        Client cl(m_cfg, m_mock.apiUrl());
        struct TOut : Got2<provider::TranscribeResult> { provider::TranscribeResult partial; QList<QPair<int, int>> pieces; };
        auto send = [&cl, this](const QByteArray& audio, const QString& format, provider::TranscribeOptions opts) {
            auto g = std::make_shared<TOut>();
            opts.onPiece = [g](int i, int n) { g->pieces << qMakePair(i, n); };
            cl.api.transcribeLong(QStringLiteral("acme/stt-1"), audio, format, opts, this,
                                  [g](const provider::TranscribeResult& r) { g->res = r; g->done = true; },
                                  [g](const WireError& e, const provider::TranscribeResult& p) { g->err = e; g->partial = p; g->done = true; });
            return g;
        };
        const QString text = QStringLiteral("The Henry County school board approved a new budget on Tuesday. Teacher pay rises three percent. Maintenance funding is restored.");
        const QByteArray wav25 = makeWav(25.0);
        provider::TranscribeOptions opts;
        opts.pieceSeconds = 10;
        m_mock.clearRequests();
        auto o = send(wav25, QStringLiteral("wav"), opts);
        QVERIFY(QTest::qWaitFor([&]() { return o->done; }, 20000));
        QVERIFY2(!o->err, o->err ? qPrintable(o->err->message) : "");
        QCOMPARE(o->pieces, (QList<QPair<int, int>>{{1, 3}, {2, 3}, {3, 3}}));
        QCOMPARE(o->res.pieces, 3);
        QCOMPARE(o->res.text, text + QLatin1Char(' ') + text + QLatin1Char(' ') + text);
        QCOMPARE(o->res.segments.size(), 6);
        const double bounds[7] = {0, 5, 10, 15, 20, 22.5, 25};
        for (int i = 0; i < 6; ++i) {
            QVERIFY2(qFuzzyCompare(1 + o->res.segments.at(i).start, 1 + bounds[i]), qPrintable(QString::number(o->res.segments.at(i).start)));
            QVERIFY2(qFuzzyCompare(1 + o->res.segments.at(i).end, 1 + bounds[i + 1]), qPrintable(QString::number(o->res.segments.at(i).end)));
        }
        QCOMPARE(o->res.duration.value_or(-1), 25.0);
        QVERIFY(qFuzzyCompare(o->res.costUsd.value_or(-1), 0.009));
        QCOMPARE(o->res.costMicro, money::Micro(9000));
        QCOMPARE(o->res.responseFormat, QStringLiteral("verbose_json"));
        // the pieces went out in order, each a complete WAV of 10, 10 and 5 s
        QList<MockRequest> reqs = m_mock.requests(QStringLiteral("POST /audio/transcriptions"));
        QCOMPARE(reqs.size(), 3);
        const double lens[3] = {10, 10, 5};
        for (int i = 0; i < 3; ++i) {
            if (i) QVERIFY(reqs.at(i).seq > reqs.at(i - 1).seq);
            const QByteArray piece = QByteArray::fromBase64(reqs.at(i).json.value(QStringLiteral("input_audio")).toObject().value(QStringLiteral("data")).toString().toLatin1());
            QVERIFY(piece.startsWith("RIFF"));
            QCOMPARE(MockOpenRouter::wavSeconds(piece), lens[i]);
        }
        // one piece rejected verbose_json: the rest are asked for json straight away; segments come from the text
        m_mock.setRejectVerboseJson(true);
        auto restore = qScopeGuard([this]() { m_mock.setRejectVerboseJson(false); });
        m_mock.clearRequests();
        auto oj = send(wav25, QStringLiteral("wav"), opts);
        QVERIFY(QTest::qWaitFor([&]() { return oj->done; }, 20000));
        QVERIFY2(!oj->err, oj->err ? qPrintable(oj->err->message) : "");
        QCOMPARE(oj->res.responseFormat, QStringLiteral("json"));
        QCOMPARE(oj->res.pieces, 3);
        QCOMPARE(oj->res.segments.size(), 3);
        const double jb[4] = {0, 10, 20, 25};
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(oj->res.segments.at(i).start, jb[i]);
            QCOMPARE(oj->res.segments.at(i).end, jb[i + 1]);
            QCOMPARE(oj->res.segments.at(i).text, text);
        }
        QCOMPARE(oj->res.duration.value_or(-1), 25.0);
        reqs = m_mock.requests(QStringLiteral("POST /audio/transcriptions"));
        QCOMPARE(reqs.size(), 4);
        QCOMPARE(reqs.at(0).json.value(QStringLiteral("response_format")).toString(), QStringLiteral("verbose_json"));
        for (int i = 1; i < 4; ++i) QCOMPARE(reqs.at(i).json.value(QStringLiteral("response_format")).toString(), QStringLiteral("json"));
        m_mock.setRejectVerboseJson(false);
        // a short WAV goes whole
        m_mock.clearRequests();
        auto os = send(makeWav(3.0), QStringLiteral("wav"), opts);
        QVERIFY(QTest::qWaitFor([&]() { return os->done; }, 10000));
        QVERIFY(!os->err);
        QCOMPARE(os->res.pieces, 1);
        QCOMPARE(os->res.segments.size(), 2);
        QCOMPARE(m_mock.count(QStringLiteral("POST /audio/transcriptions")), 1);
        QCOMPARE(os->pieces.size(), 0);
        // a small non-WAV goes whole (the mock's fixed answer); an oversize one is refused before sending
        provider::TranscribeOptions lim = opts;
        lim.maxSingleBytes = 1000;
        m_mock.clearRequests();
        auto om = send(QByteArray(500, 'm'), QStringLiteral("mp3"), lim);
        QVERIFY(QTest::qWaitFor([&]() { return om->done; }, 10000));
        QVERIFY(!om->err);
        QCOMPARE(om->res.duration.value_or(-1), 12.0);
        QCOMPARE(m_mock.last(QStringLiteral("POST /audio/transcriptions"))->json.value(QStringLiteral("input_audio")).toObject().value(QStringLiteral("format")).toString(), QStringLiteral("mp3"));
        m_mock.clearRequests();
        auto ob = send(QByteArray(2000, 'm'), QStringLiteral("mp3"), lim);
        QVERIFY(QTest::qWaitFor([&]() { return ob->done; }, 10000));
        QVERIFY(ob->err);
        QCOMPARE(ob->err->kind, WireError::Kind::TooLarge);
        QCOMPARE(ob->err->status, 413);
        QVERIFY(ob->err->sentNothing);
        QVERIFY2(ob->err->message.startsWith(QLatin1String("413 This mp3 file is 0.0 MB; this transcription model takes at most 0.0 MB per piece.")), qPrintable(ob->err->message));
        QCOMPARE(m_mock.count(), 0);
        // a Stop after the first piece: the failure carries what was answered (its cost is owed), nothing more is sent
        provider::AbortController stop;
        provider::TranscribeOptions stoppable = opts;
        stoppable.signal = stop.signal();
        m_mock.clearRequests();
        auto g = std::make_shared<TOut>();
        stoppable.onPiece = [g, &stop](int i, int n) {
            g->pieces << qMakePair(i, n);
            if (i == 2) stop.abort(QStringLiteral("operator pressed Stop"));
        };
        cl.api.transcribeLong(QStringLiteral("acme/stt-1"), wav25, QStringLiteral("wav"), stoppable, this,
                              [g](const provider::TranscribeResult& r) { g->res = r; g->done = true; },
                              [g](const WireError& e, const provider::TranscribeResult& p) { g->err = e; g->partial = p; g->done = true; });
        QVERIFY(QTest::qWaitFor([&]() { return g->done; }, 20000));
        QVERIFY(g->err);
        QCOMPARE(g->err->kind, WireError::Kind::Cancelled);
        QCOMPARE(g->err->message, QStringLiteral("operator pressed Stop"));
        QCOMPARE(g->partial.pieces, 1);
        QCOMPARE(g->partial.segments.size(), 2);
        QCOMPARE(g->partial.costUsd.value_or(-1), 0.003);
        QCOMPARE(m_mock.count(QStringLiteral("POST /audio/transcriptions")), 1);
        QCOMPARE(cl.api.pending(), 0);
    }

    // embeddings (vectors in input order), rerank (top_n), generationCost and credits (best effort: nullopt on any
    // failure), each with the reported cost as a double and as Micro.
    void clientEmbedRerankCost() {
        Client cl(m_cfg, m_mock.apiUrl());
        m_mock.clearRequests();
        const QStringList inputs{QStringLiteral("alpha"), QStringLiteral("beta gamma"), QStringLiteral("delta")};
        auto oe = std::make_shared<Got2<provider::EmbedResult>>();
        cl.api.embed(QStringLiteral("acme/embed-1"), inputs, this, [oe](const provider::EmbedResult& r) { oe->res = r; oe->done = true; },
                     [oe](const WireError& e) { oe->err = e; oe->done = true; });
        QVERIFY(QTest::qWaitFor([&]() { return oe->done; }, 10000));
        QVERIFY2(!oe->err, oe->err ? qPrintable(oe->err->message) : "");
        QCOMPARE(oe->res.vectors.size(), 3);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(oe->res.vectors.at(i).size(), 64);
            QCOMPARE(oe->res.vectors.at(i), MockOpenRouter::embedText(inputs.at(i)));
        }
        QCOMPARE(oe->res.costUsd.value_or(-1), 0.00001);
        QCOMPARE(oe->res.costMicro.value_or(-1), money::Micro(10));
        MockRequest rq = *m_mock.last(QStringLiteral("POST /embeddings"));
        QCOMPARE(rq.json.value(QStringLiteral("encoding_format")).toString(), QStringLiteral("float"));
        QCOMPARE(rq.json.value(QStringLiteral("input")).toArray().size(), 3);
        QCOMPARE(rq.json.value(QStringLiteral("model")).toString(), QStringLiteral("acme/embed-1"));
        // rerank
        auto orr = std::make_shared<Got2<provider::RerankResult>>();
        cl.api.rerank(QStringLiteral("acme/rerank-1"), QStringLiteral("school budget"), {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")}, 2, this,
                      [orr](const provider::RerankResult& r) { orr->res = r; orr->done = true; }, [orr](const WireError& e) { orr->err = e; orr->done = true; });
        QVERIFY(QTest::qWaitFor([&]() { return orr->done; }, 10000));
        QVERIFY2(!orr->err, orr->err ? qPrintable(orr->err->message) : "");
        QCOMPARE(orr->res.results.size(), 2);
        QCOMPARE(orr->res.results.at(0).index, 0);
        QCOMPARE(orr->res.results.at(0).score, 1.0);
        QCOMPARE(orr->res.results.at(1).index, 1);
        QCOMPARE(orr->res.results.at(1).score, 0.5);
        QCOMPARE(orr->res.costUsd.value_or(-1), 0.002);
        QCOMPARE(orr->res.costMicro.value_or(-1), money::Micro(2000));
        rq = *m_mock.last(QStringLiteral("POST /rerank"));
        QCOMPARE(rq.json.value(QStringLiteral("top_n")).toInt(), 2);
        QCOMPARE(rq.json.value(QStringLiteral("query")).toString(), QStringLiteral("school budget"));
        // generationCost: the default, a set value (the id percent-encoded as encodeURIComponent does), a failure
        struct CostGot { bool done = false; std::optional<double> usd; std::optional<money::Micro> micro; };
        auto cost = [&cl, this](const QString& id) {
            auto g = std::make_shared<CostGot>();
            cl.api.generationCost(id, this, [g](std::optional<double> u, std::optional<money::Micro> m) { g->usd = u; g->micro = m; g->done = true; });
            return g;
        };
        // an id no earlier test function set: the mock keeps its state across the class (chatJsonSchema sets gen-tts to 0.0123)
        auto c1 = cost(QStringLiteral("gen-unset"));
        QVERIFY(QTest::qWaitFor([&]() { return c1->done; }, 10000));
        QCOMPARE(c1->usd.value_or(-1), 0.05);
        QCOMPARE(c1->micro.value_or(-1), money::Micro(50000));
        m_mock.setGenerationCost(QStringLiteral("gen x/1+2"), 0.07);
        auto c2 = cost(QStringLiteral("gen x/1+2"));
        QVERIFY(QTest::qWaitFor([&]() { return c2->done; }, 10000));
        QCOMPARE(c2->usd.value_or(-1), 0.07);
        rq = *m_mock.last(QStringLiteral("GET /generation"));
        QCOMPARE(rq.queryValue(QStringLiteral("id")), QStringLiteral("gen x/1+2"));
        QVERIFY2(rq.target.endsWith(QLatin1String("/generation?id=gen%20x%2F1%2B2")), qPrintable(rq.target));
        m_mock.script(QStringLiteral("GET /generation"), MockReply::error(404, QStringLiteral("no such generation")));
        auto c3 = cost(QStringLiteral("gone"));
        QVERIFY(QTest::qWaitFor([&]() { return c3->done; }, 10000));
        QVERIFY(!c3->usd && !c3->micro);
        // credits: the fixture, then a failure and an answer of the wrong shape
        struct CreditsGot { bool done = false; std::optional<provider::Credits> c; };
        auto credits = [&cl, this]() {
            auto g = std::make_shared<CreditsGot>();
            cl.api.credits(this, [g](std::optional<provider::Credits> c) { g->c = c; g->done = true; });
            return g;
        };
        auto k1 = credits();
        QVERIFY(QTest::qWaitFor([&]() { return k1->done; }, 10000));
        QVERIFY(k1->c);
        QCOMPARE(k1->c->total.value_or(-1), 10.0);
        QCOMPARE(k1->c->used.value_or(-1), 1.0);
        m_mock.script(QStringLiteral("GET /credits"), MockReply::error(401, QStringLiteral("bad key")));
        auto k2 = credits();
        QVERIFY(QTest::qWaitFor([&]() { return k2->done; }, 10000));
        QVERIFY(!k2->c);
        m_mock.script(QStringLiteral("GET /credits"), MockReply::json(QJsonObject{{QStringLiteral("data"), 5}}));
        auto k3 = credits();
        QVERIFY(QTest::qWaitFor([&]() { return k3->done; }, 10000));
        QVERIFY(!k3->c);
        QCOMPARE(cl.api.pending(), 0);
        QCOMPARE(cl.wire.pending(), 0);
    }

    void cleanupTestCase() {
        m_mock.stop();
        QVERIFY(!m_mock.isRunning());
    }
};

QTEST_GUILESS_MAIN(TestStudioOpenRouter)
#include "test_studio_openrouter.moc"
