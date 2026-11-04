#ifndef PLAYPLAY_COMPOSITOR_H
#define PLAYPLAY_COMPOSITOR_H

#include <QObject>
#include <QMutex>
#include <atomic>
#include <QRectF>
#include <QRect>
#include <QColor>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <map>
#include <memory>
#include <vector>
#include <deque>
#include <limits>
#include <QStringList>
#include <functional>
#include "source.h"
#include "loudness.h"
#include "../mediaframe.h"
#include "../scenerecorder.h"

class GpuCompositor;
namespace metersnap { struct Mix; struct Snapshot; }

/**
 * Compositor — the per-channel picture and sound mixer.
 *
 * Layer stack (z ascending). Role defaults (PlayPlayPy's layer contract):
 *   10 program (A/B program bus with transitions), 15 secondary (PiP, live,
 *   squeezeback), 20 bug, 30 lower_third, 40 ticker, 80 legal_id,
 *   90 emergency (slate). Any id may be used; roles are labels.
 *
 * Each layer: a LayerSource, placement (rect in raster pixels or anchor +
 * natural size), opacity, z, audio gain/mute/duck, and in/out transitions
 * (cut, fade, slide_left/right/up/down, wipe_*). The program bus holds two
 * sources (A on air, B incoming) and runs cut/mix/dip/wipe/push/squeeze
 * between them, frame-accurately from the frame number the take was issued.
 *
 * Mixes: "main" plus any number of extra output mixes (setMix). Every mix shows the same program bus;
 * each graphics layer lists the mixes it appears in (LayerSpec::mixes, empty = all), so a web stream can
 * carry a different bug or crawl than the ATSC feed. Outputs pick their mix (engine output field "mix").
 * Mixes whose layer sets are identical share one rendered picture, so an extra mix costs nothing until
 * it differs.
 *
 * Audio: every source (program, each audio-carrying layer, line-ins, slate) is pulled ONCE per frame and
 * metered pre-fader (per-source VU). Each mix then sums the sources with its own strip gains / mutes /
 * solo and master, its own ducker (program is attenuated while a `duck` layer in that mix is audible),
 * soft clip and EBU R128 meter. Extra mixes follow main's faders for strips they have not set.
 *
 * Rendering is CPU (QPainter, premultiplied) by default; the D3D11 path renders the same draw list on the
 * GPU. Source pictures are straight-alpha BGRA, except from sources that say deliversPremultiplied() (PE-08: templates
 * and HTML with PLAYPLAY_PREMUL=1), which both paths blend without premultiplying them again. Outputs stay straight
 * BGRA: a premultiplied program picture is composited onto black, never passed through, so no sink ever sees
 * premultiplied colours. With setPipelined(true) (the engine turns it on with the GPU) every mix's output is one frame late:
 * this tick's GPU work is read back on the next tick, so the clock thread never waits on the GPU; audio
 * is delayed by the same frame so lip sync is unchanged.
 *
 * Readers (PE-05): metersJson(), status() and layersJson() never hold the render mutex while they build JSON or ask a
 * source for its status(). render() publishes the tick's measured sound as a plain snapshot (metersnapshot.h, a
 * seqlock: readers copy it without any lock); names, fader settings and layer specs are copied under a short lock
 * (implicitly shared values only); source status() calls run outside it under m_statusMutex, which render() never
 * waits for. Meters are therefore at most one tick old. A source retired while a reader may be inside its status() is
 * parked until no reader is, and a source update() that would overlap a reader's status() is applied at a later tick.
 * PLAYPLAY_COMP_LEGACY_STATUS=1 (read when a Compositor is created) restores the old readers for A/B runs.
 */
class Compositor : public QObject {
    Q_OBJECT
public:
    struct Transition {
        QString type = "cut";       // cut|mix|dip|wipe_left|wipe_right|wipe_up|wipe_down|push_left|push_right|push_up|push_down|squeeze|fade|slide_left|slide_right|slide_up|slide_down
        double durationSec = 0.0;
        QColor dipColor = Qt::black;
        static Transition fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
        bool isCut() const { return type == "cut" || durationSec <= 0.0; }
    };

