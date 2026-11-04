#pragma once
// The wire to OpenRouter (ST-07a): one call to the API, a port of request() in starmedia src/openrouter.ts (L345-466)
// on the Qt event loop, through the request gates of ST-06.
//
// What a call does, in order (the TS order):
//   1. refuses before sending: a body over the path's safe budget (payload::bodyCap: 48 MiB, /videos 29 MiB) or over a
//      provider's own request cap (limits::requestCapFor) answers 413 at once, without a byte uploaded, a gate slot or
//      a retry; a model whose quota is known to be used up (the QuotaBreaker) answers 429 "remembered" at once.
//   2. waits for its gate slot (gates.h: gateFor, the provider sub-gate key, the heavy cap); the timeout starts with the
//      request, not while it waits in line. The slot is held only while the request is on the wire and is given back
//      before any retry sleep.
//   3. sends, under a timer that covers the WHOLE answer (a server that sends headers and then stalls cannot hold a slot
//      or a run for ever).
//   4. reads the answer: 2xx is a success (the gate's AIMD counts it). An error answer goes through classifyLimit()
//      first: a rate limit slows the gate down BEFORE the slot is given back and is retried after Retry-After (when it is
//      at most 60 s); a used-up quota, a wait too long to sit through or StarMedia's spend cap is not retried, and the
//      quota is remembered for the model (breaker + model health, one decision). Other 429 / 5xx answers are retried
//      with back-off; anything else fails with the server's words (OpenRouter's "Provider returned error" gets the
//      upstream provider's name and message appended).
//   5. a Stop (AbortSignal) takes the call out of the gate line, aborts it on the wire, ends a retry sleep, and is never
//      retried.
//
// Safety (ST-15 provider hook): after the size checks and before the gate line, every POST whose path can make a
// picture, a video or cloned speech (/images*, /videos, /chat/completions and /responses asking for image or video
// output, /audio/* with audio references) goes through safety::checkOutgoing(path, body, Request::safety). A refusal
// fails the call at once (Kind::Refused, sentNothing, the verdict's reason and code, never retried, nothing sent). A
// picture or video body that is not a JSON object cannot be read and is refused (fail closed).
//
// Money safety: the POSTs that start paid generation work (videos / images / audio) are NEVER sent twice once they may
// have reached the provider:
//   - a paid POST that timed out is not re-sent (TS L457-461): the error has outcomeUnknown = true, and the executor
//     marks the node `unknown` (reconcile via generationCost / polling or a person);
//   - stricter than the TS: the same holds for a connection that broke after any request byte was written (a reset or
//     a close before a complete answer), and a Stop after sending carries outcomeUnknown as well. Only a failure before
//     a byte left (connection refused, DNS, TLS handshake) is retried;
//   - an ANSWERED error is retried as in the TS (the server said no), except a gateway timeout / bad gateway on a paid
//     POST (502, 504, 520-524): a proxy in front of the provider gave up waiting, the generation may already run and be
//     billed, so it is outcomeUnknown and not re-sent (stricter than the TS);
//   - paid POSTs do not go through QNetworkAccessManager at all: QNAM re-sends a request on its own when its connection
//     closes before an answer byte (observed in ST-05, no public switch to turn it off). They go out on a one-shot
//     HTTP/1.1 connection of their own (Connection: close), which never sends anything twice. Chat, embeddings, rerank
//     and every GET use QNAM (HTTP/2 when the server offers it; the protocol each origin answered with is logged once
//     at info, and every answer's Http2WasUsedAttribute is in Response::http2 and the debug line).
//
// Secrets: the OpenRouter key (Config::openRouterKey(), read per request) goes only to the OpenRouter origin, never to
// an absolute URL elsewhere and never through a redirect to another origin. Diagnostics log the path only (no query
// string, no body, no header), like openrouter.ts L359.
//
// Threads: a Wire lives on one thread (the Studio's main loop) with the Gates it uses; request() is called on that
// thread. Callbacks are queued to `context`'s thread, never run inside request(), and skipped when `context` is gone.
#include "provider/backpressure.h"
#include "provider/gates.h"
#include "safety/guards.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace studio {
class Clock;
class Config;
}

