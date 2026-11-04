#include "scheduleexecutor.h"
#include "formatclock.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QDateTime>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <memory>

namespace sched {

namespace {
QString sha1Hex(const QJsonObject& o, int len) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(o).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex().left(len));
}
// The settings a rebuild reads (prefixes, see ScheduleStore::settingsRevision(keyPrefixes)): recipe autofill and ad-contract
// pacing. Everything else the build uses is the channel profile (re-read by reloadChannel), the slots and the media library.
const QStringList kExecutorSettingKeys = {QStringLiteral("programming.recipes"), QStringLiteral("traffic.contracts")};
} // namespace

QJsonObject ScheduleExecutor::ChannelStatus::toJson() const {
    QJsonObject o;
    o["channelId"] = channelId; o["armed"] = armed; o["connected"] = connected; o["emergency"] = emergency;
    o["currentSlotId"] = currentSlotId; o["hourStartMs"] = double(hourStartMs); o["lastPushMs"] = double(lastPushMs);
    o["lastPushReason"] = lastPushReason; o["pushedItems"] = pushedItems; o["engineState"] = engineState;
    o["onAirRunlistId"] = onAirRunlistId; o["onAirTitle"] = onAirTitle; o["driftSec"] = driftSec; o["rebuilds"] = rebuilds;
    return o;
}

ScheduleExecutor::ScheduleExecutor(ScheduleStore* store, EngineLink* link, QObject* parent)
    : QObject(parent), m_store(store), m_link(link)
{
    qRegisterMetaType<sched::AsRunRow>("sched::AsRunRow");
    connect(&m_timer, &QTimer::timeout, this, &ScheduleExecutor::tick);
}

void ScheduleExecutor::start(int intervalMs) { m_timer.start(intervalMs); }
void ScheduleExecutor::stop() { m_timer.stop(); }

qint64 ScheduleExecutor::nowMs() const { return m_now ? m_now() : QDateTime::currentMSecsSinceEpoch(); }

MediaLookup ScheduleExecutor::mediaLookup() const {
    MediaLookup ml;
    ScheduleStore* s = m_store;
    ml.byId = [s](const QString& id, bool* found) { return s->media(id, found); };
    ml.bySeries = [s](const QString& series) { return s->mediaBySeries(series); };
    // Pools are read lazily and at most once per lookup (one rebuild or one preview): reading and parsing every media row once
    // per query block froze the executor's thread for seconds on a big library. A typed block reads only its types (memoized
    // per type set); blocks whose cached picks are still good never touch the library (see RunlistBuilder::expandQuery).
    struct Snapshot { bool loaded = false; QList<MediaRecord> media; QHash<QString, QList<MediaRecord>> typed; };
    auto snap = std::make_shared<Snapshot>();
    ml.byTypes = [s, snap](const QStringList& types) {
        const QString key = types.join(QChar(0x1f));
        auto it = snap->typed.find(key);
        if (it == snap->typed.end()) it = snap->typed.insert(key, s->mediaByType(types));
        return it.value();
    };
    ml.all = [s, snap]() { if (!snap->loaded) { snap->media = s->allMedia(false); snap->loaded = true; } return snap->media; };
    return ml;
}

// ---------------------------------------------------------------------------
// Hour synthesis: fixed-source channels, autofill policy for empty hours
// ---------------------------------------------------------------------------

