#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#endif
#include "incident.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>

namespace ppwatch {

QString sanitizedName(const QString& name) {
    QString s;
    for (const QChar c : name) s += (c.isLetterOrNumber() && c.unicode() < 128) || c == '.' || c == '_' || c == '-' ? c : QChar('_');
    return s.isEmpty() ? QStringLiteral("target") : s.left(48);
}

QString utcStamp(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, QTimeZone(QTimeZone::UTC)).toString(QStringLiteral("yyyyMMdd'T'HHmmss")) + 'Z';
}

bool sameImage(const QString& image, const QString& expected) {
    return !image.isEmpty() && QFileInfo(image).fileName().compare(QFileInfo(expected).fileName(), Qt::CaseInsensitive) == 0;
}

bool isProtectedProcess(quint32 pid, const QString& image) {
#ifdef _WIN32
    if (pid == GetCurrentProcessId()) return true;
#endif
    if (pid <= 4 || image.isEmpty()) return true;   // idle/System, or nothing confirms what it is
    static const QStringList never = {
        "System", "Registry", "smss.exe", "csrss.exe", "wininit.exe", "winlogon.exe", "services.exe", "lsass.exe",
        "lsaiso.exe", "svchost.exe", "fontdrvhost.exe", "dwm.exe", "explorer.exe", "MsMpEng.exe", "conhost.exe",
        "OpenConsole.exe", "WindowsTerminal.exe", "sihost.exe", "taskhostw.exe", "spoolsv.exe"};
    for (const QString& n : never) if (sameImage(image, n)) return true;
    return false;
}

QString threadStateName(int s) {
    static const char* const n[] = {"Initialized", "Ready", "Running", "Standby", "Terminated", "Waiting", "Transition",
                                    "DeferredReady", "GateWait", "WaitingForProcessInSwap"};
    return s >= 0 && s < int(sizeof(n) / sizeof(n[0])) ? QString::fromLatin1(n[s]) : QStringLiteral("state%1").arg(s);
}

QString waitReasonName(int r) {
    static const char* const n[] = {
        "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended", "UserRequest",
        "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution", "WrSuspended", "WrUserRequest",
        "WrEventPair", "WrQueue", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous",
        "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl", "WrCalloutStack", "WrKernel",
        "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted", "WrYieldExecution",
        "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt", "WrPhysicalFault",
        "WrIoRing", "WrMdlCache", "WrRcu"};
    return r >= 0 && r < int(sizeof(n) / sizeof(n[0])) ? QString::fromLatin1(n[r]) : QStringLiteral("reason%1").arg(r);
}

const ProcInfo* findProcess(const std::vector<ProcInfo>& procs, quint32 pid) {
    for (const ProcInfo& p : procs) if (p.pid == pid) return &p;
    return nullptr;
}

QString TcpRow::stateName() const {
    static const char* const n[] = {"?", "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT1",
                                    "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT", "DELETE_TCB"};
    return state > 0 && state < int(sizeof(n) / sizeof(n[0])) ? QString::fromLatin1(n[state]) : QStringLiteral("state%1").arg(state);
}

quint32 listenerPid(const std::vector<TcpRow>& rows, quint16 port) {
    quint32 any = 0;
    for (const TcpRow& r : rows) {
        if (r.state != 2 || r.localPort != port) continue;
        if (r.localAddr.startsWith(QLatin1String("127.")) || r.localAddr == QLatin1String("::1")) return r.pid;
        if (r.localAddr == QLatin1String("0.0.0.0") || r.localAddr == QLatin1String("::")) any = any ? any : r.pid;
    }
    return any;
}

QStringList splitCommandLine(const QString& cmd) {
    QStringList out;
#ifdef _WIN32
    if (cmd.trimmed().isEmpty()) return out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(cmd.utf16()), &argc);
    if (!argv) return out;
    for (int i = 0; i < argc; ++i) out << QString::fromWCharArray(argv[i]);
    LocalFree(argv);
#else
    out = cmd.split(' ', Qt::SkipEmptyParts);
#endif
    return out;
}

QString redactCommandLine(const QString& cmd) {
    static const QRegularExpression secret(QStringLiteral("(key|token|secret|passw|pwd|auth|credential|passphrase|cookie)"),
                                           QRegularExpression::CaseInsensitiveOption);
    const QStringList a = splitCommandLine(cmd);
    QStringList out;
    bool hideNext = false;
    for (const QString& s : a) {
        QString v = s;
        if (hideNext) { v = QStringLiteral("(hidden)"); hideNext = false; }
        else if (s.startsWith('-') || s.startsWith('/')) {
            int eq = s.indexOf('=');
            if (eq < 0 && s.startsWith('/')) eq = s.indexOf(':');       // /token:value
            const QString name = eq < 0 ? s : s.left(eq);
            if (secret.match(name).hasMatch()) {
                if (eq >= 0) v = name + s.at(eq) + QStringLiteral("(hidden)");
                else hideNext = true;
            }
        }
        out << (v.contains(' ') || v.isEmpty() ? '"' + v + '"' : v);
    }
    return out.join(' ');
}

