// SupervisorCore — integrations: live-input inventory (NDI scan across engines), ONVIF cameras
// with PTZ, the HTML CG template pack (web/cg), the AN-FS mesh (multi-node media library) and
// the caption overlays (karaoke captions from transcripts, fact boxes; docs/cg-captions.md).
// Kept apart from supervisorcore.cpp so the core start-up and schedule routes stay readable.
#include "supervisorcore.h"
#ifdef PLAYPLAY_HAVE_REPORTERFEED
#include "reporterfeed.h"
#endif
#include "cgtext.h"
#include "cgdesigner.h"
#include "channelmanager.h"
#include "restapiserver.h"
#include "onvifmanager.h"
#include "meshclient.h"
#include "transcriptionmanager.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDateTime>
#include <QUrlQuery>
#include <QTimer>
#include <QDebug>
#include <functional>

using namespace sched;
using Q = QHash<QString, QString>;

// ---------------------------------------------------------------------------------------------
// Captions: karaoke overlays driven by transcripts, fact boxes (state lives here; the core header
// is not touched). Follow state is persisted under the setting key cg.karaokeFollow.<channel>.
// ---------------------------------------------------------------------------------------------
namespace {
struct KaraokeFollow {
    bool follow = false;            // re-issue the overlay whenever the on-air asset changes
    int layer = 31;
    QString role = "lower_third";
    QString mode = "word";          // word | line
    QJsonObject style;              // position, colours, fontSize, maxLines... merged into the spec
    QString lastKey;                // on-air item (itemId|assetId) the poller / START handler last acted on; "" = off air
    bool shown = false;             // a karaoke layer of ours is on the engine
    QString shownAssetId;
    QString eventItemId;            // item of the last as-run START we acted on ...
    qint64 eventAtMs = 0;           // ... and when: telemetry still showing the previous item is stale for ~1 s after it
};
QHash<QString, KaraokeFollow> g_karaoke;        // channelId -> state

// What is on air, from engine telemetry or from an as-run START event (which beats telemetry by up to 1 s).
struct OnAir {
    QString asset, itemId, state;   // state: engine state name (ON_AIR, PAUSED, IDLE...)
    double inPoint = -1.0;          // the engine's own in point of the item when it reports one (< 0 = unknown)
    bool force = false;             // re-issue even when the item key is unchanged (START: the engine cut the layer at the take)
    QString key() const { return itemId + "|" + asset; }
};

QJsonObject karaokeFollowJson(const KaraokeFollow& k) {
    QJsonObject o; o["follow"] = k.follow; o["layer"] = k.layer; o["role"] = k.role; o["mode"] = k.mode; o["style"] = k.style; return o;
}
void karaokeFollowLoad(KaraokeFollow& k, const QJsonObject& o) {
    if (o.isEmpty()) return;
    k.follow = o.value("follow").toBool(false); k.layer = o.value("layer").toInt(31); k.role = o.value("role").toString("lower_third");
    k.mode = o.value("mode").toString("word"); k.style = o.value("style").toObject();
}

// Compact the stored transcript for the IPC payload: word timings (no probabilities, and no segment
// text when the words carry it: only segments without word times need their text), word count returned.
// An EMPTY result means "nothing to caption": transcribe.py writes {"segments": [], "text": ""} for
// music / silent media, and TranscriptionManager::transcript() still reports that file as found.
QJsonArray compactSegments(const QJsonObject& transcript, int* words) {
    QJsonArray out; int n = 0;
    for (const QJsonValue& sv : transcript.value("segments").toArray()) {
        const QJsonObject s = sv.toObject();
        QJsonObject c; c["start"] = s.value("start").toDouble(); c["end"] = s.value("end").toDouble();
        const QString text = s.value("text").toString().trimmed();
        QJsonArray ws;
        for (const QJsonValue& wv : s.value("words").toArray()) {
            const QJsonObject w = wv.toObject();
            const QString txt = w.value("word").toString(w.value("text").toString()).trimmed();
            if (txt.isEmpty()) continue;
            QJsonObject cw; cw["word"] = txt; cw["start"] = w.value("start").toDouble(); cw["end"] = w.value("end").toDouble(); ws.append(cw); ++n;
        }
        if (!ws.isEmpty()) c["words"] = ws; else c["text"] = text;
        if (!ws.isEmpty() || !text.isEmpty()) out.append(c);
    }
    if (words) *words = n;
    return out;
}

// Cheap form of "compactSegments() is not empty" (GET route: no payload is built).
bool transcriptHasText(const QJsonObject& transcript) {
    for (const QJsonValue& sv : transcript.value("segments").toArray()) {
        const QJsonObject s = sv.toObject();
        if (!s.value("text").toString().trimmed().isEmpty()) return true;
        for (const QJsonValue& wv : s.value("words").toArray()) { const QJsonObject w = wv.toObject(); if (!w.value("word").toString(w.value("text").toString()).trimmed().isEmpty()) return true; }
    }
    return false;
}

// Does this (on-air) runlist item ask for karaoke captions? Accepted markers:
//   cgOnStart: "karaoke" | "karaoke:{json opts}" | '{"kind":"karaoke",...}' | '{"source":{"kind":"karaoke"}}'
//   cgOverlays: {templateId:"karaoke", layer, ...} | {command:{source:{kind:"karaoke"}}}
// The engine ignores the bare string (identityCg knows no "karaoke"), so only the Supervisor
// expands it: it has the transcript store, the engine does not.
bool itemWantsKaraoke(const QJsonObject& item, QJsonObject* opts) {
    auto kindOf = [](const QJsonObject& o) {
        if (o.value("kind").toString() == "karaoke") return true;
        if (o.value("source").toObject().value("kind").toString() == "karaoke") return true;
        if (o.value("template").toObject().value("kind").toString() == "karaoke") return true;
        return o.value("templateId").toString() == "karaoke" || o.value("command").toObject().value("source").toObject().value("kind").toString() == "karaoke";
    };
    for (const QJsonValue& v : item.value("cgOnStart").toArray()) {
        if (v.isString()) {
            const QString s = v.toString().trimmed();
            if (s.compare("karaoke", Qt::CaseInsensitive) == 0) return true;
            if (s.startsWith("karaoke:", Qt::CaseInsensitive)) { if (opts) *opts = QJsonDocument::fromJson(s.mid(8).toUtf8()).object(); return true; }
            if (s.startsWith('{')) { const QJsonObject o = QJsonDocument::fromJson(s.toUtf8()).object(); if (kindOf(o)) { if (opts) { *opts = o.value("source").toObject(); if (o.contains("layer")) (*opts)["layer"] = o["layer"]; if (o.contains("role")) (*opts)["role"] = o["role"]; } return true; } }
        } else if (v.isObject() && kindOf(v.toObject())) {
            const QJsonObject o = v.toObject(); if (opts) { *opts = o.value("source").toObject(o); if (o.contains("layer")) (*opts)["layer"] = o["layer"]; if (o.contains("role")) (*opts)["role"] = o["role"]; }
            return true;
        }
    }
    for (const QJsonValue& v : item.value("cgOverlays").toArray()) {
        const QJsonObject o = v.toObject();
        if (!kindOf(o)) continue;
        if (opts) {
            *opts = o.value("command").toObject().value("source").toObject();
            if (o.contains("layer")) (*opts)["layer"] = o["layer"];
            if (o.contains("role")) (*opts)["role"] = o["role"];
            // One named object: begin() and end() of two temporaries (o["style"].toObject() twice) made the
            // iterator dangle (heap corruption on the Supervisor main thread).
            const QJsonObject style = o.value("style").toObject();
            for (auto it = style.begin(); it != style.end(); ++it) (*opts)[it.key()] = it.value();
        }
        return true;
    }
    return false;
}

// Fact box position -> layer placement (anchor + title-safe 5 % margins on the channel raster).
QJsonObject factboxPlacement(const QString& position, int rasterW, int rasterH) {
    QJsonObject p; const double mx = rasterW * 0.05, my = rasterH * 0.05;
    const QString pos = position.isEmpty() ? "right" : position;
    p["anchor"] = pos == "left" ? "left" : pos == "top-right" || pos == "top_right" ? "top_right" : pos == "top-left" || pos == "top_left" ? "top_left"
                : pos == "bottom-left" || pos == "bottom_left" ? "bottom_left" : pos == "bottom-right" || pos == "bottom_right" ? "bottom_right" : pos == "center" ? "center" : "right";
    p["marginX"] = mx; p["marginY"] = my;
    return p;
}
} // namespace

