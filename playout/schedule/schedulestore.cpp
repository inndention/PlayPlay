#include "schedulestore.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QDebug>
#include <QUuid>

namespace sched {

namespace {
QByteArray jsonBytes(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QJsonObject jsonObj(const QVariant& v) { return QJsonDocument::fromJson(v.toByteArray()).object(); }
QJsonArray jsonArr(const QVariant& v) { return QJsonDocument::fromJson(v.toByteArray()).array(); }
} // namespace

ScheduleStore::ScheduleStore(QObject* parent) : QObject(parent) {
    m_connName = "sched_" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

ScheduleStore::~ScheduleStore() { close(); }

bool ScheduleStore::open(const QString& dbPath) {
    QMutexLocker l(&m_mutex);
    if (m_open) return true;
    QDir().mkpath(QFileInfo(dbPath).absolutePath());
    m_db = QSqlDatabase::addDatabase("QSQLITE", m_connName);
    m_db.setDatabaseName(dbPath);
    if (!m_db.open()) {
        qCritical() << "[ScheduleStore] cannot open" << dbPath << m_db.lastError().text();
        return false;
    }
    m_path = dbPath;
    QSqlQuery q(m_db);
    q.exec("PRAGMA journal_mode=WAL");
    q.exec("PRAGMA synchronous=NORMAL");
    q.exec("PRAGMA foreign_keys=ON");
    m_open = ensureTables();
    dropChannelCacheLocked();
    if (m_open) qInfo() << "[ScheduleStore] opened" << dbPath;
    return m_open;
}

void ScheduleStore::close() {
    QMutexLocker l(&m_mutex);
    dropChannelCacheLocked();
    if (!m_open) return;
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connName);
    m_open = false;
}

bool ScheduleStore::ensureTables() {
    QSqlQuery q(m_db);
    const char* ddl[] = {
        "CREATE TABLE IF NOT EXISTS channels (id TEXT PRIMARY KEY, name TEXT, enabled INTEGER DEFAULT 1, json TEXT NOT NULL, updated_at INTEGER)",
        "CREATE TABLE IF NOT EXISTS hour_slots (channel_id TEXT NOT NULL, slot_id TEXT NOT NULL, day_of_week INTEGER, date TEXT, hour INTEGER NOT NULL,"
        " json TEXT NOT NULL, updated_at INTEGER, PRIMARY KEY (channel_id, slot_id))",
        "CREATE INDEX IF NOT EXISTS idx_slots_date ON hour_slots(channel_id, date, hour)",
        "CREATE INDEX IF NOT EXISTS idx_slots_dow ON hour_slots(channel_id, day_of_week, hour)",
        "CREATE TABLE IF NOT EXISTS media (id TEXT PRIMARY KEY, path TEXT, title TEXT, type TEXT, duration REAL, series TEXT, episode_date TEXT,"
        " client TEXT, priority INTEGER DEFAULT 0, play_count INTEGER DEFAULT 0, last_played INTEGER DEFAULT 0, archived INTEGER DEFAULT 0,"
        " available_start INTEGER DEFAULT 0, available_end INTEGER DEFAULT 0, json TEXT NOT NULL, added_at INTEGER)",
        "CREATE INDEX IF NOT EXISTS idx_media_path ON media(path)",
        "CREATE INDEX IF NOT EXISTS idx_media_type ON media(type, archived)",
        "CREATE INDEX IF NOT EXISTS idx_media_series ON media(series, episode_date)",
        "CREATE TABLE IF NOT EXISTS as_run (id INTEGER PRIMARY KEY AUTOINCREMENT, channel_id TEXT, runlist_item_id TEXT, item_id TEXT, asset_id TEXT,"
        " title TEXT, type TEXT, event_type TEXT, origin TEXT, planned_start INTEGER, actual_start INTEGER, planned_end INTEGER, actual_end INTEGER,"
        " planned_duration INTEGER, actual_duration INTEGER, variance INTEGER, frame INTEGER, timecode TEXT, status TEXT, reason TEXT, slot_id TEXT,"
        " json TEXT, created_at INTEGER)",
        "CREATE INDEX IF NOT EXISTS idx_asrun_time ON as_run(channel_id, actual_start)",
        "CREATE INDEX IF NOT EXISTS idx_asrun_item ON as_run(channel_id, runlist_item_id, event_type)",
        // airedCount (ad-contract pacing, every rebuild) filters on asset_id first: without this it scans the whole as-run table.
        "CREATE INDEX IF NOT EXISTS idx_asrun_asset ON as_run(asset_id, event_type, actual_start)",
        "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)",
    };
    for (const char* s : ddl) {
        if (!q.exec(QString::fromUtf8(s))) {
            qCritical() << "[ScheduleStore] DDL failed:" << q.lastError().text() << s;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------

bool ScheduleStore::loadChannelsLocked() const {
    if (m_chanCacheValid) return true;
    if (!m_open) return false;
    // The same rows, order (ORDER BY id: SQLite's binary collation) and parse as the per-call queries this replaces; the
    // index is keyed by the row id, which is what "WHERE id = ?" matched.
    QSqlQuery q(m_db);
    if (!q.exec("SELECT id, json FROM channels ORDER BY id")) {
        qWarning() << "[ScheduleStore] channels:" << q.lastError().text();
        return false;                                       // not cached: the next call asks SQLite again
    }
    m_chanCache.clear(); m_chanIndex.clear();
    while (q.next()) {
        m_chanIndex.insert(q.value(0).toString(), int(m_chanCache.size()));
        m_chanCache.append(ChannelProfile::fromJson(jsonObj(q.value(1))));
    }
    m_chanCacheValid = true;
    return true;
}

QList<ChannelProfile> ScheduleStore::channels() const {
    QMutexLocker l(&m_mutex);
    if (!loadChannelsLocked()) return {};
    return m_chanCache;                                     // implicitly shared: a copy costs a reference count
}

ChannelProfile ScheduleStore::channel(const QString& id, bool* found) const {
    QMutexLocker l(&m_mutex);
    if (loadChannelsLocked()) {
        const int i = m_chanIndex.value(id, -1);
        if (i >= 0) { if (found) *found = true; return m_chanCache.at(i); }
    }
    if (found) *found = false;
    ChannelProfile c; c.id = id; c.name = id;
    return c;
}

quint64 ScheduleStore::scheduleRevision() const { QMutexLocker l(&m_mutex); return m_scheduleRevision; }
quint64 ScheduleStore::settingsRevision() const { QMutexLocker l(&m_mutex); return m_settingsRevision; }

quint64 ScheduleStore::settingsRevision(const QStringList& keyPrefixes) const {
    QMutexLocker l(&m_mutex);
    quint64 rev = 0;
    for (auto it = m_settingKeyRevision.cbegin(); it != m_settingKeyRevision.cend(); ++it) {
        if (it.value() <= rev) continue;
        for (const QString& p : keyPrefixes) if (it.key().startsWith(p)) { rev = it.value(); break; }
    }
    return rev;
}

bool ScheduleStore::saveChannel(const ChannelProfile& c) {
    {
        QMutexLocker l(&m_mutex);
        dropChannelCacheLocked();                           // reloaded from the table on the next read
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO channels (id, name, enabled, json, updated_at) VALUES (?,?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET name=excluded.name, enabled=excluded.enabled, json=excluded.json, updated_at=excluded.updated_at");
        q.addBindValue(c.id); q.addBindValue(c.name); q.addBindValue(c.enabled ? 1 : 0);
        q.addBindValue(QString::fromUtf8(jsonBytes(c.toJson()))); q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!q.exec()) { qWarning() << "[ScheduleStore] saveChannel:" << q.lastError().text(); return false; }
        ++m_revision; ++m_scheduleRevision;
    }
    emit channelChanged(c.id);
    return true;
}

bool ScheduleStore::deleteChannel(const QString& id) {
    {
        QMutexLocker l(&m_mutex);
        dropChannelCacheLocked();
        QSqlQuery q(m_db);
        q.prepare("DELETE FROM channels WHERE id = ?"); q.addBindValue(id);
        if (!q.exec()) return false;
        q.prepare("DELETE FROM hour_slots WHERE channel_id = ?"); q.addBindValue(id); q.exec();
        ++m_revision; ++m_scheduleRevision;
    }
    emit channelChanged(id);
    return true;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

HourSlot ScheduleStore::slot(const QString& channelId, const QString& slotId, bool* found) const {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT json FROM hour_slots WHERE channel_id = ? AND slot_id = ?");
    q.addBindValue(channelId); q.addBindValue(slotId);
    if (q.exec() && q.next()) {
        if (found) *found = true;
        HourSlot s = HourSlot::fromJson(jsonObj(q.value(0)));
        s.channelId = channelId; s.id = slotId;
        return s;
    }
    if (found) *found = false;
    HourSlot s; s.id = slotId; s.channelId = channelId;
    HourSlot::parseId(slotId, &s.dayOfWeek, &s.date, &s.hour);
    return s;
}

bool ScheduleStore::saveSlot(const HourSlot& in) {
    HourSlot s = in;
    if (s.id.isEmpty()) s.id = s.isDated() ? HourSlot::dateId(s.date, s.hour) : HourSlot::templateId(s.dayOfWeek, s.hour);
    s.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker l(&m_mutex);
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO hour_slots (channel_id, slot_id, day_of_week, date, hour, json, updated_at) VALUES (?,?,?,?,?,?,?) "
                  "ON CONFLICT(channel_id, slot_id) DO UPDATE SET day_of_week=excluded.day_of_week, date=excluded.date, hour=excluded.hour,"
                  " json=excluded.json, updated_at=excluded.updated_at");
        q.addBindValue(s.channelId); q.addBindValue(s.id);
        q.addBindValue(s.isDated() ? QVariant() : QVariant(s.dayOfWeek));
        q.addBindValue(s.isDated() ? QVariant(s.date.toString(Qt::ISODate)) : QVariant());
        q.addBindValue(s.hour); q.addBindValue(QString::fromUtf8(jsonBytes(s.toJson()))); q.addBindValue(s.updatedAtMs);
        if (!q.exec()) { qWarning() << "[ScheduleStore] saveSlot:" << q.lastError().text(); return false; }
        ++m_revision; ++m_scheduleRevision;
    }
    emit slotChanged(s.channelId, s.id);
    return true;
}

bool ScheduleStore::deleteSlot(const QString& channelId, const QString& slotId) {
    {
        QMutexLocker l(&m_mutex);
        QSqlQuery q(m_db);
        q.prepare("DELETE FROM hour_slots WHERE channel_id = ? AND slot_id = ?");
        q.addBindValue(channelId); q.addBindValue(slotId);
        if (!q.exec()) return false;
        ++m_revision; ++m_scheduleRevision;
    }
    emit slotChanged(channelId, slotId);
    return true;
}

HourSlot ScheduleStore::effectiveSlot(const QString& channelId, const QDate& localDate, int hour, bool* isOverride) const {
    bool found = false;
    HourSlot s = slot(channelId, HourSlot::dateId(localDate, hour), &found);
    if (found) { if (isOverride) *isOverride = true; return s; }
    if (isOverride) *isOverride = false;
    s = slot(channelId, HourSlot::templateId(scheduleDow(localDate), hour), &found);
    s.date = localDate;                     // stamp the date so callers know which day this instance is for
    return s;
}

QList<HourSlot> ScheduleStore::week(const QString& channelId, const QDate& weekStart) const {
    QList<HourSlot> out;
    for (int d = 0; d < 7; ++d) {
        const QDate day = weekStart.addDays(d);
        for (int h = 0; h < 24; ++h) {
            bool ov = false;
            HourSlot s = effectiveSlot(channelId, day, h, &ov);
            if (ov) s.showBlock["_override"] = true;
            out.append(s);
        }
    }
    return out;
}

QList<HourSlot> ScheduleStore::templateSlots(const QString& channelId) const {
    QMutexLocker l(&m_mutex);
    QList<HourSlot> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT slot_id, json FROM hour_slots WHERE channel_id = ? AND date IS NULL ORDER BY day_of_week, hour");
    q.addBindValue(channelId);
    if (q.exec()) while (q.next()) { HourSlot s = HourSlot::fromJson(jsonObj(q.value(1))); s.channelId = channelId; s.id = q.value(0).toString(); out.append(s); }
    return out;
}

// ---------------------------------------------------------------------------
// Media
// ---------------------------------------------------------------------------

MediaRecord ScheduleStore::mediaFromQuery(const QSqlQuery& q) {
    MediaRecord m = MediaRecord::fromJson(jsonObj(q.value("json")));
    m.playCount = q.value("play_count").toInt();
    m.lastPlayedMs = q.value("last_played").toLongLong();
    return m;
}

bool ScheduleStore::saveMedia(const MediaRecord& in) {
    MediaRecord m = in;
    if (m.id.isEmpty()) m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (m.addedAtMs == 0) m.addedAtMs = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker l(&m_mutex);
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO media (id, path, title, type, duration, series, episode_date, client, priority, play_count, last_played, archived,"
                  " available_start, available_end, json, added_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET path=excluded.path, title=excluded.title, type=excluded.type, duration=excluded.duration,"
                  " series=excluded.series, episode_date=excluded.episode_date, client=excluded.client, priority=excluded.priority,"
                  " archived=excluded.archived, available_start=excluded.available_start, available_end=excluded.available_end, json=excluded.json");
        q.addBindValue(m.id); q.addBindValue(m.path); q.addBindValue(m.title); q.addBindValue(m.type); q.addBindValue(m.durationSec);
        q.addBindValue(m.series); q.addBindValue(m.episodeDate.isValid() ? m.episodeDate.toString(Qt::ISODate) : QString());
        q.addBindValue(m.client); q.addBindValue(m.priority); q.addBindValue(m.playCount); q.addBindValue(m.lastPlayedMs);
        q.addBindValue(m.archived ? 1 : 0); q.addBindValue(m.availableStartMs); q.addBindValue(m.availableEndMs);
        q.addBindValue(QString::fromUtf8(jsonBytes(m.toJson()))); q.addBindValue(m.addedAtMs);
        if (!q.exec()) { qWarning() << "[ScheduleStore] saveMedia:" << q.lastError().text(); return false; }
    }
    emit mediaChanged(m.id);
    return true;
}

bool ScheduleStore::deleteMedia(const QString& id) {
    { QMutexLocker l(&m_mutex); QSqlQuery q(m_db); q.prepare("DELETE FROM media WHERE id = ?"); q.addBindValue(id); if (!q.exec()) return false; }
    emit mediaChanged(id);
    return true;
}

MediaRecord ScheduleStore::media(const QString& id, bool* found) const {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT json, play_count, last_played FROM media WHERE id = ?"); q.addBindValue(id);
    if (q.exec() && q.next()) { if (found) *found = true; return mediaFromQuery(q); }
    if (found) *found = false;
    return MediaRecord();
}

MediaRecord ScheduleStore::mediaByPath(const QString& path, bool* found) const {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT json, play_count, last_played FROM media WHERE path = ? LIMIT 1"); q.addBindValue(path);
    if (q.exec() && q.next()) { if (found) *found = true; return mediaFromQuery(q); }
    if (found) *found = false;
    return MediaRecord();
}

QList<MediaRecord> ScheduleStore::allMedia(bool includeArchived) const {
    QMutexLocker l(&m_mutex);
    QList<MediaRecord> out;
    QSqlQuery q(m_db);
    q.exec(includeArchived ? "SELECT json, play_count, last_played FROM media ORDER BY title"
                           : "SELECT json, play_count, last_played FROM media WHERE archived = 0 ORDER BY title");
    while (q.next()) out.append(mediaFromQuery(q));
    return out;
}

QList<MediaRecord> ScheduleStore::mediaByType(const QStringList& types, bool includeArchived) const {
    QMutexLocker l(&m_mutex);
    QList<MediaRecord> out;
    if (types.isEmpty()) return out;
    QStringList marks; for (int i = 0; i < types.size(); ++i) marks << "?";
    QSqlQuery q(m_db);
    // Case-insensitive like every other type match (QueryBlocks::inList): a record stored as "Program" belongs to a "program" block.
    q.prepare(QString("SELECT json, play_count, last_played FROM media WHERE lower(type) IN (%1)%2 ORDER BY title")
              .arg(marks.join(',')).arg(includeArchived ? "" : " AND archived = 0"));
    for (const auto& t : types) q.addBindValue(t.trimmed().toLower());
    if (q.exec()) while (q.next()) out.append(mediaFromQuery(q));
    return out;
}

QList<MediaRecord> ScheduleStore::mediaBySeries(const QString& series) const {
    QMutexLocker l(&m_mutex);
    QList<MediaRecord> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT json, play_count, last_played FROM media WHERE series = ? AND archived = 0 ORDER BY episode_date DESC, added_at DESC");
    q.addBindValue(series);
    if (q.exec()) while (q.next()) out.append(mediaFromQuery(q));
    return out;
}

QList<MediaRecord> ScheduleStore::searchMedia(const QString& text, int limit) const {
    QMutexLocker l(&m_mutex);
    QList<MediaRecord> out;
    QSqlQuery q(m_db);
    q.prepare("SELECT json, play_count, last_played FROM media WHERE archived = 0 AND (title LIKE ? OR path LIKE ? OR series LIKE ? OR client LIKE ?) ORDER BY title LIMIT ?");
    const QString like = "%" + text + "%";
    q.addBindValue(like); q.addBindValue(like); q.addBindValue(like); q.addBindValue(like); q.addBindValue(limit);
    if (q.exec()) while (q.next()) out.append(mediaFromQuery(q));
    return out;
}

bool ScheduleStore::recordPlay(const QString& mediaId, qint64 atMs) {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("UPDATE media SET play_count = play_count + 1, last_played = ? WHERE id = ?");
    q.addBindValue(atMs); q.addBindValue(mediaId);
    return q.exec() && q.numRowsAffected() > 0;
}

// ---------------------------------------------------------------------------
// As-run
// ---------------------------------------------------------------------------

AsRunRow ScheduleStore::asRunFromQuery(const QSqlQuery& q) {
    AsRunRow r;
    r.rowId = q.value("id").toLongLong(); r.channelId = q.value("channel_id").toString();
    r.runlistItemId = q.value("runlist_item_id").toString(); r.itemId = q.value("item_id").toString();
    r.assetId = q.value("asset_id").toString(); r.title = q.value("title").toString(); r.type = q.value("type").toString();
    r.eventType = q.value("event_type").toString(); r.origin = q.value("origin").toString();
    r.plannedStartMs = q.value("planned_start").toLongLong(); r.actualStartMs = q.value("actual_start").toLongLong();
    r.plannedEndMs = q.value("planned_end").toLongLong(); r.actualEndMs = q.value("actual_end").toLongLong();
    r.plannedDurationMs = q.value("planned_duration").toLongLong(); r.actualDurationMs = q.value("actual_duration").toLongLong();
    r.varianceMs = q.value("variance").toLongLong(); r.frame = q.value("frame").toLongLong();
    r.timecode = q.value("timecode").toString(); r.status = q.value("status").toString(); r.reason = q.value("reason").toString();
    r.slotId = q.value("slot_id").toString(); r.extra = jsonObj(q.value("json"));
    return r;
}

qint64 ScheduleStore::insertAsRun(const AsRunRow& r) {
    qint64 id = 0;
    {
        QMutexLocker l(&m_mutex);
        QSqlQuery q(m_db);
        q.prepare("INSERT INTO as_run (channel_id, runlist_item_id, item_id, asset_id, title, type, event_type, origin, planned_start, actual_start,"
                  " planned_end, actual_end, planned_duration, actual_duration, variance, frame, timecode, status, reason, slot_id, json, created_at)"
                  " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.addBindValue(r.channelId); q.addBindValue(r.runlistItemId); q.addBindValue(r.itemId); q.addBindValue(r.assetId);
        q.addBindValue(r.title); q.addBindValue(r.type); q.addBindValue(r.eventType); q.addBindValue(r.origin);
        q.addBindValue(r.plannedStartMs); q.addBindValue(r.actualStartMs); q.addBindValue(r.plannedEndMs); q.addBindValue(r.actualEndMs);
        q.addBindValue(r.plannedDurationMs); q.addBindValue(r.actualDurationMs); q.addBindValue(r.varianceMs); q.addBindValue(r.frame);
        q.addBindValue(r.timecode); q.addBindValue(r.status); q.addBindValue(r.reason); q.addBindValue(r.slotId);
        q.addBindValue(QString::fromUtf8(jsonBytes(r.extra))); q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!q.exec()) { qWarning() << "[ScheduleStore] insertAsRun:" << q.lastError().text(); return 0; }
        id = q.lastInsertId().toLongLong();
    }
    AsRunRow stored = r; stored.rowId = id;
    emit asRunAdded(stored);
    return id;
}

bool ScheduleStore::completeAsRun(const QString& channelId, const QString& runlistItemId, qint64 actualEndMs, const QString& eventType) {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("UPDATE as_run SET actual_end = ?, actual_duration = ? - actual_start, reason = CASE WHEN reason = '' THEN ? ELSE reason END"
              " WHERE id = (SELECT id FROM as_run WHERE channel_id = ? AND runlist_item_id = ? AND event_type = 'START' ORDER BY id DESC LIMIT 1)");
    q.addBindValue(actualEndMs); q.addBindValue(actualEndMs); q.addBindValue(eventType); q.addBindValue(channelId); q.addBindValue(runlistItemId);
    return q.exec() && q.numRowsAffected() > 0;
}

int ScheduleStore::airedCount(const QString& mediaId, qint64 fromMs, qint64 toMs, const QString& channelId) const {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    QString sql = "SELECT COUNT(*) FROM as_run WHERE asset_id = ? AND event_type = 'START' AND actual_start >= ? AND actual_start <= ?";
    if (!channelId.isEmpty()) sql += " AND channel_id = ?";
    q.prepare(sql);
    q.addBindValue(mediaId); q.addBindValue(fromMs); q.addBindValue(toMs);
    if (!channelId.isEmpty()) q.addBindValue(channelId);
    if (q.exec() && q.next()) return q.value(0).toInt();
    return 0;
}

QList<AsRunRow> ScheduleStore::asRun(const QString& channelId, qint64 fromMs, qint64 toMs, int limit) const {
    QMutexLocker l(&m_mutex);
    QList<AsRunRow> out;
    QSqlQuery q(m_db);
    QString sql = "SELECT * FROM as_run WHERE actual_start >= ? AND actual_start <= ?";
    if (!channelId.isEmpty()) sql += " AND channel_id = ?";
    sql += " ORDER BY actual_start ASC, id ASC LIMIT ?";
    q.prepare(sql);
    q.addBindValue(fromMs); q.addBindValue(toMs);
    if (!channelId.isEmpty()) q.addBindValue(channelId);
    q.addBindValue(limit);
    if (q.exec()) while (q.next()) out.append(asRunFromQuery(q));
    return out;
}

QJsonObject ScheduleStore::asRunSummary(const QString& channelId, qint64 fromMs, qint64 toMs) const {
    return summarizeAsRun(asRun(channelId, fromMs, toMs, 100000));
}
QJsonObject ScheduleStore::summarizeAsRun(const QList<AsRunRow>& rows) {
    QJsonObject o; int starts = 0, onTime = 0, late = 0, early = 0, failed = 0, skipped = 0, fill = 0; qint64 airedMs = 0; double absVar = 0;
    for (const auto& r : rows) {
        if (r.eventType == "START") {
            ++starts; airedMs += r.actualDurationMs; absVar += qAbs(double(r.varianceMs));
            if (r.status == "ON_TIME") ++onTime; else if (r.status == "LATE_START") ++late; else if (r.status == "EARLY_START") ++early;
            if (r.origin == "fill" || r.origin == "emergency") ++fill;
        } else if (r.eventType == "FAILED") ++failed;
        else if (r.eventType == "SKIP") ++skipped;
    }
    o["events"] = starts; o["onTime"] = onTime; o["late"] = late; o["early"] = early; o["failed"] = failed; o["skipped"] = skipped;
    o["fill"] = fill; o["airedSeconds"] = airedMs / 1000.0; o["avgAbsVarianceMs"] = starts ? absVar / starts : 0.0;
    o["onTimePct"] = starts ? 100.0 * onTime / starts : 100.0;
    return o;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

QString ScheduleStore::setting(const QString& key, const QString& def) const {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("SELECT value FROM settings WHERE key = ?"); q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return def;
}

bool ScheduleStore::setSetting(const QString& key, const QString& value) {
    QMutexLocker l(&m_mutex);
    QSqlQuery q(m_db);
    q.prepare("INSERT INTO settings (key, value) VALUES (?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.addBindValue(key); q.addBindValue(value);
    const bool ok = q.exec();
    // revision() keeps counting settings (today's executor trigger); scheduleRevision() does not (PS-6).
    if (ok) { ++m_revision; m_settingKeyRevision.insert(key, ++m_settingsRevision); }
    return ok;
}

QJsonObject ScheduleStore::settingJson(const QString& key) const { return QJsonDocument::fromJson(setting(key).toUtf8()).object(); }
bool ScheduleStore::setSettingJson(const QString& key, const QJsonObject& v) { return setSetting(key, QString::fromUtf8(jsonBytes(v))); }

} // namespace sched
