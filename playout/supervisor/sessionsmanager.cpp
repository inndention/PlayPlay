// SessionsManager — station state bundles (capture / clone / export / import / apply / replicate).
#include "sessionsmanager.h"
#include "../schedule/schedulestore.h"
#include "../schedule/schedulemodel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSysInfo>
#include <QUuid>
#include <QDebug>
#include <algorithm>

using namespace sched;

// ---------------------------------------------------------------------------
// Policy tables
// ---------------------------------------------------------------------------

static const QStringList kGroups = {"channels", "schedule", "media", "settings", "cg", "spots", "programming"};

// Exact local-only keys (identity, bind/auth, pairing, secrets) and prefixes.
static const QSet<QString> kLocalKeys = {
    "rest.token", "rest.bind", "rest.port", "ws.port", "station.password", "station.port",
    "failover.config", "failover.epoch", "remote.nodes", "node.id", "node.name",
    "mesh.anfsToken", "mesh.anfsCookie", "alerts.twilio",
};
static const QStringList kLocalPrefixes = {"failover.", "redundancy.", "node.", "rest.", "remote.", "mesh.anfs", "alerts.twilio", "local."};
static const QStringList kProgrammingPrefixes = {"recipes.", "traffic.", "clock.", "programming.", "hour_recipes"};
// Credentials the station needs to work but that must not travel in a session file: the spots LLM api key, the Star agent
// tokens and the ONVIF camera passwords. Left out of stored/exported/pushed sessions; exportLive(includeSecrets) carries them
// to an authenticated failover peer so a promoted backup can still generate spots, drive cameras and reach its Star agents.
static const QSet<QString> kSecretKeys = {"spots.llm", "star.agents", "onvif.devices"};

QStringList SessionsManager::allGroups() { return kGroups; }

bool SessionsManager::isSecretSettingKey(const QString& key) { return kSecretKeys.contains(key); }

// A session id is a file name and arrives from request bodies, URLs and peers, so it must never be able to name another
// path: only the exact shape newId() produces maps to a file ("../x", "a\\b", "index", Windows device names are all out).
bool SessionsManager::isValidSessionId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[0-9]{14}-[0-9a-f]{4}$"));
    return id.size() == 19 && re.match(id).hasMatch();
}

QJsonObject SessionsManager::withoutSecrets(QJsonObject wire) {
    QJsonObject g = wire.value("groups").toObject();
    bool touched = false;
    for (const char* grp : {"settings", "cg", "spots", "programming"}) {
        if (!g.contains(grp)) continue;
        QJsonObject kv = g.value(grp).toObject();
        for (const QString& k : kv.keys()) if (kSecretKeys.contains(k)) { kv.remove(k); touched = true; }
        g[grp] = kv;
    }
    if (!touched) return wire;
    wire["groups"] = g;
    wire["summary"] = summaryOf(wire);
    return wire;
}

bool SessionsManager::isLocalSettingKey(const QString& key) {
    if (kLocalKeys.contains(key)) return true;
    for (const QString& p : kLocalPrefixes) if (key.startsWith(p)) return true;
    return false;
}

QString SessionsManager::settingGroup(const QString& key) {
    if (isLocalSettingKey(key)) return QString();
    if (key.startsWith("cg.")) return "cg";
    if (key.startsWith("spots.")) return "spots";
    for (const QString& p : kProgrammingPrefixes) if (key.startsWith(p)) return "programming";
    return "settings";
}

// ---------------------------------------------------------------------------
// Construction / files
// ---------------------------------------------------------------------------

SessionsManager::SessionsManager(ScheduleStore* store, const QString& dataDir, QObject* parent)
    : QObject(parent), m_store(store) {
    m_dir = QDir(QDir::fromNativeSeparators(dataDir)).filePath("sessions");
    QDir().mkpath(m_dir);
    m_roName = "sessions_ro_" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    loadIndex();
    qInfo() << "[Sessions]" << m_index.size() << "session(s) in" << QDir::toNativeSeparators(m_dir);
}

SessionsManager::~SessionsManager() {
    if (m_ro.isValid()) { m_ro.close(); m_ro = QSqlDatabase(); QSqlDatabase::removeDatabase(m_roName); }
}

QString SessionsManager::newId() {
    return QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss") + "-" + QString("%1").arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QChar('0'));
}

