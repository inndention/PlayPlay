#include "meshclient.h"
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

static const qint64 kChunk = 2 * 1024 * 1024;

MeshClient::MeshClient(ScheduleStore* store, const QString& downloadRoot, QObject* parent)
    : QObject(parent), m_store(store), m_downloadRoot(downloadRoot) {
    m_notify.setSingleShot(true);
    connect(&m_notify, &QTimer::timeout, this, [this]() { emit jobsChanged(); });
    QJsonObject cfg;
    if (m_store) { cfg["url"] = m_store->setting("mesh.anfsUrl"); cfg["token"] = m_store->setting("mesh.anfsToken"); cfg["cookie"] = m_store->setting("mesh.anfsCookie"); }
    setConfig(cfg);
}

MeshClient::~MeshClient() { for (Job* j : m_jobs) { if (j->reply) j->reply->abort(); delete j->file; delete j; } }

void MeshClient::setConfig(const QJsonObject& cfg) {
    if (cfg.contains("url")) { m_baseUrl = cfg.value("url").toString().trimmed(); while (m_baseUrl.endsWith('/')) m_baseUrl.chop(1); if (m_store) m_store->setSetting("mesh.anfsUrl", m_baseUrl); }
    if (cfg.contains("token")) { m_token = cfg.value("token").toString().trimmed(); if (m_store) m_store->setSetting("mesh.anfsToken", m_token); }
    if (cfg.contains("cookie")) { m_cookie = cfg.value("cookie").toString().trimmed(); if (m_store) m_store->setSetting("mesh.anfsCookie", m_cookie); }
    // No default bridge: the mesh feature stays off until mesh.anfsUrl is configured.
}

QJsonObject MeshClient::status() const {
    QJsonObject o; o["url"] = m_baseUrl; o["hasToken"] = !m_token.isEmpty(); o["hasCookie"] = !m_cookie.isEmpty();
    o["lastOkMs"] = double(m_lastOkMs); o["lastError"] = m_lastError; o["nodes"] = m_nodeCount; o["downloadRoot"] = QDir::toNativeSeparators(m_downloadRoot);
    int active = 0; for (Job* j : m_jobs) if (j->state == "running" || j->state == "queued") ++active; o["activeJobs"] = active;
    return o;
}

QNetworkRequest MeshClient::request(const QString& path) const {
    QNetworkRequest r(QUrl(m_baseUrl + path));
    r.setTransferTimeout(20000);
    r.setRawHeader("Accept", "application/json");
    if (!m_token.isEmpty()) r.setRawHeader("Authorization", ("Bearer " + m_token).toUtf8());
    if (!m_cookie.isEmpty()) r.setRawHeader("Cookie", m_cookie.toUtf8());
    return r;
}

void MeshClient::getJson(const QString& path, Reply reply) {
    if (m_baseUrl.isEmpty()) { m_lastError = "mesh bridge not configured (set mesh.anfsUrl)"; QJsonObject e; e["error"] = m_lastError; reply(503, e); return; }
    QNetworkReply* r = m_nam.get(request(path));
    connect(r, &QNetworkReply::finished, this, [this, r, reply, path]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = r->isOpen() ? r->readAll() : QByteArray();
        if (r->error() != QNetworkReply::NoError && http == 0) { m_lastError = r->errorString(); QJsonObject e; e["error"] = "mesh bridge unreachable: " + r->errorString(); reply(502, e); return; }
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (http >= 400) { QJsonObject e; e["error"] = doc.isObject() && doc.object().contains("error") ? doc.object().value("error").toString() : QString("mesh bridge HTTP %1").arg(http); e["http"] = http; m_lastError = e["error"].toString(); reply(http == 401 ? 401 : 502, e); return; }
        m_lastOkMs = QDateTime::currentMSecsSinceEpoch(); m_lastError.clear();
        QJsonObject o; if (doc.isArray()) o["items"] = doc.array(); else if (doc.isObject()) o = doc.object(); else o["raw"] = QString::fromUtf8(data);
        reply(200, o);
    });
}

