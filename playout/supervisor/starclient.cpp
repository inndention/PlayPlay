#include "starclient.h"
#include "../schedule/schedulestore.h"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QDebug>

using namespace sched;

StarClient::StarClient(ScheduleStore* store, const QString& downloadRoot, QObject* parent) : QObject(parent), m_store(store), m_downloadRoot(downloadRoot) {
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, [this]() { emit jobsChanged(); });
    m_home = m_store ? m_store->setting("star.home", "D:/StarMedia") : "D:/StarMedia";
    load();
    discoverLocal();
}

StarClient::~StarClient() { for (Job* j : m_jobs) { if (j->reply) j->reply->abort(); delete j->file; delete j; } }

// ---------------------------------------------------------------------------------------------
// Agents
// ---------------------------------------------------------------------------------------------
void StarClient::load() {
    if (!m_store) return;
    for (const QJsonValue& v : m_store->settingJson("star.agents").value("agents").toArray()) {
        const QJsonObject o = v.toObject();
        Agent a; a.node = o.value("node").toString(); a.name = o.value("name").toString(a.node); a.url = o.value("url").toString(); a.token = o.value("token").toString(); a.configured = true;
        if (!a.node.isEmpty() && !a.url.isEmpty()) m_agents[a.node] = a;
    }
}

void StarClient::persist() {
    if (!m_store) return;
    QJsonArray arr;
    for (const Agent& a : m_agents) if (a.configured) { QJsonObject o; o["node"] = a.node; o["name"] = a.name; o["url"] = a.url; o["token"] = a.token; arr.append(o); }
    QJsonObject s; s["agents"] = arr; m_store->setSettingJson("star.agents", s);
    emit agentsChanged();
}

QJsonObject StarClient::discoverLocal() {
    QJsonObject report; report["home"] = m_home;
    int added = 0;
    // The local daemon: token file + agent.json port.
    const QString tokenPath = QDir(m_home).filePath("StarAgent/agent-token");
    QFile tf(tokenPath);
    if (tf.open(QIODevice::ReadOnly)) {
        const QString token = QString::fromUtf8(tf.readAll()).trimmed();
        int port = 8780;
        QFile cf(QDir(m_home).filePath("StarAgent/agent.json"));
        if (cf.open(QIODevice::ReadOnly)) port = QJsonDocument::fromJson(cf.readAll()).object().value("port").toInt(8780);
        Agent& a = m_agents["local"];
        if (a.node.isEmpty()) { a.node = "local"; a.name = "this PC"; ++added; }
        a.url = QString("http://127.0.0.1:%1").arg(port); if (!token.isEmpty()) a.token = token;
        report["localAgent"] = a.url;
    }
    // The registry: nodes with an `agent.url` (tokens are secret refs there, so they stay empty until pasted).
    QFile nf(QDir(m_home).filePath("mesh/nodes.json"));
    if (nf.open(QIODevice::ReadOnly)) {
        QByteArray raw = nf.readAll(); if (raw.startsWith("\xEF\xBB\xBF")) raw.remove(0, 3);
        const QJsonArray nodes = QJsonDocument::fromJson(raw).object().value("nodes").toArray();
        QJsonArray seen;
        for (const QJsonValue& v : nodes) {
            const QJsonObject n = v.toObject(); const QString id = n.value("id").toString();
            const QJsonObject ag = n.value("agent").toObject();
            QJsonObject s; s["id"] = id; s["name"] = n.value("name").toString(id); s["ip"] = n.value("ip").toString(); s["roles"] = n.value("roles"); s["agentUrl"] = ag.value("url").toString(); s["playout"] = n.value("playout"); s["obs"] = n.value("obs");
            seen.append(s);
            if (ag.value("url").toString().isEmpty() || n.value("self").toBool()) continue;
            if (!m_agents.contains(id)) { Agent a; a.node = id; a.name = n.value("name").toString(id); a.url = ag.value("url").toString(); m_agents[id] = a; ++added; }
        }
        report["registryNodes"] = seen;
    }
    report["added"] = added;
    if (added) emit agentsChanged();
    return report;
}

QJsonArray StarClient::agents() const {
    QJsonArray a;
    for (const Agent& x : m_agents) { QJsonObject o; o["node"] = x.node; o["name"] = x.name; o["url"] = x.url; o["hasToken"] = !x.token.isEmpty(); o["configured"] = x.configured; o["online"] = x.online; o["version"] = x.version; o["hostname"] = x.hostname; o["error"] = x.error; o["seenMs"] = double(x.seenMs); a.append(o); }
    return a;
}

