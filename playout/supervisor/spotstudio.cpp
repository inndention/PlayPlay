#include "spotstudio.h"
#include "spotgenerator.h"
#include "renderfarm.h"
#include "publisher.h"
#include "suitehttp.h"
#include "suitelogic.h"
#include "aimodels.h"
#include "areamarket.h"
#include "../schedule/schedulestore.h"
#include "../schedule/formatclock.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <memory>

using namespace sched;

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static QJsonObject errObj(int status, const QString& m) { QJsonObject e; e["error"] = m; e["status"] = status; return e; }
static const char* kRecipe = "studio_spot";

SpotStudio::SpotStudio(ScheduleStore* store, SpotGenerator* spots, RenderFarm* farm, const QString& dataDir, const QString& studioDir, QObject* parent)
    : QObject(parent), m_store(store), m_spots(spots), m_farm(farm), m_http(new SuiteHttp(this)), m_dataDir(dataDir), m_studioDir(studioDir) {
    QDir().mkpath(m_studioDir);
    ensureRecipe();
    // live progress of the render behind each project (local generator or render farm)
    if (m_spots) connect(m_spots, &SpotGenerator::event, this, [this](const QString& type, const QJsonObject& p) {
        if (type != "spots") return; const QJsonObject cur = p.value("current").toObject(); if (cur.isEmpty()) return;
        for (const QJsonValue& v : all()) { const QJsonObject pr = v.toObject(); if (pr.value("render").toObject().value("genId").toString() == cur.value("id").toString() && pr.value("status").toString() == "rendering") {
            QJsonObject live; live["pct"] = cur.value("pct"); live["step"] = cur.value("step"); m_live[pr.value("id").toString()] = live; emit event("spotstudio", QJsonObject{{"project", project(pr.value("id").toString())}}); } }
    });
    if (m_farm) connect(m_farm, &RenderFarm::event, this, [this](const QString& type, const QJsonObject& p) {
        if (type != "render") return;
        for (const QJsonValue& jv : p.value("jobs").toArray()) { const QJsonObject j = jv.toObject();
            for (const QJsonValue& v : all()) { const QJsonObject pr = v.toObject(); if (pr.value("render").toObject().value("jobId").toString() == j.value("id").toString() && pr.value("status").toString() == "rendering") {
                QJsonObject live; live["pct"] = j.value("pct"); live["step"] = j.value("step"); live["node"] = j.value("nodeName"); m_live[pr.value("id").toString()] = live; emit event("spotstudio", QJsonObject{{"project", project(pr.value("id").toString())}}); } } }
    });
}

QStringList SpotStudio::voices() {
    return {"af_heart", "af_bella", "af_nicole", "af_sarah", "af_sky", "am_adam", "am_michael", "bf_emma", "bf_isabella", "bm_george", "bm_lewis", "Microsoft Zira Desktop", "Microsoft David Desktop"};
}

QJsonArray SpotStudio::musicBeds() const {
    QJsonArray a; const QDir d(QDir(m_dataDir).filePath("spots/music"));
    for (const QFileInfo& fi : d.entryInfoList({"*.mp3", "*.wav", "*.m4a", "*.flac", "*.ogg"}, QDir::Files, QDir::Name)) a.append(fi.fileName());
    return a;
}

void SpotStudio::ensureRecipe() {
    QJsonObject over = m_store->settingJson("spots.recipes");
    if (over.contains(kRecipe)) return;
    QJsonObject r; r["label"] = "Studio spot"; r["source"] = "text"; r["type"] = "promo"; r["category"] = "studio"; r["subcategory"] = "Spots"; r["series"] = "";
    r["kicker"] = "SPOT"; r["ttlMin"] = 525600; r["targetSec"] = 30; r["voice"] = "af_heart"; r["energy"] = 5; r["skipIfNone"] = false;
    r["prompt"] = "Polish this advertising or promotional script for a local TV station. Keep every name, number, offer and date exactly as given; never add claims.";
    over[kRecipe] = r; m_store->setSettingJson("spots.recipes", over);
}

