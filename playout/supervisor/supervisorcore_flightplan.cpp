// SupervisorCore — the flight plan (Operate / Plan views): now/next/later, time debt, corrections, ghost preview,
// "why", readiness and the pre-air readiness watcher. Pure logic is in playout/schedule/flightplan.*.
//
//   GET  /api/v1/flightplan/{ch}                    analysis (now, next, later, anchors, intervals, readiness, protected boundary)
//   GET  /api/v1/flightplan/{ch}/corrections?interval=N   options + best plan for an over / under interval
//   POST /api/v1/flightplan/{ch}/preview {ops[]}     the ghost: what the ops would do (nothing is saved)
//   POST /api/v1/flightplan/{ch}/apply   {ops[], reason, who}   saves the edited hours as dated overrides, audited
//   GET  /api/v1/flightplan/{ch}/why?id=<item id>    reasons + readiness for one event
//   GET  /api/v1/flightplan/{ch}/audit               who changed what, when, and why
//   GET  /api/v1/readiness                           pre-air readiness of every armed channel's next hours
#include "supervisorcore.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"
#include "../schedule/flightplan.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSet>
#include <QStorageInfo>
#include <QTimer>
#include <QDebug>

using namespace sched;
using Q = QHash<QString, QString>;

namespace {

FlightOptions flightOptions(SupervisorCore* /*core*/, ScheduleStore* store, ScheduleExecutor* exec, const QString& ch, const Q& q) {
    FlightOptions o; o.channelId = ch; o.nowMs = exec->clockNowMs();
    const ChannelProfile p = store->channel(ch); o.tz = p.tz();
    if (q.contains("protectSec")) o.protectSec = qBound(0.0, q.value("protectSec").toDouble(), 3600.0);
    else { const double v = store->settingJson("flightplan.config").value("protectSec").toDouble(-1.0); if (v >= 0.0) o.protectSec = v; }
    o.media.byId = [store](const QString& id, bool* f) { return store->media(id, f); };
    o.media.byTypes = [store](const QStringList& t) { return store->mediaByType(t); };
    o.media.all = [store]() { return store->allMedia(false); };
    return o;
}

/// The hours the executor has built; with no engine connected nothing is built, so the current and next hour are laid out from the
/// stored schedule instead (the plan is useful, and testable, without an engine).
QList<Runlist> plannedHours(ScheduleStore* store, ScheduleExecutor* exec, const QString& ch) {
    QList<Runlist> built = exec->builtRunlists(ch);
    if (!built.isEmpty()) return built;
    const ChannelProfile p = store->channel(ch); const qint64 now = exec->clockNowMs();
    const qint64 h0 = RunlistBuilder::hourStartUtcMsForTime(now, p.tz());
    for (int k = 0; k < 2; ++k) {
        const qint64 hs = h0 + k * 3600000LL; QDate d; int h = 0; RunlistBuilder::hourStartUtcMsForTime(hs, p.tz(), &d, &h);
        const HourSlot s = store->effectiveSlot(ch, d, h);
        if (!s.items.isEmpty()) built << exec->previewSlot(ch, s, hs);
    }
    return built;
}

/// The stored hour behind a runlist (dated override, else the weekly template), or — for a synthesized hour with nothing stored — the
/// runlist's own items, so a correction can still be applied (and saved as a dated override).
HourSlot editableSlot(ScheduleStore* store, const ChannelProfile& p, const Runlist& rl, QString* how) {
    QDate d; int h = 0; RunlistBuilder::hourStartUtcMsForTime(rl.hourStartMs, p.tz(), &d, &h);
    HourSlot s = store->effectiveSlot(p.id, d, h);
    if (!s.items.isEmpty()) { if (how) *how = "stored"; return s; }
    HourSlot g; g.channelId = p.id;
    for (const RunlistItem& r : rl.items) {
        if (r.origin == "fill" || r.origin == "emergency") continue;
        SlotItem it = r.item; it.queryBlock = QJsonObject(); it.folderBlock = QJsonObject();
        if (r.hard && !it.isHardStart()) { it.timingMode = "soft_start"; }
        g.items << it;
    }
    if (how) *how = "frozen from the build";
    return g;
}

/// Slot ids of a saved override: dated, with the date set.
void asDated(HourSlot& s, const QDate& d, int h, const QString& ch) {
    s.channelId = ch; s.date = d; s.dayOfWeek = -1; s.hour = h; s.id = HourSlot::dateId(d, h);
}

struct Rebuilt { QList<Runlist> lists; QJsonArray log, errors; QList<HourSlot> edited; QList<QPair<QDate, int>> keys; bool any = false; };

Rebuilt applyToHours(ScheduleStore* store, ScheduleExecutor* exec, const QString& ch, const QJsonArray& ops, const FlightOptions& fo) {
    Rebuilt out; const QList<Runlist> before = plannedHours(store, exec, ch); const ChannelProfile p = store->channel(ch);
    QHash<QString, QStringList> picks = exec->pickCacheCopy(ch);
    QSet<int> claimed;        // ops that belong to a built hour; the rest name events that do not exist
    for (const Runlist& rl : before) {
        // the ops that belong to this hour: named by hourStartMs, else by an event it contains
        QJsonArray mine;
        for (int oi = 0; oi < ops.size(); ++oi) {
            const QJsonObject op = ops[oi].toObject();
            bool belongs = op.contains("hourStartMs") ? qint64(op.value("hourStartMs").toDouble()) == rl.hourStartMs : false;
            if (!op.contains("hourStartMs")) for (const RunlistItem& r : rl.items) if (r.item.id == op.value("itemId").toString()) belongs = true;
            if (belongs) { mine.append(op); claimed.insert(oi); }
        }
        if (mine.isEmpty()) { out.lists << rl; continue; }
        QString how; HourSlot slot = editableSlot(store, p, rl, &how);
        QStringList log, errors;
        const bool ok = FlightPlan::applyOps(slot, mine, rl, fo, &log, &errors);
        for (const QString& e : errors) out.errors.append(e);
        if (!ok) { out.lists << rl; continue; }
        for (const QString& l : log) out.log.append(l);
        if (how != "stored") out.log.append(QString("hour %1 was generated by autofill: its current content is saved as a dated override").arg(QDateTime::fromMSecsSinceEpoch(rl.hourStartMs, p.tz()).toString("h:mm AP")));
        QDate d; int h = 0; RunlistBuilder::hourStartUtcMsForTime(rl.hourStartMs, p.tz(), &d, &h);
        asDated(slot, d, h, ch);
        out.lists << exec->previewSlot(ch, slot, rl.hourStartMs, &picks);
        out.edited << slot; out.keys << qMakePair(d, h); out.any = true;
    }
    for (int i = 0; i < ops.size(); ++i) if (!claimed.contains(i)) out.errors.append(QString("%1: no such event (%2) in the built hours").arg(ops[i].toObject().value("op").toString(), ops[i].toObject().value("itemId").toString(ops[i].toObject().value("mediaId").toString())));
    return out;
}

} // namespace

