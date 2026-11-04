#pragma once
// MasterPlay media resolution (the lane design notes §6.3 media, §6.4 folder coverage; slice MP-06): which PlayPlay
// library record a playlist media line names, and how much of the library an !InsertDirectory folder covers.
//
// Resolution order per media line (design §6.3):
//   0. The forced bracket duration is already split off by the parser (Entry::path / forcedDurationSec; a raw string
//      goes through mp::parseLine). %name% tokens are replaced from SubstituteTable (one pass, unknown names kept), the
//      path is normalised (/ -> \, doubled separators collapsed, trailing blanks dropped as Windows does).
//      mp://<source> live sources and other scheme:// URLs are not library media: kind live / url, never missing.
//   1. Path rules (ini::PathRule: longest prefix, whole components, case-insensitive; the MP-05 rules) map the playlist
//      path to the path this station reads ("mapped").
//   2. Exact library path, case-folded, through an in-memory index built once from the records (ScheduleStore::
//      allMedia(true): mediaByPath is case-sensitive SQL and is not used): the mapped path, then the playlist path as
//      written when a rule changed it, then <search path>\<file name> for every MplayIni search path (SuchPfade; with
//      searchSubDirs also the only library file of that exact name below one). resolvedBy "path".
//   3. Filename join key (the ANFS rule, joinKey() below) on the library file name, then on the title; UNIQUE hits only.
//      Several files -> MP018; the one under the mapped folder (then the playlist folder, then a search path) is taken
//      when exactly one is there, else the line stays unresolved with the hits as candidates. resolvedBy "name".
//   4. Quick hash: when the mapped file is readable here, the IngestManager quick md5 (a 12-line copy, quickHash())
//      is matched against extra.hash of records whose hashKind is "quick" (a "md5" full hash is never compared). When
//      no record has a quick hash the file is not read at all (hashSkipped "noLibraryHashes").
//      Identical bytes at several library paths are the same content, so one is taken (no MP018): under the playlist's
//      folders first, then the preferred record. resolvedBy "hash".
//   5. Otherwise resolvedBy "none" (MP005) with the top candidates by join-key similarity (trigram Dice, see similar()).
//
// Records: several records at one path (case / separator variants) count as ONE file. Among the hits of a step, live
// records (not archived, extra.missing not set) win over archived / missing ones, so a re-ingested file is not
// ambiguous with its stale record. Within one file the preferred record is: live, then the earliest addedAtMs, then
// the smallest id, so the answer never depends on the order of the input list. A path hit on an archived or missing
// record still resolves (the path is authoritative) and is flagged.
//
// Title keys: joinKey() drops "an extension" (/\.[^.]+$/), which on a title ("St. Louis News" -> "st") makes keys no
// file name would give. Titles carry no extension, so titleKey() is joinKey() without that step: a deliberate,
// stricter deviation from the ANFS bridge, which keys display_title with the full joinKey().
//
// Folder coverage (folder()): the library records an !InsertDirectory folderBlock would draw from after the path rules,
// with the same matching as QueryBlocks::matchesFolder (case-insensitive prefix on '/'-normalised paths, recursive or
// direct children) plus the folder's mask (Name*.ext, ? and *), playable ones by QueryBlocks::eligible, durations, and
// MP019 with "scan" / "watch" actions when nothing playable lives there.
//
// FILE ACCESS: steps 4 and the existence probes (missing / folder report) touch the file system, which blocks on a dead
// UNC host. They run only for lines the library could not resolve, cache per root, folder and file, and stop after
// Options::fileBudgetMs (later lines report "budget"). Callers on an event-loop thread set useHash / probeFiles false or
// run the resolver in a worker. A Resolver may be used from several threads (its caches are locked).
//
// Diagnostics (lane catalog codes, resolver stage, catalog severities): MP005 not in the library, MP018 ambiguous
// filename match, MP019 folder has no library media. Report builds them per use with line numbers.
#include "masterplayin.h"
#include "masterplayinstructions.h"
#include "mpini.h"
#include "schedulemodel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

namespace sched { class ScheduleStore; }

