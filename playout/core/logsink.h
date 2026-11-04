#ifndef PLAYPLAY_LOGSINK_H
#define PLAYPLAY_LOGSINK_H

#include <QString>

/**
 * File logging for the headless processes (Supervisor, engines).
 *
 * installFileLogging(dir, base) routes Qt messages to <dir>/<base>-YYYY-MM-DD.log as JSON lines
 * ({"ts","level","msg","cat"}, plus "dropped":N after lines were lost to a full queue) with daily rotation, and
 * deletes files older than `keepDays`. Logging never blocks the calling thread: a writer thread owns the file
 * (asynclog.h). Nothing goes to stderr unless PLAYPLAY_CONSOLE_LOG=1 or PLAYPLAY_LOG_STDERR=1 (echo on its own
 * bounded thread). Without a log dir (Supervisor --log-dir none, engine without --log-dir) Qt's default handler stays.
 * Safe to call once per process.
 */
void installFileLogging(const QString& dir, const QString& baseName, int keepDays = 14);
QString fileLoggingPath();

#endif // PLAYPLAY_LOGSINK_H
