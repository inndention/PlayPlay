// ST-05: mock OpenRouter (see mockopenrouter.h). A hand-written HTTP/1.1 server on QTcpServer, not the Studio's own
// net::HttpServer: the mock must be able to misbehave on the wire (stall, trickle, cut, reset) in ways a correct server
// never does, and it must not share bugs with the code under test.
#ifdef _WIN32
// first: winsock2.h must come before anything that pulls in windows.h (SO_LINGER and shutdown() below)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#endif

#include "mockopenrouter.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace studio::test {

namespace mockdetail {

// "", "/path", "/prefix*", "POST /path"
struct Pattern {
    QByteArray method;
    QString path;
    bool prefix = false;
    bool any = true;
};

QString normalizePath(const QString& raw) {
    QString p = QUrl::fromPercentEncoding(raw.toUtf8());
    if (!p.startsWith(QLatin1Char('/'))) p.prepend(QLatin1Char('/'));
    if (p == QLatin1String("/api/v1")) p = QStringLiteral("/");
    else if (p.startsWith(QLatin1String("/api/v1/"))) p = p.mid(7);
    while (p.size() > 1 && p.endsWith(QLatin1Char('/'))) p.chop(1);
    return p;
}

Pattern parsePattern(const QString& text) {
    Pattern p;
    QString t = text.trimmed();
    if (t.isEmpty()) return p;
    p.any = false;
    const qsizetype sp = t.indexOf(QLatin1Char(' '));
    if (sp > 0) {
        p.method = t.left(sp).toUpper().toLatin1();
        t = t.mid(sp + 1).trimmed();
    }
    if (t.endsWith(QLatin1Char('*'))) {
        p.prefix = true;
        t.chop(1);
        p.path = t.isEmpty() ? QStringLiteral("/") : t;
        if (p.path.startsWith(QLatin1String("/api/v1/"))) p.path = p.path.mid(7);
    } else {
        p.path = normalizePath(t);
    }
    return p;
}

bool matches(const Pattern& p, const QByteArray& method, const QString& path) {
    if (p.any) return true;
    if (!p.method.isEmpty() && p.method != method) return false;
    return p.prefix ? path.startsWith(p.path) : path == p.path;
}

QByteArray reason(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "Status";
    }
}

QByteArray compact(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray compact(const QJsonArray& a) { return QJsonDocument(a).toJson(QJsonDocument::Compact); }

QJsonObject errorBody(int status, const QString& message, const QJsonObject& metadata = {}) {
    QJsonObject e{{QStringLiteral("code"), status}, {QStringLiteral("message"), message}};
    if (!metadata.isEmpty()) e.insert(QStringLiteral("metadata"), metadata);
    return QJsonObject{{QStringLiteral("error"), e}};
}

const char kPngB64[] = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

// The TS mock's transcription answer (test/mock.ts L471).
const char kSttText[] = "The Henry County school board approved a new budget on Tuesday. Teacher pay rises three percent. Maintenance funding is restored.";
const char kSttSeg1[] = "The Henry County school board approved a new budget on Tuesday.";
const char kSttSeg2[] = "Teacher pay rises three percent.";

bool videoInProgress(const QString& s) {
    static const QSet<QString> k{QStringLiteral("pending"), QStringLiteral("queued"), QStringLiteral("in_progress"), QStringLiteral("processing"),
                                 QStringLiteral("running"), QStringLiteral("starting"), QStringLiteral("submitted")};
    return k.contains(s);
}

// JS String(x) of a message content, as the TS mock joins them (string as is, missing = "", anything else JSON).
QString jsContentText(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isUndefined()) return QString();
    if (v.isNull()) return QStringLiteral("null");
    if (v.isObject()) return QString::fromUtf8(compact(v.toObject()));
    if (v.isArray()) return QString::fromUtf8(compact(v.toArray()));
    if (v.isBool()) return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return QString::number(v.toDouble(), 'g', 17);
}

bool readJsonFile(const QString& path, QJsonObject* out, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *err = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) {
        *err = QStringLiteral("%1 is not a JSON object (%2 at %3)").arg(path, pe.errorString()).arg(pe.offset);
        return false;
    }
    *out = d.object();
    return true;
}

}   // namespace mockdetail

using namespace mockdetail;

// ------------------------------------------------------------------------------------------------------------------
// MockRequest / MockReply
// ------------------------------------------------------------------------------------------------------------------

QByteArray MockRequest::header(const QByteArray& name) const {
    for (const auto& h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) return h.second;
    return {};
}

bool MockRequest::hasHeader(const QByteArray& name) const {
    for (const auto& h : headers)
        if (h.first.compare(name, Qt::CaseInsensitive) == 0) return true;
    return false;
}

QString MockRequest::queryValue(const QString& key) const {
    return QUrlQuery(query).queryItemValue(key, QUrl::FullyDecoded);
}

MockReply MockReply::json(const QJsonObject& body, int status) {
    MockReply r;
    r.status = status;
    r.body = compact(body);
    return r;
}

MockReply MockReply::raw(const QByteArray& body, const QByteArray& contentType, int status) {
    MockReply r;
    r.status = status;
    r.body = body;
    r.contentType = contentType;
    return r;
}

MockReply MockReply::error(int status, const QString& message, const QJsonObject& metadata) {
    return json(errorBody(status, message, metadata), status);
}

MockReply MockReply::rateLimited(int retryAfterSeconds, const QString& message) {
    MockReply r = error(429, message.isEmpty() ? QStringLiteral("Rate limit exceeded: too many requests per minute. Please slow down.") : message);
    if (retryAfterSeconds >= 0) r.withHeader("Retry-After", QByteArray::number(retryAfterSeconds));
    return r;
}

MockReply MockReply::rateLimitedMs(int retryAfterMs, const QString& message) {
    MockReply r = error(429, message.isEmpty() ? QStringLiteral("Rate limit exceeded: too many requests per minute. Please slow down.") : message);
    r.withHeader("retry-after-ms", QByteArray::number(retryAfterMs));
    return r;
}

MockReply MockReply::rateLimitedUntil(const QDateTime& when, const QString& message) {
    MockReply r = error(429, message.isEmpty() ? QStringLiteral("Rate limit exceeded: too many requests per minute. Please slow down.") : message);
    r.withHeader("Retry-After", MockOpenRouter::httpDate(when));
    return r;
}

