#pragma once
// MasterPlay log readers (the lane design notes §3.3 formats, §8 as-run import; slice MP-04): the as-run
// LogFile.log, and Status.log / Operator.log / DebugFile.log, which share one message format. The readers are QtCore
// only and pure: bytes in, values out. MP-09 adds the as-run DB writer (QtSql, at the end) next to them; MP-11 serves
// them over REST.
//
// Every MasterPlay log line is "<stamp> <code> <text>":
//   2025-06-19T18:21:16 000 D:\media\testcards\Card-6.bmp<TAB>            as-run: an airing started
//   2025-06-19T18:21:36 000 !ShowLogo<TAB>f=D:\...\Logo.bmp<TAB>cfg=...      as-run: an instruction executed
//   2025-06-19T19:02:32 000 End Of Play                                      as-run: the run ended
//   2025-06-21T02:34:54 118 Sample.Show.S01E03.1080p.x265-GROUP.mpg<TAB>     as-run: a missed entry
//   2025-06-21T02:34:54 002 File not found: Sample.Show.S01E03...mpg        Status.log: minor error
//   2025-06-21T02:29:27 009 Change Playlist from A.in to: B.in               Operator.log: operator action
//   2025-06-19T18:21:12.060 004 UpdateOutputRes in                           DebugFile.log: debug, milliseconds
// The stamp is ISO (AsRunLogIsoDateTime=1, the only form seen) with optional milliseconds, or the legacy
// "DD/MM/YYYY hh:mm:ss" form, read like a legacy header stamp (auto | dmy | mdy, MP007 once per file) [UNVERIFIED: no
// legacy sample exists]. Lines are decoded like a playlist: UTF-8 BOM, UTF-16 LE / BE BOM (MasterPlay offers UTF-16 for
// as-run logs), strict UTF-8, else cp1252 (MP010).
//
// As-run semantics (§3.3, [ANFS] parseAsRunText): 000 = started (an airing, an executed !instruction, or "End Of
// Play"), 118 = missed entry (pairs with Status.log 002 "File not found:"); 110 / 119 exist in MasterPlay's reader
// tables with no known meaning and are kept and counted as "other". The log has no duration and no end, so an
// airing's duration is DERIVED: the gap to the next boundary (the next airing, a missed entry, or End Of Play), with
// commands and other codes transparent (a !ShowLogo fired 20 s into a 45 s story must not cut it to 20 s), capped
// (3600 s: a stop between runs must not become a nine-hour spot). A gap above the cap or running backwards leaves the
// duration unknown (-1, truncated); the last airing with nothing after it is open (still on air when the log was read).
// "Missing" is never guessed.
//
// Diagnostics (mp::Diagnostic, codes from the lane catalog with their catalog meaning):
//   MP001 warning  a line that is not "<stamp> <code> <text>" (a cut-off live tail, garbage); it is skipped
//   MP004 info     a line stamped earlier than the line above it (the clock went back; a fall-back hour read with a
//                  time zone is not reported, its instants still run forward)
//   MP007 warning  legacy stamps valid in both DMY and MDY order (once per file)
//   MP010 warning  not UTF-8 and no BOM: decoded as cp1252; or bytes not valid in the BOM's encoding
//
// Everything lives in mp::logs so the names (inferType, titleOf ...) cannot collide with the resolver's in the same
// static library.
#include "masterplayin.h"
#include "schedulemodel.h"   // sched::AsRunRow (the DB writer, MP-09)

#include <QDate>
#include <QPair>
#include <functional>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimeZone>

