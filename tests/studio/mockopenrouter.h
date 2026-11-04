// ST-05: a mock OpenRouter for the Studio's hermetic tests (docs/plans/starmedia-studio.md §8). It speaks the wire formats
// of StarMedia's own test mock (starmedia test/mock.ts, startMock()) over plain HTTP/1.1 on an ephemeral loopback port,
// so the C++ provider client (ST-07) and everything above it (catalog, executor, agents, spot, film) can run without a
// key, a network or a cent of spend.
//
// Routes (every route also answers under /api/v1/..., so a client configured with <url>/api/v1 works unchanged):
//   GET  /models[?...]                    the fixture catalog (tests/studio/fixtures/models.json, exported from mock.ts)
//   GET  /models/{id}/endpoints           {data:{id, endpoints:[{supports_voice_cloning}]}}
//   GET  /images/models, /images/models/{id}/endpoints, /videos/models
//   POST /chat/completions                json_schema answers (canned TS answers by schema name, a test's own handler, or
//                                         an object synthesized from the request's schema), tool calls (tools /
//                                         tool_choice, then a final answer after a role:"tool" message), plain text (the
//                                         TS article / read-aloud texts), usage {prompt_tokens, completion_tokens, cost}
//   POST /images                          {data:[{b64_json, media_type}] x n, usage:{cost}}
//   POST /videos -> 202 {id, polling_url, status}; GET /videos/{id} (scripted status sequence); GET /videos/{id}/content
//   POST /audio/speech                    the TS mock's 3 s 24 kHz WAV tone, x-generation-id
//   POST /audio/transcriptions            text + duration (from the WAV header when the input is WAV) + segments
//   POST /embeddings, POST /rerank, GET /generation?id=, GET /credits
// Anything else: 404 {"error":{"message":"mock: no route <path>"}}, as the TS mock answers.
//
// Scripting (thread-safe, from the test): script("[METHOD ]/path[*]", reply, times) queues answers that win over the
// routes: OpenRouter-shaped failures (429 with Retry-After / retry-after-ms / an HTTP date, the daily free-model quota,
// 402 no credit, 503, a 502 "Provider returned error" with metadata, the StarMedia spend-cap 429, 413) and delivery
// faults (a delay before the status line, a slow body trickled in chunks, a body that stalls after N bytes, a body cut
// by a close, a connection dropped or reset without an answer). MockReply::normal() keeps the route's own answer and
// changes only its delivery. setLatency / setConcurrencyLimit shape every answer of a path; requests() records what
// arrived (method, decoded path, query, headers, body, parsed JSON, connection number) and inFlight / peakInFlight
// count answers being served, so gate widths can be checked from the server side.
//
// Threading: by default the mock serves from the thread that calls start(), so tests spin the event loop while they wait
// (QTest::qWait / qWaitFor, never a blocking waitFor*()). Options::ownThread serves from a thread of its own, for tests
// whose client blocks the calling thread. Handlers installed with setChatHandler / setJsonReply run on the serving
// thread. The mock never logs request headers or bodies (a test key in Authorization stays in the test).
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

class QThread;

namespace studio::test {

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

// One request as it arrived (recorded when its last body byte was read).
struct MockRequest {
    qint64 seq = 0;              // 1-based order of arrival
    int connection = 0;          // 1-based connection number; equal for requests on one keep-alive connection
    QByteArray method;
    QString path;                // percent-decoded, without the query and without an /api/v1 prefix
    QString target;              // the request target exactly as sent
    QString query;               // the raw query string (no '?')
    HeaderList headers;          // as received, in order
    QByteArray body;             // cut at Options::recordBodyLimit (bodySize is always the full size)
    qint64 bodySize = 0;
    bool bodyTruncated = false;
    bool chunked = false;        // the body came with Transfer-Encoding: chunked
    QJsonObject json;            // the body as a JSON object ({} when it is not one)
    qint64 receivedMs = 0;       // wall clock (ms since epoch) when the last body byte arrived