void SessionsManager::loadIndex() {
    m_index.clear();
    QFile f(m_dir + "/index.json");
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).object().value("sessions").toArray()) {
            const QJsonObject row = v.toObject();
            if (!row.value("id").toString().isEmpty() && QFileInfo::exists(filePath(row.value("id").toString()))) m_index.append(row);
        }
        return;
    }
    // No index: rebuild from the files present.
    for (const QFileInfo& fi : QDir(m_dir).entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        if (fi.fileName() == "index.json") continue;
        bool ok = false; const QJsonObject w = readSession(fi.completeBaseName(), &ok);
        if (ok && w.value("id").toString() == fi.completeBaseName()) m_index.append(rowOf(w));
    }
    if (!m_index.isEmpty()) saveIndex();
}

bool SessionsManager::saveIndex() const {
    QJsonArray a; for (const QJsonObject& r : m_index) a.append(r);
    QJsonObject o; o["sessions"] = a; o["savedAt"] = double(QDateTime::currentMSecsSinceEpoch());
    QFile f(m_dir + "/index.json");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { qWarning() << "[Sessions] cannot write index:" << f.errorString(); return false; }
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return true;
}

int SessionsManager::indexOf(const QString& id) const {
    for (int i = 0; i < m_index.size(); ++i) if (m_index[i].value("id").toString() == id) return i;
    return -1;
}

bool SessionsManager::writeSession(const QJsonObject& wire) const {
    const QString id = wire.value("id").toString();
    if (!isValidSessionId(id)) { qWarning() << "[Sessions] refusing to write a session with an invalid id" << id.left(40); return false; }
    QFile f(filePath(id));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { qWarning() << "[Sessions] cannot write" << f.fileName() << f.errorString(); return false; }
    f.write(QJsonDocument(wire).toJson(QJsonDocument::Compact));
    return true;
}

QJsonObject SessionsManager::readSession(const QString& id, bool* ok) const {
    if (!isValidSessionId(id)) { if (ok) *ok = false; return QJsonObject(); }   // never builds a path from an unvalidated id
    QFile f(filePath(id));
    if (!f.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return QJsonObject(); }
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
    if (ok) *ok = d.isObject();
    return withoutSecrets(d.object());     // sessions captured before secrets were kept out never hand them back
}

QJsonObject SessionsManager::rowOf(const QJsonObject& w) {
    QJsonObject r;
    for (const char* k : {"id", "name", "notes", "version", "createdAt", "updatedAt", "node", "origin", "lastSyncedAt"}) if (w.contains(k)) r[k] = w.value(k);
    r["summary"] = w.contains("summary") ? w.value("summary") : summaryOf(w);
    return r;
}

QJsonObject SessionsManager::summaryOf(const QJsonObject& bundle) {
    const QJsonObject g = bundle.value("groups").toObject();
    QJsonObject s;
    s["channels"] = g.value("channels").toArray().size();
    s["schedule"] = g.value("schedule").toArray().size();
    s["media"] = g.value("media").toArray().size();
    for (const char* k : {"settings", "cg", "spots", "programming"}) s[k] = g.value(k).toObject().size();
    s["mediaRefs"] = bundle.value("mediaManifest").toObject().size();
    QStringList present; for (auto it = g.begin(); it != g.end(); ++it) present << it.key();
    s["groups"] = QJsonArray::fromStringList(present);
    return s;
}

// ---------------------------------------------------------------------------
// Read-only SQL (enumeration + marks). The store API has no "all settings" / "all slots"
// accessor, so a second connection to the same SQLite file runs SELECTs only.
// ---------------------------------------------------------------------------

bool SessionsManager::ensureRo() const {
    if (m_ro.isValid() && m_ro.isOpen()) return true;
    if (!m_store || m_store->path().isEmpty()) return false;
    m_ro = QSqlDatabase::addDatabase("QSQLITE", m_roName);
    m_ro.setDatabaseName(m_store->path());
    if (!m_ro.open()) { qWarning() << "[Sessions] read-only connection failed:" << m_ro.lastError().text(); return false; }
    QSqlQuery q(m_ro); q.exec("PRAGMA query_only=1"); q.exec("PRAGMA busy_timeout=2000");
    return true;
}

QList<QPair<QString, QString>> SessionsManager::allSettings() const {
    QList<QPair<QString, QString>> out;
    if (!ensureRo()) return out;
    QSqlQuery q(m_ro);
    if (q.exec("SELECT key, value FROM settings ORDER BY key")) while (q.next()) out.append({q.value(0).toString(), q.value(1).toString()});
    return out;
}

QJsonArray SessionsManager::allSlotsJson(const QString& channelId) const {
    QJsonArray out;
    if (!ensureRo()) return out;
    QSqlQuery q(m_ro);
    q.prepare("SELECT json FROM hour_slots WHERE channel_id = ? ORDER BY slot_id");
    q.addBindValue(channelId);
    if (q.exec()) while (q.next()) { const QJsonObject o = QJsonDocument::fromJson(q.value(0).toByteArray()).object(); if (!o.isEmpty()) out.append(o); }
    return out;
}

