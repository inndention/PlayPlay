#include "psip.h"
#include "scte35bin.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>

#include <cstring>

namespace tskit {

// =============================================================================================== XML
namespace xml {

bool Element::hasAttribute(const QString& n) const {
    for (const auto& a : attributes) if (a.first.compare(n, Qt::CaseInsensitive) == 0) return true;
    return false;
}
QString Element::attribute(const QString& n) const {
    for (const auto& a : attributes) if (a.first.compare(n, Qt::CaseInsensitive) == 0) return a.second;
    return {};
}
const Element* Element::child(const char* n) const {
    for (const Element& c : children) if (c.is(n)) return &c;
    return nullptr;
}

namespace {
bool isWs(QChar c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool isNameStart(QChar c) { return c.isLetter() || c == '_' || c == ':'; }
bool isNameChar(QChar c) { return c.isLetterOrNumber() || c == '_' || c == ':' || c == '.' || c == '-'; }

// TSDuck's reader decodes the five predefined entities only; "&#233;" and unknown entities stay literal text.
QString decodeEntities(QStringView raw) {
    if (!raw.contains('&')) return raw.toString();
    static const struct { const char* name; char ch; } kEntities[] = {{"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}};
    QString out; out.reserve(raw.size());
    for (qsizetype i = 0; i < raw.size(); ++i) {
        if (raw[i] == '&') {
            bool hit = false;
            for (const auto& en : kEntities) {
                if (raw.mid(i + 1).startsWith(QLatin1String(en.name))) { out += QChar(en.ch); i += qsizetype(std::strlen(en.name)); hit = true; break; }
            }
            if (hit) continue;
        }
        out += raw[i];
    }
    return out;
}

class Parser {
public:
    explicit Parser(QString s) : m_s(std::move(s)) {}
    bool document(Element* root, QString* error) {
        if (!m_s.isEmpty() && m_s[0] == QChar(0xFEFF)) m_pos = 1;
        bool ok = misc();
        if (ok && (eof() || m_s[m_pos] != '<')) ok = fail("no root element");
        ok = ok && element(root, 0) && misc();
        if (ok && !eof()) ok = fail("content after the root element");
        if (!ok && error) *error = m_err;
        return ok;
    }
private:
    QString m_s;
    int m_pos = 0;
    int m_linePos = 0, m_line = 1;                        // line() counts forward only: m_pos never moves back
    QString m_err;

    int line() { for (; m_linePos < m_pos && m_linePos < m_s.size(); ++m_linePos) if (m_s[m_linePos] == '\n') ++m_line; return m_line; }
    bool fail(const QString& e) { if (m_err.isEmpty()) m_err = QString("XML line %1: %2").arg(line()).arg(e); return false; }
    bool eof() const { return m_pos >= m_s.size(); }
    bool at(const char* t) const { return QStringView(m_s).mid(m_pos).startsWith(QLatin1String(t)); }
    void skipWs() { while (!eof() && isWs(m_s[m_pos])) ++m_pos; }
    bool skipPast(const char* t) {
        const int i = int(m_s.indexOf(QLatin1String(t), m_pos));
        if (i < 0) return fail(QString("missing \"%1\"").arg(QLatin1String(t)));
        m_pos = i + int(std::strlen(t));
        return true;
    }
    // Whitespace, comments, processing instructions and DOCTYPE between top-level items.
    bool misc() {
        for (;;) {
            skipWs();
            if (at("<?")) { if (!skipPast("?>")) return false; }
            else if (at("<!--")) { if (!skipPast("-->")) return false; }
            else if (at("<!")) { if (!skipPast(">")) return false; }
            else return true;
        }
    }
    QString name() {
        const int start = m_pos;
        if (eof() || !isNameStart(m_s[m_pos])) return {};
        while (!eof() && isNameChar(m_s[m_pos])) ++m_pos;
        return m_s.mid(start, m_pos - start);
    }
    bool element(Element* e, int depth) {
        if (depth > 64) return fail("elements nested too deep");
        ++m_pos;                                          // '<'
        e->line = line();
        e->name = name();
        if (e->name.isEmpty()) return fail("bad element name");
        for (;;) {
            skipWs();
            if (eof()) return fail(QString("unterminated <%1>").arg(e->name));
            if (at("/>")) { m_pos += 2; return true; }
            if (m_s[m_pos] == '>') { ++m_pos; break; }
            const QString an = name();
            if (an.isEmpty()) return fail(QString("bad attribute in <%1>").arg(e->name));
            skipWs();
            if (eof() || m_s[m_pos] != '=') return fail(QString("attribute '%1' in <%2> has no value").arg(an, e->name));
            ++m_pos; skipWs();
            if (eof() || (m_s[m_pos] != '"' && m_s[m_pos] != '\'')) return fail(QString("attribute '%1' in <%2> is not quoted").arg(an, e->name));
            const QChar q = m_s[m_pos++];
            const int end = int(m_s.indexOf(q, m_pos));
            if (end < 0) return fail(QString("unterminated value of attribute '%1'").arg(an));
            const QStringView raw = QStringView(m_s).mid(m_pos, end - m_pos);
            if (raw.contains('<')) return fail(QString("'<' in the value of attribute '%1'").arg(an));
            if (e->hasAttribute(an)) return fail(QString("duplicate attribute '%1' in <%2>").arg(an, e->name));
            e->attributes.append({an, decodeEntities(raw)});   // raw newlines / tabs kept, like TSDuck
            m_pos = end + 1;
        }
        for (;;) {
            if (eof()) return fail(QString("missing </%1>").arg(e->name));
            if (at("</")) {
                m_pos += 2;
                const QString n = name();
                if (n.compare(e->name, Qt::CaseInsensitive) != 0) return fail(QString("</%1> closes <%2>").arg(n, e->name));
                skipWs();
                if (eof() || m_s[m_pos] != '>') return fail(QString("bad </%1>").arg(n));
                ++m_pos;
                return true;
            }
            if (at("<!--")) { if (!skipPast("-->")) return false; continue; }
            if (at("<?")) { if (!skipPast("?>")) return false; continue; }
            if (at("<![CDATA[")) {
                m_pos += 9;
                const int end = int(m_s.indexOf(QLatin1String("]]>"), m_pos));
                if (end < 0) return fail("unterminated CDATA");
                e->text += m_s.mid(m_pos, end - m_pos);
                m_pos = end + 3;
                continue;
            }
            if (m_s[m_pos] == '<') {
                Element c;
                if (!element(&c, depth + 1)) return false;
                e->children.push_back(std::move(c));
                continue;
            }
            const int lt = int(m_s.indexOf('<', m_pos));
            const int end = lt < 0 ? int(m_s.size()) : lt;
            e->text += decodeEntities(QStringView(m_s).mid(m_pos, end - m_pos));
            m_pos = end;
        }
    }
};
}   // namespace

bool parse(const QByteArray& utf8, Element* root, QString* error) {
    QString s = QString::fromUtf8(utf8);
    s.replace(QLatin1String("\r\n"), QLatin1String("\n")).replace('\r', '\n');   // XML end-of-line handling
    *root = Element();
    return Parser(std::move(s)).document(root, error);
}

// ----------------------------------------------------------------------------------------------- Reader
static QString where(const Element& e) { return QString("<%1>, line %2").arg(e.name).arg(e.line); }

void Reader::fail(const Element& e, const QString& msg) { if (m_error.isEmpty()) m_error = msg + " in " + where(e); }
void Reader::failUnsupported(const Element& e, const QString& msg) {
    if (!m_error.isEmpty()) return;
    m_error = "unsupported: " + msg + " in " + where(e);
    m_unsupported = true;
}
void Reader::failWith(const QString& msg, bool unsupported) {
    if (!m_error.isEmpty()) return;
    m_error = msg;
    m_unsupported = unsupported;
}

static bool parseUInt(QString s, quint64* out) {
    s = s.trimmed();
    s.remove(',');
    bool ok = false;
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        const QString h = s.mid(2);
        if (h.isEmpty()) return false;
        for (QChar c : h) if (!c.isDigit() && !(c.toLower() >= 'a' && c.toLower() <= 'f')) return false;
        *out = h.toULongLong(&ok, 16);
    } else {
        if (s.isEmpty()) return false;
        for (QChar c : s) if (c < '0' || c > '9') return false;
        *out = s.toULongLong(&ok, 10);
    }
    return ok;
}

quint64 Reader::uint(const Element& e, const char* name, int bits, bool required, quint64 def) {
    if (!ok()) return def;
    if (!e.hasAttribute(name)) { if (required) fail(e, QString("missing attribute '%1'").arg(name)); return def; }
    const QString v = e.attribute(name);
    quint64 x = 0;
    if (!parseUInt(v, &x)) { fail(e, QString("incorrect value '%1' for attribute '%2'").arg(v, name)); return def; }
    const quint64 max = bits >= 64 ? ~0ULL : (1ULL << bits) - 1;
    if (x > max) { fail(e, QString("value %1 of attribute '%2' is out of range (max %3)").arg(v, name).arg(max)); return def; }
    return x;
}

bool Reader::boolean(const Element& e, const char* name, bool required, bool def) {
    if (!ok()) return def;
    if (!e.hasAttribute(name)) { if (required) fail(e, QString("missing attribute '%1'").arg(name)); return def; }
    const QString v = e.attribute(name).trimmed().toLower();
    if (v == "true" || v == "yes" || v == "on" || v == "1") return true;
    if (v == "false" || v == "no" || v == "off" || v == "0") return false;
    fail(e, QString("incorrect boolean '%1' for attribute '%2'").arg(e.attribute(name), name));
    return def;
}

QString Reader::string(const Element& e, const char* name, bool required, int maxChars) {
    if (!ok()) return {};
    if (!e.hasAttribute(name)) { if (required) fail(e, QString("missing attribute '%1'").arg(name)); return {}; }
    const QString v = e.attribute(name);
    if (maxChars >= 0 && v.size() > maxChars)
        fail(e, QString("attribute '%1' has %2 characters, allowed 0 to %3").arg(name).arg(v.size()).arg(maxChars));
    return v;
}

QString Reader::char3(const Element& e, const char* name) {
    const QString v = string(e, name, true);
    if (ok() && v.size() != 3) fail(e, QString("attribute '%1' must be 3 characters, not \"%2\"").arg(name, v));
    return v;
}

int Reader::enumeration(const Element& e, const char* name, const QList<QPair<QString, int>>& names, int bits, bool required, int def) {
    if (!ok()) return def;
    if (!e.hasAttribute(name)) { if (required) fail(e, QString("missing attribute '%1'").arg(name)); return def; }
    const QString v = e.attribute(name).trimmed();
    for (const auto& n : names) if (n.first.compare(v, Qt::CaseInsensitive) == 0) return n.second;
    return int(uint(e, name, bits, required, quint64(def)));
}

qint64 Reader::gpsTime(const Element& e, const char* name) {
    const QString v = string(e, name, true);
    if (!ok()) return 0;
    // "YYYY-MM-DD hh:mm:ss": six integer fields (TSDuck accepts any separators between them).
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{1,4})\\D+(\\d{1,2})\\D+(\\d{1,2})\\D+(\\d{1,2})\\D+(\\d{1,2})\\D+(\\d{1,2})\\s*$"));
    const QRegularExpressionMatch m = re.match(v);
    const QDate d = m.hasMatch() ? QDate(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt()) : QDate();
    const QTime t = m.hasMatch() ? QTime(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt()) : QTime();
    const qint64 gps = d.isValid() && t.isValid() ? gpsSeconds(QDateTime(d, t, QTimeZone::utc())) : -1;
    if (gps < 0) { fail(e, QString("incorrect time '%1' for attribute '%2' (YYYY-MM-DD hh:mm:ss, from 1980-01-06)").arg(v, name)); return 0; }
    return gps;
}

QByteArray Reader::hexContent(const Element& e) {
    if (!ok()) return {};
    QByteArray digits;
    for (QChar c : e.text) {
        if (isWs(c)) continue;
        const char l = char(c.toLower().unicode());
        if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f')) || c.unicode() > 0x7F) { fail(e, QString("invalid hexadecimal content")); return {}; }
        digits.append(l);
    }
    if (digits.size() % 2) { fail(e, QString("odd number of hexadecimal digits")); return {}; }
    return QByteArray::fromHex(digits);
}

}   // namespace xml

// =============================================================================================== building blocks
namespace {
bool validMode(int m) { return m <= 0x06 || (m >= 0x09 && m <= 0x10) || (m >= 0x20 && m <= 0x27) || (m >= 0x30 && m <= 0x33); }

void appendSegments(QByteArray& out, const QString& text, EmptyText empty) {
    if (text.isEmpty()) {
        if (empty == EmptyText::OneSegment) out.append("\x01\x00\x00\x00", 4);   // one segment: compression 0, mode 0, 0 bytes
        else out.append('\0');                                                   // number_segments 0
        return;
    }
    int mode = text[0].unicode() >> 8;
    for (QChar c : text) if ((c.unicode() >> 8) != mode) { mode = -1; break; }
    QByteArray bytes;
    int maxChunk = 255;
    if (mode >= 0 && validMode(mode)) {
        for (QChar c : text) bytes.append(char(c.unicode() & 0xFF));
    } else {
        mode = 0x3F;
        for (QChar c : text) { bytes.append(char(c.unicode() >> 8)); bytes.append(char(c.unicode() & 0xFF)); }
        maxChunk = 254;
    }
    // number_bytes is 8 bits: longer text goes into more segments (tstabcomp overflows here, see psip.h).
    QList<QByteArray> chunks;
    for (int pos = 0; pos < bytes.size();) {
        int n = qMin(int(bytes.size()) - pos, maxChunk);
        if (mode == 0x3F && pos + n < bytes.size()) {
            const int unit = (quint8(bytes[pos + n - 2]) << 8) | quint8(bytes[pos + n - 1]);
            if (unit >= 0xD800 && unit <= 0xDBFF) n -= 2;  // keep a surrogate pair in one segment
        }
        chunks.append(bytes.mid(pos, n));
        pos += n;
    }
    if (chunks.size() > 255) chunks.resize(255);
    out.append(char(chunks.size()));
    for (const QByteArray& c : chunks) { out.append('\0'); out.append(char(mode)); out.append(char(c.size())); out.append(c); }
}

QByteArray languageBytes(const QString& lang) {
    const QString l = lang.leftJustified(3, ' ', true);
    QByteArray b;
    for (QChar c : l) b.append(char(c.unicode() & 0xFF));
    return b;
}
}   // namespace

QByteArray encodeMultiString(const MultiString& m, EmptyText empty) {
    QByteArray out;
    const int n = int(qMin<qsizetype>(m.strings.size(), 255));
    out.append(char(n));
    for (int i = 0; i < n; ++i) { out.append(languageBytes(m.strings[i].language)); appendSegments(out, m.strings[i].text, empty); }
    return out;
}

QByteArray encodeMultiStringWithLength(const MultiString& m, EmptyText empty) {
    if (m.isEmpty()) return QByteArray(1, '\0');
    const QByteArray s = encodeMultiString(m, empty);
    if (s.size() > 255) return {};                        // does not fit its uint8 length: the caller reports it
    return char(s.size()) + s;
}

bool decodeMultiString(const QByteArray& d, int* pos, int end, MultiString* out) {
    const auto at = [&](int i) { return quint8(d[i]); };
    int p = *pos;
    end = qMin(end, int(d.size()));
    if (p >= end) return false;
    MultiString m;
    const int n = at(p++);
    for (int i = 0; i < n; ++i) {
        if (p + 4 > end) return false;
        MultiString::Str s;
        s.language = QString::fromLatin1(d.constData() + p, 3);
        p += 3;
        const int segs = at(p++);
        for (int k = 0; k < segs; ++k) {
            if (p + 3 > end) return false;
            const int comp = at(p), mode = at(p + 1), nb = at(p + 2);
            p += 3;
            if (p + nb > end || comp != 0) return false;
            if (mode == 0x3F) {
                if (nb % 2) return false;
                for (int j = 0; j < nb; j += 2) s.text += QChar(char16_t((at(p + j) << 8) | at(p + j + 1)));
            } else if (validMode(mode)) {
                for (int j = 0; j < nb; ++j) s.text += QChar(char16_t((mode << 8) | at(p + j)));
            } else return false;
            p += nb;
        }
        m.strings.append(s);
    }
    *pos = p;
    if (out) *out = m;
    return true;
}

QByteArray encodeDescriptors(const Descriptors& ds) {
    QByteArray out;
    for (const Descriptor& d : ds) { out.append(char(d.tag)); out.append(char(qMin<qsizetype>(d.payload.size(), 255))); out.append(d.payload.left(255)); }
    return out;
}
int descriptorsSize(const Descriptors& ds) { int n = 0; for (const Descriptor& d : ds) n += 2 + int(qMin<qsizetype>(d.payload.size(), 255)); return n; }

Descriptor registrationDescriptor(quint32 id, const QByteArray& additional) {
    BitWriter w; w.u32(id); w.bytes(additional);
    return {0x05, w.data()};
}
Descriptor extendedChannelNameDescriptor(const MultiString& name) { return {0xA0, encodeMultiString(name)}; }

qint64 gpsSeconds(const QDateTime& utc) {
    static const QDateTime epoch(QDate(1980, 1, 6), QTime(0, 0), QTimeZone::utc());
    if (!utc.isValid()) return -1;
    const qint64 s = epoch.secsTo(utc);
    return s < 0 || s > 0xFFFFFFFFLL ? -1 : s;
}

// =============================================================================================== MPEG PSI
namespace {
// Greedy split of fixed-size entries into section payloads of at most maxPayload bytes.
QList<QByteArray> packEntries(const QList<QByteArray>& entries, int maxPayload) {
    QList<QByteArray> payloads;
    QByteArray cur;
    for (const QByteArray& e : entries) {
        if (!cur.isEmpty() && cur.size() + e.size() > maxPayload) { payloads.append(cur); cur.clear(); }
        cur.append(e);
    }
    payloads.append(cur);
    return payloads;
}
bool fits(const QList<QByteArray>& sections, int maxSection) {
    for (const QByteArray& s : sections) if (s.size() > maxSection) return false;
    return !sections.isEmpty();
}
}   // namespace

QList<QByteArray> buildPat(const Pat& t) {
    QList<QByteArray> entries;
    auto entry = [](int program, int pid) { BitWriter w; w.u16(program); w.reserved(3); w.bits(pid, 13); return w.data(); };
    if (t.networkPid >= 0) entries.append(entry(0, t.networkPid));
    for (const PatProgram& p : t.programs) entries.append(entry(p.programNumber, p.pid));
    LongHeader h; h.tableId = 0x00; h.privateIndicator = false; h.tableIdExt = t.tsid; h.version = t.version; h.current = t.current;
    return longTable(h, packEntries(entries, kMaxPsiSection - 12));
}

QList<QByteArray> buildPmt(const Pmt& t) {
    BitWriter w;
    w.reserved(3); w.bits(t.pcrPid, 13);
    w.reserved(4); w.bits(descriptorsSize(t.descriptors), 12); w.bytes(encodeDescriptors(t.descriptors));
    for (const PmtStream& s : t.streams) {
        w.u8(s.streamType); w.reserved(3); w.bits(s.pid, 13);
        w.reserved(4); w.bits(descriptorsSize(s.descriptors), 12); w.bytes(encodeDescriptors(s.descriptors));
    }
    LongHeader h; h.tableId = 0x02; h.privateIndicator = false; h.tableIdExt = t.programNumber; h.version = t.version; h.current = t.current;
    const QList<QByteArray> out = longTable(h, {w.data()});
    return fits(out, kMaxPsiSection) ? out : QList<QByteArray>();
}

namespace {
bool parseDescriptorLoop(const QByteArray& d, int pos, int len, Descriptors* out) {
    const int end = pos + len;
    if (end > d.size()) return false;
    while (pos < end) {
        if (pos + 2 > end) return false;
        const int tag = quint8(d[pos]), l = quint8(d[pos + 1]);
        if (pos + 2 + l > end) return false;
        out->append({tag, d.mid(pos + 2, l)});
        pos += 2 + l;
    }
    return true;
}
}   // namespace

bool parsePmt(const QByteArray& s, Pmt* out, QString* error) {
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    SectionInfo i;
    if (!sectionInfo(s, 0, &i, error)) return false;
    if (i.tableId != 0x02 || !i.longSyntax) return fail(QString("not a PMT section (table_id 0x%1)").arg(i.tableId, 2, 16, QChar('0')));
    if (i.size != s.size()) return fail("PMT section length does not match its data");
    if (!crcValid(s)) return fail("bad CRC32 in PMT");
    const auto u8 = [&](int p) { return int(quint8(s[p])); };
    Pmt t;
    t.version = i.version; t.current = i.current; t.programNumber = i.tableIdExt;
    const int end = i.size - 4;
    int p = 8;
    if (p + 4 > end) return fail("truncated PMT");
    t.pcrPid = ((u8(p) & 0x1F) << 8) | u8(p + 1);
    const int pil = ((u8(p + 2) & 0x0F) << 8) | u8(p + 3);
    p += 4;
    if (p + pil > end || !parseDescriptorLoop(s, p, pil, &t.descriptors)) return fail("bad program_info loop in PMT");
    p += pil;
    while (p < end) {
        if (p + 5 > end) return fail("truncated PMT component");
        PmtStream st;
        st.streamType = u8(p);
        st.pid = ((u8(p + 1) & 0x1F) << 8) | u8(p + 2);
        const int esl = ((u8(p + 3) & 0x0F) << 8) | u8(p + 4);
        p += 5;
        if (p + esl > end || !parseDescriptorLoop(s, p, esl, &st.descriptors)) return fail("bad ES_info loop in PMT");
        p += esl;
        t.streams.append(st);
    }
    if (out) *out = t;
    return true;
}

QByteArray pmtWithScte35(const QByteArray& section, int pid, QString* error) {
    Pmt t;
    if (!parsePmt(section, &t, error)) return {};
    static const QByteArray kCuei("CUEI");
    bool hasCuei = false;
    for (const Descriptor& d : t.descriptors) if (d.tag == 0x05 && d.payload.startsWith(kCuei)) hasCuei = true;
    if (!hasCuei) t.descriptors.append(registrationDescriptor(0x43554549));
    bool found = false;
    for (PmtStream& s : t.streams) if (s.pid == pid) { s.streamType = 0x86; found = true; }
    if (!found) { PmtStream s; s.streamType = 0x86; s.pid = pid; t.streams.append(s); }
    const QList<QByteArray> out = buildPmt(t);
    if (out.isEmpty()) { if (error) *error = "PMT with SCTE-35 does not fit in 1024 bytes"; return {}; }
    return out.first();
}

// =============================================================================================== ATSC PSIP
QList<QByteArray> buildMgt(const Mgt& t) {
    BitWriter w;
    w.u8(t.protocol);
    w.u16(t.tables.size());
    for (const MgtEntry& e : t.tables) {
        w.u16(e.type); w.reserved(3); w.bits(e.pid, 13); w.reserved(3); w.bits(e.version, 5); w.u32(e.numberBytes);
        w.reserved(4); w.bits(descriptorsSize(e.descriptors), 12); w.bytes(encodeDescriptors(e.descriptors));
    }
    w.reserved(4); w.bits(descriptorsSize(t.descriptors), 12); w.bytes(encodeDescriptors(t.descriptors));
    LongHeader h; h.tableId = 0xC7; h.tableIdExt = 0; h.version = t.version;
    const QList<QByteArray> out = longTable(h, {w.data()});
    return fits(out, kMaxPrivateSection) ? out : QList<QByteArray>();
}

namespace {
QByteArray encodeChannel(const VctChannel& c) {
    BitWriter w;
    for (int i = 0; i < 7; ++i) w.u16(i < c.shortName.size() ? c.shortName[i].unicode() : 0);   // UTF-16, zero padded
    w.reserved(4); w.bits(c.major, 10); w.bits(c.minor, 10);
    w.u8(c.modulation); w.u32(c.carrierFrequency); w.u16(c.channelTsid); w.u16(c.programNumber);
    w.bits(c.etmLocation, 2); w.bits(c.accessControlled, 1); w.bits(c.hidden, 1); w.reserved(2); w.bits(c.hideGuide, 1); w.reserved(3);
    w.bits(c.serviceType, 6); w.u16(c.sourceId);
    w.reserved(6); w.bits(descriptorsSize(c.descriptors), 10); w.bytes(encodeDescriptors(c.descriptors));
    return w.data();
}
QByteArray descriptorLoop10(const Descriptors& ds) {   // reserved(6) + length(10) + descriptors
    BitWriter w; w.reserved(6); w.bits(descriptorsSize(ds), 10); w.bytes(encodeDescriptors(ds));
    return w.data();
}
}   // namespace

QList<QByteArray> buildTvct(const Tvct& t) {
    // Channels packed greedily into 1024-byte sections, each closed by an additional_descriptors loop that is empty except
    // in the last section (what TSDuck does; the big-TVCT probe in the DEP-14 log confirms 9 x 106-byte channels/section).
    const int maxPayload = kMaxPsiSection - 12;
    QList<QByteArray> payloads;
    QByteArray cur; int count = 0;
    auto close = [&](const Descriptors& ds) {
        QByteArray p; p.append(char(t.protocol)); p.append(char(count)); p.append(cur); p.append(descriptorLoop10(ds));
        payloads.append(p); cur.clear(); count = 0;
    };
    for (const VctChannel& c : t.channels) {
        const QByteArray e = encodeChannel(c);
        if (count > 0 && 2 + cur.size() + e.size() + 2 > maxPayload) close({});
        cur.append(e); ++count;
    }
    if (count > 0 && 2 + cur.size() + 2 + descriptorsSize(t.descriptors) > maxPayload) close({});
    close(t.descriptors);
    LongHeader h; h.tableId = 0xC8; h.tableIdExt = t.tsid; h.version = t.version; h.current = t.current;
    const QList<QByteArray> out = longTable(h, payloads);
    return fits(out, kMaxPsiSection) && out.size() <= 256 ? out : QList<QByteArray>();
}

QList<QByteArray> buildRrt(const Rrt& t) {
    QByteArray p;
    p.append(char(t.protocol));
    const QByteArray rn = encodeMultiStringWithLength(t.regionName);
    if (rn.isEmpty()) return {};
    p.append(rn);
    p.append(char(t.dimensions.size()));
    for (const RrtDimension& d : t.dimensions) {
        const QByteArray dn = encodeMultiStringWithLength(d.name);
        if (dn.isEmpty() || d.values.size() > 15) return {};
        p.append(dn);
        p.append(char(0xE0 | (d.graduated ? 0x10 : 0x00) | d.values.size()));
        for (const RrtValue& v : d.values) {
            const QByteArray a = encodeMultiStringWithLength(v.abbrev), r = encodeMultiStringWithLength(v.value);
            if (a.isEmpty() || r.isEmpty()) return {};
            p.append(a); p.append(r);
        }
    }
    p.append(descriptorLoop10(t.descriptors));
    LongHeader h; h.tableId = 0xCA; h.tableIdExt = 0xFF00 | (t.region & 0xFF); h.version = t.version;
    const QList<QByteArray> out = longTable(h, {p});
    return fits(out, kMaxPrivateSection) ? out : QList<QByteArray>();
}

QList<QByteArray> buildEit(const Eit& t) {
    const int maxPayload = kMaxPrivateSection - 12;
    QList<QByteArray> payloads;
    QByteArray cur; int count = 0;
    auto close = [&]() { QByteArray p; p.append(char(t.protocol)); p.append(char(count)); p.append(cur); payloads.append(p); cur.clear(); count = 0; };
    for (const EitEvent& e : t.events) {
        const QByteArray title = e.title.isEmpty() ? QByteArray(1, '\0') : encodeMultiStringWithLength(e.title, EmptyText::OneSegment);
        if (title.isEmpty()) return {};
        BitWriter w;
        w.reserved(2); w.bits(e.eventId, 14); w.u32(e.startTime);
        w.reserved(2); w.bits(e.etmLocation, 2); w.bits(e.lengthSeconds, 20);
        w.bytes(title);
        w.reserved(4); w.bits(descriptorsSize(e.descriptors), 12); w.bytes(encodeDescriptors(e.descriptors));
        if (count > 0 && 2 + cur.size() + w.size() > maxPayload) close();
        cur.append(w.data()); ++count;
    }
    close();
    LongHeader h; h.tableId = 0xCB; h.tableIdExt = t.sourceId; h.version = t.version;
    const QList<QByteArray> out = longTable(h, payloads);
    return fits(out, kMaxPrivateSection) && out.size() <= 256 ? out : QList<QByteArray>();
}

QList<QByteArray> buildEtt(const Ett& t) {
    BitWriter w;
    w.u8(t.protocol); w.u32(t.etmId); w.bytes(encodeMultiString(t.text));
    LongHeader h; h.tableId = 0xCC; h.tableIdExt = t.tableIdExt; h.version = t.version;
    const QList<QByteArray> out = longTable(h, {w.data()});
    return fits(out, kMaxPrivateSection) ? out : QList<QByteArray>();
}

QList<QByteArray> buildStt(const Stt& t) {
    BitWriter w;
    w.u8(t.protocol); w.u32(t.systemTime); w.u8(t.gpsUtcOffset);
    w.bits(t.dsStatus, 1); w.reserved(2); w.bits(t.dsDayOfMonth, 5); w.u8(t.dsHour);
    w.bytes(encodeDescriptors(t.descriptors));          // no length field: descriptors run to the CRC
    LongHeader h; h.tableId = 0xCD; h.tableIdExt = 0; h.version = 0;
    const QList<QByteArray> out = longTable(h, {w.data()});
    return fits(out, kMaxPrivateSection) ? out : QList<QByteArray>();
}

// =============================================================================================== XML -> tables
namespace {
using xml::Element;
using xml::Reader;

// <x><string language=".." text=".."/>...</x>; a missing element is an empty structure.
MultiString multiStringFromXml(Reader& r, const Element* e) {
    MultiString m;
    if (!e) return m;
    for (const Element& c : e->children) {
        if (c.is("metadata")) continue;
        if (!c.is("string")) { r.fail(c, QString("unexpected <%1> in <%2>").arg(c.name, e->name)); return m; }
        MultiString::Str s; s.language = r.char3(c, "language"); s.text = r.string(c, "text", true);
        m.strings.append(s);
    }
    return m;
}

QByteArray languageCode(Reader& r, const Element& e, const char* attr, bool required) {
    if (!required && !e.hasAttribute(attr)) return QByteArray(3, '\0');
    return languageBytes(r.char3(e, attr));
}

bool descriptorFromXml(Reader& r, const Element& e, Descriptor* d) {
    BitWriter w;
    if (e.is("registration_descriptor")) {
        d->tag = 0x05;
        w.u32(quint32(r.uint(e, "format_identifier", 32, true)));
        if (const Element* a = e.child("additional_identification_info")) w.bytes(r.hexContent(*a));
    } else if (e.is("ISO_639_language_descriptor")) {
        d->tag = 0x0A;
        for (const Element& c : e.children) {
            if (!c.is("language")) { r.fail(c, "unexpected element"); return false; }
            w.bytes(languageCode(r, c, "code", true)); w.u8(quint32(r.uint(c, "audio_type", 8, true)));
        }
    } else if (e.is("extended_channel_name_descriptor")) {
        d->tag = 0xA0;
        w.bytes(encodeMultiString(multiStringFromXml(r, e.child("long_channel_name_text"))));
    } else if (e.is("service_location_descriptor")) {
        d->tag = 0xA1;
        QList<const Element*> comps;
        for (const Element& c : e.children) { if (!c.is("component")) { r.fail(c, "unexpected element"); return false; } comps.append(&c); }
        w.reserved(3); w.bits(r.uint(e, "PCR_PID", 13, false, kNullPid), 13); w.u8(quint32(comps.size()));
        for (const Element* c : comps) {
            w.u8(quint32(r.uint(*c, "stream_type", 8, true))); w.reserved(3); w.bits(r.uint(*c, "elementary_PID", 13, true), 13);
            w.bytes(languageCode(r, *c, "ISO_639_language_code", false));
        }
    } else if (e.is("caption_service_descriptor")) {
        d->tag = 0x86;
        QList<const Element*> svcs;
        for (const Element& c : e.children) { if (!c.is("service")) { r.fail(c, "unexpected element"); return false; } svcs.append(&c); }
        w.reserved(3); w.bits(svcs.size(), 5);
        for (const Element* c : svcs) {
            w.bytes(languageCode(r, *c, "language", true));
            const bool digital = r.boolean(*c, "digital_cc", true);
            w.bits(digital, 1); w.reserved(1);
            if (digital) w.bits(r.uint(*c, "caption_service_number", 6, true), 6);
            else { w.reserved(5); w.bits(r.boolean(*c, "line21_field", false), 1); }
            w.bits(r.boolean(*c, "easy_reader", true), 1); w.bits(r.boolean(*c, "wide_aspect_ratio", true), 1); w.reserved(14);
        }
    } else if (e.is("content_advisory_descriptor")) {
        d->tag = 0x87;
        QList<const Element*> regions;
        for (const Element& c : e.children) { if (!c.is("region")) { r.fail(c, "unexpected element"); return false; } regions.append(&c); }
        w.reserved(2); w.bits(regions.size(), 6);
        for (const Element* g : regions) {
            w.u8(quint32(r.uint(*g, "rating_region", 8, true)));
            QList<const Element*> dims;
            const Element* desc = nullptr;
            for (const Element& c : g->children) {
                if (c.is("dimension")) dims.append(&c);
                else if (c.is("rating_description")) desc = &c;
                else { r.fail(c, "unexpected element"); return false; }
            }
            w.u8(quint32(dims.size()));
            for (const Element* x : dims) { w.u8(quint32(r.uint(*x, "rating_dimension_j", 8, true))); w.reserved(4); w.bits(r.uint(*x, "rating_value", 4, true), 4); }
            const QByteArray ms = encodeMultiStringWithLength(multiStringFromXml(r, desc));
            if (ms.isEmpty()) { r.fail(*g, "rating_description longer than 255 bytes"); return false; }
            w.bytes(ms);
        }
    } else if (e.is("generic_descriptor")) {
        d->tag = int(r.uint(e, "tag", 8, true));
        w.bytes(r.hexContent(e));
    } else {
        r.failUnsupported(e, QString("descriptor <%1>").arg(e.name));
        return false;
    }
    if (!r.ok()) return false;
    d->payload = w.data();
    if (d->payload.size() > 255) { r.fail(e, QString("descriptor payload is %1 bytes (max 255)").arg(d->payload.size())); return false; }
    return true;
}

// Children of `e`: the named structural ones go to `onChild`, <metadata> is skipped, everything else is a descriptor.
template <typename F>
void walk(Reader& r, const Element& e, Descriptors* descs, const QStringList& structural, F onChild) {
    for (const Element& c : e.children) {
        if (!r.ok()) return;
        if (c.is("metadata")) continue;
        bool hit = false;
        for (const QString& n : structural) if (c.name.compare(n, Qt::CaseInsensitive) == 0) { onChild(c); hit = true; break; }
        if (hit) continue;
        if (!descs) { r.fail(c, QString("unexpected <%1> in <%2>").arg(c.name, e.name)); return; }
        Descriptor d;
        if (descriptorFromXml(r, c, &d)) descs->append(d);
    }
}

int version(Reader& r, const Element& e) { return int(r.uint(e, "version", 5, false, 0)); }

QList<QByteArray> patFromXml(Reader& r, const Element& e) {
    Pat t; t.version = version(r, e); t.current = r.boolean(e, "current", false, true); t.tsid = int(r.uint(e, "transport_stream_id", 16, true));
    if (e.hasAttribute("network_PID")) t.networkPid = int(r.uint(e, "network_PID", 13, true));
    walk(r, e, nullptr, {"service"}, [&](const Element& c) { t.programs.append({int(r.uint(c, "service_id", 16, true)), int(r.uint(c, "program_map_PID", 13, true))}); });
    return r.ok() ? buildPat(t) : QList<QByteArray>();
}

QList<QByteArray> pmtFromXml(Reader& r, const Element& e) {
    Pmt t; t.version = version(r, e); t.current = r.boolean(e, "current", false, true);
    t.programNumber = int(r.uint(e, "service_id", 16, true)); t.pcrPid = int(r.uint(e, "PCR_PID", 13, false, kNullPid));
    walk(r, e, &t.descriptors, {"component"}, [&](const Element& c) {
        PmtStream s; s.streamType = int(r.uint(c, "stream_type", 8, true)); s.pid = int(r.uint(c, "elementary_PID", 13, true));
        walk(r, c, &s.descriptors, {}, [](const Element&) {});
        t.streams.append(s);
    });
    return r.ok() ? buildPmt(t) : QList<QByteArray>();
}

int mgtTableType(Reader& r, const Element& e) {
    const QString v = e.attribute("type").trimmed();
    static const QList<QPair<QString, int>> fixed = {{"TVCT-current", 0x0000}, {"TVCT-next", 0x0001}, {"CVCT-current", 0x0002}, {"CVCT-next", 0x0003}, {"ETT", 0x0004}, {"DCCSCT", 0x0005}};
    for (const auto& f : fixed) if (f.first.compare(v, Qt::CaseInsensitive) == 0) return f.second;
    static const QRegularExpression re(QStringLiteral("^(EIT|ETT|RRT|DCCT)-(\\d+)$"), QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = re.match(v);
    if (m.hasMatch()) {
        const QString k = m.captured(1).toUpper(); const int n = m.captured(2).toInt();
        if (k == "EIT" && n <= 127) return 0x0100 + n;
        if (k == "ETT" && n <= 127) return 0x0200 + n;
        if (k == "RRT" && n >= 1 && n <= 255) return 0x0300 + n;
        if (k == "DCCT" && n <= 255) return 0x1400 + n;
    }
    return int(r.uint(e, "type", 16, true));
}

QList<QByteArray> mgtFromXml(Reader& r, const Element& e) {
    Mgt t; t.version = version(r, e); t.protocol = int(r.uint(e, "protocol_version", 8, false, 0));
    walk(r, e, &t.descriptors, {"table"}, [&](const Element& c) {
        MgtEntry m; m.type = mgtTableType(r, c); m.pid = int(r.uint(c, "PID", 13, true)); m.version = int(r.uint(c, "version_number", 5, true));
        m.numberBytes = quint32(r.uint(c, "number_bytes", 32, true));
        walk(r, c, &m.descriptors, {}, [](const Element&) {});
        t.tables.append(m);
    });
    return r.ok() ? buildMgt(t) : QList<QByteArray>();
}

QList<QByteArray> tvctFromXml(Reader& r, const Element& e) {
    static const QList<QPair<QString, int>> modulation = {{"analog", 0x01}, {"64-QAM", 0x02}, {"256-QAM", 0x03}, {"8-VSB", 0x04}, {"16-VSB", 0x05}};
    static const QList<QPair<QString, int>> serviceType = {{"analog", 0x01}, {"dtv", 0x02}, {"audio", 0x03}, {"data", 0x04}, {"software", 0x05}};
    Tvct t; t.version = version(r, e); t.current = r.boolean(e, "current", false, true); t.protocol = int(r.uint(e, "protocol_version", 8, false, 0));
    t.tsid = int(r.uint(e, "transport_stream_id", 16, true));
    walk(r, e, &t.descriptors, {"channel"}, [&](const Element& c) {
        VctChannel ch;
        ch.shortName = r.string(c, "short_name", true, 7);
        ch.major = int(r.uint(c, "major_channel_number", 10, true)); ch.minor = int(r.uint(c, "minor_channel_number", 10, true));
        ch.modulation = r.enumeration(c, "modulation_mode", modulation, 8, true);
        ch.carrierFrequency = quint32(r.uint(c, "carrier_frequency", 32, false, 0));
        ch.channelTsid = int(r.uint(c, "channel_TSID", 16, true)); ch.programNumber = int(r.uint(c, "program_number", 16, true));
        ch.etmLocation = int(r.uint(c, "ETM_location", 2, false, 0));
        ch.accessControlled = r.boolean(c, "access_controlled", false); ch.hidden = r.boolean(c, "hidden", false); ch.hideGuide = r.boolean(c, "hide_guide", false);
        ch.serviceType = r.enumeration(c, "service_type", serviceType, 6, false, 0x02);
        ch.sourceId = int(r.uint(c, "source_id", 16, true));
        walk(r, c, &ch.descriptors, {}, [](const Element&) {});
        t.channels.append(ch);
    });
    return r.ok() ? buildTvct(t) : QList<QByteArray>();
}

QList<QByteArray> rrtFromXml(Reader& r, const Element& e) {
    Rrt t; t.version = version(r, e); t.protocol = int(r.uint(e, "protocol_version", 8, false, 0)); t.region = int(r.uint(e, "rating_region", 8, true));
    walk(r, e, &t.descriptors, {"rating_region_name", "dimension"}, [&](const Element& c) {
        if (c.is("rating_region_name")) { t.regionName = multiStringFromXml(r, &c); return; }
        RrtDimension d; d.graduated = r.boolean(c, "graduated_scale", true);
        walk(r, c, nullptr, {"dimension_name", "value"}, [&](const Element& x) {
            if (x.is("dimension_name")) { d.name = multiStringFromXml(r, &x); return; }
            RrtValue v; v.abbrev = multiStringFromXml(r, x.child("abbrev_rating_value")); v.value = multiStringFromXml(r, x.child("rating_value"));
            for (const Element& y : x.children) if (!y.is("abbrev_rating_value") && !y.is("rating_value") && !y.is("metadata")) r.fail(y, "unexpected element");
            d.values.append(v);
        });
        if (d.values.size() > 15) r.fail(c, "more than 15 values in a dimension");
        t.dimensions.append(d);
    });
    if (t.dimensions.size() > 255) r.fail(e, "more than 255 dimensions");
    return r.ok() ? buildRrt(t) : QList<QByteArray>();
}

QList<QByteArray> eitFromXml(Reader& r, const Element& e) {
    Eit t; t.version = version(r, e); t.protocol = int(r.uint(e, "protocol_version", 8, false, 0)); t.sourceId = int(r.uint(e, "source_id", 16, true));
    walk(r, e, nullptr, {"event"}, [&](const Element& c) {
        EitEvent ev;
        ev.eventId = int(r.uint(c, "event_id", 14, true));            // 14-bit field; tstabcomp refuses more too
        ev.startTime = quint32(r.gpsTime(c, "start_time"));
        ev.etmLocation = int(r.uint(c, "ETM_location", 2, true)); ev.lengthSeconds = quint32(r.uint(c, "length_in_seconds", 20, true));
        walk(r, c, &ev.descriptors, {"title_text"}, [&](const Element& x) { ev.title = multiStringFromXml(r, &x); });
        if (!ev.title.isEmpty() && encodeMultiStringWithLength(ev.title, EmptyText::OneSegment).isEmpty()) r.fail(c, "title_text longer than 255 bytes");
        t.events.append(ev);
    });
    if (t.events.size() > 255 * 256) r.fail(e, "too many events");
    return r.ok() ? buildEit(t) : QList<QByteArray>();
}

QList<QByteArray> ettFromXml(Reader& r, const Element& e) {
    Ett t; t.version = version(r, e); t.protocol = int(r.uint(e, "protocol_version", 8, false, 0));
    t.tableIdExt = int(r.uint(e, "ETT_table_id_extension", 16, true)); t.etmId = quint32(r.uint(e, "ETM_id", 32, true));
    walk(r, e, nullptr, {"extended_text_message"}, [&](const Element& x) { t.text = multiStringFromXml(r, &x); });
    return r.ok() ? buildEtt(t) : QList<QByteArray>();
}

QList<QByteArray> sttFromXml(Reader& r, const Element& e) {
    Stt t; t.protocol = int(r.uint(e, "protocol_version", 8, false, 0)); t.systemTime = quint32(r.uint(e, "system_time", 32, true));
    t.gpsUtcOffset = int(r.uint(e, "GPS_UTC_offset", 8, true)); t.dsStatus = r.boolean(e, "DS_status", true);
    t.dsDayOfMonth = int(r.uint(e, "DS_day_of_month", 5, false, 0)); t.dsHour = int(r.uint(e, "DS_hour", 8, false, 0));
    walk(r, e, &t.descriptors, {}, [](const Element&) {});
    return r.ok() ? buildStt(t) : QList<QByteArray>();
}

QList<QByteArray> genericShortFromXml(Reader& r, const Element& e) {
    const int tid = int(r.uint(e, "table_id", 8, true)); const bool priv = r.boolean(e, "private", false, true);
    const QByteArray payload = r.hexContent(e);
    if (r.ok() && payload.size() > kMaxPrivateSection - 3) r.fail(e, "payload too large");
    return r.ok() ? QList<QByteArray>{shortSection(tid, priv, payload)} : QList<QByteArray>();
}

QList<QByteArray> genericLongFromXml(Reader& r, const Element& e) {
    LongHeader h; h.tableId = int(r.uint(e, "table_id", 8, true)); h.tableIdExt = int(r.uint(e, "table_id_ext", 16, false, 0xFFFF));
    h.version = version(r, e); h.current = r.boolean(e, "current", false, true); h.privateIndicator = r.boolean(e, "private", false, true);
    QList<QByteArray> payloads;
    walk(r, e, nullptr, {"section"}, [&](const Element& c) { payloads.append(r.hexContent(c)); });
    if (r.ok() && (payloads.isEmpty() || payloads.size() > 256)) r.fail(e, "a generic_long_table needs 1 to 256 <section>");
    const QList<QByteArray> out = r.ok() ? longTable(h, payloads) : QList<QByteArray>();
    return fits(out, kMaxPrivateSection) ? out : QList<QByteArray>();
}

QList<QByteArray> spliceFromXml(Reader& r, const Element& e) {
    scte35::SpliceInfo s; QString err; bool unsupported = false;
    if (!scte35::fromXml(e, &s, &err, &unsupported)) { r.failWith(err, unsupported); return {}; }
    const QByteArray sec = scte35::buildSection(s);
    return sec.size() <= kMaxPrivateSection ? QList<QByteArray>{sec} : QList<QByteArray>();
}
}   // namespace

QByteArray CompileResult::bin() const {
    QByteArray b;
    for (const CompiledTable& t : tables) for (const QByteArray& s : t.sections) b.append(s);
    return b;
}
int CompileResult::sectionCount() const { int n = 0; for (const CompiledTable& t : tables) n += int(t.sections.size()); return n; }

CompileResult compileXml(const QByteArray& utf8) {
    CompileResult res;
    xml::Element root;
    if (!xml::parse(utf8, &root, &res.error)) return res;
    if (!root.is("tsduck")) { res.error = QString("root element is <%1>, expected <tsduck>").arg(root.name); return res; }
    using Builder = QList<QByteArray> (*)(Reader&, const Element&);
    static const QList<QPair<const char*, Builder>> kTables = {
        {"PAT", patFromXml}, {"PMT", pmtFromXml}, {"MGT", mgtFromXml}, {"TVCT", tvctFromXml}, {"RRT", rrtFromXml}, {"ATSC_EIT", eitFromXml},
        {"ETT", ettFromXml}, {"STT", sttFromXml}, {"splice_information_table", spliceFromXml},
        {"generic_short_table", genericShortFromXml}, {"generic_long_table", genericLongFromXml}};
    Reader r;
    for (const Element& e : root.children) {
        Builder b = nullptr;
        for (const auto& k : kTables) if (e.is(k.first)) { b = k.second; break; }
        if (!b) { r.failUnsupported(e, QString("table <%1>").arg(e.name)); break; }
        CompiledTable t; t.element = e.name; t.line = e.line; t.sections = b(r, e);
        if (r.ok() && t.sections.isEmpty()) r.fail(e, "table does not fit its sections (a field or the table is too long)");
        if (!r.ok()) break;
        res.tables.append(t);
    }
    if (!r.ok()) { res.error = r.error(); res.unsupported = r.unsupported(); res.tables.clear(); return res; }
    res.ok = true;
    return res;
}

CompileResult compileFile(const QString& xmlPath, const QString& binPath) {
    CompileResult res;
    QFile f(xmlPath);
    if (!f.open(QIODevice::ReadOnly)) { res.error = QString("cannot read %1").arg(xmlPath); return res; }
    res = compileXml(f.readAll());
    if (!res.ok) { res.error = QFileInfo(xmlPath).fileName() + ": " + res.error; return res; }
    if (binPath.isEmpty()) return res;
    QSaveFile o(binPath);
    if (!o.open(QIODevice::WriteOnly) || o.write(res.bin()) < 0 || !o.commit()) { res.ok = false; res.error = QString("cannot write %1").arg(binPath); }
    return res;
}

}   // namespace tskit