namespace mp::res {

// ---------------------------------------------------------------------------------------------------------------------
// Keys

/// The ANFS bridge join key (playout.js joinKey): the extension (/\.[^.]+$/) dropped, runs of '_' / '-' become one
/// space, JavaScript whitespace runs collapse to one space, trimmed, lower case. Pass a file name, not a path.
QString joinKey(const QString& file);
/// joinKey() without the extension step, for titles (see the header comment).
QString titleKey(const QString& title);
/// The path as the index compares it: ini::normalizePath, trailing blanks dropped, case-folded.
QString pathKey(const QString& path);
/// Last component (\ or /, trailing separators ignored).
QString fileName(const QString& path);
/// Parent folder of a normalised path ("D:\a\b.mpg" -> "D:\a", "D:\b.mpg" -> "D:\"); "" for a bare name.
QString parentFolder(const QString& path);
/// Drive-absolute ("D:\...") or UNC ("\\host\share...") after normalisation.
bool isAbsolute(const QString& path);
/// Trigram Dice coefficient (0..1) of two join keys, punctuation read as spaces: 1 for equal keys, 0 for nothing shared.
double similarity(const QString& keyA, const QString& keyB);

/// Copy of IngestManager::quickHash (ingestmanager.cpp:286): md5 of the first 4 MiB, then the last 4 MiB (or the rest
/// when the file is under 8 MiB), then the decimal size; full = md5 of the whole file. "" when unreadable. A library
/// record's extra.hash (hashKind "quick") matches only while the two stay byte-for-byte the same algorithm.
QString quickHash(const QString& path, bool full = false);

// ---------------------------------------------------------------------------------------------------------------------
// Options

struct Options {
    QList<ini::PathRule> pathMap;      // playlist path -> path this station reads (normalised by the Resolver)
    QList<ini::Substitute> substitutes;// SubstituteTable: %name% -> value (name without the '%' pair, any case)
    QStringList searchPaths;           // MplayIni::searchPaths, playlist side (mapped through pathMap)
    bool searchSubDirs = false;        // MplayIni::searchIncludeSubDirs: a file of that name anywhere below a search path
    bool useHash = true;               // step 4 (reads up to 8 MiB per unresolved file)
    bool probeFiles = true;            // exists() probes for unresolved lines and folders
    int fileBudgetMs = 30000;          // total file-system time per Resolver; afterwards probes / hashes are skipped
    int maxCandidates = 3;             // similar files listed for an unresolved line
    double minScore = 0.3;             // similarity below this is no candidate
    QString channelId;                 // folder coverage: QueryBlocks::eligible (allowedChannels)
    qint64 nowMs = 0;                  // folder coverage: availability / max age; 0 = the clock when the Resolver is built

    /// MplayIni substitutes, search paths and the sub-folder flag, with these rules.
    static Options fromIni(const ini::MplayIni& ini, const QList<ini::PathRule>& pathMap);
    /// {pathMap[{from,to,origin,note?,guess?}], substitutes[{name,value}] or {name: value}, searchPaths[],
    ///  searchSubDirs, hash, probe, fileBudgetMs, candidates, minScore, channel} (REST / CLI); absent keys keep defaults.
    static Options fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Results

enum class ResolvedBy { None, Path, Name, Hash };
QString resolvedByName(ResolvedBy b);   // none | path | name | hash

struct Candidate {
    QString mediaId, path, title;
    double score = 0;                  // 1 for a hit of the same key, else similarity()
    QString why;                       // filename | title (same join key) | hash (same bytes) | similar
    bool underFolder = false;          // below the mapped / playlist folder or a search path
    bool live = true;                  // not archived, not missing
    QJsonObject toJson() const;        // {mediaId, path, title, score, why, underFolder?, stale?}
};

struct Resolution {
    QString input;                     // as given (the media line's path, without the bracket)
    QString kind = QStringLiteral("file");   // file | live (mp://source) | url (another scheme://) | empty
    QString path;                      // after %substitute% and normalisation (the playlist side)
    QString mapped;                    // after the path rules, normalised: the path this station reads
    int rule = -1;                     // index into Resolver::rules(), -1 none
    double forcedDurationSec = -1;     // the playlist bracket ([HH:MM:SS;FF]), -1 none
    QStringList unknownSubstitutes;    // %name% tokens with no SubstituteTable entry (left in the path)

