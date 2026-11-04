#include "tssection.h"

#include <array>
#include <cstring>

namespace tskit {

namespace {
// Table-driven MSB-first CRC (one table, built once; sections are small, this is plenty fast).
const std::array<quint32, 256>& crcTable() {
    static const std::array<quint32, 256> t = [] {
        std::array<quint32, 256> a{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i << 24;
            for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            a[i] = c;
        }
        return a;
    }();
    return t;
}
int headerSize(const QByteArray& s) { return s.size() > 1 && (quint8(s[1]) & 0x80) ? 8 : 3; }
}   // namespace

quint32 crc32(const char* data, qsizetype size) {
    const auto& t = crcTable();
    quint32 c = 0xFFFFFFFFu;
    for (qsizetype i = 0; i < size; ++i) c = (c << 8) ^ t[((c >> 24) ^ quint8(data[i])) & 0xFF];
    return c;
}

void BitWriter::bits(quint64 value, int count) {
    // Push the bits one byte at a time through a small accumulator: fields are at most 64 bits and never hot.
    for (int i = count - 1; i >= 0; --i) {
        m_acc = (m_acc << 1) | ((value >> i) & 1);
        if (++m_n == 8) { m_out.append(char(m_acc & 0xFF)); m_acc = 0; m_n = 0; }
    }
}

void BitWriter::bytes(const QByteArray& b) {
    if (m_n == 0) { m_out.append(b); return; }
    for (char c : b) bits(quint8(c), 8);
}

QByteArray longSection(const LongHeader& h, int sectionNumber, int lastSectionNumber, const QByteArray& payload) {
    const int len = int(payload.size()) + 5 + 4;          // after section_length: 5 header bytes, payload, CRC
    BitWriter w;
    w.u8(h.tableId);
    w.bits(1, 1); w.bits(h.privateIndicator ? 1 : 0, 1); w.reserved(2); w.bits(len, 12);
    w.u16(h.tableIdExt);
    w.reserved(2); w.bits(h.version, 5); w.bits(h.current ? 1 : 0, 1);
    w.u8(sectionNumber); w.u8(lastSectionNumber);
    w.bytes(payload);
    QByteArray s = w.data();
    const quint32 crc = crc32(s);
    s.append(char(crc >> 24)); s.append(char(crc >> 16)); s.append(char(crc >> 8)); s.append(char(crc));
    return s;
}

QList<QByteArray> longTable(const LongHeader& h, const QList<QByteArray>& payloads) {
    QList<QByteArray> out;
    const int last = int(payloads.size()) - 1;
    for (int i = 0; i <= last; ++i) out.append(longSection(h, i, last, payloads[i]));
    return out;
}

QByteArray shortSection(int tableId, bool privateIndicator, const QByteArray& payload) {
    BitWriter w;
    w.u8(tableId);
    w.bits(0, 1); w.bits(privateIndicator ? 1 : 0, 1); w.reserved(2); w.bits(payload.size(), 12);
    w.bytes(payload);
    return w.data();
}

bool sectionInfo(const QByteArray& d, int off, SectionInfo* info, QString* error) {
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    if (off < 0 || d.size() - off < 3) return fail(QString("truncated section header at byte %1").arg(off));
    const quint8* p = reinterpret_cast<const quint8*>(d.constData()) + off;
    SectionInfo i;
    i.tableId = p[0];
    i.longSyntax = p[1] & 0x80;
    i.privateIndicator = p[1] & 0x40;
    i.size = 3 + (((p[1] & 0x0F) << 8) | p[2]);
    if (i.size > kMaxPrivateSection) return fail(QString("section at byte %1 is %2 bytes (max %3)").arg(off).arg(i.size).arg(kMaxPrivateSection));
    if (d.size() - off < i.size) return fail(QString("truncated section at byte %1 (%2 of %3 bytes)").arg(off).arg(d.size() - off).arg(i.size));
    if (i.longSyntax) {
        if (i.size < 12) return fail(QString("long section at byte %1 is only %2 bytes").arg(off).arg(i.size));
        i.tableIdExt = (p[3] << 8) | p[4]; i.version = (p[5] >> 1) & 0x1F; i.current = p[5] & 1;
        i.sectionNumber = p[6]; i.lastSectionNumber = p[7];
    }
    if (info) *info = i;
    return true;
}

bool hasCrc(const QByteArray& s) { return s.size() >= 7 && ((quint8(s[1]) & 0x80) || quint8(s[0]) == 0xFC); }

bool crcValid(const QByteArray& s) {
    if (!hasCrc(s)) return true;
    return crc32(s.constData(), s.size()) == 0;           // the CRC over data + CRC of an intact section is 0
}

void fixCrc(QByteArray& s) {
    if (!hasCrc(s)) return;
    const quint32 c = crc32(s.constData(), s.size() - 4);
    char* e = s.data() + s.size() - 4;
    e[0] = char(c >> 24); e[1] = char(c >> 16); e[2] = char(c >> 8); e[3] = char(c);
}

QList<QByteArray> splitSections(const QByteArray& bin, QString* error) {
    QList<QByteArray> out;
    int off = 0;
    while (off < bin.size()) {
        SectionInfo i;
        if (!sectionInfo(bin, off, &i, error)) return {};
        const QByteArray s = bin.mid(off, i.size);
        if (!crcValid(s)) { if (error) *error = QString("bad CRC32 in section 0x%1 at byte %2").arg(i.tableId, 2, 16, QChar('0')).arg(off); return {}; }
        out.append(s);
        off += i.size;
    }
    return out;
}

QByteArray Packetizer::packetize(const QList<QByteArray>& all, Mode mode) {
    QList<QByteArray> secs;                               // implicitly shared copies; empty entries are not sections
    for (const QByteArray& s : all) if (!s.isEmpty()) secs.append(s);
    QByteArray out;
    const int n = int(secs.size());
    int i = 0, off = 0;                                   // current section, bytes of it already sent
    while (i < n) {
        char p[kPacketSize];
        std::memset(p, 0xFF, sizeof(p));
        // A section starts in this packet when the current one starts here, or when its tail plus a pointer_field leaves
        // room for the whole header of the next one (TSDuck never splits a section header across packets).
        const int tail = int(secs[i].size()) - off;
        bool pusi = off == 0;
        int pointer = 0;
        if (!pusi && mode == Packed && i + 1 < n && tail + 1 + headerSize(secs[i + 1]) <= kPacketSize - 4) { pusi = true; pointer = tail; }
        p[0] = 0x47;
        p[1] = char((pusi ? 0x40 : 0x00) | ((m_pid >> 8) & 0x1F));
        p[2] = char(m_pid & 0xFF);
        p[3] = char(0x10 | m_cc);
        int pos = 4;
        if (pusi) p[pos++] = char(pointer);
        while (pos < kPacketSize && i < n) {
            const QByteArray& cur = secs[i];
            const int take = qMin(kPacketSize - pos, int(cur.size()) - off);
            std::memcpy(p + pos, cur.constData() + off, size_t(take));
            pos += take; off += take;
            if (off < cur.size()) break;                  // packet full, section continues in the next one
            ++i; off = 0;
            // Another section may follow in this packet only behind a pointer_field (PUSI) and with its header whole.
            if (i >= n || mode != Packed || !pusi || kPacketSize - pos < headerSize(secs[i])) break;
        }
        out.append(p, kPacketSize);
        m_cc = (m_cc + 1) & 0x0F;
    }
    return out;
}

}   // namespace tskit
