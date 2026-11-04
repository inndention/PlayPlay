#ifndef PUBRAILS_H
#define PUBRAILS_H

// pubrails — the publishing lane's hard rails for PlayPlay's Publisher (docs/plans/starmedia-studio.md ST-30; port of the
// StarMedia station pub lane: core/rules.ts PUB_CONST, store.ts approvalCurrent, publish.ts persist-before-send and
// classifyError, lifecycle.ts reconcile). Pure: no store, no network, no clock (callers pass nowMs). QtCore only, so
// test_pubrails links just this file.
//
// The rails, in the order Publisher applies them:
//  1. approval bound to {rev, bodySha256}: an approval covers exactly the content that was approved; any edit, or a
//     different media file, makes it stale and the item goes back to a person;
//  2. per-hour cap, per-day cap and minimum gap per channel (Facebook: code constants 4/h, 15/day, 10 min; config may
//     only tighten them; the website has none unless configured);
//  3. the channel is marked 'publishing' (and persisted) BEFORE the network call;
//  4. an outcome nobody can be sure of (no answer, 5xx, a transient Graph error, a 2xx without an id, a restart during
//     the call) is 'unknown': the item goes to 'reconcile', which LOOKS at the platform; it is never sent again blind;
//  5. only a definitive refusal is retried automatically, with backoff (2, 10 min), at most 3 attempts.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace pubrails {

// Hard rails: code constants. publisher.config.rails may tighten them, never loosen them.
constexpr int kFbPerHour = 4;
constexpr int kFbPerDay = 15;
constexpr int kFbGapMin = 10;
constexpr int kMaxAttempts = 3;
constexpr int kBackoffMin[3] = {2, 10, 30};
constexpr int kReconcileGiveUpMin = 30;          // not on the platform this long after the attempt: failed, retry is a person's click
constexpr int kReconcileEveryMs = 120000;        // automatic re-check interval per item
constexpr int kReconcileSlackMin = 10;           // look this far before the attempt started

struct Check { QString name; bool pass = true; QString detail; };
struct Limits { int perHour = -1; int perDay = -1; int minGapMin = 0; };   // -1 = no cap
struct Stats { int hour = 0; int day = 0; qint64 lastMs = 0; QList<qint64> times; };

// ---- approval bound to {rev, bodySha256} ---------------------------------------------------------------------------
QString normalizeText(const QString& s);         // lower case, whitespace collapsed, trimmed
QString textSha256(const QString& s);            // hex sha256 of normalizeText(s): what reconcile compares with the platform
/// Everything a reader or a channel would see (kind, title, body, caption, excerpt, links, media, channels, tags, ai),
/// none of the bookkeeping. `mediaFingerprint` identifies the media file (name + size) so a re-rendered file is new content.
QString contentSha256(const QJsonObject& item, const QString& mediaFingerprint = QString());
QJsonObject bindApproval(const QJsonObject& item, const QString& by, qint64 nowMs, const QString& mediaFingerprint = QString());
/// Why the item's approval does not cover it (empty when it does).
QString approvalProblem(const QJsonObject& item, const QString& mediaFingerprint = QString());

// ---- rate rails ----------------------------------------------------------------------------------------------------
Limits limitsFor(const QString& channel, const QJsonObject& railsConfig);
/// Posts per channel from the queue: published ones by postedMs, and in-flight / unknown ones by attemptStartedMs
/// (they may have gone out, so they use up the budget too).
Stats statsFor(const QJsonArray& items, const QString& channel, qint64 nowMs, qint64 dayStartMs);
QList<Check> rateChecks(const QString& channel, const Stats& s, const Limits& l, qint64 nowMs, qint64 dayStartMs);
/// When the next post may go (nowMs when it may go now; -1 when a configured cap of 0 closes the channel).
qint64 nextAllowedMs(const Stats& s, const Limits& l, qint64 nowMs, qint64 dayStartMs);
qint64 localDayStartMs(qint64 nowMs);
qint64 nextDayStartMs(qint64 dayStartMs);

// ---- send outcome --------------------------------------------------------------------------------------------------
enum class Outcome { Published, Failed, Unknown };
struct SendResult {
    Outcome state = Outcome::Failed;
    QString errorClass;                          // not_sent | auth | permission | rate | content | blocked | rejected | no_answer | server_error | transient | duplicate | bad_answer
    QString error;
    bool retryable = false;                      // a definitive refusal that may be retried automatically
};
/// True only when the request certainly never left (the connection was never made, or the file could not be read).
bool definitelyNotSent(int http, const QString& netError);
SendResult notSent(const QString& why);
SendResult classifyFacebook(int http, const QJsonObject& json, const QString& errorText, const QString& netError, const QString& remoteId);
SendResult classifyWebsite(int http, const QJsonObject& json, const QString& errorText, const QString& netError, const QString& remoteId);
QString stateName(Outcome o);                    // published | failed | unknown (the perChannel state)
/// When a failed channel may be tried again: lastTryMs + backoff(attempts); 0 when the attempts are used up.
qint64 retryAtMs(int attempts, qint64 lastTryMs);
bool mayAutoRetry(const QJsonObject& channelState);

// ---- reconcile -----------------------------------------------------------------------------------------------------
/// A Page post (Graph /posts or /videos `data`) whose message or description matches, created at or after sinceMs:
/// {id, permalink, createdMs}, or empty.
QJsonObject findFacebookPost(const QJsonArray& data, const QString& msgSha256, qint64 sinceMs);
/// A WordPress post (REST /wp/v2/posts rows) with the same title, dated at or after sinceMs: {id, permalink, status}, or empty.
QJsonObject findWebsitePost(const QJsonArray& rows, const QString& title, qint64 sinceMs);
enum class Verdict { Published, NotFound, StillUnknown };
/// "I could not look" is never a green light: unchecked stays unknown; checked and absent for kReconcileGiveUpMin is NotFound.
Verdict reconcileVerdict(bool checked, bool found, qint64 ageMs);

// ---- the item ladder around the rails ------------------------------------------------------------------------------
/// After a restart: a channel still marked 'publishing' (or every unsettled channel of a legacy item-level 'publishing'
/// item) becomes 'unknown' and the item goes to 'reconcile'. Sets *changed.
QJsonObject recoverInterrupted(const QJsonObject& item, qint64 nowMs, bool* changed = nullptr);
/// The item status once its channels settle: publishing | reconcile | published | approved (retry at *nextTryMs) | failed.
QString aggregateStatus(const QJsonObject& item, qint64* nextTryMs = nullptr, QStringList* errors = nullptr);
/// Why an operator action is refused while a send runs or its outcome is unknown (empty when allowed).
QString actionBlock(const QString& status, const QString& action);
/// Channels whose state is 'publishing' or 'unknown' (nothing may be sent to them).
QStringList busyChannels(const QJsonObject& item);

} // namespace pubrails

#endif // PUBRAILS_H
