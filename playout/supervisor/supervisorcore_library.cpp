// SupervisorCore — the media library surface: rich queries, bulk edits, collections, ingest
// pipeline (watch folders, removable drives, jobs), thumbnails, Star agents, the ATSC mux and the
// PlayPlay OBS station-compatible API (/api/channels, /api/state, /api/media, /api/media/ingest ...)
// so the Star tooling (pp_media, pp_ingest, the AN-FS bridge "send to PlayPlay") treats this
// Supervisor as a station.
#include "supervisorcore.h"
#include "channelmanager.h"
#include "restapiserver.h"
#include "ingestmanager.h"
#include "starclient.h"
#include "meshclient.h"
#include "muxmanager.h"
#include "livestream.h"
#include "transcriptionmanager.h"
#include "mediapreview.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDir>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QTcpSocket>
#include <QDebug>
#include <QDirIterator>
#include <QCollator>
#include <QThreadPool>
#include <QPointer>
#include <QCoreApplication>
#include <QUrl>
#include <QCryptographicHash>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace sched;
using Q = QHash<QString, QString>;

static QString ctypeFor(const QString& type) {
    if (type == "ad" || type == "commercial") return "ad";
    if (type == "promo") return "promo"; if (type == "bumper") return "bumper"; if (type == "station_id") return "station_id";
    if (type == "filler" || type == "psa") return "filler"; if (type == "live") return "other";
    return "program";
}
static QString typeForCtype(const QString& c) {
    if (c == "ad" || c == "sponsor") return "ad"; if (c == "promo") return "promo"; if (c == "bumper") return "bumper"; if (c == "station_id") return "station_id"; if (c == "filler") return "filler";
    if (c == "news" || c == "weather" || c == "program") return "program"; return c.isEmpty() ? "program" : c;
}
static QString isoOrEmpty(qint64 ms) { return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC).toString(Qt::ISODate) : QString(); }
static qint64 msFromAny(const QJsonValue& v) {
    if (v.isDouble()) { const double d = v.toDouble(); return d > 1e12 ? qint64(d) : qint64(d * 1000.0); }
    const QString s = v.toString(); if (s.isEmpty()) return 0;
    QDateTime dt = QDateTime::fromString(s, Qt::ISODate); if (!dt.isValid()) dt = QDateTime::fromString(s, "yyyy-MM-dd"); if (!dt.isValid()) dt = QDateTime::fromString(s, "yyyy-MM-dd HH:mm:ss");
    return dt.isValid() ? dt.toMSecsSinceEpoch() : 0;
}

QJsonObject SupervisorCore::stationMediaRow(const MediaRecord& m) const {
    QJsonObject o;
    o["id"] = m.id; o["title"] = m.title; o["display_title"] = m.title; o["filename"] = QFileInfo(QString(m.path).replace('\\', '/')).fileName(); o["raw_filename"] = o["filename"];
    o["ctype"] = ctypeFor(m.type); o["type"] = m.type; o["category"] = m.extra.value("category").toString(); o["subcategory"] = m.extra.value("subcategory").toString();
    o["client"] = m.client; o["series"] = m.series; o["status"] = m.extra.value("status").toString("approved"); o["playout_status"] = m.extra.value("missing").toBool() ? "missing" : m.extra.value("playoutStatus").toString("ready");
    o["duration_ms"] = double(m.durationSec * 1000.0); o["file_size_bytes"] = double(m.fileSizeBytes); QJsonArray ac; for (const QString& c : m.allowedChannels) ac.append(c); o["allowed_channels"] = ac;
    o["valid_from"] = isoOrEmpty(m.availableStartMs); o["valid_until"] = isoOrEmpty(m.availableEndMs); o["expire"] = isoOrEmpty(m.availableEndMs);
    o["path"] = m.path; o["file_path"] = m.path; o["content_hash"] = m.extra.value("hash").toString(); o["created_at"] = isoOrEmpty(m.addedAtMs);
    o["thumbnail"] = m.thumbnailPath.isEmpty() ? QString() : "/thumbs/" + m.id + ".jpg"; o["play_count"] = m.playCount; o["archived"] = m.archived;
    return o;
}

QJsonObject SupervisorCore::stationChannelRow(const ChannelProfile& p) const {
    const QJsonObject t = m_telemetry.value(p.id); const auto st = m_exec->status(p.id);
    QJsonObject o; o["id"] = p.id; o["channel_id"] = p.id; o["name"] = p.name; o["state"] = t.value("state").toString("OFFLINE"); o["armed"] = st.armed; o["connected"] = m_channels->isChannelConnected(p.id);
    QJsonObject now; now["title"] = t.value("title").toString(); now["mediaId"] = t.value("assetId").toString(t.value("mediaId").toString()); now["kind"] = t.value("type").toString();
    now["durationMs"] = double(t.value("duration").toDouble() * 1000.0); now["elapsedMs"] = double(t.value("position").toDouble() * 1000.0); now["remainingMs"] = double(t.value("remaining").toDouble() * 1000.0);
    now["startedAt"] = isoOrEmpty(qint64(QDateTime::currentMSecsSinceEpoch() - t.value("position").toDouble() * 1000.0));
    o["now"] = t.value("state").toString() == "ON_AIR" ? now : QJsonValue(QJsonValue::Null);
    QJsonArray next; const QJsonObject rl = m_exec->runlistJson(p.id); const QString cur = t.value("itemId").toString(); bool after = false; int n = 0;
    for (const QJsonValue& v : rl.value("current").toObject().value("items").toArray()) { const QJsonObject it = v.toObject(); if (it.value("id").toString() == cur) { after = true; continue; } if (!after) continue; QJsonObject nx; nx["title"] = it.value("title"); nx["kind"] = it.value("type"); nx["at"] = isoOrEmpty(qint64(it.value("plannedStartMs").toDouble())); nx["durationMs"] = double(it.value("duration").toDouble() * 1000.0); next.append(nx); if (++n >= 5) break; }
    o["next"] = next; o["next_title"] = t.value("nextTitle").toString(); o["timecode"] = t.value("timecode").toString();
    return o;
}

// Mux services changed (or the mux started): (1) ndi services get their hidden `mux-<id>` channels
// created/updated/removed and the engines spawned/restarted/killed to match, (2) every channel that
// feeds a service gets the UDP TS output for it with the current statmux allocation, (3) channels that
// no longer feed one lose their `mux:*` outputs. Engines only see MSG_OUTPUT_CONFIG when something changed.
void SupervisorCore::applyMuxOutputs() {
    if (!m_mux) return;
    const QJsonObject sync = m_mux->syncManagedChannels();
    for (const QJsonValue& v : sync.value("removed").toArray()) { const QString id = v.toString(); m_exec->removeChannel(id); m_channels->removeChannel(id); qInfo().noquote() << "[Mux] managed channel removed" << id; }
    for (const QJsonValue& v : sync.value("created").toArray()) { const QString id = v.toString(); bool f = false; const ChannelProfile p = m_store->channel(id, &f); if (f) { spawnChannel(p); qInfo().noquote() << "[Mux] managed channel spawned" << id; } }
    for (const QJsonValue& v : sync.value("updated").toArray()) {
        const QJsonObject u = v.toObject(); const QString id = u.value("id").toString(); bool f = false; const ChannelProfile p = m_store->channel(id, &f); if (!f) continue;
        if (u.value("restart").toBool() || !m_channels->isChannelRunning(id)) { if (m_channels->channel(id)) m_channels->removeChannel(id); spawnChannel(p); qInfo().noquote() << "[Mux] managed channel restarted" << id; }
        else if (u.value("sourceChanged").toBool()) m_exec->reloadChannel(id);     // the executor re-pins the fixed source
    }
    QSet<QString> fed; for (const QJsonValue& v : m_mux->services()) { const QString ch = MuxManager::feedChannelFor(v.toObject()); if (!ch.isEmpty()) fed.insert(ch); }
    for (const ChannelProfile& p0 : m_store->channels()) {
        if (fed.contains(p0.id)) continue;
        QJsonArray kept; bool changed = false; for (const QJsonValue& o : p0.outputs) { if (o.toObject().value("name").toString().startsWith("mux:")) { changed = true; continue; } kept.append(o); }
        if (!changed) continue;
        ChannelProfile p = p0; p.outputs = kept; m_store->saveChannel(p);
        QJsonObject o; o["outputs"] = kept; if (m_channels->isChannelConnected(p.id)) m_channels->sendToChannel(p.id, IpcProtocol::make(IpcProtocol::MSG_OUTPUT_CONFIG, p.id, o));
        qInfo().noquote() << "[Mux] outputs removed from channel" << p.id;
    }
    QSet<QString> done;
    for (const QJsonValue& v : m_mux->services()) {
        const QString ch = MuxManager::feedChannelFor(v.toObject()); if (ch.isEmpty() || done.contains(ch)) continue; done.insert(ch);
        ChannelProfile p = m_store->channel(ch); if (p.id.isEmpty()) continue;
        const QJsonArray outs = m_mux->engineOutputsFor(ch, p.outputs);
        if (QJsonDocument(outs) == QJsonDocument(p.outputs)) continue;
        p.outputs = outs; m_store->saveChannel(p);
        QJsonObject o; o["outputs"] = outs;
        if (m_channels->isChannelConnected(ch)) m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_OUTPUT_CONFIG, ch, o));
        qInfo().noquote() << "[Mux] outputs applied to channel" << ch;
    }
}

