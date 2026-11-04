#include "processlaunch.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace processlaunch {

quint32 detachedCreationFlags() {
#ifdef Q_OS_WIN
    return CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
#else
    return 0;
#endif
}

namespace {
// A breadcrumb file is optional: QProcess refuses to start when a redirect file cannot be opened, and a missing log
// folder must never keep the Supervisor or an engine off air. So check it here and fall back to NUL.
bool usableLogFile(const QString& path) {
    if (path.isEmpty()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Append);
}
} // namespace

void configureDetachedChild(QProcess& proc, const QString& logFileOrEmpty) {
    proc.setInputChannelMode(QProcess::ManagedInputChannel);       // ForwardedInputChannel would hand over our stdin
    proc.setStandardInputFile(QProcess::nullDevice());             // a read gets EOF at once, never our console
    if (usableLogFile(logFileOrEmpty)) {
        // One handle for both streams: two append handles on one file would overwrite each other's bytes.
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.setStandardOutputFile(logFileOrEmpty, QIODevice::Append);
    } else {
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setStandardErrorFile(QProcess::nullDevice());
    }
#ifdef Q_OS_WIN
    proc.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags &= ~static_cast<unsigned long>(CREATE_NEW_CONSOLE | DETACHED_PROCESS);
        a->flags |= CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    });
#else
    proc.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
}

} // namespace processlaunch
