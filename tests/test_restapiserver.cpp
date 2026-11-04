#include <QtTest/QtTest>
#include <QTcpSocket>
#include <QTcpServer>
#include <QNetworkProxy>
#include <QThread>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QFile>
#include <atomic>
#include <memory>
#include <functional>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include "playout/supervisor/restapiserver.h"
#include "playout/schedule/storereader.h"
#include "playout/schedule/schedulestore.h"
#include "playout/core/loopwatchdog.h"

// REST server hardening (perf-supervisor PS-9 / W15): the route trie against the old per-request matcher for every key
// of the Supervisor's route dump, the keep-alive idle sweep over 2,000 sockets, a stable handle count over 10,000
// requests, 431 on an oversize header, the slow-handler log with the watchdog tag, and the StoreReader: /asrun off the
// event loop with the store's own answer, a 2 s read while the loop keeps answering. Ephemeral ports, temp databases.

using namespace sched;

namespace {

// Route dump: every registerRoute / registerAsync key in playout/supervisor/*.cpp on 2026-10-26 (the test Supervisor's
// capability list). PP_ROUTE_DUMP=<file with one "METHOD /path" per line> replaces it.
const char* kRouteDump = R"DUMP(
DELETE /api/nodes/{nid}
DELETE /api/v1/agents/tasks/{id}
DELETE /api/v1/cameras/{id}
DELETE /api/v1/cg/designer/templates/{id}
DELETE /api/v1/channels/{id}
DELETE /api/v1/channels/{id}/outputs/{name}
DELETE /api/v1/media/collections/{id}
DELETE /api/v1/media/{id}
DELETE /api/v1/media/{id}/transcript
DELETE /api/v1/mesh/jobs/{id}
DELETE /api/v1/overlays/{ch}/{layer}
DELETE /api/v1/publish/items/{id}
DELETE /api/v1/render/jobs/{id}
DELETE /api/v1/render/nodes/{id}
DELETE /api/v1/schedule/{ch}/slot/{slot}
DELETE /api/v1/sessions/{id}
DELETE /api/v1/spots/generation/{id}
DELETE /api/v1/spots/jobs/{id}
DELETE /api/v1/spotstudio/projects/{id}
DELETE /api/v1/starnodes/{id}
DELETE /api/v1/transcribe/jobs/{id}
DELETE /api/v1/transfers/{id}
GET /api/ai/status
GET /api/channels
GET /api/events/recent
GET /api/fleet
GET /api/ingest/batch
GET /api/ingest/batch/{id}
GET /api/ingest/taxonomy
GET /api/media
GET /api/media/{id}
GET /api/newsroom/render-request/{id}
GET /api/newsroom/status
GET /api/newsroom/studio
GET /api/nodes
GET /api/nodes/{nid}/inventory
GET /api/op/commands
GET /api/op/status
GET /api/publish/config
GET /api/publish/publications
GET /api/social/config
GET /api/social/pulse
GET /api/social/queue
GET /api/state
GET /api/stations
GET /api/stations/{sid}/capabilities
GET /api/v1/agents
GET /api/v1/agents/commands
GET /api/v1/agents/config
GET /api/v1/agents/tasks
GET /api/v1/agents/tasks/{id}
GET /api/v1/ai/config
GET /api/v1/ai/models
GET /api/v1/alerts
GET /api/v1/alerts/twilio
GET /api/v1/asrun
GET /api/v1/asrun/summary
GET /api/v1/audio/devices
GET /api/v1/cameras
GET /api/v1/cameras/{id}
GET /api/v1/cameras/{id}/presets
GET /api/v1/cameras/{id}/stream
GET /api/v1/cg/designer
GET /api/v1/cg/designer/config
GET /api/v1/cg/designer/jobs/{id}
GET /api/v1/cg/designer/templates
GET /api/v1/cg/designer/templates/{id}
GET /api/v1/cg/factbox/presets
GET /api/v1/cg/html/{ch}
GET /api/v1/cg/templates
GET /api/v1/channels
GET /api/v1/channels/{id}
GET /api/v1/channels/{id}/autofill
GET /api/v1/channels/{id}/cg/karaoke
GET /api/v1/channels/{id}/mixes
GET /api/v1/channels/{id}/outputs
GET /api/v1/channels/{id}/pacing
GET /api/v1/channels/{id}/planned
GET /api/v1/channels/{id}/runlist
GET /api/v1/channels/{id}/state
GET /api/v1/channels/{id}/weights
GET /api/v1/failover/heartbeat
GET /api/v1/failover/history
GET /api/v1/failover/status
GET /api/v1/flightplan/{ch}
GET /api/v1/flightplan/{ch}/audit
GET /api/v1/flightplan/{ch}/corrections
GET /api/v1/flightplan/{ch}/why
GET /api/v1/ingest/config
GET /api/v1/ingest/jobs
GET /api/v1/ingest/watch
GET /api/v1/inputs
GET /api/v1/inputs/ndi-config
GET /api/v1/library
GET /api/v1/live/facebook
GET /api/v1/media
GET /api/v1/media/collections
GET /api/v1/media/duplicates
GET /api/v1/media/export
GET /api/v1/media/upload-info
GET /api/v1/media/{id}
GET /api/v1/media/{id}/frameinfo
GET /api/v1/media/{id}/segments
GET /api/v1/media/{id}/storyboard
GET /api/v1/media/{id}/transcript
GET /api/v1/mesh/browse
GET /api/v1/mesh/jobs
GET /api/v1/mesh/nodes
GET /api/v1/mesh/search
GET /api/v1/mesh/status
GET /api/v1/mux/capabilities
GET /api/v1/mux/config
GET /api/v1/mux/dektec
GET /api/v1/mux/encoder-presets
GET /api/v1/mux/guide
GET /api/v1/mux/log
GET /api/v1/mux/scte35
GET /api/v1/mux/services
GET /api/v1/mux/status
GET /api/v1/mux/tables
GET /api/v1/mux/tables/analyze
GET /api/v1/mux/tables/overrides
GET /api/v1/outputs
GET /api/v1/overlays
GET /api/v1/overlays/cgslots
GET /api/v1/overlays/presets
GET /api/v1/overlays/renderer
GET /api/v1/overlays/rules
GET /api/v1/overlays/rules/test
GET /api/v1/overlays/{ch}
GET /api/v1/overlays/{ch}/cues
GET /api/v1/overlays/{ch}/defaults
GET /api/v1/pillar
GET /api/v1/pillar/config
GET /api/v1/pillar/{ch}/data
GET /api/v1/programming/facets
GET /api/v1/programming/recipes
GET /api/v1/publish/audit
GET /api/v1/publish/config
GET /api/v1/publish/items/{id}
GET /api/v1/publish/queue
GET /api/v1/publish/rails
GET /api/v1/publish/status
GET /api/v1/readiness
GET /api/v1/remote/nodes
GET /api/v1/remote/nodes/{id}/status
GET /api/v1/remote/overview
GET /api/v1/remote/pad
GET /api/v1/remote/pad/log
GET /api/v1/render/config
GET /api/v1/render/jobs
GET /api/v1/render/jobs/{id}
GET /api/v1/render/nodes
GET /api/v1/render/status
GET /api/v1/render/templates
GET /api/v1/reports
GET /api/v1/reports/
GET /api/v1/reports/asset/{id}
GET /api/v1/schedule/{ch}/slot/{slot}
GET /api/v1/schedule/{ch}/template
GET /api/v1/schedule/{ch}/week
GET /api/v1/sessions
GET /api/v1/sessions/export-live
GET /api/v1/sessions/marks
GET /api/v1/sessions/{id}
GET /api/v1/sessions/{id}/export
GET /api/v1/settings/ai.config
GET /api/v1/settings/live.facebook
GET /api/v1/settings/spots.llm
GET /api/v1/settings/{key}
GET /api/v1/spots/config
GET /api/v1/spots/jobs
GET /api/v1/spots/markers
GET /api/v1/spots/markets
GET /api/v1/spots/markets/{key}
GET /api/v1/spots/preview-script/{id}
GET /api/v1/spots/recipes
GET /api/v1/spots/scatter
GET /api/v1/spots/spot/{mediaId}
GET /api/v1/spots/status
GET /api/v1/spotstudio/options
GET /api/v1/spotstudio/projects
GET /api/v1/spotstudio/projects/{id}
GET /api/v1/star/agents
GET /api/v1/star/{node}/list
GET /api/v1/star/{node}/nowplaying
GET /api/v1/star/{node}/search
GET /api/v1/star/{node}/status
GET /api/v1/starnodes
GET /api/v1/starnodes/audit
GET /api/v1/starnodes/config
GET /api/v1/station/config
GET /api/v1/status
GET /api/v1/system/publisher
GET /api/v1/system/stats
GET /api/v1/time
GET /api/v1/traffic/contracts
GET /api/v1/traffic/contracts/{id}
GET /api/v1/transcribe/config
GET /api/v1/transcribe/jobs
GET /api/v1/transcribe/levels
GET /api/v1/transfers
GET /api/v2/playout/current
GET /api/watch-folders
PATCH /api/nodes/{nid}
POST /api/ai/test
POST /api/media/ingest
POST /api/newsroom/spot
POST /api/newsroom/spot/{mediaId}/revise
POST /api/nodes
POST /api/nodes/{nid}/refresh
POST /api/op/exec
POST /api/publish/schedule
POST /api/publish/{pid}/correct
POST /api/publish/{pid}/metrics
POST /api/publish/{pid}/retract
POST /api/publish/{pid}/send
POST /api/social/config
POST /api/social/queue
POST /api/social/queue/{pid}/approve
POST /api/social/queue/{pid}/retry
POST /api/social/queue/{pid}/skip
POST /api/social/verify
POST /api/stations/{sid}/call
POST /api/v1/agents/tasks
POST /api/v1/agents/tasks/clear
POST /api/v1/cameras
POST /api/v1/cameras/discover
POST /api/v1/cameras/{id}/ptz
POST /api/v1/cameras/{id}/refresh
POST /api/v1/cameras/{id}/register
POST /api/v1/cg/designer/generate
POST /api/v1/cg/designer/templates
POST /api/v1/cg/designer/templates/{id}/restore
POST /api/v1/cg/html/{ch}
POST /api/v1/cg/html/{ch}/clear
POST /api/v1/channels
POST /api/v1/channels/{id}/
POST /api/v1/channels/{id}/cg
POST /api/v1/channels/{id}/cg/factbox
POST /api/v1/channels/{id}/cg/karaoke
POST /api/v1/channels/{id}/monitor
POST /api/v1/channels/{id}/outputs/add
POST /api/v1/channels/{id}/outputs/{name}/
POST /api/v1/channels/{id}/playlist
POST /api/v1/channels/{id}/reroll
POST /api/v1/channels/{id}/transport
POST /api/v1/failover/config
POST /api/v1/failover/demote
POST /api/v1/failover/promote
POST /api/v1/flightplan/{ch}/apply
POST /api/v1/flightplan/{ch}/preview
POST /api/v1/ingest/file
POST /api/v1/ingest/jobs/clear
POST /api/v1/ingest/reprobe-all
POST /api/v1/ingest/rescan
POST /api/v1/ingest/sweep
POST /api/v1/inputs/scan
POST /api/v1/live/facebook/
POST /api/v1/media
POST /api/v1/media/bulk
POST /api/v1/media/delete
POST /api/v1/media/import
POST /api/v1/media/resolve
POST /api/v1/media/scan
POST /api/v1/media/{id}/relink
POST /api/v1/media/{id}/reprobe
POST /api/v1/media/{id}/segments/auto
POST /api/v1/media/{id}/segments/from-words
POST /api/v1/media/{id}/thumbnail
POST /api/v1/media/{id}/transcribe
POST /api/v1/mesh/fetch
POST /api/v1/mesh/jobs/clear
POST /api/v1/mesh/push
POST /api/v1/mux/dryrun
POST /api/v1/mux/preset
POST /api/v1/mux/restart
POST /api/v1/mux/scte35/cue
POST /api/v1/mux/start
POST /api/v1/mux/stop
POST /api/v1/mux/tables/compile
POST /api/v1/mux/validate
POST /api/v1/overlays/cgslots/assign
POST /api/v1/overlays/renderer/test
POST /api/v1/overlays/{ch}/cgslots/{id}/fire
POST /api/v1/overlays/{ch}/cgslots/{id}/hide
POST /api/v1/overlays/{ch}/clear
POST /api/v1/overlays/{ch}/defaults/apply
POST /api/v1/overlays/{ch}/deploy
POST /api/v1/overlays/{ch}/presets/{id}/fire
POST /api/v1/overlays/{ch}/presets/{id}/hide
POST /api/v1/overlays/{ch}/{layer}/hide
POST /api/v1/overlays/{ch}/{layer}/retake
POST /api/v1/overlays/{ch}/{layer}/update
POST /api/v1/pillar/refresh
POST /api/v1/pillar/{ch}/preview
POST /api/v1/programming/query/preview
POST /api/v1/programming/recipes/ad-break
POST /api/v1/programming/recipes/preview
POST /api/v1/publish/draft
POST /api/v1/publish/items/{id}/
POST /api/v1/publish/items/{id}/metrics
POST /api/v1/publish/items/{id}/reconcile
POST /api/v1/publish/items/{id}/resolve
POST /api/v1/publish/items/{id}/send
POST /api/v1/publish/release
POST /api/v1/publish/stop
POST /api/v1/publish/verify
POST /api/v1/remote/nodes/{id}/proxy
POST /api/v1/remote/nodes/{id}/pull-session/{sid}
POST /api/v1/remote/nodes/{id}/push-session/{sid}
POST /api/v1/remote/pad/{id}/press
POST /api/v1/render/jobs
POST /api/v1/render/jobs/clear
POST /api/v1/render/jobs/{id}/cancel
POST /api/v1/render/nodes
POST /api/v1/render/nodes/{id}/probe
POST /api/v1/render/probe
POST /api/v1/schedule/{ch}/clear
POST /api/v1/schedule/{ch}/copy
POST /api/v1/schedule/{ch}/hour-recipe
POST /api/v1/schedule/{ch}/import
POST /api/v1/schedule/{ch}/slot/{slot}/lock
POST /api/v1/schedule/{ch}/slot/{slot}/overrun
POST /api/v1/schedule/{ch}/slot/{slot}/resolved
POST /api/v1/schedule/{ch}/slot/{slot}/shuffle
POST /api/v1/schedule/{ch}/slot/{slot}/validate
POST /api/v1/schedule/{ch}/weighted-fill
POST /api/v1/sessions/apply-bundle
POST /api/v1/sessions/capture
POST /api/v1/sessions/import
POST /api/v1/sessions/receive
POST /api/v1/sessions/{id}/apply
POST /api/v1/sessions/{id}/clone
POST /api/v1/sessions/{id}/recapture
POST /api/v1/spots/generate
POST /api/v1/spots/jobs
POST /api/v1/spots/jobs/{id}/placements
POST /api/v1/spots/jobs/{id}/run
POST /api/v1/spots/markers/enforce
POST /api/v1/spots/preview-script
POST /api/v1/spots/scatter/apply
POST /api/v1/spots/scatter/generate
POST /api/v1/spots/spot/{mediaId}/revise
POST /api/v1/spots/sweep
POST /api/v1/spotstudio/projects
POST /api/v1/spotstudio/projects/{id}/generate
POST /api/v1/spotstudio/projects/{id}/publish
POST /api/v1/spotstudio/projects/{id}/review
POST /api/v1/star/discover
POST /api/v1/star/fetch
POST /api/v1/star/probe
POST /api/v1/star/push
POST /api/v1/star/{node}/call
POST /api/v1/starnodes/enrol
POST /api/v1/starnodes/probe
POST /api/v1/starnodes/{id}/action
POST /api/v1/starnodes/{id}/probe
POST /api/v1/transcribe/bulk
POST /api/v1/transcribe/jobs/clear
POST /api/v1/transcribe/probe
POST /api/v1/transfers/clear
PUT /api/nodes/{nid}
PUT /api/v1/agents/config
PUT /api/v1/ai/config
PUT /api/v1/alerts/twilio
PUT /api/v1/cg/designer/config
PUT /api/v1/cg/factbox/presets
PUT /api/v1/channels/{id}
PUT /api/v1/channels/{id}/autofill
PUT /api/v1/channels/{id}/mixes
PUT /api/v1/channels/{id}/outputs
PUT /api/v1/channels/{id}/outputs/{name}
PUT /api/v1/channels/{id}/weights
PUT /api/v1/failover/config
PUT /api/v1/ingest/config
PUT /api/v1/ingest/watch
PUT /api/v1/inputs/ndi-config
PUT /api/v1/live/facebook
PUT /api/v1/media/collections/{id}
PUT /api/v1/media/{id}
PUT /api/v1/media/{id}/segments
PUT /api/v1/media/{id}/segments/collections
PUT /api/v1/mesh/config
PUT /api/v1/mux/config
PUT /api/v1/mux/scte35
PUT /api/v1/mux/services
PUT /api/v1/mux/tables/overrides
PUT /api/v1/overlays/cgslots
PUT /api/v1/overlays/presets
PUT /api/v1/overlays/renderer
PUT /api/v1/overlays/rules
PUT /api/v1/overlays/{ch}/defaults
PUT /api/v1/pillar/config
PUT /api/v1/programming/recipes
PUT /api/v1/publish/config
PUT /api/v1/remote/nodes
PUT /api/v1/remote/pad
PUT /api/v1/render/config
PUT /api/v1/render/nodes
PUT /api/v1/render/nodes/{id}
PUT /api/v1/schedule/{ch}/slot/{slot}
PUT /api/v1/sessions/{id}
PUT /api/v1/settings/{key}
PUT /api/v1/spots/config
PUT /api/v1/spots/jobs/{id}
PUT /api/v1/spotstudio/projects/{id}
PUT /api/v1/star/agents
PUT /api/v1/starnodes/config
PUT /api/v1/starnodes/{id}
PUT /api/v1/station/config
PUT /api/v1/traffic/contracts
PUT /api/v1/transcribe/config
)DUMP";

