#ifndef ENGINE_H
#define ENGINE_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QImage>
#include <atomic>
#include <deque>
#include <memory>
#include <map>
#include <thread>
#include "mediaframe.h"
#include "tickprofiler.h"
#include "compositor/metersnapshot.h"
#include "../core/framerate.h"

class DecoderPipeline;
class Compositor;
class SceneRecorder;
class LayerSource;
class FrameClock;
class EncoderStage;
class PreviewTap;
class InputMonitor;
class PacketSink;
class BasebandSink;

/**
 * PlayoutEngine — frame-accurate per-channel playout.
 *
 * Driven by FrameClock::frameTick on the clock thread. Every tick produces one
 * MediaFrame (picture + exactly the audio samples for that frame) and fans it
 * out to encoder stages (TS/FLV sinks) and baseband sinks (NDI/SDI/preview).
 *
 * Threading model:
 *   - Commands from IPC/UI threads are queued and applied at the start of the
 *     next tick, so state only changes on the clock thread.
 *   - Opening media blocks (probe, seek, first frame), so the *next* item is
 *     prepared on a loader thread while the current one plays. The cut happens
 *     on the tick where the current item ends; if the next item is ready the
 *     transition is gapless, otherwise the engine holds slate and counts it.
 *
 * States: IDLE (slate), PREROLL (waiting for an item to be ready), ON_AIR,
 *         PAUSED (hold frame, silence), HOLD (operator slate), ERROR.
 *
 * Playlist item JSON:
 *   { "id", "assetId", "path", "title", "duration"?, "inPoint"?, "outPoint"?,
 *     "loop"?, "hardStartUtcMs"? }
 */
class PlayoutEngine : public QObject {
    Q_OBJECT

public:
    struct Config {
        QString channelId;
        QString channelName;
        FrameRate frameRate = FrameRate::ntsc();
        int width = 1920;
        int height = 1080;
        int audioChannels = 2;
        int sampleRate = 48000;
        bool hwDecode = true;
        bool hwDecodeAuto = false;        // "hwDecode": "auto": hw or sw per codec + resolution by measured cost (PE-12)
        double prerollSec = 3.0;          // start preparing next item this early
        QStringList slateCaptions;
        bool gpu = false;                 // Direct3D 11 compositing (CPU fallback)
        QString gpuMode = "auto";         // auto: use whichever path is cheaper on the live content; on: always the GPU
        QString clockSource = "auto";    // auto|internal|output: lock the frame clock to the SDI output cadence
        double stallSkipSec = 3.0;        // decoder stalled this long while ON_AIR -> skip to next
        QJsonObject identity;             // {bugAsset, bugText, callLetters, brandName, tickerText, legalIdAsset, emergencySlate}
        QJsonObject ducking;              // {depthDb, attackSec, releaseSec, thresholdDbfs}
        QJsonArray outputs;               // see configureOutputs(); an output's "mix" picks its output mix (default "main")
        QJsonArray mixes;                 // extra output mixes: [{id, name, strips, master, solo, follow}] (Compositor::setMix)

        static Config fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
    };

    struct Item {
        QString id;
        QString assetId;
        QString path;
        QString title;
        double durationSec = 0.0;         // 0 = from media (minus in/out)
        double inPointSec = 0.0;
        double outPointSec = 0.0;
        bool loop = false;
        qint64 hardStartUtcMs = 0;        // 0 = auto-follow
        QJsonObject transition;           // {type: cut|mix|dip|wipe_*|push_*|squeeze, durationSec} into this item
        QJsonArray cgOnStart, cgOnEnd;    // CG commands fired at START / END of this item
        QJsonObject source;               // live/stream program source spec (LayerSource::create); empty = file at `path`
        bool audioMute = false;           // audio follows video OFF for this item: its own audio is muted (an operator take of a live input)
        static Item fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
    };

    struct State {
        QString stateName;
        QString currentItemId;
        QString currentAssetId;
        QString currentAssetPath;
        QString currentTitle;
        QString nextItemId;
        QString nextTitle;
        bool nextReady = false;
        double positionSeconds = 0.0;
        double durationSeconds = 0.0;
        double remainingSeconds = 0.0;
        int playlistIndex = -1;
        int playlistSize = 0;
        qint64 currentFrame = 0;
        QString timecode;                 // of-day, drop-frame aware
        bool isHealthy = true;
        QString errorMessage;
        // Telemetry
        double tickMsLast = 0.0;
        double tickMsMax = 0.0;
        qint64 lateTicks = 0;
        qint64 decoderQueue = 0;
        qint64 audioUnderruns = 0;
        qint64 framesDroppedByOutputs = 0;
        qint64 missedCuts = 0;            // item ended but next was not ready
        QJsonArray outputs;               // per-sink stats
    };

