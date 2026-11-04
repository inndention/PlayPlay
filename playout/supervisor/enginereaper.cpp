#include "enginereaper.h"
#include "../core/enginelock.h"
#include "../schedule/schedulemodel.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <climits>
#include <vector>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace reaper {

namespace {

QString cleanAbs(const QString& p) {
    if (p.isEmpty()) return {};
    QString s = QDir::cleanPath(QDir::fromNativeSeparators(p));
    if (QDir::isRelativePath(s)) s = QDir::cleanPath(QFileInfo(s).absoluteFilePath());
    while (s.size() > 1 && s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s.toLower();   // NTFS paths ignore case
}

bool samePath(const QString& a, const QString& b) { return !a.isEmpty() && cleanAbs(a) == cleanAbs(b); }

QString startedText(quint64 createTime) {
    const qint64 ms = enginelock::createTimeToMs(createTime);
    return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("HH:mm:ss")) : QStringLiteral("?");
}

bool isOnAir(const EngineFacts& f) { return f.state.compare(QLatin1String("ON_AIR"), Qt::CaseInsensitive) == 0; }

// Which of two adoptable engines of one channel stays: ON_AIR, then the earlier HELLO, then the older process.
bool keepsBefore(const EngineFacts& a, const EngineFacts& b) {
    if (isOnAir(a) != isOnAir(b)) return isOnAir(a);
    const qint64 ha = a.helloAtMs > 0 ? a.helloAtMs : LLONG_MAX, hb = b.helloAtMs > 0 ? b.helloAtMs : LLONG_MAX;
    if (ha != hb) return ha < hb;
    const quint64 ca = a.createTime ? a.createTime : ULLONG_MAX, cb = b.createTime ? b.createTime : ULLONG_MAX;
    if (ca != cb) return ca < cb;
    return a.pid < b.pid;
}

} // namespace

QString reasonName(Reason r) {
    switch (r) {
    case Reason::None: return QStringLiteral("same build");
    case Reason::UnknownChannel: return QStringLiteral("unknown channel");
    case Reason::Disabled: return QStringLiteral("channel disabled");
    case Reason::BuildMismatch: return QStringLiteral("build mismatch");
    case Reason::Duplicate: return QStringLiteral("duplicate engine for the channel");
    case Reason::AdoptDisabled: return QStringLiteral("adoption disabled");
    }
    return {};
}

bool sameBuild(const EngineFacts& f, const BuildFacts& current) {
    if (current.exe.isEmpty() || !samePath(f.exe, current.exe)) return false;
    if (f.exeMtimeMs > 0) return f.exeMtimeMs == current.exeMtimeMs;
    // No lock (an older engine): its build is unknown, but a build replaced in place has a newer mtime than the
    // process's start. Unknown start time = cannot prove it, so not the same build.
    const qint64 started = enginelock::createTimeToMs(f.createTime);
    return started > 0 && current.exeMtimeMs > 0 && current.exeMtimeMs <= started;
}

ReapDecision decide(const EngineFacts& f, const sched::ChannelProfile* profile, const BuildFacts& current,
                    bool adoptEnabled, bool duplicate) {
    ReapDecision d;
    d.facts = f;
    if (!profile) d.reason = Reason::UnknownChannel;
    else if (!profile->enabled) d.reason = Reason::Disabled;
    else if (!sameBuild(f, current)) d.reason = Reason::BuildMismatch;
    else if (duplicate) d.reason = Reason::Duplicate;
    else if (!adoptEnabled) d.reason = Reason::AdoptDisabled;
    d.verdict = d.reason == Reason::None ? Verdict::Adopt : Verdict::Kill;
    const QString who = QString("pid %1 (%2, started %3").arg(f.pid).arg(f.channelId.isEmpty() ? QStringLiteral("?") : f.channelId,
                                                               startedText(f.createTime));
    d.text = d.verdict == Verdict::Adopt ? QString("adopting orphan engine %1)").arg(who)
                                         : QString("reaped orphan engine %1, %2)").arg(who, reasonName(d.reason));
    return d;
}

QVector<ReapDecision> decideAll(const QVector<EngineFacts>& engines, const ProfileLookup& profileOf,
                                const BuildFacts& current, bool adoptEnabled) {
    QVector<ReapDecision> out;
    out.reserve(engines.size());
    QHash<QString, int> keeper;   // channel -> index in out of the engine that stays
    for (const EngineFacts& f : engines) {
        out << decide(f, profileOf ? profileOf(f.channelId) : nullptr, current, adoptEnabled);
        if (out.last().verdict != Verdict::Adopt) continue;
        const int i = out.size() - 1;
        auto it = keeper.find(f.channelId);
        if (it == keeper.end()) { keeper.insert(f.channelId, i); continue; }
        // Two adoptable engines on one channel (two NDI senders of one name, two streams on one group): one stays.
        const int loser = keepsBefore(f, out[*it].facts) ? *it : i;
        if (loser == *it) *it = i;
        const EngineFacts lf = out[loser].facts;
        out[loser] = decide(lf, profileOf ? profileOf(lf.channelId) : nullptr, current, adoptEnabled, true);
    }
    return out;
}

