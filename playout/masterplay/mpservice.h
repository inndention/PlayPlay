#pragma once
// MasterPlay REST service (the lane design notes §9 files, §10 REST API, §12 settings keys, §13 safety; slice MP-11):
// the /api/v1/masterplay routes over the lane's parser, resolver, importer, exporter and log readers. The service is a
// route TABLE (routes()): the Supervisor wiring (MP-12, a laneext::Registrar in its own translation unit) registers it on
// the RestApiServer through install<Rest>(), tests and the CLI call the same handlers through call() / callSync(), so this
// library never links the REST server, the IngestManager or the Supervisor.
//
// Routes (bearer-gated like every /api/v1 route; every write defaults to a dry run; status / body as the REST server
// sends them, errors as {error, status}):
//   GET    /api/v1/masterplay                         {lane, apiVersion, routes[], sources[], pending, settingsKeys[]}
//   GET    /api/v1/masterplay/catalog                 {instructions[] (the catalog), diagnostics[] (design §15)}
//   POST   /api/v1/masterplay/parse                   {text | path [+ source | node], name?, fps?, legacyDateOrder?, withEntries?}
//                                                     -> {name, file, read, encoding, bom, eol, finalNewline, stats, events[],
//                                                         preamble[], diagnostics[], roundTrip} (withEntries: the entries' raw)
//   POST   /api/v1/masterplay/import                  {text | path [+ source | node], name?, channel, mode grid | dates, from, to,
//                                                      weekdays[], hours[], options{imp::Options keys}, resolver{res::Options keys},
//                                                      pathMap[]?, dryRun (default true), allowOnAir, importId?, by?, withSlots
//                                                      (default true), withPlacements}
//                                                     dry run: {dryRun: true, plan: Plan::toJson()}; apply: ApplyResult::toJson()
//                                                     + {plan: the plan without its slots, ingest{requested, enqueued[], skipped[]}}.
//                                                     409 on-air slots unless allowOnAir; 400 bad options / channel / plan errors.
//   POST   /api/v1/masterplay/import/{id}/undo        {allowOnAir?, force?, by?} -> ApplyResult::toJson() (404 / 409 as apply / undo say)
//   GET    /api/v1/masterplay/imports                 ?limit= -> {imports[]} (newest first)
//   GET    /api/v1/masterplay/imports/{id}            {entry, backup{importId, channel, at, order[], slots: n}} (404 unknown)
//   POST   /api/v1/masterplay/export                  {channel, mode, from, to, dateStyle, encoding, eol, finalNewline, rawLines,
//                                                      foldEveryDay, mergeFolderPicks, fps, pathMap[]?, withText (default true),
//                                                      download (also base64 bytes)} -> Result::toJson(withText) + {bytesBase64?}
//   GET    /api/v1/masterplay/export.in               the same options as query parameters: the file itself (text/plain,
//                                                     Content-Disposition attachment; a raw route, see install<Rest>())
//   GET    /api/v1/masterplay/browse                  ?source=<id> [&sub=<rel dir>] | ?dir=<path> [&node=] [&quick=0]
//                                                     -> {dir, parent?, node?, read, entries[{name, path, size, mtime, isDir, dated,
//                                                         quick{events, media, instructions, firstStamp, errors}?}]} (.in and folders)
//   POST   /api/v1/masterplay/asrun/import            {text | path [+ source | node], statusPath? | statusText?, from?, to?, tz?,
//                                                      channelAs?, countTowardContracts?, includeCommands?, includeMissed?, capSec?,
//                                                      sample?, dryRun (default true), by?}
//                                                     -> AsRunWriteResult::toJson() + {channel, tz, source, file, stats, diagnostics[],
//                                                        status{report}?, state?}
//   GET    /api/v1/masterplay/asrun/state             {state{<sourceId>: {at, channel, file, lastStamp, lastKey, lastLine, inserted}}}
//   GET    /api/v1/masterplay/config                  {sources[], pathMap[], defaults{}, resolver{}, asrun{}}
//   PUT    /api/v1/masterplay/config                  the same (replaced whole; 400 lists what is wrong)
//   POST   /api/v1/masterplay/config/from-ini         {text | path [+ source | node]} -> SourceSuggestion::toJson(ini) + {exists{}}
//   POST   /api/v1/masterplay/resolve                 {paths[], pathMap?, resolver{...} | res::Options keys} -> {results[], stats}
//   GET    /api/v1/masterplay/links                   {links[]}
//   POST   /api/v1/masterplay/links                   a link record {id?, channel, subPath, slots[], source?, node?, enabled?, ...} -> {link, links[]}
//   DELETE /api/v1/masterplay/links/{id}              {deleted, links[]}
//
// Inputs. A playlist / log / INI comes as `text` (pasted; `name` labels it, relative sub-playlists cannot be read then),
// or as `path`: absolute, or relative to the `source`'s playlistDir (logPath's folder for logs). A source (config
// `sources[]`: {id, label, read fs | mesh, node?, playlistDir, logPath, osdDir, tz}) says where to read: the file system
// (fs; sub-playlists through tl::fileReader with the config pathMap) or the AN-FS mesh (mesh: GET /api/download?node=&path=
// on the bridge named by the store's mesh.anfsUrl with mesh.anfsToken / mesh.anfsCookie, over the service's own
// QNetworkAccessManager; `node` in the body overrides the source's). A bare `node` with an absolute `path` reads the mesh
// without a source. Files over 32 MiB are refused (413).
//
// Settings keys (design §12): masterplay.config, masterplay.links, masterplay.asrun.state (this service);
// masterplay.imports and masterplay.backup.<id> (the importer, MP-09).
//
// THREADING: parse / import / export / browse / as-run / from-ini / resolve run in the service's own QThreadPool (Host::
// maxThreads, 2): file and mesh reads block, plan() reads sub-playlists and probes files, the exporter's dateSlots reads
// the store once per hour, the as-run writer opens its own SQLite connection. The reply, the ingest enqueue, afterSave
// and broadcast are queued back to the service's thread (QPointer-guarded: a reply for a service that is gone is
// dropped). Store writes (apply / undo) happen on the worker under the store's own mutex, the way the lane's tests run
// them. Config, links, imports and state are quick settings reads and answer synchronously.
//
// SAFETY (design §13): every write route is a dry run unless dryRun:false; apply / undo refuse slots in [now, now + 2 h]
// (409) unless allowOnAir; apply writes through the importer's backup + history, so every apply can be undone; after a
// write Host::afterSave runs (the Supervisor's GEN-marker enforcement: by default a POST to this Supervisor's own
// /api/v1/spots/markers/enforce when Host::restPort is set, the same pass the PUT slot route queues). Mesh credentials
// are read per call from the store and never echoed. The IniFile itself is never returned (design: it holds passwords).
#include "masterplayin.h"
#include "mpasrun.h"
#include "mpexporter.h"
#include "mpimporter.h"
#include "mpini.h"
#include "mpresolver.h"
#include "mptimeline.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTcpSocket>   // the raw download route of install<Rest>() (the template's non-dependent parts need the type)
#include <functional>
#include <memory>