EngineArgs parseEngineArgs(const QString& cmd) {
    EngineArgs e;
    const QStringList a = splitCommandLine(cmd);
    auto opt = [](const QString& s, const char* name, QString* inlineValue) {
        const QString n = QString::fromLatin1(name);
        if (s == n) return true;
        if (s.startsWith(n + '=')) { *inlineValue = s.mid(n.size() + 1); return true; }
        return false;
    };
    for (int i = 0; i < a.size(); ++i) {
        QString iv;
        const bool hasNext = i + 1 < a.size();
        if (a[i] == QLatin1String("--standalone")) e.standalone = true;
        else if (opt(a[i], "--id", &iv)) e.id = iv.isEmpty() && hasNext ? a[++i] : iv;
        else if (opt(a[i], "--port", &iv)) e.port = (iv.isEmpty() && hasNext ? a[++i] : iv).toInt();
        else if (opt(a[i], "--log-dir", &iv)) e.logDir = iv.isEmpty() && hasNext ? a[++i] : iv;
    }
    return e;
}

std::vector<Orphan> findOrphans(const std::vector<ProcInfo>& procs, const QString& engineExe, const QString& supervisorExe) {
    std::vector<Orphan> out;
    for (const ProcInfo& p : procs) {
        if (!sameImage(p.image, engineExe)) continue;
        const ProcInfo* parent = findProcess(procs, p.ppid);
        QString why;
        if (!parent) why = QStringLiteral("parent pid %1 is gone").arg(p.ppid);
        else if (parent->createTime > p.createTime) why = QStringLiteral("parent pid %1 now belongs to a newer process (%2)").arg(p.ppid).arg(parent->image);
        else if (!sameImage(parent->image, supervisorExe)) why = QStringLiteral("parent is %1, not %2").arg(parent->image, supervisorExe);
        if (!why.isEmpty()) out.push_back(Orphan{p.pid, p.ppid, why});
    }
    return out;
}

QString newestMatch(const QString& glob) {
    const QFileInfo fi(QDir::fromNativeSeparators(glob));
    const QString pattern = fi.fileName();
    if (!pattern.contains('*') && !pattern.contains('?')) return fi.isFile() ? fi.absoluteFilePath() : QString();
    const QFileInfoList l = QDir(fi.absolutePath()).entryInfoList({pattern}, QDir::Files, QDir::Time);
    return l.isEmpty() ? QString() : l.first().absoluteFilePath();
}

// --- incident folders -------------------------------------------------------------------------------------------------

namespace {

const QRegularExpression& incidentDirRe() {
    static const QRegularExpression re(QStringLiteral("^(\\d{8}T\\d{6})Z-(.+)-(\\d+)$"));
    return re;
}

} // namespace

int recentIncidents(const QString& root, const QString& target, qint64 withinSecs, bool withDump, qint64 nowMs) {
    const qint64 now = nowMs ? nowMs : QDateTime::currentMSecsSinceEpoch();
    const QString name = sanitizedName(target);
    int n = 0;
    for (const QFileInfo& fi : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QRegularExpressionMatch m = incidentDirRe().match(fi.fileName());
        if (!m.hasMatch() || m.captured(2) != name) continue;
        const QString s = m.captured(1);
        const QDateTime t(QDate::fromString(s.left(8), QStringLiteral("yyyyMMdd")), QTime::fromString(s.mid(9), QStringLiteral("HHmmss")),
                          QTimeZone(QTimeZone::UTC));
        if (!t.isValid() || now - t.toMSecsSinceEpoch() > withinSecs * 1000) continue;
        if (withDump && QDir(fi.absoluteFilePath()).entryList({"*.dmp"}, QDir::Files).isEmpty()) continue;
        ++n;
    }
    return n;
}

int pruneIncidents(const QString& root, int keep) {
    QStringList ours;
    for (const QString& d : QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (incidentDirRe().match(d).hasMatch()) ours << d;    // the stamp leads, so name order is time order
    int removed = 0;
    while (ours.size() > std::max(1, keep)) {
        if (QDir(QDir(root).filePath(ours.takeFirst())).removeRecursively()) ++removed;
    }
    return removed;
}

#ifdef _WIN32

namespace {

// SystemProcessInformation (5) record layouts: winternl.h only declares a truncated SYSTEM_PROCESS_INFORMATION and no
// thread records. These match the documented x64 layout (sizes asserted below).
struct PPUnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
struct PPSystemThreadInformation {
    LARGE_INTEGER KernelTime, UserTime, CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    struct { HANDLE UniqueProcess; HANDLE UniqueThread; } ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};
struct PPSystemProcessInformation {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    PPUnicodeString ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
        QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount,
        WriteTransferCount, OtherTransferCount;
};
#ifdef _WIN64
static_assert(sizeof(PPSystemThreadInformation) == 80, "SYSTEM_THREAD_INFORMATION layout");
static_assert(sizeof(PPSystemProcessInformation) == 256, "SYSTEM_PROCESS_INFORMATION layout");
#endif

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

constexpr LONG kStatusInfoLengthMismatch = LONG(0xC0000004);
constexpr LONG kStatusBufferTooSmall = LONG(0xC0000023);
constexpr LONG kStatusBufferOverflow = LONG(0x80000005);

template <typename F> F ntdll(const char* name) {
    return reinterpret_cast<F>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}

qint64 li(const LARGE_INTEGER& v) { return qint64(v.QuadPart); }
qint64 ft(const FILETIME& f) { return qint64((ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime); }

QString fileTimeText(qint64 t) {
    if (t <= 0) return QString();
    // FILETIME (100 ns since 1601) -> ms since 1970
    return QDateTime::fromMSecsSinceEpoch((t - 116444736000000000LL) / 10000, QTimeZone(QTimeZone::UTC)).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE x = nullptr) : h(x) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

QString imagePathOf(HANDLE h) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = DWORD(sizeof(buf) / sizeof(buf[0]));
    return QueryFullProcessImageNameW(h, 0, buf, &n) ? QString::fromWCharArray(buf, int(n)) : QString();
}

qint64 createTimeOf(HANDLE h) {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(h, &c, &e, &k, &u) ? ft(c) : 0;
}

struct ModuleRange { quint64 base = 0, size = 0; QString name; };

std::vector<ModuleRange> modulesOf(quint32 pid) {
    std::vector<ModuleRange> out;
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid));
    if (!h) return out;
    std::vector<HMODULE> mods(1024);
    DWORD needed = 0;
    if (!EnumProcessModulesEx(h.h, mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) return out;
    mods.resize(std::min<size_t>(mods.size(), needed / sizeof(HMODULE)));
    for (HMODULE m : mods) {
        MODULEINFO mi{};
        wchar_t name[MAX_PATH] = {};
        if (!GetModuleInformation(h.h, m, &mi, sizeof(mi))) continue;
        GetModuleBaseNameW(h.h, m, name, MAX_PATH);
        QString n = QString::fromWCharArray(name);
        const int dot = n.lastIndexOf('.');
        if (dot > 0) n.truncate(dot);
        out.push_back(ModuleRange{quint64(reinterpret_cast<quintptr>(mi.lpBaseOfDll)), mi.SizeOfImage, n});
    }
    return out;
}

QString addressText(quint64 a, const std::vector<ModuleRange>& mods) {
    if (!a) return QString();
    for (const ModuleRange& m : mods)
        if (a >= m.base && a < m.base + m.size) return QStringLiteral("%1+0x%2").arg(m.name).arg(a - m.base, 0, 16);
    return QStringLiteral("0x%1").arg(a, 0, 16);
}

bool writeText(const QString& path, const QByteArray& data, QString* error = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { if (error) *error = f.errorString(); return false; }
    f.write(data);
    return true;
}

} // namespace

