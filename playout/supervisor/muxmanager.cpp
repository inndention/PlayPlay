#include "muxmanager.h"
#include "scte35.h"
#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"
#include "../../tools/avkit/avcaps.h"      // PlayPlayAvKit: the encoder list / build flags of capabilities() in-process (DEP-02b)
#include "../../tools/mux/psip.h"          // PlayPlayTsKit: the PSIP / PAT / SCTE-35 XML compiler, in-process (DEP-14 / DEP-15)

#include <QDir>
#include <QElapsedTimer>
#include <QPointer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QUrl>
#include <QTimeZone>
#include <QUdpSocket>
#include <QHostAddress>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winver.h>     // GetFileVersionInfoW (version.lib): tsp.exe's version without starting it
#else
#include <cstdio>       // std::rename (replaceFile)
#endif

using namespace sched;

// The Supervisor's MuxManager, for the async routes the lane-extension registry installs once every manager exists
// (supervisorcore.cpp: laneext::installAll after registerRoutes, so these handlers replace the blocking ones).
static QPointer<MuxManager>& primaryMux() { static QPointer<MuxManager> p; return p; }
MuxManager* MuxManager::primary() { return primaryMux().data(); }
static laneext::Registrar s_muxAsyncRoutes("mux-async", 10, [](const laneext::Context& ctx) { MuxManager::installAsyncRoutes(ctx.rest, MuxManager::primary()); });

static const int kEitWindowHours = 3;
static const double kPacketsPerSec = 19392658.0 / 1504.0;     // TS packets per second at the ATSC rate (for --inter-packet)
static QString eitPid(int k) { return QString("0x%1").arg(0x0200 + 2 * k, 4, 16, QChar('0')).toUpper().replace("0X", "0x"); }
static QString ettPid(int k) { return QString("0x%1").arg(0x0201 + 2 * k, 4, 16, QChar('0')).toUpper().replace("0X", "0x"); }
static QString xmlEsc(QString s) { return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;"); }
static QString hex4(int v) { return QString("0x%1").arg(v, 4, 16, QChar('0')); }

// Channel-count presets (broadcast_gui core/channel_presets.py): resolution/bitrate per tier so
// N * (video + audio) * 1.20 stays under the transport minus the PSIP reserve.
struct Tier { int w, h, vid, aud, smin, smax; };
static Tier tierFor(int n) {
    if (n <= 2) return {1280, 720, 5000, 192, 3000, 6000};
    if (n <= 4) return {1280, 720, 3000, 192, 2000, 4000};
    if (n <= 6) return {1280, 720, 2000, 192, 1200, 2500};
    if (n <= 8) return {960, 540, 1500, 128, 900, 1900};
    if (n <= 10) return {854, 480, 1200, 128, 700, 1500};
    return {640, 360, 1000, 128, 600, 1300};
}

// ---------------------------------------------------------------------------------------------
// Service encoder view: the advanced shape (encoder.audio / encoder.ts) with the legacy flat fields
// (audCodec, audBitrateKbps, audChannels) as fallbacks.
// ---------------------------------------------------------------------------------------------
struct EncView {
    QString encPreset, codec, fps, rc, preset, profile, level, extra;
    int w = 1280, h = 720, vidKbps = 3000, maxKbps = 0, bufKbps = 0, qp = 23, gop = 30, kmin = 0, bframes = 2, afd = -1;
    bool interlaced = false, closedGop = true, scenecut = false, cc = true;
    QString audCodec = "ac3", aacProfile = "aac_low"; int audKbps = 192, audCh = 2, audRate = 48000, dialnorm = -27, mixingLevel = -1; bool audNormalize = false;
    QJsonObject ts;
};
static EncView encView(const QJsonObject& svc) {
    const QJsonObject e = svc.value("encoder").toObject(); const QJsonObject au = e.value("audio").toObject();
    EncView v;
    v.encPreset = e.value("encoderPreset").toString();
    v.codec = e.value("codec").toString("libx264"); v.w = e.value("width").toInt(1280); v.h = e.value("height").toInt(720); v.fps = e.value("fps").toString("30000/1001");
    v.vidKbps = e.value("vidBitrateKbps").toInt(3000); v.maxKbps = e.value("maxrateKbps").toInt(0); v.bufKbps = e.value("bufsizeKbps").toInt(0);
    v.rc = e.value("rateControl").toString().toLower(); if (v.rc != "cbr" && v.rc != "vbr" && v.rc != "cqp") v.rc = "cbr";
    v.qp = e.value("qp").toInt(23); v.gop = e.value("gop").toInt(e.value("gopFrames").toInt(30)); v.kmin = e.value("keyintMin").toInt(0);
    v.bframes = e.contains("bframes") ? e.value("bframes").toInt() : e.value("bFrames").toInt(2);
    v.closedGop = e.value("closedGop").toBool(true); v.scenecut = e.value("scenecut").toBool(false);
    v.preset = e.value("preset").toString("veryfast"); v.profile = e.value("profile").toString("high"); v.level = e.value("level").isString() ? e.value("level").toString("4.1") : e.contains("level") ? QString::number(e.value("level").toDouble()) : "4.1";
    v.interlaced = e.value("interlaced").toBool(false); v.afd = e.value("afd").toInt(-1); v.cc = e.value("ccPassthrough").toBool(true);
    v.extra = e.value("extra").isString() ? e.value("extra").toString() : QString();
    v.audCodec = au.value("codec").toString(e.value("audCodec").toString("ac3")); v.audKbps = au.value("bitrateKbps").toInt(e.value("audBitrateKbps").toInt(192));
    v.audCh = au.value("channels").toInt(e.value("audChannels").toInt(2)); v.audRate = au.value("sampleRate").toInt(48000);
    v.dialnorm = qBound(-31, au.value("dialnorm").toInt(-27), -1); v.mixingLevel = au.value("mixingLevel").toInt(-1); v.aacProfile = au.value("aacProfile").toString("aac_low"); v.audNormalize = au.value("normalize").toBool(false);
    v.ts = e.value("ts").toObject();
    return v;
}

// Service-encoder shaped presets (ids match EncoderProfile::preset so channel services and ffmpeg inputs agree).
QJsonArray MuxManager::encoderPresets() {
    auto mk = [](const char* id, const char* label, const char* codec, int w, int h, int vid, const char* rc, int maxk, int bufk, int gop, int bf, const char* preset, const char* profile, const char* level, bool il, const char* ac, int ak, int dn, const char* note) {
        QJsonObject o; o["id"] = id; o["label"] = label; o["codec"] = codec; o["width"] = w; o["height"] = h; o["fps"] = "30000/1001"; o["vidBitrateKbps"] = vid; o["rateControl"] = rc;
        if (maxk) o["maxrateKbps"] = maxk; if (bufk) o["bufsizeKbps"] = bufk; o["gop"] = gop; o["bframes"] = bf; o["preset"] = preset; o["profile"] = profile; o["level"] = level; o["interlaced"] = il; o["closedGop"] = true; o["scenecut"] = false; o["ccPassthrough"] = true;
        QJsonObject au; au["codec"] = ac; au["bitrateKbps"] = ak; au["channels"] = 2; au["sampleRate"] = 48000; au["dialnorm"] = dn; if (QString(ac) == "aac") au["aacProfile"] = "aac_low"; o["audio"] = au; o["note"] = note; return o; };
    QJsonArray a;
    a.append(mk("atsc_mpeg2_ac3_hd", "ATSC MPEG-2 1080i MP@HL 12 Mb/s + AC-3 384k", "mpeg2video", 1920, 1080, 12000, "cbr", 12000, 1835, 15, 2, "veryfast", "main", "4.1", true, "ac3", 384, -27, "Legacy-receiver safe main service. Needs most of the transport: pair with SD sub-channels."));
    a.append(mk("atsc_mpeg2_ac3_sd", "ATSC MPEG-2 480i MP@ML 4 Mb/s + AC-3 192k", "mpeg2video", 704, 480, 4000, "cbr", 4000, 1835, 15, 2, "veryfast", "main", "3", true, "ac3", 192, -27, "Classic SD sub-channel (704x480 anamorphic 16:9)."));
    a.append(mk("atsc_h264_ac3", "ATSC H.264 high@4.1 6 Mb/s CBR + AC-3 384k", "libx264", 1280, 720, 6000, "cbr", 6000, 6000, 30, 2, "veryfast", "high", "4.1", false, "ac3", 384, -27, "H.264 sub-channel (A/72); not decoded by pre-2010 receivers."));
    a.append(mk("stream_h264_aac_1080p", "Stream H.264 1080p 6 Mb/s VBR + AAC 160k", "libx264", 1920, 1080, 6000, "vbr", 7000, 12000, 60, 2, "veryfast", "high", "4.1", false, "aac", 160, -27, "RTMP/SRT/HLS origin feed."));
    a.append(mk("stream_h264_aac_720p", "Stream H.264 720p 3.5 Mb/s VBR + AAC 128k", "libx264", 1280, 720, 3500, "vbr", 4000, 7000, 60, 2, "veryfast", "main", "3.1", false, "aac", 128, -27, "Social/RTMP ingest friendly."));
    a.append(mk("stream_hevc_aac", "Stream HEVC 4 Mb/s VBR + AAC 128k", "libx265", 1920, 1080, 4000, "vbr", 5000, 8000, 60, 2, "veryfast", "main", "4.1", false, "aac", 128, -27, "Contribution/OTT; not for ATSC 1.0."));
    a.append(mk("qsv_h264_aac", "Intel QSV H.264 6 Mb/s CBR + AAC 160k", "h264_qsv", 1920, 1080, 6000, "cbr", 6000, 12000, 60, 2, "veryfast", "high", "4.1", false, "aac", 160, -27, "Hardware encode; falls back to libx264 when this FFmpeg build has no QSV."));
    a.append(mk("mpeg2_ac3", "MPEG-2 + AC-3 (legacy preset name)", "mpeg2video", 1920, 1080, 12000, "cbr", 12000, 1835, 15, 2, "veryfast", "main", "4.1", false, "ac3", 384, -27, "Same as atsc_mpeg2_ac3_hd, progressive."));
    a.append(mk("h264_aac", "H.264 + AAC 8 Mb/s (legacy preset name)", "libx264", 1920, 1080, 8000, "cbr", 8000, 16000, 30, 2, "veryfast", "high", "4.1", false, "aac", 192, -27, ""));
    a.append(mk("h264_aac_low", "H.264 main + AAC 2.5 Mb/s (legacy)", "libx264", 1280, 720, 2500, "cbr", 2500, 5000, 30, 2, "veryfast", "main", "3.1", false, "aac", 128, -27, ""));
    return a;
}

MuxManager::MuxManager(ScheduleStore* store, const QString& dataDir, QObject* parent) : QObject(parent), m_store(store), m_dataDir(dataDir) {
    if (!primaryMux()) primaryMux() = this;
    load();
    QDir().mkpath(tablesDir());
    m_nextEventId = quint32(QDateTime::currentSecsSinceEpoch() & 0x3FFFFFFF);   // splice_event_id stays unique across restarts
    m_heartbeat.setInterval(1000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        const QJsonObject c = scte35Config(); const int every = c.value("heartbeatSec").toInt(0);
        if (!running() || every <= 0 || !c.value("enabled").toBool(false)) return;
        const qint64 t = QDateTime::currentMSecsSinceEpoch();
        for (int i = 0; i < m_services.size(); ++i) {
            const QJsonObject svc = m_services[i].toObject(); if (!scte35::serviceEnabled(c, svc)) continue;
            BreakState& b = m_breaks[svc.value("id").toString()];
            if (t - b.lastCueMs >= qint64(every) * 1000) sendCue(svc, i + 1, "null", 0, false, "heartbeat");
        }
    });
    m_heartbeat.start();
    connect(&m_tableTimer, &QTimer::timeout, this, [this]() { if (running()) { QString e; writeTables(&e); writeStt(); } });
    m_tableTimer.start(10 * 60 * 1000);
    QTimer* stt = new QTimer(this); connect(stt, &QTimer::timeout, this, [this]() { if (running()) writeStt(); });
    stt->start(qBound(1, tableOverrides().value("sttIntervalSec").toInt(5), 60) * 1000);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        if (!m_wantRunning) return;
        // metrics silence = tsp wedged
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_state == "running" && m_metrics.value("lastMetricMs").toDouble() > 0 && now - qint64(m_metrics.value("lastMetricMs").toDouble()) > 30000) {
            raise("critical", "mux: no bitrate reports for 30 s, restarting tsp", "mux_silent");
            if (m_tsp) m_tsp->kill();
        }
        for (Encoder* e : m_encoders) if (e->proc && e->proc->state() == QProcess::NotRunning) {
            if (now - e->startedMs > 3000) { e->restarts++; raise(e->restarts >= 3 ? "critical" : "warning", QString("mux: encoder for service %1 exited (restart #%2)").arg(e->serviceId).arg(e->restarts), "enc_" + e->serviceId); e->proc->deleteLater(); e->proc = nullptr; }
        }
        for (Encoder* e : m_encoders) if (!e->proc) {
            for (const QJsonValue& v : m_services) if (v.toObject().value("id").toString() == e->serviceId) {
                QString err; const QStringList args = ffmpegArgs(v.toObject(), &err);
                if (args.isEmpty()) { appendLog("encoder " + e->serviceId + ": " + err); break; }
                e->proc = new QProcess(this); e->startedMs = now;
                connect(e->proc, &QProcess::readyReadStandardError, this, [this, e]() { const QString s = QString::fromUtf8(e->proc->readAllStandardError()).trimmed(); if (!s.isEmpty()) { e->lastLine = s.section('\n', -1); } });
                e->proc->start(ffmpeg(), args); if (e->proc->waitForStarted(3000)) adopt(e->proc);
                appendLog("encoder " + e->serviceId + " restarted");
                break;
            }
        }
    });
    m_watchdog.start(3000);
    if (m_cfg.value("autoStart").toBool(false) && m_cfg.value("enabled").toBool(true)) { m_autoAttempt = 0; scheduleAutoStart(); }
}

MuxManager::~MuxManager() { stop();
    if (QProcess* p = m_dektecProbe) { m_dektecProbe = nullptr; p->disconnect(this); p->kill(); p->waitForFinished(1000); }   // a listing; nothing to keep
    m_dektecWaiters.clear();
#ifdef Q_OS_WIN
    if (m_job) CloseHandle(static_cast<HANDLE>(m_job));
#endif
}

// Auto-start ~3 s after construction; retry with backoff 3, 6, 12, 24, 48 s; critical alert after the 5th failure.
void MuxManager::scheduleAutoStart() {
    const int delayMs = (m_autoAttempt == 0 ? 3 : 3 << qMin(m_autoAttempt, 4)) * 1000;
    QTimer::singleShot(delayMs, this, [this]() {
        if (running() || !m_cfg.value("autoStart").toBool(false)) return;
        // startAsync: with a DekTec output the start completes after the asynchronous port setup (or fails there)
        startAsync([this](bool ok, const QString& e) {
            if (ok) { appendLog(QString("autostart: mux started (attempt %1)").arg(m_autoAttempt + 1)); m_autoAttempt = 0; return; }
            if (m_state == "stopped") return;    // stopped by the operator while the port was being set up: not a failure to retry
            ++m_autoAttempt;
            appendLog(QString("autostart attempt %1 failed: %2").arg(m_autoAttempt).arg(e));
            if (m_autoAttempt >= 5) { raise("critical", "mux: auto-start gave up after 5 attempts: " + e, "mux_autostart"); return; }
            raise("warning", QString("mux: auto-start failed (%1), retrying in %2 s").arg(e).arg(3 << qMin(m_autoAttempt, 4)), "mux_autostart");
            scheduleAutoStart();
        });
    });
}

// Put a child into a kill-on-close job so a crashed or killed Supervisor never leaves encoders
// multicasting into the mux groups (two senders on one group doubles the bitrate and breaks continuity).
void MuxManager::adopt(QProcess* p) {
#ifdef Q_OS_WIN
    if (!p || p->processId() == 0) return;
    if (!m_job) {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{}; li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(h, JobObjectExtendedLimitInformation, &li, sizeof(li)); m_job = h; }
    }
    if (!m_job) return;
    HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(p->processId()));
    if (ph) { AssignProcessToJobObject(static_cast<HANDLE>(m_job), ph); CloseHandle(ph); }
#else
    Q_UNUSED(p);
#endif
}

// ---------------------------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------------------------
void MuxManager::load() {
    m_cfg = m_store ? m_store->settingJson("mux.config") : QJsonObject();
    QJsonObject o = m_cfg.value("output").toObject();
    if (!o.contains("kind")) o["kind"] = "dektec"; if (!o.contains("device")) o["device"] = 0; if (!o.contains("port")) o["port"] = 2; if (!o.contains("channel")) o["channel"] = 0;
    if (!o.contains("url")) o["url"] = "udp://239.1.1.1:1234?pkt_size=1316"; if (!o.contains("bitrate")) o["bitrate"] = double(kAtscBps);
    if (!o.contains("preloadMs")) o["preloadMs"] = 4000; if (!o.contains("maintainPreload")) o["maintainPreload"] = true; if (!o.contains("stuffing")) o["stuffing"] = true;
    m_cfg["output"] = o;
    if (!m_cfg.contains("tsid")) m_cfg["tsid"] = 0x19;
    if (!m_cfg.contains("timezone")) m_cfg["timezone"] = "America/New_York";
    if (!m_cfg.contains("statmux")) { QJsonObject s; s["enabled"] = true; s["reservedBps"] = 900000; s["defaultMinKbps"] = 700; s["defaultMaxKbps"] = 1300; m_cfg["statmux"] = s; }
    if (!m_cfg.contains("tsduckDir")) m_cfg["tsduckDir"] = "C:/Program Files/TSDuck/bin";
    if (!m_cfg.contains("mcastBase")) m_cfg["mcastBase"] = "239.255.25.";
    if (!m_cfg.contains("alerts")) { QJsonObject a; a["bitrateDeviationPct"] = 12; a["restartThreshold"] = 3; a["checkFifo"] = true; m_cfg["alerts"] = a; }
    m_services = m_store ? m_store->settingJson("mux.services").value("services").toArray() : QJsonArray();
}
void MuxManager::persist() {
    if (!m_store) return;
    m_store->setSettingJson("mux.config", m_cfg);
    QJsonObject s; s["services"] = m_services; m_store->setSettingJson("mux.services", s);
}
QJsonObject MuxManager::config() const { return m_cfg; }
void MuxManager::setConfig(const QJsonObject& cfg) {
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
        if (it.key() == "output" && it.value().isObject()) { QJsonObject o = m_cfg.value("output").toObject(); const QJsonObject n = it.value().toObject(); for (auto j = n.begin(); j != n.end(); ++j) o[j.key()] = j.value(); m_cfg["output"] = o; }
        else if (it.key() == "statmux" && it.value().isObject()) { QJsonObject o = m_cfg.value("statmux").toObject(); const QJsonObject n = it.value().toObject(); for (auto j = n.begin(); j != n.end(); ++j) o[j.key()] = j.value(); m_cfg["statmux"] = o; }
        else m_cfg[it.key()] = it.value();
    }
    persist(); emit statusChanged();
}
QJsonArray MuxManager::services() const { return m_services; }
qint64 MuxManager::transportBps() const { const qint64 b = qint64(m_cfg.value("output").toObject().value("bitrate").toDouble(double(kAtscBps))); return b >= 1000000 ? b : kAtscBps; }

