// FailoverPair — heartbeat, promote/demote, state replication, media pulls.
#include "failoverpair.h"
#include "sessionsmanager.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>

using namespace sched;

static const int kHttpTimeoutMs = 4000;
static const int kReplTimeoutMs = 60000;
static const int kMaxPulls = 3;
static const qint64 kPullRetryMs = 10 * 60 * 1000;
static const QStringList kReplGroups = {"channels", "schedule", "media", "settings", "cg", "spots", "programming"};

// ---------------------------------------------------------------------------
// Construction / config
// ---------------------------------------------------------------------------

FailoverPair::FailoverPair(ScheduleStore* store, ScheduleExecutor* exec, SessionsManager* sessions, QObject* parent)
    : QObject(parent), m_store(store), m_exec(exec), m_sessions(sessions) {
    m_dataDir = sessions ? QFileInfo(sessions->sessionsDir()).path() : QString("./data");
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &FailoverPair::cycle);
}

QString FailoverPair::normUrl(const QString& u) { QString s = u.trimmed(); while (s.endsWith('/')) s.chop(1); if (!s.isEmpty() && !s.contains("://")) s = "http://" + s; return s; }

QJsonObject FailoverPair::config() const {
    QJsonObject c = m_store->settingJson("failover.config");
    auto def = [&c](const char* k, const QJsonValue& v) { if (!c.contains(k)) c[k] = v; };
    def("role", "standalone"); def("peerUrl", ""); def("peerToken", ""); def("intervalSec", 5); def("graceSec", 30);
    def("auto", true); def("replicate", true); def("replicateIntervalSec", 60); def("pullMedia", false);
    const QString r = c.value("role").toString();
    if (r != "primary" && r != "backup") c["role"] = "standalone";
    c["intervalSec"] = qBound(2, c.value("intervalSec").toInt(5), 120);
    c["graceSec"] = qMax(c.value("intervalSec").toInt() * 2, c.value("graceSec").toInt(30));
    c["replicateIntervalSec"] = qBound(c.value("intervalSec").toInt(), c.value("replicateIntervalSec").toInt(60), 3600);
    c["peerUrl"] = normUrl(c.value("peerUrl").toString());
    return c;
}

QJsonObject FailoverPair::setConfig(const QJsonObject& patch) {
    const QJsonObject before = config();
    QJsonObject c = before;
    for (const char* k : {"role", "peerUrl", "peerToken", "intervalSec", "graceSec", "auto", "replicate", "replicateIntervalSec", "pullMedia"})
        if (patch.contains(k)) c[k] = patch.value(k);
    if (patch.contains("role")) {
        const QString r = patch.value("role").toString().toLower().trimmed();
        c["role"] = (r == "primary" || r == "backup") ? r : "standalone";
    }
    // backup -> primary is a takeover, exactly like the Promote button: it must arm the channels and bump the epoch, otherwise the
    // old primary yields to the newer epoch while this node airs nothing. Store the rest of the patch first, then let promote() switch the role.
    const QString oldRole = before.value("role").toString();
    const bool takeover = oldRole == "backup" && c.value("role").toString() == "primary";
    if (takeover) c["role"] = "backup";
    m_store->setSettingJson("failover.config", c);
    const QJsonObject after = config();
    if (after.value("peerUrl") != before.value("peerUrl")) { m_peer = QJsonObject(); m_peerLastOkMs = 0; m_peerError.clear(); m_missed = 0; m_peerMarks = QJsonObject(); }
    const QString newRole = after.value("role").toString();
    if (newRole != oldRole) {
        if (newRole == "backup") {
            setArmedAll(false);
            note("role", "set to backup by operator — all channels disarmed and taken off air (warm standby)", "warning");
            emit takeOffAir("role set to backup by operator");
        }
        else if (newRole == "primary") { if (epoch() == 0) m_store->setSetting("failover.epoch", QString::number(QDateTime::currentMSecsSinceEpoch())); note("role", "set to primary by operator (channels left as they are)"); }   // standalone -> primary
        else note("role", "set to standalone — pairing idle");
        emit roleChanged(newRole);
    }
    m_announcedDown = m_announcedAuth = m_announcedBothBackup = false;
    if (takeover) promote("role set to primary by operator");   // restarts the timer and emits roleChanged/statusChanged itself
    else restartTimer();
    emit statusChanged();
    return status();
}

