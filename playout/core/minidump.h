#ifndef PLAYPLAY_MINIDUMP_H
#define PLAYPLAY_MINIDUMP_H

#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <atomic>
#include <string>
#include <vector>

/**
 * Self minidumps, the crash filter and in-process symbol lookup (docs/plans/perf-supervisor-app.md §5.1 and P1 rank
 * 20, W4).
 *
 * Why: the 10-25 hang left nothing to look at (no dump, no symbols), and the app's Qt6Core 0x1ac4ad access violation
 * on 10-23 could not be diagnosed. Here every process can write a dump of itself:
 *
 *   - A dump worker thread (plain Win32, started by init()) does every MiniDumpWriteDump. A hang dump requested by the
 *     watchdog and a crash dump requested by the unhandled-exception filter both only signal it and wait with a
 *     timeout, so a dump that blocks (loader lock held by a frozen thread, a stuck disk) never takes the caller with
 *     it, and a crash on a nearly exhausted stack still gets its dump (the worker has its own stack).
 *   - dbghelp.dll is loaded from System32 only (LoadLibraryEx LOAD_LIBRARY_SEARCH_SYSTEM32), never from the exe folder.
 *   - Files: <dir>/<name>-<pid>-<yyyyMMddTHHmmssZ>-<reason>.dmp (MiniDumpWithThreadInfo | WithUnloadedModules |
 *     WithIndirectlyReferencedMemory; crash dumps also carry the exception record). About 2-10 MB.
 *   - Rate limit from the files on disk, so it survives restarts (a crash loop cannot fill the disk): at most
 *     maxPerHour hang/manual dumps and maxPerHour crash dumps of this name in the last hour; only the newest `keep`
 *     dumps of this name are kept (older ones are deleted before a new one is written).
 *   - installCrashHandler(): SetUnhandledExceptionFilter. After the dump it calls the filter that was installed before
 *     (or returns EXCEPTION_CONTINUE_SEARCH), so the process still ends the way it did before (WER, exit code).
 *     Not covered: __fastfail, abort(), qFatal (TerminateProcess), CRT invalid-parameter and pure-call handlers, and
 *     any library that replaces the filter later.
 */
namespace minidump {

struct Options {
    QString dir;            // created if missing
    QString name;           // file prefix: "supervisor" | "app" | "engine-<id>" (sanitised to [A-Za-z0-9._-])
    int maxPerHour = 3;
    int keep = 20;
};

/// Resolves dbghelp, starts the worker. Safe to call again (updates dir/name/limits). False without dbghelp.
bool init(const Options& o);
bool isReady();

/// Writes a dump of this process from the worker thread and waits up to timeoutMs (or until *cancel turns true).
/// `reason` is a short lower-case ASCII word for the file name ("hang", "manual"). Returns the file path; empty when
/// rate-limited, busy, failed or timed out (*error says which; after "timeout" the worker may still finish the file).
/// Never call it on the thread whose state you want in the dump: the dump has every thread, but the caller waits.
QString writeDump(const char* reason, int timeoutMs = 30000, QString* error = nullptr,
                  const std::atomic<bool>* cancel = nullptr);

/// Installs the unhandled-exception filter (once per process; later calls are ignored). Needs init().
void installCrashHandler();

/// Dumps of `name` in dir, newest first (tests, diagnostics).
QStringList existingDumps(const QString& dir, const QString& name);

/// The name as it appears in file names.
QString sanitizedName(const QString& name);

/// Names for code addresses of this process, one per address: "module!function+0x1a (file.cpp:123)", or
/// "module+0x1234" without symbols. DbgHelp looks only next to each module (its CodeView PDB path, then the exe
/// folder): no symbol server, _NT_SYMBOL_PATH ignored. Serialised with the dump worker (DbgHelp is single-threaded).
/// Each call loads the PDBs it needs (a second or two for a big one) and drops the session afterwards (SymCleanup),
/// so a process does not keep ~55 MB of symbols after its first hang. Addresses after the first are treated as return
/// addresses (looked up at pc-1).
std::vector<std::string> symbolize(const quint64* pcs, int count);

} // namespace minidump

#endif // PLAYPLAY_MINIDUMP_H