void StarClient::setAgents(const QJsonArray& agents) {
    QHash<QString, Agent> next;
    for (const QJsonValue& v : agents) {
        const QJsonObject o = v.toObject(); const QString node = o.value("node").toString();
        if (node.isEmpty()) continue;
        Agent a = m_agents.value(node); a.node = node; a.name = o.value("name").toString(a.name.isEmpty() ? node : a.name); a.url = o.value("url").toString(a.url);
        if (!o.value("token").toString().isEmpty()) a.token = o.value("token").toString();
        a.configured = true; next[node] = a;
    }
    // keep discovered-but-unconfigured agents
    for (const Agent& a : m_agents) if (!a.configured && !next.contains(a.node)) next[a.node] = a;
    m_agents = next;
    persist();
}

const StarClient::Agent* StarClient::agentFor(const QString& node) const {
    auto it = m_agents.constFind(node); if (it != m_agents.cend()) return &it.value();
    for (auto j = m_agents.cbegin(); j != m_agents.cend(); ++j) if (j.value().name.compare(node, Qt::CaseInsensitive) == 0 || j.value().hostname.compare(node, Qt::CaseInsensitive) == 0) return &j.value();
    return nullptr;
}

QNetworkRequest StarClient::request(const Agent& a, const QString& path, int timeoutMs) const {
    QNetworkRequest r(QUrl(a.url + path));
    r.setTransferTimeout(timeoutMs);
    r.setRawHeader("Accept", "application/json");
    if (!a.token.isEmpty()) r.setRawHeader("Authorization", ("Bearer " + a.token).toUtf8());
    return r;
}

QJsonObject StarClient::unwrap(const QByteArray& data, int http, const QString& netError, int* statusOut) {
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    if (http == 0) { *statusOut = 502; QJsonObject e; e["error"] = "agent unreachable: " + netError; return e; }
    if (o.value("ok").toBool() && o.contains("data")) { *statusOut = 200; const QJsonValue d = o.value("data"); if (d.isObject()) return d.toObject(); QJsonObject w; w["data"] = d; return w; }
    const QJsonObject err = o.value("error").toObject();
    QJsonObject e; e["error"] = err.value("message").toString(http == 401 ? "agent token rejected (set the StarAgent token)" : QString("agent HTTP %1").arg(http)); e["code"] = err.value("code").toString();
    *statusOut = http >= 400 ? (http == 401 || http == 403 ? 401 : 502) : 502;
    return e;
}

void StarClient::call(const QString& node, const QString& method, const QString& path, const QJsonObject& body, Reply reply, int timeoutMs) {
    const Agent* a = agentFor(node);
    if (!a) { QJsonObject e; e["error"] = "unknown Star agent: " + node; reply(404, e); return; }
    QNetworkRequest req = request(*a, path, timeoutMs);
    QNetworkReply* r = nullptr;
    if (method == "GET") r = m_nam.get(req);
    else { req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact); r = method == "PUT" ? m_nam.put(req, payload) : method == "DELETE" ? m_nam.sendCustomRequest(req, "DELETE", payload) : m_nam.post(req, payload); }
    const QString key = a->node;
    connect(r, &QNetworkReply::finished, this, [this, r, reply, key]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        int st = 0; const QJsonObject o = unwrap(r->isOpen() ? r->readAll() : QByteArray(), http, r->errorString(), &st);
        if (m_agents.contains(key)) { Agent& a = m_agents[key]; a.online = st == 200 || st == 401; a.error = st == 200 ? QString() : o.value("error").toString(); if (st == 200) a.seenMs = QDateTime::currentMSecsSinceEpoch(); }
        reply(st, o);
    });
}

void StarClient::health(const QString& node, Reply reply) {
    call(node, "GET", "/health", QJsonObject(), [this, node, reply](int st, const QJsonObject& o) {
        if (st == 200 && m_agents.contains(node)) { m_agents[node].version = o.value("version").toString(); m_agents[node].hostname = o.value("hostname").toString(); }
        reply(st, o);
    }, 4000);
}
void StarClient::status(const QString& node, Reply reply) { call(node, "GET", "/status", QJsonObject(), reply, 8000); }
void StarClient::nowPlaying(const QString& node, Reply reply) { call(node, "GET", "/nowplaying", QJsonObject(), reply, 8000); }