void SpotStudio::options(JsonCb cb) {
    QJsonObject o;
    QJsonArray local; local.append(QJsonObject{{"id", "local-card"}, {"title", "Station card (local)"}, {"engine", "local"}, {"description", "Branded card with headline and bullets over a photo or gradient bed, narration and music bed, rendered on this Supervisor."}});
    QJsonArray v; for (const QString& s : voices()) v.append(s);
    o["voices"] = v; o["music"] = musicBeds(); o["aspects"] = QJsonArray{"16:9", "9:16", "1:1", "4:5"}; o["lengths"] = QJsonArray{6, 15, 30, 60}; o["kinds"] = QJsonArray{"ad", "promo", "psa", "announcement"};
    const QJsonObject env = m_spots ? m_spots->status().value("env").toObject() : QJsonObject();
    // Ready when every step the card spot needs has a program: its in-house tool (spots.engines, DEP-13) or python + ffmpeg as before.
    // Music is optional (a spot goes without when it cannot be composed). An older SpotGenerator without the engines block: python + ffmpeg.
    const QJsonObject steps = env.value("engines").toObject();
    QStringList missing;
    if (steps.isEmpty()) { if (!env.value("python").toBool()) missing << "python"; if (!env.value("ffmpeg").toBool()) missing << "ffmpeg"; }
    else for (const char* s : {"card", "tts", "compose"}) {
        const QJsonObject e = steps.value(s).toObject(); if (e.value("ready").toBool()) continue;
        missing << QString("%1 (%2)").arg(QString(s), e.value("uses").toString() == "ffmpeg" ? QString("ffmpeg") : e.value("uses").toString() == "native" ? e.value("tool").toString() : QString("python or its script"));
    }
    const bool localOk = missing.isEmpty();
    QJsonObject engines; engines["local"] = QJsonObject{{"ok", localOk}, {"reason", localOk ? QString() : "missing for the local spot pipeline: " + missing.join(", ")}};
    const QJsonObject sn = m_farm ? suites::chooseRenderNode(m_farm->nodes(), "spotnode") : QJsonObject();
    engines["studio"] = QJsonObject{{"ok", !sn.isEmpty()}, {"node", sn.value("name")}, {"reason", sn.isEmpty() ? QString("no spot-node ready (Render page)") : QString()}};
    o["engines"] = engines;
    if (!m_farm) { o["templates"] = local; cb(o); return; }
    m_farm->templates([o, local, cb](const QJsonObject& t) mutable {
        QJsonArray all = local; for (const QJsonValue& x : t.value("templates").toArray()) { QJsonObject y = x.toObject(); y["engine"] = "studio"; all.append(y); }
        o["templates"] = all; if (t.contains("reason")) o["templatesReason"] = t.value("reason"); cb(o);
    });
}

// =====================================================================================================
// projects
// =====================================================================================================
QJsonArray SpotStudio::all() const { return m_store->settingJson("spotstudio.projects").value("projects").toArray(); }

QJsonObject SpotStudio::save(QJsonObject p) {
    p["updatedMs"] = double(nowMs());
    QJsonArray a = all(); bool hit = false;
    for (int i = 0; i < a.size(); ++i) if (a[i].toObject().value("id").toString() == p.value("id").toString()) { a[i] = p; hit = true; }
    if (!hit) a.append(p);
    while (a.size() > 200) a.removeFirst();
    QJsonObject o; o["projects"] = a; m_store->setSettingJson("spotstudio.projects", o);
    const QJsonObject out = project(p.value("id").toString());
    emit event("spotstudio", QJsonObject{{"project", out}});
    return out;
}

QJsonArray SpotStudio::projects() const {
    QJsonArray out; const QJsonArray a = all();
    for (int i = a.size() - 1; i >= 0; --i) { QJsonObject p = a[i].toObject(); const QJsonObject live = m_live.value(p.value("id").toString()); if (!live.isEmpty() && p.value("status").toString() == "rendering") { QJsonObject r = p.value("render").toObject(); for (auto it = live.begin(); it != live.end(); ++it) r[it.key()] = it.value(); p["render"] = r; } out.append(p); }
    return out;
}

QJsonObject SpotStudio::project(const QString& id, bool* found) const {
    for (const QJsonValue& v : projects()) if (v.toObject().value("id").toString() == id) { if (found) *found = true; return v.toObject(); }
    if (found) *found = false; return {};
}

void SpotStudio::history(QJsonObject& p, const QString& actor, const QString& action, const QString& note) const {
    QJsonArray h = p.value("history").toArray(); QJsonObject e; e["ms"] = double(nowMs()); e["actor"] = actor; e["action"] = action; if (!note.isEmpty()) e["note"] = note.left(500);
    h.append(e); while (h.size() > 80) h.removeFirst(); p["history"] = h;
}

static int snapLength(int s) { const int opts[] = {6, 15, 30, 60}; int best = 30; for (int o : opts) if (qAbs(o - s) < qAbs(best - s)) best = o; return best; }

