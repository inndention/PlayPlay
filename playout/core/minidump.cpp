#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif
#include "minidump.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace minidump {

QString sanitizedName(const QString& name) {
    QString s;
    for (const QChar c : name) s += (c.isLetterOrNumber() && c.unicode() < 128) || c == '.' || c == '_' || c == '-' ? c : QChar('_');
    return s.isEmpty() ? QStringLiteral("process") : s.left(48);
}

QStringList existingDumps(const QString& dir, const QString& name) {
    const QString n = sanitizedName(name);
    const QRegularExpression re("^" + QRegularExpression::escape(n) + R"(-\d+-\d{8}T\d{6}Z-[a-z]+\.dmp$)");
    QFileInfoList l = QDir(dir).entryInfoList({n + "-*.dmp"}, QDir::Files, QDir::Time);
    QStringList out;
    for (const QFileInfo& fi : l) if (re.match(fi.fileName()).hasMatch()) out << fi.absoluteFilePath();
    return out;
}

#ifdef _WIN32

namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymInitializeWFn = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymRefreshModuleListFn = BOOL(WINAPI*)(HANDLE);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);
using SymFromAddrWFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFOW);
using SymGetLineFromAddrW64Fn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINEW64);

enum : LONG { Idle = 0, Requested = 1, Done = 2, Abandoned = 3 };
enum Result { Ok = 0, RateLimited = 1, Failed = 2 };

// Everything the worker and the crash filter touch lives here, is plain data, and is never freed: the crash path must
// not allocate or take locks someone else may hold.
struct State {
    bool ready = false;
    HMODULE dbghelp = nullptr;
    MiniDumpWriteDumpFn writeDump = nullptr;
    SymSetOptionsFn symSetOptions = nullptr;
    SymInitializeWFn symInitialize = nullptr;
    SymRefreshModuleListFn symRefresh = nullptr;
    SymCleanupFn symCleanup = nullptr;
    SymFromAddrWFn symFromAddr = nullptr;
    SymGetLineFromAddrW64Fn symGetLine = nullptr;
    HANDLE dbgLock = nullptr;           // mutex: DbgHelp is single-threaded
    bool symReady = false;
    HANDLE symProcess = nullptr;

    HANDLE worker = nullptr, reqEvent = nullptr, doneEvent = nullptr, crashDone = nullptr;
    DWORD workerTid = 0;
    volatile LONG busy = 0;             // one request at a time
    volatile LONG state = Idle;

    // configuration (written by init() while holding `busy`)
    wchar_t dir[1024] = {};
    wchar_t name[64] = {};
    int maxPerHour = 3;
    int keep = 20;

    // request / result
    bool crash = false;
    char reason[16] = {};
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD tid = 0;
    int result = Failed;
    DWORD error = 0;
    wchar_t path[1200] = {};

    LPTOP_LEVEL_EXCEPTION_FILTER previousFilter = nullptr;
    volatile LONG crashEntered = 0;
    bool crashInstalled = false;
};
State g;

// --- plain helpers (no heap, no CRT locale) --------------------------------------------------------------------------

void wappend(wchar_t* dst, size_t cap, const wchar_t* src) {
    size_t n = wcslen(dst);
    while (*src && n + 1 < cap) dst[n++] = *src++;
    dst[n] = 0;
}
void wappendAscii(wchar_t* dst, size_t cap, const char* src) {
    size_t n = wcslen(dst);
    while (*src && n + 1 < cap) dst[n++] = wchar_t(static_cast<unsigned char>(*src++));
    dst[n] = 0;
}
void wappendUInt(wchar_t* dst, size_t cap, unsigned long long v, int minDigits = 1) {
    wchar_t tmp[24]; int i = 0;
    do { tmp[i++] = wchar_t(L'0' + v % 10); v /= 10; } while (v && i < 23);
    while (i < minDigits && i < 23) tmp[i++] = L'0';
    size_t n = wcslen(dst);
    while (i > 0 && n + 1 < cap) dst[n++] = tmp[--i];
    dst[n] = 0;
}

