#ifndef PLAYPLAY_PPWATCH_HEALTHPROBE_H
#define PLAYPLAY_PPWATCH_HEALTHPROBE_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
#include <vector>

/**
 * PlayPlayWatchdog (docs/plans/perf-supervisor-app.md §5.3, W19): the /healthz probe and the per-target decision logic.
 *
 * Why: the in-process LoopWatchdog (STB-4) explains a stall from inside the process, but nothing outside noticed the
 * 10-24 / 10-25 freezes, and a process that is wholly stuck (suspended, loader-lock deadlock, its dump worker blocked)
 * cannot dump itself. ppwatch polls each process's /healthz from outside and, on HANG, writes an incident bundle and a
 * dump of that one process.
 *
 *   - probeAll(): plain Winsock, non-blocking, every target at once (one select loop), so one frozen process costs one
 *     timeout per cycle, not one per target. Loopback only: parseUrl() refuses any other host.
 *   - Tracker: pure state machine fed one Observation per poll (tests drive it with synthetic clocks):
 *       answered    -> the body's "state" (ok | stall | hang | exiting; no "state" field = ok, a non-2xx reply = error)
 *       no answer   -> "unresponsive" after unresponsivePolls polls in a row while the port still has a listener
 *       no listener -> "down"; the configured pid has exited -> "gone"
 *     hang and unresponsive are a hang episode: incident = true on the first poll of an episode (once), restart = true
 *     once per episode when restartAfterMs > 0 (opt-in, supervisor role only) and the episode has lasted that long.
 *     An episode ends on any other state or when the process behind the port changes (new pid).
 */
namespace ppwatch {

struct Url {
    QString host;               // 127.0.0.1 | localhost | ::1 (normalised: localhost -> 127.0.0.1)
    quint16 port = 0;
    QByteArray path = "/healthz";
    bool v6 = false;
    QString text() const;
};

/// http://<loopback>:<port>[/path]; anything else (https, other hosts, no port) is refused with a reason.
bool parseUrl(const QString& s, Url* out, QString* error = nullptr);

struct ProbeResult {
    enum Kind { Answered, Refused, ConnectTimeout, ReplyTimeout, BadReply, Error };
    Kind kind = Error;
    int status = 0;             // HTTP status when answered
    QByteArray body;
    QJsonObject json;           // body parsed as a JSON object (empty if it is not one)
    qint64 ms = 0;              // time to the reply (or to the failure)
    QString error;

    bool answered() const { return kind == Answered; }
    /// ok | stall | hang | exiting | ... from the body; "ok" for a 2xx reply without one; "error" for other statuses.
    QString state() const;
    qint64 pid() const { return qint64(json.value(QStringLiteral("pid")).toDouble()); }
    static const char* kindName(Kind k);
};

/// GETs every URL at once and waits at most timeoutMs in all. Results are in the order of `urls`.
std::vector<ProbeResult> probeAll(const std::vector<Url>& urls, int timeoutMs);
ProbeResult probe(const Url& url, int timeoutMs);

struct Observation {
    enum Reach { Answered, NoAnswer, NoListener, PidGone };
    qint64 tMs = 0;             // monotonic
    Reach reach = NoAnswer;
    QString state;              // when answered
    qint64 pid = 0;             // the process behind the port (0 = unknown); a change ends the episode
};

struct Decision {
    QString from, to;
    bool changed = false;       // the reported state changed (from -> to)
    bool incident = false;      // first poll of a hang episode
    bool restart = false;       // the opt-in restart is due now (once per episode)
    qint64 episodeMs = 0;       // length of the current hang episode
};

struct TrackerOptions {
    int unresponsivePolls = 2;  // polls without an answer (listener still there) before "unresponsive"
    qint64 restartAfterMs = 0;  // 0 = never (default; --restart-hung-supervisor-after)
    bool supervisorRole = false;// restart applies to the supervisor role only
};

class Tracker {
public:
    explicit Tracker(const TrackerOptions& o = TrackerOptions()) : m_o(o) {}
    Decision feed(const Observation& ob);
    QString state() const { return m_state; }
    bool inEpisode() const { return m_inEpisode; }
    qint64 episodeStartMs() const { return m_episodeStart; }
    qint64 episodePid() const { return m_episodePid; }
    void setOptions(const TrackerOptions& o) { m_o = o; }
    const TrackerOptions& options() const { return m_o; }
    static bool isHangState(const QString& s);

private:
    TrackerOptions m_o;
    QString m_state = QStringLiteral("unknown");
    int m_noAnswer = 0;
    bool m_inEpisode = false, m_restartDone = false;
    qint64 m_episodeStart = 0, m_episodePid = 0;
};

} // namespace ppwatch

#endif // PLAYPLAY_PPWATCH_HEALTHPROBE_H
