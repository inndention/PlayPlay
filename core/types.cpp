#include "types.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDate>
#include <QTime>

QString mediaTypeToString(MediaType type) {
    switch(type) {
        case MediaType::Video: return "video";
        case MediaType::Audio: return "audio";
        case MediaType::Image: return "image";
        case MediaType::Live: return "live";
        case MediaType::StationId: return "station_id";
        case MediaType::Bumper: return "bumper";
        case MediaType::Program: return "program";
        case MediaType::PaidProgram: return "paid_program";
        case MediaType::Ad: return "ad";
        case MediaType::Promo: return "promo";
        case MediaType::Gap: return "gap";
    }
    return "video";
}

MediaType stringToMediaType(const QString& str) {
    if (str == "video") return MediaType::Video;
    if (str == "audio") return MediaType::Audio;
    if (str == "image") return MediaType::Image;
    if (str == "live") return MediaType::Live;
    if (str == "station_id") return MediaType::StationId;
    if (str == "bumper") return MediaType::Bumper;
    if (str == "program") return MediaType::Program;
    if (str == "paid_program") return MediaType::PaidProgram;
    if (str == "ad") return MediaType::Ad;
    if (str == "promo") return MediaType::Promo;
    if (str == "gap") return MediaType::Gap;
    return MediaType::Video;
}

QString bumperSubtypeToString(BumperSubtype subtype) {
    switch(subtype) {
        case BumperSubtype::Generic: return "generic";
        case BumperSubtype::Breaking: return "breaking";
        case BumperSubtype::Program: return "program";
    }
    return "generic";
}

BumperSubtype stringToBumperSubtype(const QString& str) {
    if (str == "generic") return BumperSubtype::Generic;
    if (str == "breaking") return BumperSubtype::Breaking;
    if (str == "program") return BumperSubtype::Program;
    return BumperSubtype::Generic;
}

QJsonObject MediaItem::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["title"] = title;
    obj["duration"] = duration;
    obj["src"] = src.toString();
    obj["type"] = mediaTypeToString(type);
    if (!thumbnail.isEmpty()) obj["thumbnail"] = thumbnail;
    if (!filePath.isEmpty()) obj["filePath"] = filePath;
    if (!filename.isEmpty()) obj["filename"] = filename;
    if (!client.isEmpty()) obj["client"] = client;
    if (!programSeries.isEmpty()) obj["programSeries"] = programSeries;
    if (episodeDate.isValid()) obj["episodeDate"] = episodeDate.toString(Qt::ISODate);
    if (!notes.isEmpty()) obj["notes"] = notes;
    
    QJsonArray segsArray;
    for (const auto& seg : segments) {
        QJsonObject segObj;
        segObj["id"] = seg.id;
        segObj["label"] = seg.label;
        segObj["startTime"] = seg.startTime;
        segObj["endTime"] = seg.endTime;
        if (!seg.color.isEmpty()) segObj["color"] = seg.color;
        segsArray.append(segObj);
    }
    if (!segsArray.isEmpty()) obj["segments"] = segsArray;
    
    if (liveSource.has_value()) {
        QJsonObject liveObj;
        liveObj["kind"] = static_cast<int>(liveSource->kind);
        if (!liveSource->name.isEmpty()) liveObj["name"] = liveSource->name;
        if (!liveSource->ndiName.isEmpty()) liveObj["ndiName"] = liveSource->ndiName;
        if (!liveSource->device.isEmpty()) liveObj["device"] = liveSource->device;
        if (!liveSource->url.isEmpty()) liveObj["url"] = liveSource->url.toString();
        if (!liveSource->obsScene.isEmpty()) liveObj["obsScene"] = liveSource->obsScene;
        if (!liveSource->pipeline.isEmpty()) liveObj["pipeline"] = liveSource->pipeline;
        obj["liveSource"] = liveObj;
    }
    
    obj["bumperSubtype"] = bumperSubtypeToString(bumperSubtype);
    if (!bumperProgramId.isEmpty()) obj["bumperProgramId"] = bumperProgramId;
    
    if (addedAt.isValid()) obj["addedAt"] = addedAt.toString(Qt::ISODate);
    if (dateMade.isValid()) obj["dateMade"] = dateMade.toString(Qt::ISODate);
    if (lastPlayedAt.isValid()) obj["lastPlayedAt"] = lastPlayedAt.toString(Qt::ISODate);
    obj["playCount"] = playCount;
    obj["priority"] = priority;
    obj["archived"] = archived;
    
    return obj;
}

