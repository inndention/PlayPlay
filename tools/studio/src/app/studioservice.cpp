#include "app/studioservice.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QStringList>
#include <algorithm>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef PLAYPLAY_STUDIO_VERSION
#define PLAYPLAY_STUDIO_VERSION "0.0.0-dev"
#endif

// AUTORCC compiles tools/studio/resources.qrc into this static library, where nothing references its initializer and
// the linker would drop it. Q_INIT_RESOURCE has to run from the global namespace.
static void studioInitResources() { Q_INIT_RESOURCE(resources); }

namespace studio {

namespace {

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

net::Response homePage() {
    QFile f(QStringLiteral(":/studio/home.html"));
    if (!f.open(QIODevice::ReadOnly)) return net::Response::error(500, QStringLiteral("home page missing from resources"));
    QByteArray b = f.readAll();
    b.replace("{{version}}", version().toUtf8());
    net::Response r = net::Response::bytes(200, "text/html; charset=utf-8", b);
    r.headers.set("X-Frame-Options", "DENY");
    return r;
}

QString absClean(const QString& dir) { return QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(dir)).absoluteFilePath()); }

// Where an existing path really is: junctions, symlinks and 8.3 short names resolved. Empty when it cannot be opened.
QString finalPath(const QString& existing) {
#ifdef Q_OS_WIN
    // No access rights and every share mode: the handle only names the file, it never blocks its owner.
    const HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(QDir::toNativeSeparators(existing).utf16()), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // BACKUP_SEMANTICS: needed to open a directory
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<wchar_t> buf(MAX_PATH);
    DWORD n = GetFinalPathNameByHandleW(h, buf.data(), DWORD(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buf.size()) {                                       // too small: n is the size needed
        buf.resize(size_t(n) + 1);
        n = GetFinalPathNameByHandleW(h, buf.data(), DWORD(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= buf.size()) return {};
    QString p = QString::fromWCharArray(buf.data(), int(n));
    if (p.startsWith(QLatin1String("\\\\?\\UNC\\"))) p = QLatin1String("\\\\") + p.mid(8);
    else if (p.startsWith(QLatin1String("\\\\?\\"))) p = p.mid(4);
    return QDir::cleanPath(QDir::fromNativeSeparators(p));
#else
    return QFileInfo(existing).canonicalFilePath();
#endif
}

// `path` with its deepest existing ancestor replaced by where that ancestor really is, so a junction, symlink or short
// name cannot hide a path inside another tree; the components below it (not created yet) are kept as given.
QString realPath(const QString& path) {
    QString head = absClean(path);
    QStringList tail;
    while (!QFileInfo::exists(head)) {
        const QFileInfo fi(head);
        const QString parent = fi.path();
        if (parent.isEmpty() || parent == head || parent == QLatin1String(".")) return absClean(path);
        tail.prepend(fi.fileName());
        head = parent;
    }
    QString real = finalPath(head);
    if (real.isEmpty()) return absClean(path);
    for (const QString& t : tail) real += QLatin1Char('/') + t;
    return QDir::cleanPath(real);
}

} // namespace

QString version() { return QStringLiteral(PLAYPLAY_STUDIO_VERSION); }
QString serviceName() { return QStringLiteral("playplay-studio"); }

void setAppIdentity() {
    QCoreApplication::setApplicationName(QStringLiteral("PlayPlayStudio"));
    QCoreApplication::setOrganizationName(QStringLiteral("PlayPlayStudio"));
    QCoreApplication::setApplicationVersion(version());
}

bool reservedPort(quint16 port) {
    // 7709 = on-air Supervisor watchdog /healthz (watchdog.port), 7710/7711 = test Supervisor REST/WS in the older docs.
    static const quint16 kReserved[] = {443,  7700, 7701, 7709, 7710, 7711, 7797, 7798, 7799,
                                        8080, 8123, 8775, 8780, 8790, 8899, 11434};
    if (port >= 7827 && port <= 7833) return true;   // StarMedia Local: node :7827 + PHP workers :7828-7833
    if (port >= 9000 && port <= 9999) return true;   // engines' --port (ChannelManager hands out 9001-9999), SRT 9000+N
    return std::find(std::begin(kReserved), std::end(kReserved), port) != std::end(kReserved);
}

bool insideSupervisorData(const QString& dir) {
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty() || dir.trimmed().isEmpty()) return false;
    // Both sides resolved (realPath), so C:\Users\X\APPDAT~1\Roaming\PlayPlay or a junction into it is caught too.
    const QString live = realPath(appData + QStringLiteral("/PlayPlay"));
    const QString d = realPath(dir);
    return d.compare(live, Qt::CaseInsensitive) == 0 || d.startsWith(live + QLatin1Char('/'), Qt::CaseInsensitive);
}

QString defaultDataDir() {
    const QString env = qEnvironmentVariable("PLAYPLAY_STUDIO_DATA").trimmed();
    if (!env.isEmpty()) return absClean(env);
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return absClean(base + QStringLiteral("/PlayPlayStudio"));
}

QStringList allowedHostsFor(const QHostAddress& bind) {
    QStringList hosts{QStringLiteral("localhost"), QStringLiteral("127.0.0.1"), QStringLiteral("::1")};
    const QString b = bind.toString().toLower();
    if (!b.isEmpty() && !hosts.contains(b)) hosts << b;
    return hosts;
}

QString checkOptions(const ServiceOptions& opt) {
    if (opt.port != 0 && reservedPort(opt.port))
        return QStringLiteral("port %1 belongs to another PlayPlay / StarMedia service").arg(opt.port);
    if (opt.wsPort < -1 || opt.wsPort > 65535) return QStringLiteral("bad WS port %1").arg(opt.wsPort);
    if (opt.wsPort > 0 && reservedPort(quint16(opt.wsPort)))
        return QStringLiteral("port %1 belongs to another PlayPlay / StarMedia service").arg(opt.wsPort);
    if (opt.wsPort > 0 && opt.wsPort == opt.port) return QStringLiteral("the REST and WS ports must differ (both %1)").arg(opt.port);
    // Loopback only. The token (net/auth.h) guards every route but /health and the home page, but a remote bind would
    // also expose those and the token's brute-force surface; a tailnet bind (design §4.1) is a later, explicit option.
    if (opt.bind.isNull() || !opt.bind.isLoopback())
        return QStringLiteral("only a loopback bind is allowed (got %1)").arg(opt.bind.toString());
    const QString dir = opt.dataDir.isEmpty() ? defaultDataDir() : opt.dataDir;
    if (insideSupervisorData(dir)) return QStringLiteral("refusing a data dir inside the Supervisor's data (%APPDATA%/PlayPlay)");
    return {};
}

// m_http and m_hub are members, so they get no QObject parent (the parent would delete them a second time).
StudioService::StudioService(QObject* parent) : QObject(parent) {
    studioInitResources();
    m_router.setAuth(&m_auth);
    m_hub.setAuth(&m_auth);
    m_http.setAdmit([this](net::Request& rq) { return m_router.admit(rq); });
    const net::RouteOptions open{net::Access::Public};
    m_router.add("GET", QStringLiteral("/health"), [this](const net::Request&) { return net::Response::json(health()); }, open);
    m_router.add("GET", QStringLiteral("/"), [](const net::Request&) { return homePage(); }, open);
    m_router.add("GET", QStringLiteral("/index.html"), [](const net::Request&) { return homePage(); }, open);
    m_router.add("GET", QStringLiteral("/api/v1/status"), [this](const net::Request&) { return net::Response::json(status()); });
}

StudioService::~StudioService() { stop(); }

bool StudioService::start(const ServiceOptions& opt, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    if (isListening()) return fail(QStringLiteral("already listening on port %1").arg(port()));
    if (const QString why = checkOptions(opt); !why.isEmpty()) return fail(why);   // before anything touches disk or a port
    const QString dir = opt.dataDir.isEmpty() ? defaultDataDir() : absClean(opt.dataDir);
    if (!QDir().mkpath(dir)) return fail(QStringLiteral("cannot create the data dir %1").arg(QDir::toNativeSeparators(dir)));
    QString err;
    if (!m_auth.load(dir, &err)) return fail(err);                 // a malformed token file refuses start-up
    const QStringList hosts = allowedHostsFor(opt.bind);
    net::ServerOptions so = m_http.options();
    so.allowedHosts = hosts;
    so.spoolDir = dir + QStringLiteral("/tmp");
    m_http.setOptions(so);
    m_hub.setAllowedHosts(hosts);
    if (!m_http.listen(opt.bind, opt.port, &err)) return fail(err);
    if (opt.wsPort >= 0 && !m_hub.listen(opt.bind, quint16(opt.wsPort), &err)) {
        m_http.close();
        return fail(err);
    }
    m_dataDir = dir;
    m_startedMs = nowMs();
    emit started(m_http.port());
    return true;
}

void StudioService::stop() {
    m_http.close();
    m_hub.close();
}

bool StudioService::isListening() const { return m_http.isListening(); }
quint16 StudioService::port() const { return m_http.port(); }
quint16 StudioService::wsPort() const { return m_hub.port(); }

QJsonObject StudioService::health() const {
    QJsonObject queue;
    queue["running"] = 0;
    queue["queued"] = 0;
    queue["max"] = 0;
    QJsonObject o;
    o["ok"] = true;
    o["service"] = serviceName();
    o["version"] = version();
    // Not capable until a job pipeline is installed (spots, ST-19): RenderFarm only sends work to a capable node.
    o["capable"] = false;
    o["reasons"] = QJsonArray{QStringLiteral("engine skeleton: no job pipeline installed yet")};
    o["queue"] = queue;
    o["uptimeMs"] = double(m_startedMs ? nowMs() - m_startedMs : 0);
    return o;
}

QJsonObject StudioService::status() const {
    QJsonObject o = health();
    o["http"] = m_http.statsJson();
    o["ws"] = m_hub.stats();
    return o;
}

} // namespace studio