std::vector<ProcInfo> processSnapshot(bool withThreads, QString* error) {
    std::vector<ProcInfo> out;
    static const auto query = ntdll<NtQuerySystemInformationFn>("NtQuerySystemInformation");
    if (!query) { if (error) *error = QStringLiteral("NtQuerySystemInformation not found"); return out; }
    ULONG size = 2u * 1024 * 1024;
    std::unique_ptr<ULONGLONG[]> buf;   // 8-byte aligned
    LONG st = kStatusInfoLengthMismatch;
    for (int tries = 0; tries < 6 && st == kStatusInfoLengthMismatch; ++tries) {
        buf.reset(new ULONGLONG[size / 8 + 1]);
        ULONG needed = 0;
        st = query(5 /* SystemProcessInformation */, buf.get(), size, &needed);
        if (st == kStatusInfoLengthMismatch) size = std::max<ULONG>(needed, size) + 512u * 1024;
    }
    if (st < 0) { if (error) *error = QStringLiteral("NtQuerySystemInformation failed (0x%1)").arg(quint32(st), 8, 16, QChar('0')); return out; }
    const auto* base = reinterpret_cast<const unsigned char*>(buf.get());
    size_t off = 0;
    for (;;) {
        const auto* p = reinterpret_cast<const PPSystemProcessInformation*>(base + off);
        ProcInfo pi;
        pi.pid = quint32(reinterpret_cast<quintptr>(p->UniqueProcessId));
        pi.ppid = quint32(reinterpret_cast<quintptr>(p->InheritedFromUniqueProcessId));
        pi.image = p->ImageName.Buffer ? QString::fromWCharArray(p->ImageName.Buffer, p->ImageName.Length / 2)
                                       : (pi.pid == 0 ? QStringLiteral("Idle") : QString());
        pi.createTime = li(p->CreateTime);
        pi.threadCount = p->NumberOfThreads;
        pi.handles = p->HandleCount;
        pi.sessionId = p->SessionId;
        pi.privateBytes = p->PrivatePageCount;
        pi.workingSet = p->WorkingSetSize;
        pi.userTime = li(p->UserTime);
        pi.kernelTime = li(p->KernelTime);
        if (withThreads) {
            const auto* t = reinterpret_cast<const PPSystemThreadInformation*>(p + 1);
            pi.threads.reserve(p->NumberOfThreads);
            for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
                ThreadInfo ti;
                ti.tid = quint32(reinterpret_cast<quintptr>(t[i].ClientId.UniqueThread));
                ti.state = int(t[i].ThreadState);
                ti.waitReason = int(t[i].WaitReason);
                ti.createTime = li(t[i].CreateTime);
                ti.userTime = li(t[i].UserTime);
                ti.kernelTime = li(t[i].KernelTime);
                ti.contextSwitches = t[i].ContextSwitches;
                ti.priority = int(t[i].Priority);
                ti.startAddress = quint64(reinterpret_cast<quintptr>(t[i].StartAddress));
                pi.threads.push_back(ti);
            }
        }
        out.push_back(std::move(pi));
        if (!p->NextEntryOffset) break;
        off += p->NextEntryOffset;
    }
    if (error) error->clear();
    return out;
}