MediaItem MediaItem::fromJson(const QJsonObject& json) {
    MediaItem item;
    item.id = json["id"].toString();
    item.title = json["title"].toString();
    item.duration = json["duration"].toDouble();
    item.src = QUrl(json["src"].toString());
    item.type = stringToMediaType(json["type"].toString());
    item.thumbnail = json["thumbnail"].toString();
    item.filePath = json["filePath"].toString();
    item.filename = json["filename"].toString();
    item.client = json["client"].toString();
    item.programSeries = json["programSeries"].toString();
    item.episodeDate = QDate::fromString(json["episodeDate"].toString(), Qt::ISODate);
    item.notes = json["notes"].toString();
    
    QJsonArray segsArray = json["segments"].toArray();
    for (const auto& segVal : segsArray) {
        QJsonObject segObj = segVal.toObject();
        Segment seg;
        seg.id = segObj["id"].toString();
        seg.label = segObj["label"].toString();
        seg.startTime = segObj["startTime"].toDouble();
        seg.endTime = segObj["endTime"].toDouble();
        seg.color = segObj["color"].toString();
        item.segments.append(seg);
    }
    
    if (json.contains("liveSource")) {
        QJsonObject liveObj = json["liveSource"].toObject();
        LiveSourceConfig live;
        live.kind = static_cast<LiveSourceKind>(liveObj["kind"].toInt());
        live.name = liveObj["name"].toString();
        live.ndiName = liveObj["ndiName"].toString();
        live.device = liveObj["device"].toString();
        live.url = QUrl(liveObj["url"].toString());
        live.obsScene = liveObj["obsScene"].toString();
        live.pipeline = liveObj["pipeline"].toString();
        item.liveSource = live;
    }
    
    item.bumperSubtype = stringToBumperSubtype(json["bumperSubtype"].toString());
    item.bumperProgramId = json["bumperProgramId"].toString();
    
    item.addedAt = QDateTime::fromString(json["addedAt"].toString(), Qt::ISODate);
    item.dateMade = QDateTime::fromString(json["dateMade"].toString(), Qt::ISODate);
    item.lastPlayedAt = QDateTime::fromString(json["lastPlayedAt"].toString(), Qt::ISODate);
    item.playCount = json["playCount"].toInt();
    item.priority = json["priority"].toInt();
    item.archived = json["archived"].toBool();
    
    return item;
}

QJsonObject PlaylistItem::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["mediaId"] = mediaId;
    obj["media"] = media.toJson();
    obj["timeIn"] = timeIn;
    obj["timeOut"] = timeOut;
    if (startTime.isValid()) obj["startTime"] = startTime.toString(Qt::ISODate);
    if (endTime.isValid()) obj["endTime"] = endTime.toString(Qt::ISODate);
    obj["isFixedTime"] = isFixedTime;
    if (fixedTime.isValid()) obj["fixedTime"] = fixedTime.toString(Qt::ISODate);
    obj["isResolved"] = isResolved;
    
    QJsonArray obsArray;
    for (const auto& cmd : obsCommands) obsArray.append(cmd);
    if (!obsArray.isEmpty()) obj["obsCommands"] = obsArray;
    
    QJsonArray cgStartArray;
    for (const auto& cg : cgOnStart) cgStartArray.append(cg);
    if (!cgStartArray.isEmpty()) obj["cgOnStart"] = cgStartArray;
    
    QJsonArray cgEndArray;
    for (const auto& cg : cgOnEnd) cgEndArray.append(cg);
    if (!cgEndArray.isEmpty()) obj["cgOnEnd"] = cgEndArray;
    
    if (!actions.isEmpty()) obj["actions"] = actions;
    obj["priority"] = priority;
    
    return obj;
}

