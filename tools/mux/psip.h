#ifndef PP_PSIP_H
#define PP_PSIP_H

#include "tssection.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>

#include <vector>

/**
 * PlayPlayTsKit, part 2 (docs/plans/inhouse-deps.md §6.5, DEP-14): MPEG PSI (PAT, PMT) and ATSC A/65 PSIP (MGT, TVCT, RRT,
 * EIT, ETT, STT) table models and builders, plus a compiler for the TSDuck-XML subset MuxManager::writeTables writes, so
 * the mux needs no `tstabcomp` (up to 260 synchronous spawns per validate today).
 *
 * Output is byte-identical with `tstabcomp --atsc` (TSDuck 3.41) for everything below; tests/inhouse/test_inhouse_psip.cpp
 * compares against goldens made with it. TSDuck behaviours this reproduces on purpose:
 *  - multiple_string_structure: one segment, compression 0. Mode 0x00 + Latin-1 bytes when every character is <= U+00FF;
 *    mode = the common high byte when all characters share one that A/65 defines (0x00-0x06, 0x09-0x10, 0x20-0x27,
 *    0x30-0x33); else mode 0x3F with UTF-16BE. An empty text has no segment, except in an EIT title (one empty segment).
 *    A length-prefixed structure with no strings (RRT names, EIT title) is length 0; ETT / descriptors write number_strings 0.
 *  - EIT start_time = seconds since 1980-01-06 00:00:00 UTC with no leap seconds (TSDuck ignores GPS_UTC_offset here).
 *  - XML: names are case-insensitive; only &amp; &lt; &gt; &quot; &apos; are decoded (a numeric reference like &#233; stays
 *    literal text); raw newlines and tabs inside attribute values are kept (no attribute normalisation); CR LF -> LF.
 *  - Section splitting: PAT and TVCT at 1024 bytes, EIT at 4096; TVCT additional descriptors only in its last section.
 * One deliberate difference: a string longer than 255 encoded bytes. tstabcomp truncates it to 256 bytes and writes
 * number_bytes = 0 (a uint8 overflow), which corrupts the table; here it becomes several segments of at most 255 bytes
 * (254 in UTF-16, never splitting a surrogate pair), as A/65 allows. MuxManager's ETT descriptions (up to 1000 characters)
 * hit this case.
 *
 * Supported XML (anything else fails with `unsupported` set, so a caller can fall back to tstabcomp):
 *   tables      PAT, PMT, MGT, TVCT, RRT, ATSC_EIT, ETT, STT, splice_information_table (scte35bin.h),
 *               generic_short_table, generic_long_table
 *   descriptors registration_descriptor, ISO_639_language_descriptor, extended_channel_name_descriptor,
 *               service_location_descriptor, caption_service_descriptor, content_advisory_descriptor, generic_descriptor
 */
namespace tskit {

// ------------------------------------------------------------------------------------------------ XML (TSDuck subset)
namespace xml {
struct Element {
    QString name;
    int line = 0;
    QList<QPair<QString, QString>> attributes;
    QString text;                                         // character data directly inside, entities decoded
    std::vector<Element> children;

    bool is(const char* n) const { return name.compare(QLatin1String(n), Qt::CaseInsensitive) == 0; }
    bool hasAttribute(const QString& n) const;
    QString attribute(const QString& n) const;            // case-insensitive; empty when absent
    const Element* child(const char* n) const;            // first child with that name
};
/// Parses a UTF-8 document; root = the document element. False (+error with the line) on malformed XML.
bool parse(const QByteArray& utf8, Element* root, QString* error);

/**
 * Typed attribute access with TSDuck's value rules: integers decimal or 0x-hex (',' thousands separators allowed), range
 * checked against the field width; booleans true/false/yes/no/on/off/1/0; enumerations by name (case-insensitive) or
 * number. Keeps the first error; every getter returns the default once an error is set.
 */
class Reader {
public:
    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    bool unsupported() const { return m_unsupported; }
    void fail(const Element& e, const QString& msg);
    void failUnsupported(const Element& e, const QString& msg);
    void failWith(const QString& fullMessage, bool unsupported);             // an error already located by a nested reader

