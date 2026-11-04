#pragma once
// Engine switches between the in-house tools and the programs they replace (docs/plans/inhouse-deps.md §5.2-5.3, DEP-13).
// Header-only and QtCore-only on purpose: the Supervisor (SpotGenerator, TranscriptionManager) includes it without linking
// PlayPlayInhouseCommon, and test_inhouse_common tests it.
//
//   mode     "python" / "ffmpeg" / "script" / unset = the old program (today's behaviour, the shipped default);
//            "native" = the in-house tool whenever its exe is found; "auto" = the tool when its exe is found AND its
//            `version` lists the sub-command (probed once per exe, asynchronously), else the old program.
//   fallback a native run that exits 64 ("unsupported here") or fails in any other way except the step's own timeout is
//            run again with the old program: every step writes per-job files atomically, so a retry is safe, and the old
//            path is the proven one. A timeout never retries (the step's budget is spent).
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>

namespace toolswitch {

enum class Mode { Legacy, Native, Auto };

constexpr int kUnsupported = 64;     // the tools' "not implemented here: run the old program" exit code (ppcli)
constexpr int kFailedToStart = -2;   // Supervisor-side: QProcess::FailedToStart
constexpr int kTimedOut = -3;        // Supervisor-side: killed by the step's timeout
constexpr int kCrashed = -1;         // Supervisor-side: QProcess::CrashExit (Windows: any NT status in [0x80000000, 0xD0000000), a missing DLL included)

/// Setting value -> mode. Unknown values keep the old program (a typo must never switch production to an untested path).
inline Mode parseMode(const QString& v) {
    const QString s = v.trimmed().toLower();
    if (s == "native") return Mode::Native;
    if (s == "auto") return Mode::Auto;
    return Mode::Legacy;
}
inline QString modeName(Mode m, const QString& legacyName) { return m == Mode::Native ? QStringLiteral("native") : m == Mode::Auto ? QStringLiteral("auto") : legacyName; }
/// Values a settings PUT may store ("" removes the key: back to the default).
inline bool validModeValue(const QString& v) { static const QStringList ok = {"", "auto", "native", "python", "ffmpeg", "script"}; return ok.contains(v.trimmed().toLower()); }

/// The ppcli result: the last non-empty stdout line as a JSON object ({} when it is not one).
inline QJsonObject lastJsonLine(const QByteArray& out) {
    const QList<QByteArray> lines = out.trimmed().split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) { const QByteArray l = lines[i].trimmed(); if (l.isEmpty()) continue; return QJsonDocument::fromJson(l).object(); }
    return QJsonObject();
}

/// Whether a native run is retried with the old program (see the header comment). `ok` = the step's own success test.
inline bool shouldFallBack(int code, bool ok) { if (code == kTimedOut) return false; return code != 0 || !ok; }
/// The quiet case: the tool said "unsupported" or is not startable; anything else is worth a warning.
inline bool expectedFallBack(int code) { return code == kUnsupported || code == kFailedToStart; }
/// QProcess::finished(code, status) -> the code shouldFallBack judges: a crash exit is kCrashed, so a native tool that dies
/// (no Qt6Core.dll next to it, an access violation in a voice DLL, abort()) is retried with the old program like exit 64.
/// `cancelled` = the Supervisor killed it on purpose: that is final (kTimedOut), never a second run.
inline int exitCode(bool normalExit, int code, bool cancelled = false) { return cancelled ? kTimedOut : normalExit ? code : kCrashed; }

/// A script command line -> the tool's: interpreter flags ("-I", "-u", ...) and the script path go, the sub-command comes
/// first, the script's own arguments follow unchanged ("-I x/spot_card.py --spec a --out b" -> "card --spec a --out b").
/// A list without a .py script (an ffmpeg command line) passes through behind the sub-command (or alone when sub is empty).
inline QStringList nativeArgs(const QString& sub, const QStringList& scriptArgs) {
    int i = 0;
    while (i < scriptArgs.size() && scriptArgs[i].startsWith('-') && !scriptArgs[i].endsWith(".py", Qt::CaseInsensitive)) ++i;
    QStringList rest = (i < scriptArgs.size() && scriptArgs[i].endsWith(".py", Qt::CaseInsensitive)) ? scriptArgs.mid(i + 1) : scriptArgs;
    if (!sub.isEmpty()) rest.prepend(sub);
    return rest;
}