QString FailoverPair::role() const { return config().value("role").toString(); }
qint64 FailoverPair::epoch() const { return m_store->setting("failover.epoch", "0").toLongLong(); }
QString FailoverPair::nodeId() const { return m_sessions ? m_sessions->nodeId() : m_store->setting("node.id"); }
QString FailoverPair::peerUrl() const { return config().value("peerUrl").toString(); }
QString FailoverPair::peerToken() const { return config().value("peerToken").toString(); }

void FailoverPair::start() {
    if (m_started) return;
    m_started = true;
    if (role() == "backup") { setArmedAll(false); qInfo() << "[Failover] booting as BACKUP — channels disarmed until promoted"; }
    QDir().mkpath(QDir(m_dataDir).filePath("media/replica"));
    restartTimer();
    note("start", "failover pairing started as " + role());
}

void FailoverPair::restartTimer() {
    const QJsonObject c = config();
    m_timer.stop();
    if (c.value("role").toString() == "standalone" || c.value("peerUrl").toString().isEmpty()) return;
    m_timer.start(c.value("intervalSec").toInt() * 1000);
    QTimer::singleShot(3000, this, &FailoverPair::cycle);   // first probe soon after start / config change
}

void FailoverPair::note(const QString& event, const QString& detail, const QString& level) {
    QJsonObject h; h["timeMs"] = double(QDateTime::currentMSecsSinceEpoch()); h["event"] = event; h["detail"] = detail; h["level"] = level; h["role"] = role();
    m_history.prepend(h);
    while (m_history.size() > 50) m_history.removeLast();
    qInfo().noquote() << "[Failover]" << event << detail;
}

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

void FailoverPair::fetchJson(const QString& method, const QString& url, const QString& token, const QJsonObject& body, int timeoutMs, JsonCb cb) {
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Accept", "application/json");
    if (!token.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
        req.setRawHeader("X-PP-Pass", token.toUtf8());
    }
    req.setTransferTimeout(timeoutMs > 0 ? timeoutMs : kHttpTimeoutMs);
    const QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QString m = method.toUpper();
    QNetworkReply* r = m == "POST" ? m_nam.post(req, data) : m == "PUT" ? m_nam.put(req, data) : m == "DELETE" ? m_nam.deleteResource(req) : m_nam.get(req);
    connect(r, &QNetworkReply::finished, this, [r, cb]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = r->readAll();
        QJsonObject j = QJsonDocument::fromJson(raw).object();
        if (r->error() != QNetworkReply::NoError && http == 0) { cb(0, j, r->errorString()); return; }
        if (http >= 400 && !j.contains("error")) j["error"] = QString("HTTP %1 %2").arg(http).arg(r->errorString());
        cb(http, j, http >= 400 ? j.value("error").toString() : QString());
    });
}

// ---------------------------------------------------------------------------
// Heartbeat cycle
// ---------------------------------------------------------------------------

QJsonObject FailoverPair::heartbeatBody() const {
    QJsonObject o;
    o["nodeId"] = nodeId(); o["nodeName"] = m_store->setting("node.name"); o["role"] = role(); o["epoch"] = double(epoch());
    o["timeMs"] = double(QDateTime::currentMSecsSinceEpoch());
    QJsonArray armed; QJsonObject onAir;
    for (const ChannelProfile& p : m_store->channels()) {
        const ScheduleExecutor::ChannelStatus st = m_exec->status(p.id);
        if (st.armed || (p.armed && !m_exec->hasChannel(p.id))) armed.append(p.id);
        QJsonObject c; c["state"] = st.engineState; c["title"] = st.onAirTitle; c["connected"] = st.connected; c["armed"] = st.armed;
        onAir[p.id] = c;
    }
    o["armedChannels"] = armed; o["onAir"] = onAir;
    return o;
}