// ---------------------------------------------------------------------------------------------
// CG manifest (web/cg/templates.json), cached by mtime
// ---------------------------------------------------------------------------------------------
QJsonObject SupervisorCore::cgManifest() {
    const QString path = QDir(m_opt.webRoot).filePath("cg/templates.json");
    const QFileInfo fi(path);
    const qint64 mt = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0;
    if (mt != m_cgManifestMs || m_cgManifest.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) { m_cgManifest = cgtext::annotateManifest(QJsonDocument::fromJson(f.readAll()).object()); m_cgManifestMs = mt; }
        else { m_cgManifest = QJsonObject(); m_cgManifest["templates"] = QJsonArray(); m_cgManifest["error"] = "web/cg/templates.json not found under the web root"; }
    }
    return CgDesigner::mergeInto(m_cgManifest);      // + the CG Designer's templates (live: not part of the mtime cache)
}

static QJsonObject findTemplate(const QJsonObject& manifest, const QString& id) {
    for (const QJsonValue& v : manifest.value("templates").toArray()) if (v.toObject().value("id").toString() == id) return v.toObject();
    return QJsonObject();
}

// Build "/cg/<template>.html?name=..&animIn=.." from params + animation (same query vocabulary as js/cg-client.js).
static QString cgUrlFor(const QString& templateId, const QJsonObject& params, const QJsonObject& anim) {
    QUrlQuery q;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const QJsonValue v = it.value();
        const QString s = v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? "1" : "0") : v.isDouble() ? QString::number(v.toDouble()) : QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
        if (!s.isEmpty()) q.addQueryItem(it.key(), s);
    }
    const QString in = anim.value("in").toString(), out = anim.value("out").toString();
    if (!in.isEmpty() && in != "none") { q.addQueryItem("animIn", in); q.addQueryItem("animInDuration", QString::number(anim.value("duration").toInt(anim.value("inDuration").toInt(600)))); }
    if (!out.isEmpty() && out != "none") { q.addQueryItem("animOut", out); q.addQueryItem("animOutDuration", QString::number(anim.value("outDuration").toInt(anim.value("duration").toInt(400)))); }
    if (anim.contains("easing") && anim.value("easing").toString() != "ease-out") q.addQueryItem("animEasing", anim.value("easing").toString());
    if (anim.value("delay").toInt() > 0) q.addQueryItem("animDelay", QString::number(anim.value("delay").toInt()));
    if (anim.value("autoHide").toInt() > 0) q.addQueryItem("autoHide", QString::number(anim.value("autoHide").toInt()));
    if (anim.value("loop").toBool()) q.addQueryItem("loop", "1");
    const QString qs = q.toString(QUrl::FullyEncoded);
    return "/cg/" + templateId + ".html" + (qs.isEmpty() ? "" : "?" + qs);
}

