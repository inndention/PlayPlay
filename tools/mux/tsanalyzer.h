#ifndef PP_TSANALYZER_H
#define PP_TSANALYZER_H

#include "tssection.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

#include <functional>
#include <vector>

/**
 * PlayPlayTsKit, part 4 (docs/plans/inhouse-deps.md §6.5 stage 2, DEP-16): the transport stream analyzer behind
 * `PlayPlayMux analyze`, replacing `tsp -I ip ... -P tables / -P analyze` in MuxManager::analyze. QtCore only; input comes from a file or udpin.h, in any chunking.
 *
 * Two outputs:
 *  - tables text in the layout of `tsp -P tables` (TSDuck 3.41), line for line for the tables PlayPlay's mux carries: PAT,
 *    PMT, SDT, ATSC MGT / TVCT / RRT / EIT / ETT / STT and SCTE 35 splice_info_section, with their descriptors
 *    (registration, ISO-639, service, stream identifier, extended channel name, service location, caption service,
 *    content advisory). Other tables and descriptors get TSDuck's name and a hex dump. The demux follows TSDuck's rules
 *    (tests/inhouse/test_inhouse_tsanalyzer.cpp compares with tsp): sections on every PID unless filtered, PES PIDs
 *    skipped, sections with a bad CRC32 or a continuity break dropped, "next" sections ignored, a long table shown each
 *    time a new version completes (same version once), every short section (SCTE 35) shown;
 *  - measurements (toJson / reportText): services, per-PID packets and bitrate, continuity errors (duplicates and
 *    discontinuity_indicator honoured), CRC errors, PCR interval and jitter, PTS/DTS - PCR offsets, table repetition
 *    intervals and the decoded SCTE 35 cues. The TS bitrate comes from the PCRs (CBR mux), else from arrival times.
 * Everything is assumed ATSC (stream_type 0x81 is AC-3, 0xC7..0xCD are PSIP tables), which is what tsp concludes once it
 * has seen the MGT.
 */
namespace tskit {

/// The `tsp -P tables` text of one complete table: header, "Version ..." / "Short section ..." line and every section.
/// `sections` = the sections of one long table in section_number order, or one short section. Lines end in '\n'.
/// `dvbContext`: tsp decodes DVB descriptors (tags 0x40..0x7F) only after it has seen a DVB table (SDT, NIT, ...) in the
/// stream; before that they show as "unknown" with a dump. TsAnalyzer tracks this; a standalone call defaults to DVB on.
QString formatTable(int pid, const QList<QByteArray>& sections, bool dvbContext = true);
/// Tables of the DVB SI range (0x40..0x7F: NIT, SDT, BAT, EIT, TDT, TOT ...).
bool isDvbTable(int tableId);
/// TSDuck's table name for a table id ("PAT", "TVCT (ATSC)", "SCTE 35 Splice Information", "unknown (0xF0)").
QString tableName(int tableId);
/// TSDuck's stream_type name ("MPEG-2 Video", "ATSC AC-3 Audio", "unknown").
QString streamTypeName(int streamType);

struct AnalyzerOptions {
    QSet<int> displayPids;                 // tables shown for these PIDs only (tsp -P tables --pid); empty = all PIDs
    int maxTables = 0;                     // stop showing tables after this many (tsp --max-tables); 0 = no limit
    qint64 jitterMaxTicks = 1350;          // PCR jitter threshold, 27 MHz ticks (50 us, tsp pcrverify's default)
    int maxEvents = 200;                   // error / SCTE 35 events kept for the report and the JSON
};

class TsAnalyzer {
public:
    explicit TsAnalyzer(const AnalyzerOptions& options = AnalyzerOptions());

    /// Any chunking; resynchronises on 0x47. `arrivalNs` = receive time of this chunk (UDP), -1 for files.
    void feed(const char* data, qsizetype size, qint64 arrivalNs = -1);
    void feed(const QByteArray& b, qint64 arrivalNs = -1) { feed(b.constData(), b.size(), arrivalNs); }
    /// Computes bitrates, PCR and PTS statistics. Call once, after the last feed().
    void finish();

    /// Called with the text of each table as it completes (already in tablesText() layout).
    std::function<void(const QString& text)> onTable;

    QString tablesText() const { return m_tablesText; }   // exactly what `tsp -P tables` prints for the same packets
    QString reportText() const;                           // the `--mode analyze` report
    QJsonObject toJson() const;
    bool tableLimitReached() const { return m_opt.maxTables > 0 && m_displayed >= m_opt.maxTables; }