HourSlot ScheduleExecutor::effectiveOrSynthesized(ChannelRun& ch, const QDate& d, int hour, qint64 hourStart, QStringList* notes, const MediaLookup& media) const {
    HourSlot s = m_store->effectiveSlot(ch.profile.id, d, hour);
    s.channelId = ch.profile.id;
    // A channel pinned to one live source (mux NDI/UDP feeds): the hour is that source, looping, whatever the schedule says.
    const QJsonObject fixed = ch.profile.extra.value("fixedSource").toObject();
    if (!fixed.isEmpty()) {
        // One endless item: neither the slot id nor the item id depends on the hour (the item id carries a hash of the source
        // spec), so the runlist id is the same every hour. The item on air stays the item that is planned: nothing is queued
        // at :00 (a second receiver and a hard cut to it), and a changed source spec is a different item that replaces it.
        SlotItem it; it.id = "fixed-" + sha1Hex(fixed, 8); it.type = "live"; it.timingMode = "fill_until"; it.title = fixed.value("title").toString(fixed.value("name").toString(fixed.value("url").toString("Live source")));
        it.liveSource = fixed;
        const QString t = fixed.value("type").toString();
        if (t == "ndi") it.liveSource["ndiName"] = fixed.value("name").toString(fixed.value("ndiName").toString());
        else if (t == "sdi") it.liveSource["port"] = fixed.value("port");
        else if (!fixed.value("url").toString().isEmpty()) it.liveSource["url"] = fixed.value("url");
        it.durationSec = 3600.0; it.locked = true;
        s.id = "fixed";
        s.items = {it}; s.isLooping = false;
        if (notes) *notes << "fixed source channel";
        return s;
    }
    if (!s.items.isEmpty()) return s;
    const AutofillPolicy pol = FormatClock::policyFor(ch.profile);
    if (!pol.enabled) return s;
    // The hour is synthesized again on every rebuild, so what it produces must not move: the ids are a function of the hour
    // (runlist ids, pick-cache keys and the push hash stay put), and the weighted layout, which is concrete media sorted by play
    // counts that change as items air, is frozen per hour in ChannelRun::synth.
    const QString idPrefix = QString("af-%1").arg(hourStart / 3600000LL);
    if (pol.mode == "weighted") {
        QList<WeightBucket> buckets = FormatClock::bucketsFromJson(ch.profile.extra.value("programWeights"));
        if (buckets.isEmpty()) buckets = FormatClock::defaultBuckets();
        // A long program the previous hour had to split leaves its remainder for this one.
        const QJsonObject carryIn = ch.synth.contains(hourStart - 3600000LL) ? ch.synth.value(hourStart - 3600000LL).carry : QJsonObject();
        QJsonObject sigSrc; sigSrc["policy"] = pol.toJson(); sigSrc["buckets"] = FormatClock::bucketsToJson(buckets); sigSrc["carry"] = carryIn;
        const QString sig = sha1Hex(sigSrc, 16);
        bool reuse = false;
        auto cached = ch.synth.constFind(hourStart);
        if (cached != ch.synth.constEnd() && cached->sig == sig) {
            reuse = true;
            const qint64 at = qMax(hourStart, nowMs());      // the upcoming hour is checked at its own start, the airing one right now
            for (const SlotItem& si : cached->items) {
                if (si.mediaId.isEmpty() || !media.byId) continue;
                bool f = false; const MediaRecord m = media.byId(si.mediaId, &f);
                if (!f || m.path.isEmpty() || !m.isAvailableAt(at)) { reuse = false; break; }    // gone or expired: lay the hour again
            }
        }
        if (!reuse) {
            const QList<MediaRecord> pool = media.all ? media.all() : m_store->allMedia(false);
            // The seed is a pure function of the hour and the re-roll epoch (stable across rebuilds); a re-roll also queues the
            // media of the layout it dropped last, so the rotation (ordered by play count and freshness, jitter only breaks
            // ties) really moves on instead of laying the very same hour again.
            auto st = FormatClock::beginWeighted(buckets, pool, ch.profile.id, hourStart, (quint32(hourStart / 3600000LL) ^ (ch.rerollEpoch * 2654435761u)) | 1u);
            auto av = ch.synthAvoid.find(hourStart);
            if (av != ch.synthAvoid.end()) {
                for (QList<MediaRecord>& q : st.queues) std::stable_partition(q.begin(), q.end(), [&](const MediaRecord& m) { return !av->contains(m.id); });
                ch.synthAvoid.erase(av);
            }
            st.carry = carryIn;
            const HourLayout lay = FormatClock::fillWeighted(buckets, st, s, true, true, 2400.0, idPrefix);
            SynthHour h; h.sig = sig; h.items = lay.items; h.notes = lay.notes; h.carry = lay.carry;
            ch.synth.insert(hourStart, h);
        }
        const SynthHour& h = ch.synth[hourStart];
        s.items = h.items;
        if (notes) *notes << "autofill (weighted): " + h.notes.join("; ");
    } else {
        HourRecipe r = FormatClock::defaultRecipe();
        for (const HourRecipe& x : FormatClock::recipesFromSettings(m_store->settingJson("programming.recipes"))) if (x.id == pol.recipeId || (pol.recipeId.isEmpty() && x.name.compare("default", Qt::CaseInsensitive) == 0)) { r = x; break; }
        const HourLayout lay = FormatClock::layRecipe(r, s, true, 200, idPrefix);
        s.items = lay.items;
        if (notes) *notes << "autofill (recipe '" + r.name + "')";
    }
    s.showBlock["_autofill"] = true;
    return s;
}

void ScheduleExecutor::refreshPacing(ChannelRun& ch, qint64 now) const {
    ch.pacing.clear();
    const AutofillPolicy pol = FormatClock::policyFor(ch.profile);
    if (!pol.adPacing) return;
    const QList<AdContract> contracts = FormatClock::contractsFromSettings(m_store->settingJson("traffic.contracts"));
    if (contracts.isEmpty()) return;
    ScheduleStore* st = m_store;
    const QDate today = QDateTime::fromMSecsSinceEpoch(now, ch.profile.tz()).date();
    ch.pacing = FormatClock::contractPacing(contracts, ch.profile.id, today, ch.profile.tz(), [st](const QString& id, qint64 a, qint64 b) { return st->airedCount(id, a, b); });
}

