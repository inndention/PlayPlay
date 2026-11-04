#ifndef OVERLAYMANAGER_H
#define OVERLAYMANAGER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>

namespace sched { class ScheduleStore; class ScheduleExecutor; }

/**
 * OverlayManager - what is on air, per channel, in the way of graphics (docs/overlays.md).
 *
 * The 50-template HTML CG pack (web/cg) reaches air through the engine's `html` layer source (a headless
 * Chromium driven over the DevTools protocol). This class is the Supervisor's side of it:
 *
 *   * deploy / update / re-take / hide / clear / delete on engine layers, with layer ids allocated per role
 *     band (never colliding with the compositor roles 10/15/20/30/40/80/90 or the caption layers 31/32);
 *   * html or native (the engine's own Qt-rendered template kinds) with an explicit fallback chain, so a
 *     missing browser degrades to the native twin and never to silence;
 *   * a state record per layer, reconciled with the engine telemetry `layers[]` every second (a layer the
 *     engine lost is marked `lost`, a default one is redeployed);
 *   * presets (hotkeys), defaults (always-on graphics: station bug), rules (suppress / show by item
 *     type, category, keyword, series) and cues (per-item `cgOverlays`, per-media `extra.cgCues`) driven
 *     from the on-air item the engine reports;
 *   * the renderer status / test / browser path.
 *
 * Everything runs on the Supervisor's main thread. The manager owns no sockets: commands leave through
 * Hooks::sendCg and everything that happens leaves as a signal (the Supervisor turns them into WS/IPC events).
 * REST-facing methods return a JSON object; one holding `error` and `status` is an error reply (the
 * RestApiServer convention, SupervisorCore::err).
 *
 * A layer's life: pending (show sent, engine has not attached it: an html page is attached only after its
 * first frame) -> on_air -> hiding (html: the page's own out animation runs first) -> removed. Hide and
 * delete on a pending layer are held until the engine has attached it, so a rapid deploy / hide never
 * leaves a graphic stuck on air. suppressed: hidden by a rule or by the operator's emergency option and
 * restored later. hidden: a default the operator took down. lost / failed: see the docs.
 */
class OverlayManager : public QObject {
    Q_OBJECT
public:
    struct Hooks {
        std::function<bool(const QString& channelId, const QJsonObject& cgCommand)> sendCg;   // MSG_CG_COMMAND to that engine; false when it is not connected
        std::function<bool(const QString& channelId)> engineConnected;
        std::function<QJsonObject()> manifest;           // web/cg/templates.json
        std::function<QJsonArray()> channelIds;          // optional: ids of every configured channel (default: the store's)
        std::function<qint64()> nowMs;                   // test clock (default: wall clock)
        QString webRoot;                                 // static root that serves /cg/*.html (template files are checked here)
        int restPort = 7700;                             // /cg/<id>.html is fetched from http://127.0.0.1:<restPort>
        /// Pillar panels (docs/pillar-panels.md): given the on-air item ({id,title,type,mediaType,category,width,height,live,path,
        /// input,extra}; {} when nothing is on air), the graphic that fills the side bars, {template, params, mode?}, or {} for none.
        std::function<QJsonObject(const QString& channelId, const QJsonObject& item)> pillarFor;
    };

    OverlayManager(sched::ScheduleStore* store, sched::ScheduleExecutor* exec, QObject* parent = nullptr);
    ~OverlayManager() override;

    void setHooks(const Hooks& h) { m_hooks = h; }
    void start();                                        // 100 ms timer
    void shutdown();

    // --- engine events (SupervisorCore wires these) ---------------------------------------------
    void onEngineConnected(const QString& channelId);
    void onEngineDisconnected(const QString& channelId);
    void onTelemetry(const QString& channelId, const QJsonObject& state);
    /// As-run rows as the executor stores them: START (item clock), CG_SHOW (layer attached by the engine).
    void onAsRun(const QString& channelId, const QString& eventType, const QString& runlistItemId, const QString& assetId,
                 qint64 actualStartMs, const QJsonObject& extra);
    /// Engine errors (MSG_UI_ALERT): "CG layer <n>: ..." fails the pending layer.
    void onEngineAlert(const QString& channelId, const QString& level, const QString& message);

