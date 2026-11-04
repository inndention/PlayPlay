#ifndef PP_SCTE35BIN_H
#define PP_SCTE35BIN_H

#include "psip.h"

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * PlayPlayTsKit, part 3 (docs/plans/inhouse-deps.md §6.5, DEP-14): SCTE 35 splice_info_section (table 0xFC) in binary.
 * The Supervisor's playout/supervisor/scte35.cpp writes the cue as TSDuck XML (spliceInsertXml / spliceCancelXml /
 * spliceNullXml) for tsp's spliceinject plugin; compileXml() turns that XML into the same bytes TSDuck would put on the
 * PID (byte-identical with `tstabcomp`, goldens in tests/inhouse/data/psip/scte35_*.bin), and parseSection() reads a section
 * back (the analyzer, DEP-16).
 *
 * Header as TSDuck writes it: section_syntax_indicator 0, private_indicator 0, sap_type '11', not encrypted, cw_index 0,
 * splice_command_length = the real length (not 0xFFF). Reserved bits are '1' (the four after splice_immediate_flag included).
 * Commands: splice_null, splice_insert (program or component mode, immediate or timed, optional break_duration, cancel),
 * time_signal, bandwidth_reservation. splice_schedule, private_command and splice descriptors are not supported.
 */
namespace tskit {
namespace scte35 {

enum Command : int { SpliceNull = 0x00, SpliceSchedule = 0x04, SpliceInsert = 0x05, TimeSignal = 0x06, BandwidthReservation = 0x07, PrivateCommand = 0xFF };

struct SpliceTime { bool specified = false; quint64 pts = 0; };   // 33-bit, 90 kHz
struct Component { int tag = 0; SpliceTime time; };

struct Insert {
    quint32 eventId = 0;
    bool cancel = false;
    bool outOfNetwork = false;
    bool immediate = false;
    SpliceTime time;                                      // program mode, not immediate
    QList<Component> components;                          // empty = program_splice_flag 1
    bool hasDuration = false;
    bool autoReturn = false;
    quint64 duration = 0;                                 // 33-bit, 90 kHz
    int uniqueProgramId = 0, availNum = 0, availsExpected = 0;
};

struct SpliceInfo {
    int protocolVersion = 0;
    quint64 ptsAdjustment = 0;                            // 33-bit
    int cwIndex = 0;
    int tier = 0xFFF;
    int command = SpliceNull;
    Insert insert;                                        // command == SpliceInsert
    SpliceTime timeSignal;                                // command == TimeSignal
    QByteArray descriptors;                               // raw splice descriptor loop (parse only; build writes it as is)
};

QByteArray buildSection(const SpliceInfo& s);
bool parseSection(const QByteArray& section, SpliceInfo* out, QString* error = nullptr);

/// Convenience for the Supervisor's cues: an immediate program splice_insert (out or in), optional break_duration.
QByteArray immediateInsert(quint32 eventId, bool outOfNetwork, double durationSec, bool autoReturn, int uniqueProgramId, int availNum = 0, int availsExpected = 0);
QByteArray cancelSection(quint32 eventId);
QByteArray nullSection();

/// <splice_information_table> (TSDuck XML) -> SpliceInfo. `unsupported` set for elements this library lacks.
bool fromXml(const xml::Element& e, SpliceInfo* out, QString* error, bool* unsupported);

}   // namespace scte35
}   // namespace tskit

#endif // PP_SCTE35BIN_H
