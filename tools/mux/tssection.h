#ifndef PP_TSSECTION_H
#define PP_TSSECTION_H

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * PlayPlayTsKit, part 1 (docs/plans/inhouse-deps.md §6.5, DEP-14): MPEG-2 sections and their TS packetization.
 * QtCore only, no I/O, no TSDuck. psip.h builds the tables on top of this, scte35bin.h the splice_info_section.
 *
 * Byte-identical with TSDuck 3.41 (`tstabcomp --atsc`, `tspacketize`, the tsp `inject` plugin), checked against goldens in
 * tests/inhouse/data/psip made once with those tools:
 *  - reserved bits are written as '1' (also the two bits after private_indicator in short sections: 0x30 / 0xB0 / 0xF0);
 *  - sections are packed back to back in the TS payload, but a section header (3 bytes short, 8 bytes long) is never split
 *    across packets: when it would not fit, the rest of the packet is stuffed with 0xFF and the section starts in the next
 *    packet (PUSI set, pointer_field 0).
 */
namespace tskit {

constexpr int kPacketSize = 188;
constexpr int kMaxPsiSection = 1024;        // PAT, PMT, TVCT (A/65 caps VCT section_length at 1021; TSDuck splits at 1024)
constexpr int kMaxPrivateSection = 4096;    // MGT, EIT, ETT, RRT, STT, SCTE-35, generic private tables
constexpr int kNullPid = 0x1FFF;

/// MPEG-2 CRC32 (poly 0x04C11DB7, init 0xFFFFFFFF, MSB first, no final XOR). "123456789" -> 0x0376E6E7.
quint32 crc32(const char* data, qsizetype size);
inline quint32 crc32(const QByteArray& b) { return crc32(b.constData(), b.size()); }

/// MSB-first bit writer. Reserved fields go through reserved() so they are always '1'.
class BitWriter {
public:
    void bits(quint64 value, int count);                 // 1..64 bits, low `count` bits of value
    void reserved(int count) { bits(~0ULL, count); }
    void u8(quint32 v) { bits(v, 8); }
    void u16(quint32 v) { bits(v, 16); }
    void u24(quint32 v) { bits(v, 24); }
    void u32(quint32 v) { bits(v, 32); }
    void bytes(const QByteArray& b);                      // must be byte aligned
    bool aligned() const { return m_n == 0; }
    int size() const { return int(m_out.size()); }        // whole bytes written so far
    const QByteArray& data() const { return m_out; }      // call when aligned
private:
    QByteArray m_out;
    quint64 m_acc = 0;
    int m_n = 0;                                          // bits pending in m_acc (< 8)
};

struct LongHeader {
    int tableId = 0;
    bool privateIndicator = true;     // MPEG PSI: false; ATSC PSIP and private tables: true
    int tableIdExt = 0;
    int version = 0;
    bool current = true;
};
/// One long section (section_syntax_indicator = 1): 8-byte header + payload + CRC32.
QByteArray longSection(const LongHeader& h, int sectionNumber, int lastSectionNumber, const QByteArray& payload);
/// A long table from its section payloads: section_number 0..n-1, last_section_number n-1.
QList<QByteArray> longTable(const LongHeader& h, const QList<QByteArray>& payloads);
/// A short section (section_syntax_indicator = 0, reserved '11', no CRC).
QByteArray shortSection(int tableId, bool privateIndicator, const QByteArray& payload);

struct SectionInfo {
    int tableId = -1;
    bool longSyntax = false;
    bool privateIndicator = false;
    int size = 0;                     // whole section in bytes (3 + section_length)
    int tableIdExt = 0, version = 0;  // long sections only
    bool current = true;
    int sectionNumber = 0, lastSectionNumber = 0;
};
/// Header of the section at data[offset]. False (+error) when truncated or longer than 4096 bytes.
bool sectionInfo(const QByteArray& data, int offset, SectionInfo* info, QString* error = nullptr);
/// Long sections and SCTE-35 splice_info_section (0xFC) end in a CRC32.
bool hasCrc(const QByteArray& section);
bool crcValid(const QByteArray& section);
/// Recomputes the trailing CRC32 in place (no-op for sections without one).
void fixCrc(QByteArray& section);
/// A tstabcomp .bin (sections back to back) -> sections. Fails on truncation or a bad CRC.
QList<QByteArray> splitSections(const QByteArray& bin, QString* error = nullptr);

/**
 * Packetizer: sections -> 188-byte TS packets on one PID, adaptation_field_control 01 (payload only), continuity counter
 * running across calls. One call = one cycle: the last packet is stuffed with 0xFF.
 *  - Packed (default, what `inject` and `tspacketize` do): a section may start right after the previous one in the same
 *    packet; PUSI + pointer_field mark the first section start in a packet.
 *  - StuffEachSection (`--stuffing`): every section starts at the beginning of a packet.
 */
class Packetizer {
public:
    enum Mode { Packed, StuffEachSection };
    explicit Packetizer(int pid, int continuity = 0) : m_pid(pid & 0x1FFF), m_cc(continuity & 0x0F) {}
    QByteArray packetize(const QList<QByteArray>& sections, Mode mode = Packed);
    int pid() const { return m_pid; }
    int continuity() const { return m_cc; }              // CC of the next packet
private:
    int m_pid;
    int m_cc;
};

// TS packet header fields (p points at the 0x47 sync byte).
inline int packetPid(const char* p) { return ((quint8(p[1]) & 0x1F) << 8) | quint8(p[2]); }
inline bool packetPusi(const char* p) { return quint8(p[1]) & 0x40; }
inline int packetCc(const char* p) { return quint8(p[3]) & 0x0F; }
inline int packetAfc(const char* p) { return (quint8(p[3]) >> 4) & 0x03; }

}   // namespace tskit

#endif // PP_TSSECTION_H