namespace sched { class ScheduleStore; }

namespace mp::svc {

using Query = QHash<QString, QString>;
using Reply = std::function<void(int status, const QJsonObject& body)>;
using Handler = std::function<void(const QJsonObject& body, const Query& query, Reply reply)>;

/// What the service needs from its host: a subset of laneext::Context (MP-12 fills it from one), so tests and the CLI
/// need no Supervisor.
struct Host {
    sched::ScheduleStore* store = nullptr;                    // required (open)
    QString dataDir;                                          // the Supervisor's data dir (informational)
    int restPort = 0;                                         // this Supervisor's REST port (the default afterSave)
    std::function<QString()> restToken;                       // ... and its bearer token
    /// IngestManager::enqueue(path, type, "masterplay", meta) for missing = ingest; none: the list is reported only.
    std::function<QJsonObject(const QString& path, const QString& type, const QJsonObject& meta)> enqueueIngest;
    /// After slots were written / restored (SpotGenerator::enforceMarkers). Unset: the default above (none without a port).
    std::function<void()> afterSave;
    std::function<void(const QString& type, const QJsonObject& payload)> broadcast;   // WebSocket {type: "masterplay", ...}
    std::function<void(const QString& channelId, const QString& level, const QString& message)> alert;
    /// The AN-FS bridge {url, token, cookie}; unset: the store's mesh.anfsUrl / mesh.anfsToken / mesh.anfsCookie.
    std::function<QJsonObject()> meshConfig;
    qint64 nowMs = 0;                                         // a fixed clock (tests); 0 = the wall clock
    int maxThreads = 2;
    int meshTimeoutMs = 30000;
};

struct Route {
    QString method, path;                                     // "GET", "/api/v1/masterplay/..." ({id} params as the REST server matches them)
    Handler handle;
};

/// A finished file for the raw download route.
struct FileReply {
    int status = 200;
    QByteArray contentType = QByteArrayLiteral("text/plain; charset=utf-8");
    QString filename;                                         // Content-Disposition attachment
    QByteArray bytes;                                         // the file, or a JSON error body when status >= 400
};

class MpService : public QObject {
public:
    explicit MpService(const Host& host, QObject* parent = nullptr);
    ~MpService() override;

