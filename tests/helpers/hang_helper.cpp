// Test helper for STB-4 (tests/test_loopwatchdog.cpp): a process with the LoopWatchdog installed that freezes its own
// main thread on demand, so the test can watch /healthz, the JSON lines and the dumps from outside.
//
//   hang_helper --dir <dir> --name <name> [--beat ms] [--stall ms] [--hang ms] [--dump ms] [--abort ms] [--max-dumps n]
//               [--health]                      /healthz on any free port, written to <dir>/port.txt
//               [--block-at ms] [--block ms] [--blocks n] [--gap ms] [--busy] [--activity tag]
//               [--crash av] [--crash-at ms] [--prev-filter]
//               [--exit-after ms]
//
// Blocks start --block-at ms after the event loop starts; each writes <dir>/block-<i>.start right before it. --busy
// spins instead of sleeping. --crash av dereferences null on the main thread; --prev-filter installs an unhandled-
// exception filter first (it writes <dir>/prevfilter.txt), to check that the watchdog's filter chains to it.
// At --exit-after it writes <dir>/result.json: {"recovered":[{stalledMs,activity,worst}],"stalls","hangs","dumps","port"}.
// It never writes to stdout/stderr itself.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "playout/core/loopwatchdog.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <string>

namespace {

QString g_dir;

void writeFile(const QString& name, const QByteArray& data) {
    QFile f(QDir(g_dir).filePath(name));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(data);
}

#ifdef _WIN32
LONG WINAPI prevFilter(EXCEPTION_POINTERS*) {
    // Runs after the watchdog's dump (it chains to the filter that was there before). Plain Win32: we are crashing.
    const std::wstring p = QDir::toNativeSeparators(QDir(g_dir).filePath("prevfilter.txt")).toStdWString();
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD w = 0; WriteFile(h, "called", 6, &w, nullptr); CloseHandle(h); }
    return EXCEPTION_CONTINUE_SEARCH;
}
#define PP_NOINLINE __declspec(noinline)
#else
#define PP_NOINLINE __attribute__((noinline))
#endif

volatile int g_after = 0;

// The function the test expects to find in the hang stack sample. The store after Sleep keeps the call from becoming a
// tail jump (which would drop this frame from the stack).
PP_NOINLINE void blockMainThread(int ms, bool busy) {
    if (busy) {
        QElapsedTimer t; t.start();
        volatile quint64 x = 0;
        while (t.elapsed() < ms) x = x + 1;
        return;
    }
#ifdef _WIN32
    ::Sleep(DWORD(ms));
#endif
    g_after = g_after + 1;
}

PP_NOINLINE void crashNow() {
    volatile int* p = nullptr;
    *p = 42;
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // No WER dialog for the deliberate crash (the test would wait for it).
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    QCoreApplication app(argc, argv);
    app.setApplicationVersion("stb4-test");
    const QStringList a = app.arguments();
    auto val = [&a](const char* k, const QString& def = QString()) {
        const int i = a.indexOf(QString::fromLatin1(k));
        return i >= 0 && i + 1 < a.size() ? a[i + 1] : def;
    };
    auto num = [&](const char* k, int def) { bool ok = false; const int v = val(k).toInt(&ok); return ok ? v : def; };
    auto has = [&a](const char* k) { return a.contains(QString::fromLatin1(k)); };

    g_dir = val("--dir", QDir::tempPath());
    QDir().mkpath(g_dir);
#ifdef _WIN32
    if (has("--prev-filter")) SetUnhandledExceptionFilter(prevFilter);
#endif

    LoopWatchdogOptions o;
    o.name = val("--name", "hanghelper");
    o.logDir = g_dir;
    o.beatMs = num("--beat", 250);
    o.stallMs = num("--stall", 1000);
    o.hangMs = num("--hang", 5000);
    o.dumpAfterMs = num("--dump", 10000);
    o.abortAfterMs = num("--abort", 0);
    o.maxDumpsPerHour = num("--max-dumps", 3);
    o.healthPort = has("--health") ? -1 : 0;
    LoopWatchdog* wd = LoopWatchdog::install(o);
    if (!wd) return 2;
    writeFile("port.txt", QByteArray::number(wd->healthPort()));

    QJsonArray recovered;
    QObject::connect(wd, &LoopWatchdog::recovered, &app, [&recovered](const QString&, qint64 ms, const QString& act, const QString& worst) {
        recovered.append(QJsonObject{{"stalledMs", double(ms)}, {"activity", act}, {"worst", worst}});
    });

    const int blockAt = num("--block-at", -1), blockMs = num("--block", 3000), blocks = num("--blocks", 1), gap = num("--gap", 700);
    const bool busy = has("--busy");
    const QByteArray activity = val("--activity", "test-block").toUtf8();
    if (blockAt >= 0) {
        auto runBlock = std::make_shared<std::function<void(int)>>();
        *runBlock = [=, &app](int i) {
            writeFile(QStringLiteral("block-%1.start").arg(i), QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
            {
                WatchdogScope scope(activity);
                blockMainThread(blockMs, busy);
            }
            if (i + 1 < blocks) QTimer::singleShot(gap, &app, [runBlock, i]() { (*runBlock)(i + 1); });
        };
        QTimer::singleShot(blockAt, &app, [runBlock]() { (*runBlock)(0); });
    }
    if (val("--crash") == "av") QTimer::singleShot(num("--crash-at", 500), &app, []() { crashNow(); });

    QTimer::singleShot(num("--exit-after", 5000), &app, [&]() {
        const LoopWatchdog::Stats s = wd->stats();
        QJsonObject r{{"recovered", recovered}, {"stalls", double(s.stalls)}, {"hangs", double(s.hangs)},
                      {"dumps", double(s.dumps)}, {"port", int(s.healthPort)}, {"maxLagMs", double(s.maxLagMs)}};
        writeFile("result.json", QJsonDocument(r).toJson(QJsonDocument::Compact));
        app.quit();
    });
    return app.exec();
}
