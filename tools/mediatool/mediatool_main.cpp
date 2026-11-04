// PlayPlayMediaTool.exe: the in-house replacement for the ffprobe / ffmpeg spawns of the media path
// (docs/plans/inhouse-deps.md §6.2), on the FFmpeg 9 libraries the engines link. Sub-commands:
//   probe    ffprobe's two forms PlayPlay uses, byte for byte (probecli.h)                       (DEP-03)
//   thumb    the ingest's ffmpeg thumbnail arguments (thumbnailer.h)                              (DEP-03)
//   --serve  JSON-lines server for probe / duration / thumb, 3 workers by default (serve.h)       (DEP-03)
//   version  {ok, tool, commands, ffmpeg, libav}: lets the Supervisor see what this build supports
//   parse    the ffmpeg-argument subset parser as a diagnostic: {ok, command} for a command line in the subset,
//            else the 64 / 2 line the ffmpeg mode would give (argparse.h)                          (DEP-05a)
// With no sub-command the arguments are ffmpeg's: the thumbnail form is taken as `thumb`; any other command line goes
// through the subset parser (an argument outside the subset exits 64 so the caller runs ffmpeg.exe instead, one ffmpeg
// would refuse exits 2) and then the graph runner (graphrunner.h): spot composition and render-farm jobs in-process on
// libavformat / libavfilter / libavcodec, -progress pipe:1 blocks on stdout for the Supervisor.           (DEP-05b)
// Exit codes: probe = ffprobe's (0, 1 when the input cannot be probed); otherwise 0 ok, 2 failed; 64 unsupported
// argument / sub-command, with {"ok": false, "error": "unsupported: ...", "unsupported": true} as the last stdout line.
#include "argparse.h"
#include "avcaps.h"
#include "graphrunner.h"
#include "ppcli.h"
#include "probecli.h"
#include "serve.h"
#include "thumbnailer.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#  include <cstdlib>
#endif

namespace {

int versionCommand(const QStringList& args) {
    if (!args.isEmpty()) return ppcli::unsupported(args.first());
    return ppcli::finish(QJsonObject{{"ok", true}, {"tool", "PlayPlayMediaTool"},
                                     {"commands", QJsonArray{"probe", "thumb", "serve", "version", "parse"}},
                                     {"serveOps", QJsonArray{"probe", "duration", "thumb", "version", "ping", "quit"}},
                                     {"ffmpeg", avkit::versionInfo()}, {"libav", avkit::versions()}});
}

const char kUsage[] =
    "usage: PlayPlayMediaTool <command> [options]\n\n"
    "commands:\n"
    "  probe   [-v quiet] -print_format json -show_format -show_streams <file>\n"
    "          [-v error] -show_entries format=duration -of default=nw=1:nk=1 <file>\n"
    "  thumb   -y [-v quiet] [-ss <t>] -i <in> -frames:v 1 -vf scale=320:-2 -q:v 4 <out>.jpg\n"
    "  --serve [--workers 3] [--timeout-ms 60000] [--idle-exit-ms 0] [--threads 2] [-v <level>]\n"
    "          JSON lines: {\"id\",\"op\":\"probe|duration|thumb|version|ping|quit\",\"path\",\"out\",\"at\",\"width\",\"timeoutMs\"}\n"
    "  version\n"
    "  parse   <ffmpeg arguments>   checks a command line against the graph runner's subset: {ok, command} / 64 / 2\n"
    "  <ffmpeg arguments>   the thumbnail form above, without the word thumb; other command lines exit 64 until DEP-05b\n";

}   // namespace

int main(int argc, char** argv) {
    ppcli::initStdio();
#ifdef _WIN32
    // A crash must end the process (the --serve client restarts it), not park it behind a Windows error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    // QCoreApplication for the UTF-16 command line (non-ASCII paths, the same UTF-8 ffprobe prints as "filename");
    // no plugins or platform are loaded.
    QCoreApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments();
    if (!args.isEmpty()) args.removeFirst();
    const QString sub = args.value(0);
    const QStringList rest = args.mid(1);

    if (sub == "probe") return mediatool::probeCli(rest);
    if (sub == "thumb") return mediatool::thumbCli(rest);
    if (sub == "--serve" || sub == "serve") return mediatool::serveCli(rest);
    if (sub == "version") return versionCommand(rest);
    if (sub == "parse") return mediatool::graphParseCli(rest);
    if (sub == "--help" || sub == "-h" || sub == "help") { std::fputs(kUsage, stdout); std::fflush(stdout); return ppcli::ExitOk; }
    if (sub.isEmpty()) { std::fputs(kUsage, stderr); return ppcli::fail("no command given"); }
    if (sub.startsWith('-')) {
        // ffmpeg-style: the thumbnail form runs as thumb; everything else goes to the graph runner, which exits 64 for a
        // command line outside its subset (the Supervisor runs ffmpeg.exe) and 2 for one ffmpeg would refuse as well.
        const mediatool::ThumbCommand c = mediatool::parseThumbArgs(args);
        if (c.unsupported.isEmpty()) return mediatool::thumbCli(args);
        return mediatool::graphRunCli(args);
    }
    std::fputs(kUsage, stderr);
    return ppcli::unsupported(sub);
}
