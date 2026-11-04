#include "formatclock.h"
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QRandomGenerator>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace sched {

static QJsonArray arr(const QStringList& l) { QJsonArray a; for (const auto& s : l) a << s; return a; }
static constexpr double kHourSec = 3600.0;
static constexpr double kFillTolerance = 12.0;     // stop when within this of a full hour
static constexpr double kAnchorTolerance = 30.0;   // an anchor goes down when the cursor is within this of its offset

namespace {
bool isProgramType(const QString& t) { return t == "program" || t == "video" || t == "movie" || t == "episode"; }
/// A recipe part that airs programs (or an episode series): never the filler that closes the last gap of an hour.
bool partIsProgram(const RecipePart& p) {
    if (!p.series.isEmpty()) return true;
    for (const QString& t : p.types) if (isProgramType(QueryBlocks::norm(t))) return true;
    return p.types.isEmpty() && !p.folder.isEmpty();     // "fill from this folder" is programming
}
} // namespace

QString FormatClock::newItemId(const QString& prefix) {
    return prefix + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

double FormatClock::lockOffset(const SlotItem& it) {
    const double a = it.anchorSec();
    return a >= 0.0 ? a : (it.isHardStart() ? it.fixedOffsetSec : -1.0);
}

// ---------------------------------------------------------------------------
// Recipes
// ---------------------------------------------------------------------------

RecipePart RecipePart::fromJson(const QJsonObject& o) {
    RecipePart p;
    p.label = o.value("label").toString();
    p.types = QueryBlocks::listOf(o, "types", "type");
    p.minutes = o.value("minutes").toDouble(o.value("targetSec").toDouble(0.0) / 60.0);
    p.minutes = qBound(0.0, p.minutes, 60.0);
    const QString pref = o.value("prefer").toString("least_played");
    static const QStringList prefers = {"even", "newest", "oldest", "most_played", "least_played", "random", "longest", "shortest"};
    p.prefer = prefers.contains(pref) ? pref : "least_played";
    p.categories = QueryBlocks::listOf(o, "categories", "category");
    p.subcategories = QueryBlocks::listOf(o, "subcategories", "subcategory");
    p.series = QueryBlocks::listOf(o, "series", "seriesName");
    p.tags = QueryBlocks::listOf(o, "tags", "tag");
    p.anchorTop = o.value("anchorTop").toBool(false);
    p.folder = o.value("folder").toString().trimmed();
    p.cond = Conditions::fromJson(o);
    if (p.label.isEmpty()) p.label = !p.folder.isEmpty() ? QFileInfo(QDir::fromNativeSeparators(p.folder)).fileName()
                                  : p.types.isEmpty() ? (p.categories.isEmpty() ? "Block" : p.categories.join("/")) : p.types.join("/");
    return p;
}

QJsonObject RecipePart::toJson() const {
    QJsonObject o; o["label"] = label; o["types"] = arr(types); o["minutes"] = minutes; o["prefer"] = prefer;
    if (!categories.isEmpty()) o["categories"] = arr(categories);
    if (!subcategories.isEmpty()) o["subcategories"] = arr(subcategories);
    if (!series.isEmpty()) o["series"] = arr(series);
    if (!tags.isEmpty()) o["tags"] = arr(tags);
    if (anchorTop) o["anchorTop"] = true;
    if (!folder.isEmpty()) o["folder"] = folder;
    cond.writeJson(o);
    return o;
}

QueryBlock RecipePart::block() const {
    QueryBlock b; b.types = types; b.categories = categories; b.subcategories = subcategories; b.series = series; b.tags = tags;
    b.targetSec = minutes * 60.0; b.prefer = prefer;
    b.cond = cond;
    if (!folder.isEmpty()) {
        // A folder part is a query block with a folder rule, so it keeps the part's prefer, age limits and rules.
        const QString f = QDir::fromNativeSeparators(folder);
        QJsonObject fr;
        if (f.contains('/') || f.contains(':')) { fr["field"] = "path"; fr["op"] = "starts_with"; fr["value"] = f.endsWith('/') ? f : f + "/"; }
        else {
            QJsonArray any;
            for (const char* fld : {"category", "subcategory", "tag", "series"}) { QJsonObject r; r["field"] = fld; r["op"] = "is"; r["value"] = f; any.append(r); }
            QJsonObject dir; dir["field"] = "path"; dir["op"] = "contains"; dir["value"] = "/" + f + "/"; any.append(dir);
            fr["match"] = "any"; fr["rules"] = any;
        }
        if (b.cond.rules.isEmpty()) b.cond.rules = QJsonArray{fr};
        else if (b.cond.match == "any") { QJsonObject g; g["match"] = "any"; g["rules"] = b.cond.rules; b.cond.rules = QJsonArray{g, fr}; b.cond.match = "all"; }
        else b.cond.rules.append(fr);
    }
    return b;
}

HourRecipe HourRecipe::fromJson(const QJsonObject& o) {
    HourRecipe r;
    r.id = o.value("id").toString();
    r.name = o.value("name").toString();
    r.description = o.value("description").toString();
    for (const QJsonValue& v : o.value("parts").toArray()) { const RecipePart p = RecipePart::fromJson(v.toObject()); if (p.minutes > 0 && (!p.types.isEmpty() || !p.categories.isEmpty() || !p.subcategories.isEmpty() || !p.series.isEmpty() || !p.tags.isEmpty() || !p.folder.isEmpty() || !p.cond.isEmpty())) r.parts.append(p); }
    r.idAtTop = o.value("idAtTop").toBool(false);
    r.idTypes = o.value("idTypes").toString("station_id");
    if (r.id.isEmpty()) r.id = FormatClock::newItemId("recipe");
    if (r.name.isEmpty()) r.name = r.id;
    return r;
}

QJsonObject HourRecipe::toJson() const {
    QJsonObject o; o["id"] = id; o["name"] = name; if (!description.isEmpty()) o["description"] = description;
    QJsonArray a; for (const auto& p : parts) a.append(p.toJson()); o["parts"] = a;
    o["cycleSec"] = cycleSec(); if (idAtTop) { o["idAtTop"] = true; o["idTypes"] = idTypes; }
    return o;
}

double HourRecipe::cycleSec() const { double t = 0; for (const auto& p : parts) t += p.minutes * 60.0; return t; }

HourRecipe FormatClock::defaultRecipe() {
    HourRecipe r; r.id = "default"; r.name = "Default (10 programs / 5 ads / 3 autogen / 1 ID)";
    r.description = "PlayPlay OBS default hour parts: programs least played, ads, the autogen rotation newest first, IDs and promos.";
    RecipePart a; a.label = "Programs"; a.types = {"program"}; a.minutes = 10; a.prefer = "least_played"; r.parts << a;
    RecipePart b; b.label = "Ads"; b.types = {"ad"}; b.minutes = 5; b.prefer = "least_played"; r.parts << b;
    RecipePart c; c.label = "Autogen"; c.types = {"news", "weather"}; c.minutes = 3; c.prefer = "newest"; c.categories = {"autogen rotation"}; r.parts << c;
    RecipePart d; d.label = "ID / promo"; d.types = {"station_id", "promo", "bumper"}; d.minutes = 1; d.prefer = "least_played"; r.parts << d;
    return r;
}

namespace {
RecipePart part(const QString& label, const QStringList& types, double minutes, const QString& prefer = "even") {
    RecipePart p; p.label = label; p.types = types; p.minutes = minutes; p.prefer = prefer; return p;
}
HourRecipe recipe(const QString& id, const QString& name, const QString& description, const QList<RecipePart>& parts, bool idAtTop = true) {
    HourRecipe r; r.id = id; r.name = name; r.description = description; r.parts = parts; r.idAtTop = idAtTop; return r;
}
const QStringList kBreakTypes = {"ad", "promo", "psa"};
} // namespace

QList<HourRecipe> FormatClock::builtinRecipes() {
    QList<HourRecipe> out;
    out << recipe("builtin-breaks-15", "Programs, 2-min break every 15 min",
                  "Programmes in even rotation with a 2-minute break of ads, promos and PSAs every 15 minutes; station ID at :00.",
                  {part("Program", {"program"}, 15), part("Break", kBreakTypes, 2)});
    out << recipe("builtin-half-hours", "Half-hour shows (28 + 2)",
                  "Two half-hour slots: 28 minutes of programme and a 2-minute break each.",
                  {part("Show", {"program"}, 28), part("Break", kBreakTypes, 2)});
    out << recipe("builtin-long-form", "Long form (55 + 5)",
                  "One long programme (movie, documentary, service) and a 5-minute break before the top of the hour.",
                  {part("Feature", {"program"}, 55, "least_played"), part("Break", kBreakTypes, 5)});
    {
        RecipePart p = part("Fresh programs", {"program"}, 15); p.cond.maxAgeDays = 14;
        out << recipe("builtin-fresh-14", "Fresh only (newer than 14 days)",
                      "Only programmes added in the last two weeks, even rotation, 2-minute breaks every 15 minutes.",
                      {p, part("Break", kBreakTypes, 2)});
    }
    {
        RecipePart p = part("Classics", {"program"}, 20); p.cond.minAgeDays = 365;
        out << recipe("builtin-classics", "Classics (older than a year)",
                      "Programmes more than a year old in even rotation, a 2-minute break every 20 minutes.",
                      {p, part("Break", kBreakTypes, 2)});
    }
    {
        RecipePart news = part("News", {"news"}, 5, "newest"); news.anchorTop = true;
        out << recipe("builtin-news-top", "News at the top, programs after",
                      "The newest 5 minutes of news at :00, then programmes with a 2-minute break every 15 minutes.",
                      {news, part("Program", {"program"}, 13), part("Break", kBreakTypes, 2)});
    }
    out << recipe("builtin-news-wheel", "News & weather wheel",
                  "A 15-minute wheel four times an hour: newest news, newest weather, a break, then promos and IDs.",
                  {part("News", {"news"}, 8, "newest"), part("Weather", {"weather"}, 2, "newest"), part("Break", kBreakTypes, 3),
                   part("Promo / ID", {"promo", "station_id", "bumper"}, 2)});
    out << recipe("builtin-ad-heavy", "Commercial heavy (8 + 4)",
                  "Short programme segments with 4-minute breaks every 8 minutes, for high ad loads.",
                  {part("Program", {"program"}, 8), part("Break", kBreakTypes, 4)});
    out << recipe("builtin-light-ads", "Light ad load (20 + 1)",
                  "Long stretches of programme with a 1-minute break every 20 minutes.",
                  {part("Program", {"program"}, 20), part("Break", kBreakTypes, 1)});
    out << recipe("builtin-programs-only", "Programs only, even rotation",
                  "The whole hour from programmes in even rotation; no breaks.",
                  {part("Program", {"program"}, 60)}, false);
    out << recipe("builtin-overnight", "Overnight filler loop",
                  "Filler, PSAs and promos in even rotation with an ID every 12 minutes, for unattended overnight hours.",
                  {part("Filler", {"filler", "psa", "promo"}, 12), part("ID", {"station_id", "bumper"}, 0.5)});
    return out;
}

bool FormatClock::findRecipe(const QString& idOrName, const QList<HourRecipe>& saved, HourRecipe* out) {
    const QString key = idOrName.trimmed();
    if (key.isEmpty() || key.compare("default", Qt::CaseInsensitive) == 0) { *out = defaultRecipe(); return true; }
    for (const HourRecipe& r : saved) if (r.id == key || r.name.compare(key, Qt::CaseInsensitive) == 0) { *out = r; return true; }
    for (const HourRecipe& r : builtinRecipes()) if (r.id == key || r.name.compare(key, Qt::CaseInsensitive) == 0) { *out = r; return true; }
    return false;
}

QList<HourRecipe> FormatClock::recipesFromSettings(const QJsonObject& settings) {
    QList<HourRecipe> out;
    for (const QJsonValue& v : settings.value("recipes").toArray()) { const HourRecipe r = HourRecipe::fromJson(v.toObject()); if (r.isValid()) out.append(r); }
    return out;
}

QJsonObject FormatClock::recipesToSettings(const QList<HourRecipe>& recipes) {
    QJsonObject o; QJsonArray a; for (const auto& r : recipes) a.append(r.toJson()); o["recipes"] = a; return o;
}

HourRecipe FormatClock::adBreakRecipe(const QJsonObject& spec) {
    // {programTypes[], programMin, adTypes[], adMin, prefer, adPrefer, autogenMin, autogenCategories[], idEveryBreak, programCategories[], programSeries[], name}
    HourRecipe r;
    const double programMin = qBound(1.0, spec.value("programMin").toDouble(spec.value("everyMin").toDouble(12.0)), 59.0);
    const double adMin = qBound(0.5, spec.value("adMin").toDouble(3.0), 30.0);
    const double autogenMin = qBound(0.0, spec.value("autogenMin").toDouble(0.0), 15.0);
    QStringList pt = QueryBlocks::listOf(spec, "programTypes"); if (pt.isEmpty()) pt = {"program"};
    QStringList at = QueryBlocks::listOf(spec, "adTypes"); if (at.isEmpty()) at = {"ad", "promo", "psa"};
    const QString prefer = spec.value("prefer").toString("least_played");
    const QString adPrefer = spec.value("adPrefer").toString("least_played");
    r.name = spec.value("name").toString(QString("%1 with %2-min breaks every %3 min").arg(pt.join("/"), QString::number(adMin, 'g', 3), QString::number(programMin, 'g', 3)));
    r.id = spec.value("id").toString(newItemId("recipe"));
    r.description = "Built by the ad-break recipe builder.";
    RecipePart p; p.label = spec.value("programLabel").toString("Program"); p.types = pt; p.minutes = programMin; p.prefer = prefer;
    p.categories = QueryBlocks::listOf(spec, "programCategories"); p.subcategories = QueryBlocks::listOf(spec, "programSubcategories"); p.series = QueryBlocks::listOf(spec, "programSeries");
    r.parts << p;
    if (spec.value("idEveryBreak").toBool(false)) { RecipePart id; id.label = "ID"; id.types = {"station_id", "bumper"}; id.minutes = 0.25; id.prefer = "least_played"; r.parts << id; }
    RecipePart a; a.label = spec.value("adLabel").toString("Break"); a.types = at; a.minutes = adMin; a.prefer = adPrefer; a.categories = QueryBlocks::listOf(spec, "adCategories"); r.parts << a;
    if (autogenMin > 0) { RecipePart g; g.label = "Autogen"; g.types = QueryBlocks::listOf(spec, "autogenTypes"); if (g.types.isEmpty()) g.types = {"news", "weather"}; g.minutes = autogenMin; g.prefer = "newest"; g.categories = QueryBlocks::listOf(spec, "autogenCategories"); if (g.categories.isEmpty()) g.categories = {"autogen rotation"}; r.parts << g; }
    r.idAtTop = spec.value("idAtTop").toBool(true);
    return r;
}

static SlotItem queryItem(const QueryBlock& b, const QString& label, const QString& id) {
    SlotItem it; it.id = id; it.queryBlock = b.toJson(); it.durationSec = b.targetSec;
    it.title = QString("\xE2\x9A\xA1 %1 ~%2m").arg(label, QString::number(b.targetSec / 60.0, 'g', 3));
    it.type = b.types.size() == 1 ? b.types.first() : (b.types.contains("program") ? "program" : (b.types.isEmpty() ? "program" : b.types.first()));
    it.timingMode = "soft_start";
    return it;
}

/// The part that closes the last gap of an hour: the longest non-program part that still fits (else the shortest one),
/// so an hour never ends on "a program cut after 3 minutes". A programs-only recipe closes with its own part.
static const RecipePart& closingPart(const HourRecipe& r, const RecipePart& current, double remSec) {
    const RecipePart* best = nullptr;
    for (const RecipePart& q : r.parts) {
        if (q.anchorTop || partIsProgram(q)) continue;
        if (!best) { best = &q; continue; }
        const double qs = q.minutes * 60.0, bs = best->minutes * 60.0;
        const bool qFits = qs <= remSec + kAnchorTolerance, bFits = bs <= remSec + kAnchorTolerance;
        if (qFits && (!bFits || qs > bs)) best = &q;
        else if (!qFits && !bFits && qs < bs) best = &q;
    }
    return best ? *best : current;
}

HourLayout FormatClock::layRecipe(const HourRecipe& r, const HourSlot& existing, bool keepLocks, int cycleLimit, const QString& idPrefix, double startSec) {
    HourLayout out;
    if (!r.isValid()) { out.notes << "recipe has no parts"; return out; }
    // A prefix makes the ids deterministic ("<prefix>-q<n>"): an hour synthesized again keeps the ids (and so the picks and
    // runlist ids) it had. Without one every block gets a random id (layouts that are saved into a slot).
    int seq = 0;
    auto nextId = [&]() { return idPrefix.isEmpty() ? newItemId("q") : QString("%1-q%2").arg(idPrefix).arg(seq++); };
    QList<SlotItem> items;
    double used = qBound(0.0, startSec, kHourSec);
    if (r.idAtTop && used <= 0.0) {
        QueryBlock idb; idb.types = r.idTypes.split(',', Qt::SkipEmptyParts); for (auto& s : idb.types) s = s.trimmed(); if (idb.types.isEmpty()) idb.types = {"station_id"};
        idb.targetSec = 0.0; idb.prefer = "least_played";
        SlotItem it = queryItem(idb, "Station ID", nextId()); it.title = "\xE2\x9A\xA1 Station ID"; it.durationSec = 10.0; items.append(it); used += 10.0;
    }
    int guard = 0;
    while (used < kHourSec - 30.0 && guard++ < cycleLimit) {
        bool placedAny = false;
        for (const RecipePart& p : r.parts) {
            if (p.anchorTop && (guard > 1 || startSec > 0.0)) continue;   // top-of-hour parts never go into a remainder
            const double target = p.minutes * 60.0;
            if (used + target > kHourSec + 30.0) {
                // The hour is shaped; a last short block closes the gap if there is enough room for anything.
                const double rem = kHourSec - used;
                if (rem >= 60.0) {
                    const RecipePart& cp = closingPart(r, p, rem);
                    const double bs = cp.minutes * 60.0;
                    // Much more room than one break: a shorter programme block, then a normal-length break, instead of
                    // closing the hour with eight minutes of ads.
                    const RecipePart* prog = nullptr;
                    for (const RecipePart& q : r.parts) if (!q.anchorTop && partIsProgram(q)) { prog = &q; break; }
                    if (prog && &cp != prog && rem - bs >= qMax(120.0, bs * 1.5)) {
                        QueryBlock pb = prog->block(); pb.targetSec = rem - bs; items.append(queryItem(pb, prog->label, nextId()));
                        QueryBlock cb = cp.block(); cb.targetSec = bs; items.append(queryItem(cb, cp.label, nextId()));
                    } else { QueryBlock b = cp.block(); b.targetSec = rem; items.append(queryItem(b, cp.label, nextId())); }
                    used += rem;
                }
                used = kHourSec; placedAny = true; break;
            }
            items.append(queryItem(p.block(), p.label, nextId())); used += target; placedAny = true;
        }
        if (!placedAny) break;
    }
    if (keepLocks) items = carryLocks(items, existing.items);
    out.items = items;
    for (const auto& it : items) out.placedSec += itemSec(it);
    out.notes << QString("recipe '%1': %2 block(s), cycle %3 s").arg(r.name).arg(items.size()).arg(r.cycleSec());
    return out;
}


// ---------------------------------------------------------------------------
// Programmes that run past the end of the hour
// ---------------------------------------------------------------------------

namespace {
QString mmss(double sec) {
    const int t = qMax(0, qRound(sec));
    if (t >= 3600) return QString("%1:%2:%3").arg(t / 3600).arg((t / 60) % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}
} // namespace

double FormatClock::slotItemSec(const SlotItem& it) {
    if (it.isQuery()) {
        const QJsonObject q = !it.query().isEmpty() ? it.query() : it.folder();
        const double t = q.value("targetSec").toDouble(0.0);
        return t > 0.0 ? t : it.durationSec;
    }
    return it.effectiveDurationSec();
}

FormatClock::OverrunResult FormatClock::planOverrun(const HourSlot& cur0, const QString& itemId, const HourSlot& next0, const OverrunPlan& plan) {
    OverrunResult res; res.cur = cur0; res.next = next0;
    int idx = -1;
    for (int i = 0; i < cur0.items.size(); ++i) if (cur0.items[i].id == itemId) { idx = i; break; }
    if (idx < 0) { res.error = "item not found in the hour"; return res; }
    double offset = 0.0; for (int i = 0; i < idx; ++i) offset += slotItemSec(cur0.items[i]);
    double total = 0.0;  for (const SlotItem& it : cur0.items) total += slotItemSec(it);
    const SlotItem item = cur0.items[idx];
    const double dur = slotItemSec(item);
    res.overrunSec = offset + dur - kHourSec;
    if (offset >= kHourSec - 1.0) { res.error = "the item starts at or after the end of the hour"; return res; }
    if (res.overrunSec <= 1.0) { res.error = "the item ends within the hour"; return res; }

    auto breakItem = [&](double sec) {
        QueryBlock b; b.types = plan.adTypes.isEmpty() ? QStringList{"ad", "promo", "psa"} : plan.adTypes; b.targetSec = sec; b.prefer = "even";
        return queryItem(b, "Break", newItemId("q"));
    };
    // The rest of the next hour after `startSec` of carried-in content; `moved` opens it.
    auto applyNext = [&](double startSec, const QList<SlotItem>& moved) {
        QList<SlotItem> locks, own;
        for (const SlotItem& it : res.next.items) (it.locked ? locks : own).append(it);
        const double room = kHourSec - startSec;
        QList<SlotItem> tail;
        if (plan.next == "ads") {
            if (room >= 30.0) tail << breakItem(room);
            tail << locks;
            res.notes << QString("the rest of the next hour (%1) is ads and promos").arg(mmss(room));
        } else if (plan.next == "recipe" && plan.recipe.isValid()) {
            HourSlot ex = res.next; ex.items = locks;
            tail = layRecipe(plan.recipe, ex, true, 200, QString(), startSec).items;
            res.notes << QString("the rest of the next hour is laid from recipe '%1'").arg(plan.recipe.name);
        } else {
            tail = res.next.items;
            if (!own.isEmpty()) res.notes << QString("the next hour's own %1 item(s) follow it; whatever no longer fits is dropped at air").arg(own.size());
        }
        if (startSec >= kHourSec) res.notes << "the carried-in programme fills the whole next hour as well";
        res.next.items = moved + tail;
    };
    auto clearContinuation = [&]() {
        const QJsonObject me = res.next.mergedEvent;
        if (me.value("rootSlotId").toString() == cur0.id || me.value("continuedFrom").toString() == cur0.id) res.next.mergedEvent = QJsonObject();
    };

    if (plan.mode == "cut") {
        SlotItem it = item;
        const double room = kHourSec - offset;
        it.outPointSec = it.inPointSec + room;
        it.extra["overrunMode"] = "cut";
        res.cur.items[idx] = it;
        const int after = cur0.items.size() - idx - 1;
        res.notes << QString("'%1' is cut at the top of the hour: %2 of its %3 airs").arg(item.title, mmss(room), mmss(dur));
        if (after > 0) res.notes << QString("%1 item(s) after it no longer fit in the hour").arg(after);
        clearContinuation();
        res.ok = true; return res;
    }
    if (plan.mode == "continue") {
        SlotItem it = item; it.extra["overrunMode"] = "continue"; res.cur.items[idx] = it;
        res.carrySec = qMax(0.0, total - kHourSec);
        QJsonObject me; me["isContinuation"] = true; me["rootSlotId"] = cur0.id; me["title"] = item.title; me["carrySec"] = res.carrySec;
        res.next.mergedEvent = me;
        res.notes << QString("'%1' plays in full and the hour runs %2 into the next hour").arg(item.title, mmss(res.carrySec));
        applyNext(res.carrySec, {});
        res.ok = true; return res;
    }
    if (plan.mode == "split") {
        // Cut points: the ends of the media's segments inside the item, else every breakEverySec of programme.
        const double a0 = item.inPointSec, b0 = item.inPointSec + dur;
        QList<double> cuts;
        for (const QJsonValue& v : plan.segments) {
            const QJsonObject sg = v.toObject();
            const double e = sg.value("endTime").toDouble(-1.0);
            if (e > a0 + 5.0 && e < b0 - 5.0) cuts << e;
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end(), [](double x, double y) { return qAbs(x - y) < 1.0; }), cuts.end());
        const bool bySegments = !cuts.isEmpty();
        if (!bySegments) { const double step = qMax(60.0, plan.breakEverySec); for (double t = a0 + step; t < b0 - 5.0; t += step) cuts << t; }
        QList<QPair<double, double>> parts; double p0 = a0;
        for (double c : cuts) { parts << qMakePair(p0, c); p0 = c; }
        parts << qMakePair(p0, b0);
        const int n = parts.size();
        auto partItem = [&](int k, double a, double b, bool cont) {
            SlotItem it = item; it.id = newItemId("p"); it.inPointSec = a; it.outPointSec = b;
            it.title = QString("%1 (part %2/%3%4)").arg(item.title).arg(k + 1).arg(n).arg(cont ? ", cont." : "");
            it.extra["overrunMode"] = "split"; it.extra["partOf"] = item.id;
            return it;
        };
        double cursor = offset; bool spilled = false; QList<SlotItem> placed, moved; int breaks = 0, partsMoved = 0;
        for (int k = 0; k < n; ++k) {
            const double a = parts[k].first, b = parts[k].second, len = b - a;
            if (!spilled) {
                const double room = kHourSec - cursor;
                if (len <= room + 2.0) { placed << partItem(k, a, b, false); cursor += len; }
                else {
                    if (room >= 5.0) { placed << partItem(k, a, a + room, false); moved << partItem(k, a + room, b, true); }
                    else moved << partItem(k, a, b, false);
                    ++partsMoved; spilled = true; cursor = kHourSec;
                }
            } else { moved << partItem(k, a, b, false); ++partsMoved; }
            if (k == n - 1) break;
            // the break after this part
            if (!spilled) {
                const double room = kHourSec - cursor;
                if (plan.breakSec <= room + 2.0) { placed << breakItem(plan.breakSec); cursor += plan.breakSec; ++breaks; }
                else { if (room >= 30.0) { placed << breakItem(room); ++breaks; } spilled = true; cursor = kHourSec; }
            } else { moved << breakItem(plan.breakSec); ++breaks; }
        }
        for (int i = idx + 1; i < cur0.items.size(); ++i) moved << cur0.items[i];   // whatever followed the programme follows it again
        QList<SlotItem> head = cur0.items.mid(0, idx);
        res.cur.items = head + placed;
        double movedSec = 0.0; for (const SlotItem& it : moved) movedSec += slotItemSec(it);
        QJsonObject me; me["continuedFrom"] = cur0.id; me["title"] = item.title;
        res.next.mergedEvent = me;
        res.notes << QString("'%1' is broken into %2 part(s) at its %3 with %4 break(s); %5 part(s) open the next hour")
                         .arg(item.title).arg(n).arg(bySegments ? QString("segments") : QString("%1-minute marks").arg(qRound(plan.breakEverySec / 60.0))).arg(breaks).arg(partsMoved);
        applyNext(movedSec, moved);
        res.ok = true; return res;
    }
    res.error = "unknown mode '" + plan.mode + "' (continue, split or cut)";
    return res;
}

// ---------------------------------------------------------------------------
// Weighted fill
// ---------------------------------------------------------------------------

WeightBucket WeightBucket::fromJson(const QJsonObject& o) {
    WeightBucket b; b.label = o.value("label").toString(); b.types = QueryBlocks::listOf(o, "types", "type");
    b.categories = QueryBlocks::listOf(o, "categories", "category"); b.subcategories = QueryBlocks::listOf(o, "subcategories", "subcategory");
    b.tags = QueryBlocks::listOf(o, "tags", "tag"); b.series = o.value("series").toString();
    b.pct = qMax(0.0, o.value("pct").toDouble(o.value("percent").toDouble(0.0))); b.maxPerHour = qMax(0, o.value("maxPerHour").toInt(0));
    if (b.label.isEmpty()) b.label = b.types.isEmpty() ? "Bucket" : b.types.join("/");
    return b;
}
QJsonObject WeightBucket::toJson() const {
    QJsonObject o; o["label"] = label; o["types"] = arr(types); o["categories"] = arr(categories); o["subcategories"] = arr(subcategories);
    if (!tags.isEmpty()) o["tags"] = arr(tags); o["series"] = series; o["pct"] = pct; if (maxPerHour) o["maxPerHour"] = maxPerHour; return o;
}
bool WeightBucket::matches(const MediaRecord& m) const {
    if (!QueryBlocks::inList(m.type, types)) return false;
    if (!QueryBlocks::inList(QueryBlocks::category(m), categories)) return false;
    if (!QueryBlocks::inList(QueryBlocks::subcategory(m), subcategories)) return false;
    if (!series.isEmpty() && QueryBlocks::norm(m.series) != QueryBlocks::norm(series)) return false;
    if (!tags.isEmpty()) { bool any = false; for (const auto& t : QueryBlocks::tags(m)) if (QueryBlocks::inList(t, tags)) { any = true; break; } if (!any) return false; }
    return true;
}

QList<WeightBucket> FormatClock::defaultBuckets() {
    QList<WeightBucket> b;
    WeightBucket p; p.label = "Programs"; p.types = {"program"}; p.pct = 60; b << p;
    WeightBucket a; a.label = "Ads"; a.types = {"ad"}; a.pct = 30; b << a;
    WeightBucket i; i.label = "IDs / Promos / Bumpers"; i.types = {"station_id", "promo", "bumper", "filler"}; i.pct = 10; b << i;
    return b;
}
QList<WeightBucket> FormatClock::bucketsFromJson(const QJsonValue& v) {
    QList<WeightBucket> out; const QJsonArray a = v.isObject() ? v.toObject().value("buckets").toArray() : v.toArray();
    for (const QJsonValue& x : a) { const WeightBucket b = WeightBucket::fromJson(x.toObject()); if (b.pct > 0) out.append(b); }
    return out;
}
QJsonArray FormatClock::bucketsToJson(const QList<WeightBucket>& b) { QJsonArray a; for (const auto& x : b) a.append(x.toJson()); return a; }

namespace {
double ageDays(qint64 ms, qint64 now) { return ms <= 0 ? 400.0 : qMax(0.0, (now - ms) / 86400000.0); }
// Lower sorts sooner: stale on air (older last play) and newness bonus decaying with a 45-day half-life.
double freshness(const MediaRecord& m, qint64 now) {
    const double stale = qMin(ageDays(m.lastPlayedMs, now), 400.0);
    const double newness = 45.0 * std::pow(0.5, ageDays(m.addedAtMs, now) / 45.0);
    return -stale - newness;
}
} // namespace

FormatClock::WeightedState FormatClock::beginWeighted(const QList<WeightBucket>& buckets, const QList<MediaRecord>& pool,
                                                      const QString& channelId, qint64 nowMs, quint32 seed) {
    WeightedState st;
    QRandomGenerator rng(seed ? seed : 1);
    for (int i = 0; i < buckets.size(); ++i) {
        QList<MediaRecord> q;
        for (const MediaRecord& m : pool) {
            if (m.path.isEmpty() || m.durationSec <= 0.5 || !m.isAvailableAt(nowMs)) continue;
            if (!m.allowedChannels.isEmpty() && !m.allowedChannels.contains(channelId)) continue;
            if (m.extra.value("missing").toBool(false)) continue;
            // first matching bucket wins: specific buckets go before broad catch-alls
            int first = -1; for (int k = 0; k < buckets.size(); ++k) if (buckets[k].matches(m)) { first = k; break; }
            if (first == i) q.append(m);
        }
        QHash<QString, double> jitter; for (const auto& m : q) jitter[m.id] = rng.generateDouble();
        std::sort(q.begin(), q.end(), [&](const MediaRecord& a, const MediaRecord& b) {
            if (a.playCount != b.playCount) return a.playCount < b.playCount;
            const double fa = freshness(a, nowMs), fb = freshness(b, nowMs);
            if (fa != fb) return fa < fb;
            return jitter[a.id] < jitter[b.id];
        });
        st.queues.append(q);
    }
    return st;
}

static SlotItem mediaItem(const MediaRecord& m, const QString& id) {
    SlotItem it; it.id = id; it.mediaId = m.id; it.title = m.title; it.type = m.type; it.durationSec = m.durationSec; it.path = m.path;
    return it;
}

enum class Rotation { Chosen, NoFit, Blocked };

/// Next rotation item not already used this hour that fits `room`; used items rotate to the back, unfit stay in front.
/// With `blockPrograms` (a second program in a one-program hour) programs are neither chosen nor rotated: they stay where
/// they are, so a rejected candidate never burns its turn. Blocked = nothing was left but blocked programs.
static Rotation nextUnused(QList<MediaRecord>& q, const QSet<QString>& used, double room, bool allowSplit, double capSec, bool blockPrograms, MediaRecord* out) {
    QList<MediaRecord> usedPass, unfit; bool got = false; MediaRecord chosen; int blocked = 0, notFit = 0;
    while (!q.isEmpty()) {
        MediaRecord m = q.takeFirst();
        if (used.contains(m.id)) { usedPass.append(m); continue; }
        if (blockPrograms && isProgramType(m.type)) { unfit.append(m); ++blocked; continue; }
        const bool fits = allowSplit && m.durationSec > capSec ? qMin(capSec, m.durationSec) <= room + 0.01 : m.durationSec <= room + 0.01;
        if (!fits) { unfit.append(m); ++notFit; continue; }
        chosen = m; got = true; break;
    }
    QList<MediaRecord> rebuilt = unfit; rebuilt.append(q); rebuilt.append(usedPass); if (got) rebuilt.append(chosen);
    q = rebuilt;
    if (got) { if (out) *out = chosen; return Rotation::Chosen; }
    return blocked > 0 && notFit == 0 ? Rotation::Blocked : Rotation::NoFit;
}

HourLayout FormatClock::fillWeighted(const QList<WeightBucket>& buckets, WeightedState& st, const HourSlot& existing,
                                     bool keepLocks, bool oneProgramPerHour, double programCapSec, const QString& idPrefix) {
    HourLayout out;
    int seq = 0;      // deterministic ids when a prefix is given (see layRecipe)
    auto nextId = [&]() { return idPrefix.isEmpty() ? newItemId("w") : QString("%1-w%2").arg(idPrefix).arg(seq++); };
    QList<SlotItem> leads, anchors, free;
    splitLocks(existing.items, &leads, &anchors, &free);
    if (!keepLocks) { leads.clear(); anchors.clear(); }
    QList<SlotItem> items = leads;
    QSet<QString> used; for (const auto& it : leads) if (!it.mediaId.isEmpty()) used.insert(it.mediaId);
    for (const auto& a : anchors) if (!a.mediaId.isEmpty()) used.insert(a.mediaId);
    double filled = 0.0; for (const auto& it : items) filled += itemSec(it);
    bool programAnchored = false;

    // A continuation carried from the previous hour leads this hour.
    if (!st.carry.isEmpty()) {
        SlotItem seg; seg.id = nextId(); seg.mediaId = st.carry.value("mediaId").toString(); seg.title = st.carry.value("title").toString() + " (cont.)";
        seg.type = st.carry.value("type").toString("program"); seg.durationSec = st.carry.value("duration").toDouble(); seg.path = st.carry.value("path").toString();
        seg.inPointSec = st.carry.value("inPoint").toDouble();
        const double rem = seg.durationSec - seg.inPointSec;
        if (rem > programCapSec) { seg.outPointSec = seg.inPointSec + programCapSec; st.carry["inPoint"] = seg.outPointSec; }
        else { seg.outPointSec = 0.0; st.carry = QJsonObject(); }
        seg.note = "continuation";
        items.insert(0, seg); used.insert(seg.mediaId); filled += itemSec(seg); programAnchored = true;
        out.notes << QString("continues '%1' from the previous hour").arg(seg.title);
    }

    QList<SlotItem> pending = anchors;   // sorted by offset
    std::stable_sort(pending.begin(), pending.end(), [](const SlotItem& a, const SlotItem& b) { return lockOffset(a) < lockOffset(b); });
    auto placeAnchor = [&]() { SlotItem a = pending.takeFirst(); items.append(a); filled += itemSec(a); };

    QList<double> placedSec; QList<int> placedCount; for (int i = 0; i < buckets.size(); ++i) { placedSec << 0.0; placedCount << 0; }
    QSet<int> alive; for (int i = 0; i < buckets.size(); ++i) if (!st.queues[i].isEmpty()) alive.insert(i);
    int guard = 0;
    while (filled < kHourSec - kFillTolerance && (!alive.isEmpty() || !pending.isEmpty()) && guard++ < 10000) {
        if (!pending.isEmpty() && filled >= lockOffset(pending.first()) - kAnchorTolerance) { placeAnchor(); continue; }
        if (alive.isEmpty()) { placeAnchor(); continue; }
        double livePct = 0, liveTotal = 0; for (int i : alive) { livePct += buckets[i].pct; liveTotal += placedSec[i]; }
        if (livePct <= 0) livePct = 1;
        QList<int> order(alive.begin(), alive.end());
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const double da = placedSec[a] - (buckets[a].pct / livePct) * liveTotal, db = placedSec[b] - (buckets[b].pct / livePct) * liveTotal;
            if (da != db) return da < db; return buckets[a].pct > buckets[b].pct;
        });
        double room = kHourSec + kFillTolerance - filled;
        if (!pending.isEmpty()) {
            for (const auto& a : pending) room -= itemSec(a);
            room = qMin(room, lockOffset(pending.first()) - filled);
            if (room < 1.0) { placeAnchor(); continue; }
        }
        int chosen = -1; MediaRecord pick;
        for (int i : order) {
            if (buckets[i].maxPerHour > 0 && placedCount[i] >= buckets[i].maxPerHour) continue;
            MediaRecord m;
            const Rotation rot = nextUnused(st.queues[i], used, room, true, programCapSec, programAnchored && oneProgramPerHour, &m);
            if (rot != Rotation::Chosen) { if (rot == Rotation::Blocked || pending.isEmpty()) alive.remove(i); continue; }
            chosen = i; pick = m; break;
        }
        if (chosen < 0) { if (!pending.isEmpty()) { placeAnchor(); continue; } break; }
        SlotItem it = mediaItem(pick, nextId());
        if (isProgramType(pick.type) && pick.durationSec > programCapSec && st.carry.isEmpty()) {
            // Long program: air the leading segment, carry the remainder into the next hour.
            it.outPointSec = programCapSec; it.note = "segment 1";
            QJsonObject c; c["mediaId"] = pick.id; c["title"] = pick.title; c["type"] = pick.type; c["duration"] = pick.durationSec; c["path"] = pick.path; c["inPoint"] = programCapSec;
            st.carry = c; programAnchored = true; alive.remove(chosen);
            out.notes << QString("'%1' (%2 s) split: %3 s now, rest carried").arg(pick.title).arg(pick.durationSec, 0, 'f', 0).arg(programCapSec, 0, 'f', 0);
        } else if (isProgramType(pick.type) && oneProgramPerHour) {
            programAnchored = true;
        }
        used.insert(pick.id); st.usedRun.insert(pick.id);
        items.append(it); const double d = itemSec(it); placedSec[chosen] += d; placedCount[chosen]++; filled += d;
    }
    while (!pending.isEmpty()) placeAnchor();
    out.items = items; out.placedSec = filled; out.carry = st.carry; st.hoursDone++;
    QStringList shares; for (int i = 0; i < buckets.size(); ++i) shares << QString("%1 %2%").arg(buckets[i].label).arg(filled > 0 ? placedSec[i] / filled * 100.0 : 0.0, 0, 'f', 0);
    out.notes << QString("weighted fill: %1 item(s), %2 s — %3").arg(items.size()).arg(filled, 0, 'f', 0).arg(shares.join(", "));
    return out;
}