QStringList routeDump() {
    QStringList keys;
    const QByteArray file = qgetenv("PP_ROUTE_DUMP");
    if (!file.isEmpty()) { QFile f(QString::fromLocal8Bit(file)); if (f.open(QIODevice::ReadOnly)) for (const QByteArray& l : f.readAll().split('\n')) if (!l.trimmed().isEmpty()) keys << QString::fromUtf8(l.trimmed()); }
    if (keys.isEmpty()) for (const QString& l : QString::fromUtf8(kRouteDump).split('\n')) if (!l.trimmed().isEmpty()) keys << l.trimmed();
    return keys;
}

// --- the matcher the server had before PS-9 (restapiserver.cpp:267-290), kept here as the oracle --------------------
// Returns every registered key the old wildcard scan would have accepted for (method, path): the old code took the
// exact key when present, else the first match in QHash order, so any member of this set was a possible answer.
struct OldMatch { QStringList keys; QHash<QString, QHash<QString, QString>> params; bool exact = false; };
OldMatch oldMatcher(const QStringList& routes, const QString& method, const QString& path) {
    OldMatch m;
    const QString routeKey = method + " " + path;
    if (routes.contains(routeKey)) { m.keys << routeKey; m.exact = true; return m; }
    const QStringList pathParts = path.split('/', Qt::SkipEmptyParts);
    for (const QString& key : routes) {
        const QStringList routeParts = key.split(' ', Qt::SkipEmptyParts);
        if (routeParts.size() < 2 || routeParts[0] != method) continue;
        const QStringList rp = routeParts[1].split('/', Qt::SkipEmptyParts);
        if (rp.size() != pathParts.size()) continue;
        bool match = true; QHash<QString, QString> params;
        for (int i = 0; i < rp.size(); ++i) {
            if (rp[i].startsWith("{") && rp[i].endsWith("}")) params[rp[i].mid(1, rp[i].length() - 2)] = pathParts[i];
            else if (rp[i] != pathParts[i]) { match = false; break; }
        }
        if (match) { m.keys << key; m.params[key] = params; }
    }
    return m;
}