    // --- REST ---------------------------------------------------------------------------------------
    QJsonObject overview();                                              // GET /overlays
    QJsonObject channelState(const QString& ch);                         // GET /overlays/{ch}
    QJsonObject deploy(const QString& ch, const QJsonObject& body);      // POST /overlays/{ch}/deploy
    QJsonObject update(const QString& ch, int layer, const QJsonObject& body);
    QJsonObject retake(const QString& ch, int layer);
    QJsonObject hide(const QString& ch, int layer, const QJsonObject& body);
    QJsonObject remove(const QString& ch, int layer);                    // DELETE /overlays/{ch}/{layer}
    QJsonObject clear(const QString& ch, const QJsonObject& body);       // POST /overlays/{ch}/clear

    QJsonObject presets();
    QJsonObject setPresets(const QJsonObject& body);
    QJsonObject firePreset(const QString& ch, const QString& presetId, const QJsonObject& body);
    QJsonObject hidePreset(const QString& ch, const QString& presetId);

    QJsonObject defaults(const QString& ch);
    QJsonObject setDefaults(const QString& ch, const QJsonObject& body);
    QJsonObject applyDefaults(const QString& ch);

    QJsonObject rules();
    QJsonObject setRules(const QJsonObject& body);
    QJsonObject testRules(const QString& ch, const QString& mediaId, const QHash<QString, QString>& query);

    // CG slots CG1, CG2, ... (setting overlays.cgSlots, docs/overlays.md "CG slots"): stored graphics (template, Text 1/2/3, timing),
    // each assigned to every switcher input ("*"), some inputs (in1..in8) or none. An assigned CG shows when its input goes on air
    // (it runs as a rule, id "cg:<id>"); any CG can also be fired by hand.
    QJsonObject cgSlots();                                               // GET /overlays/cgslots -> {slots[], byInput{in1:[ids]}}
    QJsonObject setCgSlots(const QJsonObject& body);                     // PUT /overlays/cgslots
    QJsonObject assignCgInput(const QJsonObject& body);                  // POST /overlays/cgslots/assign {input:"in3", cg:["cg1"]}
    QJsonObject fireCgSlot(const QString& ch, const QString& id, const QJsonObject& body);   // {toggle?, params?}
    QJsonObject hideCgSlot(const QString& ch, const QString& id);

    QJsonObject cues(const QString& ch);                                 // GET /overlays/{ch}/cues

    /// Re-ask Hooks::pillarFor for the item on air and deploy / update / hide the pillar layer (layer 11) to match. Called on every
    /// item change; the pillar controller calls it when its data or settings change.
    void refreshPillar(const QString& ch);
    /// The on-air item as Hooks::pillarFor sees it ({} when nothing is on air).
    QJsonObject onAirItem(const QString& ch) const;

    QJsonObject renderer();                                              // GET /overlays/renderer
    QJsonObject setRenderer(const QJsonObject& body);                    // PUT /overlays/renderer
    void testRenderer(const QJsonObject& body, std::function<void(int, const QJsonObject&)> reply);   // POST /overlays/renderer/test (async)

    /// Environment the engines are spawned with: PLAYPLAY_BROWSER (settings cg.browserPath), PLAYPLAY_HTML_NTH_FRAME
    /// (cg.htmlNthFrame), PLAYPLAY_HTML_FIRST_FRAME_MS (cg.htmlFirstFrameMs). Empty values are omitted.
    QHash<QString, QString> engineEnvironment() const;

