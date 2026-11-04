#pragma once
// MasterPlay OSD configs (the lane design notes §3.1, §6.8; slice MP-05): the *.osd files a !ShowLogo / !ShowCrawl /
// !ShowClock / !CountDown line names with cfg=, and the per-layer OSD defaults Mplay.INI keeps in the same shape
// ([CountDown0], [Laufschrift0] ...). QtCore only and pure: bytes in, values out (mpini.h reads the INI).
//
// An .osd file is an INI with one section [Global] (the D:\Masterplay\OSD samples):
//   Type       1 logo, 5 clock / counter, 6 crawl; absent in DefaultCountDown.osd
//   XPos YPos  position (50/50 is the default; [Laufschrift0] in Mplay.INI has YPos=486, an NTSC line) [UNVERIFIED unit]
//   Trans1 BTrans DTrans (Trans2 Trans3 in Mplay.INI)    transparency modes [UNVERIFIED]
//   FontName FontSize FontColor BFarbe (Farbe1 Farbe2)  font; colours are Delphi TColor integers
//   CrawlTime RollTime FieldMode (OSDFieldMode)         crawl speed and field mode [UNVERIFIED]
// Keys the reader does not model are kept in `extra`, never dropped and never an error (newer MasterPlay versions add
// keys).
//
// TColor is $00BBGGRR: the low byte is red. 16777215 = $FFFFFF = #FFFFFF, 8421376 = $808000 = #008080 (teal). A high
// byte of $FF or $80 is a Windows system colour index (clBtnFace = $FF00000F), $1FFFFFFF clNone, $20000000 clDefault:
// those have no fixed RGB and give "" (the importer uses its own default). The value may be written in decimal
// (TIniFile.WriteInteger, the samples), "$hex", "0xhex" or as a Delphi colour name (clWhite, clTeal ...).
//
// Placement: XPos / YPos are kept as written; their unit is not settled (design open question 4), so placement() gives
// the design's default anchor per kind (logo top right, crawl bottom, clock top left) in the compositor's
// {anchor, marginX, marginY} form and says so (fromOsd false).
//
// Diagnostics (lane catalog codes): MP011 for a value that is not a number / colour, and for a file without [Global];
// readIni() adds MP001 / MP010.
#include "mpini.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace mp::osd {

/// [Global] Type: 1 logo, 5 clock / counter, 6 crawl. Other numbers are kept in OsdConfig::type with Kind::Other.
enum class Kind { Absent, Logo, Clock, Crawl, Other };
QString kindName(Kind k);   // absent | logo | clock | crawl | other
Kind kindOfType(int type);  // -1 -> Absent

/// "#RRGGBB" from a TColor; nullopt for a system colour, clNone / clDefault, or a value outside 32 bits.
std::optional<QString> tColorToHex(qint64 tcolor);
/// The TColor of "#RRGGBB" / "RRGGBB" (the exporter writes BFarbe / FontColor with it); nullopt for anything else.
std::optional<qint64> hexToTColor(const QString& hex);
/// Decimal (also negative: system colours), "$hex", "0xhex", "#RRGGBB" (taken as RGB) or a Delphi colour name.
std::optional<qint64> parseTColor(const QString& text);

struct Color {
    QString raw;                  // as written; empty when the key is absent
    qint64 tcolor = -1;           // parsed value (-1 also for "absent"; check present / valid)
    bool present = false, valid = false;
    QString hex;                  // "#RRGGBB", "" when absent, invalid or a system colour
    QJsonValue toJson() const;    // the hex, or {raw, tcolor} when it has none, or null when absent
};

struct OsdConfig {
    QString name;                 // the file name (or the INI section) it was read from
    Encoding encoding = Encoding::Ansi1252;
    bool hasGlobal = false;       // a [Global] section was found (an .osd); a section read from Mplay.INI sets it too
    int type = -1;                // Type, -1 absent
    Kind kind = Kind::Absent;
    int x = -1, y = -1;           // XPos / YPos as written, -1 absent
    int trans1 = -1, trans2 = -1, trans3 = -1, bTrans = -1, dTrans = -1;
    QString fontName;
    int fontSize = -1;
    bool bold = false;
    Color fontColor;              // FontColor
    Color backColor;              // BFarbe ("Hintergrundfarbe")
    Color color1, color2;         // Farbe1 / Farbe2 (Mplay.INI sections)
    int textAlignment = -1, textFieldWidth = -1, textFieldHeight = -1;
    int crawlTime = -1, rollTime = -1, fieldMode = -1;   // FieldMode, or OSDFieldMode in Mplay.INI sections
    int timeSec = -1;             // Time (Mplay.INI sections)
    QString text;                 // Text (Mplay.INI crawl sections)
    QList<ini::Key> extra;        // keys not modelled above, in file order
    QList<Diagnostic> diagnostics;

    /// Kind from Type, else a hint from the name ("CountDown" / "CountUp" / "RealtimeClock" / "Clock" -> Clock,
    /// "Crawl" / "Laufschrift" -> Crawl, "Logo" -> Logo), else Absent. fromName (optional) tells which one answered.
    Kind effectiveKind(bool* fromName = nullptr) const;
    /// {anchor, marginX, marginY, fromOsd:false}: logo top_right 60/60, clock top_left 60/60, crawl bottom 0/0, else
    /// bottom_left 120/90 (the engine identity defaults).
    QJsonObject placement() const;
    /// {name, type, kind, x, y, trans1, bTrans, dTrans, ..., fontName, fontSize, fontColor, backColor, extra{},
    ///  placement, diagnostics[]} (absent values left out).
    QJsonObject toJson() const;
};

/// An .osd file: readIni() + section [Global].
OsdConfig readOsd(const QByteArray& bytes, const QString& name = {});
/// One INI section in the OSD shape (the .osd [Global], or Mplay.INI's [CountDown0], [Laufschrift0] ...).
OsdConfig fromSection(const ini::Section& section, const QString& name = {});

/// The OSD file a cfg= path names, looked up by file name in a folder listing, case-insensitively (cfg paths are
/// written by hand: "MasterPlay Logo.osd" names the sample "Masterplay Logo.osd"). "" when none matches.
QString matchOsdName(const QString& cfgPath, const QStringList& names);

} // namespace mp::osd
