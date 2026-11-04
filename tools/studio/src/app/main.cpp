// PlayPlayStudio.exe: the Studio engine process (docs/plans/starmedia-studio.md). A separate process from the on-air
// Supervisor: it never binds a port another PlayPlay / StarMedia service owns, never writes the Supervisor's data and
// (later) talks to it only over REST.
//
//   PlayPlayStudio --version
//   PlayPlayStudio [--port 7760] [--ws-port 7761|off] [--bind 127.0.0.1] [--data <dir>]
//   PlayPlayStudio --print-token [--data <dir>]      (prints the API token, creating <data>/studio-token if missing)
//
// Once listening it prints one line "LISTENING http://<host>:<port> ws=ws://<host>:<wsport> data=<dir>" (ws=off without
// an event stream) to stdout, so scripts and tests that start it with --port 0 learn both ports. With --port 0 and no
// --ws-port the event stream takes any free port too, so a test never binds the default 7761.
#include "app/studioservice.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>

// Ctrl+C / Ctrl+Break quit the event loop instead of killing the process, so later slices (DbWriter, artifact moves)
// can finish their writes. Runs on a system thread: only post to the loop.
static BOOL WINAPI consoleHandler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    if (QCoreApplication* app = QCoreApplication::instance())
        QMetaObject::invokeMethod(app, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
    return TRUE;
}
#endif

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    studio::setAppIdentity();   // organization "PlayPlayStudio": AppDataLocation / QSettings stay out of %APPDATA%/PlayPlay

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("PlayPlayStudio: the StarMedia engine service (runs apart from the on-air Supervisor)."));
    const QCommandLineOption helpOpt = cli.addHelpOption();
    const QCommandLineOption versionOpt = cli.addVersionOption();
    const QCommandLineOption portOpt(QStringLiteral("port"), QStringLiteral("REST port (default 7760; 0 = any free port)."),
                                     QStringLiteral("port"), QString::number(studio::kDefaultRestPort));
    const QCommandLineOption bindOpt(QStringLiteral("bind"), QStringLiteral("Listen address (loopback only for now)."),
                                     QStringLiteral("address"), QStringLiteral("127.0.0.1"));
    const QCommandLineOption dataOpt(QStringLiteral("data"),
                                     QStringLiteral("Data dir (default: env PLAYPLAY_STUDIO_DATA, else %LOCALAPPDATA%\\PlayPlayStudio)."),
                                     QStringLiteral("dir"));
    const QCommandLineOption wsPortOpt(QStringLiteral("ws-port"),
                                       QStringLiteral("Event stream (WebSocket) port (default 7761; 0 = any free port; off = none)."),
                                       QStringLiteral("port"));
    const QCommandLineOption printTokenOpt(QStringLiteral("print-token"),
                                           QStringLiteral("Print the API token (creating <data>/studio-token if missing) and exit."));
    cli.addOptions({portOpt, wsPortOpt, bindOpt, dataOpt, printTokenOpt});
    // parse(), not process(): without a console window QCommandLineParser shows --help / --version / errors in a
    // modal message box, which would hang a headless service. Everything goes to stdout / stderr instead.
    if (!cli.parse(QCoreApplication::arguments())) {
        std::fprintf(stderr, "PlayPlayStudio: %s\n\n%s", qPrintable(cli.errorText()), qPrintable(cli.helpText()));
        return 2;
    }
    if (cli.isSet(versionOpt)) {
        std::printf("PlayPlayStudio %s\n", qPrintable(studio::version()));
        return 0;
    }
    if (cli.isSet(helpOpt) || cli.isSet(QStringLiteral("help-all"))) {
        std::fputs(qPrintable(cli.helpText()), stdout);
        return 0;
    }

    bool ok = false;
    const uint port = cli.value(portOpt).toUInt(&ok);
    if (!ok || port > 65535) {
        std::fprintf(stderr, "PlayPlayStudio: bad --port '%s'\n", qPrintable(cli.value(portOpt)));
        return 2;
    }
    studio::ServiceOptions opt;
    opt.port = quint16(port);
    if (cli.isSet(wsPortOpt)) {
        const QString w = cli.value(wsPortOpt).trimmed();
        const int ws = w.compare(QStringLiteral("off"), Qt::CaseInsensitive) == 0 ? -1 : w.toInt(&ok);
        if ((ws != -1 && !ok) || ws < -1 || ws > 65535) {
            std::fprintf(stderr, "PlayPlayStudio: bad --ws-port '%s'\n", qPrintable(w));
            return 2;
        }
        opt.wsPort = ws;
    } else if (port == 0) {
        opt.wsPort = 0;                    // an ephemeral REST port (tests) never takes the default WS port
    }
    const QString bind = cli.value(bindOpt).trimmed();
    opt.bind = bind.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0 ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(bind);
    if (opt.bind.isNull()) {
        std::fprintf(stderr, "PlayPlayStudio: bad --bind '%s'\n", qPrintable(bind));
        return 2;
    }
    opt.dataDir = cli.value(dataOpt);

    if (cli.isSet(printTokenOpt)) {
        // The same guards as a start (never inside the Supervisor's data), without binding anything.
        studio::ServiceOptions check = opt;
        check.port = 0;
        check.wsPort = -1;
        if (const QString why = studio::checkOptions(check); !why.isEmpty()) {
            std::fprintf(stderr, "PlayPlayStudio: %s\n", qPrintable(why));
            return 1;
        }
        const QString dir = opt.dataDir.isEmpty() ? studio::defaultDataDir() : QDir::cleanPath(QDir().absoluteFilePath(opt.dataDir));
        studio::net::Auth auth;
        QString err;
        const QString token = QDir().mkpath(dir) && auth.load(dir, &err) ? studio::net::Auth::readTokenFile(auth.tokenPath(), &err) : QString();
        if (token.isEmpty()) {
            std::fprintf(stderr, "PlayPlayStudio: %s\n", qPrintable(err.isEmpty() ? QStringLiteral("cannot create the data dir") : err));
            return 1;
        }
        std::printf("%s\n", qPrintable(token));   // asked for explicitly; never logged
        return 0;
    }

    studio::StudioService service;
    QString error;
    if (!service.start(opt, &error)) {
        std::fprintf(stderr, "PlayPlayStudio: %s\n", qPrintable(error));
        return 1;
    }
    QString host = opt.bind.toString();
    if (opt.bind.protocol() == QAbstractSocket::IPv6Protocol) host = QLatin1Char('[') + host + QLatin1Char(']');
    const QString ws = service.wsPort() ? QStringLiteral("ws://%1:%2").arg(host).arg(service.wsPort()) : QStringLiteral("off");
    std::printf("LISTENING http://%s:%u ws=%s data=%s\n", qPrintable(host), unsigned(service.port()), qPrintable(ws),
                qPrintable(QDir::toNativeSeparators(service.dataDir())));
    std::fflush(stdout);
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#endif
    const int rc = app.exec();
    service.stop();
    return rc;
}
