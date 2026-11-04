#include "overlaymanager.h"
#include "cgtext.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"
#include "../core/browserlocator.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimeZone>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QDebug>
#include <algorithm>
#include <cmath>

using namespace sched;
using J = QJsonObject;

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
namespace {

J err(int status, const QString& msg) { J o; o["error"] = msg; o["status"] = status; return o; }
bool isErr(const J& o) { return o.contains("error") && o.contains("status"); }

double num(const QJsonValue& v, double def) {
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) { bool ok = false; const double d = v.toString().trimmed().toDouble(&ok); return ok ? d : def; }
    if (v.isBool()) return v.toBool() ? 1.0 : 0.0;
    return def;
}

QString normRole(QString r) {
    r = r.trimmed().toLower();
    r.replace('-', '_'); r.replace(' ', '_');
    return r;
}

// Query-string value of a template parameter (the pages read them with URLSearchParams).
QString paramString(const QJsonValue& v) {
    if (v.isString()) return v.toString();
    if (v.isBool()) return v.toBool() ? "1" : "0";
    if (v.isDouble()) {
        const double d = v.toDouble();
        return (std::floor(d) == d && std::fabs(d) < 1e15) ? QString::number(qint64(d)) : QString::number(d, 'g', 12);
    }
    if (v.isArray()) return QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact));
    if (v.isObject()) return QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact));
    return QString();
}

// Role bands: engine layer ids an overlay of that role may use, in z order. 10 (program), 15 (secondary), 20 (bug), 30 (lower_third),
// 40 (ticker), 80 (legal_id), 90 (emergency) are the compositor's own role ids (identity graphics, cgOnStart hooks) and 31 / 32 are
// the karaoke / fact box caption layers: none of them is ever handed out. 50 is what the compositor picks for an unknown role.
const QMap<QString, QList<int>>& bandTable() {
    static const QMap<QString, QList<int>> t = {
        {"pillar",      {11}},                                   // pillar panels: the side bars beside a narrow picture, under every other graphic
        {"secondary",   {12, 13, 14, 16, 17, 18, 19}},
        {"bug",         {21, 22, 23, 24, 25, 26, 27, 28, 29}},
        {"lower_third", {33, 34, 35, 36, 37, 38, 39}},
        {"ticker",      {41, 42, 43, 44, 45, 46, 47, 48, 49}},
        {"legal_id",    {81, 82, 83, 84, 85, 86, 87, 88, 89}},
        {"emergency",   {91, 92, 93, 94, 95, 96, 97, 98, 99}},
    };
    return t;
}
int compositorRoleId(const QString& band) {
    static const QHash<QString, int> ids = {{"secondary", 15}, {"bug", 20}, {"lower_third", 30}, {"ticker", 40}, {"legal_id", 80}, {"emergency", 90}};
    return ids.value(band, 0);
}

QStringList listOf(const QJsonValue& v) {
    QStringList out;
    if (v.isString()) { for (const QString& s : v.toString().split(',', Qt::SkipEmptyParts)) if (!s.trimmed().isEmpty()) out << s.trimmed().toLower(); }
    else for (const QJsonValue& x : v.toArray()) { const QString s = x.toVariant().toString().trimmed().toLower(); if (!s.isEmpty()) out << s; }
    return out;
}

bool isImagePath(const QString& p) {
    const QString l = p.toLower();
    for (const char* e : {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".svg", ".tga", ".tif", ".tiff"}) if (l.endsWith(e)) return true;
    return false;
}

// Karaoke cues belong to the caption follower in supervisorcore_integrations.cpp: the cue scheduler leaves them alone.
bool isKaraokeCue(const J& o) {
    if (o.value("kind").toString() == "karaoke") return true;
    if (o.value("templateId").toString() == "karaoke" || o.value("template").toString() == "karaoke") return true;
    if (o.value("source").toObject().value("kind").toString() == "karaoke") return true;
    return o.value("command").toObject().value("source").toObject().value("kind").toString() == "karaoke";
}

// Fact box placement from the pack's `position` parameter (anchor + title-safe 5 % margins).
J factboxPlacement(const QString& position, int rw, int rh) {
    J p; const double mx = rw * 0.05, my = rh * 0.05;
    const QString pos = position.isEmpty() ? "right" : position;
    p["anchor"] = pos == "left" ? "left" : pos == "top-right" || pos == "top_right" ? "top_right" : pos == "top-left" || pos == "top_left" ? "top_left"
                : pos == "bottom-left" || pos == "bottom_left" ? "bottom_left" : pos == "bottom-right" || pos == "bottom_right" ? "bottom_right" : pos == "center" ? "center" : "right";
    p["marginX"] = mx; p["marginY"] = my;
    return p;
}


} // namespace

// ---------------------------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------------------------

QList<int> OverlayManager::bandIds(const QString& role) { return bandTable().value(bandFor(role)); }

QString OverlayManager::bandFor(const QString& role) {
    static const QHash<QString, QString> alias = {
        {"lowerthird", "lower_third"}, {"lower3rd", "lower_third"}, {"lt", "lower_third"}, {"strap", "lower_third"},
        {"logo", "bug"}, {"badge", "bug"}, {"watermark", "bug"}, {"crawl", "ticker"}, {"scroll", "ticker"},
        {"legal", "legal_id"}, {"id", "legal_id"}, {"clock", "legal_id"}, {"alert", "emergency"}};
    const QString r = normRole(role);
    if (bandTable().contains(r)) return r;
    return alias.value(r, "secondary");
}

QJsonObject OverlayManager::bandsJson() {
    J o;
    for (auto it = bandTable().begin(); it != bandTable().end(); ++it) {
        QJsonArray ids; for (int id : it.value()) ids.append(id);
        J b; b["ids"] = ids; b["compositorRoleId"] = compositorRoleId(it.key());
        o[it.key()] = b;
    }
    return o;
}

QJsonValue OverlayManager::expandTokens(const QJsonValue& v, const QHash<QString, QString>& tokens) {
    if (v.isString()) {
        const QString s = v.toString();
        if (!s.contains("{{")) return v;
        static const QRegularExpression re("\\{\\{\\s*([A-Za-z_]+)\\s*\\}\\}");
        QString out; int last = 0;
        QRegularExpressionMatchIterator it = re.globalMatch(s);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += s.mid(last, m.capturedStart() - last);
            const QString key = m.captured(1).toLower();
            out += tokens.contains(key) ? tokens.value(key) : m.captured(0);     // unknown tokens stay as typed
            last = m.capturedEnd();
        }
        out += s.mid(last);
        return out;
    }
    if (v.isArray()) { QJsonArray a; for (const QJsonValue& x : v.toArray()) a.append(expandTokens(x, tokens)); return a; }
    if (v.isObject()) { const J o = v.toObject(); J r; for (auto it = o.begin(); it != o.end(); ++it) r[it.key()] = expandTokens(it.value(), tokens); return r; }
    return v;
}

QJsonObject OverlayManager::resolveAnimation(const QJsonValue& v, const QJsonArray& presets) {
    J a; QString pid;
    if (v.isString()) pid = v.toString();
    else if (v.isObject()) { a = v.toObject(); pid = a.value("preset").toString(); }
    if (!pid.isEmpty()) {
        for (const QJsonValue& pv : presets) {
            const J p = pv.toObject();
            if (p.value("id").toString() != pid) continue;
            for (const char* k : {"in", "out", "duration", "easing"}) if (!a.contains(k) && p.contains(k)) a[k] = p.value(k);
            a["preset"] = pid;
            break;
        }
    }
    return a;
}

QString OverlayManager::urlFor(const QString& file, const QJsonObject& params, const QJsonObject& a) {
    QUrlQuery q;
    for (auto it = params.begin(); it != params.end(); ++it) { const QString s = paramString(it.value()); if (!s.isEmpty()) q.addQueryItem(it.key(), s); }
    const QString in = a.value("in").toString(), out = a.value("out").toString();
    const int inDur = int(num(a.value("inDuration"), num(a.value("duration"), 600)));
    const int outDur = int(num(a.value("outDuration"), num(a.value("duration"), 400)));
    if (!in.isEmpty() && in != "none") { q.addQueryItem("animIn", in); q.addQueryItem("animInDuration", QString::number(inDur)); }
    if (!out.isEmpty() && out != "none") { q.addQueryItem("animOut", out); q.addQueryItem("animOutDuration", QString::number(outDur)); }
    if (!a.value("easing").toString().isEmpty() && a.value("easing").toString() != "ease-out") q.addQueryItem("animEasing", a.value("easing").toString());
    if (num(a.value("delay"), 0) > 0) q.addQueryItem("animDelay", QString::number(int(num(a.value("delay"), 0))));
    if (a.value("loop").toBool()) q.addQueryItem("loop", "1");
    QString qs = q.toString(QUrl::FullyEncoded);
    qs.replace('+', "%2B");                 // URLSearchParams reads a bare '+' as a space; every real '+' must travel as %2B
    return "/cg/" + file + (qs.isEmpty() ? "" : "?" + qs);
}

// Native fallback: the pack template expressed in the engine's own template kinds (TemplateSource). Same mapping as the original
// supervisorcore_integrations.cpp nativeCgFor, with the accent colour under the key the renderer reads (accentColor), margins scaled to
// the channel raster, and a few template-specific twins (fact box, ad slate, countdown timer, candidate / podcast straps).
QJsonObject OverlayManager::nativeCommandFor(const QJsonObject& tpl, const QJsonObject& params, const QString& role, const QJsonObject& anim, int rw, int rh) {
    const QString cat = tpl.value("category").toString(), id = tpl.value("id").toString();
    auto p = [&](const char* k, const QString& def = QString()) { const QString v = paramString(params.value(k)).trimmed(); return v.isEmpty() ? def : v; };
    auto hex = [](const QString& c) { const QString s = c.trimmed(); if (s.isEmpty()) return QString(); return s.startsWith('#') ? s : "#" + s; };
    const double sc = rw > 0 ? rw / 1920.0 : 1.0;
    const double inDur = num(anim.value("inDuration"), num(anim.value("duration"), 600)) / 1000.0;
    const double outDur = num(anim.value("outDuration"), num(anim.value("duration"), 400)) / 1000.0;
    auto tr = [](const QString& a, const QString& def) {
        if (a.startsWith("slide-left") || a == "wipe-left") return QString("slide_left");
        if (a.startsWith("slide-right") || a == "wipe-right") return QString("slide_right");
        if (a == "slide-up") return QString("slide_up");
        if (a == "slide-down") return QString("slide_down");
        if (a == "fade" || a == "zoom" || a == "flip" || a == "bounce" || a == "typewriter") return QString("fade");
        return def;
    };
    J cmd; cmd["action"] = "show"; cmd["role"] = role;
    J src; src["type"] = "template";
    J place;
    QString defIn = "fade";
    if (id == "karaoke") {
        return err(409, "karaoke captions need the transcript of the item on air: use POST /api/v1/channels/{id}/cg/karaoke (they have no template-only native twin)");
    } else if (id == "factbox") {
        src["kind"] = "factbox"; src["title"] = p("title");
        if (!p("icon").isEmpty()) src["icon"] = p("icon");
        QJsonArray lines;
        if (params.value("lines").isArray()) lines = params.value("lines").toArray();
        else for (const QString& l : p("lines").split('|', Qt::SkipEmptyParts)) if (!l.trimmed().isEmpty()) lines.append(l.trimmed());
        QJsonArray items;
        if (params.value("items").isArray()) items = params.value("items").toArray();
        else for (const QString& it : p("items").split('|', Qt::SkipEmptyParts)) {
            const int eq = it.indexOf('=');
            if (eq <= 0) continue;
            J row; row["label"] = it.left(eq).trimmed(); row["value"] = it.mid(eq + 1).trimmed(); items.append(row);
        }
        if (!items.isEmpty()) src["items"] = items; else src["lines"] = lines;
        if (!p("width").isEmpty()) src["width"] = int(p("width").toDouble());
        if (!p("fontSize").isEmpty()) src["fontSize"] = int(p("fontSize").toDouble());
        if (!p("footer").isEmpty()) src["footer"] = p("footer");
        if (!p("image").isEmpty()) src["image"] = p("image");
        if (!p("reveal").isEmpty()) src["reveal"] = p("reveal");
        if (!p("interval").isEmpty()) src["revealIntervalSec"] = p("interval").toDouble();
        if (!p("accent").isEmpty()) src["accentColor"] = hex(p("accent"));
        place = factboxPlacement(p("position"), rw, rh);
        defIn = "slide_up";
    } else if (id == "ad_slate" || id == "countdown_timer" || cat == "countdown") {
        src["kind"] = "countdown"; src["label"] = p("label", p("title"));
        if (id == "countdown_timer" && !p("target").isEmpty()) {
            QDateTime t = QDateTime::fromString(p("target"), Qt::ISODate);
            if (!t.isValid()) t = QDateTime::fromString(p("target"), "yyyy-MM-ddTHH:mm");
            if (t.isValid()) src["targetUtcMs"] = double(t.toMSecsSinceEpoch()); else src["seconds"] = 60.0;
        } else src["seconds"] = p(id == "ad_slate" ? "duration" : "seconds", "60").toDouble();
        place["anchor"] = "center";
    } else if (id == "candidate_lower" || id == "podcast_host") {
        src["kind"] = "lower_third"; src["title"] = p("name", p("title"));
        QStringList sub; for (const char* k : {"title", "party", "state", "show", "role"}) if (!p(k).isEmpty() && p(k) != src["title"].toString()) sub << p(k);
        src["subtitle"] = sub.join("  |  ");
        place["anchor"] = p("position") == "right" ? "bottom_right" : p("position") == "center" ? "bottom" : "bottom_left"; place["marginX"] = 120 * sc; place["marginY"] = 90 * sc;
        defIn = "slide_left";
    } else if (cat == "lower-thirds" || cat == "banners" || cat == "social") {
        src["kind"] = "lower_third"; src["title"] = p("name", p("title", p("text", p("headline", p("handle", tpl.value("name").toString())))));
        src["subtitle"] = params.contains("name") ? p("title") : p("subtitle", p("label", p("details", p("tweet", p("message")))));
        place["anchor"] = p("position") == "right" ? "bottom_right" : p("position") == "center" ? "bottom" : "bottom_left"; place["marginX"] = 120 * sc; place["marginY"] = 90 * sc;
        defIn = "slide_left";
    } else if (cat == "bugs" || cat == "score-bugs" || cat == "weather" || cat == "sports-extended") {
        src["kind"] = "bug"; src["text"] = p("text", p("label", p("name", p("location", p("team1", "LIVE")))));
        place["anchor"] = "top_right"; place["margin"] = 60 * sc;
    } else if (cat == "tickers") {
        src["kind"] = "ticker"; src["text"] = p("text", p("items", "PLAY PLAY")); src["label"] = p("label", "NEWS");
        if (!p("speed").isEmpty()) src["speed"] = p("speed").toDouble();
        place["anchor"] = "bottom";
    } else if (cat == "emergency") {
        src = J(); src["type"] = "bars"; QJsonArray caps; caps.append(p("message", p("text", p("service", "EMERGENCY")))); if (!p("subtitle").isEmpty()) caps.append(p("subtitle")); src["captions"] = caps;
    } else {
        return err(409, QString("template '%1' (category '%2') has no native equivalent: it needs a Chromium-family browser (set settings cg.browserPath or PLAYPLAY_BROWSER)").arg(id, cat));
    }
    const QString accent = p("color1", p("color", p("teamColor", p("accentColor"))));
    if (!accent.isEmpty() && src.value("kind").toString() != "factbox" && src.value("type").toString() == "template") src["accentColor"] = hex(accent);
    cmd["source"] = src; if (!place.isEmpty()) cmd["placement"] = place;
    J in; in["type"] = tr(anim.value("in").toString(), defIn); in["durationSec"] = inDur; cmd["in"] = in;
    J out; out["type"] = tr(anim.value("out").toString(), "fade"); out["durationSec"] = outDur; cmd["out"] = out;
    return cmd;
}

