#include "tsanalyzer.h"

#include "psip.h"
#include "scte35bin.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace tskit {

namespace {

constexpr qint64 kPcrWrap = (Q_INT64_C(1) << 33) * 300;  // PCR = 33-bit base * 300 + extension
constexpr double kPcrHz = 27000000.0;
constexpr int kMaxTablesText = 8 * 1024 * 1024;          // a long UDP run keeps counting, the text stops growing

// ============================================================================================ text helpers (TSDuck style)
QString hex(quint64 v, int digits) { return QString::number(v, 16).toUpper().rightJustified(digits, QLatin1Char('0')); }
QString hd(quint64 v, int digits) { return QStringLiteral("0x%1 (%2)").arg(hex(v, digits)).arg(v); }   // 0x0019 (25)
QString yesNo(bool b) { return b ? QStringLiteral("yes") : QStringLiteral("no"); }
QString trueFalse(bool b) { return b ? QStringLiteral("true") : QStringLiteral("false"); }
QString num(qint64 v) { return QString::number(v); }
QString sep(qint64 v) {                                   // 2,700,000
    QString s = QString::number(v < 0 ? -v : v);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(i, QLatin1Char(','));
    return v < 0 ? QLatin1Char('-') + s : s;
}
QString fixed(double v, int decimals) { return QString::number(v, 'f', decimals); }

struct Lines {
    QString out;
    bool dvb = true;                                      // TSDuck decodes DVB descriptors only once a DVB table was seen
    void add(int indent, const QString& s) { out += QString(indent, QLatin1Char(' ')); out += s; out += QLatin1Char('\n'); }
};

// TSDuck's hex dump: offset, hex bytes, ASCII; bytes per line from a 78-column budget (even), trailing blanks trimmed.
void dump(Lines& L, int indent, const QByteArray& d) {
    int bpl = (78 - indent - 9) / 4;
    if (bpl > 1) bpl &= ~1;
    bpl = qMax(1, bpl);
    for (int off = 0; off < d.size(); off += bpl) {
        QString hx, asc;
        const int n = qMin(bpl, int(d.size()) - off);
        for (int i = 0; i < n; ++i) {
            const quint8 c = quint8(d[off + i]);
            hx += hex(c, 2); hx += QLatin1Char(' ');
            asc += c >= 0x20 && c < 0x7F ? QChar(c) : QChar('.');
        }
        QString line = hex(off, 4) + QStringLiteral(":  ") + hx.leftJustified(bpl * 3 + 1, QLatin1Char(' ')) + asc;
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        L.add(indent, line);
    }
}

QString spacedHex(const QByteArray& d) {
    QStringList b;
    for (char c : d) b << hex(quint8(c), 2);
    return b.join(QLatin1Char(' '));
}

QString printable4(const QByteArray& d, int pos) {
    QString s;
    for (int i = pos; i < pos + 4 && i < d.size(); ++i) { const quint8 c = quint8(d[i]); s += c >= 0x20 && c < 0x7F ? QChar(c) : QChar('.'); }
    return s;
}

// ISO 639 code: 3 Latin-1 bytes, NULs dropped (TSDuck shows a zero code as "").
QString lang3(const QByteArray& d, int pos) {
    QString s;
    for (int i = pos; i < pos + 3 && i < d.size(); ++i) if (d[i] != '\0') s += QChar(quint8(d[i]));
    return s;
}

// DVB SI string (EN 300 468 Annex A): 0x15 = UTF-8, 0x11 = UCS-2, other selectors / no selector read as Latin-1.
QString dvbString(const QByteArray& b) {
    if (b.isEmpty()) return {};
    const quint8 c0 = quint8(b[0]);
    if (c0 == 0x15) return QString::fromUtf8(b.mid(1));
    if (c0 == 0x11) { QString s; for (int i = 1; i + 1 < b.size(); i += 2) s += QChar(char16_t((quint8(b[i]) << 8) | quint8(b[i + 1]))); return s; }
    if (c0 == 0x10) return QString::fromLatin1(b.mid(3));
    if (c0 < 0x20) return QString::fromLatin1(b.mid(1));
    return QString::fromLatin1(b);
}

QDateTime gpsEpoch() { return QDateTime(QDate(1980, 1, 6), QTime(0, 0), QTimeZone::utc()); }
QString gpsTimeText(qint64 seconds) { return gpsEpoch().addSecs(seconds).toString(QStringLiteral("yyyy/MM/dd hh:mm:ss")); }

QString ptsText(quint64 v) {                              // SCTE 35 times as TSDuck prints them
    if (v == 0) return QStringLiteral("0x000000000");
    return QStringLiteral("0x%1 (%2, %3 ms)").arg(hex(v, 9), sep(qint64(v)), sep(qint64(v / 90)));
}

// Big-endian reader over [p, end) with a sticky failure flag (reads past the end yield 0 and set `bad`).
struct Rd {
    const QByteArray& d; int p; int end; bool bad = false;
    bool has(int n) const { return p + n <= end; }
    int u8() { if (p + 1 > end) { bad = true; p = end; return 0; } return quint8(d[p++]); }
    int u16() { const int a = u8(); return (a << 8) | u8(); }
    quint32 u24() { const quint32 a = quint32(u16()); return (a << 8) | quint32(u8()); }
    quint32 u32() { const quint32 a = quint32(u16()); return (a << 16) | quint32(u16()); }
    QByteArray bytes(int n) { if (n < 0 || p + n > end) { bad = true; n = qMax(0, end - p); } const QByteArray b = d.mid(p, n); p += n; return b; }
};

// ============================================================================================ names (TSDuck 3.41 tsduck.dtv.names)
QString descriptorName(int tag) {
    switch (tag) {
    case 0x02: return "Video Stream";            case 0x03: return "Audio Stream";
    case 0x04: return "Hierarchy";               case 0x05: return "Registration";
    case 0x06: return "Data Stream Alignment";   case 0x07: return "Target Background Grid";
    case 0x08: return "Video Window";            case 0x09: return "CA";
    case 0x0A: return "ISO-639 Language";        case 0x0B: return "System Clock";
    case 0x0C: return "Multiplex Buffer Utilization"; case 0x0D: return "Copyright";
    case 0x0E: return "Maximum Bitrate";         case 0x0F: return "Private Data Indicator";
    case 0x10: return "Smoothing Buffer";        case 0x11: return "STD";
    case 0x12: return "IBP";                     case 0x13: return "DSM-CC Carousel Identifier";
    case 0x14: return "DSM-CC Association Tag";  case 0x15: return "DSM-CC Deferred Association Tags";
    case 0x17: return "DSM-CC NPT Reference";    case 0x18: return "DSM-CC NPT Endpoint";
    case 0x19: return "DSM-CC Stream Mode";      case 0x1A: return "DSM-CC Stream Event";
    case 0x1B: return "MPEG-4 Video Stream";     case 0x1C: return "MPEG-4 Audio Stream";
    case 0x1D: return "IOD";                     case 0x1E: return "SL";
    case 0x1F: return "FMC";                     case 0x20: return "External ES id";
    case 0x21: return "MuxCode";                 case 0x22: return "FmxBufferSize";
    case 0x23: return "MultiplexBuffer";         case 0x24: return "Content Labelling";
    case 0x25: return "Metadata Pointer";        case 0x26: return "Metadata";
    case 0x27: return "Metadata STD";            case 0x28: return "AVC Video";
    case 0x29: return "MPEG-2 IPMP";             case 0x2A: return "AVC Timing and HRD";
    case 0x2B: return "MPEG-2 AAC Audio";        case 0x2C: return "M4Mux Timing";
    case 0x2D: return "MPEG-4 Text";             case 0x2E: return "MPEG-4 Audio Extension";
    case 0x2F: return "Auxiliary Video Stream";  case 0x30: return "SVC Extension";
    case 0x31: return "MVC Extension";           case 0x32: return "JPEG-2000 Video";
    case 0x33: return "MVC Operation Point";     case 0x34: return "MPEG-2 Stereoscopic Video Format";
    case 0x35: return "Stereoscopic Program Info"; case 0x36: return "Stereoscopic Video Info";
    case 0x37: return "Transport Profile";       case 0x38: return "HEVC Video";
    case 0x39: return "VVC Video (H.222.0)";     case 0x3A: return "EVC Video (H.222.0)";
    case 0x3F: return "MPEG-2 Extension";
    case 0x40: return "Network Name";            case 0x41: return "Service List";
    case 0x42: return "Stuffing";                case 0x43: return "Satellite Delivery System";
    case 0x44: return "Cable Delivery System";   case 0x45: return "VBI Data";
    case 0x46: return "VBI Teletext";            case 0x47: return "Bouquet Name";
    case 0x48: return "Service";                 case 0x49: return "Country Availability";
    case 0x4A: return "Linkage";                 case 0x4B: return "NVOD Reference";
    case 0x4C: return "Time Shifted Service";    case 0x4D: return "Short Event";
    case 0x4E: return "Extended Event";          case 0x4F: return "Time Shifted Event";
    case 0x50: return "Component";               case 0x51: return "Mosaic";
    case 0x52: return "Stream Identifier";       case 0x53: return "CA Identifier";
    case 0x54: return "Content";                 case 0x55: return "Parental Rating";
    case 0x56: return "Teletext";                case 0x57: return "Telephone";
    case 0x58: return "Local Time Offset";       case 0x59: return "Subtitling";
    case 0x5A: return "Terrestrial Delivery System"; case 0x5B: return "Multilingual Network Name";
    case 0x5C: return "Multilingual Bouquet Name"; case 0x5D: return "Multilingual Service Name";
    case 0x5E: return "Multilingual Component";  case 0x5F: return "Private Data Specifier";
    case 0x60: return "Service Move";            case 0x61: return "Short Smoothing Buffer";
    case 0x62: return "Frequency List";          case 0x63: return "Partial Transport Stream";
    case 0x64: return "Data Broadcast";          case 0x65: return "Scrambling";
    case 0x66: return "Data Broadcast Id";       case 0x67: return "Transport Stream";
    case 0x68: return "DSNG";                    case 0x69: return "PDC";
    case 0x6A: return "AC-3";                    case 0x6B: return "Ancillary Data";
    case 0x6C: return "Cell List";               case 0x6D: return "Cell Frequency Link";
    case 0x6E: return "Announcement Support";    case 0x6F: return "Application Signalling";
    case 0x70: return "Adaptation Field Data";   case 0x71: return "Service Identifier";
    case 0x72: return "Service Availability";    case 0x73: return "Default Authority";
    case 0x74: return "Related Content";         case 0x75: return "TVA Id";
    case 0x76: return "Content Identifier";      case 0x77: return "Time Slice FEC Identifier";
    case 0x78: return "ECM Repetition Rate";     case 0x79: return "S2 Satellite Delivery System";
    case 0x7A: return "Enhanced AC-3";           case 0x7B: return "DTS";
    case 0x7C: return "AAC";                     case 0x7D: return "XAIT Location";
    case 0x7E: return "FTA Content Management";  case 0x7F: return "Extension Descriptor";
    case 0x80: return "ATSC Stuffing";           case 0x81: return "ATSC AC-3 Audio";
    case 0x85: return "ATSC Program Identifier"; case 0x86: return "ATSC Caption Service";
    case 0x87: return "ATSC Content Advisory";   case 0x8D: return "ATSC Parameterized Service";
    case 0xA0: return "ATSC Extended Channel Name"; case 0xA1: return "ATSC Service Location";
    case 0xA2: return "ATSC Time-Shifted Service"; case 0xA3: return "ATSC Component Name";
    case 0xA4: return "ATSC Data Service";       case 0xA5: return "ATSC PID Count";
    case 0xA6: return "ATSC Download";           case 0xA7: return "ATSC Multiprotocol Encapsulation";
    case 0xA8: return "ATSC DCC Departing Request"; case 0xA9: return "ATSC DCC Arriving Request";
    case 0xAA: return "ATSC Redistribution Control"; case 0xAB: return "ATSC Genre";
    case 0xAD: return "ATSC Private Information"; case 0xB4: return "ATSC Module Link";
    case 0xB5: return "ATSC CRC32";              case 0xB8: return "ATSC Group Link";
    case 0xBB: return "ATSC Component List";     case 0xCC: return "ATSC E-AC-3 Audio Stream";
    case 0xFF: return "Forbidden Descriptor Id 0xFF";
    default: return "unknown";
    }
}

QString dvbServiceTypeName(int t) {
    switch (t) {
    case 0x00: return "Undefined";                     case 0x01: return "Digital television service";
    case 0x02: return "Digital radio sound service";   case 0x03: return "Teletext service";
    case 0x04: return "NVOD reference service";        case 0x05: return "NVOD time-shifted service";
    case 0x06: return "Mosaic service";                case 0x07: return "PAL-coded signal";
    case 0x08: return "SECAM-coded signal";            case 0x09: return "D/D2-MAC";
    case 0x0A: return "FM radio";                      case 0x0B: return "NTSC-coded signal";
    case 0x0C: return "Data broadcast service";        case 0x0D: return "Common Interface usage";
    case 0x0E: return "RCS map";                       case 0x0F: return "RCS FLS";
    case 0x10: return "DVB-MHP service";               case 0x11: return "MPEG-2 HD digital television service";
    case 0x16: return "Advanced codec SD digital television service";
    case 0x17: return "Advanced codec SD NVOD time-shifted service";
    case 0x18: return "Advanced codec SD NVOD reference service";
    case 0x19: return "Advanced codec HD digital television service";
    case 0x1A: return "Advanced codec HD NVOD time-shifted service";
    case 0x1B: return "Advanced codec HD NVOD reference service";
    case 0x1C: return "H.264/AVC frame compatible plano-stereoscopic HD digital television service";
    case 0x1D: return "H.264/AVC frame compatible plano-stereoscopic HD NVOD time-shifted";
    case 0x1E: return "H.264/AVC frame compatible plano-stereoscopic HD NVOD reference service";
    case 0x1F: return "HEVC digital television service";
    case 0x20: return "HEVC UHD up to 2160p120 or 4320p60";
    case 0x21: return "VVC";                           case 0x22: return "AVS3";
    default: return "unknown";
    }
}

QString atscServiceTypeName(int t) {
    switch (t) {
    case 0x01: return "Analog Television";             case 0x02: return "ATSC Digital Television";
    case 0x03: return "ATSC Audio";                    case 0x04: return "ATSC Data Only Service";
    case 0x05: return "ATSC Software Download Service";
    default: return "unknown";
    }
}

QString modulationName(int m) {
    switch (m) {
    case 0x01: return "Analog";                        case 0x02: return "SCTE mode 1 (64-QAM)";
    case 0x03: return "SCTE mode 2 (256-QAM)";         case 0x04: return "ATSC 8-VSB";
    case 0x05: return "ATSC 16-VSB";
    default: return "unknown";
    }
}

QString runningStatusName(int s) {
    static const char* n[] = {"undefined", "not running", "starting", "pausing", "running", "off-air"};
    return s >= 0 && s < 6 ? QString::fromLatin1(n[s]) : QStringLiteral("unknown");
}

QString audioTypeName(int t) {
    static const char* n[] = {"undefined", "clean effects", "hearing impaired", "visual impaired commentary"};
    return t >= 0 && t < 4 ? QString::fromLatin1(n[t]) : QStringLiteral("unknown");
}

QString spliceCommandName(int c) {
    switch (c) {
    case 0x00: return "SpliceNull";   case 0x04: return "SpliceSchedule"; case 0x05: return "SpliceInsert";
    case 0x06: return "TimeSignal";   case 0x07: return "BandwidthReservation"; case 0xFF: return "PrivateCommand";
    default: return "unknown";
    }
}

QString mgtTypeName(int t) {
    switch (t) {
    case 0x0000: return "TVCT-current"; case 0x0001: return "TVCT-next";
    case 0x0002: return "CVCT-current"; case 0x0003: return "CVCT-next";
    case 0x0004: return "ETT";          case 0x0005: return "DCCSCT";
    case 0x0006: return "LTST";
    default: break;
    }
    if (t >= 0x0100 && t <= 0x017F) return QStringLiteral("EIT-%1").arg(t - 0x0100);
    if (t >= 0x0200 && t <= 0x027F) return QStringLiteral("ETT-%1").arg(t - 0x0200);
    if (t >= 0x0301 && t <= 0x03FF) return QStringLiteral("RRT-%1").arg(t - 0x0300);
    if (t >= 0x1000 && t <= 0x10FF) return QStringLiteral("AEIT-%1").arg(t - 0x1000);
    if (t >= 0x1100 && t <= 0x11FF) return QStringLiteral("AETT-%1").arg(t - 0x1100);
    if (t >= 0x1200 && t <= 0x127F) return QStringLiteral("EET-DET-%1").arg(t - 0x1200);
    if (t >= 0x1300 && t <= 0x137F) return QStringLiteral("DET-%1").arg(t - 0x1300);
    if (t >= 0x1400 && t <= 0x14FF) return QStringLiteral("DCCT-%1").arg(t - 0x1400);
    return QString::number(t);
}

// ============================================================================================ multiple_string_structure (A/65 6.10)
// Uncompressed segments: modes 0x00..0x33 are the Unicode page `mode`, 0x3F is UTF-16BE. Huffman text (compression 1/2)
// is not decoded (PlayPlay never sends it); its bytes are skipped.
void multiString(Lines& L, int indent, const QString& title, const QByteArray& d, int pos, int end) {
    end = qMin(end, int(d.size()));
    const int n = pos < end ? quint8(d[pos++]) : 0;
    L.add(indent, title + QStringLiteral("Number of strings: %1").arg(n));
    for (int i = 0; i < n && pos + 4 <= end; ++i) {
        const QString lang = lang3(d, pos);
        pos += 3;
        const int segs = quint8(d[pos++]);
        QString text;
        for (int k = 0; k < segs && pos + 3 <= end; ++k) {
            const int comp = quint8(d[pos]), mode = quint8(d[pos + 1]);
            int nb = quint8(d[pos + 2]);
            pos += 3;
            nb = qMin(nb, end - pos);
            if (comp == 0 && mode == 0x3F) {
                for (int j = 0; j + 1 < nb; j += 2) text += QChar(char16_t((quint8(d[pos + j]) << 8) | quint8(d[pos + j + 1])));
            } else if (comp == 0 && mode <= 0x33) {
                for (int j = 0; j < nb; ++j) text += QChar(char16_t((mode << 8) | quint8(d[pos + j])));
            }
            pos += nb;
        }
        L.add(indent + 2, QStringLiteral("Language: \"") + lang + QStringLiteral("\", text: \"") + text + QLatin1Char('"'));
    }
}

// ============================================================================================ descriptors
void descriptorBody(Lines& L, int ind, int tag, const QByteArray& p) {
    Rd r{p, 0, int(p.size())};
    switch (tag) {
    case 0x05:                                            // registration
        if (p.size() >= 4) {
            const quint32 id = r.u32();
            L.add(ind, QStringLiteral("Format identifier: 0x%1 (\"%2\")").arg(hex(id, 8), printable4(p, 0)));
            if (p.size() > 4) L.add(ind, QStringLiteral("Additional identification info (%1 bytes): %2").arg(p.size() - 4).arg(spacedHex(p.mid(4))));
            return;
        }
        break;
    case 0x0A:                                            // ISO_639_language
        if (p.size() % 4 == 0) {
            for (int i = 0; i + 4 <= p.size(); i += 4) {
                const int t = quint8(p[i + 3]);
                L.add(ind, QStringLiteral("Language: ") + lang3(p, i) + QStringLiteral(", Type: 0x%1 (%2)").arg(hex(t, 2), audioTypeName(t)));
            }
            return;
        }
        break;
    case 0x48: {                                          // DVB service
        const int st = r.u8();
        const QByteArray prov = r.bytes(r.u8());
        const QByteArray name = r.bytes(r.u8());
        if (r.bad) break;
        L.add(ind, QStringLiteral("Service type: 0x%1 (%2)").arg(hex(st, 2), dvbServiceTypeName(st)));
        L.add(ind, QStringLiteral("Service: \"") + dvbString(name) + QStringLiteral("\", Provider: \"") + dvbString(prov) + QLatin1Char('"'));
        return;
    }
    case 0x52:                                            // stream_identifier
        if (p.size() == 1) { L.add(ind, QStringLiteral("Component tag: ") + hd(quint8(p[0]), 2)); return; }
        break;
    case 0xA0:                                            // extended_channel_name
        multiString(L, ind, QStringLiteral("Long channel name: "), p, 0, int(p.size()));
        return;
    case 0xA1: {                                          // service_location
        if (p.size() < 3) break;
        const int pcr = r.u16() & 0x1FFF, n = r.u8();
        L.add(ind, QStringLiteral("PCR PID: ") + (pcr == kNullPid ? QStringLiteral("none") : hd(pcr, 4)) + QStringLiteral(", number of elements: %1").arg(n));
        for (int i = 0; i < n && r.has(6); ++i) {
            const int st = r.u8(), pid = r.u16() & 0x1FFF;
            const QString lg = lang3(p, r.p);
            r.p += 3;
            // TSDuck 3.41 names this stream_type from the DVB service_type table; reproduced so the text matches.
            L.add(ind, QStringLiteral("- PID: ") + hd(pid, 4) + QStringLiteral(", language: \"") + lg + QStringLiteral("\", type: 0x%1 (%2)").arg(hex(st, 2), dvbServiceTypeName(st)));
        }
        return;
    }
    case 0x86: {                                          // caption_service
        if (p.isEmpty()) break;
        const int n = r.u8() & 0x1F;
        L.add(ind, QStringLiteral("Number of services: %1").arg(n));
        for (int i = 0; i < n && r.has(6); ++i) {
            const QString lg = lang3(p, r.p);
            r.p += 3;
            const int b = r.u8();
            const bool digital = b & 0x80;
            const int b2 = r.u8();
            r.u8();
            QString s = QStringLiteral("- Language: \"") + lg + QStringLiteral("\", digital: ") + trueFalse(digital);
            if (digital) s += QStringLiteral(", service: ") + hd(b & 0x3F, 2);
            else s += QStringLiteral(", line 21: ") + trueFalse(b & 0x01);
            s += QStringLiteral(", easy reader: ") + trueFalse(b2 & 0x80) + QStringLiteral(", wide: ") + trueFalse(b2 & 0x40);
            L.add(ind, s);
        }
        return;
    }
    case 0x87: {                                          // content_advisory
        if (p.isEmpty()) break;
        const int regions = r.u8() & 0x3F;
        L.add(ind, QStringLiteral("Number of regions: %1").arg(regions));
        for (int g = 0; g < regions && r.has(2); ++g) {
            const int region = r.u8(), dims = r.u8();
            L.add(ind, QStringLiteral("- Rating region: ") + hd(region, 2) + QStringLiteral(", number of dimensions: %1").arg(dims));
            for (int k = 0; k < dims && r.has(2); ++k) {
                const int j = r.u8(), v = r.u8() & 0x0F;
                L.add(ind + 4, QStringLiteral("Rating dimension j: ") + hd(j, 2) + QStringLiteral(", rating value: %1").arg(v));
            }
            const int dl = r.u8();
            if (dl > 0) multiString(L, ind + 2, QStringLiteral("Rating description: "), p, r.p, r.p + dl);
            r.p = qMin(r.end, r.p + dl);
        }
        return;
    }
    default:
        break;
    }
    dump(L, ind, p);
}

void descriptorLoop(Lines& L, int indent, const QByteArray& d, int pos, int len) {
    const int end = qMin(pos + len, int(d.size()));
    for (int i = 0; pos + 2 <= end; ++i) {
        const int tag = quint8(d[pos]), l = quint8(d[pos + 1]);
        const bool known = L.dvb || tag < 0x40 || tag > 0x7F;
        L.add(indent, QStringLiteral("- Descriptor %1: %2 (0x%3, %4), %5 bytes").arg(i).arg(known ? descriptorName(tag) : QStringLiteral("unknown"), hex(tag, 2)).arg(tag).arg(l));
        if (known) descriptorBody(L, indent + 2, tag, d.mid(pos + 2, qMin(l, end - pos - 2)));
        else dump(L, indent + 2, d.mid(pos + 2, qMin(l, end - pos - 2)));
        pos += 2 + l;
    }
}

// ============================================================================================ table bodies (indent 4)
QString patLine(const QString& label, int v) { return label.leftJustified(8) + QLatin1Char(' ') + QString::number(v).rightJustified(5) + QStringLiteral(" (0x") + hex(v, 4) + QLatin1Char(')'); }

void bodyPat(Lines& L, const QByteArray& s, const SectionInfo& i) {
    L.add(4, patLine(QStringLiteral("TS id:"), i.tableIdExt));
    for (int p = 8; p + 4 <= i.size - 4; p += 4) {
        const int prog = (quint8(s[p]) << 8) | quint8(s[p + 1]), pid = ((quint8(s[p + 2]) & 0x1F) << 8) | quint8(s[p + 3]);
        L.add(4, patLine(prog == 0 ? QStringLiteral("NIT:") : QStringLiteral("Program:"), prog) + QStringLiteral("  PID: ") + QString::number(pid).rightJustified(4) + QStringLiteral(" (0x") + hex(pid, 4) + QLatin1Char(')'));
    }
}

void bodyPmt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    const int pcr = r.u16() & 0x1FFF, pil = r.u16() & 0x0FFF;
    L.add(4, QStringLiteral("Program: ") + hd(i.tableIdExt, 4) + QStringLiteral(", PCR PID: ") + (pcr == kNullPid ? QStringLiteral("none") : hd(pcr, 4)));
    if (pil > 0) { L.add(4, QStringLiteral("Program information:")); descriptorLoop(L, 4, s, r.p, qMin(pil, r.end - r.p)); }
    r.p = qMin(r.end, r.p + pil);
    while (r.has(5)) {
        const int st = r.u8(), pid = r.u16() & 0x1FFF, esl = r.u16() & 0x0FFF;
        L.add(4, QStringLiteral("Elementary stream: type 0x%1 (%2), PID: ").arg(hex(st, 2), streamTypeName(st)) + hd(pid, 4));
        descriptorLoop(L, 4, s, r.p, qMin(esl, r.end - r.p));
        r.p = qMin(r.end, r.p + esl);
    }
}