Runlist ScheduleExecutor::previewSlot(const QString& channelId, const HourSlot& slot0, qint64 hourStartMs, QHash<QString, QStringList>* sharedPicks) const {
    HourSlot slot = slot0; slot.channelId = channelId;
    auto it = m_channels.find(channelId);
    BuildOptions o;
    o.nowMs = nowMs();
    QHash<QString, QStringList> scratch;
    o.pickCache = sharedPicks ? sharedPicks : &scratch;
    if (slot.isContinuation()) { o.continuesFromPreviousHour = true; o.startOffsetSec = slot.mergedEvent.value("carrySec").toDouble(0.0); }
    if (it != m_channels.end()) {
        o.fill = FillSettings::fromJson(it->profile.fill);
        o.seedSalt = it->rerollEpoch;           // the preview resolves with the seeds the channel is airing with
        const QHash<QString, double> pacing = it->pacing;
        if (!pacing.isEmpty()) o.contractBonus = [pacing](const QString& id) { return pacing.value(id, 0.0); };
    } else {
        o.fill = FillSettings::fromJson(m_store->channel(channelId).fill);
    }
    return RunlistBuilder::build(slot, hourStartMs, mediaLookup(), o);
}

void ScheduleExecutor::rerollBlocks(const QString& channelId, bool fresh) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    ChannelRun& ch = *it;
    // A block that has started airing keeps its picks: the item on air must stay the item the runlist says is on air.
    const qint64 now = nowMs();
    QSet<QString> started;
    for (const Runlist* rl : {&ch.current, &ch.next})
        for (const RunlistItem& r : rl->items)
            if (r.plannedStartMs <= now && r.item.extra.value("queryFill").toBool(false))
                started.insert(QString::number(rl->hourStartMs) + "#" + r.item.extra.value("blockItemId").toString());
    // The others forget their picks, but remember them: resolution is a pure function of block, hour and library, so without
    // the avoid list (and the salt below) the same picks would simply come back.
    for (auto p = ch.picks.begin(); p != ch.picks.end();) {
        QString key = p.key();
        const bool isSpec = key.endsWith("@spec");
        if (isSpec) key.chop(5);
        if (started.contains(key)) { ++p; continue; }
        if (fresh && !isSpec) ch.avoid.insert(key, p.value());
        p = ch.picks.erase(p);
    }
    if (fresh) {
        ++ch.rerollEpoch;
        // Upcoming synthesized hours are laid again, without what they had (the airing hour's layout is concrete media on air).
        for (auto sy = ch.synth.begin(); sy != ch.synth.end();) {
            if (sy.key() > ch.hourStartMs) {
                QSet<QString>& gone = ch.synthAvoid[sy.key()];
                for (const SlotItem& si : sy->items) if (!si.mediaId.isEmpty()) gone.insert(si.mediaId);
                sy = ch.synth.erase(sy);
            } else ++sy;
        }
    }
    it->storeRevision = 0;          // forces rebuild on the next tick
    it->forcePush = true;
}

void ScheduleExecutor::addChannel(const QString& channelId) {
    if (m_channels.contains(channelId)) return;
    ChannelRun ch;
    ch.profile = m_store->channel(channelId);
    m_channels.insert(channelId, ch);
}

void ScheduleExecutor::removeChannel(const QString& channelId) { m_channels.remove(channelId); }

void ScheduleExecutor::onEngineConnected(const QString& channelId) {
    if (!m_channels.contains(channelId)) addChannel(channelId);
    ChannelRun& ch = m_channels[channelId];
    ch.connected = true;
    ch.pushedHash.clear();          // an empty hash makes the next tick push with reason engine_connected
    ch.configSent = false;
    ch.emergency = false;
    tick();
}

void ScheduleExecutor::onEngineDisconnected(const QString& channelId) {
    if (!m_channels.contains(channelId)) return;
    m_channels[channelId].connected = false;
    m_channels[channelId].pushedHash.clear();
}

void ScheduleExecutor::onTelemetry(const QString& channelId, const QJsonObject& state) {
    if (!m_channels.contains(channelId)) return;
    ChannelRun& ch = m_channels[channelId];
    ch.state = state;
    ch.stateMs = nowMs();
    const QString onAir = state.value("itemId").toString();
    // An operator take just went to air: queue the schedule behind it (never cutting it), so the hour resumes when it ends.
    const QString st = state.value("state").toString();
    if (onAir.startsWith("take-") && (st == "ON_AIR" || st == "PAUSED")) {
        const double dur = state.value("duration").toDouble(0.0), pos = state.value("position").toDouble(0.0);
        ch.takeEndsAtMs = dur > 0.0 ? ch.stateMs + qint64(qMax(0.0, dur - pos) * 1000.0) : 0;
        if (onAir != ch.lastTakeId) { ch.lastTakeId = onAir; ch.forcePush = true; }
    }
    if (const RunlistItem* ri = lookupRunlistItem(channelId, onAir)) {
        if (!ch.profile.extra.value("fixedSource").toObject().isEmpty()) {
            ch.driftSec = 0.0;      // an endless live loop airs across hours: it has no schedule position to drift from
        } else {
            const double expected = (ch.stateMs - ri->plannedStartMs) / 1000.0 + ri->inSec;
            const double actual = state.value("position").toDouble() + ri->inSec;
            ch.driftSec = actual - expected;
        }
    } else {
        ch.driftSec = 0.0;
    }
}

void ScheduleExecutor::setArmed(const QString& channelId, bool armed) {
    if (!m_channels.contains(channelId)) return;
    ChannelRun& ch = m_channels[channelId];
    ch.profile.armed = armed;
    m_store->saveChannel(ch.profile);
    if (armed) { ch.forcePush = true; tick(); }
}

