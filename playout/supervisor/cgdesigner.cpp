// CgDesigner — store, page files, manifest merge and AI jobs of the CG Designer (docs/cg-designer.md).
#include "cgdesigner.h"
#include "cgdesign.h"
#include "aimodels.h"
#include "suitehttp.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDebug>
#include <algorithm>

using Q = QHash<QString, QString>;
QPointer<CgDesigner> CgDesigner::s_instance;

namespace {
QJsonObject errObj(int status, const QString& m) { QJsonObject e; e["error"] = m; e["status"] = status; return e; }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
const int kVersions = 10;
const int kJobs = 20;
QString recKey(const QString& id) { return "cgdesigner.t." + id; }
QString pick(const QString& v, const QStringList& allowed, const QString& def) { const QString t = v.trimmed().toLower(); return allowed.contains(t) ? t : def; }
QString roleOf(const QString& v) {
    const QString t = v.trimmed().toLower().replace('-', '_').replace(' ', '_');
    if (cgdesign::roles().contains(t)) return t;
    if (t.contains("lower") || t == "strap" || t == "lt") return "lower_third";
    if (t.contains("bug") || t.contains("logo") || t.contains("clock")) return "bug";
    if (t.contains("ticker") || t.contains("crawl")) return "ticker";
    if (t.contains("legal") || t == "id") return "legal_id";
    if (t.contains("emerg") || t.contains("alert")) return "emergency";
    return "secondary";
}
QString categoryOf(const QString& v) { return pick(v, cgdesign::categories(), "custom"); }
}

CgDesigner::CgDesigner(sched::ScheduleStore* store, const QString& webRoot, std::function<QJsonObject()> manifest, QObject* parent)
    : QObject(parent), m_store(store), m_webRoot(webRoot), m_manifest(std::move(manifest)), m_http(new SuiteHttp(this)) {
    s_instance = this;
    for (const QJsonValue& v : m_store->settingJson("cgdesigner.index").value("ids").toArray()) {
        const QString id = v.toString();
        if (!cgdesign::validId(id) || m_recs.contains(id)) continue;
        const QJsonObject r = m_store->settingJson(recKey(id));
        if (r.value("id").toString() != id || r.value("html").toString().isEmpty()) continue;
        m_recs.insert(id, r); m_order << id;
    }
}

CgDesigner::~CgDesigner() { if (s_instance == this) s_instance = nullptr; }

CgDesigner* CgDesigner::instance() { return s_instance.data(); }

void CgDesigner::start() {
    int written = 0;
    for (const QString& id : m_order) {
        const QString html = m_recs.value(id).value("html").toString();
        QFile f(pagePath(id));
        if (f.open(QIODevice::ReadOnly) && QString::fromUtf8(f.readAll()) == html) continue;
        f.close();
        QString err;
        if (writePage(id, html, &err)) ++written; else qWarning().noquote() << "[CgDesigner]" << id << err;
    }
    qInfo().noquote() << QString("[CgDesigner] %1 designed template(s), %2 page(s) written to %3").arg(m_order.size()).arg(written).arg(QDir(m_webRoot).filePath("cg"));
}

// =====================================================================================================
// manifest
// =====================================================================================================
QJsonObject CgDesigner::mergeInto(const QJsonObject& manifest) {
    CgDesigner* d = instance();
    if (!d || d->m_order.isEmpty()) return manifest;
    return d->manifestEntries(manifest);
}

QJsonObject CgDesigner::manifestEntries(const QJsonObject& manifest) const {
    QJsonObject m = manifest;
    QJsonArray t = m.value("templates").toArray();
    for (const QString& id : m_order) {
        const QJsonObject r = m_recs.value(id);
        QJsonObject e;
        e["id"] = id; e["file"] = id + ".html"; e["name"] = r.value("name").toString(id);
        e["category"] = "custom"; e["designCategory"] = r.value("category").toString("custom");
        e["icon"] = QString::fromUtf8("\xE2\x9C\xA8");
        e["description"] = r.value("description").toString();
        e["params"] = r.value("params").toArray(); e["textFields"] = r.value("textFields").toArray();
        e["role"] = r.value("role").toString("secondary");
        e["listed"] = true; e["custom"] = true; e["updatedMs"] = r.value("updatedMs");
        t.append(e);
    }
    m["templates"] = t;
    return m;
}

