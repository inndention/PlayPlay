#ifndef PLAYPLAY_BROWSERHOST_H
#define PLAYPLAY_BROWSERHOST_H

#include <QObject>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include "../mediaframe.h"

class QProcess;
class QWebSocket;
class QNetworkAccessManager;
class BrowserHost;
class HtmlPage;
struct HtmlPageShared;

/**
 * Headless Chromium-family renderer for HTML overlays (the engine's `html` layer source).
 *
 * One browser per engine process (Chrome, else Edge: playout/core/browserlocator.*), driven over the DevTools
 * protocol through ONE websocket to the browser endpoint with flattened sessions. Every HTML layer is its own
 * page in its own (headless) window, so any number of overlays stream concurrently (measured: the browser's
 * compositor delivers about 58 screencast frames per second in total, shared by the pages that are animating;
 * a static page costs nothing after its first frame).
 *
 * Threading: BrowserHost and every HtmlPage live on a dedicated "PlayPlayBrowserHost" QThread (not the process
 * main thread: the CDP traffic and the PNG base64 handling stay off the engine's IPC thread). Every public
 * member below is thread-safe and non-blocking unless it says otherwise, because HtmlSource objects are
 * created and opened on detached loader threads and updated / polled / closed on the clock thread.
 *
 * Proven command sequence per page (see docs/overlay-renderer.md for the spike numbers):
 *   Target.createTarget{about:blank,w,h,newWindow:true} -> Target.attachToTarget{flatten:true}
 *   -> Page.enable, Runtime.enable, Network.enable, Emulation.setDeviceMetricsOverride{w,h,dsf 1},
 *      Emulation.setDefaultBackgroundColorOverride{rgba 0,0,0,0} -> Page.navigate -> (Page.loadEventFired)
 *   -> Page.bringToFront -> Page.startScreencast{png, everyNthFrame}
 * The order matters: bringToFront / startScreencast issued before the page has loaded leaves some windows
 * without frames; a Page.stopScreencast + Page.startScreencast revives them, which is the watchdog's remedy.
 */

/// PE-19: the screencast ingest helpers (host thread), public for test_browserhost.
namespace htmlingest {
/// 64-bit content digest (qHashBits plus the length) of a screencast payload. Equal digests only nominate a skip: the
/// payloads are then compared byte for byte (sameBytes), so a hash collision can never hold an old picture.
struct Digest {
    quint64 h = 0;
    qint64 n = -1;                                           // -1 = none (never equal to a real payload)
    bool operator==(const Digest& o) const { return n >= 0 && n == o.n && h == o.h; }
    bool operator!=(const Digest& o) const { return !(*this == o); }
};
Digest digest(const QByteArray& bytes);
/// Exact test: same digest AND same bytes (a memcmp only when the digests match, i.e. when the skip is real).
inline bool sameBytes(const Digest& da, const QByteArray& a, const Digest& db, const QByteArray& b) { return da == db && a == b; }
/// Byte-level split of a CDP `Page.screencastFrame` message: copies the base64 of params.data straight from the UTF-16
/// text into *b64 (no QString->UTF-8 copy of the whole message, no JSON string for the picture) and returns in *rest the
/// message with "data":"" (a few hundred bytes for QJsonDocument). False when the text has no plain "data":"..." string
/// (escapes, no key): the caller parses the whole message as before.
bool splitScreencastData(QStringView text, QByteArray* b64, QString* rest);
/// Decode threads for `livePages` streaming pages: one slot per page, at least 2 (today's pool), at most 8.
/// PLAYPLAY_HTML_DECODE_THREADS=n fixes it (2 = the old fixed pool).
int decodeThreadsFor(int livePages);
bool fastParseEnabled();                                     // PLAYPLAY_HTML_FASTPARSE=0 turns the byte-level parse off
bool skipSameDefault();                                      // PLAYPLAY_HTML_SKIP_SAME=0 turns the identical-PNG skip off
} // namespace htmlingest

