#include "pubrails.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>

namespace pubrails {

static QString hex256(const QByteArray& b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); }
static qint64 msOf(const QJsonValue& v) { return qint64(v.toDouble()); }

// ISO times from Graph ("2026-10-26T12:34:56+0000") and WordPress date_gmt ("2026-10-26T12:34:56", UTC without a zone).
static qint64 parseTimeMs(QString s) {
    s = s.trimmed();
    if (s.isEmpty() || s.startsWith("0000")) return 0;
    static const QRegularExpression compact("([+-]\\d{2})(\\d{2})$"), zoned("(Z|[+-]\\d{2}:\\d{2})$");
    s.replace(compact, "\\1:\\2");
    if (!zoned.match(s).hasMatch()) s += 'Z';
    const QDateTime d = QDateTime::fromString(s, Qt::ISODate);
    return d.isValid() ? d.toMSecsSinceEpoch() : 0;
}

// =====================================================================================================
// approval bound to {rev, bodySha256}
// =====================================================================================================
QString normalizeText(const QString& s) { return s.toLower().simplified(); }
QString textSha256(const QString& s) { return hex256(normalizeText(s).toUtf8()); }

QString contentSha256(const QJsonObject& item, const QString& mediaFingerprint) {
    QJsonObject c;   // QJsonObject keeps its keys sorted, so the compact JSON is a stable serialisation
    for (const char* k : {"kind", "title", "body", "caption", "excerpt", "linkUrl", "canonicalUrl", "mediaId", "category"}) c[k] = item.value(k).toString();
    QStringList ch; for (const QJsonValue& v : item.value("channels").toArray()) ch << v.toString();
    ch.sort(); ch.removeDuplicates(); c["channels"] = QJsonArray::fromStringList(ch);
    c["tags"] = item.value("tags").toArray();        // tag order is content
    c["ai"] = item.value("ai").toObject();           // what the AI label says
    c["media"] = mediaFingerprint;
    return hex256(QJsonDocument(c).toJson(QJsonDocument::Compact));
}

QJsonObject bindApproval(const QJsonObject& item, const QString& by, qint64 nowMs, const QString& mediaFingerprint) {
    QJsonObject a; a["by"] = by; a["ms"] = double(nowMs); a["rev"] = item.value("revision").toInt(1); a["bodySha256"] = contentSha256(item, mediaFingerprint);
    return a;
}

QString approvalProblem(const QJsonObject& item, const QString& mediaFingerprint) {
    const QJsonObject a = item.value("approval").toObject();
    if (a.isEmpty() || a.value("bodySha256").toString().isEmpty()) return "no approval is bound to this item's content (approved before the publishing rails, or never): a person approves it again";
    const int rev = item.value("revision").toInt(1);
    if (a.value("rev").toInt(-1) != rev) return QString("the approval is for revision %1, the item is now revision %2: a person approves it again").arg(a.value("rev").toInt(-1)).arg(rev);
    if (a.value("bodySha256").toString() != contentSha256(item, mediaFingerprint)) return "the approval is stale: the item (or its media file) changed after it was approved; a person approves it again";
    return {};
}

// =====================================================================================================
// rate rails
// =====================================================================================================
Limits limitsFor(const QString& channel, const QJsonObject& railsConfig) {
    Limits l;
    if (channel == "facebook") { l.perHour = kFbPerHour; l.perDay = kFbPerDay; l.minGapMin = kFbGapMin; }
    const QJsonObject c = railsConfig.value(channel).toObject();
    auto tighten = [](int hard, const QJsonValue& v) { if (!v.isDouble()) return hard; const int x = qBound(0, v.toInt(), 100000); return hard < 0 ? x : qMin(hard, x); };
    l.perHour = tighten(l.perHour, c.value("perHour"));
    l.perDay = tighten(l.perDay, c.value("perDay"));
    if (c.value("minGapMin").isDouble()) l.minGapMin = qMax(l.minGapMin, qBound(0, c.value("minGapMin").toInt(), 1440));
    return l;
}

