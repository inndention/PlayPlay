#include "fillpolicy.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace sched {

FillSettings FillSettings::fromJson(const QJsonObject& o) {
    FillSettings s;
    s.fillWithIds = o.value("fillGapsWithIds").toBool(o.value("fillWithIds").toBool(true));
    s.fillWithBumpers = o.value("fillGapsWithBumpers").toBool(o.value("fillWithBumpers").toBool(true));
    s.fillWithFiller = o.value("fillWithFiller").toBool(true);
    s.fillWithPromos = o.value("fillWithPromos").toBool(true);
    s.fillWithPsa = o.value("fillWithPsa").toBool(true);
    s.maxFillSec = o.value("maxFillSec").toDouble(o.value("maxFillDuration").toDouble(3600.0));
    s.minGapSec = o.value("minGapSec").toDouble(2.0);
    s.idFirst = o.value("idFirst").toBool(true);
    if (o.contains("emergencyTypes")) { s.emergencyTypes.clear(); for (const auto& v : o["emergencyTypes"].toArray()) s.emergencyTypes << v.toString(); }
    for (const auto& v : o.value("fillExtraTypes").toArray()) { const QString t = v.toString().trimmed().toLower(); if (!t.isEmpty() && !s.fillExtraTypes.contains(t)) s.fillExtraTypes << t; }
    for (const auto& v : o.value("emergencyPlaylist").toArray()) {
        if (v.isString()) s.emergencyPlaylist << v.toString();
        else s.emergencyPlaylist << v.toObject().value("mediaId").toString(v.toObject().value("id").toString());
    }
    return s;
}

QJsonObject FillSettings::toJson() const {
    QJsonObject o;
    o["fillGapsWithIds"] = fillWithIds; o["fillGapsWithBumpers"] = fillWithBumpers; o["fillWithFiller"] = fillWithFiller;
    o["fillWithPromos"] = fillWithPromos; o["fillWithPsa"] = fillWithPsa; o["maxFillSec"] = maxFillSec; o["minGapSec"] = minGapSec;
    o["idFirst"] = idFirst;
    QJsonArray t; for (const auto& s : emergencyTypes) t << s; o["emergencyTypes"] = t;
    QJsonArray e; for (const auto& s : emergencyPlaylist) e << s; o["emergencyPlaylist"] = e;
    QJsonArray x; for (const auto& s : fillExtraTypes) x << s; o["fillExtraTypes"] = x;
    return o;
}

QStringList FillPolicy::fillTypes(const FillSettings& s) {
    QStringList t;
    if (s.fillWithIds) t << "station_id";
    if (s.fillWithBumpers) t << "bumper";
    if (s.fillWithFiller) t << "filler";
    if (s.fillWithPromos) t << "promo";
    if (s.fillWithPsa) t << "psa";
    for (const QString& x : s.fillExtraTypes) if (!t.contains(x)) t << x;     // e.g. news / weather spots for a station with no filler clips
    return t;
}

double FillPolicy::weight(const MediaRecord& m, const FillContext& ctx, const FillSettings& s) {
    if (!m.isAvailableAt(ctx.nowMs)) return 0.0;
    if (m.durationSec <= 0.0 || m.path.isEmpty()) return 0.0;
    if (!m.allowedChannels.isEmpty() && !ctx.channelId.isEmpty() && !m.allowedChannels.contains(ctx.channelId)) return 0.0;
    if (ctx.recentlyPlayedIds.contains(m.id)) return 0.0;

    // Freshness: newer additions score up to +80, decaying over 30 days.
    double w = 20.0;
    if (m.addedAtMs > 0 && ctx.nowMs > 0) {
        const double ageDays = (ctx.nowMs - m.addedAtMs) / 86400000.0;
        w += qMax(0.0, 80.0 - ageDays * (80.0 / 30.0));
    }
    w += m.priority * 4.0;
    if (!ctx.currentSeries.isEmpty() && !m.series.isEmpty()) {
        w += (m.series.compare(ctx.currentSeries, Qt::CaseInsensitive) == 0) ? 40.0 : 0.0;
    } else if (m.type == "bumper" && m.series.isEmpty()) {
        w += 15.0;   // generic bumper
    }
    w -= m.playCount * 8.0;
    // Recency multipliers (traffic engine)
    if (m.lastPlayedMs > 0 && ctx.nowMs > 0) {
        const double h = (ctx.nowMs - m.lastPlayedMs) / 3600000.0;
        if (h < 1.0) w *= 0.3; else if (h < 4.0) w *= 0.7;
    }
    return qMax(w, 0.5);   // never fully exclude eligible content: better than dead air
}