// --- blocking HTTP client on its own thread (the server shares the test's main loop) ---------------------------------
struct Resp { int status = 0; QByteArray headers, body; bool closed = false; bool ok = false; QString error; };

Resp parseResponse(QTcpSocket& s, int timeoutMs) {
    Resp r; QByteArray buf; QElapsedTimer t; t.start();
    for (;;) {
        const int he = buf.indexOf("\r\n\r\n");
        if (he >= 0) {
            r.headers = buf.left(he);
            const QRegularExpressionMatch cl = QRegularExpression("Content-Length:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption).match(QString::fromUtf8(r.headers));
            const int len = cl.hasMatch() ? cl.captured(1).toInt() : -1;
            if (len >= 0) {
                while (buf.size() < he + 4 + len && t.elapsed() < timeoutMs) { if (!s.waitForReadyRead(qMax(1, int(timeoutMs - t.elapsed())))) break; buf += s.readAll(); }
                r.body = buf.mid(he + 4, len); r.ok = buf.size() >= he + 4 + len;
            } else {
                while (s.state() == QAbstractSocket::ConnectedState && t.elapsed() < timeoutMs) { if (!s.waitForReadyRead(200)) continue; buf += s.readAll(); }
                r.body = buf.mid(he + 4); r.ok = true;
            }
            r.status = QString::fromUtf8(r.headers.left(r.headers.indexOf("\r\n"))).section(' ', 1, 1).toInt();
            return r;
        }
        if (t.elapsed() >= timeoutMs) { r.error = "timeout waiting for headers"; return r; }
        if (!s.waitForReadyRead(qMax(1, int(timeoutMs - t.elapsed())))) { if (s.state() != QAbstractSocket::ConnectedState) { r.closed = true; r.error = "closed before headers"; return r; } continue; }
        buf += s.readAll();
    }
}

// Runs `fn` on a worker thread while the caller spins the main loop.
template <typename F> void onThread(F fn, int timeoutMs = 30000) {
    std::atomic<bool> done{false};
    QThread* t = QThread::create([&]() { fn(); done = true; });
    t->start();
    QElapsedTimer e; e.start();
    while (!done && e.elapsed() < timeoutMs) QTest::qWait(1);
    t->wait(5000); delete t;
}

Resp request(quint16 port, const QByteArray& raw, int timeoutMs = 5000) {
    Resp r;
    onThread([&]() {
        QTcpSocket s; s.connectToHost("127.0.0.1", port);
        if (!s.waitForConnected(timeoutMs)) { r.error = "connect: " + s.errorString(); return; }
        s.write(raw); s.waitForBytesWritten(timeoutMs);
        r = parseResponse(s, timeoutMs);
        if (!r.ok && r.error.isEmpty()) r.error = s.errorString();
        // closed-after-reply detection (431 / 413)
        if (r.ok) { QElapsedTimer t; t.start(); while (s.state() == QAbstractSocket::ConnectedState && t.elapsed() < 1500) s.waitForReadyRead(100); r.closed = s.state() != QAbstractSocket::ConnectedState; }
    }, timeoutMs + 3000);
    return r;
}

QByteArray get(const QString& path, const QByteArray& extra = QByteArray()) { return "GET " + path.toUtf8() + " HTTP/1.1\r\nHost: t\r\n" + extra + "\r\n"; }

struct Captured { QStringList lines; };
Captured* g_cap = nullptr;
void capHandler(QtMsgType, const QMessageLogContext&, const QString& msg) { if (g_cap) g_cap->lines << msg; fprintf(stderr, "%s\n", qPrintable(msg)); }

qint64 handleCount() {
#ifdef Q_OS_WIN
    DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n); return n;
#else
    return 0;
#endif
}