// ---------------------------------------------------------------------------
// Autofill policy / contracts
// ---------------------------------------------------------------------------

AutofillPolicy AutofillPolicy::fromJson(const QJsonObject& o) {
    AutofillPolicy p; if (o.isEmpty()) return p;
    p.enabled = o.value("enabled").toBool(false); p.mode = o.value("mode").toString("recipe") == "weighted" ? "weighted" : "recipe";
    p.recipeId = o.value("recipeId").toString();
    const QStringList ord = QueryBlocks::listOf(o, "order"); if (!ord.isEmpty()) p.order = ord;
    p.noRepeatMin = qMax(0.0, o.value("noRepeatMin").toDouble(o.value("no_repeat_min").toDouble(30.0)));
    p.adPacing = o.value("adPacing").toBool(true);
    return p;
}
QJsonObject AutofillPolicy::toJson() const {
    QJsonObject o; o["enabled"] = enabled; o["mode"] = mode; o["recipeId"] = recipeId; o["order"] = arr(order); o["noRepeatMin"] = noRepeatMin; o["adPacing"] = adPacing; return o;
}
AutofillPolicy FormatClock::policyFor(const ChannelProfile& p) { return AutofillPolicy::fromJson(p.extra.value("autofill").toObject()); }

AdContract AdContract::fromJson(const QJsonObject& o) {
    AdContract c; c.id = o.value("id").toString(); c.client = o.value("client").toString(); c.title = o.value("title").toString();
    c.mediaIds = QueryBlocks::listOf(o, "mediaIds", "media_id"); c.spotsTotal = o.value("spotsTotal").toInt(o.value("contracted_spots").toInt(0));
    c.spotsPerDay = o.value("spotsPerDay").toInt(0);
    c.startDate = QDate::fromString(o.value("startDate").toString(o.value("contract_start").toString()).left(10), Qt::ISODate);
    c.endDate = QDate::fromString(o.value("endDate").toString(o.value("contract_end").toString()).left(10), Qt::ISODate);
    c.channels = QueryBlocks::listOf(o, "channels"); c.dayparts = o.value("dayparts").toArray(); c.status = o.value("status").toString("active"); c.priority = o.value("priority").toInt(0);
    if (c.id.isEmpty()) c.id = FormatClock::newItemId("contract");
    return c;
}
QJsonObject AdContract::toJson() const {
    QJsonObject o; o["id"] = id; o["client"] = client; o["title"] = title; o["mediaIds"] = arr(mediaIds); o["spotsTotal"] = spotsTotal; o["spotsPerDay"] = spotsPerDay;
    o["startDate"] = startDate.isValid() ? startDate.toString(Qt::ISODate) : QString(); o["endDate"] = endDate.isValid() ? endDate.toString(Qt::ISODate) : QString();
    o["channels"] = arr(channels); o["dayparts"] = dayparts; o["status"] = status; o["priority"] = priority; return o;
}
bool AdContract::activeOn(const QDate& day, const QString& channelId) const {
    if (status != "active") return false;
    if (startDate.isValid() && day < startDate) return false;
    if (endDate.isValid() && day > endDate) return false;
    if (!channels.isEmpty() && !channelId.isEmpty() && !channels.contains(channelId)) return false;
    return !mediaIds.isEmpty();
}