// ---------------------------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------------------------

OverlayManager::OverlayManager(ScheduleStore* store, ScheduleExecutor* exec, QObject* parent) : QObject(parent), m_store(store), m_exec(exec) {
    m_tick.setInterval(100);
    connect(&m_tick, &QTimer::timeout, this, &OverlayManager::tick);
}

OverlayManager::~OverlayManager() { shutdown(); }

void OverlayManager::start() { if (!m_tick.isActive()) m_tick.start(); }

void OverlayManager::shutdown() {
    m_tick.stop();
    m_timed.clear();
    // A test waiting for frames would leave its HTTP socket open: answer it.
    const QList<PendingTest> tests = m_tests; m_tests.clear();
    for (const PendingTest& t : tests) if (t.reply) t.reply(503, err(503, "supervisor shutting down"));
}

qint64 OverlayManager::now() const { return m_hooks.nowMs ? m_hooks.nowMs() : QDateTime::currentMSecsSinceEpoch(); }

QString OverlayManager::settingStr(const QString& key) const { return m_store ? m_store->setting(key).trimmed() : QString(); }

bool OverlayManager::channelExists(const QString& ch, int* rw, int* rh) const {
    if (ch.isEmpty() || !m_store) return false;
    bool f = false; const ChannelProfile p = m_store->channel(ch, &f);
    if (!f) return false;
    if (rw) *rw = p.width > 0 ? p.width : 1920;
    if (rh) *rh = p.height > 0 ? p.height : 1080;
    return true;
}

QJsonObject OverlayManager::manifest() const {
    if (m_hooks.manifest) return m_hooks.manifest();
    J o; o["templates"] = QJsonArray(); return o;
}

QJsonObject OverlayManager::findTemplate(const QString& id) const {
    if (id.isEmpty()) return J();
    for (const QJsonValue& v : manifest().value("templates").toArray()) if (v.toObject().value("id").toString() == id) return v.toObject();
    return J();
}

QJsonObject OverlayManager::browserInfo() {
    const qint64 t = now();
    if (m_browserCache.isEmpty() || t - m_browserCacheMs > 5000 || t < m_browserCacheMs) { m_browserCache = cgbrowser::describe(settingStr("cg.browserPath")); m_browserCacheMs = t; }
    return m_browserCache;
}

bool OverlayManager::browserAvailable() { return browserInfo().value("available").toBool(); }

QString OverlayManager::browserMissingMessage() {
    QStringList tried;
    for (const QJsonValue& v : browserInfo().value("candidates").toArray()) { if (tried.size() >= 6) break; tried << v.toObject().value("path").toString(); }
    return "no Chromium-family browser found (set settings cg.browserPath or PLAYPLAY_BROWSER); tried: " + tried.join("; ");
}

QHash<QString, QString> OverlayManager::engineEnvironment() const {
    QHash<QString, QString> env;
    const QString b = settingStr("cg.browserPath"); if (!b.isEmpty()) env["PLAYPLAY_BROWSER"] = QDir::toNativeSeparators(b);
    const QString n = settingStr("cg.htmlNthFrame"); if (!n.isEmpty()) env["PLAYPLAY_HTML_NTH_FRAME"] = n;
    const QString f = settingStr("cg.htmlFirstFrameMs"); if (!f.isEmpty()) env["PLAYPLAY_HTML_FIRST_FRAME_MS"] = f;
    return env;
}

// ---------------------------------------------------------------------------------------------
// Layers: allocation, records, deploy
// ---------------------------------------------------------------------------------------------
namespace {
QString newUid() { return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8); }

J mergeDefaults(const J& tpl, const J& params) {
    J m;
    for (const QJsonValue& pv : tpl.value("params").toArray()) {
        const J p = pv.toObject(); const QString d = paramString(p.value("default"));
        if (!d.isEmpty()) m[p.value("name").toString()] = p.value("default");
    }
    for (auto it = params.begin(); it != params.end(); ++it) m[it.key()] = it.value();
    return m;
}

bool matchesEngine(const QString& mode, const QString& liveUrl, const J& e) {
    const J src = e.value("source").toObject(); const QString t = src.value("type").toString();
    if (mode == "html") { if (t != "html") return false; const QString u = src.value("url").toString(); return u.isEmpty() || u == liveUrl; }
    return t != "html";
}
} // namespace

int OverlayManager::htmlFirstFrameMs() const { const int v = settingStr("cg.htmlFirstFrameMs").toInt(); return v > 0 ? v : 8000; }

bool OverlayManager::templateFileOk(const QJsonObject& tpl, QString* why) {
    if (m_hooks.webRoot.isEmpty()) { if (why) *why = "no web root configured (the Supervisor serves /cg/*.html from it)"; return false; }
    const QString f = QDir(m_hooks.webRoot).filePath("cg/" + tpl.value("file").toString());
    if (!QFileInfo(f).isFile()) { if (why) *why = "template file not found: " + QDir::toNativeSeparators(f); return false; }
    return true;
}

int OverlayManager::allocLayer(ChannelState& c, const QString& band, int wantSlot, int wantLayer, QString* error) const {
    const QList<int> ids = bandTable().value(band);
    if (wantLayer >= 0) {
        for (auto it = bandTable().begin(); it != bandTable().end(); ++it) if (it.value().contains(wantLayer)) return wantLayer;
        if (error) *error = QString("layer %1 is not an overlay layer (see the layer table in docs/overlays.md)").arg(wantLayer);
        return -1;
    }
    if (wantSlot >= 0) {
        if (wantSlot >= ids.size()) { if (error) *error = QString("slot %1 out of range for band '%2' (0..%3)").arg(wantSlot).arg(band).arg(ids.size() - 1); return -1; }
        return ids.at(wantSlot);
    }
    const qint64 t = now();
    for (int pass = 0; pass < 2; ++pass)
        for (int id : ids) if (!c.layers.contains(id) && (pass == 1 || c.tombstones.value(id, 0) < t)) return id;
    if (error) *error = QString("no free layer in the '%1' band (%2 in use): hide or delete one first").arg(band).arg(ids.size());
    return -1;
}

QJsonObject OverlayManager::layerJson(const Layer& L) const {
    J o;
    o["ch"] = L.ch; o["channelId"] = L.ch; o["id"] = L.uid; o["layer"] = L.layer; o["slot"] = L.slot; o["role"] = L.role; o["band"] = bandFor(L.role);
    o["template"] = L.tpl; o["name"] = L.name; o["mode"] = L.mode; o["requestedMode"] = L.requestedMode;
    o["params"] = L.params; o["animation"] = L.animation;
    o["shownAtMs"] = double(L.shownAtMs); o["holdUntilMs"] = double(L.holdUntilMs); o["holdSec"] = L.holdSec; o["deployedMs"] = double(L.deployedMs);
    o["source"] = L.origin; o["label"] = L.label; o["hotkey"] = L.hotkey; o["state"] = L.state;
    if (!L.url.isEmpty()) o["url"] = L.url;
    if (!L.filePath.isEmpty()) o["filePath"] = L.filePath;
    if (!L.presetId.isEmpty()) o["presetId"] = L.presetId;
    if (!L.defaultId.isEmpty()) o["defaultId"] = L.defaultId;
    if (!L.cueKey.isEmpty()) o["cueId"] = L.cueKey;
    if (!L.ruleId.isEmpty()) o["ruleId"] = L.ruleId;
    if (!L.itemKey.isEmpty()) o["itemId"] = L.itemKey;
    if (!L.error.isEmpty()) o["error"] = L.error;
    if (!L.note.isEmpty()) o["note"] = L.note;
    if (!L.fellBackFrom.isEmpty()) o["fellBackFrom"] = L.fellBackFrom;
    if (L.restored) o["restored"] = true;
    if (L.userHidden) o["userHidden"] = true;
    if (!L.engineState.isEmpty()) { J e; e["state"] = L.engineState; e["status"] = L.engineStatus; o["engine"] = e; }
    return o;
}

QJsonArray OverlayManager::layersJson(const QString& ch) const {
    QJsonArray a;
    const auto it = m_ch.constFind(ch);
    if (it == m_ch.constEnd()) return a;
    for (const Layer& L : it->layers) a.append(layerJson(L));
    return a;
}

OverlayManager::Layer* OverlayManager::find(const QString& ch, int layer) {
    auto it = m_ch.find(ch); if (it == m_ch.end()) return nullptr;
    auto l = it->layers.find(layer); return l == it->layers.end() ? nullptr : &l.value();
}
OverlayManager::Layer* OverlayManager::findUid(const QString& ch, const QString& uid) {
    auto it = m_ch.find(ch); if (it == m_ch.end()) return nullptr;
    for (Layer& L : it->layers) if (L.uid == uid) return &L;
    return nullptr;
}

bool OverlayManager::parseSpec(const QJsonObject& body, Spec* s, QJsonObject* errOut) const {
    s->tpl = body.value("template").toString(body.value("templateId").toString()).trimmed();
    s->filePath = body.value("filePath").toString();
    if (body.contains("params") && !body.value("params").isObject() && !body.value("params").isNull()) { *errOut = err(400, "params must be an object"); return false; }
    s->params = body.value("params").toObject();
    s->animation = body.value("animation");
    s->role = body.value("role").toString();
    s->slot = (body.contains("slot") && !body.value("slot").isNull()) ? int(num(body.value("slot"), -1)) : -1;
    s->layer = body.contains("layer") ? int(num(body.value("layer"), -1)) : -1;
    s->mode = body.value("mode").toString("auto").trimmed().toLower(); if (s->mode.isEmpty()) s->mode = "auto";
    if (s->mode != "auto" && s->mode != "html" && s->mode != "native") { *errOut = err(400, "mode must be auto, html or native"); return false; }
    double hold = num(body.value("holdSec"), num(body.value("hold"), 0));
    if (hold <= 0) { const double ah = num(body.value("animation").toObject().value("autoHide"), 0); if (ah > 0) hold = ah / 1000.0; }
    if (hold < 0 || hold > 86400) { *errOut = err(400, "holdSec must be between 0 and 86400"); return false; }
    s->holdSec = hold;
    s->label = body.value("label").toString(); s->hotkey = body.value("hotkey").toString();
    s->exclusive = body.value("exclusive").toBool(false);
    return true;
}

QJsonObject OverlayManager::buildShow(const Layer& L, QString* error) const {
    int rw = 1920, rh = 1080; channelExists(L.ch, &rw, &rh);
    J cmd; cmd["action"] = "show"; cmd["layer"] = L.layer; cmd["role"] = L.role;
    if (L.mode == "html") {
        J src; src["type"] = "html"; src["url"] = L.liveUrl.isEmpty() ? L.url : L.liveUrl; src["width"] = 1920; src["height"] = 1080; src["transparent"] = true; src["reloadOnMiss"] = true;
        cmd["source"] = src;
        J place; place["x"] = 0; place["y"] = 0; place["width"] = rw; place["height"] = rh; cmd["placement"] = place;   // page is laid out at 1080p, scaled to the raster
        cmd["opacity"] = (L.revealPending || L.invisible) ? 0.0 : 1.0;
        J in; in["type"] = L.revealPending ? "cut" : "fade"; in["durationSec"] = 0.2; cmd["in"] = in;
        J out; out["type"] = "fade"; out["durationSec"] = 0.2; cmd["out"] = out;
    } else if (!L.tpl.isEmpty()) {
        const J tpl = findTemplate(L.tpl);
        if (tpl.isEmpty()) { if (error) *error = "unknown template: " + L.tpl; return J(); }
        J n = nativeCommandFor(tpl, mergeDefaults(tpl, L.params), L.role, L.animation, rw, rh);
        if (isErr(n)) { if (error) *error = n.value("error").toString(); return J(); }
        n["layer"] = L.layer; n["role"] = L.role; cmd = n;
    } else {
        J src; src["type"] = isImagePath(L.filePath) ? "image" : "file"; src["path"] = L.filePath; cmd["source"] = src;
        J place = L.params.value("placement").toObject();
        if (place.isEmpty()) { place["anchor"] = "top_right"; place["margin"] = 60.0 * rw / 1920.0; }
        cmd["placement"] = place;
        J in; in["type"] = "fade"; in["durationSec"] = 0.3; cmd["in"] = in; cmd["out"] = in;
    }
    if (L.holdSec > 0) cmd["holdSec"] = L.holdSec + 3.0;      // dead-man switch: the manager hides first
    return cmd;
}

bool OverlayManager::sendShow(Layer& L) {
    QString e; const J cmd = buildShow(L, &e);
    if (cmd.isEmpty()) { L.error = e; return false; }
    if (!m_hooks.sendCg || !m_hooks.sendCg(L.ch, cmd)) { L.error = "engine not connected"; return false; }
    L.state = "pending"; L.attachSeen = false; L.error.clear();
    L.sentMs = now(); L.lastRedeployMs = L.sentMs; L.liveUrl = L.url;
    L.pendingTimeoutMs = L.sentMs + (L.mode == "html" ? htmlFirstFrameMs() + 6000 : 6000);
    cancelTimed(L.ch, L.layer);
    schedule(L.pendingTimeoutMs, L, "pendingTimeout");
    return true;
}

void OverlayManager::sendEngine(const QString& ch, const QJsonObject& cmd) { if (m_hooks.sendCg) m_hooks.sendCg(ch, cmd); }

void OverlayManager::schedule(qint64 atMs, const Layer& L, const QString& kind) { Timed t; t.atMs = atMs; t.ch = L.ch; t.uid = L.uid; t.kind = kind; t.layer = L.layer; m_timed.append(t); }
void OverlayManager::cancelTimed(const QString& ch, int layer) {
    for (int i = m_timed.size() - 1; i >= 0; --i) if (m_timed[i].ch == ch && m_timed[i].layer == layer) m_timed.removeAt(i);
}

