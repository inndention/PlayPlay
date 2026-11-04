// libobs live-input layer, Supervisor side (docs/libobs.md, docs/libobs-spike.md): the lane-extension module that mounts
// /api/v1/libobs/* and /api/v1/channels/{id}/libobs/* on the REST server. It is compiled into PlayPlaySupervisor from
// cmake/lane-libobs.cmake UNCONDITIONALLY, so a Supervisor built without PLAYPLAY_WITH_LIBOBS still answers the catalog,
// keeps the saved scenes and forwards the commands; an engine without the source then refuses the layer with
// "unknown source type" in its own log, which GET .../libobs/status reports as "not on the engine".
//
// Delivery: the laneext context has no ChannelManager by design, so a channel command goes the way every other lane module
// talks to an engine: this Supervisor's own REST over loopback (POST /api/v1/channels/{id}/cg, the layer command path the
// On Air switcher and the CG Designer use) and GET /api/v1/channels/{id}/state for the 1 Hz telemetry. The routes that do
// that are async (registerAsync) so the event loop is never blocked. In a test fixture with restPort 0 they answer 503.
//
// Scene store: settings key "libobs.scenes" = {"scenes":[{id, name, spec, updatedMs}]}. Every write is validated against the
// catalog below (the same item / filter / transition ids the engine's modules register), so a bad spec is refused here with a
// message instead of failing in the engine's loader thread.
#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <functional>