namespace studio::provider {

class ModelHealth;
struct ProviderError;
namespace detail {
struct WireLink;
}

struct Request {
    QByteArray method = "GET";
    /// an API path ("/chat/completions", "/videos/abc?x=1") resolved against the base, or an absolute URL
    QString path;
    /// the body (POST); std::nullopt sends none
    std::optional<QByteArray> body;
    /// Content-Type for the body
    QByteArray contentType = "application/json";
    /// more headers (Authorization and Content-Type are set by the wire)
    HeaderList headers;
    qint64 timeoutMs = 180000;
    int retries = 3;
    /// the model whose "daily quota is used up" answer is remembered (TS quotaKey); empty = none
    QString quotaKey;
    /// the model the request is for: its provider's request cap, the provider sub-gate key and providerBase()
    QString model;
    AbortSignal signal;
    /// largest answer body accepted (bytes); a bigger one fails the call
    qint64 maxResponseBytes = qint64(256) << 20;
    /// what the ST-15 provider hook needs beyond the body (rule: Spot unless a plain picture / video tool; the cloned
    /// voice profile for cloned speech)
    safety::SendContext safety;
};

struct Response {
    int status = 0;
    HeaderList headers;
    QByteArray body;
    /// QNetworkRequest::Http2WasUsedAttribute (always false on the one-shot path, which is HTTP/1.1)
    bool http2 = false;
    /// requests sent for this call (1 + retries)
    int attempts = 0;
    qint64 ms = 0;
    QByteArray header(const QByteArray& name) const;     // case-insensitive, repeated values joined by ", "
    QJsonValue json() const;                             // Undefined when the body is not JSON
};

/// What travels on a limit answer's error (TS ProviderLimitError.info).
struct LimitInfo {
    LimitKind kind = LimitKind::Rate;
    QString gate;                                        // "chat" ... (empty when the request had none)
    QString model;
    std::optional<qint64> retryAfterMs;
    QString limitProvider;
    int retries = 0;
    bool remembered = false;                             // an earlier answer said so; nothing was sent now
};

struct WireError {
    enum class Kind {
        Http,          // the server answered an error (status set)
        Limit,         // a limit answer (rate / quota / cap; `limit` set), or a remembered quota
        TooLarge,      // 413: refused before sending (status 413, nothing sent) or answered 413
        Timeout,       // no complete answer in time
        Network,       // the connection failed (refused, reset, closed early, TLS ...)
        Cancelled,     // the caller's Stop
        Invalid,       // the request itself is unusable (bad URL); nothing was sent
        Refused,       // the safety hook (ST-15) refused the body before sending; `safetyCode` set, nothing was sent
    };
    Kind kind = Kind::Network;
    int status = 0;                                      // HTTP status, 0 = no answer
    QString message;                                     // OpenRouterError style: "<status> <text>" when answered
    QJsonValue body;                                     // the answer (JSON, or its text)
    QString provider;                                    // error.metadata.provider_name, when given
    QString providerMessage;                             // error.metadata.raw, one line
    std::optional<LimitInfo> limit;
    /// a paid request may have reached the provider and may run (and be billed): it was NOT sent again. The executor
    /// marks the node `unknown`; reconcile before trying again.
    bool outcomeUnknown = false;
    /// nothing reached the provider (refused before sending, remembered quota, cancelled in the line)
    bool sentNothing = false;
    int attempts = 0;
    /// the safety verdict's code ("real_person", "voice_consent:...", "safety_data_unavailable") for Kind::Refused
    QString safetyCode;

    QString kindName() const;                            // "http" | "limit" | "too_large" | "timeout" | "refused" | ...
    /// waiting will not help (a used-up quota, the spend cap): move to a fallback model instead of retrying
    bool hopeless() const { return limit && limit->kind != LimitKind::Rate; }
    /// The shape model health reads (modelhealth.h classifyError).
    ProviderError toProviderError() const;
    QJsonObject toJson() const;                          // safe to return from the API (no secrets, no request body)
};

/// The upstream provider's own words behind an OpenRouter error (TS providerDetail): error.metadata.provider_name
/// (<= 60 chars) and error.metadata.raw (a string, often JSON; its message picked out, one line, <= 300 chars).
struct ProviderDetail { QString provider; QString raw; };
ProviderDetail providerDetail(const QJsonValue& body);

/// The TS user-facing texts, exactly as the TS words them.
QString fmtWait(qint64 ms);
QString limitMessage(int status, const LimitSignal& sig, const QString& model, int retries, bool remembered, bool asked);
QString tooLargeMessage(QStringView path, qint64 bytes, const QString& upstream);

struct WireOptions {
    /// the OpenRouter base; empty = Config::openRouterBaseUrl() (OPENROUTER_BASE_URL, else https://openrouter.ai/api/v1)
    QString baseUrl;
    /// other providers that speak the same wire format, by model-id prefix ("local" -> "http://127.0.0.1:4317/v1"): a
    /// call whose model starts with "<prefix>/" goes there, with STARMEDIA_PROVIDER_TOKEN instead of the OpenRouter key
    QMap<QString, QString> providers;
    /// upper bound for every request's own timeout (ms); 0 = none
    qint64 maxTimeoutMs = 0;
    /// back-off after a failed answer / connection: base * 2^attempt (+ jitter * rand for answers)
    qint64 backoffBaseMs = 1000;
    qint64 backoffJitterMs = 300;
    /// the local server's "every generation slot is taken" answer (503 + x-local-busy): one wait of this (+ 500 * rand)
    qint64 busyWaitMs = 2000;
    /// paid POSTs on the one-shot transport (never re-sent). Off only for diagnostics: QNAM may then send twice.
    bool paidOneShot = true;
    /// [0, 1) for jitter; default QRandomGenerator::global()
    std::function<double()> rand;
    /// X-Title (OPENROUTER_TITLE when set)
    QString title = QStringLiteral("PlayPlay Studio");
};

class Wire : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(const Response&)>;
    using Failed = std::function<void(const WireError&)>;

