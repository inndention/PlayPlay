// Spawned by test_asynclog: floods the file log sink from N threads, then reports timings and counters in
// <logDir>/result.json. Its stdout/stderr may be a pipe nobody reads, so it never writes to them itself.
//
//   logflood_helper <logDir> <lines> [threads]
#include "playout/core/asynclog.h"
#include "playout/core/logsink.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <thread>
#include <vector>

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.size() < 3) return 2;
    const QString dir = a.at(1);
    const int lines = a.at(2).toInt();
    const int threads = a.size() > 3 ? qMax(1, a.at(3).toInt()) : 1;
    installFileLogging(dir, "flood");

    QElapsedTimer t;
    t.start();
    std::vector<std::thread> pool;
    for (int ti = 0; ti < threads; ++ti) {
        pool.emplace_back([=] {
            const int n = lines / threads + (ti < lines % threads ? 1 : 0);
            for (int i = 0; i < n; ++i) qInfo("flood t=%d i=%d", ti, i);
        });
    }
    for (std::thread& th : pool) th.join();
    const qint64 logMs = t.elapsed();
    const bool drained = asynclog::flush(5000);
    qInfo("flood-end");                        // gets in after the drain and carries any drop count still pending
    asynclog::flush(5000);

    const asynclog::Stats st = asynclog::stats();
    const QJsonObject r{{"logMs", logMs}, {"totalMs", t.elapsed()}, {"drained", drained},
                        {"written", qint64(st.written)}, {"dropped", qint64(st.dropped)},
                        {"consoleWritten", qint64(st.consoleWritten)}, {"consoleDropped", qint64(st.consoleDropped)},
                        {"consoleEcho", st.consoleEcho}};
    QFile f(dir + "/result.json");
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(r).toJson());
    return 0;                                  // exit runs asynclog's atexit drain; a blocked echo thread is left behind
}