// --- scope helpers -------------------------------------------------------------------------------------------------

QString normalizeIpcName(const QString& ipcName) {
    QString s = ipcName.trimmed();
    const QString prefix = QStringLiteral("\\\\.\\pipe\\");
    if (s.startsWith(prefix, Qt::CaseInsensitive)) s = s.mid(prefix.size());
    return s.toLower();
}

bool sameIpc(const QString& a, const QString& b) {
    const QString na = normalizeIpcName(a);
    return !na.isEmpty() && na == normalizeIpcName(b);
}

bool pathUnder(const QString& path, const QString& root) {
    const QString p = cleanAbs(path), r = cleanAbs(root);
    if (p.isEmpty() || r.isEmpty()) return false;
    if (p == r) return true;
    return p.startsWith(r.endsWith(QLatin1Char('/')) ? r : r + QLatin1Char('/'));   // never ".../supervisor2"
}

EngineArgs parseEngineArgs(const QStringList& argv) {
    EngineArgs a;
    a.supervisor = QStringLiteral("PlayPlaySupervisor");   // engine_main.cpp's default for --supervisor
    for (int i = 1; i < argv.size(); ++i) {
        QString opt = argv[i];
        if (!opt.startsWith(QLatin1Char('-'))) continue;
        while (opt.startsWith(QLatin1Char('-'))) opt.remove(0, 1);
        QString value;
        bool inlineValue = false;
        const int eq = opt.indexOf(QLatin1Char('='));
        if (eq >= 0) { value = opt.mid(eq + 1); opt = opt.left(eq); inlineValue = true; }
        if (opt == QLatin1String("standalone")) { a.standalone = true; continue; }
        if (opt != QLatin1String("id") && opt != QLatin1String("data") && opt != QLatin1String("supervisor")) continue;
        if (!inlineValue) { if (i + 1 >= argv.size()) continue; value = argv[++i]; }
        if (opt == QLatin1String("id")) a.id = value;
        else if (opt == QLatin1String("data")) a.data = value;
        else { a.supervisor = value; a.supervisorGiven = true; }
    }
    return a;
}

QStringList splitCommandLine(const QString& cl) {
    // The rules CommandLineToArgvW / the MSVC runtime apply (and QProcess quotes for): whitespace separates arguments
    // outside quotes; 2n backslashes + quote = n backslashes and a quote toggle; 2n+1 backslashes + quote = n
    // backslashes and a literal quote; other backslashes are literal; "" inside quotes is a literal quote.
    QStringList out;
    QString cur;
    bool inQuotes = false, have = false;
    const int n = cl.size();
    for (int i = 0; i < n;) {
        const QChar c = cl[i];
        if (!inQuotes && (c == QLatin1Char(' ') || c == QLatin1Char('\t'))) {
            if (have) { out << cur; cur.clear(); have = false; }
            ++i; continue;
        }
        have = true;
        if (c == QLatin1Char('\\')) {
            int bs = 0;
            while (i < n && cl[i] == QLatin1Char('\\')) { ++bs; ++i; }
            if (i < n && cl[i] == QLatin1Char('"')) {
                cur += QString(bs / 2, QLatin1Char('\\'));
                if (bs % 2) { cur += QLatin1Char('"'); ++i; }
            } else {
                cur += QString(bs, QLatin1Char('\\'));
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            if (inQuotes && i + 1 < n && cl[i + 1] == QLatin1Char('"')) { cur += QLatin1Char('"'); i += 2; continue; }
            inQuotes = !inQuotes; ++i; continue;
        }
        cur += c; ++i;
    }
    if (have) out << cur;
    return out;
}

// --- process helpers -----------------------------------------------------------------------------------------------

QString processCommandLine(qint64 pid) {
#ifdef Q_OS_WIN
    if (pid <= 0) return {};
    using NtQip = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const NtQip ntQip = reinterpret_cast<NtQip>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")));
    if (!ntQip) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return {};
    constexpr ULONG kProcessCommandLineInformation = 60;   // Windows 8.1+; PROCESS_QUERY_LIMITED_INFORMATION suffices
    struct UnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };
    QString result;
    ULONG len = 4096;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::vector<unsigned char> buf(size_t(len) + sizeof(UnicodeString));
        ULONG need = 0;
        const LONG st = ntQip(h, kProcessCommandLineInformation, buf.data(), ULONG(buf.size()), &need);
        if (st >= 0) {
            const auto* us = reinterpret_cast<const UnicodeString*>(buf.data());
            if (us->Buffer && us->Length) result = QString::fromWCharArray(us->Buffer, int(us->Length / sizeof(wchar_t)));
            break;
        }
        if (need <= buf.size()) break;   // a failure other than "buffer too small"
        len = need;
    }
    CloseHandle(h);
    return result;