void ScheduleExecutor::returnToSchedule(const QString& channelId) {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return;
    ChannelRun& ch = *it;
    const qint64 now = nowMs();
    ch.takeEndsAtMs = now;          // lay the schedule out from now (the take ends now), queued behind it
    ch.forcePush = true;
    evaluate(ch, now);
}

void ScheduleExecutor::loadHourNow(const QString& channelId) {
    if (!m_channels.contains(channelId)) return;
    m_channels[channelId].forcePush = true;
    m_channels[channelId].pushedHash.clear();
    tick();
}

void ScheduleExecutor::snapToSchedule(const QString& channelId) {
    if (!m_channels.contains(channelId)) return;
    ChannelRun& ch = m_channels[channelId];
    const qint64 now = nowMs();
    const QString onAir = ch.state.value("itemId").toString();
    const RunlistItem* ri = lookupRunlistItem(channelId, onAir);
    if (ri && now >= ri->plannedStartMs && now < ri->plannedEndMs) {
        QJsonObject p; p["seconds"] = (now - ri->plannedStartMs) / 1000.0;
        m_link->transport(channelId, "seek", p);
        emit alert(channelId, "info", QString("snapped to schedule (%1 s)").arg(p["seconds"].toDouble(), 0, 'f', 1));
    } else {
        loadHourNow(channelId);
    }
}

void ScheduleExecutor::reloadChannel(const QString& channelId) {
    if (!m_channels.contains(channelId)) return;
    ChannelRun& ch = m_channels[channelId];
    ch.profile = m_store->channel(channelId);
    ch.configSent = false;
    ch.pushedHash.clear();
    ch.plannedHash = plannedHashOf(ch);     // a fixed-source channel hashes its (re-read) source spec
    tick();
}

ScheduleExecutor::ChannelStatus ScheduleExecutor::status(const QString& channelId) const {
    ChannelStatus s; s.channelId = channelId;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return s;
    const ChannelRun& ch = it.value();
    s.armed = ch.profile.armed; s.connected = ch.connected; s.emergency = ch.emergency;
    s.currentSlotId = ch.current.slotId; s.hourStartMs = ch.hourStartMs; s.lastPushMs = ch.lastPushMs;
    s.lastPushReason = ch.lastPushReason; s.pushedItems = ch.pushedItems;
    s.engineState = ch.state.value("state").toString(); s.onAirRunlistId = ch.state.value("itemId").toString();
    s.onAirTitle = ch.state.value("title").toString(); s.driftSec = ch.driftSec; s.rebuilds = ch.rebuilds;
    return s;
}

QList<Runlist> ScheduleExecutor::builtRunlists(const QString& channelId) const {
    QList<Runlist> out;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return out;
    if (!it->current.items.isEmpty()) out << it->current;
    if (!it->next.items.isEmpty()) out << it->next;
    return out;
}
QHash<QString, QStringList> ScheduleExecutor::pickCacheCopy(const QString& channelId) const {
    auto it = m_channels.find(channelId);
    return it == m_channels.end() ? QHash<QString, QStringList>() : it->picks;
}

QJsonObject ScheduleExecutor::runlistJson(const QString& channelId) const {
    QJsonObject o;
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return o;
    o["channelId"] = channelId;
    o["current"] = it->current.toJson();
    o["next"] = it->next.toJson();
    o["status"] = status(channelId).toJson();
    return o;
}

const RunlistItem* ScheduleExecutor::lookupRunlistItem(const QString& channelId, const QString& runlistId) const {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end() || runlistId.isEmpty()) return nullptr;
    auto r = it->byRunlistId.find(runlistId);
    return r == it->byRunlistId.end() ? nullptr : &r.value();
}

QJsonObject ScheduleExecutor::engineConfigFor(const ChannelProfile& p) const {
    QJsonObject c;
    c["channelId"] = p.id; c["channelName"] = p.name; c["frameRate"] = p.frameRate;
    c["width"] = p.width; c["height"] = p.height; c["audioChannels"] = p.audioChannels;
    c["prerollSec"] = p.prerollSec; c["outputs"] = p.outputs;
    QJsonArray caps = p.identity.value("slateCaptions").toArray();
    if (caps.isEmpty()) { caps << (p.callLetters.isEmpty() ? p.name.toUpper() : p.callLetters) << "STAND BY"; }
    c["slateCaptions"] = caps;
    QJsonObject identity = p.identity;
    if (!identity.contains("callLetters")) identity["callLetters"] = p.callLetters;
    if (!identity.contains("brandName")) identity["brandName"] = p.brandName.isEmpty() ? p.name : p.brandName;
    c["identity"] = identity;
    c["gpu"] = p.extra.value("gpu").toBool(false);
    c["hwDecode"] = p.extra.value("hwDecode").toBool(true);
    if (p.extra.value("gpuMode").isString()) c["gpuMode"] = p.extra.value("gpuMode").toString();
    if (p.extra.value("mixes").isArray()) c["mixes"] = p.extra.value("mixes").toArray();     // output mixes (see PlayoutEngine::Config)
    return c;
}