void MeshClient::nodes(Reply reply) {
    getJson("/api/nodes", [this, reply](int st, const QJsonObject& b) {
        if (st != 200) { reply(st, b); return; }
        QJsonArray out;
        for (const QJsonValue& v : b.value("items").toArray()) {
            const QJsonObject n = v.toObject();
            QJsonObject o;
            o["id"] = n.value("id").toString(); o["hostname"] = n.value("hostname").toString(); o["machineName"] = n.value("machineName").toString();
            o["ip"] = n.value("ip").toString(); o["port"] = n.value("port").toInt(); o["online"] = n.value("online").toBool(); o["isLocal"] = n.value("isLocal").toBool();
            o["elevated"] = n.value("elevated").toBool(); o["relay"] = n.value("relay").toBool();
            o["drives"] = n.value("drives").toArray(); o["sharedFolders"] = n.value("sharedFolders").toArray(); o["dropboxPath"] = n.value("dropboxPath").toString();
            m_nodeNames[o["id"].toString()] = o["hostname"].toString();
            out.append(o);
        }
        m_nodeCount = out.size();
        QJsonObject r; r["nodes"] = out; r["url"] = m_baseUrl; reply(200, r);
    });
}

void MeshClient::browse(const QString& node, const QString& path, Reply reply) {
    QUrlQuery q; q.addQueryItem("path", path); if (!node.isEmpty() && node != "__local__") q.addQueryItem("node", node);
    getJson("/api/browse?" + q.toString(QUrl::FullyEncoded), [reply, node](int st, const QJsonObject& b) {
        if (st != 200) { reply(st, b); return; }
        QJsonObject o = b; o["node"] = node; reply(200, o);
    });
}

void MeshClient::search(const QString& q, const QString& node, Reply reply) {
    QUrlQuery qq; qq.addQueryItem("q", q);
    getJson("/api/search?" + qq.toString(QUrl::FullyEncoded), [reply, node](int st, const QJsonObject& b) {
        if (st != 200) { reply(st, b); return; }
        QJsonArray hits;
        for (const QJsonValue& v : b.value("items").toArray()) {
            const QJsonObject h = v.toObject();
            if (!node.isEmpty() && h.value("nodeId").toString() != node && h.value("nodeName").toString() != node) continue;
            hits.append(h);
        }
        QJsonObject o; o["hits"] = hits; o["count"] = hits.size(); reply(200, o);
    });
}

// ---------------------------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------------------------
QJsonObject MeshClient::Job::toJson() const {
    QJsonObject o; o["id"] = id; o["kind"] = kind; o["node"] = node; o["nodeName"] = nodeName; o["remotePath"] = remotePath; o["localPath"] = QDir::toNativeSeparators(localPath);
    o["type"] = type; o["folder"] = folder; o["state"] = state; o["bytes"] = double(bytes); o["total"] = double(total); o["rateBps"] = rateBps;
    o["startedMs"] = double(startedMs); o["finishedMs"] = double(finishedMs); o["error"] = error; o["name"] = QFileInfo(remotePath.isEmpty() ? localPath : QString(remotePath).replace(QChar('\\'), QChar('/'))).fileName();
    return o;
}

QJsonArray MeshClient::jobs() const { QJsonArray a; for (Job* j : m_jobs) a.append(j->toJson()); return a; }

QString MeshClient::safeName(const QString& s) {
    QString n = s; n.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_"); return n.isEmpty() ? "node" : n;
}

bool MeshClient::cancel(const QString& jobId) {
    for (Job* j : m_jobs) if (j->id == jobId) {
        if (j->state == "running" || j->state == "queued") { if (j->reply) j->reply->abort(); else finish(j, "cancelled", "cancelled"); return true; }
        m_jobs.removeAll(j); delete j->file; delete j; notify(); return true;
    }
    return false;
}

void MeshClient::clearFinished() {
    for (int i = m_jobs.size() - 1; i >= 0; --i) { Job* j = m_jobs[i]; if (j->state != "running" && j->state != "queued") { m_jobs.removeAt(i); delete j->file; delete j; } }
    notify();
}

void MeshClient::finish(Job* j, const QString& state, const QString& error) {
    j->state = state; j->error = error; j->finishedMs = QDateTime::currentMSecsSinceEpoch(); j->reply = nullptr;
    if (j->file) { j->file->close(); delete j->file; j->file = nullptr; }
    if (state == "done") qInfo().noquote() << "[Mesh]" << j->kind << "done" << j->remotePath << "->" << j->localPath << j->bytes << "bytes";
    else qWarning().noquote() << "[Mesh]" << j->kind << state << j->remotePath << error;
    notify();
}