#else
    Q_UNUSED(pid); return {};
#endif
}

QVector<ProcessEntry> listProcesses(const QString& exeName) {
    QVector<ProcessEntry> out;
#ifdef Q_OS_WIN
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const QString name = QString::fromWCharArray(pe.szExeFile);
        if (name.compare(exeName, Qt::CaseInsensitive) == 0) out << ProcessEntry{qint64(pe.th32ProcessID), name};
    }
    CloseHandle(snap);
#else
    Q_UNUSED(exeName);
#endif
    return out;
}

// --- scan ----------------------------------------------------------------------------------------------------------

ScanResult scan(const ScanOptions& opt) {
    ScanResult r;
    if (opt.dataDir.isEmpty() || normalizeIpcName(opt.ipcName).isEmpty()) {
        r.notes << QStringLiteral("no data dir or IPC name: nothing is in scope");
        return r;
    }
    const QString root = QDir(opt.dataDir).absolutePath();
    const qint64 self = QCoreApplication::applicationPid();
    QSet<qint64> seen;

    // 1. Lock files: the engines that wrote one (PS-3 and later), checked against the live process (pid alive, same
    //    image, same creation time: a pid reused by another process has another creation time).
    if (opt.useLocks) {
        const QDir channels(QDir(root).filePath(QStringLiteral("channels")));
        for (const QFileInfo& d : channels.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString lp = enginelock::lockPath(d.absoluteFilePath());
            if (!QFile::exists(lp)) continue;
            enginelock::LockInfo li;
            QString err;
            if (!enginelock::readLock(lp, &li, &err)) { r.notes << QString("%1: unreadable (%2)").arg(lp, err); continue; }
            if (!sameIpc(li.supervisorIpc, opt.ipcName)) {
                r.notes << QString("%1: IPC \"%2\" is not ours, left alone").arg(lp, li.supervisorIpc); continue;
            }
            const QString dd = li.dataDir.isEmpty() ? d.absoluteFilePath() : li.dataDir;
            if (!pathUnder(dd, root)) { r.notes << QString("%1: data dir %2 is outside ours, left alone").arg(lp, dd); continue; }
            if (li.pid == self) continue;
            if (!enginelock::processAlive(li.pid)) { r.staleLocks << lp; continue; }
            const quint64 ct = enginelock::processCreateTime(li.pid);
            if (!ct) { r.notes << QString("%1: pid %2 cannot be opened, left alone").arg(lp).arg(li.pid); continue; }
            if (ct != li.createTime || !samePath(enginelock::processImagePath(li.pid), li.exe)) {
                r.staleLocks << lp;   // the pid now belongs to another process
                continue;
            }
            EngineFacts f;
            f.channelId = li.channelId; f.pid = li.pid; f.createTime = li.createTime; f.exe = li.exe;
            f.exeMtimeMs = li.exeMtimeMs; f.configHash = li.configHash; f.supervisorIpc = li.supervisorIpc;
            f.supervisorPid = li.supervisorPid; f.dataDir = dd; f.fromLock = true;
            r.engines << f;
            seen.insert(f.pid);
        }
    }

    // 2. The process list: engines without a lock (older builds, or a lock that was removed) matched by their command
    //    line. Scope = an explicit or default --supervisor equal to ours AND an absolute --data under our data dir.
    if (opt.useProcessList) {
        for (const ProcessEntry& pe : listProcesses(opt.engineExeName)) {
            if (pe.pid == self || seen.contains(pe.pid)) continue;
            const QString cl = processCommandLine(pe.pid);
            if (cl.isEmpty()) { r.notes << QString("pid %1: command line unreadable, left alone").arg(pe.pid); continue; }
            const EngineArgs a = parseEngineArgs(splitCommandLine(cl));
            if (a.standalone || a.supervisor.isEmpty()) continue;                  // never a Supervisor's engine
            if (!sameIpc(a.supervisor, opt.ipcName)) continue;                      // another Supervisor's
            if (a.id.isEmpty() || a.data.isEmpty() || QDir::isRelativePath(QDir::fromNativeSeparators(a.data))) {
                r.notes << QString("pid %1: no --id or no absolute --data, left alone").arg(pe.pid); continue;
            }
            if (!pathUnder(a.data, root)) continue;                                 // another data dir
            const quint64 ct = enginelock::processCreateTime(pe.pid);
            if (!ct) { r.notes << QString("pid %1: cannot be opened, left alone").arg(pe.pid); continue; }
            EngineFacts f;
            f.channelId = a.id; f.pid = pe.pid; f.createTime = ct; f.exe = enginelock::processImagePath(pe.pid);
            f.supervisorIpc = a.supervisor; f.dataDir = a.data; f.fromCommandLine = true;
            r.engines << f;
            seen.insert(f.pid);
        }
    }
    return r;
}

} // namespace reaper
