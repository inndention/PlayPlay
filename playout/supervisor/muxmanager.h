#ifndef MUXMANAGER_H
#define MUXMANAGER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QDateTime>
#include <functional>

namespace sched { class ScheduleStore; }
class QUdpSocket;
class RestApiServer;

/**
 * MuxManager — the ATSC 1.0 multiplexer, ported from the Starfile "broadcast_gui" controller
 * (the original Python tool: core/mux_backend.py, statmux.py, program_guide.py,
 * encoder_backend.py, channel_presets.py, alert_manager.py).
 *
 * Services (settings `mux.services`): one per sub-channel of the transport (19.392658 Mb/s by default).
 *   { id, major, minor, shortName, longName, serviceId, vidPid, audPid, udpPort,
 *     source: { kind: channel|ndi|input|udp,
 *               channelId                                   (channel: a PlayPlay channel engine)
 *               ndiName, ndiTitle, audio                    (ndi: auto-provisioned hidden channel "mux-<id>")
 *               input: {type: srt|rtmp|rtsp|udp|http|file|test|dshow, location, audio, transport, feed: engine|ffmpeg}
 *                                                           (network types: hidden engine channel; others: ffmpeg we spawn)
 *               url }                                       (udp: an externally produced CBR TS we only merge)
 *     encoder: { encoderPreset, codec: libx264|libx265|h264_qsv|hevc_qsv|h264_nvenc|hevc_nvenc|mpeg2video,
 *                width, height, fps, vidBitrateKbps, maxrateKbps, bufsizeKbps, rateControl: cbr|vbr|cqp, qp,
 *                gop, keyintMin, bframes, closedGop, scenecut, preset, profile, level, interlaced, afd, ccPassthrough, extra,
 *                audio: { codec: ac3|aac|mp2|eac3, bitrateKbps, channels, sampleRate, dialnorm, mixingLevel, aacProfile, normalize },
 *                ts: { pmtPid, pcrPid, pcrPeriodMs, patPeriodSec, provider, muxrate }     // vidPid/audPid/serviceId live on the service
 *                (legacy flat fields audCodec/audBitrateKbps/audChannels are still read) },
 *     statmux: { managed, minKbps, maxKbps, weight, priority } }
 *
 * Feeds: a `channel` service is a PlayPlay channel whose engine is given a UDP TS output
 * (239.255.25.N:port, service id / PIDs / encoder profile from the service); an `ndi` service gets a
 * hidden, mux-managed channel profile `mux-<id>` (extra.hidden, extra.managedBy = "mux",
 * extra.fixedSource = {type:"ndi", name}) that the ScheduleExecutor pins to that NDI source and whose
 * engine carries the same UDP TS output — the Supervisor spawns/removes those engines in
 * applyMuxOutputs() from syncManagedChannels(); an `input` service of a network type (rtsp/srt/rtmp/udp/http)
 * gets the same kind of hidden channel pinned to {type:"stream", url} (DEP-18); other `input` services
 * (test/file/dshow, or source.input.feed = "ffmpeg") are encoded by an FFmpeg process we spawn; a `udp`
 * service is merged as-is.
 *
 * The master is one TSDuck `tsp`: null input regulated to the transport rate, PAT + PSIP injection
 * (MGT/TVCT/STT, optional RRT, EIT-k/ETT-k built from the PlayPlay schedule, validated in-process by
 * PlayPlayTsKit (`mux.config.tables` = native, the default) or by tstabcomp (`tables` = tstabcomp);
 * `mux.tableOverrides` adjusts TVCT attributes, per-service flags, EIT depth and repetition rates),
 * one `-P merge` per service (sub-tsp receiving the multicast, PMT remapped to 0x20+serviceId,
 * PAT/SDT/null stripped), bitrate monitors emitting JSON lines, continuity/pcr checks, and the
 * output: DekTec ASI (the port is set up first by `PlayPlayMux dektec prep` or `tsdektec --output`, run
 * asynchronously; preload/stuffing options), UDP, SRT or a file.
 *
 * Tools (DEP-15, docs/mux.md): nothing here waits on a child process on the Supervisor's event-loop thread.
 * `mux.config.tools` = auto (default: PlayPlayMux.exe when it is found, else TSDuck) | native | tsduck picks
 * the analyzer (`PlayPlayMux analyze` / `tsp -P tables|analyze`) and the DekTec listing (`PlayPlayMux dektec
 * list` / `tsdektec -a -v`); the port setup before tsp uses `PlayPlayMux dektec prep` only with `native` (or
 * when tsdektec.exe is missing). A PlayPlayMux answer "unsupported" (exit 64, e.g. built without DTAPI) falls
 * back to the TSDuck tool.
 *
 * Statmux: bounded weighted water-filling of the video budget (transport − reserve − audio);
 * the result is applied as encoder targets (engine output profiles / ffmpeg -b:v) at start and
 * whenever the service list changes. Alerts (FIFO underrun, bitrate deviation, restarts) go to the
 * Supervisor alert bus and, optionally, to Twilio SMS (settings `alerts.twilio`).
 *
 * Auto-start: `mux.config.autoStart` starts the mux ~3 s after construction with retry/backoff
 * (3, 6, 12, 24, 48 s; a critical alert after the 5th failure).
 */
