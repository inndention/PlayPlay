#pragma once
// MasterPlay import plan (the lane design notes §6.2 events -> hour slots, §6.4 folders, §6.6 breaks, §6.7 acts,
// §6.8 OSD, §6.9 other instructions, §6.10 round-trip payload, §9.2 importer API, §13 deterministic ids; slices MP-08a
// and MP-08b): folds the event-anchored day timelines of a playlist (mptimeline.h) into PlayPlay hour slots
// (sched::HourSlot), as a dry-run plan; apply / undo / history (MP-09) write it, see "Apply / undo / history" below.
//
// Placement list. Every window of every day (expanded sub-playlists included) becomes placements, one per item line
// (a folder line one per pick, a looping event one per line and unrolled copy):
//   media        a media line: resolved through the library (mpresolver.h), or, with missing = keepPath / ingest, the
//                mapped path with a duration from the forced bracket or LengthStr / ExpectedLengthStr. An unresolved
//                line with missing = drop, or a line with no duration, is dropped and listed. actinout=N (acts, design
//                §6.7): the library record's segments[N] gives segmentId and the planned length; without segments the
//                parts of one file in one event pass collapse into one whole-programme item at the first part's place
//                (the later parts are dropped "actCollapsed", MP026 info), or with collapseUnsegmentedActs false every
//                part airs whole with segmentLabel "part N+1" (MP027 error: it would air the programme N times).
//   folder       !InsertDirectory / !InsertTopinDirectory as folderBlock items (design §6.4): folder = the mapped
//                folder, recursive = SubDirs > 0 (2, one file per subfolder, is MP021), MaxAge hours -> maxAgeDays
//                (added date; MasterPlay reads the file or filename time), a Name*.ext mask -> filename starts_with /
//                ends_with rules (a '?' or a middle '*' is MP021). LoadingMode: 0 one block, order name, targetSec =
//                the folder's playable length; 1 the same, order random / uniform; 2 ToSelect picks, order name; 3 (and
//                the older SelectRandom=N) ToSelect picks, order random / freshness (selectRandomOrder "even": strict
//                rotation); 4 / 5 one pick by name (MP021); 6 the whole folder by name (MP021); 7 one block whose
//                targetSec is the time left until the next event; 8 MP022, dropped. A pick is planned at the folder's
//                average length. A %search term% (MP020), mode 8 and a folder with nothing playable (MP019, in the
//                folder report with its scan / watch actions) are dropped. Other options in effect are MP021.
//   live         mp://<source> and URL media lines, !Video <source> (a bare !Video takes networkInput; "Set only" is
//                MP022) and !LiveStream Path= as type "live" items (design §6.9): the source goes through inputMap
//                (a URL / ndi:// path string, "inN" read from the store's ui.inputMap slots, or a liveSource object); a
//                URL or a !LiveStream path is used as is. An unmapped source is dropped "liveUnmapped" (MP006). The
//                length is the line's bracket / LengthStr, else the time left until the next event.
//   structural   play-mode markers (!Normal !Repeat !Fillup !WaitForGPI !Package), !InsertPlaylist (its target's lines
//                follow it) and !ChangePlaylist: nothing to air; kept as provenance on the next item.
//   break        !Break opens a pod: the timed lines after it up to the next !Break or the end of the event pass
//                (design §6.6; PlayPlay has no GPI). breaks "asScheduled": pod items carry extra.masterplay.break (the #=
//                number, else the pod's ordinal); a pod with schedtime= starts with an exact hard start at that time
//                of the event's day (the next day's when it lies more than 12 h before the event's start; still before
//                the content started: ignored, MP025; in an unrolled loop copy: ignored, the copies follow the content);
//                "skip": pod items are dropped "breakSkipped"; "live": a
//                break-mode event (PlayMode 4) becomes one live item from networkInput for its whole window, its lines
//                listed and not placed (!Break lines in other events follow asScheduled). Every event with pods or
//                break mode gets MP025.
//   osd          !ShowLogo / !ShowCrawl(FromFile) / !ShowText(FromFile) / !ShowClock / !RealTimeClock open a layer by role
//                (logo Layer 1 or none: bug, other layers: secondary; crawl: ticker; text: lower_third; clock: clock)
//                that stays up until !OSD off, a replacing instruction of the same role, or the end of the plan (design
//                §2.7.6, §6.8; OSDClearEOForExactEvent is 0 at the plant). The importer walks the laid items in time
//                order and puts every open layer on every item of its span. osd "auto" / "cues": item cues
//                (cgOverlays: {id "mp-<role>-<hash>", filePath | template "ticker" | "lower_third", role, params
//                {placement | text | title}, startOffset 0}), managed by OverlayManager. "hooks": cgOnStart show
//                commands on the span's first item and on its first item in every later hour, cgOnEnd hide on its last,
//                written as compact JSON strings: the design's command objects. NOTE the engine runs only identity-name
//                hooks today (SlotItem::cgOnStart is a string list and runCgHooks reads a string as an identity graphic
//                name), so these show nothing on air until command-object hooks land (MP-17); that is why "auto" uses
//                cues for multi-item spans too, not the design's hooks, and the plan notes it in hooks mode. Clocks are
//                always the identity hook "clock" (cgOnStart / cgOnEnd; style from the channel identity, MP006). The
//                placement is the kind's default (osd::OsdConfig::placement, which does not use XPos / YPos: their unit
//                is design open question 4), so the .osd cfg= file is not read (the line stays in the provenance).
//                *FromFile text is read at import through the ReadFn (a snapshot, MP028; unreadable: MP006, not
//                shown). "off": the lines stay provenance. CountDown / CountUp / !Overlay / !ShowCGC are MP022.
//   info         !Info: its text (else Name=) becomes the note of the next placed item.
//   unsupported  device, legacy and unknown instructions, and the content instructions with no PlayPlay form
//                (!InsertTextDirectory, !InsertFile, !ReadPlaylist, !Switcher): kept as provenance on the next item and
//                listed in Plan::unsupported with MP022 (unknown names: MP011 from the parser).
// Provenance: lines that place nothing ride on the next placed item (extra.masterplay.instructions), else on the
// window's last item ("after"), else on the event's unplaced lines (showBlock.masterplay).
//
// Repeat (design §6.2, §2.7.3). A window whose content loops (Window::loopFrom; after a followed !ChangePlaylist the
// target's loop, Item::targetLoopFrom; a loop the change line sits in never repeats) lays its lines once, then unrolls
// the loop part (item ids carry the copy number) until the next event's start (the next window that is not the same
// event going on past midnight; the end of the laid days). repeat "loop" (default): an hour that lies wholly inside the
// unrolled part, when one pass of the loop is at most an hour, is written as the loop's items once with isLooping:
// RunlistBuilder starts the loop over at the top of the hour (after a carried item) and repeats it to the hour end,
// cutting the last copy, and the plan lays it the same way (the replays are not placements; hours[].loop). "unroll":
// copies everywhere. A carry window laid from before its start restarts the loop at the carry's start. maxUnrollItems
// bounds one window's unrolled lines (a plan note when reached); a loop with nothing that takes air time is noted, not
// unrolled. A next event that places nothing still ends the loop at its start (the gap is left to the fill policy).
//
// Layout. The days of the import are laid out as one continuous timeline (ms from the first day's midnight), so a
// programme crossing midnight is one item: a weekly grid lays the week twice (Sun..Sat Sun..Sat) and keeps the second
// week, whose Sunday then continues the first week's Saturday exactly as the cyclic week does; a date range starts at
// its first date. A day's carry window that continues the previous day's last window is that window (nothing new
// starts); any other carry window (the range start, a followed !ChangePlaylist that ended the day before) is laid from
// its event's start (Window::offsetSec before midnight) and the item playing at midnight joins in progress (inPoint).
// Each window lays its whole content from its actual start; the next event's start then trims what is still playing:
//   exact (EventType 0)              items starting at or after the anchor never air (cutByEvent); the item playing
//                                    is cut at the anchor; the event starts on time.
//   floating skip (EventType 1)      items starting at or after the anchor are skipped (skippedByFloating); the item
//                                    playing finishes, by at most floatingSkipMaxLateSec (else it is cut there); the
//                                    event starts when it ends. floatingSkipMaxLateSec <= 0 makes skip behave as exact.
//   floating play all (EventType 2)  everything still to play of the earlier content plays first; the event starts
//                                    when it ends (design §2.7.2; no limit).
// An event that places nothing (every line dropped, kept or consumed) trims nothing: it writes no hard start, so on
// air the earlier content goes on, and the plan lays it that way (an MP016 note names the event). The provenance lines
// of a laid item a later start drops move to the next placed item (extra.masterplay.instructions, "carriedFrom").
// hourBoundary "cut" also ends an item at the top of the hour it crosses (the rest of the content moves up).
//
// Hour slots (one per hour of the output days; id "<dow>-<hour>" or "<date>-<hour>"):
//   - items: the laid items starting in that hour, in order (soft_start unless below). randomizeAds false,
//     keepProgramOrder true; blockType "show" with showBlock.title = the Name= of the event holding most of the hour
//     (ties: the later one; none when that event has no Name=); showBlock.masterplay {file, sha256, events[]} is the
//     provenance (the events in force during the hour, the lines of an event with nothing placed as "unplaced"; such an
//     event is marked "empty" and never gives the title).
//   - an event's first placed item is timingMode hard_start, fixedOffsetSec = its second in the hour, with
//     extra.conflictPolicy "truncate" (exact) or "finish_previous" (floating; extra.maxLateSec for skip), the way
//     RunlistBuilder lays a hard start. A floating event that starts late is laid by RunlistBuilder itself when the item
//     it waits for is in the same hour slot as both its anchor and its start ("native"); a late start capped by
//     maxLateSec in another slot is a truncate hard start at the planned second ("capped"); any other late start (the
//     item it waits for came from an earlier hour, or the start moved past its anchor's hour) is soft_start, so it
//     follows the carried programme the way the executor measures it ("soft"). extra.masterplay.anchor records it.
//   - hourBoundary "continue" (default): an item running past the top of the hour stays in its slot and every hour it
//     reaches gets mergedEvent {isContinuation, rootSlotId (the slot it started in), carrySec (its part in that hour,
//     at most 3600), title, source "masterplay"}, so RunlistBuilder neither truncates it nor lays the next hour on top
//     of it. An exact event inside that hour still cuts it there (its hard start wins). A continuation whose root slot is
//     not written by this import (filters, or a date range's first day) is a join instead: the item is repeated at the
//     top of the hour with inPointSec advanced by what already aired (id suffix "~j").
//   - Hours no window covers are left out (the grid keeps its old content; a date range gets no override) unless
//     clearUncovered; covered hours with nothing placed are written empty (design §6.2 afterNormal "fill": the executor
//     fills a gap inside an hour from the fill policy and plays its emergency loop through hours with nothing queued).
//     weekdays / hours filters drop slots from the output (joins keep what remains consistent).
//
// Validation: every written slot goes through ScheduleValidator::validate (fill off, the continuation flags of its
// neighbours, file checks only with validateFiles) at its hour in the channel's zone: a grid slot at its next
// occurrence on or after nowMs. Its "overfill ... will be cut" (when the next hour continues the slot) and "hour is
// empty" (a carried programme covers it) become info, reworded. A continuation hour with no item followed by another
// hour with none is a plan note: the executor keeps a carried programme on air only while something is queued behind it.
// onAirSlots lists the written slots whose hour overlaps [nowMs, nowMs + 2 h].
//
// Item ids are deterministic: "mp-" + 16 hex of sha1(item file | window file#line | window day | window start | item
// index in the window | item line | copy) (design §13; copy is the loop pass, 0 for the first), plus "~p<k>" for the
// k-th pick (k >= 1) of a folder line and "~j" for a join. The same document, options, library and nowMs give the same
// plan (the JSON compares equal).
//
// Diagnostics: the root document's parser diagnostics, the timeline notes (MP015 MP017 MP023 MP024), the resolver's
// (MP005 MP018 MP019) per file, and the importer's MP006 MP020 MP021 MP022 MP025 MP026 MP027 MP028 (above) and MP016
// (content far from its window: a window whose placed content is more than max(10 min, 25 %) shorter, or more than
// max(1 min, 5 %) longer, than the time until the next event; skipped for repeat / break-mode events, looping windows
// and windows with unsupported timed lines or lines dropped for want of media). Only lines of the output (grid: the
// second laid week) are reported. Each is a tl::Note (de-duplicated across days).
//
// THREADING: plan() reads sub-playlists and *FromFile texts through the ReadFn and may stat / hash files through the
// Resolver (folder coverage included); run it in a worker (MP-11). It does not touch the ScheduleStore except for the
// channel's time zone and the ui.inputMap setting ("inN" live sources).
#include "mpresolver.h"
#include "mptimeline.h"
#include "runlistbuilder.h"
#include "schedulemodel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <functional>

