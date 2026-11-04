#include "publisher.h"
#include "laneext.h"
#include "restapiserver.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QUuid>
#include <algorithm>
#include <memory>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QJsonObject errObj(int status, const QString& m) { QJsonObject e; e["error"] = m; e["status"] = status; return e; }
static QString iso(qint64 ms) { return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toUTC().toString(Qt::ISODate) : QString(); }
static QString hhmm(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms).toString("HH:mm"); }
static const QStringList kChannels = {"facebook", "website"};
static Publisher* s_instance = nullptr;

// Rails routes (ST-30) through the lane-extension registry, so supervisorcore_suites.cpp stays untouched:
//   GET  /api/v1/publish/rails                 limits, per-channel counts, when the next post may go
//   POST /api/v1/publish/items/{id}/reconcile  look at the Page / site now for a channel whose outcome is unknown
//   POST /api/v1/publish/items/{id}/resolve    {channel, outcome: posted|not_posted, permalink?, remoteId?, note?}: a person's verdict
static laneext::Registrar s_railRoutes("pubrails", 40, [](const laneext::Context& ctx) {
    if (!ctx.rest) return;
    using Q = QHash<QString, QString>;
    ctx.rest->registerRoute("GET", "/api/v1/publish/rails", [](const QJsonObject&, const Q&) { return s_instance ? s_instance->rails() : errObj(503, "the publisher is not running"); });
    ctx.rest->registerAsync("POST", "/api/v1/publish/items/{id}/reconcile", [](const QJsonObject& b, const Q& q, RestApiServer::Reply reply) {
        if (!s_instance) { reply(503, errObj(503, "the publisher is not running")); return; }
        s_instance->reconcile(q.value("id"), b.value("actor").toString("operator"), reply);
    });
    ctx.rest->registerRoute("POST", "/api/v1/publish/items/{id}/resolve", [](const QJsonObject& b, const Q& q) {
        return s_instance ? s_instance->act(q.value("id"), "resolve", b, b.value("actor").toString("operator")) : errObj(503, "the publisher is not running");
    });
});

Publisher::Publisher(ScheduleStore* store, QObject* parent) : QObject(parent), m_store(store), m_http(new SuiteHttp(this)) {
    connect(&m_worker, &QTimer::timeout, this, &Publisher::tick);
    s_instance = this;
}

Publisher::~Publisher() { if (s_instance == this) s_instance = nullptr; }

Publisher* Publisher::instance() { return s_instance; }

void Publisher::start() { recoverInterrupted(); m_worker.start(30000); }

// A send that was running when the Supervisor stopped has an unknown outcome: never sent again blind, reconciled instead.
void Publisher::recoverInterrupted() {
    QJsonArray a = items(); int n = 0; const qint64 now = nowMs(); const QJsonObject cfg = config(true);
    for (int i = 0; i < a.size(); ++i) {
        bool changed = false; QJsonObject it = pubrails::recoverInterrupted(a[i].toObject(), now, &changed);
        if (!changed) continue;
        QJsonObject pc = it.value("perChannel").toObject();
        for (const QString& c : pc.keys()) {   // a legacy mark has no message hash: the text it sent is rebuilt for the Page match
            QJsonObject one = pc.value(c).toObject();
            if (one.value("state").toString() == "unknown" && one.value("msgSha256").toString().isEmpty()) { one["msgSha256"] = pubrails::textSha256(outgoingText(it, c, cfg)); pc[c] = one; }
        }
        it["perChannel"] = pc;
        if (it.value("status").toString() == "published" && !it.contains("publishedMs")) it["publishedMs"] = double(now);
        history(it, "supervisor", "interrupted", "the Supervisor stopped during a send: " + it.value("status").toString());
        logAudit(it.value("id").toString(), "supervisor", "interrupted", it.value("status").toString(), it.value("revision").toInt(1));
        a[i] = it; ++n;
    }
    if (!n) return;
    saveItems(a);
    emit alert("warn", QString("%1 publishing item(s) were being sent when the Supervisor stopped: their outcome is reconciled with the platform, never sent again blind").arg(n));
}

// =====================================================================================================
// config
// =====================================================================================================
QJsonObject Publisher::config(bool withSecrets) const {
    QJsonObject c = m_store->settingJson("publisher.config");
    auto def = [&c](const char* k, const QJsonValue& v) { if (!c.contains(k)) c[k] = v; };
    def("mode", "review"); def("recipes", QJsonArray{"weather_am", "weather_pm", "weather_week", "weather", "headlines"});
    def("tagline", "Watch StarNews TV — getstarnews.com"); def("maxPerDay", 12); def("aiLabel", "Made with AI assistance.");
    def("graphBase", "https://graph.facebook.com/v21.0"); def("graphVideoBase", "https://graph-video.facebook.com/v21.0");
    def("killed", QJsonObject{{"all", false}, {"facebook", false}, {"site", false}});
    def("rails", QJsonObject());   // {facebook|website: {perHour, perDay, minGapMin}}: tightens the hard rails, never loosens them
    QJsonObject fb = c.value("facebook").toObject(), web = c.value("website").toObject();
    fb["hasToken"] = !fb.value("token").toString().isEmpty(); web["hasKey"] = !web.value("key").toString().isEmpty();
    if (!web.contains("autoPublish")) web["autoPublish"] = false;
    if (!withSecrets) { fb.remove("token"); web.remove("key"); }
    c["facebook"] = fb; c["website"] = web;
    return c;
}

QJsonObject Publisher::setConfig(const QJsonObject& patch) {
    QJsonObject c = m_store->settingJson("publisher.config");
    for (const char* k : {"mode", "recipes", "tagline", "maxPerDay", "aiLabel", "graphBase", "graphVideoBase"}) if (patch.contains(k)) c[k] = patch.value(k);
    if (!QStringList{"off", "review", "auto"}.contains(c.value("mode").toString("review"))) c["mode"] = "review";
    c["maxPerDay"] = qBound(0, c.value("maxPerDay").toInt(12), 500);
    if (patch.value("rails").isObject()) {   // only numbers are kept; pubrails::limitsFor clamps them under the hard rails
        QJsonObject rails;
        for (const char* ch : {"facebook", "website"}) {
            const QJsonObject in = patch.value("rails").toObject().value(ch).toObject(); QJsonObject out;
            for (const char* k : {"perHour", "perDay", "minGapMin"}) if (in.value(k).isDouble()) out[k] = qBound(0, in.value(k).toInt(), 100000);
            if (!out.isEmpty()) rails[ch] = out;
        }
        c["rails"] = rails;
    }
    for (const char* sec : {"facebook", "website"}) {
        if (!patch.contains(sec)) continue;
        QJsonObject cur = c.value(sec).toObject(); const QJsonObject in = patch.value(sec).toObject();
        for (auto it = in.begin(); it != in.end(); ++it) {
            if ((it.key() == "token" || it.key() == "key") && it.value().toString().isEmpty()) continue;   // write-only: blank keeps
            if (it.key() == "hasToken" || it.key() == "hasKey") continue;
            cur[it.key()] = it.value();
        }
        c[sec] = cur;
    }
    m_store->setSettingJson("publisher.config", c);
    notify();
    return config();
}

bool Publisher::killed(const QString& channel) const {
    const QJsonObject k = config().value("killed").toObject();
    return k.value("all").toBool() || (channel == "facebook" && k.value("facebook").toBool()) || (channel == "website" && k.value("site").toBool());
}