// Native fallback: express a pack template with the engine's own Qt-rendered template kinds.
static QJsonObject nativeCgFor(const QJsonObject& tpl, const QJsonObject& params, const QString& role, const QJsonObject& anim) {
    const QString cat = tpl.value("category").toString();
    auto p = [&](const char* k, const QString& def = QString()) { const QString v = params.value(k).toString(); return v.isEmpty() ? def : v; };
    QJsonObject cmd; cmd["action"] = "show"; cmd["role"] = role;
    QJsonObject src; src["type"] = "template";
    QJsonObject place;
    const double inDur = anim.value("duration").toInt(600) / 1000.0, outDur = anim.value("outDuration").toInt(anim.value("duration").toInt(400)) / 1000.0;
    auto tr = [](const QString& a, const QString& def) { if (a.startsWith("slide-left") || a == "wipe-left") return QString("slide_left"); if (a.startsWith("slide-right") || a == "wipe-right") return QString("slide_right"); if (a == "slide-up") return QString("slide_up"); if (a == "slide-down") return QString("slide_down"); if (a == "fade" || a == "zoom" || a == "flip" || a == "bounce" || a == "typewriter") return QString("fade"); return def; };
    if (cat == "lower-thirds" || cat == "banners" || cat == "social") {
        src["kind"] = "lower_third"; src["title"] = p("name", p("title", p("text", tpl.value("name").toString()))); src["subtitle"] = params.contains("name") ? p("title") : p("subtitle", p("label"));
        place["anchor"] = p("position") == "right" ? "bottom_right" : p("position") == "center" ? "bottom" : "bottom_left"; place["marginX"] = 120; place["marginY"] = 90;
    } else if (cat == "bugs" || cat == "score-bugs" || cat == "weather" || cat == "sports-extended") {
        src["kind"] = "bug"; src["text"] = p("text", p("label", p("name", p("team1", "LIVE")))); place["anchor"] = "top_right"; place["margin"] = 60;
    } else if (cat == "tickers") {
        src["kind"] = "ticker"; src["text"] = p("text", p("items", "PLAY PLAY")); src["label"] = p("label", "NEWS"); place["anchor"] = "bottom";
    } else if (cat == "countdown") {
        src["kind"] = "countdown"; src["seconds"] = p("seconds", "60").toDouble(); src["label"] = p("label", p("title")); place["anchor"] = "center";
    } else if (cat == "emergency") {
        src = QJsonObject(); src["type"] = "bars"; QJsonArray caps; caps.append(p("message", p("text", "EMERGENCY"))); if (!p("subtitle").isEmpty()) caps.append(p("subtitle")); src["captions"] = caps;
    } else {
        QJsonObject e; e["error"] = "no native equivalent for category '" + cat + "' — use the HTML output page (/cg/output.html) or build with PLAYPLAY_WITH_WEBENGINE"; e["status"] = 409; return e;
    }
    if (!p("color1").isEmpty()) src["accent"] = "#" + p("color1").remove('#');
    cmd["source"] = src; if (!place.isEmpty()) cmd["placement"] = place;
    QJsonObject in; in["type"] = tr(anim.value("in").toString(), "fade"); in["durationSec"] = inDur; cmd["in"] = in;
    QJsonObject out; out["type"] = tr(anim.value("out").toString(), "fade"); out["durationSec"] = outDur; cmd["out"] = out;
    if (anim.value("autoHide").toInt() > 0) cmd["holdSec"] = anim.value("autoHide").toInt() / 1000.0;
    return cmd;
}

// ---------------------------------------------------------------------------------------------
// Scheduled device commands (HourSlot.deviceCommands: [{id, commandId, offsetSec, label}])
// ---------------------------------------------------------------------------------------------
void SupervisorCore::deviceCommandTick() {
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 nowMs = now.toMSecsSinceEpoch();
    const QDateTime hourStart(now.date(), QTime(now.time().hour(), 0));
    const qint64 hourMs = hourStart.toMSecsSinceEpoch();
    if (m_firedDeviceCmds.size() > 4000) m_firedDeviceCmds.clear();
    for (const ChannelProfile& p : m_store->channels()) {
        if (!p.enabled) continue;
        const HourSlot slot = m_store->effectiveSlot(p.id, now.date(), now.time().hour());
        for (const QJsonValue& v : slot.deviceCommands) {
            const QJsonObject c = v.toObject();
            const qint64 fireAt = hourMs + qint64(c.value("offsetSec").toDouble() * 1000.0);
            if (nowMs < fireAt || nowMs > fireAt + 5000) continue;
            const QString key = p.id + "|" + slot.id + "|" + QString::number(hourMs) + "|" + c.value("id").toString(c.value("commandId").toString());
            if (m_firedDeviceCmds.contains(key)) continue;
            m_firedDeviceCmds.insert(key);
            runDeviceCommand(p.id, c);
        }
    }
}

void SupervisorCore::runDeviceCommand(const QString& channelId, const QJsonObject& c) {
    // Forms: {commandId: "ptz:<camera>:preset:<token>" | "ptz:<camera>:home" | "ptz:<camera>:move:x,y,z,secs" | "cg:<json>" | "transport:<action>"}
    //        or explicit fields {device, action, preset, x, y, z}.
    QString cmdId = c.value("commandId").toString();
    const QString label = c.value("label").toString(cmdId);
    auto reply = [this, channelId, label](int st, const QJsonObject& b) {
        pushAlert(channelId, st < 400 ? "info" : "warning", QString("device command '%1' %2").arg(label, st < 400 ? "sent" : "failed: " + b.value("error").toString()));
    };
    if (c.contains("device") && m_onvif) { m_onvif->ptz(c.value("device").toString(), c, reply); return; }
    if (cmdId.startsWith("ptz:") && m_onvif) {
        const QStringList parts = cmdId.split(':');
        if (parts.size() < 3) { reply(400, QJsonObject{{"error", "bad ptz command"}}); return; }
        QJsonObject cmd; cmd["action"] = parts[2] == "preset" || parts[2] == "goto" ? "preset" : parts[2];
        if (parts.size() > 3) {
            if (parts[2] == "move") { const QStringList n = parts[3].split(','); cmd["x"] = n.value(0).toDouble(); cmd["y"] = n.value(1).toDouble(); cmd["z"] = n.value(2).toDouble(); if (n.size() > 3) cmd["timeoutSec"] = n.value(3).toDouble(); }
            else cmd["preset"] = parts[3];
        }
        // camera may be given by id or by name
        QString dev = parts[1];
        bool found = false; m_onvif->deviceJson(dev, &found);
        if (!found) for (const QJsonValue& v : m_onvif->devicesJson()) if (v.toObject().value("name").toString().compare(dev, Qt::CaseInsensitive) == 0) { dev = v.toObject().value("id").toString(); break; }
        m_onvif->ptz(dev, cmd, reply);
        return;
    }
    if (cmdId.startsWith("cg:")) {
        const QJsonObject cg = QJsonDocument::fromJson(cmdId.mid(3).toUtf8()).object();
        if (cg.isEmpty()) { reply(400, QJsonObject{{"error", "cg command is not JSON"}}); return; }
        m_channels->sendToChannel(channelId, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, channelId, cg));
        reply(200, QJsonObject());
        return;
    }
    if (cmdId.startsWith("transport:")) { transport(channelId, cmdId.mid(10), QJsonObject()); reply(200, QJsonObject()); return; }
    reply(400, QJsonObject{{"error", "unknown command id: " + cmdId}});
}

