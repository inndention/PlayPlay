// Reporter feed: see reporterfeed.h. Pure logic first (ReporterFeed), then the Supervisor wiring (install) that hooks it to
// ffmpeg.exe listeners, the media library, loopback transport takes and the WebSocket.
#include "reporterfeed.h"
#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using sched::MediaRecord;
using sched::ChannelProfile;

namespace reporterfeed {

// ---------------------------------------------------------------------------------------------------------------------
// Config / Session JSON
// ---------------------------------------------------------------------------------------------------------------------
QJsonObject Config::toJson() const {
    QJsonObject o;
    o["host"] = host; o["rtmpPortBase"] = rtmpPortBase; o["portCount"] = portCount; o["udpPortBase"] = udpPortBase; o["app"] = app;
    o["mode"] = mode; o["mediamtxPublishBase"] = mediamtxPublishBase; o["mediamtxWhipBase"] = mediamtxWhipBase; o["mediamtxPullBase"] = mediamtxPullBase;
    o["defaultCountdownSec"] = defaultCountdownSec; o["ffmpegPath"] = ffmpegPath;
    return o;
}

Config Config::fromJson(const QJsonObject& o, const Config& base) {
    Config c = base;
    auto str = [&](const char* k, QString& v) { if (o.contains(k)) v = o.value(k).toString().trimmed(); };
    auto num = [&](const char* k, int& v, int lo, int hi) { if (o.contains(k)) v = qBound(lo, o.value(k).toInt(v), hi); };
    str("host", c.host); num("rtmpPortBase", c.rtmpPortBase, 1024, 65000); num("portCount", c.portCount, 1, 64); num("udpPortBase", c.udpPortBase, 1024, 65000);
    str("app", c.app); str("mode", c.mode); str("mediamtxPublishBase", c.mediamtxPublishBase); str("mediamtxWhipBase", c.mediamtxWhipBase);
    str("mediamtxPullBase", c.mediamtxPullBase); num("defaultCountdownSec", c.defaultCountdownSec, 0, 600); str("ffmpegPath", c.ffmpegPath);
    if (c.app.isEmpty()) c.app = "reporter";
    if (c.mode != "mediamtx") c.mode = "ffmpeg";
    static const QRegularExpression safeApp("^[A-Za-z0-9_-]+$");
    if (!safeApp.match(c.app).hasMatch()) c.app = "reporter";
    return c;
}

QJsonObject Session::toJson() const {
    QJsonObject o;
    o["id"] = id; o["reporter"] = reporter; o["title"] = title; o["channel"] = channel; o["status"] = status;
    o["countdownSec"] = countdownSec; o["remainingSec"] = remainingSec; o["slot"] = slot; o["keyHint"] = keyHash.left(8);
    o["feedUp"] = feedUp; o["ingestRestarts"] = ingestRestarts; o["lastError"] = lastError;
    o["createdMs"] = double(createdMs); o["goMs"] = double(goMs); o["liveMs"] = double(liveMs); o["endedMs"] = double(endedMs);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------
// ReporterFeed
// ---------------------------------------------------------------------------------------------------------------------
ReporterFeed::ReporterFeed(QObject* parent) : QObject(parent) {
    nowMs = [] { return QDateTime::currentMSecsSinceEpoch(); };
}

QString ReporterFeed::newKey() {
    QByteArray b(12, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(b.data()), 3);
    return QString::fromLatin1(b.toHex());
}

QString ReporterFeed::hashKey(const QString& key) {
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
}

int ReporterFeed::freeSlot() const {
    for (int i = 0; i < config.portCount; ++i) {
        bool used = false;
        for (const Session& s : m_sessions) if (s.active() && s.slot == i) { used = true; break; }
        if (!used) return i;
    }
    return -1;
}

QString ReporterFeed::hostForUrls() const {
    if (!config.host.isEmpty()) return config.host;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal()) return a.toString();
    return "127.0.0.1";
}

QString ReporterFeed::enginePath(const Session& s) const {
    if (config.mode == "mediamtx") return config.mediamtxPullBase + "/" + config.app + "/" + s.id;
    return QString("udp://127.0.0.1:%1").arg(udpPort(s));
}

QString ReporterFeed::publishUrl(const Session& s, const QString& key) const {
    if (config.mode == "mediamtx") return config.mediamtxPublishBase + "/" + config.app + "/" + key;
    return QString("rtmp://%1:%2/%3/%4").arg(hostForUrls()).arg(rtmpPort(s)).arg(config.app, key);
}

QString ReporterFeed::whipUrl(const Session& s, const QString& key) const {
    Q_UNUSED(s);
    if (config.mode != "mediamtx" || config.mediamtxWhipBase.isEmpty()) return QString();
    return config.mediamtxWhipBase + "/" + config.app + "/" + key + "/whip";
}

QStringList ReporterFeed::ffmpegArgs(const Session& s, const QString& key) const {
    // One publisher at a time on this port (-listen 1); the key is the RTMP stream name (ffmpeg only warns about another
    // name: Ingest::onOutput drops such a publisher). Copy the phone's H.264/AAC into TS; the engine opens the UDP side.
    // Two outputs from the one input: the monitors' copy first, the engine's own port last (a UDP port takes one reader, and GO
    // must be able to open the engine's while the On Air wall shows this feed).
    const QStringList copy = {"-c", "copy", "-f", "mpegts", "-muxdelay", "0", "-flush_packets", "1"};
    QStringList a = {"-nostdin", "-hide_banner", "-loglevel", "info", "-listen", "1", "-rw_timeout", "10000000",
                     "-i", QString("rtmp://0.0.0.0:%1/%2/%3").arg(rtmpPort(s)).arg(config.app, key)};
    a << copy << QString("udp://127.0.0.1:%1?pkt_size=1316").arg(monitorUdpPort(s));
    a << copy << QString("udp://127.0.0.1:%1?pkt_size=1316").arg(udpPort(s));
    return a;
}

QJsonObject ReporterFeed::inputJson(const Session& s) const {
    QJsonObject o;
    o["name"] = "Reporter: " + s.reporter; o["type"] = "stream"; o["url"] = enginePath(s); o["path"] = "live://reporter/" + s.id;
    o["sessionId"] = s.id; o["status"] = s.status; o["feedUp"] = s.feedUp; o["channel"] = s.channel; o["title"] = s.title;
    if (!monitorPath(s).isEmpty()) o["monitorUrl"] = monitorPath(s);
    return o;
}

QJsonArray ReporterFeed::inputsJson() const {
    QJsonArray a; for (const Session& s : sessions(false)) a.append(inputJson(s)); return a;
}

Session ReporterFeed::session(const QString& id, bool* found) const {
    auto it = m_sessions.constFind(id); if (found) *found = it != m_sessions.constEnd();
    return it != m_sessions.constEnd() ? it.value() : Session();
}

QList<Session> ReporterFeed::sessions(bool includeEnded) const {
    QList<Session> out;
    for (const QString& id : m_order) { const Session& s = m_sessions[id]; if (includeEnded || s.active()) out.append(s); }
    return out;
}

QJsonArray ReporterFeed::sessionsJson(bool includeEnded) const {
    QJsonArray a; for (const Session& s : sessions(includeEnded)) a.append(s.toJson()); return a;
}

void ReporterFeed::emitSession(const QString& event, const Session& s) {
    if (!broadcast) return;
    QJsonObject p; p["event"] = event; p["id"] = s.id; p["session"] = s.toJson(); p["remainingSec"] = s.remainingSec; p["status"] = s.status;
    broadcast(event, p);
}

QJsonObject ReporterFeed::create(const QJsonObject& body, QString* err) {
    Session s;
    s.reporter = body.value("reporter").toString().trimmed().left(80);
    if (s.reporter.isEmpty()) { if (err) *err = "reporter (name) required"; return QJsonObject(); }
    s.title = body.value("title").toString().trimmed().left(200);
    if (s.title.isEmpty()) s.title = "LIVE";
    s.channel = body.value("channel").toString(body.value("channel_id").toString()).trimmed();
    s.countdownSec = qBound(0, body.contains("countdownSec") ? body.value("countdownSec").toInt() : body.contains("countdown_sec") ? int(body.value("countdown_sec").toDouble()) : config.defaultCountdownSec, 600);
    s.slot = freeSlot();
    if (s.slot < 0) { if (err) *err = QString("all %1 ingest ports are in use: end a session first").arg(config.portCount); return QJsonObject(); }
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    while (m_sessions.contains(s.id)) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    s.createdMs = nowMs();
    const QString key = newKey();
    s.keyHash = hashKey(key);
    m_sessions.insert(s.id, s); m_keys.insert(s.id, key); m_order.append(s.id);
    if (saveInput) saveInput(s);
    if (startIngest) startIngest(s, key);
    emitSession("session", s);
    QJsonObject o = s.toJson();
    o["publishUrl"] = publishUrl(s, key); o["streamKey"] = key; o["enginePath"] = enginePath(s);
    const QString whip = whipUrl(s, key); if (!whip.isEmpty()) o["whipUrl"] = whip;
    o["keyShownOnce"] = true;
    return o;
}

bool ReporterFeed::goLiveNow(Session& s, QString* err) {
    if (takeToProgramAsync) {
        if (config.mode == "ffmpeg" && !s.feedUp) {
            s.status = "idle"; s.remainingSec = 0; s.lastError = "reporter feed is not publishing";
            if (err) *err = s.lastError;
            emitSession("session", s);
            return false;
        }
        s.status = "taking"; s.remainingSec = 0; s.lastError.clear();
        const QString id = s.id;
        const quint64 generation = ++m_takeGeneration[id];
        emitSession("session", s);
        takeToProgramAsync(s, [this, id, generation](bool ok, const QString& why) {
            auto it = m_sessions.find(id);
            if (it == m_sessions.end() || it->status != "taking" || m_takeGeneration.value(id) != generation) return;
            Session& current = it.value();
            if (!ok) {
                current.status = "idle"; current.lastError = why.isEmpty() ? "take failed" : why;
                if (alert) alert(current.channel, "error", "Reporter feed take failed (" + current.reporter + "): " + current.lastError);
                emitSession("session", current);
                return;
            }
            current.status = "live"; current.liveMs = nowMs(); current.lastError.clear();
            emitSession("live", current);
        });
        return true;
    }
    QString why;
    if (takeToProgram && !takeToProgram(s, &why)) {
        s.status = "idle"; s.remainingSec = 0; s.lastError = why.isEmpty() ? "take failed" : why;
        if (alert) alert(s.channel, "error", "Reporter feed take failed (" + s.reporter + "): " + s.lastError);
        if (err) *err = s.lastError;
        emitSession("session", s);
        return false;
    }
    s.status = "live"; s.remainingSec = 0; s.liveMs = nowMs(); s.lastError.clear();
    if (alert) alert(s.channel, "info", "Reporter feed on air: " + s.reporter);
    emitSession("live", s);
    return true;
}

bool ReporterFeed::go(const QString& id, int countdownSec, QString* err) {
    auto it = m_sessions.find(id); if (it == m_sessions.end()) { if (err) *err = "no such session"; return false; }
    Session& s = it.value();
    if (s.status == "live" || s.status == "taking") return true; // idempotent while pending
    if (s.status == "countdown") return true;            // already counting: ignore
    if (s.status == "ended") { if (err) *err = "session has ended"; return false; }
    if (s.channel.isEmpty()) { if (err) *err = "session has no channel"; return false; }
    if (countdownSec >= 0) s.countdownSec = qBound(0, countdownSec, 600);
    s.goMs = nowMs();
    if (s.countdownSec == 0) return goLiveNow(s, err);
    s.status = "countdown"; s.remainingSec = s.countdownSec;
    emitSession("countdown", s);
    return true;
}

bool ReporterFeed::takeNow(const QString& id, QString* err) {
    auto it = m_sessions.find(id); if (it == m_sessions.end()) { if (err) *err = "no such session"; return false; }
    Session& s = it.value();
    if (s.status == "live" || s.status == "taking") return true;
    if (s.status == "ended") { if (err) *err = "session has ended"; return false; }
    if (s.channel.isEmpty()) { if (err) *err = "session has no channel"; return false; }
    if (!s.goMs) s.goMs = nowMs();
    return goLiveNow(s, err);
}

bool ReporterFeed::cancel(const QString& id, QString* err) {
    auto it = m_sessions.find(id); if (it == m_sessions.end()) { if (err) *err = "no such session"; return false; }
    Session& s = it.value();
    if (s.status != "countdown") { if (err) *err = "not counting down"; return false; }
    s.status = "idle"; s.remainingSec = 0;
    emitSession("cancel", s);
    return true;
}

bool ReporterFeed::end(const QString& id, QString* err) {
    auto it = m_sessions.find(id); if (it == m_sessions.end()) { if (err) *err = "no such session"; return false; }
    Session& s = it.value();
    if (s.status == "ended") return true;
    if (s.status == "taking") { if (err) *err = "take is still pending"; return false; }
    const bool wasLive = s.status == "live";
    s.status = "ended"; s.remainingSec = 0; s.endedMs = nowMs(); s.feedUp = false;
    if (wasLive && releaseProgram) releaseProgram(s);
    if (stopIngest) stopIngest(s);
    if (removeInput) removeInput(s);
    m_keys.remove(id);
    emitSession("ended", s);
    return true;
}

bool ReporterFeed::remove(const QString& id) {
    if (!m_sessions.contains(id)) return false;
    if (!end(id, nullptr)) return false;
    m_takeGeneration.remove(id);
    m_sessions.remove(id); m_order.removeAll(id);
    return true;
}

void ReporterFeed::noteFeed(const QString& id, bool up, const QString& error) {
    auto it = m_sessions.find(id); if (it == m_sessions.end()) return;
    Session& s = it.value();
    const bool changed = s.feedUp != up || (!error.isEmpty() && s.lastError != error);
    s.feedUp = up; if (!error.isEmpty()) s.lastError = error;
    if (!up && s.active()) ++s.ingestRestarts;
    if (changed) emitSession(up ? "feedUp" : "feedDown", s);
}

void ReporterFeed::noteTakeFailure(const QString& id, const QString& error) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end() || (it->status != "taking" && it->status != "live")) return;
    it->status = "idle"; it->liveMs = 0;
    it->lastError = error.isEmpty() ? "engine failed to play reporter feed" : error;
    emitSession("session", *it);
}

