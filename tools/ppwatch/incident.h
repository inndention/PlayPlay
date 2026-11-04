#ifndef PLAYPLAY_PPWATCH_INCIDENT_H
#define PLAYPLAY_PPWATCH_INCIDENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <vector>

/**
 * PlayPlayWatchdog (docs/plans/perf-supervisor-app.md §5.3, W19): what ppwatch reads about other processes, and the
 * incident bundle it writes when one of them hangs.
 *
 * Read-only system queries (no debugger, nothing attached, nothing suspended):
 *   - processSnapshot(): every process with its threads, from one NtQuerySystemInformation(SystemProcessInformation)
 *     call: parent pid, create time, handle count, private bytes, CPU, and per thread its state and WAIT REASON
 *     (UserRequest, DelayExecution, Suspended, WrLpcReply, WrAlertByThreadId ...): what the 10-25 hang needed and nobody
 *     could see without attaching a debugger.
 *   - tcpTable(): GetExtendedTcpTable with owning pids (the netstat part of the bundle, and who owns a listening port).
 *   - commandLine(): another process's command line (NtQueryInformationProcess, PROCESS_QUERY_LIMITED_INFORMATION).
 *   - tailLines(): the end of a log file, opened with full sharing so the writer never notices.
 *
 * Actions, each through a process HANDLE whose create time and image were checked first (a pid recycled between the
 * decision and the action is refused):
 *   - dumpProcess(): MiniDumpWriteDump of another process (dbghelp from System32). DbgHelp suspends the target's threads
 *     while it copies them: a few hundred ms for a Supervisor-sized process.
 *   - terminateProcess() / launchDetached(): only for the opt-in restart (main.cpp decides; default off).
 *
 * writeIncident() writes <root>/<yyyyMMddTHHmmssZ>-<target>-<pid>/:
 *   incident.json   what, when, who (identity checks), the last /healthz bodies, the thread summary, the files below
 *   threads.txt     every thread of the process: state, wait reason, CPU, context switches, start module, name
 *   processes.txt   PlayPlay* processes (and the target's parent and children) with parents, command lines (secrets
 *                   masked) and the orphan check
 *   netstat.txt     TCP rows on the watched ports and every row of the target pid, with a per-port state count
 *   logs/<file>.tail.txt   the last N lines of each configured log (the newest file matching each glob)
 *   <target>-<pid>-<utc>-hang.dmp   when dumping is on and not rate-limited
 */