namespace libobsroutes {

namespace {
QJsonObject err(int status, const QString& msg) { QJsonObject o; o["error"] = msg; o["status"] = status; return o; }
QJsonObject param(const char* key, const char* type, const QJsonValue& def, const char* hint, const QJsonArray& values = QJsonArray()) {
    QJsonObject p; p["key"] = key; p["type"] = type; if (!def.isUndefined()) p["default"] = def; p["hint"] = hint; if (!values.isEmpty()) p["values"] = values; return p;
}
QJsonObject kind(const char* id, const char* name, const char* module, const QJsonArray& params, const char* note = "") {
    QJsonObject k; k["id"] = id; k["name"] = name; k["module"] = module; k["params"] = params; if (*note) k["note"] = note; return k;
}
}

/// The catalog: what the engine's libobs modules register (docs/libobs-spike.md §3.1: image-source, obs-ffmpeg, text-freetype2,
/// obs-filters, obs-transitions, test-input). Built once; the UI editors render their forms from it.
const QJsonObject& catalog() {
    static const QJsonObject c = []() {
        QJsonObject o;
        o["version"] = 1;
        o["layerDefault"] = 50;
        o["onePerProcess"] = true;
        o["fit"] = QJsonArray{"stretch", "inner", "outer", "none"};
        QJsonArray items;
        items << kind("color_source_v3", "Colour", "image-source", {param("color", "color", "#000000", "#rrggbb or #aarrggbb"), param("width", "int", QJsonValue(), "defaults to the raster"), param("height", "int", QJsonValue(), "defaults to the raster")})
              << kind("image_source", "Image", "image-source", {param("file", "path", "", "png / jpg / gif / webp"), param("unload", "bool", false, "unload when hidden")})
              << kind("slideshow", "Slideshow", "image-source", {param("files", "paths", QJsonArray(), "list of image files"), param("slide_time", "int", 8000, "ms per slide"), param("transition", "enum", "fade", "", {"cut", "fade", "swipe", "slide"}), param("loop", "bool", true, "")})
              << kind("ffmpeg_source", "Media file", "obs-ffmpeg", {param("local_file", "path", "", "mp4 / mov / mkv …"), param("looping", "bool", true, ""), param("is_local_file", "bool", true, "false: use `input` (a URL)"), param("input", "url", "", "when is_local_file is false"), param("hw_decode", "bool", false, "")}, "decodes with libobs' own FFmpeg (avcodec 62), not PlayPlay's")
              << kind("text_ft2_source_v2", "Text", "text-freetype2", {param("text", "text", "LIVE", ""), param("font", "font", QJsonObject{{"face", "Arial"}, {"size", 72}}, "{face, size, style, flags}"), param("color1", "color", "#ffffffff", "top colour"), param("color2", "color", "#ffffffff", "bottom colour"), param("outline", "bool", false, ""), param("drop_shadow", "bool", false, "")})
              << kind("random", "Noise (test)", "test-input", {}, "full-raster random pixels; a CPU-heavy test pattern")
              << kind("test_sinewave", "Sine wave (test)", "test-input", {}, "audio only: 440 Hz test tone");
        o["items"] = items;
        QJsonArray filters;
        filters << kind("color_filter_v2", "Colour correction", "obs-filters", {param("opacity", "float", 1.0, "0..1"), param("gamma", "float", 0.0, "-3..3"), param("contrast", "float", 0.0, "-4..4"), param("brightness", "float", 0.0, "-1..1"), param("saturation", "float", 0.0, "-1..5"), param("hue_shift", "float", 0.0, "-180..180")})
                << kind("chroma_key_filter_v2", "Chroma key", "obs-filters", {param("key_color_type", "enum", "green", "", {"green", "blue", "magenta", "custom"}), param("key_color", "color", "#00ff00", "when custom"), param("similarity", "int", 400, "1..1000"), param("smoothness", "int", 80, "1..1000"), param("spill", "int", 100, "1..1000"), param("opacity", "float", 1.0, "0..1")})
                << kind("color_key_filter_v2", "Colour key", "obs-filters", {param("key_color_type", "enum", "green", "", {"green", "blue", "magenta", "custom"}), param("key_color", "color", "#00ff00", ""), param("similarity", "int", 80, ""), param("smoothness", "int", 50, "")})
                << kind("scale_filter", "Scale", "obs-filters", {param("resolution", "string", "1920x1080", "WxH"), param("sampling", "enum", "bicubic", "", {"point", "bilinear", "bicubic", "lanczos"})})
                << kind("sharpness_filter_v2", "Sharpen", "obs-filters", {param("sharpness", "float", 0.08, "0..1")})
                << kind("gain_filter", "Audio gain", "obs-filters", {param("db", "float", 0.0, "-30..30 dB")})
                << kind("crop_filter", "Crop", "obs-filters", {param("left", "int", 0, ""), param("top", "int", 0, ""), param("right", "int", 0, ""), param("bottom", "int", 0, "")});
        o["filters"] = filters;
        QJsonArray tr;
        tr << kind("cut_transition", "Cut", "obs-transitions", {})
           << kind("fade_transition", "Fade", "obs-transitions", {param("durationMs", "int", 500, "")})
           << kind("fade_to_color_transition", "Fade to colour", "obs-transitions", {param("durationMs", "int", 600, ""), param("color", "color", "#000000", ""), param("switch_point", "int", 50, "% of the duration")})
           << kind("swipe_transition", "Swipe", "obs-transitions", {param("durationMs", "int", 500, ""), param("direction", "enum", "left", "", {"left", "right", "up", "down"}), param("swipe_in", "bool", false, "")})
           << kind("slide_transition", "Slide", "obs-transitions", {param("durationMs", "int", 500, ""), param("direction", "enum", "left", "", {"left", "right", "up", "down"})})
           << kind("wipe_transition", "Wipe", "obs-transitions", {param("durationMs", "int", 500, ""), param("luma_image", "string", "linear-h.png", "a luma wipe from obs-transitions' data"), param("luma_invert", "bool", false, ""), param("luma_softness", "float", 0.03, "")})
           << kind("obs_stinger_transition", "Stinger", "obs-transitions", {param("durationMs", "int", 800, "ignored when tp_type is time"), param("path", "path", "", "the stinger clip (alpha mov / webm)"), param("transition_point", "int", 400, "ms (or frames)"), param("tp_type", "enum", 0, "0 = time (ms), 1 = frames", {0, 1})}, "the clip is loaded on the source's own thread, never on the clock thread");
        o["transitions"] = tr;
        return o;
    }();
    return c;
}

namespace {
QSet<QString> idsOf(const char* key) { QSet<QString> s; for (const QJsonValue& v : catalog().value(key).toArray()) s.insert(v.toObject().value("id").toString()); return s; }
const QSet<QString>& itemIds() { static const QSet<QString> s = idsOf("items"); return s; }
const QSet<QString>& filterIds() { static const QSet<QString> s = idsOf("filters"); return s; }
const QSet<QString>& transitionIds() { static const QSet<QString> s = idsOf("transitions"); return s; }
const QRegularExpression& sceneIdRe() { static const QRegularExpression re("^[A-Za-z0-9_-]{1,48}$"); return re; }

/// A transition given as "fade_transition" or {id, durationMs, settings}; empty id = "cut". Normalised to the object form.
bool normTransition(const QJsonValue& v, QJsonObject* out, QString* why) {
    QJsonObject t;
    if (v.isString()) t["id"] = v.toString(); else if (v.isObject()) t = v.toObject(); else if (!v.isUndefined() && !v.isNull()) { *why = "transition must be a string or an object"; return false; }
    if (t.value("id").toString().isEmpty()) t["id"] = "cut_transition";
    if (!transitionIds().contains(t.value("id").toString())) { *why = "unknown transition '" + t.value("id").toString() + "'"; return false; }
    const double d = t.value("durationMs").toDouble(t.value("id").toString() == "cut_transition" ? 0 : 500);
    if (d < 0 || d > 60000) { *why = "transition durationMs out of range (0..60000)"; return false; }
    t["durationMs"] = int(d);
    if (t.contains("settings") && !t.value("settings").isObject()) { *why = "transition settings must be an object"; return false; }
    *out = t;
    return true;
}
}

/// Checks a scene spec the way LibObsSource::buildScenes will read it and fills in the normalised form (type "libobs", transition
/// as an object, program defaulting to the first scene). Empty return = valid; otherwise the first problem.
QString validateSpec(const QJsonObject& in, QJsonObject* normalized) {
    QJsonObject spec = in;
    const QString type = spec.value("type").toString("libobs");
    if (type != "libobs") return "spec type must be \"libobs\"";
    spec["type"] = "libobs";
    const QJsonArray scenes = spec.value("scenes").toArray();
    if (scenes.isEmpty()) return "spec needs at least one scene";
    if (scenes.size() > 32) return "at most 32 scenes";
    QSet<QString> names; QJsonArray outScenes;
    for (const QJsonValue& sv : scenes) {
        if (!sv.isObject()) return "every scene must be an object";
        QJsonObject so = sv.toObject();
        const QString name = so.value("name").toString(QString("scene%1").arg(outScenes.size() + 1)).trimmed();
        if (name.isEmpty() || name.size() > 48) return "scene name must be 1..48 characters";
        if (names.contains(name)) return "duplicate scene name '" + name + "'";
        names.insert(name); so["name"] = name;
        if (so.contains("items") && !so.value("items").isArray()) return "scene '" + name + "': items must be an array";
        QJsonArray outItems; QSet<QString> itemNames;
        for (const QJsonValue& iv : so.value("items").toArray()) {
            if (!iv.isObject()) return "scene '" + name + "': every item must be an object";
            QJsonObject io = iv.toObject();
            const QString id = io.value("id").toString();
            if (id.isEmpty()) return "scene '" + name + "': item without id";
            if (!itemIds().contains(id)) return "scene '" + name + "': unknown item kind '" + id + "' (see GET /api/v1/libobs/catalog)";
            const QString iname = io.value("name").toString(QString("%1_%2").arg(id).arg(outItems.size() + 1));
            if (itemNames.contains(iname)) return "scene '" + name + "': duplicate item name '" + iname + "'";
            itemNames.insert(iname); io["name"] = iname;
            if (io.contains("settings") && !io.value("settings").isObject()) return "item '" + iname + "': settings must be an object";
            if (io.contains("fit") && !catalog().value("fit").toArray().contains(io.value("fit").toString())) return "item '" + iname + "': fit must be stretch, inner, outer or none";
            for (const char* k : {"pos", "size"}) if (io.contains(k) && !io.value(k).isObject()) return QString("item '%1': %2 must be {x,y} / {width,height}").arg(iname, k);
            if (io.contains("filters") && !io.value("filters").isArray()) return "item '" + iname + "': filters must be an array";
            for (const QJsonValue& fv : io.value("filters").toArray()) {
                const QString fid = fv.toObject().value("id").toString();
                if (!filterIds().contains(fid)) return "item '" + iname + "': unknown filter kind '" + fid + "'";
                if (fv.toObject().contains("settings") && !fv.toObject().value("settings").isObject()) return "item '" + iname + "': filter settings must be an object";
            }
            outItems.append(io);
        }
        so["items"] = outItems;
        outScenes.append(so);
    }
    spec["scenes"] = outScenes;
    const QString program = spec.value("program").toString(outScenes.first().toObject().value("name").toString());
    if (!names.contains(program)) return "program '" + program + "' is not a scene";
    spec["program"] = program;
    QJsonObject tr; QString why;
    if (!normTransition(spec.value("transition"), &tr, &why)) return why;
    spec["transition"] = tr;
    if (spec.contains("audio") && !spec.value("audio").isBool()) return "audio must be a bool";
    if (normalized) *normalized = spec;
    return QString();
}

/// Validates one saved-scene record {id, name, spec}; `id` is generated from the name when missing.
QString validateSaved(const QJsonObject& in, QJsonObject* out) {
    QJsonObject rec = in;
    QString id = rec.value("id").toString().trimmed();
    const QString name = rec.value("name").toString().trimmed();
    if (name.isEmpty() || name.size() > 64) return "scene name must be 1..64 characters";
    if (id.isEmpty()) { id = name.toLower(); id.replace(QRegularExpression("[^a-z0-9_-]+"), "-"); id = id.left(48); if (id.isEmpty()) id = "scene"; }
    if (!sceneIdRe().match(id).hasMatch()) return "scene id must match [A-Za-z0-9_-]{1,48}";
    QJsonObject spec;
    const QString why = validateSpec(rec.value("spec").toObject(), &spec);
    if (!why.isEmpty()) return "scene '" + name + "': " + why;
    rec["id"] = id; rec["name"] = name; rec["spec"] = spec;
    if (rec.contains("layer")) { const int l = rec.value("layer").toInt(); if (l < 1 || l > 99) return "layer must be 1..99"; rec["layer"] = l; }
    if (out) *out = rec;
    return QString();
}

/// Validates a whole list for PUT; unique ids.
QString validateList(const QJsonArray& in, QJsonArray* out) {
    QJsonArray clean; QSet<QString> ids;
    for (const QJsonValue& v : in) {
        QJsonObject rec; const QString why = validateSaved(v.toObject(), &rec);
        if (!why.isEmpty()) return why;
        if (ids.contains(rec.value("id").toString())) return "duplicate scene id '" + rec.value("id").toString() + "'";
        ids.insert(rec.value("id").toString());
        clean.append(rec);
    }
    if (out) *out = clean;
    return QString();
}

/// The engine command for "show": the existing CG layer command with the spec as the layer's source.
QJsonObject showCommand(const QJsonObject& spec, int layer, const QJsonObject& opts) {
    QJsonObject cmd; cmd["action"] = "show"; cmd["layer"] = layer; cmd["role"] = "libobs"; cmd["source"] = spec;
    QJsonObject in; in["type"] = opts.value("in").toString("cut"); if (opts.contains("inSec")) in["durationSec"] = opts.value("inSec").toDouble(); cmd["in"] = in;
    if (opts.contains("audioMute")) cmd["audioMute"] = opts.value("audioMute").toBool();
    if (opts.contains("opacity")) cmd["opacity"] = opts.value("opacity").toDouble(1.0);
    if (opts.value("placement").isObject()) cmd["placement"] = opts.value("placement").toObject();
    return cmd;
}
QJsonObject switchCommand(int layer, const QString& to, const QJsonObject& transition) {
    QJsonObject cmd; cmd["action"] = "update"; cmd["layer"] = layer; cmd["switch"] = to;
    if (!transition.isEmpty()) cmd["transition"] = transition;
    return cmd;
}
QJsonObject hideCommand(int layer, bool remove, double fadeSec) {
    QJsonObject cmd; cmd["action"] = remove ? "remove" : "hide"; cmd["layer"] = layer;
    if (!remove) { QJsonObject t; t["type"] = fadeSec > 0 ? "fade" : "cut"; t["durationSec"] = fadeSec; cmd["transition"] = t; }
    return cmd;
}

/// Picks the libobs layer out of an engine telemetry snapshot (layers[] with source.type == "libobs"); empty when none.
QJsonObject layerFromTelemetry(const QJsonObject& state, int preferLayer) {
    QJsonObject best;
    for (const QJsonValue& v : state.value("layers").toArray()) {
        const QJsonObject L = v.toObject();
        if (L.value("source").toObject().value("type").toString() != "libobs") continue;
        if (best.isEmpty() || L.value("id").toInt() == preferLayer) best = L;
    }
    return best;
}

namespace {

/// The module instance: scene store access, the per-channel memory of what was shown, and the loopback client.
class Module : public QObject {
public:
    explicit Module(const laneext::Context& ctx) : QObject(ctx.parent), m_ctx(ctx) {}