void StarClient::list(const QString& node, const QString& path, Reply reply) {
    QUrlQuery q; q.addQueryItem("path", path); q.addQueryItem("limit", "5000");
    call(node, "GET", "/fs/list?" + q.toString(QUrl::FullyEncoded), QJsonObject(), [reply, node, path](int st, const QJsonObject& o) {
        if (st != 200) { reply(st, o); return; }
        // Normalise to the mesh browse shape: items[{name, path, isDirectory, size, mtime}]
        QJsonArray items;
        for (const QJsonValue& v : o.value("entries").toArray()) {
            const QJsonObject e = v.toObject(); QJsonObject i;
            const bool dir = e.value("type").toString() == "dir";
            const QString base = o.value("path").toString();
            i["name"] = e.value("name").toString(); i["isDirectory"] = dir; i["size"] = e.value("size"); i["mtime"] = e.value("mtime"); i["drive"] = e.value("drive").toBool(); i["free"] = e.value("free");
            i["path"] = base.isEmpty() ? e.value("name").toString() : (base.endsWith('\\') || base.endsWith('/') ? base + e.value("name").toString() : base + "\\" + e.value("name").toString());
            items.append(i);
        }
        QJsonObject r; r["node"] = node; r["currentPath"] = o.value("path").toString(path); r["parent"] = o.value("parent"); r["items"] = items; r["total"] = o.value("total"); r["truncated"] = o.value("truncated");
        reply(200, r);
    }, 20000);
}

void StarClient::search(const QString& node, const QString& path, const QString& q, Reply reply) {
    QUrlQuery qq; qq.addQueryItem("path", path.isEmpty() ? "C:\\" : path); qq.addQueryItem("q", q); qq.addQueryItem("max", "500"); qq.addQueryItem("timeMs", "15000"); qq.addQueryItem("type", "file");
    call(node, "GET", "/fs/search?" + qq.toString(QUrl::FullyEncoded), QJsonObject(), [reply, node](int st, const QJsonObject& o) {
        if (st != 200) { reply(st, o); return; }
        QJsonArray hits;
        const QJsonArray src = o.contains("entries") ? o.value("entries").toArray() : o.value("results").toArray();
        for (const QJsonValue& v : src) { QJsonObject h = v.toObject(); h["nodeId"] = node; h["isDirectory"] = h.value("type").toString() == "dir"; if (!h.contains("name")) h["name"] = QFileInfo(h.value("path").toString().replace('\\', '/')).fileName(); hits.append(h); }
        QJsonObject r; r["hits"] = hits; r["count"] = hits.size(); r["truncated"] = o.value("truncated"); reply(200, r);
    }, 25000);
}

void StarClient::probeAll(Reply reply) {
    const QStringList nodes = m_agents.keys();
    if (nodes.isEmpty()) { QJsonObject o; o["agents"] = QJsonArray(); reply(200, o); return; }
    auto left = std::make_shared<int>(nodes.size());
    for (const QString& n : nodes) health(n, [this, left, reply](int, const QJsonObject&) { if (--*left == 0) { QJsonObject o; o["agents"] = agents(); reply(200, o); emit agentsChanged(); } });
}

// ---------------------------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------------------------
QJsonObject StarClient::Job::toJson() const {
    QJsonObject o; o["id"] = id; o["kind"] = kind; o["via"] = "star"; o["node"] = node; o["nodeName"] = nodeName; o["remotePath"] = remotePath; o["localPath"] = QDir::toNativeSeparators(localPath);
    o["type"] = type; o["state"] = state; o["bytes"] = double(bytes); o["total"] = double(total); o["rateBps"] = rateBps; o["startedMs"] = double(startedMs); o["finishedMs"] = double(finishedMs); o["error"] = error;
    o["name"] = QFileInfo(QString(kind == "fetch" ? remotePath : localPath).replace('\\', '/')).fileName();
    return o;
}
QJsonArray StarClient::jobs() const { QJsonArray a; for (Job* j : m_jobs) a.append(j->toJson()); return a; }
bool StarClient::cancel(const QString& jobId) {
    for (Job* j : m_jobs) if (j->id == jobId) { if ((j->state == "running" || j->state == "queued") && j->reply) { j->reply->abort(); return true; } m_jobs.removeAll(j); delete j->file; delete j; notify(); return true; }
    return false;
}
void StarClient::clearFinished() { for (int i = m_jobs.size() - 1; i >= 0; --i) { Job* j = m_jobs[i]; if (j->state != "running" && j->state != "queued") { m_jobs.removeAt(i); delete j->file; delete j; } } notify(); }
void StarClient::finish(Job* j, const QString& state, const QString& error) {
    j->state = state; j->error = error; j->finishedMs = QDateTime::currentMSecsSinceEpoch(); j->reply = nullptr;
    if (j->file) { j->file->close(); delete j->file; j->file = nullptr; }
    if (state == "done") qInfo().noquote() << "[Star]" << j->kind << "done" << j->remotePath << j->bytes << "bytes"; else qWarning().noquote() << "[Star]" << j->kind << state << j->remotePath << error;
    notify();
}