namespace sched { class ScheduleStore; }

namespace mp::imp {

// ---------------------------------------------------------------------------------------------------------------------
// Options

struct Options {
    QString channelId;                     // the slots' channelId (may be empty for a dry run)
    QString mode = QStringLiteral("grid"); // grid (the weekly template "D-H") | dates (dated overrides "YYYY-MM-DD-H")
    QDate from, to;                        // dates mode: the range (at most 366 days)
    QList<int> weekdays;                   // output filter, 0 Sunday .. 6 (a date's weekday in dates mode); empty = all
    QList<int> hours;                      // output filter, 0..23; empty = all
    QString missing = QStringLiteral("drop");           // drop | keepPath | ingest (keepPath + listed for MP-11 to ingest)
    QString hourBoundary = QStringLiteral("continue");  // continue | cut
    double floatingSkipMaxLateSec = 300;   // EventType=1: how late the event may start (<= 0: skip behaves as exact)
    bool clearUncovered = false;           // write empty slots for hours no event covers
    double minItemSec = 0.5;               // shorter remains are dropped (RunlistBuilder's minItemSec)
    QString timeZone;                      // hour starts for validation / on-air; "" = the channel's (store), else UTC
    qint64 nowMs = 0;                      // 0 = the clock when plan() runs
    bool validate = true;                  // ScheduleValidator per written slot
    bool validateFiles = false;            // ... with its file-exists checks (file I/O)
    QString repeat = QStringLiteral("loop");            // loop (isLooping hours inside a loop) | unroll (copies only)
    int maxUnrollItems = 20000;            // unrolled lines per window (guard); 100..500000
    QString osd = QStringLiteral("auto");               // auto | cues | hooks | off (see the header comment)
    QString breaks = QStringLiteral("asScheduled");     // asScheduled | live | skip
    QString networkInput;                  // breaks live / a bare !Video: an inputMap key, "inN", or a URL / ndi:// path
    QJsonObject inputMap;                  // MasterPlay source / router input -> path string, "inN", or a liveSource {}
    bool collapseUnsegmentedActs = true;   // acts without library segments: one whole item (MP026) or every part (MP027)
    QString selectRandomOrder = QStringLiteral("random"); // LoadingMode 3 picks: random (freshness) | even (rotation)
    tl::Options timeline;                  // maxDepth, subPlaylists, followChangePlaylist, samePriority, dayChange,
                                           // timeShiftMin, fps, legacyDateOrder (tl::Options::fromJson keys)