PlaylistItem PlaylistItem::fromJson(const QJsonObject& json) {
    PlaylistItem item;
    item.id = json["id"].toString();
    item.mediaId = json["mediaId"].toString();
    item.media = MediaItem::fromJson(json["media"].toObject());
    item.timeIn = json["timeIn"].toDouble();
    item.timeOut = json["timeOut"].toDouble();
    item.startTime = QDateTime::fromString(json["startTime"].toString(), Qt::ISODate);
    item.endTime = QDateTime::fromString(json["endTime"].toString(), Qt::ISODate);
    item.isFixedTime = json["isFixedTime"].toBool();
    item.fixedTime = QDateTime::fromString(json["fixedTime"].toString(), Qt::ISODate);
    item.isResolved = json["isResolved"].toBool();
    
    QJsonArray obsArray = json["obsCommands"].toArray();
    for (const auto& val : obsArray) item.obsCommands.append(val.toString());
    
    QJsonArray cgStartArray = json["cgOnStart"].toArray();
    for (const auto& val : cgStartArray) item.cgOnStart.append(val.toString());
    
    QJsonArray cgEndArray = json["cgOnEnd"].toArray();
    for (const auto& val : cgEndArray) item.cgOnEnd.append(val.toString());
    
    item.actions = json["actions"].toObject();
    item.priority = json["priority"].toInt(0); // Default to 0 if not present
    
    return item;
}

// WeeklySchedule helper methods
void WeeklySchedule::clear() {
    this->slotKeys.clear();
    this->slotValues.clear();
}

bool WeeklySchedule::empty() const {
    return this->slotKeys.isEmpty();
}

HourSlot* WeeklySchedule::findSlot(const QString& key) {
    int index = this->slotKeys.indexOf(key);
    if (index >= 0 && index < this->slotValues.size()) {
        return &this->slotValues[index];
    }
    return nullptr;
}

const HourSlot* WeeklySchedule::findSlot(const QString& key) const {
    int index = this->slotKeys.indexOf(key);
    if (index >= 0 && index < this->slotValues.size()) {
        return &this->slotValues[index];
    }
    return nullptr;
}

HourSlot& WeeklySchedule::operator[](const QString& key) {
    HourSlot* slot = findSlot(key);
    if (!slot) {
        this->slotKeys.append(key);
        this->slotValues.append(HourSlot());
        slot = &this->slotValues.last();
    }
    return *slot;
}

bool WeeklySchedule::contains(const QString& key) const {
    return this->slotKeys.contains(key);
}

QJsonObject WeeklySchedule::toJson() const {
    QJsonObject obj;
    QJsonObject slotsObj;
    for (int i = 0; i < this->slotKeys.size() && i < this->slotValues.size(); ++i) {
        const HourSlot& slot = this->slotValues[i];
        QJsonObject slotObj;
        slotObj["id"] = slot.id;
        slotObj["dayOfWeek"] = slot.dayOfWeek;
        slotObj["hour"] = slot.hour;
        QJsonArray itemsArray;
        for (int j = 0; j < slot.items.size(); ++j) {
            itemsArray.append(slot.items[j].toJson());
        }
        slotObj["items"] = itemsArray;
        slotObj["isLooping"] = slot.isLooping;
        slotObj["randomizeAds"] = slot.randomizeAds;
        slotObj["keepProgramOrder"] = slot.keepProgramOrder;
        slotObj["updatedAt"] = slot.updatedAt.toString(Qt::ISODate);
        slotsObj[this->slotKeys[i]] = slotObj;
    }
    obj["slots"] = slotsObj;
    obj["lastModified"] = this->lastModified.toString(Qt::ISODate);
    return obj;
}

