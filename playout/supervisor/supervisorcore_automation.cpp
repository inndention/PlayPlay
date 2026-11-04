#include "supervisorcore.h"
#include "restapiserver.h"
#include "channelmanager.h"
#include "failoverpair.h"
#include "../schedule/runlistbuilder.h"
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUuid>
#include <algorithm>

using namespace sched;
namespace {
QJsonObject slaveOf(const ChannelProfile& p) { return p.extra.value("slave").toObject(); }
QString backupKey(const QString& id) { return "local.automation.originalSlots." + id; }
QJsonObject sourceFor(const QString& url) {
    if (url.startsWith("ndi://")) return {{"type", "ndi"}, {"name", QUrl::fromPercentEncoding(url.mid(6).toUtf8())}};
    return {{"type", "stream"}, {"url", url}, {"realtime", true}};
}
QByteArray stableSlot(HourSlot s) { auto o = s.toJson(); o.remove("updatedAtMs"); o.remove("updatedAt"); return QJsonDocument(o).toJson(QJsonDocument::Compact); }
}

void SupervisorCore::registerChannelAutomationRoutes() {
    if (m_store->setting("node.id").isEmpty()) m_store->setSetting("node.id", QUuid::createUuid().toString(QUuid::WithoutBraces));
    using Q = QHash<QString, QString>;
    auto configuration = [this]() {
        QJsonArray list;
        auto profiles = m_store->channels();
        std::stable_sort(profiles.begin(), profiles.end(), [](const ChannelProfile& a, const ChannelProfile& b) { return a.extra.value("automationSlot").toInt(999) < b.extra.value("automationSlot").toInt(999); });
        for (const auto& p : profiles) if (!p.extra.value("hidden").toBool()) {
            auto c = p.toJson(); c["slaveStatus"] = m_slaveStatus.value(p.id); list.append(c);
        }
        return QJsonObject{{"channelCount", m_store->settingJson("local.automation.channels").value("count").toInt(1)}, {"channels", list}};
    };
    m_rest->registerRoute("GET", "/api/v1/automation/channels", [configuration](const QJsonObject&, const Q&) { return configuration(); });
    m_rest->registerRoute("PUT", "/api/v1/automation/channels", [this, configuration](const QJsonObject& body, const Q&) {
        const auto val = body.value("count"); const int n = val.toInt();
        if (!val.isDouble() || val.toDouble() != n || n < 1 || n > 8) return err(422, "Automation channel count must be an integer from 1 to 8");
        auto cfg = m_store->settingJson("local.automation.channels"); auto ids = cfg.value("ids").toArray();
        if (ids.isEmpty()) { // Adopt visible existing channels before allocating slots. Never delete their schedules.
            const auto channels = m_store->channels();
            for (const auto& p : channels) if (p.id == "main" && !p.extra.value("hidden").toBool()) ids.append(p.id);
            for (const auto& p : channels) if (p.id != "main" && !p.extra.value("hidden").toBool() && ids.size() < 8) ids.append(p.id);
        }
        while (ids.size() < n) {
            QString id = "auto" + QString::number(ids.size() + 1); bool exists = false; m_store->channel(id, &exists);
            if (exists) { int suffix = 2; const QString base = id; do { id = base + "_" + QString::number(suffix++); m_store->channel(id, &exists); } while (exists); }
            ChannelProfile p; p.id = id; p.name = "Channel " + QString::number(ids.size() + 1); p.armed = false; p.enabled = false;
            if (!m_store->saveChannel(p)) return err(500, "Cannot save automation channel"); ids.append(id);
        }
        for (int i = 0; i < ids.size(); ++i) {
            bool found = false; auto p = m_store->channel(ids[i].toString(), &found); if (!found) continue;
            const bool wasEnabled = p.enabled; p.enabled = i < n; p.extra["automationSlot"] = i + 1;
            if (!m_store->saveChannel(p)) return err(500, "Cannot update channel count");
            if (!p.enabled) { m_exec->removeChannel(p.id); m_channels->removeChannel(p.id); }
            else if (!wasEnabled || !m_exec->hasChannel(p.id)) { if (m_opt.autoStartChannels) spawnChannel(p); else m_exec->addChannel(p.id); }
        }
        cfg["count"] = n; cfg["ids"] = ids; m_store->setSettingJson("local.automation.channels", cfg); return configuration();
    });
    m_rest->registerRoute("PUT", "/api/v1/automation/channels/{id}", [this, configuration](const QJsonObject& body, const Q& q) {
        bool found = false; auto p = m_store->channel(q["id"], &found); if (!found) return err(404, "No such channel");
        const QString mode = body.value("mode").toString("local");
        if (mode != "local" && mode != "relay" && mode != "mirror") return err(422, "Mode must be local, relay, or mirror");
        if (mode != "local") {
            const QString nodeId = body.value("nodeId").toString(), remote = body.value("channelId").toString(); bool known = false;
            for (const auto& v : m_store->settingJson("remote.nodes").value("nodes").toArray()) if (v.toObject().value("id").toString() == nodeId) known = true;
            if (!known || !QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(remote).hasMatch()) return err(422, "Choose a registered remote node and valid remote channel id");
            const QString input = body.value("inputUrl").toString().trimmed();
            if (mode == "relay" && !input.isEmpty() && !QRegularExpression("^(ndi|srt|udp|rtsp|rtmp|https?)://", QRegularExpression::CaseInsensitiveOption).match(input).hasMatch()) return err(422, "Relay input must be an NDI or network stream URL");
            if (!p.extra.contains("slaveOriginal")) p.extra["slaveOriginal"] = QJsonObject{{"armed", p.armed}, {"timezone", p.timezone}, {"fixedSource", p.extra.value("fixedSource")}};
            p.extra["slave"] = QJsonObject{{"mode", mode}, {"nodeId", nodeId}, {"channelId", remote}, {"inputUrl", input}};
            p.extra.remove("fixedSource"); p.armed = false; // Arm only after the first valid upstream snapshot.
        } else {
            const auto original = p.extra.value("slaveOriginal").toObject();
            const auto saved = m_store->settingJson(backupKey(p.id));
            for (auto it = saved.begin(); it != saved.end(); ++it) { if (it.value().isNull()) m_store->deleteSlot(p.id, it.key()); else m_store->saveSlot(HourSlot::fromJson(it.value().toObject())); }
            if (!saved.isEmpty()) m_store->setSettingJson(backupKey(p.id), {});
            if (!original.isEmpty()) { p.armed = original.value("armed").toBool(); p.timezone = original.value("timezone").toString(p.timezone); p.extra.remove("fixedSource"); if (original.value("fixedSource").isObject()) p.extra["fixedSource"] = original.value("fixedSource"); }
            p.extra.remove("slave"); p.extra.remove("slaveOriginal");
        }
        if (!m_store->saveChannel(p)) return err(500, "Cannot save channel mapping");
        m_slaveStatus.remove(p.id); m_exec->reloadChannel(p.id); m_exec->setArmed(p.id, p.armed);
        return configuration();
    });
    // One authenticated snapshot contains concrete current/next runlists and media identities.
    m_rest->registerRoute("GET", "/api/v1/automation/export/{id}", [this](const QJsonObject&, const Q& q) {
        bool found = false; const auto p = m_store->channel(q["id"], &found); if (!found) return err(404, "No such upstream channel");
        const auto run = m_exec->runlistJson(p.id); QJsonObject media;
        for (const char* which : {"current", "next"}) for (const auto& iv : run.value(which).toObject().value("items").toArray()) {
            const auto it = iv.toObject(); bool mf = false; auto mr = m_store->media(it.value("assetId").toString(), &mf);
            if (!mf) mr = m_store->mediaByPath(it.value("path").toString(), &mf);
            if (mf) media[it.value("path").toString()] = QJsonObject{{"id", mr.id}, {"hash", mr.extra.value("hash")}, {"hashKind", mr.extra.value("hashKind")}, {"size", double(mr.fileSizeBytes)}};
        }
        return QJsonObject{{"nodeId", m_store->setting("node.id")}, {"host", QSysInfo::machineHostName()}, {"utcMs", double(QDateTime::currentMSecsSinceEpoch())}, {"profile", p.toJson()}, {"state", m_telemetry.value(p.id)}, {"runlist", run}, {"media", media}};
    });
    auto* timer = new QTimer(this); timer->setInterval(2000);
    auto pending = std::make_shared<QSet<QString>>();
    connect(timer, &QTimer::timeout, this, [this, pending]() {
        if (m_redundancy->standby()) return;
        for (const auto& profile : m_store->channels()) {
            const auto slave = slaveOf(profile); const QString mode = slave.value("mode").toString();
            if (!profile.enabled || mode.isEmpty() || mode == "local" || pending->contains(profile.id)) continue;
            QJsonObject node; for (const auto& nv : m_store->settingJson("remote.nodes").value("nodes").toArray()) if (nv.toObject().value("id") == slave.value("nodeId")) node = nv.toObject();
            if (node.isEmpty()) { m_slaveStatus[profile.id] = {{"error", "Upstream node is unavailable"}, {"mode", mode}}; continue; }
            const QUrl url(node.value("url").toString()); if ((url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()) continue;
            pending->insert(profile.id);
            QString base = node.value("url").toString(); while (base.endsWith('/')) base.chop(1);
            m_redundancy->fetchJson("GET", base + "/api/v1/automation/export/" + QString::fromUtf8(QUrl::toPercentEncoding(slave.value("channelId").toString())), node.value("token").toString(), {}, 3000,
                [this, pending, id = profile.id, slave, mode](int http, const QJsonObject& remote, const QString& networkError) {
                pending->remove(id); bool found = false; auto p = m_store->channel(id, &found);
                if (!found || !p.enabled || slaveOf(p) != slave || m_redundancy->standby()) return; // Mapping changed while request was in flight.
                auto report = [&](const QString& e) { m_slaveStatus[id] = {{"mode", mode}, {"nodeId", slave.value("nodeId")}, {"channelId", slave.value("channelId")}, {"error", e}, {"lastPollMs", double(QDateTime::currentMSecsSinceEpoch())}}; };
                if (http != 200) { report(networkError.isEmpty() ? "Upstream HTTP " + QString::number(http) : networkError); return; }
                const auto rp = remote.value("profile").toObject();
                if (remote.value("nodeId").toString().isEmpty() || remote.value("nodeId").toString() == m_store->setting("node.id") || !rp.value("slave").toObject().isEmpty() || !rp.value("extra").toObject().value("slave").toObject().isEmpty()) { report("Choose a source channel on another node; slave chains are refused"); return; }
                bool changed = false;
                if (mode == "relay") {
                    QString input = slave.value("inputUrl").toString();
                    if (input.isEmpty()) for (const auto& ov : rp.value("outputs").toArray()) { const auto o = ov.toObject();
                        if (!o.value("enabled").toBool(true)) continue;
                        if (o.value("type").toString() == "ndi") { const QString source = o.value("sourceName").toString(o.value("options").toObject().value("sourceName").toString(o.value("name").toString("PLAY PLAY"))); input = "ndi://" + remote.value("host").toString() + " (" + source + ")"; break; }
                    }
                    if (input.isEmpty()) { report("Set the receiver URL, or enable an NDI output on the upstream channel"); return; }
                    auto source = sourceFor(input); source["title"] = "SLAVE " + slave.value("channelId").toString();
                    if (p.extra.value("fixedSource").toObject() != source) { p.extra["fixedSource"] = source; changed = true; }
                } else {
                    QHash<QString, MediaRecord> byHash, byPath;
                    for (const auto& m : m_store->allMedia()) { if (!QFileInfo::exists(m.path)) continue; byPath[m.path] = m; const QString hash = m.extra.value("hash").toString(); if (!hash.isEmpty()) byHash[hash + ":" + m.extra.value("hashKind").toString() + ":" + QString::number(m.fileSizeBytes)] = m; }
                    QList<HourSlot> mirroredHours; QString missing;
                    const auto run = remote.value("runlist").toObject(); const auto manifest = remote.value("media").toObject();
                    for (const char* which : {"current", "next"}) {
                        const auto hour = run.value(which).toObject(); const qint64 start = qint64(hour.value("hourStartMs").toDouble()); if (start <= 0) continue;
                        const auto dt = QDateTime::fromMSecsSinceEpoch(start, QTimeZone(rp.value("timezone").toString("America/New_York").toUtf8()));
                        HourSlot slot; slot.channelId = id; slot.date = dt.date(); slot.hour = dt.time().hour(); slot.id = slot.date.toString(Qt::ISODate) + "-" + QString::number(slot.hour); slot.randomizeAds = false;
                        for (const auto& iv : hour.value("items").toArray()) {
                            auto item = iv.toObject(); const QString remotePath = item.value("path").toString();
                            if (item.value("origin").toString() == "emergency") continue;
                            auto si = SlotItem::fromJson(item); si.id = item.value("itemId").toString(item.value("id").toString()); si.liveSource = item.value("source").toObject();
                            if (si.liveSource.isEmpty() && !remotePath.contains("://")) {
                                const auto ref = manifest.value(remotePath).toObject(); const QString hash = ref.value("hash").toString();
                                const QString key = hash + ":" + ref.value("hashKind").toString() + ":" + QString::number(qint64(ref.value("size").toDouble()));
                                MediaRecord local;
                                if (!hash.isEmpty() && byHash.contains(key)) local = byHash.value(key);
                                else if (byPath.contains(remotePath) && (hash.isEmpty() || byPath.value(remotePath).extra.value("hash").toString() == hash)) local = byPath.value(remotePath);
                                else { missing = "Local media missing: " + item.value("title").toString(remotePath); break; }
                                si.path = local.path; si.mediaId = local.id;
                            }
                            si.inPointSec = item.value("inPoint").toDouble(); si.outPointSec = si.inPointSec + item.value("duration").toDouble(); si.durationSec = si.outPointSec;
                            si.timingMode = "hard_start"; si.fixedUtcMs = qint64(item.value("plannedStartMs").toDouble()); slot.items.append(si);
                        }
                        mirroredHours.append(slot); if (!missing.isEmpty()) break;
                    }
                    if (!missing.isEmpty() || mirroredHours.isEmpty()) { report(missing.isEmpty() ? "Upstream schedule is not ready" : missing); return; }
                    auto backups = m_store->settingJson(backupKey(id));
                    for (const auto& slot : mirroredHours) {
                        bool exists = false; auto old = m_store->slot(id, slot.id, &exists);
                        if (exists && stableSlot(old) == stableSlot(slot)) continue;
                        if (!backups.contains(slot.id)) { backups[slot.id] = exists ? QJsonValue(old.toJson()) : QJsonValue(QJsonValue::Null); m_store->setSettingJson(backupKey(id), backups); }
                        if (!m_store->saveSlot(slot)) { report("Cannot save mirrored schedule"); return; } changed = true;
                    }
                    const QString timezone = rp.value("timezone").toString(p.timezone); if (timezone != p.timezone) { p.timezone = timezone; changed = true; }
                }
                if (!p.armed) { p.armed = true; changed = true; }
                if (changed) { if (!m_store->saveChannel(p)) { report("Cannot save slave profile"); return; } m_exec->reloadChannel(id); m_exec->setArmed(id, true); }
                report({}); m_slaveStatus[id]["connected"] = true; m_slaveStatus[id]["lastSyncMs"] = double(QDateTime::currentMSecsSinceEpoch());
            });
        }
    });
    timer->start();
}
