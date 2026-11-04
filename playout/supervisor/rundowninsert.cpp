#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"
#include <QDateTime>
#include <QJsonArray>
#include <QUuid>

namespace {
void install(const laneext::Context& ctx) {
    if (!ctx.rest || !ctx.store || !ctx.runlist || !ctx.engineState) return;
    ctx.rest->registerRoute("POST", "/api/v1/channels/{id}/rundown/insert", [ctx](const QJsonObject& body, const QHash<QString,QString>& q) {
        auto error = [](int status, const QString& s) { return QJsonObject{{"status", status}, {"error", s}}; };
        const QString ch = q["id"], anchorId = body.value("anchorId").toString(), side = body.value("side").toString();
        if (anchorId.isEmpty() || (side != "before" && side != "after") || !body.value("items").isArray()) return error(422, "Choose a rundown row and before/after position");
        const auto additions = body.value("items").toArray(); if (additions.isEmpty() || additions.size() > 200) return error(422, "Drop 1–200 playable items");
        bool found = false; const auto profile = ctx.store->channel(ch, &found); if (!found) return error(404, "No such channel");
        const auto run = ctx.runlist(ch), state = ctx.engineState(ch); const QString onAir = state.value("itemId").toString();
        QJsonObject anchor, hour; int anchorIndex = -1, onAirIndex = -1; bool current = false;
        for (const char* which : {"current", "next"}) {
            const auto h = run.value(which).toObject(); const auto entries = h.value("items").toArray();
            for (int i = 0; i < entries.size(); ++i) {
                const auto entry = entries[i].toObject();
                if (entry.value("id").toString() == onAir && QString::fromLatin1(which) == "current") onAirIndex = i;
                if (entry.value("id").toString() == anchorId) { anchor = entry; hour = h; anchorIndex = i; current = QString::fromLatin1(which) == "current"; }
            }
        }
        if (anchor.isEmpty()) return error(409, "The rundown changed; drop on a current row again");
        const qint64 now = QDateTime::currentMSecsSinceEpoch(), end = qint64(anchor.value("plannedEndMs").toDouble()), start = qint64(anchor.value("plannedStartMs").toDouble());
        if ((end > 0 && end <= now && anchorId != onAir) || (current && onAirIndex >= 0 && (anchorIndex < onAirIndex || (anchorIndex == onAirIndex && side == "before"))) ||
            (current && onAirIndex < 0 && start > 0 && start <= now && side == "before")) return error(409, "Cannot insert into the already-played or on-air part of the rundown");
        if (anchor.value("queryFill").toBool() || anchor.value("itemId").toString().isEmpty()) return error(409, "This row belongs to a dynamic block; edit the hour to place an item there");
        const qint64 hourStart = qint64(hour.value("hourStartMs").toDouble()); if (hourStart <= 0) return error(409, "This rundown has no editable hour");
        const QDateTime local = QDateTime::fromMSecsSinceEpoch(hourStart, profile.tz()); const QString dated = sched::HourSlot::dateId(local.date(), local.time().hour());
        auto slot = ctx.store->effectiveSlot(ch, local.date(), local.time().hour());
        int position = -1;
        for (int i=0; i<slot.items.size(); ++i) if (slot.items[i].id == anchor.value("itemId").toString()) { if (position >= 0) return error(409, "Ambiguous source item; edit the hour directly"); position = i; }
        if (position < 0) return error(409, "The schedule changed; reload the rundown and try again");
        if (side == "after") ++position;
        int count = 0;
        for (const auto& v : additions) {
            if (!v.isObject()) return error(422, "Every dropped item must be a media item");
            auto item = sched::SlotItem::fromJson(v.toObject());
            if (item.path.isEmpty() && item.mediaId.isEmpty() && item.liveSource.isEmpty()) return error(422, "Dropped item has no media source");
            if (item.durationSec <= 0 && item.liveSource.isEmpty()) return error(422, "Dropped media needs a known duration");
            item.id = "drop-" + QUuid::createUuid().toString(QUuid::Id128); slot.items.insert(position++, item); ++count;
        }
        slot.id = dated; slot.channelId = ch; slot.date = local.date(); slot.hour = local.time().hour(); slot.dayOfWeek = -1;
        if (!ctx.store->saveSlot(slot)) return error(500, "Could not save the dated hour");
        if (ctx.reloadChannel) ctx.reloadChannel(ch);
        return QJsonObject{{"ok",true}, {"slotId",dated}, {"inserted",count}, {"side",side}};
    });
}
static laneext::Registrar registration("rundown-insert", 120, install);
}