QJsonArray describeThreads(const ProcInfo& p, QString* text) {
    static const auto queryThread = ntdll<NtQueryInformationThreadFn>("NtQueryInformationThread");
    static const auto getDesc = reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    const std::vector<ModuleRange> mods = modulesOf(p.pid);
    // The process's first thread is its main thread for every program here (Qt's event loop runs on it).
    quint32 firstTid = 0; qint64 firstCreate = 0;
    for (const ThreadInfo& t : p.threads)
        if (!firstTid || (t.createTime && t.createTime < firstCreate)) { firstTid = t.tid; firstCreate = t.createTime; }
    std::vector<ThreadInfo> order = p.threads;
    std::stable_sort(order.begin(), order.end(), [firstTid](const ThreadInfo& a, const ThreadInfo& b) {
        return (a.tid == firstTid) > (b.tid == firstTid);
    });
    QJsonArray arr;
    QStringList rows;
    rows << QStringLiteral("%1 %2 %3 %4 %5 %6 %7  %8  %9")
                .arg("tid", -7).arg("state", -9).arg("waitReason", -18).arg("userMs", 9).arg("kernelMs", 9)
                .arg("switches", 10).arg("prio", 4).arg("start", -40).arg("name");
    for (const ThreadInfo& t : order) {
        quint64 start = 0;
        QString name;
        Handle th(OpenThread(THREAD_QUERY_INFORMATION, FALSE, t.tid));
        if (!th) th.h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, t.tid);
        if (th) {
            PVOID a = nullptr;
            if (queryThread && queryThread(th.h, 9 /* ThreadQuerySetWin32StartAddress */, &a, sizeof(a), nullptr) >= 0)
                start = quint64(reinterpret_cast<quintptr>(a));
            PWSTR d = nullptr;
            if (getDesc && SUCCEEDED(getDesc(th.h, &d)) && d) { name = QString::fromWCharArray(d); LocalFree(d); }
        }
        if (!start) start = t.startAddress;
        const QString startText = addressText(start, mods);
        const bool waiting = t.state == 5;
        QJsonObject o{{"tid", double(t.tid)}, {"state", threadStateName(t.state)},
                      {"waitReason", waiting ? waitReasonName(t.waitReason) : QString()},
                      {"userMs", double(t.userTime / 10000)}, {"kernelMs", double(t.kernelTime / 10000)},
                      {"contextSwitches", double(t.contextSwitches)}, {"priority", t.priority},
                      {"start", startText}, {"name", name}, {"created", fileTimeText(t.createTime)}};
        if (t.tid == firstTid) o.insert(QStringLiteral("first"), true);
        arr.append(o);
        rows << QStringLiteral("%1 %2 %3 %4 %5 %6 %7  %8  %9")
                    .arg(t.tid, -7).arg(threadStateName(t.state), -9).arg(waiting ? waitReasonName(t.waitReason) : QString("-"), -18)
                    .arg(t.userTime / 10000, 9).arg(t.kernelTime / 10000, 9).arg(t.contextSwitches, 10).arg(t.priority, 4)
                    .arg(startText, -40).arg(name + (t.tid == firstTid ? QStringLiteral(" (first thread)") : QString()));
    }
    if (text) *text = rows.join('\n') + '\n';
    return arr;
}

std::vector<TcpRow> tcpTable(QString* error) {
    std::vector<TcpRow> out;
    auto fetch = [&](ULONG family) {
        DWORD size = 64 * 1024;
        std::unique_ptr<ULONGLONG[]> buf;
        DWORD rc = ERROR_INSUFFICIENT_BUFFER;
        for (int tries = 0; tries < 5 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
            buf.reset(new ULONGLONG[size / 8 + 1]);
            rc = GetExtendedTcpTable(buf.get(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
            if (rc == ERROR_INSUFFICIENT_BUFFER) size += 16 * 1024;
        }
        if (rc != NO_ERROR) { if (error) *error = QStringLiteral("GetExtendedTcpTable failed (%1)").arg(rc); return; }
        char addr[INET6_ADDRSTRLEN] = {};
        if (family == AF_INET) {
            const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.get());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCPROW_OWNER_PID& r = t->table[i];
                TcpRow x;
                in_addr a{}; a.s_addr = r.dwLocalAddr;
                inet_ntop(AF_INET, &a, addr, sizeof(addr)); x.localAddr = QString::fromLatin1(addr);
                a.s_addr = r.dwRemoteAddr;
                inet_ntop(AF_INET, &a, addr, sizeof(addr)); x.remoteAddr = QString::fromLatin1(addr);
                x.localPort = ntohs(u_short(r.dwLocalPort)); x.remotePort = ntohs(u_short(r.dwRemotePort));
                x.state = int(r.dwState); x.pid = r.dwOwningPid;
                out.push_back(x);
            }
        } else {
            const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.get());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const MIB_TCP6ROW_OWNER_PID& r = t->table[i];
                TcpRow x; x.v6 = true;
                inet_ntop(AF_INET6, r.ucLocalAddr, addr, sizeof(addr)); x.localAddr = QString::fromLatin1(addr);
                inet_ntop(AF_INET6, r.ucRemoteAddr, addr, sizeof(addr)); x.remoteAddr = QString::fromLatin1(addr);
                x.localPort = ntohs(u_short(r.dwLocalPort)); x.remotePort = ntohs(u_short(r.dwRemotePort));
                x.state = int(r.dwState); x.pid = r.dwOwningPid;
                out.push_back(x);
            }
        }
    };
    if (error) error->clear();
    fetch(AF_INET);
    fetch(AF_INET6);
    return out;
}

ProcessId queryProcess(quint32 pid) {
    ProcessId r; r.pid = pid;
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
    if (!h) return r;
    r.alive = WaitForSingleObject(h.h, 0) == WAIT_TIMEOUT;
    r.createTime = createTimeOf(h.h);
    r.path = imagePathOf(h.h);
    r.image = QFileInfo(r.path).fileName();
    return r;
}

