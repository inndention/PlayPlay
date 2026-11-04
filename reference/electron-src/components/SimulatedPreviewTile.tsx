/**
 * SimulatedPreviewTile — one cell of a confidence preview wall.
 *
 *   - Polls the peer's /api/v2/playout/current at `refreshMs` cadence
 *   - Resolves the file locally (callback `resolveLocalUrl`)
 *   - Snaps a frame via the shared preview pool
 *   - Shows live mediaTitle, currentTime, isPlaying badge, peer name
 *
 * Drop several of these into a CSS grid to build a multi-station wall.
 */

import { useEffect, useRef, useState } from 'react';
import { snapFromPeerState, type PeerPlayoutState } from '../lib/simulatedPreview';
import { RemoteApiClient } from '../lib/remoteApiClient';

export interface SimulatedPreviewTileProps {
    /** REST endpoint of the peer (e.g. http://192.168.1.42:3000) */
    baseUrl: string;
    /** Bearer token issued by the peer */
    token: string;
    /** Display label override (otherwise uses peer's displayName) */
    label?: string;
    /** Local URL resolver — given mediaId + hash, return a URL playable here */
    resolveLocalUrl: (mediaId: string, hash: string | null) => string | null;
    /** ms between state polls + frame snaps (default 2000) */
    refreshMs?: number;
    /** Output thumbnail width (default 320) */
    width?: number;
    /** Tile click handler (e.g. navigate to detail) */
    onClick?: (peer: PeerPlayoutState) => void;
}

export function SimulatedPreviewTile({
    baseUrl, token, label, resolveLocalUrl,
    refreshMs = 2000, width = 320, onClick,
}: SimulatedPreviewTileProps) {
    const [peer, setPeer] = useState<PeerPlayoutState | null>(null);
    const [imgUrl, setImgUrl] = useState<string | null>(null);
    const [error, setError] = useState<string | null>(null);
    const [stale, setStale] = useState(false);
    const lastImgUrlRef = useRef<string | null>(null);
    const clientRef = useRef<RemoteApiClient | null>(null);

    if (!clientRef.current) {
        clientRef.current = new RemoteApiClient({ baseUrl, token, timeoutMs: 4000 });
    } else if (clientRef.current && (clientRef.current as any)['baseUrl'] !== baseUrl) {
        clientRef.current.setBaseUrl(baseUrl);
        clientRef.current.setToken(token);
    }

    useEffect(() => {
        let alive = true;
        let timer: number | null = null;

        const tick = async () => {
            if (!alive) return;
            const client = clientRef.current!;
            const stateRes = await (client as any).call?.('GET', '/api/v2/playout/current').catch(() => null) ?? null;
            // The typed client doesn't expose a getPlayoutCurrent yet — fall back to fetch
            let next: PeerPlayoutState | null = null;
            try {
                const r = await fetch(baseUrl + '/api/v2/playout/current', {
                    headers: { Authorization: `Bearer ${token}` },
                });
                const j = await r.json();
                if (j?.ok) next = j.data as PeerPlayoutState;
                else throw new Error(j?.error?.message || 'state fetch failed');
            } catch (e) {
                if (alive) {
                    setError((e as Error).message);
                    setStale(true);
                }
            }
            void stateRes; // not used directly

            if (alive && next) {
                setPeer(next);
                setError(null);
                // Mark stale if the peer's update is more than 5s old
                setStale(Date.now() - next.updatedAt > 5000);

                if (next.mediaId) {
                    const snap = await snapFromPeerState(next, resolveLocalUrl, { width });
                    if (!alive) return;
                    if (snap.ok) {
                        // Revoke previous blob URL
                        if (lastImgUrlRef.current) {
                            try { URL.revokeObjectURL(lastImgUrlRef.current); } catch { /* */ }
                        }
                        lastImgUrlRef.current = snap.objectUrl;
                        setImgUrl(snap.objectUrl);
                        setError(null);
                    } else {
                        setError(snap.error);
                    }
                }
            }

            if (alive) timer = window.setTimeout(tick, refreshMs);
        };

        tick();
        return () => {
            alive = false;
            if (timer) window.clearTimeout(timer);
            if (lastImgUrlRef.current) {
                try { URL.revokeObjectURL(lastImgUrlRef.current); } catch { /* */ }
                lastImgUrlRef.current = null;
            }
        };
    }, [baseUrl, token, refreshMs, width, resolveLocalUrl]);

    return (
        <div
            onClick={() => peer && onClick?.(peer)}
            style={{
                background: '#0a0a0a',
                border: '1px solid #1f2937',
                borderRadius: 8,
                overflow: 'hidden',
                cursor: onClick ? 'pointer' : 'default',
                position: 'relative',
                width,
            }}
        >
            <div style={{ width, aspectRatio: '16 / 9', background: '#000', position: 'relative' }}>
                {imgUrl ? (
                    <img src={imgUrl} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover', display: 'block' }} />
                ) : (
                    <div style={{ display: 'grid', placeItems: 'center', height: '100%', color: '#666', fontSize: 12 }}>
                        {error ? error : 'Loading…'}
                    </div>
                )}
                {/* Tally */}
                {peer?.isPlaying && (
                    <span style={{
                        position: 'absolute', top: 6, left: 6,
                        background: '#ef4444', color: '#fff', fontSize: 10, fontWeight: 700,
                        padding: '2px 6px', borderRadius: 4, letterSpacing: 0.5,
                    }}>● ON AIR</span>
                )}
                {stale && (
                    <span style={{
                        position: 'absolute', top: 6, right: 6,
                        background: '#f59e0b', color: '#000', fontSize: 10, fontWeight: 700,
                        padding: '2px 6px', borderRadius: 4,
                    }}>STALE</span>
                )}
            </div>
            <div style={{ padding: '6px 8px', fontSize: 12, color: '#e5e7eb' }}>
                <div style={{ fontWeight: 600, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                    {label || peer?.displayName || baseUrl}
                </div>
                <div style={{ color: '#9ca3af', fontSize: 11, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                    {peer?.mediaTitle || (peer ? '— no media —' : '')}
                    {peer && ` · ${formatTime(peer.currentTime)}`}
                </div>
            </div>
        </div>
    );
}

function formatTime(t: number): string {
    if (!Number.isFinite(t) || t < 0) return '--:--';
    const h = Math.floor(t / 3600);
    const m = Math.floor((t % 3600) / 60);
    const s = Math.floor(t % 60);
    if (h > 0) return `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`;
    return `${m}:${String(s).padStart(2, '0')}`;
}