AsRunRow row(int i) {
    AsRunRow r; r.channelId = (i % 3 == 0) ? "ch2" : "main"; r.runlistItemId = QString("s:%1").arg(i); r.itemId = QString("i%1").arg(i);
    r.assetId = QString("a%1").arg(i % 50); r.title = QString("Clip %1").arg(i); r.type = "video";
    r.eventType = (i % 17 == 0) ? "FAILED" : (i % 11 == 0) ? "SKIP" : "START"; r.origin = (i % 7 == 0) ? "fill" : "schedule";
    r.plannedStartMs = 1700000000000LL + qint64(i) * 30000; r.actualStartMs = r.plannedStartMs + (i % 5) * 400 - 800;
    r.plannedDurationMs = 29000; r.actualDurationMs = 28500 + (i % 3) * 200; r.plannedEndMs = r.plannedStartMs + 29000; r.actualEndMs = r.actualStartMs + r.actualDurationMs;
    r.varianceMs = r.actualStartMs - r.plannedStartMs; r.frame = i * 900; r.timecode = "00:00:00:00";
    r.status = (i % 5 == 2) ? "ON_TIME" : (i % 5 == 3) ? "LATE_START" : "EARLY_START"; r.reason = ""; r.slotId = QString("slot%1").arg(i / 120);
    r.extra = QJsonObject{{"n", i}};
    return r;
}

} // namespace

class TestRestApiServer : public QObject {
    Q_OBJECT
private slots:
    void routeTrieMatchesOldMatcher();
    void paramsBindIntoQuery();
    void idleKeepAliveSocketsAreClosed();
    void handleCountStableOver10kRequests();
    void oversizeHeaderGets431AndClose();
    void slowHandlerLoggedWithWatchdogTag();
    void storeReaderMatchesStore();
    void storeReaderKeepsLoopFree();
    void storeReaderShutdown();
};

