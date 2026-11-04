/**
 * Simulated confidence previews — render a peer's playout locally.
 *
 * Premise: every Play Play session has every media file (replicated via
 * SyncService). To preview what Studio B is currently airing we don't fetch
 * a JPEG from B — we fetch B's playout STATE (mediaId/hash + currentTime),
 * find our own copy of the file, seek a hidden <video> there, and draw a
 * frame to a canvas. The visual is identical, network cost is ~200 bytes.
 *
 * The renderer creates ONE pool of hidden <video> + <canvas> elements
 * (capped) so a wall of N peers doesn't spawn N video decoders simultaneously.
 *
 *   const pool = getPreviewPool();
 *   const blob = await pool.snap({ mediaSrc, mediaId, currentTime, width: 320 });
 */

export interface PeerPlayoutState {
    instanceId: string;
    displayName: string;
    hostname?: string;
    mediaId: string | null;
    mediaHash: string | null;
    mediaTitle: string | null;
    /** Source URL as the source instance saw it — usually NOT usable here. Resolve locally. */
    mediaSrc: string | null;
    durationMs: number;
    /** seconds (matches HTMLMediaElement.currentTime) */
    currentTime: number;
    isPlaying: boolean;
    playbackState: 'on_air' | 'filling' | 'paused' | 'gap' | 'idle';
    updatedAt: number;
    serverTime: number;
}

export interface SnapRequest {
    /** Local URL for THIS file — caller resolves via local catalog */
    mediaSrc: string;
    /** Used as a cache key. Different mediaId → reload. */
    mediaId: string;
    /** Seconds into the media */
    currentTime: number;
    /** Output thumbnail width in pixels (height auto from aspect) */
    width?: number;
    /** JPEG quality 0–1 */
    quality?: number;
    /** Hard timeout for canplay/seek */
    timeoutMs?: number;
}

export type SnapResult =
    | { ok: true; blob: Blob; objectUrl: string; width: number; height: number; capturedAt: number }
    | { ok: false; error: string };

class PreviewPool {
    private slots: Array<{ inUse: boolean; video: HTMLVideoElement; canvas: HTMLCanvasElement; lastSrc: string | null }>;
    private container: HTMLDivElement;
    private maxSlots: number;
    private waitQueue: Array<() => void> = [];

    constructor(maxSlots = 4) {
        this.maxSlots = maxSlots;

        // Hidden offscreen container — kept in DOM so video elements actually decode
        this.container = document.createElement('div');
        Object.assign(this.container.style, {
            position: 'fixed',
            left: '-99999px',
            top: '-99999px',
            width: '1px',
            height: '1px',
            overflow: 'hidden',
            pointerEvents: 'none',
            opacity: '0',
        });
        this.container.setAttribute('aria-hidden', 'true');
        document.body.appendChild(this.container);

        this.slots = [];
        for (let i = 0; i < maxSlots; i++) {
            const video = document.createElement('video');
            video.muted = true;
            video.playsInline = true;
            video.preload = 'auto';
            video.crossOrigin = 'anonymous';      // best-effort; ignored for non-CORS
            video.style.width = '320px';
            this.container.appendChild(video);

            const canvas = document.createElement('canvas');
            this.slots.push({ inUse: false, video, canvas, lastSrc: null });
        }
    }

    private async acquire(): Promise<typeof this.slots[number]> {
        const free = this.slots.find(s => !s.inUse);
        if (free) {
            free.inUse = true;
            return free;
        }
        // Wait for one to free up
        await new Promise<void>(resolve => this.waitQueue.push(resolve));
        return this.acquire();
    }

    private release(slot: typeof this.slots[number]) {
        slot.inUse = false;
        const next = this.waitQueue.shift();
        if (next) next();
    }