    /// {channel|channelId, mode, from, to ("YYYY-MM-DD"), weekdays[], hours[], missing, hourBoundary,
    ///  floatingSkipMaxLateSec, clearUncovered, minItemSec, timeZone, nowMs, validate, validateFiles, repeat,
    ///  maxUnrollItems, osd, breaks, networkInput, inputMap{}, collapseUnsegmentedActs, selectRandomOrder} plus the
    ///  tl::Options keys at the same level. Absent keys keep defaults; bad values keep the default and are reported in
    ///  errors.
    static Options fromJson(const QJsonObject& o, QStringList* errors = nullptr);
    QJsonObject toJson() const;
    /// Problems that make a plan impossible ("" list = fine): unknown mode / policy names, dates without a range, ...
    QStringList check() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Results

/// One line of a window's content, as laid out (the placement list, design §6.2).
struct Placement {
    QString kind;                  // media | folder | live | structural | break | osd | info | unsupported
    QString state;                 // placed | dropped | consumed (structural) | mapped (break / osd / info lines applied)
                                   // | kept (provenance only: unsupported, osd off, a break-live event's lines) |
                                   // filtered (placed, but its hour is left out by the weekdays / hours filters)
    QString reason;                // dropped: missing | noDuration | cutByEvent | skippedByFloating | tooShort |
                                   // beforeWindow | afterRange | wrap (grid: the week start airs it) | actCollapsed |
                                   // emptyFolder | searchTerm | unsupported | liveUnmapped | noTime | breakSkipped;
                                   // kept: unsupported | osdOff | osdUnread | breakLive
    int day = 0;                   // index of the window's day in the laid sequence
    int window = 0, item = 0;      // indexes into that day's windows and the window's items
    QString file;                  // the line's playlist ("" for pasted text)
    int line = 0;
    QString name;                  // instructions: the catalog name (or as written)
    double durationSec = -1;       // planned on-air seconds (media / folder / live; -1 unknown)
    qint64 startMs = -1, endMs = -1;   // planned, ms from the first laid day's midnight (laid items only)
    QString slotId;                // where it was placed ("" when not written)
    int copy = 0;                  // loop pass (0 the first)
    int pick = 0;                  // folder lines: the pick (0 the first)
    bool output = false;           // inside the output range (grid: the second week)
    /// {kind, state, reason?, file?, line, name?, durationSec?, startMs?, endMs?, slotId?, copy?, pick?}
    QJsonObject toJson() const;
};

struct Plan {
    Options options;
    QString file, sha256;          // the root playlist as given and the sha256 of its bytes
    QList<sched::HourSlot> hourSlots; // written slots, in time order ("slots" is a Qt keyword macro)
    QList<qint64> hourStartMs;     // parallel to slots: the hour's UTC start used for validation / preview
    QList<int> nextIndex;          // parallel to slots: the slot of the following hour (grid: cyclic), -1 when not written
    QList<Placement> placements;   // every line of every laid window (grid: both weeks), in laid order
    QJsonArray validation;         // ScheduleValidator results of the written slots ({slotId, ok, issues[] ...})
    QJsonArray missing, ambiguous; // the resolver report of the output's media lines (design §6.3)
    QJsonArray ingest;             // missing = ingest: [{path, mapped, exists}] for MP-11 to enqueue
    QJsonArray folders;            // res::Report::folders() of the output's folder lines (coverage, scan / watch actions)
    /// The unsupported instruction table: [{name, catalog (the catalog's support level: unsupported | partial | mapped |
    ///  structural, or unknown), code (MP022 | MP011 | MP006), count, why, lines[{file?, line}]}], one row per
    ///  instruction name and reason left as provenance (osd off and a break-live event's lines are not listed).
    QJsonArray unsupported;
    QJsonArray overlays;           // OSD spans: [{kind, role, mode (cue | hook | identity), id, file?, line, items, firstSlot,
                                   //  lastSlot, end (osdOff | replaced | end), text? | filePath?}]
    QJsonArray pods;               // break pods: [{file?, line, pod, mode, schedTime?, items, day, at?, slotId?}]
    QJsonArray acts;               // actinout files: [{file?, title, mediaId?, path, mode (segments | collapsed | parts), lines[]}]
    QJsonArray dropped;            // output media / folder / live lines that do not air: [{file?, line, reason, path, title?,
                                   //  durationSec?, at, day}]
    QJsonArray hours;              // per output hour: {id, written, covered, items, hard, plannedSec, continuation?, join?, loop?}
    QJsonArray diagnostics;        // tl::Note::toJson() rows (parser, timeline, resolver, importer stages)
    QJsonArray sources;            // tl::Builder::sourcesJson()
    QStringList onAirSlots;        // written slot ids whose hour overlaps [nowMs, nowMs + 2 h]
    QStringList notes;             // human-readable decisions (item-less continuations, seams, spills past the range)
    QStringList errors;            // Options::check() problems: nothing was planned
    QJsonObject stats;