namespace mp::logs {

/// One stamped line of any MasterPlay log.
struct LogLine {
    int line = 0;                 // 1-based physical line (a BOM is not a line)
    QDate date;
    QTime time;                   // wall clock as written; DebugFile.log stamps carry milliseconds
    bool hasMs = false;           // the stamp had a fraction
    bool legacyStamp = false;     // "DD/MM/YYYY hh:mm:ss" instead of ISO
    int code = -1;                // the three-digit message code (000 -> 0)
    QString text;                 // everything after the code and one separator, as written (TABs kept)
    QString raw;                  // the whole line without its terminator
    QString stamp() const;        // ISO local "YYYY-MM-DDThh:mm:ss[.zzz]"
};

struct ReadOptions {
    QString legacyDateOrder = "auto";   // legacy stamps: auto (DMY; MDY when the second number > 12) | dmy | mdy
};

/// Decoded, split and stamped lines. Blank lines are counted, unparsed ones counted and reported (MP001).
struct LogText {
    Encoding encoding = Encoding::Utf8;
    QList<LogLine> lines;
    int physicalLines = 0, blank = 0, unparsed = 0;
    QList<Diagnostic> diagnostics;
};
LogText readLog(const QByteArray& bytes, const ReadOptions& o = {});

// ---------------------------------------------------------------------------------------------------------------------
// As-run (LogFile.log)

enum class AsRunKind { Media, Command, EndOfPlay };
QString asRunKindName(AsRunKind k);   // media | command | endOfPlay

struct AsRunEntry {
    int line = 0;
    QDate date;
    QTime time;
    bool hasMs = false;
    qint64 startMs = 0;           // UTC epoch ms: the wall clock read in AsRunOptions::tz (naive UTC when it is unset)
    bool dstAmbiguous = false;    // the wall clock happens twice (fall back): the first occurrence, unless that would
                                  // run backwards from the line above, then the second
    bool dstGap = false;          // the wall clock does not exist (spring forward): read with the offset before the jump
    int code = -1;                // 0 started, 118 missed; anything else is "other" (transparent, counted)
    AsRunKind kind = AsRunKind::Media;
    QString body;                 // the first TAB field, trimmed: a path or bare name, "!ShowLogo", "End Of Play"
    QStringList fields;           // the TAB fields after it: info fields of an airing (AsRunLogAddInfoFields), the
                                  // parameters of a command. A media line's trailing TAB gives one empty field.
    QString text;                 // everything after the code, as written

    // Media (also a missed entry)
    QString path;                 // = body
    QString file;                 // the last path component (\ or /)
    QString title;                // file without its extension
    bool bare = false;            // no directory (AsRunLogWithDirectories=0, or the plant's bare names)
    bool prefetch = false;        // the prefetch copy (parent folder in AsRunOptions::prefetchDirs): the library path the
                                  // playlist asked for is not in the log, only the file name joins back
    QString type;                 // inferType(path): ad | news | promo | bumper | weather | unknown

    // Command
    QString instruction;          // catalog name ("OSD off", "ShowLogo", "Repeat" for !Wiederholen), else as written
    QString arg;                  // the head (mp::identify().head): crawl text, a folder, "f=" stays a field
    bool knownInstruction = false;

    // Derived, for airings (code 0 media) only
    qint64 durationMs = -1;       // gap to the next boundary; -1 unknown (truncated, open, or not an airing)
    int boundaryLine = 0;         // the line of that boundary (0: none followed)
    bool closedByEndOfPlay = false;
    bool truncated = false;       // a boundary followed but the gap ran backwards or exceeded the cap
    bool open = false;            // no boundary followed: still on air when the log was read

    // Missed entries (code 118), filled by attachReasons()
    QString reason;               // "File not found"
    int reasonLine = 0;           // the Status.log line

    /// Stable dedup key "<stamp>-<code>-<sha1(text) 10 hex>-<n>", n counting identical (stamp, code, text) lines above
    /// it (a missing file is retried several times a second). MP-09 stores it as runlist_item_id "mp:" + key. A
    /// re-read of a tail gives the same keys when it starts at a line whose second differs from the line before it.
    QString key;

    bool started() const { return code == 0; }
    bool missed() const { return code == 118; }
    bool airing() const { return code == 0 && kind == AsRunKind::Media; }
    QString stamp() const;
    /// {line, at, startMs, code "000", kind, body, fields?, text?, key, path/file/title/bare/prefetch/type (media),
    ///  instruction/arg/known (command), durationSec (null when unknown), boundaryLine, closedByEndOfPlay?, truncated?,
    ///  open?, reason?, reasonLine?, dstAmbiguous?, dstGap?}
    QJsonObject toJson() const;
};

struct AsRunOptions {
    int capSec = 3600;                    // longest derived duration; longer gaps are left unknown
    /// Source time zone (design §8: default the channel's, America/New_York, chosen by the caller). Invalid: naive, the
    /// wall clock is read as UTC, so durations across a DST change are off by the change.
    QTimeZone tz;
    /// A missed entry closes the airing before it: MasterPlay moved on at that moment [UNVERIFIED; the ANFS reader,
    /// which ignores codes, does the same]. false: missed entries are transparent like commands.
    bool missedIsBoundary = true;
    QStringList prefetchDirs = {QStringLiteral("prefetch")};   // parent folder names marking the prefetch copy
    QString legacyDateOrder = "auto";
};

struct AsRunLog {
    Encoding encoding = Encoding::Utf8;
    bool naive = true;            // no time zone: startMs is the wall clock read as UTC
    QString tzId;                 // the zone used ("" when naive)
    int capSec = 3600;
    QList<AsRunEntry> entries;    // file order; unparsed and blank lines are not entries
    int physicalLines = 0, blank = 0, unparsed = 0;
    QList<Diagnostic> diagnostics; // sorted by line