Stats statsFor(const QJsonArray& items, const QString& channel, qint64 nowMs, qint64 dayStartMs) {
    Stats s;
    for (const QJsonValue& v : items) {
        const QJsonObject it = v.toObject();
        const QJsonObject one = it.value("perChannel").toObject().value(channel).toObject();
        const QString st = one.value("state").toString();
        qint64 t = 0;
        if (st == "published") t = msOf(one.value("postedMs")) > 0 ? msOf(one.value("postedMs")) : msOf(one.value("lastTryMs"));
        else if (st == "publishing" || st == "unknown")   // it may have gone out: it uses up the budget until reconcile says otherwise
            for (const char* k : {"attemptStartedMs", "lastTryMs", "unknownSinceMs"}) { if (msOf(one.value(k)) > 0) { t = msOf(one.value(k)); break; } }
        if (t <= 0) continue;
        s.times << t;
    }
    std::sort(s.times.begin(), s.times.end());
    for (qint64 t : s.times) { if (t > nowMs - 3600000) ++s.hour; if (t >= dayStartMs) ++s.day; s.lastMs = qMax(s.lastMs, t); }
    return s;
}

QList<Check> rateChecks(const QString& channel, const Stats& s, const Limits& l, qint64 nowMs, qint64 dayStartMs) {
    Q_UNUSED(dayStartMs);
    QList<Check> out;
    const QString hard = channel == "facebook" ? "hard cap" : "cap";
    if (l.perHour >= 0) {
        Check c{"rail:" + channel + "_hour", s.hour + 1 <= l.perHour, QString()};
        c.detail = c.pass ? QString("%1 %2 post(s) in the last hour (%3 %4)").arg(s.hour).arg(channel).arg(hard).arg(l.perHour)
                          : QString("%1 %2 post(s) in the last hour: the %3 is %4 an hour").arg(s.hour).arg(channel).arg(hard).arg(l.perHour);
        out << c;
    }
    if (l.perDay >= 0) {
        Check c{"rail:" + channel + "_day", s.day + 1 <= l.perDay, QString()};
        c.detail = c.pass ? QString("%1 %2 post(s) today (%3 %4)").arg(s.day).arg(channel).arg(hard).arg(l.perDay)
                          : QString("%1 %2 post(s) today: the %3 is %4 a day").arg(s.day).arg(channel).arg(hard).arg(l.perDay);
        out << c;
    }
    if (l.minGapMin > 0) {
        const bool ok = s.lastMs <= 0 || nowMs - s.lastMs >= qint64(l.minGapMin) * 60000;
        Check c{"rail:" + channel + "_gap", ok, QString()};
        const qint64 agoMin = s.lastMs > 0 ? qMax<qint64>(0, (nowMs - s.lastMs) / 60000) : -1;
        c.detail = ok ? (agoMin < 0 ? QString("no earlier %1 post").arg(channel) : QString("the last %1 post was %2 min ago (minimum %3)").arg(channel).arg(agoMin).arg(l.minGapMin))
                      : QString("the last %1 post was %2 min ago: the minimum gap is %3 min").arg(channel).arg(agoMin).arg(l.minGapMin);
        out << c;
    }
    return out;
}