class MuxManager : public QObject {
    Q_OBJECT
public:
    static constexpr qint64 kAtscBps = 19392658;

    explicit MuxManager(sched::ScheduleStore* store, const QString& dataDir, QObject* parent = nullptr);
    ~MuxManager() override;

    QJsonObject config() const;                  // {enabled, output:{kind,device,port,channel,url,bitrate,preloadMs,maintainPreload,stuffing}, tsid, timezone, statmux:{...}, tsduckDir, ffmpegPath, autoStart, tables, tools, playplayMux}
    void setConfig(const QJsonObject& cfg);
    QJsonArray services() const;
    void setServices(const QJsonArray& services);
    QJsonObject applyPreset(int channelCount, bool force);
    QJsonObject allocation() const;              // service id -> video bps
    QJsonObject status() const;                  // state, metrics, per-service bitrates, restarts, tables
    QJsonArray log(int lines = 200) const;
    qint64 transportBps() const;                 // output.bitrate (default 19392658)

    /// Starts the mux. Allocation, tables (in-process) and the tsp check run here; with a DekTec output the port setup
    /// then runs as an async child and tsp starts when it succeeds (state "starting" until then). Returns false only for
    /// what fails right here; a later failure sets state "error", raises the alert and tells startAsync() waiters.
    bool start(QString* error = nullptr);
    using StartDone = std::function<void(bool ok, const QString& error)>;
    /// start() with a completion: `done` runs once tsp was started or the start failed (at once when already running;
    /// joins a start in progress). The /mux/start and /mux/restart routes reply from it (installAsyncRoutes).
    void startAsync(StartDone done);
    void stop();
    bool running() const { return m_state == "running" || m_state == "starting"; }
    QJsonObject validateTables(QString* error = nullptr);   // generates + compiles without starting
    QJsonObject compileReport();                 // same, with per-file compiler output (engine native | tstabcomp)
    QString tablesEngine() const;                // mux.config.tables: "native" (PlayPlayTsKit in-process, default) | "tstabcomp"
    QString toolsEngine() const;                 // resolved mux.config.tools: "native" (PlayPlayMux) | "tsduck"
    QString playplayMuxExe() const;              // mux.config.playplayMux, else PlayPlayMux.exe next to this exe, else PATH; "" when absent
    /// Replaces GET /api/v1/mux/dektec, POST /api/v1/mux/start and /mux/restart with asynchronous handlers (they used to wait
    /// up to 15 s / 10 s for tsdektec on the event-loop thread). Called from the lane-extension registry (laneext.h).
    static void installAsyncRoutes(RestApiServer* rest, MuxManager* mux);
    static MuxManager* primary();                // the first MuxManager alive (the Supervisor's), for installAsyncRoutes
    QJsonObject guidePreview() const;            // the EIT events we would announce
    QJsonArray engineOutputsFor(const QString& channelId, const QJsonArray& existing) const;  // channel/ndi services → UDP TS output entries