MockReply MockReply::dailyQuota(qint64 resetEpochMs) {
    if (resetEpochMs <= 0) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        resetEpochMs = QDateTime(now.date().addDays(1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    }
    MockReply r = error(429, QStringLiteral("Rate limit exceeded: free-models-per-day. Add 10 credits to unlock 1000 free model requests per day"));
    r.withHeader("X-RateLimit-Limit", "50").withHeader("X-RateLimit-Remaining", "0").withHeader("X-RateLimit-Reset", QByteArray::number(resetEpochMs));
    return r;
}

MockReply MockReply::noCredit() {
    return error(402, QStringLiteral("Insufficient credits. This account never purchased credits. Make sure your key is on the correct account or org, "
                                     "and if so, purchase more at https://openrouter.ai/settings/credits"));
}

MockReply MockReply::unavailable(int retryAfterSeconds) {
    MockReply r = error(503, QStringLiteral("Service temporarily unavailable: no provider endpoints available for this request"));
    if (retryAfterSeconds >= 0) r.withHeader("Retry-After", QByteArray::number(retryAfterSeconds));
    return r;
}

MockReply MockReply::providerError(int status, const QString& provider, const QString& raw) {
    return error(status, QStringLiteral("Provider returned error"),
                 QJsonObject{{QStringLiteral("provider_name"), provider}, {QStringLiteral("raw"), raw}});
}

MockReply MockReply::spendCap() {
    return error(429, QStringLiteral("Daily spend cap reached: $20.00 of $20.00 used today. Raise it in Settings."));
}

MockReply MockReply::tooLarge() { return error(413, QStringLiteral("Request Entity Too Large")); }
MockReply MockReply::serverError() { return error(500, QStringLiteral("Internal Server Error")); }

MockReply MockReply::normal() {
    MockReply r;
    r.passthrough = true;
    return r;
}

MockReply MockReply::dropped() {
    MockReply r;
    r.drop = true;
    return r;
}

MockReply MockReply::resetConnection() {
    MockReply r;
    r.reset = true;
    return r;
}

MockReply& MockReply::withHeader(const QByteArray& name, const QByteArray& value) {
    headers.append({name, value});
    return *this;
}
MockReply& MockReply::delayed(int ms) {
    headerDelayMs = ms;
    return *this;
}
MockReply& MockReply::slowBody(int bytes, int intervalMs) {
    chunkBytes = bytes;
    chunkIntervalMs = intervalMs;
    return *this;
}
MockReply& MockReply::stallAfter(qint64 bytes) {
    stallAfterBytes = bytes;
    return *this;
}
MockReply& MockReply::truncateAfter(qint64 bytes) {
    closeAfterBytes = bytes;
    return *this;
}
MockReply& MockReply::closing() {
    close = true;
    return *this;
}

// ------------------------------------------------------------------------------------------------------------------
// State (shared by the test thread and the serving thread; every field under `mu`)
// ------------------------------------------------------------------------------------------------------------------

struct MockOpenRouter::State {
    mutable QMutex mu;
    Options options;
    QString error;
    quint16 port = 0;
    bool running = false;
    QString fixturesDir;

    // fixtures (replaced only by start())
    QJsonObject catalog, replies;
    QJsonArray models, imageModels, videoModels;
    qint64 referenceNow = 0;
    QSet<QString> voiceCloning;
    QJsonObject imageEndpoints, credits, canned, annotations;
    QString article, readAloud;

    // knobs
    struct Script {
        Pattern pattern;
        MockReply reply;
        int remaining = 1;
    };
    struct Knob {
        Pattern pattern;
        int value = 0;
        int extra = 0;
    };
    struct Cost {
        Pattern pattern;
        double usd = 0;
    };
    QList<Script> scripts;
    QList<Knob> latency, concurrency;
    QList<Cost> usage;
    QByteArray requiredKey;
    qint64 maxBody = qint64(256) << 20;
    qint64 recordLimit = 16 << 20;
    ChatHandler chat;
    QHash<QString, JsonHandler> jsonHandlers;
    qint64 catalogNow = 0;
    QStringList videoStatuses{QStringLiteral("completed")};
    QByteArray videoContent = QByteArrayLiteral("fakemp4");
    double videoCost = 0.6;
    PollUrl pollUrl = PollUrl::Absolute;
    QByteArray imageBytes, speechBytes;
    QHash<QString, double> generationCost;
    bool rejectVerbose = false;

    // records
    QList<MockRequest> log;
    qint64 seq = 0;
    int chatSeq = 0, callSeq = 0, videoSeq = 0;
    struct Job {
        QStringList statuses;
        int polls = 0;
        double cost = 0.6;
        QByteArray content;
    };
    QHash<QString, Job> jobs;
    QHash<QString, double> chatCosts;
    QHash<QString, int> inflight, peak;
    int inflightAll = 0, peakAll = 0, accepted = 0, open = 0;

    QString urlLocked() const { return QStringLiteral("http://127.0.0.1:%1").arg(port); }

    double costLocked(const QString& path, double dflt) const {   // usage patterns ignore the method
        for (const Cost& c : usage)
            if (c.pattern.any || (c.pattern.prefix ? path.startsWith(c.pattern.path) : path == c.pattern.path)) return c.usd;
        return dflt;
    }

    QJsonArray modelsLocked() const {
        const qint64 now = catalogNow > 0 ? catalogNow : QDateTime::currentSecsSinceEpoch();
        const qint64 shift = referenceNow > 0 ? now - referenceNow : 0;
        QJsonArray out;
        for (const QJsonValue& v : models) {
            QJsonObject m = v.toObject();
            const QJsonValue c = m.value(QStringLiteral("created"));
            if (c.isDouble()) m.insert(QStringLiteral("created"), double(qint64(c.toDouble()) + shift));
            out.append(m);
        }
        return out;
    }

    void beginLocked(const QString& path) {
        const int n = ++inflight[path];
        peak[path] = std::max(peak.value(path), n);
        peakAll = std::max(peakAll, ++inflightAll);
    }
    void endLocked(const QString& path) {
        auto it = inflight.find(path);
        if (it != inflight.end() && it.value() > 0) --it.value();
        if (inflightAll > 0) --inflightAll;
    }
};

// ------------------------------------------------------------------------------------------------------------------
// Routes
// ------------------------------------------------------------------------------------------------------------------

namespace {

using State = MockOpenRouter::State;

QJsonValue jsonAnswer(State* st, const MockRequest& rq, const QString& name, const QString& text, const QJsonObject& schema) {
    MockOpenRouter::JsonHandler handler;
    QJsonValue canned;
    {
        QMutexLocker lock(&st->mu);
        handler = st->jsonHandlers.value(name);
        canned = st->canned.value(name);
    }
    if (handler) return handler(rq, text);
    if (!canned.isUndefined()) return canned;
    if (!schema.isEmpty()) return MockOpenRouter::sampleFromSchema(schema, schema);
    return QJsonObject();
}

QJsonObject toolCallMessage(State* st, const QJsonArray& tools, const QJsonValue& toolChoice) {
    QString name;
    QJsonObject params;
    if (toolChoice.isObject()) name = toolChoice.toObject().value(QStringLiteral("function")).toObject().value(QStringLiteral("name")).toString();
    for (const QJsonValue& t : tools) {
        const QJsonObject fn = t.toObject().value(QStringLiteral("function")).toObject();
        if (name.isEmpty() || fn.value(QStringLiteral("name")).toString() == name) {
            name = fn.value(QStringLiteral("name")).toString();
            params = fn.value(QStringLiteral("parameters")).toObject();
            break;
        }
    }
    const QJsonValue args = params.isEmpty() ? QJsonValue(QJsonObject()) : MockOpenRouter::sampleFromSchema(params, params);
    int n;
    {
        QMutexLocker lock(&st->mu);
        n = ++st->callSeq;
    }
    const QByteArray argText = args.isObject() ? compact(args.toObject()) : args.isArray() ? compact(args.toArray()) : QByteArray("{}");
    const QJsonObject call{{QStringLiteral("id"), QStringLiteral("call_%1").arg(n)},
                           {QStringLiteral("type"), QStringLiteral("function")},
                           {QStringLiteral("function"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), QString::fromUtf8(argText)}}}};
    return QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                       {QStringLiteral("content"), QJsonValue::Null},
                       {QStringLiteral("tool_calls"), QJsonArray{call}}};
}

MockReply chatRoute(State* st, const MockRequest& rq) {
    const QJsonObject& b = rq.json;
    if (b.value(QStringLiteral("stream")).toBool())
        return MockReply::error(400, QStringLiteral("mock: stream=true is not supported (the Studio client does not stream)"));
    QJsonValue model = b.value(QStringLiteral("model"));
    if (!model.isString()) {
        const QJsonArray ms = b.value(QStringLiteral("models")).toArray();
        model = ms.isEmpty() ? QJsonValue(QJsonValue::Undefined) : ms.first();
    }
    MockOpenRouter::ChatHandler handler;
    QString article, readAloud;
    QJsonArray cannedNotes;
    const QJsonArray messages = b.value(QStringLiteral("messages")).toArray();
    const QJsonObject rf = b.value(QStringLiteral("response_format")).toObject();
    const QJsonObject js = rf.value(QStringLiteral("json_schema")).toObject();
    const QString name = js.value(QStringLiteral("name")).toString();
    {
        QMutexLocker lock(&st->mu);
        handler = st->chat;
        article = st->article;
        readAloud = st->readAloud;
        if (!name.isEmpty()) cannedNotes = st->annotations.value(name).toArray();
    }
    std::optional<QJsonObject> message;
    QString finish;
    if (handler) message = handler(rq);
    if (message) {
        if (message->contains(QStringLiteral("finish_reason"))) finish = message->take(QStringLiteral("finish_reason")).toString();
        if (!message->contains(QStringLiteral("role"))) message->insert(QStringLiteral("role"), QStringLiteral("assistant"));
    } else {
        const QJsonArray tools = b.value(QStringLiteral("tools")).toArray();
        const QJsonValue tc = b.value(QStringLiteral("tool_choice"));
        const QString lastRole = messages.isEmpty() ? QString() : messages.last().toObject().value(QStringLiteral("role")).toString();
        const bool forced = tc.isObject() || tc.toString() == QLatin1String("required");
        const bool toolsOn = !tools.isEmpty() && tc.toString() != QLatin1String("none") && lastRole != QLatin1String("tool");
        if (toolsOn && (forced || name.isEmpty())) {
            message = toolCallMessage(st, tools, tc);
        } else if (!name.isEmpty()) {
            QStringList parts;
            for (const QJsonValue& m : messages) parts << jsContentText(m.toObject().value(QStringLiteral("content")));
            const QJsonValue v = jsonAnswer(st, rq, name, parts.join(QLatin1Char('\n')), js.value(QStringLiteral("schema")).toObject());
            const QByteArray text = v.isObject() ? compact(v.toObject()) : v.isArray() ? compact(v.toArray()) : QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact).mid(1).chopped(1);
            message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QString::fromUtf8(text)}};
            if (!cannedNotes.isEmpty()) message->insert(QStringLiteral("annotations"), cannedNotes);
        } else if (lastRole == QLatin1String("tool")) {
            const QString result = jsContentText(messages.last().toObject().value(QStringLiteral("content")));
            message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QStringLiteral("[mock] tool result: ") + result.left(200)}};
        } else if (rf.value(QStringLiteral("type")).toString() == QLatin1String("json_object")) {
            message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), QStringLiteral("{\"result\":\"mock\"}")}};
        } else {
            const bool aloud = QString::fromUtf8(compact(messages)).contains(QLatin1String("read aloud"));
            message = QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("content"), aloud ? readAloud : article}};
        }
    }
    if (finish.isEmpty()) finish = message->contains(QStringLiteral("tool_calls")) ? QStringLiteral("tool_calls") : QStringLiteral("stop");
    QString id;
    double cost;
    {
        QMutexLocker lock(&st->mu);
        id = QStringLiteral("gen-%1").arg(++st->chatSeq);
        cost = st->costLocked(rq.path, 0.002);
        st->chatCosts.insert(id, cost);
    }
    QJsonObject out{{QStringLiteral("id"), id}};
    if (!model.isUndefined()) out.insert(QStringLiteral("model"), model);
    out.insert(QStringLiteral("choices"),
               QJsonArray{QJsonObject{{QStringLiteral("index"), 0}, {QStringLiteral("message"), *message}, {QStringLiteral("finish_reason"), finish}}});
    out.insert(QStringLiteral("usage"), QJsonObject{{QStringLiteral("prompt_tokens"), 1000}, {QStringLiteral("completion_tokens"), 500}, {QStringLiteral("cost"), cost}});
    return MockReply::json(out);
}