void SpotStudio::redraft(QJsonObject& p, int concept) const {
    QJsonObject opts; opts["brief"] = p.value("brief"); opts["lengthSec"] = p.value("lengthSec"); opts["brand"] = p.value("brand"); opts["client"] = p.value("client"); opts["kind"] = p.value("kind"); opts["concepts"] = 3;
    const QJsonObject d = suites::draftScript(opts, concept);
    p["concepts"] = d.value("concepts"); p["concept"] = d.value("concept"); p["scenes"] = d.value("scenes"); p["endCard"] = d.value("endCard"); p["questions"] = d.value("questions"); p["words"] = d.value("words");
    QJsonObject st; st["compose"] = true; p["stale"] = st;
}

QJsonObject SpotStudio::create(const QJsonObject& b, const QString& actor) {
    const QString brief = b.value("brief").toString().trimmed();
    if (brief.size() < 10) return errObj(400, "brief required (product or service, audience, offer: real facts only)");
    QJsonObject p; p["id"] = "sp-" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    p["title"] = b.value("title").toString(suites::sentences(brief).value(0).left(60));
    p["kind"] = QStringList{"ad", "promo", "psa", "announcement"}.contains(b.value("kind").toString()) ? b.value("kind").toString() : "ad";
    p["client"] = b.value("client").toString(); p["brief"] = brief; p["lengthSec"] = snapLength(b.value("lengthSec").toInt(30)); p["aspect"] = b.value("aspect").toString("16:9");
    p["template"] = b.value("template").toString("local-card"); p["voice"] = b.value("voice").toString("af_heart"); p["music"] = b.value("music").toString(); p["tone"] = b.value("tone").toString();
    { const QString mk = areamarket::normalizeKey(b.value("market").toString()); p["market"] = (mk == "none" || (m_spots && !mk.isEmpty() && !m_spots->marketData(mk).isEmpty())) ? mk : QString(); }      // area market (areamarket.h): localizes the voice, strap and writer
    p["engine"] = b.value("engine").toString(p.value("template").toString() == "local-card" ? "local" : "studio");
    QJsonObject brand = b.value("brand").toObject(); if (brand.value("name").toString().isEmpty() && !p.value("client").toString().isEmpty()) brand["name"] = p.value("client"); p["brand"] = brand;
    p["status"] = "draft"; p["createdMs"] = double(nowMs()); p["revision"] = 0;
    redraft(p, 1);
    history(p, actor, "create");
    return save(p);
}