    explicit PlayoutEngine(const Config& cfg, QObject* parent = nullptr);
    ~PlayoutEngine() override;

    bool initialize();
    void shutdown();

    /// FrameClock::frameTick slot (clock thread).
    void onFrameTick(qint64 frameNumber, qint64 timestampNs);

    // --- Commands (any thread; applied on the next tick) --------------------
    void handleTransportCommand(const QString& action, const QJsonObject& params);
    void loadPlaylist(const QJsonArray& playlist, bool replaceCurrent = false);
    void appendPlaylist(const QJsonArray& items);
    void applyConfig(const QJsonObject& config);         // frameRate/slate etc. (subset at runtime)
    /// Replace the output definitions (main thread). Incremental: outputs whose definition is unchanged
    /// keep running untouched; changed/removed ones are stopped, new ones started (unless autoStart:false).
    void configureOutputs(const QJsonArray& outputs);
    /// Start / stop / restart one defined output by name without touching the others (main thread).
    /// Output entry fields: type, name, url, profile, options, enabled (false = defined but off),
    /// autoStart (default true: start with the engine), retry {enabled, maxAttempts (0 = unlimited), backoffSec}.
    bool startOutput(const QString& name, QString* error = nullptr);
    bool stopOutput(const QString& name);
    bool restartOutput(const QString& name, QString* error = nullptr);
    /// Per-output status: [{name, type, url, state running|starting|stopped|error|disabled, lastError, enabled,
    /// autoStart, restarts, uptimeSec, bitrateKbps, bytesOut, packetsOut, ...}] (any thread).
    QJsonArray outputsStatus() const;

    /// CG / layer command (any thread): {action: show|hide|update|clear|remove, layer|role, source|template,
    /// placement, opacity, in, out, holdSec, transition, audioGain, duck}
    void handleCgCommand(const QJsonObject& cmd);

    State currentState() const;
    QJsonObject stateJson() const;
    QJsonArray playlistJson() const;
    const Config& config() const { return m_cfg; }
    Compositor* compositor() const { return m_comp.get(); }
    /// The clock driving onFrameTick; lets the engine trim its rate to an output device (genlock slave).
    void setFrameClock(FrameClock* clock) { m_clock = clock; }
    /// Audio meters for the preview/VU channel (any thread).
    QJsonObject meters() const;
    /// Live inputs found by discovery (NDI finder etc.), shown in channel state as `liveInputs`.
    Q_INVOKABLE void setDiscoveredInputs(const QJsonArray& inputs);
    PreviewTap* previewTap() const { return m_preview.get(); }
    /// Live pictures of the switcher's inputs (frames: InputMonitor::frameReady).
    InputMonitor* inputMonitor() const { return m_monitor.get(); }

signals:
    void stateChanged(const QString& state, const QString& reason);
    void asRunEvent(const QJsonObject& event);          // type, itemId, assetId, frame, utcMs, timecode, ...
    void framePresented(qint64 frameNumber, qint64 timestampNs);
    /// Confidence picture of an extra output mix (the Supervisor files it as "<channel>:mix-<id>").
    void mixPreviewReady(const QString& mixId, const QByteArray& jpeg, int width, int height);
    void telemetry(const QJsonObject& state);           // ~1 Hz
    void error(const QString& message);

private:
    struct Command { QString action; QJsonObject params; std::unique_ptr<LayerSource> source; };
    struct Prepared {
        int playlistIndex = -1;
        Item item;
        std::unique_ptr<DecoderPipeline> decoder;
        std::unique_ptr<LayerSource> live;         // opened live/stream source (instead of decoder)
        bool failed = false;
        QString error;
    };

    Config m_cfg;
    std::atomic<bool> m_initialized{false};

    // Pipeline
    std::unique_ptr<Compositor> m_comp;
    mutable QMutex m_recordMutex;
    std::shared_ptr<SceneRecorder> m_recorder;
    void recordingCommand(const QJsonObject& params);
    std::unique_ptr<PreviewTap> m_preview;
    std::unique_ptr<InputMonitor> m_monitor;
    std::map<QString, std::unique_ptr<PreviewTap>> m_mixPreviews;   // one confidence picture per extra mix
    QHash<const void*, QString> m_sinkMix;               // baseband sink / encoder stage -> its mix (under m_outputMutex)
    DecoderPipeline* m_decoder = nullptr;                // current item (owned by the compositor's program source)
    mutable QMutex m_outputMutex;                        // guards the four lists below (fanOut reads them on the tick thread)
    QList<EncoderStage*> m_stages;
    QList<PacketSink*> m_packetSinks;
    QList<BasebandSink*> m_basebandSinks;