    /// {lines, entries, blank, unparsed, played (code 000 lines), missed (118), other, byCode{"000": n},
    ///  airings (000 media), commands, endOfPlay, durationKnown, truncated, open, playedSec (sum of known durations),
    ///  files (distinct airing file names), missedFiles (distinct), missedWithReason, byInstruction{name: n}, first,
    ///  last, naive, tz, capSec, diagnostics{error, warning, info}}
    QJsonObject stats() const;
    /// {encoding, stats, entries[] (the first `limit`, all when -1), diagnostics[]}
    QJsonObject toJson(int limit = -1) const;
};
AsRunLog parseAsRun(const QByteArray& bytes, const AsRunOptions& o = {});

// ---------------------------------------------------------------------------------------------------------------------
// Status.log, Operator.log, DebugFile.log

/// What a message says, read from its text (MasterPlay 6.2 wording; anything else is Other).
enum class MessageKind {
    Start,              // "Start of Mplay 6.2.0.80 PE2 Computer: NAME" (001 Status, 010 Operator)
    Close,              // "Close of Mplay"
    OnAir,              // "OnAir enabled" | "OnAir disabled"
    Output,             // "Start Output" | "Stop Output" | "Failed to set device: ..." | "FAILED TO ENABLE OUTPUT, ..."
    FileNotFound,       // "File not found: X"
    PlaylistNotFound,   // "Playlist not found: X" (X may be empty)
    Exception,          // "Exception: ..."
    PlaylistChange,     // "Change Playlist from A to: B"
    PlaylistChanged,    // "Playlist changed"
    Button,             // "Next Button pushed", "Play Button pushed"
    ConfigSaved,        // "Configuration saved"
    Other,
};
QString messageKindName(MessageKind k);   // start | close | onAir | output | fileNotFound | playlistNotFound | exception |
                                          // playlistChange | playlistChanged | button | configSaved | other
/// 001 info, 002 minor, 003 major, 004 debug, 009 operator, 010 app; "" for any other code.
QString severityName(int code);

struct MessageEntry {
    int line = 0;
    QDate date;
    QTime time;
    bool hasMs = false;
    int code = -1;
    MessageKind kind = MessageKind::Other;
    QString text;                 // the message as written
    /// Start: the version ("6.2.0.80 PE2"); FileNotFound / PlaylistNotFound: the name; Exception: the message; Button:
    /// the button ("Next"); OnAir: enabled | disabled; Output: start | stop | error.
    QString subject;
    QString from, to;             // PlaylistChange
    QString computer;             // Start
    QString stamp() const;
    QJsonObject toJson() const;   // {line, at, code "002", severity, kind, text, subject?, from?, to?, computer?}
};

struct MessageLog {
    Encoding encoding = Encoding::Utf8;
    QList<MessageEntry> entries;
    int physicalLines = 0, blank = 0, unparsed = 0;
    QList<Diagnostic> diagnostics;

