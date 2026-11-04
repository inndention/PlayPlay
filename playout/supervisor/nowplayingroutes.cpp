// Now Playing routes (docs/nowplaying.md): the one-call-per-second bundle the web "Now Playing" dashboard polls.
//
//   GET /api/v1/nowplaying              lane info: routes, where per-input snapshots live, cache window
//   GET /api/v1/nowplaying/{id}         everything the page needs for channel {id} in one JSON (see docs/nowplaying.md "Bundle")
//
// A lane-extension module (playout/supervisor/laneext.h): it is compiled into PlayPlaySupervisor from cmake/lane-nowplaying.cmake
// and registers itself with a static laneext::Registrar, so supervisorcore*.cpp stays untouched. The laneext context carries no
// SupervisorCore (telemetry, executor, overlay manager live there), so the bundle is assembled from the Supervisor's own REST
// routes over loopback (ctx.restPort + ctx.restToken, the way the MasterPlay lane posts to itself): /channels/{id}, /state,
// /runlist, /overlays/{ch}, /overlays/cgslots, /system/stats and /live/facebook, fetched in parallel, plus the switcher map
// straight from the store (setting ui.inputMap, shared with the desktop InputMap). One upstream failure does not fail the
// bundle: the part is left out and named in "errors".
//
// Per-input pictures need no new route: the Supervisor already files the engine's input monitors under "<ch>:in<N>", served as
// /preview/<ch>:in<N>.jpg (JPEG) and .mjpeg; the bundle carries each slot's snapshotUrl. (A raw route under /api/v1/channels/
// is impossible here: raw handlers match by prefix before the JSON trie and would take every channel route with them.)
//
// Cost control: concurrent requests for the same channel share one assembly (they are coalesced), and a bundle younger than
// kCacheMs is served again as is, so any number of dashboards cost the Supervisor about seven loopback calls a second.
#include "laneext.h"
#include "restapiserver.h"
#include "../schedule/schedulestore.h"

#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

