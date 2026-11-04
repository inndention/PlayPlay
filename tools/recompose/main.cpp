#include "scenerecorder.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QFile>
#include <QTextStream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("PlayPlayRecompose");
    QCommandLineParser p; p.setApplicationDescription("Recompose a PLAY PLAY layered recording to MKV. Optional edits: {omitLayers:[30], layers:{\"15\":{rect:[x,y,w,h],opacity:0.5,hidden:false}}}");
    p.addHelpOption(); p.addOptions({{{"s", "session"}, "Session directory", "directory"}, {{"o", "output"}, "New output MKV", "file"}, {{"e", "edits"}, "Optional JSON edits", "file"}}); p.process(app);
    if (!p.isSet("session") || !p.isSet("output")) p.showHelp(2);
    QJsonObject edits;
    if (p.isSet("edits")) { QFile f(p.value("edits")); QJsonParseError err;
        if (!f.open(QIODevice::ReadOnly)) { QTextStream(stderr) << "Cannot read edits\n"; return 2; }
        const auto doc = QJsonDocument::fromJson(f.readAll(), &err); if (err.error || !doc.isObject()) { QTextStream(stderr) << "Invalid edits JSON\n"; return 2; } edits = doc.object(); }
    QString error; if (!SceneRecorder::recompose(p.value("session"), p.value("output"), edits, &error)) { QTextStream(stderr) << error << '\n'; return 1; }
    QTextStream(stdout) << "Exported " << p.value("output") << '\n'; return 0;
}
