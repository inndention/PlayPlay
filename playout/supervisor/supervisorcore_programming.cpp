// SupervisorCore: programming routes — query/folder blocks, hour recipes, weighted fill (format clock),
// autofill policy, ad contracts, lock anchors, clear/shuffle/re-roll. PlayPlay OBS parity for
// /api/hour-recipes, /api/schedule/{ch}/hour-recipe, /weighted-fill, /weights, /autofill, /ad-contracts.
#include "supervisorcore.h"
#include "restapiserver.h"
#include "transcriptionmanager.h"
#include "../schedule/schedulestore.h"
#include "../schedule/scheduleexecutor.h"
#include "../schedule/formatclock.h"
#include "../schedule/queryblocks.h"
#include "../schedule/runlistbuilder.h"
#include <QDateTime>
#include <QTimeZone>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QDebug>
#include <algorithm>
#include <cmath>

using namespace sched;
using Q = QHash<QString, QString>;

namespace {

struct Target { QString slotId; HourSlot existing; QDate date; int dow = -1; int hour = 0; bool dated = false; bool rowExists = false; };

// Request limits. The REST surface runs on the Supervisor's single thread (the same one as the executor tick and the time sync)
// and is open when no token is configured, so one oversized body must not be able to stall it. PlayPlay OBS caps recipe parts at 12.
constexpr int kMaxRecipeParts = 24;
constexpr double kMinPartMinutes = 0.25;      // 15 s: an hour can never hold more than 240 recipe blocks
constexpr int kMaxBuckets = 24;
constexpr int kMaxContracts = 500;
constexpr double kMinCapSec = 300.0, kMaxCapSec = 7200.0;
constexpr double kNoSplitSec = 1.0e7;         // programCapSec <= 0: never split a program, it simply has to fit the room

QStringList strList(const QJsonValue& v) { QStringList l; for (const QJsonValue& x : v.toArray()) l << x.toString(); if (v.isString()) l << v.toString(); return l; }

/// A key counts as given when it is there and not null (JSON.stringify drops undefined, some clients send null).
bool present(const QJsonObject& o, const char* key) { return o.contains(key) && !o.value(key).isNull() && !o.value(key).isUndefined(); }

/// A small integer in [lo, hi] from a JSON number without a fraction, or a string of digits. Anything else is a caller bug, never a silent 0.
bool readInt(const QJsonValue& v, int lo, int hi, int* out) {
    double d = 0.0;
    if (v.isDouble()) d = v.toDouble();
    else if (v.isString()) { bool ok = false; const int i = v.toString().trimmed().toInt(&ok); if (!ok) return false; d = i; }
    else return false;
    if (d != std::floor(d) || d < lo || d > hi) return false;
    *out = int(d);
    return true;
}

/// Optional list of integers in [lo, hi]: absent -> *has = false; given -> a non-empty array of in-range integers, sorted and de-duplicated
/// (so a list can never name more than 24 hours or 7 days, and a typo never widens the scope to "everything").
bool readIntList(const QJsonObject& o, const char* key, int lo, int hi, QList<int>* out, bool* has, QString* why) {
    out->clear(); *has = false;
    if (!present(o, key)) return true;
    const QJsonValue v = o.value(key);
    QSet<int> seen; bool ok = v.isArray() && !v.toArray().isEmpty();
    if (ok) for (const QJsonValue& x : v.toArray()) { int i = 0; if (!readInt(x, lo, hi, &i)) { ok = false; break; } seen.insert(i); }
    if (!ok) { if (why) *why = QString("%1 must be a non-empty array of integers %2..%3").arg(QString(key)).arg(lo).arg(hi); return false; }
    *out = seen.values(); std::sort(out->begin(), out->end()); *has = true;
    return true;
}

/// Hours in scope for a programming operation. Nothing is guessed: a missing, malformed or ambiguous target comes back empty with `why`
/// set (the routes answer 400), never as a write to Sunday 00:00.
///   scope: hour (slotId | date+hour | dow+hour) · day (date | dow; hours[] optional) · week (weekStart for dated, else template) · hours (dows[] × hours[])
///   dated: write dated overrides (YYYY-MM-DD-H) instead of template slots. Left out, a `date` selects dated slots (hour and day scope alike),
///   a bare dow selects the weekly template. emptyOnly: skip hours that already hold unlocked items.
QList<Target> targetsFor(ScheduleStore* store, const QString& ch, const QJsonObject& body, QString* why) {
    QList<Target> out;
    auto fail = [&](const QString& m) { if (why) *why = m; return out; };   // `out` is still empty wherever this is called

    const QString scope = body.value("scope").toString("hour");
    if (scope != "hour" && scope != "day" && scope != "week" && scope != "hours") return fail("unknown scope '" + scope + "' (hour, day, week or hours)");
    const bool emptyOnly = body.value("emptyOnly").toBool(body.value("empty_only").toBool(false));
    const bool datedGiven = present(body, "dated");
    if (datedGiven && !body.value("dated").isBool()) return fail("dated must be true or false");
    const bool datedFlag = body.value("dated").toBool(false);

    QList<int> hours, dows; bool hasHours = false, hasDows = false; QString e;
    if (!readIntList(body, "hours", 0, 23, &hours, &hasHours, &e) || !readIntList(body, "dows", 0, 6, &dows, &hasDows, &e)) return fail(e);
    if (!hasHours) for (int h = 0; h < 24; ++h) hours << h;
    if (!hasDows) for (int d = 0; d < 7; ++d) dows << d;

    const QTimeZone tz = store->channel(ch).tz();
    QDate date, weekStart;
    const QString dateRaw = body.value("date").toString().trimmed(), weekRaw = body.value("weekStart").toString().trimmed();
    if (!dateRaw.isEmpty()) { date = QDate::fromString(dateRaw.left(10), Qt::ISODate); if (!date.isValid()) return fail("bad date '" + dateRaw + "' (use YYYY-MM-DD)"); }
    if (!weekRaw.isEmpty()) { weekStart = QDate::fromString(weekRaw.left(10), Qt::ISODate); if (!weekStart.isValid()) return fail("bad weekStart '" + weekRaw + "' (use YYYY-MM-DD)"); }
    else weekStart = date.isValid() ? date : QDateTime::currentDateTime().toTimeZone(tz).date();
    weekStart = weekStart.addDays(-scheduleDow(weekStart));

    int dow = -1, hour = -1;
    const bool hasDow = present(body, "dow"), hasHour = present(body, "hour");
    if (hasDow && !readInt(body.value("dow"), 0, 6, &dow)) return fail("dow must be an integer 0..6 (Sunday = 0)");
    if (hasHour && !readInt(body.value("hour"), 0, 23, &hour)) return fail("hour must be an integer 0..23");
    if (scope != "hour" && hasHour) return fail("scope '" + scope + "' takes hours:[..]; hour is only for scope 'hour'");
    if ((scope == "hour" || scope == "day") && date.isValid() && hasDow && scheduleDow(date) != dow) return fail("date and dow disagree");

    auto add = [&](bool isDated, const QDate& d, int dw, int h) {
        Target t; t.hour = h; t.dated = isDated;
        if (isDated) { t.date = d; t.dow = scheduleDow(d); t.slotId = HourSlot::dateId(d, h); }
        else { t.dow = dw; t.slotId = HourSlot::templateId(dw, h); t.date = d; }
        t.existing = store->slot(ch, t.slotId, &t.rowExists);
        if (!t.rowExists && t.dated) t.existing = store->effectiveSlot(ch, t.date, h);   // a dated write starts from what the template shows there
        t.existing.channelId = ch;
        if (emptyOnly) { bool hasFree = false; for (const SlotItem& it : t.existing.items) if (!it.locked) { hasFree = true; break; } if (hasFree) return; }
        out.append(t);
    };

    if (scope == "hour") {
        const QString slotId = body.value("slotId").toString().trimmed();
        if (!slotId.isEmpty()) {
            int sdow = -1, shour = 0; QDate d;
            if (!HourSlot::parseId(slotId, &sdow, &d, &shour)) return fail("bad slot id");
            Target t; t.slotId = slotId; t.hour = shour; t.dow = sdow; t.date = d; t.dated = d.isValid();
            t.existing = store->slot(ch, slotId, &t.rowExists); if (!t.rowExists && t.dated) t.existing = store->effectiveSlot(ch, d, shour); t.existing.channelId = ch;
            if (!t.date.isValid()) t.date = weekStart.addDays(sdow);
            out.append(t); return out;
        }
        if (!hasHour || (!date.isValid() && !hasDow)) return fail("scope 'hour' needs a slotId, or a date or dow together with hour");
        const bool isDated = datedGiven ? datedFlag : date.isValid();
        const int dw = hasDow ? dow : scheduleDow(date);
        add(isDated, date.isValid() ? date : weekStart.addDays(dw), dw, hour);   // dated + dow resolves inside the (given or current) week
    } else if (scope == "day") {
        if (!date.isValid() && !hasDow) return fail("scope 'day' needs a date or dow");
        const bool isDated = datedGiven ? datedFlag : date.isValid();
        const int dw = hasDow ? dow : scheduleDow(date);
        const QDate d = date.isValid() ? date : weekStart.addDays(dw);
        for (int h : hours) add(isDated, d, dw, h);
    } else if (scope == "week") {
        for (int dw = 0; dw < 7; ++dw) for (int h : hours) add(datedFlag, weekStart.addDays(dw), dw, h);
    } else {   // hours
        for (int dw : dows) for (int h : hours) add(datedFlag, weekStart.addDays(dw), dw, h);
    }
    return out;
}

/// Recipe sanity: at most kMaxRecipeParts parts, and no part shorter than kMinPartMinutes (a 1e-9 minute part would otherwise lay
/// thousands of blocks per hour). Parts are clamped, too many parts is refused.
bool limitRecipe(HourRecipe& r, QString* why) {
    if (r.parts.size() > kMaxRecipeParts) { if (why) *why = QString("recipe '%1' has %2 parts (at most %3)").arg(r.name).arg(r.parts.size()).arg(kMaxRecipeParts); return false; }
    for (RecipePart& p : r.parts) p.minutes = qMax(p.minutes, kMinPartMinutes);
    return true;
}

HourRecipe recipeFrom(ScheduleStore* store, const QJsonObject& body, QString* why) {
    auto finish = [why](HourRecipe r) { return limitRecipe(r, why) ? r : HourRecipe(); };
    if (body.contains("recipe") && body.value("recipe").isObject()) { const HourRecipe r = HourRecipe::fromJson(body.value("recipe").toObject()); if (r.isValid()) return finish(r); }
    if (body.contains("parts")) { QJsonObject o; o["parts"] = body.value("parts"); o["name"] = body.value("name").toString("ad hoc"); o["idAtTop"] = body.value("idAtTop"); const HourRecipe r = HourRecipe::fromJson(o); if (r.isValid()) return finish(r); }
    const QString id = body.value("recipeId").toString(body.value("recipe").toString());
    if (!id.isEmpty() && id != "default") {
        HourRecipe r;
        if (FormatClock::findRecipe(id, FormatClock::recipesFromSettings(store->settingJson("programming.recipes")), &r)) return finish(r);
        if (why) *why = "recipe '" + id + "' not found";
        return HourRecipe();
    }
    return FormatClock::defaultRecipe();
}

/// What a laid hour will actually play, block by block: every laid item (query / folder block, fixed media, live, lock)
/// with the files the runlist resolved it to, plus the gap fill. `laid` is the hour's items in order.
QJsonObject blocksPreview(const Runlist& rl, const QList<SlotItem>& laid) {
    QJsonArray blocks; QHash<QString, int> at;
    for (const SlotItem& it : laid) {
        QJsonObject b; b["itemId"] = it.id;
        const bool isQuery = !it.query().isEmpty(), isFolder = !isQuery && !it.folder().isEmpty();
        b["kind"] = isQuery ? "query" : isFolder ? "folder" : it.type == "live" ? "live" : "item";
        b["label"] = it.title.isEmpty() ? (it.mediaId.isEmpty() ? it.id : it.mediaId) : it.title;
        b["targetSec"] = it.durationSec; b["type"] = it.type;
        if (it.locked) b["locked"] = true;
        b["picks"] = QJsonArray(); b["filledSec"] = 0.0;
        at.insert(it.id, blocks.size()); blocks.append(b);
    }
    int fillIdx = -1, picks = 0, unresolved = 0;
    for (const RunlistItem& r : rl.items) {
        const QJsonObject rj = r.toJson();
        QString key = rj.value("queryFill").toBool() ? rj.value("blockItemId").toString() : r.item.id;
        int idx = at.value(key, -1);
        if (idx < 0 && r.origin == "fill") {
            if (fillIdx < 0) { QJsonObject f; f["itemId"] = "fill"; f["kind"] = "fill"; f["label"] = "Gap fill"; f["targetSec"] = 0.0; f["picks"] = QJsonArray(); f["filledSec"] = 0.0; fillIdx = blocks.size(); blocks.append(f); }
            idx = fillIdx;
        }
        if (idx < 0) continue;
        QJsonObject b = blocks[idx].toObject();
        QJsonArray pa = b.value("picks").toArray();
        QJsonObject pk;
        pk["mediaId"] = r.item.mediaId; pk["title"] = r.item.title.isEmpty() ? QFileInfo(r.item.path).completeBaseName() : r.item.title;
        pk["path"] = r.item.path; pk["type"] = r.item.type; pk["duration"] = r.durationSec; pk["plannedStartMs"] = double(r.plannedStartMs);
        if (rj.value("overrun").toBool()) pk["overrun"] = true;
        if (rj.value("repeat").toBool()) pk["repeat"] = true;
        if (r.truncated) pk["truncated"] = true;
        pa.append(pk); ++picks;
        b["picks"] = pa; b["filledSec"] = b.value("filledSec").toDouble() + r.durationSec;
        blocks[idx] = b;
    }
    // Blocks that resolved to nothing carry the builder's reason ("block 'X' unresolved: no media matches ...").
    for (int i = 0; i < blocks.size(); ++i) {
        QJsonObject b = blocks[i].toObject();
        if (!b.value("picks").toArray().isEmpty() || b.value("kind").toString() == "fill") continue;
        const QString label = b.value("label").toString(), id = b.value("itemId").toString();
        for (const QString& n : rl.notes)
            if (n.contains("unresolved") && (n.contains("'" + label + "'") || n.contains("'" + id + "'"))) { b["note"] = n.mid(n.indexOf("unresolved") + 12).trimmed(); break; }
        if (!b.contains("note")) b["note"] = "nothing resolved";
        ++unresolved; blocks[i] = b;
    }
    QJsonObject o;
    o["hourStartMs"] = double(rl.hourStartMs); o["slotId"] = rl.slotId;
    o["blocks"] = blocks; o["picks"] = picks; o["unresolved"] = unresolved;
    o["filledSec"] = rl.filledSec; o["gapSec"] = rl.gapSec; o["scheduledSec"] = rl.scheduledSec;
    o["notes"] = QJsonArray::fromStringList(rl.notes);
    return o;
}

/// UTC start of a target hour: its date, or the next occurrence of its weekday for a weekly template hour.
qint64 targetHourStart(const Target& t, const QTimeZone& tz) {
    QDate d = t.date;
    if (!d.isValid() || !t.dated) {
        const QDate today = QDateTime::currentDateTime().toTimeZone(tz).date();
        d = t.date.isValid() ? t.date : today.addDays((t.dow - scheduleDow(today) + 7) % 7);
    }
    return RunlistBuilder::hourStartUtcMs(d, t.hour, tz);
}

QJsonObject facetsOf(const QList<MediaRecord>& all) {
    QMap<QString, int> types, cats, subs, series, clients, tags, folders;
    for (const MediaRecord& m : all) {
        if (!m.type.isEmpty()) types[m.type]++;
        const QString c = QueryBlocks::category(m); if (!c.isEmpty()) cats[c]++;
        const QString s = QueryBlocks::subcategory(m); if (!s.isEmpty()) subs[s]++;
        if (!m.series.isEmpty()) series[m.series]++;
        if (!m.client.isEmpty()) clients[m.client]++;
        for (const QString& t : QueryBlocks::tags(m)) if (!t.isEmpty()) tags[t]++;
        const QString dir = QFileInfo(QDir::fromNativeSeparators(m.path)).path(); if (!dir.isEmpty() && dir != ".") folders[QDir::toNativeSeparators(dir)]++;
    }
    auto pack = [](const QMap<QString, int>& m) { QJsonArray a; for (auto it = m.begin(); it != m.end(); ++it) { QJsonObject o; o["name"] = it.key(); o["count"] = it.value(); a.append(o); } return a; };
    QJsonObject o; o["types"] = pack(types); o["categories"] = pack(cats); o["subcategories"] = pack(subs); o["series"] = pack(series); o["clients"] = pack(clients); o["tags"] = pack(tags); o["folders"] = pack(folders);
    o["prefers"] = QJsonArray{"least_played", "newest", "oldest", "most_played", "random", "longest", "shortest"};
    o["blockTypes"] = QJsonArray{"program", "news", "weather", "ad", "promo", "psa", "station_id", "bumper", "filler", "video", "image"};
    return o;
}

} // namespace