QJsonObject MeshClient::startFetch(const QString& node, const QString& path, const QString& type, const QString& folder) {
    const QString name = QFileInfo(QString(path).replace('\\', '/')).fileName();
    const QString nodeName = m_nodeNames.value(node, node.isEmpty() ? "server" : node);
    const QString dir = folder.isEmpty() ? QDir(m_downloadRoot).filePath(safeName(nodeName)) : folder;
    QDir().mkpath(dir);
    // Reuse a job for the same remote file (resume) instead of starting a second download.
    for (Job* j : m_jobs) if (j->kind == "fetch" && j->node == node && j->remotePath == path) {
        if (j->state == "running" || j->state == "queued") return j->toJson();
        if (j->state == "done") return j->toJson();
        j->state = "queued"; j->error.clear(); runFetch(j); return j->toJson();
    }
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->kind = "fetch"; j->node = node; j->nodeName = nodeName;
    j->remotePath = path; j->type = type.isEmpty() ? "program" : type; j->localPath = QDir(dir).filePath(name); j->folder = dir;
    m_jobs.append(j);
    runFetch(j);
    return j->toJson();
}

void MeshClient::runFetch(Job* j) {
    j->state = "running"; j->startedMs = QDateTime::currentMSecsSinceEpoch(); j->error.clear();
    const QString part = j->localPath + ".part";
    j->file = new QFile(part);
    const bool resume = QFileInfo::exists(part) && QFileInfo(part).size() > 0;
    if (!j->file->open(resume ? (QIODevice::WriteOnly | QIODevice::Append) : QIODevice::WriteOnly)) { finish(j, "failed", "cannot write " + part); return; }
    const qint64 offset = resume ? j->file->size() : 0;
    j->bytes = offset;
    QUrlQuery q; q.addQueryItem("path", j->remotePath); if (!j->node.isEmpty() && j->node != "__local__") q.addQueryItem("node", j->node);
    QNetworkRequest req = request("/api/download?" + q.toString(QUrl::FullyEncoded));
    req.setTransferTimeout(0);
    if (offset > 0) req.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(offset) + "-");
    QNetworkReply* r = m_nam.get(req);
    j->reply = r;
    auto t0 = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch());
    auto b0 = std::make_shared<qint64>(offset);
    connect(r, &QNetworkReply::metaDataChanged, this, [this, r, j, offset]() {
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 len = r->header(QNetworkRequest::ContentLengthHeader).toLongLong();
        if (http == 200 && offset > 0) { j->file->close(); j->file->open(QIODevice::WriteOnly); j->bytes = 0; }   // server ignored Range: restart the file
        if (len > 0) j->total = (http == 206 ? offset : 0) + len;
        notify();
    });
    connect(r, &QNetworkReply::readyRead, this, [this, r, j, t0, b0]() {
        if (!j->file) return;
        const QByteArray chunk = r->readAll(); j->file->write(chunk); j->bytes += chunk.size();
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - *t0 >= 1000) { j->rateBps = double(j->bytes - *b0) * 1000.0 / double(now - *t0); *t0 = now; *b0 = j->bytes; notify(); }
    });
    connect(r, &QNetworkReply::finished, this, [this, r, j]() {
        r->deleteLater();
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() == QNetworkReply::OperationCanceledError) { finish(j, "cancelled", "cancelled"); return; }
        if (r->error() != QNetworkReply::NoError || http >= 400) { finish(j, "failed", http ? QString("HTTP %1 %2").arg(http).arg(r->errorString()) : r->errorString()); return; }
        if (j->file) { j->file->close(); delete j->file; j->file = nullptr; }
        if (QFileInfo::exists(j->localPath) && QFileInfo(j->localPath).size() == QFileInfo(j->localPath + ".part").size()) { QFile::remove(j->localPath + ".part"); j->total = QFileInfo(j->localPath).size(); j->bytes = j->total; finish(j, "done"); emit fetched(j->localPath, j->type, j->node, j->remotePath); return; }
        const QString part = j->localPath + ".part";
        if (!QFile::remove(j->localPath) && QFileInfo::exists(j->localPath)) { const QFileInfo fi(j->localPath); j->localPath = fi.dir().filePath(fi.completeBaseName() + "_" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + (fi.suffix().isEmpty() ? "" : "." + fi.suffix())); }
        if (!QFile::rename(part, j->localPath)) { finish(j, "failed", "rename failed"); return; }
        j->total = QFileInfo(j->localPath).size(); j->bytes = j->total;
        finish(j, "done");
        emit fetched(j->localPath, j->type, j->node, j->remotePath);
    });
    notify();
}

QJsonObject MeshClient::startPush(const QString& localPath, const QString& node, const QString& folder) {
    if (!QFileInfo(localPath).isFile()) { QJsonObject e; e["error"] = "no such file: " + localPath; e["status"] = 404; return e; }
    Job* j = new Job; j->id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); j->kind = "push"; j->node = node; j->nodeName = m_nodeNames.value(node, node);
    j->localPath = localPath; j->folder = folder; j->remotePath = (folder.isEmpty() ? QString("<drop folder>") : folder) + "\\" + QFileInfo(localPath).fileName();
    j->total = QFileInfo(localPath).size(); j->totalChunks = int((j->total + kChunk - 1) / kChunk);
    m_jobs.append(j);
    runPush(j);
    return j->toJson();
}