QJsonObject StarClient::startFetch(const QString& node, const QString& path, const QString& type) {
    const Agent* a = agentFor(node);
    if (!a) { QJsonObject e; e["error"] = "unknown Star agent: " + node; e["status"] = 404; return e; }
    for (Job* j : m_jobs) if (j->kind == "fetch" && j->node == a->node && j->remotePath == path) { if (j->state == "running" || j->state == "queued" || j->state == "done") return j->toJson(); j->state = "queued"; j->error.clear(); runFetch(j); return j->toJson(); }
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->kind = "fetch"; j->node = a->node; j->nodeName = a->name; j->remotePath = path; j->type = type.isEmpty() ? "program" : type;
    QString safe = a->name; safe.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    const QString dir = QDir(m_downloadRoot).filePath(safe); QDir().mkpath(dir);
    j->localPath = QDir(dir).filePath(QFileInfo(QString(path).replace('\\', '/')).fileName());
    m_jobs.append(j); runFetch(j); return j->toJson();
}

void StarClient::runFetch(Job* j) {
    const Agent* a = agentFor(j->node); if (!a) { finish(j, "failed", "agent vanished"); return; }
    j->state = "running"; j->startedMs = QDateTime::currentMSecsSinceEpoch(); j->error.clear();
    const QString part = j->localPath + ".part"; j->partPath = part;
    j->file = new QFile(part);
    const bool resume = QFileInfo::exists(part) && QFileInfo(part).size() > 0;
    if (!j->file->open(resume ? (QIODevice::WriteOnly | QIODevice::Append) : QIODevice::WriteOnly)) { finish(j, "failed", "cannot write " + part); return; }
    const qint64 offset = resume ? j->file->size() : 0; j->bytes = offset;
    QUrlQuery q; q.addQueryItem("path", j->remotePath);
    QNetworkRequest req = request(*a, "/fs/download?" + q.toString(QUrl::FullyEncoded), 0);
    if (offset > 0) req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(offset) + "-");
    QNetworkReply* r = m_nam.get(req); j->reply = r;
    auto t0 = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch()); auto b0 = std::make_shared<qint64>(offset);
    connect(r, &QNetworkReply::metaDataChanged, this, [this, r, j, offset]() {
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); const qint64 len = r->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (http == 200 && offset > 0 && j->file) { j->file->close(); j->file->open(QIODevice::WriteOnly); j->bytes = 0; }
        if (len > 0) j->total = (http == 206 ? offset : 0) + len; notify();
    });
    connect(r, &QNetworkReply::readyRead, this, [this, r, j, t0, b0]() {
        if (!j->file) return; const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); if (http >= 400) return;
        const QByteArray chunk = r->readAll(); j->file->write(chunk); j->bytes += chunk.size();
        const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now - *t0 >= 1000) { j->rateBps = double(j->bytes - *b0) * 1000.0 / double(now - *t0); *t0 = now; *b0 = j->bytes; notify(); }
    });
    connect(r, &QNetworkReply::finished, this, [this, r, j]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() == QNetworkReply::OperationCanceledError) { finish(j, "cancelled", "cancelled"); return; }
        if (http >= 400) { int st = 0; const QJsonObject e = unwrap(r->isOpen() ? r->readAll() : QByteArray(), http, r->errorString(), &st); if (j->file) { j->file->close(); } finish(j, "failed", e.value("error").toString()); return; }
        if (r->error() != QNetworkReply::NoError) { finish(j, "failed", r->errorString()); return; }
        if (j->file) { j->file->close(); delete j->file; j->file = nullptr; }
        if (QFileInfo::exists(j->localPath) && QFileInfo(j->localPath).size() == QFileInfo(j->partPath).size()) {
            // Same file already here (possibly open by an engine): keep it, drop the fresh copy.
            QFile::remove(j->partPath); j->total = QFileInfo(j->localPath).size(); j->bytes = j->total; finish(j, "done"); emit fetched(j->localPath, j->type, j->node, j->remotePath); return;
        }
        if (!QFile::remove(j->localPath) && QFileInfo::exists(j->localPath)) {
            const QFileInfo fi(j->localPath); j->localPath = fi.dir().filePath(fi.completeBaseName() + "_" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + (fi.suffix().isEmpty() ? "" : "." + fi.suffix()));
        }
        if (!QFile::rename(j->partPath, j->localPath)) { finish(j, "failed", "rename failed"); return; }
        j->total = QFileInfo(j->localPath).size(); j->bytes = j->total; finish(j, "done");
        emit fetched(j->localPath, j->type, j->node, j->remotePath);
    });
    notify();
}

