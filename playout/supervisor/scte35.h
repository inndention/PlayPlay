#ifndef SCTE35_H
#define SCTE35_H

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * SCTE-35 cue messages in the ATSC multiplex (docs/mux.md "SCTE-35"). Pure helpers, no I/O: the MuxManager sends the XML to
 * the tsp master's `spliceinject` plugin over local UDP and keeps the per-service break state.
 *
 * On the wire, per service with SCTE-35 on (settings `mux.config.scte35` + `service.scte35`):
 *   -P pmt --service <sid> --add-pid <pid>/0x86 --add-registration 0x43554549      (a CUEI-registered SCTE-35 component in the PMT)
 *   -P spliceinject --service <sid> --pid <pid> --udp 127.0.0.1:<basePort + index>  (sections arrive as TSDuck XML datagrams)
 *
 * Config `mux.config.scte35`:
 *   { enabled:false (master: adds the PIDs to the PMTs at the next mux start), autoBreaks:true (cue out when a break item starts,
 *     with the break's planned length as break_duration), cueIn:true (an explicit return when the break ends early or on time),
 *     autoReturn:true (break_duration.auto_return), breakTypes:["ad","promo","psa"], minBreakSec:5, offsetMs:0 (delay before
 *     sending, positive = later, to line the cue up with encoder latency), basePort:5310, injectCount:2, heartbeatSec:0 (splice_null
 *     keep-alive; 0 = off) }
 * Service `scte35`: { enabled:true, pid } (pid default 0x0400 + service index - 1; EIT/ETT use 0x0200-0x02FF).
 */
namespace scte35 {

struct Splice {
    quint32 eventId = 1;
    bool outOfNetwork = true;        // true = cue out (to the break), false = cue in (back to the network)
    double durationSec = 0.0;        // break_duration; 0 = none
    bool autoReturn = true;
    int uniqueProgramId = 0;
    int availNum = 0, availsExpected = 0;
};

QJsonObject defaults();
QJsonObject normalize(const QJsonObject& cfg);

/// An immediate splice_insert() in TSDuck XML (what spliceinject reads from UDP).
QString spliceInsertXml(const Splice& s);
/// splice_insert with splice_event_cancel = 1 for `eventId`.
QString spliceCancelXml(quint32 eventId);
/// splice_null() (keep-alive for monitors that expect traffic on the PID).
QString spliceNullXml();

/// SCTE-35 PID of a service (1-based index in the service list): service.scte35.pid, else 0x0400 + index - 1.
int pidFor(const QJsonObject& service, int index);
/// Does this service carry SCTE-35 (master switch on and the service not switched off)?
bool serviceEnabled(const QJsonObject& cfg, const QJsonObject& service);
/// UDP port of a service's spliceinject listener.
int portFor(const QJsonObject& cfg, int index);
/// The tsp plugin arguments for every service with SCTE-35 (empty when the master switch is off).
QStringList tspPluginArgs(const QJsonObject& cfg, const QJsonArray& services);

/// Is this runlist item type a break?
bool isBreakType(const QJsonObject& cfg, const QString& type);
/// Planned length of the break that starts at `runlistId`: the item plus the break items right after it, from
/// ScheduleExecutor::runlistJson (`current` and `next` hours). 0 when the id is not found.
double breakSecondsFrom(const QJsonObject& runlists, const QString& runlistId, const QStringList& breakTypes);

} // namespace scte35

#endif // SCTE35_H
