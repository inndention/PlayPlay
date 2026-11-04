#ifndef PLAYPLAY_FLIGHTPLAN_H
#define PLAYPLAY_FLIGHTPLAN_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTimeZone>
#include "schedulemodel.h"
#include "runlistbuilder.h"

namespace sched {

/**
 * FlightPlan — the operator's "broadcast flight plan" over the runlists the executor has built (current + next hour):
 *
 *  - Now / Next / Later, with remaining time.
 *  - Clock anchors and TIME DEBT: between two anchors (hard starts, hour ends) the soft events either fit, run over
 *    (the builder will truncate or drop something) or leave a gap (fill covers it). "42 s over before 6:00 PM".
 *  - CORRECTIONS for an overrun: drop optional events, trim events that may be trimmed, swap a filler for a shorter
 *    one; the cheapest combination that covers the debt is proposed ("best plan"). For a gap: eligible clips that fit.
 *    Mandatory events (ads, station IDs, locked items, anything marked mandatory) are never touched, and nothing
 *    inside the protected window in front of the on-air item can be edited.
 *  - GHOST PREVIEW: corrections are ops ({op: remove|trim|replace|insert, ...}); applyOps() edits a copy of the stored
 *    hour, the caller rebuilds it, and diff() reports what moved, was dropped or replaced.
 *  - READINESS per event: library record, approval, file on disk and its size against the record, availability window,
 *    channel scope, duration.
 *  - WHY: the reasons an event sits where it does.
 *
 * Pure logic over Runlist / MediaLookup (no store, no network), so it is unit-tested directly.
 */
struct FlightOptions {
    QString channelId;
    qint64 nowMs = 0;
    QTimeZone tz;
    double protectSec = 90.0;            // editable boundary: the on-air event and this long after now are protected
    double readinessHorizonSec = 7200.0; // only events starting within this long are checked for readiness
    int readinessMaxItems = 200;
    bool checkFiles = true;
    MediaLookup media;
};

class FlightPlan {
public:
    /// Full picture for the Operate / Plan views: {now, next, later[], anchors[], intervals[], protectedUntilMs, summary}.
    static QJsonObject analyze(const QList<Runlist>& lists, const FlightOptions& opt);

    /// Corrections for one interval of analyze() (index into "intervals"), as {interval, options[], best{ops, savingSec, remainingSec}}
    /// for an overrun, or {interval, options[] (clips that fit), best{ops}} for a gap.
    static QJsonObject corrections(const QList<Runlist>& lists, const FlightOptions& opt, int intervalIndex);

    /// Apply correction ops to the stored hour `slot` (the runlist `rl` is that hour's build, used to locate items, to check
    /// the protected window and to turn a query-block pick into a concrete item). Ops that would touch a protected or
    /// mandatory event are refused and reported in `errors`. Returns true when at least one op was applied.
    static bool applyOps(HourSlot& slot, const QJsonArray& ops, const Runlist& rl, const FlightOptions& opt, QStringList* log, QStringList* errors);

    /// What changed between two builds of the same hours: {moved, replaced, removed, added, gapsBefore, gapsAfter, details[]}.
    static QJsonObject diff(const QList<Runlist>& before, const QList<Runlist>& after);

    /// Why one runlist item sits where it does (strings, most important first).
    static QStringList explain(const RunlistItem& r, const FlightOptions& opt);

    /// Readiness of one item: {state ok|warn|fail, checks{media,approval,file,window,channel,duration}: {state, message}, problems[]}.
    static QJsonObject readiness(const RunlistItem& r, const FlightOptions& opt, bool checkFile);

    /// "42 s", "3 m 05 s", "1 h 02 m".
    static QString fmtDuration(double sec);
    /// A droppable / protected classification, shared by corrections and applyOps.
    static bool isMandatory(const SlotItem& it);
    static bool isOptional(const SlotItem& it);
};

} // namespace sched

#endif // PLAYPLAY_FLIGHTPLAN_H