namespace nowplaying {

static const int kCacheMs = 400;          // a bundle younger than this is served again (several dashboards, one assembly)
static const int kUpstreamTimeoutMs = 1500;
static const int kInputCount = 8;         // the desktop InputMap has eight keys

/// Lower-cased engine path with trailing slashes dropped: how InputMap.qml compares what is on air with a slot (norm()).
static QString norm(const QString& p) { QString s = p.toLower(); while (s.endsWith('/')) s.chop(1); return s; }

/// Merges the pieces into the bundle. Pure (no Qt I/O) so test_nowplaying can check it without a server.
QJsonObject assemble(const QString& channelId, const QJsonObject& channel, const QJsonObject& state, const QJsonObject& runlist,
                     const QJsonObject& overlays, const QJsonObject& cgslots, const QJsonObject& stats, const QJsonObject& facebook,
                     const QJsonObject& inputMap, const QStringList& errors)
{
    QJsonObject out;
    out["atMs"] = double(QDateTime::currentMSecsSinceEpoch());
    out["channelId"] = channelId;

    // --- channel ---------------------------------------------------------------------------------------------------------------
    const QJsonObject profile = channel.value("profile").toObject();
    const QJsonObject process = channel.value("process").toObject();
    const QJsonObject executor = channel.value("executor").toObject();
    const QJsonObject extra = profile.value("extra").toObject();
    QJsonObject ch;
    ch["id"] = channelId; ch["name"] = profile.value("name").toString(channelId); ch["callLetters"] = profile.value("callLetters");
    ch["width"] = profile.value("width"); ch["height"] = profile.value("height"); ch["frameRate"] = profile.value("frameRate");
    ch["hidden"] = extra.value("hidden").toBool(false);
    ch["engineConnected"] = process.value("connected").toBool(false);
    ch["engineState"] = state.value("engineState").toString(channel.value("engineState").toString("OFFLINE"));
    ch["armed"] = executor.value("armed").toBool(false);
    ch["lastPushReason"] = executor.value("lastPushReason");
    ch["found"] = !profile.isEmpty();
    out["channel"] = ch;

    // --- transport state: what the engine says it is doing ------------------------------------------------------------------------
    const QString st = state.value("state").toString(ch["engineState"].toString());
    QJsonObject tr;
    for (const char* k : {"state", "itemId", "assetId", "path", "title", "nextItemId", "nextTitle", "nextReady", "position", "duration",
                          "remaining", "playlistIndex", "playlistSize", "frame", "timecode", "healthy", "error", "width", "height", "frameRate"})
        if (state.contains(QLatin1String(k))) tr[QLatin1String(k)] = state.value(QLatin1String(k));
    tr["state"] = st;
    out["state"] = tr;

    // --- on air / next, enriched from the runlist (type, planned times, media id) -----------------------------------------------
    const QJsonObject cur = runlist.value("current").toObject(), nxt = runlist.value("next").toObject();
    auto findItem = [&](const QString& id) -> QJsonObject {
        if (id.isEmpty()) return QJsonObject();
        for (const QJsonObject* hour : {&cur, &nxt})
            for (const QJsonValue& v : hour->value("items").toArray()) { const QJsonObject it = v.toObject(); if (it.value("id").toString() == id) return it; }
        return QJsonObject();
    };
    const QJsonObject onItem = findItem(state.value("itemId").toString());
    QJsonObject onAir;
    onAir["itemId"] = state.value("itemId"); onAir["title"] = state.value("title"); onAir["path"] = state.value("path");
    onAir["position"] = state.value("position"); onAir["duration"] = state.value("duration"); onAir["remaining"] = state.value("remaining");
    onAir["timecode"] = state.value("timecode"); onAir["live"] = state.value("path").toString().startsWith("live://") || onItem.value("type").toString() == "live";
    if (!onItem.isEmpty()) { onAir["type"] = onItem.value("type"); onAir["plannedStartMs"] = onItem.value("plannedStartMs"); onAir["plannedEndMs"] = onItem.value("plannedEndMs"); onAir["mediaId"] = onItem.value("mediaId"); onAir["note"] = onItem.value("note"); }
    out["onAir"] = onAir;
    const QJsonObject nxItem = findItem(state.value("nextItemId").toString());
    QJsonObject next;
    next["itemId"] = state.value("nextItemId"); next["title"] = state.value("nextTitle"); next["ready"] = state.value("nextReady").toBool(false);
    if (!nxItem.isEmpty()) { next["type"] = nxItem.value("type"); next["plannedStartMs"] = nxItem.value("plannedStartMs"); next["duration"] = nxItem.value("duration"); }
    out["next"] = next;
    QJsonObject rl; rl["current"] = cur; rl["next"] = nxt; out["runlist"] = rl;

    // --- inputs: the shared switcher map (ui.inputMap) + the engine's monitor state per key + which slot is on air --------------
    QHash<QString, QJsonObject> mon;
    for (const QJsonValue& v : state.value("monitors").toArray()) { const QJsonObject m = v.toObject(); mon[m.value("key").toString()] = m; }
    const QString onPath = norm(state.value("path").toString());
    const QString enginePathOnAir = norm(state.value("sourcePath").toString());
    QJsonArray slotList; const QJsonArray saved = inputMap.value("slots").toArray();
    int onAirIndex = -1;
    for (int i = 0; i < kInputCount; ++i) {
        const QJsonValue sv = i < saved.size() ? saved.at(i) : QJsonValue();
        QJsonObject s = sv.isObject() ? sv.toObject() : QJsonObject();
        const QString key = QStringLiteral("in%1").arg(i + 1);
        QJsonObject o; o["index"] = i; o["key"] = key; o["mapped"] = !s.isEmpty();
        if (!s.isEmpty()) {
            for (const char* k : {"kind", "title", "path", "mediaId", "enginePath", "sub"}) o[QLatin1String(k)] = s.value(QLatin1String(k));
            const QString ep = norm(s.value("enginePath").toString()), lp = norm(s.value("path").toString());
            const bool on = !onPath.isEmpty() && ((!ep.isEmpty() && (ep == onPath || ep == enginePathOnAir)) || (!lp.isEmpty() && lp == onPath));
            o["onAir"] = on; if (on && onAirIndex < 0) onAirIndex = i;
            o["snapshotUrl"] = QStringLiteral("/preview/%1:%2.jpg").arg(channelId, key);
            o["streamUrl"] = QStringLiteral("/preview/%1:%2.mjpeg").arg(channelId, key);
            if (mon.contains(key)) o["monitor"] = mon.value(key);
        }
        slotList.append(o);
    }
    QJsonObject inputs; inputs["count"] = kInputCount; inputs["slots"] = slotList; inputs["onAirIndex"] = onAirIndex;
    QJsonObject trans = inputMap.value("transition").toObject();
    if (trans.isEmpty()) { trans["type"] = "mix"; trans["durationSec"] = 0.5; }
    inputs["transition"] = trans;
    inputs["mapLoaded"] = !inputMap.isEmpty();
    out["inputs"] = inputs;

    // --- graphics: the overlay manager's annotated layers, the engine's raw layers, the CG slots ----------------------------------
    QJsonObject cg;
    cg["layers"] = overlays.value("layers").toArray();
    cg["engineLayers"] = state.value("layers").toArray();
    cg["item"] = overlays.value("item").toObject();
    cg["slots"] = cgslots.value("slots").toArray();
    cg["emergency"] = overlays.value("emergency").toBool(false);
    out["cg"] = cg;

    // --- meters / outputs / machine --------------------------------------------------------------------------------------------
    out["audio"] = state.value("compositor").toObject().value("audio").toObject();
    out["outputs"] = state.value("outputs");
    QJsonObject sys = stats;
    QJsonObject mine;
    for (const QJsonValue& v : stats.value("channels").toArray()) { const QJsonObject c = v.toObject(); if (c.value("id").toString() == channelId) { mine = c; break; } }
    out["stats"] = sys;
    out["channelStats"] = mine;
    out["previewUrl"] = QStringLiteral("/preview/%1.jpg").arg(channelId);
    out["facebook"] = facebook;
    QJsonArray errs; for (const QString& e : errors) errs.append(e); out["errors"] = errs;
    out["ok"] = true;
    return out;
}

/// Assembles bundles: parallel loopback GETs, coalescing and a short cache.
class Service : public QObject {
public:
    explicit Service(const laneext::Context& ctx, QObject* parent) : QObject(parent), m_ctx(ctx) {}