    // One slot per configured output. Packet outputs share an EncoderStage per identical profile
    // (m_stageByKey); the stage is released when its last slot stops. Slots live on the main thread.
    struct OutputSlot {
        QJsonObject def;                                 // the configured entry
        QString name, type, url;
        QString state = "stopped";                       // running | starting | stopped | error | disabled
        QString lastError;
        bool enabled = true, autoStart = true, manualStop = false;
        QString stageKey;                                // profile JSON of the shared stage (packet outputs)
        PacketSink* psink = nullptr;
        BasebandSink* bsink = nullptr;
        int restarts = 0, attempts = 0;
        qint64 startedMs = 0, nextRetryMs = 0, lastBytes = 0, lastBytesMs = 0, stalledSec = 0, lastDropped = 0, lastOut = 0;
        double bitrateKbps = 0.0;
    };
    QList<OutputSlot*> m_slots;                          // under m_outputMutex
    QHash<QString, EncoderStage*> m_stageByKey;          // under m_outputMutex
    OutputSlot* slotByName(const QString& name) const;   // caller holds m_outputMutex
    bool startSlot(OutputSlot* s);                       // main thread
    void stopSlot(OutputSlot* s, const QString& newState);
    void releaseStage(const QString& key);
    void scheduleRetry(OutputSlot* s, const QString& why);
    QJsonObject slotJson(const OutputSlot* s) const;     // caller holds m_outputMutex
    static bool isBasebandType(const QString& type);

    // Commands
    QMutex m_cmdMutex;
    std::deque<Command> m_commands;

    // Playlist (tick thread owns; snapshot under m_stateMutex for readers)
    QList<Item> m_playlist;
    int m_index = -1;                                    // index of current item
    Item m_current;
    bool m_hasCurrent = false;
    bool m_takeNow = false;                              // an operator take is loading: cut to it as soon as it is ready
    qint64 m_itemStartFrame = 0;
    double m_itemDurationSec = 0.0;
    qint64 m_pauseFrame = -1;
    double m_pausedPosSec = 0.0;

    // Next-item preparation. Loaders run detached; a generation counter lets
    // the engine abandon a load (skip pressed mid-open) without ever joining a
    // thread from the clock thread. Retired decoders are closed by a reaper.
    QMutex m_prepMutex;
    std::unique_ptr<Prepared> m_prepared;                // ready (or failed) result
    std::atomic<bool> m_preparing{false};
    std::atomic<quint64> m_loadGeneration{0};
    std::atomic<int> m_activeLoaders{0};
    int m_preparingIndex = -1;
    void startPrepare(int playlistIndex, const Item& item);
    void discardPrepared();
    void retireDecoder(std::unique_ptr<DecoderPipeline> dec);
    void retireSource(std::unique_ptr<LayerSource> src);
    void waitForBackgroundWork();