int OverlayManager::outWaitMs(const Layer& L) const {
    const QString out = L.animation.value("out").toString();
    if (out.isEmpty() || out == "none") return 0;
    return qBound(0, int(num(L.animation.value("outDuration"), num(L.animation.value("duration"), 400))), 3000);
}

void OverlayManager::warnBrowser(const QString& ch, const QString& detail) {
    ChannelState& c = m_ch[ch]; const qint64 t = now();
    if (c.lastBrowserAlertMs && t - c.lastBrowserAlertMs < 3600000) return;
    c.lastBrowserAlertMs = t;
    emit alert(ch, "warning", "HTML overlays unavailable: " + detail + "; using the native fallback where a twin exists");
}

void OverlayManager::emitLegacy(const Layer& L, bool cleared) {
    if (L.origin != "operator" && L.origin != "preset") return;
    ChannelState& c = m_ch[L.ch];
    J st; st["channelId"] = L.ch;
    if (cleared) {
        if (c.legacyLayer != L.layer) return;
        c.legacyLayer = -1; st["url"] = ""; st["cleared"] = true; emit cgHtmlState(L.ch, st); return;
    }
    c.legacyLayer = L.layer;
    st["template"] = L.tpl; st["name"] = L.name; st["params"] = L.params; st["animation"] = L.animation; st["role"] = L.role;
    st["url"] = L.url.mid(L.url.indexOf("/cg/") < 0 ? 0 : L.url.indexOf("/cg/")); st["absoluteUrl"] = L.url; st["atMs"] = double(L.deployedMs); st["onAir"] = L.mode; st["layer"] = L.layer;
    emit cgHtmlState(L.ch, st);
}

QJsonObject OverlayManager::deploySpec(const QString& ch, Spec s) {
    int rw = 1920, rh = 1080;
    if (!channelExists(ch, &rw, &rh)) return err(404, "no such channel: " + ch);
    if (!m_hooks.sendCg || !m_hooks.engineConnected || !m_hooks.engineConnected(ch)) return err(503, "the engine of channel '" + ch + "' is not connected");
    ChannelState& c = m_ch[ch];
    const J man = manifest();
    J tpl;
    if (!s.tpl.isEmpty()) { tpl = findTemplate(s.tpl); if (tpl.isEmpty()) return err(404, "unknown template: " + s.tpl); }
    else if (s.filePath.isEmpty() && s.urlOverride.isEmpty()) return err(400, "template (or filePath) required");
    if (!tpl.isEmpty()) s.params = cgtext::applyText(tpl, s.params);      // text1..3 -> the template's own text fields

    { const QByteArray raw = QJsonDocument(s.params).toJson(QJsonDocument::Compact);
      if (raw.contains("{{")) s.params = expandTokens(s.params, tokensFor(ch, c.item.active ? c.item.media : Media(), c.item.startMs)).toObject(); }

    const J anim = resolveAnimation(s.animation, man.value("presets").toArray());
    QString role = normRole(s.role); if (role.isEmpty()) role = normRole(tpl.value("role").toString()); if (role.isEmpty()) role = "secondary";
    const QString band = bandFor(role);

    // mode resolution: html needs the browser and the template file; native needs a twin
    QString htmlWhy; bool htmlOk = false, browserOk = false;
    if (!s.urlOverride.isEmpty() || !tpl.isEmpty()) {
        browserOk = browserAvailable();
        if (!browserOk) htmlWhy = browserMissingMessage();
        else htmlOk = s.urlOverride.isEmpty() ? templateFileOk(tpl, &htmlWhy) : true;
    } else htmlWhy = "file overlays have no html form";
    bool natOk = false; QString natWhy;
    if (!tpl.isEmpty()) { const J n = nativeCommandFor(tpl, mergeDefaults(tpl, s.params), role, anim, rw, rh); natOk = !isErr(n); if (!natOk) natWhy = n.value("error").toString(); }
    else if (!s.filePath.isEmpty()) natOk = true; else natWhy = "no native form";
    QString chosen;
    if (s.mode == "html") { if (!htmlOk) return err(409, "html overlay not possible: " + htmlWhy); chosen = "html"; }
    else if (s.mode == "native") { if (!natOk) return err(409, "native overlay not possible: " + natWhy); chosen = "native"; }
    else if (htmlOk && !(natOk && c.htmlBackoffUntilMs > now())) chosen = "html";
    else if (natOk) { chosen = "native"; if (!htmlOk && !tpl.isEmpty() && !browserOk) warnBrowser(ch, htmlWhy); }
    else return err(409, QString("template '%1' cannot be deployed: html: %2; native: %3").arg(s.tpl, htmlWhy, natWhy));

    QString e; int layerId = -1;
    if (s.layer < 0 && s.slot < 0)
        for (const Layer& x : c.layers) if ((!s.defaultId.isEmpty() && x.defaultId == s.defaultId) || (!s.presetId.isEmpty() && x.presetId == s.presetId && x.state != "hiding")) { layerId = x.layer; break; }
    if (layerId < 0) layerId = allocLayer(c, band, s.slot, s.layer, &e);
    if (layerId < 0) return err(409, e);
    if (s.exclusive) for (const int k : c.layers.keys()) { Layer& o = c.layers[k]; if (k != layerId && bandFor(o.role) == band && o.state != "hiding") beginHide(o); }
    if (c.layers.contains(layerId)) { cancelTimed(ch, layerId); c.layers.remove(layerId); }

    Layer L; L.uid = newUid(); L.ch = ch; L.layer = layerId; L.slot = bandTable().value(band).indexOf(layerId); L.role = role;
    L.tpl = s.tpl; L.filePath = s.filePath; L.mode = chosen; L.requestedMode = s.mode; L.origin = s.origin;
    L.name = !tpl.isEmpty() ? tpl.value("name").toString(s.tpl) : (s.filePath.isEmpty() ? QString("overlay") : QFileInfo(s.filePath).fileName());
    L.label = s.label; L.hotkey = s.hotkey; L.presetId = s.presetId; L.defaultId = s.defaultId; L.cueKey = s.cueKey; L.ruleId = s.ruleId; L.itemKey = s.itemKey;
    L.params = s.params; L.animation = anim; L.holdSec = s.holdSec; L.itemHideAtSec = s.itemHideAtSec;
    L.suppressOverride = s.suppressOverride; L.invisible = s.invisible; L.restored = s.restored;
    L.deployedMs = now();
    if (chosen == "html") {
        L.url = s.urlOverride.isEmpty() ? QString("http://127.0.0.1:%1%2").arg(m_hooks.restPort).arg(urlFor(tpl.value("file").toString(), s.params, anim)) : s.urlOverride;
        const QString in = anim.value("in").toString();
        L.revealPending = !s.invisible && !in.isEmpty() && in != "none";
    }
    if (chosen == "native" && s.mode == "auto" && !htmlOk && !tpl.isEmpty()) { L.fellBackFrom = "html"; L.note = htmlWhy; }
    c.layers.insert(layerId, L);
    Layer& ref = c.layers[layerId];
    if (!sendShow(ref)) { const QString msg = ref.error; c.layers.remove(layerId); return err(503, "could not send the overlay to the engine: " + msg); }
    emitLegacy(ref, false);
    touch(ch);
    qInfo().noquote() << QString("[Overlays] %1 deploy %2 layer %3 (%4, %5)").arg(ch, s.tpl.isEmpty() ? s.filePath : s.tpl).arg(layerId).arg(chosen, s.origin);
    return layerJson(ref);
}

// ---------------------------------------------------------------------------------------------
// Lifecycle: attach, hide, fail, drop, timers
// ---------------------------------------------------------------------------------------------

void OverlayManager::onShown(Layer& L) {
    cancelTimed(L.ch, L.layer);
    L.shownAtMs = now();
    if (L.hideWhenReady) { startHideSequence(L, L.hideTransition); return; }
    L.state = "on_air";
    if (L.holdSec > 0) { L.holdUntilMs = L.shownAtMs + qint64(L.holdSec * 1000.0); schedule(qMax(L.shownAtMs, L.holdUntilMs - outWaitMs(L)), L, "holdEnd"); }
    if (L.revealPending) {
        J cmd; cmd["action"] = "update"; cmd["layer"] = L.layer; J s; s["replay"] = true; cmd["source"] = s; sendEngine(L.ch, cmd);   // the page replays its in animation, then the layer is revealed
        schedule(L.shownAtMs + 220, L, "reveal");
    }
    touch(L.ch);
}

void OverlayManager::startHideSequence(Layer& L, const QJsonObject& transition) {
    L.state = "hiding"; L.hideWhenReady = false; L.hideTransition = transition;
    cancelTimed(L.ch, L.layer);
    int wait = 0;
    if (L.mode == "html" && !L.revealPending) {
        J cmd; cmd["action"] = "update"; cmd["layer"] = L.layer; J s; s["animateOut"] = true; cmd["source"] = s; sendEngine(L.ch, cmd);   // the page's own out animation
        wait = outWaitMs(L);
    }
    L.revealPending = false;
    if (wait > 0) schedule(now() + wait, L, "hideEngine"); else runHideEngine(L);
    touch(L.ch);
}

void OverlayManager::runHideEngine(Layer& L) {
    J cmd; cmd["action"] = "hide"; cmd["layer"] = L.layer;
    double fade = 0.0;
    J tr = L.hideTransition;
    if (tr.isEmpty() && L.mode == "html") { tr["type"] = "fade"; tr["durationSec"] = outWaitMs(L) > 0 ? 0.12 : 0.3; }
    if (!tr.isEmpty()) { cmd["transition"] = tr; fade = num(tr.value("durationSec"), 0.3); }
    else fade = num(L.animation.value("outDuration"), num(L.animation.value("duration"), 400)) / 1000.0;   // native: the layer's own out transition
    sendEngine(L.ch, cmd);
    schedule(now() + qint64(fade * 1000.0) + 250, L, "remove");
}

void OverlayManager::beginHide(Layer& L, const QJsonObject& transition, const QString& why) {
    if (L.state == "hiding" || L.state == "hidden") return;
    const QString ch = L.ch; const int id = L.layer;
    qInfo().noquote() << QString("[Overlays] %1 hide layer %2 %3").arg(ch).arg(id).arg(why);
    if (L.state == "pending") {            // not attached yet: hold the hide until the engine has it (see class comment)
        L.hideWhenReady = true; L.state = "hiding"; L.hideTransition = transition;
        J cmd; cmd["action"] = "hide"; cmd["layer"] = id; sendEngine(ch, cmd);
        touch(ch); return;
    }
    if (L.state == "failed" || L.state == "lost" || L.state == "suppressed") { dropLayer(ch, id, true); return; }
    startHideSequence(L, transition);
}

void OverlayManager::dropLayer(const QString& ch, int layer, bool sendRemove) {
    ChannelState& c = m_ch[ch];
    auto it = c.layers.find(layer); if (it == c.layers.end()) return;
    const Layer L = it.value();
    cancelTimed(ch, layer);
    if (sendRemove) {
        J cmd; cmd["action"] = "remove"; cmd["layer"] = layer; sendEngine(ch, cmd);
        if (L.state == "pending" || L.hideWhenReady) c.tombstones[layer] = now() + 20000;   // the loader may still attach it
    }
    for (int i = c.suppressed.size() - 1; i >= 0; --i) if (c.suppressed[i].layer == layer) c.suppressed.removeAt(i);
    c.layers.erase(c.layers.find(layer));
    emitLegacy(L, true);
    touch(ch);
}

void OverlayManager::failLayer(Layer& L, const QString& why) {
    const QString ch = L.ch; const int id = L.layer;
    ChannelState& c = m_ch[ch];
    cancelTimed(ch, id);
    if (L.hideWhenReady) { dropLayer(ch, id, true); return; }
    if (L.mode == "html") c.htmlBackoffUntilMs = now() + 30000;
    qWarning().noquote() << QString("[Overlays] %1 layer %2 (%3) failed: %4").arg(ch).arg(id).arg(L.name, why);
    if (L.mode == "html" && L.requestedMode == "auto" && !L.tpl.isEmpty()) {       // fall back to the native twin once
        int rw = 1920, rh = 1080; channelExists(ch, &rw, &rh);
        const J tpl = findTemplate(L.tpl);
        if (!tpl.isEmpty() && !isErr(nativeCommandFor(tpl, mergeDefaults(tpl, L.params), L.role, L.animation, rw, rh))) {
            L.fellBackFrom = "html"; L.note = "html failed: " + why; L.mode = "native"; L.url.clear(); L.liveUrl.clear(); L.revealPending = false;
            if (sendShow(L)) { emit alert(ch, "warning", QString("html overlay '%1' failed (%2): fell back to the native version").arg(L.name, why)); touch(ch); return; }
        }
    }
    L.state = "failed"; L.error = why; L.failedAtMs = now();
    J cmd; cmd["action"] = "remove"; cmd["layer"] = id; sendEngine(ch, cmd);
    c.tombstones[id] = now() + 20000;
    emit alert(ch, "warning", QString("overlay '%1' (layer %2) failed on %3: %4").arg(L.name).arg(id).arg(ch, why));
    touch(ch);
}

void OverlayManager::runTimed(const Timed& t) {
    Layer* L = find(t.ch, t.layer);
    if (!L || L->uid != t.uid) return;
    if (t.kind == "pendingTimeout") {
        if (L->state == "pending") failLayer(*L, QString("the engine did not attach the layer within %1 s (renderer or loader problem)").arg((L->pendingTimeoutMs - L->sentMs) / 1000));
        else if (L->state == "hiding" && L->hideWhenReady) dropLayer(t.ch, t.layer, true);
    } else if (t.kind == "holdEnd") { if (L->state == "on_air") beginHide(*L, J(), "hold elapsed"); }
    else if (t.kind == "hideEngine") { if (L->state == "hiding") runHideEngine(*L); }
    else if (t.kind == "reveal") {
        if (L->state == "on_air" && L->revealPending) { L->revealPending = false; J cmd; cmd["action"] = "update"; cmd["layer"] = L->layer; cmd["opacity"] = 1.0; sendEngine(t.ch, cmd); }
    } else if (t.kind == "remove") {
        if (!L->defaultId.isEmpty() && L->userHidden) {      // a default the operator took down keeps its slot
            J cmd; cmd["action"] = "remove"; cmd["layer"] = L->layer; sendEngine(t.ch, cmd);
            L->state = "hidden"; L->holdUntilMs = 0; touch(t.ch); return;
        }
        dropLayer(t.ch, t.layer, true);
    }
}

// ---------------------------------------------------------------------------------------------
// Engine events and reconcile
// ---------------------------------------------------------------------------------------------

void OverlayManager::onEngineConnected(const QString& ch) {
    ChannelState& c = m_ch[ch];
    c.connected = true; c.engine.clear(); c.fired.clear(); c.suppressed.clear(); c.suppressRoles.clear(); c.ruleRoles.clear(); c.tombstones.clear();
    c.item = Item(); c.startItemId.clear(); c.emergency = false;
    redeployAfterRestart(ch);
    ensureDefaults(ch, true);
    touch(ch);
}

