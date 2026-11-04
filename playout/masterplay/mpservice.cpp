// MasterPlay REST service (slice MP-11): see mpservice.h for the routes, inputs, threading and safety rules.
#include "mpservice.h"

#include "masterplayinstructions.h"
#include "mplane.h"
#include "schedulestore.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QThreadPool>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>

namespace mp::svc {
namespace {

const QString kConfigKey = QStringLiteral("masterplay.config");
const QString kLinksKey = QStringLiteral("masterplay.links");
const QString kAsRunStateKey = QStringLiteral("masterplay.asrun.state");
const QString kDefaultTz = QStringLiteral("America/New_York");   // design §8: the plant's zone

struct Resp {
    int status = 200;
    QJsonObject body;
};

Resp err(int status, const QString& message, const QJsonObject& extra = {}) {
    QJsonObject o = extra;
    o[QStringLiteral("error")] = message;
    o[QStringLiteral("status")] = status;
    return {status, o};
}

QJsonArray strings(const QStringList& l) { return QJsonArray::fromStringList(l); }

qint64 wallMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString isoUtc(qint64 ms) { return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone::UTC).toString(Qt::ISODate); }

QString sha256Hex(const QByteArray& b) { return QString::fromLatin1(QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex()); }

/// A boolean from JSON or a query string ("1", "true", "yes", "on").
bool truthy(const QJsonValue& v, bool def = false) {
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s.isEmpty()) return def;
        return s == QLatin1String("1") || s == QLatin1String("true") || s == QLatin1String("yes") || s == QLatin1String("on");
    }
    return def;
}

/// Query strings as JSON values: numbers, booleans and the rest as strings (the raw download route's options); names,
/// dates and the pathMap stay strings whatever they look like.
QJsonObject queryToJson(const Query& q) {
    static const QSet<QString> textual{QStringLiteral("channel"), QStringLiteral("channelId"), QStringLiteral("from"), QStringLiteral("to"), QStringLiteral("mode"),
                                       QStringLiteral("dateStyle"), QStringLiteral("encoding"), QStringLiteral("eol"), QStringLiteral("source"), QStringLiteral("node")};
    QJsonObject o;
    for (auto it = q.cbegin(); it != q.cend(); ++it) {
        const QString v = it.value();
        bool ok = false;
        const double n = textual.contains(it.key()) ? 0.0 : v.toDouble(&ok);
        if (ok && !v.trimmed().isEmpty()) o[it.key()] = n;
        else if (v == QLatin1String("true") || v == QLatin1String("false")) o[it.key()] = v == QLatin1String("true");
        else o[it.key()] = v;
    }
    return o;
}

/// Joins a folder and a relative name with backslashes (the folder may end with one).
QString joinPath(const QString& folder, const QString& name) {
    QString f = ini::normalizePath(folder);
    const QString n = ini::normalizePath(name);
    if (f.isEmpty()) return n;
    if (!f.endsWith(QLatin1Char('\\'))) f += QLatin1Char('\\');
    return f + n;
}