QJsonObject Publisher::status() const {
    const QJsonObject c = config(true);
    QJsonObject counts; int postedToday = 0; const QString today = QDate::currentDate().toString(Qt::ISODate);
    QList<QJsonObject> next;
    for (const QJsonValue& v : items()) {
        const QJsonObject it = v.toObject(); const QString s = it.value("status").toString(); counts[s] = counts.value(s).toInt() + 1;
        if (s == "published" && QDateTime::fromMSecsSinceEpoch(qint64(it.value("publishedMs").toDouble())).date().toString(Qt::ISODate) == today) ++postedToday;
        if (s == "scheduled") next << it;
    }
    std::sort(next.begin(), next.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("scheduleAtMs").toDouble() < b.value("scheduleAtMs").toDouble(); });
    QJsonArray nx; for (int i = 0; i < next.size() && i < 5; ++i) { QJsonObject s; s["id"] = next[i].value("id"); s["title"] = next[i].value("title"); s["scheduleAtMs"] = next[i].value("scheduleAtMs"); s["channels"] = next[i].value("channels"); nx.append(s); }
    const QJsonObject fb = c.value("facebook").toObject(), web = c.value("website").toObject();
    QJsonObject configured; configured["facebook"] = !fb.value("pageId").toString().isEmpty() && !fb.value("token").toString().isEmpty();
    configured["website"] = !web.value("url").toString().isEmpty() && !web.value("user").toString().isEmpty() && !web.value("key").toString().isEmpty();
    QJsonArray warnings;
    const QJsonObject k = c.value("killed").toObject();
    if (k.value("all").toBool()) warnings.append("kill switch engaged: nothing is sent (" + c.value("killReason").toString() + ")");
    else { if (k.value("facebook").toBool()) warnings.append("Facebook sending stopped"); if (k.value("site").toBool()) warnings.append("website sending stopped"); }
    if (c.value("mode").toString() == "auto" && !configured.value("facebook").toBool()) warnings.append("auto mode, but no Facebook Page is configured: approved posts wait");
    if (web.value("autoPublish").toBool()) warnings.append("website posts go live directly (autoPublish on)");
    if (counts.value("reconcile").toInt() > 0) warnings.append(QString("%1 item(s) wait for reconcile: a send's outcome is unknown; the Page / site is checked, nothing is sent again blind").arg(counts.value("reconcile").toInt()));
    const QJsonObject r = rails();
    for (const QString& ch : kChannels) { const QJsonObject one = r.value(ch).toObject(); if (!one.value("open").toBool(true)) warnings.append(ch + ": " + one.value("why").toString()); }
    QJsonObject o; o["config"] = config(); o["mode"] = c.value("mode"); o["killed"] = k; o["killReason"] = c.value("killReason"); o["counts"] = counts;
    o["configured"] = configured; o["next"] = nx; o["warnings"] = warnings; o["postedToday"] = postedToday; o["page"] = m_page; o["rails"] = r;
    return o;
}

void Publisher::verify(Reply reply) {
    const QJsonObject fb = config(true).value("facebook").toObject();
    if (fb.value("pageId").toString().isEmpty() || fb.value("token").toString().isEmpty()) { reply(200, QJsonObject{{"ok", false}, {"error", "page id and token must both be set"}}); return; }
    QUrlQuery q; q.addQueryItem("fields", "name,followers_count"); q.addQueryItem("access_token", fb.value("token").toString());
    m_http->json("GET", config().value("graphBase").toString() + "/" + fb.value("pageId").toString() + "?" + q.toString(QUrl::FullyEncoded), {}, {}, 15000,
                 [this, reply](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        if (http != 200) { reply(200, QJsonObject{{"ok", false}, {"error", SuiteHttp::errorText(http, j, raw, e)}}); return; }
        m_page = QJsonObject{{"name", j.value("name")}, {"followers", j.value("followers_count")}};
        reply(200, QJsonObject{{"ok", true}, {"name", j.value("name")}, {"followers", j.value("followers_count")}});
    });
}

// =====================================================================================================
// items
// =====================================================================================================
QJsonArray Publisher::items() const { return m_store->settingJson("publisher.items").value("items").toArray(); }
void Publisher::saveItems(const QJsonArray& a) {
    QJsonArray keep = a;
    while (keep.size() > 500) {   // drop the oldest finished item first
        int drop = 0; for (int i = 0; i < keep.size(); ++i) if (suites::statusInGroup(keep[i].toObject().value("status").toString(), "done")) { drop = i; break; }
        keep.removeAt(drop);
    }
    QJsonObject o; o["items"] = keep; m_store->setSettingJson("publisher.items", o);
}
int Publisher::indexOf(const QJsonArray& a, const QString& id) const { for (int i = 0; i < a.size(); ++i) if (a[i].toObject().value("id").toString() == id) return i; return -1; }

// An item carries "status"; with a top-level "error" too, RestApiServer reads the item as an error reply (HTTP 400). The
// item-level failure text is "lastError" (per-channel errors stay perChannel.X.error); older stored items are renamed on the way.
static QJsonObject restSafe(QJsonObject it) {
    if (it.contains("error")) { if (!it.contains("lastError")) it["lastError"] = it.value("error"); it.remove("error"); }
    return it;
}

QJsonObject Publisher::putItem(const QJsonObject& item) {
    QJsonArray a = items(); const int i = indexOf(a, item.value("id").toString());
    QJsonObject it = restSafe(item); it["updatedMs"] = double(nowMs());
    if (i >= 0) a[i] = it; else a.append(it);
    saveItems(a); notify(it); return it;
}

// 'reconcile' (ST-30) is not in suitelogic's groups: it needs attention and is still open.
bool Publisher::inGroup(const QString& status, const QString& group) const {
    if (status == "reconcile" && (group == "open" || group == "needs_attention")) return true;
    return suites::statusInGroup(status, group);
}

QJsonObject Publisher::item(const QString& id, bool* found) const {
    const QJsonArray a = items(); const int i = indexOf(a, id); if (found) *found = i >= 0; return i >= 0 ? restSafe(a[i].toObject()) : QJsonObject();
}

QJsonArray Publisher::queue(const QString& status, const QString& kind, const QString& source, const QString& channel, int limit) const {
    const QJsonArray a = items(); QJsonArray out;
    for (int i = a.size() - 1; i >= 0 && out.size() < limit; --i) {
        const QJsonObject it = a[i].toObject();
        if (!inGroup(it.value("status").toString(), status)) continue;
        if (!kind.isEmpty() && it.value("kind").toString() != kind) continue;
        if (!source.isEmpty() && it.value("source").toString() != source) continue;
        if (!channel.isEmpty() && !it.value("channels").toArray().contains(channel)) continue;
        out.append(restSafe(it));
    }
    return out;
}

void Publisher::history(QJsonObject& item, const QString& actor, const QString& action, const QString& note) const {
    QJsonArray h = item.value("history").toArray(); QJsonObject e; e["ms"] = double(nowMs()); e["actor"] = actor; e["action"] = action; e["revision"] = item.value("revision").toInt(1);
    if (!note.isEmpty()) e["note"] = note;
    h.append(e); while (h.size() > 60) h.removeFirst(); item["history"] = h;
}

void Publisher::logAudit(const QString& id, const QString& actor, const QString& action, const QString& note, int revision) {
    QJsonObject all = m_store->settingJson("publisher.audit"); QJsonArray a = all.value("entries").toArray();
    QJsonObject e; e["ms"] = double(nowMs()); e["item"] = id; e["actor"] = actor; e["action"] = action; e["revision"] = revision; if (!note.isEmpty()) e["note"] = note.left(300);
    a.append(e); while (a.size() > 500) a.removeFirst(); all["entries"] = a; m_store->setSettingJson("publisher.audit", all);
}

QJsonArray Publisher::audit(int limit) const {
    const QJsonArray a = m_store->settingJson("publisher.audit").value("entries").toArray(); QJsonArray out;
    for (int i = a.size() - 1; i >= 0 && out.size() < limit; --i) out.append(a[i]);
    return out;
}

QString Publisher::mediaBlock(const QString& mediaId) const {
    if (mediaId.isEmpty()) return "this video item has no media";
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f);
    if (!f) return "media asset not found";
    if (m.archived) return "media is archived (not yet published to rotation)";
    if (m.extra.value("status").toString("approved") != "approved") return "media is awaiting editorial approval";
    if (!QFileInfo::exists(m.path)) return "media file missing from disk";
    return {};
}

void Publisher::notify(const QJsonObject& item) {
    QJsonObject o; if (!item.isEmpty()) o["item"] = item;
    QJsonObject counts; for (const QJsonValue& v : items()) { const QString s = v.toObject().value("status").toString(); counts[s] = counts.value(s).toInt() + 1; }
    o["counts"] = counts; o["killed"] = config().value("killed"); emit event("publish", o);
}

