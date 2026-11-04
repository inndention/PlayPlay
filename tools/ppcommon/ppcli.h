#ifndef PPCLI_H
#define PPCLI_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

/**
 * ppcli: the command-line contract every in-house tool (PlayPlaySpeech, PlayPlaySpotRender, PlayPlayMediaTool,
 * PlayPlayMux) shares with the Python/Node scripts it replaces, so the Supervisor only changes which program it starts
 * (docs/plans/inhouse-deps.md §5):
 *  - stdout carries exactly one JSON object, on the LAST line: {"ok": true, ...} or {"ok": false, "error": "..."}.
 *    Call sites parse `out.trimmed().split('\n').last()` (spotgenerator.cpp), so nothing may follow it.
 *  - progress and diagnostics go to stderr; progress as "PROGRESS <done> <total>" (transcriptionmanager.cpp regex).
 *  - exit code 0 = ok, 2 = failed (also argparse's usage-error code), 64 = an argument / op this tool does not
 *    implement: {"ok": false, "error": "unsupported: <arg>", "unsupported": true}; the caller then falls back to the old
 *    program instead of getting a silently different result.
 *  - `--serve`: a persistent JSON-lines server for tools with expensive start-up (ServeLoop below).
 * QtCore only; no QCoreApplication needed.
 */
