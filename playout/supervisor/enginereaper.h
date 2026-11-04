#ifndef ENGINEREAPER_H
#define ENGINEREAPER_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace sched { struct ChannelProfile; }

/**
 * enginereaper — finds the channel engines a previous Supervisor left running and decides, per engine, whether this
 * Supervisor adopts it (air continues) or kills it before spawning its own (perf-supervisor PS-4a / W6, design
 * docs/plans/perf-supervisor-app.md §6.2).
 *
 * This file is the core only: the pure decision (decide / decideAll) and the scan. Nothing here kills, adopts or
 * touches a process; PS-4b wires it into SupervisorCore::start / ChannelManager (adoptChannel, async TerminateProcess,
 * the 60 s re-scan).
 *
 * SCOPE IS THE SAFETY RULE. A scan only ever selects an engine whose Supervisor IPC name equals ours and whose --data /
 * lock dataDir lies under our dataDir. A test Supervisor (IPC PlayPlayLaneTest, its own data dir) therefore never sees
 * the live engines, and the live Supervisor never sees a test's. Anything that cannot be proven in scope (no IPC name,
 * a relative data path, an unreadable command line, --standalone) is left alone and only noted.
 */
namespace reaper {

// What is known about one running engine process.
struct EngineFacts {
    QString channelId;
    qint64 pid = 0;
    quint64 createTime = 0;      // FILETIME ticks (exact): the PID-reuse guard
    QString exe;                 // native image path
    qint64 exeMtimeMs = 0;       // from engine.lock (the build it started from); 0 = unknown (no lock)
    QString configHash;
    QString supervisorIpc;
    qint64 supervisorPid = 0;    // the Supervisor it last belonged to (from engine.lock)
    QString dataDir;             // its channel data dir
    bool fromLock = false;       // found through <dataDir>/channels/<id>/engine.lock
    bool fromCommandLine = false;// found by the process list (an engine without a lock: an older build)
    // Filled by PS-4b from MSG_ENGINE_HELLO; at scan time empty / 0.
    QString state;               // "ON_AIR", "IDLE", ...
    qint64 helloAtMs = 0;        // when its HELLO reached us; 0 = none yet
};

// The engine build this Supervisor would spawn.
struct BuildFacts {
    QString exe;                 // path of PlayPlayChannelEngine.exe next to the Supervisor
    qint64 exeMtimeMs = 0;
};

enum class Verdict { Adopt, Kill };
enum class Reason { None, UnknownChannel, Disabled, BuildMismatch, Duplicate, AdoptDisabled };

struct ReapDecision {
    Verdict verdict = Verdict::Kill;
    Reason reason = Reason::None;
    QString text;                // for the alert: "reaped orphan engine pid 15704 (ch2, started 10:13:52, build mismatch)"
    EngineFacts facts;
};

QString reasonName(Reason r);    // "unknown channel", "build mismatch", ...

// True when the engine came from `current`: same exe path (case-insensitive, separators normalised) and, with a lock,
// the same exe mtime; without a lock, an exe file not modified after the process started.
bool sameBuild(const EngineFacts& f, const BuildFacts& current);

// The pure decision for one engine. `profile` = the channel's stored profile, nullptr when the channel is unknown.
// `duplicate` = another engine of the same channel was chosen to stay. Order: unknown, disabled, build, duplicate,
// adoption switched off (setting engines.adoptOrphans), else ADOPT.
ReapDecision decide(const EngineFacts& f, const sched::ChannelProfile* profile, const BuildFacts& current,
                    bool adoptEnabled, bool duplicate = false);

// decide() for every engine at once: per channel, among the engines that would otherwise be adopted, one stays (ON_AIR
// first, then the earliest HELLO, then the oldest process) and the rest are KILL / duplicate.
using ProfileLookup = std::function<const sched::ChannelProfile*(const QString& channelId)>;
QVector<ReapDecision> decideAll(const QVector<EngineFacts>& engines, const ProfileLookup& profileOf,
                                const BuildFacts& current, bool adoptEnabled);

// --- scope helpers (pure) ------------------------------------------------------------------------------------------
QString normalizeIpcName(const QString& ipcName);                    // "\\.\pipe\X" -> "x" (pipe names ignore case)
bool sameIpc(const QString& a, const QString& b);
bool pathUnder(const QString& path, const QString& root);             // equal or inside; never "root2"; case-insensitive
// The engine's own options from its command line (engine_main.cpp): --id, --data, --supervisor (default
// "PlayPlaySupervisor" when absent, as the engine itself assumes), --standalone. Accepts "--opt v" and "--opt=v".
struct EngineArgs { QString id, data, supervisor; bool standalone = false; bool supervisorGiven = false; };
EngineArgs parseEngineArgs(const QStringList& argv);
QStringList splitCommandLine(const QString& commandLine);             // CommandLineToArgvW rules

// --- scan -----------------------------------------------------------------------------------------------------------
struct ScanOptions {
    QString dataDir;                                   // the Supervisor's data dir (channels/<id>/engine.lock below it)
    QString ipcName;                                   // the Supervisor's IPC name
    QString engineExeName = QStringLiteral("PlayPlayChannelEngine.exe");   // process-list match (tests use their own)
    bool useLocks = true;
    bool useProcessList = true;
};

struct ScanResult {
    QVector<EngineFacts> engines;                      // alive, verified, in scope
    QStringList staleLocks;                            // lock files whose process is gone or whose pid was reused
    QStringList notes;                                 // why something was skipped (out of scope, unreadable, ...)
};

// Blocking (opens processes, reads files): call it on a worker thread, never on the Supervisor's event loop.
ScanResult scan(const ScanOptions& opt);

// Process helpers (Windows; empty off Windows). The command line comes from NtQueryInformationProcess
// (ProcessCommandLineInformation), which needs only PROCESS_QUERY_LIMITED_INFORMATION.
QString processCommandLine(qint64 pid);
struct ProcessEntry { qint64 pid = 0; QString exeName; };
QVector<ProcessEntry> listProcesses(const QString& exeName);           // Toolhelp32, exe name compared case-insensitively

} // namespace reaper

#endif // ENGINEREAPER_H