QJsonObject Publisher::draft(const QJsonObject& body, const QString& actor) {
    QJsonObject it; bool revise = false;
    const QString id = body.value("id").toString();
    if (!id.isEmpty()) {
        bool f = false; it = item(id, &f);
        if (!f) return errObj(404, "unknown item");
        if (QStringList{"published", "publishing", "retracted", "corrected"}.contains(it.value("status").toString())) return errObj(409, "a published item is never edited: issue a correction");
        if (it.value("status").toString() == "reconcile") return errObj(409, pubrails::actionBlock("reconcile", "revise"));   // the content an unknown send carried stays as it was
        revise = true;
    }
    QJsonObject src = body;
    if (!body.value("fromMedia").toString().isEmpty()) {   // build from a finished spot in the library
        bool f = false; const MediaRecord m = m_store->media(body.value("fromMedia").toString(), &f);
        if (!f) return errObj(404, "media not found");
        const QJsonObject ag = m.extra.value("autogen").toObject(); const QJsonValue sc = ag.value("script");
        const QString narr = sc.isObject() ? sc.toObject().value("narration").toString() : sc.toString();
        if (!src.contains("kind")) src["kind"] = "video";
        src["mediaId"] = m.id; if (!src.contains("title")) src["title"] = ag.value("headline").toString(m.title);
        if (!src.contains("caption")) src["caption"] = suites::socialBlurb(src.value("title").toString(), narr, config().value("tagline").toString());
        if (!src.contains("source")) src["source"] = ag.isEmpty() ? "manual" : "spot_job";
        if (!src.contains("ai")) src["ai"] = QJsonObject{{"text", ag.value("writer").toString() == "llm"}, {"voice", !ag.isEmpty()}};
    }
    static const QStringList fields = {"kind", "source", "title", "body", "caption", "linkUrl", "mediaId", "channels", "sourceLinks", "tags", "ai", "category", "excerpt", "canonicalUrl", "status"};
    for (const QString& k : fields) if (src.contains(k)) it[k] = src.value(k);
    if (src.contains("link_url")) it["linkUrl"] = src.value("link_url");
    if (src.contains("source_links")) it["sourceLinks"] = src.value("source_links");
    if (src.contains("schedule_at") || src.contains("scheduleAt")) it["wantedAt"] = src.value("scheduleAt").toString(src.value("schedule_at").toString());
    if (it.value("kind").toString().isEmpty()) it["kind"] = it.value("mediaId").toString().isEmpty() ? (it.value("linkUrl").toString().isEmpty() ? "text" : "link") : "video";
    if (!QStringList{"article", "spot", "video", "slate", "text", "link"}.contains(it.value("kind").toString())) return errObj(400, "kind must be article, spot, video, slate, text or link");
    if (it.value("source").toString().isEmpty()) it["source"] = "manual";
    QJsonArray ch; for (const QJsonValue& v : it.value("channels").toArray()) { QString c = v.toString(); if (c == "site_post" || c == "site_page") c = "website"; if (kChannels.contains(c) && !ch.contains(c)) ch.append(c); }
    if (ch.isEmpty()) ch.append(it.value("kind").toString() == "article" ? "website" : "facebook");
    it["channels"] = ch;
    const QString kind = it.value("kind").toString();
    if ((kind == "video" || kind == "spot") && it.value("mediaId").toString().isEmpty()) return errObj(400, "a video item needs mediaId");
    if (kind == "link" && it.value("linkUrl").toString().isEmpty()) return errObj(400, "a link item needs linkUrl");
    if (kind != "video" && kind != "spot" && kind != "link" && it.value("body").toString().trimmed().isEmpty() && it.value("caption").toString().trimmed().isEmpty() && it.value("title").toString().trimmed().isEmpty()) return errObj(400, "nothing to publish");
    if (it.value("source").toString() == "agent_draft" && !revise) {
        int waiting = 0; for (const QJsonValue& v : items()) { const QJsonObject x = v.toObject(); if (x.value("source").toString() == "agent_draft" && suites::statusInGroup(x.value("status").toString(), "waiting")) ++waiting; }
        if (waiting >= 50) return errObj(429, "50 agent drafts are already waiting");
    }
    const QJsonObject cls = suites::classifyDraft(it, config().value("mode").toString());
    QJsonObject ai = it.value("ai").toObject(); if (cls.value("aiText").toBool()) ai["text"] = true; it["ai"] = ai;
    it["status"] = cls.value("status"); it["tier"] = cls.value("tier"); it["why"] = cls.value("why");
    if (it.value("status").toString() == "approved" && (kind == "video" || kind == "spot")) { const QString blk = mediaBlock(it.value("mediaId").toString()); if (!blk.isEmpty()) { it["status"] = "pending_approval"; it["why"] = blk; } }
    if (revise) { it["revision"] = it.value("revision").toInt(1) + 1; it.remove("approvedBy"); it.remove("approvedMs"); it.remove("approval"); }
    else { it["id"] = "pub_" + QUuid::createUuid().toString(QUuid::Id128).left(10); it["revision"] = 1; it["createdMs"] = double(nowMs()); it["perChannel"] = QJsonObject(); }
    if (it.value("status").toString() == "approved") it["approval"] = pubrails::bindApproval(it, "auto:" + config().value("mode").toString(), nowMs(), mediaFingerprint(it));   // the rule approved exactly this content
    history(it, actor, revise ? "revise" : "draft", it.value("why").toString());
    logAudit(it.value("id").toString(), actor, revise ? "revise" : "draft", it.value("status").toString(), it.value("revision").toInt());
    return putItem(it);
}

QJsonObject Publisher::act(const QString& id, const QString& action, const QJsonObject& body, const QString& actor) {
    bool f = false; QJsonObject it = item(id, &f);
    if (!f) return errObj(404, "unknown item");
    const QString st = it.value("status").toString(), kind = it.value("kind").toString();
    // rails: nothing is changed while a send runs, and an unknown outcome is reconciled before anything else happens
    if (const QString blocked = pubrails::actionBlock(st, action); !blocked.isEmpty()) return errObj(409, blocked);
    if (action == "resolve") return resolve(id, body, actor);
    auto move = [&](const QString& to, const QString& note) -> QJsonObject {
        if (!suites::canTransition(st, to)) return errObj(409, QString("cannot %1 an item that is %2").arg(action, st));
        it["status"] = to; if (!note.isEmpty()) it["why"] = note;
        history(it, actor, action, note); logAudit(id, actor, action, note, it.value("revision").toInt(1));
        return putItem(it);
    };
    if (action == "approve") {
        if (kind == "video" || kind == "spot") { const QString blk = mediaBlock(it.value("mediaId").toString()); if (!blk.isEmpty()) return errObj(409, blk); }
        it["approvedBy"] = actor; it["approvedMs"] = double(nowMs());
        it["approval"] = pubrails::bindApproval(it, actor, nowMs(), mediaFingerprint(it));   // binds {revision, content sha256}: any later change needs a new approval
        const bool keepSchedule = qint64(it.value("scheduleAtMs").toDouble()) > nowMs();  // re-approving a scheduled item does not send it early
        QJsonObject r = move("approved", "approved by " + actor);
        if (keepSchedule && !r.contains("error")) { r["status"] = "scheduled"; r = putItem(r); }
        return r;
    }
    if (action == "hold") return move("held", body.value("reason").toString("held by " + actor));
    if (action == "skip") return move("skipped", body.value("reason").toString());
    if (action == "schedule") {
        qint64 at = qint64(body.value("scheduleAtMs").toDouble());
        if (!at && !body.value("at").toString().isEmpty()) at = QDateTime::fromString(body.value("at").toString(), Qt::ISODate).toMSecsSinceEpoch();
        if (at <= 0) return errObj(400, "scheduleAtMs (or an ISO 'at') required");
        if (st != "approved" && st != "scheduled") return errObj(409, "only an approved item can be scheduled");
        it["scheduleAtMs"] = double(at); it["status"] = "scheduled"; history(it, actor, "schedule", iso(at)); logAudit(id, actor, "schedule", iso(at), it.value("revision").toInt(1));
        return putItem(it);
    }
    if (action == "retry") {
        if (st != "failed") return errObj(409, "only a failed item can be retried");
        if (!pubrails::busyChannels(it).isEmpty()) return errObj(409, "a channel's outcome is unknown: reconcile it first, it is never sent again blind");
        QJsonObject pc = it.value("perChannel").toObject();
        for (auto i2 = pc.begin(); i2 != pc.end(); ++i2) { QJsonObject c = i2.value().toObject(); if (c.value("state").toString() != "published") { c["attempts"] = 0; c["state"] = "pending"; c.remove("retryable"); } pc[i2.key()] = c; }
        it["perChannel"] = pc; it.remove("nextTryMs"); return move("approved", "retry requested by " + actor);   // a person's click: the rails still apply on send
    }
    if (action == "retract") {
        if (st != "published" && st != "corrected") return errObj(409, "only a published item can be retracted");
        return move("retracted", "retracted: " + body.value("reason").toString() + " (taking the post down on the platform is a human action)");
    }
    if (action == "correct") {
        if (st != "published") return errObj(409, "only a published item can be corrected");
        QJsonObject n = it; for (const char* k : {"id", "perChannel", "history", "publishedMs", "permalink", "metrics", "approvedBy", "approvedMs", "scheduleAtMs"}) n.remove(k);
        for (const char* k : {"title", "body", "caption", "linkUrl"}) if (body.contains(k)) n[k] = body.value(k);
        n["corrects"] = id; n["correctionNote"] = body.value("note").toString(); n.remove("status");
        it["status"] = "corrected"; history(it, actor, "correct", body.value("note").toString()); putItem(it);
        QJsonObject created = draft(n, actor);
        if (!created.contains("error")) { created["corrects"] = id; created["correctionNote"] = body.value("note").toString(); created = putItem(created); }   // draft() copies only editable fields
        logAudit(id, actor, "correct", "new item " + created.value("id").toString(), it.value("revision").toInt(1));
        emit alert("info", "correction issued for " + it.value("title").toString().left(60));
        return created;
    }
    if (action == "delete") {
        if (QStringList{"published", "publishing", "corrected", "retracted", "reconcile"}.contains(st)) return errObj(409, "a published record is kept; retract it instead");
        QJsonArray a = items(); const int i = indexOf(a, id); if (i >= 0) a.removeAt(i); saveItems(a); logAudit(id, actor, "delete", QString(), it.value("revision").toInt(1)); notify();
        QJsonObject o; o["deleted"] = id; return o;
    }
    return errObj(400, "unknown action " + action);
}

