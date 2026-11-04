/**
 * useSupervisor — React hook for live telemetry from the C++ Supervisor.
 *
 * Subscribes to the WebSocket telemetry stream and exposes:
 *   - connection status
 *   - per-channel state (heartbeat, frame number, healthy)
 *   - global supervisor health
 *   - imperative client for REST calls (transport, playlist, etc.)
 */

import { useEffect, useRef, useState, useCallback } from 'react';
import {
    getSupervisorClient,
    SupervisorClient,
    SupervisorClientOptions,
    TelemetryEvent,
    ChannelInfo,
    SupervisorHealth,
} from '../lib/supervisorClient';

export interface ChannelLiveState extends ChannelInfo {
    lastHeartbeatMs: number;
    framesPerSec?: number;
}

export interface UseSupervisorState {
    connected: boolean;
    clientId: string | null;
    channels: Record<string, ChannelLiveState>;
    health: SupervisorHealth | null;
    lastEvent: TelemetryEvent | null;
    client: SupervisorClient;
}

export function useSupervisor(opts?: SupervisorClientOptions): UseSupervisorState {
    const clientRef = useRef<SupervisorClient | null>(null);
    if (!clientRef.current) clientRef.current = getSupervisorClient(opts);

    const [connected, setConnected] = useState(false);
    const [clientId, setClientId] = useState<string | null>(null);
    const [channels, setChannels] = useState<Record<string, ChannelLiveState>>({});
    const [health, setHealth] = useState<SupervisorHealth | null>(null);
    const [lastEvent, setLastEvent] = useState<TelemetryEvent | null>(null);

    const lastFrameByChannelRef = useRef<Record<string, { frame: number; t: number }>>({});

    const handleEvent = useCallback((ev: TelemetryEvent) => {
        setLastEvent(ev);

        switch (ev.type) {
            case 'connected':
                setConnected(true);
                if ('clientId' in ev) setClientId(String(ev.clientId));
                break;

            case 'channelSpawned': {
                const channelId = String((ev as any).channelId);
                setChannels((prev) => ({
                    ...prev,
                    [channelId]: {
                        ...(prev[channelId] ?? { id: channelId, name: channelId }),
                        id: channelId,
                        state: 'spawned',
                        lastHeartbeatMs: Date.now(),
                    },
                }));
                break;
            }

            case 'channelDied': {
                const channelId = String((ev as any).channelId);
                setChannels((prev) => ({
                    ...prev,
                    [channelId]: {
                        ...(prev[channelId] ?? { id: channelId, name: channelId, lastHeartbeatMs: 0 }),
                        id: channelId,
                        state: 'dead',
                        healthy: false,
                    },
                }));
                break;
            }

            case 'heartbeat': {
                const channelId = String((ev as any).channelId);
                const frameNumber = Number((ev as any).frameNumber);
                const healthy = Boolean((ev as any).healthy);
                const now = Date.now();

                const last = lastFrameByChannelRef.current[channelId];
                let fps: number | undefined;
                if (last && now > last.t) {
                    const dt = (now - last.t) / 1000;
                    const df = frameNumber - last.frame;
                    if (dt > 0 && df >= 0) fps = df / dt;
                }
                lastFrameByChannelRef.current[channelId] = { frame: frameNumber, t: now };

                setChannels((prev) => ({
                    ...prev,
                    [channelId]: {
                        ...(prev[channelId] ?? { id: channelId, name: channelId }),
                        id: channelId,
                        frameNumber,
                        healthy,
                        lastHeartbeatMs: now,
                        framesPerSec: fps,
                    },
                }));
                break;
            }

            case 'stateChange': {
                const channelId = String((ev as any).channelId);
                const state = String((ev as any).state ?? (ev as any).newState ?? '');
                if (state) {
                    setChannels((prev) => ({
                        ...prev,
                        [channelId]: {
                            ...(prev[channelId] ?? { id: channelId, name: channelId, lastHeartbeatMs: 0 }),
                            id: channelId,
                            state,
                        },
                    }));
                }
                break;
            }

            default:
                break;
        }
    }, []);

    // Connect + subscribe
    useEffect(() => {
        const client = clientRef.current!;
        const off = client.onTelemetry(handleEvent);
        client.connect();

        // Poll connection status (cheap)
        const t = window.setInterval(() => {
            setConnected(client.isConnected());
        }, 1000);

        return () => {
            off();
            window.clearInterval(t);
            // Note: do not disconnect singleton — other consumers may still need it
        };
    }, [handleEvent]);

    // Initial REST hydration: list channels + get health
    useEffect(() => {
        const client = clientRef.current!;
        let cancelled = false;

        (async () => {
            try {
                const list = await client.listChannels();
                if (cancelled) return;
                setChannels((prev) => {
                    const next = { ...prev };
                    for (const ch of list.channels ?? []) {
                        next[ch.id] = {
                            ...(next[ch.id] ?? { lastHeartbeatMs: 0 }),
                            ...ch,
                            lastHeartbeatMs: next[ch.id]?.lastHeartbeatMs ?? 0,
                        };
                    }
                    return next;
                });
            } catch (err) {
                console.warn('[useSupervisor] listChannels failed', err);
            }

            try {
                const h = await client.getHealth();
                if (!cancelled) setHealth(h);
            } catch (err) {
                console.warn('[useSupervisor] getHealth failed', err);
            }
        })();

        // Refresh health every 5s
        const t = window.setInterval(async () => {
            try {
                const h = await client.getHealth();
                if (!cancelled) setHealth(h);
            } catch { /* ignore */ }
        }, 5000);

        return () => {
            cancelled = true;
            window.clearInterval(t);
        };
    }, []);

    return {
        connected,
        clientId,
        channels,
        health,
        lastEvent,
        client: clientRef.current!,
    };
}