QJsonObject StarClient::startPush(const QString& localPath, const QString& node, const QString& remotePath, bool overwrite) {
    const Agent* a = agentFor(node);
    if (!a) { QJsonObject e; e["error"] = "unknown Star agent: " + node; e["status"] = 404; return e; }
    if (!QFileInfo(localPath).isFile()) { QJsonObject e; e["error"] = "no such file: " + localPath; e["status"] = 404; return e; }
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->kind = "push"; j->node = a->node; j->nodeName = a->name; j->localPath = localPath;
    j->remotePath = remotePath.isEmpty() ? QFileInfo(localPath).fileName() : (remotePath.endsWith('\\') || remotePath.endsWith('/') ? remotePath + QFileInfo(localPath).fileName() : remotePath);
    j->total = QFileInfo(localPath).size(); j->type = overwrite ? "overwrite" : "";
    m_jobs.append(j); runPush(j); return j->toJson();
}

void StarClient::runPush(Job* j) {
    j->state = "running"; j->startedMs = QDateTime::currentMSecsSinceEpoch();
    QJsonObject init; init["path"] = j->remotePath; init["size"] = double(j->total); init["mkdirs"] = true; init["overwrite"] = j->type == "overwrite"; init["chunkSize"] = 4 * 1024 * 1024;
    call(j->node, "POST", "/fs/upload/init", init, [this, j](int st, const QJsonObject& o) {
        if (st != 200) { finish(j, "failed", o.value("error").toString()); return; }
        j->uploadId = o.value("uploadId").toString(o.value("id").toString()); j->chunkSize = qint64(o.value("chunkSize").toDouble(4 * 1024 * 1024));
        if (j->uploadId.isEmpty()) { finish(j, "failed", "upload init returned no id"); return; }
        j->totalChunks = int((j->total + j->chunkSize - 1) / j->chunkSize);
        j->have.clear(); for (const QJsonValue& v : o.value("received").toArray()) j->have.append(v.toInt());
        j->chunk = 0; j->bytes = qint64(j->have.size()) * j->chunkSize;
        pushChunk(j);
    }, 30000);
    notify();
}

void StarClient::pushChunk(Job* j) {
    while (j->chunk < j->totalChunks && j->have.contains(j->chunk)) ++j->chunk;
    if (j->chunk >= j->totalChunks) {
        QJsonObject done; done["verify"] = false;
        call(j->node, "POST", "/fs/upload/" + j->uploadId + "/complete", done, [this, j](int st, const QJsonObject& o) {
            if (st != 200) { finish(j, "failed", "complete: " + o.value("error").toString()); return; }
            if (!o.value("path").toString().isEmpty()) j->remotePath = o.value("path").toString(); j->bytes = j->total; finish(j, "done");
        }, 600000);
        return;
    }
    const Agent* a = agentFor(j->node); if (!a) { finish(j, "failed", "agent vanished"); return; }
    QFile f(j->localPath); if (!f.open(QIODevice::ReadOnly)) { finish(j, "failed", "cannot read " + j->localPath); return; }
    f.seek(qint64(j->chunk) * j->chunkSize); const QByteArray data = f.read(j->chunkSize);
    QNetworkRequest req = request(*a, "/fs/upload/" + j->uploadId + "?index=" + QString::number(j->chunk), 180000);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    QNetworkReply* r = m_nam.put(req, data); j->reply = r; const int sz = data.size();
    connect(r, &QNetworkReply::finished, this, [this, r, j, sz]() {
        r->deleteLater(); j->reply = nullptr;
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() == QNetworkReply::OperationCanceledError) { finish(j, "cancelled", "cancelled"); return; }
        if (http != 200) { int st = 0; const QJsonObject e = unwrap(r->isOpen() ? r->readAll() : QByteArray(), http, r->errorString(), &st); finish(j, "failed", QString("chunk %1: %2").arg(j->chunk).arg(e.value("error").toString())); return; }
        j->have.append(j->chunk); ++j->chunk; j->bytes += sz;
        const qint64 el = QDateTime::currentMSecsSinceEpoch() - j->startedMs; if (el > 0) j->rateBps = double(j->bytes) * 1000.0 / double(el);
        notify(); pushChunk(j);
    });
}