// =====================================================================================================
// sending
// =====================================================================================================
QString Publisher::outgoingText(const QJsonObject& it, const QString& channel, const QJsonObject& cfg) const {
    const QString label = cfg.value("aiLabel").toString();
    if (channel == "website") return suites::withAiLabel(it.value("body").toString(it.value("caption").toString()), it.value("ai").toObject(), label);
    return suites::withAiLabel(it.value("caption").toString(it.value("body").toString(it.value("title").toString())), it.value("ai").toObject(), label);
}

QString Publisher::mediaFingerprint(const QJsonObject& it) const {
    const QString mid = it.value("mediaId").toString();
    if (mid.isEmpty()) return {};
    bool f = false; const MediaRecord m = m_store->media(mid, &f);
    if (!f) return "missing:" + mid;
    const QFileInfo fi(m.path);
    return QString("%1|%2").arg(fi.fileName()).arg(fi.exists() ? fi.size() : -1);
}

// Each callback answers once with the rails' reading of the outcome: published, failed (definitive: nothing was posted),
// or unknown (it may have been posted: the item goes to reconcile, never retried blind).
void Publisher::sendChannel(const QString& id, const QString& channel, std::function<void(const pubrails::SendResult&, const QJsonObject&)> cb) {
    const QJsonObject it = item(id); const QJsonObject c = config(true);
    if (channel == "facebook") {
        const QJsonObject fb = c.value("facebook").toObject(); const QString page = fb.value("pageId").toString(), token = fb.value("token").toString();
        if (page.isEmpty() || token.isEmpty()) { cb(pubrails::notSent("Facebook Page not configured (page id + token)"), {}); return; }
        const QString text = outgoingText(it, channel, c);
        const QString kind = it.value("kind").toString();
        if (kind == "video" || kind == "spot") {
            bool f = false; const MediaRecord m = m_store->media(it.value("mediaId").toString(), &f);
            if (!f || !QFileInfo(m.path).isReadable()) { cb(pubrails::notSent("the media file cannot be read"), {}); return; }
            const QList<QPair<QString, QString>> fields = {{"access_token", token}, {"title", it.value("title").toString(m.title).left(250)}, {"description", text}};
            m_http->multipart(c.value("graphVideoBase").toString() + "/" + page + "/videos", {}, fields, "source", m.path, "video/mp4", 600000,
                              [cb](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
                const QString rid = j.value("id").toString();
                const pubrails::SendResult r = pubrails::classifyFacebook(http, j, "Graph API: " + SuiteHttp::errorText(http, j, raw, e), e, rid);
                cb(r, r.state == pubrails::Outcome::Published ? QJsonObject{{"remoteId", rid}, {"permalink", "https://www.facebook.com/" + rid}} : QJsonObject());
            });
            return;
        }
        auto enc = [](const QString& v) { return QUrl::toPercentEncoding(v); };   // explicit: QUrlQuery leaves '+' and '\' to the server's form decoder
        QByteArray form = "access_token=" + enc(token) + "&message=" + enc(text); if (!it.value("linkUrl").toString().isEmpty()) form += "&link=" + enc(it.value("linkUrl").toString());
        m_http->raw("POST", c.value("graphBase").toString() + "/" + page + "/feed", {}, form, "application/x-www-form-urlencoded", 30000,
                    [cb](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            const QString rid = j.value("id").toString(); QString link = rid; link.replace('_', "/posts/");
            const pubrails::SendResult r = pubrails::classifyFacebook(http, j, "Graph API: " + SuiteHttp::errorText(http, j, raw, e), e, rid);
            cb(r, r.state == pubrails::Outcome::Published ? QJsonObject{{"remoteId", rid}, {"permalink", "https://www.facebook.com/" + link}} : QJsonObject());
        });
        return;
    }
    if (channel == "website") {
        const QJsonObject w = c.value("website").toObject();
        if (w.value("url").toString().isEmpty() || w.value("user").toString().isEmpty() || w.value("key").toString().isEmpty()) { cb(pubrails::notSent("website endpoint not configured (url / user / key)"), {}); return; }
        QJsonObject post; post["title"] = it.value("title").toString(); post["content"] = outgoingText(it, channel, c);
        post["excerpt"] = it.value("excerpt").toString(); post["status"] = w.value("autoPublish").toBool() ? "publish" : "draft";
        const QByteArray auth = "Basic " + (w.value("user").toString() + ":" + w.value("key").toString()).toUtf8().toBase64();
        QString url = w.value("url").toString(); while (url.endsWith('/')) url.chop(1);
        m_http->json("POST", url + "/wp/v2/posts", {{"Authorization", auth}}, post, 45000, [cb](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            const QString rid = j.value("id").toInt() > 0 ? QString::number(j.value("id").toInt()) : QString();
            const pubrails::SendResult r = pubrails::classifyWebsite(http, j, "website: " + SuiteHttp::errorText(http, j, raw, e), e, rid);
            cb(r, r.state == pubrails::Outcome::Published ? QJsonObject{{"remoteId", rid}, {"permalink", j.value("link").toString()}, {"remoteStatus", j.value("status").toString()}} : QJsonObject());
        });
        return;
    }
    cb(pubrails::notSent("unknown channel " + channel), {});
}

QString Publisher::railBlock(const QJsonObject& it, const QStringList& channels, int* code, bool* stale) const {
    if (code) *code = 0;
    if (stale) *stale = false;
    const QStringList busy = pubrails::busyChannels(it);
    for (const QString& c : channels) if (busy.contains(c)) { if (code) *code = 409; return c + " has a send whose outcome is unknown or still running: reconcile it first, it is never sent again blind"; }
    const QString ap = pubrails::approvalProblem(it, mediaFingerprint(it));
    if (!ap.isEmpty()) { if (code) *code = 409; if (stale) *stale = true; return ap; }
    const qint64 now = nowMs(), day = pubrails::localDayStartMs(now);
    const QJsonArray all = items(); const QJsonObject rc = config().value("rails").toObject();
    for (const QString& c : channels) {
        const pubrails::Limits l = pubrails::limitsFor(c, rc); const pubrails::Stats s = pubrails::statsFor(all, c, now, day);
        for (const pubrails::Check& k : pubrails::rateChecks(c, s, l, now, day)) {
            if (k.pass) continue;
            if (code) *code = 429;
            const qint64 next = pubrails::nextAllowedMs(s, l, now, day);
            return k.detail + (next > now ? " (next " + c + " post at " + hhmm(next) + ")" : QString(" (closed by publisher.config.rails)"));
        }
    }
    return {};
}

