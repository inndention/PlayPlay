#include "scte35bin.h"

#include <cmath>

namespace tskit {
namespace scte35 {

namespace {
constexpr quint64 k33 = 0x1FFFFFFFFULL;

void putTime(BitWriter& w, const SpliceTime& t) {
    if (t.specified) { w.bits(1, 1); w.reserved(6); w.bits(t.pts & k33, 33); }
    else { w.bits(0, 1); w.reserved(7); }
}

// Big-endian reader over one section with a sticky failure flag (every read past the end yields 0).
struct Rd {
    const QByteArray& d; int p; int end; bool bad = false;
    int u8() { if (p + 1 > end) { bad = true; return 0; } return quint8(d[p++]); }
    quint32 u16() { const quint32 a = quint32(u8()); return (a << 8) | quint32(u8()); }
    quint32 u32() { const quint32 a = u16(); return (a << 16) | u16(); }
    SpliceTime time() {
        SpliceTime t; const int b = u8();
        if (b & 0x80) { t.specified = true; t.pts = (quint64(b & 1) << 32) | u32(); }
        return t;
    }
};
}   // namespace

QByteArray buildSection(const SpliceInfo& s) {
    BitWriter cmd;
    if (s.command == SpliceInsert) {
        const Insert& i = s.insert;
        cmd.u32(i.eventId); cmd.bits(i.cancel, 1); cmd.reserved(7);
        if (!i.cancel) {
            const bool program = i.components.isEmpty();
            // TSDuck writes the four bits after splice_immediate_flag as '1111' (event_id_compliance_flag + reserved).
            cmd.bits(i.outOfNetwork, 1); cmd.bits(program, 1); cmd.bits(i.hasDuration, 1); cmd.bits(i.immediate, 1); cmd.reserved(4);
            if (program && !i.immediate) putTime(cmd, i.time);
            if (!program) {
                cmd.u8(quint32(i.components.size()));
                for (const Component& c : i.components) { cmd.u8(c.tag); if (!i.immediate) putTime(cmd, c.time); }
            }
            if (i.hasDuration) { cmd.bits(i.autoReturn, 1); cmd.reserved(6); cmd.bits(i.duration & k33, 33); }
            cmd.u16(i.uniqueProgramId); cmd.u8(i.availNum); cmd.u8(i.availsExpected);
        }
    } else if (s.command == TimeSignal) {
        putTime(cmd, s.timeSignal);
    }                                                     // splice_null / bandwidth_reservation: empty command
    const QByteArray c = cmd.data();
    // section_length counts: protocol_version(1) + encryption/pts_adjustment(5) + cw_index(1) + tier/command length(3)
    // + command type(1) + command + descriptor_loop_length(2) + descriptors + CRC(4).
    const int len = 11 + int(c.size()) + 2 + int(s.descriptors.size()) + 4;
    BitWriter w;
    w.u8(0xFC); w.bits(0, 1); w.bits(0, 1); w.reserved(2); w.bits(len, 12);    // sap_type '11' = not specified
    w.u8(s.protocolVersion);
    w.bits(0, 1); w.bits(0, 6); w.bits(s.ptsAdjustment & k33, 33);             // not encrypted
    w.u8(s.cwIndex); w.bits(s.tier, 12); w.bits(c.size(), 12); w.u8(s.command);
    w.bytes(c);
    w.u16(quint32(s.descriptors.size())); w.bytes(s.descriptors);
    QByteArray sec = w.data();
    sec.append(QByteArray(4, '\0'));
    fixCrc(sec);
    return sec;
}

bool parseSection(const QByteArray& sec, SpliceInfo* out, QString* error) {
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    SectionInfo info;
    if (!sectionInfo(sec, 0, &info, error)) return false;
    if (info.tableId != 0xFC) return fail(QString("not a splice_info_section (table_id 0x%1)").arg(info.tableId, 2, 16, QChar('0')));
    if (info.size < 3 + 11 + 2 + 4) return fail("splice_info_section too short");
    if (!crcValid(sec.left(info.size))) return fail("bad CRC32 in splice_info_section");
    Rd r{sec, 3, info.size - 4};
    SpliceInfo s;
    s.protocolVersion = r.u8();
    const int b = r.u8();
    if (b & 0x80) return fail("encrypted splice_info_section");
    s.ptsAdjustment = (quint64(b & 1) << 32) | r.u32();
    s.cwIndex = r.u8();
    const quint32 tlHigh = quint32(r.u8());              // separate statements: operands of | are unsequenced
    const quint32 tl = (tlHigh << 16) | r.u16();
    s.tier = int(tl >> 12);
    const int cmdLen = int(tl & 0xFFF);
    s.command = r.u8();
    const int cmdStart = r.p;
    if (s.command == SpliceInsert) {
        Insert& i = s.insert;
        i.eventId = r.u32();
        i.cancel = r.u8() & 0x80;
        if (!i.cancel) {
            const int f = r.u8();
            i.outOfNetwork = f & 0x80; const bool program = f & 0x40; i.hasDuration = f & 0x20; i.immediate = f & 0x10;
            if (program && !i.immediate) i.time = r.time();
            if (!program) {
                const int n = r.u8();
                for (int k = 0; k < n && !r.bad; ++k) { Component c; c.tag = r.u8(); if (!i.immediate) c.time = r.time(); i.components.append(c); }
            }
            if (i.hasDuration) { const int d = r.u8(); i.autoReturn = d & 0x80; i.duration = (quint64(d & 1) << 32) | r.u32(); }
            i.uniqueProgramId = int(r.u16()); i.availNum = r.u8(); i.availsExpected = r.u8();
        }
    } else if (s.command == TimeSignal) {
        s.timeSignal = r.time();
    } else if (s.command != SpliceNull && s.command != BandwidthReservation) {
        r.p = cmdStart + cmdLen;                          // splice_schedule / private_command: skipped, kept as a type only
    }
    if (cmdLen != 0xFFF && r.p != cmdStart + cmdLen) return fail(QString("splice_command_length %1 does not match the command (%2 bytes)").arg(cmdLen).arg(r.p - cmdStart));
    const int dl = int(r.u16());
    if (r.bad || r.p + dl > r.end) return fail("truncated splice_info_section");
    s.descriptors = sec.mid(r.p, dl);
    if (out) *out = s;
    return true;
}

QByteArray immediateInsert(quint32 eventId, bool outOfNetwork, double durationSec, bool autoReturn, int uniqueProgramId, int availNum, int availsExpected) {
    SpliceInfo s; s.command = SpliceInsert;
    Insert& i = s.insert;
    i.eventId = eventId; i.outOfNetwork = outOfNetwork; i.immediate = true;
    i.uniqueProgramId = qBound(0, uniqueProgramId, 65535); i.availNum = qBound(0, availNum, 255); i.availsExpected = qBound(0, availsExpected, 255);
    if (durationSec > 0) { i.hasDuration = true; i.autoReturn = autoReturn; i.duration = quint64(std::llround(durationSec * 90000.0)) & k33; }
    return buildSection(s);
}

QByteArray cancelSection(quint32 eventId) {
    SpliceInfo s; s.command = SpliceInsert; s.insert.eventId = eventId; s.insert.cancel = true;
    return buildSection(s);
}

QByteArray nullSection() { return buildSection(SpliceInfo()); }

bool fromXml(const xml::Element& e, SpliceInfo* out, QString* error, bool* unsupported) {
    xml::Reader r;
    SpliceInfo s;
    s.protocolVersion = int(r.uint(e, "protocol_version", 8, false, 0));
    s.ptsAdjustment = r.uint(e, "pts_adjustment", 33, false, 0);
    s.tier = int(r.uint(e, "tier", 12, false, 0xFFF));
    int commands = 0;
    for (const xml::Element& c : e.children) {
        if (!r.ok()) break;
        if (c.is("metadata")) continue;
        if (c.is("splice_null")) { s.command = SpliceNull; ++commands; }
        else if (c.is("bandwidth_reservation")) { s.command = BandwidthReservation; ++commands; }
        else if (c.is("time_signal")) {
            s.command = TimeSignal; ++commands;
            if (c.hasAttribute("pts_time")) { s.timeSignal.specified = true; s.timeSignal.pts = r.uint(c, "pts_time", 33, true); }
        } else if (c.is("splice_insert")) {
            s.command = SpliceInsert; ++commands;
            Insert& i = s.insert;
            i.eventId = quint32(r.uint(c, "splice_event_id", 32, true));
            i.cancel = r.boolean(c, "splice_event_cancel", false, false);
            if (i.cancel) continue;                       // only the event id matters for a cancel
            i.outOfNetwork = r.boolean(c, "out_of_network", true);
            i.immediate = r.boolean(c, "splice_immediate", false, false);
            i.uniqueProgramId = int(r.uint(c, "unique_program_id", 16, true));
            i.availNum = int(r.uint(c, "avail_num", 8, false, 0)); i.availsExpected = int(r.uint(c, "avails_expected", 8, false, 0));
            for (const xml::Element& x : c.children) {
                if (x.is("metadata")) continue;
                if (x.is("break_duration")) { i.hasDuration = true; i.autoReturn = r.boolean(x, "auto_return", true); i.duration = r.uint(x, "duration", 33, true); }
                else if (x.is("component")) {
                    Component k; k.tag = int(r.uint(x, "component_tag", 8, true));
                    if (!i.immediate && x.hasAttribute("pts_time")) { k.time.specified = true; k.time.pts = r.uint(x, "pts_time", 33, true); }
                    i.components.append(k);
                } else r.fail(x, QString("unexpected <%1> in <splice_insert>").arg(x.name));
            }
            if (i.components.isEmpty() && !i.immediate) {
                if (!c.hasAttribute("pts_time")) r.fail(c, "without <component> or splice_immediate, attribute \"pts_time\" is required");
                else { i.time.specified = true; i.time.pts = r.uint(c, "pts_time", 33, true); }
            }
        } else if (c.is("splice_schedule") || c.is("private_command")) {
            r.failUnsupported(c, QString("splice command <%1>").arg(c.name));
        } else {
            r.failUnsupported(c, QString("splice descriptor <%1>").arg(c.name));
        }
    }
    if (r.ok() && commands != 1) r.fail(e, QString("a splice_information_table needs exactly one splice command (found %1)").arg(commands));
    if (unsupported) *unsupported = r.unsupported();
    if (!r.ok()) { if (error) *error = r.error(); return false; }
    if (out) *out = s;
    return true;
}

}   // namespace scte35
}   // namespace tskit