void TestRestApiServer::routeTrieMatchesOldMatcher() {
    const QStringList routes = routeDump();
    QVERIFY2(routes.size() >= 300, qPrintable(QString("route dump has %1 keys").arg(routes.size())));
    RestApiServer srv(nullptr);
    int params = 0;
    for (const QString& k : routes) { const int sp = k.indexOf(' '); srv.registerRoute(k.left(sp), k.mid(sp + 1), [](const QJsonObject&, const QHash<QString, QString>&) { return QJsonObject(); }); if (k.contains('{')) ++params; }
    QCOMPARE(srv.routeKeys().size(), routes.size());
    qInfo() << "routes" << routes.size() << "with params" << params;

    // Every key, with each {param} replaced by a value, plus a trailing-slash variant and a wrong-method variant.
    int checked = 0, ambiguous = 0, exact = 0;
    QElapsedTimer t; qint64 oldNs = 0, newNs = 0;
    const QStringList values = {"main", "42", "a-b_c.d", "x"};
    for (const QString& k : routes) {
        const int sp = k.indexOf(' '); const QString method = k.left(sp), pattern = k.mid(sp + 1);
        for (int v = 0; v < values.size(); ++v) {
            QString path = pattern; int pi = 0; QHash<QString, QString> expectParams;
            for (;;) { const int a = path.indexOf('{'); if (a < 0) break; const int b = path.indexOf('}', a); const QString name = path.mid(a + 1, b - a - 1); const QString val = values[(v + pi++) % values.size()]; expectParams[name] = val; path.replace(a, b - a + 1, val); }
            for (const QString& p : {path, path + "/"}) {
                t.start(); const OldMatch old = oldMatcher(routes, method, p); oldNs += t.nsecsElapsed();
                QHash<QString, QString> got; t.start(); const QString key = srv.matchRoute(method, p, &got); newNs += t.nsecsElapsed();
                ++checked;
                QVERIFY2(!old.keys.isEmpty(), qPrintable("old matcher found nothing for " + method + " " + p));
                QVERIFY2(old.keys.contains(key), qPrintable(QString("trie gave '%1' for %2 %3; old matcher accepted [%4]").arg(key, method, p, old.keys.join(", "))));
                if (old.exact) ++exact; else { if (old.keys.size() > 1) ++ambiguous; QCOMPARE(got, old.params.value(key)); }
                // a different method must not match (unless that method's own route exists)
                const QString other = method == "GET" ? "DELETE" : "GET";
                const OldMatch oo = oldMatcher(routes, other, p);
                const QString ok2 = srv.matchRoute(other, p, nullptr);
                QVERIFY2(oo.keys.isEmpty() ? ok2.isEmpty() : oo.keys.contains(ok2), qPrintable("method mismatch for " + other + " " + p));
            }
        }
    }
    // Paths nothing matches.
    for (const QString& p : {"/api/v1/nope", "/api/v1/channels/a/b/c/d/e/f", "/", "/api", "/api/v1/channels/main/start/extra"}) {
        const OldMatch old = oldMatcher(routes, "GET", p);
        const QString key = srv.matchRoute("GET", p, nullptr);
        QVERIFY2(old.keys.isEmpty() ? key.isEmpty() : old.keys.contains(key), qPrintable("negative path " + p + " -> " + key));
    }
    qInfo() << "checked" << checked << "paths; exact" << exact << "ambiguous under the old matcher" << ambiguous
            << "; old scan avg" << (oldNs / checked / 1000.0) << "us, trie avg" << (newNs / checked / 1000.0) << "us";
    QVERIFY(newNs < oldNs);   // the trie beats a linear split of ~413 keys per request
    // Re-registration after the trie was built is picked up.
    srv.registerRoute("GET", "/api/v1/late/{id}/x", [](const QJsonObject&, const QHash<QString, QString>&) { return QJsonObject(); });
    QHash<QString, QString> p2; QCOMPARE(srv.matchRoute("GET", "/api/v1/late/7/x", &p2), QString("GET /api/v1/late/{id}/x")); QCOMPARE(p2.value("id"), QString("7"));
    // copyRoutes carries the table and rebuilds its own trie.
    RestApiServer second(nullptr); second.copyRoutes(srv);
    QCOMPARE(second.matchRoute("GET", "/api/v1/late/9/x", nullptr), QString("GET /api/v1/late/{id}/x"));
    QCOMPARE(second.routeKeys().size(), routes.size() + 1);
}

void TestRestApiServer::paramsBindIntoQuery() {
    RestApiServer srv(nullptr);
    QVERIFY(srv.start(0)); const quint16 port = srv.port(); QVERIFY(port > 0);
    srv.registerRoute("GET", "/api/v1/channels/{id}/slots/{slot}", [](const QJsonObject&, const QHash<QString, QString>& q) { QJsonObject o; o["id"] = q.value("id"); o["slot"] = q.value("slot"); o["extra"] = q.value("extra"); return o; });
    srv.registerRoute("GET", "/api/v1/channels/{id}", [](const QJsonObject&, const QHash<QString, QString>& q) { QJsonObject o; o["only"] = q.value("id"); return o; });
    srv.registerRoute("GET", "/api/v1/channels/all", [](const QJsonObject&, const QHash<QString, QString>&) { QJsonObject o; o["literal"] = true; return o; });
    Resp r = request(port, get("/api/v1/channels/main/slots/s3?extra=1"));
    QVERIFY2(r.ok, qPrintable(r.error)); QCOMPARE(r.status, 200);
    QJsonObject o = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(o.value("id").toString(), QString("main")); QCOMPARE(o.value("slot").toString(), QString("s3")); QCOMPARE(o.value("extra").toString(), QString("1"));
    r = request(port, get("/api/v1/channels/all")); QVERIFY(QJsonDocument::fromJson(r.body).object().value("literal").toBool());   // literal before {param}
    r = request(port, get("/api/v1/channels/ch2")); QCOMPARE(QJsonDocument::fromJson(r.body).object().value("only").toString(), QString("ch2"));
    r = request(port, get("/api/v1/none")); QCOMPARE(r.status, 404);
    r = request(port, get("/api/v1/channels/ch2/slots")); QCOMPARE(r.status, 404);
    QCOMPARE(srv.stats().requests, quint64(5));
}