QString commandLine(quint32 pid, QString* error) {
    static const auto query = ntdll<NtQueryInformationProcessFn>("NtQueryInformationProcess");
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h || !query) { if (error) *error = QStringLiteral("cannot open pid %1 (%2)").arg(pid).arg(GetLastError()); return {}; }
    ULONG size = 8192;
    for (int tries = 0; tries < 4; ++tries) {
        std::unique_ptr<ULONGLONG[]> buf(new ULONGLONG[size / 8 + 1]);
        ULONG needed = 0;
        const LONG st = query(h.h, 60 /* ProcessCommandLineInformation */, buf.get(), size, &needed);
        if (st == kStatusInfoLengthMismatch || st == kStatusBufferTooSmall || st == kStatusBufferOverflow) {
            size = std::max<ULONG>(needed, size * 2) + 64;
            continue;
        }
        if (st < 0) break;
        const auto* us = reinterpret_cast<const PPUnicodeString*>(buf.get());
        if (error) error->clear();
        return us->Buffer ? QString::fromWCharArray(us->Buffer, us->Length / 2) : QString();
    }
    if (error) *error = QStringLiteral("cannot read the command line of pid %1").arg(pid);
    return {};
}

QStringList tailLines(const QString& path, int n, QString* error, qint64 maxBytes) {
    QStringList out;
    if (n <= 0) return out;
    // Full sharing: the logging process keeps appending, rotating and pruning while we read.
    Handle h(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) { if (error) *error = QStringLiteral("cannot open (%1)").arg(GetLastError()); return out; }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h.h, &size)) { if (error) *error = QStringLiteral("cannot size (%1)").arg(GetLastError()); return out; }
    qint64 pos = size.QuadPart, read = 0;
    QByteArray data;
    int newlines = 0;
    while (pos > 0 && read < maxBytes && newlines <= n) {
        const qint64 chunk = std::min<qint64>({64 * 1024, pos, maxBytes - read});
        pos -= chunk;
        LARGE_INTEGER at; at.QuadPart = pos;
        QByteArray part(int(chunk), '\0');
        DWORD got = 0;
        if (!SetFilePointerEx(h.h, at, nullptr, FILE_BEGIN) || !ReadFile(h.h, part.data(), DWORD(chunk), &got, nullptr)) break;
        part.truncate(int(got));
        newlines += int(part.count('\n'));
        data.prepend(part);
        read += chunk;
    }
    if (data.endsWith('\n')) data.chop(1);
    QList<QByteArray> lines = data.split('\n');
    if (pos > 0 && !lines.isEmpty()) lines.removeFirst();             // we started mid-line
    const int from = std::max(0, int(lines.size()) - n);
    for (int i = from; i < lines.size(); ++i) {
        QByteArray l = lines[i];
        if (l.endsWith('\r')) l.chop(1);
        out << QString::fromUtf8(l);
    }
    if (error) error->clear();
    return out;
}

bool dumpProcess(quint32 pid, qint64 expectCreateTime, const QString& path, QString* error, qint64* ms) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    static HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static const auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!writeDump) return fail(QStringLiteral("dbghelp.dll (System32) has no MiniDumpWriteDump"));
    // Handle data (which handle each thread waits on) needs PROCESS_DUP_HANDLE; without it the dump still goes ahead.
    // SYNCHRONIZE: the "has it exited" check below waits on the handle.
    bool withHandles = true;
    Handle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid));
    if (!h) { withHandles = false; h.h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid); }
    if (!h) return fail(QStringLiteral("cannot open pid %1 for a dump (error %2)").arg(pid).arg(GetLastError()));
    if (WaitForSingleObject(h.h, 0) != WAIT_TIMEOUT) return fail(QStringLiteral("pid %1 has exited").arg(pid));
    if (expectCreateTime && createTimeOf(h.h) != expectCreateTime)
        return fail(QStringLiteral("pid %1 now belongs to another process (create time changed)").arg(pid));
    if (isProtectedProcess(pid, QFileInfo(imagePathOf(h.h)).fileName()))
        return fail(QStringLiteral("pid %1 is a protected system process").arg(pid));
    QDir().mkpath(QFileInfo(path).absolutePath());
    Handle f(CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f) return fail(QStringLiteral("cannot create %1 (error %2)").arg(path).arg(GetLastError()));
    const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                             MiniDumpWithIndirectlyReferencedMemory | (withHandles ? MiniDumpWithHandleData : 0));
    QElapsedTimer t; t.start();
    const BOOL ok = writeDump(h.h, pid, f.h, type, nullptr, nullptr, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    if (ms) *ms = t.elapsed();
    CloseHandle(f.h); f.h = nullptr;
    if (!ok) { DeleteFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16())); return fail(QStringLiteral("MiniDumpWriteDump failed (0x%1)").arg(err, 8, 16, QChar('0'))); }
    if (error) error->clear();
    return true;
}