MockReply imagesRoute(State* st, const MockRequest& rq) {
    const int n = std::clamp(rq.json.value(QStringLiteral("n")).toInt(1), 1, 10);
    QByteArray bytes;
    double cost;
    {
        QMutexLocker lock(&st->mu);
        bytes = st->imageBytes.isEmpty() ? QByteArray::fromBase64(kPngB64) : st->imageBytes;
        cost = st->costLocked(rq.path, 0.04);
    }
    const QString b64 = QString::fromLatin1(bytes.toBase64());
    QJsonArray data;
    for (int i = 0; i < n; ++i) data.append(QJsonObject{{QStringLiteral("b64_json"), b64}, {QStringLiteral("media_type"), QStringLiteral("image/png")}});
    return MockReply::json(QJsonObject{{QStringLiteral("data"), data}, {QStringLiteral("usage"), QJsonObject{{QStringLiteral("cost"), cost}}}});
}

MockReply videoSubmit(State* st) {
    QMutexLocker lock(&st->mu);
    const QString id = QStringLiteral("job%1").arg(++st->videoSeq);
    State::Job job;
    job.statuses = st->videoStatuses.isEmpty() ? QStringList{QStringLiteral("completed")} : st->videoStatuses;
    job.cost = st->videoCost;
    job.content = st->videoContent;
    st->jobs.insert(id, job);
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("status"), QStringLiteral("pending")}};
    switch (st->pollUrl) {
    case MockOpenRouter::PollUrl::Absolute: o.insert(QStringLiteral("polling_url"), st->urlLocked() + QStringLiteral("/videos/") + id); break;
    case MockOpenRouter::PollUrl::ApiRelative: o.insert(QStringLiteral("polling_url"), QStringLiteral("/api/v1/videos/") + id); break;
    case MockOpenRouter::PollUrl::Relative: o.insert(QStringLiteral("polling_url"), QStringLiteral("videos/") + id); break;
    case MockOpenRouter::PollUrl::None: break;
    }
    return MockReply::json(o, 202);
}

MockReply videoPoll(State* st, const QString& id) {
    QMutexLocker lock(&st->mu);
    auto it = st->jobs.find(id);
    if (it == st->jobs.end()) return MockReply::error(404, QStringLiteral("mock: no video job %1").arg(id));
    const QString status = it->statuses.value(std::min<qsizetype>(it->polls, it->statuses.size() - 1));
    ++it->polls;
    QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("status"), status}};
    if (status == QLatin1String("completed")) {
        o.insert(QStringLiteral("unsigned_urls"), QJsonArray{st->urlLocked() + QStringLiteral("/videos/") + id + QStringLiteral("/content")});
        o.insert(QStringLiteral("usage"), QJsonObject{{QStringLiteral("cost"), it->cost}});
    } else if (!videoInProgress(status)) {
        o.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("mock: video generation %1").arg(status)}});
    }
    return MockReply::json(o);
}

MockReply videoContent(State* st, const QString& id) {
    QMutexLocker lock(&st->mu);
    auto it = st->jobs.constFind(id);
    if (it == st->jobs.constEnd()) return MockReply::error(404, QStringLiteral("mock: no video job %1").arg(id));
    return MockReply::raw(it->content, "video/mp4");
}

MockReply transcriptionRoute(State* st, const MockRequest& rq) {
    const QString fmt = rq.json.value(QStringLiteral("response_format")).toString();
    double cost;
    {
        QMutexLocker lock(&st->mu);
        if (st->rejectVerbose && fmt == QLatin1String("verbose_json"))
            return MockReply::error(400, QStringLiteral("mock: response_format verbose_json is not supported by this model; use json"));
        cost = st->costLocked(rq.path, 0.003);
    }
    const QJsonObject ia = rq.json.value(QStringLiteral("input_audio")).toObject();
    QString data = ia.value(QStringLiteral("data")).toString();
    if (data.startsWith(QLatin1String("data:"))) data = data.mid(data.indexOf(QLatin1String("base64,")) + 7);
    const QByteArray audio = QByteArray::fromBase64(data.toLatin1());
    const double secs = (ia.value(QStringLiteral("format")).toString() == QLatin1String("wav") || audio.startsWith("RIFF")) ? MockOpenRouter::wavSeconds(audio) : -1;
    QJsonObject o{{QStringLiteral("text"), QString::fromLatin1(kSttText)}};
    if (fmt != QLatin1String("json")) {
        const auto seg = [](double a, double b, const char* t) {
            return QJsonObject{{QStringLiteral("start"), a}, {QStringLiteral("end"), b}, {QStringLiteral("text"), QString::fromLatin1(t)}};
        };
        if (secs < 0) {   // the TS mock's fixed answer
            o.insert(QStringLiteral("duration"), 12);
            o.insert(QStringLiteral("segments"), QJsonArray{seg(0, 4.2, kSttSeg1), seg(4.2, 8, kSttSeg2)});
        } else {          // timed to the WAV that came in, so split / join offsets can be checked
            o.insert(QStringLiteral("duration"), secs);
            o.insert(QStringLiteral("segments"), QJsonArray{seg(0, secs / 2, kSttSeg1), seg(secs / 2, secs, kSttSeg2)});
        }
    }
    o.insert(QStringLiteral("usage"), QJsonObject{{QStringLiteral("cost"), cost}});
    return MockReply::json(o);
}

MockReply embeddingsRoute(State* st, const MockRequest& rq) {
    const QJsonValue in = rq.json.value(QStringLiteral("input"));
    const QJsonArray inputs = in.isArray() ? in.toArray() : QJsonArray{in};
    QJsonArray data;
    for (qsizetype i = 0; i < inputs.size(); ++i) {
        QJsonArray vec;
        for (double d : MockOpenRouter::embedText(inputs.at(i).toString())) vec.append(d);
        data.append(QJsonObject{{QStringLiteral("index"), int(i)}, {QStringLiteral("embedding"), vec}});
    }
    double cost;
    {
        QMutexLocker lock(&st->mu);
        cost = st->costLocked(rq.path, 0.00001);
    }
    return MockReply::json(QJsonObject{{QStringLiteral("data"), data}, {QStringLiteral("usage"), QJsonObject{{QStringLiteral("cost"), cost}}}});
}

MockReply rerankRoute(State* st, const MockRequest& rq) {
    const QJsonArray docs = rq.json.value(QStringLiteral("documents")).toArray();
    qsizetype n = docs.size();
    const QJsonValue top = rq.json.value(QStringLiteral("top_n"));
    if (top.isDouble()) {   // JS slice(0, top_n): truncated, negative counts from the end
        qsizetype t = qsizetype(std::trunc(top.toDouble()));
        n = t < 0 ? std::max<qsizetype>(0, docs.size() + t) : std::min(t, docs.size());
    }
    QJsonArray results;
    for (qsizetype i = 0; i < n; ++i)
        results.append(QJsonObject{{QStringLiteral("index"), int(i)}, {QStringLiteral("relevance_score"), 1.0 / double(i + 1)}});
    double cost;
    {
        QMutexLocker lock(&st->mu);
        cost = st->costLocked(rq.path, 0.002);
    }
    return MockReply::json(QJsonObject{{QStringLiteral("results"), results}, {QStringLiteral("usage"), QJsonObject{{QStringLiteral("cost"), cost}}}});
}

