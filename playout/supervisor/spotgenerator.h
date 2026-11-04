#ifndef SPOTGENERATOR_H
#define SPOTGENERATOR_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QTimeZone>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QProcess>
#include <functional>
#include <memory>
#include "../schedule/schedulemodel.h"

namespace sched { class ScheduleStore; }

/**
 * SpotGenerator — auto-generated spots (port of PlayPlay OBS newsroom.py + newsspots.py).
 *
 * A RECIPE (weather, headlines, story, history, ...) says where the material comes from (RSS feeds,
 * Open-Meteo, Wikipedia "on this day", free text), which voice reads it, how the library record is
 * classified and how long the spot stays airable. A JOB runs a recipe on a cadence (every N minutes
 * or at clock times) and pins the freshest generation into the weekly grid through locked GEN
 * markers (query blocks that resolve to the newest matching media at build time). The news SCATTER
 * turns unseen RSS stories into verbatim story spots and sprinkles up to N per hour across today's
 * dated slots at banded random minutes.
 *
 * One generation at a time, never blocking the event loop:
 *   fetch (QNetworkAccessManager) -> write (templates, optional OpenRouter-style LLM) -> photo
 *   -> furniture PNG + bed (scripts/spot_card.py, Pillow) -> narration WAV (scripts/spot_tts.py:
 *   Kokoro if present, else Windows SAPI) -> ffmpeg (zoompan bed + furniture overlay + narration
 *   + ducked music bed) -> ingest callback -> library record (type news|weather, category
 *   "autogen rotation", perishable, availableEndMs = now + TTL, tag "gen:<jobId>" | "newsScatter")
 *   -> placements refreshed.
 *
 * GEN / scatter markers are SOFT locked anchors (lockAtSec, never a hard start): the RunlistBuilder seats
 * them at the item boundary nearest the minute and cuts nothing. Each marker's query block is pinned to its
 * own job through the media tag "gen:<jobId>" (scatter stories: "newsScatter"), so a newer spot of the same
 * subcategory from another job never airs under the wrong GEN label.
 *
 * Settings: spots.jobs {jobs[]}, spots.recipes {<key>: overrides}, spots.weather {lat, lon, place,
 * units}, spots.llm {apiKey, model, baseUrl}, spots.scatter {enabled, channel, perHour, feeds[],
 * voice, ttlMin, maxNewPerRun, everyMin, minSec, seen[], fails{}}, spots.tts {engine, kokoroDir,
 * python, speed}, spots.ffmpeg, spots.keepDays, spots.brand {accent, ink}, spots.market {default}
 * (the station default area market), spots.markets {<key>: market data amending / adding Martinsville, Reidsville, Danville: areamarket.h},
 * spots.engines {card, tts, music, compose, dir} (DEP-13: "python"/"ffmpeg" = the old programs, the default; "native" /
 * "auto" = PlayPlaySpotRender / PlayPlaySpeech / PlayPlayMediaTool with the same arguments, exit 64 falls back; dir = where
 * the tools are, default next to the Supervisor; tools/ppcommon/toolswitch.h).
 */
class SpotGenerator : public QObject {
    Q_OBJECT
public:
    using IngestFn = std::function<void(const QString& path, const QString& type, const QJsonObject& meta)>;
    using Done = std::function<void(bool ok, const QJsonObject& result)>;

    SpotGenerator(sched::ScheduleStore* store, const QString& dataDir, const QString& scriptsDir, QObject* parent = nullptr);
    ~SpotGenerator() override;

    void setIngest(IngestFn f) { m_ingest = std::move(f); }
    void setOutputDir(const QString& dir);            // where finished MP4s land (default <dataDir>/spots/out)
    void start();                                      // arm the job / scatter / expiry timers

    // --- recipes + configuration -----------------------------------------------------------------
    QJsonArray recipes() const;                        // built-ins merged with settings spots.recipes overrides
    QJsonObject recipe(const QString& key, bool* found = nullptr) const;
    QJsonArray recipesIn(const QString& market) const;   // recipes() with the area market applied under the operator's overrides
    QJsonObject recipeIn(const QString& key, const QString& market, bool* found = nullptr) const;   // the recipe localized to an area market ("" = generic); operator overrides still win
    QJsonObject config() const;                        // {weather, llm, scatter, tts, ffmpeg, keepDays, brand, python, dirs, env}
    QJsonObject setConfig(const QJsonObject& patch);