// ---------------------------------------------------------------------------
// Building and pushing
// ---------------------------------------------------------------------------

void ScheduleExecutor::rebuild(ChannelRun& ch, qint64 now, qint64 hourStart) {
    const QTimeZone tz = ch.profile.tz();
    QDate d0; int h0 = 0;
    RunlistBuilder::hourStartUtcMsForTime(hourStart, tz, &d0, &h0);
    const qint64 nextStart = hourStart + 3600000LL;
    QDate d1; int h1 = 0;
    RunlistBuilder::hourStartUtcMsForTime(nextStart, tz, &d1, &h1);

    // One lookup for the whole rebuild: its block pool is read at most once, shared by both hours and the autofill synthesis.
    const MediaLookup lookup = mediaLookup();
    QStringList synthNotes0, synthNotes1;
    HourSlot s0 = effectiveOrSynthesized(ch, d0, h0, hourStart, &synthNotes0, lookup);
    HourSlot s1 = effectiveOrSynthesized(ch, d1, h1, nextStart, &synthNotes1, lookup);

    // Query-block picks: cached per hour so an airing hour keeps its picks; keys of past hours are dropped
    // (a pick key is "<hourStartMs>#<itemId>", optionally with an "@spec" suffix).
    for (auto it = ch.picks.begin(); it != ch.picks.end();) {
        const qint64 k = it.key().section('#', 0, 0).toLongLong();
        if (k < hourStart) it = ch.picks.erase(it); else ++it;
    }
    for (auto it = ch.avoid.begin(); it != ch.avoid.end();) {
        const qint64 k = it.key().section('#', 0, 0).toLongLong();
        if (k < hourStart) it = ch.avoid.erase(it); else ++it;
    }
    // Synthesized layouts: the previous hour stays (its carry leads the airing hour's continuation).
    for (auto it = ch.synth.begin(); it != ch.synth.end();) { if (it.key() < hourStart - 3600000LL) it = ch.synth.erase(it); else ++it; }
    for (auto it = ch.synthAvoid.begin(); it != ch.synthAvoid.end();) { if (it.key() < hourStart) it = ch.synthAvoid.erase(it); else ++it; }
    refreshPacing(ch, now);
    const QHash<QString, double> pacing = ch.pacing;

    BuildOptions o;
    o.fill = FillSettings::fromJson(ch.profile.fill);
    o.nowMs = now;
    o.continuesFromPreviousHour = s0.isContinuation();
    o.continuesIntoNextHour = s1.isContinuation();
    // A continuation hour starts where the previous hour's programme ends: measured when the previous hour was built
    // (carryByHour), else what the operator's overrun choice recorded on the slot.
    if (s0.isContinuation()) o.startOffsetSec = ch.carryByHour.value(hourStart, s0.mergedEvent.value("carrySec").toDouble(0.0));
    for (auto it = ch.carryByHour.begin(); it != ch.carryByHour.end();) { if (it.key() < hourStart) it = ch.carryByHour.erase(it); else ++it; }
    o.pickCache = &ch.picks;
    o.seedSalt = ch.rerollEpoch;
    o.avoidPicks = &ch.avoid;
    if (!pacing.isEmpty()) o.contractBonus = [pacing](const QString& id) { return pacing.value(id, 0.0); };
    ch.current = RunlistBuilder::build(s0, hourStart, lookup, o);
    ch.current.notes = synthNotes0 + ch.current.notes;

    BuildOptions o1 = o;
    for (const auto& r : ch.current.items) if (r.origin == "query" && !r.item.mediaId.isEmpty()) o1.excludeIds.insert(r.item.mediaId);
    o1.continuesFromPreviousHour = s1.isContinuation();
    o1.startOffsetSec = 0.0;
    if (s1.isContinuation()) {
        qint64 end = 0; for (const auto& r : ch.current.items) end = qMax(end, r.plannedEndMs);
        o1.startOffsetSec = end > nextStart ? double(end - nextStart) / 1000.0 : s1.mergedEvent.value("carrySec").toDouble(0.0);
        ch.carryByHour.insert(nextStart, o1.startOffsetSec);
    }
    const QDate d2 = QDateTime::fromMSecsSinceEpoch(nextStart + 3600000LL, tz).date();
    const int h2 = QDateTime::fromMSecsSinceEpoch(nextStart + 3600000LL, tz).time().hour();
    o1.continuesIntoNextHour = m_store->effectiveSlot(ch.profile.id, d2, h2).isContinuation();
    ch.next = RunlistBuilder::build(s1, nextStart, lookup, o1);
    ch.next.notes = synthNotes1 + ch.next.notes;

    ch.hourStartMs = hourStart;
    ch.storeRevision = m_store->scheduleRevision();
    ch.settingsRevision = m_store->settingsRevision(kExecutorSettingKeys);
    ch.plannedHash = plannedHashOf(ch);          // once here, not once per tick (PS-7)
    ++ch.rebuilds;
    for (const auto& r : ch.current.items) ch.byRunlistId.insert(r.runlistId, r);
    for (const auto& r : ch.next.items) ch.byRunlistId.insert(r.runlistId, r);
    // Forget items older than two hours so the map does not grow forever.
    for (auto it = ch.byRunlistId.begin(); it != ch.byRunlistId.end();) {
        if (it->plannedEndMs < hourStart - 7200000LL) it = ch.byRunlistId.erase(it); else ++it;
    }
    emit runlistChanged(ch.profile.id, runlistJson(ch.profile.id));
}