WeeklySchedule WeeklySchedule::fromJson(const QJsonObject& json) {
    WeeklySchedule schedule;
    QJsonObject slotsObj = json["slots"].toObject();
    QJsonObject::const_iterator it;
    for (it = slotsObj.begin(); it != slotsObj.end(); ++it) {
        QJsonObject slotObj = it.value().toObject();
        HourSlot slot;
        slot.id = slotObj["id"].toString();
        slot.dayOfWeek = slotObj["dayOfWeek"].toInt();
        slot.hour = slotObj["hour"].toInt();
        QJsonArray itemsArray = slotObj["items"].toArray();
        for (int i = 0; i < itemsArray.size(); ++i) {
            slot.items.append(PlaylistItem::fromJson(itemsArray[i].toObject()));
        }
        slot.isLooping = slotObj["isLooping"].toBool();
        slot.randomizeAds = slotObj["randomizeAds"].toBool();
        slot.keepProgramOrder = slotObj["keepProgramOrder"].toBool();
        slot.updatedAt = QDateTime::fromString(slotObj["updatedAt"].toString(), Qt::ISODate);
        schedule.slotKeys.append(it.key());
        schedule.slotValues.append(slot);
    }
    schedule.lastModified = QDateTime::fromString(json["lastModified"].toString(), Qt::ISODate);
    return schedule;
}

QJsonObject AdClient::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["color"] = color;
    if (!contactName.isEmpty()) obj["contactName"] = contactName;
    if (!contactEmail.isEmpty()) obj["contactEmail"] = contactEmail;
    if (!contactPhone.isEmpty()) obj["contactPhone"] = contactPhone;
    if (!address.isEmpty()) obj["address"] = address;
    if (!website.isEmpty()) obj["website"] = website;
    if (contractStart.isValid()) obj["contractStart"] = contractStart.toString(Qt::ISODate);
    if (contractEnd.isValid()) obj["contractEnd"] = contractEnd.toString(Qt::ISODate);
    obj["budget"] = budget;
    obj["ratePerPlay"] = ratePerPlay;
    if (!billingCode.isEmpty()) obj["billingCode"] = billingCode;
    if (!terms.isEmpty()) obj["terms"] = terms;
    obj["rules"] = rules;
    if (!status.isEmpty()) obj["status"] = status;
    if (!notes.isEmpty()) obj["notes"] = notes;
    return obj;
}

AdClient AdClient::fromJson(const QJsonObject& json) {
    AdClient client;
    client.id = json["id"].toString();
    client.name = json["name"].toString();
    client.color = json["color"].toString();
    client.contactName = json["contactName"].toString();
    client.contactEmail = json["contactEmail"].toString();
    client.contactPhone = json["contactPhone"].toString();
    client.address = json["address"].toString();
    client.website = json["website"].toString();
    client.contractStart = QDate::fromString(json["contractStart"].toString(), Qt::ISODate);
    client.contractEnd = QDate::fromString(json["contractEnd"].toString(), Qt::ISODate);
    client.budget = json["budget"].toDouble();
    client.ratePerPlay = json["ratePerPlay"].toDouble();
    client.billingCode = json["billingCode"].toString();
    client.terms = json["terms"].toString();
    client.rules = json["rules"].toObject();
    client.status = json["status"].toString();
    client.notes = json["notes"].toString();
    return client;
}

QJsonObject AutomationSettings::toJson() const {
    QJsonObject obj;
    obj["alwaysPlayLatestEpisode"] = alwaysPlayLatestEpisode;
    obj["shuffleAdsInHour"] = shuffleAdsInHour;
    obj["fillGapsWithIds"] = fillGapsWithIds;
    obj["fillGapsWithBumpers"] = fillGapsWithBumpers;
    obj["maxFillDuration"] = maxFillDuration;
    obj["autoAdvance"] = autoAdvance;
    obj["loopAtEndOfSchedule"] = loopAtEndOfSchedule;
    obj["defaultCGAnimationIn"] = defaultCGAnimationIn;
    obj["defaultCGAnimationOut"] = defaultCGAnimationOut;
    obj["defaultCGDuration"] = defaultCGDuration;
    obj["trackPlayHistory"] = trackPlayHistory;
    obj["maxHistoryDays"] = maxHistoryDays;
    obj["facebookWebhookUrl"] = facebookWebhookUrl;
    obj["facebookWebhookEnabled"] = facebookWebhookEnabled;
    obj["facebookGraphEnabled"] = facebookGraphEnabled;
    obj["facebookGraphPageId"] = facebookGraphPageId;
    obj["facebookGraphAccessToken"] = facebookGraphAccessToken;
    obj["facebookPreferGraph"] = facebookPreferGraph;
    obj["openaiApiKey"] = openaiApiKey;
    obj["youtubeAccessToken"] = youtubeAccessToken;
    obj["salesReportingEndpoint"] = salesReportingEndpoint;
    return obj;
}

