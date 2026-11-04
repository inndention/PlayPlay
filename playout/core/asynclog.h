#ifndef PLAYPLAY_ASYNCLOG_H
#define PLAYPLAY_ASYNCLOG_H

#include <QDate>
#include <QString>
#include <QtGlobal>

/**
 * Non-blocking Qt message sink (docs/plans/perf-supervisor-app.md §4, W1 = perf-engine PE-01).
 *
 * Why: on 2026-10-24 and 10-25 every process of the stack wrote its log synchronously to one shared console; when
 * that console stopped draining, WriteFile(stderr) blocked the Supervisor's event loop, the app's GUI thread and an
 * engine's clock thread. Here no logging thread ever touches the console or the disk:
 *
 *   - The Qt message handler copies the message (QString is shared, so no deep copy) into a lock-free MPSC ring
 *     (8 192 cells by default) and returns. When the ring is full the message is dropped and counted; the next line
 *     that gets in carries "dropped": N.
 *   - One writer thread drains the ring into <dir>/<base>-YYYY-MM-DD.log (JSON lines {"ts","level","msg","cat"},
 *     the format logsink always had), rotates daily, prunes files older than keepDays, and flushes whenever the ring
 *     runs empty, for every warning or worse, and at least every flushMs.
 *   - Console echo is off. PLAYPLAY_CONSOLE_LOG=1 or PLAYPLAY_LOG_STDERR=1 turns it on: a second thread with its own
 *     1 024-cell ring writes the message text to stderr with raw WriteFile (no CRT stream lock held). If the console
 *     blocks, only that thread blocks; its ring fills and further echo lines are dropped (counted).
 *   - Qt's previous/default handler (stderr, OutputDebugString) is never called on the logging thread.
 *     PLAYPLAY_LOG_DEBUGGER=1 routes the echo thread through it instead of the raw stderr write.
 *   - qFatal: drains the ring and writes the fatal line synchronously (under a bounded lock wait), flushes, returns
 *     (Qt then aborts).
 *   - Process exit (atexit): drains and flushes, waits at most ~3 s for the writer and 300 ms for a blocked echo
 *     thread (then leaves it behind: exit must never hang on a console). Lines logged after that are written
 *     synchronously to the file with a bounded lock wait.
 */
namespace asynclog {

struct Options {
    QString dir;                    // log directory (created if missing)
    QString baseName;               // <base>-YYYY-MM-DD.log
    int keepDays = 14;              // prune <base>-*.log older than this at each rotation
    int ringSize = 8192;            // rounded up to a power of two
    int flushMs = 250;              // longest time a written line stays in the file buffer
    bool consoleEcho = false;       // echo thread (PLAYPLAY_CONSOLE_LOG / PLAYPLAY_LOG_STDERR)
    int consoleRingSize = 1024;
    bool echoViaPreviousHandler = false;   // PLAYPLAY_LOG_DEBUGGER: the echo thread calls Qt's previous handler
};

// Turns on the echo from PLAYPLAY_CONSOLE_LOG=1 / PLAYPLAY_LOG_STDERR=1 / PLAYPLAY_LOG_DEBUGGER=1.
void applyEnvironment(Options& o);

// Starts the writer (and echo) thread and installs the Qt message handler. Calling it again replaces the previous
// sink (its ring is drained into its own file first).
void install(const Options& o);

// Waits until every message logged before the call is written and flushed; false on timeout.
bool flush(int timeoutMs = 1000);

// Drains, flushes and stops the threads. Later messages are written synchronously. restorePreviousHandler puts back
// the handler that was installed before the first install() (tests only: in production that is Qt's stderr handler).
void shutdown(bool restorePreviousHandler = false);

QString currentPath();

struct Stats {
    quint64 written = 0;            // lines written to the file
    quint64 dropped = 0;            // lines lost because the ring was full
    quint64 consoleWritten = 0;
    quint64 consoleDropped = 0;     // echo lines lost because the echo ring was full (console blocked or slow)
    quint64 writeErrors = 0;        // lines that could not be written (file not open)
    bool installed = false;
    bool consoleEcho = false;
};
Stats stats();

namespace testing {
void setDateOverride(const QDate& d);       // invalid date = use the clock
void setWriterPaused(bool paused);          // the writer stops draining (to fill the ring on purpose)
QtMessageHandler handler();                 // the installed handler, to call it directly (qFatal without the abort)
}

} // namespace asynclog

#endif // PLAYPLAY_ASYNCLOG_H
