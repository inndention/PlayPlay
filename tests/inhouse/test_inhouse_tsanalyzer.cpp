// Lane inhouse-mux, DEP-16 (docs/plans/inhouse-deps.md §6.5 stage 2): the TS analyzer behind `PlayPlayMux analyze`
// (tools/mux/tsanalyzer.*, udpin.*), which replaces `tsp -P tables / -P analyze` in MuxManager::analyze.
//  - formatterGoldens: every PSIP / PSI / SCTE 35 golden of DEP-14 (plus an SDT and two probe files) packetized and analysed
//    gives exactly the text `tsp --atsc -I file -P tables` printed for it (tests/inhouse/data/psip/tables/*.txt, recorded once
//    with TSDuck 3.41 on the dev box; the test needs no TSDuck).
//  - a real stream: libavcodec MPEG-2 video + AC-3 through the engine's TsMuxer (CBR, null stuffing), with MGT / TVCT / STT,
//    EIT and SCTE 35 sections (PlayPlayTsKit) injected into null packets: services, PIDs, bitrates, PCR interval = pcrPeriodMs,
//    PTS-PCR, cues; an injected CC discontinuity and a TVCT section with a bad CRC are reported; with TSDuck installed the
//    tables text equals `tsp -I file -P tables` for the same file.
//  - continuity rules, PCR jitter, sync loss, UDP unicast and multicast (loopback only) input, the PlayPlayMux CLI.
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QNetworkInterface>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QUdpSocket>

#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

#include "psip.h"
#include "scte35bin.h"
#include "tsanalyzer.h"
#include "tssection.h"
#include "udpin.h"
#include "sinks/tsmuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
}

using namespace tskit;

namespace {

QString golden(const QString& name) { return QStringLiteral(TSANALYZER_GOLDEN_DIR) + "/" + name; }
QByteArray readAll(const QString& path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
QString lf(QString s) { s.replace(QStringLiteral("\r\n"), QStringLiteral("\n")); return s; }

// First differing line, for failure messages on long texts.
QString firstLineDiff(const QString& got, const QString& want) {
    const QStringList g = got.split('\n'), w = want.split('\n');
    for (int i = 0; i < qMax(g.size(), w.size()); ++i) {
        const QString a = i < g.size() ? g[i] : "<end>", b = i < w.size() ? w[i] : "<end>";
        if (a != b) return QString("line %1\n  got  [%2]\n  want [%3]").arg(i + 1).arg(a, b);
    }
    return {};
}

constexpr int kVideoPid = 0x0100, kAudioPid = 0x0101, kPmtPid = 0x1000, kEitPid = 0x0200, kScte35Pid = 0x01F0;

// ------------------------------------------------------------------------------------------------ the test stream
// `seconds` of 320x240 MPEG-2 video (30 fps, B-frames) and AC-3 stereo, muxed CBR by the engine's TsMuxer.
bool muxTestStream(int seconds, int pcrPeriodMs, qint64 muxrate, QByteArray* out, QString* err) {
    const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AC3);
    if (!vc || !ac) { *err = "mpeg2video / ac3 encoders missing"; return false; }
    AVCodecContext* v = avcodec_alloc_context3(vc);
    v->width = 320; v->height = 240; v->pix_fmt = AV_PIX_FMT_YUV420P;
    v->time_base = {1, 30}; v->framerate = {30, 1};
    v->bit_rate = 600000;
    v->gop_size = 15; v->max_b_frames = 2;
    AVCodecContext* a = avcodec_alloc_context3(ac);
    a->sample_fmt = AV_SAMPLE_FMT_FLTP; a->sample_rate = 48000; a->time_base = {1, 48000}; a->bit_rate = 128000;
    const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&a->ch_layout, &stereo);
    if (avcodec_open2(v, vc, nullptr) < 0 || avcodec_open2(a, ac, nullptr) < 0) { *err = "cannot open encoders"; avcodec_free_context(&v); avcodec_free_context(&a); return false; }
    AVCodecParameters* vp = avcodec_parameters_alloc();
    AVCodecParameters* ap = avcodec_parameters_alloc();
    avcodec_parameters_from_context(vp, v);
    avcodec_parameters_from_context(ap, a);

    QJsonObject o;
    o["muxrate"] = double(muxrate); o["pcrPeriodMs"] = pcrPeriodMs; o["patPeriodSec"] = 0.1; o["sdtPeriodSec"] = 0.5;
    o["serviceId"] = 1; o["serviceName"] = "Star News"; o["provider"] = "PlayPlay"; o["pmtPid"] = kPmtPid;
    o["videoPid"] = kVideoPid; o["audioPid"] = kAudioPid; o["tsId"] = 25;
    QByteArray ts;
    TsMuxer mux;
    bool ok = mux.open(vp, v->time_base, ap, a->time_base, o, [&ts](const uint8_t* d, int n) { ts.append(reinterpret_cast<const char*>(d), n); });
    if (!ok) *err = mux.lastError();