// ---------------------------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------------------------
void SupervisorCore::registerIntegrationRoutes() {
    RestApiServer* r = m_rest.get();

    // --- live inputs: NDI scan results from every engine, merged ---------------------------------
    r->registerRoute("GET", "/api/v1/inputs", [this](const QJsonObject&, const Q&) {
        QHash<QString, QJsonObject> ndi; qint64 last = 0;
        for (auto it = m_telemetry.begin(); it != m_telemetry.end(); ++it) {
            last = qMax(last, qint64(it.value().value("liveInputsAtMs").toDouble()));
            for (const QJsonValue& v : it.value().value("liveInputs").toArray()) {
                QJsonObject o = v.toObject(); const QString key = o.value("name").toString();
                QJsonArray seen = ndi.value(key).value("seenBy").toArray(); seen.append(it.key()); o["seenBy"] = seen; ndi[key] = o;
            }
        }
        QJsonArray arr; for (const QJsonObject& o : ndi) arr.append(o);
        QJsonObject out; out["ndi"] = arr; out["ndiCount"] = arr.size(); out["lastScanMs"] = double(last);
        out["ndiConfig"] = m_store->settingJson("ndi.discovery");
        out["cameras"] = m_onvif ? m_onvif->devicesJson() : QJsonArray(); out["camerasDiscovering"] = m_onvif && m_onvif->discovering(); out["camerasLastMs"] = m_onvif ? double(m_onvif->lastDiscoveryMs()) : 0.0;
        out["engines"] = m_telemetry.size();
#ifdef PLAYPLAY_HAVE_REPORTERFEED
        out["reporter"] = reporterfeed::inputsJson();   // reporter feed sessions (cmake/lane-reporterfeed.cmake)
#endif
        return out;
    });
    r->registerRoute("POST", "/api/v1/inputs/scan", [this](const QJsonObject& body, const Q&) {
        int n = 0; QJsonObject p; p["ndi"] = m_store->settingJson("ndi.discovery");
        for (const QString& id : m_channels->channelIds()) if (m_channels->isChannelConnected(id) && transport(id, "rescan_inputs", p)) ++n;
        const bool cams = body.value("cameras").toBool(true);
        if (cams && m_onvif) m_onvif->discover(body.value("waitMs").toInt(3000));
        QJsonObject o; o["enginesAsked"] = n; o["cameras"] = cams; return o;
    });
    r->registerRoute("GET", "/api/v1/inputs/ndi-config", [this](const QJsonObject&, const Q&) { return m_store->settingJson("ndi.discovery"); });
    r->registerRoute("PUT", "/api/v1/inputs/ndi-config", [this](const QJsonObject& body, const Q&) {
        QJsonObject cfg; cfg["groups"] = body.value("groups").toString(); cfg["extraIps"] = body.value("extraIps").toString();
        m_store->setSettingJson("ndi.discovery", cfg);
        QJsonObject upd; upd["ndi"] = cfg;
        for (const QString& id : m_channels->channelIds()) if (m_channels->isChannelConnected(id)) sendConfig(id, upd);
        return cfg;
    });

    // --- cameras (ONVIF) ------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/cameras", [this](const QJsonObject&, const Q&) {
        QJsonObject o; o["cameras"] = m_onvif->devicesJson(); o["discovering"] = m_onvif->discovering(); o["lastDiscoveryMs"] = double(m_onvif->lastDiscoveryMs()); return o;
    });
    r->registerRoute("POST", "/api/v1/cameras/discover", [this](const QJsonObject& body, const Q&) {
        m_onvif->discover(body.value("waitMs").toInt(3000)); QJsonObject o; o["started"] = true; return o;
    });
    r->registerRoute("POST", "/api/v1/cameras", [this](const QJsonObject& body, const Q&) {
        if (body.value("xaddr").toString().isEmpty() && body.value("host").toString().isEmpty() && body.value("id").toString().isEmpty()) return err(400, "xaddr, host or id required");
        return m_onvif->saveDevice(body);
    });
    r->registerRoute("GET", "/api/v1/cameras/{id}", [this](const QJsonObject&, const Q& q) { bool f = false; const QJsonObject o = m_onvif->deviceJson(q["id"], &f); return f ? o : err(404, "no such camera"); });
    r->registerRoute("DELETE", "/api/v1/cameras/{id}", [this](const QJsonObject&, const Q& q) { if (!m_onvif->removeDevice(q["id"])) return err(404, "no such camera"); QJsonObject o; o["deleted"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/cameras/{id}/refresh", [this](const QJsonObject&, const Q& q) { m_onvif->refresh(q["id"]); QJsonObject o; o["refreshing"] = q["id"]; return o; });
    r->registerAsync("POST", "/api/v1/cameras/{id}/ptz", [this](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) { m_onvif->ptz(q["id"], body, reply); });
    r->registerAsync("GET", "/api/v1/cameras/{id}/presets", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_onvif->presets(q["id"], reply); });
    r->registerAsync("GET", "/api/v1/cameras/{id}/stream", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_onvif->streamUri(q["id"], reply); });
    // Register the camera's RTSP stream as a live `stream` source in the media library.
    r->registerAsync("POST", "/api/v1/cameras/{id}/register", [this](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const QString id = q["id"];
        m_onvif->streamUri(id, [this, id, body, reply](int st, const QJsonObject& b) {
            if (st != 200) { reply(st, b); return; }
            const QJsonObject cam = m_onvif->deviceJson(id);
            MediaRecord m; m.path = "live://camera/" + id; m.type = "live"; m.title = body.value("title").toString(cam.value("name").toString("Camera " + id));
            m.durationSec = body.value("duration").toDouble(1800);
            QJsonObject src; src["type"] = "stream"; src["url"] = b.value("streamUri").toString(); src["realtime"] = true; src["lowLatency"] = true;
            m.extra["liveSource"] = src; m.extra["camera"] = id;
            bool found = false; const MediaRecord ex = m_store->mediaByPath(m.path, &found);
            if (found) m.id = ex.id;
            m_store->saveMedia(m);
            QJsonObject o = m.toJson(); o["registered"] = true; reply(200, o);
        });
    });

    // --- HTML CG templates (web/cg) --------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/cg/templates", [this](const QJsonObject&, const Q&) { return cgManifest(); });
    r->registerRoute("GET", "/api/v1/cg/html/{ch}", [this](const QJsonObject&, const Q& q) {
        bool ok = false; int output = q.value("output", "1").toInt(&ok); if (!ok || output < 1 || output > 8) return err(422, "CG output must be 1..8");
        const QString key = output == 1 ? q["ch"] : q["ch"] + ":" + QString::number(output);
        QJsonObject o = m_cgHtml.value(key); if (o.isEmpty()) { o["channelId"] = q["ch"]; o["output"] = output; o["url"] = ""; } return o;
    });
    r->registerRoute("POST", "/api/v1/cg/html/{ch}", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"], tid = body.value("template").toString();
        const int output = body.value("output").toInt(1); if ((body.contains("output") && (!body.value("output").isDouble() || body.value("output").toDouble() != output)) || output < 1 || output > 8) return err(422, "CG output must be 1..8");
        const QString key = output == 1 ? ch : ch + ":" + QString::number(output);
        const QJsonObject tpl = findTemplate(cgManifest(), tid);
        if (tpl.isEmpty()) return err(404, "unknown template: " + tid);
        const QJsonObject params = cgtext::applyText(tpl, body.value("params").toObject());
        const QJsonObject anim = body.value("animation").toObject();
        const QString role = body.value("role").toString(tpl.value("role").toString("secondary"));
        QJsonObject st; st["channelId"] = ch; st["output"] = output; st["template"] = tid; st["name"] = tpl.value("name"); st["params"] = params; st["animation"] = anim; st["role"] = role;
        st["url"] = cgUrlFor(tid, params, anim); st["atMs"] = double(QDateTime::currentMSecsSinceEpoch());
        st["absoluteUrl"] = QString("http://127.0.0.1:%1%2").arg(m_opt.restPort).arg(st["url"].toString());
        // Delivery: the output page follows "cgHtml" over the WebSocket; optionally also put it on air.
        QString onAir = body.value("onAir").toString();      // "" | "html" | "native"
        if (body.value("onAir").isBool() && body.value("onAir").toBool()) onAir = "html";
        const int layer = 70 + output;
        if (output > 1 && onAir.isEmpty() && m_cgHtml.value(key).contains("layer")) {
            QJsonObject hide{{"action", "hide"}, {"layer", m_cgHtml.value(key).value("layer")}, {"transition", QJsonObject{{"type", "fade"}, {"durationSec", 0.4}}}};
            m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, ch, hide));
        }
        if (output > 1 && !onAir.isEmpty()) {
            const auto prior = m_cgHtml.value(key);
            for (const auto& v : m_telemetry.value(ch).value("layers").toArray()) if (v.toObject().value("id").toInt(-1) == layer && prior.value("layer").toInt(-1) != layer) return err(409, "CG output layer is already in use; clear that layer first");
        }
        if (onAir == "html") {
            QJsonObject cmd; cmd["action"] = "show"; cmd["role"] = role;
            if (output > 1) { cmd["layer"] = layer; st["layer"] = layer; }
            QJsonObject src; src["type"] = "html"; src["url"] = st["absoluteUrl"]; src["width"] = 1920; src["height"] = 1080; cmd["source"] = src;
            QJsonObject in; in["type"] = "cut"; cmd["in"] = in;
            m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, ch, cmd));
            st["onAir"] = "html";
        } else if (onAir == "native") {
            QJsonObject cmd = nativeCgFor(tpl, params, role, anim);
            if (cmd.contains("status")) return cmd;
            if (output > 1) { cmd["layer"] = layer; st["layer"] = layer; }
            m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, ch, cmd));
            st["onAir"] = "native"; st["nativeCommand"] = cmd;
        }
        m_cgHtml[key] = st;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, st, "cgHtml");
        return st;
    });
    r->registerRoute("POST", "/api/v1/cg/html/{ch}/clear", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"];
        const int output = body.value("output").toInt(1); if ((body.contains("output") && (!body.value("output").isDouble() || body.value("output").toDouble() != output)) || output < 1 || output > 8) return err(422, "CG output must be 1..8");
        const QString key = output == 1 ? ch : ch + ":" + QString::number(output);
        const QJsonObject prev = m_cgHtml.take(key);
        if (!prev.value("onAir").toString().isEmpty() || body.value("hide").toBool()) {
            QJsonObject cmd; cmd["action"] = "hide"; cmd["role"] = prev.value("role").toString(body.value("role").toString("secondary"));
            if (prev.contains("layer")) cmd["layer"] = prev.value("layer");
            QJsonObject tr; tr["type"] = "fade"; tr["durationSec"] = 0.4; cmd["transition"] = tr;
            m_channels->sendToChannel(ch, IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, ch, cmd));
        }
        QJsonObject st; st["channelId"] = ch; st["output"] = output; st["url"] = ""; st["cleared"] = true;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, st, "cgHtml");
        return st;
    });

    // --- AN-FS mesh ------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/mesh/status", [this](const QJsonObject&, const Q&) { return m_mesh->status(); });
    r->registerRoute("PUT", "/api/v1/mesh/config", [this](const QJsonObject& body, const Q&) { m_mesh->setConfig(body); return m_mesh->status(); });
    r->registerAsync("GET", "/api/v1/mesh/nodes", [this](const QJsonObject&, const Q&, RestApiServer::Reply reply) { m_mesh->nodes(reply); });
    r->registerAsync("GET", "/api/v1/mesh/browse", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) { m_mesh->browse(q.value("node"), q.value("path"), reply); });
    r->registerAsync("GET", "/api/v1/mesh/search", [this](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        if (q.value("q").trimmed().isEmpty()) { QJsonObject e; e["error"] = "q required"; reply(400, e); return; }
        m_mesh->search(q.value("q"), q.value("node"), reply);
    });
    r->registerRoute("POST", "/api/v1/mesh/fetch", [this](const QJsonObject& body, const Q&) {
        const QString path = body.value("path").toString();
        if (path.isEmpty()) return err(400, "path required");
        return m_mesh->startFetch(body.value("node").toString(), path, body.value("type").toString("program"), body.value("folder").toString());
    });
    r->registerRoute("POST", "/api/v1/mesh/push", [this](const QJsonObject& body, const Q&) {
        QString local = body.value("path").toString();
        if (local.isEmpty() && !body.value("mediaId").toString().isEmpty()) { bool f = false; const MediaRecord m = m_store->media(body.value("mediaId").toString(), &f); if (!f) return err(404, "no such media"); local = m.path; }
        if (local.isEmpty()) return err(400, "path or mediaId required");
        return m_mesh->startPush(local, body.value("node").toString(), body.value("folder").toString());
    });
    r->registerRoute("GET", "/api/v1/mesh/jobs", [this](const QJsonObject&, const Q&) { QJsonObject o; o["jobs"] = m_mesh->jobs(); return o; });
    r->registerRoute("DELETE", "/api/v1/mesh/jobs/{id}", [this](const QJsonObject&, const Q& q) { if (!m_mesh->cancel(q["id"])) return err(404, "no such job"); QJsonObject o; o["cancelled"] = q["id"]; return o; });
    r->registerRoute("POST", "/api/v1/mesh/jobs/clear", [this](const QJsonObject&, const Q&) { m_mesh->clearFinished(); QJsonObject o; o["jobs"] = m_mesh->jobs(); return o; });

    // --- captions: karaoke overlays from transcripts ---------------------------------------------
    // Load persisted follow state for every channel.
    for (const ChannelProfile& p : m_store->channels()) karaokeFollowLoad(g_karaoke[p.id], m_store->settingJson("cg.karaokeFollow." + p.id));

    // The item on air as the engine reports it (telemetry; the START handler below builds one from the event).
    std::function<OnAir(const QString&)> onAirOf = [this](const QString& ch) {
        const QJsonObject t = m_telemetry.value(ch);
        OnAir a; a.asset = t.value("assetId").toString(); a.itemId = t.value("itemId").toString(); a.state = t.value("state").toString();
        if (t.contains("inPoint")) a.inPoint = t.value("inPoint").toDouble(-1.0);      // PlayoutEngine::stateJson reports it once engine.cpp does
        return a;
    };
    // The schedule's runlist item for it: telemetry itemId is the RUNLIST id (the engine item id), so it is
    // looked up by that, not by asset (the whole-hour Runlist object has no assetId/inPoint/cgOnStart of its
    // own: those live in its items). Null for operator takes and ids the executor no longer remembers.
    std::function<const RunlistItem*(const QString&, const OnAir&)> runlistItemOf = [this](const QString& ch, const OnAir& a) -> const RunlistItem* {
        if (!m_exec || a.itemId.isEmpty()) return nullptr;
        const RunlistItem* ri = m_exec->lookupRunlistItem(ch, a.itemId);
        if (!ri) return nullptr;
        return (ri->item.mediaId.isEmpty() ? ri->item.id : ri->item.mediaId) == a.asset ? ri : nullptr;     // engine assetId = mediaId, else the slot item id
    };

    // Show the karaoke layer for `mediaIdIn` (empty = the asset on air). `opts` may carry layer, role,
    // mode, style{} and any spec key (position, colours, syncOffsetSec...). `oa` is the item on air.
    // Returns the result or err() (404 when the media has no transcript text, 409 when nothing is on air).
    std::function<QJsonObject(const QString&, const QString&, const QJsonObject&, const OnAir&)> karaokeShow =
        [this, runlistItemOf](const QString& ch, const QString& mediaIdIn, const QJsonObject& opts, const OnAir& oa) -> QJsonObject {
        if (!m_transcribe) return err(503, "transcription manager not available");
        const QString mediaId = mediaIdIn.isEmpty() ? oa.asset : mediaIdIn;
        if (mediaId.isEmpty()) return err(409, "nothing on air on " + ch + " and no mediaId given");
        bool found = false; const QJsonObject tr = m_transcribe->transcript(mediaId, &found);
        int nWords = 0; const QJsonArray segs = found ? compactSegments(tr, &nWords) : QJsonArray();
        if (segs.isEmpty()) return err(404, "no transcript for media " + mediaId);     // missing file, or a transcript without any speech
        // Time base: the engine reports the item position AFTER its in point; words are file-relative, so
        // the layer adds inPointSec (see TemplateSource::karaokeTime). Preference: caller, then the engine's
        // own in point (it knows a join-in-progress offset the runlist does not), then the runlist item's.
        double inPoint = opts.value("inPointSec").toDouble(-1.0);
        if (inPoint < 0.0) {
            inPoint = 0.0;
            if (mediaId == oa.asset) {
                if (oa.inPoint >= 0.0) inPoint = oa.inPoint;
                else if (const RunlistItem* ri = runlistItemOf(ch, oa)) inPoint = ri->inSec;
            }
        }
        KaraokeFollow& k = g_karaoke[ch];
        const int layer = opts.value("layer").toInt(k.layer > 0 ? k.layer : 31);
        const QString role = opts.value("role").toString(k.role.isEmpty() ? "lower_third" : k.role);
        QJsonObject src = k.style;
        const QJsonObject style = opts.value("style").toObject();
        for (auto it = style.begin(); it != style.end(); ++it) src[it.key()] = it.value();
        static const char* passKeys[] = {"position", "align", "maxLines", "maxChars", "fontSize", "fontFamily", "color", "highlightColor", "pastColor", "nextColor", "bgColor", "fill", "syncOffsetSec", "leadSec", "tailSec", "lineGapSec", "fadeSec", "textShadow", "padding", "radius", "y"};
        for (const char* key : passKeys) if (opts.contains(key)) src[key] = opts[key];
        src["type"] = "template"; src["kind"] = "karaoke";
        src["mode"] = opts.value("mode").toString(src.value("mode").toString(k.mode.isEmpty() ? "word" : k.mode));
        src["segments"] = segs; src["inPointSec"] = inPoint; src["mediaId"] = mediaId; src["language"] = tr.value("language").toString();
        QJsonObject cmd; cmd["action"] = "show"; cmd["layer"] = layer; cmd["role"] = role; cmd["source"] = src;
        QJsonObject tr0 = opts.value("transition").toObject(); if (tr0.isEmpty()) { tr0["type"] = "fade"; tr0["durationSec"] = 0.3; }
        cmd["in"] = tr0; cmd["out"] = tr0;
        onUiCommand(ch, "cg", cmd, "rest");
        k.layer = layer; k.role = role; k.shown = true; k.shownAssetId = mediaId; k.lastKey = oa.key();
        qInfo() << "[Captions] karaoke on" << ch << "layer" << layer << "media" << mediaId << nWords << "words, in point" << inPoint;
        QJsonObject o; o["channelId"] = ch; o["mediaId"] = mediaId; o["layer"] = layer; o["role"] = role; o["mode"] = src["mode"]; o["words"] = nWords; o["segments"] = segs.size();
        o["inPointSec"] = inPoint; o["follow"] = k.follow; o["shown"] = true; o["hasTranscript"] = true;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, o, "cgKaraoke");
        return o;
    };
    std::function<void(const QString&, const QString&)> karaokeHide = [this](const QString& ch, const QString& reason) {
        KaraokeFollow& k = g_karaoke[ch];
        QJsonObject cmd; cmd["action"] = "hide"; cmd["layer"] = k.layer > 0 ? k.layer : 31;
        QJsonObject tr; tr["type"] = "fade"; tr["durationSec"] = 0.3; cmd["transition"] = tr;
        onUiCommand(ch, "cg", cmd, "rest");
        k.shown = false; k.shownAssetId.clear();
        qInfo() << "[Captions] karaoke off" << ch << "-" << reason;
        QJsonObject o; o["channelId"] = ch; o["shown"] = false; o["follow"] = k.follow; o["reason"] = reason;
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, o, "cgKaraoke");
    };

    // Decide what the caption layer should do for the item on air: show (follow mode, or the item asks in
    // cgOnStart / cgOverlays), or take it off (the item carries no captions, no transcript, program off
    // air). Called by the 1 s poller (telemetry) and, sooner, by the as-run START handler below.
    //  * PAUSED counts as on air: pausing freezes the captions with the picture (the compositor holds
    //    their position) instead of hiding them for good.
    //  * "off air" has the empty key, so coming back on air (even with the same item) re-issues.
    //  * A failed show (no transcript text, e.g. music) takes the PREVIOUS item's captions off the air:
    //    the layer would otherwise keep running on the new item's position.
    std::function<void(const QString&, const OnAir&)> evaluate = [this, karaokeShow, karaokeHide, runlistItemOf](const QString& ch, const OnAir& a) {
        KaraokeFollow& k = g_karaoke[ch];
        const bool live = (a.state == "ON_AIR" || a.state == "PAUSED") && !a.asset.isEmpty();
        const QString key = live ? a.key() : QString();
        if (!a.force && key == k.lastKey) return;                  // nothing changed
        k.lastKey = key;
        QJsonObject itemOpts;
        const RunlistItem* ri = live ? runlistItemOf(ch, a) : nullptr;
        const bool itemAsks = ri && itemWantsKaraoke(ri->toEngineJson(), &itemOpts);
        if (live && (k.follow || itemAsks)) {
            const QJsonObject r = karaokeShow(ch, a.asset, itemOpts, a);
            if (r.contains("status")) {
                if (k.shown) karaokeHide(ch, r.value("error").toString());
                else qInfo() << "[Captions]" << ch << "-" << r.value("error").toString() << "- captions stay off";
            }
        } else if (k.shown) {
            karaokeHide(ch, live ? "new item does not carry captions" : "program off air");
        }
    };

    // Follow poller (1 s): the telemetry handler in supervisorcore.cpp is not ours to edit, so the on-air
    // item is compared here.
    QTimer* karaokeTimer = new QTimer(this);
    karaokeTimer->setInterval(1000);
    connect(karaokeTimer, &QTimer::timeout, this, [this, evaluate, onAirOf]() {
        if (!m_transcribe || !m_exec) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_telemetry.begin(); it != m_telemetry.end(); ++it) {
            const QString ch = it.key();
            const OnAir a = onAirOf(ch);
            const KaraokeFollow& k = g_karaoke[ch];
            // A START event announced the new item up to a second before telemetry shows it: telemetry that
            // still names the previous item is stale, do not flip the captions back to it.
            if (!k.eventItemId.isEmpty() && a.itemId != k.eventItemId && now - k.eventAtMs < 3000) continue;
            evaluate(ch, a);
        }
    });
    karaokeTimer->start();
    // Event-driven path: the engine cuts program-time layers on the frame of every take (Compositor::programTake),
    // so the captions for the new item are re-issued the moment its as-run START arrives, not 1-2 s later.
    // START also fires again after an operator seek (same item, new in point): force a re-issue.
    connect(m_exec.get(), &ScheduleExecutor::asRunStored, this, [this, evaluate](const AsRunRow& row) {
        if (row.eventType != "START" || !m_transcribe) return;
        KaraokeFollow& k = g_karaoke[row.channelId];
        k.eventItemId = row.runlistItemId; k.eventAtMs = QDateTime::currentMSecsSinceEpoch();
        OnAir a; a.asset = row.assetId; a.itemId = row.runlistItemId; a.state = "ON_AIR"; a.force = true;
        if (row.extra.contains("inPoint")) a.inPoint = row.extra.value("inPoint").toDouble(-1.0);     // engine's own in point (PlayoutEngine::takePrepared)
        evaluate(row.channelId, a);
    });

    r->registerRoute("GET", "/api/v1/channels/{id}/cg/karaoke", [this, onAirOf, runlistItemOf](const QJsonObject&, const Q& q) {
        const QString ch = q["id"];
        const KaraokeFollow& k = g_karaoke[ch];
        const QJsonObject tele = m_telemetry.value(ch);
        const OnAir a = onAirOf(ch);
        bool found = false; QJsonObject tr; if (m_transcribe && !a.asset.isEmpty()) tr = m_transcribe->transcript(a.asset, &found);
        const RunlistItem* ri = runlistItemOf(ch, a);
        QJsonObject itemOpts;
        QJsonObject o = karaokeFollowJson(k);
        o["channelId"] = ch; o["shown"] = k.shown; o["shownAssetId"] = k.shownAssetId; o["onAirAssetId"] = a.asset; o["onAirTitle"] = tele.value("title").toString();
        o["state"] = tele.value("state").toString("OFFLINE"); o["hasTranscript"] = found && transcriptHasText(tr); o["itemAsks"] = ri && itemWantsKaraoke(ri->toEngineJson(), &itemOpts);
        return o;
    });
    // body: {mediaId?, layer?: 31, role?: "lower_third", mode?: word|line, style?: {...}, follow?: bool,
    //        action?: show|hide, position?, color?, highlightColor?, pastColor?, bgColor?, fontSize?, syncOffsetSec?...}
    r->registerRoute("POST", "/api/v1/channels/{id}/cg/karaoke", [this, karaokeShow, karaokeHide, onAirOf](const QJsonObject& body, const Q& q) {
        const QString ch = q["id"];
        KaraokeFollow& k = g_karaoke[ch];
        bool persist = false;
        if (body.contains("follow")) { k.follow = body.value("follow").toBool(); persist = true; }
        if (body.contains("layer")) { k.layer = body.value("layer").toInt(31); persist = true; }
        if (body.contains("role")) { k.role = body.value("role").toString("lower_third"); persist = true; }
        if (body.contains("mode")) { k.mode = body.value("mode").toString("word"); persist = true; }
        if (body.contains("style")) { const QJsonObject st = body.value("style").toObject(); for (auto it = st.begin(); it != st.end(); ++it) k.style[it.key()] = it.value(); persist = true; }
        static const char* styleKeys[] = {"position", "align", "maxLines", "maxChars", "fontSize", "fontFamily", "color", "highlightColor", "pastColor", "nextColor", "bgColor", "fill", "syncOffsetSec", "leadSec", "tailSec", "fadeSec", "textShadow"};
        for (const char* key : styleKeys) if (body.contains(key)) { k.style[key] = body[key]; persist = true; }
        if (persist) m_store->setSettingJson("cg.karaokeFollow." + ch, karaokeFollowJson(k));
        const QString action = body.value("action").toString("show");
        if (action == "hide" || action == "stop") {
            karaokeHide(ch, "operator");
            QJsonObject o = karaokeFollowJson(k); o["channelId"] = ch; o["shown"] = false; return o;
        }
        QJsonObject res = karaokeShow(ch, body.value("mediaId").toString(), body, onAirOf(ch));
        if (res.contains("status") && k.follow) {
            // Follow mode armed but nothing to show yet (no transcript / off air): not an error.
            QJsonObject o = karaokeFollowJson(k); o["channelId"] = ch; o["shown"] = k.shown; o["warning"] = res.value("error"); o["hasTranscript"] = false;
            broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, ch, o, "cgKaraoke");
            return o;
        }
        return res;
    });

    // --- captions: fact box ------------------------------------------------------------------
    // body = factbox spec (title, lines[] | items[], position, width, accentColor, bgColor, color, fontSize,
    //        icon, image, footer, reveal, revealIntervalSec) + {layer?: 32, role?: "factbox", action?: show|update|hide, transition?, holdSec?}
    r->registerRoute("POST", "/api/v1/channels/{id}/cg/factbox", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["id"];
        const int layer = body.value("layer").toInt(32);
        const QString action = body.value("action").toString("show");
        QJsonObject tr = body.value("transition").toObject(); if (tr.isEmpty()) { tr["type"] = action == "show" ? "slide_up" : "fade"; tr["durationSec"] = 0.4; }
        QJsonObject cmd; cmd["layer"] = layer; cmd["role"] = body.value("role").toString("factbox");
        if (action == "hide" || action == "stop") {
            cmd["action"] = "hide"; cmd["transition"] = tr;
            onUiCommand(ch, "cg", cmd, "rest");
            QJsonObject o; o["channelId"] = ch; o["layer"] = layer; o["shown"] = false; return o;
        }
        static const QSet<QString> layerKeys = {"layer", "role", "action", "transition", "in", "out", "holdSec", "placement", "opacity", "x", "y", "anchor", "marginX", "marginY", "presetId"};
        QJsonObject src; src["type"] = "template"; src["kind"] = "factbox";
        for (auto it = body.begin(); it != body.end(); ++it) if (!layerKeys.contains(it.key())) src[it.key()] = it.value();
        if (src.value("lines").isString()) { QJsonArray a; for (const QString& l : src.value("lines").toString().split('\n', Qt::SkipEmptyParts)) a.append(l.trimmed()); src["lines"] = a; }
        if (src.value("title").toString().isEmpty() && src.value("lines").toArray().isEmpty() && src.value("items").toArray().isEmpty()) return err(400, "title, lines or items required");
        if (action == "update") {
            cmd["action"] = "update"; cmd["source"] = src;
            if (body.contains("placement")) cmd["placement"] = body["placement"];
            onUiCommand(ch, "cg", cmd, "rest");
            QJsonObject o; o["channelId"] = ch; o["layer"] = layer; o["updated"] = true; o["source"] = src; return o;
        }
        bool f = false; const ChannelProfile p = m_store->channel(ch, &f);
        const int rw = f && p.width > 0 ? p.width : 1920, rh = f && p.height > 0 ? p.height : 1080;
        cmd["action"] = "show"; cmd["source"] = src;
        cmd["placement"] = body.contains("placement") ? body["placement"] : QJsonValue(factboxPlacement(body.value("position").toString(), rw, rh));
        cmd["in"] = body.value("in").toObject(tr);
        QJsonObject out = body.value("out").toObject(); if (out.isEmpty()) { out["type"] = "fade"; out["durationSec"] = 0.4; } cmd["out"] = out;
        if (body.value("holdSec").toDouble() > 0.0) cmd["holdSec"] = body["holdSec"];
        onUiCommand(ch, "cg", cmd, "rest");
        QJsonObject o; o["channelId"] = ch; o["layer"] = layer; o["shown"] = true; o["command"] = cmd; return o;
    });
    // Presets: settings key cg.factboxPresets = {presets:[{id, name, spec}]}
    r->registerRoute("GET", "/api/v1/cg/factbox/presets", [this](const QJsonObject&, const Q&) {
        QJsonObject o = m_store->settingJson("cg.factboxPresets"); if (!o.contains("presets")) o["presets"] = QJsonArray(); return o;
    });
    r->registerRoute("PUT", "/api/v1/cg/factbox/presets", [this](const QJsonObject& body, const Q&) {
        // Either the whole list {presets:[...]} or one preset {id?, name, spec} to upsert, or {deleteId}.
        QJsonObject cur = m_store->settingJson("cg.factboxPresets");
        QJsonArray presets = cur.value("presets").toArray();
        if (body.contains("presets")) presets = body.value("presets").toArray();
        else if (body.contains("deleteId")) { QJsonArray keep; for (const QJsonValue& v : presets) if (v.toObject().value("id").toString() != body.value("deleteId").toString()) keep.append(v); presets = keep; }
        else if (body.contains("spec") || body.contains("name")) {
            QJsonObject pr; pr["name"] = body.value("name").toString("Fact box"); pr["spec"] = body.value("spec").toObject();
            pr["id"] = body.value("id").toString(QString::number(QDateTime::currentMSecsSinceEpoch(), 36));
            QJsonArray next; bool replaced = false;
            for (const QJsonValue& v : presets) { if (v.toObject().value("id").toString() == pr["id"].toString()) { next.append(pr); replaced = true; } else next.append(v); }
            if (!replaced) next.append(pr);
            presets = next;
        } else return err(400, "presets[], {name, spec} or {deleteId} required");
        QJsonObject o; o["presets"] = presets; o["updatedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        m_store->setSettingJson("cg.factboxPresets", o);
        broadcastUi(IpcProtocol::MSG_UI_SCHEDULE, QString(), o, "factboxPresets");
        return o;
    });
}