    bool ok() const { return errors.isEmpty(); }
    int indexOf(const QString& slotId) const;
    const sched::HourSlot* slot(const QString& slotId) const;
    /// What the executor passes RunlistBuilder for slot i (scheduleexecutor.cpp: continuation flags, the carried
    /// start), fill off, nowMs: lays a planned slot out the way it would air.
    sched::BuildOptions buildOptions(int i) const;
    /// {file, sha256, mode, channel, options, slots{id: HourSlot} (withSlots), slotOrder[] (withSlots), hours[], validation[], missing[],
    ///  ambiguous[], ingest[], folders[], unsupported[], overlays[], pods[], acts[], dropped[], diagnostics[], onAirSlots[],
    ///  notes[], errors[], sources[], stats, placements[] (withPlacements: the output's)}
    QJsonObject toJson(bool withSlots = true, bool withPlacements = false) const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Apply / undo / history (MP-09; design §9.2, §12 settings keys, §13 safety)
//
// apply() writes a plan's hourSlots into the store for the plan's channel:
//   1. Refusals, nothing written: no store / channel / slots, plan errors (400); a slot in [now, now + 2 h] (409) unless
//      allowOnAir. "On air" is the plan's onAirSlots (taken when it was planned) plus the same rule again at apply time
//      (ApplyOptions::nowMs, the channel's zone): a grid slot "D-H" counts when its weekday / hour comes up in that
//      window, a dated one when its date / hour does. A grid write only changes the template (a dated override of the
//      same hour still wins on air), but it is refused all the same: the rule stays simple and on the safe side.
//   2. Backup first: settings "masterplay.backup.<importId>" = {importId, channel, at, file, sha256, order[],
//      slots{id: the slot's JSON as the store had it, or null when absent}, written{id: digest of what is written}}.
//   3. History: "masterplay.imports" = {imports: [newest first]}, the entry {id, at, by, file, sha256, channel, mode,
//      from?, to?, slots, slotIds[], created[], replaced[], onAir[], options, state, backup} is written with state
//      "applying" before the first slot and "applied" (or "partial" when a save failed) after the last, so a crash in
//      between still leaves an undoable record. Newest 100 entries; the backups of all but the newest 20 are cleared
//      (backup false; they can no longer be undone).
//   4. Each slot goes through ScheduleStore::saveSlot (revision bump -> the executor reloads), with
//      showBlock.masterplay.importId stamped; then afterSave() once (the Supervisor passes SpotGenerator::enforceMarkers,
//      as the PUT slot route does, so GEN markers survive). A failed save stops the apply (500) with what was written.
// undo(importId) puts every slot of that import back: the backup JSON through saveSlot, or deleteSlot when the slot was
// absent. It refuses (409) slots on air (unless allowOnAir), an import already undone, and one whose backup was pruned;
// 404 for an unknown id. A slot changed since the import (its digest is neither what the import wrote nor the backup) is
// a conflict and left alone unless force; the entry then stays "undoPartial" and can be undone again. Restored slots
// compare equal to the backup byte for byte in compact JSON, except "updatedAt" (saveSlot stamps the time).
// Digests: sha1 (16 hex) of the slot's compact JSON without "updatedAt"; an absent slot is "absent".
// THREADING: store writes on the caller's thread (the store's own mutex); quick (one row per slot).

struct ApplyOptions {
    QString importId;          // "" = generated "mp<UTC yyyyMMddTHHmmsszzz>-<6 hex>"; given: [A-Za-z0-9_.-]{1,64}, unused
    bool allowOnAir = false;   // write / restore slots inside [now, now + 2 h]
    bool force = false;        // undo: also restore slots changed since the import
    qint64 nowMs = 0;          // 0 = the clock
    QString by;                // who (history)
    std::function<void()> afterSave;   // once, after slots were written / restored (SpotGenerator::enforceMarkers)
    /// {importId, allowOnAir, force, nowMs, by}
    static ApplyOptions fromJson(const QJsonObject& o);
};

struct ApplyResult {
    int status = 200;          // 200 | 400 bad request | 404 unknown import | 409 on air / undone / no backup | 500 store
    QString error;
    QString importId, channelId;
    QStringList written;       // apply: slots saved
    QStringList created;       // apply: of those, absent before (undo deletes them)
    QStringList backedUp;      // apply: of those, present before (their JSON is in the backup)
    QStringList restored;      // undo: slots put back from the backup
    QStringList deleted;       // undo: slots the import had created
    QStringList unchanged;     // undo: already equal to the backup (e.g. never written: an apply that stopped early)
    QStringList conflicts;     // undo: changed since the import (skipped unless force; restored with force)
    QStringList failed;        // store writes that failed
    QStringList onAirSlots;    // the slots in [now, now + 2 h] (refused unless allowOnAir)
    QJsonObject entry;         // the history entry after the call
    bool ok() const { return status == 200; }
    /// {ok, status, error?, importId, channel, written[], created[], backedUp[], restored[], deleted[], unchanged[],
    ///  conflicts[], failed[], onAirSlots[], entry}
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Importer

class Importer {
public:
    /// store: optional, for the channel's time zone (Options::timeZone wins) and the ui.inputMap slots ("inN" live
    /// sources). resolver: required (the library and the path rules). read: sub-playlists and ChangePlaylist targets,
    /// .osd cfg files and *FromFile texts (tl::fileReader / memoryReader); none = not read.
    Importer(sched::ScheduleStore* store, const res::Resolver* resolver, tl::ReadFn read = {});

