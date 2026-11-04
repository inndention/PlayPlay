#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <cstdlib>
#include <cstdio>
#include <QStandardPaths>
#include <QDebug>
#include <QFileInfo>
#include <csignal>
#include "supervisorcore.h"
#include "singleinstance.h"
#include "../core/logsink.h"
#include "../core/loopwatchdog.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif

/**
 * PlayPlaySupervisor — headless automation host.
 *
 *   PlayPlaySupervisor --data-dir D:/PlayPlay/data --engine-path PlayPlayChannelEngine.exe
 *
 * Owns the schedule database, spawns one PlayPlayChannelEngine per channel,
 * executes the schedule, and serves REST (:7700), WebSocket (:7701) and the web
 * dashboard for the operator app and remote operators. See SupervisorCore.
 *
 * Windows service:
 *   PlayPlaySupervisor --install-service [--data-dir ...]   (as Administrator; then `sc start PlayPlaySupervisor`)
 *   PlayPlaySupervisor --uninstall-service
 * The service runs the same code path with --service; playout keeps running
 * without any logged-in user, and the operator app attaches over IPC.
 */

namespace {

struct Parsed {
    SupervisorCore::Options o;
    QString logDir;
    int healthPort = -1;                 // --health-port (watchdog /healthz); -1 = not given
    bool replaceHung = false, noGuard = false;   // PS-4b single-instance guard
    bool service = false, install = false, uninstall = false;
    QString serviceName = "PlayPlaySupervisor";
};

Parsed parse(const QStringList& args) {
    QCommandLineParser parser;
    parser.setApplicationDescription("PlayPlay Broadcast Playout Supervisor");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption ntpOpt(QStringList() << "n" << "ntp", "NTP server", "host", "pool.ntp.org");
    QCommandLineOption syncOpt(QStringList() << "s" << "sync-interval", "NTP sync interval (s)", "seconds", "300");
    QCommandLineOption ipcOpt(QStringList() << "i" << "ipc-name", "IPC server name", "name", "PlayPlaySupervisor");
    QCommandLineOption engineOpt(QStringList() << "e" << "engine-path", "Channel engine executable", "path", "");
    QCommandLineOption dataOpt(QStringList() << "d" << "data-dir", "Data directory (database, channel data)", "path", "./data");
    QCommandLineOption restOpt("rest-port", "REST port", "port", "7700");
    QCommandLineOption wsOpt("ws-port", "WebSocket port", "port", "7701");
    QCommandLineOption webOpt("web-root", "Static web dashboard directory", "path", "");
    QCommandLineOption mediaOpt("media-root", "Default media folder for scans and uploads", "path", "");
    QCommandLineOption logOpt("log-dir", "JSON-line log directory (default <data-dir>/logs; 'none' to disable)", "path", "");
    QCommandLineOption noAutoOpt("no-autostart", "Do not spawn channel engines at start");
    QCommandLineOption svcOpt("service", "Run as a Windows service (used by the Service Control Manager)");
    QCommandLineOption installOpt("install-service", "Install as a Windows service with the given options, then exit");
    QCommandLineOption uninstallOpt("uninstall-service", "Remove the Windows service, then exit");
    QCommandLineOption svcNameOpt("service-name", "Service name", "name", "PlayPlaySupervisor");
    QCommandLineOption healthOpt("health-port", "Watchdog /healthz port on 127.0.0.1 (default: setting watchdog.port, else 7709 with "
                                 "--rest-port 7700, else rest port + 2; 0 = off)", "port", "");
    QCommandLineOption replaceOpt("replace-hung", "If another Supervisor on this IPC name is hung (its /healthz says hang, or takes the "
                                  "connection and never answers), dump it, terminate it and take over (its engines are adopted). "
                                  "A holder whose /healthz cannot be reached is never replaced");
    QCommandLineOption noGuardOpt("no-instance-guard", "Do not take the one-Supervisor-per-IPC-name guard (emergency use only)");
    parser.addOption(replaceOpt); parser.addOption(noGuardOpt);
    parser.addOption(healthOpt);
    parser.addOption(ntpOpt); parser.addOption(syncOpt); parser.addOption(ipcOpt); parser.addOption(engineOpt);
    parser.addOption(dataOpt); parser.addOption(restOpt); parser.addOption(wsOpt); parser.addOption(webOpt);
    parser.addOption(mediaOpt); parser.addOption(logOpt); parser.addOption(noAutoOpt);
    parser.addOption(svcOpt); parser.addOption(installOpt); parser.addOption(uninstallOpt); parser.addOption(svcNameOpt);
    parser.process(args);

    Parsed p;
    p.o.ntpServer = parser.value(ntpOpt);
    p.o.ntpIntervalSec = parser.value(syncOpt).toInt();
    p.o.ipcName = parser.value(ipcOpt);
    p.o.enginePath = parser.value(engineOpt);
    p.o.dataDir = QDir(parser.value(dataOpt)).absolutePath();
    p.o.restPort = parser.value(restOpt).toInt();
    p.o.wsPort = parser.value(wsOpt).toInt();
#ifdef PLAYPLAY_DEV_TREE
    // A development build (cmake -DPLAYPLAY_DEV_TREE=ON, e.g. build-seg) never takes the live station's identity: on 2026-10-26
    // a build-seg app started without --supervisor-port launched a build-seg Supervisor on the live IPC name, port and data dir,
    // and its engine reaper killed the on-air engines. Live binaries come from the production build tree only.
    if (p.o.ipcName == QLatin1String("PlayPlaySupervisor") || p.o.restPort == 7700 || p.o.wsPort == 7701
        || p.o.dataDir.compare(QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/supervisor").absolutePath(), Qt::CaseInsensitive) == 0) {
        std::fprintf(stderr, "PlayPlaySupervisor (dev tree): refusing the live identity (ipc %s, REST %d, WS %d, data %s). Pass --ipc-name/--rest-port/--ws-port/--data-dir of a test instance.\n",
                     qPrintable(p.o.ipcName), p.o.restPort, p.o.wsPort, qPrintable(p.o.dataDir));
        std::exit(3);
    }
#endif
    p.o.webRoot = parser.value(webOpt);
    p.o.mediaRoot = parser.value(mediaOpt);
    p.o.autoStartChannels = !parser.isSet(noAutoOpt);
    p.logDir = parser.value(logOpt);
    if (p.logDir.isEmpty()) p.logDir = QDir(p.o.dataDir).filePath("logs");
    if (p.logDir == "none") p.logDir.clear();
    p.o.logDir = p.logDir;
    if (p.o.webRoot.isEmpty()) {
        const QString candidate = QDir(QCoreApplication::applicationDirPath()).filePath("web");
        if (QDir(candidate).exists()) p.o.webRoot = candidate;
    }
    p.service = parser.isSet(svcOpt); p.install = parser.isSet(installOpt); p.uninstall = parser.isSet(uninstallOpt);
    p.serviceName = parser.value(svcNameOpt);
    if (parser.isSet(healthOpt)) p.healthPort = qBound(0, parser.value(healthOpt).toInt(), 65535);
    p.replaceHung = parser.isSet(replaceOpt);
    p.noGuard = parser.isSet(noGuardOpt);
    return p;
}

int runCore(const Parsed& p, QCoreApplication& app) {
    if (!p.logDir.isEmpty()) installFileLogging(p.logDir, "supervisor");
    // STB-4: event-loop watchdog (JSON lines + dumps under the log dir; report only). /healthz answers even while this
    // loop is frozen. Port: --health-port, else 7709 for the station Supervisor (REST 7700), else REST port + 2 (the lane
    // test Supervisor on 7797 -> 7799), so a second Supervisor never takes the station's health port.
    const int healthPort = p.healthPort >= 0 ? p.healthPort : p.o.restPort == 7700 ? 7709 : qBound(0, p.o.restPort + 2, 65535);
    // PS-4b: one Supervisor per IPC name (named pipes accept several servers under one name, so two would split the
    // engines between them). Taken before any port or engine is touched; held until this function returns. A second
    // start asks the holder's /healthz (the port this command line would use) and exits unless --replace-hung finds
    // the holder hung.
    SingleInstance instance(p.o.ipcName);
    if (!p.noGuard) {
        supguard::Options go;
        go.ipcName = p.o.ipcName;
        go.healthPort = quint16(healthPort);
        go.replaceHung = p.replaceHung;
        go.dumpDir = p.logDir.isEmpty() ? QDir(p.o.dataDir).filePath("logs") : p.logDir;
        const supguard::Result g = supguard::acquireOrReplace(instance, go);
        if (!g.message.isEmpty()) {
            if (g.run) qWarning().noquote() << "[Supervisor]" << g.message;
            else qCritical().noquote() << "[Supervisor]" << g.message;
        }
        if (!g.run) return g.exitCode;
    }
    LoopWatchdogOptions wo;
    wo.name = "supervisor";
    wo.logDir = p.logDir;
    wo.healthPort = healthPort;
    LoopWatchdog* wd = LoopWatchdog::install(wo);
    SupervisorCore core;
    if (!core.start(p.o)) { qCritical() << "[Supervisor] failed to start"; return 1; }
    // Setting watchdog.port moves it, for the station Supervisor only (a test Supervisor runs on a copy of its database).
    const QString wdPort = core.store() ? core.store()->setting("watchdog.port") : QString();
    if (wd && p.healthPort < 0 && p.o.restPort == 7700 && !wdPort.trimmed().isEmpty()) {
        bool ok = false;
        const int port = wdPort.trimmed().toInt(&ok);
        QString err;
        if (ok && port >= 0 && port <= 65535 && port != wd->healthPort()) {
            if (wd->setHealthPort(quint16(port), &err)) qInfo().noquote() << "[Watchdog] /healthz" << (port ? "moved to 127.0.0.1:" + QString::number(port) : QString("off")) << "(setting watchdog.port)";
            else qWarning().noquote() << "[Watchdog] /healthz not started (setting watchdog.port):" << err;
        }
    }
    static QCoreApplication* s_app = &app;
    std::signal(SIGINT, [](int) { if (s_app) QMetaObject::invokeMethod(s_app, &QCoreApplication::quit, Qt::QueuedConnection); });
    std::signal(SIGTERM, [](int) { if (s_app) QMetaObject::invokeMethod(s_app, &QCoreApplication::quit, Qt::QueuedConnection); });
    const int rc = app.exec();
    core.stop();
    qInfo() << "[Supervisor] exited";
    return rc;
}

#ifdef Q_OS_WIN
// --- Windows service plumbing ---------------------------------------------------------------
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
QCoreApplication* g_serviceApp = nullptr;
int g_argc = 0; char** g_argv = nullptr;

void reportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

void WINAPI serviceCtrl(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        reportStatus(SERVICE_STOP_PENDING, NO_ERROR, 15000);
        if (g_serviceApp) QMetaObject::invokeMethod(g_serviceApp, &QCoreApplication::quit, Qt::QueuedConnection);
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    g_statusHandle = RegisterServiceCtrlHandlerW(L"PlayPlaySupervisor", serviceCtrl);
    reportStatus(SERVICE_START_PENDING, NO_ERROR, 10000);
    // The Qt application lives on the service thread; the SCM thread only delivers control codes.
    QCoreApplication app(g_argc, g_argv);
    app.setApplicationName("PlayPlaySupervisor");
    g_serviceApp = &app;
    QDir::setCurrent(QCoreApplication::applicationDirPath());
    const Parsed p = parse(app.arguments());
    reportStatus(SERVICE_RUNNING);
    const int rc = runCore(p, app);
    g_serviceApp = nullptr;
    reportStatus(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

int installService(const Parsed& p, const QStringList& rawArgs) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { qCritical() << "OpenSCManager failed (run as Administrator):" << GetLastError(); return 2; }
    // Re-use every option except the install switch itself, and add --service.
    QStringList svcArgs;
    for (int i = 1; i < rawArgs.size(); ++i) { if (rawArgs[i] == "--install-service") continue; svcArgs << rawArgs[i]; }
    svcArgs << "--service";
    QString cmd = "\"" + QDir::toNativeSeparators(QCoreApplication::applicationFilePath()) + "\"";
    for (const QString& a : svcArgs) cmd += " " + (a.contains(' ') ? "\"" + a + "\"" : a);
    const std::wstring name = p.serviceName.toStdWString(), wcmd = cmd.toStdWString();
    SC_HANDLE svc = CreateServiceW(scm, name.c_str(), L"PLAY PLAY Supervisor", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, wcmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) { const DWORD e = GetLastError(); CloseServiceHandle(scm); qCritical() << "CreateService failed:" << e << (e == ERROR_SERVICE_EXISTS ? "(already installed)" : ""); return 3; }
    SERVICE_DESCRIPTIONW desc; std::wstring d = L"PLAY PLAY broadcast automation host: schedule, channel engines, REST/WebSocket, web dashboard."; desc.lpDescription = d.data();
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SERVICE_FAILURE_ACTIONSW fa{}; SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 10000}, {SC_ACTION_RESTART, 30000}};
    fa.dwResetPeriod = 86400; fa.cActions = 3; fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    qInfo().noquote() << "Installed service" << p.serviceName << "->" << cmd << "\nStart it with: sc start" << p.serviceName;
    return 0;
}

int uninstallService(const Parsed& p) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) { qCritical() << "OpenSCManager failed (run as Administrator):" << GetLastError(); return 2; }
    const std::wstring name = p.serviceName.toStdWString();
    SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); qCritical() << "service not found:" << p.serviceName; return 3; }
    SERVICE_STATUS st{}; ControlService(svc, SERVICE_CONTROL_STOP, &st);
    const bool ok = DeleteService(svc);
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    qInfo() << (ok ? "Removed service" : "DeleteService failed") << p.serviceName;
    return ok ? 0 : 4;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
#ifdef Q_OS_WIN
    for (int i = 1; i < argc; ++i) {
        if (QByteArray(argv[i]) == "--service") {
            g_argc = argc; g_argv = argv;
            SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(L"PlayPlaySupervisor"), serviceMain }, { nullptr, nullptr } };
            if (!StartServiceCtrlDispatcherW(table)) { fprintf(stderr, "StartServiceCtrlDispatcher failed: %lu (not started by the SCM?)\n", GetLastError()); return 5; }
            return 0;
        }
    }
#endif
    QCoreApplication app(argc, argv);
    app.setApplicationName("PlayPlaySupervisor");
    app.setApplicationVersion("2.0.0");
    const Parsed p = parse(app.arguments());
#ifdef Q_OS_WIN
    if (p.install) return installService(p, app.arguments());
    if (p.uninstall) return uninstallService(p);
#endif
    return runCore(p, app);
}