    struct Placement {
        QRectF rect;                // raster pixels; empty = natural size at anchor, or full raster if source fills
        QString anchor = "top_left"; // top_left|top|top_right|left|center|right|bottom_left|bottom|bottom_right
        double marginX = 0.0, marginY = 0.0;
        static Placement fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
    };

    struct LayerSpec {
        int id = 0;
        QString role;
        QJsonObject source;         // LayerSource::create spec
        Placement placement;
        double opacity = 1.0;
        double audioGain = 1.0;     // linear
        bool audioMute = false;
        bool duck = false;          // while this layer has audio, duck the program
        Transition in, out;
        double holdSec = 0.0;       // auto-hide after this many seconds (0 = manual)
        QStringList mixes;          // output mixes this layer appears in (picture and sound); empty = every mix
        bool inMix(const QString& mix) const { return mixes.isEmpty() || mixes.contains(mix); }
        static LayerSpec fromJson(const QJsonObject& o);
        QJsonObject toJson() const;
    };

    struct DuckSettings { double depthDb = -12.0; double attackSec = 0.05; double releaseSec = 0.5; double thresholdDbfs = -40.0; };

    explicit Compositor(const OutputFormat& fmt, QObject* parent = nullptr);
    ~Compositor() override;

    const OutputFormat& format() const { return m_fmt; }

    // --- program bus -----------------------------------------------------------
    /// Take `next` to air (A/B). The transition starts at `frame`.
    void programTake(std::unique_ptr<LayerSource> next, const Transition& t, qint64 frame);
    /// Remove program video (to slate) with a transition.
    void programClear(const Transition& t, qint64 frame);
    LayerSource* programSource() const { return m_progA.get(); }
    LayerSource* programIncoming() const { return m_progB.get(); }
    bool programTransitioning() const { return m_progB != nullptr; }
    /// Program position (seconds) is supplied by the engine for the on-air item.
    void setProgramAudioEnabled(bool on) { m_programAudio = on; }
    void setProgramHold(bool hold) { m_programHold = hold; }   // repeat last frame, silence

    // --- slate (shown when no program) --------------------------------------------
    void setSlate(std::unique_ptr<LayerSource> slate);
    void setSlateVisible(bool on) { m_slateVisible = on; }

    // --- layers (thread-safe: queued, applied on the next render) -----------------------
    bool addLayer(const LayerSpec& spec, qint64 frame, QString* error = nullptr);  // shows with spec.in (opens on the caller's thread)
    /// Attach an already-opened source (open it off the clock thread, then call this on it).
    void addLayer(const LayerSpec& spec, std::unique_ptr<LayerSource> opened, qint64 frame);
    /// Sources leaving the stack (program A after a take, removed layers) go here instead of
    /// being destroyed on the clock thread; the engine closes them on a reaper thread.
    void setRetireHandler(std::function<void(std::unique_ptr<LayerSource>)> fn) { m_retire = std::move(fn); }
    bool showLayer(int id, const Transition& t, qint64 frame);
    bool hideLayer(int id, const Transition& t, qint64 frame);
    bool updateLayer(int id, const QJsonObject& params);
    bool removeLayer(int id);
    void clearLayers(const QString& role = QString());
    QJsonArray layersJson() const;

    // --- audio ----------------------------------------------------------------------
    void setDucking(const DuckSettings& d) { m_duck = d; }
    void setMasterGain(double linear) { m_masterGain = linear; }
    // Mixer strips: "program" (the on-air item's audio) and every audio layer ("L<id>", gain / mute through updateLayer).
    // Solo silences every other strip; AFV off mutes only the current item's own audio (cleared when the next item starts).
    void setProgramGain(double linear) { m_programGain = linear; }
    void setProgramMute(bool on) { m_programMute = on; }
    void setProgramItemMute(bool on) { m_progItemMute = on; }
    void setSolo(const QString& strip) { QMutexLocker l(&m_mutex); m_solo = strip; }
    const LoudnessMeter& meter() const { return m_mixes.front()->meter; }