QJsonObject SpotStudio::update(const QString& id, const QJsonObject& b, const QString& actor) {
    bool f = false; QJsonObject p = project(id, &f);
    if (!f) return errObj(404, "no such project");
    if (QStringList{"published", "rendering"}.contains(p.value("status").toString())) return errObj(409, "the project is " + p.value("status").toString() + "; edits wait until it is not");
    QJsonObject answers = b.value("answers").toObject();
    if (b.contains("concept")) answers["concept"] = b.value("concept");
    bool redo = false; int concept = p.value("concept").toInt(1);
    QJsonObject brand = p.value("brand").toObject();
    for (auto it = answers.begin(); it != answers.end(); ++it) {
        const QString k = it.key(); const QJsonValue v = it.value();
        if (k == "concept") { concept = v.toInt(1); redo = true; }
        else if (k.startsWith("brand.")) brand[k.mid(6)] = v;
        else if (k == "brand" && v.isObject()) { const QJsonObject bo = v.toObject(); for (auto i2 = bo.begin(); i2 != bo.end(); ++i2) brand[i2.key()] = i2.value(); }
        else if (k == "lengthSec") { const int L = snapLength(v.toInt(p.value("lengthSec").toInt())); if (suites::sceneCountFor(L) != suites::sceneCountFor(p.value("lengthSec").toInt())) redo = true; p["lengthSec"] = L; }
        else if (QStringList{"voice", "music", "template", "engine", "aspect", "tone", "client", "title", "kind", "brief"}.contains(k)) { p[k] = v; if (k == "brief") redo = true; }
    }
    for (const char* k : {"voice", "music", "template", "engine", "aspect", "tone", "client", "title"}) if (b.contains(k)) p[k] = b.value(k);
    if (b.contains("market")) { const QString mk = areamarket::normalizeKey(b.value("market").toString()); p["market"] = (mk == "none" || (m_spots && !mk.isEmpty() && !m_spots->marketData(mk).isEmpty())) ? mk : QString(); }
    if (b.contains("brief")) { p["brief"] = b.value("brief"); redo = true; }
    if (b.contains("lengthSec")) { const int L = snapLength(b.value("lengthSec").toInt()); if (suites::sceneCountFor(L) != suites::sceneCountFor(p.value("lengthSec").toInt())) redo = true; p["lengthSec"] = L; }
    p["brand"] = brand;
    if (redo || b.value("rewrite_script").toBool()) redraft(p, concept);
    else {   // keep the operator's scene edits; refresh the end card from the brand
        QJsonObject ec = p.value("endCard").toObject();
        if (!brand.value("name").toString().isEmpty()) ec["headline"] = brand.value("name"); if (!brand.value("cta").toString().isEmpty()) ec["cta"] = brand.value("cta"); if (!brand.value("url").toString().isEmpty()) ec["url"] = brand.value("url");
        if (!answers.isEmpty() && (answers.contains("brand.name") || answers.contains("brand.cta"))) ec["vo"] = (ec.value("headline").toString().isEmpty() ? QString() : ec.value("headline").toString() + ". ") + ec.value("cta").toString("Learn more") + ".";
        p["endCard"] = ec;
    }
    QJsonArray scenes = p.value("scenes").toArray();
    for (const QJsonValue& ev : b.value("edits").toArray()) {
        const QJsonObject e = ev.toObject(); const int n = e.value("scene").toInt(); if (n < 1 || n > scenes.size()) continue;
        if (e.value("remove").toBool()) { scenes.removeAt(n - 1); continue; }
        QJsonObject s = scenes[n - 1].toObject(); for (const char* k : {"vo", "onScreen", "visual", "beat", "durationSec", "transition"}) if (e.contains(k)) s[k] = e.value(k); scenes[n - 1] = s;
    }
    for (const QJsonValue& av : b.value("addScenes").toArray()) {
        QJsonObject a = av.toObject(); const int after = qBound(0, a.value("after").toInt(scenes.size()), scenes.size()); a.remove("after");
        if (!a.contains("durationSec")) a["durationSec"] = 2; if (!a.contains("beat")) a["beat"] = "detail"; scenes.insert(after, a);
    }
    if (b.contains("endCard")) { QJsonObject ec = p.value("endCard").toObject(); const QJsonObject in = b.value("endCard").toObject(); for (auto it = in.begin(); it != in.end(); ++it) ec[it.key()] = it.value(); p["endCard"] = ec; }
    p["scenes"] = suites::balanceScenes(scenes, p.value("lengthSec").toInt());
    // answered questions drop out
    QJsonArray qs; for (const QJsonValue& q : p.value("questions").toArray()) { const QString qid = q.toObject().value("id").toString();
        const bool answered = answers.contains(qid) || (qid.startsWith("brand.") && !brand.value(qid.mid(6)).toString().isEmpty()) || (qid == "lengthSec" && answers.contains("lengthSec"));
        if (!answered) qs.append(q); }
    p["questions"] = qs;
    p["words"] = suites::narrationOf(p.value("scenes").toArray(), p.value("endCard").toObject()).simplified().split(' ', Qt::SkipEmptyParts).size();
    QJsonObject st = p.value("stale").toObject(); st["compose"] = true; p["stale"] = st;
    if (QStringList{"rendered", "in_review", "changes_requested", "approved", "rejected", "failed"}.contains(p.value("status").toString())) p["status"] = "draft";
    const QString instruction = b.value("instruction").toString().trimmed();
    if (!instruction.isEmpty()) { QJsonArray notes = p.value("writerNotes").toArray(); notes.append(instruction); p["writerNotes"] = notes; }
    history(p, actor, "edit", instruction);
    const QJsonObject out = save(p);
    if (!instruction.isEmpty()) llmRewrite(id, instruction, actor);
    return out;
}

bool SpotStudio::remove(const QString& id) {
    QJsonArray a = all(), out; bool hit = false;
    for (const QJsonValue& v : a) { if (v.toObject().value("id").toString() == id) { hit = true; continue; } out.append(v); }
    if (hit) { QJsonObject o; o["projects"] = out; m_store->setSettingJson("spotstudio.projects", o); m_live.remove(id); emit event("spotstudio", QJsonObject{{"removed", id}}); }
    return hit;
}