AutomationSettings AutomationSettings::fromJson(const QJsonObject& json) {
    AutomationSettings settings;
    settings.alwaysPlayLatestEpisode = json["alwaysPlayLatestEpisode"].toBool();
    settings.shuffleAdsInHour = json["shuffleAdsInHour"].toBool();
    settings.fillGapsWithIds = json["fillGapsWithIds"].toBool();
    settings.fillGapsWithBumpers = json["fillGapsWithBumpers"].toBool();
    settings.maxFillDuration = json["maxFillDuration"].toInt();
    settings.autoAdvance = json["autoAdvance"].toBool();
    settings.loopAtEndOfSchedule = json["loopAtEndOfSchedule"].toBool();
    settings.defaultCGAnimationIn = json["defaultCGAnimationIn"].toString();
    settings.defaultCGAnimationOut = json["defaultCGAnimationOut"].toString();
    settings.defaultCGDuration = json["defaultCGDuration"].toInt();
    settings.trackPlayHistory = json["trackPlayHistory"].toBool();
    settings.maxHistoryDays = json["maxHistoryDays"].toInt();
    settings.facebookWebhookUrl = json["facebookWebhookUrl"].toString();
    settings.facebookWebhookEnabled = json["facebookWebhookEnabled"].toBool();
    settings.facebookGraphEnabled = json["facebookGraphEnabled"].toBool();
    settings.facebookGraphPageId = json["facebookGraphPageId"].toString();
    settings.facebookGraphAccessToken = json["facebookGraphAccessToken"].toString();
    settings.facebookPreferGraph = json["facebookPreferGraph"].toBool();
    settings.openaiApiKey = json["openaiApiKey"].toString();
    settings.youtubeAccessToken = json["youtubeAccessToken"].toString();
    settings.salesReportingEndpoint = json["salesReportingEndpoint"].toString();
    return settings;
}

QJsonObject Campaign::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    obj["client"] = client;
    obj["startDate"] = startDate.toString(Qt::ISODate);
    obj["endDate"] = endDate.toString(Qt::ISODate);
    obj["targetPlays"] = targetPlays;
    obj["currentPlays"] = currentPlays;
    obj["billingCode"] = billingCode;
    QJsonArray dayparts;
    for (const QString& d : daypartRestrictions) dayparts.append(d);
    obj["daypartRestrictions"] = dayparts;
    obj["separationMinutes"] = separationMinutes;
    QJsonArray exclusions;
    for (const QString& e : competitiveExclusions) exclusions.append(e);
    obj["competitiveExclusions"] = exclusions;
    return obj;
}

Campaign Campaign::fromJson(const QJsonObject& json) {
    Campaign campaign;
    campaign.id = json["id"].toString();
    campaign.name = json["name"].toString();
    campaign.client = json["client"].toString();
    campaign.startDate = QDate::fromString(json["startDate"].toString(), Qt::ISODate);
    campaign.endDate = QDate::fromString(json["endDate"].toString(), Qt::ISODate);
    campaign.targetPlays = json["targetPlays"].toInt();
    campaign.currentPlays = json["currentPlays"].toInt();
    campaign.billingCode = json["billingCode"].toString();
    QJsonArray dayparts = json["daypartRestrictions"].toArray();
    for (const QJsonValue& v : dayparts) campaign.daypartRestrictions.append(v.toString());
    campaign.separationMinutes = json["separationMinutes"].toInt();
    QJsonArray exclusions = json["competitiveExclusions"].toArray();
    for (const QJsonValue& v : exclusions) campaign.competitiveExclusions.append(v.toString());
    return campaign;
}

