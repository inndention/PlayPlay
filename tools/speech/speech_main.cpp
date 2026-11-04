// PlayPlaySpeech.exe: the in-house replacement for the spot generator's speech helpers (docs/plans/inhouse-deps.md §6.4).
// One sub-command per job, each printing one JSON object on the last stdout line (diagnostics on stderr):
//   tts      scripts/spot_tts.py's arguments and result: SAPI 5 through COM in this build (DEP-10); Kokoro (sherpa-onnx C API)
//            comes with DEP-11, and until then a run that needs it exits 64 so the caller runs the script instead
//   voices   the SAPI voices this machine has: desktop (SPCAT_VOICES) and OneCore tokens, with the default voice
//   env      which engines can run here and what `--engine auto` resolves to (takes tts's Kokoro path options)
//   version  {ok, tool, commands, qt}: lets the Supervisor see which sub-commands this build has
// Nothing is spawned: no PowerShell, no .NET, no Python, no Node. Exit codes: 0 ok, 2 failed, 64 unknown sub-command /
// option or an engine this build lacks.
#include "ppcli.h"
#include "sapitts.h"
#include "ttscli.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace {

int versionCommand(const QStringList&);
int voicesCommand(const QStringList&);
int envCommand(const QStringList&);

struct Command {
    const char* name;
    int (*run)(const QStringList& args);       // args after the sub-command name
    const char* help;
};

const Command kCommands[] = {
    {"tts", &ttscli::cli, "narration WAV(s), the scripts/spot_tts.py contract (SAPI in this build)"},
    {"voices", &voicesCommand, "installed SAPI voices (desktop + OneCore) as JSON"},
    {"env", &envCommand, "engines available here and what --engine auto picks"},
    {"version", &versionCommand, "tool name, sub-commands and Qt version as JSON"},
};

QJsonArray commandNames() {
    QJsonArray a;
    for (const Command& c : kCommands) a.append(QString::fromLatin1(c.name));
    return a;
}

QJsonObject voiceJson(const sapitts::VoiceInfo& v) {
    return QJsonObject{{"name", v.name}, {"description", v.description}, {"id", v.id}, {"category", v.category},
                       {"gender", v.gender}, {"age", v.age}, {"language", v.language}, {"vendor", v.vendor}};
}

int versionCommand(const QStringList& args) {
    if (!args.isEmpty()) return ppcli::unsupported(args.first());
    return ppcli::finish(QJsonObject{{"ok", true}, {"tool", "PlayPlaySpeech"}, {"commands", commandNames()}, {"qt", QString::fromLatin1(qVersion())}});
}

int voicesCommand(const QStringList& args) {
    if (!args.isEmpty()) return ppcli::unsupported(args.first());
    sapitts::ComScope com;
    if (!com.ok()) return ppcli::fail(com.error());
    QString err;
    const QList<sapitts::VoiceInfo> voices = sapitts::installedVoices(&err);
    if (voices.isEmpty()) return ppcli::fail(err.isEmpty() ? QStringLiteral("no SAPI voices installed") : err);
    QJsonArray list;
    for (const sapitts::VoiceInfo& v : voices) list.append(voiceJson(v));
    return ppcli::finish(QJsonObject{{"ok", true}, {"voices", list}, {"defaultVoice", sapitts::defaultVoiceName()}});
}

int envCommand(const QStringList& args) {
    const ppcli::Args a(args, {"--kokoro-dir=", "--kokoro-model=", "--sherpa-node="});
    if (!a.ok()) return a.report();
    ttscli::Options o;
    o.kokoroDir = a.value("--kokoro-dir"); o.kokoroModel = a.value("--kokoro-model"); o.sherpaNode = a.value("--sherpa-node");
    QJsonObject sapi;
    {
        sapitts::ComScope com;
        QString err;
        const QList<sapitts::VoiceInfo> voices = com.ok() ? sapitts::installedVoices(&err) : QList<sapitts::VoiceInfo>();
        QJsonArray desktop, onecore;
        for (const sapitts::VoiceInfo& v : voices) (v.category == QLatin1String("onecore") ? onecore : desktop).append(v.name);
        sapi = QJsonObject{{"available", !voices.isEmpty()}, {"desktop", desktop}, {"onecore", onecore},
                           {"defaultVoice", com.ok() ? sapitts::defaultVoiceName() : QString()}};
        if (voices.isEmpty()) sapi["error"] = com.ok() ? err : com.error();
    }
    const ttscli::KokoroAssets k = ttscli::findKokoro(o);
    QStringList order; QJsonObject refusal;
    o.engine = QStringLiteral("auto");
    const bool autoOk = ttscli::planEngines(o, &order, &refusal);
    return ppcli::finish(QJsonObject{{"ok", true}, {"tool", "PlayPlaySpeech"}, {"commands", commandNames()},
                                     {"engines", QJsonObject{{"sapi", sapi}, {"kokoro", k.toJson()}}},
                                     {"auto", autoOk ? QJsonValue(order.last()) : QJsonValue("unsupported")},
                                     {"autoReason", autoOk ? QString() : refusal.value("reason").toString()}});
}

QByteArray usage() {
    QByteArray u = "usage: PlayPlaySpeech <command> [options]   (<command> --help for its options)\n\ncommands:\n";
    for (const Command& c : kCommands) u += "  " + QByteArray(c.name).leftJustified(9) + c.help + "\n";
    return u;
}

}   // namespace

int main(int argc, char** argv) {
    ppcli::initStdio();
    // SAPI runs the voice engine DLLs (OneCore, third-party) in this process: a crash in one must end it at once, so the
    // caller sees the failure (and can fall back), not a process parked behind Windows Error Reporting until its timeout.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const char* sub = argc > 1 ? argv[1] : "";
    const Command* cmd = nullptr;
    for (const Command& c : kCommands) if (std::strcmp(c.name, sub) == 0) cmd = &c;
    if (!cmd) {
        if (!std::strcmp(sub, "--help") || !std::strcmp(sub, "-h") || !std::strcmp(sub, "help")) {
            const QByteArray u = usage(); fwrite(u.constData(), 1, size_t(u.size()), stdout); fflush(stdout);
            return ppcli::ExitOk;
        }
        const QByteArray u = usage(); fwrite(u.constData(), 1, size_t(u.size()), stderr);
        if (!*sub) return ppcli::fail("no command given");
        return ppcli::unsupported(QString::fromLocal8Bit(sub));
    }
    // QCoreApplication for the UTF-16 command line (non-ASCII paths and voice names); no platform plugin, no window, so
    // the tool runs the same from a service account as from a desktop session.
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();
    args.erase(args.begin(), args.begin() + std::min<qsizetype>(2, args.size()));
    return cmd->run(args);
}