    // --- area markets (areamarket.h): Martinsville / Reidsville / Danville, defined as data ------------------
    QJsonArray markets() const;                        // every market, expanded: built-ins merged with settings spots.markets
    QJsonObject marketData(const QString& key) const;  // one expanded market ({} when unknown)
    QString stationMarket() const;                     // settings spots.market.default ("" = none: the generic recipes)
    QString resolveMarket(const QString& requested, const QJsonObject& job, const QString& recipeKey) const;   // request > job > recipe override > station default
    QJsonObject marketInfo() const;                    // {default, effectiveDefault, markets: [summaries]} - what the pickers list

    // --- jobs --------------------------------------------------------------------------------------
    QJsonArray jobs() const;
    QJsonObject job(const QString& id, bool* found = nullptr) const;
    QJsonObject saveJob(const QJsonObject& body, const QString& id = QString());   // id empty = create; else merge
    bool deleteJob(const QString& id);
    QJsonObject runJob(const QString& id, Done done);                              // async; done(ok, {mediaId, path, durationSec, headline})
    QJsonObject generate(const QJsonObject& req, Done done);                       // {recipe, channel, data, voice, feeds[]} one-off
    void previewScript(const QJsonObject& req, std::function<void(const QJsonObject&)> cb);
    bool cancel(const QString& genId);

    // --- placements / GEN markers ------------------------------------------------------------------
    QJsonObject applyPlacements(const QString& jobId);
    QJsonObject enforceMarkers();                      // coalesced: the slot-PUT hook calls it per save, so the pass runs once ~300 ms after the first request ({queued: true})
    QJsonObject enforceMarkersNow();                   // the pass itself: {placed, updated, removed, jobs}
    QJsonArray markers() const;                        // every GEN / scatter item across template + this week's dated slots
    QJsonObject markerStates() const;                  // {jobId: {state: fresh|due|stale|off, reason, ...}}

    // --- news scatter --------------------------------------------------------------------------------
    QJsonObject scatterGenerate(Done done);            // fetch feeds, queue story spots; done after the feeds are read
    QJsonObject scatterApply(const QString& channel, int perHour);
    QJsonObject scatterStatus() const;

    // --- status / maintenance -----------------------------------------------------------------------
    QJsonObject status() const;
    QJsonObject spotInfo(const QString& mediaId) const;
    void onMediaIngested(const QString& mediaId, const QString& origin);
    int expireSweep();

signals:
    void event(const QString& type, const QJsonObject& payload);
    void alert(const QString& level, const QString& message);

private:
    struct Gen {
        QString id, jobId, recipeKey, channel, data, voice, origin;   // origin: job|manual|scatter|preview
        QString step = "queued", error, workDir, outPath, wavPath, furniturePath, bedPath, photoPath, photoUrl, headline, ttsEngine, storyUrl;
        QString notes;                                 // editor's note for the writer (revise a spot with a note)
        QString musicPath, musicMid, musicInfo;        // the composed music bed of this spot (wav), its MIDI file, "126 bpm A major (sports)"
        QJsonObject extraMeta;                         // caller fields merged into the library record (spot studio: review state, category)
        QJsonObject engines;                           // DEP-13: which program ran each tool step {card: "native" | "python" | "python (fallback: ...)", ...}
        QJsonObject recipe, script, weather, wiki;
        QJsonArray items, feeds;                       // material items [{title, text, link, image, pubMs, source, category}], feed urls
        QJsonArray stories;                            // Headline News: the picked stories [{title, summary, place, category, image, link, pubMs}]
        QJsonObject local;                             // local spots (localspots.h): what the fetch step parsed {page | incidents | games | listings, ...}
        QJsonObject prebuilt;                          // local spots: the finished script (scenes with their own narration), used instead of a writer
        QString pickMls;                               // realty: the listing this generation features (marked featured once its spot exists)
        QString market;                                // area market key this spot is made for ("" = generic); the recipe above is already localized to it
        QJsonObject marketJson;                        // that market's expanded data (voice notes, pronunciations, weather point)
        double ttlMin = 0, targetSec = 60, wavSec = 0, durSec = 0;
        int pending = 0, genNum = 0;
        qint64 queuedMs = 0, startedMs = 0, finishedMs = 0;
        bool previewOnly = false, materialReady = false, skipped = false;
        QList<Done> done;
        QJsonObject toJson() const;
    };
    using HttpCb = std::function<void(bool ok, const QByteArray& body, const QString& contentType, const QString& error)>;
    using ProcCb = std::function<void(int code, const QByteArray& out, const QByteArray& err)>;