QStringList SessionsManager::slotIds(const QString& channelId) const {
    QStringList out;
    if (!ensureRo()) return out;
    QSqlQuery q(m_ro);
    q.prepare("SELECT slot_id FROM hour_slots WHERE channel_id = ?");
    q.addBindValue(channelId);
    if (q.exec()) while (q.next()) out << q.value(0).toString();
    return out;
}

QString SessionsManager::tableMark(const QString& table, const QString& timeCol) const {
    if (!ensureRo()) return "0:0";
    QSqlQuery q(m_ro);
    if (!q.exec(QString("SELECT COUNT(*), COALESCE(MAX(%1), 0) FROM %2").arg(timeCol, table)) || !q.next()) return "0:0";
    return QString("%1:%2").arg(q.value(0).toLongLong()).arg(q.value(1).toLongLong());
}

QString SessionsManager::digestMark(const QString& table, const QString& jsonCol, const QString& orderCol) const {
    if (!ensureRo()) return "0:0";
    QSqlQuery q(m_ro);
    if (!q.exec(QString("SELECT %1 FROM %2 ORDER BY %3").arg(jsonCol, table, orderCol))) return "0:0";
    QCryptographicHash h(QCryptographicHash::Md5); qint64 n = 0;
    while (q.next()) { const QByteArray row = q.value(0).toByteArray() + '\n'; h.addData(QByteArrayView(row)); ++n; }
    return QString("%1:%2").arg(n).arg(QString::fromLatin1(h.result().toHex().left(16)));
}

// ---------------------------------------------------------------------------
// Node identity
// ---------------------------------------------------------------------------

QString SessionsManager::nodeId() {
    QString id = m_store->setting("node.id");
    if (id.isEmpty()) { id = QUuid::createUuid().toString(QUuid::WithoutBraces); m_store->setSetting("node.id", id); qInfo() << "[Sessions] node id generated" << id; }
    return id;
}

QString SessionsManager::nodeName() {
    QString n = m_store->setting("node.name");
    if (n.isEmpty()) { n = QSysInfo::machineHostName(); if (n.isEmpty()) n = "playplay"; m_store->setSetting("node.name", n); }
    return n;
}

QJsonObject SessionsManager::nodeInfo() {
    QJsonObject n; n["id"] = nodeId(); n["name"] = nodeName();
    const QString v = QCoreApplication::applicationVersion();
    n["version"] = v.isEmpty() ? QString("dev") : v;
    return n;
}

// ---------------------------------------------------------------------------
// Bundle building
// ---------------------------------------------------------------------------

QJsonObject SessionsManager::mediaForBundle(const QJsonObject& record) {
    QJsonObject o = record;
    o.remove("transcriptText");          // the big blob; the summary (`transcript`) stays
    o.remove("transcript_words"); o.remove("transcript_segments");
    return o;
}

QJsonObject SessionsManager::buildBundle(const QString& name, const QString& notes, const QStringList& groupsIn, bool includeSecrets) {
    const QStringList groups = groupsIn.isEmpty() ? kGroups : groupsIn;
    QJsonObject g, manifest;
    const QList<ChannelProfile> channels = m_store->channels();

    if (groups.contains("channels")) {
        QJsonArray a; for (const ChannelProfile& p : channels) a.append(p.toJson());
        g["channels"] = a;
    }
    QSet<QString> referenced;
    if (groups.contains("schedule")) {
        QJsonArray a;
        for (const ChannelProfile& p : channels) {
            for (const QJsonValue& v : allSlotsJson(p.id)) {
                const QJsonObject s = v.toObject();
                for (const QJsonValue& it : s.value("items").toArray()) { const QString mid = it.toObject().value("mediaId").toString(); if (!mid.isEmpty()) referenced.insert(mid); }
                a.append(s);
            }
        }
        g["schedule"] = a;
    }
    const QList<MediaRecord> media = m_store->allMedia(true);
    if (groups.contains("media")) {
        QJsonArray a; for (const MediaRecord& m : media) a.append(mediaForBundle(m.toJson()));
        g["media"] = a;
    }
    // Manifest: every media record (so a schedule-only apply can still resolve by hash).
    for (const MediaRecord& m : media) {
        if (!groups.contains("media") && !referenced.contains(m.id)) continue;
        QJsonObject e; e["hash"] = m.extra.value("hash").toString(); e["path"] = QDir::toNativeSeparators(m.path); e["size"] = double(m.fileSizeBytes); e["title"] = m.title;
        manifest[m.id] = e;
    }
    QJsonObject settings, cg, spots, programming;
    for (const auto& kv : allSettings()) {
        if (!includeSecrets && isSecretSettingKey(kv.first)) continue;     // credentials stay on this node unless a failover peer is asking
        const QString grp = settingGroup(kv.first);
        if (grp == "settings") settings[kv.first] = kv.second;
        else if (grp == "cg") cg[kv.first] = kv.second;
        else if (grp == "spots") spots[kv.first] = kv.second;
        else if (grp == "programming") programming[kv.first] = kv.second;
    }
    if (groups.contains("settings")) g["settings"] = settings;
    if (groups.contains("cg")) g["cg"] = cg;
    if (groups.contains("spots")) g["spots"] = spots;
    if (groups.contains("programming")) g["programming"] = programming;

    QJsonObject b;
    b["format"] = kFormat; b["name"] = name; b["notes"] = notes;
    b["createdAt"] = double(QDateTime::currentMSecsSinceEpoch());
    b["node"] = nodeInfo();
    b["groups"] = g; b["mediaManifest"] = manifest;
    b["summary"] = summaryOf(b);
    return b;
}