void TestRestApiServer::idleKeepAliveSocketsAreClosed() {
    RestApiServer srv(nullptr);
    srv.setIdleTimeoutMs(15000);   // long while the 2,000 connect: nothing may be closed before its time
    QVERIFY(srv.start(0)); const quint16 port = srv.port();
    // An async route whose reply takes 2.5 s: that connection is busy, not idle, and must survive the sweep.
    srv.registerAsync("GET", "/api/v1/slow", [](const QJsonObject&, const QHash<QString, QString>&, RestApiServer::Reply reply) {
        QTimer::singleShot(2500, [reply]() { QJsonObject o; o["late"] = true; reply(200, o); });
    });
    const int N = 2000;
    QList<QTcpSocket*> clients;
    QElapsedTimer t; t.start();
    for (int i = 0; i < N; ++i) {
        auto* s = new QTcpSocket(this); s->setProxy(QNetworkProxy::NoProxy); s->connectToHost("127.0.0.1", port); clients << s;
        if (i == 0) { QTest::qWait(200); qInfo() << "first client state" << s->state() << s->errorString() << "server tracks" << srv.connectionCount(); }
        if (i % 25 == 24) QTest::qWait(10);   // let the server accept (its backlog is finite)
    }
    QElapsedTimer w; w.start();
    while (srv.connectionCount() < N && w.elapsed() < 20000) QTest::qWait(20);
    int connected = 0; for (auto* s : clients) if (s->state() == QAbstractSocket::ConnectedState) ++connected;
    qInfo() << "server tracks" << srv.connectionCount() << "connections, clients connected" << connected << "after" << t.elapsed() << "ms";
    QCOMPARE(srv.connectionCount(), N);
    QCOMPARE(srv.stats().idleClosed, quint64(0));
    QVERIFY2(t.elapsed() < 15000, "connecting took longer than the long idle timeout; the sweep would have run");
    // Now the production-like short timeout (1 s; the sweep runs every 250 ms): every one of them has been idle
    // longer than that already.
    srv.setIdleTimeoutMs(1000);
    // One busy connection on top, started now so it is younger than the idle ones.
    QTcpSocket busySock; busySock.connectToHost("127.0.0.1", port); QVERIFY(busySock.waitForConnected(2000));
    busySock.write(get("/api/v1/slow")); QByteArray busyBuf;
    connect(&busySock, &QTcpSocket::readyRead, this, [&]() { busyBuf += busySock.readAll(); });
    QTest::qWait(50);
    QCOMPARE(srv.connectionCount(), N + 1);
    // Idle sockets go between idleMs and idleMs + sweep interval (250 ms) after their last activity.
    w.restart();
    while (srv.connectionCount() > 1 && w.elapsed() < 6000) QTest::qWait(20);
    qInfo() << "idle sockets closed" << w.elapsed() << "ms after the timeout was set; tracked now" << srv.connectionCount() << "; idleClosed" << srv.stats().idleClosed;
    QCOMPARE(srv.connectionCount(), 1);
    QCOMPARE(srv.stats().idleClosed, quint64(N));
    QTest::qWait(200);
    int clientClosed = 0; for (auto* s : clients) if (s->state() != QAbstractSocket::ConnectedState) ++clientClosed;
    QCOMPARE(clientClosed, N);
    QVERIFY2(busySock.state() == QAbstractSocket::ConnectedState, "the busy (async-pending) connection was closed by the sweep");
    // Its reply still arrives, and after it the connection becomes idle and is closed too.
    w.restart(); while (!busyBuf.contains("\"late\":true") && w.elapsed() < 5000) QTest::qWait(20);
    QVERIFY2(busyBuf.contains("\"late\":true"), "late async reply missing");
    w.restart(); while (srv.connectionCount() > 0 && w.elapsed() < 3000) QTest::qWait(20);
    QCOMPARE(srv.connectionCount(), 0);
    for (auto* s : clients) delete s;
}

void TestRestApiServer::handleCountStableOver10kRequests() {
    RestApiServer srv(nullptr);
    QVERIFY(srv.start(0)); const quint16 port = srv.port();
    srv.registerRoute("GET", "/api/v1/status", [](const QJsonObject&, const QHash<QString, QString>&) { QJsonObject o; o["ok"] = true; o["pad"] = QString(200, 'x'); return o; });
    srv.registerRoute("POST", "/api/v1/echo/{id}", [](const QJsonObject& b, const QHash<QString, QString>& q) { QJsonObject o = b; o["id"] = q.value("id"); return o; });
    // Warm-up (allocations the first request makes are not a leak).
    Resp r = request(port, get("/api/v1/status")); QVERIFY2(r.ok, qPrintable(r.error));
    QTest::qWait(100);
    const qint64 h0 = handleCount();
    const int conns = 10, per = 1000;   // 10,000 requests over 10 keep-alive connections, plus 200 one-shot ones
    std::atomic<int> okCount{0}, bad{0};
    QElapsedTimer t; t.start();
    onThread([&]() {
        QList<QThread*> ts;
        for (int c = 0; c < conns; ++c) ts << QThread::create([&, c]() {
            QTcpSocket s; s.connectToHost("127.0.0.1", port);
            if (!s.waitForConnected(5000)) { ++bad; return; }
            for (int i = 0; i < per; ++i) {
                const bool post = i % 4 == 3;
                const QByteArray body = "{\"n\":" + QByteArray::number(i) + "}";
                const QByteArray raw = post ? "POST /api/v1/echo/c" + QByteArray::number(c) + " HTTP/1.1\r\nHost: t\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body
                                            : get("/api/v1/status");
                s.write(raw);
                const Resp rr = parseResponse(s, 5000);
                if (rr.ok && rr.status == 200 && (!post || rr.body.contains("\"n\":" + QByteArray::number(i)))) ++okCount; else { ++bad; return; }
            }
            s.disconnectFromHost(); if (s.state() != QAbstractSocket::UnconnectedState) s.waitForDisconnected(1000);
        });
        for (auto* th : ts) th->start();
        for (auto* th : ts) { th->wait(60000); delete th; }
        // 200 connections that send one request each and go away without saying so (abort): the server must still
        // let go of every socket.
        for (int i = 0; i < 200; ++i) {
            QTcpSocket s; s.connectToHost("127.0.0.1", port);
            if (!s.waitForConnected(5000)) { ++bad; continue; }
            s.write(get("/api/v1/status", "Connection: close\r\n"));
            const Resp rr = parseResponse(s, 5000);
            if (rr.ok && rr.status == 200) ++okCount; else ++bad;
            s.abort();
        }
    }, 120000);
    QTest::qWait(300);
    const qint64 h1 = handleCount();
    qInfo() << "requests ok" << okCount.load() << "bad" << bad.load() << "in" << t.elapsed() << "ms; handles" << h0 << "->" << h1 << "; tracked connections" << srv.connectionCount() << "; server stats requests" << srv.stats().requests << "maxHandlerMs" << srv.stats().maxHandlerMs;
    QCOMPARE(bad.load(), 0);
    QCOMPARE(okCount.load(), conns * per + 200);
    QCOMPARE(srv.connectionCount(), 0);
#ifdef Q_OS_WIN
    QVERIFY2(h1 - h0 < 40, qPrintable(QString("handle count grew by %1 over 10,200 requests").arg(h1 - h0)));
#endif
}