void ReporterFeed::tick() {
    for (const QString& id : m_order) {
        auto it = m_sessions.find(id); if (it == m_sessions.end() || it->status != "countdown") continue;
        Session& s = it.value();
        s.remainingSec = qMax(0, s.remainingSec - 1);
        if (s.remainingSec > 0) { emitSession("countdown", s); continue; }
        goLiveNow(s, nullptr);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Supervisor wiring
// ---------------------------------------------------------------------------------------------------------------------
namespace {

QPointer<ReporterFeed> g_feed;
const char* kConfigKey = "reporter.config";

QString ffmpegExe(const Config& c) {
    if (!c.ffmpegPath.isEmpty()) return c.ffmpegPath;
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (QFileInfo::exists(local)) return local;
    const QString onPath = QStandardPaths::findExecutable("ffmpeg");
    return onPath.isEmpty() ? "ffmpeg" : onPath;
}

/// One ffmpeg.exe RTMP listener per session; restarts when the publisher leaves while the session is still active.
class Ingest : public QObject {
public:
    Ingest(ReporterFeed* feed, QObject* parent) : QObject(parent), m_feed(feed) {}

    void start(const Session& s, const QString& key) {
        stop(s.id);
        Proc* p = new Proc; p->id = s.id; p->key = key; p->args = m_feed->ffmpegArgs(s, key);
        m_procs.insert(s.id, p);
        launch(p);
    }
    void stop(const QString& id) {
        Proc* p = m_procs.take(id); if (!p) return;
        p->closing = true;
        if (p->proc) { p->proc->disconnect(); p->proc->kill(); p->proc->waitForFinished(1500); p->proc->deleteLater(); p->proc = nullptr; }
        delete p;
    }
    void stopAll() { const QStringList ids = m_procs.keys(); for (const QString& id : ids) stop(id); }

private:
    struct Proc { QString id, key; QStringList args; QProcess* proc = nullptr; bool closing = false, rejected = false; qint64 startedMs = 0; };

    // Put a listener into a kill-on-close job (as MuxManager::adopt): a crashed or killed Supervisor must not leave ffmpeg holding
    // the RTMP ports, or the next Supervisor's sessions only get "is the RTMP port free?" (seen 2026-11-02: two orphans after a restart).
    void adopt(QProcess* p) {
#ifdef Q_OS_WIN
        if (!p || p->processId() == 0) return;
        if (!m_job) {
            HANDLE h = CreateJobObjectW(nullptr, nullptr);
            if (h) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(h, JobObjectExtendedLimitInformation, &li, sizeof(li)); m_job = h; }
        }
        if (!m_job) return;
        HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(p->processId()));
        if (ph) { AssignProcessToJobObject(static_cast<HANDLE>(m_job), ph); CloseHandle(ph); }
#else
        Q_UNUSED(p);
#endif
    }

    void launch(Proc* p) {
        const Config& c = m_feed->config;
        if (c.mode != "ffmpeg") return;
        QProcess* q = new QProcess(this);
        q->setProcessChannelMode(QProcess::MergedChannels);
        p->proc = q; p->startedMs = QDateTime::currentMSecsSinceEpoch();
        const QString id = p->id;
        connect(q, &QProcess::readyReadStandardOutput, this, [this, id, q] { onOutput(id, q); });
        connect(q, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, id](int code, QProcess::ExitStatus) { onFinished(id, code); });
        connect(q, &QProcess::started, this, [this, q] { adopt(q); });
        q->start(ffmpegExe(c), p->args);
        // The command line holds the stream key: it is never logged (only the port is).
        qInfo().noquote() << QString("[reporter] ingest %1 listening on rtmp port %2").arg(id).arg(portOf(p->args));
    }
    static QString portOf(const QStringList& args) { const int i = args.indexOf("-i"); const QString u = i >= 0 && i + 1 < args.size() ? args[i + 1] : ""; const int a = u.indexOf(':', 7), b = u.indexOf('/', 8); return a > 0 && b > a ? u.mid(a + 1, b - a - 1) : "?"; }
    void onOutput(const QString& id, QProcess* q) {
        Proc* p = m_procs.value(id); if (!p || p->proc != q) return;
        const QByteArray out = q->readAllStandardOutput();
        // ffmpeg's listener only WARNS about a publish with another stream name and ingests it anyway (checked with
        // ffmpeg 9: "Unexpected stream X, expecting <key>" then Input #0): the key check is ours. Drop that publisher at
        // once by ending the listener (relaunched below); the warning line is never logged (it names the real key).
        if (out.contains("Unexpected stream")) { p->rejected = true; m_feed->noteFeed(id, false, "a publisher with a wrong stream key was refused"); q->kill(); return; }
        if (out.contains("Input #0")) m_feed->noteFeed(id, true);
    }
    void onFinished(const QString& id, int code) {
        Proc* p = m_procs.value(id); if (!p || p->closing) return;
        bool found = false; const Session s = m_feed->session(id, &found);
        if (!found || !s.active()) { stop(id); return; }
        const qint64 ran = QDateTime::currentMSecsSinceEpoch() - p->startedMs;
        const bool rejected = p->rejected; p->rejected = false;
        if (!rejected) m_feed->noteFeed(id, false, code != 0 && ran < 2000 ? QString("ingest exited at once (code %1): is the RTMP port free?").arg(code) : QString());
        if (p->proc) { p->proc->deleteLater(); p->proc = nullptr; }
        // Back-off: a port conflict exits immediately; do not spin. A refused publisher gets the listener back at once.
        QTimer::singleShot(rejected ? 300 : ran < 2000 ? 5000 : 500, this, [this, id] { Proc* p2 = m_procs.value(id); if (p2 && !p2->proc) launch(p2); });
    }

    ReporterFeed* m_feed;
    QHash<QString, Proc*> m_procs;
    void* m_job = nullptr;          // kill-on-close job object (Windows)
};

} // namespace