    Wire(Config& config, Gates& gates, Clock& clock, QObject* parent = nullptr);
    ~Wire() override;

    void setOptions(const WireOptions& o);
    const WireOptions& options() const { return m_options; }
    /// the model-health memory a quota / long-wait answer rests the model in (TS getHealth().mark); may be null
    void setHealth(ModelHealth* h) { m_health = h; }
    Gates& gates() { return m_gates; }
    Clock& clock() { return m_clock; }

    /// The OpenRouter base without a trailing "/".
    QString baseUrl() const;
    /// The base of the provider that serves this model id (by its prefix), or empty for OpenRouter.
    QString providerBase(QStringView model) const;

    /// One call (see the top of this file). Exactly one of `done` / `failed` runs, queued on `context`'s thread (on
    /// this Wire's thread when `context` is null); neither runs when `context` was destroyed. Returns the call id.
    quint64 request(Request rq, QObject* context, Done done, Failed failed);
    /// Calls not finished yet.
    int pending() const { return int(m_calls.size()); }

    /// {calls, sent, retries, refusedBeforeSend, refusedBySafety, unknownOutcome, cancelled, http2, http1, oneShot, inFlight}
    QJsonObject stats() const;

signals:
    /// A provider limit answer was seen (TS onProviderLimit): {gate, kind, provider, message, retryAfterMs, model, cut,
    /// willRetry}.
    void providerLimit(const QJsonObject& event);

private:
    struct Call;
    friend struct Call;
    void begin(const std::shared_ptr<Call>& c);
    void attempt(const std::shared_ptr<Call>& c);
    void send(const std::shared_ptr<Call>& c);
    void onAnswer(const std::shared_ptr<Call>& c, Response r);
    void onBroken(const std::shared_ptr<Call>& c, const QString& why, bool sentSomething);
    void retryAfter(const std::shared_ptr<Call>& c, qint64 ms);
    /// `mayHaveRun`: the caller already took the transfer down and knows a paid request may have reached the provider
    void stop(const std::shared_ptr<Call>& c, bool mayHaveRun = false);
    std::optional<WireError> safetyRefusal(const std::shared_ptr<Call>& c);
    void fail(const std::shared_ptr<Call>& c, WireError e);
    void succeed(const std::shared_ptr<Call>& c, Response r);
    void finish(const std::shared_ptr<Call>& c);
    std::optional<WireError> rememberedQuota(const std::shared_ptr<Call>& c);
    HeaderList headersFor(const std::shared_ptr<Call>& c) const;
    double rnd() const;
    void noteProtocol(const QUrl& url, bool http2);

    Config& m_config;
    Gates& m_gates;
    Clock& m_clock;
    ModelHealth* m_health = nullptr;
    WireOptions m_options;
    QNetworkAccessManager* m_nam = nullptr;
    std::shared_ptr<detail::WireLink> m_link;           // how an abort listener (any thread) finds this Wire
    QHash<quint64, std::shared_ptr<Call>> m_calls;
    quint64 m_nextId = 0;
    QSet<QString> m_protocolSeen;                        // "<origin> h2|http/1.1" logged once
    struct Stats { quint64 calls = 0, sent = 0, retries = 0, refused = 0, safety = 0, unknown = 0, cancelled = 0, http2 = 0, http1 = 0, oneShot = 0; };
    Stats m_stats;
};

} // namespace studio::provider
