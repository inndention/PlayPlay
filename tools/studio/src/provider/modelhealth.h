#pragma once
// Model health (ST-06): what a provider error means and what to do about it. A port of starmedia src/model-health.ts
// (175 lines): the one place that knows the difference between "try again in a minute", "this model is out of quota
// until tomorrow", "the server is down", "the conversation is too long for this model" and "the answer was not JSON".
//
// A model that ran out of quota (or hit a hard rate limit) is switched away from at once and remembered as cold for a
// while, so the next steps start with the next model instead of paying for another failing call. The memory is per
// process (every project and agent shares one ModelHealth) and can be saved (snapshot) and loaded back with its expiry.
// The cooldown only ORDERS the models of a step (cold ones go last): it never blocks one, so a stale entry costs at
// most one failing call. (Blocking is the QuotaBreaker's job, gates.h; the two are cleared together.)
//
// ONE reading of a provider answer: classifyError() trusts the verdict the request layer already made (LimitVerdict,
// TS limitInfoOf) and reads the words itself only for an error that never went through it, with classifyLimit() and
// the same QUOTA / RATE / CAP words (backpressure.h). So for one answer the gate, the model's rest and the choice of
// the next model always agree:
//   rate   the gate was halved and the call retried; if still refused the model rests a minute or two
//   quota  nothing retried, no gate touched, the model rests for hours and the next model takes over
//   cap    StarMedia's own spend cap: no gate, no rest, no other model; the user is told
//
// Time is the wall clock (Clock::nowMs()): rests are saved with their `until` and loaded back in another process.
// Thread-safe (one mutex; listeners run outside it, on the calling thread).
#include "provider/backpressure.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace studio {
class Clock;
}

namespace studio::provider {

enum class ErrorKind { Quota, Rate, Server, Gone, Context, Json, Auth, Cap, Cancelled, Other };
QString errorKindName(ErrorKind k);                      // "quota" | "rate" | ... | "other"
std::optional<ErrorKind> errorKindFromName(QStringView s);

/// What classifyError() reads: the C++ shape of the TS OpenRouterError / ProviderLimitError / Error.
struct ProviderError {
    QString name;                        // "AbortError", "ProviderLimitError", "OpenRouterError", "Error" ...
    std::optional<int> status;           // the HTTP status, when there was an answer
    QString message;                     // OpenRouterError messages start with the status ("429 Too many requests")
    QString providerMessage;             // the upstream provider's own text (error.metadata.raw), one line
    std::optional<LimitVerdict> verdict; // the request layer's reading of the answer (trusted as is)
};

struct Classified {
    ErrorKind kind = ErrorKind::Other;
    std::optional<int> status;
    /// switch to the next model of the step now
    bool switchModel = false;
    /// stop using this model first for this long (ms); 0 = no cooldown
    qint64 cooldownMs = 0;
    /// one line, in words a person can act on
    QString why;
    /// what the provider asked us to wait, when it said
    std::optional<qint64> retryAfterMs;
};

/// How long a model rests after each kind of failure (TS COOLDOWN_MS).
namespace cooldown {
constexpr qint64 kMinute = 60000;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kQuota = 6 * kHour;
constexpr qint64 kRate = 90000;
constexpr qint64 kServer = 45000;
constexpr qint64 kGone = 30 * kMinute;
constexpr qint64 kMax = 24 * kHour;
} // namespace cooldown

/// "try again in 12 s", "retry after 30 seconds", "in 1m30s": the request layer's reader first, then the looser one
/// for a bare "in 12 s" (TS model-health retryAfterMs).
std::optional<qint64> retryAfterFromText(QStringView text);

/// Classifies a failed model call (TS classifyError).
Classified classifyError(const ProviderError& e);

/// The ECMAScript sources and flags of the expressions classifyError() uses, verbatim from model-health.ts (tests
/// find each "/<source>/<flags>" in the checkout).
QList<QPair<QString, QString>> errorPatterns();

struct Cooling {
    qint64 until = 0;                    // wall ms
    ErrorKind kind = ErrorKind::Quota;
    QString why;
};

struct HealthEvent {
    enum Type { Cooled, Cleared };
    Type type = Cooled;
    QString model;                       // Cleared: empty = all
    Cooling cooling;                     // Cooled only
    QString to;                          // the model that took over, when known
    /// the model already rested for the same reason and the rest was only extended
    bool renewed = false;
};

class ModelHealth {
public:
    explicit ModelHealth(Clock& clock);

    qint64 now() const;
    using ListenerId = quint64;
    ListenerId onChange(std::function<void(const HealthEvent&)> fn);
    void removeListener(ListenerId id);

    /// Is the model resting now? (an expired entry is dropped on the way)
    std::optional<Cooling> coolingOf(const QString& model);
    /// Lets a model rest after a failure that asked for it (c.cooldownMs > 0). Never shortens a longer rest. Returns
    /// the entry, or nullopt when the failure does not ask for one.
    std::optional<Cooling> mark(const QString& model, const Classified& c, const QString& to = {});
    /// The models of a step in the order to try: rested ones first (in their order), then the cold ones by when they
    /// come back. Duplicates and blanks vanish.
    QStringList order(const QStringList& models);
    /// Forgets one model's rest, or all of them (empty).
    void clear(const QString& model = {});
    /// What is resting now: {model: {until, kind, why}}.
    QJsonObject snapshot();
    /// Takes saved entries back; expired, malformed and too-far-future (> 24 h) ones are dropped, and `why` is never
    /// taken from the data (the agent reads it in a note). Returns how many are resting after the load.
    int load(const QJsonValue& data);

private:
    void notify(const HealthEvent& e);       // (not "emit": a Qt keyword)
    Clock& m_clock;
    mutable std::mutex m_mutex;
    std::map<QString, Cooling> m_cold;
    std::mutex m_listenerMutex;
    std::map<ListenerId, std::function<void(const HealthEvent&)>> m_listeners;
    ListenerId m_nextListener = 0;
};

/// "in a minute" / "in about 12 minutes" / "at about 6:40 PM" / "tomorrow at about 2:10 AM" (local time).
QString backAt(qint64 untilMs, qint64 nowMs);

} // namespace studio::provider