// Streaming outputs of one channel: the stored definitions merged with the engine's live per-output
// telemetry (state, bitrate, counters), so one GET drives the Streams tables.
static QJsonObject mergedOutputs(const ChannelProfile& p, const QJsonObject& telemetry, bool engineConnected) {
    QHash<QString, QJsonObject> live; for (const QJsonValue& v : telemetry.value("outputs").toArray()) { const QJsonObject o = v.toObject(); if (o.value("kind").toString() != "encoder") live[o.value("name").toString()] = o; }
    QJsonArray outs; int running = 0;
    for (const QJsonValue& v : p.outputs) {
        QJsonObject d = v.toObject(); const QString name = d.value("name").toString(d.value("type").toString());
        QJsonObject l = live.value(name);
        if (!engineConnected) { l["state"] = "offline"; l["connected"] = false; }
        else if (l.isEmpty()) { l["state"] = d.value("enabled").toBool(true) ? "pending" : "disabled"; l["connected"] = false; }
        if (l.value("state").toString() == "running") ++running;
        d["name"] = name; d["enabled"] = d.value("enabled").toBool(true); d["autoStart"] = d.value("autoStart").toBool(true); d["live"] = l; d["state"] = l.value("state"); outs.append(d);
    }
    QJsonArray encs; for (const QJsonValue& v : telemetry.value("outputs").toArray()) if (v.toObject().value("kind").toString() == "encoder") encs.append(v);
    QJsonObject o; o["channelId"] = p.id; o["channelName"] = p.name; o["engineConnected"] = engineConnected; o["outputs"] = outs; o["encoders"] = encs; o["running"] = running; o["defined"] = outs.size(); o["hidden"] = p.extra.value("hidden").toBool(false);
    return o;
}


// Fast duration probe for drag-and-drop onto a playlist: container header only, stream info only
// when the header has no duration. Seconds, or 0 when the file cannot be opened. Thread-safe.
static double quickDurationSec(const QString& path) {
    AVFormatContext* fc = nullptr;
    const QByteArray p = QDir::toNativeSeparators(path).toUtf8();
    if (avformat_open_input(&fc, p.constData(), nullptr, nullptr) < 0) return 0.0;
    double d = fc->duration > 0 ? double(fc->duration) / AV_TIME_BASE : 0.0;
    if (d <= 0.0 && avformat_find_stream_info(fc, nullptr) >= 0 && fc->duration > 0) d = double(fc->duration) / AV_TIME_BASE;
    avformat_close_input(&fc);
    return d;
}

/// Files under `paths` (folders walked recursively, natural filename order), media files only.
static QStringList expandDropPaths(const QJsonArray& paths, int cap, int* skipped) {
    QStringList out;
    QCollator coll; coll.setNumericMode(true); coll.setCaseSensitivity(Qt::CaseInsensitive);
    for (const auto& v : paths) {
        QString path = v.toString().trimmed();
        if (path.startsWith("file:", Qt::CaseInsensitive)) path = QUrl(path).toLocalFile();
        if (path.startsWith('"') && path.endsWith('"') && path.size() > 1) path = path.mid(1, path.size() - 2);
        const QFileInfo fi(path);
        if (fi.isDir()) {
            QStringList files;
            QDirIterator it(fi.absoluteFilePath(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (it.hasNext()) { const QString f = it.next(); if (IngestManager::isMediaFile(f)) files << f; else ++*skipped; }
            std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });
            out << files;
        } else if (fi.isFile()) {
            if (IngestManager::isMediaFile(path)) out << fi.absoluteFilePath(); else ++*skipped;
        }
        if (out.size() >= cap) { out = out.mid(0, cap); break; }
    }
    return out;
}

