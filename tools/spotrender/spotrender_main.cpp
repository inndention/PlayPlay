// PlayPlaySpotRender.exe: the in-house replacement for the spot generator's Python helpers (docs/plans/inhouse-deps.md
// §6.3). One sub-command per script, each taking that script's arguments and printing its one-line JSON result:
//   music    scripts/spot_music.py   MIDI composer + synthesizer -> 48 kHz stereo WAV (+ .mid)          (DEP-07)
//   card     scripts/spot_card.py     --spec/--out/--bed furniture + gradient bed, or --scenes            (DEP-08b)
//   scenes   scripts/spot_card.py --scenes <file>: Headline News furniture + 2880x1620 photo beds          (DEP-08b)
//   version  {ok, tool, commands, qt}: lets the Supervisor see which sub-commands this build has
// card / scenes need QPainter text, so their entries set `gui` and get a QGuiApplication on the native "windows"
// platform (as engine_main.cpp does), never a window. Animated (weather_cards.py) scenes come with DEP-09; until then
// a scenes.json holding one answers 64 before anything is written.
// Exit codes: 0 ok, 2 failed, 64 unknown sub-command / option (the Supervisor then runs the Python script instead).
#include "musicsynth.h"
#include "ppcli.h"
#include "spotcard.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef Q_OS_WIN
#include <stdlib.h>
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

int versionCommand(const QStringList&);

struct Command {
    const char* name;
    bool gui;                                  // needs QGuiApplication (QPainter text); false = QCoreApplication
    int (*run)(const QStringList& args);       // args after the sub-command name
    const char* help;
};

const Command kCommands[] = {
    {"music", false, &music::cli, "MIDI-style music bed (scripts/spot_music.py)"},
    {"card", true, &spotcard::cardCli, "spot furniture PNG + gradient bed, or --scenes (scripts/spot_card.py)"},
    {"scenes", true, &spotcard::scenesCli, "Headline News scenes: furniture + photo beds (spot_card.py --scenes)"},
    {"version", false, &versionCommand, "tool name, sub-commands and Qt version as JSON"},
};

int versionCommand(const QStringList& args) {
    if (!args.isEmpty()) return ppcli::unsupported(args.first());
    QJsonArray cmds;
    for (const Command& c : kCommands) cmds.append(QString::fromLatin1(c.name));
    return ppcli::finish(QJsonObject{{"ok", true}, {"tool", "PlayPlaySpotRender"}, {"commands", cmds}, {"qt", QString::fromLatin1(qVersion())}});
}

QByteArray usage() {
    QByteArray u = "usage: PlayPlaySpotRender <command> [options]   (<command> --help for its options)\n\ncommands:\n";
    for (const Command& c : kCommands) u += "  " + QByteArray(c.name).leftJustified(9) + c.help + "\n";
    return u;
}

}   // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    // a crash (a broken picture in a decoder, a font driver) ends the process instead of parking it behind WER
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    ppcli::initStdio();
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
    // QCoreApplication for the UTF-16 command line (non-ASCII paths); no platform plugin is loaded, so `music` also runs
    // where none is deployed (a service, a stripped package). The card commands need fonts: QGuiApplication.
    auto run = [&]() {
        QStringList args = QCoreApplication::arguments();
        args.erase(args.begin(), args.begin() + std::min<qsizetype>(2, args.size()));
        return cmd->run(args);
    };
    if (cmd->gui) {
        QGuiApplication app(argc, argv);
        return run();
    }
    QCoreApplication app(argc, argv);
    return run();
}