void MuxManager::setServices(const QJsonArray& services) {
    QJsonArray out; int i = 0;
    for (const QJsonValue& v : services) {
        QJsonObject s = v.toObject(); ++i;
        if (s.value("id").toString().isEmpty()) s["id"] = QString("svc%1").arg(i);
        if (!s.contains("major")) s["major"] = 25;
        if (!s.contains("minor")) s["minor"] = i;
        if (!s.contains("serviceId")) s["serviceId"] = 100 + i;
        if (!s.contains("vidPid")) s["vidPid"] = 48 + 2 * i;
        if (!s.contains("audPid")) s["audPid"] = 49 + 2 * i;
        if (!s.contains("udpPort")) s["udpPort"] = 1230 + i;
        if (s.value("shortName").toString().isEmpty()) s["shortName"] = QString("%1-%2").arg(s.value("major").toInt()).arg(s.value("minor").toInt());
        if (s.value("longName").toString().isEmpty()) s["longName"] = QString("%1.%2").arg(s.value("major").toInt()).arg(s.value("minor").toInt());
        // encoder: defaults from the tier, optional named preset merge (encoder.applyPreset = id)
        QJsonObject e = s.value("encoder").toObject();
        if (e.isEmpty()) { const Tier t = tierFor(services.size()); e["codec"] = "libx264"; e["width"] = t.w; e["height"] = t.h; e["fps"] = "30000/1001"; e["vidBitrateKbps"] = t.vid; e["gop"] = 30; e["bframes"] = 2; e["preset"] = "veryfast"; e["profile"] = "high"; e["level"] = "4.1"; e["rateControl"] = "cbr"; QJsonObject au; au["codec"] = "ac3"; au["bitrateKbps"] = t.aud; au["channels"] = 2; au["dialnorm"] = -27; e["audio"] = au; }
        if (!e.value("applyPreset").toString().isEmpty()) {
            const QString pid = e.take("applyPreset").toString();
            for (const QJsonValue& pv : encoderPresets()) { const QJsonObject p = pv.toObject(); if (p.value("id").toString() != pid) continue;
                for (auto it = p.begin(); it != p.end(); ++it) if (it.key() != "id" && it.key() != "label" && it.key() != "note") e[it.key()] = it.value();
                e["encoderPreset"] = pid; e.remove("audCodec"); e.remove("audBitrateKbps"); e.remove("audChannels"); break; }
        }
        if (!e.contains("audio") && (e.contains("audCodec") || e.contains("audBitrateKbps"))) { QJsonObject au; au["codec"] = e.value("audCodec").toString("ac3"); au["bitrateKbps"] = e.value("audBitrateKbps").toInt(192); au["channels"] = e.value("audChannels").toInt(2); au["dialnorm"] = -27; e["audio"] = au; }
        // keep the legacy flat fields mirrored for older readers (QML list text, tier maths)
        const QJsonObject au = e.value("audio").toObject(); if (!au.isEmpty()) { e["audCodec"] = au.value("codec").toString("ac3"); e["audBitrateKbps"] = au.value("bitrateKbps").toInt(192); e["audChannels"] = au.value("channels").toInt(2); }
        const QJsonObject ts = e.value("ts").toObject();
        if (ts.contains("vidPid")) s["vidPid"] = ts.value("vidPid"); if (ts.contains("audPid")) s["audPid"] = ts.value("audPid"); if (ts.contains("serviceId")) s["serviceId"] = ts.value("serviceId");
        s["encoder"] = e;
        if (!s.contains("statmux")) { const Tier t = tierFor(services.size()); QJsonObject m; m["managed"] = true; m["minKbps"] = t.smin; m["maxKbps"] = t.smax; m["weight"] = 100; m["priority"] = "normal"; s["statmux"] = m; }
        // source: normalise {kind:"input", input:{type:"ndi"}} to the ndi kind
        QJsonObject src = s.value("source").toObject();
        if (src.isEmpty()) { src["kind"] = "input"; QJsonObject in; in["type"] = "test"; in["location"] = ""; src["input"] = in; }
        if (src.value("kind").toString() == "input" && src.value("input").toObject().value("type").toString() == "ndi") { src["kind"] = "ndi"; if (src.value("ndiName").toString().isEmpty()) src["ndiName"] = src.value("input").toObject().value("location").toString(); }
        if (src.value("kind").toString() == "ndi" && !src.contains("audio")) src["audio"] = true;
        s["source"] = src;
        out.append(s);
    }
    m_services = out; persist();
    QString e; m_alloc = computeAllocation(&e); if (!e.isEmpty()) appendLog("statmux: " + e);
    emit servicesChanged(); emit statusChanged();
}

QJsonObject MuxManager::applyPreset(int count, bool force) {
    const Tier t = tierFor(count);
    QJsonArray out; int kept = 0, dropped = 0, added = 0;
    for (int i = 0; i < m_services.size() && (i < count || !force); ++i) {
        if (i >= count) { const QJsonObject s = m_services[i].toObject(); const bool real = s.value("source").toObject().value("kind").toString() != "input" || s.value("source").toObject().value("input").toObject().value("type").toString() != "test"; if (real && !force) { out.append(s); ++kept; continue; } ++dropped; continue; }
        QJsonObject s = m_services[i].toObject();
        QJsonObject e = s.value("encoder").toObject(); e["width"] = t.w; e["height"] = t.h; e["vidBitrateKbps"] = t.vid; e["audBitrateKbps"] = t.aud; QJsonObject au = e.value("audio").toObject(); au["bitrateKbps"] = t.aud; e["audio"] = au; s["encoder"] = e;
        QJsonObject m = s.value("statmux").toObject(); m["minKbps"] = t.smin; m["maxKbps"] = t.smax; s["statmux"] = m;
        out.append(s); ++kept;
    }
    for (int i = out.size(); i < count; ++i) { QJsonObject s; s["id"] = QString("svc%1").arg(i + 1); s["minor"] = i + 1; out.append(s); ++added; }
    setServices(out);
    QJsonObject r; r["count"] = count; r["kept"] = kept; r["dropped"] = dropped; r["added"] = added; r["tier"] = QJsonObject{{"width", t.w}, {"height", t.h}, {"vidKbps", t.vid}, {"audKbps", t.aud}}; return r;
}

// ---------------------------------------------------------------------------------------------
// Statmux (bounded weighted water-filling, from core/statmux.py)
// ---------------------------------------------------------------------------------------------
QJsonObject MuxManager::computeAllocation(QString* error) const {
    QJsonObject alloc; const QJsonObject sm = m_cfg.value("statmux").toObject();
    if (m_services.isEmpty()) return alloc;
    if (!sm.value("enabled").toBool(true)) { for (const QJsonValue& v : m_services) { const QJsonObject s = v.toObject(); alloc[s.value("id").toString()] = double(encView(s).vidKbps * 1000); } return alloc; }
    qint64 audio = 0; for (const QJsonValue& v : m_services) audio += qint64(encView(v.toObject()).audKbps) * 1000;
    qint64 budget = transportBps() - qint64(sm.value("reservedBps").toInt(900000)) - audio;
    if (budget <= 0) { if (error) *error = "statmux reserve and audio exceed the transport budget"; return alloc; }
    struct M { QString id; qint64 min, max; int weight; qint64 cur = 0; };
    QList<M> managed; qint64 fixed = 0;
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); const QJsonObject st = s.value("statmux").toObject(); const QString id = s.value("id").toString();
        if (!st.value("managed").toBool(true)) { const qint64 b = qint64(encView(s).vidKbps) * 1000; alloc[id] = double(b); fixed += b; continue; }
        M m; m.id = id; m.min = qMax<qint64>(100000, qint64(st.value("minKbps").toInt(sm.value("defaultMinKbps").toInt(700))) * 1000); m.max = qMax(m.min, qint64(st.value("maxKbps").toInt(sm.value("defaultMaxKbps").toInt(1300))) * 1000);
        const int w = st.value("weight").toInt(100); const QString pr = st.value("priority").toString("normal");
        m.weight = w != 100 ? w : (pr == "high" ? 200 : pr == "low" ? 50 : 100); m.weight = qMax(1, m.weight); m.cur = m.min; managed.append(m);
    }
    qint64 remaining = budget - fixed; qint64 minReq = 0; for (const M& m : managed) minReq += m.min;
    if (managed.isEmpty()) return alloc;
    if (minReq > remaining) { if (error) *error = QString("statmux minimums need %1 b/s but only %2 b/s are available").arg(minReq).arg(remaining); for (const M& m : managed) alloc[m.id] = double(m.min); return alloc; }
    remaining -= minReq;
    QList<M*> active; for (M& m : managed) active.append(&m);
    while (remaining > 0 && !active.isEmpty()) {
        qint64 tw = 0; for (M* m : active) tw += m->weight;
        qint64 distributed = 0; QList<M*> next;
        for (M* m : active) { const qint64 head = m->max - m->cur; const qint64 share = qMax<qint64>(1, remaining * m->weight / tw); const qint64 grant = qMin(head, share); m->cur += grant; distributed += grant; if (grant < head) next.append(m); }
        if (distributed <= 0) break; remaining -= distributed; active = next;
    }
    for (const M& m : managed) alloc[m.id] = double(qMax<qint64>(100000, m.cur / 10000 * 10000));
    return alloc;
}
QJsonObject MuxManager::allocation() const { QString e; return computeAllocation(&e); }

// ---------------------------------------------------------------------------------------------
// Engine outputs for `channel` / `ndi` services, hidden channels for `ndi` services
// ---------------------------------------------------------------------------------------------
QString MuxManager::svcMcast(int index) { return QString("239.255.25.%1").arg(index); }
int MuxManager::svcIndex(const QString& serviceId) const { for (int i = 0; i < m_services.size(); ++i) if (m_services[i].toObject().value("id").toString() == serviceId) return i + 1; return 1; }
QString MuxManager::svcMcastAddr(const QJsonObject& svc) const { const int i = svcIndex(svc.value("id").toString()); return m_cfg.value("mcastBase").toString("239.255.25.") + QString::number(i) + ":" + QString::number(svc.value("udpPort").toInt(1230 + i)); }
QString MuxManager::svcMcastUrl(const QJsonObject& svc) const { return "udp://" + svcMcastAddr(svc) + "?pkt_size=1316&ttl=1"; }

QString MuxManager::feedChannelFor(const QJsonObject& svc) {
    const QJsonObject src = svc.value("source").toObject(); const QString kind = src.value("kind").toString();
    if (kind == "channel") return src.value("channelId").toString();
    if (kind == "ndi" || inputViaEngine(svc)) return "mux-" + svc.value("id").toString();
    return QString();
}

// DEP-18: network inputs are pulled by an engine (FileSource, realtime) instead of an ffmpeg.exe per service. The engine's
// EncoderStage applies every encoder field (closed GOP and interlaced coding included), the hidden channel gets the slate
// and the executor's re-arm when the source drops, and one FFmpeg copy does it all. lavfi test sources, looping files and
// dshow devices stay on ffmpeg until the engine's generator / file loop / dshow paths are verified for the mux.
bool MuxManager::inputViaEngine(const QJsonObject& svc) {
    const QJsonObject src = svc.value("source").toObject(); if (src.value("kind").toString() != "input") return false;
    const QJsonObject in = src.value("input").toObject();
    if (in.value("feed").toString() == "ffmpeg" || in.value("location").toString().trimmed().isEmpty()) return false;
    static const QSet<QString> kNet = {"rtsp", "srt", "rtmp", "udp", "http", "https"};
    return kNet.contains(in.value("type").toString());
}

QJsonObject MuxManager::fixedSourceFor(const QJsonObject& svc) {
    const QJsonObject src = svc.value("source").toObject(); QJsonObject f;
    if (src.value("kind").toString() == "ndi") {
        f["type"] = "ndi"; f["name"] = src.value("ndiName").toString(); f["ndiName"] = f["name"];
        f["title"] = src.value("ndiTitle").toString(svc.value("longName").toString(feedChannelFor(svc))); f["audio"] = src.value("audio").toBool(true);
        return f;
    }
    if (!inputViaEngine(svc)) return f;
    const QJsonObject in = src.value("input").toObject(); const QString type = in.value("type").toString();
    f["type"] = "stream"; f["url"] = in.value("location").toString().trimmed(); f["realtime"] = true;
    f["title"] = svc.value("longName").toString(feedChannelFor(svc));
    // the ffmpeg path's input options: a stalled network read ends after 10 s (the executor re-arms the item), RTSP over TCP
    QJsonObject opts; opts["rw_timeout"] = "10000000";
    if (type == "rtsp") opts["rtsp_transport"] = in.value("transport").toString("tcp");
    f["inputOptions"] = opts;
    f["firstFrameTimeoutMs"] = 8000;     // network pulls need longer than NDI for the first picture (bounded at 15 s by the engine)
    return f;
}

// URLs in log lines without user info (rtsp://user:pass@cam/...): logs are not a place for credentials.
static QString safeUrl(const QString& u) { const QUrl q(u); return q.isValid() && !q.scheme().isEmpty() ? q.toDisplayString(QUrl::RemoveUserInfo) : QString("<url>"); }

QJsonArray MuxManager::engineOutputsFor(const QString& channelId, const QJsonArray& existing) const {
    QJsonArray out; for (const QJsonValue& v : existing) if (v.toObject().value("name").toString().startsWith("mux:") == false) out.append(v);
    const QJsonObject alloc = m_alloc.isEmpty() ? allocation() : m_alloc;
    const bool statmuxOn = m_cfg.value("statmux").toObject().value("enabled").toBool(true);
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject();
        if (feedChannelFor(s) != channelId || channelId.isEmpty()) continue;
        const EncView e = encView(s);
        const bool managed = statmuxOn && s.value("statmux").toObject().value("managed").toBool(true);
        const int vid = int(alloc.value(s.value("id").toString()).toDouble(e.vidKbps * 1000) / 1000);
        QJsonObject o; o["type"] = "udp"; o["name"] = "mux:" + s.value("id").toString(); o["url"] = svcMcastUrl(s);
        o["enabled"] = true; o["autoStart"] = true; o["managedBy"] = "mux"; o["serviceId"] = s.value("id");
        QJsonObject retry; retry["enabled"] = true; retry["maxAttempts"] = 0; retry["backoffSec"] = 3; o["retry"] = retry;
        QJsonObject p; p["name"] = "mux_" + s.value("id").toString(); if (!e.encPreset.isEmpty()) p["preset"] = e.encPreset;
        p["videoCodec"] = e.codec; p["width"] = e.w; p["height"] = e.h; p["videoBitrateKbps"] = vid;
        // statmux: the allocation is the cap (capped VBR); otherwise the service's own rate control
        p["rateControl"] = managed ? "vbr" : e.rc; p["maxrateKbps"] = managed ? vid : (e.rc == "vbr" && e.maxKbps > 0 ? e.maxKbps : vid);
        p["bufsizeKbps"] = e.bufKbps > 0 ? e.bufKbps : (e.codec == "mpeg2video" ? 1835 : 2 * vid); p["qp"] = e.qp;
        p["gopFrames"] = e.gop; p["keyintMin"] = e.kmin; p["scenecut"] = e.scenecut; p["closedGop"] = e.closedGop; p["bFrames"] = e.bframes;
        p["x264Preset"] = e.preset; p["profile"] = e.profile; p["level"] = e.level; p["interlaced"] = e.interlaced || e.h == 1080 && e.codec == "mpeg2video";
        p["ccPassthrough"] = e.cc; if (e.afd >= 0) p["afd"] = e.afd;
        p["audioCodec"] = e.audCodec; p["audioBitrateKbps"] = e.audKbps; p["audioChannels"] = e.audCh; p["audioSampleRate"] = e.audRate; p["dialnorm"] = e.dialnorm; if (e.mixingLevel >= 0) p["mixingLevel"] = e.mixingLevel; p["aacProfile"] = e.aacProfile;
        if (!e.extra.isEmpty()) { QJsonObject x; x[e.codec == "libx265" ? "x265-params" : "x264-params"] = e.extra; p["extra"] = x; }
        o["profile"] = p;
        QJsonObject opts; opts["muxrate"] = e.ts.value("muxrate").toDouble() > 0 ? e.ts.value("muxrate").toDouble() : double(qint64((vid + e.audKbps) * 1000) * 115 / 100);
        opts["serviceId"] = s.value("serviceId").toInt(); opts["serviceName"] = s.value("longName").toString(); opts["provider"] = e.ts.value("provider").toString("PlayPlay");
        opts["videoPid"] = s.value("vidPid").toInt(); opts["audioPid"] = s.value("audPid").toInt(); opts["pmtPid"] = e.ts.value("pmtPid").toInt(0x1000); opts["pcrPid"] = e.ts.value("pcrPid").toInt(s.value("vidPid").toInt());
        opts["tsId"] = m_cfg.value("tsid").toInt(0x19); opts["pcrPeriodMs"] = e.ts.value("pcrPeriodMs").toInt(20); opts["patPeriodSec"] = e.ts.value("patPeriodSec").toDouble(0.1);
        o["options"] = opts;
        out.append(o);
    }
    return out;
}

QStringList MuxManager::managedChannelIds() const {
    QStringList ids; if (!m_store) return ids;
    for (const ChannelProfile& p : m_store->channels()) if (p.extra.value("managedBy").toString() == "mux") ids << p.id;
    return ids;
}

