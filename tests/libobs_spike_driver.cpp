// libobs_spike_driver — plays supervisor to one real PlayPlayChannelEngine.exe over the engine's own IPC and measures
// the engine process with the libobs layer off / on / removed (docs/libobs-spike.md, step 3).
//
//   libobs_spike_driver [--engine <PlayPlayChannelEngine.exe>] [--baseline 40] [--with 100] [--after 25]
//                       [--udp udp://127.0.0.1:50010] [--out <dir>]
//
// Phases: baseline (slate program + one UDP output), with-layer (libobs scene as layer 50: colour + noise, a cut and
// a fade switch half way), after (layer removed, libobs runtime idle). Every second: engine CPU% (one core = 100),
// working set, private bytes; from the engine's 1 Hz telemetry: tick max, late ticks and the layer's libobs status
// (frame age p50/p99, callbacks, repeats, drops). Writes <out>/libobs-spike-report.json and prints a markdown table.
// IPC name PlayPlayLibObsSpike and port 50010 only: never the live stack's names or ports.
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QDateTime>
#include <QDebug>
#include <algorithm>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
#include "ui_ipc/ipctransport.h"
#include "ui_ipc/ipcprotocol.h"

namespace {

struct Sample { double t = 0; QString phase; double cpuPct = 0, wsMb = 0, privMb = 0; };

struct ProcProbe {
#ifdef Q_OS_WIN
    HANDLE h = nullptr;
#endif
    double lastCpu = -1, lastWall = 0;
    bool open(qint64 pid) {
#ifdef Q_OS_WIN
        h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        return h != nullptr;
#else
        Q_UNUSED(pid); return false;
#endif
    }
    bool sample(Sample* s) {
#ifdef Q_OS_WIN
        if (!h) return false;
        FILETIME c, e, k, u;
        if (!GetProcessTimes(h, &c, &e, &k, &u)) return false;
        auto ft = [](const FILETIME& f) { return double((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7; };
        const double cpu = ft(k) + ft(u);
        const double wall = double(QDateTime::currentMSecsSinceEpoch()) / 1e3;
        if (lastCpu >= 0 && wall > lastWall) s->cpuPct = 100.0 * (cpu - lastCpu) / (wall - lastWall);
        lastCpu = cpu; lastWall = wall;
        PROCESS_MEMORY_COUNTERS_EX pmc{}; pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) { s->wsMb = double(pmc.WorkingSetSize) / 1048576.0; s->privMb = double(pmc.PrivateUsage) / 1048576.0; }
        return true;
#else
        Q_UNUSED(s); return false;
#endif
    }
};

QJsonObject layerSpec() {
    // Scene A: blue backdrop + the test-input noise source (every frame differs) in the middle; scene B: red backdrop.
    auto color = [](const QString& name, const QString& hex) { QJsonObject it; it["id"] = "color_source_v3"; it["name"] = name; QJsonObject s; s["color"] = hex; it["settings"] = s; return it; };
    QJsonObject noise; noise["id"] = "random"; noise["name"] = "noise"; noise["fit"] = "inner";
    QJsonObject sz; sz["width"] = 960; sz["height"] = 540; noise["size"] = sz; QJsonObject pos; pos["x"] = 480; pos["y"] = 270; noise["pos"] = pos;
    QJsonObject a; a["name"] = "A"; a["items"] = QJsonArray{color("bgA", "#2040c0"), noise};
    QJsonObject b; b["name"] = "B"; b["items"] = QJsonArray{color("bgB", "#c02020"), noise};
    QJsonObject src; src["type"] = "libobs"; src["scenes"] = QJsonArray{a, b}; src["program"] = "A";
    QJsonObject tr; tr["id"] = "fade_transition"; tr["durationMs"] = 500; src["transition"] = tr;
    return src;
}

double avg(const std::vector<double>& v) { if (v.empty()) return 0; double s = 0; for (double x : v) s += x; return s / double(v.size()); }
double mx(const std::vector<double>& v) { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCommandLineParser p;
    QCommandLineOption engineOpt("engine", "PlayPlayChannelEngine.exe", "path", QCoreApplication::applicationDirPath() + "/PlayPlayChannelEngine.exe");
    QCommandLineOption baseOpt("baseline", "seconds before the layer", "s", "40");
    QCommandLineOption withOpt("with", "seconds with the layer", "s", "100");
    QCommandLineOption afterOpt("after", "seconds after the layer is removed", "s", "25");
    QCommandLineOption udpOpt("udp", "UDP output url", "url", "udp://127.0.0.1:50010");
    QCommandLineOption outOpt("out", "report directory", "dir", QDir::currentPath());
    p.addHelpOption(); p.addOption(engineOpt); p.addOption(baseOpt); p.addOption(withOpt); p.addOption(afterOpt); p.addOption(udpOpt); p.addOption(outOpt);
    p.process(app);
    const int baseline = p.value(baseOpt).toInt(), with = p.value(withOpt).toInt(), after = p.value(afterOpt).toInt();
    const int total = baseline + with + after;
    const QString outDir = p.value(outOpt);
    QDir().mkpath(outDir);
    const QString ipcName = "PlayPlayLibObsSpike";
    const QString channelId = "perftest-obs";

    IpcTransport ipc(IpcTransport::ServerRole);
    if (!ipc.startServer(ipcName)) { qCritical() << "cannot start IPC server" << ipcName; return 2; }

    QJsonObject cfg; cfg["frameRate"] = "30000/1001"; cfg["width"] = 1920; cfg["height"] = 1080;
    QJsonObject out; out["type"] = "udp"; out["name"] = "spike"; out["url"] = p.value(udpOpt); out["profile"] = "h264_aac";
    cfg["outputs"] = QJsonArray{out};
    const QString cfgPath = outDir + "/libobs-spike-channel.json";
    { QFile f(cfgPath); f.open(QIODevice::WriteOnly); f.write(QJsonDocument(cfg).toJson()); }

    QProcess engine;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("QT_FORCE_STDERR_LOGGING", "1");
    engine.setProcessEnvironment(env);
    engine.setProcessChannelMode(QProcess::MergedChannels);
    engine.setStandardOutputFile(outDir + "/libobs-spike-engine.log", QIODevice::Truncate);
    engine.setWorkingDirectory(QFileInfo(p.value(engineOpt)).absolutePath());
    engine.start(p.value(engineOpt), {"--id", channelId, "--supervisor", ipcName, "--config", cfgPath, "--data", outDir + "/data", "--run-for", QString::number(total + 20)});
    if (!engine.waitForStarted(10000)) { qCritical() << "engine did not start:" << engine.errorString(); return 3; }
    qInfo() << "engine pid" << engine.processId() << "config" << cfgPath;
    ProcProbe probe; probe.open(engine.processId());

    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    QString phase = "baseline";
    std::vector<Sample> samples;
    std::vector<QJsonObject> telemetry;        // {t, phase, tickMsMax, lateTicks, obs:{...}}
    QString clientId;
    QObject::connect(&ipc, &IpcTransport::clientConnected, &app, [&](const QString& id) { clientId = id; qInfo() << "engine connected" << id; });
    QObject::connect(&ipc, &IpcTransport::messageReceived, &app, [&](const IpcProtocol::Message& m, const QString&) {
        if (m.type != IpcProtocol::MSG_UI_TELEMETRY) return;
        QJsonObject rec; rec["t"] = double(QDateTime::currentMSecsSinceEpoch() - t0) / 1e3; rec["phase"] = phase;
        const QJsonObject t = m.payload.value("telemetry").toObject();
        rec["tickMsLast"] = t.value("tickMsLast"); rec["tickMsMax"] = t.value("tickMsMax"); rec["lateTicks"] = t.value("lateTicks"); rec["state"] = m.payload.value("state");
        for (const auto& lv : m.payload.value("layers").toArray()) {
            const QJsonObject l = lv.toObject();
            if (l.value("id").toInt() == 50 || l.value("layer").toInt() == 50) { rec["layerState"] = l.value("state"); rec["sourceStatus"] = l.value("sourceStatus"); }
        }
        telemetry.push_back(rec);
    });

    QTimer sampler; sampler.setInterval(1000);
    QObject::connect(&sampler, &QTimer::timeout, &app, [&]() {
        Sample s; s.t = double(QDateTime::currentMSecsSinceEpoch() - t0) / 1e3; s.phase = phase;
        if (probe.sample(&s)) samples.push_back(s);
        if (int(s.t) % 10 == 0) qInfo().noquote() << QString("[%1 s] %2 cpu=%3% ws=%4 MB priv=%5 MB").arg(s.t, 0, 'f', 0).arg(phase).arg(s.cpuPct, 0, 'f', 1).arg(s.wsMb, 0, 'f', 0).arg(s.privMb, 0, 'f', 0);
    });
    sampler.start();

    auto cg = [&](const QJsonObject& payload) { ipc.broadcast(IpcProtocol::make(IpcProtocol::MSG_CG_COMMAND, channelId, payload)); };
    QTimer::singleShot(baseline * 1000, &app, [&]() {
        phase = "with-layer";
        QJsonObject c; c["action"] = "show"; c["layer"] = 50; c["role"] = "libobs"; c["source"] = layerSpec();
        QJsonObject in; in["type"] = "cut"; c["in"] = in;
        cg(c); qInfo() << "layer 50 (libobs) shown";
    });
    QTimer::singleShot((baseline + with / 2) * 1000, &app, [&]() {
        QJsonObject c; c["action"] = "update"; c["layer"] = 50; QJsonObject src; src["switch"] = "B"; src["transition"] = "cut_transition"; c["source"] = src;
        cg(c); qInfo() << "switch B (cut)";
    });
    QTimer::singleShot((baseline + with * 3 / 4) * 1000, &app, [&]() {
        QJsonObject c; c["action"] = "update"; c["layer"] = 50; QJsonObject src; src["switch"] = "A"; QJsonObject tr; tr["id"] = "fade_transition"; tr["durationMs"] = 500; src["transition"] = tr; c["source"] = src;
        cg(c); qInfo() << "switch A (fade 500)";
    });
    QTimer::singleShot((baseline + with) * 1000, &app, [&]() {
        phase = "after";
        QJsonObject c; c["action"] = "remove"; c["layer"] = 50;
        cg(c); qInfo() << "layer 50 removed";
    });
    QTimer::singleShot(total * 1000, &app, [&]() {
        sampler.stop();
        auto phaseStats = [&](const QString& ph) {
            std::vector<double> cpu, ws, priv, tickMax, late;
            for (const Sample& s : samples) if (s.phase == ph && s.t > 3) { cpu.push_back(s.cpuPct); ws.push_back(s.wsMb); priv.push_back(s.privMb); }
            QJsonObject lastObs; QString layerState;
            for (const QJsonObject& r : telemetry) if (r.value("phase").toString() == ph) { tickMax.push_back(r.value("tickMsMax").toDouble()); late.push_back(r.value("lateTicks").toDouble()); if (r.contains("sourceStatus")) { lastObs = r.value("sourceStatus").toObject(); layerState = r.value("layerState").toString(); } }
            QJsonObject o; o["samples"] = int(cpu.size()); o["cpuAvgPct"] = avg(cpu); o["cpuMaxPct"] = mx(cpu); o["wsAvgMb"] = avg(ws); o["wsMaxMb"] = mx(ws); o["privAvgMb"] = avg(priv); o["privMaxMb"] = mx(priv);
            o["tickMsMaxMax"] = mx(tickMax); o["lateTicksDelta"] = late.empty() ? 0.0 : late.back() - late.front();
            if (!lastObs.isEmpty()) { o["layerState"] = layerState; o["sourceStatus"] = lastObs; }
            return o;
        };
        QJsonObject report; report["engine"] = p.value(engineOpt); report["config"] = cfg; report["layer"] = layerSpec();
        report["baseline"] = phaseStats("baseline"); report["withLayer"] = phaseStats("with-layer"); report["after"] = phaseStats("after");
        QJsonArray sj; for (const Sample& s : samples) { QJsonObject o; o["t"] = s.t; o["phase"] = s.phase; o["cpuPct"] = s.cpuPct; o["wsMb"] = s.wsMb; o["privMb"] = s.privMb; sj << o; }
        report["samples"] = sj;
        QJsonArray tj; for (const QJsonObject& r : telemetry) tj << r; report["telemetry"] = tj;
        { QFile f(outDir + "/libobs-spike-report.json"); f.open(QIODevice::WriteOnly); f.write(QJsonDocument(report).toJson()); }
        auto row = [&](const QString& name, const QJsonObject& o) {
            const QJsonObject ss = o.value("sourceStatus").toObject(), obs = ss.value("obs").toObject();
            return QString("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 | %9 | %10 | %11 |").arg(name).arg(o.value("samples").toInt())
                .arg(o.value("cpuAvgPct").toDouble(), 0, 'f', 1).arg(o.value("cpuMaxPct").toDouble(), 0, 'f', 1)
                .arg(o.value("wsAvgMb").toDouble(), 0, 'f', 0).arg(o.value("wsMaxMb").toDouble(), 0, 'f', 0).arg(o.value("privMaxMb").toDouble(), 0, 'f', 0)
                .arg(o.value("tickMsMaxMax").toDouble(), 0, 'f', 2).arg(o.value("lateTicksDelta").toDouble(), 0, 'f', 0)
                .arg(obs.isEmpty() ? QString("-") : QString("%1 / %2 / %3").arg(obs.value("ageP50Ms").toDouble(), 0, 'f', 1).arg(obs.value("ageP99Ms").toDouble(), 0, 'f', 1).arg(obs.value("ageMaxMs").toDouble(), 0, 'f', 1))
                .arg(ss.isEmpty() ? QString("-") : QString("%1 / %2 / %3").arg(ss.value("videoIn").toDouble()).arg(ss.value("videoRepeats").toDouble()).arg(ss.value("videoDropped").toDouble()));
        };
        qInfo().noquote() << "\n| phase | s | cpu avg % | cpu max % | WS avg MB | WS max MB | priv max MB | tick max ms | late ticks | age p50/p99/max ms | frames in/repeats/drops |\n|---|---|---|---|---|---|---|---|---|---|---|";
        qInfo().noquote() << row("baseline", report["baseline"].toObject()) << "\n" << row("with layer", report["withLayer"].toObject()) << "\n" << row("after remove", report["after"].toObject());
        qInfo() << "report:" << outDir + "/libobs-spike-report.json";
        engine.kill(); engine.waitForFinished(5000);
        app.quit();
    });
    return app.exec();
}