qint64 localDayStartMs(qint64 nowMs) {
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(nowMs);
    return QDateTime(d.date(), QTime(0, 0)).toMSecsSinceEpoch();
}
qint64 nextDayStartMs(qint64 dayStartMs) {
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(dayStartMs);
    return QDateTime(d.date().addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
}

qint64 nextAllowedMs(const Stats& s, const Limits& l, qint64 nowMs, qint64 dayStartMs) {
    if (l.perHour == 0 || l.perDay == 0) return -1;
    qint64 at = nowMs;
    if (l.perHour > 0 && s.hour >= l.perHour) {
        QList<qint64> inHour; for (qint64 t : s.times) if (t > nowMs - 3600000) inHour << t;
        at = qMax(at, inHour.at(inHour.size() - l.perHour) + 3600000);   // that post has to leave the hour window first
    }
    if (l.perDay > 0 && s.day >= l.perDay) at = qMax(at, nextDayStartMs(dayStartMs));
    if (l.minGapMin > 0 && s.lastMs > 0) at = qMax(at, s.lastMs + qint64(l.minGapMin) * 60000);
    return at;
}

// =====================================================================================================
// send outcome
// =====================================================================================================
bool definitelyNotSent(int http, const QString& netError) {
    if (http != 0) return false;
    const QString e = netError.toLower();
    if (e.startsWith("cannot read") || e.startsWith("not sent")) return true;                      // the file never left this machine
    if (e.contains("connection refused") || e.contains("ssl handshake failed")) return true;       // no connection, nothing written
    if (e.contains("network unreachable") || e.contains("network is unreachable")) return true;
    static const QRegularExpression hostNotFound("host .*not found");
    return hostNotFound.match(e).hasMatch();
    // everything else at http 0 (timed out, remote host closed, reset, no answer) may have been executed: unknown
}

SendResult notSent(const QString& why) { SendResult r; r.state = Outcome::Failed; r.errorClass = "not_sent"; r.error = why; r.retryable = true; return r; }

static SendResult unknown(const QString& cls, const QString& why) { SendResult r; r.state = Outcome::Unknown; r.errorClass = cls; r.error = why; return r; }
static SendResult failed(const QString& cls, const QString& why, bool retry) { SendResult r; r.state = Outcome::Failed; r.errorClass = cls; r.error = why; r.retryable = retry; return r; }

SendResult classifyFacebook(int http, const QJsonObject& json, const QString& errorText, const QString& netError, const QString& remoteId) {
    if (http >= 200 && http < 300) {
        if (!remoteId.trimmed().isEmpty()) { SendResult r; r.state = Outcome::Published; return r; }
        return unknown("bad_answer", errorText + " (Facebook answered without a post id: it may have posted)");
    }
    if (http == 0) return definitelyNotSent(http, netError) ? failed("not_sent", errorText, true) : unknown("no_answer", errorText + " (no answer: the post may exist)");
    if (http >= 500) return unknown("server_error", errorText + " (server error: the post may exist)");
    const QJsonObject e = json.value("error").toObject();
    const int code = e.value("code").toInt();
    // Graph says "transient" (codes 1 / 2 or is_transient) on a post: it may still have been created, so it is reconciled, not retried
    if (e.value("is_transient").toBool() || code == 1 || code == 2) return unknown("transient", errorText);
    if (code == 506) return unknown("duplicate", errorText + " (Facebook reports a duplicate: the same post went out already)");
    if (code == 190 || code == 102 || code == 10 || (code >= 200 && code <= 299)) return failed(code == 190 || code == 102 ? "auth" : "permission", errorText, false);
    if (code == 4 || code == 17 || code == 32 || code == 613 || code == 80001 || http == 429) return failed("rate", errorText, true);
    if (code == 368) return failed("blocked", errorText, false);
    if (code == 100) return failed("content", errorText, false);
    if (http >= 400) return failed("rejected", errorText, true);
    return unknown("bad_answer", errorText);          // 1xx / 3xx: something answered, not a clear no
}

SendResult classifyWebsite(int http, const QJsonObject& json, const QString& errorText, const QString& netError, const QString& remoteId) {
    Q_UNUSED(json);
    if (http >= 200 && http < 300) {
        const QString id = remoteId.trimmed();
        if (!id.isEmpty() && id != "0") { SendResult r; r.state = Outcome::Published; return r; }
        return unknown("bad_answer", errorText + " (the site answered without a post id: it may have posted)");
    }
    if (http == 0) return definitelyNotSent(http, netError) ? failed("not_sent", errorText, true) : unknown("no_answer", errorText + " (no answer: the post may exist)");
    if (http >= 500) return unknown("server_error", errorText + " (server error: the post may exist)");
    if (http == 401 || http == 403) return failed("auth", errorText, false);
    if (http == 429) return failed("rate", errorText, true);
    if (http == 400 || http == 404 || http == 413 || http == 422) return failed("content", errorText, false);
    if (http >= 400) return failed("rejected", errorText, true);
    return unknown("bad_answer", errorText);
}

QString stateName(Outcome o) { return o == Outcome::Published ? "published" : o == Outcome::Unknown ? "unknown" : "failed"; }

qint64 retryAtMs(int attempts, qint64 lastTryMs) {
    if (attempts >= kMaxAttempts) return 0;
    return lastTryMs + qint64(kBackoffMin[qBound(0, attempts - 1, 2)]) * 60000;
}

bool mayAutoRetry(const QJsonObject& one) {
    return one.value("state").toString() == "failed" && one.value("retryable").toBool(false) && one.value("attempts").toInt() < kMaxAttempts;
}

// =====================================================================================================
// reconcile
// =====================================================================================================
QJsonObject findFacebookPost(const QJsonArray& data, const QString& msgSha256, qint64 sinceMs) {
    if (msgSha256.isEmpty()) return {};
    for (const QJsonValue& v : data) {
        const QJsonObject p = v.toObject();
        const bool same = (!p.value("message").toString().isEmpty() && textSha256(p.value("message").toString()) == msgSha256)
                       || (!p.value("description").toString().isEmpty() && textSha256(p.value("description").toString()) == msgSha256);
        if (!same) continue;
        const qint64 created = parseTimeMs(p.value("created_time").toString());
        if (created > 0 && created < sinceMs) continue;   // an older post with the same words is not this attempt
        const QString id = p.value("id").toString();
        if (id.isEmpty()) continue;
        QJsonObject hit; hit["id"] = id; hit["permalink"] = p.value("permalink_url").toString("https://www.facebook.com/" + id); hit["createdMs"] = double(created);
        return hit;
    }
    return {};
}

static QString plainTitle(const QJsonValue& t) {
    if (t.isString()) return t.toString();
    const QJsonObject o = t.toObject();
    if (!o.value("raw").toString().isEmpty()) return o.value("raw").toString();
    QString s = o.value("rendered").toString();         // texturized HTML: undo the common entities
    static const QList<QPair<QString, QString>> ents = {{"&#8217;", "'"}, {"&#8216;", "'"}, {"&rsquo;", "'"}, {"&lsquo;", "'"}, {"&#039;", "'"}, {"&#39;", "'"},
        {"&#8220;", "\""}, {"&#8221;", "\""}, {"&quot;", "\""}, {"&#8211;", "-"}, {"&#8212;", "-"}, {"&nbsp;", " "}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
    for (const auto& e : ents) s.replace(e.first, e.second);
    return s;
}
static QString titleKey(QString s) { s.replace(QChar(0x2019), '\'').replace(QChar(0x2018), '\'').replace(QChar(0x201C), '"').replace(QChar(0x201D), '"'); return normalizeText(s); }

QJsonObject findWebsitePost(const QJsonArray& rows, const QString& title, qint64 sinceMs) {
    const QString want = titleKey(title);
    if (want.isEmpty()) return {};
    for (const QJsonValue& v : rows) {
        const QJsonObject p = v.toObject();
        if (titleKey(plainTitle(p.value("title"))) != want) continue;
        qint64 t = parseTimeMs(p.value("date_gmt").toString());
        if (t <= 0) t = parseTimeMs(p.value("modified_gmt").toString());
        if (t <= 0 || t < sinceMs) continue;            // an undated or older post with the same title is not this attempt
        const int id = p.value("id").toInt();
        if (id <= 0) continue;
        QJsonObject hit; hit["id"] = QString::number(id); hit["permalink"] = p.value("link").toString(); hit["status"] = p.value("status").toString(); hit["createdMs"] = double(t);
        return hit;
    }
    return {};
}

Verdict reconcileVerdict(bool checked, bool found, qint64 ageMs) {
    if (checked && found) return Verdict::Published;
    if (checked && ageMs >= qint64(kReconcileGiveUpMin) * 60000) return Verdict::NotFound;
    return Verdict::StillUnknown;
}

// =====================================================================================================
// the item ladder around the rails
// =====================================================================================================
QJsonObject recoverInterrupted(const QJsonObject& item, qint64 nowMs, bool* changed) {
    QJsonObject it = item; bool any = false;
    QJsonObject pc = it.value("perChannel").toObject();
    QStringList channels; for (const QJsonValue& v : it.value("channels").toArray()) channels << v.toString();
    bool marked = false; for (const QString& c : channels) if (pc.value(c).toObject().value("state").toString() == "publishing") marked = true;
    const bool legacy = !marked && it.value("status").toString() == "publishing";   // an item-level mark only (before the rails)
    for (const QString& c : channels) {
        QJsonObject one = pc.value(c).toObject(); const QString st = one.value("state").toString();
        if (!(st == "publishing" || (legacy && st != "published" && st != "unknown"))) continue;
        one["state"] = "unknown"; one["errorClass"] = "interrupted"; one["error"] = "the Supervisor stopped while this was being sent: the outcome is unknown";
        one["unknownSinceMs"] = double(nowMs); one["attempts"] = qMax(1, one.value("attempts").toInt());
        if (msOf(one.value("attemptStartedMs")) <= 0) one["attemptStartedMs"] = double(msOf(one.value("lastTryMs")) > 0 ? msOf(one.value("lastTryMs")) : msOf(it.value("updatedMs")) > 0 ? msOf(it.value("updatedMs")) : nowMs);
        pc[c] = one; any = true;
    }
    if (any) { it["perChannel"] = pc; it["status"] = aggregateStatus(it); }
    else if (it.value("status").toString() == "publishing") { it["status"] = aggregateStatus(it); any = it.value("status").toString() != "publishing"; }
    if (changed) *changed = any;
    return any ? it : item;
}

QString aggregateStatus(const QJsonObject& item, qint64* nextTryMs, QStringList* errors) {
    const QJsonObject pc = item.value("perChannel").toObject();
    bool publishing = false, unknownOut = false, all = true, allRetry = true; qint64 next = 0;
    for (const QJsonValue& v : item.value("channels").toArray()) {
        const QString c = v.toString(); const QJsonObject one = pc.value(c).toObject(); const QString st = one.value("state").toString();
        if (st == "published") continue;
        all = false;
        if (errors) errors->append(c + ": " + one.value("error").toString(st.isEmpty() ? QString("not sent yet") : st));
        if (st == "publishing") publishing = true;
        else if (st == "unknown") unknownOut = true;
        else if (st == "failed") { if (!mayAutoRetry(one)) allRetry = false; else next = qMax(next, retryAtMs(one.value("attempts").toInt(), msOf(one.value("lastTryMs")))); }
    }
    if (nextTryMs) *nextTryMs = 0;
    if (publishing) return "publishing";
    if (unknownOut) return "reconcile";
    if (all) return "published";
    if (allRetry) { if (nextTryMs) *nextTryMs = next; return "approved"; }
    return "failed";
}

QString actionBlock(const QString& status, const QString& action) {
    if (status == "publishing") return "a send is in progress: wait for its outcome";
    if (status == "reconcile") {
        if (action == "resolve" || action == "reconcile") return {};
        return "the outcome of an earlier send is unknown: reconcile it (POST .../reconcile looks at the platform) or resolve it by hand (POST .../resolve) first; it is never sent again blind";
    }
    if (action == "resolve" || action == "reconcile") return "nothing to reconcile: no channel of this item has an unknown outcome";
    return {};
}

QStringList busyChannels(const QJsonObject& item) {
    QStringList out; const QJsonObject pc = item.value("perChannel").toObject();
    for (auto it = pc.begin(); it != pc.end(); ++it) { const QString st = it.value().toObject().value("state").toString(); if (st == "publishing" || st == "unknown") out << it.key(); }
    return out;
}

} // namespace pubrails