void bodySdt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("Transport Stream Id: ") + hd(i.tableIdExt, 4));
    const int onid = r.u16();
    r.u8();
    L.add(4, QStringLiteral("Original Network Id: ") + hd(onid, 4));
    while (r.has(5)) {
        const int sid = r.u16(), b = r.u8(), c = r.u16();
        L.add(4, QStringLiteral("Service Id: ") + hd(sid, 4) + QStringLiteral(", EITs: ") + yesNo(b & 0x02) + QStringLiteral(", EITp/f: ") + yesNo(b & 0x01)
                 + QStringLiteral(", CA mode: ") + ((c & 0x1000) ? QStringLiteral("controlled") : QStringLiteral("free")));
        L.add(4, QStringLiteral("Running status: ") + runningStatusName((c >> 13) & 0x07));
        const int dl = c & 0x0FFF;
        descriptorLoop(L, 4, s, r.p, qMin(dl, r.end - r.p));
        r.p = qMin(r.end, r.p + dl);
    }
}

void bodyMgt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    const int protocol = r.u8(), n = r.u16();
    L.add(4, QStringLiteral("Protocol version: %1, number of table types: %2").arg(protocol).arg(n));
    for (int k = 0; k < n && r.has(11); ++k) {
        const int type = r.u16(), pid = r.u16() & 0x1FFF, version = r.u8() & 0x1F;
        const quint32 nb = r.u32();
        const int dl = r.u16() & 0x0FFF;
        L.add(4, QStringLiteral("- Table type: %1 (0x%2)").arg(mgtTypeName(type), hex(type, 4)));
        L.add(6, QStringLiteral("PID: ") + hd(pid, 4) + QStringLiteral(", version: %1, size: %2 bytes").arg(version).arg(nb));
        descriptorLoop(L, 6, s, r.p, qMin(dl, r.end - r.p));
        r.p = qMin(r.end, r.p + dl);
    }
    if (r.has(2)) {
        const int gl = r.u16() & 0x0FFF;
        if (gl > 0) { L.add(4, QStringLiteral("Global descriptors:")); descriptorLoop(L, 4, s, r.p, qMin(gl, r.end - r.p)); }
    }
}