// ndi services and engine-fed `input` services (DEP-18) get a hidden channel `mux-<id>`: the ScheduleExecutor pins it to
// extra.fixedSource and its engine carries the UDP TS output of the service. Profiles are created/updated/removed here; the
// Supervisor (applyMuxOutputs) spawns/restarts/kills the engines from the returned lists.
QJsonObject MuxManager::syncManagedChannels() {
    QJsonArray created, updated, removed, all; QSet<QString> want;
    // A service that became engine-fed while the mux runs: its ffmpeg encoder goes now, before the engine starts sending to the
    // same multicast (two senders on one group double the bitrate and break continuity). Not waited for: kill, then reap.
    for (int i = m_encoders.size() - 1; i >= 0; --i) {
        Encoder* e = m_encoders[i]; bool engineFed = false;
        for (const QJsonValue& v : m_services) if (v.toObject().value("id").toString() == e->serviceId) { engineFed = inputViaEngine(v.toObject()); break; }
        if (!engineFed) continue;
        if (QProcess* p = e->proc) { p->disconnect(this); if (p->state() == QProcess::NotRunning) p->deleteLater(); else { connect(p, &QProcess::finished, p, &QObject::deleteLater); p->kill(); } }
        appendLog("encoder " + e->serviceId + " stopped: the service is fed by its managed channel now");
        m_encoders.removeAt(i); delete e;
    }
    if (!m_store) return QJsonObject();
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); const QJsonObject src = s.value("source").toObject();
        if (src.value("kind").toString() != "ndi" && !inputViaEngine(s)) continue;
        const bool ndi = src.value("kind").toString() == "ndi";
        const QString id = feedChannelFor(s); want.insert(id); all.append(id);
        const EncView e = encView(s);
        bool found = false; const ChannelProfile old = m_store->channel(id, &found);
        ChannelProfile n = found ? old : ChannelProfile();
        n.id = id; n.name = s.value("longName").toString(id); n.callLetters = s.value("shortName").toString(); n.brandName = n.name; n.enabled = true;
        n.frameRate = e.fps.isEmpty() ? QString("30000/1001") : e.fps; n.width = e.w; n.height = e.h; n.audioChannels = e.audCh;
        n.timezone = m_cfg.value("timezone").toString("America/New_York"); n.prerollSec = 2.0; n.armed = true; n.strict = false;
        QJsonObject fixed = fixedSourceFor(s);
        n.extra["hidden"] = true; n.extra["managedBy"] = "mux"; n.extra["muxServiceId"] = s.value("id"); n.extra["fixedSource"] = fixed;
        QJsonObject ident = n.identity; QJsonArray caps; caps << n.name.toUpper() << (ndi ? "NDI SOURCE OFFLINE" : "STREAM SOURCE OFFLINE"); ident["slateCaptions"] = caps; n.identity = ident;
        n.outputs = engineOutputsFor(id, n.outputs);
        const QString from = ndi ? "NDI " + fixed.value("name").toString() : "stream " + safeUrl(fixed.value("url").toString());
        if (!found) { m_store->saveChannel(n); created.append(id); appendLog("managed channel created: " + id + " <- " + from); continue; }
        if (QJsonDocument(n.toJson()) == QJsonDocument(old.toJson())) continue;
        m_store->saveChannel(n);
        QJsonObject u; u["id"] = id; u["restart"] = old.frameRate != n.frameRate || old.width != n.width || old.height != n.height || old.audioChannels != n.audioChannels;
        u["outputsChanged"] = QJsonDocument(old.outputs) != QJsonDocument(n.outputs); u["sourceChanged"] = QJsonDocument(old.extra.value("fixedSource").toObject()) != QJsonDocument(fixed);
        updated.append(u); appendLog("managed channel updated: " + id);
    }
    for (const QString& id : managedChannelIds()) if (!want.contains(id)) { m_store->deleteChannel(id); removed.append(id); appendLog("managed channel removed: " + id); }
    QJsonObject r; r["created"] = created; r["updated"] = updated; r["removed"] = removed; r["channels"] = all; return r;
}

// ---------------------------------------------------------------------------------------------
// Tables (PAT + PSIP) from core/program_guide.py, titles from the PlayPlay schedule
// ---------------------------------------------------------------------------------------------
QString MuxManager::tablesDir() const { return QDir(m_dataDir).filePath("mux/tables"); }
QString MuxManager::tsduckTool(const QString& exe) const {
    const QString local = QDir(m_cfg.value("tsduckDir").toString()).filePath(exe);
    if (QFileInfo::exists(local)) return local;
    // PATH lookup by the full "tsp.exe": without the suffix Qt tries every PATHEXT extension in every PATH directory (~50 ms per
    // tool here), which capabilities() / status() paid on the main thread for each missing tool when TSDuck is not installed.
    const QString p = QStandardPaths::findExecutable(exe); return p.isEmpty() ? local : p;
}
QString MuxManager::ffmpeg() const {
    const QString c = m_cfg.value("ffmpegPath").toString(); if (!c.isEmpty() && QFileInfo::exists(c)) return c;
    // The FFmpeg 9 build staged next to the executables (mpeg2video/mpeg2_qsv, AC-3), then PATH.
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("ffmpeg.exe");
    if (QFileInfo::exists(local)) return local;
    const QString p = QStandardPaths::findExecutable("ffmpeg"); return p.isEmpty() ? "ffmpeg" : p;
}
QJsonObject MuxManager::tableOverrides() const { return m_store ? m_store->settingJson("mux.tableOverrides") : QJsonObject(); }
void MuxManager::setTableOverrides(const QJsonObject& o) {
    if (!m_store) return;
    QJsonObject cur = tableOverrides(); for (auto it = o.begin(); it != o.end(); ++it) cur[it.key()] = it.value();
    m_store->setSettingJson("mux.tableOverrides", cur);
    QString e; writeTables(&e); if (!e.isEmpty()) appendLog("tables: " + e);
    appendLog("table overrides saved" + QString(running() ? " (tsp picks the rewritten files up via --poll-files; intervals apply at the next start)" : ""));
    emit statusChanged();
}
int MuxManager::eitTables() const { const double days = tableOverrides().value("eitDays").toDouble(1.0); return qBound(1, int(days * 24.0 / kEitWindowHours + 0.999), 128); }

// Replaces `path` atomically (a temp file in the same directory renamed over it, as QSaveFile does) but without QSaveFile's
// FlushFileBuffers: tsp's --poll-files must never read a half-written table, yet nothing needs these files to survive a power
// cut (every start rewrites them). That flush cost ~6 ms per file on the main thread here: 1.6 s for the 261 table files of
// eitDays 16 in writeTables, as much again for the compiled .bin copies, and one every 5 s for stt.xml. (DEP-15)
// A file that already holds exactly these bytes is left alone (a validate, compile and start in the same minute write the same
// tables: same version), so tsp does not even re-read it.
static bool replaceFile(const QString& path, const QByteArray& data) {
    { QFile cur(path); if (cur.size() == data.size() && cur.open(QIODevice::ReadOnly) && cur.readAll() == data) return true; }
    const QString tmp = path + QStringLiteral(".writing");
    {
        QFile f(tmp);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size()) { f.close(); QFile::remove(tmp); return false; }
    }
#ifdef Q_OS_WIN
    const bool ok = MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(tmp).utf16()), reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), MOVEFILE_REPLACE_EXISTING);
#else
    const bool ok = std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(path).constData()) == 0;
#endif
    if (!ok) QFile::remove(tmp);
    return ok;
}
static bool atomicWrite(const QString& path, const QString& text) { return replaceFile(path, text.toUtf8()); }

void MuxManager::writeStt() {
    // STT: system_time is GPS seconds since 1980-01-06 00:00:00 UTC (an integer; TSDuck's XML wants the
    // raw uint32), which runs ahead of UTC by the leap-second offset (18 as of 2017).
    static const QDateTime gpsEpoch(QDate(1980, 1, 6), QTime(0, 0), QTimeZone::utc());
    const qint64 gps = gpsEpoch.secsTo(QDateTime::currentDateTimeUtc()) + 18;
    const QString xml = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <STT protocol_version=\"0\" system_time=\"%1\" GPS_UTC_offset=\"18\" DS_status=\"false\" DS_day_of_month=\"0\" DS_hour=\"0\"/>\n</tsduck>\n").arg(gps);
    atomicWrite(QDir(tablesDir()).filePath("stt.xml"), xml);
    QJsonObject stt; stt["systemTimeGps"] = double(gps); stt["gpsUtcOffset"] = 18; stt["utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate); m_lastTables["stt"] = stt;
}

// US rating region 1 (TV Parental Guidelines + MPAA). A/65 says receivers carry it built in, so it is
// only transmitted when tableOverrides.rrt.enabled (some monitoring receivers like to see it).
static QString usRrtXml(int rev) {
    auto dim = [](const QString& name, bool graduated, const QList<QPair<QString, QString>>& values) {
        QString s = QString("    <dimension graduated_scale=\"%1\">\n      <dimension_name><string language=\"eng\" text=\"%2\"/></dimension_name>\n").arg(graduated ? "true" : "false").arg(xmlEsc(name));
        for (const auto& v : values) s += QString("      <value><abbrev_rating_value><string language=\"eng\" text=\"%1\"/></abbrev_rating_value><rating_value><string language=\"eng\" text=\"%2\"/></rating_value></value>\n").arg(xmlEsc(v.first)).arg(xmlEsc(v.second));
        return s + "    </dimension>\n"; };
    QString x = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <RRT version=\"%1\" protocol_version=\"0\" rating_region=\"1\">\n    <rating_region_name><string language=\"eng\" text=\"US (50 states + possessions)\"/></rating_region_name>\n").arg(rev);
    x += dim("Entire Audience", true, {{"", ""}, {"None", "None"}, {"TV-G", "TV-G"}, {"TV-PG", "TV-PG"}, {"TV-14", "TV-14"}, {"TV-MA", "TV-MA"}});
    x += dim("Dialogue", false, {{"", ""}, {"D", "D"}});
    x += dim("Language", false, {{"", ""}, {"L", "L"}});
    x += dim("Sex", false, {{"", ""}, {"S", "S"}});
    x += dim("Violence", false, {{"", ""}, {"V", "V"}});
    x += dim("Children", true, {{"", ""}, {"TV-Y", "TV-Y"}, {"TV-Y7", "TV-Y7"}});
    x += dim("Fantasy violence", false, {{"", ""}, {"FV", "FV"}});
    x += dim("MPAA", true, {{"", ""}, {"N/A", "MPAA Rating Not Applicable"}, {"G", "Suitable for All Ages"}, {"PG", "Parental Guidance Suggested"}, {"PG-13", "Parents Strongly Cautioned"}, {"R", "Restricted"}, {"NC-17", "No One 17 and Under Admitted"}, {"X", "No One 17 and Under Admitted"}, {"NR", "Not Rated by MPAA"}});
    return x + "  </RRT>\n</tsduck>\n";
}

bool MuxManager::writeTables(QString* error) {
    QDir().mkpath(tablesDir());
    const QJsonObject ov = tableOverrides(); const QJsonObject ovT = ov.value("tvct").toObject(); const QJsonObject ovS = ov.value("services").toObject(); const QJsonObject ovR = ov.value("rrt").toObject();
    const int rev = int((QDateTime::currentSecsSinceEpoch() / 60) % 32);   // version bumps each minute the tables are rewritten
    const int tsid = m_cfg.value("tsid").toInt(0x19);
    const int chanTsid = ovT.value("channelTsid").toInt(tsid);
    const QString modulation = ovT.value("modulation").toString("8-VSB");
    QJsonObject summary; summary["generatedMs"] = double(QDateTime::currentMSecsSinceEpoch()); summary["version"] = rev; summary["tsid"] = tsid; QJsonArray tvctRows, mgtRows, eitRows;
    QString pat = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <PAT version=\"%1\" current=\"true\" transport_stream_id=\"%2\">\n").arg(rev).arg(hex4(tsid));
    QString tvct = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <TVCT version=\"%1\" current=\"true\" protocol_version=\"0\" transport_stream_id=\"%2\">\n").arg(rev).arg(hex4(tsid));
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); const int sid = s.value("serviceId").toInt(); const QString id = s.value("id").toString(); const QJsonObject o = ovS.value(id).toObject();
        const int sourceId = o.value("sourceId").toInt(sid); const QString serviceType = o.value("serviceType").toString("dtv");
        const bool hidden = o.value("hidden").toBool(false), hideGuide = o.value("hideGuide").toBool(false), access = o.value("accessControlled").toBool(false); const int etm = o.value("etm").toInt(0);
        pat += QString("    <service service_id=\"%1\" program_map_PID=\"%2\"/>\n").arg(hex4(sid)).arg(hex4(0x20 + sid));
        tvct += QString("    <channel short_name=\"%1\" major_channel_number=\"%2\" minor_channel_number=\"%3\" modulation_mode=\"%4\" carrier_frequency=\"%5\" channel_TSID=\"%6\" program_number=\"%7\" ETM_location=\"%8\" access_controlled=\"%9\"")
                    .arg(xmlEsc(s.value("shortName").toString().left(7))).arg(s.value("major").toInt(25)).arg(s.value("minor").toInt(1)).arg(xmlEsc(modulation)).arg(ovT.value("carrierFrequency").toInt(0)).arg(hex4(chanTsid)).arg(sid).arg(etm).arg(access ? "true" : "false");
        tvct += QString(" hidden=\"%1\" hide_guide=\"%2\" service_type=\"%3\" source_id=\"%4\">\n      <extended_channel_name_descriptor><long_channel_name_text><string language=\"eng\" text=\"%5\"/></long_channel_name_text></extended_channel_name_descriptor>\n")
                    .arg(hidden ? "true" : "false").arg(hideGuide ? "true" : "false").arg(xmlEsc(serviceType)).arg(sourceId).arg(xmlEsc(s.value("longName").toString().left(64)));
        if (!o.value("extraXml").toString().trimmed().isEmpty()) tvct += "      " + o.value("extraXml").toString().trimmed() + "\n";
        tvct += "    </channel>\n";
        QJsonObject row; row["id"] = id; row["shortName"] = s.value("shortName"); row["longName"] = s.value("longName"); row["major"] = s.value("major"); row["minor"] = s.value("minor"); row["serviceId"] = sid; row["sourceId"] = sourceId; row["pmtPid"] = 0x20 + sid;
        row["modulation"] = modulation; row["hidden"] = hidden; row["hideGuide"] = hideGuide; row["serviceType"] = serviceType; row["accessControlled"] = access; row["etm"] = etm; row["vidPid"] = s.value("vidPid"); row["audPid"] = s.value("audPid"); tvctRows.append(row);
    }
    pat += "  </PAT>\n</tsduck>\n";
    if (!ovT.value("extraXml").toString().trimmed().isEmpty()) tvct += "    " + ovT.value("extraXml").toString().trimmed() + "\n";
    tvct += "  </TVCT>\n</tsduck>\n";
    const int nEit = eitTables(); const bool rrt = ovR.value("enabled").toBool(false);
    QString mgt = QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n  <MGT version=\"%1\" protocol_version=\"0\">\n    <table type=\"TVCT-current\" PID=\"0x1FFB\" version_number=\"%1\" number_bytes=\"2048\"/>\n").arg(rev);
    { QJsonObject m; m["type"] = "TVCT-current"; m["pid"] = "0x1FFB"; mgtRows.append(m); }
    for (int k = 0; k < nEit; ++k) { mgt += QString("    <table type=\"EIT-%1\" PID=\"%2\" version_number=\"%3\" number_bytes=\"16384\"/>\n").arg(k).arg(eitPid(k)).arg(rev); QJsonObject m; m["type"] = QString("EIT-%1").arg(k); m["pid"] = eitPid(k); mgtRows.append(m); }
    for (int k = 0; k < nEit; ++k) { mgt += QString("    <table type=\"ETT-%1\" PID=\"%2\" version_number=\"%3\" number_bytes=\"32768\"/>\n").arg(k).arg(ettPid(k)).arg(rev); QJsonObject m; m["type"] = QString("ETT-%1").arg(k); m["pid"] = ettPid(k); mgtRows.append(m); }
    if (rrt) { mgt += QString("    <table type=\"RRT-1\" PID=\"0x1FFB\" version_number=\"%1\" number_bytes=\"4096\"/>\n").arg(rev); QJsonObject m; m["type"] = "RRT-1"; m["pid"] = "0x1FFB"; mgtRows.append(m); }
    mgt += "  </MGT>\n</tsduck>\n";
    bool ok = atomicWrite(QDir(tablesDir()).filePath("pat.xml"), pat) && atomicWrite(QDir(tablesDir()).filePath("tvct.xml"), tvct) && atomicWrite(QDir(tablesDir()).filePath("mgt.xml"), mgt);
    if (rrt) ok = atomicWrite(QDir(tablesDir()).filePath("rrt.xml"), ovR.value("xml").toString().trimmed().isEmpty() ? usRrtXml(rev) : ovR.value("xml").toString()) && ok; else QFile::remove(QDir(tablesDir()).filePath("rrt.xml"));
    if (!ov.value("customXml").toString().trimmed().isEmpty()) ok = atomicWrite(QDir(tablesDir()).filePath("custom.xml"), ov.value("customXml").toString()) && ok; else QFile::remove(QDir(tablesDir()).filePath("custom.xml"));

    // EIT/ETT: one table pair per 3-hour window from the current hour; titles from the schedule.
    const QTimeZone tz(m_cfg.value("timezone").toString("America/New_York").toUtf8());
    const QDateTime nowLocal = QDateTime::currentDateTime().toTimeZone(tz.isValid() ? tz : QTimeZone::systemTimeZone());
    const QDateTime startLocal(nowLocal.date(), QTime(nowLocal.time().hour(), 0), nowLocal.timeZone());
    for (int k = 0; k < nEit; ++k) {
        QString eit = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<tsduck>\n", ett = eit;
        QJsonObject tbl; tbl["k"] = k; tbl["pid"] = eitPid(k); tbl["ettPid"] = ettPid(k); tbl["fromMs"] = double(startLocal.addSecs(k * kEitWindowHours * 3600).toMSecsSinceEpoch()); tbl["toMs"] = double(startLocal.addSecs((k + 1) * kEitWindowHours * 3600).toMSecsSinceEpoch()); QJsonArray evs;
        for (const QJsonValue& v : m_services) {
            const QJsonObject s = v.toObject(); const int sid = s.value("serviceId").toInt(); const int sourceId = ovS.value(s.value("id").toString()).toObject().value("sourceId").toInt(sid);
            const QString chan = feedChannelFor(s);
            eit += QString("  <ATSC_EIT version=\"%1\" source_id=\"%2\" protocol_version=\"0\">\n").arg(rev).arg(sourceId);
            for (int h = k * kEitWindowHours; h < (k + 1) * kEitWindowHours; ++h) {
                const QDateTime ls = startLocal.addSecs(h * 3600);
                QString desc; QString title = m_titles && !chan.isEmpty() && !chan.startsWith("mux-") ? m_titles(chan, ls, &desc) : QString();
                if (title.isEmpty()) title = s.value("longName").toString(); if (desc.isEmpty()) desc = title;
                const int eventId = int((qint64(ls.date().toJulianDay()) * 24 + ls.time().hour()) & 0x3FFF);
                eit += QString("    <event event_id=\"%1\" start_time=\"%2\" ETM_location=\"1\" length_in_seconds=\"3600\">\n      <title_text><string language=\"eng\" text=\"%3\"/></title_text>\n    </event>\n").arg(eventId).arg(ls.toUTC().toString("yyyy-MM-dd HH:mm:ss")).arg(xmlEsc(title.left(120)));
                const qint64 etmId = (qint64(sourceId) << 16) | (qint64(eventId) << 2) | 0x02;
                ett += QString("  <ETT version=\"%1\" protocol_version=\"0\" ETT_table_id_extension=\"%2\" ETM_id=\"%3\">\n    <extended_text_message><string language=\"eng\" text=\"%4\"/></extended_text_message>\n  </ETT>\n").arg(rev).arg(sourceId).arg(etmId).arg(xmlEsc(desc.left(1000)));
                QJsonObject ev; ev["serviceId"] = sid; ev["sourceId"] = sourceId; ev["service"] = s.value("id"); ev["eventId"] = eventId; ev["startMs"] = double(ls.toMSecsSinceEpoch()); ev["title"] = title; ev["description"] = desc; evs.append(ev);
            }
            eit += "  </ATSC_EIT>\n";
        }
        eit += "</tsduck>\n"; ett += "</tsduck>\n";
        ok = atomicWrite(QDir(tablesDir()).filePath(QString("eit%1.xml").arg(k)), eit) && ok;
        ok = atomicWrite(QDir(tablesDir()).filePath(QString("ett%1.xml").arg(k)), ett) && ok;
        tbl["events"] = evs; eitRows.append(tbl);
    }
    // stale EIT files from a deeper previous eitDays are removed so the directory mirrors the MGT
    for (int k = nEit; k < 128; ++k) { const QString a = QDir(tablesDir()).filePath(QString("eit%1.xml").arg(k)), b = QDir(tablesDir()).filePath(QString("ett%1.xml").arg(k)); if (!QFileInfo::exists(a) && !QFileInfo::exists(b)) break; QFile::remove(a); QFile::remove(b); }
    writeStt();
    summary["tvct"] = tvctRows; summary["mgt"] = mgtRows; summary["eit"] = eitRows; summary["eitTables"] = nEit; QJsonObject rr; rr["enabled"] = rrt; rr["region"] = ovR.value("region").toInt(1); summary["rrt"] = rr; summary["customXml"] = !ov.value("customXml").toString().trimmed().isEmpty();
    summary["modulation"] = modulation; summary["channelTsid"] = chanTsid; summary["stt"] = m_lastTables.value("stt");
    m_lastTables = summary;
    if (!ok && error) *error = "could not write PSIP tables under " + tablesDir();
    return ok;
}