/// What an HtmlPage needs to know at creation.
struct HtmlPageOptions {
    QString url;                    // http(s)://, file:///, data: ... (normalised by the caller)
    int width = 1920;
    int height = 1080;
    bool transparent = true;        // Emulation.setDefaultBackgroundColorOverride rgba(0,0,0,0)
    int everyNthFrame = 0;          // 0 = automatic (2 while one page streams, 1 when several do); PLAYPLAY_HTML_NTH_FRAME forces
    double maxFps = 0;              // decode no more often than this (0 = every frame the browser sends)
    int loadTimeoutMs = 6000;       // how long to wait for the load event before streaming anyway
    int idlePauseMs = 2000;         // stop the screencast when nobody asked for a frame this long (0 = never)
    QString label;                  // for log lines ("main/30")
    QJsonObject fields;             // data-field values applied after every load (no reload)
    bool premultiplied = false;     // PE-08: decode into premultiplied BGRA once, on the decode thread (HtmlSource sets it
                                    // with PLAYPLAY_PREMUL=1); false = straight alpha, as before
    bool pngPremultiplied = false;  // PE-08: the browser's screencast PNGs hold PREMULTIPLIED colours (headless Chromium /
                                    // Edge here: a 50 % red box decodes as 128,0,0,128), so the bytes are taken as such
                                    // instead of being premultiplied a second time (HtmlSource: PLAYPLAY_HTML_PNG_PREMULTIPLIED=1).
                                    // false = the PNG is read as straight alpha, as before (translucent areas come out darker
                                    // than the page asked for)
};

/**
 * One browser page rendered to RGBA frames. Created by BrowserHost::createPage(); owned through a shared_ptr
 * (the last owner's release closes the target and deletes the object on the host thread).
 */
class HtmlPage : public QObject {
    Q_OBJECT
public:
    enum class State { Starting, Loading, Streaming, Paused, Restarting, Failed, Closed };
    static QString stateName(State s);

    ~HtmlPage() override;

    // ---- thread-safe, never block (except waitForFirstFrame, which blocks the CALLING thread) ----
    /// Newest decoded frame (BGRA, frameSize(); straight alpha, or premultiplied when the page was created with
    /// HtmlPageOptions::premultiplied); null before the first frame. Marks the page "in use": a screencast the idle
    /// pause stopped is resumed by this call.
    FramePtr latestFrame();
    QSize frameSize() const;
    quint64 frameCount() const;
    State state() const;
    QString url() const;
    QString lastError() const;
    /// Blocks until the first frame was published, the open failed (precise reason in *error) or the timeout hit.
    bool waitForFirstFrame(int timeoutMs, QString* error = nullptr);
    /// {url, state, frames, framesIn, fps, lastFrameAgeMs, restarts, lastError, width, height, decodeMs, ...}
    QJsonObject status() const;

    void navigate(const QString& url);                       // re-take: new URL in the same page
    /// Runs `js` in the page (queued until the page has loaded). done(ok, value) runs on the host thread.
    void evaluate(const QString& js, std::function<void(bool ok, const QJsonValue& value)> done = {});
    /// Sets textContent of [data-field=name], #name and #nameText (img: src, input: value) and calls window.update(fields)
    /// when the page defines it; when nothing matched and reloadOnMiss, re-navigates with the fields merged into the query.
    void setFields(const QJsonObject& fields, bool reloadOnMiss = true);
    /// "in" | "out" | "replay": drives window.CGAnim when the template exposes it.
    void animate(const QString& what);
    /// Releases the target (async). Idempotent; also done by the destructor.
    void close();

private:
    friend class BrowserHost;
    HtmlPage(BrowserHost* host, const HtmlPageOptions& opt, std::shared_ptr<HtmlPageShared> sh);

    // ---- host thread only ----
    void attach();                                           // browser is ready: create + attach + load
    void onBrowserGone();                                    // browser died: wait for the restart
    void onBrowserShutdown();                                // process is exiting
    void onEvent(const QString& method, const QJsonObject& params);
    void onTargetLost(const QString& why);
    void onTargetDestroyed(const QString& targetId);
    void slowTick(qint64 nowMs);                             // watchdogs (0.5 s)
    void setNth(int nth);
    bool isLive() const;                                     // attached and loading/streaming
    int desiredNth() const { return m_opt.everyNthFrame; }
    bool autoNth() const { return m_opt.everyNthFrame <= 0; }

