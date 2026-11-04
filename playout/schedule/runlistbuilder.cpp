#include "runlistbuilder.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <cmath>
#include <algorithm>
#include <utility>
#include <QSet>
#include <QHash>

namespace sched {

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QJsonObject RunlistItem::toEngineJson() const {
    QJsonObject o;
    o["id"] = runlistId;
    o["assetId"] = item.mediaId.isEmpty() ? item.id : item.mediaId;
    o["path"] = item.path;
    o["title"] = item.title;
    o["inPoint"] = inSec;
    o["outPoint"] = outSec;
    o["duration"] = durationSec;
    o["loop"] = loop;
    if (hard) o["hardStartUtcMs"] = double(plannedStartMs);
    o["plannedUtcMs"] = double(plannedStartMs);
    o["type"] = item.type;
    o["origin"] = origin;
    o["slotId"] = slotId;
    o["itemId"] = item.id;
    if (!item.liveSource.isEmpty()) o["source"] = item.liveSource;
    if (!item.cgOnStart.isEmpty()) { QJsonArray a; for (const auto& s : item.cgOnStart) a << s; o["cgOnStart"] = a; }
    if (!item.cgOnEnd.isEmpty()) { QJsonArray a; for (const auto& s : item.cgOnEnd) a << s; o["cgOnEnd"] = a; }
    if (!item.cgOverlays.isEmpty()) o["cgOverlays"] = item.cgOverlays;
    if (!item.transition.isEmpty() && item.transition != "cut") { o["transition"] = item.transition; o["transitionSec"] = item.transitionSec; }
    return o;
}

QJsonObject RunlistItem::toJson() const {
    QJsonObject o = toEngineJson();
    o["plannedStartMs"] = double(plannedStartMs);
    o["plannedEndMs"] = double(plannedEndMs);
    o["plannedStart"] = QDateTime::fromMSecsSinceEpoch(plannedStartMs, QTimeZone::utc()).toString(Qt::ISODateWithMs);
    o["hard"] = hard; o["truncated"] = truncated; o["note"] = note;
    o["mediaId"] = item.mediaId;
    if (item.extra.value("queryFill").toBool(false)) { o["queryFill"] = true; o["blockLabel"] = item.extra.value("blockLabel"); o["blockItemId"] = item.extra.value("blockItemId"); if (item.extra.value("overrun").toBool(false)) o["overrun"] = true; if (item.extra.value("repeat").toBool(false)) o["repeat"] = true; }
    if (item.locked) { o["locked"] = true; if (item.anchorSec() >= 0.0) o["lockAtSec"] = item.anchorSec(); if (!item.lockLabel.isEmpty()) o["lockLabel"] = item.lockLabel; }
    return o;
}

QJsonArray Runlist::toEngineJson() const { QJsonArray a; for (const auto& i : items) a.append(i.toEngineJson()); return a; }

QJsonObject Runlist::toJson() const {
    QJsonObject o;
    o["channelId"] = channelId; o["slotId"] = slotId;
    o["hourStartMs"] = double(hourStartMs); o["hourEndMs"] = double(hourEndMs);
    QJsonArray a; for (const auto& i : items) a.append(i.toJson()); o["items"] = a;
    QJsonArray n; for (const auto& s : notes) n << s; o["notes"] = n;
    QJsonArray dr; for (const auto& i : dropped) dr.append(i.toJson()); o["dropped"] = dr;
    o["scheduledSec"] = scheduledSec; o["filledSec"] = filledSec; o["gapSec"] = gapSec;
    return o;
}

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------

qint64 RunlistBuilder::hourStartUtcMs(const QDate& localDate, int hour, const QTimeZone& tz) {
    QDateTime dt(localDate, QTime(hour, 0, 0), tz);
    return dt.toMSecsSinceEpoch();
}

qint64 RunlistBuilder::hourStartUtcMsForTime(qint64 utcMs, const QTimeZone& tz, QDate* localDate, int* hour) {
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(utcMs, tz);
    if (localDate) *localDate = local.date();
    if (hour) *hour = local.time().hour();
    return hourStartUtcMs(local.date(), local.time().hour(), tz);
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

bool RunlistBuilder::resolveItem(SlotItem& item, const MediaLookup& media, qint64 nowMs, QString* why) {
    auto fail = [&](const QString& r) { if (why) *why = r; return false; };
    if (item.type == "cg" || item.type == "gap") return true;
    if (item.isQuery()) return true;   // late bound: expanded by build()
    if (item.type == "live") {
        // A live entry from the library carries its source spec in the media record's extra fields.
        if (item.liveSource.isEmpty() && !item.mediaId.isEmpty() && media.byId) {
            bool lf = false; const MediaRecord lm = media.byId(item.mediaId, &lf);
            if (lf) { item.liveSource = lm.extra.value("liveSource").toObject(); if (item.title.isEmpty()) item.title = lm.title; if (item.durationSec <= 0.0) item.durationSec = lm.durationSec; }
        }
        if (item.path.isEmpty()) item.path = item.liveSource.value("url").toString();
        if (item.path.isEmpty() && !item.liveSource.value("ndiName").toString().isEmpty()) item.path = "ndi://" + item.liveSource.value("ndiName").toString();
        if (item.path.isEmpty() && !item.liveSource.value("name").toString().isEmpty()) item.path = "ndi://" + item.liveSource.value("name").toString();
        if (item.path.isEmpty() && item.liveSource.value("port").toInt() > 0) item.path = "sdi://" + QString::number(item.liveSource.value("port").toInt());
        return item.path.isEmpty() ? fail("live source has no NDI name, SDI port or URL") : true;
    }

    MediaRecord m; bool found = false;
    const QString smart = item.smartConfig.value("type").toString();
    if (!smart.isEmpty() && media.bySeries) {
        const QString series = item.smartConfig.value("seriesName").toString();
        QList<MediaRecord> eps = media.bySeries(series);
        QList<MediaRecord> avail; for (const auto& e : eps) if (e.isAvailableAt(nowMs) && !e.path.isEmpty()) avail.append(e);
        if (avail.isEmpty()) return fail("no available episode for series '" + series + "'");
        if (smart == "latest") { m = avail.first(); found = true; }
        else if (smart == "previous") { m = avail.size() > 1 ? avail[1] : avail.first(); found = true; }
        else if (smart == "specific_date") {
            const QDate d = QDate::fromString(item.smartConfig.value("targetDate").toString().left(10), Qt::ISODate);
            for (const auto& e : avail) if (e.episodeDate == d) { m = e; found = true; break; }
            if (!found) return fail("no episode dated " + d.toString(Qt::ISODate));
        } else if (smart == "random") { m = avail[int(QRandomGenerator::global()->bounded(avail.size()))]; found = true; }
        else { m = avail.first(); found = true; }
        item.mediaId = m.id;
    } else if (!item.mediaId.isEmpty() && media.byId) {
        m = media.byId(item.mediaId, &found);
        if (!found && !item.path.isEmpty()) {
            // Not in the library but has a path: play it anyway (ad-hoc file).
            if (item.durationSec <= 0.0) return fail("unknown media '" + item.mediaId + "' and no duration");
            return true;
        }
        if (!found) return fail("media '" + item.mediaId + "' not found");
    } else if (!item.path.isEmpty()) {
        return item.durationSec > 0.0 ? true : fail("item has a path but no duration");
    } else {
        return fail("item has no media");
    }

    if (!m.isAvailableAt(nowMs)) return fail("media '" + m.title + "' not available (window/archived)");
    if (m.path.isEmpty()) return fail("media '" + m.title + "' has no file path");
    item.path = m.path;
    if (item.title.isEmpty()) item.title = m.title;
    if (item.type.isEmpty() || item.type == "video") item.type = m.type.isEmpty() ? "video" : m.type;
    if (item.durationSec <= 0.0 || item.durationSec > m.durationSec + 0.5) item.durationSec = m.durationSec;
    // Segment reference: clip in/out from the media's segment list.
    if (!item.segmentId.isEmpty() && item.outPointSec <= item.inPointSec) {
        for (const auto& v : m.segments) {
            const QJsonObject seg = v.toObject();
            if (seg.value("id").toString() == item.segmentId) {
                item.inPointSec = seg.value("startTime").toDouble(seg.value("time_in").toDouble(0.0));
                item.outPointSec = seg.value("endTime").toDouble(seg.value("time_out").toDouble(0.0));
                if (item.segmentLabel.isEmpty()) item.segmentLabel = seg.value("label").toString();
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Query / folder blocks and lock anchors
// ---------------------------------------------------------------------------

QList<SlotItem> RunlistBuilder::expandQuery(const SlotItem& item, const MediaLookup& media, const BuildOptions& opt, const QString& channelId,
                                            qint64 hourStartMs, QSet<QString>& exclude, QString* why) {
    QList<SlotItem> out;
    const QJsonObject qb = item.query(), fb = item.folder();
    const QueryBlock block = QueryBlock::fromJson(qb);
    const FolderBlock folder = FolderBlock::fromJson(fb);
    const bool isFolder = qb.isEmpty() && !fb.isEmpty();
    if (!isFolder && !block.isValid()) { if (why) *why = "query block has no types/categories/series"; return out; }
    if (isFolder && !folder.isValid()) { if (why) *why = "folder block has no folder"; return out; }

    QueryContext ctx; ctx.channelId = channelId; ctx.nowMs = opt.nowMs; ctx.exclude = exclude; ctx.contractBonus = opt.contractBonus;
    // The seed is a pure function of item, hour and re-roll epoch: stable across rebuilds and restarts, different after a re-roll.
    ctx.seed = quint32(qHash(item.id, 0) ^ quint32(hourStartMs / 3600000LL) ^ (opt.seedSalt * 2654435761u)) | 1u;
    // Picks other hours already hold (cached, not aired yet) count as plays for rotation, so the next hour does not
    // take the same least-recently-aired files the airing hour did.
    if (opt.pickCache) {
        const QString own = QString::number(hourStartMs) + "#";
        for (auto it = opt.pickCache->cbegin(); it != opt.pickCache->cend(); ++it)
            if (!it.key().endsWith("@spec") && !it.key().startsWith(own))
                for (const QString& id : it.value()) ctx.planned[id] += 1;
    }

    // Pool: a block with types can only match those types (QueryBlocks::matches), so it reads just that indexed subset;
    // folder blocks and blocks without types match on more than type and read everything (else the typed subset). Loaded
    // lazily and at most once per block: a block whose cached picks are still good never reads the library (a rebuild used
    // to parse every media row once per block, seconds on a big library, on the executor's thread).
    QList<MediaRecord> pool; bool poolLoaded = false; QHash<QString, const MediaRecord*> poolById;
    auto loadPool = [&]() {
        if (poolLoaded) return;
        poolLoaded = true;
        if (!isFolder && !block.types.isEmpty() && media.byTypes) pool = media.byTypes(block.types);   // the store matches types case-insensitively
        else if (media.all) pool = media.all();
        else if (media.byTypes) pool = media.byTypes(isFolder || block.types.isEmpty() ? QStringList{"program","ad","promo","station_id","bumper","filler","psa","news","weather","video","image"} : block.types);
        for (const MediaRecord& m : std::as_const(pool)) poolById.insert(m.id, &m);
    };
    auto recordFor = [&](const QString& id, MediaRecord* outRec) -> bool {
        if (!poolLoaded && media.byId) { bool f = false; const MediaRecord m = media.byId(id, &f); if (f) *outRec = m; return f; }
        loadPool();
        const MediaRecord* m = poolById.value(id, nullptr);
        if (m) *outRec = *m;
        return m != nullptr;
    };

    // Pick cache: "<hourStartMs>#<itemId>" -> media ids, plus "<key>@spec" -> a hash of the block spec. A cached pick is
    // reused only while the spec is unchanged and every pick is still playable, still matches the block and is not taken by
    // another item of the hour; any gap re-resolves the whole block (an edited block, or an explicit item added later,
    // must not be shadowed by stale picks).
    const QString cacheKey = QString::number(hourStartMs) + "#" + item.id;
    const QString specKey = cacheKey + "@spec";
    const QString spec = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(isFolder ? folder.toJson() : block.toJson()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex().left(12));
    auto stillValid = [&](const MediaRecord& m) {
        if (!QueryBlocks::eligible(m, ctx)) return false;
        if (isFolder) return QueryBlocks::matchesFolder(m, folder) && (folder.cond.isEmpty() || folder.cond.matches(m, ctx.nowMs)) && !ctx.exclude.contains(m.id);
        return QueryBlocks::matches(m, block, ctx) && (!block.series.isEmpty() || !ctx.exclude.contains(m.id));   // series blocks ignore exclusions
    };
    QList<QueryPick> picks;
    bool fromCache = false;
    if (opt.pickCache && opt.pickCache->contains(cacheKey) && opt.pickCache->value(specKey).value(0) == spec) {
        bool ok = true;
        for (const QString& id : opt.pickCache->value(cacheKey)) {
            MediaRecord m;
            if (!recordFor(id, &m) || !stillValid(m)) { ok = false; break; }
            QueryPick p; p.media = m; picks.append(p);
        }
        fromCache = ok && !picks.isEmpty();
        if (!fromCache) picks.clear();
    }
    if (!fromCache) {
        loadPool();      // an empty pool is reported by the resolver ("no media matches ...", "folder ... has no playable media")
        if (opt.avoidPicks) for (const QString& id : opt.avoidPicks->value(cacheKey)) ctx.avoid.insert(id);   // a re-roll moves on from its previous picks
        picks = isFolder ? QueryBlocks::resolveFolder(pool, folder, ctx, why) : QueryBlocks::resolve(pool, block, ctx, why);
        if (opt.avoidPicks) opt.avoidPicks->remove(cacheKey);
        if (opt.pickCache) {
            QStringList ids; for (const auto& p : picks) ids << p.media.id;
            if (!ids.isEmpty()) { opt.pickCache->insert(cacheKey, ids); opt.pickCache->insert(specKey, QStringList{spec}); }
            else { opt.pickCache->remove(cacheKey); opt.pickCache->remove(specKey); }
        }
    }
    if (picks.isEmpty()) return out;

    // The first pick of a block with a target may be longer than the target (nothing fitted): it is cut to the target so the
    // rest of the hour keeps its shape (the flag is recomputed here because cached picks do not carry it).
    const double target = isFolder ? folder.targetSec : (block.series.isEmpty() ? block.targetSec : 0.0);
    const bool cutToTarget = target > 0.0 && (isFolder || block.fitToTarget);
    int k = 0;
    for (const QueryPick& p : picks) {
        SlotItem it = item;                      // keep locks, anchors, cg hooks, notes
        it.id = item.id + "#" + QString::number(k);
        it.queryBlock = QJsonObject(); it.folderBlock = QJsonObject();
        it.extra.remove("queryBlock"); it.extra.remove("query_block"); it.extra.remove("folderBlock"); it.extra.remove("folder_block");
        it.mediaId = p.media.id; it.path = p.media.path; it.title = p.media.title; it.type = p.media.type.isEmpty() ? "video" : p.media.type;
        it.durationSec = p.media.durationSec; it.inPointSec = 0.0; it.outPointSec = 0.0; it.segmentId.clear();
        it.extra["queryFill"] = true; it.extra["blockLabel"] = isFolder ? folder.label() : block.label(); it.extra["blockItemId"] = item.id;
        const bool overrun = (k == 0 && QueryBlocks::isOverrun(p.media.durationSec, target)) || p.overrun;
        if (overrun) { it.extra["overrun"] = true; if (cutToTarget && k == 0) it.outPointSec = target; }
        if (p.repeat) it.extra["repeat"] = true;
        if (k > 0) { it.timingMode = "soft_start"; it.fixedOffsetSec = -1.0; it.fixedUtcMs = 0; it.lockAtSec = -1.0; it.locked = false; it.cgOnStart.clear(); }
        exclude.insert(p.media.id);
        out.append(it); ++k;
    }
    return out;
}

void RunlistBuilder::seatAnchors(QList<SlotItem>& items) {
    QList<SlotItem> anchors, body;
    for (const SlotItem& it : items) { if (it.anchorSec() >= 0.0 && !it.isHardStart()) anchors.append(it); else body.append(it); }
    if (anchors.isEmpty()) return;
    std::sort(anchors.begin(), anchors.end(), [](const SlotItem& a, const SlotItem& b) { return a.anchorSec() < b.anchorSec(); });
    auto secOf = [](const SlotItem& it) { return it.isQuery() && it.durationSec > 0 ? it.durationSec : it.effectiveDurationSec(); };
    for (const SlotItem& a : anchors) {
        // nearest item boundary to the anchor offset, never splitting an item
        int best = 0; double bestD = std::fabs(a.anchorSec()), run = 0.0;
        for (int i = 0; i < body.size(); ++i) { run += secOf(body[i]); const double d = std::fabs(run - a.anchorSec()); if (d < bestD) { best = i + 1; bestD = d; } }
        body.insert(best, a);
    }
    items = body;
}

// ---------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------

Runlist RunlistBuilder::build(const HourSlot& slot, qint64 hourStartMs, const MediaLookup& media, const BuildOptions& opt) {
    Runlist rl;
    rl.channelId = slot.channelId;
    rl.slotId = slot.id;
    rl.hourStartMs = hourStartMs;
    rl.hourEndMs = hourStartMs + 3600000LL;
    const qint64 nowMs = opt.nowMs ? opt.nowMs : QDateTime::currentMSecsSinceEpoch();
    // Runlist ids of a weekly or dated hour are keyed by the hour, not the slot id: saving a weekly hour as a dated
    // override mid-hour (a file dropped onto the live rundown) keeps the ids of unchanged items, so the item on air is
    // not re-cued. Other slots (synthesized, fixed source) keep their own id, which may repeat across hours on purpose.
    int pDow = -1, pHour = -1; QDate pDate;
    const QString idKey = HourSlot::parseId(slot.id, &pDow, &pDate, &pHour) ? QString("h%1").arg(hourStartMs) : slot.id;

    // Pass 0: expand late-bound blocks (query / folder) into concrete items; picks never repeat within the
    // hour or against what the neighbouring hour already took.
    QSet<QString> exclude = opt.excludeIds;
    for (const SlotItem& it : slot.items) if (!it.isQuery() && !it.mediaId.isEmpty()) exclude.insert(it.mediaId);
    QList<SlotItem> expanded;
    for (const SlotItem& it : slot.items) {
        if (!it.isQuery()) { expanded.append(it); continue; }
        QString why;
        const QList<SlotItem> picks = expandQuery(it, media, opt, slot.channelId, hourStartMs, exclude, &why);
        if (picks.isEmpty()) { rl.notes << QString("block '%1' unresolved: %2").arg(it.title.isEmpty() ? it.id : it.title, why); continue; }
        double got = 0; for (const auto& p : picks) got += p.effectiveDurationSec();
        rl.notes << QString("block '%1' -> %2 item(s), %3 s").arg(it.title.isEmpty() ? it.id : it.title).arg(picks.size()).arg(got, 0, 'f', 0);
        expanded.append(picks);
    }
    // Locked anchors (lockAtSec) sit at the item boundary nearest their offset.
    seatAnchors(expanded);

    // Pass 1: resolve items and compute their anchors.
    struct Pending { SlotItem item; qint64 anchorMs; bool hard; bool fill; };
    QList<Pending> pend;
    int n = 0;
    for (SlotItem it : expanded) {
        ++n;
        if (!it.isPlayable()) continue;   // cg/gap items are handled by the engine CG scheduler later
        QString why;
        if (!resolveItem(it, media, nowMs, &why)) {
            rl.notes << QString("skipped '%1': %2").arg(it.title.isEmpty() ? it.id : it.title, why);
            continue;
        }
        Pending p; p.item = it; p.fill = it.isFillUntil(); p.hard = false; p.anchorMs = 0;
        if (it.isHardStart()) {
            p.hard = true;
            p.anchorMs = it.fixedUtcMs > 0 ? it.fixedUtcMs : hourStartMs + qint64(std::llround(it.fixedOffsetSec * 1000.0));
            if (p.anchorMs < hourStartMs || p.anchorMs >= rl.hourEndMs) {
                rl.notes << QString("'%1' hard start %2 is outside this hour; treated as soft").arg(it.title).arg(p.anchorMs);
                p.hard = false;
            }
        }
        pend.append(p);
    }

    // Pass 2: lay out on the wall clock (a continuation hour starts where the previous hour's programme ends).
    qint64 cursor = hourStartMs + qint64(qBound(0.0, opt.startOffsetSec, 3600.0) * 1000.0);
    int idx = 0;
    const bool anchorHourStart = !opt.continuesFromPreviousHour;
    auto nextHardAfter = [&](int from) -> int {
        for (int j = from + 1; j < pend.size(); ++j) if (pend[j].hard) return j;
        return -1;
    };
    // Where an item in front of the next hard start must end: the anchor, or (finish_previous) the anchor plus the lateness the
    // hard item allows. The hour end when no hard start follows.
    auto nextAnchorAfter = [&](int from) -> qint64 {
        const int j = nextHardAfter(from);
        if (j < 0) return rl.hourEndMs;
        if (pend[j].item.extra.value("conflictPolicy").toString() == "finish_previous")
            return pend[j].anchorMs + qint64(qMin((pend[j].item.extra.value("maxLateSec").toDouble(0.0) > 0.0 ? pend[j].item.extra.value("maxLateSec").toDouble() : 1.0e9), 1.0e6) * 1000.0);
        return pend[j].anchorMs;
    };
    auto appendFill = [&](qint64 gapStartMs, qint64 gapEndMs, bool atTop) {
        const double gap = (gapEndMs - gapStartMs) / 1000.0;
        if (!opt.fillGaps || gap < opt.fill.minGapSec || !media.byTypes) { rl.gapSec += gap; return; }
        FillContext fc; fc.nowMs = nowMs; fc.channelId = slot.channelId; fc.seed = opt.fillSeed;
        if (!pend.isEmpty()) fc.currentSeries = idx < pend.size() ? QString() : QString();
        QList<MediaRecord> pool = media.byTypes(FillPolicy::fillTypes(opt.fill));
        const QList<MediaRecord> picks = FillPolicy::fillGap(gap, pool, fc, opt.fill, atTop);
        qint64 c = gapStartMs; int k = 0;
        for (const auto& m : picks) {
            RunlistItem r;
            r.item.id = "fill-" + m.id; r.item.mediaId = m.id; r.item.path = m.path; r.item.title = m.title; r.item.type = m.type;
            r.item.durationSec = m.durationSec;
            r.runlistId = QString("%1#fill%2#%3").arg(idKey).arg(idx).arg(k++);
            r.plannedStartMs = c; r.durationSec = m.durationSec; r.plannedEndMs = c + qint64(m.durationSec * 1000.0);
            r.hard = (c == hourStartMs && anchorHourStart && rl.items.isEmpty());
            r.origin = "fill"; r.slotId = slot.id;
            rl.items.append(r); rl.filledSec += m.durationSec; c = r.plannedEndMs;
        }
        const double left = (gapEndMs - c) / 1000.0;
        rl.gapSec += left;
        // Report what was placed, not the size of the gap (the old note said "filled 3473.7 s" for eight minutes of clips).
        if (!picks.isEmpty()) rl.notes << QString("filled %1 s of a %2 s gap at %3 with %4 item(s)").arg((c - gapStartMs) / 1000.0, 0, 'f', 1).arg(gap, 0, 'f', 1).arg(gapStartMs).arg(picks.size());
        if (left >= qMax(opt.fill.minGapSec, 30.0)) rl.notes << QString("fill ran short: %1 s left empty (fill pool too small or too long)").arg(left, 0, 'f', 1);
    };

    // An event that did not fit: left out of the runlist but recorded, with its full length, so overrun can be measured and corrected.
    auto noteDropped = [&](const SlotItem& it, const QString& why, qint64 atMs, double nominalSec) {
        RunlistItem d; d.item = it; d.slotId = slot.id; d.origin = "dropped"; d.note = why;
        d.runlistId = QString("%1#%2#dropped").arg(idKey, it.id);
        d.durationSec = nominalSec > 0.0 ? nominalSec : it.effectiveDurationSec();
        d.plannedStartMs = atMs; d.plannedEndMs = atMs + qint64(d.durationSec * 1000.0);
        rl.dropped.append(d);
    };
    auto dropAtOf = [&](int i, qint64 start) -> qint64 { const int hj = nextHardAfter(i); return qMin(start, (hj >= 0 ? pend[hj].anchorMs : rl.hourEndMs) - 1); };
    int segStart = 0;   // index in rl.items of the first item after the last hard-started one: the interval backtiming shifts
    for (int i = 0; i < pend.size(); ++i) {
        Pending& p = pend[i];
        qint64 start = cursor;
        qint64 lateMs = 0;
        if (p.hard) {
            // What a hard start does to the clip still playing is the item's own decision (extra.conflictPolicy):
            //   truncate         (default) the clip is cut at the anchor: the exact start wins
            //   finish_previous  the clip finishes and this item starts late, by at most extra.maxLateSec (default: no limit);
            //                    past that the clip is cut at anchor + maxLateSec
            //   drop_previous    the clip is not played at all when it would be cut (the gap before the anchor is filled)
            const QString policy = p.item.extra.value("conflictPolicy").toString("truncate");
            if (p.anchorMs < cursor && !rl.items.isEmpty() && policy == "drop_previous") {
                const RunlistItem dropped = rl.items.takeLast();
                rl.notes << QString("dropped '%1': it would be cut by the hard start of '%2' (policy drop_previous)").arg(dropped.item.title, p.item.title);
                cursor = dropped.plannedStartMs;
                if (segStart > rl.items.size()) segStart = rl.items.size();
            }
            if (p.anchorMs > cursor) {
                const bool backtime = p.item.extra.value("backtime").toBool(false) && segStart < rl.items.size();
                if (!backtime) {
                    appendFill(cursor, p.anchorMs, cursor == hourStartMs);
                } else {
                    // Backtiming: the events since the last anchor are timed backward from this start, so the gap goes
                    // in front of them and they end exactly on the anchor.
                    const qint64 gapMs = p.anchorMs - cursor, segStartMs = rl.items[segStart].plannedStartMs;
                    const int oldSize = rl.items.size();
                    for (int k = segStart; k < oldSize; ++k) { rl.items[k].plannedStartMs += gapMs; rl.items[k].plannedEndMs += gapMs; rl.items[k].hard = false; }
                    const double gapBefore = rl.gapSec;
                    appendFill(segStartMs, segStartMs + gapMs, false);
                    const qint64 leftMs = qint64(std::llround((rl.gapSec - gapBefore) * 1000.0));    // what the fill could not cover stays empty at the front
                    for (int k = oldSize; k < rl.items.size(); ++k) { rl.items[k].plannedStartMs += leftMs; rl.items[k].plannedEndMs += leftMs; rl.items[k].hard = false; }
                    std::rotate(rl.items.begin() + segStart, rl.items.begin() + oldSize, rl.items.end());
                    // The engine only waits for hard starts: filler in front of the block follows the top of the hour; when the
                    // filler could not cover the front, the first backtimed item gets its own exact start (the engine holds until then).
                    if (leftMs == 0 && segStartMs == hourStartMs && anchorHourStart && rl.items.size() > oldSize) rl.items[segStart].hard = true;
                    else if (leftMs > 0) rl.items[segStart + (rl.items.size() - oldSize)].hard = true;
                    rl.notes << QString("backtimed %1 item(s) to end on the hard start of '%2'%3").arg(oldSize - segStart).arg(p.item.title, leftMs > 0 ? QString(" (%1 s of the front is not filled: the first item holds for its start)").arg(leftMs / 1000.0, 0, 'f', 0) : QString());
                }
            } else if (p.anchorMs < cursor && !rl.items.isEmpty()) {
                RunlistItem& prev = rl.items.last();
                double cut = (cursor - p.anchorMs) / 1000.0;
                if (policy == "finish_previous") {
                    const double maxLate = p.item.extra.value("maxLateSec").toDouble(0.0) > 0.0 ? p.item.extra.value("maxLateSec").toDouble() : 1.0e9;      // blank / 0 = no limit
                    if (cut <= maxLate) {
                        lateMs = cursor - p.anchorMs;
                        rl.notes << QString("'%1' starts %2 s late: '%3' finishes first (policy finish_previous)").arg(p.item.title).arg(cut, 0, 'f', 1).arg(prev.item.title);
                        cut = 0.0;
                    } else {
                        lateMs = qint64(maxLate * 1000.0);
                        cut -= maxLate;     // late by the allowed amount, the rest of the overlap is cut off the clip
                    }
                }
                if (cut > 0.0) {
                    if (prev.durationSec - cut < opt.minItemSec) {
                        rl.notes << QString("dropped '%1': hard start of '%2' leaves no room").arg(prev.item.title, p.item.title);
                        noteDropped(prev.item, "hard start leaves no room", p.anchorMs - 1, prev.item.effectiveDurationSec());
                        rl.items.removeLast();
                    } else {
                        prev.durationSec -= cut;
                        prev.outSec = prev.inSec + prev.durationSec;
                        prev.plannedEndMs = cursor - qint64(std::llround(cut * 1000.0));
                        prev.truncated = true;
                        prev.note = QString("truncated %1 s for hard start of '%2'").arg(cut, 0, 'f', 1).arg(p.item.title);
                        rl.notes << prev.note;
                    }
                }
            }
            start = p.anchorMs + lateMs;
        }

        RunlistItem r;
        r.item = p.item;
        r.slotId = slot.id;
        r.runlistId = QString("%1#%2#%3").arg(idKey).arg(p.item.id.isEmpty() ? QString::number(i) : p.item.id).arg(idx++);
        r.inSec = p.item.inPointSec;
        r.outSec = p.item.outPointSec > p.item.inPointSec ? p.item.outPointSec : 0.0;
        double dur = p.item.effectiveDurationSec();
        const qint64 limitMs = p.fill ? nextAnchorAfter(i) : (opt.continuesIntoNextHour ? LLONG_MAX : qMin(nextAnchorAfter(i), rl.hourEndMs));
        if (p.fill) {
            dur = qMax(0.0, (limitMs - start) / 1000.0);
            r.loop = true;
            r.note = "fill_until";
        } else if (p.item.loop) {
            r.loop = true;
        }
        if (dur < opt.minItemSec) { rl.notes << QString("dropped '%1': no time left in hour").arg(p.item.title); if (!p.fill) noteDropped(p.item, "no time left", dropAtOf(i, start), p.item.effectiveDurationSec()); continue; }
        r.plannedStartMs = start;
        r.durationSec = dur;
        r.plannedEndMs = start + qint64(std::llround(dur * 1000.0));
        // Truncate at the hour end (or the next hard anchor) unless the event continues.
        if (!p.fill && r.plannedEndMs > limitMs && limitMs != LLONG_MAX) {
            const int hj = nextHardAfter(i);
            if (hj >= 0 && limitMs == pend[hj].anchorMs && pend[hj].item.extra.value("conflictPolicy").toString() == "drop_previous") {
                rl.notes << QString("dropped '%1': it would be cut by the hard start of '%2' (policy drop_previous)").arg(p.item.title, pend[hj].item.title);
                continue;       // the gap before the anchor is filled when the hard item is placed
            }
            const double cut = (r.plannedEndMs - limitMs) / 1000.0;
            r.durationSec -= cut;
            if (r.durationSec < opt.minItemSec) { rl.notes << QString("dropped '%1': starts too late in the hour").arg(p.item.title); noteDropped(p.item, "starts too late", dropAtOf(i, start), p.item.effectiveDurationSec()); continue; }
            r.outSec = r.inSec + r.durationSec;
            r.plannedEndMs = limitMs;
            r.truncated = true;
            r.note = QString("truncated %1 s at %2").arg(cut, 0, 'f', 1).arg(limitMs == rl.hourEndMs ? "end of hour" : "next hard start");
            rl.notes << QString("'%1' %2").arg(p.item.title, r.note);
        }
        r.hard = (p.hard && lateMs == 0) || (start == hourStartMs && anchorHourStart && rl.items.isEmpty());   // a late start is not an exact one: the engine must not cut to it
        if (lateMs > 0 && r.note.isEmpty()) r.note = QString("late start +%1 s (previous item finished)").arg(lateMs / 1000.0, 0, 'f', 1);
        r.origin = p.item.extra.value("queryFill").toBool(false) ? "query" : (p.item.smartConfig.isEmpty() ? "schedule" : "smart");
        if (p.item.anchorSec() >= 0.0 && r.note.isEmpty()) r.note = QString("anchor :%1").arg(int(p.item.anchorSec() / 60.0), 2, 10, QChar('0'));
        if (r.note.isEmpty() && p.item.extra.value("overrun").toBool(false) && p.item.outPointSec > 0.0) r.note = QString("cut to the block target (%1 s)").arg(p.item.outPointSec, 0, 'f', 0);
        rl.scheduledSec += r.durationSec;
        rl.items.append(r);
        if (p.hard) segStart = rl.items.size();
        cursor = r.plannedEndMs;
        if (cursor >= rl.hourEndMs && !opt.continuesIntoNextHour) {
            if (i + 1 < pend.size()) rl.notes << QString("%1 item(s) after '%2' do not fit in the hour").arg(pend.size() - i - 1).arg(p.item.title);
            for (int k = i + 1; k < pend.size(); ++k) noteDropped(pend[k].item, "does not fit in the hour", rl.hourEndMs - 1, pend[k].item.effectiveDurationSec());
            break;
        }
    }

    // Tail gap. An hour with nothing scheduled at all is left to the executor's
    // emergency loop (typed order, repeating) rather than random gap fill.
    if (pend.isEmpty()) {
        rl.gapSec = 3600.0;
        rl.notes << "hour has no playable items";
    } else if (cursor < rl.hourEndMs && !opt.continuesIntoNextHour) {
        if (slot.isLooping && !rl.items.isEmpty()) {
            // Loop the hour's content to fill the remainder.
            const int base = rl.items.size();
            int k = 0; qint64 c = cursor;
            while (c < rl.hourEndMs && k < 200) {
                RunlistItem r = rl.items[k % base];
                if (r.origin == "fill") { ++k; if (k % base == 0) break; continue; }
                r.runlistId += QString("~loop%1").arg(k);
                r.plannedStartMs = c; r.hard = false; r.origin = "schedule"; r.note = "hour loop";
                const qint64 end = c + qint64(r.durationSec * 1000.0);
                if (end > rl.hourEndMs) { r.durationSec = (rl.hourEndMs - c) / 1000.0; r.outSec = r.inSec + r.durationSec; r.truncated = true; }
                r.plannedEndMs = c + qint64(r.durationSec * 1000.0);
                if (r.durationSec >= opt.minItemSec) rl.items.append(r);
                c = r.plannedEndMs; ++k;
            }
            cursor = c;
            rl.notes << "hour loop enabled";
        } else {
            appendFill(cursor, rl.hourEndMs, cursor == hourStartMs);
            cursor = rl.hourEndMs;
        }
    }
    return rl;
}

} // namespace sched
