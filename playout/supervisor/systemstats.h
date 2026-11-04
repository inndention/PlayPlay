#ifndef SYSTEMSTATS_H
#define SYSTEMSTATS_H

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QHash>
#include <QList>
#include <deque>
#include <functional>

class RestApiServer;

/**
 * SystemStats — what the bottom status bar (native app) and the web footer show: per-channel frame rate and output bitrate,
 * CPU and memory (whole machine, the Supervisor and every engine process) and network in / out. Sampled once a second;
 * GET /api/v1/system/stats returns the last sample, so any number of clients cost nothing.
 *
 *   fps         measured from the engine's frame counter over the last few seconds of telemetry (the nominal rate is `nominalFps`)
 *   cpu         GetSystemTimes (machine) and GetProcessTimes (processes), percent of all cores
 *   memory      GlobalMemoryStatusEx (machine), working set of each process
 *   net         GetIfTable2 octet counters of the physical adapters that are up, differenced per second
 *
 * Engines report each output's bitrate (byte delta of the sink) and drops in their telemetry; they are only collected here.
 */
class SystemStats : public QObject {
    Q_OBJECT
public:
    struct ProcRef { QString channelId; qint64 pid = 0; };
    using ProcProvider = std::function<QList<ProcRef>()>;

    /// Creates the sampler (parented to `parent`), starts it and registers GET /api/v1/system/stats.
    static SystemStats* install(RestApiServer* rest, QObject* parent, ProcProvider engines);
    /// Called for every engine telemetry message (feeds the frame-rate measurement and the output table).
    static void noteTelemetry(const QString& channelId, const QJsonObject& telemetry);
    static SystemStats* instance();

    QJsonObject snapshot() const { return m_last; }

private:
    explicit SystemStats(QObject* parent);
    void sample();
    void note(const QString& channelId, const QJsonObject& t);

    struct Hist { qint64 ms = 0; double frame = 0; };
    struct Chan { std::deque<Hist> hist; QJsonObject last; qint64 lastMs = 0; };
    struct ProcPrev { quint64 cpu100ns = 0; qint64 ms = 0; double pct = 0; };
    struct NetPrev { quint64 in = 0, out = 0; };

    QTimer m_timer;
    QElapsedTimer m_clock;
    ProcProvider m_engines;
    QHash<QString, Chan> m_chan;
    QHash<qint64, ProcPrev> m_proc;
    QHash<quint64, NetPrev> m_net;
    QJsonObject m_last;
    quint64 m_idle = 0, m_total = 0;
    qint64 m_sysMs = 0, m_netMs = 0;
    double m_cpuPct = 0;
    int m_cores = 1;
};

#endif // SYSTEMSTATS_H
