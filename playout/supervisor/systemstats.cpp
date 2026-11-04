#include "systemstats.h"
#include "restapiserver.h"
#include "../core/audiodevices.h"

#include <cmath>
#include <QCoreApplication>
#include <QSet>
#include <QDateTime>
#include <QJsonArray>
#include <QPointer>
#include <QThread>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <psapi.h>
#endif

namespace {
SystemStats* g_instance = nullptr;
constexpr qint64 kHistoryMs = 8000;        // frame-rate window
constexpr qint64 kStaleMs = 3500;          // no telemetry for this long = the channel is not reporting

double nominalFps(const QString& s) {
    const int slash = s.indexOf('/');
    if (slash > 0) { const double d = s.mid(slash + 1).toDouble(); return d > 0 ? s.left(slash).toDouble() / d : 0.0; }
    return s.toDouble();
}
double round1(double v) { return std::round(v * 10.0) / 10.0; }
double round2(double v) { return std::round(v * 100.0) / 100.0; }
#ifdef Q_OS_WIN
quint64 ft64(const FILETIME& f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
#endif
} // namespace

SystemStats* SystemStats::instance() { return g_instance; }

SystemStats::SystemStats(QObject* parent) : QObject(parent) {
    m_clock.start();
    m_cores = qMax(1, QThread::idealThreadCount());
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &SystemStats::sample);
}

SystemStats* SystemStats::install(RestApiServer* rest, QObject* parent, ProcProvider engines) {
    if (g_instance) return g_instance;
    g_instance = new SystemStats(parent);
    g_instance->m_engines = std::move(engines);
    QObject::connect(g_instance, &QObject::destroyed, [](QObject*) { g_instance = nullptr; });
    g_instance->sample();                 // primes the counters; the first deltas arrive one second later
    g_instance->m_timer.start();
    rest->registerRoute("GET", "/api/v1/system/stats", [](const QJsonObject&, const QHash<QString, QString>&) {
        return g_instance ? g_instance->snapshot() : QJsonObject();
    });
    // Sound-card inputs and playback devices for the mixer's line-in strips (WASAPI endpoints; the engine's audio_in source opens them).
    rest->registerRoute("GET", "/api/v1/audio/devices", [](const QJsonObject&, const QHash<QString, QString>&) {
        QJsonObject o; o["devices"] = audiodev::listJson(); return o;
    });
    return g_instance;
}

void SystemStats::noteTelemetry(const QString& channelId, const QJsonObject& telemetry) {
    if (g_instance) g_instance->note(channelId, telemetry);
}

void SystemStats::note(const QString& channelId, const QJsonObject& t) {
    Chan& c = m_chan[channelId];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double frame = t.value("frame").toDouble();
    if (!c.hist.empty() && frame < c.hist.back().frame) c.hist.clear();      // engine restarted
    c.hist.push_back({now, frame});
    while (c.hist.size() > 2 && now - c.hist.front().ms > kHistoryMs) c.hist.pop_front();
    c.last = t; c.lastMs = now;
}

void SystemStats::sample() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject out;
    out["atMs"] = double(now);
    out["cores"] = m_cores;