void bodyVct(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("Transport stream id: ") + hd(i.tableIdExt, 4));
    const int protocol = r.u8(), n = r.u8();
    L.add(4, QStringLiteral("Protocol version: %1, number of channels: %2").arg(protocol).arg(n));
    for (int k = 0; k < n && r.has(32); ++k) {
        QString name;
        for (int c = 0; c < 7; ++c) { const int u = r.u16(); if (u == 0) { r.p += 2 * (6 - c); break; } name += QChar(char16_t(u)); }
        const quint32 a = r.u32();
        const int major = int((a >> 18) & 0x3FF), minor = int((a >> 8) & 0x3FF), mod = int(a & 0xFF);
        const quint32 freq = r.u32();
        const int tsid = r.u16(), prog = r.u16(), f = r.u16(), src = r.u16(), dl = r.u16() & 0x03FF;
        L.add(4, QStringLiteral("- Channel %1.%2, short name: \"").arg(major).arg(minor) + name + QLatin1Char('"'));
        L.add(6, QStringLiteral("Modulation: ") + modulationName(mod) + QStringLiteral(", frequency: ") + sep(freq));
        L.add(6, QStringLiteral("TS id: ") + hd(tsid, 4) + QStringLiteral(", program number: ") + hd(prog, 4));
        L.add(6, QStringLiteral("ETM location: %1, access controlled: %2").arg((f >> 14) & 0x03).arg(yesNo(f & 0x2000)));
        L.add(6, QStringLiteral("Hidden: ") + yesNo(f & 0x1000) + QStringLiteral(", hide guide: ") + yesNo(f & 0x0200));
        L.add(6, QStringLiteral("Service type: ") + atscServiceTypeName(f & 0x3F) + QStringLiteral(", source id: ") + hd(src, 4));
        descriptorLoop(L, 6, s, r.p, qMin(dl, r.end - r.p));
        r.p = qMin(r.end, r.p + dl);
    }
    if (r.has(2)) {
        const int al = r.u16() & 0x03FF;
        if (al > 0) { L.add(4, QStringLiteral("Additional descriptors:")); descriptorLoop(L, 4, s, r.p, qMin(al, r.end - r.p)); }
    }
}

void bodyRrt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("Rating region: ") + hd(i.tableIdExt & 0xFF, 2));
    L.add(4, QStringLiteral("Protocol version: %1").arg(r.u8()));
    auto ms = [&](int indent, const QString& title) { const int len = r.u8(); multiString(L, indent, title, s, r.p, qMin(r.end, r.p + len)); r.p = qMin(r.end, r.p + len); };
    ms(4, QStringLiteral("Rating region name: "));
    const int dims = r.u8();
    L.add(4, QStringLiteral("Number of dimensions: %1").arg(dims));
    for (int d = 0; d < dims && !r.bad && r.has(2); ++d) {
        L.add(4, QStringLiteral("- Dimension %1").arg(d));
        ms(6, QStringLiteral("Dimension name: "));
        const int b = r.u8(), values = b & 0x0F;
        L.add(6, QStringLiteral("Graduated scale: %1, number of rating values: %2").arg(trueFalse(b & 0x10)).arg(values));
        for (int v = 0; v < values && !r.bad && r.has(2); ++v) {
            ms(6, QStringLiteral("- Abbreviated rating value: "));
            ms(6, QStringLiteral("  Rating value: "));
        }
    }
    if (r.has(2)) { const int dl = r.u16() & 0x03FF; descriptorLoop(L, 4, s, r.p, qMin(dl, r.end - r.p)); }
}

void bodyEit(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("Source Id: ") + hd(i.tableIdExt, 4));
    const int protocol = r.u8(), n = r.u8();
    L.add(4, QStringLiteral("Protocol version: %1, number of events: %2").arg(protocol).arg(n));
    for (int k = 0; k < n && r.has(10); ++k) {
        const int id = r.u16() & 0x3FFF;
        const quint32 start = r.u32(), b = r.u24();
        const int tl = r.u8();
        L.add(4, QStringLiteral("- Event Id: ") + hd(id, 4));
        L.add(6, QStringLiteral("Start UTC: ") + gpsTimeText(start));
        L.add(6, QStringLiteral("ETM location: %1").arg((b >> 20) & 0x03));
        L.add(6, QStringLiteral("Duration: %1 seconds").arg(b & 0xFFFFF));
        if (tl > 0) multiString(L, 6, QStringLiteral("Title text: "), s, r.p, qMin(r.end, r.p + tl));
        r.p = qMin(r.end, r.p + tl);
        const int dl = r.u16() & 0x0FFF;
        descriptorLoop(L, 6, s, r.p, qMin(dl, r.end - r.p));
        r.p = qMin(r.end, r.p + dl);
    }
}