MockReply route(State* st, const MockRequest& rq) {
    const QString& p = rq.path;
    if (p == QLatin1String("/models")) {
        QMutexLocker lock(&st->mu);
        return MockReply::json(QJsonObject{{QStringLiteral("data"), st->modelsLocked()}});
    }
    if (p.startsWith(QLatin1String("/models/")) && p.endsWith(QLatin1String("/endpoints"))) {
        const QString id = p.mid(8, p.size() - 8 - 10);
        QMutexLocker lock(&st->mu);
        const QJsonObject ep{{QStringLiteral("supports_voice_cloning"), st->voiceCloning.contains(id)}};
        return MockReply::json(QJsonObject{{QStringLiteral("data"), QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("endpoints"), QJsonArray{ep}}}}});
    }
    if (p == QLatin1String("/images/models")) {
        QMutexLocker lock(&st->mu);
        return MockReply::json(QJsonObject{{QStringLiteral("data"), st->imageModels}});
    }
    if (p.startsWith(QLatin1String("/images/models/")) && p.endsWith(QLatin1String("/endpoints"))) {
        const QString id = p.mid(15, p.size() - 15 - 10);
        QMutexLocker lock(&st->mu);
        QJsonObject o = st->imageEndpoints.value(id).toObject();
        if (o.isEmpty()) {   // the TS mock answers any id like a paid model
            for (const QJsonValue& m : st->imageModels) {
                const QJsonObject e = st->imageEndpoints.value(m.toObject().value(QStringLiteral("id")).toString()).toObject();
                const double c = e.value(QStringLiteral("endpoints")).toArray().at(0).toObject().value(QStringLiteral("pricing")).toArray().at(0).toObject().value(QStringLiteral("cost_usd")).toDouble();
                if (c > 0) {
                    o = e;
                    break;
                }
            }
            o.insert(QStringLiteral("id"), id);
        }
        return MockReply::json(o);
    }
    if (p == QLatin1String("/videos/models")) {
        QMutexLocker lock(&st->mu);
        return MockReply::json(QJsonObject{{QStringLiteral("data"), st->videoModels}});
    }
    if (p == QLatin1String("/chat/completions")) return chatRoute(st, rq);
    if (p == QLatin1String("/images")) return imagesRoute(st, rq);
    if (p == QLatin1String("/videos")) return videoSubmit(st);
    if (p.startsWith(QLatin1String("/videos/"))) {
        const QStringList seg = p.mid(8).split(QLatin1Char('/'));
        if (seg.size() == 1) return videoPoll(st, seg[0]);
        if (seg.size() == 2 && seg[1] == QLatin1String("content")) return videoContent(st, seg[0]);
    }
    if (p == QLatin1String("/audio/speech")) {
        static const QByteArray tone = MockOpenRouter::speechWav();
        QMutexLocker lock(&st->mu);
        MockReply r = MockReply::raw(st->speechBytes.isEmpty() ? tone : st->speechBytes, "audio/mpeg");
        r.withHeader("x-generation-id", "gen-tts");
        return r;
    }
    if (p == QLatin1String("/generation")) {
        const QString id = rq.queryValue(QStringLiteral("id"));
        QMutexLocker lock(&st->mu);
        const double c = st->generationCost.contains(id) ? st->generationCost.value(id) : st->chatCosts.value(id, 0.05);
        return MockReply::json(QJsonObject{{QStringLiteral("data"), QJsonObject{{QStringLiteral("total_cost"), c}}}});
    }
    if (p == QLatin1String("/audio/transcriptions")) return transcriptionRoute(st, rq);
    if (p == QLatin1String("/embeddings")) return embeddingsRoute(st, rq);
    if (p == QLatin1String("/rerank")) return rerankRoute(st, rq);
    if (p == QLatin1String("/credits")) {
        QMutexLocker lock(&st->mu);
        return MockReply::json(st->credits);
    }
    return MockReply::json(QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("mock: no route %1").arg(p)}}}}, 404);
}

void hardReset(QTcpSocket* s) {
#ifdef Q_OS_WIN
    // SO_LINGER {on, 0}: closesocket() then sends RST instead of FIN (Qt has no socket option for it)
    const qintptr fd = s->socketDescriptor();
    if (fd != -1) {
        linger l{1, 0};
        ::setsockopt(SOCKET(fd), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), int(sizeof l));
    }
#endif
    s->abort();
}

void halfClose(QTcpSocket* s) {
#ifdef Q_OS_WIN
    const qintptr fd = s->socketDescriptor();
    if (fd != -1) {
        ::shutdown(SOCKET(fd), SD_SEND);
        return;
    }
#endif
    s->disconnectFromHost();
}

}   // namespace

// ------------------------------------------------------------------------------------------------------------------
// Server (lives in the serving thread)
// ------------------------------------------------------------------------------------------------------------------

class MockOpenRouter::Server : public QObject {
public:
    explicit Server(State* st) : m_st(st), m_tcp(new QTcpServer(this)) {
        QObject::connect(m_tcp, &QTcpServer::newConnection, this, [this]() { accept(); });
    }

    bool listen(quint16 port, QString* err) {
        if (!m_tcp->listen(QHostAddress::LocalHost, port)) {
            *err = m_tcp->errorString();
            return false;
        }
        QMutexLocker lock(&m_st->mu);
        m_st->port = m_tcp->serverPort();
        return true;
    }

    void shutdown() {
        m_tcp->close();
        const QList<int> ids = m_conns.keys();
        for (int id : ids) {
            Conn* c = m_conns.value(id);
            c->sock->disconnect(this);
            c->sock->abort();
            dropConn(c, true);
        }
        m_conns.clear();
    }

    void releaseStalled() {
        const QList<int> ids = m_conns.keys();
        for (int id : ids) {
            Conn* c = m_conns.value(id);
            if (c && c->stalled) {
                endAnswer(c);
                c->closing = true;
                c->sock->disconnectFromHost();
            }
        }
    }

private:
    struct Conn {
        int id = 0;
        QTcpSocket* sock = nullptr;
        QByteArray buf;
        qsizetype pos = 0;
        // the request being read
        bool haveHead = false;
        MockRequest rq;
        bool keepAlive = true;
        bool expectContinue = false;
        bool continueSent = false;
        bool chunked = false;
        qint64 contentLength = 0;
        int chunkState = 0;   // 0 size line, 1 data, 2 CRLF after data, 3 trailer
        qint64 chunkLeft = 0;
        QByteArray body;
        // the answer being written
        bool answering = false;
        bool counted = false;
        bool stalled = false;
        bool closing = false;
        bool discarding = false;   // refused: read and drop everything until the client closes
        bool halfClosed = false;
        QString answerPath;
        QObject* ctx = nullptr;    // owns the answer's timers; deleted when the answer ends
        QByteArray out;
        qint64 outPos = 0, outLimit = 0;
    };

    enum class Parse { NeedMore, Ready, Bad };

    void accept() {
        while (QTcpSocket* s = m_tcp->nextPendingConnection()) {
            Conn* c = new Conn;
            c->id = ++m_nextId;
            c->sock = s;
            m_conns.insert(c->id, c);
            {
                QMutexLocker lock(&m_st->mu);
                ++m_st->accepted;
                ++m_st->open;
            }
            const int id = c->id;
            QObject::connect(s, &QTcpSocket::readyRead, this, [this, id]() {
                if (Conn* c = m_conns.value(id)) onRead(c);
            });
            QObject::connect(s, &QTcpSocket::disconnected, this, [this, id]() {
                if (Conn* c = m_conns.value(id)) dropConn(c, false);
            });
        }
    }

    void onRead(Conn* c) {
        if (c->discarding || c->closing) {
            c->sock->readAll();
            return;
        }
        c->buf += c->sock->readAll();
        pump(c);
    }

    // `now` = not called from one of the connection's own signal or timer handlers (shutdown): delete at once
    void dropConn(Conn* c, bool now) {
        endAnswer(c, now);
        m_conns.remove(c->id);
        {
            QMutexLocker lock(&m_st->mu);
            if (m_st->open > 0) --m_st->open;
        }
        if (now) delete c->sock;
        else c->sock->deleteLater();
        delete c;
    }

    // abort without waiting for the socket's own disconnected() (abort() does not reliably emit it)
    void kill(Conn* c) {
        c->sock->disconnect(this);
        c->sock->abort();
        dropConn(c, false);
    }

    void endAnswer(Conn* c, bool now = false) {
        if (c->counted) {
            QMutexLocker lock(&m_st->mu);
            m_st->endLocked(c->answerPath);
        }
        c->counted = false;
        c->answering = false;
        c->stalled = false;
        if (c->ctx) {
            if (now) delete c->ctx;
            else c->ctx->deleteLater();   // we may be running inside one of its timers
            c->ctx = nullptr;
        }
    }