ULONGLONG fileTimeU64(const FILETIME& ft) { return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

// "<name>-<pid>-<yyyyMMddTHHmmssZ>-<reason>.dmp": the part after "<name>-" must be digits, '-', 8 digits, 'T',
// 6 digits, 'Z-', letters, ".dmp". A channel id that extends another with a dash ("main" vs "main-2") never matches.
bool isOurDump(const wchar_t* file, bool* isCrash) {
    const size_t nl = wcslen(g.name);
    if (_wcsnicmp(file, g.name, nl) != 0 || file[nl] != L'-') return false;
    const wchar_t* p = file + nl + 1;
    auto digits = [&p](int exact) { int n = 0; while (*p >= L'0' && *p <= L'9') { ++p; ++n; } return exact ? n == exact : n > 0; };
    if (!digits(0) || *p++ != L'-' || !digits(8) || *p++ != L'T' || !digits(6) || *p++ != L'Z' || *p++ != L'-') return false;
    const wchar_t* r = p;
    while (*p >= L'a' && *p <= L'z') ++p;
    if (p == r || _wcsicmp(p, L".dmp") != 0) return false;
    if (isCrash) *isCrash = (p - r) == 5 && wcsncmp(r, L"crash", 5) == 0;
    return true;
}

// Counts this name's dumps (of the request's kind) written in the last hour, and deletes the oldest ones until fewer
// than `keep` remain. Small directories (a handful of files), so repeated scans are fine.
int recentCountAndPrune(bool crash) {
    wchar_t pattern[1100] = {};
    wappend(pattern, 1100, g.dir); wappend(pattern, 1100, L"\\"); wappend(pattern, 1100, g.name); wappend(pattern, 1100, L"-*.dmp");
    FILETIME nowFt; GetSystemTimeAsFileTime(&nowFt);
    const ULONGLONG hourAgo = fileTimeU64(nowFt) - 36000000000ULL;   // 1 h in 100 ns units
    for (int pass = 0; pass < 64; ++pass) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        int total = 0, recent = 0;
        ULONGLONG oldest = ~0ULL; wchar_t oldestName[MAX_PATH] = {};
        do {
            bool c = false;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !isOurDump(fd.cFileName, &c)) continue;
            ++total;
            const ULONGLONG t = fileTimeU64(fd.ftLastWriteTime);
            if (c == crash && t >= hourAgo) ++recent;
            if (t < oldest) { oldest = t; oldestName[0] = 0; wappend(oldestName, MAX_PATH, fd.cFileName); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        if (total < g.keep || !oldestName[0]) return recent;
        wchar_t victim[1200] = {};
        wappend(victim, 1200, g.dir); wappend(victim, 1200, L"\\"); wappend(victim, 1200, oldestName);
        if (!DeleteFileW(victim)) return recent;
    }
    return 0;
}

void makeDirs(const wchar_t* dir) {
    wchar_t part[1024] = {};
    for (size_t i = 0; dir[i] && i + 1 < 1024; ++i) {
        part[i] = dir[i]; part[i + 1] = 0;
        if ((dir[i + 1] == L'\\' || dir[i + 1] == L'/' || dir[i + 1] == 0) && i > 2) CreateDirectoryW(part, nullptr);
    }
}

bool lockDbgHelp(DWORD ms) { const DWORD w = WaitForSingleObject(g.dbgLock, ms); return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED; }
void unlockDbgHelp() { ReleaseMutex(g.dbgLock); }

void doRequest() {
    g.path[0] = 0; g.error = 0;
    makeDirs(g.dir);
    if (recentCountAndPrune(g.crash) >= g.maxPerHour) { g.result = RateLimited; return; }
    SYSTEMTIME st; GetSystemTime(&st);
    wappend(g.path, 1200, g.dir); wappend(g.path, 1200, L"\\"); wappend(g.path, 1200, g.name); wappend(g.path, 1200, L"-");
    wappendUInt(g.path, 1200, GetCurrentProcessId()); wappend(g.path, 1200, L"-");
    wappendUInt(g.path, 1200, st.wYear, 4); wappendUInt(g.path, 1200, st.wMonth, 2); wappendUInt(g.path, 1200, st.wDay, 2);
    wappend(g.path, 1200, L"T");
    wappendUInt(g.path, 1200, st.wHour, 2); wappendUInt(g.path, 1200, st.wMinute, 2); wappendUInt(g.path, 1200, st.wSecond, 2);
    wappend(g.path, 1200, L"Z-"); wappendAscii(g.path, 1200, g.reason); wappend(g.path, 1200, L".dmp");
    HANDLE f = CreateFileW(g.path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { g.error = GetLastError(); g.result = Failed; return; }
    const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = g.tid; mei.ExceptionPointers = g.ep; mei.ClientPointers = FALSE;
    // A symbol lookup on the watchdog thread can hold DbgHelp for a while; after 10 s the dump goes ahead anyway.
    const bool locked = lockDbgHelp(10000);
    const BOOL ok = g.writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, g.ep ? &mei : nullptr, nullptr, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    if (locked) unlockDbgHelp();
    CloseHandle(f);
    if (!ok) { DeleteFileW(g.path); g.error = err; g.result = Failed; g.path[0] = 0; return; }
    g.result = Ok;
}

DWORD WINAPI workerMain(void*) {
    for (;;) {
        if (WaitForSingleObject(g.reqEvent, INFINITE) != WAIT_OBJECT_0) return 0;
        doRequest();
        if (InterlockedCompareExchange(&g.state, Done, Requested) == Requested) SetEvent(g.doneEvent);
        else { InterlockedExchange(&g.state, Idle); InterlockedExchange(&g.busy, 0); }   // the requester gave up
    }
}

// Sends one request (caller holds `busy`). Returns true when the worker finished within the wait; false = abandoned
// (the worker clears `busy` when it is done).
bool runRequest(DWORD timeoutMs, const std::atomic<bool>* cancel) {
    InterlockedExchange(&g.state, Requested);
    ResetEvent(g.doneEvent);
    SetEvent(g.reqEvent);
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const ULONGLONG el = GetTickCount64() - t0;
        const DWORD slice = cancel ? 100 : timeoutMs;
        const DWORD left = el >= timeoutMs ? 0 : DWORD(timeoutMs - el);
        if (WaitForSingleObject(g.doneEvent, left < slice ? left : slice) == WAIT_OBJECT_0) break;
        if (left == 0 || (cancel && cancel->load())) {
            if (InterlockedCompareExchange(&g.state, Abandoned, Requested) == Requested) return false;
            WaitForSingleObject(g.doneEvent, INFINITE);   // it finished just now
            break;
        }
    }
    InterlockedExchange(&g.state, Idle);
    return true;
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    if (g.ready && GetCurrentThreadId() != g.workerTid) {
        if (InterlockedExchange(&g.crashEntered, 1) == 0) {
            // A hang dump in progress finishes first (bounded); then this thread's exception is written.
            bool got = false;
            for (int i = 0; i < 200 && !(got = InterlockedCompareExchange(&g.busy, 1, 0) == 0); ++i) Sleep(50);
            if (got) {
                g.crash = true; g.ep = ep; g.tid = GetCurrentThreadId();
                g.reason[0] = 0; strcpy_s(g.reason, "crash");
                if (runRequest(30000, nullptr)) InterlockedExchange(&g.busy, 0);
            }
            SetEvent(g.crashDone);
        } else {
            WaitForSingleObject(g.crashDone, 30000);   // another thread is writing the crash dump: let it finish
        }
    }
    return g.previousFilter ? g.previousFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

bool init(const Options& o) {
    if (!g.dbghelp) {
        g.dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!g.dbghelp) return false;
        g.writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(g.dbghelp, "MiniDumpWriteDump"));
        g.symSetOptions = reinterpret_cast<SymSetOptionsFn>(GetProcAddress(g.dbghelp, "SymSetOptions"));
        g.symInitialize = reinterpret_cast<SymInitializeWFn>(GetProcAddress(g.dbghelp, "SymInitializeW"));
        g.symRefresh = reinterpret_cast<SymRefreshModuleListFn>(GetProcAddress(g.dbghelp, "SymRefreshModuleList"));
        g.symCleanup = reinterpret_cast<SymCleanupFn>(GetProcAddress(g.dbghelp, "SymCleanup"));
        g.symFromAddr = reinterpret_cast<SymFromAddrWFn>(GetProcAddress(g.dbghelp, "SymFromAddrW"));
        g.symGetLine = reinterpret_cast<SymGetLineFromAddrW64Fn>(GetProcAddress(g.dbghelp, "SymGetLineFromAddrW64"));
        if (!g.writeDump) return false;
        g.dbgLock = CreateMutexW(nullptr, FALSE, nullptr);
        g.reqEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g.doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g.crashDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // 256 KB stack: MiniDumpWriteDump needs some; the thread lives for the whole process.
        g.worker = CreateThread(nullptr, 256 * 1024, workerMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, &g.workerTid);
        if (!g.worker) return false;
    }
    // Configuration changes only between requests.
    for (int i = 0; i < 100 && InterlockedCompareExchange(&g.busy, 1, 0) != 0; ++i) Sleep(10);
    const std::wstring d = QDir::toNativeSeparators(QDir::cleanPath(o.dir)).toStdWString();
    const std::wstring n = sanitizedName(o.name).toStdWString();
    g.dir[0] = 0; wappend(g.dir, 1024, d.c_str());
    g.name[0] = 0; wappend(g.name, 64, n.c_str());
    g.maxPerHour = std::max(0, o.maxPerHour);
    g.keep = std::max(1, o.keep);
    g.ready = g.dir[0] != 0;
    InterlockedExchange(&g.busy, 0);
    return g.ready;
}