    /// {lines, entries, blank, unparsed, byCode{"001": n}, bySeverity{info: n}, byKind{fileNotFound: n}, first, last}
    QJsonObject stats() const;
    /// The import report (design §8; nothing of it is stored): {sessions[{start, close, version, computer, line}],
    ///  filesNotFound[{file, count, first, last}] (grouped by name, case-insensitive), playlistsNotFound[{at, name}],
    ///  exceptions[{at, text}], playlistChanges[{at, from, to}], onAir[{at, enabled}], outputErrors[{at, text}],
    ///  buttons{Next: n}, configSaved}. Each list keeps its first maxList rows; "<list>Total" counts them all.
    QJsonObject report(int maxList = 200) const;
};
/// Status.log, Operator.log and DebugFile.log share the format; the code says which kind of line it is.
MessageLog parseMessages(const QByteArray& bytes, const ReadOptions& o = {});

/// Pairs each missed (118) entry with a Status.log "File not found:" line naming the same file (case-insensitive, by
/// file name, since either side may carry a directory) within windowSec of it, in file order, each status line used
/// once. Sets reason / reasonLine and returns how many entries got one.
int attachReasons(AsRunLog& asrun, const MessageLog& status, int windowSec = 2);

/// The ANFS content-type rules (playout.js TYPE_RULES), first match wins over the whole path, case-insensitive:
/// ad | news | promo | bumper | weather, else unknown (never defaulted to program).
QString inferType(const QString& path);
/// Last path component (\ or /, trailing separators ignored) and that name without its extension.
QString fileOf(const QString& path);
QString titleOf(const QString& file);

// ---------------------------------------------------------------------------------------------------------------------
// As-run DB writer (MP-09, design §8 "Writer"): AsRunLog entries -> as_run rows of a PlayPlay ScheduleStore database.
//
// Mapping (origin "masterplay", plannedStart = actualStart = startMs and variance 0, so adherence reports invent no
// lateness; runlist_item_id "mp:" + key, the dedup key):
//   airing (000 media)   START / ON_TIME; actualEnd / duration from the derived duration (0 when unknown: truncated or
//                        open, json.durationDerived false); title = the file without extension; type = the library's
//                        (resolve) else inferType (json.typeDerived).
//   missed (118 media)   SKIP / SKIPPED, reason = the attached Status.log reason ("File not found") else "MasterPlay missed
//                        entry"; includeMissed false leaves them out.
//   command (000 !...)   CMD rows (status "" , title "!<instruction>") only with includeCommands.
//   End Of Play, other codes: no row (End Of Play is only a duration boundary).
// json = {mpCode, mpBody, mpSource, mpKey, mpLine, mpPath, durationDerived, typeDerived, mpMediaId?, prefetch?, truncated?,
// open?, closedByEndOfPlay?, dstAmbiguous?, dstGap?, mpInstruction?, mpArg?}. Contract pacing: assetId is the resolved
// library id ONLY with countTowardContracts (ScheduleStore::airedCount counts START rows of an asset on every channel, so
// imported airings would otherwise change PlayPlay's ad pacing); without it the id goes to json.mpMediaId.
//
// Writer: its own QSQLITE connection on the store's file (never creates it), busy timeout 5000 ms, WAL (the store's
// mode), one BEGIN IMMEDIATE transaction per 2,000 rows (the write lock is taken up front, so a concurrent store write
// waits on the busy handler instead of failing with a stale snapshot). Dedup: a row whose (channel_id, runlist_item_id,
// event_type) exists is counted as a duplicate (indexed by idx_asrun_item); re-importing the same log inserts 0. Bulk rows
// do not go through ScheduleStore::insertAsRun (a signal per row, the store mutex held for 74k inserts), so no asRunAdded
// signal fires. THREADING: blocking file I/O; run it in a worker (MP-11). The connection lives and dies in the calling
// thread.

struct AsRunWriteOptions {
    QString channelId;                 // as_run.channel_id (design: the pseudo channel "mp-<sourceId>"); required to write
    QString source;                    // json.mpSource: the source id or the log path
    qint64 fromMs = 0, toMs = 0;       // entries with startMs in [fromMs, toMs] (0: that end open)
    bool includeMissed = true;         // SKIP rows for 118 entries
    bool includeCommands = false;      // CMD rows for executed instructions
    bool countTowardContracts = false; // assetId = the resolved library id (see above)
    /// Library lookup of an airing / missed entry: {mediaId, type}; empty mediaId = not in the library. Optional.
    std::function<QPair<QString, QString>(const AsRunEntry&)> resolve;
    int batchRows = 2000;              // rows per transaction (1..100000)
    int busyTimeoutMs = 5000;
    int sampleRows = 20;               // rows echoed in AsRunWriteResult::sample
};

struct AsRunWriteResult {
    bool dryRun = true;
    int played = 0, missed = 0, commands = 0;   // rows mapped in range: START, SKIP, CMD
    int outOfRange = 0;                         // entries that would map to a row but lie outside [fromMs, toMs]
    int inserted = 0, duplicates = 0;           // dry run: duplicates counted against the DB when it was given
    int wouldInsert = 0;                        // dry run: the rows not in the DB yet (all of them without a DB)
    int transactions = 0;
    QString error;                              // "" = ok
    QJsonArray sample;                          // the first sampleRows rows (AsRunRow::toJson)
    bool ok() const { return error.isEmpty(); }
    /// {dryRun, rows{played, missed, commands}, outOfRange, inserted, duplicates, wouldInsert (dry run), transactions,
    ///  error?, sample[]}
    QJsonObject toJson() const;
};

/// The as_run rows of a log (mapping above), in log order; counts the rows into *counts (played / missed / commands /
/// outOfRange) when given.
QList<sched::AsRunRow> asRunRows(const AsRunLog& log, const AsRunWriteOptions& o, AsRunWriteResult* counts = nullptr);

/// Maps the log and inserts the rows that are not there yet into dbPath's as_run table. dryRun: nothing is written; the
/// duplicates are counted read-only when dbPath is given ("" skips the DB). Errors: no channel, no such file, no as_run
/// table, a lock not obtained within the busy timeout, a failed insert / commit (that transaction is rolled back; the
/// committed ones stay, and a re-run skips them).
AsRunWriteResult writeAsRun(const QString& dbPath, const AsRunLog& log, const AsRunWriteOptions& o, bool dryRun = true);

} // namespace mp::logs
