// core/log.cpp - the queued, never-blocking process log (ANFS-F1b).
//
// The caller only masks secrets, stamps the time and appends to a bounded queue under a short lock; one writer thread
// per Log owns the file (daily anfs-YYYYMMDD.log, newest keepDays kept) and a second thread owns stdout. stdout has its
// own thread because a console or pipe that stops reading (the PlayPlay 10-24/10-25 hang) must not stall the file
// either; its queue is small and drops. Both threads are detached and share their state through shared_ptrs, so a Log
// destroyed while a thread is stuck in a write returns after the bounded wait instead of hanging.
//
// Line format (the C# Log.cs form): "yyyy-MM-dd HH:mm:ss.fff LEVEL message", local time, level padded to 5, CR -> ' ',
// LF -> " | ", CRLF line ends in the file (LF on stdout, which the CRT's text mode expands). The time is taken from
// anfs::clock() when write() is called, so a ManualClock moves both
// the stamps and the midnight rotation in tests.
//
// redact() is a hand-written scanner for the three C# Log.Redact regexes (std::regex is slow and recurses per
// character on MSVC, which a long line would turn into a stack overflow) plus `auth` and `key` values.
#include "core/log.h"

#include "core/clock.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace anfs {

namespace {

constexpr size_t kQueueMaxLines = 10000;
constexpr size_t kQueueMaxBytes = 16u << 20;
constexpr size_t kMaxMessageBytes = 64u << 10;   // one runaway message must not pin megabytes in the queue
constexpr size_t kConsoleQueueMax = 256;
constexpr int kConsoleBurst = 20;                // stdout lines per window (mgmt-integration.md section 0)
constexpr int64_t kConsoleWindowMs = 10000;
constexpr auto kDestructorWait = std::chrono::milliseconds(2000);
constexpr auto kFlushWait = std::chrono::milliseconds(10000);
constexpr auto kReopenBackoff = std::chrono::milliseconds(1000);

std::wstring widen(std::string_view s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// ---- formatting ---------------------------------------------------------------------------------------------------

struct LocalStamp {
    char date[9];    // YYYYMMDD (file name)
    char text[24];   // yyyy-MM-dd HH:mm:ss.fff
};

LocalStamp localStamp(int64_t epochMs)
{
    int64_t secs = epochMs / 1000, ms = epochMs % 1000;
    if (ms < 0) {
        ms += 1000;
        --secs;
    }
    const std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) tm = std::tm{};
    LocalStamp s{};
    std::snprintf(s.date, sizeof s.date, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::snprintf(s.text, sizeof s.text, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return s;
}

const char* levelName(LogLevel l)
{
    switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Warn: return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

// File lines end in CRLF (written in binary). Console lines end in LF: stdout is a text-mode CRT stream that turns
// LF into CRLF itself, so CRLF there would reach StarRoot's component log as CR CR LF.
void appendLine(std::string& out, const char* stamp, LogLevel level, std::string_view msg, const char* eol = "\r\n")
{
    out += stamp;
    out += ' ';
    out += levelName(level);
    out += ' ';
    for (char c : msg) {
        if (c == '\r') out += ' ';
        else if (c == '\n') out += " | ";
        else out += c;
    }
    out += eol;
}

std::string consoleLine(int64_t epochMs, LogLevel level, std::string_view msg)
{
    std::string out;
    out.reserve(msg.size() + 32);
    appendLine(out, localStamp(epochMs).text, level, msg, "\n");
    return out;
}

// ---- redaction ----------------------------------------------------------------------------------------------------

bool isWordByte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }
bool isAlnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string asciiLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool at(const std::string& lower, size_t i, std::string_view word) { return lower.compare(i, word.size(), word) == 0; }

size_t skipSpace(std::string_view s, size_t j)
{
    while (j < s.size() && isSpace(static_cast<unsigned char>(s[j]))) ++j;
    return j;
}

// C#: (?i)\b(authorization|proxy-authorization|x-agent-auth|cookie|set-cookie|x-api-key)\b(\s*[:=]\s*)[^\r\n]*
//     -> $1$2[redacted]
std::string redactHeaders(std::string_view s)
{
    static constexpr std::string_view names[] = {"authorization", "proxy-authorization", "x-agent-auth",
                                                 "cookie", "set-cookie", "x-api-key"};
    const std::string lower = asciiLower(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        bool done = false;
        if (i == 0 || !isWordByte(static_cast<unsigned char>(s[i - 1]))) {
            for (std::string_view n : names) {
                const size_t e = i + n.size();
                if (!at(lower, i, n) || (e < s.size() && isWordByte(static_cast<unsigned char>(s[e])))) continue;
                size_t j = skipSpace(s, e);
                if (j >= s.size() || (s[j] != ':' && s[j] != '=')) continue;
                j = skipSpace(s, j + 1);
                out.append(s.substr(i, j - i));
                out += "[redacted]";
                while (j < s.size() && s[j] != '\r' && s[j] != '\n') ++j;
                i = j;
                done = true;
                break;
            }
        }
        if (!done) out += s[i++];
    }
    return out;
}

// C#: (?i)\b(basic|bearer)\s+[A-Za-z0-9+/=._~\-]{6,}  ->  $1 [redacted]
std::string redactSchemes(std::string_view s)
{
    static constexpr std::string_view names[] = {"basic", "bearer"};
    auto tokenChar = [](unsigned char c) {
        return isAlnum(c) || c == '+' || c == '/' || c == '=' || c == '.' || c == '_' || c == '~' || c == '-';
    };
    const std::string lower = asciiLower(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        bool done = false;
        if (i == 0 || !isWordByte(static_cast<unsigned char>(s[i - 1]))) {
            for (std::string_view n : names) {
                const size_t e = i + n.size();
                if (!at(lower, i, n) || e >= s.size() || !isSpace(static_cast<unsigned char>(s[e]))) continue;
                const size_t t = skipSpace(s, e);
                size_t k = t;
                while (k < s.size() && tokenChar(static_cast<unsigned char>(s[k]))) ++k;
                if (k - t < 6) continue;
                out.append(s.substr(i, n.size()));
                out += " [redacted]";
                i = k;
                done = true;
                break;
            }
        }
        if (!done) out += s[i++];
    }
    return out;
}

// C#: (?i)("?(?:password|passwd|pwd|token|secret|agenttoken|api[-_]?key)"?\s*[:=]\s*)("[^"]*"|[^\s"&,;}]+)
//     -> $1"[redacted]"
// plus `auth` (?auth=, "auth":) and `key` (peer_key=, "key":), the latter only after a non-alphanumeric character
// so monkey=3 stays readable.
std::string redactKeys(std::string_view s)
{
    struct Name {
        std::string_view word;
        bool needBoundary;
    };
    static constexpr Name names[] = {{"password", false}, {"passwd", false}, {"pwd", false},     {"token", false},
                                     {"secret", false},   {"agenttoken", false}, {"api-key", false}, {"api_key", false},
                                     {"apikey", false},   {"auth", false},   {"key", true}};
    auto valueChar = [](unsigned char c) {
        return !isSpace(c) && c != '"' && c != '&' && c != ',' && c != ';' && c != '}';
    };
    const std::string lower = asciiLower(s);
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        bool done = false;
        for (const Name& n : names) {
            if (!at(lower, i, n.word)) continue;
            if (n.needBoundary && i > 0 && isAlnum(static_cast<unsigned char>(s[i - 1]))) continue;
            size_t j = i + n.word.size();
            if (j < s.size() && s[j] == '"') ++j;
            j = skipSpace(s, j);
            if (j >= s.size() || (s[j] != ':' && s[j] != '=')) continue;
            j = skipSpace(s, j + 1);
            if (j >= s.size()) continue;
            size_t end;
            if (s[j] == '"') {
                const size_t q = s.find('"', j + 1);
                if (q == std::string_view::npos) continue;
                end = q + 1;
            } else {
                end = j;
                while (end < s.size() && valueChar(static_cast<unsigned char>(s[end]))) ++end;
                if (end == j) continue;
                // Already masked by the header rule or a registered secret: leave it (C# would quote it again,
                // turning "x-api-key: [redacted]" into "x-api-key: \"[redacted]\"").
                const std::string_view v = s.substr(j, end - j);
                if (v.substr(0, 10) == "[redacted]" || v.substr(0, 8) == "[secret]") continue;
            }
            out.append(s.substr(i, j - i));
            out += "\"[redacted]\"";
            i = end;
            done = true;
            break;
        }
        if (!done) out += s[i++];
    }
    return out;
}

void replaceAll(std::string& s, const std::string& what, std::string_view with)
{
    if (what.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(what, pos)) != std::string::npos) {
        s.replace(pos, what.size(), with);
        pos += with.size();
    }
}

// ---- writer threads -------------------------------------------------------------------------------------------------

struct Entry {
    int64_t ms;
    LogLevel level;
    std::string msg;
};

bool isLow(LogLevel l) { return l == LogLevel::Debug || l == LogLevel::Info; }

struct FileState {
    std::mutex mu;
    std::condition_variable cv;        // wakes the writer
    std::condition_variable idleCv;    // flush() and the destructor
    std::deque<Entry> q;
    size_t qBytes = 0;
    size_t qLow = 0;                   // Debug/Info entries in q (skips the drop scan when there are none)
    int64_t dropped = 0;
    uint64_t enqueued = 0, written = 0;
    bool started = false, stop = false, exited = false;