    ResolvedBy by = ResolvedBy::None;
    QString via;                       // path: mapped | playlist | searchPath; name: filename | title; hash: quick
    QString mediaId;
    sched::MediaRecord media;          // the record (a copy) when resolved
    bool archived = false, missingFile = false;   // the resolved record is archived / flagged extra.missing
    int duplicates = 0;                // other records at the resolved file's path (case / separator variants)
    int hashMatches = 0;               // library files with the same quick hash (hash step)

    bool ambiguous = false;            // several library files share the join key (MP018)
    bool pickedByFolder = false;       // ... and the one under the mapped folder / a search path was taken
    int nameMatches = 0;               // library files sharing the join key (step 3; 1 for a unique hit)
    QList<Candidate> candidates;       // ambiguous: the hits (at most 20); unresolved: the closest names (maxCandidates)

    int fileExists = -1;               // the mapped file on disk: -1 not probed, 0 no, 1 yes (unresolved lines only)
    QString hash;                      // the quick hash computed in step 4 ("" not computed)
    QString hashSkipped;               // why step 4 did not run: off | noLibraryHashes (no record has a quick hash, so
                                       // the file is not read) | notAbsolute | budget | missing | unreadable

    QString type;                      // the library type, else the ANFS TYPE_RULES on the mapped path
    bool typeDerived = false;          // type came from TYPE_RULES
    double durationSec = -1;           // the library duration (> 0), else the forced bracket, else -1

    bool resolved() const { return by != ResolvedBy::None; }
    bool reportable() const { return kind == QLatin1String("file"); }   // live sources / URLs are never missing
    /// {path, mediaId, resolvedBy, candidates[]} (the POST /masterplay/resolve shape) plus input, kind, mapped, rule?,
    /// via?, title?, type, typeDerived?, durationSec?, forcedDurationSec?, ambiguous?, pickedByFolder?, duplicates?,
    /// hashMatches?, archived?, missingFile?, fileExists?, hashSkipped?, unknownSubstitutes?
    QJsonObject toJson() const;
};

/// One !InsertDirectory folder against the library (after the path rules).
struct FolderCoverage {
    QString directory;                 // as written, after %substitute%
    QString folder, mask;              // the folder and the file mask split off its last component (*, ?)
    QString mapped;                    // the folder after the path rules, normalised: what the folderBlock names
    int rule = -1;
    bool recursive = true;             // SubDirs 1 / 2
    bool searchTerm = false;           // a %...% token left after substitution (a filename search term, MP020 at import)
    double maxAgeHours = -1;
    int media = 0;                     // non-archived records the folderBlock matches (mask applied)
    int playable = 0;                  // ... and QueryBlocks::eligible (duration, availability, channel, not missing)
    int fresh = -1;                    // playable within maxAgeHours (added date); -1 when there is no age limit
    double durationSec = 0, avgSec = 0;   // sum / mean of the playable durations
    int unmappedMedia = -1;            // when a rule applied: records under the folder as written (a wrong rule shows)
    int exists = -1;                   // the mapped folder on disk: -1 not probed (or something playable), 0 no, 1 yes
    QStringList sample;                // up to 5 playable titles, name order
    bool empty() const { return playable == 0; }
    /// {directory, folder, mask?, mapped, rule?, recursive, searchTerm?, maxAgeHours?, media, playable, fresh?,
    ///  durationSec, avgSec, unmappedMedia?, exists?, sample[], actions[]}: actions, when nothing is playable, are
    ///  {action:"scan", method:"POST", path:"/api/v1/media/scan", body:{folder, type}} and {action:"watch",
    ///  method:"PUT", path:"/api/v1/ingest/watch", folder:{path, recursive, type, enabled}} (merge it into the list
    ///  GET /api/v1/ingest/watch returns: the PUT replaces the whole list).
    QJsonObject toJson() const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Resolver

class Resolver {
public:
    explicit Resolver(const QList<sched::MediaRecord>& media, const Options& o = {});
    /// The whole library, archived records included (design §6.3).
    static Resolver fromStore(const sched::ScheduleStore& store, const Options& o = {});
    Resolver(Resolver&&) noexcept;
    Resolver& operator=(Resolver&&) noexcept;
    ~Resolver();

