// PlayPlayMux: the in-house replacement for the TSDuck command-line tools the ATSC mux uses (docs/plans/inhouse-deps.md
// §6.5). Output contract: tools/ppcommon/ppcli.h (one JSON object on the last stdout line; exit 0 ok, 2 failed, 64 an
// argument or input this tool does not implement, so the caller can fall back to the TSDuck tool).
//
//   PlayPlayMux compile [--atsc] [--output F | --outdir D] [--check] <file.xml>...   = tstabcomp --atsc (PSIP, PAT/PMT, SCTE-35)
//   PlayPlayMux pmt-scte35 --input pmt.bin --pid 0x0400 [--output out.bin]           = tsp -P pmt --add-pid <pid>/0x86 --add-registration CUEI
//   PlayPlayMux packetize --pid P [--stuffing] [--continuity N] --output F <file.bin|file.xml>...   = tspacketize
//   PlayPlayMux analyze [--mode tables|analyze|both] [--seconds S] [--pid P]... [--max-tables N] <file | [src@]addr:port>
//                                                                                    = tsp -I ip|file ... -P tables / -P analyze
//   PlayPlayMux dektec list [-v] [--probe] | prep --port P [--device D] [--check] [--force]   = tsdektec -a -v / --output P D
//   PlayPlayMux version
#include "dtdevice.h"
#include "psip.h"
#include "scte35bin.h"
#include "tsanalyzer.h"
#include "tssection.h"
#include "udpin.h"

#include "ppcli.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>

#include <cstdio>
#include <memory>

namespace {

bool parseInt(const QString& s, int* out) {
    bool ok = false;
    const QString t = s.trimmed();
    *out = t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? t.mid(2).toInt(&ok, 16) : t.toInt(&ok, 10);
    return ok;
}

bool writeAtomic(const QString& path, const QByteArray& data, QString* error) {
    if (!QFileInfo(path).absolutePath().isEmpty()) QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit()) return true;
    if (error) *error = QString("cannot write %1").arg(path);
    return false;
}

// Sections of an input: a .bin as written by tstabcomp / `compile`, or an XML file compiled on the fly.
bool loadSections(const QString& path, QList<QByteArray>* out, QString* error, bool* unsupported) {
    if (path.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive)) {
        const tskit::CompileResult r = tskit::compileFile(path, QString());
        if (!r.ok) { *error = r.error; *unsupported = r.unsupported; return false; }
        for (const tskit::CompiledTable& t : r.tables) out->append(t.sections);
        return true;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { *error = QString("cannot read %1").arg(path); return false; }
    QString e;
    const QList<QByteArray> s = tskit::splitSections(f.readAll(), &e);
    if (!e.isEmpty()) { *error = QFileInfo(path).fileName() + ": " + e; return false; }
    out->append(s);
    return true;
}

int cmdCompile(const QStringList& args) {
    const ppcli::Args a(args, {"--atsc", "--output=", "-o=", "--outdir=", "--check"}, true);
    if (!a.ok()) return a.report();
    const QStringList in = a.positional();
    if (in.isEmpty()) return ppcli::fail("compile: no input file");
    const QString output = a.value("--output", a.value("-o"));
    if (!output.isEmpty() && in.size() > 1) return ppcli::fail("compile: --output takes a single input file (use --outdir)");
    QJsonArray files;
    QString firstError;
    bool allOk = true, onlyUnsupported = true;
    for (const QString& f : in) {
        QString bin;
        if (!a.has("--check")) {
            const QFileInfo fi(f);
            bin = !output.isEmpty() ? output : QDir(a.value("--outdir", fi.absolutePath())).filePath(fi.completeBaseName() + ".bin");
            QDir().mkpath(QFileInfo(bin).absolutePath());
        }
        const tskit::CompileResult r = tskit::compileFile(f, bin);
        QJsonObject o;
        o["file"] = f;
        o["ok"] = r.ok;
        if (r.ok) {
            if (!bin.isEmpty()) o["output"] = bin;
            o["tables"] = int(r.tables.size()); o["sections"] = r.sectionCount(); o["bytes"] = int(r.bin().size());
        } else {
            o["error"] = r.error;
            if (r.unsupported) o["unsupported"] = true;
            ppcli::log("compile: " + r.error);
            if (firstError.isEmpty()) firstError = r.error;
            allOk = false;
            onlyUnsupported = onlyUnsupported && r.unsupported;
        }
        files.append(o);
    }
    QJsonObject res;
    res["ok"] = allOk;
    res["files"] = files;
    if (!allOk) { res["error"] = firstError; if (onlyUnsupported) res["unsupported"] = true; }
    return ppcli::finish(res);
}