    // writer thread only
    std::string dir;
    int keep = 14;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::string fileDate;
    int64_t lost = 0;                  // lines that could not be written (folder or file unusable)
    std::chrono::steady_clock::time_point retryAt{};
};

struct ConsoleState {
    std::mutex mu;
    std::condition_variable cv;
    std::condition_variable idleCv;
    std::deque<std::string> q;
    int64_t dropped = 0;
    uint64_t enqueued = 0, written = 0;
    bool started = false, stop = false, exited = false;
};

void closeFile(FileState& s)
{
    if (s.file != INVALID_HANDLE_VALUE) CloseHandle(s.file);
    s.file = INVALID_HANDLE_VALUE;
    s.fileDate.clear();
}

bool isDailyName(const std::wstring& n)
{
    // anfs-YYYYMMDD.log exactly; the C# anfs.log / anfs.N.log files are not ours to delete.
    if (n.size() != 17 || n.compare(0, 5, L"anfs-") != 0 || _wcsicmp(n.c_str() + 13, L".log") != 0) return false;
    for (size_t i = 5; i < 13; ++i)
        if (n[i] < L'0' || n[i] > L'9') return false;
    return true;
}

void pruneOld(FileState& s)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::wstring> names;
    for (fs::directory_iterator it(widen(s.dir), ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring n = it->path().filename().wstring();
        if (isDailyName(n)) names.push_back(std::move(n));
    }
    if (names.size() <= static_cast<size_t>(s.keep)) return;
    std::sort(names.begin(), names.end(), std::greater<std::wstring>());   // newest date first
    const std::wstring open = widen("anfs-" + s.fileDate + ".log");
    for (size_t i = static_cast<size_t>(s.keep); i < names.size(); ++i) {
        if (_wcsicmp(names[i].c_str(), open.c_str()) == 0) continue;   // after a clock jump back the open file sorts old
        std::error_code rm;
        fs::remove(fs::path(widen(s.dir)) / names[i], rm);
    }
}

bool openFor(FileState& s, const char* date)
{
    if (s.file != INVALID_HANDLE_VALUE && s.fileDate == date) return true;
    closeFile(s);
    if (std::chrono::steady_clock::now() < s.retryAt) return false;
    std::error_code ec;
    std::filesystem::create_directories(widen(s.dir), ec);
    const std::wstring path = widen(s.dir + "\\anfs-" + date + ".log");
    s.file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s.file == INVALID_HANDLE_VALUE) {
        s.retryAt = std::chrono::steady_clock::now() + kReopenBackoff;
        return false;
    }
    s.fileDate = date;
    pruneOld(s);
    return true;
}