    void refuse(Conn* c, int status, const QString& message) {
        const QByteArray body = compact(errorBody(status, message));
        c->sock->write("HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\nContent-Type: application/json\r\nContent-Length: " +
                       QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        c->discarding = true;
        c->buf.clear();
        c->pos = 0;
        c->body.clear();
        // Half-close once the answer has left Qt's buffer and keep reading, so the client reads the answer instead of
        // losing it to a TCP reset caused by request bytes nobody read; the client's close (or 3 s) ends it.
        const int id = c->id;
        auto tryHalfClose = [this, id]() {
            Conn* c = m_conns.value(id);
            if (!c || c->halfClosed || c->sock->bytesToWrite() > 0) return;
            c->halfClosed = true;
            halfClose(c->sock);
        };
        QObject::connect(c->sock, &QTcpSocket::bytesWritten, this, tryHalfClose);
        QTimer::singleShot(0, this, tryHalfClose);
        QTimer::singleShot(3000, this, [this, id]() {
            if (Conn* c = m_conns.value(id)) kill(c);
        });
    }

    // the body is incomplete: say 100 Continue once if asked, keep the read buffer small while a large body streams in
    Parse needMore(Conn* c) {
        if (c->expectContinue && !c->continueSent) sendContinue(c);
        if (c->pos > (1 << 20)) {
            c->buf.remove(0, c->pos);
            c->pos = 0;
        }
        return Parse::NeedMore;
    }

    Parse parse(Conn* c, int* bad, QString* why) {
        qint64 maxBody;
        {
            QMutexLocker lock(&m_st->mu);
            maxBody = m_st->maxBody;
        }
        if (!c->haveHead) {
            while (c->pos < c->buf.size() && (c->buf.at(c->pos) == '\r' || c->buf.at(c->pos) == '\n')) ++c->pos;
            const qsizetype end = c->buf.indexOf("\r\n\r\n", c->pos);
            if (end < 0) {
                if (c->buf.size() - c->pos > 65536) {
                    *bad = 431;
                    *why = QStringLiteral("mock: request head too large");
                    return Parse::Bad;
                }
                return Parse::NeedMore;
            }
            const QByteArray head = c->buf.mid(c->pos, end - c->pos);
            c->pos = end + 4;
            const QList<QByteArray> lines = head.split('\n');
            const QList<QByteArray> rl = lines.value(0).trimmed().split(' ');
            if (rl.size() != 3) {
                *bad = 400;
                *why = QStringLiteral("mock: bad request line");
                return Parse::Bad;
            }
            const QByteArray version = rl[2];
            if (version != "HTTP/1.1" && version != "HTTP/1.0") {
                *bad = 505;
                *why = QStringLiteral("mock: HTTP/1.x only");
                return Parse::Bad;
            }
            MockRequest rq;
            rq.method = rl[0];
            rq.target = QString::fromLatin1(rl[1]);
            for (qsizetype i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines[i].endsWith('\r') ? lines[i].chopped(1) : lines[i];
                const qsizetype colon = line.indexOf(':');
                if (colon <= 0) {
                    *bad = 400;
                    *why = QStringLiteral("mock: bad header line");
                    return Parse::Bad;
                }
                rq.headers.append({line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
            }
            QString target = rq.target;
            if (target.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) {   // absolute-form
                const qsizetype slash = target.indexOf(QLatin1Char('/'), 7);
                target = slash < 0 ? QStringLiteral("/") : target.mid(slash);
            }
            const qsizetype q = target.indexOf(QLatin1Char('?'));
            rq.query = q < 0 ? QString() : target.mid(q + 1);
            rq.path = normalizePath(q < 0 ? target : target.left(q));
            const QByteArray te = rq.header("transfer-encoding").toLower();
            const QByteArray cl = rq.header("content-length");
            const QByteArray conn = rq.header("connection").toLower();
            c->keepAlive = version == "HTTP/1.1" ? !conn.contains("close") : conn.contains("keep-alive");
            c->expectContinue = rq.header("expect").compare("100-continue", Qt::CaseInsensitive) == 0;
            c->continueSent = false;
            c->chunked = te.contains("chunked");
            c->contentLength = 0;
            c->chunkState = 0;
            c->chunkLeft = 0;
            c->body.clear();
            if (!c->chunked && !cl.isEmpty()) {
                bool ok = false;
                c->contentLength = cl.toLongLong(&ok);
                if (!ok || c->contentLength < 0) {
                    *bad = 400;
                    *why = QStringLiteral("mock: bad Content-Length");
                    return Parse::Bad;
                }
            }
            rq.chunked = c->chunked;
            c->rq = rq;
            c->haveHead = true;
            if (c->contentLength > maxBody) {
                recordRefused(c, c->contentLength);
                *bad = 413;
                *why = QStringLiteral("Request Entity Too Large");
                return Parse::Bad;
            }
            if (c->contentLength > 0) c->body.reserve(qsizetype(std::min<qint64>(c->contentLength, qint64(64) << 20)));
        }
        if (c->chunked) {
            for (;;) {
                if (c->chunkState == 1) {
                    const qint64 take = std::min<qint64>(c->chunkLeft, c->buf.size() - c->pos);
                    c->body.append(c->buf.constData() + c->pos, qsizetype(take));
                    c->pos += qsizetype(take);
                    c->chunkLeft -= take;
                    if (c->body.size() > maxBody) {
                        recordRefused(c, c->body.size());
                        *bad = 413;
                        *why = QStringLiteral("Request Entity Too Large");
                        return Parse::Bad;
                    }
                    if (c->chunkLeft > 0) return needMore(c);
                    c->chunkState = 2;
                }
                if (c->chunkState == 2) {
                    if (c->buf.size() - c->pos < 2) return needMore(c);
                    if (c->buf.at(c->pos) != '\r' || c->buf.at(c->pos + 1) != '\n') {
                        *bad = 400;
                        *why = QStringLiteral("mock: bad chunk framing");
                        return Parse::Bad;
                    }
                    c->pos += 2;
                    c->chunkState = 0;
                }
                const qsizetype eol = c->buf.indexOf("\r\n", c->pos);
                if (eol < 0) {
                    if (c->buf.size() - c->pos > 4096) {
                        *bad = 400;
                        *why = QStringLiteral("mock: chunk line too long");
                        return Parse::Bad;
                    }
                    return needMore(c);
                }
                const QByteArray line = c->buf.mid(c->pos, eol - c->pos);
                c->pos = eol + 2;
                if (c->chunkState == 3) {
                    if (line.isEmpty()) break;   // end of trailers
                    continue;
                }
                bool ok = false;
                const qsizetype semi = line.indexOf(';');
                const qint64 size = (semi < 0 ? line : line.left(semi)).trimmed().toLongLong(&ok, 16);
                if (!ok || size < 0) {
                    *bad = 400;
                    *why = QStringLiteral("mock: bad chunk size");
                    return Parse::Bad;
                }
                if (size == 0) {
                    c->chunkState = 3;
                } else {
                    c->chunkLeft = size;
                    c->chunkState = 1;
                }
            }
        } else {
            const qint64 need = c->contentLength - c->body.size();
            const qint64 take = std::min<qint64>(need, c->buf.size() - c->pos);
            c->body.append(c->buf.constData() + c->pos, qsizetype(take));
            c->pos += qsizetype(take);
            if (c->body.size() < c->contentLength) return needMore(c);
        }
        c->buf.remove(0, c->pos);
        c->pos = 0;
        c->haveHead = false;
        return Parse::Ready;
    }

    void sendContinue(Conn* c) {
        c->continueSent = true;
        c->sock->write("HTTP/1.1 100 Continue\r\n\r\n");
    }

    void recordRefused(Conn* c, qint64 size) {
        MockRequest rq = c->rq;
        rq.bodySize = size;
        rq.bodyTruncated = true;
        rq.connection = c->id;
        rq.receivedMs = QDateTime::currentMSecsSinceEpoch();
        QMutexLocker lock(&m_st->mu);
        rq.seq = ++m_st->seq;
        m_st->log.append(rq);
    }

    void pump(Conn* c) {
        if (c->answering || c->closing || c->discarding) return;
        int bad = 0;
        QString why;
        const Parse r = parse(c, &bad, &why);
        if (r == Parse::NeedMore) return;
        if (r == Parse::Bad) {
            refuse(c, bad, why);
            return;
        }
        handle(c);
    }

    void handle(Conn* c) {
        MockRequest rq = c->rq;
        rq.connection = c->id;
        rq.body = std::move(c->body);
        c->body = QByteArray();
        rq.bodySize = rq.body.size();
        rq.receivedMs = QDateTime::currentMSecsSinceEpoch();
        if (!rq.body.isEmpty()) {
            const QJsonDocument d = QJsonDocument::fromJson(rq.body);
            if (d.isObject()) rq.json = d.object();
        }
        bool overLimit = false;
        int limitRetryMs = 0;
        std::optional<MockReply> scripted;
        int latency = 0;
        bool unauthorized = false;
        {
            QMutexLocker lock(&m_st->mu);
            rq.seq = ++m_st->seq;
            MockRequest rec = rq;
            if (rec.body.size() > m_st->recordLimit) {
                rec.body.truncate(qsizetype(m_st->recordLimit));
                rec.bodyTruncated = true;
            }
            m_st->log.append(rec);
            unauthorized = !m_st->requiredKey.isEmpty() && rq.header("authorization") != "Bearer " + m_st->requiredKey;
            for (const State::Knob& k : m_st->concurrency) {
                if (k.value > 0 && matches(k.pattern, rq.method, rq.path)) {
                    overLimit = m_st->inflight.value(rq.path) >= k.value;
                    limitRetryMs = k.extra;
                    break;
                }
            }
            if (!unauthorized) {
                for (qsizetype i = 0; i < m_st->scripts.size(); ++i) {
                    State::Script& s = m_st->scripts[i];
                    if (!matches(s.pattern, rq.method, rq.path)) continue;
                    scripted = s.reply;
                    if (s.remaining > 0 && --s.remaining == 0) m_st->scripts.removeAt(i);
                    break;
                }
            }
            for (const State::Knob& k : m_st->latency) {
                if (k.value > 0 && matches(k.pattern, rq.method, rq.path)) {
                    latency = k.value;
                    break;
                }
            }
            m_st->beginLocked(rq.path);
        }
        MockReply reply;
        if (unauthorized) {
            reply = MockReply::error(401, QStringLiteral("No auth credentials found"));
        } else if (scripted && !scripted->passthrough) {
            reply = *scripted;
        } else {
            reply = overLimit ? MockReply::rateLimitedMs(limitRetryMs, QStringLiteral("Rate limit exceeded: too many concurrent requests for this model (mock limit)"))
                              : route(m_st, rq);
            if (scripted) {   // MockReply::normal() with a changed delivery
                reply.headers += scripted->headers;
                reply.headerDelayMs = scripted->headerDelayMs;
                reply.chunkBytes = scripted->chunkBytes;
                reply.chunkIntervalMs = scripted->chunkIntervalMs;
                reply.stallAfterBytes = scripted->stallAfterBytes;
                reply.closeAfterBytes = scripted->closeAfterBytes;
                reply.drop = scripted->drop;
                reply.reset = scripted->reset;
                reply.close = scripted->close;
            }
        }
        reply.headerDelayMs += latency;
        deliver(c, rq.path, reply);
    }

    void deliver(Conn* c, const QString& path, const MockReply& r) {
        c->answering = true;
        c->counted = true;
        c->answerPath = path;
        c->ctx = new QObject(this);
        const int id = c->id;
        const bool keepAlive = c->keepAlive;
        auto go = [this, id, r, keepAlive]() {
            Conn* c = m_conns.value(id);
            if (!c) return;
            if (r.reset) {
                c->sock->disconnect(this);
                hardReset(c->sock);
                dropConn(c, false);
                return;
            }
            if (r.drop) {
                endAnswer(c);
                c->closing = true;
                c->sock->disconnectFromHost();
                return;
            }
            const bool close = r.close || !keepAlive || r.closeAfterBytes >= 0;
            QByteArray head = "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + reason(r.status) + "\r\n";
            if (!r.contentType.isEmpty()) head += "Content-Type: " + r.contentType + "\r\n";
            head += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
            for (const auto& h : r.headers) head += h.first + ": " + h.second + "\r\n";
            head += close ? "Connection: close\r\n\r\n" : "Connection: keep-alive\r\n\r\n";
            c->sock->write(head);
            c->out = r.body;
            c->outPos = 0;
            c->outLimit = c->out.size();
            if (r.stallAfterBytes >= 0 && r.stallAfterBytes < c->outLimit) c->outLimit = r.stallAfterBytes;
            if (r.closeAfterBytes >= 0 && r.closeAfterBytes < c->outLimit) c->outLimit = r.closeAfterBytes;
            if (r.chunkBytes <= 0 || c->outLimit == 0) {
                writeUpTo(c, c->outLimit);
                finishBody(c, r, close);
                return;
            }
            writeUpTo(c, std::min<qint64>(c->outLimit, r.chunkBytes));
            if (c->outPos >= c->outLimit) {
                finishBody(c, r, close);
                return;
            }
            QTimer* t = new QTimer(c->ctx);
            t->setInterval(std::max(0, r.chunkIntervalMs));
            QObject::connect(t, &QTimer::timeout, c->ctx, [this, id, r, close, t]() {
                Conn* c = m_conns.value(id);
                if (!c) return;
                writeUpTo(c, std::min<qint64>(c->outLimit, c->outPos + r.chunkBytes));
                if (c->outPos >= c->outLimit) {
                    t->stop();
                    finishBody(c, r, close);
                }
            });
            t->start();
        };
        if (r.headerDelayMs > 0) QTimer::singleShot(r.headerDelayMs, c->ctx, go);
        else go();
    }

    void writeUpTo(Conn* c, qint64 upTo) {
        if (upTo > c->outPos) {
            c->sock->write(c->out.constData() + c->outPos, upTo - c->outPos);
            c->outPos = upTo;
        }
    }

    void finishBody(Conn* c, const MockReply& r, bool close) {
        if (r.stallAfterBytes >= 0 && r.stallAfterBytes < c->out.size()) {
            c->stalled = true;   // still counted in flight until the client gives up or releaseStalled()
            return;
        }
        c->out.clear();
        endAnswer(c);
        if (r.closeAfterBytes >= 0 || close) {
            c->closing = true;
            c->sock->disconnectFromHost();
            return;
        }
        const int id = c->id;
        QTimer::singleShot(0, this, [this, id]() {
            if (Conn* c = m_conns.value(id)) pump(c);
        });
    }

    State* m_st;
    QTcpServer* m_tcp;
    QHash<int, Conn*> m_conns;
    int m_nextId = 0;
};

// ------------------------------------------------------------------------------------------------------------------
// MockOpenRouter
// ------------------------------------------------------------------------------------------------------------------

MockOpenRouter::MockOpenRouter() : m_state(std::make_unique<State>()) {}

MockOpenRouter::~MockOpenRouter() { stop(); }

QString MockOpenRouter::defaultFixturesDir() {
#ifdef STUDIO_TEST_DIR
    return QStringLiteral(STUDIO_TEST_DIR "/fixtures");
#else
    return QString::fromLocal8Bit(qgetenv("STUDIO_TEST_DIR")) + QStringLiteral("/fixtures");
#endif
}

bool MockOpenRouter::start(const Options& options) {
    stop();
    const QString dir = options.fixturesDir.isEmpty() ? defaultFixturesDir() : options.fixturesDir;
    QJsonObject catalog, replies;
    QString err;
    if (!readJsonFile(dir + QStringLiteral("/models.json"), &catalog, &err) || !readJsonFile(dir + QStringLiteral("/mock-replies.json"), &replies, &err)) {
        QMutexLocker lock(&m_state->mu);
        m_state->error = err;
        return false;
    }
    {
        QMutexLocker lock(&m_state->mu);
        State& s = *m_state;
        s.options = options;
        s.fixturesDir = dir;
        s.error.clear();
        s.catalog = catalog;
        s.replies = replies;
        s.models = catalog.value(QStringLiteral("models")).toArray();
        s.referenceNow = qint64(catalog.value(QStringLiteral("reference_now")).toDouble());
        s.voiceCloning.clear();
        for (const QJsonValue& v : catalog.value(QStringLiteral("voice_cloning")).toArray()) s.voiceCloning.insert(v.toString());
        s.imageModels = catalog.value(QStringLiteral("image_models")).toArray();
        s.imageEndpoints = catalog.value(QStringLiteral("image_endpoints")).toObject();
        s.videoModels = catalog.value(QStringLiteral("video_models")).toArray();
        s.credits = catalog.value(QStringLiteral("credits")).toObject();
        s.canned = replies.value(QStringLiteral("json")).toObject();
        s.annotations = replies.value(QStringLiteral("annotations")).toObject();
        s.article = replies.value(QStringLiteral("article")).toString();
        s.readAloud = replies.value(QStringLiteral("read_aloud")).toString();
        s.maxBody = options.maxBodyBytes;
        s.recordLimit = options.recordBodyLimit;
        if (s.models.isEmpty()) {
            s.error = QStringLiteral("%1/models.json has no models").arg(dir);
            return false;
        }
    }
    m_server = new Server(m_state.get());
    bool ok = false;
    QString listenErr;
    if (options.ownThread) {
        m_thread = new QThread;
        m_thread->setObjectName(QStringLiteral("MockOpenRouter"));
        m_server->moveToThread(m_thread);
        m_thread->start();
        QMetaObject::invokeMethod(m_server, [&]() { ok = m_server->listen(options.port, &listenErr); }, Qt::BlockingQueuedConnection);
    } else {
        ok = m_server->listen(options.port, &listenErr);
    }
    if (!ok) {
        stop();
        QMutexLocker lock(&m_state->mu);
        m_state->error = QStringLiteral("cannot listen: %1").arg(listenErr);
        return false;
    }
    QMutexLocker lock(&m_state->mu);
    m_state->running = true;
    return true;
}

void MockOpenRouter::stop() {
    if (!m_server) return;
    if (m_thread) {
        QThread* caller = QThread::currentThread();
        QMetaObject::invokeMethod(m_server, [this, caller]() {
            m_server->shutdown();
            m_server->moveToThread(caller);   // so it can be deleted here once the thread is gone
        }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    } else {
        m_server->shutdown();
    }
    delete m_server;
    m_server = nullptr;
    QMutexLocker lock(&m_state->mu);
    m_state->running = false;
    m_state->port = 0;
    m_state->inflight.clear();
    m_state->inflightAll = 0;
    m_state->open = 0;
}

bool MockOpenRouter::isRunning() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->running;
}

QString MockOpenRouter::errorString() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->error;
}

quint16 MockOpenRouter::port() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->port;
}

QString MockOpenRouter::url() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->urlLocked();
}