void TestRestApiServer::oversizeHeaderGets431AndClose() {
    RestApiServer srv(nullptr);
    QVERIFY(srv.start(0)); const quint16 port = srv.port();
    srv.registerRoute("GET", "/api/v1/status", [](const QJsonObject&, const QHash<QString, QString>&) { QJsonObject o; o["ok"] = true; return o; });
    // 1) headers that never end, over 64 KB: 431 and the connection is dropped.
    QByteArray big = "GET /api/v1/status HTTP/1.1\r\nHost: t\r\n";
    while (big.size() < 70 * 1024) big += "X-Pad: " + QByteArray(1000, 'a') + "\r\n";
    Resp r = request(port, big);
    QVERIFY2(r.ok, qPrintable(r.error)); QCOMPARE(r.status, 431); QVERIFY(r.headers.contains("Connection: close")); QVERIFY(r.closed);
    QCOMPARE(srv.stats().headerTooLarge, quint64(1));
    // 2) a complete header block just over the cap: also 431.
    QByteArray big2 = "GET /api/v1/status HTTP/1.1\r\nHost: t\r\nX-Pad: " + QByteArray(65 * 1024, 'b') + "\r\n\r\n";
    r = request(port, big2); QCOMPARE(r.status, 431); QVERIFY(r.closed);
    // 3) a 60 KB header is fine, keep-alive kept.
    QByteArray okHdr = "GET /api/v1/status HTTP/1.1\r\nHost: t\r\nX-Pad: " + QByteArray(60 * 1024, 'c') + "\r\n\r\n";
    r = request(port, okHdr); QCOMPARE(r.status, 200); QVERIFY(!r.closed);
    // 4) a body over the cap: 413 and close, without buffering it.
    srv.setMaxBodyBytes(1024);
    r = request(port, "POST /api/v1/status HTTP/1.1\r\nHost: t\r\nContent-Length: 5000000\r\n\r\n" + QByteArray(100, 'd'));
    QCOMPARE(r.status, 413); QVERIFY(r.closed); QCOMPARE(srv.stats().bodyTooLarge, quint64(1));
    QTest::qWait(100);
    QCOMPARE(srv.connectionCount(), 0);   // every dropped connection is forgotten
    // The server still serves afterwards.
    r = request(port, get("/api/v1/status")); QCOMPARE(r.status, 200);
}

void TestRestApiServer::slowHandlerLoggedWithWatchdogTag() {
    RestApiServer srv(nullptr);
    QVERIFY(srv.start(0)); const quint16 port = srv.port();
    QByteArray tagSeen, tagAfter;
    srv.registerRoute("GET", "/api/v1/slow/{id}", [&](const QJsonObject&, const QHash<QString, QString>&) { tagSeen = LoopWatchdog::activity(); QThread::msleep(80); QJsonObject o; o["ok"] = true; return o; });
    srv.registerRoute("GET", "/api/v1/fast", [&](const QJsonObject&, const QHash<QString, QString>&) { QJsonObject o; o["ok"] = true; return o; });
    Captured cap; g_cap = &cap; const QtMessageHandler prev = qInstallMessageHandler(capHandler);
    Resp r = request(port, get("/api/v1/slow/7")); QCOMPARE(r.status, 200);
    tagAfter = LoopWatchdog::activity();
    r = request(port, get("/api/v1/fast")); QCOMPARE(r.status, 200);
    qInstallMessageHandler(prev); g_cap = nullptr;
    QCOMPARE(tagSeen, QByteArray("rest:GET /api/v1/slow/7"));   // the watchdog names the route during the handler
    QVERIFY2(tagAfter != tagSeen, "activity tag not restored after the handler");
    QStringList slow; for (const QString& l : cap.lines) if (l.contains("slow handler")) slow << l;
    QCOMPARE(slow.size(), 1);
    QVERIFY2(slow[0].contains("GET /api/v1/slow/7") && slow[0].contains("ms"), qPrintable(slow[0]));
    QCOMPARE(srv.stats().slow, quint64(1));
    QVERIFY(srv.stats().maxHandlerMs >= 80);
    // Threshold 0 turns the log off.
    srv.setSlowHandlerMs(0); g_cap = &cap; cap.lines.clear(); qInstallMessageHandler(capHandler);
    r = request(port, get("/api/v1/slow/8")); qInstallMessageHandler(prev); g_cap = nullptr;
    for (const QString& l : cap.lines) QVERIFY(!l.contains("slow handler"));
}

void TestRestApiServer::storeReaderMatchesStore() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("pp.db");
    ScheduleStore store; QVERIFY(store.open(path));
    const int N = 3000;
    for (int i = 0; i < N; ++i) QVERIFY(store.insertAsRun(row(i)) > 0);
    // A reader on a missing file is not open and refuses jobs (the Supervisor then answers from the store).
    { StoreReader none(dir.filePath("missing.db"), 1); QVERIFY(!none.isOpen()); QVERIFY(!none.run([](QSqlDatabase&) { return QJsonObject(); }, [](const QJsonObject&) {})); }
    StoreReader reader(path, 2);
    QVERIFY(reader.isOpen()); QCOMPARE(reader.threads(), 2);
    const qint64 from = row(0).actualStartMs - 1, to = row(N - 1).actualStartMs + 1;
    auto wait = [&](std::function<bool()> cond, int ms = 10000) { QElapsedTimer t; t.start(); while (!cond() && t.elapsed() < ms) QTest::qWait(5); return cond(); };
    // /asrun body: the reader's report equals the store's own rows and summary, field by field.
    for (const QString& channel : {QString(), QString("main"), QString("ch2")}) {
        for (int limit : {5000, 100, 1}) {
            QJsonObject got; bool done = false;
            QVERIFY(reader.run([=](QSqlDatabase& db) { return StoreReader::asRunReport(db, channel, from, to, limit); }, [&](const QJsonObject& o) { got = o; done = true; }));
            QVERIFY(wait([&]() { return done; }));
            QJsonObject want; QJsonArray a; for (const auto& r2 : store.asRun(channel, from, to, limit)) a.append(r2.toJson());
            want["rows"] = a; want["count"] = a.size(); want["fromMs"] = double(from); want["toMs"] = double(to); want["summary"] = store.asRunSummary(channel, from, to);
            QVERIFY2(got == want, qPrintable(QString("channel '%1' limit %2: count %3 vs %4").arg(channel).arg(limit).arg(got.value("count").toInt()).arg(want.value("count").toInt())));
            QVERIFY(got.value("count").toInt() == qMin(limit, a.size()));
        }
    }
    // The summary route.
    { QJsonObject got; bool done = false; reader.run([=](QSqlDatabase& db) { return StoreReader::asRunSummary(db, "main", from, to); }, [&](const QJsonObject& o) { got = o; done = true; }); QVERIFY(wait([&]() { return done; })); QCOMPARE(got, store.asRunSummary("main", from, to)); }
    // Rows written after the readers opened are visible (WAL readers see committed data).
    store.insertAsRun(row(N));
    { QJsonObject got; bool done = false; reader.run([=](QSqlDatabase& db) { return StoreReader::asRunReport(db, QString(), from, to + 1000000, 10000); }, [&](const QJsonObject& o) { got = o; done = true; }); QVERIFY(wait([&]() { return done; })); QCOMPARE(got.value("count").toInt(), N + 1); }
    // Read-only: a write through the reader fails and the file is untouched.
    { QJsonObject got; bool done = false; reader.run([](QSqlDatabase& db) { QSqlQuery q(db); const bool ok = q.exec("DELETE FROM as_run"); QJsonObject o; o["wrote"] = ok; return o; }, [&](const QJsonObject& o) { got = o; done = true; }); QVERIFY(wait([&]() { return done; })); QVERIFY(!got.value("wrote").toBool()); }
    QCOMPARE(store.asRun(QString(), from, to + 1000000, 10000).size(), N + 1);
    // Through the server, wired as supervisorcore.cpp does.
    RestApiServer srv(nullptr); QVERIFY(srv.start(0));
    srv.registerAsync("GET", "/api/v1/asrun", [&](const QJsonObject&, const QHash<QString, QString>& q, RestApiServer::Reply reply) {
        const QString channel = q.value("channel"); const int limit = q.value("limit", "5000").toInt();
        const bool queued = reader.run([=](QSqlDatabase& db) { return StoreReader::asRunReport(db, channel, from, to, limit); }, [reply](const QJsonObject& o) { reply(o.contains("error") ? o.value("status").toInt(503) : 200, o); });
        QVERIFY(queued);
    });
    Resp r = request(srv.port(), get("/api/v1/asrun?channel=ch2&limit=50")); QCOMPARE(r.status, 200);
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(o.value("count").toInt(), 50); QCOMPARE(o.value("rows").toArray().size(), 50);
    QCOMPARE(o.value("summary").toObject(), store.asRunSummary("ch2", from, to));
    QCOMPARE(o.value("rows").toArray().first().toObject().value("channelId").toString(), QString("ch2"));
    qInfo() << "reader completed" << reader.completed() << "jobs; store rows" << N + 1;
}