QJsonArray inputsJson() { return g_feed ? g_feed->inputsJson() : QJsonArray(); }
static sched::ScheduleStore* g_reporterStore = nullptr;

void noteAsRunFailure(const QString& channel, const QString& assetId, const QString& reason) {
    if (!g_feed || !g_reporterStore || assetId.isEmpty()) return;
    for (const Session& s : g_feed->sessions(false)) {
        if (s.channel != channel) continue;
        bool found = false;
        const MediaRecord media = g_reporterStore->mediaByPath("live://reporter/" + s.id, &found);
        if (found && media.id == assetId) g_feed->noteTakeFailure(s.id, reason);
    }
}


void install(const laneext::Context& ctx) {
    if (!ctx.rest || !ctx.store) { qWarning() << "[reporter] not installed: no REST server or schedule store"; return; }
    auto* feed = new ReporterFeed(ctx.parent);
    g_feed = feed;
    sched::ScheduleStore* store = ctx.store;
    g_reporterStore = store;
    // Sessions live in memory only: the library entries a previous run left behind point at ports nobody listens on any more
    // (they showed up as "Reporter: x" in the input pickers for ever). Start clean.
    for (const MediaRecord& m : store->mediaByType({"live"}, true)) if (m.path.startsWith("live://reporter/")) store->deleteMedia(m.id);
    feed->config = Config::fromJson(store->settingJson(kConfigKey));
    auto* ingest = new Ingest(feed, feed);
    auto* nam = new QNetworkAccessManager(feed);
    const int restPort = ctx.restPort;
    const auto restToken = ctx.restToken;
    const auto broadcast = ctx.broadcast;
    const auto alert = ctx.alert;

    // --- hooks -------------------------------------------------------------------------------------------------------
    feed->startIngest = [ingest](const Session& s, const QString& key) { ingest->start(s, key); };
    feed->stopIngest = [ingest](const Session& s) { ingest->stop(s.id); };
    feed->saveInput = [store, feed](const Session& s) {
        MediaRecord m; m.path = "live://reporter/" + s.id; m.type = "live"; m.title = "Reporter: " + s.reporter; m.durationSec = 1800;
        QJsonObject src; src["type"] = "stream"; src["url"] = feed->enginePath(s); src["realtime"] = true; src["lowLatency"] = true;
        if (!feed->monitorPath(s).isEmpty()) src["monitorUrl"] = feed->monitorPath(s);       // monitors watch their own copy (see monitorPath)
        m.extra["liveSource"] = src; m.extra["reporterSession"] = s.id;
        bool found = false; const MediaRecord ex = store->mediaByPath(m.path, &found); if (found) m.id = ex.id;
        store->saveMedia(m);
    };
    feed->removeInput = [store](const Session& s) { bool found = false; const MediaRecord ex = store->mediaByPath("live://reporter/" + s.id, &found); if (found) store->deleteMedia(ex.id); };
    auto transportPost = [nam, restPort, restToken](const QString& channel, const QJsonObject& body) {
        QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/api/v1/channels/%2/transport").arg(restPort).arg(channel)));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setTransferTimeout(5000);
        const QString tok = restToken ? restToken() : QString(); if (!tok.isEmpty()) req.setRawHeader("Authorization", ("Bearer " + tok).toUtf8());
        QNetworkReply* r = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QObject::connect(r, &QNetworkReply::finished, r, &QNetworkReply::deleteLater);
        return r;
    };
    // Wait for the transport response before advancing the reporter session; a delivered command is
    // accepted, not proof of an on-air picture. Engine state remains the authoritative confirmation.
    feed->takeToProgramAsync = [store, transportPost](const Session& s, std::function<void(bool, const QString&)> done) {
        bool f = false; store->channel(s.channel, &f);
        if (!f) { done(false, "no such channel " + s.channel); return; }
        bool mf = false; const MediaRecord m = store->mediaByPath("live://reporter/" + s.id, &mf);
        QJsonObject b; b["action"] = "take"; b["path"] = "live://reporter/" + s.id; b["title"] = "Reporter: " + s.reporter; if (mf) b["mediaId"] = m.id;
        QNetworkReply* r = transportPost(s.channel, b);
        QObject::connect(r, &QNetworkReply::finished, r, [r, done] {
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = r->readAll();
            const QJsonObject result = QJsonDocument::fromJson(body).object();
            const bool ok = r->error() == QNetworkReply::NoError && status == 200 && result.value("accepted").toBool();
            QString why = result.value("error").toString();
            if (why.isEmpty() && !ok) why = r->errorString();
            done(ok, why);
        });
    };
    feed->releaseProgram = [transportPost](const Session& s) { QJsonObject b; b["action"] = "skip"; transportPost(s.channel, b); };
    feed->broadcast = [broadcast](const QString& event, const QJsonObject& payload) { if (broadcast) { QJsonObject p = payload; p["event"] = event; broadcast("reporter", p); } };
    feed->alert = [alert](const QString& ch, const QString& level, const QString& msg) { if (alert) alert(ch, level, msg); };

    auto* timer = new QTimer(feed); timer->setInterval(1000); QObject::connect(timer, &QTimer::timeout, feed, &ReporterFeed::tick); timer->start();
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, ingest, [ingest] { ingest->stopAll(); });

    // --- routes ------------------------------------------------------------------------------------------------------
    RestApiServer* r = ctx.rest;
    using Q = QHash<QString, QString>;
    auto err = [](int status, const QString& m) { QJsonObject o; o["error"] = m; o["status"] = status; return o; };
    auto sessionReply = [feed, err](const QString& id, bool ok, const QString& why) { if (!ok) return err(feed->exists(id) ? 409 : 404, why); QJsonObject o = feed->session(id).toJson(); o["ok"] = true; return o; };

    r->registerRoute("GET", "/api/v1/reporter/config", [feed](const QJsonObject&, const Q&) { QJsonObject o = feed->config.toJson(); o["host"] = feed->hostForUrls(); o["hostConfigured"] = feed->config.host; return o; });
    r->registerRoute("PUT", "/api/v1/reporter/config", [feed, store](const QJsonObject& body, const Q&) {
        feed->config = Config::fromJson(body, feed->config); store->setSettingJson(kConfigKey, feed->config.toJson());
        QJsonObject o = feed->config.toJson(); o["saved"] = true; o["note"] = "ports and mode apply to sessions created from now on"; return o;
    });
    r->registerRoute("GET", "/api/v1/reporter/sessions", [feed](const QJsonObject&, const Q& q) {
        QJsonObject o; o["sessions"] = feed->sessionsJson(q.value("all") == "1" || q.value("all") == "true"); o["host"] = feed->hostForUrls(); o["mode"] = feed->config.mode; return o;
    });
    r->registerRoute("POST", "/api/v1/reporter/sessions", [feed, err](const QJsonObject& body, const Q&) { QString why; const QJsonObject o = feed->create(body, &why); return o.isEmpty() ? err(400, why) : o; });
    r->registerRoute("GET", "/api/v1/reporter/sessions/{id}", [feed, err](const QJsonObject&, const Q& q) { bool f = false; const Session s = feed->session(q["id"], &f); return f ? s.toJson() : err(404, "no such session"); });
    r->registerRoute("DELETE", "/api/v1/reporter/sessions/{id}", [feed, err](const QJsonObject&, const Q& q) { if (!feed->remove(q["id"])) return err(feed->exists(q["id"]) ? 409 : 404, feed->exists(q["id"]) ? "take is still pending" : "no such session"); QJsonObject o; o["deleted"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/reporter/sessions/{id}/go", [feed, sessionReply](const QJsonObject& body, const Q& q) {
        QString why; const bool ok = feed->go(q["id"], body.contains("countdownSec") ? body.value("countdownSec").toInt() : -1, &why); return sessionReply(q["id"], ok, why);
    });
    r->registerRoute("POST", "/api/v1/reporter/sessions/{id}/take-now", [feed, sessionReply](const QJsonObject&, const Q& q) { QString why; return sessionReply(q["id"], feed->takeNow(q["id"], &why), why); });
    r->registerRoute("POST", "/api/v1/reporter/sessions/{id}/cancel", [feed, sessionReply](const QJsonObject&, const Q& q) { QString why; return sessionReply(q["id"], feed->cancel(q["id"], &why), why); });
    r->registerRoute("POST", "/api/v1/reporter/sessions/{id}/end", [feed, sessionReply](const QJsonObject&, const Q& q) { QString why; return sessionReply(q["id"], feed->end(q["id"], &why), why); });

    // --- LiveCast app compatibility (its PlayPlayApi.kt speaks the OBS-era /api/field contract, snake_case) -------------
    // The app's station password login (/login cookie) is not provided here: a Supervisor with a station password answers
    // these paths with 401 unless the app sends X-PP-Pass; without one, loopback/LAN calls pass as every /api/ station path.
    auto legacy = [feed](const Session& s) {
        QJsonObject o; o["id"] = s.id; o["channel_id"] = s.channel; o["reporter"] = s.reporter; o["lower_third1"] = s.reporter; o["lower_third2"] = s.title;
        o["bug_text"] = ""; o["last_error"] = s.lastError; o["slot"] = QString("reporter%1").arg(s.slot); o["status"] = s.status; o["countdown_sec"] = double(s.countdownSec); o["transport"] = "rtmp"; o["fb_stream"] = "0";
        return o;
    };
    r->registerRoute("GET", "/api/field/sessions", [feed, store, legacy](const QJsonObject&, const Q& q) {
        QJsonArray a; for (const Session& s : feed->sessions(q.value("all") == "1")) a.append(legacy(s));
        QJsonArray chans; for (const ChannelProfile& c : store->channels()) if (c.enabled) chans.append(c.id);
        QJsonObject o; o["sessions"] = a; o["stream"] = QJsonObject(); o["channels"] = chans; return o;
    });
    r->registerRoute("POST", "/api/field/sessions", [feed, legacy, err](const QJsonObject& body, const Q&) {
        QJsonObject b; b["reporter"] = body.value("reporter"); b["title"] = body.value("lower_third2").toString(); b["channel"] = body.value("channel_id").toString(); b["countdown_sec"] = body.value("countdown_sec");
        QString why; const QJsonObject c = feed->create(b, &why); if (c.isEmpty()) return err(400, why);
        QJsonObject o = legacy(feed->session(c.value("id").toString())); o["publish_url"] = c.value("publishUrl"); return o;   // once, like the v1 create
    });
    r->registerRoute("PATCH", "/api/field/sessions/{id}", [feed, legacy, err](const QJsonObject&, const Q& q) { bool f = false; const Session s = feed->session(q["id"], &f); return f ? legacy(s) : err(404, "no such session"); });
    r->registerRoute("POST", "/api/field/sessions/{id}/golive", [feed, legacy, err](const QJsonObject& body, const Q& q) {
        QString why; const bool ok = feed->go(q["id"], body.value("skip_countdown").toBool() ? 0 : -1, &why); return ok ? legacy(feed->session(q["id"])) : err(400, why);
    });
    auto okOr = [feed, err](bool ok, const QString& why) { if (!ok) return err(400, why); QJsonObject o; o["ok"] = true; return o; };
    r->registerRoute("POST", "/api/field/sessions/{id}/take-now", [feed, okOr](const QJsonObject&, const Q& q) { QString why; return okOr(feed->takeNow(q["id"], &why), why); });
    r->registerRoute("POST", "/api/field/sessions/{id}/cancel", [feed, okOr](const QJsonObject&, const Q& q) { QString why; return okOr(feed->cancel(q["id"], &why), why); });
    r->registerRoute("POST", "/api/field/sessions/{id}/end", [feed, okOr](const QJsonObject&, const Q& q) { QString why; return okOr(feed->end(q["id"], &why), why); });
    r->registerRoute("POST", "/api/field/sessions/{id}/heartbeat", [feed, okOr](const QJsonObject&, const Q& q) { return okOr(feed->exists(q["id"]), "no such session"); });

    qInfo().noquote() << QString("[reporter] routes installed; mode %1, rtmp ports %2-%3, host %4").arg(feed->config.mode).arg(feed->config.rtmpPortBase).arg(feed->config.rtmpPortBase + feed->config.portCount - 1).arg(feed->hostForUrls());
}

} // namespace reporterfeed

static laneext::Registrar s_reporterFeed("reporterfeed", 110, [](const laneext::Context& ctx) { reporterfeed::install(ctx); });