QString MockOpenRouter::apiUrl() const { return url() + QStringLiteral("/api/v1"); }

void MockOpenRouter::script(const QString& pattern, const MockReply& reply, int times) {
    if (times == 0) return;
    QMutexLocker lock(&m_state->mu);
    m_state->scripts.append({parsePattern(pattern), reply, times < 0 ? -1 : times});
}

void MockOpenRouter::failPath(const QString& pattern) {
    MockReply r = MockReply::json(QJsonObject{{QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QStringLiteral("mock failure")}}}}, 500);
    script(pattern, r, -1);
}

void MockOpenRouter::clearScripts() {
    QMutexLocker lock(&m_state->mu);
    m_state->scripts.clear();
}

int MockOpenRouter::pendingScripts() const {
    QMutexLocker lock(&m_state->mu);
    return int(m_state->scripts.size());
}

void MockOpenRouter::setLatency(const QString& pattern, int ms) {
    QMutexLocker lock(&m_state->mu);
    const Pattern p = parsePattern(pattern);
    m_state->latency.removeIf([&](const State::Knob& k) { return k.pattern.method == p.method && k.pattern.path == p.path && k.pattern.prefix == p.prefix && k.pattern.any == p.any; });
    if (ms > 0) m_state->latency.append({p, ms, 0});
}