#ifdef Q_OS_WIN
    // --- machine CPU ---------------------------------------------------------------------------
    {
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const quint64 i = ft64(idle), tot = ft64(kernel) + ft64(user);
            if (m_total > 0 && tot > m_total) m_cpuPct = qBound(0.0, 100.0 * double((tot - m_total) - (i - m_idle)) / double(tot - m_total), 100.0);
            m_idle = i; m_total = tot;
        }
    }
    // --- machine memory ------------------------------------------------------------------------
    QJsonObject mem;
    {
        MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            const double total = double(ms.ullTotalPhys), avail = double(ms.ullAvailPhys);
            mem["totalMB"] = round1(total / 1048576.0); mem["usedMB"] = round1((total - avail) / 1048576.0);
            mem["usedPct"] = round1(100.0 * (total - avail) / qMax(1.0, total));
            mem["commitUsedMB"] = round1(double(ms.ullTotalPageFile - ms.ullAvailPageFile) / 1048576.0);
        }
    }
    // --- network: physical adapters that are up (virtual switches would count the same bytes twice) ---
    QJsonObject net; QJsonArray ifaces;
    {
        const double dt = m_netMs > 0 ? qMax(1.0, double(now - m_netMs)) / 1000.0 : 0.0;
        double inB = 0, outB = 0; quint64 linkBps = 0;
        PMIB_IF_TABLE2 tbl = nullptr;
        if (GetIfTable2(&tbl) == NO_ERROR && tbl) {
            bool anyHw = false;
            for (ULONG k = 0; k < tbl->NumEntries; ++k) if (tbl->Table[k].InterfaceAndOperStatusFlags.HardwareInterface && tbl->Table[k].OperStatus == IfOperStatusUp) anyHw = true;
            QHash<quint64, NetPrev> seen;
            for (ULONG k = 0; k < tbl->NumEntries; ++k) {
                const MIB_IF_ROW2& r = tbl->Table[k];
                if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp) continue;
                if (anyHw && !r.InterfaceAndOperStatusFlags.HardwareInterface) continue;
                if (r.InterfaceAndOperStatusFlags.FilterInterface) continue;
                const quint64 id = r.InterfaceLuid.Value;
                NetPrev cur{r.InOctets, r.OutOctets};
                double di = 0, dout = 0;
                const auto prev = m_net.constFind(id);
                if (prev != m_net.constEnd() && dt > 0) {
                    di = cur.in >= prev->in ? double(cur.in - prev->in) / dt : 0.0;
                    dout = cur.out >= prev->out ? double(cur.out - prev->out) / dt : 0.0;
                }
                seen.insert(id, cur);
                inB += di; outB += dout;
                linkBps = qMax<quint64>(linkBps, qMax(r.ReceiveLinkSpeed, r.TransmitLinkSpeed));
                QJsonObject o; o["name"] = QString::fromWCharArray(r.Alias); o["inBps"] = round1(di); o["outBps"] = round1(dout);
                o["linkMbps"] = double(qMax(r.ReceiveLinkSpeed, r.TransmitLinkSpeed)) / 1e6;
                ifaces.append(o);
            }
            m_net = seen;
            FreeMibTable(tbl);
        }
        m_netMs = now;
        net["inBps"] = round1(inB); net["outBps"] = round1(outB); net["linkMbps"] = double(linkBps) / 1e6; net["interfaces"] = ifaces;
    }
    // --- processes ----------------------------------------------------------------------------------
    auto probe = [&](qint64 pid, bool self) {
        QJsonObject o; o["pid"] = double(pid);
        HANDLE h = self ? GetCurrentProcess() : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(pid));
        if (!h) { o["cpu"] = 0.0; o["memMB"] = 0.0; o["alive"] = false; return o; }
        FILETIME cr, ex, kr, us;
        double pct = 0;
        if (GetProcessTimes(h, &cr, &ex, &kr, &us)) {
            const quint64 cpu = ft64(kr) + ft64(us);
            ProcPrev& p = m_proc[pid];
            if (p.ms > 0 && now > p.ms && cpu >= p.cpu100ns) pct = qBound(0.0, double(cpu - p.cpu100ns) / (double(now - p.ms) * 10000.0 * m_cores) * 100.0, 100.0);
            else pct = p.pct;
            p.cpu100ns = cpu; p.ms = now; p.pct = pct;
        }
        PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof(pm);
        double ws = 0, priv = 0;
        if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) { ws = double(pm.WorkingSetSize) / 1048576.0; priv = double(pm.PrivateUsage) / 1048576.0; }
        if (!self) CloseHandle(h);
        o["cpu"] = round1(pct); o["memMB"] = round1(ws); o["privateMB"] = round1(priv); o["alive"] = true;
        return o;
    };
    QJsonObject sup = probe(qint64(QCoreApplication::applicationPid()), true);
    mem["supervisorMB"] = sup.value("memMB");
    QHash<QString, QJsonObject> engineProc;
    double engCpu = 0, engMem = 0; QSet<qint64> livePids; livePids.insert(qint64(QCoreApplication::applicationPid()));
    if (m_engines) for (const ProcRef& r : m_engines()) {
        if (r.pid <= 0) continue;
        livePids.insert(r.pid);
        const QJsonObject o = probe(r.pid, false);
        engineProc.insert(r.channelId, o);
        engCpu += o.value("cpu").toDouble(); engMem += o.value("memMB").toDouble();
    }
    for (auto it = m_proc.begin(); it != m_proc.end();) it = livePids.contains(it.key()) ? std::next(it) : m_proc.erase(it);
    mem["enginesMB"] = round1(engMem);
    QJsonObject cpu; cpu["systemPct"] = round1(m_cpuPct); cpu["supervisorPct"] = sup.value("cpu"); cpu["enginesPct"] = round1(engCpu); cpu["cores"] = m_cores;
    out["cpu"] = cpu; out["memory"] = mem; out["net"] = net;
    sup["uptimeSec"] = double(m_clock.elapsed() / 1000);
    out["supervisor"] = sup;