    static constexpr const char* kPrefix = "/api/v1/masterplay";
    static constexpr qint64 kMaxFileBytes = 32 * 1024 * 1024;
    static constexpr int kMaxResolvePaths = 5000;
    static constexpr int kMaxQuickFiles = 300;                // browse: .in files parsed for quick stats per listing

    const Host& host() const;
    /// The JSON routes, in registration order.
    const QList<Route>& routes() const;
    /// "METHOD /path" of every route, the raw download route included (what RestApiServer::routeKeys() will list).
    QStringList routeKeys() const;

    /// Registers every route on a REST server (anything with registerAsync(method, path, fn) and registerRaw(method,
    /// prefix, fn) taking a QTcpSocket*): a template, so only the Supervisor wiring instantiates it.
    template <class Rest> void install(Rest* rest);

    /// Dispatches "METHOD /api/v1/masterplay/..." like the REST server (exact, then {param} segments into the query);
    /// false when no route matches. The reply may arrive later (worker routes) and arrives on the service's thread.
    bool call(const QString& method, const QString& path, const QJsonObject& body, const Query& query, Reply reply) const;
    /// call() and wait on an event loop for the reply (at most timeoutMs): the status, or 0 on timeout / no route.
    int callSync(const QString& method, const QString& path, const QJsonObject& body, QJsonObject* out, const Query& query = {},
                 int timeoutMs = 60000) const;
    /// GET /export.in: the export as a file, delivered later on the service's thread.
    void exportFile(const Query& query, std::function<void(const FileReply&)> done) const;

    /// masterplay.config with every default filled: {sources[], pathMap[], defaults{}, resolver{}, asrun{}}.
    QJsonObject config() const;
    /// Validates and stores it whole; the stored config, or {} with *error set (400).
    QJsonObject setConfig(const QJsonObject& c, QString* error);
    /// The config source with that id ({} when unknown).
    QJsonObject source(const QString& id) const;
    /// masterplay.links {links[]}.
    QJsonArray links() const;
    /// masterplay.asrun.state {<sourceId>: {...}}.
    QJsonObject asRunState() const;
    /// Worker jobs not finished yet.
    int pending() const;

    /// "YYYY-MM-DD" from a daily playlist's name (DailyPlaylistMode: 2025-09-08.in, 20250908.in, 2025_09_08.in, with any
    /// prefix / suffix); invalid when none.
    static QDate datedName(const QString& fileName);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// ---------------------------------------------------------------------------------------------------------------------

template <class Rest> void MpService::install(Rest* rest) {
    if (!rest) return;
    for (const Route& r : routes()) {
        Handler h = r.handle;
        rest->registerAsync(r.method, r.path, [h](const QJsonObject& body, const QHash<QString, QString>& q, std::function<void(int, const QJsonObject&)> reply) {
            h(body, q, std::move(reply));
        });
    }
    // The file download owns its socket (RestApiServer::RawHandler): header + bytes, then close.
    rest->registerRaw(QStringLiteral("GET"), QString::fromLatin1(kPrefix) + QStringLiteral("/export.in"), [this](QTcpSocket* socket, const QString&, const QHash<QString, QString>& q) {
        QPointer<QTcpSocket> sp(socket);
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        exportFile(q, [sp](const FileReply& f) {
            if (!sp) return;
            QByteArray h = "HTTP/1.1 " + QByteArray::number(f.status) + (f.status == 200 ? " OK" : " Error") + "\r\n";
            h += "Content-Type: " + f.contentType + "\r\n";
            h += "Access-Control-Allow-Origin: *\r\nCache-Control: no-cache, no-store\r\n";
            if (f.status == 200 && !f.filename.isEmpty()) h += "Content-Disposition: attachment; filename=\"" + f.filename.toUtf8().replace('"', '_') + "\"\r\n";
            h += "Content-Length: " + QByteArray::number(f.bytes.size()) + "\r\nConnection: close\r\n\r\n";
            sp->write(h + f.bytes);
            sp->flush();
            sp->disconnectFromHost();
        });
    });
}

} // namespace mp::svc
