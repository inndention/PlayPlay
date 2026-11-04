#ifndef PLAYPLAY_LIBOBSSOURCE_H
#define PLAYPLAY_LIBOBSSOURCE_H

#include "livesources.h"
#include <QStringList>

/**
 * LibObsSource — libobs (OBS Studio's core) running headless inside the channel engine as ONE live input.
 *
 * The hybrid spike (docs/libobs-spike.md): libobs keeps its own free-running render loop, its scene graph
 * (cameras, images, colour, media, filters) and its transitions (cut / fade / stinger); the picture it produces
 * is captured through obs_add_raw_video_callback (BGRA at the channel raster) and handed to the compositor
 * exactly like an NDI input: newest frame wins, the clock thread reads it without blocking (LiveSourceBase).
 * Scheduled playout keeps PlayPlay's frame-exact timing; libobs is just another layer source.
 *
 * Built only with -DPLAYPLAY_WITH_LIBOBS=ON (cmake/lane-libobs.cmake); the factory does not know the type otherwise.
 *
 * spec: {
 *   type: "libobs",
 *   root: "<libobs install prefix>"           optional; default PLAYPLAY_LIBOBS_ROOT or env PLAYPLAY_LIBOBS_ROOT
 *   audio: true,                               mix libobs' program audio (channel 0) into the layer
 *   scenes: [ { name: "A",
 *               items: [ { id: "color_source_v3", name: "bg", settings: { color: "#ff0000" },
 *                          fit: "stretch" | "inner" | "outer" | "none",   (default stretch to the raster)
 *                          pos: {x, y}, visible: true,
 *                          filters: [ { id: "color_filter_v2", settings: {...} } ] } ] } ],
 *   program: "A",                              scene on air at open (default: first scene)
 *   transition: { id: "cut_transition" | "fade_transition" | "obs_stinger_transition" | ..., durationMs, settings }
 * }
 * Item ids are libobs source ids (image_source, color_source_v3, ffmpeg_source, text_ft2_source_v2, random, ...);
 * settings are the module's own keys passed through as JSON (a "color" given as "#rrggbb" is converted to the
 * 0xAABBGGRR integer libobs expects; a colour source without width/height gets the raster size).
 *
 * update(): { switch: "B", transition?: "fade_transition" | {id, durationMs, settings}, durationMs? }
 *               starts a transition to scene B (obs_transition_start). Work happens on the source's own thread.
 *           { item: "bg", settings: {...} }  obs_source_update on one scene item's source (text, colour, file...)
 *           { item: "bg", visible: false }   show / hide a scene item in every scene that holds it
 *
 * One LibObsSource per process: libobs has a single program output (channel 0). A second open() fails.
 */
class LibObsSource : public LiveSourceBase {
    Q_OBJECT
public:
    explicit LibObsSource(const QJsonObject& spec, QObject* parent = nullptr);
    ~LibObsSource() override;
    QString type() const override { return "libobs"; }
    bool open(const OutputFormat& fmt) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    FramePtr video(qint64 frame, double posSec) override;
    void update(const QJsonObject& params) override;
    QJsonObject status() const override;

    /// Timing of the most recent video() pull (clock thread): how old the frame libobs rendered was at that moment.
    struct PullTiming {
        qint64 obsTimestampNs = 0;   // libobs frame timestamp (os_gettime_ns clock)
        qint64 ageNs = -1;           // pull time - obsTimestampNs
        qint64 sinceCopyNs = -1;     // pull time - time the raw callback copied it into the slot
        qint64 pullNs = 0;           // os_gettime_ns at the pull
        bool repeat = false;         // the same frame was already served
    };
    PullTiming lastPull() const;
    /// libobs' own clock (os_gettime_ns), for callers that want to compare against PullTiming.
    static qint64 obsNowNs();

    /// Install prefix used when the spec names none.
    static QString defaultRoot();
    static bool runtimeStarted();
    /// obs_shutdown (idempotent). Called automatically at QCoreApplication exit; tests call it in cleanup.
    static void shutdownRuntime();

private:
    struct Impl;
    Impl* m_impl;
    QJsonObject m_spec;
    bool m_open = false;

    bool buildScenes(const OutputFormat& fmt, QString* err);
    void teardownScenes();
    void workerLoop();
    void doSwitch(const QJsonObject& cmd);
    void doItemUpdate(const QJsonObject& cmd);
    static void rawVideo(void* param, struct video_data* frame);
    static void rawAudio(void* param, size_t mixIdx, struct audio_data* data);
};

#endif // PLAYPLAY_LIBOBSSOURCE_H
