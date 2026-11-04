#pragma once
// MasterPlay INI files (the lane design notes §3.1 / §3.2; slice MP-05): a reader with Windows profile semantics
// (what Delphi's TIniFile gets from GetPrivateProfileString), the Mplay.INI subset the importer can use, and the
// suggested source + path rules for POST /masterplay/config/from-ini (§10). QtCore only and pure: bytes in, values out.
// The .osd reader (mposd.h) is built on readIni().
//
// Profile semantics (one line at a time, CRLF / LF / CR-LF mixes all fine):
//   [Section]            leading / trailing blanks ignored, text after ']' ignored; names compare case-insensitively
//   key=value            split on the first '='; key and value trimmed (spaces, TABs); a value wrapped in matching
//                        "..." or '...' loses the pair (GetPrivateProfileString does); TABs inside a value are kept
//   ;comment             first non-blank character ';'
//   lookups              the first section of a name, then the first key in it (Windows never sees a repeated one)
// Encoding: UTF-8 BOM, UTF-16 LE / BE BOM, strict UTF-8, else cp1252 (Delphi writes the INI in the ANSI code page, so
// cp1252 is normal here and is not reported).
//
// Diagnostics (mp::Diagnostic, lane catalog codes with their catalog meaning and severity):
//   MP001 warning  a line that is no section, key, comment or blank line; a key before the first section (Windows
//                  ignores both)
//   MP010 warning  bytes not valid in the BOM's encoding
//   MP011 warning  a value the subset cannot use (a drive letter that is no letter, a SubstituteTable item without '=',
//                  a list past 256 items: the rest is dropped)
//
// BOUNDED WORK: the from-ini route reads pasted INIs, so nothing here loops on a number taken from the file (ItemCount
// is not a loop bound), de-duplication goes through sets, and every list key (SuchPfade, [SearchDirs], SubstituteTable,
// ExchangeList, *Extensions, the recent playlists) keeps at most 256 items, so the work grows linearly with the input.
// A from-ini handler should still read the bytes and run this off the event loop like any other file parse.
//
// SECRETS: Mplay.INI stores the approval users' passwords ([ApprovalUsers] UserN=name<TAB>password<TAB>...), Password2,
// SSL keys and passwords. IniFile holds every value and has no JSON view on purpose; never serialise it whole.
// MplayIni copies only the keys listed below, none of them secret, and its JSON is safe to return over REST.
//
// [UNVERIFIED] list formats. SuchPfade (a TListBox "Search:"), SubstituteTable (a TMemo) and ExchangeList (a
// TRichEdit, one "from;to" pair per line) are multi-line controls stored in one INI value; every sample has them empty.
// No delimiter literal sits next to their keys in Mplay.exe / PlaylistEditor.exe, which points at TStrings.CommaText,
// so splitList() reads Delphi CommaText when the value holds a '"', else splits on ';' or '|' (or ',' when neither is
// present) without splitting on spaces, so an unquoted path with spaces stays whole.
#include "masterplayin.h"

#include <QChar>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTime>

namespace mp::ini {

struct Key {
    QString name;                 // trimmed, as written
    QString value;                // trimmed, outer matching quotes removed
    QString raw;                  // the value as written (untrimmed, quotes kept)
    int line = 0;                 // 1-based physical line (a BOM is not a line)
    int col = 0;                  // 1-based column where raw starts (just after the '=')
};

struct Section {
    QString name;                 // trimmed, as written
    int line = 0;
    QList<Key> keys;              // file order, repeated keys kept
    const Key* key(const QString& name) const;           // first match, case-insensitive; nullptr when absent
    QString value(const QString& name, const QString& def = {}) const;
    bool has(const QString& name) const { return key(name) != nullptr; }
};

struct IniFile {
    Encoding encoding = Encoding::Ansi1252;
    QList<Section> sections;      // file order, repeated sections kept (lookups see the first)
    int physicalLines = 0, blank = 0, comments = 0, ignored = 0;
    QList<Diagnostic> diagnostics; // sorted by line