    // --- encoder catalogue, capabilities, hardware -------------------------------------------------
    static QJsonArray encoderPresets();          // service-encoder shaped presets (same ids as EncoderProfile::preset)
    /// ffmpeg encoders/build flags, tsduck version/tools, dektec devices (cached 10 min). Never waits on a child process (DEP-02b):
    /// the ffmpeg part comes from the libav this process links (PlayPlayAvKit, path "(in-process libav)"), tsp's version from its
    /// version resource, and the dektec part is the last `tsdektec -a -v` result while a refresh re-probes in the background
    /// (`dektec.probing`; the next call has the new list). A missing TSDuck gives found:false without starting anything.
    QJsonObject capabilities(bool refresh = false);
    static QJsonObject parseFfmpegEncoders(const QString& text);   // `ffmpeg -encoders` -> {video:[{name,description}], audio:[...], has:{name:true}}
    /// DekTec devices without waiting: the last listing (younger than 10 min unless `refresh`), else the previous list while a
    /// background listing runs (`PlayPlayMux dektec list -v` or `tsdektec -a -v`).
    QJsonArray dektecDevices(bool refresh = false);
    /// The listing object {devices[], dtapi, dta, raw?, engine, tool, tsdektec, exitCode, probedMs} once a fresh one exists:
    /// at once from the cache, else when the background listing ends. The async /mux/dektec route answers from it.
    void dektecDevicesAsync(bool refresh, std::function<void(const QJsonObject&)> done);
    static QJsonObject parseTsdektec(const QString& text);   // `tsdektec -a -v` -> {devices[], dtapi, dta, raw (first 20 lines, only when no device)}
    /// `PlayPlayMux dektec list -v` (stdout text + last-line JSON) -> the same object as parseTsdektec plus the tool's fields
    /// (portConfig, name, ...); {"fallback": true} when that PlayPlayMux has no DTAPI ("available": false) or gave no JSON.
    static QJsonObject parseDektecList(int exitCode, const QString& stdoutText);
    /// What a `PlayPlayMux dektec prep` exit means for the mux start: {ok} | {fallback: true} (exit 64 / unsupported / no JSON:
    /// run tsdektec) | {ok: false, error, inUse?}. Pure (unit-tested).
    static QJsonObject prepVerdict(int exitCode, bool crashed, const QString& stdoutText, const QString& stderrText);
    QJsonObject dryRun();                        // exact tsp + per-encoder ffmpeg command lines, channel outputs; nothing starts
    /// The ffmpeg command line for one `input` service. Pure: reads config, services, the statmux allocation and the capabilities
    /// cache, spawns nothing (public for test_inhouse_psip). `error` gets a warning (codec fallback, mpeg2 scene-cut dropped
    /// for a closed GOP) or why no command can be built.
    QStringList ffmpegArgs(const QJsonObject& svc, QString* error) const;

    // --- PSIP tables ------------------------------------------------------------------------------
    QJsonObject tablesJson();                    // {dir, files:[{name,size,xml}], psip:{...}, overrides}
    QJsonObject tableOverrides() const;          // settings mux.tableOverrides
    void setTableOverrides(const QJsonObject& o);
    /// Quick live analysis with `PlayPlayMux analyze` or tsp (tables on the PSIP pids of the UDP output or the output file,
    /// or the analyzer report of a service multicast). Asynchronous, never waits: `done` receives {command, output, exitCode,
    /// source, mode, engine (native | tsduck), summary (PlayPlayMux's JSON)}.
    void analyze(const QString& serviceId, const QString& mode, std::function<void(const QJsonObject&)> done);

