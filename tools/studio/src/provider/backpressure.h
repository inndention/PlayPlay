#pragma once
// Back-pressure (ST-06): what to do when a provider says "slow down". A port of starmedia src/backpressure.ts (283
// lines, commit of 2026-10) with the same names and the same behaviour:
//
//   classifyLimit()  reads an error answer (status, headers, body, text) and says what kind of "no" it is:
//                      Rate   too many requests / tokens per minute: waiting a little helps
//                      Quota  a daily (or billing) quota is used up: waiting does NOT help today
//                      Cap    StarMedia's own spend cap answered 429: not a provider limit, never retried
//   AdaptiveLimit    one per request gate: AIMD. A rate answer halves the gate's limit (once per episode, not once per
//                    request that happens to be in flight) and pauses the gate for the provider's Retry-After; every
//                    full window of successes (or a quiet stretch of time) gives one slot back, up to the configured
//                    limit.
//   QuotaBreaker     remembers "this model's quota is used up" for a while, so ten scenes do not each find out the hard
//                    way: the next call for that model fails at once and the stage moves on to its fallback model.
//   retryDelayMs()   the pause before a retry: the provider's Retry-After when it gave one, else exponential, with
//                    jitter.
//
// The regular expressions (QUOTA_RX, CAP_RX, RATE_RX and the duration readers) are the TS sources byte for byte,
// compiled through data::regex(), which keeps their ECMAScript meaning (\s with the Unicode spaces, `$` = end of
// input, `.` without line terminators). The words are the ONLY place that decides what a provider answer means: the
// request layer (ST-07) and the model-health layer (modelhealth.h) read answers through classifyLimit(), so the gate,
// the model's rest and the fallback can never disagree about one answer.
//
// Time: AdaptiveLimit and QuotaBreaker run on Clock::steadyMs() (immune to NTP / DST steps; the TS used Date.now()).
// Header dates and epoch resets are compared with the wall clock (`nowMs`, Clock::nowMs()). Nothing here sleeps or
// owns a timer: the gates (gates.h) and the request layer do the waiting.
//
// Not thread-safe by itself except QuotaBreaker (it has a mutex): an AdaptiveLimit belongs to one Gate, which lives on
// one thread. The free functions are pure and thread-safe.
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace studio {
class Clock;
}

namespace studio::provider {

enum class LimitKind { Rate, Quota, Cap };
QString limitKindName(LimitKind k);                      // "rate" | "quota" | "cap"
std::optional<LimitKind> limitKindFromName(QStringView s);

struct LimitSignal {
    LimitKind kind = LimitKind::Rate;
    /// how long the provider asked us to wait (Retry-After and friends), when it said
    std::optional<qint64> retryAfterMs;
    /// the upstream provider that answered (OpenRouter error.metadata.provider_name), when it said (<= 60 chars)
    QString provider;
    /// the provider's own words, one line (<= 300 chars)
    QString message;
};

/// The verdict the request layer already made, as it travels on the error it reports (TS ProviderLimitError.info /
/// limitInfoOf). The model-health layer trusts it instead of reading the words again.
struct LimitVerdict {
    LimitKind kind = LimitKind::Rate;
    std::optional<qint64> retryAfterMs;                  // only when > 0
    QString model;
    bool remembered = false;
};

/// Raw response headers as QNetworkReply::rawHeaderPairs() gives them. Lookups are case-insensitive; a header sent
/// more than once reads as its values joined by ", " (what fetch's Headers.get() answers).
using HeaderList = QList<QPair<QByteArray, QByteArray>>;

constexpr qint64 kMaxHintMs = 24LL * 3600000;            // MAX_HINT_MS: no hint is believed beyond a day
constexpr qint64 kMaxInlineWaitMs = 60000;               // MAX_INLINE_WAIT_MS: the longest wait inside one request
constexpr qint64 kDefaultQuotaTtlMs = 30LL * 60000;      // DEFAULT_QUOTA_TTL_MS

/// The ECMAScript sources of the TS expressions, verbatim (tests compare them with the checkout).
namespace rx {
extern const char16_t* const kQuota;                    // QUOTA_RX, flags "i"
extern const char16_t* const kCap;                      // CAP_RX, flags "i"
extern const char16_t* const kRate;                     // RATE_RX, flags "i"
extern const char16_t* const kShortWindow;              // SHORT_WINDOW_RX, flags "i"
extern const char16_t* const kDailyWords;               // DAILY_WORDS_RX, flags "i"
extern const char16_t* const kRetryText;                // the "try again in 20s" reader in retryAfterMs, flags "i"
extern const char16_t* const kDurationPart;             // parseDurationMs's matchAll expression, flags "g"
/// QUOTA_RX / CAP_RX / RATE_RX compiled (data::regex; cached, thread-safe).
const QRegularExpression& quota();
const QRegularExpression& cap();
const QRegularExpression& rate();
/// RegExp.prototype.test with the JS meaning (a partial, unanchored match).
bool test(const QRegularExpression& re, QStringView text);
} // namespace rx

/// "12m3s", "1.5s", "250ms", "2 minutes", "1h2m", a bare number (seconds) → ms; nullopt when it is none of these.
std::optional<qint64> parseDurationMs(QStringView text);

/// How long the provider asked us to wait: retry-after-ms, Retry-After (seconds or an HTTP date), x-ratelimit-reset*,
/// the same inside OpenRouter's error.metadata.headers, or "try again in 20s" in `text`. `nowMs` = wall clock (epoch
/// ms) for dates and epoch resets.
std::optional<qint64> retryAfterMs(const HeaderList& headers, const QJsonValue& body, QStringView text, qint64 nowMs);

/// What kind of "no" is this? nullopt = not a limit at all. A 429 is always one; 400/402/403/500/502/503/504 only
/// when the words say so (OpenRouter wraps an upstream provider's rate limit as a 502 "Provider returned error" with
/// the real text in metadata.raw); a plain 402 stays a payment error.
std::optional<LimitSignal> classifyLimit(int status, const HeaderList& headers, const QJsonValue& body,
                                         QStringView text, qint64 nowMs);

/// The pause before retry number `attempt` (0 = the first retry): the hint when given (>= 0), else 1 s, 2 s, 4 s … (up
/// to 64 s), plus jitter rand() * max(250, base / 4). `rand` returns [0, 1); default QRandomGenerator::global().
qint64 retryDelayMs(int attempt, std::optional<qint64> hintMs = std::nullopt,
                    const std::function<double()>& rand = {});

/// Status of one AIMD controller (TS AdaptiveStatus). `lastSignalAtMs` is wall time.
struct AdaptiveStatus {
    QString name;
    int configured = 1;
    int effective = 1;
    bool limited = false;
    bool paused = false;
    qint64 pausedForMs = 0;
    int cuts = 0;
    int rateAnswers = 0;
    std::optional<LimitSignal> lastSignal;
    qint64 lastSignalAtMs = 0;
    QJsonObject toJson() const;
};

struct AdaptiveTuning {
    /// after a cut, further rate answers within this time are the same episode (default: the pause, at least 1.5 s)
    std::optional<qint64> holdMs;
    /// a lowered limit gets one slot back after this long without a rate answer (default 20 s)
    std::optional<qint64> recoverMs;
    /// the longest the gate pauses for one Retry-After (default kMaxInlineWaitMs)
    std::optional<qint64> maxPauseMs;
};

class AdaptiveLimit {
public:
    /// `max` is the configured limit, read on every use (a change takes effect at once). `floor` >= 1.
    AdaptiveLimit(QString name, std::function<int()> max, Clock& clock, int floor = 1, AdaptiveTuning tuning = {});