bool writeAll(HANDLE h, const std::string& data)
{
    size_t off = 0;
    while (off < data.size()) {
        const DWORD piece = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 24));
        DWORD n = 0;
        if (!WriteFile(h, data.data() + off, piece, &n, nullptr) || n == 0) return false;
        off += n;
    }
    return true;
}

// Writes one batch, grouped by local date (a line stamped 23:59:59.999 goes to that day's file even when it is
// written after midnight).
void writeBatch(FileState& s, std::deque<Entry>& batch, int64_t dropped)
{
    std::string buf, bufDate;
    size_t bufLines = 0;
    auto flushBuf = [&] {
        if (buf.empty()) return;
        if (openFor(s, bufDate.c_str())) {
            if (s.lost > 0) {
                std::string note;
                appendLine(note, localStamp(nowMs()).text, LogLevel::Warn,
                           std::to_string(s.lost) + " log lines lost (log file was not writable)");
                if (writeAll(s.file, note)) s.lost = 0;
            }
            if (!writeAll(s.file, buf)) {
                s.lost += static_cast<int64_t>(bufLines);
                closeFile(s);
                s.retryAt = std::chrono::steady_clock::now() + kReopenBackoff;
            }
        } else {
            s.lost += static_cast<int64_t>(bufLines);
        }
        buf.clear();
        bufLines = 0;
    };
    auto add = [&](int64_t ms, LogLevel level, std::string_view msg) {
        const LocalStamp st = localStamp(ms);
        if (bufDate != st.date) {
            flushBuf();
            bufDate = st.date;
        }
        appendLine(buf, st.text, level, msg);
        ++bufLines;
    };
    if (dropped > 0) {
        const int64_t ms = batch.empty() ? nowMs() : batch.front().ms;
        add(ms, LogLevel::Warn, "log queue full: dropped " + std::to_string(dropped) + " lines");
    }
    for (const Entry& e : batch) add(e.ms, e.level, e.msg);
    flushBuf();
}

