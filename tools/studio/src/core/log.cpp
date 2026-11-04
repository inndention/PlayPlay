#include "core/log.h"

#include "core/clock.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace studio::log {

namespace {

constexpr int kRecentLines = 500;

struct Entry {
    qint64 ms;
    Level level;
    QString category;
    QString message;
};

// One per process, leaked on purpose: a late qWarning from a static destructor must still find it.
class Logger {
public:
    // queue (callers) <-> writer
    std::mutex mutex;
    std::condition_variable wake;          // writer: something to do
    std::condition_variable done;          // flush() / stop(): something was written, or the writer exited
    std::deque<Entry> queue;
    quint64 accepted = 0;                  // lines queued so far
    quint64 written = 0;                   // of those, lines the writer finished
    bool stopping = false;
    quint64 generation = 0;                // the current writer; a writer of an older generation exits when it wakes
    quint64 exitedGeneration = 0;          // newest generation whose writer has returned
    std::thread thread;
    Options opt;                           // read by the writer under `mutex` at batch start
    quint64 reportedDrops = 0;             // writer side: drops already announced in the log

    std::atomic<int> minLevel{int(Level::Info)};
    std::atomic<int> maxQueue{20000};
    std::atomic<const Clock*> clock{nullptr};
    std::atomic<quint64> dropped{0};

    std::mutex recentMutex;
    std::deque<QString> recentLines;
    QString file;                          // under recentMutex (read by currentFile())

    std::mutex secretMutex;
    QStringList secrets;

    void run(quint64 gen);
};

Logger& L() {
    static Logger* const l = new Logger;
    return *l;
}

const Clock& clockOf(const Logger& l) { const Clock* c = l.clock.load(); return c ? *c : Clock::system(); }

const char* levelTag(Level l) {
    switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "INFO ";
}

QString formatLine(const Entry& e) {
    QString msg = e.message;
    msg.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    msg.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    msg.replace(QLatin1Char('\n'), QLatin1String("\n    "));   // continuation lines indented: no forged entries
    QString line = Clock::isoUtc(e.ms) + QLatin1Char(' ') + QLatin1String(levelTag(e.level)) + QLatin1Char(' ');
    if (!e.category.isEmpty()) line += e.category + QLatin1String(": ");
    return line + msg;
}

// The writer's file: one per local calendar day, rolled to .1, .2 ... past maxFileBytes.
class LogFile {
public:
    void configure(const QString& dir, const QString& base, qint64 maxBytes) {
        if (dir != m_dir || base != m_base) close();
        m_dir = dir; m_base = base; m_max = qMax<qint64>(4096, maxBytes);
    }
    /// One line (with its "\n") into the file of `date`; rolls before a line that would start past maxFileBytes.
    void writeLine(const QByteArray& line, const QString& date) {
        if (m_dir.isEmpty()) return;
        // m_bytes, not QFile::size(): size() flushes the write buffer and asks the OS on every call.
        if (date != m_date || !m_file.isOpen() || m_bytes >= m_max) {
            if (date != m_date) m_index = 0;
            open(date);
        }
        if (!m_file.isOpen()) return;
        const qint64 n = m_file.write(line);   // QFile buffers; flush() once per batch
        if (n > 0) m_bytes += n;
    }
    void flush() { if (m_file.isOpen()) m_file.flush(); }
    QString fileName() const { return m_file.isOpen() ? m_file.fileName() : QString(); }
    void close() { if (m_file.isOpen()) m_file.close(); m_date.clear(); m_index = 0; }

private:
    void open(const QString& date) {
        m_file.close();
        m_date = date;
        for (int guard = 0; guard < 10000; ++guard, ++m_index) {
            const QString name = m_base + QLatin1Char('-') + date + (m_index ? QLatin1Char('.') + QString::number(m_index) : QString()) + QStringLiteral(".log");
            m_file.setFileName(QDir(m_dir).filePath(name));
            const qint64 existing = QFileInfo(m_file.fileName()).size();
            if (existing >= m_max) continue;
            if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) return;   // no file: lines still reach stderr / recent
            m_bytes = existing;
            return;
        }
    }
    QString m_dir, m_base, m_date;
    qint64 m_max = 32LL * 1024 * 1024;
    qint64 m_bytes = 0;                    // size of the open file: what it had when opened + what was written since
    int m_index = 0;
    QFile m_file;
};