// mux.config.tables: "native" (default) compiles every table file in this process with PlayPlayTsKit (DEP-14: byte-identical
// with `tstabcomp --atsc` on the goldens and on the live table set), "tstabcomp" spawns TSDuck's compiler per file as before.
// The old path was 4 + 2 x eitTables() sequential spawns (up to 260, each waited for up to 10 s on the thread that serves
// REST/IPC and runs the schedule executor) at every validate, compile and mux start. The .bin files are only the check: tsp
// injects the XML itself (--poll-files), so the compiler decides whether a start proceeds, not what goes on air.
QString MuxManager::tablesEngine() const { return m_cfg.value("tables").toString().compare("tstabcomp", Qt::CaseInsensitive) == 0 ? QStringLiteral("tstabcomp") : QStringLiteral("native"); }

bool MuxManager::compileTables(QString* error, QJsonArray* report) {
    const QString comp = tsduckTool("tstabcomp.exe"); const bool haveComp = QFileInfo::exists(comp);
    const bool native = tablesEngine() == "native";
    if (!native && !haveComp) { if (error) *error = "tstabcomp.exe not found (set mux.config.tsduckDir, or mux.config.tables = native)"; return false; }
    QStringList files = {"pat.xml", "tvct.xml", "mgt.xml", "stt.xml"};
    for (int k = 0; k < eitTables(); ++k) files << QString("eit%1.xml").arg(k) << QString("ett%1.xml").arg(k);
    if (QFileInfo::exists(QDir(tablesDir()).filePath("rrt.xml"))) files << "rrt.xml";
    if (QFileInfo::exists(QDir(tablesDir()).filePath("custom.xml"))) files << "custom.xml";
    const QString outDir = QDir(m_dataDir).filePath("mux/compiled"); QDir().mkpath(outDir);
    // Files carrying operator XML (customXml, an RRT override, TVCT extraXml): PlayPlayTsKit ignores attributes it does not know,
    // TSDuck's model (tstabcomp, and tsp's inject plugin that puts the XML on air) rejects them. So with tstabcomp installed its
    // verdict decides for these few files even when the native compile succeeds, as before DEP-15; a mistyped attribute keeps
    // failing Validate / start with tstabcomp's message instead of a tsp that exits rc=1 every 3 s. Generated files stay native.
    QSet<QString> operatorXml;
    if (native && haveComp) {
        const QJsonObject ov = tableOverrides();
        if (!ov.value("customXml").toString().trimmed().isEmpty()) operatorXml << "custom.xml";
        if (!ov.value("rrt").toObject().value("xml").toString().trimmed().isEmpty()) operatorXml << "rrt.xml";
        bool extra = !ov.value("tvct").toObject().value("extraXml").toString().trimmed().isEmpty();
        const QJsonObject ovS = ov.value("services").toObject();
        for (auto it = ovS.begin(); it != ovS.end() && !extra; ++it) extra = !it.value().toObject().value("extraXml").toString().trimmed().isEmpty();
        if (extra) operatorXml << "tvct.xml";
    }
    // tstabcomp for one file: the whole job with tables = tstabcomp. With native only when PlayPlayTsKit rejects a file: XML outside
    // its subset (operator customXml / extraXml naming a table or descriptor it does not build) or any other error, where
    // tstabcomp's verdict decides as before DEP-15 (tsp injects the XML itself, so a file tstabcomp accepts must not stop a start).
    // Rare, synchronous like the old path, one file each; nothing is spawned while every file compiles in-process.
    auto viaTstabcomp = [&](const QString& in, const QString& bin, QString* out) {
        QProcess p; p.start(comp, {"--atsc", "--output", bin, in});
        const bool fin = p.waitForFinished(10000); *out = QString::fromUtf8(p.readAllStandardError() + p.readAllStandardOutput()).trimmed();
        return fin ? p.exitCode() : -1;
    };
    bool all = true;
    for (const QString& f : files) {
        const QString in = QDir(tablesDir()).filePath(f), bin = QDir(outDir).filePath(QFileInfo(f).completeBaseName() + ".bin");
        QJsonObject r; r["file"] = f; QString out; int code = -1; QString engine = native ? "native" : "tstabcomp";
        QElapsedTimer t; t.start();
        if (native) {
            const tskit::CompileResult c = tskit::compileFile(in, QString());      // the .bin below: replaceFile, not QSaveFile's flush
            if (c.ok && !replaceFile(bin, c.bin())) { code = 2; out = "cannot write " + QDir::toNativeSeparators(bin); }
            else if (c.ok && operatorXml.contains(f)) {
                engine = "tstabcomp"; r["operatorXml"] = true; code = viaTstabcomp(in, bin, &out);
                if (code != 0) appendLog("tables: " + f + ": PlayPlayTsKit accepts the operator XML but tstabcomp rejects it; using tstabcomp's verdict");
            }
            else if (c.ok) { code = 0; r["sections"] = c.sectionCount(); r["tables"] = int(c.tables.size()); }
            else if (haveComp) {
                engine = "tstabcomp"; r[c.unsupported ? "fallback" : "nativeError"] = c.error; code = viaTstabcomp(in, bin, &out);
                if (!c.unsupported && code == 0) appendLog("tables: " + f + ": PlayPlayTsKit rejected it (" + c.error + ") but tstabcomp accepts it; using tstabcomp's verdict");
            }
            else { code = c.unsupported ? 64 : 2; out = c.error + (c.unsupported ? QString(" (outside the in-house compiler's subset; tstabcomp.exe is not installed to compile it)") : QString()); }
        } else code = viaTstabcomp(in, bin, &out);
        const bool ok = code == 0;
        if (report) { r["ok"] = ok; r["output"] = out; r["exitCode"] = code; r["engine"] = engine; r["ms"] = double(t.nsecsElapsed() / 1000) / 1000.0; report->append(r); }
        if (!ok) { all = false; if (error && error->isEmpty()) *error = out.startsWith(f + ": ") ? out : f + ": " + (out.isEmpty() ? engine + " failed" : out); if (!report) return false; }   // compileFile names the file itself
    }
    return all;
}

QJsonObject MuxManager::validateTables(QString* error) {
    QString e; const bool w = writeTables(&e); const bool c = w && compileTables(&e);
    QJsonObject o; o["written"] = w; o["compiled"] = c; o["error"] = e; o["dir"] = QDir::toNativeSeparators(tablesDir()); o["services"] = m_services.size(); o["engine"] = tablesEngine();
    if (error) *error = e; return o;
}

QJsonObject MuxManager::compileReport() {
    QElapsedTimer t; t.start();
    QString e; QJsonArray rep; const bool w = writeTables(&e); const qint64 writeMs = t.elapsed(); const bool c = w && compileTables(&e, &rep);
    QJsonObject o; o["written"] = w; o["compiled"] = c; o["error"] = e; o["files"] = rep; o["dir"] = QDir::toNativeSeparators(tablesDir()); o["compiledDir"] = QDir::toNativeSeparators(QDir(m_dataDir).filePath("mux/compiled")); o["tstabcomp"] = tsduckTool("tstabcomp.exe");
    o["engine"] = tablesEngine(); o["writeMs"] = double(writeMs); o["totalMs"] = double(t.elapsed());
    return o;
}

QJsonObject MuxManager::tablesJson() {
    if (m_lastTables.isEmpty() || !QFileInfo::exists(QDir(tablesDir()).filePath("pat.xml"))) { QString e; writeTables(&e); }
    QJsonArray files;
    QDir d(tablesDir());
    for (const QFileInfo& fi : d.entryInfoList({"*.xml"}, QDir::Files, QDir::Name)) {
        QFile f(fi.absoluteFilePath()); QJsonObject o; o["name"] = fi.fileName(); o["size"] = double(fi.size()); o["modifiedMs"] = double(fi.lastModified().toMSecsSinceEpoch());
        if (f.open(QIODevice::ReadOnly)) o["xml"] = QString::fromUtf8(f.readAll()); files.append(o);
    }
    QJsonObject o; o["dir"] = QDir::toNativeSeparators(tablesDir()); o["files"] = files; o["psip"] = m_lastTables; o["overrides"] = tableOverrides(); o["eitTables"] = eitTables(); o["running"] = running();
    return o;
}

QJsonObject MuxManager::guidePreview() const {
    QJsonObject o; const QTimeZone tz(m_cfg.value("timezone").toString("America/New_York").toUtf8());
    const QDateTime nowLocal = QDateTime::currentDateTime().toTimeZone(tz.isValid() ? tz : QTimeZone::systemTimeZone());
    const QDateTime startLocal(nowLocal.date(), QTime(nowLocal.time().hour(), 0), nowLocal.timeZone());
    QJsonArray svcs;
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); QJsonArray ev; const QString chan = feedChannelFor(s);
        for (int h = 0; h < 24; ++h) { const QDateTime ls = startLocal.addSecs(h * 3600); QString d; QString t = m_titles && !chan.isEmpty() && !chan.startsWith("mux-") ? m_titles(chan, ls, &d) : QString(); if (t.isEmpty()) t = s.value("longName").toString(); QJsonObject e; e["startMs"] = double(ls.toMSecsSinceEpoch()); e["title"] = t; e["description"] = d; ev.append(e); }
        QJsonObject so; so["id"] = s.value("id"); so["channel"] = QString("%1.%2").arg(s.value("major").toInt()).arg(s.value("minor").toInt()); so["events"] = ev; svcs.append(so);
    }
    o["services"] = svcs; o["from"] = double(startLocal.toMSecsSinceEpoch()); return o;
}

// Quick live analysis: `tables` dumps PAT/PSIP sections seen on the UDP output, the output file (output.kind file) or a
// service's multicast; `analyze` runs the analyzer for 3 s (3 s of packets at the transport rate for a file). PlayPlayMux
// analyze (DEP-16, the `tsp -P tables` text layout) when mux.config.tools resolves to native, else tsp. Asynchronous: nothing
// here waits for the child (the old code blocked up to 3 s in waitForStarted); `done` gets the text.
void MuxManager::analyze(const QString& serviceId, const QString& mode0, std::function<void(const QJsonObject&)> done) {
    const QString tsp = tsduckTool("tsp.exe"), mux = toolsEngine() == "native" ? playplayMuxExe() : QString();
    QJsonObject r; r["mode"] = mode0.isEmpty() ? "tables" : mode0;
    if (mux.isEmpty() && !QFileInfo::exists(tsp)) { r["error"] = toolsEngine() == "native" ? "PlayPlayMux.exe not found (mux.config.playplayMux)" : "tsp.exe not found (set mux.config.tsduckDir)"; done(r); return; }
    const QJsonObject out = m_cfg.value("output").toObject(); QString src; bool psip = false, file = false;
    const QString outFile = QDir::toNativeSeparators(out.value("url").toString(QDir(m_dataDir).filePath("mux/out.ts")));
    if (!serviceId.isEmpty()) { for (const QJsonValue& v : m_services) if (v.toObject().value("id").toString() == serviceId) src = svcMcastAddr(v.toObject()); if (src.isEmpty()) { r["error"] = "no such service: " + serviceId; done(r); return; } }
    else if (out.value("kind").toString("dektec") == "udp") { src = out.value("url").toString("udp://239.1.1.1:1234").remove("udp://").section('?', 0, 0); psip = true; }
    else if (out.value("kind").toString() == "file" && QFileInfo(outFile).isFile()) { src = outFile; psip = true; file = true; }   // what tsp writes (tspArgs)
    else if (!m_services.isEmpty()) src = svcMcastAddr(m_services.first().toObject());
    if (src.isEmpty()) { r["error"] = "nothing to analyse: no services and the output is not UDP"; done(r); return; }
    const QString mode = r.value("mode").toString() == "analyze" ? "analyze" : "tables";
    const QString packets = QString::number(qMax<qint64>(1000, transportBps() * 3 / (188 * 8)));     // a file: 3 s at the transport rate
    QStringList pids = {"--pid", "0x0000"}; if (psip) pids << "--pid" << "0x1FFB" << "--pid" << "0x0200"; else pids << "--pid" << "0x1000" << "--pid" << "0x0011";
    QStringList tspA = file ? QStringList{"-I", "file", src, "-P", "until", "--packets", packets} : QStringList{"-I", "ip", "--default-interface", "--reuse-port", "--receive-timeout", "5000", src, "-P", "until", "--seconds", "3"};
    if (mode == "analyze") tspA << "-P" << "analyze"; else tspA << "-P" << "tables" << pids.mid(0, 2) << "--max-tables" << "12" << pids.mid(2);
    tspA << "-O" << "drop";
    QStringList muxA = {"analyze", "--mode", mode};
    muxA << (file ? QStringList{"--input", "file", "--max-packets", packets} : QStringList{"--input", "udp", "--seconds", "3", "--receive-timeout", "5000"});
    if (mode == "tables") muxA << pids << "--max-tables" << "12";
    muxA << src;
    r["source"] = src; r["psipOutput"] = psip; r["running"] = running(); if (file) r["file"] = true;
    if (!running() && !psip) r["note"] = "mux is not running: service multicasts only carry data while the engines/encoders feed them";
    auto viaTsp = [this, tsp, tspA, done](QJsonObject r) {
        if (!QFileInfo::exists(tsp)) { r["error"] = "tsp.exe not found (set mux.config.tsduckDir)"; done(r); return; }
        r["engine"] = "tsduck"; r["command"] = quoteArgs(tsp, tspA);
        runTool("analyze", tsp, tspA, 20000, true, [r, done](const ToolResult& t) mutable {
            r["exitCode"] = t.exitCode; r["output"] = t.out.left(200000);
            if (!t.startError.isEmpty()) { r["error"] = "tsp did not start: " + t.startError; r.remove("output"); }
            if (t.timedOut) r["timedOut"] = true;
            done(r);
        });
    };
    if (mux.isEmpty()) { viaTsp(r); return; }
    r["engine"] = "native"; r["command"] = quoteArgs(mux, muxA);
    runTool("analyze", mux, muxA, 20000, false, [r, done, viaTsp](const ToolResult& t) mutable {
        // stdout = the tables text (tsp layout) then the analyzer report, the JSON summary on the last line (ppcli)
        QString text = t.out; QJsonObject summary;
        while (!text.isEmpty() && text.back().isSpace()) text.chop(1);
        const int nl = text.lastIndexOf('\n');
        const QJsonDocument d = QJsonDocument::fromJson(text.mid(nl + 1).trimmed().toUtf8());
        if (d.isObject()) { summary = d.object(); text = nl < 0 ? QString() : text.left(nl + 1); } else text = t.out;
        if (t.exitCode == 64 && summary.value("unsupported").toBool()) { r["fallback"] = summary.value("error").toString(); viaTsp(r); return; }   // run tsp as before
        if (!t.startError.isEmpty()) { r["fallback"] = "PlayPlayMux did not start: " + t.startError; viaTsp(r); return; }
        QString diag = t.err.trimmed();
        if (text.trimmed().isEmpty() && diag.isEmpty() && summary.contains("error")) diag = "error: " + summary.value("error").toString();
        r["exitCode"] = t.exitCode; r["output"] = (text + (diag.isEmpty() ? QString() : (text.isEmpty() || text.endsWith('\n') ? "" : "\n") + diag + "\n")).left(200000);
        if (!summary.isEmpty()) r["summary"] = summary;
        if (t.timedOut) r["timedOut"] = true;
        done(r);
    });
}