void MockOpenRouter::setConcurrencyLimit(const QString& pattern, int maxInFlight, int retryAfterMs) {
    QMutexLocker lock(&m_state->mu);
    const Pattern p = parsePattern(pattern);
    m_state->concurrency.removeIf([&](const State::Knob& k) { return k.pattern.method == p.method && k.pattern.path == p.path && k.pattern.prefix == p.prefix && k.pattern.any == p.any; });
    if (maxInFlight > 0) m_state->concurrency.append({p, maxInFlight, retryAfterMs});
}

void MockOpenRouter::setRequiredKey(const QByteArray& key) {
    QMutexLocker lock(&m_state->mu);
    m_state->requiredKey = key;
}

void MockOpenRouter::setMaxBodyBytes(qint64 bytes) {
    QMutexLocker lock(&m_state->mu);
    m_state->maxBody = bytes;
}

void MockOpenRouter::setChatHandler(ChatHandler handler) {
    QMutexLocker lock(&m_state->mu);
    m_state->chat = std::move(handler);
}

void MockOpenRouter::setJsonReply(const QString& schemaName, JsonHandler handler) {
    QMutexLocker lock(&m_state->mu);
    if (handler) m_state->jsonHandlers.insert(schemaName, std::move(handler));
    else m_state->jsonHandlers.remove(schemaName);
}

void MockOpenRouter::setUsageCost(const QString& pattern, double usd) {
    QMutexLocker lock(&m_state->mu);
    const Pattern p = parsePattern(pattern);
    m_state->usage.removeIf([&](const State::Cost& c) { return c.pattern.path == p.path && c.pattern.prefix == p.prefix && c.pattern.any == p.any; });
    if (usd >= 0) m_state->usage.append({p, usd});
}

void MockOpenRouter::setCatalogNow(qint64 epochSeconds) {
    QMutexLocker lock(&m_state->mu);
    m_state->catalogNow = epochSeconds;
}

void MockOpenRouter::setVideoStatuses(const QStringList& statuses) {
    QMutexLocker lock(&m_state->mu);
    m_state->videoStatuses = statuses;
}

void MockOpenRouter::setVideoContent(const QByteArray& bytes) {
    QMutexLocker lock(&m_state->mu);
    m_state->videoContent = bytes;
}

void MockOpenRouter::setVideoCost(double usd) {
    QMutexLocker lock(&m_state->mu);
    m_state->videoCost = usd;
}

void MockOpenRouter::setVideoPollUrl(PollUrl style) {
    QMutexLocker lock(&m_state->mu);
    m_state->pollUrl = style;
}

void MockOpenRouter::setImageBytes(const QByteArray& bytes) {
    QMutexLocker lock(&m_state->mu);
    m_state->imageBytes = bytes;
}

void MockOpenRouter::setSpeechBytes(const QByteArray& bytes) {
    QMutexLocker lock(&m_state->mu);
    m_state->speechBytes = bytes;
}

void MockOpenRouter::setGenerationCost(const QString& id, double usd) {
    QMutexLocker lock(&m_state->mu);
    m_state->generationCost.insert(id, usd);
}

void MockOpenRouter::setRejectVerboseJson(bool reject) {
    QMutexLocker lock(&m_state->mu);
    m_state->rejectVerbose = reject;
}

QList<MockRequest> MockOpenRouter::requests(const QString& pattern) const {
    const Pattern p = parsePattern(pattern);
    QMutexLocker lock(&m_state->mu);
    if (p.any) return m_state->log;
    QList<MockRequest> out;
    for (const MockRequest& r : m_state->log)
        if (matches(p, r.method, r.path)) out.append(r);
    return out;
}

int MockOpenRouter::count(const QString& pattern) const {
    const Pattern p = parsePattern(pattern);
    QMutexLocker lock(&m_state->mu);
    int n = 0;
    for (const MockRequest& r : m_state->log)
        if (matches(p, r.method, r.path)) ++n;
    return n;
}

std::optional<MockRequest> MockOpenRouter::last(const QString& pattern) const {
    const Pattern p = parsePattern(pattern);
    QMutexLocker lock(&m_state->mu);
    for (qsizetype i = m_state->log.size() - 1; i >= 0; --i)
        if (matches(p, m_state->log[i].method, m_state->log[i].path)) return m_state->log[i];
    return std::nullopt;
}

bool MockOpenRouter::waitForRequests(const QString& pattern, int n, int timeoutMs) const {
    return QTest::qWaitFor([&]() { return count(pattern) >= n; }, timeoutMs);
}

void MockOpenRouter::clearRequests() {
    QMutexLocker lock(&m_state->mu);
    m_state->log.clear();
}

int MockOpenRouter::inFlight(const QString& path) const {
    QMutexLocker lock(&m_state->mu);
    return path.isEmpty() ? m_state->inflightAll : m_state->inflight.value(normalizePath(path));
}

int MockOpenRouter::peakInFlight(const QString& path) const {
    QMutexLocker lock(&m_state->mu);
    return path.isEmpty() ? m_state->peakAll : m_state->peak.value(normalizePath(path));
}

int MockOpenRouter::connectionsAccepted() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->accepted;
}

int MockOpenRouter::openConnections() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->open;
}

void MockOpenRouter::resetStats() {
    QMutexLocker lock(&m_state->mu);
    m_state->log.clear();
    m_state->peak.clear();
    m_state->peakAll = m_state->inflightAll;
    for (auto it = m_state->inflight.cbegin(); it != m_state->inflight.cend(); ++it) m_state->peak.insert(it.key(), it.value());
    m_state->accepted = 0;
}

void MockOpenRouter::releaseStalled() {
    if (!m_server) return;
    if (m_thread) QMetaObject::invokeMethod(m_server, [this]() { m_server->releaseStalled(); }, Qt::BlockingQueuedConnection);
    else m_server->releaseStalled();
}

QJsonArray MockOpenRouter::models() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->modelsLocked();
}

QStringList MockOpenRouter::modelIds() const {
    QStringList ids;
    QMutexLocker lock(&m_state->mu);
    for (const QJsonValue& v : m_state->models) ids << v.toObject().value(QStringLiteral("id")).toString();
    return ids;
}

QJsonObject MockOpenRouter::cannedJson() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->canned;
}

QJsonObject MockOpenRouter::replies() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->replies;
}

QString MockOpenRouter::fixturesDir() const {
    QMutexLocker lock(&m_state->mu);
    return m_state->fixturesDir;
}

// ------------------------------------------------------------------------------------------------------------------
// helpers
// ------------------------------------------------------------------------------------------------------------------

