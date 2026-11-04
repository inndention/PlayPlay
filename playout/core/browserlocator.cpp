#include "browserlocator.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtGlobal>
#include <QVersionNumber>

namespace cgbrowser {

static void addIf(QStringList& out, const QString& p) {
    if (p.trimmed().isEmpty()) return;
    const QString n = QDir::toNativeSeparators(QDir::cleanPath(p.trimmed()));
    if (!out.contains(n, Qt::CaseInsensitive)) out << n;
}

QStringList candidates(const QString& preferred) {
    QStringList c;
    addIf(c, preferred);
    addIf(c, qEnvironmentVariable("PLAYPLAY_BROWSER"));
    const QString pf = qEnvironmentVariable("ProgramFiles", "C:/Program Files");
    const QString pf86 = qEnvironmentVariable("ProgramFiles(x86)", "C:/Program Files (x86)");
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    for (const QString& base : {pf, pf86, local}) {
        if (base.isEmpty()) continue;
        addIf(c, base + "/Google/Chrome/Application/chrome.exe");
    }
    for (const QString& base : {pf86, pf}) addIf(c, base + "/Microsoft/Edge/Application/msedge.exe");
    for (const QString& base : {pf, pf86, local}) {
        if (base.isEmpty()) continue;
        addIf(c, base + "/Chromium/Application/chrome.exe");
        addIf(c, base + "/BraveSoftware/Brave-Browser/Application/brave.exe");
    }
    for (const QString& name : {"chrome", "msedge", "chromium", "chrome.exe", "msedge.exe"}) addIf(c, QStandardPaths::findExecutable(name));
    return c;
}

QString find(const QString& preferred) {
    for (const QString& p : candidates(preferred)) if (QFileInfo(p).isFile()) return p;
    return QString();
}

QString version(const QString& exe) {
    // chrome.exe --version prints nothing on Windows; the build number is the name of the versioned folder beside the exe.
    static const QRegularExpression re("^\\d+\\.\\d+\\.\\d+\\.\\d+$");
    QString best;
    const QDir dir = QFileInfo(exe).absoluteDir();
    for (const QString& d : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!re.match(d).hasMatch()) continue;
        if (best.isEmpty() || QVersionNumber::fromString(d) > QVersionNumber::fromString(best)) best = d;
    }
    return best.isEmpty() ? QStringLiteral("unknown") : best;
}

QJsonObject describe(const QString& preferred) {
    QJsonObject o;
    const QString p = find(preferred);
    o["available"] = !p.isEmpty();
    o["path"] = p;
    o["version"] = p.isEmpty() ? QString() : version(p);
    o["preferred"] = preferred;
    o["env"] = qEnvironmentVariable("PLAYPLAY_BROWSER");
    QJsonArray a; for (const QString& c : candidates(preferred)) { QJsonObject e; e["path"] = c; e["exists"] = QFileInfo(c).isFile(); a.append(e); }
    o["candidates"] = a;
    return o;
}

} // namespace cgbrowser