QJsonArray ScheduleExecutor::composePlaylist(const ChannelRun& ch, qint64 now, bool* replaceCurrent, QString* reason) const {
    QJsonArray out;
    const QString onAir = ch.state.value("itemId").toString();
    const QString engineState = ch.state.value("state").toString();
    // An operator take on air is never cut: the schedule queues behind it, laid out from when the take ends.
    const bool takeOn = onAir.startsWith("take-") && (engineState == "ON_AIR" || engineState == "PAUSED");
    if (takeOn && ch.takeEndsAtMs > now) now = ch.takeEndsAtMs;
    bool replace = true;
    bool first = true;
    // An item from an earlier build (the previous hour's programme running into this one) that is still inside its planned
    // window keeps airing: the new hour's items queue after it instead of replacing it.
    const auto airingIt = ch.byRunlistId.constFind(onAir);
    const bool airingOn = !onAir.isEmpty() && (engineState == "ON_AIR" || engineState == "PAUSED") && airingIt != ch.byRunlistId.constEnd();
    const qint64 airingEndMs = airingOn ? airingIt->plannedEndMs : 0;
    qint64 firstStartMs = -1;
    QSet<QString> queued;
    auto consider = [&](const RunlistItem& r0) {
        if (r0.plannedEndMs <= now) return;             // already over
        // The emergency loop's visible list (push() shows it as ch.current) is display only, never schedule: treating it as
        // schedule made the executor flap between "emergency loop" and "schedule resumed" pushes.
        if (r0.origin == "emergency") return;
        // The same runlist id planned again (a fixed-source channel's endless item is the same item in every hour) is the item
        // already on air or queued: never hand the engine two entries, the second would open a second receiver and cut to it.
        if (queued.contains(r0.runlistId)) return;
        queued.insert(r0.runlistId);
        RunlistItem r = r0;
        if (first) {
            first = false;
            firstStartMs = r.plannedStartMs;
            if (r.plannedStartMs <= now) {
                // This item should be on air right now.
                if (onAir == r.runlistId && (engineState == "ON_AIR" || engineState == "PAUSED")) {
                    replace = false;                     // engine already has it; keep it, queue the rest
                    return;
                }
                const double offset = (now - r.plannedStartMs) / 1000.0;
                r.inSec += offset;
                r.durationSec = qMax(0.0, r.durationSec - offset);
                if (r.outSec > 0.0 && r.outSec < r.inSec) r.outSec = r.inSec;
                r.hard = false;                          // start immediately
                r.note = QString("join in progress +%1 s").arg(offset, 0, 'f', 1);
                if (reason) *reason = "join_in_progress";
            }
        }
        out.append(r.toEngineJson());
    };
    for (const auto& r : ch.current.items) consider(r);
    for (const auto& r : ch.next.items) consider(r);
    if (replace && airingOn && airingEndMs > now + 1000 && !queued.contains(onAir) && firstStartMs >= airingEndMs - 2000) {
        replace = false;
        if (reason) *reason = "continuation";
    }
    if (takeOn) { replace = false; if (reason) *reason = "after_take"; }
    if (replaceCurrent) *replaceCurrent = replace;
    return out;
}

QJsonArray ScheduleExecutor::plannedPlaylist(const QString& channelId, qint64 now, bool* replaceCurrent) const {
    auto it = m_channels.find(channelId);
    if (it == m_channels.end()) return {};
    return composePlaylist(it.value(), now, replaceCurrent, nullptr);
}

QString ScheduleExecutor::hashOf(const QJsonArray& a) {
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(a).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1).toHex());
}

QString ScheduleExecutor::plannedHashOf(const ChannelRun& ch) {
    // The planned list for these two hours (independent of elapsed time) decides whether a push is needed.
    QJsonArray planned;     // schedule only: the emergency loop's visible items are not schedule (see composePlaylist)
    for (const auto& r : ch.current.items) if (r.origin != "emergency") planned.append(r.toEngineJson());
    for (const auto& r : ch.next.items) if (r.origin != "emergency") planned.append(r.toEngineJson());
    // A fixed-source channel airs one endless item whose planned times move with the hour but whose playlist does not: only
    // a change of the source spec is a new playlist.
    const QJsonObject fixedSpec = ch.profile.extra.value("fixedSource").toObject();
    if (!fixedSpec.isEmpty() && !planned.isEmpty()) return hashOf(QJsonArray{fixedSpec}) + "-fixed";
    return hashOf(planned) + (planned.isEmpty() ? "-empty" : "");
}

bool ScheduleExecutor::storeChangedSince(const ChannelRun& ch) const {
    return m_store->scheduleRevision() != ch.storeRevision || m_store->settingsRevision(kExecutorSettingKeys) != ch.settingsRevision;
}