    // --- mixes ----------------------------------------------------------------------------
    /// Create or update an extra output mix. params: {name, master:{gainDb}, strips:{"program":{gainDb,mute}, "L61":{...}},
    /// solo:"program"|"L<id>"|"", follow:true (strips not set follow main's faders), reset:true (forget strip settings)}.
    /// "main" accepts the same params (strips/master/solo set the channel-wide faders).
    void setMix(const QString& id, const QJsonObject& params);
    void removeMix(const QString& id);
    QStringList mixIds() const;                 // "main" first
    /// Output pipeline depth in frames (see class comment): 0 = synchronous, N = every mix's picture and sound come out N
    /// ticks late so the GPU readback never waits. Set before the first render.
    void setPipelineDepth(int frames) { QMutexLocker l(&m_mutex); m_depth = qBound(0, frames, 4); }
    void setPipelined(bool on) { setPipelineDepth(on ? 2 : 0); }
    int pipelineDepth() const { return m_depth; }

    // --- render -----------------------------------------------------------------------
    /// Produce the output MediaFrame of "main" for `frame` (`programPosSec` = the on-air item position). When `mixes` is
    /// given it receives every extra mix's frame by id. Frames are one tick late while pipelined.
    MediaFrame render(qint64 frame, double programPosSec, std::map<QString, MediaFrame>* mixes = nullptr);
    void setCaptureHandler(std::function<void(SceneCapture)> handler) { QMutexLocker l(&m_mutex); m_capture = std::move(handler); }

    void setGpuEnabled(bool on);
    bool gpuActive() const;
    /// With the GPU enabled: auto (default) times both paths on the live content and composites with the cheaper one
    /// (re-checked about once a minute of graphics); off = always the GPU.
    void setGpuAuto(bool on) { QMutexLocker l(&m_mutex); m_gpuAuto = on; }
    QJsonObject status() const;
    /// Cheap meter readout for high-rate UI updates (per-channel peak, momentary loudness).
    QJsonObject metersJson() const;
    /// PE-05 off switch in effect for this instance (PLAYPLAY_COMP_LEGACY_STATUS=1 when it was created).
    bool legacyStatus() const { return m_legacyStatus; }
    /// PE-13 off switch in effect for this instance (false with PLAYPLAY_AUDIO_POOL=0 when it was created).
    bool audioPooled() const { return m_poolAudio; }

private:
    enum class LayerState { Hidden, AnimIn, Shown, AnimOut };
    static constexpr int kProgramKey = std::numeric_limits<int>::min();   // the program strip's stripPeak key (any int is a layer id)
    struct Layer {
        LayerSpec spec;
        std::unique_ptr<LayerSource> src;
        LayerState state = LayerState::Hidden;
        qint64 shownFrame = 0;          // first frame shown (source timeline origin)
        qint64 animStart = 0;
        Transition anim;                // active in/out transition
        FramePtr lastVideo;
        bool audible = false;
        QString strip;                  // "L<id>", built once (PE-13: no QString::number per tick on the clock thread)
        std::vector<float> peak;        // per-channel peak of the last frame after the strip gain in main (linear)
        // Visible-content box of the last picture (overlays are mostly transparent full-raster pages): only this box is
        // drawn / uploaded. Kept per picture buffer; `bboxHeld` keeps that buffer from being recycled while cached.
        const uint8_t* bboxKey = nullptr;
        FramePtr bboxHeld;
        QRect bbox;
        quint64 srcGen = 0;             // which source this is (PE-05: a deferred update() only goes to the source it was for)
    };
    /// One output mix. "main" is m_mixes[0]; its faders are the channel-wide ones (m_programGain, layer.spec.audioGain ...).
    struct MixState {
        QString id, name;
        std::map<QString, double> stripGain;    // explicit strip gains (linear), extra mixes
        std::map<QString, bool> stripMute;
        double masterGain = 1.0;
        QString solo;
        bool follow = true;                     // strips without an explicit setting follow main
        double duckGain = 1.0;
        LoudnessMeter meter;
        std::vector<float> chPeak;
        std::map<int, std::vector<float>> stripPeak;        // post-fader peaks per strip, keyed by SrcAudio::key (PE-13)
        qint64 clips = 0;
        quint32 serial = 0;                     // unique per mix instance (the meter snapshot is matched by it, not by id)
        MixState(const QString& i, int rate, int ch) : id(i), name(i), meter(rate, ch) {}
    };
    /// A source's sound for this frame, pulled once and shared by every mix.
    struct SrcAudio {
        QString strip;                          // "program" | "L<id>" (shared copies of cached strings, PE-13)
        int layer = 0;
        int key = kProgramKey;                  // stripPeak key: the layer id, kProgramKey for the program
        FramePtr buf;                           // pre-fader (transition / animation gain applied)
        bool duck = false, audible = false;
        std::vector<float> prePeak, preRms;     // per channel, linear
    };
    /// A rendered (or being rendered) picture, shared by the mixes with the same layer set.
    struct Result { FramePtr video; int ticket = -1; };

