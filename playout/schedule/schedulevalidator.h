#ifndef PLAYPLAY_SCHEDULEVALIDATOR_H
#define PLAYPLAY_SCHEDULEVALIDATOR_H

#include <QJsonObject>
#include <QJsonArray>
#include "schedulemodel.h"
#include "runlistbuilder.h"

namespace sched {

/**
 * ScheduleValidator — pre-air checks for one hour slot. Codes are the union of
 * the two reference validators:
 *   missing_media, missing_path, missing_file, missing_source, expired,
 *   not_yet_available, channel_exclusive, overrun, item_overruns_hour, gap,
 *   underfill, overfill, timing_conflict, missing_legal_id, missing_cg_template,
 *   unresolved_smart_slot, zero_duration
 */
struct ValidationIssue {
    QString code;
    QString severity;    // error|warning|info
    QString itemId;
    QString message;
    QJsonObject toJson() const {
        QJsonObject o; o["code"] = code; o["severity"] = severity; o["itemId"] = itemId; o["message"] = message; return o;
    }
};

struct ValidationResult {
    bool ok = true;              // no errors
    QString slotId;
    QString channelId;
    double totalSeconds = 0.0;   // planned on-air seconds (after layout)
    double gapSeconds = 0.0;
    double overrunSeconds = 0.0;
    QList<ValidationIssue> issues;
    QJsonObject toJson() const;
};

struct ValidatorOptions {
    bool requireLegalId = false;     // station ID somewhere in the hour
    double underfillToleranceSec = 30.0;
    bool checkFilesExist = true;
    QStringList cgTemplateDir;       // where cg templates live, for missing_cg_template
    MediaLookup media;
    BuildOptions build;
};

class ScheduleValidator {
public:
    static ValidationResult validate(const HourSlot& slot, qint64 hourStartMs, const ValidatorOptions& opt);
};

} // namespace sched

#endif // PLAYPLAY_SCHEDULEVALIDATOR_H