void fileWriterLoop(std::shared_ptr<FileState> s)
{
    std::unique_lock<std::mutex> lk(s->mu);
    for (;;) {
        s->cv.wait(lk, [&] { return s->stop || !s->q.empty() || s->dropped > 0; });
        if (s->q.empty() && s->dropped == 0) break;   // stop requested and everything written
        std::deque<Entry> batch;
        batch.swap(s->q);
        s->qBytes = 0;
        s->qLow = 0;
        const int64_t dropped = std::exchange(s->dropped, 0);
        const uint64_t upto = s->enqueued;
        lk.unlock();
        writeBatch(*s, batch, dropped);
        batch.clear();
        lk.lock();
        s->written = upto;
        s->idleCv.notify_all();
    }
    closeFile(*s);
    s->exited = true;
    s->idleCv.notify_all();
}

void consoleLoop(std::shared_ptr<ConsoleState> s)
{
    std::unique_lock<std::mutex> lk(s->mu);
    for (;;) {
        s->cv.wait(lk, [&] { return s->stop || !s->q.empty() || s->dropped > 0; });
        if (s->q.empty() && s->dropped == 0) break;
        std::deque<std::string> batch;
        batch.swap(s->q);
        const int64_t dropped = std::exchange(s->dropped, 0);
        const uint64_t upto = s->enqueued;
        lk.unlock();
        std::string out;
        if (dropped > 0) out = consoleLine(nowMs(), LogLevel::Warn, std::to_string(dropped) + " console lines dropped (stdout is not keeping up)");
        for (const std::string& line : batch) out += line;
        // The only place that can block on a slow console: this thread, never a caller.
        std::fwrite(out.data(), 1, out.size(), stdout);
        std::fflush(stdout);
        lk.lock();
        s->written = upto;
        s->idleCv.notify_all();
    }
    s->exited = true;
    s->idleCv.notify_all();
}

} // namespace

