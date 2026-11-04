// fake_supervisor: a stand-in Supervisor process for SupervisorLink tests (perf-supervisor PS-3; PS-4b may reuse it).
//
//   fake_supervisor --name <ipc> [--adopt] [--exit-after-ms N] [--max-life-ms N]
//
// Listens on <ipc> with a real IpcTransport server and prints one line per event on stdout (flushed):
//   LISTENING <pid>                         server up
//   CONNECT <clientId> <client pid>         a client connected
//   MSG <type> <channelId> <client pid> <compact JSON payload>
//   DISCONNECT <clientId>
//   ADOPT <channelId> <client pid>          --adopt: a HELLO was answered with MSG_SUP_ALIVE addressed to its channel
// --adopt: every engine that sends MSG_ENGINE_HELLO is "bound" and gets MSG_SUP_ALIVE {seq, loopLagMs, pid} with its
//          channelId at once and then once a second, as an adopting Supervisor does (PS-4b).
// --exit-after-ms: dies abruptly (std::_Exit, no clean disconnect) after N ms, like a crashed Supervisor: the tests use
//          this process as the engine's parent (--supervisor-pid).
// Safety: never listens on the live name PlayPlaySupervisor; exits after --max-life-ms (default 60 s).
#include <QCoreApplication>
#include <QHash>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include "playout/ui_ipc/ipctransport.h"

namespace {
void out(const QString& line) {
    const QByteArray b = line.toUtf8();
    std::fwrite(b.constData(), 1, size_t(b.size()), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QString name;
    bool adopt = false;
    int exitAfterMs = 0, maxLifeMs = 60000;
    const QStringList a = app.arguments();
    for (int i = 1; i < a.size(); ++i) {
        if (a[i] == "--name" && i + 1 < a.size()) name = a[++i];
        else if (a[i] == "--adopt") adopt = true;
        else if (a[i] == "--exit-after-ms" && i + 1 < a.size()) exitAfterMs = a[++i].toInt();
        else if (a[i] == "--max-life-ms" && i + 1 < a.size()) maxLifeMs = a[++i].toInt();
    }
    if (name.isEmpty() || name.compare("PlayPlaySupervisor", Qt::CaseInsensitive) == 0) return 2;

    IpcTransport server(IpcTransport::ServerRole);
    QHash<QString, QString> bound;   // clientId -> channel (adopted engines)
    qint64 seq = 0;
    const qint64 myPid = QCoreApplication::applicationPid();
    auto supAlive = [&](const QString& clientId, const QString& ch) {
        server.sendTo(clientId, IpcProtocol::makeSupAlive(++seq, 0.0, myPid, ch), IpcTransport::Normal);
    };
    QObject::connect(&server, &IpcTransport::clientConnected, &app, [&](const QString& id) {
        out(QString("CONNECT %1 %2").arg(id).arg(server.peerProcessId(id)));
    });
    QObject::connect(&server, &IpcTransport::clientDisconnected, &app, [&](const QString& id) {
        bound.remove(id);
        out(QString("DISCONNECT %1").arg(id));
    });
    QObject::connect(&server, &IpcTransport::messageReceived, &app, [&](const IpcProtocol::Message& m, const QString& id) {
        out(QString("MSG %1 %2 %3 %4").arg(int(m.type)).arg(m.channelId.isEmpty() ? QStringLiteral("-") : m.channelId)
                .arg(server.peerProcessId(id)).arg(QString::fromUtf8(QJsonDocument(m.payload).toJson(QJsonDocument::Compact))));
        if (adopt && m.type == IpcProtocol::MSG_ENGINE_HELLO && !m.channelId.isEmpty()) {
            bound[id] = m.channelId;
            supAlive(id, m.channelId);
            out(QString("ADOPT %1 %2").arg(m.channelId).arg(server.peerProcessId(id)));
        }
    });
    QTimer alive;
    QObject::connect(&alive, &QTimer::timeout, &app, [&]() {
        for (auto it = bound.constBegin(); it != bound.constEnd(); ++it) supAlive(it.key(), it.value());
    });
    alive.start(1000);

    if (!server.startServer(name)) return 3;
    out(QString("LISTENING %1").arg(myPid));
    if (exitAfterMs > 0) QTimer::singleShot(exitAfterMs, &app, []() { std::fflush(stdout); std::_Exit(9); });
    if (maxLifeMs > 0) QTimer::singleShot(maxLifeMs, &app, []() { QCoreApplication::exit(5); });
    return app.exec();
}