void SpotStudio::llmRewrite(const QString& id, const QString& instruction, const QString& actor) {
    const QJsonObject llm = m_store->settingJson("spots.llm");
    const QString key = AiModels::apiKey(m_store);
    if (key.isEmpty()) {   // no writer configured: the note stays with the project for the reviewer
        bool f = false; QJsonObject p = project(id, &f); if (!f) return; history(p, "writer", "note kept", "no OpenRouter key (OPENROUTER_API_KEY); the instruction is kept as a note"); save(p); return;
    }
    bool f = false; QJsonObject p = project(id, &f); if (!f) return;
    const QString cur = QString::fromUtf8(QJsonDocument(QJsonObject{{"scenes", p.value("scenes")}, {"endCard", p.value("endCard")}}).toJson(QJsonDocument::Compact));
    QString prompt = QString("You write %1-second TV spots for a local station. Brief (the only facts you may use):\n%2\n\nCurrent script JSON:\n%3\n\nInstruction from the producer: %4\n\n"
                                   "Return ONLY JSON {\"scenes\":[{\"beat\",\"visual\",\"vo\",\"onScreen\"}],\"endCard\":{\"headline\",\"cta\",\"url\",\"vo\"}}. Keep %5 scenes including the end card as the last scene with empty vo. "
                                   "Never invent names, prices, dates or claims; spell numbers as words in vo; onScreen <= 40 chars.")
                               .arg(QString::number(p.value("lengthSec").toInt()), p.value("brief").toString().left(3000), cur.left(6000), instruction.left(800), QString::number(p.value("scenes").toArray().size()));
    { const QString mk = m_spots ? m_spots->resolveMarket(p.value("market").toString(), QJsonObject(), QString()) : QString();      // the project's market, else the station default
      if (!mk.isEmpty()) prompt += "\n\n" + areamarket::promptContext(m_spots->marketData(mk), m_store->settingJson("spots.markets")); }
    QJsonObject body; body["model"] = AiModels::model(m_store, llm.value("model").toString()); body["temperature"] = 0.5; body["max_tokens"] = 1500;
    body["messages"] = QJsonArray{QJsonObject{{"role", "user"}, {"content", prompt}}};
    const QString url = AiModels::baseUrl(m_store) + "/chat/completions";
    m_http->json("POST", url, {{"Authorization", "Bearer " + key.toUtf8()}, {"X-Title", "PLAY PLAY spot studio"}}, body, 90000,
                 [this, id, actor](int http, const QJsonObject& j, const QByteArray& raw, const QString& e) {
        bool f = false; QJsonObject p = project(id, &f); if (!f) return;
        if (http != 200) { history(p, "writer", "rewrite failed", SuiteHttp::errorText(http, j, raw, e)); save(p); return; }
        const QString content = j.value("choices").toArray().at(0).toObject().value("message").toObject().value("content").toString();
        const int a = content.indexOf('{'), b = content.lastIndexOf('}');
        const QJsonObject d = a >= 0 && b > a ? QJsonDocument::fromJson(content.mid(a, b - a + 1).toUtf8()).object() : QJsonObject();
        const QJsonArray sc = d.value("scenes").toArray();
        if (sc.isEmpty()) { history(p, "writer", "rewrite unusable", "the writer returned no scenes"); save(p); return; }
        QJsonArray merged; const QJsonArray old = p.value("scenes").toArray();
        for (int i = 0; i < sc.size(); ++i) { QJsonObject s = sc[i].toObject(); s["durationSec"] = i < old.size() ? old.at(i).toObject().value("durationSec").toDouble(2) : 2.0; if (i == sc.size() - 1) s["endCard"] = true; merged.append(s); }
        p["scenes"] = suites::balanceScenes(merged, p.value("lengthSec").toInt());
        if (d.value("endCard").isObject()) p["endCard"] = d.value("endCard");
        history(p, "writer", "rewrite", QString("applied (%1 scenes)").arg(merged.size())); save(p);
    });
}