void Publisher::send(const QString& id, const QString& actor, Reply reply) {
    bool f = false; QJsonObject it = item(id, &f);
    if (!f) { reply(404, errObj(404, "unknown item")); return; }
    const QString st = it.value("status").toString();
    if (st == "published") { reply(200, it); return; }
    if (st == "reconcile" || st == "publishing") { reply(409, errObj(409, pubrails::actionBlock(st, "send"))); return; }
    if (st != "approved" && st != "scheduled") { reply(409, errObj(409, "only an approved item can be sent (it is " + st + ")")); return; }
    if (m_sending.contains(id)) { reply(409, errObj(409, "already sending")); return; }
    const QString kind = it.value("kind").toString();
    if (kind == "video" || kind == "spot") { const QString blk = mediaBlock(it.value("mediaId").toString()); if (!blk.isEmpty()) { it["status"] = "held"; it["why"] = blk; history(it, actor, "hold", blk); putItem(it); reply(409, errObj(409, blk)); return; } }
    QStringList todo; QJsonObject pc = it.value("perChannel").toObject();
    for (const QJsonValue& v : it.value("channels").toArray()) { const QString c = v.toString(); if (pc.value(c).toObject().value("state").toString() != "published") todo << c; }
    for (const QString& c : todo) if (killed(c)) { reply(423, errObj(423, "sending is stopped for " + c + " (kill switch)")); return; }
    int code = 0; bool stale = false;
    if (const QString why = railBlock(it, todo, &code, &stale); !why.isEmpty()) {
        if (stale) {   // the approval no longer covers this content: back to a person (a schedule time is kept for the next approval)
            it["status"] = "pending_approval"; it["why"] = why; it.remove("approval");
            history(it, actor, "approval stale", why); logAudit(id, actor, "approval-stale", why, it.value("revision").toInt(1)); putItem(it);
        }
        QJsonObject e = errObj(code, why); e["rail"] = true;
        reply(code, e); return;
    }
    // Persist every channel as 'publishing' BEFORE any network call: if this process stops mid-call, start-up finds the mark and
    // reconciles instead of sending again. The attempt is counted now, with the hash of the exact text the platform will show.
    m_sending.insert(id);
    const qint64 now = nowMs(); const QJsonObject cfg = config(true);
    for (const QString& c : todo) {
        QJsonObject one = pc.value(c).toObject();
        one["state"] = "publishing"; one["attempts"] = one.value("attempts").toInt() + 1; one["attemptStartedMs"] = double(now); one["lastTryMs"] = double(now);
        one["msgSha256"] = pubrails::textSha256(outgoingText(it, c, cfg)); one.remove("error"); one.remove("errorClass"); one.remove("retryable"); one.remove("reconcile");
        pc[c] = one;
    }
    it["perChannel"] = pc; it["status"] = "publishing"; it.remove("nextTryMs"); history(it, actor, "send", todo.join(", ")); putItem(it);
    auto left = std::make_shared<int>(todo.size());
    if (todo.isEmpty()) { finishSend(id, actor, reply); return; }
    for (const QString& c : todo) {
        sendChannel(id, c, [this, id, c, left, actor, reply](const pubrails::SendResult& r, const QJsonObject& res) {
            bool f2 = false; QJsonObject cur = item(id, &f2); if (!f2) { m_sending.remove(id); return; }
            QJsonObject pcs = cur.value("perChannel").toObject(); QJsonObject one = pcs.value(c).toObject();
            one["lastTryMs"] = double(nowMs()); one["state"] = pubrails::stateName(r.state);
            if (r.state == pubrails::Outcome::Published) {
                one["remoteId"] = res.value("remoteId"); one["permalink"] = res.value("permalink"); one["postedMs"] = double(nowMs());
                one.remove("error"); one.remove("errorClass"); one.remove("retryable"); if (res.contains("remoteStatus")) one["remoteStatus"] = res.value("remoteStatus");
            } else {
                one["error"] = r.error; one["errorClass"] = r.errorClass; one["retryable"] = r.retryable;
                if (r.state == pubrails::Outcome::Unknown) one["unknownSinceMs"] = double(nowMs());
            }
            pcs[c] = one; cur["perChannel"] = pcs; putItem(cur);
            if (--*left == 0) finishSend(id, actor, reply);
        });
    }
}

void Publisher::finishSend(const QString& id, const QString& actor, Reply reply) {
    m_sending.remove(id);
    bool f = false; QJsonObject it = item(id, &f); if (!f) { reply(404, errObj(404, "unknown item")); return; }
    const QJsonObject pc = it.value("perChannel").toObject();
    qint64 nextTry = 0; QStringList errs;
    const QString to = pubrails::aggregateStatus(it, &nextTry, &errs);
    it["status"] = to;
    if (to == "published") {
        it["publishedMs"] = double(nowMs());
        for (const QJsonValue& v : it.value("channels").toArray()) { const QString p = pc.value(v.toString()).toObject().value("permalink").toString(); if (!p.isEmpty()) { it["permalink"] = p; break; } }
        it.remove("lastError"); it.remove("nextTryMs"); history(it, actor, "published"); logAudit(id, actor, "published", it.value("permalink").toString(), it.value("revision").toInt(1));
    } else if (to == "reconcile") {
        it["lastError"] = errs.join("; "); it["why"] = "the outcome of the send is unknown: the platform is checked before anything is sent again (never blind)";
        history(it, actor, "reconcile", it.value("lastError").toString()); logAudit(id, actor, "send-unknown", it.value("lastError").toString(), it.value("revision").toInt(1));
        emit alert("warn", "publishing outcome unknown, reconciling: " + it.value("title").toString().left(60) + " — " + errs.join("; ").left(200));
    } else if (to == "approved") {
        it["lastError"] = errs.join("; "); it["nextTryMs"] = double(nextTry);
        history(it, actor, "send failed (retry at " + hhmm(nextTry) + ")", it.value("lastError").toString()); logAudit(id, actor, "send-failed", it.value("lastError").toString(), it.value("revision").toInt(1));
    } else {   // failed: attempts used up, or a refusal that retrying cannot fix (auth, permission, content, blocked)
        it["lastError"] = errs.join("; "); it.remove("nextTryMs");
        history(it, actor, "failed", it.value("lastError").toString()); logAudit(id, actor, "send-failed", it.value("lastError").toString(), it.value("revision").toInt(1));
        emit alert("warn", "publishing failed: " + it.value("title").toString().left(60) + " — " + errs.join("; ").left(200));
    }
    putItem(it);
    QJsonObject fail{{"error", errs.join("; ")}, {"item", it}, {"outcome", to == "reconcile" ? "unknown" : "failed"}};
    reply(to == "published" ? 200 : 502, to == "published" ? it : fail);
}

// =====================================================================================================
// reconcile: an unknown outcome is settled by looking at the platform, or by a person; never by sending again
// =====================================================================================================
void Publisher::reconcileTick() {
    const qint64 now = nowMs();
    for (const QJsonValue& v : items()) {
        const QJsonObject it = v.toObject();
        if (it.value("status").toString() != "reconcile" || m_reconciling.contains(it.value("id").toString())) continue;
        const QJsonObject pc = it.value("perChannel").toObject();
        for (const QJsonValue& ch : it.value("channels").toArray()) {
            const QJsonObject one = pc.value(ch.toString()).toObject();
            if (one.value("state").toString() != "unknown") continue;
            if (now - qint64(one.value("reconcile").toObject().value("lastCheckMs").toDouble()) < pubrails::kReconcileEveryMs) continue;
            reconcileChannel(it.value("id").toString(), ch.toString(), "reconcile", [](int, const QJsonObject&) {});
            return;   // one read per tick
        }
    }
}

void Publisher::reconcile(const QString& id, const QString& actor, Reply reply) {
    bool f = false; const QJsonObject it = item(id, &f);
    if (!f) { reply(404, errObj(404, "unknown item")); return; }
    if (const QString b = pubrails::actionBlock(it.value("status").toString(), "reconcile"); !b.isEmpty()) { reply(409, errObj(409, b)); return; }
    if (m_reconciling.contains(id)) { reply(409, errObj(409, "a reconcile check is already running for this item")); return; }
    const QJsonObject pc = it.value("perChannel").toObject();
    for (const QJsonValue& ch : it.value("channels").toArray()) if (pc.value(ch.toString()).toObject().value("state").toString() == "unknown") { reconcileChannel(id, ch.toString(), actor, reply); return; }
    reply(409, errObj(409, "no channel of this item has an unknown outcome"));
}