void bodyEtt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("ETT table id extension: ") + hd(i.tableIdExt, 4));
    const int protocol = r.u8();
    const quint32 etm = r.u32();
    L.add(4, QStringLiteral("Protocol version: %1, ETM id: ").arg(protocol) + hd(etm, 8));
    multiString(L, 4, QStringLiteral("Extended text message: "), s, r.p, r.end);
}

void bodyStt(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 8, i.size - 4};
    L.add(4, QStringLiteral("Protocol version: %1").arg(r.u8()));
    const quint32 st = r.u32();
    const int off = r.u8(), ds = r.u16();
    L.add(4, QStringLiteral("System time: ") + hd(st, 8) + QStringLiteral(", GPS-UTC offset: ") + hd(off, 2));
    L.add(4, QStringLiteral("Corresponding UTC time: ") + (st == 0 ? QStringLiteral("none") : gpsTimeText(qint64(st) - off)));
    L.add(4, QStringLiteral("Daylight saving time: %1, next switch day: %2, hour: %3").arg(yesNo(ds & 0x8000)).arg((ds >> 8) & 0x1F).arg(ds & 0xFF));
    descriptorLoop(L, 4, s, r.p, r.end - r.p);
}

QString spliceTime(Rd& r) {
    const int b = r.u8();
    if (!(b & 0x80)) return QStringLiteral("unset");
    return ptsText((quint64(b & 1) << 32) | r.u32());
}

void bodyScte35(Lines& L, const QByteArray& s, const SectionInfo& i) {
    Rd r{s, 3, i.size - 4};
    L.add(4, QStringLiteral("Protocol version: %1").arg(r.u8()));
    const int e = r.u8();
    const quint64 adj = (quint64(e & 1) << 32) | r.u32();
    L.add(4, QStringLiteral("Encryption: ") + ((e & 0x80) ? QStringLiteral("algorithm %1").arg((e >> 1) & 0x3F) : QStringLiteral("none")));
    L.add(4, QStringLiteral("PTS adjustment: ") + ptsText(adj));
    const int cw = r.u8();
    const quint32 tl = r.u24();
    const int tier = int(tl >> 12), cmdLen = int(tl & 0xFFF), cmd = r.u8();
    L.add(4, QStringLiteral("CW index: ") + hd(cw, 2) + QStringLiteral(", tier: ") + hd(tier, 3));
    L.add(4, QStringLiteral("Command type: 0x%1 (%2), size: %3 bytes").arg(hex(cmd, 2), spliceCommandName(cmd)).arg(cmdLen));
    const int cmdStart = r.p;
    if (e & 0x80) {
        r.p = qMin(r.end, cmdStart + cmdLen);             // encrypted command: not decoded
    } else if (cmd == 0x05) {
        const quint32 id = r.u32();
        const bool cancel = r.u8() & 0x80;
        L.add(4, QStringLiteral("Splice event id: ") + hd(id, 8) + QStringLiteral(", cancel: %1").arg(cancel ? 1 : 0));
        if (!cancel) {
            const int f = r.u8();
            const bool out = f & 0x80, program = f & 0x40, dur = f & 0x20, imm = f & 0x10;
            L.add(4, QStringLiteral("Out of network: ") + yesNo(out) + QStringLiteral(", program splice: ") + yesNo(program) + QStringLiteral(", duration set: ") + yesNo(dur) + QStringLiteral(", immediate: ") + yesNo(imm));
            if (program && !imm) L.add(4, QStringLiteral("Time PTS: ") + spliceTime(r));
            if (!program) {
                const int n = r.u8();
                L.add(4, QStringLiteral("Number of components: %1").arg(n));
                for (int k = 0; k < n && !r.bad; ++k) {
                    const int tag = r.u8();
                    L.add(6, QStringLiteral("Component tag: ") + hd(tag, 2) + (imm ? QString() : QStringLiteral(", time PTS: ") + spliceTime(r)));
                }
            }
            if (dur) {
                const int d = r.u8();
                const quint64 v = (quint64(d & 1) << 32) | r.u32();
                L.add(4, QStringLiteral("Duration PTS: ") + ptsText(v) + QStringLiteral(", auto return: ") + yesNo(d & 0x80));
            }
            const int upid = r.u16(), avail = r.u8(), expected = r.u8();
            L.add(4, QStringLiteral("Unique program id: ") + hd(upid, 4) + QStringLiteral(", avail: ") + hd(avail, 2) + QStringLiteral(", avails expected: %1").arg(expected));
        }
    } else if (cmd == 0x06) {
        L.add(4, QStringLiteral("Time: ") + spliceTime(r));
    } else if (cmd != 0x00 && cmd != 0x07) {
        r.p = qMin(r.end, cmdStart + cmdLen);             // splice_schedule / private_command: not decoded
    }
    if (cmdLen != 0xFFF) r.p = qMin(r.end, cmdStart + cmdLen);
    if (r.has(2)) {
        const int dl = r.u16();
        // splice descriptors: tag, length, identifier (CUEI), data. Shown with their SCTE 35 names and a dump.
        int p = r.p;
        const int end = qMin(r.end, p + dl);
        for (int k = 0; p + 2 <= end; ++k) {
            const int tag = quint8(s[p]), l = quint8(s[p + 1]);
            static const char* names[] = {"SCTE 35 Avail", "SCTE 35 DTMF", "SCTE 35 Segmentation", "SCTE 35 Time", "SCTE 35 Audio"};
            const QString nm = tag < 5 ? QString::fromLatin1(names[tag]) : QStringLiteral("unknown");
            L.add(4, QStringLiteral("- Descriptor %1: %2 (0x%3, %4), %5 bytes").arg(k).arg(nm, hex(tag, 2)).arg(tag).arg(l));
            dump(L, 6, s.mid(p + 2, qMin(l, end - p - 2)));
            p += 2 + l;
        }
    }
    const quint32 crc = (quint32(quint8(s[i.size - 4])) << 24) | (quint32(quint8(s[i.size - 3])) << 16) | (quint32(quint8(s[i.size - 2])) << 8) | quint32(quint8(s[i.size - 1]));
    L.add(4, QStringLiteral("CRC32: 0x%1 (%2)").arg(hex(crc, 8), crcValid(s.left(i.size)) ? QStringLiteral("OK") : QStringLiteral("WRONG")));
}

void bodyUnknown(Lines& L, const QByteArray& s, const SectionInfo& i) {
    if (i.longSyntax) { L.add(4, QStringLiteral("TIDext: ") + hd(i.tableIdExt, 4)); dump(L, 4, s.mid(8, i.size - 12)); }
    else dump(L, 4, s.mid(3, i.size - 3));
}

void sectionBody(Lines& L, const QByteArray& s) {
    SectionInfo i;
    if (!sectionInfo(s, 0, &i)) { dump(L, 4, s); return; }
    switch (i.tableId) {
    case 0x00: if (i.longSyntax) { bodyPat(L, s, i); return; } break;
    case 0x02: if (i.longSyntax) { bodyPmt(L, s, i); return; } break;
    case 0x42: case 0x46: if (i.longSyntax) { bodySdt(L, s, i); return; } break;
    case 0xC7: if (i.longSyntax) { bodyMgt(L, s, i); return; } break;
    case 0xC8: case 0xC9: if (i.longSyntax) { bodyVct(L, s, i); return; } break;
    case 0xCA: if (i.longSyntax) { bodyRrt(L, s, i); return; } break;
    case 0xCB: if (i.longSyntax) { bodyEit(L, s, i); return; } break;
    case 0xCC: if (i.longSyntax) { bodyEtt(L, s, i); return; } break;
    case 0xCD: if (i.longSyntax) { bodyStt(L, s, i); return; } break;
    case 0xFC: if (!i.longSyntax && i.size >= 20) { bodyScte35(L, s, i); return; } break;
    default: break;
    }
    bodyUnknown(L, s, i);
}

qint64 readTimestamp(const quint8* p) {                   // PES PTS / DTS (33 bits in 5 bytes with marker bits)
    return (qint64((p[0] >> 1) & 0x07) << 30) | (qint64(p[1]) << 22) | (qint64(p[2] >> 1) << 15) | (qint64(p[3]) << 7) | qint64(p[4] >> 1);
}

QJsonObject statsJson(const TsAnalyzer::Stats& s, int decimals) {
    QJsonObject o;
    const double k = std::pow(10.0, decimals);
    o["min"] = std::round(s.min * k) / k; o["avg"] = std::round(s.avg * k) / k; o["max"] = std::round(s.max * k) / k;
    o["count"] = double(s.count);
    return o;
}

}   // namespace

// ================================================================================================ public helpers
QString tableName(int t) {
    switch (t) {
    case 0x00: return "PAT";  case 0x01: return "CAT";  case 0x02: return "PMT";  case 0x03: return "TSDT";
    case 0x40: return "NIT Actual"; case 0x41: return "NIT Other"; case 0x42: return "SDT Actual"; case 0x46: return "SDT Other";
    case 0x4A: return "BAT";  case 0x4E: return "EIT p/f Actual"; case 0x4F: return "EIT p/f Other";
    case 0x70: return "TDT";  case 0x71: return "RST";  case 0x72: return "ST";   case 0x73: return "TOT";  case 0x74: return "AIT";
    case 0x80: return "ECM (even)"; case 0x81: return "ECM (odd)";
    case 0xC7: return "MGT (ATSC)"; case 0xC8: return "TVCT (ATSC)"; case 0xC9: return "CVCT (ATSC)"; case 0xCA: return "RRT (ATSC)";
    case 0xCB: return "EIT (ATSC)"; case 0xCC: return "ETT (ATSC)"; case 0xCD: return "STT (ATSC)";
    case 0xCE: return "DET (ATSC A/90)"; case 0xCF: return "DST (ATSC A/90)"; case 0xD1: return "NRT (ATSC A/90)";
    case 0xD2: return "LTST (ATSC A/90)"; case 0xD3: return "DCCT (ATSC)"; case 0xD4: return "DCCSCT (ATSC)";
    case 0xD6: return "AEIT (ATSC)"; case 0xD7: return "AETT (ATSC)"; case 0xD8: return "Cable Emergency Alert (ATSC)";
    case 0xDA: return "SVCT (ATSC)";
    case 0xFC: return "SCTE 35 Splice Information";
    case 0xFF: return "Forbidden TID 0xFF";
    default: break;
    }
    if (t >= 0x50 && t <= 0x5F) return QStringLiteral("EIT schedule Actual");
    if (t >= 0x60 && t <= 0x6F) return QStringLiteral("EIT schedule Other");
    if (t >= 0x82 && t <= 0x8F) return QStringLiteral("EMM");
    return QStringLiteral("unknown (0x%1)").arg(hex(t, 2));
}