void OverlayManager::redeployAfterRestart(const QString& ch) {
    ChannelState& c = m_ch[ch];
    const qint64 t = now();
    for (const int id : c.layers.keys()) {
        Layer& L = c.layers[id];
        if (!L.defaultId.isEmpty()) { L.userHidden = false; continue; }              // ensureDefaults redeploys them
        if (L.origin == "cue" || L.origin == "rule" || L.origin == "test" || L.origin == "pillar" || L.state == "failed" || L.state == "hiding") { dropLayer(ch, id, false); continue; }
        if (L.holdUntilMs > 0) { const double rem = (L.holdUntilMs - t) / 1000.0; if (rem < 3.0) { dropLayer(ch, id, false); continue; } L.holdSec = rem; }
        L.restored = true; L.revealPending = (L.mode == "html" && !L.animation.value("in").toString().isEmpty() && L.animation.value("in").toString() != "none");
        if (!sendShow(L)) { L.state = "lost"; L.lostSinceMs = t; }
    }
}

void OverlayManager::onEngineDisconnected(const QString& ch) {
    ChannelState& c = m_ch[ch];
    c.connected = false; c.engine.clear();
    for (const int id : c.layers.keys()) { Layer& L = c.layers[id]; cancelTimed(ch, id); if (L.state != "failed" && L.state != "hidden") { L.state = "lost"; L.lostSinceMs = now(); } }
    touch(ch);
}

void OverlayManager::onTelemetry(const QString& ch, const QJsonObject& t) {
    ChannelState& c = m_ch[ch];
    c.connected = true; c.telemetryMs = now(); c.state = t.value("state").toString();
    c.engine.clear();
    for (const QJsonValue& v : t.value("layers").toArray()) { const J o = v.toObject(); c.engine.insert(o.value("id").toInt(), o); }
    followItem(ch, t);
    reconcile(ch);
    enforceSuppress(ch);
    checkEmergency(ch);
}

void OverlayManager::onAsRun(const QString& ch, const QString& type, const QString& runlistId, const QString& assetId, qint64 startMs, const QJsonObject& extra) {
    ChannelState& c = m_ch[ch];
    if (type == "CG_SHOW") {
        Layer* L = find(ch, extra.value("layer").toInt(-1));
        if (L && (L->state == "pending" || (L->state == "hiding" && L->hideWhenReady)) && startMs + 300 >= L->sentMs) { L->attachSeen = true; onShown(*L); }
    } else if (type == "START") {
        c.startItemId = runlistId; c.startAtMs = now();
        beginItem(ch, runlistId, assetId, extra.value("title").toString(), startMs, 0.0, extra.contains("inPoint") ? extra.value("inPoint").toDouble(-1.0) : -1.0, true, extra.value("path").toString());
    }
}

void OverlayManager::onEngineAlert(const QString& ch, const QString&, const QString& message) {
    static const QRegularExpression re("^CG layer (\\d+):\\s*(.*)$");
    const QRegularExpressionMatch m = re.match(message);
    if (!m.hasMatch()) return;
    Layer* L = find(ch, m.captured(1).toInt());
    if (L && (L->state == "pending" || (L->state == "hiding" && L->hideWhenReady))) failLayer(*L, m.captured(2));
}

void OverlayManager::reconcile(const QString& ch) {
    ChannelState& c = m_ch[ch];
    const qint64 t = now();
    for (const int id : c.layers.keys()) {
        Layer* L = find(ch, id); if (!L) continue;
        const J e = c.engine.value(id);
        const bool present = !e.isEmpty();
        const QString es = e.value("state").toString();
        L->engineState = es; L->engineStatus = e.value("sourceStatus").toObject();
        const bool shownOnEngine = present && es != "hidden" && matchesEngine(L->mode, L->liveUrl, e);
        if (L->state == "pending" || (L->state == "hiding" && L->hideWhenReady)) { if (L->attachSeen || shownOnEngine) onShown(*L); continue; }
        if (L->state == "on_air") {
            if (!shownOnEngine && L->shownAtMs > 0 && t - L->shownAtMs > 1500 && !(present && es == "hidden" && L->holdUntilMs > 0 && t >= L->holdUntilMs)) {
                L->state = "lost"; L->lostSinceMs = t; L->error = "the engine no longer reports this layer";
                emit alert(ch, "warning", QString("overlay '%1' (layer %2) disappeared from the engine").arg(L->name).arg(id));
                touch(ch);
            } else if (L->mode == "html" && present && !L->updated && L->retakes == 0 && t - L->shownAtMs > 2500) {
                const QString u = e.value("source").toObject().value("url").toString();     // a stale loader attached after a newer deploy: show it again
                if (!u.isEmpty() && u != L->liveUrl && ++L->urlMismatch >= 2 && L->redeploys < 3) { ++L->redeploys; L->urlMismatch = 0; sendShow(*L); touch(ch); }
            }
        } else if (L->state == "lost") {
            if (shownOnEngine) { L->state = "on_air"; L->error.clear(); touch(ch); }
            else if (!L->defaultId.isEmpty() && c.connected && t - L->lastRedeployMs > 5000 && L->redeploys < 6) { ++L->redeploys; L->revealPending = false; if (sendShow(*L)) touch(ch); }
            else if (L->defaultId.isEmpty() && t - L->lostSinceMs > 120000) dropLayer(ch, id, true);
            else if (!L->defaultId.isEmpty() && L->redeploys >= 6 && L->state == "lost") { L->state = "failed"; L->failedAtMs = t; emit alert(ch, "warning", QString("default overlay '%1' cannot be redeployed on %2").arg(L->name, ch)); }
        } else if (L->state == "failed" && !L->defaultId.isEmpty() && t - L->failedAtMs > 15000 && c.defaultTries[L->defaultId] < 3) {
            ++c.defaultTries[L->defaultId]; L->mode = L->requestedMode == "native" ? "native" : L->mode; sendShow(*L); touch(ch);
        }
    }
    // engine layers in our bands that have no record: stale loaders of removed layers are removed again, others adopted
    for (auto it = c.engine.begin(); it != c.engine.end(); ++it) {
        const int id = it.key();
        bool inBand = false; for (const QList<int>& b : bandTable()) if (b.contains(id)) { inBand = true; break; }
        if (!inBand || c.layers.contains(id)) continue;
        const QString es = it.value().value("state").toString();
        if (c.tombstones.value(id, 0) > t || es == "hidden") {
            if (t - c.orphanSent.value(id, 0) > 3000) { c.orphanSent[id] = t; J cmd; cmd["action"] = "remove"; cmd["layer"] = id; sendEngine(ch, cmd); }
            continue;
        }
        const J src = it.value().value("source").toObject();
        Layer L; L.uid = newUid(); L.ch = ch; L.layer = id; L.role = normRole(it.value().value("role").toString("secondary")); L.slot = bandTable().value(bandFor(L.role)).indexOf(id);
        L.mode = src.value("type").toString() == "html" ? "html" : "native"; L.requestedMode = L.mode; L.origin = "adopted"; L.state = "on_air"; L.shownAtMs = t; L.deployedMs = t;
        L.url = src.value("url").toString(); L.liveUrl = L.url; L.updated = true;
        static const QRegularExpression re("/cg/([A-Za-z0-9_]+)\\.html");
        const QRegularExpressionMatch m = re.match(L.url);
        if (m.hasMatch()) L.tpl = m.captured(1);
        L.name = L.tpl.isEmpty() ? QString("layer %1").arg(id) : findTemplate(L.tpl).value("name").toString(L.tpl);
        c.layers.insert(id, L); touch(ch);
    }
}

void OverlayManager::housekeeping() {
    const qint64 t = now();
    for (const QString& ch : m_ch.keys()) {
        ChannelState& c = m_ch[ch];
        for (const int id : c.layers.keys()) { const Layer& L = c.layers[id]; if (L.state == "failed" && L.defaultId.isEmpty() && t - L.failedAtMs > 20000) dropLayer(ch, id, false); }
        for (auto it = c.tombstones.begin(); it != c.tombstones.end();) { if (it.value() < t - 5000) it = c.tombstones.erase(it); else ++it; }
        if (c.fired.size() > 2000) c.fired.clear();
    }
}

void OverlayManager::tick() {
    const qint64 t = now();
    QList<Timed> due;
    for (int i = 0; i < m_timed.size();) { if (m_timed[i].atMs <= t) due.append(m_timed.takeAt(i)); else ++i; }
    for (const Timed& x : due) runTimed(x);
    for (const QString& ch : m_ch.keys()) if (m_ch[ch].connected) tickCues(ch);
    checkTests();
    if (t - m_lastHouseMs >= 1000) { m_lastHouseMs = t; housekeeping(); }
    for (const QString& ch : m_ch.keys()) {
        ChannelState& c = m_ch[ch];
        if (!c.dirty) continue;
        c.dirty = false;
        J p; p["channelId"] = ch; p["layers"] = layersJson(ch);
        emit layersChanged(ch, p);
    }
}

void OverlayManager::checkEmergency(const QString& ch) {
    if (!m_exec) return;
    ChannelState& c = m_ch[ch];
    const bool em = m_exec->status(ch).emergency;
    if (em == c.emergency) return;
    c.emergency = em;
    if (!em) ensureDefaults(ch, false);          // left emergency: bring the always-on graphics back
    touch(ch);
}

// ---------------------------------------------------------------------------------------------
// Pillar panels: one layer (11) that fills the side bars beside a narrow picture (docs/pillar-panels.md)
// ---------------------------------------------------------------------------------------------

QJsonObject OverlayManager::onAirItem(const QString& ch) const {
    const auto it = m_ch.constFind(ch);
    if (it == m_ch.constEnd() || !it->item.active) return J();
    J o = mediaJson(it->item.media);
    o["runlistId"] = it->item.runlistId; o["itemTitle"] = it->item.title; o["startMs"] = double(it->item.startMs); o["totalSec"] = it->item.totalSec;
    return o;
}

void OverlayManager::refreshPillar(const QString& ch) { if (m_ch.contains(ch)) { m_ch[ch].pillarRetryAtMs = 0; syncPillar(ch); } }

void OverlayManager::syncPillar(const QString& ch) {
    if (!m_hooks.pillarFor) return;
    ChannelState& c = m_ch[ch];
    Layer* P = nullptr;
    for (auto it = c.layers.begin(); it != c.layers.end(); ++it) if (it->origin == "pillar" && it->state != "hiding") { P = &it.value(); break; }
    const J want = c.connected ? m_hooks.pillarFor(ch, onAirItem(ch)) : J();
    const QString tpl = want.value("template").toString();
    if (tpl.isEmpty()) { if (P) beginHide(*P, J(), "pillar: the picture fills the frame"); c.pillarError.clear(); return; }
    const J params = want.value("params").toObject();
    if (P && (P->state == "lost" || P->state == "failed")) { dropLayer(ch, P->layer, true); P = nullptr; }
    if (P && P->tpl == tpl) {
        bool same = true; for (auto it = params.begin(); it != params.end(); ++it) if (P->params.value(it.key()) != it.value()) { same = false; break; }
        if (!same) { J body; body["params"] = params; update(ch, P->layer, body); }
        return;
    }
    if (P) beginHide(*P, J(), "pillar: template changed");
    if (now() < c.pillarRetryAtMs) return;
    Spec s; s.tpl = tpl; s.role = "pillar"; s.origin = "pillar"; s.mode = want.value("mode").toString("auto"); s.params = params; s.label = "pillar panels";
    s.layer = bandTable().value("pillar").value(0, 11);
    const J r = deploySpec(ch, s);
    if (isErr(r)) {
        const QString e = r.value("error").toString();
        if (e != c.pillarError) emit alert(ch, "warning", "pillar panels: " + e);
        c.pillarError = e; c.pillarRetryAtMs = now() + 60000;
    } else c.pillarError.clear();
}

// ---------------------------------------------------------------------------------------------
// The item on air: cues and rules
// ---------------------------------------------------------------------------------------------

OverlayManager::Media OverlayManager::mediaFor(const QString& assetId, const QString& title, const QString& itemType, const QString& path) const {
    Media m; bool f = false;
    if (m_store && (!assetId.isEmpty() || !path.isEmpty())) {
        MediaRecord r;
        if (!assetId.isEmpty()) r = m_store->media(assetId, &f);
        if (!f && !path.isEmpty() && !path.contains("://")) r = m_store->mediaByPath(path, &f);    // an operator "play item" carries no asset id
        if (f) {
            m.found = true; m.id = r.id; m.title = r.title; m.type = r.type; m.series = r.series; m.client = r.client;
            m.category = r.extra.value("category").toString(); m.subcategory = r.extra.value("subcategory").toString(); m.tags = r.extra.value("tags").toArray();
            QStringList tags; for (const QJsonValue& v : m.tags) tags << v.toVariant().toString();
            m.text = (r.title + " " + tags.join(" ") + " " + r.extra.value("notes").toString() + " " + r.extra.value("transcript").toString() + " " + r.extra.value("transcriptText").toString()).toLower();
            m.mediaType = r.type; m.width = r.width; m.height = r.height; m.extra = r.extra;
            if (m.path.isEmpty()) m.path = r.path;
        }
    }
    if (!f) { m.id = assetId; m.title = title; m.text = title.toLower(); }
    if (!itemType.isEmpty()) m.type = itemType;
    if (!path.isEmpty()) m.path = path;
    const QString lp = m.path.toLower();
    m.live = m.type == "live" || m.type == "live_source" || m.mediaType == "live"
          || lp.startsWith("ndi://") || lp.startsWith("sdi://") || lp.startsWith("rtsp://") || lp.startsWith("srt://") || lp.startsWith("rtmp://") || lp.startsWith("udp://");
    if (m.live && m.type.isEmpty()) m.type = "live";
    // which switcher input is this (ui.inputMap cgs carry the engine path and the library id)
    if (m_store && (m.live || !lp.isEmpty())) {
        const QJsonArray inputSlots = m_store->settingJson("ui.inputMap").value("slots").toArray();
        for (int i = 0; i < inputSlots.size(); ++i) {
            const J s = inputSlots[i].toObject(); if (s.isEmpty()) continue;
            const QString ep = s.value("enginePath").toString().toLower(), sp = s.value("path").toString().toLower(), mid = s.value("mediaId").toString();
            if ((!lp.isEmpty() && (lp == ep || lp == sp)) || (!mid.isEmpty() && mid == m.id)) { m.input = QString("in%1").arg(i + 1); m.inputTitle = s.value("title").toString(); break; }
        }
    }
    return m;
}

QJsonObject OverlayManager::mediaJson(const Media& m) {
    J o; o["id"] = m.id; o["title"] = m.title; o["type"] = m.type; o["mediaType"] = m.mediaType; o["category"] = m.category; o["subcategory"] = m.subcategory;
    o["series"] = m.series; o["width"] = m.width; o["height"] = m.height; o["live"] = m.live; o["path"] = m.path; o["input"] = m.input; o["inputTitle"] = m.inputTitle;
    o["extra"] = m.extra; o["found"] = m.found;
    return o;
}

