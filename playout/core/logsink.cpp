#include "logsink.h"
#include "asynclog.h"
#include <QDebug>
#include <QDir>

// The file sink is asynclog (writer thread, bounded ring). Before 2026-10-26 this handler wrote the file and then
// chained synchronously to Qt's default handler (stderr) on the logging thread; a console that stopped draining froze
// the Supervisor's event loop and an engine's clock thread (docs/plans/perf-supervisor-app.md §1.3).
void installFileLogging(const QString& dir, const QString& baseName, int keepDays) {
    asynclog::Options o;
    o.dir = dir;
    o.baseName = baseName;
    o.keepDays = keepDays;
    asynclog::applyEnvironment(o);
    asynclog::install(o);
    qInfo().noquote() << "[Log] writing" << QDir(dir).filePath(baseName + "-YYYY-MM-DD.log")
                      << (o.consoleEcho ? "(async, console echo on)" : "(async, file only)");
}

QString fileLoggingPath() { return asynclog::currentPath(); }