void Publisher::reconcileChannel(const QString& id, const QString& ch, const QString& actor, std::function<void(int, const QJsonObject&)> done) {
    const QJsonObject it = item(id); const QJsonObject one = it.value("perChannel").toObject().value(ch).toObject(); const QJsonObject c = config(true);
    qint64 started = qint64(one.value("attemptStartedMs").toDouble());
    if (started <= 0) started = qint64(one.value("lastTryMs").toDouble());
    const qint64 since = started - qint64(pubrails::kReconcileSlackMin) * 60000;
    // the verdict is applied to the item as it is when the answer arrives (it may have been resolved by hand meanwhile)
    auto finish = [this, id, ch, actor, started, done](bool checked, const QJsonObject& hit, const QString& note) {
        m_reconciling.remove(id);
        bool f = false; QJsonObject cur = item(id, &f);
        if (!f) { done(404, errObj(404, "unknown item")); return; }
        QJsonObject pc = cur.value("perChannel").toObject(); QJsonObject o = pc.value(ch).toObject();
        if (o.value("state").toString() != "unknown") { done(200, QJsonObject{{"item", cur}, {"note", ch + " was settled meanwhile"}}); return; }
        const pubrails::Verdict v = pubrails::reconcileVerdict(checked, !hit.isEmpty(), nowMs() - started);
        QJsonObject rec = o.value("reconcile").toObject(); rec["lastCheckMs"] = double(nowMs()); rec["checks"] = rec.value("checks").toInt() + 1; rec["note"] = note; o["reconcile"] = rec;
        if (v == pubrails::Verdict::Published) {
            o["state"] = "published"; o["remoteId"] = hit.value("id"); o["permalink"] = hit.value("permalink"); o["adopted"] = true;
            o["postedMs"] = hit.value("createdMs").toDouble() > 0 ? hit.value("createdMs") : QJsonValue(double(started)); o.remove("error"); o.remove("errorClass"); o.remove("retryable");
        } else if (v == pubrails::Verdict::NotFound) {
            o["state"] = "failed"; o["errorClass"] = "not_found"; o["retryable"] = false;
            o["error"] = QString("not on the %1 %2 min after the attempt: nothing was posted; a retry is a person's click").arg(ch == "facebook" ? "Page" : "site").arg(pubrails::kReconcileGiveUpMin);
        }
        pc[ch] = o; cur["perChannel"] = pc;
        if (v != pubrails::Verdict::StillUnknown) settleItem(cur, actor, "reconciled", ch + ": " + note);
        cur = putItem(cur);
        done(200, QJsonObject{{"item", cur}, {"channel", ch}, {"verdict", v == pubrails::Verdict::Published ? "published" : v == pubrails::Verdict::NotFound ? "not_found" : "unknown"}, {"note", note}});
    };
    m_reconciling.insert(id);
    if (ch == "facebook") {
        const QJsonObject fb = c.value("facebook").toObject(); const QString page = fb.value("pageId").toString(), token = fb.value("token").toString();
        if (page.isEmpty() || token.isEmpty()) { finish(false, {}, "could not look: the Facebook Page is not configured; not sent again"); return; }
        const QString kind = it.value("kind").toString(); const bool video = kind == "video" || kind == "spot";
        QUrlQuery q; q.addQueryItem("fields", video ? "id,description,created_time,permalink_url" : "id,message,created_time,permalink_url");
        q.addQueryItem("since", QString::number(qMax<qint64>(0, since / 1000))); q.addQueryItem("limit", "25"); q.addQueryItem("access_token", token);
        const QString sha = one.value("msgSha256").toString();
        m_http->json("GET", c.value("graphBase").toString() + "/" + page + (video ? "/videos" : "/posts") + "?" + q.toString(QUrl::FullyEncoded), {}, {}, 30000,
                     [finish, sha, since](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            if (http != 200) { finish(false, {}, "could not look (" + SuiteHttp::errorText(http, j, raw, e) + "): not sent again; checked again later"); return; }
            const QJsonObject hit = pubrails::findFacebookPost(j.value("data").toArray(), sha, since);
            finish(true, hit, hit.isEmpty() ? QString("not on the Page yet") : "found on the Page: " + hit.value("id").toString());
        });
        return;
    }
    if (ch == "website") {
        const QJsonObject w = c.value("website").toObject();
        if (w.value("url").toString().isEmpty() || w.value("user").toString().isEmpty() || w.value("key").toString().isEmpty()) { finish(false, {}, "could not look: the website endpoint is not configured; not sent again"); return; }
        const QString title = it.value("title").toString();
        QUrlQuery q; q.addQueryItem("search", title.simplified().section(' ', 0, 5)); q.addQueryItem("status", "publish,future,draft,pending,private");
        q.addQueryItem("context", "edit"); q.addQueryItem("per_page", "20"); q.addQueryItem("orderby", "date");
        const QByteArray auth = "Basic " + (w.value("user").toString() + ":" + w.value("key").toString()).toUtf8().toBase64();
        QString url = w.value("url").toString(); while (url.endsWith('/')) url.chop(1);
        m_http->json("GET", url + "/wp/v2/posts?" + q.toString(QUrl::FullyEncoded), {{"Authorization", auth}}, {}, 30000, [finish, title, since](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
            if (http != 200) { finish(false, {}, "could not look (" + SuiteHttp::errorText(http, j, raw, e) + "): not sent again; checked again later"); return; }
            const QJsonObject hit = pubrails::findWebsitePost(j.value("_array").toArray(), title, since);
            finish(true, hit, hit.isEmpty() ? QString("no post with this title on the site yet") : "found on the site: post " + hit.value("id").toString());
        });
        return;
    }
    finish(false, {}, "unknown channel " + ch);
}

// After reconcile / resolve settled a channel: the item status follows its channels (still reconcile while another is unknown).
void Publisher::settleItem(QJsonObject& it, const QString& actor, const QString& action, const QString& note) {
    qint64 nextTry = 0; QStringList errs;
    const QString to = pubrails::aggregateStatus(it, &nextTry, &errs);
    const QString id = it.value("id").toString();
    it["status"] = to;
    if (to == "published") {
        if (!it.contains("publishedMs")) it["publishedMs"] = double(nowMs());
        const QJsonObject pc = it.value("perChannel").toObject();
        for (const QJsonValue& v : it.value("channels").toArray()) { const QString p = pc.value(v.toString()).toObject().value("permalink").toString(); if (!p.isEmpty()) { it["permalink"] = p; break; } }
        it.remove("lastError"); it.remove("nextTryMs"); it["why"] = note;
    } else if (to == "approved") { it["lastError"] = errs.join("; "); it["nextTryMs"] = double(nextTry); it["why"] = note; }
    else if (to == "failed") { it["lastError"] = errs.join("; "); it.remove("nextTryMs"); it["why"] = note; emit alert("warn", "publishing: " + it.value("title").toString().left(60) + " — " + errs.join("; ").left(200)); }
    history(it, actor, action, note); logAudit(id, actor, action, note + " -> " + to, it.value("revision").toInt(1));
}

QJsonObject Publisher::resolve(const QString& id, const QJsonObject& body, const QString& actor) {
    bool f = false; QJsonObject it = item(id, &f);
    if (!f) return errObj(404, "unknown item");
    if (m_reconciling.contains(id)) return errObj(409, "a reconcile check is running for this item: try again in a moment");
    QString ch = body.value("channel").toString();
    QJsonObject pc = it.value("perChannel").toObject();
    if (ch.isEmpty()) for (const QJsonValue& v : it.value("channels").toArray()) if (pc.value(v.toString()).toObject().value("state").toString() == "unknown") { ch = v.toString(); break; }
    QJsonObject one = pc.value(ch).toObject();
    if (one.value("state").toString() != "unknown") return errObj(409, "channel " + ch + " has no unknown outcome to resolve");
    const QString outcome = body.value("outcome").toString(), note = body.value("note").toString().left(300);
    if (outcome == "posted") {
        one["state"] = "published"; one["adopted"] = true; one["resolvedBy"] = actor;
        one["remoteId"] = body.value("remoteId").toString(); one["permalink"] = body.value("permalink").toString();
        one["postedMs"] = one.value("attemptStartedMs").toDouble() > 0 ? one.value("attemptStartedMs") : QJsonValue(double(nowMs())); one.remove("error"); one.remove("errorClass"); one.remove("retryable");
    } else if (outcome == "not_posted") {
        one["state"] = "failed"; one["errorClass"] = "not_posted"; one["retryable"] = false; one["resolvedBy"] = actor;
        one["error"] = "checked by " + actor + ": not on the platform" + (note.isEmpty() ? QString() : " (" + note + ")") + "; a retry is a person's click";
    } else return errObj(400, "outcome must be posted or not_posted");
    pc[ch] = one; it["perChannel"] = pc;
    settleItem(it, actor, "resolve", ch + " " + outcome + (note.isEmpty() ? QString() : ": " + note));
    return putItem(it);
}