    QJsonArray scenes() const { return m_ctx.store ? m_ctx.store->settingJson("libobs.scenes").value("scenes").toArray() : QJsonArray(); }
    bool saveScenes(const QJsonArray& a) { QJsonObject o; o["scenes"] = a; o["updatedMs"] = double(QDateTime::currentMSecsSinceEpoch()); return m_ctx.store && m_ctx.store->setSettingJson("libobs.scenes", o); }
    QJsonObject sceneById(const QString& id) const { for (const QJsonValue& v : scenes()) if (v.toObject().value("id").toString() == id) return v.toObject(); return QJsonObject(); }

    /// One call to this Supervisor's own REST. The reply lands on the event loop; restPort 0 (tests) answers 503 at once.
    void self(const QString& method, const QString& path, const QJsonObject& body, std::function<void(int, QJsonObject)> cb) {
        if (m_ctx.restPort <= 0) { cb(503, err(503, "no loopback REST port: channel commands are unavailable")); return; }
        if (!m_nam) m_nam = new QNetworkAccessManager(this);
        QNetworkRequest req(QUrl(QString("http://127.0.0.1:%1/api/v1%2").arg(m_ctx.restPort).arg(path)));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setTransferTimeout(8000);
        const QString tok = m_ctx.restToken ? m_ctx.restToken() : QString();
        if (!tok.isEmpty()) req.setRawHeader("Authorization", ("Bearer " + tok).toUtf8());
        QNetworkReply* r = method == "GET" ? m_nam->get(req) : method == "PUT" ? m_nam->put(req, QJsonDocument(body).toJson(QJsonDocument::Compact)) : m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(r, &QNetworkReply::finished, this, [r, cb]() {
            r->deleteLater();
            const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QJsonObject b = QJsonDocument::fromJson(r->readAll()).object();
            if (st == 0) { cb(503, err(503, "loopback REST call failed: " + r->errorString())); return; }
            cb(st, b);
        });
    }