QString streamTypeName(int st) {
    switch (st) {
    case 0x01: return "MPEG-1 Video";           case 0x02: return "MPEG-2 Video";
    case 0x03: return "MPEG-1 Audio";           case 0x04: return "MPEG-2 Audio";
    case 0x05: return "MPEG-2 Private sections"; case 0x06: return "MPEG-2 PES private data";
    case 0x07: return "MHEG";                   case 0x08: return "DSM-CC";
    case 0x09: return "MPEG-2 over ATM";        case 0x0A: return "DSM-CC MPE";
    case 0x0B: return "DSM-CC U-N";             case 0x0C: return "DSM-CC Stream Descriptors";
    case 0x0D: return "DSM-CC Sections";        case 0x0E: return "MPEG-2 Auxiliary";
    case 0x0F: return "MPEG-2 AAC Audio";       case 0x10: return "MPEG-4 Video";
    case 0x11: return "MPEG-4 AAC Audio";       case 0x12: return "MPEG-4 SL or M4Mux in PES packets";
    case 0x13: return "MPEG-4 SL or M4Mux in MPEG-4 sections"; case 0x14: return "DSM-CC Synchronized Download Protocol";
    case 0x15: return "MetaData in PES packets"; case 0x16: return "MetaData in sections";
    case 0x17: return "MetaData in DSM-CC Data Carousel"; case 0x18: return "MetaData in DSM-CC Object Carousel";
    case 0x19: return "MetaData in DSM-CC Sync. Download Protocol"; case 0x1A: return "MPEG-2 IPMP";
    case 0x1B: return "AVC video";
    case 0x1C: return "MPEG-4 AAC Audio, without additional transport syntax, such as DST, ALS and SLS";
    case 0x1D: return "ISO/IEC 14496-17 Text";  case 0x1E: return "Auxiliary video stream as defined in ISO/IEC 23002-3";
    case 0x1F: return "SVC video sub-bitstream of an AVC video stream, Annex G of ISO 14496-10";
    case 0x20: return "MVC video sub-bitstream of an AVC video stream, Annex H of ISO 14496-10";
    case 0x21: return "JPEG 2000 video stream ISO/IEC 15444-1";
    case 0x22: return "Additional view ISO/IEC 13818-2 video stream for stereoscopic 3D services";
    case 0x23: return "Additional view ISO/IEC 14496-10 video stream for stereoscopic 3D services";
    case 0x24: return "HEVC video";             case 0x25: return "HEVC temporal video subset of an HEVC video stream";
    case 0x27: return "Timeline and External Media Information Stream";
    case 0x33: return "VVC/H.266 video or VVC temporal video sub-bitstream, Annex A of ITUT H.266|ISO/IEC 23090-3";
    case 0x42: return "Chinese Video Standard"; case 0x7F: return "IPMP";
    case 0x80: return "DigiCipher II Video";    case 0x81: return "ATSC AC-3 Audio";
    case 0x83: return "ATSC AC-3 True HD Audio"; case 0x84: return "ATSC AC-3+ Audio";
    case 0x86: return "SCTE 35 Splice Info";    case 0x87: return "ATSC Enhanced-AC-3 Audio";
    case 0x91: return "A52b/AC-3 Audio";        case 0xA0: return "MSCODEC Video";
    case 0xD2: return "AVS2 video";             case 0xD3: return "AVS2 audio";
    case 0xD4: return "AVS3 video";             case 0xD5: return "AVS3 audio";
    case 0xEA: return "Private ES (VC-1)";
    default: return "unknown";
    }
}

bool isDvbTable(int tableId) { return tableId >= 0x40 && tableId <= 0x7F; }

QString formatTable(int pid, const QList<QByteArray>& sections, bool dvbContext) {
    if (sections.isEmpty()) return {};
    SectionInfo i0;
    if (!sectionInfo(sections.first(), 0, &i0)) return {};
    Lines L;
    L.dvb = dvbContext || isDvbTable(i0.tableId);
    L.add(0, QStringLiteral("* ") + tableName(i0.tableId) + QStringLiteral(", TID ") + hd(i0.tableId, 2) + QStringLiteral(", PID ") + hd(pid, 4));
    int total = 0;
    for (const QByteArray& s : sections) total += int(s.size());
    if (i0.longSyntax) L.add(2, QStringLiteral("Version: %1, sections: %2, total size: %3 bytes").arg(i0.version).arg(sections.size()).arg(total));
    else L.add(2, QStringLiteral("Short section, total size: %1 bytes").arg(total));
    for (int k = 0; k < sections.size(); ++k) {
        L.add(2, QStringLiteral("- Section %1:").arg(k));
        sectionBody(L, sections[k]);
    }
    return L.out;
}

// ================================================================================================ TsAnalyzer
void TsAnalyzer::Stats::add(double v) {
    if (count == 0) { min = max = v; avg = 0; }
    else { min = qMin(min, v); max = qMax(max, v); }
    avg += (v - avg) / double(++count);
}

TsAnalyzer::TsAnalyzer(const AnalyzerOptions& o) : m_opt(o) {}

void TsAnalyzer::feed(const char* data, qsizetype size, qint64 arrivalNs) {
    if (size <= 0) return;
    if (arrivalNs >= 0) { if (m_firstArrivalNs < 0) m_firstArrivalNs = arrivalNs; m_lastArrivalNs = arrivalNs; }
    m_buf.append(data, size);
    const char* b = m_buf.constData();
    const qsizetype n = m_buf.size();
    qsizetype pos = 0;
    while (n - pos >= kPacketSize) {
        // In sync while every packet starts with 0x47; after a loss, sync is found again only where the next packet does too
        // (so wait for more data when it is not there yet).
        if (!m_inSync && b[pos] == 0x47 && n - pos < 2 * kPacketSize) break;
        if (b[pos] != 0x47 || (!m_inSync && b[pos + kPacketSize] != 0x47)) {
            if (m_inSync) {
                m_inSync = false;
                ++m_syncLosses;
                addEvent(m_errors, m_packets, -1, QStringLiteral("sync"), QStringLiteral("TS synchronization lost"));
            }
            ++pos;
            continue;
        }
        m_inSync = true;
        processPacket(b + pos, arrivalNs);
        pos += kPacketSize;
    }
    m_buf.remove(0, pos);
}

void TsAnalyzer::addEvent(QList<Event>& list, qint64 packet, int pid, const QString& type, const QString& text, const QJsonObject& json) {
    if (&list == &m_errors) ++m_errorCount; else ++m_scteCount;
    if (list.size() >= m_opt.maxEvents) return;
    list.append({packet, pid, type, text, json});
}

void TsAnalyzer::processPacket(const char* pk, qint64 arrivalNs) {
    const qint64 index = m_packets++;
    const quint8* u = reinterpret_cast<const quint8*>(pk);
    const int pid = packetPid(pk);
    PidStats& ps = m_pids[pid];
    ps.pid = pid;
    ++ps.packets;
    if (u[1] & 0x80) {                                    // transport_error_indicator: content unusable (TSDuck skips it too)
        ++ps.tei;
        addEvent(m_errors, index, pid, QStringLiteral("tei"), QStringLiteral("transport error indicator set"));
        return;
    }
    const bool pusi = u[1] & 0x40;
    const int scrambling = (u[3] >> 6) & 0x03, afc = (u[3] >> 4) & 0x03, cc = u[3] & 0x0F;
    if (pusi) ++ps.pusi;
    if (scrambling) ++ps.scrambled;
    int afLen = 0;
    bool disc = false;
    if (afc & 0x02) {
        afLen = u[4];
        if (afLen > 183 || (afc == 0x03 && afLen > 182)) {
            addEvent(m_errors, index, pid, QStringLiteral("af"), QStringLiteral("invalid adaptation field length %1").arg(afLen));
            return;
        }
        if (afLen > 0) disc = u[5] & 0x80;
    }
    PidState& st = m_state[pid];

    // Continuity (ISO 13818-1 2.4.3.3): +1 per packet with payload, unchanged without; one duplicate allowed; any jump
    // accepted on discontinuity_indicator. The null PID is exempt.
    bool reset = false;
    if (pid != kNullPid) {
        const bool payload = afc & 0x01;
        const int expected = payload ? ((st.lastCc + 1) & 0x0F) : st.lastCc;
        bool duplicate = false;
        if (st.lastCc >= 0 && !disc && cc != expected) {
            if (payload && cc == st.lastCc && !st.lastDuplicate) duplicate = true;
            else {
                ++ps.ccErrors;
                QJsonObject x; x["expected"] = expected; x["got"] = cc;
                addEvent(m_errors, index, pid, QStringLiteral("cc"), QStringLiteral("continuity error, expected CC %1, got %2").arg(expected).arg(cc), x);
                reset = true;
            }
        }
        if (disc && st.lastCc >= 0) ++ps.discontinuities;
        st.lastDuplicate = duplicate;
        st.lastCc = cc;
        if (duplicate) { ++ps.duplicates; return; }       // a repeated packet carries nothing new
    }

    if (afLen >= 7 && (u[5] & 0x10)) {                    // PCR
        const qint64 base = (qint64(u[6]) << 25) | (qint64(u[7]) << 17) | (qint64(u[8]) << 9) | (qint64(u[9]) << 1) | (u[10] >> 7);
        const qint64 ext = ((u[10] & 0x01) << 8) | u[11];
        const qint64 pcr = base * 300 + ext;
        if (!st.pcrs.empty() && !disc) {
            if (m_clockPid < 0) m_clockPid = pid;
            if (m_clockPid == pid) {
                qint64 d = pcr - st.pcrs.back().pcr;
                if (d < 0) d += kPcrWrap;
                if (d > 0 && d <= qint64(kPcrHz)) m_clockTicks += d;
            }
        }
        st.pcrs.push_back({index, pcr, arrivalNs, disc});
        ++ps.pcrCount;
    }
    if (scrambling || !(afc & 0x01)) return;
    const int off = 4 + ((afc & 0x02) ? 1 + afLen : 0);
    if (off >= kPacketSize) return;
    const quint8* pl = u + off;
    const int len = kPacketSize - off;

    if (pusi && len >= 3 && pl[0] == 0 && pl[1] == 0 && pl[2] == 1) {   // PES start: not a section PID (TSDuck ignores these)
        ps.pes = true;
        st.sectionBuf.clear();
        ++ps.pesStarts;
        const int sid = len > 3 ? pl[3] : 0;
        const bool hasHeader = !(sid == 0xBC || sid == 0xBE || sid == 0xBF || sid == 0xF0 || sid == 0xF1 || sid == 0xF2 || sid == 0xF8 || sid == 0xFF);
        if (hasHeader && len >= 14 && (pl[6] & 0xC0) == 0x80) {
            const int flags = pl[7] >> 6;
            if ((flags & 0x02) && st.pts.size() < 200000) {
                const qint64 pts = readTimestamp(pl + 9);
                const qint64 dts = flags == 0x03 && len >= 19 ? readTimestamp(pl + 14) : -1;
                st.pts.push_back({index, pts, dts});
            }
        }
        return;
    }
    demux(pid, st, pl, len, pusi, index, reset);
}