void TestRestApiServer::storeReaderKeepsLoopFree() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("pp.db");
    ScheduleStore store; QVERIFY(store.open(path));
    for (int i = 0; i < 200; ++i) store.insertAsRun(row(i));
    StoreReader reader(path, 2); QVERIFY(reader.isOpen());
    RestApiServer srv(nullptr); QVERIFY(srv.start(0)); const quint16 port = srv.port();
    srv.registerRoute("GET", "/api/v1/status", [](const QJsonObject&, const QHash<QString, QString>&) { QJsonObject o; o["ok"] = true; return o; });
    // /asrun whose read takes 2 s on the reader thread (a huge table stands in for the sleep).
    srv.registerAsync("GET", "/api/v1/asrun", [&](const QJsonObject&, const QHash<QString, QString>&, RestApiServer::Reply reply) {
        reader.run([](QSqlDatabase& db) { QThread::msleep(2000); return StoreReader::asRunReport(db, QString(), 0, 1LL << 60, 5000); }, [reply](const QJsonObject& o) { reply(200, o); });
    });
    // Loop gauge: a 10 ms timer; the largest gap is the longest the loop was held.
    qint64 maxGap = 0, last = 0; int fires = 0; QElapsedTimer gauge; gauge.start();
    QTimer tick; tick.setTimerType(Qt::PreciseTimer); tick.setInterval(10);
    connect(&tick, &QTimer::timeout, this, [&]() { const qint64 now = gauge.elapsed(); if (fires) maxGap = qMax(maxGap, now - last); last = now; ++fires; });
    tick.start();
    Resp slow, fast; std::atomic<bool> slowDone{false}, fastDone{false}; qint64 fastMs = -1, slowMs = -1;
    QThread* ts = QThread::create([&]() { QElapsedTimer t; t.start(); QTcpSocket s; s.connectToHost("127.0.0.1", port); s.waitForConnected(3000); s.write(get("/api/v1/asrun")); slow = parseResponse(s, 8000); slowMs = t.elapsed(); slowDone = true; });
    ts->start();
    QTest::qWait(300);   // the 2 s read is in progress now
    QCOMPARE(reader.running(), 1);
    QThread* tf = QThread::create([&]() { QElapsedTimer t; t.start(); QTcpSocket s; s.connectToHost("127.0.0.1", port); s.waitForConnected(3000); s.write(get("/api/v1/status")); fast = parseResponse(s, 8000); fastMs = t.elapsed(); fastDone = true; });
    tf->start();
    QElapsedTimer w; w.start(); while (!(slowDone && fastDone) && w.elapsed() < 10000) QTest::qWait(5);
    tick.stop(); ts->wait(2000); tf->wait(2000); delete ts; delete tf;
    qInfo() << "/status answered in" << fastMs << "ms while /asrun took" << slowMs << "ms; loop max gap" << maxGap << "ms over" << fires << "fires";
    QVERIFY2(fast.ok && fast.status == 200, qPrintable(fast.error));
    QVERIFY2(slow.ok && slow.status == 200, qPrintable(slow.error));
    QCOMPARE(QJsonDocument::fromJson(slow.body).object().value("count").toInt(), 200);
    QVERIFY2(fastMs < 500, qPrintable(QString("/status took %1 ms during the 2 s read").arg(fastMs)));
    QVERIFY2(slowMs >= 2000, "the slow read did not take its 2 s");
    QVERIFY2(maxGap < 100, qPrintable(QString("loop max gap %1 ms").arg(maxGap)));
    QCOMPARE(srv.stats().slow, quint64(0));   // nothing held the loop over 50 ms
}

void TestRestApiServer::storeReaderShutdown() {
    QTemporaryDir dir; QVERIFY(dir.isValid());
    const QString path = dir.filePath("pp.db");
    { ScheduleStore store; QVERIFY(store.open(path)); store.insertAsRun(row(1)); }
    std::atomic<int> delivered{0};
    QElapsedTimer t;
    {
        StoreReader reader(path, 1); QVERIFY(reader.isOpen());
        reader.setDrainMs(3000);
        reader.run([](QSqlDatabase&) { QThread::msleep(1000); return QJsonObject(); }, [&](const QJsonObject&) { ++delivered; });
        reader.run([](QSqlDatabase&) { return QJsonObject(); }, [&](const QJsonObject&) { ++delivered; });   // queued behind: dropped at shutdown
        QTest::qWait(100);
        QCOMPARE(reader.running(), 1); QCOMPARE(reader.queued(), 1);
        t.start();
    }   // destructor: waits for the 1 s job, drops the queued one, never delivers after
    const qint64 dtor = t.elapsed();
    QTest::qWait(200);
    qInfo() << "destructor returned after" << dtor << "ms; delivered" << delivered.load();
    QVERIFY2(dtor >= 800 && dtor < 2500, qPrintable(QString("destructor took %1 ms").arg(dtor)));
    QCOMPARE(delivered.load(), 0);
    // The connection names are gone, so a new reader on the same file opens cleanly.
    StoreReader again(path, 1); QVERIFY(again.isOpen());
    QCOMPARE(QSqlDatabase::connectionNames().filter("ppreader_").size(), 1);
}

QTEST_MAIN(TestRestApiServer)
#include "test_restapiserver.moc"