    const Section* section(const QString& name) const;   // first match, case-insensitive
    QString value(const QString& section, const QString& key, const QString& def = {}) const;
    bool has(const QString& section, const QString& key) const;
    /// Decimal integer (a leading '+' / '-' allowed, blanks trimmed); def when absent, empty or not a number (ok false).
    int intValue(const QString& section, const QString& key, int def = -1, bool* ok = nullptr) const;
    /// "1" / "true" / "yes" / "on" -> true, "0" / "false" / "no" / "off" -> false (any case), else def.
    bool boolValue(const QString& section, const QString& key, bool def = false) const;
    QStringList sectionNames() const;
};
IniFile readIni(const QByteArray& bytes);

/// One INI value holding a list (see the header comment): Delphi CommaText when it contains '"' ("a b",c -> [a b, c];
/// "" inside quotes is one quote; unquoted blanks also separate, as CommaText does), else split on ';' / '|', else on
/// ','. Items are trimmed, empty ones dropped.
QStringList splitList(const QString& value);

// ---------------------------------------------------------------------------------------------------------------------
// Mplay.INI subset

/// A drive / path exchange pair as the PlaylistEditor keeps it: when SAVING a playlist `editor` becomes `playlist`,
/// when loading the other way round (DFM caption: "Example: f:\ReplaceWith;G:\ReplaceWithThis ... vise versa when
/// loading").
struct PathPair {
    QString editor, playlist;
};

/// SubstituteTable: "%name%" in an InsertDirectory path stands for value ([VER] 480; item form "name=value"
/// [UNVERIFIED]). name is kept without the '%' pair.
struct Substitute {
    QString name, value;
};

enum class ExchangeMode { Absent = -1, Disabled = 0, Enabled = 1, Automatic = 2 };   // CExchangeDriveLetters items
QString exchangeModeName(ExchangeMode m);   // absent | disabled | enabled | automatic

struct MplayIni {
    Encoding encoding = Encoding::Ansi1252;
    bool isMplayIni = false;      // has [MPEGPlay] or [PlayListEditor]
    QList<Diagnostic> diagnostics;

    // [MPEGPlay]: the player
    QString iniVersion;           // Version= (the INI layout, 4.70 in 6.2; not Mplay's own version)
    QString playlistDir;          // InFilesDirectory (falls back to [PlayListEditor] InFilesDirectory)
    QString logDir;               // LogFilesDirectory
    QString osdDir;               // OSDFilesDirectory
    QString tempDir;              // TempPfad
    QString asRunLog = QStringLiteral("LogFile.log");      // LogFile
    QString statusLog = QStringLiteral("Status.log");      // StatusFile
    QString operatorLog = QStringLiteral("Operator.log");  // OperatorLogFilename
    bool asRunWithDirectories = true, asRunAddInfoFields = true, asRunIsoDateTime = true;   // AsRunLog* (default 1)
    int asRunNaming = 0, statusLogNaming = 0;              // NamingAsRun / NamingStatusLog (0: the fixed name)
    QStringList searchPaths;      // SuchPfade of [MPEGPlay] then [PlayListEditor], plus [SearchDirs] values that are
                                  // absolute paths, or lists of them (D:\A;E:\B) [UNVERIFIED section]; de-duplicated
                                  // case-insensitively, at most 256
    bool searchIncludeSubDirs = false, searchUseWildChar = false, searchNewProgInSearchPaths = false;
    QStringList searchExcludedExtensions;                  // SearchDirsExcludedExtensions (".txt;.jpg")
    QList<Substitute> substitutes;                         // SubstituteTable
    QStringList videoExtensions, videoOnlyExtensions, audioExtensions;   // "*.mpg;*.m2p..." -> [".mpg", ".m2p", ...]
    bool dailyPlaylistMode = false;   // [MPEGPlay] or [PlayListEditor] DailyPlaylistMode: one .in per day
    bool autoReloadPlaylist = false, loadPassedRepeatLoop = false, eventsSamePriority = false, osdClearEndOfExact = false;
    int loadPassedDays = -1;
    double defPictureTimeSec = -1;    // DefPictureTime ([PlayListEditor] first): still duration without a bracket
    double defaultFrameRate = -1;     // [MPEGPlay] DefaultFrameRate (23.98)
    QString stationName;              // KopfStellenName, empty while it is the German default "Noch kein Name"

    // [PlayListEditor]: the editor (shares the INI on the playout box)
    bool isoDateTimes = true;         // ISODateTimes (default 1 in 6.x): new playlists get ISO stamps
    int editorFramesPerSecond = -1;   // AnzFramesPerSecound (an older key; 25 next to Videoformat=23.98 in the sample)
    double editorFrameRate = -1;      // Videoformat ("23.98 ": the "Default Frame Rate" combo)
    QTime dayChange;                  // TagesWechsel (hh:mm), invalid when absent or bad
    ExchangeMode exchangeMode = ExchangeMode::Absent;   // ExchangeDriveLetters: 0 disabled, 1 enabled, 2 automatic
    QChar localDriveLetter;           // MasterPlayLocalDriveLetter ("Masterplay local drive"), upper case, null when unset
    QChar mappedDriveLetter;          // MappedDriveLetter ("Replace drive"), upper case, null when unset
    QList<PathPair> exchangeList;     // ExchangeList, one pair per "editor;playlist" item
    bool generateUuids = false;       // GenerateUUIDsForItems
    QString templateDir, commandListsDir, defaultSelectionDir;