    OutputFormat m_fmt;
    std::function<void(SceneCapture)> m_capture;
    FramePool m_pool;
    mutable QMutex m_mutex;
    std::map<int, Layer> m_layers;       // ordered by id = z

    std::unique_ptr<LayerSource> m_progA, m_progB, m_progPending;
    Transition m_progTrans; qint64 m_progTransStart = 0; bool m_progClearing = false;
    FramePtr m_progLastA, m_progLastB;
    double m_progPosA = 0.0, m_lastProgramPos = 0.0; qint64 m_progBStartFrame = 0;
    bool m_programAudio = true, m_programHold = false;
    bool m_holdActive = false; double m_holdPos = 0.0;   // program position frozen while the program is held (pause)
    std::unique_ptr<LayerSource> m_slate; bool m_slateVisible = true;
    FramePtr m_black;

    DuckSettings m_duck;
    double m_masterGain = 1.0;
    double m_programGain = 1.0;
    bool m_programMute = false;
    std::atomic<bool> m_progItemMute{false};
    QString m_solo;                      // "" | "program" | "L<layerId>"
    std::vector<float> m_progPeak;       // program strip peaks in main (post-fader, linear)
    /// Mix assignments sent for a layer that has not attached yet (its source opens off the clock thread): applied when it does.
    std::map<int, std::pair<QStringList, qint64>> m_pendingMixes;   // layer -> (mixes, received ms)
    void applyPendingMixes(int id, LayerSpec& spec);
    std::vector<std::unique_ptr<MixState>> m_mixes;     // [0] = main
    std::vector<SrcAudio> m_srcAudio;                   // last frame's sources (pre-fader meters)
    // PE-13: the audio phase allocates nothing per tick in steady state. Sample buffers come from a pool (frames stay
    // ordinary FramePtrs: the last holder, an encoder queue or the pipelined tick, returns them), last tick's SrcAudio
    // entries are recycled with their peak / rms vectors, strip names are cached strings. PLAYPLAY_AUDIO_POOL=0 (read when
    // a Compositor is created) allocates every frame and entry as before.
    static bool audioPoolFromEnv() { return !(qEnvironmentVariableIsSet("PLAYPLAY_AUDIO_POOL") && qEnvironmentVariableIntValue("PLAYPLAY_AUDIO_POOL") == 0); }
    const bool m_poolAudio = audioPoolFromEnv();
    struct AudioPool;                                   // compositor.cpp
    std::unique_ptr<AudioPool> m_audioPool;
    std::vector<SrcAudio> m_srcSpare;                   // recycled SrcAudio entries (buffers released)
    std::vector<const float*> m_planes;                 // mixAudio's plane list for the loudness meter
    FramePtr audioFrame(int nbSamples);                 // planar float, m_fmt channels / rate; NOT zeroed
    int m_depth = 0;
    const bool m_cropOverlays = !qEnvironmentVariableIsSet("PLAYPLAY_NO_OVERLAY_CROP");   // benchmark switch
    /// A tick in flight (pipelined): its pictures (GPU tickets until read back) and every mix's sound.
    struct PendingTick { std::vector<Result> results; std::map<QString, std::pair<int, FramePtr>> mixes; qint64 frame = 0; };
    std::deque<PendingTick> m_pending;