    void setup(int gen);
    void dropSession();
    void onNavigateReply(int gen, const QJsonObject& r, const QString& e);
    void resumeIfPaused();
    void onLoaded();
    void startStreaming();
    void startScreencast();
    void armWatchdog(bool reset);
    void restartScreencast(const char* why);
    /// `b64` = params.data already split out by the byte-level parse (PE-19); null = take it from params.
    void onScreencastFrame(const QJsonObject& params, QByteArray b64 = QByteArray());
    void pump();
    void onConsole(const QJsonObject& params);
    void onException(const QJsonObject& params);
    void onResponse(const QJsonObject& params);
    void failOpen(const QString& why);
    void noteError(const QString& why, bool warn);
    void doNavigate(const QString& url, bool explicitTake);
    void doEvaluate(const QString& js, std::function<void(bool, const QJsonValue&)> done);
    void doSetFields(const QJsonObject& fields, bool reloadOnMiss);
    void doClose();
    void flushQueued();
    void reattachSoon(int delayMs);
    void setState(State s);
    void closeTargetId(const QString& targetId);
    QString mergedUrl(const QJsonObject& fields) const;
    void logRate(const QString& level, const QString& text);

    QPointer<BrowserHost> m_host;
    HtmlPageOptions m_opt;
    std::shared_ptr<HtmlPageShared> m_sh;
    QString m_label;
    QString m_url;                                           // current URL (explicit navigates update it)
    QString m_targetId, m_sessionId, m_mainFrameId;
    int m_gen = 0;                                           // invalidates callbacks of an older attach
    bool m_closed = false, m_loaded = false, m_attached = false, m_screencasting = false;
    bool m_expectFrame = false;
    qint64 m_armMs = 0, m_loadStartMs = 0, m_createdMs = 0;
    int m_wdAttempts = 0;
    qint64 m_nextWdMs = 0;
    int m_nth = 2;
    QJsonObject m_fields;                                    // fields applied to the DOM since the last navigation
    struct Queued { QString js; std::function<void(bool, const QJsonValue&)> done; };
    QList<Queued> m_queued;
    // decode pump (m_pending holds the BASE64 text since PE-19: fromBase64 runs in the decode job, off the host thread)
    QByteArray m_pending;
    htmlingest::Digest m_pendingDigest, m_sentDigest;       // PE-19: of m_pending / of the picture last handed to a decode
    QByteArray m_sent;                                       // PE-19: that picture's base64 (shared, not copied) for the exact compare
    QTimer m_pumpTimer;
    qint64 m_lastDecodeStartMs = 0;
    qint64 m_minIntervalMs = 0;
    // console rate limit
    qint64 m_logWindowMs = 0;
    int m_logCount = 0, m_logSuppressed = 0;
    QString m_httpError;
};

/**
 * The browser process and its DevTools connection. Singleton on its own thread; created lazily by the first
 * instance() call (launch is lazy too: the first createPage() starts the browser).
 *
 * Environment variables read (all optional): PLAYPLAY_BROWSER (preferred browser executable, via
 * cgbrowser::find), PLAYPLAY_HTML_NTH_FRAME (forces everyNthFrame for every page), PLAYPLAY_HTML_EXTRA_ARGS
 * (space separated extra browser arguments), PLAYPLAY_HTML_VERBOSE=1 (forward page console.log / warnings too),
 * PLAYPLAY_HTML_GPU=1 (do not pass --disable-gpu). PE-19: PLAYPLAY_HTML_FASTPARSE=0 (parse screencast messages as whole
 * JSON again), PLAYPLAY_HTML_SKIP_SAME=0 (decode identical PNGs again; read per page at creation),
 * PLAYPLAY_HTML_DECODE_THREADS=n (fixed decode pool; default one slot per streaming page, 2..8),
 * PLAYPLAY_HTML_STATS=1 (an "[Html] ingest" line per page every 10 s).
 */
class BrowserHost : public QObject {
    Q_OBJECT
public:
    static BrowserHost* instance();
    /// Starts the browser now (so the first overlay does not pay the 1-2 s launch). No-op when no browser is installed.
    static void prewarm();
    /// Kills the browser, closes every page and stops the host thread. Safe to call twice; also runs on aboutToQuit.
    static void shutdown();

    ~BrowserHost() override;

    /// Preferred executable (settings cg.browserPath); empty = PLAYPLAY_BROWSER, then the usual install locations.
    void setBrowserPath(const QString& preferred);
    /// Creates a page (does not wait for frames: use HtmlPage::waitForFirstFrame). Null + *error when no browser is
    /// installed ("no Chromium-family browser found (set settings cg.browserPath or PLAYPLAY_BROWSER); tried: ...").
    std::shared_ptr<HtmlPage> createPage(const HtmlPageOptions& opt, QString* error = nullptr);