    // --- pure helpers (public so a test can drive them) ---------------------------------------------
    /// Layer ids per role band: {role: [ids]} (docs/overlays.md has the table).
    static QJsonObject bandsJson();
    static QList<int> bandIds(const QString& role);
    static QString bandFor(const QString& role);                         // normalised band name for any role string
    /// Replace {{title}} {{series}} {{client}} {{category}} {{first_aired}} in every string of `v` (recursively).
    static QJsonValue expandTokens(const QJsonValue& v, const QHash<QString, QString>& tokens);
    /// "/cg/<template>.html?name=..&animIn=.." (the query vocabulary of web/cg/js/cg-client.js).
    static QString urlFor(const QString& templateFile, const QJsonObject& params, const QJsonObject& animation);
    static QJsonObject resolveAnimation(const QJsonValue& animation, const QJsonArray& manifestPresets);
    /// The engine `show` command that expresses a pack template with the engine's own template kinds, or an error
    /// object ({error, status: 409}) when the category has no native twin.
    static QJsonObject nativeCommandFor(const QJsonObject& tpl, const QJsonObject& params, const QString& role, const QJsonObject& animation, int rasterW, int rasterH);

signals:
    void layersChanged(const QString& channelId, const QJsonObject& payload);   // {channelId, layers[]}  -> WS "overlays"
    void configChanged(const QJsonObject& payload);                              // {kind: presets|rules|defaults|renderer, channelId?}  -> WS "overlayConfig"
    void alert(const QString& channelId, const QString& level, const QString& message);
    void cgHtmlState(const QString& channelId, const QJsonObject& state);        // the legacy /api/v1/cg/html/{ch} state + "cgHtml" event
    void engineEnvChanged();

private:
    // ---------------------------------------------------------------- records
    struct Layer {
        QString uid, ch;
        int layer = 0, slot = 0;
        QString role, tpl, filePath, name, mode, requestedMode, origin, label, hotkey;
        QString presetId, defaultId, cueKey, ruleId, itemKey;
        QJsonObject params, animation;
        QString url, liveUrl;                    // html: absolute URL, and the last URL the page was told to show
        QString state = "pending";
        double holdSec = 0.0;
        double itemHideAtSec = -1.0;             // cue layers: hide when the item's elapsed time passes this
        qint64 deployedMs = 0, sentMs = 0, shownAtMs = 0, holdUntilMs = 0, lostSinceMs = 0, failedAtMs = 0, lastRedeployMs = 0;
        qint64 pendingTimeoutMs = 0;
        double holdRemainingMs = -1.0;           // suppressed: hold left when it was taken down
        bool attachSeen = false;                 // an as-run CG_SHOW for this layer arrived after it was sent
        bool hideWhenReady = false;              // hide/delete asked while pending
        bool removeWhenReady = false;
        bool revealPending = false;              // html with an in animation: attached at opacity 0, replayed, then revealed
        bool userHidden = false;                 // a default the operator took down: not redeployed until applied again
        bool suppressOverride = false;           // deployed by the operator while its role was suppressed: leave it alone
        bool restored = false;
        bool invisible = false;                  // renderer test card: attached at opacity 0, never revealed
        bool updated = false;                    // live fields / re-take since deploy: the engine's page URL no longer has to equal ours
        int retakes = 0, redeploys = 0, urlMismatch = 0;
        QString error, note, fellBackFrom, engineState;
        QJsonObject engineStatus, hideTransition;
    };
    struct Media {                                // what rules / tokens see of the item on air
        QString id, title, type, series, client, category, subcategory, text;
        QString mediaType;                       // the library record's type (type is the runlist item's when it has one)
        QString path;                            // engine path of the item (ndi://.., sdi://N, rtsp://.., a file)
        QString input, inputTitle;               // "in1".."in8" when the path is one of the mapped switcher inputs (ui.inputMap)
        bool live = false;                       // a live source (item/media type live, or a live path)
        int width = 0, height = 0;
        QJsonObject extra;
        QJsonArray tags;
        bool found = false;
    };
    struct Cue {
        QString key, origin;                     // origin: item | media | rule
        QJsonObject spec;
        double showAt = 0.0, hideAt = -1.0;
        QString state = "waiting";               // waiting | fired | skipped
        QString reason;
        int layer = -1;
    };
    struct Item {
        bool active = false;
        QString runlistId, assetId, title, itemType, mediaId;
        qint64 startMs = 0;
        double joinOffsetSec = 0.0;
        double durationSec = 0.0;                // engine duration of this instance (0 = unknown)
        double totalSec = 0.0;                   // joinOffset + duration (0 = unknown)
        QList<Cue> cues;
        Media media;
        QStringList rulesFired;
        bool fromStart = false;
    };
    struct Clock { qint64 recvMs = 0; double pos = 0.0; bool running = false; };
    struct Suppressed { int layer = 0; QString role, uid; };
    struct ChannelState {
        QMap<int, Layer> layers;                 // by engine layer id
        QHash<int, QJsonObject> engine;          // telemetry layers[] by id
        QHash<int, qint64> tombstones;           // removed while pending: remove again if the engine still attaches them (id -> until)
        QHash<int, qint64> orphanSent;
        QSet<QString> fired;                     // "<runlistId>|<cueKey>": one fire per runlist id
        QList<Suppressed> suppressed;
        QSet<QString> suppressRoles;             // roles the rules (and the emergency option) want down right now
        QStringList suppressWhy;
        QSet<QString> ruleRoles; QStringList ruleWhy;   // the rules' share of the above
        Item item; Clock clock;
        QString state;                           // last engine state name
        QString startItemId; qint64 startAtMs = 0;
        qint64 telemetryMs = 0, htmlBackoffUntilMs = 0;
        bool connected = false, emergency = false, dirty = false;
        qint64 lastBrowserAlertMs = 0;
        qint64 pillarRetryAtMs = 0;              // a failed pillar deploy waits before the next try
        QString pillarError;
        int legacyLayer = -1;                    // the layer the legacy cgHtml state currently describes
        QHash<QString, int> defaultTries;        // default id -> failed redeploys in a row
    };
    struct Timed { qint64 atMs = 0; QString ch, uid, kind; int layer = 0; };
    struct Spec {                                 // everything deploy needs, from the REST body / a preset / a default / a cue
        QString tpl, filePath, role, mode = "auto", label, hotkey, origin = "operator";
        QString presetId, defaultId, cueKey, ruleId, itemKey, urlOverride;
        QJsonObject params; QJsonValue animation;
        int slot = -1, layer = -1;
        double holdSec = 0.0, itemHideAtSec = -1.0;
        bool exclusive = false, suppressOverride = false, invisible = false, restored = false;
    };
    struct PendingTest { QString ch, uid; qint64 startedMs = 0, timeoutMs = 0, okAtMs = 0; std::function<void(int, const QJsonObject&)> reply; };