int cmdPmtScte35(const QStringList& args) {
    const ppcli::Args a(args, {"--input=", "--pid=", "--output="});
    if (!a.ok()) return a.report();
    int pid = 0;
    if (!a.has("--input") || !parseInt(a.value("--pid"), &pid) || pid < 0x20 || pid >= 0x1FFF) return ppcli::fail("pmt-scte35: --input <pmt.bin> and --pid 0x0020..0x1FFE are required");
    QList<QByteArray> secs; QString err; bool unsupported = false;
    if (!loadSections(a.value("--input"), &secs, &err, &unsupported)) return ppcli::fail(err);
    if (secs.isEmpty()) return ppcli::fail("pmt-scte35: no section in the input");
    const QByteArray out = tskit::pmtWithScte35(secs.first(), pid, &err);
    if (out.isEmpty()) return ppcli::fail(err);
    if (a.has("--output") && !writeAtomic(a.value("--output"), out, &err)) return ppcli::fail(err);
    QJsonObject res;
    res["ok"] = true; res["pid"] = pid; res["bytes"] = int(out.size()); res["section"] = QString::fromLatin1(out.toHex());
    if (a.has("--output")) res["output"] = a.value("--output");
    return ppcli::finish(res);
}

int cmdPacketize(const QStringList& args) {
    const ppcli::Args a(args, {"--pid=", "-p=", "--stuffing", "-s", "--continuity=", "--output=", "-o="}, true);
    if (!a.ok()) return a.report();
    int pid = -1, cc = 0;
    const QString output = a.value("--output", a.value("-o"));
    if (!parseInt(a.value("--pid", a.value("-p")), &pid) || pid < 0 || pid > 0x1FFF || output.isEmpty() || a.positional().isEmpty())
        return ppcli::fail("packetize: --pid, --output and at least one input file are required");
    if (a.has("--continuity") && !parseInt(a.value("--continuity"), &cc)) return ppcli::fail("packetize: bad --continuity");
    QList<QByteArray> secs; QString err; bool unsupported = false;
    for (const QString& f : a.positional()) {
        if (!loadSections(f, &secs, &err, &unsupported)) { QJsonObject x; if (unsupported) x["unsupported"] = true; return ppcli::finish(ppcli::failure(err, x)); }
    }
    tskit::Packetizer p(pid, cc);
    const QByteArray ts = p.packetize(secs, a.has("--stuffing") || a.has("-s") ? tskit::Packetizer::StuffEachSection : tskit::Packetizer::Packed);
    if (!writeAtomic(output, ts, &err)) return ppcli::fail(err);
    QJsonObject res;
    res["ok"] = true; res["output"] = output; res["sections"] = int(secs.size()); res["packets"] = int(ts.size() / tskit::kPacketSize);
    res["nextContinuity"] = p.continuity();
    return ppcli::finish(res);
}