    quint64 uint(const Element& e, const char* name, int bits, bool required, quint64 def = 0);
    bool boolean(const Element& e, const char* name, bool required, bool def = false);
    QString string(const Element& e, const char* name, bool required, int maxChars = -1);
    QString char3(const Element& e, const char* name);                       // required, exactly 3 characters
    int enumeration(const Element& e, const char* name, const QList<QPair<QString, int>>& names, int bits, bool required, int def = 0);
    qint64 gpsTime(const Element& e, const char* name);                      // required "YYYY-MM-DD hh:mm:ss" -> gpsSeconds
    QByteArray hexContent(const Element& e);                                 // element text, whitespace ignored, even digits
private:
    QString m_error;
    bool m_unsupported = false;
};
}   // namespace xml

// ------------------------------------------------------------------------------------------------ building blocks
struct MultiString {
    struct Str { QString language; QString text; };
    QList<Str> strings;
    bool isEmpty() const { return strings.isEmpty(); }
    static MultiString one(const QString& text, const QString& language = QStringLiteral("eng")) { MultiString m; m.strings.append({language, text}); return m; }
};
enum class EmptyText { NoSegment, OneSegment };
QByteArray encodeMultiString(const MultiString& m, EmptyText empty = EmptyText::NoSegment);
/// Length-prefixed (uint8) form used by RRT and EIT: no strings -> just the 0 length byte.
QByteArray encodeMultiStringWithLength(const MultiString& m, EmptyText empty = EmptyText::NoSegment);
/// Decodes uncompressed segments (modes 0x00-0x33 and 0x3F); *pos advances past the structure. False when malformed.
bool decodeMultiString(const QByteArray& data, int* pos, int end, MultiString* out);

struct Descriptor { int tag = 0; QByteArray payload; };
using Descriptors = QList<Descriptor>;
QByteArray encodeDescriptors(const Descriptors& d);
int descriptorsSize(const Descriptors& d);
Descriptor registrationDescriptor(quint32 formatIdentifier, const QByteArray& additional = {});
Descriptor extendedChannelNameDescriptor(const MultiString& longName);

/// tstabcomp's GPS seconds for an UTC time: seconds since 1980-01-06T00:00:00Z, no leap seconds. -1 when out of range.
qint64 gpsSeconds(const QDateTime& utc);

// ------------------------------------------------------------------------------------------------ MPEG PSI
struct PatProgram { int programNumber = 0; int pid = 0; };
struct Pat { int version = 0; bool current = true; int tsid = 0; int networkPid = -1; QList<PatProgram> programs; };
QList<QByteArray> buildPat(const Pat& t);

struct PmtStream { int streamType = 0; int pid = 0; Descriptors descriptors; };
struct Pmt { int version = 0; bool current = true; int programNumber = 0; int pcrPid = kNullPid; Descriptors descriptors; QList<PmtStream> streams; };
QList<QByteArray> buildPmt(const Pmt& t);
bool parsePmt(const QByteArray& section, Pmt* out, QString* error = nullptr);
/**
 * The tsp `pmt --add-pid <pid>/0x86 --add-registration 0x43554549` rewrite (scte35.h): a CUEI registration_descriptor
 * appended to the program-level descriptors and an SCTE-35 component (stream_type 0x86) appended last; version unchanged,
 * CRC recomputed. Idempotent: an existing CUEI registration is not repeated and an existing component on `pid` only gets
 * stream_type 0x86 (TSDuck keeps its place and descriptors the same way).
 */
QByteArray pmtWithScte35(const QByteArray& pmtSection, int pid, QString* error = nullptr);

// ------------------------------------------------------------------------------------------------ ATSC A/65 PSIP
struct MgtEntry { int type = 0; int pid = 0; int version = 0; quint32 numberBytes = 0; Descriptors descriptors; };
struct Mgt { int version = 0; int protocol = 0; QList<MgtEntry> tables; Descriptors descriptors; };
QList<QByteArray> buildMgt(const Mgt& t);

struct VctChannel {
    QString shortName;                                    // up to 7 UTF-16 code units
    int major = 0, minor = 0;
    int modulation = 0x04;                                // 8-VSB
    quint32 carrierFrequency = 0;
    int channelTsid = 0, programNumber = 0;
    int etmLocation = 0;
    bool accessControlled = false, hidden = false, hideGuide = false;
    int serviceType = 0x02;                               // dtv
    int sourceId = 0;
    Descriptors descriptors;
};
struct Tvct { int version = 0; bool current = true; int protocol = 0; int tsid = 0; QList<VctChannel> channels; Descriptors descriptors; };
QList<QByteArray> buildTvct(const Tvct& t);

struct RrtValue { MultiString abbrev, value; };
struct RrtDimension { MultiString name; bool graduated = false; QList<RrtValue> values; };
struct Rrt { int version = 0; int protocol = 0; int region = 1; MultiString regionName; QList<RrtDimension> dimensions; Descriptors descriptors; };
QList<QByteArray> buildRrt(const Rrt& t);

struct EitEvent {
    int eventId = 0;
    quint32 startTime = 0;                                // gpsSeconds()
    int etmLocation = 0;
    quint32 lengthSeconds = 0;
    MultiString title;                                    // no strings = title_length 0
    Descriptors descriptors;
};
struct Eit { int version = 0; int protocol = 0; int sourceId = 0; QList<EitEvent> events; };
QList<QByteArray> buildEit(const Eit& t);

struct Ett { int version = 0; int protocol = 0; int tableIdExt = 0; quint32 etmId = 0; MultiString text; };
QList<QByteArray> buildEtt(const Ett& t);

struct Stt { int protocol = 0; quint32 systemTime = 0; int gpsUtcOffset = 18; bool dsStatus = false; int dsDayOfMonth = 0; int dsHour = 0; Descriptors descriptors; };
QList<QByteArray> buildStt(const Stt& t);

// ------------------------------------------------------------------------------------------------ TSDuck XML compiler
struct CompiledTable {
    QString element;                                      // XML element name, e.g. "TVCT"
    int line = 0;
    QList<QByteArray> sections;
};
struct CompileResult {
    bool ok = false;
    bool unsupported = false;                             // failed only because of an element this compiler lacks
    QString error;
    QList<CompiledTable> tables;                          // document order
    QByteArray bin() const;                               // all sections back to back (the tstabcomp .bin layout)
    int sectionCount() const;
};
/// Compiles a <tsduck> document. On error nothing is returned (like tstabcomp, which writes no file).
CompileResult compileXml(const QByteArray& utf8);
/// Reads `xmlPath`, compiles it and (when `binPath` is not empty) writes the .bin atomically.
CompileResult compileFile(const QString& xmlPath, const QString& binPath);

}   // namespace tskit

#endif // PP_PSIP_H