namespace ppcli {

enum Exit : int { ExitOk = 0, ExitFailed = 2, ExitUnsupported = 64 };

/// Binary stdout (a JSON line ends in "\n", not "\r\n") and Qt logging forced to stderr. Call first in main().
void initStdio();

/// One diagnostic line on stderr; thread-safe, flushed (the parent reads it live).
void log(const QString& msg);
/// "PROGRESS <done> <total>\n" with `decimals` digits (transcribe.py prints %.1f).
QByteArray progressLine(double done, double total, int decimals = 1);
void progress(double done, double total, int decimals = 1);

/// Compact JSON + "\n": never more than one line (QJsonDocument escapes newlines inside strings).
QByteArray resultLine(const QJsonObject& r);
/// ok:true -> 0, unsupported:true -> 64, anything else -> 2.
int exitCodeFor(const QJsonObject& r);
/// Prints the result as the last stdout line (flushed) and returns exitCodeFor(r). `return ppcli::finish(r);`
int finish(const QJsonObject& r);
/// {"ok": false, "error": error, ...extra} -> 2.
int fail(const QString& error, const QJsonObject& extra = {});
/// {"ok": false, "error": "unsupported: <what>", "unsupported": true, ...extra} -> 64.
int unsupported(const QString& what, const QJsonObject& extra = {});
QJsonObject failure(const QString& error, const QJsonObject& extra = {});
QJsonObject unsupportedResult(const QString& what, const QJsonObject& extra = {});

/// Caller side: the JSON object on the last non-empty stdout line (CR/LF tolerant). Empty object + err when none.
QJsonObject parseResult(const QByteArray& stdoutBytes, QString* err = nullptr);
/// Caller side: parses one stderr "PROGRESS <done> <total>" line.
bool parseProgress(const QByteArray& line, double* done, double* total = nullptr);
/// Caller side: the tool refused an argument it does not implement -> run the old program instead.
bool isUnsupported(int exitCode, const QJsonObject& r);

/**
 * Args: argparse-style options as the replaced scripts take them. `spec` lists the known options: "--name=" takes a
 * value ("--name v" or "--name=v"), "--name" is a flag. An option not in the spec -> unsupported (exit 64); a missing
 * value -> error (exit 2). "--" ends options. Repeated value options keep every value (values()).
 */
class Args {
public:
    Args(const QStringList& args, const QStringList& spec, bool allowPositional = false);
    bool ok() const { return m_error.isEmpty(); }
    QString error() const { return m_error; }
    bool unknownOption() const { return m_unsupported; }
    QString unknownName() const { return m_unknown; }
    bool has(const QString& name) const { return m_values.contains(name); }
    QString value(const QString& name, const QString& def = {}) const;
    QStringList values(const QString& name) const { return m_values.value(name); }
    double number(const QString& name, double def, bool* ok = nullptr) const;
    int integer(const QString& name, int def, bool* ok = nullptr) const;
    QStringList positional() const { return m_positional; }
    /// Prints the error result and returns its exit code (64 for an unknown option, else 2).
    int report() const;
private:
    QHash<QString, QStringList> m_values;    // flags map to an empty list
    QStringList m_positional;
    QString m_error;
    QString m_unknown;
    bool m_unsupported = false;
};

class ServeLoop;
struct ServeJob;
struct ServeState;

/// What a --serve handler sees besides its request.
class ServeContext {
public:
    QJsonValue id() const;
    QString op() const;
    /// True once the request timed out (its reply is already sent) or the server is shutting down: stop early.
    bool cancelled() const;
    /// The flag cancelled() reads, for code that polls an atomic itself (avkit::ProbeOptions::cancel, an
    /// AVIOInterruptCB). Valid while the handler runs.
    const std::atomic<bool>* cancelFlag() const;
    qint64 elapsedMs() const;
    /// An intermediate line {"id": <id>, "event": ..., ...ev} (dropped once the request timed out).
    void event(const QJsonObject& ev);
    /// event {"event": "progress", "done": d, "total": t}
    void progress(double done, double total);
private:
    friend class ServeLoop;
    friend struct ServeState;
    ServeContext(std::shared_ptr<ServeJob> j, std::shared_ptr<ServeState> s) : m_job(std::move(j)), m_state(std::move(s)) {}
    std::shared_ptr<ServeJob> m_job;
    std::shared_ptr<ServeState> m_state;
};

/**
 * ServeLoop: JSON-lines requests {"id", "op", ..., "timeoutMs"?} on stdin, one JSON-lines reply per request on stdout
 * {"id": <same>, "ok": ..., ...}, handled by `workers` threads (replies come in completion order; match them by id).
 *  - Built-in ops: "ping" -> {ok, pong, pid, busy}; "quit" / "shutdown" -> {ok, bye}, then exit 0 once in-flight work
 *    is done. Unknown ops are the handler's call (unsupportedOp()).
 *  - Per-request timeout (from the moment a worker starts it): the reply {"ok": false, "error": "timeout ...",
 *    "timeout": true} is sent at the deadline, the handler's ctx.cancelled() turns true and its late result is dropped.
 *    A handler still running hangGraceMs after its timeout means the process is wedged (a stuck decoder, a COM call
 *    that never returns): queued requests are failed and the loop exits 2 (immediately, with hardExitOnHang) so the
 *    client restarts the tool.
 *  - stdin EOF or quit: in-flight requests finish, then exit 0. idleExitMs > 0: exit 0 after that long without work.
 *  - Broken stdout (the client went away): exit 2.
 * Handlers must not write to stdout themselves (use ctx.event / ppcli::log).
 */
class ServeLoop {
public:
    struct Options {
        int workers = 1;
        int defaultTimeoutMs = 60000;
        int maxTimeoutMs = 3600000;
        int hangGraceMs = 10000;
        int idleExitMs = 0;
        int maxPending = 256;                 // queued + running; more -> {"ok": false, "error": "busy"}
        int maxLineBytes = 16 * 1024 * 1024;
        bool hardExitOnHang = true;           // _Exit(2) instead of returning with a thread still stuck
        /// Blocking line source (no trailing newline); false = EOF. Default: stdin (Win32 ReadFile, cancellable).
        std::function<bool(QByteArray&)> readLine;
        /// Unblocks readLine from another thread at shutdown. Default: cancels the stdin read.
        std::function<void()> cancelRead;
        /// Writes one full line (with "\n"); false = the reader is gone. Default: stdout, flushed per line.
        std::function<bool(const QByteArray&)> writeLine;
    };
    using Handler = std::function<QJsonObject(const QJsonObject& request, ServeContext& ctx)>;

    ServeLoop();
    explicit ServeLoop(Options o);
    /// Serves until EOF / quit / idle (returns 0) or a hang / broken output (returns 2).
    int run(const Handler& handler);
    static QJsonObject unsupportedOp(const QString& op);
private:
    Options m_opt;
};

}   // namespace ppcli

#endif // PPCLI_H