void SupervisorCore::registerLibraryRoutes() {
    RestApiServer* r0 = m_rest.get();
    // Drop files / folders onto a playlist: expand folders, take library records where they exist,
    // probe the rest for a duration (worker thread) and queue them for full ingest. Returns playlist-ready
    // items in drop order: [{path, title, type, duration, mediaId}].
    r0->registerAsync("POST", "/api/v1/media/resolve", [this](const QJsonObject& body, const Q&, RestApiServer::Reply reply) {
        int skipped = 0;
        const QStringList files = expandDropPaths(body.value("paths").toArray(), qBound(1, body.value("limit").toInt(2000), 5000), &skipped);
        const QString defType = body.value("type").toString("program");
        const bool doImport = body.value("import").toBool(true);
        auto items = std::make_shared<QJsonArray>();
        auto toProbe = std::make_shared<QList<int>>();
        for (const QString& f : files) {
            bool found = false; const MediaRecord m = m_store->mediaByPath(QDir::toNativeSeparators(f), &found);
            bool found2 = false; const MediaRecord m2 = found ? m : m_store->mediaByPath(QDir::fromNativeSeparators(f), &found2);
            QJsonObject it; it["path"] = QDir::toNativeSeparators(f);
            if ((found || found2) && (found ? m : m2).durationSec > 0.0) {
                const MediaRecord& r = found ? m : m2;
                it["mediaId"] = r.id; it["title"] = r.title.isEmpty() ? QFileInfo(f).completeBaseName() : r.title; it["type"] = r.type; it["duration"] = r.durationSec;
            } else {
                it["mediaId"] = ""; it["title"] = QFileInfo(f).completeBaseName(); it["type"] = IngestManager::typeFromPath(f, defType); it["duration"] = 0.0;
                toProbe->append(items->size());
                if (doImport && m_ingest) m_ingest->enqueue(f, IngestManager::typeFromPath(f, defType), "drop");
            }
            items->append(it);
        }
        QPointer<SupervisorCore> self(this);
        QThreadPool::globalInstance()->start([self, items, toProbe, skipped, reply]() {
            static const QSet<QString> stills{"png", "jpg", "jpeg", "bmp", "gif", "webp", "tif", "tiff"};
            for (int i : *toProbe) {
                QJsonObject it = (*items)[i].toObject();
                const QString path = it.value("path").toString();
                double d = stills.contains(QFileInfo(path).suffix().toLower()) ? 10.0 : quickDurationSec(path);
                it["duration"] = d; if (d <= 0.0) it["error"] = "could not read duration";
                (*items)[i] = it;
            }
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, items, skipped, reply]() {
                if (!self) return;
                QJsonObject o; o["items"] = *items; o["count"] = items->size(); o["skipped"] = skipped; reply(200, o);
            }, Qt::QueuedConnection);
        });
    });

    RestApiServer* r = m_rest.get();

    // --- library: rich query ---------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/library", [this](const QJsonObject&, const Q& q) {
        const bool inclArchived = q.value("archived") == "1" || q.value("archived") == "all";
        QList<MediaRecord> all = m_store->allMedia(true);
        const QString text = q.value("q").toLower(), type = q.value("type"), status = q.value("status"), client = q.value("client").toLower(), series = q.value("series").toLower(), tag = q.value("tag").toLower();
        const bool onlyMissing = q.value("missing") == "1", onlyArchived = q.value("archived") == "only", onlyReview = q.value("review") == "1";
        QJsonObject fType, fStatus, fClient, fSeries, fOrigin; int missing = 0, archivedN = 0; double totalSec = 0; qint64 totalBytes = 0;
        QList<MediaRecord> out;
        for (const MediaRecord& m : all) {
            const bool isMissing = m.extra.value("missing").toBool(); const QString st = m.extra.value("status").toString("approved");
            if (m.archived) ++archivedN; if (isMissing) ++missing;
            fType[m.type] = fType.value(m.type).toInt() + 1; fStatus[st] = fStatus.value(st).toInt() + 1;
            if (!m.client.isEmpty()) fClient[m.client] = fClient.value(m.client).toInt() + 1; if (!m.series.isEmpty()) fSeries[m.series] = fSeries.value(m.series).toInt() + 1;
            const QString origin = m.extra.value("origin").toString(); if (!origin.isEmpty()) fOrigin[origin] = fOrigin.value(origin).toInt() + 1;
            if (onlyArchived ? !m.archived : (!inclArchived && m.archived)) continue;
            if (!type.isEmpty() && !type.split(',').contains(m.type)) continue;
            if (!status.isEmpty() && st != status) continue;
            if (onlyReview && st != "review") continue;
            if (onlyMissing && !isMissing) continue;
            if (!client.isEmpty() && m.client.toLower() != client) continue;
            if (!series.isEmpty() && m.series.toLower() != series) continue;
            if (!tag.isEmpty()) { bool has = false; for (const QJsonValue& t : m.extra.value("tags").toArray()) if (t.toString().toLower() == tag) has = true; if (!has) continue; }
            if (!text.isEmpty()) { const QString hay = (m.title + " " + m.path + " " + m.series + " " + m.client + " " + m.extra.value("notes").toString() + " " + QJsonDocument(m.extra.value("tags").toArray()).toJson(QJsonDocument::Compact) + " " + m.extra.value("transcriptText").toString()).toLower(); if (!hay.contains(text)) continue; }
            out.append(m); totalSec += m.durationSec; totalBytes += m.fileSizeBytes;
        }
        const QString sort = q.value("sort", "recent"); const bool desc = q.value("order", sort == "recent" || sort == "plays" || sort == "size" || sort == "duration" ? "desc" : "asc") == "desc";
        std::sort(out.begin(), out.end(), [&](const MediaRecord& a, const MediaRecord& b) {
            int c = 0;
            if (sort == "title") c = a.title.compare(b.title, Qt::CaseInsensitive); else if (sort == "duration") c = a.durationSec < b.durationSec ? -1 : a.durationSec > b.durationSec ? 1 : 0;
            else if (sort == "plays") c = a.playCount - b.playCount; else if (sort == "size") c = a.fileSizeBytes < b.fileSizeBytes ? -1 : a.fileSizeBytes > b.fileSizeBytes ? 1 : 0;
            else if (sort == "type") c = a.type.compare(b.type); else if (sort == "client") c = a.client.compare(b.client, Qt::CaseInsensitive); else if (sort == "series") { c = a.series.compare(b.series, Qt::CaseInsensitive); if (!c) c = a.episodeDate < b.episodeDate ? 1 : a.episodeDate > b.episodeDate ? -1 : 0; }
            else if (sort == "lastPlayed") c = a.lastPlayedMs < b.lastPlayedMs ? -1 : a.lastPlayedMs > b.lastPlayedMs ? 1 : 0; else if (sort == "expires") c = a.availableEndMs < b.availableEndMs ? -1 : a.availableEndMs > b.availableEndMs ? 1 : 0;
            else c = a.addedAtMs < b.addedAtMs ? -1 : a.addedAtMs > b.addedAtMs ? 1 : 0;
            if (c == 0) c = a.title.compare(b.title, Qt::CaseInsensitive);
            return desc ? c > 0 : c < 0;
        });
        const int offset = q.value("offset", "0").toInt(), limit = qBound(1, q.value("limit", "500").toInt(), 5000);
        QJsonArray rows; for (int i = offset; i < out.size() && i < offset + limit; ++i) rows.append(out[i].toJson());
        QJsonObject o; o["media"] = rows; o["count"] = rows.size(); o["total"] = out.size(); o["offset"] = offset; o["totalSec"] = totalSec; o["totalBytes"] = double(totalBytes);
        QJsonObject facets; facets["type"] = fType; facets["status"] = fStatus; facets["client"] = fClient; facets["series"] = fSeries; facets["origin"] = fOrigin; facets["missing"] = missing; facets["archived"] = archivedN; facets["all"] = all.size(); o["facets"] = facets;
        return o;
    });
    r->registerRoute("POST", "/api/v1/media/bulk", [this](const QJsonObject& body, const Q&) {
        const QJsonObject set = body.value("set").toObject(); int n = 0;
        for (const QJsonValue& v : body.value("ids").toArray()) {
            bool f = false; MediaRecord old = m_store->media(v.toString(), &f); if (!f) continue;
            QJsonObject merged = old.toJson(); for (auto it = set.begin(); it != set.end(); ++it) merged[it.key()] = it.value();
            MediaRecord m = MediaRecord::fromJson(merged); m.id = old.id; m.playCount = old.playCount; m.lastPlayedMs = old.lastPlayedMs; m_store->saveMedia(m); ++n;
        }
        QJsonObject o; o["updated"] = n; return o;
    });
    r->registerRoute("POST", "/api/v1/media/delete", [this](const QJsonObject& body, const Q&) {
        int n = 0, files = 0; const bool delFiles = body.value("deleteFiles").toBool(false);
        for (const QJsonValue& v : body.value("ids").toArray()) { bool f = false; const MediaRecord m = m_store->media(v.toString(), &f); if (!f) continue; if (delFiles && QFileInfo(m.path).isFile() && QFile::remove(m.path)) ++files; if (!m.thumbnailPath.isEmpty()) QFile::remove(m.thumbnailPath); m_store->deleteMedia(m.id); ++n; }
        QJsonObject o; o["deleted"] = n; o["filesDeleted"] = files; return o;
    });
    r->registerRoute("GET", "/api/v1/media/duplicates", [this](const QJsonObject&, const Q&) { QJsonObject o; o["groups"] = m_ingest->duplicates(); return o; });
    r->registerRoute("POST", "/api/v1/media/{id}/relink", [this](const QJsonObject& body, const Q& q) { if (!m_ingest->relink(q["id"], body.value("path").toString())) return err(400, "media not found or path is not a file"); return m_store->media(q["id"]).toJson(); });
    r->registerRoute("POST", "/api/v1/media/{id}/thumbnail", [this](const QJsonObject&, const Q& q) { if (!m_ingest->regenerateThumbnail(q["id"])) return err(404, "media not found or file missing"); QJsonObject o; o["queued"] = true; return o; });
    r->registerRoute("POST", "/api/v1/media/{id}/reprobe", [this](const QJsonObject&, const Q& q) { bool f = false; const MediaRecord m = m_store->media(q["id"], &f); if (!f) return err(404, "no such media"); QJsonObject meta; meta["title"] = m.title; return m_ingest->enqueue(m.path, m.type, "reprobe", meta, true); });
    r->registerRoute("GET", "/api/v1/media/export", [this](const QJsonObject&, const Q&) { QJsonArray a; for (const MediaRecord& m : m_store->allMedia(true)) a.append(m.toJson()); QJsonObject o; o["media"] = a; o["exportedAt"] = double(QDateTime::currentMSecsSinceEpoch()); return o; });
    r->registerRoute("POST", "/api/v1/media/import", [this](const QJsonObject& body, const Q&) {
        int n = 0; for (const QJsonValue& v : body.value("media").toArray()) { MediaRecord m = MediaRecord::fromJson(v.toObject()); if (m.path.isEmpty()) continue; bool f = false; const MediaRecord ex = m_store->mediaByPath(m.path, &f); if (f) m.id = ex.id; else if (!body.value("keepIds").toBool(true)) m.id.clear(); if (m_store->saveMedia(m)) ++n; }
        QJsonObject o; o["imported"] = n; return o;
    });
    // collections (settings media.collections)
    r->registerRoute("GET", "/api/v1/media/collections", [this](const QJsonObject&, const Q&) { return m_store->settingJson("media.collections"); });
    r->registerRoute("PUT", "/api/v1/media/collections/{id}", [this](const QJsonObject& body, const Q& q) {
        QJsonObject all = m_store->settingJson("media.collections"); QJsonArray cols = all.value("collections").toArray(); QJsonArray out; bool found = false;
        for (const QJsonValue& v : cols) { QJsonObject c = v.toObject(); if (c.value("id").toString() == q["id"]) { found = true; for (auto it = body.begin(); it != body.end(); ++it) c[it.key()] = it.value(); c["id"] = q["id"]; } out.append(c); }
        if (!found) { QJsonObject c = body; c["id"] = q["id"]; if (!c.contains("mediaIds")) c["mediaIds"] = QJsonArray(); if (c.value("name").toString().isEmpty()) c["name"] = q["id"]; out.append(c); }
        all["collections"] = out; m_store->setSettingJson("media.collections", all); return all;
    });
    r->registerRoute("DELETE", "/api/v1/media/collections/{id}", [this](const QJsonObject&, const Q& q) {
        QJsonObject all = m_store->settingJson("media.collections"); QJsonArray out; for (const QJsonValue& v : all.value("collections").toArray()) if (v.toObject().value("id").toString() != q["id"]) out.append(v); all["collections"] = out; m_store->setSettingJson("media.collections", all); return all;
    });
    // thumbnails as static files
    r->registerRaw("GET", "/thumbs/", [this](QTcpSocket* sock, const QString& path, const Q&) {
        const QString name = QFileInfo(path.mid(QString("/thumbs/").size())).fileName();
        const QString file = QDir(m_ingest->thumbDir()).filePath(name);
        QFile f(file);
        if (!name.endsWith(".jpg") || name.contains("..") || !f.open(QIODevice::ReadOnly)) { RestApiServer::writeRawHeader(sock, 404, "text/plain", false); sock->write("no thumbnail"); sock->disconnectFromHost(); return; }
        const QByteArray data = f.readAll();
        sock->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: max-age=600\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
        sock->flush(); sock->disconnectFromHost(); connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
    });

    // --- ingest pipeline -----------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/ingest/jobs", [this](const QJsonObject&, const Q& q) { QJsonObject o; o["jobs"] = m_ingest->jobsJson(q.value("limit", "400").toInt()); o["pending"] = m_ingest->pending(); return o; });
    r->registerRoute("POST", "/api/v1/ingest/jobs/clear", [this](const QJsonObject&, const Q&) { m_ingest->clearFinished(); QJsonObject o; o["jobs"] = m_ingest->jobsJson(); return o; });
    r->registerRoute("GET", "/api/v1/ingest/watch", [this](const QJsonObject&, const Q&) { QJsonObject o; o["folders"] = m_ingest->watchFolders(); o["removable"] = m_ingest->removableVolumes(); return o; });
    r->registerRoute("PUT", "/api/v1/ingest/watch", [this](const QJsonObject& body, const Q&) { m_ingest->setWatchFolders(body.value("folders").toArray()); QJsonObject o; o["folders"] = m_ingest->watchFolders(); return o; });
    r->registerRoute("POST", "/api/v1/ingest/rescan", [this](const QJsonObject&, const Q&) { m_ingest->rescanWatchFolders(); QJsonObject o; o["ok"] = true; o["pending"] = m_ingest->pending(); return o; });
    r->registerRoute("POST", "/api/v1/ingest/sweep", [this](const QJsonObject&, const Q&) { QJsonObject o; o["missing"] = m_ingest->sweepMissing(); return o; });
    r->registerRoute("GET", "/api/v1/ingest/config", [this](const QJsonObject&, const Q&) { QJsonObject o = m_ingest->config(); o["ffprobe"] = m_ingest->ffprobePath(); o["ffmpeg"] = m_ingest->ffmpegPath(); o["thumbDir"] = QDir::toNativeSeparators(m_ingest->thumbDir()); o["uploadDir"] = QDir::toNativeSeparators(uploadDir()); return o; });
    r->registerRoute("PUT", "/api/v1/ingest/config", [this](const QJsonObject& body, const Q&) { m_ingest->setConfig(body); return m_ingest->config(); });
    r->registerRoute("POST", "/api/v1/ingest/reprobe-all", [this](const QJsonObject& body, const Q&) {
        int n = 0; const bool all = body.value("all").toBool(false);
        for (const MediaRecord& m : m_store->allMedia(true)) {
            if (m.type == "live" || m.path.startsWith("live://") || !QFileInfo(m.path).isFile()) continue;
            if (!all && !m.extra.value("hash").toString().isEmpty() && !m.thumbnailPath.isEmpty()) continue;
            QJsonObject meta; meta["title"] = m.title; m_ingest->enqueue(m.path, m.type, "reprobe", meta, true); ++n;
        }
        QJsonObject o; o["queued"] = n; return o;
    });
    r->registerRoute("POST", "/api/v1/ingest/file", [this](const QJsonObject& body, const Q&) {
        const QString path = body.value("path").toString(); if (path.isEmpty()) return err(400, "path required");
        QJsonObject meta = body.value("meta").toObject(); if (body.contains("title")) meta["title"] = body.value("title"); if (body.contains("client")) meta["client"] = body.value("client"); if (body.contains("series")) meta["series"] = body.value("series"); if (body.contains("status")) meta["status"] = body.value("status");
        return m_ingest->enqueue(path, body.value("type").toString(IngestManager::typeFromPath(path, "program")), body.value("origin").toString("api"), meta, body.value("force").toBool(false));
    });

    // --- transcription (faster-whisper) and segments -------------------------------------------------
    r->registerRoute("GET", "/api/v1/transcribe/levels", [this](const QJsonObject&, const Q&) { QJsonObject o; o["levels"] = m_transcribe->levels(); o["config"] = m_transcribe->config(); return o; });
    r->registerRoute("GET", "/api/v1/transcribe/config", [this](const QJsonObject&, const Q&) { return m_transcribe->config(); });
    r->registerRoute("PUT", "/api/v1/transcribe/config", [this](const QJsonObject& body, const Q&) { m_transcribe->setConfig(body); return m_transcribe->config(); });
    r->registerRoute("POST", "/api/v1/transcribe/probe", [this](const QJsonObject&, const Q&) { return m_transcribe->probeEnvironment(); });
    r->registerRoute("GET", "/api/v1/transcribe/jobs", [this](const QJsonObject&, const Q&) { QJsonObject o; o["jobs"] = m_transcribe->jobsJson(); return o; });
    r->registerRoute("POST", "/api/v1/transcribe/jobs/clear", [this](const QJsonObject&, const Q&) { m_transcribe->clearFinished(); QJsonObject o; o["jobs"] = m_transcribe->jobsJson(); return o; });
    r->registerRoute("DELETE", "/api/v1/transcribe/jobs/{id}", [this](const QJsonObject&, const Q& q) { if (!m_transcribe->cancel(q["id"])) return err(404, "no such job"); QJsonObject o; o["cancelled"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/media/{id}/transcribe", [this](const QJsonObject& body, const Q& q) { return m_transcribe->enqueue(q["id"], body.value("level").toString(), body); });
    r->registerRoute("POST", "/api/v1/transcribe/bulk", [this](const QJsonObject& body, const Q&) { int n = 0; for (const QJsonValue& v : body.value("ids").toArray()) { const QJsonObject j = m_transcribe->enqueue(v.toString(), body.value("level").toString(), body); if (!j.contains("status")) ++n; } QJsonObject o; o["queued"] = n; return o; });
    r->registerRoute("GET", "/api/v1/media/{id}/transcript", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject t = m_transcribe->transcript(q["id"], &f); if (!f) return err(404, "no transcript for this media"); QJsonObject o = t; o["mediaId"] = q["id"]; return o; });
    r->registerRoute("DELETE", "/api/v1/media/{id}/transcript", [this](const QJsonObject&, const Q& q) { m_transcribe->removeTranscript(q["id"]); QJsonObject o; o["removed"] = q["id"]; return o; });
    r->registerRoute("GET", "/api/v1/media/{id}/segments", [this](const QJsonObject&, const Q& q) { return m_transcribe->segments(q["id"]); });
    r->registerRoute("PUT", "/api/v1/media/{id}/segments", [this](const QJsonObject& body, const Q& q) { return m_transcribe->setSegments(q["id"], body.value("segments").toArray(), body.value("collection").toString()); });
    r->registerRoute("PUT", "/api/v1/media/{id}/segments/collections", [this](const QJsonObject& body, const Q& q) { return m_transcribe->setCollections(q["id"], body.value("collections").toArray(), body.value("activeCollection").toString()); });
    r->registerRoute("POST", "/api/v1/media/{id}/segments/auto", [this](const QJsonObject& body, const Q& q) { return m_transcribe->autoSplit(q["id"], body); });
    r->registerRoute("POST", "/api/v1/media/{id}/segments/from-words", [this](const QJsonObject& body, const Q& q) { return m_transcribe->segmentFromWords(q["id"], body.value("firstWord").toInt(), body.value("lastWord").toInt(), body.value("label").toString()); });

    // --- segment editor: exact frames, storyboard sprites, frame info (MediaPreview) -----------------------------
    // Binary routes live outside /api/v1/media/: a raw prefix there would take over every JSON media route.
    //   GET /frames/<mediaId>.jpg?t=<sec>|frame=<n>&w=<px>     exact frame as JPEG (X-Frame / X-Frame-Time headers)
    //   GET /storyboards/<key>.jpg                              storyboard sprite written by the route below
    //   GET /api/v1/media/{id}/storyboard?n=100&w=160            sprite layout + times (generated once per file version)
    //   GET /api/v1/media/{id}/frameinfo                         fps (rational), frames, duration, raster, timecode base
    {
        auto* preview = new MediaPreview(QDir(m_opt.dataDir).filePath("storyboards"), this);
        auto finish = [](QTcpSocket* sock) { sock->flush(); sock->disconnectFromHost(); };
        auto rawError = [finish](QTcpSocket* sock, int code, const QByteArray& msg) {
            RestApiServer::writeRawHeader(sock, code, "text/plain", false); sock->write(msg); finish(sock);
        };
        r->registerRaw("GET", "/frames/", [this, preview, finish, rawError](QTcpSocket* sock, const QString& path, const Q& q) {
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);   // ours now; gone if the client leaves early
            const QString id = QFileInfo(path.mid(QString("/frames/").size())).completeBaseName();
            bool f = false; const MediaRecord m = m_store->media(id, &f);
            if (!f || m.path.isEmpty()) { rawError(sock, 404, "no such media"); return; }
            if (preview->pending.load() > 24) { rawError(sock, 503, "busy"); return; }    // a burst of scrubbing must not queue forever
            const double t = q.value("t").toDouble();
            const qint64 frameNo = q.contains("frame") ? q.value("frame").toLongLong() : -1;
            const int w = q.value("w", "640").toInt();
            const QString file = m.path;
            QPointer<QTcpSocket> ps(sock);
            ++preview->pending;
            preview->pool()->start([preview, ps, file, t, frameNo, w, finish, rawError]() {
                const MediaPreview::Frame fr = preview->frameJpeg(file, t, frameNo, w);
                --preview->pending;
                QMetaObject::invokeMethod(QCoreApplication::instance(), [ps, fr, finish, rawError]() {
                    if (!ps) return;
                    QTcpSocket* s = ps.data();
                    if (fr.jpeg.isEmpty()) { rawError(s, 500, fr.error.toUtf8()); return; }
                    QByteArray h = "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: max-age=60\r\nAccess-Control-Allow-Origin: *\r\n"
                                   "Access-Control-Expose-Headers: X-Frame, X-Frame-Time, X-Frame-Size\r\n";
                    h += "X-Frame: " + QByteArray::number(fr.frame) + "\r\nX-Frame-Time: " + QByteArray::number(fr.timeSec, 'f', 6) + "\r\n";
                    h += "X-Frame-Size: " + QByteArray::number(fr.width) + "x" + QByteArray::number(fr.height) + "\r\n";
                    h += "Content-Length: " + QByteArray::number(fr.jpeg.size()) + "\r\nConnection: close\r\n\r\n";
                    s->write(h); s->write(fr.jpeg); finish(s);
                }, Qt::QueuedConnection);
            });
        });
        r->registerRaw("GET", "/storyboards/", [preview, finish, rawError](QTcpSocket* sock, const QString& path, const Q&) {
            connect(sock, &QTcpSocket::disconnected, sock, &QTcpSocket::deleteLater);
            const QString name = QFileInfo(path.mid(QString("/storyboards/").size())).fileName();
            QFile f(QDir(preview->cacheDir()).filePath(name));
            if (!name.endsWith(".jpg") || name.contains("..") || !f.open(QIODevice::ReadOnly)) { rawError(sock, 404, "no storyboard"); return; }
            const QByteArray data = f.readAll();
            sock->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: max-age=86400\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: "
                        + QByteArray::number(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
            finish(sock);
        });
        r->registerAsync("GET", "/api/v1/media/{id}/frameinfo", [this, preview](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
            bool f = false; const MediaRecord m = m_store->media(q["id"], &f);
            if (!f || m.path.isEmpty()) { reply(404, QJsonObject{{"error", "no such media"}}); return; }
            const QString file = m.path, id = m.id;
            preview->pool()->start([preview, file, id, reply]() {
                const MediaPreview::Info in = preview->info(file);
                QJsonObject o = in.toJson(); o["id"] = id;
                o["lastTimecode"] = MediaPreview::timecode(qMax<qint64>(0, in.frames - 1), in.fpsNum, in.fpsDen);
                QMetaObject::invokeMethod(QCoreApplication::instance(), [reply, o, in]() { reply(in.ok ? 200 : 422, in.ok ? o : QJsonObject{{"error", in.error}}); }, Qt::QueuedConnection);
            });
        });
        r->registerAsync("GET", "/api/v1/media/{id}/storyboard", [this, preview](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
            bool f = false; const MediaRecord m = m_store->media(q["id"], &f);
            if (!f || m.path.isEmpty()) { reply(404, QJsonObject{{"error", "no such media"}}); return; }
            const int n = qBound(10, q.value("n", "100").toInt(), 400), w = qBound(64, q.value("w", "160").toInt(), 320);
            const QFileInfo fi(m.path);
            // The key carries the file's size and mtime, so a replaced file gets a fresh sprite.
            const QByteArray ver = QCryptographicHash::hash(QByteArray::number(fi.size()) + ":" + QByteArray::number(fi.lastModified().toMSecsSinceEpoch()), QCryptographicHash::Sha1).toHex().left(10);
            const QString key = QString("%1_%2_%3_%4").arg(m.id).arg(n).arg(w).arg(QString::fromLatin1(ver));
            const QString file = m.path;
            preview->storyboardPool()->start([preview, file, key, n, w, reply]() {
                QString err;
                const QJsonObject o = preview->storyboard(file, key, n, w, &err);
                QMetaObject::invokeMethod(QCoreApplication::instance(), [reply, o, err]() { reply(o.isEmpty() ? 422 : 200, o.isEmpty() ? QJsonObject{{"error", err}} : o); }, Qt::QueuedConnection);
            });
        });
    }

    // --- Star agents ---------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/star/agents", [this](const QJsonObject&, const Q&) { QJsonObject o; o["agents"] = m_star->agents(); o["home"] = m_star->starHome(); return o; });
    r->registerRoute("PUT", "/api/v1/star/agents", [this](const QJsonObject& body, const Q&) { m_star->setAgents(body.value("agents").toArray()); QJsonObject o; o["agents"] = m_star->agents(); return o; });
    r->registerRoute("POST", "/api/v1/star/discover", [this](const QJsonObject&, const Q&) { return m_star->discoverLocal(); });
    r->registerAsync("POST", "/api/v1/star/probe", [this](const QJsonObject&, const Q&, RestApiServer::Reply reply) { m_star->probeAll(reply); });
    r->registerAsync("GET", "/api/v1/star/{node}/status", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_star->status(q["node"], reply); });
    r->registerAsync("GET", "/api/v1/star/{node}/nowplaying", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_star->nowPlaying(q["node"], reply); });
    r->registerAsync("GET", "/api/v1/star/{node}/list", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_star->list(q["node"], q.value("path"), reply); });
    r->registerAsync("GET", "/api/v1/star/{node}/search", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { if (q.value("q").isEmpty()) { QJsonObject e; e["error"] = "q required"; reply(400, e); return; } m_star->search(q["node"], q.value("path"), q["q"], reply); });
    r->registerAsync("POST", "/api/v1/star/{node}/call", [this](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) { m_star->call(q["node"], body.value("method").toString("GET"), body.value("path").toString("/status"), body.value("body").toObject(), reply, body.value("timeoutMs").toInt(20000)); });
    r->registerRoute("POST", "/api/v1/star/fetch", [this](const QJsonObject& body, const Q&) { return m_star->startFetch(body.value("node").toString(), body.value("path").toString(), body.value("type").toString("program")); });
    r->registerRoute("POST", "/api/v1/star/push", [this](const QJsonObject& body, const Q&) {
        QString local = body.value("path").toString();
        if (local.isEmpty() && !body.value("mediaId").toString().isEmpty()) { bool f = false; const MediaRecord m = m_store->media(body.value("mediaId").toString(), &f); if (!f) return err(404, "no such media"); local = m.path; }
        return m_star->startPush(local, body.value("node").toString(), body.value("remotePath").toString(body.value("folder").toString()), body.value("overwrite").toBool(false));
    });
    // transfers from both transports in one list (the Mesh console reads this)
    r->registerRoute("GET", "/api/v1/transfers", [this](const QJsonObject&, const Q&) { QJsonArray all = m_mesh->jobs(); for (const QJsonValue& v : m_star->jobs()) all.append(v); QJsonObject o; o["jobs"] = all; return o; });
    r->registerRoute("DELETE", "/api/v1/transfers/{id}", [this](const QJsonObject&, const Q& q) { if (!m_mesh->cancel(q["id"]) && !m_star->cancel(q["id"])) return err(404, "no such job"); QJsonObject o; o["cancelled"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/transfers/clear", [this](const QJsonObject&, const Q&) { m_mesh->clearFinished(); m_star->clearFinished(); QJsonObject o; o["ok"] = true; return o; });

    // --- ATSC mux --------------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/mux/status", [this](const QJsonObject&, const Q&) { return m_mux->status(); });
    r->registerRoute("GET", "/api/v1/mux/config", [this](const QJsonObject&, const Q&) { QJsonObject o = m_mux->config(); o["services"] = m_mux->services(); o["twilio"] = m_store->settingJson("alerts.twilio").value("enabled").toBool(false); return o; });
    r->registerRoute("PUT", "/api/v1/mux/config", [this](const QJsonObject& body, const Q&) { QJsonObject cfg = body; cfg.remove("services"); m_mux->setConfig(cfg); return m_mux->config(); });
    r->registerRoute("GET", "/api/v1/mux/services", [this](const QJsonObject&, const Q&) { QJsonObject o; o["services"] = m_mux->services(); o["allocation"] = m_mux->allocation(); return o; });
    r->registerRoute("PUT", "/api/v1/mux/services", [this](const QJsonObject& body, const Q&) { m_mux->setServices(body.value("services").toArray()); QJsonObject o; o["services"] = m_mux->services(); o["allocation"] = m_mux->allocation(); return o; });
    r->registerRoute("POST", "/api/v1/mux/preset", [this](const QJsonObject& body, const Q&) { return m_mux->applyPreset(qBound(1, body.value("count").toInt(4), 16), body.value("force").toBool(false)); });
    r->registerRoute("POST", "/api/v1/mux/start", [this](const QJsonObject&, const Q&) { QString e; if (!m_mux->start(&e)) return err(409, e); return m_mux->status(); });
    r->registerRoute("POST", "/api/v1/mux/stop", [this](const QJsonObject&, const Q&) { m_mux->stop(); return m_mux->status(); });
    r->registerRoute("POST", "/api/v1/mux/restart", [this](const QJsonObject&, const Q&) { m_mux->stop(); QString e; if (!m_mux->start(&e)) return err(409, e); return m_mux->status(); });
    r->registerRoute("POST", "/api/v1/mux/validate", [this](const QJsonObject&, const Q&) { return m_mux->validateTables(); });
    r->registerRoute("GET", "/api/v1/mux/guide", [this](const QJsonObject&, const Q&) { return m_mux->guidePreview(); });
    r->registerRoute("GET", "/api/v1/mux/log", [this](const QJsonObject&, const Q& q) { QJsonObject o; o["lines"] = m_mux->log(q.value("n", "200").toInt()); return o; });
    // encoder catalogue, capabilities probe (ffmpeg encoders / TSDuck / DekTec; cached 10 min, ?refresh=1), hardware, dry run
    r->registerRoute("GET", "/api/v1/mux/encoder-presets", [this](const QJsonObject&, const Q&) { QJsonObject o; o["presets"] = MuxManager::encoderPresets(); return o; });
    r->registerRoute("GET", "/api/v1/mux/capabilities", [this](const QJsonObject&, const Q& q) { return m_mux->capabilities(q.value("refresh") == "1"); });
    r->registerRoute("GET", "/api/v1/mux/dektec", [this](const QJsonObject&, const Q& q) { QJsonObject o; o["devices"] = m_mux->dektecDevices(q.value("refresh", "1") == "1"); const QJsonObject dk = m_mux->capabilities().value("dektec").toObject(); o["dtapi"] = dk.value("dtapi"); o["dta"] = dk.value("dta"); o["tsdektec"] = dk.value("tsdektec"); if (dk.contains("raw")) o["raw"] = dk.value("raw"); return o; });
    r->registerRoute("POST", "/api/v1/mux/dryrun", [this](const QJsonObject&, const Q&) { return m_mux->dryRun(); });
    // SCTE-35 (docs/mux.md "SCTE-35"): toggles, status, manual cues
    r->registerRoute("GET", "/api/v1/mux/scte35", [this](const QJsonObject&, const Q&) { return m_mux->scte35Status(); });
    r->registerRoute("PUT", "/api/v1/mux/scte35", [this](const QJsonObject& body, const Q&) { return m_mux->setScte35(body); });
    r->registerRoute("POST", "/api/v1/mux/scte35/cue", [this](const QJsonObject& body, const Q&) { return m_mux->cue(body); });
    // PSIP tables: generated XML + summary, overrides, compile report, live analysis
    r->registerRoute("GET", "/api/v1/mux/tables", [this](const QJsonObject&, const Q&) { return m_mux->tablesJson(); });
    r->registerRoute("GET", "/api/v1/mux/tables/overrides", [this](const QJsonObject&, const Q&) { return m_mux->tableOverrides(); });
    r->registerRoute("PUT", "/api/v1/mux/tables/overrides", [this](const QJsonObject& body, const Q&) { m_mux->setTableOverrides(body); return m_mux->tableOverrides(); });
    r->registerRoute("POST", "/api/v1/mux/tables/compile", [this](const QJsonObject&, const Q&) { return m_mux->compileReport(); });
    r->registerAsync("GET", "/api/v1/mux/tables/analyze", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_mux->analyze(q.value("service"), q.value("mode", "tables"), [reply](const QJsonObject& r2) { reply(r2.contains("error") && !r2.contains("output") ? 409 : 200, r2); }); });

    // --- streaming outputs per channel (definition + live telemetry; start/stop/restart one output) -----
    r->registerRoute("GET", "/api/v1/channels/{id}/outputs", [this](const QJsonObject&, const Q& q) {
        bool f = false; const ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        return mergedOutputs(p, m_telemetry.value(p.id), m_channels->isChannelConnected(p.id));
    });
    r->registerRoute("GET", "/api/v1/outputs", [this](const QJsonObject&, const Q&) {
        QJsonArray a; for (const ChannelProfile& p : m_store->channels()) a.append(mergedOutputs(p, m_telemetry.value(p.id), m_channels->isChannelConnected(p.id)));
        QJsonObject o; o["channels"] = a; return o;
    });
    for (const QString& op : {"start", "stop", "restart"}) {
        r->registerRoute("POST", "/api/v1/channels/{id}/outputs/{name}/" + op, [this, op](const QJsonObject&, const Q& q) {
            bool f = false; const ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
            bool known = false; for (const QJsonValue& v : p.outputs) if (v.toObject().value("name").toString(v.toObject().value("type").toString()) == q["name"]) known = true;
            if (!known) return err(404, "no such output: " + q["name"]);
            if (!m_channels->isChannelConnected(p.id)) return err(409, "engine for " + p.id + " is not connected");
            QJsonObject params; params["name"] = q["name"]; params["op"] = op;
            transport(p.id, "output", params);                     // MSG_TRANSPORT_COMMAND {action:"output", name, op}
            QJsonObject o; o["ok"] = true; o["channelId"] = p.id; o["name"] = q["name"]; o["op"] = op; o["queued"] = true; return o;
        });
    }
    // read-modify-write conveniences on the profile's outputs[] (then MSG_OUTPUT_CONFIG: the engine diffs by name)
    auto pushOutputs = [this](ChannelProfile& p) {
        m_store->saveChannel(p); QJsonObject o; o["outputs"] = p.outputs;
        if (m_channels->isChannelConnected(p.id)) m_channels->sendToChannel(p.id, IpcProtocol::make(IpcProtocol::MSG_OUTPUT_CONFIG, p.id, o));
        return mergedOutputs(p, m_telemetry.value(p.id), m_channels->isChannelConnected(p.id));
    };
    r->registerRoute("POST", "/api/v1/channels/{id}/outputs/add", [this, pushOutputs](const QJsonObject& body, const Q& q) {
        bool f = false; ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        QJsonObject out = body.value("output").isObject() ? body.value("output").toObject() : body;
        const QString type = out.value("type").toString().toLower(); if (type.isEmpty()) return err(400, "output.type required");
        out["type"] = type; QString name = out.value("name").toString(type);
        QSet<QString> names; for (const QJsonValue& v : p.outputs) names.insert(v.toObject().value("name").toString(v.toObject().value("type").toString()));
        for (int n = 2; names.contains(name); ++n) name = out.value("name").toString(type) + "-" + QString::number(n);
        out["name"] = name; if (!out.contains("enabled")) out["enabled"] = true; if (!out.contains("autoStart")) out["autoStart"] = true;
        if (!out.contains("retry")) { QJsonObject rt; rt["enabled"] = true; rt["maxAttempts"] = 10; rt["backoffSec"] = 5; out["retry"] = rt; }
        if (type == "udp" || type == "srt" || type == "rtmp" || type == "rtmps" || type == "file" || type == "rtp") { if (out.value("url").toString().isEmpty()) return err(400, "url required for " + type); }
        p.outputs.append(out); return pushOutputs(p);
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}/outputs/{name}", [this, pushOutputs](const QJsonObject& body, const Q& q) {
        bool f = false; ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        QJsonArray outs; bool found = false;
        for (const QJsonValue& v : p.outputs) { QJsonObject o = v.toObject(); if (o.value("name").toString(o.value("type").toString()) == q["name"]) { found = true; for (auto it = body.begin(); it != body.end(); ++it) if (it.key() != "live" && it.key() != "state") o[it.key()] = it.value(); if (!o.contains("name")) o["name"] = q["name"]; } outs.append(o); }
        if (!found) return err(404, "no such output: " + q["name"]);
        p.outputs = outs; return pushOutputs(p);
    });
    r->registerRoute("DELETE", "/api/v1/channels/{id}/outputs/{name}", [this, pushOutputs](const QJsonObject&, const Q& q) {
        bool f = false; ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        QJsonArray outs; bool found = false;
        for (const QJsonValue& v : p.outputs) { const QJsonObject o = v.toObject(); if (o.value("name").toString(o.value("type").toString()) == q["name"]) { found = true; continue; } outs.append(o); }
        if (!found) return err(404, "no such output: " + q["name"]);
        p.outputs = outs; return pushOutputs(p);
    });
    // --- Facebook Live: one RTMPS output ("facebook-live") on a chosen channel. The stream key is write-only: it lives in settings
    //     live.facebook and is merged into the URL only in the message to the engine (livestream.h), never in channel JSON or replies. -----
    auto fbCfg = [this]() {
        QJsonObject c = m_store->settingJson("live.facebook");
        if (c.value("serverUrl").toString().isEmpty()) c["serverUrl"] = "rtmps://live-api-s.facebook.com:443/rtmp/";
        if (c.value("preset").toString().isEmpty()) c["preset"] = "stream_h264_aac_720p";
        return c;
    };
    auto fbStatus = [this, fbCfg]() {
        const QJsonObject c = fbCfg(); const QString key = c.value("streamKey").toString(), ch = c.value("channel").toString();
        QJsonObject o; o["channel"] = ch; o["serverUrl"] = c.value("serverUrl"); o["preset"] = c.value("preset"); o["hasKey"] = !key.isEmpty();
        o["keyHint"] = key.isEmpty() ? QString() : key.size() > 6 ? QString("…") + key.right(4) : QString("…"); o["outputName"] = "facebook-live";
        o["state"] = "idle"; o["live"] = false; o["engineConnected"] = false; o["configured"] = false;
        if (!ch.isEmpty()) {
            bool f = false; const ChannelProfile p = m_store->channel(ch, &f);
            if (!f) o["state"] = "channel missing";
            else {
                o["channelName"] = p.name; o["configured"] = !key.isEmpty();
                const QJsonObject mo = mergedOutputs(p, m_telemetry.value(p.id), m_channels->isChannelConnected(p.id)); o["engineConnected"] = mo.value("engineConnected");
                for (const QJsonValue& v : mo.value("outputs").toArray()) {
                    const QJsonObject d = v.toObject(); if (d.value("name").toString() != "facebook-live") continue;
                    const QJsonObject l = d.value("live").toObject(); const QString st = d.value("state").toString();
                    o["state"] = st; o["live"] = st == "running"; o["bitrateKbps"] = l.value("bitrateKbps"); o["uptimeSec"] = l.value("uptimeSec"); o["attempts"] = l.value("attempts");
                    if (l.contains("retryInSec")) o["retryInSec"] = l.value("retryInSec");
                    QString le = l.value("lastError").toString(); if (!key.isEmpty()) le.replace(key, QStringLiteral("…")); if (!le.isEmpty()) o["lastError"] = le;
                    if (l.contains("encodedWidth")) { o["width"] = l.value("encodedWidth"); o["height"] = l.value("encodedHeight"); }
                }
            }
        }
        return o;
    };
    r->registerRoute("GET", "/api/v1/live/facebook", [fbStatus](const QJsonObject&, const Q&) { return fbStatus(); });
    r->registerRoute("GET", "/api/v1/settings/live.facebook", [this, fbCfg](const QJsonObject&, const Q&) {
        QJsonObject v = fbCfg(); const bool has = !v.value("streamKey").toString().isEmpty(); v.remove("streamKey"); v["hasKey"] = has;
        QJsonObject o; o["key"] = "live.facebook"; o["value"] = QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact)); return o;
    });
    r->registerRoute("PUT", "/api/v1/live/facebook", [this, fbCfg, fbStatus, pushOutputs](const QJsonObject& body, const Q&) {
        QJsonObject cur = m_store->settingJson("live.facebook");
        if (body.contains("channel")) {
            const QString ch = body.value("channel").toString().trimmed(); bool f = false; (void)m_store->channel(ch, &f);
            if (!ch.isEmpty() && !f) return err(404, "no such channel: " + ch);
            cur["channel"] = ch;
        }
        if (body.contains("serverUrl")) {
            const QString u = body.value("serverUrl").toString().trimmed().toLower().startsWith("rtmp") ? body.value("serverUrl").toString().trimmed() : QString();
            if (u.isEmpty() && !body.value("serverUrl").toString().trimmed().isEmpty()) return err(400, "serverUrl must be an rtmp:// or rtmps:// address");
            cur["serverUrl"] = u;
        }
        if (body.contains("preset") && !body.value("preset").toString().trimmed().isEmpty()) cur["preset"] = body.value("preset").toString().trimmed();
        if (body.value("clearKey").toBool(false)) cur.remove("streamKey");
        if (body.contains("streamKey")) {
            QString k = body.value("streamKey").toString().trimmed();
            if (k.contains("://") || k.contains('/')) {                                   // a pasted full address: server + key
                const int slash = k.lastIndexOf('/'); const QString server = k.left(slash + 1); k = k.mid(slash + 1);
                if (server.toLower().startsWith("rtmp") && !body.contains("serverUrl")) cur["serverUrl"] = server;
            }
            const int q = k.indexOf('?'); if (q >= 0) k = k.left(q);
            if (!k.isEmpty()) { if (k.contains(' ') || k.size() < 8) return err(400, "that does not look like a stream key"); cur["streamKey"] = k; }
        }
        m_store->setSettingJson("live.facebook", cur);
        const QJsonObject c = fbCfg(); const QString target = c.value("channel").toString();
        // the output follows the settings: removed from every other channel, added / updated on the target
        for (const ChannelProfile& p0 : m_store->channels()) {
            QJsonArray outs; bool changed = false, seen = false;
            for (const QJsonValue& v : p0.outputs) {
                QJsonObject o = v.toObject();
                if (o.value("name").toString() != "facebook-live") { outs.append(o); continue; }
                if (p0.id != target) { changed = true; continue; }                       // moved away: dropped here (the engine stops it)
                seen = true;
                QJsonObject n = o; n["type"] = "rtmps"; n["url"] = c.value("serverUrl"); n["keyRef"] = "live.facebook"; QJsonObject pr; pr["preset"] = c.value("preset"); n["profile"] = pr; n["enabled"] = true; n["autoStart"] = false;
                if (n != o) changed = true;
                outs.append(n);
            }
            if (p0.id == target && !seen) {
                QJsonObject n; n["name"] = "facebook-live"; n["type"] = "rtmps"; n["url"] = c.value("serverUrl"); n["keyRef"] = "live.facebook"; QJsonObject pr; pr["preset"] = c.value("preset"); n["profile"] = pr;
                n["enabled"] = true; n["autoStart"] = false; QJsonObject rt; rt["enabled"] = true; rt["maxAttempts"] = 5; rt["backoffSec"] = 5; n["retry"] = rt;
                outs.append(n); changed = true;
            }
            if (changed) { ChannelProfile p = p0; p.outputs = outs; pushOutputs(p); }
        }
        return fbStatus();
    });
    for (const QString& op : {"start", "stop"}) {
        r->registerRoute("POST", "/api/v1/live/facebook/" + op, [this, op, fbCfg, fbStatus](const QJsonObject&, const Q&) {
            const QJsonObject c = fbCfg(); const QString ch = c.value("channel").toString();
            if (ch.isEmpty()) return err(409, "pick the channel to stream first");
            bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (!f) return err(404, "no such channel: " + ch);
            if (op == "start" && c.value("streamKey").toString().isEmpty()) return err(409, "paste the Facebook stream key first");
            bool known = false; for (const QJsonValue& v : p.outputs) if (v.toObject().value("name").toString() == "facebook-live") known = true;
            if (!known) return err(409, "save the Facebook Live settings first");
            if (!m_channels->isChannelConnected(p.id)) return err(409, "engine for " + p.id + " is not connected");
            QJsonObject params; params["name"] = "facebook-live"; params["op"] = op;
            transport(p.id, "output", params);
            qInfo().noquote() << "[Live] facebook" << op << "on" << p.id;
            QJsonObject o = fbStatus(); o["ok"] = true; o["op"] = op; o["queued"] = true; return o;
        });
    }
    r->registerRoute("PUT", "/api/v1/alerts/twilio", [this](const QJsonObject& body, const Q&) { QJsonObject cur = m_store->settingJson("alerts.twilio"); for (auto it = body.begin(); it != body.end(); ++it) if (!(it.key() == "authToken" && it.value().toString().isEmpty())) cur[it.key()] = it.value(); m_store->setSettingJson("alerts.twilio", cur); QJsonObject o = cur; o["authToken"] = cur.value("authToken").toString().isEmpty() ? "" : "set"; return o; });
    r->registerRoute("GET", "/api/v1/alerts/twilio", [this](const QJsonObject&, const Q&) { QJsonObject o = m_store->settingJson("alerts.twilio"); o["authToken"] = o.value("authToken").toString().isEmpty() ? "" : "set"; return o; });
    r->registerRoute("PUT", "/api/v1/station/config", [this](const QJsonObject& body, const Q&) {
        if (body.contains("password")) { m_store->setSetting("station.password", body.value("password").toString()); m_rest->setStationPassword(body.value("password").toString()); if (m_stationRest) m_stationRest->setStationPassword(body.value("password").toString()); }
        if (body.contains("port")) m_store->setSetting("station.port", QString::number(body.value("port").toInt()));
        QJsonObject o; o["port"] = m_store->setting("station.port", "0").toInt(); o["hasPassword"] = !m_store->setting("station.password").isEmpty(); o["note"] = "a port change takes effect at the next Supervisor start"; return o;
    });
    r->registerRoute("GET", "/api/v1/station/config", [this](const QJsonObject&, const Q&) { QJsonObject o; o["port"] = m_store->setting("station.port", "0").toInt(); o["hasPassword"] = !m_store->setting("station.password").isEmpty(); o["listening"] = m_stationRest != nullptr; return o; });

    // --- PlayPlay OBS station-compatible API (what the Star tools and the AN-FS bridge expect) -----------
    r->registerRoute("GET", "/api/channels", [this](const QJsonObject&, const Q&) { QJsonArray a; for (const ChannelProfile& p : m_store->channels()) a.append(stationChannelRow(p)); QJsonObject o; o["channels"] = a; o["station"] = "playplay"; o["version"] = "2.0"; return o; });
    r->registerRoute("GET", "/api/state", [this](const QJsonObject&, const Q&) { QJsonObject o; QJsonArray a; for (const ChannelProfile& p : m_store->channels()) { QJsonObject c = stationChannelRow(p); a.append(c); o[p.id] = c; } o["channels"] = a; o["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); return o; });
    r->registerRoute("GET", "/api/media", [this](const QJsonObject&, const Q& q) {
        const int limit = q.value("limit", "100000").toInt(); QJsonArray a;
        for (const MediaRecord& m : m_store->allMedia(q.value("archived") == "1")) { if (a.size() >= limit) break; a.append(stationMediaRow(m)); }
        QJsonObject o; o["media"] = a; o["count"] = a.size(); return o;
    });
    r->registerRoute("GET", "/api/media/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const MediaRecord m = m_store->media(q["id"], &f); return f ? stationMediaRow(m) : err(404, "no such media"); });
    r->registerRoute("POST", "/api/media/ingest", [this](const QJsonObject& body, const Q&) {
        QStringList paths; if (body.value("path").isString()) paths << body.value("path").toString(); for (const QJsonValue& v : body.value("paths").toArray()) paths << v.toString();
        if (paths.isEmpty()) return err(400, "path required");
        QJsonObject meta;
        if (!body.value("display_title").toString().isEmpty()) meta["title"] = body.value("display_title");
        if (!body.value("client").toString().isEmpty()) meta["client"] = body.value("client"); if (!body.value("series").toString().isEmpty()) meta["series"] = body.value("series");
        if (!body.value("category").toString().isEmpty()) meta["category"] = body.value("category"); if (!body.value("subcategory").toString().isEmpty()) meta["subcategory"] = body.value("subcategory");
        meta["status"] = body.value("review").toString("auto") == "require" ? "review" : "approved"; meta["ctype"] = body.value("ctype").toString("program"); meta["type"] = typeForCtype(body.value("ctype").toString("program"));
        if (body.contains("allowed_channels")) meta["allowedChannels"] = body.value("allowed_channels");
        const qint64 vf = msFromAny(body.value("valid_from")), vu = msFromAny(body.value("valid_until").isUndefined() ? body.value("expire") : body.value("valid_until"));
        if (vf > 0) meta["availableStartMs"] = double(vf); if (vu > 0) meta["availableEndMs"] = double(vu);
        const QString type = typeForCtype(body.value("ctype").toString("program"));
        const QString batch = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        QJsonArray ingested;
        for (const QString& p : paths) {
            const QJsonObject j = m_ingest->enqueue(p, type, "api", meta, body.value("force").toBool(false), batch);
            QJsonObject row; row["path"] = p; row["job"] = j.value("id"); row["state"] = j.value("state"); row["id"] = j.value("mediaId"); row["media_id"] = j.value("mediaId"); ingested.append(row);
        }
        QJsonObject o; o["ok"] = true; o["batch_id"] = batch; o["ingested"] = ingested; o["count"] = ingested.size(); return o;
    });
    r->registerRoute("GET", "/api/ingest/batch", [this](const QJsonObject&, const Q&) { QJsonObject o; o["jobs"] = m_ingest->jobsJson(200); return o; });
    r->registerRoute("GET", "/api/ingest/batch/{id}", [this](const QJsonObject&, const Q& q) { return m_ingest->batchJson(q["id"]); });
    r->registerRoute("GET", "/api/ingest/taxonomy", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["ctypes"] = QJsonArray{"program", "news", "weather", "ad", "sponsor", "promo", "bumper", "station_id", "filler"}; o["types"] = QJsonArray{"program", "ad", "promo", "station_id", "bumper", "filler", "psa", "image", "live"};
        QJsonObject cats; for (const MediaRecord& m : m_store->allMedia(true)) { const QString c = m.extra.value("category").toString(); if (!c.isEmpty()) cats[c] = cats.value(c).toInt() + 1; } o["categories"] = cats;
        QJsonArray chans; for (const ChannelProfile& p : m_store->channels()) chans.append(p.id); o["channels"] = chans; return o;
    });
    r->registerRoute("GET", "/api/watch-folders", [this](const QJsonObject&, const Q&) { QJsonObject o; o["watch_folders"] = m_ingest->watchFolders(); o["landing_dir"] = QDir::toNativeSeparators(uploadDir()); return o; });
    r->registerRoute("GET", "/api/events/recent", [this](const QJsonObject&, const Q& q) {
        const int limit = q.value("limit", "30").toInt(); QJsonArray ev;
        for (int i = m_recentAlerts.size() - 1; i >= 0 && ev.size() < limit; --i) { QJsonObject a = m_recentAlerts[i]; a["kind"] = "alert"; ev.append(a); }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (const ChannelProfile& p : m_store->channels()) for (const AsRunRow& r2 : m_store->asRun(p.id, now - 3600000LL * 6, now, limit)) { QJsonObject a = r2.toJson(); a["kind"] = "asrun"; ev.append(a); if (ev.size() >= limit * 2) break; }
        QJsonObject o; o["events"] = ev; return o;
    });
    // StarAgent now-playing probe (probes.playplayV2 → /api/v2/playout/current)
    r->registerRoute("GET", "/api/v2/playout/current", [this](const QJsonObject&, const Q& q) {
        const QString id = q.value("channel", m_store->channels().isEmpty() ? QString() : m_store->channels().first().id);
        const QJsonObject t = m_telemetry.value(id); const QString st = t.value("state").toString("OFFLINE");
        QJsonObject o; o["displayName"] = m_store->channel(id).name; o["mediaTitle"] = t.value("title").toString(); o["mediaId"] = t.value("assetId").toString(); o["instanceId"] = id;
        o["playbackState"] = st == "ON_AIR" ? "on_air" : st == "PAUSED" ? "paused" : st.toLower(); o["isPlaying"] = st == "ON_AIR"; o["currentTime"] = t.value("position").toDouble(); o["durationMs"] = double(t.value("duration").toDouble() * 1000.0);
        o["nextTitle"] = t.value("nextTitle").toString(); o["timecode"] = t.value("timecode").toString(); return o;
    });
}