// ---------------------------------------------------------------------------------------------
// FFmpeg encoders for `input` services (core/encoder_backend.py build_ffmpeg_cmd)
// ---------------------------------------------------------------------------------------------
bool MuxManager::ffmpegHasEncoder(const QString& name) const {
    const QJsonObject has = m_caps.value("ffmpeg").toObject().value("has").toObject();
    return has.isEmpty() || !has.contains(name) ? true : has.value(name).toBool();
}

QStringList MuxManager::ffmpegArgs(const QJsonObject& svc, QString* error) const {
    const QJsonObject src = svc.value("source").toObject(); const QJsonObject in = src.value("input").toObject(); const EncView e = encView(svc);
    const QString type = in.value("type").toString("test"); const QString loc = in.value("location").toString();
    const int w = e.w, h = e.h; const QString fps = e.fps;
    const int vid = int(m_alloc.value(svc.value("id").toString()).toDouble(e.vidKbps * 1000) / 1000); const int aud = e.audKbps;
    QStringList a = {"-hide_banner", "-loglevel", "warning", "-nostats"};
    QStringList maps = {"-map", "0:v:0", "-map", "0:a:0"};
    auto silent = [&]() { a << "-f" << "lavfi" << "-i" << QString("anullsrc=channel_layout=%1:sample_rate=48000").arg(e.audCh >= 6 ? "5.1" : "stereo"); maps = {"-map", "0:v:0", "-map", "1:a:0"}; };
    if (type == "test") { a << "-re" << "-f" << "lavfi" << "-i" << (loc.contains('=') ? loc : QString("testsrc2=size=%1x%2:rate=%3").arg(w).arg(h).arg(fps)); silent(); }
    else if (type == "file") { if (!QFileInfo(loc).isFile()) { if (error) *error = "media file does not exist: " + loc; return {}; } a << "-re" << "-stream_loop" << "-1" << "-i" << loc; if (in.value("audio").toString() == "silence") silent(); }
    else if (type == "rtsp") { a << "-fflags" << "+genpts+nobuffer" << "-thread_queue_size" << "1024" << "-rtsp_transport" << in.value("transport").toString("tcp") << "-rw_timeout" << "10000000" << "-i" << loc; if (in.value("audio").toString() == "silence") silent(); }
    else if (type == "srt" || type == "rtmp" || type == "udp" || type == "http") { a << "-fflags" << "+genpts+nobuffer" << "-thread_queue_size" << "1024" << "-rw_timeout" << "10000000" << "-i" << loc; if (in.value("audio").toString() == "silence") silent(); }
    else if (type == "dshow") { a << "-fflags" << "nobuffer" << "-flags" << "low_delay" << "-rtbufsize" << "32M" << "-thread_queue_size" << "512" << "-f" << "dshow" << "-i" << ("video=" + loc + (in.value("audioDevice").toString().isEmpty() ? QString() : ":audio=" + in.value("audioDevice").toString())); if (in.value("audioDevice").toString().isEmpty()) silent(); }
    else { if (error) *error = "input type '" + type + "' is handled by a PlayPlay channel (use source kind 'ndi' or 'channel')"; return {}; }
    a << maps;
    const bool interlaced = e.interlaced && (h >= 1080 || h == 480 || h == 576);
    QString vf = QString("fps=fps=%1:round=near,scale=%2:%3:flags=bicubic").arg(fps).arg(w).arg(h) + (interlaced ? ",interlace=scan=tff" : "") + ",format=yuv420p";
    a << "-vf" << vf << "-r" << fps;
    QString codec = e.codec; const QString br = QString::number(vid) + "k"; const int gop = e.gop; const int kmin = e.kmin > 0 ? qMin(e.kmin, gop) : gop; const int bf = e.bframes;
    const bool managed = m_cfg.value("statmux").toObject().value("enabled").toBool(true) && svc.value("statmux").toObject().value("managed").toBool(true);
    const QString rc = managed ? "vbr" : e.rc;    // statmux: the allocation is the cap
    const QString maxbr = (rc == "vbr" && !managed && e.maxKbps > 0) ? QString::number(e.maxKbps) + "k" : br;
    const QString buf = e.bufKbps > 0 ? QString::number(e.bufKbps) + "k" : (codec == "mpeg2video" ? "1835k" : QString::number(2 * vid) + "k");
    if (!ffmpegHasEncoder(codec)) { const QString fb = codec.contains("265") || codec.contains("hevc") ? (ffmpegHasEncoder("libx265") ? "libx265" : "libx264") : "libx264"; if (error) *error = QString("encoder %1 is not in this FFmpeg build; using %2").arg(codec, fb); codec = fb; }
    auto rcArgs = [&]() { if (rc == "cqp") return; a << "-b:v" << br; if (rc == "cbr") a << "-minrate" << br; a << "-maxrate" << maxbr << "-bufsize" << buf; };
    if (codec == "h264_nvenc" || codec == "hevc_nvenc") {
        a << "-c:v" << codec << "-preset" << (e.preset.startsWith('p') ? e.preset : "p4") << "-rc" << (rc == "cbr" ? "cbr" : rc == "cqp" ? "constqp" : "vbr"); if (rc == "cqp") a << "-qp" << QString::number(e.qp); rcArgs();
        a << "-g" << QString::number(gop) << "-bf" << QString::number(bf) << "-profile:v" << (codec == "hevc_nvenc" && e.profile == "high" ? "main" : e.profile) << "-level" << e.level << "-zerolatency" << "1" << "-forced-idr" << "1" << "-no-scenecut" << (e.scenecut ? "0" : "1") << "-strict_gop" << "1" << "-a53cc" << (e.cc ? "1" : "0");
    } else if (codec == "h264_qsv" || codec == "hevc_qsv") {
        a << "-c:v" << codec << "-preset" << e.preset; if (rc == "cqp") a << "-q:v" << QString::number(e.qp); else rcArgs();
        a << "-g" << QString::number(gop) << "-bf" << QString::number(bf) << "-profile:v" << (codec == "hevc_qsv" && e.profile == "high" ? "main" : e.profile) << "-forced_idr" << "1" << "-look_ahead" << "0" << "-a53cc" << (e.cc ? "1" : "0");
    } else if (codec == "mpeg2video") {
        a << "-c:v" << "mpeg2video"; if (rc == "cqp") a << "-q:v" << QString::number(e.qp); else { a << "-b:v" << br << "-minrate" << (rc == "cbr" ? br : QString::number(qMax(100, vid / 4)) + "k") << "-maxrate" << maxbr << "-bufsize" << buf; }
        // FFmpeg's mpeg2video refuses closed GOPs with scene-change detection (avcodec_open2: "closed gop with scene change detection
        // are not supported yet"): no video, watchdog restart loop. Closed GOP wins; scene-cut only on open-GOP mpeg2 services.
        const bool sceneCut = e.scenecut && !e.closedGop;
        if (e.scenecut && e.closedGop && error) { if (!error->isEmpty()) *error += "; "; *error += "scene-cut is ignored for MPEG-2 with closed GOPs (FFmpeg cannot do both)"; }
        a << "-g" << QString::number(gop) << "-bf" << QString::number(bf) << "-sc_threshold" << (sceneCut ? "0" : "1000000000") << "-intra_vlc" << "1" << "-non_linear_quant" << "1" << "-qmax" << "28" << "-seq_disp_ext" << "1" << "-video_format" << "component" << "-a53cc" << (e.cc ? "1" : "0") << "-profile:v" << "4" << "-level" << (h > 576 ? "4" : "8");
        // Closed GOP when the service asks (same rule as EncoderStage), interlaced coding on interlaced rasters; the field order comes from
        // the interlace=scan=tff filter above (FFmpeg 9 removed -top). The -flags statement used to sit inside this comment (never emitted).
        QString flags = e.closedGop ? "+cgop" : ""; if (interlaced) { flags += "+ilme+ildct"; a << "-alternate_scan" << "1"; }
        if (!flags.isEmpty()) a << "-flags" << flags;
    } else if (codec == "libx265") {
        a << "-c:v" << "libx265" << "-preset" << e.preset << "-tune" << "zerolatency"; if (rc == "cqp") a << "-x265-params" << QString("qp=%1:").arg(e.qp); else rcArgs();
        QStringList x = {QString("keyint=%1").arg(gop), QString("min-keyint=%1").arg(kmin), QString("scenecut=%1").arg(e.scenecut ? 40 : 0), QString("open-gop=%1").arg(e.closedGop ? 0 : 1), "repeat-headers=1", QString("bframes=%1").arg(bf)};
        if (rc == "cbr") x << "strict-cbr=1"; if (!e.extra.isEmpty()) x << e.extra;
        a << "-profile:v" << (e.profile == "high" ? "main" : e.profile) << "-x265-params" << x.join(':');
    } else {
        a << "-c:v" << "libx264" << "-preset" << e.preset << "-tune" << "zerolatency"; if (rc == "cqp") a << "-qp" << QString::number(e.qp); else rcArgs();
        a << "-g" << QString::number(gop) << "-keyint_min" << QString::number(kmin) << "-sc_threshold" << (e.scenecut ? "40" : "0") << "-bf" << QString::number(bf) << "-profile:v" << e.profile << "-level" << e.level << "-a53cc" << (e.cc ? "1" : "0");
        QStringList x = {"repeat-headers=1", "force-cfr=1", QString("open-gop=%1").arg(e.closedGop ? 0 : 1)}; if (rc == "cbr") x << "nal-hrd=cbr"; else if (rc == "vbr") x << "nal-hrd=vbr"; if (interlaced) x << "interlaced=1:tff=1"; if (!e.extra.isEmpty()) x << e.extra;
        a << "-x264-params" << x.join(':');
    }
    a << "-color_range" << "1" << "-color_primaries" << "1" << "-color_trc" << "1" << "-colorspace" << "1";
    // audio: codec, metadata (AC-3 dialnorm/mixing level, AAC profile), optional single-pass loudness to -24 LKFS
    QString af = "aresample=async=1000"; if (e.audNormalize) af += ",loudnorm=I=-24:LRA=7:TP=-2"; af += ",alimiter=level_in=1:level_out=1:limit=0.95:attack=0.1:release=50";
    a << "-c:a" << e.audCodec << "-b:a" << QString::number(aud) + "k" << "-ar" << QString::number(e.audRate) << "-ac" << QString::number(e.audCh) << "-af" << af;
    if (e.audCodec == "ac3" || e.audCodec == "eac3") { a << "-dialnorm" << QString::number(e.dialnorm); if (e.mixingLevel >= 0) a << "-mixing_level" << QString::number(e.mixingLevel); }
    else if (e.audCodec == "aac") a << "-profile:a" << e.aacProfile;
    const QString url = svcMcastUrl(svc);
    a << "-streamid" << QString("0:%1").arg(svc.value("vidPid").toInt()) << "-streamid" << QString("1:%1").arg(svc.value("audPid").toInt()) << "-f" << "mpegts" << "-mpegts_service_id" << QString::number(svc.value("serviceId").toInt()) << "-mpegts_pmt_start_pid" << QString::number(e.ts.value("pmtPid").toInt(0x1000)) << "-mpegts_transport_stream_id" << QString::number(m_cfg.value("tsid").toInt(0x19));
    a << "-metadata" << "service_name=" + svc.value("longName").toString() << "-metadata" << "service_provider=" + e.ts.value("provider").toString("PlayPlay") << "-pcr_period" << QString::number(e.ts.value("pcrPeriodMs").toInt(20)) << "-pat_period" << QString::number(e.ts.value("patPeriodSec").toDouble(0.1));
    const qint64 muxrate = e.ts.value("muxrate").toDouble() > 0 ? qint64(e.ts.value("muxrate").toDouble()) : qint64((vid + aud) * 1000) * 115 / 100;
    a << "-muxrate" << QString::number(muxrate) << "-flush_packets" << "1" << url;
    return a;
}

void MuxManager::startEncoders() {
    stopEncoders();
    // Codec fallbacks need the encoder list: in-process, and only that part. A full capabilities() could start its background
    // `tsdektec -a -v` while the tsp we are about to start attaches the DekTec output; the old blocking probe ran strictly before it.
    if (!m_caps.contains("ffmpeg")) m_caps["ffmpeg"] = avkit::ffmpegCapabilities();
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); if (s.value("source").toObject().value("kind").toString() != "input") continue;
        if (inputViaEngine(s)) continue;     // DEP-18: its hidden channel's engine sends the service multicast
        QString err; const QStringList args = ffmpegArgs(s, &err);
        Encoder* e = new Encoder; e->serviceId = s.value("id").toString(); m_encoders.append(e);
        if (args.isEmpty()) { appendLog("encoder " + e->serviceId + ": " + err); raise("error", "mux: service " + e->serviceId + ": " + err, "enc_cfg_" + e->serviceId); continue; }
        if (!err.isEmpty()) { appendLog("encoder " + e->serviceId + ": " + err); raise("warning", "mux: service " + e->serviceId + ": " + err, "enc_fb_" + e->serviceId); }
        e->proc = new QProcess(this); e->startedMs = QDateTime::currentMSecsSinceEpoch();
        connect(e->proc, &QProcess::readyReadStandardError, this, [this, e]() { const QString s2 = QString::fromUtf8(e->proc->readAllStandardError()).trimmed(); if (!s2.isEmpty()) { e->lastLine = s2.section('\n', -1); appendLog("[" + e->serviceId + "] " + e->lastLine); } });
        e->proc->start(ffmpeg(), args); if (e->proc->waitForStarted(3000)) adopt(e->proc);
        appendLog("encoder " + e->serviceId + ": " + quoteArgs(ffmpeg(), args));
    }
}
void MuxManager::stopEncoders() { for (Encoder* e : m_encoders) { if (e->proc) { e->proc->kill(); e->proc->waitForFinished(2000); delete e->proc; } delete e; } m_encoders.clear(); }

// ---------------------------------------------------------------------------------------------
// tsp master (core/mux_backend.py _run_native_mux)
// ---------------------------------------------------------------------------------------------
QStringList MuxManager::tspArgs() const {
    const QString td = QDir::toNativeSeparators(tablesDir()); const QString bps = QString::number(transportBps());
    const QJsonObject ov = tableOverrides();
    // repetition: proven defaults unless the overrides set intervals (seconds -> TS packets between injections)
    const int psipInter = ov.value("mgtIntervalSec").toDouble() > 0 ? qBound(50, int(ov.value("mgtIntervalSec").toDouble() * kPacketsPerSec / 3.0), 20000) : 300;
    const int eit0Inter = ov.value("eitIntervalSec").toDouble() > 0 ? qBound(100, int(ov.value("eitIntervalSec").toDouble() * kPacketsPerSec), 60000) : 1200;
    const int eitNInter = ov.value("eitIntervalSec").toDouble() > 0 ? qBound(500, eit0Inter * 5, 120000) : 6000;
    QStringList a = {"--timed-log", "--verbose", "--max-flushed-packets", "300", "--max-input-packets", "300", "--max-output-packets", "300", "--bitrate", bps, "--buffer-size-mb", "8",
                     "-I", "null", "-P", "regulate", "--bitrate", bps,
                     "-P", "inject", td + "\\pat.xml", "--pid", "0x0000", "--inter-packet", "1500", "--poll-files"};
    QStringList psip = {td + "\\mgt.xml=100", td + "\\tvct.xml=300", td + "\\stt.xml=500"};
    if (QFileInfo::exists(QDir(tablesDir()).filePath("rrt.xml"))) psip << td + "\\rrt.xml=2000";
    a << "-P" << "inject" << psip << "--pid" << "0x1FFB" << "--inter-packet" << QString::number(psipInter) << "--poll-files";
    if (QFileInfo::exists(QDir(tablesDir()).filePath("custom.xml"))) a << "-P" << "inject" << td + "\\custom.xml" << "--pid" << ov.value("customPid").toString("0x1FFA") << "--inter-packet" << "3000" << "--poll-files";
    const int nEit = eitTables();
    for (int k = 0; k < nEit; ++k) a << "-P" << "inject" << td + QString("\\eit%1.xml").arg(k) << "--pid" << eitPid(k) << "--inter-packet" << QString::number(k == 0 ? eit0Inter : eitNInter) << "--poll-files";
    for (int k = 0; k < nEit; ++k) a << "-P" << "inject" << td + QString("\\ett%1.xml").arg(k) << "--pid" << ettPid(k) << "--inter-packet" << QString::number(k == 0 ? eit0Inter * 4 / 3 : eitNInter * 4 / 3) << "--poll-files";
    const QString tsp = tsduckTool("tsp.exe");
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); const EncView e = encView(s);
        const int pmt = 0x20 + s.value("serviceId").toInt();
        const QString src = s.value("source").toObject().value("kind").toString() == "udp" ? s.value("source").toObject().value("url").toString().remove("udp://").section('?', 0, 0) : svcMcastAddr(s);
        const qint64 merge = qint64((m_alloc.value(s.value("id").toString()).toDouble(e.vidKbps * 1000) + e.audKbps * 1000) * 1.20);
        const QString sub = QString("\"%1\" -I ip --default-interface --reuse-port --buffer-size 2097152 --receive-timeout 5000 %2 -P remap 0x1000=0x%3 -P filter --negate --pid 0 --pid 0x0011 --pid 0x1FFF -O file -").arg(tsp, src).arg(pmt, 4, 16, QChar('0'));
        a << "-P" << "merge" << "--bitrate" << QString::number(merge) << "--max-queue" << "20000" << "--incremental-pcr-restamp" << "--restart" << "--restart-interval" << "1000" << sub;
    }
    a << scte35::tspPluginArgs(scte35Config(), m_services);
    for (const QJsonValue& v : m_services) { const QJsonObject s = v.toObject(); a << "-P" << "bitrate_monitor" << "--pid" << QString::number(s.value("vidPid").toInt()) << "--pid" << QString::number(s.value("audPid").toInt()) << "--time-interval" << "5" << "--periodic-bitrate" << "5" << "--json-line=CHSTAT:" + s.value("id").toString(); }
    a << "-P" << "continuity" << "-P" << "pcradjust" << "--bitrate" << bps << "--min-ms-interval" << "40" << "-P" << "pcrverify" << "--bitrate" << bps << "--jitter-max" << "5000"
      << "-P" << "bitrate_monitor" << "--time-interval" << "5" << "--periodic-bitrate" << "5" << "--json-line=MUXSTAT" << "-P" << "count" << "--total" << "--interval" << "10000" << "--tag" << "MUXCOUNT";
    const QJsonObject out = m_cfg.value("output").toObject(); const QString kind = out.value("kind").toString("dektec");
    if (kind == "dektec") {
        a << "-O" << "dektec" << "--device" << QString::number(out.value("device").toInt(0)) << "--channel" << QString::number(out.value("channel").toInt(0)) << "--io-standard" << "ASI" << "--bitrate" << bps;
        const int preload = out.value("preloadMs").toInt(4000);
        if (preload > 0) { a << "--preload-fifo" << "--preload-fifo-delay" << QString::number(preload); if (out.value("maintainPreload").toBool(true)) a << "--maintain-preload"; }
        if (out.value("stuffing").toBool(true)) a << "--stuffing";
    }
    else if (kind == "udp") { const QString u = out.value("url").toString("udp://239.1.1.1:1234").remove("udp://").section('?', 0, 0); a << "-O" << "ip" << u << "--packet-burst" << "7"; if (out.value("ttl").toInt() > 0) a << "--ttl" << QString::number(out.value("ttl").toInt()); }
    else if (kind == "srt") {
        const QUrl u(out.value("url").toString("srt://0.0.0.0:9000?mode=listener")); const QUrlQuery q(u);
        const QString mode = q.queryItemValue("mode").isEmpty() ? out.value("srtMode").toString("caller") : q.queryItemValue("mode");
        a << "-O" << "srt"; if (mode == "listener") a << "--listener" << QString(":%1").arg(u.port(9000)); else a << "--caller" << QString("%1:%2").arg(u.host()).arg(u.port(9000));
        a << "--latency" << QString::number(out.value("latencyMs").toInt(q.queryItemValue("latency").toInt() > 0 ? q.queryItemValue("latency").toInt() / 1000 : 200)) << "--transtype" << "live";
        if (!q.queryItemValue("passphrase").isEmpty()) a << "--passphrase" << q.queryItemValue("passphrase");
    }
    else a << "-O" << "file" << QDir::toNativeSeparators(out.value("url").toString(QDir(m_dataDir).filePath("mux/out.ts")));
    return a;
}