#else
    const QHash<QString, QJsonObject> engineProc;
    Q_UNUSED(engineProc)
#endif

    // --- channels: measured frame rate, outputs, bitrate, drops ----------------------------------------------------
    QJsonArray chans; double totalKbps = 0; double totalDrops = 0;
    for (auto it = m_chan.constBegin(); it != m_chan.constEnd(); ++it) {
        const Chan& c = it.value();
        const QJsonObject& t = c.last;
        QJsonObject o; o["id"] = it.key();
        const bool stale = c.lastMs == 0 || now - c.lastMs > kStaleMs;
        o["stale"] = stale; o["state"] = stale ? QStringLiteral("OFFLINE") : t.value("state").toString();
        const double nom = nominalFps(t.value("frameRate").toString());
        o["nominalFps"] = round2(nom); o["width"] = t.value("width"); o["height"] = t.value("height"); o["frame"] = t.value("frame");
        double fps = 0;
        if (!stale && c.hist.size() >= 2) {
            // oldest sample at least ~3 s back (or the oldest we have): the longer the span, the smaller the arrival-time jitter
            const Hist& last = c.hist.back(); const Hist* from = &c.hist.front();
            for (const Hist& h : c.hist) if (last.ms - h.ms >= 3000) from = &h; else break;
            const qint64 span = last.ms - from->ms;
            if (span >= 900 && last.frame >= from->frame) fps = (last.frame - from->frame) * 1000.0 / double(span);
        }
        o["fps"] = round2(fps);
        const QJsonObject tel = t.value("telemetry").toObject();
        const double drops = tel.value("outputDrops").toDouble() + t.value("compositor").toObject().value("program").toObject().value("dropped").toDouble();
        o["drops"] = drops; o["lateTicks"] = tel.value("lateTicks"); o["tickMs"] = tel.value("tickMsLast"); o["tickMsMax"] = tel.value("tickMsMax");
        o["audioUnderruns"] = tel.value("audioUnderruns");
        QJsonArray outs; double kbps = 0; int up = 0, n = 0;
        for (const QJsonValue& ov : t.value("outputs").toArray()) {
            const QJsonObject r = ov.toObject();
            if (r.value("enabled").isBool() && !r.value("enabled").toBool()) continue;
            if (r.value("kind").toString() == "encoder") continue;       // the encoder stage reports its configured target; the sink behind it reports the measured bitrate
            QJsonObject x; x["name"] = r.value("name"); x["kind"] = r.value("kind").toString(r.value("type").toString());
            x["state"] = r.value("state"); x["connected"] = r.value("connected"); x["error"] = r.value("error");
            const double br = r.contains("bitrateKbps") ? r.value("bitrateKbps").toDouble() : r.value("videoKbps").toDouble();
            x["bitrateKbps"] = round1(br); x["dropped"] = r.value("dropped");
            kbps += qMax(0.0, br); ++n; if (r.value("connected").toBool() || r.value("state").toString() == "running") ++up;
            outs.append(x);
        }
        o["outputs"] = outs; o["outputsUp"] = up; o["outputCount"] = n; o["bitrateKbps"] = round1(kbps);
        totalKbps += kbps; totalDrops += drops;
        const auto ep = engineProc.constFind(it.key());
        if (ep != engineProc.constEnd()) { o["pid"] = ep->value("pid"); o["cpu"] = ep->value("cpu"); o["memMB"] = ep->value("memMB"); }
        chans.append(o);
    }
    out["channels"] = chans;
    QJsonObject totals; totals["bitrateKbps"] = round1(totalKbps); totals["drops"] = totalDrops; out["totals"] = totals;
    m_last = out;
}