    // --- NDI and network-input services: hidden channels ---------------------------------------------
    /// Create/update/delete the `mux-<id>` channel profiles for ndi services and engine-fed `input` services (DEP-18).
    /// Returns {created:[id], updated:[{id, restart}], removed:[id], channels:[id]} for the Supervisor to act on; also
    /// stops a running ffmpeg encoder of a service that is now engine-fed (one sender per service multicast).
    QJsonObject syncManagedChannels();
    QStringList managedChannelIds() const;
    /// The channel id that feeds a service: channelId for `channel`, "mux-<id>" for `ndi` and engine-fed `input`, empty otherwise.
    static QString feedChannelFor(const QJsonObject& svc);
    /// DEP-18: an `input` service of type rtsp|srt|rtmp|udp|http(s) is pulled by a hidden engine channel (the engine's
    /// EncoderStage applies the whole encoder profile) unless source.input.feed = "ffmpeg"; test / file / dshow stay ffmpeg.
    static bool inputViaEngine(const QJsonObject& svc);
    /// extra.fixedSource of the hidden channel: {type:"ndi", name, ...} or {type:"stream", url, inputOptions, ...}; {} otherwise.
    static QJsonObject fixedSourceFor(const QJsonObject& svc);

    // --- SCTE-35 cue messages (scte35.h, docs/mux.md "SCTE-35") --------------------------------------
    QJsonObject scte35Config() const;                    // normalised mux.config.scte35
    /// PUT /api/v1/mux/scte35 {enabled, autoBreaks, cueIn, autoReturn, breakTypes, minBreakSec, offsetMs, basePort, injectCount,
    /// heartbeatSec, services:{<svcId>:{enabled, pid}}} -> scte35Status(). PID / on-off changes apply at the next mux start.
    QJsonObject setScte35(const QJsonObject& body);
    QJsonObject scte35Status() const;                    // {config, running, restartRequired, services:[...], log:[...]}
    /// POST /api/v1/mux/scte35/cue {service:"svc1"|"*", action:"out"|"in"|"cancel"|"null", durationSec, autoReturn}.
    QJsonObject cue(const QJsonObject& body);
    /// The Supervisor reports every new on-air item: a break type starts a cue out on the services that channel feeds,
    /// the first non-break item after it a cue in (settings autoBreaks / cueIn). breakSec = the break's planned length.
    void onChannelItem(const QString& channelId, const QString& runlistId, const QString& type, double breakSec);

    /// Hook for the Supervisor: resolve program titles per channel/hour for the EPG.
    using TitleResolver = std::function<QString(const QString& channelId, const QDateTime& hourStartLocal, QString* description)>;
    void setTitleResolver(TitleResolver r) { m_titles = std::move(r); }

signals:
    void alert(const QString& level, const QString& message);
    void statusChanged();
    void servicesChanged();                      // the Supervisor re-applies engine outputs / managed channels

private:
    struct Encoder { QString serviceId; QProcess* proc = nullptr; int restarts = 0; qint64 startedMs = 0; QString lastLine; };