    /// {state, path, version, product, pid, port, profile, restarts, pages, targets, lastError, uptimeMs}
    QJsonObject status() const;
    int pageCount() const { return m_pageCount.load(); }       // registered, not yet closed
    int targetCount() const { return m_targetCount.load(); }   // attached browser targets
    QString browserPath() const;
    QString browserVersion() const;
    QString lastError() const;

private:
    friend class HtmlPage;
    enum class State { Idle, Launching, Connecting, Ready, Backoff, Missing, Stopping };
    using Reply = std::function<void(const QJsonObject& result, const QString& error)>;

    explicit BrowserHost(QObject* parent = nullptr);
    static QString stateName(State s);
    bool ready() const { return m_state == State::Ready; }
    int forcedNth() const { return m_forcedNth; }
    int autoNth() const { return m_autoNth; }
    void setState(State s);

    bool invokeSync(const std::function<void()>& fn, int timeoutMs);
    void initOnHost();
    std::shared_ptr<HtmlPage> createPageOnHost(const HtmlPageOptions& opt, QString* error);
    void stopOnHost();
    QString resolveExe(QString* why);
    void ensureLaunched();
    void launch();
    void pollVersion();
    void openSocket(const QString& wsUrl);
    void onSocketConnected();
    void onBrowserLost(const QString& why);
    void scheduleRestart();
    void tick();
    void rebalance();
    void onMessage(const QString& text);
    void onBrowserEvent(const QString& method, const QJsonObject& params);
    void cleanStaleProfiles();
    void adopt(QProcess* p);
    void registerPage(HtmlPage* p);
    void unregisterPage(HtmlPage* p);
    void bindSession(const QString& sessionId, HtmlPage* p) { m_sessions.insert(sessionId, p); }
    void unbindSession(const QString& sessionId) { m_sessions.remove(sessionId); }
    void targetAttached(bool on) { m_targetCount += on ? 1 : -1; }
    void setError(const QString& e);
    void setInfo(const QString& path, const QString& version, const QString& product);
    void removeProfile();

    /// CDP command. `owner` (may be null) guards the callback: it is skipped when the owner was deleted.
    int send(const QString& method, const QJsonObject& params = QJsonObject(), const QString& sessionId = QString(),
             QObject* owner = nullptr, Reply cb = Reply());

    struct Pending { QPointer<QObject> owner; bool hasOwner = false; Reply cb; qint64 sentMs = 0; QString method; };

    // ---- host thread state ----
    QProcess* m_proc = nullptr;
    QWebSocket* m_ws = nullptr;
    QNetworkAccessManager* m_nam = nullptr;
    QTimer* m_pollTimer = nullptr;
    QTimer* m_restartTimer = nullptr;
    QTimer* m_tickTimer = nullptr;
    State m_state = State::Idle;
    QString m_preferred, m_exe, m_profileDir;
    quint16 m_port = 0;
    int m_failStreak = 0, m_nextId = 1, m_profileSeq = 0, m_forcedNth = 0;
    qint64 m_launchMs = 0, m_readyMs = 0;
    QHash<int, Pending> m_pending;
    QHash<QString, HtmlPage*> m_sessions;
    QList<HtmlPage*> m_pages;
    bool m_staleCleaned = false, m_pollBusy = false;
    int m_autoNth = 2;
    int m_decodeThreads = 2;                                   // PE-19: current decode pool size
    qint64 m_lastStatsMs = 0;                                  // PE-19: PLAYPLAY_HTML_STATS line
    QString m_product;
    void* m_job = nullptr;                                     // Windows job object (kill-on-close)

    // ---- shared with other threads ----
    mutable QMutex m_infoMtx;
    QString m_infoState, m_infoPath, m_infoVersion, m_infoProduct, m_infoError, m_infoProfile, m_infoPreferred;
    qint64 m_infoPid = 0;
    int m_infoPort = 0;
    std::atomic<int> m_restarts{0}, m_pageCount{0}, m_targetCount{0};
    std::atomic<qint64> m_infoReadyMs{0};
};

#endif // PLAYPLAY_BROWSERHOST_H