    /// The limit right now (creeps back by time on the way, as the TS getter does).
    int limit();
    bool paused() const;
    /// ms until the gate may send again (0 = now)
    qint64 pausedForMs() const;
    /// while lowered: ms until the next slot comes back by time alone; nullopt at the configured limit
    std::optional<qint64> nextStepMs() const;

    struct LimitResult { bool cut = false; qint64 pauseMs = 0; };
    /// A request got a limit answer. Only Rate is congestion; Quota and Cap change nothing but lastSignal.
    LimitResult onLimit(const LimitSignal& sig);
    /// A request came back fine: a full window of them gives one slot back.
    void onSuccess();

    /// Changes the recovery timing (an unset member = the default), as TS tune().
    void tune(const AdaptiveTuning& t) { m_tuning = t; }
    /// Forgets the slow-down (the configured limit changed, or "try again now").
    void reset();
    void resetStats();
    AdaptiveStatus status();
    const QString& name() const { return m_name; }

private:
    qint64 now() const;
    int configuredMax() const;
    void creep();

    QString m_name;
    std::function<int()> m_max;
    Clock& m_clock;
    int m_floor;
    AdaptiveTuning m_tuning;
    std::optional<int> m_cur;                            // nullopt = at the configured limit
    qint64 m_pausedUntil = 0;
    qint64 m_holdUntil;                                  // "-Infinity" at start
    qint64 m_changedAt = 0;
    int m_wins = 0;
    int m_cuts = 0;
    int m_rateAnswers = 0;
    std::optional<LimitSignal> m_lastSignal;
    qint64 m_lastSignalAtMs = 0;
};

/// "this model's quota is used up until …": calls for it fail at once, without a request, until the time passes.
/// Bounded (oldest entry dropped first, insertion order as a JS Map). Thread-safe.
class QuotaBreaker {
public:
    struct Entry {
        QString key;
        LimitKind kind = LimitKind::Quota;
        qint64 untilSteadyMs = 0;
        qint64 untilMs = 0;                              // wall time, for display
        QString message;
        QString provider;
        QJsonObject toJson() const;
    };
    explicit QuotaBreaker(Clock& clock, int max = 200);
    /// Remembers `key` for max(200, ttlMs).
    void trip(const QString& key, const LimitSignal& sig, qint64 ttlMs);
    /// The entry while it holds; an expired one is dropped on the way.
    std::optional<Entry> check(const QString& key);
    void clear(const QString& key = {});
    std::vector<Entry> list() const;
    int size() const;

private:
    Clock& m_clock;
    int m_max;
    mutable std::mutex m_mutex;
    std::vector<Entry> m_entries;                        // insertion order
};

} // namespace studio::provider