void ScheduleExecutor::push(ChannelRun& ch, qint64 now, const QString& reason) {
    bool replace = true;
    QString why = reason;
    QJsonArray items = composePlaylist(ch, now, &replace, &why);

    if (items.isEmpty() && !(ch.state.value("state").toString() == "ON_AIR" && !replace)) {
        if (reason == "nothing_due" || reason == "engine_idle" || reason == "engine_connected" || reason == "hour_changed" || reason == "schedule_changed" || reason == "operator") { /* fall through */ }
        // Nothing scheduled for the next two hours: emergency loop.
        FillSettings fs = FillSettings::fromJson(ch.profile.fill);
        const QList<MediaRecord> pool = m_store->mediaByType(fs.emergencyTypes.isEmpty() ? QStringList{"station_id","bumper","filler","promo","psa"} : fs.emergencyTypes);
        const QList<MediaRecord> loop = FillPolicy::emergencyLoop(7200.0, pool, fs, 400);
        int k = 0;
        qint64 cursor = now;
        Runlist visible; visible.channelId = ch.profile.id; visible.slotId = "emergency"; visible.hourStartMs = ch.hourStartMs; visible.hourEndMs = ch.hourStartMs + 3600000LL;
        for (const auto& m : loop) {
            RunlistItem r;
            r.runlistId = QString("emergency#%1#%2").arg(now).arg(k++);
            r.item.id = r.runlistId; r.item.mediaId = m.id; r.item.path = m.path; r.item.title = m.title; r.item.type = m.type;
            r.item.durationSec = m.durationSec; r.durationSec = m.durationSec; r.origin = "emergency";
            r.plannedStartMs = cursor; r.plannedEndMs = cursor + qint64(m.durationSec * 1000.0);
            r.note = "emergency loop";
            cursor = r.plannedEndMs;
            ch.byRunlistId.insert(r.runlistId, r);
            items.append(r.toEngineJson());
            if (visible.items.size() < 60) visible.items.append(r);     // what the operator sees (not all 400)
        }
        visible.notes << "nothing scheduled: emergency loop";
        if (!items.isEmpty() && !(ch.emergency && ch.state.value("state").toString() == "ON_AIR")) { ch.current = visible; emit runlistChanged(ch.profile.id, runlistJson(ch.profile.id)); }
        if (items.isEmpty()) {
            if (!ch.emergency) emit alert(ch.profile.id, "critical", "nothing scheduled and no emergency content: slate on air");
            ch.emergency = true;
            ch.pushedHash = "empty";
            return;
        }
        if (ch.emergency && ch.state.value("state").toString() == "ON_AIR") return;   // already looping
        replace = true;
        why = "emergency_loop";
        if (!ch.emergency) emit alert(ch.profile.id, "warning", QString("nothing scheduled: emergency loop (%1 items)").arg(items.size()));
        ch.emergency = true;
    } else {
        if (ch.emergency) emit alert(ch.profile.id, "info", "schedule resumed");
        ch.emergency = false;
    }

    m_link->pushPlaylist(ch.profile.id, items, replace);
    if (replace || ch.state.value("state").toString() != "ON_AIR") m_link->transport(ch.profile.id, "play", {});
    ch.lastPushMs = now;
    ch.lastPushReason = why;
    ch.pushedItems = items.size();
    ch.forcePush = false;
    emit pushed(ch.profile.id, items.size(), replace, why);
    qInfo().noquote() << QString("[Executor] %1 pushed %2 item(s) replace=%3 (%4)").arg(ch.profile.id).arg(items.size()).arg(replace).arg(why);
}

void ScheduleExecutor::evaluate(ChannelRun& ch, qint64 now) {
    if (!ch.connected) return;
    if (!ch.configSent) {
        m_link->sendConfig(ch.profile.id, engineConfigFor(ch.profile));
        ch.configSent = true;
    }
    if (!ch.profile.armed) return;

    const qint64 hourStart = RunlistBuilder::hourStartUtcMsForTime(now, ch.profile.tz());
    const bool hourChanged = (hourStart != ch.hourStartMs);
    const bool storeChanged = storeChangedSince(ch);
    if (hourChanged || storeChanged || ch.current.items.isEmpty() && ch.next.items.isEmpty() && ch.pushedHash.isEmpty()) {
        rebuild(ch, now, hourStart);
    }

    // The planned hash was computed by that rebuild (plannedHashOf); push() showing the emergency loop as ch.current does not
    // move it, so a channel in its emergency loop is not pushed a second time on the next tick (the startup flap).
    const QString h = ch.plannedHash;

    QString reason;
    if (ch.forcePush) reason = "operator";
    else if (ch.pushedHash.isEmpty()) reason = "engine_connected";
    else if (h != ch.pushedHash) reason = hourChanged ? "hour_changed" : "schedule_changed";
    else {
        // Engine idle while something is due: re-arm (operator stop is overridden while armed).
        const QString st = ch.state.value("state").toString();
        const bool stale = (now - ch.stateMs) > 5000;
        if (!stale && (st == "IDLE") && (now - ch.lastPushMs) > 5000) {
            bool due = false;
            for (const auto& r : ch.current.items) if (r.origin != "emergency" && r.plannedEndMs > now) { due = true; break; }
            for (const auto& r : ch.next.items) if (!due && r.origin != "emergency" && r.plannedEndMs > now) { due = true; break; }
            if (due) reason = "engine_idle";
            // Nothing due: push() falls through to the emergency loop. An emergency loop that played out (the engine went IDLE at
            // its playlist end) is pushed again; before, the channel stayed IDLE until an operator reloaded it (2026-10-26 02:20).
            else reason = ch.emergency ? "emergency_refill" : "nothing_due";
        }
        // Strict mode: correct drift beyond two seconds.
        if (reason.isEmpty() && ch.profile.strict && std::fabs(ch.driftSec) > 2.0 && st == "ON_AIR") {
            const RunlistItem* ri = lookupRunlistItem(ch.profile.id, ch.state.value("itemId").toString());
            if (ri && ri->origin != "emergency") {
                QJsonObject p; p["seconds"] = (now - ri->plannedStartMs) / 1000.0;
                m_link->transport(ch.profile.id, "seek", p);
                emit alert(ch.profile.id, "warning", QString("strict: corrected %1 s drift").arg(ch.driftSec, 0, 'f', 2));
                ch.driftSec = 0.0;
            }
        }
    }
    if (reason.isEmpty()) return;
    ch.pushedHash = h;
    push(ch, now, reason);
}