QJsonObject Publisher::rails() const {
    const qint64 now = nowMs(), day = pubrails::localDayStartMs(now);
    const QJsonArray all = items(); const QJsonObject rc = config().value("rails").toObject();
    QJsonObject o; int reconcileCount = 0;
    for (const QJsonValue& v : all) if (v.toObject().value("status").toString() == "reconcile") ++reconcileCount;
    for (const QString& ch : kChannels) {
        const pubrails::Limits l = pubrails::limitsFor(ch, rc); const pubrails::Stats s = pubrails::statsFor(all, ch, now, day);
        const qint64 next = pubrails::nextAllowedMs(s, l, now, day);
        QJsonObject c; c["hour"] = s.hour; c["day"] = s.day; c["lastMs"] = double(s.lastMs); c["perHour"] = l.perHour; c["perDay"] = l.perDay; c["minGapMin"] = l.minGapMin;
        c["nextAllowedMs"] = double(next); c["open"] = next >= 0 && next <= now;
        QJsonArray checks; QString why;
        for (const pubrails::Check& k : pubrails::rateChecks(ch, s, l, now, day)) { checks.append(QJsonObject{{"name", k.name}, {"pass", k.pass}, {"detail", k.detail}}); if (!k.pass && why.isEmpty()) why = k.detail; }
        if (next < 0) why = "closed by publisher.config.rails (a cap of 0)";
        else if (!why.isEmpty()) why += " (next post at " + hhmm(next) + ")";
        c["checks"] = checks; if (!why.isEmpty()) c["why"] = why;
        o[ch] = c;
    }
    o["reconcile"] = reconcileCount;
    o["constants"] = QJsonObject{{"facebookPerHour", pubrails::kFbPerHour}, {"facebookPerDay", pubrails::kFbPerDay}, {"facebookGapMin", pubrails::kFbGapMin},
                                 {"maxAttempts", pubrails::kMaxAttempts}, {"backoffMin", QJsonArray{pubrails::kBackoffMin[0], pubrails::kBackoffMin[1], pubrails::kBackoffMin[2]}},
                                 {"reconcileGiveUpMin", pubrails::kReconcileGiveUpMin}, {"reconcileEverySec", pubrails::kReconcileEveryMs / 1000}};
    return o;
}

void Publisher::metrics(const QString& id, Reply reply) {
    bool f = false; const QJsonObject it = item(id, &f);
    if (!f) { reply(404, errObj(404, "unknown item")); return; }
    const QString rid = it.value("perChannel").toObject().value("facebook").toObject().value("remoteId").toString();
    if (rid.isEmpty()) { reply(400, errObj(400, "nothing published to Facebook to measure yet")); return; }
    const QString token = config(true).value("facebook").toObject().value("token").toString();
    if (token.isEmpty()) { reply(400, errObj(400, "Facebook Page not configured")); return; }
    QUrlQuery q; q.addQueryItem("access_token", token); q.addQueryItem("fields", "shares,comments.summary(true),reactions.summary(true),permalink_url");
    m_http->json("GET", config().value("graphBase").toString() + "/" + rid + "?" + q.toString(QUrl::FullyEncoded), {}, {}, 30000, [this, id, reply](int http, const QJsonObject& d, const QByteArray& raw, const QString& e) {
        if (http != 200) { reply(502, errObj(502, "Graph API: " + SuiteHttp::errorText(http, d, raw, e))); return; }
        bool f2 = false; QJsonObject cur = item(id, &f2); if (!f2) { reply(404, errObj(404, "unknown item")); return; }
        QJsonObject m; m["reactions"] = d.value("reactions").toObject().value("summary").toObject().value("total_count").toInt();
        m["comments"] = d.value("comments").toObject().value("summary").toObject().value("total_count").toInt(); m["shares"] = d.value("shares").toObject().value("count").toInt();
        m["ms"] = double(nowMs()); cur["metrics"] = m; if (!d.value("permalink_url").toString().isEmpty()) cur["permalink"] = d.value("permalink_url");
        reply(200, putItem(cur));
    });
}

QJsonObject Publisher::stop(const QString& reason, const QString& scope, const QString& actor) {
    QJsonObject c = m_store->settingJson("publisher.config"); QJsonObject k = c.value("killed").toObject();
    const QString s = scope.isEmpty() ? "all" : scope;
    if (s == "all") k["all"] = true; else if (s == "facebook") k["facebook"] = true; else if (s == "site" || s == "website") k["site"] = true; else return errObj(400, "scope must be all, facebook or site");
    c["killed"] = k; c["killReason"] = reason; c["killedMs"] = double(nowMs()); c["killedBy"] = actor; if (s == "all") c["mode"] = "off";
    m_store->setSettingJson("publisher.config", c);
    logAudit(QString(), actor, "stop:" + s, reason, 0); emit alert("warn", "publishing stopped (" + s + "): " + reason); notify();
    return status();
}

QJsonObject Publisher::release(const QString& scope, const QString& actor) {
    QJsonObject c = m_store->settingJson("publisher.config"); QJsonObject k = c.value("killed").toObject();
    const QString s = scope.isEmpty() ? "all" : scope;
    if (s == "all") { k["all"] = false; k["facebook"] = false; k["site"] = false; } else if (s == "facebook") k["facebook"] = false; else if (s == "site" || s == "website") k["site"] = false;
    c["killed"] = k; m_store->setSettingJson("publisher.config", c);
    logAudit(QString(), actor, "release:" + s, QString(), 0); notify();
    return status();
}

void Publisher::tick() {
    reconcileTick();   // reads only, so it runs while sending is stopped too
    if (killed("all") && config().value("killed").toObject().value("all").toBool()) return;
    const QJsonObject c = config(true);
    const bool fbOk = !c.value("facebook").toObject().value("pageId").toString().isEmpty() && !c.value("facebook").toObject().value("token").toString().isEmpty();
    const bool webOk = !c.value("website").toObject().value("url").toString().isEmpty() && !c.value("website").toObject().value("key").toString().isEmpty();
    const qint64 now = nowMs();
    for (const QJsonValue& v : items()) {
        const QJsonObject it = v.toObject(); const QString st = it.value("status").toString();
        const bool due = st == "approved" || (st == "scheduled" && qint64(it.value("scheduleAtMs").toDouble()) <= now);
        if (!due || m_sending.contains(it.value("id").toString())) continue;
        if (qint64(it.value("nextTryMs").toDouble()) > now) continue;   // backoff after a definitive refusal
        bool ready = true;   // the queue accumulates until the channel is connected and not stopped
        QStringList todo; const QJsonObject pc = it.value("perChannel").toObject();
        for (const QJsonValue& ch : it.value("channels").toArray()) {
            const QString cn = ch.toString(); if (killed(cn) || (cn == "facebook" && !fbOk) || (cn == "website" && !webOk)) ready = false;
            if (pc.value(cn).toObject().value("state").toString() != "published") todo << cn;
        }
        if (!ready) continue;
        int code = 0; bool stale = false;
        if (!railBlock(it, todo, &code, &stale).isEmpty() && !stale) continue;   // a rate rail: it waits quietly (a stale approval is sent back to a person by send())
        send(it.value("id").toString(), "scheduler", [](int, const QJsonObject&) {});
        return;   // one at a time
    }
}