void FailoverPair::cycle() {
    const QJsonObject c = config();
    if (c.value("role").toString() == "standalone" || c.value("peerUrl").toString().isEmpty() || m_inFlight) return;
    m_inFlight = true; m_lastAttemptMs = QDateTime::currentMSecsSinceEpoch();
    fetchJson("GET", c.value("peerUrl").toString() + "/api/v1/failover/heartbeat", c.value("peerToken").toString(), QJsonObject(), kHttpTimeoutMs,
              [this](int http, const QJsonObject& j, const QString& err) { m_inFlight = false; onHeartbeat(http, j, err); });
}

void FailoverPair::onHeartbeat(int http, const QJsonObject& peer, const QString& error) {
    const QJsonObject c = config();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool wasOk = m_peerError.isEmpty() && !m_peer.isEmpty();
    bool ok = false;
    if (http == 401 || http == 403) {
        // The peer answered: it is alive, our token is wrong. Never count this toward the silence window.
        m_peerLastOkMs = now; m_peer = QJsonObject(); m_missed = 0;
        m_peerError = QString("peer is alive but rejects our token (HTTP %1) — set peerToken to the peer's rest.token").arg(http);
        if (!m_announcedAuth) { m_announcedAuth = true; emit alert("error", "[failover] " + m_peerError); note("auth", m_peerError, "error"); }
    } else if (http == 0 || http >= 400 || peer.value("nodeId").toString().isEmpty()) {
        ++m_missed; m_peer = QJsonObject();
        m_peerError = http == 0 ? ("peer unreachable: " + error) : QString("peer HTTP %1 %2").arg(http).arg(error);
        if (wasOk) { emit alert("warning", "[failover] " + m_peerError); note("peer_offline", m_peerError, "warning"); }
    } else {
        ok = true; m_missed = 0; m_peer = peer; m_peerLastOkMs = now;
        if (!wasOk) { note("peer_online", QString("peer %1 is %2, epoch %3").arg(peer.value("nodeName").toString(peer.value("nodeId").toString()), peer.value("role").toString()).arg(qint64(peer.value("epoch").toDouble())));
                      if (!m_peerError.isEmpty()) emit alert("info", "[failover] peer back: " + peer.value("role").toString()); }
        m_peerError.clear(); m_announcedAuth = false; m_announcedDown = false;
        resolveRoles(peer);
    }
    const QString myRole = config().value("role").toString();   // may have changed in resolveRoles
    if (myRole == "backup") {
        if (ok) {
            if (c.value("replicate").toBool(true) && !m_replInFlight && now - m_lastReplTryMs >= c.value("replicateIntervalSec").toInt() * 1000LL) { m_lastReplTryMs = now; replicate(); }
        } else if (m_peerLastOkMs > 0) {
            const qint64 silentMs = now - m_peerLastOkMs;
            if (silentMs > c.value("graceSec").toInt() * 1000LL) {
                if (c.value("auto").toBool(true)) promote(QString("peer silent %1 s").arg(silentMs / 1000));
                else if (!m_announcedDown) { m_announcedDown = true; emit alert("critical", QString("[failover] PRIMARY UNREACHABLE for %1 s — auto-promote is OFF; promote from the Remote view to take air").arg(silentMs / 1000)); note("primary_down", "auto-promote off", "critical"); }
            }
        } else if (!m_announcedDown && now - m_lastAttemptMs >= 0 && m_missed * c.value("intervalSec").toInt() > c.value("graceSec").toInt()) {
            m_announcedDown = true;
            emit alert("warning", "[failover] the primary has never answered since this backup started — not promoting automatically until it has been seen once");
            note("peer_never_seen", m_peerError, "warning");
        }
    }
    emit statusChanged();
}