QList<AdContract> FormatClock::contractsFromSettings(const QJsonObject& settings) {
    QList<AdContract> out; for (const QJsonValue& v : settings.value("contracts").toArray()) out.append(AdContract::fromJson(v.toObject())); return out;
}
QJsonObject FormatClock::contractsToSettings(const QList<AdContract>& c) { QJsonObject o; QJsonArray a; for (const auto& x : c) a.append(x.toJson()); o["contracts"] = a; return o; }

QHash<QString, double> FormatClock::contractPacing(const QList<AdContract>& contracts, const QString& channelId, const QDate& day, const QTimeZone& tz,
                                                   const std::function<int(const QString&, qint64, qint64)>& airedCount) {
    QHash<QString, double> out;
    if (!airedCount) return out;
    const qint64 dayStart = QDateTime(day, QTime(0, 0), tz).toMSecsSinceEpoch();
    const qint64 dayEnd = dayStart + 86400000LL;
    for (const AdContract& c : contracts) {
        if (!c.activeOn(day, channelId)) continue;
        // Pacing belongs to the CONTRACT: spotsTotal / spotsPerDay count every creative together (contractReport sums the same
        // way), so a multi-creative campaign is not seen as behind while its creatives have already delivered the total.
        double bonus = 0.0;
        if (c.spotsTotal > 0 && c.startDate.isValid()) {
            const QDate end = c.endDate.isValid() ? c.endDate : day;
            const int totalDays = qMax(1, int(c.startDate.daysTo(end)) + 1);
            const int elapsed = qMax(0, int(c.startDate.daysTo(day)) + 1);
            const double target = c.spotsTotal * qMin(1.0, double(elapsed) / totalDays);
            const qint64 from = QDateTime(c.startDate, QTime(0, 0), tz).toMSecsSinceEpoch();
            int aired = 0; for (const QString& mid : c.mediaIds) aired += airedCount(mid, from, dayEnd);
            if (aired >= c.spotsTotal) bonus -= 500.0; else bonus += qMax(0.0, target - aired) * 25.0;
        }
        if (c.spotsPerDay > 0) {
            int today = 0; for (const QString& mid : c.mediaIds) today += airedCount(mid, dayStart, dayEnd);
            if (today >= c.spotsPerDay) bonus -= 300.0; else bonus += (c.spotsPerDay - today) * 20.0;
        }
        bonus += c.priority * 5.0;
        for (const QString& mid : c.mediaIds) out[mid] = out.value(mid, 0.0) + bonus;
    }
    return out;
}