    // State (PE-04: the clock thread publishes plain data only; strings and JSON are built by the readers)
    //  - TickPod: every tick's numbers, through a seqlock the clock thread never waits on (metersnapshot.h).
    //  - Ident: the names / ids of what is on air and next, and the state name. They change at cuts only: the clock
    //    thread compares them with what it last published and copies them under m_stateMutex (tryLock; a busy reader
    //    means the copy waits a tick) only when they differ.
    //  - The once-a-second part (output stats, hardening, tickPhases, the telemetry JSON) is built by
    //    publishTelemetry() on the engine's own thread (main), queued from the telemetry tick; PLAYPLAY_TELEMETRY_MAIN=0
    //    builds it on the clock thread as before PE-04.
    //  currentState() / stateJson() compose a State from the pod, the ident and the last published outputs; the
    //  timecode string is made there, from the pod's wall-clock ms.
    struct alignas(8) TickPod {
        qint64 frame = 0;
        qint64 utcMs = 0;                                // wall clock of the frame (frameToUtcMs): readers build the timecode from it
        qint64 lateTicks = 0, missedCuts = 0, decoderQueue = 0, audioUnderruns = 0;
        double positionSec = 0.0, durationSec = 0.0, remainingSec = 0.0;
        double tickMsLast = 0.0, tickMsMax = 0.0;
        qint32 playlistIndex = -1, playlistSize = 0;
        quint8 hasCurrent = 0, nextReady = 0, healthy = 1, pad[5] = {};
    };
    struct Ident {
        QString stateName, currentItemId, currentAssetId, currentAssetPath, currentTitle, nextItemId, nextTitle;
        bool operator==(const Ident& o) const {
            return stateName == o.stateName && currentItemId == o.currentItemId && currentAssetId == o.currentAssetId
                && currentAssetPath == o.currentAssetPath && currentTitle == o.currentTitle && nextItemId == o.nextItemId && nextTitle == o.nextTitle;
        }
    };
    mutable QMutex m_stateMutex;
    QString m_stateName = "IDLE";
    metersnap::SeqLock<TickPod> m_pod;                   // written every tick (clock thread), read anywhere
    Ident m_ident;                                       // under m_stateMutex
    Ident m_identClock;                                  // clock thread only: what it last published
    bool m_identDirty = false;                           // clock thread only: a publish is owed (tryLock failed)
    QJsonArray m_outputsJson;                            // under m_stateMutex: per-output stats of the last telemetry
    qint64 m_outputDrops = 0;                            // under m_stateMutex
    qint64 m_lastFrame = 0;
    qint64 m_lastTickNs = 0;
    qint64 m_lastTelemetryNs = 0;
    double m_tickMsMax = 0.0;
    qint64 m_lateTicks = 0;
    qint64 m_missedCuts = 0;
    qint64 m_originUtcMs = 0;                            // wall-clock at frame 0 (for TC + hard starts)
    const bool m_telemetryOnMain;                        // PE-04 switch (PLAYPLAY_TELEMETRY_MAIN, default on), read once
    // Per-command debug log rate limit (PE-04): PLAYPLAY_CMD_LOG=0 none, all = every command, else 10 lines/s.
    const int m_cmdLogMode;                              // 0 off, 1 rate-limited, 2 all
    qint64 m_cmdLogNs = 0; int m_cmdLogBudget = 10; qint64 m_cmdLogSuppressed = 0;   // clock thread only
    // Hardening, once a second: the decoder stall watchdog runs on the clock thread (it reads the item and pushes a
    // skip); the output-locked clock, output supervision and sink alarms run with the telemetry (publishTelemetry).
    FrameClock* m_clock = nullptr;
    double m_lockIntegral = 0.0, m_lockPpm = 0.0; QString m_lockSink; bool m_lockActive = false;
    qint64 m_lastDecoded = -1; std::atomic<int> m_stallSeconds{0};
    QHash<QString, QJsonObject> m_sinkLast; QHash<QString, qint64> m_sinkAlarmMs;
    void stallWatchdogTick(qint64 frame);                // clock thread, 1 Hz
    void collectOutputStats(State& s);                   // telemetry thread: encoder / sink stats, slot bitrates
    void outputHardening(State& s);                      // telemetry thread: clock lock, dead-sink retry, alarms
    void publishTelemetry(const tickprof::Window& w, qint64 cmdsNotLogged);   // telemetry thread: outputs + tickPhases, emits telemetry()
    QString timecodeAt(qint64 utcMs) const;
    QJsonArray m_liveInputs;
    qint64 m_liveInputsMs = 0;                             // under m_stateMutex
    // Tick phase profiler (PE-06): fed on the clock thread; m_tickPhases is the last closed window, the telemetry's
    // `tickPhases` (under m_stateMutex). PLAYPLAY_TICK_PROFILE=0 turns both off.
    tickprof::TickProfiler m_tickProf;
    tickprof::Window m_tickPhases;

    // Tick phases
    void processCommands(qint64 frame);
    void executeCommand(Command& c, qint64 frame);
    void executeCg(Command& c, qint64 frame);
    void runCgHooks(const QJsonArray& hooks, bool ending = false);
    QJsonObject identityCg(const QString& name, bool ending) const;
    void rebuildSlate();
    void advanceIfNeeded(qint64 frame);
    void takePrepared(qint64 frame, const QString& reason);
    void endCurrent(qint64 frame, const QString& eventType, bool takeFollows = false);
    void goIdle(qint64 frame, const QString& reason);
    void ensureNextPrepared();
    int nextIndexAfter(int idx) const;
    double currentPosSec(qint64 frame) const;
    qint64 frameToUtcMs(qint64 frame) const;
    qint64 utcMsToFrame(qint64 utcMs) const;
    void fanOut(const MediaFrame& mf, const std::map<QString, MediaFrame>& mixes);
    void applyMixes(const QJsonArray& mixes);           // clock thread
    void ensureMixPreview(const QString& id);
    void transitionTo(const QString& newState, const QString& reason, qint64 frame);
    void logAsRun(const QString& type, const Item& item, qint64 frame, const QJsonObject& extra = {});
    void updateSnapshot(qint64 frame, double tickMs);
    void teardownOutputs();
    bool buildOutputs(const QJsonArray& outputs);
};

#endif // ENGINE_H