bool terminateProcess(quint32 pid, qint64 expectCreateTime, const QString& expectImage, quint32 exitCode, int waitMs, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    Handle h(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return fail(QStringLiteral("cannot open pid %1 (error %2)").arg(pid).arg(GetLastError()));
    const QString image = QFileInfo(imagePathOf(h.h)).fileName();
    if (isProtectedProcess(pid, image)) return fail(QStringLiteral("pid %1 (%2) is protected").arg(pid).arg(image));
    if (expectCreateTime && createTimeOf(h.h) != expectCreateTime) return fail(QStringLiteral("pid %1 now belongs to another process").arg(pid));
    if (!expectImage.isEmpty() && !sameImage(image, expectImage)) return fail(QStringLiteral("pid %1 is %2, not %3").arg(pid).arg(image, expectImage));
    if (WaitForSingleObject(h.h, 0) != WAIT_TIMEOUT) return fail(QStringLiteral("pid %1 has already exited").arg(pid));
    if (!TerminateProcess(h.h, exitCode)) return fail(QStringLiteral("TerminateProcess failed (error %1)").arg(GetLastError()));
    if (WaitForSingleObject(h.h, DWORD(std::max(0, waitMs))) != WAIT_OBJECT_0) return fail(QStringLiteral("terminated, but still not gone after %1 ms").arg(waitMs));
    if (error) error->clear();
    return true;
}

quint32 launchDetached(const QString& cmd, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return quint32(0); };
    const QStringList args = splitCommandLine(cmd);
    if (args.isEmpty()) return fail(QStringLiteral("empty command"));
    const QFileInfo prog(args.first());
    const std::wstring workDir = prog.isFile() ? QDir::toNativeSeparators(prog.absolutePath()).toStdWString() : std::wstring();
    // NUL for stdin/stdout/stderr, and only those two handles are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the
    // relaunched program gets no console of ours, no pipe of ours, and none of our other handles.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle nulIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    Handle nulOut(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    if (!nulIn || !nulOut) return fail(QStringLiteral("cannot open NUL (%1)").arg(GetLastError()));
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::unique_ptr<unsigned char[]> attrBuf(new unsigned char[attrSize]);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.get());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) return fail(QStringLiteral("attribute list (%1)").arg(GetLastError()));
    HANDLE inherit[2] = {nulIn.h, nulOut.h};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        return fail(QStringLiteral("handle list (%1)").arg(GetLastError()));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nulIn.h;
    si.StartupInfo.hStdOutput = nulOut.h;
    si.StartupInfo.hStdError = nulOut.h;
    si.lpAttributeList = attrs;
    std::wstring line = cmd.toStdWString();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                   nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si.StartupInfo, &pi);
    const DWORD err = ok ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!ok) return fail(QStringLiteral("CreateProcess failed (error %1)").arg(err));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (error) error->clear();
    return pi.dwProcessId;
}

QString netstatText(const std::vector<TcpRow>& rows, const QList<quint16>& ports, quint32 alsoPid, const std::vector<ProcInfo>* procs) {
    const QSet<quint16> want(ports.begin(), ports.end());
    std::vector<TcpRow> sel;
    for (const TcpRow& r : rows)
        if (want.contains(r.localPort) || want.contains(r.remotePort) || (alsoPid && r.pid == alsoPid)) sel.push_back(r);
    std::sort(sel.begin(), sel.end(), [](const TcpRow& a, const TcpRow& b) {
        return a.localPort != b.localPort ? a.localPort < b.localPort : a.state != b.state ? a.state < b.state : a.remotePort < b.remotePort;
    });
    auto imageOf = [procs](quint32 pid) { const ProcInfo* p = procs ? findProcess(*procs, pid) : nullptr; return p ? p->image : QString(); };
    QStringList l;
    QStringList portText;
    for (quint16 p : ports) portText << QString::number(p);
    l << QStringLiteral("TCP connections on ports %1%2").arg(portText.join(',')).arg(alsoPid ? QStringLiteral(" and of pid %1").arg(alsoPid) : QString());
    l << QStringLiteral("%1 %2 %3 %4 %5  %6").arg("proto", -6).arg("local", -28).arg("remote", -28).arg("state", -12).arg("pid", 7).arg("image");
    QMap<quint16, QMap<QString, int>> counts;
    for (const TcpRow& r : sel) {
        auto ep = [&r](const QString& a, quint16 p) { return (r.v6 ? '[' + a + ']' : a) + ':' + QString::number(p); };
        l << QStringLiteral("%1 %2 %3 %4 %5  %6").arg(r.v6 ? "TCPv6" : "TCP", -6).arg(ep(r.localAddr, r.localPort), -28)
                 .arg(ep(r.remoteAddr, r.remotePort), -28).arg(r.stateName(), -12).arg(r.pid, 7).arg(imageOf(r.pid));
        if (want.contains(r.localPort)) counts[r.localPort][r.stateName()]++;
    }
    l << QString() << QStringLiteral("per local port:");
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        QStringList s;
        for (auto c = it.value().cbegin(); c != it.value().cend(); ++c) s << QStringLiteral("%1 %2").arg(c.key()).arg(c.value());
        l << QStringLiteral("  %1: %2").arg(it.key()).arg(s.join(QStringLiteral(", ")));
    }
    if (counts.isEmpty()) l << QStringLiteral("  (no rows on the watched ports)");
    return l.join('\n') + '\n';
}