    /// The dry-run plan of a parsed playlist. docPath: where it was read from ("" for pasted text). Deterministic.
    Plan plan(const Document& doc, const QString& docPath, const Options& o) const;

    /// A RunlistBuilder lookup over the resolver's library (byId, bySeries, byTypes, all).
    sched::MediaLookup mediaLookup() const;

    /// "mp-" + 16 hex of sha1 over the parts (the id rule of the header comment).
    static QString itemId(const QString& itemFile, const QString& windowFile, int windowLine, const QString& dayKey, int windowStartSec,
                          int itemIndex, int itemLine, int copy = 0);

    /// Writes plan.hourSlots for plan.options.channelId with a backup and a history entry (see Apply / undo above).
    /// Needs the store; the resolver is not used.
    ApplyResult apply(const Plan& plan, const ApplyOptions& a = {}) const;
    /// Puts the slots of an applied import back (a.allowOnAir, a.force, a.nowMs, a.by, a.afterSave).
    ApplyResult undo(const QString& importId, const ApplyOptions& a = {}) const;
    /// The history, newest first (at most limit; <= 0: all kept).
    QJsonArray imports(int limit = 0) const;
    /// The backup of an import ({} when unknown or pruned).
    QJsonObject backup(const QString& importId) const;
    /// sha1 (16 hex) of a slot JSON's compact form without "updatedAt" (the digest apply / undo compare).
    static QString slotDigest(const QJsonObject& slotJson);
    /// Slot ids among `ids` whose hour overlaps [nowMs, nowMs + 2 h] in tz ("D-H" by weekday / hour, dated ids by date).
    static QStringList onAirAmong(const QStringList& ids, const QTimeZone& tz, qint64 nowMs);

    static constexpr int kMaxImports = 100;   // history entries kept
    static constexpr int kMaxBackups = 20;    // newest imports whose backup is kept (undoable)

private:
    sched::ScheduleStore* m_store = nullptr;
    const res::Resolver* m_res = nullptr;
    tl::ReadFn m_read;
};

} // namespace mp::imp