// =====================================================================================================
// render
// =====================================================================================================
QJsonObject SpotStudio::generate(const QString& id, const QJsonObject& b, const QString& actor) {
    bool f = false; QJsonObject p = project(id, &f);
    if (!f) return errObj(404, "no such project");
    const QString step = b.value("step").toString("compose");
    if (step == "script") { redraft(p, b.value("concept").toInt(p.value("concept").toInt(1))); history(p, actor, "redraft"); return save(p); }
    if (p.value("status").toString() == "rendering") return errObj(409, "already rendering");
    if (p.value("status").toString() == "published") return errObj(409, "published; duplicate the project to make a new version");
    const QString engine = b.value("engine").toString(p.value("engine").toString("local"));
    const QString narration = suites::narrationOf(p.value("scenes").toArray(), p.value("endCard").toObject());
    if (narration.trimmed().isEmpty()) return errObj(400, "the script has no narration");
    const int rev = p.value("revision").toInt() + 1;
    QJsonObject render; render["engine"] = engine; render["state"] = "running"; render["pct"] = 0; render["revision"] = rev; render["startedMs"] = double(nowMs());
    const QString title = (p.value("brand").toObject().value("name").toString(p.value("title").toString()) + " — " + p.value("title").toString()).left(120);
    if (engine == "studio") {
        if (!m_farm) return errObj(503, "no render farm");
        QJsonObject job; job["kind"] = "spot"; job["type"] = "motion_spot"; job["project"] = id; job["title"] = title;
        QString brief = QString("%1 spot (%2 s). Use ONLY these facts and this script, keep every name and number exactly:\n").arg(p.value("kind").toString().toUpper()).arg(p.value("lengthSec").toInt());
        int n = 1; for (const QJsonValue& v : p.value("scenes").toArray()) { const QJsonObject s = v.toObject(); brief += QString("Scene %1 (%2 s, %3): VO \"%4\"; on screen \"%5\"; visual: %6\n").arg(n++).arg(s.value("durationSec").toDouble()).arg(s.value("beat").toString(), s.value("vo").toString(), s.value("onScreen").toString(), s.value("visual").toString()); }
        const QJsonObject ec = p.value("endCard").toObject(); brief += QString("End card: %1 / %2 / %3. VO \"%4\"\nFacts: %5").arg(ec.value("headline").toString(), ec.value("cta").toString(), ec.value("url").toString(), ec.value("vo").toString(), p.value("brief").toString());
        { const QString mk = m_spots ? m_spots->resolveMarket(p.value("market").toString(), QJsonObject(), QString()) : QString();
          if (!mk.isEmpty()) brief += "\n" + areamarket::promptContext(m_spots->marketData(mk), m_store->settingJson("spots.markets")); }
        job["brief"] = brief.left(4000);
        if (p.value("template").toString() != "local-card" && !p.value("template").toString().isEmpty()) job["template"] = p.value("template");
        job["lengthSec"] = p.value("lengthSec"); job["aspect"] = p.value("aspect"); job["voice"] = p.value("voice"); if (!p.value("music").toString().isEmpty()) job["music"] = p.value("music");
        QJsonObject brand = p.value("brand").toObject(); QJsonObject sb; for (const char* k : {"name", "cta", "url", "colors"}) if (brand.contains(k) && !(brand.value(k).isString() && brand.value(k).toString().isEmpty())) sb[k] = brand.value(k);
        if (!sb.isEmpty()) job["brand"] = sb;
        job["target"] = b.value("target").toString("auto");
        job["ingest"] = QJsonObject{{"type", p.value("kind").toString() == "ad" ? "ad" : "promo"}, {"title", title}, {"client", p.value("client")}, {"archived", true}, {"status", "review"}, {"category", "studio review"}, {"studioProject", id}};
        const QJsonObject r = m_farm->submit(job, [this, id](bool ok, const QJsonObject& res) { onRendered(id, ok, res, "studio"); });
        if (r.contains("error")) return errObj(r.value("status").toInt(400), r.value("error").toString());
        render["jobId"] = r.value("id");
    } else {
        if (!m_spots) return errObj(503, "no spot generator");
        ensureRecipe();
        QJsonObject over = m_store->settingJson("spots.recipes"); QJsonObject rec = over.value(kRecipe).toObject();
        rec["kicker"] = p.value("brand").toObject().value("name").toString(p.value("kind").toString() == "psa" ? "COMMUNITY" : "SPOT").toUpper().left(28); rec["type"] = p.value("kind").toString() == "ad" ? "ad" : "promo";
        over[kRecipe] = rec; m_store->setSettingJson("spots.recipes", over);
        QJsonObject req; req["recipe"] = kRecipe; req["market"] = p.value("market"); req["data"] = narration; req["voice"] = p.value("voice"); req["targetSec"] = p.value("lengthSec"); req["ttlMin"] = 525600;
        req["meta"] = QJsonObject{{"title", title}, {"status", "review"}, {"category", "studio review"}, {"perishable", false}, {"availableEndMs", 0}, {"client", p.value("client")}, {"studioProject", id}};
        const QJsonObject r = m_spots->generate(req, [this, id](bool ok, const QJsonObject& res) { onRendered(id, ok, res, "local"); });
        if (r.contains("error")) return errObj(r.value("status").toInt(400), r.value("error").toString());
        render["genId"] = r.value("genId");
    }
    p["render"] = render; p["status"] = "rendering"; p["revision"] = rev;
    history(p, actor, "render", engine);
    return save(p);
}

