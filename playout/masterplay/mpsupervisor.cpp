// MasterPlay Supervisor wiring (the lane design notes §10; slice MP-12): the laneext registrar that mounts
// mp::svc::MpService's /api/v1/masterplay routes on the Supervisor's REST server. This translation unit is NOT part of
// the PlayPlayMasterplay static library (the linker would drop its unreferenced static registrar): cmake/lane-masterplay.cmake
// adds it to PlayPlaySupervisor with target_sources when PLAYPLAY_BUILD_MASTERPLAY is ON, so supervisorcore*.cpp stays
// untouched and a tree with the option OFF (the live build) keeps today's routes exactly.
//
// Order 100: after the core registerRoutes and the other lane modules (mux-async 10, transcribe-probe 20, pubrails 40),
// before the station listener copies the route table. The install only builds the route table and starts the service's
// idle thread pool; nothing here blocks the event loop.
#include "mpservice.h"
#include "mplane.h"
#include "../supervisor/laneext.h"
#include "../supervisor/restapiserver.h"
#include "../supervisor/ingestmanager.h"
#include <QDebug>
#include <QPointer>

namespace mp { namespace svc {

/// The MpService::Host for a Supervisor: a subset of the laneext context plus the ingest enqueue. Exposed (not static) so
/// test_mpsupervisor can check the mapping without a SupervisorCore.
Host hostFromContext(const laneext::Context& ctx) {
    Host h;
    h.store = ctx.store;
    h.dataDir = ctx.dataDir;
    h.restPort = ctx.restPort;
    h.restToken = ctx.restToken;
    h.broadcast = ctx.broadcast;
    h.alert = ctx.alert;
    if (ctx.ingest) {
        QPointer<IngestManager> ing(ctx.ingest);
        h.enqueueIngest = [ing](const QString& path, const QString& type, const QJsonObject& meta) {
            return ing ? ing->enqueue(path, type, QStringLiteral("masterplay"), meta) : QJsonObject();
        };
    }
    // afterSave: the service default (POST /api/v1/spots/markers/enforce on this Supervisor's restPort).
    // meshConfig: the service default (the store's mesh.anfsUrl / mesh.anfsToken / mesh.anfsCookie), read per call.
    return h;
}

/// Creates the service (parented to the SupervisorCore) and installs its routes; the service, or null without a store.
MpService* installOn(const laneext::Context& ctx) {
    if (!ctx.rest || !ctx.store) { qWarning() << "[masterplay] not installed: no REST server or schedule store"; return nullptr; }
    auto* s = new MpService(hostFromContext(ctx), ctx.parent);
    s->install<RestApiServer>(ctx.rest);
    qInfo() << "[masterplay] routes installed:" << s->routeKeys().size() << "under" << MpService::kPrefix << "api" << kApiVersion;
    return s;
}

} } // namespace mp::svc

static laneext::Registrar s_masterplayRoutes("masterplay", 100, [](const laneext::Context& ctx) { mp::svc::installOn(ctx); });