    std::unique_ptr<GpuCompositor> m_gpu;
    bool m_gpuWanted = false;
    // auto path choice (see setGpuAuto)
    bool m_gpuAuto = true;
    bool m_useGpu = true;                // the path compositing now
    QString m_autoPhase = "trial_gpu";   // trial_gpu | trial_cpu | run | recheck
    int m_autoCount = 0;
    double m_gpuCostMs = -1, m_cpuCostMs = -1, m_trialAcc = 0; int m_trialN = 0;
    double m_tickFetchMs = 0;            // GPU readback time spent this tick
    void autoAccount(bool gpu, double ms);

    /// `premultiplied` (PE-08): the frame's source said deliversPremultiplied(), so it is blended as is (CPU: drawn as
    /// Format_ARGB32_Premultiplied; GPU: DrawOp::premultiplied, the shader skips its rgb *= a).
    struct DrawItem { FramePtr frame; QRectF dst; QRectF srcRect; double opacity; QRectF clip; int layer = 0; QRect upload; bool premultiplied = false; };
    /// Bounding box of the pixels with alpha > 0 (empty = fully transparent).
    static QRect alphaBox(const FramePtr& f);
    void buildProgramDraws(qint64 frame, double programPosSec, std::vector<DrawItem>& out, FramePtr* passthrough);
    void buildLayerDraws(qint64 frame, double programPosSec, std::vector<DrawItem>& out);
    FramePtr renderCpu(const std::vector<DrawItem>& draws, const FramePtr& passthrough);
    void pullAudio(qint64 frame, double programPosSec);
    FramePtr mixAudio(MixState& mix, qint64 frame, metersnap::Mix* snap = nullptr);
    double stripGain(const MixState& mix, const QString& strip, bool* mute, bool* explicitSet = nullptr) const;
    MixState* mixById(const QString& id) const;
    FramePtr resolve(Result& r);
    FramePtr silentAudio(qint64 frame);
    static double progress(qint64 frame, qint64 start, const Transition& t, const FrameRate& rate);
    static double ease(double t);
    QRectF placeRect(const Placement& p, const QSize& natural) const;
    FramePtr blackFrame();
    /// Layers synced to the program item's position (karaoke captions) belong to the item that was on air:
    /// they are cut when the item changes or the program is cleared (caller holds m_mutex).
    void hideProgramTimeLayers();
    void retire(std::unique_ptr<LayerSource> s);
    std::function<void(std::unique_ptr<LayerSource>)> m_retire;

    // --- PE-05: readers that never hold m_mutex while building JSON or calling src->status() ----------------------
    static bool legacyStatusFromEnv() { return qEnvironmentVariableIntValue("PLAYPLAY_COMP_LEGACY_STATUS") == 1; }
    const bool m_legacyStatus = legacyStatusFromEnv();
    /// Held by a reader for as long as it uses source pointers it copied under m_mutex (its status() calls). Lock order:
    /// m_statusMutex, then m_mutex. Code holding m_mutex (render, mutators) only ever tryLock()s it, so it never waits.
    mutable QMutex m_statusMutex;
    std::vector<std::unique_ptr<LayerSource>> m_graveyard;   // retired while a reader might be inside its status()
    struct DeferredUpdate { int layer; quint64 gen; QJsonObject params; };
    std::vector<DeferredUpdate> m_deferredUpdates;           // src->update() calls waiting for readers to leave (in order)
    quint64 m_srcGen = 0;
    quint32 m_mixSerial = 0;
    struct MeterSnap;                                        // seqlock + the render thread's staging copy (compositor.cpp)
    std::unique_ptr<MeterSnap> m_snap;
    struct StatusCtl;                                        // what a reader copies under m_mutex (compositor.cpp)
    enum CtlParts : unsigned { CtlLayers = 1, CtlMixes = 2, CtlProgram = 4, CtlGpu = 8 };
    void copyStatusCtl(StatusCtl& c, unsigned parts) const;  // caller holds m_mutex
    void flushGraveyard();                                   // caller holds m_mutex
    void applyDeferredUpdates();                             // caller holds m_mutex
    void publishSnapshot(qint64 frame);                      // render, caller holds m_mutex
    void loadSnapshot(metersnap::Snapshot& out) const;
    QJsonObject metersJsonLegacy() const;
    QJsonObject statusLegacy() const;
    QJsonArray layersJsonLegacy() const;
};

#endif // PLAYPLAY_COMPOSITOR_H