// =====================================================================================================
// records
// =====================================================================================================
QString CgDesigner::pagePath(const QString& id) const { return QDir(m_webRoot).filePath("cg/" + id + ".html"); }

bool CgDesigner::writePage(const QString& id, const QString& html, QString* err) const {
    if (m_webRoot.isEmpty()) { if (err) *err = "no web root"; return false; }
    QDir().mkpath(QDir(m_webRoot).filePath("cg"));
    QSaveFile f(pagePath(id));
    if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
    f.write(html.toUtf8());
    if (!f.commit()) { if (err) *err = f.errorString(); return false; }
    return true;
}

void CgDesigner::persist(const QJsonObject& rec) { m_store->setSettingJson(recKey(rec.value("id").toString()), rec); }
void CgDesigner::persistIndex() { QJsonArray a; for (const QString& id : m_order) a.append(id); m_store->setSettingJson("cgdesigner.index", QJsonObject{{"ids", a}}); }

QJsonObject CgDesigner::summary(const QJsonObject& rec) const {
    QJsonObject s = rec;
    s.remove("html"); s.remove("versions");
    s["versionCount"] = rec.value("versions").toArray().size();
    s["url"] = "/cg/" + rec.value("id").toString() + ".html";
    return s;
}

QJsonObject CgDesigner::config() const {
    QJsonObject c = m_store->settingJson("cgdesigner.config");
    if (!c.contains("model")) c["model"] = "";
    if (!c.contains("temperature")) c["temperature"] = 0.7;
    if (!c.contains("maxTokens")) c["maxTokens"] = 16000;
    if (!c.contains("timeoutSec")) c["timeoutSec"] = 240;
    if (!c.contains("houseStyle")) c["houseStyle"] = "";
    c["effectiveModel"] = AiModels::model(m_store, c.value("model").toString());
    return c;
}

QJsonObject CgDesigner::setConfig(const QJsonObject& b) {
    QJsonObject c = m_store->settingJson("cgdesigner.config");
    if (b.contains("model")) c["model"] = b.value("model").toString().trimmed();
    if (b.contains("temperature")) c["temperature"] = qBound(0.0, b.value("temperature").toDouble(0.7), 1.5);
    if (b.contains("maxTokens")) c["maxTokens"] = qBound(1000, b.value("maxTokens").toInt(16000), 64000);
    if (b.contains("timeoutSec")) c["timeoutSec"] = qBound(30, b.value("timeoutSec").toInt(240), 600);
    if (b.contains("houseStyle")) {
        const QString hs = b.value("houseStyle").toString().trimmed();
        if (hs.size() > 4000) return errObj(400, "houseStyle is longer than 4,000 characters");
        c["houseStyle"] = hs;
    }
    c.remove("effectiveModel");
    m_store->setSettingJson("cgdesigner.config", c);
    return config();
}

QJsonObject CgDesigner::overview() const {
    QJsonObject o;
    const QJsonObject cfg = config();
    QJsonObject ai = AiModels::keyStatus(m_store);
    ai["model"] = cfg.value("effectiveModel");
    o["ai"] = ai;
    o["config"] = cfg;
    QList<QJsonObject> list; for (const QString& id : m_order) list << summary(m_recs.value(id));
    std::sort(list.begin(), list.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("updatedMs").toDouble() > b.value("updatedMs").toDouble(); });
    QJsonArray t; for (const QJsonObject& s : list) t.append(s);
    o["templates"] = t;
    QJsonArray pack;
    if (m_manifest) for (const QJsonValue& v : m_manifest().value("templates").toArray()) {
        const QJsonObject x = v.toObject();
        if (x.value("custom").toBool() || x.value("listed").toBool(true) == false) continue;
        pack.append(QJsonObject{{"id", x.value("id")}, {"name", x.value("name")}, {"category", x.value("category")}, {"file", x.value("file")},
                                {"role", x.value("role")}, {"icon", x.value("icon")}, {"description", x.value("description")}});
    }
    o["pack"] = pack;
    o["categories"] = QJsonArray::fromStringList(cgdesign::categories());
    o["roles"] = QJsonArray::fromStringList(cgdesign::roles());
    return o;
}

QJsonObject CgDesigner::get(const QString& id) const {
    if (!m_recs.contains(id)) return errObj(404, "no designed template " + id);
    QJsonObject r = m_recs.value(id);
    r["url"] = "/cg/" + id + ".html";
    return r;
}