// ---- Log --------------------------------------------------------------------------------------------------------------

struct Log::Impl {
    std::string dir;
    std::shared_ptr<FileState> file = std::make_shared<FileState>();
    std::shared_ptr<ConsoleState> console = std::make_shared<ConsoleState>();

    std::atomic<int> minLevel{static_cast<int>(LogLevel::Info)};
    std::atomic<bool> consoleOn{false};

    mutable std::shared_mutex secretsMu;
    std::vector<std::string> secrets;

    mutable std::mutex statsMu;
    std::string lastError;
    int64_t lastErrorAt = 0;
    int errorCount = 0;

    std::mutex rateMu;                 // console rate limit (anfs::monoMs() time)
    int64_t windowStart = 0;
    bool windowOpen = false;
    int windowCount = 0;
    int64_t suppressed = 0;

    std::string clean(std::string_view msg) const
    {
        std::string text(msg.substr(0, std::min(msg.size(), kMaxMessageBytes)));
        {
            std::shared_lock<std::shared_mutex> g(secretsMu);
            for (const std::string& sec : secrets) replaceAll(text, sec, "[secret]");
        }
        text = Log::redact(text);
        if (msg.size() > kMaxMessageBytes) text += " ...(" + std::to_string(msg.size() - kMaxMessageBytes) + " bytes cut)";
        return text;
    }

    void enqueueFile(int64_t ms, LogLevel level, std::string text)
    {
        FileState& s = *file;
        std::lock_guard<std::mutex> g(s.mu);
        if (s.stop) return;
        if (!s.started) {
            s.started = true;
            s.dir = dir;
            std::thread(fileWriterLoop, file).detach();
        }
        const size_t bytes = text.size() + 32;
        while (!s.q.empty() && (s.q.size() >= kQueueMaxLines || s.qBytes + bytes > kQueueMaxBytes)) {
            // Full: drop the oldest Debug/Info line; with none queued, a new Debug/Info line is the one dropped,
            // and only a queue made entirely of Warn/Error loses its oldest line.
            auto victim = s.q.end();
            if (s.qLow > 0) victim = std::find_if(s.q.begin(), s.q.end(), [](const Entry& e) { return isLow(e.level); });
            if (victim == s.q.end()) {
                if (isLow(level)) {
                    ++s.dropped;
                    s.cv.notify_one();
                    return;
                }
                victim = s.q.begin();
            }
            s.qBytes -= victim->msg.size() + 32;
            if (isLow(victim->level)) --s.qLow;
            s.q.erase(victim);
            ++s.dropped;
        }
        s.q.push_back(Entry{ms, level, std::move(text)});
        s.qBytes += bytes;
        if (isLow(level)) ++s.qLow;
        ++s.enqueued;
        s.cv.notify_one();
    }

    void enqueueConsole(std::string line)
    {
        ConsoleState& s = *console;
        std::lock_guard<std::mutex> g(s.mu);
        if (s.stop) return;
        if (!s.started) {
            s.started = true;
            std::thread(consoleLoop, console).detach();
        }
        if (s.q.size() >= kConsoleQueueMax) {
            ++s.dropped;
        } else {
            s.q.push_back(std::move(line));
            ++s.enqueued;
        }
        s.cv.notify_one();
    }

    void toConsole(int64_t ms, LogLevel level, const std::string& text)
    {
        std::string marker;
        {
            std::lock_guard<std::mutex> g(rateMu);
            const int64_t now = monoMs();
            if (!windowOpen || now - windowStart >= kConsoleWindowMs || now < windowStart) {
                if (suppressed > 0)
                    marker = consoleLine(ms, LogLevel::Warn, std::to_string(suppressed) +
                                                                 " console lines suppressed (at most 20 per 10 s)");
                windowOpen = true;
                windowStart = now;
                windowCount = 0;
                suppressed = 0;
            }
            if (windowCount >= kConsoleBurst) {
                ++suppressed;
                return;
            }
            ++windowCount;
        }
        if (!marker.empty()) enqueueConsole(std::move(marker));
        enqueueConsole(consoleLine(ms, level, text));
    }