    QByteArray header(const QByteArray& name) const;     // case-insensitive, first match, empty if absent
    bool hasHeader(const QByteArray& name) const;
    QString queryValue(const QString& key) const;        // decoded; empty if absent
};

// A scripted answer, or (passthrough) only a changed delivery of the route's own answer.
struct MockReply {
    int status = 200;
    QByteArray contentType = "application/json";
    QByteArray body;
    HeaderList headers;
    bool passthrough = false;    // answer as the routes would; only the delivery fields below apply
    // delivery
    int headerDelayMs = 0;       // wait this long before the status line (headers that never come in time)
    int chunkBytes = 0;          // > 0: write the body in pieces of this size ...
    int chunkIntervalMs = 0;     // ... one piece per interval (the first at once)
    qint64 stallAfterBytes = -1; // >= 0: write this many body bytes, then nothing more (the connection stays open)
    qint64 closeAfterBytes = -1; // >= 0: write this many body bytes, then close (a body shorter than Content-Length)
    bool drop = false;           // close (FIN) without writing anything
    bool reset = false;          // abort (RST) without writing anything
    bool close = false;          // Connection: close after this answer

    static MockReply json(const QJsonObject& body, int status = 200);
    static MockReply raw(const QByteArray& body, const QByteArray& contentType = "application/octet-stream", int status = 200);
    // {"error":{"code":status,"message":...,"metadata":...}}, OpenRouter's error shape
    static MockReply error(int status, const QString& message, const QJsonObject& metadata = {});
    // 429 "Rate limit exceeded ..." with Retry-After: <seconds> (omitted when < 0)
    static MockReply rateLimited(int retryAfterSeconds = 1, const QString& message = {});
    static MockReply rateLimitedMs(int retryAfterMs, const QString& message = {});        // retry-after-ms
    static MockReply rateLimitedUntil(const QDateTime& when, const QString& message = {}); // Retry-After as an HTTP date
    // 429 free-models-per-day with X-RateLimit-Limit / -Remaining / -Reset (epoch ms; default the next UTC midnight)
    static MockReply dailyQuota(qint64 resetEpochMs = 0);
    static MockReply noCredit();                                      // 402 "Insufficient credits ..." (a payment error)
    static MockReply unavailable(int retryAfterSeconds = -1);         // 503
    static MockReply providerError(int status, const QString& provider, const QString& raw);   // "Provider returned error"
    static MockReply spendCap();                                      // the StarMedia proxy's own daily cap (429)
    static MockReply tooLarge();                                      // 413
    static MockReply serverError();                                   // 500
    static MockReply normal();                                        // passthrough
    static MockReply dropped();
    static MockReply resetConnection();

    MockReply& withHeader(const QByteArray& name, const QByteArray& value);
    MockReply& delayed(int ms);
    MockReply& slowBody(int chunkBytes, int intervalMs);
    MockReply& stallAfter(qint64 bytes);
    MockReply& truncateAfter(qint64 bytes);
    MockReply& closing();
};

class MockOpenRouter {
public:
    enum class PollUrl { Absolute, ApiRelative, Relative, None };   // the shape of polling_url in a video submit answer

    struct Options {
        quint16 port = 0;                          // 0 = ephemeral (always use 0 in tests)
        bool ownThread = false;                    // serve from a thread of its own
        QString fixturesDir;                       // empty = tests/studio/fixtures of this source tree
        qint64 recordBodyLimit = 16 << 20;         // bodies are kept in requests() up to this size
        qint64 maxBodyBytes = qint64(256) << 20;   // larger requests are answered 413 and closed
    };

    MockOpenRouter();
    ~MockOpenRouter();
    MockOpenRouter(const MockOpenRouter&) = delete;
    MockOpenRouter& operator=(const MockOpenRouter&) = delete;

    bool start() { return start(Options{}); }
    bool start(const Options& options);
    void stop();                                    // closes every connection; safe to call twice
    bool isRunning() const;
    QString errorString() const;
    quint16 port() const;
    QString url() const;                            // http://127.0.0.1:<port> (the TS mock's base)
    QString apiUrl() const;                         // url() + "/api/v1" (OpenRouter's own layout)