void Publisher::considerSpot(const QString& mediaId) {
    const QJsonObject c = config();
    if (c.value("mode").toString() == "off") return;
    bool f = false; const MediaRecord m = m_store->media(mediaId, &f); if (!f) return;
    const QJsonObject ag = m.extra.value("autogen").toObject(); if (ag.isEmpty()) return;
    const QString recipe = ag.value("recipe").toString();
    bool listed = false; for (const QJsonValue& r : c.value("recipes").toArray()) if (r.toString() == recipe) listed = true;
    if (!listed) return;
    const QString today = QDate::currentDate().toString(Qt::ISODate); int todayCount = 0;
    for (const QJsonValue& v : items()) {
        const QJsonObject it = v.toObject();
        if (it.value("mediaId").toString() == mediaId) return;   // queued once already
        if (it.value("source").toString() == "spot_job" && QDateTime::fromMSecsSinceEpoch(qint64(it.value("createdMs").toDouble())).date().toString(Qt::ISODate) == today) ++todayCount;
    }
    if (todayCount >= c.value("maxPerDay").toInt(12)) { emit alert("info", "daily social cap reached; skipped " + ag.value("headline").toString(m.title)); return; }
    QJsonObject d; d["fromMedia"] = mediaId; d["source"] = "spot_job"; d["channels"] = QJsonArray{"facebook"}; d["recipe"] = recipe;
    const QJsonObject r = draft(d, "spots");
    if (r.contains("error")) emit alert("info", "social queue: " + r.value("error").toString());
}

QJsonObject Publisher::pulse() const {
    QJsonObject o; o["page"] = m_page; o["comments"] = QJsonArray(); o["tagline"] = config().value("tagline");
    QJsonArray posts; for (const QJsonValue& v : queue("published", QString(), QString(), QString(), 6)) { const QJsonObject it = v.toObject(); posts.append(QJsonObject{{"title", it.value("title")}, {"message", it.value("caption").toString(it.value("body").toString())}, {"posted_at", iso(qint64(it.value("publishedMs").toDouble()))}}); }
    o["posts"] = posts;
    QList<MediaRecord> spots = m_store->mediaByType({"news", "weather", "program"});
    std::sort(spots.begin(), spots.end(), [](const MediaRecord& a, const MediaRecord& b) { return a.addedAtMs > b.addedAtMs; });
    QJsonArray hl; for (const MediaRecord& m : spots) { if (m.series.isEmpty() || m.archived) continue; hl.append(QJsonObject{{"id", m.id}, {"display_title", m.title}, {"program_series", m.series}, {"duration_sec", m.durationSec}, {"category", m.extra.value("category")}, {"created_at", iso(m.addedAtMs)}}); if (hl.size() >= 12) break; }
    o["highlights"] = hl;
    return o;
}

// =====================================================================================================
// PlayPlay OBS station-compatible shapes
// =====================================================================================================
QJsonObject Publisher::socialConfig() const {
    const QJsonObject c = config(true); const QJsonObject fb = c.value("facebook").toObject();
    QJsonObject o; o["page_id"] = fb.value("pageId"); o["page_token_set"] = !fb.value("token").toString().isEmpty(); o["mode"] = c.value("mode").toString() == "review" ? "review" : c.value("mode");
    o["recipes"] = c.value("recipes"); o["tagline"] = c.value("tagline"); o["max_per_day"] = c.value("maxPerDay"); return o;
}

QJsonObject Publisher::saveSocialConfig(const QJsonObject& b) {
    QJsonObject patch; QJsonObject fb;
    if (b.contains("page_id")) fb["pageId"] = b.value("page_id").toString().trimmed();
    if (!b.value("page_token").toString().isEmpty()) fb["token"] = b.value("page_token").toString().trimmed();
    if (!fb.isEmpty()) patch["facebook"] = fb;
    if (b.contains("mode")) { const QString m = b.value("mode").toString(); if (!QStringList{"off", "review", "auto"}.contains(m)) return errObj(400, "mode must be off|review|auto"); patch["mode"] = m; }
    if (b.value("recipes").isArray()) patch["recipes"] = b.value("recipes");
    if (b.contains("tagline")) patch["tagline"] = b.value("tagline");
    if (b.contains("max_per_day")) patch["maxPerDay"] = b.value("max_per_day").toInt(12);
    setConfig(patch); return socialConfig();
}

QJsonObject Publisher::socialRow(const QJsonObject& it) {
    static const QHash<QString, QString> map = {{"pending_approval", "pending_review"}, {"held", "pending_review"}, {"draft", "pending_review"}, {"approved", "approved"}, {"scheduled", "approved"},
        {"publishing", "posting"}, {"reconcile", "posting"}, {"published", "posted"}, {"failed", "error"}, {"skipped", "skipped"}, {"retracted", "skipped"}, {"corrected", "posted"}};
    const QJsonObject fb = it.value("perChannel").toObject().value("facebook").toObject();
    QJsonObject r; r["id"] = it.value("id"); r["platform"] = "facebook"; r["kind"] = it.value("kind").toString() == "spot" ? "video" : it.value("kind"); r["media_id"] = it.value("mediaId");
    r["recipe"] = it.value("recipe"); r["title"] = it.value("title"); r["message"] = it.value("caption").toString(it.value("body").toString()); r["link"] = it.value("linkUrl");
    r["status"] = map.value(it.value("status").toString(), it.value("status").toString()); r["attempts"] = fb.value("attempts").toInt(); r["error"] = it.value("lastError").toString(it.value("error").toString(fb.value("error").toString()));
    r["remote_id"] = fb.value("remoteId"); r["created_at"] = iso(qint64(it.value("createdMs").toDouble())); r["posted_at"] = iso(qint64(fb.value("postedMs").toDouble())); r["updated_at"] = iso(qint64(it.value("updatedMs").toDouble()));
    return r;
}

QJsonArray Publisher::socialQueue(bool includeDone) const {
    QJsonArray out;
    for (const QJsonValue& v : queue(includeDone ? "all" : "open", QString(), QString(), "facebook", 60)) out.append(socialRow(v.toObject()));
    return out;
}

QJsonObject Publisher::socialEnqueue(const QJsonObject& b) {
    QJsonObject d; d["mediaId"] = b.value("media_id"); d["caption"] = b.value("message"); d["title"] = b.value("title"); d["linkUrl"] = b.value("link"); d["channels"] = QJsonArray{"facebook"};
    const QString kind = b.value("kind").toString("video"); d["kind"] = kind == "status" ? "text" : kind;
    if (d.value("kind").toString() == "video" && b.value("media_id").toString().isEmpty()) return errObj(400, "video post needs a media_id");
    QJsonObject it = draft(d, "station-api");
    if (it.contains("error")) return it;
    if (b.value("approved").toBool() && it.value("status").toString() != "approved") { const QJsonObject a = act(it.value("id").toString(), "approve", QJsonObject(), "station-api"); if (!a.contains("error")) it = a; }
    return socialRow(it);
}

QJsonObject Publisher::publishConfig() const {
    const QJsonObject s = status(); const QJsonObject cf = s.value("configured").toObject(); const QJsonObject c = config();
    QJsonObject o;
    o["facebook"] = QJsonObject{{"configured", cf.value("facebook")}, {"page_id", c.value("facebook").toObject().value("pageId")}, {"needs", "facebook.pageId + facebook.token (Publish page)"}};
    o["website"] = QJsonObject{{"configured", cf.value("website")}, {"endpoint", c.value("website").toObject().value("url")}, {"needs", "website.url + website.user + website.key"}};
    o["tv"] = QJsonObject{{"configured", true}, {"needs", "spots publish into ad rotation from the Spot studio"}};
    return o;
}

QJsonArray Publisher::publications(int limit) const {
    QJsonArray out;
    for (const QJsonValue& v : items()) {
        const QJsonObject it = v.toObject(); const QJsonObject pc = it.value("perChannel").toObject();
        for (auto c = pc.begin(); c != pc.end(); ++c) {
            const QJsonObject one = c.value().toObject(); if (one.value("state").toString().isEmpty() || one.value("state").toString() == "pending") continue;
            QJsonObject p; p["id"] = it.value("id").toString() + ":" + c.key(); p["item_id"] = it.value("id"); p["platform"] = c.key(); p["status"] = it.value("status").toString() == "retracted" ? "retracted" : one.value("state").toString();
            p["platform_object_id"] = one.value("remoteId"); p["permalink"] = one.value("permalink"); p["published_at"] = iso(qint64(one.value("postedMs").toDouble())); p["corrects"] = it.value("corrects");
            p["metrics"] = it.value("metrics"); p["payload"] = QJsonObject{{"headline", it.value("title")}, {"body", it.value("body").toString(it.value("caption").toString())}}; p["error"] = one.value("error");
            out.append(p);
        }
    }
    while (out.size() > limit) out.removeFirst();
    return out;
}
