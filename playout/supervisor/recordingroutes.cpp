#include "laneext.h"
#include "restapiserver.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QProcess>
#include <QUuid>
#include <memory>

namespace {
void install(const laneext::Context& ctx) {
    if (!ctx.rest) return;
    auto jobs = std::make_shared<QHash<QString, QJsonObject>>();
    ctx.rest->registerRoute("GET", "/api/v1/recordings/jobs/{id}", [jobs](const QJsonObject&, const QHash<QString,QString>& q) {
        return jobs->contains(q["id"]) ? jobs->value(q["id"]) : QJsonObject{{"error", "No such recording export"}, {"status", 404}};
    });
    ctx.rest->registerRoute("POST", "/api/v1/recordings/recompose", [ctx, jobs](const QJsonObject& body, const QHash<QString,QString>&) {
        const QString session = body.value("session").toString(), output = body.value("output").toString();
        auto error = [](int status, const QString& message) { return QJsonObject{{"status", status}, {"error", message}}; };
        if (!QDir::isAbsolutePath(session) || !QFileInfo::exists(QDir(session).filePath("session.json"))) return error(422, "Choose a layered recording session directory on this node");
        if (!QDir::isAbsolutePath(output) || !output.endsWith(".mkv", Qt::CaseInsensitive) || !QFileInfo(output).dir().exists()) return error(422, "Choose an absolute new MKV path in an existing output folder");
        if (QFileInfo::exists(output)) return error(409, "Output exists; choose a new filename");
        for (const auto& j : *jobs) if (j.value("state").toString() == "running") return error(409, "A recording export is already running");
        const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath("PlayPlayRecompose.exe");
        if (!QFileInfo::exists(exe)) return error(503, "PlayPlayRecompose is missing from the build");
        const QString id = QUuid::createUuid().toString(QUuid::Id128), folder = QDir(ctx.dataDir).filePath("recording-jobs");
        if (!QDir().mkpath(folder)) return error(500, "Cannot create recording job directory");
        const QString edits = QDir(folder).filePath(id + ".json"); QSaveFile f(edits);
        const auto json = QJsonDocument(body.value("edits").toObject()).toJson(QJsonDocument::Compact);
        if (!f.open(QIODevice::WriteOnly) || f.write(json) != json.size() || !f.commit()) return error(500, "Cannot save recording edits");
        if (jobs->size() >= 32) jobs->clear();
        QJsonObject job{{"id", id}, {"state", "running"}, {"session", session}, {"output", output}}; jobs->insert(id, job);
        auto* process = new QProcess(ctx.parent); auto tail = std::make_shared<QByteArray>(); process->setProcessChannelMode(QProcess::MergedChannels);
        QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, tail]() { tail->append(process->readAllStandardOutput()); if (tail->size() > 16000) *tail = tail->right(16000); });
        QObject::connect(process, &QProcess::errorOccurred, process, [process, jobs, id](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) { (*jobs)[id]["state"] = "failed"; (*jobs)[id]["error"] = process->errorString(); process->deleteLater(); } });
        QObject::connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), process, [process, jobs, id, tail](int code, QProcess::ExitStatus status) {
            const bool ok = code == 0 && status == QProcess::NormalExit; (*jobs)[id]["state"] = ok ? "complete" : "failed";
            if (!ok) (*jobs)[id]["error"] = QString::fromUtf8(*tail).right(2000); process->deleteLater();
        });
        process->setProgram(exe); process->setArguments({"--session", session, "--output", output, "--edits", edits}); process->start();
        return job;
    });
}
static laneext::Registrar registration("scene-recordings", 115, install);
}
