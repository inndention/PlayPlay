#ifndef PLAYPLAY_LOOPWATCHDOG_H
#define PLAYPLAY_LOOPWATCHDOG_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <memory>

/**
 * Event-loop watchdog (docs/plans/perf-supervisor-app.md §5.1, W4): every process reports its own stalls.
 *
 * Why: on 10-24 / 10-25 the Supervisor's event loop froze for hours and nothing said so: the REST port accepted and
 * never answered, the logs just stopped. Here a process explains its next stall by itself:
 *
 *   - Beat: a main-thread QTimer (Qt::PreciseTimer, beatMs) stores the steady clock. lag = time since the last beat
 *     minus beatMs.
 *   - Monitor: a std::thread without a Qt event loop wakes every min(200, stallMs/5) ms and runs OK -> STALL (lag >=
 *     stallMs) -> HANG (lag >= hangMs) -> back to OK when the loop beats again (lag < stallMs/2), which is one
 *     "recovered" event and one recovered() signal per episode.
 *   - It never uses the path that may be blocked: it writes its own JSON lines with WriteFile (no qDebug, no asynclog)
 *     to <logDir>/watchdog-<name>-YYYY-MM-DD.jsonl (local date, files older than keepDays pruned):
 *       {"ts","event":"start|stall|hang|dump|recovered|abort","name","pid","lagMs","activity","mainCpuDeltaMs",
 *        "blocked","privateMB","handles","threads", ...}
 *     mainCpuDeltaMs is the main thread's CPU time since the stall began (GetThreadTimes): "blocked": true means it
 *     waits (lock, I/O, console), false means it is busy computing.
 *   - HANG: one stack sample of the main thread ("stack": up to 32 "module!function+0x.. (file:line)" frames). The
 *     thread is suspended only for GetThreadContext and a copy of its used stack (no locks, no allocation), then
 *     resumed; unwinding and symbol lookup run on the copy (minidump::symbolize, PDBs next to the exes, STB-3).
 *   - dumpAfterMs: a minidump of the whole process (minidump.h: worker thread, <logDir>/dumps, rate-limited from the
 *     files on disk). The same install sets the crash filter (crash dumps) unless crashDumps is false.
 *   - abortAfterMs > 0 (default 0 = never): after the dump, TerminateProcess(self, 0xDEAD) so a parent can relaunch.
 *   - /healthz on 127.0.0.1:healthPort (HealthEndpoint, its own thread) answers while the main thread is frozen:
 *       {"name","pid","state":"ok|stall|hang|exiting","lagMs","activity","lastBeatAgeMs","uptimeS","exe","build",
 *        "version","stalls","hangs","dumps","maxLagMs","recentMaxLagMs","beatMs","stallMs","hangMs"}
 *     Always HTTP 200 (the state is in the body: clients such as the app's stall banner drop an error reply's body).
 *     build = the exe's modification time (UTC).
 *   - Activity tag: what the main thread is doing right now, for the stall/hang lines and /healthz. setActivity() with
 *     a static string (cheapest), setActivityText() with a copied one (up to 95 bytes, seqlock), or WatchdogScope
 *     (RAII, restores the previous tag, nests). Main thread only: calls from other threads are ignored. Until the first
 *     beat the activity reads "startup".
 *   - When the application quits (aboutToQuit) monitoring stops ("exiting"): shutdown work is not a stall.
 *
 * Cost on the main thread: one timer callback per beatMs that reads the clock and stores a few atomics.
 */
struct LoopWatchdogOptions {
    QString name;               // "supervisor" | "app" | "engine-<id>"
    QString logDir;             // JSON lines + <logDir>/dumps/; empty = no files (no JSONL, no dumps)
    int beatMs = 250;
    int stallMs = 1000;
    int hangMs = 5000;
    int dumpAfterMs = 10000;    // 0 = no hang dump
    int abortAfterMs = 0;       // 0 = never
    int healthPort = 0;         // 127.0.0.1 only; 0 = off, -1 = any free port (tests; healthPort() tells which)
    int maxDumpsPerHour = 3;
    int keepDumps = 20;
    int keepDays = 30;          // watchdog-<name>-*.jsonl older than this are deleted at install
    bool crashDumps = true;     // install the unhandled-exception filter (needs logDir)
    bool stackSample = true;    // main-thread stack on HANG
};

class LoopWatchdog : public QObject {
    Q_OBJECT
public:
    enum State { Ok = 0, Stall = 1, Hang = 2 };
    struct Stats {
        int state = Ok;
        qint64 lagMs = 0;            // now
        qint64 recentMaxLagMs = 0;   // largest lag in the last 5-10 s (for /metrics: a scrape on the main thread itself
                                     // always sees ~0 now)
        qint64 maxLagMs = 0;         // largest lag since install (including one in progress)
        quint64 stalls = 0, hangs = 0, dumps = 0;
        quint16 healthPort = 0;
    };

    /// Installs the process-wide watchdog (call on the main thread after Q[Core|Gui]Application exists). A second
    /// install replaces the first (tests). Returns nullptr without an application object.
    static LoopWatchdog* install(const LoopWatchdogOptions& o);
    static LoopWatchdog* instance();

    /// Activity tag of the main thread. `staticTag` must outlive the process (a literal).
    static void setActivity(const char* staticTag);
    static void setActivityText(const QByteArray& text);
    static QByteArray activity();

    ~LoopWatchdog() override;

    Stats stats() const;
    /// (Re)binds /healthz; 0 turns it off. False (and *error) when the port cannot be bound.
    bool setHealthPort(quint16 port, QString* error = nullptr);
    quint16 healthPort() const;
    const LoopWatchdogOptions& options() const;
    QString jsonlPath() const;      // today's file ("" without logDir)
    QByteArray healthJson() const;  // the /healthz body; callable from any thread

signals:
    /// Main thread, once per stall episode, after the loop runs again. worst = "stall" | "hang"; activity = the tag
    /// when the stall was detected.
    void recovered(const QString& name, qint64 stalledMs, const QString& activity, const QString& worst);

private:
    explicit LoopWatchdog(const LoopWatchdogOptions& o, QObject* parent);
    void beat();
    struct Impl;
    std::unique_ptr<Impl> d;
};

/// RAII activity tag for the main thread: sets the tag, restores the previous one (static or copied) on exit.
class WatchdogScope {
public:
    explicit WatchdogScope(const char* staticTag);
    explicit WatchdogScope(const QByteArray& text);
    ~WatchdogScope();
    WatchdogScope(const WatchdogScope&) = delete;
    WatchdogScope& operator=(const WatchdogScope&) = delete;
private:
    void save();
    bool m_active = false;
    const char* m_prevTag = nullptr;
    bool m_prevText = false;
    char m_prevBuf[96] = {};
};

#endif // PLAYPLAY_LOOPWATCHDOG_H