void TsAnalyzer::demux(int pid, PidState& st, const quint8* pl, int len, bool pusi, qint64 index, bool reset) {
    if (reset) st.sectionBuf.clear();                     // a continuity break invalidates the section in progress
    if (!pusi) {
        if (st.sectionBuf.isEmpty()) return;
        st.sectionBuf.append(reinterpret_cast<const char*>(pl), len);
        consume(pid, st, index);
        return;
    }
    const int ptr = pl[0];
    if (1 + ptr > len) { st.sectionBuf.clear(); return; }
    if (!st.sectionBuf.isEmpty()) {                       // the bytes before pointer_field end the pending section
        st.sectionBuf.append(reinterpret_cast<const char*>(pl) + 1, ptr);
        consume(pid, st, index);
    }
    st.sectionBuf = QByteArray(reinterpret_cast<const char*>(pl) + 1 + ptr, len - 1 - ptr);
    consume(pid, st, index);
}

void TsAnalyzer::consume(int pid, PidState& st, qint64 index) {
    QByteArray& b = st.sectionBuf;
    while (!b.isEmpty()) {
        if (quint8(b[0]) == 0xFF) { b.clear(); return; } // stuffing up to the end of the packet
        if (b.size() < 3) return;
        const int size = 3 + (((quint8(b[1]) & 0x0F) << 8) | quint8(b[2]));
        if (size > kMaxPrivateSection) { b.clear(); return; }
        if (b.size() < size) return;
        const QByteArray sec = b.left(size);
        b.remove(0, size);
        onSection(pid, sec, index);
    }
}

void TsAnalyzer::onSection(int pid, const QByteArray& sec, qint64 index) {
    PidStats& ps = m_pids[pid];
    SectionInfo i;
    if (!sectionInfo(sec, 0, &i)) return;
    ++ps.sections;
    const quint32 tkey = (quint32(pid) << 8) | quint32(i.tableId);
    TableStats& ts = m_tableStats[tkey];
    ts.pid = pid; ts.tableId = i.tableId;
    ++ts.sections;
    if (hasCrc(sec) && !crcValid(sec)) {
        ++ts.crcErrors; ++ps.crcErrors;
        QJsonObject x; x["tableId"] = i.tableId;
        addEvent(m_errors, index, pid, QStringLiteral("crc"), QStringLiteral("CRC32 error in %1 section (TID 0x%2)").arg(tableName(i.tableId), hex(i.tableId, 2)), x);
        return;
    }
    const auto last = m_tableLastPacket.constFind(tkey);
    if (last != m_tableLastPacket.constEnd()) ts.intervalPackets.add(double(index - *last));
    m_tableLastPacket[tkey] = index;
    if (i.longSyntax) { ts.version = i.version; ts.tidExts.insert(i.tableIdExt); }

    if (!i.longSyntax) {
        if (i.tableId == 0xFC) {
            scte35::SpliceInfo si;
            QString err;
            QJsonObject x; x["tableId"] = 0xFC;
            QString text;
            if (scte35::parseSection(sec, &si, &err)) {
                x["command"] = spliceCommandName(si.command);
                text = spliceCommandName(si.command);
                if (si.command == scte35::SpliceInsert) {
                    const scte35::Insert& in = si.insert;
                    x["eventId"] = double(in.eventId); x["cancel"] = in.cancel;
                    text += QStringLiteral(" event %1").arg(in.eventId);
                    if (in.cancel) text += QStringLiteral(", cancel");
                    else {
                        x["outOfNetwork"] = in.outOfNetwork; x["immediate"] = in.immediate; x["uniqueProgramId"] = in.uniqueProgramId;
                        text += in.outOfNetwork ? QStringLiteral(", out of network") : QStringLiteral(", return to network");
                        if (in.immediate) text += QStringLiteral(", immediate");
                        else if (in.time.specified) { x["ptsTime"] = double(in.time.pts); text += QStringLiteral(", at PTS %1").arg(in.time.pts); }
                        if (in.hasDuration) {
                            x["durationSec"] = double(in.duration) / 90000.0; x["autoReturn"] = in.autoReturn;
                            text += QStringLiteral(", duration %1 s").arg(fixed(double(in.duration) / 90000.0, 3)) + (in.autoReturn ? QStringLiteral(", auto return") : QString());
                        }
                    }
                } else if (si.command == scte35::TimeSignal && si.timeSignal.specified) {
                    x["ptsTime"] = double(si.timeSignal.pts);
                    text += QStringLiteral(" at PTS %1").arg(si.timeSignal.pts);
                }
            } else {
                x["error"] = err;
                text = QStringLiteral("undecodable splice_info_section: ") + err;
            }
            addEvent(m_scte35, index, pid, QStringLiteral("scte35"), text, x);
        }
        display(pid, {sec});
        return;
    }
    if (!i.current) return;                               // "next" sections: tsp ignores them by default
    const quint64 key = (quint64(pid) << 24) | (quint64(i.tableId) << 16) | quint64(i.tableIdExt);
    LongTable& t = m_long[key];
    if (t.version != i.version || t.last != i.lastSectionNumber) {
        t = LongTable();
        t.version = i.version; t.last = i.lastSectionNumber;
        t.sections = QList<QByteArray>(t.last + 1);
    }
    if (t.complete || i.sectionNumber > t.last) return;
    if (t.sections[i.sectionNumber].isEmpty()) t.sections[i.sectionNumber] = sec;
    for (const QByteArray& s : t.sections) if (s.isEmpty()) return;
    t.complete = true;
    onTableComplete(pid, t.sections);
    display(pid, t.sections);
}

void TsAnalyzer::display(int pid, const QList<QByteArray>& sections) {
    if (!m_opt.displayPids.isEmpty() && !m_opt.displayPids.contains(pid)) return;
    if (tableLimitReached()) return;
    SectionInfo i0;
    if (sectionInfo(sections.first(), 0, &i0) && isDvbTable(i0.tableId)) m_dvbContext = true;   // tsp: DVB from now on
    QString t = formatTable(pid, sections, m_dvbContext) + QLatin1Char('\n');
    if (m_displayed == 0) t.prepend(QLatin1Char('\n'));   // tsp starts with an empty line, then a blank line after each table
    ++m_displayed;
    if (m_tablesText.size() < kMaxTablesText) m_tablesText += t;
    if (onTable) onTable(t);
}

void TsAnalyzer::onTableComplete(int pid, const QList<QByteArray>& sections) {
    SectionInfo i0;
    sectionInfo(sections.first(), 0, &i0);
    switch (i0.tableId) {
    case 0x00:                                            // PAT
        m_tsid = i0.tableIdExt;
        m_patPrograms.clear();
        for (const QByteArray& s : sections)
            for (int p = 8; p + 4 <= s.size() - 4; p += 4) {
                const int prog = (quint8(s[p]) << 8) | quint8(s[p + 1]), ppid = ((quint8(s[p + 2]) & 0x1F) << 8) | quint8(s[p + 3]);
                if (prog != 0) m_patPrograms[prog] = ppid;
            }
        break;
    case 0x02: {                                          // PMT
        Pmt pmt;
        if (!parsePmt(sections.first(), &pmt)) break;
        Service svc;
        svc.programNumber = pmt.programNumber; svc.pmtPid = pid; svc.pcrPid = pmt.pcrPid;
        for (const PmtStream& s : pmt.streams) {
            Stream x; x.pid = s.pid; x.streamType = s.streamType;
            for (const Descriptor& d : s.descriptors) if (d.tag == 0x0A && d.payload.size() >= 3) x.language = lang3(d.payload, 0);
            svc.streams.append(x);
        }
        m_pmt[pmt.programNumber] = svc;
        break;
    }
    case 0x42:                                            // SDT actual: service names
        for (const QByteArray& s : sections) {
            Rd r{s, 11, int(s.size()) - 4};
            while (r.has(5)) {
                const int sid = r.u16();
                r.u8();
                const int dl = r.u16() & 0x0FFF, end = qMin(r.end, r.p + dl);
                for (int p = r.p; p + 2 <= end;) {
                    const int tag = quint8(s[p]), l = quint8(s[p + 1]);
                    if (tag == 0x48 && p + 2 + l <= end) {
                        const QByteArray d = s.mid(p + 2, l);
                        Rd q{d, 1, int(d.size())};
                        const QByteArray prov = q.bytes(q.u8()), name = q.bytes(q.u8());
                        if (!q.bad) m_sdt[sid] = {dvbString(name), dvbString(prov)};
                    }
                    p += 2 + l;
                }
                r.p = end;
            }
        }
        break;
    case 0xC8: case 0xC9:                                 // TVCT / CVCT: virtual channels
        m_vct.clear();
        for (const QByteArray& s : sections) {
            Rd r{s, 8, int(s.size()) - 4};
            r.u8();
            const int n = r.u8();
            for (int k = 0; k < n && r.has(32); ++k) {
                Service c;
                for (int j = 0; j < 7; ++j) { const int u = r.u16(); if (u) c.shortName += QChar(char16_t(u)); else { r.p += 2 * (6 - j); break; } }
                const quint32 a = r.u32();
                c.major = int((a >> 18) & 0x3FF); c.minor = int((a >> 8) & 0x3FF);
                r.u32(); r.u16();
                c.programNumber = r.u16();
                const int f = r.u16();
                c.hidden = f & 0x1000;
                c.sourceId = r.u16();
                const int dl = r.u16() & 0x03FF, end = qMin(r.end, r.p + dl);
                for (int p = r.p; p + 2 <= end;) {
                    const int tag = quint8(s[p]), l = quint8(s[p + 1]);
                    if (tag == 0xA0 && p + 2 + l <= end) {
                        Lines tmp;                        // reuse the display decoder: first string's text
                        multiString(tmp, 0, QString(), s, p + 2, p + 2 + l);
                        const int q = int(tmp.out.indexOf(QStringLiteral("text: \"")));
                        if (q >= 0) { c.longName = tmp.out.mid(q + 7); c.longName = c.longName.left(c.longName.indexOf(QStringLiteral("\"\n"))); }
                    }
                    p += 2 + l;
                }
                r.p = end;
                m_vct[c.programNumber] = c;
            }
        }
        break;
    case 0xC7: {                                          // MGT: PSIP table PIDs
        m_mgtPids.clear();
        Rd r{sections.first(), 9, int(sections.first().size()) - 4};
        const int n = r.u16();
        for (int k = 0; k < n && r.has(11); ++k) {
            const int type = r.u16(), tpid = r.u16() & 0x1FFF;
            r.u8(); r.u32();
            const int dl = r.u16() & 0x0FFF;
            r.p = qMin(r.end, r.p + dl);
            if (tpid != 0x1FFB && !m_mgtPids.contains(tpid)) m_mgtPids[tpid] = type;
        }
        break;
    }
    default:
        break;
    }
}

double TsAnalyzer::streamSeconds() const { return double(m_clockTicks) / kPcrHz; }