void ScheduleExecutor::tick() {
    const qint64 now = nowMs();
    for (auto it = m_channels.begin(); it != m_channels.end(); ++it) evaluate(it.value(), now);
}

// ---------------------------------------------------------------------------
// As-run
// ---------------------------------------------------------------------------

AsRunRow ScheduleExecutor::rowFromEvent(const ChannelRun& ch, const QJsonObject& ev) const {
    AsRunRow r;
    r.channelId = ch.profile.id;
    r.runlistItemId = ev.value("itemId").toString();
    r.assetId = ev.value("assetId").toString();
    r.title = ev.value("title").toString();
    r.eventType = ev.value("type").toString();
    r.actualStartMs = qint64(ev.value("utcMs").toDouble());
    r.frame = qint64(ev.value("frame").toDouble());
    r.timecode = ev.value("timecode").toString();
    r.reason = ev.value("error").toString();
    r.extra = ev;
    if (const RunlistItem* ri = lookupRunlistItem(ch.profile.id, r.runlistItemId)) {
        r.itemId = ri->item.id; r.type = ri->item.type; r.origin = ri->origin; r.slotId = ri->slotId;
        if (r.title.isEmpty()) r.title = ri->item.title;
        r.plannedStartMs = ri->plannedStartMs; r.plannedEndMs = ri->plannedEndMs;
        r.plannedDurationMs = qint64(ri->durationSec * 1000.0);
        if (r.origin == "emergency") r.plannedStartMs = 0;
    } else {
        r.origin = "manual";
        r.itemId = r.runlistItemId;
    }
    if (r.eventType == "START") {
        if (r.plannedStartMs > 0) {
            r.varianceMs = r.actualStartMs - r.plannedStartMs;
            if (qAbs(r.varianceMs) <= 1000) r.status = "ON_TIME";
            else r.status = r.varianceMs > 0 ? "LATE_START" : "EARLY_START";
        } else {
            r.status = (r.origin == "emergency" || r.origin == "fill") ? "FILL" : "MANUAL";
        }
    } else if (r.eventType == "FAILED") r.status = "FAILED";
    else if (r.eventType == "SKIP") r.status = "SKIPPED";
    else if (r.eventType == "TRUNCATED") r.status = "TRUNCATED";
    else r.status = r.eventType;
    return r;
}

void ScheduleExecutor::onEngineAsRun(const QString& channelId, const QJsonObject& event) {
    if (!m_channels.contains(channelId)) return;
    const ChannelRun& ch = m_channels[channelId];
    AsRunRow r = rowFromEvent(ch, event);
    const QString t = r.eventType;
    if (t == "START") {
        m_store->insertAsRun(r);
        if (!r.assetId.isEmpty()) m_store->recordPlay(r.assetId, r.actualStartMs);
        if (r.status == "LATE_START" && r.varianceMs > 5000) emit alert(channelId, "warning", QString("'%1' started %2 s late").arg(r.title).arg(r.varianceMs / 1000.0, 0, 'f', 1));
    } else if (t == "END" || t == "STOP" || t == "HOLD" || t == "REPLACED" || t == "TRUNCATED" || t == "SKIP") {
        if (!m_store->completeAsRun(channelId, r.runlistItemId, r.actualStartMs, t)) {
            m_store->insertAsRun(r);    // END without a START we know about: keep the evidence
        } else if (t == "SKIP" || t == "TRUNCATED") {
            m_store->insertAsRun(r);
        }
    } else {
        m_store->insertAsRun(r);        // FAILED, PAUSE, RESUME, SEEK, FILL...
        if (t == "FAILED") emit alert(channelId, "error", QString("'%1' failed: %2").arg(r.title, r.reason));
    }
    emit asRunStored(r);
}

} // namespace sched