// analyze: TS from a file or a UDP (multicast) address through tskit::TsAnalyzer. Text on stdout first (tables in the
// `tsp -P tables` layout as they complete, then the --mode analyze report), the JSON summary on the last line.
//   --mode tables|analyze|both   tables (default) = tsp -P tables; analyze = PID / PCR / PTS / error report; both
//   --seconds S                  UDP: receive for S s of wall time (default 3); file: stop after S s of stream (PCR) time
//   --receive-timeout MS         UDP: give up after MS without a datagram (default 5000; tsp --receive-timeout)
//   --pid P | P1-P2              show tables of these PIDs only (tsp tables --pid); repeatable
//   --max-tables N, -x N         stop showing tables after N (tsp --max-tables); in tables mode the input stops too
//   --max-packets N              stop after N packets
//   --local-address A            UDP: interface for the multicast join (default interface otherwise)
//   --buffer-size B              UDP: socket receive buffer (default 2097152, as MuxManager passes to tsp)
//   --jitter-max T               PCR jitter threshold in 27 MHz ticks (default 1350 = 50 us)
//   --input file|udp|ip          force the input kind (default: an existing file, else a UDP address)
//   --output F, -o F             write the text to F instead of stdout
//   --quiet                      no text, JSON only;  --atsc  accepted for tsp compatibility (always ATSC here)
int cmdAnalyze(const QStringList& args) {
    const ppcli::Args a(args, {"--mode=", "--input=", "--seconds=", "--max-packets=", "--max-tables=", "-x=", "--pid=", "-p=", "--receive-timeout=",
                               "--local-address=", "--buffer-size=", "--jitter-max=", "--output=", "-o=", "--quiet", "--atsc"}, true);
    if (!a.ok()) return a.report();
    if (a.positional().size() != 1) return ppcli::fail("analyze: one input is required (a TS file or [source@]address:port)");
    const QString src = a.positional().first();
    const QString mode = a.value("--mode", "tables");
    if (mode != "tables" && mode != "analyze" && mode != "both") return ppcli::unsupported("--mode " + mode);

    tskit::AnalyzerOptions opt;
    for (const QString& p : a.values("--pid") + a.values("-p")) {
        int lo = 0, hi = 0;
        const QStringList r = p.split('-');
        if (r.size() > 2 || !parseInt(r.first(), &lo) || !parseInt(r.last(), &hi) || lo < 0 || hi > 0x1FFF || lo > hi) return ppcli::fail("analyze: bad --pid " + p);
        for (int v = lo; v <= hi; ++v) opt.displayPids.insert(v);
    }
    bool ok = true;
    opt.maxTables = a.integer("--max-tables", a.integer("-x", 0), &ok);
    if (!ok || opt.maxTables < 0) return ppcli::fail("analyze: bad --max-tables");
    opt.jitterMaxTicks = a.integer("--jitter-max", 1350, &ok);
    if (!ok || opt.jitterMaxTicks <= 0) return ppcli::fail("analyze: bad --jitter-max");
    const qint64 maxPackets = a.value("--max-packets", "0").toLongLong(&ok);
    if (!ok || maxPackets < 0) return ppcli::fail("analyze: bad --max-packets");

    QString kind = a.value("--input");
    tskit::UdpSpec udp;
    QString err;
    if (kind.isEmpty()) kind = QFileInfo::exists(src) ? "file" : (tskit::UdpSpec::parse(src, &udp, &err) ? "udp" : "file");
    if (kind == "ip") kind = "udp";
    if (kind != "file" && kind != "udp") return ppcli::unsupported("--input " + kind);
    if (kind == "udp" && !tskit::UdpSpec::parse(src, &udp, &err)) return ppcli::fail("analyze: " + err);
    const double seconds = a.number("--seconds", kind == "udp" ? 3.0 : 0.0, &ok);
    if (!ok || seconds < 0) return ppcli::fail("analyze: bad --seconds");

    std::unique_ptr<QFile> textFile;
    if (a.has("--output") || a.has("-o")) {
        textFile = std::make_unique<QFile>(a.value("--output", a.value("-o")));
        if (!textFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) return ppcli::fail("analyze: cannot write " + textFile->fileName());
    }
    const bool quiet = a.has("--quiet");
    auto write = [&](const QString& t) {
        if (quiet || t.isEmpty()) return;
        const QByteArray b = t.toUtf8();
        if (textFile) { textFile->write(b); return; }
        std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
        std::fflush(stdout);
    };

    tskit::TsAnalyzer an(opt);
    if (mode != "analyze") an.onTable = write;
    const bool stopOnTables = mode == "tables" && opt.maxTables > 0;
    QElapsedTimer wall;
    wall.start();
    QJsonObject res;
    if (kind == "file") {
        QFile f(src);
        if (!f.open(QIODevice::ReadOnly)) return ppcli::fail("analyze: cannot read " + src);
        while (!f.atEnd()) {
            QByteArray chunk = f.read(tskit::kPacketSize * 2048);
            if (chunk.isEmpty()) break;
            if (maxPackets > 0) chunk.truncate(int(qMin<qint64>(chunk.size(), (maxPackets - an.packets()) * tskit::kPacketSize)));
            an.feed(chunk);
            if ((maxPackets > 0 && an.packets() >= maxPackets) || (seconds > 0 && an.streamSeconds() >= seconds) || (stopOnTables && an.tableLimitReached())) break;
        }
    } else {
        bool okBuf = true;
        const int recvTimeout = a.integer("--receive-timeout", 5000, &ok);
        const int bufferSize = a.integer("--buffer-size", 2097152, &okBuf);
        if (!ok || recvTimeout <= 0 || !okBuf || bufferSize < 0) return ppcli::fail("analyze: bad --receive-timeout / --buffer-size");
        if (a.has("--local-address")) udp.localAddress = a.value("--local-address");
        tskit::UdpReceiver rx;
        if (!rx.open(udp, bufferSize, &err)) return ppcli::fail("analyze: " + err);
        qint64 lastDataMs = 0, datagrams = 0;
        bool timedOut = false;
        QByteArray dg;
        for (;;) {
            const qint64 now = wall.elapsed();
            if (seconds > 0 && now >= qint64(seconds * 1000.0)) break;
            qint64 wait = recvTimeout - (now - lastDataMs);
            if (wait <= 0) { timedOut = true; break; }
            if (seconds > 0) wait = qMin(wait, qint64(seconds * 1000.0) - now);
            const int n = rx.receive(&dg, int(qBound(qint64(1), wait, qint64(200))));
            if (n < 0) return ppcli::fail("analyze: " + rx.lastError());
            if (n == 0) continue;
            lastDataMs = wall.elapsed();
            ++datagrams;
            an.feed(tskit::udpTsPayload(dg), tskit::UdpReceiver::nowNs());
            if ((maxPackets > 0 && an.packets() >= maxPackets) || (stopOnTables && an.tableLimitReached())) break;
        }
        res["datagrams"] = double(datagrams);
        res["receiveTimeout"] = timedOut;
        if (datagrams == 0) {
            an.finish();
            return ppcli::fail(QString("analyze: no data received on %1 within %2 ms").arg(udp.toString()).arg(recvTimeout), QJsonObject{{"receiveTimeout", true}, {"source", udp.toString()}});
        }
    }
    an.finish();
    if (mode != "tables") write(an.reportText());
    if (textFile) textFile->close();
    const QJsonObject summary = an.toJson();
    for (auto it = summary.constBegin(); it != summary.constEnd(); ++it) res[it.key()] = it.value();
    res["ok"] = an.packets() > 0;
    if (an.packets() == 0) res["error"] = "no TS packets in " + src;
    res["input"] = kind;
    res["source"] = kind == "udp" ? udp.toString() : src;
    res["mode"] = mode;
    res["wallSec"] = double(wall.elapsed()) / 1000.0;
    if (textFile) res["output"] = textFile->fileName();
    return ppcli::finish(res);
}