// Phase 1 here, all in-process: allocation, tables (PlayPlayTsKit, no spawns by default), tsp present. Phase 2, DekTec output
// only: the port setup (`PlayPlayMux dektec prep`, else `tsdektec --output`) as an async child; the old code waited up to 10 s
// for tsdektec on the event-loop thread. Phase 3 (launchTsp): engine outputs, encoders, tsp, in the order they always ran.
bool MuxManager::start(QString* error) {
    if (running()) return true;
    if (m_services.isEmpty()) { if (error) *error = "no services configured"; finishStart(false, "no services configured"); return false; }
    m_wantRunning = true; m_state = "starting"; m_lastError.clear(); const quint64 gen = ++m_startGen; emit statusChanged();
    QString e; m_alloc = computeAllocation(&e); if (!e.isEmpty()) appendLog("statmux: " + e);
    if (!writeTables(&e) || !compileTables(&e)) { failStart(e, true, "mux: PSIP tables: " + e); if (error) *error = e; return false; }
    const QString tsp = tsduckTool("tsp.exe");
    if (!QFileInfo::exists(tsp)) { failStart("tsp.exe not found (set mux.config.tsduckDir)", false); if (error) *error = m_lastError; return false; }
    if (m_cfg.value("output").toObject().value("kind").toString("dektec") == "dektec") { prepareDektecAsync(gen); return true; }
    return launchTsp(gen, error);
}

void MuxManager::startAsync(StartDone done) {
    if (m_state == "running") { if (done) done(true, QString()); return; }
    if (done) m_startWaiters.append(std::move(done));
    if (m_state == "starting") return;          // joins the start in progress
    QString e; start(&e);                        // every outcome (now or after the port setup) answers the waiters
}

void MuxManager::finishStart(bool ok, const QString& error) {
    const QList<StartDone> w = std::move(m_startWaiters); m_startWaiters.clear();
    for (const StartDone& d : w) if (d) d(ok, error);
}

void MuxManager::failStart(const QString& error, bool alert, const QString& alertText) {
    m_state = "error"; m_lastError = error; m_wantRunning = false;
    if (alert) raise("error", alertText.isEmpty() ? "mux: " + error : alertText);
    emit statusChanged();
    finishStart(false, error);
}

// `PlayPlayMux dektec prep` exit -> what the start does: exit 0 ok; 64 / "unsupported" (a build without DTAPI) or no JSON at all
// (crash, foreign exe) -> run tsdektec as before; else the tool's own reason ("port 2 of DTA-2154 #0 is in use: ...").
QJsonObject MuxManager::prepVerdict(int exitCode, bool crashed, const QString& stdoutText, const QString& stderrText) {
    QJsonObject json;
    const QString t = stdoutText.trimmed(); const QString last = t.mid(t.lastIndexOf('\n') + 1).trimmed();
    if (last.startsWith('{')) json = QJsonDocument::fromJson(last.toUtf8()).object();
    QJsonObject v;
    if (!crashed && exitCode == 0 && json.value("ok").toBool()) { v["ok"] = true; v["actions"] = json.value("actions"); v["changed"] = json.value("changed"); return v; }
    if (exitCode == 64 || json.value("unsupported").toBool() || json.isEmpty()) {
        v["ok"] = false; v["fallback"] = true;
        v["reason"] = json.value("error").toString(crashed ? QString("PlayPlayMux crashed") : json.isEmpty() ? "no answer from PlayPlayMux: " + stderrText.trimmed().left(200) : QString("unsupported"));
        return v;
    }
    v["ok"] = false; v["error"] = json.value("error").toString(stderrText.trimmed().isEmpty() ? QString("PlayPlayMux dektec prep failed (exit %1)").arg(exitCode) : stderrText.trimmed());
    if (json.value("inUse").toBool()) v["inUse"] = true;
    return v;
}

// The port setup is the one tool step on the on-air path, so `auto` keeps tsdektec for it while tsp (TSDuck, which brings
// tsdektec) is the master; PlayPlayMux prepares the port with mux.config.tools = native, or when tsdektec.exe is missing.
bool MuxManager::nativePrep() const {
    const QString t = m_cfg.value("tools").toString("auto").toLower();
    if (t == "tsduck" || playplayMuxExe().isEmpty()) return false;
    return t == "native" || !QFileInfo::exists(tsduckTool("tsdektec.exe"));
}

void MuxManager::prepareDektecAsync(quint64 gen) {
    const QJsonObject out = m_cfg.value("output").toObject();
    const QString mux = nativePrep() ? playplayMuxExe() : QString();
    if (mux.isEmpty()) { prepWithTsdektec(gen); return; }
    // PlayPlayMux refuses a port whose channel any process has attached (an engine's ASI sink, a leftover tsp) instead of
    // reconfiguring it under that user, sets IODIR OUTPUT + IOSTD ASI only where they differ (no write at all when the port is
    // already an ASI output) and reads the result back.
    const QStringList a = {"dektec", "prep", "--port", QString::number(out.value("port").toInt(2)), "--device", QString::number(out.value("device").toInt(0))};
    appendLog("dektec: " + quoteArgs(mux, a));
    m_prep = runTool("dektec-prep", mux, a, 10000, false, [this, gen](const ToolResult& t) {
        m_prep = nullptr;
        if (gen != m_startGen || !m_wantRunning) return;         // stopped (or restarted) meanwhile
        QJsonObject v = t.startError.isEmpty() ? prepVerdict(t.exitCode, t.crashed, t.out, t.err) : QJsonObject{{"fallback", true}, {"reason", "PlayPlayMux did not start: " + t.startError}};
        if (t.timedOut) v = QJsonObject{{"error", "PlayPlayMux dektec prep did not finish within 10 s"}};
        if (v.value("ok").toBool()) {
            const QJsonArray act = v.value("actions").toArray();
            appendLog(act.isEmpty() ? QString("dektec: output port ready (nothing to change)") : "dektec: output port set up: " + QString::fromUtf8(QJsonDocument(act).toJson(QJsonDocument::Compact)));
            QString e; launchTsp(gen, &e); return;
        }
        if (v.value("fallback").toBool()) { appendLog("dektec: PlayPlayMux cannot prepare the port (" + v.value("reason").toString() + "); using tsdektec"); prepWithTsdektec(gen); return; }
        failStart("the DekTec output port is not ready: " + v.value("error").toString(), true);
    });
}

void MuxManager::prepWithTsdektec(quint64 gen) {
    const QJsonObject out = m_cfg.value("output").toObject();
    m_prep = runTool("dektec-prep", tsduckTool("tsdektec.exe"), {"--output", QString::number(out.value("port").toInt(2)), QString::number(out.value("device").toInt(0))}, 10000, true, [this, gen](const ToolResult& t) {
        m_prep = nullptr;
        if (gen != m_startGen || !m_wantRunning) return;
        if (t.startError.isEmpty() && !t.crashed && !t.timedOut && t.exitCode == 0) { QString e; launchTsp(gen, &e); return; }
        const QString d = t.out.trimmed().isEmpty() ? t.startError : t.out.trimmed();
        failStart("tsdektec could not configure the output port: " + d, true);
    });
}

bool MuxManager::launchTsp(quint64 gen, QString* error) {
    if (gen != m_startGen || !m_wantRunning) return false;
    const QString tsp = tsduckTool("tsp.exe"); const QJsonObject out = m_cfg.value("output").toObject();
    emit servicesChanged();           // Supervisor pushes the UDP outputs to channel engines with the current allocation
    startEncoders();
    m_tsp = new QProcess(this);
    m_tsp->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_tsp, &QProcess::readyReadStandardError, this, [this]() { m_stderrBuf += m_tsp->readAllStandardError(); int nl; while ((nl = m_stderrBuf.indexOf('\n')) >= 0) { onTspLine(QString::fromUtf8(m_stderrBuf.left(nl)).trimmed()); m_stderrBuf.remove(0, nl + 1); } });
    connect(m_tsp, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MuxManager::onTspFinished);
    const QStringList args = tspArgs();
    appendLog("tsp " + args.join(' '));
    m_runningArgs = args; m_breaks.clear();
    m_tsp->start(tsp, args);
    if (!m_tsp->waitForStarted(5000)) { failStart("tsp did not start", false); if (error) *error = m_lastError; return false; }   // Windows: CreateProcess already returned; no wait
    adopt(m_tsp);
    m_state = "running"; m_startedMs = QDateTime::currentMSecsSinceEpoch(); m_metrics = QJsonObject(); m_metrics["fifo"] = out.value("kind").toString("dektec") == "dektec" ? "preloading" : "n/a"; m_metrics["continuity"] = "OK"; m_metrics["pcr"] = "OK"; m_metrics["patPmt"] = "OK";
    raise("info", QString("mux: started with %1 service(s) → %2 at %3 b/s").arg(m_services.size()).arg(out.value("kind").toString("dektec")).arg(transportBps()));
    emit statusChanged();
    finishStart(true, QString());
    return true;
}

void MuxManager::stop() {
    m_wantRunning = false; ++m_startGen;
    // a port setup still running: its result is stale now; it finishes on its own (killed, deleted when it exits)
    if (QProcess* p = m_prep) { m_prep = nullptr; p->disconnect(this); connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), p, &QObject::deleteLater); p->kill(); }
    // Detach first: waitForFinished() delivers QProcess::finished synchronously, and onTspFinished() used to null m_tsp under
    // our feet (the next line then dereferenced a null pointer: Stop / Restart mux crashed the Supervisor).
    if (QProcess* p = m_tsp) { m_tsp = nullptr; p->disconnect(this); p->kill(); p->waitForFinished(3000); p->deleteLater(); }
    stopEncoders();
    if (m_state != "stopped") { m_state = "stopped"; appendLog("mux stopped"); emit statusChanged(); }
    finishStart(false, "the mux was stopped before it started");
}

void MuxManager::onTspFinished(int code, QProcess::ExitStatus) {
    if (m_tsp) { m_tsp->deleteLater(); m_tsp = nullptr; }
    if (!m_wantRunning) return;
    ++m_restarts;
    m_state = "error"; m_lastError = QString("tsp exited rc=%1").arg(code);
    raise(m_restarts >= m_cfg.value("alerts").toObject().value("restartThreshold").toInt(3) ? "critical" : "warning", QString("mux: tsp exited (rc=%1), restart #%2 in 3 s").arg(code).arg(m_restarts), "mux_restart");
    emit statusChanged();
    // Only the restart this exit armed. A Start / Restart / Stop since then bumped m_startGen and owns the mux now; above all a
    // DekTec start whose port setup is still running must not be joined with m_wantRunning cleared: its answer would then see
    // !m_wantRunning, start nothing and leave the mux (and every waiter) in "starting" for good.
    const quint64 gen = m_startGen;
    QTimer::singleShot(3000, this, [this, gen]() {
        if (gen != m_startGen || !m_wantRunning || m_tsp || m_prep || m_state == "starting") return;
        m_wantRunning = false; startAsync([this](bool ok, const QString& e) { if (!ok) appendLog("restart failed: " + e); });
    });
}

void MuxManager::onTspLine(const QString& line) {
    if (line.isEmpty()) return;
    static const QRegularExpression rxJson("(MUXSTAT|CHSTAT:([^\\s{]+))(\\{.*\\})");
    const auto m = rxJson.match(line);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m.hasMatch()) {
        const QJsonObject p = QJsonDocument::fromJson(m.captured(3).toUtf8()).object();
        const qint64 br = qint64(p.value("bitrate").toDouble());
        m_metrics["lastMetricMs"] = double(now);
        if (m.captured(1) == "MUXSTAT") {
            m_metrics["muxBitrate"] = double(br); m_metrics["stuffing"] = p.value("stuffing").toDouble();
            if (now - m_lastCcErrorMs >= 4000) m_metrics["continuity"] = "OK";
            if (now - m_lastPcrErrorMs >= 4000) m_metrics["pcr"] = "OK";
            if (m_lastFifoUnderrunMs && now - m_lastFifoUnderrunMs >= 4000) { m_lastFifoUnderrunMs = 0; m_metrics["fifo"] = "ok"; }
            const double dev = qAbs(double(br) - double(transportBps())) * 100.0 / double(transportBps());
            if (dev > m_cfg.value("alerts").toObject().value("bitrateDeviationPct").toInt(12) && now - m_startedMs > 20000) raise("warning", QString("mux: transport bitrate %1 b/s deviates %2% from %3").arg(br).arg(dev, 0, 'f', 1).arg(transportBps()), "mux_bitrate");
        } else {
            QJsonObject per = m_metrics.value("perService").toObject(); QJsonObject s = per.value(m.captured(2)).toObject(); s["bitrate"] = double(br); s["atMs"] = double(now); per[m.captured(2)] = s; m_metrics["perService"] = per;
            m_lastSeenBps[m.captured(2)] = br;
            if (br == 0 && now - m_startedMs > 20000) raise("warning", "mux: service " + m.captured(2) + " carries no bitrate (encoder down?)", "svc_zero_" + m.captured(2));
        }
        emit statusChanged();
        return;
    }
    static const QRegularExpression rxCount("MUXCOUNT:.*?total:\\s*([\\d,]+)\\s+packets");
    if (const auto c = rxCount.match(line); c.hasMatch()) { m_metrics["packets"] = double(QString(c.captured(1)).remove(',').toLongLong()); return; }
    if (line.contains("resetting PCR restamping", Qt::CaseInsensitive) || line.contains("distinct time sources", Qt::CaseInsensitive)) return;
    static const QRegularExpression rxPre("preload FIFO size:?\\s*([\\d,]+)\\s*bytes|preload FIFO size adjusted:\\s*([\\d,]+)\\s*bytes", QRegularExpression::CaseInsensitiveOption);
    if (const auto f = rxPre.match(line); f.hasMatch()) { m_metrics["fifoTarget"] = double(QString(f.captured(1).isEmpty() ? f.captured(2) : f.captured(1)).remove(',').toLongLong()); m_metrics["fifo"] = "preloading"; }
    static const QRegularExpression rxLoad("output FIFO load is\\s*([\\d,]+)\\s*bytes, starting transmission", QRegularExpression::CaseInsensitiveOption);
    if (const auto f = rxLoad.match(line); f.hasMatch()) { m_metrics["fifoLoad"] = double(QString(f.captured(1)).remove(',').toLongLong()); m_lastFifoUnderrunMs = 0; m_metrics["fifo"] = "ok"; }
    if (line.contains("Pausing transmission temporarily", Qt::CaseInsensitive) || line.contains(QRegularExpression("Got (FIFO|CPU|DMA) underflow", QRegularExpression::CaseInsensitiveOption))) {
        m_lastFifoUnderrunMs = now; m_metrics["fifo"] = "underrun";
        if (m_cfg.value("alerts").toObject().value("checkFifo").toBool(true)) raise("critical", "mux: DTA ASI output FIFO underrun — hardware cushion drained, refilling preload", "dta_fifo");
    }
    if (line.contains(QRegularExpression("continuity:\\s*packet index", QRegularExpression::CaseInsensitiveOption))) { m_lastCcErrorMs = now; m_metrics["continuity"] = "ERROR"; }
    if (line.contains(QRegularExpression("pcrverify:.*PCR jitter", QRegularExpression::CaseInsensitiveOption))) { m_lastPcrErrorMs = now; m_metrics["pcr"] = "ERROR"; }
    if (line.contains(QRegularExpression("PAT.*error|PMT.*error", QRegularExpression::CaseInsensitiveOption))) m_metrics["patPmt"] = "ERROR";
    appendLog(line);
}

// ---------------------------------------------------------------------------------------------
// Capabilities (ffmpeg encoders, TSDuck, DekTec), dry run
// ---------------------------------------------------------------------------------------------
QString MuxManager::quoteArgs(const QString& exe, const QStringList& args) {
    auto q = [](const QString& s) { return s.isEmpty() || s.contains(' ') || s.contains('"') ? "\"" + QString(s).replace("\"", "\\\"") + "\"" : s; };
    QStringList all; all << q(QDir::toNativeSeparators(exe)); for (const QString& a : args) all << q(a); return all.join(' ');
}