    AVFrame* vf = av_frame_alloc();
    vf->format = v->pix_fmt; vf->width = v->width; vf->height = v->height;
    av_frame_get_buffer(vf, 0);
    AVFrame* af = av_frame_alloc();
    af->format = a->sample_fmt; af->sample_rate = 48000; af->nb_samples = a->frame_size;
    av_channel_layout_copy(&af->ch_layout, &a->ch_layout);
    av_frame_get_buffer(af, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&](AVCodecContext* c, int kind) {
        while (ok && avcodec_receive_packet(c, pkt) == 0) { ok = mux.writePacket(pkt, kind); if (!ok) *err = mux.lastError(); av_packet_unref(pkt); }
    };
    qint64 samples = 0;
    const int frames = seconds * 30;
    for (int i = 0; i < frames && ok; ++i) {
        av_frame_make_writable(vf);
        for (int y = 0; y < v->height; ++y) for (int x = 0; x < v->width; ++x) vf->data[0][y * vf->linesize[0] + x] = uint8_t(x + y + 3 * i);
        for (int y = 0; y < v->height / 2; ++y) for (int x = 0; x < v->width / 2; ++x) { vf->data[1][y * vf->linesize[1] + x] = uint8_t(128 + y); vf->data[2][y * vf->linesize[2] + x] = uint8_t(64 + x); }
        vf->pts = i;
        avcodec_send_frame(v, vf);
        drain(v, 0);
        while (samples * 30 < qint64(i + 1) * 48000 && ok) {     // audio up to the end of this video frame
            av_frame_make_writable(af);
            for (int ch = 0; ch < 2; ++ch) {
                float* d = reinterpret_cast<float*>(af->data[ch]);
                for (int k = 0; k < af->nb_samples; ++k) d[k] = 0.3f * float(std::sin(2.0 * 3.14159265358979 * 1000.0 * double(samples + k) / 48000.0));
            }
            af->pts = samples;
            samples += af->nb_samples;
            avcodec_send_frame(a, af);
            drain(a, 1);
        }
    }
    avcodec_send_frame(v, nullptr); drain(v, 0);
    avcodec_send_frame(a, nullptr); drain(a, 1);
    mux.close();
    av_packet_free(&pkt); av_frame_free(&vf); av_frame_free(&af);
    avcodec_parameters_free(&vp); avcodec_parameters_free(&ap);
    avcodec_free_context(&v); avcodec_free_context(&a);
    if (ok && ts.size() % kPacketSize != 0) { *err = QString("TsMuxer output is %1 bytes, not whole packets").arg(ts.size()); ok = false; }
    *out = ts;
    return ok;
}

int pidAt(const QByteArray& ts, qint64 index) { return packetPid(ts.constData() + index * kPacketSize); }

// Sections of `pid` into null packets at or after `atPacket` (the packetizer keeps the PID's continuity counter).
bool inject(QByteArray& ts, Packetizer& pk, const QList<QByteArray>& sections, qint64 atPacket) {
    const QByteArray pkts = pk.packetize(sections);
    const qint64 total = ts.size() / kPacketSize;
    qint64 idx = atPacket;
    for (int k = 0; k < pkts.size() / kPacketSize; ++k) {
        while (idx < total && pidAt(ts, idx) != kNullPid) ++idx;
        if (idx >= total) return false;
        std::memcpy(ts.data() + idx * kPacketSize, pkts.constData() + k * kPacketSize, kPacketSize);
        ++idx;
    }
    return true;
}

qint64 gpsAt(int h, int m) { return gpsSeconds(QDateTime(QDate(2026, 10, 26), QTime(h, m), QTimeZone::utc())); }

QByteArray tvctSection(bool corrupt) {
    Tvct t; t.version = 1; t.tsid = 25;
    VctChannel c; c.shortName = "WSTR"; c.major = 25; c.minor = 1; c.channelTsid = 25; c.programNumber = 1; c.sourceId = 1;
    c.descriptors.append(extendedChannelNameDescriptor(MultiString::one("Star News Plus")));
    t.channels.append(c);
    QByteArray s = buildTvct(t).first();
    if (corrupt) s[20] = char(s[20] ^ 0x5A);              // inside the short name; CRC32 left as it was
    return s;
}

struct Injected { qint64 firstScte35 = -1; int psipGroups = 0; };

// MGT + TVCT + STT on 0x1FFB every 200 packets, EIT-0 on 0x0200 every 800, three SCTE 35 cues on 0x01F0.
Injected injectTables(QByteArray& ts, bool corruptFirstTvct) {
    Injected r;
    const QByteArray tvct = tvctSection(false);
    Eit e; e.version = 1; e.sourceId = 1;
    EitEvent ev; ev.eventId = 7850; ev.startTime = quint32(gpsAt(14, 0)); ev.etmLocation = 0; ev.lengthSeconds = 3600; ev.title = MultiString::one("Morning News");
    e.events.append(ev);
    const QByteArray eit = buildEit(e).first();
    Mgt m; m.version = 1;
    m.tables.append({0x0000, 0x1FFB, 1, quint32(tvct.size()), {}});
    m.tables.append({0x0100, kEitPid, 1, quint32(eit.size()), {}});
    const QByteArray mgt = buildMgt(m).first();
    Stt stt; stt.systemTime = quint32(gpsAt(14, 5) + 18);
    const QByteArray sttS = buildStt(stt).first();
    Packetizer psip(0x1FFB), eitPk(kEitPid), cue(kScte35Pid);
    const qint64 total = ts.size() / kPacketSize;
    for (qint64 at = 10; at + 50 < total; at += 200, ++r.psipGroups)
        inject(ts, psip, {mgt, r.psipGroups == 0 && corruptFirstTvct ? tvctSection(true) : tvct, sttS}, at);
    for (qint64 at = 60; at + 50 < total; at += 800) inject(ts, eitPk, {eit}, at);
    const QList<QByteArray> cues = {scte35::immediateInsert(101, true, 30.0, true, 1), scte35::immediateInsert(102, false, 0, false, 1), scte35::nullSection()};
    for (int k = 0; k < cues.size(); ++k) {
        const qint64 at = 1500 + 1500 * k;
        inject(ts, cue, {cues[k]}, at);
        if (k == 0) for (qint64 i = at; i < total; ++i) if (pidAt(ts, i) == kScte35Pid) { r.firstScte35 = i; break; }
    }
    return r;
}