QJsonObject CgDesigner::save(const QJsonObject& b) {
    QString html = b.value("html").toString();
    if (html.trimmed().isEmpty()) return errObj(400, "html required");
    if (html.size() > 2000000) return errObj(413, "the page is larger than 2 MB (embedded images?): use image fields with a URL instead");
    QString id = b.value("id").toString().trimmed();
    const bool exists = !id.isEmpty() && m_recs.contains(id);
    if (!id.isEmpty() && !exists && !cgdesign::validId(id)) return errObj(400, "invalid id (cgd_ followed by a-z, 0-9, _)");
    QString name = b.value("name").toString().trimmed();
    if (name.isEmpty()) name = exists ? m_recs.value(id).value("name").toString() : QString();
    if (name.isEmpty()) name = "Untitled graphic";
    if (id.isEmpty()) id = cgdesign::makeId(name, m_order);

    QStringList warnings;
    html = cgdesign::normalizeHtml(html, &warnings);
    // labels / defaults the caller (or the model) gave; without any, the ones this design already had
    const QJsonArray params = cgdesign::deriveParams(html, b.contains("params") ? b.value("params").toArray() : m_recs.value(id).value("params").toArray());
    if (params.isEmpty()) warnings << "no data-field elements or :root variables: an operator cannot change anything in this graphic";

    const qint64 now = nowMs();
    QJsonObject r = exists ? m_recs.value(id) : QJsonObject{{"id", id}, {"createdMs", double(now)}};
    if (exists && r.value("html").toString() != html) {
        QJsonArray vs = r.value("versions").toArray();
        const int n = r.value("versionSeq").toInt() + 1;
        vs.append(QJsonObject{{"n", n}, {"html", r.value("html")}, {"savedMs", r.value("updatedMs")}, {"note", r.value("note")}, {"name", r.value("name")}});
        while (vs.size() > kVersions) vs.removeFirst();
        r["versions"] = vs; r["versionSeq"] = n;
    }
    r["name"] = name.left(120);
    if (b.contains("description") || !exists) r["description"] = b.value("description").toString().left(400);
    if (b.contains("category") || !exists) r["category"] = categoryOf(b.value("category").toString());
    if (b.contains("role") || !exists) r["role"] = roleOf(b.value("role").toString());
    if (b.contains("prompt")) r["prompt"] = b.value("prompt").toString().left(8000);
    if (b.contains("examples")) {                    // what the design was made from (images are not kept: too large)
        QJsonArray ex;
        for (const QJsonValue& v : b.value("examples").toArray()) {
            const QJsonObject e = v.toObject();
            QJsonObject k{{"kind", e.value("kind")}};
            if (e.contains("id")) k["id"] = e.value("id");
            if (e.contains("note")) k["note"] = e.value("note").toString().left(400);
            if (e.value("kind").toString() == "html") k["chars"] = e.value("html").toString().size();
            ex.append(k);
        }
        r["examples"] = ex;
    }
    if (b.contains("model")) r["model"] = b.value("model").toString();
    r["note"] = b.value("note").toString().left(400);
    r["html"] = html;
    r["params"] = params;
    r["textFields"] = cgdesign::textFields(params);
    r["updatedMs"] = double(now);

    QString err;
    if (!writePage(id, html, &err)) return errObj(500, "could not write the page " + pagePath(id) + ": " + err);
    m_recs.insert(id, r);
    if (!m_order.contains(id)) { m_order << id; persistIndex(); }
    persist(r);
    emit changed(QJsonObject{{"kind", "saved"}, {"id", id}});
    QJsonObject out = r;
    out["url"] = "/cg/" + id + ".html";
    out["created"] = !exists;
    out["warnings"] = QJsonArray::fromStringList(warnings);
    return out;
}

QJsonObject CgDesigner::remove(const QString& id) {
    if (!m_recs.contains(id)) return errObj(404, "no designed template " + id);
    m_recs.remove(id); m_order.removeAll(id);
    persistIndex();
    m_store->setSetting(recKey(id), QString());
    QFile::remove(pagePath(id));
    emit changed(QJsonObject{{"kind", "removed"}, {"id", id}});
    return QJsonObject{{"removed", id}};
}