void SpotStudio::onRendered(const QString& id, bool ok, const QJsonObject& res, const QString& engine) {
    bool f = false; QJsonObject p = project(id, &f); if (!f) return;
    m_live.remove(id);
    QJsonObject r = p.value("render").toObject(); r["finishedMs"] = double(nowMs());
    const QString mid = res.value("mediaId").toString();
    if (!ok || mid.isEmpty()) {
        r["state"] = "failed"; r["error"] = res.value("error").toString(ok ? QString("rendered, but no library record appeared") : QString("render failed"));
        p["render"] = r; p["status"] = "failed"; history(p, "render", "failed", r.value("error").toString()); save(p); return;
    }
    MediaRecord m = m_store->media(mid, &f);
    if (f) {
        // out of the spot generator's folder (its expiry sweep deletes autogen files after spots.keepDays) and out of rotation until review
        if (engine == "local") {
            const QString dest = QDir(m_studioDir).filePath(QString("%1_r%2.mp4").arg(id).arg(r.value("revision").toInt()));
            QFile::remove(dest);
            if (QFile::rename(m.path, dest) || (QFile::copy(m.path, dest) && QFile::remove(m.path))) m.path = QDir::toNativeSeparators(dest);
            QJsonObject studio = m.extra.value("autogen").toObject(); m.extra.remove("autogen"); m.extra["studio"] = studio;
        }
        m.archived = true; m.availableEndMs = 0; m.extra["status"] = "review"; m.extra["category"] = "studio review"; m.extra.remove("perishable"); m.extra["studioProject"] = id;
        m_store->saveMedia(m);
    }
    r["state"] = "done"; r["pct"] = 100; r["mediaId"] = mid; r["path"] = m.path; r["durationSec"] = m.durationSec > 0 ? m.durationSec : res.value("durationSec").toDouble();
    if (res.contains("nodeName")) r["node"] = res.value("nodeName");
    p["render"] = r; p["status"] = "rendered"; QJsonObject st = p.value("stale").toObject(); st["compose"] = false; p["stale"] = st;
    QJsonArray versions = p.value("versions").toArray(); versions.append(QJsonObject{{"revision", r.value("revision")}, {"mediaId", mid}, {"engine", engine}, {"ms", double(nowMs())}}); p["versions"] = versions;
    history(p, "render", "rendered", engine + " → " + mid);
    save(p);
}

// =====================================================================================================
// review + publish
// =====================================================================================================
QJsonObject SpotStudio::review(const QString& id, const QString& action, const QString& notes, const QString& actor) {
    bool f = false; QJsonObject p = project(id, &f);
    if (!f) return errObj(404, "no such project");
    const QString st = p.value("status").toString();
    QString to;
    if (action == "submit") { if (st != "rendered" && st != "changes_requested") return errObj(409, "render the spot first (status " + st + ")"); if (p.value("render").toObject().value("mediaId").toString().isEmpty()) return errObj(409, "no rendered media"); to = "in_review"; }
    else if (action == "approve") { if (st != "in_review" && st != "rendered") return errObj(409, "only a spot in review can be approved"); to = "approved"; }
    else if (action == "changes") { if (st != "in_review" && st != "rendered" && st != "approved") return errObj(409, "nothing to send back"); if (notes.trimmed().isEmpty()) return errObj(400, "say what should change"); to = "changes_requested"; }
    else if (action == "reject") { if (st == "published") return errObj(409, "already published"); to = "rejected"; }
    else return errObj(400, "action must be submit, approve, changes or reject");
    QJsonObject rv = p.value("review").toObject(); QJsonArray h = rv.value("history").toArray();
    h.append(QJsonObject{{"ms", double(nowMs())}, {"actor", actor}, {"action", action}, {"notes", notes}}); rv["history"] = h; if (!notes.isEmpty()) rv["notes"] = notes; rv["last"] = action;
    p["review"] = rv; p["status"] = to; history(p, actor, "review:" + action, notes);
    return save(p);
}