struct CcBreak { qint64 packet = -1; int expected = -1, got = -1; };   // packet that shows the break, CC wanted / seen there

// Replaces one payload-only video packet near `near` with a null packet: the next video packet is one CC ahead (the CC of
// the removed packet is what the analyzer expects there).
CcBreak breakContinuity(QByteArray& ts, qint64 near) {
    const qint64 total = ts.size() / kPacketSize;
    for (qint64 i = near; i < total; ++i) {
        const char* p = ts.constData() + i * kPacketSize;
        if (packetPid(p) != kVideoPid || packetPusi(p) || packetAfc(p) != 1) continue;
        const int cc = packetCc(p);
        QByteArray null(kPacketSize, char(0xFF));
        null[0] = 0x47; null[1] = 0x1F; null[2] = char(0xFF); null[3] = 0x10;
        std::memcpy(ts.data() + i * kPacketSize, null.constData(), kPacketSize);
        for (qint64 j = i + 1; j < total; ++j) {
            const char* q = ts.constData() + j * kPacketSize;
            if (packetPid(q) == kVideoPid && (packetAfc(q) & 1)) return {j, cc, packetCc(q)};
        }
    }
    return {};
}

QByteArray packet(int pid, int cc, bool payload, bool adaptation, bool discontinuity = false) {
    QByteArray p(kPacketSize, char(0xFF));
    p[0] = 0x47; p[1] = char((pid >> 8) & 0x1F); p[2] = char(pid & 0xFF);
    p[3] = char(((adaptation ? 2 : 0) | (payload ? 1 : 0)) << 4 | (cc & 0x0F));
    if (adaptation) { p[4] = char(payload ? 1 : 183); p[5] = char(discontinuity ? 0x80 : 0x00); }
    return p;
}

TsAnalyzer analyse(const QByteArray& ts, AnalyzerOptions o = AnalyzerOptions()) {
    TsAnalyzer an(o);
    for (int off = 0; off < ts.size(); off += 7 * kPacketSize) an.feed(ts.constData() + off, qMin<qsizetype>(7 * kPacketSize, ts.size() - off));
    an.finish();
    return an;
}

const TsAnalyzer::PidStats* pidStats(const TsAnalyzer& an, int pid) { const auto it = an.pids().constFind(pid); return it == an.pids().constEnd() ? nullptr : &it.value(); }

QString tspExe() {
    const QString env = qEnvironmentVariable("TSDUCK_BIN");
    const QString p = (env.isEmpty() ? QStringLiteral("C:/Program Files/TSDuck/bin") : env) + "/tsp.exe";
    return QFileInfo::exists(p) ? p : QString();
}

// The last stdout line of a ppcli tool.
QJsonObject lastJson(const QByteArray& out) { return QJsonDocument::fromJson(out.trimmed().split('\n').last()).object(); }

}   // namespace

class TestTsAnalyzer : public QObject {
    Q_OBJECT
    QByteArray m_clean, m_broken;                         // the TsMuxer stream with tables; the same with a CC break + bad CRC
    Injected m_inj;
    CcBreak m_cc;
    QTemporaryDir m_tmp;
    static constexpr qint64 kMuxrate = 2500000;
    static constexpr int kPcrPeriodMs = 30;

private slots:
    void initTestCase() {
        QVERIFY(m_tmp.isValid());
        QByteArray ts;
        QString err;
        QVERIFY2(muxTestStream(4, kPcrPeriodMs, kMuxrate, &ts, &err), qPrintable(err));
        QVERIFY(ts.size() / kPacketSize > 6000);
        m_clean = ts;
        m_inj = injectTables(m_clean, false);
        QVERIFY(m_inj.psipGroups > 20);
        QVERIFY(m_inj.firstScte35 > 0);
        m_broken = ts;
        injectTables(m_broken, true);
        m_cc = breakContinuity(m_broken, 2500);
        QVERIFY(m_cc.packet > 0);
        QFile c(m_tmp.filePath("clean.ts")), b(m_tmp.filePath("broken.ts"));
        QVERIFY(c.open(QIODevice::WriteOnly) && c.write(m_clean) == m_clean.size());
        QVERIFY(b.open(QIODevice::WriteOnly) && b.write(m_broken) == m_broken.size());
    }