void SupervisorCore::registerFlightPlanRoutes() {
    RestApiServer* r = m_rest.get();

    r->registerRoute("GET", "/api/v1/flightplan/{ch}", [this](const QJsonObject&, const Q& q) {
        const QString ch = q["ch"]; { bool known = false; m_store->channel(ch, &known); if (!known) return err(404, "no such channel"); }
        const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
        return FlightPlan::analyze(plannedHours(m_store.get(), m_exec.get(), ch), fo);
    });

    r->registerRoute("GET", "/api/v1/flightplan/{ch}/corrections", [this](const QJsonObject&, const Q& q) {
        const QString ch = q["ch"]; { bool known = false; m_store->channel(ch, &known); if (!known) return err(404, "no such channel"); }
        const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
        const QJsonObject c = FlightPlan::corrections(plannedHours(m_store.get(), m_exec.get(), ch), fo, q.value("interval", "0").toInt());
        return c.contains("error") ? err(404, c.value("error").toString()) : c;
    });

    r->registerRoute("POST", "/api/v1/flightplan/{ch}/preview", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"]; { bool known = false; m_store->channel(ch, &known); if (!known) return err(404, "no such channel"); }
        const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
        const QList<Runlist> before = plannedHours(m_store.get(), m_exec.get(), ch);
        const Rebuilt rb = applyToHours(m_store.get(), m_exec.get(), ch, body.value("ops").toArray(), fo);
        QJsonObject o; o["applied"] = rb.any; o["log"] = rb.log; o["errors"] = rb.errors;
        o["diff"] = FlightPlan::diff(before, rb.lists); o["after"] = FlightPlan::analyze(rb.lists, fo);
        return o;
    });

    r->registerRoute("POST", "/api/v1/flightplan/{ch}/apply", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"]; { bool known = false; m_store->channel(ch, &known); if (!known) return err(404, "no such channel"); }
        const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
        const QJsonArray ops = body.value("ops").toArray();
        if (ops.isEmpty()) return err(400, "no ops");
        const QList<Runlist> before = plannedHours(m_store.get(), m_exec.get(), ch);
        const Rebuilt rb = applyToHours(m_store.get(), m_exec.get(), ch, ops, fo);
        if (!rb.any) { QJsonObject e = err(409, rb.errors.isEmpty() ? "nothing to apply" : rb.errors.first().toString()); e["errors"] = rb.errors; return e; }
        for (const HourSlot& s : rb.edited) if (!m_store->saveSlot(s)) return err(500, "could not save " + s.id);
        const QString who = body.value("who").toString("operator"), reason = body.value("reason").toString("time correction");
        QJsonObject entry; entry["atMs"] = double(fo.nowMs); entry["channelId"] = ch; entry["who"] = who; entry["reason"] = reason; entry["ops"] = ops; entry["log"] = rb.log;
        entry["summary"] = FlightPlan::diff(before, rb.lists).value("summary");
        QJsonArray audit = m_store->settingJson("flightplan.audit").value("entries").toArray(); audit.append(entry); while (audit.size() > 300) audit.removeFirst();
        m_store->setSettingJson("flightplan.audit", QJsonObject{{"entries", audit}});
        pushAlert(ch, "info", QString("schedule corrected by %1: %2").arg(who, entry.value("summary").toString()));
        QJsonObject o; o["applied"] = true; o["saved"] = int(rb.edited.size()); o["log"] = rb.log; o["errors"] = rb.errors;
        o["diff"] = FlightPlan::diff(before, rb.lists); o["after"] = FlightPlan::analyze(rb.lists, fo);
        return o;
    });

    r->registerRoute("GET", "/api/v1/flightplan/{ch}/why", [this](const QJsonObject&, const Q& q) {
        const QString ch = q["ch"]; { bool known = false; m_store->channel(ch, &known); if (!known) return err(404, "no such channel"); }
        const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
        const QString id = q.value("id");
        for (const Runlist& rl : plannedHours(m_store.get(), m_exec.get(), ch)) for (const RunlistItem& it : rl.items)
            if (it.item.id == id || it.runlistId == id) {
                QJsonObject o; o["id"] = it.item.id; o["title"] = it.item.title; o["why"] = QJsonArray::fromStringList(FlightPlan::explain(it, fo)); o["ready"] = FlightPlan::readiness(it, fo, true);
                return o;
            }
        return err(404, "no such event in the built hours");
    });

    r->registerRoute("GET", "/api/v1/flightplan/{ch}/audit", [this](const QJsonObject&, const Q& q) {
        QJsonArray out; const QString ch = q["ch"]; const int limit = q.value("limit", "100").toInt();
        const QJsonArray all = m_store->settingJson("flightplan.audit").value("entries").toArray();
        for (int i = all.size() - 1; i >= 0 && out.size() < limit; --i) if (all[i].toObject().value("channelId").toString() == ch) out.append(all[i]);
        return QJsonObject{{"entries", out}};
    });

    r->registerRoute("GET", "/api/v1/readiness", [this](const QJsonObject&, const Q& q) {
        QJsonArray chans; int fails = 0, warns = 0;
        for (const ChannelProfile& cp : m_store->channels()) {
            const QString ch = cp.id;
            const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, q);
            const QJsonObject fp = FlightPlan::analyze(plannedHours(m_store.get(), m_exec.get(), ch), fo);
            QJsonArray problems;
            auto scan = [&](const QJsonObject& it) { const QJsonObject rd = it.value("ready").toObject(); if (rd.value("state").toString() == "ok" || rd.isEmpty()) return;
                QJsonObject p; p["id"] = it.value("id"); p["title"] = it.value("title"); p["startMs"] = it.value("startMs"); p["state"] = rd.value("state"); p["problems"] = rd.value("problems"); problems.append(p); };
            scan(fp.value("now").toObject()); scan(fp.value("next").toObject()); for (const QJsonValue& v : fp.value("later").toArray()) scan(v.toObject());
            QJsonObject c; c["channelId"] = ch; c["problems"] = problems; c["summary"] = fp.value("summary"); chans.append(c);
            fails += fp.value("summary").toObject().value("readinessFails").toInt(); warns += fp.value("summary").toObject().value("readinessWarnings").toInt();
        }
        return QJsonObject{{"channels", chans}, {"fails", fails}, {"warnings", warns}, {"atMs", double(QDateTime::currentMSecsSinceEpoch())}};
    });

    // Pre-air readiness watcher: every 5 minutes the next two hours of every channel are checked; a NEW failing event raises one alert
    // (the same event and problem is not repeated), so a missing file is known hours before air instead of 3 seconds before.
    auto* watch = new QTimer(this); watch->setInterval(5 * 60 * 1000);
    auto alerted = std::make_shared<QSet<QString>>();
    connect(watch, &QTimer::timeout, this, [this, alerted]() {
        QSet<QString> now;
        for (const ChannelProfile& cp : m_store->channels()) {
            const QString ch = cp.id; if (!cp.armed || !cp.enabled) continue;
            const FlightOptions fo = flightOptions(this, m_store.get(), m_exec.get(), ch, Q());
            const QJsonObject fp = FlightPlan::analyze(plannedHours(m_store.get(), m_exec.get(), ch), fo);
            QList<QJsonObject> items; items << fp.value("now").toObject() << fp.value("next").toObject(); for (const QJsonValue& v : fp.value("later").toArray()) items << v.toObject();
            for (const QJsonObject& it : items) {
                const QJsonObject rd = it.value("ready").toObject(); if (rd.value("state").toString() != "fail") continue;
                const QString msg = it.value("title").toString() + ": " + rd.value("problems").toArray().first().toString();
                const QString key = ch + "|" + it.value("id").toString() + "|" + msg; now.insert(key);
                if (!alerted->contains(key)) pushAlert(ch, "error", QString("not ready for air at %1 — %2").arg(QDateTime::fromMSecsSinceEpoch(qint64(it.value("startMs").toDouble())).toString("h:mm AP"), msg));
            }
        }
        // Disk space where the library and the database live: a full disk stops ingest, spot generation and the as-run log.
        // Thresholds (settings alerts.disk, GB): warn 20, critical 5.
        const QJsonObject dc = m_store->settingJson("alerts.disk"); const double warnGb = dc.value("warnGB").toDouble(20.0), critGb = dc.value("criticalGB").toDouble(5.0);
        QSet<QString> seenRoots;
        for (const QString& path : {m_opt.dataDir, uploadDir()}) {
            const QStorageInfo si(path); if (!si.isValid() || !si.isReady() || seenRoots.contains(si.rootPath())) continue; seenRoots.insert(si.rootPath());
            const double freeGb = si.bytesAvailable() / 1073741824.0;
            const QString level = freeGb < critGb ? "critical" : freeGb < warnGb ? "warning" : QString();
            if (level.isEmpty()) continue;
            const QString key = "disk|" + si.rootPath() + "|" + level; now.insert(key);
            if (!alerted->contains(key)) pushAlert(QString(), level, QString("low disk space on %1: %2 GB free (media and database are stored here)").arg(QDir::toNativeSeparators(si.rootPath())).arg(freeGb, 0, 'f', 1));
        }
        *alerted = now;                 // a problem that was fixed and returns alerts again
    });
    QTimer::singleShot(45000, this, [watch]() { watch->start(); });
}