IncidentResult writeIncident(const IncidentRequest& req) {
    IncidentResult res;
    QElapsedTimer total; total.start();
    const qint64 now = req.nowMs ? req.nowMs : QDateTime::currentMSecsSinceEpoch();
    const QString stamp = utcStamp(now);
    const QString name = sanitizedName(req.target);
    res.dir = QDir(req.root).filePath(QStringLiteral("%1-%2-%3").arg(stamp, name).arg(req.pid));
    if (!QDir().mkpath(res.dir + "/logs")) { res.error = QStringLiteral("cannot create ") + res.dir; return res; }
    QDir dir(res.dir);

    // 1. Threads first: the dump suspends every thread for a moment and would change what they wait on.
    QString snapErr;
    const std::vector<ProcInfo> procs = processSnapshot(true, &snapErr);
    const ProcInfo* p = findProcess(procs, req.pid);
    const bool sameProcess = p && (!req.createTime || p->createTime == req.createTime);
    QString threadsText;
    QJsonArray threads;
    QJsonObject process, firstThread;
    QMap<QString, int> stateCounts;
    if (sameProcess) {
        threads = describeThreads(*p, &threadsText);
        for (const QJsonValue& v : threads) {
            const QJsonObject o = v.toObject();
            const QString key = o.value("waitReason").toString().isEmpty() ? o.value("state").toString()
                                                                             : o.value("state").toString() + '/' + o.value("waitReason").toString();
            stateCounts[key]++;
            if (o.value("first").toBool()) firstThread = o;
        }
        const ProcInfo* parent = findProcess(procs, p->ppid);
        process = QJsonObject{{"pid", double(p->pid)}, {"ppid", double(p->ppid)},
                              {"parentImage", parent && parent->createTime <= p->createTime ? parent->image : QStringLiteral("(gone)")},
                              {"image", p->image}, {"created", fileTimeText(p->createTime)},
                              {"threads", double(p->threadCount)}, {"handles", double(p->handles)},
                              {"privateMB", double(p->privateBytes / 1048576.0)}, {"workingSetMB", double(p->workingSet / 1048576.0)},
                              {"cpuUserMs", double(p->userTime / 10000)}, {"cpuKernelMs", double(p->kernelTime / 10000)}};
    } else {
        threadsText = p ? QStringLiteral("pid %1 now belongs to another process (create time changed)\n").arg(req.pid)
                        : QStringLiteral("pid %1 is not running%2\n").arg(req.pid).arg(snapErr.isEmpty() ? QString() : " (" + snapErr + ")");
    }
    writeText(dir.filePath("threads.txt"),
              (QStringLiteral("%1 pid %2 (%3), %4 threads at %5\n\n").arg(req.target).arg(req.pid).arg(req.image)
                   .arg(threads.size()).arg(QDateTime::fromMSecsSinceEpoch(now).toString(Qt::ISODateWithMs)) + threadsText).toUtf8());

    // 2. The dump of this one process (verified by the caller; re-checked through the handle in dumpProcess).
    QString dumpFile;
    if (req.dump && sameProcess) {
        dumpFile = QStringLiteral("%1-%2-%3-hang.dmp").arg(name).arg(req.pid).arg(stamp);
        if (dumpProcess(req.pid, req.createTime, dir.filePath(dumpFile), &res.dumpError, &res.dumpMs)) res.dumpPath = dir.filePath(dumpFile);
        else dumpFile.clear();
    } else if (req.dump) {
        res.dumpError = QStringLiteral("the process is gone or was replaced");
    }

    // 3. Processes: PlayPlay*, the configured images, the target's parent and children, with the orphan check.
    {
        QStringList l;
        l << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8  %9").arg("pid", 7).arg("ppid", 7).arg("image", -28).arg("parent", -28)
                 .arg("created", -23).arg("threads", 7).arg("handles", 7).arg("privMB", 7).arg("command line (credentials masked)");
        const std::vector<Orphan> orphans = findOrphans(procs, req.engineExe, req.supervisorExe);
        QStringList orphanLines;
        for (const ProcInfo& q : procs) {
            const bool interesting = q.image.startsWith(QLatin1String("PlayPlay"), Qt::CaseInsensitive) || q.pid == req.pid ||
                                     (p && (q.ppid == req.pid || q.pid == p->ppid)) || sameImage(q.image, req.image) ||
                                     std::any_of(req.images.begin(), req.images.end(), [&q](const QString& i) { return sameImage(q.image, i); });
            if (!interesting || q.pid <= 4) continue;
            const ProcInfo* parent = findProcess(procs, q.ppid);
            const QString parentText = parent && parent->createTime <= q.createTime ? QStringLiteral("%1 (%2)").arg(parent->image).arg(parent->pid)
                                                                                    : QStringLiteral("(gone)");
            QString cmd = redactCommandLine(commandLine(q.pid));
            if (cmd.size() > 400) cmd = cmd.left(400) + QStringLiteral(" ...");
            l << QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8  %9").arg(q.pid, 7).arg(q.ppid, 7).arg(q.image, -28).arg(parentText, -28)
                     .arg(fileTimeText(q.createTime), -23).arg(q.threadCount, 7).arg(q.handles, 7)
                     .arg(QString::number(q.privateBytes / 1048576.0, 'f', 1), 7).arg(cmd);
        }
        for (const Orphan& o : orphans) orphanLines << QStringLiteral("  pid %1 (parent %2): %3").arg(o.pid).arg(o.ppid).arg(o.reason);
        l << QString() << QStringLiteral("orphan check (%1 whose parent is not a live %2; report only):").arg(req.engineExe, req.supervisorExe);
        l << (orphanLines.isEmpty() ? QStringList{QStringLiteral("  none")} : orphanLines);
        writeText(dir.filePath("processes.txt"), (l.join('\n') + '\n').toUtf8());
    }

    // 4. netstat for the watched ports and every socket of the target.
    QString tcpErr;
    const std::vector<TcpRow> tcp = tcpTable(&tcpErr);
    writeText(dir.filePath("netstat.txt"), (netstatText(tcp, req.ports, req.pid, &procs) + (tcpErr.isEmpty() ? QString() : tcpErr + '\n')).toUtf8());

    // 5. Log tails: the newest file for each glob.
    QJsonArray logs;
    QSet<QString> seen;
    for (const QString& g : req.logGlobs) {
        const QString src = newestMatch(g);
        QJsonObject o{{"glob", g}};
        if (src.isEmpty()) { o.insert("error", QStringLiteral("no file matches")); logs.append(o); continue; }
        if (seen.contains(src.toLower())) continue;
        seen.insert(src.toLower());
        QString err;
        const QStringList lines = tailLines(src, req.tailLines, &err);
        QString out = QFileInfo(src).fileName() + QStringLiteral(".tail.txt");
        for (int i = 2; QFileInfo::exists(dir.filePath("logs/" + out)); ++i) out = QStringLiteral("%1.%2.tail.txt").arg(QFileInfo(src).fileName()).arg(i);
        writeText(dir.filePath("logs/" + out), lines.isEmpty() ? QByteArray() : (lines.join('\n') + '\n').toUtf8());
        o.insert("source", src);
        o.insert("file", "logs/" + out);
        o.insert("lines", lines.size());
        o.insert("bytes", double(QFileInfo(src).size()));
        if (!err.isEmpty()) o.insert("error", err);
        logs.append(o);
    }

    // 6. incident.json
    const QJsonObject h = req.lastHealth;
    QString summary = QStringLiteral("%1 (pid %2, %3) %4").arg(req.target).arg(req.pid).arg(req.image.isEmpty() ? QStringLiteral("?") : req.image).arg(req.trigger);
    if (req.episodeMs > 0) summary += QStringLiteral(" for %1 s").arg(req.episodeMs / 1000.0, 0, 'f', 1);
    if (h.contains("lagMs")) summary += QStringLiteral("; event loop lag %1 ms").arg(qint64(h.value("lagMs").toDouble()));
    if (!h.value("activity").toString().isEmpty()) summary += QStringLiteral(" in '%1'").arg(h.value("activity").toString());
    if (!firstThread.isEmpty())
        summary += QStringLiteral("; first thread %1%2").arg(firstThread.value("state").toString())
                       .arg(firstThread.value("waitReason").toString().isEmpty() ? QString() : '/' + firstThread.value("waitReason").toString());
    if (!process.isEmpty())
        summary += QStringLiteral("; %1 threads, %2 handles, %3 MB private").arg(process.value("threads").toInt()).arg(process.value("handles").toInt())
                       .arg(process.value("privateMB").toDouble(), 0, 'f', 1);
    res.summary = summary;
    QJsonObject states;
    for (auto it = stateCounts.cbegin(); it != stateCounts.cend(); ++it) states.insert(it.key(), it.value());
    QJsonObject m{{"ts", QDateTime::fromMSecsSinceEpoch(now, QTimeZone(QTimeZone::UTC)).toString(Qt::ISODateWithMs)},
                  {"target", req.target}, {"url", req.url}, {"role", req.role}, {"pid", double(req.pid)}, {"image", req.image},
                  {"trigger", req.trigger}, {"episodeMs", double(req.episodeMs)}, {"summary", summary},
                  {"identity", req.identity}, {"health", req.lastHealth}, {"polls", req.polls},
                  {"process", process}, {"threadStates", states}, {"firstThread", firstThread},
                  {"threads", "threads.txt"}, {"processes", "processes.txt"}, {"netstat", "netstat.txt"}, {"logs", logs},
                  {"dump", dumpFile.isEmpty() ? QJsonValue() : QJsonValue(dumpFile)}, {"dumpMs", double(res.dumpMs)},
                  {"watchdog", QJsonObject{{"pid", double(GetCurrentProcessId())}, {"tool", "PlayPlayWatchdog"}}}};
    if (!res.dumpError.isEmpty()) m.insert("dumpError", res.dumpError);
    if (!req.dumpSkipped.isEmpty()) m.insert("dumpSkipped", req.dumpSkipped);
    res.ms = total.elapsed();
    m.insert("elapsedMs", double(res.ms));
    res.manifest = m;
    QString err;
    if (!writeText(dir.filePath("incident.json"), QJsonDocument(m).toJson(QJsonDocument::Indented), &err)) { res.error = err; return res; }
    res.ok = true;
    return res;
}