namespace ppwatch {

struct ThreadInfo {
    quint32 tid = 0;
    int state = -1;              // KTHREAD_STATE (5 = Waiting)
    int waitReason = -1;         // KWAIT_REASON
    qint64 createTime = 0, userTime = 0, kernelTime = 0;   // FILETIME units (100 ns)
    quint32 contextSwitches = 0;
    int priority = 0;
    quint64 startAddress = 0;    // kernel's (usually RtlUserThreadStart); describeThreads() reads the Win32 one
};

struct ProcInfo {
    quint32 pid = 0, ppid = 0;
    QString image;               // file name only ("PlayPlaySupervisor.exe")
    qint64 createTime = 0;       // FILETIME units; 0 for the idle/system pseudo processes
    quint32 threadCount = 0, handles = 0, sessionId = 0;
    quint64 privateBytes = 0, workingSet = 0;
    qint64 userTime = 0, kernelTime = 0;
    std::vector<ThreadInfo> threads;
};

std::vector<ProcInfo> processSnapshot(bool withThreads, QString* error = nullptr);
const ProcInfo* findProcess(const std::vector<ProcInfo>& procs, quint32 pid);
QString threadStateName(int state);
QString waitReasonName(int reason);

/// Per thread, as JSON objects {tid, state, waitReason, userMs, kernelMs, contextSwitches, priority, start, name,
/// first}; start = "module+0x..." of the Win32 start address, name = SetThreadDescription's text. *text gets the
/// same as a table.
QJsonArray describeThreads(const ProcInfo& p, QString* text = nullptr);

struct TcpRow {
    bool v6 = false;
    QString localAddr, remoteAddr;
    quint16 localPort = 0, remotePort = 0;
    int state = 0;               // MIB_TCP_STATE_* (2 = LISTEN, 5 = ESTABLISHED, 8 = CLOSE_WAIT)
    quint32 pid = 0;
    QString stateName() const;
};
std::vector<TcpRow> tcpTable(QString* error = nullptr);
/// Owner of the LISTEN socket on port (loopback or any-address, IPv4 or IPv6); 0 when nothing listens.
quint32 listenerPid(const std::vector<TcpRow>& rows, quint16 port);
QString netstatText(const std::vector<TcpRow>& rows, const QList<quint16>& ports, quint32 alsoPid,
                    const std::vector<ProcInfo>* procs = nullptr);

/// Facts about a live process, read through a handle.
struct ProcessId {
    quint32 pid = 0;
    qint64 createTime = 0;
    QString image;               // file name
    QString path;                // full image path
    bool alive = false;
};
ProcessId queryProcess(quint32 pid);
bool sameImage(const QString& image, const QString& expected);   // file names, case-insensitive
/// System and session processes ppwatch never dumps or terminates, whatever the configuration says.
bool isProtectedProcess(quint32 pid, const QString& image);

QString commandLine(quint32 pid, QString* error = nullptr);
QStringList splitCommandLine(const QString& cmd);                // CommandLineToArgvW rules
/// Values of options that look like credentials (--api-key x, --token=x, password ...) become "(hidden)".
QString redactCommandLine(const QString& cmd);

struct EngineArgs { QString id; int port = 0; QString logDir; bool standalone = false; };
EngineArgs parseEngineArgs(const QString& cmd);

struct Orphan { quint32 pid = 0, ppid = 0; QString reason; };
/// Processes named engineExe whose parent is gone, was replaced by another process with the same pid (created after
/// the child), or is not supervisorExe. Report-only input (§6): ppwatch never touches them.
std::vector<Orphan> findOrphans(const std::vector<ProcInfo>& procs, const QString& engineExe, const QString& supervisorExe);

/// The last n lines (without line ends) of a file; reads at most maxBytes from the end.
QStringList tailLines(const QString& path, int n, QString* error = nullptr, qint64 maxBytes = 8 * 1024 * 1024);
/// Newest file matching a glob in its folder ("C:\\logs\\supervisor-*.log"); a plain path if it exists; else "".
QString newestMatch(const QString& glob);

bool dumpProcess(quint32 pid, qint64 expectCreateTime, const QString& path, QString* error, qint64* ms = nullptr);
bool terminateProcess(quint32 pid, qint64 expectCreateTime, const QString& expectImage, quint32 exitCode, int waitMs,
                      QString* error);
/// CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NUL stdin/stdout/stderr, no other handle inherited, working directory
/// = the program's folder. Returns the new pid (0 and *error on failure).
quint32 launchDetached(const QString& commandLine, QString* error);

QString sanitizedName(const QString& name);
QString utcStamp(qint64 msSinceEpoch);                           // yyyyMMddTHHmmssZ

struct IncidentRequest {
    QString root;                // <out>/incidents
    QString target, url, role;
    quint32 pid = 0;
    qint64 createTime = 0;
    QString image;
    QString trigger;             // hang | unresponsive
    qint64 episodeMs = 0;
    QJsonObject lastHealth;      // newest /healthz body
    QJsonArray polls;            // recent observations
    QJsonObject identity;        // how the pid was verified
    QStringList logGlobs;        // the target's and the global ones
    QList<quint16> ports;        // netstat filter
    bool dump = true;
    QString dumpSkipped;         // why no dump (rate limit, dumping off for this target)
    int tailLines = 500;
    qint64 nowMs = 0;            // wall clock for names (0 = now)
    QString engineExe = QStringLiteral("PlayPlayChannelEngine.exe");     // processes.txt orphan check
    QString supervisorExe = QStringLiteral("PlayPlaySupervisor.exe");
    QStringList images;          // more image names to list in processes.txt (the configured exe names)
};

struct IncidentResult {
    bool ok = false;
    QString dir, dumpPath, dumpError, error, summary;
    qint64 ms = 0, dumpMs = 0;
    QJsonObject manifest;        // what incident.json holds
};
IncidentResult writeIncident(const IncidentRequest& req);

/// Incident folders of `target` (exact name) whose UTC stamp is within the last withinSecs seconds; with withDump only
/// those holding a .dmp. Read from disk, so the limits survive a ppwatch restart.
int recentIncidents(const QString& root, const QString& target, qint64 withinSecs, bool withDump, qint64 nowMs = 0);
/// Keeps the newest `keep` incident folders in root (only folders named like ours); returns how many were removed.
int pruneIncidents(const QString& root, int keep);

} // namespace ppwatch

#endif // PLAYPLAY_PPWATCH_INCIDENT_H