QJsonObject SessionsManager::marks() {
    QJsonObject m;
    m["channels"] = digestMark("channels", "json", "id");
    m["schedule"] = tableMark("hour_slots", "updated_at");
    m["media"] = digestMark("media", "json", "id");
    const QString s = digestMark("settings", "key || '=' || value", "key");
    for (const char* k : {"settings", "cg", "spots", "programming"}) m[k] = s;
    m["revision"] = double(m_store->revision());
    return m;
}

QJsonObject SessionsManager::exportLive(const QStringList& groupsIn, const QJsonObject& since, bool includeSecrets) {
    const QJsonObject mine = marks();
    QStringList want;
    for (const QString& gname : (groupsIn.isEmpty() ? kGroups : groupsIn)) {
        if (!kGroups.contains(gname)) continue;
        if (!since.isEmpty() && since.contains(gname) && since.value(gname).toString() == mine.value(gname).toString()) continue;   // unchanged
        want << gname;
    }
    QJsonObject b = want.isEmpty() ? QJsonObject{{"format", kFormat}, {"node", nodeInfo()}, {"groups", QJsonObject()}, {"mediaManifest", QJsonObject()}}
                                   : buildBundle("live", "replication export", want, includeSecrets);
    b["secrets"] = includeSecrets;      // whether credential settings (spots.llm, star.agents, onvif.devices) ride in this bundle
    b["marks"] = mine;
    b["exported"] = QJsonArray::fromStringList(want);
    b["timeMs"] = double(QDateTime::currentMSecsSinceEpoch());
    return b;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

QJsonArray SessionsManager::list() const {
    QList<QJsonObject> rows = m_index;
    std::sort(rows.begin(), rows.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("updatedAt").toDouble() > b.value("updatedAt").toDouble(); });
    QJsonArray a; for (const QJsonObject& r : rows) a.append(r);
    return a;
}

QJsonObject SessionsManager::get(const QString& id, bool withData, bool* found) const {
    const int i = indexOf(id);
    if (found) *found = i >= 0;
    if (i < 0) return QJsonObject();
    if (!withData) return m_index[i];
    bool ok = false; QJsonObject w = readSession(id, &ok);
    if (!ok) { if (found) *found = false; return QJsonObject(); }
    return w;
}

QJsonObject SessionsManager::capture(const QString& name, const QString& notes) {
    QJsonObject w = buildBundle(name.isEmpty() ? "Session" : name, notes);
    w["id"] = newId(); w["version"] = 1; w["origin"] = "capture";
    w["updatedAt"] = w.value("createdAt");
    if (!writeSession(w)) return QJsonObject();
    m_index.append(rowOf(w)); saveIndex();
    qInfo() << "[Sessions] captured" << w.value("name").toString() << w.value("id").toString();
    emit changed();
    return rowOf(w);
}

QJsonObject SessionsManager::recapture(const QString& id, bool* found) {
    const int i = indexOf(id);
    if (found) *found = i >= 0;
    if (i < 0) return QJsonObject();
    const QJsonObject old = m_index[i];
    QJsonObject w = buildBundle(old.value("name").toString(), old.value("notes").toString());
    w["id"] = id; w["version"] = old.value("version").toInt(1) + 1; w["origin"] = old.value("origin").toString("capture");
    w["createdAt"] = old.value("createdAt"); w["updatedAt"] = double(QDateTime::currentMSecsSinceEpoch());
    if (!writeSession(w)) return QJsonObject();
    m_index[i] = rowOf(w); saveIndex();
    emit changed();
    return m_index[i];
}