    sched::ScheduleStore* m_store;
    QString m_dataDir;
    QJsonObject m_cfg;
    QJsonArray m_services;
    QJsonObject m_alloc;
    QJsonObject m_caps;                          // capabilities cache (+ probedMs)
    QProcess* m_dektecProbe = nullptr;           // background DekTec listing (PlayPlayMux dektec list -v | tsdektec -a -v), one at a time
    QList<std::function<void(const QJsonObject&)>> m_dektecWaiters;   // dektecDevicesAsync() callers waiting for it
    void probeDektecAsync();                     // starts it (or records found:false when no tool exists); fills m_caps["dektec"]
    void probeDektecWith(bool native);
    void setDektecResult(const QJsonObject& dk);  // m_caps["dektec"] = dk, waiters answered
    // One child process run asynchronously (DEP-15): no waitFor* on the event-loop thread; kill after timeoutMs.
    struct ToolResult { int exitCode = -1; bool crashed = false, timedOut = false; QString startError, out, err; };
    QProcess* runTool(const QString& name, const QString& exe, const QStringList& args, int timeoutMs, bool merged, std::function<void(const ToolResult&)> done);
    // start(): phase 2 (DekTec port setup, async) and phase 3 (engine outputs, encoders, tsp)
    quint64 m_startGen = 0;                      // bumped by start()/stop(): a stale prep result is ignored
    QProcess* m_prep = nullptr;                  // the running port setup
    QList<StartDone> m_startWaiters;
    bool nativePrep() const;                     // the port setup with PlayPlayMux (tools native, or auto without tsdektec.exe)
    void prepareDektecAsync(quint64 gen);
    void prepWithTsdektec(quint64 gen);
    bool launchTsp(quint64 gen, QString* error);
    void failStart(const QString& error, bool alert, const QString& alertText = QString());
    void finishStart(bool ok, const QString& error);
    QJsonObject m_lastTables;                    // summary of the last writeTables() run
    QProcess* m_tsp = nullptr;
    QList<Encoder*> m_encoders;
    QString m_state = "stopped";                 // stopped|starting|running|error
    QString m_lastError;
    QTimer m_tableTimer, m_watchdog, m_metricsTimer;
    QNetworkAccessManager m_nam;
    QStringList m_log;
    QJsonObject m_metrics;                       // muxBitrate, stuffing, packets, fifo, continuity, pcr, patPmt, perService{}
    QHash<QString, qint64> m_lastSeenBps;
    qint64 m_lastCcErrorMs = 0, m_lastPcrErrorMs = 0, m_lastFifoUnderrunMs = 0, m_startedMs = 0;
    int m_restarts = 0; bool m_wantRunning = false;
    int m_autoAttempt = 0;
    QHash<QString, qint64> m_smsSentMs;
    TitleResolver m_titles;
    QByteArray m_stderrBuf;
    // SCTE-35
    struct BreakState { bool inBreak = false; quint32 eventId = 0; qint64 sinceMs = 0; double durationSec = 0.0; QString runlistId, lastAction; qint64 lastCueMs = 0; };
    QHash<QString, BreakState> m_breaks;                 // by service id
    QHash<QString, QString> m_lastItem;                  // channel id -> last runlist id reported
    QJsonArray m_cueLog;                                 // newest first, 100 kept
    QStringList m_runningArgs;                           // the tsp command line of the running master
    quint32 m_nextEventId = 0;
    QUdpSocket* m_udp = nullptr;
    QTimer m_heartbeat;
    QJsonObject sendCue(const QJsonObject& svc, int index, const QString& action, double durationSec, bool autoReturn, const QString& why);
    void* m_job = nullptr;                       // Windows job object: child processes die with the Supervisor
    void adopt(QProcess* p);

    QString tablesDir() const;
    QString tsduckTool(const QString& exe) const;
    QString ffmpeg() const;
    void load();
    void persist();
    void appendLog(const QString& line);
    void scheduleAutoStart();
    QJsonObject computeAllocation(QString* error) const;
    int eitTables() const;                       // EIT-0..k-1 from tableOverrides.eitDays (default 8 = 24 h)
    bool writeTables(QString* error);
    bool compileTables(QString* error, QJsonArray* report = nullptr);
    void writeStt();
    QStringList tspArgs() const;
    QString svcMcastUrl(const QJsonObject& svc) const;        // udp://239.255.25.N:port?pkt_size=1316&ttl=1
    QString svcMcastAddr(const QJsonObject& svc) const;       // 239.255.25.N:port
    int svcIndex(const QString& serviceId) const;             // 1-based position in m_services
    bool ffmpegHasEncoder(const QString& name) const;         // from the capabilities cache; true when not probed yet
    void startEncoders();
    void stopEncoders();
    void onTspLine(const QString& line);
    void onTspFinished(int code, QProcess::ExitStatus st);
    void raise(const QString& level, const QString& message, const QString& key = QString());
    void sendSms(const QString& message);
    static QString svcMcast(int index);
    static QString quoteArgs(const QString& exe, const QStringList& args);
};

#endif // MUXMANAGER_H