bool safeRelative(const QString& rel) {   // no drive, no UNC, no ".." segment
    if (rel.isEmpty()) return true;
    if (res::isAbsolute(rel) || rel.startsWith(QLatin1Char('/')) || rel.startsWith(QLatin1Char('\\'))) return false;
    for (const QString& seg : ini::normalizePath(rel).split(QLatin1Char('\\'), Qt::SkipEmptyParts))
        if (seg == QLatin1String("..")) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Config sources

struct Source {
    QString id, label, read = QStringLiteral("fs"), node, playlistDir, logPath, osdDir, tz;
    bool valid = false;
    bool mesh() const { return read == QLatin1String("mesh"); }
    static Source fromJson(const QJsonObject& o) {
        Source s;
        s.id = o.value(QStringLiteral("id")).toString().trimmed();
        s.label = o.value(QStringLiteral("label")).toString();
        s.read = o.value(QStringLiteral("read")).toString(QStringLiteral("fs")).trimmed().toLower();
        if (s.read.isEmpty()) s.read = QStringLiteral("fs");
        s.node = o.value(QStringLiteral("node")).toString().trimmed();
        s.playlistDir = o.value(QStringLiteral("playlistDir")).toString().trimmed();
        s.logPath = o.value(QStringLiteral("logPath")).toString().trimmed();
        s.osdDir = o.value(QStringLiteral("osdDir")).toString().trimmed();
        s.tz = o.value(QStringLiteral("tz")).toString().trimmed();
        s.valid = !s.id.isEmpty();
        return s;
    }
    QJsonObject toJson() const {
        QJsonObject o{{"id", id}, {"label", label}, {"read", read}, {"playlistDir", playlistDir}, {"logPath", logPath}, {"osdDir", osdDir}, {"tz", tz}};
        if (!node.isEmpty()) o["node"] = node;
        return o;
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Mesh reads (design §10: the AN-FS bridge over the service's own QNetworkAccessManager, in the worker that needs them)

struct MeshCfg {
    QString url, token, cookie;
};

/// GET <url><pathAndQuery>, waiting on a local event loop (worker threads only). httpStatus 0 = no answer (why says).
QByteArray meshGet(const MeshCfg& cfg, const QString& pathAndQuery, int timeoutMs, qint64 maxBytes, int* httpStatus, QString* why) {
    *httpStatus = 0;
    why->clear();
    if (cfg.url.isEmpty()) { *why = QStringLiteral("mesh.anfsUrl is not set (PUT /api/v1/mesh/config)"); return {}; }
    QNetworkAccessManager nam;
    QNetworkRequest req(QUrl(cfg.url + pathAndQuery));
    req.setRawHeader("Accept", "*/*");
    if (!cfg.token.isEmpty()) req.setRawHeader("Authorization", ("Bearer " + cfg.token).toUtf8());
    if (!cfg.cookie.isEmpty()) req.setRawHeader("Cookie", cfg.cookie.toUtf8());
    req.setTransferTimeout(timeoutMs);
    QEventLoop loop;
    QNetworkReply* r = nam.get(req);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, r, &QNetworkReply::abort);
    guard.start(timeoutMs + 2000);
    loop.exec();
    *httpStatus = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray bytes = r->readAll();
    if (*httpStatus == 0) *why = r->errorString();
    else if (*httpStatus == 401) *why = QStringLiteral("mesh authentication required: set mesh.anfsToken or mesh.anfsCookie");
    else if (*httpStatus >= 400) *why = QStringLiteral("bridge answered %1").arg(*httpStatus);
    if (why->isEmpty() && bytes.size() > maxBytes) { *why = QStringLiteral("file over %1 MiB").arg(maxBytes / (1024 * 1024)); *httpStatus = 413; bytes.clear(); }
    r->deleteLater();
    return bytes;
}

QString meshQuery(const QString& node, const QString& path) {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("path"), path);
    if (!node.isEmpty() && node != QLatin1String("__local__")) q.addQueryItem(QStringLiteral("node"), node);
    return q.toString(QUrl::FullyEncoded);
}

/// Sub-playlists / *FromFile texts of a mesh-read playlist: absolute targets through the path rules, relative ones
/// against the inserting file's folder, each one GET /api/download.
tl::ReadFn meshReader(const MeshCfg& cfg, const QString& node, const QList<ini::PathRule>& rules, int timeoutMs, qint64 maxBytes) {
    return [cfg, node, rules, timeoutMs, maxBytes](const QString& target, const QString& fromFile) -> tl::ReadResult {
        tl::ReadResult rr;
        QString path = ini::normalizePath(target.trimmed());
        if (path.isEmpty()) { rr.why = QStringLiteral("no name"); return rr; }
        if (res::isAbsolute(path)) path = ini::mapPath(path, rules);
        else {
            const QString folder = res::parentFolder(ini::normalizePath(fromFile));
            if (folder.isEmpty()) { rr.why = QStringLiteral("relative name with no folder to read from"); rr.path = path; return rr; }
            path = joinPath(folder, path);
        }
        rr.path = path;
        int st = 0;
        QString why;
        rr.bytes = meshGet(cfg, QStringLiteral("/api/download?") + meshQuery(node, path), timeoutMs, maxBytes, &st, &why);
        rr.ok = st == 200 && why.isEmpty();
        if (!rr.ok) { rr.why = st == 404 ? QStringLiteral("not found") : why; rr.bytes.clear(); }
        return rr;
    };
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Impl

struct MpService::Impl {
    MpService* q = nullptr;
    Host host;
    QList<Route> routes;
    QThreadPool pool;
    std::atomic<int> pending{0};
    std::unique_ptr<QNetworkAccessManager> selfNam;   // the default afterSave (a POST to this Supervisor's own REST)

    sched::ScheduleStore* store() const { return host.store; }
    qint64 now() const { return host.nowMs > 0 ? host.nowMs : wallMs(); }

    // ---- settings -----------------------------------------------------------------------------------------------------
    QJsonObject configJson() const {
        QJsonObject c = store() ? store()->settingJson(kConfigKey) : QJsonObject();
        if (!c.value(QStringLiteral("sources")).isArray()) c[QStringLiteral("sources")] = QJsonArray();
        if (!c.value(QStringLiteral("pathMap")).isArray()) c[QStringLiteral("pathMap")] = QJsonArray();
        for (const char* k : {"defaults", "resolver", "asrun"})
            if (!c.value(QLatin1String(k)).isObject()) c[QLatin1String(k)] = QJsonObject();
        return c;
    }
    QList<Source> sources() const {
        QList<Source> l;
        for (const QJsonValue& v : configJson().value(QStringLiteral("sources")).toArray()) {
            const Source s = Source::fromJson(v.toObject());
            if (s.valid) l << s;
        }
        return l;
    }
    Source sourceById(const QString& id) const {
        for (const Source& s : sources())
            if (s.id.compare(id, Qt::CaseInsensitive) == 0) return s;
        return {};
    }
    /// The path rules of a run: the body's pathMap (an array) else the config's; normalised.
    QList<ini::PathRule> rules(const QJsonObject& body) const {
        const QJsonValue pm = body.value(QStringLiteral("pathMap"));
        const QJsonArray a = pm.isArray() ? pm.toArray() : configJson().value(QStringLiteral("pathMap")).toArray();
        QList<ini::PathRule> l;
        for (const QJsonValue& v : a) {
            const ini::PathRule r = ini::PathRule::fromJson(v.toObject());
            if (!r.from.isEmpty()) l << r;
        }
        return ini::normalizeRules(l);
    }
    QJsonArray rulesJson(const QList<ini::PathRule>& l) const {
        QJsonArray a;
        for (const ini::PathRule& r : l) a << r.toJson();
        return a;
    }
    /// The resolver options of a run: config.resolver, then body.resolver (or the body's own keys), the rules, channel, clock.
    res::Options resolverOptions(const QJsonObject& body, const QList<ini::PathRule>& r, const QString& channelId) const {
        QJsonObject m = configJson().value(QStringLiteral("resolver")).toObject();
        const QJsonValue ov = body.value(QStringLiteral("resolver"));
        const QJsonObject over = ov.isObject() ? ov.toObject() : QJsonObject();
        for (auto it = over.begin(); it != over.end(); ++it) m[it.key()] = it.value();
        m[QStringLiteral("pathMap")] = rulesJson(r);
        res::Options o = res::Options::fromJson(m);
        o.channelId = channelId;
        o.nowMs = now();
        return o;
    }
    MeshCfg meshCfg() const {
        MeshCfg c;
        QJsonObject j;
        if (host.meshConfig) j = host.meshConfig();
        else if (store()) { j[QStringLiteral("url")] = store()->setting(QStringLiteral("mesh.anfsUrl")); j[QStringLiteral("token")] = store()->setting(QStringLiteral("mesh.anfsToken")); j[QStringLiteral("cookie")] = store()->setting(QStringLiteral("mesh.anfsCookie")); }
        c.url = j.value(QStringLiteral("url")).toString().trimmed();
        while (c.url.endsWith(QLatin1Char('/'))) c.url.chop(1);
        c.token = j.value(QStringLiteral("token")).toString().trimmed();
        c.cookie = j.value(QStringLiteral("cookie")).toString().trimmed();
        return c;
    }

    // ---- inputs -------------------------------------------------------------------------------------------------------
    struct Input {
        int status = 200;
        QString error;
        QString name, file;            // a label; the path as read ("" for pasted text without a dir)
        QString read;                  // text | fs | mesh
        QString node, sourceId;
        QByteArray bytes;
        Source src;
        MeshCfg mesh;                  // filled for mesh reads (sub-playlists use it)
    };
    /// kind: playlist | log | ini (what a source's default path is). Blocking: worker threads only.
    Input readInput(const QJsonObject& body, const QString& kind) const {
        Input in;
        const QString sourceId = body.value(QStringLiteral("source")).toString().trimmed();
        if (!sourceId.isEmpty()) {
            in.src = sourceById(sourceId);
            if (!in.src.valid) { in.status = 404; in.error = QStringLiteral("unknown source '%1'").arg(sourceId); return in; }
            in.sourceId = in.src.id;
        }
        in.node = body.value(QStringLiteral("node")).toString().trimmed();
        if (in.node.isEmpty()) in.node = in.src.node;
        const QJsonValue text = body.value(QStringLiteral("text"));
        const QString path = body.value(QStringLiteral("path")).toString().trimmed();
        if (text.isString() && !path.isEmpty()) { in.status = 400; in.error = QStringLiteral("give text or path, not both"); return in; }
        if (text.isString()) {
            in.bytes = text.toString().toUtf8();
            in.read = QStringLiteral("text");
            in.name = body.value(QStringLiteral("name")).toString().trimmed();
            if (in.name.isEmpty()) in.name = QStringLiteral("pasted.") + (kind == QLatin1String("log") ? QStringLiteral("log") : kind == QLatin1String("ini") ? QStringLiteral("ini") : QStringLiteral("in"));
            const QString dir = body.value(QStringLiteral("dir")).toString().trimmed();   // where relative sub-playlists live
            if (!dir.isEmpty()) in.file = joinPath(dir, in.name);
            return in;
        }
        QString p = path;
        if (p.isEmpty() && in.src.valid) p = kind == QLatin1String("log") ? in.src.logPath : QString();
        if (p.isEmpty()) { in.status = 400; in.error = QStringLiteral("text or path required"); return in; }
        p = ini::normalizePath(p);
        if (!res::isAbsolute(p)) {
            if (!in.src.valid) { in.status = 400; in.error = QStringLiteral("a relative path needs a source"); return in; }
            if (!safeRelative(p)) { in.status = 400; in.error = QStringLiteral("a relative path may not leave the source folder"); return in; }
            const QString base = kind == QLatin1String("log") ? res::parentFolder(ini::normalizePath(in.src.logPath)) : in.src.playlistDir;
            if (base.isEmpty()) { in.status = 400; in.error = QStringLiteral("source '%1' has no folder for %2 files").arg(in.src.id, kind); return in; }
            p = joinPath(base, p);
        }
        in.file = p;
        in.name = res::fileName(p);
        const bool mesh = in.src.valid ? in.src.mesh() : !in.node.isEmpty();
        if (mesh) {
            in.read = QStringLiteral("mesh");
            in.mesh = meshCfg();
            int st = 0;
            QString why;
            in.bytes = meshGet(in.mesh, QStringLiteral("/api/download?") + meshQuery(in.node, p), host.meshTimeoutMs, kMaxFileBytes, &st, &why);
            if (st == 404) { in.status = 404; in.error = QStringLiteral("not found on the mesh: %1").arg(p); }
            else if (st == 413) { in.status = 413; in.error = why; }
            else if (st != 200 || !why.isEmpty()) { in.status = 502; in.error = QStringLiteral("mesh read failed: %1").arg(why.isEmpty() ? QStringLiteral("status %1").arg(st) : why); }
            return in;
        }
        in.read = QStringLiteral("fs");
        const QFileInfo fi(p);
        if (!fi.exists() || !fi.isFile()) { in.status = 404; in.error = QStringLiteral("not found: %1").arg(p); return in; }
        if (fi.size() > kMaxFileBytes) { in.status = 413; in.error = QStringLiteral("file over %1 MiB: %2").arg(kMaxFileBytes / (1024 * 1024)).arg(p); return in; }
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) { in.status = 500; in.error = QStringLiteral("cannot read %1: %2").arg(p, f.errorString()); return in; }
        in.bytes = f.readAll();
        return in;
    }
    static QJsonObject inputJson(const Input& in) {
        QJsonObject o{{"name", in.name}, {"file", in.file}, {"read", in.read}, {"bytes", double(in.bytes.size())}, {"sha256", sha256Hex(in.bytes)}};
        if (!in.node.isEmpty()) o["node"] = in.node;
        if (!in.sourceId.isEmpty()) o["source"] = in.sourceId;
        return o;
    }
    tl::ReadFn readerFor(const Input& in, const QList<ini::PathRule>& r) const {
        if (in.read == QLatin1String("mesh")) return meshReader(in.mesh, in.node, r, host.meshTimeoutMs, kMaxFileBytes);
        return tl::fileReader(r, kMaxFileBytes);
    }

    // ---- workers ------------------------------------------------------------------------------------------------------
    /// work in the pool; then onMain (optional) and the reply on the service's thread.
    void run(std::function<Resp()> work, Reply reply, std::function<void(Resp&)> onMain = {}) {
        ++pending;
        QPointer<MpService> self(q);
        pool.start([self, work = std::move(work), reply = std::move(reply), onMain = std::move(onMain), this]() {
            Resp r;
            try { r = work(); } catch (const std::exception& e) { r = err(500, QStringLiteral("masterplay: %1").arg(QString::fromUtf8(e.what()))); } catch (...) { r = err(500, QStringLiteral("masterplay: unexpected error")); }
            --pending;
            if (!self) return;
            QMetaObject::invokeMethod(self.data(), [self, r, reply, onMain]() mutable {
                if (!self) return;
                if (onMain) onMain(r);
                reply(r.status, r.body);
            }, Qt::QueuedConnection);
        });
    }
    void afterSave() {
        if (host.afterSave) { host.afterSave(); return; }
        if (host.restPort <= 0) return;
        if (!selfNam) selfNam = std::make_unique<QNetworkAccessManager>();
        QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/v1/spots/markers/enforce").arg(host.restPort)));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        const QString token = host.restToken ? host.restToken() : QString();
        if (!token.isEmpty()) req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
        req.setTransferTimeout(10000);
        QNetworkReply* r = selfNam->post(req, QByteArrayLiteral("{}"));
        QObject::connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
    }
    void broadcast(const QString& event, QJsonObject payload) {
        payload[QStringLiteral("event")] = event;
        if (host.broadcast) host.broadcast(QStringLiteral("masterplay"), payload);
    }

    // ---- handlers -----------------------------------------------------------------------------------------------------
    Resp info() const {
        QJsonObject o = laneInfo();
        o[QStringLiteral("routes")] = strings(q->routeKeys());
        QJsonArray src;
        for (const Source& s : sources()) src << QJsonObject{{"id", s.id}, {"label", s.label}, {"read", s.read}, {"node", s.node}};
        o[QStringLiteral("sources")] = src;
        const QJsonObject c = configJson();
        o[QStringLiteral("pathMap")] = c.value(QStringLiteral("pathMap")).toArray().size();
        o[QStringLiteral("defaults")] = c.value(QStringLiteral("defaults")).toObject();
        o[QStringLiteral("pending")] = pending.load();
        o[QStringLiteral("imports")] = store() ? imp::Importer(store(), nullptr).imports(1).size() : 0;
        o[QStringLiteral("links")] = q->links().size();
        o[QStringLiteral("asrunState")] = q->asRunState();
        o[QStringLiteral("settingsKeys")] = QJsonArray{kConfigKey, QStringLiteral("masterplay.imports"), QStringLiteral("masterplay.backup.<importId>"), kLinksKey, kAsRunStateKey};
        o[QStringLiteral("dryRunByDefault")] = true;
        return {200, o};
    }
    Resp catalog() const {
        QJsonArray diags;
        for (const DiagnosticInfo& d : diagnosticCatalog()) diags << QJsonObject{{"code", d.code}, {"severity", d.severity}, {"stage", d.stage}, {"title", d.title}};
        return {200, QJsonObject{{"instructions", catalogJson()}, {"diagnostics", diags}}};
    }

    Resp parseRoute(const QJsonObject& body) const {
        const Input in = readInput(body, QStringLiteral("playlist"));
        if (in.status != 200) return err(in.status, in.error);
        ParseOptions po;
        po.fps = qBound(1, body.value(QStringLiteral("fps")).toInt(configJson().value(QStringLiteral("defaults")).toObject().value(QStringLiteral("fps")).toInt(30)), 120);
        const QString ldo = body.value(QStringLiteral("legacyDateOrder")).toString().trimmed().toLower();
        if (!ldo.isEmpty()) po.legacyDateOrder = ldo;
        const Document doc = parse(in.bytes, po);
        QJsonObject o = doc.toJson(truthy(body.value(QStringLiteral("withEntries"))));
        const QJsonObject ij = inputJson(in);
        for (auto it = ij.begin(); it != ij.end(); ++it) o[it.key()] = it.value();
        o[QStringLiteral("roundTrip")] = serialize(doc) == in.bytes;
        o[QStringLiteral("instructionStats")] = instructionStats(doc);
        o[QStringLiteral("fps")] = po.fps;
        return {200, o};
    }

    struct ImportSetup {
        imp::Options o;
        QStringList errors;
    };
    ImportSetup importOptions(const QJsonObject& body) const {
        ImportSetup s;
        QJsonObject m = configJson().value(QStringLiteral("defaults")).toObject();
        const QJsonValue ov = body.value(QStringLiteral("options"));
        const QJsonObject over = ov.isObject() ? ov.toObject() : QJsonObject();
        for (auto it = over.begin(); it != over.end(); ++it) m[it.key()] = it.value();
        for (const char* k : {"channel", "channelId", "mode", "from", "to", "weekdays", "hours", "allowOnAir"})
            if (body.contains(QLatin1String(k))) m[QLatin1String(k)] = body.value(QLatin1String(k));
        m.remove(QStringLiteral("nowMs"));   // the clock is the service's (the on-air rule must not be steered by a client)
        s.o = imp::Options::fromJson(m, &s.errors);
        s.o.nowMs = now();
        s.errors << s.o.check();
        if (s.o.channelId.isEmpty()) s.errors << QStringLiteral("channel required");
        else if (store()) { bool found = false; store()->channel(s.o.channelId, &found); if (!found) s.errors << QStringLiteral("unknown channel '%1'").arg(s.o.channelId); }
        return s;
    }

    Resp importRoute(const QJsonObject& body, QJsonArray* ingestList, bool* applied) {
        *applied = false;
        const ImportSetup s = importOptions(body);
        if (!s.errors.isEmpty()) return err(400, QStringLiteral("bad import request: %1").arg(s.errors.join(QStringLiteral("; "))), QJsonObject{{"errors", strings(s.errors)}});
        const Input in = readInput(body, QStringLiteral("playlist"));
        if (in.status != 200) return err(in.status, in.error);
        const Document doc = parse(in.bytes, s.o.timeline.parse);
        const QList<ini::PathRule> r = rules(body);
        const res::Resolver resolver = res::Resolver::fromStore(*store(), resolverOptions(body, r, s.o.channelId));
        const imp::Importer im(store(), &resolver, readerFor(in, r));
        const imp::Plan plan = im.plan(doc, in.file, s.o);
        const bool dryRun = truthy(body.value(QStringLiteral("dryRun")), true);
        const bool withSlots = truthy(body.value(QStringLiteral("withSlots")), true);
        const bool withPlacements = truthy(body.value(QStringLiteral("withPlacements")), false);
        if (dryRun) {
            QJsonObject o{{"dryRun", true}, {"source", inputJson(in)}, {"plan", plan.toJson(withSlots, withPlacements)}, {"pathMap", rulesJson(r)}};
            if (!plan.ok()) return err(400, QStringLiteral("nothing planned: %1").arg(plan.errors.join(QStringLiteral("; "))), o);
            return {200, o};
        }
        if (!plan.ok()) return err(400, QStringLiteral("nothing planned: %1").arg(plan.errors.join(QStringLiteral("; "))), QJsonObject{{"errors", strings(plan.errors)}});
        imp::ApplyOptions a = imp::ApplyOptions::fromJson(body);
        a.nowMs = now();
        if (a.by.isEmpty()) a.by = QStringLiteral("rest");
        QPointer<MpService> self(q);
        a.afterSave = [self]() { if (self) QMetaObject::invokeMethod(self.data(), [self]() { if (self) self->d->afterSave(); }, Qt::QueuedConnection); };
        const imp::ApplyResult res = im.apply(plan, a);
        QJsonObject o = res.toJson();
        o[QStringLiteral("dryRun")] = false;
        o[QStringLiteral("source")] = inputJson(in);
        o[QStringLiteral("plan")] = plan.toJson(false, false);
        if (!res.ok()) return {res.status, o};
        *applied = true;
        if (s.o.missing == QLatin1String("ingest")) *ingestList = plan.ingest;
        return {200, o};
    }

    Resp undoRoute(const QString& id, const QJsonObject& body) {
        if (id.trimmed().isEmpty()) return err(400, QStringLiteral("import id required"));
        imp::ApplyOptions a = imp::ApplyOptions::fromJson(body);
        a.importId.clear();
        a.nowMs = now();
        if (a.by.isEmpty()) a.by = QStringLiteral("rest");
        a.afterSave = [this]() { afterSave(); };
        const imp::ApplyResult res = imp::Importer(store(), nullptr).undo(id.trimmed(), a);
        QJsonObject o = res.toJson();
        if (res.ok()) broadcast(QStringLiteral("undone"), QJsonObject{{"importId", res.importId}, {"channel", res.channelId}, {"restored", res.restored.size()}, {"deleted", res.deleted.size()}});
        return {res.status, o};
    }

    Resp importsRoute(const Query& qy) const {
        int limit = qy.value(QStringLiteral("limit"), QStringLiteral("50")).toInt();
        if (limit <= 0) limit = 0;
        QJsonArray a = imp::Importer(store(), nullptr).imports(limit);
        return {200, QJsonObject{{"imports", a}, {"count", a.size()}}};
    }
    Resp importRoute(const QString& id) const {
        const imp::Importer im(store(), nullptr);
        const QJsonArray all = im.imports(0);
        for (const QJsonValue& v : all) {
            const QJsonObject e = v.toObject();
            if (e.value(QStringLiteral("id")).toString() != id) continue;
            QJsonObject b = im.backup(id);
            QJsonObject bs;
            if (!b.isEmpty()) {
                bs = QJsonObject{{"importId", b.value(QStringLiteral("importId"))}, {"channel", b.value(QStringLiteral("channel"))}, {"at", b.value(QStringLiteral("at"))},
                                 {"order", b.value(QStringLiteral("order"))}, {"slots", b.value(QStringLiteral("slots")).toObject().size()}};
            }
            return {200, QJsonObject{{"entry", e}, {"backup", bs}, {"undoable", !b.isEmpty()}}};
        }
        return err(404, QStringLiteral("unknown import '%1'").arg(id));
    }

    struct ExportRun {
        Resp resp;
        ex::Result result;
        int slotCount = 0;
    };
    ExportRun exportRun(const QJsonObject& body) const {
        ExportRun e;
        QJsonObject m = body;
        const QList<ini::PathRule> r = rules(body);
        m[QStringLiteral("pathMap")] = rulesJson(r);
        QStringList errors;
        ex::Options o = ex::Options::fromJson(m, &errors);
        errors << o.check();
        if (o.channelId.isEmpty()) errors << QStringLiteral("channel required");
        else { bool found = false; store()->channel(o.channelId, &found); if (!found) errors << QStringLiteral("unknown channel '%1'").arg(o.channelId); }
        QDate from, to;
        if (errors.isEmpty() && o.mode == QLatin1String("dates")) {
            from = QDate::fromString(body.value(QStringLiteral("from")).toString().trimmed(), Qt::ISODate);
            to = body.contains(QStringLiteral("to")) ? QDate::fromString(body.value(QStringLiteral("to")).toString().trimmed(), Qt::ISODate) : from;
            if (!from.isValid()) errors << QStringLiteral("dates mode needs from (YYYY-MM-DD)");
            else if (!to.isValid() || to < from) errors << QStringLiteral("bad range");
            else if (from.daysTo(to) >= 366) errors << QStringLiteral("range over 366 days");
        }
        if (!errors.isEmpty()) { e.resp = err(400, QStringLiteral("bad export request: %1").arg(errors.join(QStringLiteral("; "))), QJsonObject{{"errors", strings(errors)}}); return e; }
        const QList<sched::HourSlot> input = o.mode == QLatin1String("dates") ? ex::Exporter::dateSlots(*store(), o.channelId, from, to) : ex::Exporter::gridSlots(*store(), o.channelId);
        o.segmentIndex = ex::Exporter::segmentIndexFrom(store());
        e.result = ex::Exporter(o).run(input);
        e.slotCount = input.size();
        if (!e.result.ok()) { e.resp = err(400, QStringLiteral("export failed: %1").arg(e.result.errors.join(QStringLiteral("; "))), e.result.toJson(false)); return e; }
        QJsonObject out = e.result.toJson(truthy(body.value(QStringLiteral("withText")), true));
        out[QStringLiteral("slots")] = e.slotCount;
        out[QStringLiteral("bytes")] = double(e.result.bytes.size());
        if (truthy(body.value(QStringLiteral("download")))) out[QStringLiteral("bytesBase64")] = QString::fromLatin1(e.result.bytes.toBase64());
        e.resp = {200, out};
        return e;
    }

    Resp browseRoute(const Query& qy) const {
        const QString sourceId = qy.value(QStringLiteral("source")).trimmed();
        QString dir = ini::normalizePath(qy.value(QStringLiteral("dir")).trimmed());
        QString node = qy.value(QStringLiteral("node")).trimmed();
        Source src;
        if (!sourceId.isEmpty()) {
            src = sourceById(sourceId);
            if (!src.valid) return err(404, QStringLiteral("unknown source '%1'").arg(sourceId));
            if (dir.isEmpty()) {
                const QString sub = qy.value(QStringLiteral("sub")).trimmed();
                if (!safeRelative(sub)) return err(400, QStringLiteral("sub may not leave the source folder"));
                dir = sub.isEmpty() ? ini::normalizePath(src.playlistDir) : joinPath(src.playlistDir, sub);
            }
            if (node.isEmpty()) node = src.node;
        }
        if (dir.isEmpty()) return err(400, QStringLiteral("source or dir required"));
        const bool mesh = src.valid ? src.mesh() : !node.isEmpty();
        const bool quick = truthy(QJsonValue(qy.value(QStringLiteral("quick"), QStringLiteral("1"))), true);
        QJsonArray entries;
        int dated = 0, files = 0, dirs = 0;
        auto entry = [&](const QString& name, const QString& path, bool isDir, qint64 size, const QString& mtime) {
            QJsonObject e{{"name", name}, {"path", path}, {"isDir", isDir}, {"size", double(size)}, {"mtime", mtime}};
            if (!isDir) { const QDate d = datedName(name); e[QStringLiteral("dated")] = d.isValid() ? QJsonValue(d.toString(Qt::ISODate)) : QJsonValue(); if (d.isValid()) ++dated; }
            return e;
        };
        if (mesh) {
            int st = 0;
            QString why;
            const QByteArray bytes = meshGet(meshCfg(), QStringLiteral("/api/browse?") + meshQuery(node, dir), host.meshTimeoutMs, kMaxFileBytes, &st, &why);
            if (st == 404) return err(404, QStringLiteral("not found on the mesh: %1").arg(dir));
            if (st != 200 || !why.isEmpty()) return err(502, QStringLiteral("mesh browse failed: %1").arg(why.isEmpty() ? QStringLiteral("status %1").arg(st) : why));
            const QJsonObject b = QJsonDocument::fromJson(bytes).object();
            QList<QJsonObject> ds, fs;
            for (const QJsonValue& v : b.value(QStringLiteral("items")).toArray()) {
                const QJsonObject it = v.toObject();
                const QString name = it.value(QStringLiteral("name")).toString();
                const bool isDir = it.value(QStringLiteral("isDirectory")).toBool() || it.value(QStringLiteral("isDir")).toBool();
                if (!isDir && !name.endsWith(QLatin1String(".in"), Qt::CaseInsensitive)) continue;
                QString path = it.value(QStringLiteral("path")).toString();
                if (path.isEmpty()) path = joinPath(dir, name);
                QString mtime = it.value(QStringLiteral("mtime")).toString();
                if (mtime.isEmpty() && it.value(QStringLiteral("mtime")).isDouble()) mtime = isoUtc(qint64(it.value(QStringLiteral("mtime")).toDouble()));
                (isDir ? ds : fs) << entry(name, path, isDir, qint64(it.value(QStringLiteral("size")).toDouble()), mtime);
            }
            auto byName = [](const QJsonObject& a, const QJsonObject& b) { return a.value(QStringLiteral("name")).toString().compare(b.value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) < 0; };
            std::sort(ds.begin(), ds.end(), byName);
            std::sort(fs.begin(), fs.end(), byName);
            for (const QJsonObject& e : ds) entries << e;
            for (const QJsonObject& e : fs) entries << e;
            dirs = ds.size();
            files = fs.size();
            QJsonObject o{{"dir", b.value(QStringLiteral("currentPath")).toString(dir)}, {"node", node}, {"read", "mesh"}, {"entries", entries}, {"dirs", dirs}, {"files", files}, {"dated", dated}};
            if (src.valid) o[QStringLiteral("source")] = src.id;
            return {200, o};
        }
        const QDir d(dir);
        if (!d.exists()) return err(404, QStringLiteral("not found: %1").arg(dir));
        int quickLeft = quick ? kMaxQuickFiles : 0;
        for (const QFileInfo& fi : d.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase)) {
            const bool isDir = fi.isDir();
            if (!isDir && fi.suffix().compare(QLatin1String("in"), Qt::CaseInsensitive) != 0) continue;
            QJsonObject e = entry(fi.fileName(), QDir::toNativeSeparators(fi.absoluteFilePath()), isDir, isDir ? 0 : fi.size(), fi.lastModified().toUTC().toString(Qt::ISODate));
            if (isDir) ++dirs; else ++files;
            if (!isDir && quickLeft > 0 && fi.size() <= 4 * 1024 * 1024) {
                --quickLeft;
                QFile f(fi.absoluteFilePath());
                if (f.open(QIODevice::ReadOnly)) {
                    const Document doc = parse(f.readAll());
                    const QJsonObject st = doc.stats();
                    QString first;
                    if (!doc.events.isEmpty()) first = doc.header(doc.events.first()).stamp.raw;
                    e[QStringLiteral("quick")] = QJsonObject{{"events", st.value(QStringLiteral("events"))}, {"media", st.value(QStringLiteral("media"))}, {"instructions", st.value(QStringLiteral("instructions"))},
                                                             {"firstStamp", first}, {"errors", st.value(QStringLiteral("diagnostics")).toObject().value(QStringLiteral("error"))}};
                }
            }
            entries << e;
        }
        QJsonObject o{{"dir", QDir::toNativeSeparators(d.absolutePath())}, {"read", "fs"}, {"entries", entries}, {"dirs", dirs}, {"files", files}, {"dated", dated}};
        const QString parent = res::parentFolder(QDir::toNativeSeparators(d.absolutePath()));
        if (!parent.isEmpty() && parent != QDir::toNativeSeparators(d.absolutePath())) o[QStringLiteral("parent")] = parent;
        if (src.valid) o[QStringLiteral("source")] = src.id;
        return {200, o};
    }

    /// "YYYY-MM-DD" (the start of that day; end: its last ms) or an ISO date-time, in tz. 0 when absent; -1 when bad.
    static qint64 rangeMs(const QJsonValue& v, const QTimeZone& tz, bool end) {
        const QString s = v.toString().trimmed();
        if (s.isEmpty()) return 0;
        const QDate d = QDate::fromString(s, Qt::ISODate);
        if (d.isValid()) {
            const QDateTime start(d, QTime(0, 0), tz);
            return end ? start.addDays(1).toMSecsSinceEpoch() - 1 : start.toMSecsSinceEpoch();
        }
        QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
        if (!dt.isValid()) return -1;
        if (dt.timeSpec() == Qt::LocalTime) dt = QDateTime(dt.date(), dt.time(), tz);
        return dt.toMSecsSinceEpoch();
    }

    Resp asRunRoute(const QJsonObject& body, QJsonObject* stateUpdate) const {
        const Input in = readInput(body, QStringLiteral("log"));
        if (in.status != 200) return err(in.status, in.error);
        const QJsonObject cfg = configJson().value(QStringLiteral("asrun")).toObject();
        QString tzId = body.value(QStringLiteral("tz")).toString().trimmed();
        if (tzId.isEmpty()) tzId = in.src.tz;
        if (tzId.isEmpty()) tzId = cfg.value(QStringLiteral("tz")).toString().trimmed();
        if (tzId.isEmpty()) tzId = kDefaultTz;
        const QTimeZone tz(tzId.toUtf8());
        if (!tz.isValid()) return err(400, QStringLiteral("unknown time zone '%1'").arg(tzId));
        logs::AsRunOptions ao;
        ao.tz = tz;
        ao.capSec = qBound(60, body.value(QStringLiteral("capSec")).toInt(cfg.value(QStringLiteral("capSec")).toInt(3600)), 86400);
        const QString ldo = body.value(QStringLiteral("legacyDateOrder")).toString().trimmed().toLower();
        if (!ldo.isEmpty()) ao.legacyDateOrder = ldo;
        logs::AsRunLog log = logs::parseAsRun(in.bytes, ao);
        // Status.log for the missed-file reasons: given as text / path, else the file next to a file-system log.
        QByteArray statusBytes;
        QString statusFile;
        bool statusTried = false;
        if (body.value(QStringLiteral("statusText")).isString()) { statusBytes = body.value(QStringLiteral("statusText")).toString().toUtf8(); statusFile = QStringLiteral("statusText"); statusTried = true; }
        else {
            QString sp = body.value(QStringLiteral("statusPath")).toString().trimmed();
            if (!sp.isEmpty() || (in.read == QLatin1String("fs") && !in.file.isEmpty())) {
                QJsonObject sb = body;
                sb.remove(QStringLiteral("text"));
                if (sp.isEmpty()) sp = joinPath(res::parentFolder(in.file), QStringLiteral("Status.log"));
                sb[QStringLiteral("path")] = sp;
                const bool required = !body.value(QStringLiteral("statusPath")).toString().trimmed().isEmpty();
                if (required || QFileInfo::exists(sp)) {
                    statusTried = true;
                    const Input st = readInput(sb, QStringLiteral("log"));
                    if (st.status != 200) { if (required) return err(st.status, QStringLiteral("status log: %1").arg(st.error)); }
                    else { statusBytes = st.bytes; statusFile = st.file; }
                }
            }
        }
        QJsonObject status;
        QJsonArray diags;
        for (const Diagnostic& d : log.diagnostics) diags << d.toJson();
        if (!statusBytes.isEmpty()) {
            const logs::MessageLog ml = logs::parseMessages(statusBytes);
            const int attached = logs::attachReasons(log, ml);
            status = QJsonObject{{"file", statusFile}, {"attached", attached}, {"stats", ml.stats()}, {"report", ml.report()}};
            for (const Diagnostic& d : ml.diagnostics) { QJsonObject j = d.toJson(); j[QStringLiteral("file")] = QStringLiteral("status"); diags << j; }
        } else if (statusTried) status = QJsonObject{{"file", statusFile}, {"attached", 0}};
        logs::AsRunWriteOptions wo;
        wo.channelId = body.value(QStringLiteral("channelAs")).toString().trimmed();
        if (wo.channelId.isEmpty()) wo.channelId = cfg.value(QStringLiteral("channelAs")).toString().trimmed();
        if (wo.channelId.isEmpty()) wo.channelId = QStringLiteral("mp-") + (in.sourceId.isEmpty() ? QStringLiteral("import") : in.sourceId);
        wo.source = in.sourceId.isEmpty() ? in.file : in.sourceId;
        if (wo.source.isEmpty()) wo.source = in.name;
        wo.fromMs = rangeMs(body.value(QStringLiteral("from")), tz, false);
        wo.toMs = rangeMs(body.value(QStringLiteral("to")), tz, true);
        if (wo.fromMs < 0 || wo.toMs < 0) return err(400, QStringLiteral("from / to: YYYY-MM-DD or an ISO date-time"));
        if (wo.fromMs && wo.toMs && wo.toMs < wo.fromMs) return err(400, QStringLiteral("to lies before from"));
        wo.includeMissed = truthy(body.value(QStringLiteral("includeMissed")), truthy(cfg.value(QStringLiteral("includeMissed")), true));
        wo.includeCommands = truthy(body.value(QStringLiteral("includeCommands")), truthy(cfg.value(QStringLiteral("includeCommands")), false));
        wo.countTowardContracts = truthy(body.value(QStringLiteral("countTowardContracts")), truthy(cfg.value(QStringLiteral("countTowardContracts")), false));
        wo.sampleRows = qBound(0, body.value(QStringLiteral("sample")).toInt(20), 500);
        const QList<ini::PathRule> r = rules(body);
        res::Options ro = resolverOptions(body, r, QString());
        ro.useHash = false;       // a log line names a file that aired; never read the disk for it
        ro.probeFiles = false;
        const res::Resolver resolver = res::Resolver::fromStore(*store(), ro);
        wo.resolve = [&resolver](const logs::AsRunEntry& e) { const res::Resolution x = resolver.resolve(e.path, -1); return QPair<QString, QString>(x.mediaId, x.type); };
        const bool dryRun = truthy(body.value(QStringLiteral("dryRun")), true);
        const logs::AsRunWriteResult res = logs::writeAsRun(store()->path(), log, wo, dryRun);
        QJsonObject o = res.toJson();
        o[QStringLiteral("channel")] = wo.channelId;
        o[QStringLiteral("tz")] = tzId;
        o[QStringLiteral("source")] = inputJson(in);
        o[QStringLiteral("stats")] = log.stats();
        o[QStringLiteral("diagnostics")] = diags;
        if (!status.isEmpty()) o[QStringLiteral("status")] = status;
        o[QStringLiteral("range")] = QJsonObject{{"fromMs", double(wo.fromMs)}, {"toMs", double(wo.toMs)}};
        if (!res.ok()) return err(res.error.contains(QLatin1String("no such")) ? 404 : 500, QStringLiteral("as-run import failed: %1").arg(res.error), o);
        if (!dryRun) {
            const QString key = in.sourceId.isEmpty() ? QStringLiteral("adhoc") : in.sourceId;
            const logs::AsRunEntry* last = log.entries.isEmpty() ? nullptr : &log.entries.last();
            QJsonObject st{{"at", isoUtc(now())}, {"channel", wo.channelId}, {"file", in.file.isEmpty() ? in.name : in.file}, {"read", in.read}, {"inserted", res.inserted},
                           {"duplicates", res.duplicates}, {"entries", log.entries.size()}, {"lastStamp", last ? last->stamp() : QString()}, {"lastKey", last ? last->key : QString()},
                           {"lastLine", last ? last->line : 0}, {"by", body.value(QStringLiteral("by")).toString(QStringLiteral("rest"))}};
            *stateUpdate = QJsonObject{{key, st}};
            o[QStringLiteral("state")] = st;
        }
        return {200, o};
    }

    Resp fromIniRoute(const QJsonObject& body) const {
        const Input in = readInput(body, QStringLiteral("ini"));
        if (in.status != 200) return err(in.status, in.error);
        const ini::MplayIni mi = ini::readMplayIni(in.bytes);
        QJsonArray diags;
        for (const Diagnostic& d : mi.diagnostics) diags << d.toJson();
        if (!mi.isMplayIni) return err(400, QStringLiteral("not an Mplay.INI (no [MPEGPlay] / [PlayListEditor] section)"), QJsonObject{{"isMplayIni", false}, {"diagnostics", diags}, {"source", inputJson(in)}});
        ini::SuggestOptions so;
        so.iniPath = in.read == QLatin1String("text") ? QString() : in.file;
        const ini::SourceSuggestion sg = ini::suggestSource(mi, so);
        QJsonObject o = sg.toJson(mi);
        o[QStringLiteral("source")] = sg.source;   // toJson(mi) writes it; restated so the key is certain
        o[QStringLiteral("input")] = inputJson(in);
        if (in.read != QLatin1String("mesh")) {   // the guess rules want confirmation: does this station see those folders?
            QJsonObject ex;
            const QJsonObject s = sg.source;
            for (const char* k : {"playlistDir", "logPath", "osdDir"}) {
                const QString p = s.value(QLatin1String(k)).toString();
                ex[QLatin1String(k)] = p.isEmpty() ? QJsonValue() : QJsonValue(QFileInfo::exists(p));
            }
            QJsonArray rm;
            for (const ini::PathRule& r : sg.pathMap) { QJsonObject j = r.toJson(); j[QStringLiteral("exists")] = QFileInfo::exists(r.to); rm << j; }
            o[QStringLiteral("exists")] = ex;
            o[QStringLiteral("pathMap")] = rm;
        }
        return {200, o};
    }

    Resp resolveRoute(const QJsonObject& body) const {
        const QJsonValue pv = body.value(QStringLiteral("paths"));
        if (!pv.isArray() || pv.toArray().isEmpty()) return err(400, QStringLiteral("paths[] required"));
        const QJsonArray paths = pv.toArray();
        if (paths.size() > kMaxResolvePaths) return err(400, QStringLiteral("at most %1 paths per call").arg(kMaxResolvePaths));
        const QList<ini::PathRule> r = rules(body);
        QJsonObject m = body;
        if (!body.value(QStringLiteral("resolver")).isObject()) { QJsonObject ro; for (const char* k : {"substitutes", "searchPaths", "searchSubDirs", "hash", "probe", "fileBudgetMs", "candidates", "minScore"}) if (body.contains(QLatin1String(k))) ro[QLatin1String(k)] = body.value(QLatin1String(k)); m[QStringLiteral("resolver")] = ro; }
        const res::Resolver resolver = res::Resolver::fromStore(*store(), resolverOptions(m, r, body.value(QStringLiteral("channel")).toString()));
        QJsonArray results;
        int resolved = 0, missing = 0, live = 0, ambiguous = 0;
        QJsonObject by;
        for (const QJsonValue& v : paths) {
            const QString p = v.isString() ? v.toString() : v.toObject().value(QStringLiteral("path")).toString();
            const res::Resolution x = resolver.resolve(p);
            results << x.toJson();
            if (x.resolved()) ++resolved; else if (x.reportable()) ++missing; else ++live;
            if (x.ambiguous) ++ambiguous;
            const QString bn = res::resolvedByName(x.by);
            by[bn] = by.value(bn).toInt() + 1;
        }
        return {200, QJsonObject{{"results", results}, {"stats", QJsonObject{{"paths", paths.size()}, {"resolved", resolved}, {"missing", missing}, {"liveOrUrl", live}, {"ambiguous", ambiguous}, {"byResolvedBy", by}}},
                                 {"pathMap", rulesJson(r)}, {"library", resolver.size()}}};
    }

    // ---- links --------------------------------------------------------------------------------------------------------
    QJsonArray linksArray() const { return store() ? store()->settingJson(kLinksKey).value(QStringLiteral("links")).toArray() : QJsonArray(); }
    bool saveLinks(const QJsonArray& a) { return store() && store()->setSettingJson(kLinksKey, QJsonObject{{"links", a}}); }
    Resp linkPost(const QJsonObject& body) {
        const QString channel = body.value(QStringLiteral("channel")).toString().trimmed();
        const QString sub = body.value(QStringLiteral("subPath")).toString().trimmed();
        if (channel.isEmpty() || sub.isEmpty()) return err(400, QStringLiteral("channel and subPath required"));
        static const QRegularExpression idRx(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
        QString id = body.value(QStringLiteral("id")).toString().trimmed();
        if (!id.isEmpty() && !idRx.match(id).hasMatch()) return err(400, QStringLiteral("bad link id"));
        QJsonArray slotIds;
        for (const QJsonValue& v : body.value(QStringLiteral("slots")).toArray()) if (v.isString() && !v.toString().isEmpty()) slotIds << v;
        QJsonArray all = linksArray();
        const QString at = isoUtc(now());
        if (id.isEmpty()) {
            const QByteArray seed = (channel + '|' + sub + '|' + QString::number(now()) + '|' + QString::number(all.size())).toUtf8();
            id = QStringLiteral("lnk-") + QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Sha1).toHex().left(8));
        }
        QJsonObject link;
        int at_i = -1;
        for (int i = 0; i < all.size(); ++i) if (all[i].toObject().value(QStringLiteral("id")).toString() == id) { at_i = i; link = all[i].toObject(); break; }
        for (const char* k : {"source", "node", "importId", "options", "lastMtime", "sha256", "label"}) if (body.contains(QLatin1String(k))) link[QLatin1String(k)] = body.value(QLatin1String(k));
        link[QStringLiteral("id")] = id;
        link[QStringLiteral("channel")] = channel;
        link[QStringLiteral("subPath")] = ini::normalizePath(sub);
        link[QStringLiteral("slots")] = slotIds;
        link[QStringLiteral("enabled")] = truthy(body.value(QStringLiteral("enabled")), link.value(QStringLiteral("enabled")).toBool(true));
        if (at_i < 0) link[QStringLiteral("createdAt")] = at;
        link[QStringLiteral("updatedAt")] = at;
        if (at_i < 0) all << link; else all[at_i] = link;
        if (!saveLinks(all)) return err(500, QStringLiteral("could not save the links"));
        broadcast(QStringLiteral("links"), QJsonObject{{"count", all.size()}});
        return {200, QJsonObject{{"link", link}, {"links", all}, {"created", at_i < 0}}};
    }
    Resp linkDelete(const QString& id) {
        QJsonArray all = linksArray();
        for (int i = 0; i < all.size(); ++i) {
            if (all[i].toObject().value(QStringLiteral("id")).toString() != id) continue;
            all.removeAt(i);
            if (!saveLinks(all)) return err(500, QStringLiteral("could not save the links"));
            broadcast(QStringLiteral("links"), QJsonObject{{"count", all.size()}});
            return {200, QJsonObject{{"deleted", id}, {"links", all}}};
        }
        return err(404, QStringLiteral("unknown link '%1'").arg(id));
    }

    // ---- the table ----------------------------------------------------------------------------------------------------
    void buildRoutes() {
        const QString P = QString::fromLatin1(kPrefix);
        auto add = [this](const char* method, const QString& path, Handler h) { routes << Route{QString::fromLatin1(method), path, std::move(h)}; };
        auto sync = [](Reply& reply, const Resp& r) { reply(r.status, r.body); };
        add("GET", P, [this, sync](const QJsonObject&, const Query&, Reply reply) { sync(reply, info()); });
        add("GET", P + "/catalog", [this, sync](const QJsonObject&, const Query&, Reply reply) { sync(reply, catalog()); });
        add("POST", P + "/parse", [this](const QJsonObject& b, const Query&, Reply reply) { run([this, b]() { return parseRoute(b); }, std::move(reply)); });
        add("POST", P + "/import", [this](const QJsonObject& b, const Query&, Reply reply) {
            auto ingestList = std::make_shared<QJsonArray>();
            auto applied = std::make_shared<bool>(false);
            run([this, b, ingestList, applied]() { return importRoute(b, ingestList.get(), applied.get()); }, std::move(reply), [this, ingestList, applied](Resp& r) {
                if (!*applied) return;
                // missing = ingest: the plan's list goes to the IngestManager now, on the service's thread (MP-08b: ingest[]).
                QJsonArray enqueued, skipped;
                for (const QJsonValue& v : *ingestList) {
                    const QJsonObject e = v.toObject();
                    const QString path = e.value(QStringLiteral("mapped")).toString(e.value(QStringLiteral("path")).toString());
                    const QJsonValue ex = e.value(QStringLiteral("exists"));
                    if (ex.isBool() && !ex.toBool()) { skipped << QJsonObject{{"path", path}, {"why", "not found"}}; continue; }
                    if (!host.enqueueIngest) { skipped << QJsonObject{{"path", path}, {"why", "no ingest manager"}}; continue; }
                    const QJsonObject meta{{"origin", "masterplay"}, {"importId", r.body.value(QStringLiteral("importId"))}, {"playlistPath", e.value(QStringLiteral("path"))}};
                    QJsonObject job = host.enqueueIngest(path, QString(), meta);
                    job[QStringLiteral("path")] = path;
                    enqueued << job;
                }
                r.body[QStringLiteral("ingest")] = QJsonObject{{"requested", ingestList->size()}, {"enqueued", enqueued}, {"skipped", skipped}};
                broadcast(QStringLiteral("applied"), QJsonObject{{"importId", r.body.value(QStringLiteral("importId"))}, {"channel", r.body.value(QStringLiteral("channel"))},
                                                                 {"written", r.body.value(QStringLiteral("written")).toArray().size()}, {"onAir", r.body.value(QStringLiteral("onAirSlots")).toArray().size()}});
            });
        });
        add("POST", P + "/import/{id}/undo", [this, sync](const QJsonObject& b, const Query& qy, Reply reply) { sync(reply, undoRoute(qy.value(QStringLiteral("id")), b)); });
        add("GET", P + "/imports", [this, sync](const QJsonObject&, const Query& qy, Reply reply) { sync(reply, importsRoute(qy)); });
        add("GET", P + "/imports/{id}", [this, sync](const QJsonObject&, const Query& qy, Reply reply) { sync(reply, importRoute(qy.value(QStringLiteral("id")))); });
        add("POST", P + "/export", [this](const QJsonObject& b, const Query& qy, Reply reply) {
            QJsonObject body = b;
            if (qy.contains(QStringLiteral("download")) && !body.contains(QStringLiteral("download"))) body[QStringLiteral("download")] = truthy(QJsonValue(qy.value(QStringLiteral("download"))));
            run([this, body]() { return exportRun(body).resp; }, std::move(reply));
        });
        add("GET", P + "/browse", [this](const QJsonObject&, const Query& qy, Reply reply) { run([this, qy]() { return browseRoute(qy); }, std::move(reply)); });
        add("POST", P + "/asrun/import", [this](const QJsonObject& b, const Query&, Reply reply) {
            auto state = std::make_shared<QJsonObject>();
            run([this, b, state]() { return asRunRoute(b, state.get()); }, std::move(reply), [this, state](Resp& r) {
                if (state->isEmpty() || r.status != 200) return;
                QJsonObject all = q->asRunState();
                for (auto it = state->begin(); it != state->end(); ++it) all[it.key()] = it.value();
                if (store()) store()->setSettingJson(kAsRunStateKey, all);
                broadcast(QStringLiteral("asrun"), QJsonObject{{"channel", r.body.value(QStringLiteral("channel"))}, {"inserted", r.body.value(QStringLiteral("inserted"))}});
            });
        });
        add("GET", P + "/asrun/state", [this, sync](const QJsonObject&, const Query&, Reply reply) { sync(reply, Resp{200, QJsonObject{{"state", q->asRunState()}}}); });
        add("GET", P + "/config", [this, sync](const QJsonObject&, const Query&, Reply reply) { sync(reply, Resp{200, q->config()}); });
        add("PUT", P + "/config", [this, sync](const QJsonObject& b, const Query&, Reply reply) {
            QString e;
            const QJsonObject c = q->setConfig(b, &e);
            sync(reply, e.isEmpty() ? Resp{200, c} : err(400, e));
        });
        add("POST", P + "/config/from-ini", [this](const QJsonObject& b, const Query&, Reply reply) { run([this, b]() { return fromIniRoute(b); }, std::move(reply)); });
        add("POST", P + "/resolve", [this](const QJsonObject& b, const Query&, Reply reply) { run([this, b]() { return resolveRoute(b); }, std::move(reply)); });
        add("GET", P + "/links", [this, sync](const QJsonObject&, const Query&, Reply reply) { const QJsonArray a = linksArray(); sync(reply, Resp{200, QJsonObject{{"links", a}, {"count", a.size()}}}); });
        add("POST", P + "/links", [this, sync](const QJsonObject& b, const Query&, Reply reply) { sync(reply, linkPost(b)); });
        add("DELETE", P + "/links/{id}", [this, sync](const QJsonObject&, const Query& qy, Reply reply) { sync(reply, linkDelete(qy.value(QStringLiteral("id")))); });
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// MpService

MpService::MpService(const Host& host, QObject* parent) : QObject(parent), d(std::make_unique<Impl>()) {
    d->q = this;
    d->host = host;
    d->pool.setMaxThreadCount(qBound(1, host.maxThreads, 8));
    d->pool.setExpiryTimeout(30000);
    d->buildRoutes();
}

MpService::~MpService() { d->pool.waitForDone(); }

const Host& MpService::host() const { return d->host; }
const QList<Route>& MpService::routes() const { return d->routes; }

QStringList MpService::routeKeys() const {
    QStringList k;
    for (const Route& r : d->routes) k << r.method + QLatin1Char(' ') + r.path;
    k << QStringLiteral("GET ") + QString::fromLatin1(kPrefix) + QStringLiteral("/export.in");
    std::sort(k.begin(), k.end());
    return k;
}

bool MpService::call(const QString& method, const QString& path, const QJsonObject& body, const Query& query, Reply reply) const {
    const QString m = method.toUpper();
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const Route& r : d->routes) {
        if (r.method != m) continue;
        if (r.path == path) { r.handle(body, query, std::move(reply)); return true; }
        const QStringList rp = r.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (rp.size() != parts.size()) continue;
        Query qy = query;
        bool match = true;
        for (int i = 0; i < rp.size() && match; ++i) {
            if (rp[i].startsWith(QLatin1Char('{')) && rp[i].endsWith(QLatin1Char('}'))) qy[rp[i].mid(1, rp[i].size() - 2)] = parts[i];
            else if (rp[i] != parts[i]) match = false;
        }
        if (!match) continue;
        r.handle(body, qy, std::move(reply));
        return true;
    }
    return false;
}

int MpService::callSync(const QString& method, const QString& path, const QJsonObject& body, QJsonObject* out, const Query& query, int timeoutMs) const {
    struct State { int status = 0; bool done = false; QJsonObject body; QEventLoop loop; };
    auto st = std::make_shared<State>();
    const bool routed = call(method, path, body, query, [st](int status, const QJsonObject& b) { st->status = status; st->body = b; st->done = true; st->loop.quit(); });
    if (!routed) return 0;
    if (!st->done) {
        QTimer::singleShot(timeoutMs, &st->loop, &QEventLoop::quit);
        st->loop.exec();
    }
    if (out) *out = st->body;
    return st->done ? st->status : 0;
}

void MpService::exportFile(const Query& query, std::function<void(const FileReply&)> done) const {
    QJsonObject body = queryToJson(query);
    body.remove(QStringLiteral("token"));
    body[QStringLiteral("withText")] = false;
    auto out = std::make_shared<FileReply>();
    d->run([this, body, out]() {
        const Impl::ExportRun e = d->exportRun(body);
        if (e.resp.status != 200) { out->status = e.resp.status; out->contentType = QByteArrayLiteral("application/json"); out->bytes = QJsonDocument(e.resp.body).toJson(QJsonDocument::Compact); }
        else { out->bytes = e.result.bytes; out->filename = e.result.filename; }
        return e.resp;
    }, [done, out](int, const QJsonObject&) { done(*out); });
}

QJsonObject MpService::config() const { return d->configJson(); }

QJsonObject MpService::setConfig(const QJsonObject& c, QString* error) {
    QStringList problems;
    QJsonObject out;
    static const QRegularExpression idRx(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    QJsonArray sources;
    QSet<QString> ids;
    const QJsonValue sv = c.value(QStringLiteral("sources"));
    if (c.contains(QStringLiteral("sources")) && !sv.isArray()) problems << QStringLiteral("sources must be a list");
    for (const QJsonValue& v : sv.toArray()) {
        if (!v.isObject()) { problems << QStringLiteral("a source must be an object"); continue; }
        const Source s = Source::fromJson(v.toObject());
        if (!idRx.match(s.id).hasMatch()) { problems << QStringLiteral("source id '%1' is not [A-Za-z0-9_.-]{1,64}").arg(s.id); continue; }
        if (ids.contains(s.id.toLower())) { problems << QStringLiteral("source id '%1' is used twice").arg(s.id); continue; }
        ids.insert(s.id.toLower());
        if (s.read != QLatin1String("fs") && s.read != QLatin1String("mesh")) problems << QStringLiteral("source '%1': read must be fs or mesh").arg(s.id);
        if (s.mesh() && s.node.isEmpty()) problems << QStringLiteral("source '%1': a mesh source needs a node").arg(s.id);
        if (!s.tz.isEmpty() && !QTimeZone(s.tz.toUtf8()).isValid()) problems << QStringLiteral("source '%1': unknown time zone '%2'").arg(s.id, s.tz);
        sources << s.toJson();
    }
    QJsonArray pathMap;
    const QJsonValue pv = c.value(QStringLiteral("pathMap"));
    if (c.contains(QStringLiteral("pathMap")) && !pv.isArray()) problems << QStringLiteral("pathMap must be a list");
    for (const QJsonValue& v : pv.toArray()) {
        const ini::PathRule r = ini::PathRule::fromJson(v.toObject());
        if (r.from.trimmed().isEmpty()) { problems << QStringLiteral("a path rule needs from"); continue; }
        pathMap << r.toJson();
    }
    QJsonObject defaults = c.value(QStringLiteral("defaults")).toObject();
    if (c.contains(QStringLiteral("defaults")) && !c.value(QStringLiteral("defaults")).isObject()) problems << QStringLiteral("defaults must be an object");
    {
        QStringList errs;
        QJsonObject probe = defaults;
        probe.remove(QStringLiteral("channel"));
        probe.remove(QStringLiteral("channelId"));
        probe.remove(QStringLiteral("nowMs"));
        imp::Options::fromJson(probe, &errs);
        for (const QString& e : errs) problems << QStringLiteral("defaults: %1").arg(e);
        defaults = probe;
    }
    for (const char* k : {"resolver", "asrun"})
        if (c.contains(QLatin1String(k)) && !c.value(QLatin1String(k)).isObject()) problems << QStringLiteral("%1 must be an object").arg(QLatin1String(k));
    const QJsonObject asrun = c.value(QStringLiteral("asrun")).toObject();
    const QString atz = asrun.value(QStringLiteral("tz")).toString().trimmed();
    if (!atz.isEmpty() && !QTimeZone(atz.toUtf8()).isValid()) problems << QStringLiteral("asrun.tz: unknown time zone '%1'").arg(atz);
    if (!problems.isEmpty()) { if (error) *error = problems.join(QStringLiteral("; ")); return {}; }
    out[QStringLiteral("sources")] = sources;
    out[QStringLiteral("pathMap")] = pathMap;
    out[QStringLiteral("defaults")] = defaults;
    out[QStringLiteral("resolver")] = c.value(QStringLiteral("resolver")).toObject();
    out[QStringLiteral("asrun")] = asrun;
    out[QStringLiteral("updatedAt")] = isoUtc(d->now());
    if (!d->store() || !d->store()->setSettingJson(kConfigKey, out)) { if (error) *error = QStringLiteral("could not save the config"); return {}; }
    d->broadcast(QStringLiteral("config"), QJsonObject{{"sources", sources.size()}, {"pathMap", pathMap.size()}});
    if (error) error->clear();
    return d->configJson();
}

QJsonObject MpService::source(const QString& id) const {
    const Source s = d->sourceById(id);
    return s.valid ? s.toJson() : QJsonObject();
}

QJsonArray MpService::links() const { return d->linksArray(); }

QJsonObject MpService::asRunState() const { return d->store() ? d->store()->settingJson(kAsRunStateKey) : QJsonObject(); }

int MpService::pending() const { return d->pending.load(); }

QDate MpService::datedName(const QString& fileName) {
    static const QRegularExpression rx(QStringLiteral("(?<!\\d)(\\d{4})[-_.]?(\\d{2})[-_.]?(\\d{2})(?!\\d)"));
    const QRegularExpressionMatch m = rx.match(fileName);
    if (!m.hasMatch()) return {};
    const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    return d.isValid() && d.year() >= 1990 && d.year() <= 2100 ? d : QDate();
}

} // namespace mp::svc