void MeshClient::runPush(Job* j) {
    j->state = "running"; j->startedMs = QDateTime::currentMSecsSinceEpoch();
    const QFileInfo fi(j->localPath);
    QJsonObject init; init["name"] = fi.fileName(); init["size"] = double(fi.size()); init["targetDir"] = j->folder;
    init["fileId"] = QString("file_%1_%2_%3").arg(fi.fileName()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    QNetworkRequest req = request("/api/upload/init?agentId=" + (j->node == "__local__" ? QString() : j->node));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* r = m_nam.post(req, QJsonDocument(init).toJson(QJsonDocument::Compact));
    j->reply = r;
    connect(r, &QNetworkReply::finished, this, [this, r, j]() {
        r->deleteLater(); j->reply = nullptr;
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject o = QJsonDocument::fromJson(r->isOpen() ? r->readAll() : QByteArray()).object();
        if (r->error() == QNetworkReply::OperationCanceledError) { finish(j, "cancelled", "cancelled"); return; }
        if (http == 401) { finish(j, "failed", "mesh authentication required: set mesh.anfsToken or mesh.anfsCookie (PUT /api/v1/mesh/config)"); return; }
        if (http != 200 || o.value("uploadId").toString().isEmpty()) { finish(j, "failed", o.value("error").toString(http ? QString("HTTP %1").arg(http) : r->errorString())); return; }
        j->uploadId = o.value("uploadId").toString();
        j->have.clear(); for (const QJsonValue& v : o.value("uploadedChunks").toArray()) j->have.append(v.toInt());
        j->chunk = 0; j->bytes = qint64(j->have.size()) * kChunk;
        pushChunk(j);
    });
    notify();
}

void MeshClient::pushChunk(Job* j) {
    while (j->chunk < j->totalChunks && j->have.contains(j->chunk)) ++j->chunk;
    const QString agent = j->node == "__local__" ? QString() : j->node;
    if (j->chunk >= j->totalChunks) {
        QJsonObject done; done["uploadId"] = j->uploadId; done["name"] = QFileInfo(j->localPath).fileName(); done["targetDir"] = j->folder; done["totalChunks"] = j->totalChunks;
        QNetworkRequest req = request("/api/upload/complete?agentId=" + agent);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* r = m_nam.post(req, QJsonDocument(done).toJson(QJsonDocument::Compact));
        j->reply = r;
        connect(r, &QNetworkReply::finished, this, [this, r, j]() {
            r->deleteLater(); j->reply = nullptr;
            const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QJsonObject o = QJsonDocument::fromJson(r->isOpen() ? r->readAll() : QByteArray()).object();
            if (http != 200) { finish(j, "failed", "complete: " + o.value("error").toString(QString("HTTP %1").arg(http))); return; }
            if (!o.value("path").toString().isEmpty()) j->remotePath = o.value("path").toString();
            j->bytes = j->total; finish(j, "done");
        });
        return;
    }
    QFile f(j->localPath);
    if (!f.open(QIODevice::ReadOnly)) { finish(j, "failed", "cannot read " + j->localPath); return; }
    f.seek(qint64(j->chunk) * kChunk);
    const QByteArray data = f.read(kChunk);
    QNetworkRequest req = request("/api/upload/chunk?agentId=" + agent);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setRawHeader("x-upload-id", j->uploadId.toUtf8());
    req.setRawHeader("x-chunk-index", QByteArray::number(j->chunk));
    req.setTransferTimeout(120000);
    QNetworkReply* r = m_nam.post(req, data);
    j->reply = r;
    const int sz = data.size();
    connect(r, &QNetworkReply::finished, this, [this, r, j, sz]() {
        r->deleteLater(); j->reply = nullptr;
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() == QNetworkReply::OperationCanceledError) { finish(j, "cancelled", "cancelled"); return; }
        if (http != 200) { finish(j, "failed", QString("chunk %1: HTTP %2 %3").arg(j->chunk).arg(http).arg(r->errorString())); return; }
        j->have.append(j->chunk); ++j->chunk; j->bytes += sz;
        const qint64 el = QDateTime::currentMSecsSinceEpoch() - j->startedMs; if (el > 0) j->rateBps = double(j->bytes) * 1000.0 / double(el);
        notify();
        pushChunk(j);
    });
}