bool isReady() { return g.ready; }

QString writeDump(const char* reason, int timeoutMs, QString* error, const std::atomic<bool>* cancel) {
    auto fail = [error](const QString& e) { if (error) *error = e; return QString(); };
    if (!g.ready) return fail(QStringLiteral("not initialised"));
    if (InterlockedCompareExchange(&g.busy, 1, 0) != 0) return fail(QStringLiteral("busy (another dump is being written)"));
    g.crash = false; g.ep = nullptr; g.tid = 0;
    strncpy_s(g.reason, reason && *reason ? reason : "manual", _TRUNCATE);
    if (!runRequest(DWORD(std::max(1, timeoutMs)), cancel)) return fail(QStringLiteral("timeout"));
    const int r = g.result; const DWORD err = g.error;
    const QString path = QString::fromWCharArray(g.path);
    InterlockedExchange(&g.busy, 0);
    if (r == RateLimited) return fail(QStringLiteral("rate limit (%1 per hour)").arg(g.maxPerHour));
    if (r != Ok) return fail(QStringLiteral("MiniDumpWriteDump failed (error %1)").arg(err));
    if (error) error->clear();
    return path;
}

void installCrashHandler() {
    if (!g.ready || g.crashInstalled) return;
    g.crashInstalled = true;
    g.previousFilter = SetUnhandledExceptionFilter(crashFilter);
}

