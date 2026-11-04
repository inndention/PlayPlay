// Lane inhouse-mux (docs/plans/inhouse-deps.md).
//  - MX-0: MuxManager's pure ffmpeg argument builder for `input` services (the mpeg2 `-flags` statement had been swallowed by a
//    // comment, so 1080i/480i services were never coded interlaced and never closed their GOPs).
//  - DEP-14: PlayPlayTsKit (tools/mux): sections byte-identical with goldens made once with `tstabcomp --atsc` / `tspacketize`
//    (TSDuck 3.41) on the dev box, tests/inhouse/data/psip/*.xml -> *.bin / *.ts. The test itself needs no TSDuck; when
//    tstabcomp is installed, writeTablesMatchTstabcomp also compares what MuxManager::writeTables writes today.
//  - DEP-02b: MuxManager::capabilities() in-process (PlayPlayAvKit) instead of `ffmpeg -encoders` / `-version` spawns, tsp's
//    version from its version resource, the DekTec listing in the background, and nothing started when TSDuck is missing.
//  - DEP-15: compileTables() in-process (mux.config.tables = native; 261 files at eitDays 16 in well under a second, no child
//    process; tstabcomp only for XML outside the subset or with tables = tstabcomp), the compiler robust to mangled XML, and
//    analyze / DekTec listing / the port setup before tsp as asynchronous children (PlayPlayMux or TSDuck) that never block
//    the event loop. Every PlayPlayMux run here sees a simulated card (PLAYPLAYMUX_DEKTEC_SIM, checked before each start):
//    no test touches the DTA-2154; the fake tsp is a copy of PlayPlayMux.exe (it rejects tsp's arguments and exits).
//  - DEP-18: network `input` services (rtsp/srt/rtmp/udp/http) become hidden managed channels `mux-<id>` pinned to
//    {type:"stream", url} with the service's encoder profile; their ffmpeg encoders go (PSIP_DEP18_DUMP=<dir> writes the
//    managed channel config + fixed item for the standalone-engine bench run in docs/plans/impl-log/inhouse-mux.md).
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QUdpSocket>
#include <memory>
#include "../../playout/supervisor/muxmanager.h"
#include "../../playout/supervisor/restapiserver.h"
#include "../../playout/supervisor/scte35.h"
#include "../../playout/schedule/schedulestore.h"
#include "psip.h"
#include "scte35bin.h"
#include "tssection.h"
#ifdef Q_OS_WIN
#include <windows.h>
// analyzeFallsBackToTsp starts a text file named PlayPlayMux.exe on purpose. At an interactive desktop Windows answers with the
// modal "Unsupported 16-Bit Application" box, which blocks CreateProcess until somebody clicks OK (the test then timed out under ctest).
// With these error modes the start simply fails, which is what the test expects.
namespace { const struct NoErrorDialogs { NoErrorDialogs() { SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX); } } g_noErrorDialogs; }
#endif

using J = QJsonObject;

static QString golden(const QString& name) { return QStringLiteral(PSIP_GOLDEN_DIR) + "/" + name; }
static QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

// "" when equal, else where the bytes part (with hex context) for the failure message.
static QString firstDiff(const QByteArray& got, const QByteArray& want) {
    int i = 0;
    while (i < got.size() && i < want.size() && got[i] == want[i]) ++i;
    if (i == got.size() && i == want.size()) return {};
    return QString("sizes %1 / %2, first difference at byte %3\n  got  %4\n  want %5").arg(got.size()).arg(want.size()).arg(i)
        .arg(QString::fromLatin1(got.mid(qMax(0, i - 8), 24).toHex(' '))).arg(QString::fromLatin1(want.mid(qMax(0, i - 8), 24).toHex(' ')));
}

static QList<QByteArray> sectionsOf(const QString& bin) { QString e; const QList<QByteArray> s = tskit::splitSections(readAll(golden(bin)), &e); if (!e.isEmpty()) qWarning() << bin << e; return s; }

// Minimal TS -> sections reassembly that also enforces the packing rules: PUSI + pointer_field land on a section start,
// a section header never straddles two packets, everything after the last section in a packet is 0xFF, CC runs by one.
static QList<QByteArray> depacketize(const QByteArray& ts, int pid, QString* err) {
    QList<QByteArray> out;
    QByteArray cur; int need = 0, cc = -1;
    auto bad = [&](const QString& e, int pkt) { *err = QString("packet %1: %2").arg(pkt).arg(e); return QList<QByteArray>(); };
    if (ts.size() % 188) return bad("not a whole number of packets", 0);
    for (int k = 0; k < ts.size() / 188; ++k) {
        const char* p = ts.constData() + k * 188;
        if (quint8(p[0]) != 0x47 || tskit::packetPid(p) != pid || tskit::packetAfc(p) != 1) return bad("bad header", k);
        if (cc >= 0 && tskit::packetCc(p) != ((cc + 1) & 15)) return bad("continuity counter jump", k);
        cc = tskit::packetCc(p);
        QByteArray pay(p + 4, 184);
        int pos = 0;
        if (tskit::packetPusi(p)) {
            const int ptr = quint8(pay[0]); pay.remove(0, 1);
            if (need != ptr) return bad(QString("pointer_field %1 but %2 bytes were missing").arg(ptr).arg(need), k);
            if (need) { cur.append(pay.left(ptr)); out.append(cur); cur.clear(); need = 0; }
            pos = ptr;
            while (pos < pay.size() && quint8(pay[pos]) != 0xFF) {
                const int avail = int(pay.size()) - pos;
                if (avail < 3) return bad("section header split across packets", k);
                const int size = 3 + (((quint8(pay[pos + 1]) & 0x0F) << 8) | quint8(pay[pos + 2]));
                if (avail >= size) { out.append(pay.mid(pos, size)); pos += size; continue; }
                if (avail < ((quint8(pay[pos + 1]) & 0x80) ? 8 : 3)) return bad("section header split across packets", k);
                cur = pay.mid(pos); need = size - avail; pos = int(pay.size());
            }
        } else {
            if (need <= 0) return bad("payload without a section in progress", k);
            const int take = qMin(need, int(pay.size()));
            cur.append(pay.left(take)); need -= take; pos = take;
            if (!need) { out.append(cur); cur.clear(); }
        }
        for (int i = pos; i < pay.size(); ++i) if (quint8(pay[i]) != 0xFF) return bad("no 0xFF stuffing after the last section", k);
    }
    if (need) return bad("last section incomplete", int(ts.size() / 188));
    return out;
}

// JSON written with single quotes (moc misreads some raw string literals)
static J sq(const char* json) { return QJsonDocument::fromJson(QByteArray(json).replace('\'', '"')).object(); }

static J inputService(const QString& id, const J& encoder) {
    J s; s["id"] = id; s["encoder"] = encoder;
    J src; src["kind"] = "input"; src["input"] = sq("{'type':'test','location':''}"); s["source"] = src;
    return s;
}

// ---- DEP-02b helpers
// The "ffmpeg" object exactly as MuxManager::capabilities() built it before DEP-02b, from two spawns of an ffmpeg.exe.
static J legacyFfmpegCaps(const QString& exe) {
    J ff; ff["path"] = exe;
    QProcess p; p.setProcessChannelMode(QProcess::MergedChannels); p.start(exe, {"-hide_banner", "-encoders"}); const bool fin = p.waitForFinished(15000);
    ff["found"] = fin && p.exitCode() == 0;
    const J enc = MuxManager::parseFfmpegEncoders(QString::fromUtf8(p.readAll()));
    J has = enc.value("has").toObject();
    for (const char* n : {"libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf", "h264_mf", "hevc_mf", "ac3", "eac3", "aac", "mp2", "libopus"}) if (!has.contains(n)) has[n] = false;
    QProcess v; v.setProcessChannelMode(QProcess::MergedChannels); v.start(exe, {"-hide_banner", "-version"}); v.waitForFinished(10000);
    const QString ver = QString::fromUtf8(v.readAll()); ff["version"] = ver.section('\n', 0, 0).trimmed(); const QString conf = ver.section("configuration:", 1).section('\n', 0, 0);
    J build; build["srt"] = conf.contains("--enable-libsrt"); build["rist"] = conf.contains("--enable-librist"); build["ndi"] = conf.contains("libndi"); build["qsv"] = conf.contains("--enable-libvpl") || conf.contains("--enable-libmfx"); build["nvenc"] = conf.contains("--enable-nvenc") || conf.contains("--enable-ffnvcodec") || has.value("h264_nvenc").toBool(); build["x264"] = conf.contains("--enable-libx264"); build["x265"] = conf.contains("--enable-libx265");
    ff["build"] = build; ff["video"] = enc.value("video"); ff["audio"] = enc.value("audio"); ff["has"] = has;
    return ff;
}
static QString compact(const QJsonValue& v) { return v.isObject() ? QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact)) : v.isArray() ? QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)) : v.toVariant().toString(); }
// "" when equal, else the differing keys / indexes with both values (QCOMPARE on QJsonObject prints nothing useful).
static QString jsonDiff(const QJsonValue& got, const QJsonValue& want, const QString& at = QString()) {
    if (got == want) return {};
    if (got.isObject() && want.isObject()) {
        QStringList keys = got.toObject().keys() + want.toObject().keys(); keys.removeDuplicates(); keys.sort(); QStringList d;
        for (const QString& k : keys) { const QString s = jsonDiff(got.toObject().value(k), want.toObject().value(k), at + "." + k); if (!s.isEmpty()) d << s; if (d.size() >= 6) break; }
        return d.join("; ");
    }
    if (got.isArray() && want.isArray() && got.toArray().size() == want.toArray().size()) {
        QStringList d; for (int i = 0; i < got.toArray().size() && d.size() < 6; ++i) { const QString s = jsonDiff(got.toArray()[i], want.toArray()[i], at + QString("[%1]").arg(i)); if (!s.isEmpty()) d << s; }
        return d.join("; ");
    }
    return at + ": got " + compact(got).left(160) + " want " + compact(want).left(160);
}
// Sets an environment variable for one scope (PATH without TSDuck), restoring it afterwards.
struct EnvScope {
    QByteArray name, old; bool had;
    EnvScope(const char* n, const QByteArray& v) : name(n), old(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) { qputenv(n, v); }
    ~EnvScope() { if (had) qputenv(name.constData(), old); else qunsetenv(name.constData()); }
};
static QByteArray pathWithoutTsduck() {
    QStringList keep;
    for (const QString& d : qEnvironmentVariable("PATH").split(';', Qt::SkipEmptyParts))
        if (!QFileInfo::exists(d + "/tsp.exe") && !QFileInfo::exists(d + "/tsdektec.exe") && !QFileInfo::exists(d + "/tstabcomp.exe")) keep << d;
    return keep.join(';').toLocal8Bit();
}
// ---- DEP-15 helpers
static QString muxExe() { return QStringLiteral(PLAYPLAYMUX_EXE); }
static bool writeFile(const QString& path, const QByteArray& data) { QFile f(path); return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(data) == data.size(); }
// The recorded DTA-2154 as a simulation (tests/inhouse/data/psip/dektec), output port 2 adjusted.
static QString simWithPort2(const QString& dir, const QString& name, bool inUse, const QString& ioDir) {
    QJsonObject sim = QJsonDocument::fromJson(readAll(golden("dektec/dta2154_sim.json"))).object();
    QJsonArray devs = sim["devices"].toArray(); QJsonObject d = devs[0].toObject(); QJsonArray ports = d["ports"].toArray();
    QJsonObject p2 = ports[1].toObject(); p2["inUse"] = inUse; p2["ioDir"] = ioDir; ports[1] = p2; d["ports"] = ports; devs[0] = d; sim["devices"] = devs;
    const QString path = QDir(dir).filePath(name);
    return writeFile(path, QJsonDocument(sim).toJson()) ? path : QString();
}
static QJsonObject lastJsonLine(const QString& text) { const QString t = text.trimmed(); return QJsonDocument::fromJson(t.mid(t.lastIndexOf('\n') + 1).toUtf8()).object(); }
// PlayPlayMux started the way MuxManager starts it (inherited environment) must see the simulated card, never the hardware.
static QString simGuard() {
    QProcess p; p.start(muxExe(), {"dektec", "list", "--quiet"});
    if (!p.waitForFinished(15000)) return "PlayPlayMux dektec list did not finish";
    const QJsonObject j = lastJsonLine(QString::fromUtf8(p.readAllStandardOutput()));
    return j.contains("simulated") ? QString() : "PlayPlayMux does not see the simulated card: " + QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact)).left(200);
}
static QJsonArray udpServices(int n) {
    QJsonArray a;
    for (int i = 1; i <= n; ++i) {
        J s; s["id"] = QString("svc%1").arg(i); s["serviceId"] = i; s["shortName"] = QString("STAR-%1").arg(i); s["longName"] = QString("Star Test %1").arg(i);
        s["major"] = 25; s["minor"] = i; s["udpPort"] = 50075 + i;
        J src; src["kind"] = "udp"; src["url"] = QString("udp://127.0.0.1:%1").arg(50075 + i); s["source"] = src; a.append(s);
    }
    return a;
}
// A MuxManager with a settings store (mux.tableOverrides lives there) in a scratch dir.
struct MuxWithStore {
    QTemporaryDir dir; sched::ScheduleStore store; std::unique_ptr<MuxManager> mux;
    bool open() { if (!dir.isValid() || !store.open(dir.filePath("t.db"))) return false; mux = std::make_unique<MuxManager>(&store, dir.filePath("data")); return true; }
};
// Longest gap between 5 ms ticks of the event loop while waiting for `until` (children must never stall it).
static qint64 maxLoopGapMs(const std::function<bool()>& until, int timeoutMs) {
    QElapsedTimer t, gap; t.start(); gap.start(); qint64 worst = 0;
    QTimer tick; tick.setInterval(5); QObject::connect(&tick, &QTimer::timeout, [&]() { worst = qMax(worst, gap.restart()); }); tick.start();
    while (!until() && t.elapsed() < timeoutMs) QTest::qWait(5);
    return worst;
}
static bool logHas(const MuxManager& m, const QString& part) { for (const QJsonValue& v : m.log(2000)) if (v.toString().contains(part)) return true; return false; }
static QString logText(const MuxManager& m) { QStringList l; for (const QJsonValue& v : m.log(60)) l << v.toString(); return l.join('\n'); }
static const QString kTstabcomp = "C:/Program Files/TSDuck/bin/tstabcomp.exe";
// A transport stream like the one tsp writes for output.kind = file: PAT on 0x0000, MGT + TVCT + STT on 0x1FFB and EIT-0 on 0x0200
// from the XML writeTables wrote (compiled in-process), `rounds` times, each followed by `nulls` null packets.
static QByteArray tsFromTables(const QString& tablesDir, int rounds, int nulls, QString* err) {
    auto secs = [&](const QString& f) {
        const tskit::CompileResult r = tskit::compileFile(QDir(tablesDir).filePath(f), QString());
        if (!r.ok && err->isEmpty()) *err = r.error;
        QList<QByteArray> s; for (const tskit::CompiledTable& t : r.tables) s += t.sections; return s;
    };
    const QList<QByteArray> pat = secs("pat.xml"), psip = secs("mgt.xml") + secs("tvct.xml") + secs("stt.xml"), eit = secs("eit0.xml");
    tskit::Packetizer p0(0x0000), p1(0x1FFB), p2(0x0200);
    QByteArray nul(188, char(0xFF)); nul[0] = 0x47; nul[1] = 0x1F; nul[3] = 0x10;
    QByteArray ts;
    for (int r = 0; r < rounds; ++r) { ts += p0.packetize(pat) + p1.packetize(psip) + p2.packetize(eit); for (int i = 0; i < nulls; ++i) ts += nul; }
    return ts;
}
// One HTTP call against a RestApiServer of this process; the event loop keeps running while it waits (the routes are async).
static J httpCall(QNetworkAccessManager& nam, const QString& method, const QString& url, int* status, int timeoutMs = 20000) {
    QNetworkRequest rq{QUrl(url)}; rq.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* r = method == "POST" ? nam.post(rq, QByteArray("{}")) : nam.get(rq);
    QElapsedTimer t; t.start();
    while (!r->isFinished() && t.elapsed() < timeoutMs) QTest::qWait(5);
    *status = r->isFinished() ? r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : -1;
    const J o = QJsonDocument::fromJson(r->readAll()).object(); r->deleteLater(); return o;
}
static quint16 freeTcpPort() { QTcpServer s; return s.listen(QHostAddress::LocalHost, 0) ? s.serverPort() : 0; }
// A scratch "TSDuck" directory whose tsp.exe is a copy of PlayPlayMux.exe: started with tsp's arguments it answers "unsupported
// command" and exits, so a mux start can reach the master without TSDuck, and no fallback can find the real tsdektec / tsp.
static QString fakeTsduckDir(const QString& parent) {
    const QString d = QDir(parent).filePath("tsduck"); QDir().mkpath(d);
    return QFile::exists(d + "/tsp.exe") || QFile::copy(muxExe(), d + "/tsp.exe") ? d : QString();
}
static QString sdtXml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <SDT version=\"0\" current=\"true\" transport_stream_id=\"0x0019\" original_network_id=\"0x0001\" actual=\"true\">\n"
           "    <service service_id=\"0x0001\" EIT_schedule=\"false\" EIT_present_following=\"false\" running_status=\"running\" CA_mode=\"false\">\n"
           "      <service_descriptor service_type=\"0x01\" service_provider_name=\"Star\" service_name=\"Star 1\"/>\n    </service>\n  </SDT>\n</tsduck>\n";
}
static const QString kTruncatedXml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <PAT version=\"1\" current=\"true\" transport_stream_id=\"0x0019\">\n    <service service_id=\"0x0001\" program_map_PID=\"0x0021\"/>\n";
static J fileEntry(const J& report, const QString& name) { for (const QJsonValue& v : report.value("files").toArray()) if (v.toObject().value("file").toString() == name) return v.toObject(); return {}; }