    void bundle(const QString& channelId, RestApiServer::Reply reply) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        auto cached = m_cache.find(channelId);
        if (cached != m_cache.end() && now - cached->atMs < kCacheMs) { QJsonObject o = cached->bundle; o["cached"] = true; reply(200, o); return; }
        auto waiting = m_pending.find(channelId);
        if (waiting != m_pending.end()) { waiting->append(std::move(reply)); return; }
        m_pending[channelId] = QList<RestApiServer::Reply>{ std::move(reply) };
        assemble(channelId);
    }

    QJsonObject info() const {
        QJsonObject o; o["lane"] = "nowplaying"; o["cacheMs"] = kCacheMs; o["inputs"] = kInputCount;
        o["routes"] = QJsonArray{"GET /api/v1/nowplaying", "GET /api/v1/nowplaying/{id}"};
        o["snapshots"] = "/preview/{channel}:in{N}.jpg (engine input monitors, leased with POST /api/v1/channels/{id}/monitor)";
        o["program"] = "/preview/{channel}.jpg";
        o["upstream"] = QJsonArray{"/channels/{id}", "/channels/{id}/state", "/channels/{id}/runlist", "/overlays/{id}", "/overlays/cgslots", "/system/stats", "/live/facebook"};
        return o;
    }

private:
    struct Parts {
        QHash<QString, QJsonObject> got;
        QStringList errors;
        int left = 0;
    };
    struct Cached { qint64 atMs = 0; QJsonObject bundle; };