    void submit(LogLevel level, std::string_view msg, bool isState)
    {
        if (!isState && static_cast<int>(level) < minLevel.load(std::memory_order_relaxed)) return;
        const int64_t ms = nowMs();
        std::string text = clean(msg);
        if (level == LogLevel::Error) {
            std::lock_guard<std::mutex> g(statsMu);
            lastError = text;
            lastErrorAt = ms;
            ++errorCount;
        }
        if (consoleOn.load(std::memory_order_relaxed) && (isState || level >= LogLevel::Warn)) toConsole(ms, level, text);
        if (!dir.empty()) enqueueFile(ms, level, std::move(text));
    }

    template <class State> static void waitIdle(State& s, std::chrono::milliseconds limit)
    {
        std::unique_lock<std::mutex> lk(s.mu);
        if (!s.started) return;
        const uint64_t target = s.enqueued;
        s.idleCv.wait_for(lk, limit, [&] { return s.written >= target || s.exited; });
    }

    template <class State> static void shutdown(State& s, std::chrono::steady_clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lk(s.mu);
        s.stop = true;
        s.cv.notify_all();
        if (s.started) s.idleCv.wait_until(lk, deadline, [&] { return s.exited; });
    }
};

Log::Log(std::string logsDir, int keepDays) : d_(std::make_unique<Impl>())
{
    while (logsDir.size() > 3 && (logsDir.back() == '\\' || logsDir.back() == '/')) logsDir.pop_back();
    d_->dir = std::move(logsDir);
    d_->file->keep = std::max(1, keepDays);
}

Log::~Log()
{
    // Bounded: at most 2 s in total for both threads; a thread stuck in a write keeps its state alive and exits later.
    const auto deadline = std::chrono::steady_clock::now() + kDestructorWait;
    Impl::shutdown(*d_->file, deadline);
    Impl::shutdown(*d_->console, deadline);
}

void Log::write(LogLevel level, std::string_view msg) { d_->submit(level, msg, false); }
void Log::state(std::string_view msg) { d_->submit(LogLevel::Info, msg, true); }

void Log::setMinLevel(LogLevel level) { d_->minLevel.store(static_cast<int>(level), std::memory_order_relaxed); }
void Log::setConsole(bool on) { d_->consoleOn.store(on, std::memory_order_relaxed); }

void Log::registerSecret(std::string literal)
{
    if (literal.size() < 6) return;
    std::unique_lock<std::shared_mutex> g(d_->secretsMu);
    if (std::find(d_->secrets.begin(), d_->secrets.end(), literal) != d_->secrets.end()) return;
    d_->secrets.push_back(std::move(literal));
    // Longest first, so a secret that contains another one is masked whole.
    std::sort(d_->secrets.begin(), d_->secrets.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
}

void Log::flush()
{
    Impl::waitIdle(*d_->file, kFlushWait);
    Impl::waitIdle(*d_->console, kFlushWait);
}

std::string Log::lastError() const
{
    std::lock_guard<std::mutex> g(d_->statsMu);
    return d_->lastError;
}

int64_t Log::lastErrorAtMs() const
{
    std::lock_guard<std::mutex> g(d_->statsMu);
    return d_->lastErrorAt;
}

int Log::errorCount() const
{
    std::lock_guard<std::mutex> g(d_->statsMu);
    return d_->errorCount;
}

std::string Log::currentFile() const
{
    if (d_->dir.empty()) return {};
    return d_->dir + "\\anfs-" + localStamp(nowMs()).date + ".log";
}

std::string Log::dir() const { return d_->dir; }

std::string Log::redact(std::string_view s)
{
    if (s.empty()) return {};
    return redactKeys(redactSchemes(redactHeaders(s)));
}

Log& nullLog()
{
    // No folder and no console: write() only updates the error counters. Never destroyed (used from destructors).
    static Log* l = new Log();
    return *l;
}

} // namespace anfs