std::vector<std::string> symbolize(const quint64* pcs, int count) {
    std::vector<std::string> out;
    if (!pcs || count <= 0) return out;
    out.reserve(size_t(count));
    const bool haveSym = g.dbghelp && g.symInitialize && g.symFromAddr && g.dbgLock;
    const bool locked = haveSym && lockDbgHelp(15000);
    if (locked && !g.symReady) {
        // A handle of our own (DbgHelp keys sessions by handle; another library may own GetCurrentProcess()'s).
        DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &g.symProcess, 0, FALSE, DUPLICATE_SAME_ACCESS);
        if (g.symSetOptions)
            g.symSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                            SYMOPT_NO_PROMPTS | SYMOPT_IGNORE_NT_SYMPATH);
        wchar_t exeDir[1024] = {};
        GetModuleFileNameW(nullptr, exeDir, 1023);
        if (wchar_t* slash = wcsrchr(exeDir, L'\\')) *slash = 0;
        g.symReady = g.symInitialize(g.symProcess, exeDir, TRUE) != FALSE;
    } else if (locked && g.symRefresh) {
        g.symRefresh(g.symProcess);                       // modules loaded since the last lookup
    }
    alignas(SYMBOL_INFOW) unsigned char symBuf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)];
    for (int i = 0; i < count; ++i) {
        const DWORD64 pc = pcs[i];
        const DWORD64 look = i > 0 && pc ? pc - 1 : pc;
        std::string mod = "?";
        DWORD64 modBase = 0;
        HMODULE hm = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(look), &hm) && hm) {
            wchar_t mp[MAX_PATH] = {};
            GetModuleFileNameW(hm, mp, MAX_PATH - 1);
            const wchar_t* base = wcsrchr(mp, L'\\');
            std::wstring b = base ? base + 1 : mp;
            const size_t dot = b.rfind(L'.');
            if (dot != std::wstring::npos) b.resize(dot);
            mod = utf8(b.c_str());
            modBase = DWORD64(reinterpret_cast<quintptr>(hm));
        }
        char tmp[64];
        std::string line;
        bool named = false;
        if (locked && g.symReady) {
            auto* si = reinterpret_cast<SYMBOL_INFOW*>(symBuf);
            std::memset(symBuf, 0, sizeof(symBuf));
            si->SizeOfStruct = sizeof(SYMBOL_INFOW);
            si->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (g.symFromAddr(g.symProcess, look, &disp, si)) {
                std::snprintf(tmp, sizeof(tmp), "+0x%llx", static_cast<unsigned long long>(pc - si->Address));
                line = mod + "!" + utf8(si->Name) + tmp;
                named = true;
                IMAGEHLP_LINEW64 li{}; li.SizeOfStruct = sizeof(li);
                DWORD ld = 0;
                if (g.symGetLine && g.symGetLine(g.symProcess, look, &ld, &li) && li.FileName) {
                    const wchar_t* fb = wcsrchr(li.FileName, L'\\');
                    std::snprintf(tmp, sizeof(tmp), ":%lu)", static_cast<unsigned long>(li.LineNumber));
                    line += " (" + utf8(fb ? fb + 1 : li.FileName) + tmp;
                }
            }
        }
        if (!named) {
            std::snprintf(tmp, sizeof(tmp), modBase ? "+0x%llx" : "0x%llx",
                          static_cast<unsigned long long>(modBase ? pc - modBase : pc));
            line = (modBase ? mod : std::string()) + tmp;
        }
        out.push_back(line);
    }
    // Loaded PDBs cost tens of MB (the Supervisor's: about 55 MB private) for as long as the session lives. Hangs are
    // rare and memory on the playout machine is not, so the session is dropped after each lookup and rebuilt next time.
    if (locked && g.symReady && g.symCleanup) { g.symCleanup(g.symProcess); g.symReady = false; }
    if (locked) unlockDbgHelp();
    return out;
}

#else   // not Windows

bool init(const Options&) { return false; }
bool isReady() { return false; }
QString writeDump(const char*, int, QString* error, const std::atomic<bool>*) {
    if (error) *error = QStringLiteral("minidumps are implemented for Windows only");
    return {};
}
void installCrashHandler() {}
std::vector<std::string> symbolize(const quint64* pcs, int count) {
    std::vector<std::string> out;
    for (int i = 0; pcs && i < count; ++i) { char b[32]; std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(pcs[i])); out.push_back(b); }
    return out;
}

#endif

} // namespace minidump
