#pragma once
// Studio log (ST-02). Two rules from the live stack shape it:
//   1. Logging never blocks the caller. write() only stamps the line and queues it (bounded; a full queue drops the
//      line and counts it). One writer thread formats, scrubs and writes to <dir>/studio-yyyy-MM-dd.log and, when
//      asked, stderr. A frozen console (the 2026-10-25 live hang: every process logging to one console) can then
//      stall only the writer, never the event loop that serves the API and the provider requests.
//   2. Secrets never reach a log line. Every line passes scrub() in the writer: registered secret values (the
//      OpenRouter key: Config registers it whenever it reads it) plus the key / token shapes of the MCP server's
//      redactText (mcp/core/util.ts: sk-or-v1-..., Bearer ..., apv_ approval tokens, JWTs, "api_key=..."), data: URIs
//      and long base64 runs. Diagnostics log paths only (urlForLog drops query strings), like openrouter.ts L359.
// The fuller redaction of structured values (redactValue) belongs to the safety module (ST-15); isSecretKey() here is
// the same field-name rule, shared so Config::describe() and the API hide the same fields.
#include <QString>
#include <QStringList>
#include <QStringView>

class QUrl;

namespace studio {
class Clock;
}

namespace studio::log {

enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };
QString levelName(Level l);                // "debug" | "info" | "warn" | "error"
bool parseLevel(QStringView text, Level* out);

struct Options {
    QString dir;                           // empty: no file (stderr and recent() only)
    QString baseName = QStringLiteral("studio");   // <dir>/<baseName>-yyyy-MM-dd[.N].log
    bool toStderr = false;
    Level minLevel = Level::Info;
    int maxQueue = 20000;                  // lines waiting for the writer; more are dropped and counted
    qint64 maxFileBytes = 32LL * 1024 * 1024;      // then <baseName>-yyyy-MM-dd.1.log, .2 ...
    int keepDays = 14;                     // older <baseName>-*.log files are removed when the writer starts (0 = keep)
    const Clock* clock = nullptr;          // line stamps and file dates; nullptr = Clock::system(). Used until stop().
};

/// Starts (or restarts with new options) the writer thread. Lines written before start() wait in the queue. False
/// (and `error`) when the dir cannot be created; the log then keeps working without a file.
bool start(const Options& o, QString* error = nullptr);
/// Drains and stops the writer (waits up to `timeoutMs`; a writer stuck in I/O is left behind, the process is exiting).
void stop(int timeoutMs = 2000);
bool running();
/// Waits until every line queued before the call is written. Shutdown and tests only (it blocks). False on timeout or
/// when no writer runs.
bool flush(int timeoutMs = 2000);

/// A longer message is cut before it is queued, back to the last space / tab / line break, so a secret that straddles
/// the cut is dropped whole rather than left as a prefix too short for scrub() to recognise (assumes secrets contain
/// no whitespace, which holds for API keys and tokens). A note with the dropped length is appended.
constexpr qsizetype kMaxMessageChars = 64 * 1024;
/// Never blocks on I/O. Lines below the minimum level are discarded here.
void write(Level level, QStringView category, const QString& message);
inline void debug(QStringView category, const QString& m) { write(Level::Debug, category, m); }
inline void info(QStringView category, const QString& m) { write(Level::Info, category, m); }
inline void warn(QStringView category, const QString& m) { write(Level::Warn, category, m); }
inline void error(QStringView category, const QString& m) { write(Level::Error, category, m); }
void setMinLevel(Level l);
Level minLevel();

/// The newest written lines (scrubbed, oldest first), at most `max` (the writer keeps 500).
QStringList recent(int max = 200);
/// Lines dropped because the queue was full, since the process started.
quint64 droppedLines();
/// The file the writer writes now (empty without a file).
QString currentFile();

// ---------------- secrets ----------------

/// From now on this exact value is replaced in every line (values shorter than 8 chars are ignored, as in the TS).
void addSecret(const QString& value);
/// Text with registered secret values, key / token shapes, data: URIs and base64 runs replaced (mcp/core/util.ts
/// redactText rules; "[redacted]", "[data:<mime> removed, ~N KB]", "[base64 removed, N chars]"). Thread-safe.
QString scrub(const QString& text);
/// mcp/core/util.ts SECRET_KEY: true for field names whose values are never logged or returned (apiKey, token,
/// password, client_secret, authorization, x-api-key ...).
bool isSecretKey(QStringView fieldName);
/// scheme://host[:port]/path of a URL: no user info, query or fragment (they may carry keys or signed tokens).
QString urlForLog(const QUrl& url);

/// Routes qDebug / qInfo / qWarning / qCritical / qFatal through write() (category = the logging category name).
void installQtMessageHandler();
void removeQtMessageHandler();

} // namespace studio::log