    // [PlayCart0] / [LastUsedOnAirPlaylists0]: what was on air
    QString lastPlaylist;             // LastInFilename
    QString startupPlaylist;          // OnApplicationStartAlwaysLoadThisPlaylist
    QStringList recentPlaylists;      // Item0..Item<ItemCount-1> (without ItemCount: up to the first gap), empty ones
                                      // skipped, at most 256

    // [Medialooks 1]: the PE2 output
    QString outputFormat;             // Format ("1280x1080 HD720 24p")
    QString outputDevice;             // OutputDevice
    QString channelName;              // ChannelName
    bool outputNdi = false;           // OutputNDI

    /// Inferred MasterPlay folder (Mplay.INI sits in it): the most common parent of the playlist / log / OSD / temp /
    /// template / command-list folders and the DefaultSelectionDirectory / BackgroundDir values. "" when none is set.
    QString installDir;

    /// Frame rate the editor writes durations at: Videoformat rounded (23.98 -> 24, 29.97 -> 30, 59.94 -> 60), else
    /// AnzFramesPerSecound, else 30 (the parser default).
    int suggestedFps() const;
    /// Every field above (no secrets): {encoding, isMplayIni, player{...}, editor{...}, onAir{...}, output{...},
    /// installDir, suggestedFps, diagnostics[]}.
    QJsonObject toJson() const;
};
MplayIni readMplayIni(const QByteArray& bytes);
MplayIni readMplayIni(const IniFile& file);

// ---------------------------------------------------------------------------------------------------------------------
// Path rules and the source suggestion

/// A path rule as the resolver applies it: a path starting with `from` (case-insensitive, '/' == '\', whole
/// components) gets `to` instead. origin says where it came from: ExchangeList | ExchangeDriveLetters |
/// MasterPlayLocalDriveLetter | iniLocation | user. guess: not stated by the INI, the operator should confirm it.
struct PathRule {
    QString from, to, origin, note;
    bool guess = false;
    QJsonObject toJson() const;   // {from, to, origin, note?, guess?}
    static PathRule fromJson(const QJsonObject& o);
};
/// Backslashes, no doubled separators (a leading "\\" of a UNC path kept), "D:" -> "D:\".
QString normalizePath(const QString& path);
/// Applies the longest matching rule (rules with an empty `to` are skipped). Unmatched paths come back unchanged.
/// used (optional) receives the index of the rule applied, -1 when none.
QString mapPath(const QString& path, const QList<PathRule>& rules, int* used = nullptr);
/// Normalised, de-duplicated by `from` (case-insensitive; the first one wins) and sorted longest `from` first.
QList<PathRule> normalizeRules(const QList<PathRule>& rules);

struct SuggestOptions {
    /// Where the INI was read from. A UNC path (\\host\share\...\Mplay.INI) says how this station reaches the
    /// MasterPlay folder: installDir -> that folder ("iniLocation"), and the host for drive guesses. The location rule
    /// is a guess unless the share folder has the MasterPlay folder's name (the INI may be a copy kept elsewhere). A
    /// local or copied path says nothing and is only reported.
    QString iniPath;
};

struct SourceSuggestion {
    /// The design §10 source record: {id, label, read:"fs", playlistDir, logPath, osdDir, tz:""} with the folders
    /// mapped through pathMap, plus logDir, statusLog, operatorLog, dailyPlaylistMode, lastPlaylist, recentPlaylists
    /// and ini{playlistDir, logPath, osdDir} as the INI wrote them.
    QJsonObject source;
    QList<PathRule> pathMap;      // normalizeRules() order
    QStringList notes;            // why each rule is there, and what the operator still has to decide
    /// {source, pathMap[], searchPaths[], substitutes[], defaults{fps, defPictureTimeSec}, notes[], ini{MplayIni}}
    QJsonObject toJson(const MplayIni& ini) const;
};
SourceSuggestion suggestSource(const MplayIni& ini, const SuggestOptions& o = {});

} // namespace mp::ini
