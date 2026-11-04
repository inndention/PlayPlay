// FlightPlan: time debt, corrections, ghost-preview diff, readiness and "why" for the operator's flight-plan views.
#include "flightplan.h"

#include <QDateTime>
#include <QFileInfo>
#include <QLocale>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>

namespace sched {

namespace {

double nominalSec(const RunlistItem& r) {
    const double n = r.item.effectiveDurationSec();
    return n > 0.0 ? qMax(n, r.durationSec) : r.durationSec;
}
bool isFillOrigin(const RunlistItem& r) { return r.origin == "fill" || r.origin == "emergency"; }
QString clockLabel(qint64 ms, const QTimeZone& tz) {
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(ms, tz.isValid() ? tz : QTimeZone::systemTimeZone());
    return QLocale("en_US").toString(t.time(), t.time().second() ? "h:mm:ss AP" : "h:mm AP");
}
QString itemTitle(const RunlistItem& r) { return r.item.title.isEmpty() ? r.item.id : r.item.title; }

/// Hard-start anchor of an item (the fixed time it asks for), or 0.
qint64 anchorOf(const RunlistItem& r, const Runlist& rl) {
    if (!r.item.isHardStart()) return 0;
    return r.item.fixedUtcMs > 0 ? r.item.fixedUtcMs : rl.hourStartMs + qint64(std::llround(r.item.fixedOffsetSec * 1000.0));
}

struct Interval {
    qint64 startMs = 0, endMs = 0;
    QString endLabel;               // what the interval ends at: "6:00 PM" (hour end) or the anchor item's title
    double demandSec = 0, availSec = 0, fillSec = 0, droppedSec = 0;
    QStringList droppedTitles;      // events the builder left out because they did not fit (part of the demand)
    QList<const RunlistItem*> items;
    const Runlist* list = nullptr;
};

QList<Interval> intervalsOf(const QList<Runlist>& lists, const FlightOptions& opt) {
    // boundaries: every list's hour start and end, and every timed hard start
    QList<qint64> cuts; QHash<qint64, QString> names;
    for (const Runlist& rl : lists) {
        cuts << rl.hourStartMs << rl.hourEndMs; names[rl.hourEndMs] = QString("end of hour");
        for (const RunlistItem& r : rl.items) { const qint64 a = anchorOf(r, rl); if (a > 0) { cuts << a; names[a] = itemTitle(r); } }
    }
    std::sort(cuts.begin(), cuts.end()); cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    QList<Interval> out;
    for (int i = 0; i + 1 < cuts.size(); ++i) {
        Interval v; v.startMs = cuts[i]; v.endMs = cuts[i + 1];
        if (v.endMs <= opt.nowMs) continue;
        v.endLabel = names.value(v.endMs, QString());
        const qint64 from = qMax(v.startMs, opt.nowMs);
        v.availSec = (v.endMs - from) / 1000.0;
        for (const Runlist& rl : lists) {
            if (rl.hourStartMs > v.startMs || rl.hourEndMs <= v.startMs) continue;
            v.list = &rl;
            for (const RunlistItem& r : rl.items) {
                if (r.plannedEndMs <= opt.nowMs || r.plannedStartMs < v.startMs || r.plannedStartMs >= v.endMs) continue;
                if (isFillOrigin(r)) { v.fillSec += qMax(0.0, (r.plannedEndMs - qMax(r.plannedStartMs, opt.nowMs)) / 1000.0); continue; }   // only what is still to air
                const double elapsed = r.plannedStartMs < opt.nowMs ? (opt.nowMs - r.plannedStartMs) / 1000.0 : 0.0;
                v.demandSec += qMax(0.0, nominalSec(r) - elapsed);
                v.items << &r;
            }
            for (const RunlistItem& r : rl.dropped) {
                if (r.plannedEndMs <= opt.nowMs || r.plannedStartMs < v.startMs || r.plannedStartMs >= v.endMs) continue;
                v.demandSec += r.durationSec; v.droppedSec += r.durationSec; v.droppedTitles << itemTitle(r);
            }
        }
        if (v.list) out << v;
    }
    return out;
}

QJsonObject itemJson(const RunlistItem& r, const FlightOptions& opt, qint64 protectedUntilMs, const QJsonObject& ready) {
    QJsonObject o = r.toJson();
    o["id"] = r.item.id; o["runlistId"] = r.runlistId; o["title"] = itemTitle(r);
    o["startMs"] = double(r.plannedStartMs); o["endMs"] = double(r.plannedEndMs);
    o["remainingSec"] = qMax(0.0, (r.plannedEndMs - qMax(opt.nowMs, r.plannedStartMs)) / 1000.0);
    o["startsInSec"] = (r.plannedStartMs - opt.nowMs) / 1000.0;
    o["protected"] = r.plannedStartMs < protectedUntilMs;
    o["mandatory"] = FlightPlan::isMandatory(r.item); o["optional"] = FlightPlan::isOptional(r.item);
    o["fill"] = isFillOrigin(r);
    if (!ready.isEmpty()) o["ready"] = ready;
    return o;
}

struct Option {
    QString kind, label, itemId; double savingSec = 0; QJsonObject op; int pref = 0;
};

QJsonObject optionJson(const Option& o, int i) {
    QJsonObject j; j["id"] = QString("c%1").arg(i); j["kind"] = o.kind; j["label"] = o.label; j["itemId"] = o.itemId; j["savingSec"] = o.savingSec; j["op"] = o.op; return j;
}

int typeRank(const QString& t) { static const QStringList order = {"filler", "image", "video", "promo", "bumper", "psa"}; const int i = order.indexOf(t); return i < 0 ? 50 : i; }

/// The cheapest subset (in preference order) whose savings reach `need`: minimal overshoot, earlier options preferred. Falls back to everything.
QList<int> bestSubset(const QList<Option>& opts, double need) {
    const int target = int(std::ceil(need - 1e-9));
    QList<int> sav; int total = 0;
    for (const Option& o : opts) { const int s = qMax(1, int(std::lround(o.savingSec))); sav << s; total += s; }
    if (target <= 0) return {};
    if (total < target) { QList<int> all; for (int i = 0; i < opts.size(); ++i) all << i; return all; }
    const int cap = qMin(total, target + 3600);
    QVector<int> prev(cap + 1, -1), via(cap + 1, -1); prev[0] = 0;
    for (int i = 0; i < sav.size(); ++i)
        for (int s = cap; s >= sav[i]; --s)
            if (prev[s] < 0 && prev[s - sav[i]] >= 0 && via[s - sav[i]] != i) { prev[s] = s - sav[i]; via[s] = i; }
    int best = -1; for (int s = target; s <= cap; ++s) if (prev[s] >= 0) { best = s; break; }
    if (best < 0) { QList<int> all; for (int i = 0; i < opts.size(); ++i) all << i; return all; }
    QList<int> out; for (int s = best; s > 0; s = prev[s]) out << via[s];
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

QString FlightPlan::fmtDuration(double sec) {
    const int s = int(std::lround(qAbs(sec)));
    if (s < 60) return QString("%1 s").arg(s);
    if (s < 3600) return QString("%1 m %2 s").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1 h %2 m").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QChar('0'));
}

bool FlightPlan::isMandatory(const SlotItem& it) {
    if (it.extra.contains("mandatory")) return it.extra.value("mandatory").toBool();
    return it.locked || it.type == "ad" || it.type == "station_id" || it.type == "live";
}
bool FlightPlan::isOptional(const SlotItem& it) {
    if (isMandatory(it)) return false;
    if (it.extra.contains("optional")) return it.extra.value("optional").toBool();
    return it.type == "filler" || it.type == "promo" || it.type == "bumper" || it.type == "psa" || it.type == "image";
}

// ---------------------------------------------------------------------------------------------
// Readiness / why
// ---------------------------------------------------------------------------------------------
QJsonObject FlightPlan::readiness(const RunlistItem& r, const FlightOptions& opt, bool checkFile) {
    QJsonObject checks; QStringList problems; QString worst = "ok";
    auto put = [&](const char* k, const QString& state, const QString& msg) {
        QJsonObject c; c["state"] = state; c["message"] = msg; checks[k] = c;
        if (state == "fail") { worst = "fail"; problems << msg; } else if (state == "warn") { if (worst != "fail") worst = "warn"; problems << msg; }
    };
    const SlotItem& it = r.item;
    MediaRecord m; bool found = false;
    if (!it.mediaId.isEmpty() && opt.media.byId) m = opt.media.byId(it.mediaId, &found);
    const bool live = it.type == "live" || !it.liveSource.isEmpty() || !it.liveSourceId.isEmpty();
    const QString path = !m.path.isEmpty() ? m.path : it.path;
    const bool url = path.contains("://") && !QFileInfo(path).exists();
    if (live) put("media", "ok", "live source");
    else if (it.mediaId.isEmpty()) put("media", path.isEmpty() ? "fail" : "ok", path.isEmpty() ? "no media and no path" : "plays from a path, not the library");
    else put("media", found ? "ok" : "fail", found ? "in the library" : "media id is not in the library");
    if (found) {
        const QString st = m.extra.value("status").toString("approved");
        put("approval", st == "rejected" ? "fail" : st == "review" ? "warn" : "ok", st == "rejected" ? "media is rejected" : st == "review" ? "media is waiting for review" : "approved");
        if (m.extra.value("missing").toBool(false)) put("file", "fail", "library sweep: file is missing on disk");
        const qint64 end = m.availableEndMs;
        if (!m.isAvailableAt(r.plannedStartMs)) put("window", "fail", m.archived ? "media is archived" : (end > 0 && r.plannedStartMs > end) ? "expired before this airtime" : "not yet available at this airtime");
        else if (end > 0 && end - opt.nowMs < 24 * 3600000LL && end - opt.nowMs > 0 && !m.extra.value("perishable").toBool(false)) put("window", "warn", "expires " + clockLabel(end, opt.tz));   // generated spots are perishable by design
        else put("window", "ok", "inside its availability window");
        if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(opt.channelId)) put("channel", "fail", "not allowed on this channel");
    }
    if (!live && !url && checkFile && opt.checkFiles && !path.isEmpty() && !checks.contains("file")) {
        const QFileInfo fi(path);
        if (!fi.exists()) put("file", "fail", "file is not on disk: " + path);
        else if (fi.size() == 0) put("file", "fail", "file is empty");
        else if (found && m.fileSizeBytes > 0 && fi.size() != m.fileSizeBytes) put("file", "warn", "file size differs from the library record (copy in progress or file replaced)");
        else put("file", "ok", "file present");
    }
    if (!live && r.durationSec < 0.5) put("duration", "fail", "no playable duration");
    QJsonObject o; o["state"] = worst; o["checks"] = checks; o["problems"] = QJsonArray::fromStringList(problems);
    return o;
}

QStringList FlightPlan::explain(const RunlistItem& r, const FlightOptions& opt) {
    QStringList why; const SlotItem& it = r.item;
    if (r.origin == "emergency") why << "Emergency loop: nothing was scheduled for this time";
    else if (r.origin == "fill") why << "Fill: placed to cover a gap, picked by the fill policy (eligible, not recently played)";
    else if (it.extra.value("queryFill").toBool(false)) why << QString("Pick of the block '%1' (rotation / eligibility rules chose it)").arg(it.extra.value("blockLabel").toString(it.extra.value("blockItemId").toString()));
    else if (r.origin == "smart") why << "Smart slot: resolved to this media when the hour was built";
    else if (r.origin == "continuation") why << "Continuation of the previous hour's program";
    else why << "Scheduled item of its hour";
    if (r.item.isHardStart()) {
        const QString pol = it.extra.value("conflictPolicy").toString("truncate");
        const QString at = it.fixedUtcMs > 0 ? clockLabel(it.fixedUtcMs, opt.tz)
                         : QString(":%1:%2 into the hour").arg(int(it.fixedOffsetSec) / 60, 2, 10, QChar('0')).arg(int(it.fixedOffsetSec) % 60, 2, 10, QChar('0'));
        why << QString("Hard start at %1 (if a clip is playing: %2)").arg(at, pol == "finish_previous" ? "it finishes and this starts late" : pol == "drop_previous" ? "it is dropped" : "it is cut");
        if (it.extra.value("backtime").toBool(false)) why << "Back-timed: the events before it were timed backward from this start";
    } else if (r.hard) why << "Top of the hour: the engine cuts to it exactly";
    else if (!isFillOrigin(r)) why << "Soft start: follows the previous event";
    if (!it.lockLabel.isEmpty()) why << "Locked marker: " + it.lockLabel;
    else if (it.locked) why << "Locked: kept in place";
    if (it.anchorSec() >= 0.0) why << QString("Anchored near :%1 of the hour").arg(int(it.anchorSec() / 60.0), 2, 10, QChar('0'));
    if (r.truncated && !r.note.isEmpty()) why << "Shortened: " + r.note;
    else if (!r.note.isEmpty() && !r.truncated) why << r.note;
    if (isMandatory(it)) why << "Mandatory: corrections never remove or trim it";
    else if (isOptional(it)) why << "Optional: the first thing a time correction drops";
    if (opt.media.byId && !it.mediaId.isEmpty()) {
        bool f = false; const MediaRecord m = opt.media.byId(it.mediaId, &f);
        if (f && m.availableEndMs > 0) why << "Expires " + clockLabel(m.availableEndMs, opt.tz);
        if (f && !m.client.isEmpty()) why << "Client: " + m.client;
    }
    return why;
}

// ---------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------
QJsonObject FlightPlan::analyze(const QList<Runlist>& lists, const FlightOptions& opt) {
    QList<const RunlistItem*> upcoming;
    for (const Runlist& rl : lists) for (const RunlistItem& r : rl.items) if (r.plannedEndMs > opt.nowMs) upcoming << &r;
    std::stable_sort(upcoming.begin(), upcoming.end(), [](const RunlistItem* a, const RunlistItem* b) { return a->plannedStartMs < b->plannedStartMs; });

    // Protected boundary: the on-air event, and the editable window after now.
    qint64 protectedUntil = opt.nowMs + qint64(opt.protectSec * 1000.0);
    int onAirIdx = -1;
    for (int i = 0; i < upcoming.size(); ++i) if (upcoming[i]->plannedStartMs <= opt.nowMs) { onAirIdx = i; protectedUntil = qMax(protectedUntil, upcoming[i]->plannedEndMs); }

    int checked = 0, fails = 0, warns = 0;
    auto build = [&](const RunlistItem& r) {
        QJsonObject ready;
        const bool within = (r.plannedStartMs - opt.nowMs) / 1000.0 <= opt.readinessHorizonSec && checked < opt.readinessMaxItems && !isFillOrigin(r);
        if (within) { ++checked; ready = readiness(r, opt, true); const QString s = ready.value("state").toString(); if (s == "fail") ++fails; else if (s == "warn") ++warns; }
        return itemJson(r, opt, protectedUntil, ready);
    };

    QJsonObject out;
    out["channelId"] = opt.channelId; out["nowMs"] = double(opt.nowMs); out["protectedUntilMs"] = double(protectedUntil);
    QJsonArray later; int shown = 0;
    for (int i = 0; i < upcoming.size(); ++i) {
        const RunlistItem& r = *upcoming[i];
        if (i == onAirIdx) out["now"] = build(r);
        else if ((onAirIdx < 0 && i == 0) || i == onAirIdx + 1) { if (!out.contains("next")) out["next"] = build(r); else later.append(build(r)); }
        else if (shown++ < 60) later.append(build(r));
    }
    if (onAirIdx < 0 && !upcoming.isEmpty() && out.contains("next") && !out.contains("now")) { /* nothing on air in the plan */ }
    out["later"] = later;

    QJsonArray anchors;
    for (const Runlist& rl : lists) for (const RunlistItem& r : rl.items) {
        const qint64 a = anchorOf(r, rl); if (a <= 0 || a <= opt.nowMs - 3600000LL) continue;
        QJsonObject j; j["itemId"] = r.item.id; j["title"] = itemTitle(r); j["atMs"] = double(a); j["label"] = clockLabel(a, opt.tz);
        j["policy"] = r.item.extra.value("conflictPolicy").toString("truncate"); j["backtime"] = r.item.extra.value("backtime").toBool(false);
        j["actualStartMs"] = double(r.plannedStartMs); j["lateSec"] = qMax(0.0, (r.plannedStartMs - a) / 1000.0);
        anchors.append(j);
    }
    out["anchors"] = anchors;

    QJsonArray ivs; double totalOver = 0, totalUnder = 0;
    const QList<Interval> list = intervalsOf(lists, opt);
    for (int i = 0; i < list.size(); ++i) {
        const Interval& v = list[i];
        const double debt = v.demandSec - v.availSec;
        QJsonObject j; j["index"] = i; j["startMs"] = double(v.startMs); j["endMs"] = double(v.endMs); j["endsAt"] = clockLabel(v.endMs, opt.tz); j["endsAtName"] = v.endLabel;
        j["demandSec"] = v.demandSec; j["availSec"] = v.availSec; j["fillSec"] = v.fillSec; j["debtSec"] = debt;
        j["droppedSec"] = v.droppedSec; j["droppedTitles"] = QJsonArray::fromStringList(v.droppedTitles);
        const bool over = debt > 1.0;
        const double under = -debt - v.fillSec;     // empty time left after fill
        j["state"] = over ? "over" : (-debt > 1.0 ? "under" : "ok");
        j["gapSec"] = qMax(0.0, under);
        if (over) { j["label"] = QString("%1 over before %2%3").arg(fmtDuration(debt), clockLabel(v.endMs, opt.tz), v.droppedTitles.isEmpty() ? QString() : QString(" (%1 event%2 will not air)").arg(v.droppedTitles.size()).arg(v.droppedTitles.size() == 1 ? "" : "s")); totalOver += debt; }
        else if (-debt > 1.0) { j["label"] = v.fillSec > 1.0 ? QString("%1 under before %2, %3 filled").arg(fmtDuration(-debt), clockLabel(v.endMs, opt.tz), fmtDuration(v.fillSec)) : QString("%1 gap before %2").arg(fmtDuration(-debt), clockLabel(v.endMs, opt.tz)); totalUnder += qMax(0.0, under); }
        else j["label"] = QString("on time for %1").arg(clockLabel(v.endMs, opt.tz));
        QJsonArray ids; for (const RunlistItem* r : v.items) ids.append(r->item.id); j["itemIds"] = ids;
        ivs.append(j);
    }
    out["intervals"] = ivs;
    QJsonObject sum; sum["overSec"] = totalOver; sum["unfilledGapSec"] = totalUnder; sum["readinessFails"] = fails; sum["readinessWarnings"] = warns; sum["checked"] = checked;
    sum["label"] = totalOver > 1.0 ? QString("%1 over").arg(fmtDuration(totalOver)) : totalUnder > 30.0 ? QString("%1 unfilled").arg(fmtDuration(totalUnder)) : QString("on time");
    out["summary"] = sum;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Corrections
// ---------------------------------------------------------------------------------------------
QJsonObject FlightPlan::corrections(const QList<Runlist>& lists, const FlightOptions& opt, int intervalIndex) {
    QJsonObject out;
    const QList<Interval> ivs = intervalsOf(lists, opt);
    if (intervalIndex < 0 || intervalIndex >= ivs.size()) { out["error"] = "no such interval"; return out; }
    const Interval& v = ivs[intervalIndex];
    const double debt = v.demandSec - v.availSec;
    qint64 protectedUntil = opt.nowMs + qint64(opt.protectSec * 1000.0);
    for (const Runlist& rl : lists) for (const RunlistItem& r : rl.items) if (r.plannedStartMs <= opt.nowMs && r.plannedEndMs > opt.nowMs) protectedUntil = qMax(protectedUntil, r.plannedEndMs);
    out["interval"] = intervalIndex; out["debtSec"] = debt; out["protectedUntilMs"] = double(protectedUntil);

    QList<Option> opts;
    QJsonObject best;
    if (debt > 1.0) {
        // OVER: free `debt` seconds from the events that may give time.
        for (const RunlistItem* r : v.items) {
            if (r->plannedStartMs < protectedUntil || isMandatory(r->item)) continue;
            const double nom = nominalSec(*r), cur = nom;      // savings are measured against the full length: it is what the event asks of the interval, even when the builder cut its tail
            const QString name = itemTitle(*r);
            const int rank = r->item.priority * 100 + typeRank(r->item.type);
            if (isOptional(r->item)) {
                Option o; o.kind = "drop"; o.itemId = r->item.id; o.savingSec = cur; o.pref = rank;
                o.label = QString("Drop optional %1 '%2': %3").arg(r->item.type, name, "−" + fmtDuration(cur));
                o.op = QJsonObject{{"op", "remove"}, {"itemId", r->item.id}, {"hourStartMs", double(v.list->hourStartMs)}};
                opts << o;
            }
            const double minDurSet = r->item.extra.value("minDurationSec").toDouble(0.0);
            const double minDur = minDurSet > 0.0 ? minDurSet : (r->item.extra.value("canTrim").toBool(false) ? 5.0 : -1.0);    // blank / 0 = may not be trimmed
            if (minDur >= 0.0 && cur - minDur > 1.0 && !r->item.isQuery()) {
                Option o; o.kind = "trim"; o.itemId = r->item.id; o.savingSec = cur - minDur; o.pref = 5000 + rank;
                o.label = QString("Trim '%1' to %2: %3").arg(name, fmtDuration(minDur), "−" + fmtDuration(cur - minDur));
                o.op = QJsonObject{{"op", "trim"}, {"itemId", r->item.id}, {"outPointSec", r->item.inPointSec + minDur}, {"hourStartMs", double(v.list->hourStartMs)}};
                opts << o;
            }
            if (isOptional(r->item) && opt.media.byTypes && !r->item.mediaId.isEmpty()) {
                // a shorter version of the same kind: the shortest one that still covers what is needed, else the shortest available
                const double need = qMin(debt, cur - 1.0);
                const MediaRecord* bestM = nullptr; QList<MediaRecord> pool = opt.media.byTypes({r->item.type});
                for (const MediaRecord& m : pool) {
                    if (m.id == r->item.mediaId || !m.isAvailableAt(r->plannedStartMs) || m.durationSec < 3.0 || m.durationSec >= cur - 1.0) continue;
                    if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(opt.channelId)) continue;
                    if (!bestM || (cur - m.durationSec >= need ? (cur - bestM->durationSec < need || m.durationSec > bestM->durationSec) : (cur - bestM->durationSec < need && m.durationSec < bestM->durationSec))) bestM = &m;
                }
                if (bestM) {
                    Option o; o.kind = "shorten"; o.itemId = r->item.id; o.savingSec = cur - bestM->durationSec; o.pref = 3000 + rank;
                    o.label = QString("Replace %1 '%2' with the shorter '%3': %4").arg(r->item.type, name, bestM->title, "−" + fmtDuration(o.savingSec));
                    o.op = QJsonObject{{"op", "replace"}, {"itemId", r->item.id}, {"mediaId", bestM->id}, {"hourStartMs", double(v.list->hourStartMs)}};
                    opts << o;
                }
            }
            Q_UNUSED(nom);
        }
        // one option per event: when an event has several, the one saving the most stays out of the way of a plain drop
        std::stable_sort(opts.begin(), opts.end(), [](const Option& a, const Option& b) { return a.pref < b.pref; });
        QSet<QString> seen; QList<Option> uniq;
        for (const Option& o : opts) if (!seen.contains(o.itemId + o.kind)) { seen.insert(o.itemId + o.kind); uniq << o; }
        opts = uniq;
        // an event may be used by one option only in the best plan
        QList<Option> planPool; QSet<QString> used;
        for (const Option& o : opts) if (!used.contains(o.itemId)) { used.insert(o.itemId); planPool << o; }
        const QList<int> pick = bestSubset(planPool, debt);
        double saving = 0; QJsonArray ops, labels;
        for (int i : pick) { saving += planPool[i].savingSec; ops.append(planPool[i].op); labels.append(planPool[i].label); }
        best["ops"] = ops; best["labels"] = labels; best["savingSec"] = saving; best["remainingSec"] = qMax(0.0, debt - saving); best["overshootSec"] = qMax(0.0, saving - debt);
        best["covers"] = saving + 0.5 >= debt;
        out["kind"] = "over";
    } else {
        // GAP: clips that fit what is empty before the next anchor (fill already placed counts as used)
        const double gap = qMax(0.0, -debt - v.fillSec);
        out["kind"] = "gap"; out["gapSec"] = gap;
        QList<MediaRecord> pool;
        if (opt.media.byTypes) pool = opt.media.byTypes({"filler", "promo", "bumper", "psa", "station_id"});
        QSet<QString> inPlan; for (const RunlistItem* r : v.items) inPlan.insert(r->item.mediaId);
        std::stable_sort(pool.begin(), pool.end(), [](const MediaRecord& a, const MediaRecord& b) { return a.lastPlayedMs < b.lastPlayedMs; });   // least recently played first
        QString beforeId; qint64 beforeStart = 0;
        for (const RunlistItem& r : v.list->items) if (r.plannedStartMs >= v.endMs - 1 && (beforeId.isEmpty() || r.plannedStartMs < beforeStart) && anchorOf(r, *v.list) == v.endMs) { beforeId = r.item.id; beforeStart = r.plannedStartMs; }
        for (const MediaRecord& m : pool) {
            if (gap < 3.0 || m.durationSec > gap + 0.5 || m.durationSec < 3.0 || !m.isAvailableAt(v.startMs)) continue;
            if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(opt.channelId)) continue;
            if (m.extra.value("status").toString("approved") != "approved" || m.extra.value("missing").toBool(false)) continue;
            Option o; o.kind = "insert"; o.itemId = m.id; o.savingSec = m.durationSec;
            o.label = QString("Insert %1 '%2' (%3), leaves %4").arg(m.type, m.title, fmtDuration(m.durationSec), fmtDuration(gap - m.durationSec));
            o.op = QJsonObject{{"op", "insert"}, {"mediaId", m.id}, {"beforeItemId", beforeId}, {"hourStartMs", double(v.list->hourStartMs)}};
            opts << o;
            if (opts.size() >= 40) break;
        }
        // the option that leaves the least empty, repeats of what already plays in the interval last
        std::stable_sort(opts.begin(), opts.end(), [&](const Option& a, const Option& b) {
            const bool ra = inPlan.contains(a.itemId), rb = inPlan.contains(b.itemId); if (ra != rb) return !ra;
            return (gap - a.savingSec) < (gap - b.savingSec); });
        if (!opts.isEmpty()) {
            QJsonArray ops, labels; ops.append(opts.first().op); labels.append(opts.first().label);
            best["ops"] = ops; best["labels"] = labels; best["savingSec"] = opts.first().savingSec; best["remainingSec"] = gap - opts.first().savingSec; best["covers"] = gap - opts.first().savingSec < 3.0;
        } else { best["ops"] = QJsonArray(); best["remainingSec"] = gap; best["covers"] = false; }
    }
    QJsonArray arr; for (int i = 0; i < opts.size(); ++i) arr.append(optionJson(opts[i], i));
    out["options"] = arr; out["best"] = best;
    return out;
}

// ---------------------------------------------------------------------------------------------
// Applying ops
// ---------------------------------------------------------------------------------------------
namespace {
int indexOfItem(const HourSlot& s, const QString& id) { for (int i = 0; i < s.items.size(); ++i) if (s.items[i].id == id) return i; return -1; }

/// A query-block pick ("<block>#k") only exists in the build: turn the whole block into the concrete items it resolved to.
bool materialize(HourSlot& s, const Runlist& rl, const QString& blockId) {
    const int at = indexOfItem(s, blockId);
    if (at < 0) return false;
    QList<SlotItem> picks;
    for (const RunlistItem& r : rl.items) {
        if (r.item.extra.value("blockItemId").toString() != blockId) continue;
        SlotItem c = r.item;
        c.queryBlock = QJsonObject(); c.folderBlock = QJsonObject(); c.locked = false; c.lockAtSec = -1.0; c.timingMode = "soft_start"; c.fixedOffsetSec = -1.0;
        for (const char* k : {"queryFill", "blockItemId", "blockLabel", "queryBlock", "query_block", "folderBlock", "folder_block"}) c.extra.remove(k);
        if (r.outSec > r.inSec) { c.inPointSec = r.inSec; c.outPointSec = r.outSec; }
        picks << c;
    }
    if (picks.isEmpty()) return false;
    s.items.removeAt(at);
    for (int k = 0; k < picks.size(); ++k) s.items.insert(at + k, picks[k]);
    return true;
}
} // namespace

bool FlightPlan::applyOps(HourSlot& slot, const QJsonArray& ops, const Runlist& rl, const FlightOptions& opt, QStringList* log, QStringList* errors) {
    qint64 protectedUntil = opt.nowMs + qint64(opt.protectSec * 1000.0);
    for (const RunlistItem& r : rl.items) if (r.plannedStartMs <= opt.nowMs && r.plannedEndMs > opt.nowMs) protectedUntil = qMax(protectedUntil, r.plannedEndMs);
    auto runlistItem = [&](const QString& id) -> const RunlistItem* { for (const RunlistItem& r : rl.items) if (r.item.id == id) return &r; return nullptr; };
    auto fail = [&](const QString& m) { if (errors) *errors << m; };
    bool any = false; int seq = 0;
    for (const QJsonValue& ov : ops) {
        const QJsonObject op = ov.toObject(); const QString kind = op.value("op").toString();
        const QString id = op.value("itemId").toString();
        if (kind == "insert") {
            const QString mediaId = op.value("mediaId").toString(); bool f = false;
            const MediaRecord m = opt.media.byId ? opt.media.byId(mediaId, &f) : MediaRecord();
            if (!f) { fail("insert: media " + mediaId + " is not in the library"); continue; }
            const QString before = op.value("beforeItemId").toString();
            const RunlistItem* anchor = before.isEmpty() ? nullptr : runlistItem(before);
            if (anchor && anchor->plannedStartMs < protectedUntil) { fail("insert: the position is inside the protected window"); continue; }
            if (!anchor && rl.hourEndMs <= protectedUntil) { fail("insert: the hour is protected"); continue; }
            SlotItem n; n.id = QString("fp-%1-%2").arg(opt.nowMs).arg(++seq); n.mediaId = m.id; n.title = m.title; n.type = m.type; n.path = m.path; n.durationSec = m.durationSec;
            int at = before.isEmpty() ? slot.items.size() : indexOfItem(slot, before);
            if (at < 0 && !before.isEmpty() && anchor && anchor->item.extra.contains("blockItemId")) at = indexOfItem(slot, anchor->item.extra.value("blockItemId").toString());
            if (at < 0) at = slot.items.size();
            slot.items.insert(at, n); any = true; if (log) *log << QString("inserted %1 '%2' (%3 s)").arg(m.type, m.title).arg(m.durationSec, 0, 'f', 0);
            continue;
        }
        const RunlistItem* r = runlistItem(id);
        if (!r) { fail(kind + ": no such event " + id); continue; }
        if (r->plannedStartMs < protectedUntil) { fail(QString("%1 '%2': protected (it starts before %3)").arg(kind, itemTitle(*r), clockLabel(protectedUntil, opt.tz))); continue; }
        if (isMandatory(r->item) && kind != "replace") { fail(QString("%1 '%2': mandatory events are never removed or trimmed").arg(kind, itemTitle(*r))); continue; }
        if (indexOfItem(slot, id) < 0) {
            const QString block = r->item.extra.value("blockItemId").toString();
            if (block.isEmpty() || !materialize(slot, rl, block)) { fail(kind + ": cannot locate " + itemTitle(*r) + " in the stored hour"); continue; }
            if (log) *log << QString("block '%1' fixed to its current picks").arg(r->item.extra.value("blockLabel").toString(block));
        }
        const int at = indexOfItem(slot, id);
        if (at < 0) { fail(kind + ": cannot locate " + itemTitle(*r)); continue; }
        if (kind == "remove") { slot.items.removeAt(at); any = true; if (log) *log << QString("removed '%1' (%2 s)").arg(itemTitle(*r)).arg(r->durationSec, 0, 'f', 0); }
        else if (kind == "trim") {
            SlotItem& it = slot.items[at]; const double out = op.value("outPointSec").toDouble();
            if (out <= it.inPointSec + 1.0) { fail("trim: out point too early"); continue; }
            it.outPointSec = out; any = true; if (log) *log << QString("trimmed '%1' to %2 s").arg(itemTitle(*r)).arg(out - it.inPointSec, 0, 'f', 0);
        } else if (kind == "replace") {
            const QString mediaId = op.value("mediaId").toString(); bool f = false;
            const MediaRecord m = opt.media.byId ? opt.media.byId(mediaId, &f) : MediaRecord();
            if (!f) { fail("replace: media " + mediaId + " is not in the library"); continue; }
            SlotItem& it = slot.items[at]; it.mediaId = m.id; it.title = m.title; it.path = m.path; it.durationSec = m.durationSec; it.inPointSec = 0; it.outPointSec = 0; it.type = m.type;
            any = true; if (log) *log << QString("replaced '%1' with '%2'").arg(itemTitle(*r), m.title);
        } else fail("unknown op '" + kind + "'");
    }
    return any;
}

// ---------------------------------------------------------------------------------------------
// Diff
// ---------------------------------------------------------------------------------------------
QJsonObject FlightPlan::diff(const QList<Runlist>& before, const QList<Runlist>& after) {
    struct Rec { qint64 start; QString media, title; double dur; };
    auto collect = [](const QList<Runlist>& ls, double* gap, double* fill) {
        QHash<QString, Rec> m; *gap = 0; *fill = 0;
        for (const Runlist& rl : ls) {
            *gap += rl.gapSec; *fill += rl.filledSec;
            for (const RunlistItem& r : rl.items) if (!isFillOrigin(r)) m[r.item.id] = Rec{r.plannedStartMs, r.item.mediaId, itemTitle(r), r.durationSec};
        }
        return m;
    };
    double g0, f0, g1, f1;
    const QHash<QString, Rec> a = collect(before, &g0, &f0), b = collect(after, &g1, &f1);
    int moved = 0, replaced = 0, removed = 0, added = 0; QJsonArray details;
    auto d = [&](const char* kind, const Rec& r, const QString& text) { QJsonObject j; j["kind"] = kind; j["title"] = r.title; j["text"] = text; details.append(j); };
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (!b.contains(it.key())) { ++removed; d("removed", it.value(), "removed"); continue; }
        const Rec& n = b[it.key()];
        if (n.media != it.value().media) { ++replaced; d("replaced", it.value(), "now " + n.title); }
        else if (qAbs(n.start - it.value().start) > 1000) { ++moved; d("moved", n, QString("%1 %2").arg(n.start > it.value().start ? "later by" : "earlier by", FlightPlan::fmtDuration((n.start - it.value().start) / 1000.0))); }
    }
    for (auto it = b.begin(); it != b.end(); ++it) if (!a.contains(it.key())) { ++added; d("added", it.value(), "added"); }
    QJsonObject o; o["moved"] = moved; o["replaced"] = replaced; o["removed"] = removed; o["added"] = added;
    o["gapsBeforeSec"] = g0; o["gapsAfterSec"] = g1; o["fillBeforeSec"] = f0; o["fillAfterSec"] = f1; o["details"] = details;
    QStringList parts;
    if (removed) parts << QString("%1 removed").arg(removed);
    if (replaced) parts << QString("%1 replaced").arg(replaced);
    if (moved) parts << QString("%1 moved").arg(moved);
    if (added) parts << QString("%1 added").arg(added);
    parts << (g1 > 1.0 ? QString("%1 gap left").arg(FlightPlan::fmtDuration(g1)) : QString("no gap left"));
    o["summary"] = parts.join(", ");
    return o;
}

} // namespace sched
