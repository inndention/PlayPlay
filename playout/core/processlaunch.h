#ifndef PLAYPLAY_PROCESSLAUNCH_H
#define PLAYPLAY_PROCESSLAUNCH_H

#include <QString>
#include <QtGlobal>

class QProcess;

/**
 * Console-less child launch (docs/plans/perf-supervisor-app.md §4, W2 = perf-engine PE-02).
 *
 * Why: until 2026-10-26 the app started the Supervisor, and the Supervisor started the engines, with
 * QProcess::ForwardedChannels. The app had a console (a Windows Terminal ConPTY tab), so QProcess passed creation
 * flags 0 and all four processes shared that console and its stdout/stderr handles. When the tab stopped draining,
 * every write to it blocked (the 10-24 / 10-25 outages), and closing the tab ended every process with 0xC000013A
 * (STATUS_CONTROL_C_EXIT). A child configured here cannot reach our console at all:
 *
 *   - stdin, stdout and stderr are the NUL device. The children log through asynclog to their --log-dir, so nothing
 *     the operator reads is lost. With `logFileOrEmpty` stdout and stderr are appended (merged) to that file instead,
 *     for crash breadcrumbs (CRT abort text, FFmpeg's default av_log); if it cannot be opened, NUL is used, so a bad
 *     path never stops a launch.
 *   - Windows: CREATE_NO_WINDOW, so a console program gets its own hidden console instead of inheriting ours (console
 *     close / Ctrl+C / Ctrl+Break of our console never reach it, GetConsoleWindow() is NULL in the child), and
 *     CREATE_NEW_PROCESS_GROUP (its own Ctrl group, Ctrl+C ignored by default). CREATE_NEW_CONSOLE and
 *     DETACHED_PROCESS are cleared because either would cancel CREATE_NO_WINDOW.
 *   - Elsewhere: a new session (setsid), the POSIX equivalent of leaving the controlling terminal.
 *
 * Call before start(); it never blocks. On Windows it installs a CreateProcessArgumentsModifier: a caller that needs
 * its own modifier must OR in detachedCreationFlags() itself. QProcess::kill() (TerminateProcess) and finished() are
 * unaffected; readAllStandardOutput() returns nothing (there is no pipe).
 */
namespace processlaunch {

void configureDetachedChild(QProcess& proc, const QString& logFileOrEmpty = QString());
inline void configureDetachedChild(QProcess* proc, const QString& logFileOrEmpty = QString()) {
    if (proc) configureDetachedChild(*proc, logFileOrEmpty);
}

/// CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP on Windows, 0 elsewhere.
quint32 detachedCreationFlags();

} // namespace processlaunch

#endif // PLAYPLAY_PROCESSLAUNCH_H