    // ---- scripting (any thread) ----
    // pattern: "" = every request; "/path" exact; "/prefix*" prefix; "POST /path" also matches the method.
    void script(const QString& pattern, const MockReply& reply, int times = 1);   // times < 0: until clearScripts()
    void failPath(const QString& pattern);          // the TS mock's `fail` set: 500 "mock failure" until clearScripts()
    void clearScripts();
    int pendingScripts() const;                     // entries not used up yet
    void setLatency(const QString& pattern, int ms);                // extra delay before every matching answer (0 = off)
    // more than maxInFlight answers in flight on a matching path: 429 with retry-after-ms (0 = off)
    void setConcurrencyLimit(const QString& pattern, int maxInFlight, int retryAfterMs = 1000);
    void setRequiredKey(const QByteArray& key);     // non-empty: anything but "Authorization: Bearer <key>" gets 401
    void setMaxBodyBytes(qint64 bytes);

    // Chat: return the assistant message object ({role, content | tool_calls, ...}) and optionally finish_reason inside
    // it as "finish_reason" (moved to the choice); std::nullopt falls through to the built-in behaviour.
    using ChatHandler = std::function<std::optional<QJsonObject>(const MockRequest&)>;
    void setChatHandler(ChatHandler handler);
    // json_schema answers by schema name; `text` is the messages' contents joined by '\n' (as the TS mock passes it).
    using JsonHandler = std::function<QJsonValue(const MockRequest&, const QString& text)>;
    void setJsonReply(const QString& schemaName, JsonHandler handler);    // an empty handler removes it
    void setUsageCost(const QString& pattern, double usd);                 // usage.cost of matching answers; < 0 = default
    void setCatalogNow(qint64 epochSeconds);        // pins "now" for the catalog's created stamps (0 = the real clock)

    void setVideoStatuses(const QStringList& statuses);   // per poll, the last repeats; default {"completed"} (as TS)
    void setVideoContent(const QByteArray& bytes);         // default "fakemp4"
    void setVideoCost(double usd);                         // usage.cost on the completed poll; default 0.6
    void setVideoPollUrl(PollUrl style);
    void setImageBytes(const QByteArray& bytes);           // default the TS mock's 1x1 PNG
    void setSpeechBytes(const QByteArray& bytes);          // default speechWav()
    void setGenerationCost(const QString& id, double usd); // /generation?id=; default 0.05 (as TS)
    void setRejectVerboseJson(bool reject);                // transcriptions: response_format verbose_json -> 400

    // ---- what arrived (any thread) ----
    QList<MockRequest> requests(const QString& pattern = {}) const;
    int count(const QString& pattern = {}) const;
    std::optional<MockRequest> last(const QString& pattern = {}) const;
    bool waitForRequests(const QString& pattern, int n, int timeoutMs = 5000) const;   // spins the caller's event loop
    void clearRequests();
    int inFlight(const QString& path = {}) const;          // answers being served (exact path; empty = all)
    int peakInFlight(const QString& path = {}) const;
    int connectionsAccepted() const;
    int openConnections() const;
    void resetStats();                                     // peaks, counters and the request log
    void releaseStalled();                                 // close every connection whose answer stalled

    // ---- fixtures ----
    QJsonArray models() const;                     // as served now (created rebased)
    QStringList modelIds() const;
    QJsonObject cannedJson() const;                // schema name -> the TS answer
    QJsonObject replies() const;                   // mock-replies.json as loaded
    QString fixturesDir() const;
    static QString defaultFixturesDir();

    // ---- helpers (no server needed) ----
    static QByteArray speechWav();                 // the TS mock's SPEECH buffer, byte for byte
    static QByteArray pixelPng();                  // the TS mock's 1x1 PNG
    static QVector<double> embedText(const QString& text);   // the TS mock's 64-dim bag-of-words hash
    static double wavSeconds(const QByteArray& wav);          // < 0 when not a WAV this can read
    // A value that satisfies a JSON schema (object / array / string / number / integer / boolean / null, enum, const,
    // default, anyOf / oneOf / allOf, $ref into #/$defs or #/definitions, min/max bounds): the answer a strict-mode model
    // would give with every field present. `name` seeds string values ("mock <name>").
    static QJsonValue sampleFromSchema(const QJsonValue& schema, const QJsonObject& root = {}, const QString& name = {});
    static QByteArray httpDate(const QDateTime& when);        // IMF-fixdate (RFC 9110)

    struct State;
    class Server;
private:
    std::unique_ptr<State> m_state;
    Server* m_server = nullptr;
    QThread* m_thread = nullptr;
};

}   // namespace studio::test