QList<MediaRecord> FillPolicy::fillGap(double gapSec, const QList<MediaRecord>& pool, const FillContext& ctx,
                                       const FillSettings& s, bool atTopOfHour) {
    QList<MediaRecord> out;
    double remaining = qMin(gapSec, s.maxFillSec);
    if (remaining < s.minGapSec) return out;

    QRandomGenerator rng = ctx.seed ? QRandomGenerator(ctx.seed) : QRandomGenerator(*QRandomGenerator::global());
    QList<MediaRecord> candidates;
    const QStringList types = fillTypes(s);
    for (const auto& m : pool) if (types.contains(m.type)) candidates.append(m);

    auto pickWeighted = [&](const QList<MediaRecord>& from, double maxDur) -> int {
        QList<double> ws; double total = 0.0;
        for (const auto& m : from) {
            double w = (m.durationSec <= maxDur + 0.01) ? weight(m, ctx, s) : 0.0;
            ws.append(w); total += w;
        }
        if (total <= 0.0) return -1;
        double r = rng.generateDouble() * total;
        for (int i = 0; i < ws.size(); ++i) { r -= ws[i]; if (r <= 0.0 && ws[i] > 0.0) return i; }
        for (int i = ws.size() - 1; i >= 0; --i) if (ws[i] > 0.0) return i;
        return -1;
    };

    QList<MediaRecord> all = candidates;
    // Station ID first at the top of the hour.
    if (atTopOfHour && s.idFirst && s.fillWithIds) {
        QList<MediaRecord> ids; for (const auto& m : candidates) if (m.type == "station_id") ids.append(m);
        const int i = pickWeighted(ids, remaining);
        if (i >= 0) { out.append(ids[i]); remaining -= ids[i].durationSec; candidates.removeOne(ids[i]); }
    }
    int cycles = 0;
    while (remaining >= s.minGapSec) {
        if (out.size() >= 5000) break;                     // runaway guard (a gap is at most a few hours)
        if (candidates.isEmpty()) {
            // Pool exhausted: start over (never back-to-back with the last pick). A small library of short clips needs many
            // cycles: 50 used to stop a pool of 10-second clips about eight minutes into an hour-long gap, leaving dead air.
            if (all.isEmpty() || ++cycles > 5000) break;
            candidates = all;
            if (!out.isEmpty()) candidates.removeOne(out.last());
            if (candidates.isEmpty()) candidates = all;
        }
        const int i = pickWeighted(candidates, remaining);
        if (i < 0) {
            // Nothing short enough is left in this cycle; try the full pool once more.
            bool anyFits = false; for (const auto& m : all) if (m.durationSec <= remaining + 0.01) { anyFits = true; break; }
            if (!anyFits) break;
            candidates.clear(); continue;
        }
        out.append(candidates[i]);
        remaining -= candidates[i].durationSec;
        candidates.removeAt(i);
    }
    return out;
}

QList<MediaRecord> FillPolicy::emergencyLoop(double totalSec, const QList<MediaRecord>& pool, const FillSettings& s, int maxItems) {
    QList<MediaRecord> ordered;
    if (!s.emergencyPlaylist.isEmpty()) {
        for (const auto& id : s.emergencyPlaylist)
            for (const auto& m : pool) if (m.id == id && m.durationSec > 0.0) { ordered.append(m); break; }
    }
    if (ordered.isEmpty()) {
        for (const auto& t : s.emergencyTypes)
            for (const auto& m : pool) if (m.type == t && m.durationSec > 0.0 && !m.archived) ordered.append(m);
    }
    QList<MediaRecord> out;
    if (ordered.isEmpty()) return out;
    double acc = 0.0; int i = 0;
    while (acc < totalSec && out.size() < maxItems) {
        const MediaRecord& m = ordered[i % ordered.size()];
        out.append(m); acc += m.durationSec; ++i;
    }
    return out;
}

} // namespace sched