QJsonObject CgDesigner::restore(const QString& id, const QJsonObject& b) {
    if (!m_recs.contains(id)) return errObj(404, "no designed template " + id);
    const int n = b.value("n").toInt(-1);
    const QJsonObject r = m_recs.value(id);
    for (const QJsonValue& v : r.value("versions").toArray()) {
        const QJsonObject ver = v.toObject();
        if (ver.value("n").toInt() != n) continue;
        QJsonObject body{{"id", id}, {"html", ver.value("html")}, {"note", QString("restored version %1").arg(n)}};
        return save(body);
    }
    return errObj(404, QString("%1 has no version %2").arg(id).arg(n));
}

// =====================================================================================================
// generate
// =====================================================================================================
QString CgDesigner::exampleHtml(const QString& templateId) const {
    QString html;
    if (m_recs.contains(templateId)) html = m_recs.value(templateId).value("html").toString();
    else if (m_manifest) {
        for (const QJsonValue& v : m_manifest().value("templates").toArray()) {
            const QJsonObject t = v.toObject();
            if (t.value("id").toString() != templateId) continue;
            const QString file = t.value("file").toString();
            if (file.contains("..") || file.contains('/') || file.contains('\\')) break;
            QFile f(QDir(m_webRoot).filePath("cg/" + file));
            if (f.open(QIODevice::ReadOnly)) html = QString::fromUtf8(f.readAll());
            break;
        }
    }
    static const QRegularExpression bigData("data:[a-zA-Z/+.-]+;base64,[A-Za-z0-9+/=]{200,}");
    html.replace(bigData, "data:image/png;base64,(omitted)");
    return html;
}

QJsonObject CgDesigner::generate(const QJsonObject& b) {
    const bool dryRun = b.value("dryRun").toBool();
    cgdesign::Request rq;
    rq.prompt = b.value("prompt").toString().trimmed();
    rq.mode = b.value("mode").toString("new") == "refine" ? "refine" : "new";
    rq.baseHtml = b.value("baseHtml").toString();
    rq.kind = b.value("kind").toString().left(200);
    rq.brand = b.value("brand").toObject();
    if (rq.prompt.isEmpty()) return errObj(400, rq.mode == "refine" ? "instruction required (what should change?)" : "prompt required (describe the graphic)");
    if (rq.prompt.size() > 12000) return errObj(400, "prompt longer than 12,000 characters");
    if (rq.mode == "refine" && rq.baseHtml.trimmed().isEmpty()) return errObj(400, "refine needs baseHtml (the current page)");
    const QJsonArray exs = b.value("examples").toArray();
    if (exs.size() > 4) return errObj(400, "at most 4 examples");
    int images = 0;
    for (const QJsonValue& v : exs) {
        const QJsonObject e = v.toObject();
        cgdesign::Example x; x.kind = e.value("kind").toString(); x.note = e.value("note").toString().left(600);
        if (x.kind == "template") {
            x.name = e.value("id").toString();
            x.html = exampleHtml(x.name);
            if (x.html.isEmpty()) return errObj(400, "unknown example template " + x.name);
        } else if (x.kind == "html") {
            x.name = e.value("name").toString("pasted HTML");
            x.html = e.value("html").toString();
            if (x.html.trimmed().isEmpty()) return errObj(400, "an html example is empty");
        } else if (x.kind == "image") {
            x.dataUrl = e.value("dataUrl").toString();
            if (!x.dataUrl.startsWith("data:image/")) return errObj(400, "an image example must be a data:image/... URL");
            if (x.dataUrl.size() > 8 * 1024 * 1024) return errObj(413, "an example image is larger than 6 MB: use a smaller screenshot");
            if (++images > 3) return errObj(400, "at most 3 example images");
        } else return errObj(400, "example kind must be template, html or image");
        rq.examples << x;
    }
    const QJsonObject cfg = config();
    const QString model = AiModels::model(m_store, b.value("model").toString().trimmed().isEmpty() ? cfg.value("model").toString() : b.value("model").toString());
    const int count = qBound(1, b.value("count").toInt(1), 3);
    double temp = cfg.value("temperature").toDouble(0.7);
    if (rq.mode == "refine") temp = qMin(temp, 0.4);
    else if (count > 1) temp = qMin(1.0, temp + 0.15);
    const int maxTokens = cfg.value("maxTokens").toInt(16000);
    const int timeoutMs = cfg.value("timeoutSec").toInt(240) * 1000;
    rq.houseStyle = cfg.value("houseStyle").toString();

    if (dryRun) {        // "View full prompt": exactly what would be sent (variation 1), images shown as their size; no call, no key needed
        cgdesign::Request r = rq; r.variations = count;
        QJsonObject o = cgdesign::redactImages(cgdesign::chatBody(r, model, temp, maxTokens));
        o["dryRun"] = true; o["maxTokens"] = maxTokens; o["count"] = count;
        o["hasKey"] = !AiModels::apiKey(m_store).isEmpty();
        o["url"] = AiModels::baseUrl(m_store) + "/chat/completions";
        if (count > 1) o["note"] = QString("variation 1 of %1 shown; the others differ only in the variation line").arg(count);
        return o;
    }
    const QString key = AiModels::apiKey(m_store);
    if (key.isEmpty()) return errObj(409, "no OpenRouter key: set the OPENROUTER_API_KEY environment variable (or save one under AI settings)");

    // the job table keeps the last kJobs
    while (m_jobs.size() >= kJobs && !m_jobOrder.isEmpty()) {
        auto it = m_jobOrder.begin();
        bool dropped = false;
        for (; it != m_jobOrder.end(); ++it) if (m_jobs.value(it.value()).state != "running") { m_jobs.remove(it.value()); m_jobOrder.erase(it); dropped = true; break; }
        if (!dropped) return errObj(429, "too many designs are being generated at once; wait for one to finish");
    }
    Job j;
    j.id = QString("cgj%1_%2").arg(nowMs(), 0, 36).arg(++m_jobSeq);
    j.model = model; j.mode = rq.mode; j.startedMs = nowMs(); j.pending = j.total = count;
    m_jobs.insert(j.id, j); m_jobOrder.insert(m_jobSeq, j.id);

    const QString url = AiModels::baseUrl(m_store) + "/chat/completions";
    const QString jid = j.id;
    for (int v = 1; v <= count; ++v) {
        cgdesign::Request r = rq; r.variation = v; r.variations = count;
        const QJsonObject body = cgdesign::chatBody(r, model, temp, maxTokens);
        m_http->json("POST", url, {{"Authorization", "Bearer " + key.toUtf8()}, {"HTTP-Referer", "https://playplay.local"}, {"X-Title", "PLAY PLAY CG designer"}}, body, timeoutMs,
                     [this, jid, v](int http, const QJsonObject& resp, const QByteArray& raw, const QString& e) { finishDraft(jid, v, http, resp, raw, e); });
    }
    qInfo().noquote() << QString("[CgDesigner] job %1: %2 x%3 with %4 (%5 example(s))").arg(jid, rq.mode).arg(count).arg(model).arg(rq.examples.size());
    emit changed(QJsonObject{{"kind", "job"}, {"id", jid}, {"state", "running"}});
    return QJsonObject{{"jobId", jid}, {"state", "running"}, {"model", model}, {"count", count}};
}