    void assemble(const QString& channelId) {
        const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(channelId));
        const QList<QPair<QString, QString>> parts = {
            {"channel", "/api/v1/channels/" + enc}, {"state", "/api/v1/channels/" + enc + "/state"}, {"runlist", "/api/v1/channels/" + enc + "/runlist"},
            {"overlays", "/api/v1/overlays/" + enc}, {"cgslots", "/api/v1/overlays/cgslots"}, {"stats", "/api/v1/system/stats"}, {"facebook", "/api/v1/live/facebook"},
        };
        auto p = std::make_shared<Parts>();
        p->left = parts.size();
        if (m_ctx.restPort <= 0) { p->left = 0; p->errors << "no restPort: upstream routes unreachable"; finish(channelId, p); return; }
        const QString token = m_ctx.restToken ? m_ctx.restToken() : QString();
        for (const auto& part : parts) {
            QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_ctx.restPort).arg(part.second)));
            req.setTransferTimeout(kUpstreamTimeoutMs);
            if (!token.isEmpty()) req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
            QNetworkReply* r = m_nam.get(req);
            const QString name = part.first;
            connect(r, &QNetworkReply::finished, this, [this, r, name, channelId, p]() {
                r->deleteLater();
                const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QJsonDocument d = QJsonDocument::fromJson(r->readAll());
                if (r->error() == QNetworkReply::NoError && status < 400 && d.isObject()) p->got[name] = d.object();
                else p->errors << (name + ": " + (status ? QString::number(status) : r->errorString()));
                if (--p->left == 0) finish(channelId, p);
            });
        }
    }

    void finish(const QString& channelId, const std::shared_ptr<Parts>& p) {
        QJsonObject inputMap;
        if (m_ctx.store) {
            const QString raw = m_ctx.store->setting("ui.inputMap");
            if (!raw.isEmpty()) { const QJsonDocument d = QJsonDocument::fromJson(raw.toUtf8()); if (d.isObject()) inputMap = d.object(); else p->errors << "ui.inputMap: unreadable"; }
        }
        const QJsonObject b = nowplaying::assemble(channelId, p->got.value("channel"), p->got.value("state"), p->got.value("runlist"), p->got.value("overlays"),
                                                   p->got.value("cgslots"), p->got.value("stats"), p->got.value("facebook"), inputMap, p->errors);
        const bool found = b.value("channel").toObject().value("found").toBool();
        if (found) m_cache[channelId] = Cached{ QDateTime::currentMSecsSinceEpoch(), b };
        const QList<RestApiServer::Reply> waiters = m_pending.take(channelId);
        for (const auto& w : waiters) {
            if (!found) { QJsonObject e = b; e["error"] = "no such channel: " + channelId; e["status"] = 404; w(404, e); }
            else w(200, b);
        }
    }

    laneext::Context m_ctx;
    QNetworkAccessManager m_nam;
    QHash<QString, Cached> m_cache;
    QHash<QString, QList<RestApiServer::Reply>> m_pending;
};

/// Installs the routes; the service (parented to ctx.parent), or null without a REST server.
Service* installOn(const laneext::Context& ctx) {
    if (!ctx.rest) { qWarning() << "[nowplaying] not installed: no REST server"; return nullptr; }
    auto* s = new Service(ctx, ctx.parent);
    QPointer<Service> sp(s);
    ctx.rest->registerRoute("GET", "/api/v1/nowplaying", [sp](const QJsonObject&, const QHash<QString, QString>&) { return sp ? sp->info() : QJsonObject(); });
    ctx.rest->registerAsync("GET", "/api/v1/nowplaying/{id}", [sp](const QJsonObject&, const QHash<QString, QString>& q, RestApiServer::Reply reply) {
        const QString id = q.value("id");
        static const QRegularExpression okId("^[A-Za-z0-9_.-]{1,64}$");
        if (!okId.match(id).hasMatch()) { QJsonObject e; e["error"] = "bad channel id"; e["status"] = 400; reply(400, e); return; }
        if (!sp) { QJsonObject e; e["error"] = "nowplaying service gone"; e["status"] = 503; reply(503, e); return; }
        sp->bundle(id, std::move(reply));
    });
    qInfo() << "[nowplaying] routes installed: /api/v1/nowplaying, /api/v1/nowplaying/{id}";
    return s;
}

} // namespace nowplaying

static laneext::Registrar s_nowplayingRoutes("nowplaying", 110, [](const laneext::Context& ctx) { nowplaying::installOn(ctx); });