OverlayManager::Media OverlayManager::testMedia(const QJsonObject& o) {
    Media m; m.id = o.value("id").toString(); m.title = o.value("title").toString(); m.type = o.value("type").toString().toLower(); m.mediaType = o.value("mediaType").toString().toLower();
    m.category = o.value("category").toString(); m.subcategory = o.value("subcategory").toString(); m.series = o.value("series").toString();
    m.text = o.value("text").toString(m.title).toLower(); m.path = o.value("path").toString(); m.input = o.value("input").toString(); m.inputTitle = o.value("inputTitle").toString();
    m.live = o.value("live").toBool(m.type == "live"); m.width = o.value("width").toInt(); m.height = o.value("height").toInt(); m.extra = o.value("extra").toObject();
    m.found = true;
    return m;
}

QHash<QString, QString> OverlayManager::tokensFor(const QString& ch, const Media& m, qint64 itemStartMs) {
    QHash<QString, QString> t;
    t["title"] = m.title; t["series"] = m.series; t["client"] = m.client; t["category"] = m.category;
    t["first_aired"] = firstAired(ch, m.id, itemStartMs > 0 ? itemStartMs - 3000 : now());
    return t;
}

qint64 OverlayManager::earliestStart(const QString& id, qint64 before) {
    if (!m_store || id.isEmpty() || before <= 1) return 0;
    const auto cached = m_firstAired.constFind(id);
    if (cached != m_firstAired.constEnd()) return cached.value() < before ? cached.value() : 0;
    qint64 hi = before - 1;
    if (m_store->airedCount(id, 0, hi) <= 0) return 0;
    qint64 lo = 0;
    while (hi - lo > 1) { const qint64 mid = lo + (hi - lo) / 2; if (m_store->airedCount(id, 0, mid) >= 1) hi = mid; else lo = mid; }
    m_firstAired.insert(id, hi);
    return hi;
}

// MM-dd-yyyy of the first airing BEFORE the current one (the row of the play in progress is excluded); "" until it has aired before.
QString OverlayManager::firstAired(const QString& ch, const QString& mediaId, qint64 beforeMs) {
    const qint64 ms = earliestStart(mediaId, beforeMs);
    if (ms <= 0) return QString();
    bool f = false; const ChannelProfile p = m_store->channel(ch, &f);
    return QDateTime::fromMSecsSinceEpoch(ms, f ? p.tz() : QTimeZone::systemTimeZone()).toString("MM-dd-yyyy");
}

double OverlayManager::elapsed(const ChannelState& c) const {
    double e = c.item.joinOffsetSec + c.clock.pos;
    if (c.clock.running) e += qMin<qint64>(now() - c.clock.recvMs, 3000) / 1000.0;
    return e;
}

void OverlayManager::followItem(const QString& ch, const QJsonObject& t) {
    ChannelState& c = m_ch[ch];
    const QString itemId = t.value("itemId").toString(), state = t.value("state").toString();
    const bool live = (state == "ON_AIR" || state == "PAUSED") && !itemId.isEmpty();
    c.clock.recvMs = now(); c.clock.pos = t.value("position").toDouble(); c.clock.running = state == "ON_AIR";
    const double dur = t.value("duration").toDouble();
    if (live) {
        if (!c.item.active || c.item.runlistId != itemId) {
            if (!c.startItemId.isEmpty() && itemId != c.startItemId && now() - c.startAtMs < 3000) return;   // telemetry still names the previous item
            beginItem(ch, itemId, t.value("assetId").toString(), t.value("title").toString(), now() - qint64(c.clock.pos * 1000.0), c.clock.pos, t.contains("inPoint") ? t.value("inPoint").toDouble(-1.0) : -1.0, false, t.value("path").toString());
        }
        if (c.item.active && dur > 0 && qAbs(dur - c.item.durationSec) > 0.5) {
            c.item.durationSec = dur; c.item.totalSec = c.item.joinOffsetSec + dur;
            for (Cue& q : c.item.cues) if (q.state == "waiting") timeCue(q, c.item.totalSec);
        }
    } else if (c.item.active) { endItem(ch, "off air"); syncPillar(ch); }
}

void OverlayManager::endItem(const QString& ch, const QString& why) {
    ChannelState& c = m_ch[ch];
    const QString key = c.item.runlistId;
    for (const int id : c.layers.keys()) { Layer& L = c.layers[id]; if (!key.isEmpty() && L.itemKey == key && (L.origin == "cue" || L.origin == "rule")) beginHide(L, J(), "item ended: " + why); }
    c.item = Item();
    c.ruleRoles.clear(); c.ruleWhy.clear(); c.suppressRoles.clear();
    restoreSuppressed(ch, true);
}

void OverlayManager::beginItem(const QString& ch, const QString& runlistId, const QString& assetId, const QString& title, qint64 startMs, double enginePos, double engineInPoint, bool fromStart, const QString& path) {
    ChannelState& c = m_ch[ch];
    if (c.item.active && c.item.runlistId == runlistId) {      // START again (seek): re-anchor the clock only
        if (fromStart) { c.clock.recvMs = now(); c.clock.pos = 0; c.clock.running = true; }
        return;
    }
    if (c.item.active) {
        // CG slot graphics stay up across a take to another input that has the same CG (no out / in on every cut): park them
        // here, and after the new item's rules are known either keep them or hide them.
        for (Layer& L : c.layers)
            if (L.origin == "rule" && L.ruleId.startsWith("cg:") && L.itemKey == c.item.runlistId && (L.state == "on_air" || L.state == "pending")) L.itemKey = "carry:" + L.ruleId;
        endItem(ch, "next item");
    }
    Item it; it.active = true; it.runlistId = runlistId; it.assetId = assetId; it.title = title; it.startMs = startMs; it.fromStart = fromStart;
    QJsonArray itemCues; QString itemType, itemPath = path;
    if (m_exec) {
        if (const RunlistItem* ri = m_exec->lookupRunlistItem(ch, runlistId)) {
            if (ri->item.mediaId.isEmpty() ? ri->item.id == assetId : ri->item.mediaId == assetId) {
                itemCues = ri->item.cgOverlays; itemType = ri->item.type; if (itemPath.isEmpty()) itemPath = ri->item.path;
                if (it.title.isEmpty()) it.title = ri->item.title;
                it.durationSec = ri->durationSec;
                if (engineInPoint >= 0.0) it.joinOffsetSec = qMax(0.0, engineInPoint - ri->inSec);
                else {
                    const ScheduleExecutor::ChannelStatus st = m_exec->status(ch);       // join in progress: the executor advanced the in point by (push time - planned start)
                    if (st.lastPushReason == "join_in_progress" && ri->plannedStartMs <= st.lastPushMs && st.lastPushMs < ri->plannedEndMs && qAbs(startMs - st.lastPushMs) <= 30000)
                        it.joinOffsetSec = (st.lastPushMs - ri->plannedStartMs) / 1000.0;
                }
            }
        }
    }
    it.media = mediaFor(assetId, it.title, itemType, itemPath);        // live-ness comes from the path / library type, not from the take id
    if (!it.media.live && runlistId.startsWith("take-") && !it.media.found && (itemPath.startsWith("http://") || itemPath.startsWith("https://"))) { it.media.live = true; if (it.media.type.isEmpty()) it.media.type = "live"; }   // a stream URL taken from the switcher
    if (m_ch[ch].clock.recvMs == 0) c.clock.recvMs = now();
    if (fromStart) { c.clock.recvMs = now(); c.clock.pos = 0; c.clock.running = true; } else { c.clock.pos = enginePos; }
    if (it.durationSec <= 0) it.durationSec = c.clock.running ? 0 : 0;
    it.totalSec = it.durationSec > 0 ? it.joinOffsetSec + it.durationSec : 0;
    c.item = it;

    // cues: item cgOverlays (default in 0, until the end) and media extra.cgCues (default in 10 s, out 10 s before the end)
    int n = 0;
    auto add = [&](const QJsonArray& arr, const QString& origin) {
        for (const QJsonValue& v : arr) {
            const J o = v.toObject(); ++n;
            if (o.isEmpty() || isKaraokeCue(o) || !o.value("enabled").toBool(true)) continue;
            if (o.value("template").toString(o.value("templateId").toString()).isEmpty() && o.value("filePath").toString().isEmpty()) continue;
            Cue q; q.origin = origin; q.spec = o; q.key = origin + ":" + o.value("id").toString(QString::number(n));
            q.showAt = num(o.value("startOffset"), origin == "media" ? 10.0 : 0.0);
            timeCue(q, c.item.totalSec);
            c.item.cues.append(q);
        }
    };
    add(itemCues, "item"); add(it.media.found ? it.media.id.isEmpty() ? QJsonArray() : m_store->media(it.media.id).extra.value("cgCues").toArray() : QJsonArray(), "media");

    // rules: suppress roles now, `show` as a cue
    const QList<RuleHit> hits = evaluateRules(ch, c.item.media);
    applyRules(ch, c.item, hits);
    for (const RuleHit& h : hits) {
        if (!h.match) continue;
        c.item.rulesFired << h.name;
        const QJsonArray shows = h.then.value("show").isArray() ? h.then.value("show").toArray() : QJsonArray{h.then.value("show")};
        for (int k = 0; k < shows.size(); ++k) {
            const J show = shows[k].toObject();
            if (show.isEmpty()) continue;
            Cue q; q.origin = "rule"; q.spec = show; q.spec["id"] = h.id; q.key = "rule:" + h.id + (k ? QString("#%1").arg(k) : QString()); q.showAt = num(show.value("startOffset"), 0.0);
            timeCue(q, c.item.totalSec);
            c.item.cues.append(q);
        }
    }
    for (const int id : c.layers.keys()) {
        Layer& L = c.layers[id];
        if (!L.itemKey.startsWith("carry:")) continue;
        bool keep = false;
        for (Cue& q : c.item.cues)
            if (q.origin == "rule" && q.key == "rule:" + L.ruleId && q.state == "waiting") {
                keep = true; q.state = "fired"; q.layer = L.layer; q.reason = "kept on air from the previous input";
                c.fired.insert(runlistId + "|" + q.key); L.itemHideAtSec = q.hideAt; break;
            }
        L.itemKey = runlistId;
        if (!keep) beginHide(L, J(), "next item");
    }
    touch(ch);
    syncPillar(ch);
}

void OverlayManager::timeCue(Cue& q, double total) const {
    const double d = num(q.spec.value("duration"), 0.0);
    const double e = num(q.spec.value("endOffset"), q.origin == "media" && !q.spec.contains("duration") ? 10.0 : 0.0);
    double hide = d > 0 ? q.showAt + d : -1.0;
    if (total > 0) { const double cap = total - qMax(0.0, e); hide = hide < 0 ? cap : qMin(hide, cap); }
    q.hideAt = hide;
    if (hide >= 0 && hide - q.showAt < 0.5) { q.state = "skipped"; q.reason = "item too short for this cue"; }
    else if (q.state == "skipped" && q.reason == "item too short for this cue") { q.state = "waiting"; q.reason.clear(); }
}

void OverlayManager::tickCues(const QString& ch) {
    ChannelState& c = m_ch[ch];
    if (!c.item.active) return;
    const double el = elapsed(c);
    for (int i = 0; i < c.item.cues.size(); ++i) {
        Cue& q = c.item.cues[i];
        if (q.state != "waiting" || el < q.showAt) continue;
        if (c.fired.contains(c.item.runlistId + "|" + q.key)) { q.state = "fired"; continue; }
        if (q.hideAt >= 0 && el >= q.hideAt - 0.5) { q.state = "skipped"; q.reason = "window already passed (late join)"; continue; }
        fireCue(ch, q);
    }
    for (const int id : c.layers.keys()) {
        Layer& L = c.layers[id];
        if (L.itemHideAtSec >= 0 && L.state == "on_air" && L.itemKey == c.item.runlistId && el >= L.itemHideAtSec) beginHide(L, J(), "cue window ended");
    }
}

void OverlayManager::fireCue(const QString& ch, Cue& q) {
    ChannelState& c = m_ch[ch];
    c.fired.insert(c.item.runlistId + "|" + q.key);
    q.state = "fired";
    Spec s; J body = q.spec;
    s.tpl = body.value("template").toString(body.value("templateId").toString()); s.filePath = body.value("filePath").toString();
    s.animation = body.value("animation"); s.role = body.value("role").toString(); s.mode = body.value("mode").toString("auto");
    if (s.mode.isEmpty()) s.mode = "auto";
    s.origin = q.origin == "rule" ? "rule" : "cue"; s.cueKey = q.key; s.ruleId = q.origin == "rule" ? body.value("id").toString() : QString(); s.itemKey = c.item.runlistId;
    // Input-assigned CG slots obey station suppression rules, including on a take to an ad or ID.
    // Explicit rule show actions retain their existing override semantics.
    s.itemHideAtSec = q.hideAt; s.suppressOverride = q.origin == "rule" && !s.ruleId.startsWith("cg:"); s.label = body.value("label").toString();
    const QHash<QString, QString> tok = tokensFor(ch, c.item.media, c.item.startMs);
    s.params = expandTokens(body.value("params"), tok).toObject();
    if (body.value("requireAired").toBool(false) && tok.value("first_aired").isEmpty()) { q.state = "skipped"; q.reason = "not aired before"; return; }
    const QString role = normRole(s.role.isEmpty() ? findTemplate(s.tpl).value("role").toString("secondary") : s.role);
    if (!s.suppressOverride && roleSuppressed(c.suppressRoles, role)) { q.state = "skipped"; q.reason = "role suppressed by rule " + c.suppressWhy.join(", "); return; }
    if (s.ruleId.startsWith("cg:"))         // a CG slot the operator already put up by hand: leave that one on air
        for (const Layer& L : c.layers) if (L.presetId == s.ruleId && (L.state == "on_air" || L.state == "pending")) { q.state = "skipped"; q.reason = "already on air (fired by hand)"; return; }
    const J r = deploySpec(ch, s);
    if (isErr(r)) { q.state = "skipped"; q.reason = r.value("error").toString(); emit alert(ch, "warning", QString("cue %1 on '%2': %3").arg(q.key, c.item.title, q.reason)); }
    else {
        q.layer = r.value("layer").toInt(-1);
        // output mixes the graphic is limited to (a CG slot's `mixes`); the engine holds it while the layer opens
        const QJsonArray mixes = body.value("mixes").toArray();
        if (!mixes.isEmpty() && q.layer >= 0) { J u; u["action"] = "update"; u["layer"] = q.layer; u["mixes"] = mixes; sendEngine(ch, u); }
    }
}