void removeOld(const QString& dir, const QString& base, int keepDays, qint64 nowMs) {
    if (dir.isEmpty() || keepDays <= 0) return;
    // Only files this logger names are ever deleted.
    const QRegularExpression re(QLatin1Char('^') + QRegularExpression::escape(base) + QStringLiteral("-\\d{4}-\\d{2}-\\d{2}(\\.\\d+)?\\.log$"));
    const QFileInfoList files = QDir(dir).entryInfoList({base + QStringLiteral("-*.log")}, QDir::Files);
    for (const QFileInfo& fi : files) {
        if (!re.match(fi.fileName()).hasMatch()) continue;
        if (nowMs - fi.lastModified().toMSecsSinceEpoch() > qint64(keepDays) * 86400000LL) QFile::remove(fi.absoluteFilePath());
    }
}

void Logger::run(quint64 gen) {
    LogFile out;
    bool retentionDone = false;
    QString retentionDir;
    std::vector<Entry> batch;
    for (;;) {
        Options o;
        {
            std::unique_lock<std::mutex> lk(mutex);
            wake.wait(lk, [&]() { return stopping || gen != generation || !queue.empty(); });
            if (gen != generation) break;  // superseded while stuck in I/O: the newer writer owns the queue
            if (queue.empty() && stopping) break;
            batch.assign(std::make_move_iterator(queue.begin()), std::make_move_iterator(queue.end()));
            queue.clear();
            o = opt;
        }
        out.configure(o.dir, o.baseName, o.maxFileBytes);
        const Clock& clk = o.clock ? *o.clock : Clock::system();
        if (!retentionDone || retentionDir != o.dir) {
            removeOld(o.dir, o.baseName, o.keepDays, clk.nowMs());
            retentionDone = true;
            retentionDir = o.dir;
        }
        const size_t real = batch.size();  // queued lines in this batch (the drop notice below is extra)
        const quint64 drops = dropped.load();
        if (drops > reportedDrops) {
            batch.push_back(Entry{clk.nowMs(), Level::Warn, QStringLiteral("log"),
                                  QStringLiteral("%1 log line(s) dropped: the queue was full").arg(drops - reportedDrops)});
            reportedDrops = drops;
        }
        // Each line goes to the file of its own local date, so a batch that crosses midnight lands in both files.
        QByteArray errChunk;
        std::vector<QString> lines;
        lines.reserve(batch.size());
        for (const Entry& e : batch) {
            const QString line = scrub(formatLine(e));
            const QByteArray bytes = line.toUtf8() + '\n';
            out.writeLine(bytes, Clock::localDateOf(e.ms));
            if (o.toStderr) errChunk += bytes;
            lines.push_back(line);
        }
        out.flush();
        const QString fileNow = out.fileName();
        if (!errChunk.isEmpty()) { std::fwrite(errChunk.constData(), 1, size_t(errChunk.size()), stderr); std::fflush(stderr); }
        {
            std::lock_guard<std::mutex> g(recentMutex);
            for (QString& l : lines) {
                recentLines.push_back(std::move(l));
                if (recentLines.size() > size_t(kRecentLines)) recentLines.pop_front();
            }
            if (!fileNow.isEmpty() || o.dir.isEmpty()) file = fileNow;
        }
        {
            std::lock_guard<std::mutex> g(mutex);
            written += real;
        }
        done.notify_all();
        batch.clear();
    }
    out.close();
    {
        std::lock_guard<std::mutex> g(mutex);
        exitedGeneration = qMax(exitedGeneration, gen);
    }
    done.notify_all();
}

const std::vector<QRegularExpression>& valuePatterns() {
    // mcp/core/util.ts SECRET_VALUE, same order. ECMAScript and PCRE2 agree on these constructs (ASCII \b, classes).
    static const std::vector<QRegularExpression> p = [] {
        const QRegularExpression::PatternOptions ci = QRegularExpression::CaseInsensitiveOption;
        std::vector<QRegularExpression> v;
        v.emplace_back(QStringLiteral("\\bsk-or-v1-[A-Za-z0-9]{16,}"));
        v.emplace_back(QStringLiteral("\\bapv_[a-f0-9]{16,}"));
        v.emplace_back(QStringLiteral("\\b(sk|pk|rk)-[A-Za-z0-9_-]{16,}"));
        v.emplace_back(QStringLiteral("\\b(sk|pk|rk)_(live|test)_[A-Za-z0-9]{8,}"));
        v.emplace_back(QStringLiteral("\\bBearer\\s+[A-Za-z0-9._~+/-]{12,}=*"));
        v.emplace_back(QStringLiteral("\\bgh[pousr]_[A-Za-z0-9]{20,}"));
        v.emplace_back(QStringLiteral("\\bxox[abposr]-[A-Za-z0-9-]{10,}"));
        v.emplace_back(QStringLiteral("\\bAIza[0-9A-Za-z_-]{30,}"));
        v.emplace_back(QStringLiteral("\\bAKIA[0-9A-Z]{16}\\b"));
        v.emplace_back(QStringLiteral("\\beyJ[A-Za-z0-9_-]{5,}\\.eyJ[A-Za-z0-9_-]{5,}\\.[A-Za-z0-9_-]*"));
        v.emplace_back(QStringLiteral("\\b(api[_ -]?key|secret|token|password|passwd|pwd)\\s*[:=]\\s*\\S{4,}"), ci);
        for (auto& re : v) re.optimize();
        return v;
    }();
    return p;
}

void installedHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    Level l = Level::Info;
    switch (type) {
    case QtDebugMsg: l = Level::Debug; break;
    case QtInfoMsg: l = Level::Info; break;
    case QtWarningMsg: l = Level::Warn; break;
    case QtCriticalMsg:
    case QtFatalMsg: l = Level::Error; break;
    }
    const QString cat = ctx.category && qstrcmp(ctx.category, "default") != 0 ? QString::fromLatin1(ctx.category) : QStringLiteral("qt");
    write(l, cat, msg);
    if (type == QtFatalMsg) flush(1000);  // Qt aborts right after: get the reason to disk first
}

QtMessageHandler g_previousHandler = nullptr;
std::atomic<bool> g_handlerInstalled{false};

} // namespace

QString levelName(Level l) {
    switch (l) {
    case Level::Debug: return QStringLiteral("debug");
    case Level::Info: return QStringLiteral("info");
    case Level::Warn: return QStringLiteral("warn");
    case Level::Error: return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

bool parseLevel(QStringView text, Level* out) {
    const QString t = text.trimmed().toString().toLower();
    if (t == QLatin1String("debug")) *out = Level::Debug;
    else if (t == QLatin1String("info")) *out = Level::Info;
    else if (t == QLatin1String("warn") || t == QLatin1String("warning")) *out = Level::Warn;
    else if (t == QLatin1String("error")) *out = Level::Error;
    else return false;
    return true;
}

bool start(const Options& o, QString* error) {
    Logger& l = L();
    stop();
    bool ok = true;
    Options use = o;
    if (!use.dir.isEmpty() && !QDir().mkpath(use.dir)) {
        if (error) *error = QStringLiteral("cannot create the log dir %1").arg(QDir::toNativeSeparators(use.dir));
        use.dir.clear();
        ok = false;
    }
    if (use.baseName.isEmpty()) use.baseName = QStringLiteral("studio");
    l.minLevel = int(use.minLevel);
    l.maxQueue = qMax(16, use.maxQueue);
    l.clock = use.clock;
    quint64 gen;
    {
        std::lock_guard<std::mutex> g(l.mutex);
        l.opt = use;
        l.stopping = false;
        gen = ++l.generation;
    }
    {
        std::lock_guard<std::mutex> g(l.recentMutex);
        l.file.clear();
    }
    l.thread = std::thread([&l, gen]() { l.run(gen); });
    return ok;
}

void stop(int timeoutMs) {
    Logger& l = L();
    std::unique_lock<std::mutex> lk(l.mutex);
    if (!l.thread.joinable()) return;
    const quint64 gen = l.generation;
    l.stopping = true;
    l.wake.notify_all();
    const bool finished = l.done.wait_for(lk, std::chrono::milliseconds(qMax(0, timeoutMs)), [&]() { return l.exitedGeneration >= gen; });
    if (!finished) ++l.generation;         // the stuck writer exits as soon as it wakes, whatever restarts meanwhile
    l.stopping = false;
    lk.unlock();
    if (finished) l.thread.join();
    else l.thread.detach();                // stuck in I/O (a frozen console): never hang the caller's shutdown
    l.clock = nullptr;                     // an injected clock need not outlive the writer it was given to
}

bool running() {
    Logger& l = L();
    std::lock_guard<std::mutex> g(l.mutex);
    return l.thread.joinable() && l.exitedGeneration < l.generation;
}

bool flush(int timeoutMs) {
    Logger& l = L();
    std::unique_lock<std::mutex> lk(l.mutex);
    const quint64 target = l.accepted;
    if (l.written >= target) return true;
    const quint64 gen = l.generation;
    if (!l.thread.joinable() || l.exitedGeneration >= gen) return false;
    l.done.wait_for(lk, std::chrono::milliseconds(qMax(0, timeoutMs)), [&]() { return l.written >= target || l.exitedGeneration >= gen; });
    return l.written >= target;
}

void write(Level level, QStringView category, const QString& message) {
    Logger& l = L();
    if (int(level) < l.minLevel.load(std::memory_order_relaxed)) return;
    Entry e{clockOf(l).nowMs(), level, category.toString(), message};
    if (message.size() > kMaxMessageChars) {
        // Scrubbing happens later, in the writer: a key cut in half here would no longer equal its registered value,
        // and "sk-or-v1-" plus fewer than 16 characters misses the shape pattern. So a word cut by the limit goes whole.
        const auto isBreak = [](QChar c) { return c == u' ' || c == u'\t' || c == u'\n' || c == u'\r'; };
        qsizetype cut = kMaxMessageChars;
        if (!isBreak(message.at(cut)))
            while (cut > 0 && !isBreak(message.at(cut - 1))) --cut;
        e.message = message.left(cut) + QStringLiteral("…[truncated %1 chars]").arg(message.size() - cut);
    }
    {
        std::lock_guard<std::mutex> g(l.mutex);
        if (l.queue.size() >= size_t(l.maxQueue.load(std::memory_order_relaxed))) {
            l.dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        l.queue.push_back(std::move(e));
        ++l.accepted;
    }
    l.wake.notify_one();
}

void setMinLevel(Level lvl) { L().minLevel = int(lvl); }
Level minLevel() { return Level(L().minLevel.load()); }

QStringList recent(int max) {
    Logger& l = L();
    std::lock_guard<std::mutex> g(l.recentMutex);
    QStringList out;
    const int n = qBound(0, max, int(l.recentLines.size()));
    for (size_t i = l.recentLines.size() - size_t(n); i < l.recentLines.size(); ++i) out << l.recentLines[i];
    return out;
}

quint64 droppedLines() { return L().dropped.load(); }

QString currentFile() {
    Logger& l = L();
    std::lock_guard<std::mutex> g(l.recentMutex);
    return l.file;
}

void addSecret(const QString& value) {
    if (value.size() < 8) return;
    Logger& l = L();
    std::lock_guard<std::mutex> g(l.secretMutex);
    if (!l.secrets.contains(value)) l.secrets << value;
}

QString scrub(const QString& text) {
    static const QString kRedacted = QStringLiteral("[redacted]");
    QString t = text;
    QStringList secrets;
    {
        Logger& l = L();
        std::lock_guard<std::mutex> g(l.secretMutex);
        secrets = l.secrets;
    }
    for (const QString& s : secrets) t.replace(s, kRedacted);
    // data: URIs and base64 runs first (as the TS does), so their payload never meets the patterns below.
    static const QRegularExpression dataUri(QStringLiteral("data:([a-z0-9.+/-]+)?(;[a-z0-9=.+-]+)*,[A-Za-z0-9+/=%_-]{16,}"),
                                            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression base64Run(QStringLiteral("[A-Za-z0-9+/]{200,}={0,2}"));
    if (t.contains(QLatin1String("data:"), Qt::CaseInsensitive)) {
        QString out;
        qsizetype last = 0;
        for (auto it = dataUri.globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            out += QStringView(t).mid(last, m.capturedStart() - last);
            const QString mime = m.captured(1).isEmpty() ? QStringLiteral("unknown") : m.captured(1);
            out += QStringLiteral("[data:%1 removed, ~%2 KB]").arg(mime).arg(qRound(double(m.capturedLength()) * 3 / 4 / 1024));
            last = m.capturedEnd();
        }
        if (last > 0) { out += QStringView(t).mid(last); t = out; }
    }
    if (t.size() >= 200) {
        QString out;
        qsizetype last = 0;
        for (auto it = base64Run.globalMatch(t); it.hasNext();) {
            const auto m = it.next();
            out += QStringView(t).mid(last, m.capturedStart() - last);
            out += QStringLiteral("[base64 removed, %1 chars]").arg(m.capturedLength());
            last = m.capturedEnd();
        }
        if (last > 0) { out += QStringView(t).mid(last); t = out; }
    }
    for (const QRegularExpression& re : valuePatterns()) t.replace(re, kRedacted);
    return t;
}

bool isSecretKey(QStringView fieldName) {
    // mcp/core/util.ts SECRET_KEY, verbatim (case-insensitive).
    static const QRegularExpression re(
        QStringLiteral("^(api[_-]?key|apikey|key|token|access[_-]?token|refresh[_-]?token|secret|client[_-]?secret|password|passwd|pwd|"
                       "authorization|auth|cookie|bearer|credentials?|approve|private[_-]?key)$|(_|-)(key|token|secret|password)$|apikey|secret|password"),
        QRegularExpression::CaseInsensitiveOption);
    return re.matchView(fieldName).hasMatch();
}

QString urlForLog(const QUrl& url) {
    return url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
}

void installQtMessageHandler() {
    if (g_handlerInstalled.exchange(true)) return;
    g_previousHandler = qInstallMessageHandler(installedHandler);
}

void removeQtMessageHandler() {
    if (!g_handlerInstalled.exchange(false)) return;
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
}

} // namespace studio::log