static const QStringList kWatched = {"libx264", "libx265", "mpeg2video", "h264_qsv", "hevc_qsv", "h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf", "h264_mf", "hevc_mf", "ac3", "eac3", "aac", "mp2", "libopus"};

// Output-side options only: everything after "-c:v" (the dshow input carries its own "-flags low_delay" before -i).
static int outputIndexOf(const QStringList& a, const QString& opt) { const int cv = a.indexOf("-c:v"); return cv < 0 ? -1 : a.indexOf(opt, cv); }
static QString outputValue(const QStringList& a, const QString& opt) { const int i = outputIndexOf(a, opt); return i >= 0 && i + 1 < a.size() ? a[i + 1] : QString(); }

class TestInhousePsip : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

    // One mux with just this service (normalised by setServices: preset merge, statmux defaults), then its ffmpeg args.
    QStringList argsFor(const J& encoder, QString* error) {
        MuxManager mux(nullptr, m_dir.path());
        mux.setServices(QJsonArray{inputService("svc1", encoder)});
        const QJsonArray svcs = mux.services();
        if (svcs.size() != 1) return {};
        return mux.ffmpegArgs(svcs[0].toObject(), error);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        // Every PlayPlayMux this test starts (MuxManager's DekTec listing, the port setup) sees a simulated card without devices
        // unless a test points it at its own simulation: nothing here touches real DekTec hardware through PlayPlayMux.
        const QString empty = m_dir.filePath("nodevices_sim.json");
        QVERIFY(writeFile(empty, "{\"versions\": {\"dtapi\": \"sim\", \"dta\": \"sim\"}, \"devices\": []}"));
        qputenv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(empty).toLocal8Bit());
    }

    // ------------------------------------------------------------------ MX-0: mpeg2 -flags
    void mpeg2Flags_data() {
        QTest::addColumn<QString>("encoder");        // single-quoted JSON
        QTest::addColumn<QString>("flags");          // expected -flags value; empty = no -flags at all
        QTest::addColumn<bool>("altScan");           // -alternate_scan 1 (interlaced coding)
        QTest::addColumn<bool>("tff");               // interlace=scan=tff in -vf
        QTest::newRow("preset atsc_mpeg2_ac3_hd (1080i, closed GOP)") << "{'applyPreset':'atsc_mpeg2_ac3_hd'}" << "+cgop+ilme+ildct" << true << true;
        QTest::newRow("preset atsc_mpeg2_ac3_sd (480i, closed GOP)") << "{'applyPreset':'atsc_mpeg2_ac3_sd'}" << "+cgop+ilme+ildct" << true << true;
        QTest::newRow("preset mpeg2_ac3 (1080p, closed GOP)") << "{'applyPreset':'mpeg2_ac3'}" << "+cgop" << false << false;
        QTest::newRow("1080i open GOP") << "{'codec':'mpeg2video','width':1920,'height':1080,'interlaced':true,'closedGop':false}" << "+ilme+ildct" << true << true;
        QTest::newRow("576i closed GOP") << "{'codec':'mpeg2video','width':720,'height':576,'interlaced':true,'closedGop':true,'fps':'25'}" << "+cgop+ilme+ildct" << true << true;
        QTest::newRow("720p asked interlaced stays progressive") << "{'codec':'mpeg2video','width':1280,'height':720,'interlaced':true}" << "+cgop" << false << false;
        QTest::newRow("1080p open GOP: no -flags") << "{'codec':'mpeg2video','width':1920,'height':1080,'interlaced':false,'closedGop':false}" << "" << false << false;
    }
    void mpeg2Flags() {
        QFETCH(QString, encoder); QFETCH(QString, flags); QFETCH(bool, altScan); QFETCH(bool, tff);
        QString err; const QStringList a = argsFor(sq(encoder.toUtf8().constData()), &err);
        QVERIFY2(!a.isEmpty(), qPrintable(err));
        QVERIFY2(err.isEmpty(), qPrintable(err));                       // no capabilities probed: no codec fallback
        QCOMPARE(outputValue(a, "-c:v"), QString("mpeg2video"));
        const int fi = outputIndexOf(a, "-flags");
        if (flags.isEmpty()) QCOMPARE(fi, -1);
        else {
            QVERIFY2(fi > 0, qPrintable(a.join(' ')));
            QCOMPARE(a[fi + 1], flags);
            QCOMPARE(a.indexOf("-flags", fi + 1), -1);                  // exactly once
            QVERIFY(fi < a.indexOf("-f", fi));                          // a video option, before the mpegts muxer and the URL
            QVERIFY(a.last().startsWith("udp://"));
        }
        QCOMPARE(outputValue(a, "-alternate_scan") == "1", altScan);
        const int vf = a.indexOf("-vf");                                // the filter chain precedes -c:v
        QVERIFY(vf > 0 && vf < a.indexOf("-c:v"));
        QCOMPARE(a[vf + 1].contains("interlace=scan=tff"), tff);
        QVERIFY(!flags.contains("+cgop") || outputValue(a, "-sc_threshold") == "1000000000");   // the only pair FFmpeg refuses to open
    }

    // mpeg2video refuses +cgop with scene-change detection (sc_threshold < 1e9: "closed gop with scene change detection are not
    // supported yet", the encoder does not open). Closed GOP wins and the dropped scene-cut is reported; open GOP keeps scene-cut.
    void mpeg2SceneCutNeverWithClosedGop_data() {
        QTest::addColumn<QString>("encoder");
        QTest::addColumn<QString>("scThreshold");
        QTest::addColumn<bool>("cgop");
        QTest::addColumn<bool>("warned");
        QTest::newRow("1080i closed GOP + scene-cut") << "{'codec':'mpeg2video','width':1920,'height':1080,'interlaced':true,'closedGop':true,'scenecut':true}" << "1000000000" << true << true;
        QTest::newRow("1080p scene-cut, closedGop default") << "{'codec':'mpeg2video','width':1920,'height':1080,'scenecut':true}" << "1000000000" << true << true;
        QTest::newRow("480i open GOP + scene-cut") << "{'codec':'mpeg2video','width':704,'height':480,'interlaced':true,'closedGop':false,'scenecut':true}" << "0" << false << false;
        QTest::newRow("1080i closed GOP, no scene-cut") << "{'codec':'mpeg2video','width':1920,'height':1080,'interlaced':true,'closedGop':true,'scenecut':false}" << "1000000000" << true << false;
        QTest::newRow("1080p open GOP, no scene-cut") << "{'codec':'mpeg2video','width':1920,'height':1080,'closedGop':false,'scenecut':false}" << "1000000000" << false << false;
    }
    void mpeg2SceneCutNeverWithClosedGop() {
        QFETCH(QString, encoder); QFETCH(QString, scThreshold); QFETCH(bool, cgop); QFETCH(bool, warned);
        QString err; const QStringList a = argsFor(sq(encoder.toUtf8().constData()), &err);
        QVERIFY2(!a.isEmpty(), qPrintable(err));
        QCOMPARE(outputValue(a, "-c:v"), QString("mpeg2video"));
        QCOMPARE(outputValue(a, "-sc_threshold"), scThreshold);
        QCOMPARE(outputValue(a, "-flags").contains("+cgop"), cgop);
        QCOMPARE(a.count("-sc_threshold"), 1);
        QCOMPARE(err.contains("scene-cut"), warned);
        if (!warned) QVERIFY2(err.isEmpty(), qPrintable(err));
    }

    // libx264 closes GOPs through x264-params (open-gop=0) and codes interlaced with interlaced=1:tff=1: no -flags.
    void x264UsesParamsNotFlags() {
        QString err;
        const QStringList a = argsFor(sq("{'applyPreset':'atsc_h264_ac3'}"), &err);
        QVERIFY2(!a.isEmpty(), qPrintable(err));
        QCOMPARE(outputValue(a, "-c:v"), QString("libx264"));
        QCOMPARE(outputIndexOf(a, "-flags"), -1);
        QVERIFY(outputValue(a, "-x264-params").contains("open-gop=0"));
        const QStringList i = argsFor(sq("{'codec':'libx264','width':1920,'height':1080,'interlaced':true}"), &err);
        QVERIFY(outputValue(i, "-x264-params").contains("interlaced=1:tff=1"));
        QCOMPARE(outputIndexOf(i, "-flags"), -1);
    }

    // `ffmpeg -encoders` (FFmpeg 9 layout). mpeg2video carries the slice-threading flag ("V.S..D"); the old pattern skipped it, so
    // capabilities() reported no mpeg2video and every mpeg2 input service fell back to libx264 (the -flags fix alone changed nothing).
    void parsesEncoderList() {
        const QString text = QStringLiteral(
            "Encoders:\r\n V..... = Video\r\n A..... = Audio\r\n S..... = Subtitle\r\n .F.... = Frame-level multithreading\r\n"
            " ..S... = Slice-level multithreading\r\n ...X.. = Codec is experimental\r\n ....B. = Supports draw_horiz_band\r\n"
            " .....D = Supports direct rendering method 1\r\n ------\r\n"
            " V....D libx264              libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (codec h264)\r\n"
            " V....D h264_nvenc           NVIDIA NVENC H.264 encoder (codec h264)\r\n"
            " V.S..D mpeg2video           MPEG-2 video\r\n"
            " V..... mpeg2_qsv            MPEG-2 video (Intel Quick Sync Video acceleration) (codec mpeg2video)\r\n"
            " VFS..D dnxhd                VC3/DNxHD\r\n"
            " A....D ac3                  ATSC A/52A (AC-3)\r\n"
            " A....D aac                  AAC (Advanced Audio Coding)\r\n"
            " S..... ssa                  ASS (Advanced SubStation Alpha) subtitle (codec ass)\r\n");
        const J r = MuxManager::parseFfmpegEncoders(text);
        const J has = r.value("has").toObject();
        for (const char* n : {"libx264", "h264_nvenc", "mpeg2video", "mpeg2_qsv", "dnxhd", "ac3", "aac", "ssa"}) QVERIFY2(has.value(n).toBool(), n);
        QVERIFY(!has.contains("="));                                       // the legend is not an encoder
        QCOMPARE(r.value("video").toArray().size(), 5);
        QCOMPARE(r.value("audio").toArray().size(), 2);
        QCOMPARE(r.value("video").toArray()[2].toObject().value("description").toString(), QString("MPEG-2 video"));
        // No separator line (unexpected layout): rows still parse, legend rows still skipped.
        const J r2 = MuxManager::parseFfmpegEncoders(" V..... = Video\n V.S..D mpeg2video           MPEG-2 video\n");
        QVERIFY(r2.value("has").toObject().value("mpeg2video").toBool());
        QCOMPARE(r2.value("video").toArray().size(), 1);
    }

    // The dshow input keeps its input-side "-flags low_delay" (before -i), separate from the mpeg2 output flags.
    void dshowInputFlagsStaySeparate() {
        MuxManager mux(nullptr, m_dir.path());
        J s = inputService("svc1", sq("{'applyPreset':'atsc_mpeg2_ac3_hd'}"));
        s["source"] = sq("{'kind':'input','input':{'type':'dshow','location':'Some Camera'}}");
        mux.setServices(QJsonArray{s});
        QString err; const QStringList a = mux.ffmpegArgs(mux.services()[0].toObject(), &err);
        QVERIFY2(!a.isEmpty(), qPrintable(err));
        const int in = a.indexOf("-i");
        QVERIFY(in > 0);
        QVERIFY(a.indexOf("-flags") >= 0 && a.indexOf("-flags") < in);     // the first -flags is the input-side one
        QCOMPARE(a[a.indexOf("-flags") + 1], QString("low_delay"));
        QCOMPARE(outputValue(a, "-flags"), QString("+cgop+ilme+ildct"));
    }

    // ------------------------------------------------------------------ DEP-14: PlayPlayTsKit
    void crc32KnownVectors() {
        QCOMPARE(tskit::crc32(QByteArray("123456789")), 0x0376E6E7u);      // CRC-32/MPEG-2 check value
        QCOMPARE(tskit::crc32(QByteArray()), 0xFFFFFFFFu);
        QCOMPARE(tskit::crc32(QByteArray(1, '\0')), 0x4E08BFB4u);
        // The PAT golden: CRC over the section minus its last 4 bytes is those 4 bytes; over all of it, 0.
        const QByteArray pat = readAll(golden("pat.bin"));
        QCOMPARE(pat.size(), 20);
        QCOMPARE(tskit::crc32(pat.constData(), 16), 0xD609B15Du);
        QCOMPARE(tskit::crc32(pat), 0u);
        QVERIFY(tskit::crcValid(pat));
        QByteArray broken = pat; broken[10] = char(broken[10] ^ 0x01);
        QVERIFY(!tskit::crcValid(broken));
        QString e; QVERIFY(tskit::splitSections(broken, &e).isEmpty()); QVERIFY(e.contains("CRC"));
        tskit::fixCrc(broken); QVERIFY(tskit::crcValid(broken));
    }

    // Every golden XML compiles to exactly the bytes tstabcomp wrote for it.
    void goldens_data() {
        QTest::addColumn<QString>("name");
        for (const char* n : {"pat", "pmt", "pmt_scte35", "mgt", "tvct", "tvct_extra", "stt", "eit0", "ett0", "eit_unicode", "rrt", "descriptors",
                              "custom", "split", "quirks", "scte35_out", "scte35_in", "scte35_cancel", "scte35_null", "scte35_misc"})
            QTest::newRow(n) << QString(n);
    }
    void goldens() {
        QFETCH(QString, name);
        const QByteArray want = readAll(golden(name + ".bin"));
        QVERIFY2(!want.isEmpty(), qPrintable("missing golden " + name + ".bin"));
        const tskit::CompileResult r = tskit::compileFile(golden(name + ".xml"), QString());
        QVERIFY2(r.ok, qPrintable(r.error));
        const QString d = firstDiff(r.bin(), want);
        QVERIFY2(d.isEmpty(), qPrintable(name + ": " + d));
        for (const tskit::CompiledTable& t : r.tables)                  // CRCs intact (short sections have none)
            for (const QByteArray& s : t.sections) QVERIFY(tskit::crcValid(s));
        QCOMPARE(tskit::splitSections(want).size(), r.sectionCount());   // the .bin layout reads back
    }

    // Section splitting (split.xml: 60 TVCT channels, 40 long EIT events, 300 PAT programs) as TSDuck does it.
    void splitsLikeTsduck() {
        const tskit::CompileResult r = tskit::compileFile(golden("split.xml"), QString());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.tables.size(), 3);
        QCOMPARE(r.tables[0].sections.size(), 7);                       // TVCT: 9 channels of 106 bytes per 1024-byte section
        QCOMPARE(r.tables[1].sections.size(), 2);                       // EIT: 29 events per 4096-byte section
        QCOMPARE(r.tables[2].sections.size(), 2);                       // PAT: 253 programs per 1024-byte section
        for (const QByteArray& s : r.tables[0].sections) QVERIFY(s.size() <= 1024);
        QCOMPARE(r.tables[1].sections[0].size(), 4074);
        tskit::SectionInfo i; QVERIFY(tskit::sectionInfo(r.tables[0].sections[6], 0, &i));
        QCOMPARE(i.sectionNumber, 6); QCOMPARE(i.lastSectionNumber, 6);
        QVERIFY(r.tables[0].sections[6].contains(QByteArray::fromHex("fc04f102cafe")));   // additional descriptors in the last only
        QVERIFY(r.tables[0].sections[0].contains(QByteArray::fromHex("fc00")));
    }

    // The SCTE-35 XML the Supervisor really sends (playout/supervisor/scte35.cpp) compiles to the golden sections, and the
    // binary convenience builders give the same bytes.
    void scte35FromSupervisorXml() {
        scte35::Splice out; out.eventId = 101; out.outOfNetwork = true; out.durationSec = 30.0; out.autoReturn = true; out.uniqueProgramId = 1;
        scte35::Splice in; in.eventId = 102; in.outOfNetwork = false; in.uniqueProgramId = 1;
        const struct { QString xml; QString bin; QByteArray direct; } cases[] = {
            {scte35::spliceInsertXml(out), "scte35_out.bin", tskit::scte35::immediateInsert(101, true, 30.0, true, 1)},
            {scte35::spliceInsertXml(in), "scte35_in.bin", tskit::scte35::immediateInsert(102, false, 0.0, true, 1)},
            {scte35::spliceCancelXml(101), "scte35_cancel.bin", tskit::scte35::cancelSection(101)},
            {scte35::spliceNullXml(), "scte35_null.bin", tskit::scte35::nullSection()},
        };
        for (const auto& c : cases) {
            const QByteArray want = readAll(golden(c.bin));
            const tskit::CompileResult r = tskit::compileXml(c.xml.toUtf8());
            QVERIFY2(r.ok, qPrintable(r.error));
            QString d = firstDiff(r.bin(), want); QVERIFY2(d.isEmpty(), qPrintable(c.bin + " (xml): " + d));
            d = firstDiff(c.direct, want); QVERIFY2(d.isEmpty(), qPrintable(c.bin + " (direct): " + d));
        }
    }

    void scte35ParseRoundTrip() {
        QList<QByteArray> secs;
        for (const char* n : {"scte35_out.bin", "scte35_in.bin", "scte35_cancel.bin", "scte35_null.bin", "scte35_misc.bin"}) secs += sectionsOf(n);
        QCOMPARE(secs.size(), 9);
        for (const QByteArray& s : secs) {
            tskit::scte35::SpliceInfo info; QString e;
            QVERIFY2(tskit::scte35::parseSection(s, &info, &e), qPrintable(e));
            QCOMPARE(tskit::scte35::buildSection(info).toHex(), s.toHex());
        }
        tskit::scte35::SpliceInfo o; QVERIFY(tskit::scte35::parseSection(secs[0], &o));
        QCOMPARE(o.command, int(tskit::scte35::SpliceInsert));
        QVERIFY(o.insert.outOfNetwork && o.insert.immediate && o.insert.hasDuration && o.insert.autoReturn);
        QCOMPARE(o.insert.duration, quint64(2700000)); QCOMPARE(o.insert.eventId, 101u);
        tskit::scte35::SpliceInfo m; QVERIFY(tskit::scte35::parseSection(secs[4], &m));   // first of scte35_misc
        QCOMPARE(m.ptsAdjustment, 0x1FFFFFFFFULL); QCOMPARE(m.tier, 0x123);
        QVERIFY(m.insert.time.specified); QCOMPARE(m.insert.time.pts, quint64(900000));
        QByteArray broken = secs[0]; broken[12] = char(broken[12] ^ 0x40);
        QVERIFY(!tskit::scte35::parseSection(broken, &m));                // CRC
        // tstabcomp's rule: a program splice that is neither immediate nor timed is refused.
        const tskit::CompileResult r = tskit::compileXml("<tsduck><splice_information_table><splice_insert splice_event_id='9' out_of_network='true' unique_program_id='3'/></splice_information_table></tsduck>");
        QVERIFY(!r.ok && !r.unsupported); QVERIFY2(r.error.contains("pts_time"), qPrintable(r.error));
    }

    // tsp -P pmt --add-pid <pid>/0x86 --add-registration 0x43554549, on the binary PMT.
    void pmtScte35Rewrite() {
        const QByteArray pmt = readAll(golden("pmt.bin")), want = readAll(golden("pmt_scte35.bin"));
        QString e;
        const QByteArray got = tskit::pmtWithScte35(pmt, 0x0400, &e);
        QVERIFY2(!got.isEmpty(), qPrintable(e));
        QString d = firstDiff(got, want); QVERIFY2(d.isEmpty(), qPrintable(d));
        d = firstDiff(tskit::pmtWithScte35(want, 0x0400), want); QVERIFY2(d.isEmpty(), qPrintable("not idempotent: " + d));
        // A PID already in the PMT keeps its place and descriptors; only stream_type changes.
        tskit::Pmt t; QVERIFY(tskit::parsePmt(tskit::pmtWithScte35(pmt, 0x0034), &t, &e));
        QCOMPARE(t.streams.size(), 2);
        QCOMPARE(t.streams[1].pid, 0x0034); QCOMPARE(t.streams[1].streamType, 0x86); QCOMPARE(t.streams[1].descriptors.size(), 1);
        QCOMPARE(t.descriptors.size(), 2); QCOMPARE(t.descriptors[1].payload, QByteArray("CUEI"));
        QCOMPARE(t.version, 3); QCOMPARE(t.pcrPid, 0x0031);
        QVERIFY(tskit::pmtWithScte35(readAll(golden("pat.bin")), 0x0400, &e).isEmpty());   // not a PMT
        QVERIFY(e.contains("PMT"));
    }

    // Packetization byte-identical with tspacketize (packed, and --stuffing), then the rules on their own.
    void packetizerMatchesTspacketize() {
        QList<QByteArray> psip = sectionsOf("mgt.bin") + sectionsOf("tvct.bin") + sectionsOf("stt.bin") + sectionsOf("rrt.bin");
        tskit::Packetizer p(0x1FFB);
        const QByteArray ts = p.packetize(psip);
        QString d = firstDiff(ts, readAll(golden("psip_1ffb.ts"))); QVERIFY2(d.isEmpty(), qPrintable(d));
        QCOMPARE(p.continuity(), 8);
        tskit::Packetizer s(0x0201);
        d = firstDiff(s.packetize(sectionsOf("ett0.bin"), tskit::Packetizer::StuffEachSection), readAll(golden("ett0_stuffed.ts")));
        QVERIFY2(d.isEmpty(), qPrintable(d));
        QString e; QCOMPARE(depacketize(ts, 0x1FFB, &e), psip); QVERIFY2(e.isEmpty(), qPrintable(e));
    }

    void packetizerCcPusiAndHeaders() {
        // A first section of every size from 150 to 360 bytes, then a short (3-byte header) or long (8-byte header) one: the
        // second starts in the same packet only when its whole header fits behind the first (and its pointer_field).
        for (int size = 150; size <= 360; ++size) {
            for (bool longNext : {false, true}) {
                const QByteArray s1 = tskit::shortSection(0xFC, false, QByteArray(size - 3, '\x11'));
                tskit::LongHeader h; h.tableId = 0xD0; h.tableIdExt = 1;
                const QByteArray s2 = longNext ? tskit::longSection(h, 0, 0, QByteArray(4, '\x22')) : tskit::shortSection(0xFD, false, QByteArray(4, '\x22'));
                const int hdr = longNext ? 8 : 3;
                tskit::Packetizer p(0x100, 15);
                const QByteArray ts = p.packetize({s1, s2});
                QString e;
                QCOMPARE(depacketize(ts, 0x100, &e), (QList<QByteArray>{s1, s2}));
                QVERIFY2(e.isEmpty(), qPrintable(QString("size %1: %2").arg(size).arg(e)));
                QCOMPARE(tskit::packetCc(ts.constData()), 15);                                  // CC wraps 15 -> 0
                if (ts.size() > 188) QCOMPARE(tskit::packetCc(ts.constData() + 188), 0);
                int startPkt, offset;                                                           // where s2 must begin
                if (size <= 183) { startPkt = 183 - size >= hdr ? 0 : 1; offset = startPkt == 0 ? 5 + size : 5; }
                else { const int x = size - 183; startPkt = 183 - x >= hdr ? 1 : 2; offset = startPkt == 1 ? 5 + x : 5; }
                const char* pk = ts.constData() + 188 * startPkt;
                QVERIFY2(ts.size() > 188 * startPkt && tskit::packetPusi(pk), qPrintable(QString("size %1 long %2").arg(size).arg(longNext)));
                QCOMPARE(quint8(pk[offset]), quint8(s2[0]));
            }
        }
        // CC continues across calls; one call ends with a stuffed packet.
        tskit::Packetizer p(0x30, 14);
        const QByteArray a = p.packetize({tskit::shortSection(0x70, false, QByteArray(300, '\0'))});
        QCOMPARE(a.size(), 2 * 188); QCOMPARE(p.continuity(), 0);
        const QByteArray b = p.packetize({tskit::shortSection(0x70, false, QByteArray(5, '\0'))});
        QCOMPARE(tskit::packetCc(b.constData()), 0); QCOMPARE(p.continuity(), 1);
        QCOMPARE(tskit::packetPid(b.constData()), 0x30);
    }

    // Mode selection for multiple_string_structure, as recorded from tstabcomp (ETT text -> MSS bytes).
    void multiStringModes_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QByteArray>("mss");
        auto row = [](const char* name, const QString& t, const char* hex) { QTest::newRow(name) << t << QByteArray::fromHex(hex); };
        row("latin-1", QString::fromUtf8("a\xC3\xA9"), "01656e670100000261e9");
        row("U+00FF", QString(QChar(0xFF)), "01656e6701000001ff");
        row("U+0100 page 0x01", QString(QChar(0x100)), "01656e670100010100");
        row("U+0500 pair", QString(QChar(0x500)) + QChar(0x501), "01656e67010005020001");
        row("U+0600", QString(QChar(0x600)), "01656e670100060100");
        row("U+0700 invalid mode", QString(QChar(0x700)), "01656e6701003f020700");
        row("U+0900", QString(QChar(0x900)), "01656e670100090100");
        row("U+1100 invalid", QString(QChar(0x1100)), "01656e6701003f021100");
        row("U+20AC euro", QString(QChar(0x20AC)), "01656e6701002001ac");
        row("U+2800 invalid", QString(QChar(0x2800)), "01656e6701003f022800");
        row("U+3000 pair", QString(QChar(0x3000)) + QChar(0x3001), "01656e67010030020001");
        row("U+3F00 not the UTF-16 mode", QString(QChar(0x3F00)), "01656e6701003f023f00");
        row("mixed pages", QString(QChar(0xC9)) + QChar(0x20AC), "01656e6701003f0400c920ac");
        row("empty", QString(), "01656e6700");
    }
    void multiStringModes() {
        QFETCH(QString, text); QFETCH(QByteArray, mss);
        QCOMPARE(tskit::encodeMultiString(tskit::MultiString::one(text)).toHex(), mss.toHex());
        int pos = 0; tskit::MultiString back;
        QVERIFY(tskit::decodeMultiString(mss, &pos, int(mss.size()), &back));
        QCOMPARE(pos, int(mss.size()));
        QCOMPARE(back.strings.value(0).text, text);
        // EIT titles alone write an empty text as one empty segment (tstabcomp does; eit_unicode.bin has one).
        if (text.isEmpty()) QCOMPARE(tskit::encodeMultiString(tskit::MultiString::one(text), tskit::EmptyText::OneSegment).toHex(), QByteArray("01656e6701000000"));
    }

    // tstabcomp corrupts strings over 255 bytes (number_bytes overflows to 0); here they become several segments.
    void longStringsBecomeSegments() {
        const QString emoji = QString::fromUtf8("\xF0\x9F\x8E\xAC");                    // U+1F3AC, a surrogate pair
        const QString cases[] = {QString(600, 'a'), QString::fromUtf8("\xC3\xA9\xE2\x80\x99").repeated(150), QString(QChar(0x2019)) + QString(125, 'x') + emoji + "tail",
                                 QString(255, 'b'), QString(256, 'c')};
        const int segments[] = {3, 3, 2, 1, 2};
        for (int k = 0; k < 5; ++k) {
            const QByteArray m = tskit::encodeMultiString(tskit::MultiString::one(cases[k]));
            int pos = 0; tskit::MultiString back;
            QVERIFY(tskit::decodeMultiString(m, &pos, int(m.size()), &back));
            QCOMPARE(back.strings.value(0).text, cases[k]);
            QCOMPARE(int(quint8(m[4])), segments[k]);
            for (int p = 5, s = 0; s < segments[k]; ++s) {                                  // every segment fits a uint8 length
                const int nb = quint8(m[p + 2]);
                QVERIFY(nb > 0 && nb <= 255);
                if (quint8(m[p + 1]) == 0x3F) { QCOMPARE(nb % 2, 0); const int last = (quint8(m[p + 3 + nb - 2]) << 8) | quint8(m[p + 3 + nb - 1]); QVERIFY(last < 0xD800 || last > 0xDBFF); }
                p += 3 + nb;
            }
        }
        // A 1000-character ETT description (MuxManager's cap) compiles and stays within one 4096-byte section.
        const QString desc = QString::fromUtf8("Caf\xC3\xA9 \xE2\x80\x94 ").repeated(125).left(1000);
        tskit::Ett ett; ett.tableIdExt = 1; ett.etmId = 0x10002; ett.text = tskit::MultiString::one(desc);
        const QList<QByteArray> s = tskit::buildEtt(ett);
        QCOMPARE(s.size(), 1); QVERIFY(s[0].size() < 4096); QVERIFY(tskit::crcValid(s[0]));
    }

    // TSDuck XML reader behaviour on the edges (quirks.xml covers the byte level) and error reporting.
    void xmlReaderRules() {
        auto compile = [](const char* x) { return tskit::compileXml(QByteArray(x)); };
        // CR LF anywhere is LF; a raw newline in an attribute is kept; a numeric reference stays literal.
        tskit::xml::Element root; QString e;
        QVERIFY(tskit::xml::parse("<?xml version='1.0'?>\r\n<!-- c -->\r\n<tsduck><A b='x\r\ny&#233;&amp;' C=\"1\"><![CDATA[01 02]]></A></tsduck>", &root, &e));
        QCOMPARE(root.children.size(), size_t(1));
        QCOMPARE(root.children[0].attribute("B"), QString("x\ny&#233;&"));
        QCOMPARE(root.children[0].text, QString("01 02"));
        QCOMPARE(root.children[0].line, 3);
        // Malformed XML: the line is reported.
        tskit::CompileResult r = compile("<tsduck>\n<PAT version='1' transport_stream_id='1'>\n</PMT>\n</tsduck>");
        QVERIFY(!r.ok); QVERIFY2(r.error.contains("line 3"), qPrintable(r.error));
        r = compile("<tsduck><TVCT transport_stream_id='1'><channel short_name='ABCDEFGH' major_channel_number='1' minor_channel_number='1' modulation_mode='8-VSB' channel_TSID='1' program_number='1' source_id='1'/></TVCT></tsduck>");
        QVERIFY(!r.ok && !r.unsupported); QVERIFY2(r.error.contains("short_name"), qPrintable(r.error));
        r = compile("<tsduck><PAT version='32' transport_stream_id='1'/></tsduck>");
        QVERIFY(!r.ok); QVERIFY2(r.error.contains("version"), qPrintable(r.error));
        r = compile("<tsduck><PAT version='1'/></tsduck>");
        QVERIFY(!r.ok); QVERIFY2(r.error.contains("transport_stream_id"), qPrintable(r.error));
        r = compile("<tsduck><ATSC_EIT source_id='1'><event event_id='16384' start_time='2026-01-01 00:00:00' ETM_location='0' length_in_seconds='1'/></ATSC_EIT></tsduck>");
        QVERIFY(!r.ok); QVERIFY2(r.error.contains("event_id"), qPrintable(r.error));
        r = compile("<tsduck><ATSC_EIT source_id='1'><event event_id='1' start_time='1979-12-31 23:00:00' ETM_location='0' length_in_seconds='1'/></ATSC_EIT></tsduck>");
        QVERIFY(!r.ok); QVERIFY2(r.error.contains("start_time"), qPrintable(r.error));
        // Elements this compiler lacks are flagged unsupported (the caller falls back to tstabcomp), and nothing is returned.
        r = compile("<tsduck><PAT transport_stream_id='1'/><SDT transport_stream_id='1' original_network_id='1'/></tsduck>");
        QVERIFY(!r.ok && r.unsupported && r.tables.isEmpty()); QVERIFY2(r.error.contains("SDT"), qPrintable(r.error));
        r = compile("<tsduck><PMT service_id='1'><AC3_descriptor/></PMT></tsduck>");
        QVERIFY(!r.ok && r.unsupported); QVERIFY2(r.error.contains("AC3_descriptor"), qPrintable(r.error));
        r = compile("<tsduck><splice_information_table><splice_null/><avail_descriptor provider_avail_id='1'/></splice_information_table></tsduck>");
        QVERIFY(!r.ok && r.unsupported);
        r = compile("<root/>");
        QVERIFY(!r.ok && r.error.contains("tsduck"));
    }

    // What MuxManager::writeTables writes today compiles in-process; with TSDuck installed it also equals tstabcomp's output.
    void writeTablesMatchTstabcomp() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MuxManager mux(nullptr, dir.path());
        J a = sq("{'id':'main','serviceId':1,'shortName':'WSTR','longName':'Star News Plus','major':25,'minor':1}");
        J b = sq("{'id':'ch2','serviceId':2,'shortName':'STAR-2','longName':'Star Classic Movies & More','major':25,'minor':2}");
        b["longName"] = QString::fromUtf8("Caf\xC3\xA9 Classics \xE2\x80\x94 \"24/7\" <Movies> & More");
        mux.setServices(QJsonArray{a, b});
        const J tables = mux.tablesJson();
        const QString td = QDir::fromNativeSeparators(tables.value("dir").toString());
        const QStringList files = QDir(td).entryList({"*.xml"}, QDir::Files, QDir::Name);
        QVERIFY2(files.contains("pat.xml") && files.contains("tvct.xml") && files.contains("mgt.xml") && files.contains("stt.xml") && files.contains("eit0.xml") && files.contains("ett0.xml"), qPrintable(files.join(',')));
        QHash<QString, QByteArray> ours;
        for (const QString& f : files) {
            const tskit::CompileResult r = tskit::compileFile(QDir(td).filePath(f), QString());
            QVERIFY2(r.ok, qPrintable(f + ": " + r.error));
            ours.insert(f, r.bin());
        }
        tskit::SectionInfo i;
        QVERIFY(tskit::sectionInfo(ours["pat.xml"], 0, &i)); QCOMPARE(i.tableId, 0x00); QCOMPARE(i.tableIdExt, 0x19);
        QVERIFY(tskit::sectionInfo(ours["tvct.xml"], 0, &i)); QCOMPARE(i.tableId, 0xC8);
        QVERIFY(tskit::sectionInfo(ours["mgt.xml"], 0, &i)); QCOMPARE(i.tableId, 0xC7);
        QVERIFY(tskit::sectionInfo(ours["stt.xml"], 0, &i)); QCOMPARE(i.tableId, 0xCD);
        QCOMPARE(tskit::splitSections(ours["eit0.xml"]).size(), 2);       // one EIT per service
        QCOMPARE(tskit::splitSections(ours["ett0.xml"]).size(), 6);       // 3 hours x 2 services
        const QString comp = "C:/Program Files/TSDuck/bin/tstabcomp.exe";
        if (!QFileInfo::exists(comp)) QSKIP("tstabcomp not installed: in-process compile checked, byte comparison skipped");
        for (const QString& f : files) {
            const QString bin = dir.filePath(f + ".bin");
            QProcess p; p.start(comp, {"--atsc", "--output", bin, QDir(td).filePath(f)});
            QVERIFY2(p.waitForFinished(20000) && p.exitCode() == 0, qPrintable(f + ": " + p.readAllStandardError()));
            const QString d = firstDiff(ours[f], readAll(bin));
            QVERIFY2(d.isEmpty(), qPrintable(f + ": " + d));
        }
    }

    // ------------------------------------------------------------------ DEP-02b: capabilities in-process
    // The "ffmpeg" object comes from PlayPlayAvKit: same keys, and has{} / build{} / video[] / audio[] equal to what the old
    // `ffmpeg -encoders` / `-version` parse gives for the ffmpeg.exe of the same build; a refresh answers in well under 100 ms.
    void capabilitiesFfmpegInProcess() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        MuxManager mux(nullptr, dir.path());
        QElapsedTimer t; t.start();
        const J caps = mux.capabilities(true);
        const qint64 first = t.elapsed(); qint64 best = first;
        for (int i = 0; i < 3; ++i) { t.restart(); mux.capabilities(true); best = qMin(best, t.elapsed()); }
        qInfo().noquote() << QString("capabilities(refresh=1): first %1 ms, best of 3 more %2 ms").arg(first).arg(best);
        QVERIFY2(best < 100, qPrintable(QString("%1 ms").arg(best)));
        const J ff = caps.value("ffmpeg").toObject();
        QCOMPARE(ff.keys(), QStringList({"audio", "build", "found", "has", "path", "version", "video"}));
        QCOMPARE(ff.value("path").toString(), QString("(in-process libav)"));
        QVERIFY(ff.value("found").toBool());
        QVERIFY2(ff.value("version").toString().startsWith("ffmpeg version "), qPrintable(ff.value("version").toString()));
        const J has = ff.value("has").toObject();
        for (const QString& n : kWatched) QVERIFY2(has.value(n).isBool(), qPrintable(n));                 // always reported
        for (const char* n : {"libx264", "mpeg2video", "ac3", "aac", "mp2"}) QVERIFY2(has.value(n).toBool(), n);
        QVERIFY(ff.value("build").toObject().value("srt").toBool());                                      // the FFmpeg 9 build has libsrt
        // nothing spawned for ffmpeg / tsp: the only process capabilities() may own is the background DekTec listing
        for (QProcess* p : mux.findChildren<QProcess*>()) QCOMPARE(p->objectName(), QString("dektec-probe"));
        // codec decisions read this has{}: an mpeg2 input service keeps mpeg2video (MX-0's fallback bug stays fixed)
        mux.setServices(QJsonArray{inputService("svc1", sq("{'applyPreset':'atsc_mpeg2_ac3_hd'}"))});
        QString err; const QStringList a = mux.ffmpegArgs(mux.services()[0].toObject(), &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(outputValue(a, "-c:v"), QString("mpeg2video"));
        const J dr = mux.dryRun();                                                                         // uses the same cache
        QCOMPARE(dr.value("encoders").toArray()[0].toObject().value("error").toString(), QString());
        const QString exe = QStringLiteral(PP_FFMPEG_BIN) + "/ffmpeg.exe";
        if (!QFileInfo::exists(exe)) QSKIP("ffmpeg.exe of the linked FFmpeg build not found: no comparison with the old spawn");
        t.restart(); const J old = legacyFfmpegCaps(exe); const qint64 spawnMs = t.elapsed();
        qInfo().noquote() << QString("old path (two ffmpeg.exe spawns): %1 ms; %2 video / %3 audio encoders").arg(spawnMs).arg(old.value("video").toArray().size()).arg(old.value("audio").toArray().size());
        QVERIFY(old.value("found").toBool());
        for (const char* k : {"has", "build", "video", "audio", "found"}) { const QString d = jsonDiff(ff.value(k), old.value(k), k); QVERIFY2(d.isEmpty(), qPrintable(d)); }
        // the version line names the same build: "ffmpeg version <n9.0.2-...> Copyright ..." vs "... <n9.0.2-...> (in-process libav)"
        QCOMPARE(ff.value("version").toString().section(' ', 2, 2), old.value("version").toString().section(' ', 2, 2));
    }

    // No TSDuck (tsduckDir empty, no TSDuck on PATH): found:false everywhere, no process started, nothing waits.
    void capabilitiesWithoutTsduck() {
        QTemporaryDir dir, empty; QVERIFY(dir.isValid() && empty.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        MuxManager mux(nullptr, dir.path());
        mux.setConfig(J{{"tsduckDir", empty.path()}, {"tools", "tsduck"}});        // TSDuck tools only (PlayPlayMux sits next to this test)
        QElapsedTimer t; t.start();
        const J caps = mux.capabilities(true);
        qint64 best = t.elapsed();
        for (int i = 0; i < 2; ++i) { t.restart(); mux.capabilities(true); best = qMin(best, t.elapsed()); }
        qInfo().noquote() << QString("capabilities(refresh=1) without TSDuck: best of 3 %1 ms (PATH has %2 entries)").arg(best).arg(qEnvironmentVariable("PATH").split(';', Qt::SkipEmptyParts).size());
        QVERIFY2(best < 100, qPrintable(QString("%1 ms").arg(best)));
        const J td = caps.value("tsduck").toObject();
        QCOMPARE(td.value("found").toBool(), false);
        QCOMPARE(td.value("version").toString(), QString());
        QCOMPARE(td.value("dir").toString(), empty.path());
        const J tools = td.value("tools").toObject();
        QCOMPARE(tools.keys(), QStringList({"tsanalyze", "tsdektec", "tsp", "tspacketize", "tstabcomp"}));
        for (const QString& k : tools.keys()) QVERIFY2(!tools.value(k).toObject().value("found").toBool(), qPrintable(k));
        const J dk = caps.value("dektec").toObject();
        QCOMPARE(dk.value("devices").toArray().size(), 0);
        QCOMPARE(dk.value("found").toBool(true), false);
        QVERIFY(!dk.contains("probing"));
        QVERIFY(mux.findChildren<QProcess*>().isEmpty());
        QVERIFY(caps.value("ffmpeg").toObject().value("found").toBool());                                // libav needs no TSDuck
        QCOMPARE(mux.dektecDevices(false).size(), 0);                                                    // and the blocking route gives up at once
        QCOMPARE(mux.dektecDevices(true).size(), 0);
        QVERIFY(mux.findChildren<QProcess*>().isEmpty());
        bool warned = false; for (const QJsonValue& w : mux.dryRun().value("warnings").toArray()) warned |= w.toString().startsWith("tsp.exe not found");
        QVERIFY(warned);
    }

    // TSDuck installed: tsp's version from its version resource equals the `tsp --version` text the old code parsed, and the
    // DekTec listing runs in the background (old result kept while re-probing), its result equal to a blocking `tsdektec -a -v`.
    void capabilitiesTsduckVersionAndDektecInBackground() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QTest::failOnWarning(QRegularExpression("MergedChannels"));               // the merged probe never reads stderr (Supervisor log noise)
        MuxManager mux(nullptr, dir.path());
        mux.setConfig(J{{"tools", "tsduck"}});                                      // the tsdektec listing (DEP-15 covers PlayPlayMux's)
        QElapsedTimer t; t.start();
        const J caps = mux.capabilities(true);
        const qint64 ms = t.elapsed();
        const J td = caps.value("tsduck").toObject(), tools = td.value("tools").toObject();
        if (!td.value("found").toBool()) QSKIP("TSDuck is not installed");
        QVERIFY2(ms < 250, qPrintable(QString("%1 ms").arg(ms)));                                        // first call (AvKit's list is cached by now)
        QProcess p; p.setProcessChannelMode(QProcess::MergedChannels); p.start(tools.value("tsp").toObject().value("path").toString(), {"--version"});
        QVERIFY(p.waitForFinished(10000));
        const QString v = QString::fromUtf8(p.readAll()).trimmed();
        QCOMPARE(td.value("version").toString(), v.section("version", 1).trimmed().isEmpty() ? v : v.section("version", 1).trimmed());
        if (!tools.value("tsdektec").toObject().value("found").toBool()) QSKIP("tsdektec.exe not installed");
        const QString tsdektec = tools.value("tsdektec").toObject().value("path").toString();
        J dk = caps.value("dektec").toObject();
        QVERIFY(dk.value("probing").toBool());                                                           // first probe: placeholder
        QCOMPARE(dk.value("devices").toArray().size(), 0);
        QVERIFY(dk.value("raw").toString().contains("background"));
        QVERIFY(mux.findChild<QProcess*>("dektec-probe"));
        QTRY_VERIFY_WITH_TIMEOUT(!mux.capabilities().value("dektec").toObject().value("probing").toBool(), 20000);
        dk = mux.capabilities().value("dektec").toObject();
        QCOMPARE(dk.value("tsdektec").toString(), tsdektec);
        QCOMPARE(dk.value("exitCode").toInt(-1), 0);
        QVERIFY(dk.value("probedMs").toDouble() > 0);
        QProcess q; q.setProcessChannelMode(QProcess::MergedChannels); q.start(tsdektec, {"-a", "-v"}); QVERIFY(q.waitForFinished(15000));
        const J want = MuxManager::parseTsdektec(QString::fromUtf8(q.readAll()));
        for (const char* k : {"devices", "dtapi", "dta"}) { const QString d = jsonDiff(dk.value(k), want.value(k), k); QVERIFY2(d.isEmpty(), qPrintable(d)); }
        qInfo().noquote() << QString("tsdektec in the background: %1 device(s), DTAPI %2").arg(dk.value("devices").toArray().size()).arg(dk.value("dtapi").toString());
        // a refresh keeps showing that list while the next probe runs, and still answers at once
        t.restart(); const J again = mux.capabilities(true).value("dektec").toObject(); const qint64 ms2 = t.elapsed();
        QVERIFY2(ms2 < 100, qPrintable(QString("%1 ms").arg(ms2)));
        QVERIFY(again.value("probing").toBool());
        QCOMPARE(compact(again.value("devices")), compact(dk.value("devices")));
        // the blocking route reuses a fresh background result instead of spawning
        t.restart(); const QJsonArray cached = mux.dektecDevices(false); QVERIFY2(t.elapsed() < 100, qPrintable(QString("%1 ms").arg(t.elapsed())));
        QCOMPARE(compact(cached), compact(dk.value("devices")));
        QTRY_VERIFY_WITH_TIMEOUT(!mux.capabilities().value("dektec").toObject().value("probing").toBool(), 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!mux.findChild<QProcess*>("dektec-probe"), 5000);                        // deleteLater'd
    }

    // The listing parser shared by the blocking route and the background probe, on tsdektec 3.41's output for this box's card.
    void parsesTsdektec() {
        const J dk = MuxManager::parseTsdektec(QString::fromUtf8(readAll(golden("dektec/tsdektec_av.txt"))));
        QCOMPARE(dk.value("dtapi").toString(), QString("6.9.0.237"));
        QCOMPARE(dk.value("dta").toString(), QString("4.28.14.293"));
        QVERIFY(!dk.contains("raw"));
        const QJsonArray devs = dk.value("devices").toArray();
        QCOMPARE(devs.size(), 1);
        const J d = devs[0].toObject();
        QCOMPARE(d.value("index").toInt(-1), 0);
        QCOMPARE(d.value("model").toString(), QString("DTA-2154"));
        QCOMPARE(d.value("description").toString(), QString("Quad HD-SDI/ASI in+out"));
        QCOMPARE(d.value("ports").toInt(), 8);
        QCOMPARE(d.value("inputs").toInt(), 3);
        QCOMPARE(d.value("outputs").toInt(), 1);
        QCOMPARE(d.value("serial").toString(), QString("2154000576"));                                  // not the hex "Serial number" line
        QCOMPARE(d.value("firmware").toInt(), 10);
        const QJsonArray ch = d.value("channels").toArray();
        QCOMPARE(ch.size(), 4);
        QCOMPARE(compact(ch[0]), QString("{\"index\":0,\"kind\":\"input\",\"label\":\"top\",\"port\":1,\"standards\":\"ASI, SDI\"}"));
        QCOMPARE(compact(ch[3]), QString("{\"index\":0,\"kind\":\"output\",\"port\":2,\"standards\":\"ASI, SDI\"}"));
        const J none = MuxManager::parseTsdektec("\r\nDTAPI: 6.9.0.237, Dta: 4.28.14.293, DtaNw: -1.-1.-1.-1, Service: 5.2.8.141\r\n\r\n");
        QCOMPARE(none.value("devices").toArray().size(), 0);
        QCOMPARE(none.value("dtapi").toString(), QString("6.9.0.237"));
        QVERIFY(none.value("raw").toString().contains("DTAPI: 6.9.0.237"));
        QCOMPARE(MuxManager::parseTsdektec(QString()).value("devices").toArray().size(), 0);
    }

    // ------------------------------------------------------------------ DEP-15: in-process tables, asynchronous tools
    // eitDays 16 with the RRT: 261 files compiled in this process (no child), well under a second, every .bin the bytes
    // PlayPlayTsKit makes from that XML and, with TSDuck installed, tstabcomp's bytes for a sample of them.
    void compileTablesInProcess() {
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(4));
        m.mux->setTableOverrides(J{{"eitDays", 16}, {"rrt", J{{"enabled", true}}}});
        QCOMPARE(m.mux->tablesEngine(), QString("native"));                                       // the default
        QElapsedTimer t; t.start();
        const J rep = m.mux->compileReport();
        const qint64 ms = t.elapsed();
        QVERIFY2(rep.value("compiled").toBool(), qPrintable(rep.value("error").toString()));
        QCOMPARE(rep.value("engine").toString(), QString("native"));
        const QJsonArray files = rep.value("files").toArray();
        QCOMPARE(files.size(), 4 + 2 * 128 + 1);
        double compileMs = 0;
        for (const QJsonValue& v : files) {
            const J f = v.toObject();
            QVERIFY2(f.value("ok").toBool() && f.value("exitCode").toInt(-1) == 0 && f.value("engine").toString() == "native", qPrintable(compact(f)));
            QVERIFY2(f.value("sections").toInt() >= 1, qPrintable(compact(f)));
            compileMs += f.value("ms").toDouble();
        }
        QVERIFY(m.mux->findChildren<QProcess*>().isEmpty());
        qInfo().noquote() << QString("compileReport, eitDays 16 (%1 files): %2 ms total (write %3 ms, compile %4 ms)").arg(files.size()).arg(ms).arg(rep.value("writeMs").toDouble()).arg(compileMs, 0, 'f', 1);
        QVERIFY2(ms < 1000, qPrintable(QString("%1 ms").arg(ms)));
        const QString td = QDir::fromNativeSeparators(rep.value("dir").toString()), cd = QDir::fromNativeSeparators(rep.value("compiledDir").toString());
        QCOMPARE(QDir(td).entryList({"*.xml"}, QDir::Files).size(), 261);                            // replaced in place, no temp left
        QCOMPARE(QDir(td).entryList(QDir::Files).size(), 261);
        QCOMPARE(QDir(cd).entryList({"*.bin"}, QDir::Files).size(), 261);
        QCOMPARE(QDir(cd).entryList(QDir::Files).size(), 261);
        for (const QString& f : {"pat", "tvct", "mgt", "stt", "rrt", "eit0", "ett0", "eit127", "ett127"}) {
            const tskit::CompileResult r = tskit::compileFile(td + "/" + f + ".xml", QString());
            QVERIFY2(r.ok, qPrintable(r.error));
            const QString d = firstDiff(readAll(cd + "/" + f + ".bin"), r.bin()); QVERIFY2(d.isEmpty(), qPrintable(f + ": " + d));
        }
        // the same tables again within the same minute (same version): files holding the same bytes are not rewritten
        const int version = m.mux->tablesJson().value("psip").toObject().value("version").toInt(-1);
        const QDateTime patXml = QFileInfo(td + "/pat.xml").lastModified(), patBin = QFileInfo(cd + "/pat.bin").lastModified();
        t.restart();
        const J val = m.mux->validateTables();
        const qint64 again = t.elapsed();
        QVERIFY(val.value("compiled").toBool()); QCOMPARE(val.value("engine").toString(), QString("native"));
        if (m.mux->tablesJson().value("psip").toObject().value("version").toInt(-2) == version) {
            QCOMPARE(QFileInfo(td + "/pat.xml").lastModified(), patXml); QCOMPARE(QFileInfo(cd + "/pat.bin").lastModified(), patBin);
            qInfo().noquote() << QString("validate again within the minute: %1 ms").arg(again);
        }
        if (!QFileInfo::exists(kTstabcomp)) QSKIP("tstabcomp not installed: no byte comparison with it");
        for (const QString& f : {"pat", "tvct", "mgt", "stt", "rrt", "eit0", "ett0", "eit127", "ett127"}) {
            const QString bin = m.dir.filePath(f + ".tstabcomp.bin");
            QProcess p; p.start(kTstabcomp, {"--atsc", "--output", bin, td + "/" + f + ".xml"});
            QVERIFY2(p.waitForFinished(20000) && p.exitCode() == 0, qPrintable(f + ": " + p.readAllStandardError()));
            const QString d = firstDiff(readAll(cd + "/" + f + ".bin"), readAll(bin)); QVERIFY2(d.isEmpty(), qPrintable(f + ": " + d));
        }
    }

    // XML outside the subset (an SDT in customXml) and broken XML: without tstabcomp the file fails (64 / 2) and so does the start;
    // with tstabcomp its verdict decides (fallback / nativeError recorded); tables = tstabcomp without it is the old error.
    void compileTablesFallbacks() {
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(1));
        QTemporaryDir empty; QVERIFY(empty.isValid());
        {
            EnvScope path("PATH", pathWithoutTsduck());
            m.mux->setConfig(J{{"tables", "native"}, {"tsduckDir", empty.path()}});
            m.mux->setTableOverrides(J{{"customXml", sdtXml()}});
            J rep = m.mux->compileReport();
            QVERIFY(!rep.value("compiled").toBool());
            J c = fileEntry(rep, "custom.xml");
            QCOMPARE(c.value("exitCode").toInt(), 64); QCOMPARE(c.value("engine").toString(), QString("native"));
            QVERIFY2(c.value("output").toString().contains("subset"), qPrintable(compact(c)));
            QVERIFY2(rep.value("error").toString().startsWith("custom.xml: ") && !rep.value("error").toString().startsWith("custom.xml: custom.xml"), qPrintable(rep.value("error").toString()));
            QVERIFY(fileEntry(rep, "tvct.xml").value("ok").toBool());
            QString e; QVERIFY(!m.mux->start(&e));                                                    // a start stops at the tables, as before
            QVERIFY2(e.contains("custom.xml"), qPrintable(e)); QCOMPARE(m.mux->status().value("state").toString(), QString("error"));
            m.mux->setTableOverrides(J{{"customXml", kTruncatedXml}});
            rep = m.mux->compileReport(); c = fileEntry(rep, "custom.xml");
            QCOMPARE(c.value("exitCode").toInt(), 2); QVERIFY(!c.value("ok").toBool());
            QVERIFY2(c.value("output").toString().contains("line"), qPrintable(compact(c)));
            m.mux->setConfig(J{{"tables", "tstabcomp"}});
            rep = m.mux->compileReport();
            QVERIFY(!rep.value("compiled").toBool()); QVERIFY2(rep.value("error").toString().contains("tstabcomp.exe not found"), qPrintable(rep.value("error").toString()));
            QVERIFY(m.mux->findChildren<QProcess*>().isEmpty());
        }
        if (!QFileInfo::exists(kTstabcomp)) QSKIP("tstabcomp not installed: the fallbacks to it are not checked");
        m.mux->setConfig(J{{"tables", "native"}, {"tsduckDir", QFileInfo(kTstabcomp).path()}});
        m.mux->setTableOverrides(J{{"customXml", sdtXml()}});
        J rep = m.mux->compileReport();
        QVERIFY2(rep.value("compiled").toBool(), qPrintable(rep.value("error").toString()));
        J c = fileEntry(rep, "custom.xml");
        QCOMPARE(c.value("engine").toString(), QString("tstabcomp")); QVERIFY(c.value("fallback").toString().contains("SDT")); QVERIFY(c.value("ok").toBool());
        QCOMPARE(readAll(QDir::fromNativeSeparators(rep.value("compiledDir").toString()) + "/custom.bin").size(), 35);
        QCOMPARE(fileEntry(rep, "tvct.xml").value("engine").toString(), QString("native"));
        m.mux->setTableOverrides(J{{"customXml", kTruncatedXml}});
        rep = m.mux->compileReport(); c = fileEntry(rep, "custom.xml");
        QVERIFY(!rep.value("compiled").toBool());
        QCOMPARE(c.value("engine").toString(), QString("tstabcomp")); QVERIFY(!c.value("nativeError").toString().isEmpty());
        QVERIFY2(c.value("output").toString().contains("parsing error"), qPrintable(compact(c)));     // tstabcomp's own words
        m.mux->setTableOverrides(J{{"customXml", ""}});
        m.mux->setConfig(J{{"tables", "tstabcomp"}});
        rep = m.mux->compileReport();
        QVERIFY2(rep.value("compiled").toBool(), qPrintable(rep.value("error").toString()));
        for (const QJsonValue& v : rep.value("files").toArray()) QCOMPARE(v.toObject().value("engine").toString(), QString("tstabcomp"));
    }

    // Operator XML (TVCT extraXml, customXml): PlayPlayTsKit ignores unknown attributes, TSDuck (tstabcomp and tsp's inject) rejects
    // them. With tstabcomp installed its verdict decides for exactly those files, so a mistyped attribute still fails Validate.
    void operatorXmlCheckedByTstabcomp() {
        if (!QFileInfo::exists(kTstabcomp)) QSKIP("tstabcomp not installed: nothing to compare with");
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(2));
        m.mux->setConfig(J{{"tables", "native"}, {"tsduckDir", QFileInfo(kTstabcomp).path()}});
        const QString badExtra = "<extended_channel_name_descriptor lang=\"eng\"><long_channel_name_text><string language=\"eng\" text=\"Extra\"/></long_channel_name_text></extended_channel_name_descriptor>";
        m.mux->setTableOverrides(J{{"services", J{{"svc1", J{{"extraXml", badExtra}}}}}});
        J val = m.mux->validateTables();
        QVERIFY2(!val.value("compiled").toBool(), qPrintable(compact(val)));
        QVERIFY2(val.value("error").toString().contains("unexpected attribute"), qPrintable(val.value("error").toString()));
        const tskit::CompileResult nat = tskit::compileFile(QDir::fromNativeSeparators(val.value("dir").toString()) + "/tvct.xml", QString());
        QVERIFY2(nat.ok, "the in-house compiler is expected to accept it (that is the gap this guards)");
        J rep = m.mux->compileReport(); J t = fileEntry(rep, "tvct.xml");
        QCOMPARE(t.value("engine").toString(), QString("tstabcomp")); QVERIFY(t.value("operatorXml").toBool()); QVERIFY(!t.value("ok").toBool());
        QCOMPARE(fileEntry(rep, "pat.xml").value("engine").toString(), QString("native"));   // generated files stay in-process
        QVERIFY(logHas(*m.mux, "tstabcomp rejects it"));
        // a valid extraXml passes through tstabcomp; a customXml PAT with a bogus attribute fails like tvct did
        m.mux->setTableOverrides(J{{"services", J{{"svc1", J{{"extraXml", QString(badExtra).remove(" lang=\"eng\"")}}}}}});
        rep = m.mux->compileReport(); t = fileEntry(rep, "tvct.xml");
        QVERIFY2(rep.value("compiled").toBool(), qPrintable(rep.value("error").toString()));
        QCOMPARE(t.value("engine").toString(), QString("tstabcomp")); QVERIFY(t.value("ok").toBool());
        m.mux->setTableOverrides(J{{"services", J{{"svc1", J{{"extraXml", ""}}}}}, {"customXml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <PAT version=\"0\" current=\"true\" transport_stream_id=\"0x0019\" bogus_attr=\"1\">\n    <service service_id=\"0x0001\" program_map_PID=\"0x0021\"/>\n  </PAT>\n</tsduck>\n"}});
        rep = m.mux->compileReport();
        QVERIFY(!rep.value("compiled").toBool());
        QVERIFY2(rep.value("error").toString().startsWith("custom.xml: ") && rep.value("error").toString().contains("bogus_attr"), qPrintable(rep.value("error").toString()));
        QCOMPARE(fileEntry(rep, "tvct.xml").value("engine").toString(), QString("native"));   // no operator XML left in it
        // tables = native without tstabcomp: nothing to ask, the in-house verdict stands (tsp, also TSDuck, is missing then anyway)
        QTemporaryDir empty; QVERIFY(empty.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        m.mux->setConfig(J{{"tsduckDir", empty.path()}});
        rep = m.mux->compileReport();
        QCOMPARE(fileEntry(rep, "custom.xml").value("engine").toString(), QString("native")); QVERIFY(fileEntry(rep, "custom.xml").value("ok").toBool());
    }

    // The Supervisor now runs the compiler in its own process: mangled XML (byte flips, cuts, stray markup) is an error with a
    // message, never a crash, and a failed compile returns nothing.
    void compilerSurvivesMangledXml() {
        QRandomGenerator rng(15);
        int rejected = 0, accepted = 0;
        for (const char* g : {"tvct.xml", "eit0.xml", "ett0.xml", "mgt.xml", "rrt.xml", "scte35_out.xml", "quirks.xml"}) {
            const QByteArray src = readAll(golden(g)); QVERIFY2(!src.isEmpty(), g);
            for (int i = 0; i < 250; ++i) {
                QByteArray x = src;
                for (int e = 1 + int(rng.bounded(4)); e > 0 && !x.isEmpty(); --e) {
                    const int pos = int(rng.bounded(quint32(x.size())));
                    switch (rng.bounded(5)) {
                    case 0: x[pos] = char(rng.bounded(256)); break;
                    case 1: x.remove(pos, 1 + int(rng.bounded(24))); break;
                    case 2: x.insert(pos, "<\"&>=/"[rng.bounded(6)]); break;
                    case 3: x.insert(pos, x.mid(int(rng.bounded(quint32(x.size()))), 40)); break;
                    default: x.truncate(pos);
                    }
                }
                const tskit::CompileResult r = tskit::compileXml(x);
                if (r.ok) { ++accepted; QVERIFY(r.sectionCount() > 0); }
                else { ++rejected; QVERIFY2(!r.error.isEmpty(), g); QVERIFY(r.bin().isEmpty()); }
            }
        }
        qInfo().noquote() << QString("mangled XML: %1 rejected with a message, %2 still valid").arg(rejected).arg(accepted);
        QVERIFY(rejected > 0);
    }

    // Live analysis of the mux output file: PlayPlayMux analyze as an async child (done runs later, the loop keeps turning),
    // PAT / MGT / TVCT decoded; with TSDuck installed tsp -P tables prints the same text for the same file.
    void analyzeTablesFromMuxFile() {
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(2));
        const QString out = m.dir.filePath("out.ts");
        m.mux->setConfig(J{{"output", J{{"kind", "file"}, {"url", out}}}, {"tools", "native"}, {"playplayMux", muxExe()}});
        const J tables = m.mux->tablesJson();
        QString err; const QByteArray ts = tsFromTables(QDir::fromNativeSeparators(tables.value("dir").toString()), 6, 400, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err)); QVERIFY(writeFile(out, ts));
        auto run = [&](J* res) {
            bool got = false; QElapsedTimer t; t.start();
            m.mux->analyze(QString(), "tables", [&](const J& r) { *res = r; got = true; });
            const qint64 callMs = t.elapsed();
            if (got) return QString("answered inside the call: ") + compact(*res).left(300);
            const qint64 gap = maxLoopGapMs([&]() { return got; }, 30000);       // longer than runTool's 20 s kill timer
            if (!got) return QString("no answer within 30 s");
            qInfo().noquote() << QString("analyze (%1): call %2 ms, longest event-loop gap %3 ms, answer after %4 ms").arg(res->value("engine").toString()).arg(callMs).arg(gap).arg(t.elapsed());
            return callMs < 250 && gap < 250 ? QString() : QString("blocked: call %1 ms, gap %2 ms").arg(callMs).arg(gap);
        };
        J res; QString e = run(&res); QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(res.value("engine").toString(), QString("native"));
        QVERIFY(res.value("file").toBool()); QVERIFY(res.value("psipOutput").toBool());
        QCOMPARE(res.value("source").toString(), QDir::toNativeSeparators(out));
        QCOMPARE(res.value("exitCode").toInt(-1), 0);
        QVERIFY2(res.value("command").toString().contains("PlayPlayMux") && res.value("command").toString().contains("--input file"), qPrintable(res.value("command").toString()));
        const QString text = res.value("output").toString();
        for (const char* h : {"* PAT, TID 0x00 (0), PID 0x0000 (0)", "* MGT (ATSC), TID 0xC7 (199), PID 0x1FFB (8187)", "* TVCT (ATSC), TID 0xC8 (200), PID 0x1FFB (8187)"})
            QVERIFY2(text.contains(h), qPrintable(h + QString("\n") + text.left(800)));
        QVERIFY(text.contains("STAR-1") && text.contains("STAR-2"));
        QVERIFY2(!text.contains("{\""), "the JSON summary line is not part of the text");
        const J sum = res.value("summary").toObject();
        QVERIFY2(sum.value("tablesDisplayed").toInt() >= 4, qPrintable(compact(sum).left(300)));
        QCOMPARE(qint64(sum.value("packets").toDouble()), qint64(ts.size() / 188));
        if (!QFileInfo::exists(kTstabcomp)) QSKIP("TSDuck not installed: no comparison with tsp");
        m.mux->setConfig(J{{"tools", "tsduck"}, {"tsduckDir", QFileInfo(kTstabcomp).path()}});
        J viaTsp; e = run(&viaTsp); QVERIFY2(e.isEmpty(), qPrintable(e));
        QCOMPARE(viaTsp.value("engine").toString(), QString("tsduck"));
        QCOMPARE(viaTsp.value("exitCode").toInt(-1), 0);
        QVERIFY2(viaTsp.value("command").toString().contains("-I file"), qPrintable(viaTsp.value("command").toString()));
        const QStringList a = text.trimmed().split('\n'), b = viaTsp.value("output").toString().remove('\r').trimmed().split('\n');
        for (int i = 0; i < qMin(a.size(), b.size()); ++i) QVERIFY2(a[i] == b[i], qPrintable(QString("line %1\n  PlayPlayMux %2\n  tsp         %3").arg(i + 1).arg(a[i], b[i])));
        QCOMPARE(a.size(), b.size());
    }

    // The same over UDP (output.kind udp on loopback, the test sending the tables): decoded while the event loop keeps running.
    void analyzeUdpOutputAsync() {
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(1));
        m.mux->setConfig(J{{"output", J{{"kind", "udp"}, {"url", "udp://127.0.0.1:50077?pkt_size=1316"}}}, {"tools", "native"}, {"playplayMux", muxExe()}});
        const J tables = m.mux->tablesJson();
        QString err; const QByteArray ts = tsFromTables(QDir::fromNativeSeparators(tables.value("dir").toString()), 1, 20, &err);
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QUdpSocket tx; int sent = 0;
        QTimer feed; feed.setInterval(10);
        QObject::connect(&feed, &QTimer::timeout, [&]() { for (int off = 0; off < ts.size(); off += 7 * 188) tx.writeDatagram(ts.mid(off, 7 * 188), QHostAddress::LocalHost, 50077); ++sent; });
        feed.start();
        bool got = false; J res;
        m.mux->analyze(QString(), "tables", [&](const J& r) { res = r; got = true; });
        QVERIFY(!got);
        const qint64 gap = maxLoopGapMs([&]() { return got; }, 30000);
        feed.stop();
        QVERIFY(got);
        qInfo().noquote() << QString("analyze over UDP: %1 rounds sent, longest event-loop gap %2 ms").arg(sent).arg(gap);
        QVERIFY2(gap < 250, qPrintable(QString("%1 ms").arg(gap)));
        QCOMPARE(res.value("source").toString(), QString("127.0.0.1:50077"));
        QVERIFY2(res.value("command").toString().contains("--input udp"), qPrintable(res.value("command").toString()));
        QCOMPARE(res.value("exitCode").toInt(-1), 0);
        QVERIFY2(res.value("output").toString().contains("* TVCT (ATSC)"), qPrintable(res.value("output").toString().left(600)));
        QVERIFY(res.value("summary").toObject().value("datagrams").toInt() > 0);
    }

    // PlayPlayMux that cannot start (a file that is no program): analyze answers from tsp, or says tsp is missing.
    void analyzeFallsBackToTsp() {
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(1));
        const QString out = m.dir.filePath("out.ts"), bogus = m.dir.filePath("PlayPlayMux.exe");
        QVERIFY(writeFile(bogus, "not a program"));
        const J tables = m.mux->tablesJson();
        QString err; QVERIFY(writeFile(out, tsFromTables(QDir::fromNativeSeparators(tables.value("dir").toString()), 2, 50, &err))); QVERIFY(err.isEmpty());
        QTemporaryDir empty; QVERIFY(empty.isValid());
        J res; bool got = false;
        {
            EnvScope path("PATH", pathWithoutTsduck());
            m.mux->setConfig(J{{"output", J{{"kind", "file"}, {"url", out}}}, {"tools", "native"}, {"playplayMux", bogus}, {"tsduckDir", empty.path()}});
            m.mux->analyze(QString(), "tables", [&](const J& r) { res = r; got = true; });
            QVERIFY(!got);                                                                              // FailedToStart arrives queued
            QTRY_VERIFY_WITH_TIMEOUT(got, 10000);
        }
        QVERIFY2(res.value("fallback").toString().contains("did not start"), qPrintable(compact(res)));
        QVERIFY2(res.value("error").toString().contains("tsp.exe not found"), qPrintable(compact(res)));
        if (!QFileInfo::exists(kTstabcomp)) QSKIP("TSDuck not installed: the tsp fallback is not run");
        m.mux->setConfig(J{{"tsduckDir", QFileInfo(kTstabcomp).path()}});
        got = false; m.mux->analyze(QString(), "tables", [&](const J& r) { res = r; got = true; });
        QTRY_VERIFY_WITH_TIMEOUT(got, 20000);
        QCOMPARE(res.value("engine").toString(), QString("tsduck"));
        QVERIFY(res.value("fallback").toString().contains("did not start"));
        QVERIFY2(res.value("output").toString().contains("* TVCT (ATSC)"), qPrintable(compact(res).left(600)));
    }

    // What MuxManager makes of PlayPlayMux's answers (pure).
    void prepVerdictAndListParsing() {
        J v = MuxManager::prepVerdict(0, false, "Port 2 ...\n{\"ok\":true,\"actions\":[],\"changed\":false}\n", "");
        QVERIFY(v.value("ok").toBool()); QVERIFY(v.value("actions").toArray().isEmpty());
        v = MuxManager::prepVerdict(64, false, "{\"ok\":false,\"unsupported\":true,\"error\":\"DekTec support (DTAPI) is not compiled into this build\"}", "");
        QVERIFY(!v.value("ok").toBool()); QVERIFY(v.value("fallback").toBool()); QVERIFY(v.value("reason").toString().contains("DTAPI"));
        v = MuxManager::prepVerdict(-1, true, "", "");
        QVERIFY(v.value("fallback").toBool()); QCOMPARE(v.value("reason").toString(), QString("PlayPlayMux crashed"));
        v = MuxManager::prepVerdict(1, false, "garbage", "boom");
        QVERIFY(v.value("fallback").toBool()); QVERIFY(v.value("reason").toString().contains("boom"));
        v = MuxManager::prepVerdict(2, false, "text\n{\"ok\":false,\"refused\":true,\"inUse\":true,\"error\":\"port 2 of DTA-2154 #0 is in use\"}", "dektec prep: ...");
        QVERIFY(!v.value("ok").toBool()); QVERIFY(!v.value("fallback").toBool()); QVERIFY(v.value("inUse").toBool());
        QCOMPARE(v.value("error").toString(), QString("port 2 of DTA-2154 #0 is in use"));
        v = MuxManager::prepVerdict(2, false, "{\"ok\":false}", "");
        QVERIFY(!v.value("fallback").toBool()); QCOMPARE(v.value("error").toString(), QString("PlayPlayMux dektec prep failed (exit 2)"));
        J dk = MuxManager::parseDektecList(0, "{\"ok\":true,\"available\":false,\"devices\":[],\"note\":\"DekTec support (DTAPI) is not compiled into this build\"}");
        QVERIFY(dk.value("fallback").toBool()); QVERIFY(dk.value("reason").toString().contains("DTAPI"));
        QVERIFY(MuxManager::parseDektecList(0, "").value("fallback").toBool());
        QVERIFY(MuxManager::parseDektecList(2, "{\"ok\":false,\"error\":\"x\"}").value("fallback").toBool());
        dk = MuxManager::parseDektecList(0, "DTAPI: 6.13.0.242, Dta: 4.28.14.293\n{\"ok\":true,\"available\":true,\"devices\":[],\"dtapi\":\"6.13.0.242\",\"dta\":\"4.28.14.293\",\"scanError\":\"no driver\"}");
        QVERIFY(!dk.contains("fallback")); QCOMPARE(dk.value("devices").toArray().size(), 0);
        QCOMPARE(dk.value("dtapi").toString(), QString("6.13.0.242")); QCOMPARE(dk.value("error").toString(), QString("no driver"));
        QVERIFY(dk.value("raw").toString().contains("DTAPI: 6.13.0.242"));
    }

    // The DekTec listing through PlayPlayMux (simulated DTA-2154): placeholder first, the async callback later, the devices equal
    // to what MuxManager's tsdektec parser reads from the tool's own text; a fresh result answers at once, refresh=1 re-lists.
    void dektecListAsyncViaPlayPlayMux() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString sim = simWithPort2(dir.path(), "card.json", false, "output"); QVERIFY(!sim.isEmpty());
        EnvScope simEnv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(sim).toLocal8Bit());
        QCOMPARE(simGuard(), QString());
        MuxManager mux(nullptr, dir.path());
        mux.setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}});
        J dk = mux.capabilities(true).value("dektec").toObject();
        QVERIFY(dk.value("probing").toBool()); QVERIFY2(dk.value("raw").toString().contains("PlayPlayMux dektec list"), qPrintable(compact(dk)));
        QCOMPARE(mux.capabilities().value("playplayMux").toObject().value("tools").toString(), QString("native"));
        bool got = false; J res; QElapsedTimer t; t.start();
        mux.dektecDevicesAsync(false, [&](const J& d) { res = d; got = true; });                     // joins the running listing
        QVERIFY(!got); QVERIFY2(t.elapsed() < 100, qPrintable(QString("%1 ms").arg(t.elapsed())));
        QTRY_VERIFY_WITH_TIMEOUT(got, 15000);
        QCOMPARE(res.value("engine").toString(), QString("native")); QCOMPARE(res.value("exitCode").toInt(-1), 0); QCOMPARE(res.value("tool").toString(), muxExe());
        QProcess p; p.start(muxExe(), {"dektec", "list", "-v"}); QVERIFY(p.waitForFinished(15000));
        QString text = QString::fromUtf8(p.readAllStandardOutput()).trimmed(); text = text.left(text.lastIndexOf('\n'));
        const J want = MuxManager::parseTsdektec(text);
        QCOMPARE(res.value("dtapi").toString(), want.value("dtapi").toString()); QCOMPARE(res.value("dta").toString(), want.value("dta").toString());
        const QJsonArray gd = res.value("devices").toArray(), wd = want.value("devices").toArray();
        QCOMPARE(gd.size(), 1); QCOMPARE(wd.size(), 1);
        for (const QString& k : wd[0].toObject().keys()) {
            if (k == "channels") { const QJsonArray gc = gd[0].toObject().value(k).toArray(), wc = wd[0].toObject().value(k).toArray(); QCOMPARE(gc.size(), wc.size());
                for (int i = 0; i < wc.size(); ++i) for (const QString& ck : wc[i].toObject().keys()) QVERIFY2(gc[i].toObject().value(ck) == wc[i].toObject().value(ck), qPrintable(QString("channel %1 %2: %3 vs %4").arg(i).arg(ck, compact(gc[i].toObject().value(ck)), compact(wc[i].toObject().value(ck))))); }
            else QVERIFY2(gd[0].toObject().value(k) == wd[0].toObject().value(k), qPrintable(k + ": " + compact(gd[0].toObject().value(k)) + " vs " + compact(wd[0].toObject().value(k))));
        }
        got = false; mux.dektecDevicesAsync(false, [&](const J& d) { res = d; got = true; });
        QVERIFY(got);                                                                                  // fresh: at once
        QCOMPARE(mux.dektecDevices(false).size(), 1);
        QCOMPARE(mux.capabilities().value("dektec").toObject().value("engine").toString(), QString("native"));
        got = false; mux.dektecDevicesAsync(true, [&](const J& d) { res = d; got = true; });
        QVERIFY(!got); QVERIFY(mux.findChild<QProcess*>("dektec-probe"));
        QTRY_VERIFY_WITH_TIMEOUT(got, 15000);
        QCOMPARE(res.value("devices").toArray().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(mux.findChildren<QProcess*>().isEmpty(), 5000);
    }

    // A DekTec mux start: start() returns at once ("starting"); `PlayPlayMux dektec prep` runs as a child and the start completes
    // (or fails) when it answers. Simulated card only; PATH without TSDuck and a scratch tsduckDir whose tsp.exe is a copy of
    // PlayPlayMux, so neither a fallback nor the master can reach the real tsdektec / card.
    void startPreparesDektecPortAsync() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        const QString td = fakeTsduckDir(dir.path()); QVERIFY(!td.isEmpty());
        const QString busy = simWithPort2(dir.path(), "busy.json", true, "output"), free = simWithPort2(dir.path(), "free.json", false, "input");
        QVERIFY(!busy.isEmpty() && !free.isEmpty());
        EnvScope simEnv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(busy).toLocal8Bit());
        QCOMPARE(simGuard(), QString());
        bool done = false, ok = true; QString err; int answers = 0; bool ok1 = false, ok2 = false;   // before the mux: its destructor answers waiters
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(2));
        m.mux->setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}, {"tsduckDir", td}, {"output", J{{"kind", "dektec"}, {"port", 2}, {"device", 0}}}});
        const J dr = m.mux->dryRun();
        QCOMPARE(dr.value("tools").toObject().value("portSetup").toString(), QString("native"));
        QVERIFY2(dr.value("dektecPrep").toString().contains("dektec prep --port 2 --device 0"), qPrintable(dr.value("dektecPrep").toString()));
        // 1. port 2 attached by someone: refused, the start fails with that reason, the card is untouched, no master started
        const QByteArray before = readAll(busy);
        QElapsedTimer t; t.start();
        m.mux->startAsync([&](bool o, const QString& e) { done = true; ok = o; err = e; });
        const qint64 callMs = t.elapsed();
        QVERIFY(!done); QCOMPARE(m.mux->status().value("state").toString(), QString("starting"));
        QVERIFY(m.mux->findChild<QProcess*>("dektec-prep"));
        const qint64 gap = maxLoopGapMs([&]() { return done; }, 15000);
        qInfo().noquote() << QString("mux start with DekTec output: start() %1 ms, port setup answered after %2 ms, longest event-loop gap %3 ms").arg(callMs).arg(t.elapsed()).arg(gap);
        QVERIFY(done); QVERIFY(!ok);
        QVERIFY2(err.contains("in use"), qPrintable(err));
        QVERIFY2(callMs < 1000 && gap < 250, qPrintable(QString("call %1 ms, gap %2 ms").arg(callMs).arg(gap)));
        QCOMPARE(m.mux->status().value("state").toString(), QString("error"));
        QCOMPARE(readAll(busy), before);
        QVERIFY2(logHas(*m.mux, "dektec prep --port 2"), qPrintable(logText(*m.mux)));
        QVERIFY(!logHas(*m.mux, "tsp --timed-log"));
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->findChildren<QProcess*>().isEmpty(), 5000);
        // 2. port free but an input: prep makes it an output (written to the simulated card), then the master starts; a second
        //    waiter joins the start in progress and both hear the same outcome
        qputenv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(free).toLocal8Bit());
        m.mux->startAsync([&](bool o, const QString&) { ++answers; ok1 = o; });
        m.mux->startAsync([&](bool o, const QString&) { ++answers; ok2 = o; });
        QCOMPARE(answers, 0);
        QTRY_COMPARE_WITH_TIMEOUT(answers, 2, 15000);
        QVERIFY2(ok1 && ok2, qPrintable(logText(*m.mux)));
        QVERIFY2(logHas(*m.mux, "dektec: output port set up"), qPrintable(logText(*m.mux)));
        QVERIFY(logHas(*m.mux, "tsp --timed-log"));
        const QJsonArray ports = QJsonDocument::fromJson(readAll(free)).object().value("devices").toArray()[0].toObject().value("ports").toArray();
        QCOMPARE(ports[1].toObject().value("ioDir").toString(), QString("output"));
        m.mux->stop();
        QCOMPARE(m.mux->status().value("state").toString(), QString("stopped"));
        // 3. stopped while the port setup runs: the waiter hears it at once and the late answer starts nothing
        done = false; ok = true;
        m.mux->startAsync([&](bool o, const QString& e) { done = true; ok = o; err = e; });
        QVERIFY(!done);
        m.mux->stop();
        QVERIFY(done); QVERIFY(!ok); QVERIFY2(err.contains("stopped"), qPrintable(err));
        QTest::qWait(1500);
        QCOMPARE(m.mux->status().value("state").toString(), QString("stopped"));
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->findChildren<QProcess*>().isEmpty(), 5000);
        // 4. tools = native but no PlayPlayMux: the port setup is tsdektec's, which is missing here -> a clear failure
        m.mux->setConfig(J{{"playplayMux", dir.filePath("missing/PlayPlayMux.exe")}});
        QCOMPARE(m.mux->dryRun().value("tools").toObject().value("portSetup").toString(), QString("tsduck"));
        done = false; m.mux->startAsync([&](bool o, const QString& e) { done = true; ok = o; err = e; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(!ok); QVERIFY2(err.contains("tsdektec could not configure the output port"), qPrintable(err));
        m.mux->stop();
    }

    // tsp exits (restart armed for 3 s), then the operator presses Start and its port setup is still running when the restart
    // timer fires: the timer must leave that start alone. Before the fix it cleared m_wantRunning, the setup's answer then started
    // nothing and the mux sat in "starting" with the operator's request unanswered. The slow setup is a copy of cmd.exe as
    // tsdektec.exe (it waits on its stdin pipe until runTool's 10 s timeout kills it); the master is the PlayPlayMux copy, which
    // exits at once. Simulated card only.
    void restartTimerLeavesPendingStartAlone() {
        const QString cmd = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows")).filePath("System32/cmd.exe");
        if (!QFileInfo::exists(cmd)) QSKIP("cmd.exe not found");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        const QString td = fakeTsduckDir(dir.path()); QVERIFY(!td.isEmpty());
        QVERIFY(QFile::copy(cmd, td + "/tsdektec.exe"));
        const QString free = simWithPort2(dir.path(), "free.json", false, "output"); QVERIFY(!free.isEmpty());
        EnvScope simEnv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(free).toLocal8Bit());
        QCOMPARE(simGuard(), QString());
        bool done1 = false, ok1 = false, done = false, ok = true; QString err;
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(1));
        m.mux->setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}, {"tsduckDir", td}, {"output", J{{"kind", "dektec"}, {"port", 2}, {"device", 0}}}});
        m.mux->startAsync([&](bool o, const QString&) { done1 = true; ok1 = o; });
        QTRY_VERIFY_WITH_TIMEOUT(done1, 15000); QVERIFY2(ok1, qPrintable(logText(*m.mux)));
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->status().value("error").toString().startsWith("tsp exited"), 5000);   // restart armed now
        QCOMPARE(m.mux->status().value("state").toString(), QString("error"));
        m.mux->setConfig(J{{"tools", "tsduck"}});                                               // the port setup: the slow "tsdektec"
        QElapsedTimer t; t.start();
        m.mux->startAsync([&](bool o, const QString& e) { done = true; ok = o; err = e; });
        QCOMPARE(m.mux->status().value("state").toString(), QString("starting"));
        QTest::qWait(3600);                                                                       // the restart timer fired meanwhile
        QVERIFY(!done); QCOMPARE(m.mux->status().value("state").toString(), QString("starting"));
        QVERIFY(m.mux->findChild<QProcess*>("dektec-prep"));
        QTRY_VERIFY_WITH_TIMEOUT(done, 12000);                                                    // the setup's own verdict answers
        QVERIFY(!ok); QVERIFY2(err.contains("tsdektec could not configure the output port"), qPrintable(err));
        QCOMPARE(m.mux->status().value("state").toString(), QString("error"));
        qInfo().noquote() << QString("operator start during a pending restart: answered after %1 ms (the port setup's timeout)").arg(t.elapsed());
        QTest::qWait(500);
        QCOMPARE(m.mux->status().value("state").toString(), QString("error"));                   // no restart after a failed start
        m.mux->stop();
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->findChildren<QProcess*>().isEmpty(), 5000);
    }

    // `auto` (the default) keeps tsdektec for the port setup while it exists; PlayPlayMux only lists / analyses.
    void portSetupToolSelection() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        const QString td = fakeTsduckDir(dir.path()); QVERIFY(!td.isEmpty());
        MuxManager mux(nullptr, dir.path());
        mux.setConfig(J{{"playplayMux", muxExe()}, {"tsduckDir", td}, {"output", J{{"kind", "dektec"}, {"port", 2}, {"device", 0}}}});
        QCOMPARE(mux.toolsEngine(), QString("native"));                                              // auto + PlayPlayMux found
        QCOMPARE(mux.dryRun().value("tools").toObject().value("portSetup").toString(), QString("native"));   // no tsdektec here
        QVERIFY(writeFile(td + "/tsdektec.exe", "placeholder"));                                     // never started by this test
        J dr = mux.dryRun();
        QCOMPARE(dr.value("tools").toObject().value("portSetup").toString(), QString("tsduck"));
        QVERIFY2(dr.value("dektecPrep").toString().contains("tsdektec.exe") && dr.value("dektecPrep").toString().contains("--output"), qPrintable(dr.value("dektecPrep").toString()));
        mux.setConfig(J{{"tools", "native"}});
        QCOMPARE(mux.dryRun().value("tools").toObject().value("portSetup").toString(), QString("native"));
        mux.setConfig(J{{"tools", "tsduck"}});
        QCOMPARE(mux.toolsEngine(), QString("tsduck"));
        QCOMPARE(mux.dryRun().value("tools").toObject().value("portSetup").toString(), QString("tsduck"));
        mux.setConfig(J{{"tools", "auto"}, {"playplayMux", dir.filePath("nope.exe")}});
        QCOMPARE(mux.toolsEngine(), QString("tsduck"));
        QCOMPARE(mux.playplayMuxExe(), QString());
        // dry runs start nothing but capabilities()' background listing (PlayPlayMux on the empty simulated card of initTestCase)
        for (QProcess* p : mux.findChildren<QProcess*>()) { QCOMPARE(p->objectName(), QString("dektec-probe")); QCOMPARE(QFileInfo(p->program()).fileName(), QString("PlayPlayMux.exe")); }
    }

    // installAsyncRoutes over HTTP: /mux/dektec, /mux/start and /mux/restart answer when their child finished (409 + reason on a
    // refused port), and another route answers meanwhile.
    void asyncRoutesOverHttp() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        const QString td = fakeTsduckDir(dir.path()); QVERIFY(!td.isEmpty());
        const QString busy = simWithPort2(dir.path(), "busy.json", true, "output"), free = simWithPort2(dir.path(), "free.json", false, "output");
        EnvScope simEnv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(free).toLocal8Bit());
        QCOMPARE(simGuard(), QString());
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setServices(udpServices(1));
        m.mux->setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}, {"tsduckDir", td}, {"output", J{{"kind", "dektec"}, {"port", 2}, {"device", 0}}}});
        RestApiServer rest(nullptr);
        const quint16 port = freeTcpPort(); QVERIFY(port > 0); QVERIFY(rest.start(port));
        rest.registerRoute("GET", "/api/v1/ping", [](const QJsonObject&, const QHash<QString, QString>&) { return J{{"pong", true}}; });
        MuxManager::installAsyncRoutes(&rest, m.mux.get());
        const QString base = QString("http://127.0.0.1:%1/api/v1").arg(port);
        QNetworkAccessManager nam; int st = 0;
        J r = httpCall(nam, "GET", base + "/mux/dektec?refresh=1", &st);
        QCOMPARE(st, 200);
        QCOMPARE(r.value("devices").toArray().size(), 1); QCOMPARE(r.value("devices").toArray()[0].toObject().value("model").toString(), QString("DTA-2154"));
        QCOMPARE(r.value("engine").toString(), QString("native"));
        r = httpCall(nam, "GET", base + "/ping", &st); QCOMPARE(st, 200); QVERIFY(r.value("pong").toBool());
        qputenv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(busy).toLocal8Bit());
        r = httpCall(nam, "POST", base + "/mux/start", &st);
        QCOMPARE(st, 409); QVERIFY2(r.value("error").toString().contains("in use"), qPrintable(compact(r)));
        qputenv("PLAYPLAYMUX_DEKTEC_SIM", QDir::toNativeSeparators(free).toLocal8Bit());
        r = httpCall(nam, "POST", base + "/mux/start", &st);
        QCOMPARE(st, 200); QCOMPARE(r.value("state").toString(), QString("running"));
        r = httpCall(nam, "POST", base + "/mux/restart", &st);
        QCOMPARE(st, 200); QCOMPARE(r.value("state").toString(), QString("running"));
        m.mux->stop();
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->findChildren<QProcess*>().isEmpty(), 5000);
    }

    // ------------------------------------------------------------------ DEP-18: network inputs as managed engine channels
    void dep18InputFeed_data() {
        QTest::addColumn<QString>("source");         // single-quoted JSON
        QTest::addColumn<bool>("engine");
        QTest::newRow("udp") << "{'kind':'input','input':{'type':'udp','location':'udp://127.0.0.1:50061'}}" << true;
        QTest::newRow("srt") << "{'kind':'input','input':{'type':'srt','location':'srt://10.0.0.5:9000?mode=caller'}}" << true;
        QTest::newRow("rtmp") << "{'kind':'input','input':{'type':'rtmp','location':'rtmp://host/live/key'}}" << true;
        QTest::newRow("rtsp") << "{'kind':'input','input':{'type':'rtsp','location':'rtsp://cam/stream'}}" << true;
        QTest::newRow("http") << "{'kind':'input','input':{'type':'http','location':'http://host/a.ts'}}" << true;
        QTest::newRow("udp forced to ffmpeg") << "{'kind':'input','input':{'type':'udp','location':'udp://127.0.0.1:50061','feed':'ffmpeg'}}" << false;
        QTest::newRow("udp without location") << "{'kind':'input','input':{'type':'udp','location':''}}" << false;
        QTest::newRow("test (lavfi)") << "{'kind':'input','input':{'type':'test','location':''}}" << false;
        QTest::newRow("file") << "{'kind':'input','input':{'type':'file','location':'C:/media/a.mp4'}}" << false;
        QTest::newRow("dshow") << "{'kind':'input','input':{'type':'dshow','location':'Cam'}}" << false;
        QTest::newRow("merged udp TS (kind udp)") << "{'kind':'udp','url':'udp://127.0.0.1:50061'}" << false;
    }
    void dep18InputFeed() {
        QFETCH(QString, source); QFETCH(bool, engine);
        J s = inputService("svc3", J()); s["source"] = sq(source.toUtf8().constData());
        QCOMPARE(MuxManager::inputViaEngine(s), engine);
        QCOMPARE(MuxManager::feedChannelFor(s), engine ? QString("mux-svc3") : QString());
        const J f = MuxManager::fixedSourceFor(s);
        QCOMPARE(f.isEmpty(), !engine);
        if (engine) { QCOMPARE(f.value("type").toString(), QString("stream")); QCOMPARE(f.value("url").toString(), s["source"].toObject()["input"].toObject()["location"].toString()); QVERIFY(f.value("realtime").toBool()); QCOMPARE(f.value("inputOptions").toObject().value("rw_timeout").toString(), QString("10000000")); }
    }

    // A udp:// input service becomes hidden channel mux-<id> pinned to {type:"stream", url} whose engine output carries the whole
    // encoder profile (mpeg2video 1080i: closed GOP + interlaced, the fields the ffmpeg path once dropped); no ffmpeg encoder for
    // it in the dry run; the test service next to it stays on ffmpeg; feed = "ffmpeg" brings the old path back and removes the channel.
    void dep18ManagedChannelForNetworkInput() {
        EnvScope path("PATH", pathWithoutTsduck());
        MuxWithStore m; QVERIFY(m.open());
        const QString td = fakeTsduckDir(m.dir.path()); QVERIFY(!td.isEmpty());       // dryRun's capabilities(): never the real TSDuck / card
        m.mux->setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}, {"tsduckDir", td}, {"mcastBase", "127.0.0."}, {"output", J{{"kind", "file"}, {"url", m.dir.filePath("mux.ts")}}}});
        J s1; s1["id"] = "svc1"; s1["longName"] = "Star UDP"; s1["udpPort"] = 50062; s1["encoder"] = sq("{'applyPreset':'atsc_mpeg2_ac3_hd'}");
        s1["source"] = sq("{'kind':'input','input':{'type':'udp','location':'udp://127.0.0.1:50061?fifo_size=100000'}}");
        J s2 = inputService("svc2", J()); s2["udpPort"] = 50063;
        m.mux->setServices(QJsonArray{s1, s2});
        J sync = m.mux->syncManagedChannels();
        QCOMPARE(sync.value("created").toArray(), QJsonArray{"mux-svc1"});
        QCOMPARE(sync.value("channels").toArray(), QJsonArray{"mux-svc1"});
        bool found = false; const sched::ChannelProfile p = m.store.channel("mux-svc1", &found); QVERIFY(found);
        QVERIFY(p.extra.value("hidden").toBool()); QCOMPARE(p.extra.value("managedBy").toString(), QString("mux")); QCOMPARE(p.extra.value("muxServiceId").toString(), QString("svc1"));
        const J fixed = p.extra.value("fixedSource").toObject();
        QCOMPARE(fixed.value("type").toString(), QString("stream")); QCOMPARE(fixed.value("url").toString(), QString("udp://127.0.0.1:50061?fifo_size=100000"));
        QCOMPARE(fixed.value("title").toString(), QString("Star UDP"));
        QCOMPARE(p.width, 1920); QCOMPARE(p.height, 1080);
        QCOMPARE(p.identity.value("slateCaptions").toArray().last().toString(), QString("STREAM SOURCE OFFLINE"));
        QCOMPARE(p.outputs.size(), 1);
        const J out = p.outputs[0].toObject(), prof = out.value("profile").toObject();
        QCOMPARE(out.value("name").toString(), QString("mux:svc1")); QCOMPARE(out.value("url").toString(), QString("udp://127.0.0.1:50062?pkt_size=1316&ttl=1"));
        QCOMPARE(prof.value("videoCodec").toString(), QString("mpeg2video")); QVERIFY(prof.value("closedGop").toBool()); QVERIFY(prof.value("interlaced").toBool());
        QCOMPARE(prof.value("audioCodec").toString(), QString("ac3"));
        QCOMPARE(m.mux->status().value("managedChannels").toArray(), QJsonArray{"mux-svc1"});
        const J dr = m.mux->dryRun();
        QCOMPARE(dr.value("encoders").toArray().size(), 1); QCOMPARE(dr.value("encoders").toArray()[0].toObject().value("serviceId").toString(), QString("svc2"));
        J ch; for (const QJsonValue& v : dr.value("channels").toArray()) if (v.toObject().value("serviceId").toString() == "svc1") ch = v.toObject();
        QVERIFY(ch.value("managed").toBool()); QCOMPARE(ch.value("channelId").toString(), QString("mux-svc1")); QCOMPARE(ch.value("fixedSource").toObject().value("type").toString(), QString("stream"));
        QVERIFY(logHas(*m.mux, "managed channel created: mux-svc1 <- stream udp://127.0.0.1:50061"));
        QVERIFY(m.mux->syncManagedChannels().value("updated").toArray().isEmpty());                  // idempotent
        // Same channel, other source: an update with sourceChanged (the Supervisor reloads; no engine restart for the url alone).
        J s1b = s1; s1b["source"] = sq("{'kind':'input','input':{'type':'rtsp','location':'rtsp://op:s3cret@10.1.1.9/live','transport':'udp'}}");
        m.mux->setServices(QJsonArray{s1b, s2});
        sync = m.mux->syncManagedChannels();
        QCOMPARE(sync.value("updated").toArray().size(), 1);
        const J u = sync.value("updated").toArray()[0].toObject(); QVERIFY(u.value("sourceChanged").toBool()); QVERIFY(!u.value("restart").toBool());
        const J f2 = m.store.channel("mux-svc1").extra.value("fixedSource").toObject();
        QCOMPARE(f2.value("inputOptions").toObject().value("rtsp_transport").toString(), QString("udp"));
        QVERIFY2(!logText(*m.mux).contains("s3cret"), qPrintable(logText(*m.mux)));                   // no credentials in the log
        if (qEnvironmentVariableIsSet("PSIP_DEP18_DUMP")) {
            // the bench run (docs/plans/impl-log/inhouse-mux.md, DEP-18) starts a standalone engine with exactly this profile
            m.mux->setServices(QJsonArray{s1, s2}); m.mux->syncManagedChannels();
            const sched::ChannelProfile q = m.store.channel("mux-svc1");
            J cfg{{"frameRate", q.frameRate}, {"width", q.width}, {"height", q.height}, {"audioChannels", q.audioChannels}, {"outputs", q.outputs}};
            J item{{"id", "fixed-dep18"}, {"title", "Star UDP"}, {"liveSource", q.extra.value("fixedSource")}, {"duration", 3600.0}};
            const QString d = qEnvironmentVariable("PSIP_DEP18_DUMP");
            QVERIFY(writeFile(d + "/channel.json", QJsonDocument(cfg).toJson()) && writeFile(d + "/list.json", QJsonDocument(QJsonArray{item}).toJson()));
        }
        // feed = ffmpeg: the old encoder path, the hidden channel goes
        J s1c = s1; s1c["source"] = sq("{'kind':'input','input':{'type':'udp','location':'udp://127.0.0.1:50061','feed':'ffmpeg'}}");
        m.mux->setServices(QJsonArray{s1c, s2});
        sync = m.mux->syncManagedChannels();
        QCOMPARE(sync.value("removed").toArray(), QJsonArray{"mux-svc1"});
        m.store.channel("mux-svc1", &found); QVERIFY(!found);
        QCOMPARE(m.mux->dryRun().value("encoders").toArray().size(), 2);
    }

    // A service that becomes engine-fed while the mux runs loses its ffmpeg encoder at once (one sender per multicast), without
    // waiting on the event loop; the other encoder keeps running. The "ffmpeg" here is a copy of cmd.exe that waits on its stdin.
    void dep18RunningEncoderGoesWhenServiceIsEngineFed() {
        const QString cmd = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows")).filePath("System32/cmd.exe");
        if (!QFileInfo::exists(cmd)) QSKIP("cmd.exe not found");
        QTemporaryDir dir; QVERIFY(dir.isValid());
        EnvScope path("PATH", pathWithoutTsduck());
        const QString td = fakeTsduckDir(dir.path()); QVERIFY(!td.isEmpty());
        const QString fakeFfmpeg = dir.filePath("ffmpeg.exe"); QVERIFY(QFile::copy(cmd, fakeFfmpeg));
        MuxWithStore m; QVERIFY(m.open());
        m.mux->setConfig(J{{"tools", "native"}, {"playplayMux", muxExe()}, {"tsduckDir", td}, {"ffmpegPath", fakeFfmpeg}, {"mcastBase", "127.0.0."},
                           {"output", J{{"kind", "file"}, {"url", dir.filePath("mux.ts")}}}});
        J s1 = inputService("svc1", J()); s1["udpPort"] = 50064; J s2 = inputService("svc2", J()); s2["udpPort"] = 50065;
        m.mux->setServices(QJsonArray{s1, s2});
        bool done = false, ok = false; m.mux->startAsync([&](bool o, const QString&) { done = true; ok = o; });
        QTRY_VERIFY_WITH_TIMEOUT(done, 15000); QVERIFY2(ok, qPrintable(logText(*m.mux)));
        auto running = [&]() { QStringList r; for (const QJsonValue& v : m.mux->status().value("encoders").toArray()) if (v.toObject().value("running").toBool()) r << v.toObject().value("service").toString(); r.sort(); return r; };
        QTRY_COMPARE_WITH_TIMEOUT(running(), (QStringList{"svc1", "svc2"}), 5000);
        s1["source"] = sq("{'kind':'input','input':{'type':'udp','location':'udp://127.0.0.1:50061'}}");
        m.mux->setServices(QJsonArray{s1, s2});
        QElapsedTimer t; t.start();
        const J sync = m.mux->syncManagedChannels();
        QVERIFY2(t.elapsed() < 500, qPrintable(QString::number(t.elapsed())));                       // kill, not waitForFinished
        QCOMPARE(sync.value("created").toArray(), QJsonArray{"mux-svc1"});
        QCOMPARE(running(), QStringList{"svc2"});
        QCOMPARE(m.mux->status().value("encoders").toArray().size(), 1);
        QVERIFY(logHas(*m.mux, "encoder svc1 stopped"));
        QTest::qWait(3500);                                                                           // the watchdog does not bring it back
        QCOMPARE(m.mux->status().value("encoders").toArray().size(), 1);
        m.mux->stop();
        QTRY_VERIFY_WITH_TIMEOUT(m.mux->findChildren<QProcess*>().isEmpty(), 5000);
    }
};

QTEST_GUILESS_MAIN(TestInhousePsip)
#include "test_inhouse_psip.moc"