// The listing part of `tsdektec -a -v`, shared by the blocking dektecDevices() and capabilities()' background probe.
QJsonObject MuxManager::parseTsdektec(const QString& text) {
    QJsonArray devices; const QStringList lines = text.split('\n');
    static const QRegularExpression rxDev("^\\*?\\s*(?:Device )?(\\d+): (\\S+) \\((.*)\\)\\s*$"), rxPorts("^\\s+Physical ports: (\\d+)"), rxChans("^\\s+Channels: input: (\\d+), output: (\\d+)"),
        rxChan("^\\s+(Input|Output) (\\d+): \\S+ port (\\d+)(?: \\(([^)]*)\\))?, ([^(]*)"), rxSerial("^\\s+Serial number: (\\d+)\\s*$"), rxFw("^\\s+Firmware version: (\\d+)"), rxDtapi("^DTAPI: ([\\d.]+), Dta: ([\\d.]+)");
    QJsonObject cur; QJsonArray chans; QString dtapi, dta;
    auto flush = [&]() { if (!cur.isEmpty()) { cur["channels"] = chans; devices.append(cur); } cur = QJsonObject(); chans = QJsonArray(); };
    for (const QString& l0 : lines) {
        const QString l = QString(l0).remove('\r');
        if (auto m = rxDtapi.match(l); m.hasMatch()) { dtapi = m.captured(1); dta = m.captured(2); continue; }
        if (auto m = rxDev.match(l); m.hasMatch()) { flush(); cur["index"] = m.captured(1).toInt(); cur["model"] = m.captured(2); cur["description"] = m.captured(3); continue; }
        if (cur.isEmpty()) continue;
        if (auto m = rxPorts.match(l); m.hasMatch()) cur["ports"] = m.captured(1).toInt();
        else if (auto m = rxChans.match(l); m.hasMatch()) { cur["inputs"] = m.captured(1).toInt(); cur["outputs"] = m.captured(2).toInt(); }
        else if (auto m = rxChan.match(l); m.hasMatch()) { QJsonObject c; c["kind"] = m.captured(1).toLower(); c["index"] = m.captured(2).toInt(); c["port"] = m.captured(3).toInt(); if (!m.captured(4).isEmpty()) c["label"] = m.captured(4); c["standards"] = m.captured(5).trimmed().remove(QRegularExpression(",\\s*$")); chans.append(c); }
        else if (auto m = rxSerial.match(l); m.hasMatch()) cur["serial"] = m.captured(1);
        else if (auto m = rxFw.match(l); m.hasMatch()) cur["firmware"] = m.captured(1).toInt();
    }
    flush();
    QJsonObject dk; dk["devices"] = devices; dk["dtapi"] = dtapi; dk["dta"] = dta; if (devices.isEmpty()) dk["raw"] = lines.mid(0, 20).join('\n');
    return dk;
}

// Never waits (DEP-15; the route used to block up to 15 s in waitForFinished): a fresh listing from the cache, else the previous
// list while a background listing runs. Every result carries its own probedMs; capabilities()' placeholder (no result yet) has none.
QJsonArray MuxManager::dektecDevices(bool refresh) {
    const QJsonObject cached = m_caps.value("dektec").toObject();
    const double cachedMs = cached.value("probedMs").toDouble(0.0);
    if (!refresh && cachedMs > 0 && cached.value("found").toBool(true) && QDateTime::currentMSecsSinceEpoch() - qint64(cachedMs) < 600000) return cached.value("devices").toArray();
    probeDektecAsync();
    return m_caps.value("dektec").toObject().value("devices").toArray();   // found:false is recorded at once when no tool exists
}

void MuxManager::dektecDevicesAsync(bool refresh, std::function<void(const QJsonObject&)> done) {
    const QJsonObject cached = m_caps.value("dektec").toObject();
    const double cachedMs = cached.value("probedMs").toDouble(0.0);
    // refresh=0 (both UIs when the Output tab opens) takes a listing younger than 10 min at once, even while a newer one runs
    if (!refresh && cachedMs > 0 && cached.value("found").toBool(true) && QDateTime::currentMSecsSinceEpoch() - qint64(cachedMs) < 600000) { done(cached); return; }
    m_dektecWaiters.append(std::move(done));
    probeDektecAsync();                          // joins a listing in progress; answers at once when no tool exists
}

void MuxManager::setDektecResult(const QJsonObject& dk) {
    m_caps["dektec"] = dk;
    const auto w = std::move(m_dektecWaiters); m_dektecWaiters.clear();
    for (const auto& d : w) if (d) d(dk);
}

// `PlayPlayMux dektec list -v`: the tsdektec -a -v text on stdout, the JSON with the same device keys (+ portConfig, name, caps ...)
// on the last line. A PlayPlayMux without DTAPI answers "available": false: the caller then asks tsdektec, as before.
QJsonObject MuxManager::parseDektecList(int exitCode, const QString& stdoutText) {
    const QString t = stdoutText.trimmed(); const int nl = t.lastIndexOf('\n');
    const QJsonObject json = QJsonDocument::fromJson(t.mid(nl + 1).trimmed().toUtf8()).object();
    QJsonObject dk;
    if (json.isEmpty() || !json.value("ok").toBool() || !json.value("available").toBool(true) || exitCode != 0) {
        dk["fallback"] = true; dk["reason"] = json.isEmpty() ? QString("no JSON answer (exit %1)").arg(exitCode) : json.value("note").toString(json.value("error").toString("DTAPI not available")); return dk;
    }
    dk["devices"] = json.value("devices").toArray(); dk["dtapi"] = json.value("dtapi").toString(); dk["dta"] = json.value("dta").toString();
    if (json.contains("scanError")) dk["error"] = json.value("scanError");
    if (dk.value("devices").toArray().isEmpty()) dk["raw"] = (nl < 0 ? QString() : t.left(nl)).split('\n').mid(0, 20).join('\n');
    return dk;
}

// capabilities() and the routes never wait for the listing (0.3 s with the DTA-2154 here, 15 s when a driver hangs, all of it on
// the main thread that serves REST/IPC): it runs in the background and replaces m_caps["dektec"] when it ends. One at a time.
void MuxManager::probeDektecAsync() {
    if (m_dektecProbe) return;
    const bool native = toolsEngine() == "native" && !playplayMuxExe().isEmpty();
    if (!native && !QFileInfo::exists(tsduckTool("tsdektec.exe"))) {   // no tool: nothing to start
        QJsonObject dk; dk["devices"] = QJsonArray(); dk["found"] = false; dk["tsdektec"] = tsduckTool("tsdektec.exe"); dk["probedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        if (toolsEngine() == "native") dk["error"] = "PlayPlayMux.exe not found (mux.config.playplayMux)";
        setDektecResult(dk); return;
    }
    probeDektecWith(native);
}

void MuxManager::probeDektecWith(bool native) {
    const QString exe = native ? playplayMuxExe() : tsduckTool("tsdektec.exe");
    const QStringList args = native ? QStringList{"dektec", "list", "-v"} : QStringList{"-a", "-v"};
    m_dektecProbe = runTool("dektec-probe", exe, args, 15000, !native, [this, native, exe](const ToolResult& t) {
        m_dektecProbe = nullptr;
        QJsonObject dk;
        if (native) {
            dk = t.startError.isEmpty() && !t.crashed && !t.timedOut ? parseDektecList(t.exitCode, t.out) : QJsonObject{{"fallback", true}, {"reason", t.timedOut ? QString("did not finish within 15 s") : t.crashed ? QString("crashed") : t.startError}};
            if (dk.value("fallback").toBool() && QFileInfo::exists(tsduckTool("tsdektec.exe"))) { appendLog("dektec list: PlayPlayMux " + dk.value("reason").toString() + "; using tsdektec"); probeDektecWith(false); return; }
            dk.remove("fallback");
            if (dk.contains("reason")) { dk["error"] = dk.take("reason"); dk["devices"] = QJsonArray(); }
        } else if (!t.startError.isEmpty()) { dk["devices"] = QJsonArray(); dk["dtapi"] = QString(); dk["dta"] = QString(); dk["error"] = t.startError; }   // FailedToStart
        else {
            dk = parseTsdektec(t.out);
            if (t.crashed || t.timedOut) dk["error"] = t.timedOut ? QString("tsdektec did not finish within 15 s") : QString("tsdektec crashed");
        }
        dk["exitCode"] = t.exitCode; dk["engine"] = native ? "native" : "tsduck"; dk["tool"] = exe; dk["tsdektec"] = tsduckTool("tsdektec.exe"); dk["probedMs"] = double(QDateTime::currentMSecsSinceEpoch());
        setDektecResult(dk);
    });
}

// One child, asynchronously (DEP-15): `started` adopts it into the kill-on-close job, `finished` / FailedToStart answer `done`
// exactly once with the whole output, a timer kills it after timeoutMs. Never waits; `done` always runs in a later event-loop
// turn (FailedToStart is queued), so a caller can store the returned pointer before its callback clears it.
QProcess* MuxManager::runTool(const QString& name, const QString& exe, const QStringList& args, int timeoutMs, bool merged, std::function<void(const ToolResult&)> done) {
    QProcess* p = new QProcess(this);
    p->setObjectName(name);
    p->setProcessChannelMode(merged ? QProcess::MergedChannels : QProcess::SeparateChannels);
    auto fired = std::make_shared<bool>(false);
    const QPointer<QProcess> pp(p);
    auto finish = [pp, done, fired](ToolResult r) {
        if (*fired) return; *fired = true;
        // stderr only when kept separate: Qt warns "readAllStandardError: Called with MergedChannels" (one log line per probe)
        if (pp) { r.out = QString::fromUtf8(pp->readAllStandardOutput()); if (pp->processChannelMode() != QProcess::MergedChannels) r.err = QString::fromUtf8(pp->readAllStandardError()); r.timedOut = pp->property("timedOut").toBool(); pp->deleteLater(); }
        done(r);
    };
    connect(p, &QProcess::started, this, [this, p]() { adopt(p); });
    connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [finish](int code, QProcess::ExitStatus st) { ToolResult r; r.exitCode = code; r.crashed = st != QProcess::NormalExit; finish(r); });
    connect(p, &QProcess::errorOccurred, this, [pp, finish](QProcess::ProcessError e) { if (e != QProcess::FailedToStart) return; ToolResult r; r.startError = pp ? pp->errorString() : QString("failed to start"); finish(r); }, Qt::QueuedConnection);
    QTimer::singleShot(timeoutMs, p, [p]() { if (p->state() != QProcess::NotRunning) { p->setProperty("timedOut", true); p->kill(); } });
    p->start(exe, args);
    return p;
}

// mux.config.tools: "auto" (default) = PlayPlayMux when found, else TSDuck; "native" = PlayPlayMux (an "unsupported" answer still
// falls back to the TSDuck tool); "tsduck" = TSDuck only. PlayPlayMux is EXCLUDE_FROM_ALL, but a tree configured with
// PLAYPLAY_BUILD_TESTS=ON (the live build folder is) builds it with test_inhouse_psip / test_inhouse_tsanalyzer, so its next full
// build puts PlayPlayMux.exe next to the Supervisor and `auto` then lists DekTec devices and analyses through it; the port setup
// stays on tsdektec while that exists (nativePrep). Only a tree that never built PlayPlayMux keeps the TSDuck tools throughout.
QString MuxManager::playplayMuxExe() const {
    const QString c = m_cfg.value("playplayMux").toString(); if (!c.isEmpty()) return QFileInfo(c).isFile() ? c : QString();
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayMux.exe");
    if (QFileInfo(local).isFile()) return local;
    return QStandardPaths::findExecutable("PlayPlayMux.exe");   // the full name: no PATHEXT walk (see tsduckTool)
}
QString MuxManager::toolsEngine() const {
    const QString t = m_cfg.value("tools").toString("auto").toLower();
    if (t == "tsduck") return "tsduck";
    if (t == "native") return "native";
    return playplayMuxExe().isEmpty() ? "tsduck" : "native";
}

// The async replacements of three routes registered in supervisorcore_library.cpp (registerRoutes runs before laneext::installAll,
// and registering the same "METHOD /path" again replaces the handler). Same replies as before: /mux/dektec {devices, dtapi, dta,
// tsdektec, raw?}; /mux/start and /mux/restart the status, or 409 with the reason (now also when the port setup fails).
void MuxManager::installAsyncRoutes(RestApiServer* rest, MuxManager* mux0) {
    if (!rest || !mux0) return;
    QPointer<MuxManager> mux(mux0);
    using Q = QHash<QString, QString>;
    rest->registerAsync("GET", "/api/v1/mux/dektec", [mux](const QJsonObject&, const Q& q, RestApiServer::Reply reply) {
        if (!mux) { reply(503, QJsonObject{{"error", "mux manager is gone"}}); return; }
        mux->dektecDevicesAsync(q.value("refresh", "1") == "1", [reply](const QJsonObject& dk) {
            QJsonObject o; o["devices"] = dk.value("devices").toArray(); o["dtapi"] = dk.value("dtapi"); o["dta"] = dk.value("dta"); o["tsdektec"] = dk.value("tsdektec");
            for (const char* k : {"raw", "engine", "tool", "error", "found", "probedMs"}) if (dk.contains(k)) o[k] = dk.value(k);
            reply(200, o);
        });
    });
    auto started = [mux](RestApiServer::Reply reply) {
        return [mux, reply](bool ok, const QString& e) { if (!ok) reply(409, QJsonObject{{"error", e}}); else reply(200, mux ? mux->status() : QJsonObject()); };
    };
    rest->registerAsync("POST", "/api/v1/mux/start", [mux, started](const QJsonObject&, const Q&, RestApiServer::Reply reply) {
        if (!mux) { reply(503, QJsonObject{{"error", "mux manager is gone"}}); return; }
        mux->startAsync(started(reply));
    });
    rest->registerAsync("POST", "/api/v1/mux/restart", [mux, started](const QJsonObject&, const Q&, RestApiServer::Reply reply) {
        if (!mux) { reply(503, QJsonObject{{"error", "mux manager is gone"}}); return; }
        mux->stop(); mux->startAsync(started(reply));
    });
    qInfo().noquote() << "[Supervisor] mux: asynchronous GET /api/v1/mux/dektec, POST /api/v1/mux/start, /mux/restart (DEP-15)";
}

#ifdef Q_OS_WIN
// The version text of an executable's version resource (StringFileInfo ProductVersion, else FileVersion). TSDuck stamps the
// same "3.41-4299" there that `tsp --version` ends with, so capabilities() reads it without starting tsp.
static QString exeVersionText(const QString& path) {
    const std::wstring wp = QDir::toNativeSeparators(path).toStdWString();
    DWORD handle = 0; const DWORD size = GetFileVersionInfoSizeW(wp.c_str(), &handle);
    if (!size) return {};
    QByteArray buf(int(size), '\0');
    if (!GetFileVersionInfoW(wp.c_str(), 0, size, buf.data())) return {};
    struct LangCp { WORD lang, cp; }; LangCp* tr = nullptr; UINT trBytes = 0;
    QStringList blocks;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trBytes) && tr)
        for (UINT i = 0; i < trBytes / sizeof(LangCp); ++i) blocks << QString("\\StringFileInfo\\%1%2\\").arg(tr[i].lang, 4, 16, QChar('0')).arg(tr[i].cp, 4, 16, QChar('0'));
    blocks << "\\StringFileInfo\\040904b0\\" << "\\StringFileInfo\\040904e4\\";
    for (const char* key : {"ProductVersion", "FileVersion"})
        for (const QString& b : blocks) {
            wchar_t* v = nullptr; UINT len = 0;
            if (VerQueryValueW(buf.data(), (b + key).toStdWString().c_str(), reinterpret_cast<void**>(&v), &len) && v && len > 0) {
                const QString s = QString::fromWCharArray(v, int(len)).section(QChar(0), 0, 0).trimmed();
                if (!s.isEmpty()) return s;
            }
        }
    return {};
}
#else
static QString exeVersionText(const QString&) { return {}; }
#endif

// `ffmpeg -encoders` rows are " <type><5 capability flags> <name> <description>", flags from ".FSXBD". The old pattern left out
// 'S' (slice threading), so mpeg2video ("V.S..D") never matched and every mpeg2 input service fell back to libx264 once the
// capabilities were probed (startEncoders probes first). The legend above the "------" line ("V..... = Video") is skipped.
QJsonObject MuxManager::parseFfmpegEncoders(const QString& text) {
    static const QRegularExpression rx("^ ([VAS])[.A-Z]{5} (\\S+)\\s+(.*)$");
    QJsonArray video, audio; QJsonObject has;
    QStringList lines = text.split('\n'); for (QString& l : lines) l.remove('\r');
    int from = 0; for (int i = 0; i < lines.size(); ++i) if (lines[i].trimmed().startsWith("------")) { from = i + 1; break; }
    for (int i = from; i < lines.size(); ++i) {
        const auto m = rx.match(lines[i]); if (!m.hasMatch() || m.captured(2) == "=") continue;
        QJsonObject e; e["name"] = m.captured(2); e["description"] = m.captured(3).trimmed();
        if (m.captured(1) == "V") video.append(e); else if (m.captured(1) == "A") audio.append(e);
        has[m.captured(2)] = true;
    }
    QJsonObject r; r["video"] = video; r["audio"] = audio; r["has"] = has; return r;
}

QJsonObject MuxManager::capabilities(bool refresh) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!refresh && !m_caps.isEmpty() && m_caps.contains("ffmpeg") && now - qint64(m_caps.value("probedMs").toDouble()) < 600000) return m_caps;
    QJsonObject c;
    // --- ffmpeg: encoders + build configuration, in-process (DEP-02b) --------------------------------
    // PlayPlayAvKit reads the libav this Supervisor links (av_codec_iterate, avcodec_configuration) and builds the object the
    // `ffmpeg -encoders` / `-version` spawns gave ({path, found, version, video[], audio[], has{}, build{}}; path "(in-process
    // libav)"): those spawns waited up to 25 s on the main thread, and every refresh, dry run and mux start paid them. It is the
    // FFmpeg build staged with the engines and next to this exe, i.e. the ffmpeg.exe the `input` services run; a
    // mux.config.ffmpegPath pointing at another build is not what this list describes.
    c["ffmpeg"] = avkit::ffmpegCapabilities();
    // --- TSDuck: file checks and tsp.exe's version resource. Nothing is started, so a missing TSDuck costs nothing. -------------
    QJsonObject td; td["dir"] = m_cfg.value("tsduckDir").toString(); QJsonObject tools; for (const char* t : {"tsp.exe", "tstabcomp.exe", "tsdektec.exe", "tsanalyze.exe", "tspacketize.exe"}) { const QString pth = tsduckTool(t); QJsonObject x; x["path"] = pth; x["found"] = QFileInfo::exists(pth); tools[QString(t).chopped(4)] = x; } td["tools"] = tools;
    { const QJsonObject tsp = tools.value("tsp").toObject(); const bool found = tsp.value("found").toBool(); td["found"] = found; td["version"] = found ? exeVersionText(tsp.value("path").toString()) : QString(); }
    c["tsduck"] = td;
    // --- DekTec: the last `tsdektec -a -v` result; a refresh (or a result older than 10 min) re-probes in the background --------
    { const double dkMs = m_caps.value("dektec").toObject().value("probedMs").toDouble(0.0);
      if (refresh || dkMs <= 0 || now - qint64(dkMs) >= 600000) probeDektecAsync(); }
    QJsonObject dk = m_caps.value("dektec").toObject();      // probeDektecAsync() records found:false at once when no tool exists
    if (m_dektecProbe) {
        dk["probing"] = true;
        if (!dk.contains("devices")) {   // first probe still running: both UIs print `raw` under "no DekTec device found"
            dk["devices"] = QJsonArray(); dk["tsdektec"] = tsduckTool("tsdektec.exe");
            dk["raw"] = QString("%1 is running in the background; Re-probe in a moment shows the devices").arg(m_dektecProbe->arguments().value(0) == "dektec" ? "PlayPlayMux dektec list" : "tsdektec -a -v");
        }
    }
    c["dektec"] = dk;
    // In-house tools (DEP-15): which engine the analyzer / DekTec tool and the table compiler use. A file check, nothing started.
    { QJsonObject pm; const QString exe = playplayMuxExe(); pm["path"] = exe; pm["found"] = !exe.isEmpty(); pm["tools"] = toolsEngine(); pm["toolsSetting"] = m_cfg.value("tools").toString("auto"); pm["tables"] = tablesEngine();
      c["playplayMux"] = pm; }
    QJsonArray names; for (const QJsonValue& p : encoderPresets()) names.append(p.toObject().value("id"));
    c["presets"] = names; c["probedMs"] = double(now); c["transportBps"] = double(transportBps());
    m_caps = c; return m_caps;
}