    struct Shown { int layer = 50; QString sceneId, name, program; qint64 shownMs = 0; QJsonObject transition; };
    QHash<QString, Shown> shown;      // channelId -> last show (the Supervisor's memory; the engine's telemetry is the truth)
    laneext::Context m_ctx;
    QNetworkAccessManager* m_nam = nullptr;
};

QJsonObject shownJson(const Module::Shown& s) {
    QJsonObject o; o["layer"] = s.layer; o["sceneId"] = s.sceneId; o["name"] = s.name; o["program"] = s.program; o["shownMs"] = double(s.shownMs); if (!s.transition.isEmpty()) o["transition"] = s.transition; return o;
}

void install(const laneext::Context& ctx) {
    if (!ctx.rest) { qWarning() << "[libobs] not installed: no REST server"; return; }
    auto* m = new Module(ctx);
    RestApiServer* r = ctx.rest;
    using Q = QHash<QString, QString>;

    r->registerRoute("GET", "/api/v1/libobs/catalog", [](const QJsonObject&, const Q&) { return catalog(); });
    r->registerRoute("GET", "/api/v1/libobs/scenes", [m](const QJsonObject&, const Q&) { QJsonObject o; o["scenes"] = m->scenes(); return o; });
    r->registerRoute("PUT", "/api/v1/libobs/scenes", [m](const QJsonObject& body, const Q&) {
        QJsonArray clean; const QString why = validateList(body.value("scenes").toArray(), &clean);
        if (!why.isEmpty()) return err(422, why);
        if (!m->saveScenes(clean)) return err(500, "could not save libobs.scenes");
        if (m->m_ctx.broadcast) { QJsonObject p; p["scenes"] = clean; m->m_ctx.broadcast("libobsScenes", p); }
        QJsonObject o; o["ok"] = true; o["scenes"] = clean; return o;
    });
    // Upsert one / delete one: what the editors use, so two operators saving different scenes never clobber each other.
    r->registerRoute("POST", "/api/v1/libobs/scenes", [m](const QJsonObject& body, const Q&) {
        QJsonObject rec; const QString why = validateSaved(body, &rec);
        if (!why.isEmpty()) return err(422, why);
        rec["updatedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        QJsonArray all = m->scenes(); bool replaced = false;
        for (int i = 0; i < all.size(); ++i) if (all[i].toObject().value("id").toString() == rec.value("id").toString()) { all[i] = rec; replaced = true; }
        if (!replaced) all.append(rec);
        if (!m->saveScenes(all)) return err(500, "could not save libobs.scenes");
        if (m->m_ctx.broadcast) { QJsonObject p; p["scenes"] = all; m->m_ctx.broadcast("libobsScenes", p); }
        QJsonObject o; o["ok"] = true; o["scene"] = rec; o["replaced"] = replaced; return o;
    });
    r->registerRoute("DELETE", "/api/v1/libobs/scenes/{id}", [m](const QJsonObject&, const Q& q) {
        QJsonArray all = m->scenes(), kept; for (const QJsonValue& v : all) if (v.toObject().value("id").toString() != q["id"]) kept.append(v);
        if (kept.size() == all.size()) return err(404, "no such scene");
        if (!m->saveScenes(kept)) return err(500, "could not save libobs.scenes");
        if (m->m_ctx.broadcast) { QJsonObject p; p["scenes"] = kept; m->m_ctx.broadcast("libobsScenes", p); }
        QJsonObject o; o["deleted"] = q["id"]; return o;
    });
    // Dry run of the command builder: what POST .../show would send, without sending it (the UI's "preview command").
    r->registerRoute("POST", "/api/v1/libobs/validate", [](const QJsonObject& body, const Q&) {
        QJsonObject spec; const QString why = validateSpec(body.value("spec").toObject(body), &spec);
        QJsonObject o; o["ok"] = why.isEmpty(); if (!why.isEmpty()) o["error"] = why; else { o["spec"] = spec; o["command"] = showCommand(spec, body.value("layer").toInt(catalog().value("layerDefault").toInt()), body); }
        return o;
    });

    // --- per channel ---------------------------------------------------------------------------------
    r->registerAsync("POST", "/api/v1/channels/{id}/libobs/show", [m](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const QString ch = q["id"];
        QJsonObject spec, rec; QString sceneId = body.value("sceneId").toString();
        if (!sceneId.isEmpty()) {
            rec = m->sceneById(sceneId);
            if (rec.isEmpty()) { reply(404, err(404, "no saved scene '" + sceneId + "'")); return; }
            spec = rec.value("spec").toObject();
        } else {
            const QString why = validateSpec(body.value("spec").toObject(), &spec);
            if (!why.isEmpty()) { reply(422, err(422, why)); return; }
        }
        const int layer = body.value("layer").toInt(rec.value("layer").toInt(catalog().value("layerDefault").toInt()));
        if (layer < 1 || layer > 99) { reply(422, err(422, "layer must be 1..99")); return; }
        const QJsonObject cmd = showCommand(spec, layer, body);
        m->self("POST", "/channels/" + ch + "/cg", cmd, [m, ch, reply, cmd, layer, sceneId, rec, spec](int st, QJsonObject b) {
            if (st != 200) { reply(st, b.contains("error") ? b : err(st, "cg command refused")); return; }
            Module::Shown s; s.layer = layer; s.sceneId = sceneId; s.name = rec.value("name").toString(); s.program = spec.value("program").toString(); s.shownMs = QDateTime::currentMSecsSinceEpoch(); s.transition = spec.value("transition").toObject();
            m->shown[ch] = s;
            if (m->m_ctx.broadcast) { QJsonObject p = shownJson(s); p["channelId"] = ch; p["event"] = "show"; m->m_ctx.broadcast("libobs", p); }
            QJsonObject o; o["ok"] = true; o["accepted"] = true; o["channelId"] = ch; o["layer"] = layer; o["command"] = cmd; o["scenes"] = spec.value("scenes").toArray().size();
            o["note"] = "accepted, not confirmed: the engine opens the source on its own thread; GET .../libobs/status shows it once it is on";
            reply(200, o);
        });
    });
    r->registerAsync("POST", "/api/v1/channels/{id}/libobs/switch", [m](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const QString ch = q["id"];
        const QString to = body.value("to").toString().trimmed();
        if (to.isEmpty()) { reply(422, err(422, "`to` (scene name) required")); return; }
        QJsonObject tr; QString why;
        if (body.contains("transition") || body.contains("durationMs")) {
            QJsonValue tv = body.value("transition");
            if (tv.isString() || tv.isUndefined()) { QJsonObject t; if (tv.isString()) t["id"] = tv.toString(); if (body.contains("durationMs")) t["durationMs"] = body.value("durationMs"); tv = t; }
            else if (tv.isObject() && body.contains("durationMs")) { QJsonObject t = tv.toObject(); t["durationMs"] = body.value("durationMs"); tv = t; }
            if (!normTransition(tv, &tr, &why)) { reply(422, err(422, why)); return; }
        }
        const int layer = body.value("layer").toInt(m->shown.value(ch).layer);
        const QJsonObject cmd = switchCommand(layer, to, tr);
        m->self("POST", "/channels/" + ch + "/cg", cmd, [m, ch, reply, cmd, to, tr](int st, QJsonObject b) {
            if (st != 200) { reply(st, b.contains("error") ? b : err(st, "cg command refused")); return; }
            if (m->shown.contains(ch)) { m->shown[ch].program = to; if (!tr.isEmpty()) m->shown[ch].transition = tr; }
            if (m->m_ctx.broadcast) { QJsonObject p = shownJson(m->shown.value(ch)); p["channelId"] = ch; p["event"] = "switch"; m->m_ctx.broadcast("libobs", p); }
            QJsonObject o; o["ok"] = true; o["accepted"] = true; o["channelId"] = ch; o["command"] = cmd; reply(200, o);
        });
    });
    r->registerAsync("POST", "/api/v1/channels/{id}/libobs/hide", [m](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const QString ch = q["id"];
        const int layer = body.value("layer").toInt(m->shown.value(ch).layer);
        // Default remove: libobs allows one source per process, so a hidden-but-open layer would block the next show.
        const bool keep = body.value("keep").toBool(false);
        const QJsonObject cmd = hideCommand(layer, !keep, body.value("fadeSec").toDouble(0.3));
        m->self("POST", "/channels/" + ch + "/cg", cmd, [m, ch, reply, cmd, keep](int st, QJsonObject b) {
            if (st != 200) { reply(st, b.contains("error") ? b : err(st, "cg command refused")); return; }
            if (!keep) m->shown.remove(ch);
            if (m->m_ctx.broadcast) { QJsonObject p; p["channelId"] = ch; p["event"] = keep ? "hide" : "remove"; m->m_ctx.broadcast("libobs", p); }
            QJsonObject o; o["ok"] = true; o["accepted"] = true; o["channelId"] = ch; o["command"] = cmd; reply(200, o);
        });
    });
    r->registerAsync("POST", "/api/v1/channels/{id}/libobs/filter", [m](const QJsonObject& body, const Q& q, RestApiServer::Reply reply) {
        const auto filter = body.value("filter").toObject(); const QString fid = filter.value("id").toString();
        if ((fid != "chroma_key_filter_v2" && fid != "color_key_filter_v2") || body.value("item").toString().isEmpty()) { reply(422, err(422, "A source item and chroma/colour key filter are required")); return; }
        const auto settings = filter.value("settings").toObject();
        for (const char* k : {"similarity", "smoothness", "spill"}) if (settings.contains(k) && (!settings.value(k).isDouble() || settings.value(k).toDouble() < 1 || settings.value(k).toDouble() > 1000)) { reply(422, err(422, QString(k) + " must be 1..1000")); return; }
        if (settings.contains("key_color") && !QRegularExpression("^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$").match(settings.value("key_color").toString()).hasMatch()) { reply(422, err(422, "key_color must be #RRGGBB or #RRGGBBAA")); return; }
        if (settings.contains("opacity") && (!settings.value("opacity").isDouble() || settings.value("opacity").toDouble() < 0 || settings.value("opacity").toDouble() > 1)) { reply(422, err(422, "opacity must be 0..1")); return; }
        const QString ch = q["id"]; const int requestedLayer = body.value("layer").toInt();
        m->self("GET", "/channels/" + ch + "/state", {}, [m, ch, body, filter, requestedLayer, reply](int st, QJsonObject state) {
            const auto layer = layerFromTelemetry(state, requestedLayer);
            if (st != 200 || layer.isEmpty()) { reply(409, err(409, "Show the OBS scene set on this channel before applying a live key")); return; }
            QJsonObject source{{"item", body.value("item")}, {"filter", filter}}; if (body.contains("scene")) source["scene"] = body.value("scene");
            QJsonObject cmd{{"action", "update"}, {"layer", layer.value("id")}, {"source", source}};
            m->self("POST", "/channels/" + ch + "/cg", cmd, [reply](int status, QJsonObject response) { response["accepted"] = status == 200; reply(status, response); });
        });
    });
    r->registerAsync("GET", "/api/v1/channels/{id}/libobs/status", [m](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        const QString ch = q["id"];
        m->self("GET", "/channels/" + ch + "/state", QJsonObject(), [m, ch, reply](int st, QJsonObject state) {
            QJsonObject o; o["channelId"] = ch;
            const bool known = m->shown.contains(ch);
            if (known) o["shown"] = shownJson(m->shown.value(ch));
            o["engineState"] = state.value("engineState").toString(st == 200 ? "UNKNOWN" : "UNREACHABLE");
            const QJsonObject L = layerFromTelemetry(state, known ? m->shown.value(ch).layer : 0);
            o["onAir"] = !L.isEmpty() && L.value("state").toString() != "hidden";
            if (!L.isEmpty()) {
                o["layer"] = L.value("id"); o["layerState"] = L.value("state"); o["audible"] = L.value("audible");
                o["obs"] = L.value("sourceStatus").toObject().value("obs").toObject();
                o["sourceText"] = L.value("sourceStatus").toObject().value("text");
                o["connected"] = L.value("sourceStatus").toObject().value("connected");
            } else if (known) {
                o["note"] = (QDateTime::currentMSecsSinceEpoch() - m->shown.value(ch).shownMs) < 5000 ? "show accepted, waiting for the engine to open the source" : "not on the engine: the layer was refused or removed (an engine built without PLAYPLAY_WITH_LIBOBS logs \"unknown source type\")";
            }
            if (st != 200 && st != 404) o["stateError"] = state.value("error").toString();
            reply(200, o);
        });
    });
    qInfo() << "[libobs] routes installed:" << r->routeKeys().filter("/libobs").size();
}

} // namespace

} // namespace libobsroutes

static laneext::Registrar s_libobsRoutes("libobs", 110, [](const laneext::Context& ctx) { libobsroutes::install(ctx); });