bool OverlayManager::ruleMatches(const QJsonObject& r, const QString& ch, const Media& m, QJsonObject* whyOut) {
    const J w = r.value("when").toObject(); J why;
    const bool any = w.value("mode").toString().toLower() == "any";
    const bool negate = r.value("negate").toBool(false) || w.value("negate").toBool(false);
    // channels scope the rule: always AND, never negated
    const QStringList chs = listOf(r.value("channels"));
    bool chOk = true;
    if (chs.isEmpty()) why["channels"] = "any"; else { chOk = chs.contains(ch.toLower()); why["channels"] = chOk; }
    int asked = 0, hits = 0;
    auto tally = [&](const char* key, bool hit) { ++asked; if (hit) ++hits; why[key] = hit; };
    auto crit = [&](const char* key, const QStringList& l, const QStringList& values) {
        if (l.isEmpty()) { why[key] = "any"; return; }
        bool hit = false; for (const QString& v : values) if (!v.isEmpty() && l.contains(v.toLower())) { hit = true; break; }
        tally(key, hit);
    };
    crit("types", listOf(w.value("types")), {m.type, m.mediaType, m.live ? QString("live") : QString()});
    crit("categories", listOf(w.value("categories")), {m.category, m.subcategory});
    crit("subcategories", listOf(w.value("subcategories")), {m.subcategory});
    crit("series", listOf(w.value("series")), {m.series});
    const QStringList kw = listOf(w.value("keywords"));
    if (kw.isEmpty()) why["keywords"] = "any";
    else { bool hit = false; if (!m.text.trimmed().isEmpty()) for (const QString& k : kw) if (m.text.contains(k)) { hit = true; break; } tally("keywords", hit); }
    // inputs: "in3" / "3" = switcher slot 3; anything else matches the engine path or the input / item title (exact or part of it)
    const QStringList ins = listOf(w.value("inputs"));
    if (ins.isEmpty()) why["inputs"] = "any";
    else {
        static const QRegularExpression slotRe("^(?:in|input\\s*)?(\\d{1,2})$");
        const QString lp = m.path.toLower(), lt = m.inputTitle.toLower(), li = m.title.toLower();
        bool hit = false;
        for (const QString& v : ins) {
            if (v == "*" || v == "all" || v == "any") { if (!m.input.isEmpty()) { hit = true; break; } continue; }    // any switcher input
            const QRegularExpressionMatch sm = slotRe.match(v);
            if (sm.hasMatch()) { if (!m.input.isEmpty() && m.input == "in" + QString::number(sm.captured(1).toInt())) { hit = true; break; } continue; }
            if ((!lp.isEmpty() && lp.contains(v)) || (!lt.isEmpty() && lt.contains(v)) || (m.live && !li.isEmpty() && li.contains(v))) { hit = true; break; }
        }
        tally("inputs", hit);
    }
    const bool crits = asked == 0 ? true : (any ? hits > 0 : hits == asked);
    bool match;
    if (negate) { match = chOk && asked > 0 && !crits; why["negate"] = true; }
    else match = chOk && crits;
    if (any) why["mode"] = "any";
    if (whyOut) *whyOut = why;
    return match;
}

QList<OverlayManager::RuleHit> OverlayManager::evaluateRules(const QString& ch, const Media& m) const {
    QList<RuleHit> out;
    for (const QJsonValue& rv : ruleList()) {
        const J r = rv.toObject(); RuleHit h; h.id = r.value("id").toString(); h.name = r.value("name").toString(h.id); h.then = r.value("then").toObject();
        if (!r.value("enabled").toBool(true)) { h.reasons["enabled"] = false; out << h; continue; }
        J why; h.match = ruleMatches(r, ch, m, &why); h.reasons = why; out << h;
    }
    // CG slots assigned to inputs run as rules after the station rules (a suppress rule still hides other graphics).
    for (const QJsonValue& sv : cgSlotList()) {
        const J r = cgSlotRule(sv.toObject());
        if (r.isEmpty()) continue;
        RuleHit h; h.id = r.value("id").toString(); h.name = r.value("name").toString(); h.then = r.value("then").toObject();
        J why; h.match = ruleMatches(r, ch, m, &why); why["cgSlot"] = sv.toObject().value("id"); h.reasons = why; out << h;
    }
    return out;
}

bool OverlayManager::roleSuppressed(const QSet<QString>& roles, const QString& role) {
    return roles.contains(role) || (roles.contains("*") && role != "legal_id" && role != "emergency" && role != "pillar");
}

void OverlayManager::applyRules(const QString& ch, const Item&, const QList<RuleHit>& hits) {
    setSuppress(ch, QSet<QString>(), QStringList());
    QSet<QString> roles; QStringList why;
    for (const RuleHit& h : hits) {
        if (!h.match) continue;
        for (const QString& r : listOf(h.then.value("suppress"))) roles.insert(r == "all" ? QString("*") : normRole(r));
        if (!h.then.value("suppress").toArray().isEmpty()) why << h.name;
    }
    setSuppress(ch, roles, why);
}

void OverlayManager::setSuppress(const QString& ch, const QSet<QString>& roles, const QStringList& why) {
    ChannelState& c = m_ch[ch];
    c.ruleRoles = roles; c.ruleWhy = why; c.suppressRoles = roles; c.suppressWhy = why;
    restoreSuppressed(ch, false);
    enforceSuppress(ch);
}

void OverlayManager::suppressLayer(const QString& ch, int layer, const QString& role) {
    ChannelState& c = m_ch[ch];
    for (const Suppressed& s : c.suppressed) if (s.layer == layer) return;
    Layer* L = find(ch, layer);
    if (L) {
        if (L->state != "on_air") return;
        L->state = "suppressed"; L->holdRemainingMs = L->holdUntilMs > 0 ? qMax<qint64>(0, L->holdUntilMs - now()) : -1.0; L->holdUntilMs = 0;
        cancelTimed(ch, layer);
    }
    J cmd; cmd["action"] = "hide"; cmd["layer"] = layer; J tr; tr["type"] = "fade"; tr["durationSec"] = 0.25; cmd["transition"] = tr; sendEngine(ch, cmd);
    Suppressed s; s.layer = layer; s.role = role; s.uid = L ? L->uid : QString();
    c.suppressed.append(s);
    touch(ch);
}

void OverlayManager::restoreSuppressed(const QString& ch, bool all) {
    ChannelState& c = m_ch[ch];
    for (int i = c.suppressed.size() - 1; i >= 0; --i) {
        const Suppressed s = c.suppressed[i];
        if (!all && roleSuppressed(c.suppressRoles, s.role)) continue;
        c.suppressed.removeAt(i);
        Layer* L = s.uid.isEmpty() ? nullptr : findUid(ch, s.uid);
        if (L && L->state == "suppressed") {
            const double rem = L->holdRemainingMs;
            if (rem >= 0 && rem < 400) { dropLayer(ch, s.layer, true); continue; }
            L->state = "on_air";
            if (rem > 0) {
                L->holdUntilMs = now() + qint64(rem); schedule(L->holdUntilMs - outWaitMs(*L), *L, "holdEnd");
                J u; u["action"] = "update"; u["layer"] = s.layer; u["holdSec"] = rem / 1000.0 + 3.0; sendEngine(ch, u);
            }
            L->holdRemainingMs = -1.0;
        }
        J cmd; cmd["action"] = "show"; cmd["layer"] = s.layer; J tr; tr["type"] = "fade"; tr["durationSec"] = 0.25; cmd["transition"] = tr; sendEngine(ch, cmd);
        touch(ch);
    }
}

void OverlayManager::enforceSuppress(const QString& ch) {
    ChannelState& c = m_ch[ch];
    if (c.suppressRoles.isEmpty()) return;
    for (auto it = c.engine.begin(); it != c.engine.end(); ++it) {
        const int id = it.key(); const QString st = it.value().value("state").toString();
        if ((st != "shown" && st != "in") || id == 31 || id == 32) continue;        // captions are never role-suppressed
        const QString role = normRole(it.value().value("role").toString());
        if (role.isEmpty() || !roleSuppressed(c.suppressRoles, role)) continue;
        Layer* L = find(ch, id);
        if (L && (L->suppressOverride || L->state != "on_air")) continue;
        suppressLayer(ch, id, role);
    }
}

void OverlayManager::checkTests() {
    const qint64 t = now();
    const QList<PendingTest> list = m_tests; m_tests.clear();
    for (PendingTest pt : list) {
        Layer* L = findUid(pt.ch, pt.uid);
        J r; r["channelId"] = pt.ch;
        auto finish = [&](bool ok, const QString& msg) {
            r["ok"] = ok; if (!ok) r["error"] = msg;
            if (L) { r["status"] = L->engineStatus; r["elapsedMs"] = double(t - pt.startedMs); }
            r["browser"] = browserInfo();
            if (L) { if (L->state == "failed") dropLayer(pt.ch, pt.uid.isEmpty() ? 0 : L->layer, false); else beginHide(*L, J(), "renderer test done"); }
            if (pt.reply) pt.reply(200, r);
        };
        if (!L) { finish(false, "the test layer disappeared"); continue; }
        if (L->state == "failed") { finish(false, L->error); continue; }
        if (L->state == "on_air" && pt.okAtMs == 0) pt.okAtMs = t;
        if (pt.okAtMs > 0 && t - pt.okAtMs >= 1500) { finish(true, QString()); continue; }
        if (t - pt.startedMs > pt.timeoutMs) { finish(false, L->engineStatus.value("lastError").toString("no frame from the engine's html renderer within the timeout")); continue; }
        m_tests.append(pt);
    }
}

// ---------------------------------------------------------------------------------------------
// Presets, defaults, rules (settings overlays.presets, overlays.defaults.<ch>, overlays.rules)
// ---------------------------------------------------------------------------------------------

QJsonObject OverlayManager::normalizePreset(const QJsonObject& p, int index) {
    J o;
    o["id"] = p.value("id").toString(QString("preset%1_%2").arg(index + 1).arg(QDateTime::currentMSecsSinceEpoch(), 0, 36));
    o["name"] = p.value("name").toString(o.value("id").toString()); o["hotkey"] = p.value("hotkey").toString();
    o["template"] = p.value("template").toString(p.value("templateId").toString()); o["params"] = p.value("params").toObject();
    o["animation"] = p.value("animation").isString() ? p.value("animation") : QJsonValue(p.value("animation").toObject());
    o["role"] = p.value("role").toString(); o["mode"] = p.value("mode").toString("auto"); o["holdSec"] = num(p.value("holdSec"), 0);
    return o;
}
QJsonObject OverlayManager::normalizeDefault(const QJsonObject& p, int index) {
    J o = normalizePreset(p, index); o.remove("hotkey"); o.remove("holdSec"); o.remove("name");
    o["id"] = p.value("id").toString(QString("default%1").arg(index + 1));
    o["enabled"] = p.value("enabled").toBool(true);
    return o;
}
QJsonObject OverlayManager::normalizeRule(const QJsonObject& p, int index) {
    J o; o["id"] = p.value("id").toString(QString("rule%1_%2").arg(index + 1).arg(QDateTime::currentMSecsSinceEpoch(), 0, 36));
    o["name"] = p.value("name").toString(o.value("id").toString()); o["enabled"] = p.value("enabled").toBool(true);
    QJsonArray chs; for (const QString& s : listOf(p.value("channels"))) chs.append(s); o["channels"] = chs;
    const J w = p.value("when").toObject(); J nw;
    for (const char* k : {"types", "categories", "subcategories", "keywords", "series", "inputs"}) { QJsonArray a; for (const QString& s : listOf(w.value(k))) a.append(s); nw[k] = a; }
    nw["mode"] = w.value("mode").toString().toLower() == "any" ? "any" : "all";
    o["when"] = nw;
    o["negate"] = p.value("negate").toBool(w.value("negate").toBool(false));
    const J t = p.value("then").toObject(); J nt;
    QJsonArray sup; for (const QString& s : listOf(t.value("suppress"))) sup.append(s == "all" ? QString("*") : normRole(s)); nt["suppress"] = sup;
    if (t.value("show").isObject() && !t.value("show").toObject().isEmpty()) nt["show"] = t.value("show");
    else if (t.value("show").isArray()) {                  // several graphics (a name strap and a bug for one input, say)
        QJsonArray shows; for (const QJsonValue& v : t.value("show").toArray()) if (v.isObject() && !v.toObject().isEmpty()) shows.append(v);
        if (shows.size() == 1) nt["show"] = shows[0]; else if (!shows.isEmpty()) nt["show"] = shows;
    }
    o["then"] = nt;
    return o;
}

QJsonArray OverlayManager::presetList() const { return m_store ? m_store->settingJson("overlays.presets").value("presets").toArray() : QJsonArray(); }
QJsonArray OverlayManager::ruleList() const { return m_store ? m_store->settingJson("overlays.rules").value("rules").toArray() : QJsonArray(); }
QJsonArray OverlayManager::defaultList(const QString& ch) const { return m_store ? m_store->settingJson("overlays.defaults." + ch).value("defaults").toArray() : QJsonArray(); }

static QJsonArray arrayBody(const J& body, const char* key) { return body.contains(key) ? body.value(key).toArray() : body.value("_array").toArray(); }

QJsonObject OverlayManager::presets() { J o; o["presets"] = presetList(); return o; }
QJsonObject OverlayManager::setPresets(const QJsonObject& body) {
    if (!body.contains("presets") && !body.contains("_array")) return err(400, "presets[] required");
    QJsonArray out; QSet<QString> ids; int i = 0;
    for (const QJsonValue& v : arrayBody(body, "presets")) { J p = normalizePreset(v.toObject(), i++); if (ids.contains(p.value("id").toString())) return err(400, "duplicate preset id: " + p.value("id").toString()); ids.insert(p.value("id").toString()); out.append(p); }
    J st; st["presets"] = out; m_store->setSettingJson("overlays.presets", st);
    J ev; ev["kind"] = "presets"; emit configChanged(ev);
    J o; o["presets"] = out; return o;
}

QJsonObject OverlayManager::firePreset(const QString& ch, const QString& id, const QJsonObject& body) {
    J pr; for (const QJsonValue& v : presetList()) if (v.toObject().value("id").toString() == id) pr = v.toObject();
    if (pr.isEmpty()) return err(404, "no such preset: " + id);
    if (body.value("toggle").toBool(false)) for (Layer& L : m_ch[ch].layers) if (L.presetId == id && L.state == "on_air") { const int l = L.layer; beginHide(L, J(), "preset toggle"); J o = layerJson(m_ch[ch].layers.value(l)); o["toggledOff"] = true; return o; }
    Spec s; J pe;
    J merged = pr; for (auto it = body.begin(); it != body.end(); ++it) if (it.key() != "toggle" && it.key() != "params") merged[it.key()] = it.value();
    if (body.contains("params")) { J p = pr.value("params").toObject(); const J bp = body.value("params").toObject(); for (auto it = bp.begin(); it != bp.end(); ++it) p[it.key()] = it.value(); merged["params"] = p; }
    if (!parseSpec(merged, &s, &pe)) return pe;
    s.origin = "preset"; s.presetId = id; if (s.label.isEmpty()) s.label = pr.value("name").toString(); s.hotkey = pr.value("hotkey").toString();
    return deploySpec(ch, s);
}