    async snap(req: SnapRequest): Promise<SnapResult> {
        if (!req.mediaSrc) return { ok: false, error: 'No mediaSrc' };
        const slot = await this.acquire();

        try {
            const video = slot.video;
            const wantW = req.width ?? 320;
            const quality = req.quality ?? 0.8;
            const timeoutMs = req.timeoutMs ?? 4000;

            // Only reload if the source actually changed; saves a decode on
            // periodic refreshes of the same clip.
            const needsLoad = slot.lastSrc !== req.mediaSrc;
            if (needsLoad) {
                video.src = req.mediaSrc;
                slot.lastSrc = req.mediaSrc;
                await waitFor(video, ['loadedmetadata'], timeoutMs);
            }

            // Clamp seek to media bounds
            const clamped = Math.max(0, Math.min(req.currentTime, Math.max(0, (video.duration || 0) - 0.1)));
            // Only seek if the delta is meaningful — avoids re-decoding the same frame
            if (Math.abs(video.currentTime - clamped) > 0.1) {
                video.currentTime = clamped;
                await waitFor(video, ['seeked'], timeoutMs);
            }

            const vw = video.videoWidth || 1280;
            const vh = video.videoHeight || 720;
            const w = wantW;
            const h = Math.round((vh / vw) * w);
            slot.canvas.width = w;
            slot.canvas.height = h;
            const ctx = slot.canvas.getContext('2d', { alpha: false });
            if (!ctx) return { ok: false, error: 'No canvas 2d context' };
            ctx.drawImage(video, 0, 0, w, h);

            const blob = await new Promise<Blob | null>((resolve) =>
                slot.canvas.toBlob(resolve, 'image/jpeg', quality)
            );
            if (!blob) return { ok: false, error: 'toBlob returned null' };

            return {
                ok: true,
                blob,
                objectUrl: URL.createObjectURL(blob),
                width: w,
                height: h,
                capturedAt: Date.now(),
            };
        } catch (e) {
            return { ok: false, error: (e as Error).message };
        } finally {
            this.release(slot);
        }
    }

    destroy() {
        for (const s of this.slots) {
            try { s.video.removeAttribute('src'); s.video.load(); } catch { /* */ }
        }
        try { this.container.remove(); } catch { /* */ }
    }
}

function waitFor(el: HTMLVideoElement, events: string[], timeoutMs: number): Promise<void> {
    return new Promise((resolve, reject) => {
        let done = false;
        const onErr = () => finish(true);
        const onOk = () => finish(false);
        const finish = (err: boolean) => {
            if (done) return;
            done = true;
            for (const evt of events) el.removeEventListener(evt, onOk);
            el.removeEventListener('error', onErr);
            clearTimeout(timer);
            err ? reject(new Error(`Video error during ${events.join('/')}`)) : resolve();
        };
        const timer = window.setTimeout(() => {
            if (done) return;
            done = true;
            for (const evt of events) el.removeEventListener(evt, onOk);
            el.removeEventListener('error', onErr);
            reject(new Error(`Timeout waiting for ${events.join('/')}`));
        }, timeoutMs);
        for (const evt of events) el.addEventListener(evt, onOk, { once: true });
        el.addEventListener('error', onErr, { once: true });
    });
}

let pool: PreviewPool | null = null;

/** Lazily-created shared pool — reused across all preview tiles in the app. */
export function getPreviewPool(maxSlots = 4): PreviewPool {
    if (!pool) pool = new PreviewPool(maxSlots);
    return pool;
}

/**
 * Convenience: given a peer's playout state and a local URL resolver, return
 * a Blob URL for the rendered preview. Caller is responsible for revoking
 * the previous object URL when replacing.
 */
export async function snapFromPeerState(
    peer: PeerPlayoutState,
    resolveLocalUrl: (mediaId: string, hash: string | null) => string | null,
    options: { width?: number; quality?: number; timeoutMs?: number } = {},
): Promise<SnapResult> {
    if (!peer.mediaId) return { ok: false, error: 'Peer has no current media' };
    const localUrl = resolveLocalUrl(peer.mediaId, peer.mediaHash);
    if (!localUrl) return { ok: false, error: 'No local copy of that media' };

    return getPreviewPool().snap({
        mediaId: peer.mediaId,
        mediaSrc: localUrl,
        currentTime: peer.currentTime,
        width: options.width,
        quality: options.quality,
        timeoutMs: options.timeoutMs,
    });
}