QJsonObject FormatClock::contractReport(const AdContract& c, const QDate& today, const std::function<int(const QString&, qint64, qint64)>& airedCount, const QTimeZone& tz) {
    QJsonObject o = c.toJson();
    int aired = 0, airedToday = 0;
    const qint64 from = c.startDate.isValid() ? QDateTime(c.startDate, QTime(0, 0), tz).toMSecsSinceEpoch() : 0;
    const qint64 dayStart = QDateTime(today, QTime(0, 0), tz).toMSecsSinceEpoch();
    QJsonObject perMedia;
    for (const QString& mid : c.mediaIds) {
        const int n = airedCount ? airedCount(mid, from, dayStart + 86400000LL) : 0;
        const int t = airedCount ? airedCount(mid, dayStart, dayStart + 86400000LL) : 0;
        aired += n; airedToday += t; QJsonObject pm; pm["aired"] = n; pm["today"] = t; perMedia[mid] = pm;
    }
    o["aired"] = aired; o["airedToday"] = airedToday; o["perMedia"] = perMedia;
    if (c.spotsTotal > 0 && c.startDate.isValid()) {
        const QDate end = c.endDate.isValid() ? c.endDate : today;
        const int totalDays = qMax(1, int(c.startDate.daysTo(end)) + 1), elapsed = qBound(0, int(c.startDate.daysTo(today)) + 1, totalDays);
        const double target = c.spotsTotal * double(elapsed) / totalDays;
        o["target"] = target; o["pacingPct"] = target > 0 ? aired / target * 100.0 : 100.0; o["remaining"] = qMax(0, c.spotsTotal - aired);
        const int daysLeft = qMax(0, int(today.daysTo(end)));
        o["daysLeft"] = daysLeft; o["neededPerDay"] = daysLeft > 0 ? double(qMax(0, c.spotsTotal - aired)) / daysLeft : double(qMax(0, c.spotsTotal - aired));
        o["projectedShortfall"] = elapsed > 0 ? qMax(0.0, c.spotsTotal - (double(aired) / elapsed) * totalDays) : 0.0;
    }
    if (c.spotsPerDay > 0) { o["todayTarget"] = c.spotsPerDay; o["todayPct"] = airedToday * 100.0 / c.spotsPerDay; }
    return o;
}