void FailoverPair::resolveRoles(const QJsonObject& peer) {
    const QString mine = role(), theirs = peer.value("role").toString();
    if (mine == "primary" && theirs == "primary") {
        const qint64 myE = epoch(), theirE = qint64(peer.value("epoch").toDouble());
        if (theirE > myE || (theirE == myE && peer.value("nodeId").toString() > nodeId()))
            demote(QString("peer is primary with a newer epoch (%1 >= %2)").arg(theirE).arg(myE));
    } else if (mine == "backup" && theirs == "backup") {
        if (!m_announcedBothBackup) { m_announcedBothBackup = true; emit alert("critical", "[failover] BOTH nodes are backup — nobody drives playout; promote one"); note("both_backup", "both nodes report role backup", "critical"); }
    } else {
        m_announcedBothBackup = false;
    }
}

// ---------------------------------------------------------------------------
// Promotion
// ---------------------------------------------------------------------------

void FailoverPair::setArmedAll(bool armed) {
    // Only touch channels that are not already in the wanted state (a backup calls this after every replication).
    for (ChannelProfile p : m_store->channels()) {
        if (m_exec->hasChannel(p.id)) {
            if (m_exec->status(p.id).armed != armed || p.armed != armed) {
                // setArmed() saves the profile the executor CACHED (taken at addChannel/reloadChannel). Refresh it first when arming, so a
                // replicated profile (fill, identity, timezone, strict, ...) is never overwritten by the boot-time copy on promotion.
                // (Not when disarming: reloadChannel() would re-evaluate a still-armed channel and could push once more before the disarm.)
                if (armed) m_exec->reloadChannel(p.id);
                m_exec->setArmed(p.id, armed);   // saves the profile too
            }
        }
        else if (p.armed != armed) { p.armed = armed; m_store->saveChannel(p); }
    }
}

QJsonObject FailoverPair::promote(const QString& reason) {
    QJsonObject r;
    if (role() == "primary") { r["role"] = "primary"; r["note"] = "already primary"; return r; }
    QJsonObject c = config(); c["role"] = "primary"; m_store->setSettingJson("failover.config", c);
    m_store->setSetting("failover.epoch", QString::number(QDateTime::currentMSecsSinceEpoch()));
    setArmedAll(true);
    m_announcedDown = false;
    note("promoted", reason, "critical");
    emit alert("critical", "[failover] PROMOTED TO PRIMARY (" + reason + ") — every channel armed, joining the schedule at the wall clock");
    emit roleChanged("primary");
    restartTimer();
    emit statusChanged();
    r["role"] = "primary"; r["epoch"] = double(epoch()); r["reason"] = reason;
    return r;
}

QJsonObject FailoverPair::demote(const QString& reason) {
    QJsonObject r;
    if (role() == "backup") { r["role"] = "backup"; r["note"] = "already backup"; return r; }
    QJsonObject c = config(); c["role"] = "backup"; m_store->setSettingJson("failover.config", c);
    setArmedAll(false);
    m_peerMarks = QJsonObject();   // full re-sync from the new primary
    note("demoted", reason, "warning");
    emit alert("warning", "[failover] demoted to BACKUP (" + reason + ") — channels disarmed and taken off air, warm standby");
    emit takeOffAir("demoted: " + reason);   // disarming only stops pushing; the engines would keep airing the hour(s) already pushed
    emit roleChanged("backup");
    restartTimer();
    emit statusChanged();
    r["role"] = "backup"; r["reason"] = reason;
    return r;
}

// ---------------------------------------------------------------------------
// Replication
// ---------------------------------------------------------------------------

