#include "schedulevalidator.h"
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

namespace sched {

QJsonObject ValidationResult::toJson() const {
    QJsonObject o;
    o["ok"] = ok; o["slotId"] = slotId; o["channelId"] = channelId;
    o["totalSeconds"] = totalSeconds; o["gapSeconds"] = gapSeconds; o["overrunSeconds"] = overrunSeconds;
    QJsonArray a; int errors = 0, warnings = 0;
    for (const auto& i : issues) { a.append(i.toJson()); if (i.severity == "error") ++errors; else if (i.severity == "warning") ++warnings; }
    o["issues"] = a; o["errors"] = errors; o["warnings"] = warnings;
    return o;
}

ValidationResult ScheduleValidator::validate(const HourSlot& slot, qint64 hourStartMs, const ValidatorOptions& opt) {
    ValidationResult res;
    res.slotId = slot.id;
    res.channelId = slot.channelId;
    const qint64 nowMs = opt.build.nowMs ? opt.build.nowMs : QDateTime::currentMSecsSinceEpoch();
    auto add = [&](const QString& code, const QString& sev, const QString& itemId, const QString& msg) {
        res.issues.append({code, sev, itemId, msg});
        if (sev == "error") res.ok = false;
    };

    bool hasLegalId = false;
    double rawTotal = 0.0;
    qint64 lastHardMs = -1;
    for (const SlotItem& raw : slot.items) {
        SlotItem it = raw;
        if (it.type == "station_id") hasLegalId = true;
        if (it.type == "cg") {
            bool found = it.path.isEmpty();
            for (const auto& dir : opt.cgTemplateDir) if (QFileInfo::exists(QDir(dir).filePath(it.path))) found = true;
            if (!it.path.isEmpty() && QFileInfo::exists(it.path)) found = true;
            if (!found) add("missing_cg_template", "error", it.id, QString("CG template '%1' not found").arg(it.path));
            continue;
        }
        if (!it.isPlayable()) continue;
        QString why;
        if (!RunlistBuilder::resolveItem(it, opt.media, nowMs, &why)) {
            QString code = "missing_media";
            if (why.contains("not available")) code = why.contains("window") ? "not_yet_available" : "expired";
            if (why.contains("no file path")) code = "missing_path";
            if (why.contains("live source")) code = "missing_source";
            if (!raw.smartConfig.isEmpty()) code = "unresolved_smart_slot";
            add(code, "error", it.id, why);
            continue;
        }
        if (it.type != "live" && opt.checkFilesExist && !it.path.startsWith("http") && !it.path.contains("://") && !QFileInfo::exists(it.path)) {
            add("missing_file", "error", it.id, QString("file not found: %1").arg(it.path));
        }
        if (it.type != "live" && it.effectiveDurationSec() <= 0.0 && !it.isFillUntil()) {
            add("zero_duration", "error", it.id, QString("'%1' has no duration").arg(it.title));
        }
        if (it.isHardStart()) {
            const qint64 anchor = it.fixedUtcMs > 0 ? it.fixedUtcMs : hourStartMs + qint64(it.fixedOffsetSec * 1000.0);
            if (anchor < hourStartMs || anchor >= hourStartMs + 3600000LL) add("timing_conflict", "warning", it.id, QString("'%1' hard start is outside this hour").arg(it.title));
            else if (lastHardMs >= 0 && anchor <= lastHardMs) add("timing_conflict", "error", it.id, QString("'%1' hard start is not after the previous hard start").arg(it.title));
            lastHardMs = anchor;
        }
        rawTotal += it.effectiveDurationSec();
    }

    // Layout with fill disabled to measure the slot itself.
    BuildOptions bo = opt.build; bo.fillGaps = false; bo.nowMs = nowMs;
    const Runlist rl = RunlistBuilder::build(slot, hourStartMs, opt.media, bo);
    res.totalSeconds = rl.scheduledSec;
    res.gapSeconds = rl.gapSec;
    for (const auto& r : rl.items) if (r.truncated) add("item_overruns_hour", "warning", r.item.id, r.note);
    for (const auto& n : rl.notes) if (n.contains("do not fit") || n.startsWith("dropped")) add("overrun", "warning", "", n);
    if (rawTotal > 3600.0 + 0.5) { res.overrunSeconds = rawTotal - 3600.0; add("overfill", "warning", "", QString("%1 s scheduled; %2 s will be cut").arg(rawTotal, 0, 'f', 1).arg(res.overrunSeconds, 0, 'f', 1)); }
    if (!slot.isLooping && !opt.build.continuesIntoNextHour && res.gapSeconds > opt.underfillToleranceSec) {
        add("underfill", "warning", "", QString("%1 s unfilled (fill policy will cover it at playout)").arg(res.gapSeconds, 0, 'f', 1));
        add("gap", "info", "", QString("gaps total %1 s").arg(res.gapSeconds, 0, 'f', 1));
    }
    if (opt.requireLegalId && !hasLegalId) add("missing_legal_id", "warning", "", "no station ID in this hour");
    if (slot.items.isEmpty()) add("gap", "warning", "", "hour is empty; emergency loop will play");
    return res;
}

} // namespace sched