// dektec: DekTec devices through DTAPI (tools/mux/dtdevice.*), replacing MuxManager's two tsdektec calls.
//   dektec list [-v|--verbose] [--probe] [--quiet]          = tsdektec -a [-v]; text first, JSON {devices[]} last
//       --probe   also ask DTAPI whether each channel is attached (ProbeOnly: nothing is held)
//   dektec prep --port P [--direction output|input] [--device D | --serial S | <device>] [--io-standard asi|keep]
//               [--check] [--force]                         = tsdektec --output P <device> (also --output P / --input P)
//       output ports also get the DVB-ASI I/O standard unless --io-standard keep; nothing that already matches is
//       re-applied (unless --force); a port whose channel is attached by any process is refused (exit 2, "inUse": true).
//   --sim F (or PLAYPLAYMUX_DEKTEC_SIM=F): a JSON-described card instead of the hardware (tests); prep writes the new state
//   back to F. Without DTAPI compiled in, list answers no devices and prep is unsupported (exit 64: run tsdektec).
bool parseSerial(const QString& s, qint64* out) {
    bool ok = false;
    const QString t = s.trimmed();
    *out = t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? t.mid(2).toLongLong(&ok, 16) : t.toLongLong(&ok, 10);
    return ok && *out > 0;
}

int cmdDektec(QStringList args) {
    if (args.isEmpty()) return ppcli::fail("usage: PlayPlayMux dektec list [-v] [--probe] | prep --port P [--device D] [--check] ...");
    const QString sub = args.takeFirst();
    if (sub != "list" && sub != "prep") return ppcli::unsupported("dektec " + sub);
    const ppcli::Args a = sub == "list"
        ? ppcli::Args(args, {"--verbose", "-v", "--probe", "--quiet", "--sim="})
        : ppcli::Args(args, {"--port=", "-p=", "--output=", "-o=", "--input=", "-i=", "--direction=", "--device=", "-d=", "--serial=",
                             "--io-standard=", "--check", "--force", "--sim="}, true);
    if (!a.ok()) return a.report();

    const QString simPath = a.value("--sim", qEnvironmentVariable("PLAYPLAYMUX_DEKTEC_SIM"));
    std::unique_ptr<tskit::dt::SimHardware> sim;
    std::unique_ptr<tskit::dt::Hardware> real;
    if (!simPath.isEmpty()) {
        QString err;
        sim = tskit::dt::openSim(simPath, &err);
        if (!sim) return ppcli::fail("dektec: " + err);
    } else {
        real = tskit::dt::openDtapi();
    }
    tskit::dt::Hardware& hw = sim ? static_cast<tskit::dt::Hardware&>(*sim) : *real;

    if (sub == "list") {
        QList<tskit::dt::Device> devices;
        tskit::dt::Versions versions;
        QJsonObject res = tskit::dt::listResult(hw, a.has("--probe"), &devices, &versions);
        if (res.contains("scanError")) ppcli::log("dektec list: " + res.value("scanError").toString());
        if (!a.has("--quiet")) {
            const QByteArray t = tskit::dt::listText(devices, versions, a.has("--verbose") || a.has("-v")).toUtf8();
            std::fwrite(t.constData(), 1, size_t(t.size()), stdout);
            std::fflush(stdout);
        }
        if (sim) res["simulated"] = simPath;
        return ppcli::finish(res);
    }

    // prep: --port P [--direction D], or tsdektec's --output P / --input P; the device as --device, --serial or positional.
    tskit::dt::PrepRequest req;
    const QStringList portOpts{"--port", "-p", "--output", "-o", "--input", "-i"};
    QString portText, dirOpt;
    for (const QString& o : portOpts) {
        if (!a.has(o)) continue;
        if (!portText.isEmpty()) return ppcli::fail("dektec prep: give one of --port, --output, --input");
        portText = a.value(o);
        dirOpt = o;
    }
    if (portText.isEmpty() || !parseInt(portText, &req.port) || req.port < 1) return ppcli::fail("dektec prep: --port P (1-based, as tsdektec -av shows it) is required");
    QString direction = dirOpt == "--input" || dirOpt == "-i" ? QStringLiteral("input") : QStringLiteral("output");
    if (a.has("--direction")) {
        if (dirOpt != "--port" && dirOpt != "-p") return ppcli::fail("dektec prep: --direction goes with --port");
        direction = a.value("--direction").toLower();
    }
    if (direction != "output" && direction != "input") return ppcli::unsupported("--direction " + direction);
    req.output = direction == "output";
    const QString ioStd = a.value("--io-standard", "asi").toLower();
    if (ioStd != "asi" && ioStd != "keep") return ppcli::unsupported("--io-standard " + ioStd);
    req.asi = ioStd == "asi";
    const QStringList pos = a.positional();
    const int deviceSources = int(a.has("--device") || a.has("-d")) + int(a.has("--serial")) + int(!pos.isEmpty());
    if (deviceSources > 1 || pos.size() > 1) return ppcli::fail("dektec prep: give the device once (--device D, --serial S or a positional index)");
    if (a.has("--serial") && !parseSerial(a.value("--serial"), &req.serial)) return ppcli::fail("dektec prep: bad --serial");
    const QString devText = a.has("--device") || a.has("-d") ? a.value("--device", a.value("-d")) : pos.value(0, "0");
    if (!parseInt(devText, &req.device) || req.device < 0) return ppcli::fail("dektec prep: bad device index " + devText);
    req.check = a.has("--check");
    req.force = a.has("--force");

    QJsonObject res = tskit::dt::prepare(hw, req);
    if (!res.value("ok").toBool()) ppcli::log("dektec prep: " + res.value("error").toString());
    if (sim) {
        res["simulated"] = simPath;
        QString err;
        if (!req.check && !res.value("actions").toArray().isEmpty() && !tskit::dt::saveSim(*sim, simPath, &err)) return ppcli::fail("dektec: " + err);
    }
    return ppcli::finish(res);
}

int cmdVersion() {
    QJsonObject res;
    res["ok"] = true; res["tool"] = "PlayPlayMux"; res["version"] = "1.2";
    res["commands"] = QJsonArray{"compile", "pmt-scte35", "packetize", "analyze", "dektec", "version"};
    res["dtapi"] = tskit::dt::openDtapi()->available();
    return ppcli::finish(res);
}

}   // namespace

int main(int argc, char** argv) {
    ppcli::initStdio();
    QCoreApplication app(argc, argv);                     // Unicode argv on Windows (GetCommandLineW)
    QStringList args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty()) return ppcli::fail("usage: PlayPlayMux compile|pmt-scte35|packetize|analyze|dektec|version ...");
    const QString cmd = args.takeFirst();
    if (cmd == "compile") return cmdCompile(args);
    if (cmd == "pmt-scte35") return cmdPmtScte35(args);
    if (cmd == "packetize") return cmdPacketize(args);
    if (cmd == "analyze") return cmdAnalyze(args);
    if (cmd == "dektec") return cmdDektec(args);
    if (cmd == "version" || cmd == "--version") return cmdVersion();
    return ppcli::unsupported("command " + cmd);
}