#else   // not Windows: nothing to watch with

std::vector<ProcInfo> processSnapshot(bool, QString* e) { if (e) *e = QStringLiteral("Windows only"); return {}; }
QJsonArray describeThreads(const ProcInfo&, QString* t) { if (t) t->clear(); return {}; }
std::vector<TcpRow> tcpTable(QString* e) { if (e) *e = QStringLiteral("Windows only"); return {}; }
QString netstatText(const std::vector<TcpRow>&, const QList<quint16>&, quint32, const std::vector<ProcInfo>*) { return {}; }
ProcessId queryProcess(quint32 pid) { ProcessId r; r.pid = pid; return r; }
QString commandLine(quint32, QString* e) { if (e) *e = QStringLiteral("Windows only"); return {}; }
QStringList tailLines(const QString&, int, QString* e, qint64) { if (e) *e = QStringLiteral("Windows only"); return {}; }
bool dumpProcess(quint32, qint64, const QString&, QString* e, qint64*) { if (e) *e = QStringLiteral("Windows only"); return false; }
bool terminateProcess(quint32, qint64, const QString&, quint32, int, QString* e) { if (e) *e = QStringLiteral("Windows only"); return false; }
quint32 launchDetached(const QString&, QString* e) { if (e) *e = QStringLiteral("Windows only"); return 0; }
IncidentResult writeIncident(const IncidentRequest&) { IncidentResult r; r.error = QStringLiteral("Windows only"); return r; }

#endif

} // namespace ppwatch