    // ---------------------------------------------------------------- state
    sched::ScheduleStore* m_store;
    sched::ScheduleExecutor* m_exec;
    Hooks m_hooks;
    QTimer m_tick;
    QHash<QString, ChannelState> m_ch;
    QList<Timed> m_timed;
    QList<PendingTest> m_tests;
    QHash<QString, qint64> m_firstAired;         // media id -> earliest START (ms); found values never change
    QJsonObject m_browserCache; qint64 m_browserCacheMs = 0;
    qint64 m_lastTickMs = 0, m_lastHouseMs = 0;
    int m_seq = 0;

    // ---------------------------------------------------------------- helpers
    qint64 now() const;
    ChannelState& cs(const QString& ch) { return m_ch[ch]; }
    bool channelExists(const QString& ch, int* rasterW = nullptr, int* rasterH = nullptr) const;
    QJsonObject manifest() const;
    QJsonObject findTemplate(const QString& id) const;
    QJsonObject browserInfo();
    bool browserAvailable();
    QString browserMissingMessage();
    QString settingStr(const QString& key) const;

    // layers
    int allocLayer(ChannelState& c, const QString& band, int wantSlot, int wantLayer, QString* error) const;
    QJsonObject layerJson(const Layer& L) const;
    QJsonArray layersJson(const QString& ch) const;
    Layer* find(const QString& ch, int layer);
    Layer* findUid(const QString& ch, const QString& uid);
    QJsonObject deploySpec(const QString& ch, Spec s);                  // record JSON or an error object
    bool parseSpec(const QJsonObject& body, Spec* s, QJsonObject* errOut) const;
    QJsonObject buildShow(const Layer& L, QString* error) const;       // the engine `show` command for a record
    bool sendShow(Layer& L);                                            // build + send + (re)arm pending
    void onShown(Layer& L);                                             // the engine attached it
    void failLayer(Layer& L, const QString& why);
    void beginHide(Layer& L, const QJsonObject& transition = QJsonObject(), const QString& why = QString());
    void dropLayer(const QString& ch, int layer, bool sendRemove);
    void touch(const QString& ch) { m_ch[ch].dirty = true; }
    void schedule(qint64 atMs, const Layer& L, const QString& kind);
    void cancelTimed(const QString& ch, int layer);
    void runTimed(const Timed& t);
    int outWaitMs(const Layer& L) const;
    void sendEngine(const QString& ch, const QJsonObject& cmd);
    void emitLegacy(const Layer& L, bool cleared);
    void warnBrowser(const QString& ch, const QString& detail);
    bool templateFileOk(const QJsonObject& tpl, QString* why);
    int htmlFirstFrameMs() const;
    void startHideSequence(Layer& L, const QJsonObject& transition);
    void runHideEngine(Layer& L);