void FailoverPair::replicate() {
    if (!m_sessions) return;
    const QJsonObject c = config();
    QUrlQuery q;
    q.addQueryItem("groups", kReplGroups.join(","));
    q.addQueryItem("since", QString::fromUtf8(QJsonDocument(m_peerMarks).toJson(QJsonDocument::Compact)));
    q.addQueryItem("secrets", "1");   // ask for the credential settings too (spots LLM key, Star tokens, camera passwords); the primary only sends them when its REST is token-protected
    m_replInFlight = true;
    fetchJson("GET", c.value("peerUrl").toString() + "/api/v1/sessions/export-live?" + q.toString(QUrl::FullyEncoded), c.value("peerToken").toString(), QJsonObject(), kReplTimeoutMs,
              [this](int http, const QJsonObject& bundle, const QString& err) {
        m_replInFlight = false;
        if (http != 200) { note("replicate_failed", err.isEmpty() ? QString("HTTP %1").arg(http) : err, "warning"); emit statusChanged(); return; }
        if (role() != "backup") return;   // promoted while the request was in flight: never overwrite a primary
        const QJsonObject groups = bundle.value("groups").toObject();
        QStringList present; for (auto it = groups.begin(); it != groups.end(); ++it) if (kReplGroups.contains(it.key())) present << it.key();   // never an unknown group: that would make applyBundle refuse the whole bundle
        m_peerMarks = bundle.value("marks").toObject();
        if (!present.isEmpty()) {
            // mirror: keep the primary's media ids (contracts / recipes / spots settings reference them and are copied verbatim)
            SessionsManager::ApplyOptions opt; opt.replace = present.contains("schedule"); opt.keepOutputs = true; opt.keepArmed = true; opt.mirror = true;
            const QJsonObject rep = m_sessions->applyBundle(bundle, present, false, opt);
            m_lastReplReport = rep; m_lastReplGroups = QJsonArray::fromStringList(present); m_lastReplOkMs = QDateTime::currentMSecsSinceEpoch();
            // The unresolved list is only as complete as the groups this cycle carried: the media group reports every record whose file is
            // missing (rebuild); a schedule-only cycle reports just the items it references (merge); every other group says nothing about
            // media and must not wipe what earlier cycles found.
            QList<QJsonObject> found; for (const QJsonValue& u : rep.value("unresolvedMedia").toArray()) found.append(u.toObject());
            if (present.contains("media")) m_unresolved = found;
            else if (present.contains("schedule")) {
                for (const QJsonObject& u : found) {
                    bool have = false; for (const QJsonObject& k : m_unresolved) if (k.value("id") == u.value("id")) { have = true; break; }
                    if (!have) m_unresolved.append(u);
                }
            }
            // The channel set / profiles may have changed behind the executor's back: let the Supervisor reload it and spawn engines for new channels.
            if (present.contains("channels")) emit channelsApplied(rep);
            setArmedAll(false);   // belt and braces: a replicated profile never arms the standby
            note("replicated", QString("%1 from primary; %2 unresolved media").arg(present.join(", ")).arg(m_unresolved.size()));
            if (!rep.value("errors").toArray().isEmpty()) note("replicate_errors", QString::fromUtf8(QJsonDocument(rep.value("errors").toArray()).toJson(QJsonDocument::Compact)).left(300), "warning");
            if (config().value("pullMedia").toBool(false)) pullMissingMedia();
        } else {
            m_lastReplOkMs = QDateTime::currentMSecsSinceEpoch();
        }
        emit statusChanged();
    });
}

void FailoverPair::pullMissingMedia() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonObject& m : m_unresolved) {
        if (m_pulling.size() >= kMaxPulls) break;
        const QString id = m.value("id").toString();
        if (id.isEmpty() || m_pulling.contains(id)) continue;
        if (m_pullFailedMs.contains(id) && now - m_pullFailedMs.value(id) < kPullRetryMs) continue;
        pullOne(m);
    }
}

