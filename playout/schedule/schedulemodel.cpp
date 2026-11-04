#include "schedulemodel.h"
#include <QJsonValue>

namespace sched {

namespace {
QJsonArray toArray(const QStringList& l) { QJsonArray a; for (const auto& s : l) a.append(s); return a; }
QStringList toList(const QJsonValue& v) { QStringList l; for (const auto& x : v.toArray()) l << x.toString(); return l; }
double numOr(const QJsonObject& o, const char* a, const char* b, double def) {
    if (o.contains(a)) return o.value(a).toDouble(def);
    if (b && o.contains(b)) return o.value(b).toDouble(def);
    return def;
}
} // namespace

// ---------------------------------------------------------------------------
// SlotItem
// ---------------------------------------------------------------------------

QJsonObject SlotItem::toJson() const {
    QJsonObject o = extra;
    o["id"] = id; o["mediaId"] = mediaId; o["title"] = title; o["path"] = path; o["type"] = type;
    o["duration"] = durationSec; o["timeIn"] = inPointSec; o["timeOut"] = outPointSec;
    o["timingMode"] = timingMode;
    if (fixedOffsetSec >= 0.0) o["fixedOffsetSec"] = fixedOffsetSec;
    if (fixedUtcMs > 0) o["fixedTime"] = double(fixedUtcMs);
    o["loop"] = loop; o["isLocked"] = locked; o["priority"] = priority;
    o["transitionType"] = transition; if (transitionSec > 0) o["transitionSec"] = transitionSec;
    if (!smartConfig.isEmpty()) o["smartConfig"] = smartConfig;
    if (!cgOnStart.isEmpty()) o["cgOnStart"] = toArray(cgOnStart);
    if (!cgOnEnd.isEmpty()) o["cgOnEnd"] = toArray(cgOnEnd);
    if (!cgOverlays.isEmpty()) o["cgOverlays"] = cgOverlays;
    if (!liveSourceId.isEmpty()) o["liveSourceId"] = liveSourceId;
    if (!liveSource.isEmpty()) o["liveSource"] = liveSource;
    if (!segmentId.isEmpty()) { o["segmentId"] = segmentId; o["segmentLabel"] = segmentLabel; o["parentMediaId"] = parentMediaId; }
    if (!note.isEmpty()) o["scheduleNote"] = note;
    if (!status.isEmpty()) o["status"] = status;
    if (!queryBlock.isEmpty()) o["queryBlock"] = queryBlock;
    if (!folderBlock.isEmpty()) o["folderBlock"] = folderBlock;
    if (lockAtSec >= 0.0) o["lockAtSec"] = lockAtSec;
    if (!lockLabel.isEmpty()) o["lockLabel"] = lockLabel;
    return o;
}

SlotItem SlotItem::fromJson(const QJsonObject& o) {
    SlotItem i;
    i.id = o.value("id").toString();
    i.mediaId = o.value("mediaId").toString(o.value("assetId").toString());
    // PlayPlayClean nests the media object; lift the fields we need.
    const QJsonObject media = o.value("media").toObject();
    i.title = o.value("title").toString(media.value("title").toString());
    i.path = o.value("path").toString(o.value("filePath").toString(o.value("file_path").toString(media.value("filePath").toString())));
    i.type = o.value("type").toString(media.value("type").toString("video"));
    i.durationSec = numOr(o, "duration", "durationSec", media.value("duration").toDouble(0.0));
    i.inPointSec = numOr(o, "timeIn", "inPoint", 0.0);
    i.outPointSec = numOr(o, "timeOut", "outPoint", 0.0);
    i.timingMode = o.value("timingMode").toString("soft_start");
    i.fixedOffsetSec = o.value("fixedOffsetSec").toDouble(-1.0);
    if (o.contains("fixedTime")) {
        const QJsonValue v = o["fixedTime"];
        if (v.isDouble()) i.fixedUtcMs = qint64(v.toDouble());
        else if (v.isString()) { const QDateTime dt = QDateTime::fromString(v.toString(), Qt::ISODateWithMs); if (dt.isValid()) i.fixedUtcMs = dt.toMSecsSinceEpoch(); }
    }
    if (o.value("isFixedTime").toBool(false) && i.fixedUtcMs > 0) i.timingMode = "hard_start";
    if (i.fixedOffsetSec >= 0.0 || i.fixedUtcMs > 0) { if (i.timingMode == "soft_start") i.timingMode = "hard_start"; }
    i.loop = o.value("loop").toBool(false);
    i.locked = o.value("isLocked").toBool(o.value("locked").toBool(false));
    i.priority = o.value("priority").toInt(0);
    i.transition = o.value("transitionType").toString(o.value("transition").toString("cut"));
    i.transitionSec = o.value("transitionSec").toDouble(0.0);
    i.smartConfig = o.value("smartConfig").toObject();
    i.cgOnStart = toList(o.value("cgOnStart"));
    i.cgOnEnd = toList(o.value("cgOnEnd"));
    i.cgOverlays = o.value("cgOverlays").toArray();
    i.liveSourceId = o.value("liveSourceId").toString(o.value("source_id").toString());
    i.liveSource = o.value("liveSource").toObject(media.value("liveSource").toObject());
    i.segmentId = o.value("segmentId").toString();
    i.segmentLabel = o.value("segmentLabel").toString();
    i.parentMediaId = o.value("parentMediaId").toString();
    i.note = o.value("scheduleNote").toString(o.value("note").toString());
    i.status = o.value("status").toString();
    if (i.type == "live_source") i.type = "live";
    i.queryBlock = o.value("queryBlock").toObject(o.value("query_block").toObject());
    i.folderBlock = o.value("folderBlock").toObject(o.value("folder_block").toObject());
    // A negative lockAtSec is "no anchor" (both editors send -1 to unlock or clear): it must stay -1, not clamp to :00.
    { const QJsonValue v = o.contains("lockAtSec") ? o.value("lockAtSec") : o.value("lock_at_sec"); if (v.isDouble() && v.toDouble() >= 0.0) i.lockAtSec = qMin(v.toDouble(), 3600.0); }
    i.lockLabel = o.value("lockLabel").toString(o.value("lock_label").toString());
    // An anchor implies a lock only when the producer did not say either way (an explicit isLocked:false wins).
    if (i.lockAtSec >= 0.0 && !i.locked && !o.contains("isLocked") && !o.contains("locked")) i.locked = true;
    // A query block with no explicit duration occupies its target.
    if (i.durationSec <= 0.0 && !i.queryBlock.isEmpty()) i.durationSec = i.queryBlock.value("targetSec").toDouble(i.queryBlock.value("target_sec").toDouble(0.0));
    if (i.durationSec <= 0.0 && !i.folderBlock.isEmpty()) i.durationSec = i.folderBlock.value("targetSec").toDouble(i.folderBlock.value("target_sec").toDouble(0.0));
    // Keep anything we did not model.
    static const QStringList known = {"id","mediaId","assetId","media","title","path","filePath","file_path","type","duration","durationSec",
        "timeIn","inPoint","timeOut","outPoint","timingMode","fixedOffsetSec","fixedTime","isFixedTime","loop","isLocked","locked","priority",
        "transitionType","transition","transitionSec","smartConfig","cgOnStart","cgOnEnd","cgOverlays","liveSourceId","source_id","liveSource",
        "segmentId","segmentLabel","parentMediaId","scheduleNote","note","status","startTime","endTime","isResolved","resolvedMedia","resolvedAt",
        "queryBlock","query_block","folderBlock","folder_block","lockAtSec","lock_at_sec","lockLabel","lock_label"};
    for (auto it = o.begin(); it != o.end(); ++it) if (!known.contains(it.key())) i.extra[it.key()] = it.value();
    return i;
}

// ---------------------------------------------------------------------------
// HourSlot
// ---------------------------------------------------------------------------

double HourSlot::totalSeconds() const {
    double t = 0.0;
    for (const auto& i : items) if (i.isPlayable()) t += i.effectiveDurationSec();
    return t;
}

bool HourSlot::parseId(const QString& id, int* dayOfWeek, QDate* date, int* hour) {
    const QStringList p = id.split('-');
    if (p.size() == 2) {
        bool ok1, ok2; const int d = p[0].toInt(&ok1); const int h = p[1].toInt(&ok2);
        if (!ok1 || !ok2 || d < 0 || d > 6 || h < 0 || h > 23) return false;
        if (dayOfWeek) *dayOfWeek = d; if (date) *date = QDate(); if (hour) *hour = h;
        return true;
    }
    if (p.size() == 4) {
        const QDate d = QDate::fromString(p[0] + "-" + p[1] + "-" + p[2], Qt::ISODate);
        bool ok; const int h = p[3].toInt(&ok);
        if (!d.isValid() || !ok || h < 0 || h > 23) return false;
        if (dayOfWeek) *dayOfWeek = scheduleDow(d); if (date) *date = d; if (hour) *hour = h;
        return true;
    }
    return false;
}

QJsonObject HourSlot::toJson() const {
    QJsonObject o;
    o["id"] = id; o["channelId"] = channelId; o["hour"] = hour;
    if (isDated()) o["date"] = date.toString(Qt::ISODate); else o["dayOfWeek"] = dayOfWeek;
    QJsonArray arr; for (const auto& i : items) arr.append(i.toJson()); o["items"] = arr;
    o["isLooping"] = isLooping; o["randomizeAds"] = randomizeAds; o["keepProgramOrder"] = keepProgramOrder;
    o["blockType"] = blockType;
    if (!showBlock.isEmpty()) o["showBlock"] = showBlock;
    if (!showHour.isEmpty()) o["showHour"] = showHour;
    if (!mergedEvent.isEmpty()) o["mergedEvent"] = mergedEvent;
    if (!loopOverride.isEmpty()) o["loopOverride"] = loopOverride;
    if (!deviceCommands.isEmpty()) o["deviceCommands"] = deviceCommands;
    o["updatedAt"] = double(updatedAtMs);
    return o;
}

HourSlot HourSlot::fromJson(const QJsonObject& o) {
    HourSlot s;
    s.id = o.value("id").toString();
    s.channelId = o.value("channelId").toString();
    s.hour = o.value("hour").toInt(0);
    if (o.contains("date") && !o.value("date").toString().isEmpty()) {
        s.date = QDate::fromString(o.value("date").toString(), Qt::ISODate);
        s.dayOfWeek = s.date.isValid() ? scheduleDow(s.date) : -1;
    } else {
        s.dayOfWeek = o.value("dayOfWeek").toInt(0);
    }
    if (s.id.isEmpty()) s.id = s.isDated() ? dateId(s.date, s.hour) : templateId(s.dayOfWeek, s.hour);
    else if (!o.contains("hour")) parseId(s.id, &s.dayOfWeek, &s.date, &s.hour);
    for (const auto& v : o.value("items").toArray()) s.items.append(SlotItem::fromJson(v.toObject()));
    s.isLooping = o.value("isLooping").toBool(false);
    s.randomizeAds = o.value("randomizeAds").toBool(true);
    s.keepProgramOrder = o.value("keepProgramOrder").toBool(true);
    s.blockType = o.value("blockType").toString("general");
    s.showBlock = o.value("showBlock").toObject();
    s.showHour = o.value("showHour").toObject();
    s.mergedEvent = o.value("mergedEvent").toObject();
    s.loopOverride = o.value("loopOverride").toObject();
    s.deviceCommands = o.value("deviceCommands").toArray();
    s.updatedAtMs = qint64(o.value("updatedAt").toDouble(0));
    return s;
}

// ---------------------------------------------------------------------------
// ChannelProfile
// ---------------------------------------------------------------------------

QJsonObject ChannelProfile::toJson() const {
    QJsonObject o = extra;
    o["id"] = id; o["name"] = name; o["callLetters"] = callLetters; o["brandName"] = brandName; o["enabled"] = enabled;
    o["frameRate"] = frameRate; o["width"] = width; o["height"] = height; o["audioChannels"] = audioChannels;
    o["timezone"] = timezone; o["prerollSec"] = prerollSec; o["outputs"] = outputs; o["identity"] = identity; o["fill"] = fill;
    o["armed"] = armed; o["strict"] = strict;
    return o;
}

ChannelProfile ChannelProfile::fromJson(const QJsonObject& o) {
    ChannelProfile c;
    c.id = o.value("id").toString(o.value("channelId").toString("main"));
    c.name = o.value("name").toString(o.value("channelName").toString(c.id));
    c.callLetters = o.value("callLetters").toString();
    c.brandName = o.value("brandName").toString();
    c.enabled = o.value("enabled").toBool(true);
    c.frameRate = o.value("frameRate").isString() ? o.value("frameRate").toString() : (o.contains("frameRate") ? QString::number(o.value("frameRate").toDouble()) : "30000/1001");
    c.width = o.value("width").toInt(1920);
    c.height = o.value("height").toInt(1080);
    c.audioChannels = o.value("audioChannels").toInt(2);
    c.timezone = o.value("timezone").toString("America/New_York");
    c.prerollSec = o.value("prerollSec").toDouble(3.0);
    c.outputs = o.value("outputs").toArray();
    c.identity = o.value("identity").toObject();
    c.fill = o.value("fill").toObject();
    c.armed = o.value("armed").toBool(true);
    c.strict = o.value("strict").toBool(false);
    static const QStringList known = {"id","channelId","name","channelName","callLetters","brandName","enabled","frameRate","width","height",
        "audioChannels","timezone","prerollSec","outputs","identity","fill","armed","strict"};
    for (auto it = o.begin(); it != o.end(); ++it) if (!known.contains(it.key())) c.extra[it.key()] = it.value();
    return c;
}

// ---------------------------------------------------------------------------
// MediaRecord
// ---------------------------------------------------------------------------

QJsonObject MediaRecord::toJson() const {
    QJsonObject o = extra;
    o["id"] = id; o["path"] = path; o["filePath"] = path; o["title"] = title; o["type"] = type; o["duration"] = durationSec;
    o["programSeries"] = series; if (episodeDate.isValid()) o["episodeDate"] = episodeDate.toString(Qt::ISODate);
    o["client"] = client; o["priority"] = priority; o["playCount"] = playCount; o["lastPlayedMs"] = double(lastPlayedMs);
    o["archived"] = archived;
    if (availableStartMs > 0) o["availableStartMs"] = double(availableStartMs);
    if (availableEndMs > 0) o["availableEndMs"] = double(availableEndMs);
    if (!allowedChannels.isEmpty()) o["allowedChannels"] = toArray(allowedChannels);
    o["width"] = width; o["height"] = height; o["frameRate"] = frameRate; o["videoCodec"] = videoCodec; o["audioCodec"] = audioCodec;
    o["fileSizeBytes"] = double(fileSizeBytes); o["thumbnail"] = thumbnailPath; o["segments"] = segments; o["addedAtMs"] = double(addedAtMs);
    return o;
}

MediaRecord MediaRecord::fromJson(const QJsonObject& o) {
    MediaRecord m;
    m.id = o.value("id").toString();
    m.path = o.value("path").toString(o.value("filePath").toString(o.value("file_path").toString()));
    m.title = o.value("title").toString(o.value("display_title").toString());
    m.type = o.value("type").toString(o.value("content_type").toString("video"));
    m.durationSec = numOr(o, "duration", "duration_sec", 0.0);
    m.series = o.value("programSeries").toString(o.value("program_series").toString());
    const QString ed = o.value("episodeDate").toString(o.value("episode_date").toString());
    if (!ed.isEmpty()) m.episodeDate = QDate::fromString(ed.left(10), Qt::ISODate);
    m.client = o.value("client").toString();
    m.priority = o.value("priority").toInt(0);
    m.playCount = o.value("playCount").toInt(o.value("played_count").toInt(0));
    m.lastPlayedMs = qint64(o.value("lastPlayedMs").toDouble(0));
    m.archived = o.value("archived").toBool(false);
    m.availableStartMs = qint64(o.value("availableStartMs").toDouble(0));
    m.availableEndMs = qint64(o.value("availableEndMs").toDouble(0));
    m.allowedChannels = toList(o.value("allowedChannels"));
    m.width = o.value("width").toInt(0); m.height = o.value("height").toInt(0);
    m.frameRate = o.value("frameRate").toString();
    m.videoCodec = o.value("videoCodec").toString(); m.audioCodec = o.value("audioCodec").toString();
    m.fileSizeBytes = qint64(o.value("fileSizeBytes").toDouble(0));
    m.thumbnailPath = o.value("thumbnail").toString();
    m.segments = o.value("segments").toArray();
    m.addedAtMs = qint64(o.value("addedAtMs").toDouble(0));
    static const QStringList known = {"id","path","filePath","file_path","title","display_title","type","content_type","duration","duration_sec",
        "programSeries","program_series","episodeDate","episode_date","client","priority","playCount","played_count","lastPlayedMs","archived",
        "availableStartMs","availableEndMs","allowedChannels","width","height","frameRate","videoCodec","audioCodec","fileSizeBytes","thumbnail","segments","addedAtMs"};
    for (auto it = o.begin(); it != o.end(); ++it) if (!known.contains(it.key())) m.extra[it.key()] = it.value();
    return m;
}

// ---------------------------------------------------------------------------
// AsRunRow
// ---------------------------------------------------------------------------

QJsonObject AsRunRow::toJson() const {
    QJsonObject o = extra;
    o["rowId"] = double(rowId); o["channelId"] = channelId; o["runlistItemId"] = runlistItemId; o["itemId"] = itemId;
    o["assetId"] = assetId; o["title"] = title; o["type"] = type; o["eventType"] = eventType; o["origin"] = origin;
    o["plannedStartMs"] = double(plannedStartMs); o["actualStartMs"] = double(actualStartMs);
    o["plannedEndMs"] = double(plannedEndMs); o["actualEndMs"] = double(actualEndMs);
    o["plannedDurationMs"] = double(plannedDurationMs); o["actualDurationMs"] = double(actualDurationMs);
    o["varianceMs"] = double(varianceMs); o["frame"] = double(frame); o["timecode"] = timecode;
    o["status"] = status; o["reason"] = reason; o["slotId"] = slotId;
    if (plannedStartMs > 0) o["plannedStart"] = QDateTime::fromMSecsSinceEpoch(plannedStartMs, QTimeZone::utc()).toString(Qt::ISODateWithMs);
    if (actualStartMs > 0) o["actualStart"] = QDateTime::fromMSecsSinceEpoch(actualStartMs, QTimeZone::utc()).toString(Qt::ISODateWithMs);
    return o;
}

} // namespace sched