    // ------------------------------------------------------------------ text layout = tsp -P tables
    void formatterGoldens_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("pid");
        const QList<QPair<const char*, int>> rows = {
            {"pat", 0x0000}, {"pmt", 0x1000}, {"pmt_scte35", 0x1000}, {"mgt", 0x1FFB}, {"tvct", 0x1FFB}, {"tvct_extra", 0x1FFB},
            {"stt", 0x1FFB}, {"rrt", 0x1FFB}, {"eit0", 0x0200}, {"ett0", 0x0201}, {"eit_unicode", 0x0200}, {"descriptors", 0x1FFB},
            {"quirks", 0x1FFB}, {"split", 0x1FFB}, {"scte35_out", 0x01F0}, {"scte35_in", 0x01F0}, {"scte35_cancel", 0x01F0},
            {"scte35_null", 0x01F0}, {"scte35_misc", 0x01F0}, {"analyzer_probe", 0x1FFB}, {"analyzer_mgt", 0x1FFB}, {"sdt", 0x0011}};
        for (const auto& r : rows) QTest::newRow(r.first) << QString::fromLatin1(r.first) << r.second;
    }
    void formatterGoldens() {
        QFETCH(QString, name);
        QFETCH(int, pid);
        QString e;
        const QList<QByteArray> secs = splitSections(readAll(golden(name + ".bin")), &e);
        QVERIFY2(!secs.isEmpty(), qPrintable(name + ": " + e));
        Packetizer pk(pid);
        const TsAnalyzer an = analyse(pk.packetize(secs));
        const QString want = lf(QString::fromUtf8(readAll(golden("tables/" + name + ".txt"))));
        QVERIFY(!want.isEmpty());
        QVERIFY2(an.tablesText() == want, qPrintable(name + ": " + firstLineDiff(an.tablesText(), want)));
        QCOMPARE(an.crcErrors(), qint64(0));
    }

    void namesAndDvbContext() {
        QCOMPARE(tableName(0xC8), QString("TVCT (ATSC)"));
        QCOMPARE(tableName(0xF0), QString("unknown (0xF0)"));
        QCOMPARE(tableName(0x55), QString("EIT schedule Actual"));
        QCOMPARE(streamTypeName(0x81), QString("ATSC AC-3 Audio"));
        QCOMPARE(streamTypeName(0x1B), QString("AVC video"));
        QCOMPARE(streamTypeName(0x90), QString("unknown"));
        // A DVB descriptor in a PMT: decoded only once a DVB table (here the SDT) was shown, as tsp does.
        Pmt p; p.programNumber = 1; p.pcrPid = 0x100;
        PmtStream s; s.streamType = 0x1B; s.pid = 0x100; s.descriptors.append({0x52, QByteArray(1, '\x07')});
        p.streams.append(s);
        const QList<QByteArray> pmt = buildPmt(p);
        QVERIFY(formatTable(0x1000, pmt, false).contains("unknown (0x52, 82), 1 bytes"));
        QVERIFY(formatTable(0x1000, pmt, true).contains("Stream Identifier (0x52, 82), 1 bytes\n      Component tag: 0x07 (7)"));
        Packetizer pp(0x1000), sp(0x0011);
        const QList<QByteArray> sdt = splitSections(readAll(golden("sdt.bin")));
        QVERIFY(analyse(pp.packetize(pmt)).tablesText().contains("unknown (0x52"));
        QVERIFY(analyse(sp.packetize(sdt) + pp.packetize(pmt)).tablesText().contains("Stream Identifier (0x52"));
    }

    // ------------------------------------------------------------------ the TsMuxer stream
    void streamServicesPidsBitrates() {
        const TsAnalyzer an = analyse(m_clean);
        QCOMPARE(an.packets(), qint64(m_clean.size() / kPacketSize));
        QCOMPARE(an.syncLosses(), qint64(0));
        QCOMPARE(an.ccErrors(), qint64(0));
        QCOMPARE(an.crcErrors(), qint64(0));
        QCOMPARE(an.bitrateSource(), QString("pcr"));
        QVERIFY2(std::fabs(an.bitrate() - double(kMuxrate)) / double(kMuxrate) < 0.002, qPrintable(QString::number(an.bitrate())));
        QCOMPARE(an.referencePcrPid(), kVideoPid);
        double sum = 0;
        for (const auto& p : an.pids()) sum += p.bitrate;
        QVERIFY(std::fabs(sum - an.bitrate()) < 1.0);
        for (int pid : {0x0000, 0x0011, kPmtPid, kVideoPid, kAudioPid, 0x1FFB, kEitPid, kScte35Pid, kNullPid}) QVERIFY2(pidStats(an, pid), qPrintable(QString::number(pid, 16)));
        QCOMPARE(pidStats(an, kPmtPid)->description, QString("PMT (program 1)"));
        QCOMPARE(pidStats(an, kVideoPid)->description, QString("MPEG-2 Video (program 1), PCR"));
        QCOMPARE(pidStats(an, kAudioPid)->description, QString("ATSC AC-3 Audio (program 1)"));
        QCOMPARE(pidStats(an, kEitPid)->description, QString("ATSC EIT-0"));
        QVERIFY(pidStats(an, kVideoPid)->bitrate > 300000 && pidStats(an, kVideoPid)->bitrate < 1000000);
        QVERIFY(pidStats(an, kAudioPid)->bitrate > 120000 && pidStats(an, kAudioPid)->bitrate < 160000);

        const QList<TsAnalyzer::Service> svcs = an.services();
        QCOMPARE(svcs.size(), 1);
        const TsAnalyzer::Service& s = svcs.first();
        QCOMPARE(s.programNumber, 1);
        QCOMPARE(s.pmtPid, kPmtPid);
        QCOMPARE(s.pcrPid, kVideoPid);
        QCOMPARE(s.name, QString("Star News"));
        QCOMPARE(s.provider, QString("PlayPlay"));
        QCOMPARE(s.major, 25); QCOMPARE(s.minor, 1);
        QCOMPARE(s.shortName, QString("WSTR"));
        QCOMPARE(s.longName, QString("Star News Plus"));
        QCOMPARE(s.sourceId, 1);
        QCOMPARE(s.streams.size(), 2);
        QCOMPARE(s.streams[0].pid, kVideoPid); QCOMPARE(s.streams[0].streamType, 0x02);
        QCOMPARE(s.streams[1].pid, kAudioPid); QCOMPARE(s.streams[1].streamType, 0x81);

        const QJsonObject j = an.toJson();
        QCOMPARE(j.value("services").toArray().first().toObject().value("channel").toString(), QString("25.1"));
        QCOMPARE(j.value("pcrPid").toInt(), kVideoPid);
        QCOMPARE(j.value("tsid").toInt(), 25);
    }

    void streamPcrAndPts() {
        const TsAnalyzer an = analyse(m_clean);
        const auto* v = pidStats(an, kVideoPid);
        QVERIFY(v && v->pcrIntervalMs.count > 100);
        // libavformat stamps PCR every pcr_period at the mux rate: the interval is the configured pcrPeriodMs.
        QVERIFY2(std::fabs(v->pcrIntervalMs.avg - kPcrPeriodMs) < 0.2 * kPcrPeriodMs, qPrintable(QString::number(v->pcrIntervalMs.avg)));
        QVERIFY2(v->pcrIntervalMs.max < kPcrPeriodMs + 5.0, qPrintable(QString::number(v->pcrIntervalMs.max)));
        QCOMPARE(v->pcrIntervalOver100ms, qint64(0));
        QVERIFY2(v->pcrJitterNs.max < 1000.0, qPrintable(QString::number(v->pcrJitterNs.max)));   // CBR from byte positions
        QCOMPARE(v->pcrJitterOverMax, qint64(0));
        QCOMPARE(v->pcrArrivalJitterMs, -1.0);            // a file has no arrival times
        // PTS ahead of PCR (decoder buffering), DTS present for the B-frame video only.
        QVERIFY(v->ptsPcrMs.count > 50 && v->ptsPcrMs.min > 0 && v->ptsPcrMs.max < 2000);
        QVERIFY(v->dtsPcrMs.count > 0 && v->dtsPcrMs.min > 0);
        const auto* a = pidStats(an, kAudioPid);
        QVERIFY2(a && a->ptsPcrMs.count > 10 && a->ptsPcrMs.min > 0, qPrintable(QString("%1 PES").arg(a ? a->ptsPcrMs.count : -1)));   // several AC-3 frames per PES
        // Tables repeat as injected: PSIP every 200 packets, about 120 ms at 2.5 Mb/s.
        bool found = false;
        for (const TsAnalyzer::TableStats& t : an.tables())
            if (t.pid == 0x1FFB && t.tableId == 0xC8) {
                found = true;
                QCOMPARE(t.sections, qint64(m_inj.psipGroups));
                QVERIFY2(std::fabs(t.intervalMs.avg - 200.0 * 1504.0 * 1000.0 / kMuxrate) < 5.0, qPrintable(QString::number(t.intervalMs.avg)));
            }
        QVERIFY(found);
    }

    void streamTablesAndScte35() {
        const TsAnalyzer an = analyse(m_clean);
        const QString t = an.tablesText();
        for (const char* h : {"* SDT Actual, TID 0x42 (66), PID 0x0011 (17)", "* PAT, TID 0x00 (0), PID 0x0000 (0)", "* PMT, TID 0x02 (2), PID 0x1000 (4096)",
                              "* MGT (ATSC), TID 0xC7 (199), PID 0x1FFB (8187)", "* TVCT (ATSC), TID 0xC8 (200), PID 0x1FFB (8187)",
                              "* STT (ATSC), TID 0xCD (205), PID 0x1FFB (8187)", "* EIT (ATSC), TID 0xCB (203), PID 0x0200 (512)"})
            QVERIFY2(t.count(QString::fromLatin1(h)) == 1, h);   // long tables: each version once
        QCOMPARE(t.count("* SCTE 35 Splice Information, TID 0xFC (252), PID 0x01F0 (496)"), 3);   // short sections: every one
        QVERIFY(t.contains("    - Channel 25.1, short name: \"WSTR\"\n"));
        QVERIFY(t.contains("    Elementary stream: type 0x81 (ATSC AC-3 Audio), PID: 0x0101 (257)\n"));
        QCOMPARE(an.displayedTables(), 10);

        const QList<TsAnalyzer::Event>& cues = an.scte35();
        QCOMPARE(cues.size(), 3);
        QCOMPARE(cues[0].packet, m_inj.firstScte35);
        QCOMPARE(cues[0].json.value("command").toString(), QString("SpliceInsert"));
        QCOMPARE(cues[0].json.value("eventId").toInt(), 101);
        QVERIFY(cues[0].json.value("outOfNetwork").toBool());
        QVERIFY(cues[0].json.value("immediate").toBool());
        QCOMPARE(cues[0].json.value("durationSec").toDouble(), 30.0);
        QCOMPARE(cues[1].json.value("eventId").toInt(), 102);
        QVERIFY(!cues[1].json.value("outOfNetwork").toBool());
        QCOMPARE(cues[2].json.value("command").toString(), QString("SpliceNull"));
        const QJsonObject j = an.toJson();
        QVERIFY(j.value("scte35").toArray().first().toObject().value("timeSec").toDouble() > 0.5);

        // --pid / --max-tables as tsp: only the PSIP base PID, the first two tables.
        AnalyzerOptions o; o.displayPids = {0x1FFB}; o.maxTables = 2;
        const TsAnalyzer f = analyse(m_clean, o);
        QCOMPARE(f.displayedTables(), 2);
        QVERIFY(f.tableLimitReached());
        QVERIFY(f.tablesText().startsWith("\n* MGT (ATSC)"));
        QVERIFY(!f.tablesText().contains("PID 0x0000"));
    }

    void streamErrorsReported() {
        const TsAnalyzer an = analyse(m_broken);
        QCOMPARE(an.ccErrors(), qint64(1));
        QCOMPARE(an.crcErrors(), qint64(1));
        const auto* v = pidStats(an, kVideoPid);
        QCOMPARE(v->ccErrors, qint64(1));
        QCOMPARE(pidStats(an, 0x1FFB)->crcErrors, qint64(1));
        bool cc = false, crc = false;
        for (const TsAnalyzer::Event& e : an.errors()) {
            if (e.type == "cc") {
                cc = true;
                QCOMPARE(e.pid, kVideoPid);
                QCOMPARE(e.packet, m_cc.packet);
                QCOMPARE(e.json.value("expected").toInt(), m_cc.expected);
                QCOMPARE(e.json.value("got").toInt(), m_cc.got);
                QCOMPARE(m_cc.got, (m_cc.expected + 1) & 0x0F);
            } else if (e.type == "crc") {
                crc = true;
                QCOMPARE(e.pid, 0x1FFB);
                QCOMPARE(e.json.value("tableId").toInt(), 0xC8);
                QVERIFY(e.packet < 200);                  // the first PSIP group
            }
        }
        QVERIFY(cc && crc);
        // The bad TVCT is dropped; the next repetition is shown instead (later in the text than in the clean stream).
        QCOMPARE(an.tablesText().count("* TVCT (ATSC)"), 1);
        QVERIFY(an.tablesText().indexOf("* TVCT (ATSC)") > an.tablesText().indexOf("* STT (ATSC)"));
        const QString report = an.reportText();
        QVERIFY(report.contains("continuity error, expected CC"));
        QVERIFY(report.contains("CRC32 error in TVCT (ATSC) section (TID 0xC8)"));
        const QJsonObject j = an.toJson();
        QCOMPARE(j.value("ccErrors").toInt(), 1);
        QCOMPARE(j.value("crcErrors").toInt(), 1);
    }

    void matchesTspTables_data() {
        QTest::addColumn<QString>("file");
        QTest::newRow("clean") << "clean.ts";
        QTest::newRow("cc-break-bad-crc") << "broken.ts";
    }
    void matchesTspTables() {
        QFETCH(QString, file);
        const QString tsp = tspExe();
        if (tsp.isEmpty()) QSKIP("TSDuck not installed (C:/Program Files/TSDuck/bin or TSDUCK_BIN)");
        QProcess p;
        p.start(tsp, {"-I", "file", m_tmp.filePath(file), "-P", "tables", "-O", "drop"});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 0);
        const QString want = lf(QString::fromUtf8(p.readAllStandardOutput()));
        const TsAnalyzer an = analyse(readAll(m_tmp.filePath(file)));
        QVERIFY(want.count("\n* ") >= 10);
        QVERIFY2(an.tablesText() == want, qPrintable(firstLineDiff(an.tablesText(), want)));
    }

    // ------------------------------------------------------------------ measurement rules on synthetic packets
    void continuityRules() {
        QByteArray ts;
        ts += packet(0x200, 0, true, false);
        ts += packet(0x200, 1, true, false);
        ts += packet(0x200, 1, true, false);              // one duplicate: allowed
        ts += packet(0x200, 2, true, false);
        ts += packet(0x200, 2, true, false);              // duplicate
        ts += packet(0x200, 2, true, false);              // a second repeat: error
        ts += packet(0x200, 3, false, true);              // adaptation only: CC must not move -> error (3 != 2)
        ts += packet(0x200, 3, false, true);              // stays 3: fine
        ts += packet(0x200, 9, true, true, true);         // discontinuity_indicator: any jump accepted
        ts += packet(0x200, 10, true, false);
        ts += packet(0x200, 12, true, false);             // skipped 11: error
        ts += packet(kNullPid, 5, true, false);           // null packets are not checked
        ts += packet(kNullPid, 0, true, false);
        const TsAnalyzer an = analyse(ts);
        const auto* p = pidStats(an, 0x200);
        QCOMPARE(p->packets, qint64(11));
        QCOMPARE(p->duplicates, qint64(2));
        QCOMPARE(p->ccErrors, qint64(3));
        QCOMPARE(p->discontinuities, qint64(1));
        QCOMPARE(pidStats(an, kNullPid)->ccErrors, qint64(0));
        QList<qint64> at;
        for (const TsAnalyzer::Event& e : an.errors()) if (e.type == "cc") at << e.packet;
        QCOMPARE(at, (QList<qint64>{5, 6, 10}));
        QCOMPARE(an.bitrateSource(), QString("none"));
    }

    void pcrJitterDetected() {
        QByteArray ts = m_clean;
        // Move one PCR 100 us (2700 ticks) off the constant-rate line: the intervals before and after it show the jitter.
        const qint64 total = ts.size() / kPacketSize;
        qint64 at = -1;
        for (qint64 i = total / 2; i < total && at < 0; ++i) {
            const quint8* u = reinterpret_cast<const quint8*>(ts.constData() + i * kPacketSize);
            if (packetPid(reinterpret_cast<const char*>(u)) == kVideoPid && (u[3] & 0x20) && u[4] >= 7 && (u[5] & 0x10)) at = i;
        }
        QVERIFY(at > 0);
        quint8* u = reinterpret_cast<quint8*>(ts.data() + at * kPacketSize);
        qint64 base = (qint64(u[6]) << 25) | (qint64(u[7]) << 17) | (qint64(u[8]) << 9) | (qint64(u[9]) << 1) | (u[10] >> 7);
        qint64 pcr = base * 300 + (((u[10] & 1) << 8) | u[11]) + 2700;
        base = pcr / 300;
        const int ext = int(pcr % 300);
        u[6] = quint8(base >> 25); u[7] = quint8(base >> 17); u[8] = quint8(base >> 9); u[9] = quint8(base >> 1);
        u[10] = quint8(((base & 1) << 7) | 0x7E | (ext >> 8)); u[11] = quint8(ext & 0xFF);
        const TsAnalyzer an = analyse(ts);
        const auto* v = pidStats(an, kVideoPid);
        QVERIFY2(v->pcrJitterNs.max > 95000.0 && v->pcrJitterNs.max < 105000.0, qPrintable(QString::number(v->pcrJitterNs.max)));
        QCOMPARE(v->pcrJitterOverMax, qint64(2));
        QVERIFY(an.reportText().contains("(2 over 50.0 us)"));
    }

    void syncLossRecovers() {
        QByteArray ts = m_clean.left(2000 * kPacketSize);
        ts.insert(1000 * kPacketSize, QByteArray(5, '\0'));      // 5 stray bytes before packet 1000: lost sync, then found again
        const TsAnalyzer an = analyse(ts);
        QCOMPARE(an.syncLosses(), qint64(1));
        QCOMPARE(an.packets(), qint64(2000));
        QCOMPARE(an.ccErrors(), qint64(0));
        QCOMPARE(an.errors().first().type, QString("sync"));
        QCOMPARE(an.errors().first().packet, qint64(1000));
    }

    // ------------------------------------------------------------------ UDP input
    void udpSpecParse() {
        UdpSpec s;
        QVERIFY(UdpSpec::parse("239.255.97.1:1234", &s));
        QCOMPARE(s.group, QString("239.255.97.1")); QCOMPARE(s.port, quint16(1234)); QVERIFY(s.isMulticast());
        QVERIFY(UdpSpec::parse("udp://239.1.2.3:5000?pkt_size=1316&localaddr=10.0.0.5", &s));
        QCOMPARE(s.group, QString("239.1.2.3")); QCOMPARE(s.localAddress, QString("10.0.0.5")); QCOMPARE(s.port, quint16(5000));
        QVERIFY(UdpSpec::parse("udp://@239.1.2.3:5000", &s));
        QCOMPARE(s.group, QString("239.1.2.3"));
        QVERIFY(UdpSpec::parse("10.1.1.1@232.1.1.1:4000", &s));
        QCOMPARE(s.source, QString("10.1.1.1")); QCOMPARE(s.toString(), QString("10.1.1.1@232.1.1.1:4000"));
        QVERIFY(UdpSpec::parse(":50070", &s)); QVERIFY(s.group.isEmpty()); QVERIFY(!s.isMulticast());
        QVERIFY(UdpSpec::parse("50070", &s)); QCOMPARE(s.port, quint16(50070));
        QVERIFY(UdpSpec::parse("127.0.0.1:50070", &s)); QVERIFY(!s.isMulticast());
        QString e;
        QVERIFY(!UdpSpec::parse("239.255.0.1", &s, &e));            // no port
        QVERIFY(!UdpSpec::parse("host.example:1234", &s, &e));
        QVERIFY(!UdpSpec::parse("10.0.0.1@10.0.0.2:1234", &s, &e));   // SSM needs a multicast group
        QVERIFY(!UdpSpec::parse("239.255.0.1:70000", &s, &e));
        QVERIFY(!e.isEmpty());
        // RTP header (12 bytes + 1 CSRC) stripped, raw TS untouched.
        const QByteArray tsData = m_clean.left(7 * kPacketSize);
        QByteArray rtp(16, '\0'); rtp[0] = char(0x81); rtp[1] = 33;
        QCOMPARE(udpTsPayload(rtp + tsData), tsData);
        QCOMPARE(udpTsPayload(tsData), tsData);
    }

    void udpUnicastAndMulticast_data() {
        QTest::addColumn<QString>("spec");
        QTest::addColumn<QString>("dest");
        QTest::newRow("unicast") << "127.0.0.1:50071" << "127.0.0.1";
        // Multicast joined on the loopback interface only: nothing reaches the station network.
        QTest::newRow("multicast-loopback") << "udp://239.255.97.250:50072?localaddr=127.0.0.1" << "239.255.97.250";
    }
    void udpUnicastAndMulticast() {
        QFETCH(QString, spec);
        QFETCH(QString, dest);
        UdpSpec s;
        QVERIFY(UdpSpec::parse(spec, &s));
        UdpReceiver rx;
        QString err;
        if (!rx.open(s, 4 * 1024 * 1024, &err)) {
            if (s.isMulticast()) QSKIP(qPrintable("multicast on loopback unavailable: " + err));
            QFAIL(qPrintable(err));
        }
        const QByteArray ts = m_clean;
        std::atomic<bool> sendOk{true};
        std::thread sender([&]() {
            QUdpSocket out;
            if (s.isMulticast()) {
                out.bind(QHostAddress(QHostAddress::AnyIPv4), 0);
                for (const QNetworkInterface& i : QNetworkInterface::allInterfaces())
                    if (i.flags() & QNetworkInterface::IsLoopBack) { out.setMulticastInterface(i); break; }
                out.setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
                out.setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
            }
            const int dg = 7 * kPacketSize;
            for (int off = 0, n = 0; off < ts.size(); off += dg, ++n) {
                if (out.writeDatagram(ts.constData() + off, qMin<qsizetype>(dg, ts.size() - off), QHostAddress(dest), s.port) < 0) sendOk = false;
                if (n % 16 == 15) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        TsAnalyzer an;
        QByteArray d;
        qint64 datagrams = 0;
        for (;;) {
            const int n = rx.receive(&d, datagrams ? 500 : 3000);
            QVERIFY2(n >= 0, qPrintable(rx.lastError()));
            if (n == 0) break;
            ++datagrams;
            an.feed(udpTsPayload(d), UdpReceiver::nowNs());
        }
        sender.join();
        QVERIFY(sendOk);
        if (datagrams == 0 && s.isMulticast()) QSKIP("no multicast delivery on the loopback interface");
        an.finish();
        QCOMPARE(an.packets(), qint64(ts.size() / kPacketSize));
        QCOMPARE(an.ccErrors(), qint64(0));
        QCOMPARE(an.bitrateSource(), QString("pcr"));
        QVERIFY(pidStats(an, kVideoPid)->pcrArrivalJitterMs >= 0);   // arrival times were used
        QCOMPARE(an.tablesText(), analyse(ts).tablesText());
    }

    // ------------------------------------------------------------------ PlayPlayMux analyze
    void cliAnalyzeFile() {
        const QString exe = QStringLiteral(PLAYPLAYMUX_EXE);
        QVERIFY(QFileInfo::exists(exe));
        QProcess p;
        p.start(exe, {"analyze", m_tmp.filePath("broken.ts")});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 0);
        const QByteArray out = p.readAllStandardOutput();
        const QJsonObject j = lastJson(out);
        QVERIFY(j.value("ok").toBool());
        QCOMPARE(j.value("input").toString(), QString("file"));
        QCOMPARE(j.value("mode").toString(), QString("tables"));
        QCOMPARE(j.value("packets").toDouble(), double(m_broken.size() / kPacketSize));
        QCOMPARE(j.value("ccErrors").toInt(), 1);
        QCOMPARE(j.value("crcErrors").toInt(), 1);
        QCOMPARE(j.value("services").toArray().size(), 1);
        QVERIFY(j.value("pids").toArray().size() >= 9);
        const QString text = QString::fromUtf8(out.left(out.lastIndexOf('\n', out.size() - 2) + 1));
        QCOMPARE(text, analyse(m_broken).tablesText());   // stdout text then the JSON line

        p.start(exe, {"analyze", "--mode", "analyze", m_tmp.filePath("clean.ts")});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 0);
        const QString report = QString::fromUtf8(p.readAllStandardOutput());
        QVERIFY(report.contains("* PIDs"));
        QVERIFY(report.contains("* PCR"));
        QVERIFY(report.contains("Channel 25.1 \"WSTR\""));
        QVERIFY(!report.contains("* PAT, TID"));

        p.start(exe, {"analyze", "--pid", "0x1FFB", "-x", "2", "--quiet", m_tmp.filePath("clean.ts")});
        QVERIFY(p.waitForFinished(60000));
        const QByteArray q = p.readAllStandardOutput();
        QCOMPARE(q.count('\n'), 1);                      // --quiet: the JSON line only
        QCOMPARE(lastJson(q).value("tablesDisplayed").toInt(), 2);
        QVERIFY(lastJson(q).value("packets").toDouble() < double(m_clean.size() / kPacketSize));   // tables mode stops at -x

        p.start(exe, {"analyze", "--mode", "fancy", m_tmp.filePath("clean.ts")});
        QVERIFY(p.waitForFinished(60000));
        QCOMPARE(p.exitCode(), 64);
        QVERIFY(lastJson(p.readAllStandardOutput()).value("unsupported").toBool());
    }

    void cliAnalyzeUdp() {
        const QString exe = QStringLiteral(PLAYPLAYMUX_EXE);
        QElapsedTimer t;
        t.start();
        QProcess p;
        p.start(exe, {"analyze", "127.0.0.1:50073", "--receive-timeout", "300", "--seconds", "5"});
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 2);
        const QJsonObject j = lastJson(p.readAllStandardOutput());
        QVERIFY(!j.value("ok").toBool());
        QVERIFY(j.value("receiveTimeout").toBool());
        QVERIFY(t.elapsed() < 4000);

        // A live run: the tool listens 3 s while this test sends the stream (starting after the socket is bound).
        p.start(exe, {"analyze", "--mode", "both", "--seconds", "4", "--receive-timeout", "3000", "127.0.0.1:50074"});
        QVERIFY(p.waitForStarted(10000));
        QThread::msleep(700);
        QUdpSocket out;
        for (int off = 0, n = 0; off < m_clean.size(); off += 7 * kPacketSize, ++n) {
            out.writeDatagram(m_clean.constData() + off, qMin<qsizetype>(7 * kPacketSize, m_clean.size() - off), QHostAddress("127.0.0.1"), 50074);
            if (n % 16 == 15) QThread::msleep(1);
        }
        QVERIFY(p.waitForFinished(30000));
        QCOMPARE(p.exitCode(), 0);
        const QByteArray o = p.readAllStandardOutput();
        const QJsonObject r = lastJson(o);
        QVERIFY(r.value("ok").toBool());
        QCOMPARE(r.value("input").toString(), QString("udp"));
        QVERIFY(r.value("datagrams").toDouble() > 100);
        QVERIFY(r.value("tablesDisplayed").toInt() >= 8);
        QVERIFY(QString::fromUtf8(o).contains("* TVCT (ATSC)"));
        QVERIFY(QString::fromUtf8(o).contains("* PIDs"));
    }
};

QTEST_GUILESS_MAIN(TestTsAnalyzer)
#include "test_inhouse_tsanalyzer.moc"