void FailoverPair::pullOne(const QJsonObject& m) {
    const QString id = m.value("id").toString();
    const QString base = QFileInfo(QDir::fromNativeSeparators(m.value("path").toString())).fileName();
    const QString dir = QDir(m_dataDir).filePath("media/replica");
    QDir().mkpath(dir);
    const QString target = QDir(dir).filePath(id.left(12) + "_" + (base.isEmpty() ? "media.bin" : base));
    auto* file = new QFile(target + ".part", this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) { file->deleteLater(); m_pullFailedMs[id] = QDateTime::currentMSecsSinceEpoch(); return; }
    QNetworkRequest req(QUrl(peerUrl() + "/api/v1/download/" + id));
    req.setRawHeader("Authorization", "Bearer " + peerToken().toUtf8());
    req.setTransferTimeout(0);
    QNetworkReply* r = m_nam.get(req);
    m_pulling.insert(id);
    connect(r, &QNetworkReply::readyRead, this, [r, file]() { file->write(r->readAll()); });
    connect(r, &QNetworkReply::finished, this, [this, r, file, id, target]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        file->write(r->readAll()); file->close();
        m_pulling.remove(id);
        if (r->error() != QNetworkReply::NoError || http != 200) {
            QFile::remove(target + ".part"); m_pullFailedMs[id] = QDateTime::currentMSecsSinceEpoch();
            note("pull_failed", id + ": " + (http ? QString("HTTP %1").arg(http) : r->errorString()), "warning");
        } else {
            QFile::remove(target); QFile::rename(target + ".part", target);
            bool f = false; MediaRecord rec = m_store->media(id, &f);
            if (f) { rec.path = QDir::toNativeSeparators(target); rec.extra.remove("missing"); rec.extra["replicaOf"] = peerUrl(); m_store->saveMedia(rec); }
            for (int i = 0; i < m_unresolved.size(); ++i) if (m_unresolved[i].value("id").toString() == id) { m_unresolved.removeAt(i); break; }
            ++m_pulled; note("pulled", rec.title.isEmpty() ? id : rec.title);
        }
        file->deleteLater();
        if (config().value("pullMedia").toBool(false)) pullMissingMedia();
        emit statusChanged();
    });
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

QJsonObject FailoverPair::status() const {
    const QJsonObject c = config();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject o;
    o["role"] = c.value("role"); o["nodeId"] = nodeId(); o["nodeName"] = m_store->setting("node.name"); o["epoch"] = double(epoch());
    o["config"] = c; o["active"] = m_timer.isActive();
    QJsonObject peer;
    peer["url"] = c.value("peerUrl"); peer["state"] = m_peer; peer["reachable"] = !m_peer.isEmpty() && m_peerError.isEmpty();
    peer["lastOkMs"] = double(m_peerLastOkMs); peer["silentSec"] = m_peerLastOkMs > 0 ? double((now - m_peerLastOkMs) / 1000.0) : -1.0;
    peer["error"] = m_peerError; peer["missed"] = m_missed; peer["role"] = m_peer.value("role"); peer["epoch"] = m_peer.value("epoch"); peer["nodeId"] = m_peer.value("nodeId"); peer["nodeName"] = m_peer.value("nodeName");
    o["peer"] = peer;
    QJsonObject repl;
    repl["enabled"] = c.value("replicate"); repl["lastOkMs"] = double(m_lastReplOkMs); repl["lastTryMs"] = double(m_lastReplTryMs); repl["inFlight"] = m_replInFlight;
    repl["lastGroups"] = m_lastReplGroups; repl["peerMarks"] = m_peerMarks; repl["lastReport"] = m_lastReplReport;
    QJsonArray un; for (const QJsonObject& u : m_unresolved) un.append(u); repl["unresolvedMedia"] = un;
    repl["pulling"] = int(m_pulling.size()); repl["pulled"] = m_pulled;
    o["replication"] = repl;
    QJsonArray armed; for (const ChannelProfile& p : m_store->channels()) { const auto st = m_exec->status(p.id); if (st.armed || (!m_exec->hasChannel(p.id) && p.armed)) armed.append(p.id); }
    o["armedChannels"] = armed;
    o["historyCount"] = m_history.size();
    o["timeMs"] = double(now);
    return o;
}

QJsonArray FailoverPair::history() const { QJsonArray a; for (const QJsonObject& h : m_history) a.append(h); return a; }

QJsonObject FailoverPair::wsPayload() const {
    const QJsonObject c = config();
    QJsonObject o; o["role"] = c.value("role"); o["epoch"] = double(epoch());
    QJsonObject peer; peer["url"] = c.value("peerUrl"); peer["reachable"] = !m_peer.isEmpty() && m_peerError.isEmpty(); peer["role"] = m_peer.value("role"); peer["epoch"] = m_peer.value("epoch"); peer["error"] = m_peerError;
    o["peer"] = peer; o["lastHeartbeat"] = double(m_peerLastOkMs); o["lastReplicatedMs"] = double(m_lastReplOkMs);
    o["sessions"] = m_sessions ? m_sessions->list().size() : 0;
    return o;
}