    // ------------------------------------------------------------------ results (valid after finish())
    struct Stats { double min = 0, avg = 0, max = 0; qint64 count = 0; void add(double v); };
    struct PidStats {
        int pid = 0;
        qint64 packets = 0, pusi = 0, ccErrors = 0, duplicates = 0, discontinuities = 0, scrambled = 0, tei = 0;
        qint64 sections = 0, crcErrors = 0, pesStarts = 0, pcrCount = 0;
        double bitrate = 0;                               // b/s
        Stats pcrIntervalMs, pcrJitterNs, ptsPcrMs, dtsPcrMs;
        qint64 pcrIntervalOver100ms = 0, pcrJitterOverMax = 0;
        double pcrArrivalJitterMs = -1;                   // peak-to-peak (PCR clock vs arrival), UDP only
        bool pes = false;
        QString description;
    };
    struct Stream { int pid = 0; int streamType = 0; QString language; };
    struct Service {
        int programNumber = 0, pmtPid = -1, pcrPid = -1;
        QString name, provider;                           // SDT service descriptor
        int major = -1, minor = -1, sourceId = -1;        // TVCT
        QString shortName, longName;
        bool hidden = false;
        QList<Stream> streams;
    };
    struct TableStats {
        int pid = 0, tableId = 0;
        qint64 sections = 0, crcErrors = 0;               // every section seen (repetitions included)
        int version = -1;                                 // last long-section version
        QSet<int> tidExts;
        Stats intervalPackets;                            // repetition interval in packets (valid sections)
        Stats intervalMs;                                 // the same in ms at the TS bitrate (after finish())
    };
    struct Event { qint64 packet = 0; int pid = 0; QString type; QString text; QJsonObject json; };

    qint64 packets() const { return m_packets; }
    qint64 syncLosses() const { return m_syncLosses; }
    double bitrate() const { return m_bitrate; }
    QString bitrateSource() const { return m_bitrateSource; }
    double durationSec() const;
    qint64 ccErrors() const;
    qint64 crcErrors() const;
    int displayedTables() const { return m_displayed; }
    const QMap<int, PidStats>& pids() const { return m_pids; }
    QList<Service> services() const;
    QList<TableStats> tables() const;
    const QList<Event>& errors() const { return m_errors; }
    const QList<Event>& scte35() const { return m_scte35; }
    int referencePcrPid() const { return m_refPcrPid; }
    /// Stream time seen so far from the reference PCR (works before finish(); 0 without PCR).
    double streamSeconds() const;

private:
    struct PcrSample { qint64 packet; qint64 pcr; qint64 arrivalNs; bool discontinuity; };
    struct PtsSample { qint64 packet; qint64 pts; qint64 dts; };
    struct PidState {
        int lastCc = -1;
        bool lastDuplicate = false;
        QByteArray sectionBuf;                            // bytes of a section in progress (starts at its table_id)
        std::vector<PcrSample> pcrs;
        std::vector<PtsSample> pts;
    };
    struct LongTable { int version = -1, last = -1; QList<QByteArray> sections; bool complete = false; };

    void processPacket(const char* p, qint64 arrivalNs);
    void demux(int pid, PidState& st, const quint8* payload, int len, bool pusi, qint64 index, bool reset);
    void consume(int pid, PidState& st, qint64 index);
    void onSection(int pid, const QByteArray& section, qint64 index);
    void onTableComplete(int pid, const QList<QByteArray>& sections);
    void display(int pid, const QList<QByteArray>& sections);
    void addEvent(QList<Event>& list, qint64 packet, int pid, const QString& type, const QString& text, const QJsonObject& json = {});
    QString describePid(int pid) const;
    double packetSeconds(qint64 packet) const;            // stream time of a packet index (bitrate based), -1 if unknown

    AnalyzerOptions m_opt;
    QByteArray m_buf;                                     // partial packet / resync buffer
    bool m_inSync = true;
    qint64 m_packets = 0, m_syncLosses = 0;
    qint64 m_firstArrivalNs = -1, m_lastArrivalNs = -1;
    int m_clockPid = -1;                                  // first PID with two PCRs: streamSeconds() while feeding
    qint64 m_clockTicks = 0;
    QHash<int, PidState> m_state;
    QMap<int, PidStats> m_pids;
    QHash<quint64, LongTable> m_long;                     // (pid, tid, tidExt)
    QMap<quint32, TableStats> m_tableStats;               // (pid, tid)
    QHash<quint32, qint64> m_tableLastPacket;
    QString m_tablesText;
    int m_displayed = 0;
    bool m_dvbContext = false;                            // a DVB table was shown (formatTable's dvbContext)
    QList<Event> m_errors, m_scte35;
    qint64 m_errorCount = 0, m_scteCount = 0;

    // signalization model (latest complete tables)
    QMap<int, int> m_patPrograms;                         // program -> PMT PID
    int m_tsid = -1;
    QMap<int, Service> m_pmt;                             // program -> PMT content
    QMap<int, QPair<QString, QString>> m_sdt;             // service id -> (name, provider)
    QMap<int, Service> m_vct;                             // program -> TVCT channel
    QMap<int, int> m_mgtPids;                             // PID -> MGT table type

    // finish()
    bool m_finished = false;
    double m_bitrate = 0;
    QString m_bitrateSource = "none";
    int m_refPcrPid = -1;
};

}   // namespace tskit

#endif // PP_TSANALYZER_H