// ---------------------------------------------------------------------------
// Locks
// ---------------------------------------------------------------------------

void FormatClock::splitLocks(const QList<SlotItem>& items, QList<SlotItem>* leads, QList<SlotItem>* anchors, QList<SlotItem>* free) {
    for (const SlotItem& it : items) {
        if (!it.locked) { if (free) free->append(it); continue; }
        if (it.anchorSec() >= 0.0 || it.isHardStart()) { if (anchors) anchors->append(it); } else if (leads) leads->append(it);
    }
    if (anchors) std::stable_sort(anchors->begin(), anchors->end(), [](const SlotItem& a, const SlotItem& b) { return lockOffset(a) < lockOffset(b); });
}

int FormatClock::indexAtOffset(const QList<SlotItem>& items, double offsetSec) {
    int best = 0; double bestD = std::fabs(offsetSec), run = 0.0;
    for (int i = 0; i < items.size(); ++i) { run += itemSec(items[i]); const double d = std::fabs(run - offsetSec); if (d < bestD) { best = i + 1; bestD = d; } }
    return best;
}

QList<SlotItem> FormatClock::carryLocks(QList<SlotItem> dest, const QList<SlotItem>& src) {
    QList<SlotItem> leads, anchors;
    splitLocks(src, &leads, &anchors, nullptr);
    if (leads.isEmpty() && anchors.isEmpty()) return dest;
    QSet<QString> keepOut; for (const auto& it : leads) if (!it.mediaId.isEmpty()) keepOut.insert(it.mediaId); for (const auto& it : anchors) if (!it.mediaId.isEmpty()) keepOut.insert(it.mediaId);
    QList<SlotItem> body; for (const auto& it : dest) if (!it.locked && (it.mediaId.isEmpty() || !keepOut.contains(it.mediaId))) body.append(it);
    QList<SlotItem> out = leads; out.append(body);
    for (const auto& a : anchors) out.insert(indexAtOffset(out, lockOffset(a)), a);
    // Trim trailing unlocked items until the hour fits (+60 s).
    double total = 0; for (const auto& it : out) total += itemSec(it);
    for (int i = out.size() - 1; i >= 0 && total > kHourSec + 60.0; --i) if (!out[i].locked) { total -= itemSec(out[i]); out.removeAt(i); }
    return out;
}

QList<SlotItem> FormatClock::shuffleAds(const QList<SlotItem>& items, quint32 seed) {
    QList<int> idx; QList<SlotItem> pool;
    for (int i = 0; i < items.size(); ++i) { const SlotItem& it = items[i]; if (!it.locked && !isProgramType(it.type) && it.type != "live" && !it.isHardStart()) { idx << i; pool << it; } }
    QRandomGenerator rng(seed ? seed : QRandomGenerator::global()->generate());
    for (int i = pool.size() - 1; i > 0; --i) { const int j = int(rng.bounded(i + 1)); pool.swapItemsAt(i, j); }
    QList<SlotItem> out = items; for (int k = 0; k < idx.size(); ++k) out[idx[k]] = pool[k];
    return out;
}

QJsonObject HourLayout::toJson() const {
    QJsonObject o; QJsonArray a; for (const auto& it : items) a.append(it.toJson()); o["items"] = a;
    QJsonArray n; for (const auto& s : notes) n << s; o["notes"] = n; o["placedSec"] = placedSec; if (!carry.isEmpty()) o["carry"] = carry; return o;
}

} // namespace sched