    const Options& options() const;
    const QList<ini::PathRule>& rules() const;       // normalised (ini::normalizeRules order)
    const QList<sched::MediaRecord>& media() const;
    int size() const;

    /// %substitute% + normalisation (the playlist side). unknown receives %name% tokens without a substitute.
    QString substitute(const QString& path, QStringList* unknown = nullptr) const;
    /// substitute(), then the path rules, normalised. rule receives the rule index (-1 none).
    QString mapPath(const QString& playlistPath, int* rule = nullptr) const;

    /// A media line's path (bracket already split off) and its forced duration.
    Resolution resolve(const QString& path, double forcedDurationSec) const;
    /// A raw media line or path: a trailing bracket duration and TAB fields are split off by mp::parseLine.
    Resolution resolve(const QString& line) const;
    /// A parsed media entry (Entry::path / forcedDurationSec).
    Resolution resolve(const Entry& media) const;

    /// The library files closest to a file name by join key (filename and title keys), best first, at most n, score at
    /// least Options::minScore; files under preferFolder (a normalised folder) rank first among equal scores.
    QList<Candidate> similar(const QString& file, int n, const QString& preferFolder = {}) const;

    FolderCoverage folder(const QString& directory, bool recursive, double maxAgeHours = -1) const;
    FolderCoverage folder(const InsertDirectorySpec& spec) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// ---------------------------------------------------------------------------------------------------------------------
// Report: the missing / ambiguous / folder reports of an import (design §6.3, §6.4)

/// Where a resolution was needed.
struct Use {
    QString file;                      // the playlist file ("" for pasted text)
    int line = 0, col = 0;             // the media / instruction line
    int eventLine = 0;                 // its event header (0: before the first header)
    QString event;                     // the header stamp as written
};

class Report {
public:
    void add(const Resolution& r, const Use& u);
    void addFolder(const FolderCoverage& f, const Use& u);

    /// Unresolved files grouped by mapped path, in first-use order: [{path, mapped, rule?, file?, uses, lines[],
    /// events[{line, stamp}], exists?, hashSkipped?, type, durationSec?, forcedDurationsSec[]?, candidates[]}] (lines /
    /// events: first 50). durationSec is the first use's; when uses carry different brackets, forcedDurationsSec lists
    /// the distinct ones in first-use order (null = no bracket). The per-item duration is each use's Resolution.
    QJsonArray missing() const;
    /// MP018 files: [{path, mapped, picked (mediaId or ""), uses, lines[], candidates[]}].
    QJsonArray ambiguous() const;
    /// Every folder used, grouped by (mapped, mask, recursive, maxAgeHours, searchTerm, folder as written):
    /// FolderCoverage::toJson() + uses, lines[], events[].
    QJsonArray folders() const;
    /// {items, resolved, byResolvedBy{path, name, hash, none}, live, urls, files, missingFiles, ambiguousFiles,
    ///  pickedByFolder, folders, emptyFolders, folderUses}
    QJsonObject stats() const;
    /// One per use, sorted by line: MP005 (unresolved file), MP018 (ambiguous), MP019 (empty folder).
    QList<Diagnostic> diagnostics() const;
    /// {stats, missing, ambiguous, folders, diagnostics}
    QJsonObject toJson() const;

private:
    struct FileRow { Resolution r; QList<Use> uses; QList<double> forced; };   // r: the first use's; forced: distinct brackets
    struct FolderRow { FolderCoverage f; QList<Use> uses; };
    QList<FileRow> m_files;            // one per mapped key (case-folded), first-use order
    QHash<QString, int> m_fileIndex;
    QList<FolderRow> m_folders;
    QHash<QString, int> m_folderIndex;
    int m_items = 0, m_live = 0, m_urls = 0;
    int m_by[4] = {0, 0, 0, 0};
};

/// Resolves every media line and !InsertDirectory folder of a parsed document (events and preamble), each distinct
/// path once. file names the playlist in the report rows.
Report resolveDocument(const Document& doc, const Resolver& r, const QString& file = {});

} // namespace mp::res