bool SessionsManager::rename(const QString& id, const QString& name, const QString& notes, bool nameGiven, bool notesGiven) {
    const int i = indexOf(id);
    if (i < 0) return false;
    bool ok = false; QJsonObject w = readSession(id, &ok);
    if (!ok) return false;
    if (nameGiven) w["name"] = name;
    if (notesGiven) w["notes"] = notes;
    w["updatedAt"] = double(QDateTime::currentMSecsSinceEpoch());
    if (!writeSession(w)) return false;
    m_index[i] = rowOf(w); saveIndex();
    emit changed();
    return true;
}

bool SessionsManager::remove(const QString& id) {
    const int i = indexOf(id);
    if (i < 0) return false;
    if (isValidSessionId(id)) QFile::remove(filePath(id));     // an index row with a bad id (hand-edited index.json) is only dropped, never mapped to a file
    m_index.removeAt(i); saveIndex();
    emit changed();
    return true;
}

QJsonObject SessionsManager::clone(const QString& id, const QString& newName, bool* found) {
    bool ok = false; QJsonObject w = readSession(id, &ok);
    if (found) *found = ok;
    if (!ok) return QJsonObject();
    w["id"] = newId(); w["name"] = newName.isEmpty() ? w.value("name").toString() + " (copy)" : newName;
    w["version"] = 1; w["origin"] = "clone:" + id;
    w["createdAt"] = double(QDateTime::currentMSecsSinceEpoch()); w["updatedAt"] = w.value("createdAt");
    if (!writeSession(w)) return QJsonObject();
    m_index.append(rowOf(w)); saveIndex();
    emit changed();
    return rowOf(w);
}

QJsonObject SessionsManager::exportBundle(const QString& id, bool* found) const {
    bool ok = false; const QJsonObject w = readSession(id, &ok);
    if (found) *found = ok;
    return ok ? w : QJsonObject();
}

QJsonObject SessionsManager::importBundle(const QJsonObject& wireIn, const QString& name) {
    // Accept {bundle:{...}}, {data:{...}} or the bundle itself.
    QJsonObject w = wireIn;
    if (w.value("bundle").isObject() && w.value("bundle").toObject().contains("groups")) w = w.value("bundle").toObject();
    else if (w.value("data").isObject() && w.value("data").toObject().contains("groups")) w = w.value("data").toObject();
    if (!w.value("groups").isObject()) return QJsonObject{{"error", "not a session bundle (missing groups)"}};
    if (w.value("format").toString().isEmpty()) w["format"] = kFormat;
    w = withoutSecrets(w);               // a file from another station never plants credentials in the registry
    w["id"] = newId();
    if (!name.isEmpty()) w["name"] = name; else if (w.value("name").toString().isEmpty()) w["name"] = "Imported session";
    w["version"] = 1; w["origin"] = "import";
    if (!w.contains("createdAt")) w["createdAt"] = double(QDateTime::currentMSecsSinceEpoch());
    w["updatedAt"] = double(QDateTime::currentMSecsSinceEpoch());
    if (!w.contains("node")) w["node"] = QJsonObject{{"name", "imported"}};
    w["summary"] = summaryOf(w);
    if (!writeSession(w)) return QJsonObject{{"error", "cannot write session file"}};
    m_index.append(rowOf(w)); saveIndex();
    emit changed();
    return rowOf(w);
}