    sched::ScheduleStore* m_store;
    QString m_dataDir, m_scriptsDir, m_outDir;
    IngestFn m_ingest;
    QNetworkAccessManager m_nam;
    QList<Gen*> m_gens;                                // queued, running and awaiting-ingest generations
    Gen* m_current = nullptr;
    QProcess* m_proc = nullptr;
    QTimer m_procTimer, m_tick, m_sweep, m_notify, m_scatterSoon, m_enforceSoon;
    QList<QJsonObject> m_recent;                       // last results, newest last
    bool m_scatterBusy = false;
    qint64 m_scatterGenMs = 0, m_scatterApplyMs = 0;
    QJsonObject m_lastScatter;
    qint64 m_deferEnforceMs = 0;                       // a placement change skipped the hour on air: re-apply once that hour is over
    QSet<QString> m_deferredJobs;                      // jobs whose placements were (partly) held back for the same reason
    struct ToolCaps;                                   // toolswitch::CapsCache (kept out of this header)
    std::unique_ptr<ToolCaps> m_toolCaps;              // `<tool> version` per exe, probed once, asynchronously
    int m_pyKokoro = -1;                               // -1 not known yet, 0 no kokoro package, 1 present (auto keeps tts on the script)
    bool m_pyKokoroBusy = false;
    QHash<QString, qint64> m_fallbackLogMs;            // step -> last "fell back" log (once per hour)

    // generation pipeline
    void pump();
    Gen* findGen(const QString& id) const;
    Gen* enqueue(Gen* g);
    void startGen(Gen* g);
    void bindMarket(Gen* g, const QString& requested, const QJsonObject& job);     // resolve the market, localize g->recipe to it
    void fetchSources(Gen* g);
    void onFetched(Gen* g);
    void writeScript(Gen* g);
    void llmScript(Gen* g, std::function<void()> next);
    QJsonObject templateScript(Gen* g) const;
    QJsonObject weatherScript(Gen* g) const;
    void fetchPhoto(Gen* g);
    void renderCard(Gen* g);
    void synthVoice(Gen* g);
    void compose(Gen* g);
    // Local spots (recipe render "scenes", source cinema | traffic | sports | realty): structured pages -> scenes with their own narration (localspots.h)
    bool sceneRender(const Gen* g) const;
    void fetchLocal(Gen* g);
    void buildLocal(Gen* g);
    QString weatherFullUrl(const Gen* g) const;
    QJsonObject lakesFrom(const QJsonObject& aep, const QJsonObject& usgs, bool persist);      // lake readings with a 24 h trend from the history in settings spots.lakes
    // Headline News (recipe render "stories"): story pages -> one scene per story (headlinenews.h)
    bool storiesMode(const Gen* g) const;
    int storyCount(const Gen* g) const;
    void enrichStories(Gen* g);
    QJsonObject storiesScript(Gen* g) const;
    void fetchStoryPhotos(Gen* g);
    void renderStoryCards(Gen* g);
    void synthStoryVoices(Gen* g);
    void composeStories(Gen* g);
    void ingest(Gen* g);
    void finishGen(Gen* g, bool ok, const QJsonObject& result);
    void failGen(Gen* g, const QString& error) { QJsonObject o; o["error"] = error; finishGen(g, false, o); }
    void runProcess(Gen* g, const QString& program, const QStringList& args, int timeoutMs, ProcCb cb);   // cb code: exit code, -1 crash, -2 not started, -3 timed out
    // DEP-13 engine switches: step "card" | "tts" | "music" | "compose". The old program and its arguments are what the call site
    // ran before; the tool gets the same arguments (toolswitch::nativeArgs). okFn = the step's own success test, used only to decide
    // whether a native run falls back. cb sees whichever run counted.
    void runStep(Gen* g, const QString& step, const QString& legacyProgram, const QStringList& legacyArgs, int timeoutMs, std::function<bool(int code, const QByteArray& out)> okFn, ProcCb cb);
    QJsonObject engineFor(const QString& step) const;  // {mode, uses: native|python|ffmpeg, tool, exe, reason, ready}
    QString toolPath(const QString& tool) const;       // the tool exe in spots.engines.dir, else next to the Supervisor ("" = none)
    void probeKokoroPackage();                         // async: is the `kokoro` pip package (spot_tts.py's first engine) in the Supervisor's Python?
    void httpGet(const QString& url, int timeoutMs, HttpCb cb, qint64 maxBytes = 6 * 1024 * 1024, bool imageOnly = false);   // body capped (aborts past maxBytes); imageOnly also aborts on a non-image content type
    void httpPost(const QString& url, const QJsonObject& body, const QStringList& headers, int timeoutMs, HttpCb cb);