QByteArray MockOpenRouter::speechWav() {
    // test/mock.ts SPEECH: RIFF header + 3 s of a 220 Hz tone at 24 kHz mono 16-bit, the last 10 % silent
    const double kPi = 3.141592653589793;   // JS Math.PI (MSVC has no M_PI without _USE_MATH_DEFINES)
    const int sr = 24000, n = sr * 3;
    QByteArray b(44 + n * 2, '\0');
    char* p = b.data();
    auto u32 = [&](int off, quint32 v) { qToLittleEndian<quint32>(v, p + off); };
    auto u16 = [&](int off, quint16 v) { qToLittleEndian<quint16>(v, p + off); };
    memcpy(p, "RIFF", 4);
    u32(4, quint32(36 + n * 2));
    memcpy(p + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, 1);
    u32(24, quint32(sr));
    u32(28, quint32(sr * 2));
    u16(32, 2);
    u16(34, 16);
    memcpy(p + 36, "data", 4);
    u32(40, quint32(n * 2));
    for (int i = 0; i < n; ++i) {
        const double x = std::sin((2 * kPi * 220 * double(i)) / sr) * 6000 * ((double(i) / n) < 0.9 ? 1 : 0);
        const double f = std::floor(x);
        const double r = (x - f >= 0.5) ? f + 1 : f;   // JS Math.round: halves go up
        qToLittleEndian<qint16>(qint16(r), p + 44 + i * 2);
    }
    return b;
}

QByteArray MockOpenRouter::pixelPng() { return QByteArray::fromBase64(kPngB64); }

QVector<double> MockOpenRouter::embedText(const QString& text) {
    // test/mock.ts embedText: lower-case, words = runs of [a-z0-9], h = (h * 31 + code) >>> 0, v[h % 64] += 1
    QVector<double> v(64, 0.0);
    const QString t = text.toLower();
    quint32 h = 0;
    bool inWord = false;
    for (QChar ch : t) {
        const ushort u = ch.unicode();
        if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
            if (!inWord) {
                h = 0;
                inWord = true;
            }
            h = h * 31u + u;
        } else if (inWord) {
            v[int(h % 64u)] += 1;
            inWord = false;
        }
    }
    if (inWord) v[int(h % 64u)] += 1;
    return v;
}

double MockOpenRouter::wavSeconds(const QByteArray& wav) {
    if (wav.size() < 12 || !wav.startsWith("RIFF") || wav.mid(8, 4) != "WAVE") return -1;
    const uchar* p = reinterpret_cast<const uchar*>(wav.constData());
    qsizetype off = 12;
    quint32 byteRate = 0;
    while (off + 8 <= wav.size()) {
        const QByteArray id = wav.mid(off, 4);
        const quint32 size = qFromLittleEndian<quint32>(p + off + 4);
        const qsizetype data = off + 8;
        if (id == "fmt " && data + 12 <= wav.size()) byteRate = qFromLittleEndian<quint32>(p + data + 8);
        if (id == "data") {
            if (!byteRate) return -1;
            const qint64 avail = wav.size() - data;
            const qint64 len = (size == 0xFFFFFFFFu || qint64(size) > avail) ? avail : qint64(size);
            return double(len) / double(byteRate);
        }
        off = data + qsizetype(size) + (size & 1);
    }
    return -1;
}

QByteArray MockOpenRouter::httpDate(const QDateTime& when) {
    return QLocale::c().toString(when.toUTC(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss")).toLatin1() + " GMT";
}

namespace {

QJsonValue sample(const QJsonValue& schemaValue, const QJsonObject& root, const QString& name, int depth) {
    if (depth > 24 || !schemaValue.isObject()) return QStringLiteral("mock");
    QJsonObject s = schemaValue.toObject();
    const QString ref = s.value(QStringLiteral("$ref")).toString();
    if (!ref.isEmpty()) {
        QJsonValue target = root;
        if (ref != QLatin1String("#")) {
            if (!ref.startsWith(QLatin1String("#/"))) return QStringLiteral("mock");
            for (const QString& part : ref.mid(2).split(QLatin1Char('/'))) {
                QString key = part;
                key.replace(QLatin1String("~1"), QLatin1String("/")).replace(QLatin1String("~0"), QLatin1String("~"));
                target = target.toObject().value(key);
            }
        }
        return sample(target, root, name, depth + 1);
    }
    if (s.contains(QStringLiteral("const"))) return s.value(QStringLiteral("const"));
    const QJsonArray en = s.value(QStringLiteral("enum")).toArray();
    if (!en.isEmpty()) return en.first();
    if (s.contains(QStringLiteral("default"))) return s.value(QStringLiteral("default"));
    for (const char* key : {"anyOf", "oneOf"}) {
        const QJsonArray opts = s.value(QLatin1String(key)).toArray();
        if (opts.isEmpty()) continue;
        for (const QJsonValue& o : opts)
            if (o.toObject().value(QStringLiteral("type")).toString() != QLatin1String("null")) return sample(o, root, name, depth + 1);
        return QJsonValue::Null;
    }
    const QJsonArray all = s.value(QStringLiteral("allOf")).toArray();
    if (!all.isEmpty()) {
        QJsonObject merged = s;
        merged.remove(QStringLiteral("allOf"));
        QJsonObject props = merged.value(QStringLiteral("properties")).toObject();
        for (const QJsonValue& part : all) {
            QJsonObject po = part.toObject();
            if (po.contains(QStringLiteral("$ref"))) {
                const QJsonValue r = sample(po, root, name, depth + 1);
                if (r.isObject())
                    for (auto it = r.toObject().constBegin(); it != r.toObject().constEnd(); ++it) props.insert(it.key(), QJsonObject{{QStringLiteral("const"), it.value()}});
                continue;
            }
            const QJsonObject pp = po.value(QStringLiteral("properties")).toObject();
            for (auto it = pp.constBegin(); it != pp.constEnd(); ++it) props.insert(it.key(), it.value());
            if (!merged.contains(QStringLiteral("type")) && po.contains(QStringLiteral("type"))) merged.insert(QStringLiteral("type"), po.value(QStringLiteral("type")));
        }
        merged.insert(QStringLiteral("properties"), props);
        return sample(merged, root, name, depth + 1);
    }
    QString type;
    const QJsonValue tv = s.value(QStringLiteral("type"));
    if (tv.isString()) type = tv.toString();
    else if (tv.isArray()) {
        for (const QJsonValue& t : tv.toArray())
            if (t.toString() != QLatin1String("null")) {
                type = t.toString();
                break;
            }
        if (type.isEmpty()) return QJsonValue::Null;
    }
    if (type.isEmpty()) type = s.contains(QStringLiteral("properties")) ? QStringLiteral("object") : s.contains(QStringLiteral("items")) ? QStringLiteral("array") : QStringLiteral("string");
    if (type == QLatin1String("object")) {
        QJsonObject out;
        const QJsonObject props = s.value(QStringLiteral("properties")).toObject();
        for (auto it = props.constBegin(); it != props.constEnd(); ++it) out.insert(it.key(), sample(it.value(), root, it.key(), depth + 1));
        return out;
    }
    if (type == QLatin1String("array")) {
        int n = s.value(QStringLiteral("minItems")).toInt(1);
        if (s.contains(QStringLiteral("maxItems"))) n = std::min(n, s.value(QStringLiteral("maxItems")).toInt());
        n = std::clamp(n, 0, 16);
        QJsonArray out;
        for (int i = 0; i < n; ++i) out.append(sample(s.value(QStringLiteral("items")), root, name, depth + 1));
        return out;
    }
    if (type == QLatin1String("integer") || type == QLatin1String("number")) {
        const bool integer = type == QLatin1String("integer");
        double v = 0;
        if (s.contains(QStringLiteral("minimum"))) v = s.value(QStringLiteral("minimum")).toDouble();
        else if (s.contains(QStringLiteral("exclusiveMinimum"))) v = s.value(QStringLiteral("exclusiveMinimum")).toDouble() + (integer ? 1 : 0.5);
        if (s.contains(QStringLiteral("maximum"))) v = std::min(v, s.value(QStringLiteral("maximum")).toDouble());
        if (integer) v = std::ceil(v);
        return v;
    }
    if (type == QLatin1String("boolean")) return false;
    if (type == QLatin1String("null")) return QJsonValue::Null;
    // string
    const QString format = s.value(QStringLiteral("format")).toString();
    QString v = format == QLatin1String("date-time") ? QStringLiteral("2026-01-01T00:00:00Z")
              : format == QLatin1String("date")      ? QStringLiteral("2026-01-01")
              : format == QLatin1String("uri") || format == QLatin1String("url") ? QStringLiteral("https://example.com/")
              : format == QLatin1String("email")     ? QStringLiteral("mock@example.com")
              : name.isEmpty()                       ? QStringLiteral("mock")
                                                     : QStringLiteral("mock ") + name;
    const int minLen = s.value(QStringLiteral("minLength")).toInt(0);
    if (v.size() < minLen) v += QString(minLen - v.size(), QLatin1Char('x'));
    if (s.contains(QStringLiteral("maxLength"))) v.truncate(std::max(0, s.value(QStringLiteral("maxLength")).toInt()));
    return v;
}

}   // namespace

QJsonValue MockOpenRouter::sampleFromSchema(const QJsonValue& schema, const QJsonObject& root, const QString& name) {
    return sample(schema, root.isEmpty() ? schema.toObject() : root, name, 0);
}

}   // namespace studio::test