void SupervisorCore::registerProgrammingRoutes() {
    RestApiServer* r = m_rest.get();
    ScheduleStore* st = m_store.get();

    // --- facets, query preview ---------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/programming/facets", [this](const QJsonObject&, const Q&) { return facetsOf(m_store->allMedia(false)); });
    r->registerRoute("POST", "/api/v1/programming/query/preview", [this](const QJsonObject& body, const Q&) {
        // {channel, block:{...} | folder:{...}, n}
        const QString ch = body.value("channel").toString(body.value("channelId").toString());
        const QList<MediaRecord> pool = m_store->allMedia(false);
        QueryContext ctx; ctx.channelId = ch; ctx.nowMs = QDateTime::currentMSecsSinceEpoch();
        QJsonObject o; QJsonArray cands, picks;
        QString why;
        if (body.contains("folder")) {
            const FolderBlock f = FolderBlock::fromJson(body.value("folder").toObject());
            { QJsonArray e; for (const QString& x : f.cond.errors()) e << x; if (!e.isEmpty()) o["ruleErrors"] = e; }
            for (const MediaRecord& m : pool) if (QueryBlocks::matchesFolder(m, f) && (f.cond.isEmpty() || f.cond.matches(m, ctx.nowMs)) && m.isAvailableAt(ctx.nowMs)) { QJsonObject c; c["id"] = m.id; c["title"] = m.title; c["type"] = m.type; c["duration"] = m.durationSec; c["playCount"] = m.playCount; c["lastPlayedMs"] = double(m.lastPlayedMs); cands.append(c); if (cands.size() >= body.value("n").toInt(50)) break; }
            for (const QueryPick& p : QueryBlocks::resolveFolder(pool, f, ctx, &why)) { QJsonObject c; c["id"] = p.media.id; c["title"] = p.media.title; c["duration"] = p.media.durationSec; c["overrun"] = p.overrun; picks.append(c); }
            o["label"] = f.label();
        } else {
            const QueryBlock b = QueryBlock::fromJson(body.value("block").toObject(body.value("queryBlock").toObject(body)));
            if (!b.isValid()) return err(400, "block needs types, categories, subcategories, series, tags, an age limit or a rule");
            { QJsonArray e; for (const QString& x : b.cond.errors()) e << x; if (!e.isEmpty()) o["ruleErrors"] = e; }
            { int total = 0; for (const MediaRecord& m : pool) if (QueryBlocks::matches(m, b, ctx)) ++total; o["matching"] = total; }
            for (const QueryPick& p : QueryBlocks::candidates(pool, b, ctx, body.value("n").toInt(50))) { QJsonObject c; c["id"] = p.media.id; c["title"] = p.media.title; c["type"] = p.media.type; c["duration"] = p.media.durationSec; c["playCount"] = p.media.playCount; c["lastPlayedMs"] = double(p.media.lastPlayedMs); c["addedAtMs"] = double(p.media.addedAtMs); c["repeat"] = p.repeat; c["category"] = QueryBlocks::category(p.media); c["subcategory"] = QueryBlocks::subcategory(p.media); c["series"] = p.media.series; cands.append(c); }
            for (const QueryPick& p : QueryBlocks::resolve(pool, b, ctx, &why)) { QJsonObject c; c["id"] = p.media.id; c["title"] = p.media.title; c["duration"] = p.media.durationSec; c["overrun"] = p.overrun; c["repeat"] = p.repeat; picks.append(c); }
            o["label"] = b.label(); o["block"] = b.toJson();
        }
        o["candidates"] = cands; o["picks"] = picks; if (!why.isEmpty()) o["why"] = why;
        return o;
    });

    // --- hour recipes -----------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/programming/recipes", [this](const QJsonObject&, const Q&) {
        QJsonObject o = FormatClock::recipesToSettings(FormatClock::recipesFromSettings(m_store->settingJson("programming.recipes")));
        o["default"] = FormatClock::defaultRecipe().toJson();
        { QJsonArray b; for (const HourRecipe& x : FormatClock::builtinRecipes()) b.append(x.toJson()); o["builtin"] = b; }
        o["types"] = QJsonArray{"program", "news", "weather", "ad", "promo", "psa", "station_id", "bumper", "filler"};
        o["prefers"] = QJsonArray{"even", "least_played", "newest", "oldest", "most_played", "random", "longest", "shortest"};
        return o;
    });
    r->registerRoute("PUT", "/api/v1/programming/recipes", [this](const QJsonObject& body, const Q&) {
        QList<HourRecipe> clean;
        for (const QJsonValue& v : body.value("recipes").toArray()) {
            HourRecipe x = HourRecipe::fromJson(v.toObject());
            if (x.isValid() && !x.name.trimmed().isEmpty()) { QString why; if (!limitRecipe(x, &why)) return err(400, why); clean.append(x); }
            if (clean.size() >= 48) break;
        }
        m_store->setSettingJson("programming.recipes", FormatClock::recipesToSettings(clean));
        QJsonObject o = FormatClock::recipesToSettings(clean); o["default"] = FormatClock::defaultRecipe().toJson(); return o;
    });
    r->registerRoute("POST", "/api/v1/programming/recipes/ad-break", [this](const QJsonObject& body, const Q&) {
        // {programMin, adMin, programTypes[], adTypes[], prefer, adPrefer, autogenMin, idEveryBreak, idAtTop, name, save}
        const HourRecipe rec = FormatClock::adBreakRecipe(body);
        if (body.value("save").toBool(false)) {
            QList<HourRecipe> all = FormatClock::recipesFromSettings(m_store->settingJson("programming.recipes"));
            bool replaced = false; for (auto& x : all) if (x.id == rec.id) { x = rec; replaced = true; }
            if (!replaced) all.append(rec);
            m_store->setSettingJson("programming.recipes", FormatClock::recipesToSettings(all));
        }
        QJsonObject o = rec.toJson();
        const HourLayout lay = FormatClock::layRecipe(rec, HourSlot(), false);
        o["preview"] = lay.toJson();
        // With a channel: the files the next hour would play from this recipe.
        const QString ch = body.value("channel").toString();
        bool f = false; const ChannelProfile p = ch.isEmpty() ? ChannelProfile() : m_store->channel(ch, &f);
        if (f) {
            QDate d; int h = 0; RunlistBuilder::hourStartUtcMsForTime(QDateTime::currentMSecsSinceEpoch() + 3600000LL, p.tz(), &d, &h);
            HourSlot s; s.channelId = ch; s.id = HourSlot::dateId(d, h); s.date = d; s.dayOfWeek = -1; s.hour = h; s.items = lay.items;
            const Runlist rl = m_exec->previewSlot(ch, s, RunlistBuilder::hourStartUtcMs(d, h, p.tz()));
            o["resolved"] = blocksPreview(rl, lay.items);
        }
        return o;
    });
    r->registerRoute("POST", "/api/v1/programming/recipes/preview", [this](const QJsonObject& body, const Q&) {
        // {recipeId | recipe{} | parts[], channel, date (YYYY-MM-DD), hour, hours 1..24}: lays the recipe into consecutive hours
        // (keeping each hour's locked items) and resolves them with one shared pick cache: which files each block would play.
        QString why;
        const HourRecipe rec = recipeFrom(m_store.get(), body, &why);
        if (!rec.isValid()) return err(400, why.isEmpty() ? "recipe has no parts" : why);
        QString ch = body.value("channel").toString(body.value("channelId").toString());
        if (ch.isEmpty()) { const QList<ChannelProfile> all = m_store->channels(); if (!all.isEmpty()) ch = all.first().id; }
        bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (!f) return err(404, "no such channel");
        const QTimeZone tz = p.tz();
        QDate d; int h = 0;
        RunlistBuilder::hourStartUtcMsForTime(QDateTime::currentMSecsSinceEpoch() + 3600000LL, tz, &d, &h);   // default: the next hour
        if (body.contains("date")) { const QDate x = QDate::fromString(body.value("date").toString().left(10), Qt::ISODate); if (!x.isValid()) return err(400, "bad date (use YYYY-MM-DD)"); d = x; }
        if (body.contains("hour")) { int x = -1; if (!readInt(body.value("hour"), 0, 23, &x)) return err(400, "hour must be 0..23"); h = x; }
        const int hours = qBound(1, body.value("hours").toInt(1), 24);
        const bool keepLocks = body.value("keepLocks").toBool(true);
        QHash<QString, QStringList> cache;
        QJsonArray out; int picks = 0, unresolved = 0;
        for (int k = 0; k < hours; ++k) {
            const QDateTime at = QDateTime(d, QTime(h, 0), tz).addSecs(3600LL * k);
            const QDate dd = at.date(); const int hh = at.time().hour();
            const HourSlot existing = m_store->effectiveSlot(ch, dd, hh);
            const HourLayout lay = FormatClock::layRecipe(rec, existing, keepLocks);
            HourSlot s = existing; s.channelId = ch; s.id = HourSlot::dateId(dd, hh); s.date = dd; s.dayOfWeek = -1; s.hour = hh; s.items = lay.items;
            const Runlist rl = m_exec->previewSlot(ch, s, RunlistBuilder::hourStartUtcMs(dd, hh, tz), &cache);
            QJsonObject ho = blocksPreview(rl, lay.items);
            ho["date"] = dd.toString(Qt::ISODate); ho["hour"] = hh;
            picks += ho.value("picks").toInt(); unresolved += ho.value("unresolved").toInt();
            out.append(ho);
        }
        QJsonObject o; o["recipe"] = rec.toJson(); o["channel"] = ch; o["hours"] = out;
        QJsonObject sm; sm["hours"] = hours; sm["picks"] = picks; sm["unresolvedBlocks"] = unresolved; o["summary"] = sm;
        return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/hour-recipe", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"];
        QString why;
        const HourRecipe rec = recipeFrom(m_store.get(), body, &why);
        if (!rec.isValid()) return err(400, why.isEmpty() ? "recipe has no parts" : why);
        const QList<Target> targets = targetsFor(m_store.get(), ch, body, &why);
        if (targets.isEmpty()) {
            if (!why.isEmpty()) return err(400, why);                       // an unresolvable scope is a bad request, not "0 hours"
            QJsonObject o; o["slots"] = QJsonObject(); QJsonObject s; s["targets"] = 0; s["written"] = 0; s["applied"] = body.value("apply").toBool(false); s["note"] = "no hours in scope"; o["summary"] = s; return o;
        }
        const bool keepLocks = body.value("keepLocks").toBool(body.value("keep_locks").toBool(true));
        const bool apply = body.value("apply").toBool(false);
        // A preview (apply false) also resolves the laid hours, so the operator sees which files each block will play.
        // One pick cache across the previewed hours: rotation spreads across them as it will on air. At most 24 hours.
        const bool resolve = !apply && body.value("resolve").toBool(true);
        const QTimeZone tz = m_store->channel(ch).tz();
        QHash<QString, QStringList> previewPicks;
        QJsonObject slotMap, previews, runlists; int written = 0, previewed = 0;
        for (const Target& t : targets) {
            const HourLayout lay = FormatClock::layRecipe(rec, t.existing, keepLocks);
            HourSlot s = t.existing; s.channelId = ch; s.id = t.slotId; s.hour = t.hour;
            if (t.dated) { s.date = t.date; s.dayOfWeek = -1; } else { s.dayOfWeek = t.dow; s.date = QDate(); }
            s.items = lay.items; s.showBlock["recipe"] = rec.name; s.showBlock.remove("_override");
            QJsonObject so = s.toJson(); so["notes"] = QJsonArray::fromStringList(lay.notes); so["placedSec"] = lay.placedSec; slotMap[t.slotId] = so;
            if (apply && !lay.items.isEmpty()) { if (m_store->saveSlot(s)) ++written; }
            if (resolve && previewed < 24) {
                const Runlist rl = m_exec->previewSlot(ch, s, targetHourStart(t, tz), &previewPicks);
                previews[t.slotId] = blocksPreview(rl, lay.items);
                runlists[t.slotId] = rl.toJson();
                ++previewed;
            }
        }
        if (apply) pushAlert(ch, "info", QString("recipe '%1' laid into %2 hour(s)").arg(rec.name).arg(written));
        QJsonObject o; o["slots"] = slotMap;
        if (resolve) { o["previews"] = previews; o["runlists"] = runlists; }
        QJsonObject s; s["targets"] = targets.size(); s["written"] = written; s["recipe"] = rec.toJson(); s["cycleSec"] = rec.cycleSec(); s["applied"] = apply; o["summary"] = s;
        return o;
    });

    // --- weighted fill (format clock) -----------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/channels/{id}/weights", [this](const QJsonObject&, const Q& q) {
        bool f = false; const ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        QList<WeightBucket> b = FormatClock::bucketsFromJson(p.extra.value("programWeights"));
        QJsonObject o; o["buckets"] = FormatClock::bucketsToJson(b.isEmpty() ? FormatClock::defaultBuckets() : b); o["isDefault"] = b.isEmpty();
        double tot = 0; for (const auto& x : (b.isEmpty() ? FormatClock::defaultBuckets() : b)) tot += x.pct; o["totalPct"] = tot;
        return o;
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}/weights", [this](const QJsonObject& body, const Q& q) {
        bool f = false; ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        const QList<WeightBucket> b = FormatClock::bucketsFromJson(body.contains("buckets") ? body.value("buckets") : QJsonValue(body));
        if (b.size() > kMaxBuckets) return err(400, QString("at most %1 buckets").arg(kMaxBuckets));
        QJsonObject pw; pw["buckets"] = FormatClock::bucketsToJson(b); p.extra["programWeights"] = pw;
        m_store->saveChannel(p); m_exec->reloadChannel(p.id);
        QJsonObject o; o["buckets"] = pw["buckets"]; return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/weighted-fill", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"];
        bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (!f) return err(404, "no such channel");
        QList<WeightBucket> buckets = body.contains("buckets") ? FormatClock::bucketsFromJson(body.value("buckets")) : FormatClock::bucketsFromJson(p.extra.value("programWeights"));
        if (buckets.isEmpty()) buckets = FormatClock::defaultBuckets();
        if (buckets.size() > kMaxBuckets) return err(400, QString("at most %1 buckets").arg(kMaxBuckets));
        // programCapSec: 300..7200 s (default 2400). <= 0 means "never split a program". A zero or negative cap would otherwise make
        // fillWeighted carry the same program, from the same inPoint, into every following adjacent hour.
        double cap = 2400.0;
        if (present(body, "programCapSec")) {
            if (!body.value("programCapSec").isDouble()) return err(400, "programCapSec must be a number of seconds (300..7200, or 0 to never split a program)");
            const double v = body.value("programCapSec").toDouble();
            cap = v <= 0.0 ? kNoSplitSec : qBound(kMinCapSec, v, kMaxCapSec);
        }
        QString why;
        const QList<Target> targets = targetsFor(m_store.get(), ch, body, &why);
        if (targets.isEmpty()) {
            if (!why.isEmpty()) return err(400, why);
            QJsonObject o; o["slots"] = QJsonObject(); QJsonObject s; s["targets"] = 0; s["written"] = 0; s["placed"] = 0; s["applied"] = body.value("apply").toBool(false); s["note"] = "no hours in scope"; o["summary"] = s; return o;
        }
        const bool keepLocks = body.value("keepLocks").toBool(true), apply = body.value("apply").toBool(false), onePer = body.value("oneProgramPerHour").toBool(true);
        const QList<MediaRecord> pool = m_store->allMedia(false);
        auto state = FormatClock::beginWeighted(buckets, pool, ch, QDateTime::currentMSecsSinceEpoch(), quint32(body.value("seed").toInt(1)));
        QJsonObject slotMap; int written = 0, placed = 0; QJsonObject poolSizes; for (int i = 0; i < buckets.size(); ++i) poolSizes[buckets[i].label] = state.queues[i].size();
        QString prevId;
        for (const Target& t : targets) {
            // a continuation only flows into the physically adjacent hour
            if (!prevId.isEmpty() && !state.carry.isEmpty()) {
                int pd = -1, ph = 0; QDate pdate; HourSlot::parseId(prevId, &pd, &pdate, &ph);
                const bool adjacent = (t.dated && pdate.isValid()) ? (QDateTime(pdate, QTime(ph, 0)).addSecs(3600) == QDateTime(t.date, QTime(t.hour, 0))) : ((pd * 24 + ph + 1) % 168 == (t.dow * 24 + t.hour) % 168);
                if (!adjacent) state.carry = QJsonObject();
            }
            const HourLayout lay = FormatClock::fillWeighted(buckets, state, t.existing, keepLocks, onePer, cap);
            HourSlot s = t.existing; s.channelId = ch; s.id = t.slotId; s.hour = t.hour;
            if (t.dated) { s.date = t.date; s.dayOfWeek = -1; } else { s.dayOfWeek = t.dow; s.date = QDate(); }
            s.items = lay.items; s.showBlock["weightedFill"] = true; s.showBlock.remove("_override");
            QJsonObject so = s.toJson(); so["notes"] = QJsonArray::fromStringList(lay.notes); so["placedSec"] = lay.placedSec; slotMap[t.slotId] = so;
            placed += lay.items.size(); prevId = t.slotId;
            if (apply && !lay.items.isEmpty()) { if (m_store->saveSlot(s)) ++written; }   // never blank an hour the pool could not fill
        }
        if (apply) pushAlert(ch, "info", QString("weighted fill: %1 hour(s), %2 item(s)").arg(written).arg(placed));
        QJsonObject o; o["slots"] = slotMap;
        QJsonObject s; s["targets"] = targets.size(); s["written"] = written; s["placed"] = placed; s["eligible"] = pool.size(); s["perBucketPool"] = poolSizes; s["applied"] = apply; s["programCapSec"] = cap >= kNoSplitSec ? 0.0 : cap; s["buckets"] = FormatClock::bucketsToJson(buckets); o["summary"] = s;
        return o;
    });

    // --- autofill policy -----------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/channels/{id}/autofill", [this](const QJsonObject&, const Q& q) {
        bool f = false; const ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        QJsonObject o = FormatClock::policyFor(p).toJson(); o["channelId"] = p.id; return o;
    });
    r->registerRoute("PUT", "/api/v1/channels/{id}/autofill", [this](const QJsonObject& body, const Q& q) {
        bool f = false; ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        AutofillPolicy cur = FormatClock::policyFor(p); QJsonObject merged = cur.toJson(); for (auto it = body.begin(); it != body.end(); ++it) merged[it.key()] = it.value();
        p.extra["autofill"] = AutofillPolicy::fromJson(merged).toJson();
        m_store->saveChannel(p); m_exec->reloadChannel(p.id);
        QJsonObject o = p.extra.value("autofill").toObject(); o["channelId"] = p.id; return o;
    });

    // --- ad contracts ----------------------------------------------------------------------------------------
    r->registerRoute("GET", "/api/v1/traffic/contracts", [this](const QJsonObject&, const Q& q) {
        const QList<AdContract> all = FormatClock::contractsFromSettings(m_store->settingJson("traffic.contracts"));
        const QString ch = q.value("channel"); const QTimeZone tz = m_store->channel(ch.isEmpty() ? "main" : ch).tz();
        const QDate today = QDateTime::currentDateTime().toTimeZone(tz).date();
        ScheduleStore* st2 = m_store.get();
        QJsonArray a; for (const AdContract& c : all) a.append(FormatClock::contractReport(c, today, [st2, ch](const QString& id, qint64 f, qint64 t) { return st2->airedCount(id, f, t, ch); }, tz));
        QJsonObject o; o["contracts"] = a; return o;
    });
    r->registerRoute("PUT", "/api/v1/traffic/contracts", [this](const QJsonObject& body, const Q&) {
        const QJsonArray in = body.value("contracts").toArray();
        if (in.size() > kMaxContracts) return err(400, QString("at most %1 contracts").arg(kMaxContracts));
        QList<AdContract> clean; for (const QJsonValue& v : in) clean.append(AdContract::fromJson(v.toObject()));
        m_store->setSettingJson("traffic.contracts", FormatClock::contractsToSettings(clean));
        for (const QString& id : m_exec->channelIds()) m_exec->rerollBlocks(id);
        return FormatClock::contractsToSettings(clean);
    });
    r->registerRoute("GET", "/api/v1/traffic/contracts/{id}", [this](const QJsonObject&, const Q& q) {
        for (const AdContract& c : FormatClock::contractsFromSettings(m_store->settingJson("traffic.contracts"))) if (c.id == q["id"]) {
            const QTimeZone tz = m_store->channel(c.channels.isEmpty() ? "main" : c.channels.first()).tz();
            ScheduleStore* st2 = m_store.get();
            return FormatClock::contractReport(c, QDateTime::currentDateTime().toTimeZone(tz).date(), [st2](const QString& id, qint64 f, qint64 t) { return st2->airedCount(id, f, t); }, tz);
        }
        return err(404, "no such contract");
    });
    r->registerRoute("GET", "/api/v1/channels/{id}/pacing", [this](const QJsonObject&, const Q& q) {
        bool f = false; const ChannelProfile p = m_store->channel(q["id"], &f); if (!f) return err(404, "no such channel");
        ScheduleStore* st2 = m_store.get();
        const QDate today = QDateTime::currentDateTime().toTimeZone(p.tz()).date();
        const QHash<QString, double> pacing = FormatClock::contractPacing(FormatClock::contractsFromSettings(m_store->settingJson("traffic.contracts")), p.id, today, p.tz(), [st2](const QString& id, qint64 a, qint64 b) { return st2->airedCount(id, a, b); });
        QJsonObject o, bonus; for (auto it = pacing.begin(); it != pacing.end(); ++it) bonus[it.key()] = it.value(); o["bonus"] = bonus; o["date"] = today.toString(Qt::ISODate); return o;
    });

    // --- hour tools: resolved preview, clear, shuffle, lock, re-roll --------------------------------------------
    r->registerRoute("POST", "/api/v1/schedule/{ch}/slot/{slot}/resolved", [this](const QJsonObject& body, const Q& q) {
        // Body = slot JSON (unsaved editor state) or empty for the stored slot. Returns the runlist the executor would build.
        const QString ch = q["ch"]; bool f = false; const ChannelProfile p = m_store->channel(ch, &f); if (!f) return err(404, "no such channel");
        int dow = -1, hour = 0; QDate d; if (!HourSlot::parseId(q["slot"], &dow, &d, &hour)) return err(400, "bad slot id");
        HourSlot s = body.isEmpty() || !body.contains("items") ? (d.isValid() ? m_store->effectiveSlot(ch, d, hour) : m_store->slot(ch, q["slot"])) : HourSlot::fromJson(body);
        s.channelId = ch; s.id = q["slot"]; s.hour = hour; if (d.isValid()) { s.date = d; s.dayOfWeek = -1; } else s.dayOfWeek = dow;
        QDate day = d; if (!day.isValid()) { day = QDateTime::currentDateTime().toTimeZone(p.tz()).date(); day = day.addDays((dow - scheduleDow(day) + 7) % 7); }
        const Runlist rl = m_exec->previewSlot(ch, s, RunlistBuilder::hourStartUtcMs(day, hour, p.tz()));
        QJsonObject o = rl.toJson(); o["date"] = day.toString(Qt::ISODate); return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/slot/{slot}/overrun", [this](const QJsonObject& body, const Q& q) {
        // A programme that runs past the top of the hour: {slot (editor state, optional), itemId, mode continue|split|cut,
        // next keep|ads|recipe, recipeId, adTypes[], breakMin, breakEveryMin, apply (default true)}. Plans both hours with
        // FormatClock::planOverrun and saves them; the next hour follows the slot kind (weekly template or dated).
        const QString ch = q["ch"];
        bool f = false; m_store->channel(ch, &f); if (!f) return err(404, "no such channel");
        int dow = -1, hour = 0; QDate d;
        if (!HourSlot::parseId(q["slot"], &dow, &d, &hour)) return err(400, "bad slot id");
        HourSlot cur = body.value("slot").isObject() ? HourSlot::fromJson(body.value("slot").toObject()) : m_store->slot(ch, q["slot"]);
        cur.channelId = ch; cur.id = q["slot"]; cur.hour = hour;
        if (d.isValid()) { cur.date = d; cur.dayOfWeek = -1; } else { cur.dayOfWeek = dow; cur.date = QDate(); }
        int ndow = dow, nhour = hour + 1; QDate nd = d;
        if (nhour > 23) { nhour = 0; if (nd.isValid()) nd = nd.addDays(1); else ndow = (dow + 1) % 7; }
        const QString nextId = nd.isValid() ? HourSlot::dateId(nd, nhour) : HourSlot::templateId(ndow, nhour);
        bool nf = false;
        HourSlot next = m_store->slot(ch, nextId, &nf);
        if (!nf && nd.isValid()) next = m_store->effectiveSlot(ch, nd, nhour);   // a dated write starts from what the template shows there
        next.channelId = ch; next.id = nextId; next.hour = nhour;
        if (nd.isValid()) { next.date = nd; next.dayOfWeek = -1; } else { next.dayOfWeek = ndow; next.date = QDate(); }

        FormatClock::OverrunPlan plan;
        plan.mode = body.value("mode").toString("continue");
        plan.next = body.value("next").toString("keep");
        const QStringList at = QueryBlocks::listOf(body, "adTypes"); if (!at.isEmpty()) plan.adTypes = at;
        if (body.contains("breakMin")) plan.breakSec = qBound(15.0, body.value("breakMin").toDouble() * 60.0, 1200.0);
        if (body.contains("breakEveryMin")) plan.breakEverySec = qBound(60.0, body.value("breakEveryMin").toDouble() * 60.0, 3600.0);
        if (plan.next == "recipe") {
            QString why; QJsonObject rb; rb["recipeId"] = body.value("recipeId").toString("default");
            plan.recipe = recipeFrom(m_store.get(), rb, &why);
            if (!plan.recipe.isValid()) return err(400, why.isEmpty() ? QString("recipe not found") : why);
        }
        const QString itemId = body.value("itemId").toString();
        if (plan.mode == "split" && m_transcribe)
            for (const SlotItem& it : cur.items) if (it.id == itemId && !it.mediaId.isEmpty()) { plan.segments = m_transcribe->segments(it.mediaId).value("segments").toArray(); break; }

        const FormatClock::OverrunResult res = FormatClock::planOverrun(cur, itemId, next, plan);
        if (!res.ok) return err(400, res.error);
        const bool apply = body.value("apply").toBool(true);
        if (apply) {
            if (!m_store->saveSlot(res.cur)) return err(500, "could not save " + res.cur.id);
            if (!m_store->saveSlot(res.next)) return err(500, "could not save " + res.next.id);
            pushAlert(ch, "info", QString("overrun in %1 (%2): %3").arg(res.cur.id, plan.mode, res.notes.value(0)));
        }
        QJsonObject o;
        o["slot"] = res.cur.toJson(); o["next"] = res.next.toJson(); o["nextSlotId"] = nextId;
        o["overrunSec"] = res.overrunSec; o["carrySec"] = res.carrySec; o["notes"] = QJsonArray::fromStringList(res.notes); o["applied"] = apply;
        return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/clear", [this](const QJsonObject& body, const Q& q) {
        const QString ch = q["ch"]; QString why;
        const QList<Target> targets = targetsFor(m_store.get(), ch, body, &why);
        if (targets.isEmpty() && !why.isEmpty()) return err(400, why);
        const bool keepLocks = body.value("keepLocks").toBool(true);
        // Clearing empties the hour but keeps its row (show title, device commands, loop override, block type) and, for a dated hour,
        // writes an (empty or locks-only) override so the template cannot show through. Only hours that actually changed are counted.
        int n = 0; QJsonArray ids;
        for (const Target& t : targets) {
            QList<SlotItem> keep; if (keepLocks) for (const SlotItem& it : t.existing.items) if (it.locked) keep.append(it);
            if (keep.size() == t.existing.items.size()) continue;       // already empty (or nothing but locks): nothing to clear
            HourSlot s = t.existing; s.channelId = ch; s.id = t.slotId; s.hour = t.hour; s.items = keep; s.showBlock.remove("_override");
            if (t.dated) { s.date = t.date; s.dayOfWeek = -1; } else { s.dayOfWeek = t.dow; s.date = QDate(); }
            if (m_store->saveSlot(s)) { ++n; ids << t.slotId; }
        }
        QJsonObject o; o["cleared"] = n; o["slots"] = ids; return o;
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/slot/{slot}/shuffle", [this](const QJsonObject& body, const Q& q) {
        bool f = false; HourSlot s = m_store->slot(q["ch"], q["slot"], &f); if (!f) return err(404, "no such slot");
        s.items = FormatClock::shuffleAds(s.items, quint32(body.value("seed").toInt(0)));
        if (!m_store->saveSlot(s)) return err(500, "save failed");
        return s.toJson();
    });
    r->registerRoute("POST", "/api/v1/schedule/{ch}/slot/{slot}/lock", [this](const QJsonObject& body, const Q& q) {
        // {itemId, locked, lockAtSec (-1 clears), lockLabel}
        bool f = false; HourSlot s = m_store->slot(q["ch"], q["slot"], &f); if (!f) return err(404, "no such slot");
        const QString itemId = body.value("itemId").toString(); bool hit = false;
        for (SlotItem& it : s.items) {
            if (it.id != itemId) continue; hit = true;
            if (body.contains("locked")) it.locked = body.value("locked").toBool();
            if (body.contains("lockAtSec")) { const double v = body.value("lockAtSec").toDouble(-1); it.lockAtSec = v >= 0 ? qBound(0.0, v, 3600.0) : -1.0; if (v >= 0) it.locked = true; }
            if (body.contains("lockLabel")) it.lockLabel = body.value("lockLabel").toString().left(24);
            if (!it.locked) { it.lockAtSec = -1.0; it.lockLabel.clear(); }
        }
        if (!hit) return err(404, "no such item in slot");
        if (!m_store->saveSlot(s)) return err(500, "save failed");
        return s.toJson();
    });
    r->registerRoute("POST", "/api/v1/channels/{id}/reroll", [this](const QJsonObject&, const Q& q) {
        if (!m_exec->hasChannel(q["id"])) return err(404, "no such channel");
        m_exec->rerollBlocks(q["id"]); QJsonObject o; o["ok"] = true; o["channelId"] = q["id"]; return o;
    });
    // Copy an hour / day / week onto another day or week (template or dated).
    r->registerRoute("POST", "/api/v1/schedule/{ch}/copy", [this](const QJsonObject& body, const Q& q) {
        // {from:{scope, slotId|date|dow|weekStart, dated, hours[], dows[], hour}, to:{date|dow|weekStart, hour?, dated}}
        //
        // Source: whatever targetsFor selects. A `date` (or dated:true) reads the hour as shown on that date (dated override, else the template);
        // a bare dow reads the weekly template. Empty hours are sources too: copying replaces the destination, it does not merge into it.
        // Destination (nothing is guessed, an unresolvable `to` is a 400 before anything is written):
        //   dated: to.dated when given; else true when to.date / to.weekStart is given; else false when only to.dow is given (a weekday names the
        //          weekly template); else the source's own kind (a dated hour copied to {hour} stays dated, on the same date).
        //   to.date    a single-day source lands on that date; a multi-day source (week / hours) lands in the week of that date, weekdays kept.
        //   to.weekStart + to.dow (single-day source) names the day inside that week; without to.dow every source day keeps its weekday.
        //   to.hour    single-hour source only.
        const QString ch = q["ch"]; QString why;
        QJsonObject from = body.value("from").toObject(); from["emptyOnly"] = false;
        const QList<Target> src = targetsFor(m_store.get(), ch, from, &why);
        if (src.isEmpty()) return err(400, why.isEmpty() ? "nothing to copy" : why);
        const QJsonObject to = body.value("to").toObject();
        QDate toDate, toWeek; int toDow = -1, toHour = -1;
        if (present(to, "date")) { toDate = QDate::fromString(to.value("date").toString().trimmed().left(10), Qt::ISODate); if (!toDate.isValid()) return err(400, "bad to.date (use YYYY-MM-DD)"); }
        if (present(to, "weekStart")) { toWeek = QDate::fromString(to.value("weekStart").toString().trimmed().left(10), Qt::ISODate); if (!toWeek.isValid()) return err(400, "bad to.weekStart (use YYYY-MM-DD)"); toWeek = toWeek.addDays(-scheduleDow(toWeek)); }
        if (present(to, "dow") && !readInt(to.value("dow"), 0, 6, &toDow)) return err(400, "to.dow must be an integer 0..6 (Sunday = 0)");
        if (present(to, "hour") && !readInt(to.value("hour"), 0, 23, &toHour)) return err(400, "to.hour must be an integer 0..23");
        if (present(to, "dated") && !to.value("dated").isBool()) return err(400, "to.dated must be true or false");
        if (!toDate.isValid() && !toWeek.isValid() && toDow < 0 && toHour < 0 && !present(to, "dated")) return err(400, "to needs a date, weekStart, dow, hour or dated");
        QSet<int> srcDows; for (const Target& t : src) srcDows.insert(t.dow);
        const bool singleDay = srcDows.size() == 1, srcDated = src.first().dated;
        if (toDow >= 0 && !singleDay) return err(400, "to.dow needs a single-day source (a week or multi-day copy keeps each weekday)");
        if (toHour >= 0 && src.size() != 1) return err(400, "to.hour needs a single-hour source");
        if (toDate.isValid() && toDow >= 0 && scheduleDow(toDate) != toDow) return err(400, "to.date and to.dow disagree");
        const bool toDated = present(to, "dated") ? to.value("dated").toBool() : (toDate.isValid() || toWeek.isValid() ? true : (toDow >= 0 ? false : srcDated));
        if (!toDated && (toDate.isValid() || toWeek.isValid())) return err(400, "to.date / to.weekStart address dated hours: to.dated must not be false");
        if (toDated && !toDate.isValid() && !toWeek.isValid() && !srcDated) return err(400, "to.date or to.weekStart required for a dated copy");
        if (!toDate.isValid() && !toWeek.isValid() && toDow < 0 && toHour < 0 && toDated == srcDated) return err(400, "the destination is the source: name a date, weekStart, dow or hour");
        int n = 0; QJsonArray ids;
        for (const Target& t : src) {
            const int hour = toHour >= 0 ? toHour : t.hour;
            HourSlot s = t.existing; s.channelId = ch; s.showBlock.remove("_override");
            QDate d; int dow = t.dow;
            if (toDated) {
                if (toDate.isValid()) d = singleDay ? toDate : toDate.addDays(-scheduleDow(toDate)).addDays(t.dow);
                else d = (toWeek.isValid() ? toWeek : t.date.addDays(-t.dow)).addDays(singleDay && toDow >= 0 ? toDow : t.dow);   // dated source: its own week
            } else if (singleDay && toDow >= 0) dow = toDow;
            const QString destId = toDated ? HourSlot::dateId(d, hour) : HourSlot::templateId(dow, hour);
            s.id = destId; s.hour = hour;
            if (toDated) { s.date = d; s.dayOfWeek = -1; } else { s.date = QDate(); s.dayOfWeek = dow; }
            if (s.items.isEmpty()) {
                // An empty source empties the destination instead of leaving its old content behind.
                bool destRow = false; m_store->slot(ch, destId, &destRow);
                if (toDated) { if (!destRow && m_store->effectiveSlot(ch, d, hour).items.isEmpty()) continue; }   // nothing shows there already: no clutter
                else if (!t.rowExists) { if (destRow && m_store->deleteSlot(ch, destId)) { ++n; ids << destId; } continue; }
                if (m_store->saveSlot(s)) { ++n; ids << destId; }      // an empty override hides the template; an empty source row carries its flags over
                continue;
            }
            for (SlotItem& it : s.items) if (!it.locked) it.id = FormatClock::newItemId("c");
            if (m_store->saveSlot(s)) { ++n; ids << s.id; }
        }
        QJsonObject o; o["written"] = n; o["slots"] = ids; return o;
    });

    qInfo() << "[Supervisor] programming routes: recipes, weighted fill, autofill, contracts, locks";
    Q_UNUSED(st);
}