QJsonObject SessionsManager::receiveBundle(const QJsonObject& wireIn) {
    QJsonObject w = wireIn;
    if (w.value("bundle").isObject()) w = w.value("bundle").toObject();
    const QString id = w.value("id").toString();
    if (id.isEmpty() || !w.value("groups").isObject()) return QJsonObject{{"error", "session id and groups required"}};
    // The id becomes a file name: only the exact shape newId() makes is accepted (a peer or a crafted body must not pick the path).
    if (!isValidSessionId(id)) return QJsonObject{{"error", "invalid session id (expected <14-digit UTC stamp>-<4 hex>)"}};
    w = withoutSecrets(w);
    const int incomingV = w.value("version").toInt(1);
    const int i = indexOf(id);
    QJsonObject r; r["id"] = id; r["version"] = incomingV;
    if (i >= 0) {
        const int localV = m_index[i].value("version").toInt(1);
        if (incomingV < localV) { r["action"] = "skipped"; r["reason"] = "local copy is newer"; r["localVersion"] = localV; return r; }
        if (incomingV == localV) {
            bool ok = false; const QJsonObject local = readSession(id, &ok);
            if (ok && local.value("groups") != w.value("groups")) { r["action"] = "conflict"; r["reason"] = "same version, divergent content — kept local; re-capture on the sender to bump"; return r; }
            r["action"] = "skipped"; r["reason"] = "identical copy already held"; return r;
        }
    }
    w["lastSyncedAt"] = double(QDateTime::currentMSecsSinceEpoch());
    w["updatedAt"] = w.value("lastSyncedAt");
    if (!w.contains("origin")) w["origin"] = "peer";
    w["summary"] = summaryOf(w);
    if (!writeSession(w)) return QJsonObject{{"error", "cannot write session file"}};
    if (i >= 0) m_index[i] = rowOf(w); else m_index.append(rowOf(w));
    saveIndex();
    r["action"] = i >= 0 ? "updated" : "created";
    qInfo() << "[Sessions] received" << w.value("name").toString() << "v" << incomingV << r.value("action").toString();
    emit changed();
    return r;
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

QJsonObject SessionsManager::applySession(const QString& id, const QStringList& groups, bool dryRun, const ApplyOptions& opt, bool* found) {
    bool ok = false; const QJsonObject w = readSession(id, &ok);
    if (found) *found = ok;
    if (!ok) return QJsonObject();
    return applyBundle(w, groups, dryRun, opt);
}

QJsonObject SessionsManager::applyBundle(const QJsonObject& bundle, const QStringList& groupsIn, bool dryRun, const ApplyOptions& opt) {
    QJsonObject report; QJsonArray warnings, errors, unresolved; QJsonObject applied, remapped;
    const QJsonObject g = bundle.value("groups").toObject();
    const QJsonObject manifest = bundle.value("mediaManifest").toObject();
    const bool wantOutputs = groupsIn.contains("outputs");

    // Unknown group names are an error, never "apply everything".
    QStringList groups;
    for (const QString& x : groupsIn) { if (x == "outputs") continue; if (!kGroups.contains(x)) { errors.append("unknown group: " + x); continue; } groups << x; }
    if (groupsIn.isEmpty()) for (auto it = g.begin(); it != g.end(); ++it) if (kGroups.contains(it.key())) groups << it.key();
    report["dryRun"] = dryRun; report["groups"] = QJsonArray::fromStringList(groups);
    if (!errors.isEmpty()) { report["errors"] = errors; report["ok"] = false; return report; }   // nothing was written: no `applied`

    // --- local library index: by id, by hash, by path --------------------------------------
    const QList<MediaRecord> local = m_store->allMedia(true);
    QHash<QString, MediaRecord> byId; QHash<QString, QString> idByHash; QHash<QString, QString> idByPath;
    for (const MediaRecord& m : local) {
        byId[m.id] = m;
        const QString h = m.extra.value("hash").toString();
        const bool fileThere = !m.path.isEmpty() && QFileInfo::exists(QDir::fromNativeSeparators(m.path));
        if (!h.isEmpty() && fileThere && !idByHash.contains(h)) idByHash[h] = m.id;
        if (fileThere) idByPath[QDir::fromNativeSeparators(m.path).toLower()] = m.id;
    }
    auto fileExists = [](const QString& p) { return !p.isEmpty() && QFileInfo::exists(QDir::fromNativeSeparators(p)); };

    // Resolve a bundle media id to the local id (identity, or the local record holding the same content).
    QSet<QString> unresolvedIds;
    QHash<QString, QString> resolvedPath;     // bundle id -> local path when known
    auto resolve = [&](const QString& srcId) -> QString {
        if (srcId.isEmpty()) return srcId;
        if (remapped.contains(srcId)) return remapped.value(srcId).toString();
        const QJsonObject man = manifest.value(srcId).toObject();
        const QString hash = man.value("hash").toString();
        if (byId.contains(srcId) && fileExists(byId[srcId].path)) { resolvedPath[srcId] = byId[srcId].path; return srcId; }
        if (!hash.isEmpty() && idByHash.contains(hash)) { const QString lid = idByHash[hash]; resolvedPath[srcId] = byId[lid].path; if (lid != srcId) remapped[srcId] = lid; return lid; }
        const QString p = QDir::fromNativeSeparators(man.value("path").toString());
        if (fileExists(p)) { resolvedPath[srcId] = p; if (idByPath.contains(p.toLower()) && idByPath[p.toLower()] != srcId) { remapped[srcId] = idByPath[p.toLower()]; return idByPath[p.toLower()]; } return srcId; }
        if (!unresolvedIds.contains(srcId)) {
            unresolvedIds.insert(srcId);
            QJsonObject u; u["id"] = srcId; u["title"] = man.value("title").toString(); u["hash"] = hash; u["path"] = man.value("path").toString(); u["size"] = man.value("size").toDouble();
            unresolved.append(u);
        }
        return srcId;
    };

    // --- media (first, so schedule items can be rewritten) -----------------------------------
    if (groups.contains("media")) {
        int n = 0;
        for (const QJsonValue& v : g.value("media").toArray()) {
            MediaRecord in = MediaRecord::fromJson(v.toObject());
            if (in.id.isEmpty()) continue;
            const QString hash = in.extra.value("hash").toString();
            const bool haveLocal = byId.contains(in.id);
            const MediaRecord loc = haveLocal ? byId[in.id] : MediaRecord();
            // Foreign apply: same content already here under another id → no duplicate, the bundle id is remapped.
            // A mirror (replication) keeps every record under its own id: contracts/recipes/spots settings reference those ids and are not remapped.
            if (!opt.mirror && !haveLocal && !hash.isEmpty() && idByHash.contains(hash) && idByHash[hash] != in.id) {
                remapped[in.id] = idByHash[hash]; resolvedPath[in.id] = byId[idByHash[hash]].path;
                continue;
            }
            if (fileExists(in.path)) {
                // path valid on this node: keep it
            } else if (!hash.isEmpty() && idByHash.contains(hash)) {
                in.path = byId[idByHash[hash]].path; in.extra.remove("missing");
            } else if (haveLocal && opt.keepLocalMediaPaths && fileExists(loc.path)) {
                in.path = loc.path; in.extra.remove("missing");
                warnings.append(QString("media %1: bundle path differs, kept local file %2").arg(in.title, QDir::toNativeSeparators(loc.path)));
            } else {
                in.extra["missing"] = true;
                if (!unresolvedIds.contains(in.id)) {
                    unresolvedIds.insert(in.id);
                    QJsonObject u; u["id"] = in.id; u["title"] = in.title; u["hash"] = hash; u["path"] = QDir::toNativeSeparators(in.path); u["size"] = double(in.fileSizeBytes);
                    unresolved.append(u);
                }
            }
            if (haveLocal) {
                // Keep this node's heavy/derived fields the bundle does not carry.
                if (loc.extra.contains("transcriptText")) in.extra["transcriptText"] = loc.extra.value("transcriptText");
                if (in.thumbnailPath.isEmpty()) in.thumbnailPath = loc.thumbnailPath;
                if (!in.extra.contains("transcript") && loc.extra.contains("transcript")) in.extra["transcript"] = loc.extra.value("transcript");
            }
            resolvedPath[in.id] = in.path;
            if (!dryRun && !m_store->saveMedia(in)) { errors.append("saveMedia failed: " + in.id); continue; }
            byId[in.id] = in;
            if (!hash.isEmpty() && fileExists(in.path) && !idByHash.contains(hash)) idByHash[hash] = in.id;
            ++n;
        }
        applied["media"] = n;
    }

    // --- channels ------------------------------------------------------------------------
    if (groups.contains("channels")) {
        int n = 0;
        QJsonArray createdChannels, restartChannels;   // for the caller: spawn the new ones, restart the ones whose frame format changed
        for (const QJsonValue& v : g.value("channels").toArray()) {
            ChannelProfile in = ChannelProfile::fromJson(v.toObject());
            if (in.id.isEmpty()) continue;
            // The id names the engine's data directory (channels/<id>): a bundle must not be able to point it elsewhere.
            if (in.id.contains('/') || in.id.contains('\\') || in.id.contains(':') || in.id.contains("..")) { errors.append("unsafe channel id skipped: " + in.id.left(40)); continue; }
            bool f = false; const ChannelProfile loc = m_store->channel(in.id, &f);
            bool restart = false;
            if (f) {
                if (opt.keepOutputs && !wantOutputs) in.outputs = loc.outputs;
                if (opt.keepArmed) in.armed = loc.armed;
                restart = in.frameRate != loc.frameRate || in.width != loc.width || in.height != loc.height || in.audioChannels != loc.audioChannels;
            } else {
                if (opt.keepArmed) in.armed = false;   // a channel new to this node never starts armed by a bundle
                // The sender's outputs address the sender's hardware/targets: a new channel here must not start emitting to them.
                if (opt.keepOutputs && !wantOutputs) { in.outputs = QJsonArray(); warnings.append(QString("channel %1: new on this node, outputs left empty (configure them here or apply the \"outputs\" group)").arg(in.id)); }
            }
            // Identity assets are paths; warn when they do not exist here.
            for (const char* k : {"bugAsset", "legalIdAsset", "emergencySlate"}) {
                const QString p = in.identity.value(k).toString();
                if (!p.isEmpty() && (p.contains('/') || p.contains('\\')) && !fileExists(p)) warnings.append(QString("channel %1: identity.%2 '%3' not found on this machine").arg(in.id, k, p));
            }
            if (!dryRun && !m_store->saveChannel(in)) { errors.append("saveChannel failed: " + in.id); continue; }
            if (!f) createdChannels.append(in.id);
            else if (restart) restartChannels.append(in.id);
            ++n;
        }
        applied["channels"] = n;
        report["createdChannels"] = createdChannels; report["restartChannels"] = restartChannels;
    }
    if (wantOutputs) applied["outputs"] = groups.contains("channels") ? applied.value("channels").toInt() : 0;

    // --- schedule -----------------------------------------------------------------------
    if (groups.contains("schedule")) {
        int n = 0, rewritten = 0, flagged = 0;
        QHash<QString, QSet<QString>> keep;   // channel -> slot ids carried by the bundle (for replace)
        for (const QJsonValue& v : g.value("schedule").toArray()) {
            HourSlot s = HourSlot::fromJson(v.toObject());
            if (s.channelId.isEmpty() || s.id.isEmpty()) continue;
            for (SlotItem& it : s.items) {
                if (it.mediaId.isEmpty()) continue;
                const QString lid = resolve(it.mediaId);
                if (lid != it.mediaId) { it.mediaId = lid; ++rewritten; }
                if (unresolvedIds.contains(lid)) { it.extra["unresolvedMedia"] = true; ++flagged; }
                else { it.extra.remove("unresolvedMedia"); if (resolvedPath.contains(lid) || byId.contains(lid)) it.path = resolvedPath.value(lid, byId.value(lid).path); }
            }
            keep[s.channelId].insert(s.id);
            if (!dryRun && !m_store->saveSlot(s)) { errors.append("saveSlot failed: " + s.channelId + "/" + s.id); continue; }
            ++n;
        }
        int removed = 0;
        if (opt.replace) {
            for (auto it = keep.begin(); it != keep.end(); ++it)
                for (const QString& sid : slotIds(it.key()))
                    if (!it.value().contains(sid)) { if (!dryRun) m_store->deleteSlot(it.key(), sid); ++removed; }
        }
        applied["schedule"] = n;
        report["scheduleRewrittenRefs"] = rewritten; report["scheduleFlaggedItems"] = flagged; report["scheduleRemovedSlots"] = removed;
    }

    // --- settings groups ---------------------------------------------------------------
    for (const char* grp : {"settings", "cg", "spots", "programming"}) {
        if (!groups.contains(grp)) continue;
        int n = 0;
        const QJsonObject kv = g.value(grp).toObject();
        for (auto it = kv.begin(); it != kv.end(); ++it) {
            if (isLocalSettingKey(it.key())) { warnings.append(QString("skipped local-only setting %1").arg(it.key())); continue; }
            QString val;
            if (it.value().isString()) val = it.value().toString();
            else if (it.value().isObject()) val = QString::fromUtf8(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact));
            else if (it.value().isArray()) val = QString::fromUtf8(QJsonDocument(it.value().toArray()).toJson(QJsonDocument::Compact));
            else if (it.value().isBool()) val = it.value().toBool() ? "1" : "0";
            else val = QString::number(it.value().toDouble());
            // Settings that name media (traffic contracts, recipes, spots) follow the schedule items through the remap table.
            if (!remapped.isEmpty() && val.contains('"')) for (auto rm = remapped.begin(); rm != remapped.end(); ++rm) val.replace("\"" + rm.key() + "\"", "\"" + rm.value().toString() + "\"");
            if (!dryRun && !m_store->setSetting(it.key(), val)) { errors.append("setSetting failed: " + it.key()); continue; }
            ++n;
        }
        applied[grp] = n;
    }

    report["applied"] = applied; report["remapped"] = remapped; report["unresolvedMedia"] = unresolved;
    report["warnings"] = warnings; report["errors"] = errors;
    report["ok"] = errors.isEmpty();
    if (!dryRun) {
        qInfo().noquote() << "[Sessions] applied" << groups.join(",") << QJsonDocument(applied).toJson(QJsonDocument::Compact) << unresolved.size() << "unresolved media";
        emit changed();
    }
    return report;
}
