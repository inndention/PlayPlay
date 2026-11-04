#ifndef PLAYPLAY_GPUCOMPOSITOR_H
#define PLAYPLAY_GPUCOMPOSITOR_H

#include <QRectF>
#include <QRect>
#include <QString>
#include <QJsonObject>
#include <memory>
#include <vector>
#include "../mediaframe.h"

/**
 * GpuCompositor — Direct3D 11 implementation of the compositor's draw list.
 *
 * Each DrawOp blits a BGRA frame into a destination rectangle with opacity and an optional clip
 * rectangle (wipes), premultiplied source-over, onto a BGRA render target the size of the raster.
 *
 * Performance model (the OBS one):
 *   - Uploads are cached. A source frame that is still the same picture (same pixel buffer, which the
 *     cache keeps referenced so a pool cannot recycle it) is not uploaded again: a lower third at rest
 *     or a bug costs nothing after its first frame. Only pictures that really changed cross the bus.
 *   - Readback is asynchronous. submit() draws and queues the copy into a staging texture, then returns
 *     a ticket at once; fetch(ticket) maps it later (the compositor fetches one tick later, by which time
 *     the GPU has long finished, so the clock thread never waits on the GPU).
 *   - Several outputs per tick (one per mix) share the cached textures; each submit is one draw pass.
 *
 * create() returns null when no D3D11 device is available; the Compositor then stays on the QPainter
 * path. Both paths render the same draw list, so the tests compare them pixel-for-pixel within tolerance.
 */
class GpuCompositor {
public:
    struct DrawOp {
        FramePtr frame;          // BGRA source
        QRectF dst;              // destination in raster pixels
        QRectF src;              // source rect in frame pixels (empty = whole frame)
        double opacity = 1.0;
        QRectF clip;             // empty = none
        bool premultiplied = false;  // the frame is premultiplied BGRA (PE-08: LayerSource::deliversPremultiplied()); false = straight
        QRect upload;            // only this part of the frame is uploaded (empty = whole); the draw samples inside it
    };

    virtual ~GpuCompositor() = default;
    static std::unique_ptr<GpuCompositor> create(int width, int height, QString* error = nullptr);

    /// Draw the list and queue its readback. Returns a ticket (>= 0), or -1 on device loss / no free staging texture.
    virtual int submit(const std::vector<DrawOp>& ops) = 0;
    /// Copy a ticket's picture into `out` (BGRA, raster size) and free the ticket. Blocks only if the GPU is still on it.
    virtual bool fetch(int ticket, const FramePtr& out) = 0;
    /// Drop a ticket without reading it.
    virtual void release(int ticket) = 0;
    /// Synchronous convenience: submit + fetch.
    bool render(const std::vector<DrawOp>& ops, const FramePtr& out) { const int t = submit(ops); return t >= 0 && fetch(t, out); }

    virtual QString adapterName() const = 0;
    /// CPU time of the last submit (upload + draw calls) and of the last fetch (map + copy), ms.
    virtual double lastRenderMs() const = 0;
    virtual double lastFetchMs() const = 0;
    /// {uploads, cacheHits, uploadedMB, submits, fetchWaitsMs ...} since start.
    virtual QJsonObject stats() const = 0;

    /// stats() as plain numbers (PE-05: Compositor::status() copies these under its lock and builds the JSON after it).
    /// The averages are -1 until there is something to average (stats() then leaves the key out).
    struct Counters {
        double submits = 0, uploads = 0, cacheHits = 0, uploadedMB = 0, fetchWaits = 0, fetchWaitMs = 0;
        double cachedTextures = 0, stagingTextures = 0, gpuDoneAtFetch = 0, gpuBusyAtFetch = 0, copyMs = 0;
        double submitMsAvg = -1, fetchMsAvg = -1, gpuDrawMsAvg = -1, gpuCopyMsAvg = -1;
        double lastRenderMs = 0, lastFetchMs = 0;
    };
    /// (Declared after stats() so the vtable of older translation units keeps its layout.)
    virtual Counters counters() const { Counters c; c.lastRenderMs = lastRenderMs(); c.lastFetchMs = lastFetchMs(); return c; }
    /// The stats() object for `c` (same keys).
    static QJsonObject countersJson(const Counters& c) {
        QJsonObject o;
        o["submits"] = c.submits; o["uploads"] = c.uploads; o["cacheHits"] = c.cacheHits;
        o["uploadedMB"] = c.uploadedMB; o["fetchWaits"] = c.fetchWaits; o["fetchWaitMs"] = c.fetchWaitMs;
        o["cachedTextures"] = int(c.cachedTextures); o["stagingTextures"] = int(c.stagingTextures);
        o["gpuDoneAtFetch"] = c.gpuDoneAtFetch; o["gpuBusyAtFetch"] = c.gpuBusyAtFetch; o["copyMs"] = c.copyMs;
        if (c.submitMsAvg >= 0) o["submitMsAvg"] = c.submitMsAvg;
        if (c.fetchMsAvg >= 0) o["fetchMsAvg"] = c.fetchMsAvg;
        if (c.gpuDrawMsAvg >= 0) { o["gpuDrawMsAvg"] = c.gpuDrawMsAvg; o["gpuCopyMsAvg"] = c.gpuCopyMsAvg; }
        return o;
    }
};

#endif // PLAYPLAY_GPUCOMPOSITOR_H