/// <dir>/<name>(.exe) in the first dir that has it; "" when none does.
inline QString findTool(const QString& name, const QStringList& dirs) {
#ifdef Q_OS_WIN
    const QString file = name.endsWith(".exe", Qt::CaseInsensitive) ? name : name + ".exe";
#else
    const QString file = name;
#endif
    for (const QString& d : dirs) {
        if (d.trimmed().isEmpty()) continue;
        const QFileInfo fi(QDir(QDir::fromNativeSeparators(d.trimmed())).filePath(file));
        if (fi.isFile()) return QDir::toNativeSeparators(fi.absoluteFilePath());
    }
    return QString();
}

/// What `<exe> version` said ({ok, tool, commands}).
struct Caps {
    bool probed = false, running = false, ok = false;
    QStringList commands; QString error;
    qint64 mtimeMs = 0;
};

/// Runs `<exe> version` without blocking (QProcess signals on ctx's thread) and calls cb once.
inline void probeVersion(QObject* ctx, const QString& exe, std::function<void(bool ok, const QStringList& commands, const QString& error)> cb, int timeoutMs = 10000) {
    auto* p = new QProcess(ctx);
    auto done = std::make_shared<bool>(false);
    auto finish = [p, done, cb](bool ok, const QStringList& cmds, const QString& err) { if (*done) return; *done = true; p->deleteLater(); if (cb) cb(ok, cmds, err); };
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), ctx, [p, finish](int code, QProcess::ExitStatus st) {
        const QJsonObject r = lastJsonLine(p->readAllStandardOutput());
        QStringList cmds; for (const QJsonValue& v : r.value("commands").toArray()) cmds << v.toString();
        if (st == QProcess::NormalExit && code == 0 && r.value("ok").toBool()) finish(true, cmds, QString());
        else finish(false, cmds, st == QProcess::NormalExit ? QString("version exited %1").arg(code) : QStringLiteral("version crashed or timed out"));
    });
    QObject::connect(p, &QProcess::errorOccurred, ctx, [finish](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) finish(false, {}, QStringLiteral("could not start")); });
    QTimer::singleShot(timeoutMs, p, [p]() { if (p->state() != QProcess::NotRunning) p->kill(); });
    p->start(exe, {QStringLiteral("version")});
}

/// Per-exe capability cache: the first look at an exe (or at a replaced one: other mtime) starts a probe and answers
/// "not probed yet"; later looks answer from the cache. `changed` runs after each probe (to refresh a status view).
class CapsCache {
public:
    const Caps& get(QObject* ctx, const QString& exe, std::function<void()> changed = {}) {
        Caps& c = m_caps[exe];
        const qint64 mt = QFileInfo(exe).lastModified().toMSecsSinceEpoch();
        if ((c.probed || c.running) && c.mtimeMs == mt) return c;
        if (c.running) return c;
        c = Caps(); c.running = true; c.mtimeMs = mt;
        QPointer<QObject> guard(ctx); auto* self = this; const QString key = exe;
        probeVersion(ctx, exe, [guard, self, key, mt, changed](bool ok, const QStringList& cmds, const QString& err) {
            if (!guard) return;
            Caps& c = self->m_caps[key]; if (c.mtimeMs != mt) return;
            c.running = false; c.probed = true; c.ok = ok; c.commands = cmds; c.error = err;
            if (changed) changed();
        });
        return c;
    }
    void clear() { m_caps.clear(); }
private:
    QHash<QString, Caps> m_caps;
};

/// The engine a step resolves to.
struct Choice {
    bool native = false;
    QString exe;          // the tool when native
    QString reason;       // why the old program runs ("" when native)
};
inline Choice choose(Mode m, const QString& exe, const Caps& caps, const QString& sub, const QString& toolName) {
    Choice c;
    if (m == Mode::Legacy) { c.reason = QStringLiteral("switched off (old program)"); return c; }
    if (exe.isEmpty()) { c.reason = toolName + QStringLiteral(" not found"); return c; }
    if (m == Mode::Native) { c.native = true; c.exe = exe; return c; }
    if (caps.running || !caps.probed) { c.reason = toolName + QStringLiteral(" capabilities not probed yet"); return c; }
    if (!caps.ok) { c.reason = toolName + QStringLiteral(" version check failed: ") + caps.error; return c; }
    if (!sub.isEmpty() && !caps.commands.contains(sub)) { c.reason = toolName + QStringLiteral(" has no '") + sub + QStringLiteral("' command"); return c; }
    c.native = true; c.exe = exe; return c;
}

} // namespace toolswitch
