#ifndef SINGLEINSTANCE_H
#define SINGLEINSTANCE_H

#include <QList>
#include <QString>
#include <memory>

namespace enginelock { class InstanceGuard; }

/**
 * SingleInstance — one Supervisor per IPC name (perf-supervisor PS-4a / W6, design §6.2).
 *
 * Windows named pipes accept several server instances under one name, so two Supervisors on one IPC name would split the
 * engines between them (each engine connects to whichever instance answers). The named mutex
 * Local\PlayPlaySupervisor-<ipcName> is held for the life of the process; the OS releases it when the process ends,
 * however it ends (a crashed owner leaves it "abandoned" and the next Supervisor gets it).
 *
 * Only the guard lives here; what a refused Supervisor does (ask the old one's /healthz, exit "already running", or with
 * --replace-hung dump and terminate it) is supervisor_main's job (PS-4b). The IPC name is case-folded because pipe names
 * ignore case and mutex names do not: "PlayPlaySupervisor" and "playplaysupervisor" are one pipe and one guard.
 */
class SingleInstance {
public:
    explicit SingleInstance(const QString& ipcName);
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Takes the mutex, waiting up to waitMs (0 = just try). True when this process holds it, also when the previous
    // holder died without releasing it (abandoned()). Call it on the thread that lives as long as the process (main).
    bool tryAcquire(int waitMs = 0);
    bool owned() const;
    bool abandoned() const;
    QString name() const;
    QString error() const;

    static QString mutexName(const QString& ipcName);   // "Local\PlayPlaySupervisor-<ipc, lower case, no pipe prefix>"

private:
    std::unique_ptr<enginelock::InstanceGuard> m_guard;
};

/**
 * supguard — what supervisor_main does with the guard (PS-4b, design §6.2). Runs before the event loop and before
 * anything binds a port, so its short blocking waits stall nothing.
 *
 *   mutex free (or abandoned)                    -> run
 *   held, the holder's /healthz says ok/exiting -> exit kExitAlreadyRunning ("already running")
 *   held, /healthz says hang (or stays at stall), or takes the connection and never answers (its watchdog is stuck)
 *       without --replace-hung                   -> exit kExitAlreadyRunning (logged as possibly hung)
 *       with --replace-hung                      -> dump the old instance, terminate it, take the mutex, run (its
 *                                                   engines are then adopted by this instance's orphan scan)
 *   held, and its health cannot be judged: no port to ask (--health-port 0), connection refused (the holder's /healthz
 *   is on another port, e.g. setting watchdog.port, or off), or an answer that is not a watchdog state
 *                                                -> exit kExitAlreadyRunning, also with --replace-hung: the watchdog's
 *                                                   /healthz answers "hang" even while the holder's loop is frozen, so
 *                                                   silence on a refused port is no evidence of a hang
 * The old instance's pid comes from /healthz when it answers, else from the process list (same exe name, same
 * --ipc-name on its command line); an ambiguous or missing pid is never guessed: exit instead.
 */
namespace supguard {

constexpr int kExitAlreadyRunning = 6;

struct Health {
    bool answered = false;
    bool connected = false; // the port took the connection
    bool timedOut = false;  // connected, then no complete reply within the timeout while the connection stayed open
    QString state;          // ok | stall | hang | exiting (LoopWatchdog /healthz)
    qint64 pid = 0;
    QString raw;
};
// GET http://127.0.0.1:<port>/healthz, blocking up to timeoutMs (main thread, before the loop).
Health probeHealth(quint16 port, int timeoutMs = 1500);

// Positive evidence of a hang: state hang / stall, or connected and then no reply (timedOut).
bool looksHung(const Health& h);

enum class Action { Run, ExitRunning, Replace };
// Pure (table-tested). acquired = this process holds the mutex.
Action decide(bool acquired, const Health& h, bool replaceHung);
const char* actionName(Action a);

// Supervisors on this IPC name other than this process: processes named exeName whose command line says
// --ipc-name / -i <ipc> (default "PlayPlaySupervisor" when absent, as supervisor_main assumes).
QList<qint64> findSupervisorPids(const QString& ipcName, const QString& exeName);
// MiniDumpWriteDump of another process into dir (dbghelp loaded at run time). Returns the file path or empty.
QString dumpProcess(qint64 pid, const QString& dir, const QString& tag, QString* error = nullptr);
// TerminateProcess and wait up to waitMs for it to go. True when it has exited.
bool terminateAndWait(qint64 pid, int waitMs, QString* error = nullptr);

struct Options {
    QString ipcName;
    quint16 healthPort = 0;        // the old instance's /healthz (0 = do not ask)
    bool replaceHung = false;
    QString dumpDir;               // where --replace-hung writes the old instance's dump
    QString exeName;               // process-list match; default: this executable's file name
    int healthTimeoutMs = 1500;
    int stallRetries = 3;          // "stall" is asked again (1 s apart) before it counts as hung
    int predecessorWaitMs = 3000;  // a predecessor that is just exiting gets this long to release the mutex
    int terminateWaitMs = 5000;
};
struct Result {
    bool run = false;
    int exitCode = 0;
    QString message;               // what happened, for the log
    qint64 oldPid = 0;
    QString dumpPath;
};
// The whole flow; `guard` stays owned (and must live) for the rest of the process when run is true.
Result acquireOrReplace(SingleInstance& guard, const Options& o);

} // namespace supguard

#endif // SINGLEINSTANCE_H