QJsonObject SpotStudio::publish(const QString& id, const QJsonObject& b, const QString& actor) {
    bool f = false; QJsonObject p = project(id, &f);
    if (!f) return errObj(404, "no such project");
    if (p.value("status").toString() != "approved") return errObj(409, "only an approved spot can be published (status " + p.value("status").toString() + ")");
    const QString mid = p.value("render").toObject().value("mediaId").toString();
    MediaRecord m = m_store->media(mid, &f);
    if (!f) return errObj(409, "the rendered media is gone from the library");
    const QString type = QStringList{"ad", "promo", "psa"}.contains(b.value("type").toString()) ? b.value("type").toString() : (p.value("kind").toString() == "ad" ? "ad" : p.value("kind").toString() == "psa" ? "psa" : "promo");
    const QDate start = QDate::fromString(b.value("startDate").toString(), Qt::ISODate), end = QDate::fromString(b.value("endDate").toString(), Qt::ISODate);
    if (start.isValid() && end.isValid() && end < start) return errObj(400, "endDate is before startDate");
    QStringList channels; for (const QJsonValue& v : b.value("channels").toArray()) if (!v.toString().isEmpty()) channels << v.toString();
    m.type = type; m.archived = false; m.client = b.value("client").toString(p.value("client").toString()); m.title = b.value("title").toString(m.title);
    m.allowedChannels = channels;
    m.availableStartMs = start.isValid() ? start.startOfDay().toMSecsSinceEpoch() : 0;
    m.availableEndMs = end.isValid() ? end.endOfDay().toMSecsSinceEpoch() : 0;
    m.extra["status"] = "approved"; m.extra["category"] = type == "ad" ? "ad rotation" : type == "psa" ? "psa rotation" : "promo rotation";
    m.extra["studioProject"] = id; m.extra["publishedMs"] = double(nowMs());
    m_store->saveMedia(m); m_store->mediaChanged(m.id);
    // ad contract: add to an existing one or create one, so the ad-break builder / format clock rotate it
    QString contractId = b.value("contractId").toString();
    if (type == "ad") {
        QList<AdContract> cs = FormatClock::contractsFromSettings(m_store->settingJson("traffic.contracts"));
        bool hit = false;
        if (!contractId.isEmpty()) { for (AdContract& c : cs) if (c.id == contractId) { if (!c.mediaIds.contains(mid)) c.mediaIds << mid; hit = true; } if (!hit) return errObj(404, "no such contract " + contractId); }
        else if (b.value("contract").isObject()) {
            const QJsonObject in = b.value("contract").toObject(); AdContract c;
            c.id = "ct-" + QUuid::createUuid().toString(QUuid::Id128).left(6); c.client = m.client.isEmpty() ? p.value("title").toString() : m.client; c.title = in.value("title").toString(p.value("title").toString());
            c.mediaIds << mid; c.spotsTotal = in.value("spotsTotal").toInt(); c.spotsPerDay = in.value("spotsPerDay").toInt(4); c.startDate = start.isValid() ? start : QDate::currentDate();
            c.endDate = end.isValid() ? end : c.startDate.addDays(30); c.channels = channels; c.dayparts = in.value("dayparts").toArray(); c.priority = in.value("priority").toInt(); c.status = "active";
            cs.append(c); contractId = c.id; hit = true;
        }
        if (hit) m_store->setSettingJson("traffic.contracts", FormatClock::contractsToSettings(cs));
    }
    QJsonObject pubd; pubd["mediaId"] = mid; pubd["type"] = type; pubd["contractId"] = contractId; pubd["ms"] = double(nowMs()); pubd["channels"] = QJsonArray::fromStringList(channels);
    if (b.value("social").toObject().value("queue").toBool() && m_pub) {
        QJsonObject d; d["kind"] = "video"; d["mediaId"] = mid; d["title"] = m.title; d["caption"] = b.value("social").toObject().value("message").toString(p.value("brief").toString().left(400));
        d["source"] = "spot_studio"; d["channels"] = QJsonArray{"facebook"}; d["ai"] = QJsonObject{{"voice", true}};
        const QJsonObject it = m_pub->draft(d, actor); pubd["socialItem"] = it.value("id"); if (it.contains("error")) pubd["socialError"] = it.value("error");
    }
    p["published"] = pubd; p["status"] = "published"; history(p, actor, "publish", type + (contractId.isEmpty() ? QString() : " · contract " + contractId));
    return save(p);
}

QJsonObject SpotStudio::reviseAutoSpot(const QString& mediaId, const QString& notes, const QString& voice) {
    if (!m_spots) return errObj(503, "no spot generator");
    if (notes.trimmed().isEmpty()) return errObj(400, "say what should change");
    const QJsonObject info = m_spots->spotInfo(mediaId);
    if (info.contains("error")) return errObj(404, info.value("error").toString());
    const QJsonObject ag = info.value("autogen").toObject();
    if (ag.value("recipe").toString().isEmpty()) return errObj(409, "not an auto-generated spot (no recipe recorded)");
    QJsonObject req; req["recipe"] = ag.value("recipe"); req["notes"] = notes; req["jobId"] = ag.value("jobId");
    const QJsonArray ac = info.value("allowedChannels").toArray(); if (!ac.isEmpty()) req["channel"] = ac.first();
    if (!voice.isEmpty()) req["voice"] = voice;
    QJsonObject m; m["autogen"] = QJsonObject{{"revisionOf", mediaId}}; m["revisionOf"] = mediaId; req["meta"] = m;
    QJsonObject r = m_spots->generate(req, SpotGenerator::Done());
    if (r.contains("error")) return r;
    r["revisionOf"] = mediaId; r["recipe"] = ag.value("recipe");
    r["note"] = AiModels::apiKey(m_store).isEmpty() ? "no LLM writer is configured: the spot is regenerated from fresh material with the note recorded" : "the writer applies the note";
    return r;
}