QJsonObject OverlayManager::hidePreset(const QString& ch, const QString& id) {
    QJsonArray hidden;
    for (const int k : m_ch[ch].layers.keys()) { Layer& L = m_ch[ch].layers[k]; if (L.presetId == id && L.state != "hiding") { hidden.append(k); beginHide(L, J(), "preset hide"); } }
    J o; o["channelId"] = ch; o["presetId"] = id; o["hidden"] = hidden; return o;
}

QJsonObject OverlayManager::defaults(const QString& ch) {
    QJsonArray out;
    for (const QJsonValue& v : defaultList(ch)) {
        J d = v.toObject(); d["state"] = "none";
        for (const Layer& L : m_ch.value(ch).layers) if (L.defaultId == d.value("id").toString()) { d["state"] = L.state; d["layer"] = L.layer; }
        out.append(d);
    }
    J o; o["channelId"] = ch; o["defaults"] = out; return o;
}

void OverlayManager::ensureDefaults(const QString& ch, bool force) {
    ChannelState& c = m_ch[ch];
    if (!c.connected && (!m_hooks.engineConnected || !m_hooks.engineConnected(ch))) return;
    QSet<QString> wanted;
    for (const QJsonValue& v : defaultList(ch)) {
        const J d = v.toObject(); const QString id = d.value("id").toString();
        if (!d.value("enabled").toBool(true)) continue;
        wanted.insert(id);
        Layer* ex = nullptr; for (Layer& L : c.layers) if (L.defaultId == id) { ex = &L; break; }
        if (ex) {
            const bool changed = ex->tpl != d.value("template").toString() || ex->params != d.value("params").toObject();
            if (!force && !changed) continue;
            if (!force && ex->userHidden) continue;
            if (!changed) {      // same graphic: put it up again in its slot
                ex->userHidden = false; ex->redeploys = 0;
                ex->revealPending = ex->mode == "html" && !ex->animation.value("in").toString().isEmpty() && ex->animation.value("in").toString() != "none";
                if (!sendShow(*ex)) { ex->state = "lost"; ex->lostSinceMs = now(); }
                continue;
            }
        }
        Spec s; J pe; J body = d; body["holdSec"] = 0;
        if (!parseSpec(body, &s, &pe)) continue;
        s.origin = "default"; s.defaultId = id;
        const J r = deploySpec(ch, s);
        if (isErr(r)) emit alert(ch, "warning", QString("default overlay '%1' cannot be deployed on %2: %3").arg(id, ch, r.value("error").toString()));
    }
    for (const int k : c.layers.keys()) { Layer& L = c.layers[k]; if (!L.defaultId.isEmpty() && !wanted.contains(L.defaultId)) { L.userHidden = false; beginHide(L, J(), "default removed"); } }
}

QJsonObject OverlayManager::setDefaults(const QString& ch, const QJsonObject& body) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    if (!body.contains("defaults") && !body.contains("_array")) return err(400, "defaults[] required");
    QJsonArray out; QSet<QString> ids; int i = 0;
    for (const QJsonValue& v : arrayBody(body, "defaults")) { J d = normalizeDefault(v.toObject(), i++); if (ids.contains(d.value("id").toString())) return err(400, "duplicate default id: " + d.value("id").toString()); ids.insert(d.value("id").toString()); out.append(d); }
    J st; st["defaults"] = out; m_store->setSettingJson("overlays.defaults." + ch, st);
    ensureDefaults(ch, false);
    J ev; ev["kind"] = "defaults"; ev["channelId"] = ch; emit configChanged(ev);
    return defaults(ch);
}

QJsonObject OverlayManager::applyDefaults(const QString& ch) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    if (!m_hooks.engineConnected || !m_hooks.engineConnected(ch)) return err(503, "the engine of channel '" + ch + "' is not connected");
    ensureDefaults(ch, true);
    return defaults(ch);
}

QJsonObject OverlayManager::rules() { J o; o["rules"] = ruleList(); return o; }
QJsonObject OverlayManager::setRules(const QJsonObject& body) {
    if (!body.contains("rules") && !body.contains("_array")) return err(400, "rules[] required");
    QJsonArray out; int i = 0;
    for (const QJsonValue& v : arrayBody(body, "rules")) out.append(normalizeRule(v.toObject(), i++));
    J st; st["rules"] = out; m_store->setSettingJson("overlays.rules", st);
    J ev; ev["kind"] = "rules"; emit configChanged(ev);
    J o; o["rules"] = out; return o;
}

QJsonObject OverlayManager::testRules(const QString& ch, const QString& mediaId, const QHash<QString, QString>& q) {
    if (mediaId.isEmpty() && q.value("type").isEmpty() && q.value("path").isEmpty() && q.value("input").isEmpty()) return err(400, "mediaId (or type / path / input) required");
    Media m = mediaFor(mediaId, q.value("title"), q.value("type"), q.value("path"));
    if (!q.value("input").isEmpty()) m.input = q.value("input").toLower().startsWith("in") ? q.value("input").toLower() : "in" + q.value("input");
    if (q.value("live") == "1" || q.value("live") == "true") m.live = true;
    if (!mediaId.isEmpty() && !m.found) return err(404, "no such media: " + mediaId);
    const QList<RuleHit> hits = evaluateRules(ch, m);
    QJsonArray arr; QSet<QString> roles;
    for (const RuleHit& h : hits) {
        J r; r["id"] = h.id; r["name"] = h.name; r["match"] = h.match; r["reasons"] = h.reasons; r["then"] = h.then; arr.append(r);
        if (h.match) for (const QString& s : listOf(h.then.value("suppress"))) roles.insert(s == "all" ? QString("*") : normRole(s));
    }
    QJsonArray rl; for (const QString& r : roles) rl.append(r);
    J media = mediaJson(m); media.remove("extra");
    J o; o["channelId"] = ch; o["media"] = media; o["rules"] = arr; o["suppressRoles"] = rl; return o;
}

// ---------------------------------------------------------------------------------------------
// CG slots CG1, CG2, ... (setting overlays.cgSlots)
// ---------------------------------------------------------------------------------------------

QString OverlayManager::normCgInput(const QString& v0) {
    const QString v = v0.trimmed().toLower();
    if (v == "*" || v == "all" || v == "any") return "*";
    static const QRegularExpression re("^(?:in|input\\s*)?(\\d{1,2})$");
    const QRegularExpressionMatch m = re.match(v);
    if (m.hasMatch()) { const int n = m.captured(1).toInt(); return n > 0 ? QString("in%1").arg(n) : QString(); }
    return v;                                   // part of an engine path or input title (ruleMatches matches it the same way)
}

QJsonObject OverlayManager::normalizeCgSlot(const QJsonObject& p, int index) {
    J o;
    QString id = p.value("id").toString().trimmed().toLower();
    id.remove(QRegularExpression("[^a-z0-9_-]"));
    if (id.isEmpty()) id = QString("cg%1").arg(index + 1);
    o["id"] = id;
    o["name"] = p.value("name").toString().trimmed().isEmpty() ? id.toUpper() : p.value("name").toString().trimmed();
    o["enabled"] = p.value("enabled").toBool(true);
    o["template"] = p.value("template").toString(p.value("templateId").toString()).trimmed();
    o["params"] = p.value("params").toObject();
    o["animation"] = p.value("animation").isString() ? p.value("animation") : QJsonValue(p.value("animation").toObject());
    o["role"] = p.value("role").toString(); o["mode"] = p.value("mode").toString("auto");
    o["holdSec"] = qBound(0.0, num(p.value("holdSec"), 0), 86400.0);              // fired by hand: auto-hide (0 = until hidden)
    o["startOffset"] = qBound(0.0, num(p.value("startOffset"), 1.0), 3600.0);     // on an input: seconds after the take
    o["duration"] = qBound(0.0, num(p.value("duration"), 0.0), 86400.0);          // on an input: 0 = while the input is on air
    QJsonArray ins; QSet<QString> seen; bool all = false;
    for (const QString& s : listOf(p.value("inputs"))) { const QString n = normCgInput(s); if (n.isEmpty() || seen.contains(n)) continue; seen.insert(n); if (n == "*") all = true; ins.append(n); }
    o["inputs"] = all ? QJsonArray{QString("*")} : ins;
    QJsonArray chs; for (const QString& s : listOf(p.value("channels"))) chs.append(s); o["channels"] = chs;
    QJsonArray mixes; for (const QString& s : listOf(p.value("mixes"))) mixes.append(s); o["mixes"] = mixes;     // [] = every output mix
    return o;
}

bool OverlayManager::cgCoversInput(const QJsonObject& slot, const QString& input) {
    const QString in = normCgInput(input);
    for (const QJsonValue& v : slot.value("inputs").toArray()) { const QString s = v.toString(); if (s == "*" || s == in) return true; }
    return false;
}

QJsonObject OverlayManager::cgSlotRule(const QJsonObject& s) {
    const QJsonArray ins = s.value("inputs").toArray();
    if (!s.value("enabled").toBool(true) || s.value("template").toString().isEmpty() || ins.isEmpty()) return J();
    J show; show["template"] = s.value("template"); show["params"] = s.value("params"); show["animation"] = s.value("animation");
    show["role"] = s.value("role"); show["mode"] = s.value("mode"); show["startOffset"] = s.value("startOffset"); show["duration"] = s.value("duration");
    show["label"] = s.value("name"); if (!s.value("mixes").toArray().isEmpty()) show["mixes"] = s.value("mixes");
    J when; when["inputs"] = ins; when["mode"] = "all";
    J then; then["suppress"] = QJsonArray(); then["show"] = show;
    J r; r["id"] = "cg:" + s.value("id").toString(); r["name"] = s.value("name").toString() + " (CG on inputs)"; r["enabled"] = true;
    r["channels"] = s.value("channels"); r["negate"] = false; r["when"] = when; r["then"] = then;
    return r;
}

QJsonArray OverlayManager::assignCgInputIn(QJsonArray cgs, const QString& input, const QStringList& cg, int inputCount) {
    const QString in = normCgInput(input);
    for (int i = 0; i < cgs.size(); ++i) {
        J s = cgs[i].toObject();
        const bool want = cg.contains(s.value("id").toString()), has = cgCoversInput(s, in);
        if (want == has) continue;
        QJsonArray ins = s.value("inputs").toArray();
        if (want) ins.append(in);
        else if (ins.contains(QJsonValue(QStringLiteral("*")))) {     // "all inputs" minus this one: every other bus key, then the path / title entries
            QJsonArray rest; for (int k = 1; k <= qMax(1, inputCount); ++k) { const QString key = QString("in%1").arg(k); if (key != in) rest.append(key); }
            ins = rest;
        } else { QJsonArray rest; for (const QJsonValue& v : ins) if (v.toString() != in) rest.append(v); ins = rest; }
        s["inputs"] = ins; cgs[i] = s;
    }
    return cgs;
}

int OverlayManager::switcherInputCount() const {
    const int n = m_store ? m_store->settingJson("ui.inputMap").value("slots").toArray().size() : 0;
    return qMax(8, n);
}

QJsonArray OverlayManager::cgSlotList() const {
    const QJsonArray a = m_store ? m_store->settingJson("overlays.cgSlots").value("slots").toArray() : QJsonArray();
    if (!a.isEmpty()) return a;
    QJsonArray out; for (int i = 0; i < 3; ++i) out.append(normalizeCgSlot(J(), i));      // nothing stored yet: CG1..CG3, empty
    return out;
}

QJsonObject OverlayManager::cgSlots() {
    const QJsonArray cgs = cgSlotList(); const int n = switcherInputCount();
    J by;
    for (int k = 1; k <= n; ++k) {
        QJsonArray ids; const QString key = QString("in%1").arg(k);
        for (const QJsonValue& v : cgs) if (cgCoversInput(v.toObject(), key)) ids.append(v.toObject().value("id"));
        by[key] = ids;
    }
    J o; o["slots"] = cgs; o["byInput"] = by; o["inputs"] = n; return o;
}

QJsonObject OverlayManager::setCgSlots(const QJsonObject& body) {
    if (!body.contains("slots") && !body.contains("_array")) return err(400, "slots[] required");
    QJsonArray out; QSet<QString> ids; int i = 0;
    for (const QJsonValue& v : arrayBody(body, "slots")) {
        if (!v.isObject()) return err(400, "every slot must be an object");
        const J s = normalizeCgSlot(v.toObject(), i++);
        if (ids.contains(s.value("id").toString())) return err(400, "duplicate CG id: " + s.value("id").toString());
        ids.insert(s.value("id").toString()); out.append(s);
    }
    if (out.size() > 32) return err(400, "at most 32 CG slots");
    J st; st["slots"] = out; m_store->setSettingJson("overlays.cgSlots", st);
    J ev; ev["kind"] = "cgSlots"; emit configChanged(ev);
    return cgSlots();
}

QJsonObject OverlayManager::assignCgInput(const QJsonObject& body) {
    const QString in = normCgInput(body.value("input").isDouble() ? QString::number(body.value("input").toInt()) : body.value("input").toString());
    if (!in.startsWith("in") || in.mid(2).toInt() <= 0) return err(400, "input must be a switcher input: in1..in" + QString::number(switcherInputCount()));
    const QStringList want = listOf(body.value("cg"));
    QJsonArray cgs = cgSlotList(); QSet<QString> known; for (const QJsonValue& v : cgs) known.insert(v.toObject().value("id").toString());
    for (const QString& id : want) if (!known.contains(id)) return err(404, "no such CG: " + id);
    cgs = assignCgInputIn(cgs, in, want, switcherInputCount());
    J st; st["slots"] = cgs; m_store->setSettingJson("overlays.cgSlots", st);
    J ev; ev["kind"] = "cgSlots"; emit configChanged(ev);
    return cgSlots();
}

QJsonObject OverlayManager::fireCgSlot(const QString& ch, const QString& id0, const QJsonObject& body) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    const QString id = id0.toLower(), key = "cg:" + id;
    J slot; for (const QJsonValue& v : cgSlotList()) if (v.toObject().value("id").toString() == id) slot = v.toObject();
    if (slot.isEmpty()) return err(404, "no such CG: " + id);
    if (slot.value("template").toString().isEmpty()) return err(409, slot.value("name").toString() + " is empty: store a graphic in it first");
    if (body.value("toggle").toBool(false)) {
        QJsonArray hidden;
        for (const int k : m_ch[ch].layers.keys()) { Layer& L = m_ch[ch].layers[k]; if ((L.presetId == key || L.ruleId == key) && (L.state == "on_air" || L.state == "pending")) { hidden.append(k); beginHide(L, J(), "CG toggle"); } }
        if (!hidden.isEmpty()) { J o; o["channelId"] = ch; o["cg"] = id; o["hidden"] = hidden; o["toggledOff"] = true; return o; }
    }
    J merged; merged["template"] = slot.value("template"); merged["animation"] = slot.value("animation"); merged["role"] = slot.value("role"); merged["mode"] = slot.value("mode");
    merged["holdSec"] = body.contains("holdSec") ? body.value("holdSec") : slot.value("holdSec");
    J p = slot.value("params").toObject(); const J bp = body.value("params").toObject(); for (auto it = bp.begin(); it != bp.end(); ++it) p[it.key()] = it.value(); merged["params"] = p;
    Spec s; J pe;
    if (!parseSpec(merged, &s, &pe)) return pe;
    s.origin = "preset"; s.presetId = key; s.label = slot.value("name").toString(); s.suppressOverride = true;
    J r = deploySpec(ch, s);
    const QJsonArray mixes = slot.value("mixes").toArray();
    if (!isErr(r) && !mixes.isEmpty() && r.contains("layer")) { J u; u["action"] = "update"; u["layer"] = r.value("layer"); u["mixes"] = mixes; sendEngine(ch, u); }
    if (!isErr(r)) r["cg"] = id;
    return r;
}