void TsAnalyzer::finish() {
    if (m_finished) return;
    m_finished = true;
    // Reference clock: the PID with the most PCRs. TS bitrate = packets between PCRs / PCR time over every continuous span.
    qint64 best = 1;
    for (auto it = m_state.constBegin(); it != m_state.constEnd(); ++it)
        if (qint64(it->pcrs.size()) > best || (qint64(it->pcrs.size()) == best && best > 1 && it.key() < m_refPcrPid)) { best = qint64(it->pcrs.size()); m_refPcrPid = it.key(); }
    if (m_refPcrPid >= 0) {
        const auto& v = m_state[m_refPcrPid].pcrs;
        double pk = 0, ticks = 0;
        for (size_t k = 1; k < v.size(); ++k) {
            qint64 d = v[k].pcr - v[k - 1].pcr;
            if (d < 0) d += kPcrWrap;
            if (v[k].discontinuity || d <= 0 || d > qint64(kPcrHz)) continue;
            pk += double(v[k].packet - v[k - 1].packet);
            ticks += double(d);
        }
        if (ticks > 0 && pk > 0) { m_bitrate = pk * kPacketSize * 8.0 * kPcrHz / ticks; m_bitrateSource = QStringLiteral("pcr"); }
    }
    if (m_bitrate <= 0 && m_lastArrivalNs > m_firstArrivalNs && m_packets > 0) {
        m_bitrate = double(m_packets) * kPacketSize * 8.0 / (double(m_lastArrivalNs - m_firstArrivalNs) / 1e9);
        m_bitrateSource = QStringLiteral("arrival");
    }
    const double ticksPerPacket = m_bitrate > 0 ? kPacketSize * 8.0 * kPcrHz / m_bitrate : 0;

    for (auto it = m_pids.begin(); it != m_pids.end(); ++it) {
        PidStats& ps = it.value();
        const PidState& st = m_state[ps.pid];
        ps.bitrate = m_packets > 0 ? m_bitrate * double(ps.packets) / double(m_packets) : 0;
        ps.description = describePid(ps.pid);
        // PCR interval, jitter against the constant TS rate (tsp pcrverify), arrival jitter (UDP).
        double segMin = 0, segMax = 0, maxPp = -1;
        bool seg = false;
        qint64 unwrapped = 0;
        for (size_t k = 0; k < st.pcrs.size(); ++k) {
            const PcrSample& c = st.pcrs[k];
            bool cont = k > 0 && !c.discontinuity;
            qint64 d = 0;
            if (cont) {
                d = c.pcr - st.pcrs[k - 1].pcr;
                if (d < 0) d += kPcrWrap;
                cont = d > 0 && d <= 10 * qint64(kPcrHz);
            }
            if (cont) {
                const double ms = double(d) / 27000.0;
                ps.pcrIntervalMs.add(ms);
                if (ms > 100.0) ++ps.pcrIntervalOver100ms;
                if (ticksPerPacket > 0) {
                    const double jitter = std::fabs(double(d) - double(c.packet - st.pcrs[k - 1].packet) * ticksPerPacket);
                    ps.pcrJitterNs.add(jitter * 1000.0 / 27.0);
                    if (jitter > double(m_opt.jitterMaxTicks)) ++ps.pcrJitterOverMax;
                }
                unwrapped += d;
            } else {
                unwrapped = 0;
                if (seg) maxPp = qMax(maxPp, segMax - segMin);
                seg = false;
            }
            if (c.arrivalNs >= 0) {
                const double off = double(unwrapped) / 27000.0 - double(c.arrivalNs) / 1e6;
                if (!seg) { segMin = segMax = off; seg = true; }
                else { segMin = qMin(segMin, off); segMax = qMax(segMax, off); }
            }
        }
        if (seg) maxPp = qMax(maxPp, segMax - segMin);
        ps.pcrArrivalJitterMs = maxPp;
    }

    // PTS / DTS - PCR: PCR of the program's PCR PID at the PES packet, extrapolated at the TS rate from the last PCR.
    QHash<int, int> pcrPidOf;
    for (const Service& s : std::as_const(m_pmt)) for (const Stream& x : s.streams) if (!pcrPidOf.contains(x.pid)) pcrPidOf[x.pid] = s.pcrPid;
    for (auto it = m_pids.begin(); it != m_pids.end(); ++it) {
        const PidState& st = m_state[it.key()];
        if (st.pts.empty()) continue;
        const int pcrPid = pcrPidOf.value(it.key(), m_refPcrPid);
        if (pcrPid < 0 || !m_state.contains(pcrPid)) continue;
        const auto& pcrs = m_state[pcrPid].pcrs;
        if (pcrs.empty()) continue;
        auto offsetMs = [&](qint64 ts, qint64 pcrAt) {
            qint64 d = (ts * 300 - pcrAt) % kPcrWrap;
            if (d < 0) d += kPcrWrap;
            if (d >= kPcrWrap / 2) d -= kPcrWrap;
            return double(d) / 27000.0;
        };
        for (const PtsSample& s : st.pts) {
            auto up = std::upper_bound(pcrs.begin(), pcrs.end(), s.packet, [](qint64 p, const PcrSample& c) { return p < c.packet; });
            if (up == pcrs.begin()) continue;
            const PcrSample& c = *(up - 1);
            const qint64 pcrAt = c.pcr + qint64(std::llround(double(s.packet - c.packet) * ticksPerPacket));
            it->ptsPcrMs.add(offsetMs(s.pts, pcrAt));
            if (s.dts >= 0) it->dtsPcrMs.add(offsetMs(s.dts, pcrAt));
        }
    }

    const double msPerPacket = m_bitrate > 0 ? kPacketSize * 8.0 * 1000.0 / m_bitrate : 0;
    for (TableStats& t : m_tableStats) {
        if (t.intervalPackets.count == 0 || msPerPacket <= 0) continue;
        t.intervalMs.min = t.intervalPackets.min * msPerPacket;
        t.intervalMs.avg = t.intervalPackets.avg * msPerPacket;
        t.intervalMs.max = t.intervalPackets.max * msPerPacket;
        t.intervalMs.count = t.intervalPackets.count;
    }
}

double TsAnalyzer::durationSec() const { return m_bitrate > 0 ? double(m_packets) * kPacketSize * 8.0 / m_bitrate : 0; }
double TsAnalyzer::packetSeconds(qint64 packet) const { return m_bitrate > 0 ? double(packet) * kPacketSize * 8.0 / m_bitrate : -1; }
qint64 TsAnalyzer::ccErrors() const { qint64 n = 0; for (const PidStats& p : m_pids) n += p.ccErrors; return n; }
qint64 TsAnalyzer::crcErrors() const { qint64 n = 0; for (const PidStats& p : m_pids) n += p.crcErrors; return n; }

QString TsAnalyzer::describePid(int pid) const {
    switch (pid) {
    case 0x0000: return QStringLiteral("PAT");
    case 0x0001: return QStringLiteral("CAT");
    case 0x0010: return QStringLiteral("NIT");
    case 0x0011: return QStringLiteral("SDT/BAT");
    case 0x0012: return QStringLiteral("EIT (DVB)");
    case 0x0014: return QStringLiteral("TDT/TOT");
    case 0x1FFB: return QStringLiteral("ATSC PSIP (MGT/TVCT/STT/RRT)");
    case kNullPid: return QStringLiteral("Null packets");
    default: break;
    }
    for (const Service& s : m_pmt) {
        for (const Stream& x : s.streams)
            if (x.pid == pid) {
                QString d = streamTypeName(x.streamType) + QStringLiteral(" (program %1)").arg(s.programNumber);
                if (!x.language.isEmpty()) d += QLatin1Char(' ') + x.language;
                if (pid == s.pcrPid) d += QStringLiteral(", PCR");
                return d;
            }
    }
    for (auto it = m_patPrograms.constBegin(); it != m_patPrograms.constEnd(); ++it) if (it.value() == pid) return QStringLiteral("PMT (program %1)").arg(it.key());
    for (const Service& s : m_pmt) if (s.pcrPid == pid) return QStringLiteral("PCR (program %1)").arg(s.programNumber);
    if (m_mgtPids.contains(pid)) return QStringLiteral("ATSC ") + mgtTypeName(m_mgtPids[pid]);
    const PidStats ps = m_pids.value(pid);
    if (ps.pes) return QStringLiteral("PES, not in any PMT");
    if (ps.sections > 0) return QStringLiteral("sections, not referenced");
    return QStringLiteral("not referenced");
}

QList<TsAnalyzer::Service> TsAnalyzer::services() const {
    QMap<int, Service> out;
    for (auto it = m_patPrograms.constBegin(); it != m_patPrograms.constEnd(); ++it) { Service& s = out[it.key()]; s.programNumber = it.key(); s.pmtPid = it.value(); }
    for (const Service& p : m_pmt) {
        Service& s = out[p.programNumber];
        s.programNumber = p.programNumber; s.pmtPid = p.pmtPid; s.pcrPid = p.pcrPid; s.streams = p.streams;
    }
    for (auto it = m_sdt.constBegin(); it != m_sdt.constEnd(); ++it) if (out.contains(it.key())) { out[it.key()].name = it.value().first; out[it.key()].provider = it.value().second; }
    for (const Service& c : m_vct) {
        Service& s = out[c.programNumber];
        s.programNumber = c.programNumber; s.major = c.major; s.minor = c.minor; s.shortName = c.shortName; s.longName = c.longName;
        s.sourceId = c.sourceId; s.hidden = c.hidden;
    }
    return out.values();
}

QList<TsAnalyzer::TableStats> TsAnalyzer::tables() const { return m_tableStats.values(); }