QJsonObject MuxManager::dryRun() {
    QJsonObject r; QString e; QJsonArray warnings;
    capabilities();
    m_alloc = computeAllocation(&e); if (!e.isEmpty()) warnings.append("statmux: " + e);
    r["allocation"] = m_alloc; r["transportBps"] = double(transportBps()); r["services"] = m_services.size();
    QString te; writeTables(&te); if (!te.isEmpty()) warnings.append(te);
    const QString tsp = tsduckTool("tsp.exe"); if (!QFileInfo::exists(tsp)) warnings.append("tsp.exe not found at " + tsp);
    if (tablesEngine() == "tstabcomp" && !QFileInfo::exists(tsduckTool("tstabcomp.exe"))) warnings.append("tstabcomp.exe not found (mux.config.tables = tstabcomp)");
    const QString pmux = playplayMuxExe();
    if (m_cfg.value("tools").toString() == "native" && pmux.isEmpty()) warnings.append("PlayPlayMux.exe not found (mux.config.tools = native)");
    { QJsonObject tl; tl["engine"] = toolsEngine(); tl["playplayMux"] = pmux; tl["tables"] = tablesEngine(); tl["portSetup"] = nativePrep() ? "native" : "tsduck"; r["tools"] = tl; }
    const QStringList args = tspArgs();
    QJsonObject t; t["exe"] = tsp; QJsonArray aa; for (const QString& a : args) aa.append(a); t["args"] = aa; t["commandLine"] = quoteArgs(tsp, args); r["tsp"] = t;
    const QJsonObject out = m_cfg.value("output").toObject();
    if (out.value("kind").toString("dektec") == "dektec") {
        const QString port = QString::number(out.value("port").toInt(2)), dev = QString::number(out.value("device").toInt(0));
        r["dektecPrep"] = nativePrep() ? quoteArgs(pmux, {"dektec", "prep", "--port", port, "--device", dev}) : quoteArgs(tsduckTool("tsdektec.exe"), {"--output", port, dev});
    }
    QJsonArray encs, chans;
    for (const QJsonValue& v : m_services) {
        const QJsonObject s = v.toObject(); const QString kind = s.value("source").toObject().value("kind").toString();
        if (kind == "input" && !inputViaEngine(s)) { QString err; const QStringList fa = ffmpegArgs(s, &err); QJsonObject o; o["serviceId"] = s.value("id"); o["exe"] = ffmpeg(); QJsonArray x; for (const QString& a : fa) x.append(a); o["args"] = x; o["commandLine"] = fa.isEmpty() ? QString() : quoteArgs(ffmpeg(), fa); o["error"] = err; if (!err.isEmpty()) warnings.append("service " + s.value("id").toString() + ": " + err); encs.append(o); }
        else if (!feedChannelFor(s).isEmpty()) { QJsonObject o; o["serviceId"] = s.value("id"); o["channelId"] = feedChannelFor(s); o["managed"] = kind == "ndi" || inputViaEngine(s); o["outputs"] = engineOutputsFor(feedChannelFor(s), QJsonArray()); if (o["managed"].toBool()) o["fixedSource"] = fixedSourceFor(s); chans.append(o); }
        else { QJsonObject o; o["serviceId"] = s.value("id"); o["merge"] = s.value("source").toObject().value("url"); chans.append(o); }
    }
    r["encoders"] = encs; r["channels"] = chans;
    QJsonObject tb; tb["dir"] = QDir::toNativeSeparators(tablesDir()); tb["eitTables"] = eitTables(); QJsonArray files; for (const QFileInfo& fi : QDir(tablesDir()).entryInfoList({"*.xml"}, QDir::Files, QDir::Name)) files.append(fi.fileName()); tb["files"] = files; r["tables"] = tb;
    r["warnings"] = warnings; r["output"] = out;
    return r;
}

// ---------------------------------------------------------------------------------------------
// Status, log, alerts
// ---------------------------------------------------------------------------------------------
void MuxManager::appendLog(const QString& line) { m_log.append(QDateTime::currentDateTime().toString("HH:mm:ss ") + line); while (m_log.size() > 2000) m_log.removeFirst(); }
QJsonArray MuxManager::log(int lines) const { QJsonArray a; for (int i = qMax(0, m_log.size() - lines); i < m_log.size(); ++i) a.append(m_log[i]); return a; }

QJsonObject MuxManager::status() const {
    QJsonObject o; o["state"] = m_state; o["error"] = m_lastError; o["running"] = running(); o["restarts"] = m_restarts; o["startedMs"] = double(m_startedMs); o["metrics"] = m_metrics; o["allocation"] = m_alloc.isEmpty() ? allocation() : m_alloc;
    o["services"] = m_services.size(); o["transportBps"] = double(transportBps()); o["output"] = m_cfg.value("output"); o["tsp"] = tsduckTool("tsp.exe"); o["tspFound"] = QFileInfo::exists(tsduckTool("tsp.exe")); o["ffmpeg"] = ffmpeg(); o["autoStart"] = m_cfg.value("autoStart").toBool(false); o["autoAttempt"] = m_autoAttempt;
    QJsonArray enc; for (Encoder* e : m_encoders) { QJsonObject x; x["service"] = e->serviceId; x["running"] = e->proc && e->proc->state() == QProcess::Running; x["restarts"] = e->restarts; x["lastLine"] = e->lastLine; enc.append(x); } o["encoders"] = enc;
    qint64 sum = 0; const QJsonObject al = o.value("allocation").toObject(); for (auto it = al.begin(); it != al.end(); ++it) sum += qint64(it.value().toDouble()); o["allocatedVideoBps"] = double(sum);
    QJsonArray managed; for (const QJsonValue& v : m_services) if (v.toObject().value("source").toObject().value("kind").toString() == "ndi" || inputViaEngine(v.toObject())) managed.append(feedChannelFor(v.toObject())); o["managedChannels"] = managed;
    o["eitTables"] = eitTables(); o["tablesGeneratedMs"] = m_lastTables.value("generatedMs");
    const QJsonObject sc = scte35Config(); QJsonObject s35; s35["enabled"] = sc.value("enabled"); s35["autoBreaks"] = sc.value("autoBreaks");
    s35["restartRequired"] = running() && !m_runningArgs.isEmpty() && m_runningArgs != tspArgs();
    o["scte35"] = s35;
    return o;
}

// ---------------------------------------------------------------------------------------------
// SCTE-35 (scte35.h): PMT component + spliceinject per service, cues over local UDP
// ---------------------------------------------------------------------------------------------
QJsonObject MuxManager::scte35Config() const { return scte35::normalize(m_cfg.value("scte35").toObject()); }

QJsonObject MuxManager::setScte35(const QJsonObject& body) {
    QJsonObject c = m_cfg.value("scte35").toObject();
    for (auto it = body.begin(); it != body.end(); ++it) if (it.key() != "services") c[it.key()] = it.value();
    m_cfg["scte35"] = scte35::normalize(c);
    const QJsonObject per = body.value("services").toObject();
    if (!per.isEmpty()) {
        for (int i = 0; i < m_services.size(); ++i) {
            QJsonObject svc = m_services[i].toObject(); const QString id = svc.value("id").toString();
            if (!per.contains(id)) continue;
            QJsonObject x = svc.value("scte35").toObject(); const QJsonObject n = per.value(id).toObject();
            if (n.contains("enabled")) x["enabled"] = n.value("enabled").toBool(true);
            if (n.contains("pid")) { if (n.value("pid").isNull() || n.value("pid").toVariant().toString().trimmed().isEmpty()) x.remove("pid"); else x["pid"] = n.value("pid"); }
            svc["scte35"] = x; m_services[i] = svc;
        }
    }
    persist();
    appendLog(QString("scte35 settings saved (%1)").arg(m_cfg.value("scte35").toObject().value("enabled").toBool() ? "on" : "off"));
    emit statusChanged();
    return scte35Status();
}

QJsonObject MuxManager::scte35Status() const {
    const QJsonObject c = scte35Config();
    QJsonObject o; o["config"] = c; o["running"] = running();
    o["restartRequired"] = running() && !m_runningArgs.isEmpty() && m_runningArgs != tspArgs();
    // what the running master was started with: the injectors exist only for those services
    QJsonArray svcs;
    for (int i = 0; i < m_services.size(); ++i) {
        const QJsonObject s = m_services[i].toObject(); const QString id = s.value("id").toString();
        const int pid = scte35::pidFor(s, i + 1), port = scte35::portFor(c, i + 1);
        QJsonObject x; x["id"] = id; x["name"] = s.value("shortName").toString(id); x["serviceId"] = s.value("serviceId");
        x["enabled"] = s.value("scte35").toObject().value("enabled").toBool(true); x["active"] = scte35::serviceEnabled(c, s);
        x["pid"] = pid; x["pidHex"] = QString("0x%1").arg(pid, 4, 16, QChar('0')); x["port"] = port; x["channelId"] = feedChannelFor(s);
        x["live"] = running() && m_runningArgs.contains(QString("127.0.0.1:%1").arg(port));
        const BreakState b = m_breaks.value(id);
        x["inBreak"] = b.inBreak; x["eventId"] = double(b.eventId); x["lastAction"] = b.lastAction; x["lastCueMs"] = double(b.lastCueMs);
        if (b.inBreak) { x["breakSinceMs"] = double(b.sinceMs); x["breakSec"] = b.durationSec; x["runlistId"] = b.runlistId; }
        svcs.append(x);
    }
    o["services"] = svcs; o["log"] = m_cueLog;
    return o;
}

QJsonObject MuxManager::sendCue(const QJsonObject& svc, int index, const QString& action, double durationSec, bool autoReturn, const QString& why) {
    const QJsonObject c = scte35Config();
    const QString id = svc.value("id").toString();
    BreakState& b = m_breaks[id];
    QString xml; quint32 ev = b.eventId;
    if (action == "out") {
        ev = ++m_nextEventId; scte35::Splice sp; sp.eventId = ev; sp.outOfNetwork = true; sp.durationSec = durationSec; sp.autoReturn = autoReturn; sp.uniqueProgramId = svc.value("serviceId").toInt();
        xml = scte35::spliceInsertXml(sp);
        b.inBreak = true; b.eventId = ev; b.sinceMs = QDateTime::currentMSecsSinceEpoch(); b.durationSec = durationSec;
    } else if (action == "in") {
        if (ev == 0) ev = ++m_nextEventId;
        scte35::Splice sp; sp.eventId = ev; sp.outOfNetwork = false; sp.uniqueProgramId = svc.value("serviceId").toInt();
        xml = scte35::spliceInsertXml(sp);
        b.inBreak = false; b.durationSec = 0; b.runlistId.clear();
    } else if (action == "cancel") {
        if (ev == 0) { QJsonObject e; e["error"] = "no splice event to cancel on " + id; e["status"] = 409; return e; }
        xml = scte35::spliceCancelXml(ev);
        b.inBreak = false; b.durationSec = 0; b.runlistId.clear();
    } else xml = scte35::spliceNullXml();
    b.lastAction = action; b.lastCueMs = QDateTime::currentMSecsSinceEpoch();

    const int port = scte35::portFor(c, index);
    const QByteArray dgram = xml.toUtf8();
    auto send = [this, dgram, port]() {
        if (!m_udp) m_udp = new QUdpSocket(this);
        m_udp->writeDatagram(dgram, QHostAddress::LocalHost, quint16(port));
    };
    const int delay = c.value("offsetMs").toInt(0);
    if (delay > 0) QTimer::singleShot(delay, this, send); else send();

    QJsonObject rec; rec["atMs"] = double(QDateTime::currentMSecsSinceEpoch()); rec["service"] = id; rec["action"] = action; rec["eventId"] = double(ev);
    rec["durationSec"] = durationSec; rec["port"] = port; rec["why"] = why; rec["delayMs"] = delay;
    rec["delivered"] = running() && m_runningArgs.contains(QString("127.0.0.1:%1").arg(port));   // no injector listens otherwise
    if (action != "null") {
        m_cueLog.prepend(rec); while (m_cueLog.size() > 100) m_cueLog.removeLast();
        appendLog(QString("scte35 %1 %2 event %3%4 (%5)%6").arg(id, action).arg(ev).arg(durationSec > 0 ? QString(" %1 s").arg(durationSec, 0, 'f', 1) : QString()).arg(why)
                  .arg(rec.value("delivered").toBool() ? QString() : QString(" - not delivered: the running mux has no SCTE-35 injector for this service")));
        emit statusChanged();
    }
    return rec;
}

QJsonObject MuxManager::cue(const QJsonObject& body) {
    const QString action = body.value("action").toString("out").toLower();
    if (action != "out" && action != "in" && action != "cancel" && action != "null") { QJsonObject e; e["error"] = "action must be out, in, cancel or null"; e["status"] = 400; return e; }
    const QString want = body.value("service").toString("*");
    const QJsonObject c = scte35Config();
    if (!c.value("enabled").toBool(false)) { QJsonObject e; e["error"] = "SCTE-35 is off (mux.config.scte35.enabled)"; e["status"] = 409; return e; }
    QJsonArray sent;
    for (int i = 0; i < m_services.size(); ++i) {
        const QJsonObject svc = m_services[i].toObject();
        if (want != "*" && !want.isEmpty() && svc.value("id").toString() != want) continue;
        if (!scte35::serviceEnabled(c, svc)) continue;
        const QJsonObject r = sendCue(svc, i + 1, action, qBound(0.0, body.value("durationSec").toDouble(0.0), 86400.0), body.value("autoReturn").toBool(c.value("autoReturn").toBool(true)), "operator");
        if (!r.contains("error")) sent.append(r);
    }
    if (sent.isEmpty()) { QJsonObject e; e["error"] = want == "*" ? "no service has SCTE-35 on" : "service '" + want + "' not found or its SCTE-35 is off"; e["status"] = 404; return e; }
    QJsonObject o; o["sent"] = sent; o["status"] = scte35Status(); return o;
}

void MuxManager::onChannelItem(const QString& channelId, const QString& runlistId, const QString& type, double breakSec) {
    if (runlistId.isEmpty() || m_lastItem.value(channelId) == runlistId) return;
    m_lastItem[channelId] = runlistId;
    const QJsonObject c = scte35Config();
    if (!c.value("enabled").toBool(false) || !c.value("autoBreaks").toBool(true) || !running()) return;
    const bool isBreak = scte35::isBreakType(c, type);
    for (int i = 0; i < m_services.size(); ++i) {
        const QJsonObject svc = m_services[i].toObject();
        if (feedChannelFor(svc) != channelId || !scte35::serviceEnabled(c, svc)) continue;
        BreakState& b = m_breaks[svc.value("id").toString()];
        if (isBreak && !b.inBreak) {
            if (breakSec > 0 && breakSec < c.value("minBreakSec").toDouble(5)) { appendLog(QString("scte35 %1: break of %2 s under minBreakSec, no cue").arg(svc.value("id").toString()).arg(breakSec, 0, 'f', 1)); continue; }
            sendCue(svc, i + 1, "out", breakSec, c.value("autoReturn").toBool(true), "break: " + type);
            m_breaks[svc.value("id").toString()].runlistId = runlistId;
        } else if (!isBreak && b.inBreak) {
            if (c.value("cueIn").toBool(true)) sendCue(svc, i + 1, "in", 0, false, "break over: " + (type.isEmpty() ? QString("next item") : type));
            else { b.inBreak = false; b.runlistId.clear(); }
        }
    }
}

void MuxManager::raise(const QString& level, const QString& message, const QString& key) {
    appendLog("[" + level + "] " + message);
    if (!key.isEmpty()) { const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now - m_smsSentMs.value(key + "|alert") < 30000) return; m_smsSentMs[key + "|alert"] = now; }
    emit alert(level, message);
    if (level == "critical") sendSms(message);
}

void MuxManager::sendSms(const QString& message) {
    const QJsonObject tw = m_store ? m_store->settingJson("alerts.twilio") : QJsonObject();
    if (!tw.value("enabled").toBool(false) || tw.value("accountSid").toString().isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now - m_smsSentMs.value("sms") < 60000) return; m_smsSentMs["sms"] = now;
    for (const QJsonValue& v : tw.value("to").toArray()) {
        QNetworkRequest req(QUrl(QString("https://api.twilio.com/2010-04-01/Accounts/%1/Messages.json").arg(tw.value("accountSid").toString())));
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
        req.setRawHeader("Authorization", "Basic " + (tw.value("accountSid").toString() + ":" + tw.value("authToken").toString()).toUtf8().toBase64());
        QUrlQuery body; body.addQueryItem("From", tw.value("from").toString()); body.addQueryItem("To", v.toString()); body.addQueryItem("Body", "[PLAY PLAY] " + message.left(600));
        QNetworkReply* r = m_nam.post(req, body.toString(QUrl::FullyEncoded).toUtf8());
        connect(r, &QNetworkReply::finished, this, [this, r]() { r->deleteLater(); const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); appendLog(QString("twilio: HTTP %1").arg(http)); });
    }
}