void CgDesigner::finishDraft(const QString& jobId, int variation, int http, const QJsonObject& resp, const QByteArray& raw, const QString& netErr) {
    if (!m_jobs.contains(jobId)) return;
    Job& j = m_jobs[jobId];
    if (http != 200) {
        j.errors.append(QJsonObject{{"variation", variation}, {"error", SuiteHttp::errorText(http, resp, raw, netErr)}});
    } else {
        const QJsonObject choice = resp.value("choices").toArray().at(0).toObject();
        const QJsonValue cv = choice.value("message").toObject().value("content");
        QString content;
        if (cv.isString()) content = cv.toString();
        else for (const QJsonValue& p : cv.toArray()) content += p.toObject().value("text").toString();
        const cgdesign::Reply rep = cgdesign::parseModelReply(content);
        if (!rep.error.isEmpty()) {
            QString why = rep.error;
            if (choice.value("finish_reason").toString() == "length") why += " (finish_reason length: raise cgdesigner.config maxTokens)";
            j.errors.append(QJsonObject{{"variation", variation}, {"error", why}, {"raw", content.left(600)}});
        } else {
            QStringList warnings;
            const QString html = cgdesign::normalizeHtml(rep.html, &warnings);
            const QJsonArray params = cgdesign::deriveParams(html, rep.meta.value("params").toArray());
            if (params.isEmpty()) warnings << "the page has no data-field elements or :root variables";
            QJsonObject meta;
            meta["name"] = rep.meta.value("name").toString("Generated graphic").left(120);
            meta["description"] = rep.meta.value("description").toString().left(400);
            meta["category"] = categoryOf(rep.meta.value("category").toString());
            meta["role"] = roleOf(rep.meta.value("role").toString());
            j.drafts.append(QJsonObject{{"variation", variation}, {"meta", meta}, {"html", html}, {"params", params}, {"textFields", cgdesign::textFields(params)},
                                        {"warnings", QJsonArray::fromStringList(warnings)}, {"finishReason", choice.value("finish_reason")}});
        }
        const QJsonObject u = resp.value("usage").toObject();
        for (const QString& k : {QString("prompt_tokens"), QString("completion_tokens"), QString("total_tokens"), QString("cost")})
            if (u.contains(k)) j.usage[k] = j.usage.value(k).toDouble() + u.value(k).toDouble();
    }
    if (--j.pending > 0) return;
    QList<QJsonObject> ds; for (const QJsonValue& v : j.drafts) ds << v.toObject();
    std::sort(ds.begin(), ds.end(), [](const QJsonObject& a, const QJsonObject& b) { return a.value("variation").toInt() < b.value("variation").toInt(); });
    QJsonArray sorted; for (const QJsonObject& d : ds) sorted.append(d);
    j.drafts = sorted;
    j.endedMs = nowMs();
    j.state = j.drafts.isEmpty() ? "failed" : "done";
    if (j.drafts.isEmpty()) { QStringList e; for (const QJsonValue& v : j.errors) e << v.toObject().value("error").toString(); j.error = e.join("; "); }
    qInfo().noquote() << QString("[CgDesigner] job %1 %2 in %3 s: %4 draft(s)%5").arg(jobId, j.state).arg((j.endedMs - j.startedMs) / 1000.0, 0, 'f', 1).arg(j.drafts.size()).arg(j.error.isEmpty() ? QString() : " - " + j.error.left(200));
    emit changed(QJsonObject{{"kind", "job"}, {"id", jobId}, {"state", j.state}});
}