QJsonObject PlayEvent::toJson() const {
    QJsonObject obj;
    obj["timestamp"] = timestamp;
    obj["duration"] = duration;
    obj["completed"] = completed;
    obj["hourSlot"] = hourSlot;
    obj["itemId"] = itemId;
    obj["mediaTitle"] = mediaTitle;
    obj["scheduledStart"] = scheduledStart;
    obj["actualStart"] = actualStart;
    obj["actualEnd"] = actualEnd;
    obj["playlistIndex"] = playlistIndex;
    obj["source"] = source;
    return obj;
}

PlayEvent PlayEvent::fromJson(const QJsonObject& json) {
    PlayEvent event;
    event.timestamp = json["timestamp"].toVariant().toLongLong();
    event.duration = json["duration"].toDouble();
    event.completed = json["completed"].toBool();
    event.hourSlot = json["hourSlot"].toString();
    event.itemId = json["itemId"].toString();
    event.mediaTitle = json["mediaTitle"].toString();
    event.scheduledStart = json["scheduledStart"].toVariant().toLongLong();
    event.actualStart = json["actualStart"].toVariant().toLongLong();
    event.actualEnd = json["actualEnd"].toVariant().toLongLong();
    event.playlistIndex = json["playlistIndex"].toInt();
    event.source = json["source"].toString();
    return event;
}

QJsonObject MediaStats::toJson() const {
    QJsonObject obj;
    obj["playCount"] = playCount;
    obj["totalPlayTime"] = totalPlayTime;
    obj["lastPlayed"] = lastPlayed;
    QJsonArray history;
    for (const PlayEvent& e : playHistory) history.append(e.toJson());
    obj["playHistory"] = history;
    return obj;
}

MediaStats MediaStats::fromJson(const QJsonObject& json) {
    MediaStats stats;
    stats.playCount = json["playCount"].toInt();
    stats.totalPlayTime = json["totalPlayTime"].toDouble();
    stats.lastPlayed = json["lastPlayed"].toVariant().toLongLong();
    QJsonArray history = json["playHistory"].toArray();
    for (const QJsonValue& v : history) stats.playHistory.append(PlayEvent::fromJson(v.toObject()));
    return stats;
}

QJsonObject AsPlayedEntry::toJson() const {
    QJsonObject obj = PlayEvent::toJson();
    obj["mediaId"] = mediaId;
    return obj;
}

AsPlayedEntry AsPlayedEntry::fromJson(const QJsonObject& json) {
    AsPlayedEntry entry;
    PlayEvent base = PlayEvent::fromJson(json);
    entry = static_cast<const AsPlayedEntry&>(base);
    entry.mediaId = json["mediaId"].toString();
    return entry;
}

QJsonObject FreePlaylist::toJson() const {
    QJsonObject obj;
    obj["id"] = id;
    obj["name"] = name;
    QJsonArray itemsArray;
    for (const PlaylistItem& item : items) itemsArray.append(item.toJson());
    obj["items"] = itemsArray;
    obj["isLocked"] = isLocked;
    obj["createdAt"] = createdAt;
    obj["createdFrom"] = createdFrom;
    return obj;
}

FreePlaylist FreePlaylist::fromJson(const QJsonObject& json) {
    FreePlaylist playlist;
    playlist.id = json["id"].toString();
    playlist.name = json["name"].toString();
    QJsonArray itemsArray = json["items"].toArray();
    for (const QJsonValue& v : itemsArray) playlist.items.append(PlaylistItem::fromJson(v.toObject()));
    playlist.isLocked = json["isLocked"].toBool();
    playlist.createdAt = json["createdAt"].toVariant().toLongLong();
    playlist.createdFrom = json["createdFrom"].toString();
    return playlist;
}