    // material
    QString weatherUrl(const Gen* g) const;
    QJsonObject weatherFor(const Gen* g) const;        // {place, lat, lon, units}: the market's weather point when one is bound, else settings spots.weather
    QStringList feedsFor(const QJsonObject& recipe, const QJsonArray& jobFeeds) const;
    QJsonObject pickMusic(int energy) const;
    // MIDI-style music bed (scripts/spot_music.py): used when no music files are in spots/music (or spots.music.mode = "generated")
    QString moodFor(const Gen* g) const;                      // recipe "mood", else by recipe key / energy
    QJsonObject musicFor(const Gen* g) const;                 // the generated bed of this spot, else a file from spots/music by energy
    void makeMusic(Gen* g, std::function<void()> next);       // composes the bed (about 1 s) between the voice and the compose step
    void keepMidi(Gen* g);                                    // copies the composed MIDI next to the finished spot

    // jobs
    QJsonArray loadJobs() const;
    void storeJobs(const QJsonArray& a);
    QJsonObject normalizeJob(const QJsonObject& in) const;
    void seedJobs();
    qint64 nextDueMs(const QJsonObject& job, qint64 nowMs, bool* dueNow) const;
    double effectiveCadenceMin(const QJsonObject& job) const;
    double ttlForJob(const QJsonObject& job) const;
    void tick();
    void runDueJobs();
    sched::SlotItem makeMarker(const QJsonObject& job, const QJsonObject& recipe, int atMin, const QString& existingId) const;
    QJsonObject applyPlacementsFor(const QJsonObject& job, bool remove = false);
    QString genTitle(const QJsonObject& job, const QJsonObject& recipe) const;
    bool airableSpot(const QJsonObject& job, sched::MediaRecord* out = nullptr) const;   // the job's lastMediaId exists and its window is open
    void ensureTag(sched::MediaRecord& m, const QString& tag);                          // adds a media tag (saved) when missing
    void ensureCategory(sched::MediaRecord& m, const QString& category);                // moves a spot to its recipe's library category (saved) when it differs
    void saveSlotQuiet(const sched::HourSlot& s, QSet<QString>& touched);              // saveSlot without the per-slot "schedule" event
    void announceSlots(const QSet<QString>& touched);                                   // one "schedule" event per touched channel
    void promoteJobSpot(QJsonObject& job, const QString& mediaId, const QString& headline, qint64 whenMs);   // lastMediaId bookkeeping + closing the prior window
    void adoptLateSpot(const sched::MediaRecord& m);                                   // an autogen record that landed after its generation gave up waiting
    QStringList allowedChannelsFor(const Gen* g) const;                                 // channel scope written onto the library record

    // scatter
    void scatterTick();
    void onScatterFeeds(const QJsonArray& items, Done done);
    QJsonObject scatterCfg() const;
    void setScatterCfg(const QJsonObject& o);

    // helpers
    QTimeZone tzFor(const QString& channel) const;
    QDateTime nowIn(const QString& channel) const;
    QString firstChannel() const;
    QString pythonPath() const;
    QString ffmpegPath() const;
    QString workRoot() const;
    QString musicDir() const;
    QString backgroundsDir() const;
    QString defaultChannelFor(const QJsonObject& job) const;
    void notify() { if (!m_notify.isActive()) m_notify.start(300); }
    void log(const QString& level, const QString& msg);
};

#endif // SPOTGENERATOR_H