QJsonObject CgDesigner::job(const QString& id) const {
    if (!m_jobs.contains(id)) return errObj(404, "no job " + id + " (only the last 20 are kept)");
    const Job& j = m_jobs.value(id);
    QJsonObject o;
    o["id"] = j.id; o["state"] = j.state; o["mode"] = j.mode; o["model"] = j.model;
    o["startedMs"] = double(j.startedMs); o["ms"] = double((j.endedMs ? j.endedMs : nowMs()) - j.startedMs);
    o["total"] = j.total; o["pending"] = j.pending;
    o["drafts"] = j.drafts; o["errors"] = j.errors;
    if (!j.error.isEmpty()) o["error"] = j.error;     // never with "status": a 200 reply must not look like an error
    o["usage"] = j.usage;
    return o;
}

// =====================================================================================================
// routes
// =====================================================================================================
void CgDesigner::registerRoutes(RestApiServer* r) {
    r->registerRoute("GET", "/api/v1/cg/designer", [this](const QJsonObject&, const Q&) { return overview(); });
    r->registerRoute("GET", "/api/v1/cg/designer/config", [this](const QJsonObject&, const Q&) { return config(); });
    r->registerRoute("PUT", "/api/v1/cg/designer/config", [this](const QJsonObject& b, const Q&) { return setConfig(b); });
    r->registerRoute("GET", "/api/v1/cg/designer/templates", [this](const QJsonObject&, const Q&) { return QJsonObject{{"templates", overview().value("templates")}}; });
    r->registerRoute("POST", "/api/v1/cg/designer/templates", [this](const QJsonObject& b, const Q&) { return save(b); });
    r->registerRoute("GET", "/api/v1/cg/designer/templates/{id}", [this](const QJsonObject&, const Q& q) { return get(q.value("id")); });
    r->registerRoute("DELETE", "/api/v1/cg/designer/templates/{id}", [this](const QJsonObject&, const Q& q) { return remove(q.value("id")); });
    r->registerRoute("POST", "/api/v1/cg/designer/templates/{id}/restore", [this](const QJsonObject& b, const Q& q) { return restore(q.value("id"), b); });
    r->registerRoute("POST", "/api/v1/cg/designer/generate", [this](const QJsonObject& b, const Q&) { return generate(b); });
    r->registerRoute("GET", "/api/v1/cg/designer/jobs/{id}", [this](const QJsonObject&, const Q& q) { return job(q.value("id")); });
}