QJsonObject OverlayManager::hideCgSlot(const QString& ch, const QString& id0) {
    const QString key = "cg:" + id0.toLower();
    QJsonArray hidden;
    for (const int k : m_ch[ch].layers.keys()) { Layer& L = m_ch[ch].layers[k]; if ((L.presetId == key || L.ruleId == key) && L.state != "hiding") { hidden.append(k); beginHide(L, J(), "CG hide"); } }
    J o; o["channelId"] = ch; o["cg"] = id0.toLower(); o["hidden"] = hidden; return o;
}

// ---------------------------------------------------------------------------------------------
// REST: layers
// ---------------------------------------------------------------------------------------------

QJsonObject OverlayManager::deploy(const QString& ch, const QJsonObject& body) {
    Spec s; J pe;
    if (!parseSpec(body, &s, &pe)) return pe;
    s.origin = "operator"; s.suppressOverride = true; s.presetId = body.value("presetId").toString();
    return deploySpec(ch, s);
}

QJsonObject OverlayManager::update(const QString& ch, int layer, const QJsonObject& body) {
    Layer* L = find(ch, layer);
    if (!L) return err(404, QString("no overlay on layer %1 of channel %2").arg(layer).arg(ch));
    if (!body.value("params").isObject() || body.value("params").toObject().isEmpty()) return err(400, "params (object) required");
    if (L->state == "hiding") return err(409, "the layer is being hidden");
    ChannelState& c = m_ch[ch];
    J np = expandTokens(body.value("params"), tokensFor(ch, c.item.active ? c.item.media : Media(), c.item.startMs)).toObject();
    if (!L->tpl.isEmpty()) np = cgtext::applyText(findTemplate(L->tpl), np);
    J merged = body.value("replace").toBool(false) ? J() : L->params;
    for (auto it = np.begin(); it != np.end(); ++it) merged[it.key()] = it.value();
    L->params = merged;
    if (L->mode == "html") L->url = QString("http://127.0.0.1:%1%2").arg(m_hooks.restPort).arg(urlFor(findTemplate(L->tpl).value("file").toString(), L->params, L->animation));
    if (L->state == "pending") { L->revealPending = L->revealPending; sendShow(*L); touch(ch); return layerJson(*L); }
    L->updated = true;
    J cmd; cmd["action"] = "update"; cmd["layer"] = layer;
    if (L->mode == "html") {
        J fields; for (auto it = np.begin(); it != np.end(); ++it) fields[it.key()] = paramString(it.value());
        J src; src["fields"] = fields; src["reloadOnMiss"] = true; cmd["source"] = src;
    } else {
        int rw = 1920, rh = 1080; channelExists(ch, &rw, &rh);
        const J tpl = findTemplate(L->tpl);
        const J n = tpl.isEmpty() ? J() : nativeCommandFor(tpl, mergeDefaults(tpl, L->params), L->role, L->animation, rw, rh);
        if (tpl.isEmpty() || isErr(n)) return err(409, "native update not possible: " + n.value("error").toString("no template"));
        cmd["source"] = n.value("source");
    }
    sendEngine(ch, cmd);
    touch(ch);
    J o = layerJson(*L); o["updateRequested"] = true; return o;
}

QJsonObject OverlayManager::retake(const QString& ch, int layer) {
    Layer* L = find(ch, layer);
    if (!L) return err(404, QString("no overlay on layer %1 of channel %2").arg(layer).arg(ch));
    if (L->state == "hiding" || L->state == "suppressed") return err(409, "layer is " + L->state);
    ++L->retakes;
    if (L->mode == "html" && L->state == "on_air") {       // navigate the page again: its in animation replays on air
        L->liveUrl = L->url + (L->url.contains('?') ? "&" : "?") + "_rt=" + QString::number(now());
        J cmd; cmd["action"] = "update"; cmd["layer"] = layer; J src; src["url"] = L->liveUrl; cmd["source"] = src; sendEngine(ch, cmd);
        L->shownAtMs = now();
        if (L->holdSec > 0) { cancelTimed(ch, layer); L->holdUntilMs = L->shownAtMs + qint64(L->holdSec * 1000.0); schedule(L->holdUntilMs - outWaitMs(*L), *L, "holdEnd"); }
    } else sendShow(*L);
    touch(ch);
    return layerJson(*L);
}

QJsonObject OverlayManager::hide(const QString& ch, int layer, const QJsonObject& body) {
    Layer* L = find(ch, layer);
    if (!L) return err(404, QString("no overlay on layer %1 of channel %2").arg(layer).arg(ch));
    if (!L->defaultId.isEmpty()) L->userHidden = true;       // an operator hide on a default sticks until the defaults are applied again
    beginHide(*L, body.value("transition").toObject(), "operator");
    Layer* after = find(ch, layer);
    if (!after) { J o; o["ch"] = ch; o["channelId"] = ch; o["layer"] = layer; o["state"] = "gone"; return o; }
    return layerJson(*after);
}

QJsonObject OverlayManager::remove(const QString& ch, int layer) {
    if (!find(ch, layer)) return err(404, QString("no overlay on layer %1 of channel %2").arg(layer).arg(ch));
    dropLayer(ch, layer, true);
    J o; o["deleted"] = layer; o["channelId"] = ch; return o;
}

QJsonObject OverlayManager::clear(const QString& ch, const QJsonObject& body) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    const QStringList roles = listOf(body.value("roles")); const bool defs = body.value("includeDefaults").toBool(false);
    QJsonArray cleared, kept;
    for (const int k : m_ch[ch].layers.keys()) {
        Layer& L = m_ch[ch].layers[k];
        if (!roles.isEmpty() && !roles.contains(L.role) && !roles.contains(bandFor(L.role))) continue;
        if (!L.defaultId.isEmpty() && !defs) { kept.append(k); continue; }
        if (!L.defaultId.isEmpty()) L.userHidden = true;
        if (L.state == "hiding") continue;
        cleared.append(k); beginHide(L, J(), "clear");
    }
    J o; o["channelId"] = ch; o["cleared"] = cleared; o["keptDefaults"] = kept; return o;
}

QJsonObject OverlayManager::channelState(const QString& ch) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    const ChannelState& c = m_ch[ch];
    J o; o["channelId"] = ch; o["connected"] = c.connected; o["engineState"] = c.state; o["emergency"] = c.emergency;
    o["layers"] = layersJson(ch); o["defaults"] = defaults(ch).value("defaults");
    J item; item["active"] = c.item.active;
    if (c.item.active) {
        item["runlistId"] = c.item.runlistId; item["assetId"] = c.item.assetId; item["title"] = c.item.title; item["elapsedSec"] = elapsed(c); item["totalSec"] = c.item.totalSec; item["joinOffsetSec"] = c.item.joinOffsetSec;
        QJsonArray cs; for (const Cue& q : c.item.cues) { J j; j["key"] = q.key; j["origin"] = q.origin; j["state"] = q.state; j["reason"] = q.reason; j["showAt"] = q.showAt; j["hideAt"] = q.hideAt; j["layer"] = q.layer; j["template"] = q.spec.value("template").toString(q.spec.value("templateId").toString()); cs.append(j); }
        item["cues"] = cs; QJsonArray rf; for (const QString& r : c.item.rulesFired) rf.append(r); item["rulesFired"] = rf;
    }
    o["item"] = item;
    QJsonArray sr; for (const QString& r : c.suppressRoles) sr.append(r); o["suppressedRoles"] = sr;
    o["bands"] = bandsJson();
    return o;
}

QJsonObject OverlayManager::cues(const QString& ch) {
    if (!channelExists(ch)) return err(404, "no such channel: " + ch);
    QJsonArray items;
    if (m_exec) {
        const J rl = m_exec->runlistJson(ch);
        for (const char* part : {"current", "next"}) for (const QJsonValue& v : rl.value(part).toObject().value("items").toArray()) {
            const J it = v.toObject();
            QJsonArray ic = it.value("cgOverlays").toArray(), mc;
            const QString mid = it.value("mediaId").toString();
            if (!mid.isEmpty() && m_store) { bool f = false; const MediaRecord r = m_store->media(mid, &f); if (f) mc = r.extra.value("cgCues").toArray(); }
            if (ic.isEmpty() && mc.isEmpty()) continue;
            J row; row["runlistId"] = it.value("id"); row["title"] = it.value("title"); row["plannedStartMs"] = it.value("plannedStartMs"); row["mediaId"] = mid; row["itemCues"] = ic; row["mediaCues"] = mc; items.append(row);
        }
    }
    J o; o["channelId"] = ch; o["items"] = items; o["active"] = channelState(ch).value("item"); return o;
}

QJsonObject OverlayManager::overview() {
    J o = renderer(); J out; out["renderer"] = o;
    J chs; QStringList ids;
    if (m_hooks.channelIds) for (const QJsonValue& v : m_hooks.channelIds()) ids << v.toString();
    else if (m_store) for (const ChannelProfile& p : m_store->channels()) ids << p.id;
    for (const QString& ch : ids) { J c; c["layers"] = layersJson(ch); c["defaults"] = defaults(ch).value("defaults"); c["connected"] = m_ch.value(ch).connected; chs[ch] = c; }
    out["channels"] = chs; out["presets"] = presetList(); out["rules"] = ruleList(); out["templates"] = manifest(); out["bands"] = bandsJson();
    return out;
}

// ---------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------

QJsonObject OverlayManager::renderer() {
    J r = browserInfo();
    J engines;
    for (auto it = m_ch.begin(); it != m_ch.end(); ++it) {
        QJsonArray ls; double frames = 0, fps = 0, restarts = 0; QString state = "idle", lastError;
        for (const QJsonObject& e : it->engine) {
            if (e.value("source").toObject().value("type").toString() != "html") continue;
            const J st = e.value("sourceStatus").toObject(); ls.append(st);
            frames = qMax(frames, num(st.value("frames"), 0)); fps = qMax(fps, num(st.value("fps"), 0)); restarts = qMax(restarts, num(st.value("restarts"), 0));
            if (lastError.isEmpty()) lastError = st.value("lastError").toString();
            const QString s = st.value("state").toString("ok");
            if (s.contains("err") || s.contains("fail") || s.contains("crash")) state = s; else if (state == "idle" || state == "ok") state = s;
        }
        J html; html["state"] = state; html["frames"] = frames; html["fps"] = fps; html["restarts"] = restarts; html["lastError"] = lastError; html["layers"] = ls;
        J e; e["html"] = html; e["connected"] = it->connected; engines[it.key()] = e;
    }
    r["engines"] = engines; r["htmlBackoff"] = false;
    r["webRoot"] = QDir::toNativeSeparators(m_hooks.webRoot); r["restPort"] = m_hooks.restPort;
    r["settings"] = J{{"cg.browserPath", settingStr("cg.browserPath")}, {"cg.htmlNthFrame", settingStr("cg.htmlNthFrame")}, {"cg.htmlFirstFrameMs", settingStr("cg.htmlFirstFrameMs")}};
    return r;
}

QJsonObject OverlayManager::setRenderer(const QJsonObject& body) {
    if (body.contains("browserPath")) {
        const QString p = QDir::fromNativeSeparators(body.value("browserPath").toString().trimmed());
        if (!p.isEmpty() && !QFileInfo(p).isFile() && !body.value("force").toBool(false)) return err(400, "not a file on the Supervisor's machine: " + QDir::toNativeSeparators(p));
        m_store->setSetting("cg.browserPath", p.isEmpty() ? QString() : QDir::toNativeSeparators(p));
    }
    if (body.contains("htmlNthFrame")) m_store->setSetting("cg.htmlNthFrame", body.value("htmlNthFrame").toVariant().toString());
    if (body.contains("htmlFirstFrameMs")) m_store->setSetting("cg.htmlFirstFrameMs", body.value("htmlFirstFrameMs").toVariant().toString());
    m_browserCache = J();
    emit engineEnvChanged();
    J ev; ev["kind"] = "renderer"; emit configChanged(ev);
    J o = renderer(); o["note"] = "stored; engines pick the new browser path up the next time they start (nothing was restarted)"; return o;
}

void OverlayManager::testRenderer(const QJsonObject& body, std::function<void(int, const QJsonObject&)> reply) {
    QString ch = body.value("channel").toString(body.value("channelId").toString());
    if (ch.isEmpty() && m_hooks.channelIds) for (const QJsonValue& v : m_hooks.channelIds()) if (m_hooks.engineConnected && m_hooks.engineConnected(v.toString())) { ch = v.toString(); break; }
    if (ch.isEmpty() || !channelExists(ch)) { reply(404, err(404, "channel required (an existing channel with a connected engine)")); return; }
    if (!browserAvailable()) { J r; r["ok"] = false; r["channelId"] = ch; r["error"] = browserMissingMessage(); r["browser"] = browserInfo(); reply(200, r); return; }
    const bool visible = body.value("visible").toBool(false);
    static const QString page = "<html><body style='margin:0;background:transparent'><div style=\"position:absolute;right:40px;bottom:40px;padding:14px 22px;background:#0a84ff;color:#fff;font:700 28px Segoe UI,Arial;border-radius:10px;animation:p 1s infinite alternate\">PLAY PLAY overlay test</div><style>@keyframes p{from{opacity:.55}to{opacity:1}}</style></body></html>";
    Spec s; s.origin = "test"; s.mode = "html"; s.role = "secondary"; s.invisible = !visible; s.suppressOverride = true; s.label = "renderer test";
    s.urlOverride = "data:text/html;charset=utf-8," + QString::fromLatin1(QUrl::toPercentEncoding(page));
    s.holdSec = visible ? qBound(1.0, num(body.value("holdSec"), 3.0), 30.0) : 0.0;
    const J r = deploySpec(ch, s);
    if (isErr(r)) { reply(r.value("status").toInt(500), r); return; }
    PendingTest pt; pt.ch = ch; pt.uid = r.value("id").toString(); pt.startedMs = now(); pt.timeoutMs = qint64(num(body.value("timeoutMs"), htmlFirstFrameMs() + 5000)); pt.reply = reply;
    m_tests.append(pt);
}