    // reconcile / lifecycle
    void reconcile(const QString& ch);
    void housekeeping();
    void tick();
    void redeployAfterRestart(const QString& ch);
    void ensureDefaults(const QString& ch, bool force);
    QJsonArray defaultList(const QString& ch) const;
    QJsonArray presetList() const;
    QJsonArray ruleList() const;
    static QJsonObject normalizePreset(const QJsonObject& p, int index);
    static QJsonObject normalizeDefault(const QJsonObject& p, int index);
    static QJsonObject normalizeRule(const QJsonObject& p, int index);

    // item / cues / rules
    void followItem(const QString& ch, const QJsonObject& t);
    void beginItem(const QString& ch, const QString& runlistId, const QString& assetId, const QString& title, qint64 startMs, double enginePos, double engineInPoint, bool fromStart, const QString& path = QString());
    void endItem(const QString& ch, const QString& why);
    double elapsed(const ChannelState& c) const;
    void buildCues(const QString& ch, Item& it, const QJsonArray& itemCues, const QJsonArray& mediaCues);
    void timeCue(Cue& c, double total) const;
    void tickCues(const QString& ch);
    void fireCue(const QString& ch, Cue& c);
    Media mediaFor(const QString& assetId, const QString& fallbackTitle, const QString& itemType, const QString& path = QString()) const;
    static QJsonObject mediaJson(const Media& m);
    void syncPillar(const QString& ch);
public:
    /// Rule matching on its own (public so a test can drive it): does rule `r` fire for this item on channel `ch`? `why` gets the
    /// per-criterion verdicts. Criteria: channels (always AND), types / categories / subcategories / series / keywords / inputs,
    /// combined with when.mode "all" (default) or "any"; rule.negate fires when the criteria do NOT match.
    static bool ruleMatches(const QJsonObject& r, const QString& ch, const Media& m, QJsonObject* why);
    static Media testMedia(const QJsonObject& o);   // a Media from JSON (tests): {id,title,type,mediaType,category,subcategory,series,text,path,input,inputTitle,live}
    // CG slots (pure, public for tests)
    static QJsonObject normalizeCgSlot(const QJsonObject& p, int index);
    static QString normCgInput(const QString& v);                 // "3" / "in3" / "Input 3" -> "in3"; "all" / "any" / "*" -> "*"
    static bool cgCoversInput(const QJsonObject& slot, const QString& input);
    static QJsonObject cgSlotRule(const QJsonObject& slot);       // the rule a slot runs as; {} when it has no graphic or no inputs
    /// Give input `input` exactly the CGs `cg` (ids); "*" slots that lose it become the explicit list of the other inputs.
    static QJsonArray assignCgInputIn(QJsonArray cgs, const QString& input, const QStringList& cg, int inputCount);
private:
    QJsonArray cgSlotList() const;
    int switcherInputCount() const;
    QHash<QString, QString> tokensFor(const QString& ch, const Media& m, qint64 itemStartMs);
    QString firstAired(const QString& ch, const QString& mediaId, qint64 beforeMs);
    qint64 earliestStart(const QString& mediaId, qint64 beforeMs);
    struct RuleHit { QString id, name; bool match = false; QJsonObject reasons; QJsonObject then; };
    QList<RuleHit> evaluateRules(const QString& ch, const Media& m) const;
    void applyRules(const QString& ch, const Item& it, const QList<RuleHit>& hits);
    void setSuppress(const QString& ch, const QSet<QString>& roles, const QStringList& why);
    void suppressLayer(const QString& ch, int layer, const QString& role);
    void restoreSuppressed(const QString& ch, bool all);
    void enforceSuppress(const QString& ch);
    static bool roleSuppressed(const QSet<QString>& roles, const QString& role);
    void checkEmergency(const QString& ch);
    void checkTests();
};

#endif // OVERLAYMANAGER_H