QString TsAnalyzer::reportText() const {
    Lines L;
    L.add(0, QString());
    L.add(0, QStringLiteral("* Transport stream"));
    L.add(2, QStringLiteral("Packets: %1 (%2 bytes), duration: %3 s, bitrate: %4 b/s (%5)")
               .arg(sep(m_packets), sep(m_packets * kPacketSize), fixed(durationSec(), 3), sep(qint64(std::llround(m_bitrate))), m_bitrateSource));
    qint64 tei = 0, scr = 0;
    for (const PidStats& p : m_pids) { tei += p.tei; scr += p.scrambled; }
    L.add(2, QStringLiteral("Errors: sync %1, transport %2, continuity %3, CRC %4, scrambled packets %5").arg(m_syncLosses).arg(tei).arg(ccErrors()).arg(crcErrors()).arg(scr));
    if (m_tsid >= 0) L.add(2, QStringLiteral("TS id: ") + hd(m_tsid, 4));

    const QList<Service> svcs = services();
    if (!svcs.isEmpty()) {
        L.add(0, QString());
        L.add(0, QStringLiteral("* Services"));
        for (const Service& s : svcs) {
            QString h = QStringLiteral("- Program ") + hd(s.programNumber, 4);
            if (s.pmtPid >= 0) h += QStringLiteral(", PMT PID ") + hd(s.pmtPid, 4);
            if (s.pcrPid >= 0) h += QStringLiteral(", PCR PID ") + (s.pcrPid == kNullPid ? QStringLiteral("none") : hd(s.pcrPid, 4));
            L.add(2, h);
            if (!s.name.isEmpty() || !s.provider.isEmpty()) L.add(4, QStringLiteral("Name: \"") + s.name + QStringLiteral("\", provider: \"") + s.provider + QLatin1Char('"'));
            if (s.major >= 0) L.add(4, QStringLiteral("Channel %1.%2 \"").arg(s.major).arg(s.minor) + s.shortName + QStringLiteral("\"") + (s.longName.isEmpty() ? QString() : QStringLiteral(" (") + s.longName + QLatin1Char(')'))
                                  + QStringLiteral(", source id ") + hd(s.sourceId, 4) + (s.hidden ? QStringLiteral(", hidden") : QString()));
            for (const Stream& x : s.streams)
                L.add(4, QStringLiteral("PID ") + hd(x.pid, 4) + QStringLiteral(": ") + streamTypeName(x.streamType) + QStringLiteral(" (0x%1)").arg(hex(x.streamType, 2)) + (x.language.isEmpty() ? QString() : QStringLiteral(", ") + x.language));
        }
    }

    L.add(0, QString());
    L.add(0, QStringLiteral("* PIDs"));
    L.add(2, QStringLiteral("PID              Packets   Bitrate (b/s)      %  CC err  Description"));
    for (const PidStats& p : m_pids) {
        const double pct = m_packets > 0 ? 100.0 * double(p.packets) / double(m_packets) : 0;
        L.add(2, (QStringLiteral("0x") + hex(p.pid, 4) + QStringLiteral(" (%1)").arg(p.pid)).leftJustified(13) + sep(p.packets).rightJustified(11) + sep(qint64(std::llround(p.bitrate))).rightJustified(16)
                     + fixed(pct, 1).rightJustified(7) + QString::number(p.ccErrors).rightJustified(8) + QStringLiteral("  ") + p.description);
    }
    L.add(2, QStringLiteral("Total").leftJustified(13) + sep(m_packets).rightJustified(11) + sep(qint64(std::llround(m_bitrate))).rightJustified(16) + QStringLiteral("100.0").rightJustified(7) + QString::number(ccErrors()).rightJustified(8));

    bool header = false;
    for (const PidStats& p : m_pids) {
        if (p.pcrCount == 0) continue;
        if (!header) { L.add(0, QString()); L.add(0, QStringLiteral("* PCR")); header = true; }
        QString s = QStringLiteral("PID ") + hd(p.pid, 4) + QStringLiteral(": %1 PCRs").arg(p.pcrCount);
        if (p.pcrIntervalMs.count) s += QStringLiteral(", interval %1 ms avg (min %2, max %3)").arg(fixed(p.pcrIntervalMs.avg, 2), fixed(p.pcrIntervalMs.min, 2), fixed(p.pcrIntervalMs.max, 2));
        if (p.pcrIntervalOver100ms) s += QStringLiteral(", %1 over 100 ms").arg(p.pcrIntervalOver100ms);
        if (p.pcrJitterNs.count) s += QStringLiteral(", jitter %1 us max, %2 us avg (%3 over %4 us)").arg(fixed(p.pcrJitterNs.max / 1000.0, 2), fixed(p.pcrJitterNs.avg / 1000.0, 2)).arg(p.pcrJitterOverMax).arg(fixed(double(m_opt.jitterMaxTicks) / 27.0, 1));
        if (p.pcrArrivalJitterMs >= 0) s += QStringLiteral(", arrival jitter %1 ms").arg(fixed(p.pcrArrivalJitterMs, 2));
        if (p.discontinuities) s += QStringLiteral(", %1 discontinuities").arg(p.discontinuities);
        L.add(2, s);
    }
    header = false;
    for (const PidStats& p : m_pids) {
        if (p.ptsPcrMs.count == 0) continue;
        if (!header) { L.add(0, QString()); L.add(0, QStringLiteral("* PTS / DTS - PCR")); header = true; }
        QString s = QStringLiteral("PID ") + hd(p.pid, 4) + QStringLiteral(": %1 PES, PTS-PCR %2 ms avg (min %3, max %4)").arg(p.pesStarts).arg(fixed(p.ptsPcrMs.avg, 1), fixed(p.ptsPcrMs.min, 1), fixed(p.ptsPcrMs.max, 1));
        if (p.dtsPcrMs.count) s += QStringLiteral(", DTS-PCR %1 ms avg (min %2, max %3)").arg(fixed(p.dtsPcrMs.avg, 1), fixed(p.dtsPcrMs.min, 1), fixed(p.dtsPcrMs.max, 1));
        L.add(2, s);
    }
    if (!m_tableStats.isEmpty()) {
        L.add(0, QString());
        L.add(0, QStringLiteral("* Tables"));
        for (const TableStats& t : m_tableStats) {
            QString s = QStringLiteral("PID ") + hd(t.pid, 4) + QStringLiteral(" ") + tableName(t.tableId) + QStringLiteral(" (0x%1): %2 sections").arg(hex(t.tableId, 2)).arg(t.sections);
            if (t.version >= 0) s += QStringLiteral(", version %1").arg(t.version);
            if (t.tidExts.size() > 1) s += QStringLiteral(", %1 table id extensions").arg(t.tidExts.size());
            if (t.intervalMs.count) s += QStringLiteral(", every %1 ms (max %2 ms)").arg(fixed(t.intervalMs.avg, 1), fixed(t.intervalMs.max, 1));
            if (t.crcErrors) s += QStringLiteral(", %1 CRC errors").arg(t.crcErrors);
            L.add(2, s);
        }
    }
    auto events = [&](const char* title, const QList<Event>& list, qint64 total) {
        if (list.isEmpty()) return;
        L.add(0, QString());
        L.add(0, QString::fromLatin1(title));
        for (const Event& e : list) {
            QString s = QStringLiteral("packet %1").arg(e.packet);
            const double t = packetSeconds(e.packet);
            if (t >= 0) s += QStringLiteral(" (%1 s)").arg(fixed(t, 3));
            s += e.pid >= 0 ? QStringLiteral(": PID ") + hd(e.pid, 4) + QStringLiteral(" ") : QStringLiteral(": ");
            L.add(2, s + e.text);
        }
        if (total > list.size()) L.add(2, QStringLiteral("... %1 more").arg(total - list.size()));
    };
    events("* SCTE 35", m_scte35, m_scteCount);
    events("* Errors", m_errors, m_errorCount);
    return L.out;
}

QJsonObject TsAnalyzer::toJson() const {
    QJsonObject o;
    o["packets"] = double(m_packets);
    o["bytes"] = double(m_packets * kPacketSize);
    o["bitrate"] = std::round(m_bitrate);
    o["bitrateSource"] = m_bitrateSource;
    o["durationSec"] = std::round(durationSec() * 1000.0) / 1000.0;
    o["syncLosses"] = double(m_syncLosses);
    o["ccErrors"] = double(ccErrors());
    o["crcErrors"] = double(crcErrors());
    o["errorCount"] = double(m_errorCount);
    o["tablesDisplayed"] = m_displayed;
    if (m_tsid >= 0) o["tsid"] = m_tsid;
    if (m_refPcrPid >= 0) o["pcrPid"] = m_refPcrPid;

    QJsonArray pids;
    for (const PidStats& p : m_pids) {
        QJsonObject x;
        x["pid"] = p.pid;
        x["description"] = p.description;
        x["packets"] = double(p.packets);
        x["bitrate"] = std::round(p.bitrate);
        x["percent"] = m_packets > 0 ? std::round(1000.0 * double(p.packets) / double(m_packets)) / 10.0 : 0.0;
        x["ccErrors"] = double(p.ccErrors);
        if (p.duplicates) x["duplicates"] = double(p.duplicates);
        if (p.discontinuities) x["discontinuities"] = double(p.discontinuities);
        if (p.scrambled) x["scrambled"] = double(p.scrambled);
        if (p.tei) x["transportErrors"] = double(p.tei);
        if (p.sections) { x["sections"] = double(p.sections); x["crcErrors"] = double(p.crcErrors); }
        if (p.pes) x["pesStarts"] = double(p.pesStarts);
        if (p.pcrCount) {
            QJsonObject c;
            c["count"] = double(p.pcrCount);
            if (p.pcrIntervalMs.count) c["intervalMs"] = statsJson(p.pcrIntervalMs, 3);
            if (p.pcrJitterNs.count) c["jitterNs"] = statsJson(p.pcrJitterNs, 0);
            c["intervalOver100ms"] = double(p.pcrIntervalOver100ms);
            c["jitterOverMax"] = double(p.pcrJitterOverMax);
            if (p.pcrArrivalJitterMs >= 0) c["arrivalJitterMs"] = std::round(p.pcrArrivalJitterMs * 1000.0) / 1000.0;
            x["pcr"] = c;
        }
        if (p.ptsPcrMs.count) x["ptsPcrMs"] = statsJson(p.ptsPcrMs, 2);
        if (p.dtsPcrMs.count) x["dtsPcrMs"] = statsJson(p.dtsPcrMs, 2);
        pids.append(x);
    }
    o["pids"] = pids;

    QJsonArray svcs;
    for (const Service& s : services()) {
        QJsonObject x;
        x["programNumber"] = s.programNumber;
        if (s.pmtPid >= 0) x["pmtPid"] = s.pmtPid;
        if (s.pcrPid >= 0) x["pcrPid"] = s.pcrPid;
        if (!s.name.isEmpty()) x["name"] = s.name;
        if (!s.provider.isEmpty()) x["provider"] = s.provider;
        if (s.major >= 0) {
            x["channel"] = QStringLiteral("%1.%2").arg(s.major).arg(s.minor);
            x["shortName"] = s.shortName;
            if (!s.longName.isEmpty()) x["longName"] = s.longName;
            x["sourceId"] = s.sourceId;
            x["hidden"] = s.hidden;
        }
        QJsonArray st;
        for (const Stream& e : s.streams) {
            QJsonObject y; y["pid"] = e.pid; y["streamType"] = e.streamType; y["type"] = streamTypeName(e.streamType);
            if (!e.language.isEmpty()) y["language"] = e.language;
            st.append(y);
        }
        x["streams"] = st;
        svcs.append(x);
    }
    o["services"] = svcs;

    QJsonArray tabs;
    for (const TableStats& t : m_tableStats) {
        QJsonObject x;
        x["pid"] = t.pid; x["tableId"] = t.tableId; x["name"] = tableName(t.tableId);
        x["sections"] = double(t.sections); x["crcErrors"] = double(t.crcErrors);
        if (t.version >= 0) x["version"] = t.version;
        if (!t.tidExts.isEmpty()) x["tableIdExtensions"] = int(t.tidExts.size());
        if (t.intervalMs.count) x["intervalMs"] = statsJson(t.intervalMs, 1);
        tabs.append(x);
    }
    o["tables"] = tabs;

    auto events = [&](const QList<Event>& list) {
        QJsonArray a;
        for (const Event& e : list) {
            QJsonObject x = e.json;
            x["packet"] = double(e.packet);
            if (e.pid >= 0) x["pid"] = e.pid;
            x["type"] = e.type;
            x["text"] = e.text;
            const double t = packetSeconds(e.packet);
            if (t >= 0) x["timeSec"] = std::round(t * 1000.0) / 1000.0;
            a.append(x);
        }
        return a;
    };
    o["errors"] = events(m_errors);
    o["scte35"] = events(m_scte35);
    return o;
}

}   // namespace tskit
